// Arena strides (kenshi-k16.6.636, k16.6.607 item 5): the fight tier steps every arena step, everyone else every second (CombatSettings
// fightStride / idleStride). Invariants with the tiers on: no member goes unstepped longer than its stride; a target promotes both ends within
// one step; a walker on a 2,000 u route arrives within 2 % of the stride-1 time; a save and load between ticks gives the straight run's hash.
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "hub_point.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

struct Rig
{
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	Simulation sim;
	std::uint64_t walker = 0;
	std::uint64_t idler = 0;
	double routeLength = 0.0;
	Route route;
};

CombatSettings tiers(int idle)
{
	CombatSettings s;
	s.substeps = 2;
	s.idleStride = idle;
	return s;
}

void spawnAll(Rig& r, const kdata::GameData& gd, const std::string& sid)
{
	const auto spot = r.nav->walker().snap({kHubX, 1535.0, kHubZ}, 200.0);
	test::require(spot.has_value(), "no Hub spot");
	std::uint64_t rng = 12345;
	state::Fighter w = r.data->makeFighter(gd, sid, "", rng);
	w.team = 0;
	w.playerControlled = true;
	w.x = spot->x;
	w.y = spot->y;
	w.z = spot->z;
	r.walker = r.sim.spawnFighter(std::move(w)).get<state::Id>()->value;
	state::Fighter i = r.data->makeFighter(gd, sid, "", rng);
	i.team = 0;
	const auto there = r.nav->walker().snap({spot->x + 40.0, spot->y, spot->z}, 60.0);
	test::require(there.has_value(), "no spot for the idler");
	i.x = there->x;
	i.y = there->y;
	i.z = there->z;
	r.idler = r.sim.spawnFighter(std::move(i)).get<state::Id>()->value;
	// A 2,000 u route: the farthest of a few headings.
	for (const double angle : {0.0, 1.0, 2.0, 3.0, 4.0, 5.0})
	{
		Route route = r.nav->routeFrom({spot->x, spot->y, spot->z}, spot->x + std::sin(angle) * 2000.0, spot->z + std::cos(angle) * 2000.0);
		if (!route.found)
			continue;
		double length = 0.0;
		for (std::size_t k = 1; k < route.points.size(); ++k)
			length += std::hypot(route.points[k].x - route.points[k - 1].x, route.points[k].z - route.points[k - 1].z);
		if (length > r.routeLength)
		{
			r.routeLength = length;
			r.route = std::move(route);
		}
	}
	test::require(r.routeLength >= 1500.0, "no route of 1,500 u or more from the Hub spot");
}

Rig makeRig(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav, const std::string& sid, int idle)
{
	Rig r{data, nav, Simulation::load(KENSHI_DATA, 77)};
	r.sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	r.sim.enableCombat(data, tiers(idle));
	r.sim.enableNavigation(nav);
	spawnAll(r, gd, sid);
	r.sim.run(3);
	return r;
}

void startWalk(Rig& r)
{
	Route copy = r.route;
	r.sim.enqueue(Command::fighterMoveTo(state::Id{r.walker}, std::move(copy.points), std::move(copy.heights), 1, false, copy.approach));
}

// Ticks to the route's end.
int arrival(Rig& r)
{
	startWalk(r);
	int ticks = 0;
	for (; ticks < 40 * 60 * 3; ++ticks)
	{
		r.sim.run(1);
		if (ticks > 10 && r.sim.combat()->find(r.walker)->route.empty())
			break;
	}
	return ticks;
}

void checkStrides(Rig& r, int ticks, int stride)
{
	for (int t = 0; t < ticks; ++t)
	{
		r.sim.run(1);
		const std::uint64_t now = r.sim.combat()->phaseStep();
		for (const CombatArena::Member& m : r.sim.combat()->members())
		{
			const std::uint64_t allowed = m.fightTier ? 1 : static_cast<std::uint64_t>(stride);
			test::require(now - m.lastStep <= allowed, "a member went unstepped for " + std::to_string(now - m.lastStep) + " steps, stride " + std::to_string(allowed));
		}
	}
}

// The due list the loops outside the arena run on (kenshi-k16.6.639): every member is due within its stride (a far member's task / crime / door
// step still happens), the elapsed ticks it carries add up to the ticks that passed, and 1 / 1 strides publish everyone every tick.
void checkDue(Rig& r, int ticks, int maxGap)
{
	std::map<std::uint64_t, int> since;
	std::map<std::uint64_t, int> owed;
	bool anyIdle = false;
	for (int t = 0; t < ticks; ++t)
	{
		r.sim.run(1);
		for (const CombatArena::Member& m : r.sim.combat()->members())
		{
			++since[m.id];
			++owed[m.id];
			if (m.due)
			{
				test::require(static_cast<int>(m.dueTicks) <= maxGap, "a due member owes more ticks than its stride");
				since[m.id] = 0;
				owed[m.id] = 0;
			} else {
				anyIdle = true;
			}
			test::require(since[m.id] < maxGap, "a member was not on the due list within its stride");
		}
	}
	test::require(maxGap == 1 || anyIdle, "the due list never left a member out at stride " + std::to_string(maxGap));
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc >= 2, "usage: ksim_arena_strides HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		std::string sid;
		for (const kdata::Record* rec : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (rec->name == "Escaped Servant")
				sid = rec->stringID;
		}

		// Arrival: the stride-2 walker against the stride-1 one on the same route.
		Rig one = makeRig(data, gd, nav, sid, 1);
		Rig two = makeRig(data, gd, nav, sid, 2);
		const int t1 = arrival(one);
		const int t2 = arrival(two);
		std::printf("route %.0f u: arrives in %d ticks at stride 1, %d at stride 2\n", one.routeLength, t1, t2);
		test::require(t1 > 100 && t1 < 40 * 60 * 3, "the stride-1 walker did not arrive");
		test::require(std::abs(t1 - t2) * 50 <= t1, "the stride-2 arrival is more than 2 % off the stride-1 time");

		// Nobody unstepped longer than their stride, with a walker, an idler and (after the attack order) a fight.
		Rig inv = makeRig(data, gd, nav, sid, 2);
		startWalk(inv);
		checkStrides(inv, 200, 2);
		const CombatArena::Member* idler = nullptr;
		for (const CombatArena::Member& m : inv.sim.combat()->members())
		{
			if (m.id == inv.idler)
				idler = &m;
		}
		test::require(idler != nullptr && !idler->fightTier, "the idler is in the fight tier before any fight");
		// Promotion: the attack order promotes both ends within a step of the target being set.
		inv.sim.enqueue(Command::fighterAttack(state::Id{inv.walker}, state::Id{inv.idler}));
		bool promoted = false;
		for (int t = 0; t < 10 && !promoted; ++t)
		{
			inv.sim.run(1);
			const CombatArena::Member* w = nullptr;
			for (const CombatArena::Member& m : inv.sim.combat()->members())
			{
				if (m.id == inv.walker)
					w = &m;
			}
			if (w->fighter.target == inv.idler)
			{
				test::require(w->fightTier && idler->fightTier, "a target did not promote both ends in the step it was set");
				promoted = true;
			}
		}
		test::require(promoted, "the attack order never set a target");
		checkStrides(inv, 200, 2);

		{
			Rig d1 = makeRig(data, gd, nav, sid, 1);
			checkDue(d1, 60, 1);
			Rig d4 = makeRig(data, gd, nav, sid, 4); // 2 substeps: a due step every 2 ticks
			checkDue(d4, 100, 2);
		}

		for (const int idle : {1, 2})
		{
			// Save and load between ticks (half the members owe a step there): the same hash as the straight run.
			Rig a = makeRig(data, gd, nav, sid, idle);
			// A move order does not survive the round trip even at stride 1, so the walker is driven straight (a drive order does)
			a.sim.enqueue(Command::fighterDrive(state::Id{a.walker}, 1, 0.0, 1));
			a.sim.run(101);
			std::ostringstream out;
			a.sim.save(out);
			const std::string bytes = out.str();
			a.sim.run(100);
			const std::uint64_t straight = a.sim.stateHash();
			std::istringstream in(bytes);
			a.sim.reload(in);
			a.sim.run(100);
			std::printf("stride %d hash straight %016llx reloaded %016llx\n", idle, static_cast<unsigned long long>(straight), static_cast<unsigned long long>(a.sim.stateHash()));
			test::require(a.sim.stateHash() == straight, "stride "+std::to_string(idle)+": a save and load mid-stride changed the state hash against the straight run");
		}
		// The same through a fight and its tail (kenshi-k16.6.658): the tier is rebuilt from the fighter state at a load, so a member that left the fight
		// predicate just before the save must already be idle in the straight run too. Saves at several ticks after the attack order, a fight's start,
		// middle and end among them, each reloaded and run on against the straight run.
		for (const int after : {5, 41, 97, 163, 251, 389, 523})
		{
			Rig a = makeRig(data, gd, nav, sid, 2);
			a.sim.enqueue(Command::fighterAttack(state::Id{a.walker}, state::Id{a.idler}));
			a.sim.run(after);
			std::ostringstream out;
			a.sim.save(out);
			const std::string bytes = out.str();
			a.sim.run(120);
			const std::uint64_t straight = a.sim.stateHash();
			std::istringstream in(bytes);
			a.sim.reload(in);
			a.sim.run(120);
			std::printf("fight +%d hash straight %016llx reloaded %016llx\n", after, static_cast<unsigned long long>(straight), static_cast<unsigned long long>(a.sim.stateHash()));
			test::require(a.sim.stateHash() == straight, "a save and load " + std::to_string(after) + " ticks into a fight changed the state hash against the straight run");
		}
		std::cout << "ksim_arena_strides ok\n";
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
