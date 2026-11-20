// Interior furniture in the navmesh (docs/formats/NAVMESH_CACHE.md "Interior furniture"): loads the Hub cache (argv[1]) and checks
// that the layout furniture of the Hub's buildings is cut out: no walkable polygon at the centre of any piece that has collision and
// is at least kMinSize across (a table, a counter, a bench), on its own floor. Also checks that the same pieces are not cut from the
// floor below (the reason PROJECTED furniture goes in as OBSTACLE).

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/layouts.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

constexpr float kMinSize = 12.0f; // both xz extents: smaller pieces may sit inside the agent radius of a walkable gap
constexpr float kCentreReach = 2.0f; // a walkable point this close to the centre means the piece was not cut

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_furniture CACHE");
		const knav::NavMesh mesh = knav::NavMesh::load(argv[1]);
		const knav::NavQuery query(mesh);
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, { 20, 32 }, { 20, 32 });
		test::require(area.coverage.furniture > 100 && area.coverage.furnitureWithCollision > 50,
			"the Hub's layouts place furniture with collision (" + std::to_string(area.coverage.furniture) + " pieces, "
				+ std::to_string(area.coverage.furnitureWithCollision) + " with collision)");

		const kdata::GameData gameData = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library(dataDir, gameData);
		const kworld::InteriorLayouts layouts = kworld::InteriorLayouts::load(dataDir);
		std::size_t checked = 0;
		std::size_t cut = 0;
		std::size_t notBaked = 0;
		std::size_t buildings = 0;
		std::string failures;
		for (const kworld::AreaBuilding& building : area.buildings)
		{
			const kworld::InteriorLayout* layout = layouts.of(building.placed);
			if (layout == nullptr || building.placed.positionCell.x != 20 || building.placed.positionCell.z != 32)
				continue;
			++buildings;
			for (const kworld::LayoutItem& item : layout->items)
			{
				const kworld::PlacedInstance piece = kworld::placeFurniture(building.placed, item, gameData);
				std::vector<kworld::WorldShape> shapes;
				kworld::CollisionCoverage coverage;
				const auto pieceNav = library.addBuilding(piece, shapes, coverage, false, true);
				// BUILDING path mode 0 is not baked in the original (rwh.199 §4: 43 + 11 + 24 shipped records): no shapes, so no hole.
				if (pieceNav && pieceNav->pathMode == kworld::PathMode::Ignore)
				{
					test::require(shapes.empty(), "a path mode 0 piece contributes no shape: " + gameData.find(item.target)->name);
					++notBaked;
					continue;
				}
				if (shapes.empty() || std::any_of(shapes.begin(), shapes.end(), [](const kworld::WorldShape& s) { return s.use != kworld::NavUse::Obstacle; }))
					continue;
				knav::Point lo = { shapes[0].min[0], shapes[0].min[1], shapes[0].min[2] };
				knav::Point hi = { shapes[0].max[0], shapes[0].max[1], shapes[0].max[2] };
				for (const kworld::WorldShape& s : shapes)
					for (int k = 0; k < 3; ++k)
					{
						lo[k] = std::min(lo[k], s.min[k]);
						hi[k] = std::max(hi[k], s.max[k]);
					}
				if (hi[0] - lo[0] < kMinSize || hi[2] - lo[2] < kMinSize)
					continue;
				++checked;
				const knav::Point centre = { 0.5f * (lo[0] + hi[0]), lo[1], 0.5f * (lo[2] + hi[2]) };
				// The piece's own floor: from a little below its base to a little above (a stool top is not a floor).
				if (!query.onMesh(centre, { kCentreReach, 6.0f, kCentreReach }))
				{
					++cut;
					continue;
				}
				if (failures.size() < 600)
					failures += " " + gameData.find(item.target)->name + "@" + std::to_string(static_cast<int>(centre[0])) + ","
						+ std::to_string(static_cast<int>(centre[2]));
			}
		}
		std::cout << buildings << " Hub buildings with a layout; " << checked << " pieces checked, " << cut << " cut out, " << notBaked << " path mode 0 (not baked)\n";
		test::require(buildings > 0 && checked > 20, "the census found the Hub's tables and counters");
		test::require(cut == checked, "walkable at the centre of" + failures);
		std::cout << "knav_furniture ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
