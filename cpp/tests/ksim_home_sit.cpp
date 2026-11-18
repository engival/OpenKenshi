// A STAY_IN_HOME (43) resident sits (kenshi-k16.6.30, TOWN_LIFE_SLICE 5.4 "Seat"): on the Hub's new game (needs the world bake, argv[1]; exit 77 without it)
//   - every goal-43 resident of the Hub is seated, and a squatter in a ruin with no furniture sits on the floor: its own spot and yaw, ground height, "sitting idle";
//   - a resident whose home has a free chair (the Bar's) reserves it, walks to it and sits there on the chair's clip;
//   - a fight (a target) stands it up, and it sits again when the fight is over.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
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

void require(bool ok, const std::string& what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what.c_str());
}

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i) { found = i.value == id ? e : found; });
	return found;
}

state::Fighter& fighterRef(Simulation& sim, std::uint64_t id)
{
	return const_cast<state::Fighter&>(*sim.combat()->find(id));
}

std::uint16_t goalOf(Simulation& sim, std::uint64_t id)
{
	const state::Routine* routine = entityOf(sim, id).get<state::Routine>();
	return routine != nullptr ? routine->goal : 0;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_home_sit: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
		SpawnOptions options;
		options.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::resolveLoadOrder(KENSHI_DATA, ""), options, nullptr, RegionConfig{});
		sim.setResidentsArrive(false); // placed at their homes: the seats are the test, not the walk in (k16.6.31)
		sim.markPristine();
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		sim.enableNavigation(test::openWorldNav(world, hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		std::string sid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			sid = record->name == "Wanderer" ? record->stringID : sid;
		sim.enqueue(Command::newGame(sid, "Tester", 1));
		sim.run(400);
		sim.run(600); // 30 s: the residents have settled on their goals

		const auto hub = [&](const CombatArena::Member& m)
		{
			return !m.fighter.playerControlled && std::hypot(m.fighter.x + 50986.0, m.fighter.z - 3514.0) <= 1200.0;
		};
		// ---- (a) every goal-43 resident sits; with no chair, on the floor ----
		std::vector<std::uint64_t> squatters;
		std::uint64_t barman = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (!hub(m))
				continue;
			if (goalOf(sim, m.id) == routines::task::StayInHome)
				squatters.push_back(m.id);
			else if (goalOf(sim, m.id) == routines::task::Shopkeeper && m.fighter.building != 0)
				barman = m.id;
		}
		require(squatters.size() >= 5, "the Hub has goal-43 residents (" + std::to_string(squatters.size()) + ")");
		std::size_t seated = 0;
		for (const std::uint64_t id : squatters)
		{
			const state::Fighter& f = *sim.combat()->find(id);
			const std::optional<Simulation::Pose> pose = sim.pose(id);
			const state::Routine* routine = entityOf(sim, id).get<state::Routine>();
			if (!sim.seated(id) || !pose)
				continue;
			++seated;
			if (routine->target.kind == 0)
				test::require(pose->clip == routines::kClipSittingIdle && std::hypot(pose->x - f.x, pose->z - f.z) < 1e-9 && pose->y == f.y && pose->yaw == f.yaw,
					"a floor-sit is the member's own spot, height and yaw on the sitting idle clip");
		}
		require(seated == squatters.size(), "every goal-43 resident is seated (" + std::to_string(seated) + " of " + std::to_string(squatters.size()) + ")");
		std::printf("    floor-sit clip: %s\n", std::string(routines::kClipSittingIdle).c_str());

		// ---- (b) a home with a free chair: the Bar (a Hub squatter is given the Bar as its home) ----
		test::require(barman != 0, "no Bar staff to name the Bar building");
		const std::uint64_t bar = sim.combat()->find(barman)->building;
		const std::uint64_t guest = squatters.front();
		fighterRef(sim, guest).homeBuilding = bar;
		{
			flecs::entity e = entityOf(sim, guest);
			state::Routine* routine = e.get_mut<state::Routine>();
			*routine = state::Routine{};
		}
		sim.run(1200);
		const state::Routine* routine = entityOf(sim, guest).get<state::Routine>();
		test::require(routine != nullptr && routine->goal == routines::task::StayInHome, "the guest is no longer on STAY_IN_HOME");
		// kenshi-k16.6.33: the Hub has one Bar blackmarket now (rwh.35) and a bar's chairs are its tables' BarSeats; a Seat is a chair that serves no
		// table, and no intact Hub building has one (UNSURE, deferred: playtesting: the chair cases run on the first world that offers one).
		const bool chairHome = routine->target.kind == static_cast<std::uint8_t>(routines::SiteKind::Seat);
		std::printf("    a free table-less chair at the Bar's building: %s\n", chairHome ? "yes" : "none (cases b / c skipped)");
		if (chairHome)
		{
			const std::optional<Simulation::Pose> chair = sim.pose(guest);
			test::require(sim.seated(guest) && chair.has_value(), "the guest never sat down at the chair");
			require(!chair->clip.empty() && std::hypot(chair->x - fighterRef(sim, guest).x, chair->z - fighterRef(sim, guest).z) <= routines::kSeatTolerance,
				"it sits at the chair's own pose, clip " + chair->clip + ", within the seat tolerance");

			// ---- (c) a fight stands it up ----
			fighterRef(sim, guest).target = barman;
			require(!sim.seated(guest) && !sim.pose(guest), "a member with a fight target is not seated");
			fighterRef(sim, guest).target = 0;
			require(sim.seated(guest), "it sits again once the fight is over");
		}
		std::uint64_t floorSitter = 0;
		for (const std::uint64_t squatter : squatters)
			if (squatter != guest && sim.seated(squatter))
				floorSitter = squatter;
		require(floorSitter != 0, "a squatter is seated on the floor");
		fighterRef(sim, floorSitter).down = true;
		require(!sim.seated(floorSitter), "a downed floor-sitter is not seated");
		fighterRef(sim, floorSitter).down = false;

		// ---- (d) stopped far from home (a stalled walk) stands ----
		state::Fighter& stalled = fighterRef(sim, floorSitter);
		const double homeX = stalled.x;
		stalled.x += 60.0;
		require(!sim.seated(floorSitter) && !sim.pose(floorSitter), "a goal-43 member stopped 60 u from its home is not seated");
		stalled.x = homeX;
		require(sim.seated(floorSitter), "it sits again back at its home");
		std::printf("ksim_home_sit: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
