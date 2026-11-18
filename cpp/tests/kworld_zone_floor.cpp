// A placed zone building's y is node y + max(100, ground) (kenshi-k16.7.10; the original's Building::createPhysical, kinspect
// findings/bridge_placement.md). The Greenlands Pontoon Bridge: span A (14792-Newwworld-INGAME) is floored at the sea plane, 112.7395
// live; span B (2507-Newwworld-INGAME) stands on the dry bank, 115.7238 live. Ours reads the terrain fallback (the water-body field is
// not loaded): B differs from the field by the heightmap's own 0.1 u.

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"
#include "kworld_test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>

int main()
{
	try
	{
		const auto zones = kworld::Zones::load(KENSHI_DATA);
		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto heightmap = kworld::Heightmap::open(test::dataPath("newland/land/fullmap.tif"));
		const auto placed = zones.placeMerged(heightmap, gameData);
		const struct { const char* id; double x, z, live; } spans[] = {
			{ "14792-Newwworld-INGAME", -45549.33, -26493.05, 112.7395 }, { "2507-Newwworld-INGAME", -45473.16, -26373.55, 115.7238 } };
		for (const auto& span : spans)
		{
			bool found = false;
			for (const kworld::PlacedInstance& p : placed)
			{
				if (p.instanceID != span.id || std::fabs(p.x - span.x) > 1.0 || std::fabs(p.z - span.z) > 1.0) // ids repeat across zones
					continue;
				found = true;
				std::cout << span.id << ": y " << p.y << " (live " << span.live << "), instance y " << p.relativeY << ", terrain "
						  << heightmap.height(p.x, p.z) << "\n";
				test::require(std::fabs(p.y - span.live) < 0.2, std::string(span.id) + ": y is not the original's");
			}
			test::require(found, std::string(span.id) + " is not placed");
		}
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "kworld_zone_floor: " << e.what() << "\n";
		return 1;
	}
}
