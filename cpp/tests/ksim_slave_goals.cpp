// The slave-goal rows of the native selector and the trader's package slot (task ds/slave-goals):
//   - 172 SLAVE_GET_IN_MY_MASTERS_CAGE (dt24_172): a slave of the shop's faction, down to no errand, walks into a free cage of
//     its CAPTOR's faction and puts itself in (the plan is [67 Running, 107 USE_CAGE, 172]).
//   - 254 FINAL_KIDNAPPER_CAGE_JOB (dt26): a kidnapper squad's Squad row finds the nearest downed non-ally (132's finder), lifts
//     it (68 LIFT_PERSON under 132), then the same routine becomes the caging goal 112 at a cage of its town and 108 puts the
//     body in. The end-to-end home-town run is not live-verified (dt26 section 6): this case gives the squad the cage owner's
//     faction on purpose and marks the general case UNSURE.
//   - 39 WANDERING_TRADER (dt24_39): data-only - no resolver, no effect, no goal. The behaviour is the package system's trade
//     circuit; on arrival the three preconditions the row reads hold: no weapon equipped, a destination town, and standing in it.
//
// These cases run the live paths: the Heft world at Full detail with the members' own packages (172 / 254), and the real trade
// circuit over the loaded towns (39). No NPC is ordered.

#include "ksim/ai/ai.hpp"
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
#include "kworld/collision.hpp"
#include "kworld/zones.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

// Heft's grid cell (the baked world's region_11_4.knav) and its town centre (the cases run inside its 2000 u cage finder).
constexpr int kHeftCellX = 46;
constexpr int kHeftCellZ = 16;
constexpr double kHeftX = 68772.98;
constexpr double kHeftZ = -71496.61;
// The kidnapper template (Squad role carries both 254 and 112), placed as the town's slavers in the 254 case.
constexpr const char* kKidnappers = "96357-rebirth.mod";
// The Peeler Machine BUILDING (function 8 through limbs.mod's "skin peeler").
constexpr const char* kPeelerMachine = "96396-Newwworld.mod";
// The slave-carrying guard template (its Slave list carries 172): the UC storehouses' 'Storage Guard' package.
constexpr const char* kSlaverGuards = "56041-rebirth.mod";
// The wandering-trader caravan (the 39 case).
constexpr const char* kCaravan = "1161-gamedata.base";

std::ostream& say()
{
	static std::ostream nowhere(nullptr);
	return std::getenv("KSIM_SLAVE_GOALS_VERBOSE") != nullptr ? std::cout : nowhere;
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

const state::Routine* onGoal(const std::vector<flecs::entity>& members, std::uint16_t goal, std::uint64_t* who)
{
	for (flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		if (routine != nullptr && routine->goal == goal)
		{
			*who = idOf(member);
			return routine;
		}
	}
	return nullptr;
}

std::string planText(const state::Routine& routine)
{
	std::string out;
	for (std::size_t i = 0; i < routine.plan.size(); ++i)
		out += (i == 0 ? "" : " -> ") + std::to_string(routine.plan[i]);
	return out;
}

bool planHas(const state::Routine& routine, std::uint16_t task)
{
	return std::find(routine.plan.begin(), routine.plan.end(), task) != routine.plan.end();
}

// A development probe (KSIM_SLAVE_GOALS_PROBE=1): how many members each candidate template spawns at Heft. Not part of the suite.
void probeTemplates(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");
	const char* candidates[] = {"54289-rebirth.mod", "56041-rebirth.mod", "50930-rebirth.mod", "5239-gamedata.base",
		"55376-rebirth.mod", "5220-gamedata.base", "49398-rebirth.mod", "51411-rebirth.mod", "56040-rebirth.mod",
		"96357-rebirth.mod"};
	double x = home.x + 40.0;
	for (const char* candidate : candidates)
	{
		const flecs::entity squad = game.place(candidate, x, home.z + 40.0);
		const std::vector<flecs::entity> members = membersOf(sim, squad);
		const state::SquadInstance* instance = squad.get<state::SquadInstance>();
		const Mapping& mapping = sim.ai()->registry().squad(instance != nullptr ? instance->templateSid : "");
		std::cout << "probe " << candidate << " -> " << members.size() << " members, faction '"
				  << (instance != nullptr && instance->faction.id != 0
						  ? ecsOf(sim).entity(instance->faction.id).get<state::FactionState>()->templateSid : std::string())
				  << "', package '" << mapping.packageName << "'\n";
		x += 80.0;
	}
}

// ---- 172 SLAVE_GET_IN_MY_MASTERS_CAGE ----
void testSlaveInCage(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");
	say() << "cage home " << home.building << " at " << home.x << "," << home.z << " owner '" << home.owner.sid << "'\n";

	const flecs::entity squad = game.place(kSlaverGuards, home.x + 40.0, home.z + 40.0);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	say() << "placed " << kSlaverGuards << " squad " << (squad.is_valid() ? idOf(squad) : 0) << " with " << members.size()
		  << " members\n";
	test::require(members.size() >= 2, "the slaver guard squad was built (" + std::to_string(members.size()) + " members)");

	// A member given the Slave role and the shop owner as its captor: the slave-state path the exe runs for the slavers' stock. It
	// must NOT be the squad's follow leader (the selector skips that one), so the last member in Id order is taken.
	flecs::entity slave = members.back();
	const std::uint64_t slaveId = idOf(slave);
	{
		state::MemberRole role = *slave.get<state::MemberRole>();
		role.list = static_cast<std::uint8_t>(routines::Role::Slave);
		slave.set<state::MemberRole>(role);
	}
	{
		state::Fighter* f = fighterOf(sim, slaveId);
		test::require(f != nullptr, "the slave member has a Fighter");
		f->slaveState = 1;
		f->captorSid = home.owner.sid;
		f->captorFaction = home.owner.id;
		f->clearRoute();
	}

	std::uint64_t who = 0;
	const state::Routine* routine = nullptr;
	for (int i = 0; i < 200 && routine == nullptr; ++i)
	{
		game.tick();
		routine = onGoal(membersOf(sim, squad), routines::task::SlaveGetInCage, &who);
	}
	if (routine == nullptr)
	{
		for (flecs::entity member : membersOf(sim, squad))
		{
			const state::MemberRole* role = member.get<state::MemberRole>();
			const state::Routine* r = member.get<state::Routine>();
			say() << "  member " << idOf(member) << " role " << (role != nullptr ? int(role->list) : -1) << " routine "
				  << (r != nullptr ? int(r->goal) : -1) << "\n";
		}
		const std::size_t from = game.taskLog.size() > 20 ? game.taskLog.size() - 20 : 0;
		for (std::size_t i = from; i < game.taskLog.size(); ++i)
			say() << "  log " << game.taskLog[i] << "\n";
	}
	test::require(routine != nullptr, "the slave never scored 172 SLAVE_GET_IN_MY_MASTERS_CAGE");
	say() << "172 pick member " << who << " plan [" << planText(*routine) << "] cage " << routine->target.object << ":"
		  << static_cast<unsigned>(routine->target.slot) << "\n";
	test::require(routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Cage), "172's routine holds a cage site");
	test::require(planHas(*routine, 107), "172's plan runs 107 USE_CAGE (its I_AM_IMPRISONED writer)");
	test::require(!routine->plan.empty() && routine->plan.back() == routines::task::SlaveGetInCage,
		"172's plan ends at the goal row (107 alone when the slave already stands in the cage's reach)");
	test::require(routine->target.object == home.building,
		"172 picked a cage of the captor's building (" + std::to_string(routine->target.object) + ")");

	bool caged = false;
	for (int i = 0; i < 4000 && !caged; ++i)
	{
		game.tick();
		const state::Fighter* f = sim.combat()->find(slaveId);
		caged = f != nullptr && f->cageBuilding != 0;
	}
	const state::Fighter* f = sim.combat()->find(slaveId);
	test::require(caged, "the slave never put itself in the cage");
	test::require(f->cageBuilding == home.building, "the slave is in the home's cage");
	say() << "172 slave " << slaveId << " in cage " << f->cageBuilding << ":" << static_cast<unsigned>(f->cageSlot) << "\n";
	std::cout << "172 slave in cage ok\n";
}

// ---- 254 FINAL_KIDNAPPER_CAGE_JOB ----
void testKidnapper(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");

	const flecs::entity squad = game.place(kKidnappers, home.x - 60.0, home.z - 60.0);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(members.size() >= 2, "the kidnapper squad was built");

	// The template's own faction owns no cages; the case gives the platoon the cage owner's (the end-to-end home-town run is UNSURE,
	// dt26 section 6). The applied package stays the template's (it is the first of its priority bucket).
	{
		flecs::entity squadEntity = squad;
		state::SquadInstance instance = *squadEntity.get<state::SquadInstance>();
		instance.faction.id = home.owner.entity;
		squadEntity.set<state::SquadInstance>(instance);
		for (flecs::entity member : members)
		{
			state::Fighter* f = fighterOf(sim, idOf(member));
			if (f != nullptr)
				f->faction = home.owner.id;
			if (state::Allegiance* allegiance = member.get_mut<state::Allegiance>())
				allegiance->factionSid = home.owner.sid;
		}
	}

	// The victim: a KO'd, factionless body 25 u from the squad (132's finder: downed, not an ally, not dead, not an animal).
	const double vx = home.x - 60.0 + 25.0;
	const double vz = home.z - 60.0;
	std::uint64_t victim = 0;
	{
		state::Fighter template_ = *sim.combat()->find(idOf(members.front()));
		template_.playerControlled = false;
		template_.dead = false;
		template_.down = false;
		template_.unconscious = true;
		template_.koTimer = 1.0e6;
		template_.asleep = false;
		template_.carrying = 0;
		template_.carriedBy = 0;
		template_.cageBuilding = 0;
		template_.cageSlot = 0;
		template_.slaveState = 0;
		template_.captorSid.clear();
		template_.captorFaction = 0;
		template_.target = 0;
		template_.orderTarget = 0;
		template_.faction = 0;
		template_.team = 0;
		template_.animal = false;
		template_.goal = state::IndividualGoal{};
		template_.routeSeq = 0;
		template_.clearRoute();
		template_.x = vx;
		template_.z = vz;
		template_.y = sim.groundHeight(vx, vz).value_or(template_.y);
		const flecs::entity body = sim.spawnFighter(std::move(template_));
		body.set<state::Allegiance>(state::Allegiance{"test-victim"});
		victim = idOf(body);
	}

	std::uint64_t who = 0;
	const state::Routine* routine = nullptr;
	for (int i = 0; i < 400 && routine == nullptr; ++i)
	{
		game.tick();
		routine = onGoal(membersOf(sim, squad), routines::task::FinalKidnapperCageJob, &who);
	}
	test::require(routine != nullptr, "no kidnapper scored 254 FINAL_KIDNAPPER_CAGE_JOB");
	say() << "254 pick member " << who << " plan [" << planText(*routine) << "] victim " << routine->chainTarget << "\n";
	test::require(routine->chainTarget == victim, "254's resolver picked the downed non-ally");
	test::require(planHas(*routine, goap::kLiftPerson), "254's plan lifts (68 LIFT_PERSON under 132 FIND_AND_KIDNAP)");
	test::require(planHas(*routine, routines::task::FindAndKidnap), "254's plan carries the 132 row");

	bool caged = false;
	bool handedOver = false;
	for (int i = 0; i < 8000 && !caged; ++i)
	{
		game.tick();
		if (!handedOver)
			for (flecs::entity member : membersOf(sim, squad))
				if (const state::Routine* r = member.get<state::Routine>();
					r != nullptr && r->goal == routines::task::FindCageAndPutIn && r->chainTarget == victim)
					handedOver = true;
		const state::Fighter* f = sim.combat()->find(victim);
		caged = f != nullptr && f->cageBuilding != 0;
		if (i % 500 == 0)
		{
			const state::Fighter* kidnapper = sim.combat()->find(who);
			say() << "  t" << i << " kidnapper " << who << " goal "
				  << (kidnapper != nullptr ? int(kidnapper->goal.kind) : -1) << " carrying "
				  << (kidnapper != nullptr ? kidnapper->carrying : 0) << " at " << (kidnapper != nullptr ? kidnapper->x : 0) << ","
				  << (kidnapper != nullptr ? kidnapper->z : 0) << " victim carriedBy " << (f != nullptr ? f->carriedBy : 0)
				  << " at " << (f != nullptr ? f->x : 0) << "," << (f != nullptr ? f->z : 0) << "\n";
		}
	}
	if (!handedOver)
		for (std::size_t i = game.taskLog.size() > 30 ? game.taskLog.size() - 30 : 0; i < game.taskLog.size(); ++i)
			say() << "  log " << game.taskLog[i] << "\n";
	const state::Fighter* f = sim.combat()->find(victim);
	test::require(handedOver, "the lifted victim was never handed to the caging goal 112");
	test::require(caged, "the kidnapper never put the victim in a cage");
	test::require(f->cageBuilding == home.building, "the victim is in a cage of the squad's home");
	say() << "254 victim " << victim << " in cage " << f->cageBuilding << ":" << static_cast<unsigned>(f->cageSlot) << "\n";
	std::cout << "254 kidnapper ok\n";
}

// ---- 254 into a placed function-8 cage (kenshi-cym.28, rwh.97 section 6-7) ----
// The Peeler Machine is a placed furniture building of its own (no doors, no collision): World::addFurnitureCage makes its cage site. A
// kidnapper squad of the town's faction lifts a downed non-ally (the Skin Bandits' chain after the club-down), carries it to that site
// and puts it in; the machine then severs limbs and leaves the head and torso alone. Before the fix no Cage entry existed ("no cage for
// victim") and the victim was never put anywhere.
void testKidnapperPeeler(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "a Cage Warden home with cages is in the world");
	const double sx = home.x - 60.0;
	const double sz = home.z - 60.0;

	kworld::PlacedInstance machine;
	machine.instanceID = "test-peeler-machine";
	machine.buildingID = kPeelerMachine;
	machine.x = sx - 4.0; // clearly nearer than any shop cage (a 12 u gap was a 0.4 u tie against the Heft shop's nearest cage: k16.7.36)
	machine.z = sz;
	machine.y = sim.groundHeight(machine.x, machine.z).value_or(0.0);
	machine.ownerFaction = home.owner.sid;
	test::require(sim.placeFurnitureCage(machine), "the Peeler Machine placement became a cage site");
	kworld::PlacedInstance chair = machine; // not a cage: refused, no site
	chair.buildingID = kKidnappers;
	test::require(!sim.placeFurnitureCage(chair), "a placement that is no cage building makes no site");
	const std::uint64_t peeler = kworld::doorKey(kworld::placementId(machine));
	test::require(sim.world().buildingSite(peeler) != nullptr && sim.world().buildingSite(peeler)->furnitureCage, "the site is in the world");

	const flecs::entity squad = game.place(kKidnappers, sx, sz);
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(members.size() >= 2, "the kidnapper squad was built");
	{
		flecs::entity squadEntity = squad;
		state::SquadInstance instance = *squadEntity.get<state::SquadInstance>();
		instance.faction.id = home.owner.entity;
		instance.homeBuilding = 0; // the shop's x100 for a home cage would beat the machine 12 u away
		squadEntity.set<state::SquadInstance>(instance);
		for (flecs::entity member : members)
		{
			if (state::Fighter* f = fighterOf(sim, idOf(member)))
			{
				f->faction = home.owner.id;
				f->homeBuilding = 0;
			}
			if (state::Allegiance* allegiance = member.get_mut<state::Allegiance>())
				allegiance->factionSid = home.owner.sid;
		}
	}
	std::uint64_t victim = 0;
	{
		state::Fighter body = *sim.combat()->find(idOf(members.front()));
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
		body.clearRoute();
		body.x = sx + 25.0;
		body.z = sz;
		body.y = sim.groundHeight(body.x, body.z).value_or(body.y);
		const flecs::entity entity = sim.spawnFighter(std::move(body));
		entity.set<state::Allegiance>(state::Allegiance{"test-victim"});
		victim = idOf(entity);
	}
	bool caged = false;
	for (int i = 0; i < 8000 && !caged; ++i)
	{
		game.tick();
		const state::Fighter* f = sim.combat()->find(victim);
		caged = f != nullptr && f->cageBuilding != 0;
	}
	for (std::size_t i = game.taskLog.size() < 25 ? 0 : game.taskLog.size() - 25; i < game.taskLog.size(); ++i)
		say() << "  log " << game.taskLog[i] << "\n";
	test::require(caged, "the kidnapper never put the victim in a cage");
	test::require(sim.combat()->find(victim)->cageBuilding == peeler, "the victim is in the Peeler Machine (the function-8 cage), not a shop cage");
	// The machine: it takes the armour first (one piece a tick-second), then wears the limbs; a limb at its line is severed and the head and
	// torso are never touched (rwh.97 section 7). The wear itself is ksim_peeler's; here the right leg is put at its line to keep the case short.
	const auto limbSevered = [&]
	{
		for (const state::FighterPart& p : sim.combat()->find(victim)->parts)
			if (p.name == "Right Leg")
				return p.severed();
		return false;
	};
	for (state::FighterPart& p : fighterOf(sim, victim)->parts)
		if (p.name == "Right Leg")
			p.health = -p.maxHealth;
	for (int i = 0; i < 3000 && !limbSevered(); ++i)
		game.tick();
	test::require(limbSevered(), "the Peeler Machine never severed the limb at its line");
	for (const state::FighterPart& p : sim.combat()->find(victim)->parts)
		test::require(p.limb() || p.health >= p.maxHealth, "the machine hurt the head or torso");
	std::cout << "254 peeler ok\n";
}

// ---- 39 WANDERING_TRADER (data-only: the trade circuit runs it) ----

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

bool hasWeapon(const state::Inventory* pack)
{
	if (pack == nullptr)
		return false;
	return std::any_of(pack->items.begin(), pack->items.end(),
		[](const state::Inventory::Entry& entry) { return entry.slot == state::Inventory::kWeapon; });
}

void testWanderingTrader(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	config.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(config));

	flecs::entity caravan;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id&, const state::SquadInstance& instance)
	{
		if (!caravan.is_valid() && instance.templateSid == kCaravan)
			caravan = entity;
	});
	test::require(caravan.is_valid(), "the caravan squad is in the loaded world (" + std::string(kCaravan) + ")");
	const state::SquadInstance* instance = caravan.get<state::SquadInstance>();
	const Mapping& mapping = sim.ai()->registry().squad(instance->templateSid);
	test::require(mapping.behaviour == Behaviour::TradeCircuit,
		"39's package runs the trade circuit (" + mapping.packageName + " -> " + std::string(behaviourName(mapping.behaviour)) + ")");

	while (sim.clockTick() % 20 != 0)
		sim.run(1);
	sim.run(20);
	test::require(caravan.get<state::TraderTrip>() != nullptr, "the caravan runs the circuit state");

	// Up to 4 days for the first arrival.
	bool staying = false;
	for (int step = 0; step < 52364 * 4 / 20 && !staying; ++step)
	{
		sim.run(20);
		const state::TraderTrip* trip = caravan.get<state::TraderTrip>();
		staying = trip != nullptr && trip->staying;
	}
	test::require(staying, "the caravan never arrived at a town");
	const state::TraderTrip trip = *caravan.get<state::TraderTrip>();
	test::require(trip.town.id != 0, "39's hasDestinationTown: the circuit picked a town");
	const state::TownState* town = ecsOf(sim).entity(trip.town.id).get<state::TownState>();
	test::require(town != nullptr, "the destination town is a TownState");
	const state::Position at = *caravan.get<state::Position>();
	test::require(std::hypot(at.x - town->x, at.z - town->z) <= UtilityGroupLayer::kHomeRadius + 1.0,
		"39's atDestinationTown: the caravan stands in the destination town");

	const std::vector<flecs::entity> members = membersOf(sim, caravan);
	test::require(!members.empty(), "the caravan has members");
	const flecs::entity leader = members.front();
	const state::Inventory* pack = leader.get<state::Inventory>();
	test::require(!hasWeapon(pack), "39's hasWeaponEquipped=0: the leader carries no equipped weapon");
	say() << "39 caravan arrived at " << trip.town.id << " (arrivals " << trip.arrivals << ")\n";
	std::cout << "39 wandering trader ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_slave_goals KSIM_NAVWORLD");
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		if (std::getenv("KSIM_SLAVE_GOALS_PROBE") != nullptr)
		{
			Game game(argv[1]);
			probeTemplates(game);
			return 0;
		}
		{
			Game game(argv[1]);
			testSlaveInCage(game);
		}
		{
			Game game(argv[1]);
			testKidnapper(game);
		}
		{
			Game game(argv[1]);
			testKidnapperPeeler(game);
		}
		testWanderingTrader(order);
		std::cout << "ksim_slave_goals: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_slave_goals: FAILED: " << e.what() << "\n";
		return 1;
	}
}
