// Per-zone instance counts must match the reference kmod.py, the zone cell
// mapping must round-trip, and every placement must lie in its file's cell.

#include "kworld/coords.hpp"
#include "kworld/zones.hpp"
#include "kworld_test_util.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct Expected
{
	long records = 0;
	long version = 0;
	long placements = 0;
	long deletions = 0;
	long inventory = 0;
};

std::map<std::string, Expected> referenceCounts()
{
	const std::string command = "python3 -I "
		+ test::shellQuote(test::pythonScript("zone_expected.py")) + " "
		+ test::shellQuote(test::refTool("kmod.py")) + " "
		+ test::shellQuote(KENSHI_DATA) + " 2>&1";
	const std::string output = test::runCommand(command);

	std::map<std::string, Expected> expected;
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
	{
		if (line.empty())
			continue;
		std::istringstream fields(line);
		std::string path;
		Expected entry;
		std::string token;
		if (!std::getline(fields, path, '\t') || !std::getline(fields, token, '\t'))
			throw test::Failure("unexpected zone_expected.py line: " + line);
		entry.records = std::stol(token);
		if (!std::getline(fields, token, '\t'))
			throw test::Failure("unexpected zone_expected.py line: " + line);
		entry.version = std::stol(token);
		if (!std::getline(fields, token, '\t'))
			throw test::Failure("unexpected zone_expected.py line: " + line);
		entry.placements = std::stol(token);
		if (!std::getline(fields, token, '\t'))
			throw test::Failure("unexpected zone_expected.py line: " + line);
		entry.deletions = std::stol(token);
		if (!std::getline(fields, token, '\t'))
			throw test::Failure("unexpected zone_expected.py line: " + line);
		entry.inventory = std::stol(token);
		expected.emplace(path, entry);
	}
	return expected;
}

void checkIndexHelpers()
{
	for (int x = 0; x < kworld::kZoneCellsPerAxis; ++x)
	{
		for (int z = 0; z < kworld::kZoneCellsPerAxis; ++z)
		{
			const kworld::ZoneCell cell{ x, z };
			const int index = kworld::zoneIndexOf(cell);
			test::require(index == x + 64 * z, "zone index formula");
			const kworld::ZoneCell roundTrip = kworld::zoneCellOfIndex(index);
			test::require(roundTrip == cell, "zone index round trip");

			const double insideX = kworld::zoneMinX(x) + 0.25;
			const double insideZ = kworld::zoneMinZ(z) + 0.25;
			test::require(kworld::zoneCellOf(insideX, insideZ) == cell,
				"cell origin does not map back");
			test::require(kworld::zoneCellOf(
				kworld::zoneMinX(x) + kworld::kZoneSize - 0.25,
				kworld::zoneMinZ(z) + kworld::kZoneSize - 0.25) == cell,
				"cell upper edge does not map back");
		}
	}
	test::require(kworld::zoneCellOf(-147456.0, -147456.0).x == 0
		&& kworld::zoneCellOf(-147456.0, -147456.0).z == 0, "world corner origin");
	const kworld::ZoneCell last = kworld::zoneCellOf(147455.75, 147455.75);
	test::require(last.x == 63 && last.z == 63, "world corner limit");
}

} // namespace

int main()
{
	try
	{
		const auto zones = kworld::Zones::load(KENSHI_DATA);
		const auto expected = referenceCounts();
		test::require(expected.size() == zones.files().size(),
			"zone file count mismatch: kmod.py " + std::to_string(expected.size())
				+ ", library " + std::to_string(zones.files().size()));

		std::size_t outside = 0;
		for (const auto& file : zones.files())
		{
			const std::string relative = fs::path(file.path)
				.lexically_relative(zones.levelDataRoot()).generic_string();
			const auto found = expected.find(relative);
			test::require(found != expected.end(), "extra zone file " + relative);
			const auto& reference = found->second;
			test::require(reference.records == static_cast<long>(file.recordCount),
				"record count mismatch in " + relative);
			test::require(reference.version == file.version,
				"version mismatch in " + relative);
			test::require(reference.placements == static_cast<long>(file.placementCount),
				"placement count mismatch in " + relative + ": expected "
					+ std::to_string(reference.placements) + ", got "
					+ std::to_string(file.placementCount));
			test::require(reference.deletions == static_cast<long>(file.deletionCount),
				"deletion count mismatch in " + relative);
			test::require(reference.inventory == static_cast<long>(file.inventoryRowCount),
				"inventory row count mismatch in " + relative);

			test::require(file.cellX >= 0 && file.cellX < 64 && file.cellZ >= 0
				&& file.cellZ < 64, "zone cell out of range in " + relative);
			for (const auto& instance : file.instances)
				if (instance.kind == kworld::ZoneInstanceKind::Placement)
				{
					const kworld::ZoneCell cell =
						kworld::zoneCellOf(instance.x, instance.z);
					if (cell.x != file.cellX || cell.z != file.cellZ)
						++outside;
				}
		}
		test::require(outside == 0,
			std::to_string(outside) + " placements outside their file's cell");

		checkIndexHelpers();

		std::cout << "zones: " << zones.files().size() << " files, "
			<< zones.placementCount() << " placements, " << zones.deletionCount()
			<< " deletes, " << zones.inventoryRowCount()
			<< " inventory rows; per-zone counts match kmod ("
			<< expected.size() << " files); index round trips ok; "
			<< outside << " placements outside their cell\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "zones: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
