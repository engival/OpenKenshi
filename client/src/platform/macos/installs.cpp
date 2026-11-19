#include "platform/installs.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>

#include <mach-o/dyld.h>

namespace plat {

namespace {
std::string home()
{
	const char* v = getenv("HOME");
	return v != nullptr ? std::string(v) : std::string();
}

std::string exeDir()
{
	char buf[4096];
	uint32_t size = sizeof buf;
	if (_NSGetExecutablePath(buf, &size) != 0) { return std::string(); }
	return std::filesystem::path(buf).parent_path().string();
}
}

std::vector<InstallPlace> installPlaces()
{
	std::vector<InstallPlace> places = {
		{ "Steam", home() + "/Library/Application Support/Steam/steamapps/common/Kenshi" },
		{ "Steam root", home() + "/Library/Application Support/Steam", true },
	};
	if (const std::string here = exeDir(); !here.empty()) { places.push_back({ "a Kenshi folder beside the binary", here + "/Kenshi" }); }
	return places;
}

std::string userConfigDir() { return home() + "/Library/Application Support"; }

std::string dataHint() { return "Kenshi has no macOS build: a copied Windows install works, point --data at its data folder.\n"; }

}
