// The construction tasks (task ds/construction; kinspect b50c_static Q6-Q10 Q18, dt24_enterleave, dt24_plantasks): 71 adds materials (a 5 s
// warm-up, then one unit per missing kind per tick), 2 BUILD / 95 REPAIR add skill-scaled progress capped by the delivered materials, 96
// dismantles (refused over furniture or mounted buildings, refunds delivered material), 156 / 200 set and clear the interior, 97 trains
// at a dummy. The ruin / dismantled / furniture / mounted fields are saved (v88). Bare worlds: the actor stands beside the building.

#include "dt27_rig.hpp"

#include "ksim/world/buildings.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/ai/routines.hpp"

#include <algorithm>
#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;

std::vector<std::string> resourceItems(Rig& rig, std::size_t want)
{
	std::vector<std::string> items;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == 0 && items.size() < want)
			items.push_back(record->stringID);
	test::require(items.size() == want, "the data lacks plain resource ITEMs");
	return items;
}

std::uint64_t playerFaction(Rig& rig)
{
	std::uint64_t id = 0;
	ecsOf(rig.sim).each([&](const state::Id& key, const state::PlayerFaction&) { id = id == 0 ? key.value : std::min(id, key.value); });
	return id;
}

// Both rig characters join the player's faction.
void joinPlayer(Rig& rig)
{
	std::string sid;
	ecsOf(rig.sim).each([&](const state::PlayerFaction&, const state::FactionState& state) { sid = state.templateSid; });
	test::require(!sid.empty(), "the rig has no player faction");
	state::Allegiance side;
	side.factionSid = sid;
	for (const std::uint64_t id : {rig.leader, rig.member})
		rig.entityOf(id).set<state::Allegiance>(side);
}

std::uint64_t makeSite(Rig& rig, double dx, std::vector<state::BuildMaterial> mats, double hp, bool built, const std::string& record = "")
{
	state::PlayerBuilding b;
	b.recordSid = record;
	b.ownerFaction = playerFaction(rig);
	b.x = kX + dx;
	b.z = kZ;
	b.y = rig.fighterOf(rig.member).y;
	b.built = built;
	b.hp = hp;
	b.maxHp = 0.0;
	for (const state::BuildMaterial& row : mats)
		b.maxHp += row.need;
	b.mats = std::move(mats);
	b.status = buildings::statusOf(b);
	flecs::entity entity = state::createInstance(ecsOf(rig.sim));
	const std::uint64_t id = entity.get<state::Id>()->value;
	entity.set<state::PlayerBuilding>(std::move(b));
	return id;
}

flecs::entity siteEntity(Rig& rig, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(rig.sim).each([&](flecs::entity e, const state::Id& key, const state::PlayerBuilding&)
	{
		if (key.value == id)
			found = e;
	});
	return found;
}
const state::PlayerBuilding& siteOf(Rig& rig, std::uint64_t id) { return *siteEntity(rig, id).get<state::PlayerBuilding>(); }

void give(Rig& rig, std::uint64_t who, const std::string& item, std::uint32_t count)
{
	state::Inventory pack;
	if (const state::Inventory* packed = rig.entityOf(who).get<state::Inventory>())
		pack = *packed;
	state::Inventory::Entry entry;
	entry.itemSid = item;
	entry.count = count;
	test::require(grid::autoplace(pack, grid::Rules(*rig.gd), entry) == 0, "the items did not fit the pack");
	++pack.version;
	rig.entityOf(who).set<state::Inventory>(pack);
}

std::uint32_t carried(Rig& rig, std::uint64_t who, const std::string& item)
{
	const state::Inventory* pack = rig.entityOf(who).get<state::Inventory>();
	return pack != nullptr ? pack->carried(item) : 0;
}

std::uint32_t onGround(Rig& rig, const std::string& item)
{
	std::uint32_t total = 0;
	ecsOf(rig.sim).each([&](const state::GroundItem& g)
	{
		if (g.itemSid == item)
			total += g.count;
	});
	return total;
}

void addMaterials()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 2);
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 3, 0}, {items[1], 2, 0}}, 0.0, false);
	give(rig, rig.member, items[0], 3);
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::AddMaterials, site), "71: the order was refused");
	rig.tick(90);
	test::require(siteOf(rig, site).mats[0].have == 0, "71: a unit moved during the 5 s warm-up");
	test::require(rig.until(2.0, [&] { return siteOf(rig, site).mats[0].have > 0; }), "71: nothing moved after the warm-up");
	test::require(siteOf(rig, site).mats[0].have == 1, "71: more than one unit of a kind moved in the first tick");
	rig.tick(1);
	test::require(siteOf(rig, site).mats[0].have == 2, "71: not one unit per tick");
	rig.tick(1);
	test::require(siteOf(rig, site).mats[0].have == 3 && carried(rig, rig.member, items[0]) == 0, "71: the third unit did not move");
	rig.tick(2);
	// The second kind is not in the pack, and nothing is left of the first: impossible, the order ends.
	test::require(rig.sim.constructionOrderCount() == 0, "71: the order did not end when no missing kind was in the pack");
	test::require(siteOf(rig, site).mats[1].have == 0, "71: a kind that was not carried was delivered");
	// No material of a missing kind at all: refused at once after the warm-up, nothing changes.
	const std::uint64_t other = makeSite(rig, 8.0, {{items[1], 4, 0}}, 0.0, false);
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::AddMaterials, other), "71: the second order was refused");
	rig.tick(120);
	test::require(rig.sim.constructionOrderCount() == 0 && siteOf(rig, other).mats[0].have == 0, "71: an empty-handed order did not end or delivered");
	std::cout << "  71: 5 s warm-up, one unit per missing kind per tick, impossible when none is carried\n";
}

void ruinRevives()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 20, 10}}, 0.0, false);
	flecs::entity entity = siteEntity(rig, site);
	state::PlayerBuilding* b = entity.get_mut<state::PlayerBuilding>();
	b->ruin = true;
	b->hp = 0.0;
	give(rig, rig.member, items[0], 2);
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::AddMaterials, site), "ruin: the order was refused");
	test::require(rig.until(10.0, [&] { return !siteOf(rig, site).ruin; }), "ruin: the first delivery did not revive it");
	const state::PlayerBuilding& revived = siteOf(rig, site);
	// 11 delivered, then floor(11 x U(0.3, 0.5) + 0.5): 3 to 6; HP 0.4 of max.
	test::require(revived.mats[0].have >= 3 && revived.mats[0].have <= 6, "ruin: delivered kept is outside floor(11 x U(0.3, 0.5) + 0.5)");
	test::require(std::fabs(revived.hp - 0.4 * revived.maxHp) < 1e-6 && !revived.built, "ruin: HP is not 0.4 max or the building is built");
	std::cout << "  ruin: the first delivery revived it at 0.4 max HP keeping " << revived.mats[0].have << " of 11\n";
}

void buildSkill()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	rig.fighterOf(rig.leader).skills.engineer = 100.0;
	rig.fighterOf(rig.member).skills.engineer = 1.0;
	const std::uint64_t fast = makeSite(rig, 0.0, {{items[0], 100, 100}}, 0.0, false);
	const std::uint64_t slow = makeSite(rig, 4.0, {{items[0], 100, 100}}, 0.0, false);
	test::require(rig.sim.orderConstructionTask(rig.leader, routines::task::Build, fast), "2: the leader's order was refused");
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Build, slow), "2: the member's order was refused");
	rig.tick(400);
	const double hpFast = siteOf(rig, fast).hp;
	const double hpSlow = siteOf(rig, slow).hp;
	test::require(hpSlow > 0.0 && hpFast > hpSlow, "2: a higher Engineering skill did not build faster");
	const double want = buildings::skillMult(100.0) / buildings::skillMult(1.0);
	test::require(std::fabs(hpFast / hpSlow - want) < 0.15 * want, "2: the rate ratio is not skillMult(100) / skillMult(1)");
	// HP per tick equals the Phase 2 rate at the builder's power: maxHp / hours / ticksPerGameHour x skillMult.
	const double hours = buildings::buildHours(100, 1.0, rig.sim.buildSpeed());
	const double perTick = buildings::hpPerTick(100.0, hours, buildings::skillMult(1.0));
	test::require(std::fabs(hpSlow - perTick * 400.0) < 0.05 * hpSlow, "2: the HP per tick is not the construction rate times skillMult");
	test::require(rig.fighterOf(rig.leader).skills.engineer > 100.0 - 1e-9 && rig.fighterOf(rig.member).skills.engineer > 1.0, "2: Engineering did not train");
	std::cout << "  2: HP after 400 ticks " << hpFast << " (skill 100) vs " << hpSlow << " (skill 1), ratio " << hpFast / hpSlow << "\n";
}

void buildCapAndComplete()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	rig.fighterOf(rig.member).skills.engineer = 100.0;
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 6, 3}}, 0.0, false);
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Build, site), "2: the order was refused");
	test::require(rig.until(300.0, [&] { return rig.sim.constructionOrderCount() == 0; }), "2: the order never ended at the material cap");
	test::require(!siteOf(rig, site).built && siteOf(rig, site).hp <= 3.0 + 1e-6 && siteOf(rig, site).hp > 2.0, "2: HP is not held at the delivered materials (3 of 6)");
	test::require(rig.logged("refused: the delivered materials"), "2: the stop at the cap was not logged");
	// The rest is delivered (71's own step is the same call as Simulation::addMaterials); the next build runs to the end.
	test::require(rig.sim.addMaterials(site, items[0], 3) == 3, "addMaterials took the rest");
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Build, site), "2: the second order was refused");
	test::require(rig.until(300.0, [&] { return siteOf(rig, site).built; }), "2: the site never completed");
	test::require(siteOf(rig, site).hp == siteOf(rig, site).maxHp, "2: a built site is not at max HP");
	test::require(rig.logged("construction complete building " + std::to_string(site)), "2: no \"construction complete\" line");
	test::require(rig.until(5.0, [&] { return rig.sim.constructionOrderCount() == 0; }), "2: the order did not end on a whole building");
	std::cout << "  2: held at the 3 delivered of 6, finished after the rest, \"construction complete\" logged\n";
}

void repairs()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	rig.fighterOf(rig.member).skills.engineer = 100.0;
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 10, 10}}, 10.0, true);
	flecs::entity entity = siteEntity(rig, site);
	test::require(!buildings::removeHp(*entity.get_mut<state::PlayerBuilding>(), 4.0, 1.0), "a hit of 4 destroyed the building");
	const state::PlayerBuilding& hit = siteOf(rig, site);
	test::require(!hit.built && hit.broken && std::fabs(hit.hp - 5.0) < 1e-9, "damage: a built building at max HP did not drop to max - 1, minus the hit, and stop being built");
	test::require(hit.mats[0].have == 10, "damage: delivered materials changed (repair needs none)");
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Repair, site), "95: the order was refused");
	test::require(rig.until(600.0, [&] { return siteOf(rig, site).built; }), "95: the building was never repaired");
	test::require(!siteOf(rig, site).broken && siteOf(rig, site).mats[0].have == 10, "95: still broken, or materials changed");
	std::cout << "  95: a damaged building (HP 5 of 10) was repaired with no new materials\n";
}

void dismantles()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	rig.fighterOf(rig.member).skills.engineer = 100.0;
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 2, 2}}, 2.0, true);
	flecs::entity entity = siteEntity(rig, site);
	entity.get_mut<state::PlayerBuilding>()->furniture = 1;
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Dismantle, site), "96: the order was refused");
	rig.tick(3);
	test::require(rig.sim.constructionOrderCount() == 0 && !siteOf(rig, site).dismantled && siteOf(rig, site).hp == 2.0, "96: not refused over furniture");
	test::require(rig.logged("dismantle the furniture first"), "96: the furniture bark was not logged");
	entity.get_mut<state::PlayerBuilding>()->furniture = 0;
	entity.get_mut<state::PlayerBuilding>()->mounted = 1;
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Dismantle, site), "96: the second order was refused");
	rig.tick(3);
	test::require(rig.sim.constructionOrderCount() == 0 && !siteOf(rig, site).dismantled, "96: not refused over a mounted building");
	test::require(rig.logged("unmount the buildings first"), "96: the mounted bark was not logged");
	entity.get_mut<state::PlayerBuilding>()->mounted = 0;
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Dismantle, site), "96: the third order was refused");
	test::require(rig.until(120.0, [&] { return siteOf(rig, site).dismantled; }), "96: the building was never dismantled");
	const std::uint32_t refund = onGround(rig, items[0]);
	test::require(refund >= 1 && refund <= 2, "96: the refund is not max(1, U(0.6, 1) x delivered) of 2 units on the ground");
	test::require(!siteOf(rig, site).built && siteOf(rig, site).hp == 0.0, "96: a dismantled building is still built or has HP");
	test::require(!rig.sim.orderConstructionTask(rig.member, routines::task::Build, site), "a dismantled building took a new order");
	std::cout << "  96: refused over furniture and mounted buildings; dismantled, " << refund << " of 2 delivered units on the ground\n";
}

void orderRules()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 2, 0}}, 0.0, false);
	// Not the player's character.
	state::Allegiance other;
	other.factionSid = "somebody-else";
	rig.entityOf(rig.leader).set<state::Allegiance>(other);
	test::require(!rig.sim.orderConstructionTask(rig.leader, routines::task::Build, site), "2: an NPC was ordered to build");
	// Not the player's building.
	flecs::entity entity = siteEntity(rig, site);
	entity.get_mut<state::PlayerBuilding>()->ownerFaction += 1000;
	test::require(!rig.sim.orderConstructionTask(rig.member, routines::task::Build, site), "2: another faction's building took an order");
	entity.get_mut<state::PlayerBuilding>()->ownerFaction -= 1000;
	test::require(!rig.sim.orderConstructionTask(rig.member, routines::task::Build, 123456789), "2: an unknown building took an order");
	test::require(!rig.sim.orderConstructionTask(rig.member, routines::task::UseTrainingDummy, site), "97: a building that is no dummy took the order");
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Build, site), "2: the player's own order was refused");
	std::cout << "  orders: only a player character on a player building; 97 only on a dummy\n";
}

void enterLeave()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 1, 1}}, 1.0, true);
	rig.fighterOf(rig.member).x = kX + 6.0;
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::EnterBuilding, site), "156: the order was refused");
	rig.tick(3);
	test::require(rig.fighterOf(rig.member).building == site && rig.sim.constructionOrderCount() == 0, "156: the member is not inside, or the order did not end");
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::LeaveBuilding, site), "200: the order was refused");
	rig.tick(3);
	test::require(rig.fighterOf(rig.member).building == 0 && rig.sim.constructionOrderCount() == 0, "200: the member is still inside, or the order did not end");
	// Never stuck: no way to the door (no navigator), the order gives up after 30 s.
	rig.fighterOf(rig.member).x = kX + 300.0;
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::EnterBuilding, site), "156: the far order was refused");
	test::require(rig.until(40.0, [&] { return rig.sim.constructionOrderCount() == 0; }), "156: an unreachable building held the order for good");
	test::require(rig.fighterOf(rig.member).building == 0, "156: entered a building it never reached");
	std::cout << "  156 / 200: interior set and cleared at the door; an unreachable one ends after 30 s\n";
}

void trainingDummy()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	std::string dummy;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("BUILDING")))
		for (const std::int32_t function : routines::buildingFunctions(*rig.gd, *record))
			if (function == routines::kFunctionTraining && dummy.empty() && record->floatOr("output rate", 0.0f) > 0.0f)
				dummy = record->stringID;
	if (dummy.empty())
	{
		std::cout << "  97: no training dummy in the data, skipped\n";
		return;
	}
	const double cap = rig.gd->find(dummy)->floatOr("output rate", 0.0f);
	const std::uint64_t site = makeSite(rig, 6.0, {}, 1.0, true, dummy);
	state::Fighter& f = rig.fighterOf(rig.member);
	f.skills.attack = 5.0;
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::UseTrainingDummy, site), "97: the order was refused");
	rig.tick(200);
	test::require(rig.fighterOf(rig.member).skills.attack > 5.0, "97: melee attack did not train");
	rig.fighterOf(rig.member).skills.attack = cap;
	rig.tick(3);
	test::require(rig.sim.constructionOrderCount() == 0, "97: the order did not end at the dummy's level cap");
	std::cout << "  97: trains melee attack, ends at the dummy's level cap " << cap << " (" << dummy << ")\n";
}

void saved()
{
	Rig rig({}, 0);
	joinPlayer(rig);
	const std::vector<std::string> items = resourceItems(rig, 1);
	const std::uint64_t site = makeSite(rig, 6.0, {{items[0], 8, 5}}, 3.0, false);
	state::PlayerBuilding* b = siteEntity(rig, site).get_mut<state::PlayerBuilding>();
	b->ruin = true;
	b->dismantled = false;
	b->furniture = 3;
	b->mounted = 2;
	rig.fighterOf(rig.member).skills.engineer = 37.5;
	rig.tick(2); // the arena's fighter reaches the saved component at the tick's mirror
	{
		std::stringstream first;
		rig.sim.save(first);
		rig.sim.reload(first);
	}
	const std::uint64_t before = rig.sim.stateHash();
	std::stringstream bytes;
	rig.sim.save(bytes);
	rig.sim.reload(bytes);
	test::require(rig.sim.stateHash() == before, "the state hash changed over a save / reload with a ruin");
	const state::PlayerBuilding& loaded = siteOf(rig, site);
	test::require(loaded.ruin && !loaded.dismantled && loaded.furniture == 3 && loaded.mounted == 2, "ruin / dismantled / furniture / mounted did not survive a save");
	test::require(loaded.mats[0].have == 5 && loaded.hp == 3.0, "the construction progress did not survive a save");
	test::require(std::fabs(rig.fighterOf(rig.member).skills.engineer - 37.5) < 0.01, "Engineering did not survive a save");
	// The dismantled-for-good flag too.
	siteEntity(rig, site).get_mut<state::PlayerBuilding>()->dismantled = true;
	std::stringstream again;
	rig.sim.save(again);
	rig.sim.reload(again);
	test::require(siteOf(rig, site).dismantled, "the dismantled flag did not survive a save");
	std::cout << "  save v88: ruin, dismantled, furniture, mounted and Engineering round-trip, state hash equal\n";
}

} // namespace

int main()
{
	try
	{
		addMaterials();
		ruinRevives();
		buildSkill();
		buildCapAndComplete();
		repairs();
		dismantles();
		orderRules();
		enterLeave();
		trainingDummy();
		saved();
		std::cout << "ksim_construction: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_construction: FAILED: " << e.what() << "\n";
		return 1;
	}
}
