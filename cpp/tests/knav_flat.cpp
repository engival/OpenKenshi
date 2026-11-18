// The flat-tile shortcut (BakeTuning::flatTiles) is exact: a window with open sea, a coast and
// land bakes to the same bytes with and without it. Zones 52..53 x 20..21 (region 13, 5).

#include "knav/area.hpp"
#include "knav/world.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <iostream>
#include <string>

namespace {

std::vector<knav::BakedTile> bake(knav::AreaGatherer& gatherer, const kworld::Heightmap& terrain,
	kworld::ZoneCell min, kworld::ZoneCell max)
{
	const knav::AreaGeometry area = gatherer.gather(min, max);
	knav::NavWorld world(knav::configForWorldZones(min, max), terrain, area.foliage);
	for (const knav::AreaBuilding& building : area.buildings)
		world.setBuilding(building.id, building.shapes, building.nav);
	world.bakeAll(4);
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
		const kworld::ZoneCell min{ 52, 20 };
		const kworld::ZoneCell max{ 53, 21 };

		knav::BakeTuning tuning;
		tuning.flatTiles = false;
		knav::setBakeTuning(tuning);
		const std::uint64_t before = knav::flatTilesBaked();
		const auto reference = bake(gatherer, terrain, min, max);
		test::require(knav::flatTilesBaked() == before, "flat tiles were built with the shortcut off");

		tuning.flatTiles = true;
		knav::setBakeTuning(tuning);
		const auto fast = bake(gatherer, terrain, min, max);
		const std::uint64_t flat = knav::flatTilesBaked() - before;

		test::require(fast.size() == reference.size(), "the shortcut changed the tile count");
		for (std::size_t i = 0; i < fast.size(); ++i)
		{
			test::require(fast[i].x == reference[i].x && fast[i].z == reference[i].z, "tile order differs");
			test::require(fast[i].data == reference[i].data && fast[i].doors == reference[i].doors,
				"tile (" + std::to_string(fast[i].x) + ", " + std::to_string(fast[i].z) + ") differs");
		}
		std::cout << "tiles compared: " << fast.size() << ", flat shortcuts: " << flat << "\n";
		test::require(flat > 100, "too few flat tiles: the window no longer covers open sea");
		test::require(flat < fast.size(), "no tile took the full pipeline: the window no longer covers land");
	} catch (const std::exception& e) {
		std::cerr << "knav_flat: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_flat ok\n";
	return 0;
}
