// Task BM: a KSCEN scenario owns its radius. flat1.kscen (tests/data, task BI) puts an unarmed player and an unarmed hostile
// bandit at the Hub with a 200 u area around them; the default world still fills that ground afterwards - the Hub's bar crowd
// (TownBar squads: Pacifier + Mercenary Captain + 4 mercenaries) is materialised by the resident pass at its bar table, which
// stands at flat1's spot 356 u from the squad's own token, and its bodies push the pair apart at the 18 u/s avoidance cap (the
// BI caveat). Asserted with the world AI ON: no character the file did not place stands inside the file's area at the end of
// any tick in the first 30 s (Simulation::holdScenarioArea), and the ordered fight still lands blows on both sides.
//
// Needs the world navmesh bake (argv[1], exit 77 without it, like ksim_scenario_ai).
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_KSCEN_FIXTURE
#error KSIM_KSCEN_FIXTURE must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

constexpr double kFreeSeconds = 30.0; // the area is checked over the first 30 s
constexpr double kFightSeconds = 120.0;
constexpr std::uint64_t kOrderTick = 80;

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_kscen_radius: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
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
		sim.enableNavigation(test::openWorldNav(world, zones));
		simlink::ServerContext context;
		context.gameData = gameData;
		const scenario::File file = scenario::parseFile(KSIM_KSCEN_FIXTURE);
		test::require(file.radius > 0.0, "the fixture names an area (radius)");
		const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, file, 1);
		test::require(placed.players.size() == 1 && placed.others.size() == 1, "the fixture is a 1v1");
		const std::uint64_t player = placed.players.front();
		const std::uint64_t enemy = placed.others.front();
		std::set<std::uint64_t> fileChars;
		for (const auto& [charId, id] : placed.idOf)
			fileChars.insert(id);
		fileChars.insert(player);
		fileChars.insert(enemy);

		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));

		std::uint64_t playerHits = 0;
		std::uint64_t enemyHits = 0;
		sim.setCombatObserver([&](const CombatEvent& e)
		{
			if (e.kind != CombatEvent::Kind::Hit)
				return;
			if (e.who == enemy) ++playerHits;
			if (e.who == player) ++enemyHits;
		});

		const std::uint64_t start = sim.clockTick();
		const std::uint64_t freeTicks = static_cast<std::uint64_t>(kFreeSeconds / Simulation::kTickSeconds);
		const std::uint64_t fightTicks = static_cast<std::uint64_t>(kFightSeconds / Simulation::kTickSeconds);
		flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
		std::uint64_t firstIntruder = 0;
		std::uint64_t intruders = 0;
		std::uint64_t lastChecked = start;
		while (sim.clockTick() - start < fightTicks)
		{
			const std::uint64_t elapsed = sim.clockTick() - start;
			if (elapsed == kOrderTick)
				sim.enqueue(Command::fighterAttack(state::Id{player}, state::Id{enemy}));
			sim.tick();
			lastChecked = sim.clockTick() - start;
			if (elapsed <= freeTicks)
			{
				// The world's characters (not the combat arena, which drops a removed squad's bodies on the next tick): no one the
				// file did not place stands inside its area at the end of any tick.
				const std::uint64_t tick = sim.clockTick();
				ecs.each([&](flecs::entity entity, const state::Id& id, const state::Fighter& f)
				{
					(void)entity;
					if (fileChars.count(id.value) != 0 || f.dead)
						return;
					const double d = std::hypot(f.x - file.centreX, f.z - file.centreZ);
					if (d > file.radius)
						return;
					++intruders;
					if (firstIntruder == 0)
						firstIntruder = tick;
					if (intruders <= 6)
						std::printf("ksim_kscen_radius: intruder %llu at tick %llu, %.1f u from the centre (radius %.0f): %s\n",
							static_cast<unsigned long long>(id.value), static_cast<unsigned long long>(tick), d, file.radius,
							f.name.c_str());
				});
			}
			// Timing only (kenshi-k16.8.1): the fight can now land both blows before the 30 s the area is held for (518 ticks); the check runs on to the end of them.
			if (playerHits > 0 && enemyHits > 0 && elapsed >= freeTicks)
				break;
		}
		std::printf("ksim_kscen_radius: %llu ticks, hits %llu/%llu, intruder ticks %llu (first at %llu), last tick checked %llu\n",
			static_cast<unsigned long long>(sim.clockTick() - start), static_cast<unsigned long long>(playerHits),
			static_cast<unsigned long long>(enemyHits), static_cast<unsigned long long>(intruders),
			static_cast<unsigned long long>(firstIntruder), static_cast<unsigned long long>(lastChecked));
		test::require(lastChecked >= freeTicks, "the run covered the first 30 s");
		test::require(intruders == 0, "a character outside the file stood inside its area");
		test::require(playerHits > 0 && enemyHits > 0, "both sides must land a hit within " + std::to_string(kFightSeconds) + " s");
		std::printf("ksim_kscen_radius: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_kscen_radius: FAIL: " << e.what() << "\n";
		return 1;
	}
}
