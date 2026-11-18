// A tile baked in a small world window is the same bytes as in a bigger one: region files can
// be baked on their own and still line up at the seams. Zones 20..21 x 32..33 (around the Hub)
// against 19..22 x 31..34.

#include "knav/area.hpp"
#include "knav/world.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
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
		const auto small = bake(gatherer, terrain, { 20, 32 }, { 21, 33 });
		const auto large = bake(gatherer, terrain, { 19, 31 }, { 22, 34 });
		std::size_t compared = 0;
		for (const knav::BakedTile& tile : small)
		{
			const auto other = std::find_if(large.begin(), large.end(),
				[&](const knav::BakedTile& t) { return t.x == tile.x && t.z == tile.z; });
			test::require(other != large.end(), "a tile of the small window is missing from the large one");
			test::require(other->data == tile.data, "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z)
				+ ") differs between windows");
			++compared;
		}
		std::cout << "tiles compared: " << compared << " of " << small.size() << " small, " << large.size() << " large\n";
		test::require(compared > 1000, "too few tiles compared");
	} catch (const std::exception& e) {
		std::cerr << "knav_seam: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_seam ok\n";
	return 0;
}
