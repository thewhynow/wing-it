#include "raylib.h"
#include "raymath.h"
#include <math.h>

int create3d(Camera camera) {
    
    float dist = 14.14f;
    float yaw = 0.0f;
    float pitch = PI / 4.0f;

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

    return 0;
}
