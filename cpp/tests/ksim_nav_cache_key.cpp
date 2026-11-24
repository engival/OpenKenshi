// The nav cache tree name folds the bake source hash (docs/formats/NAVMESH_CACHE.md "Where"): a different hash is a different
// tree (a rebuild), the same hash and the compiled-in default are the same tree (a hit). Exit 77 without the navtiles (kenshi-k16.7.161).

#include "ksim/nav/navigator.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <filesystem>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

std::string tree(const std::string& dir, const std::string& srcHash)
{
	ksim::NativeCacheOptions cache;
	cache.dir = dir;
	cache.worldKey = 0x1234;
	cache.bakeSrcHash = srcHash;
	// No zones: the tree is only opened, never filled.
	const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, {}, [](const std::string&) {}, nullptr, cache);
	return nav->nativeCacheDir();
}

} // namespace

int main()
{
	namespace fs = std::filesystem;
	const fs::path scratch = fs::current_path() / "ksim_nav_cache_key_scratch";
	try
	{
		if (!fs::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_nav_cache_key: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		fs::remove_all(scratch);
		const std::string dir = scratch.string();
		const std::string a = tree(dir, "aaaaaaaaaaaaaaaaaaaa");
		test::require(!a.empty(), "no cache tree opened");
		test::require(tree(dir, "aaaaaaaaaaaaaaaaaaaa") == a, "the same bake source hash is another tree");
		test::require(tree(dir, "bbbbbbbbbbbbbbbbbbbb") != a, "a changed bake source hash reuses the tree");
		test::require(tree(dir, "") == tree(dir, ""), "the compiled-in hash is not stable");
		test::require(tree(dir, "") != a, "the compiled-in hash equals the injected one");
		fs::remove_all(scratch);
		return 0;
	} catch (const std::exception& e) {
		fs::remove_all(scratch);
		std::cerr << "ksim_nav_cache_key: FAILED: " << e.what() << "\n";
		return 1;
	}
}
