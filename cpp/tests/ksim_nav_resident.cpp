// The resident navmesh (kenshi-k16.7.204): MeshNavigator::makeResident, the load path the server takes with MeshNavigator::worldZones(), here on a small
// named set (the whole world is minutes and gigabytes). After it the mesh holds exactly those zones, setCoverage (the sim's streaming call) changes
// nothing, a seam between two loaded zones is linked, and the planner's connected sets are built already. Runs against the install (KENSHI_DATA);
// exit 77 without the navtiles. Fenced with the other nav tests while KSIM_NAV_TESTS=OFF.

#include "ksim/dev/nav_zone_set.hpp"
#include "ksim/nav/navigator.hpp"

#include "kworld/coords.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <iostream>

int main()
{
	using namespace ksim;
	try
	{
		if (!test::worldRegionReady("", 20, 32))
		{
			std::cerr << "ksim_nav_resident: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav("", {{20, 32}});
		test::require(!nav->resident(), "resident before makeResident");
		const auto set = dev::namedZoneSet("20,32,21,32");
		test::require(set && set->size() == 2, "the named zone set 20,32,21,32 is two zones");
		test::require(!dev::namedZoneSet("nonsense") && !dev::namedZoneSet("0,0,64,0"), "a bad set name or a box off the world is refused");
		nav->makeResident(*set);
		test::require(nav->resident() && nav->coveredZones().size() == 2, "two zones resident");
		const ksim::MeshNavigator::ResidentLoad& load = nav->residentLoad();
		test::require(load.zones == 2 && load.tiles > 0 && load.polys > 0, "the load counts the zones, tiles and polygons");
		std::cout << "resident: " << load.zones << " zones, " << load.tiles << " tiles, " << load.polys << " polys, load " << load.loadMs << " ms, sets "
			<< load.setsMs << " ms\n";

		// No streaming: the sim's coverage call changes nothing, whatever the areas want.
		const std::size_t tiles = nav->mesh().tiles().size();
		const MeshNavigator::CoverageChange change = nav->setCoverage({{40, 40}, {20, 32}});
		test::require(!change.changed() && nav->coveredZones().size() == 2 && nav->mesh().tiles().size() == tiles, "setCoverage on a resident mesh changes nothing");
		test::require(nav->zoneLoaded({21, 32}) && !nav->zoneLoaded({22, 32}) && !nav->canCover({40, 40}), "a zone outside the set is not loaded and cannot be");

		// The seam between the two zones is linked.
		const double border = kworld::zoneMinX(21);
		bool across = false;
		for (double z = kworld::zoneMinZ(32) + 150.0; z < kworld::zoneMinZ(33) && !across; z += 200.0)
		{
			const auto west = nav->nearestWalkable(border - 80.0, z, 20.0);
			const auto east = nav->nearestWalkable(border + 80.0, z, 20.0);
			across = west && east && west->x < border && east->x > border && nav->route(west->x, west->z, east->x, east->z).found;
		}
		test::require(across, "a route over the seam of the two resident zones");
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_nav_resident: FAILED: " << error.what() << "\n";
		return 1;
	}
	std::cout << "ksim_nav_resident: ok\n";
	return 0;
}
