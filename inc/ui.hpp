#pragma once
// Person 4
#include "common.hpp"

struct UiState { SimParams params; int model = 0; int resolution = 1; float slicePos = 0.5f; bool paused = false, reset = false; };

void uiDraw(UiState& ui, const Forces& forces, float stepsPerSec);
void simStart(const Grid& grid, const SimParams& params);
void simStop();
bool simLatest(FlowField& field, Forces& forces);
