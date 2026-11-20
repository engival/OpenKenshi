#pragma once
// KCLIENT_PACELOG=1 (kenshi-k16.2.418.1, dev-only): the present pacing log, see dev/pacelog.cpp. Unset: begin(), note() and noteLockstep() are
// one branch each.
namespace logicthread { struct MissStats; }
#ifdef KCLIENT_DEV_TOOLS
namespace pacelog
{
extern bool gOn;	// KCLIENT_PACELOG is set
void init();	// once, from main before the frame callbacks
void beginSlow();
void noteSlow(bool drew);
void noteLockstepSlow(const logicthread::MissStats& m, double logicMs, double boundMs);
inline void begin() { if (gOn) { beginSlow(); } }	// the top of every frame callback, sapp thread
inline void note(bool drew) { if (gOn) { noteSlow(drew); } }	// its end: drew = it ran a pass; false = it returned with the kept frame
// The logic thread's frame, once per callback on the sapp thread (kenshi-k16.2.437.1): the frames without a pass since the last frame
// (logicthread::takeMisses), the world frame's logic ms (logicthread::lastLogicMs) and the wait's bound. The once-a-second line carries their
// sums; no line without it.
inline void noteLockstep(const logicthread::MissStats& m, double logicMs, double boundMs) { if (gOn) { noteLockstepSlow(m, logicMs, boundMs); } }
}
#else
namespace pacelog
{
inline void init() {}
inline void begin() {}
inline void note(bool) {}
inline void noteLockstep(const logicthread::MissStats&, double, double) {}
}
#endif
