#pragma once
// Phone upload server: serves assets/index.html on port 8081 and accepts model uploads into ./uploads.
#include <string>
#include <vector>

constexpr int kUploadPort = 8081;

void start_server_thread();
void stop_server_thread();

// --- thread-safe, callable from the render thread ---
std::string serverUrl();                       // "http://<lan-ip>:8081" (fixed once the server is started)
bool serverHasLan();                           // false if no network was found (URL then points at 127.0.0.1)
bool serverListening();                        // true once the port is bound
std::string serverError();                     // non-empty if the server couldn't start (e.g. port in use)
std::vector<std::string> serverTakeUploads();  // model IDs (see models.hpp) received since the last call

// --- photo for the AI model generator (phone page at /camera) ---
// The newest photo (JPEG bytes) sent from a phone since the last call; false if none arrived. Photos are kept
// in memory only, never written to disk.
bool serverTakePhoto(std::string& jpeg);
