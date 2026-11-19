#pragma once
#include <string>
#include <vector>

// The OS half of the game-data lookup (app/datadir.cpp, kenshi-379.13; moved out of datadir.cpp's #ifdefs by kenshi-379.31.1): where Kenshi is
// installed by default, where per-user config lives. One file per OS: platform/linux/installs.cpp, macos/installs.cpp, windows/installs.cpp.
namespace plat {

struct InstallPlace
{
	std::string label, path;
	bool steamRoot = false;	// a Steam root: datadir.cpp adds the libraries its libraryfolders.vdf lists, at this point of the order
};

// The default installs, in the order datadir tries them.
std::vector<InstallPlace> installPlaces();
// The per-user config root datadir's data.conf goes under (%APPDATA%, ~/Library/Application Support, $XDG_CONFIG_HOME or ~/.config).
std::string userConfigDir();
// A line for the "Set it with" help, or empty.
std::string dataHint();

}
