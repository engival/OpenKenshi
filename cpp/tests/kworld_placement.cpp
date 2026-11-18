// Placed instances: terrain-relative y converted to absolute, BUILDING scale
// resolved from merged records, and the COLLISION_FORMAT.md section 4 example.

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/zones.hpp"
#include "kworld_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace {

std::map<std::string, std::string> referenceValues()
{
	const std::string command = "python3 -I "
		+ test::shellQuote(test::pythonScript("placement_expected.py")) + " "
		+ test::shellQuote(test::refTool("kmod.py")) + " "
		+ test::shellQuote(KENSHI_DATA) + " 2>&1";
	const std::string output = test::runCommand(command);

	std::map<std::string, std::string> values;
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
	{
		if (line.empty())
			continue;
		const auto space = line.find(' ');
		if (space == std::string::npos)
			continue;
		values[line.substr(0, space)] = line.substr(space + 1);
	}
	return values;
}

long number(const std::map<std::string, std::string>& values, const char* key)
{
	const auto found = values.find(key);
	if (found == values.end())
		throw test::Failure(std::string("reference output has no ") + key);
	return std::stol(found->second);
}

double real(const std::map<std::string, std::string>& values, const char* key)
{
	const auto found = values.find(key);
	if (found == values.end())
		throw test::Failure(std::string("reference output has no ") + key);
	return std::stod(found->second);
}

} // namespace

int main()
{
	try
	{
		const auto zones = kworld::Zones::load(KENSHI_DATA);
		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto heightmap = kworld::Heightmap::open(
			test::dataPath("newland/land/fullmap.tif"));
		const auto placed = zones.place(heightmap, gameData);
		const auto expected = referenceValues();

		test::require(placed.size() == static_cast<std::size_t>(
			number(expected, "placements")), "placement count mismatch");

		std::size_t withScale = 0;
		std::size_t outsideCell = 0;
		std::size_t nonUnit = 0;
		double worstRecompute = 0.0;
		std::set<std::string> missingTargets;
		for (const auto& entry : placed)
		{
			if (entry.hasScale)
				++withScale;
			else
				missingTargets.insert(entry.buildingID);
			if (entry.positionCell.x != entry.cell.x
				|| entry.positionCell.z != entry.cell.z)
				++outsideCell;
			const double norm = std::sqrt(entry.qw * entry.qw + entry.qx * entry.qx
				+ entry.qy * entry.qy + entry.qz * entry.qz);
			if (std::abs(norm - 1.0) > 2e-3)
				++nonUnit;
			// zoneBuildingY: node y + max(sea plane, terrain), or node y + terrain for a door building.
			const double terrain = heightmap.height(entry.x, entry.z);
			worstRecompute = std::max(worstRecompute, std::min(std::abs(entry.relativeY + std::max(kworld::kSeaPlane, terrain) - entry.y),
				std::abs(entry.relativeY + terrain - entry.y)));
		}
		test::require(withScale == static_cast<std::size_t>(number(expected, "resolved")),
			"resolved scale count mismatch");
		test::require(placed.size() - withScale == static_cast<std::size_t>(
			number(expected, "missing_placements")), "missing-scale count mismatch");
		test::require(missingTargets.size() == static_cast<std::size_t>(
			number(expected, "missing_targets")), "missing-target id count mismatch");
		test::require(outsideCell == 0,
			std::to_string(outsideCell) + " placements outside their file's cell");
		test::require(nonUnit == 0, std::to_string(nonUnit) + " non-unit quaternions");
		test::require(worstRecompute < 1e-9, "absolute y is not relative y + max(100, terrain)");

		// COLLISION_FORMAT.md section 4 worked example.
		const kworld::PlacedInstance* example = nullptr;
		for (const auto& entry : placed)
			if (entry.instanceID == "2-rebirth-INGAME" && entry.layer == "Newwworld"
				&& entry.cell.x == 5 && entry.cell.z == 44)
				example = &entry;
		test::require(example != nullptr, "COLLISION_FORMAT.md example not found");
		test::require(example->buildingID == "50625-Newwworld.mod",
			"example building target mismatch");
		test::require(std::abs(example->x - real(expected, "example_x")) < 1e-6
			&& std::abs(example->z - real(expected, "example_z")) < 1e-6,
			"example position mismatch");
		test::require(std::abs(example->relativeY - real(expected, "example_y")) < 1e-6,
			"example relative y mismatch");
		test::require(std::abs(static_cast<double>(example->scale)
			- real(expected, "example_scale")) < 1e-6, "example scale mismatch");
		const double exampleTerrain = heightmap.height(example->x, example->z);
		test::require(std::abs(exampleTerrain - 1948.9) < 0.2,
			"example terrain height differs from the documented 1,948.9: "
				+ std::to_string(exampleTerrain));
		test::require(std::abs(example->y - 1961.6) < 0.2,
			"example absolute y differs from the documented value: "
				+ std::to_string(example->y));

		std::cout << "placement: " << placed.size() << " placed, " << withScale
			<< " with scale, " << placed.size() - withScale << " missing targets ("
			<< missingTargets.size() << " ids); all in their file cell; quaternions unit; "
			<< "example 2-rebirth-INGAME terrain " << exampleTerrain << " abs y "
			<< example->y << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "placement: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
