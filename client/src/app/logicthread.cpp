// The logic thread in lockstep and gated overlap (logicthread.hpp; docs/RENDER_THREAD.md §7.1 rules 1, 3, 4, 6-9, 12; kenshi-k16.2.205.12.25,
// kenshi-k16.2.205.12.96).
#include "app/logicthread.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <pthread.h>
#include <thread>

#include "app/config.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "app/threadsplit.hpp"
#endif

namespace logicthread
{
namespace
{
const cfg::Text kThread{ cfg::Group{ "Performance" }, "render.thread", "off", { "off", "lockstep", "overlap" },
	"The render thread (docs/RENDER_THREAD.md 7.1): off = one thread; lockstep = a logic thread that alternates with the render thread; overlap = "
	"the logic thread's next frame starts while the render thread draws (gated: its second half waits for the draw's end; scripted runs stay "
	"lockstep unless debug.overlap_scripted). Read at startup" };
const cfg::Bool kOverlapScripted{ cfg::Group{ "Scripted runs" }, "debug.overlap_scripted", false,
	"Scripted runs with render.thread=overlap really overlap (a ThreadSanitizer run of the overlap, overlap review .85 A3), with rule 6's skip; "
	"frame-exact shots are not expected. Off: scripted runs and the scene test run lockstep (docs/RENDER_THREAD.md 7.1 rule 9)" };

// Logic's wait for its packet's draw (lockstep) or take (overlap), rule 6's bound: on each expiry lockstep polls the link and waits again (it
// never skips, rule 9); overlap takes the packet back and runs its next frame (the skip). The gate's wait (renderIdle) checks the stop flag at it.
constexpr auto kLogicBound = std::chrono::milliseconds(100);

enum class Slot { kFree, kFilled, kDrawing };

Mode gMode = Mode::kOff;
Hooks gHooks;
std::thread gThread;
bool gRunning = false;	// render thread only
std::atomic<bool> gStop{ false };

// Guarded by gMutex: the slot states and the counters of the hand-off (rule 1). Never the slots' contents.
std::mutex gMutex;
std::condition_variable gCv;
Slot gSlot[2] = { Slot::kFree, Slot::kFree };
int gFilled = -1, gDrawing = -1;	// the filled slot (at most one), the drawing slot (at most one)
unsigned long gFilledCount = 0, gDrawnCount = 0;	// packets logic marked filled, packets the render thread drew
bool gInDraw = false;	// the render thread is inside hooks.render (set at the flip, cleared after the draw): what renderIdle waits on
bool gExited = false;	// the logic thread left its loop

// Render thread only: the waits that ran out since the last draw.
int gMissed = 0;
std::chrono::steady_clock::time_point gMissStart;
// Render thread only, since the last takeMisses() (kenshi-k16.2.437.1): the frames without a pass, the runs that ended, the longest run.
long gRepeats = 0, gRuns = 0, gLongest = 0, gPrinted = 0;
double gLongestMs = 0, gLastBound = 0;

std::atomic<double> gStallMs{ 0 };	// the scripted render stall, taken once
std::atomic<double> gLogicMs{ 0 };	// the newest world frame's logic ms (kenshi-k16.2.437.1): logic writes it, the pace log reads it

// Logic thread only.
IdleStats gIdle;
thread_local bool tLogic = false;	// this is the logic thread (renderIdle is a no-op on any other)

// Overlap's take-wait (rule 6): until the render thread has taken `slot` (filled -> drawing). The bound runs out with the slot still filled:
// filled -> free (the skip; the packet is never drawn, rule 2 makes that harmless) and false, so logic refills the same slot.
bool waitTaken(std::unique_lock<std::mutex>& lk, int slot)
{
	const auto w0 = std::chrono::steady_clock::now();
	bool taken = true, waited = false;
	while (!gStop.load() && gSlot[slot] == Slot::kFilled)
	{
		if (gCv.wait_for(lk, kLogicBound, [&] { return gStop.load() || gSlot[slot] != Slot::kFilled; })) { break; }
		waited = true;
		gIdle.expiries++;
		if (gSlot[slot] == Slot::kFilled)	// the render thread did not take it meanwhile (it would have flipped under this mutex)
		{
			gSlot[slot] = Slot::kFree;
			gFilled = -1;
			gIdle.skips++;
			taken = false;
		}
	}
	if (waited)
	{
		gIdle.waits++;
		gIdle.waitedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
	}
	return taken;
}

void logicLoop()
{
#if defined(__APPLE__)
	pthread_setname_np("kclient-logic");	// macOS names only the calling thread
#else
	pthread_setname_np(pthread_self(), "kclient-logic");	// gdb, top -H, the crash report
#endif
	tLogic = true;
	int slot = 0;	// slot 0 was the one-thread frames' slot; logic's first packet reuses it (nothing draws it any more until it is filled)
	while (!gStop.load())
	{
		gHooks.logic(slot);	// the slot is free: logic writes it without the mutex (rule 1)
		std::unique_lock<std::mutex> lk(gMutex);
		gSlot[slot] = Slot::kFilled;
		gFilled = slot;
		const unsigned long mine = ++gFilledCount;
		gCv.notify_all();
		if (gMode == Mode::kOverlap)
		{
			// Overlap (rule 12): wait only for the take, then run the next frame's first half while this packet is drawn. A skipped packet's
			// slot is free again: the next frame refills it. The slot drawn before this one went free at this one's flip.
			if (waitTaken(lk, slot)) { slot ^= 1; }
			continue;
		}
		// Lockstep (rule 9): wait until the render thread has drawn this packet (and finished its readbacks); no skip.
		const auto w0 = std::chrono::steady_clock::now();
		bool waited = false;
		while (!gStop.load() && gDrawnCount < mine)
		{
			if (gCv.wait_for(lk, kLogicBound, [&] { return gStop.load() || gDrawnCount >= mine; })) { break; }
			waited = true;
			gIdle.expiries++;
			// Ran out. While the render thread has not taken the slot it reads nothing of app, so the link can be polled under the mutex
			// (the flip waits for it). Once the slot is drawing, render_frame may be reading the sim: wait again without polling.
			if (gSlot[slot] == Slot::kFilled && gHooks.idle) { gHooks.idle(); gIdle.polls++; }
		}
		if (waited)
		{
			gIdle.waits++;
			gIdle.waitedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
		}
		lk.unlock();
		slot ^= 1;	// the slot drawn before this one went free at this one's flip
	}
	std::lock_guard<std::mutex> lk(gMutex);
	gExited = true;
	gCv.notify_all();
}
}

Mode mode() { return gMode; }

void configure()
{
	const std::string v = kThread.get();
	gMode = v == "overlap" ? Mode::kOverlap : v == "lockstep" ? Mode::kLockstep : Mode::kOff;
	if (gMode != Mode::kOff) { fprintf(stderr, "kclient: render.thread=%s: the logic thread starts when loading ends\n", v.c_str()); }
}

void start(Hooks hooks, bool scripted)
{
	if (gRunning || gMode == Mode::kOff) { return; }
	if (gMode == Mode::kOverlap && scripted && !kOverlapScripted.get())	// rule 9: frame counts, kScriptDt steps and shots stay per logic frame
	{
		fprintf(stderr, "kclient: render.thread=overlap: a scripted run runs lockstep (docs/RENDER_THREAD.md 7.1 rule 9; debug.overlap_scripted=1 overlaps it)\n");
		gMode = Mode::kLockstep;
	}
	gHooks = std::move(hooks);
	gStop.store(false);
	{
		std::lock_guard<std::mutex> lk(gMutex);
		gSlot[0] = gSlot[1] = Slot::kFree;
		gFilled = gDrawing = -1;
		gExited = false;
		gInDraw = false;
	}
	gRunning = true;
	gThread = std::thread(logicLoop);
	fprintf(stderr, "kclient: logic thread started (%s)\n", gMode == Mode::kOverlap ? "overlap" : "lockstep");
}

bool running() { return gRunning; }
bool stopping() { return gStop.load(); }

bool renderFrame(double boundMs)
{
	std::unique_lock<std::mutex> lk(gMutex);
	const auto bound = std::chrono::duration<double, std::milli>(boundMs);
	if (!gCv.wait_for(lk, bound, [] { return gFilled >= 0 || gExited; }) || gFilled < 0)
	{
		if (gMissed++ == 0) { gMissStart = std::chrono::steady_clock::now(); }
		gLastBound = boundMs;
		gRepeats++;
		return false;
	}
	// The flip (rule 3): filled -> drawing, the slot drawn before -> free, then logic is told (it waits for the draw itself in lockstep).
	const int s = gFilled;
	gFilled = -1;
	if (gDrawing >= 0) { gSlot[gDrawing] = Slot::kFree; }
	gSlot[s] = Slot::kDrawing;
	gDrawing = s;
	gInDraw = true;
	gCv.notify_all();	// overlap: logic's take-wait ends here
	lk.unlock();
	if (gMissed)
	{
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gMissStart).count() + boundMs;
		gRuns++;
		if (gMissed > gLongest) { gLongest = gMissed; gLongestMs = ms; }	// the run with the most frames; ms is its wall time (today's number)
		// Only a real stall prints (kenshi-k16.2.437.1): a run of misses over kMissLogMs, once, when it ends. A one-frame repeat is the normal
		// jitter of a logic frame that just misses the bound; the count of those goes to the pace log (dev/pacelog.hpp).
		if (ms > kMissLogMs)
		{
			fprintf(stderr, "kclient: lockstep: no packet for %.0f ms: %d frames without a pass showed the last frame\n", ms, gMissed);
			gPrinted++;
		}
		gMissed = 0;
	}
	gHooks.render(s);
	lk.lock();
	gDrawnCount++;
	gInDraw = false;
	gCv.notify_all();
	return true;
}

void renderIdle()
{
	if (!tLogic || gMode != Mode::kOverlap) { return; }	// gMode is written before the thread is made, never after
	std::unique_lock<std::mutex> lk(gMutex);
	if (!gInDraw && gFilled < 0) { return; }
	const auto t0 = std::chrono::steady_clock::now();
	// A filled slot here (not on the frame path: logic fills only at its frame's end) is waited out too: taken, drawn, then idle.
	while (!gStop.load() && (gInDraw || gFilled >= 0)) { gCv.wait_for(lk, kLogicBound, [] { return gStop.load() || (!gInDraw && gFilled < 0); }); }
	lk.unlock();
#ifdef KCLIENT_DEV_TOOLS
	threadsplit::gateWait(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
#else
	(void)t0;
#endif
}

double stop()
{
	if (!gRunning) { return 0; }
	const auto t0 = std::chrono::steady_clock::now();
	gStop.store(true);	// rule 8: the flag, the notify, the join; the caller destroys only after this returns
	{
		std::lock_guard<std::mutex> lk(gMutex);
		gCv.notify_all();
	}
	gThread.join();
	gRunning = false;
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void stallRender(double ms) { gStallMs.store(ms); }

void noteLogicMs(double ms) { gLogicMs.store(ms); }

double lastLogicMs() { return gLogicMs.load(); }

void renderStallPoint()
{
	const double ms = gStallMs.exchange(0);
	if (ms > 0) { std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms)); }
}

IdleStats takeIdle()
{
	std::lock_guard<std::mutex> lk(gMutex);	// logic writes gIdle under the mutex (its wait loop)
	IdleStats s = gIdle;
	gIdle = {};
	return s;
}

MissStats takeMisses()
{
	MissStats s{ gRepeats, gRuns, gLongest, gPrinted, gLongestMs };	// render thread only: renderFrame writes these, the caller is that thread
	if (gMissed > 0)	// a run still going: its length and the time it has lasted so far (it may have begun in an earlier second)
	{
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gMissStart).count() + gLastBound;
		if (gMissed > s.longest) { s.longest = gMissed; s.longestMs = ms; }
		else if (gMissed == s.longest && ms > s.longestMs) { s.longestMs = ms; }
	}
	gRepeats = gRuns = gLongest = gPrinted = 0;
	gLongestMs = 0;
	return s;
}
}
