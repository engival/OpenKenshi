// Foliage generator against plants measured live in the running original
// game (Hub, Day 1 10:14, 400 units around (-50893, 3418); measured by
// kinspect 2026-10-31, tests/data/foliage_hub_10-14.json). The game lists a
// plant once per mesh part (Trunk, Leaves); entries are deduped by position.

#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "kworld_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr double kCentreX = -50893.0;
constexpr double kCentreZ = 3418.0;
constexpr double kRadius = 400.0;
constexpr double kPage = 2304.0;

struct Measured
{
	std::string mesh;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	int u = 0;
	int v = 0;
	int yawByte = 0;
	int scaleByte = 0;
	int hashX = 0;
	int hashZ = 0;
};

// The findings file is pretty-printed JSON with one key per line; "mesh"
// opens each entry and "hash_in" closes it.
std::vector<Measured> loadFixture(const std::string& path)
{
	std::ifstream in(path);
	test::require(in.good(), "cannot open " + path);
	std::vector<Measured> out;
	std::string line;
	bool inHash = false;
	int hashIndex = 0;
	while (std::getline(in, line))
	{
		const std::size_t colon = line.find("\":");
		const std::size_t open = line.find('"');
		const std::string value = colon == std::string::npos ? line : line.substr(colon + 2);
		if (colon != std::string::npos)
		{
			const std::string key = line.substr(open + 1, colon - open - 1);
			const std::string text = value.substr(value.find_first_not_of(' '));
			if (key == "mesh")
			{
				out.emplace_back();
				out.back().mesh = text.substr(1, text.find('"', 1) - 1);
			}
			else if (key == "x") out.back().x = std::atof(text.c_str());
			else if (key == "y") out.back().y = std::atof(text.c_str());
			else if (key == "z") out.back().z = std::atof(text.c_str());
			else if (key == "u") out.back().u = std::atoi(text.c_str());
			else if (key == "v") out.back().v = std::atoi(text.c_str());
			else if (key == "yaw_byte") out.back().yawByte = std::atoi(text.c_str());
			else if (key == "scale_byte") out.back().scaleByte = std::atoi(text.c_str());
			else if (key == "hash_in") { inHash = true; hashIndex = 0; }
		} else if (inHash && line.find_first_of("-0123456789") != std::string::npos)
		{
			(hashIndex++ == 0 ? out.back().hashX : out.back().hashZ) = std::atoi(line.c_str());
			if (hashIndex == 2)
				inHash = false;
		}
	}
	return out;
}

// "Plant_Fluffyhead01_Trunk.mesh" -> "Plant_Fluffyhead01".
std::string stemOf(const std::string& mesh)
{
	std::string stem = mesh.substr(0, mesh.rfind('.'));
	for (const char* suffix : { "_Trunk", "-Trunk", "_Leaves", "-Leaves" })
	{
		const std::string tail = suffix;
		if (stem.size() > tail.size() && stem.compare(stem.size() - tail.size(), tail.size(), tail) == 0)
			stem.resize(stem.size() - tail.size());
	}
	return stem;
}

// Whether a FOLIAGE_MESH record points at a mesh or collision file of this stem.
bool recordHasStem(const kdata::Record& record, const std::string& stem)
{
	for (const auto& file : record.files)
		if (file.value.find(stem) != std::string::npos)
			return true;
	return false;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::exists(std::string(KWORLD_SOURCE_DIR) + "/tests/data/foliage_hub_10-14.json"))
		{
			std::cout << "kworld_foliage_live: SKIPPED: the captured fixture is not in this tree (game-derived, not shipped)\n";
			return 77;
		}
		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto terrain = kworld::Heightmap::open(test::dataPath("newland/land/fullmap.tif"));
		const auto biomes = kworld::BiomeMap::load(test::dataPath("newland/land/biomemap.png"));
		const auto features = kworld::Features::join(gameData, KENSHI_DATA);
		const auto towns = kworld::Towns::join(gameData, KENSHI_DATA);

		// Deduped plants (the trunk entry stands for both parts).
		std::vector<Measured> plants;
		for (const Measured& entry : loadFixture(std::string(KWORLD_SOURCE_DIR) + "/tests/data/foliage_hub_10-14.json"))
			if (std::none_of(plants.begin(), plants.end(), [&](const Measured& p) {
					return p.x == entry.x && p.z == entry.z; }))
				plants.push_back(entry);
		test::require(plants.size() == 29, "29 distinct plants in the fixture");

		// The Hub zone and its neighbours (one plant lies in the next zone east).
		const kworld::ZoneCell hub = kworld::zoneCellOf(kCentreX, kCentreZ);
		std::vector<kworld::FoliageInstance> ours;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				for (auto& instance : kworld::generateFoliage({ hub.x + dx, hub.z + dz },
					terrain, biomes, gameData, features, towns))
					if (std::hypot(instance.x - kCentreX, instance.z - kCentreZ) < kRadius)
						ours.push_back(std::move(instance));

		std::size_t exact = 0;
		std::size_t close = 0;
		std::size_t missing = 0;
		std::vector<bool> used(ours.size(), false);
		double residualSum = 0.0;
		double residualMax = 0.0;
		std::size_t residualCount = 0;
		for (const Measured& plant : plants)
		{
			const std::string stem = stemOf(plant.mesh);
			std::size_t best = ours.size();
			double bestDistance = 1.0e9;
			for (std::size_t i = 0; i < ours.size(); ++i)
			{
				const kdata::Record* record = gameData.find(ours[i].recordID);
				if (record == nullptr || !recordHasStem(*record, stem))
					continue;
				const double distance = std::hypot(ours[i].x - plant.x, ours[i].z - plant.z);
				if (distance < bestDistance)
				{
					best = i;
					bestDistance = distance;
				}
			}
			if (best == ours.size() || bestDistance > 5.0)
			{
				++missing;
				std::cout << "missing " << plant.mesh << " at " << plant.x << "," << plant.z << "\n";
				continue;
			}
			used[best] = true;
			const kworld::FoliageInstance& mine = ours[best];
			const double left = kworld::zoneMinX(kworld::zoneCellOf(mine.x, mine.z).x);
			const double top = kworld::zoneMinZ(kworld::zoneCellOf(mine.x, mine.z).z);
			const auto offsetU = [](double world, double origin) {
				const double d = world - origin;
				return static_cast<int>(std::lround((d - std::floor(d / kPage) * kPage) / kPage * 65535.0));
			};
			const bool sameU = offsetU(mine.x, left) == plant.u && offsetU(mine.z, top) == plant.v;
			const bool sameYaw = std::lround(mine.yawDegrees * 255.0 / 360.0) == plant.yawByte;
			const bool sameScale = std::lround((mine.scale - 0.01) * 255.0 / 10.0) == plant.scaleByte;
			const bool sameHash = static_cast<int>(std::floor(mine.x)) == plant.hashX
				&& static_cast<int>(std::floor(mine.z)) == plant.hashZ;
			if (sameU && sameYaw && sameScale && sameHash)
			{
				++exact;
				residualSum += mine.y - plant.y;
				residualMax = std::max(residualMax, std::fabs(mine.y - plant.y));
				++residualCount;
			} else {
				++close;
				std::cout << "close " << plant.mesh << " at " << plant.x << "," << plant.z
					<< " d=" << bestDistance << " u/v " << sameU << " yaw " << sameYaw
					<< " scale " << sameScale << " hash " << sameHash << "\n";
			}
		}

		// Ours inside the radius that the game does not have (any mesh).
		std::size_t extra = 0;
		for (std::size_t i = 0; i < ours.size(); ++i)
			if (!used[i])
			{
				++extra;
				if (const kdata::Record* r = gameData.find(ours[i].recordID))
				{
					std::cout << "  " << r->name << " road avoidance " << r->floatOr("road avoidance", -1.0f)
						<< " scale " << ours[i].scale << "\n";
				}
				std::cout << "extra " << ours[i].recordID << " at " << ours[i].x << ","
					<< ours[i].z << (ours[i].isChild ? " (child)" : "") << "\n";
			}
		std::cout << "plants " << plants.size() << ": exact " << exact << ", close " << close
			<< ", missing " << missing << "; ours in radius " << ours.size() << ", extra " << extra << "\n";
		if (residualCount != 0)
			std::cout << "y residual (ours - game) over exact matches: mean "
				<< residualSum / residualCount << ", max |" << residualMax << "|\n";

		// Hard floor: positions and quantised values must reproduce for every
		// plant that has a counterpart; the rest is reported above.
		test::require(exact + close > 0, "no measured plant found in our output");
		test::require(close == 0, "a located plant differs in u/v, yaw, scale or tumble hash");
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld foliage live: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld foliage live: ok\n";
	return 0;
}
