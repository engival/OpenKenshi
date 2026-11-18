// Indoors state per character (FEATURES.md "Indoors state per character"; kinspect indoors_state.md / dt24_enterleave.md): a
// character is indoors when it is in a building's interior AND under a ceiling, so a roof / top deck inside the footprint is
// outdoors. Fighter::indoors is the flag (CHAR_DYNAMIC flag 19, save v85).
//
//  1. the Hub Bar (3384-TwoStorey, placement 4933): the ground floor is indoors, the roof deck is outdoors, the open ground is
//     outdoors, and the porch (inside the placement's footprint, outside its interior mask hull) is outdoors;
//  2. acid rain (a WEATHER with `affect type` 2): the same three spots, stated as "burns" or "does not burn" (the user's Deadlands
//     observation: no burn inside the bar, a burn on its roof), plus the worn gear's weather protection (P >= 1 = none) and a
//     race's own `weather immunity0..N` type set (a Hive Soldier Drone lists 2: no burn bare);
//  3. the weapon `indoors mod` (indoors_state.md section 2): a Fragment Axe (-6) lowers attack and defence inside only, and a
//     Nodachi (+4 attack, -4 indoors) is zeroed first, so inside it is 8 lower (the exe's floor).
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr double kBarX = -51181.0; // the Hub Bar (3384-TwoStorey, placement 4933); floor 1 is y 1647 (ksim_native_nav)
constexpr double kBarZ = 2639.0;
constexpr double kRampX = -51085.0; // open ground east of the Hub buildings
constexpr double kRampZ = 3145.0;

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;
	std::uint32_t seq = 0;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, const kworld::LoadOrder& order)
		: gd(g)
		, data(d)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		// Nobody else in the arena, so the test reads one character.
		sim->removeFighter(duel.enemy);
		// The first look at a fighter only notes where it stands (stepBuildings); a move is what it tests, so every put() below
		// needs this one tick behind it.
		sim->tick();
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	CombatArena& arena() { return *const_cast<CombatArena*>(sim->combat()); }
	state::Fighter& player() { return *arena().find(duel.player); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	void put(double x, double y, double z)
	{
		state::Fighter& p = player();
		p.x = x;
		p.y = y;
		p.z = z;
	}
	void groundY(double x, double z, double near, double& y)
	{
		const auto spot = nav->walker().snap({x, near, z}, 60.0);
		test::require(spot.has_value(), "no mesh near " + std::to_string(x) + ", " + std::to_string(z));
		y = spot->y;
	}
};

// The acid drain lands in the wear pool (the blunt channel, k = 0; weather_acid_rain.md section 4), so the sum of the parts'
// wear is what "burns".
double wearSum(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& p : f.parts)
		sum += p.wear;
	return sum;
}

// "burns" / "does not burn" at one spot: wear sums before and after 2 s of acid rain (0.08 HP/s per organic part, no protection).
void burnCheck(Rig& rig, const std::string& where, double x, double y, double z, bool burns)
{
	rig.put(x, y, z);
	rig.tick(3); // the placement step sets `indoors` / `sheltered`, the next arena steps burn by them
	const double before = wearSum(rig.player());
	rig.tick(120);
	const double after = wearSum(rig.player());
	std::cout << where << ": wear " << before << " -> " << after << (burns ? " (burns)" : " (does not burn)") << "\n";
	if (burns)
		test::require(after > before + 1.0e-4, where + " does not burn");
	else
		test::require(after <= before + 1.0e-9, where + " burns");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_indoors HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// ---- 1. the Bar: ground floor, roof deck, outside, the porch ----
		double groundY = 0.0;
		double porchX = 0.0;
		double porchZ = 0.0;
		{
			Rig rig(gd, data, nav, order);
			const BuildingSite* bar = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
				if (std::hypot(site.cx - kBarX, site.cz - kBarZ) < 30.0)
					bar = &site;
			test::require(bar != nullptr, "no building site at the Hub Bar");
			test::require(bar->hull, "the Bar has no interior mask hull");
			test::require(bar->floors.size() == 1, "the Bar does not report one floor above the ground");
			std::cout << "bar: key " << bar->key << ", origin " << bar->originY << ", maxY " << bar->maxY << ", roof " << bar->floors.front()
				<< ", hull y " << bar->hullMinY << ".." << bar->hullMaxY << "\n";

			rig.groundY(kRampX, kRampZ, 1530.0, groundY);
			// The ground floor: inside the hull, with the roof deck above.
			rig.put(bar->hullCx, bar->hullMinY + 5.0, bar->hullCz);
			rig.tick(3);
			test::require(rig.player().building == bar->key, "inside the Bar: building " + std::to_string(rig.player().building));
			test::require(rig.player().floor == 0, "inside the Bar is not floor 0: " + std::to_string(rig.player().floor));
			test::require(rig.player().indoors, "the Bar's ground floor is not indoors");
			// The roof deck: the top walkable surface has nothing above it but the sky.
			rig.put(bar->hullCx, bar->floors.front(), bar->hullCz);
			rig.tick(3);
			test::require(rig.player().building == bar->key, "on the Bar roof: building " + std::to_string(rig.player().building));
			test::require(rig.player().floor == 1, "the Bar roof is not floor 1: " + std::to_string(rig.player().floor));
			test::require(!rig.player().indoors, "the Bar roof is indoors");
			// Outside: no building at all.
			rig.put(kRampX, groundY, kRampZ);
			rig.tick(3);
			test::require(rig.player().building == 0, "outside: building " + std::to_string(rig.player().building));
			test::require(!rig.player().indoors, "the open ground is indoors");
			// The porch: inside the placement's footprint (roof overhang included) but outside the interior mask, so no building:
			// under a porch roof is outdoors, like a roof deck (the client checks the same spot live).
			const double y = bar->hullMinY + 5.0;
			for (double a = -bar->halfA; a <= bar->halfA && porchX == 0.0; a += 0.5)
				for (double b = -bar->halfB; b <= bar->halfB && porchX == 0.0; b += 0.5)
				{
					const double x = bar->cx + bar->axisX * a - bar->axisZ * b;
					const double z = bar->cz + bar->axisZ * a + bar->axisX * b;
					if (insideFootprint(*bar, {x, z}) && rig.sim->buildingAt(x, y, z) == 0)
					{
						porchX = x;
						porchZ = z;
					}
				}
			test::require(porchX != 0.0 || porchZ != 0.0, "no point of the Bar's footprint is outside its interior mask");
			rig.put(porchX, y, porchZ);
			rig.tick(3);
			test::require(rig.player().building == 0, "the porch is a building: " + std::to_string(rig.player().building));
			test::require(!rig.player().indoors, "the porch (outside the interior mask) is indoors");
		}

		// ---- 2. acid rain: burns on the roof and outside, does not burn under the ceiling ----
		{
			Rig rig(gd, data, nav, order);
			const BuildingSite* bar = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
				if (std::hypot(site.cx - kBarX, site.cz - kBarZ) < 30.0)
					bar = &site;
			test::require(bar != nullptr, "no building site at the Hub Bar");
			// Acid rain everywhere (WEATHER `affect type` 2, `effect strength` 1): the environment asks the weather system by
			// position; the test answers for every spot, so the gate under test is Fighter::indoors alone.
			rig.arena().environmentMutable().weatherAt = [](double, double, int& type, double& strength)
			{
				type = 2;
				strength = 1.0;
			};
			burnCheck(rig, "the Bar's ground floor", bar->hullCx, bar->hullMinY + 5.0, bar->hullCz, false);
			burnCheck(rig, "the Bar's roof", bar->hullCx, bar->floors.front(), bar->hullCz, true);
			burnCheck(rig, "the open ground", kRampX, groundY, kRampZ, true);
			// The gear's protection (weather_acid_rain.md section 3): a Dustcoat (0.8) and an Ashland Hat (0.5) both list type 2,
			// so P = 1.3 -- more than total protection, and the exposed character does not burn.
			const kdata::Record* dustcoat = gd->find("3061-gamedata.base");
			const kdata::Record* hat = gd->find("55393-rebirth.mod");
			test::require(dustcoat != nullptr && hat != nullptr, "no Dustcoat / Ashland Hat ARMOUR record");
			state::Fighter& p = rig.player();
			p.armour.clear();
			p.armour.push_back(data->makeArmour(*dustcoat, 1.0));
			p.armour.push_back(data->makeArmour(*hat, 1.0));
			double protection = 0.0;
			for (const state::FighterArmour& a : p.armour)
				if ((a.weatherTypes & (1u << 2)) != 0)
					protection += a.weatherProtection;
			test::require(protection > 1.0, "the Dustcoat + Ashland Hat weather protection is " + std::to_string(protection));
			burnCheck(rig, "the open ground in acid gear", kRampX, groundY, kRampZ, false);
			// The race's own immunity (RACE `weather immunity0..N`, the VALUE is the affect type): a Hive Soldier Drone lists 2,
			// so acid rain does not burn it, bare. A Greenlander lists only 0 (skipped), so the same spot burns again.
			p.armour.clear();
			const RaceCombat* drone = data->race("18961-gamedata.base");
			test::require(drone != nullptr, "no Hive Soldier Drone RACE record");
			test::require((drone->weatherImmunity & (1u << 2)) != 0, "the Hive Soldier Drone is not immune to acid (type 2)");
			p.raceSid = drone->sid;
			burnCheck(rig, "the open ground as a Hive Soldier Drone", kRampX, groundY, kRampZ, false);
			p.raceSid = "17-gamedata.quack";
			burnCheck(rig, "the open ground as a Greenlander", kRampX, groundY, kRampZ, true);
			// The shelter gate (weather_acid_rain.md section 5): a roof overhead belonging to an `is shelter` building stops it. A
			// spot inside such a building's footprint, outside any interior mask, with the roof more than 30 u up.
			const BuildingSite* shelter = nullptr;
			double shelterX = 0.0;
			double shelterZ = 0.0;
			double shelterY = 0.0;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
			{
				if (!site.shelter || shelter != nullptr)
					continue;
				for (double a = -site.halfA; a <= site.halfA && shelter == nullptr; a += 1.0)
					for (double b = -site.halfB; b <= site.halfB && shelter == nullptr; b += 1.0)
					{
						const double x = site.cx + site.axisX * a - site.axisZ * b;
						const double z = site.cz + site.axisZ * a + site.axisX * b;
						if (!insideFootprint(site, {x, z}) || site.maxY < site.minY + 2.0 * kRoofClearance)
							continue;
						const double y = site.minY + 5.0;
						if (rig.sim->buildingAt(x, y, z) == 0)
						{
							shelter = &site;
							shelterX = x;
							shelterZ = z;
							shelterY = y;
						}
					}
			}
			if (shelter != nullptr)
			{
				std::cout << "shelter building " << shelter->buildingSid << " at " << shelterX << ", " << shelterZ << "\n";
				burnCheck(rig, "under a shelter roof", shelterX, shelterY, shelterZ, false);
			} else {
				std::cout << "no is-shelter building with an open spot in the Hub\n";
			}
		}

		// ---- 3. the weapon indoors mod: attack and defence, inside only, with the attack-side floor ----
		{
			Rig rig(gd, data, nav, order);
			const BuildingSite* bar = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
				if (std::hypot(site.cx - kBarX, site.cz - kBarZ) < 30.0)
					bar = &site;
			test::require(bar != nullptr, "no building site at the Hub Bar");
			state::Fighter& p = rig.player();
			p.armour.clear(); // the equipment bonus is the weapon's alone, so the numbers below are exact
			const auto equip = [&](const char* sid, double indoorsMod, double attackMod)
			{
				const kdata::Record* record = gd->find(sid);
				test::require(record != nullptr, std::string("no WEAPON record ") + sid);
				p.weapon = data->makeWeapon(*record, 0.5);
				test::require(std::fabs(p.weapon.indoorsMod - indoorsMod) < 1.0e-9,
					std::string("the weapon's indoors mod: ") + std::to_string(p.weapon.indoorsMod));
				test::require(std::fabs(p.weapon.attackMod - attackMod) < 1.0e-9,
					std::string("the weapon's attack mod: ") + std::to_string(p.weapon.attackMod));
			};
			const CombatRules& rules = rig.arena().rules();
			const auto values = [&](double x, double y, double z, double& attack, double& defence)
			{
				rig.put(x, y, z);
				rig.tick(3);
				attack = rules.attackValue(p);
				defence = rules.defenceValue(p);
			};
			double outsideAttack = 0.0;
			double outsideDefence = 0.0;
			double insideAttack = 0.0;
			double insideDefence = 0.0;

			// A Fragment Axe: attack mod 0, defence mod 0, indoors mod -6. Nothing to zero, so the mod is the whole difference.
			equip("477-gamedata.base", -6.0, 0.0);
			values(kRampX, groundY, kRampZ, outsideAttack, outsideDefence);
			values(bar->hullCx, bar->hullMinY + 5.0, bar->hullCz, insideAttack, insideDefence);
			std::cout << "fragment axe: attack " << outsideAttack << " -> " << insideAttack << ", defence " << outsideDefence << " -> "
				<< insideDefence << "\n";
			test::require(std::fabs(insideAttack - (outsideAttack - 6.0)) < 1.0e-9, "the indoors mod is not -6 on attack");
			test::require(std::fabs(insideDefence - (outsideDefence - 6.0)) < 1.0e-9, "the indoors mod is not -6 on defence");

			// A Nodachi: attack mod +4, indoors mod -4. Inside, the +4 is zeroed first and the -4 added (the exe's attack floor),
			// so inside is 8 below outside. Defence has no defence mod: the mod is added plain.
			equip("922-gamedata.base", -4.0, 4.0);
			values(kRampX, groundY, kRampZ, outsideAttack, outsideDefence);
			values(bar->hullCx, bar->hullMinY + 5.0, bar->hullCz, insideAttack, insideDefence);
			std::cout << "nodachi: attack " << outsideAttack << " -> " << insideAttack << ", defence " << outsideDefence << " -> "
				<< insideDefence << "\n";
			test::require(std::fabs(insideAttack - (outsideAttack - 8.0)) < 1.0e-9, "the attack floor did not zero the +4 before -4");
			test::require(std::fabs(insideDefence - (outsideDefence - 4.0)) < 1.0e-9, "the defence path adds the mod with a floor");
		}

		std::cout << "ksim_indoors: all checks passed\n";
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "ksim_indoors: " << e.what() << "\n";
		return 1;
	}
}
