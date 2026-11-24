// The nav cache root keeps only the newest 3 recipe trees (docs/formats/NAVMESH_CACHE.md "Prune"): a tmp root with 5 fake
// w<N>_<hash> trees plus bystanders; the oldest 2 go, the one in use stays, nothing else is touched.

#include "ksim/nav/navigator.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main()
{
	try
	{
		// A fresh unique directory (mkdtemp is not portable: macOS hides it in strict C++20, MinGW lacks it; kenshi-k16.6.804).
		const fs::path root = fs::temp_directory_path()
			/ ("ksim_nav_cache_prune_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
		if (!fs::create_directory(root))
			throw test::Failure("temp dir exists: " + root.string());
		const auto now = fs::file_time_type::clock::now();
		// w5 oldest .. w9 newest, but w5 is the tree in use: touching it on open makes it the newest.
		for (int i = 5; i <= 9; ++i)
		{
			const fs::path tree = root / ("w" + std::to_string(i) + "_00ab12cd");
			fs::create_directories(tree / "zones");
			std::ofstream(tree / "zones" / "z.bin") << "x";
			fs::last_write_time(tree, now - std::chrono::hours(100 - i));
		}
		fs::create_directories(root / "keepme");
		fs::create_directories(root / "w3_notahexZZ");
		std::ofstream(root / "w2_00ab12cd") << "a file, not a tree";

		std::vector<std::string> lines;
		auto removed = ksim::MeshNavigator::pruneCacheRoot(root.string(), "w5_00ab12cd", 3,
			[&lines](const std::string& line) { lines.push_back(line); });

		std::sort(removed.begin(), removed.end());
		test::require(removed.size() == 2 && removed[0] == "w6_00ab12cd" && removed[1] == "w7_00ab12cd",
			"the two oldest not in use (w6, w7) are removed");
		test::require(lines.size() == 2, "one log line per deleted tree");
		test::require(fs::exists(root / "w5_00ab12cd" / "zones" / "z.bin"), "the tree in use is kept whole");
		test::require(fs::exists(root / "w8_00ab12cd") && fs::exists(root / "w9_00ab12cd"), "the newest two are kept");
		test::require(!fs::exists(root / "w6_00ab12cd") && !fs::exists(root / "w7_00ab12cd"), "w6 and w7 are gone");
		test::require(fs::exists(root / "keepme") && fs::exists(root / "w3_notahexZZ") && fs::exists(root / "w2_00ab12cd"),
			"names that are not w<N>_<hex> trees are left alone");
		test::require(ksim::MeshNavigator::pruneCacheRoot(root.string(), "w5_00ab12cd", 3).empty(), "a second prune removes nothing");

		fs::remove_all(root); // root is this test's own fresh directory
		std::cout << "ksim_nav_cache_prune: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_nav_cache_prune: FAILED: " << error.what() << "\n";
		return 1;
	}
}
