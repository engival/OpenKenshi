// kenshi-k16.7.203: the clearance components after a tile swap. A mesh whose labels were built, then had tiles removed, re-added and swapped
// (NavMesh::replaceTiles, what a building's rebake does), is relabelled over a window round the changed tiles; the partition must equal the one a
// fresh query builds whole. Runs on a two-zone resident set (KENSHI_DATA navtiles); exit 77 without them. Fenced with the other nav tests.

#include "ksim/dev/nav_zone_set.hpp"
#include "ksim/nav/navigator.hpp"

#include "knav/navmesh.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"

#include <chrono>
#include <iostream>

int main()
{
	using namespace ksim;
	try
	{
		if (!test::worldRegionReady("", 20, 32))
		{
			std::cerr << "knav_components_incremental: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		const std::shared_ptr<MeshNavigator> nav = test::openWorldNav("", {{20, 32}});
		nav->makeResident(*dev::namedZoneSet("20,32,21,32"));
		knav::NavMesh& mesh = const_cast<knav::NavMesh&>(nav->mesh());
		const std::vector<float> radii{ 2.0f, 5.0f, 12.0f };
		knav::NavQuery live(mesh);
		for (const float radius : radii)
			live.componentsDigest(radius);
		test::require(live.componentStat(0) == radii.size(), "one whole build per radius");

		// Tiles near the middle of the set: swap one for itself, remove two neighbours, put them back, remove a lone one.
		const std::vector<knav::BakedTile> all = mesh.tiles();
		const knav::BakedTile& a = all[all.size() / 2];
		const knav::BakedTile& b = all[all.size() / 2 + 1];
		const knav::BakedTile& c = all[all.size() / 3];
		const auto ms = [](std::chrono::steady_clock::time_point since) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count(); };
		const auto check = [&](const char* what)
		{
			knav::NavQuery fresh(mesh);
			for (const float radius : radii)
			{
				const auto t0 = std::chrono::steady_clock::now();
				const std::uint64_t mine = live.componentsDigest(radius);
				const double liveMs = ms(t0);
				const auto t1 = std::chrono::steady_clock::now();
				const std::uint64_t whole = fresh.componentsDigest(radius);
				test::require(mine == whole, std::string("the incremental partition differs from a whole build: ") + what);
				std::cout << what << " r " << radius << ": ok, live " << liveMs << " ms, whole " << ms(t1) << " ms (updates " << live.componentStat(1) << ", fallbacks " << live.componentStat(2) << ")\n";
			}
		};
		mesh.replaceTiles({}, { a });
		check("a tile swapped for itself");
		mesh.replaceTiles({ { a.x, a.z }, { b.x, b.z } }, {});
		check("two tiles removed");
		mesh.replaceTiles({}, { a, b });
		check("the two tiles put back");
		mesh.replaceTiles({ { c.x, c.z } }, {});
		check("one tile removed");
		std::cout << "updates " << live.componentStat(1) << ", fallbacks " << live.componentStat(2) << ", builds " << live.componentStat(0) << "\n";
		test::require(live.componentStat(1) > 0, "no window update ran");
	}
	catch (const std::exception& error)
	{
		std::cerr << "knav_components_incremental: FAILED: " << error.what() << "\n";
		return 1;
	}
	std::cout << "knav_components_incremental: ok\n";
	return 0;
}
