#pragma once
#include "common.hpp"

void   solverReset(const Grid& grid, const SimParams& params);
void   solverStep(const Grid& grid, const SimParams& params);
Forces solverForces();
void   solverSnapshot(FlowField& out);
