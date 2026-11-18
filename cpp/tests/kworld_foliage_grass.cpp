// `limit to grass areas` (FOLIAGE_EXE.md section 4, 1406d3740:143-163): a candidate is re-rolled until the overlay's R (the
// PNG's baked grass density) plus G (the grass spots earlier placements wrote) exceeds 149. Zone 16,15 (Floodwastes) holds
// the live capture of kinspect findings/navhash.md section 15: 37 Thin Tree [Wide] trunks, the layer whose mesh is
// `limit to grass areas`.

#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "kworld_test_util.hpp"

#include <iostream>
#include <map>
#include <string>

int main()
{
	try
	{
		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto terrain = kworld::Heightmap::open(test::dataPath("newland/land/fullmap.tif"));
		const auto biomes = kworld::BiomeMap::load(test::dataPath("newland/land/biomemap.png"));
		const auto features = kworld::Features::join(gameData, KENSHI_DATA);
		const auto towns = kworld::Towns::join(gameData, KENSHI_DATA);

		const auto instances = kworld::generateFoliage({ 16, 15 }, terrain, biomes, gameData, features, towns);
		std::map<std::string, std::size_t> limited;
		std::size_t total = 0;
		for (const kworld::FoliageInstance& instance : instances)
			if (instance.grassLimited)
			{
				++total;
				++limited[instance.recordID];
			}
		// Before the gate (every candidate passed): 34 Thin Tree trunks and 1023 grass-limited instances. The live capture has 37 trunks
		// (and 37 leaf entities, which are not instances here), so the gate's 3 extra survivors are the ones the exe's re-rolls find.
		// The three Bouldersmall layers' counts do not move: they already matched the capture (1126 / 321 / 314 with their children).
		test::require(limited["3631-Newwworld.mod"] == 37, "Thin Tree trunks: the live capture has 37, got " + std::to_string(limited["3631-Newwworld.mod"]));
		test::require(limited["5667-Newwworld.mod"] == 413 && limited["5668-Newwworld.mod"] == 284 && limited["5669-Newwworld.mod"] == 292,
			"the Bouldersmall grass-limited counts moved");
		test::require(total == 1026 && instances.size() == 2375, "zone 16,15: " + std::to_string(instances.size()) + " instances, "
			+ std::to_string(total) + " grass-limited (pinned 2375 / 1026; 2372 / 1023 before the gate)");
		std::cout << "zone 16,15: " << instances.size() << " instances, " << total << " grass-limited, Thin Tree 37 (live capture 37)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "kworld_foliage_grass: FAILED: " << e.what() << "\n";
		return 1;
	}
}
