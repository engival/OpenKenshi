// Lockpick order sequence as the original (kenshi-k16.6.324 / .323; CRIME_STATIC.md "2 Burglary"): the order walks to the lock with no crime and
// no witness reaction; the crime and the reaction start when picking starts; the reaction does not end the order; a 0% order walks to the lock, never starts
// picking (no crime, no reaction) and ends refused there (kenshi-k16.6.366); a targeting stops the running task and the order restarts from its beginning, no fight alongside (kenshi-k16.6.335, rwh.165).

#include "ksim/clock.hpp"
#include "ksim/crime.hpp"
#include "ksim/door_plan.hpp"
#include "ksim/factions.hpp"
#include "ksim/interact.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace cr = ksim::crime;
namespace ks = ksim::simlink;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

struct Data
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::string servant;
	std::string holy;
	std::string nameless;

	Data()
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		data = CombatData::build(*gd, KENSHI_DATA);
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
		test::require(!servant.empty() && !holy.empty() && !nameless.empty(), "missing data records");
	}
};

struct Rig
{
	const Data& w;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;
	std::uint64_t thief = 0;
	std::uint64_t rng = 3;
	std::vector<cr::DialogueEvent> events;
	std::vector<InteractEvent> interacts;
	flecs::entity box;
	std::uint64_t boxId = 0;
	double boxX = 0.0;
	double boxZ = 0.0;

	Rig(const Data& world, const std::shared_ptr<MeshNavigator>& mesh, const kworld::LoadOrder& order)
		: w(world)
		, nav(mesh)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(w.gd);
		sim->setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		context.gameData = w.gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(w.data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *w.gd, *w.data, context);
		thief = duel.player;
		f(duel.enemy).team = 0; // nobody fights unless a test asks
		f(duel.enemy).x += 200.0; // out of the way
		sim->setInteractObserver([this](const InteractEvent& e) { interacts.push_back(e); });
		sim->setDialogueObserver([this](const cr::DialogueEvent& e) { events.push_back(e); });
		entity(thief).set<state::Allegiance>(state::Allegiance{w.nameless});
		f(thief).npcClass = 9;
		f(thief).attention = 0.5;
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	flecs::entity entity(std::uint64_t id)
	{
		flecs::entity found;
		ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == id) found = e; });
		return found;
	}
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// A guard of `faction` standing at (x, z), looking along +z.
	std::uint64_t guard(const std::string& faction, double x, double z)
	{
		state::Fighter g = w.data->makeFighter(*w.gd, w.servant, "", rng);
		g.team = 0;
		g.x = x;
		g.z = z;
		g.y = f(thief).y;
		g.yaw = 0.0;
		g.npcClass = 1;
		g.attention = 0.5;
		flecs::entity e = sim->spawnFighter(std::move(g));
		e.set<state::Allegiance>(state::Allegiance{faction});
		return e.get<state::Id>()->value;
	}
	// A locked container of `owner` `dz` metres ahead of the thief on the navmesh, and the pick order the client gives (interact action 3, routed to
	// the use point). `skill` and `level` give the chance (pickChance).
	void order(const std::string& owner, double dz, double skill, double level)
	{
		state::Fighter& me = f(thief);
		const Route ahead = nav->routeFrom({me.x, me.y, me.z}, me.x, me.z + dz);
		test::require(ahead.found && !ahead.points.empty(), "no route ahead of the thief");
		state::Container c;
		c.recordSid = "box";
		c.x = c.useX = boxX = ahead.points.back().x;
		c.z = c.useZ = boxZ = ahead.points.back().z;
		c.y = c.useY = ahead.heights.back();
		c.ownerSid = owner;
		c.locked = true;
		c.lockLevel = level;
		box = sim->spawnContainer(c, state::Inventory{});
		boxId = box.get<state::Id>()->value;
		me.skills.lockpicking = skill;
		const InteractTarget target = findInteractTarget(ecs(), sim->combat(), boxId);
		const Route route = routeToInteract(*nav, me, target, 3, false, sim->pathRadius(me));
		test::require(route.found, "no route to the box");
		sim->enqueue(Command::fighterInteract(state::Id{thief}, boxId, 3, 1, route.points, route.heights, route.approach));
	}
	bool picking() { return entity(thief).has<state::DoorStep>(); }
	bool ordered() { return entity(thief).has<state::InteractTask>(); }
	const state::Container& lock() { return *box.get<state::Container>(); }
	std::uint64_t witnessed() const { return sim->dialogueEventCount(cr::kEvWitnessThiefOrLockpick); }
	bool crimeOnThief() const
	{
		const state::CrimeRecord* r = sim->crimeRecord(thief);
		return r != nullptr && r->crime != cr::kNone;
	}
};

constexpr double kSkill = 1.0;
constexpr double kLevel = 40.0; // chance 0.9 / 2^3.9 = 6%: low, but above the planner's floor (kMinPickChance)

void walkThenPick(const Data& w, const std::shared_ptr<MeshNavigator>& nav, const kworld::LoadOrder& order)
{
	Rig rig(w, nav, order);
	const state::Fighter start = rig.f(rig.thief);
	rig.guard(w.holy, start.x, start.z - 4.0); // looks along +z: the thief and the box are in its view the whole way
	rig.tick(20 * 3); // the sensors settle
	rig.order(w.holy, 14.0, kSkill, kLevel);
	bool walked = false;
	bool crimeInWalk = false;
	for (int i = 0; i < 20 * 30 && !rig.picking(); ++i)
	{
		rig.tick();
		walked = walked || rig.f(rig.thief).walking();
		if (!rig.picking() && (rig.crimeOnThief() || rig.witnessed() > 0))
			crimeInWalk = true;
	}
	check(walked, "1: the order walked to the lock");
	check(rig.picking(), "1: the pick started on arrival");
	check(!crimeInWalk, "1: no crime record and no witness reaction during the walk");
	rig.tick(20 * 3);
	const state::CrimeRecord* r = rig.sim->crimeRecord(rig.thief);
	check(r != nullptr && r->crime == cr::kBurglary && r->victimFaction == w.holy, "2: the pick records Burglary against the owner's faction");
	check(rig.witnessed() > 0, "2: the owner-faction guard witnessed it (EV_WITNESS_THIEF_OR_LOCKPICK)");
	rig.tick(20 * 3);
	check(rig.ordered() && rig.picking(), "3: the witness reaction did not end the lockpick order");
	check(rig.lock().lockElapsed > 0.0 || rig.lock().lockAttempts > 0, "3: the pick keeps ticking after the reaction");
}

void zeroChance(const Data& w, const std::shared_ptr<MeshNavigator>& nav, const kworld::LoadOrder& order)
{
	Rig rig(w, nav, order);
	const state::Fighter start = rig.f(rig.thief);
	rig.guard(w.holy, start.x, start.z - 4.0);
	rig.tick(20 * 3);
	check(pickChance(100.0, kSkill) < kMinPickChance, "4: the setup chance is below the floor (a 0% display)");
	rig.order(w.holy, 14.0, kSkill, 100.0);
	const auto distance = [&]() { return std::hypot(rig.f(rig.thief).x - rig.boxX, rig.f(rig.thief).z - rig.boxZ); };
	const double first = distance();
	rig.tick(); // the command is applied on the tick
	check(rig.ordered() || rig.f(rig.thief).walking(), "4: the order is accepted whatever the chance (not refused at once)");
	bool started = false;
	bool crime = false;
	bool walked = false;
	for (int i = 0; i < 20 * 30; ++i)
	{
		rig.tick();
		walked = walked || rig.f(rig.thief).walking();
		started = started || rig.picking();
		crime = crime || rig.crimeOnThief() || rig.witnessed() > 0;
	}
	check(walked && distance() < first - 5.0, "4: the order walked to the lock (distance " + std::to_string(first) + " -> " + std::to_string(distance()) + ")");
	check(!crime, "4: no crime and no reaction");
	check(rig.lock().lockElapsed == 0.0 && rig.lock().lockAttempts == 0, "4: no lock roll");
	bool rolled = false;
	int refused = 0;
	for (const InteractEvent& e : rig.interacts)
	{
		rolled = rolled || e.kind == InteractEvent::Kind::LockRoll;
		refused += e.kind == InteractEvent::Kind::Done && e.action == 3 && e.result == InteractEvent::kRefused;
	}
	check(!rolled, "4: no LockRoll event");
	check(refused == 1, "4: the order ended once, refused at the lock (event 13 result 1)");
	check(!rig.ordered() && !rig.picking(), "4: the order ended cleanly: no order, no pick step left");
	check(!rig.f(rig.thief).walking(), "4: the character idles (no re-plan loop)");
}

// Attacked mid-order, the original (rwh.165): the character runs one task, so it never fights while the order runs (it is only hit); a targeting or a hit
// stops the task and the order starts again from its beginning (stepOrderCombat), the shared lock's progress stays; the order is never dropped.
void attackedMidPick(const Data& w, const std::shared_ptr<MeshNavigator>& nav, const kworld::LoadOrder& order)
{
	Rig rig(w, nav, order);
	rig.tick(20);
	rig.order(w.nameless, 8.0, kSkill, kLevel); // the thief's own faction: no crime, so the only reaction under test is the fight
	for (int i = 0; i < 20 * 30 && !rig.picking(); ++i)
		rig.tick();
	check(rig.picking(), "fight: the pick started");
	rig.tick(20);
	state::Fighter& me = rig.f(rig.thief);
	state::Fighter& foe = rig.f(rig.duel.enemy);
	foe.team = 1;
	foe.x = me.x + 1.5;
	foe.z = me.z;
	foe.y = me.y;
	foe.target = rig.thief;
	foe.state = state::CombatState::Attack;
	const double start = rig.lock().lockElapsed;
	int fought = 0;
	int stopped = 0;
	for (int i = 0; i < 20 * 4; ++i)
	{
		rig.tick();
		fought += me.target != 0;
		stopped += me.orderPause > 0;
	}
	check(fought == 0, "fight: the picker never fights while the order runs (" + std::to_string(fought) + " ticks with a target)");
	check(stopped > 0, "fight: being targeted / hit stopped the task (" + std::to_string(stopped) + " ticks)");
	check(rig.ordered(), "fight: the order stays queued");
	check(rig.lock().lockElapsed > start || rig.lock().lockAttempts > 0, "fight: the lock's progress kept advancing across the restarts");
	foe.dead = true;
	me.engaged.clear();
	rig.tick(20);
	check(rig.ordered() || !rig.lock().locked, "fight: the order is still there (or done) after the attacker died");
}

// The lift (Task_LiftSomething, id 170): the same rule, no per-kind code. The lift's own timer is task-local, so each hit starts it again; it completes
// once nobody targets the carrier.
void attackedMidLift(const Data& w, const std::shared_ptr<MeshNavigator>& nav, const kworld::LoadOrder& order)
{
	Rig rig(w, nav, order);
	rig.tick(20);
	const std::uint64_t bodyId = rig.guard(w.nameless, rig.f(rig.thief).x, rig.f(rig.thief).z + 3.0);
	state::Fighter& me = rig.f(rig.thief); // after the spawn: it moves the arena's storage
	state::Fighter& body = rig.f(bodyId);
	body.unconscious = true;
	body.koTimer = 1e6;
	body.hold = true;
	const InteractTarget target = findInteractTarget(rig.ecs(), rig.sim->combat(), bodyId);
	const Route route = routeToInteract(*rig.nav, me, target, 8, false, rig.sim->pathRadius(me));
	test::require(route.found, "no route to the body");
	rig.sim->enqueue(Command::fighterInteract(state::Id{rig.thief}, bodyId, 8, 1, route.points, route.heights, route.approach));
	for (int i = 0; i < 20 * 30 && !rig.entity(rig.thief).has<state::CarryStep>(); ++i)
		rig.tick();
	check(rig.entity(rig.thief).has<state::CarryStep>(), "lift: the lift started");
	state::Fighter& foe = rig.f(rig.duel.enemy);
	foe.team = 1;
	foe.x = me.x + 1.5;
	foe.z = me.z;
	foe.y = me.y;
	foe.target = rig.thief;
	foe.state = state::CombatState::Attack;
	int fought = 0;
	int stopped = 0;
	for (int i = 0; i < 20 * 3; ++i)
	{
		rig.tick();
		fought += me.target != 0;
		stopped += me.orderPause > 0;
	}
	check(fought == 0, "lift: the carrier never fights while the order runs (" + std::to_string(fought) + " ticks with a target)");
	check(stopped > 0, "lift: being targeted / hit stopped the task (" + std::to_string(stopped) + " ticks)");
	foe.dead = true;
	me.engaged.clear();
	rig.tick(20 * 5);
	check(me.carrying == bodyId, "lift: the order was not dropped; the lift completed after the attacker died");
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try
	{
		test::require(argc == 2, "usage: ksim_lockpick_seq HUB.knav");
		const Data w;
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		walkThenPick(w, nav, order);
		zeroChance(w, nav, order);
		attackedMidPick(w, nav, order);
		attackedMidLift(w, nav, order);
		std::printf("ksim_lockpick_seq: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::printf("FAIL: %s\n", e.what());
		return 1;
	}
}
