// Quarry placements at South Stone Camp: every one exists and routes (kenshi-gzto / k16.5.23 / k16.5.30).
// argv[1]: KSIM_NAVWORLD.
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/interact.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>

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
		// A walking operator at the camp, then every camp quarry placement: the placement exists (the sim no longer drops one for want of a
		// work spot, kenshi-k16.5.30) and the order routes to within the use range of its origin (the original's 65 u hint, 10 / 50 search).
		std::vector<ProducerSite> sites;
		for (const ProducerSite& s : sim.world().producerSites())
		{
			if (std::hypot(s.x - 64800.0, s.z + 50000.0) < 1200.0 && (s.recordSid == "47945-Newwworld.mod" || s.recordSid == "47948-Newwworld.mod"))
				sites.push_back(s);
		}
		test::require(!sites.empty(), "camp quarry placements exist");
		if (const kworld::ZoneCell c = kworld::zoneCellOf(sites[0].x, sites[0].z); !mesh->covers(sites[0].x, sites[0].z))
			mesh->setCoverage({{c.x, c.z}});
		const auto mover = mesh->walker().snap({sites[0].x + 200.0, sites[0].y, sites[0].z}, 600.0);
		test::require(mover.has_value(), "the camp has mesh for an operator to stand on");
		state::Fighter operatorBody;
		operatorBody.x = mover->x;
		operatorBody.y = mover->y;
		operatorBody.z = mover->z;
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const producers::Book book(*gd);
		const auto routed = [&](const ProducerSite& s)
		{
			// The use range is the record's, not a literal (Spec::useRange: the functionality's `use range`).
			const producers::Spec* spec = book.find(s.recordSid);
			test::require(spec != nullptr, "the placement's record is a producer");
			InteractTarget target;
			target.kind = InteractKind::Producer;
			target.x = s.x;
			target.y = s.y;
			target.z = s.z;
			target.reach = target.reachStopped = spec->useRange;
			target.reachY = kProducerReachY;
			target.boxMinX = s.boxMinX;
			target.boxMaxX = s.boxMaxX;
			target.boxMinZ = s.boxMinZ;
			target.boxMaxZ = s.boxMaxZ;
			return routeToInteract(*mesh, operatorBody, target, static_cast<std::uint8_t>(InteractAction::Use), false, Body{}).found;
		};
		std::size_t camp = 0, campRouted = 0;
		for (const ProducerSite& s : sites)
		{
			++camp;
			const bool ok = routed(s);
			campRouted += ok ? 1 : 0;
			std::cout << "  " << s.recordSid << " at " << s.x << "," << s.z << (ok ? " routes\n" : " NO ROUTE\n");
		}
		std::cout << "camp quarries " << camp << ", routed " << campRouted << "\n";
		test::require(camp == 7, "seven quarry placements at the camp");
		test::require(campRouted == camp, "every camp quarry routes (7 of 7)");
		// A wild deposit with mesh near: routes. One with no mesh at all: the order fails as impossible (no route).
		ProducerSite deposit;
		deposit.recordSid = sites[0].recordSid;
		deposit.x = mover->x + 120.0;
		deposit.y = mover->y;
		deposit.z = mover->z;
		test::require(routed(deposit), "a deposit with mesh near has a route");
		ProducerSite lost = deposit;
		lost.x = 64800.0 + 30000.0;
		lost.z = -50000.0;
		test::require(!routed(lost), "a deposit with no mesh at all: the order is impossible");
		// A deposit whose origin is on the mesh but whose 10 / 50 ring has no walkable face: found by scanning the camp mesh.
		std::size_t scanned = 0, onMeshNoRing = 0;
		ProducerSite island = deposit;
		bool haveIsland = false;
		for (double dx = -3000.0; dx <= 3000.0; dx += 50.0)
		{
			for (double dz = -3000.0; dz <= 3000.0; dz += 50.0)
			{
				const double px = 64800.0 + dx, pz = -50000.0 + dz;
				if (!mesh->covers(px, pz))
					continue;
				const auto here = mesh->walker().snap({px, mover->y, pz}, 3.0);
				if (!here)
					continue;
				++scanned;
				bool ring = false;
				for (int k = 0; k < 16 && !ring; ++k)
				{
					const double a = k * (3.14159265358979 / 8.0);
					ring = mesh->walker().snap({here->x + std::sin(a) * kProducerApproach, here->y, here->z + std::cos(a) * kProducerApproach}, kProducerApproachSnapFar).has_value();
				}
				if (!ring)
				{
					++onMeshNoRing;
					island.x = here->x;
					island.y = here->y;
					island.z = here->z;
					haveIsland = true;
				}
			}
		}
		std::cout << "mesh points scanned " << scanned << ", on mesh with no ring face " << onMeshNoRing << "\n";
		if (haveIsland)
			std::cout << "island deposit routes: " << (routed(island) ? "yes (origin fallback)" : "no") << "\n";
		// A formerly dropped deposit becomes a producer entity: a placement or wild deposit of the camp zones whose use point has no
		// walkable point within the old 150 u snap (kenshi-k16.5.23's drop) but whose zone the mesh covers. Step the sim with a player
		// character beside it and look for the entity.
		std::vector<ProducerSite> candidates = sites;
		for (const kworld::ZoneCell& z : zones)
			for (const ProducerSite& w : sim.wildProducerSites(z.x, z.z))
				candidates.push_back(w);
		const ProducerSite* dropped = nullptr;
		std::size_t covered = 0;
		for (const ProducerSite& c : candidates)
		{
			if (!mesh->covers(c.x, c.z))
				continue;
			++covered;
			if (!dropped && !mesh->walker().snap({c.x, c.y, c.z}, 150.0))
				dropped = &c;
		}
		std::cout << "placements and deposits on covered mesh " << covered << ", of them with no walkable point within 150 u: " << (dropped ? 1 : 0) << "\n";
		test::require(covered > 0, "covered producer sites exist");
		const ProducerSite& watch = dropped ? *dropped : sites[0];
		sim.enableCombat(CombatData::build(*gd, KENSHI_DATA));
		sim.enableNavigation(mesh);
		state::Fighter player;
		player.playerControlled = true;
		const auto stand = mesh->walker().snap({watch.x, watch.y, watch.z}, 800.0);
		test::require(stand.has_value(), "mesh near the watched site for the player");
		player.x = stand->x;
		player.y = stand->y;
		player.z = stand->z;
		sim.spawnFighter(player);
		const auto exists = [&]
		{
			bool found = false;
			sim.world().ecs().each([&](const state::Id& id, const state::Producer&) { found = found || id.value == watch.id; });
			return found;
		};
		test::require(!exists(), "the site is not an entity before the sim steps");
		sim.run(producers::kIntervalTicks * 2);
		test::require(exists(), "the placement becomes a producer entity (never dropped)");
		std::cout << "watched site " << watch.recordSid << " #" << watch.id << (dropped ? " (formerly dropped)" : " (no formerly dropped site on this mesh; a camp quarry stands in)") << " is an entity\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAILED: " << e.what() << "\n";
		return 1;
	}
}
