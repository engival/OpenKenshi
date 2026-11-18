// The steal chance (crime.hpp stealChance, kenshi-rwh.163, CRIME_STATIC.md "Steal chance"): the formula per factor, inspecting an owned
// container as no crime, and the GUI take's roll (a forced failure aborts the take with theft 3 + event 8; a forced pass takes the item).

#include "ksim/core/clock.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace cr = ksim::crime;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

bool near(double a, double b) { return std::fabs(a - b) < 1.0e-9; }

void formula()
{
	cr::StealInput c; // a character source
	c.character = true;
	c.thievery = 50.0;
	check(near(cr::stealChance(c), 0.55), "character: 1.1 x thievery / 100 (50 -> 55%)");
	c.thievery = 0.0;
	check(near(cr::stealChance(c), 0.0), "character: thievery 0 -> 0");
	c.thievery = 90.0;
	c.worn = true;
	check(near(cr::stealChance(c), 0.9 * 1.1 * 0.75), "character: a worn item x0.75 (90 -> 74.25%)");
	c.itemIsContainer = true;
	check(near(cr::stealChance(c), 0.9 * 1.1 * 0.75 * 0.75), "character: a worn container item x0.75 again");
	c.itemIsContainer = false;
	c.worn = false;
	c.itemIsContainer = true; // only counts inside the worn branch
	check(near(cr::stealChance(c), 0.99), "character: the container factor needs a worn item");
	c.disabled = true;
	c.thievery = 0.0;
	check(near(cr::stealChance(c), 1.0), "character: a disabled target is 1.0");

	cr::StealInput b; // a building / container source
	b.thievery = 50.0;
	check(near(cr::stealChance(b), 0.3 + 1.95 * 0.5 * 0.5), "container: lerp(0.3, 2.25, thievery/100 x 0.5) at distance 0 (78.75%)");
	b.distance = 100.0;
	check(near(cr::stealChance(b), 0.3 + 1.95 * 0.5 * 0.75), "container: the distance factor 0.75 at 100 u");
	b.distance = 200.0;
	check(near(cr::stealChance(b), 0.3 + 1.95 * 0.5), "container: the distance factor 1.0 at 200 u");
	b.distance = 900.0;
	check(near(cr::stealChance(b), 0.3 + 1.95 * 0.5), "container: the distance factor stops at 200 u");
	b.distance = 200.0;
	b.ownPlatoon = true;
	check(near(cr::stealChance(b), 0.3 + 1.95 * 0.25), "container: own platoon halves the distance factor");
	b.ownPlatoon = false;
	b.posture3 = true;
	check(near(cr::stealChance(b), (0.3 + 1.95 * 0.5) * 2.0), "container: posture 3 x2");
	b.posture3 = false;
	b.floorMismatch = true;
	check(near(cr::stealChance(b), (0.3 + 1.95 * 0.5) * 1.5), "container: a floor mismatch x1.5");
	b.floorMismatch = false;
	b.thievery = 100.0;
	b.distance = 0.0;
	check(near(cr::stealChance(b), 1.275) && !cr::stealCaught(1.275, 0.999), "container: thievery 100 at the box is 127.5%, never caught");
	check(cr::stealCaught(0.3, 0.31) && !cr::stealCaught(0.3, 0.3) && !cr::stealCaught(0.3, 0.0), "the roll: caught iff draw > chance");
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::unique_ptr<Simulation> sim;
	std::string servant, holy, nameless, item;
	std::uint64_t rng = 3;
	std::vector<cr::DialogueEvent> events;

	explicit Rig(std::uint64_t seed, std::shared_ptr<const kdata::GameData> shared = nullptr, std::shared_ptr<const CombatData> sharedData = nullptr)
	{
		if (shared == nullptr)
		{
			const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
			gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
			data = CombatData::build(*gd, KENSHI_DATA);
		} else {
			gd = shared;
			data = sharedData;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "The Holy Nation")
				holy = r->stringID;
			if (r->name == "Nameless")
				nameless = r->stringID;
		}
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ITEM")))
		{
			item = r->stringID;
			break;
		}
		test::require(!servant.empty() && !holy.empty() && !nameless.empty() && !item.empty(), "missing data records");
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, seed));
		sim->setGameData(gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->setDialogueObserver([this](const cr::DialogueEvent& e) { events.push_back(e); });
	}
	std::uint64_t add(const std::string& faction, int cls, double x, double z, bool player)
	{
		state::Fighter f = data->makeFighter(*gd, servant, "", rng);
		f.team = 0;
		f.x = x;
		f.z = z;
		f.yaw = 0.0;
		f.npcClass = static_cast<std::uint8_t>(cls);
		f.playerControlled = player;
		f.attention = 0.5;
		flecs::entity e = sim->spawnFighter(std::move(f));
		e.set<state::Allegiance>(state::Allegiance{faction});
		return e.get<state::Id>()->value;
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	// A container of `owner` at the character's feet holding one item.
	std::uint64_t box(std::uint64_t at, const std::string& owner)
	{
		state::Container c;
		c.recordSid = "box";
		c.x = f(at).x;
		c.z = f(at).z;
		c.y = f(at).y;
		c.useX = c.x;
		c.useY = c.y;
		c.useZ = c.z;
		c.ownerSid = owner;
		state::Inventory stock;
		stock.add(item, 1);
		return sim->spawnContainer(c, stock).get<state::Id>()->value;
	}
	std::uint32_t carried(std::uint64_t id) const
	{
		const state::Inventory* held = nullptr;
		sim->world().ecs().each([&](const state::Id& i, const state::Inventory& inv) { if (i.value == id) held = &inv; });
		return held != nullptr ? held->carried(item) : 0;
	}
	void tick(int n)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
};

void inspect(const Rig& shared)
{
	Rig rig(7, shared.gd, shared.data);
	const std::uint64_t guard = rig.add(rig.holy, 1, 0.0, 0.0, false);
	const std::uint64_t thief = rig.add(rig.nameless, 9, 0.0, 5.0, true);
	const std::uint64_t id = rig.box(guard, rig.holy);
	rig.tick(20 * 3);
	rig.f(thief).x = rig.f(guard).x;
	rig.f(thief).z = rig.f(guard).z;
	rig.f(thief).orderStop = true;
	rig.sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&)
	{
		if (i.value == thief)
			e.set<state::InteractTask>(state::InteractTask{id, 5, 1, 1}); // loot, flag bit 0 = open
	});
	rig.tick(20 * 3);
	const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
	check(r == nullptr || r->crime == cr::kNone, "inspecting (opening) an owned container in view of its owner records no crime");
	check(rig.sim->dialogueEventCount(cr::kEvThiefCaughtStealingFromMe) == 0, "inspecting raises no EV_THIEF_CAUGHT_STEALING_FROM_ME");
	check(rig.carried(thief) == 0 && rig.sim->bountyOn(thief, rig.holy) == 0, "inspecting takes nothing and costs no bounty");
}

struct Take
{
	bool caught;
	bool taken;
	bool theft;
	std::uint64_t event8;
};

// The thief (player, `thievery`) takes the one item from a Holy Nation box with its guard standing at it.
Take take(const Rig& shared, std::uint64_t seed, double thievery, double* chance)
{
	Rig rig(seed, shared.gd, shared.data);
	const std::uint64_t guard = rig.add(rig.holy, 1, 0.0, 0.0, false);
	const std::uint64_t thief = rig.add(rig.nameless, 9, 0.0, 0.0, true);
	rig.f(thief).skills.thievery = thievery;
	const std::uint64_t id = rig.box(thief, rig.holy);
	if (chance != nullptr)
		*chance = rig.sim->stealChance(thief, id, rig.item);
	rig.sim->enqueue(Command::containerTransfer(state::Id{thief}, id, rig.item, 1, true, 1));
	rig.tick(1);
	const state::CrimeRecord* r = rig.sim->crimeRecord(thief);
	(void)guard;
	Take t;
	t.taken = rig.carried(thief) == 1;
	t.theft = r != nullptr && r->crime == cr::kTheft;
	t.event8 = rig.sim->dialogueEventCount(cr::kEvThiefCaughtStealingFromMe);
	t.caught = !t.taken;
	return t;
}

void roll(const Rig& shared)
{
	double chance = 0.0;
	// Thievery 100 at the box: 127.5%, always passes.
	for (std::uint64_t seed = 1; seed <= 3; ++seed)
	{
		const Take t = take(shared, seed, 100.0, &chance);
		check(near(chance, 1.275) && t.taken && t.theft && t.event8 == 0, "seed " + std::to_string(seed) + ": a sure take passes, records theft 3, fires no event 8");
	}
	// Thievery 0: 30%. Some seed fails and some passes; a failure aborts the take, records theft 3 and fires event 8.
	int fails = 0;
	int passes = 0;
	for (std::uint64_t seed = 1; seed <= 16; ++seed)
	{
		const Take t = take(shared, seed, 0.0, &chance);
		test::require(near(chance, 0.3), "thievery 0 at the box is not 30%");
		if (t.taken)
		{
			++passes;
			test::require(t.theft && t.event8 == 0, "a passed roll must record theft and fire no event 8");
		} else {
			++fails;
			test::require(t.theft && t.event8 >= 1, "a failed roll did not record theft 3 and fire event 8");
		}
	}
	check(fails > 0 && passes > 0, "a 30% chance fails " + std::to_string(fails) + " and passes " + std::to_string(passes) + " of 16 seeds");
}

} // namespace

int main()
{
	formula();
	Rig shared(1);
	inspect(shared);
	roll(shared);
	std::printf("ksim_steal_chance: all checks passed\n");
	return 0;
}
