#include "solver.hpp"
#include <mutex>

constexpr float cx[19] = { 0, 1, -1, 0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 0, 0, 0 };
constexpr float cy[19] = { 0, 0, 0, 1, -1, 0, 0, 1, -1, -1, -1, 0, 0, 0, 0, 1, 1, -1, -1 };
constexpr float cz[19] = { 0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr float weight[19]  = { 1.f/3.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f };
constexpr int opp[19] = { 0, 2, 1, 4, 3, 6, 5, 10, 9, 8, 7, 14, 13, 12, 11, 18, 17, 16, 15 };
constexpr float ρ_air = 1.225f; // kg/m^3
std::mutex solverMutex;

std::vector<float> f_in;
std::vector<float> f_out;

void solverReset(const Grid& grid, const SimParams& params) {
	f_in.resize(grid.nx * grid.ny * grid.nz * 19, 0.f);
	f_out.resize(grid.nx * grid.ny * grid.nz * 19, 0.f);

	float rho = 1.0f;
	float u = params.inletU;
	float v = 0.0f;
	float w = 0.0f;
	float u_sq = u * u + v * v + w * w;

	size_t totalCells = grid.nx * grid.ny * grid.nz;
	for (int idx = 0; idx < totalCells; idx++) {
		for (int i = 0; i < 19; i++) {
			float cu = cx[idx] * u + cy[idx] * v + cz[idx] * w;
			float feq = weight[i] * rho * (1.f + 3.f * cu + 4.5f * cu * cu - 1.5f * u_sq);

			int mem_idx = idx * 19 + i;
			f_in[mem_idx] = feq;
			f_out[mem_idx] = feq;
		}
	}
};

void solverStep(const Grid& grid, const SimParams& params) {
	#pragma omp parallel for collapse(3)
	for (int z = 0; z < grid.nz; z++) {
		for (int y = 0; y < grid.ny; y++) {
			for (int x = 0; x < grid.nx; x++) {
				float rho = 0.f, u = 0.f, v = 0.f, w = 0.f;
				int idx = (z * grid.ny + y) * grid.nx + x;

				for (int i = 0; i < 19; i++) {
					int mem_idx = idx * 19 + i;
					rho += f_in[mem_idx];
					u += f_in[mem_idx] * cx[i];
					v += f_in[mem_idx] * cy[i];
					w += f_in[mem_idx] * cz[i];
				}
				u /= rho;
				v /= rho;
				w /= rho;

				if (x == 0) {
					rho = 1.f;
					u = params.inletU;
					v = 0.f;
					w = 0.f;
				}

				float f_post[19];
				for (int i = 0; i < 19; i++) {
					float cu = cx[i] * u + cy[i] * v + cz[i] * w;
					float feq = weight[i] * rho * (1.f + 3.f * cu + 4.5f * cu * cu - 1.5f * (u*u + v*v + w*w));
					int mem_idx = idx * 19 + i;
					if (x == 0) {
						f_post[i] = feq;
					} else {
						f_post[i] = f_in[mem_idx] - (f_in[mem_idx] - feq) / params.tau;
					}
				}

				for (int i = 0; i < 19; i++) {
					int xn = x + cx[i];

					if (xn < 0 || xn >= grid.nx) continue;

					int yn = y + (int)cy[i] + grid.ny % grid.ny;
					int zn = z + (int)cz[i] + grid.nz % grid.nz;
					size_t neighbor_idx = (zn * grid.ny + yn) * grid.nx + xn;
					if (grid.solid[neighbor_idx]) {
						f_out[idx * 19 + opp[i]] = f_post[i];
					} else {
						f_out[neighbor_idx * 19 + i] = f_post[i];
					}
				}
			}
		}
	}

	#pragma omp parallel for collapse(2)
	for (int z = 0; z < grid.nz; z++) {
		for (int y = 0; y < grid.ny; y++) {
			int x = grid.nx - 1;
			int idx = (z * grid.ny + y) * grid.nx + x;
			int nidx = (z * grid.ny + y) * grid.nx + x - 1;
			for (int i = 0; i < 19; i++) {
				int mem_idx = idx * 19 + i;
				int nmem_idx = nidx * 19 + i;
				f_out[mem_idx] = f_out[nmem_idx];
			}
		}
	}

	std::swap(f_in, f_out);
};

float calculateFrontalArea(const Grid& grid) {
	float area = 0.f;
	for (int z = 0; z < grid.nz; z++) {
		for (int y = 0; y < grid.ny; y++) {
			for (int x = 0; x < grid.nx; x++) {
				int idx = (z * grid.ny + y) * grid.nx + x;
				if (grid.solid[idx]) {
					area += 1.f;
					break;
				}
			}
		}
	}
	return area;
};

Forces solverForces(const Grid& grid, const SimParams& params) { 
	glm::vec3 F(0.f);
	for (int z = 0; z < grid.nz; z++) {
		for (int y = 0; y < grid.ny; y++) {
			for (int x = 0; x < grid.nx; x++) {
				int idx = (z * grid.ny + y) * grid.nx + x;
				if (!grid.solid[idx]) {
					for (int i = 0; i < 19; i++) {
						int xn = x + cx[i];

						if (xn < 0 || xn >= grid.nx) continue;

						int yn = y + (int)cy[i] + grid.ny % grid.ny;
						int zn = z + (int)cz[i] + grid.nz % grid.nz;
						size_t neighbor_idx = (zn * grid.ny + yn) * grid.nx + xn;
						if (grid.solid[neighbor_idx]) {
							float f_to = f_in[idx * 19 + i];
							float f_from = f_in[idx * 19 + opp[i]];
							F += glm::vec3(cx[i], cy[i], cz[i]) * (f_to + f_from);
						}
					}
				}
			}
		}
	}

	float frontalArea = calculateFrontalArea(grid);

	float cd = (2 * F.x) / (ρ_air * params.inletU * params.inletU * frontalArea);
	float cl = (2 * F.y) / (ρ_air * params.inletU * params.inletU * frontalArea);
	float cz = (2 * F.z) / (ρ_air * params.inletU * params.inletU * frontalArea);

	return Forces{F, cd, cl, cz, frontalArea};
};

void solverSnapshot(FlowField& out) {
	std::lock_guard<std::mutex> lock(solverMutex);

	size_t totalCells = f_in.size() / 19;

	if (out.rho.size() != totalCells) {
		out.ux.resize(totalCells);
		out.uy.resize(totalCells);
		out.uz.resize(totalCells);
		out.rho.resize(totalCells);
	}

	#pragma omp parallel for
	for (int idx = 0; idx < totalCells; idx++) {
		float rho = 0.f, u = 0.f, v = 0.f, w = 0.f;
		for (int i = 0; i < 19; i++) {
			int mem_idx = idx * 19 + i;
			rho += f_in[mem_idx];
			u += f_in[mem_idx] * cx[i];
			v += f_in[mem_idx] * cy[i];
			w += f_in[mem_idx] * cz[i];
		}
		u /= rho;
		v /= rho;
		w /= rho;

		out.rho[idx] = rho;
		out.ux[idx] = u;
		out.uy[idx] = v;
		out.uz[idx] = w;
	}
};
