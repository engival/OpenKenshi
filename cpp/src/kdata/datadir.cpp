#include "kdata/datadir.hpp"

#include "kdata/steamvdf.hpp"
#include "kutil/executable.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace kdata::datadir {

namespace {
struct InstallPlace
{
	std::string label, path;
	bool steamRoot = false;	// a Steam root: the libraries its libraryfolders.vdf lists are added at this point of the order
};

std::string env(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr ? std::string(v) : std::string();
}

bool holdsGameData(const fs::path& p)
{
	std::error_code ec;
	return fs::is_regular_file(p / "gamedata.base", ec);
}

std::string readFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

#if defined(_WIN32)
// A string value under HKCU or HKLM (any registry view), or empty.
std::string regString(HKEY root, const char* key, const char* value)
{
	char buf[1024];
	DWORD size = sizeof buf;
	if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) { return std::string(); }
	return std::string(buf);
}

// Steam's and GOG's Windows defaults, and Steam's own record of where it lives (kenshi-379.13; untested on a real Windows install).
std::vector<InstallPlace> installPlaces()
{
	std::vector<InstallPlace> places;
	for (const char* programFiles : { "ProgramFiles(x86)", "ProgramFiles" })
	{
		const std::string pf = env(programFiles);
		if (pf.empty()) { continue; }
		places.push_back({ "Steam (" + pf + ")", pf + "\\Steam\\steamapps\\common\\Kenshi" });
		places.push_back({ "GOG Galaxy (" + pf + ")", pf + "\\GOG Galaxy\\Games\\Kenshi" });
		places.push_back({ "Steam root " + pf, pf + "\\Steam", true });
	}
	// Steam's own record: HKCU SteamPath (the running user's client), HKLM InstallPath (the installer's, 32-bit view on 64-bit Windows).
	if (const std::string sp = regString(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath"); !sp.empty())
	{
		places.push_back({ "Steam (registry SteamPath)", sp + "/steamapps/common/Kenshi" });
		places.push_back({ "Steam root (registry SteamPath)", sp, true });
	}
	if (const std::string ip = regString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath"); !ip.empty())
	{
		places.push_back({ "Steam (registry InstallPath)", ip + "\\steamapps\\common\\Kenshi" });
		places.push_back({ "Steam root (registry InstallPath)", ip, true });
	}
	// GOG (Galaxy and the offline installer) records each game under HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<product id>: gameName, path.
	HKEY games = nullptr;
	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\GOG.com\\Games", 0, KEY_READ, &games) == ERROR_SUCCESS)
	{
		char id[256];
		for (DWORD i = 0;; ++i)
		{
			DWORD len = sizeof id;
			if (RegEnumKeyExA(games, i, id, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) { break; }
			const std::string sub = std::string("SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + id;
			if (regString(HKEY_LOCAL_MACHINE, sub.c_str(), "gameName") != "Kenshi") { continue; }
			if (const std::string path = regString(HKEY_LOCAL_MACHINE, sub.c_str(), "path"); !path.empty()) { places.push_back({ "GOG (registry)", path }); }
		}
		RegCloseKey(games);
	}
	// Believed, not checked: GOG's installer default outside Galaxy.
	places.push_back({ "GOG Games (believed)", "C:\\GOG Games\\Kenshi" });
	return places;
}

std::string dataHint() { return std::string(); }

#elif defined(__APPLE__)
std::vector<InstallPlace> installPlaces()
{
	const std::string home = env("HOME");
	std::vector<InstallPlace> places = {
		{ "Steam", home + "/Library/Application Support/Steam/steamapps/common/Kenshi" },
		{ "Steam root", home + "/Library/Application Support/Steam", true },
	};
	std::error_code ec;
	const fs::path exe = kutil::executablePath(ec);
	if (!ec) { for (const Place& p : bundlePlaces(exe)) { places.push_back({ p.label, p.path }); } }
	return places;
}

std::string dataHint() { return "Kenshi has no macOS build: a copied Windows install works, point --data at its data folder.\n"; }

#else
std::vector<InstallPlace> installPlaces()
{
	const std::string home = env("HOME");
	return {
		{ "Steam (~/.steam)", home + "/.steam/steam/steamapps/common/Kenshi" },
		{ "Steam (~/.local/share)", home + "/.local/share/Steam/steamapps/common/Kenshi" },
		{ "Steam root ~/.steam", home + "/.steam/steam", true },
		{ "Steam root ~/.local/share", home + "/.local/share/Steam", true },
		{ "GOG (Heroic)", home + "/Games/Heroic/Kenshi" },
		{ "GOG (minigalaxy)", home + "/GOG Games/Kenshi" },
	};
}

std::string dataHint() { return std::string(); }
#endif

// The Steam libraries a Steam root lists: steamapps/libraryfolders.vdf and config/libraryfolders.vdf, parsed by nesting (steamvdf.hpp), deduped.
// Kenshi's folder is the one steamapps/appmanifest_233860.acf names ("installdir"); the plain steamapps/common/Kenshi stays as the fallback
// place when no manifest names it.
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
		const std::string dir = steamvdf::installDir(readFile(lib + "/steamapps/appmanifest_233860.acf"));
		if (!dir.empty()) { out.push_back({ "Steam library " + lib + " (appmanifest_233860)", lib + "/steamapps/common/" + dir }); }
		else { out.push_back({ "Steam library " + lib, lib + "/steamapps/common/Kenshi" }); }
	}
}

std::string howToSet()
{
	return "  --data DIR\n  KENSHI_DATA=/path/to/Kenshi/data\nDIR is the game's data folder, the one holding gamedata.base.\n" + dataHint();
}

Result fail(Result r, const std::string& what)
{
	r.message = what + " is not a Kenshi data folder (no gamedata.base, and no data/gamedata.base).\nSet it with:\n" + howToSet();
	return r;
}

Result hit(Result r, const std::string& dir, const std::string& label)
{
	r.found = true;
	r.dir = dir;
	r.matched = label;
	return r;
}
}

std::string dataDirOf(const std::string& path)
{
	if (path.empty()) { return std::string(); }
	const fs::path p(path);
	if (holdsGameData(p)) { return p.lexically_normal().string(); }
	if (holdsGameData(p / "data")) { return (p / "data").lexically_normal().string(); }
	return std::string();
}

std::vector<Place> bundlePlaces(const fs::path& executable)
{
	const fs::path here = executable.parent_path();	// Foo.app/Contents/MacOS
	const fs::path resources = here.parent_path() / "Resources";
	return {
		{ "a Kenshi folder beside the binary", (here / "Kenshi").string() },
		{ "the app bundle's Contents/Resources", resources.string() },
		{ "the app bundle's Contents/Resources/Kenshi", (resources / "Kenshi").string() },
	};
}

Result resolve(const std::string& cli, const std::vector<Place>& extra)
{
	Result r;

	// 1. --data: an explicit path must be a Kenshi install.
	r.tried.push_back({ "--data", cli.empty() ? "(not given)" : cli });
	if (!cli.empty())
	{
		const std::string dir = dataDirOf(cli);
		return dir.empty() ? fail(r, "--data " + cli) : hit(r, dir, "--data");
	}

	// 2. $KENSHI_DATA, the variable the docs and the build use.
	const std::string envData = env("KENSHI_DATA");
	r.tried.push_back({ "$KENSHI_DATA", envData.empty() ? "(not set)" : envData });
	if (!envData.empty())
	{
		const std::string dir = dataDirOf(envData);
		return dir.empty() ? fail(r, "$KENSHI_DATA " + envData) : hit(r, dir, "$KENSHI_DATA");
	}

	// 3. The caller's places (the build's baked KENSHI_DATA, the client's saved setting), then the OS defaults.
	std::vector<Place> places = extra;
	for (const InstallPlace& p : installPlaces())
	{
		if (p.steamRoot) { addSteamLibraries(places, p.path); }
		else { places.push_back({ p.label, p.path }); }
	}
	r.tried.insert(r.tried.end(), places.begin(), places.end());
	for (const Place& p : places)
	{
		if (const std::string dir = dataDirOf(p.path); !dir.empty()) { return hit(r, dir, p.label); }
	}

	r.message = "cannot find the Kenshi game data.\nLooked in:\n";
	for (const Place& p : r.tried) { r.message += "  " + p.label + ": " + p.path + "\n"; }
	r.message += "Set it with:\n" + howToSet();
	return r;
}

}
