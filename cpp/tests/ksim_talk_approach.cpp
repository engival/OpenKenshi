// The talk / trade approach goal (docs/FEATURES.md "Talk / trade approach stops on the near side of a counter"), on hand-built
// navmeshes (no game data):
//   - a counter between the player and the barman: the old route (to the barman's own position) circles round behind the counter;
//     the new goal is on the near side, within the stopped talk reach, and its path is shorter;
//   - the walk (Walker::follow over the route) ends in reach and never goes behind the counter;
//   - a wall (thin, a long way round through a door) blocks the line: the goal is on the barman's side of it;
//   - no candidate (the target in a hole wider than the reach): the fallback is the route to the target's own position.
// The mesh is one tile of rectangles round omitted cells (the counter, the wall).

#include "ksim/interact.hpp"
#include "ksim/navigator.hpp"
#include "knav/navmesh.hpp"
#include "knav/travel.hpp"
#include "knav/walk.hpp"

#include "test_util.hpp"

#include <DetourNavMeshBuilder.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

namespace {

using namespace ksim;

// A tile of the grid xs x zs (world units, even) with the cells in `omit` (column, row) left out; all Land, y = 0.
std::shared_ptr<MeshNavigator> gridMesh(const std::vector<int>& xs, const std::vector<int>& zs, const std::set<std::pair<int, int>>& omit)
{
	const int nx = static_cast<int>(xs.size());
	const int nz = static_cast<int>(zs.size());
	std::vector<unsigned short> verts;
	for (int r = 0; r < nz; ++r)
		for (int c = 0; c < nx; ++c)
		{
			verts.push_back(static_cast<unsigned short>(xs[c] / 2));
			verts.push_back(0);
			verts.push_back(static_cast<unsigned short>(zs[r] / 2));
		}
	std::map<std::pair<int, int>, int> index; // cell -> polygon number
	for (int r = 0; r + 1 < nz; ++r)
		for (int c = 0; c + 1 < nx; ++c)
			if (omit.count({c, r}) == 0)
				index.emplace(std::make_pair(c, r), static_cast<int>(index.size()));
	const auto neighbour = [&](int c, int r) -> unsigned short
	{
		const auto it = index.find({c, r});
		return it == index.end() ? static_cast<unsigned short>(0xffff) : static_cast<unsigned short>(it->second);
	};
	std::vector<unsigned short> polys;
	std::vector<unsigned char> areas;
	std::vector<unsigned short> flags;
	for (int r = 0; r + 1 < nz; ++r)
		for (int c = 0; c + 1 < nx; ++c)
		{
			if (omit.count({c, r}) != 0)
				continue;
			const unsigned short corners[4] = {static_cast<unsigned short>(r * nx + c), static_cast<unsigned short>((r + 1) * nx + c),
				static_cast<unsigned short>((r + 1) * nx + c + 1), static_cast<unsigned short>(r * nx + c + 1)};
			for (const unsigned short v : corners)
				polys.push_back(v);
			polys.push_back(neighbour(c - 1, r)); // edge 0: x = c side
			polys.push_back(neighbour(c, r + 1));
			polys.push_back(neighbour(c + 1, r));
			polys.push_back(neighbour(c, r - 1));
			areas.push_back(static_cast<unsigned char>(knav::Area::Land));
			flags.push_back(knav::areaFlag(knav::Area::Land));
		}
	dtNavMeshCreateParams params{};
	params.verts = verts.data();
	params.vertCount = nx * nz;
	params.polys = polys.data();
	params.polyAreas = areas.data();
	params.polyFlags = flags.data();
	params.polyCount = static_cast<int>(areas.size());
	params.nvp = 4;
	params.walkableHeight = 18.0f;
	params.walkableRadius = 4.0f;
	params.walkableClimb = 5.0f;
	params.bmax[0] = 256.0f;
	params.bmax[1] = 20.0f;
	params.bmax[2] = 256.0f;
	params.cs = 2.0f;
	params.ch = 1.0f;
	params.buildBvTree = true;
	unsigned char* data = nullptr;
	int size = 0;
	test::require(dtCreateNavMeshData(&params, &data, &size), "dtCreateNavMeshData failed");
	knav::BakedTile tile;
	tile.x = 0;
	tile.z = 0;
	tile.data.assign(data, data + size);
	dtFree(data);
	knav::NavConfig config;
	config.tilesX = 1;
	config.tilesZ = 1;
	config.window = {0, 0, 0, 0};
	std::vector<knav::BakedTile> tiles;
	tiles.push_back(std::move(tile));
	return std::make_shared<MeshNavigator>(knav::NavMesh::fromTiles(config, std::move(tiles)));
}

state::Fighter fighterAt(double x, double z)
{
	state::Fighter f;
	f.x = x;
	f.y = 0.0;
	f.z = z;
	return f;
}

InteractTarget characterAt(double x, double z, double reachStopped)
{
	InteractTarget t;
	t.kind = InteractKind::Character;
	t.id = 7;
	t.x = x;
	t.y = 0.0;
	t.z = z;
	t.reach = characterReach(2.6, false, false);
	t.reachStopped = reachStopped;
	return t;
}

double lengthOf(const Route& route, double fx, double fz)
{
	double length = 0.0;
	for (const state::GroundPoint& p : route.points)
	{
		length += std::hypot(p.x - fx, p.z - fz);
		fx = p.x;
		fz = p.z;
	}
	return length;
}

constexpr auto kTalk = static_cast<std::uint8_t>(InteractAction::Talk);

} // namespace

int main()
{
	try
	{
		const double reach = characterReach(2.6, false, true); // 44.6
		// ---- A counter: x 60..140, z 96..112 is not walkable. The barman stands behind it (north), the player south of it.
		const auto counter = gridMesh({0, 60, 140, 200}, {0, 96, 112, 200}, {{1, 1}});
		const state::Fighter player = fighterAt(100.0, 20.0);
		const InteractTarget barman = characterAt(100.0, 122.0, reach);

		const Route old = counter->routeFrom({player.x, player.y, player.z}, barman.x, barman.z, barman.y);
		test::require(old.found, "the old route is not found");
		bool behind = false;
		for (const state::GroundPoint& p : old.points)
			behind = behind || p.z > 112.0;
		test::require(behind, "layout: the old route no longer goes round behind the counter");
		const double oldLength = lengthOf(old, player.x, player.z);

		const Route fresh = routeToInteract(*counter, player, barman, kTalk, false, 0.0); // no game data: no race, no clearance
		test::require(fresh.found && !fresh.points.empty(), "no talk route");
		const state::GroundPoint end = fresh.points.back();
		const double endDistance = std::hypot(end.x - barman.x, end.z - barman.z);
		const double freshLength = lengthOf(fresh, player.x, player.z);
		std::cout << "old length " << oldLength << ", new " << freshLength << ", goal (" << end.x << ", " << end.z << ") " << endDistance
			<< " from the barman\n";
		test::require(end.z < 96.0, "the goal is not on the near side of the counter");
		test::require(endDistance <= reach, "the goal is out of the stopped talk reach");
		test::require(freshLength < oldLength, "the new path is not shorter than the old one");
		for (const state::GroundPoint& p : fresh.points)
			test::require(p.z < 97.0, "the walk passes behind the counter");

		// The walk itself: follow the route and stand in reach.
		{
			knav::Position at{player.x, player.y, player.z};
			std::size_t next = 0;
			for (int i = 0; i < 400 && next < fresh.points.size(); ++i)
			{
				const knav::StepResult step = counter->walker().follow(at, fresh.points, next, 3.0);
				at = step.at;
				test::require(at.z < 97.0, "the walker went behind the counter");
			}
			test::require(next >= fresh.points.size(), "the walker did not finish the route");
			test::require(std::hypot(at.x - barman.x, at.z - barman.z) <= reach, "the walk does not end in talk reach");
		}

		// Another action keeps the plain route (to the target's own position).
		const Route loot = routeToInteract(*counter, player, barman, static_cast<std::uint8_t>(InteractAction::Loot), false, 0.0);
		test::require(loot.found && std::fabs(lengthOf(loot, player.x, player.z) - oldLength) < 0.01, "a non-talk order changed its route");

		// ---- Fallback: the target stands in the counter's hole, farther from the mesh than its reach: no candidate, the old route.
		const InteractTarget stuck = characterAt(100.0, 104.0, 1.0);
		const Route fallback = routeToInteract(*counter, player, stuck, kTalk, false, 0.0);
		const Route direct = counter->routeFrom({player.x, player.y, player.z}, stuck.x, stuck.z, stuck.y);
		test::require(fallback.found && direct.found && fallback.points.size() == direct.points.size()
			&& std::fabs(lengthOf(fallback, player.x, player.z) - lengthOf(direct, player.x, player.z)) < 0.01,
			"the fallback is not the route to the target");

		// ---- A wall (x 98..102, z 0..230, a door gap above it): talking through it is no option, the goal is the barman's side.
		const auto wall = gridMesh({0, 98, 102, 200}, {0, 230, 250}, {{1, 0}});
		const state::Fighter west = fighterAt(80.0, 100.0);
		const InteractTarget east = characterAt(115.0, 100.0, reach);
		const Route round = routeToInteract(*wall, west, east, kTalk, false, 0.0);
		test::require(round.found && !round.points.empty(), "no route round the wall");
		std::cout << "wall goal (" << round.points.back().x << ", " << round.points.back().z << ")\n";
		test::require(round.points.back().x > 102.0, "the goal is on the far side of the wall from the barman");
		test::require(std::hypot(round.points.back().x - east.x, round.points.back().z - east.z) <= reach, "the wall goal is out of reach");
	} catch (const std::exception& e) {
		std::cerr << "ksim_talk_approach: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_talk_approach: ok\n";
	return 0;
}
