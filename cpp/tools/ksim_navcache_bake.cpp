// ksim_navcache_bake: fills the native nav cache tree (docs/formats/NAVMESH_CACHE.md "Patch cache") for a range of world regions.
//   ksim_navcache_bake [--jobs N] CACHEDIR [RX0 RZ0 RX1 RZ1]      (default: the whole world, one job)
// --jobs N: N processes, each baking every Nth region of the range (a region's bake runs mostly on one core; an entry is written under a
// temp name of its process and renamed, so the jobs share CACHEDIR). A heavy region holds several GB: size N to the memory (kenshi-k16.7.212).
// One region per open (its 16 zones), so each zone is read from CACHEDIR or baked and written there (kenshi-k16.7.190). Prints
// every line the loader logs plus a per-region wall time; a zone that throws is reported and the run goes on (exit 1 at the end).
#include "kworld/loadorder.hpp"
#include "kworld/gamedata.hpp"
#include "knav/native_tiles.hpp"
#include "ksim/nav/navigator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

int main(int argc, char** argv)
{
	int jobs = 1;
	if (argc >= 3 && std::string(argv[1]) == "--jobs")
	{
		jobs = std::max(1, std::atoi(argv[2]));
		argv += 2;
		argc -= 2;
	}
	if (argc != 2 && argc != 6)
	{
		std::cerr << "usage: ksim_navcache_bake [--jobs N] CACHEDIR [RX0 RZ0 RX1 RZ1]\n";
		return 2;
	}
	const int regionsPerAxis = kworld::kZoneCellsPerAxis / knav::kRegionZones;
	int range[4] = { 0, 0, regionsPerAxis - 1, regionsPerAxis - 1 };
	for (int i = 0; i < 4 && argc == 6; ++i)
		range[i] = std::atoi(argv[2 + i]);
	int job = 0;
#ifndef _WIN32
	// the children are forked before any game data is read; each prints whole lines, so the jobs' output interleaves by line
	std::vector<pid_t> children;
	for (int k = 1; k < jobs && job == 0; ++k)
	{
		const pid_t pid = fork();
		if (pid == 0)
			job = k;
		else if (pid > 0)
			children.push_back(pid);
	}
	if (job != 0)
		children.clear();
#else
	jobs = 1;
#endif
	ksim::NativeCacheOptions cache;
	cache.dir = argv[1];
	cache.sharedDir.clear();
	cache.shareGroundWindows = true;
	cache.lookupName = true; // the tree a server's shared lookup finds
	cache.worldKey = kworld::dataHash(kworld::resolveLoadOrder(KENSHI_DATA));
	cache.keepTrees = 1000;
	int failed = 0, index = 0;
	for (int rz = range[1]; rz <= range[3]; ++rz)
		for (int rx = range[0]; rx <= range[2]; ++rx)
		{
			if (index++ % jobs != job)
				continue;
			const std::string tag = "[" + std::to_string(rx) + "," + std::to_string(rz) + "] ";
			const auto start = std::chrono::steady_clock::now();
			try
			{
				std::vector<kworld::ZoneCell> zones;
				for (int z = 0; z < knav::kRegionZones; ++z)
					for (int x = 0; x < knav::kRegionZones; ++x)
						zones.push_back({ rx * knav::kRegionZones + x, rz * knav::kRegionZones + z });
				const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, zones,
					[&tag](const std::string& line) { std::cout << tag + line + "\n" << std::flush; }, nullptr, cache);
			} catch (const std::exception& e) {
				++failed;
				std::cout << tag + "FAILED: " + e.what() + "\n" << std::flush;
			}
			std::cout << tag + "region wall " + std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()) + " s\n" << std::flush;
		}
#ifndef _WIN32
	for (const pid_t pid : children)
	{
		int status = 0;
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			++failed;
	}
#endif
	return failed != 0 ? 1 : 0;
}
