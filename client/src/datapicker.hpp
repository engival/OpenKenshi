#pragma once
#include <functional>
#include <string>

// The first-run folder picker (kenshi-379.28.1): when datadir::resolve finds no Kenshi install, ask where it is with the platform's native folder
// dialog (tinyfiledialogs, third_party/tinyfiledialogs). Runs before any window exists. Never a dialog without a display (Linux: neither DISPLAY
// nor WAYLAND_DISPLAY), under KCLIENT_NO_DIALOG=1 or KCLIENT_CONFIG=none (headless.sh sets both).
namespace datapicker
{
	// True when a dialog may be shown.
	bool available();

	// Asks until the user picks a folder `dataDirOf` accepts (returns the data folder it gave) or cancels / no dialog tool exists (returns
	// empty). A wrong folder reopens the picker with the reason in its title. `dataDirOf` returns "" for a folder that is not a Kenshi install.
	std::string pick(const std::function<std::string(const std::string&)>& dataDirOf);
}
