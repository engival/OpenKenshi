// Animals eat on their own (task animal-feed, 2026-11-08; kinspect dt24_eat section 9: FUN_1405d29c0, the only search that
// reaches another character's inventory). StepMeals' food sources, in the routine's order, all with no walk (the food
// teleports - one unit leaves the source):
//   1. the animal's own main inventory;
//   2. its own worn backpack's contents;
//   3. a squadmate's worn backpack only (never its main inventory), and only with both switches on (rwh.203);
//   4. an outdoor storage box that holds food (the exe's food finder rejects a building with an interior for an animal, so food
//      kept indoors is not eaten: that is the negative case here).
// The exe's two switches (DAT_142134690+0x114 Feed Animals, +0x115 Share Food) default OFF; ours are WorldOptions::feedAnimals / shareFood (Simulation::setGameOption).
// A bare world (Simulation::load), no AI and no navmesh: an inventory, a squad, a box and stepMeals are all the fixture needs.

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// A point outside the Hub's buildings (the animal and the outdoor box stand here; the test checks the box's spot is outdoors).
constexpr double kX = -50978.8;
constexpr double kZ = 2932.48;

std::string characterSid(const kdata::GameData& gd, std::string_view name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("ANIMAL_CHARACTER")))
		if (record->name == name)
			return record->stringID;
	throw std::runtime_error("no ANIMAL_CHARACTER " + std::string(name));
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	Simulation sim;
	std::uint64_t rng = 11;
	std::string animalSid;
	std::string food;
	std::string bread;
	std::string backpack;
	std::uint64_t animal = 0;
	std::uint64_t mate = 0;
	flecs::entity squad;

	Rig()
		: gd(std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA)))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::load(KENSHI_DATA, 91))
	{
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		animalSid = characterSid(*gd, "Beak Thing");
		// The food records: the first FOOD and the first FOOD_RESTRICTED the data sells (the exe takes the restricted kind
		// first, dt24_eat section 9).
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
		{
			const ItemInfo* info = data->item(record->stringID);
			if (info == nullptr || info->charges <= 0.0)
				continue;
			if (bread.empty() && info->function == kItemFunctionFood)
				bread = record->stringID;
			if (food.empty() && info->function == kItemFunctionFoodRestricted)
				food = record->stringID;
		}
		test::require(!bread.empty(), "the data has a FOOD item");
		if (food.empty())
			food = bread;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("CONTAINER")))
		{
			if (record->intOr("slot", 7) == 12) // AttachSlot BACKPACK (inventory_orders.cpp kBackpackAttach)
			{
				backpack = record->stringID;
				break;
			}
		}
		test::require(!backpack.empty(), "the data has a CONTAINER worn in the backpack slot");
	}

	void tick(int n)
	{
		for (int i = 0; i < n; ++i)
			sim.tick();
	}

	std::uint64_t spawn(const std::string& sid, double x, double z)
	{
		state::Fighter f = data->makeFighter(*gd, sid, "", rng);
		f.x = x;
		f.z = z;
		f.y = 1647.0;
		f.team = 0;
		f.hunger = 3.0; // fed: it eats nothing until a case says so
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& key, const state::Fighter&)
		{
			if (key.value == id)
				found = e;
		});
		return found;
	}
	state::Fighter& fighterOf(std::uint64_t id) { return *const_cast<CombatArena*>(sim.combat())->find(id); }

	void invent(std::uint64_t id, const std::vector<std::pair<std::string, std::uint32_t>>& items)
	{
		state::Inventory inventory = *entityOf(id).get<state::Inventory>();
		inventory.items.clear();
		inventory.version = 1;
		for (const auto& [item, count] : items)
			inventory.add(item, count, state::Inventory::kCarried);
		entityOf(id).set<state::Inventory>(std::move(inventory));
	}

	// A worn backpack holding `count` of the food.
	void wearBackpackWithFood(std::uint64_t id, std::uint32_t count)
	{
		state::Inventory inventory = *entityOf(id).get<state::Inventory>();
		inventory.items.clear();
		inventory.version = 1;
		inventory.add(backpack, 1, state::Inventory::kBackpackSlot);
		state::Inventory::Entry* bag = inventory.wornBackpack();
		test::require(bag != nullptr, "the backpack is worn");
		bag->contents.clear();
		bag->contents.push_back({food, count, state::Inventory::kCarried});
		entityOf(id).set<state::Inventory>(std::move(inventory));
	}

	std::uint32_t carried(std::uint64_t id, const std::string& item)
	{
		const state::Inventory* inventory = entityOf(id).get<state::Inventory>();
		return inventory != nullptr ? inventory->carried(item) : 0;
	}

	// Feeds a hungry animal: hunger at the bottom of the band, no bite in flight.
	void hungry(std::uint64_t id)
	{
		state::Fighter& f = fighterOf(id);
		f.hunger = 1.0;
		f.foodCharges = 0.0;
		f.eatTimer = 0.0;
		f.target = 0;
	}

	void setUp()
	{
		animal = spawn(animalSid, kX, kZ);
		mate = spawn(characterSid(*gd, "Beak Thing"), kX + 60.0, kZ);
		squad = ecsOf(sim).entity();
		entityOf(animal).add<state::MemberOf>(squad);
		entityOf(mate).add<state::MemberOf>(squad);
		// The mate is fed, so it never touches its own pack.
		fighterOf(mate).hunger = 3.0;
	}

	void clearSources()
	{
		invent(animal, {});
		invent(mate, {});
		state::Fighter& a = fighterOf(animal);
		a.foodCharges = 0.0;
		a.eatTimer = 0.0;
	}
};

void ownInventory(Rig& rig)
{
	rig.hungry(rig.animal);
	rig.invent(rig.animal, {{rig.food, 2}});
	rig.tick(2);
	test::require(rig.fighterOf(rig.animal).hunger > 1.0, "the animal does not eat its own food");
	test::require(rig.carried(rig.animal, rig.food) == 1, "the animal's own food did not lose one unit ("
		+ std::to_string(rig.carried(rig.animal, rig.food)) + " left)");
	std::cout << "  own inventory: hunger 1 -> " << rig.fighterOf(rig.animal).hunger << ", one unit eaten\n";
	rig.clearSources();
}

void ownBackpack(Rig& rig)
{
	rig.hungry(rig.animal);
	rig.wearBackpackWithFood(rig.animal, 2);
	rig.tick(2);
	test::require(rig.fighterOf(rig.animal).hunger > 1.0, "the animal does not eat from its own backpack");
	test::require(rig.carried(rig.animal, rig.food) == 1, "the animal's backpack did not lose one unit ("
		+ std::to_string(rig.carried(rig.animal, rig.food)) + " left)");
	std::cout << "  own backpack: hunger " << rig.fighterOf(rig.animal).hunger << ", one unit eaten\n";
	rig.clearSources();
}

void squadMate(Rig& rig)
{
	// Both switches default OFF (rwh.203): a squadmate's backpack food is not eaten.
	test::require(rig.sim.worldOptions() == nullptr || (!rig.sim.worldOptions()->feedAnimals && !rig.sim.worldOptions()->shareFood), "the two switches default off");
	rig.hungry(rig.animal);
	rig.wearBackpackWithFood(rig.mate, 2);
	rig.tick(30);
	test::require(rig.fighterOf(rig.animal).hunger <= 1.0 && rig.carried(rig.mate, rig.food) == 2, "the animal ate a squadmate's backpack with the switches off");
	rig.clearSources();
	std::cout << "  switches off: refused\n";
	// One switch alone is not enough.
	test::require(rig.sim.setGameOption("feedAnimals", 1.0).empty(), "set feedAnimals");
	rig.hungry(rig.animal);
	rig.wearBackpackWithFood(rig.mate, 2);
	rig.tick(30);
	test::require(rig.fighterOf(rig.animal).hunger <= 1.0 && rig.carried(rig.mate, rig.food) == 2, "the animal ate with Share Food off");
	rig.clearSources();
	test::require(rig.sim.setGameOption("shareFood", 1.0).empty(), "set shareFood");
	// A squadmate's MAIN inventory is never searched (the equipped backpack only).
	rig.hungry(rig.animal);
	rig.invent(rig.mate, {{rig.food, 2}});
	rig.tick(30);
	test::require(rig.fighterOf(rig.animal).hunger <= 1.0 && rig.carried(rig.mate, rig.food) == 2, "the animal ate a squadmate's main inventory");
	std::cout << "  squadmate's main inventory: refused\n";
	rig.clearSources();
	rig.hungry(rig.animal);
	rig.wearBackpackWithFood(rig.mate, 2);
	rig.tick(2);
	test::require(rig.fighterOf(rig.animal).hunger > 1.0, "the animal does not eat a squadmate's backpack food");
	test::require(rig.carried(rig.mate, rig.food) == 1, "the squadmate's backpack did not lose one unit ("
		+ std::to_string(rig.carried(rig.mate, rig.food)) + " left)");
	std::cout << "  squadmate's backpack (both switches on): one unit eaten\n";
	rig.clearSources();
}

std::uint64_t boxRig(Rig& rig, double x, double y, double z, const std::string& item, std::uint32_t count)
{
	state::Container box;
	box.x = box.useX = x;
	box.y = box.useY = y;
	box.z = box.useZ = z;
	state::Inventory stock;
	stock.add(item, count, state::Inventory::kCarried);
	return rig.sim.spawnContainer(std::move(box), std::move(stock)).get<state::Id>()->value;
}

flecs::entity entityById(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& key)
	{
		if (key.value == id)
			found = e;
	});
	return found;
}

std::uint32_t boxCount(Rig& rig, std::uint64_t id, const std::string& item)
{
	const state::Inventory* stock = entityById(rig.sim, id).get<state::Inventory>();
	return stock != nullptr ? stock->carried(item) : 0;
}

void outdoorBox(Rig& rig)
{
	// The outdoor spot: within 200 u of the animal and in no building (the animal rejects a building with an interior).
	const double y = rig.fighterOf(rig.animal).y;
	double bx = kX + 60.0;
	double bz = kZ;
	for (const double dz : {0.0, 40.0, -40.0, 80.0, -80.0})
	{
		bx = kX + 60.0;
		bz = kZ + dz;
		if (rig.sim.buildingAt(bx, y, bz) == 0)
			break;
	}
	test::require(rig.sim.buildingAt(bx, y, bz) == 0, "an outdoor spot for the food box");
	const std::uint64_t box = boxRig(rig, bx, y, bz, rig.food, 2);
	rig.hungry(rig.animal);
	rig.tick(2);
	test::require(rig.fighterOf(rig.animal).hunger > 1.0, "the animal does not eat from an outdoor food box");
	test::require(boxCount(rig, box, rig.food) == 1, "the outdoor box did not lose one unit ("
		+ std::to_string(boxCount(rig, box, rig.food)) + " left)");
	std::cout << "  outdoor box: one unit eaten\n";
	entityById(rig.sim, box).destruct();
	rig.clearSources();
}

void indoorBox(Rig& rig)
{
	// A building with an interior (its hull is the indoors trigger): the animal must not eat the food kept inside.
	const BuildingSite* indoor = nullptr;
	for (const BuildingSite& site : rig.sim.world().buildingSites())
	{
		if (!site.hasInterior || !site.hull || site.hullHalfA <= 0.0 || site.hullHalfB <= 0.0)
			continue;
		indoor = &site;
		break;
	}
	test::require(indoor != nullptr, "a building with an interior in the world model");
	const double y = (indoor->hullMinY + indoor->hullMaxY) * 0.5;
	const double bx = indoor->hullCx;
	const double bz = indoor->hullCz;
	test::require(rig.sim.buildingAt(bx, y, bz) == indoor->key, "the box's spot is inside the building");
	const std::uint64_t box = boxRig(rig, bx, y, bz, rig.food, 2);
	// The animal stands 100 u from the box (the reach is 200), outside the building.
	state::Fighter& f = rig.fighterOf(rig.animal);
	f.x = bx + 100.0;
	f.z = bz;
	f.y = y;
	rig.hungry(rig.animal);
	rig.tick(60);
	test::require(rig.fighterOf(rig.animal).hunger <= 1.0, "the animal ate food kept indoors");
	test::require(boxCount(rig, box, rig.food) == 2, "the indoor box lost food");
	std::cout << "  indoor box: refused (" << boxCount(rig, box, rig.food) << " units kept)\n";
	entityById(rig.sim, box).destruct();
	rig.clearSources();
}

} // namespace

int main()
{
	try
	{
		Rig rig;
		rig.setUp();
		test::require(rig.fighterOf(rig.animal).animal, "the fixture's eater is not an animal");
		std::cout << "ksim_animal_feed: " << rig.food << " (food)\n";
		ownInventory(rig);
		ownBackpack(rig);
		squadMate(rig);
		outdoorBox(rig);
		indoorBox(rig);
		std::cout << "ksim_animal_feed: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_animal_feed: FAILED: " << e.what() << "\n";
		return 1;
	}
}
