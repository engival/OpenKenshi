#include "app/userdirs.hpp"

#include <cstdlib>

// The per-user roots on macOS (kenshi-379.31.3): config under ~/Library/Application Support as before, cache and data from the XDG variables
// with the $HOME fallbacks the client has always used, byte for byte (app/userdirs.hpp; every caller goes through these).
namespace plat {

namespace {
std::string env(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr ? std::string(v) : std::string();
}

std::string home()
{
	const char* v = getenv("HOME");
	return v != nullptr ? std::string(v) : std::string(".");
}
}

std::string userHomeDir()
{
	const char* v = getenv("HOME");
	return v != nullptr && *v != 0 ? std::string(v) : std::string(".");
}

std::string userConfigDir()
{
	const char* v = getenv("HOME");
	return (v != nullptr ? std::string(v) : std::string()) + "/Library/Application Support";
}

std::string userCacheDir()
{
	const std::string xdg = env("XDG_CACHE_HOME");
	return xdg.empty() ? home() + "/.cache" : xdg;
}

std::string userDataDir()
{
	const std::string xdg = env("XDG_DATA_HOME");
	return xdg.empty() ? home() + "/.local/share" : xdg;
}

}
