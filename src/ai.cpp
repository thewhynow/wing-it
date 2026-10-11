#include "ai.hpp"
#include "models.hpp"
#include "settings.hpp"
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>

using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// leaked on purpose: a detached worker may still be unwinding when main exits
struct Shared {
    std::mutex mu;
    AiStatus st;
    Clock::time_point start;
    bool finished = true;
    std::atomic<bool> running{false}, cancel{false};
    AiTestStatus test;
    std::atomic<bool> testRunning{false};
};
Shared& S = *new Shared;

const char* kSystemPrompt =
    "You are a 3D modelling assistant for an aerodynamics wind-tunnel simulator. The user sends a photo of an "
    "object. Reply with a simplified, low-poly 3D model of the main object as a Wavefront .obj file.\n"
    "Rules:\n"
    "- Output ONLY geometry: lines starting with 'v x y z' (vertices) and 'f a b c' (triangles). No normals, "
    "texture coordinates, materials, groups or colours.\n"
    "- Triangles only, 1-based indices, counter-clockwise winding seen from outside. Every index must refer to "
    "a vertex you already defined.\n"
    "- The mesh must be one closed, watertight solid (no holes, no loose parts, no flat zero-thickness "
    "sheets). Give thin parts real thickness.\n"
    "- Capture the overall silhouette and proportions faithfully; leave out small details, backgrounds, "
    "tables and hands. Use between 80 and 600 vertices.\n"
    "- Axes: +Y is up, the object's front/nose points toward -X, +Z is its side. Centre it at the origin and "
    "scale it so its longest side is about 2 units.\n"
    "- Wrap the entire answer in a single ```obj code fence and write nothing outside it.";

std::string base64(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += T[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += T[v >> 18]; out += T[(v >> 12) & 63]; out += T[(v >> 6) & 63]; out += '=';
    }
    return out;
}

void update(const std::function<void(AiStatus&)>& f) {
    std::lock_guard<std::mutex> l(S.mu);
    f(S.st);
}

// ---- one HTTPS request via libcurl; lines are delivered as they arrive (server-sent events) ----
struct Http {
    long code = 0;
    std::string netErr;           // curl-level failure, already phrased for the user
    bool cancelled = false;
    std::string raw;              // first bytes of the body, for error messages
};

struct CbCtx {
    std::string pending;
    std::string* raw;
    std::function<void(const std::string&)> onLine;
};

size_t onWrite(char* p, size_t sz, size_t n, void* ud) {
    auto* c = (CbCtx*)ud;
    size_t len = sz * n;
    if (c->raw->size() < 16384) c->raw->append(p, std::min(len, 16384 - c->raw->size()));
    c->pending.append(p, len);
    size_t pos;
    while ((pos = c->pending.find('\n')) != std::string::npos) {
        std::string line = c->pending.substr(0, pos);
        c->pending.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (c->onLine) c->onLine(line);
    }
    return len;
}

int onProgress(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) { return S.cancel ? 1 : 0; }

Http request(const std::string& body, const std::string& key, std::function<void(const std::string&)> onLine,
             long timeoutSec) {
    Http h;
    CURL* c = curl_easy_init();
    if (!c) { h.netErr = "couldn't start the network library"; return h; }
    CbCtx ctx{"", &h.raw, std::move(onLine)};
    curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Content-Type: application/json");
    hdr = curl_slist_append(hdr, ("Authorization: Bearer " + key).c_str());
    hdr = curl_slist_append(hdr, "Accept: text/event-stream, application/json");
    std::string url = std::string(kAiBaseUrl) + "/chat/completions";
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK && !ctx.pending.empty() && ctx.onLine) ctx.onLine(ctx.pending); // last line, no newline
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &h.code);
    curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    switch (rc) {
    case CURLE_OK: break;
    case CURLE_ABORTED_BY_CALLBACK: h.cancelled = true; break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT: h.netErr = "Can't reach ai.hackclub.com - check your internet connection."; break;
    case CURLE_OPERATION_TIMEDOUT: h.netErr = "The AI took too long to answer and timed out. Try again."; break;
    default: h.netErr = std::string("Network error: ") + curl_easy_strerror(rc); break;
    }
    return h;
}

// turn a non-200 reply into advice the user can act on
std::string httpError(const Http& h) {
    if (!h.netErr.empty()) return h.netErr;
    std::string msg;
    auto j = json::parse(h.raw, nullptr, false);
    if (j.is_object() && j.contains("error")) {
        auto& e = j["error"];
        if (e.is_object() && e.contains("message") && e["message"].is_string()) msg = e["message"].get<std::string>();
        else if (e.is_string()) msg = e.get<std::string>();
    }
    switch (h.code) {
    case 401: case 403: return "The API key was rejected. Check it in Settings.";
    case 402: return "The AI account is out of credit." + (msg.empty() ? "" : " (" + msg + ")");
    case 404: return "That model isn't available" + (msg.empty() ? "." : ": " + msg) + " Pick another in Settings.";
    case 429: return "Rate limited by the AI service - wait a moment and try again.";
    }
    if (h.code >= 500) return "The AI service had a problem (HTTP " + std::to_string(h.code) + ")." + (msg.empty() ? "" : " " + msg);
    return "The AI request failed (HTTP " + std::to_string(h.code) + ")." + (msg.empty() ? "" : " " + msg);
}

// ---- mesh text handling ----
// Keep only the 'v' and 'f' lines; fences, chatter and anything exotic are dropped. A last line cut off by
// the token limit is dropped too (it would be a half-written face).
std::string extractObj(const std::string& text, bool truncated) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        bool complete = nl != std::string::npos;
        std::string line = text.substr(pos, complete ? nl - pos : std::string::npos);
        pos = complete ? nl + 1 : text.size();
        if (!complete && truncated) break;
        size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        line.erase(0, a);
        while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
        if ((line.rfind("v ", 0) == 0 || line.rfind("f ", 0) == 0)) out += line + "\n";
    }
    return out;
}

std::string slug(const std::string& hint) {
    std::string s;
    for (unsigned char c : hint) {
        if (isalnum(c)) s += (char)tolower(c);
        else if (!s.empty() && s.back() != '-') s += '-';
        if (s.size() >= 24) break;
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    return "ai-" + (s.empty() ? std::string("model") : s) + ".obj";
}

void finish(AiPhase p, const std::string& err = "", const std::string& id = "") {
    {
        std::lock_guard<std::mutex> l(S.mu);
        S.st.phase = p;
        S.st.error = err;
        S.st.modelId = id;
        S.st.elapsed = std::chrono::duration<double>(Clock::now() - S.start).count();
        S.finished = true;
    }
    S.running = false;
}

// one streamed completion; returns the mesh text. On failure sets err and returns "".
bool generate(const json& messages, const std::string& key, std::string& content, bool& truncated, std::string& err) {
    json req = {{"model", settingsModel()}, {"stream", true}, {"max_tokens", 16000}, {"messages", messages}};
    content.clear();
    truncated = false;
    std::string streamErr, curLine;
    int verts = 0, faces = 0;
    auto onLine = [&](const std::string& line) {
        if (line.rfind("data:", 0) != 0) return;
        std::string payload = line.substr(5);
        size_t a = payload.find_first_not_of(' ');
        payload = a == std::string::npos ? "" : payload.substr(a);
        if (payload.empty() || payload == "[DONE]") return;
        json j = json::parse(payload, nullptr, false);
        if (!j.is_object()) return;
        if (j.contains("error")) {
            auto& e = j["error"];
            streamErr = e.is_object() && e.contains("message") && e["message"].is_string() ? e["message"].get<std::string>()
                                                                                            : "the AI reported an error";
            return;
        }
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        auto& ch = j["choices"][0];
        if (ch.contains("finish_reason") && ch["finish_reason"].is_string() && ch["finish_reason"] == "length") truncated = true;
        if (!ch.contains("delta") || !ch["delta"].is_object()) return;
        auto& d = ch["delta"];
        if (d.contains("content") && d["content"].is_string()) {
            std::string piece = d["content"].get<std::string>();
            content += piece;
            for (char chr : piece) {
                if (chr == '\n') {
                    if (curLine.rfind("v ", 0) == 0) ++verts;
                    else if (curLine.rfind("f ", 0) == 0) ++faces;
                    curLine.clear();
                } else curLine += chr;
            }
            update([&](AiStatus& s) { s.phase = AiPhase::Writing, s.vertices = verts, s.faces = faces; });
        } else if (d.contains("reasoning") || d.contains("reasoning_content")) {
            update([&](AiStatus& s) { if (s.phase == AiPhase::Sending) s.phase = AiPhase::Thinking; });
        }
    };
    Http h = request(req.dump(), key, onLine, 300);
    if (h.cancelled) { err = ""; return false; }
    if (!h.netErr.empty() || h.code != 200) { err = httpError(h); return false; }
    if (!streamErr.empty()) { err = "The AI stopped with an error: " + streamErr; return false; }
    if (content.empty()) { err = "The AI sent back nothing. Try again."; return false; }
    return true;
}

void worker(std::string jpeg, std::string hint, std::string key) {
    std::string prompt = "Make a low-poly 3D model of the main object in this photo.";
    if (!hint.empty()) prompt += " It is: " + hint + ".";
    json messages = json::array({
        {{"role", "system"}, {"content", kSystemPrompt}},
        {{"role", "user"}, {"content", json::array({
            {{"type", "text"}, {"text", prompt}},
            {{"type", "image_url"}, {"image_url", {{"url", "data:image/jpeg;base64," + base64(jpeg)}}}},
        })}},
    });
    jpeg.clear();

    std::string lastWhy;
    for (int attempt = 1; attempt <= 2; ++attempt) {
        update([&](AiStatus& s) { s.attempt = attempt, s.phase = AiPhase::Sending, s.vertices = s.faces = 0; });
        std::string content, err;
        bool truncated = false;
        if (!generate(messages, key, content, truncated, err)) {
            if (S.cancel) return finish(AiPhase::Cancelled);
            return finish(AiPhase::Failed, err);
        }
        if (S.cancel) return finish(AiPhase::Cancelled);

        update([](AiStatus& s) { s.phase = AiPhase::Importing; });
        std::string obj = extractObj(content, truncated);
        std::string why, id;
        if (obj.find("\nf ") == std::string::npos && obj.rfind("f ", 0) != 0)
            why = truncated ? "the reply was cut off before any faces were written" : "the reply contained no faces";
        else
            id = modelsImportBytes(slug(hint), obj.data(), obj.size(), why);
        if (!id.empty()) return finish(AiPhase::Done, "", id);

        lastWhy = why + (truncated ? " (the reply hit the length limit)" : "");
        if (attempt == 2) break;
        messages.push_back({{"role", "assistant"}, {"content", content}});
        messages.push_back({{"role", "user"}, {"content",
            "That mesh was rejected: " + lastWhy + ". Output a corrected, simpler OBJ (fewer vertices, valid "
            "1-based indices, closed solid) in a single ```obj fence, nothing else."}});
    }
    finish(AiPhase::Failed, "The AI's model couldn't be loaded: " + lastWhy + ". Try again or retake the photo.");
}

} // namespace

bool aiBusy() {
    return S.running;
}

bool aiStart(const std::string& jpeg, const std::string& hint, std::string& err) {
    std::string key = settingsApiKey();
    if (key.empty()) { err = "No API key set - add one in Settings."; return false; }
    if (jpeg.empty()) { err = "No photo yet."; return false; }
    if (S.running.exchange(true)) { err = "A model is already being generated."; return false; }
    S.cancel = false;
    {
        std::lock_guard<std::mutex> l(S.mu);
        S.st = AiStatus{};
        S.st.phase = AiPhase::Sending;
        S.start = Clock::now();
        S.finished = false;
    }
    std::thread(worker, jpeg, hint, key).detach();
    return true;
}

void aiCancel() {
    if (S.running) S.cancel = true;
}

AiStatus aiStatus() {
    std::lock_guard<std::mutex> l(S.mu);
    AiStatus s = S.st;
    if (!S.finished && s.phase != AiPhase::Idle) s.elapsed = std::chrono::duration<double>(Clock::now() - S.start).count();
    return s;
}

void aiAcknowledge() {
    if (S.running) return;
    std::lock_guard<std::mutex> l(S.mu);
    S.st = AiStatus{};
}

std::string aiStatusJson() {
    AiStatus s = aiStatus();
    const char* phase = "idle";
    std::string text;
    switch (s.phase) {
    case AiPhase::Idle: break;
    case AiPhase::Sending: phase = "working", text = "Sending your photo to the AI..."; break;
    case AiPhase::Thinking: phase = "working", text = "The AI is studying the photo..."; break;
    case AiPhase::Writing: phase = "working", text = "Building the model (" + std::to_string(s.vertices) + " points so far)..."; break;
    case AiPhase::Importing: phase = "working", text = "Checking the model..."; break;
    case AiPhase::Done: phase = "done", text = "Your model is ready - look at the computer!"; break;
    case AiPhase::Failed: phase = "failed", text = s.error; break;
    case AiPhase::Cancelled: phase = "failed", text = "Cancelled on the computer."; break;
    }
    return json{{"phase", phase}, {"text", text}, {"elapsed", (int)s.elapsed}}.dump();
}

// ---- key test ----
void aiTestKey(const std::string& key, const std::string& model) {
    if (S.testRunning.exchange(true)) return;
    {
        std::lock_guard<std::mutex> l(S.mu);
        S.test = {AiTestState::Running, "Checking the key..."};
    }
    std::thread([key, model] {
        AiTestStatus r;
        if (key.empty()) {
            r = {AiTestState::Failed, "Enter a key first."};
        } else {
            json req = {{"model", model.empty() ? kDefaultAiModel : model}, {"max_tokens", 8},
                        {"messages", json::array({{{"role", "user"}, {"content", "Reply with: ok"}}})}};
            bool sawChoices = false;
            std::string whole;
            Http h = request(req.dump(), key, [&](const std::string& l) { whole += l; }, 40);
            auto j = json::parse(h.raw, nullptr, false);
            sawChoices = j.is_object() && j.contains("choices");
            if (h.netErr.empty() && h.code == 200 && sawChoices)
                r = {AiTestState::Ok, "Key works - " + (model.empty() ? std::string(kDefaultAiModel) : model) + " answered."};
            else
                r = {AiTestState::Failed, httpError(h)};
        }
        std::lock_guard<std::mutex> l(S.mu);
        S.test = r;
        S.testRunning = false;
    }).detach();
}

AiTestStatus aiTestStatus() {
    std::lock_guard<std::mutex> l(S.mu);
    return S.test;
}
