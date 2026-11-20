// Instance orientation against the original game's batch instances at
// Sho-Battai (world x ~ +53.9k, z ~ -93.6k; slope-align skeleton parts and
// tumbling junk/grass; measured by kinspect 2026-10-31,
// tests/data/foliage_tumble_shobattai.json). Matched by mesh stem and decoded
// position; quaternions compared up to sign (q == -q).

#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "kworld_test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Measured
{
	std::string mesh;
	double x = 0.0;
	double z = 0.0;
	double q[4] = {}; // w, x, y, z
};

// Pretty-printed findings: "mesh" opens an entry, "ori_wxyz" is a 4-line array.
std::vector<Measured> loadFixture(const std::string& path)
{
	std::ifstream in(path);
	test::require(in.good(), "cannot open " + path);
	std::vector<Measured> out;
	std::string line;
	int quatIndex = -1;
	while (std::getline(in, line))
	{
		const std::size_t colon = line.find("\":");
		if (colon != std::string::npos)
		{
			const std::string key = line.substr(line.find('"') + 1, colon - line.find('"') - 1);
			const std::string text = line.substr(line.find_first_not_of(' ', colon + 2));
			if (key == "mesh")
			{
				out.emplace_back();
				out.back().mesh = text.substr(1, text.find('"', 1) - 1);
			}
			else if (key == "x") out.back().x = std::atof(text.c_str());
			else if (key == "z") out.back().z = std::atof(text.c_str());
			else if (key == "ori_wxyz") quatIndex = 0;
		} else if (quatIndex >= 0 && quatIndex < 4 && line.find_first_of("-0123456789") != std::string::npos)
			out.back().q[quatIndex++] = std::atof(line.c_str());
	}
	return out;
}

// "Human_Skeleton_Part13.mesh" -> "Human_Skeleton_Part13".
bool recordHasStem(const kdata::Record& record, const std::string& mesh)
{
	const std::string stem = mesh.substr(0, mesh.rfind('.'));
	for (const auto& file : record.files)
		if (file.value.find(stem + ".") != std::string::npos)
			return true;
	return false;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::exists(std::string(KWORLD_SOURCE_DIR) + "/tests/data/foliage_tumble_shobattai.json"))
		{
			std::cout << "kworld_foliage_orient: SKIPPED: the captured fixture is not in this tree (game-derived, not shipped)\n";
			return 77;
		}
		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto terrain = kworld::Heightmap::open(test::dataPath("newland/land/fullmap.tif"));
		const auto biomes = kworld::BiomeMap::load(test::dataPath("newland/land/biomemap.png"));
		const auto features = kworld::Features::join(gameData, KENSHI_DATA);
		const auto towns = kworld::Towns::join(gameData, KENSHI_DATA);

		const auto measured = loadFixture(std::string(KWORLD_SOURCE_DIR) + "/tests/data/foliage_tumble_shobattai.json");
		test::require(measured.size() == 1243, "1243 instances in the fixture");

		// The zones under the fixture's bounding box.
		const kworld::ZoneCell low = kworld::zoneCellOf(50700.0, -96800.0);
		const kworld::ZoneCell high = kworld::zoneCellOf(57700.0, -89900.0);
		std::vector<kworld::FoliageInstance> ours;
		for (int cz = low.z; cz <= high.z; ++cz)
			for (int cx = low.x; cx <= high.x; ++cx)
				for (auto& instance : kworld::generateFoliage({ cx, cz }, terrain, biomes, gameData, features, towns))
					ours.push_back(std::move(instance));

		std::size_t matched = 0;
		std::size_t missing = 0;
		double maxError = 0.0;
		double sumError = 0.0;
		// [0] slope align, [1] tumble (not keep upright), [2] yaw only: count, max.
		std::size_t kindCount[3] = {};
		double kindMax[3] = {};
		std::size_t exact = 0; // within 0.05 deg
		std::size_t far = 0; // beyond 3 deg
		for (const Measured& plant : measured)
		{
			// Several plants of one mesh can share a position (within 0.5):
			// take the candidate whose orientation is closest.
			const kworld::FoliageInstance* best = nullptr;
			double degrees = 1.0e9;
			for (const auto& instance : ours)
			{
				if (std::hypot(instance.x - plant.x, instance.z - plant.z) >= 0.5)
					continue;
				const kdata::Record* record = gameData.find(instance.recordID);
				if (record == nullptr || !recordHasStem(*record, plant.mesh))
					continue;
				const double dot = std::fabs(instance.qw * plant.q[0] + instance.qx * plant.q[1]
					+ instance.qy * plant.q[2] + instance.qz * plant.q[3]);
				const double angle = 2.0 * std::acos(std::min(dot, 1.0)) * 180.0 / 3.14159265358979323846;
				if (angle < degrees)
				{
					best = &instance;
					degrees = angle;
				}
			}
			if (best == nullptr)
			{
				++missing;
				continue;
			}
			++matched;
			const kdata::Record& record = *gameData.find(best->recordID);
			const int kind = record.boolOr("slope align", false) ? 0
				: record.boolOr("keep upright", true) ? 2 : 1;
			++kindCount[kind];
			kindMax[kind] = std::max(kindMax[kind], degrees);
			maxError = std::max(maxError, degrees);
			sumError += degrees;
			exact += degrees <= 0.05;
			if (degrees > 3.0)
			{
				++far;
				std::cout << "far " << plant.mesh << " at " << plant.x << "," << plant.z << " by " << degrees << " deg; ours " << best->qw << " " << best->qx << " " << best->qy << " " << best->qz
					<< " game " << plant.q[0] << " " << plant.q[1] << " " << plant.q[2] << " " << plant.q[3] << "\n";
			}
		}
		std::cout << "slope align " << kindCount[0] << " max " << kindMax[0] << " deg; tumble " << kindCount[1]
			<< " max " << kindMax[1] << " deg; yaw only " << kindCount[2] << " max " << kindMax[2] << " deg\n";
		std::cout << "instances " << measured.size() << ": matched " << matched << ", missing " << missing
			<< ", within 0.05 deg " << exact << ", beyond 3 deg " << far << "; orientation error mean " << sumError / std::max<std::size_t>(matched, 1)
			<< " deg, max " << maxError << " deg\n";

		test::require(matched * 100 >= measured.size() * 99, "at least 99% of measured instances found in our output");
		// Tumble is exact; slope-align uses the exe's fixed lattice triangle
		// (1407ea850). Three of 1243 differ: two game plants share a position
		// and mesh with another plant and have no counterpart in our output.
		test::require(kindMax[1] < 0.1, "tumble orientation differs from the game by more than 0.1 deg");
		test::require(far <= 2, "more than 2 orientations are more than 3 deg off");
		test::require(exact + 3 >= matched, "more than 3 orientations differ from the game by 0.05 deg");
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld foliage orient: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld foliage orient: ok\n";
	return 0;
}
