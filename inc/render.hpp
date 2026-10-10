#pragma once
// Person 3
#include "common.hpp"
#include "raylib.h"

struct AppState;

struct CameraState {
    float dist = 14.14f;
    float yaw = 0.0f;
    float pitch = PI / 4.0f;
    Camera camera = {
        .position = (Vector3){ 0.0f, 10.0f, 10.0f },
        .target = (Vector3){ 0.0f, 0.0f, 0.0f },
        .up = (Vector3){ 0.0f, 1.0f, 0.0f },
        .fovy = 45.0f,
        .projection = CAMERA_PERSPECTIVE
    };
};

void renderInit();
void renderSetMesh(const TriMesh& mesh);
void renderFrame(const FlowField& field, const Forces& forces, float slicePos);
void renderShutdown();

void camera_loop(AppState& appState, CameraState& cameraState);