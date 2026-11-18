#pragma once

// The surface comparison of knav_compare, as a library (the tool and the shipped-tile gate test share it): two navmeshes are
// sampled at the centres of a grid of cells, one sample per surface layer, and two samples match when they share a cell and lie
// within `tol` in height (the original's faces are coarse in height, so a tolerance of 30 measures where, not how high).
// Method: the kinspect session's knative.py compare (docs/formats/NAVMESH_CACHE.md "Conversion").

#include "knav/native.hpp"
#include "knav/navmesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace knav::compare {

using Vec = std::array<double, 3>;

struct Tri
{
	Vec a, b, c;
	int source = 0; // the native mesh index, or our polygon's area
};

struct Sample
{
	std::int64_t cell = 0;
	double h = 0.0;
	int tri = 0;
};

struct Grid
{
	double x0 = 0.0, z0 = 0.0, cell = 2.0;
	int nx = 0, nz = 0;
};

using Wall = std::array<Vec, 2>;

// Every triangle sampled at the centres of the cells it covers (xz), then one sample per surface layer, sorted by (cell, height).
std::vector<Sample> raster(const std::vector<Tri>& tris, const Grid& g);
// The samples of `cell` in a (cell, h)-sorted list: [first, last).
std::pair<std::size_t, std::size_t> cellRange(const std::vector<Sample>& samples, std::int64_t cell);
// For each sample of `a`, the signed height difference to the nearest-height sample of `b` in its cell (infinity if none).
std::vector<double> match(const std::vector<Sample>& a, const std::vector<Sample>& b);
// The native meshes' triangles (each face fanned) and wall segments.
void nativeGeometry(const std::vector<native::Mesh>& meshes, std::vector<Tri>& tris, std::vector<Wall>& walls);
// Our mesh's detail triangles (the surface Detour walks), `source` = the polygon's area. Polygons cut by region pruning (flags 0) are left out.
std::vector<Tri> ourTriangles(const NavMesh& mesh);
// Each sample's distance (xz) to the nearest wall at its height (within tol), up to `far`; infinity beyond.
std::vector<double> wallDistances(const std::vector<Sample>& samples, const Grid& g, const std::vector<Wall>& walls, double tol, double far);

// One box scored: how much of the original's surface (beyond `radius` of its walls) ours covers, and how much of ours it lacks.
struct Score
{
	std::size_t nativeKept = 0; // native samples at least `radius` from a native wall
	std::size_t nativeHit = 0; // ... within tol of one of ours
	std::size_t missWorld = 0;
	std::size_t missInterior = 0;
	std::size_t oursTotal = 0;
	std::size_t oursNoNative = 0; // ours with no native surface within tol (blue + yellow in the ppm)
	double cell = 2.0;
	double coveredPercent() const noexcept { return nativeKept == 0 ? 100.0 : 100.0 * static_cast<double>(nativeHit) / static_cast<double>(nativeKept); }
	double surplusPercent() const noexcept { return oursTotal == 0 ? 0.0 : 100.0 * static_cast<double>(oursNoNative) / static_cast<double>(oursTotal); }
};

// The native triangles / walls against our triangles over the grid; `interior` of a native mesh index says it is an interior.
Score score(const std::vector<Tri>& nativeTris, const std::vector<Wall>& walls, const std::vector<bool>& nativeInterior,
	const std::vector<Tri>& ourTris, const Grid& g, double tol, double radius);

} // namespace knav::compare
