// Skips a test that reads the game install when there is none (exit 77, ctest's SKIP_RETURN_CODE). cpp/CMakeLists.txt adds this source to every
// test target compiled with KENSHI_DATA (the block at the end of the file), so a public checkout without Kenshi installed runs the rest of the
// suite and reports these as skipped; a new test that takes KENSHI_DATA needs no line of its own. A static initializer: the check runs before main.
#ifdef KENSHI_DATA

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

struct NeedsData
{
	NeedsData()
	{
		if (std::filesystem::exists(std::filesystem::path(KENSHI_DATA) / "gamedata.base"))
			return;
		std::cout << "SKIPPED: needs the Kenshi game data (" KENSHI_DATA " has no gamedata.base); configure with -DKENSHI_DATA=DIR or set KENSHI_DATA\n" << std::flush;
		std::_Exit(77);
	}
};

const NeedsData gNeedsData;

} // namespace

#endif
