#pragma once
#include "common.hpp"

void   solverReset(const Grid& grid, const SimParams& params);
void   solverStep(const Grid& grid, const SimParams& params);
float calculateFrontalArea(const Grid& grid);
Forces solverForces(const Grid& grid, const SimParams& params);
void   solverSnapshot(FlowField& out);
