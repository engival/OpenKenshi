// ksim_spectate_approach (kenshi-k16.6.95): approach table of the spectate_fight squads. Spectator mode (2026-11-07): `scenarios/spectate_fight.kscen`, the player-less fight. Two NPC squads near the Hub's Bar whose
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

constexpr int kMaxTicks = 20 * 120;
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

		// The approach, per game second from the unpause to the first hit: each member's step over the last second (u), whether it is
		// moving, and its squad's spread (the largest distance from the squad's centroid). The squads are told apart by where they start.
		const auto fighterOf = [&](std::uint64_t id) { return sim.combat()->find(id); };
		std::vector<int> side;
		for (std::uint64_t id : placed.others)
			side.push_back(fighterOf(id)->z < -1000.0 ? 0 : 1);
		std::uint64_t firstHit = 0;
		sim.setCombatObserver([&](const CombatEvent& event)
		{
			if (firstHit == 0 && event.kind == CombatEvent::Kind::Hit)
				firstHit = sim.clockTick();
		});
		const std::uint64_t start = sim.clockTick();
		std::vector<double> lastX(placed.others.size()), lastZ(placed.others.size()), peak(placed.others.size(), 0.0);
		for (std::size_t i = 0; i < placed.others.size(); ++i)
		{
			lastX[i] = fighterOf(placed.others[i])->x;
			lastZ[i] = fighterOf(placed.others[i])->z;
		}
		double maxSpread = 0.0;
		double slowest = 1e9; // the smallest step of a closing second (the target still over 60 u away, from the third second), as a fraction of the run speed
		std::vector<double> prevX = lastX;
		std::vector<double> prevZ = lastZ;
		for (int tick = 0; tick < kMaxTicks && firstHit == 0; ++tick)
		{
			sim.tick();
			for (std::size_t i = 0; i < placed.others.size(); ++i)
			{
				const state::Fighter* f = fighterOf(placed.others[i]);
				peak[i] = std::max(peak[i], std::hypot(f->x - lastX[i], f->z - lastZ[i]) / 0.05 / sim.combat()->runSpeed(*f));
				lastX[i] = f->x;
				lastZ[i] = f->z;
			}
			if (tick % 20 != 19)
				continue;
			const int second = (tick + 1) / 20;
			std::printf("t=%2d", second);
			for (int sd = 0; sd < 2; ++sd)
			{
				double cx = 0.0;
				double cz = 0.0;
				int n = 0;
				for (std::size_t i = 0; i < placed.others.size(); ++i)
					if (side[i] == sd)
					{
						cx += fighterOf(placed.others[i])->x;
						cz += fighterOf(placed.others[i])->z;
						++n;
					}
				double spread = 0.0;
				std::printf(" |");
				for (std::size_t i = 0; i < placed.others.size(); ++i)
				{
					const state::Fighter* f = fighterOf(placed.others[i]);
					if (side[i] != sd)
						continue;
					spread = std::max(spread, std::hypot(f->x - cx / n, f->z - cz / n));
					const double step = std::hypot(f->x - prevX[i], f->z - prevZ[i]);
					prevX[i] = f->x;
					prevZ[i] = f->z;
					std::printf(" %3.0f", step);
					const state::Fighter* t = f->target != 0 ? fighterOf(f->target) : nullptr;
					if (second >= 3 && t != nullptr && std::hypot(t->x - f->x, t->z - f->z) > 60.0)
						slowest = std::min(slowest, step / sim.combat()->runSpeed(*f));
				}
				std::printf(" spread %3.0f", spread);
				if (second >= 2)
					maxSpread = std::max(maxSpread, spread);
			}
			std::printf("\n");
		}
		test::require(firstHit != 0, "no hit within " + std::to_string(kMaxTicks / 20) + " game s");
		double lowestPeak = 1e9;
		for (const double p : peak)
			lowestPeak = std::min(lowestPeak, p);
		// "Held the line" (kenshi-k16.6.109, user 2026-11-12): for 8 game s after the first contact, per second, the members on their own side of the perpendicular
		// bisector between the two squad centroids. The original's own log (rwh.91, runs 1 and 2) has 0.50 / 0.50 and 0.5 / 0.33 of the sides at the same
		// point; ours kscen / import runs (ksim_checkpoints) 0.50-1.0, pooled 0.6. A squad that mingled at once would sit near 0.5 or below.
		int ownSide = 0;
		int sideTotal = 0;
		for (int second = 0; second < 8; ++second)
		{
			sim.run(20);
			double cx[2] = {0.0, 0.0};
			double cz[2] = {0.0, 0.0};
			int n[2] = {0, 0};
			for (std::size_t i = 0; i < placed.others.size(); ++i)
			{
				cx[side[i]] += fighterOf(placed.others[i])->x;
				cz[side[i]] += fighterOf(placed.others[i])->z;
				++n[side[i]];
			}
			for (int sd = 0; sd < 2; ++sd)
			{
				cx[sd] /= n[sd];
				cz[sd] /= n[sd];
			}
			const double midX = (cx[0] + cx[1]) / 2.0;
			const double midZ = (cz[0] + cz[1]) / 2.0;
			for (std::size_t i = 0; i < placed.others.size(); ++i)
			{
				const state::Fighter* f = fighterOf(placed.others[i]);
				const int sd = side[i];
				ownSide += (f->x - midX) * (cx[sd] - cx[1 - sd]) + (f->z - midZ) * (cz[sd] - cz[1 - sd]) > 0.0 ? 1 : 0;
				++sideTotal;
			}
		}
		const double heldShare = static_cast<double>(ownSide) / sideTotal;
		std::printf("spectate_approach: scenario %016llx; first hit at %.1f s; lowest member peak %.2f x run speed; slowest closing second %.2f x run speed; "
			"squad spread while closing at most %.0f u; %.2f of the members on their own side of the line in the 8 s after the first hit\n", static_cast<unsigned long long>(scenario::fileHash(info.path)), static_cast<double>(firstHit - start) * 0.05, lowestPeak, slowest, maxSpread, heldShare);
		// The original (rwh85, three runs): the members came in unarmed, every one of them closes on its target, and a squad closing is
		// never tighter than 44 u from its centroid there (our line of 9 u spacing starts at 13); the run speed is the top of the chase.
		test::require(lowestPeak >= 0.6, "a member never got near its run speed (" + std::to_string(lowestPeak) + " of it): it held its ground or crept");
		test::require(slowest >= 0.2, "a member nearly stood (" + std::to_string(slowest) + " x run speed in a second) while its target was over 60 u off");
		test::require(maxSpread <= 55.0, "a squad spread to " + std::to_string(maxSpread) + " u while closing: it did not move as a squad");
		test::require(heldShare >= 0.45, "the squads mingled at once: " + std::to_string(heldShare) + " of the members on their own side of the line between the centroids in the 8 s after the first hit (the original's log: 0.4-0.5)");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_spectate_approach: " << e.what() << "\n";
		return 1;
	}
}
