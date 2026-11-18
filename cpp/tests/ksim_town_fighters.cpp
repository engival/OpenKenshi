// Residents as Fighters (docs/reference/TOWN_LIFE_SLICE.md phase T3b1, save v48):
//   - every spawned member carries MemberRole (Leader / Squad / Squad2 by the template list it came from);
//   - a town squad at Full detail in routine mode gets its members built into Fighters with a Routine (SquadRoutine on the squad);
//   - leaving Full detail collapses them back to the token: no Fighter, no Routine, no reservation, no entity leak, and a second
//     promotion builds the same people;
//   - a dead resident loses its Routine and its reservation but stays as a body; a squad removed from under its fighters leaves no
//     dangling arena member;
//   - save v48 mid-routine round-trips every field, the reservation book is rebuilt identically from Routine::target and
//     SquadRoutine::barTable (the lower Id wins a conflict), and load-and-continue gives the uninterrupted run's state hash.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/combat/corpses.hpp"
#include "ksim/core/command.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

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
#include <memory>
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
	// This test pins the T3b1 layer (a resident on its ring, no goal yet); the scorer is ksim_town_scorer's.
	sim.setTownScorer(false);
}

// The bar tables the staying wandering traders hold (T3d, 5.17): the only claims in the book that no member owns. The bar finder reaches the town
// circle (k16.6.254), so more traders hold one.
std::size_t traderTables(Simulation& sim)
{
	std::size_t tables = 0;
	ecsOf(sim).each([&](const state::SquadRoutine& routine) { tables += routine.barTable != 0 ? 1 : 0; });
	return tables;
}

std::size_t instanceCount(Simulation& sim)
{
	std::size_t n = 0;
	ecsOf(sim).each([&](const state::Id&) { ++n; });
	return n;
}

// The arena fighters that are not the player's (residents promoted, bodies).
std::size_t arenaSize(const Simulation& sim)
{
	return sim.combat() == nullptr ? 0 : sim.combat()->members().size();
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

// How many of these characters are in the arena.
std::size_t inArena(const Simulation& sim, const std::vector<flecs::entity>& members)
{
	return static_cast<std::size_t>(std::count_if(members.begin(), members.end(), [&](flecs::entity m) { return sim.combat()->find(idOf(m)) != nullptr; }));
}

// ---- MemberRole at spawn ----

void memberRoles(Simulation& sim)
{
	const kdata::GameData& data = *sim.gameData();
	std::size_t leaders = 0;
	std::size_t squad2 = 0;
	std::size_t checked = 0;
	ecsOf(sim).each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		const kdata::Record* record = data.find(instance.templateSid);
		if (record == nullptr)
			return;
		std::size_t leaderMembers = 0;
		std::size_t squad2Members = 0;
		for (const flecs::entity member : membersOf(sim, squad))
		{
			const state::MemberRole* role = member.get<state::MemberRole>();
			test::require(role != nullptr && role->list <= state::kRoleLeader, "every spawned member has a MemberRole");
			leaderMembers += role->list == state::kRoleLeader ? 1 : 0;
			squad2Members += role->list == state::kRoleSquad2 ? 1 : 0;
		}
		const kdata::RefCategory* leader = record->findRefs("leader");
		const kdata::RefCategory* second = record->findRefs("squad2");
		test::require(leader != nullptr || leaderMembers == 0, instance.templateSid + ": a Leader role without a `leader` list");
		test::require(second != nullptr || squad2Members == 0, instance.templateSid + ": a Squad2 role without a `squad2` list");
		leaders += leaderMembers;
		squad2 += squad2Members;
		++checked;
	});
	test::require(checked > 500 && leaders > 100 && squad2 > 0, "roles on the default world: " + std::to_string(leaders) + " leaders, " + std::to_string(squad2) + " squad2");
	std::cout << "member roles ok (" << leaders << " leaders, " << squad2 << " squad2 members over " << checked << " squads)\n";
}

// ---- promotion, demotion ----

struct Resident
{
	flecs::entity squad;
	std::uint64_t id = 0;
	double x = 0.0;
	double z = 0.0;
};

// The first town squad (by Id) with at least two buildable members whose package the scorer models and whose behaviour is home-bound.
Resident pickSquad(Simulation& sim, const routines::PackageTable& packages, const CombatData& combat)
{
	const BehaviourRegistry& registry = sim.ai()->registry();
	const kdata::GameData& data = *sim.gameData();
	std::vector<Resident> found;
	ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.homeTown.id == 0 || instance.source != state::SpawnSource::TownResident)
			return;
		const Mapping& mapping = registry.squad(instance.templateSid);
		if (mapping.behaviour != Behaviour::GoHome && mapping.behaviour != Behaviour::StayHome)
			return;
		const routines::CompiledPackage* package = packages.find(mapping.package);
		if (package == nullptr || std::strcmp(routines::routineStatus(*package), "modelled") != 0)
			return;
		found.push_back({squad, id.value, at.x, at.z});
	});
	std::sort(found.begin(), found.end(), [](const Resident& a, const Resident& b) { return a.id < b.id; });
	for (const Resident& candidate : found)
	{
		const std::vector<flecs::entity> members = membersOf(sim, candidate.squad);
		if (members.size() < 2 || members.size() > 8)
			continue;
		bool buildable = true;
		for (const flecs::entity member : members)
		{
			std::uint64_t dice = 1;
			try
			{
				combat.makeFighter(data, member.get<state::CharacterInstance>()->templateSid, "", dice, candidate.squad.get<state::SquadInstance>()->templateSid);
			} catch (const std::exception&) {
				buildable = false;
			}
		}
		if (buildable)
			return candidate;
	}
	throw test::Failure("no resident squad to test");
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

struct Person
{
	std::string race;
	std::string name;
	std::string weapon;
	double attack = 0.0;
	bool operator==(const Person&) const = default;
};

std::vector<Person> peopleOf(Simulation&, const std::vector<flecs::entity>& members)
{
	std::vector<Person> people;
	for (const flecs::entity member : members)
	{
		const state::Fighter* f = member.get<state::Fighter>();
		test::require(f != nullptr, "a promoted member has a Fighter");
		people.push_back({f->raceSid, f->name, f->weapon.sid, f->skills.attack});
	}
	return people;
}

void promoteDemote(Simulation& sim, const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const kdata::GameData& data = *sim.gameData();
	const routines::PackageTable packages(data);
	const Resident resident = pickSquad(sim, packages, *combat);
	std::vector<flecs::entity> members = membersOf(sim, resident.squad);
	const std::size_t entities = instanceCount(sim);
	test::require(arenaSize(sim) == 0 && sim.routineBook().empty(), "nothing is promoted before a player is near");

	// Nobody is near: the squad stays a token for a while.
	sim.run(120);
	test::require(!resident.squad.has<state::SquadRoutine>() && arenaSize(sim) == 0, "a squad at Squad detail stays a token");

	// A player area on the squad: it goes Full, rests at home (Idle) and its members are built.
	centreOn(sim, resident.x, resident.z);
	runUntil(sim, [&] { return resident.squad.has<state::SquadRoutine>(); }, 3000, "the squad was never promoted");
	test::require(resident.squad.get<Detail>()->level == DetailLevel::Full, "promoted at Full detail");
	test::require(resident.squad.get<state::GroupAI>()->goal == state::GoalKind::Idle, "promoted while resting at home");
	test::require(inArena(sim, members) == members.size(), "one arena fighter per member (" + std::to_string(inArena(sim, members)) + " of " + std::to_string(members.size()) + ")");
	test::require(instanceCount(sim) == entities, "promotion creates no entities (the CharacterInstance is the fighter)");
	const std::uint64_t now = sim.clockTick();
	std::vector<std::uint64_t> ids;
	for (const flecs::entity member : members)
	{
		const std::uint64_t id = idOf(member);
		ids.push_back(id);
		const state::Fighter* f = member.get<state::Fighter>();
		const state::Routine* routine = member.get<state::Routine>();
		test::require(f != nullptr && routine != nullptr && member.get<state::MemberRole>() != nullptr, "member has Fighter, Routine and MemberRole");
		test::require(sim.combat()->find(id) != nullptr && sim.combat()->find(id)->raceSid == f->raceSid, "the arena holds the same fighter");
		test::require(!f->playerControlled && f->team == 0 && !f->dead, "a resident is not the player's and not hostile to it (team 0)");
		test::require(std::hypot(f->x - resident.x, f->z - resident.z) <= routines::kResidentRing + 1.0, "stands round the squad's position");
		test::require(routine->goal == 0 && routine->target.kind == 0 && routine->blocked.empty(), "a fresh Routine has no goal yet (T3b2 writes it)");
		const double hours = gameHoursIn(now);
		test::require(routine->lastSleep <= hours && routine->lastSleep > hours - 24.0 - 1.0, "lastSleep is within the last 24 game hours");
		test::require(member.get<state::Inventory>() != nullptr, "a promoted member owns its gear");
	}
	// The arena runs in Id order and every flecs Fighter mirrors it after a tick.
	sim.run(3);
	for (const flecs::entity member : members)
		test::require(member.get<state::Fighter>()->x == sim.combat()->find(idOf(member))->x, "the Fighter component follows the arena");
	// T3b1 reserves nothing for a member (the scorer does). The only claims in the book are the tables of staying wandering traders (T3d, 5.17).
	std::size_t tables = 0;
	ecsOf(sim).each([&](const state::SquadRoutine& routine) { tables += routine.barTable != 0 ? 1 : 0; });
	test::require(sim.routineBook().size() == tables, "T3b1 reserves nothing for a member (the scorer does)");
	const std::vector<Person> first = peopleOf(sim, members);

	// Demote: the player leaves. Members go back to the token, nothing leaks.
	centreOn(sim, 500000.0, 500000.0); // far outside the world window
	runUntil(sim, [&] { return !resident.squad.has<state::SquadRoutine>(); }, 3000, "the squad was never demoted");
	for (const flecs::entity member : members)
	{
		test::require(!member.has<state::Fighter>() && !member.has<state::Routine>(), "a demoted member is a token again");
		test::require(member.has<state::MemberRole>() && member.has<state::CharacterInstance>(), "the token keeps its role and identity");
	}
	// The book then holds only the staying traders' tables (T3d), as above: the bar finder reaches the town circle (k16.6.254), so more towns have one.
	test::require(arenaSize(sim) == 0 && instanceCount(sim) == entities && sim.routineBook().size() == traderTables(sim), "demotion leaves no fighter, entity or reservation");
	test::require(sim.residentStats().demotedMembers == sim.residentStats().promotedMembers && sim.residentStats().promotedMembers >= members.size(), "every promoted member was demoted once");

	// Three cycles: the same people every time, and no growth.
	for (int cycle = 0; cycle < 3; ++cycle)
	{
		centreOn(sim, resident.x, resident.z);
		runUntil(sim, [&] { return resident.squad.has<state::SquadRoutine>(); }, 3000, "promoted again");
		const std::vector<Person> again = peopleOf(sim, members);
		for (std::size_t i = 0; i < again.size() && i < first.size(); ++i)
		{
			if (!(again[i] == first[i]))
				std::cout << "member " << i << ": " << first[i].name << " / " << first[i].weapon << " / " << first[i].attack << " -> " << again[i].name
					<< " / " << again[i].weapon << " / " << again[i].attack << "\n";
		}
		test::require(again == first, "a second promotion builds the same people");
		test::require(inArena(sim, members) == members.size(), "arena fighters per cycle");
		centreOn(sim, 500000.0, 500000.0);
		runUntil(sim, [&] { return !resident.squad.has<state::SquadRoutine>(); }, 3000, "demoted again");
		test::require(arenaSize(sim) == 0 && instanceCount(sim) == entities, "no growth after a cycle");
	}
	std::cout << "promote / demote ok (" << members.size() << " members, 4 cycles, " << sim.residentStats().promotedSquads << " promotions)\n";

	// ---- death, a removed squad ----
	centreOn(sim, resident.x, resident.z);
	runUntil(sim, [&] { return resident.squad.has<state::SquadRoutine>(); }, 3000, "promoted for the death test");
	{
		flecs::entity victim = members.front();
		state::Routine routine = *victim.get<state::Routine>();
		routine.goal = 43;
		routine.target = {static_cast<std::uint8_t>(routines::SiteKind::Bed), 777, 1};
		victim.set<state::Routine>(routine);
		test::require(sim.rebuildRoutineBook() == 0 && sim.routineBook().size() == 1 + traderTables(sim), "book rebuilt with the victim's bed");
		test::require(sim.routineBook().held(idOf(victim)).object == 777, "the bed is the victim's");
		const_cast<state::Fighter&>(*sim.combat()->find(idOf(victim))).dead = true;
		sim.run(routines::kResidentIntervalTicks + 1);
		test::require(!victim.has<state::Routine>() && sim.routineBook().size() == traderTables(sim) && victim.has<state::Fighter>() && victim.get<state::Fighter>()->dead,
			"a dead resident gives up its Routine and bed and stays as a body");
		// Leaving Full detail: the living collapse to tokens. BQ15 (b0's mapping of "corpse unloaded", ARCHITECTURE.md "Corpses"): the
		// player went to the far corner, so no player area or load range covers the body any more and the next corpse pass destroys it
		// (this used to be "the body stays in the arena": the old code had no corpse removal at all).
		centreOn(sim, 500000.0, 500000.0);
		runUntil(sim, [&] { return !resident.squad.has<state::SquadRoutine>(); }, 3000, "demoted with a body");
		sim.run(kCorpseIntervalTicks + 2);
		test::require(!victim.is_alive() && arenaSize(sim) == 0, "the unloaded body is destroyed, the living collapse");
	}
	{
		// A second squad removed from under its promoted members (a SquadLost, a despawn): the arena drops them in the same tick.
		Simulation& s = sim;
		// the first body is still there; promote the squad's survivors again is not possible (a Fighter exists), so use another squad
		const Resident other = [&]
		{
			std::vector<Resident> all;
			ecsOf(s).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
			{
				// with a member: a recruit list can roll no one (45676, 0-1 random chars)
				if (id.value != resident.id && instance.homeTown.id != 0 && instance.source == state::SpawnSource::TownResident
					&& std::hypot(at.x - resident.x, at.z - resident.z) < 400.0 && !membersOf(s, squad).empty())
					all.push_back({squad, id.value, at.x, at.z});
			});
			std::sort(all.begin(), all.end(), [](const Resident& a, const Resident& b) { return a.id < b.id; });
			return all.empty() ? Resident{} : all.front();
		}();
		if (other.id != 0)
		{
			centreOn(s, other.x, other.z);
			runUntil(s, [&] { return other.squad.has<state::SquadRoutine>() || !other.squad.is_alive(); }, 3000, "the neighbour squad never promoted (or died)");
			if (other.squad.has<state::SquadRoutine>())
			{
				const std::size_t before = arenaSize(s);
				test::require(before >= 1, "the neighbour has fighters");
				ecsOf(s).delete_with<state::MemberOf>(other.squad.id());
				other.squad.destruct();
				s.run(2);
				test::require(arenaSize(s) < before, "the arena let go of the removed squad's fighters");
				s.run(60);
				std::cout << "removed squad ok\n";
			}
		}
	}
	(void)order;
}

// ---- save v48 mid-routine ----

void saveLoad(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat)
{
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_town_fighters_test.ksav";
	Simulation straight = newGame(order);
	enable(straight, combat);
	const routines::PackageTable packages(*straight.gameData());
	const Resident resident = pickSquad(straight, packages, *combat);
	centreOn(straight, resident.x, resident.z);
	runUntil(straight, [&] { return resident.squad.has<state::SquadRoutine>(); }, 3000, "promoted before the save");
	const std::vector<flecs::entity> members = membersOf(straight, resident.squad);
	test::require(members.size() >= 2, "two members to put mid-routine");

	// Mid-routine: goals, targets, a block list, a table claim.
	state::Routine a = *members[0].get<state::Routine>();
	a.goal = 153;
	a.since = straight.clockTick() - 40;
	a.holdUntil = straight.clockTick() + 4000;
	a.target = {static_cast<std::uint8_t>(routines::SiteKind::Bed), 9001, 2};
	a.lastSleep = 12.25;
	a.blocked = {{38, {static_cast<std::uint8_t>(routines::SiteKind::GuardNode), 9001, 0}, straight.clockTick() + 300}, {153, {static_cast<std::uint8_t>(routines::SiteKind::Bed), 9001, 1}, straight.clockTick() + 100}};
	a.nextScore = straight.clockTick() + 20;
	members[0].set<state::Routine>(a);
	state::Routine b = *members[1].get<state::Routine>();
	b.goal = 214;
	b.target = {static_cast<std::uint8_t>(routines::SiteKind::Table), 9002, 0};
	members[1].set<state::Routine>(b);
	resident.squad.set<state::SquadRoutine>(state::SquadRoutine{1, 9003});
	// A conflict is resolved for the lower Id: a third member claiming the first one's bed loses it.
	if (members.size() > 2)
	{
		state::Routine c = *members[2].get<state::Routine>();
		c.target = a.target;
		members[2].set<state::Routine>(c);
		test::require(straight.rebuildRoutineBook() == 1, "a conflicting claim is the one loss");
		test::require(straight.routineBook().holder(routines::Site{routines::SiteKind::Bed, 9001, 2})->owner == idOf(members[0]), "the lower Id keeps the site");
		c.target = {};
		members[2].set<state::Routine>(c);
	}
	test::require(straight.rebuildRoutineBook() == 0, "no conflict left");
	const routines::Reservations bookBefore = straight.routineBook();
	test::require(bookBefore.size() == 3, "bed, table and the squad's bar table are claimed (" + std::to_string(bookBefore.size()) + ")");
	test::require(bookBefore.held(idOf(resident.squad)).object == 9003, "the squad's table is claimed by the squad");

	straight.run(7);
	const std::uint64_t split = straight.stateHash();
	straight.saveGame(path);
	straight.run(2 * kTicksPerGameHour / 8);

	Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
	test::require(loaded.stateHash() == split, "load gives the saved hash");
	enable(loaded, combat);
	test::require(loaded.stateHash() == split, "enabling AI and combat does not change the hash");
	// Every component, field by field.
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity twin;
		ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) { twin = id.value == idOf(members[i]) ? e : twin; });
		test::require(twin.is_valid() && twin.has<state::Routine>() && twin.has<state::MemberRole>() && twin.has<state::Fighter>(), "member survives with its components");
		const state::Routine& before = *members[i].get<state::Routine>();
		const state::Routine& after = *twin.get<state::Routine>();
		test::require(before.goal == after.goal && before.since == after.since && before.holdUntil == after.holdUntil && before.target == after.target
				&& before.lastSleep == after.lastSleep && before.blocked == after.blocked && before.nextScore == after.nextScore,
			"Routine round-trips");
		test::require(twin.get<state::MemberRole>()->list == members[i].get<state::MemberRole>()->list, "MemberRole round-trips");
	}
	{
		flecs::entity twin;
		ecsOf(loaded).each([&](flecs::entity e, const state::Id& id) { twin = id.value == resident.id ? e : twin; });
		const state::SquadRoutine* squad = twin.get<state::SquadRoutine>();
		test::require(squad != nullptr && squad->homeLocked == 1 && squad->barTable == 9003, "SquadRoutine round-trips");
	}
	// The book, rebuilt from the components.
	const routines::Reservations& bookAfter = loaded.routineBook();
	test::require(bookAfter.size() == bookBefore.size(), "the rebuilt book has the same claims");
	for (const routines::Site& site : {routines::Site{routines::SiteKind::Bed, 9001, 2}, routines::Site{routines::SiteKind::Table, 9002, 0}, routines::Site{routines::SiteKind::Table, 9003, 0}})
	{
		const routines::Claim* was = bookBefore.holder(site);
		const routines::Claim* now = bookAfter.holder(site);
		test::require(was != nullptr && now != nullptr && was->owner == now->owner && was->since == now->since, "a claim is rebuilt identically");
	}
	test::require(bookAfter.held(idOf(members[0])) == bookBefore.held(idOf(members[0])), "held() agrees");

	// Continue: the loaded run equals the uninterrupted one.
	loaded.run(2 * kTicksPerGameHour / 8);
	test::require(loaded.stateHash() == straight.stateHash(), "load and continue gives the straight run's hash");
	test::require(loaded.routineBook().size() == straight.routineBook().size(), "and the same book");
	std::remove(path.c_str());
	std::cout << "save v48 ok (" << members.size() << " members mid-routine, hash " << std::hex << split << std::dec << ")\n";
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation sim = newGame(order);
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		enable(sim, combat);
		memberRoles(sim);
		promoteDemote(sim, order, combat);
		saveLoad(order, combat);
		std::cout << "ksim_town_fighters: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_fighters: FAILED: " << e.what() << "\n";
		return 1;
	}
}
