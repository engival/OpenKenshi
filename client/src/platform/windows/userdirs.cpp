#include "app/userdirs.hpp"

#include <cstdlib>

// The per-user roots on Windows (kenshi-379.31.3): under OpenKenshi in the user's own application data, never XDG_* / HOME (under Proton
// Wine leaves the Linux HOME set, and on real Windows it usually is not set at all; either way the client's files must not land in the
// Linux client's ~/.cache/kclient or beside the exe). app/userdirs.hpp has the whole rule.
namespace plat {

namespace {
std::string env(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr ? std::string(v) : std::string();
}

// %LOCALAPPDATA% (per-machine local app data); %APPDATA% is the fallback for a stripped environment.
std::string localAppData()
{
	std::string v = env("LOCALAPPDATA");
	if (v.empty()) { v = env("APPDATA"); }
	return v.empty() ? std::string(".") : v;
}
}

std::string userHomeDir()
{
	const std::string v = env("USERPROFILE");
	return v.empty() ? std::string(".") : v;
}

std::string userConfigDir()
{
	const std::string v = env("APPDATA");
	return (v.empty() ? std::string(".") : v) + "/OpenKenshi";
}

std::string userCacheDir()
{
	return localAppData() + "/OpenKenshi/cache";
}

std::string userDataDir()
{
	return localAppData() + "/OpenKenshi/data";
}

}
