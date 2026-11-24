// kenshi-k16.6.125 (rwh.104): a guard's arrest order and a slaver's capture chain on ONE downed body, on the Heft world. The original has no
// cross-claim, no "being arrested" flag and no id tie-break: each finder reads only the body's own state (carried / slave state 1 / in an
// object), a cuff does not cancel the guard's arrest order (DA 29 -> 240 -> 112 -> 108 cage a cuffed slave), DA 30 lifts a carried body off its
// carrier, and 240 (Task_LiftSomething) refuses a carried body. The slaver is a Heft Cage Warden (the town's own 167 / 181 / 166 chain); a fresh world per
// case and per Ids.next bump 0..5 (the body's id moves through the guard's and the warden's), the same outcome each time. Cases:
//   a. the slaver cuffs first: the guard's arrest still lifts and cages the cuffed body; nobody else carries it
//   b. the guard lifts first: the wardens' chain ends, nobody cuffs the body, the guard cages it
//   c. the slaver carries first (a lift ordered after the cuff): the guard's 240 fails at once, the slaver's carry finishes in the cage
//   d. the same, but DA 30's 213 lift: the guard takes the carried body off the warden and its order stands
//   e. DA 30's snatch loser (the body is carried again): the order ends at once (rwh.124 section 5), no arrest wait
//   f. a 240 victim that stood up before the lift: the lift is refused at assignment, the order stays queued with no expiry, and once the victim is
//      down again (the guard's own combat re-KOs it: here the case does) it is lifted and caged (k16.6.187, rwh.124 / rwh.125)
//   g. a conscious victim that lies down (ragdoll, posture >= 2) is lifted and caged at once: isTargetDown, not unconscious (rwh.124 4.1)
// Each tick no body has two carriers. Needs the world bake (argv[1], exit 77 without it).

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
#include "ksim/world/interact.hpp"
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
#include <functional>
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
	flecs::entity squad;
	std::vector<std::uint64_t> wardens; // the Cage Warden squad's members: the town's own slavers (their 167 / 112 run on the town scorer)
};

bool findCageHome(Simulation& sim, CageHome& out)
{
	bool found = false;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance)
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
		out.squad = squad;
		found = out.owner.id != 0;
	});
	out.wardens.clear();
	if (found)
		for (const flecs::entity member : membersOf(sim, out.squad))
			out.wardens.push_back(idOf(member));
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
std::string where; // the case running, for the watch loop's messages

// The staging shared by the cases. The slaver is one of the Heft Cage Warden squad (the town's own slavers: its 167 / 181 / 166 / 168 / 112 run on the
// town scorer, nothing set by hand); the guard is a fresh solo guard of the cage owner's side that comes with its order.
struct Stage
{
	CageHome home;
	std::uint64_t guard = 0;
	std::uint64_t slaver = 0; // the warden that holds the capture chain on the case's body (0 until one does)
};

std::uint64_t soloGuard(Game& game, const Stage& stage, double x, double z)
{
	Simulation& sim = *game.sim;
	const std::vector<flecs::entity> members = membersOf(sim, game.place(kSlaverGuards, x, z));
	test::require(!members.empty(), "the guard squad was built");
	for (std::size_t i = 0; i + 1 < members.size(); ++i)
		fighterOf(sim, idOf(members[i]))->dead = true;
	const std::uint64_t id = idOf(members.back());
	fighterOf(sim, id)->faction = stage.home.owner.id;
	entityOf(sim, id).set<state::Allegiance>(state::Allegiance{stage.home.owner.sid});
	entityOf(sim, id).remove<state::Routine>();
	return id;
}

void stand(Simulation& sim, std::uint64_t id, double x, double z)
{
	state::Fighter& f = *fighterOf(sim, id);
	f.x = x;
	f.z = z;
	f.y = sim.groundHeight(x, z).value_or(f.y);
	f.clearRoute();
	f.forwardSpeed = 0.0;
	f.lateralSpeed = 0.0;
	f.carrying = 0;
}

// One tick with the guard and the wardens kept on their feet: the town's police fight a stranger slaver at the cage home (hits are the world's, not the
// case's), and a KO'd catcher ends the chain (a different test).
void step(Game& game, const Stage& stage)
{
	std::vector<std::uint64_t> people = stage.home.wardens;
	people.push_back(stage.guard);
	for (const std::uint64_t id : people)
	{
		state::Fighter& f = *fighterOf(*game.sim, id);
		for (state::FighterPart& part : f.parts)
		{
			part.health = part.maxHealth;
			part.stun = 0.0;
			part.collapsed = false;
			part.pool = 0.0;
		}
		f.blood = game.sim->combat()->rules().maxBlood(f);
		f.wounds.clear();
		f.bleeding = 0.0;
		f.koTimer = 0.0;
		f.koLatch = false;
		f.unconscious = false;
		f.down = false;
	}
	game.tick();
}

bool isWarden(const Stage& stage, std::uint64_t id)
{
	return std::find(stage.home.wardens.begin(), stage.home.wardens.end(), id) != stage.home.wardens.end();
}

struct Outcome
{
	bool caged = false;
	bool guardCarried = false;
	bool wardenCarried = false;
	std::uint8_t slaveState = 0; // at the put-in (the cage wardens process a prisoner later: 182 cuffs it)
	std::string text() const
	{
		return std::string(caged ? "caged" : "free") + (guardCarried ? " guard-carried" : "") + (wardenCarried ? " warden-carried" : "")
			+ " slave" + std::to_string(slaveState);
	}
};

// The loop every case runs: tick until the body is in a cage (then `after` more ticks, to show nobody takes it out) or `maxTicks`; one carrier per body
// at every tick, and a carrier's hand and the body's carriedBy agree.
Outcome watch(Game& game, const Stage& stage, std::uint64_t body, int maxTicks, int after)
{
	Simulation& sim = *game.sim;
	Outcome out;
	int cagedFor = -1;
	for (int i = 0; i < maxTicks && cagedFor < after; ++i)
	{
		step(game, stage);
		int carriers = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			if (m.fighter.carrying == body)
			{
				++carriers;
				out.guardCarried = out.guardCarried || m.id == stage.guard;
				out.wardenCarried = out.wardenCarried || isWarden(stage, m.id);
			}
		const state::Fighter& b = *sim.combat()->find(body);
		test::require(carriers <= 1, where + ": two members carry one body at tick " + std::to_string(i));
		test::require(carriers == 0 || b.carriedBy != 0, where + ": the carrier's hand and the body's carriedBy disagree at tick " + std::to_string(i));
		if (b.cageBuilding != 0)
		{
			if (!out.caged)
				out.slaveState = b.slaveState;
			out.caged = true;
			++cagedFor;
		}
	}
	if (!out.caged)
		out.slaveState = sim.combat()->find(body)->slaveState;
	return out;
}

// The warden whose routine holds the capture chain on `body`, or 0.
std::uint64_t catcherOf(Simulation& sim, const Stage& stage, std::uint64_t body)
{
	for (const std::uint64_t id : stage.home.wardens)
	{
		const state::Routine* r = entityOf(sim, id).get<state::Routine>();
		if (r != nullptr && r->goal == routines::task::CaptureSlaves && r->chainTarget == body)
			return id;
	}
	return 0;
}

// A fresh KO'd stranger `gap` u from the first warden, its id moved by `bump`; a fresh guard 150 u off (outside the 60 u catch sight, so it does not take
// the body by itself) until the case brings it. The last case's people are gone first.
std::uint64_t setUp(Game& game, Stage& stage, int bump, double gap)
{
	Simulation& sim = *game.sim;
	if (stage.guard != 0)
		fighterOf(sim, stage.guard)->dead = true;
	const state::Fighter& warden = *sim.combat()->find(stage.home.wardens.front());
	const double x = warden.x + gap;
	const double z = warden.z;
	stage.guard = soloGuard(game, stage, x + 150.0, z);
	ecsOf(sim).get_mut<state::Ids>()->next += static_cast<std::uint64_t>(bump);
	const std::uint64_t body = spawnBody(sim, stage.guard, x, z);
	stand(sim, stage.guard, x + 150.0, z);
	entityOf(sim, stage.guard).remove<state::Routine>();
	stage.slaver = 0;
	return body;
}

// A finished case's body leaves the world (and its cage): the next case needs a free cage.
void clearBody(Simulation& sim, std::uint64_t body)
{
	state::Fighter& b = *fighterOf(sim, body);
	b.cageBuilding = 0;
	b.cageSlot = 0;
	b.carriedBy = 0;
	b.dead = true;
	for (const CombatArena::Member& m : sim.combat()->members())
		if (m.fighter.carrying == body)
			fighterOf(sim, m.id)->carrying = 0;
}

constexpr int kCaseTicks = 20 * 240;

// Tick until a warden's chain is on the body (the town scorer picks it from its sensor list), then until `done` holds.
void waitFor(Game& game, Stage& stage, std::uint64_t body, const std::function<bool()>& done, const std::string& what)
{
	for (int i = 0; i < kCaseTicks && !done(); ++i)
	{
		step(game, stage);
		if (stage.slaver == 0)
			stage.slaver = catcherOf(*game.sim, stage, body);
	}
	test::require(done(), where + ": " + what);
}

void run(Game& game, Stage& stage, char which, int bump, std::map<std::string, std::string>& seen)
{
	Simulation& sim = *game.sim;
	const std::string tag = " (Ids bump " + std::to_string(bump) + ")";
	const auto record = [&](const char* key, const Outcome& out)
	{
		const std::string text = out.text();
		const auto [it, fresh] = seen.emplace(key, text);
		test::require(fresh || it->second == text, std::string(key) + ": the outcome moved with the id: '" + it->second + "' then '" + text + "'" + tag);
	};
	// The warden that cuffs the body lifts it (its 168 / 112 chain: the town's wardens process a cuffed slave where it lies, so the lift is ordered the way a
	// carry to a cage is): the case is about the body being in the slaver's arms when the guard's order starts.
	const auto slaverCarries = [&](std::uint64_t body)
	{
		waitFor(game, stage, body, [&] { return sim.combat()->find(body)->slaveState == 1; }, "no warden cuffed the body" + tag);
		test::require(sim.orderArrest(stage.slaver, body, goap::kLiftPersonSnatchingInTown), where + ": the warden's own lift order was refused" + tag);
		waitFor(game, stage, body, [&] { return sim.combat()->find(stage.slaver)->carrying == body; }, "the warden never lifted the cuffed body" + tag);
	};

	// a. The slaver cuffs first. The guard comes once the shackles are on and its arrest order (DA 29: 240 -> 112 -> 108) lifts and cages the slave.
	if (which == 'a')
	{
		where = "a";
		const std::uint64_t body = setUp(game, stage, bump, 10.0);
		waitFor(game, stage, body, [&] { return sim.combat()->find(body)->slaveState == 1; }, "no warden cuffed the body" + tag);
		test::require(sim.combat()->find(stage.slaver)->carrying != body, "a: the warden already carries (the cuff comes first)" + tag);
		const state::Fighter& b = *sim.combat()->find(body);
		stand(sim, stage.guard, b.x + 6.0, b.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatchingInTown), "a: the arrest order was refused" + tag);
		const Outcome out = watch(game, stage, body, kCaseTicks, 200);
		test::require(out.caged && out.guardCarried, "a: the guard's arrest did not lift and cage the cuffed body: " + out.text() + tag);
		test::require(out.slaveState == 1, "a: the cage put-in changed the slave state: " + out.text() + tag);
		record("a", out);
		clearBody(sim, body);
	}

	// b. The guard lifts first: the wardens (30 u off) come to a carried body and their chain ends; nobody cuffs it before the cage.
	if (which == 'b')
	{
		where = "b";
		const std::uint64_t body = setUp(game, stage, bump, 30.0);
		const state::Fighter& b = *sim.combat()->find(body);
		stand(sim, stage.guard, b.x + 3.0, b.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatchingInTown), "b: the arrest order was refused" + tag);
		const Outcome out = watch(game, stage, body, kCaseTicks, 200);
		test::require(out.caged && out.guardCarried && !out.wardenCarried, "b: the guard did not lift and cage first: " + out.text() + tag);
		test::require(out.slaveState == 0, "b: a warden cuffed a body the guard had lifted: " + out.text() + tag);
		record("b", out);
		clearBody(sim, body);
	}

	// c. The slaver carries first: the guard's 240 (a lift for DA 29) refuses a carried body, so the order ends at once (not after a wait), and the guard
	// never takes the body.
	if (which == 'c')
	{
		where = "c";
		const std::uint64_t body = setUp(game, stage, bump, 10.0);
		slaverCarries(body);
		const state::Fighter& carrier = *sim.combat()->find(stage.slaver);
		stand(sim, stage.guard, carrier.x + 4.0, carrier.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatchingInTown), "c: the arrest order was refused" + tag);
		step(game, stage);
		step(game, stage);
		test::require(!sim.arrestOrdered(stage.guard), "c: the guard's 240 on a carried body did not fail at once (its order is still there two ticks on)" + tag);
		test::require(sim.combat()->find(stage.slaver)->carrying == body, "c: the failed arrest took the body from the slaver" + tag);
		const Outcome out = watch(game, stage, body, kCaseTicks, 20);
		test::require(out.caged && !out.guardCarried && out.wardenCarried, "c: the slaver's carry did not finish alone: " + out.text() + tag);
		record("c", out);
		clearBody(sim, body);
	}

	// d. DA 30 (213 LIFT_PERSON_SNATCHING, then 112): the guard takes the carried body off the warden and its order stands.
	if (which == 'd')
	{
		where = "d";
		const std::uint64_t body = setUp(game, stage, bump, 10.0);
		slaverCarries(body);
		const state::Fighter& carrier = *sim.combat()->find(stage.slaver);
		stand(sim, stage.guard, carrier.x + 1.5, carrier.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatching), "d: the arrest order was refused" + tag);
		for (int i = 0; i < 3 && sim.combat()->find(stage.slaver)->carrying == body; ++i)
			step(game, stage);
		test::require(sim.combat()->find(stage.slaver)->carrying == 0 && sim.combat()->find(body)->carriedBy != stage.slaver,
			"d: the guard's 213 did not take the body off the warden" + tag);
		// The guard's own lift starts in the snatch tick (it does not wait for the next squad interval).
		test::require(entityOf(sim, stage.guard).has<state::InteractTask>() || entityOf(sim, stage.guard).has<state::CarryStep>()
			|| sim.combat()->find(stage.guard)->carrying == body, "d: the guard's lift did not start in the snatch tick" + tag);
		test::require(sim.arrestOrdered(stage.guard), "d: DA 30's order ended with the snatch" + tag);
		// The dropped body lies a few units off, where the warden's own order and the guard's both go for it: one of them lifts it (the guard's 213 snatches only
		// once, so the loser does not take it back) and cages it.
		const Outcome out = watch(game, stage, body, kCaseTicks, 20);
		test::require(out.caged && (out.guardCarried != out.wardenCarried), "d: nobody (or both) lifted the dropped body and caged it: " + out.text() + tag);
		record("d", out);
		clearBody(sim, body);
	}

	// e. DA 30 snatches once; the body then stays in a carrier's arms for good (held here every tick): the snatch loser's order ends at once (the finder no
	// longer resolves a carried body, rwh.124 section 5, no arrest wait), and it does not snatch again.
	if (which == 'e')
	{
		where = "e";
		const std::uint64_t body = setUp(game, stage, bump, 10.0);
		// A bystander (down, no orders) holds the body: no warden's order cages it behind the test.
		const state::Fighter& at = *sim.combat()->find(body);
		stage.slaver = spawnBody(sim, stage.guard, at.x + 2.0, at.z);
		fighterOf(sim, stage.slaver)->carrying = body;
		fighterOf(sim, body)->carriedBy = stage.slaver;
		stand(sim, stage.guard, at.x + 1.0, at.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatching), "e: the arrest order was refused" + tag);
		for (int i = 0; i < 3 && sim.combat()->find(stage.slaver)->carrying == body; ++i)
			step(game, stage);
		test::require(sim.combat()->find(stage.slaver)->carrying == 0, "e: the guard's 213 did not take the body off the carrier" + tag);
		bool again = false;
		int endedAt = -1;
		for (int i = 0; i < 20 * 140 && endedAt < 0; ++i)
		{
			// The carrier holds the body again (its hand and the body's carriedBy agree), as a lift that won the dropped body would.
			state::Fighter& hold = *fighterOf(sim, stage.slaver);
			state::Fighter& held = *fighterOf(sim, body);
			if (hold.carrying != body)
			{
				again = again || i > 0; // taken off it after the first tick: a second snatch
				hold.carrying = body;
				held.carriedBy = stage.slaver;
			}
			fighterOf(sim, stage.guard)->carrying = 0;
			step(game, stage);
			if (!sim.arrestOrdered(stage.guard))
				endedAt = i;
		}
		test::require(endedAt >= 0 && endedAt < 40, "e: the snatch loser's order did not end at once (ended at tick " + std::to_string(endedAt) + ")" + tag);
		test::require(sim.combat()->find(stage.guard)->carrying == 0 && sim.combat()->find(body)->carriedBy == stage.slaver, "e: the guard took the body again" + tag);
		test::require(!again, "e: the guard snatched the body a second time" + tag);
		clearBody(sim, body);
	}

	// f. The victim stood up before the lift: 240's isTargetDown is false, the lift is refused and the order stays queued (no expiry, no attack step of
	// its own: rwh.125). When the victim is down again the guard lifts and cages it.
	if (which == 'f')
	{
		where = "f";
		const std::uint64_t body = setUp(game, stage, bump, 30.0);
		state::Fighter& victim = *fighterOf(sim, body);
		victim.unconscious = false;
		victim.koTimer = 0.0;
		victim.faction = stage.home.owner.id; // the guard's own side: the town's police do not knock it out by themselves (that re-KO is the guard's own package)
		stand(sim, stage.guard, victim.x + 3.0, victim.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatchingInTown), "f: the arrest order was refused" + tag);
		for (int i = 0; i < 20 * 150; ++i)
		{
			// Held on its feet, so the order's lifetime is what is pinned.
			state::Fighter& held = *fighterOf(sim, body);
			held.unconscious = false;
			held.down = false;
			held.koTimer = 0.0;
			step(game, stage);
			test::require(sim.arrestOrdered(stage.guard), "f: the order on a standing victim ended at tick " + std::to_string(i) + tag);
			test::require(sim.combat()->find(body)->carriedBy == 0 && fighterOf(sim, stage.guard)->carrying == 0, "f: a standing victim was lifted" + tag);
		}
		fighterOf(sim, body)->unconscious = true;
		fighterOf(sim, body)->koTimer = 1.0e6; // with 0 the KO ends on the next tick, before the guard reaches it (k16.6.187)
		const Outcome out = watch(game, stage, body, kCaseTicks, 20);
		test::require(out.caged && out.guardCarried, "f: the guard did not lift and cage the victim once it was down again: " + out.text() + tag);
		record("f", out);
		clearBody(sim, body);
	}

	// g. A conscious victim at posture >= 2 (crouched) is lifted by 240: isTargetDown is true, unconscious is not needed.
	if (which == 'g')
	{
		where = "g";
		const std::uint64_t body = setUp(game, stage, bump, 30.0);
		state::Fighter& victim = *fighterOf(sim, body);
		victim.unconscious = false;
		victim.koTimer = 0.0;
		victim.faction = stage.home.owner.id;
		// Posture >= 2 on a conscious body (isTargetDown, rwh.124 4.1: a sneaking / crouched or crawling character). A really collapsed leg cannot be pinned
		// here: the collapse's forced KO re-latches every tick of the combat step.
		victim.crouched = true;
		victim.hold = true; // stays where it is: the lift's approach walk to a body out of reach is k16.6.191
		stand(sim, stage.guard, victim.x + 3.0, victim.z);
		test::require(sim.orderArrest(stage.guard, body, goap::kLiftPersonSnatchingInTown), "g: the arrest order was refused" + tag);
		// A conscious body pushes the guard off (bodies of the KO'd do not): until the lift's approach walk is ported (k16.6.191) the guard is put back beside it.
		Outcome out;
		for (int i = 0; i < kCaseTicks && !out.caged; ++i)
		{
			state::Fighter& guard = *fighterOf(sim, stage.guard);
			if (guard.carrying == 0 && !entityOf(sim, stage.guard).has<state::CarryStep>())
			{
				guard.x = victim.x + 2.0;
				guard.z = victim.z;
			}
			step(game, stage);
			out.caged = fighterOf(sim, body)->cageBuilding != 0;
			out.guardCarried = out.guardCarried || fighterOf(sim, stage.guard)->carrying == body;
			out.wardenCarried = out.wardenCarried || (fighterOf(sim, body)->carriedBy != 0 && fighterOf(sim, body)->carriedBy != stage.guard);
		}
		test::require(out.caged && out.guardCarried, "g: the guard did not lift and cage the conscious down victim: " + out.text() + tag);
		record("g", out);
		clearBody(sim, body);
	}
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || std::string(argv[1]).empty())
		{
			std::cout << "ksim_arrest_vs_capture: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::map<std::string, std::string> seen;
		// A fresh world per case: the wardens' routines and the cages' claims are the town's, and an earlier case leaves them spent.
		// argv[2]: only these cases (a part of the test: ctest runs the seven in parallel; the id-independence check is per case across the six bumps).
		const std::string only = argc > 2 ? argv[2] : "abcdefg";
		for (int bump = 0; bump <= 5; ++bump)
			for (const char which : {'a', 'b', 'c', 'd', 'e', 'f', 'g'})
			{
				if (only.find(which) == std::string::npos)
					continue;
				Game game(argv[1]);
				Stage stage;
				test::require(findCageHome(*game.sim, stage.home) && !stage.home.wardens.empty(), "a Cage Warden home with cages and wardens is in the world");
				// The area streams to Full detail around the first placed squad: the wardens' Fighters exist from then on.
				stage.guard = soloGuard(game, stage, stage.home.x + 40.0, stage.home.z + 40.0);
				stage.home.wardens.erase(std::remove_if(stage.home.wardens.begin(), stage.home.wardens.end(),
					[&](std::uint64_t id) { return game.sim->combat()->find(id) == nullptr; }), stage.home.wardens.end());
				test::require(!stage.home.wardens.empty(), "the Cage Warden squad has Fighters in the streamed area");
				run(game, stage, which, bump, seen);
			}
		for (const auto& [key, text] : seen)
			std::cout << "case " << key << ": " << text << "\n";
		std::cout << "ksim_arrest_vs_capture: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_arrest_vs_capture: FAILED: " << e.what() << "\n";
		return 1;
	}
}
