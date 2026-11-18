// TOWN records from the merged game data and townlist positions/zone cells
// must match the reference kmod.py parse (WORLD_LAYOUT.md "Towns").

#include "kworld/towns.hpp"
#include "kworld_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ExpectedPlacement
{
	std::string layer;
	std::string instanceID;
	std::string targetID;
	std::string stateID;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	int zonesCount = 0;
	int cellX = 0;
	int cellZ = 0;
	std::vector<std::pair<int, int>> declared;
};

struct Expected
{
	std::size_t townCount = 0;
	std::set<std::string> townIDs;
	std::vector<ExpectedPlacement> placements;
};

std::vector<std::string> splitTabs(const std::string& line)
{
	std::vector<std::string> fields;
	std::istringstream stream(line);
	std::string field;
	while (std::getline(stream, field, '\t'))
		fields.push_back(field);
	return fields;
}

Expected referenceData()
{
	const std::string command = "python3 -I "
		+ test::shellQuote(test::pythonScript("towns_expected.py")) + " "
		+ test::shellQuote(test::refTool("kmod.py")) + " "
		+ test::shellQuote(KENSHI_DATA) + " 2>&1";
	const std::string output = test::runCommand(command);

	Expected expected;
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
	{
		if (line.empty())
			continue;
		if (line.starts_with("towns\t"))
		{
			expected.townCount = static_cast<std::size_t>(std::stoul(line.substr(6)));
			continue;
		}
		const auto fields = splitTabs(line);
		if (!fields.empty() && fields[0] == "town" && fields.size() == 2)
		{
			expected.townIDs.insert(fields[1]);
			continue;
		}
		if (!fields.empty() && fields[0] == "placement" && fields.size() == 12)
		{
			ExpectedPlacement placement;
			placement.layer = fields[1];
			placement.instanceID = fields[2];
			placement.targetID = fields[3];
			placement.stateID = fields[4];
			placement.x = std::stod(fields[5]);
			placement.y = std::stod(fields[6]);
			placement.z = std::stod(fields[7]);
			placement.zonesCount = std::stoi(fields[8]);
			placement.cellX = std::stoi(fields[9]);
			placement.cellZ = std::stoi(fields[10]);
			if (fields[11] != "-")
			{
				std::istringstream cells(fields[11]);
				std::string cell;
				while (std::getline(cells, cell, ';'))
				{
					const auto comma = cell.find(',');
					if (comma == std::string::npos)
						throw test::Failure("bad declared cell " + cell);
					placement.declared.emplace_back(std::stoi(cell.substr(0, comma)),
						std::stoi(cell.substr(comma + 1)));
				}
			}
			expected.placements.push_back(std::move(placement));
			continue;
		}
		throw test::Failure("unexpected towns_expected.py line: " + line);
	}
	return expected;
}

int layerRank(const std::string& layer)
{
	if (layer == "Newwworld")
		return 1;
	if (layer == "Dialogue")
		return 2;
	if (layer == "rebirth")
		return 3;
	throw test::Failure("unexpected townlist layer " + layer);
}

// Independent restatement of the Zones::resolveLayers rule: layers in load
// order, a repeated instance id replaces in place, an empty target deletes.
std::vector<ExpectedPlacement> stackLayers(const std::vector<ExpectedPlacement>& all)
{
	std::vector<ExpectedPlacement> sorted = all;
	std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
		return layerRank(a.layer) < layerRank(b.layer);
	});
	std::vector<ExpectedPlacement> result;
	for (const auto& entry : sorted)
	{
		const auto same = std::find_if(result.begin(), result.end(),
			[&](const auto& kept) { return kept.instanceID == entry.instanceID; });
		if (entry.targetID.empty())
		{
			if (same != result.end())
				result.erase(same);
		}
		else if (same != result.end())
		{
			*same = entry;
		} else {
			result.push_back(entry);
		}
	}
	return result;
}

std::string declaredKey(const std::vector<std::pair<int, int>>& cells)
{
	if (cells.empty())
		return "-";
	std::string out;
	for (const auto& cell : cells)
	{
		if (!out.empty())
			out += ';';
		out += std::to_string(cell.first) + "," + std::to_string(cell.second);
	}
	return out;
}

} // namespace

int main()
{
	try
	{
		const auto towns = kworld::Towns::load(KENSHI_DATA);
		const auto expected = referenceData();

		test::require(towns.towns().size() == expected.townCount,
			"TOWN count mismatch: expected " + std::to_string(expected.townCount)
				+ ", got " + std::to_string(towns.towns().size()));
		std::set<std::string> actualIDs;
		for (const auto& town : towns.towns())
			actualIDs.insert(town.stringID);
		test::require(actualIDs == expected.townIDs, "TOWN stringIDs differ");

		const auto stacked = stackLayers(expected.placements);
		test::require(stacked.size() == 304 && expected.placements.size() == 383,
			"shipped townlist: expected 383 entries stacking to 304");
		test::require(towns.placements().size() == stacked.size(),
			"townlist placement count mismatch");
		for (std::size_t i = 0; i < stacked.size(); ++i)
		{
			const auto& wanted = stacked[i];
			const auto& actual = towns.placements()[i];
			const std::string label = "townlist entry " + std::to_string(i);
			test::require(actual.layer == wanted.layer && actual.instanceID
				== wanted.instanceID && actual.targetID == wanted.targetID
				&& actual.stateID == wanted.stateID, label + " identity");
			test::require(std::abs(actual.x - wanted.x) < 1e-6
				&& std::abs(actual.y - wanted.y) < 1e-6
				&& std::abs(actual.z - wanted.z) < 1e-6, label + " position");
			test::require(actual.zonesCount == wanted.zonesCount
				&& actual.cell.x == wanted.cellX && actual.cell.z == wanted.cellZ,
				label + " zone data");
			test::require(declaredKey(actual.declaredCells) == declaredKey(wanted.declared),
				label + " declared cells");
		}

		std::set<std::string> targets;
		for (const auto& placement : towns.placements())
			targets.insert(placement.targetID);
		std::size_t linked = 0;
		std::size_t indexed = 0;
		for (const auto& town : towns.towns())
		{
			if (!town.placementIndices.empty())
				++linked;
			indexed += town.placementIndices.size();
		}
		test::require(indexed == towns.placements().size(),
			"not every townlist entry is linked to a TOWN record");
		test::require(towns.townsWithoutPlacement() == expected.townCount - linked,
			"townsWithoutPlacement mismatch");
		test::require(targets.size() == linked,
			"distinct townlist targets differ from linked TOWN records");

		std::cout << "towns: " << towns.towns().size() << " TOWN records, "
			<< towns.placements().size() << " townlist placements ("
			<< targets.size() << " targets, " << towns.townsWithoutPlacement()
			<< " towns without a position); positions/cells match kmod\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "towns: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
