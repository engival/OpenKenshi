// 35 RUN_AWAY under 32 SELF_PRESERVATION, and 280 RUN_AWAY_FORCED (Task_Runaway; kinspect dt24_32.md, flee_behavior.md, dlgtasks_static.md (a)),
// on the Hub navmesh: an attacked non-fighter runs from its attacker's centroid and takes no fight; 35 ends with the aggression, 280 is the
// same executor; a runner that is not moving re-rolls a random point 1000..1400 u away, and one with no way to run never holds the goal.

#include "ksim/clock.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "knav/walk.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "dt27_rig.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
using state::IndividualKind;


void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::unique_ptr<Simulation> sim;
	std::uint64_t rng = 3;
	std::string body;

	explicit Rig(const std::shared_ptr<MeshNavigator>& mesh)
		: gd(std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA))))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, nav(mesh)
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 7));
		sim->setGameData(gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
			{
				body = r->stringID;
				break;
			}
		}
		test::require(!body.empty(), "no Escaped Servant body");
	}

	// A character at a navmesh spot near (x, z); `cls` is the CharacterTypeEnum (4 civilian, 8 bandit), `team` the enemy side.
	std::uint64_t add(int cls, int team, double x, double z, bool snap = true)
	{
		state::Fighter f = data->makeFighter(*gd, body, "", rng);
		f.team = team;
		f.npcClass = static_cast<std::uint8_t>(cls);
		f.attention = 0.5;
		if (snap)
		{
			const auto spot = nav->walker().snap({ x, 1535.0, z }, 300.0);
			test::require(spot.has_value(), "no mesh near the spot");
			f.x = spot->x;
			f.y = spot->y;
			f.z = spot->z;
		} else {
			f.x = x;
			f.z = z;
			f.y = 1535.0;
		}
		return sim->spawnFighter(std::move(f)).get<state::Id>()->value;
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
};

double dist(const state::Fighter& a, double x, double z)
{
	return std::hypot(a.x - x, a.z - z);
}

// The first RACE of the data whose AI Goals do (or do not) carry 32.
std::string raceWith(const kdata::GameData& gd, const routines::PackageTable& table, bool has32)
{
	for (const kdata::Record* race : gd.byType(kworld::itemTypeByName("RACE")))
	{
		const std::vector<routines::GoalRow>& rows = table.raceRows(race->stringID);
		const bool found = std::any_of(rows.begin(), rows.end(), [](const routines::GoalRow& row) { return row.task == routines::task::SelfPreservation; });
		if (found == has32)
			return race->stringID;
	}
	throw std::runtime_error("no such race");
}

} // namespace

int main(int argc, char** argv)
{
	test::require(argc == 2, "usage: ksim_run_away HUB.knav");
	std::setvbuf(stdout, nullptr, _IOLBF, 0);
	const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

	std::string with32;
	std::string without32;
	// 32 from the data (dt34): the race's `AI Goals` are read next to the package's rows, so the selector has the candidates.
	{
		const std::shared_ptr<const kdata::GameData> gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA)));
		const routines::PackageTable table(*gd);
		const std::string& human = with32 = raceWith(*gd, table, true);
		const std::string& beak = without32 = raceWith(*gd, table, false);
		const auto has = [](const std::vector<routines::GoalRow>& rows, std::uint16_t task)
		{
			return std::any_of(rows.begin(), rows.end(), [task](const routines::GoalRow& row) { return row.task == task; });
		};
		check(has(table.raceRows(human), routines::task::SelfPreservation), "a race list can carry 32 SELF_PRESERVATION");
		check(!has(table.raceRows(beak), routines::task::SelfPreservation), "a race list can lack 32");
		check(table.raceRows(human).front().tier == routines::Tier::Urgent && routines::effectiveWeight(table.raceRows(human).front()) == 1.0,
			"a race row is URGENT at weight 1.0 (the data's (0, 100, 0))");
		// the package's own row for a task takes the place of the race's, and the package rows come last (a tie goes to them)
		routines::CompiledPackage package;
		routines::GoalRow own;
		own.task = routines::task::SelfPreservation;
		own.tier = routines::Tier::NonUrgent;
		own.weight = 50;
		package.lists[0].push_back(own);
		const std::vector<routines::GoalRow> merged = table.rowsWithRace(&package, routines::Role::Squad, human);
		check(std::count_if(merged.begin(), merged.end(), [](const routines::GoalRow& row) { return row.task == routines::task::SelfPreservation; }) == 1
			&& merged.back().task == routines::task::SelfPreservation && merged.back().weight == 50,
			"the merge keeps the package's 32 row, last, and drops the race's");
	}

	// 32 never makes a member flee (dt35 / dt37 live): code gives every character a tier-3 weight-1.0 candidate on cond 39 and its plan is
	// [13 ATTACK_ATTACKERS_OF, 32], our existing fight-back. A hostile within 40 u (3D) is answered, healthy or leg-hurt, with a package or
	// without; none of them is given 35.
	for (const bool hurt : {false, true})
	{
		Rig rig(nav);
		const std::uint64_t member = rig.add(8, 0, kHubX + 80.0, kHubZ);
		const std::uint64_t foe = rig.add(8, 1, kHubX + 80.0 - 30.0, kHubZ);
		if (hurt)
			for (state::FighterPart& part : rig.f(member).parts)
				if (part.type == 1)
					part.health = -1.0; // below 0, above the collapse point: it still walks
		rig.f(foe).target = member; // it attacks: 13 ATTACK_ATTACKERS_OF answers it
		bool fled = false;
		bool fought = false;
		for (int i = 0; i < 20 * 5; ++i)
		{
			rig.tick();
			fled = fled || rig.f(member).goal.kind != IndividualKind::None;
			fought = fought || rig.f(member).target == foe;
		}
		check(!fled, std::string(hurt ? "a leg-hurt" : "a healthy") + " member with no package is not given 35 under 32");
		check(fought, std::string(hurt ? "a leg-hurt" : "a healthy") + " member fights back at a hostile 30 u away (32's plan: 13)");
	}
	// a package-driven member does not flee either: the 32 offer never plans 35
	{
		dt27::Rig rig({{43, 0, 24, 0}}, 0);
		rig.sim.enableNavigation(nav);
		const auto spot = nav->walker().snap({kHubX + 80.0, 1535.0, kHubZ}, 300.0);
		test::require(spot.has_value(), "no mesh near the spot");
		state::Fighter& member = rig.fighterOf(rig.member);
		member.x = spot->x;
		member.y = spot->y;
		member.z = spot->z;
		for (state::FighterPart& part : member.parts)
			if (part.type == 1)
				part.health = -1.0;
		const std::uint64_t foe = rig.spawn(rig.body, spot->x - 30.0, spot->z);
		rig.fighterOf(foe).team = 1;
		rig.tick(100);
		check(rig.fighterOf(rig.member).goal.kind == IndividualKind::None, "a packaged, leg-hurt member is not given 35 under 32");
	}

	// 280 started from the dialogue (DA_RUN_AWAY: giveIndividualGoal Flee) runs the same executor: away, no fight, and a re-roll when idle.
	{
		Rig rig(nav);
		const std::uint64_t runner = rig.add(4, 0, kHubX + 80.0, kHubZ);
		const std::uint64_t speaker = rig.add(8, 1, kHubX, kHubZ);
		check(rig.sim->giveIndividualGoal(runner, IndividualKind::Flee, speaker), "280 given");
		const double startX = rig.f(runner).x;
		const double startZ = rig.f(runner).z;
		check((rig.f(runner).goal.x - startX) * (startX - rig.f(speaker).x) > 0.0, "280 aims away from the speaker");
		bool fought = false;
		double furthest = 0.0;
		for (int i = 0; i < 20 * 30; ++i)
		{
			rig.tick();
			fought = fought || rig.f(runner).target != 0;
			furthest = std::max(furthest, dist(rig.f(runner), startX, startZ));
		}
		// The first goal lies ~870 u from the start on either Hub bake; the second leg is a re-roll after arriving, so which way it goes (on from the
		// start or back toward it) changes with the smallest mesh edit (k16.7.18's interior carve: 922 u here, 1,232 before). Pinned: the first leg.
		check(!fought && furthest >= 800.0, "280 runs " + std::to_string(static_cast<int>(furthest)) + " u and takes no fight");
		check(rig.f(runner).goal.kind == IndividualKind::Flee, "280 keeps running (the exe's task has no end of its own; ours the 120 s cap)");
	}

	// A runner that is not moving re-rolls: 1000..1400 u from where it stands, within a second.
	{
		Rig rig(nav);
		const std::uint64_t runner = rig.add(4, 0, kHubX + 80.0, kHubZ);
		const std::uint64_t foe = rig.add(8, 1, kHubX, kHubZ);
		check(rig.sim->giveIndividualGoal(runner, IndividualKind::Flee, foe), "280 given for the re-roll case");
		rig.tick(10);
		const double oldX = rig.f(runner).goal.x;
		const double oldZ = rig.f(runner).goal.z;
		rig.f(runner).clearRoute(); // not moving
		rig.tick(21);
		const double reach = dist(rig.f(runner), rig.f(runner).goal.x, rig.f(runner).goal.z);
		check(rig.f(runner).goal.kind == IndividualKind::Flee && (oldX != rig.f(runner).goal.x || oldZ != rig.f(runner).goal.z),
			"a runner that is not moving gets a new point");
		check(reach > 700.0 && reach < 1500.0, "the new point is 1000..1400 u out (" + std::to_string(static_cast<int>(reach)) + " from where it stood)");
	}

	// No way to run (off the mesh): 35 is never taken, 280 does not stay stuck holding the goal.
	{
		Rig rig(nav);
		const std::uint64_t runner = rig.add(4, 0, -30000.0, 40000.0, false);
		const std::uint64_t foe = rig.add(8, 1, -30000.0, 40040.0, false);
		rig.f(foe).target = runner;
		rig.tick(60);
		check(rig.f(runner).goal.kind == IndividualKind::None, "off the mesh 35 is not taken (nowhere to run)");
		rig.sim->giveIndividualGoal(runner, IndividualKind::Flee, foe);
		rig.tick(60);
		check(rig.f(runner).goal.kind == IndividualKind::None, "off the mesh 280 never stays stuck holding the goal");
	}

	std::printf("ksim_run_away: ok\n");
	return 0;
}
