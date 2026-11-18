#pragma once
// The mesh viewer's command-line side (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.11, pass 7 of 7): --view-mesh, --tex and
// --dist. dev/devhost.cpp's parseArg hands its flag here; the viewer itself is dev/viewmesh.cpp, built only with KCLIENT_DEV_TOOLS and reached
// through dev/devhooks.hpp. Plain declarations only, so the non-gated devhost.cpp can call it in every configuration that builds it.
#include <string>
#include <vector>

namespace dev
{
	// A --view-mesh / --tex / --dist flag: true when taken. windowOff gets the argv text of --view-mesh (windowgeom stays unarmed).
	bool viewArg(int argc, char** argv, int& i, std::vector<std::string>& windowOff);
}
