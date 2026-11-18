// The window-state gate (windowgate.hpp, kenshi-k16.2.222). Polls the window once a frame: the first change applies at once, later
// changes within the 1 s cooldown are held, and the latest held state applies once when it ends (which starts a new cooldown).
#include "windowgate.hpp"

#include <chrono>
#include <cstdio>
#include <mutex>

#include "sokol_app.h"
#include "windowgeom.hpp"

namespace windowgate
{
	namespace
	{
		constexpr double kCooldown = 1.0;	// s: how long after an apply further changes are held (one per second at most)

		bool gInit = false;
		int gW = 0, gH = 0;		// the applied state: what the frame renders at
		int gHeldW = 0, gHeldH = 0;	// the latest change held during the cooldown
		bool gHeld = false;
		bool gHoldNoted = false;	// the hold line is printed once per cooldown, not once per event (a storm can be thousands)
		long gHeldEvents = 0, gApplyCount = 0;
		bool gAppliedFrame = false;
		std::chrono::steady_clock::time_point gCooldownEnd;

		// The published state (kenshi-k16.2.205.12.23): frame() and flush() run on the sapp (render) thread and publish; logic copies it once at its
		// frame start (take) and every accessor reads that copy. Before the first take the accessors read the gate's own state.
		struct State { bool init = false; int w = 0, h = 0; bool applied = false, held = false; long applies = 0; };
		std::mutex gMx;
		State gPub, gLogic;
		bool gTaken = false;
		State own() { return { gInit, gW, gH, gAppliedFrame, gHeld, gApplyCount }; }
		void publish() { std::lock_guard<std::mutex> l(gMx); gPub = own(); }
		State cur() { return gTaken ? gLogic : own(); }

		void apply(int w, int h, const char* why)
		{
			const bool changed = w != gW || h != gH;
			if (!changed) { gHeld = false; gHoldNoted = false; return; }
			gW = w;
			gH = h;
			gHeld = false;
			gHoldNoted = false;
			gCooldownEnd = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kCooldown));
			gAppliedFrame = true;
			gApplyCount++;
			printf("windowgate: apply %dx%d (%s; %ld held event%s since the last apply)\n", w, h, why, gHeldEvents, gHeldEvents == 1 ? "" : "s");
			gHeldEvents = 0;
			windowgeom::applied(w, h);	// windowgeom.log shows the state the client applied, not each event
		}
	}

	void init(int w, int h)
	{
		if (gInit) { return; }
		gInit = true;
		gW = gHeldW = w;
		gH = gHeldH = h;
		gCooldownEnd = std::chrono::steady_clock::now();
		printf("windowgate: start %dx%d, further changes held for %.1f s after an apply\n", w, h, kCooldown);
	}

	static void poll()
	{
		const int w = sapp_width(), h = sapp_height();
		gAppliedFrame = false;
		if (w < 1 || h < 1) { return; }	// iconified: the applied state stays what it was
		if (!gInit) { init(w, h); return; }
		const auto now = std::chrono::steady_clock::now();
		if (w != gW || h != gH)
		{
			if (windowgeom::restoring()) { apply(w, h, "startup restore"); return; }	// the remembered window still arriving: apply at once
			if (!gHeld && now >= gCooldownEnd) { apply(w, h, "first change, applied at once"); return; }
			if (!gHeld) { gHeld = true; gHeldEvents = 1; }
			else if (w != gHeldW || h != gHeldH) { gHeldEvents++; }
			gHeldW = w;
			gHeldH = h;
			if (!gHoldNoted)
			{
				gHoldNoted = true;
				printf("windowgate: holding %dx%d: another change within the cooldown only replaces the held state\n", w, h);
			}
		}
		if (gHeld && now >= gCooldownEnd)
		{
			if (w == gW && h == gH) { gHeld = false; gHoldNoted = false; printf("windowgate: the window came back to the applied %dx%d; the held change is dropped\n", gW, gH); }
			else { apply(gHeldW, gHeldH, "the latest held change, at the end of the cooldown"); }
		}
	}

	void frame()
	{
		poll();
		publish();
	}

	void flush()
	{
		if (!gInit || !gHeld) { return; }
		apply(gHeldW, gHeldH, "flush at exit, so the saved state is the applied one");
		publish();
	}

	void take()
	{
		std::lock_guard<std::mutex> l(gMx);
		gLogic = gPub;
		gTaken = true;
	}

	int width() { const State s = cur(); return s.init ? s.w : sapp_width(); }
	int height() { const State s = cur(); return s.init ? s.h : sapp_height(); }
	float widthf() { return (float)width(); }
	float heightf() { return (float)height(); }
	float aspect() { return height() > 0 ? (float)width() / (float)height() : 1.0f; }
	bool appliedThisFrame() { return cur().applied; }
	bool holding() { return cur().held; }
	long applies() { return cur().applies; }
}

namespace view
{
	namespace
	{
		struct Snap { int w = 0, h = 0; double dt = 0, refresh = 0; };	// refresh: the display's period in s, 0 unknown
		std::mutex gMx;
		Snap gPub, gLogic;	// gPub: the sapp thread's snapshot; gLogic: logic's copy of it (take)
		bool gPublished = false, gTaken = false;
	}
	void snapshot()
	{
		const double hz = windowgeom::refreshHz();
		const Snap s{ sapp_width(), sapp_height(), sapp_frame_duration(), hz > 0 ? 1.0 / hz : 0.0 };
		std::lock_guard<std::mutex> l(gMx);
		gPub = s;
		gPublished = true;
	}
	void take()
	{
		std::lock_guard<std::mutex> l(gMx);
		if (!gPublished) { return; }
		gLogic = gPub;
		gTaken = true;
	}
	thread_local int tRenderW = 0, tRenderH = 0;	// the render thread's scope (RenderSize): the packet's applied size, 0 outside one
	RenderSize::RenderSize(int w, int h) { tRenderW = w; tRenderH = h; }
	RenderSize::~RenderSize() { tRenderW = tRenderH = 0; }
	int width() { return tRenderW > 0 ? tRenderW : gTaken ? gLogic.w : sapp_width(); }
	int height() { return tRenderH > 0 ? tRenderH : gTaken ? gLogic.h : sapp_height(); }
	float widthf() { return (float)width(); }
	float heightf() { return (float)height(); }
	double frameDuration() { return gTaken ? gLogic.dt : sapp_frame_duration(); }
	double frameDurationLive()
	{
		std::lock_guard<std::mutex> l(gMx);
		return gPublished ? gPub.dt : sapp_frame_duration();
	}
	double refreshPeriodLive()
	{
		std::lock_guard<std::mutex> l(gMx);
		return gPublished ? gPub.refresh : 0.0;
	}
}
