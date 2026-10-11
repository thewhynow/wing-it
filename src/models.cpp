#include "models.hpp"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;
static std::vector<ModelEntry> gModels;

static void scan(const char* dir, bool custom) {
    std::error_code ec;
    std::vector<ModelEntry> found;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".obj") found.push_back({e.path().generic_string(), e.path().filename().string(), custom});
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.label < b.label; });
    gModels.insert(gModels.end(), found.begin(), found.end());
}

void modelsRefresh() { gModels.clear(); scan(kSampleDir, false); scan(kUploadDir, true); }
const std::vector<ModelEntry>& modelsList() { return gModels; }
std::string modelLabel(const std::string& id) { return fs::path(id).filename().string(); }
