// The off-screen medical step and the corpses it leaves (kenshi-k16.6.52; COMBAT_OPTIONS.md 8, RUNTIME_SAVE.md v100), behind
// Simulation::setOffscreenMedical. A world squad is built beside the player, a member is hurt, the player leaves (the squad is a token) and the
// world runs. With the setting on:
//   - a bleeding member dies off-screen and leaves a body at the token's position (CorpseDecay with its death tick and cause, the gear in its
//     Inventory, a Fighter that is dead); the player returning finds a body he can loot;
//   - the body is gone 12 game hours after the death, wherever it lies; a save and a load keep it;
//   - a member whose wounds clot heals, and its record goes away;
//   - a squadmate with a first-aid kit saves a member who dies without one.
// With the setting off nothing is stepped (the record stays as it was) and a body no player area covers is destroyed.

#include "delta_scene.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/combat/corpses.hpp"
#include "ksim/save/save.hpp"

#include <cstdio>
#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace delta_scene;

std::shared_ptr<MeshNavigator> gNav;
const kworld::LoadOrder* gOrder = nullptr;
std::shared_ptr<const CombatData> gCombat;

constexpr std::uint64_t kMinute = 37; // a little over a game minute of ticks
constexpr std::uint64_t kHour = kTicksPerGameHour;

std::string itemNamed(const Simulation& sim, const std::string& name)
{
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("ITEM")))
	{
		if (record->name == name)
			return record->stringID;
	}
	test::require(false, "the data has " + name);
	return {};
}

// Takes every first-aid kit out of the squad so that only the kits a case gives are there.
void withoutKits(Simulation& sim, const std::vector<flecs::entity>& members)
{
	const std::string kit = itemNamed(sim, "Basic First Aid Kit");
	for (const flecs::entity member : members)
	{
		if (const state::Inventory* held = member.get<state::Inventory>())
		{
			state::Inventory inventory = *held;
			while (inventory.removeCarried(kit, 1)) {}
			member.set<state::Inventory>(std::move(inventory));
		}
	}
}

void give(flecs::entity member, const std::string& item)
{
	state::Inventory inventory;
	if (const state::Inventory* held = member.get<state::Inventory>())
		inventory = *held;
	inventory.add(item, 1);
	member.set<state::Inventory>(std::move(inventory));
}

enum class Hurt
{
	BleedOut, // wounds no clotting outruns: the blood runs out
	Clots, // a small wound and a little blood lost: it heals by itself
	Treatable, // a wound a kit closes in time, and which kills without one
};

struct Case
{
	std::unique_ptr<Simulation> box;
	Simulation& sim;
	Scene scene;
	std::string bread; // a recognisable piece of gear on the hurt one
	// What is wrong with whom, as the records a token's members would hold: written once the squad is a token (a wound given in detail would
	// kill or wake the member before the player is far enough for the demotion).
	std::vector<std::pair<flecs::entity, state::MemberDelta>> records;
};

Case prepare(bool on, Hurt hurt, bool kit)
{
	std::unique_ptr<Simulation> box = newGameBoxed(*gOrder, gCombat, gNav, 23);
	Simulation& sim = *box;
	sim.setOffscreenMedical(on);
	Scene scene = pick(sim, 3);
	withoutKits(sim, scene.members);
	const std::string bread = itemNamed(sim, "Bread");
	give(scene.members[0], bread);
	const state::Fighter whole = fighterOf(sim, scene.members[0]);
	state::Fighter f = whole;
	switch (hurt)
	{
	case Hurt::BleedOut:
		f.wounds = {40.0, 40.0};
		f.blood -= 40.0;
		break;
	case Hurt::Clots:
		f.wounds = {0.05};
		f.blood -= 25.0;
		break;
	case Hurt::Treatable:
		f.wounds = {0.5, 0.5, 0.5};
		f.blood -= 60.0;
		break;
	}
	Case c{std::move(box), sim, std::move(scene), bread, {}};
	c.records.emplace_back(c.scene.members[0], *state::MemberDelta::between(f, whole));
	if (kit)
	{
		give(c.scene.members[1], itemNamed(sim, "Basic First Aid Kit"));
		state::Fighter medic = fighterOf(sim, c.scene.members[1]);
		const state::Fighter unskilled = medic;
		medic.skills.medic = 60.0;
		c.records.emplace_back(c.scene.members[1], *state::MemberDelta::between(medic, unskilled));
	}
	return c;
}

bool holds(flecs::entity member, const std::string& item)
{
	const state::Inventory* held = member.get<state::Inventory>();
	return held != nullptr && std::any_of(held->items.begin(), held->items.end(), [&](const state::Inventory::Entry& e) { return e.itemSid == item; });
}

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i) { found = i.value == id ? e : found; });
	return found;
}

// The player leaves, the squad is a token, and the case's wounds are written to it.
void depart(Case& c)
{
	leave(c.sim, c.scene);
	for (const auto& [member, record] : c.records)
	{
		test::require(!member.has<state::Fighter>() && !member.has<state::MemberDelta>(), "the member is stored untouched before it is hurt");
		member.set<state::MemberDelta>(record);
	}
}

void runMinutes(Simulation& sim, std::uint64_t minutes)
{
	sim.run(minutes * kMinute);
}

void died(Case& c)
{
	flecs::entity hurt = c.scene.members[0];
	test::require(hurt.has<state::Fighter>() && hurt.get<state::Fighter>()->dead, "the bleeding member died off-screen");
	test::require(!hurt.has<state::MemberDelta>(), "a body is a Fighter again: its record is spent");
	const state::CorpseDecay* decay = hurt.get<state::CorpseDecay>();
	test::require(decay != nullptr && decay->stamp > 0.0 && decay->deathTick > 0 && decay->cause == state::CorpseDecay::kCauseBledOut, "the body carries its death tick and cause");
	const state::Position at = *c.scene.squad.get<state::Position>();
	const state::Fighter& body = *hurt.get<state::Fighter>();
	test::require(std::hypot(body.x - at.x, body.z - at.z) < 1.0, "the body lies at the token's position");
	test::require(holds(hurt, c.bread), "the gear stays in its Inventory");
	test::require(!c.scene.members[1].get<state::Fighter>() || !c.scene.members[1].get<state::Fighter>()->dead, "the others are alive");
}

void run()
{
	// Bleeding out: dies off-screen, leaves a body at the token.
	Case c = prepare(true, Hurt::BleedOut, false);
	depart(c);
	test::require(c.scene.members[0].has<state::MemberDelta>(), "the hurt member stored a record");
	runMinutes(c.sim, 4);
	died(c);
	const std::uint64_t deathTick = c.scene.members[0].get<state::CorpseDecay>()->deathTick;
	std::cout << "died at tick " << deathTick << "\n";

	// Promoted near it: a body he can loot.
	{
		Simulation& sim = c.sim;
		const state::Position at = *c.scene.squad.get<state::Position>();
		teleport(sim, c.scene.player, at.x + 3.0, at.z);
		sim.run(60);
		test::require(c.scene.members[0].get<state::Fighter>()->dead, "the body is still dead near the player");
		sim.enqueue(Command::fighterInteract(state::Id{c.scene.player}, idOf(c.scene.members[0]), static_cast<std::uint8_t>(InteractAction::Loot), 1, {}, {}, true));
		sim.run(200);
		test::require(!holds(c.scene.members[0], c.bread) && holds(entityOf(sim, c.scene.player), c.bread), "the body was looted: its gear is the player's");
	}

	// Save and load keep the body (and its cause).
	{
		Case d = prepare(true, Hurt::BleedOut, false);
		depart(d);
		runMinutes(d.sim, 4);
		died(d);
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_offscreen_medical.ksav";
		d.sim.saveGame(path);
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, *gOrder, path, false, nullptr, RegionConfig{});
		std::remove(path.c_str());
		flecs::entity twin;
		ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) { twin = id.value == idOf(d.scene.members[0]) ? e : twin; });
		test::require(twin.is_valid() && twin.has<state::Fighter>() && twin.get<state::Fighter>()->dead, "the load keeps the body");
		const state::CorpseDecay* decay = twin.get<state::CorpseDecay>();
		test::require(decay != nullptr && decay->deathTick == d.scene.members[0].get<state::CorpseDecay>()->deathTick && decay->cause == state::CorpseDecay::kCauseBledOut,
			"the load keeps the death tick and the cause");
	}

	// Decay: gone 12 game hours after the death, with the player away.
	{
		Case d = prepare(true, Hurt::BleedOut, false);
		depart(d);
		runMinutes(d.sim, 4);
		died(d);
		const std::uint64_t at = d.scene.members[0].get<state::CorpseDecay>()->deathTick;
		const flecs::entity body = d.scene.members[0];
		const std::uint64_t id = idOf(body);
		d.sim.run(at + 11 * kHour + kHour / 2 - d.sim.clockTick());
		bool alive = false;
		ecsOf(d.sim).each([&](const state::Id& i, const state::CorpseDecay&) { alive = alive || i.value == id; });
		test::require(alive, "the body is still there 11.5 game hours after the death");
		d.sim.run(kHour + 200);
		alive = false;
		ecsOf(d.sim).each([&](const state::Id& i, const state::CorpseDecay&) { alive = alive || i.value == id; });
		test::require(!alive, "the body is gone 12 game hours after the death");
	}

	// A member that clots heals, and its record goes away.
	{
		Case d = prepare(true, Hurt::Clots, false);
		depart(d);
		test::require(d.scene.members[0].has<state::MemberDelta>(), "the hurt member stored a record");
		std::uint64_t minutes = 0;
		while (d.scene.members[0].has<state::MemberDelta>() && minutes < 3000)
		{
			runMinutes(d.sim, 1);
			++minutes;
		}
		std::cout << "healed in " << minutes << " game minutes\n";
		const state::Fighter* f = d.scene.members[0].get<state::Fighter>();
		test::require(f == nullptr && !d.scene.members[0].has<state::MemberDelta>(), "the member healed off-screen and its record is gone");
	}

	// First aid: dies alone, lives with a squadmate's kit.
	{
		Case alone = prepare(true, Hurt::Treatable, false);
		depart(alone);
		runMinutes(alone.sim, 120);
		const bool diedAlone = alone.scene.members[0].has<state::Fighter>() && alone.scene.members[0].get<state::Fighter>()->dead;
		Case aided = prepare(true, Hurt::Treatable, true);
		depart(aided);
		runMinutes(aided.sim, 120);
		const bool diedAided = aided.scene.members[0].has<state::Fighter>() && aided.scene.members[0].get<state::Fighter>()->dead;
		std::cout << "treatable wound: died alone " << diedAlone << ", died with a kit " << diedAided << "\n";
		test::require(diedAlone && !diedAided, "a squadmate's first aid saves a member who dies without it");
		test::require(!holds(aided.scene.members[1], itemNamed(aided.sim, "Basic First Aid Kit")) || aided.scene.members[1].has<state::MemberDelta>(), "the kit was used");
	}

	// Setting off: nothing is stepped, and a body no player area covers is destroyed.
	{
		Case off = prepare(false, Hurt::BleedOut, false);
		depart(off);
		const state::MemberDelta before = *off.scene.members[0].get<state::MemberDelta>();
		runMinutes(off.sim, 10);
		test::require(!off.scene.members[0].has<state::Fighter>() && off.scene.members[0].has<state::MemberDelta>(), "setting off: the member does not die off-screen");
		const state::MemberDelta& after = *off.scene.members[0].get<state::MemberDelta>();
		test::require(after.blood == before.blood && after.wounds == before.wounds, "setting off: the record does not change");

		// A body: the original's rule destroys it once the player is away; with the setting on it persists.
		for (const bool on : {false, true})
		{
			Case body = prepare(on, Hurt::Clots, false);
			state::Fighter& f = fighterOf(body.sim, body.scene.members[1]);
			f.dead = true;
			f.unconscious = true;
			const std::uint64_t id = idOf(body.scene.members[1]);
			teleport(body.sim, body.scene.player, body.scene.farX, body.scene.farZ); // the body stays in the arena, so no leave()
			body.sim.run(600);
			bool there = false;
			ecsOf(body.sim).each([&](const state::Id& i, const state::Fighter& dead) { there = there || (i.value == id && dead.dead); });
			test::require(there == on, on ? "setting on: a body outside every player area persists" : "setting off: a body outside every player area is destroyed");
		}
	}
	std::cout << "offscreen medical ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "argv[1]: the Hub navmesh cache");
		gNav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gOrder = &order;
		{
			Simulation probe = Simulation::newGame(KENSHI_DATA, order, SpawnOptions{}, nullptr, RegionConfig{});
			gCombat = CombatData::build(*probe.gameData(), KENSHI_DATA);
		}
		run();
	} catch (const std::exception& e) {
		std::cerr << "ksim_offscreen_medical: FAILED: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
