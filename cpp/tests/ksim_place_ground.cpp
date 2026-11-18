// One placement for a character put at a saved or authored spot (kenshi-k16.6.296, Simulation::placeOnGround): the navmesh's nearest point and nothing
// more (k16.8.31: no terrain snap). Pinned for the scenario file (scenario::apply) and the squad import (importPromote, the save importer's route):
// open ground on a slope and the Bar's ground floor both end exactly on the mesh point.
// argv[1]: the world navmesh bake (or KSIM_NATIVE_NAV=1 for the runtime mesh, nav_source.hpp); exit 77 without it.
#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

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

namespace {

using namespace ksim;

constexpr double kBarX = -50592.0; // the Hub's own bar (ksim_feet_indoors)
constexpr double kBarZ = -1211.0;
constexpr double kOpenX = -50622.0; // spectate_fight's open ground
constexpr double kOpenZ = -1110.0;
constexpr const char* kSquad = "16865-gamedata.base";
constexpr const char* kChar = "16863-gamedata.base";

struct Game
{
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::shared_ptr<const CombatData> combat;
	kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");

	explicit Game(const std::string& bake)
	{
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		mesh = test::openWorldNav(bake, zones);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 3;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		sim->enableCombat(combat, CombatSettings{});
		sim->enableNavigation(mesh);
		sim->newWorld();
		sim->run(1);
	}

	// A scenario character at (x, y, z): where it stands after scenario::apply.
	state::Fighter viaScenario(double x, double y, double z)
	{
		scenario::File file;
		file.centreX = x;
		file.centreZ = z;
		scenario::Squad squad;
		squad.id = 1;
		squad.name = "probe";
		squad.templateSid = kSquad;
		file.squads.push_back(squad);
		scenario::Char c;
		c.id = 1;
		c.squad = 1;
		c.name = "probe";
		c.templateSid = kChar;
		c.x = x;
		c.y = y;
		c.z = z;
		c.leader = true;
		file.chars.push_back(c);
		simlink::ServerContext context;
		context.gameData = sim->gameData();
		context.squadLimit = 50;
		const scenario::Placed placed = scenario::apply(*sim, *sim->gameData(), *combat, context, file, 5);
		test::require(placed.others.size() == 1, "the scenario placed its character");
		return *sim->combat()->find(placed.others.front());
	}

	// An imported squad's member at (x, y, z): where it stands after importPromote (the save importer's route, snapFull on).
	state::Fighter viaImport(double x, double y, double z)
	{
		state::SquadInstance instance;
		instance.templateSid = kSquad;
		const flecs::entity squad = sim->importSquad(instance, state::Position{x, z});
		const auto member = sim->importMember(squad, kChar, true, 0, [](state::Fighter&, state::Inventory&) {}, x, z);
		test::require(member.has_value(), "the import built its member");
		const std::size_t unplaced = sim->importPromote(squad, {{*member, x, y, z}}, true);
		test::require(unplaced == 0, "the imported member found ground");
		return *sim->combat()->find(member->get<state::Id>()->value);
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		if (std::getenv("KSIM_NATIVE_NAV") == nullptr
			&& !std::filesystem::exists(bake + "/" + knav::regionFileName(simlink::kHubZoneX / knav::kRegionZones, simlink::kHubZoneZ / knav::kRegionZones)))
		{
			std::cout << "ksim_place_ground: SKIPPED: needs the world navmesh bake (pass its directory) or KSIM_NATIVE_NAV=1\n";
			return 77;
		}
		Game game(bake);
		// A placement is the navmesh's nearest point and nothing more (no terrain snap, k16.8.31): open ground on a slope (where the mesh
		// floats over the terrain) and the Bar's ground floor both end exactly on the mesh point, by the scenario and by the import.
		double floatMax = -1.0;
		double openX = kOpenX;
		double openZ = kOpenZ;
		for (double x = kOpenX - 100.0; x <= kOpenX + 100.0; x += 4.0)
			for (double z = kOpenZ - 100.0; z <= kOpenZ + 100.0; z += 4.0)
			{
				const auto at = game.mesh->walker().snap({x, game.terrain.terrainHeight(x, z), z}, 100.0);
				if (!at || game.sim->buildingAt(at->x, at->y, at->z) != 0)
					continue;
				const double gap = std::fabs(at->y - game.terrain.terrainHeight(at->x, at->z));
				if (gap > floatMax && gap <= 5.0)
				{
					floatMax = gap;
					openX = x;
					openZ = z;
				}
			}
		std::printf("place_ground: the mesh floats %.3f u over the terrain at (%.0f, %.0f)\n", floatMax, openX, openZ);
		test::require(floatMax > 0.05, "the probe finds a spot where the mesh floats over the terrain");
		const double open = game.terrain.terrainHeight(openX, openZ);
		const double barTerrain = game.terrain.terrainHeight(kBarX, kBarZ);
		const auto floor = game.mesh->walker().snap({kBarX, barTerrain + 15.0, kBarZ}, 100.0);
		test::require(floor && game.sim->buildingAt(floor->x, floor->y, floor->z) != 0, "the Bar floor point is inside the Bar");
		const struct { double x, y, z; } probes[] = { {openX, open, openZ}, {openX, open + 2.0, openZ}, {floor->x, floor->y, floor->z} };
		for (const auto& at : probes)
		{
			const auto mesh = game.mesh->walker().snap({at.x, at.y, at.z}, 100.0);
			test::require(mesh.has_value(), "the probe has a mesh point");
			const state::Fighter s = game.viaScenario(at.x, at.y, at.z);
			test::require(s.y == mesh->y, "scenario character stands " + std::to_string(s.y - mesh->y) + " u off the mesh point");
			const state::Fighter i = game.viaImport(at.x, at.y, at.z);
			test::require(i.y == mesh->y, "imported member stands " + std::to_string(i.y - mesh->y) + " u off the mesh point");
		}
		std::printf("place_ground: scenario and import place on the mesh point (open ground, Bar floor)\n");
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_place_ground: %s\n", e.what());
		return 1;
	}
}
