#include "app/datadir.hpp"

#include "app/datapicker.hpp"
#include "app/steamvdf.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace
{
std::string env(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr ? std::string(v) : std::string();
}

std::string homePath()
{
#if defined(_WIN32)
	return env("USERPROFILE");
#else
	return env("HOME");
#endif
}

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
	std::ifstream f(datadir::configFile());
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
	const fs::path file = datadir::configFile();
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

bool holdsGameData(const fs::path& p)
{
	std::error_code ec;
	return fs::is_regular_file(p / "gamedata.base", ec);
}

// A candidate may be the data folder itself (--data / $KENSHI_DATA as the docs have it) or an install root; the data folder is what the client
// wants. Empty = not a Kenshi install.
std::string dataDirOf(const std::string& path)
{
	if (path.empty()) { return std::string(); }
	const fs::path p(path);
	if (holdsGameData(p)) { return p.lexically_normal().string(); }
	if (holdsGameData(p / "data")) { return (p / "data").lexically_normal().string(); }
	return std::string();
}

struct Place
{
	std::string label, path;
};

std::string readFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The Steam libraries a Steam root lists: steamapps/libraryfolders.vdf and config/libraryfolders.vdf, parsed by nesting (steamvdf.hpp), deduped.
// Kenshi's folder is the one steamapps/appmanifest_233860.acf names ("installdir", as ~/git/steaminfo does); the plain steamapps/common/Kenshi
// stays as the fallback place when no manifest names it.
void addSteamLibraries(std::vector<Place>& out, const std::string& steamRoot)
{
	std::vector<std::string> libs;
	for (const char* rel : { "/steamapps/libraryfolders.vdf", "/config/libraryfolders.vdf" })
	{
		for (const std::string& p : steamvdf::libraryPaths(readFile(steamRoot + rel)))
		{
			if (std::find(libs.begin(), libs.end(), p) == libs.end()) { libs.push_back(p); }
		}
	}
	for (const std::string& lib : libs)
	{
		const std::string manifest = readFile(lib + "/steamapps/appmanifest_233860.acf");
		const std::string dir = steamvdf::installDir(manifest);
		if (!dir.empty()) { out.push_back({ "Steam library " + lib + " (appmanifest_233860)", lib + "/steamapps/common/" + dir }); }
		else { out.push_back({ "Steam library " + lib, lib + "/steamapps/common/Kenshi" }); }
	}
}

#if defined(_WIN32)
// HKCU\Software\Valve\Steam SteamPath (compile-checked only by reading: no Windows build on ds).
std::string steamPathFromRegistry()
{
	char buf[1024];
	DWORD size = sizeof buf;
	if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) { return std::string(); }
	return std::string(buf);
}
#endif

#if defined(__APPLE__)
std::string exeDir()
{
	char buf[4096];
	uint32_t size = sizeof buf;
	if (_NSGetExecutablePath(buf, &size) != 0) { return std::string(); }
	return fs::path(buf).parent_path().string();
}
#endif

// The installs the three desktop platforms have by default (the brief names them; kenshi-379.13).
std::vector<Place> defaultInstalls()
{
	std::vector<Place> places;
#ifdef KENSHI_DATA
	// A dev build only (client/CMakeLists.txt): the developer's own install, so a checkout runs with no flags. Never in a release build.
	places.push_back({ "build default (KENSHI_DATA)", KENSHI_DATA });
#endif
	const std::string home = homePath();
#if defined(_WIN32)
	for (const char* programFiles : { "ProgramFiles(x86)", "ProgramFiles" })
	{
		const std::string pf = env(programFiles);
		if (pf.empty()) { continue; }
		places.push_back({ "Steam (" + pf + ")", pf + "\\Steam\\steamapps\\common\\Kenshi" });
		places.push_back({ "GOG Galaxy (" + pf + ")", pf + "\\GOG Galaxy\\Games\\Kenshi" });
		addSteamLibraries(places, pf + "\\Steam");
	}
	if (const std::string sp = steamPathFromRegistry(); !sp.empty())
	{
		places.push_back({ "Steam (registry SteamPath)", sp + "/steamapps/common/Kenshi" });
		addSteamLibraries(places, sp);
	}
	// Believed, not checked: GOG's installer default outside Galaxy.
	places.push_back({ "GOG Games (believed)", "C:\\GOG Games\\Kenshi" });
#elif defined(__APPLE__)
	places.push_back({ "Steam", home + "/Library/Application Support/Steam/steamapps/common/Kenshi" });
	addSteamLibraries(places, home + "/Library/Application Support/Steam");
	if (const std::string here = exeDir(); !here.empty()) { places.push_back({ "a Kenshi folder beside the binary", here + "/Kenshi" }); }
#else
	places.push_back({ "Steam (~/.steam)", home + "/.steam/steam/steamapps/common/Kenshi" });
	places.push_back({ "Steam (~/.local/share)", home + "/.local/share/Steam/steamapps/common/Kenshi" });
	addSteamLibraries(places, home + "/.steam/steam");
	addSteamLibraries(places, home + "/.local/share/Steam");
	places.push_back({ "GOG (Heroic)", home + "/Games/Heroic/Kenshi" });
	places.push_back({ "GOG (minigalaxy)", home + "/GOG Games/Kenshi" });
#endif
	return places;
}

std::string howToSet()
{
	std::string s;
	s += "  --data DIR\n";
	s += "  KENSHI_DATA=/path/to/Kenshi/data\n";
	s += "  or the line \"data=/path/to/Kenshi/data\" in " + datadir::configFile() + "\n";
	s += "DIR is the game's data folder, the one holding gamedata.base.\n";
#if defined(__APPLE__)
	s += "Kenshi has no macOS build: a copied Windows install works, point --data at its data folder.\n";
#endif
	return s;
}
}

std::string datadir::configFile()
{
#if defined(_WIN32)
	const std::string base = env("APPDATA").empty() ? std::string(".") : env("APPDATA");
	return base + "\\kenshi-client\\data.conf";
#elif defined(__APPLE__)
	return homePath() + "/Library/Application Support/kenshi-client/data.conf";
#else
	const std::string config = env("XDG_CONFIG_HOME").empty() ? homePath() + "/.config" : env("XDG_CONFIG_HOME");
	return config + "/kenshi-client/data.conf";
#endif
}

bool datadir::resolve(const std::string& cli, std::string& out, std::string& message)
{
	std::vector<Place> looked;

	// 1. --data: an explicit path must be a Kenshi install (a typo should not silently select another one).
	looked.push_back({ "--data", cli.empty() ? "(not given)" : cli });
	if (!cli.empty())
	{
		if (const std::string dir = dataDirOf(cli); !dir.empty()) { out = dir; return true; }
		message = "--data " + cli + " is not a Kenshi data folder (no gamedata.base, and no data/gamedata.base).\nSet it with:\n" + howToSet();
		return false;
	}

	// 2. $KENSHI_DATA, the variable the docs and the build use.
	const std::string envData = env("KENSHI_DATA");
	looked.push_back({ "$KENSHI_DATA", envData.empty() ? "(not set)" : envData });
	if (!envData.empty())
	{
		if (const std::string dir = dataDirOf(envData); !dir.empty()) { out = dir; return true; }
		message = "$KENSHI_DATA " + envData + " is not a Kenshi data folder (no gamedata.base, and no data/gamedata.base).\nSet it with:\n" + howToSet();
		return false;
	}

	// 3. The saved setting; a path the game has moved from is stale, not an error: the search below replaces it.
	const std::string saved = readSetting();
	looked.push_back({ "saved setting (" + configFile() + ")",
		!settingEnabled() ? "(ignored: KCLIENT_CONFIG=none)" : saved.empty() ? "(not set)" : saved });
	if (!saved.empty())
	{
		if (const std::string dir = dataDirOf(saved); !dir.empty()) { out = dir; return true; }
		looked.back().path += " (no gamedata.base any more)";
	}

	// 4. The default installs; a hit is saved, so the next start looks nowhere.
	const std::vector<Place> installs = defaultInstalls();
	for (const Place& p : installs) { looked.push_back(p); }
	for (const Place& p : installs)
	{
		if (const std::string dir = dataDirOf(p.path); !dir.empty())
		{
			out = dir;
			saveSetting(dir);
			const std::string note = settingEnabled() ? "; saved to " + configFile() : "";
			fprintf(stderr, "kclient: game data: %s (found at %s%s)\n", dir.c_str(), p.label.c_str(), note.c_str());
			return true;
		}
	}

	// 5. Nothing found: ask with the native folder picker (datapicker.cpp; never without a display, under KCLIENT_NO_DIALOG=1 or KCLIENT_CONFIG=none).
	if (datapicker::available())
	{
		if (const std::string dir = datapicker::pick(dataDirOf); !dir.empty())
		{
			out = dir;
			saveSetting(dir);
			fprintf(stderr, "kclient: game data: %s (picked; saved to %s)\n", dir.c_str(), configFile().c_str());
			return true;
		}
	}

	message = "cannot find the Kenshi game data.\nLooked in:\n";
	for (const Place& p : looked) { message += "  " + p.label + ": " + p.path + "\n"; }
	message += "Set it with:\n" + howToSet();
	return false;
}
