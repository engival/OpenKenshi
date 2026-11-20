#include "platform/datasetting.hpp"

#include "app/datapicker.hpp"
#include "app/userdirs.hpp"
#include "kdata/datadir.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// The one line of the setting: "data=PATH".
const char* kSettingKey = "data=";

// KCLIENT_CONFIG=none (client/tools/headless.sh and the scene tests) reads and writes no per-user state; the saved data path is user state. A
// dev build still resolves through its built-in KENSHI_DATA, so headless runs do not depend on the file.
bool settingEnabled()
{
	const char* c = getenv("KCLIENT_CONFIG");
	return c == nullptr || *c == 0 || std::string(c) != "none";
}

std::string readSetting()
{
	if (!settingEnabled()) { return std::string(); }
	std::ifstream f(datasetting::configFile());
	for (std::string line; std::getline(f, line);)
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
		const std::size_t at = line.find_first_not_of(" \t");
		if (at == std::string::npos) { continue; }
		line = line.substr(at);
		if (line.rfind(kSettingKey, 0) == 0) { return line.substr(std::char_traits<char>::length(kSettingKey)); }
	}
	return std::string();
}

void saveSetting(const std::string& dir)
{
	if (!settingEnabled()) { return; }
	std::error_code ec;
	const fs::path file = datasetting::configFile();
	fs::create_directories(file.parent_path(), ec);
	const fs::path tmp = file.string() + ".tmp";
	{
		std::ofstream f(tmp, std::ios::trunc);
		if (!f) { return; }
		f << kSettingKey << dir << "\n";
		if (!f) { return; }
	}
	fs::rename(tmp, file, ec);
}
}

std::string datasetting::configFile()
{
	return (fs::path(plat::userConfigDir()) / "kenshi-client" / "data.conf").string();
}

bool datasetting::resolve(const std::string& cli, std::string& out, std::string& message)
{
	// kdata tries --data, $KENSHI_DATA, then these places, then the OS defaults. A saved path the game has moved from is stale, not an error:
	// it just does not match and the defaults replace it.
	std::vector<kdata::datadir::Place> extra;
	const std::string savedLabel = "saved setting (" + configFile() + ")";
	const std::string saved = readSetting();
	extra.push_back({ savedLabel, !settingEnabled() ? "(ignored: KCLIENT_CONFIG=none)" : saved.empty() ? "(not set)" : saved });
#ifdef KENSHI_DATA
	// A dev build only (client/CMakeLists.txt): the developer's own install, so a checkout runs with no flags. Never in a release build.
	extra.push_back({ "build default (KENSHI_DATA)", KENSHI_DATA });
#endif

	const kdata::datadir::Result r = kdata::datadir::resolve(cli, extra);
	if (r.found)
	{
		out = r.dir;
		const bool fromDefault = r.matched != "--data" && r.matched != "$KENSHI_DATA" && r.matched != savedLabel;
		if (fromDefault)	// a hit among the installs is saved, so the next start looks nowhere
		{
			saveSetting(r.dir);
			const std::string note = settingEnabled() ? "; saved to " + configFile() : "";
			fprintf(stderr, "kclient: game data: %s (found at %s%s)\n", r.dir.c_str(), r.matched.c_str(), note.c_str());
		}
		return true;
	}
	// An explicit --data / $KENSHI_DATA that is wrong fails at once, with no picker.
	const char *envData = getenv("KENSHI_DATA");
	if (!cli.empty() || (envData && *envData)) { message = r.message; return false; }

	// Nothing found: ask with the native folder picker (datapicker.cpp; never without a display, under KCLIENT_NO_DIALOG=1 or KCLIENT_CONFIG=none).
	if (datapicker::available())
	{
		if (const std::string dir = datapicker::pick(kdata::datadir::dataDirOf); !dir.empty())
		{
			out = dir;
			saveSetting(dir);
			fprintf(stderr, "kclient: game data: %s (picked; saved to %s)\n", dir.c_str(), configFile().c_str());
			return true;
		}
	}
	message = r.message + "  or the line \"data=/path/to/Kenshi/data\" in " + configFile() + "\n";
	return false;
}
