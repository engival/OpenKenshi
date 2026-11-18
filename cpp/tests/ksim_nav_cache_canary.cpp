// The nav cache's staleness canary (docs/formats/NAVMESH_CACHE.md "Staleness canary"): one fixture zone (the Hub's) is baked
// fresh and its digest pinned beside the recipe number. A change to the bake moves the digest: the test then fails until the
// recipe (navigator.cpp kNativeCacheRecipe) is bumped and both pins here are updated, so cached entries of the old bake are
// never read as the new one. Runs against the install (KENSHI_DATA); exit 77 without the navtiles.

#include "ksim/nav/navigator.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

constexpr std::uint32_t kPinnedRecipe = 21;
constexpr std::uint64_t kPinnedDigest = 0x4fba5addfa175094ull;

std::string hex(std::uint64_t value)
{
	char text[32];
	std::snprintf(text, sizeof(text), "0x%016llxull", static_cast<unsigned long long>(value));
	return text;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_nav_cache_canary: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		ksim::NativeCacheOptions off;
		off.enabled = false;
		const kworld::ZoneCell hub{ 20, 32 };
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, { hub }, [](const std::string&) {}, nullptr, off);
		const std::uint64_t digest = nav->bakedZoneDigest(hub);
		const std::uint32_t recipe = ksim::MeshNavigator::nativeCacheRecipe();
		std::cout << "ksim_nav_cache_canary: recipe " << recipe << " digest " << hex(digest) << "\n";
		test::require(recipe == kPinnedRecipe, "kNativeCacheRecipe is " + std::to_string(recipe) + ", pinned " + std::to_string(kPinnedRecipe)
			+ ": update kPinnedRecipe and kPinnedDigest (" + hex(digest) + ") here");
		test::require(digest == kPinnedDigest, "the Hub zone's bake digest moved to " + hex(digest) + " with no recipe bump: bump "
			"kNativeCacheRecipe in navigator.cpp (cached bakes are stale), then update both pins here");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_nav_cache_canary: FAILED: " << e.what() << "\n";
		return 1;
	}
}
