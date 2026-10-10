#include "raylib.h"
#include "raymath.h"
#include <math.h>

int create3d(void) {
    InitWindow(800, 450, "raylib [core] example - 3d mode");

    Camera3D camera = { 0 };
    camera.position = (Vector3){ 0.0f, 10.0f, 10.0f };
    camera.target = (Vector3){ 0.0f, 0.0f, 0.0f };    
    camera.up = (Vector3){ 0.0f, 1.0f, 0.0f };   
    camera.fovy = 45.0f;    
    camera.projection = CAMERA_PERSPECTIVE;

    SetTargetFPS(60);

    float dist = 14.14f;
    float yaw = 0.0f;
    float pitch = PI / 4.0f;

    while (!WindowShouldClose()) {

         // --- update (before drawing) ---
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            Vector2 d = GetMouseDelta();
            yaw   -= d.x * 0.005f;
            pitch += d.y * 0.005f;
            pitch = Clamp(pitch, -1.5f, 1.5f);   // avoid flipping over the poles
        }
        dist -= GetMouseWheelMove() * 0.5f;      // optional zoom
        dist = Clamp(dist, 3.0f, 40.0f);

        camera.position = (Vector3){
            dist * cosf(pitch) * sinf(yaw),
            dist * sinf(pitch),
            dist * cosf(pitch) * cosf(yaw)
        };

        BeginDrawing();
            ClearBackground(RAYWHITE);

            BeginMode3D(camera);

	        DrawCube((Vector3){0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 2.0f, RED);
            DrawCubeWires((Vector3){0.0f, 0.0f, 0.0f}, 2.0f, 2.0f, 2.0f, MAROON);

            EndMode3D();

        EndDrawing();

        DrawCircleV(GetMousePosition(), 5, DARKBLUE);
    }

    CloseWindow();
    return 0;
}
