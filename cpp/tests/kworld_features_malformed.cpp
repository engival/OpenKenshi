// Truncated and corrupted features.dat (MF01) must throw, never crash. Also
// checks the synthetic round trip, the road foliage overlay and record
// resolution. Built a second time against ASan/UBSan-instrumented libraries.
// Synthetic data only.

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/features.hpp"
#include "kworld/gamedata.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

template <typename F>
void requireThrows(F&& action, const std::string& label)
{
	try
	{
		action();
	}
	catch (const std::exception&)
	{
		return;
	}
	catch (...)
	{
		throw test::Failure(label + ": non-standard exception");
	}
	throw test::Failure(label + ": accepted malformed input");
}

void appendU32(std::vector<std::byte>& out, std::uint32_t value)
{
	for (int shift = 0; shift < 32; shift += 8)
		out.push_back(static_cast<std::byte>((value >> shift) & 0xff));
}

void appendF32(std::vector<std::byte>& out, float value)
{
	appendU32(out, std::bit_cast<std::uint32_t>(value));
}

void putU32(std::vector<std::byte>& out, std::size_t at, std::uint32_t value)
{
	for (int i = 0; i < 4; ++i)
		out[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((value >> (8 * i)) & 0xff);
}

void appendRecord(std::vector<std::byte>& out, const std::string& name,
	const std::array<float, 10>& values)
{
	out.push_back(static_cast<std::byte>(name.size()));
	for (char c : name)
		out.push_back(static_cast<std::byte>(c));
	for (float value : values)
		appendF32(out, value);
}

constexpr std::size_t kHeaderBytes = 8 + 8 * 4096;

// Zone 5 (cell 5,0) holds two records; zone 70 (cell 6,1) holds one.
std::vector<std::byte> makeFeatures()
{
	std::vector<std::byte> blob5;
	appendU32(blob5, 2);
	appendRecord(blob5, "10-a.mod", { -131000.5f, 12.5f, -146000.25f, 1, 1, 1, 1, 0, 0, 0 });
	appendRecord(blob5, "11-b.mod", { -131100.0f, 20.0f, -146100.0f, 2, 0.5f, -1, 0.5f, 0.5f, 0.5f, 0.5f });
	std::vector<std::byte> blob70;
	appendU32(blob70, 1);
	appendRecord(blob70, "10-a.mod", { -120000.0f, 3.0f, -140000.0f, 1, 1, 1,
		std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN() });

	std::vector<std::byte> out;
	out.push_back(std::byte{ 'M' });
	out.push_back(std::byte{ 'F' });
	out.push_back(std::byte{ '0' });
	out.push_back(std::byte{ '1' });
	appendU32(out, 4096);
	out.resize(kHeaderBytes, std::byte{ 0 });
	putU32(out, 8 + 8 * 5, static_cast<std::uint32_t>(kHeaderBytes));
	putU32(out, 8 + 8 * 5 + 4, static_cast<std::uint32_t>(blob5.size()));
	putU32(out, 8 + 8 * 70, static_cast<std::uint32_t>(kHeaderBytes + blob5.size()));
	putU32(out, 8 + 8 * 70 + 4, static_cast<std::uint32_t>(blob70.size()));
	out.insert(out.end(), blob5.begin(), blob5.end());
	out.insert(out.end(), blob70.begin(), blob70.end());
	return out;
}

void checkParse()
{
	const auto bytes = makeFeatures();
	const auto file = kworld::parseFeatureFile(bytes, "test");
	test::require(file.zoneCount == 2 && file.placements.size() == 3, "synthetic counts");
	const auto& first = file.placements[0];
	test::require(first.recordID == "10-a.mod" && first.x == -131000.5 && first.y == 12.5
		&& first.z == -146000.25 && first.layer == "test", "synthetic first placement");
	test::require(first.cell == kworld::ZoneCell({ 5, 0 })
		&& first.positionCell == kworld::ZoneCell({ 3, 0 }), "synthetic cells");
	const auto& second = file.placements[1];
	test::require(second.scaleX == 2.0f && second.scaleY == 0.5f && second.scaleZ == -1.0f
		&& second.qw == 0.5 && second.qz == 0.5, "synthetic scale and rotation");
	test::require(file.placements[2].cell == kworld::ZoneCell({ 6, 1 })
		&& !file.placements[2].rotationFinite() && first.rotationFinite(),
		"synthetic non-finite rotation");

	for (std::size_t length = 0; length < bytes.size(); ++length)
	{
		const auto prefix = std::span(bytes.data(), length);
		requireThrows([prefix] { kworld::parseFeatureFile(prefix, "x"); },
			"features truncation at " + std::to_string(length));
	}

	requireThrows([&bytes] {
		auto bad = bytes;
		bad[0] = std::byte{ 'X' };
		kworld::parseFeatureFile(bad, "x");
	}, "features magic");
	requireThrows([&bytes] {
		auto bad = bytes;
		putU32(bad, 4, 4095);
		kworld::parseFeatureFile(bad, "x");
	}, "features zone count");
	requireThrows([&bytes] {
		auto bad = bytes;
		bad.push_back(std::byte{ 0 });
		kworld::parseFeatureFile(bad, "x");
	}, "features trailing byte");
	requireThrows([&bytes] {
		auto bad = bytes;
		putU32(bad, 8 + 8 * 5, 100); // blob inside the zone table
		kworld::parseFeatureFile(bad, "x");
	}, "features blob offset in the table");
	requireThrows([&bytes] {
		auto bad = bytes;
		putU32(bad, 8 + 8 * 5 + 4, 0x7fffffff); // size past the file
		kworld::parseFeatureFile(bad, "x");
	}, "features blob size");
	const std::size_t firstX = kHeaderBytes + 4 + 1 + std::string("10-a.mod").size();
	for (const float badValue : { std::numeric_limits<float>::quiet_NaN(),
		std::numeric_limits<float>::infinity(), 3.0e38f })
		for (const std::size_t axis : { std::size_t(0), std::size_t(4), std::size_t(8) })
		{
			requireThrows([&bytes, firstX, axis, badValue] {
				auto bad = bytes;
				putU32(bad, firstX + axis, std::bit_cast<std::uint32_t>(badValue));
				kworld::parseFeatureFile(bad, "x");
			}, "features non-finite or huge position, offset " + std::to_string(axis));
		}
	requireThrows([&bytes] {
		auto bad = bytes; // zone 70's blob overlaps zone 5's
		putU32(bad, 8 + 8 * 70, static_cast<std::uint32_t>(kHeaderBytes + 4));
		kworld::parseFeatureFile(bad, "x");
	}, "features overlapping blob");
	requireThrows([&bytes] {
		auto bad = bytes; // gap before zone 70's blob
		putU32(bad, 8 + 8 * 70, static_cast<std::uint32_t>(bytes.size() - 1));
		putU32(bad, 8 + 8 * 70 + 4, 1);
		kworld::parseFeatureFile(bad, "x");
	}, "features blob size below 4");
	requireThrows([&bytes] {
		auto bad = bytes;
		putU32(bad, 8 + 8 * 70, static_cast<std::uint32_t>(bytes.size() + 100));
		kworld::parseFeatureFile(bad, "x");
	}, "features offset beyond the file");
	requireThrows([&bytes] {
		auto bad = bytes; // blob offsets out of zone order
		putU32(bad, 8 + 8 * 5, static_cast<std::uint32_t>(bytes.size() - 1));
		kworld::parseFeatureFile(bad, "x");
	}, "features blob out of zone order");
	requireThrows([&bytes] {
		auto bad = bytes;
		putU32(bad, kHeaderBytes, 0xffffffff); // negative record count
		kworld::parseFeatureFile(bad, "x");
	}, "features negative count");
	requireThrows([&bytes] {
		auto bad = bytes;
		putU32(bad, kHeaderBytes, 1); // blob has one record left over
		kworld::parseFeatureFile(bad, "x");
	}, "features blob not consumed");
	requireThrows([&bytes] {
		auto bad = bytes;
		bad[kHeaderBytes + 4] = std::byte{ 0 }; // empty record name
		kworld::parseFeatureFile(bad, "x");
	}, "features empty name");

	std::mt19937 generator(0x5eed);
	for (int iteration = 0; iteration < 2000; ++iteration)
	{
		auto mutated = bytes;
		if (generator() % 3 == 0)
			mutated.resize(generator() % (mutated.size() + 1));
		else
		{
			// Bias towards the table and blob area, where the structure is.
			const auto limit = std::min<std::size_t>(mutated.size(), kHeaderBytes + 200);
			const auto position = generator() % limit;
			mutated[position] = static_cast<std::byte>(
				std::to_integer<std::uint8_t>(mutated[position]) ^ (1u << (generator() % 8)));
		}
		try
		{
			(void)kworld::parseFeatureFile(mutated, "x");
		}
		catch (const std::exception&)
		{
		}
		catch (...)
		{
			throw test::Failure("features fuzz: non-standard exception");
		}
	}
}

std::array<std::byte, 28> transform(float x, float y, float z, float u)
{
	const std::array<float, 7> values = { x, y, z, u, 0, 0, 0 };
	std::vector<std::byte> bytes;
	for (float value : values)
		appendF32(bytes, value);
	std::array<std::byte, 28> out{};
	std::copy(bytes.begin(), bytes.end(), out.begin());
	return out;
}

kdata::Instance foliageInstance(const std::string& id, const std::string& target, float x, float u)
{
	kdata::Instance instance;
	instance.id = id;
	instance.idIsString = true;
	instance.targetStringID = target;
	instance.transform = transform(x, 5.0f, -7.0f, u);
	return instance;
}

kdata::File foliageFile(std::vector<kdata::Instance> instances, const std::string& collectionID)
{
	kdata::File file;
	file.version = 15;
	kdata::Record collection;
	collection.type = kworld::itemTypeByName("INSTANCE_COLLECTION");
	collection.stringID = collectionID;
	collection.instances = std::move(instances);
	file.records.push_back(std::move(collection));
	return file;
}

void checkRoadFoliage()
{
	const auto base = kworld::collectRoadFoliage(foliageFile({
		foliageInstance("0", "-", 1.0f, 10.0f), foliageInstance("1", "-", 2.0f, 11.0f),
		foliageInstance("2", "-", 3.0f, 12.0f) }, "5-roadfoliage"), ".");
	test::require(base.size() == 3 && base[1].x == 2.0 && base[1].u == 11.0
		&& base[1].y == 5.0 && base[1].z == -7.0 && base[1].layer == "."
		&& base[1].targetID == "-" && !base[1].deleted, "road foliage collect");
	test::require(kworld::collectRoadFoliage(foliageFile({
		foliageInstance("0", "-", 1.0f, 1.0f) }, "4-roadlist"), ".").empty(),
		"road foliage ignores other collections");

	const auto mod = kworld::collectRoadFoliage(foliageFile({
		foliageInstance("1", "-", 20.0f, 21.0f), foliageInstance("2", "", 0.0f, 0.0f),
		foliageInstance("3", "-", 30.0f, 31.0f) }, "5-roadfoliage"), "mod");
	test::require(mod[1].deleted, "road foliage tombstone flag");
	const std::vector<std::vector<kworld::RoadFoliagePoint>> layers = { base, mod };
	const auto merged = kworld::overlayRoadFoliage(layers);
	test::require(merged.size() == 3 && merged[0].instanceID == "0"
		&& merged[1].instanceID == "1" && merged[1].x == 20.0 && merged[1].layer == "mod"
		&& merged[2].instanceID == "3", "road foliage overlay: replace, delete, add");
	test::require(kworld::overlayRoadFoliage({}).empty(), "road foliage overlay of nothing");
}

kdata::Record featureRecord(const std::string& id, const std::string& type)
{
	kdata::Record record;
	record.type = kworld::itemTypeByName(type);
	record.stringID = id;
	return record;
}

void checkResolve()
{
	auto rock = featureRecord("10-a.mod", "MAP_FEATURES");
	rock.files = { { "mesh", ".\\data\\rock.mesh" }, { "texture map", ".\\data\\rock.dds" } };
	rock.ints = { { "texture mode", 2 }, { "alpha threshold", 0 } };
	rock.bools = { { "local coordinates", true } };
	auto marker = featureRecord("11-b.mod", "MAP_FEATURES");
	marker.files = { { "mesh", ".\\data\\marker.mesh" } };
	marker.bools = { { "hidden", true } };
	auto tree = featureRecord("12-tree.mod", "FOLIAGE_MESH");
	tree.files = { { "mesh", ".\\data\\trunk.mesh" }, { "texture map", ".\\data\\trunk.dds" },
		{ "leaves mesh", ".\\data\\leaves.mesh" }, { "leaves texture", ".\\data\\leaves.dds" } };
	tree.ints = { { "material type", 4 } };
	auto other = featureRecord("13-c.mod", "BUILDING");

	kdata::File file;
	file.version = 15;
	file.records = { rock, marker, tree, other };
	kdata::GameData data;
	data.apply(file);

	const auto resolvedRock = kworld::resolveFeatureRecord(data, "10-a.mod");
	test::require(resolvedRock.found && resolvedRock.recordType == "MAP_FEATURES"
		&& resolvedRock.localCoordinates && resolvedRock.meshes.size() == 1
		&& resolvedRock.meshes[0].mode == kworld::FeatureMode::Terrain
		&& resolvedRock.meshes[0].texture == ".\\data\\rock.dds"
		&& resolvedRock.meshes[0].alphaThreshold == 0, "resolve MAP_FEATURES");
	test::require(kworld::resolveFeatureRecord(data, "11-b.mod").meshes.empty(),
		"hidden feature has no meshes");
	const auto resolvedTree = kworld::resolveFeatureRecord(data, "12-tree.mod");
	test::require(resolvedTree.recordType == "FOLIAGE_MESH" && resolvedTree.meshes.size() == 2
		&& resolvedTree.meshes[0].alphaThreshold == 128 && !resolvedTree.meshes[0].leaves
		&& resolvedTree.meshes[1].leaves && resolvedTree.meshes[1].mesh == ".\\data\\leaves.mesh"
		&& resolvedTree.meshes[1].texture == ".\\data\\leaves.dds"
		&& resolvedTree.meshes[1].alphaThreshold == 80, "resolve FOLIAGE_MESH with leaves");
	const auto resolvedOther = kworld::resolveFeatureRecord(data, "13-c.mod");
	test::require(resolvedOther.found && resolvedOther.recordType == "BUILDING"
		&& resolvedOther.meshes.empty(), "other record types have no meshes");
	const auto missing = kworld::resolveFeatureRecord(data, "nope");
	test::require(!missing.found && missing.recordType.empty() && missing.meshes.empty(),
		"missing record");
}

} // namespace

int main()
{
	try
	{
		checkParse();
		checkRoadFoliage();
		checkResolve();
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld features malformed: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld features malformed: MF01 truncation/fuzz, road foliage overlay and record resolution ok\n";
	return 0;
}
