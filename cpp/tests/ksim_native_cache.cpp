// The native patch cache (docs/formats/NAVMESH_CACHE.md "Patch cache"): the converted shipped regions and our zone rebakes
// kept on disk between opens (MeshNavigator::openNative, NativeCacheOptions).
//  - a cold open of the Hub 3 x 3 zones (an empty cache), then a warm open of the same zones: the same mesh, the warm one
//    from the cache and faster, with the times and RSS printed;
//  - one changed zone placement rebuilds that zone's entry only: the other zone's and the regions' entries are the hits;
//  - a corrupt entry (a truncated file) is deleted, logged and rebuilt;
//  - the cache off (--no-nav-cache: enabled = false) reads and writes nothing.
// The placement change is made on a mirrored data directory (the install is read-only): the Hub's rebirth zone file is
// rewritten with one building moved 8 u, which moves that cell's zone hash and nothing else. Runs against the install
// (KENSHI_DATA); exit 77 without the navtiles.

#include "ksim/nav/navigator.hpp"

#include "kdata/format.hpp"
#include "knav/native.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

namespace fs = std::filesystem;
using ksim::MeshNavigator;

const fs::path kScratch = fs::path(KSIM_BINARY_DIR) / "native_cache_test";

// A hash of the mesh's tiles, the way the streaming tests compare two meshes: the tile coordinates, the Detour bytes and the
// door lists, in (z, x) order (NavMesh keeps them sorted).
std::uint64_t meshHash(const knav::NavMesh& mesh)
{
	std::uint64_t hash = 14695981039346656037ull;
	const auto mix = [&hash](const void* data, std::size_t size)
	{
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for (std::size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= 1099511628211ull;
		}
	};
	for (const knav::BakedTile& tile : mesh.tiles())
	{
		mix(&tile.x, sizeof(tile.x));
		mix(&tile.z, sizeof(tile.z));
		mix(tile.data.data(), tile.data.size());
		for (const knav::DoorPoly& door : tile.doors)
		{
			mix(&door.poly, sizeof(door.poly));
			mix(&door.door, sizeof(door.door));
		}
	}
	return hash;
}

// One /proc/self/status field in kB (VmRSS now, VmHWM the peak), -1 when unreadable.
long statusKb(const char* field)
{
	std::ifstream in("/proc/self/status");
	for (std::string line; std::getline(in, line);)
	{
		if (line.rfind(field, 0) != 0)
			continue;
		const std::size_t at = line.find_first_of("0123456789");
		if (at != std::string::npos)
			return std::stol(line.substr(at));
	}
	return -1;
}

// One open's result: the mesh's hash and tiles, the time and RSS, the cache counters and the log.
struct Opening
{
	std::uint64_t hash = 0;
	std::size_t tiles = 0;
	std::size_t zones = 0;
	double ms = 0.0;
	long rssKb = 0; // the open's VmRSS delta
	long peakKb = -1; // the process's peak when the open returned
	ksim::NativeCacheStats cache;
	std::string tree;
	std::vector<std::string> log;
};

Opening open(const std::string& dataDir, const std::vector<kworld::ZoneCell>& zones, const ksim::NativeCacheOptions& cache)
{
	Opening opening;
	const long rssBefore = statusKb("VmRSS:");
	const auto start = std::chrono::steady_clock::now();
	const auto nav = MeshNavigator::openNative(dataDir, zones, [&opening](const std::string& line) { opening.log.push_back(line); },
		nullptr, cache);
	opening.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	opening.rssKb = statusKb("VmRSS:") - rssBefore;
	opening.peakKb = statusKb("VmHWM:");
	opening.hash = meshHash(nav->mesh());
	opening.tiles = nav->mesh().tiles().size();
	opening.zones = nav->coveredZones().size();
	opening.cache = nav->nativeCacheStats();
	opening.tree = nav->nativeCacheDir();
	return opening;
}

bool logged(const std::vector<std::string>& log, const std::string& text)
{
	for (const std::string& line : log)
		if (line.find(text) != std::string::npos)
			return true;
	return false;
}

// The entries in a cache tree: their names and a content hash.
std::map<std::string, std::uint64_t> cacheFiles(const std::string& tree)
{
	std::map<std::string, std::uint64_t> files;
	std::error_code error;
	for (const auto& entry : fs::directory_iterator(tree, error))
	{
		if (!entry.is_regular_file() || entry.path().extension() != ".knav")
			continue;
		std::ifstream in(entry.path(), std::ios::binary);
		std::uint64_t hash = 14695981039346656037ull;
		std::array<char, 1 << 16> block;
		while (in)
		{
			in.read(block.data(), block.size());
			const auto got = in.gcount();
			for (std::streamsize i = 0; i < got; ++i)
			{
				hash ^= static_cast<std::uint8_t>(block[static_cast<std::size_t>(i)]);
				hash *= 1099511628211ull;
			}
		}
		files[entry.path().filename().string()] = hash;
	}
	return files;
}

// A data directory that is the install's, symlinked entry by entry, except the directories we descend into to replace one
// zone file (newland and newland/leveldata).
void mirrorTree(const fs::path& source, const fs::path& target, const std::set<fs::path>& descend)
{
	fs::create_directories(target);
	for (const auto& entry : fs::directory_iterator(source))
	{
		const fs::path to = target / entry.path().filename();
		if (entry.is_directory() && descend.count(entry.path()) != 0)
			mirrorTree(entry.path(), to, descend);
		else if (entry.is_directory())
			fs::create_directory_symlink(entry.path(), to);
		else
			fs::create_symlink(entry.path(), to);
	}
}

} // namespace

int main()
{
	unsetenv("KENSHI_NAV_CACHE_SHARED"); // the cold/warm contract is of the test's own dir; the shared world cache would turn every miss into a hit (kenshi-k16.7.209)
	try
	{
		// The cache root from a faked environment (no data needed): the option wins, then XDG, the Windows variables, HOME.
		const auto root = [](std::map<std::string, std::string> env)
		{
			return ksim::nativeCacheRoot([&env](const char* name) { const auto at = env.find(name); return at == env.end() ? nullptr : at->second.c_str(); });
		};
		test::require(root({}).empty(), "no environment must give no cache root");
		test::require(root({ { "HOME", "/h" } }) == "/h/.cache/kenshi-sim/navmesh", "HOME root");
		test::require(root({ { "HOME", "/h" }, { "XDG_CACHE_HOME", "/x" } }) == "/x/kenshi-sim/navmesh", "XDG beats HOME");
		test::require(root({ { "LOCALAPPDATA", "C:/L" }, { "APPDATA", "C:/A" } }) == "C:/L/kenshi-sim/navmesh", "LOCALAPPDATA beats APPDATA");
		test::require(root({ { "APPDATA", "C:/A" } }) == "C:/A/kenshi-sim/navmesh", "APPDATA root");
		test::require(root({ { "LOCALAPPDATA", "C:/L" }, { "HOME", "/h" } }) == "C:/L/kenshi-sim/navmesh", "LOCALAPPDATA beats HOME (Proton)");
		test::require(root({ { "KENSHI_NAV_CACHE", "/k" }, { "XDG_CACHE_HOME", "/x" } }) == "/k", "KENSHI_NAV_CACHE wins");
		test::require(root({ { "XDG_CACHE_HOME", "" }, { "HOME", "/h" } }) == "/h/.cache/kenshi-sim/navmesh", "an empty variable is unset");
		if (!fs::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_native_cache: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::error_code error;
		fs::remove_all(kScratch, error);
		fs::create_directories(kScratch);
		const std::uint64_t worldKey = kworld::dataHash(kworld::resolveLoadOrder(KENSHI_DATA));
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({ 20 + dx, 32 + dz });

		// ---- cold, then warm: the Hub 3 x 3 zones ----
		ksim::NativeCacheOptions cache;
		cache.dir = (kScratch / "hub").string();
		cache.worldKey = worldKey;
		const Opening cold = open(KENSHI_DATA, hubZones, cache);
		test::require(cold.hash != 0 && cold.tiles > 0, "the cold open has no mesh");
		test::require(cold.cache.regionMisses == 0, "the cold open converted a shipped region (none after the cutover): "
			+ std::to_string(cold.cache.regionMisses));
		test::require(cold.cache.zoneMisses >= 1, "the cold open rebaked no zone");
		test::require(cold.cache.filesWritten == cold.cache.regionMisses + cold.cache.zoneMisses,
			"the cold open did not write one file per miss");
		test::require(cold.cache.bytesWritten > 0, "the cold open wrote no bytes");
		test::require(cold.cache.regionHits == 0 && cold.cache.zoneHits == 0, "a cold open hit an entry");
		test::require(!cold.tree.empty() && logged(cold.log, "nav cache ") && logged(cold.log, " miss"),
			"the open logged no cache line");

		const Opening warm = open(KENSHI_DATA, hubZones, cache);
		test::require(warm.hash == cold.hash && warm.tiles == cold.tiles, "the warm open's mesh differs from the cold one");
		test::require(warm.zones == cold.zones, "the warm open covers a different zone set");
		test::require(warm.cache.regionHits == cold.cache.regionMisses, "the warm open did not hit every converted region");
		test::require(warm.cache.zoneHits == cold.cache.zoneMisses, "the warm open did not hit every zone entry");
		test::require(warm.cache.filesWritten == 0 && warm.cache.bytesWritten == 0, "a warm open wrote files");
		test::require(warm.ms < cold.ms, "the warm open is not faster (" + std::to_string(warm.ms) + " vs "
			+ std::to_string(cold.ms) + " ms)");
		test::require(logged(warm.log, " hit"), "the warm open logged no hit");
		std::cout << "ksim_native_cache: cold open " << cold.ms << " ms (+" << cold.rssKb << " kB RSS), warm " << warm.ms
			<< " ms (+" << warm.rssKb << " kB), " << cold.tiles << " tiles in " << cold.zones << " zones, "
			<< cold.cache.filesWritten << " files " << cold.cache.bytesWritten << " B; peak RSS " << warm.peakKb << " kB\n";

		// ---- a corrupt entry: truncated, it is deleted, logged and rebuilt ----
		{
			std::vector<fs::path> zoneFiles;
			for (const auto& entry : fs::directory_iterator(cold.tree))
				if (entry.is_regular_file() && entry.path().filename().string().rfind("zone_", 0) == 0
					&& entry.path().extension() == ".knav")
					zoneFiles.push_back(entry.path());
			test::require(!zoneFiles.empty(), "no zone entry to corrupt");
			const fs::path victim = zoneFiles.front();
			fs::resize_file(victim, 64);
			const Opening rebuilt = open(KENSHI_DATA, hubZones, cache);
			test::require(rebuilt.cache.loadFailures >= 1, "the corrupt entry was not counted");
			test::require(logged(rebuilt.log, "unusable entry"), "the corrupt entry was not logged");
			test::require(fs::file_size(victim) > 64, "the corrupt entry was not rebuilt");
			test::require(rebuilt.hash == cold.hash, "the rebuilt open's mesh differs");
			test::require(rebuilt.cache.zoneMisses >= 1, "the rebuilt open baked no zone");
			std::cout << "ksim_native_cache: corrupt entry -> " << rebuilt.cache.loadFailures << " deleted and rebuilt, "
				<< rebuilt.ms << " ms\n";
		}

		// ---- a corrupt .agree sidecar (garbage) is recomputed, never trusted or fatal ----
		{
			for (const auto& entry : fs::directory_iterator(cold.tree))
				if (entry.path().extension() == ".agree")
				{
					std::ofstream(entry.path(), std::ios::trunc) << "agree5\n12 x\nnot a tile\n";
				}
			const Opening again = open(KENSHI_DATA, hubZones, cache);
			test::require(again.hash == cold.hash, "a corrupt .agree sidecar changed the mesh");
		}

		// ---- the cache off: nothing read or written ----
		{
			ksim::NativeCacheOptions off;
			off.dir = (kScratch / "off").string();
			off.worldKey = worldKey;
			off.enabled = false;
			const Opening disabled = open(KENSHI_DATA, hubZones, off);
			test::require(disabled.hash == cold.hash, "a cache-off open has another mesh");
			test::require(!fs::exists(off.dir), "a cache-off open created its directory");
			test::require(disabled.cache.filesWritten == 0 && disabled.cache.regionHits == 0 && disabled.cache.zoneHits == 0,
				"a cache-off open touched entries");
			test::require(logged(disabled.log, "nav cache: off"), "a cache-off open did not say so");
			std::cout << "ksim_native_cache: cache off " << disabled.ms << " ms, nothing written\n";
		}

		// ---- the shared tree: found by recipe + world data alone, never by the source hash (kenshi-k16.7.214) ----
		{
			const fs::path shared = kScratch / "shared";
			char hex[17];
			std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(worldKey));
			const std::string name = "w" + std::to_string(MeshNavigator::nativeCacheRecipe()) + "_" + hex;
			fs::create_directories(shared / name);
			for (const auto& entry : fs::directory_iterator(cold.tree))
				if (entry.is_regular_file())
					fs::copy_file(entry.path(), shared / name / entry.path().filename());
			ksim::NativeCacheOptions use;
			use.dir = (kScratch / "sharedUse").string();
			use.sharedDir = shared.string();
			use.worldKey = worldKey;
			use.bakeSrcHash = "cccccccccccccccccccc"; // another build: its own tree name, the shared one still found
			const Opening hit = open(KENSHI_DATA, hubZones, use);
			test::require(hit.hash == cold.hash, "the shared hit's mesh differs");
			test::require(hit.cache.zoneHits == cold.cache.zoneMisses && hit.cache.zoneMisses == 0,
				"a build with another source hash missed the shared tree");
			test::require(hit.cache.filesWritten == 0, "a shared hit wrote files");
			// Another world key (a different mod set) finds no shared tree: a cold bake into its own tree.
			ksim::NativeCacheOptions other = use;
			other.dir = (kScratch / "sharedOther").string();
			other.worldKey = worldKey ^ 1;
			const Opening miss = open(KENSHI_DATA, hubZones, other);
			test::require(miss.cache.zoneHits == 0 && miss.cache.zoneMisses >= 1, "another world key hit the shared tree");
			std::cout << "ksim_native_cache: shared tree hit " << hit.cache.zoneHits << " zones with another source hash, miss on another world key\n";
		}

		// ---- one changed zone placement: only that zone's entry is rebuilt ----
		{
			// The install mirrored (symlinks) with newland/leveldata as real directories, so one zone file can be replaced.
			const fs::path install = KENSHI_DATA;
			const fs::path mirror = kScratch / "data";
			const fs::path leveldata = install / "newland" / "leveldata";
			mirrorTree(install, mirror, { install / "newland", leveldata, leveldata / "Newwworld",
				leveldata / "Dialogue", leveldata / "rebirth" });
			const fs::path hubLayer = mirror / "newland" / "leveldata" / "rebirth" / "zone.20.32.zone";
			test::require(fs::is_symlink(hubLayer), "the Hub's rebirth zone file is not in the mirror as a symlink");
			ksim::NativeCacheOptions changed;
			changed.dir = (kScratch / "changed").string();
			changed.worldKey = kworld::dataHash(kworld::resolveLoadOrder(mirror.string()));
			test::require(changed.worldKey == worldKey, "the mirrored data has another load order hash");

			const Opening before = open(mirror.string(), { { 20, 32 } }, changed);
			test::require(before.cache.zoneMisses == 1, "the Hub's first open did not bake its zone");
			const Opening other = open(mirror.string(), { { 38, 22 } }, changed);
			const std::map<std::string, std::uint64_t> filesBefore = cacheFiles(before.tree);
			test::require(filesBefore.size() == before.cache.filesWritten + other.cache.filesWritten,
				"the entries before the change do not add up");

			// Change the Hub cell's placements: move one of its buildings 8 u in x. Our zone hash folds each building's f32
			// position (kworld/navhash.cpp buildingHash), so the zone's recomputed hash moves and its entry's key with it.
			// Swapping the rebirth file for Newwworld's would not: the two layers carry the same 104 placements and differ
			// in two heights, which the hash ignores. No data file changes, so the tree key stays.
			{
				std::ifstream in(install / "newland" / "leveldata" / "rebirth" / "zone.20.32.zone", std::ios::binary);
				const std::vector<char> raw{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
				test::require(!raw.empty(), "the Hub's rebirth zone file is empty");
				std::vector<std::byte> bytes(raw.size());
				std::memcpy(bytes.data(), raw.data(), raw.size());
				kdata::File zone = kdata::parse(bytes, "zone.20.32.zone");
				bool moved = false;
				for (kdata::Record& record : zone.records)
					for (kdata::Instance& instance : record.instances)
					{
						if (moved || instance.targetStringID.empty())
							continue;
						float x = 0.0f;
						std::memcpy(&x, instance.transform.data(), sizeof x);
						x += 8.0f;
						std::memcpy(instance.transform.data(), &x, sizeof x);
						moved = true;
					}
				test::require(moved, "the Hub's rebirth zone file has no building placement to move");
				const std::vector<std::byte> patched = kdata::write(zone);
				fs::remove(hubLayer);
				std::ofstream out(hubLayer, std::ios::binary);
				out.write(reinterpret_cast<const char*>(patched.data()), static_cast<std::streamsize>(patched.size()));
				test::require(out.good(), "cannot write the changed zone file");
			}

			const Opening afterHub = open(mirror.string(), { { 20, 32 } }, changed);
			test::require(afterHub.cache.zoneMisses == 1 && afterHub.cache.zoneHits == 0,
				"the changed zone was not the one rebaked");
			test::require(afterHub.cache.regionHits == 0 && afterHub.cache.regionMisses == 0,
				"a region entry was read or written after the cutover");
			const Opening afterOther = open(mirror.string(), { { 38, 22 } }, changed);
			test::require(afterOther.cache.zoneHits == 1 && afterOther.cache.zoneMisses == 0,
				"another zone's entry was not reused");

			const std::map<std::string, std::uint64_t> filesAfter = cacheFiles(before.tree);
			std::size_t kept = 0;
			for (const auto& [name, hash] : filesBefore)
				if (filesAfter.count(name) != 0 && filesAfter.at(name) == hash)
					++kept;
			test::require(kept == filesBefore.size(), "an unchanged entry was rewritten");
			test::require(filesAfter.size() == filesBefore.size() + 1, "the change wrote more than one new entry");
			std::cout << "ksim_native_cache: one zone changed: " << filesBefore.size() << " entries kept, "
				<< afterHub.cache.filesWritten + afterOther.cache.filesWritten << " written\n";
		}

		std::cout << "ksim_native_cache: ok\n";
		fs::remove_all(kScratch, error);
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_native_cache: " << e.what() << "\n";
		return 1;
	}
}
