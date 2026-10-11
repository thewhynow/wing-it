#pragma once
// "Photo -> 3D model": sends a photo to the Hack Club AI API (OpenAI-compatible, streamed) and asks for a
// low-poly Wavefront .obj, which is validated and imported into the model library (models.hpp).
// All work runs on a background thread; every function here is thread-safe and never blocks the caller.
#include <string>

enum class AiPhase {
    Idle,
    Sending,     // request in flight, nothing back yet
    Thinking,    // model is reasoning, no mesh text yet
    Writing,     // mesh text is streaming in
    Importing,   // validating + saving the .obj
    Done,
    Failed,
    Cancelled,
};

struct AiStatus {
    AiPhase phase = AiPhase::Idle;
    double elapsed = 0;       // seconds since the run started (frozen once finished)
    int vertices = 0, faces = 0; // counted live while the mesh streams in
    int attempt = 1;          // 2 = first mesh was rejected, asking the model to fix it
    std::string modelId;      // Done: the imported model's ID (models.hpp)
    std::string error;        // Failed: human readable reason
};

// Starts a run. jpeg = the photo's bytes, hint = optional "what is it?" text from the user.
// Returns false (with err) if a run is already going or no API key is set.
bool aiStart(const std::string& jpeg, const std::string& hint, std::string& err);
void aiCancel();                 // asks the running job to stop; phase becomes Cancelled shortly after
AiStatus aiStatus();
void aiAcknowledge();            // Done / Failed / Cancelled -> Idle (call after the UI has shown the result)
bool aiBusy();                   // Sending / Thinking / Writing / Importing
std::string aiStatusJson();      // for the phone page: {"phase":"writing","text":"...","vertices":N,...}

// Settings "Test key": one tiny request, so a wrong key is caught before a long generation.
enum class AiTestState { Idle, Running, Ok, Failed };
struct AiTestStatus { AiTestState state = AiTestState::Idle; std::string message; };
void aiTestKey(const std::string& key, const std::string& model);
AiTestStatus aiTestStatus();
