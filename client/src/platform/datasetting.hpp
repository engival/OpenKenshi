#pragma once
#include <string>

// The client's half of the game-data lookup (kenshi-u70n.1). Finding the install (--data, $KENSHI_DATA, the Steam/GOG/bundle defaults) is
// kdata::datadir (cpp/include/kdata/datadir.hpp); what stays here is client-only: the saved setting (one "data=PATH" line in the per-user config
// dir), the build's own KENSHI_DATA in a dev build, and the first-run folder picker (app/datapicker.hpp).
namespace datasetting
{
	// The file the setting lives in: <app/userdirs.hpp plat::userConfigDir()>/kenshi-client/data.conf.
	std::string configFile();

	// Resolves the data dir. `cli` is --data's value (empty = not given). On success `out` is the data folder and, when a default install or the
	// picker found it, the setting is saved to configFile() (not under KCLIENT_CONFIG=none). false: nothing valid; `message` has the places
	// looked and the ways to set one.
	bool resolve(const std::string& cli, std::string& out, std::string& message);
}
