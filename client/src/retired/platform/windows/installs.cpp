#include "platform/installs.hpp"

#include <cstdlib>

#include <windows.h>

// Steam's and GOG's Windows defaults, and Steam's own record of where it lives (kenshi-379.13; untested on a real Windows install).
namespace plat {

namespace {
std::string env(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr ? std::string(v) : std::string();
}

// A string value under HKCU or HKLM (any registry view), or empty.
std::string regString(HKEY root, const char* key, const char* value)
{
	char buf[1024];
	DWORD size = sizeof buf;
	if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) { return std::string(); }
	return std::string(buf);
}
}

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

std::string userConfigDir()
{
	const std::string appdata = env("APPDATA");
	return appdata.empty() ? std::string(".") : appdata;
}

std::string dataHint() { return std::string(); }

}
