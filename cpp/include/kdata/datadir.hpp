#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Finding the Kenshi game data directory without a flag (moved from client/src/app/datadir.cpp, kenshi-k16.6.421; the client and ksim_server
// share it). Tried in order: --data, $KENSHI_DATA, the caller's extra places, then the default Steam/GOG installs of the three desktop
// platforms (Steam libraries via libraryfolders.vdf) and, on macOS, the app bundle's own folders. A path is valid when it is the game's data
// folder (it holds gamedata.base) or an install root (data/gamedata.base). The client's saved setting and folder picker stay in the client.
namespace kdata::datadir {

struct Place
{
	std::string label, path;
};

struct Result
{
	bool found = false;
	std::string dir;		// found: the data folder
	std::string matched;	// found: the label of the place that held it
	std::vector<Place> tried;	// every place looked at, in order (the unset ones too, path "(not given)" / "(not set)")
	std::string message;	// not found: what was looked at and the ways to set one
};

// $KENSHI_DATA-style resolve. `cli` is --data's value (empty = not given); an explicit --data / $KENSHI_DATA that is not a Kenshi folder
// fails at once (a typo must not select another install). `extra` places are tried after the variable, before the defaults.
Result resolve(const std::string& cli, const std::vector<Place>& extra = {});

// The data folder a path names (itself, or its data/ child), normalised; empty = not a Kenshi install.
std::string dataDirOf(const std::string& path);

// The places an executable inside a macOS app bundle (Foo.app/Contents/MacOS/foo) can hold the game: a Kenshi folder beside the binary,
// Contents/Resources and Contents/Resources/Kenshi. Pure path logic (any platform); resolve() uses it on macOS only.
std::vector<Place> bundlePlaces(const std::filesystem::path& executable);

}
