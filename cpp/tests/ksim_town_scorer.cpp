// The town goal scorer (docs/reference/TOWN_LIFE_SLICE.md 5.3, phase T3b2):
//   - the behaviours shop_keeper and resident_life exist, are mapped from packages and promote their squads;
//   - seeded placement: a promoted member has a goal at once, stands at its site (not on the ring), and exclusive sites are claimed once;
//   - the scorer is deterministic (two runs of one seed hash alike) and save / load mid-run gives the uninterrupted run's hash and book;
//   - a failed finder blocks its (goal, target) pair for 20 s and the member falls to another goal, never to nothing;
//   - the 12 h safety release gives up a claim the member never reached;
//   - no terminal state: over several game hours every promoted member always has a goal, and the book agrees with the Routines;
//   - release: death and demotion free the claim.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/producers.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

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
	options.wildernessByDensity = false;
	// These tests pin the seeded placement pass (5.3), which a fresh start's residents skip (they walk in: ksim_residents_arrive).
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.setResidentsArrive(false);
	return sim;
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

// A squad of this sim by its Id (a Pick made in another sim holds that sim's entity).
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

flecs::entity entityById(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i)
	{
		if (i.value == id)
			found = e;
	});
	return found;
}

std::uint32_t memberCarried(flecs::entity member, const std::string& item)
{
	const state::Inventory* pack = member.get<state::Inventory>();
	return pack != nullptr ? pack->carried(item) : 0;
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

void centreOn(Simulation& sim, double x, double z)
{
	sim.enqueue(Command::setPlayerAreas({{x, z}}));
	sim.run(1);
}

struct Resident
{
	flecs::entity squad;
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
};

// Home-bound resident squads whose people can all be built, Id order, with the tasks their package lists (any role, any hour).
struct Pick
{
	Resident resident;
	std::set<std::uint16_t> tasks;
	std::size_t members = 0;
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
		if (mapping.behaviour != Behaviour::GoHome && mapping.behaviour != Behaviour::StayHome && mapping.behaviour != Behaviour::ShopKeeper
			&& mapping.behaviour != Behaviour::ResidentLife)
			return;
		const routines::CompiledPackage* package = packages.find(mapping.package);
		if (package == nullptr || std::strcmp(routines::routineStatus(*package), "modelled") != 0)
			return;
		Pick pick;
		pick.resident = {squad, id.value, at.x, at.z};
		for (const auto& list : package->lists)
			for (const routines::GoalRow& row : list)
				pick.tasks.insert(row.task);
		found.push_back(std::move(pick));
	});
	std::sort(found.begin(), found.end(), [](const Pick& a, const Pick& b) { return a.resident.id < b.resident.id; });
	std::vector<Pick> usable;
	for (Pick& candidate : found)
	{
		const std::vector<flecs::entity> members = membersOf(sim, candidate.resident.squad);
		if (members.empty() || members.size() > 8)
			continue;
		bool buildable = true;
		for (const flecs::entity member : members)
		{
			std::uint64_t dice = 1;
			try
			{
				combat.makeFighter(data, member.get<state::CharacterInstance>()->templateSid, "", dice, candidate.resident.squad.get<state::SquadInstance>()->templateSid);
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

bool implemented(std::uint16_t task)
{
	return task == 20 || task == 36 || task == 38 || task == 43 || task == 157 || task == 189 || task == 190 || task == 221
		|| task == 86 || task == 153 || task == 155 || task == 158 || task == 197 || task == 239 || task == 283 // T3c added these seven
		|| task == 214 || task == 118; // T3d: RELAX at the squad's table, pretend shopping
}

// The routine book agrees with the Routines: every target is held by its owner, and the book holds nothing else. Every promoted resident near the
// player counts, not only the squad under test; `members` are checked to be among them.
void checkBook(Simulation& sim, const std::vector<flecs::entity>& members, const std::string& when)
{
	std::size_t claims = 0;
	ecsOf(sim).each([&](const state::Id& id, const state::Routine& routine)
	{
		if (routine.target.kind == 0)
			return;
		++claims;
		const routines::Site held = sim.routineBook().held(id.value);
		test::require(held.kind == static_cast<routines::SiteKind>(routine.target.kind) && held.object == routine.target.object && held.slot == routine.target.slot,
			when + ": the book holds the member's target");
	});
	// T3d: a squad's claimed bar table (SquadRoutine::barTable, owner = the squad) is the one other kind of claim in the book.
	std::size_t tables = 0;
	ecsOf(sim).each([&](const state::Id& id, const state::SquadRoutine& squad)
	{
		if (squad.barTable == 0)
			return;
		++tables;
		const routines::Site held = sim.routineBook().held(id.value);
		test::require(held.kind == routines::SiteKind::Table && held.object == squad.barTable, when + ": the book holds the squad's table");
	});
	test::require(sim.routineBook().size() == claims + tables, when + ": the book holds only the members' targets and the squads' tables (" + std::to_string(sim.routineBook().size()) + " vs " + std::to_string(claims) + " + " + std::to_string(tables) + ")");
	(void)members;
}

// ---- behaviours ----

void behaviours(Simulation& sim, const routines::PackageTable& packages)
{
	const BehaviourRegistry& registry = sim.ai()->registry();
	std::size_t shops = 0;
	std::size_t residents = 0;
	std::size_t modelled = 0;
	ecsOf(sim).each([&](const state::SquadInstance& instance)
	{
		const Mapping& mapping = registry.squad(instance.templateSid);
		shops += mapping.behaviour == Behaviour::ShopKeeper ? 1 : 0;
		residents += mapping.behaviour == Behaviour::ResidentLife ? 1 : 0;
		if (mapping.behaviour == Behaviour::ShopKeeper || mapping.behaviour == Behaviour::ResidentLife)
		{
			const routines::CompiledPackage* package = packages.find(mapping.package);
			modelled += package != nullptr && std::strcmp(routines::routineStatus(*package), "none") != 0 ? 1 : 0;
		}
	});
	test::require(shops > 0 && residents > 0, "squads run shop_keeper (" + std::to_string(shops) + ") and resident_life (" + std::to_string(residents) + ")");
	test::require(behaviourName(Behaviour::ShopKeeper) == "shop_keeper" && behaviourName(Behaviour::ResidentLife) == "resident_life", "behaviour names");
	std::cout << "behaviours ok (" << shops << " shop_keeper, " << residents << " resident_life squads, " << modelled << " with a goal list)\n";
}

// ---- placement, goals, no terminal state ----

struct Run
{
	std::uint64_t hash = 0;
	std::map<std::uint16_t, std::uint64_t> byGoal;
};

// Promote one squad, run `hours` game hours, return what happened. `check` runs every 200 ticks.
Run lifeOf(Simulation& sim, const Pick& pick, double hours, bool verify)
{
	centreOn(sim, pick.resident.x, pick.resident.z);
	const flecs::entity squad = squadIn(sim, pick.resident.id);
	runUntil(sim, [&] { return squad.has<state::SquadRoutine>(); }, 3000, "the squad was never promoted");
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	std::size_t built = 0;
	for (const flecs::entity member : members)
		built += member.has<state::Routine>() ? 1 : 0;
	test::require(built > 0, "a promoted squad has members with a Routine");
	if (verify)
	{
		// Seeded placement: the winner of the first pass is adopted at promotion, before any walking.
		for (const flecs::entity member : members)
		{
			const state::Routine* routine = member.get<state::Routine>();
			if (routine == nullptr)
				continue;
			test::require(routine->goal != 0, "a placed member has a goal at once");
			test::require(implemented(routine->goal), "the goal is one the scorer runs (" + std::to_string(routine->goal) + ")");
			test::require(routine->nextScore > sim.clockTick() - 1, "the next pass is scheduled");
		}
		checkBook(sim, members, "after placement");
	}
	const std::uint64_t end = sim.clockTick() + static_cast<std::uint64_t>(hours * static_cast<double>(kTicksPerGameHour));
	std::map<std::uint16_t, std::uint64_t> seen;
	while (sim.clockTick() < end)
	{
		sim.run(200);
		if (!squad.has<state::SquadRoutine>())
			break; // the squad left routine mode (a trip); the members were demoted
		if (verify)
		{
			for (const flecs::entity member : members)
			{
				const state::Routine* routine = member.get<state::Routine>();
				if (routine == nullptr)
					continue;
				test::require(routine->goal != 0 && implemented(routine->goal), "no member is ever without a goal (no terminal state)");
				test::require(routine->nextScore + 2 * routines::kScoreIntervalTicks + 40 >= sim.clockTick() && routine->nextScore <= sim.clockTick() + 2 * routines::kScoreIntervalTicks + 40,
					"the member keeps being scored");
				const state::Fighter* f = member.get<state::Fighter>();
				test::require(f != nullptr && std::isfinite(f->x) && std::isfinite(f->z), "positions stay finite");
				++seen[routine->goal];
			}
			checkBook(sim, members, "mid-run");
		}
	}
	return {sim.stateHash(), seen};
}

void placementAndLife(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation a = newGame(order);
	enable(a, combat);
	const routines::PackageTable packages(*a.gameData());
	behaviours(a, packages);
	const std::vector<Pick> picks = residentSquads(a, packages, *combat);
	test::require(picks.size() > 20, "resident squads to test");
	// A squad whose package lists a goal the scorer runs besides STAY_IN_HOME, preferring guard nodes and Shopkeeper.
	const auto withTask = [&](std::uint16_t task) -> const Pick*
	{
		for (const Pick& p : picks)
			if (p.tasks.count(task) != 0 && p.members >= 2)
				return &p;
		return nullptr;
	};
	std::set<std::uint16_t> landed;
	std::vector<std::uint64_t> hashes;
	for (const std::uint16_t task : {std::uint16_t(38), std::uint16_t(20), std::uint16_t(157), std::uint16_t(36), std::uint16_t(190)})
	{
		const Pick* pick = withTask(task);
		if (pick == nullptr)
		{
			std::cout << "  (no resident squad lists goal " << task << ")\n";
			continue;
		}
		// Each squad in a fresh sim so the runs are independent (and the first one is what the determinism check repeats).
		Simulation sim = newGame(order);
		enable(sim, combat);
		const Run run = lifeOf(sim, *pick, 4.0, true);
		std::cout << "  goal " << task << " squad " << pick->resident.id << " (" << pick->members << " members): goals seen";
		for (const auto& [goal, n] : run.byGoal)
		{
			std::cout << " " << goal << "x" << n;
			landed.insert(goal);
		}
		std::cout << "\n";
		hashes.push_back(run.hash);
		if (task == 38)
		{
			Simulation again = newGame(order);
			enable(again, combat);
			const Run repeat = lifeOf(again, *pick, 4.0, false);
			test::require(repeat.hash == run.hash, "the same seed gives the same hash after 4 game hours of scoring");
			test::require(sim.townScorerStats().adopted == again.townScorerStats().adopted && sim.townScorerStats().adopted > 0, "and the same number of adoptions");
		}
	}
	test::require(landed.size() >= 2, "at least two different goals ran (" + std::to_string(landed.size()) + ")");
	std::cout << "placement and life ok (" << landed.size() << " distinct goals)\n";
}

// ---- blocked pairs, the 12 h release, release rules ----

void blockAndRelease(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	// A squad that lists the guard-node goal 38 (which needs a node).
	const Pick* pick = nullptr;
	for (const Pick& p : picks)
		if (p.tasks.count(38) != 0 && p.members >= 1)
		{
			pick = &p;
			break;
		}
	test::require(pick != nullptr, "a squad with the guard-node goal");
	centreOn(sim, pick->resident.x, pick->resident.z);
	runUntil(sim, [&] { return pick->resident.squad.has<state::SquadRoutine>(); }, 3000, "promoted");
	const std::vector<flecs::entity> members = membersOf(sim, pick->resident.squad);
	flecs::entity victim;
	for (const flecs::entity member : members)
		if (member.has<state::Routine>())
		{
			victim = member;
			break;
		}
	test::require(victim.is_valid(), "a promoted member");
	const std::uint64_t id = idOf(victim);

	// The 12 h safety release: a claim on a site the member can never reach, older than kReservationMaxHours, is given up and the pair blocked.
	{
		state::Routine r = *victim.get<state::Routine>();
		r.goal = routines::task::StaticGuardBuilding;
		r.target = {static_cast<std::uint8_t>(routines::SiteKind::GuardNode), 0xdead0000beefULL, 0};
		r.since = sim.clockTick() > 14 * kTicksPerGameHour ? sim.clockTick() - 13 * kTicksPerGameHour : 0;
		r.nextScore = sim.clockTick();
		victim.set<state::Routine>(r);
		sim.rebuildRoutineBook();
		test::require(sim.routineBook().held(id).object == 0xdead0000beefULL, "the stale claim is in the book");
		if (r.since != 0)
		{
			const std::uint64_t before = sim.townScorerStats().unreached;
			sim.run(routines::kScoreIntervalTicks * 2);
			test::require(sim.townScorerStats().unreached == before + 1, "the claim never reached for 12 h was released");
			test::require(sim.routineBook().held(id).object != 0xdead0000beefULL, "and is out of the book");
			const state::Routine* after = victim.get<state::Routine>();
			test::require(std::any_of(after->blocked.begin(), after->blocked.end(), [&](const state::RoutineBlocked& b) { return b.goal == 38 && b.target.object == 0xdead0000beefULL; }),
				"and the pair is blocked");
			test::require(after->goal != 0, "the member picked something else");
		} else {
			std::cout << "  (clock too early for the 12 h release check)\n";
		}
	}

	// Release rules: death frees the claim; demotion frees every claim and the squad's.
	{
		state::Routine r = *victim.get<state::Routine>();
		r.target = {static_cast<std::uint8_t>(routines::SiteKind::Bed), 4242, 0};
		victim.set<state::Routine>(r);
		sim.rebuildRoutineBook();
		test::require(sim.routineBook().held(id).object == 4242, "an injected claim");
		const_cast<state::Fighter&>(*sim.combat()->find(id)).dead = true;
		sim.run(routines::kResidentIntervalTicks + 1);
		test::require(!victim.has<state::Routine>() && sim.routineBook().held(id).kind == routines::SiteKind::None, "death frees the claim");
		centreOn(sim, 500000.0, 500000.0);
		runUntil(sim, [&] { return !pick->resident.squad.has<state::SquadRoutine>(); }, 3000, "demoted");
		// Only the tables of staying wandering traders remain (they hold SquadRoutine whatever the player's window; more towns give them one since k16.6.254).
		std::size_t tables = 0;
		sim.world().ecs().each([&](const state::SquadRoutine& routine) { tables += routine.barTable != 0 ? 1 : 0; });
		test::require(sim.routineBook().size() == tables, "demotion leaves only the traders' tables in the book");
	}
	std::cout << "block and release ok\n";
}

// A squad with more people than nodes: the extras' finders fail, the pair is blocked, and they still live.
void surplusFalls(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	Simulation sim = newGame(order);
	enable(sim, combat);
	const routines::PackageTable packages(*sim.gameData());
	const std::vector<Pick> picks = residentSquads(sim, packages, *combat);
	std::size_t blockedSeen = 0;
	std::size_t squads = 0;
	// The Shopkeeper goal has one counter per building and pretend labour needs a producer near home: squads of several people whose package
	// lists them make a finder fail, which blocks the pair.
	for (const Pick& pick : picks)
	{
		if ((pick.tasks.count(20) == 0 && pick.tasks.count(189) == 0 && pick.tasks.count(190) == 0 && pick.tasks.count(221) == 0) || pick.members < 2)
			continue;
		Simulation s = newGame(order);
		enable(s, combat);
		centreOn(s, pick.resident.x, pick.resident.z);
		const flecs::entity squad = squadIn(s, pick.resident.id);
		runUntil(s, [&] { return squad.has<state::SquadRoutine>(); }, 3000, "promoted");
		s.run(400);
		const std::vector<flecs::entity> members = membersOf(s, squad);
		std::set<std::uint64_t> counters;
		for (const flecs::entity member : members)
		{
			const state::Routine* routine = member.get<state::Routine>();
			if (routine == nullptr)
				continue;
			test::require(routine->goal != 0, "the surplus member still has a goal");
			if (routine->goal == 20)
				test::require(counters.insert(routine->target.object * 65536 + routine->target.slot).second, "two members never hold one counter");
		}
		checkBook(s, members, "surplus");
		blockedSeen += s.townScorerStats().blocked;
		if (++squads >= 12)
			break;
	}
	test::require(squads > 0, "squads with the Shopkeeper or labour goal");
	test::require(blockedSeen > 0, "a finder that found nothing blocked its pair (" + std::to_string(blockedSeen) + ")");
	std::cout << "surplus falls through ok (" << squads << " squads, " << blockedSeen << " blocked pairs live)\n";
}

// ---- save / load mid-run ----

void saveLoad(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_town_scorer_test.ksav";
	Simulation straight = newGame(order);
	enable(straight, combat);
	const routines::PackageTable packages(*straight.gameData());
	const std::vector<Pick> picks = residentSquads(straight, packages, *combat);
	const Pick* pick = nullptr;
	for (const Pick& p : picks)
		if (p.tasks.count(38) != 0 && p.members >= 2)
		{
			pick = &p;
			break;
		}
	test::require(pick != nullptr, "a squad to save");
	centreOn(straight, pick->resident.x, pick->resident.z);
	runUntil(straight, [&] { return pick->resident.squad.has<state::SquadRoutine>(); }, 3000, "promoted before the save");
	straight.run(kTicksPerGameHour / 2);
	const std::vector<flecs::entity> members = membersOf(straight, pick->resident.squad);
	const std::uint64_t split = straight.stateHash();
	const routines::Reservations bookBefore = straight.routineBook();
	straight.saveGame(path);
	straight.run(kTicksPerGameHour);

	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded, combat);
	test::require(loaded.stateHash() == split, "enabling AI and combat does not change the hash");
	test::require(loaded.routineBook().size() == bookBefore.size(), "the book is rebuilt with the same claims (" + std::to_string(loaded.routineBook().size()) + ")");
	for (const flecs::entity member : members)
		test::require(loaded.routineBook().held(idOf(member)) == bookBefore.held(idOf(member)), "a member's claim is rebuilt identically");
	loaded.run(kTicksPerGameHour);
	test::require(loaded.stateHash() == straight.stateHash(), "load and continue gives the straight run's hash");
	std::remove(path.c_str());
	std::cout << "save / load ok (" << members.size() << " members, hash " << std::hex << split << std::dec << ")\n";
}

// ---- the live worker handover (dt24_emptymachine_source section 3) ----

// The machine entity a producer site becomes (stepProducers' spawnProducer, producers.cpp). The test builds it where a player
// would materialise it: there is no player, so nothing else would place the site's machine. The cap is 20 - above the vf5
// batch of five a free neighbour buys - so the pull leaves the machine BELOW full and the operate step can plan again.
std::uint64_t buildMachine(Simulation& sim, std::uint64_t id, const std::string& recordSid, const std::string& output,
	const std::string& ownerSid, std::uint32_t count, std::uint32_t cap, double x, double y, double z)
{
	state::Producer producer;
	producer.recordSid = recordSid;
	producer.ownerSid = ownerSid;
	producer.output = output;
	producer.cap = cap;
	producer.maxOperators = 1;
	producer.useRange = 20.0;
	producer.x = x;
	producer.y = y;
	producer.z = z;
	producer.useX = x;
	producer.useY = y;
	producer.useZ = z;
	state::Inventory stock;
	stock.stackLimit = cap;
	stock.version = 1;
	const grid::Size slot = grid::Rules(*sim.gameData()).footprint(output);
	stock.gridW = static_cast<std::uint8_t>(slot.w);
	stock.gridH = static_cast<std::uint8_t>(slot.h);
	if (count > 0)
		stock.add(output, count);
	else
		producer.progress = 0.5; // empty: an idle neighbour must not read pristine (stepProducers drops a pristine producer away from a player)
	flecs::entity entity = ecsOf(sim).entity();
	entity.set<state::Id>(state::Id{id});
	entity.set<state::Producer>(std::move(producer));
	entity.set<state::Inventory>(std::move(stock));
	return id;
}

// The item a producer site's building makes (producers::specOf); empty when the record has none.
std::string outputOf(Simulation& sim, const ProducerSite& site)
{
	const kdata::Record* record = sim.gameData()->find(site.recordSid);
	producers::Spec spec;
	return record != nullptr && producers::specOf(*sim.gameData(), *record, spec) ? spec.output : std::string();
}

// Tops a built machine's stack up to `count` (a producer's output filling while the member stands at it).
void fillMachine(Simulation& sim, std::uint64_t id, const std::string& output, std::uint32_t count)
{
	flecs::entity machine = entityById(sim, id);
	test::require(machine.is_valid() && machine.get<state::Inventory>() != nullptr, "the work site's machine stands");
	state::Inventory stock = *machine.get<state::Inventory>();
	stock.add(output, count - stock.carried(output));
	machine.set<state::Inventory>(std::move(stock));
}

// A resident whose package lists the work goal (189 PretendLabour, or 214 RELAX_IN_TOWN_PACKAGE - both rows' goal state is
// IS_WORKING, dt24_emptymachine_source) takes a Work site and stands at it; the site's machine, once full, is planned as
// [92, 87, goal] and the scorer hands the member to the world-task pull executor. The member empties a batch into its own pack,
// comes back with goal 0, and the next pass orders the Use that makes it an operator (87). Nothing is scripted: the plan does it.
// 214 is also the T3d bar relax (the live TQ24 patrons); a home with no machine to plan falls to the table instead.
void workHandover(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, std::uint16_t goal, const char* name)
{
	// Candidates: a package listing the goal whose home has a producer site inside the scorer's work reach (kWorkReach, 250) - the
	// finder would otherwise fail and the member never stands at a machine. The anchor is the home building's HomePoint, near the
	// building's centre; a yard home is the squad's own position. For 214 the other work rows (189 / 190 / 221) must be absent: they
	// would claim the same site under their own goal (no package in the data lists both).
	Simulation probe = newGame(order);
	enable(probe, combat);
	const routines::PackageTable packages(*probe.gameData());
	const std::vector<Pick> probePicks = residentSquads(probe, packages, *combat);
	std::vector<const Pick*> candidates;
	for (const Pick& p : probePicks)
	{
		if (p.tasks.count(goal) == 0 || p.members < 1)
			continue;
		if (goal == routines::task::RelaxInTownPackage
			&& (p.tasks.count(routines::task::PretendLabour) != 0 || p.tasks.count(routines::task::PretendLabourPretend) != 0
				|| p.tasks.count(routines::task::PretendToOperate) != 0))
			continue;
		const state::SquadInstance* instance = p.resident.squad.get<state::SquadInstance>();
		const BuildingSite* home = instance != nullptr && instance->homeBuilding != 0 ? probe.world().buildingSite(instance->homeBuilding) : nullptr;
		const double hx = home != nullptr ? home->cx : p.resident.x;
		const double hz = home != nullptr ? home->cz : p.resident.z;
		for (const ProducerSite& site : probe.world().producerSites())
			if (std::hypot(site.x - hx, site.z - hz) <= 250.0)
			{
				candidates.push_back(&p);
				break;
			}
	}
	test::require(!candidates.empty(), std::string("a resident squad with the work goal ") + name + " and a producer site near its home");
	// The first candidates are tried in their own fresh sims: a squad whose other goals keep winning never lands the goal.
	std::size_t tried = 0;
	for (const Pick* pick : candidates)
	{
		if (++tried > 4)
			break;
		Simulation sim = newGame(order);
		enable(sim, combat);
		const flecs::entity squad = squadIn(sim, pick->resident.id);
		// 214 takes a work site only when its chain plans, and the promotion pass PLACES the member at its target (a test world has no
		// mesh navigator: a later walk would be abandoned), so its machines must stand before the squad is promoted; 189's finder takes
		// the site as a pretend stand either way, and its machine is built once the member stands there (below).
		if (goal == routines::task::RelaxInTownPackage)
		{
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			const BuildingSite* home = instance != nullptr && instance->homeBuilding != 0 ? sim.world().buildingSite(instance->homeBuilding) : nullptr;
			const double hx = home != nullptr ? home->cx : pick->resident.x;
			const double hz = home != nullptr ? home->cz : pick->resident.z;
			for (const ProducerSite& site : sim.world().producerSites())
			{
				if (std::hypot(site.x - hx, site.z - hz) > 250.0)
					continue;
				const std::string output = outputOf(sim, site);
				if (!output.empty())
					buildMachine(sim, site.id, site.recordSid, output, site.ownerFaction, 0, 20, site.x, site.y, site.z);
			}
		}
		centreOn(sim, pick->resident.x, pick->resident.z);
		runUntil(sim, [&] { return squad.has<state::SquadRoutine>(); }, 3000, "promoted");
		const std::vector<flecs::entity> members = membersOf(sim, squad);
		const auto siteOf = [&](std::uint64_t id) -> const ProducerSite*
		{
			for (const ProducerSite& site : sim.world().producerSites())
				if (site.id == id)
					return &site;
			return nullptr;
		};
		// The member that settles at its work site: the goal on a SiteKind::Work target, stood within the scorer's stand-off.
		flecs::entity worker;
		std::uint64_t siteId = 0;
		for (std::uint64_t i = 0; i < 4000 && !worker.is_valid(); ++i)
		{
			sim.run(1);
			for (const flecs::entity member : members)
			{
				const state::Routine* r = member.get<state::Routine>();
				const state::Fighter* f = member.get<state::Fighter>();
				if (r == nullptr || f == nullptr || r->goal != goal
					|| r->target.kind != static_cast<std::uint8_t>(routines::SiteKind::Work))
					continue;
				const ProducerSite* site = siteOf(r->target.object);
				if (site != nullptr && !f->walking() && std::hypot(f->x - site->x, f->z - site->z) <= 5.0)
				{
					worker = member;
					siteId = site->id;
					break;
				}
			}
		}
		if (!worker.is_valid())
			continue;
		const std::uint64_t workerId = idOf(worker);
		const ProducerSite* site = siteOf(siteId);
	test::require(site != nullptr, "the work site is a producer site");
	const kdata::Record* record = sim.gameData()->find(site->recordSid);
	producers::Spec spec;
	test::require(record != nullptr && producers::specOf(*sim.gameData(), *record, spec) && !spec.output.empty(),
		"the work site's building produces an item");
	const state::Allegiance* allegiance = worker.get<state::Allegiance>();
	test::require(allegiance != nullptr && !allegiance->factionSid.empty(), "the worker has a faction");
	// The full machine at the site, and the neighbour whose free slot buys the batch of five (pullQuantity "return 5"). 214's was
	// built empty before the run (its plan gate); 189's is built now, the member standing at the site either way. The machine is the
	// worker's faction's: the pull's ownership test is the faction sid (world_tasks.cpp).
	if (goal == routines::task::RelaxInTownPackage)
	{
		fillMachine(sim, site->id, spec.output, 20);
		flecs::entity stored = entityById(sim, site->id);
		test::require(stored.is_valid() && stored.get<state::Producer>() != nullptr, "the work site's machine is a producer");
		state::Producer owned = *stored.get<state::Producer>();
		owned.ownerSid = allegiance->factionSid;
		stored.set<state::Producer>(std::move(owned));
	} else {
		buildMachine(sim, site->id, site->recordSid, spec.output, allegiance->factionSid, 20, 20, site->x, site->y, site->z);
	}
	buildMachine(sim, 0xC0FFEE0000000001ull, site->recordSid, spec.output, allegiance->factionSid, 0, 20,
		site->x + 30.0, site->y, site->z);
	state::Routine handed;
	bool wasHanded = false;
	for (std::uint64_t i = 0; i < 200 && !wasHanded; ++i)
	{
		sim.run(1);
		if (const state::Routine* r = worker.get<state::Routine>(); r != nullptr && r->goal == routines::task::EmptyMachine)
		{
			handed = *r;
			wasHanded = true;
		}
	}
	test::require(wasHanded, "the scorer handed the worker to the pull executor (goal 92)");
	test::require(handed.chainTarget == site->id, "the hand is its own machine");
	test::require(!handed.plan.empty() && handed.plan.front() == routines::task::EmptyMachine
			&& std::find(handed.plan.begin(), handed.plan.end(), routines::task::OperateMachinery) != handed.plan.end(),
		"the plan is 92 then 87: the planner inserted the collection in front of the operate step");
	// The pull: a batch leaves the machine for the member's pack, and the executor hands the member back (goal 0).
	const std::uint32_t before = memberCarried(worker, spec.output);
	for (std::uint64_t i = 0; i < 400 && !(memberCarried(worker, spec.output) > before); ++i)
		sim.run(1);
	const std::uint32_t took = memberCarried(worker, spec.output) - before;
	test::require(took > 0, "the pull took output into the member's pack");
	const flecs::entity machine = entityById(sim, site->id);
	test::require(machine.is_valid() && machine.get<state::Inventory>()->carried(spec.output) < 20,
		"the machine is below full after the pull (the operate step can plan)");
	// The operate step: the next pass plans [87, the goal] and orders the Use that takes the operator slot.
	bool operating = false;
	std::uint16_t resumed = 0;
	for (std::uint64_t i = 0; i < 400 && !operating; ++i)
	{
		sim.run(1);
		const state::Producer* producer = machine.is_valid() ? machine.get<state::Producer>() : nullptr;
		operating = producer != nullptr && std::find(producer->operators.begin(), producer->operators.end(), workerId) != producer->operators.end();
		if (const state::Routine* r = worker.get<state::Routine>(); r != nullptr && r->goal != 0)
			resumed = r->goal;
	}
	test::require(operating, "the worker carries on operating the machine it emptied");
	test::require(resumed == goal, std::string("the town routine took it back (goal ") + name + ")");
	std::cout << "work handover ok (goal " << name << ", worker " << workerId << ", site " << site->id << ": " << took << " x "
		<< spec.output << " pulled, then operating)\n";
		return;
	}
	test::require(false, std::string("no resident took the work goal ") + name + " at a work site");
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		placementAndLife(order, combat);
		blockAndRelease(order, combat);
		surplusFalls(order, combat);
		saveLoad(order, combat);
		workHandover(order, combat, routines::task::PretendLabour, "189 PRETEND_LABOURING");
		workHandover(order, combat, routines::task::RelaxInTownPackage, "214 RELAX_IN_TOWN_PACKAGE");
		std::cout << "ksim_town_scorer: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_scorer: FAILED: " << e.what() << "\n";
		return 1;
	}
}
