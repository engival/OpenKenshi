// Staleness by agreement: see knav/navgeom.hpp.
#include "knav/navgeom.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace knav {

namespace {
constexpr std::size_t kSmall = 200; // under this many samples either way the percentages are noise
constexpr std::size_t kSmallMisses = 12;
constexpr double kMinCovered = 90.0; // CALIBRATED: see NAVMESH_CACHE.md "Staleness by agreement"
constexpr double kMaxSurplus = 10.0;
constexpr std::size_t kRegionCells = 25; // 100 u2 of surface: a room, a landing; smaller pieces are noise
constexpr double kJoinStep = 3.0; // u: neighbouring samples this level are one surface

// Union-find over sample indices.
struct Sets
{
	std::vector<int> parent;
	explicit Sets(std::size_t n) : parent(n) { for (std::size_t i = 0; i < n; ++i) parent[i] = static_cast<int>(i); }
	int find(int i) { while (parent[static_cast<std::size_t>(i)] != i) i = parent[static_cast<std::size_t>(i)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(i)])]; return i; }
	void join(int a, int b) { parent[static_cast<std::size_t>(find(a))] = find(b); }
};

// Connected regions of a (cell, height)-sorted sample list: neighbouring cells (4-way) whose surfaces are within kJoinStep.
Sets regionsOf(const std::vector<compare::Sample>& samples, const compare::Grid& g)
{
	Sets sets(samples.size());
	for (std::size_t i = 0; i < samples.size(); ++i)
		for (const std::int64_t step : { std::int64_t{ 1 }, static_cast<std::int64_t>(g.nx) })
		{
			if (step == 1 && (samples[i].cell + 1) % g.nx == 0)
				continue;
			const auto [lo, hi] = compare::cellRange(samples, samples[i].cell + step);
			for (std::size_t j = lo; j < hi; ++j)
				if (std::fabs(samples[j].h - samples[i].h) <= kJoinStep)
					sets.join(static_cast<int>(i), static_cast<int>(j));
		}
	return sets;
}
}

bool TileAgreement::agrees() const noexcept
{
	if (bridges != 0)
		return false;
	if (cuts != 0 || offHeight * 20 > eroded)
		return false;
	if (nativeKept < kSmall && oursTotal < kSmall)
		return misses() <= kSmallMisses;
	return covered() >= kMinCovered && surplus() <= kMaxSurplus && bridges == 0;
}

std::vector<TileAgreement> tileAgreements(const std::vector<native::Mesh>& meshes, const std::vector<BakedTile>& bake,
	const NavConfig& config, const std::set<TileCoord>& only, double tol)
{
	std::vector<compare::Tri> nativeTris, ourTris;
	std::vector<compare::Wall> walls;
	compare::nativeGeometry(meshes, nativeTris, walls);
	std::vector<bool> interior;
	for (const native::Mesh& mesh : meshes)
		interior.push_back(mesh.interior());
	ourTris = compare::ourTriangles(NavMesh::fromTiles(config, bake));
	std::vector<TileCoord> coords;
	if (only.empty())
		for (const BakedTile& tile : bake)
			coords.push_back({ tile.x, tile.z });
	else
		coords.assign(only.begin(), only.end());
	// Each triangle goes to the tiles its bounds touch, once, so a zone's 324 tiles do not each scan every triangle.
	const auto bucket = [&](const std::vector<compare::Tri>& tris)
	{
		std::map<TileCoord, std::vector<compare::Tri>> out;
		const double size = static_cast<double>(config.tileSize);
		const auto at = [&](double v, double origin) { return static_cast<int>(std::floor((v - origin) / size)); };
		for (const compare::Tri& tri : tris)
		{
			const double lowX = std::min({ tri.a[0], tri.b[0], tri.c[0] }), highX = std::max({ tri.a[0], tri.b[0], tri.c[0] });
			const double lowZ = std::min({ tri.a[2], tri.b[2], tri.c[2] }), highZ = std::max({ tri.a[2], tri.b[2], tri.c[2] });
			for (int z = at(lowZ, config.originZ); z <= at(highZ, config.originZ); ++z)
				for (int x = at(lowX, config.originX); x <= at(highX, config.originX); ++x)
					if (only.empty() || only.count({ x, z }) != 0)
						out[{ x, z }].push_back(tri);
		}
		return out;
	};
	const std::map<TileCoord, std::vector<compare::Tri>> nativeBy = bucket(nativeTris), oursBy = bucket(ourTris);
	const std::vector<compare::Tri> none;
	std::vector<TileAgreement> result;
	for (const TileCoord& tile : coords)
	{
		const double x0 = config.originX + tile.x * static_cast<double>(config.tileSize);
		const double z0 = config.originZ + tile.z * static_cast<double>(config.tileSize);
		const double edge = static_cast<double>(config.tileSize);
		const compare::Grid grid{ x0, z0, 2.0, static_cast<int>(edge / 2.0), static_cast<int>(edge / 2.0) };
		const auto n = nativeBy.find(tile);
		const auto o = oursBy.find(tile);
		const compare::Score score = compare::score(n == nativeBy.end() ? none : n->second, walls, interior,
			o == oursBy.end() ? none : o->second, grid, tol, kAgreeRadius);
		TileAgreement agreement;
		agreement.tile = tile;
		{
			// The regions of the shipped surface a body fits on (kAgreeRadius off its walls) and of ours (baked eroded): does ours
			// join regions the shipped mesh keeps apart?
			const std::vector<compare::Tri>& nativeHere = n == nativeBy.end() ? none : n->second;
			const std::vector<compare::Tri>& oursHere = o == oursBy.end() ? none : o->second;
			const std::vector<compare::Sample> ns = compare::raster(nativeHere, grid);
			const std::vector<double> wall = compare::wallDistances(ns, grid, walls, tol, kAgreeRadius + 1.0);
			std::vector<compare::Sample> eroded;
			for (std::size_t k = 0; k < ns.size(); ++k)
				if (wall[k] >= kAgreeRadius)
					eroded.push_back(ns[k]);
			const std::vector<compare::Sample> os = compare::raster(oursHere, grid);
			if (!eroded.empty() && !os.empty())
			{
				Sets shipped = regionsOf(eroded, grid);
				Sets ours = regionsOf(os, grid);
				std::map<int, std::size_t> shippedSize, oursSize;
				for (std::size_t k = 0; k < eroded.size(); ++k)
					++shippedSize[shipped.find(static_cast<int>(k))];
				for (std::size_t k = 0; k < os.size(); ++k)
					++oursSize[ours.find(static_cast<int>(k))];
				std::map<int, std::set<int>> touched; // our region -> the shipped regions under it
				for (std::size_t k = 0; k < os.size(); ++k)
				{
					const int region = ours.find(static_cast<int>(k));
					if (oursSize[region] < kRegionCells)
						continue;
					const auto [lo, hi] = compare::cellRange(eroded, os[k].cell);
					for (std::size_t j = lo; j < hi; ++j)
						if (std::fabs(eroded[j].h - os[k].h) <= tol && shippedSize[shipped.find(static_cast<int>(j))] >= kRegionCells)
							touched[region].insert(shipped.find(static_cast<int>(j)));
				}
				agreement.eroded = eroded.size();
				std::vector<compare::Sample> missing;
				for (const compare::Sample& sample : eroded)
				{
					const auto [lo, hi] = compare::cellRange(os, sample.cell);
					double best = std::numeric_limits<double>::infinity();
					for (std::size_t j = lo; j < hi; ++j)
						best = std::min(best, std::fabs(os[j].h - sample.h));
					if (best > tol)
						missing.push_back(sample);
					else if (best > kHeightOff)
						++agreement.offHeight;
				}
				{
					Sets gaps = regionsOf(missing, grid);
					std::map<int, std::size_t> gapSize;
					for (std::size_t k = 0; k < missing.size(); ++k)
						++gapSize[gaps.find(static_cast<int>(k))];
					for (const auto& [region, size] : gapSize)
						agreement.cuts += size >= kRegionCells ? 1 : 0;
				}
				for (const auto& [region, under] : touched)
					agreement.bridges += under.size() >= 2 ? 1 : 0;
			}
		}
		agreement.nativeKept = score.nativeKept;
		agreement.nativeHit = score.nativeHit;
		agreement.oursTotal = score.oursTotal;
		agreement.oursNoNative = score.oursNoNative;
		result.push_back(agreement);
	}
	return result;
}

} // namespace knav
