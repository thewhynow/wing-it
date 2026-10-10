#pragma once
// Person 1
#include "common.hpp"

void   solverReset(const Grid& grid, const SimParams& params);
void   solverStep();
Forces solverForces();
void   solverSnapshot(FlowField& out);
