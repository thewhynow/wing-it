#include "raylib.h"
#include "raymath.h"
#include <math.h>
#include "ui.hpp"
#include "render.hpp"

void camera_loop(AppState& appState, CameraState& cameraState) {
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

    BeginMode3D(cameraState.camera);

    DrawCube((Vector3){0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 2.0f, RED);
    DrawCubeWires((Vector3){0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 2.0f, MAROON);

    EndMode3D();


    DrawCircleV(GetMousePosition(), 5, DARKBLUE);

}
