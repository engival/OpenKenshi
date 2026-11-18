// Feet on the ground, indoors (kenshi-k16.7.14): a character inside a building's footprint (Fighter::building != 0) keeps the walker's
// mesh/floor height, not the terrain's: the Hub's Bar, ground floor. Driven in four headings from the bar's point; every step stays
// inside the footprint stays well above the terrain. The Bar floor is ~15 u over the terrain, out of the 5 u
// snap anyway, so the footprint gate itself is pinned on a walker with a ground 2 u under the floor (a floor inside the snap):
// `grounded` false keeps the mesh height, true takes the ground.
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

constexpr double kBarX = -50592.0; // ksim_bar_fight's Bar point (the Hub's own bar)
constexpr double kBarZ = -1211.0;

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
			std::cout << "ksim_feet_indoors: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		Game game(argv[1]);
		game.newGame();
		test::require(game.player != 0, "the new game has a player character");
		Simulation& sim = *game.sim;
		sim.run(200);
		const kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		{
			state::Fighter& f = game.me();
			f.x = kBarX;
			f.z = kBarZ;
			f.y = sim.groundHeight(kBarX, kBarZ).value_or(f.y);
			f.clearRoute();
			f.building = 0;
		}
		sim.run(20);
		test::require(game.me().building != 0, "the player is inside the Bar's footprint (building set)");
		const double floorY = game.me().y;
		std::printf("feet_indoors: bar floor y %.3f, terrain %.3f, building %llu floor %d\n", floorY, terrain.terrainHeight(kBarX, kBarZ),
			static_cast<unsigned long long>(game.me().building), static_cast<int>(game.me().floor));
		std::size_t inside = 0;
		for (const double heading : { 0.0, 1.5708, 3.1416, 4.7124 })
		{
			sim.enqueue(Command::fighterDrive(state::Id{game.player}, 1, heading, 1));
			for (int tick = 0; tick < 40; ++tick)
			{
				sim.tick();
				const state::Fighter& f = game.me();
				if (f.building == 0)
					break;
				++inside;
				test::require(f.y - terrain.terrainHeight(f.x, f.z) > knav::Walker::kGroundSnap, "inside the Bar y " + std::to_string(f.y)
					+ " came down to the terrain");
			}
			sim.enqueue(Command::fighterDrive(state::Id{game.player}, 0, heading, 2));
			sim.run(10);
		}
		test::require(inside >= 40, "the player walked inside the Bar (" + std::to_string(inside) + " samples)");
		{
			const state::Fighter& f = game.me();
			knav::Walker walker(game.mesh->mesh());
			const double under = f.y - 2.0;
			walker.setGround([under](double, double) { return under; });
			const knav::StepResult kept = walker.step({f.x, f.y, f.z}, 0.5, 0.0, false);
			const knav::StepResult taken = walker.step({f.x, f.y, f.z}, 0.5, 0.0, true);
			test::require(kept.onMesh && taken.onMesh, "the probe steps stay on the mesh");
			test::require(kept.at.y > under + 1.0, "an indoor step (grounded false) kept the mesh height");
			test::require(std::fabs(taken.at.y - under) < 1e-9, "the same step grounded takes the ground (the probe is live)");
		}
		std::printf("feet_indoors: %zu samples inside, floor height kept\n", inside);
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_feet_indoors: %s\n", e.what());
		return 1;
	}
}
