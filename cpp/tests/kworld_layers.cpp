// Load-order layering of world placements: synthetic keys (override, delete,
// revive, per-cell keys, order) and the shipped data (11,714 survivors).

#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"
#include "kworld_test_util.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

kworld::ZoneFile makeFile(const std::string& layer, int cellX, int cellZ,
	std::vector<std::pair<std::string, std::string>> entries)
{
	kworld::ZoneFile file;
	file.layer = layer;
	file.path = layer + "/zone";
	file.cellX = cellX;
	file.cellZ = cellZ;
	for (const auto& [id, target] : entries)
	{
		kworld::ZoneInstance instance;
		instance.instanceID = id;
		instance.targetID = target;
		instance.recordType = kworld::itemTypeByName("INSTANCE_COLLECTION");
		instance.collectionID = "0-buildinglist";
		instance.x = 1.0;
		instance.kind = target.empty() ? kworld::ZoneInstanceKind::Deleted
			: kworld::ZoneInstanceKind::Placement;
		file.instances.push_back(std::move(instance));
	}
	return file;
}

void checkSynthetic()
{
	// Files given out of load order on purpose.
	std::vector<kworld::ZoneFile> files;
	files.push_back(makeFile("rebirth", 1, 1,
		{ { "a", "" }, { "b", "b-rebirth" }, { "e", "" }, { "f", "f-rebirth" } }));
	files.push_back(makeFile(".", 1, 1,
		{ { "a", "a-base" }, { "b", "b-base" }, { "c", "c-base" }, { "f", "f-base" } }));
	files.push_back(makeFile("Dialogue", 1, 1, { { "f", "" } }));
	files.push_back(makeFile("Newwworld", 1, 1, { { "d", "d-new" } }));
	files.push_back(makeFile(".", 2, 2, { { "a", "a-other-cell" } }));

	const auto zones = kworld::Zones::fromFiles(std::move(files));
	kworld::LayerStats stats;
	const auto resolved = zones.resolveLayers(&stats);

	// Slots in first-appearance order: a(1,1) b c f a(2,2) d.
	// a deleted by rebirth; b overridden; f deleted by Dialogue then revived.
	test::require(resolved.size() == 5, "synthetic survivor count");
	const std::vector<std::pair<std::string, std::string>> expected = {
		{ "b", "b-rebirth" }, { "c", "c-base" }, { "f", "f-rebirth" },
		{ "a", "a-other-cell" }, { "d", "d-new" },
	};
	for (std::size_t i = 0; i < expected.size(); ++i)
		test::require(resolved[i].instance->instanceID == expected[i].first
			&& resolved[i].instance->targetID == expected[i].second,
			"synthetic survivor " + std::to_string(i));
	test::require(stats.entries == 11 && stats.overrides == 1 && stats.deletions == 2
		&& stats.orphanDeletions == 1 && stats.survivors == 5 && stats.placements == 5,
		"synthetic layer stats");

	std::vector<kworld::ZoneFile> unknown;
	unknown.push_back(makeFile("mystery", 0, 0, {}));
	bool threw = false;
	try
	{
		kworld::Zones::fromFiles(std::move(unknown));
	}
	catch (const kworld::Error&)
	{
		threw = true;
	}
	test::require(threw, "unknown layer accepted");
}

} // namespace

int main()
{
	try
	{
		checkSynthetic();

		const auto zones = kworld::Zones::load(KENSHI_DATA);
		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto heightmap = kworld::Heightmap::open(
			test::dataPath("newland/land/fullmap.tif"));
		kworld::LayerStats stats;
		const auto merged = zones.placeMerged(heightmap, gameData, &stats);
		test::require(merged.size() == 11714, "merged placement count "
			+ std::to_string(merged.size()) + ", expected 11714");
		test::require(merged.size() == stats.placements, "stats disagree with result");

		std::size_t missing = 0;
		for (const auto& entry : merged)
			if (!entry.hasScale)
				++missing;
		test::require(missing == 12, "missing-target count " + std::to_string(missing));
		test::require(zones.place(heightmap, gameData).size() == 14614,
			"place() must stay unlayered");

		std::cout << "layers: " << stats.entries << " entries, " << stats.overrides
			<< " overrides, " << stats.deletions << " deletions, "
			<< stats.orphanDeletions << " orphan deletions, " << stats.survivors
			<< " survivors, " << stats.placements << " placements\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "layers: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
