#pragma once
#include <string>
#include <vector>

// The server-setting Options rows (docs/COMBAT_OPTIONS.md, kenshi-h2v.22.7.1), in their own unit so kui_demo shows the same rows as the client.
namespace serverrows
{
	// Appends `--flag on|off` for each row to the local OpenKenshi-server's arguments, unless `args` already names that flag (the user's own wins).
	// The retarget hold is passed only above 0: 0 is the exe's own default, and passing it would pin over a scenario file's retarget_hold.
	void append_flags(std::vector<std::string>& args);
}
