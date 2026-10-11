#pragma once
// Model library: bundled samples ("assets/sample models") + user models ("uploads", from file picker / phone QR).
// A model ID is the file's path relative to the working directory; use modelLabel() for display.
#include <string>
#include <vector>

constexpr const char* kSampleDir = "./assets/sample models";
constexpr const char* kUploadDir = "./uploads";

struct ModelEntry { std::string id, label; bool custom; };

void modelsRefresh();                        // rescan both folders (samples first, then uploads, each sorted)
const std::vector<ModelEntry>& modelsList();
std::string modelLabel(const std::string& id); // filename only
