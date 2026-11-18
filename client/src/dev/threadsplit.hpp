#pragma once
// The render thread's split on the stages line (kenshi-k16.2.205.12.84, dev-only: KCLIENT_DEV_TOOLS, printed only with KCLIENT_STAGES).
// Appends to the frame's line, between logic_frame and render_frame (both render.thread modes):
//   | lt.logic L | lt.handoff H | lt.rwait R | lt.present P | lt.gate G | lt.period T
// lt.logic   pk.logicMs: the world path's logic half (logic_frame's start to its end).
// lt.handoff logic_frame's end to render_frame's start: the hand-off (lockstep: mark filled, notify, the render thread's wake and flip; off: ~0).
// lt.rwait   the render thread's wait for a filled packet in this frame callback (lockstep only; off 0).
// lt.present the sapp thread's time outside frame() before this callback: the swap / present / vsync wait and sokol's event handling.
// lt.gate    logic's wait at the gate (logicthread::renderIdle: logic waits until the render thread is done with the last packet), summed over
//            the frame, ms (kenshi-k16.2.205.12.94, overlap review A5). 0 until a gate exists (lockstep's own wait is lt.handoff / the idle
//            wait); the gate's caller reports through gateWait(). Stage 2 of the overlap starts when its mean is over 0.5 ms at a user camera.
// lt.period  the render thread's time since the previous split, i.e. since the previous drawn world frame's draw start: the frame period
//            (kenshi-k16.2.205.12.96: in overlap the line's total spans logic's start to the draw's end, a latency, not the period). 0 first.
// The draw's own CPU is the line's cpu minus lt.logic. Under lockstep logic N+1 runs while the sapp thread presents N.
#ifdef KCLIENT_DEV_TOOLS
#include <atomic>
#include <chrono>
#include <cstdio>
#include "../cpustage.hpp"

namespace threadsplit
{
	using Clock = std::chrono::steady_clock;
	struct State { Clock::time_point in, out, lastSplit; double presentMs = 0; };
	inline std::atomic<double>& gateMs() { static std::atomic<double> g{ 0 }; return g; }	// written by the logic thread, taken by split (any thread)
	// The gate's caller (logic thread): the ms this wait took.
	inline void gateWait(double ms) { double o = gateMs().load(); while (!gateMs().compare_exchange_weak(o, o + ms)) {} }
	inline State& state() { static State s; return s; }	// the sapp (render) thread only
	inline double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }
	// frame()'s entry and exit, on the sapp thread.
	inline void enter()
	{
		State& s = state();
		s.in = Clock::now();
		s.presentMs = s.out.time_since_epoch().count() ? ms(s.out, s.in) : 0;
	}
	inline void leave() { state().out = Clock::now(); }
	// Just before render_frame: logicEnd = logic's start + its ms (the packet's), lockstep says whether a wait for the packet happened in this callback.
	inline void split(Clock::time_point logicStart, double logicMs, bool lockstep)
	{
		if (!cpustage::on()) { return; }
		State& s = state();
		const auto now = Clock::now();
		const double period = s.lastSplit.time_since_epoch().count() ? ms(s.lastSplit, now) : 0.0;
		s.lastSplit = now;
		const double handoff = ms(logicStart, now) - logicMs;
		char buf[192];
		snprintf(buf, sizeof buf, " | lt.logic %.2f | lt.handoff %.2f | lt.rwait %.2f | lt.present %.2f | lt.gate %.2f | lt.period %.2f", logicMs, handoff < 0 ? 0 : handoff,
			lockstep ? ms(s.in, now) : 0.0, s.presentMs, gateMs().exchange(0.0), period);
		cpustage::extra(buf);
	}
}
#endif
