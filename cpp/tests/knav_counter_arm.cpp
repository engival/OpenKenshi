// The Hub bar's counter (Storm House "Bar" layout, Shop Counter at -50558.7, -1188.4; kenshi-k16.7.19, playtest kenshi-2vt): the bake cuts all of
// its collision out of the walkable mesh, and the walk from the customer side to the stock behind it goes round the counter's ends.
// The counter's model is L-shaped but its collision file (shop_counter.xml) is ONE box, 31.6 x 11.5 u (the "west arm" seen in the client is render
// only, so the original has no collision there either). Reads the baked Hub cache (argv[1]).

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/layouts.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_counter_arm CACHE");
		const knav::NavMesh mesh = knav::NavMesh::load(argv[1]);
		const knav::NavQuery query(mesh);
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, { 20, 32 }, { 20, 32 });
		const kdata::GameData gameData = kworld::loadMergedGameData(dataDir);
		kworld::CollisionLibrary library(dataDir, gameData);
		const kworld::InteriorLayouts layouts = kworld::InteriorLayouts::load(dataDir);
		int counters = 0;
		int samples = 0;
		int walkable = 0;
		int controls = 0;
		std::string failures;
		for (const kworld::AreaBuilding& building : area.buildings)
		{
			const kworld::InteriorLayout* layout = layouts.of(building.placed);
			if (layout == nullptr || layout->sid != "3384-TwoStorey.mod-Bar")
				continue;
			for (const kworld::LayoutItem& item : layout->items)
			{
				if (gameData.find(item.target)->name != "Shop Counter")
					continue;
				const kworld::PlacedInstance piece = kworld::placeFurniture(building.placed, item, gameData);
				std::vector<kworld::WorldShape> shapes;
				kworld::CollisionCoverage coverage;
				library.addBuilding(piece, shapes, coverage, false, true);
				test::require(shapes.size() == 1 && coverage.partsWithCollision == 1, "the Shop Counter is one collision box (shop_counter.xml)");
				++counters;
				const kworld::WorldShape& box = shapes[0];
				// The hull's bottom quad is vertices 0-3 (the box is tilted with the floor, so its bounds are wider than its footprint): sample it
				// bilinearly, inset from the edges, at the height of its base.
				const float* q[4] = { &box.vertices[0], &box.vertices[3], &box.vertices[6], &box.vertices[9] };
				for (float u = 0.1f; u < 0.95f; u += 0.1f)
					for (float v = 0.15f; v < 0.9f; v += 0.2f)
					{
						// q0 -> q1 runs along the counter, q0 -> q3 across it.
						const float x = q[0][0] + u * (q[1][0] - q[0][0]) + v * (q[3][0] - q[0][0]);
						const float y = q[0][1] + u * (q[1][1] - q[0][1]);
						const float z = q[0][2] + u * (q[1][2] - q[0][2]) + v * (q[3][2] - q[0][2]);
						++samples;
						if (query.onMesh({ x, y + 1.0f, z }, { 0.5f, 3.0f, 0.5f }))
						{
							++walkable;
							if (failures.size() < 300)
								failures += " (" + std::to_string(static_cast<int>(x)) + "," + std::to_string(static_cast<int>(z)) + ")";
						}
					}
				// Control: a point 4 u off the counter's long face (v = -0.35 of its width, on the customer side) is walkable, so the samples above can say yes.
				const float cx = q[0][0] + 0.5f * (q[1][0] - q[0][0]) - 0.35f * (q[3][0] - q[0][0]);
				const float cy = 0.5f * (q[0][1] + q[1][1]);
				const float cz = q[0][2] + 0.5f * (q[1][2] - q[0][2]) - 0.35f * (q[3][2] - q[0][2]);
				if (query.onMesh({ cx, cy + 1.0f, cz }, { 0.5f, 3.0f, 0.5f }))
					++controls;
			}
		}
		std::cout << counters << " Shop Counter(s) in the Hub bar; " << samples << " footprint samples, " << walkable << " walkable\n";
		test::require(counters >= 1 && samples > 40, "the census found the Hub bar's counter");
		test::require(controls >= 1, "the floor beside the counter is walkable (the control)");
		test::require(walkable == 0, "walkable inside the counter's footprint at" + failures);
		std::cout << "knav_counter_arm ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
