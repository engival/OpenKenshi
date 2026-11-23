#pragma once
// KCLIENT_CATCHLOG=1 (kenshi-6q7f, dev-only): one stderr line per base jump that starts or feeds a catch-up (simview.cpp), with what could have caused it:
// the sample pair behind the base, the route version, the clock's last move, the snapshot age. Unset: one branch.
#include <cstdint>

#include "bridge/simview.hpp"
#ifdef KCLIENT_DEV_TOOLS
namespace catchlog
{
extern bool gOn;
void init();
void noteSlow(const SimView::CatchJump& j);
}
#else
namespace catchlog { inline void init() {} }
#endif
