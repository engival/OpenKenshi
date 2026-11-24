// Quarry placements at South Stone Camp become producers (kenshi-gzto / k16.5.23).
// argv[1]: KSIM_NAVWORLD.
#include "ksim/core/simulation.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/nav/navigator.hpp"

#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <map>

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_quarry_camp KSIM_NAVWORLD");
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		const kworld::ZoneCell cell = kworld::zoneCellOf(64800.0, -50000.0);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({cell.x + dx, cell.z + dz});
		const std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(argv[1], zones);
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, SpawnOptions{}, nullptr, RegionConfig{});
		// The reach stepProducers snaps a placement with (no collision bounds known: kPlacementSnap) against the old flat kUseSnap.
		std::size_t camp = 0, campOld = 0, sites = 0, droppedOld = 0, droppedNow = 0, uncovered = 0;
		std::map<std::string, std::size_t> stillBy;
		for (const ProducerSite& s : sim.world().producerSites())
		{
			if (s.boxMaxX > s.boxMinX)
				continue;
			++sites;
			const kworld::ZoneCell c = kworld::zoneCellOf(s.x, s.z);
			if (!mesh->covers(s.x, s.z))
				mesh->setCoverage({{c.x, c.z}});
			if (!mesh->covers(s.x, s.z))
			{
				++uncovered;
				continue;
			}
			const bool old = mesh->walker().snap({s.x, s.y, s.z}, producers::kUseSnap).has_value();
			const bool now = mesh->walker().snap({s.x, s.y, s.z}, producers::kPlacementSnap).has_value();
			if (!now)
				++stillBy[s.recordSid];
			droppedOld += old ? 0 : 1;
			droppedNow += now ? 0 : 1;
			if (std::hypot(s.x - 64800.0, s.z + 50000.0) < 1200.0 && (s.recordSid == "47945-Newwworld.mod" || s.recordSid == "47948-Newwworld.mod"))
			{
				++camp;
				campOld += old ? 1 : 0;
				test::require(now, "every quarry placement at the camp has a use point");
			}
		}
		std::cout << "placements " << sites << " uncovered " << uncovered << " dropped by the old rule " << droppedOld
			<< " still dropped " << droppedNow << "; camp quarries " << camp << " (old rule found " << campOld << ")\n";
		for (const auto& [sid, n] : stillBy)
			std::cout << "  still dropped " << sid << " x" << n << "\n";
		test::require(camp == 7 && campOld == 3, "seven quarry placements at the camp, the old rule found three");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}
