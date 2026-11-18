// Procedural foliage (docs/reference/FOLIAGE_EXE.md): the exe's RNG and seed
// against hand-computed values, determinism (also across threads), and
// plausibility on real cells.

#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"
#include "kworld_test_util.hpp"

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

struct SeedCase
{
	int cellX;
	int cellZ;
	int layerID;
	std::uint32_t seed;
	std::array<std::uint32_t, 5> outputs;
	std::array<float, 3> yaws; // uniform(0, 360) of the first three outputs
};

// Computed by an independent Python implementation of FOLIAGE_EXE.md section 2
// (its MT19937 core also reproduces the standard seed-5489 first output).
const SeedCase kSeedCases[] = {
	{ 0, 0, 1452, 4286388281u,
		{ 0xa10d8394u, 0x26f7189cu, 0xf9928a63u, 0xe1abc982u, 0xefe1c051u },
		{ 226.48048400878906f, 54.794837951660156f, 350.9612121582031f } },
	{ 40, 24, 2721, 16329003u,
		{ 0x26d3b6bcu, 0xc57a0715u, 0x967cc5dcu, 0x8028bd98u, 0x3143da4fu },
		{ 54.60047912597656f, 277.7015686035156f, 211.62289428710938f } },
	{ 63, 63, 5748, 7136943u,
		{ 0xb28e6176u, 0x9d5ed690u, 0x5bb33353u, 0x327914c3u, 0x6f16d4a4u },
		{ 251.09461975097656f, 221.30221557617188f, 128.953125f } },
};

void checkRng()
{
	for (const SeedCase& c : kSeedCases)
	{
		const std::uint32_t seed = kworld::foliageLayerSeed(
			static_cast<int>(kworld::zoneMinX(c.cellX)),
			static_cast<int>(kworld::zoneMinZ(c.cellZ)), c.layerID);
		test::require(seed == c.seed, "seed of cell " + std::to_string(c.cellX)
			+ "," + std::to_string(c.cellZ));
		kworld::FoliageRng rng(seed);
		for (std::size_t i = 0; i < c.outputs.size(); ++i)
			test::require(rng.next() == c.outputs[i], "tempered output " + std::to_string(i));

		kworld::FoliageRng again(seed);
		for (const float yaw : c.yaws)
			test::require(again.uniform(0.0f, 360.0f) == yaw, "uniform(0, 360)");
	}

	// Integer draws: masked rejection. Max 5 accepts every draw of this seed;
	// max 2 rejects the third (0x63 & 3 == 3).
	const std::uint32_t seed = kworld::foliageLayerSeed(
		static_cast<int>(kworld::zoneMinX(0)), static_cast<int>(kworld::zoneMinZ(0)), 1452);
	kworld::FoliageRng five(seed);
	for (const std::uint32_t expected : { 4u, 4u, 3u, 2u })
		test::require(five.uniformInt(5) == expected, "uniformInt(5)");
	kworld::FoliageRng two(seed);
	for (const std::uint32_t expected : { 0u, 0u, 2u, 1u })
		test::require(two.uniformInt(2) == expected, "uniformInt(2)");
	kworld::FoliageRng zero(seed);
	test::require(zero.uniformInt(0) == 0, "uniformInt(0)");

	test::require(kworld::leadingRecordID("1452-vurt.mod") == 1452, "leading id");
	test::require(kworld::leadingRecordID("1477-D-vurt.mod") == 1477, "leading id, D");
	test::require(kworld::leadingRecordID("abc") == 0, "no leading id");
}

bool same(const kworld::FoliageInstance& a, const kworld::FoliageInstance& b)
{
	return a.recordID == b.recordID && a.x == b.x && a.y == b.y && a.z == b.z
		&& a.qw == b.qw && a.qx == b.qx && a.qy == b.qy && a.qz == b.qz
		&& a.yawDegrees == b.yawDegrees && a.scale == b.scale && a.isChild == b.isChild;
}

bool same(const std::vector<kworld::FoliageInstance>& a,
	const std::vector<kworld::FoliageInstance>& b)
{
	if (a.size() != b.size())
		return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (!same(a[i], b[i]))
			return false;
	return true;
}

} // namespace

int main()
{
	try
	{
		checkRng();

		const auto gameData = kworld::loadMergedGameData(KENSHI_DATA);
		const auto terrain = kworld::Heightmap::open(test::dataPath("newland/land/fullmap.tif"));
		const auto biomes = kworld::BiomeMap::load(test::dataPath("newland/land/biomemap.png"));
		const auto features = kworld::Features::join(gameData, KENSHI_DATA);
		const auto towns = kworld::Towns::join(gameData, KENSHI_DATA);
		test::require(biomes.width() == 1024 && biomes.height() == 1024, "biomemap size");

		// A cell holding a road-foliage eraser, plus a spread of others.
		std::vector<kworld::ZoneCell> cells = { { 31, 31 }, { 20, 40 }, { 45, 20 } };
		for (const auto& point : features.roadFoliage())
			if (!point.deleted && point.u > 100.0)
			{
				cells.push_back(kworld::zoneCellOf(point.x, point.z));
				break;
			}

		std::size_t nonEmpty = 0;
		for (const kworld::ZoneCell cell : cells)
		{
			const auto first = kworld::generateFoliage(cell, terrain, biomes, gameData, features, towns);
			const auto second = kworld::generateFoliage(cell, terrain, biomes, gameData, features, towns);
			test::require(same(first, second), "two calls differ");

			std::vector<std::vector<kworld::FoliageInstance>> parallel(4);
			std::vector<std::thread> threads;
			for (auto& slot : parallel)
				threads.emplace_back([&] {
					slot = kworld::generateFoliage(cell, terrain, biomes, gameData, features, towns);
				});
			for (auto& thread : threads)
				thread.join();
			for (const auto& slot : parallel)
				test::require(same(first, slot), "threaded call differs");

			// Parents lie inside the cell; children may spill past it by their
			// scatter radius but never past the world.
			const double x0 = kworld::zoneMinX(cell.x);
			const double z0 = kworld::zoneMinZ(cell.z);
			std::size_t children = 0;
			for (const auto& instance : first)
			{
				test::require(instance.x >= x0 && instance.x <= x0 + kworld::kZoneSize
					&& instance.z >= z0 && instance.z <= z0 + kworld::kZoneSize,
					"instance outside its cell");
				test::require(instance.scale >= 0.01f && instance.scale <= 10.0f, "scale range");
				const double norm = instance.qw * instance.qw + instance.qx * instance.qx
					+ instance.qy * instance.qy + instance.qz * instance.qz;
				test::require(std::fabs(norm - 1.0) < 1e-9, "unit quaternion");
				children += instance.isChild;
				for (const auto& point : features.roadFoliage())
				{
					if (point.deleted || point.u <= 0.0)
						continue;
					const double dx = point.x - instance.x;
					const double dz = point.z - instance.z;
					test::require(dx * dx + dz * dz >= point.u * point.u,
						"instance inside an eraser disc");
				}
			}
			nonEmpty += !first.empty();
			std::cout << "cell " << cell.x << "," << cell.z << ": " << first.size()
				<< " instances (" << children << " children)\n";
		}
		test::require(nonEmpty >= 2, "too few cells produced foliage");

		// The Hub: rebirth.mod zeroes its `no-foliage range`, which the exe
		// reads literally; with the 2000 default it clears 1500 units around it.
		const double hubX = -50978.8;
		const double hubZ = 2932.48;
		const auto near = [&](bool zeroIsDefault) {
			kworld::FoliageParams params;
			params.zeroTownRangeIsDefault = zeroIsDefault;
			std::size_t count = 0;
			for (const auto& instance : kworld::generateFoliage(kworld::zoneCellOf(hubX, hubZ),
				terrain, biomes, gameData, features, towns, params))
				count += std::hypot(instance.x - hubX, instance.z - hubZ) < 1500.0;
			return count;
		};
		test::require(near(false) > 0 && near(true) == 0, "town no-foliage range around The Hub");

		test::require(kworld::generateFoliage({ -1, 0 }, terrain, biomes, gameData, features,
			towns).empty(), "cell outside the world");
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld foliage: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld foliage: rng, determinism and plausibility ok\n";
	return 0;
}
