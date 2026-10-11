#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#if defined(__APPLE__)
    #define GL_SILENCE_DEPRECATION
    #include <OpenGL/gl3.h>
#else
    #include <GL/gl.h>
#endif
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
    cameraState.dist *= 1.0f - GetMouseWheelMove() * 0.1f; // zoom step scales with distance
    cameraState.dist = Clamp(cameraState.dist, 1.0f, 400.0f); // far enough to see the whole 160x80x80 grid

    cameraState.camera.position = (Vector3){
        cameraState.dist * cosf(cameraState.pitch) * sinf(cameraState.yaw),
        cameraState.dist * sinf(cameraState.pitch),
        cameraState.dist * cosf(cameraState.pitch) * cosf(cameraState.yaw)
    };

    DrawCircleV(GetMousePosition(), 5, DARKBLUE);
}

// one GPU vertex per cell, uploaded once and drawn as GL points
static Mesh buildRow(const std::vector<Vector2>& coords, float z) {
    Mesh mesh = {};
    mesh.vertexCount = (int)coords.size();
    mesh.vertices = (float*)MemAlloc(mesh.vertexCount * 3 * sizeof(float));

    float* out = mesh.vertices;
    for (const auto& c : coords) {
        *out++ = c.x; *out++ = c.y; *out++ = z;
    }

    UploadMesh(&mesh, false);
    return mesh;
}

void voxel_loop(AppState& appState, Render3DState& cameraState) {
    const Grid& g = cameraState.grid;
    static std::vector<Mesh> rows; // rows[k] is the mesh for coords[k]

    // raylib can't draw GL_POINTS from a mesh, so use a tiny flat-color shader and draw it ourselves
    static Shader dotShader = LoadShaderFromMemory(
        "#version 330\n"
        "in vec3 vertexPosition; uniform mat4 mvp;\n"
        "void main() { gl_Position = mvp * vec4(vertexPosition, 1.0); }\n",
        "#version 330\n"
        "uniform vec4 color; out vec4 finalColor;\n"
        "void main() { finalColor = color; }\n");
    static int mvpLoc = GetShaderLocation(dotShader, "mvp");
    static int colorLoc = GetShaderLocation(dotShader, "color");

    // add a new z row each frame until there's one per grid cell in z
    if ((int)cameraState.coords.size() < g.nz) {
        std::vector<Vector2> morecoords;
        morecoords.reserve(g.nx * g.ny);
        for (int x = -(g.nx/2); x < g.nx - g.nx/2; x++) {
            for (int y = -(g.ny/2); y < g.ny - g.ny/2; y++) {
                morecoords.push_back(Vector2(x, y));
            }
        }
        cameraState.coords.push_back(morecoords);
    }

    // upload rows that don't have a mesh yet. row 0 gets the lowest z, so it's furthest
    // from the default camera (which sits on +z)
    while (rows.size() < cameraState.coords.size()) {
        int k = (int)rows.size();
        rows.push_back(buildRow(cameraState.coords[k], (float)(k - g.nz/2)));
    }

    BeginMode3D(cameraState.camera);
    rlDrawRenderBatchActive(); // flush anything raylib queued so draw order stays right

    Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    rlEnableShader(dotShader.id);
    rlSetUniformMatrix(mvpLoc, mvp);
    rlSetUniform(colorLoc, red, SHADER_UNIFORM_VEC4, 1);
    glPointSize(2.0f);
    for (const auto& row : rows) {
        rlEnableVertexArray(row.vaoId);
        glDrawArrays(GL_POINTS, 0, row.vertexCount);
    }
    rlDisableVertexArray();
    rlDisableShader();

    EndMode3D();
}

void renderSetMesh(const TriMesh& mesh){
    Mesh rayMesh;
    rayMesh.indices;

    #error SWITCH INDICES GLOBALLY TO uint16_t
    rayMesh.indices = (uint16_t*) MemAlloc(mesh.indices.size());

    rayMesh.vertices = (float*) MemAlloc(mesh.vertices.size() * sizeof(float));
    std::memcpy(rayMesh.vertices, mesh.vertices.data(), mesh.vertices.size() * sizeof(float));
    rayMesh.normals = (float*) MemAlloc(mesh.normals.size() * sizeof(float));
    std::memcpy(rayMesh.normals, mesh.normals.data(), mesh.normals.size() * sizeof(float));

    Model model = LoadModelFromMesh(rayMesh);
    DrawModel(model, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
}
