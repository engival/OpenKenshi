#pragma once

// The logic thread and its hand-off with the render thread (docs/RENDER_THREAD.md §7.1, kenshi-k16.2.205.12.25). `render.thread` picks the mode,
// read once at startup:
//   off       one thread: frame() runs logic_frame and render_frame itself (as before; nothing here runs).
//   lockstep  a real logic thread, started once loading first ends (rule 7). The two threads strictly alternate (rule 9): logic fills a slot and
//             waits until the render thread has drawn it; the render thread draws only while logic waits. So render_frame's `app.` reads stay
//             legal and there is no data race by construction.
//   overlap   gated overlap (overlap review .85 section 1.1 stage 1, kenshi-k16.2.205.12.96): logic waits only until the render thread has
//             TAKEN its packet, runs the first half of its next frame while that packet is drawn, and calls renderIdle() before its second
//             half (and before any rare write to what the draw reads), which waits until the draw has ended (rule 12). When the take-wait's
//             bound runs out, logic takes its untaken packet back and runs the next frame (rule 6's skip). Scripted runs and the scene test
//             run lockstep unless debug.overlap_scripted (review A3).
// The packet slots themselves are the caller's (gPackets); this module owns their states (free / filled / drawing, rule 1), the one mutex and
// condition variable, and the thread.
#include <functional>

namespace logicthread
{
enum class Mode { kOff, kLockstep, kOverlap };

struct Hooks
{
	std::function<void(int slot)> logic;	// logic thread: fill slot `slot` (logic_frame); the slot is free, nobody else reads it
	std::function<void(int slot)> render;	// render thread: draw slot `slot` (render_frame, the window commands); logic is waiting meanwhile
	// Logic thread, its wait for the draw timed out while the render thread had not taken the slot yet (rule 9): keep the sim link moving
	// without stepping the world. Runs under the hand-off mutex, so the render thread cannot flip (and start reading app.) while it runs.
	std::function<void()> idle;
};

Mode mode();	// render.thread as read by configure() (kOff before); from start() on, the mode the thread runs (a scripted overlap: kLockstep)
void configure();	// reads render.thread once; call after the config is loaded
// Starts the logic thread (rule 7): call once, on the render thread, at the end of the first frame that is not loading. Not in kOff. scripted:
// a script, a screenshot or the scene test drives the run; overlap then runs lockstep (rule 9) unless debug.overlap_scripted is set.
void start(Hooks hooks, bool scripted);
// Logic thread, overlap only (rule 12): returns once the render thread is outside render_frame and no packet is filled, so what follows may write
// anything the draw reads until logic marks its next packet filled. Called before the world frame's second half (part B), at the top of a frame
// that is not a plain world frame (session load, title, creator, lab), and before rare writes (sim_opened, exec_cmd, the buildings' set). Its
// wait checks the stop flag every 100 ms and never gives up otherwise; the time waited goes to lt.gate (app/threadsplit.hpp). Free (returns at
// once) in off and lockstep and on any other thread.
void renderIdle();
// Logic thread: the session load screen is up (the load gate, kenshi-k16.2.437.4.3): a stall that touches it is expected and prints nothing.
void setLoadScreen(bool up);
bool running();	// start() ran and stop() has not
// Render thread, once per frame() after start(): waits up to boundMs for a filled slot. Found: flips it to drawing (the slot drawn before goes
// free), runs hooks.render, and lets logic go on (true). Not found: false, nothing drawn (the caller keeps the window's last frame). A wait that
// ran out is counted; a run of them over kMissLogMs prints one line when it ends (kenshi-k16.2.437.1), shorter runs stay in takeMisses().
bool renderFrame(double boundMs);
// A run of missed packets longer than this prints; at 144 Hz one frame is 6.9 ms, so a shorter run is one or two repeated frames (the user's
// 2026-11-20 spam, kenshi-k16.2.437.1).
constexpr double kMissLogMs = 50.0;
// Render thread only, the frames without a pass since the last call (kenshi-k16.2.437.1; the pace log reads it, dev/pacelog.hpp): repeats =
// those frames, runs = runs of them that ended, longest = the longest run's frames, longestMs its wall time (today's printed ms), printed = the
// runs that were over kMissLogMs (each printed one line). A frame without a pass redraws the kept frame (frame/lastframe.hpp), which the pace
// log counts beside the callback interval.
struct MissStats { long repeats = 0, runs = 0, longest = 0, printed = 0; double longestMs = 0; };
MissStats takeMisses();
bool lastWasRedraw();	// render thread only: the last renderFrame() redrew the drawing packet (overlap, rule 4) instead of taking a new one
// Logic thread, at the end of a world frame's logic half (kenshi-k16.2.437.1): the packet's logic ms (FramePacket::logicMs), published atomically
// so the pace log can put it against the render thread's bound; the render thread may read it while logic writes the next one.
void noteLogicMs(double ms);
double lastLogicMs();	// any thread: the newest published value (0 before the first world frame)
// Render thread, from cleanup() (rule 8): stop flag, notify, join. Logic finishes the frame it is in (a long load step included) and exits.
// Returns the join's wall time in ms (0 when no thread runs).
double stop();
bool stopping();	// the stop flag (logic's long loops may check it)

// The scripted stalls (dev ops `stall`, `renderstall`): the render thread sleeps ms at its next frame() before it looks for a packet, as a
// window held in its swap would; logic's waits then run out and its idle hook polls the link.
void stallRender(double ms);
void renderStallPoint();	// render thread, frame() start: the pending scripted sleep, once
struct IdleStats { long waits = 0, expiries = 0, polls = 0, skips = 0; double waitedMs = 0; };
// Logic thread: the waits that ran out since the last call (their count, the bound's expiries, the idle polls, the time waited; overlap: the
// packets taken back untaken, rule 6's skip).
IdleStats takeIdle();
}
