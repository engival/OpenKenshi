// The `hints at X Y` and `hints kinds` script ops (kenshi-k16.2.294.2, control-hints step 0; DESIGN 5): `hints at` prints the resolved hint list
// for a screen point, exactly what the F12 sidecar's `hints:` block shows there; `hints kinds` prints the kind registry the guide-coverage check
// greps (one `kind <id> "<phrase>"` line each). The resolution is host side (main.cpp's hint_report, through Context::hintsAt): it reads the app's
// selection, modifiers and pick, which this file has no business knowing; the registry is controlhints.hpp's own. Built only with KCLIENT_DEV_TOOLS.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "controlhints.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"

namespace dev
{
namespace
{
bool number(const std::string& s, double& out)
{
	char* end = nullptr;
	out = std::strtod(s.c_str(), &end);
	return end != s.c_str() && !*end;
}
}

bool hintsOp(const Context& cx, const std::vector<std::string>& a)
{
	if (a.empty()) { return false; }
	if (a[0] == "kinds")
	{
		if (a.size() != 1) { return false; }
		printf("hints kinds: %zu registered\n", hints::hint_kinds().size());
		for (const hints::HintKind& k : hints::hint_kinds()) { printf("kind %s \"%s\"\n", k.id, k.phrase); }
		return true;
	}
	if (a[0] == "at")
	{
		double mx, my;
		if (a.size() != 3 || !number(a[1], mx) || !number(a[2], my)) { return false; }
		fputs(cx.hintsAt(mx, my).c_str(), stdout);
		return true;
	}
	return false;
}
}	// namespace dev
