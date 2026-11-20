#pragma once
// Removal A/B switches for upper-bound performance measurements (kenshi-k16.2.205.7, dev builds): KCLIENT_ABL holds a comma-separated token list;
// each caller asks for its token. Every switch removes real work to bound what that work could ever give back: the output is wrong by design (a bind
// pose, a character with no armour) and a run with a token set is a measurement, never a playtest. Tokens:
//   charshadow  no character shadow casters (Characters::renderShadow skipped)
//   nosample    bind pose: no clip sampling / layer blending per bone (the hierarchy and palette still run)
//   noprops     no per-frame props list rebuild: the buildings keep the last frame's props
// A release client (KCLIENT_DEV_TOOLS off) has no ablations: every token reads false.
#include <cstdlib>
#include <string>

namespace ablate
{
	inline bool on(const char* token)
	{
#ifdef KCLIENT_DEV_TOOLS
		static const std::string list = std::getenv("KCLIENT_ABL") ? std::getenv("KCLIENT_ABL") : "";
		return !list.empty() && list.find(token) != std::string::npos;
#else
		(void)token;
		return false;
#endif
	}
}
