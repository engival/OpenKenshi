// A squad animal rolls its age at spawn (rwh.193 section 5, createRandomSquad 140583a10): age = rand * `animal age random` - 0.1 +
// the entry's v2 * 0.01, clamped >= 0, for an `animals` / `animals2` member, by the one makeFighter every spawn path (new game, world
// squads, residents) builds a fighter with. Every world SQUAD_TEMPLATE with an animals list is swept over seeds: each age lies in its own
// range, and a template whose range has width shows more than one age. Anything not in such a list stays 1. The age is saved (v119).

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/save/scenario_file.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "sim_access.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main()
{
	try
	{
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const auto data = CombatData::build(*gd, KENSHI_DATA);
		const std::int32_t animalType = kworld::itemTypeByName("ANIMAL_CHARACTER");
		std::size_t entries = 0;
		std::size_t spread = 0;
		for (const kdata::Record* squad : gd->byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		{
			const double random = squad->floatOr("animal age random", 0.1);
			std::set<std::string> listed; // makeFighter takes the FIRST entry naming an animal (it knows the character, not its list slot: UNSURE for a repeat)
			for (const std::string_view list : {"animals", "animals2"})
			{
				const kdata::RefCategory* refs = squad->findRefs(list);
				if (refs == nullptr)
					continue;
				for (const kdata::RefEntry& entry : refs->entries)
				{
					const kdata::Record* animal = gd->find(entry.targetID);
					if (animal == nullptr || animal->type != animalType || !listed.insert(entry.targetID).second)
						continue;
					const double low = std::max(0.0, entry.v2 * 0.01 - 0.1);
					const double high = std::max(0.0, entry.v2 * 0.01 - 0.1 + random);
					std::set<double> seen;
					for (std::uint64_t seed = 1; seed <= 40; ++seed)
					{
						std::uint64_t rng = seed * 7919;
						const state::Fighter f = data->makeFighter(*gd, entry.targetID, "", rng, squad->stringID);
						test::require(f.age >= low - 1e-9 && f.age <= high + 1e-9, squad->name + ": " + animal->name + " age " + std::to_string(f.age)
							+ " outside " + std::to_string(low) + ".." + std::to_string(high));
						seen.insert(f.age);
					}
					++entries;
					if (high > low + 1e-6)
					{
						test::require(seen.size() > 1, squad->name + ": " + animal->name + " rolled one age over a " + std::to_string(high - low) + " wide range (random " + std::to_string(random) + ", v2 " + std::to_string(entry.v2) + ")");
						++spread;
					}
				}
			}
		}
		test::require(entries > 0 && spread > 0, "the data has world squads with animals lists, some with an age range");
		std::cout << "ksim_animal_age: " << entries << " animals entries swept, " << spread << " with a range\n";

		// Out of a squad's animals list: adult. A human never rolls.
		std::uint64_t rng = 5;
		for (const kdata::Record* animal : gd->byType(animalType))
		{
			test::require(data->makeFighter(*gd, animal->stringID, "", rng).age == 1.0, "an animal spawned with no squad template stays adult");
			break;
		}

		// A saved / scenario character's own `age` wins over the roll; an absent one keeps it.
		{
			scenario::Char c;
			std::vector<scenario::Item> carried;
			std::set<std::string> unmapped;
			state::Fighter rolled = data->makeFighter(*gd, gd->byType(animalType).front()->stringID, "", rng);
			rolled.age = 0.8;
			scenario::applyChar(*gd, *data, c, rolled, carried, unmapped);
			test::require(rolled.age == 0.8, "a character with no age kept the roll");
			c.age = 0.25;
			scenario::applyChar(*gd, *data, c, rolled, carried, unmapped);
			test::require(rolled.age == 0.25, "an explicit age did not win over the roll");
		}

		// The save keeps it.
		Simulation sim = Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		state::Fighter f = data->makeFighter(*gd, gd->byType(animalType).front()->stringID, "", rng);
		f.age = 0.37;
		f.x = -50978.8;
		f.z = 2932.48;
		f.y = 1647.0;
		const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		std::ostringstream out;
		sim.save(out);
		std::istringstream in(out.str());
		sim.reload(in);
		const state::Fighter* back = sim.combat()->find(id);
		test::require(back != nullptr, "the animal is gone after the reload");
		test::require(back->age == 0.37, "the animal's age was not kept by the save: " + std::to_string(back->age));
		std::cout << "ksim_animal_age: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_animal_age: FAILED: " << e.what() << "\n";
		return 1;
	}
}
