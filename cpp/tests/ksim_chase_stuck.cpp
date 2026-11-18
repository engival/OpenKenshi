// A fighter steered straight at a target it cannot walk at (behind a wall, within the chase's 50 u direct band). Before
// the stuck timer in combat.cpp it pushed into the wall for ever (ksim_fight_crowd_real seed 2: a bandit pinned in a
// mesh corner, its target 28 u away across a wall, both holding the target for the whole run). Now kStuckRepath (5 s) of
// chasing beyond the ring with no route switches to the routed chase, and the route is kept across clips; here the wall
// has a short way round and the attacker takes it. MINE/UNSURE: the exe's behaviour for a target under 50 u behind a
// wall is not decoded; the timer and the switch are ours.
// The mesh is built in the test (no game data beyond the fighter's own): a 4 u grid with one row of cells missing.

#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include "knav/navmesh.hpp"
#include "knav/travel.hpp"
#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <DetourNavMeshBuilder.h>

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <utility>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// A tile of the grid xs x zs (world units, even) with the cells in `omit` (column, row) left out; all Land, y = 0
// (the helper of ksim_talk_approach).
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
	std::map<std::pair<int, int>, int> index;
	for (int r = 0; r + 1 < nz; ++r)
		for (int c = 0; c + 1 < nx; ++c)
			if (omit.count({ c, r }) == 0)
				index.emplace(std::make_pair(c, r), static_cast<int>(index.size()));
	const auto neighbour = [&](int c, int r) -> unsigned short
	{
		const auto it = index.find({ c, r });
		return it == index.end() ? static_cast<unsigned short>(0xffff) : static_cast<unsigned short>(it->second);
	};
	std::vector<unsigned short> polys;
	std::vector<unsigned char> areas;
	std::vector<unsigned short> flags;
	for (int r = 0; r + 1 < nz; ++r)
		for (int c = 0; c + 1 < nx; ++c)
		{
			if (omit.count({ c, r }) != 0)
				continue;
			const unsigned short corners[4] = { static_cast<unsigned short>(r * nx + c), static_cast<unsigned short>((r + 1) * nx + c),
				static_cast<unsigned short>((r + 1) * nx + c + 1), static_cast<unsigned short>(r * nx + c + 1) };
			for (const unsigned short v : corners)
				polys.push_back(v);
			polys.push_back(neighbour(c - 1, r));
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
	config.window = { 0, 0, 0, 0 };
	std::vector<knav::BakedTile> tiles;
	tiles.push_back(std::move(tile));
	return std::make_shared<MeshNavigator>(knav::NavMesh::fromTiles(config, std::move(tiles)));
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		std::string servantSid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servantSid = r->stringID;
		}
		test::require(!servantSid.empty(), "no Escaped Servant in the data");

		// The wall: the cell row z 40..44, x 0..32 (the way round is east of x 32). 4 u cells over 88 x 88 (22 x 22
		// cells: under the 512 polygons one Detour tile holds).
		std::vector<int> xs;
		std::vector<int> zs;
		for (int v = 0; v <= 88; v += 4)
		{
			xs.push_back(v);
			zs.push_back(v);
		}
		std::set<std::pair<int, int>> wall;
		for (int c = 0; c < 8; ++c)
			wall.insert({ c, 10 });
		const std::shared_ptr<MeshNavigator> nav = gridMesh(xs, zs, wall);

		Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5)); // noon: full daylight

		std::uint64_t rng = 11;
		const auto spawn = [&](double x, double z, int team, bool player)
		{
			state::Fighter f = data->makeFighter(gd, servantSid, "", rng);
			f.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
			f.x = x;
			f.y = 0.0;
			f.z = z;
			f.team = team;
			f.playerControlled = player;
			return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		};
		const std::uint64_t target = spawn(22.0, 76.0, 0, true); // a player character: it keeps its spot (no order)
		const std::uint64_t attacker = spawn(22.0, 38.0, 1, false); // 28 u apart, the wall between
		const auto fighter = [&](std::uint64_t id) -> state::Fighter&
		{
			return *const_cast<state::Fighter*>(sim.combat()->find(id));
		};

		sim.enqueue(Command::fighterAttack(state::Id{ attacker }, state::Id{ target }));
		sim.run(2);
		test::require(fighter(attacker).target == target, "the attack order did not set the target");
		test::require(fighter(attacker).z < 40.0, "the attacker did not start south of the wall");

		// Before the fix the straight steer pushed into the wall for ever (the walker holds it, and no progress is no
		// reason to change course). Now 5 s of that switches to the routed chase, which goes round the wall's east end.
		double crossedAt = -1.0;
		double closest = 1.0e9;
		for (int tick = 0; tick < 20 * 12; ++tick)
		{
			fighter(target).x = 22.0; // pinned at its spot, whatever the crowd push does
			fighter(target).z = 76.0;
			sim.run(1);
			if (std::getenv("KSIM_STUCK_VERBOSE") != nullptr && tick % 20 == 0)
			{
				const state::Fighter& d = fighter(attacker);
				std::printf("  t=%d x %.1f z %.1f state %d chaseDirect %d walkStuck %.1f fwd %.1f target %d\n", tick / 20, d.x, d.z,
					static_cast<int>(d.state), d.chaseDirect ? 1 : 0, d.walkStuck, d.forwardSpeed, d.target != 0);
			}
			const state::Fighter& a = fighter(attacker);
			closest = std::min(closest, std::hypot(a.x - fighter(target).x, a.z - fighter(target).z));
			if (crossedAt < 0.0 && a.z > 44.0)
				crossedAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
		}
		const state::Fighter& a = fighter(attacker);
		std::printf("attacker at %.1f %.1f after 12 s, target %.1f %.1f, closest %.1f u, crossed the wall at %.1f s, walkStuck %.1f\n",
			a.x, a.z, fighter(target).x, fighter(target).z, closest, crossedAt, a.walkStuck);
		test::require(crossedAt >= 0.0, "the attacker never got round the wall (it pushed into it for the whole 12 s)");
		test::require(crossedAt < 12.0, "the attacker got round the wall too late (" + std::to_string(crossedAt) + " s)");
		test::require(a.z > 44.0, "the attacker is not on the target's side of the wall");
		// The ring the approach stops at (CADENCE_LIVE M2: 23 + 2.4): it closed in, not merely crossed.
		test::require(closest <= 26.0, "the attacker never closed to the approach ring (closest " + std::to_string(closest) + " u)");
		
		std::printf("ksim_chase_stuck: ok (round the wall in %.1f s)\n", crossedAt);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_chase_stuck: FAIL: " << e.what() << "\n";
		return 1;
	}
}
