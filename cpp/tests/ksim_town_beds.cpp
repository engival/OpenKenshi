// The T3c layer of the town scorer (docs/reference/TOWN_LIFE_SLICE.md 5.16): beds, tiredness, the resident door routine and shop closing.
//   - residents sleep at night (asleep, in a bed, lastSleep following them) and wake when the window closes;
//   - homeless squads on one point share its yard's beds, one sleeper per bed;
//   - a shop locks after hours and reopens: the keeper's plan locks the door, then sleeps; the morning goal opens it again;
//   - the keeper's routine follows the doors (awake at day, in bed at night, trade refused while it sleeps);
//   - a stranger in the home blocks the lock, the resident shoos and, after its patience, gets on with its day (never stranded);
//   - a locked door with a resident inside never strands it, and a resident that is outside when the door is locked still has a goal;
//   - determinism and save / load mid-night (hash, doors, flags, the book).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

Simulation newGame(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessByDensity = false;
	// These tests pin the seeded placement pass (5.3), which a fresh start's residents skip (they walk in: ksim_residents_arrive).
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.setResidentsArrive(false);
	return sim;
}

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

flecs::entity squadIn(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::SquadInstance&) { found = i.value == id ? e : found; });
	test::require(found.is_valid(), "the squad exists in this sim");
	return found;
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

void centreOn(Simulation& sim, const std::vector<std::array<double, 2>>& at)
{
	std::vector<state::GroundPoint> areas;
	for (const auto& p : at)
		areas.push_back({p[0], p[1]});
	sim.enqueue(Command::setPlayerAreas(areas));
	sim.run(1);
}

state::Fighter& fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<state::Fighter&>(*sim.combat()->find(id));
}

// A home-bound resident squad whose people can all be built.
struct Pick
{
	flecs::entity squad;
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
	std::set<std::uint16_t> tasks;
	std::size_t members = 0;
	bool leader = false; // a member spawned from the template's `leader` list (it runs the Leader list: the shop, the secure bed)
	std::string behaviour;
	std::string package;
};

std::vector<Pick> residentSquads(Simulation& sim, const routines::PackageTable& packages, const CombatData& combat)
{
	const BehaviourRegistry& registry = sim.ai()->registry();
	const kdata::GameData& data = *sim.gameData();
	std::vector<Pick> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id == 0 || instance.source != state::SpawnSource::TownResident)
			return;
		const Mapping& mapping = registry.squad(instance.templateSid);
		if (mapping.behaviour != Behaviour::ShopKeeper && mapping.behaviour != Behaviour::ResidentLife)
			return;
		const routines::CompiledPackage* package = packages.find(mapping.package);
		if (package == nullptr || std::strcmp(routines::routineStatus(*package), "modelled") != 0)
			return;
		Pick pick;
		pick.squad = squad;
		pick.id = id.value;
		pick.x = at.x;
		pick.z = at.z;
		pick.behaviour = behaviourName(mapping.behaviour);
		pick.package = mapping.package;
		for (const auto& list : package->lists)
			for (const routines::GoalRow& row : list)
				pick.tasks.insert(row.task);
		found.push_back(std::move(pick));
	});
	std::sort(found.begin(), found.end(), [](const Pick& a, const Pick& b) { return a.id < b.id; });
	std::vector<Pick> usable;
	for (Pick& candidate : found)
	{
		const std::vector<flecs::entity> members = membersOf(sim, candidate.squad);
		if (members.empty() || members.size() > 6)
			continue;
		bool buildable = true;
		for (const flecs::entity member : members)
		{
			const state::MemberRole* role = member.get<state::MemberRole>();
			candidate.leader = candidate.leader || (role != nullptr && role->list == state::kRoleLeader);
			std::uint64_t dice = 1;
			try
			{
				combat.makeFighter(data, member.get<state::CharacterInstance>()->templateSid, "", dice, candidate.squad.get<state::SquadInstance>()->templateSid);
			} catch (const std::exception&) {
				buildable = false;
			}
		}
		if (buildable)
		{
			candidate.members = members.size();
			usable.push_back(std::move(candidate));
		}
	}
	return usable;
}

bool isBed(std::uint16_t goal)
{
	return goal == routines::task::GoHomeToBed || goal == routines::task::GoHomeToBedSecure;
}

// The first window-valid bed row of the member's list at `hour` (the same test the scorer uses).
bool bedWindowOpen(const routines::PackageTable& packages, Simulation& sim, flecs::entity member, double hour)
{
	const flecs::entity squad = member.target<state::MemberOf>();
	const routines::CompiledPackage* package = packages.find(sim.ai()->registry().squad(squad.get<state::SquadInstance>()->templateSid).package);
	if (package == nullptr)
		return false;
	for (const routines::Candidate& c : routines::candidatesAt(*package, sim.liveRole(member), hour))
		if (!c.fallback && isBed(c.task))
			return true;
	return false;
}

double hourOf(Simulation& sim)
{
	return std::fmod(trader::gameHours(sim.clockTick()), 24.0);
}

std::string clockText(Simulation& sim)
{
	char buffer[16];
	std::snprintf(buffer, sizeof buffer, "%05.2f", hourOf(sim));
	return buffer;
}

// ---- beds: sleep at night, wake in the morning ----

void sleepAndWake(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	sim.setClockTick(tickAt(2, 1, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	std::vector<const Pick*> chosen;
	for (const Pick& pick : picks)
		if ((pick.tasks.count(153) != 0 || pick.tasks.count(239) != 0) && chosen.size() < 8)
			chosen.push_back(&pick);
	test::require(chosen.size() >= 4, "resident squads with a bed goal (" + std::to_string(chosen.size()) + ")");
	std::vector<std::array<double, 2>> at;
	for (const Pick* pick : chosen)
		at.push_back({pick->x, pick->z});
	centreOn(sim, at);
	runUntil(sim, [&] { return std::all_of(chosen.begin(), chosen.end(), [&](const Pick* p) { return squadIn(sim, p->id).has<state::SquadRoutine>(); }); },
		4000, "the squads were never promoted");

	std::vector<flecs::entity> members;
	for (const Pick* pick : chosen)
		for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
			if (member.has<state::Routine>())
				members.push_back(member);
	test::require(!members.empty(), "promoted members");

	// 01:00, seeded placement: whoever has a bed window (and was not up within 3 h) lies in a bed.
	std::set<std::uint64_t> asleepAtStart;
	std::map<std::uint64_t, double> sleepSeen;
	for (const flecs::entity member : members)
	{
		const state::Fighter& f = fighterOf(sim, idOf(member));
		const state::Routine& r = *member.get<state::Routine>();
		if (f.asleep)
		{
			test::require(isBed(r.goal), "only a member on a bed goal is asleep");
			test::require(r.target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed), "and it holds a Bed site");
			test::require(sim.routineBook().held(idOf(member)).kind == routines::SiteKind::Bed, "which is in the book");
			test::require(std::fabs(r.lastSleep - trader::gameHours(sim.clockTick())) < 0.01, "lastSleep is now for a sleeper");
			// The pose tail: a real bed's clip is its own (sleepinbed when it names none; Camp Bed names sleeponfloor), a synthetic bed's
			// sleeponfloor; the facing is finite.
			const std::optional<Simulation::Pose> pose = sim.pose(idOf(member));
			test::require(pose.has_value() && std::isfinite(pose->yaw), "a sleeper has a pose");
			const routines::SiteEntry* bed = sim.routineSite(r.target);
			test::require(bed != nullptr, "the sleeper's bed is indexed");
			if (bed->synthetic)
				test::require(pose->clip == routines::kClipSleepOnFloor && pose->x == f.x && pose->z == f.z, "a synthetic bed's pose is the floor at the sleeper");
			else
			{
				// The body lies on the use node itself: its x/z and its own rotated y (a level bed's node sits on the floor, a tilted
				// piece routes it up or down; kinspect: the original's position is the use node exactly).
				const double nodeY = bed->hasNodeY ? bed->nodeY : bed->y;
				test::require(!pose->clip.empty() && pose->x == bed->x && pose->z == bed->z && pose->y == nodeY, "a real bed's pose is its node");
				// sleeponfloor lies along the model's X: a quarter turn more than the node's facing (client clip probe, 2026-11-04)
				const double expect = std::atan2(bed->faceX, bed->faceZ) - (pose->clip == routines::kClipSleepOnFloor ? 1.57079632679489662 : 0.0);
				test::require(std::fabs(pose->yaw - expect) < 1e-9, "a real bed's yaw: the node's -X, a quarter turn more for sleeponfloor");
			}
			asleepAtStart.insert(idOf(member));
		}
	}
	std::cout << "  01:00 placement: " << asleepAtStart.size() << " of " << members.size() << " members asleep\n";
	test::require(!asleepAtStart.empty(), "residents sleep at night (seeded placement)");

	// Run to 10:00. Invariants every 200 ticks: asleep implies a bed goal inside its window; every member has a goal; the book agrees.
	std::set<std::uint64_t> woke;
	std::map<std::uint64_t, double> lastAsleepHour;
	std::map<std::uint64_t, state::RoutineSite> sleptIn; // each sleeper's bed at the last check, while it stays asleep
	const std::uint64_t end = tickAt(2, 10, 0);
	std::uint64_t checks = 0;
	while (sim.clockTick() < end)
	{
		sim.run(200);
		const double hour = hourOf(sim);
		std::vector<std::array<double, 2>> beds; // where the sleepers lie: never two on one spot (a shared synthetic grid stacked them)
		for (const flecs::entity member : members)
		{
			const state::Routine* r = member.get<state::Routine>();
			if (r == nullptr)
				continue;
			const state::Fighter& f = fighterOf(sim, idOf(member));
			test::require(r->goal != 0, "no member is ever without a goal (" + clockText(sim) + ")");
			if (f.asleep)
			{
				test::require(isBed(r->goal), "asleep implies a bed goal (" + clockText(sim) + ")");
				test::require(bedWindowOpen(packages, sim, member, hour) || bedWindowOpen(packages, sim, member, std::fmod(hour + 23.9, 24.0)),
					"a sleeper is inside its bed window (" + clockText(sim) + ")");
				lastAsleepHour[idOf(member)] = trader::gameHours(sim.clockTick());
				// The plain and the secure bed goal may swap while it sleeps; the bed does not (a re-roll moved sleepers unwoken).
				const auto [was, first] = sleptIn.try_emplace(idOf(member), r->target);
				test::require(first || was->second == r->target, "a sleeper stays in its bed until it wakes (" + clockText(sim) + ")");
				for (const std::array<double, 2>& other : beds)
					test::require(std::hypot(f.x - other[0], f.z - other[1]) > 1.0, "no two sleepers on one spot (" + clockText(sim) + ")");
				beds.push_back({f.x, f.z});
			} else {
				sleptIn.erase(idOf(member));
			}
			if (!f.asleep && asleepAtStart.count(idOf(member)) != 0 && lastAsleepHour.count(idOf(member)) != 0)
				woke.insert(idOf(member));
			++checks;
		}
	}
	// Wake: some of the early sleepers are up by 10:00, and the ones that woke have a lastSleep at the moment they got up.
	test::require(!woke.empty(), "sleepers wake when the window closes (" + std::to_string(woke.size()) + ")");
	for (const flecs::entity member : members)
	{
		const auto seen = lastAsleepHour.find(idOf(member));
		if (seen == lastAsleepHour.end() || fighterOf(sim, idOf(member)).asleep)
			continue;
		test::require(member.get<state::Routine>()->lastSleep >= seen->second - 0.2 && member.get<state::Routine>()->lastSleep <= seen->second + 1.0,
			"after waking lastSleep stays at the last night's sleep (it is the wake cooldown's clock)");
	}
	std::cout << "sleep and wake ok (" << members.size() << " members, " << asleepAtStart.size() << " asleep at 01:00, " << woke.size() << " woke, " << checks
		<< " checks, slept " << sim.townScorerStats().slept << ")\n";
}

// ---- a shop at night ----

constexpr std::uint64_t kDoorKey = 0x7a3c000001ULL;

struct HomeRig
{
	std::vector<flecs::entity> members;
	flecs::entity door;
	std::uint64_t building = 0;
	double cx = 0.0;
	double cz = 0.0;
	std::uint64_t squad = 0;
};

// Promotes the squad and gives it a real home: the placed building nearest to it, one door six units from the centre, and the members
// standing inside (the test world has no navmesh, so nobody can walk; a walk is abandoned by the arena, exactly as the scorer expects).
HomeRig setupHome(Simulation& sim, const Pick& pick, std::uint8_t doorState)
{
	centreOn(sim, {{pick.x, pick.z}});
	runUntil(sim, [&] { return squadIn(sim, pick.id).has<state::SquadRoutine>(); }, 4000, "the squad was never promoted");
	HomeRig rig;
	rig.squad = pick.id;
	for (const flecs::entity member : membersOf(sim, squadIn(sim, pick.id)))
		if (member.has<state::Routine>())
			rig.members.push_back(member);
	test::require(!rig.members.empty(), "promoted members");
	const BuildingSite* nearest = nullptr;
	for (const BuildingSite& site : sim.world().buildingSites())
		if (nearest == nullptr || std::hypot(site.cx - pick.x, site.cz - pick.z) < std::hypot(nearest->cx - pick.x, nearest->cz - pick.z))
			nearest = &site;
	test::require(nearest != nullptr, "a placed building");
	rig.building = nearest->key;
	rig.cx = nearest->cx;
	rig.cz = nearest->cz;
	state::Door door;
	door.key = kDoorKey;
	door.building = rig.building;
	door.x = rig.cx + 6.0;
	door.z = rig.cz;
	door.state = doorState;
	rig.door = sim.spawnDoor(door);
	// Side by side across the doorway, one footprint sum (8) apart: stacked members would be pushed out into a line (the depenetration, BQ16), and with no
	// navmesh the far ones could never walk back within kDoorReach of the door. The keeper (first) stands opposite the door, the others
	// alternate outward from it: 0, +8, -8, +16, ...
	for (std::size_t i = 0; i < rig.members.size(); ++i)
	{
		state::Fighter& f = fighterOf(sim, idOf(rig.members[i]));
		f.x = rig.cx + 1.0;
		f.z = rig.cz + 8.0 * static_cast<double>((i + 1) / 2) * (i % 2 == 1 ? 1.0 : -1.0);
		f.building = rig.building;
		f.homeBuilding = rig.building;
		f.clearRoute();
	}
	return rig;
}

std::uint8_t doorState(const HomeRig& rig)
{
	return rig.door.get<state::Door>()->state;
}

std::uint8_t lockedFlag(Simulation& sim, const HomeRig& rig)
{
	return squadIn(sim, rig.squad).get<state::SquadRoutine>()->homeLocked;
}

const Pick* findPick(const std::vector<Pick>& picks, const std::string& behaviour, const std::vector<std::uint16_t>& all, const std::vector<std::uint16_t>& any)
{
	for (const Pick& p : picks)
	{
		if (p.behaviour != behaviour || !p.leader) // a leaderless squad (housemates) runs only the Squad list
			continue;
		bool ok = std::all_of(all.begin(), all.end(), [&](std::uint16_t t) { return p.tasks.count(t) != 0; });
		ok = ok && (any.empty() || std::any_of(any.begin(), any.end(), [&](std::uint16_t t) { return p.tasks.count(t) != 0; }));
		if (ok)
			return &p;
	}
	return nullptr;
}

bool anyAsleep(Simulation& sim, const HomeRig& rig)
{
	return std::any_of(rig.members.begin(), rig.members.end(), [&](const flecs::entity& m) { return fighterOf(sim, idOf(m)).asleep; });
}

// Everyone of the rig has a goal; the keeper (first member) is the squad's leader or the shop's counter holder.
void everyoneHasAGoal(const HomeRig& rig, Simulation& sim)
{
	for (const flecs::entity member : rig.members)
		test::require(member.get<state::Routine>()->goal != 0, "no member is ever without a goal (" + clockText(sim) + ")");
}

// A shop closes and reopens, from 21:00 to 08:00: lock, sleep, open, counter. Returns the stateHash at 08:00 (determinism).
std::uint64_t shopNight(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, bool verify, const std::string& savePath, std::uint64_t* hashAtSave)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	sim.setClockTick(tickAt(1, 21, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	const Pick* pick = findPick(picks, "shop_keeper", {20, 86}, {239, 153});
	test::require(pick != nullptr, "a shop squad that opens, closes and sleeps");
	const HomeRig rig = setupHome(sim, *pick, 0);
	const flecs::entity keeper = rig.members.front(); // the squad's first member by Id: the leader, who has the shop list
	const std::uint64_t keeperId = idOf(keeper);
	std::set<std::uint16_t> keeperGoals;
	bool lockedAtNight = false;
	bool asleepAtNight = false;
	bool reopened = false;
	bool tradeRefused = false;
	std::uint64_t saved = 0;
	const std::uint64_t end = tickAt(2, 8, 0);
	while (sim.clockTick() < end)
	{
		sim.run(std::min<std::uint64_t>(kTicksPerGameHour / 8, end - sim.clockTick()));
		const double hour = hourOf(sim);
		keeperGoals.insert(keeper.get<state::Routine>()->goal);
		if (!savePath.empty() && saved == 0 && sim.clockTick() >= tickAt(2, 2, 0))
		{
			sim.saveGame(savePath);
			saved = sim.stateHash();
			if (hashAtSave != nullptr)
				*hashAtSave = saved;
		}
		if (!verify)
			continue;
		everyoneHasAGoal(rig, sim);
		const state::Fighter& f = fighterOf(sim, keeperId);
		if (hour >= 21.0 && hour < 22.9)
		{
			test::require(doorState(rig) == 0 && lockedFlag(sim, rig) == 0, "the shop is open in the evening (" + clockText(sim) + ")");
			test::require(!f.asleep, "and the keeper is awake");
		}
		if (hour >= 23.6 || hour < 5.9)
		{
			test::require(doorState(rig) == 2 && lockedFlag(sim, rig) == 1, "the shop is locked at night (" + clockText(sim) + "; door " + std::to_string(doorState(rig))
				+ ", flag " + std::to_string(lockedFlag(sim, rig)) + ", keeper goal " + std::to_string(keeper.get<state::Routine>()->goal) + ", home "
				+ std::to_string(f.homeBuilding == rig.building) + ", building " + std::to_string(f.building == rig.building) + ")");
			lockedAtNight = true;
			test::require(f.asleep && isBed(keeper.get<state::Routine>()->goal), "and the keeper is asleep in a bed (" + clockText(sim) + ")");
			asleepAtNight = true;
			test::require(sim.tradeLost(idOf(rig.members.back()), keeperId) == "gone", "a sleeping keeper does not trade (" + clockText(sim) + ")");
			tradeRefused = true;
			test::require(f.houseLocked, "and the trespass sensor's houseLocked follows the lock");
		}
		if (hour >= 6.6 && hour < 7.9)
		{
			test::require(doorState(rig) == 0 && lockedFlag(sim, rig) == 0, "the shop opens again in the morning (" + clockText(sim) + ")");
			reopened = true;
			test::require(!f.asleep && keeper.get<state::Routine>()->goal == routines::task::Shopkeeper, "and the keeper is at the counter");
			test::require(!f.houseLocked, "and houseLocked is clear");
		}
	}
	if (verify)
	{
		test::require(lockedAtNight && asleepAtNight && reopened && tradeRefused, "the whole night was observed");
		test::require(keeperGoals.count(20) != 0 && keeperGoals.count(239) != 0, "the keeper ran Shopkeeper and the secure bed");
		test::require(sim.townScorerStats().doorsLocked >= 1 && sim.townScorerStats().doorsOpened >= 1, "doors were locked and opened by residents");
		test::require(rig.door.get<state::Door>()->version >= 3, "each change bumped the door version (the door stream's trigger)");
		std::cout << "shop night ok (package " << pick->package << ", " << rig.members.size() << " people, keeper goals:";
		for (const std::uint16_t g : keeperGoals)
			std::cout << " " << g;
		std::cout << ", door version " << rig.door.get<state::Door>()->version << ")\n";
	}
	return sim.stateHash();
}

// A door locked by someone else, or a flag left behind, never strands a resident: at noon the shop door is found locked and the keeper opens it;
// a stale flag with an unlocked door is dropped; a broken door is nobody's to lock.
// Homeless squads that stand on one point (a town's homeless squads on its town point) share that point's yard: their sleepers take
// distinct synthetic beds (client live Heng check 2026-11-04: per-squad yards laid the same grid, three peasants on one spot).
void sharedYard(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	sim.setClockTick(tickAt(2, 1, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	std::vector<const Pick*> chosen;
	for (const Pick& pick : picks)
		if ((pick.tasks.count(153) != 0 || pick.tasks.count(239) != 0) && chosen.size() < 3)
			chosen.push_back(&pick);
	test::require(chosen.size() == 3, "three resident squads with a bed goal");
	for (const Pick* pick : chosen)
	{
		flecs::entity squad = squadIn(sim, pick->id);
		squad.get_mut<state::SquadInstance>()->homeBuilding = 0;
		squad.get_mut<state::Position>()->x = chosen.front()->x;
		squad.get_mut<state::Position>()->z = chosen.front()->z;
	}
	centreOn(sim, { { chosen.front()->x, chosen.front()->z } });
	runUntil(sim, [&] { return std::all_of(chosen.begin(), chosen.end(), [&](const Pick* p) { return squadIn(sim, p->id).has<state::SquadRoutine>(); }); },
		4000, "the squads were never promoted");
	std::size_t most = 0;
	for (int pass = 0; pass < 20; ++pass)
	{
		sim.run(200);
		std::vector<std::array<double, 2>> beds;
		for (const Pick* pick : chosen)
		{
			for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
			{
				const state::Fighter& f = fighterOf(sim, idOf(member));
				test::require(f.homeBuilding == 0, "the members have no home building (a yard)");
				if (!f.asleep)
					continue;
				for (const std::array<double, 2>& other : beds)
					test::require(std::hypot(f.x - other[0], f.z - other[1]) > 1.0, "no two sleepers of the shared yard on one spot (" + clockText(sim) + ")");
				beds.push_back({ f.x, f.z });
			}
		}
		most = std::max(most, beds.size());
	}
	test::require(most >= 4, "the yard's squads sleep (" + std::to_string(most) + " at most at once)");
	std::cout << "shared yard ok (" << most << " sleepers at most at once, on distinct spots)\n";
}

void staleDoors(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	sim.setClockTick(tickAt(1, 12, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	const Pick* pick = findPick(picks, "shop_keeper", {20, 86}, {239}); // 239: it locks after hours
	test::require(pick != nullptr, "a shop squad that locks up");
	const HomeRig rig = setupHome(sim, *pick, 2); // the player (or a bandit) locked it at noon
	sim.run(kTicksPerGameHour / 4);
	test::require(doorState(rig) == 0, "a keeper that finds its shop locked in opening hours opens it");
	// Someone unlocks the door at night while the flag says locked: the flag is dropped, the keeper locks again from within.
	sim.setClockTick(tickAt(1, 23, 30));
	sim.run(kTicksPerGameHour / 4);
	test::require(doorState(rig) == 2 && lockedFlag(sim, rig) == 1, "locked after hours");
	rig.door.get_mut<state::Door>()->state = 1; // a returning resident (the owner) unlocked it
	sim.run(kTicksPerGameHour / 8);
	test::require(doorState(rig) == 2 && lockedFlag(sim, rig) == 1, "an unlocked door at night is locked again from within");
	// A broken door is nobody's to lock or open: the routine goes on without it.
	{
		state::Door* d = rig.door.get_mut<state::Door>();
		d->state = 3;
	}
	sim.run(kTicksPerGameHour / 2);
	test::require(doorState(rig) == 3, "a broken door stays broken");
	everyoneHasAGoal(rig, sim);
	test::require(anyAsleep(sim, rig), "and the keeper still goes to bed (no lock to plan)");
	std::cout << "stale doors ok\n";
}

// A stranger in the home: a shop (public) ignores them for the bed, but a 239 never goes to bed unlocked, so the shop stays up and open; when
// the stranger leaves the keeper locks up and sleeps. A private home shoos, then gets on with its day after its patience.
void strangers(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	{
		Simulation sim = newGame(order);
		enable(sim, combat);
		sim.setClockTick(tickAt(1, 22, 30));
		const routines::PackageTable packages(*sim.gameData());
		const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
		const Pick* pick = findPick(picks, "shop_keeper", {20, 86}, {239});
		test::require(pick != nullptr, "a shop squad with the secure bed");
		const HomeRig rig = setupHome(sim, *pick, 0);
		// A player character walks in (the arena entry of a real Fighter, a CharacterInstance's template).
		std::uint64_t dice = 7;
		state::Fighter visitor = combat->makeFighter(*sim.gameData(), rig.members.front().get<state::CharacterInstance>()->templateSid, "", dice,
			squadIn(sim, pick->id).get<state::SquadInstance>()->templateSid);
		visitor.team = 0;
		visitor.playerControlled = true;
		visitor.x = rig.cx + 2.0;
		visitor.z = rig.cz;
		visitor.building = rig.building;
		const std::uint64_t visitorId = idOf(sim.spawnFighter(std::move(visitor)));
		sim.run(2 * kTicksPerGameHour);
		test::require(doorState(rig) == 0 && lockedFlag(sim, rig) == 0, "a stranger inside keeps the shop from locking up");
		test::require(!anyAsleep(sim, rig), "and the keepers from going to bed");
		everyoneHasAGoal(rig, sim);
		fighterOf(sim, visitorId).building = 0; // the stranger leaves
		sim.run(kTicksPerGameHour / 2);
		test::require(doorState(rig) == 2 && lockedFlag(sim, rig) == 1, "the stranger gone: the keeper locks up");
		test::require(anyAsleep(sim, rig), "and goes to bed");
	}
	{
		// A private home: goal 158 / 283 appears, and rests after its patience.
		Simulation sim = newGame(order);
		enable(sim, combat);
		sim.setClockTick(tickAt(1, 19, 0));
		const routines::PackageTable packages(*sim.gameData());
		const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
		const Pick* pick = nullptr;
		for (const Pick& p : picks)
			if (p.behaviour == "resident_life" && (p.tasks.count(283) != 0 || p.tasks.count(158) != 0) && p.tasks.count(239) != 0 && p.tasks.count(155) != 0)
			{
				pick = &p;
				break;
			}
		test::require(pick != nullptr, "a private resident squad that shoos, locks and sleeps");
		const HomeRig rig = setupHome(sim, *pick, 0);
		std::uint64_t dice = 9;
		state::Fighter visitor = combat->makeFighter(*sim.gameData(), rig.members.front().get<state::CharacterInstance>()->templateSid, "", dice,
			squadIn(sim, pick->id).get<state::SquadInstance>()->templateSid);
		visitor.team = 0;
		visitor.playerControlled = true;
		visitor.x = rig.cx + 2.0;
		visitor.z = rig.cz;
		visitor.building = rig.building;
		const std::uint64_t visitorId = idOf(sim.spawnFighter(std::move(visitor)));
		sim.run(kTicksPerGameHour / 4);
		test::require(sim.townScorerStats().shoos > 0, "a private home shoos the stranger");
		bool shooing = false;
		for (const flecs::entity member : rig.members)
		{
			const std::uint16_t g = member.get<state::Routine>()->goal;
			shooing = shooing || g == routines::task::ShooStrangers || g == routines::task::ShooStrangersPrivate;
		}
		test::require(shooing, "somebody is shooing");
		// 4 game hours at night, the stranger never leaves: the residents are never stuck on it (no terminal state).
		for (int i = 0; i < 16; ++i)
		{
			sim.run(kTicksPerGameHour / 4);
			everyoneHasAGoal(rig, sim);
		}
		test::require(doorState(rig) == 0 && !anyAsleep(sim, rig), "a private home with a stranger stays unlocked and up");
		const std::uint64_t shoosBefore = sim.townScorerStats().shoos;
		fighterOf(sim, visitorId).building = 0;
		sim.run(kTicksPerGameHour);
		test::require(doorState(rig) == 2 && lockedFlag(sim, rig) == 1 && anyAsleep(sim, rig), "the stranger gone: the home locks up and sleeps");
		test::require(sim.townScorerStats().shoos == shoosBefore, "and nobody shoos any more");
	}
	std::cout << "strangers ok\n";
}

// Determinism and save / load mid-night.
void determinism(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_town_beds_test.ksav";
	std::uint64_t midnight = 0;
	const std::uint64_t first = shopNight(order, combat, false, path, &midnight);
	const std::uint64_t second = shopNight(order, combat, false, "", nullptr);
	test::require(first == second, "the same seed gives the same hash after a night of locking and sleeping");

	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == midnight, "load gives the hash saved at 02:00");
	enable(loaded, combat);
	test::require(loaded.stateHash() == midnight, "enabling AI and combat does not change it");
	std::size_t asleep = 0;
	ecsOf(loaded).each([&](const state::Id& id, const state::Routine&)
	{
		const state::Fighter* f = loaded.combat()->find(id.value);
		asleep += f != nullptr && f->asleep ? 1 : 0;
	});
	test::require(asleep > 0, "sleepers are asleep after the load");
	std::size_t locked = 0;
	ecsOf(loaded).each([&](const state::Door& d) { locked += d.state == 2 ? 1 : 0; });
	test::require(locked == 1, "and the shop door is still locked");
	loaded.run(tickAt(2, 8, 0) - loaded.clockTick());
	test::require(loaded.stateHash() == first, "load mid-night and continue gives the uninterrupted run's hash at 08:00");
	std::remove(path.c_str());
	std::cout << "determinism and save / load ok (" << asleep << " asleep and a locked door at the save)\n";
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		sleepAndWake(order, combat);
		sharedYard(order, combat);
		shopNight(order, combat, true, "", nullptr);
		staleDoors(order, combat);
		strangers(order, combat);
		determinism(order, combat);
		std::cout << "ksim_town_beds: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_beds: FAILED: " << e.what() << "\n";
		return 1;
	}
}
