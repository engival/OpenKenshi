// Prints the same summary lines as tools/world_survey.py so the two
// can be diffed. Usage: kworld_survey [KENSHI_DATA_ROOT]

#include "kworld/towns.hpp"
#include "kworld/zones.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

struct CountedKey
{
	std::string key;
	std::size_t value = 0;
};

void bump(std::vector<CountedKey>& counts, const std::string& key)
{
	for (auto& entry : counts)
		if (entry.key == key)
		{
			++entry.value;
			return;
		}
	counts.push_back({ key, 1 });
}

void add(std::vector<kworld::ZoneTypeCount>& counts,
	const std::string& key, std::size_t amount)
{
	for (auto& entry : counts)
		if (entry.type == key)
		{
			entry.count += amount;
			return;
		}
	counts.push_back({ key, amount });
}

std::string printCounts(const std::vector<CountedKey>& counts)
{
	std::string out = "{";
	bool first = true;
	for (const auto& entry : counts)
	{
		if (!first)
			out += ", ";
		first = false;
		out += "'" + entry.key + "': " + std::to_string(entry.value);
	}
	return out + "}";
}

std::string printTypeCounts(const std::vector<kworld::ZoneTypeCount>& counts)
{
	std::string out = "{";
	bool first = true;
	for (const auto& entry : counts)
	{
		if (!first)
			out += ", ";
		first = false;
		out += "'" + entry.type + "': " + std::to_string(entry.count);
	}
	return out + "}";
}

struct VersionCount
{
	std::string layer;
	int version = 0;
	std::size_t count = 0;
};

struct LayerTypes
{
	std::string layer;
	std::vector<kworld::ZoneTypeCount> types;
};

} // namespace

int main(int argc, char** argv)
{
	if (argc > 2)
	{
		std::cerr << "usage: kworld_survey [KENSHI_DATA_ROOT]\n";
		return 2;
	}
	const std::string dataRoot = argc == 2 ? argv[1] : std::string(KENSHI_DATA);

	try
	{
		using Clock = std::chrono::steady_clock;
		const auto start = Clock::now();
		const auto zones = kworld::Zones::load(dataRoot);
		const auto zonesEnd = Clock::now();
		const auto levelFiles = kworld::loadLevelDataFiles(dataRoot);
		const auto levelEnd = Clock::now();

		std::vector<VersionCount> versions;
		std::vector<CountedKey> trailing;
		std::vector<LayerTypes> layers;
		std::vector<CountedKey> instances;
		std::set<std::pair<int, int>> cells;
		int minCellX = 0;
		int maxCellX = 0;
		int minCellZ = 0;
		int maxCellZ = 0;
		std::size_t inside = 0;
		std::size_t outside = 0;

		for (const auto& file : zones.files())
		{
			bool foundVersion = false;
			for (auto& entry : versions)
				if (entry.layer == file.layer && entry.version == file.version)
				{
					++entry.count;
					foundVersion = true;
					break;
				}
			if (!foundVersion)
				versions.push_back({ file.layer, file.version, 1 });

			bump(trailing, file.hasTrailingBlock() ? "present" : "absent");
			if (cells.empty())
			{
				minCellX = maxCellX = file.cellX;
				minCellZ = maxCellZ = file.cellZ;
			}
			else
			{
				minCellX = std::min(minCellX, file.cellX);
				maxCellX = std::max(maxCellX, file.cellX);
				minCellZ = std::min(minCellZ, file.cellZ);
				maxCellZ = std::max(maxCellZ, file.cellZ);
			}
			cells.emplace(file.cellX, file.cellZ);

			LayerTypes* layer = nullptr;
			for (auto& entry : layers)
				if (entry.layer == file.layer)
				{
					layer = &entry;
					break;
				}
			if (layer == nullptr)
			{
				layers.push_back({ file.layer, {} });
				layer = &layers.back();
			}
			for (const auto& type : file.typeCounts)
				add(layer->types, type.type, type.count);

			for (const auto& instance : file.instances)
			{
				using Kind = kworld::ZoneInstanceKind;
				if (instance.kind == Kind::Deleted)
					bump(instances, "deleted");
				else if (instance.kind == Kind::InventoryRow)
					bump(instances, "inventory row");
				else
				{
					bump(instances, "placement");
					const kworld::ZoneCell cell =
						kworld::zoneCellOf(instance.x, instance.z);
					if (cell.x == file.cellX && cell.z == file.cellZ)
						++inside;
					else
						++outside;
				}
			}
		}

		std::cout << "zone files by (layer, version): {";
		for (std::size_t i = 0; i < versions.size(); ++i)
		{
			if (i != 0)
				std::cout << ", ";
			std::cout << "('" << versions[i].layer << "', " << versions[i].version
				<< "): " << versions[i].count;
		}
		std::cout << "}\n";
		std::cout << "trailing blocks: " << printCounts(trailing) << "\n";
		for (const auto& layer : layers)
			std::cout << "records [" << layer.layer << "]: "
				<< printTypeCounts(layer.types) << "\n";
		std::cout << "instances: " << printCounts(instances) << "\n";
		std::cout << "placements inside the cell named by their file: " << inside
			<< ", outside: " << outside << "\n";
		if (cells.empty())
			std::cout << "0 distinct cells\n";
		else
			std::cout << cells.size() << " distinct cells, X " << minCellX << ".."
				<< maxCellX << ", Y " << minCellZ << ".." << maxCellZ << "\n";

		for (const auto& file : levelFiles)
		{
			std::size_t okay = 0;
			std::size_t total = 0;
			for (const auto& placement : file.placements)
			{
				if (placement.zonesCount == 0)
					continue;
				++total;
				bool found = false;
				for (const auto& declared : placement.declaredCells)
					if (declared.first == placement.cell.x
						&& declared.second == placement.cell.z)
					{
						found = true;
						break;
					}
				if (found)
					++okay;
			}
			std::cout << file.layer << "/leveldata.level: " << file.recordCount
				<< " records, " << file.townStateCount << " GAMESTATE_TOWN, "
				<< file.roadCount << " ROAD, towns inside declared cells "
				<< okay << "/" << total << "\n";
		}

		const auto end = Clock::now();
		const auto seconds = [](Clock::duration duration) {
			return std::chrono::duration<double>(duration).count();
		};
		std::cerr << "kworld_survey: zones " << seconds(zonesEnd - start)
			<< " s, leveldata " << seconds(levelEnd - zonesEnd) << " s, total "
			<< seconds(end - start) << " s\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld_survey: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
