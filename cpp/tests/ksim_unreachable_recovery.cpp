// Task BN: a melee target that cannot be reached is never a terminal state.
//  1. A bake pinch: a wall whose only gap is a 4 u cell, narrower than the body's clearance radius. The body-clearance
//     route finds no way through (BJ: an ~8 u Area::Edge band, routeFrom NoRoute with the body, a route at radius 0),
//     but the clearance-free route does: the fighter walks the gap and the fight ends.
//  2. No gap at all: the target is truly unreachable. The fighter gives it up (addBlockedTarget), retries when the
//     block expires (not a latch), and after the failed retry leaves the fight and walks its own goal instead of
//     standing there.
// The mesh is built in the test (no game data beyond the fighters' own): a 4 u grid with one row omitted, like
// ksim_chase_stuck.

#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "knav/travel.hpp"
#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <DetourNavMeshBuilder.h>

#include <cmath>
#include <cstdio>
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
// (the helper of ksim_chase_stuck).
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

struct Rig
{
	Rig(const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav, const std::shared_ptr<const CombatData>& data,
		const std::string& sid)
		: sim(Simulation::load(KENSHI_DATA, 5, RegionConfig{}))
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5)); // noon: full daylight
		const_cast<CombatArena*>(sim.combat())->setObserver([this](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::Hit && e.who == target)
				++hits;
		});
		std::uint64_t rng = 11;
		const auto spawn = [&](double x, double z, int team, bool player)
		{
			state::Fighter f = data->makeFighter(gd, sid, "", rng);
			f.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
			f.x = x;
			f.y = 0.0;
			f.z = z;
			f.team = team;
			f.playerControlled = player;
			return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		};
		target = spawn(20.0, 76.0, 0, true); // north of the wall; a player character keeps its spot (no order)
		attacker = spawn(20.0, 38.0, 1, false); // 38 u south of it, the wall between
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<state::Fighter*>(sim.combat()->find(id)); }

	Simulation sim;
	std::uint64_t target = 0;
	std::uint64_t attacker = 0;
	int hits = 0;
};

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

		// The grid: 4 u cells over 88 x 88 (22 x 22 cells: under the 512 polygons one Detour tile holds). The wall is
		// the cell row z 40..44, omitted across x 0..88 except in the pinch case's single 4 u cell (x 40..44).
		std::vector<int> xs;
		std::vector<int> zs;
		for (int v = 0; v <= 88; v += 4)
		{
			xs.push_back(v);
			zs.push_back(v);
		}

		// ---- 1. the pinch: the only gap is 4 u, narrower than the body's clearance radius ----
		{
			std::set<std::pair<int, int>> wall;
			for (int c = 0; c < 22; ++c)
				if (c != 10)
					wall.insert({ c, 10 }); // the gap is the kept cell x 40..44
			const std::shared_ptr<MeshNavigator> nav = gridMesh(xs, zs, wall);
			Rig rig(gd, nav, data, servantSid);
			// The pinch is real: the body's own clearance finds no route through the 4 u gap, the bare mesh does.
			const RaceCombat* race = data->race(rig.f(rig.attacker).raceSid);
			test::require(race != nullptr && race->footprintRadius > 0.0,
				"the body's clearance radius is wider than the 4 u gap");
			const Route byBody = nav->routeFrom({ 20.0, 0.0, 38.0 }, 20.0, 76.0, std::nullopt, false, race->footprintRadius);
			const Route bare = nav->routeFrom({ 20.0, 0.0, 38.0 }, 20.0, 76.0, std::nullopt, false, 0.0);
			test::require(!byBody.found, "the clearance route finds no way through the pinch");
			test::require(bare.found && !bare.points.empty(), "the clearance-free route crosses the pinch");
			rig.sim.enqueue(Command::fighterAttack(state::Id{ rig.attacker }, state::Id{ rig.target }));
			rig.sim.run(2);
			test::require(rig.f(rig.attacker).target == rig.target, "the attack order did not set the target");
			double crossedAt = -1.0;
			double closest = 1.0e9;
			double endedAt = -1.0; // the target down
			double lostAt = -1.0; // the attacker down (it lost the duel; who wins is not what this block measures)
			int drops = 0;
			std::uint64_t last = rig.target;
			for (int tick = 0; tick < 20 * 180; ++tick)
			{
				rig.f(rig.target).x = 20.0; // pinned at its spot, whatever the crowd push does
				rig.f(rig.target).z = 76.0;
				rig.sim.run(1);
				const state::Fighter& a = rig.f(rig.attacker);
				// A fighter that is down or out has not "given up": the sim clears its target because it cannot fight.
				if (a.target == 0 && last != 0 && !a.down && !a.unconscious && !a.dead)
					++drops; // given up on
				last = a.target;
				closest = std::min(closest, std::hypot(a.x - rig.f(rig.target).x, a.z - rig.f(rig.target).z));
				if (crossedAt < 0.0 && a.z > 44.0)
					crossedAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
				const state::Fighter& t = rig.f(rig.target);
				if (endedAt < 0.0 && (t.dead || t.unconscious || t.down))
					endedAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
				if (lostAt < 0.0 && (a.dead || a.unconscious || a.down))
					lostAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
				if (endedAt >= 0.0 || lostAt >= 0.0)
					break;
			}
			const state::Fighter& a = rig.f(rig.attacker);
			std::printf("  1. pinch (gap 4 u, narrower than the body): attacker at %.1f %.1f, crossed at %.1f s, closest %.1f u, "
				"hits %d, target %s at %.1f s, attacker %s at %.1f s, drops %d\n",
				a.x, a.z, crossedAt, closest, rig.hits, endedAt >= 0.0 ? "down" : "up", endedAt,
				lostAt >= 0.0 ? "down" : "up", lostAt, drops);
			test::require(drops == 0, "the fighter never gives up a target a clearance-free route reaches (the pinch)");
			test::require(crossedAt >= 0.0, "the fighter crosses the pinch (a route at radius 0)");
			test::require(crossedAt < 60.0, "the fighter crosses the pinch in under 60 s (" + std::to_string(crossedAt) + " s)");
			test::require(rig.hits > 0, "the fight starts on the far side of the pinch");
			// At the synced medical clock a duel lasts about twice as long in sim time; either side may be the one to go
			// down first, so the scenario's end is the fight's resolution, not the target's KO.
			test::require(endedAt >= 0.0 || lostAt >= 0.0, "the fight resolves: one of the two goes down within 180 s");
		}

		// ---- 2. no gap: truly unreachable ----
		{
			std::set<std::pair<int, int>> wall;
			for (int c = 0; c < 22; ++c)
				wall.insert({ c, 10 });
			const std::shared_ptr<MeshNavigator> nav = gridMesh(xs, zs, wall);

			// 2a. the block is re-tested, not latched: at expiry the navmesh is queried again and the 20 s timer is
			// re-armed while the target stays unreachable (kinspect 7726815). The target is never picked meanwhile.
			{
				Rig rig(gd, nav, data, servantSid);
				rig.sim.enqueue(Command::fighterAttack(state::Id{ rig.attacker }, state::Id{ rig.target }));
				double firstDrop = -1.0;
				double rearmedAt = -1.0;
				double previous = 0.0;
				bool sawArmed = false;
				bool picked = false;
				for (int tick = 0; tick < 20 * 40; ++tick)
				{
					rig.f(rig.attacker).x = 20.0; // both held at their spots: the re-test is the only thing that can happen
					rig.f(rig.attacker).z = 38.0;
					rig.f(rig.target).x = 20.0;
					rig.f(rig.target).z = 76.0;
					rig.sim.run(1);
					const double seconds = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
					const state::Fighter& a = rig.f(rig.attacker);
					if (firstDrop < 0.0 && a.aggroBlock > 0.0)
						firstDrop = seconds;
					// The block ran down to nothing, then the expiry re-test re-armed it.
					if (rearmedAt < 0.0 && sawArmed && previous < 0.5 && a.aggroBlock > 19.5)
						rearmedAt = seconds;
					sawArmed = sawArmed || a.aggroBlock > 19.5;
					previous = a.aggroBlock;
					picked = picked || a.target != 0;
				}
				const state::Fighter& a = rig.f(rig.attacker);
				std::printf("  2a. no gap: first give-up at %.1f s, re-armed at %.1f s, block %.0f s, target picked %d, target %d\n",
					firstDrop, rearmedAt, a.aggroBlock, picked ? 1 : 0, static_cast<int>(a.target));
				test::require(firstDrop >= 0.0, "the unreachable target is given up");
				test::require(rearmedAt >= 0.0 && rearmedAt > firstDrop + 15.0,
					"the block expires and is re-armed by the reachability re-test (re-tested, not latched)");
				test::require(!picked, "a blocked target that stays unreachable is not picked again");
			}

			// 2c. the entry is erased at expiry the moment a walk exists: the pair engages at once.
			{
				Rig rig(gd, nav, data, servantSid);
				rig.sim.enqueue(Command::fighterAttack(state::Id{ rig.attacker }, state::Id{ rig.target }));
				double droppedAt = -1.0;
				for (int tick = 0; tick < 20 * 2; ++tick)
				{
					rig.sim.run(1);
					const state::Fighter& a = rig.f(rig.attacker);
					if (a.aggroBlock > 0.0)
					{
						droppedAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
						break;
					}
				}
				test::require(droppedAt >= 0.0, "the unreachable target is given up");
				// Bring the target round to the attacker's side of the wall: the next expiry re-test finds a walk.
				rig.f(rig.target).x = 20.0;
				rig.f(rig.target).z = 30.0;
				double engagedAt = -1.0;
				for (int tick = 0; tick < 20 * 30 && engagedAt < 0.0; ++tick)
				{
					rig.sim.run(1);
					if (rig.f(rig.attacker).target == rig.target)
						engagedAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
				}
				const state::Fighter& a = rig.f(rig.attacker);
				std::printf("  2c. no gap: gave up at %.1f s, target moved to a walk, re-engaged at %.1f s "
					"(block id %llu, target %d)\n",
					droppedAt, engagedAt, static_cast<unsigned long long>(a.aggroBlockId), static_cast<int>(a.target));
				test::require(engagedAt >= 0.0 && engagedAt > droppedAt + 15.0,
					"the blocked entry is erased at expiry once a walk exists and the pair engages");
			}

			// 2b. after the failed retry the fighter leaves the fight and walks its own goal, not standing there.
			{
				Rig rig(gd, nav, data, servantSid);
				rig.sim.enqueue(Command::fighterAttack(state::Id{ rig.attacker }, state::Id{ rig.target }));
				double droppedAt = -1.0;
				double dropX = 0.0;
				double dropZ = 0.0;
				for (int tick = 0; tick < 20 * 10 && droppedAt < 0.0; ++tick)
				{
					rig.sim.run(1);
					const state::Fighter& a = rig.f(rig.attacker);
					if (a.aggroBlock > 0.0)
					{
						droppedAt = static_cast<double>(tick + 1) * Simulation::kTickSeconds;
						dropX = a.x;
						dropZ = a.z;
					}
				}
				test::require(droppedAt >= 0.0, "the unreachable target is given up");
				// Its own business: a goal south of where it gave up (the AI layer's walk, not the combat steer). The home
				// point is off the 88 u mesh (snapped to its south edge), so the goal is not "arrived" at once (kArrive 100).
				state::Fighter& a = rig.f(rig.attacker);
				a.goal.kind = state::IndividualKind::GoHome;
				a.goal.until = rig.sim.clockTick() + 20 * 600;
				a.goal.x = 20.0;
				a.goal.z = -200.0;
				double moved = 0.0;
				for (int tick = 0; tick < 20 * 25; ++tick)
				{
					rig.sim.run(1);
					const state::Fighter& n = rig.f(rig.attacker);
					moved = std::max(moved, std::hypot(n.x - dropX, n.z - dropZ));
					if (std::getenv("KSIM_BN_VERBOSE") != nullptr && tick % 100 == 0)
						std::printf("    t=%.0f goal %d walking %d route %zu at %.1f %.1f target %d block %.1f\n",
							tick * Simulation::kTickSeconds, static_cast<int>(n.goal.kind), n.walking() ? 1 : 0,
							n.route.size(), n.x, n.z, static_cast<int>(n.target), n.aggroBlock);
				}
				const state::Fighter& n = rig.f(rig.attacker);
				std::printf("  2b. no gap: gave up at %.1f s (%.1f %.1f), then walked %.1f u toward home in 25 s, "
					"at %.1f %.1f, target %d, block %.0f s\n",
					droppedAt, dropX, dropZ, moved, n.x, n.z, static_cast<int>(n.target), n.aggroBlock);
				test::require(moved >= 25.0, "the fighter leaves the fight and walks its goal (moved " + std::to_string(moved) + " u)");
			}
		}

		std::printf("ksim_unreachable_recovery: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_unreachable_recovery: FAIL: " << e.what() << "\n";
		return 1;
	}
}
