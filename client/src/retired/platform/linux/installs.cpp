#include "platform/installs.hpp"

#include <cstdlib>

namespace plat {

namespace {
std::string env(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr ? std::string(v) : std::string();
}
}

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

std::string userConfigDir()
{
	const std::string xdg = env("XDG_CONFIG_HOME");
	return xdg.empty() ? env("HOME") + "/.config" : xdg;
}

std::string dataHint() { return std::string(); }

}
