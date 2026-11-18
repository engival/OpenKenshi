// The bed resolver and 106 FIND_BED_AND_PUT_IN (task ds/bed-audit; dt29 / dt24_106):
//   - the registry: 106 is a goal on isCarriedDudeInBed with 99 PUT_SOMEONE_IN_BED as its provider;
//   - the shared resolver, live: a resident whose home has beds sleeps in its home (home first, no other building offered), and the
//     first bed in slot order is the one taken (the order of the other rungs and the ties are worldres::bedLadder153's unit test);
//   - 106 on the live world selector: a squad whose package carries the row, carrying a downed person, walks no further than the bed
//     of its home building and lays the person in it, asleep, the bed claimed for it.
// The town scorer and the selector run live; no NPC is ordered.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/detail.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "knav/navmesh.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

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

Simulation newGame(const kworld::LoadOrder& order)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessPopulation = 0.0; // a spawner with nobody in it: placeWildernessSquad needs one
	options.nestMultiplier = 0.0;
	return Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
}

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

flecs::entity squadIn(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::SquadInstance&) { found = i.value == id ? e : found; });
	test::require(found.is_valid(), "the squad exists in this sim");
	return found;
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

void runUntil(Simulation& sim, const std::function<bool()>& done, std::uint64_t maxTicks, const std::string& what)
{
	for (std::uint64_t i = 0; i < maxTicks; ++i)
	{
		if (done())
			return;
		sim.run(1);
	}
	test::require(done(), what);
}

void centreOn(Simulation& sim, const std::vector<std::array<double, 2>>& at)
{
	std::vector<state::GroundPoint> areas;
	for (const auto& p : at)
		areas.push_back({p[0], p[1]});
	sim.enqueue(Command::setPlayerAreas(areas));
	sim.run(1);
}

state::Fighter& fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<state::Fighter&>(*sim.combat()->find(id));
}

// A home-bound resident squad whose people can all be built.
struct Pick
{
	flecs::entity squad;
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
	std::set<std::uint16_t> tasks;
	std::size_t members = 0;
	bool leader = false; // a member spawned from the template's `leader` list (it runs the Leader list: the shop, the secure bed)
	std::string behaviour;
	std::string package;
};

std::vector<Pick> residentSquads(Simulation& sim, const routines::PackageTable& packages, const CombatData& combat)
{
	const BehaviourRegistry& registry = sim.ai()->registry();
	const kdata::GameData& data = *sim.gameData();
	std::vector<Pick> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id == 0 || instance.source != state::SpawnSource::TownResident)
			return;
		const Mapping& mapping = registry.squad(instance.templateSid);
		if (mapping.behaviour != Behaviour::ShopKeeper && mapping.behaviour != Behaviour::ResidentLife)
			return;
		const routines::CompiledPackage* package = packages.find(mapping.package);
		if (package == nullptr || std::strcmp(routines::routineStatus(*package), "modelled") != 0)
			return;
		Pick pick;
		pick.squad = squad;
		pick.id = id.value;
		pick.x = at.x;
		pick.z = at.z;
		pick.behaviour = behaviourName(mapping.behaviour);
		pick.package = mapping.package;
		for (const auto& list : package->lists)
			for (const routines::GoalRow& row : list)
				pick.tasks.insert(row.task);
		found.push_back(std::move(pick));
	});
	std::sort(found.begin(), found.end(), [](const Pick& a, const Pick& b) { return a.id < b.id; });
	std::vector<Pick> usable;
	for (Pick& candidate : found)
	{
		const std::vector<flecs::entity> members = membersOf(sim, candidate.squad);
		if (members.empty() || members.size() > 6)
			continue;
		bool buildable = true;
		for (const flecs::entity member : members)
		{
			const state::MemberRole* role = member.get<state::MemberRole>();
			candidate.leader = candidate.leader || (role != nullptr && role->list == state::kRoleLeader);
			std::uint64_t dice = 1;
			try
			{
				combat.makeFighter(data, member.get<state::CharacterInstance>()->templateSid, "", dice, candidate.squad.get<state::SquadInstance>()->templateSid);
			} catch (const std::exception&) {
				buildable = false;
			}
		}
		if (buildable)
		{
			candidate.members = members.size();
			usable.push_back(std::move(candidate));
		}
	}
	return usable;
}

bool isBed(std::uint16_t goal)
{
	return goal == routines::task::GoHomeToBed || goal == routines::task::GoHomeToBedSecure;
}

double hourOf(Simulation& sim)
{
	return std::fmod(trader::gameHours(sim.clockTick()), 24.0);
}

std::string clockText(Simulation& sim)
{
	char buffer[16];
	std::snprintf(buffer, sizeof buffer, "%05.2f", hourOf(sim));
	return buffer;
}




// A squad template whose Squad list carries 106 and which spawns a few people (the kidnap-style world squads).
std::vector<std::string> templatesWith(Simulation& sim, const routines::PackageTable& packages, std::uint16_t task)
{
	std::vector<std::string> found;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
	{
		const routines::CompiledPackage* package = packages.find(sim.ai()->registry().squad(record->stringID).package);
		if (package == nullptr)
			continue;
		for (const routines::GoalRow& row : package->list(routines::Role::Squad))
			if (row.task == task && routines::windowHolds(row, 2.0))
			{
				found.push_back(record->stringID);
				break;
			}
	}
	return found;
}

// Heft's grid cell (the baked world's region_11_4.knav) and its town centre (the cases run inside its 2000 u cage finder).
constexpr int kHeftCellX = 46;
constexpr int kHeftCellZ = 16;
constexpr double kHeftX = 68772.98;
constexpr double kHeftZ = -71496.61;
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
		if (state::Fighter* me = &fighterOf(*sim, player))
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


void bedChoice(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	// ---- the registry rows ----
	{
		const goap::TaskRow* put = goap::rowOf(goap::kPutInBed);
		test::require(put != nullptr && put->effCount == 1 && put->eff[0].sym == goap::Sym::CarriedDudeInBed && put->eff[0].value,
			"99 PUT_SOMEONE_IN_BED provides isCarriedDudeInBed");
		const goap::TaskRow* find = goap::rowOf(routines::task::FindBedAndPutIn);
		test::require(find != nullptr && find->hasGoalState && find->goalState.sym == goap::Sym::CarriedDudeInBed && find->goalState.value,
			"106 is a goal on isCarriedDudeInBed");
	}

	Simulation sim = newGame(order);
	enable(sim, combat);
	{
		std::string start;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				start = record->stringID;
		sim.enqueue(Command::newGame(start, "Tester", 1));
		sim.run(3);
	}
	sim.setClockTick(tickAt(2, 1, 0));
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	std::vector<const Pick*> chosen;
	for (const Pick& pick : picks)
		if ((pick.tasks.count(153) != 0 || pick.tasks.count(239) != 0) && chosen.size() < 10)
			chosen.push_back(&pick);
	test::require(chosen.size() >= 4, "resident squads with a bed goal (" + std::to_string(chosen.size()) + ")");
	std::vector<std::array<double, 2>> at;
	for (const Pick* pick : chosen)
		at.push_back({pick->x, pick->z});
	centreOn(sim, at);
	runUntil(sim, [&] { return std::all_of(chosen.begin(), chosen.end(), [&](const Pick* p) { return squadIn(sim, p->id).has<state::SquadRoutine>(); }); },
		4000, "the squads were never promoted");

	// ---- home first, in slot order ----
	std::uint64_t sleeperId = 0;
	routines::Site sleeperBed;
	std::size_t asleep = 0;
	runUntil(sim, [&]
	{
		for (const Pick* pick : chosen)
			for (const flecs::entity member : membersOf(sim, squadIn(sim, pick->id)))
			{
				const state::Routine* r = member.get<state::Routine>();
				const state::Fighter& f = fighterOf(sim, idOf(member));
				if (r == nullptr || !f.asleep || !isBed(r->goal) || r->target.kind != static_cast<std::uint8_t>(routines::SiteKind::Bed))
					continue;
				++asleep;
				if (f.homeBuilding != 0)
					test::require(r->target.object == f.homeBuilding, "a resident whose home has beds sleeps in its home building");
				if (sleeperId == 0 && f.homeBuilding != 0)
				{
					sleeperId = idOf(member);
					sleeperBed = {routines::SiteKind::Bed, r->target.object, r->target.slot};
				}
			}
		return sleeperId != 0;
	}, 6000, "no resident went to bed");
	std::cout << "  sleeper " << sleeperId << " in building " << sleeperBed.object << " slot " << sleeperBed.slot << "\n";

	std::cout << "home first ok\n";
}

// ---- 106 on the live selector over the real Heft world ----
void putInBed(const char* bake)
{
	Game game(bake);
	Simulation& sim = *game.sim;
	sim.setClockTick(tickAt(2, 1, 0));
	std::vector<std::array<double, 2>> at{{kHeftX, kHeftZ}};
	game.bringPlayer(kHeftX + 900.0, sim.groundHeight(kHeftX, kHeftZ).value_or(0.0), kHeftZ + 900.0);
	centreOn(sim, at);
	// A resident asleep in Heft: its bed building is the squad's home.
	std::uint64_t sleeperId = 0;
	std::uint64_t sleeperBuilding = 0;
	const auto found = [&]() -> bool
	{
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (!m.fighter.asleep || m.fighter.playerControlled)
				continue;
			flecs::entity who;
			ecsOf(sim).each([&](flecs::entity e, const state::Id& i) { if (i.value == m.id) who = e; });
			const state::Routine* r = who.is_valid() ? who.get<state::Routine>() : nullptr;
			if (r == nullptr || r->target.kind != static_cast<std::uint8_t>(routines::SiteKind::Bed) || std::hypot(m.fighter.x - kHeftX, m.fighter.z - kHeftZ) > 2000.0)
				continue;
			sleeperId = m.id;
			sleeperBuilding = r->target.object;
			return true;
		}
		return false;
	};
	for (int i = 0; i < 6000 && !found(); ++i)
		sim.run(1);
	{
		std::size_t routines = 0, sleepers = 0, residents = 0;
		ecsOf(sim).each([&](const state::SquadRoutine&, const state::Position& p)
		{
			if (std::hypot(p.x - kHeftX, p.z - kHeftZ) < 2500.0)
				++routines;
		});
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (std::hypot(m.fighter.x - kHeftX, m.fighter.z - kHeftZ) < 2500.0)
				++residents;
			if (m.fighter.asleep)
				++sleepers;
		}
		std::cout << "  squad routines " << routines << " fighters near " << residents << " asleep " << sleepers << " hour " << clockText(sim) << "\n";
	}
	test::require(sleeperId != 0, "no Heft resident went to bed");
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<std::string> templates = templatesWith(sim, packages, routines::task::FindBedAndPutIn);
	test::require(!templates.empty(), "a squad template whose Squad list carries 106");
	std::cout << "  " << templates.size() << " templates carry 106 in the Squad list; sleeper " << sleeperId << "\n";
	// ---- a squad with the row, carrying a downed person, lays it in a bed of its home ----
	const state::Fighter& sleeper = fighterOf(sim, sleeperId);
	const double bx = sleeper.x;
	const double bz = sleeper.z;
	const std::uint64_t faction = sleeper.faction;
	const std::uint64_t homeKey = sleeperBuilding;
	flecs::entity squad;
	for (const std::string& sid : templates)
	{
		squad = sim.placeWildernessSquad(sid, bx + 6.0, bz + 6.0, 2); // two people: the first is the squad's follow leader, which never scores
		if (!squad.is_valid())
			continue;
		{
			bool buildable = !membersOf(sim, squad).empty();
			for (const flecs::entity member : membersOf(sim, squad))
			{
				std::uint64_t dice = 1;
				try
				{
					game.combat->makeFighter(*sim.gameData(), member.get<state::CharacterInstance>()->templateSid, "", dice, sid);
				} catch (const std::exception&) {
					buildable = false;
				}
			}
			if (!buildable)
				continue;
		}
		game.bringPlayer(bx + 900.0, sim.groundHeight(bx, bz).value_or(0.0), bz + 900.0);
		at.push_back({bx, bz});
		centreOn(sim, at);
		bool built = false;
		for (int i = 0; i < 400 && !built; ++i)
		{
			sim.run(1);
			const std::vector<flecs::entity> now = membersOf(sim, squad);
			built = !now.empty() && std::all_of(now.begin(), now.end(), [&](flecs::entity m) { return sim.combat()->find(idOf(m)) != nullptr; });
		}
		if (built)
			break;
		squad = flecs::entity();
	}
	test::require(squad.is_valid(), "a 106 squad was built");
	std::vector<flecs::entity> members = membersOf(sim, squad);
	std::string factionSid;
	ecsOf(sim).each([&](flecs::entity e, const state::FactionState& fs)
	{
		const state::Id* id = e.get<state::Id>();
		if (id != nullptr && id->value == faction)
			factionSid = fs.templateSid;
	});
	for (flecs::entity member : members)
	{
		state::Fighter& f = fighterOf(sim, idOf(member));
		f.faction = faction;
		f.homeBuilding = homeKey;
		f.x = bx + 1.0;
		f.z = bz + 1.0;
		f.clearRoute();
		if (!factionSid.empty())
			member.set<state::Allegiance>(state::Allegiance{factionSid});
	}
	{
		flecs::entity squadEntity = squad;
		state::SquadInstance instance = *squadEntity.get<state::SquadInstance>();
		instance.homeBuilding = homeKey;
		squadEntity.set<state::SquadInstance>(instance);
	}
	// The person: a downed copy of the first member, in the second member's arms (the carry itself is the chain's own, 168 / 68; the
	// state is set up directly, as the lift is not under test).
	const flecs::entity carrier = members.back(); // not the follow leader (the selector skips that one)
	const std::uint64_t carrierId = idOf(carrier);
	std::uint64_t bodyId = 0;
	{
		state::Fighter body = fighterOf(sim, carrierId);
		body.playerControlled = false;
		body.unconscious = true;
		body.koTimer = 1.0e6;
		body.asleep = false;
		body.carrying = 0;
		body.carriedBy = carrierId;
		body.target = 0;
		body.orderTarget = 0;
		body.homeBuilding = 0;
		body.faction = 0;
		body.team = 0;
		body.clearRoute();
		const flecs::entity entity = sim.spawnFighter(std::move(body));
		entity.set<state::Allegiance>(state::Allegiance{"test-victim"});
		bodyId = idOf(entity);
	}
	fighterOf(sim, carrierId).carrying = bodyId;

	// The resident that lay down in this home holds the building's bed (a home may have only one): it dies, and the book frees the bed.
	fighterOf(sim, sleeperId).dead = true;
	runUntil(sim, [&] { return sim.routineBook().held(sleeperId).kind != routines::SiteKind::Bed; }, 2000, "the dead resident still holds the bed");
	const std::uint64_t t0 = sim.clockTick();
	bool planned = false;
	const auto dump = [&]
	{
		const state::Routine* r = carrier.get<state::Routine>();
		const state::Fighter& c = fighterOf(sim, carrierId);
		std::cout << "  carrier goal " << (r != nullptr ? int(r->goal) : -1) << " chain " << (r != nullptr ? r->chainTarget : 0) << " carrying " << c.carrying
				  << " asleep " << c.asleep << " dead " << c.dead << " at " << c.x << "," << c.z << " bed at " << bx << "," << bz << "\n";
		const std::size_t from = game.taskLog.size() > 15 ? game.taskLog.size() - 15 : 0;
		for (std::size_t i = from; i < game.taskLog.size(); ++i)
			std::cout << "  log " << game.taskLog[i] << "\n";
	};
	// No settling wait before the watch (it was 600 ticks of run, then dump): the carrier is decided in its squad's own slice of the second
	// (Simulation::inSlice, k16.6.320), not at the second's tick, so 106 can be planned and the put-in done inside those ticks, and the check
	// below would see the finished routine (goal 106 left with no chain, the person already in the bed). The watch starts at once.
	dump();
	runUntil(sim, [&]
	{
		const state::Routine* r = carrier.get<state::Routine>();
		// A 106 routine with no chain is the one the carrier held before it was handed the person (not yet re-scored with the body): not
		// the plan under test. The person is put in the bed only through a 106 that chains it, so a missing chain still fails below.
		if (r != nullptr && r->goal == routines::task::FindBedAndPutIn && r->chainTarget != 0)
		{
			planned = true;
			test::require(r->chainTarget == bodyId, "106 carries the downed person as its chain target");
			test::require(r->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Bed), "106's target is a bed");
			test::require(r->target.object == homeKey, "the bed is in the member's home building (the resolver's first rung)");
			test::require(std::find(r->plan.begin(), r->plan.end(), goap::kPutInBed) != r->plan.end(), "its plan ends with 99 PUT_SOMEONE_IN_BED");
		}
		return fighterOf(sim, bodyId).asleep;
	}, 3000, "the downed person was never put in a bed");
	test::require(planned, "106 was scored and held a routine before the put-in");
	const state::Fighter& body = fighterOf(sim, bodyId);
	test::require(fighterOf(sim, carrierId).carrying == 0 && body.carriedBy == 0, "the carry is released");
	test::require(sim.routineBook().held(bodyId).kind == routines::SiteKind::Bed, "the bed is claimed for the person");
	test::require(sim.routineBook().held(bodyId).object == homeKey, "in the home building");
	std::cout << "  106 put person " << bodyId << " in bed " << sim.routineBook().held(bodyId).object << ":" << sim.routineBook().held(bodyId).slot
			  << " after " << (sim.clockTick() - t0) << " ticks\n";
	std::cout << "106 ok\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_bed_choice KSIM_NAVWORLD");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		bedChoice(order, combat);
		putInBed(argv[1]);
		std::cout << "ksim_bed_choice: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_bed_choice: FAILED: " << e.what() << "\n";
		return 1;
	}
}
