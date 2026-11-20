// The limb leak and dismemberment (kinspect BQ11, ~/git/kenshi/kenshi-inspect/findings/bq11_limb_leak.md and s76_static.md; BAR_FIGHT_LOOP
// 0d). Drives CombatRules::updateMedical on a Dust Bandit with medicalClock 1.0 (one step of dt is dt medical units). Checked:
//   - an intact limb at any negative health never leaks and the body regains blood;
//   - a hit past -M severs a humanoid's limb (Dismemberment 1), never an animal's, not at level 0; level 2 also by the passive sink;
//   - a sever costs U[5, 30] blood once, adds a wound of 0.3, forces the limb's max to 5 and leaks 0.002 x |H| from then on;
//   - the heal pool (first aid) cancels the leak; the sink stops at -3M (intact -300, severed -15);
//   - the severed state survives save + load.

#include "ksim/combat/combat.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

state::FighterPart* part(state::Fighter& f, const char* name)
{
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == name)
			return &p;
	}
	throw std::runtime_error(std::string("no part ") + name);
}

void require(bool ok, const char* what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what);
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* bandit = recordNamed(gd, "CHARACTER", "Dust Bandit");
		const auto rulesFor = [&](int level)
		{
			CombatSettings settings;
			settings.medicalClock = 1.0;
			settings.limbLossLevel = level;
			return CombatRules(data, settings);
		};
		const CombatRules rules = rulesFor(1);
		require(CombatSettings{}.limbLossLevel == 1, "Dismemberment defaults to 1 (Rare)");
		std::uint64_t rng = 11;
		const auto make = [&]()
		{
			state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
			f.skills.toughness = 5.0;
			f.hunger = 3.0;
			f.unconscious = true;
			f.koTimer = 1.0e9; // stays down
			return f;
		};

		// ---- 1. an intact limb at -80 never leaks, the body regains blood ----
		{
			state::Fighter f = make();
			part(f, "Left Leg")->health = -80.0;
			f.blood = 20.0;
			double worstBleed = 0.0;
			for (int i = 0; i < 1000; ++i)
			{
				rules.updateMedical(f, 1.0);
				worstBleed = std::max(worstBleed, f.bleeding);
			}
			require(worstBleed == 0.0 && f.wounds.empty(), "an intact limb at -80 never leaks");
			require(f.blood > 20.0 + 30.0, "and the body regains blood");
			require(!part(f, "Left Leg")->severed(), "and the limb stays intact (sinking, not severed)");
		}

		// ---- 2. a hit past -M severs: cost, wound, leak, the -15 floor ----
		{
			state::Fighter f = make();
			state::FighterPart* leg = part(f, "Right Leg");
			leg->health = -99.0;
			require(!rules.trySever(f, *leg, true), "-99 with M 100 does not sever");
			leg->health = -104.5;
			f.blood = 100.0;
			require(rules.trySever(f, *leg, true) && leg->severed(), "-104.5 severs");
			const double cost = 100.0 - f.blood;
			std::printf("    sever cost %.2f blood\n", cost);
			require(cost >= 5.0 && cost <= 30.0, "the sever costs U[5, 30] blood");
			require(f.wounds.size() == 1 && std::fabs(f.wounds[0] - data->k.bleedRate * 200.0) < 1e-9, "and adds a wound entry of 200 x bleed rate (0.3 on base data, 0.2 on rebirth)");
			require(leg->maxHealth == 5.0, "the limb's max is forced to 5");
			require(!rules.trySever(f, *leg, true), "a severed limb is not severed again");
			const double blood0 = f.blood;
			for (int i = 0; i < 400; ++i)
				rules.updateMedical(f, 1.0); // the wound clots (0.3 at 0.00085 per unit), the leak does not
			const double leak = 0.002 * 104.5 * data->k.extraBloodLoss * 1.2 * f.bloodFactor;
			std::printf("    bleed %.4f (leak %.4f), health %.2f\n", f.bleeding, leak, leg->health);
			require(f.wounds.empty() && std::fabs(f.bleeding - leak) < 1e-9, "the severed limb leaks 0.002 x |H| for good");
			require(leg->health == -104.5, "a limb already below the -15 floor does not sink or rise");
			require(f.blood < blood0, "and the body bleeds");
			// first aid: the heal pool cancels the leak
			leg->pool = 110.0;
			rules.updateMedical(f, 1.0);
			require(f.bleeding == 0.0, "a heal pool above |H| cancels the leak");
		}
		{
			// a severed limb that is still above the floor sinks to -15 only
			state::Fighter f = make();
			state::FighterPart* arm = part(f, "Left Arm");
			arm->health = -100.5;
			require(rules.trySever(f, *arm, true), "an arm severs the same way");
			arm->health = -3.0;
			for (int i = 0; i < 20000; ++i)
				rules.updateMedical(f, 1.0);
			std::printf("    severed limb floor: %.2f\n", arm->health);
			require(arm->health <= -15.0 && arm->health > -15.5, "a severed limb's sink stops at -3 x 5");
		}

		{
			// an intact limb sinks to -3M = -300 and no further (it never leaks on the way)
			state::Fighter f = make();
			state::FighterPart* leg = part(f, "Left Leg");
			leg->health = -299.0;
			for (int i = 0; i < 3000; ++i)
				rules.updateMedical(f, 1.0);
			std::printf("    intact limb floor: %.2f\n", leg->health);
			require(leg->health <= -300.0 && leg->health > -301.0 && f.bleeding == 0.0, "an intact limb's sink stops at -3M (-300)");
		}

		// ---- 3. the setting and the race ----
		{
			state::Fighter f = make();
			state::FighterPart* leg = part(f, "Left Leg");
			leg->health = -150.0;
			require(!rulesFor(0).trySever(f, *leg, true), "Dismemberment 0 (Never) severs nothing");
			require(!rules.trySever(f, *leg, false), "Dismemberment 1 has no passive path");
			require(!part(f, "Chest")->severed() && !rules.trySever(f, *part(f, "Chest"), true), "a vital part is never severed");
			state::Fighter g = make();
			part(g, "Left Leg")->health = -101.0;
			rulesFor(2).updateMedical(g, 1.0);
			require(part(g, "Left Leg")->severed(), "Dismemberment 2 severs by the passive sink");
			state::Fighter h = make();
			part(h, "Left Leg")->health = -101.0;
			rules.updateMedical(h, 1.0);
			require(!part(h, "Left Leg")->severed(), "Dismemberment 1 does not");
			// an animal: a race with forelegs
			state::Fighter a = make();
			part(a, "Left Leg")->health = -150.0;
			part(a, "Left Arm")->name = "Left Foreleg";
			require(!rules.trySever(a, *part(a, "Left Leg"), true), "an animal (forelegs) is never severed");
		}

		// ---- 4. the severed state survives save + load ----
		{
			Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
			sim.enableCombat(data, CombatSettings{});
			state::Fighter f = make();
			part(f, "Right Leg")->health = -104.5;
			rules.trySever(f, *part(f, "Right Leg"), true);
			const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_limb_leak.save";
			sim.saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path);
			loaded.enableCombat(data, CombatSettings{});
			const state::FighterPart* leg = nullptr;
			for (const state::FighterPart& p : loaded.combat()->find(id)->parts)
				leg = p.name == "Right Leg" ? &p : leg;
			require(leg != nullptr && leg->severed() && leg->health == -104.5 && loaded.combat()->find(id)->wounds.size() == 1,
				"a severed limb and its wound survive save + load");
			std::remove(path.c_str());
		}
		std::printf("ksim_limb_leak: pass\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
