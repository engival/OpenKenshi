// resolveLoadOrder: the vanilla default equals the old hardcoded list, and a
// synthetic mods.cfg is honoured (order, comments, missing, workshop-style
// folders). Uses a scratch install tree under the system temp directory.

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

void write(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream(path) << text;
}

void requireVanilla(const kworld::LoadOrder& order, const std::string& label)
{
	test::require(order.missing.empty() && order.files.size() == kworld::kGameDataLoadOrder.size(),
		label + ": size");
	for (std::size_t i = 0; i < order.files.size(); ++i)
		test::require(order.files[i].name == kworld::kGameDataLoadOrder[i], label + ": name");
}

} // namespace

int main()
{
	const fs::path scratch = fs::temp_directory_path()
		/ ("kworld_loadorder." + std::to_string(::getpid()));
	try
	{
		// No mods file: the vanilla four, all present, whatever the install's
		// own data/mods.cfg says (it is never read).
		const auto shipped = kworld::resolveLoadOrder(KENSHI_DATA);
		requireVanilla(shipped, "shipped");
		for (const auto& entry : shipped.files)
			test::require(fs::is_regular_file(entry.path), "missing " + entry.path);
		requireVanilla(kworld::vanillaLoadOrder(KENSHI_DATA), "vanilla");

		// <scratch>/Kenshi/data, with a workshop tree two levels above Kenshi.
		const fs::path data = scratch / "steamapps/common/Kenshi/data";
		const fs::path cfg = scratch / "mods.cfg";
		write(data / "gamedata.base", "");
		write(data / "rebirth.mod", "");
		write(scratch / "steamapps/workshop/content/233860/42/Cool.mod", "");
		write(scratch / "steamapps/workshop/content/233860/42/_Cool.info", "<id>42</id>");
		write(scratch / "steamapps/common/Kenshi/mods/Local/Local.mod", "");

		requireVanilla(kworld::resolveLoadOrder(data.string(), (scratch / "none.cfg").string()),
			"missing cfg");
		write(cfg, " \r\n\n");
		requireVanilla(kworld::resolveLoadOrder(data.string(), cfg.string()), "blank cfg");

		write(cfg, "# comment\r\nrebirth.mod\n  Cool \nLocal.mod\nGone.mod\n../evil.mod\n");
		const auto order = kworld::resolveLoadOrder(data.string() + "/", cfg.string());
		const char* expected[] = { "gamedata.base", "rebirth.mod", "Cool.mod", "Local.mod" };
		test::require(order.files.size() == 4, "synthetic: size");
		for (std::size_t i = 0; i < 4; ++i)
			test::require(order.files[i].name == expected[i], "synthetic: order");
		test::require(order.missing.size() == 2 && order.missing[0] == "Gone.mod"
			&& order.missing[1] == "../evil.mod", "synthetic: missing");
		test::require(order.files[2].path == (scratch
			/ "steamapps/workshop/content/233860/42/Cool.mod").string(), "workshop path");

		const auto folders = kworld::discoverModFolders(data.string());
		test::require(folders.size() == 2 && folders[0].id.empty() && folders[1].id == "42",
			"discoverModFolders");
		fs::remove_all(scratch);
	}
	catch (const std::exception& error)
	{
		fs::remove_all(scratch);
		std::cerr << "loadorder: " << error.what() << "\n";
		return 1;
	}
	std::cout << "loadorder: ok\n";
	return 0;
}
