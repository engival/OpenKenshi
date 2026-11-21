// Arrest chain, sentence, release and smash (task ds/arrest; CRIME_STATIC "What guards do (arrest chain)"):
//   - 203 as one order: a guard of the cage owner's faction lifts a KO'd person (240), carries it to a free cage and puts it in
//     (imprison), where the sentence starts from the CAGE OWNER's legal faction's bounty only (another faction's bounty gives none).
//   - 211: a served prisoner is freed by a law-enforcement NPC through 110 (an unserved one is not); the bounty goes with it.
//   - 272: a smash order bashes a player building down to a ruin.
//   - kenshi-k16.6.6 (crime-rows): the town alarm manager and 209 / 210, 212 LOOT_STOLEN_GOODS, 211's town-wide finder, 204's group test.
// The Heft world at Full detail, like ksim_slave_goals.

#include "ksim/ai/ai.hpp"
#include "ksim/law/alarm.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/sensor.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/core/detail.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/world/world.hpp"

#include "knav/navmesh.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// Heft's grid cell (the baked world's region_11_4.knav) and its town centre (the cases run inside its 2000 u cage finder).
constexpr int kHeftCellX = 46;
constexpr int kHeftCellZ = 16;
constexpr double kHeftX = 68772.98;
constexpr double kHeftZ = -71496.61;
// The kidnapper template (Squad role carries both 254 and 112), placed as the town's slavers in the 254 case.
// The slave-carrying guard template (its Slave list carries 172): the UC storehouses' 'Storage Guard' package.
constexpr const char* kSlaverGuards = "56041-rebirth.mod";
// The wandering-trader caravan (the 39 case).

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i)
	{
		if (i.value == id)
			found = e;
	});
	return found;
}

std::vector<flecs::entity> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each(
		[&](flecs::entity member, const state::Id& id) { found.emplace_back(id.value, member); });
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<flecs::entity> out;
	for (const auto& [id, member] : found)
		out.push_back(member);
	return out;
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

// The FactionState of a faction record (the cage owner, the captor).
struct FactionLookup
{
	std::uint64_t entity = 0; // the flecs entity id
	std::uint64_t id = 0; // its state::Id value (the id a Cage's faction and a captor's resolve hold)
	std::string sid;
};

FactionLookup factionBySid(Simulation& sim, const std::string& sid)
{
	FactionLookup out;
	ecsOf(sim).each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (faction.templateSid != sid)
			return;
		out.entity = entity.id();
		out.sid = faction.templateSid;
		const state::Id* id = entity.get<state::Id>();
		out.id = id != nullptr ? id->value : static_cast<std::uint64_t>(entity.id());
	});
	return out;
}

struct Game
{
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	std::vector<std::string> taskLog;

	explicit Game(const std::string& bake)
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({kHeftCellX + dx, kHeftCellZ + dz});
		mesh = test::openWorldNav(bake, zones);
		SpawnOptions options;
		options.seed = 1;
		options.wildernessPopulation = 0.0;
		options.nestMultiplier = 0.0;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
		sim->setTaskLog([this](const std::string& line) { taskLog.push_back(line); });
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				sid = record->stringID;
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}

	// The player's camera is the squad's spot (the area streams to Full and the world squads build Fighters); the player CHARACTER
	// stands well away, so the placed squad has nobody to fight.
	void bringPlayer(double x, double y, double z)
	{
		if (state::Fighter* me = fighterOf(*sim, player))
		{
			me->x = x + 400.0;
			me->y = y;
			me->z = z + 400.0;
			me->clearRoute();
			me->down = false;
			me->unconscious = false;
		}
	}

	flecs::entity place(const char* templateSid, double x, double z)
	{
		const flecs::entity squad = sim->placeWildernessSquad(templateSid, x, z);
		bringPlayer(x, sim->groundHeight(x, z).value_or(0.0), z);
		sim->enqueue(Command::setPlayerAreas({{x, z}}));
		sim->run(1);
		for (int i = 0; i < 20 * 20; ++i)
		{
			sim->run(1);
			const std::vector<flecs::entity> members = membersOf(*sim, squad);
			if (!members.empty() && std::all_of(members.begin(), members.end(),
				[&](flecs::entity member)
				{
					const state::Id* id = member.get<state::Id>();
					return id != nullptr && sim->combat()->find(id->value) != nullptr;
				}))
				break;
		}
		return squad;
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->run(1);
	}
};

// The cage-bearing home the two Heft cases use: the resident Cage Warden squads' home building (a slave shop whose placement is the
// Slave Traders'). Its building sites are materialised by the selector itself, so the test only needs where it stands.
struct CageHome
{
	std::uint64_t building = 0;
	double x = 0.0;
	double z = 0.0;
	FactionLookup owner;
};

bool findCageHome(Simulation& sim, CageHome& out)
{
	bool found = false;
	ecsOf(sim).each([&](flecs::entity, const state::SquadInstance& instance)
	{
		if (found || instance.homeBuilding == 0 || instance.templateSid.empty())
			return;
		const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
		if (mapping.packageName != "Cage Warden")
			return;
		const BuildingSite* site = sim.world().buildingSite(instance.homeBuilding);
		if (site == nullptr || std::hypot(site->cx - kHeftX, site->cz - kHeftZ) > 2000.0)
			return; // the Heft slave shops are the loaded ones; the world's other shops have no mesh here
		out.building = instance.homeBuilding;
		out.x = site->cx;
		out.z = site->cz;
		if (!site->ownerFaction.empty())
			out.owner = factionBySid(sim, site->ownerFaction);
		if (out.owner.id == 0 && instance.faction.id != 0)
		{
			const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
			if (faction != nullptr)
				out.owner = factionBySid(sim, faction->templateSid);
		}
		found = out.owner.id != 0;
	});
	return found;
}


std::uint64_t spawnBody(Simulation& sim, std::uint64_t like, double x, double z)
{
	state::Fighter body = *sim.combat()->find(like);
	body.playerControlled = false;
	body.dead = false;
	body.down = false;
	body.unconscious = true;
	body.koTimer = 1.0e6;
	body.asleep = false;
	body.carrying = 0;
	body.carriedBy = 0;
	body.cageBuilding = 0;
	body.cageSlot = 0;
	body.slaveState = 0;
	body.captorSid.clear();
	body.captorFaction = 0;
	body.target = 0;
	body.orderTarget = 0;
	body.faction = 0;
	body.team = 0;
	body.animal = false;
	body.goal = state::IndividualGoal{};
	body.routeSeq = 0;
	// A body of its own squad: copied from the guard it would share the guard's squad key, and the first squad-internal targeting puts that key
	// on the guard's own hostile list for good (Ctx::setTarget), so the guard never stops treating its prisoners as enemies (211 release).
	body.hostileSquads.clear();
	body.aggressorSquads.clear();
	body.engaged.clear();
	body.clearRoute();
	body.x = x;
	body.z = z;
	body.y = sim.groundHeight(x, z).value_or(body.y);
	const flecs::entity entity = sim.spawnFighter(std::move(body));
	entity.set<state::Allegiance>(state::Allegiance{"test-victim"});
	fighterOf(sim, idOf(entity))->squad = (1ull << 20) | idOf(entity);
	return idOf(entity);
}

// The faction that holds a different bounty than the cage owner's legal faction.
std::string otherHolder(Simulation& sim, const std::string& legal)
{
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("FACTION")))
		if (record->boolOr("offers bounties", false) && record->stringID != legal)
			return record->stringID;
	throw std::runtime_error("no second faction that offers bounties");
}

bool arrest(Game& game, std::uint64_t guard, std::uint64_t victim, std::uint16_t lift, int maxTicks)
{
	Simulation& sim = *game.sim;
	test::require(sim.orderArrest(guard, victim, lift), "orderArrest refused");
	for (int i = 0; i < maxTicks; ++i)
	{
		game.tick();
		const state::Fighter* f = sim.combat()->find(victim);
		if (f != nullptr && f->cageBuilding != 0)
			return true;
		if (sim.arrestOrderCount() == 0)
			break;
	}
	for (const std::string& line : game.taskLog)
		if (line.rfind("arrest", 0) == 0 || line.rfind("imprison", 0) == 0 || line.find("member " + std::to_string(guard) + " ") != std::string::npos)
			std::cout << "  log: " << line << "\n";
	const state::Fighter* gf = sim.combat()->find(guard);
	const state::Fighter* vf = sim.combat()->find(victim);
	std::cout << "arrest stalled: orders " << sim.arrestOrderCount() << " guard carrying " << gf->carrying << " (victim " << victim
			  << ") victim carriedBy " << vf->carriedBy << " unconscious " << vf->unconscious << " dist " << std::hypot(gf->x - vf->x, gf->z - vf->z)
			  << " guard dead " << gf->dead << " down " << gf->down << " ko " << gf->unconscious << "\n";
	return vf->cageBuilding != 0;
}

void testArrest(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");
	const std::string legal = crime::legalFaction(sim.gameData().get(), home.owner.sid);
	test::require(!legal.empty(), "the cage owner has a legal faction");
	const std::string other = otherHolder(sim, legal);

	const flecs::entity squad = game.place(kSlaverGuards, home.x + 40.0, home.z + 40.0);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(members.size() >= 2, "the guard squad was built");
	const std::uint64_t guard = idOf(members.back());
	{
		fighterOf(sim, guard)->faction = home.owner.id;
		entityOf(sim, guard).set<state::Allegiance>(state::Allegiance{home.owner.sid});
		entityOf(sim, guard).remove<state::Routine>();
	}
	const state::Fighter& g = *sim.combat()->find(guard);
	// The guard's squad-mates (a slaver squad, its package lifts a downed person: the world selector's 168) are held down for good: which of them
	// reaches the body before the guard's order does follows the phase of each one's pass (the 1 Hz passes run on id % 20 slices, k16.6.320), and
	// this test is about the guard's order, as its own Routine is removed above.
	for (const flecs::entity& mate : members)
		if (idOf(mate) != guard)
		{
			state::Fighter* held = fighterOf(sim, idOf(mate));
			held->unconscious = true;
			held->koTimer = 1.0e6;
		}

	// 1. The cage owner's legal faction holds a bounty: the sentence starts at the put-in (int(amount x 0.02 x prison time) hours).
	const std::uint64_t first = spawnBody(sim, guard, g.x + 15.0, g.z);
	sim.addBounty(first, legal, 1000);
	test::require(arrest(game, guard, first, goap::kLiftPersonSnatchingInTown, 6000), "203: the target was not put in a cage");
	const state::Fighter* caged = sim.combat()->find(first);
	test::require(caged->cageBuilding == home.building, "203: the cage is the home's");
	const state::CrimeRecord* record = sim.crimeRecord(first);
	test::require(record != nullptr && record->sentenceHours == crime::sentenceHours(1000) && record->sentenceHours > 0.0,
		"112: the cage owner's legal bounty started the sentence");
	test::require(g.carrying == 0, "203: the guard still carries");
	test::require(!sim.prisonerServed(first), "211: a prisoner still serving its sentence counts as served");
	std::cout << "203 arrest ok, sentence " << record->sentenceHours << " h\n";

	// 2. A bounty from another faction only: caged, but nothing sentences it.
	const std::uint64_t second = spawnBody(sim, guard, g.x + 15.0, g.z);
	sim.addBounty(second, other, 5000);
	test::require(arrest(game, guard, second, goap::kLiftPersonSnatchingInTown, 6000), "203: the second target was not put in a cage");
	const state::CrimeRecord* none = sim.crimeRecord(second);
	test::require(none != nullptr && none->sentenceHours == 0.0 && none->sentenceStart == 0.0,
		"112: another faction's bounty started a sentence");
	std::cout << "no sentence from another faction's bounty ok\n";

	// 3. 211: a law-enforcement NPC frees the served prisoner; the one still serving stays.
	fighterOf(sim, guard)->npcClass = crime::kClassLaw;
	fighterOf(sim, guard)->x = sim.combat()->find(first)->x + 3.0;
	fighterOf(sim, guard)->z = sim.combat()->find(first)->z;
	// One release scan (every 20 ticks) decides both prisoners, so they wake the tick before it: `first` has served its sentence, `second` (given
	// `first`'s record as it stood: 20 h, just started) has not. A longer wait would leave the idle guard's town time to process the conscious, unchained
	// prisoners as new slaves (260 -> 182: the slaver packages of the world's squads), which is not what 211 is about (k16.6.137: the lift takes 2.3 s,
	// the earlier stages end later and a slaver squad is near by then, so waking early and waiting for a second scan lost the race).
	state::CrimeRecord serving = *sim.crimeRecord(first);
	entityOf(sim, second).set<state::CrimeRecord>(serving);
	state::CrimeRecord served = serving;
	served.sentenceStart = -1000.0;
	entityOf(sim, first).set<state::CrimeRecord>(served);
	while ((sim.clockTick() + 1) % 20 != 0)
		game.tick();
	fighterOf(sim, first)->unconscious = false;
	fighterOf(sim, first)->koTimer = 0.0;
	fighterOf(sim, second)->unconscious = false;
	fighterOf(sim, second)->koTimer = 0.0;
	bool freed = false;
	for (int i = 0; i < 5 && !freed; ++i)
	{
		game.tick();
		freed = sim.combat()->find(first)->cageBuilding == 0;
	}
	test::require(sim.combat()->find(second)->cageBuilding == home.building, "211: freed a prisoner that has not served its sentence");
	test::require(freed, "211: the served prisoner was not released");
	test::require(sim.bountyOn(first, legal) == 0, "110: the release did not erase the legal faction's bounty");
	std::cout << "211 release ok\n";

	// 4. kenshi-h2v.27.8, Task_Kidnapping (246; rwh.76): a start on a target already down fails and the target force-notices the arrester (state 1,
	// once at the start and again on the next tick, then the order ends with nobody carried). A lift that succeeds (the 203 arrests above) writes none.
	fighterOf(sim, guard)->npcClass = 0;
	const std::uint64_t lifted = spawnBody(sim, guard, g.x + 15.0, g.z);
	sim.addBounty(lifted, legal, 1000);
	test::require(arrest(game, guard, lifted, goap::kLiftPersonSnatchingInTown, 6000), "203: the control target was not put in a cage");
	test::require(perception::find(*fighterOf(sim, lifted), guard) == nullptr, "203: a successful lift force-noticed the arrester");
	const std::uint64_t downed = spawnBody(sim, guard, g.x + 15.0, g.z);
	test::require(sim.orderArrest(guard, downed, goap::kKidnapOrder), "246: the order was refused");
	game.tick(2);
	const state::SightRecord* told = perception::find(*fighterOf(sim, downed), guard);
	test::require(told != nullptr && told->state == 1, "246: the failed start did not force-notice the arrester");
	test::require(!sensor::detected(sim.sensorContext(), *fighterOf(sim, downed), *fighterOf(sim, guard), *told).friendly, "246: a stranger's entry is friendly");
	game.tick(2);
	test::require(sim.arrestOrderCount() == 0 && sim.combat()->find(downed)->cageBuilding == 0 && sim.combat()->find(guard)->carrying == 0,
		"246: the failed start did not end the order");
	const std::uint64_t mate = spawnBody(sim, guard, g.x + 15.0, g.z);
	fighterOf(sim, mate)->faction = home.owner.id;
	test::require(sim.orderArrest(guard, mate, goap::kKidnapOrder), "246: the order on an ally was refused");
	game.tick(2);
	fighterOf(sim, mate)->unconscious = false; // detected() hides a downed observer's records; the record itself is what the notice wrote
	const state::SightRecord* friendly = perception::find(*fighterOf(sim, mate), guard);
	test::require(friendly != nullptr && sensor::detected(sim.sensorContext(), *fighterOf(sim, mate), *fighterOf(sim, guard), *friendly).friendly,
		"246: an ally's entry is not friendly");
	std::cout << "246 kidnap start / tick force-notice ok\n";
}

// kenshi-k16.6.6 (crime-rows; dt41_E): the crime goals on the generic path. A law guard of the cage owner's faction stands in the Heft
// town with a Routine set by hand (it is in no resident squad, so the town scorer leaves it alone and stepCrimeGoals executes the goal).
void testCrimeGoals(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");
	const std::string legal = crime::legalFaction(sim.gameData().get(), home.owner.sid);
	const flecs::entity squad = game.place(kSlaverGuards, home.x + 40.0, home.z + 40.0);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(members.size() >= 2, "the guard squad was built");
	const std::uint64_t guard = idOf(members.back());
	fighterOf(sim, guard)->faction = home.owner.id;
	fighterOf(sim, guard)->npcClass = crime::kClassLaw;
	entityOf(sim, guard).set<state::Allegiance>(state::Allegiance{home.owner.sid});
	entityOf(sim, guard).remove<state::Routine>();
	const auto setGoal = [&](std::uint16_t goal, std::uint64_t target)
	{
		state::Routine routine;
		routine.goal = goal;
		routine.chainTarget = target;
		entityOf(sim, guard).set<state::Routine>(routine);
	};
	const auto goalOf = [&]() { return entityOf(sim, guard).get<state::Routine>()->goal; };

	// 1. The alarm manager (1409335e0 / 14092bfe0): a raise names an alarm in the town, a re-raise refreshes it in place WITHOUT resetting
	// its 8 s lifetime, and it expires 8 s after the first raise.
	// k16.6.329: EVENT 52 alarm_raised once per new alarm, a re-raise of a live one sends nothing.
	std::vector<Simulation::CrimeEvent> alarmEvents;
	sim.setCrimeObserver([&](const Simulation::CrimeEvent& e) { alarmEvents.push_back(e); });
	const alarm::Alarm* first = sim.raiseAlarm(crime::kTrespassing, fighterOf(sim, guard)->x + 50.0, fighterOf(sim, guard)->z, guard, 4242);
	test::require(alarmEvents.size() == 1 && alarmEvents[0].kind == Simulation::CrimeEvent::Kind::AlarmRaised && alarmEvents[0].who == guard
		&& alarmEvents[0].other == 4242 && alarmEvents[0].cause == crime::kTrespassing && alarmEvents[0].faction == home.owner.sid,
		"EVENT 52: a new alarm did not send one event with the raiser, criminal, crime and faction");
	test::require(first != nullptr && sim.alarmCount() == 1, "raise: no alarm in the guard's town");
	const std::uint64_t alarmId = first->id;
	game.tick(80); // 4 s
	const alarm::Alarm* again = sim.raiseAlarm(crime::kTrespassing, fighterOf(sim, guard)->x + 60.0, fighterOf(sim, guard)->z, guard, 4242);
	test::require(alarmEvents.size() == 1, "EVENT 52: a re-raise of a live alarm sent a second event");
	test::require(again != nullptr && again->id == alarmId && sim.alarmCount() == 1, "raise: a repeat raise made a second alarm");
	game.tick(70); // 7.5 s since the first raise
	test::require(sim.alarmCount() == 1, "expiry: the alarm ended before its 8 s");
	game.tick(15); // 8.25 s: the refresh did not extend it
	test::require(sim.alarmCount() == 0, "expiry: a repeat raise reset the lifetime (it must not)");
	// 1409335e0 keys the registry per town AND kind AND owner (rwh.203): another kind in the same town is its own alarm.
	sim.raiseAlarm(crime::kTrespassing, fighterOf(sim, guard)->x + 50.0, fighterOf(sim, guard)->z, guard);
	sim.raiseAlarm(crime::kBurglary, fighterOf(sim, guard)->x + 50.0, fighterOf(sim, guard)->z, guard);
	test::require(sim.alarmCount() == 2, "raise: two kinds in one town did not make two alarms");
	game.tick(180);
	test::require(sim.alarmCount() == 0, "expiry: the two kinds outlived their 8 s");
	std::cout << "alarm raise / refresh / expiry ok\n";

	// 2. 209: the investigator within 100 u of the alarm waits (this+0x7c) and the goal ends when the alarm is gone (here by the 8 s
	// expiry, which comes before the 10 s wait - the exe's order too); far from it the goal does not start the wait. The selection
	// (14092a810) takes the alarm within 375 u and halves the radius for a raiser with no usable building.
	const double gx = fighterOf(sim, guard)->x;
	const double gz = fighterOf(sim, guard)->z;
	test::require(sim.pickAlarm(guard, false) == nullptr, "selection: an alarm out of nothing");
	const alarm::Alarm* near = sim.raiseAlarm(crime::kTrespassing, gx + 60.0, gz, guard);
	test::require(near != nullptr && sim.pickAlarm(guard, false) == near, "selection: the alarm 60 u away is not the nearest");
	setGoal(routines::task::InvestigateAlarms, near->id);
	game.tick(100); // 5 s inside the 100 u radius: waiting, the alarm still stands
	test::require(sim.alarmCount() == 1 && goalOf() == routines::task::InvestigateAlarms, "209: the investigator gave up inside its wait");
	game.tick(80);
	test::require(sim.alarmCount() == 0 && goalOf() == 0, "209: the goal did not end with the alarm");
	// An alarm 300 u off (inside 375, outside 100): the investigator has not arrived, no wait runs; the goal waits for the scorer's walk.
	fighterOf(sim, guard)->clearRoute();
	const alarm::Alarm* far = sim.raiseAlarm(crime::kTrespassing, fighterOf(sim, guard)->x + 300.0, fighterOf(sim, guard)->z, guard);
	test::require(far != nullptr && sim.pickAlarm(guard, false) == far, "selection: an alarm 300 u away (inside 375) is not found");
	const std::uint64_t farId = far->id; // the raise may move the list: ids, not pointers
	const alarm::Alarm* wide = sim.raiseAlarm(crime::kTrespassing, fighterOf(sim, guard)->x + 400.0, fighterOf(sim, guard)->z, 0);
	test::require(wide != nullptr && wide->id != farId && sim.pickAlarm(guard, false) != nullptr && sim.pickAlarm(guard, false)->id == farId,
		"selection: an alarm 400 u away (outside 375) was taken");
	game.tick(200);
	test::require(sim.alarmCount() == 0, "expiry: alarms outlived their 8 s");
	// 210 (allies only): the raiser is an ally (the guard's own faction) or not (a body of no faction).
	const std::uint64_t stranger = spawnBody(sim, guard, fighterOf(sim, guard)->x + 20.0, fighterOf(sim, guard)->z);
	fighterOf(sim, stranger)->unconscious = false;
	fighterOf(sim, stranger)->faction = 4242;
	sim.raiseAlarm(crime::kAssault, fighterOf(sim, guard)->x + 25.0, fighterOf(sim, guard)->z, stranger);
	test::require(sim.pickAlarm(guard, false) != nullptr && sim.pickAlarm(guard, true) == nullptr, "210: an alarm of a stranger passed the allies-only filter");
	sim.raiseAlarm(crime::kBurglary, fighterOf(sim, guard)->x + 30.0, fighterOf(sim, guard)->z, guard);
	const std::size_t held = sim.alarmCount();
	sim.raiseAlarm(crime::kBurglary, fighterOf(sim, guard)->x + 30.0, fighterOf(sim, guard)->z, stranger);
	test::require(sim.alarmCount() == held + 1, "raise: the same kind by another owner (faction) did not make its own alarm");
	test::require(sim.pickAlarm(guard, true) != nullptr && sim.pickAlarm(guard, true)->raiser == guard, "210: an ally's alarm was refused");
	game.tick(200);
	// The 210 tick's ally notify (1405cb0a0): standing at the alarm the responder force-notices an ally within 100 u (a squadmate of its
	// faction, state 1, friendly in the arrow's entry), not a stranger, and a 209 responder notices no one (kenshi-h2v.27.5).
	const std::uint64_t mate = idOf(members.front());
	const state::Fighter mateBefore = *fighterOf(sim, mate);
	const state::Fighter strangerBefore = *fighterOf(sim, stranger);
	fighterOf(sim, mate)->faction = home.owner.id;
	fighterOf(sim, mate)->x = fighterOf(sim, guard)->x + 10.0;
	fighterOf(sim, mate)->z = fighterOf(sim, guard)->z;
	fighterOf(sim, stranger)->x = fighterOf(sim, guard)->x + 12.0;
	fighterOf(sim, stranger)->z = fighterOf(sim, guard)->z;
	fighterOf(sim, guard)->sight.clear();
	const alarm::Alarm* here = sim.raiseAlarm(crime::kBurglary, fighterOf(sim, guard)->x + 5.0, fighterOf(sim, guard)->z, guard);
	test::require(here != nullptr, "210 notify: no alarm raised");
	setGoal(routines::task::InvestigateAlarms, here->id);
	game.tick(2);
	test::require(perception::find(*fighterOf(sim, guard), mate) == nullptr, "209 notify: a plain investigator noticed an ally");
	setGoal(routines::task::InvestigateAlarmsAlliesOnly, here->id);
	game.tick(2);
	const state::SightRecord* told = perception::find(*fighterOf(sim, guard), mate);
	test::require(told != nullptr && told->state == 1, "210 notify: the ally was not force-noticed");
	test::require(sensor::detected(sim.sensorContext(), *fighterOf(sim, guard), *fighterOf(sim, mate), *told).friendly, "210 notify: the ally's entry is not friendly");
	test::require(perception::find(*fighterOf(sim, guard), stranger) == nullptr, "210 notify: a stranger was noticed");
	*fighterOf(sim, mate) = mateBefore; // the 204 case below runs on a clean board
	*fighterOf(sim, stranger) = strangerBefore;
	fighterOf(sim, guard)->sight.clear();
	game.tick(200);
	std::cout << "209 / 210 selection and wait, 210 ally notify ok\n";

	// 3. 212 LOOT_STOLEN_GOODS: a caged prisoner holds a stolen stack (the victim faction on the stack) and a clean one; the finder names
	// the prisoner anywhere in the town, the confiscation moves the stolen stack into the looter's pack and leaves the clean one.
	const std::uint64_t prisoner = spawnBody(sim, guard, fighterOf(sim, guard)->x + 15.0, fighterOf(sim, guard)->z);
	sim.addBounty(prisoner, legal, 1000);
	test::require(arrest(game, guard, prisoner, goap::kLiftPersonSnatchingInTown, 6000), "203: the loot target was not put in a cage");
	test::require(sim.lootedPrisoner212(guard) == 0, "212: a prisoner with nothing stolen was named");
	flecs::entity prisonerEntity = entityOf(sim, prisoner);
	state::Inventory pack;
	if (const state::Inventory* held = prisonerEntity.get<state::Inventory>())
		pack = *held;
	const std::string victimSid = otherHolder(sim, legal);
	std::vector<std::string> goods; // two plain ITEM records: the finder reads the stack's `stolen`, the grid needs a real record
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("ITEM")))
		if (goods.size() < 2 && record->intOr("stackable", 1) >= 3 && record->intOr("size x", 1) == 1 && record->intOr("size y", 1) == 1)
			goods.push_back(record->stringID);
	test::require(goods.size() == 2, "two small stackable ITEM records");
	pack.add(goods[0], 1);
	pack.items.back().stolen = victimSid;
	pack.add(goods[1], 3);
	prisonerEntity.set<state::Inventory>(pack);
	test::require(sim.lootedPrisoner212(guard) == prisoner, "212: the prisoner with a stolen stack was not named");
	fighterOf(sim, guard)->x = sim.combat()->find(prisoner)->x + 3.0;
	fighterOf(sim, guard)->z = sim.combat()->find(prisoner)->z;
	setGoal(routines::task::LootStolenGoods, prisoner);
	game.tick(5);
	const state::Inventory* after = prisonerEntity.get<state::Inventory>();
	const auto count = [&](const state::Inventory* inv, const char* sid)
	{
		std::uint32_t n = 0;
		for (const state::Inventory::Entry& entry : inv->items)
			n += entry.itemSid == sid ? entry.count : 0;
		return n;
	};
	test::require(count(after, goods[0].c_str()) == 0 && count(after, goods[1].c_str()) == 3, "212: the stolen stack stayed or the clean one went");
	test::require(count(entityOf(sim, guard).get<state::Inventory>(), goods[0].c_str()) == 1 && goalOf() == 0,
		"212: the looter did not take the stolen stack, or the goal did not end");
	test::require(sim.lootedPrisoner212(guard) == 0, "212: the finder still names a prisoner with nothing stolen left");
	std::cout << "212 confiscation ok\n";

	// 4. 211's finder is the whole town's cages (no 600 u cap): the served prisoner 900 u from the guard is freed.
	state::CrimeRecord served = *sim.crimeRecord(prisoner);
	served.sentenceStart = -1000.0;
	prisonerEntity.set<state::CrimeRecord>(served);
	fighterOf(sim, prisoner)->unconscious = false;
	fighterOf(sim, prisoner)->koTimer = 0.0;
	fighterOf(sim, guard)->x = sim.combat()->find(prisoner)->x + 900.0;
	fighterOf(sim, guard)->z = sim.combat()->find(prisoner)->z;
	const std::uint64_t freeable = sim.prisonerToFree211(guard);
	test::require(freeable == prisoner, "211: the finder did not name the served prisoner 900 u off");
	setGoal(routines::task::PoliceFreePrisoners, freeable);
	bool freed = false;
	for (int i = 0; i < 400 && !freed; ++i)
	{
		game.tick();
		freed = sim.combat()->find(prisoner)->cageBuilding == 0;
	}
	test::require(freed, "211: the goal did not free the prisoner");
	std::cout << "211 town-wide finder ok\n";

	// 5. 204 HUNT_BOUNTIES: the finder's admission is the group test - a sensed non-ally holding a bounty with my legal faction is the
	// target, one with none (and no enmity) is not; the goal orders the arrest chain.
	fighterOf(sim, guard)->x = home.x + 40.0;
	fighterOf(sim, guard)->z = home.z + 40.0;
	const std::uint64_t wanted = spawnBody(sim, guard, home.x + 60.0, home.z + 40.0);
	const std::uint64_t innocent = spawnBody(sim, guard, home.x + 60.0, home.z + 55.0);
	for (const std::uint64_t id : {wanted, innocent})
	{
		fighterOf(sim, id)->unconscious = false;
		fighterOf(sim, id)->koTimer = 0.0;
	}
	sim.addBounty(wanted, legal, 800);
	std::uint64_t target = 0;
	for (int i = 0; i < 200 && target == 0; ++i)
	{
		game.tick();
		target = sim.bountyTarget204(guard);
	}
	test::require(target == wanted, "204: the finder did not name the bounty holder (" + std::to_string(target) + ", wanted " + std::to_string(wanted) + ", stranger " + std::to_string(stranger) + ", guard " + std::to_string(guard) + ")");
	setGoal(routines::task::HuntBounties, target);
	game.tick(2);
	test::require(sim.arrestOrderCount() == 1, "204: the goal did not order the arrest chain");
	std::cout << "204 group test ok\n";

	// 6. The generic path end to end: a resident of a town whose package lists 209 / 210 (the law guards') adopts the goal through the
	// town scorer when an alarm stands near it, and the goal ends when the alarm is gone.
	std::vector<std::uint64_t> residents;
	for (const CombatArena::Member& member : sim.combat()->members())
		if (member.id != guard && !member.fighter.playerControlled && !member.fighter.dead && entityOf(sim, member.id).has<state::Routine>()
			&& entityOf(sim, member.id).target<state::MemberOf>().is_valid() && entityOf(sim, member.id).target<state::MemberOf>().has<state::SquadRoutine>())
			residents.push_back(member.id);
	test::require(!residents.empty(), "no resident with a Routine at Full detail in Heft");
	std::map<std::uint16_t, int> goals;
	for (int round = 0; round < 3; ++round)
	{
		for (const std::uint64_t id : residents)
			if (const state::Fighter* f = sim.combat()->find(id))
				sim.raiseAlarm(crime::kAssault + round, f->x + 40.0, f->z, id);
		for (int i = 0; i < 100; ++i)
		{
			game.tick();
			for (const std::uint64_t id : residents)
				if (const state::Routine* r = entityOf(sim, id).get<state::Routine>())
					if (r->goal == routines::task::InvestigateAlarms || r->goal == routines::task::InvestigateAlarmsAlliesOnly)
						++goals[r->goal];
		}
	}
	std::cout << "residents " << residents.size() << " adopted 209: " << goals[209] << " 210: " << goals[210] << " (member-ticks)\n";
	test::require(goals[209] + goals[210] > 0, "209 / 210: no resident adopted the investigation through the town scorer");
}

// kenshi-k16.6.15: a standing order whose route keeps being dropped (a failed re-plan, a path swap) still ends in the cage: the guard re-plans
// every second while the order stands (stepArrestOrders), never idling on a dropped route. Own Game: the steps above leave the world in a state
// that depends on walker timing.
void testRouteDrop(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");
	const std::string legal = crime::legalFaction(sim.gameData().get(), home.owner.sid);
	const flecs::entity squad = game.place(kSlaverGuards, home.x + 40.0, home.z + 40.0);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(members.size() >= 2, "the guard squad was built");
	const std::uint64_t guard = idOf(members.back());
	fighterOf(sim, guard)->faction = home.owner.id;
	entityOf(sim, guard).set<state::Allegiance>(state::Allegiance{home.owner.sid});
	entityOf(sim, guard).remove<state::Routine>();
	const std::uint64_t third = spawnBody(sim, guard, sim.combat()->find(guard)->x + 40.0, sim.combat()->find(guard)->z);
	sim.addBounty(third, legal, 1000);
	test::require(sim.orderArrest(guard, third, goap::kLiftPersonSnatchingInTown), "k16.6.15: orderArrest refused");
	bool caged3 = false;
	int dropped = 0;
	for (int i = 0; i < 6000 && !caged3; ++i)
	{
		game.tick();
		if (i % 3 == 0 && i < 300)
		{
			fighterOf(sim, guard)->clearRoute();
			++dropped;
		}
		caged3 = sim.combat()->find(third)->cageBuilding != 0;
		if (sim.arrestOrderCount() == 0)
			break;
	}
	test::require(caged3, "k16.6.15: the guard whose route was dropped " + std::to_string(dropped) + " times never put the target in a cage");
	// kenshi-k16.8.1 (a) + (b): imprisoning takes the prisoner's squad off its captors' hostile lists, and a body in a cage picks no combat target.
	{
		const std::uint64_t fourth = spawnBody(sim, guard, fighterOf(sim, guard)->x + 20.0, fighterOf(sim, guard)->z);
		sim.addBounty(fourth, legal, 1000);
		const std::uint64_t fourthSquad = sim.combat()->find(fourth)->squad;
		fighterOf(sim, guard)->hostileSquads.push_back(fourthSquad); // an arrest is an attack: the guard lists its target's squad
		std::sort(fighterOf(sim, guard)->hostileSquads.begin(), fighterOf(sim, guard)->hostileSquads.end());
		test::require(arrest(game, guard, fourth, goap::kLiftPersonSnatchingInTown, 6000), "k16.8.1: the fourth target was not put in a cage");
		const auto& listed = sim.combat()->find(guard)->hostileSquads;
		test::require(!std::binary_search(listed.begin(), listed.end(), fourthSquad), "k16.8.1: the imprisoned body's squad is still on its captor's hostile list");
		state::Fighter* prisoner = fighterOf(sim, fourth);
		prisoner->unconscious = false;
		prisoner->koTimer = 0.0;
		prisoner->hostileSquads.push_back(sim.combat()->find(guard)->squad);
		prisoner->target = guard;
		game.tick(40);
		test::require(sim.combat()->find(fourth)->target == 0, "k16.8.1: a caged prisoner picked a combat target");
		std::cout << "k16.8.1 captors forget the prisoner, a caged body picks no target ok\n";
	}
	std::cout << "k16.6.15 route dropped " << dropped << " times, still caged ok\n";
}

void testSmash(Game& game)
{
	Simulation& sim = *game.sim;
	std::uint64_t smasher = 0;
	for (const CombatArena::Member& member : sim.combat()->members())
		if (!member.fighter.dead && !member.fighter.playerControlled)
		{
			smasher = member.id;
			break;
		}
	test::require(smasher != 0, "a character exists");
	test::require(!sim.orderSmash(smasher, 987654321ull), "272: a building that is not a player building is refused");
	std::cout << "272 refusal ok\n";
}

}

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_arrest KSIM_NAVWORLD");
		{
			Game drop(argv[1]);
			testRouteDrop(drop);
		}
		{
			Game game(argv[1]);
			testArrest(game);
			testSmash(game);
		}
		{
			Game goals(argv[1]);
			testCrimeGoals(goals);
		}
		std::cout << "ksim_arrest: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_arrest: FAILED: " << e.what() << "\n";
		return 1;
	}
}
