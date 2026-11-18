// Shipped data: features.dat and road foliage counts (TERRAIN_FORMAT.md,
// WORLD_LAYOUT.md) and that every placement's record resolves or is a known
// miss.

#include "kworld/features.hpp"
#include "test_util.hpp"

#include <iostream>
#include <set>
#include <string>

int main()
{
	try
	{
		const auto features = kworld::Features::load(KENSHI_DATA);
		test::require(features.zoneCount() == 587, "features.dat zone count");
		test::require(features.placements().size() == 1821, "features.dat placement count");

		std::set<std::string> names;
		std::size_t hidden = 0;
		std::size_t missing = 0;
		std::size_t nonFinite = 0;
		for (const auto& placement : features.placements())
		{
			names.insert(placement.recordID);
			const auto& record = features.records()[static_cast<std::size_t>(placement.record)];
			hidden += record.found && record.hidden;
			missing += !record.found;
			nonFinite += !placement.rotationFinite();
		}
		test::require(names.size() == 224 && features.records().size() == 224,
			"features.dat distinct names");
		test::require(missing == 11, "features.dat placements with a missing record: "
			+ std::to_string(missing));
		test::require(nonFinite == 1, "features.dat non-finite rotations");
		std::size_t nonUniform = 0;
		std::size_t negative = 0;
		for (const auto& placement : features.placements())
		{
			nonUniform += placement.scaleX != placement.scaleY || placement.scaleX != placement.scaleZ;
			negative += placement.scaleX < 0 || placement.scaleY < 0 || placement.scaleZ < 0;
		}
		test::require(nonUniform == 194, "features.dat non-uniform scales: " + std::to_string(nonUniform));
		test::require(negative == 1, "features.dat negative scales: " + std::to_string(negative));
		test::require(features.unresolvedPlacements() == missing + hidden,
			"unresolved = missing + hidden");

		// 2,666 Newwworld + 59 rebirth points; ids never collide, Dialogue is empty.
		test::require(features.roadFoliage().size() == 2725, "road foliage after overlay: "
			+ std::to_string(features.roadFoliage().size()));
		for (const auto& point : features.roadFoliage())
			test::require(point.targetID == "-" && !point.deleted, "road foliage point target");
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld features: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld features: shipped data counts ok\n";
	return 0;
}
