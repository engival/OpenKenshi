// The cost of the sim's route clearance (docs/formats/NAVMESH_CACHE.md "Clearance", CombatData::pathRadius): a sweep of
// routes across the world bake at the character radius, as the squads ask for them, logging the worst single request.
// The clearance search is capped (kClearanceNodes in knav/navmesh.cpp) so no request can hold a tick: the worst one here
// must stay under its limit (CPU time of the thread, so a loaded host does not fail it). argv[1]: the world bake (`knav_bake --world DIR`, the world_sample fixture), like ksim_world_hub.
// Exit 77 without it.

#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"

#include "DetourNavMesh.h"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <time.h>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

struct Spot
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

// A deterministic spread of points over the loaded mesh: the centres of its polygons, every nth one (so the routes span
// the whole window and cross rooms, stairs and doorways, not just one street).
std::vector<Spot> samplePoints(const ksim::MeshNavigator& nav, std::size_t wanted)
{
	const dtNavMesh& mesh = nav.mesh().detour();
	std::vector<Spot> all;
	for (int t = 0; t < mesh.getMaxTiles(); ++t)
	{
		const dtMeshTile* tile = mesh.getTile(t);
		if (tile == nullptr || tile->header == nullptr)
			continue;
		for (int p = 0; p < tile->header->polyCount; ++p)
		{
			const dtPoly& poly = tile->polys[p];
			if (poly.getType() != DT_POLYTYPE_GROUND || poly.vertCount == 0)
				continue;
			Spot centre;
			for (int k = 0; k < poly.vertCount; ++k)
			{
				const float* v = &tile->verts[poly.verts[k] * 3];
				centre.x += v[0];
				centre.y += v[1];
				centre.z += v[2];
			}
			centre.x /= poly.vertCount;
			centre.y /= poly.vertCount;
			centre.z /= poly.vertCount;
			all.push_back(centre);
		}
	}
	std::sort(all.begin(), all.end(), [](const Spot& a, const Spot& b) { return a.x != b.x ? a.x < b.x : a.z < b.z; });
	std::vector<Spot> points;
	if (all.empty())
		return points;
	const std::size_t step = std::max<std::size_t>(1, all.size() / wanted);
	for (std::size_t i = 0; i < all.size() && points.size() < wanted; i += step)
		points.push_back(all[i]);
	return points;
}

} // namespace

namespace
{

// CPU time of this thread in ms: a route preempted on a busy host (the full ctest run) costs the same as on an idle one.
double threadCpuMs()
{
	timespec at{};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &at);
	return static_cast<double>(at.tv_sec) * 1000.0 + static_cast<double>(at.tv_nsec) / 1.0e6;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), ksim::simlink::kHubZoneX, ksim::simlink::kHubZoneZ))
		{
			std::cout << "ksim_route_soak: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({ ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz });
		}
		const std::shared_ptr<ksim::MeshNavigator> nav = test::openWorldNav(argv[1], hubZones);
		const std::vector<Spot> points = samplePoints(*nav, 240);
		test::require(points.size() >= 100, "too few walkable spots to soak (" + std::to_string(points.size()) + ")");

		// Each route is asked the way a squad asks: from where one sample stands to the next, at the character radius
		// (the merged data's human footprint, 4 u).
		double worstMs = 0.0;
		double worstFoundMs = 0.0;
		double totalMs = 0.0;
		int asked = 0;
		int found = 0;
		std::string worstText;
		for (std::size_t i = 1; i < points.size(); ++i)
		{
			const Spot& from = points[i - 1];
			const Spot& to = points[i];
			const double began = threadCpuMs();
			const ksim::Route route = nav->routeFrom({ from.x, from.y, from.z }, to.x, to.z, to.y, false, 4.0);
			const double ms = threadCpuMs() - began;
			++asked;
			found += route.found ? 1 : 0;
			totalMs += ms;
			if (route.found && ms > worstFoundMs)
				worstFoundMs = ms;
			if (ms > worstMs)
			{
				worstMs = ms;
				worstText = "from " + std::to_string(static_cast<long long>(from.x)) + ", " + std::to_string(static_cast<long long>(from.z))
					+ " to " + std::to_string(static_cast<long long>(to.x)) + ", " + std::to_string(static_cast<long long>(to.z))
					+ (route.found ? "" : " (no route)");
			}
		}
		std::cout << "routes at radius 4: " << asked << " asked, " << found << " found, mean " << totalMs / asked << " ms\n";
		std::cout << "worst route: " << worstMs << " ms, " << worstText << "\n";
		std::cout << "worst route that found a way: " << worstFoundMs << " ms\n";
		// Reachability first: with a fixed 2,400-node budget only 100 of these 239 pairs routed at radius 4 (the client's
		// Hub-to-SW moves answered no_route); the probe-scaled budget and the corridor skip route 171.
		test::require(found >= 160, "only " + std::to_string(found) + " of " + std::to_string(asked) + " routes found at radius 4 (171 expected)");
		// Then the cost (navmesh.cpp kClearanceNodes, kClearanceProbeScale): a route that finds its way stays interactive; a
		// hopeless long pair pays up to 4x its radius-0 search before it answers OutOfNodes (~160-215 ms for 8 km, measured).
		// Re-pinned from a flat 50 ms when the budget became probe-scaled (task U follow-up, 2026-11-05); measured as thread CPU time since
		// kenshi-k16.15.12 (wall time hit 98.9 ms in a loaded full run; CPU time is 64 ms idle, about 74 ms with 32 spinners), limit 80 -> 110.
		test::require(worstFoundMs < 110.0, "a found route took " + std::to_string(worstFoundMs) + " ms CPU (limit 110 ms)");
		test::require(worstMs < 250.0, "a route took " + std::to_string(worstMs) + " ms (limit 250 ms for a hopeless pair)");
		std::cout << "ksim_route_soak: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_route_soak: FAIL: " << e.what() << "\n";
		return 1;
	}
}
