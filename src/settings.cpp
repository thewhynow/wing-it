#include "settings.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sys/stat.h>

namespace fs = std::filesystem;

namespace {
std::mutex gMu;
std::string gKey, gModel;

fs::path settingsPath() {
    const char* home = std::getenv("HOME");
    return fs::path(home && *home ? home : ".") / ".wing-it" / "settings.json";
}

std::string trim(std::string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    return s.substr(i);
}
} // namespace

void settingsLoad() {
    std::lock_guard<std::mutex> l(gMu);
    std::ifstream f(settingsPath());
    if (!f) return;
    auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    if (j.contains("apiKey") && j["apiKey"].is_string()) gKey = trim(j["apiKey"].get<std::string>());
    if (j.contains("model") && j["model"].is_string()) gModel = trim(j["model"].get<std::string>());
}

bool settingsSave(const std::string& apiKey, const std::string& model, std::string& err) {
    std::lock_guard<std::mutex> l(gMu);
    std::error_code ec;
    fs::path p = settingsPath();
    fs::create_directories(p.parent_path(), ec);
    nlohmann::json j = {{"apiKey", trim(apiKey)}, {"model", trim(model)}};
    {
        std::ofstream f(p, std::ios::trunc);
        if (!f) { err = "couldn't write " + p.string(); return false; }
        f << j.dump(2) << "\n";
        if (!f) { err = "couldn't write " + p.string(); return false; }
    }
    chmod(p.c_str(), 0600); // the key is a secret
    gKey = trim(apiKey), gModel = trim(model);
    return true;
}

bool settingsKeyFromEnv() {
    std::lock_guard<std::mutex> l(gMu);
    const char* e = std::getenv("HACKCLUB_API_KEY");
    return gKey.empty() && e && *e;
}

std::string settingsApiKey() {
    std::lock_guard<std::mutex> l(gMu);
    if (!gKey.empty()) return gKey;
    const char* e = std::getenv("HACKCLUB_API_KEY");
    return e ? trim(e) : "";
}

std::string settingsModel() {
    std::lock_guard<std::mutex> l(gMu);
    return gModel.empty() ? kDefaultAiModel : gModel;
}

std::string settingsMaskedKey() {
    std::string k = settingsApiKey();
    if (k.empty()) return "";
    if (k.size() <= 12) return std::string(k.size(), '*');
    return k.substr(0, 9) + "..." + k.substr(k.size() - 4);
}
