// The world's new-game advanced options (ADVANCED_OPTIONS_STATIC.md "Ours"; save v77, task BT): the state::WorldOptions
// singleton that a NEW_GAME carries (cod / gdm / ht), setGameOption / gameOptionRows (DEBUG_SET_OPTION 215, DEBUG_QUERY kind 2),
// the medical clock's synced default and its dev override, save -> load, and what each knob does to the medical step:
// cod doubles the bleed rate, ht halves the hunger drain at twice its value, and medicalClock is what turns medical units
// into sim seconds. The NEW_GAME frame's optional trailing options block is pinned in ksim_newgame; the debug wire in
// ksim_debug_weather_time.

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/save/save.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

// One key's value in gameOptionRows.
std::string row(const ksim::Simulation& sim, const std::string& key)
{
	for (const auto& [name, value] : sim.gameOptionRows())
		if (name == key)
			return value;
	throw std::runtime_error("no option row " + key);
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const ksim::CombatData> data = ksim::CombatData::build(*gd, KENSHI_DATA);

		// ---- the synced medical clock and the defaults of a world without options ----
		{
			const ksim::CombatSettings defaults;
			test::require(std::fabs(defaults.medicalClock - 1.0 / 1.04) < 1e-12,
				"the medical clock default is the original's nominal rate (1 unit per 1.04 real s), not 2.0: "
				+ std::to_string(defaults.medicalClock));
			test::require(defaults.chanceOfDeath == 1.0 && defaults.hungerTime == 4.0 && defaults.damageScale == 1.0,
				"the CombatSettings knobs keep their defaults");
		}

		ksim::Simulation sim = ksim::Simulation::load(KENSHI_DATA, 91);
		sim.setGameData(gd);
		sim.enableCombat(data, ksim::CombatSettings{});
		test::require(sim.worldOptions() == nullptr, "a fresh world carries no WorldOptions singleton");
		test::require(row(sim, "cod") == "1" && row(sim, "gdm") == "1" && row(sim, "ht") == "4" && row(sim, "saved") == "no",
			"the default rows are the data defaults and not saved: cod " + row(sim, "cod") + ", ht " + row(sim, "ht"));
		test::require(sim.combat()->rules().settings().medicalClock == ksim::CombatSettings{}.medicalClock,
			"the arena starts at the medical clock default");

		// ---- setGameOption: refusals change nothing ----
		{
			const std::string rowsBefore = row(sim, "cod") + "/" + row(sim, "ht");
			test::require(sim.setGameOption("bs", 1.0) == "unknown option 'bs'", "an unimplemented knob is refused by name");
			test::require(sim.setGameOption("cod", 4.5) == "'cod' out of range 0.5..4", "cod above its range");
			test::require(sim.setGameOption("cod", 0.4) == "'cod' out of range 0.5..4", "cod below its range");
			test::require(sim.setGameOption("gdm", 0.25) == "'gdm' out of range 0.5..4", "gdm below its range");
			test::require(sim.setGameOption("ht", 8.5) == "'ht' out of range 0.25..8", "ht above its range");
			test::require(sim.setGameOption("medicalClock", 9.0) == "'medicalClock' out of range 0.25..4", "clock above its range");
			test::require(sim.setGameOption("cod", std::nan("")) == "value is not finite", "a NaN");
			test::require(sim.worldOptions() == nullptr && row(sim, "cod") + "/" + row(sim, "ht") == rowsBefore,
				"a refusal made the singleton or changed a value");
		}

		// ---- the saved options, live in the arena, in the rows, and over a save -> load ----
		{
			test::require(sim.setGameOption("cod", 2.0).empty(), "cod 2.0");
			test::require(sim.setGameOption("gdm", 1.5).empty(), "gdm 1.5");
			test::require(sim.setGameOption("ht", 2.0).empty(), "ht 2.0");
			test::require(sim.setGameOption("medicalClock", 2.0).empty(), "the clock override 2.0");
			const ksim::state::WorldOptions* saved = sim.worldOptions();
			test::require(saved != nullptr && saved->chanceOfDeath == 2.0 && saved->damageScale == 1.5 && saved->hungerTime == 2.0,
				"the singleton holds cod / gdm / ht");
			const ksim::CombatSettings& live = sim.combat()->rules().settings();
			test::require(live.chanceOfDeath == 2.0 && live.damageScale == 1.5 && live.hungerTime == 2.0
				&& live.medicalClock == 2.0, "the arena's copy took the change at once");
			test::require(row(sim, "saved") == "yes", "the rows say the options are saved");

			std::ostringstream out;
			sim.save(out);
			std::istringstream in(out.str());
			sim.reload(in);
			saved = sim.worldOptions();
			test::require(saved != nullptr && saved->chanceOfDeath == 2.0 && saved->damageScale == 1.5 && saved->hungerTime == 2.0,
				"save -> load kept the world's options (save v77)");
			test::require(sim.combat()->rules().settings().chanceOfDeath == 2.0, "the loaded arena took cod");
			test::require(sim.combat()->rules().settings().hungerTime == 2.0, "the loaded arena took ht");
			// The medical clock is a session's dev override, not a saved option: a reload in this session keeps it, a fresh
			// restore with the default settings comes back to the synced rate.
			test::require(sim.combat()->rules().settings().medicalClock == 2.0,
				"the session's clock override survives its own reload");
			std::istringstream fresh(out.str());
			ksim::Simulation restored = ksim::Simulation::restore(ksim::World::load(KENSHI_DATA), fresh, {}, false, nullptr, gd);
			restored.enableCombat(data, ksim::CombatSettings{});
			test::require(restored.worldOptions() != nullptr && restored.worldOptions()->chanceOfDeath == 2.0,
				"a fresh restore reads the world's saved options");
			test::require(restored.combat()->rules().settings().medicalClock == ksim::CombatSettings{}.medicalClock,
				"the medical clock override is not saved (a fresh restore is at the default)");
			std::cout << "options: cod 2, gdm 1.5, ht 2 survived a save/load; the clock override did not\n";
		}

		// ---- what cod and ht do to the medical step ----
		{
			const kdata::Record* bandit = recordNamed(*gd, "CHARACTER", "Dust Bandit");
			std::uint64_t rng = 11;
			const auto make = [&]()
			{
				ksim::state::Fighter f = data->makeFighter(*gd, bandit->stringID, "", rng);
				f.skills.toughness = 5.0;
				f.hunger = 8.0;
				return f;
			};
			ksim::CombatSettings settings;
			settings.medicalClock = 0.0; // no medical time passes: the wound does not clot, only the rate is set
			settings.chanceOfDeath = 1.0;
			const ksim::CombatRules calm(data, settings);
			ksim::state::Fighter wounded = make();
			wounded.wounds.push_back(0.05);
			calm.updateMedical(wounded, 0.0);
			const double calmBleed = wounded.bleeding;
			test::require(calmBleed > 0.0, "a 0.05 wound bleeds at cod 1");
			settings.chanceOfDeath = 2.0;
			const ksim::CombatRules deadly(data, settings);
			ksim::state::Fighter hurt = make();
			hurt.wounds.push_back(0.05);
			deadly.updateMedical(hurt, 0.0);
			test::require(std::fabs(hurt.bleeding - 2.0 * calmBleed) < 1e-9,
				"cod 2 doubles the bleed rate (ADVANCED_OPTIONS_STATIC)");

			// Hunger runs on game time, not the medical clock (medicalClock 0 freezes the rest of the step).
			ksim::CombatSettings hunger;
			hunger.medicalClock = 0.0;
			hunger.hungerTime = 4.0;
			const ksim::CombatRules ht4(data, hunger);
			ksim::state::Fighter a = make();
			ht4.updateMedical(a, 600.0);
			const double drain4 = 8.0 - a.hunger;
			hunger.hungerTime = 8.0;
			const ksim::CombatRules ht8(data, hunger);
			ksim::state::Fighter b = make();
			ht8.updateMedical(b, 600.0);
			const double drain8 = 8.0 - b.hunger;
			test::require(drain4 > 0.0 && std::fabs(drain8 * 2.0 - drain4) < 1e-9,
				"ht 8 drains the hunger in half the time of ht 4 (MEDICAL_HUNGER_LIVE a)");
			std::cout << "cod 2 doubles the bleed; ht 8 vs 4 drains " << drain8 << " vs " << drain4 << " in 600 s\n";
		}

		std::cout << "ksim_world_options: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_world_options: FAIL: " << error.what() << "\n";
		return 1;
	}
}
