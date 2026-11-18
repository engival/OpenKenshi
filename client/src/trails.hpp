#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "sokol_gfx.h"	// sg_pixel_format / sgl_context in the interface below; sokol_gl.h needs it first
#include "sokol_gl.h"

class Characters;
namespace ovl { class List; }
class SimView;

// Debug trails (kenshi-k16.2.73; once whole-fight, a rolling 30 s since k16.19.6.1; user 2026-11-10: "Foot tracking lines (kind of like we did for the animation viewer), overlayed on the
// ground for the ENTIRE fight. This should show weird teleporting lines. (maybe plot the character 'center point' too) ... 'show all', 'show
// specific unit', 'show rolling X seconds'"). Every drawn character's two foot contact points, its sim centre and its drawn centre, recorded once a
// frame in world space and drawn through the world camera as lines over the scene. The paths float in full 3D (the user, 2026-11-10: the "overlayed
// on the ground" description was loose): the feet at their own heights, the two centres at the body's mid height.
//
// The sim centre and the drawn centre are separate tracks on purpose (kenshi-k16.2.74): the display slop (anim.slop_tau) may sit the drawn position
// slightly off the sim, and the two lines then show the offset, so the slop can never hide a sim bug. A frame move larger than 2.5x the faster of
// this track's own smoothed speed and the sim's, x dt, plus a floor, is a pop (a teleport or a snap) and draws in the warning colour, thicker.
// A foot's stride swing is its own speed and so never trips it, and the feet's floor is 15 u, just above the ~12 u a clip switch snaps a foot
// (k16.2.73 review: feet flagged 181 / 248 times in one fight on the sim's speed); the centres' floor is 4 u.
//
// Memory is bounded: per character four tracks, a point kept when the track moved over 0.5 u (at most every 0.05 s) or 0.25 s passed, capped at
// 640 points a track (the oldest dropped), at most 64 characters (the least recently drawn evicted). A player character keeps 60 s, or since its
// newest move order when longer, capped at 120 s (2400 points; kenshi-k16.2.231.1: the shot carries the whole walk out of a building).
//
// The draw is bounded too (kenshi-k16.19.6.1): only the newest `debug.trails_seconds` (default 30 s) of a track are ever drawn, and only up to the
// decimation's top rate a track within that window; the lines go into the trails' own sgl context, sized for the whole cap, so a full history can never
// fill the default context's vertex buffer and silently drop the HUD, minimap and windows with it (the k16.19.6 bug). `debug.trails_dump` (off by default,
// needs --debug) writes the points that drop out of memory to segment files under the client's data home, so a long session's whole history survives.
class Trails
{
public:
	static constexpr int kTracks = 4;	// 0 L foot, 1 R foot, 2 sim centre, 3 drawn centre
	// A track in memory (k16.19.6.1, retuned from 2048): the widest window the draw can show is kCapMax x kTopRate = 600 points, so 640 keeps the whole
	// drawn window plus a margin at ~5 MB for the 64-character cap; a long session's older points are the dump's (debug.trails_dump), one segment file each.
	static constexpr size_t kMaxPoints = 640;
	// A player character keeps more (kenshi-k16.2.231.1, the stuck-at-a-door shot): 60 s, or the whole time since its last move order when that is
	// longer, capped at 120 s — 2400 points a track at the 20 Hz top rate. Worst case 64 x 4 x 2400 points (~20 MB); a real squad is a few characters.
	static constexpr size_t kMaxPointsLong = 2400;
	static constexpr double kLongWindow = 60.0, kLongMax = 120.0;	// seconds: the longer trail's floor and cap
	static constexpr size_t kMaxChars = 64;
	static constexpr double kTopRate = 20.0;	// the decimation's top rate: 1 / kKeepMinDt points a second a track
	static constexpr int kVerticesPerPoint = 6;	// a pop draws three 2-vertex lines; a normal point one
	static constexpr double kCapMax = 30.0;	// debug.trails_seconds' top, the user's "~30 seconds" (the sgl context is sized once, at startup, from the then-current value)

	bool on() const;	// debug.trails: --debug at the call sites gates it too
	void toggle();
	// After Characters::update: one frame's points for every drawn character. dt: sim seconds (0: paused, nothing moved).
	void record(const SimView& sim, const Characters& chars, double dt);
	// The trails' own sgl context (k16.19.6.1): create it after sgl_setup with the overlay pass's formats. submit() plays draw()'s list into it; the
	// host draws it with sgl_context_draw_layer(context(), 0) inside the overlay pass, before the default context's layer 0.
	void initSgl(sg_pixel_format colour, sg_pixel_format depth, int samples);
	sgl_context context() const { return ctx_; }
	int lastVertices() const { return lastVertices_; }	// the last draw's vertex count
	int budget() const { return maxVertices_; }	// the context's, the most draw() can ever record
	// Logic side (render sitting 11, kenshi-k16.2.205.12.57): world points through mvp with the camera at cam, in pixels like navoverlay.cpp /
	// animlab.cpp, recorded through ovl:: into the list record_overlay binds (the packet's trails list).
	void draw(const float mvp[16], const double cam[3], float width, float height);
	// Render side, in the overlay pass (begin_overlay): the recorded list into the trails' context, set up for width x height pixels. No context: nothing.
	void submit(const ovl::List& list, float width, float height);
	// `trails ...`: all | id=N | name NAME... | window SECONDS (0 = the cap) | clear | off. False: the arguments are wrong (reply says why).
	bool op(const std::vector<std::string>& a, std::string& reply);
	std::string status() const;	// one line: on/off, the filter, the window, characters and points
	// The trails panel's view (kenshi-k16.3.11, ui/dev/trailspanel.hpp): every recorded character with its kept points (all four tracks) and its
	// warning points, the character's track hue for the swatch, the current pick (0: all) and the window.
	struct Row
	{
		uint64_t id = 0;
		std::string name;
		uint32_t points = 0, pops = 0;
	};
	std::vector<Row> rows() const;
	std::string colour(uint64_t id) const;	// "#rrggbb": the hue of draw()'s track (a name pick's character: its own colour)
	uint64_t picked() const;	// the pick: the one character's id, 0 for all (a name pick: its character's id while it is recorded)
	double window() const { return window_; }	// the view filter, seconds (0: the cap, debug.trails_seconds)
	static double cap();	// the draw cap, seconds: debug.trails_seconds clamped to 1..kCapMax (the panel's LAST <cap> S, kenshi-k16.2.106)
	// <shot>.trails.tsv: every kept point of the whole fight (not the view window), one row a point; false if the file cannot be written.
	bool writeFile(const std::string& path) const;
	// debug.trails_dump (k16.19.6.1): the directory the dropped points are flushed into, one segment file per kSegmentPoints points. Empty: off.
	void setDumpDir(const std::string& dir) { dumpDir_ = dir; }
	static constexpr size_t kSegmentPoints = 250000;	// a segment file holds this many dropped points (~22 MB at ~90 bytes a line)
	void shutdown();	// closes the open segment (the sgl context belongs to sgl_shutdown)
	~Trails();	// closes the open segment too (a static teardown without the explicit shutdown)

private:
	struct Pt
	{
		float p[3] = {};
		double t = 0;	// sim seconds since the last clear or trails on
		double tick = 0;	// the render tick it was recorded at (SimView::tick)
		bool warn = false;	// arrived by a frame move over the pop rule: the segment into it draws in the warning colour
	};
	struct Track
	{
		std::deque<Pt> pts;
		float prev[3] = {};	// last frame's point (the frame-move test; not the last kept point)
		bool has = false;
		double lastT = 0;
		float rate = 0;	// this track's own smoothed speed (u/s, ~0.25 s): the pop rule's yardstick, so a foot's stride swing is not a pop
	};
	struct Set
	{
		uint64_t id = 0;
		std::string name;
		Track tr[kTracks];
		double prevSim[3] = {}, prevDrawn[3] = {};
		double speed = 0;	// the sim's speed last frame (u/s): the pop rule's yardstick
		bool seen = false;	// drawn in a frame before this one
		double lastT = 0;	// the last frame recorded (eviction)
		bool player = false;	// a player character: the longer trail (kMaxPointsLong / the window in record())
		uint64_t lastMoveTick = 0;	// the sim tick of its newest move_to order (SimView::lastMoveTick); 0 none
	};

	std::vector<Set> sets_;
	double t_ = 0;	// sim seconds
	double window_ = 0;	// s; 0 = the cap. A view filter (draw/status), never above debug.trails_seconds: the points stay kept, but the draw only ever shows the newest cap
	enum class Pick { All, One, Name } pick_ = Pick::All;
	uint64_t pickId_ = 0;
	std::string pickName_;

	void dumpWrite(const Set& s, int track, const Pt& p);	// one dropped point to the open segment (opens it, rotates it)
	void dumpAll(const Set& s);	// every point of a set leaving memory (eviction, clear)
	void dumpClose();

	sgl_context ctx_ = {};	// the trails' own context (initSgl); id SG_INVALID_ID until then
	sgl_pipeline pip_ = {};	// alpha blending, no depth test: the overlay's look
	int maxVertices_ = 0, lastVertices_ = 0;
	size_t lastCalls_ = 0, lastListBytes_ = 0;	// the last submitted overlay list (calls, bytes)
	std::string dumpDir_;	// debug.trails_dump's directory; empty: off
	FILE* dump_ = nullptr;
	std::string dumpStamp_;	// the run's segment prefix, fixed at the first dropped point (date-time-pid)
	int dumpIndex_ = 0;
	size_t dumpPoints_ = 0;
};
