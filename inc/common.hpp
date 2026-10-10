#pragma once
// Shared interfaces. Axes: +X = wind, +Y = lift. Default grid 160x80x80.
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

struct Grid      { int nx, ny, nz; std::vector<uint8_t> solid; };      // 1 = obstacle
struct FlowField { std::vector<float> ux, uy, uz, rho; };              // per cell
struct Forces    { glm::vec3 F; float cd, cl, frontalArea; };
struct SimParams { float inletU = 0.05f, tau = 0.55f; float aoaDeg = 0; };
struct TriMesh   {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
    std::vector<glm::vec3> normals;
};
