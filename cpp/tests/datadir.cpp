// Unit check of the Steam VDF parsing behind the data-dir discovery (steamvdf.hpp, kenshi-379.36.1): no files, no game.
//   kdata_datadir   exit 0 and "kdata_datadir: ok" when every check holds
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>
#include <vector>

#include "kdata/datadir.hpp"
#include "kdata/steamvdf.hpp"

#ifdef _WIN32
// MinGW UCRT has no POSIX setenv/unsetenv (kenshi-k16.6.487); an empty value removes the variable.
static int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
static int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

namespace
{
int gFails = 0;

void check(bool ok, const char* what)
{
	if (!ok) { fprintf(stderr, "kdata_datadir: FAIL %s\n", what); gFails++; }
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
	const std::vector<std::string> cur = kdata::steamvdf::libraryPaths(kCurrent);
	check(is(cur, { "/srv/u/.local/share/Steam", "/mnt/games/SteamLibrary" }), "current format: two libraries, no apps entry");

	const std::vector<std::string> old = kdata::steamvdf::libraryPaths(kOld);
	check(is(old, { "/mnt/old/SteamLibrary", "/mnt/older/SteamLibrary" }), "old flat format: numbered keys only");

	const std::vector<std::string> win = kdata::steamvdf::libraryPaths(kWindows);
	check(is(win, { "C:\\Program Files (x86)\\Steam", "D:\\SteamLibrary" }), "windows escapes undone, escaped quote does not stop the scan");

	check(kdata::steamvdf::libraryPaths("").empty(), "empty file");
	check(kdata::steamvdf::libraryPaths("\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\n").empty(), "truncated file");

	check(kdata::steamvdf::installDir(kManifest) == "Kenshi", "installdir at depth 1 only");
	check(kdata::steamvdf::installDir("\"AppState\"\n{\n\t\"name\"\t\"x\"\n}\n").empty(), "manifest without installdir");

	// The place lookup: a temp install root holding data/gamedata.base.
	namespace fs = std::filesystem;
	const fs::path root = fs::temp_directory_path() / ("kdata_datadir_" + std::to_string((unsigned long)getpid()));
	fs::create_directories(root / "inst" / "data");
	std::ofstream(root / "inst" / "data" / "gamedata.base") << "x";
	check(kdata::datadir::dataDirOf((root / "inst").string()) == (root / "inst" / "data").string(), "install root -> data/");
	check(kdata::datadir::dataDirOf((root / "inst" / "data").string()) == (root / "inst" / "data").string(), "data folder itself");
	check(kdata::datadir::dataDirOf((root / "nope").string()).empty() && kdata::datadir::dataDirOf("").empty(), "not an install");
	const kdata::datadir::Result viaCli = kdata::datadir::resolve((root / "inst").string());
	check(viaCli.found && viaCli.matched == "--data" && viaCli.tried.size() == 1, "--data hit");
	const kdata::datadir::Result badCli = kdata::datadir::resolve((root / "nope").string());
	check(!badCli.found && badCli.message.find("--data") != std::string::npos, "--data typo fails at once");
	unsetenv("KENSHI_DATA");
	const kdata::datadir::Result viaExtra = kdata::datadir::resolve("", { { "mine", (root / "inst").string() } });
	check(viaExtra.found && viaExtra.matched == "mine" && viaExtra.tried.size() >= 3 && viaExtra.tried[1].path == "(not set)", "extra place, tried list");
	setenv("KENSHI_DATA", (root / "inst").string().c_str(), 1);
	check(kdata::datadir::resolve("").matched == "$KENSHI_DATA", "$KENSHI_DATA hit");
	unsetenv("KENSHI_DATA");
	fs::remove_all(root);

	const std::vector<kdata::datadir::Place> bundle = kdata::datadir::bundlePlaces("/Apps/Foo.app/Contents/MacOS/foo");
	// Compared in generic form: on Windows fs::path joins with '\\' (kenshi-k16.6.492).
	const auto generic = [&](std::size_t i) { return fs::path(bundle[i].path).generic_string(); };
	check(bundle.size() == 3 && generic(0) == "/Apps/Foo.app/Contents/MacOS/Kenshi" && generic(1) == "/Apps/Foo.app/Contents/Resources"
		&& generic(2) == "/Apps/Foo.app/Contents/Resources/Kenshi", "macOS bundle places");

	if (gFails == 0) { printf("kdata_datadir: ok\n"); }
	return gFails == 0 ? 0 : 1;
}
