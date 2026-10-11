#pragma once
// Shared interfaces. Axes: +X = wind, +Y = lift. Default grid 160x80x80.
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

struct Grid      { int nx = 40, ny = 20, nz = 40;
    std::vector<uint8_t> solid; };      // 1 = obstacle
struct FlowField {
    std::vector<float> ux, uy, uz, rho;

    static size_t cellIndex(Grid &g, size_t x, size_t y, size_t z);
};              // per cell
struct Forces    { glm::vec3 F; float cd, cl, cz, frontalArea; };
struct SimParams { float inletU = 0.05f, tau = 0.55f; float aoaDeg = 0; };
struct TriMesh   {
    std::vector<glm::vec3> vertices;
    std::vector<uint16_t> indices;
    std::vector<glm::vec3> normals;
};
