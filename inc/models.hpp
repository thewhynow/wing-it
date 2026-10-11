#pragma once
// Model library: bundled samples ("assets/sample models") + user models ("uploads", from file picker / phone QR).
// A model ID is the file's path relative to the working directory; use modelLabel() for display.
#include <cstddef>
#include <string>
#include <vector>

constexpr const char* kSampleDir = "./assets/sample models";
constexpr const char* kUploadDir = "./uploads";
constexpr const char* kModelExtsHint = ".obj, .stl or .ply";
constexpr size_t kMaxModelBytes = 64u << 20; // 64 MB, applies to imported / uploaded files

struct ModelEntry { std::string id, label; bool custom; };

// --- main thread only ---
void modelsRefresh();                        // rescan both folders (samples first, then uploads, each sorted)
const std::vector<ModelEntry>& modelsList();
bool modelIsCustom(const std::string& id);   // true for models in kUploadDir

// --- pure / thread-safe ---
std::string modelLabel(const std::string& id); // filename only
bool modelSupportedExt(const std::string& path); // .obj / .stl / .ply, any case
std::string modelUploadId(const std::string& filename); // ID of kUploadDir/filename, same form modelsList() uses
// client-supplied name -> safe plain filename (path components, leading dots and odd characters stripped).
// "" if nothing usable is left or the extension is unsupported.
std::string modelSanitizeName(const std::string& name);

// Import into kUploadDir. Thread-safe (used by the HTTP server threads and the file-dialog worker):
// writes a hidden temp file, validates it with loadMesh, then renames it to a unique name (never overwrites).
// Returns the new model ID, or "" with a reason in err.
std::string modelsImportBytes(const std::string& name, const char* data, size_t size, std::string& err);
std::string modelsImportFile(const std::string& srcPath, std::string& err); // copy a file from anywhere on disk
