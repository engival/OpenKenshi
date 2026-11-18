// Truncated and corrupted world files must throw, never crash. Built a second
// time against ASan/UBSan-instrumented kworld and kdata libraries.
//
// Uses small synthetic files only, so the ASan run does not map the 537 MB
// heightmap.

#include "kdata/format.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/pathing.hpp"
#include "kworld/zones.hpp"
#include "test_util.hpp"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

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

class TempDir
{
public:
	TempDir()
	{
		static unsigned counter = 0;
		const auto stamp = std::chrono::steady_clock::now()
			.time_since_epoch().count();
		path_ = fs::temp_directory_path()
			/ ("kworld_malformed_" + std::to_string(::getpid()) + "_"
				+ std::to_string(stamp) + "_" + std::to_string(counter++));
		std::error_code error;
		fs::create_directories(path_, error);
		if (error)
			throw test::Failure("cannot create " + path_.string());
	}

	~TempDir()
	{
		std::error_code error;
		fs::remove_all(path_, error);
	}

	const fs::path& path() const noexcept { return path_; }

private:
	fs::path path_;
};

void writeFile(const fs::path& path, std::span<const std::byte> bytes)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		throw test::Failure("cannot write " + path.string());
	out.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	if (!out)
		throw test::Failure("cannot write " + path.string());
}

void appendU16(std::vector<std::byte>& out, std::uint16_t value)
{
	out.push_back(static_cast<std::byte>(value & 0xff));
	out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
}

void appendU32(std::vector<std::byte>& out, std::uint32_t value)
{
	out.push_back(static_cast<std::byte>(value & 0xff));
	out.push_back(static_cast<std::byte>((value >> 8) & 0xff));
	out.push_back(static_cast<std::byte>((value >> 16) & 0xff));
	out.push_back(static_cast<std::byte>((value >> 24) & 0xff));
}

void putU32(std::byte* out, std::uint32_t value)
{
	out[0] = static_cast<std::byte>(value & 0xff);
	out[1] = static_cast<std::byte>((value >> 8) & 0xff);
	out[2] = static_cast<std::byte>((value >> 16) & 0xff);
	out[3] = static_cast<std::byte>((value >> 24) & 0xff);
}

std::array<std::byte, 28> transform(float x, float y, float z,
	float qw, float qx, float qy, float qz)
{
	const std::array<float, 7> values = { x, y, z, qw, qx, qy, qz };
	std::array<std::byte, 28> out{};
	for (std::size_t i = 0; i < values.size(); ++i)
		putU32(out.data() + 4 * i, std::bit_cast<std::uint32_t>(values[i]));
	return out;
}

// Minimal single-strip 16-bit TIFF. Pixel (col,row) is 100*row+col.
std::vector<std::byte> makeTiff()
{
	constexpr int width = 3;
	constexpr int rows = 3;
	constexpr std::uint32_t pixelOffset = 8 + 2 + 9 * 12 + 4;

	std::vector<std::byte> out;
	out.push_back(std::byte{ 'I' });
	out.push_back(std::byte{ 'I' });
	appendU16(out, 42);
	appendU32(out, 8);
	appendU16(out, 9);
	const auto entry = [&out](std::uint16_t tag, std::uint16_t type,
		std::uint32_t count, std::uint32_t value) {
		appendU16(out, tag);
		appendU16(out, type);
		appendU32(out, count);
		appendU32(out, value);
	};
	entry(256, 3, 1, width); // ImageWidth SHORT
	entry(257, 3, 1, rows); // ImageLength SHORT
	entry(258, 3, 1, 16); // BitsPerSample
	entry(259, 3, 1, 1); // Compression = none
	entry(262, 3, 1, 1); // PhotometricInterpretation = BlackIsZero
	entry(273, 4, 1, pixelOffset); // StripOffsets LONG
	entry(277, 3, 1, 1); // SamplesPerPixel
	entry(278, 4, 1, rows); // RowsPerStrip
	entry(279, 4, 1, width * rows * 2); // StripByteCounts
	appendU32(out, 0); // next IFD
	test::require(out.size() == pixelOffset, "synthetic TIFF header size");
	for (int row = 0; row < rows; ++row)
		for (int col = 0; col < width; ++col)
			appendU16(out, static_cast<std::uint16_t>(100 * row + col));
	return out;
}

void checkTiff(const TempDir& directory)
{
	const auto bytes = makeTiff();
	const fs::path good = directory.path() / "good.tif";
	writeFile(good, bytes);
	{
		const auto heightmap = kworld::Heightmap::open(good.string());
		test::require(heightmap.sampleCountX() == 3 && heightmap.sampleCountZ() == 3,
			"synthetic TIFF dimensions");
		test::require(std::abs(heightmap.height(-147456.0, -147456.0) - 0.0) < 1e-9,
			"synthetic TIFF origin sample");
		test::require(std::abs(heightmap.height(-147456.0 + 18.0, -147456.0)
			- 1.0 * kworld::kHeightScale) < 1e-9, "synthetic TIFF col step");
		test::require(std::abs(heightmap.height(-147456.0 + 9.0, -147456.0 + 9.0)
			- 50.5 * kworld::kHeightScale) < 1e-9, "synthetic TIFF bilinear centre");
		test::require(std::abs(heightmap.height(-147456.0 + 9.0, -147456.0 + 27.0)
			- 150.5 * kworld::kHeightScale) < 1e-9, "synthetic TIFF bilinear row 1.5");
		test::require(std::abs(heightmap.height(147456.0, -147456.0)
			- 2.0 * kworld::kHeightScale) < 1e-9, "synthetic TIFF right edge");
		test::require(std::abs(heightmap.height(-147456.0, 147456.0)
			- 200.0 * kworld::kHeightScale) < 1e-9, "synthetic TIFF bottom edge");
		requireThrows([&heightmap] { heightmap.height(std::nan(""), 0.0); },
			"heightmap NaN coordinate");
	}

	const fs::path bad = directory.path() / "truncated.tif";
	for (std::size_t length = 0; length < bytes.size(); ++length)
	{
		writeFile(bad, std::span(bytes.data(), length));
		requireThrows([&bad] { kworld::Heightmap::open(bad.string()); },
			"TIFF truncation at " + std::to_string(length));
	}

	auto corrupted = bytes;
	corrupted[2] = std::byte{ 0 };
	writeFile(bad, corrupted);
	requireThrows([&bad] { kworld::Heightmap::open(bad.string()); }, "TIFF magic");
}

std::vector<std::byte> makeZone(std::vector<std::byte> leftover)
{
	using namespace kworld;

	kdata::File file;
	file.version = 15;
	file.lastID = 2;

	kdata::Record collection;
	collection.type = itemTypeByName("INSTANCE_COLLECTION");
	collection.id = 1;
	collection.stringID = "0-buildinglist";
	collection.hasFlags = true;
	collection.flags = 0x10;

	kdata::Instance placement;
	placement.id = "p1";
	placement.idIsString = true;
	placement.targetStringID = "b-1";
	placement.transform = transform(100.5f, 5.25f, -200.25f,
		0.5f, 0.5f, 0.5f, 0.5f);
	placement.states = { "s1" };

	kdata::Instance inventory;
	inventory.id = "i1";
	inventory.idIsString = true;
	inventory.targetStringID = "inv-1";
	inventory.transform = transform(0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);

	kdata::Instance deleted;
	deleted.id = "d1";
	deleted.idIsString = true;
	deleted.targetStringID = "";
	deleted.transform = transform(9.0f, 9.0f, 9.0f, 1.0f, 0.0f, 0.0f, 0.0f);

	collection.instances = { placement, inventory, deleted };

	kdata::Record building;
	building.type = itemTypeByName("GAMESTATE_BUILDING");
	building.id = 2;
	building.stringID = "g-1";
	building.hasFlags = true;
	building.flags = 0x10;

	file.records = { collection, building };
	file.leftover = std::move(leftover);
	return kdata::write(file);
}

std::vector<std::byte> trailer(std::vector<std::int32_t> values)
{
	std::vector<std::byte> out;
	appendU32(out, static_cast<std::uint32_t>(values.size()));
	for (const auto value : values)
		appendU32(out, static_cast<std::uint32_t>(value));
	return out;
}

void checkZones()
{
	const auto bytes = makeZone(trailer({ 1, 2, 3 }));
	const auto zone = kworld::parseZoneFile(bytes, "zone.3.4.zone", "test");
	test::require(zone.version == 15, "synthetic zone version");
	test::require(zone.cellX == 3 && zone.cellZ == 4, "synthetic zone cell");
	test::require(zone.recordCount == 2, "synthetic zone record count");
	test::require(zone.typeCounts.size() == 2
		&& zone.typeCounts[0].type == "INSTANCE_COLLECTION"
		&& zone.typeCounts[0].count == 1
		&& zone.typeCounts[1].type == "GAMESTATE_BUILDING",
		"synthetic zone type counts");
	test::require(zone.placementCount == 1 && zone.deletionCount == 1
		&& zone.inventoryRowCount == 1, "synthetic zone instance counts");
	test::require(zone.hasTrailingBlock() && zone.trailingBytes.size() == 16
		&& zone.trailingValues == std::vector<std::int32_t>({ 1, 2, 3 }),
		"synthetic zone trailing block");

	const auto placement = zone.instances[0];
	test::require(placement.instanceID == "p1" && placement.targetID == "b-1"
		&& placement.kind == kworld::ZoneInstanceKind::Placement,
		"synthetic placement identity");
	test::require(placement.x == 100.5 && placement.y == 5.25
		&& placement.z == -200.25, "synthetic placement position");
	test::require(placement.qw == 0.5 && placement.qx == 0.5
		&& placement.qy == 0.5 && placement.qz == 0.5, "synthetic quaternion");
	test::require(zone.instances[1].kind == kworld::ZoneInstanceKind::InventoryRow,
		"synthetic inventory row");
	test::require(zone.instances[2].kind == kworld::ZoneInstanceKind::Deleted,
		"synthetic tombstone");

	const std::size_t cutoff = bytes.size() - 16;
	for (std::size_t length = 0; length < bytes.size(); ++length)
	{
		const auto prefix = std::span(bytes.data(), length);
		if (length == cutoff)
		{
			const auto complete = kworld::parseZoneFile(prefix, "zone.0.0.zone", "x");
			test::require(!complete.hasTrailingBlock(),
				"zone without a trailing block should parse");
			continue;
		}
		requireThrows([prefix] {
			kworld::parseZoneFile(prefix, "zone.0.0.zone", "x");
		}, "zone truncation at " + std::to_string(length));
	}

	requireThrows([&bytes] {
		kworld::parseZoneFile(bytes, "not-a-zone.bin", "x");
	}, "zone file name");
	requireThrows([&bytes] {
		kworld::parseZoneFile(bytes, "zone.3.zone", "x");
	}, "zone file name without a cell row");
	requireThrows([&bytes] {
		kworld::parseZoneFile(bytes, "zone.3.4.5.zone", "x");
	}, "zone file name with three parts");

	requireThrows([] {
		const auto bad = makeZone(trailer({ 2, 1, 3 }));
		kworld::parseZoneFile(bad, "zone.0.0.zone", "x");
	}, "trailing block not ascending");
	requireThrows([] {
		const auto bad = makeZone(trailer({ 1, 1, 2 }));
		kworld::parseZoneFile(bad, "zone.0.0.zone", "x");
	}, "trailing block not unique");
	requireThrows([] {
		auto bad = trailer({ 1, 2 });
		bad.push_back(std::byte{ 0 });
		kworld::parseZoneFile(makeZone(std::move(bad)), "zone.0.0.zone", "x");
	}, "trailing block size mismatch");

	std::mt19937 generator(0x5eed);
	for (int iteration = 0; iteration < 500; ++iteration)
	{
		auto mutated = bytes;
		if (generator() % 3 == 0)
			mutated.resize(generator() % (mutated.size() + 1));
		else
		{
			const auto position = generator() % mutated.size();
			const auto bit = static_cast<std::uint8_t>(1u << (generator() % 8));
			mutated[position] = static_cast<std::byte>(
				std::to_integer<std::uint8_t>(mutated[position]) ^ bit);
		}
		try
		{
			(void)kworld::parseZoneFile(mutated, "zone.0.0.zone", "x");
		}
		catch (const std::exception&)
		{
		}
		catch (...)
		{
			throw test::Failure("zone fuzz: non-standard exception");
		}
	}
}

std::vector<std::byte> makePathing()
{
	std::vector<std::byte> out;
	appendU32(out, 3); // version
	appendU32(out, 2); // count
	appendU32(out, 2); // count2

	// Node 1: horizontal portal on the (0,0)/(0,1) boundary.
	appendU32(out, 1); // id
	appendU32(out, 0); // a
	appendU32(out, 0); // b
	appendU32(out, 0); // na
	appendU32(out, 1); // nb
	appendU32(out, std::bit_cast<std::uint32_t>(-147456.0f));
	appendU32(out, std::bit_cast<std::uint32_t>(-129024.0f));
	appendU32(out, std::bit_cast<std::uint32_t>(-129096.0f));
	appendU32(out, std::bit_cast<std::uint32_t>(-129023.0f));
	appendU32(out, 1); // edge count
	appendU32(out, 2); // target
	appendU32(out, std::bit_cast<std::uint32_t>(1.5f));
	appendU32(out, 20); // class

	// Node 2: vertical portal on the (0,1)/(1,1) boundary.
	appendU32(out, 2);
	appendU32(out, 0);
	appendU32(out, 1);
	appendU32(out, 1);
	appendU32(out, 1);
	appendU32(out, std::bit_cast<std::uint32_t>(-129024.0f));
	appendU32(out, std::bit_cast<std::uint32_t>(-129024.0f));
	appendU32(out, std::bit_cast<std::uint32_t>(-129023.0f));
	appendU32(out, std::bit_cast<std::uint32_t>(-128952.0f));
	appendU32(out, 1);
	appendU32(out, 1);
	appendU32(out, std::bit_cast<std::uint32_t>(1.5f));
	appendU32(out, 20);

	appendU32(out, 0);
	appendU32(out, 16);
	appendU32(out, 0);
	appendU32(out, 16);
	return out;
}

void checkPathing()
{
	const auto bytes = makePathing();
	{
		const auto pathing = kworld::GlobalPathing::parse(bytes);
		test::require(pathing.version == 3 && pathing.count == 2
			&& pathing.count2 == 2, "synthetic pathing header");
		test::require(pathing.nodes.size() == 2
			&& pathing.directedEdgeCount() == 2, "synthetic pathing counts");
		test::require(pathing.trailer == std::array<std::uint32_t, 4>{ 0, 16, 0, 16 },
			"synthetic pathing trailer");
		test::require(pathing.validate().empty(), "synthetic pathing invariants");
	}

	for (std::size_t length = 0; length < bytes.size(); ++length)
		requireThrows([&bytes, length] {
			kworld::GlobalPathing::parse(std::span(bytes.data(), length));
		}, "pathing truncation at " + std::to_string(length));

	auto trailing = bytes;
	trailing.push_back(std::byte{ 0 });
	requireThrows([&trailing] {
		kworld::GlobalPathing::parse(trailing);
	}, "pathing trailing byte");

	std::mt19937 generator(0xc0ffee);
	for (int iteration = 0; iteration < 500; ++iteration)
	{
		auto mutated = bytes;
		if (generator() % 3 == 0)
			mutated.resize(generator() % (mutated.size() + 1));
		else
		{
			const auto position = generator() % mutated.size();
			const auto bit = static_cast<std::uint8_t>(1u << (generator() % 8));
			mutated[position] = static_cast<std::byte>(
				std::to_integer<std::uint8_t>(mutated[position]) ^ bit);
		}
		try
		{
			const auto pathing = kworld::GlobalPathing::parse(mutated);
			(void)pathing.validate();
			(void)pathing.directedEdgeCount();
		}
		catch (const std::exception&)
		{
		}
		catch (...)
		{
			throw test::Failure("pathing fuzz: non-standard exception");
		}
	}
}

void run()
{
	const TempDir directory;
	checkTiff(directory);
	checkZones();
	checkPathing();
}

} // namespace

int main()
{
	try
	{
		run();
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld malformed: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld malformed: TIFF, zone and pathing truncation/fuzz checks ok\n";
	return 0;
}
