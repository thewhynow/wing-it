#pragma once
// Person 2
#include <string>
#include "common.hpp"

/* load an .obj / .stl (ascii or binary) / .ply (ascii or binary) into `out` (triangulated, degenerate tris
 * dropped, normals computed). meshes over 65535 vertices are simplified by vertex clustering to fit the uint16
 * indices. false on failure, with a human-readable reason in *err. thread-safe (no globals) */
bool loadMesh(const std::string& path, TriMesh& out, std::string* err = nullptr);

/* up-axis fix, center, rotate by aoaDeg, scale and place at (nx/4, ny/2, nz/2). result is in grid cells */
void fitToGrid(TriMesh& mesh, const Grid& grid, float aoaDeg, bool zUp);

/* fill grid.solid: 1 where a cell's center is inside the mesh. mesh must already be fit to the grid */
void voxelize(const TriMesh& mesh, Grid& grid);

/* built-in test shapes, in their own units (run through fitToGrid before use) */
TriMesh makeSphere();
TriMesh makeCube();
TriMesh makeNacaWing();
TriMesh makeCar();
