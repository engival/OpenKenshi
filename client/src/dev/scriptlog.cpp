// The `log gait` / `log pos` script ops' dispatch (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.11, pass 7 of 7): the per-frame
// loggers are dev/gaitlog.cpp and dev/poslog.cpp; their branches off main.cpp's exec_cmd are this one hook, so a game build has no `log gait` /
// `log pos` code at all (the rest of `log`, feet and hits, is dev::charsOp). Built only with KCLIENT_DEV_TOOLS.
#include <string>
#include <vector>

#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"

namespace dev
{

bool logOp(const Context& cx, const std::vector<std::string>& a)
{
	if (a.size() != 2) { return false; }
	if (a[0] == "gait")
	{
		if (!gaitOp(a[1])) { return false; }
		if (a[1] != "off") { cx.warnFrozenAnimLog("log gait"); }	// a scripted run holds the animation: say so once (kenshi-h2v.3.4.3)
		return true;
	}
	if (a[0] == "pos") { return posOp(a[1]); }
	return false;
}

}	// namespace dev
