#pragma once
// KCLIENT_FRAMELOG=boom,predict,drive,cursor,hud,pick (or `all`), dev-only (kenshi-jvhw, k16.2.522): the per-frame / per-step diagnostics that a
// scripted run used to print into every log. Unset (and in a build without KCLIENT_DEV_TOOLS): none of them prints. Families:
//   boom     the follow camera's boom line each frame ("boom:")
//   predict  the predicted-versus-server position every 10th frame ("predict: shown")
//   drive    each direct-drive order sent ("drive N strafe M heading ...")
//   cursor   each world-cursor change ("cursor: NAME at X Y")
//   hud      the squad's health / detect / medical block every 300th frame ("hud: NAME ...")
//   pick     the pick cost summaries ("pickat cost", "pickbody cost", "pickfoliage cost"), also needs --debug
// Use once per site: `static const bool on = framelog::on("boom");`
#ifdef KCLIENT_DEV_TOOLS
#include <cstdlib>
#include <cstring>
namespace framelog
{
inline bool on(const char* family)
{
	const char* v = std::getenv("KCLIENT_FRAMELOG");
	if (!v) { return false; }
	const size_t n = std::strlen(family);
	for (const char* p = v; *p;)
	{
		const char* e = std::strchr(p, ',');
		const size_t l = e ? (size_t)(e - p) : std::strlen(p);
		if ((l == n && !std::strncmp(p, family, n)) || (l == 3 && !std::strncmp(p, "all", 3))) { return true; }
		if (!e) { break; }
		p = e + 1;
	}
	return false;
}
}
#else
namespace framelog { inline bool on(const char*) { return false; } }
#endif
