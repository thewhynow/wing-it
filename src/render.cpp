#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "solver.hpp"
#if defined(__APPLE__)
    #define GL_SILENCE_DEPRECATION
    #include <OpenGL/gl3.h>
#else
    #include <GL/gl.h>
#endif
#include <math.h>
#include <algorithm>
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
    cameraState.dist *= 1.0f - GetMouseWheelMove() * 0.1f; // zoom step scales with distance
    cameraState.dist = Clamp(cameraState.dist, 0.00000000000001f, 4000000000000.0f); // far enough to see the whole 160x80x80 grid

    cameraState.camera.position = (Vector3){
        cameraState.dist * cosf(cameraState.pitch) * sinf(cameraState.yaw),
        cameraState.dist * sinf(cameraState.pitch),
        cameraState.dist * cosf(cameraState.pitch) * cosf(cameraState.yaw)
    };

    DrawCircleV(GetMousePosition(), 5, DARKBLUE);
}

static Model meshModel = {}; // uploaded once by renderSetMesh, drawn in voxel_loop

// trilinear sample of the flow at grid position p (cell centres sit at i + 0.5). solid cells count as still air
static void sampleFlow(const FlowField &f, const Grid &g, Vector3 p, Vector3 &vel, float &rho) {
    float fx = p.x - 0.5f, fy = p.y - 0.5f, fz = p.z - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy), z0 = (int)floorf(fz);
    float tx = fx - x0, ty = fy - y0, tz = fz - z0;

    vel = {0, 0, 0};
    rho = 0.0f;
    float rhoW = 0.0f;
    for (int c = 0; c < 8; ++c) {
        int dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
        int x = std::clamp(x0 + dx, 0, g.nx - 1),
            y = std::clamp(y0 + dy, 0, g.ny - 1),
            z = std::clamp(z0 + dz, 0, g.nz - 1);
        float w = (dx ? tx : 1 - tx) * (dy ? ty : 1 - ty) * (dz ? tz : 1 - tz);

        size_t idx = FlowField::cellIndex(g, x, y, z);
        if (!g.solid.empty() && g.solid[idx])
            continue;

        vel = Vector3Add(vel, Vector3Scale({f.ux[idx], f.uy[idx], f.uz[idx]}, w));
        rho += w * f.rho[idx];
        rhoW += w;
    }
    rho = rhoW > 0.0f ? rho / rhoW : 1.0f;
}

void voxel_loop(AppState& appState, Render3DState& cameraState) {
    Grid &g = cameraState.grid;
    auto &lines = cameraState.lines;

    // the solver is always running, so retrace every streamline from a fresh snapshot a few times a second
    static FlowField out; // static so the snapshot buffers aren't reallocated every frame
    static int framesSinceTrace = 1 << 30;
    if (++framesSinceTrace >= 30) {
        framesSinceTrace = 0;
        solverSnapshot(out);

        const float step = 0.5f; // in cells
        const int maxSteps = 4 * (g.nx + g.ny + g.nz);
        std::vector<std::vector<float>> rhos;
        float rhoMin = 1e30f, rhoMax = -1e30f;

        size_t l = 0;
        for (int sz = 1; sz < g.nz && !out.ux.empty(); sz += 3) {
            for (int sy = 1; sy < g.ny; sy += 3, ++l) {
                if (lines.size() <= l)
                    lines.emplace_back();
                Streamline &sl = lines[l];

                Vector3 p = {0.5f, (float)sy, (float)sz};
                Vector3 freeVel;
                float r;
                sampleFlow(out, g, p, freeVel, r);

                sl.target.clear();
                rhos.emplace_back();
                bool interesting = false;

                for (int s = 0; s < maxSteps; ++s) {
                    Vector3 vel;
                    sampleFlow(out, g, p, vel, r);
                    sl.target.push_back(p);
                    rhos.back().push_back(r);

                    // only keep lines that get bent or sped up / slowed down by the model
                    if (Vector3Distance(vel, freeVel) > 0.1f * Vector3Length(freeVel))
                        interesting = true;

                    float speed = Vector3Length(vel);
                    if (speed < 1e-6f)
                        break;
                    p = Vector3Add(p, Vector3Scale(vel, step / speed)); // fixed step along the flow

                    int x = (int)floorf(p.x), y = (int)floorf(p.y), z = (int)floorf(p.z);
                    if (x < 0 || x >= g.nx || y < 0 || y >= g.ny || z < 0 || z >= g.nz)
                        break;
                    if (!g.solid.empty() && g.solid[FlowField::cellIndex(g, x, y, z)]) {
                        interesting = true;
                        break;
                    }
                }

                sl.targetPresence = interesting && sl.target.size() > 1 ? 1.0f : 0.0f;
                if (sl.targetPresence > 0.0f)
                    for (float v : rhos.back()) {
                        rhoMin = std::min(rhoMin, v);
                        rhoMax = std::max(rhoMax, v);
                    }
            }
        }

        // pressure is rho / 3 in LBM, so normalizing rho gives the same thin -> thick ramp
        float range = rhoMax - rhoMin;
        for (size_t i = 0; i < rhos.size(); ++i) {
            Streamline &sl = lines[i];
            sl.targetWidth.resize(rhos[i].size());
            for (size_t p = 0; p < rhos[i].size(); ++p) {
                float t = range > 1e-9f ? std::clamp((rhos[i][p] - rhoMin) / range, 0.0f, 1.0f) : 0.0f;
                sl.targetWidth[p] = 0.15f + 1.1f * t;
            }

            // a brand new line starts on its target and just fades in
            if (sl.shown.empty()) {
                sl.shown = sl.target;
                sl.shownWidth = sl.targetWidth;
            }
        }
    }

    // ease shown toward target. a target that got shorter is padded with its end point so the tail pulls in
    float ease = 1.0f - expf(-6.0f * GetFrameTime());
    for (Streamline &sl : lines) {
        if (sl.target.empty())
            continue;
        sl.presence += (sl.targetPresence - sl.presence) * ease;

        size_t n = std::max(sl.shown.size(), sl.target.size());
        while (sl.shown.size() < n) {
            sl.shown.push_back(sl.shown.back());
            sl.shownWidth.push_back(0.0f);
        }
        for (size_t p = 0; p < n; ++p) {
            bool past = p >= sl.target.size();
            Vector3 to = past ? sl.target.back() : sl.target[p];
            float w = past ? 0.0f : sl.targetWidth[p];
            sl.shown[p] = Vector3Lerp(sl.shown[p], to, ease);
            sl.shownWidth[p] += (w - sl.shownWidth[p]) * ease;
        }

        // drop tail points once they've collapsed onto the end
        while (sl.shown.size() > sl.target.size() && Vector3Distance(sl.shown.back(), sl.target.back()) < 0.01f) {
            sl.shown.pop_back();
            sl.shownWidth.pop_back();
        }
    }

    BeginMode3D(cameraState.camera);

    // ribbons that always face the camera, since GL lines can't be thicker than 1px on macOS
    Vector3 off = {-(float)(g.nx/2), -(float)(g.ny/2), -(float)(g.nz/2)}; // same offset the model gets
    Vector3 cam = cameraState.camera.position;
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlBegin(RL_TRIANGLES);
    rlColor4ub(SKYBLUE.r, SKYBLUE.g, SKYBLUE.b, 255);
    for (const Streamline &sl : lines) {
        if (sl.presence < 0.01f || sl.shown.size() < 2)
            continue;

        // half-width offset at each point, perpendicular to the line and the view direction
        auto side = [&](size_t p) {
            size_t a = p > 0 ? p - 1 : p, b = p + 1 < sl.shown.size() ? p + 1 : p;
            Vector3 pos = Vector3Add(sl.shown[p], off);
            Vector3 tangent = Vector3Subtract(sl.shown[b], sl.shown[a]);
            Vector3 s = Vector3Normalize(Vector3CrossProduct(tangent, Vector3Subtract(cam, pos)));
            return Vector3Scale(s, 0.5f * sl.shownWidth[p] * sl.presence);
        };

        Vector3 s0 = side(0);
        for (size_t p = 0; p + 1 < sl.shown.size(); ++p) {
            Vector3 s1 = side(p + 1);
            Vector3 a = Vector3Add(sl.shown[p], off), b = Vector3Add(sl.shown[p + 1], off);
            Vector3 a0 = Vector3Subtract(a, s0), a1 = Vector3Add(a, s0),
                    b0 = Vector3Subtract(b, s1), b1 = Vector3Add(b, s1);
            rlVertex3f(a0.x, a0.y, a0.z); rlVertex3f(b0.x, b0.y, b0.z); rlVertex3f(b1.x, b1.y, b1.z);
            rlVertex3f(a0.x, a0.y, a0.z); rlVertex3f(b1.x, b1.y, b1.z); rlVertex3f(a1.x, a1.y, a1.z);
            s0 = s1;
        }
    }
    rlEnd();
    rlDrawRenderBatchActive();
    rlEnableBackfaceCulling();

    // mesh is in grid coords (0..n), the lines are centered on the origin
    if (meshModel.meshCount > 0)
        DrawModel(meshModel, {-(float)(g.nx/2), -(float)(g.ny/2), -(float)(g.nz/2)}, 1.0f, WHITE);

    EndMode3D();
}

// one directional light plus some ambient so the model isn't a flat silhouette
static Shader litShader() {
    static Shader shader = LoadShaderFromMemory(
        "#version 330\n"
        "in vec3 vertexPosition; in vec3 vertexNormal;\n"
        "uniform mat4 mvp; uniform mat4 matNormal;\n"
        "out vec3 fragNormal;\n"
        "void main() {\n"
        "    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));\n"
        "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
        "}\n",
        "#version 330\n"
        "in vec3 fragNormal; uniform vec4 colDiffuse; out vec4 finalColor;\n"
        "void main() {\n"
        "    vec3 light = normalize(vec3(-0.4, 0.8, 0.5));\n"
        "    float d = max(dot(normalize(fragNormal), light), 0.0);\n"
        "    finalColor = vec4(colDiffuse.rgb * (0.25 + 0.75 * d), colDiffuse.a);\n"
        "}\n");
    return shader;
}

void renderSetMesh(const TriMesh& mesh){
    if (meshModel.meshCount > 0) {
        // hand back the default shader first so UnloadModel doesn't free the shared lit one
        meshModel.materials[0].shader = {rlGetShaderIdDefault(), rlGetShaderLocsDefault()};
        UnloadModel(meshModel);
    }

    Mesh rayMesh;
    memset(&rayMesh, 0, sizeof rayMesh);

    rayMesh.indices = (uint16_t*) MemAlloc(mesh.indices.size() * sizeof(uint16_t));
    std::memcpy(rayMesh.indices, mesh.indices.data(), mesh.indices.size() * sizeof(uint16_t));

    rayMesh.vertices = (float*) MemAlloc(mesh.vertices.size() * sizeof(glm::vec3));
    std::memcpy(rayMesh.vertices, mesh.vertices.data(), mesh.vertices.size() * sizeof(glm::vec3));
    rayMesh.normals = (float*) MemAlloc(mesh.normals.size() * sizeof(glm::vec3));
    std::memcpy(rayMesh.normals, mesh.normals.data(), mesh.normals.size() * sizeof(glm::vec3));

    rayMesh.triangleCount = mesh.indices.size() / 3;
    rayMesh.vertexCount = mesh.vertices.size();

    UploadMesh(&rayMesh, false); // LoadModelFromMesh doesn't send it to the GPU
    meshModel = LoadModelFromMesh(rayMesh);
    meshModel.materials[0].shader = litShader();
}

void worldToFlow(float x, float y, float z) {

}
