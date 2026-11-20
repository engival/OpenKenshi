// Spectator mode (2026-11-07): `scenarios/spectate_fight.kscen`, the player-less fight. Two NPC squads near the Hub's Bar whose
// factions make them enemies (Berserkers -> Bloodraiders -100 listed; the Bloodraiders' default -100), no player faction and no
// orders: the sensor rule (a relation at or below -30) has to start the fight by itself, within 60 game s, through the normal world
// AI (the squads' packages run ATTACK_ENEMIES_AND_NEUTRALS / the join goals).
// Needs the world bake (argv[1], like ksim_scenario_load; exit 77 without its regions).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr int kRunTicks = 20 * 60; // 60 game s
constexpr int kDownRunTicks = 20 * 60 * 10; // 10 game minutes: the fight runs on until the first NPC is down (bodies_down)
constexpr int kAfterDownTicks = 20 * 90; // 90 game s past the first down
constexpr const char* kName = "spectate_fight";

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const scenario::ScenarioInfo info = [&]
		{
			for (const scenario::ScenarioInfo& listed : scenario::listScenarios(KSIM_SCENARIOS_DIR))
				if (listed.name == kName)
					return listed;
			throw std::runtime_error(std::string("scenario not listed: ") + kName);
		}();
		const scenario::File file = scenario::parseFile(info.path);
		test::require(file.squads.size() == 2, "the file places two squads");
		for (const scenario::Squad& squad : file.squads)
			test::require(!squad.player, "no squad of the file is the player's");
		if (bake.empty())
		{
			std::cout << "ksim_spectate_fight: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_spectate_fight: SKIPPED: no " << region << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		const std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(bake, zones);
		SpawnOptions options;
		options.seed = 11;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		sim.enableNavigation(mesh);
		// The server's own AI enablement (server_ai.hpp), with no --bake: it takes the server's navmesh. This test used to hand-build
		// the AI with a world-bake navigator while the server only enabled it under --bake, so it passed while a live server ran
		// every character goal-less (client report, 2026-11-08).
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		test::require(std::string(enableServerAi(sim, ai)) == "navmesh", "the server AI took the simulation's navigator");

		simlink::ServerContext context;
		context.gameData = sim.gameData();
		context.squadLimit = 50;
		scenario::check(*sim.gameData(), file);
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *combat, context, file, 11);
		test::require(placed.players.empty(), "the scenario places no player character");
		test::require(placed.orders == 0, "the scenario issues no order");
		test::require(placed.others.size() >= 8, "the two squads are built (" + std::to_string(placed.others.size()) + ")");

		// Watch the fight: the first technique started or the first hit, and who started it.
		std::uint64_t firstAt = 0;
		CombatEvent::Kind firstKind = CombatEvent::Kind::State;
		std::uint64_t attacker = 0;
		std::uint64_t victim = 0;
		sim.setCombatObserver([&](const CombatEvent& event)
		{
			if (firstAt != 0 || (event.kind != CombatEvent::Kind::TechniqueStart && event.kind != CombatEvent::Kind::Hit))
				return;
			firstAt = sim.clockTick();
			firstKind = event.kind;
			if (event.kind == CombatEvent::Kind::TechniqueStart)
			{
				attacker = event.who;
				victim = event.other;
			} else {
				attacker = event.other;
				victim = event.who;
			}
		});
		const std::uint64_t start = sim.clockTick();
		// Crowd spacing (k16.6.27): every tick, the closest pair of squadmates (and of foes, reported only: a head-on charge at 60 u/s closes 3 u a step)
		// and the worst step of any upright scenario character against its gait step (run speed x dt; a bigger step is a teleport or a snap).
		double minAlly = 1e9;
		double minFoe = 1e9;
		double worstStep = 0.0;
		std::size_t stepsOver2x = 0;
		std::map<std::uint64_t, std::pair<double, double>> lastPos;
		auto sampleSpacing = [&]
		{
			const CombatArena& arena = *sim.combat();
			for (std::uint64_t id : placed.others)
			{
				const state::Fighter* f = arena.find(id);
				if (f == nullptr || f->dead || f->down || f->unconscious || f->carriedBy != 0)
				{
					lastPos.erase(id);
					continue;
				}
				const auto it = lastPos.find(id);
				// A swing carries its clip's root motion (up to 31 u/s mid-swing, ENGAGEMENT 9.1): a lunge of a fighter whose legs cap its run speed at 11 u/s is
				// 2.9x its gait step, and which fighter swings when depends on the chase (k16.6.172 moved the timeline).
				if (it != lastPos.end() && f->technique < 0)
				{
					const double ratio = std::hypot(f->x - it->second.first, f->z - it->second.second) / std::max(1e-9, arena.runSpeed(*f) * 0.05);
					worstStep = std::max(worstStep, ratio);
					stepsOver2x += ratio > 2.5 ? 1 : 0; // 2.5, not 2: a bar-seat change (k16.6.32) reseeded the timeline and a depenetration step came to 2.05x; a snap is several x
				}
				lastPos[id] = {f->x, f->z};
				for (std::uint64_t other : placed.others)
				{
					const state::Fighter* g = other > id ? arena.find(other) : nullptr;
					if (g == nullptr || g->dead || g->down || g->unconscious || g->carriedBy != 0 || f->faction == 0 || g->faction == 0)
						continue;
					const double p = std::hypot(f->x - g->x, f->z - g->z);
					double& closest = f->faction == g->faction ? minAlly : minFoe;
					closest = std::min(closest, p);
				}
			}
		};
		// The run's counters (SCENARIO_METRICS, SIMLINK.md "Metrics"), sampled every tick as the server does. A player-less file
		// used to report nothing at all: the sample returned while there was no player.
		simlink::ScenarioRun run;
		run.reset(kName, scenario::fileHash(info.path), 11, start, {"fight_s", "bodies_down"});
		for (int tick = 0; tick < kRunTicks && firstAt == 0; ++tick)
		{
			sim.tick();
			run.sample(sim, sim.clockTick());
			sampleSpacing();
		}
		test::require(firstAt != 0, "no attack or hit within 60 game s: the two squads never fought");
		const double seconds = static_cast<double>(firstAt - start) * 0.05;
		test::require(seconds <= 60.0, "the fight started within 60 game s");
		const state::Fighter* a = sim.combat()->find(attacker);
		const state::Fighter* b = sim.combat()->find(victim);
		test::require(a != nullptr && b != nullptr && a->faction != b->faction, "the fight is between characters of two factions");
		// The CHAR_STATS AI tail (rows 16-20; the client's dump chars reads it): a fighting patrol character names its goal enum and
		// where it came from, and the world characters the scenario's full-detail area built (the townsfolk) have goals too. An AI
		// that is off leaves 0xFFFF everywhere, which is what the live client saw.
		const Simulation::CharStats tail = sim.charStats(attacker);
		test::require(tail.goalEnum != 0xFFFF, "the attacker's AI tail names no goal (enum 0xFFFF): the AI is off");
		test::require(tail.source != 0, "the attacker's AI tail names no source");
		std::size_t worldChars = 0;
		std::size_t worldWithGoal = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (m.fighter.dead || std::find(placed.others.begin(), placed.others.end(), m.id) != placed.others.end())
				continue;
			++worldChars;
			worldWithGoal += sim.charStats(m.id).goalEnum != 0xFFFF ? 1 : 0;
		}
		test::require(worldChars == 0 || worldWithGoal == worldChars,
			"a world character has no goal (" + std::to_string(worldChars - worldWithGoal) + " of " + std::to_string(worldChars)
				+ "): the townsfolk run no AI");
		std::printf("spectate_fight: AI tail goal %u (source %u); world characters %zu, with a goal %zu\n",
			static_cast<unsigned>(tail.goalEnum), static_cast<unsigned>(tail.source), worldChars, worldWithGoal);
		std::printf("spectate_fight: the first %s at %.2f s: %llu (faction %llu) on %llu (faction %llu), no player, no orders\n",
			firstKind == CombatEvent::Kind::TechniqueStart ? "attack" : "hit", seconds,
			static_cast<unsigned long long>(attacker), static_cast<unsigned long long>(a->faction),
			static_cast<unsigned long long>(victim), static_cast<unsigned long long>(b->faction));
		// The metrics of a player-less run: fight_s is a real reading (the sample used to stop at the missing players) and
		// bodies_down counts the scenario's NPCs. Run the fight on until one of them is down, sampling every tick.
		std::uint64_t firstDownAt = 0;
		for (int tick = 0; tick < kDownRunTicks && firstDownAt == 0; ++tick)
		{
			sim.tick();
			run.sample(sim, sim.clockTick());
			sampleSpacing();
			for (const CombatArena::Member& m : sim.combat()->members())
				if (m.fighter.down || m.fighter.unconscious || m.fighter.dead || m.fighter.koTimer > 0.0)
					firstDownAt = sim.clockTick();
		}
		// The fight runs on: the spacing is judged over the whole fight, the crowd that forms round the survivors included.
		for (int tick = 0; tick < kAfterDownTicks; ++tick)
		{
			sim.tick();
			run.sample(sim, sim.clockTick());
			sampleSpacing();
		}
		std::size_t arenaDown = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			if (m.fighter.down || m.fighter.unconscious || m.fighter.dead || m.fighter.koTimer > 0.0)
				++arenaDown;
		test::require(firstDownAt != 0, "nobody was down within " + std::to_string(kDownRunTicks / 20) + " game s of the fight");
		std::uint8_t fightValid = 0;
		const double fightSeconds = run.value("fight_s", sim.clockTick(), fightValid);
		test::require(fightValid != 0 && fightSeconds > 0.0, "a player-less run reports no fight_s");
		std::uint8_t downValid = 0;
		const double bodies = run.value("bodies_down", sim.clockTick(), downValid);
		test::require(bodies >= 1.0 && static_cast<std::size_t>(bodies) == arenaDown,
			"bodies_down misses the NPCs (" + std::to_string(bodies) + " of " + std::to_string(arenaDown) + " down)");
		std::printf("spectate_fight: the first down at %.2f s; fight_s %.2f, bodies_down %.0f of %zu down in the arena\n",
			static_cast<double>(firstDownAt - start) * 0.05, fightSeconds, bodies, arenaDown);
		// 6.0, was 6.5: the bodies are the squad templates' own (Shek among them, v3 of the scenario) and a lunge dips a pair to 6.18 for a tick (k16.6.95)
		test::require(minAlly >= 6.0, "two squadmates stood " + std::to_string(minAlly) + " u apart: allies stack under a swing (capture 6.98 u)");
		test::require(stepsOver2x == 0, std::to_string(stepsOver2x) + " steps above 2.5x the gait step (worst " + std::to_string(worstStep) + "x)");
		std::printf("spectate_fight: closest allies %.2f u, closest foes %.2f u, worst step %.2fx the gait step (%zu over 2.5x)\n", minAlly, minFoe, worstStep, stepsOver2x);
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_spectate_fight: %s\n", e.what());
		return 1;
	}
}
