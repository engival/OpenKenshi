// A building the zone files do not carry stands in the world with collision and nav (kenshi-k16.7.125, finding k16.7.122): a Storm House (record
// 4028) at the pose the original's Desert Paradise Longhouse has (rwh.270), placed through Simulation::standBuilding, the path the save import and
// the construction commit share. Before: the runtime mesh there is terrain running through the building. After: the placement is in the world's
// placements, the doors and the building site exist, the mesh holds a walkable floor inside, and a route from the street reaches it.
// Runs against the install (KENSHI_DATA); exit 77 without the navtiles.

#include "ksim/core/simulation.hpp"
#include "ksim/core/command.hpp"
#include "ksim/save/save.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "knav/native.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_stand_building: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, {});
		SpawnOptions spawn;
		spawn.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const auto gameData = sim.gameData();
		sim.setGameData(gameData);
		kworld::PlacedInstance placed;
		placed.instanceID = "save/longhouse";
		placed.buildingID = "4028-TwoStorey.mod";
		placed.x = 29990.818;
		placed.y = 1016.181;
		placed.z = -83488.695;
		placed.qw = 0.5578;
		placed.qx = -0.0169;
		placed.qy = 0.8295;
		placed.qz = -0.0214;
		const kworld::ZoneCell zone = kworld::zoneCellOf(placed.x, placed.z);
		sim.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, {zone}, {}, gameData.get(), NativeCacheOptions{}));
		const MeshNavigator& nav = *sim.navigation();
		const auto heights = [&]()
		{
			std::map<int, int> byHeight;
			for (double dx = -150; dx <= 150; dx += 10)
				for (double dz = -150; dz <= 150; dz += 10)
					for (double above = -20; above <= 160; above += 20)
						if (const auto spot = nav.walker().snap({placed.x + dx, placed.y + above, placed.z + dz}, 6.0, nullptr, 10.0); spot && std::fabs(spot->y - (placed.y + above)) < 10.0)
							++byHeight[static_cast<int>(std::lround((spot->y - placed.y) / 5.0)) * 5];
			return byHeight;
		};
		const auto total = [&](const std::map<int, int>& byHeight) { int n = 0; for (const auto& [h, c] : byHeight) n += c; return n; };
		const int spotsBefore = total(heights());
		const std::size_t placementsBefore = sim.world().placements().size();
		const std::size_t doorsBefore = sim.world().doorSites().size();
		const std::size_t sitesBefore = sim.world().buildingSites().size();

		const Simulation::Stood stood = sim.standBuilding(placed);
		std::cout << "stood: placed " << stood.placed << " nav " << stood.nav << " tiles " << stood.navTiles << "\n";
		test::require(stood.placed == 1, "stood.placed == 1");
		test::require(stood.nav == 1, "stood.nav == 1");
		test::require(stood.navTiles > 0, "stood.navTiles > 0");
		test::require(sim.world().placements().size() == placementsBefore + 1, "sim.world().placements().size() == placementsBefore + 1");
		test::require(sim.world().doorSites().size() > doorsBefore, "sim.world().doorSites().size() > doorsBefore");
		test::require(sim.world().buildingSites().size() == sitesBefore + 1, "sim.world().buildingSites().size() == sitesBefore + 1");
		const int spotsAfter = total(heights());
		std::cout << "footprint grid: " << spotsBefore << " walkable spots before, " << spotsAfter << " after\n";
		// The walls and the footprint took the terrain out: fewer places to stand than the terrain running straight through.
		test::require(spotsAfter < spotsBefore, "the building's collision removed walkable ground");
		// The door is reachable from outside: some street point 250 u out on the ring routes to the door's own spot.
		std::size_t reached = 0;
		for (int k = 0; k < 8; ++k)
		{
			const double a = k * 0.785398;
			const knav::Position from{placed.x + 250.0 * std::cos(a), placed.y, placed.z + 250.0 * std::sin(a)};
			if (const auto start = nav.walker().snap(from, 60.0, nullptr, 40.0))
			{
				const Route route = nav.routeFrom(*start, placed.x, placed.z, std::nullopt, false, {});
				if (route.found && !route.approach && !route.reseated && !route.points.empty()
					&& std::hypot(route.points.back().x - placed.x, route.points.back().z - placed.z) < 15.0)
					++reached;
			}
		}
		std::cout << "door reached from " << reached << " of 8 street points\n";
		test::require(reached > 0, "a route from outside reaches the door");
		// The same instance id is not placed twice.
		test::require(sim.standBuilding(placed).placed == 0, "sim.standBuilding(placed).placed == 0");

		// A constructed building survives our own save and load (kenshi-k16.7.156): the PlayerBuilding records its placement, a load stands it again.
		{
			Simulation game = Simulation::newGame(KENSHI_DATA, order, spawn);
			game.setGameData(gameData);
			const kdata::Record* faction = nullptr;
			for (const kdata::Record* record : gameData->byType(kworld::itemTypeByName("FACTION")))
				if (record->name == "Nameless")
					faction = record;
			test::require(faction != nullptr, "the Nameless faction record");
			game.createPlayerFaction(faction->stringID);
			game.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, {zone}, {}, gameData.get(), NativeCacheOptions{}));
			BuildOrder build;
			for (const std::string& sid : game.unlockedBuildings(game.completedResearch()))
			{
				const kdata::Record* record = gameData->find(sid);
				if (record != nullptr && record->findRefs("doors") != nullptr && !record->findRefs("doors")->entries.empty())
				{
					build.record = sid;
					break;
				}
			}
			test::require(!build.record.empty(), "an unlocked building with doors");
			build.x = 29990.818;
			build.z = -83488.695;
			build.y = game.groundHeight(build.x, build.z).value_or(0.0);
			game.enqueue(Command::buildPlace(build));
			game.run(3);
			std::string constructed;
			game.world().ecs().each([&](const state::PlayerBuilding& b) { constructed = b.placement; });
			test::require(!constructed.empty(), "the construction recorded its placement");
			test::require(game.world().placements().size() == placementsBefore + 1, "the construction stood a placement");
			// The interior of an imported placement (layout, label, hand, relative y) is saved with the building and stands again with it.
			game.world().ecs().each([&](flecs::entity entity, const state::PlayerBuilding&)
			{
				state::PlayerBuilding* building = entity.get_mut<state::PlayerBuilding>();
				building->interiorLayout = "TestLayout";
				building->label = "TestLabel";
				building->townHand = 77;
				building->relativeY = 1.5;
			});
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_stand_building.ksav";
			const std::uint64_t held = game.stateHash();
			game.saveGame(path);
			Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{});
			std::remove(path.c_str());
			test::require(loaded.stateHash() == held, "the save round-trips its hash");
			test::require(loaded.world().placements().size() == placementsBefore + 1, "the load stood the placement again");
			test::require(loaded.world().doorSites().size() > doorsBefore, "the load has the building's doors");
			test::require(loaded.world().buildingSites().size() == sitesBefore + 1, "the load has the building's site");
			const auto& site = loaded.world().buildingSites().back();
			test::require(site.interiorLayout == "TestLayout" && site.label == "TestLabel" && site.townHand == 77,
				"the re-stand carries the interior layout, label and town hand");
			loaded.setGameData(gameData);
			loaded.enableNavigation(MeshNavigator::openNative(KENSHI_DATA, {zone}, {}, gameData.get(), NativeCacheOptions{}));
			const MeshNavigator& after = *loaded.navigation();
			int under = 0;
			for (double dx = -30; dx <= 30; dx += 5)
				for (double dz = -30; dz <= 30; dz += 5)
					if (after.walker().snap({build.x + dx, build.y + 2.0, build.z + dz}, 3.0, nullptr, 3.0))
						++under;
			int fresh = 0;
			for (double dx = -30; dx <= 30; dx += 5)
				for (double dz = -30; dz <= 30; dz += 5)
					if (nav.walker().snap({build.x + dx, build.y + 2.0, build.z + dz}, 3.0, nullptr, 3.0))
						++fresh;
			std::cout << "constructed building: walkable probes " << under << " after the load, " << fresh << " on the mesh without it\n";
			test::require(under != fresh, "the loaded mesh carries the building's collision");
			// An older save's player building has no placement: it loads as before. Only the version just before the placement field
			// (v133) lacks it; once the save moves on, the previous version has it and this case ends (as ksim_save_stamps).
			if (kSaveVersion == 134)
			{
				const std::string oldPath = std::string(KSIM_BINARY_DIR) + "/ksim_stand_building_old.ksav";
				writeSaveFile(oldPath, game.world().ecs(), game.world().loadOrder(), {}, kSaveVersion - 1);
				Simulation old = Simulation::loadGame(KENSHI_DATA, order, oldPath, false, nullptr, RegionConfig{});
				std::remove(oldPath.c_str());
				test::require(old.world().placements().size() == placementsBefore, "an old save's building is not stood");
			}
		}
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_stand_building: " << e.what() << "\n";
		return 1;
	}
}
