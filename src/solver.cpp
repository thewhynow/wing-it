#include "solver.hpp"

constexpr float cx[19] = { 0, 1, -1, 0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 0, 0, 0 };
constexpr float cy[19] = { 0, 0, 0, 1, -1, 0, 0, 1, -1, -1, -1, 0, 0, 0, 0, 1, 1, -1, -1 };
constexpr float cz[19] = { 0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr float w[19]  = { 1.f/3.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f };
constexpr int opposite[19] = { 0, 2, 1, 4, 3, 6, 5, 10, 9, 8, 7, 14, 13, 12, 11, 18, 17, 16, 15 };

void   solverReset(const Grid& grid, const SimParams& params) {};

void   solverStep() {};

Forces solverForces() { return Forces{}; };

void   solverSnapshot(FlowField& out) {};
