#pragma once

// The streaming world navigation a test (or a tool) opens: by default the game's shipped navtiles converted to Detour at load
// (the runtime mesh, MeshNavigator::openNative, docs/formats/NAVMESH_CACHE.md "Runtime mesh"), the mesh the server and the game
// run on, with the nav cache on (a tree per load order; ctest points KENSHI_NAV_CACHE into the build tree, so the check and the
// rebakes are paid once per build). KSIM_NAVWORLD_BAKE set and non-empty selects the `knav_bake --world DIR` bake instead
// (`KSIM_NAVWORLD_BAKE=1 ctest -R ...`). KSIM_NATIVE_NAV=1 (the old switch) still selects the runtime mesh, now the default.
// The navigator is opened by a test's own queries; a sim route asks the character's clearance (Simulation::pathRadius,
// NAVMESH_CACHE.md "Clearance"), which the tests pass where they make one.

#include "ksim/navigator.hpp"

#include "knav/native.hpp"
#include "knav/navmesh.hpp"
#include "kworld/loadorder.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace test {

// true when the run is on the world bake (KSIM_NAVWORLD_BAKE set and non-empty; empty means unset)
inline bool bakeNav()
{
	const char* bake = std::getenv("KSIM_NAVWORLD_BAKE");
	return bake != nullptr && *bake != '\0';
}

inline std::shared_ptr<ksim::MeshNavigator> openWorldNav(const std::string& bakeDir, const std::vector<kworld::ZoneCell>& zones)
{
	if (bakeNav())
		return ksim::MeshNavigator::openWorld(bakeDir, zones, KENSHI_DATA);
	ksim::NativeCacheOptions cache;
	cache.worldKey = kworld::dataHash(kworld::resolveLoadOrder(KENSHI_DATA));
	return ksim::MeshNavigator::openNative(KENSHI_DATA, zones, {}, nullptr, cache);
}

// Whether the world navigation this run opens can serve the zone (x, z): the bake directory has its region file, or, on the
// runtime mesh, the game data has the shipped navtiles (a test that finds it false skips, exit 77).
inline bool worldRegionReady(const std::string& bakeDir, int zoneX, int zoneZ)
{
	if (!bakeNav())
		return std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA));
	return std::filesystem::exists(bakeDir + "/" + knav::regionFileName(zoneX / knav::kRegionZones, zoneZ / knav::kRegionZones));
}

} // namespace test
