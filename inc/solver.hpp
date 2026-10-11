#pragma once
#include "common.hpp"

void   solverReset(const Grid& grid, const SimParams& params);
void   solverStep(const Grid& grid, const SimParams& params);
float calculateFrontalArea(const Grid& grid);
// Forces are momentum-exchange forces on the solid, accumulated every solverStep on the stepping thread.
// All getters below are thread-safe (own mutex), safe to call from the stepping thread, and return a smoothed
// value (bias-corrected running mean, then EMA) that restarts on solverReset / solverResetDragAverage.
Forces solverForces(const Grid& grid, const SimParams& params); // args unused, kept for compatibility
float  solverDragCoefficient();   // smoothed Cd = 2Fx/(rho U^2 A), A = projected area along x; 0 if no solid
void   solverResetDragAverage();
int    solverDragHistory(float* out, int maxCount); // newest-last smoothed Cd samples (every 25 steps), returns count
void   solverSnapshot(FlowField& out);
