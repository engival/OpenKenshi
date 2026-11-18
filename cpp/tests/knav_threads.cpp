// A bake is the same bytes at any thread count and on every run (kenshi-k16.7.50): Recast's rcBuildContours reads
// heap bytes it never wrote, so zone 29,16 (tile 2,6) came out two ways at --threads 8 until bakeTileSet gave Recast
// a zeroing allocator. The first bake is the reference; the rest repeat at several thread counts.

#include "knav/area.hpp"
#include "knav/world.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <iostream>
#include <string>

namespace {

std::vector<knav::BakedTile> bake(const knav::AreaGeometry& area, const kworld::Heightmap& terrain, int threads)
{
	const kworld::ZoneCell zone{ 29, 16 };
	knav::NavWorld world(knav::configForWorldZones(zone, zone), terrain, area.foliage);
	for (const knav::AreaBuilding& building : area.buildings)
		world.setBuilding(building.id, building.shapes, building.nav);
	world.bakeAll(threads);
	return world.mesh().tiles();
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		knav::AreaGatherer gatherer(dataDir, terrain);
		const knav::AreaGeometry area = gatherer.gather({ 29, 16 }, { 29, 16 });
		const auto reference = bake(area, terrain, 1);
		test::require(reference.size() > 100, "too few tiles baked");
		// Before the fix about one run in three at 8 threads differed (tile 2,6: 26236 or 25988 bytes).
		for (const int threads : { 8, 8, 8, 8, 8, 8, 16, 16, 16, 16 })
		{
			const auto again = bake(area, terrain, threads);
			test::require(again.size() == reference.size(), "tile count differs at " + std::to_string(threads) + " threads");
			for (std::size_t i = 0; i < reference.size(); ++i)
				test::require(again[i].x == reference[i].x && again[i].z == reference[i].z && again[i].data == reference[i].data,
					"tile (" + std::to_string(reference[i].x) + ", " + std::to_string(reference[i].z) + ") differs at "
					+ std::to_string(threads) + " threads");
		}
		std::cout << "tiles compared: " << reference.size() << "\n";
	} catch (const std::exception& e) {
		std::cerr << "knav_threads: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_threads ok\n";
	return 0;
}
