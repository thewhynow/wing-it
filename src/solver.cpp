#include "solver.hpp"

constexpr float cx[19] = { 0, 1, -1, 0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 0, 0, 0 };
constexpr float cy[19] = { 0, 0, 0, 1, -1, 0, 0, 1, -1, -1, -1, 0, 0, 0, 0, 1, 1, -1, -1 };
constexpr float cz[19] = { 0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr float w[19]  = { 1.f/3.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f };
constexpr int opp[19] = { 0, 2, 1, 4, 3, 6, 5, 10, 9, 8, 7, 14, 13, 12, 11, 18, 17, 16, 15 };

std::vector<float> f_in;
std::vector<float> f_out;

void   solverReset(const Grid& grid, const SimParams& params) {
	f_in.resize(grid.nx * grid.ny * grid.nz * 19, 0.f);
	f_out.resize(grid.nx * grid.ny * grid.nz * 19, 0.f);

	float rho = 1.0f;
	float u = params.inletU;
	float v = 0.0f;
	float w = 0.0f;
	float u_sq = u * u + v * v + w * w;

	usize_t totalCells = grid.nx * grid.ny * grid.nz;
	for (int idx = 0; idx < totalCells; idx++) {
		for (int i = 0; i < 19; i++) {
			float cu = cx[idx] * u + cy[idx] * v + cz[idx] * w;
			float feq = w[i] * rho * (1.f + 3.f * cu + 4.5f * cu * cu - 1.5f * u_sq);

			int mem_idx = idx * 19 + i;
			f_in[mem_idx] = feq;
			f_out[mem_idx] = feq;
		}
	}
};

void   solverStep() {};

Forces solverForces() { return Forces{}; };

void   solverSnapshot(FlowField& out) {};
