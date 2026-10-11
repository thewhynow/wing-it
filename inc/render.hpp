#pragma once
// Person 3
#include "common.hpp"
#include "raylib.h"

struct AppState;

struct Render3DState {
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
    Grid grid = {
        .nx = 160,
        .ny = 80,
        .nz = 80,
        .solid = std::vector<uint8_t>(160 * 80 * 80, 0),
    };
    // First vector stores the Z of all the coordinates
    // Inner vector stores all the coordinates
    // vec2 stores X and Y positions
    std::vector<std::vector<Vector2>> coords;
};

void renderInit();
void renderSetMesh(const TriMesh& mesh);
void renderFrame(const FlowField& field, const Forces& forces, float slicePos);
void renderShutdown();

void camera_loop(AppState& appState, Render3DState& cameraState);
void voxel_loop(AppState& appState, Render3DState& cameraState);