// KCLIENT_PACELOG=1 (kenshi-k16.2.418.1, dev-only, docs/CLIENT.md "Dev-only code is fenced"): once a second, one stderr line on how the window is
// presented. No hook of any kind: nothing of libc, libGL or MangoHud is interposed, so it measures the same thing with or without an overlay.
//   pace: cb/s C drew/s D | callback interval ms (clock: steady, callback starts) mean M p50 P p95 P p99 P max X | <0.75R A >1.5R B (R=refresh ms) |
//         vblanks per callback (GLX_OML_sync_control glXGetSyncValuesOML) 0:n 1:n 2:n 3+:n |
//         lockstep repeats C (runs N, longest L frames / M ms, P over 50 ms printed) | logic ms L vs bound B
// cb: frame callbacks; drew: callbacks that ran a pass. Sokol swaps at the end of every callback, a no-pass one included, so a callback is a swap and
// a blocking vsync swap shows in the gap to the next callback's start. The vblank counter is read once per callback on the sapp thread; its advance
// between two callbacks is how many refreshes passed: 0 = two presents inside one refresh (over the refresh rate), 1 = paced, 2+ = a missed refresh.
// The histogram is "n/a" when the extension is missing.
// The lockstep section (kenshi-k16.2.437.1) appears only while the logic thread runs: the callbacks without a pass that repeated the kept frame
// (logicthread::takeMisses: their count, the runs of them that ended, the longest run in frames and ms, and the runs over 50 ms that printed a
// line), and the world frame's logic ms (logicthread::lastLogicMs, the packet's) against the wait's bound (main.cpp lockstep_bound_ms, the
// refresh period or the cap's step): whether logic misses the bound at all, and by how much.
#include "dev/pacelog.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/logicthread.hpp"
#include "app/realfps.hpp"
#include "platform/swapinfo.hpp"
#include "platform/windowgate.hpp"

namespace pacelog
{
bool gOn = false;
namespace
{
using Clock = std::chrono::steady_clock;
Clock::time_point gLast{}, gWindow{};
std::vector<double> gDt;	// ms between callback starts this second
long gCb = 0, gDrew = 0, gUniq = 0;
Clock::time_point gLastUniq{};
std::vector<double> gUdt;	// ms between unique frames this second (kenshi-k16.2.437.3)
long long gMsc = -1;
long gHist[4] = {};	// vblank advance 0, 1, 2, 3+
bool gHaveMsc = false;
double gRefreshMs = 0;
// The lockstep section this second (noteLockstep): its calls say the logic thread runs.
long gLockCalls = 0, gLockRepeats = 0, gLockRuns = 0, gLockLongest = 0, gLockPrinted = 0, gLockLogic = 0;
double gLockLongestMs = 0, gLockLogicSum = 0, gLockBound = 0;
}

void init() { gOn = getenv("KCLIENT_PACELOG") != nullptr; }

static void report(Clock::time_point now)
{
	const double secs = std::chrono::duration<double>(now - gWindow).count();
	std::sort(gDt.begin(), gDt.end());
	auto pct = [&](double p) { return gDt.empty() ? 0.0 : gDt[std::min(gDt.size() - 1, (size_t)(p * gDt.size()))]; };
	double sum = 0;
	for (double v : gDt) { sum += v; }
	const double refresh = view::refreshPeriodLive() * 1000.0;
	if (refresh > 0) { gRefreshMs = refresh; }
	const double R = gRefreshMs > 0 ? gRefreshMs : 1000.0 / 60.0;
	long lo = 0, hi = 0;
	for (double v : gDt) { if (v < 0.75 * R) { lo++; } else if (v > 1.5 * R) { hi++; } }
	char vb[96];
	if (gHaveMsc) { snprintf(vb, sizeof vb, "0:%ld 1:%ld 2:%ld 3+:%ld", gHist[0], gHist[1], gHist[2], gHist[3]); } else { snprintf(vb, sizeof vb, "n/a"); }
	// kenshi-k16.2.437.1: only while the logic thread runs (noteLockstep was called); otherwise the line is as before.
	char lk[224] = "";
	if (gLockCalls)
	{
		snprintf(lk, sizeof lk, " | lockstep repeats %ld (runs %ld, longest %ld frames / %.0f ms, %ld over %ld ms printed) | logic ms %.2f vs bound %.2f",
			gLockRepeats, gLockRuns, gLockLongest, gLockLongestMs, gLockPrinted, (long)logicthread::kMissLogMs, gLockLogic ? gLockLogicSum / gLockLogic : 0.0,
			gLockBound);
	}
	std::sort(gUdt.begin(), gUdt.end());
	auto upct = [&](double p) { return gUdt.empty() ? 0.0 : gUdt[std::min(gUdt.size() - 1, (size_t)(p * gUdt.size()))]; };
	fprintf(stderr, "pace: swaps/s %.1f unique/s %.1f (present-to-present ms p50 %.2f p95 %.2f max %.2f; the HUD shows %.1f) | cb/s %.1f drew/s %.1f | callback interval ms (steady clock, callback starts) mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f | <0.75R %ld >1.5R %ld (R=%.2f ms%s) | vblanks per callback (glXGetSyncValuesOML) %s%s\n",
		gCb / secs, gUniq / secs, upct(0.5), upct(0.95), gUdt.empty() ? 0.0 : gUdt.back(), realfps::last().unique, gCb / secs, gDrew / secs, gDt.empty() ? 0.0 : sum / gDt.size(), pct(0.5), pct(0.95), pct(0.99), gDt.empty() ? 0.0 : gDt.back(), lo, hi, R,
		gRefreshMs > 0 ? "" : ", refresh unknown: 60 Hz assumed", vb, lk);
	gDt.clear();
	gCb = gDrew = gUniq = 0;
	gUdt.clear();
	for (long& h : gHist) { h = 0; }
	gLockCalls = gLockRepeats = gLockRuns = gLockLongest = gLockPrinted = gLockLogic = 0;
	gLockLongestMs = gLockLogicSum = 0;
	gWindow = now;
}

void beginSlow()
{
	const auto now = Clock::now();
	if (gWindow.time_since_epoch().count() == 0) { gWindow = now; }
	if (gLast.time_since_epoch().count() != 0) { gDt.push_back(std::chrono::duration<double, std::milli>(now - gLast).count()); }
	gLast = now;
	long long msc = 0;
	if (swapinfo::vblankCount(msc))
	{
		if (gMsc >= 0) { gHaveMsc = true; gHist[std::min<long long>(3, std::max<long long>(0, msc - gMsc))]++; }
		gMsc = msc;
	}
	if (now - gWindow >= std::chrono::seconds(1)) { report(now); }
}

void noteSlow(bool drew, bool unique)
{
	gCb++;
	if (drew) { gDrew++; }
	if (!unique) { return; }
	const auto now = Clock::now();
	gUniq++;
	if (gLastUniq.time_since_epoch().count() != 0) { gUdt.push_back(std::chrono::duration<double, std::milli>(now - gLastUniq).count()); }
	gLastUniq = now;
}

void noteLockstepSlow(const logicthread::MissStats& m, double logicMs, double boundMs)
{
	gLockCalls++;
	gLockRepeats += m.repeats;
	gLockRuns += m.runs;
	gLockPrinted += m.printed;
	if (m.longest > gLockLongest || (m.longest == gLockLongest && m.longestMs > gLockLongestMs)) { gLockLongest = m.longest; gLockLongestMs = m.longestMs; }
	if (logicMs > 0) { gLockLogicSum += logicMs; gLockLogic++; }	// logicthread::lastLogicMs: the world frame's logic half (0 outside it)
	if (boundMs > 0) { gLockBound = boundMs; }
}

}
