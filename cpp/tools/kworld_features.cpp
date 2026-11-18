// Dumps hand-placed world features (features.dat + road foliage).
// Usage: kworld_features [--list] [KENSHI_DATA_ROOT]
// Summary lines always; --list prints one tab-separated line per placement:
// record id, record type, x y z, qw qx qy qz, sx sy sz, layer, meshes.

#include "kworld/features.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <string>

int main(int argc, char** argv)
{
	bool list = false;
	std::string dataDir = KENSHI_DATA;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--list") == 0)
			list = true;
		else
			dataDir = argv[i];
	}
	try
	{
		const auto features = kworld::Features::load(dataDir);
		std::map<std::string, std::size_t> placementsByType;
		std::map<std::string, std::size_t> recordsByType;
		std::size_t hidden = 0;
		std::size_t meshes = 0;
		std::size_t leaves = 0;
		for (const auto& record : features.records())
		{
			++recordsByType[record.found ? record.recordType : "(missing)"];
			meshes += record.meshes.size();
			for (const auto& mesh : record.meshes)
				leaves += mesh.leaves;
		}
		for (const auto& placement : features.placements())
		{
			const auto& record = features.records()[static_cast<std::size_t>(placement.record)];
			++placementsByType[record.found ? record.recordType : "(missing)"];
			hidden += record.hidden;
			if (list)
			{
				std::printf("%s\t%s\t%.2f %.2f %.2f\t%.6f %.6f %.6f %.6f\t%g %g %g\t%s",
					placement.recordID.c_str(), record.recordType.c_str(), placement.x, placement.y,
					placement.z, placement.qw, placement.qx, placement.qy, placement.qz,
					placement.scaleX, placement.scaleY, placement.scaleZ, placement.layer.c_str());
				for (const auto& mesh : record.meshes)
					std::printf("\t%s", mesh.mesh.c_str());
				std::printf("\n");
			}
		}
		std::printf("features.dat: %zu zones, %zu placements, %zu distinct records, %zu unresolved (no record or no mesh), %zu hidden\n",
			features.zoneCount(), features.placements().size(), features.records().size(),
			features.unresolvedPlacements(), hidden);
		for (const auto& [type, count] : placementsByType)
			std::printf("  placements of %s: %zu (records %zu)\n", type.c_str(), count, recordsByType[type]);
		std::printf("  drawable meshes over records: %zu (%zu leaves)\n", meshes, leaves);
		std::printf("road foliage: %zu points after the layer overlay (no record: target \"-\")\n",
			features.roadFoliage().size());
		std::map<std::string, std::size_t> byLayer;
		for (const auto& point : features.roadFoliage())
			++byLayer[point.layer];
		for (const auto& [layer, count] : byLayer)
			std::printf("  layer %s: %zu\n", layer.c_str(), count);
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld_features: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
