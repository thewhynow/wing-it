#include "models.hpp"
#include "geometry.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;
static std::vector<ModelEntry> gModels;

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

bool modelSupportedExt(const std::string& path) {
    std::string ext = lower(fs::path(path).extension().string());
    return ext == ".obj" || ext == ".stl" || ext == ".ply";
}

static void scan(const char* dir, bool custom) {
    std::error_code ec;
    std::vector<ModelEntry> found;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue; // hidden + in-progress imports
        if (!e.is_regular_file(ec)) continue;
        if (modelSupportedExt(name)) found.push_back({e.path().generic_string(), name, custom});
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return lower(a.label) < lower(b.label); });
    gModels.insert(gModels.end(), found.begin(), found.end());
}

void modelsRefresh() { gModels.clear(); scan(kSampleDir, false); scan(kUploadDir, true); }
const std::vector<ModelEntry>& modelsList() { return gModels; }
std::string modelLabel(const std::string& id) { return fs::path(id).filename().string(); }

bool modelIsCustom(const std::string& id) {
    for (const auto& m : gModels)
        if (m.id == id) return m.custom;
    return false;
}

std::string modelUploadId(const std::string& filename) {
    // directory_iterator yields path(dir) / filename, so build the ID the same way
    return (fs::path(kUploadDir) / filename).generic_string();
}

std::string modelSanitizeName(const std::string& name) {
    // basename on both separators (a phone / Windows client may send either)
    size_t slash = name.find_last_of("/\\");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    std::string out;
    for (unsigned char c : base) {
        if (std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '(' || c == ')') out += (char)c;
        else if (c == ' ') out += '_';
        // everything else (control chars, ':', '%', unicode bytes, ...) is dropped
    }
    while (!out.empty() && (out[0] == '.' || out[0] == '-')) out.erase(0, 1);
    if (out.size() > 80) { // keep the extension
        std::string ext = fs::path(out).extension().string();
        out = out.substr(0, 80 - ext.size()) + ext;
    }
    if (!modelSupportedExt(out) || fs::path(out).stem().string().empty()) return "";
    return out;
}

static std::mutex gCommitMutex; // exists-check + rename must be atomic w.r.t. other importers

// validate tmp with loadMesh, then rename to a unique name in kUploadDir
static std::string commit(const fs::path& tmp, const std::string& name, std::string& err) {
    std::error_code ec;
    TriMesh probe;
    std::string why;
    if (!loadMesh(tmp.string(), probe, &why)) {
        fs::remove(tmp, ec);
        err = "can't use " + name + ": " + why;
        return "";
    }
    std::lock_guard<std::mutex> lock(gCommitMutex);
    std::string stem = fs::path(name).stem().string(), ext = lower(fs::path(name).extension().string());
    for (int n = 1; n < 10000; ++n) {
        std::string candidate = n == 1 ? stem + ext : stem + "-" + std::to_string(n) + ext;
        fs::path dst = fs::path(kUploadDir) / candidate;
        if (fs::exists(dst, ec)) continue; // case-insensitive on macOS / Windows by itself
        fs::rename(tmp, dst, ec);
        if (ec) { fs::remove(tmp, ec); err = "couldn't save " + candidate + ": " + ec.message(); return ""; }
        return modelUploadId(candidate);
    }
    fs::remove(tmp, ec);
    err = "too many files named " + name;
    return "";
}

static fs::path tempPath(const std::string& name) {
    static std::atomic<unsigned> counter{0};
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    // hidden (leading dot) so modelsRefresh() ignores it, keeps the extension so loadMesh knows the format
    return fs::path(kUploadDir) / (".incoming-" + std::to_string(t) + "-" + std::to_string(counter++) +
                                   lower(fs::path(name).extension().string()));
}

static bool prepare(const std::string& rawName, std::string& name, std::string& err) {
    name = modelSanitizeName(rawName);
    if (name.empty()) {
        err = modelSupportedExt(rawName) ? "bad file name" : "unsupported file type (use " + std::string(kModelExtsHint) + ")";
        return false;
    }
    std::error_code ec;
    fs::create_directories(kUploadDir, ec);
    return true;
}

std::string modelsImportBytes(const std::string& rawName, const char* data, size_t size, std::string& err) {
    std::string name;
    if (!prepare(rawName, name, err)) return "";
    if (size == 0) { err = "the file is empty"; return ""; }
    if (size > kMaxModelBytes) { err = "the file is too large (max " + std::to_string(kMaxModelBytes >> 20) + " MB)"; return ""; }
    fs::path tmp = tempPath(name);
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f || !f.write(data, (std::streamsize)size)) {
            std::error_code ec;
            fs::remove(tmp, ec);
            err = "couldn't write to " + std::string(kUploadDir);
            return "";
        }
    }
    return commit(tmp, name, err);
}

std::string modelsImportFile(const std::string& srcPath, std::string& err) {
    std::string name;
    if (!prepare(srcPath, name, err)) return "";
    std::error_code ec;
    if (!fs::is_regular_file(srcPath, ec)) { err = "not a file: " + srcPath; return ""; }
    auto size = fs::file_size(srcPath, ec);
    if (ec) { err = "can't read " + srcPath; return ""; }
    if (size == 0) { err = "the file is empty"; return ""; }
    if (size > kMaxModelBytes) { err = "the file is too large (max " + std::to_string(kMaxModelBytes >> 20) + " MB)"; return ""; }
    fs::path tmp = tempPath(name);
    fs::copy_file(srcPath, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { fs::remove(tmp, ec); err = "couldn't copy " + modelLabel(srcPath) + " into " + kUploadDir; return ""; }
    return commit(tmp, name, err);
}
