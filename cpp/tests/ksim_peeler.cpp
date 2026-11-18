// The skin peeler (TortureBuilding::vf28 14029af90; kinspect dt39_eaten_alive.md section 6): every tick each living occupant of the
// building takes limb damage through the part helper (not the combat damage path), a limb at its threshold is severed (the item drops),
// the occupant screams (SCREAMING_TORTURE 0x0b) every tick, and no per-tick toughness XP is given; an occupant carrying a first aid kit
// is left alone. Worn armour is destroyed first, the torso parts follow the limbs and the occupant is released last (rwh42 5). A bare world: the cage building is marked a peeler by hand (the data's "skin peeler" record is a mod's; keyedOnTheData checks the record test).

#include "dt27_rig.hpp"

#include "ksim/routines.hpp"


namespace {

using namespace ksim;
using namespace dt27;


const state::FighterPart& partOf(const state::Fighter& f, const char* name)
{
	for (const state::FighterPart& p : f.parts)
		if (p.name == name)
			return p;
	throw std::runtime_error(std::string("no part ") + name);
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

void peelerTearsTheOccupant()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	const std::uint64_t cageOnly = occupantIn(rig, *site);
	rig.sim.setPeeler(site->key, true);
	undress(rig, cageOnly);
	state::Fighter& v = rig.fighterOf(cageOnly);
	v.skills.toughness = 20.0;
	const double toughness = v.skills.toughness;
	const double leg = partOf(v, "Right Leg").health;
	rig.tick(40);
	test::require(partOf(rig.fighterOf(cageOnly), "Right Leg").health < leg, "peeler: the limbs took no damage over time");
	test::require(rig.sim.dialogueEventCount(0x0b) >= 40, "peeler: the occupant did not scream every tick");
	test::require(rig.fighterOf(cageOnly).skills.toughness == toughness, "peeler: the occupant got per-tick toughness XP");
	// Force the right leg to its threshold: the next tick severs it (the item drops, the sever XP fires once).
	for (state::FighterPart& p : rig.fighterOf(cageOnly).parts)
		if (p.name == "Right Leg")
			p.health = -p.maxHealth;
	std::size_t items = 0;
	const auto countItems = [&] { std::size_t n = 0; rig.sim.world().ecs().each([&](const state::GroundItem&) { ++n; }); return n; };
	items = countItems();
	rig.tick(3);
	test::require(countItems() > items, "peeler: the severed limb item did not drop");
	test::require(partOf(rig.fighterOf(cageOnly), "Right Leg").severed(), "peeler: the limb at its threshold was not severed");
	test::require(rig.fighterOf(cageOnly).skills.toughness > toughness, "peeler: the sever gave no one-off XP");
	std::cout << "  peeler: limbs wear down, a limb at threshold is severed, 0x0b every tick, no per-tick XP\n";
}

// rwh42 5: worn armour is destroyed first (no limb damage while any remains), then the limbs (severed with their items), then the occupant is released (the head and torso are never touched, rwh97 7).
void peelerStepsInOrder()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	const std::uint64_t id = occupantIn(rig, *site);
	rig.sim.setPeeler(site->key, true);
	state::Inventory pack;
	state::Fighter& v = rig.fighterOf(id);
	v.armour.clear();
	for (const int slot : {5, 7})
	{
		state::FighterArmour piece;
		piece.slot = slot;
		piece.cut = 0.02; // (cut + blunt) / 0.04 = 1 game-second per piece
		piece.blunt = 0.02;
		piece.sid = "test-armour-" + std::to_string(slot);
		v.armour.push_back(piece);
		pack.add(piece.sid, 1, static_cast<std::uint8_t>(state::Inventory::kArmourBase + slot));
	}
	rig.entityOf(id).set<state::Inventory>(std::move(pack));
	const auto worn = [&]
	{
		std::size_t n = 0;
		for (const state::Inventory::Entry& e : rig.entityOf(id).get<state::Inventory>()->items)
			n += e.slot >= state::Inventory::kArmourBase ? 1 : 0;
		return n;
	};
	const auto hurt = [&](bool torso)
	{
		for (const state::FighterPart& p : rig.fighterOf(id).parts)
			if (p.limb() != torso && p.health < p.maxHealth)
				return true;
		return false;
	};
	test::require(worn() == 2, "peeler steps: the armour was not set up");
	rig.tick(10);
	test::require(!hurt(false) && !hurt(true), "peeler steps: a body part took damage while armour remained");
	test::require(rig.until(5.0, [&] { return worn() == 0; }), "peeler steps: the armour was never destroyed");
	test::require(rig.fighterOf(id).armour.empty(), "peeler steps: the fighter's armour rows stayed");
	test::require(!hurt(false) && !hurt(true), "peeler steps: a part was damaged before the last piece went");
	rig.tick(10);
	test::require(hurt(false), "peeler steps: the limbs took no damage once the armour was gone");
	test::require(!hurt(true), "peeler steps: the torso was damaged while limbs still stood");
	for (state::FighterPart& p : rig.fighterOf(id).parts)
		if (p.limb())
			p.health = -p.maxHealth;
	rig.tick(3);
	for (const state::FighterPart& p : rig.fighterOf(id).parts)
		test::require(!p.limb() || p.severed(), "peeler steps: a limb at its line was not severed");
	// rwh97 7: TortureBuilding::vf28 walks the limbs only; head and torso are never touched. The occupant is let go once no limb is left (UNSURE:
	// the exe's release of a limbless occupant was not traced).
	test::require(!hurt(true), "peeler steps: the machine touched the head or torso");
	rig.tick(10);
	test::require(!hurt(true), "peeler steps: the machine touched the head or torso after the limbs went");
	std::cout << "  peeler: armour first, then limbs, head and torso untouched\n";
}

void notAPeelerIsLeftAlone()
{
	Rig rig({}, 0);
	const BuildingSite* site = findCageBuilding(rig);
	test::require(site != nullptr, "no building with a cage layout exists");
	const std::uint64_t id = occupantIn(rig, *site);
	const double leg = partOf(rig.fighterOf(id), "Right Leg").health;
	rig.tick(40);
	test::require(partOf(rig.fighterOf(id), "Right Leg").health == leg, "an ordinary cage hurt its occupant");
	std::cout << "  peeler: an ordinary cage does not\n";
}

// FUN_14057cc70 case function 8: a `produces` reference or a non-zero `flags` makes a TortureBuilding, whatever the record is called.
void keyedOnTheData()
{
	kdata::Record cage;
	cage.name = "Cage";
	cage.ints.push_back({"function", routines::kFunctionCage});
	cage.ints.push_back({"flags", 0});
	test::require(!routines::isTortureFunctionality(cage), "a plain cage is a torture building");
	kdata::Record modded = cage;
	modded.name = "Rack of Sorrows";
	modded.references.push_back({"produces", {{"42241-rebirth.mod", 5}}});
	test::require(routines::isTortureFunctionality(modded), "a function-8 record with a produces ref (another name) is not a peeler");
	kdata::Record flagged = cage;
	flagged.name = "Iron Maiden";
	flagged.ints[1].value = 2;
	test::require(routines::isTortureFunctionality(flagged), "a function-8 record with flags != 0 is not a peeler");
	kdata::Record notCage = modded;
	notCage.ints[0].value = 6;
	test::require(!routines::isTortureFunctionality(notCage), "a produces ref on another function made a peeler");
	kdata::Record named = cage;
	named.name = "skin peeler";
	test::require(!routines::isTortureFunctionality(named), "the name alone made a peeler");
	std::cout << "  peeler: keyed on produces / flags of function 8, not the name\n";
}

} // namespace

int main()
{
	try
	{
		peelerTearsTheOccupant();
		peelerStepsInOrder();
		notAPeelerIsLeftAlone();
		keyedOnTheData();
		std::cout << "ksim_peeler: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_peeler: FAILED: " << e.what() << "\n";
		return 1;
	}
}
