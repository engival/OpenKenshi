// Task raid-food-loot (kenshi-h2v.21.2; kinspect rwh.53 sections 2.2 - 2.4, 4.6, rwh.52 "What the loot does with the item"): the in-town food
// loot of a raider. A member of a faction that is not the player's takes 131 LOOT_FOOD_AND_STUFF for the nearest food box of the town it
// stands in, whoever owns it (the exe's accept rule, 1408dac20), and 284's pull moves the food into its OWN inventory (the original
// loses it, rwh.52; ours does not, so killing the raider gets it back) - 1, 1, 2, 4 ... per pass - until its pack refuses the type or the
// box is empty (4.6). Fighting interleaves through the selector: an enemy takes the member off the box, and it returns after the fight.
// A bare dt27 world (no navigator): the member starts within reach of the box.

#include "dt27_rig.hpp"

#include "ksim/combat/combat_data.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/ai/routines.hpp"

namespace {

using namespace ksim;
using namespace dt27;

constexpr std::uint64_t kEnemySquad = 0x7778;
constexpr std::uint64_t kEnemyFaction = 9;
constexpr std::uint64_t kBoxId = 888001;

std::string foodItem(Rig& rig)
{
	std::string best;
	for (const auto& [sid, info] : rig.data->items)
		if (info.function == kItemFunctionFood && (best.empty() || sid < best))
			best = sid;
	test::require(!best.empty(), "no food item in the data");
	return best;
}

struct Scene
{
	std::unique_ptr<Rig> rig;
	flecs::entity box;
	std::string food;

	// A raider in a town beside the PLAYER faction's food box holding `units` of food.
	explicit Scene(int units, const std::vector<Row>& rows)
		: rig(std::make_unique<Rig>(rows, 0))
	{
		makeTown(*rig, 2, kX, kZ);
		food = foodItem(*rig);
		state::Allegiance side;
		side.factionSid = "raid-loot-test-raiders";
		for (const std::uint64_t id : {rig->leader, rig->member})
			rig->entityOf(id).set<state::Allegiance>(side);
		test::require(rig->sim.playerFactionSid() != side.factionSid && !rig->sim.playerFactionSid().empty(), "the raiders are the player faction");
		state::Container container;
		container.ownerSid = rig->sim.playerFactionSid();
		container.x = container.useX = kX + 6.0;
		container.z = container.useZ = kZ;
		container.y = container.useY = rig->fighterOf(rig->member).y;
		state::Inventory stock;
		stock.version = 1;
		stock.gridW = 40; // room for every unit: a box that cannot hold its own stock drops the overflow at its first settle
		stock.gridH = 40;
		stock.add(food, static_cast<std::uint32_t>(units));
		box = ecsOf(rig->sim).entity();
		box.set<state::Id>(state::Id{kBoxId});
		box.set<state::Container>(std::move(container));
		box.set<state::Inventory>(std::move(stock));
	}

	std::uint32_t stored() { return box.get<state::Inventory>()->carried(food); }
	std::uint32_t carried()
	{
		const state::Inventory* pack = rig->entityOf(rig->member).get<state::Inventory>();
		return pack != nullptr ? pack->carried(food) : 0;
	}
};

// The raider's pack takes no more of the food: the exe's char vf+0x170.
bool packFull(Scene& scene)
{
	const grid::Rules rules(*scene.rig->gd);
	state::Inventory probe;
	if (const state::Inventory* pack = scene.rig->entityOf(scene.rig->member).get<state::Inventory>())
		probe = *pack;
	state::Inventory::Entry one;
	one.itemSid = scene.food;
	one.count = 1;
	one.slot = state::Inventory::kCarried;
	one.section = state::Inventory::Entry::kUnplaced;
	return grid::autoplace(probe, rules, one) != 0;
}

// A store that is not mine: the raider takes all of it into its own inventory, then the goal ends.
void drainsTheStore()
{
	Scene scene(5, {{routines::task::LootFoodAndStuff}});
	test::require(scene.rig->until(60.0, [&] { return scene.stored() == 0; }), "131: the raider left " + std::to_string(scene.stored()) + " of 5 in the box");
	test::require(scene.carried() == 5, "131: the raider carries " + std::to_string(scene.carried()) + " of the 5, the rest was lost");
	test::require(scene.rig->logged("empty 131 member"), "131: no pull line was logged");
	test::require(scene.rig->until(5.0, [&] { return scene.rig->goalOf(scene.rig->member) == 0; }), "131: the goal outlived the empty box");
	std::cout << "  131: a raider emptied the player's box (5 -> 0) into its own pack\n";
}

// More food than the pack holds: the raider loads up its whole inventory and stops, the rest stays in the box.
void fillsThePack()
{
	Scene scene(60, {{routines::task::LootFoodAndStuff}});
	test::require(scene.rig->until(120.0, [&] { return packFull(scene); }), "131: the raider's pack never filled (carries " + std::to_string(scene.carried()) + ")");
	test::require(scene.rig->until(10.0, [&] { return scene.rig->goalOf(scene.rig->member) == 0; }), "131: the goal outlived a full pack");
	test::require(scene.stored() > 0 && scene.carried() > 0, "131: the box was not left with the rest");
	test::require(scene.stored() + scene.carried() == 60, "131: food was lost: " + std::to_string(scene.stored() + scene.carried()) + " of 60");
	std::cout << "  131: a raider filled its pack (" << scene.carried() << " carried, " << scene.stored() << " left in the box)\n";
}

// An enemy mid-loot takes the member off the box; after the fight it goes back to looting.
void fightInterleaves()
{
	Scene scene(6, {{routines::task::LootFoodAndStuff}, {routines::task::AttackEnemies, 0, 24, 50}}); // 6 units: what the pack holds
	Rig& rig = *scene.rig;
	test::require(rig.until(30.0, [&] { return scene.stored() < 6; }), "131: the loot never began");
	const std::uint32_t before = scene.stored();
	test::require(before > 0, "131: the box emptied before the fight");
	const std::uint64_t enemy = rig.spawn(rig.body, kX + 14.0, kZ + 4.0);
	rig.fighterOf(enemy).team = 1;
	rig.fighterOf(enemy).faction = kEnemyFaction;
	rig.fighterOf(enemy).squad = kEnemySquad;
	rig.fighterOf(rig.member).hostileSquads = {kEnemySquad};
	state::SightRecord seen;
	seen.target = enemy;
	seen.seen = true;
	rig.fighterOf(rig.member).sight.clear();
	rig.fighterOf(rig.member).sight.push_back(seen);
	test::require(rig.until(10.0, [&] { return rig.fighterOf(rig.member).target == enemy || rig.goalOf(rig.member) == routines::task::AttackEnemies; }),
		"16: the raider did not turn on the enemy mid-loot");
	const std::uint32_t during = scene.stored();
	rig.tick(40);
	test::require(rig.fighterOf(rig.member).target == enemy || rig.goalOf(rig.member) == routines::task::AttackEnemies || scene.stored() < during,
		"16: the fight did not hold the raider");
	// The enemy is gone: the loot goes on to the end.
	rig.fighterOf(enemy).dead = true;
	rig.fighterOf(rig.member).sight.clear();
	rig.fighterOf(rig.member).target = 0;
	test::require(rig.until(60.0, [&] { return scene.stored() == 0; }), "131: the raider never went back to the box after the fight (" + std::to_string(scene.stored()) + " left)");
	test::require(scene.carried() == 6, "131: the raider carries " + std::to_string(scene.carried()) + " of the 6");
	std::cout << "  16 / 131: an enemy took the raider off the box (" << before << " in it), it went back and emptied it\n";
}

// 243 and the box of my own faction keep their rules: a store of the raider's own faction is the classic pull (the goal ends once it holds some).
void ownBoxKeepsClassicRule()
{
	Scene scene(20, {{routines::task::LootFoodAndStuff}});
	state::Container container = *scene.box.get<state::Container>();
	container.ownerSid = "raid-loot-test-raiders";
	scene.box.set<state::Container>(std::move(container));
	test::require(scene.rig->until(60.0, [&] { return scene.carried() > 0; }), "131: the member did not take from its own faction's box");
	test::require(scene.rig->until(20.0, [&] { return scene.rig->goalOf(scene.rig->member) == 0; }), "131: the classic goal did not end on holding some");
	test::require(scene.stored() > 0, "131: an own-faction box was drained like a raid");
	std::cout << "  131: an own-faction box keeps the classic rule (" << scene.carried() << " taken, " << scene.stored() << " left)\n";
}

} // namespace

int main()
{
	try
	{
		drainsTheStore();
		fillsThePack();
		fightInterleaves();
		ownBoxKeepsClassicRule();
		std::cout << "ksim_raid_loot: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_raid_loot: FAILED: " << e.what() << "\n";
		return 1;
	}
}
