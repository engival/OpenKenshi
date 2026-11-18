// Guard shifts (docs/reference/TOWN_LIFE_SLICE.md 5.8, 5.18, phase T3e1):
//   - `Guard town` squads run the guard_shift behaviour; the two halves flip at the window hour (the Squad list on post 157 at
//     night and in bed by day, the Squad2 list the other way round), and only the off-shift half goes to bed;
//   - at most the available beds fill: a squad whose off-shift half outnumbers its home's beds leaves the surplus up with a goal;
//   - the night-shift addon's bed windows reach the roles of `Guard building, residents`;
//   - intruder pickup (223) is adopted by the scorer only when its precondition holds (a KO'd non-ally in the home building), and
//     the existing executor stepIntruders carries the body out through the door (the TAKE_INTRUDER_OUTSIDE phase, state::TakeOut);
//   - after the take-out the goal is released (no member left on 223 with nothing to pick up).
// The intruder is set up by hand (spawned into the building, KO'd); no NPC is ordered.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/crime.hpp"
#include "ksim/interact.hpp"
#include "ksim/navigator.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"
#include "ksim/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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
#ifndef KSIM_NAVWORLD_DIR
#error KSIM_NAVWORLD_DIR must be defined
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

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav = nullptr)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	if (nav != nullptr)
	{
		sim.enableNavigation(nav);
		spawnMeshDoors(sim, *nav);
	}
}

// A squad is named by its template and its home point: the squad ids move whenever a town's population changes (kenshi-k16.6.33 took the
// Hub's second Bar blackmarket and Thieves Guild out), the template and the home do not. The lowest id when several match.
struct Squad
{
	const char* name;
	double x;
	double z;
};
constexpr Squad kShekHq{"shek HQ", -88801.0, 15248.0};
constexpr Squad kThieves{"Thieves Guild", -51773.0, 4846.0};
constexpr Squad kHeft{"Slaver Shop", 68335.0, -71263.0};


flecs::entity squadIn(Simulation& sim, const Squad& want)
{
	flecs::entity found;
	std::uint64_t foundId = 0;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::SquadInstance& squad)
	{
		const kdata::Record* record = sim.gameData()->find(squad.templateSid);
		if (record != nullptr && record->name == want.name && std::hypot(squad.homeX - want.x, squad.homeZ - want.z) < 1.0 && (foundId == 0 || i.value < foundId))
		{
			found = e;
			foundId = i.value;
		}
	});
	test::require(found.is_valid(), "the squad exists in this sim");
	return found;
}

flecs::entity squadById(Simulation& sim, std::uint64_t id)
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

int roleOf(flecs::entity member)
{
	const state::MemberRole* role = member.get<state::MemberRole>();
	return role != nullptr ? static_cast<int>(role->list) : -1;
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

void centreOn(Simulation& sim, double x, double z)
{
	std::vector<state::GroundPoint> areas{{x, z}};
	sim.enqueue(Command::setPlayerAreas(areas));
	sim.run(1);
}

const state::Fighter& fighterOf(Simulation& sim, std::uint64_t id)
{
	return *sim.combat()->find(id);
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

// The number of beds the site index gives a real home: the layout's Bed pieces, or the synthetic grid when the building has no
// layout (kSyntheticBeds: one per guard-node count, 8).
std::size_t homeBeds(Simulation& sim, std::uint64_t key)
{
	const BuildingSite* building = sim.world().buildingSite(key);
	test::require(building != nullptr, "the home is a placed building");
	std::string layoutName = building->interiorLayout;
	if (layoutName.empty() && !building->squadTemplate.empty())
		if (const kdata::Record* record = sim.gameData()->find(building->squadTemplate))
			layoutName = record->stringOr("layout interior", "");
	const routines::LayoutLibrary* layouts = sim.interiorLayouts();
	const routines::Layout* layout = layouts != nullptr && !layoutName.empty() ? layouts->find(building->buildingSid + "-" + layoutName) : nullptr;
	if (layout == nullptr)
		return 8; // the synthetic guard-node count (routines::kBuildingGuardNodes; synthetic beds are on for a building with no layout)
	return routines::bedPieces(*sim.gameData(), *layout).size();
}

// Every bed a member holds is a distinct one, and it is a bed of its home.
void checkBedsHeld(Simulation& sim, const std::vector<flecs::entity>& members, std::uint64_t home, const std::string& when)
{
	std::set<routines::Site> beds;
	for (const flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		if (routine == nullptr || !fighterOf(sim, idOf(member)).asleep)
			continue;
		test::require(routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed), "a sleeper holds a Bed site (" + when + ")");
		test::require(routine->target.object == home, "a sleeper's bed is its home's (" + when + ")");
		const routines::Site site{static_cast<routines::SiteKind>(routine->target.kind), routine->target.object, routine->target.slot};
		test::require(beds.insert(site).second, "two sleepers share one bed (" + when + ")");
	}
}

// ---- the halves and the beds ----

void halves(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	sim.setClockTick(tickAt(2, 1, 0));
	// A Guard town squad of the Manhunter barracks whose Squad half outnumbers its Hideout's beds (the member counts are rolled, so which
	// squad that is moves with the seed and with every change to the town populate): the lowest id that does.
	std::map<std::uint64_t, flecs::entity> candidates; // ecs.each is not nested: the members are read after the walk
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::SquadInstance& candidate)
	{
		const kdata::Record* record = sim.gameData()->find(candidate.templateSid);
		if (record != nullptr && record->name == "manhunter barracks")
			candidates.emplace(id.value, e);
	});
	flecs::entity squad;
	std::uint64_t squadId = 0;
	for (const auto& [id, e] : candidates)
	{
		std::size_t first = 0;
		std::size_t second = 0;
		for (const flecs::entity member : membersOf(sim, e))
		{
			first += roleOf(member) == static_cast<int>(routines::Role::Squad) ? 1 : 0;
			second += roleOf(member) == static_cast<int>(routines::Role::Squad2) ? 1 : 0;
		}
		if (first > homeBeds(sim, e.get<state::SquadInstance>()->homeBuilding) && second > 0)
		{
			squad = e;
			squadId = id;
			break;
		}
	}
	test::require(squad.is_valid(), "a Guard town squad with more off-shift guards than beds exists");
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
	test::require(mapping.packageName == "Guard town" && mapping.behaviour == Behaviour::GuardShift,
		"Guard town runs guard_shift (" + mapping.packageName + " -> " + std::string(behaviourName(mapping.behaviour)) + ")");
	const std::uint64_t home = instance.homeBuilding;
	const std::size_t beds = homeBeds(sim, home);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	std::size_t squadHalf = 0;
	std::size_t squad2Half = 0;
	for (const flecs::entity member : members)
	{
		squadHalf += roleOf(member) == static_cast<int>(routines::Role::Squad) ? 1 : 0;
		squad2Half += roleOf(member) == static_cast<int>(routines::Role::Squad2) ? 1 : 0;
	}
	test::require(squadHalf > beds && squad2Half > 0, "the fixture has more off-shift guards than beds ("
		+ std::to_string(squadHalf) + " Squad, " + std::to_string(beds) + " beds, " + std::to_string(squad2Half) + " Squad2)");

	const state::Position at = *squad.get<state::Position>();
	centreOn(sim, at.x, at.z);
	runUntil(sim, [&] { return squadById(sim, squadId).has<state::SquadRoutine>(); }, 4000, "the squad was never promoted");

	// 01:00: the Squad half is on the bed window (23-11), the Squad2 half is on the post window (23-11). Only a bed's worth of the
	// Squad half sleeps; the others have a goal (the fallback), never nothing.
	std::size_t asleepSquad = 0;
	std::size_t asleepSquad2 = 0;
	for (const flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		test::require(routine != nullptr, "every promoted guard has a Routine");
		test::require(routine->goal != 0, "no guard is without a goal at 01:00");
		if (fighterOf(sim, idOf(member)).asleep)
			(roleOf(member) == static_cast<int>(routines::Role::Squad2) ? asleepSquad2 : asleepSquad) += 1;
	}
	std::printf("  01:00: %zu Squad and %zu Squad2 asleep, %zu beds\n", asleepSquad, asleepSquad2, beds);
	test::require(asleepSquad2 == 0, "the Squad2 half is not in bed at 01:00 (its bed window is 12-22)");
	test::require(asleepSquad == beds, "the off-shift half fills every available bed, and no more (" + std::to_string(asleepSquad) + ")");
	test::require(asleepSquad < squadHalf, "the surplus off-shift guards stay up, not stacked on a bed");
	checkBedsHeld(sim, members, home, "01:00");

	// 14:00: the halves have flipped. The Squad2 half is off shift and sleeps; the Squad half is on post/patrol and is up.
	const std::uint64_t end = tickAt(2, 14, 0);
	std::size_t checks = 0;
	while (sim.clockTick() < end)
	{
		sim.run(200);
		for (const flecs::entity member : members)
		{
			const state::Routine* routine = member.get<state::Routine>();
			test::require(routine != nullptr && routine->goal != 0, "no guard is ever without a goal (" + clockText(sim) + ")");
			if (fighterOf(sim, idOf(member)).asleep)
				test::require(routine->goal == routines::task::GoHomeToBed || routine->goal == routines::task::GoHomeToBedSecure,
					"asleep implies a bed goal (" + clockText(sim) + ")");
		}
		checks += 1;
	}
	test::require(checks > 10, "the run covered the flip");
	asleepSquad = 0;
	asleepSquad2 = 0;
	for (const flecs::entity member : members)
	{
		if (!fighterOf(sim, idOf(member)).asleep)
			continue;
		(roleOf(member) == static_cast<int>(routines::Role::Squad2) ? asleepSquad2 : asleepSquad) += 1;
	}
	std::printf("  14:00: %zu Squad and %zu Squad2 asleep\n", asleepSquad, asleepSquad2);
	test::require(asleepSquad == 0, "the Squad half is up at 14:00 (its post window is 12-22)");
	test::require(asleepSquad2 == beds, "the Squad2 half fills the beds by day (" + std::to_string(asleepSquad2) + ")");
	checkBedsHeld(sim, members, home, "14:00");

	// Task CH group 3 (dt24_38 section 2): every member the scorer put on post 157 stands at one of its TOWN's outside nodes (the
	// node-database pick, the free node nearest the member), never a home guard node. This fixture's squad is an extra guard squad on
	// top of the Hub's residents, whose guards hold the town's 12 nodes; the extras' finder fails and they fall to patrol (the
	// documented rule), so the post is seen on the Hub's own guards.
	std::size_t posted = 0;
	ecsOf(sim).each([&](flecs::entity, const state::Routine& routine)
	{
		if (routine.goal != routines::task::StaticGuardTown)
			return;
		test::require(routine.target.kind == static_cast<std::uint8_t>(routines::SiteKind::TownNode),
			"a post 157 guard stands at a town outside node");
		++posted;
	});
	test::require(posted > 0, "the Hub's guards stand at the town's outside nodes (" + std::to_string(posted) + ")");
	std::printf("  14:00: %zu guards on post 157\n", posted);
	std::cout << "halves ok (" << squadHalf << " Squad / " << squad2Half << " Squad2, " << beds << " beds)\n";
}

// ---- the night-shift addon ----

void nightShiftAddon(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	const routines::PackageTable packages(*sim.gameData());
	flecs::entity squad = squadIn(sim, kShekHq); // Guard building, residents: the addon merged in at T3a
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
	test::require(mapping.behaviour == Behaviour::GuardShift, "Guard building, residents runs guard_shift");
	const routines::CompiledPackage* package = packages.find(mapping.package);
	test::require(package != nullptr, "the package compiled");
	const auto bedWindow = [&](routines::Role role, double hour)
	{
		for (const routines::Candidate& candidate : routines::candidatesAt(*package, role, hour))
			if (!candidate.fallback && (candidate.task == routines::task::GoHomeToBed || candidate.task == routines::task::GoHomeToBedSecure))
				return true;
		return false;
	};
	test::require(bedWindow(routines::Role::Leader, 2.0) && !bedWindow(routines::Role::Leader, 14.0), "the addon's leader bed is 23-8");
	test::require(bedWindow(routines::Role::Squad, 2.0) && !bedWindow(routines::Role::Squad, 14.0), "the addon's Squad bed is 22-10");
	test::require(!bedWindow(routines::Role::Squad2, 2.0) && bedWindow(routines::Role::Squad2, 14.0), "the addon's Squad2 bed is 10-22");

	// In the sim: at 01:00 nobody of the Squad2 half is asleep (its bed window is closed), and every guard has a goal.
	sim.setClockTick(tickAt(2, 1, 0));
	const state::Position at = *squad.get<state::Position>();
	centreOn(sim, at.x, at.z);
	runUntil(sim, [&] { return squadIn(sim, kShekHq).has<state::SquadRoutine>(); }, 4000, "Guard building, residents was never promoted");
	std::size_t asleep = 0;
	for (const flecs::entity member : membersOf(sim, squadIn(sim, kShekHq)))
	{
		const state::Routine* routine = member.get<state::Routine>();
		test::require(routine != nullptr && routine->goal != 0, "every guard has a goal at 01:00");
		if (fighterOf(sim, idOf(member)).asleep)
		{
			++asleep;
			test::require(roleOf(member) != static_cast<int>(routines::Role::Squad2), "a Squad2 member sleeps outside its 10-22 window");
		}
	}
	std::cout << "night-shift addon ok (" << asleep << " asleep at 01:00)\n";
}

// ---- intruder pickup: the scorer decides when, stepIntruders executes ----

void intruderPickup(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav)
{
	Simulation sim = newGame(order);
	enable(sim, combat, nav);
	sim.setClockTick(tickAt(2, 14, 0));
	flecs::entity squad = squadIn(sim, kThieves); // Guard building 24hr service w/patrol: a leader with 223, a Hub building with a door
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
	test::require(mapping.behaviour == Behaviour::GuardShift, "the Hub's 24hr guard runs guard_shift");
	test::require(mapping.packageName == "Guard building 24hr service w/patrol", "fixture package (" + mapping.packageName + ")");
	const std::uint64_t home = instance.homeBuilding;
	const state::Position at = *squad.get<state::Position>();
	centreOn(sim, at.x, at.z);
	runUntil(sim, [&] { return squadIn(sim, kThieves).has<state::SquadRoutine>(); }, 4000, "the guard squad was never promoted");
	const std::vector<flecs::entity> members = membersOf(sim, squadIn(sim, kThieves));
	std::uint64_t leader = 0;
	for (const flecs::entity member : members)
		if (roleOf(member) == static_cast<int>(routines::Role::Leader))
			leader = idOf(member);
	test::require(leader != 0, "the squad has a leader");

	// The home's door (the mesh's doors are spawned above); the way in is 13 u past it, the body lies there.
	std::optional<state::Door> door;
	flecs::entity doorEntity;
	ecsOf(sim).each([&](flecs::entity e, const state::Door& d)
	{
		if (d.building == home && !door)
		{
			door = d;
			doorEntity = e;
		}
	});
	test::require(door.has_value(), "the guard's home has a door");
	std::string guardFactionSid;
	for (const flecs::entity member : members)
		if (const state::Allegiance* faction = member.get<state::Allegiance>(); faction != nullptr)
			guardFactionSid = faction->factionSid;
	test::require(!guardFactionSid.empty() && door->ownerSid == guardFactionSid, "the guard's faction owns the home's door");
	const auto spot = [&](double along) -> std::optional<knav::Position>
	{
		const double x = door->x + door->inX * along;
		const double z = door->z + door->inZ * along;
		const auto snapped = nav->walker().snap({x, door->y, z}, 40.0);
		if (!snapped || std::hypot(snapped->x - x, snapped->z - z) > 4.0 || std::fabs(snapped->y - door->y) > 6.0)
			return std::nullopt;
		return snapped;
	};
	const std::optional<knav::Position> inside = spot(13.0);
	test::require(inside.has_value() && sim.buildingAt(inside->x, inside->y, inside->z) == home, "a place inside the home for the intruder");

	// Before any intruder exists, nobody wants to pick one up (the scorer's precondition).
	for (const flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		test::require(routine == nullptr || routine->goal != routines::task::PickupIntruders, "223 is not planned with no intruder in the home");
	}

	// A KO'd non-ally inside: a fighter of the leader's, set down as an enemy of an unknown faction (never an ally).
	state::Fighter body = fighterOf(sim, leader);
	body.team = 1;
	body.x = inside->x;
	body.y = inside->y;
	body.z = inside->z;
	body.building = home;
	body.unconscious = true;
	body.koTimer = 100000.0;
	body.target = 0;
	body.carrying = 0;
	body.carriedBy = 0;
	body.asleep = false;
	// Task CH: PICKUP_INTRUDERS 223 reads the exe's sensor record, whose ally bit comes from the Arena faction Id (sensor::ally),
	// not the Allegiance SID. This body is a copy of the leader's, so it would carry the leader's faction Id (same faction = ally, a
	// fixture artifact); a real stranger of an unknown faction has no faction here.
	body.faction = 0;
	flecs::entity bodyEntity = sim.spawnFighter(std::move(body));
	bodyEntity.set<state::Allegiance>(state::Allegiance{"test-intruder"});
	const std::uint64_t intruder = idOf(bodyEntity);

	bool sawGoal = false;
	bool sawTakeOut = false;
	bool carried = false;
	bool outsideWithIt = false;
	std::set<std::uint64_t> holders;
	int ticks = 0;
	bool done = false;
	for (; ticks < 4000 && !done; ++ticks)
	{
		sim.run(1);
		flecs::entity leaderEntity;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& i) { if (i.value == leader) leaderEntity = e; });
		if (const state::Routine* routine = leaderEntity.get<state::Routine>(); routine != nullptr && routine->goal == routines::task::PickupIntruders)
		{
			sawGoal = true;
			holders.insert(leader);
		}
		for (const flecs::entity member : members)
		{
			const state::Routine* routine = member.get<state::Routine>();
			if (routine != nullptr && routine->goal == routines::task::PickupIntruders && idOf(member) != leader)
				holders.insert(idOf(member));
		}
		const state::TakeOut* take = leaderEntity.get<state::TakeOut>();
		sawTakeOut = sawTakeOut || take != nullptr;
		carried = carried || fighterOf(sim, intruder).carriedBy == leader;
		outsideWithIt = outsideWithIt || (fighterOf(sim, intruder).carriedBy == leader && fighterOf(sim, leader).building == 0);
		done = carried && leaderEntity.get<state::TakeOut>() == nullptr && fighterOf(sim, intruder).carriedBy == 0;
	}
	test::require(sawGoal, "the leader never adopted PICKUP_INTRUDERS 223");
	test::require(sawTakeOut, "stepIntruders (the executor) never started a take-out");
	test::require(carried && outsideWithIt, "the intruder was never carried out of the building");
	test::require(ticks < 4000, "the take-out did not finish in 200 game-s");
	test::require(holders == std::set<std::uint64_t>{leader}, "only the leader (the list with 223) went for the intruder");
	const state::Fighter& lying = fighterOf(sim, intruder);
	std::printf("  take-out done in %.1f game-s, the intruder %.0f u from the door\n", ticks * kTickSeconds,
		std::hypot(lying.x - door->x, lying.z - door->z));
	test::require(lying.building == 0 && lying.carriedBy == 0 && lying.unconscious, "the intruder lies outside, uncarried, still KO'd");
	test::require(std::hypot(lying.x - door->x, lying.z - door->z) > 30.0, "the intruder was carried out past the door");
	test::require(doorEntity.get<state::Door>()->state == 0, "the take-out left the door open");

	// The precondition is gone: the leader rescores off 223 and nobody is left carrying or standing on it.
	sim.run(100);
	test::require(fighterOf(sim, leader).carrying == 0, "the leader still carries someone");
	for (const flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		test::require(routine == nullptr || routine->goal != routines::task::PickupIntruders, "223 is held with no intruder in the home");
	}
	std::cout << "intruder pickup ok\n";
}

// ---- the player as the intruder: PICKUP_INTRUDERS 223 -> TAKE_INTRUDER_OUTSIDE 224 on a knocked-out player (task CH) ----
// The player is knocked out inside a resident's building (no order to any NPC): the resident's package carries 223, the exe's resolver
// 14099c110 picks the body, the 223 plan lifts it (68 LIFT_PERSON) and 224 walks it out of the building.
void intruderPlayerEvict(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav)
{
	Simulation sim = newGame(order);
	enable(sim, combat, nav);
	// The player character (a new-game start-off, as ksim_after_fight's tester).
	std::string startOff;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		if (record->name == "Wanderer")
			startOff = record->stringID;
	test::require(!startOff.empty(), "the data has the Wanderer new-game start-off");
	sim.enqueue(Command::newGame(startOff, "Tester", 1));
	sim.run(3);
	sim.setClockTick(tickAt(2, 14, 0));
	flecs::entity squad = squadIn(sim, kThieves); // the Hub's 24hr guard (the 223 fixture's squad), homed in a building with a door
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const std::uint64_t home = instance.homeBuilding;
	const state::Position at = *squad.get<state::Position>();
	centreOn(sim, at.x, at.z);
	runUntil(sim, [&] { return squadIn(sim, kThieves).has<state::SquadRoutine>(); }, 4000, "the guard squad was never promoted");

	std::uint64_t player = 0;
	for (const CombatArena::Member& member : sim.combat()->members())
		if (member.fighter.playerControlled)
			player = member.id;
	test::require(player != 0, "the new game has a player character");

	std::optional<state::Door> door;
	flecs::entity doorEntity;
	ecsOf(sim).each([&](flecs::entity e, const state::Door& d)
	{
		if (d.building == home && !door)
		{
			door = d;
			doorEntity = e;
		}
	});
	test::require(door.has_value(), "the guard's home has a door");
	// The resident stands just OUTSIDE its home when the intruder is there (12 u out along the door's outward normal). The exe's
	// resolver 14099c110 reads the sensor list and MY HOME building, so the pickup starts from outside; the older executor required
	// the resident to be inside the building, so this half is what the ported path carries out (it is the red-before case).
	std::uint64_t resident = 0;
	for (const flecs::entity member : membersOf(sim, squadIn(sim, kThieves)))
		if (roleOf(member) == static_cast<int>(routines::Role::Leader))
			resident = idOf(member);
	test::require(resident != 0, "the guard squad has a leader");
	{
		// Outside the building's footprint (the interior hull may reach well past the door, the Hub Bar's does): step out along the
		// door's outward normal until the point is no building, as the take-out's drop search does.
		std::optional<knav::Position> outside;
		for (double along = 4.0; along <= 96.0; along += 8.0)
		{
			const knav::Position probe{door->x - door->inX * along, door->y, door->z - door->inZ * along};
			if (sim.buildingAt(probe.x, probe.y, probe.z) == 0)
			{
				outside = nav->walker().snap(probe, 40.0);
				if (outside && sim.buildingAt(outside->x, outside->y, outside->z) == 0)
					break;
				outside.reset();
			}
		}
		test::require(outside.has_value(), "a spot outside the guard's home");
		state::Fighter& leader = const_cast<state::Fighter&>(fighterOf(sim, resident));
		leader.clearRoute();
		leader.followTarget = 0;
		leader.x = outside->x;
		leader.y = outside->y;
		leader.z = outside->z;
		leader.building = 0;
		sim.placeInBuildings();
		test::require(leader.building == 0, "the leader stands outside its home");
	}
	// The way in is 13 u past the door (the 223 fixture's spot): a place inside the resident's building.
	const std::optional<knav::Position> snapped = nav->walker().snap({door->x + door->inX * 13.0, door->y, door->z + door->inZ * 13.0}, 40.0);
	test::require(snapped && sim.buildingAt(snapped->x, snapped->y, snapped->z) == home, "a place inside the guard's home");
	// The player is knocked out there, put down inside (the setup the slice allows; no NPC is ordered). combat() is const: the fixture
	// writes the arena's fighter as the other fixtures do.
	state::Fighter& me = const_cast<state::Fighter&>(fighterOf(sim, player));
	me.clearRoute();
	me.followTarget = 0;
	me.target = 0;
	me.carrying = 0;
	me.carriedBy = 0;
	me.x = snapped->x;
	me.y = snapped->y;
	me.z = snapped->z;
	me.building = home;
	me.unconscious = true;
	me.koTimer = 100000.0;
	me.asleep = false;
	sim.placeInBuildings();
	test::require(me.building == home, "the player is inside the guard's building");

	bool carried = false;
	bool outsideWithIt = false;
	bool sawLiftStep = false;
	std::set<std::uint64_t> holders;
	int ticks = 0;
	for (; ticks < 4000; ++ticks)
	{
		sim.run(1);
		const CombatArena::Member& body = *std::find_if(sim.combat()->members().begin(), sim.combat()->members().end(),
			[&](const CombatArena::Member& member) { return member.id == player; });
		// The ported path: the take-out runs the 223 goal's G4 chain (67 Running -> 68 LIFT_PERSON; the goal node is popped). The old
		// executor had no plan at all.
		for (const flecs::entity member : membersOf(sim, squadIn(sim, kThieves)))
			if (const state::TakeOut* take = member.get<state::TakeOut>(); take != nullptr)
				sawLiftStep = sawLiftStep || std::find(take->plan.begin(), take->plan.end(), std::uint16_t{68}) != take->plan.end();
		if (body.fighter.carriedBy != 0)
		{
			carried = true;
			holders.insert(body.fighter.carriedBy);
			outsideWithIt = outsideWithIt || fighterOf(sim, body.fighter.carriedBy).building == 0;
		}
		if (carried && body.fighter.carriedBy == 0)
			break; // put down outside: the take-out is over
	}
	test::require(carried, "no resident ever lifted the knocked-out player");
	test::require(outsideWithIt, "the player was never carried out of the building");
	test::require(ticks < 4000, "the player's take-out did not finish in 200 game-s");
	test::require(holders.size() == 1, "more than one resident carried the player");
	test::require(sawLiftStep, "the 223 goal never ran the planned 68 LIFT_PERSON step (the hand-written path had no plan)");
	const state::Fighter& lying = fighterOf(sim, player);
	std::printf("player evicted in %.1f game-s, %.0f u from the door, carrier %llu\n", ticks * kTickSeconds,
		std::hypot(lying.x - door->x, lying.z - door->z), static_cast<unsigned long long>(*holders.begin()));
	test::require(lying.building == 0 && lying.carriedBy == 0 && lying.unconscious, "the player lies outside, uncarried, still KO'd");
	test::require(std::hypot(lying.x - door->x, lying.z - door->z) > 30.0, "the player was carried out past the door");
	std::cout << "player intruder eviction ok\n";
}

// ---- the cage warden: catching chooses, the chain chains, the cage holds (T3e2) ----

void cageWarden(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav)
{
	Simulation sim = newGame(order);
	enable(sim, combat, nav);
	sim.setClockTick(tickAt(2, 14, 0));
	flecs::entity squad = squadIn(sim, kHeft); // Cage Warden in Heft, homed in a Slave Shop (4028-TwoStorey.mod) with cages in its layout
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
	test::require(mapping.packageName == "Cage Warden" && mapping.behaviour == Behaviour::CageWarden,
		"the Cage Warden runs cage_warden (" + mapping.packageName + " -> " + std::string(behaviourName(mapping.behaviour)) + ")");
	const std::uint64_t home = instance.homeBuilding;
	const state::Position at = *squad.get<state::Position>();
	centreOn(sim, at.x, at.z);
	runUntil(sim, [&] { return squadIn(sim, kHeft).has<state::SquadRoutine>(); }, 4000, "the Cage Warden was never promoted");
	const std::vector<flecs::entity> members = membersOf(sim, squadIn(sim, kHeft));
	const state::Fighter template_ = fighterOf(sim, idOf(members.front())); // a copy: the arena grows as bodies are added
	std::string wardenFaction;
	for (const flecs::entity member : members)
		if (const state::Allegiance* faction = member.get<state::Allegiance>(); faction != nullptr && !faction->factionSid.empty())
			wardenFaction = faction->factionSid;

	// Four KO'd bodies on the shop floor, only the last of them catchable: an ally of the wardens' own FACTION (the bond the capture
	// resolver reads, task BR2), a law-class character, an already-enslaved one, and the only spare body. No NPC is ordered: they are
	// placed and KO'd (the setup the slice allows).
	const auto floor = [&](double dx)
	{
		const BuildingSite* building = sim.world().buildingSite(home);
		const auto snapped = nav->walker().snap({building->cx + dx, building->originY, building->cz}, 100.0);
		test::require(snapped.has_value() && sim.buildingAt(snapped->x, snapped->y, snapped->z) == home, "a place on the shop floor");
		return *snapped;
	};
	const auto body = [&](double dx, std::uint8_t npcClass, std::uint8_t slaveState, std::int32_t team, const std::string& factionSid,
		std::uint64_t faction)
	{
		const knav::Position spot = floor(dx);
		state::Fighter f = template_;
		f.team = team;
		f.faction = faction;
		f.npcClass = npcClass;
		f.slaveState = slaveState;
		f.captorSid.clear();
		f.cageBuilding = 0;
		f.cageSlot = 0;
		f.x = spot.x;
		f.y = spot.y;
		f.z = spot.z;
		f.building = home;
		f.unconscious = true;
		f.down = false;
		f.koTimer = 1.0e6;
		f.target = 0;
		f.carrying = 0;
		f.carriedBy = 0;
		f.asleep = false;
		f.clearRoute();
		flecs::entity e = sim.spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{factionSid});
		return idOf(e);
	};
	const std::uint64_t ally = body(-12.0, 0, 0, 0, wardenFaction, template_.faction); // the wardens' own faction: an ally
	const std::uint64_t law = body(-6.0, crime::kClassLaw, 0, 1, "test-law", 0);
	const std::uint64_t owned = body(0.0, 0, 1, 1, "test-owner", 0); // already a slave (state 1)
	const std::uint64_t catchable = body(12.0, 0, 0, 1, "test-caught", 0);

	std::uint64_t picked = 0;
	bool carriedByAnyone = false;
	std::uint64_t carrier = 0;
	bool caged = false;
	// The put-in's own line names the warden (task CB): the caging plan (168 lift, 108 put-in) can do both in one tick when the body
	// lies within the cage's reach, and no tick then shows carriedBy set.
	std::vector<std::string> taskLog;
	sim.setTaskLog([&](const std::string& line) { taskLog.push_back(line); });
	int ticks = 0;
	for (; ticks < 8000 && !caged; ++ticks)
	{
		sim.run(1);
		for (const flecs::entity member : members)
		{
			const state::Routine* routine = member.get<state::Routine>();
			if (routine != nullptr && routine->goal == routines::task::CaptureSlaves && routine->chainTarget != 0)
			{
				picked = routine->chainTarget;
				test::require(picked == catchable, "catching picked " + std::to_string(picked) + ", not the nearest downed non-ally non-slave"
					" (" + std::to_string(catchable) + ")");
			}
		}
		if (const state::Fighter& victim = fighterOf(sim, catchable); victim.carriedBy != 0 && !carriedByAnyone)
		{
			carriedByAnyone = true;
			carrier = victim.carriedBy;
		}
		caged = fighterOf(sim, catchable).cageBuilding == home;
	}
	if (!carriedByAnyone)
	{
		const std::string marked = "put-in member ";
		for (const std::string& line : taskLog)
		{
			const std::size_t at = line.find(marked);
			const std::size_t bodyAt = at == std::string::npos ? std::string::npos : line.find(" body ", at);
			if (bodyAt == std::string::npos || std::stoull(line.substr(bodyAt + 6)) != catchable)
				continue;
			carriedByAnyone = true;
			carrier = std::stoull(line.substr(at + marked.size()));
			break;
		}
	}
	test::require(picked == catchable, "the warden never adopted CAPTURE_NEW_SLAVES 167 for the catchable body");
	test::require(carriedByAnyone && carrier != 0, "the catchable body was never lifted by a warden");
	test::require(caged, "the catchable body never reached a cage of the home");
	const state::Fighter& slave = fighterOf(sim, catchable);
	test::require(slave.slaveState == 1, "the caught body is not a slave (state 1)");
	test::require(!slave.captorSid.empty(), "the caught body has no captor faction");
	test::require(slave.carriedBy == 0 && fighterOf(sim, carrier).carrying == 0, "the warden still holds the caught body");
	test::require(sim.buildingAt(slave.x, slave.y, slave.z) == home, "the caught body does not stand inside the shop");
	for (const std::uint64_t other : {ally, law, owned})
	{
		const state::Fighter& body = fighterOf(sim, other);
		test::require(body.slaveState == (other == owned ? 1 : 0) && body.cageBuilding == 0 && body.carriedBy == 0,
			"a body that must never be caught was (id " + std::to_string(other) + ")");
	}
	std::printf("  caught in %.1f game-s, caged in the home (slot %u)\n", ticks * kTickSeconds, static_cast<unsigned>(slave.cageSlot));

	// The evening cage lock (195): inside the shop, at the Leader's 22-24 window, the cages are locked once for the squad.
	sim.setClockTick(tickAt(2, 22, 30));
	runUntil(sim, [&] { return squadIn(sim, kHeft).get<state::SquadRoutine>()->cagesLocked != 0; }, 1200, "the warden never locked the cages (195)");
	test::require(sim.townScorerStats().cagesLocked >= 1, "the cage lock was not counted");
	// A prisoner picked the cage open (the Lock record's open flag, dt38 Q2): 195 takes the unlocked cage again and 77 locks it.
	sim.lockEnsure(false, home).open = true;
	test::require(!sim.cageDoorLocked(home), "an opened cage reads locked");
	runUntil(sim, [&] { return sim.lockFind(false, home) == nullptr; }, 1200, "the warden never re-locked the opened cage (195 + 77)");
	test::require(sim.cageDoorLocked(home), "the re-locked cage reads open");
	std::cout << "cage warden ok\n";
}

// Task CN: 112's UTILITY (kinspect dt24_112 section 3, FUN_1405a0f10), the body's side of the decision - the existing cage warden
// stage shows the cage PICK. A body handed to a warden in its arms with the warden's chain on it: a non-ally is carried to a cage,
// an ally of the warden's own faction and an animal (the vtable+0x248 veto) score 0 and the caging goal is never adopted.
void cageLadder(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, const std::shared_ptr<MeshNavigator>& nav)
{
	Simulation sim = newGame(order);
	enable(sim, combat, nav);
	sim.setClockTick(tickAt(2, 14, 0));
	flecs::entity squad = squadIn(sim, kHeft); // the Heft Cage Wardens, homed in the Slave Shop with cages in its layout
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const std::uint64_t home = instance.homeBuilding;
	const state::Position at = *squad.get<state::Position>();
	centreOn(sim, at.x, at.z);
	runUntil(sim, [&] { return squadIn(sim, kHeft).has<state::SquadRoutine>(); }, 4000, "the Cage Warden was never promoted");
	flecs::entity carrier = membersOf(sim, squadIn(sim, kHeft)).front();
	const std::uint64_t warden = idOf(carrier);
	const state::Fighter template_ = fighterOf(sim, warden); // a copy: the arena grows as bodies are added
	const std::string wardenFaction = carrier.get<state::Allegiance>() != nullptr ? carrier.get<state::Allegiance>()->factionSid : "";

	// The body is put in the warden's arms with the chain target naming it: the scorer's chainedCarry lets it evaluate 112 for this
	// member (routine_scorer.cpp). `ally` is the warden's own faction, `animal` a body flagged as an animal of another faction.
	std::uint64_t previous = 0;
	const auto handOver = [&](bool animal, bool ally) -> std::uint64_t
	{
		if (previous != 0)
			sim.removeFighter(previous); // the last body: taken out (its carrier is freed by removeFighter)
		state::Fighter body = template_;
		body.team = 0;
		body.faction = ally ? template_.faction : 0;
		body.npcClass = 0;
		body.slaveState = 0;
		body.animal = animal;
		body.captorSid.clear();
		body.cageBuilding = 0;
		body.cageSlot = 0;
		body.building = home;
		body.unconscious = true;
		body.down = false;
		body.koTimer = 1.0e6;
		body.target = 0;
		body.carrying = 0;
		body.asleep = false;
		body.clearRoute();
		const state::Fighter& me = fighterOf(sim, warden);
		body.x = me.x;
		body.y = me.y;
		body.z = me.z;
		body.carriedBy = warden;
		flecs::entity spawned = sim.spawnFighter(std::move(body));
		spawned.set<state::Allegiance>(state::Allegiance{ally ? wardenFaction : "test-ladder"});
		const std::uint64_t id = idOf(spawned);
		const_cast<state::Fighter&>(fighterOf(sim, warden)).carrying = id;
		// The chain: idle goal, this body as the target, scored now.
		state::Routine r = carrier.has<state::Routine>() ? *carrier.get<state::Routine>() : state::Routine{};
		r.goal = 0;
		r.target = {};
		r.chainTarget = id;
		r.chainElapsed = -1.0;
		r.plan.clear();
		r.step = 0;
		r.nextScore = 0;
		carrier.set<state::Routine>(std::move(r));
		previous = id;
		return id;
	};
	// The goal the warden holds after `ticks` of scoring.
	const auto goalAfter = [&](int ticks) -> std::uint16_t
	{
		for (int i = 0; i < ticks; ++i)
			sim.run(1);
		const state::Routine* routine = carrier.get<state::Routine>();
		return routine != nullptr ? routine->goal : 0;
	};

	const std::uint64_t ally = handOver(false, true);
	test::require(goalAfter(600) != routines::task::FindCageAndPutIn, "112 adopted the caging goal for an ally (the utility must be 0)");
	test::require(fighterOf(sim, ally).cageBuilding == 0, "the ally was caged");
	const std::uint64_t animal = handOver(true, false);
	test::require(goalAfter(600) != routines::task::FindCageAndPutIn, "112 adopted the caging goal for an animal (the 0x248 veto must score 0)");
	test::require(fighterOf(sim, animal).cageBuilding == 0, "the animal was caged");
	const std::uint64_t stranger = handOver(false, false);
	// The utility is 2.0: the caging goal is adopted and the chain walks the body to a free cage of the home.
	bool caged = false;
	for (int i = 0; i < 12000 && !caged; ++i)
	{
		sim.run(1);
		caged = fighterOf(sim, stranger).cageBuilding == home;
	}
	test::require(caged, "the non-ally carried by the warden never reached a cage of the home");
	std::cout << "cage ladder ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_town_guards HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation first = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*first.gameData(), KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		halves(order, combat);
		nightShiftAddon(order, combat);
		intruderPickup(order, combat, nav);
		intruderPlayerEvict(order, combat, nav);
		// The warden's town (Heft) lies in its own region file of the baked world: without the world dir the third stage is skipped.
		const std::string heftMesh = std::string(KSIM_NAVWORLD_DIR) + "/region_11_4.knav";
		std::FILE* probe = std::fopen(heftMesh.c_str(), "rb");
		if (probe != nullptr)
		{
			std::fclose(probe);
			cageWarden(order, combat, MeshNavigator::load(heftMesh));
			cageLadder(order, combat, MeshNavigator::load(heftMesh));
		} else {
			std::cout << "cage warden skipped: no " << heftMesh << "\n";
		}
		std::cout << "ksim_town_guards: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_guards: FAILED: " << e.what() << "\n";
		return 1;
	}
}
