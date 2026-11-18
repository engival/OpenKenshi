// Unit check of the Steam VDF parsing behind the data-dir discovery (steamvdf.hpp, kenshi-379.36.1): no files, no game.
//   kdatadir_test   exit 0 and "kdatadir_test: ok" when every check holds
#include <cstdio>
#include <string>
#include <vector>

#include "app/steamvdf.hpp"

namespace
{
int gFails = 0;

void check(bool ok, const char* what)
{
	if (!ok) { fprintf(stderr, "kdatadir_test: FAIL %s\n", what); gFails++; }
}

// The current format, with apps blocks (made-up paths). Every "appid" "size" line must stay out of the result.
const char* kCurrent = R"VDF("libraryfolders"
{
	"0"
	{
		"path"		"/srv/u/.local/share/Steam"
		"label"		""
		"contentid"		"1234567890"
		"totalsize"		"0"
		"apps"
		{
			"228980"		"1238672805"
			"233860"		"5498126251"
			"250820"		"353645464"
		}
	}
	"1"
	{
		"path"		"/mnt/games/SteamLibrary"
		"label"		"games"
		"apps"
		{
			"427520"		"5368970239"
		}
	}
}
)VDF";

// The old flat format: numbered keys at depth 1 hold the paths; the other keys do not.
const char* kOld = R"VDF("LibraryFolders"
{
	"TimeNextStatsReport"		"1400000000"
	"ContentStatsID"		"-123456789"
	"1"		"/mnt/old/SteamLibrary"
	"2"		"/mnt/older/SteamLibrary"
}
)VDF";

// A Windows path with escaped backslashes, and an escaped quote in a label that must not end the string.
const char* kWindows = R"VDF("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"label"		"say \"hi\" there"
		"apps"
		{
			"233860"		"100"
		}
	}
	"1"
	{
		"path"		"D:\\SteamLibrary"
	}
}
)VDF";

const char* kManifest = R"ACF("AppState"
{
	"appid"		"233860"
	"Universe"		"1"
	"name"		"Kenshi"
	"installdir"		"Kenshi"
	"UserConfig"
	{
		"installdir"		"Wrong"
	}
}
)ACF";

bool is(const std::vector<std::string>& v, std::initializer_list<const char*> want)
{
	if (v.size() != want.size()) { return false; }
	std::size_t i = 0;
	for (const char* w : want) { if (v[i++] != w) { return false; } }
	return true;
}
}

int main()
{
	const std::vector<std::string> cur = steamvdf::libraryPaths(kCurrent);
	check(is(cur, { "/srv/u/.local/share/Steam", "/mnt/games/SteamLibrary" }), "current format: two libraries, no apps entry");

	const std::vector<std::string> old = steamvdf::libraryPaths(kOld);
	check(is(old, { "/mnt/old/SteamLibrary", "/mnt/older/SteamLibrary" }), "old flat format: numbered keys only");

	const std::vector<std::string> win = steamvdf::libraryPaths(kWindows);
	check(is(win, { "C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary" }), "windows escapes undone, escaped quote does not stop the scan");

	check(steamvdf::libraryPaths("").empty(), "empty file");
	check(steamvdf::libraryPaths("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\n").empty(), "truncated file");

	check(steamvdf::installDir(kManifest) == "Kenshi", "installdir at depth 1 only");
	check(steamvdf::installDir("\"AppState\"\n{\n\t\"name\"\t\"x\"\n}\n").empty(), "manifest without installdir");

	if (gFails == 0) { printf("kdatadir_test: ok\n"); }
	return gFails == 0 ? 0 : 1;
}
