#include "solver.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

constexpr int cx[19] = { 0, 1, -1, 0, 0, 0, 0, 1, 1, -1, -1, 1, 1, -1, -1, 0, 0, 0, 0 };
constexpr int cy[19] = { 0, 0, 0, 1, -1, 0, 0, 1, -1, 1, -1, 0, 0, 0, 0, 1, 1, -1, -1 };
constexpr int cz[19] = { 0, 0, 0, 0, 0, 1, -1, 0, 0, 0, 0, 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr float weight[19]  = { 1.f/3.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/18.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f, 1.f/36.f };
constexpr int opp[19] = { 0, 2, 1, 4, 3, 6, 5, 10, 9, 8, 7, 14, 13, 12, 11, 18, 17, 16, 15 };
std::mutex solverMutex;

// structure of arrays: direction i of cell idx lives at f[i * totalCells + idx], so each direction streams contiguously along x
std::vector<float> f_in;
std::vector<float> f_out;
size_t totalCells = 0;

// ---- drag bookkeeping. Links are (fluid cell, direction j pointing away from a solid neighbour) pairs. ----
// collideStream bounces the outgoing post-collision population f_post_i (i points into the solid) into slot
// opp[i]=j of the same fluid cell, and nothing else writes that slot, so after the swap f_in[j*N+idx] == f_post_i.
// Momentum exchange for a link is c_i*(f_post_i + f_reflected) = 2*c_i*f_post_i = -2*c_j*f_in[j*N+idx].
struct DragLink { uint32_t idx; uint8_t dir; };
static std::vector<DragLink> dragLinks;
static float frontalAreaCached = 0.f;
static std::mutex dragMutex;
static Forces dragSmoothed{glm::vec3(0.f), 0.f, 0.f, 0.f, 0.f};
static long long dragSamples = 0;
static long long dragSteps = 0;
static std::deque<float> dragHistory;
constexpr float DRAG_ALPHA = 0.02f;      // EMA weight once warmed up (bias-corrected running mean before that)
constexpr int   DRAG_HISTORY_EVERY = 25; // steps between history samples
constexpr size_t DRAG_HISTORY_MAX = 400;

static void buildDragLinks(const Grid& grid) {
	dragLinks.clear();
	const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
	for (int z = 0; z < nz; z++) for (int y = 0; y < ny; y++) for (int x = 0; x < nx; x++) {
		size_t idx = ((size_t)z * ny + y) * nx + x;
		if (grid.solid[idx]) continue;
		for (int j = 1; j < 19; j++) {
			int xs = x - cx[j]; // solid neighbour sits in direction opp[j]
			if (xs < 0 || xs >= nx) continue;
			int ys = (y - cy[j] + ny) % ny, zs = (z - cz[j] + nz) % nz;
			if (grid.solid[((size_t)zs * ny + ys) * nx + xs]) dragLinks.push_back({(uint32_t)idx, (uint8_t)j});
		}
	}
	frontalAreaCached = calculateFrontalArea(grid);
}

void solverResetDragAverage() {
	std::lock_guard<std::mutex> lock(dragMutex);
	dragSmoothed = Forces{glm::vec3(0.f), 0.f, 0.f, 0.f, frontalAreaCached};
	dragSamples = 0;
	dragSteps = 0;
	dragHistory.clear();
}

// runs on the thread that calls solverStep, right after the swap, so f_in is stable
static void accumulateDrag(const SimParams& params) {
	const size_t N = totalCells;
	glm::vec3 F(0.f);
	for (const DragLink& l : dragLinks) {
		float v = 2.f * f_in[l.dir * N + l.idx];
		F.x -= cx[l.dir] * v;
		F.y -= cy[l.dir] * v;
		F.z -= cz[l.dir] * v;
	}
	const float A = frontalAreaCached;
	const float q = params.inletRho * params.inletU * params.inletU * A;
	if (!(A > 0.f) || !(q > 0.f) || !std::isfinite(F.x + F.y + F.z)) return;
	const float k = 2.f / q;

	std::lock_guard<std::mutex> lock(dragMutex);
	float a = std::max(DRAG_ALPHA, 1.f / (float)(dragSamples + 1));
	dragSamples++;
	dragSmoothed.F += (F - dragSmoothed.F) * a;
	dragSmoothed.cd += (F.x * k - dragSmoothed.cd) * a;
	dragSmoothed.cl += (F.y * k - dragSmoothed.cl) * a;
	dragSmoothed.cz += (F.z * k - dragSmoothed.cz) * a;
	dragSmoothed.frontalArea = A;
	if (++dragSteps % DRAG_HISTORY_EVERY == 0) {
		dragHistory.push_back(dragSmoothed.cd);
		if (dragHistory.size() > DRAG_HISTORY_MAX) dragHistory.pop_front();
	}
}

void solverReset(const Grid& grid, const SimParams& params) {
	totalCells = (size_t)grid.nx * grid.ny * grid.nz;
	f_in.assign(totalCells * 19, 0.f);
	f_out.assign(totalCells * 19, 0.f);

	float rho = params.inletRho;
	float u = params.inletU;
	float v = 0.0f;
	float w = 0.0f;
	float u_sq = u * u + v * v + w * w;

	for (int i = 0; i < 19; i++) {
		float cu = cx[i] * u + cy[i] * v + cz[i] * w;
		float feq = weight[i] * rho * (1.f + 3.f * cu + 4.5f * cu * cu - 1.5f * u_sq);
		std::fill_n(f_in.begin() + i * totalCells, totalCells, feq);
		std::fill_n(f_out.begin() + i * totalCells, totalCells, feq);
	}

	if (grid.solid.size() == totalCells) buildDragLinks(grid);
	else { dragLinks.clear(); frontalAreaCached = 0.f; }
	solverResetDragAverage();
};

// collide one fluid cell and push its populations to the neighbours. Checked handles solids and the x ends;
// the unchecked version is branch free so whole rows away from the obstacle vectorize.
template <bool Checked>
static inline void collideStream(const float* __restrict fi, float* __restrict fo, const uint8_t* __restrict solid,
                                 const size_t* nrow, size_t N, int nx, size_t idx, int x,
                                 float omega, float inletRho, float inletU) {
	float f[19];
	float rho = 0.f, u = 0.f, v = 0.f, w = 0.f;
	#pragma clang loop unroll(full)
	for (int i = 0; i < 19; i++) {
		f[i] = fi[i * N + idx];
		rho += f[i];
		u += f[i] * cx[i];
		v += f[i] * cy[i];
		w += f[i] * cz[i];
	}
	const float inv_rho = 1.f / rho;
	u *= inv_rho;
	v *= inv_rho;
	w *= inv_rho;

	const bool inlet = Checked && x == 0;
	if (inlet) {
		rho = inletRho;
		u = inletU;
		v = 0.f;
		w = 0.f;
	}

	const float u_sq = 1.5f * (u * u + v * v + w * w);
	#pragma clang loop unroll(full)
	for (int i = 0; i < 19; i++) {
		float cu = cx[i] * u + cy[i] * v + cz[i] * w;
		float feq = weight[i] * rho * (1.f + 3.f * cu + 4.5f * cu * cu - u_sq);
		float f_post = inlet ? feq : f[i] - (f[i] - feq) * omega;

		int xn = x + cx[i];
		size_t neighbor_idx = nrow[i] + xn;
		if constexpr (Checked) {
			if (xn < 0 || xn >= nx) continue;
			if (solid[neighbor_idx]) {
				fo[opp[i] * N + idx] = f_post;
				continue;
			}
		}
		fo[i * N + neighbor_idx] = f_post;
	}
}

static std::vector<uint8_t> rowSolid; // per (z, y) row: any solid cell in it

void solverStep(const Grid& grid, const SimParams& params) {
	const int nx = grid.nx, ny = grid.ny, nz = grid.nz;
	const size_t N = totalCells;
	const float* __restrict fi = f_in.data();
	float* __restrict fo = f_out.data();
	const uint8_t* __restrict solid = grid.solid.data();
	const float omega = 1.f / params.tau;

	rowSolid.resize((size_t)ny * nz);
	#pragma omp parallel for schedule(static)
	for (int zy = 0; zy < ny * nz; zy++) {
		const uint8_t* r = solid + (size_t)zy * nx;
		uint8_t any = 0;
		for (int x = 0; x < nx; x++) any |= r[x];
		rowSolid[zy] = any;
	}

	// dynamic: rows near the obstacle take the slow path, and the M2 mixes fast and slow cores
	#pragma omp parallel for collapse(2) schedule(dynamic, 8)
	for (int z = 0; z < nz; z++) {
		for (int y = 0; y < ny; y++) {
			// periodic y/z wrap resolved once per row instead of per direction per cell
			const int ys[3] = { (y + ny - 1) % ny, y, (y + 1) % ny };
			const int zs[3] = { (z + nz - 1) % nz, z, (z + 1) % nz };
			const size_t row = ((size_t)z * ny + y) * nx;
			size_t nrow[19];
			bool nearSolid = false;
			for (int i = 0; i < 19; i++) {
				size_t zy = (size_t)zs[cz[i] + 1] * ny + ys[cy[i] + 1];
				nrow[i] = zy * nx;
				nearSolid |= rowSolid[zy] != 0;
			}

			if (nearSolid) {
				for (int x = 0; x < nx; x++)
					if (!solid[row + x])
						collideStream<true>(fi, fo, solid, nrow, N, nx, row + x, x, omega, params.inletRho, params.inletU);
				continue;
			}

			collideStream<true>(fi, fo, solid, nrow, N, nx, row, 0, omega, params.inletRho, params.inletU);
			#pragma clang loop vectorize(assume_safety) // fi and fo never alias, each fo slot has one writer
			for (int x = 1; x < nx - 1; x++)
				collideStream<false>(fi, fo, solid, nrow, N, nx, row + x, x, omega, params.inletRho, params.inletU);
			collideStream<true>(fi, fo, solid, nrow, N, nx, row + nx - 1, nx - 1, omega, params.inletRho, params.inletU);
		}
	}

	// outlet: copy the second-to-last column into the last
	#pragma omp parallel for collapse(2) schedule(static)
	for (int i = 0; i < 19; i++) {
		for (int zy = 0; zy < ny * nz; zy++) {
			size_t idx = (size_t)zy * nx + nx - 1;
			fo[i * N + idx] = fo[i * N + idx - 1];
		}
	}

	// snapshot reads f_in from the render thread, so only the swap needs the lock
	{
		std::lock_guard<std::mutex> lock(solverMutex);
		std::swap(f_in, f_out);
	}
	accumulateDrag(params);
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

// latest smoothed forces (lattice units). cd/cl/cz are normalised by 0.5*rho*U^2*frontalArea
Forces solverForces(const Grid&, const SimParams&) {
	std::lock_guard<std::mutex> lock(dragMutex);
	return dragSmoothed;
};

float solverDragCoefficient() {
	std::lock_guard<std::mutex> lock(dragMutex);
	return dragSmoothed.cd;
}

int solverDragHistory(float* out, int maxCount) {
	std::lock_guard<std::mutex> lock(dragMutex);
	int n = std::min<int>((int)dragHistory.size(), maxCount);
	std::copy(dragHistory.end() - n, dragHistory.end(), out);
	return n;
}

void solverSnapshot(FlowField& out) {
	std::lock_guard<std::mutex> lock(solverMutex);

	if (out.rho.size() != totalCells) {
		out.ux.resize(totalCells);
		out.uy.resize(totalCells);
		out.uz.resize(totalCells);
		out.rho.resize(totalCells);
	}

	const float* __restrict fi = f_in.data();
	const size_t N = totalCells;
	#pragma omp parallel for schedule(static)
	for (std::ptrdiff_t idx = 0; idx < (std::ptrdiff_t)N; idx++) {
		float rho = 0.f, u = 0.f, v = 0.f, w = 0.f;
		#pragma clang loop unroll(full)
		for (int i = 0; i < 19; i++) {
			float f = fi[i * N + idx];
			rho += f;
			u += f * cx[i];
			v += f * cy[i];
			w += f * cz[i];
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
