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

    // build the point cloud once and keep it on the GPU, rebuild only when the slice changes
    static Model points = {};
    static int builtZIndex = -1;

    if (builtZIndex != currentZIndex) {
        if (builtZIndex != -1) UnloadModel(points);

        // a tiny cube per cell (36 verts, no indices so we don't hit the 16-bit index limit)
        const float r = 0.05f;
        // outward normal n, and u, v with u x v = n so the winding is CCW from outside
        const Vector3 faces[6][3] = {
            {{ 1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
            {{ 0, 1, 0}, {0, 0, 1}, {1, 0, 0}}, {{ 0,-1, 0}, {1, 0, 0}, {0, 0, 1}},
            {{ 0, 0, 1}, {1, 0, 0}, {0, 1, 0}}, {{ 0, 0,-1}, {0, 1, 0}, {1, 0, 0}},
        };

        const auto& slice = cameraState.coords[currentZIndex];
        Mesh mesh = {};
        mesh.vertexCount = (int)slice.size() * 36;
        mesh.triangleCount = (int)slice.size() * 12;
        mesh.vertices = (float*)MemAlloc(mesh.vertexCount * 3 * sizeof(float));

        float* out = mesh.vertices;
        for (const auto& c : slice) {
            Vector3 p = { c.x, c.y, (float)currentZIndex };
            for (const auto& f : faces) {
                Vector3 n = f[0], u = f[1], v = f[2];
                Vector3 q[4];
                for (int s = 0; s < 4; s++) {
                    float a = (s == 1 || s == 2) ? 1.0f : -1.0f;
                    float b = (s >= 2) ? 1.0f : -1.0f;
                    q[s] = Vector3Add(p, Vector3Scale(Vector3Add(n, Vector3Add(Vector3Scale(u, a), Vector3Scale(v, b))), r));
                }
                for (int idx : {0, 1, 2, 0, 2, 3}) {
                    *out++ = q[idx].x; *out++ = q[idx].y; *out++ = q[idx].z;
                }
            }
        }
        UploadMesh(&mesh, false);
        points = LoadModelFromMesh(mesh);
        builtZIndex = currentZIndex;
    }

    BeginMode3D(cameraState.camera);
    DrawModel(points, Vector3Zero(), 1.0f, RED); // one draw call
    EndMode3D();
}