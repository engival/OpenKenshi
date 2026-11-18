// kenshi-k16.6.73: the orders of the runtime state (arrest, smash, construction, lock, crop, the standing goals; the dog's limb play and the alarm
// wait are saved from v110) are lost by a save. A save taken mid-order and loaded - into the running Simulation (reload) and into a fresh
// one (restore) - must leave the member either on its order again or free of the goal, never on a Routine goal with no order behind it (the scorer
// skips a member that holds a goal, so it would stand there for good). Each case gives the order, saves 2 s in, loads, runs 60 s and names the
// outcome: resumes (an order is held, or the goal's own state carried it) or drops (the member is free).
// A bare dt27 world: no navmesh, so the cases that walk are given the member already at the target.

#include "dt27_rig.hpp"

#include "ksim/buildings.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/crime.hpp"
#include "ksim/factions.hpp"
#include "ksim/goap.hpp"
#include "ksim/routines.hpp"

#include <functional>
#include <memory>
#include <sstream>

namespace {

using namespace ksim;
using namespace dt27;

constexpr int kRunTicks = 1200; // 60 s after the load

enum class Load { Reload, Fresh };

const char* const kLoadName[] = {"reload", "fresh"};

// The save, then the load: `Reload` puts it in the running Simulation after the world moved on, `Fresh` in a new one (a restart's load).
void saveAndLoad(Rig& rig, Load how)
{
	std::ostringstream out;
	rig.sim.save(out);
	const std::string bytes = out.str();
	rig.tick(100); // the world moves on: every stamp and order it made is ahead of the save
	std::istringstream in(bytes);
	if (how == Load::Reload)
	{
		rig.sim.reload(in);
	} else {
		Simulation fresh = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, rig.gd);
		std::destroy_at(&rig.sim); // not an assignment: the old world's queries must go before the world does, as a destructor orders them
		std::construct_at(&rig.sim, std::move(fresh));
		CombatSettings settings;
		settings.substeps = 2;
		rig.sim.enableCombat(rig.data, settings);
		rig.sim.setTaskLog([&rig](const std::string& line) { rig.log.push_back(line); });
	}
	rig.squad = rig.entityOf(rig.member).target<state::MemberOf>();
	rig.squad.set<Detail>(Detail{DetailLevel::Full}); // the load puts the bare squad back at token detail; the rig keeps it on the mesh
}

struct Verdict
{
	bool held = false; // an order serves the member
	std::uint16_t goal = 0;
};

Verdict after(Rig& rig, std::uint64_t who)
{
	rig.tick(kRunTicks);
	return {rig.sim.holdsOrder(who), rig.goalOf(who)};
}

void say(const char* name, Load how, const char* outcome)
{
	std::cout << "  " << name << " (" << kLoadName[static_cast<int>(how)] << "): " << outcome << "\n";
}

// ---- fixtures ----

std::string toolSid(Rig& rig)
{
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (record->intOr("item function", 0) == 9 && rig.data->item(record->stringID) != nullptr)
			return record->stringID;
	throw std::runtime_error("no ITEM of function 9");
}

void give(Rig& rig, std::uint64_t id, const std::string& sid, int slot)
{
	flecs::entity entity = rig.entityOf(id);
	state::Inventory pack = entity.get<state::Inventory>() != nullptr ? *entity.get<state::Inventory>() : state::Inventory{};
	state::Inventory::Entry entry;
	entry.itemSid = sid;
	entry.count = 1;
	entry.slot = slot;
	pack.items.push_back(std::move(entry));
	++pack.version;
	entity.set<state::Inventory>(std::move(pack));
}

std::uint64_t playerFaction(Rig& rig)
{
	std::uint64_t id = 0;
	ecsOf(rig.sim).each([&](const state::Id& key, const state::PlayerFaction&) { id = key.value; });
	return id;
}

// The rig's member joins the player's faction and a player building stands beside it (a construction site with nothing delivered, or a built one).
std::uint64_t playerSite(Rig& rig, bool built)
{
	std::string sid;
	ecsOf(rig.sim).each([&](const state::PlayerFaction&, const state::FactionState& faction) { sid = faction.templateSid; });
	state::Allegiance side;
	side.factionSid = sid;
	rig.entityOf(rig.member).set<state::Allegiance>(side);
	state::PlayerBuilding b;
	b.ownerFaction = playerFaction(rig);
	b.x = kX + 6.0;
	b.z = kZ;
	b.y = rig.fighterOf(rig.member).y;
	b.built = built;
	b.hp = built ? 100.0 : 0.0;
	b.maxHp = 100.0;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("ITEM")))
		if (const ItemInfo* info = rig.data->item(record->stringID); info != nullptr && info->function == 0 && b.mats.empty())
			b.mats.push_back({record->stringID, 100, built ? 100u : 60u});
	b.status = buildings::statusOf(b);
	flecs::entity entity = state::createInstance(ecsOf(rig.sim));
	const std::uint64_t id = entity.get<state::Id>()->value;
	entity.set<state::PlayerBuilding>(std::move(b));
	return id;
}

// ---- the cases: each leaves the order running, 2 s in, and names who holds it ----

// 118 / the guard's arrest order (arrestOrders_), given directly: it takes the member over and rides no goal.
void arrest(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	test::require(rig.sim.orderArrest(rig.member, rig.leader, 240), "arrest: refused");
	rig.tick(40);
	test::require(rig.sim.holdsOrder(rig.member), "arrest: no order held before the save");
	saveAndLoad(rig, how);
	const Verdict v = after(rig, rig.member);
	test::require(rig.exists(rig.member), "arrest: the member is gone");
	say("arrest order", how, v.held ? "resumes" : "drops (the planner takes the member)");
}

// kenshi-k16.6.200: the 240 arrest order is saved (state::ArrestStates, v109). The victim stands, so the order waits (isTargetDown, rwh.124): saved and
// loaded in that wait it must still be there, and lift the victim once it is down.
void arrestWaits(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	test::require(rig.sim.orderArrest(rig.member, rig.leader, goap::kLiftPersonSnatchingInTown), "arrest wait: refused");
	rig.tick(40);
	test::require(rig.sim.arrestOrdered(rig.member), "arrest wait: no order before the save");
	saveAndLoad(rig, how);
	test::require(rig.sim.arrestOrdered(rig.member), "arrest wait: the order is gone after the load");
	rig.tick(100);
	test::require(rig.sim.arrestOrdered(rig.member), "arrest wait: the order lapsed while the victim stood");
	rig.fighterOf(rig.leader).unconscious = true;
	rig.fighterOf(rig.leader).koTimer = 1.0e6;
	bool lifted = false;
	for (int i = 0; i < 400 && !lifted; ++i)
	{
		rig.tick(1);
		lifted = rig.fighterOf(rig.member).carrying == rig.leader;
	}
	test::require(lifted, "arrest wait: the loaded order did not lift the downed victim");
	say("arrest order waiting on a standing victim", how, "lifts it once down");
}

// 213 HUNT_BOUNTIES: the goal rides the saved Routine (chainTarget = the target) and re-issues the arrest from it.
void huntBounties(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	state::Routine routine;
	routine.goal = routines::task::HuntBounties;
	routine.chainTarget = rig.leader;
	routine.plan = {routines::task::HuntBounties};
	routine.since = rig.sim.clockTick();
	routine.nextScore = rig.sim.clockTick() + 100000;
	rig.entityOf(rig.member).set<state::Routine>(std::move(routine));
	rig.tick(60);
	test::require(rig.sim.holdsOrder(rig.member), "hunt: the goal gave no arrest order before the save");
	saveAndLoad(rig, how);
	const Verdict v = after(rig, rig.member);
	test::require(v.held || v.goal != routines::task::HuntBounties, "hunt: stuck on HUNT_BOUNTIES with no order");
	say("arrest under goal HUNT_BOUNTIES", how, v.held ? "resumes (re-issued from the saved goal)" : "drops");
}

// 272 SMASH (smashOrders_): the player's order on a player building; the building itself is saved.
void smash(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const std::uint64_t site = playerSite(rig, true);
	test::require(rig.sim.orderSmash(rig.member, site), "smash: refused");
	rig.tick(40);
	test::require(rig.sim.holdsOrder(rig.member), "smash: no order held before the save");
	saveAndLoad(rig, how);
	const Verdict v = after(rig, rig.member);
	say("smash order", how, v.held ? "resumes" : "drops (the order is the player's: the member is free)");
}

// 2 BUILD (buildOrders_): the player's order on a construction site.
void build(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const std::uint64_t site = playerSite(rig, false);
	test::require(rig.sim.orderConstructionTask(rig.member, routines::task::Build, site), "build: refused");
	rig.tick(40);
	test::require(rig.sim.holdsOrder(rig.member), "build: no order held before the save");
	saveAndLoad(rig, how);
	const Verdict v = after(rig, rig.member);
	say("construction order", how, v.held ? "resumes" : "drops (the order is the player's: the member is free)");
}

// 185 CUT_SHACKLES (lockOrders_): the lock state itself is saved (v87), the order is not.
void lock(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	give(rig, rig.member, rig.gd->find("SHACKLES")->stringID, state::Inventory::kArmourBase + 9);
	give(rig, rig.member, toolSid(rig), state::Inventory::kCarried);
	rig.fighterOf(rig.member).slaveState = 1;
	rig.fighterOf(rig.member).skills.strength = 100.0;
	test::require(rig.sim.orderLockTask(rig.member, routines::task::CutShackles, 0), "lock: refused");
	rig.tick(40);
	test::require(rig.sim.holdsOrder(rig.member), "lock: no order held before the save");
	saveAndLoad(rig, how);
	const Verdict v = after(rig, rig.member);
	say("lock order", how, v.held ? "resumes" : "drops (the order is the player's: the member is free)");
}

// 231 EAT_CROPS (cropOrders_): a goal on the saved Routine with the order unsaved.
void crops(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const std::vector<std::uint64_t> plots = rig.sim.cropPlots();
	test::require(!plots.empty(), "crops: no farm plot in the world");
	flecs::world& ecs = ecsOf(rig.sim);
	std::uint64_t plotId = plots.front();
	state::CropPlot plot;
	flecs::entity plotEntity;
	ecs.each([&](flecs::entity entity, const state::Id& key, const state::CropPlot& p)
	{
		if (key.value == plotId)
		{
			plot = p;
			plotEntity = entity;
		}
	});
	state::Fighter& f = rig.fighterOf(rig.member);
	f.x = plot.x;
	f.z = plot.z;
	f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	std::uint64_t own = 0;
	ecs.each([&](flecs::entity entity, const state::FactionState&)
	{
		own = own == 0 ? entity.id() : own;
	});
	f.faction = own;
	flecs::entity_t hostile = 0; // the town's owner: a faction the member does not like
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("FACTION")))
	{
		const flecs::entity candidate = ecs.entity();
		state::FactionState faction;
		faction.templateSid = record->stringID;
		candidate.set<state::FactionState>(std::move(faction));
		candidate.set<state::Id>(state::Id{(1ull << 58) + candidate.id()});
		if (candidate.id() != own && factionRelation(*rig.gd, flecs::entity(ecs.c_ptr(), own), candidate) < crime::kFriendlyRelation)
		{
			hostile = candidate.id();
			break;
		}
		candidate.destruct();
	}
	test::require(hostile != 0, "crops: no faction unfriendly to the member");
	state::TownState town;
	town.rootSid = town.activeSid = rig.gd->byType(kworld::itemTypeByName("TOWN")).front()->stringID;
	town.owner = state::EntityRef{hostile};
	town.x = plot.x;
	town.z = plot.z;
	const flecs::entity townEntity = ecs.entity();
	townEntity.set<state::TownState>(std::move(town));
	townEntity.set<state::Id>(state::Id{(1ull << 58) + townEntity.id()});
	plot.town = state::EntityRef{townEntity.id()};
	plotEntity.set<state::CropPlot>(plot);
	f.hunger = 0.5;
	f.foodCharges = 0.0;
	test::require(rig.sim.orderEatCrops(rig.member), "crops: the eat order was refused");
	rig.tick(40);
	test::require(rig.sim.holdsOrder(rig.member) && rig.goalOf(rig.member) == routines::task::EatCrops, "crops: no order and goal before the save");
	saveAndLoad(rig, how);
	test::require(rig.goalOf(rig.member) == routines::task::EatCrops, "crops: the goal is not in the save");
	const Verdict v = after(rig, rig.member);
	test::require(v.held || v.goal != routines::task::EatCrops,
		std::string("crops (") + kLoadName[static_cast<int>(how)] + "): stuck on EAT_CROPS with no order for 60 s");
	say("crop order (231)", how, v.held ? "resumes" : "drops (the goal ends with the order)");
}

// 24 / 30 / 19 / 62: the standing goals ride the Routine, their place (standingRun_) is not saved.
void standing(Load how, std::uint16_t goal)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const state::Fighter& f = rig.fighterOf(rig.member);
	if (goal == routines::task::Wanderer)
		makeTown(rig, 2, kX + 6000.0, kZ); // a bare world has no town to walk to
	test::require(goal == routines::task::HoldPosition ? rig.sim.orderStandingGoal(rig.member, goal, f.x + 300.0, f.z)
		: rig.sim.orderStandingGoal(rig.member, goal), "standing: refused");
	rig.tick(40);
	test::require(rig.goalOf(rig.member) == goal && rig.sim.holdsOrder(rig.member), "standing: no order and goal before the save");
	saveAndLoad(rig, how);
	const Verdict v = after(rig, rig.member);
	const std::string name = "standing goal " + std::to_string(goal);
	test::require(v.held || v.goal != goal, name + ": stuck with no place for 60 s");
	say(name.c_str(), how, v.held ? "resumes" : "drops (the goal ends: the place is not saved)");
}

// 278 PLAY_BECAUSE_I_HAVE_A_LIMB_IN_MOUTH: limbPlay_ (the current destination and the pick count) is saved from v110 (state::RunStates): the run goes on
// to the same destination, not a re-pick.
void limbPlay(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	rig.fighterOf(rig.member).animal = true;
	std::string limb;
	for (const auto& [sid, info] : rig.data->items)
		if (info.function == kItemFunctionLimb && (limb.empty() || sid < limb))
			limb = sid;
	test::require(!limb.empty(), "limb: no limb item");
	give(rig, rig.member, limb, state::Inventory::kCarried);
	test::require(rig.sim.carriesLimb(rig.member), "limb: not carrying");
	test::require(rig.sim.orderAnimalGoal(rig.member, routines::task::PlayWithLimbInMouth), "limb: refused");
	rig.tick(40);
	test::require(rig.sim.limbPlayPicks(rig.member) >= 1, "limb: no destination before the save");
	const std::pair<double, double> where = rig.sim.limbPlayDestination(rig.member);
	const std::uint32_t picks = rig.sim.limbPlayPicks(rig.member);
	saveAndLoad(rig, how);
	test::require(rig.sim.limbPlayDestination(rig.member) == where && rig.sim.limbPlayPicks(rig.member) == picks, "limb: the run's destination is not in the save");
	const Verdict v = after(rig, rig.member);
	test::require(v.goal == routines::task::PlayWithLimbInMouth && rig.sim.limbPlayPicks(rig.member) >= picks, "limb: the play did not go on with the limb in the mouth");
	say("limb play (278)", how, "resumes (the destination and pick count are saved)");
}

// 277's cooldown (limbCooldown_, AI+0x318): the fetch that took a limb shuts the finder for a while; a load must keep it shut for the same time.
void limbCooldown(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	state::Fighter& f = rig.fighterOf(rig.member);
	f.animal = true;
	f.hunger = 1.0;
	std::string limb;
	for (const auto& [sid, info] : rig.data->items)
		if (info.function == kItemFunctionLimb && (limb.empty() || sid < limb))
			limb = sid;
	test::require(!limb.empty(), "cooldown: no limb item");
	state::GroundItem item;
	item.itemSid = limb;
	item.x = item.useX = f.x + 8.0;
	item.y = item.useY = f.y;
	item.z = item.useZ = f.z;
	rig.sim.spawnGroundItem(std::move(item));
	test::require(rig.sim.orderAnimalGoal(rig.member, routines::task::AnimalFetchALimb), "cooldown: the finder named no limb");
	for (int i = 0; i < 600 && !rig.sim.carriesLimb(rig.member); ++i)
		rig.tick(1);
	test::require(rig.sim.carriesLimb(rig.member), "cooldown: the dog never took the limb");
	rig.tick(10);
	const std::uint64_t until = rig.sim.limbCooldownUntil(rig.member);
	test::require(until > rig.sim.clockTick(), "cooldown: none after the fetch");
	saveAndLoad(rig, how);
	test::require(rig.sim.limbCooldownUntil(rig.member) == until, "cooldown: not in the save");
	say("limb fetch cooldown (277)", how, "resumes (the tick the finder opens again is saved)");
}

// 209 INVESTIGATE_ALARMS: the board (an alarm lives 8 s) and the wait are saved from v110: the investigator is still on the alarm with the same
// seconds left on its wait.
void alarmWait(Load how)
{
	Rig rig({{routines::task::WorkTheSlaves}}, 0);
	const state::Fighter& f = rig.fighterOf(rig.member);
	makeTown(rig, 2, f.x, f.z); // an alarm belongs to a town's manager
	rig.tick(220); // the arena's town circles refresh every 10 s
	const alarm::Alarm* raised = rig.sim.raiseAlarm(crime::kTrespassing, f.x + 50.0, f.z, rig.leader);
	test::require(raised != nullptr, "alarm: not raised");
	const std::uint64_t alarmId = raised->id;
	state::Routine routine;
	routine.goal = routines::task::InvestigateAlarms;
	routine.chainTarget = alarmId;
	routine.plan = {routines::task::InvestigateAlarms};
	routine.since = rig.sim.clockTick();
	routine.nextScore = rig.sim.clockTick() + 100000;
	rig.entityOf(rig.member).set<state::Routine>(std::move(routine));
	rig.tick(40);
	test::require(rig.goalOf(rig.member) == routines::task::InvestigateAlarms, "alarm: the goal ended before the save");
	const double left = rig.sim.alarmDwellLeft(rig.member);
	test::require(left > 0.0 && rig.sim.alarmCount() == 1, "alarm: no wait running before the save");
	saveAndLoad(rig, how);
	test::require(rig.sim.alarmCount() == 1 && rig.sim.alarmDwellLeft(rig.member) == left, "alarm: the board and the wait are not in the save");
	rig.tick(1);
	test::require(rig.goalOf(rig.member) == routines::task::InvestigateAlarms, "alarm: the goal ended on the loaded alarm");
	say("alarm wait (209)", how, "resumes (the board and the wait are saved)");
}

} // namespace

int main()
{
	try
	{
		for (const Load how : {Load::Reload, Load::Fresh})
		{
			arrest(how);
			arrestWaits(how);
			huntBounties(how);
			smash(how);
			build(how);
			lock(how);
			crops(how);
			for (const std::uint16_t goal : {routines::task::Wanderer, routines::task::HoldPosition, routines::task::StandStill})
				standing(how, goal);
			limbPlay(how);
			limbCooldown(how);
			alarmWait(how);
		}
		std::cout << "ksim_save_mid_order: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_save_mid_order: FAILED: " << e.what() << "\n";
		return 1;
	}
}
