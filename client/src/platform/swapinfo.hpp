#pragma once
// The GLX swap interval in effect (kenshi-k16.2.418.1): one stderr line at startup, so a "does it even vsync" question has a fact behind it.
#include <string>

namespace swapinfo
{
// The interval the window's drawable really has, asked of the GLX driver on the calling thread (which must own the GL context, after a first
// swap): "<n> via GLX_EXT_swap_control", "<n> via GLX_MESA_swap_control", or "n/a (...)" with the reason. Empty-reason "n/a" off Linux/GLX.
std::string effective();
// The vblank counter (MSC) of the window's drawable by glXGetSyncValuesOML (GLX_OML_sync_control, found with glXGetProcAddress; nothing is hooked), on
// the thread that owns the context. False: not available (no context, no extension, other OS) and `msc` untouched. Dev pace log only (dev/pacelog.cpp).
bool vblankCount(long long& msc);
}
