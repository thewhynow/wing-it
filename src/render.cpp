#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include "ui.hpp"
#include "render.hpp"

void camera_loop(AppState& appState, Render3DState& cameraState) {
    // --- update (before drawing) ---
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Vector2 d = GetMouseDelta();
        cameraState.yaw -= d.x * 0.005f;
        cameraState.pitch += d.y * 0.005f;
        cameraState.pitch = Clamp(cameraState.pitch, -1.5f, 1.5f); // avoid flipping over the poles
    }
    cameraState.dist -= GetMouseWheelMove() * 0.5f; // optional zoom
    cameraState.dist = Clamp(cameraState.dist, 3.0f, 40.0f);

    cameraState.camera.position = (Vector3){
        cameraState.dist * cosf(cameraState.pitch) * sinf(cameraState.yaw),
        cameraState.dist * sinf(cameraState.pitch),
        cameraState.dist * cosf(cameraState.pitch) * cosf(cameraState.yaw)
    };

    DrawCircleV(GetMousePosition(), 5, DARKBLUE);
}

void voxel_loop(AppState& appState, const Render3DState& cameraState) {
    static int currentZIndex = 0;
    // currentZIndex++;
    // currentZIndex %= 1;

    BeginMode3D(cameraState.camera);

    for (const auto& coord : cameraState.coords[currentZIndex]) {
        DrawSphere(Vector3(coord.x, coord.y, currentZIndex), 0.01, RED);
    }

    EndMode3D();
}