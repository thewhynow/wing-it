#pragma once
// Person 3
#include "common.hpp"

void renderInit();
void renderSetMesh(const TriMesh& mesh);
void renderFrame(const FlowField& field, const Forces& forces, float slicePos);
void renderShutdown();
