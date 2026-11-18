// Ground ray (kenshi-k16.7.35, rwh.130): a standing character's y is the first hit of a ray from its navmesh point + 20 straight down
// onto the building collision, not the navmesh face's y. The Rebel Base bar's four standing NPCs (rwh.130's table): the sim's y at their
// xz is within 0.05 u of the original's char y (1074.305 / 1075.694 / 1074.457 / 1072.859), where the navmesh y is 0.02..0.7 off.
// argv[1]: the world navmesh bake directory.

#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

state::Fighter* fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<CombatArena*>(sim.combat())->find(id);
}

struct Spot
{
	double x;
	double z;
	double original; // the original's char y (Character+0x48)
};
constexpr Spot kSpots[] = {
	{-50546.8398, -1211.6248, 1074.3049},
	{-50567.7539, -1267.5566, 1075.6941},
	{-50546.4258, -1189.9853, 1074.4567},
	{-50535.9023, -1264.6639, 1072.8589},
};

struct Game
{
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	explicit Game(const std::string& bake)
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, hubZones);
	}

	void newGame()
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		std::string startoff;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				startoff = record->stringID;
		sim->enqueue(Command::newGame(startoff, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}
	state::Fighter& me() { return *fighterOf(*sim, player); }
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_ground_ray: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		Game game(argv[1]);
		game.newGame();
		test::require(game.player != 0, "the new game has a player character");
		Simulation& sim = *game.sim;
		sim.run(200);
		test::require(sim.lineOfSightEnabled(), "the collision (line of sight) is on");
		for (const Spot& spot : kSpots)
		{
			{
				state::Fighter& f = game.me();
				f.x = spot.x;
				f.z = spot.z;
				f.y = sim.groundHeight(spot.x, spot.z).value_or(f.y);
				f.clearRoute();
				f.building = 0;
			}
			sim.run(20);
			const state::Fighter& f = game.me();
			// The character as the original stands it: at the NPC's own xz (our navmesh may put a character a few u off it: the first
			// barman's point is behind the counter, off our mesh), on our navmesh floor there.
			state::Fighter at = f;
			at.x = spot.x;
			at.z = spot.z;
			const double ray = sim.groundY(game.player, at);
			std::printf("ground_ray: (%.1f, %.1f) navmesh y %.3f ray y %.3f original %.3f\n", spot.x, spot.z, f.y, ray, spot.original);
			// The navmesh itself at the NPC's xz (k16.7.36): on the mesh within 0.3 u, and its y within 0.1 u of the original's (0.02..0.05
			// below the hull top), not 0.45..0.7 above it.
			knav::Point onMesh{};
			knav::NavQuery query(game.mesh->mesh());
			const bool found = query.nearestPoint({ static_cast<float>(spot.x), static_cast<float>(spot.original), static_cast<float>(spot.z) }, { 0.3f, 3.0f, 0.3f }, onMesh);
			std::printf("ground_ray: navmesh at the exact xz: %s y %.3f\n", found ? "found" : "OFF THE MESH", found ? onMesh[1] : 0.0f);
			{
				knav::PolyLoc loc;
				const bool located = query.locate({ static_cast<float>(spot.x), static_cast<float>(spot.original), static_cast<float>(spot.z) }, { 0.3f, 3.0f, 0.3f }, loc);
				std::printf("ground_ray:   located %d wall distance %.2f; walker at (%.2f, %.2f) i.e. %.2f u from the NPC\n", located ? 1 : 0,
					located ? query.wallDistance(loc, 10.0f) : -1.0f, f.x, f.z, std::hypot(f.x - spot.x, f.z - spot.z));
			}
			test::require(found, "the NPC's own xz is on the navmesh");
			test::require(std::fabs(onMesh[1] - spot.original) <= 0.1, "the navmesh y " + std::to_string(onMesh[1]) + " is within 0.1 of the original's " + std::to_string(spot.original));
			test::require(f.building != 0, "the character is inside the bar");
			test::require(std::fabs(ray - spot.original) <= 0.05, "the ray y " + std::to_string(ray) + " is within 0.05 of the original's " + std::to_string(spot.original));
		}
		{
			// The cost of one uncached ray (every call moves the character a hair, so the memo never answers).
			state::Fighter at = game.me();
			const auto start = std::chrono::steady_clock::now();
			constexpr int kRays = 20000;
			double sink = 0.0;
			for (int i = 0; i < kRays; ++i)
			{
				at.x = kSpots[0].x + 1e-3 * (i % 100);
				sink += sim.groundY(game.player, at);
			}
			const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / kRays;
			std::printf("ground_ray: %.2f us per uncached ray (%.0f)\n", us, sink > 0 ? 1.0 : 0.0);
		}
		std::cout << "ksim_ground_ray: OK\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_ground_ray: " << e.what() << "\n";
		return 1;
	}
}
