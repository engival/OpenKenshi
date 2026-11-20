// See platform/swapinfo.hpp. Linked libGL's entry points, called on the thread that owns the context (the sapp thread: every render.thread mode
// draws and swaps there; the logic thread has no GL context).
#include "platform/swapinfo.hpp"

#include <GL/glx.h>

#include <cstring>

namespace swapinfo
{
std::string effective()
{
	Display* dpy = glXGetCurrentDisplay();
	const GLXDrawable draw = glXGetCurrentDrawable();
	if (dpy == nullptr || draw == 0) { return "n/a (no current GLX context on this thread)"; }
	const char* ext = glXQueryExtensionsString(dpy, DefaultScreen(dpy));
	if (ext != nullptr && strstr(ext, "GLX_EXT_swap_control") != nullptr)
	{
		unsigned int v = 0;
		glXQueryDrawable(dpy, draw, GLX_SWAP_INTERVAL_EXT, &v);
		return std::to_string(v) + " via GLX_EXT_swap_control (glXQueryDrawable)";
	}
	if (ext != nullptr && strstr(ext, "GLX_MESA_swap_control") != nullptr)
	{
		typedef int (*Get)(void);
		Get get = (Get)glXGetProcAddress((const GLubyte*)"glXGetSwapIntervalMESA");
		if (get != nullptr) { return std::to_string(get()) + " via GLX_MESA_swap_control (glXGetSwapIntervalMESA)"; }
	}
	return "n/a (no GLX swap-control extension)";
}

bool vblankCount(long long& msc)
{
	typedef Bool (*Get)(Display*, GLXDrawable, int64_t*, int64_t*, int64_t*);
	static int state = 0;	// 0 not looked up, 1 usable, -1 not available
	static Get get = nullptr;
	Display* dpy = glXGetCurrentDisplay();
	const GLXDrawable draw = glXGetCurrentDrawable();
	if (dpy == nullptr || draw == 0) { return false; }
	if (state == 0)
	{
		const char* ext = glXQueryExtensionsString(dpy, DefaultScreen(dpy));
		get = ext != nullptr && strstr(ext, "GLX_OML_sync_control") != nullptr ? (Get)glXGetProcAddress((const GLubyte*)"glXGetSyncValuesOML") : nullptr;
		state = get != nullptr ? 1 : -1;
	}
	if (state < 0) { return false; }
	int64_t ust = 0, m = 0, sbc = 0;
	if (!get(dpy, draw, &ust, &m, &sbc)) { return false; }
	msc = m;
	return true;
}
}
