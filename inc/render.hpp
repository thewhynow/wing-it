#pragma once
// Person 3
#include "common.hpp"
#include "raylib.h"

struct AppState;
struct UiState;

// "shown" eases toward the latest trace every frame so lines don't jump between retraces
struct Streamline {
    std::vector<Vector3> target, shown;
    std::vector<float> targetWidth, shownWidth; // ribbon width per point, from pressure
    float presence = 0.0f, targetPresence = 0.0f; // fades lines in and out
};

struct Render3DState {
    float dist = 14.14f;
    float yaw = 0.0f;
    float pitch = PI / 4.0f;
    Camera camera = {
        .position = Vector3{ 0.0f, 10.0f, 10.0f },
        .target = Vector3{ 0.0f, 0.0f, 0.0f },
        .up = Vector3{ 0.0f, 1.0f, 0.0f },
        .fovy = 45.0f,
        .projection = CAMERA_PERSPECTIVE
    };
    Grid grid;
    // one per seed on the inlet face (x = 0), in grid coords
    std::vector<Streamline> lines;
};

void renderInit();
void renderSetMesh(const TriMesh& mesh);
void renderFrame(const FlowField& field, const Forces& forces, float slicePos);
void renderShutdown();

// halftime preview: the raw (unrotated, unfitted) mesh, drawn rotated like buildModel does (Rx*Ry*Rz), no voxelizing.
void renderSetPreviewMesh(const TriMesh& rawMesh);
// draws the preview into an internal w x h render texture and returns it (flip V when showing it with ImGui::Image).
// call on the main thread between BeginDrawing/EndDrawing, outside BeginMode3D.
RenderTexture2D renderMeshPreview(int w, int h, glm::vec3 rotationDeg, Color background);

void camera_loop(AppState& appState, Render3DState& cameraState);
void voxel_loop(AppState& appState, Render3DState& cameraState, UiState& uiState);