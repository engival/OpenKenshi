// 131 LOOT_FOOD_AND_STUFF, 188 WORK_THE_SLAVES and 245 GATHER_SLAVES_FROM_CAGES_IF_FEMALE_OR_BEAST (task ds/slave-loot; kinspect
// dt24_131.md sections 1, 4, 7). 131 is 243's twin: the same food-box finder, goal HAVE_SOME_LOOT_FROM, plan [67, 284, 131] and 284's
// pull; 188 speaks the work dialogue at a random owned slave (plan 161); 245 is 217's gather over the female or other-race occupants of
// my town's cages (plan 174). Bare worlds: a package squad with the row takes the goal and runs its plan.

#include "dt27_rig.hpp"

#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"

#include <algorithm>

namespace {

using namespace ksim;
using namespace dt27;

std::string planText(const state::Routine& routine)
{
	std::string out;
	for (const std::uint16_t task : routine.plan)
		out += (out.empty() ? "" : " ") + std::to_string(task);
	return out;
}

std::uint32_t carriedBy(Rig& rig, std::uint64_t id, const std::string& item)
{
	const state::Inventory* pack = rig.entityOf(id).get<state::Inventory>();
	return pack != nullptr ? pack->carried(item) : 0;
}

void lootsFoodBox()
{
	Rig rig({{routines::task::LootFoodAndStuff}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::LootFoodAndStuff), "no AI_PACKAGE carries 131");
	std::string food;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == kItemFunctionFood)
		{
			food = record->stringID;
			break;
		}
	test::require(!food.empty(), "no food item in the data");
	state::Allegiance side;
	side.factionSid = "slave-loot-test-faction";
	for (const std::uint64_t id : {rig.leader, rig.member})
		rig.entityOf(id).set<state::Allegiance>(side);
	// A box of my faction holding food, the member beside it (a bare world has no navigator to walk it there).
	state::Container container;
	container.ownerSid = side.factionSid;
	container.x = container.useX = kX + 6.0;
	container.z = container.useZ = kZ;
	container.y = container.useY = rig.fighterOf(rig.member).y;
	state::Inventory stock;
	stock.version = 1;
	stock.gridW = 8;
	stock.gridH = 10;
	stock.add(food, 5);
	flecs::entity box = ecsOf(rig.sim).entity();
	box.set<state::Id>(state::Id{777001});
	box.set<state::Container>(std::move(container));
	box.set<state::Inventory>(std::move(stock));

	state::Routine seen;
	bool took = false;
	for (int i = 0; i < 400 && !took; ++i)
	{
		rig.tick(1);
		if (const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>(); r != nullptr && r->goal == routines::task::LootFoodAndStuff)
			seen = *r;
		took = box.get<state::Inventory>()->carried(food) < 5;
	}
	test::require(seen.goal == routines::task::LootFoodAndStuff, "131: the member did not take LOOT_FOOD_AND_STUFF for a food box of its faction");
	test::require(std::find(seen.plan.begin(), seen.plan.end(), routines::task::LootContainer) != seen.plan.end()
		&& seen.plan.back() == routines::task::LootFoodAndStuff, "131: the plan is not [.. 284, 131] (" + planText(seen) + ")");
	test::require(seen.chainTarget == 777001, "131: the plan's hand is not the box");
	test::require(took, "131: nothing was taken from the box");
	test::require(carriedBy(rig, rig.member, food) > 0, "131: the loot is not in the member's pack");
	test::require(rig.logged("empty 131 member"), "131: no pull line was logged");
	std::cout << "  131: took " << carriedBy(rig, rig.member, food) << " x food from the box (plan " << planText(seen) << ")\n";
}

void worksTheSlaves()
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	test::require(dataHasRow(*rig.gd, routines::task::WorkTheSlaves), "no AI_PACKAGE carries 188");
	// With no slave of mine the goal has no target: not taken.
	rig.tick(160);
	test::require(rig.goalOf(rig.member) != routines::task::WorkTheSlaves, "188: taken with no slave to work");
	const std::uint64_t slave = rig.spawn(rig.body, kX + 3.0, kZ + 3.0);
	state::Fighter& s = rig.fighterOf(slave);
	s.slaveState = 1;
	s.followTarget = rig.member;
	test::require(rig.until(10.0, [&] { return rig.goalOf(rig.member) == routines::task::WorkTheSlaves; }),
		"188: the member did not take WORK_THE_SLAVES for the slave following it");
	const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>();
	test::require(r != nullptr && r->chainTarget == slave && r->plan == std::vector<std::uint16_t>{161},
		"188: the routine's hand or plan is wrong");
	test::require(rig.until(10.0, [&] { return rig.logged("work 188 member"); }), "188: no work dialogue was delivered");
	test::require(rig.until(3.0, [&] { return rig.goalOf(rig.member) != routines::task::WorkTheSlaves; }), "188: the goal outlived the dialogue");
	std::cout << "  188: spoke the work dialogue to its slave (plan 161)\n";
}

void gathersFemaleOrBeast()
{
	// The female occupant's sex comes from the spawn roll (CombatData::rollFemale), not a hand-set flag: a CHARACTER with female chance 100.
	std::string femaleCharacter;
	{
		Rig probe({}, 0);
		for (const kdata::Record* record : probe.gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (record->intOr("female chance", 0) != 100)
				continue;
			try
			{
				std::uint64_t state = 3;
				if (probe.data->makeFighter(*probe.gd, record->stringID, "", state).look.female == 1)
				{
					femaleCharacter = record->stringID;
					break;
				}
			} catch (const std::exception&) {
			}
		}
	}
	test::require(!femaleCharacter.empty(), "no CHARACTER with female chance 100 spawns female");
	const auto scene = [&](Rig& rig, bool female, bool otherRace)
	{
		const BuildingSite* cage = findCageBuilding(rig);
		test::require(cage != nullptr, "no building with a cage layout exists");
		makeTown(rig, 2, cage->cx, cage->cz);
		for (const std::uint64_t id : {rig.leader, rig.member})
		{
			state::Fighter& f = rig.fighterOf(id);
			f.x = cage->cx + (id == rig.member ? 2.0 : -8.0);
			f.z = cage->cz + 1.5;
			f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
		}
		const std::uint64_t captive = rig.spawn(rig.body, cage->cx, cage->cz);
		state::Fighter& victim = rig.fighterOf(captive);
		victim.y = rig.sim.groundHeight(victim.x, victim.z).value_or(victim.y);
		victim.cageBuilding = cage->key;
		victim.cageSlot = 0;
		victim.slaveState = 1;
		victim.look.female = 0; // the body template may roll female itself
		if (female)
		{
			std::uint64_t state = captive;
			victim.look.female = rig.data->makeFighter(*rig.gd, femaleCharacter, "", state).look.female;
		}
		if (otherRace)
			victim.raceSid = victim.raceSid + "-other";
		return std::make_pair(captive, cage->key);
	};
	{
		Rig rig({{routines::task::GatherSlavesFemaleOrBeast}}, 0);
		test::require(dataHasRow(*rig.gd, routines::task::GatherSlavesFemaleOrBeast), "no AI_PACKAGE carries 245");
		const auto [captive, cageKey] = scene(rig, false, false); // same race, not female: refused
		rig.tick(240);
		test::require(rig.goalOf(rig.member) != routines::task::GatherSlavesFemaleOrBeast, "245: a male same-race occupant was gathered");
		test::require(rig.fighterOf(captive).cageBuilding == cageKey, "245: a male same-race occupant left its cage");
	}
	for (const bool female : {true, false})
	{
		Rig rig({{routines::task::GatherSlavesFemaleOrBeast}}, 0);
		const auto [captive, cageKey] = scene(rig, female, !female); // a female of my race, or a male of another
		(void)cageKey;
		test::require(rig.until(5.0, [&] { return rig.goalOf(rig.member) == routines::task::GatherSlavesFemaleOrBeast; }),
			std::string("245: the member did not take the goal for a ") + (female ? "female" : "other-race") + " occupant");
		const state::Routine* r = rig.entityOf(rig.member).get<state::Routine>();
		test::require(r != nullptr && r->plan == std::vector<std::uint16_t>{174}, "245: the plan is not [174]");
		test::require(rig.until(20.0, [&] { return rig.fighterOf(captive).cageBuilding == 0; }), "245: the occupant never left its cage");
		test::require(rig.fighterOf(captive).followTarget == rig.member, "245: the gathered slave does not follow its taker");
	}
	std::cout << "  245: a female or other-race occupant was taken, a male of my race left\n";
}

} // namespace

int main()
{
	try
	{
		lootsFoodBox();
		worksTheSlaves();
		gathersFemaleOrBeast();
		std::cout << "ksim_slave_loot: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_slave_loot: FAILED: " << e.what() << "\n";
		return 1;
	}
}
