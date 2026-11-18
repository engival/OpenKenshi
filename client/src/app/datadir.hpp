#pragma once
#include <string>

// The game data directory without a flag (kenshi-379.13, user 2026-11-13: the preview "needs to launch into the game and main menu with NO cli
// options"). One resolver, tried in order: --data, $KENSHI_DATA, a saved setting (one "data=PATH" line in the per-user config dir), then the
// Steam/GOG default installs of the three desktop platforms (plus the build's own KENSHI_DATA in a dev build). A path is valid when it is the
// game's data folder (it holds gamedata.base) or an install root (data/gamedata.base).
namespace datadir
{
	// The file the setting lives in: $XDG_CONFIG_HOME/kenshi-client/data.conf (or ~/.config/...), ~/Library/Application Support/kenshi-client/
	// data.conf on macOS, %APPDATA%\kenshi-client\data.conf on Windows.
	std::string configFile();

	// Resolves the data dir. `cli` is --data's value (empty = not given). On success `out` is the data folder and, when the search found it,
	// the setting is saved to configFile() (not under KCLIENT_CONFIG=none, headless.sh's "no per-user state"). false: nothing valid; `message`
	// has the places looked and the three ways to set one.
	bool resolve(const std::string& cli, std::string& out, std::string& message);
}
