// kenshi-k16.17.13.1: the new-game start "Guy with a dog" (rebirth.mod) spawns its human and its dog as the start's data says, and nobody in the
// player's squad moves by itself. The tester's report: no dog, and "AI spinning in circles".
//   Spawn: the members are the start's template's `leader` + `squad` + `animals` counts (read from the data here, not from newgame.cpp), the animal
//   ones animals, all playerControlled, and the dog is in the squad's member list (outcome.members).
//   3 game minutes with no orders (user 2026-11-19: "even an animal in your squad is player controlled, there's no automatic AI beyond the
//   default stuff, like auto-eating"): per member
//     - no spin: the heading's accumulated turning since the member last stood more than kStillUnits from where it is now never passes one full
//       circle (2 pi). kStillUnits = 3 u: a body is about 1.5 u wide, so a walk of 3 u is a real step, and a character turning on the spot for a
//       whole circle without ever stepping is the spin the tester saw.
//     - no unordered motion: it ends within kStillUnits * 2 of where it started (a town crowd may shove it a step; nothing may walk it).
//     - no re-plan loop: the route's version (bumped on every set or clear) changes at most kRouteBumps times in the 3 minutes (no order was
//       given, so none is expected; a re-path loop bumps it every few seconds).
// KSIM_GUYDOG_VERBOSE=1: one line per member per game second (position, yaw, route, follow target, goal kind).
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	if (const kdata::RefCategory* refs = record.findRefs(category))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (!entry.opaqueEntry)
				func(entry);
		}
	}
}

constexpr double kPi = 3.14159265358979323846;
constexpr double kStillUnits = 3.0;
constexpr std::uint32_t kRouteBumps = 2;

double wrap(double a)
{
	while (a > kPi)
		a -= 2.0 * kPi;
	while (a < -kPi)
		a += 2.0 * kPi;
	return a;
}

struct Track
{
	std::uint64_t id = 0;
	bool animal = false;
	double startX = 0.0;
	double startZ = 0.0;
	double anchorX = 0.0;
	double anchorZ = 0.0;
	double lastYaw = 0.0;
	double turned = 0.0;
	double worstTurn = 0.0;
	double farthest = 0.0;
	std::uint32_t routeVersion0 = 0;
	std::uint32_t routeVersion = 0;
};

} // namespace

int main(int argc, char** argv)
try
{
	if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
	{
		std::cout << "ksim_start_guy_dog: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
	const bool verbose = std::getenv("KSIM_GUYDOG_VERBOSE") != nullptr;
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.markPristine();
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
	std::vector<kworld::ZoneCell> hubZones;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
			hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	}
	sim.enableNavigation(test::openWorldNav(argv[1], hubZones));
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, argv[1], 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(ai));

	// The start, by name; its squad as the data lists it.
	const kdata::GameData& data = *sim.gameData();
	const kdata::Record* start = nullptr;
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Guy with a dog")
			start = record;
	}
	test::require(start != nullptr, "the data has the start \"Guy with a dog\" (rebirth.mod)");
	std::int64_t wantHumans = 0;
	std::int64_t wantAnimals = 0;
	std::string dogSid;
	forEachRef(*start, "squad", [&](const kdata::RefEntry& ref)
	{
		const kdata::Record* squad = data.find(ref.targetID);
		test::require(squad != nullptr, "the start's squad ref resolves");
		for (const char* list : {"leader", "squad", "squad2"})
			forEachRef(*squad, list, [&](const kdata::RefEntry& entry) { wantHumans += entry.v0; });
		for (const char* list : {"animals", "animals2"})
		{
			forEachRef(*squad, list, [&](const kdata::RefEntry& entry)
			{
				wantAnimals += entry.v0;
				dogSid = entry.targetID;
			});
		}
	});
	test::require(wantHumans >= 1 && wantAnimals >= 1, "the data gives the start a human and an animal (" + std::to_string(wantHumans) + " / " + std::to_string(wantAnimals) + ")");

	NewGameOutcome outcome;
	sim.addNewGameObserver(&outcome, [&](const NewGameOutcome& o) { outcome = o; });
	sim.enqueue(Command::newGame(start->stringID, "Tester", 1));
	sim.run(1);
	test::require(outcome.reason.empty() && !outcome.members.empty(), "NEW_GAME refused: '" + outcome.reason + "'");

	std::vector<Track> tracks;
	std::int64_t humans = 0;
	std::int64_t animals = 0;
	for (const std::uint64_t id : outcome.members)
	{
		const state::Fighter* f = sim.combat()->find(id);
		test::require(f != nullptr && f->playerControlled, "every start member is a player-controlled fighter");
		(f->animal ? animals : humans) += 1;
		Track t;
		t.id = id;
		t.animal = f->animal;
		t.startX = t.anchorX = f->x;
		t.startZ = t.anchorZ = f->z;
		t.lastYaw = f->yaw;
		t.routeVersion0 = t.routeVersion = f->routeVersion;
		tracks.push_back(t);
	}
	test::require(humans == wantHumans, "the start spawns its " + std::to_string(wantHumans) + " human(s) (" + std::to_string(humans) + ")");
	test::require(animals == wantAnimals, "the start spawns its " + std::to_string(wantAnimals) + " animal(s), the dog " + dogSid + " (" + std::to_string(animals) + ")");

	// 3 game minutes, no orders.
	const std::uint64_t ticks = static_cast<std::uint64_t>(180.0 / kTickSeconds);
	const std::uint64_t perSecond = static_cast<std::uint64_t>(1.0 / kTickSeconds);
	for (std::uint64_t i = 0; i < ticks; ++i)
	{
		sim.run(1);
		for (Track& t : tracks)
		{
			const state::Fighter* f = sim.combat()->find(t.id);
			if (f == nullptr || f->dead)
				continue;
			if (std::hypot(f->x - t.anchorX, f->z - t.anchorZ) > kStillUnits)
			{
				t.anchorX = f->x;
				t.anchorZ = f->z;
				t.turned = 0.0;
			}
			t.turned += std::fabs(wrap(f->yaw - t.lastYaw));
			t.lastYaw = f->yaw;
			t.worstTurn = std::max(t.worstTurn, t.turned);
			t.farthest = std::max(t.farthest, std::hypot(f->x - t.startX, f->z - t.startZ));
			t.routeVersion = f->routeVersion;
			if (verbose && i % perSecond == 0)
			{
				std::cout << "t=" << i / perSecond << " id " << t.id << (t.animal ? " animal" : " human") << " pos " << f->x << " " << f->z << " yaw " << f->yaw
					<< " route " << f->routeNext << "/" << f->route.size() << " v" << f->routeVersion << (f->routeChase ? " chase" : "")
					<< " follow " << f->followTarget << " goal " << static_cast<int>(f->goal.kind) << " target " << f->target << "\n";
			}
		}
	}
	for (const Track& t : tracks)
	{
		const std::string who = std::string(t.animal ? "the dog " : "the human ") + std::to_string(t.id);
		std::cout << who << ": worst turning " << t.worstTurn << " rad without a " << kStillUnits << " u step, farthest " << t.farthest << " u, route bumps " << t.routeVersion - t.routeVersion0 << "\n";
		test::require(t.worstTurn <= 2.0 * kPi, who + " spins in place (" + std::to_string(t.worstTurn) + " rad of turning within " + std::to_string(kStillUnits) + " u)");
		test::require(t.farthest <= 2.0 * kStillUnits, who + " moved by itself with no order (" + std::to_string(t.farthest) + " u)");
		test::require(t.routeVersion - t.routeVersion0 <= kRouteBumps, who + " re-planned its route in a loop (" + std::to_string(t.routeVersion - t.routeVersion0) + " route changes)");
	}
	std::cout << "ksim_start_guy_dog: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_start_guy_dog: FAIL: " << e.what() << "\n";
	return 1;
}
