// A hand-written KSCEN fight runs to blows (task BI, user playtest 2026-11-06): tests/data/flat1.kscen, an unarmed player
// (Tester) and an unarmed hostile bandit (Solo1) at the Hub, the player ordered to attack at tick 80. Both sides entered
// combat and targeted, then stood at velocity 0 for ever (the client's "state between/circle, no swings"): with martial arts
// 1.0 the only admissible standing technique is `ma badpunch` (skill <= 20) at a pick distance <= 5, i.e. a centre distance
// of ~15, and the pair drifted to 26 apart with a 39 u walk between them (a ledge). The circle state had no exit for a target
// its walk cannot reach and stood on the ring for ever. Asserted: the player's order starts a fight and BOTH sides land hits
// within kFightSeconds.
//
// Player orders only: the world AI is not enabled (the default-world camp around the spot would push the pair apart with its
// avoidance, a separate import question). Needs the world navmesh bake (argv[1], exit 77 without it, like ksim_scenario_ai).
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_KSCEN_FIXTURE
#error KSIM_KSCEN_FIXTURE must be defined
#endif

namespace {

using namespace ksim;

// The fight window and the tick the player's order goes in at.
constexpr std::uint64_t kFightSeconds = 120;
constexpr std::uint64_t kOrderTick = 80;

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_kscen_fight: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string bake(argv[1]);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const std::shared_ptr<const kdata::GameData> gameData = sim.gameData();
		sim.enableWeather(*gameData);
		const std::shared_ptr<const CombatData> combatData = CombatData::build(*gameData, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combatData, settings);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		sim.enableNavigation(test::openWorldNav(bake, zones));
		simlink::ServerContext context;
		context.gameData = gameData;
		const scenario::File file = scenario::parseFile(KSIM_KSCEN_FIXTURE);
		const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, file, 1);
		test::require(placed.players.size() == 1 && placed.others.size() == 1, "the fixture is a 1v1");
		const std::uint64_t player = placed.players.front();
		const std::uint64_t enemy = placed.others.front();

		// Per side: hits landed (a blow that resolved through) and swings started.
		std::uint64_t playerHits = 0;
		std::uint64_t enemyHits = 0;
		std::uint64_t playerSwings = 0;
		std::uint64_t enemySwings = 0;
		std::uint64_t firstHitTick = 0;
		const std::uint64_t start = sim.clockTick();
		sim.setCombatObserver([&](const CombatEvent& e)
		{
			const std::uint64_t tick = sim.clockTick();
			if (e.kind == CombatEvent::Kind::TechniqueStart)
			{
				if (e.who == player) ++playerSwings;
				if (e.who == enemy) ++enemySwings;
			}
			if (e.kind == CombatEvent::Kind::Hit)
			{
				if (e.who == enemy) ++playerHits;
				if (e.who == player) ++enemyHits;
				if (firstHitTick == 0) firstHitTick = tick;
			}
		});

		while (sim.clockTick() - start < kOrderTick)
			sim.tick(); // the client's script: the order comes at tick 80
		sim.enqueue(Command::fighterAttack(state::Id{player}, state::Id{enemy}));

		while (sim.clockTick() - start < kOrderTick + kFightSeconds / Simulation::kTickSeconds)
		{
			sim.tick();
			if (playerHits > 0 && enemyHits > 0)
				break;
		}
		const std::uint64_t ticks = sim.clockTick() - start;
		std::printf("ksim_kscen_fight: %llu ticks, swings %llu/%llu, hits %llu/%llu, first hit at %llu\n",
			static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(playerSwings),
			static_cast<unsigned long long>(enemySwings), static_cast<unsigned long long>(playerHits),
			static_cast<unsigned long long>(enemyHits), static_cast<unsigned long long>(firstHitTick));
		test::require(playerSwings > 0 && enemySwings > 0, "both sides must start a swing");
		test::require(playerHits > 0, "the player must land a hit within " + std::to_string(kFightSeconds) + " s");
		test::require(enemyHits > 0, "the enemy must land a hit within " + std::to_string(kFightSeconds) + " s");
		std::printf("ksim_kscen_fight: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_kscen_fight: FAIL: " << e.what() << "\n";
		return 1;
	}
}
