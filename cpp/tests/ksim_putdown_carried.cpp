// 169 PUT_DOWN_CARRIED_DUDE_IF_THEY_CAN_WALK (task dt25; kinspect dt25_bodyguard_unarmed_putdown.md section 3). The goal is
// finder-less; its node hook is the can-walk gate - carrying, the carried resolved, awake (not KO), alive, on its feet (not
// down), and either a captive (the exe's char+0x320; ours slaveState) or no enemy - and when it opens, the plan's 69
// PUT_DOWN_OBJECT drops the person (our generic dropCarried). A down or KO body rides on, and so does an awake hostile who is
// no captive. The row is the package's: a Drifter squad ("Wandering from town to town" carries 169 on the Squad list).
// A bare world (Simulation::load): the carrier is hand-made, the burden is set in its arms directly, and no lift, walk or
// navigation is involved (a carry step in flight is the carry machinery's own, not this goal's).

#include "ksim/combat_data.hpp"
#include "ksim/clock.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/world.hpp"

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
// "Drifter squad": its package "Wandering from town to town" carries 169 on the Squad list.
constexpr const char* kSquadTemplate = "3950-gamedata.base";
constexpr std::uint64_t kEnemySquad = 0x7777;
constexpr std::uint64_t kEnemyFaction = 9;

std::string bodySid(const kdata::GameData& gd)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (record->name == "Escaped Servant")
			return record->stringID;
	throw std::runtime_error("no CHARACTER Escaped Servant");
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
	std::uint64_t rng = 3;
	std::string body;
	flecs::entity squad;
	std::uint64_t leader = 0;
	std::uint64_t carrier = 0;

	Rig()
		: gd(std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA)))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::load(KENSHI_DATA, 91))
	{
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		body = bodySid(*gd);
		// A player faction and a squad at Full detail with a position: the resident pass keeps such a world squad built.
		sim.createPlayerFaction(namelessSid(*gd));
		squad = state::createInstance(ecsOf(sim));
		state::SquadInstance instance;
		instance.templateSid = kSquadTemplate;
		squad.set<state::SquadInstance>(std::move(instance));
		squad.set<state::Position>(state::Position{kX, kZ});
		squad.set<Detail>(Detail{DetailLevel::Full});
		leader = spawn(kX, kZ, 0, 0);
		carrier = spawn(kX + 6.0, kZ, 0, 0);
		join(leader);
		join(carrier);
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

	// The burden in the carrier's arms (a settled carry: no state::CarryStep).
	std::uint64_t carry(state::Fighter burden)
	{
		const std::uint64_t id = sim.spawnFighter(std::move(burden)).get<state::Id>()->value;
		fighterOf(carrier).carrying = id;
		fighterOf(id).carriedBy = carrier;
		return id;
	}

	void tick(int n)
	{
		for (int i = 0; i < n; ++i)
			sim.tick();
	}
};

void dropsACaptiveWhoCanWalk()
{
	Rig rig;
	state::Fighter burden = rig.data->makeFighter(*rig.gd, rig.body, "", rig.rng);
	burden.x = kX + 6.0;
	burden.z = kZ;
	burden.y = 1647.0;
	burden.slaveState = 1;
	const std::uint64_t dude = rig.carry(std::move(burden));
	rig.tick(60); // the selector's 0.4 s pass (kSquadIntervalTicks 20)
	test::require(rig.fighterOf(rig.carrier).carrying == 0, "the carrier still holds a captive who can walk");
	test::require(rig.fighterOf(dude).carriedBy == 0, "the captive is still marked carried");
	std::cout << "  captive who walks: put down\n";
}

void keepsAKoCaptive()
{
	Rig rig;
	state::Fighter burden = rig.data->makeFighter(*rig.gd, rig.body, "", rig.rng);
	burden.x = kX + 6.0;
	burden.z = kZ;
	burden.y = 1647.0;
	burden.slaveState = 1;
	burden.unconscious = true; // KO'd: not a walker (the gate's not-KO test, dt25 section 3.2)
	burden.koTimer = 600.0;
	const std::uint64_t dude = rig.carry(std::move(burden));
	rig.tick(60);
	test::require(rig.fighterOf(rig.carrier).carrying == dude, "the carrier put down a captive who cannot walk");
	test::require(rig.fighterOf(dude).carriedBy == rig.carrier, "the KO'd captive left the carrier's arms");
	std::cout << "  KO'd captive: kept on the shoulder\n";
}

void keepsAnAwakeHostile()
{
	Rig rig;
	state::Fighter burden = rig.data->makeFighter(*rig.gd, rig.body, "", rig.rng);
	burden.x = kX + 6.0;
	burden.z = kZ;
	burden.y = 1647.0;
	burden.faction = kEnemyFaction;
	burden.squad = kEnemySquad;
	rig.fighterOf(rig.carrier).hostileSquads = {kEnemySquad}; // the carrier's personal tag makes the pair enemies
	const std::uint64_t dude = rig.carry(std::move(burden));
	rig.tick(60);
	test::require(rig.fighterOf(rig.carrier).carrying == dude, "the carrier put down an awake hostile who is no captive");
	std::cout << "  awake hostile, no captive: kept\n";
}

} // namespace

int main()
{
	try
	{
		dropsACaptiveWhoCanWalk();
		keepsAKoCaptive();
		keepsAnAwakeHostile();
		std::cout << "ksim_putdown_carried: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_putdown_carried: FAILED: " << e.what() << "\n";
		return 1;
	}
}
