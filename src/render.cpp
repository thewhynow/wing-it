#include "raylib.h"
#include <imgui.h>
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
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !ImGui::GetIO().WantCaptureMouse) { // not while dragging a slider
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

static std::vector<Vector3> surfPos, surfNormal; // mesh verts in grid coords, for sampling the flow there
static float sliceY = 0.5f; // the floor slice sits just under the model

// blue -> cyan -> green -> yellow -> red
static Color jet(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ch = [&](float c) { return (unsigned char)(255 * std::clamp(1.5f - fabsf(4.0f * t - c), 0.0f, 1.0f)); };
    return {ch(3), ch(2), ch(1), 255};
}

// trace a streamline from every seed on the inlet face, widths ramp with pressure
static void traceLines(const FlowField &out, const Grid &g, std::vector<Streamline> &lines) {
    const float step = 0.5f; // in cells
    const int maxSteps = 4 * (g.nx + g.ny + g.nz);
    std::vector<std::vector<float>> rhos;
    float rhoMin = 1e30f, rhoMax = -1e30f;

    size_t l = 0;
    for (int sz = 1; sz < g.nz; sz += 3) {
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

    lines.resize(l); // grid may have shrunk, drop seeds that no longer exist
    float range = rhoMax - rhoMin;
    for (size_t i = 0; i < rhos.size(); ++i) {
        Streamline &sl = lines[i];
        sl.targetWidth.resize(rhos[i].size());
        for (size_t p = 0; p < rhos[i].size(); ++p) {
            float t = range > 1e-9f ? std::clamp((rhos[i][p] - rhoMin) / range, 0.0f, 1.0f) : 0.0f;
            sl.targetWidth[p] = 0.05f + 0.35f * t;
        }

        // a brand new line starts on its target and just fades in
        if (sl.shown.empty()) {
            sl.shown = sl.target;
            sl.shownWidth = sl.targetWidth;
        }
    }
}

// ease shown toward target. a target that got shorter is padded with its end point so the tail pulls in
static void easeLines(std::vector<Streamline> &lines, float dt) {
    float ease = 1.0f - expf(-6.0f * dt);
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
}

// ribbons that always face the camera, since GL lines can't be thicker than 1px on macOS
static void drawLines(const std::vector<Streamline> &lines, Vector3 off, Vector3 cam) {
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();
    rlDisableDepthMask(); // see-through, so overlapping lines shouldn't hide each other
    rlBegin(RL_TRIANGLES);
    rlColor4ub(SKYBLUE.r, SKYBLUE.g, SKYBLUE.b, 110);
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
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
}

void voxel_loop(AppState& appState, Render3DState& cameraState, UiState& uiState) {
    Grid &g = cameraState.grid;
    size_t nSurf = surfPos.size(), nSlice = (size_t)g.nx * g.nz;

    // sample surface verts then slice cells a few times a second, "shown" eases toward it so nothing pops
    static FlowField out;
    static std::vector<float> rhoTarget, rhoShown;
    static std::vector<Vector3> velTarget, velShown;
    static float lo = 1, hi = 1, loShown = 1, hiShown = 1;
    static int framesSinceSample = 1 << 30;
    if (++framesSinceSample >= 30) {
        framesSinceSample = 0;
        solverSnapshot(out);
        if (!out.ux.empty()) {
            rhoTarget.resize(nSurf + nSlice);
            velTarget.resize(nSurf + nSlice);
            for (size_t i = 0; i < nSurf; ++i) // a cell out along the normal so it reads fluid, not the voxels
                sampleFlow(out, g, Vector3Add(surfPos[i], surfNormal[i]), velTarget[i], rhoTarget[i]);
            for (int z = 0; z < g.nz; ++z)
                for (int x = 0; x < g.nx; ++x) {
                    size_t i = nSurf + x + (size_t)g.nx * z;
                    sampleFlow(out, g, {x + 0.5f, sliceY, z + 0.5f}, velTarget[i], rhoTarget[i]);
                }

            auto [mn, mx] = std::minmax_element(rhoTarget.begin(), rhoTarget.end());
            lo = *mn, hi = *mx;
            if (rhoShown.size() != rhoTarget.size()) {
                rhoShown = rhoTarget, velShown = velTarget;
                loShown = lo, hiShown = hi;
            }
        }
        if (uiState.showLines && !out.ux.empty())
            traceLines(out, g, cameraState.lines);
    }

    float ease = 1.0f - expf(-4.0f * GetFrameTime());
    for (size_t i = 0; i < rhoShown.size(); ++i) {
        rhoShown[i] += (rhoTarget[i] - rhoShown[i]) * ease;
        velShown[i] = Vector3Lerp(velShown[i], velTarget[i], ease);
    }
    loShown += (lo - loShown) * ease;
    hiShown += (hi - hiShown) * ease;
    // pressure is rho / 3 in LBM, so normalizing rho gives the same colour ramp
    auto shade = [&](size_t i) { return jet((rhoShown[i] - loShown) / std::max(hiShown - loShown, 1e-6f)); };
    bool haveFlow = !rhoShown.empty();

    if (haveFlow && meshModel.meshCount > 0) {
        Mesh &m = meshModel.meshes[0];
        for (size_t i = 0; i < nSurf; ++i) {
            Color c = uiState.showColors ? shade(i) : WHITE;
            std::memcpy(m.colors + 4 * i, &c, 4);
        }
        UpdateMeshBuffer(m, 3, m.colors, (int)nSurf * 4, 0);
    }

    if (uiState.showLines)
        easeLines(cameraState.lines, GetFrameTime());

    BeginMode3D(cameraState.camera);
    Vector3 off = {-(float)(g.nx/2), -(float)(g.ny/2), -(float)(g.nz/2)}; // mesh is in grid coords, the scene is centered

    // floor slice, corners at cell centres so the colours blend
    if (haveFlow) {
        rlDrawRenderBatchActive();
        rlDisableBackfaceCulling();
        rlBegin(RL_TRIANGLES);
        auto vert = [&](int x, int z) {
            Color c = shade(nSurf + x + (size_t)g.nx * z);
            rlColor4ub(c.r, c.g, c.b, 255);
            rlVertex3f(x + 0.5f + off.x, sliceY + off.y, z + 0.5f + off.z);
        };
        for (int z = 0; z + 1 < g.nz; ++z)
            for (int x = 0; x + 1 < g.nx; ++x) {
                vert(x, z); vert(x + 1, z); vert(x + 1, z + 1);
                vert(x, z); vert(x + 1, z + 1); vert(x, z + 1);
            }
        rlEnd();
        rlDrawRenderBatchActive();
        rlEnableBackfaceCulling();
    }

    if (meshModel.meshCount > 0)
        DrawModel(meshModel, off, 1.0f, WHITE);

    if (uiState.showLines) // after the model since they're see-through
        drawLines(cameraState.lines, off, cameraState.camera.position);

    // short black dashes along the flow, like a hedgehog plot
    if (haveFlow) {
        rlBegin(RL_LINES);
        rlColor4ub(0, 0, 0, 255);
        auto dash = [&](Vector3 base, Vector3 dir, float len) {
            if (Vector3Length(dir) < 1e-6f)
                return;
            Vector3 h = Vector3Scale(Vector3Normalize(dir), 0.5f * len);
            Vector3 a = Vector3Add(Vector3Subtract(base, h), off), b = Vector3Add(Vector3Add(base, h), off);
            rlVertex3f(a.x, a.y, a.z);
            rlVertex3f(b.x, b.y, b.z);
        };
        for (int z = 0; z < g.nz; z += 2)
            for (int x = 0; x < g.nx; x += 2) {
                Vector3 v = velShown[nSurf + x + (size_t)g.nx * z];
                dash({x + 0.5f, sliceY + 0.05f, z + 0.5f}, {v.x, 0, v.z}, 1.5f);
            }
        rlEnd();
    }

    EndMode3D();
}

// one directional light plus some ambient so the model isn't a flat silhouette
static Shader litShader() {
    static Shader shader = LoadShaderFromMemory(
        "#version 330\n"
        "in vec3 vertexPosition; in vec3 vertexNormal; in vec4 vertexColor;\n"
        "uniform mat4 mvp; uniform mat4 matNormal;\n"
        "out vec3 fragNormal; out vec4 fragColor;\n"
        "void main() {\n"
        "    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));\n"
        "    fragColor = vertexColor;\n"
        "    gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
        "}\n",
        "#version 330\n"
        "in vec3 fragNormal; in vec4 fragColor; uniform vec4 colDiffuse; out vec4 finalColor;\n"
        "void main() {\n"
        "    vec3 light = normalize(vec3(-0.4, 0.8, 0.5));\n"
        "    float d = max(dot(normalize(fragNormal), light), 0.0);\n"
        "    vec4 c = colDiffuse * fragColor;\n"
        "    finalColor = vec4(c.rgb * (0.4 + 0.6 * d), c.a);\n"
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

    // per-vertex pressure colours, rewritten every frame in voxel_loop
    rayMesh.colors = (unsigned char*) MemAlloc(mesh.vertices.size() * 4);
    std::memset(rayMesh.colors, 255, mesh.vertices.size() * 4);

    rayMesh.triangleCount = mesh.indices.size() / 3;
    rayMesh.vertexCount = mesh.vertices.size();

    surfPos.resize(mesh.vertices.size());
    surfNormal.resize(mesh.vertices.size());
    float minY = 1e30f;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        surfPos[i] = {mesh.vertices[i].x, mesh.vertices[i].y, mesh.vertices[i].z};
        surfNormal[i] = {mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z};
        minY = std::min(minY, mesh.vertices[i].y);
    }
    sliceY = std::max(0.5f, minY - 1.5f);

    UploadMesh(&rayMesh, true); // dynamic so the colours can be updated, LoadModelFromMesh doesn't upload
    meshModel = LoadModelFromMesh(rayMesh);
    meshModel.materials[0].shader = litShader();
}

void worldToFlow(float x, float y, float z) {

}
