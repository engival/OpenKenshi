// 45 BODYGUARD (task dt25; kinspect dt25_bodyguard_unarmed_putdown.md section 1). Two halves, both finder-less:
//  - the protected hand (the original's AITaskSystem+0x10): a squad member whose package carries the goal holds the platoon
//    leader's hand while a hostile is within 40 u of the charge or of it - the goal is offered only while broken (pre 16 / 39
//    hold in a calm moment, so it is dropped then and the follow wins) - and lets go in the calm;
//  - the combat picker's +10 (dt25 section 1.5): a candidate that is fighting the charge outscores a nearer one, while fewer
//    than two attack it and it is not the charge's own target.
// The player's 45 job (the order, its 31 Staying close companion and the defense plan) is ksim_jobs'.
// A bare world (Simulation::load): the squad is hand-made (a SquadInstance of a real template whose package carries the row),
// the hostile is spawned and its sight record written by hand, so no sensing or navigation is needed.

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// Open ground outside the Hub's buildings (the animal-feed fixture's spot).
constexpr double kX = -50978.8;
constexpr double kZ = 2932.48;
// "Cannibal grand wizard": its package "Cannibals at home-wizard" carries 45 BODYGUARD (and 42) on the Squad list.
constexpr const char* kGuardTemplate = "50521-rebirth.mod";
// The hostile squad key the personal type-4 tag writes (dt11 step 2): what makes the spawned pair enemies without factions.
constexpr std::uint64_t kEnemySquad = 0x7777;
constexpr std::uint64_t kEnemyFaction = 9;

std::string characterSid(const kdata::GameData& gd, std::string_view name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (record->name == name)
			return record->stringID;
	throw std::runtime_error("no CHARACTER " + std::string(name));
}

std::string namelessSid(const kdata::GameData& gd)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("FACTION")))
		if (record->name == "Nameless")
			return record->stringID;
	throw std::runtime_error("no FACTION Nameless");
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	Simulation sim;
	std::uint64_t rng = 5;
	std::string body;
	flecs::entity squad; // a package squad (withSquad), else none
	std::uint64_t leader = 0;
	std::uint64_t guard = 0;

	explicit Rig(bool withSquad)
		: gd(std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA)))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::load(KENSHI_DATA, 91))
	{
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		body = characterSid(*gd, "Escaped Servant");
		const kdata::Record* tmpl = gd->find(kGuardTemplate);
		test::require(tmpl != nullptr, std::string("the guard template exists: ") + kGuardTemplate);
		test::require(tmpl->findRefs("AI packages") != nullptr, std::string("the guard template lists AI packages: ") + kGuardTemplate);
		// A player faction and a squad at Full detail with a position: the resident pass keeps such a world squad built
		// (residents.cpp stepResidents: without a player to relate to, a built squad is demoted and its members culled).
		sim.createPlayerFaction(namelessSid(*gd));
		if (withSquad)
		{
			squad = state::createInstance(ecsOf(sim));
			state::SquadInstance instance;
			instance.templateSid = kGuardTemplate;
			squad.set<state::SquadInstance>(std::move(instance));
			squad.set<state::Position>(state::Position{kX, kZ});
			squad.set<Detail>(Detail{DetailLevel::Full});
		}
		leader = spawn(kX, kZ, 0, 0);
		guard = spawn(kX + 15.0, kZ, 0, 0);
		if (withSquad)
		{
			join(leader);
			join(guard);
		}
	}

	std::uint64_t spawn(double x, double z, std::uint64_t team, std::uint64_t faction)
	{
		state::Fighter f = data->makeFighter(*gd, body, "", rng);
		f.x = x;
		f.z = z;
		f.y = 1647.0;
		f.team = team;
		f.faction = faction;
		f.hunger = 3.0;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& key)
		{
			if (key.value == id)
				found = e;
		});
		return found;
	}
	state::Fighter& fighterOf(std::uint64_t id) { return *const_cast<CombatArena*>(sim.combat())->find(id); }

	void join(std::uint64_t id)
	{
		flecs::entity entity = entityOf(id);
		entity.add<state::MemberOf>(squad);
		entity.set<state::MemberRole>(state::MemberRole{});
		fighterOf(id).squad = squad.get<state::Id>()->value;
	}

	// The guard knows the target (a scan would have written this record).
	void see(std::uint64_t who, std::uint64_t target)
	{
		state::SightRecord record;
		record.target = target;
		record.seen = true;
		record.sinceSeen = 0.0;
		record.sinceSensed = 0.0;
		fighterOf(who).sight.clear();
		fighterOf(who).sight.push_back(record);
	}

	void tick(int n)
	{
		for (int i = 0; i < n; ++i)
			sim.tick();
	}
};

// ---- the hand: set while the charge or the guard is under threat, dropped in the calm ----

void handSetAndCleared()
{
	Rig rig(true);
	const std::uint64_t leader = rig.leader;
	const std::uint64_t guard = rig.guard;
	const std::uint64_t enemy = rig.spawn(kX + 6.0, kZ + 4.0, 1, kEnemyFaction);
	rig.fighterOf(enemy).squad = kEnemySquad;
	rig.see(guard, enemy);
	rig.fighterOf(guard).hostileSquads = {kEnemySquad};
	test::require(rig.fighterOf(guard).protectTarget == 0 && rig.fighterOf(leader).protectTarget == 0, "a hand is set in the calm");
	rig.tick(60); // the selector's 0.4 s pass (kSquadIntervalTicks 20)
	// The squad's follow leader keeps out of the selector, so the other member is the guard: it holds the leader's hand.
	const bool holds = rig.fighterOf(guard).protectTarget == leader || rig.fighterOf(leader).protectTarget == guard;
	test::require(holds, "no member holds the other's hand under the threat");
	// The hostile walks off: the goal's state is satisfied, so the hand drops and the follow wins the calm moment.
	rig.fighterOf(enemy).x = kX + 600.0;
	rig.tick(60);
	test::require(rig.fighterOf(guard).protectTarget == 0 && rig.fighterOf(leader).protectTarget == 0,
		"the hand stays after the threat is gone");
	std::cout << "  hand: set under the threat, cleared in the calm\n";
}

// ---- no squad-level fight guard (kenshi-k16.6.152, rwh.102 1): a built world squad's member with a target fights; a squadmate with no
// record of its own keeps taking its package goal ----

void squadmateKeepsPackage()
{
	Rig rig(true);
	const std::uint64_t fighter = rig.guard;
	const std::uint64_t idle = rig.spawn(kX - 15.0, kZ, 0, 0);
	rig.join(idle);
	// The enemy is far from everyone and the calm member has no sight record: only `fighter` holds a target.
	const std::uint64_t enemy = rig.spawn(kX + 300.0, kZ, 1, kEnemyFaction);
	rig.fighterOf(enemy).squad = kEnemySquad;
	bool took = false;
	for (int i = 0; i < 120 && !took; ++i)
	{
		rig.fighterOf(fighter).target = enemy; // its own committed target, held for the whole window
		rig.tick(1);
		took = rig.entityOf(idle).get<state::Routine>() != nullptr || rig.fighterOf(idle).goal.kind != state::IndividualKind::None;
	}
	test::require(took, "a squadmate with no target record sat idle while another member fought (the squad-level fight guard)");
	test::require(rig.entityOf(fighter).get<state::Routine>() == nullptr, "the member with a target was given a package goal");
	std::cout << "  guard: the squadmate with no record took its package goal while a member fought\n";
}

} // namespace

int main()
{
	try
	{
		handSetAndCleared();
		squadmateKeepsPackage();
		std::cout << "ksim_bodyguard: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_bodyguard: FAILED: " << e.what() << "\n";
		return 1;
	}
}
