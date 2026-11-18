#pragma once
// The scene test's command-line side (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.11, pass 7 of 7): --scene-test, --scenes,
// --scene-file, --scene-hud and --scene-frames. dev/devhost.cpp's parseArg hands its flag here, so main.cpp carries none of the names; the scene
// code itself is dev/scenetest.cpp, built only with KCLIENT_DEV_TOOLS and reached through dev/devhooks.hpp. Plain declarations only, so the
// non-gated devhost.cpp can call it in every configuration that builds it.
#include <string>
#include <vector>

namespace dev
{
	// A --scene-* flag: true when taken. windowOff gets the argv text of the value flags (windowgeom stays unarmed, as before).
	bool sceneArg(int argc, char** argv, int& i, std::vector<std::string>& windowOff);
}
