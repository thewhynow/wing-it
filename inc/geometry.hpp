#pragma once
// Person 2
#include <string>
#include "common.hpp"

bool    loadMesh(const std::string& path, TriMesh& out);
void    fitToGrid(TriMesh& mesh, const Grid& grid);
void    applyAoA(TriMesh& mesh, float aoaDeg);
void    voxelize(const TriMesh& mesh, Grid& grid);
float   frontalArea(const Grid& grid);
TriMesh makeSphere();
TriMesh makeCube();
TriMesh makeNacaWing();
TriMesh makeCar();
