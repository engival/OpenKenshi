// The playtest metrics answer the run, not the load (2026-11-07; SIMLINK.md "SCENARIO", docs/reference/PLAYTEST.md 3): a scenario
// loaded and left alone is a world standing still - nobody is fighting anybody, so `targeted_player` is 0 and `fight_s` /
// `crew_join_s` are not valid. Then the player attacks a bandit (the provoke) and the same counters answer: someone targets the
// player and the fight has a duration.
//
// bar_lure is the case the client's live run caught (server 27bc19c5, --scenarios --no-world newgame): 11 of 11 bandits counted as
// targeting the player within 5 s of SCENARIO_LOAD, with no order given. It was not the sampler: the --no-world world has no faction
// instances, so scenarioSquad gave the file's patrol no faction, and worldTeam reads a factionless squad as hostile to everyone
// (kWildTeam) - the patrol charged. scenarioSquad now makes the instance of a faction the file names (as createPlayerFaction does
// for the player), so the patrol is a Dust Bandit squad whose record reads -10 toward the player: neutral until attacked.
// bar_lure_hostile is the other side: Blood Raiders, always hostile, the fight starts on sight, and the metric must say so.
// Needs the world bake (argv[1], like ksim_scenario_load; exit 77 without the Bar's region).
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/scenario_run.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

namespace {

using namespace ksim;

constexpr int kIdleTicks = 200; // 10 game s: the client's "load, give no order, wait ~5 s" with room to spare
constexpr int kFightTicks = 1200;

struct BuiltWorld
{
	std::unique_ptr<Simulation> sim;
	std::shared_ptr<const CombatData> combat;
};

// The server's --no-world world (ksim_server: Simulation::load, no default world), combat, navigation and the AI on it.
BuiltWorld openWorld(const std::string& bake, std::shared_ptr<MeshNavigator>& mesh)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({ simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz });
	}
	mesh = test::openWorldNav(bake, zones);
	const std::shared_ptr<const kdata::GameData> data = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
	std::unique_ptr<Simulation> sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 7));
	sim->setGameData(data);
	sim->enableWeather(*data);
	sim->markPristine(); // SCENARIO_LOAD returns to this world (Simulation::newWorld)
	std::shared_ptr<const CombatData> combat = CombatData::build(*data, KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	sim->enableCombat(combat, settings);
	sim->enableNavigation(mesh);
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim->gameData());
	sim->enableAi(std::move(ai));
	return { std::move(sim), std::move(combat) };
}

// The factions of the loaded world and their view of the player (the relation behind worldTeam and enemy()).
void dumpFactions(const Simulation& sim)
{
	const flecs::world& ecs = sim.world().ecs();
	flecs::entity player{};
	ecs.each([&](flecs::entity e, const state::PlayerFaction&) { player = e; });
	if (!player.is_valid())
	{
		std::printf("  no player faction\n");
		return;
	}
	ecs.each([&](flecs::entity e, const state::FactionState& faction)
	{
		const kdata::Record* record = sim.gameData()->find(faction.templateSid);
		std::printf("  faction %-16s %-20s toward the player %6.1f, hostile %d\n",
			record != nullptr ? record->name.c_str() : "?", faction.templateSid.c_str(),
			factionRelation(*sim.gameData(), e, player), factionsHostile(*sim.gameData(), e, player) ? 1 : 0);
	});
}

void report(const Simulation& sim, const simlink::ScenarioRun& run, const std::vector<std::uint64_t>& players, const char* when)
{
	std::printf("== %s\n", when);
	for (const CombatArena::Member& member : sim.combat()->members())
	{
		const state::Fighter& f = member.fighter;
		const bool player = std::find(players.begin(), players.end(), member.id) != players.end();
		const bool targetsPlayer = f.target != 0 && std::find(players.begin(), players.end(), f.target) != players.end();
		std::printf("  %6llu %-6s target %6llu%s goal %3u engaged %zu at (%.0f, %.0f)%s\n",
			static_cast<unsigned long long>(member.id), player ? "PLAYER" : "npc",
			static_cast<unsigned long long>(f.target), targetsPlayer ? " (a player)" : "", f.combatGoal, f.engaged.size(), f.x, f.z,
			f.dead ? " DEAD" : "");
	}
	for (const std::string& metric : run.metrics)
	{
		std::uint8_t valid = 0;
		const double value = run.value(metric, sim.clockTick(), valid);
		std::printf("  metric %-16s %8.3f%s\n", metric.c_str(), value, valid == 0 ? " (not reached)" : "");
	}
}

double metric(const Simulation& sim, const simlink::ScenarioRun& run, const char* name, std::uint8_t& valid)
{
	return run.value(name, sim.clockTick(), valid);
}

// SCENARIO_LOAD: back to the pristine world, the file on it (simlink_playtest.cpp runScenarioLoad), then the idle wait.
void load(Simulation& sim, const CombatData& combat, const scenario::ScenarioInfo& info, const scenario::File& file,
	simlink::ScenarioRun& run, scenario::Placed& placed)
{
	simlink::ServerContext context;
	context.gameData = sim.gameData();
	context.squadLimit = 50;
	sim.newWorld();
	sim.run(1);
	scenario::check(*sim.gameData(), file);
	placed = scenario::apply(sim, *sim.gameData(), combat, context, file, 100);
	test::require(!placed.players.empty(), std::string("no player character placed for ") + info.name);
	run.reset(info.name, info.fileHash, 100, sim.clockTick(), info.metrics);
}

const scenario::ScenarioInfo& infoOf(const std::vector<scenario::ScenarioInfo>& infos, const char* name)
{
	const auto it = std::find_if(infos.begin(), infos.end(), [&](const scenario::ScenarioInfo& info) { return info.name == name; });
	test::require(it != infos.end(), std::string("no scenario ") + name);
	return *it;
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // a failed check's line must not sit in the buffer when require() aborts
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const std::vector<scenario::ScenarioInfo> infos = scenario::listScenarios(KSIM_SCENARIOS_DIR);
		const scenario::ScenarioInfo& lure = infoOf(infos, "bar_lure");
		const scenario::ScenarioInfo& hostile = infoOf(infos, "bar_lure_hostile");
		const scenario::File lureFile = scenario::parseFile(lure.path);
		const scenario::File hostileFile = scenario::parseFile(hostile.path);
		if (bake.empty())
		{
			std::cout << "ksim_playtest_metrics: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(lureFile.centreX, lureFile.centreZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_playtest_metrics: SKIPPED: no " << region << "\n";
			return 77;
		}

		std::shared_ptr<MeshNavigator> mesh;
		BuiltWorld world = openWorld(bake, mesh);
		const std::unique_ptr<Simulation>& sim = world.sim;

		// bar_lure: neutral on load, a fight only once the player provokes it.
		simlink::ScenarioRun run;
		scenario::Placed placed;
		load(*sim, *world.combat, lure, lureFile, run, placed);
		for (int tick = 0; tick < kIdleTicks; ++tick)
		{
			sim->tick();
			run.sample(*sim, sim->clockTick());
		}
		dumpFactions(*sim);
		report(*sim, run, placed.players, "bar_lure after 10 s idle");
		std::uint8_t valid = 0;
		test::require(metric(*sim, run, "targeted_player", valid) == 0.0, "bar_lure idle: nobody targets the player");
		(void)metric(*sim, run, "fight_s", valid);
		test::require(valid == 0, "bar_lure idle: fight_s is not reached");
		(void)metric(*sim, run, "crew_join_s", valid);
		test::require(valid == 0, "bar_lure idle: crew_join_s is not reached");
		const double idleTargeted = metric(*sim, run, "targeted_player", valid);

		// The provoke: the player attacks the nearest bandit.
		const state::Fighter& me = *sim->combat()->find(placed.players.front());
		std::uint64_t nearest = 0;
		double nearestD = 0.0;
		for (const CombatArena::Member& member : sim->combat()->members())
		{
			if (member.fighter.playerControlled || member.fighter.dead)
				continue;
			const double d = std::hypot(member.fighter.x - me.x, member.fighter.z - me.z);
			if (nearest == 0 || d < nearestD)
			{
				nearest = member.id;
				nearestD = d;
			}
		}
		test::require(nearest != 0, "bar_lure places a bandit");
		sim->enqueue(Command::fighterAttack(state::Id{placed.players.front()}, state::Id{nearest}));
		for (int tick = 0; tick < kFightTicks; ++tick)
		{
			sim->tick();
			run.sample(*sim, sim->clockTick());
		}
		report(*sim, run, placed.players, "bar_lure after the provoke");
		const double provokedTargeted = metric(*sim, run, "targeted_player", valid);
		const double fight = metric(*sim, run, "fight_s", valid);
		const std::uint8_t fightValid = valid;
		test::require(provokedTargeted > 0.0, "bar_lure provoked: somebody targets the player");
		test::require(fightValid == 1 && fight >= 0.0, "bar_lure provoked: fight_s is reached");

		// bar_lure_hostile: Blood Raiders, always hostile (the file's own words): the fight starts on sight.
		load(*sim, *world.combat, hostile, hostileFile, run, placed);
		for (int tick = 0; tick < kIdleTicks; ++tick)
		{
			sim->tick();
			run.sample(*sim, sim->clockTick());
		}
		report(*sim, run, placed.players, "bar_lure_hostile after 10 s idle");
		const double hostileTargeted = metric(*sim, run, "targeted_player", valid);
		test::require(hostileTargeted > 0.0, "bar_lure_hostile: an always-hostile patrol targets the player on sight");

		std::printf("playtest metrics: bar_lure idle targeted_player %.0f (fight_s, crew_join_s not reached), provoked %.0f "
			"(fight_s %.2f); bar_lure_hostile idle %.0f\n",
			idleTargeted, provokedTargeted, fight, hostileTargeted);
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_playtest_metrics: %s\n", e.what());
		return 1;
	}
}
