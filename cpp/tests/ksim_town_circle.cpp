// The towns' circles (task town-circle): the exe's Town radius, kinspect Q2/Q3 in BUILDINGS_SLICE - the base by TOWN `type` x
// `town radius mult` (TOWN 2500, POI 437.5, ...). The base commit used the record's `size radius` instead (the nest size,
// GAMEDATA_SCHEMA): 350 for the Hub and 100 for the Rebel Base, which puts each town's own bar outside its circle - the Hub's bar
// stands 356 u from the Hub's centre and the Rebel Base's bar 187 u from the Rebel Base's centre. The bar fixtures' joins (goal 34's
// `getTown(t) == myTown`) and the 144 home ring read these circles, so the test pins both bars inside their towns.

#include "ksim/combat_data.hpp"
#include "ksim/perception.hpp"
#include "ksim/simulation.hpp"
#include "ksim/state.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include <memory>

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

void requireNear(double have, double want, double tolerance, const std::string& what)
{
	test::require(std::fabs(have - want) <= tolerance, what + " (" + std::to_string(have) + ", want " + std::to_string(want) + ")");
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const std::shared_ptr<const kdata::GameData> gameData =
			std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		SpawnOptions options;
		options.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
		const std::shared_ptr<const CombatData> combatData = CombatData::build(*gameData, KENSHI_DATA);
		sim.enableCombat(combatData, CombatSettings{});
		// stepCombat returns while the arena is empty (a fresh world's characters are promoted to Fighters lazily), so one
		// fighter is spawned only to make the first step run.
		std::uint64_t rng = 3;
		for (const kdata::Record* record : gameData->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (record->name == "Escaped Servant")
			{
				state::Fighter fighter = combatData->makeFighter(*gameData, record->stringID, "", rng);
				fighter.x = -50000.0;
				fighter.z = -1700.0;
				sim.spawnFighter(std::move(fighter));
				break;
			}
		}
		sim.run(2); // the environment is rebuilt on the first step (stepCombat's walled / towns block)
		const perception::Environment& env = sim.combat()->environment();
		test::require(!env.towns.empty(), "the simulation lists the towns' circles");

		// The Hub (TOWN type 2, town radius mult 1.0): radius 2500 u, and its Bar (Storm House 3384, TOWN_LIFE_SLICE 5.17) inside
		// at 356 u. The record's `size radius` 350 would have left it outside.
		const int hub = env.townOf(-50978.8, 2932.5);
		test::require(hub >= 0, "the Hub's placement is inside a town circle");
		requireNear(env.towns[hub].x, -50978.8, 5.0, "the Hub's circle is centred on its placement x");
		requireNear(env.towns[hub].z, 2932.5, 5.0, "the Hub's circle is centred on its placement z");
		requireNear(env.towns[hub].radius, 2500.0, 0.5, "a TOWN's radius is its type's base x mult (2500)");
		test::require(env.townOf(-51181.0, 2639.0) == hub,
			"the Hub's circle contains the Hub's Bar at (-51181, 2639), 356 u out (size radius 350 did not)");

		// The Rebel Base (TOWN type 9 POI, mult 1.0): radius 437.5, and the Bar the bar-fight fixtures use at (-50592, -1211), 187 u
		// out. The record's `size radius` 100 would have left the crew out of the town (and goal 34's myTown == 0 gate would have
		// refused the join).
		const int rebel = env.townOf(-50642.4, -1030.4);
		test::require(rebel >= 0 && rebel != hub, "the Rebel Base's placement is inside its own circle");
		requireNear(env.towns[rebel].radius, 437.5, 0.5, "a POI's radius is 437.5");
		test::require(env.townOf(-50592.0, -1211.0) == rebel,
			"the Rebel Base's circle contains its Bar at (-50592, -1211), 187 u out (size radius 100 did not)");

		// The radius is a bound, not just a big number: 2400 u from the Hub is in the Hub, 2600 u is not.
		test::require(env.townOf(-50978.8 + 2400.0, 2932.5) == hub, "2400 u from the Hub's centre is in the Hub");
		test::require(env.townOf(-50978.8 + 2600.0, 2932.5) != hub, "2600 u from the Hub's centre is not");

		std::cout << "ksim_town_circle OK (Hub radius " << env.towns[hub].radius << ", Rebel Base " << env.towns[rebel].radius << ")\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_circle FAILED: " << e.what() << "\n";
		return 1;
	}
}
