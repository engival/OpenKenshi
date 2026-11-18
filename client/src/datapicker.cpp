#include "datapicker.hpp"

#include <cstdlib>
#include <string>

#include "tinyfiledialogs/tinyfiledialogs.h"

namespace
{
bool envSet(const char* name)
{
	const char* v = getenv(name);
	return v != nullptr && *v != 0;
}
}

bool datapicker::available()
{
	if (envSet("KCLIENT_NO_DIALOG") && std::string(getenv("KCLIENT_NO_DIALOG")) != "0") { return false; }
	if (const char* c = getenv("KCLIENT_CONFIG"); c != nullptr && std::string(c) == "none") { return false; }
#if !defined(_WIN32) && !defined(__APPLE__)
	if (!envSet("DISPLAY") && !envSet("WAYLAND_DISPLAY")) { return false; }
#endif
	return true;
}

std::string datapicker::pick(const std::function<std::string(const std::string&)>& dataDirOf)
{
	tinyfd_allowCursesDialogs = 0;	// no console fallbacks: no tool = NULL = cancel
	tinyfd_forceConsole = 0;
	tinyfd_silent = 1;
#if !defined(_WIN32) && !defined(__APPLE__)
	// tinyfd's own last resort is a prompt on the terminal (it reads stdin); the query form says whether a graphic tool exists, so none = cancel.
	if (tinyfd_selectFolderDialog("tinyfd_query", nullptr) == nullptr) { return std::string(); }
#endif
	std::string title = "Where is Kenshi installed?";
	for (int attempt = 0; attempt < 8; attempt++)
	{
		const char* chosen = tinyfd_selectFolderDialog(title.c_str(), "");
		if (chosen == nullptr || *chosen == 0) { return std::string(); }
		const std::string path = chosen;
		if (const std::string dir = dataDirOf(path); !dir.empty()) { return dir; }
		title = "No gamedata.base in " + path + "; pick the Kenshi install folder";
	}
	return std::string();
}
