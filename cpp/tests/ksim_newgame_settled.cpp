// kenshi-k16.17.9: a New Game settles before the player sees it (TOWN_LIFE_SLICE 5.40). Vanilla "settles that before you load in, and you start with
// almost nobody in sight" (user 2026-11-12, journey_new_game v1): the residents round the Wanderer are already at their sites (seated, in bed, at post)
// at the first frame, not standing on a ring round the squad token and walking in. Setup as ksim_residents_arrive: New Game at the Hub. t0 is the first
// frame (right after the tick that applied NEW_GAME). Asserts: at t0 at most 2 residents within 300 u of the player are walking; the clock reads the start
// time (10:00 day 1, plus the few ticks the command took); and the aliveness of TOWN_LIFE_SLICE 5.34 still holds in the next 30 s (some residents move).
//
// KSIM_SETTLE=S: the settle in seconds (default: SpawnOptions'; 0 = off, the world as before 5.40: this test fails). KSIM_SETTLE_VERBOSE=1: the census.
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main(int argc, char** argv)
try
{
	if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
	{
		std::cout << "ksim_newgame_settled: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
	constexpr double kCx = -50986.0;
	constexpr double kCz = 3514.0;
	constexpr double kRadius = 1200.0;
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 1;
	if (const char* settle = std::getenv("KSIM_SETTLE"))
		options.settleSeconds = std::atof(settle);
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
	std::string sid;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Wanderer")
			sid = record->stringID;
	}
	// The settle runs inside the tick that applies NEW_GAME: the observer sees the clock at that command, before the settle.
	std::uint64_t commandTick = 0;
	sim.addNewGameObserver(&commandTick, [&](const NewGameOutcome&) { commandTick = sim.clockTick(); });
	const std::uint64_t stuckBefore = sim.ai()->stats().failures[static_cast<std::size_t>(state::AiReason::Stuck)];
	sim.enqueue(Command::newGame(sid, "Tester", 1));
	sim.run(1); // the tick that applies NEW_GAME, and the settle with it
	const std::uint64_t startTick = tickAt(kNewGameDay, kNewGameHour, kNewGameMinute);
	const std::uint64_t settleTicks = static_cast<std::uint64_t>(std::llround(options.settleSeconds / kTickSeconds));
	// The first frame shows day 1 10:00 (the few ticks the command took apart).
	test::require(sim.clockTick() >= startTick && sim.clockTick() <= startTick + 10, "the first frame shows the start time (tick " + std::to_string(sim.clockTick()) + ", start " + std::to_string(startTick) + ")");
	const GameTime shown = gameTimeAt(sim.clockTick());
	test::require(shown.day == kNewGameDay && shown.hour == kNewGameHour && shown.minute <= kNewGameMinute + 1, "the first frame is day 1 10:00");
	// The same, by the stamp itself: no squad's AI records progress in the future (the sim subtracts stamps unsigned).
	std::size_t future = 0;
	sim.world().ecs().each([&](const state::GroupAI& ai) { future += ai.progressTick > sim.clockTick() || ai.lastStep > sim.clockTick() ? 1 : 0; });
	test::require(future == 0, "no squad's AI stamps a tick after the clock (" + std::to_string(future) + " do)");
	// A stamp written on the NEW_GAME tick is never in the future: no group goal fails as Stuck (an unsigned now - stamp wraps), world-wide.
	test::require(sim.ai()->stats().failures[static_cast<std::size_t>(state::AiReason::Stuck)] == stuckBefore, "the settle failed a goal as Stuck (" + std::to_string(sim.ai()->stats().failures[static_cast<std::size_t>(state::AiReason::Stuck)] - stuckBefore) + " of them)");
	// The clock never goes backwards: the settle moves it the whole settle forward from the command's tick (a rewind and re-run ends at the command's tick).
	test::require(sim.clockTick() == commandTick + settleTicks, "the settle moves the clock forward by its length (command at " + std::to_string(commandTick) + ", first frame " + std::to_string(sim.clockTick()) + ", settle " + std::to_string(settleTicks) + ")");
	// With the settle off the Hub's residents are built a few ticks after the command (the player areas first): t0 is then the first tick they are Fighters.
	const auto promoted = [&]
	{
		std::size_t n = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			n += !m.fighter.playerControlled && std::hypot(m.fighter.x - kCx, m.fighter.z - kCz) <= kRadius ? 1 : 0;
		return n;
	};
	for (int i = 0; i < 400 && promoted() == 0; ++i)
		sim.run(1);
	const state::Fighter* player = nullptr;
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		if (m.fighter.playerControlled)
			player = &m.fighter;
	}
	test::require(player != nullptr, "NEW_GAME made no player character");
	const double px = player->x;
	const double pz = player->z;
	std::size_t nearby = 0;
	std::size_t walking = 0;
	std::size_t hubWalking = 0;
	std::map<std::uint64_t, std::pair<double, double>> start;
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		if (m.fighter.playerControlled)
			continue;
		const double d = std::hypot(m.fighter.x - px, m.fighter.z - pz);
		if (d <= 300.0)
		{
			++nearby;
			walking += m.fighter.walking() ? 1 : 0;
		}
		if (std::hypot(m.fighter.x - kCx, m.fighter.z - kCz) <= kRadius)
		{
			start[m.id] = {m.fighter.x, m.fighter.z};
			hubWalking += m.fighter.walking() ? 1 : 0;
		}
	}
	std::cout << "t0: residents within 300 u of the Wanderer " << nearby << ", walking " << walking << "; Hub residents " << start.size() << ", walking " << hubWalking << ", the Wanderer at " << px << " " << pz << ", tick " << sim.clockTick() << "\n";
	sim.run(static_cast<std::uint64_t>(30.0 / kTickSeconds));
	std::size_t moved = 0;
	std::size_t far = 0;
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		const auto it = start.find(m.id);
		if (it == start.end())
			continue;
		const double d = std::hypot(m.fighter.x - it->second.first, m.fighter.z - it->second.second);
		moved += d > 1.0 ? 1 : 0;
		far += d > 200.0 ? 1 : 0;
	}
	std::cout << "next 30 s: moved >1 u " << moved << ", >200 u " << far << "\n";
	test::require(start.size() >= 10, "the Hub's residents are promoted by the first frame");
	test::require(walking <= 2, "at the first frame at most 2 residents within 300 u of the Wanderer are walking (" + std::to_string(walking) + " are)");
	test::require(moved >= 2, "the Hub stays alive after the settle: some residents move in the next 30 s (5.34's aliveness)");
	std::cout << "ksim_newgame_settled: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_newgame_settled: FAIL: " << e.what() << "\n";
	return 1;
}
