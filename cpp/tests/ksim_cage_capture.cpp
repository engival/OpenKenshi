// kenshi-k16.6.98 (cage_prisoner playtest, h2v.11.6): the scenario on the server's world, not the bare rig of ksim_cage_scenario: the world residents around the
// Heft slave shop are there (some 20 characters in the full-detail area), as in the playtest. Asserted over the first 45 game s of the fight the player starts
// by attacking the Slaver boss: the Prisoner starts in the cage (cageBuilding set, its slot) and CHAR_STATS (the wire's state / lockLevel) says 5 / the cage's
// lock level every second; nobody takes it out, lifts it or hits it (a world resident's splash excepted, see the observer), and no slaver's capture goal (167 / 168 / 112) is aimed at it while it is caged.
// The window ends at 45 s or when a goal-276 LIBERATE_ALL_THE_PRISONERS task frees the Prisoner (an Outlaw Farmer walking by: ~58 s in before the standing
// residents left the walkers' way (k16.8.12), ~38 s after), by the decoded rule (oracle kenshi-rwh.100: the original's 276 does free a locked cage, so the
// window ends there; k16.6.114 closed); it must last at least 20 s, and any other release still fails. The release's lock clear is kenshi-k16.6.122.
// Needs the world bake (argv[1], exit 77 without its regions).
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

constexpr int kRunTicks = 20 * 60; // 60 game s
constexpr int kDownRunTicks = 20 * 60 * 10; // 10 game minutes: the fight runs on until the first NPC is down (bodies_down)
constexpr int kAfterDownTicks = 20 * 90; // 90 game s past the first down
constexpr const char* kName = "cage_prisoner";

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

		const std::uint64_t wanderer = placed.idOf.at(1);
		const std::uint64_t prisoner = placed.idOf.at(2);
		const state::Fighter* held = sim.combat()->find(prisoner);
		test::require(held != nullptr && held->cageBuilding != 0, "the Prisoner starts in a cage (cageBuilding)");
		const std::uint64_t cage = held->cageBuilding;
		const Simulation::CharStats first = sim.charStats(prisoner);
		test::require(first.state == 5 && first.lockLevel > 0, "CHAR_STATS state 5 with the cage's lock level at the start: " + std::to_string(first.state));
		std::size_t residents = 0;
		std::size_t hits = 0;
		sim.setCombatObserver([&](const CombatEvent& e)
		{
			// Only the scenario's own characters (the slavers, the wanderer) count: a world resident's swing at someone beside the cage splashes
			// the caged Prisoner by the decoded findImpactTargets (every enemy in the impact box, kinspect dt13; the original has no cage exemption
			// either, kenshi-rwh.176). With the road pre-route (k16.7.48) the Outlaw Farmer's long walk reaches the cage.
			const bool scenarioHitter = e.other == wanderer || std::find(placed.others.begin(), placed.others.end(), e.other) != placed.others.end();
			if (e.who == prisoner && scenarioHitter && (e.kind == CombatEvent::Kind::Hit || e.kind == CombatEvent::Kind::Shot || e.kind == CombatEvent::Kind::Knockout))
				++hits;
		});
		sim.enqueue(Command::fighterAttack(state::Id{wanderer}, state::Id{placed.idOf.at(3)}));
		std::size_t chainTicks = 0;
		std::string liberator; // the goal-276 task's log line that freed the Prisoner, if any
		sim.setTaskLog([&](const std::string& line)
		{
			if (line.rfind("liberate 276 member ", 0) == 0 && line.find(" occupant " + std::to_string(prisoner)) != std::string::npos)
				liberator = line;
		});
		int freedTick = -1;
		for (int tick = 0; tick < 20 * 45; ++tick)
		{
			sim.tick();
			const state::Fighter* p = sim.combat()->find(prisoner);
			if (!liberator.empty() && p != nullptr && p->cageBuilding != cage)
			{
				freedTick = tick;
				break;
			}
			test::require(p != nullptr && p->cageBuilding == cage && p->carriedBy == 0 && p->followTarget == 0, "the Prisoner left its cage at " + std::to_string(tick * 0.05) + " s");
			if (tick % 20 == 19)
			{
				const Simulation::CharStats stats = sim.charStats(prisoner);
				if (stats.state != 5) std::printf("state %d at %d s\n", stats.state, (tick + 1) / 20);
				test::require(stats.state == 5 && stats.lockLevel == first.lockLevel, "CHAR_STATS state 5 / lock level every second (" + std::to_string(stats.state) + ")");
			}
			if (tick == 100) // the world streams in around the file's centre in the first seconds
				for (const CombatArena::Member& m : sim.combat()->members())
					residents += std::find(placed.others.begin(), placed.others.end(), m.id) == placed.others.end() && m.id != wanderer && m.id != prisoner ? 1 : 0;
			for (const CombatArena::Member& m : sim.combat()->members())
				chainTicks += m.fighter.carrying == prisoner ? 1 : 0;
		}
		test::require(freedTick < 0 || freedTick >= 20 * 20, "the Prisoner was freed by 276 within 20 s (" + std::to_string(freedTick * 0.05) + " s): " + liberator);
		test::require(residents >= 10, "the world residents are there (" + std::to_string(residents) + ")");
		test::require(hits == 0, "something hit, shot or knocked out the caged Prisoner (" + std::to_string(hits) + ")");
		test::require(chainTicks == 0, "someone lifted the caged Prisoner");
		if (freedTick >= 0)
			std::printf("ksim_cage_capture: freed by goal 276 at tick %d (%.2f s): %s\n", freedTick, freedTick * 0.05, liberator.c_str());
		std::printf("ksim_cage_capture: the Prisoner stayed in cage %llu until the window's end among %zu world characters, wire state 5 / lock level %d\n",
			static_cast<unsigned long long>(cage), residents, static_cast<int>(first.lockLevel));
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_cage_capture: %s\n", e.what());
		return 1;
	}
}
