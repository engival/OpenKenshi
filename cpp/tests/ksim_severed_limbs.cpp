// Severed limbs become ground items (kenshi-h2v.13.1; rwh42 severed-limbs static, docs/reference/COMBAT_MEDICAL.md): when a limb is severed the race's
// `severed limbs` item for the slot lies at the body, ownerless, not stolen, one of it.
//   - a hit past -M severs a limb and leaves exactly one item for it;
//   - a peeler occupant loses its four limbs in the exe's order (Left Arm, Right Arm, Left Leg, Right Leg), one item each;
//   - Dismemberment 0 severs nothing and drops nothing, at the peeler too;
//   - the eat-alive path severs a limb and drops no item (severPart param_3 = 0);
//   - a save and a load keep the items.
// A bare world (dt27_rig), the cage building marked a peeler by hand.

#include "dt27_rig.hpp"

#include "ksim/save/save.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdio>


namespace {

using namespace ksim;
using namespace dt27;

state::FighterPart& partOf(state::Fighter& f, const char* name)
{
	for (state::FighterPart& p : f.parts)
		if (p.name == name)
			return p;
	throw std::runtime_error(std::string("no part ") + name);
}

constexpr const char* kLimbs[4] = {"Left Arm", "Right Arm", "Left Leg", "Right Leg"};

// The race's item for a slot (0 L arm, 1 R arm, 2 L leg, 3 R leg): the first entry of the slot with an item.
std::string raceItem(const Rig& rig, const state::Fighter& f, int slot)
{
	const kdata::Record* race = rig.gd->find(f.raceSid);
	const kdata::RefCategory* limbs = race != nullptr ? race->findRefs("severed limbs") : nullptr;
	test::require(limbs != nullptr, "the race has a `severed limbs` list: " + f.raceSid);
	for (const kdata::RefEntry& e : limbs->entries)
		if (e.v0 == slot && !e.targetID.empty())
			return e.targetID;
	return {};
}

struct Item
{
	flecs::entity_t entity;
	state::GroundItem item;
};

// Every ground item of a kind in creation order (the entity ids rise).
std::vector<Item> limbItems(Rig& rig, const std::vector<std::string>& kinds)
{
	std::vector<Item> found;
	rig.sim.world().ecs().each([&](flecs::entity e, const state::GroundItem& g)
	{
		if (std::find(kinds.begin(), kinds.end(), g.itemSid) != kinds.end())
			found.push_back({e.id(), g});
	});
	std::sort(found.begin(), found.end(), [](const Item& a, const Item& b) { return a.entity < b.entity; });
	return found;
}

std::vector<std::string> kindsOf(const Rig& rig, const state::Fighter& f)
{
	std::vector<std::string> kinds;
	for (int slot = 0; slot < 4; ++slot)
		kinds.push_back(raceItem(rig, f, slot));
	return kinds;
}

std::uint64_t occupantIn(Rig& rig, const BuildingSite& site)
{
	const std::uint64_t id = rig.spawn(rig.body, site.cx, site.cz);
	state::Fighter& f = rig.fighterOf(id);
	f.cageBuilding = site.key;
	f.cageSlot = 0;
	f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	return id;
}

void requireOwnerless(const Item& it, const state::Fighter& body)
{
	test::require(it.item.ownerSid.empty() && it.item.stolen.empty() && it.item.count == 1, "a limb item is ownerless, not stolen, one of it");
	test::require(std::hypot(it.item.x - body.x, it.item.z - body.z) < 1.0, "the limb lies at the body (the character position: no bones)");
}

// A hit past -M severs a limb: every limb of the victim sits just above its line, an attacker ordered onto it hits (the bare arena of ksim_combat_facing),
// and the arena raises one LimbSevered note for each limb the hits severed. The note is what Simulation::onLimbSevered turns into the item: the peeler
// case below runs the same note through the Simulation. (A hit through the Simulation's own AI could not be steered in the rig: its goal layer drops
// the order.)
void hitSevers()
{
	Rig rig({}, 0);
	// Which part a blow lands on is the arena's dice: duels of other seeds are tried until a blow lands on an arm (the same seeds every run).
	for (std::uint64_t seed = 1; seed <= 60; ++seed)
	{
		CombatArena arena(rig.data, CombatSettings{}, seed);
		std::uint64_t rng = seed;
		state::Fighter a = rig.data->makeFighter(*rig.gd, rig.body, "", rng);
		a.x = 0.0;
		a.z = 0.0;
		a.team = 1;
		a.orderTarget = 2;
		state::Fighter b = rig.data->makeFighter(*rig.gd, rig.body, "", rng);
		b.x = 1.5;
		b.z = 0.0;
		b.team = 0;
		b.playerControlled = true; // stands still
		for (const char* name : {"Left Arm", "Right Arm"}) // the arms only: a victim with a leg down is a crawler the attacker leaves alone
		{
			state::FighterPart& p = partOf(b, name);
			p.health = -(p.maxHealth - p.wear) + 0.5;
		}
		arena.add(1, std::move(a));
		arena.add(2, std::move(b));
		std::vector<CombatArena::SeverNote> notes;
		for (int i = 0; i < 600 && notes.empty(); ++i)
		{
			arena.step(kTickSeconds);
			notes = arena.takeSevers();
		}
		if (notes.empty())
			continue;
		for (int i = 0; i < 60; ++i)
		{
			arena.step(kTickSeconds);
			for (const CombatArena::SeverNote& n : arena.takeSevers())
				notes.push_back(n);
		}
		std::size_t severed = 0;
		for (const char* name : kLimbs)
			severed += partOf(*arena.find(2), name).severed() ? 1 : 0;
		test::require(severed == notes.size(), "one note for each limb a hit severed: " + std::to_string(notes.size()) + " notes, " + std::to_string(severed) + " limbs");
		for (const CombatArena::SeverNote& n : notes)
			test::require(n.who == 2 && n.item, "the note names the victim and asks for the item");
		std::cout << "  a hit severs a limb (seed " << seed << "): " << severed << " severed, " << notes.size() << " note(s), one each\n";
		return;
	}
	test::require(false, "no hit severed a limb in 60 duels");
}

// The peeler: the four limbs go in order, one item each, at the occupant; saved and loaded, the items stay.
void peelerDrops()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	rig.sim.setPeeler(site->key, true);
	const std::uint64_t id = occupantIn(rig, *site);
	undress(rig, id);
	state::Fighter& occupant = rig.fighterOf(id);
	const std::vector<std::string> kinds = kindsOf(rig, occupant);
	for (const char* name : kLimbs)
	{
		state::FighterPart& p = partOf(occupant, name);
		p.health = -(p.maxHealth - p.wear);
	}
	rig.tick(3);
	const std::vector<Item> items = limbItems(rig, kinds);
	test::require(items.size() == 4, "one item per limb at the peeler, got " + std::to_string(items.size()));
	for (int slot = 0; slot < 4; ++slot)
	{
		test::require(items[static_cast<std::size_t>(slot)].item.itemSid == kinds[static_cast<std::size_t>(slot)], std::string("the items came in the exe's order: ") + kLimbs[slot]);
		requireOwnerless(items[static_cast<std::size_t>(slot)], rig.fighterOf(id));
	}
	for (const char* name : kLimbs)
		test::require(partOf(rig.fighterOf(id), name).severed(), std::string(name) + " is severed");
	rig.tick(40);
	test::require(limbItems(rig, kinds).size() == 4, "no second item for a limb already severed");
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_severed_limbs.ksav";
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	std::remove(path.c_str());
	std::size_t kept = 0;
	const_cast<flecs::world&>(loaded.world().ecs()).each([&](const state::GroundItem& g)
	{
		kept += std::find(kinds.begin(), kinds.end(), g.itemSid) != kinds.end() ? 1 : 0;
	});
	test::require(kept == 4, "the save keeps the four limb items, got " + std::to_string(kept));
	std::cout << "  peeler: four limbs in order, four items, none twice; a save and a load keep them\n";
}

// Dismemberment 0: a hit and the peeler sever nothing and drop nothing.
void dismembermentOff()
{
	Rig rig({}, 0, 0.5, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	rig.sim.setPeeler(site->key, true);
	const std::uint64_t id = occupantIn(rig, *site);
	undress(rig, id);
	const std::vector<std::string> kinds = kindsOf(rig, rig.fighterOf(id));
	for (const char* name : kLimbs)
	{
		state::FighterPart& p = partOf(rig.fighterOf(id), name);
		p.health = -(p.maxHealth - p.wear) - 1.0;
	}
	rig.tick(40);
	for (const char* name : kLimbs)
		test::require(!partOf(rig.fighterOf(id), name).severed(), std::string(name) + " stays on at Dismemberment 0");
	test::require(limbItems(rig, kinds).empty(), "no item at Dismemberment 0");
	std::cout << "  Dismemberment 0: nothing severed, nothing dropped\n";
}

// The eat-alive path severs with param_3 = 0: the limb goes, no item.
void eatLeavesNoItem()
{
	Rig rig({}, 0);
	const std::uint64_t victimId = rig.leader;
	state::Fighter& victim = rig.fighterOf(victimId);
	const std::vector<std::string> kinds = kindsOf(rig, victim);
	for (state::FighterPart& p : victim.parts)
		p.health = -(p.maxHealth - p.wear) - 1.0; // every part past its line: the walk severs what it reaches
	for (state::FighterPart& p : victim.parts)
		if (p.vital)
			p.health = 1.0;
	CombatArena::EatState state;
	const_cast<CombatArena*>(rig.sim.combat())->eat(victimId, false, 0.05, state);
	rig.tick(3);
	std::size_t severed = 0;
	for (const char* name : kLimbs)
		severed += partOf(rig.fighterOf(victimId), name).severed() ? 1 : 0;
	test::require(severed >= 1, "the eat walk severed a limb");
	test::require(limbItems(rig, kinds).empty(), "the eat-alive path leaves no limb item");
	std::cout << "  eat alive: " << severed << " limb(s) severed, no item\n";
}

} // namespace

int main()
{
	try
	{
		hitSevers();
		peelerDrops();
		dismembermentOff();
		eatLeavesNoItem();
		std::cout << "ksim_severed_limbs: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_severed_limbs: FAILED: " << e.what() << "\n";
		return 1;
	}
}
