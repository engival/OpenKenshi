#include "bridge/simview.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "app/config.hpp"
#include "app/cpustage.hpp"
#include "app/eventlog.hpp"
#include "kworld/loadorder.hpp"
#include "kutil/platform/socket.hpp"

using namespace simlink;

// kenshi-k16.2.237.10 (user 2026-11-18, A/B): drive.facing = travel turns the drawn body of the driven character to its direction of travel and plays the
// forward walk / run; the movement, the sim heading and the wire are as in camera. Display only (the presentation layer may render off the server's heading).
static const cfg::Text kDriveFacing{ cfg::Group{ "Direct drive" }, "drive.facing", "camera", { "camera", "travel" }, "Direct drive: camera = the body faces the camera's heading (strafe and backpedal clips); travel = the body turns to its direction of travel and walks forward, strafe / backward / mouse are camera-relative (display only, movement unchanged)" };
// kenshi-k16.1.3 (run flicker): the legs' speed blends from the previous sample segment's velocity into the current one's, so it is continuous
// rather than a step a tick; then the original's own filter (below) runs on it.
static const cfg::Bool kVelBlend{ cfg::Group{ "Locomotion" }, "anim.vel_blend", true, "Locomotion: the legs' speed blends between consecutive sample segments, continuous instead of stepping once a tick (0 = the current pair's velocity alone)" };
static const cfg::Float kSimJump{ cfg::Group{ "Logging" }, "debug.simjump", 2.0, 0.5, 20.0, "Events log: a wire segment faster than this many times the run speed is logged as a sim jump (evlog simjump)" };
static const cfg::Bool kSpeedFilter{ cfg::Group{ "Locomotion" }, "anim.speed_filter", true, "Locomotion: the legs' fwd / lat speed is the original's trimmed mean of the last 8 frames (0 = the raw speed)" };
// kenshi-k16.2.74 (display slop, user 2026-11-10: "The client should 'ease into correction'"): the drawn position may sit slightly off the sim's
// interpolated position while gameplay (picking, hit tests) reads the sim; the offset eases to 0 with this time constant, speed-capped so the legs
// walk the correction. 0 = the old capped catch-up chase (the before/after flag).
static const cfg::Bool kCharDefer{ cfg::Group{ "Character loading" }, "sim.char_defer", true, "New characters are built a few per frame, after their textures and race body decoded on worker threads; off: all at once in the frame they arrive (the old join stall; kenshi-k16.2.205.10)" };
static const cfg::Bool kCharWarm{ cfg::Group{ "Character loading" }, "sim.char_warm", true, "A race new to the scene is built ahead of its first character: its body, that character's gear meshes and a race-only build (morph, foot contacts), one step a frame in the add budget's leftover, so its add does not pay them all in one frame (kenshi-k16.2.205.12.77)" };
static const cfg::Float kWarmSlackMs{ cfg::Group{ "Character loading" }, "sim.char_warm_slack_ms", 0.0, 0.0, 100.0, "The race warm-up (sim.char_warm) also runs in a frame whose adds took up to this many ms past sim.char_add_budget_ms (0: only in the budget's leftover; kenshi-k16.2.205.12.77)" };
static const cfg::Float kAddBudgetMs{ cfg::Group{ "Character loading" }, "sim.char_add_budget_ms", 4.0, 0.5, 1000.0, "New characters (race, meshes, textures load on the main thread) are built per frame until this many ms are spent; the rest wait for the next frame (kenshi-k16.2.205.10)" };
static const cfg::Bool kBodyNudge{ cfg::Group{ "Locomotion" }, "anim.body_nudge", true, "Display depenetration (kenshi-k16.2.345): a drawn standing body is nudged (cap 0.45 u, eased) out of the client collision and other bodies; never fed to the sim, picks or orders. Off = the drawn body stands at the sim's x, z" };
static int64_t nudgeKey(int cx, int cz) { return ((int64_t)cx << 32) ^ (int64_t)(uint32_t)cz; }
// kenshi-k16.2.395.1 (k16.2.395; user 2026-11-19: "Walking up the stairs is a jumpy jittery mess, because every new stair rung makes the character
// teleport to a new height. It needs to interpolate between current height and new requested height."): the DRAWN body's y eases toward the stand
// height the ray reports (kenshi-k16.2.382.2) with this time constant, render side only. 0 = off (the old per-tread step, the A/B's before).
static const cfg::Float kStandEaseTau{ cfg::Group{ "Locomotion" }, "anim.stand_ease_tau", 0.03, 0.0, 1.0, "Locomotion: the drawn body's y eases toward the stand height (the drawn floor / stair / ramp ray) with this time constant (s), so a stair tread does not step it; a drop past 8 u (a fall, a ledge, a spawn) snaps. 0 = off (the body steps per tread)" };
static const cfg::Float kSlopTau{ cfg::Group{ "Locomotion" }, "anim.slop_tau", 0.25, 0.0, 2.0, "Display slop: the drawn position eases to the sim with this time constant (s), speed-capped; a small offset rides. 0 = the capped catch-up chase" };
// render.interest_radius (kenshi-k16.2.258.10.1, the town approach): the radius of the SET_INTEREST send filter. It is not simulation detail
// (the full-detail bounds decide that): a character inside the bounds and inside this circle is sent at full detail, so a radius that reaches the
// buildings the walk already draws brings a town's residents in with the town instead of ~2.7 km later (measured on the Stack approach: the
// bounds covered the town at 4.74 km, the residents were only sent at 3.34 km with 2000). A change resends the interest at once.
static const cfg::Float kInterestRadiusCfg{ cfg::Group{ "Character loading" }, "render.interest_radius", 2000.0, 200.0, 20000.0, "The radius (u) of the SET_INTEREST filter the camera sends: characters inside it and inside the full-detail bounds are sent and drawn (2000 = the old fixed value, SIMLINK 5)" };
// The run file's three case lines (kenshi-k16.2.264.1). The user's 2026-11-15 walk into Outpost s-IV: "running in place ... movement spurts every
// second or two", the body gone indoors while its tag drew, and nothing in the run file. kN: how long the wire's own position must stand still while
// the legs walk before that is a stall. kX: how far the drawn y may sit off the wire's y; the deck blend is 4 u wide by design, so a placement error
// is well past half of it. Both rate-limited per character to one line every 5 s.
static const cfg::Float kWireStillTicks{ cfg::Group{ "Run files", "", "Debug" }, "anim.wire_still_ticks", 10.0, 2.0, 200.0, "Run file: a character walking (0.5 u/s or more) while its wire position has not changed for this many ticks (20 a second) logs a walkstall line" };
static const cfg::Float kDrawYOff{ cfg::Group{ "Run files", "", "Debug" }, "anim.draw_y_off", 2.0, 0.5, 50.0, "Run file: a drawn character whose drawn y is further than this (units) from the wire's y logs a drawnoff line" };
#ifdef KCLIENT_DEV_TOOLS
// kenshi-h2v.11.5.2: the wire's kFlagDrawn decides whether a weapon is drawn (the sim's equip state). Until the sim sends that state (kenshi-k16.6.132)
// this forces the bit for shots of both states; -1 = the wire (the product behaviour).
static const cfg::Int kDrawnForce{ cfg::Group{ "Simulation", "", "Debug" }, "sim.drawn_force", -1, -1, 1, "Dev: -1 the wire's kFlagDrawn decides; 0 / 1 force every weapon sheathed / drawn (screenshots of both rest states before the sim sends the equip state)" };
#endif

// The original's locomotion speed filter (kinspect s70_static.md 6a, AnimationClass ctor): one sample per rendered frame, a window of 8, sorted,
// 3 dropped at each end, the middle 2 averaged; forward and lateral alike. The original starts from an empty window (0 until 7 samples); ours
// fills it with the first speed so a character first seen running does not stand for 6 frames (MINE).
static float trimmed_mean8(const float (&win)[8])
{
	float s[8];
	std::copy(std::begin(win), std::end(win), s);
	std::sort(s, s + 8);
	return 0.5f * (s[3] + s[4]);
}

bool SimView::open(const std::string& target, const std::string& dataDir, std::string& err)
{
	if (std::ifstream in{ target, std::ios::binary }) { return open(target, 0, err); }	// a replay file: no hash, no data needed
	try { return open(target, kworld::dataHash(kworld::resolveLoadOrder(dataDir)), err); }
	catch (const std::exception& e) { err = std::string("data hash: ") + e.what(); return false; }
}

bool SimView::open(const std::string& target, std::uint64_t dataHash, std::string& err)
{
	const size_t colon = target.rfind(':');
	std::ifstream in(target, std::ios::binary);
	if (in)
	{
		file_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		mode_ = File;
		return true;
	}
	if (colon == std::string::npos) { err = target + ": no such file (or HOST:PORT)"; return false; }
	dataHash_ = dataHash;
	std::string why;
	fd_ = kutil::net::tcpConnect(target.substr(0, colon), target.substr(colon + 1), why);
	if (fd_ == kutil::net::kNoSocket) { err = target + ": " + why; return false; }
	mode_ = Live;
	return true;
}

void SimView::end()
{
	kutil::net::tcpClose(fd_);
	*this = SimView();
}

// A character's build / pose / action failure or fallback: the console line as before, and the critical-events log (evlog)
static void charError(const char* kind, double tick, uint64_t id, const std::string& err)
{
	fprintf(stderr, "kclient: simlink: character %llu: %s\n", (unsigned long long)id, err.c_str());
	evlog::add(kind, tick, id, "%s", err.c_str());
}

void SimView::fail(const std::string& why)
{
	if (error_.empty())
	{
		fprintf(stderr, "kclient: simlink: %s\n", why.c_str());
		evlog::add("link", tick_, 0, "%s", why.c_str());
	}
	error_ = why;
	kutil::net::tcpClose(fd_);
	fd_ = kutil::net::kNoSocket;
}

bool SimView::pump()
{
	if (!error_.empty()) { return false; }
#ifdef KCLIENT_DEV_TOOLS
	if (devStall_ && mode_ == Live) { return true; }	// `set stall 1`: the snapshots are held (a network gap), the socket is left unread (kenshi-k16.2.479.1)
#endif
	if (mode_ == File)
	{
		// whole frames, until the snapshot after the render tick is complete
		while (fed_ + 6 <= file_.size() && (tick_ < 0 ? dec_.lastCompleteTick == 0 : (double)dec_.lastCompleteTick < std::ceil(tick_)))
		{
			uint32_t len;
			memcpy(&len, &file_[fed_], 4);
			const size_t n = std::min<size_t>(4 + (size_t)len, file_.size() - fed_);
			if (!dec_.feed(&file_[fed_], n)) { fail(dec_.error()); return false; }
			fed_ += n;
		}
		return true;
	}
	uint8_t buf[65536];
	for (;;)
	{
		size_t n = 0;
		std::string why;
		const kutil::net::Io r = kutil::net::tcpRecv(fd_, buf, sizeof buf, n, why);
		if (r == kutil::net::Io::Ok) { if (!dec_.feed(buf, n)) { fail(dec_.error()); return false; } continue; }
		if (r == kutil::net::Io::Closed) { fail("server closed the connection"); return false; }
		if (r == kutil::net::Io::WouldBlock) { break; }
		fail("recv: " + why);
		return false;
	}
	if (dec_.helloSeen() && !helloSent_)
	{
		std::vector<uint8_t> out;
		OrderWriter::hello(out, dataHash_, "kclient");
		if (!sendNow(out, "hello")) { return false; }
		helloSent_ = true;
		if (!preHello_.empty())
		{
			std::vector<uint8_t> h;
			h.swap(preHello_);
			if (!sendNow(h, "held frames")) { return false; }
		}
		if (dec_.serverDataHash() != dataHash_) { fprintf(stderr, "kclient: simlink: data hash %016llx, server %016llx (the server will refuse)\n", (unsigned long long)dataHash_, (unsigned long long)dec_.serverDataHash()); }
	}
	if (helloSent_ && std::chrono::steady_clock::now() - lastSend_ >= std::chrono::seconds(5))
	{
		std::vector<uint8_t> out;
		OrderWriter::ping(out);
		if (!send(out, "ping")) { return false; }
	}
	return true;
}

// kenshi-k16.2.414.2: the ONE ground solver, navmesh -> drawn (CLIENT.md Decisions "One ground solver, both directions"); the rule is 412.1's (kenshi-k16.2.382 / .382.1 / .412.1).
// `raw` is the session's ray over WorldBuildings::groundDrawn, which sees only WALKABLE drawn parts (the navmesh bake's own set, BuildingPartDef::walkable): 2 a drawn floor / stair / ramp
// triangle in the window, 0 a walkable surface within 12 u of the navmesh y covers x,z but none in the window (ry = the nearest such surface), -1 a walkable building mesh near x,z
// still loading, 3 open ground: no walkable surface over x,z. `th` is the drawn terrain under the same x, z. Open ground: the terrain at any distance (the coarse navmesh y runs 12-33 u
// under the drawn dune of the hill east of the Storm House, user 2026-11-19), so the body never stays at the navmesh y. A walkable surface outside the window (0): the nearer to the navmesh y of that
// surface and the terrain, so a body on the terrain under a porch roof or on a bridge deck whose navmesh point sits low is not lifted onto it; the terrain is excluded only where it is
// not ground, inside a carved interior (an interior mask whose BUILDING has `interior terrain` 0, the sunk-dune rule in data form: terrainMasked_ at the terrain point). In the window (1, 2)
// the terrain competes and wins when nearer the navmesh y (a floor under a dune, a dune over a floor); not loaded (-1) keeps the navmesh y. Returns the new result, ry set when >= 1.
int SimView::solveGround(int raw, double& ry, double x, double z, double th, double navY, double up, double down) const
{
	if (raw == 3) { ry = th; return 1; }
	if (raw == 0)
	{
		if (!(terrainMasked_ && terrainMasked_(x, th, z)) && std::fabs(th - navY) < std::fabs(ry - navY)) { ry = th; }
		return 1;
	}
	if (raw != -1 && th <= navY + up && th >= navY - down && (raw < 1 || std::fabs(th - navY) < std::fabs(ry - navY))) { ry = th; return 1; }
	return raw;
}

int SimView::groundAt(double x, double z, double navY, double up, double down, double& y, const char** kind) const
{
	const char* k = "";
	if (kind) { *kind = k; }
	if (!groundRay_) { return -1; }
	const double th = terrainFn_ ? terrainFn_(x, z) : navY;
	const void* gen = nullptr;
	double ry = 0;
	const int raw = groundRay_(x, z, navY, up, std::max(down, kDrawnDown), ry, &gen);
	const double before = ry;
	const int res = solveGround(raw, ry, x, z, th, navY, up, kDrawnDown);
	if (res >= 1) { k = raw == 3 ? "open ground (terrain)" : raw < 1 || ry != before ? "terrain" : "drawn floor"; y = ry; }
	else { k = raw == -1 ? "building mesh still loading" : "no drawn floor in the window"; }
	if (kind) { *kind = k; }
	return res;
}

int SimView::standRayAt(double x, double z, double wireY, double terrain, double& y, const char** kind) const
{
	const char* k = "";
	if (kind) { *kind = k; }
	if (!groundRay_) { return -1; }
	const void* gen = nullptr;
	double ry = 0;
	const int raw = groundRay_(x, z, wireY, kRayUp, kRayDown, ry, &gen);
	const double before = ry;
	const int res = solveGround(raw, ry, x, z, terrain, wireY, kRayUp, kDrawnDown);	// the one rule update() applies
	if (res >= 1) { k = raw == 3 ? "open ground (terrain)" : raw < 1 || ry != before ? "terrain" : "drawn floor"; y = ry; }
	else { k = raw == 3 ? "open ground, terrain beyond the reach" : raw == -1 ? "building mesh still loading" : "no drawn floor in the window"; }
	if (kind) { *kind = k; }
	return res;
}

void SimView::update(double dt, Characters& chars, const std::function<double(double, double)>& ground)
{
	if (mode_ == None) { return; }
	const bool paused = dec_.clock.valid && dec_.clock.paused;
	// kenshi-k16.2.479.1: the animation clock is the sim time the render clock really advanced this frame (set at the end of update), not the
	// wall dt x game speed: a stall (no snapshot, the clock clamped at the newest sample, a slow sim) holds the pose instead of cycling the legs on the spot
	const double tickFrom = tick_;
	dtAnim_ = 0;
	if (tick_ >= 0)
	{
		// A paused sim holds the render clock (kenshi-h2v.23.1). It used to advance at 1x, so the Live hold below pulled it back a fraction
		// of a tick a frame, less than it gained at 60 fps: the clock ran past the newest snapshot (the drawn clamped, looking still), the
		// error grew to the snap size and the hold jumped it back, replaying the last ~0.5 s of motion — the paused "shimmer" of the
		// bar_lure v2 bandits. Frozen, the hold settles it once at the newest snapshot less the delay and it stays; unpausing resumes.
		// kenshi-k16.2.477.1: at a speed the sim cannot reach the clock advances at the rate the snapshots really arrive (a nominal 100 ticks/s
		// against 50 delivered ran the clock ahead of the newest snapshot, and the hold below eased it back -10 ticks every few seconds: the screen
		// jumps); a sim that keeps up (achieved >= 95% of nominal) leaves the nominal rate, so 1x and a healthy 5x are unchanged.
		double rate = kTickRate * (dec_.clock.valid ? dec_.clock.speed : 1.0);
		if (mode_ == Live && rateValid_ && achievedRate_ < rate * 0.95) { rate = std::max(achievedRate_, rate * 0.1); }
		tick_ += paused ? 0.0 : dt * rate;
		dtSim_ = dt * (paused ? 0.0 : dec_.clock.valid ? dec_.clock.speed : 1.0);
	}
	// The 100 ms of wall time the client renders behind the newest snapshot, in ticks of this game speed (kenshi-k16.2.181.1; delayTicks)
	const double delay = delayTicks();
	bool pumped;
	{ CPUSTAGE_ACC("sim.pump"); pumped = pump(); }	// the socket read and the wire decode (kenshi-k16.2.205.12.76)
	if (!pumped && dec_.lastCompleteTick == 0) { return; }
	if (mode_ == Live && !paused && dec_.clock.valid)	// the achieved tick rate: newest snapshot's advance over >= 0.5 s of wall time, smoothed
	{
		const double newest = (double)dec_.lastCompleteTick;
		if (!rateSeeded_ || newest < rateFrom_) { rateSeeded_ = true; rateFrom_ = newest; rateWall_ = 0; }
		rateWall_ += dt;
		if (rateWall_ >= 0.5)
		{
			const double r = (newest - rateFrom_) / rateWall_;
			achievedRate_ = rateValid_ ? 0.5 * achievedRate_ + 0.5 * r : r;
			rateValid_ = true;
			rateFrom_ = newest;
			rateWall_ = 0;
		}
	}
	else if (paused) { rateSeeded_ = false; }
	// The focused producer's row (k16.19.8.2): it arrived only because of order 87, so hold it apart from dec_.buildings, where the player's own buildings
	// live, for the focus panel; one the player owns is a player's building and stays in that map (focusBuilding reads it there).
	for (const uint64_t id : focusProducers_)
	{
		const auto it = dec_.buildings.find(id);
		if (it == dec_.buildings.end() || playerBuilding(it->second)) { continue; }
		simlink::SimLinkDecoder::Building row = std::move(it->second);
		dec_.buildings.erase(it);
		dec_.buildingsSerial++;
		if (id == focusSent_ && focusProducer_) { focusRow_ = std::move(row); }
	}
	if (tick_ < 0)
	{
		if (dec_.lastCompleteTick == 0) { return; }
		tick_ = mode_ == File && startTick > 0 ? startTick : (double)dec_.lastCompleteTick - (mode_ == Live ? delay : 0.0);
		pump();
	}
	if (mode_ == Live)
	{
		// Hold about `delay` ticks behind the newest snapshot: snap when far off, else nudge the clock (the user's "leap frames": a small
		// desync is fixed by the interpolation clock, not by a spatial chase). The rate is time-based so a low frame rate does not slow
		// the correction, and capped so one late tick is spread over a few frames instead of a jump. After this frame's pump, or the hold
		// trails the snapshot by one frame (measured 2.33 ticks instead of 2 before the move). Measured: the render lag ran 1.68..3.64
		// ticks around the 2-tick design at 1x and the legs ride this clock (a longer lag keeps the stride running after the sim stops, L1).
		// The tick-domain caps stay unscaled with the game speed (kenshi-k16.2.181.1): they bound how fast the clock is corrected, and at
		// 5x the sim sometimes runs slower than its nominal speed (a tick over the 10 ms interval), so a tighter band tracks it better.
		// MINE: kClockTau and kClockStep; the original's correction rate is not traced.
		// kenshi-k16.2.477.1: the correction caps (kClockStep, kClockEase, kClockEaseMax) are ticks, and a game speed s runs s times the ticks a frame: at x5 a sim that
		// bursts after a 250 ms hiccup leaves the clock 25-33 ticks behind, past the 24-tick ease limit, and the ease at 0.5 ticks a frame lost to the burst: every one
		// became a +25 tick snap (88 u of a running body). The caps scale with the speed (>= 1x), so the ease finishes in the frames the 1x one does.
		const double kSpeedScale = std::max(1.0, (double)(dec_.clock.valid ? dec_.clock.speed : 1.0));
		const double kClockTau = 0.10, kClockStep = 0.25 * kSpeedScale, kClockBand = 0.25, kClockEase = 0.5 * kSpeedScale, kClockEaseMax = 24.0 * kSpeedScale;
		// kenshi-k16.2.74: the first snapshots arrive in a burst, so the first big clock error is a backlog, not a discontinuity. Ease it in (the
		// sample history reaches back further than kClockEaseMax, so the interpolation never clamps) instead of jumping the whole world forward; a
		// reset (load_game) and a jump past kClockEaseMax ticks still snap. The display offset holds each drawn character still while it eases.
		clockSnapped_ = false;
		const bool resetLoad = synced_ && dec_.resetSerial != resetSeen_;
		if (resetLoad) { resetSnap_ = true; clockEasing_ = false; }	// a load: snap the clock and every display offset
		// The clock never runs past the newest snapshot (it could only draw the last sample still): a stall bounds the overrun to the delay instead
		// of building a -10..-24 tick error that the ease then swept back as a visible jump (kenshi-k16.2.477.1). A load still snaps below.
		if (!paused && !resetLoad && dec_.lastCompleteTick > 0 && tick_ > (double)dec_.lastCompleteTick - 1.0) { tick_ = (double)dec_.lastCompleteTick - 1.0; }
		const double want = (double)dec_.lastCompleteTick - delay, err = want - tick_;
		// A paused sim holds the render clock whole (h2v.23.1, kenshi-k16.2.317): no ease and no snap either, not just no settle. The deadband
		// settle was frozen first; the ease was not, and a run slow enough to leave the clock over 10 ticks behind when the pause lands (the
		// headless Intel test card draws pausetoggle at ~20 fps while the sim runs in real time) swept the clock forward over the samples frozen
		// at the pause, replaying the walk from before it: a paused character walked 0.58 u a frame for ~26 frames (0.646 u max) in
		// kenshi-k16.2.317's log. Frozen, the pause's frame stays; unpausing resumes the correction, and a load (resetLoad) still snaps.
		const bool holdClock = paused && !resetLoad;
		// a big error starts an ease; once started it runs down to the deadband (below 10 ticks the size shows in the history, so it is still safe)
		if (!holdClock && (std::fabs(err) > 10 || clockEasing_))
		{
			if (kSlopTau.get() > 0 && !resetLoad && std::fabs(err) < kClockEaseMax)
			{
				if (!clockEasing_) { evlog::add("clock", tick_, 0, "easing %+.1f ticks to the newest snapshot less %.0f (newest %llu)", err, delay, (unsigned long long)dec_.lastCompleteTick); clockEasing_ = true; }
				tick_ += std::clamp(err * std::min(1.0, dt / kClockTau), -kClockEase, kClockEase);
				if (std::fabs(want - tick_) <= kClockBand) { evlog::add("clock", tick_, 0, "ease done at %+.1f ticks behind the newest snapshot", want - tick_); clockEasing_ = false; }
			}
			else
			{
				evlog::add("clock", want, 0, "render clock snapped %+.1f ticks to the newest snapshot less %.0f (newest %llu)", err, delay, (unsigned long long)dec_.lastCompleteTick);
				tick_ = want;
				clockEasing_ = false;
				clockSnapped_ = true;
			}
		}
		else if (std::fabs(err) > kClockBand && !paused)	// the deadband: a fractional lag is the ticks' own staircase, chasing it only judders the drawn motion; paused: no settle, the pause's frame stays (h2v.23.1)
		{
			const double over = err - std::copysign(kClockBand, err);
			tick_ += std::clamp(over * std::min(1.0, dt / kClockTau), -kClockStep, kClockStep);
		}
	}
	// a file at its end holds the last tick
	if (mode_ == File) { tick_ = std::min(tick_, (double)dec_.lastCompleteTick); }
	// kenshi-k16.2.479.1: the render clock never runs backwards (a snap, a load excepted). The clamp to the newest snapshot and the settle above pulled it back
	// a fraction of a tick a frame through a stall: the drawn bodies slid backwards (rtick 83160.6 -> 83158.3 in the stall check) while the legs held.
	if (tickFrom >= 0 && !clockSnapped_ && tick_ < tickFrom) { tick_ = tickFrom; }
	// The sim time drawn this frame, for the animation. A snap (a load, a far jump) is a teleport, not time passing: it takes the nominal frame.
	dtAnim_ = clockSnapped_ && tickFrom >= 0 ? dtSim_ : tickFrom >= 0 ? std::max(0.0, (tick_ - tickFrom) / kTickRate) : 0.0;
	if (!held_) { CPUSTAGE_ACC("sim.sync"); sync(chars, ground); }
}

void SimView::hold(bool on)
{
	if (held_ && !on) { rebuild_ = true; }
	held_ = on;
}

std::string SimView::wornMaterial(const Characters& chars, uint64_t id, const std::string& part) const
{
	static const char* const kMaterial[] = { "Cloth", "Leather", "Chain", "Plate" };
	const auto v = dec_.chars.find(id);
	if (v == dec_.chars.end()) { return std::string(); }
	int worn = -1;	// the highest MaterialType among the worn pieces (that cover the part)
	for (const Item& it : v->second.look.items)
	{
		const Characters::ItemInfo* info = chars.itemInfo(dec_.str(it.item));
		if (!info || info->armourClass < 0) { continue; }
		if (part.empty()) { worn = std::max(worn, info->material); continue; }
		for (const auto& [name, cover] : info->coverage) { if (cover > 0 && name == part) { worn = std::max(worn, info->material); } }
	}
	return worn >= 0 ? kMaterial[worn] : "";
}

// Door, carry, pick-up and lock sounds (kenshi-h2v.29.8), after the events of the batch: door state changes and carry changes are read off the wire's
// state, so a first sight (a load, a newly interesting door) posts nothing. Door objects: 0x40000000 + the interactable's id, in the audiohook's uint32
// space between the characters' sim ids (small counters) and the temporary objects (counting down from 0xFFFFFFFF).
void SimView::cueWorld(const Characters& chars)
{
	const auto cue = [&](uint64_t object, const double pos[3], std::vector<std::pair<std::string, std::string>> sw, const char* event, double due = 0)
	{
		SoundCue c;
		c.object = object;
		c.tick = (uint64_t)std::max(0.0, tick_);
		for (int k = 0; k < 3; k++) { c.pos[k] = pos[k]; }
		c.switches = std::move(sw);
		c.event = event;
		c.gate = 0;
		if (due > 0) { pending_.push_back({ due, std::move(c) }); } else { cues_.push_back(std::move(c)); }
	};
	for (auto it = pending_.begin(); it != pending_.end();)
	{
		if (tick_ >= it->first) { cues_.push_back(std::move(it->second)); it = pending_.erase(it); } else { ++it; }
	}
	std::unordered_set<uint64_t> seenDoors;
	for (const auto& [id, t] : dec_.things)
	{
		if (t.kind != Interactable::kDoor) { continue; }
		seenDoors.insert(id);
		const auto prev = doorState_.find(id);
		const uint8_t was = prev == doorState_.end() ? 255 : prev->second;
		doorState_[id] = t.state;
		if (was == 255) { doorFirst_[id] = tick_; }
		if (!soundsPrimed_ || was == 255 || was == t.state || tick_ - doorFirst_[id] < kTickRate) { continue; }
		const std::string sid = dec_.str(t.record);
		const auto ds = doorSounds_.find(sid);
		static const char* const kType[] = { "Wood", "Mechanical_Metal", "Gate" };
		std::vector<std::pair<std::string, std::string>> sw;
		if (ds != doorSounds_.end()) { sw.push_back({ "Door_Type", kType[std::clamp(ds->second.type, 0, 2)] }); }
		const uint64_t object = 0x40000000ull + id;
		const double travel = (ds != doorSounds_.end() && ds->second.speed > 0 ? 1.0 / ds->second.speed : 2.0) * kTickRate;
		double p[3] = { t.pos[0], t.pos[1], t.pos[2] };
		if (t.state == Interactable::kBroken) { cue(object, p, sw, "Break"); }
		else if (t.state == Interactable::kOpen) { cue(object, p, sw, "Door_Open"); cue(object, p, sw, "Door_Shut", tick_ + travel); }
		else if (t.state == Interactable::kClosed && was == Interactable::kOpen) { cue(object, p, sw, "Door_Close"); cue(object, p, sw, "Door_Shut", tick_ + travel); }
		else if (t.state == Interactable::kLocked) { cue(object, p, sw, "Character_Lock"); }
		if (was == Interactable::kLocked && t.state != Interactable::kLocked && t.state != Interactable::kBroken && !rollOk_.count(id)) { cue(object, p, sw, "Character_Unlock_Success"); }
	}
	std::erase_if(doorState_, [&](const auto& kv) { return !seenDoors.count(kv.first); });
	std::erase_if(doorFirst_, [&](const auto& kv) { return !seenDoors.count(kv.first); });
	// carrying: the body's carriedBy
	std::unordered_map<uint64_t, uint64_t> now;
	for (const auto& [id, c] : dec_.chars) { if (c.cur.carriedBy) { now[id] = c.cur.carriedBy; } }
	if (soundsPrimed_)
	{
		double p[3];
		float hull[3];
		// Character_Pick_Up_Person on the carrier (Armor_Worn of the body), Character_Drop_Person where the carrier is when the body is put down
		for (const auto& [body, carrier] : now)
		{
			if (carried_.count(body) || !placed(carrier, p, hull)) { continue; }
			const std::string worn = wornMaterial(chars, body, std::string());
			cue(carrier, p, worn.empty() ? std::vector<std::pair<std::string, std::string>>() : std::vector<std::pair<std::string, std::string>>{ { "Armor_Worn", worn } }, "Character_Pick_Up_Person");
		}
		for (const auto& [body, carrier] : carried_)
		{
			if (now.count(body) || !placed(carrier, p, hull)) { continue; }
			const std::string worn = wornMaterial(chars, body, std::string());
			cue(carrier, p, worn.empty() ? std::vector<std::pair<std::string, std::string>>() : std::vector<std::pair<std::string, std::string>>{ { "Armor_Worn", worn } }, "Character_Drop_Person");
		}
	}
	carried_ = std::move(now);
	soundsPrimed_ = true;
}

void SimView::cueSounds(const Characters& chars, const Event& e, const std::set<std::pair<uint64_t, uint64_t>>& shots)
{
	const auto cue = [&](uint64_t object, const double pos[3], std::vector<std::pair<std::string, std::string>> sw, const char* event, bool temporary = false)
	{
		SoundCue c;
		c.object = object;
		c.tick = e.tick;
		c.temporary = temporary;
		for (int k = 0; k < 3; k++) { c.pos[k] = pos[k]; }
		c.switches = std::move(sw);
		c.event = event;
		cues_.push_back(std::move(c));
	};
	const auto place = [&](uint64_t id, double pos[3]) { float hull[3]; return placed(id, pos, hull); };
	// the voice switches of a character (VO_Races / Creature_VO), set before its voice event
	const auto voice = [&](uint64_t id, Characters::Voice& v)
	{
		const auto it = dec_.chars.find(id);
		if (it == dec_.chars.end()) { return false; }
		v = chars.voiceOf(chars.nameOf(dec_.str(it->second.look.race)), it->second.look.sex != 0);
		return !v.switches.empty();
	};
	// rand() & 8 == 0 in receiveHit: a deterministic draw from the event (tick, victim): bit 3 of a mixed hash
	const auto draw = [&](uint64_t a, uint64_t b) { uint64_t h = (a ^ 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull ^ b * 0xc4ceb9fe1a85ec53ull; h ^= h >> 29; return (h >> 3 & 1) == 0; };
	double p[3];
	if (e.kind == 1)	// hit: a = victim, b = attacker, result 0 hit / 1 blocked / 2 dodged
	{
		if (e.result == 2 || !place(e.a, p)) { return; }	// a dodge: the findings give no sound
		if (e.result == 0) { lastAttacker_[e.a] = e.b; }
		std::vector<std::pair<std::string, std::string>> sw;
		const char* event = "Impact";
		if (e.result == 1) { sw.push_back({ "Impacts", "Parry" }); }
		else if (shots.count({ e.b, e.a }))	// a projectile hit (hit type 6, 140439c90): Deflection on an armoured part, else the plain impact
		{
			const std::string worn = wornMaterial(chars, e.a, chars.nameOf(dec_.str(e.str)));
			if (!worn.empty())
			{
				event = "Deflection";
				sw.push_back({ "Armor_Worn", worn });
			}
			else { sw.push_back({ "Impacts", "HtH_Impacts" }); }
		}
		else
		{
			bool armed = false;	// the attacker holds a weapon in the hand (attacker+0x1f0 != 5)
			if (const auto a = dec_.chars.find(e.b); a != dec_.chars.end())
			{
				for (const Item& it : a->second.look.items) { armed = armed || (it.section == kHand && chars.isWeapon(chars.nameOf(dec_.str(it.item)))); }
			}
			sw.push_back({ "Impacts", armed ? "HtH_Impacts" : "HtH_Impacts_Robot" });
		}
		cue(e.a, p, std::move(sw), event);
		Characters::Voice v;
		if (e.result == 0 && draw(e.tick, e.a) && voice(e.a, v)) { cue(e.a, p, v.switches, v.animal ? "VO_Creature_Take_Damage" : "VO_Get_Hit"); }
	}
	else if (e.kind == 5)	// death: an animal's own cry; its killer, an animal, has won
	{
		Characters::Voice v;
		if (place(e.a, p) && voice(e.a, v) && v.animal) { cue(e.a, p, v.switches, "VO_Creature_Die"); }
		const auto k = lastAttacker_.find(e.a);
		if (k != lastAttacker_.end())
		{
			if (place(k->second, p) && voice(k->second, v) && v.animal) { cue(k->second, p, v.switches, "VO_Creature_Victory"); }
			lastAttacker_.erase(k);
		}
	}
	else if (e.kind == 49)	// shot: a = shooter, str = the CROSSBOW; Weapons from its `shoot sound`, Attack on the shooter's object
	{
		if (!place(e.a, p)) { return; }
		static const char* const kShoot[] = { "Crossbow", "Harpoon", "Crossbow_Turret", "Harpoon_Turret" };
		const Characters::ItemInfo* gun = chars.itemInfo(dec_.str(e.str));
		std::vector<std::pair<std::string, std::string>> sw;
		if (gun && gun->shootSound >= 0 && gun->shootSound < 4) { sw.push_back({ "Weapons", kShoot[gun->shootSound] }); }
		cue(e.a, p, std::move(sw), "Attack");
		if (e.result == 2)	// a miss: the bolt's ground impact at the target point, on a temporary object (the ground material is not known: Stone)
		{
			const double to[3] = { e.to[0], e.to[1], e.to[2] };
			cue(tempObject_--, to, { { "Impacts", "Stone" } }, "Impact", true);
		}
	}
	else if (e.kind == 3) { lastAttacker_.erase(e.a); }
	else if (e.kind == 7)	// a character picked an item up from the ground: Inventory_Items from the item (none for a weapon: not established)
	{
		if (!place(e.a, p)) { return; }
		std::vector<std::pair<std::string, std::string>> sw;
		if (const Characters::ItemInfo* info = chars.itemInfo(dec_.str(e.str)); info && !info->soundSwitch.empty()) { sw.push_back({ "Inventory_Items", info->soundSwitch }); }
		cue(e.a, p, std::move(sw), "Character_Pick_Up_Ground");
		cues_.back().gate = 0;
	}
	else if (e.kind == 13 && e.dir == 5 && (e.result == 0 || e.result == 6))	// a loot order reached its target (the loot task's start, Task_Loot_Order::vf1)
	{
		if (!place(e.a, p)) { return; }
		cue(e.a, p, {}, "Character_Inspect");
		cues_.back().gate = 0;
	}
	else if (e.kind == 20)	// lock_roll: a = the picker, b = the door, result = success
	{
		if (!place(e.a, p)) { return; }
		if (e.result) { rollOk_.insert(e.b); }
		cue(e.a, p, {}, e.result ? "Character_Unlock_Success" : "Character_Unlock_Fail");
		cues_.back().gate = 0;
	}
}

Characters::Spec SimView::specOf(uint64_t id, const Characters& chars) const
{
	const Character& c = dec_.chars.at(id);
	Characters::Spec s;
	const Appearance& a = c.look;
	s.id = id;	// the log lines' name for it (Characters::label: "41253 Greenlander")
	s.player = (c.cur.flags & kPlayer) != 0;	// the dev stand-in hold skips the squad (kenshi-k16.2.258.1)
	s.race = chars.nameOf(dec_.str(a.race));
	s.female = a.sex != 0;
	s.head = a.head;
	if (a.faction) { s.faction = chars.nameOf(dec_.str(a.faction)); }
	if (a.hair) { s.hair = chars.nameOf(dec_.str(a.hair)); }
	if (a.beard) { s.beard = chars.nameOf(dec_.str(a.beard)); }
	if (a.hairColour) { s.hairColour = chars.nameOf(dec_.str(a.hairColour)); }
	if (a.skin > 0) { s.skin = a.skin; }
	std::string hand, hip, back;
	for (const Item& it : a.items)
	{
		const std::string name = chars.nameOf(dec_.str(it.item));
		const bool weapon = chars.isWeapon(name);
		if (weapon && it.section == kHand) { hand = name; }
		else if (weapon && it.section == kHip) { hip = name; }
		else if (weapon && (it.section == kBack || it.section == kBack2)) { back = name; }
		else if (!weapon) { s.wear.push_back(name); }
	}
	// weapons[0] at the hip (drawn: in the hand, its sheath left at the hip), [1] on the back. The sim sends the weapon item in the section it is
	// held in (today always the hand), so the item list alone cannot say whether it is DRAWN: that is the wire's kFlagDrawn, the sim's equip state
	// (the plan's EQUIP_WEAPON / UNEQUIP_WEAPON; the original sheathes 2-2.5 s after a fight, no sheathe clip, kenshi-rwh.105 / kenshi-k16.6.132).
	// Code against the flag, not a timer: clear puts the item mesh back on its attach and the idle / upper-body records back on the hands set.
	bool drawn = (c.cur.flags & kDrawn) != 0;
#ifdef KCLIENT_DEV_TOOLS
	if (const int f = kDrawnForce.get(); f >= 0) { drawn = f != 0; }	// dev A/B, sim.drawn_force
#endif
	if (!hand.empty() || !hip.empty()) { s.weapons.push_back(hand.empty() ? hip : hand); s.drawn = drawn; }
	if (!back.empty()) { if (s.weapons.empty()) { s.weapons.push_back(""); } s.weapons.push_back(back); }
	// Operating a producer (CHAR_STATS goal 87 OPERATE_MACHINERY, targetId = the producer, SIMLINK row 16): its functionality's `special tool` ITEM is
	// held in the right hand (the stonemine / farming clips have `has weapon R`), and the weapon goes back to its sheath. The sheathing is MINE: the
	// original's weapon handling during the tool is NOT TRACED (Task_OperateMachine::vf1 1400b64b0).
	if (const auto cs = dec_.charStats.find(id); cs != dec_.charStats.end() && cs->second.hasAi && cs->second.goalEnum == 87 && !specialTools_.empty())
	{
		const auto th = dec_.things.find(cs->second.targetId);
		const auto tool = th != dec_.things.end() && th->second.kind == simlink::Interactable::kProducer ? specialTools_.find(dec_.str(th->second.record)) : specialTools_.end();
		if (tool != specialTools_.end() && !tool->second.empty()) { s.tool = tool->second; s.drawn = false; }
	}
	s.loaded = a.crossbowAmmo > 0;
	s.loco = true;	// an up character is drawn by the locomotion controller; its speed is the live vFwd (line 404), which may be negative
	s.move = 0;
	s.carry = c.cur.flags & kCarryL ? 1 : c.cur.flags & kCarryR ? 2 : 0;	// carrying a body (CHAR_DYNAMIC flag bits 7 / 8)
	s.severed = severedOf(id, s.race, chars);
	return s;
}

// severed limbs: CHAR_MEDICAL bit i = part row i, mapped to the limb slot by the part's bones; race: specOf's (kenshi-k16.2.205.12.77: the rebuild's
// carry-over test reads this alone, not a whole spec per drawn character)
int SimView::severedOf(uint64_t id, const std::string& race, const Characters& chars) const
{
	int out = 0;
	if (const auto m = dec_.medical.find(id); m != dec_.medical.end() && m->second.severed)
	{
		for (size_t i = 0; i < m->second.parts.size() && i < 16; i++)
		{
			if (!(m->second.severed >> i & 1)) { continue; }
			if (const int k = chars.limbSlot(race, chars.nameOf(dec_.str(m->second.parts[i].part))); k >= 0) { out |= 1 << k; }
		}
	}
	return out;
}

bool SimView::nudgeOk(const Dynamic& d, uint64_t id) const
{
	return !(d.flags & (kKO | kDead | kProne | kSeated | kAsleep | kCombat | kMounted)) && !d.carriedBy && !dec_.cages.count(id);
}

void SimView::sync(Characters& chars, const std::function<double(double, double)>& ground)
{
	rayStats_.maxFrameCasts = std::max(rayStats_.maxFrameCasts, rayStats_.frameCasts);	// the standing ray's last frame (kenshi-k16.2.344)
	rayStats_.maxFrameMs = std::max(rayStats_.maxFrameMs, rayStats_.frameMs);
	rayStats_.frameCasts = 0; rayStats_.frameMs = 0;
	if (rayStats_.casts + rayStats_.cached > 0 && tick_ - rayLogTick_ >= 10 * kTickRate)	// one summary line every 10 s of sim time
	{
		rayLogTick_ = tick_;
		const RayStats& r = rayStats_;
		evlog::add("standray", tick_, 0, "casts %llu cached %llu hits %llu (drawn faces %llu) misses %llu unloaded %llu; %.3f ms/cast, frame max %d casts %.2f ms; shift mean %.2f max %.2f u", (unsigned long long)r.casts, (unsigned long long)r.cached,
			(unsigned long long)r.hits, (unsigned long long)r.faces, (unsigned long long)r.misses, (unsigned long long)r.unloaded, r.casts ? r.ms / (double)r.casts : 0.0, r.maxFrameCasts, r.maxFrameMs, r.hits ? r.sumShift / (double)r.hits : 0.0, r.maxShift);
	}
	if (nudge_.frames > 0 && tick_ - nudgeLogTick_ >= 10 * kTickRate)	// kenshi-k16.2.345: one summary line every 10 s of sim time
	{
		nudgeLogTick_ = tick_;
		for (const auto& [sid, ss] : slots_)	// the bodies nudged now (where to frame a before / after shot)
		{
			if (const double o = std::hypot(ss.nudge[0], ss.nudge[1]); o > 0.1) { std::string why; double pu[2]; if (depen_) { depen_(ss.pos[0] - ss.nudge[0], ss.pos[2] - ss.nudge[1], ss.pos[1], 0.45, 0.45, pu, &why); } evlog::add("nudge", tick_, sid, "nudged now: %.2f u (%.2f, %.2f) at %.1f,%.1f y %.1f: %s", o, ss.nudge[0], ss.nudge[1], ss.pos[0], ss.pos[2], ss.pos[1], why.c_str()); }
		}
		evlog::add("nudge", tick_, 0, "body nudge: %.2f bodies a frame (frame max %d), largest offset %.3f u (cap 0.45); %.4f ms a frame mean, %.3f ms frame max", (double)nudge_.nudgedSum / (double)nudge_.frames, nudge_.maxFrameNudged, nudge_.maxOffset, nudge_.msSum / (double)nudge_.frames, nudge_.maxFrameMs);
	}
	bool worldReset = false;	// a load / new game: no drawn pose of the old world carries over (kenshi-k16.2.144)
	if (resetSeen_ != dec_.resetSerial)
	{
		resetSeen_ = dec_.resetSerial;
		worldReset = true;
		selected_.clear();
		holdEmpty_ = false;
		selectedOnce_ = false;	// the loaded game starts with its squad selected, as a new connection does
		rosterWarm_ = false;	// a load's roster is a first roster too
		viewOnce_ = false;	// resend the interest and the view focus: the new world's session may not hold them
		foreign_.clear();
		splashes_.clear();
		orderRows_.clear();	// the shot's route sidecar: the old world's orders are not the new one's
		orderResults_.clear();
		doorState_.clear();
		doorFirst_.clear();
		carried_.clear();
		departing_.clear();	// a load / new world leaves no one to dither out (kenshi-k16.2.258.10.1)
		pending_.clear();
		soundsPrimed_ = false;
	}
	// the character set: every sim character with an appearance, in id order. On a change, the ones whose look (version, severs) is unchanged keep
	// their drawn state (animation phase, blend weights); a rebuild of all of them restarted every walk cycle whenever someone entered interest.
	std::vector<uint64_t> ids;
	for (const auto& [id, c] : dec_.chars) { if (c.hasAppearance && c.samples > 0) { ids.push_back(id); } }
	std::sort(ids.begin(), ids.end());
	bool rebuild = rebuild_ || worldReset || ids != order_ || chars.takeDepartDone();	// a finished departure is dropped by the next keep
	rebuild_ = false;
	for (size_t i = 0; i < ids.size() && !rebuild; i++) { rebuild = slots_[ids[i]].version != dec_.chars.at(ids[i]).look.version; }
	if (rebuild)
	{
		CPUSTAGE_ACC("sim.sync.rebuild");	// the character set: keep, prefetch, readiness and the new adds (kenshi-k16.2.205.12.76)
		std::unordered_map<uint64_t, Slot> old = std::move(slots_);
#ifdef KCLIENT_DEV_TOOLS
		// the frame each character first enters the set (KCLIENT_STAGES): its add's delay is measured from here (kenshi-k16.2.205.12.77)
		if (cpustage::on()) { for (const uint64_t id : ids) { if (!old.count(id) && arrived_.insert(id).second) { CPUSTAGE_NOTE("arrive id %llu", (unsigned long long)id); } } }
#endif
		slots_.clear();
		order_ = ids;
		std::vector<int> from;
		{
			CPUSTAGE_ACC("sim.rebuild.match");	// which drawn characters carry over (kenshi-k16.2.205.12.77)
			for (const uint64_t id : ids)
			{
				const auto o = old.find(id);
				if (worldReset || o == old.end() || o->second.index < 0 || o->second.down == Carried || o->second.version != dec_.chars.at(id).look.version || severedOf(id, chars) != o->second.severed) { continue; }
				Slot& sl = slots_[id] = o->second;
				sl.index = (int)from.size();
				from.push_back(o->second.index);
			}
		}
		// The characters that left the wire this frame dither out where they last stood instead of vanishing in one frame (render.char_fade,
		// kenshi-k16.2.258.10.1): their build is kept (frozen) after the shown ones until the fade ends (Characters::takeDepartDone). A character
		// that comes back inside the fade is built fresh below and its ghost dropped here (one frame of the old body, at its old spot).
		std::vector<int> depart;
		std::vector<uint64_t> departIds;
		if (!worldReset)
		{
			for (const auto& [id, idx] : departing_)
			{
				if (std::binary_search(ids.begin(), ids.end(), id)) { continue; }	// back: the adds below build it again
				if (idx < 0 || (size_t)idx >= chars.size() || chars.cover((size_t)idx) <= 0) { continue; }	// its dither ended (takeDepartDone): drop it
				depart.push_back(idx);
				departIds.push_back(id);
			}
			for (const auto& [id, o] : old)
			{
				if (o.index < 0 || slots_.count(id) || departing_.count(id) || std::binary_search(ids.begin(), ids.end(), id)) { continue; }
				depart.push_back(o.index);
				departIds.push_back(id);
			}
		}
		{ CPUSTAGE_ACC("sim.rebuild.keep"); chars.keep(from, depart); }
		departing_.clear();
		for (size_t j = 0; j < depart.size(); j++) { departing_[departIds[j]] = (int)(from.size() + j); }
		// Building a character loads its race, meshes and textures on this thread (2-4 ms each, 1.1 s for the 302 of a join: kenshi-k16.2.205.10), so
		// the adds of one sync stop at a time budget and the rest wait for the next frame (their slot stays out, index -1, as an unbuildable one's does).
		// At least one is built per frame, so a join always finishes.
		const auto addStart = std::chrono::steady_clock::now();
		int added = 0, ahead = 0;
		squadWaiting_ = false;
		constexpr int kLookahead = 12;	// characters whose textures are queued for the decode workers at once (a 4096^2 armour .dds is 22 MB decoded)
		// the player's own squad first: the camera, the selection and the HUD wait on it, so a join shows it before the crowd
		std::vector<uint64_t> addOrder = ids;
		std::stable_partition(addOrder.begin(), addOrder.end(), [&](uint64_t id) { return dec_.chars.at(id).cur.flags & kPlayer; });
		// the squad's arrival effects (the first selection, so the start centre + follow and a script's keys on the selection) wait for the frame it is
		// built, not the frame it arrives: a new squad member whose race body or textures are still on the workers holds them (squadWaiting_) for a few
		// frames instead of blocking the main thread in the take (kenshi-k16.2.205.12.63). Its decodes are queued together first.
		if (kCharDefer.get()) { for (const uint64_t id : addOrder) { if (!slots_.count(id) && (dec_.chars.at(id).cur.flags & kPlayer)) { chars.prefetch(specOf(id, chars)); } } }
		// the first roster's races (body mesh + skeleton, skin and head textures) go to the workers before its build (kenshi-k16.2.205.12.63)
		if (!rosterWarm_ && !ids.empty())
		{
			rosterWarm_ = true;
			std::unordered_set<std::string> seen;
			for (const uint64_t id : addOrder)
			{
				const Characters::Spec rs = specOf(id, chars);
				if (seen.insert(rs.race + (rs.female ? "/f" : "/m")).second) { chars.prefetchRace(rs); }
			}
		}
		for (const uint64_t id : addOrder)
		{
			if (slots_.count(id)) { continue; }
			// the player's own squad is not held by the time budget: the camera, the selection and a script's keys act on it
			const bool mine = dec_.chars.at(id).cur.flags & kPlayer;
			const bool defer = kCharDefer.get() && !mine;
			const bool over = defer && added > 0 && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - addStart).count() > kAddBudgetMs.get();
			bool ready = true;
			// the squad skips the budget and the lookahead, not the decodes: its race body or textures still on the workers (queued above) keep it for a
			// later frame instead of blocking this one in the take (kenshi-k16.2.205.12.63: 16-60 ms of a join frame); a decode takes a few frames
			// one already shown (a look change rebuilds it) blocks as before, so it never blinks out
			const auto was = old.find(id);
			const bool arriving = worldReset || was == old.end() || was->second.index < 0;
			if (!defer) { ready = !kCharDefer.get() || !arriving || chars.texturesReady(specOf(id, chars)); }
			else if (ahead < kLookahead)
			{
				// the textures decode on the workers while earlier characters are built; one whose decode is still running waits for a later frame
				CPUSTAGE_ACC("sim.rebuild.ready");	// the lookahead's spec, prefetch and readiness (kenshi-k16.2.205.12.77)
				const Characters::Spec ps = specOf(id, chars);
				{ CPUSTAGE_ACC("sim.rebuild.prefetch"); chars.prefetch(ps); }
				{ CPUSTAGE_ACC("sim.rebuild.isready"); ready = chars.texturesReady(ps); }
				ahead++;
			}
			else { ready = false; }
			if (over || !ready)
			{
				rebuild_ = true;	// the next sync builds the rest
				if (mine) { squadWaiting_ = true; }
				continue;
			}
			added++;
			const Character& c = dec_.chars.at(id);
			Slot& sl = slots_[id];
			sl.version = c.look.version;
			chars.hull(chars.nameOf(dec_.str(c.look.race)), sl.hull);
			std::string err;
			const Characters::Spec s = specOf(id, chars);
			sl.severed = s.severed;
			// a race body still decoding (the squad is not held by the readiness check above): no error, the character is added on a later frame
			bool notReady = false;
			CPUSTAGE_ACC("sim.add.new");
			if (chars.add(s, err, mine && !arriving ? nullptr : &notReady)) { sl.index = (int)chars.size() - 1; CPUSTAGE_NOTE("add id %llu %s", (unsigned long long)id, s.race.c_str()); }
			else if (notReady) { slots_.erase(id); added--; rebuild_ = true; squadWaiting_ = squadWaiting_ || mine; }
			else { charError("build", tick_, id, err); }
		}
		// one step of a new race's first build for a character still waiting (its textures decoding, or past the budget), in the budget's leftover:
		// the frame's adds are done, so nobody is added later than without it, and its add finds the body, gear and contacts built (kenshi-k16.2.205.12.77)
		if (kCharDefer.get() && kCharWarm.get() && rebuild_ && std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - addStart).count() < kAddBudgetMs.get() + kWarmSlackMs.get())
		{
			CPUSTAGE_ACC("sim.add.warm");
			int seen = 0;
			for (const uint64_t id : addOrder)
			{
				if (slots_.count(id) || (dec_.chars.at(id).cur.flags & kPlayer)) { continue; }
				if (++seen > 2 * kLookahead) { break; }
				if (const Characters::Spec ws = specOf(id, chars); chars.warm(ws)) { CPUSTAGE_NOTE("warm id %llu %s", (unsigned long long)id, ws.race.c_str()); break; }
			}
		}
	}
	if (!selectedOnce_ && !order_.empty() && !squadWaiting_)
	{
		// the first snapshot selects the player's characters (the game starts with the squad selected)
		for (const uint64_t id : order_) { if (dec_.chars.at(id).cur.flags & kPlayer) { selected_.push_back(id); } }
		selectedOnce_ = true;
	}
	// one of ours always selected: when the selection loses its last (gone from the wire, left the squad), the first player character
	std::erase_if(selected_, [&](uint64_t id) { return !isPlayer(id); });
	for (size_t i = 0; i < order_.size() && selected_.empty() && !squadWaiting_ && !holdEmpty_; i++) { if (isPlayer(order_[i])) { selected_.push_back(order_[i]); } }
	// carriers first: a carried character's `carry me` needs its carrier's spec carrying already (Characters::add). A live character is also
	// rebuilt when the wire flips its drawn bit or swaps the weapon (kenshi-h2v.11.5.2): the weapon mesh moves hand <-> its sheathed attach and
	// the idle / upper-body records change with the hands (characters.cpp handsFit); the original has no sheathe clip, so the pose just changes.
	for (const uint64_t id : order_)
	{
		Slot& sl = slots_[id];
		const Characters::Spec carrier = specOf(id, chars);
		if (carrier.severed != sl.severed) { rebuild_ = true; }	// a sever (rare): the next sync rebuilds the set with the stump
		if (sl.index < 0 || sl.down != Up) { continue; }
		const Characters::Spec& cur = chars.live((size_t)sl.index);	// by reference: the spec is copied only when something below differs
		if (carrier.carry == sl.carry && carrier.drawn == cur.drawn && carrier.weapons == cur.weapons && carrier.tool == cur.tool) { continue; }
		Characters::Spec s = cur;
		s.carry = carrier.carry;
		s.drawn = carrier.drawn;
		s.weapons = carrier.weapons;
		s.tool = carrier.tool;
		std::string err;
		CPUSTAGE_ACC("sim.add.equip");	// a carry, drawn or weapon change: a rebuild (kenshi-k16.2.205.12.76)
		if (chars.replace((size_t)sl.index, s, err)) { sl.carry = s.carry; sl.action = 0; sl.actionStart = 0; }
		else { charError("build", tick_, id, err); }
	}
	// kenshi-k16.2.345: the other drawn standing bodies by last frame's drawn x, z (cell = one body diameter), for the nudge's body-body part
	nudgeGrid_.clear();
	nudge_.frames++; nudge_.nudgedSum += (uint64_t)nudge_.frameNudged; nudge_.maxFrameNudged = std::max(nudge_.maxFrameNudged, nudge_.frameNudged); nudge_.msSum += nudge_.frameMs; nudge_.maxFrameMs = std::max(nudge_.maxFrameMs, nudge_.frameMs); nudge_.frameMs = 0; nudge_.frameNudged = 0; nudge_.frameMax = 0;
	for (const uint64_t id : order_)
	{
		const Slot& sl = slots_[id];
		if (sl.drawn && nudgeOk(dec_.chars.at(id).cur, id)) { nudgeGrid_[nudgeKey((int)std::floor(sl.pos[0] / 0.9), (int)std::floor(sl.pos[2] / 0.9))].push_back(id); }
	}
	haveFocus_ = false;
	bool focusPlayer = false;
	for (const uint64_t id : order_)
	{
		const Character& c = dec_.chars.at(id);
		Slot& sl = slots_[id];
		// motion: interpolate the two samples around the render tick (kDelayTicks behind cur, so not the newest pair); state: the newest sample
		const Dynamic& b = c.cur;
		// back(k): the k-th sample back from the newest (0 = cur, 1 = prev, then past newest first), null past the oldest
		auto back = [&](size_t k) -> const Dynamic*
		{
			if (k == 0) { return &c.cur; }
			if (c.samples < 2) { return nullptr; }
			if (k == 1) { return &c.prev; }
			return k - 2 < c.past.size() ? &c.past[c.past.size() - 1 - (k - 2)] : nullptr;
		};
		size_t ka = c.samples > 1 ? 1 : 0;
		const Dynamic* ia = back(ka);
		const Dynamic* ib = &c.cur;
		while (tick_ < (double)ia->tick && back(ka + 1)) { ib = ia; ia = back(++ka); }
		const Dynamic* iz = back(ka + 1);	// the sample before the pair: its segment's velocity is the one the legs' speed blends from
		const double span = (double)ib->tick - (double)ia->tick;
		const float t = span > 0 ? (float)std::clamp((tick_ - (double)ia->tick) / span, 0.0, 1.0) : 1.0f;
		double p[3];
		for (int k = 0; k < 3; k++) { p[k] = ia->pos[k] + (ib->pos[k] - ia->pos[k]) * t; }
		float dy = ib->yaw - ia->yaw;
		dy -= 6.2831853f * std::floor((dy + 3.14159265f) / 6.2831853f);
		float yaw = ia->yaw + dy * t;
		if (id == local_.id) { predictYOff_ = 0; predict(c, p, yaw); }
		float byaw = yaw;	// the drawn body's yaw: the sim's, or the travel facing (kenshi-k16.2.237.10); sl.yaw (the camera follow's) stays the sim's
		const bool posed = (b.flags & (kSeated | kAsleep)) && b.poseClip;	// snapped to the furniture (kinspect bed_seat_pose.md): no interpolation, no ground fit
		// The drawn y is the sim's wire y (kenshi-k16.2.339, standing height stage 1: no terrain blend, no interior hull or indoors-flag branch; the
		// sim's ground ray is the one authority, retired/simview_ground_blend.cpp). Only without a navmesh y (yValid clear) the heightmap stands in.
		const double terrain = ground(p[0], p[2]);
		const double simY = p[1];
		int ylogRes = -9;	// the stand ray's result this frame (KYLOG only; -9: no ray)
		if (!(ib->flags & kYValid)) { p[1] = terrain; }
		else if (groundRay_ && !((b.flags & (kSeated | kAsleep)) && b.poseClip))
		{
			// kenshi-k16.2.344: the wire y is the sim's point; the body stands on the nearest DRAWN floor / stair / ramp under its x, z (WorldBuildings::groundDrawn, .382.2) or the
			// heightmap there, within [wire - kRayDown, wire + kRayUp]. No hit / not loaded: the wire y, counted.
			const double wy = p[1] - (id == local_.id ? predictYOff_ : 0.0);	// the driven character's prediction carries the wire offset: ray from the navmesh point
			const double rx = p[0], rz = p[2];
			const bool same = std::fabs(rx - sl.rayX) < 0.02 && std::fabs(rz - sl.rayZ) < 0.02 && std::fabs(wy - sl.rayWireY) < 0.02;
			const void* gen = nullptr;
			double ry = 0;
			int res;
			if (same && sl.rayRes != -2 && sl.rayRes != -1 && sl.rayGen == (groundGen_ ? groundGen_() : nullptr)) { res = sl.rayRes; ry = sl.rayY; rayStats_.cached++; }
			else
			{
				const auto t0 = std::chrono::steady_clock::now();
				res = groundRay_(rx, rz, wy, kRayUp, kRayDown, ry, &gen);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				rayStats_.casts++; rayStats_.ms += ms; rayStats_.frameCasts++; rayStats_.frameMs += ms;
				sl.rayX = rx; sl.rayZ = rz; sl.rayWireY = wy; sl.rayRes = res; sl.rayY = ry; sl.rayGen = gen;
			}
			res = solveGround(res, ry, rx, rz, terrain, wy, kRayUp, kDrawnDown);
			ylogRes = res;
			if (res >= 1) { rayStats_.hits++; if (res == 2) { rayStats_.faces++; } rayStats_.sumShift += std::fabs(ry - p[1]); rayStats_.maxShift = std::max(rayStats_.maxShift, std::fabs(ry - p[1])); p[1] = ry; }
			else
			{
				rayStats_.unloaded++;
				if (id == local_.id) { p[1] = wy + predictYOff_; }
				if (std::hypot(rx - sl.missX, rz - sl.missZ) > 3.0)
				{
					sl.missX = rx; sl.missZ = rz;
					evlog::add("standmiss", tick_, id, "standing ray: building mesh still loading at %.1f,%.1f wire y %.2f; drawn at the wire y", rx, rz, wy);
				}
			}
		}
		if (posed)
		{
			for (int k = 0; k < 3; k++) { p[k] = b.pose[k]; }
			yaw = b.pose[3];
		}
		// the sim's own velocity: the displacement between the two samples around the render tick over their span (world units/s)
		const double wvx = span > 0 ? (ib->pos[0] - ia->pos[0]) / span * kTickRate : 0.0;
		const double wvz = span > 0 ? (ib->pos[2] - ia->pos[2]) / span * kTickRate : 0.0;
		// kenshi-k16.2.237.10 (MINE: a half turn in 0.15 s; no turn speed in the data used): drive.facing = travel. While the character is the direct-drive
		// one (setDriven: the follow camera on a player, moving or not) the body eases to the travel direction (the prediction's velocity, the wire's when
		// the prediction is off or settled); still, it keeps its last facing. In combat (swings and blocks follow the sim heading), when the mode is
		// camera, or on leaving direct drive, it eases back to the sim yaw and releases. Nothing is posed here.
		{
			constexpr double kHalfTurn = 0.15, kMoving = 3.0;
			const bool driven = id == drivenId_ && !posed;
			const bool travel = driven && kDriveFacing.get() == "travel" && !(b.flags & kCombat);
			if (!travel && !sl.bodyOn) { sl.bodyOn = false; }
			else
			{
				if (!sl.bodyOn) { sl.bodyYaw = yaw; sl.bodyOn = true; }
				double want = travel ? sl.bodyYaw : (double)yaw;
				const double vx = local_.on && local_.id == id ? local_.vel[0] : wvx, vz = local_.on && local_.id == id ? local_.vel[1] : wvz;
				if (travel && std::hypot(vx, vz) > kMoving) { want = std::atan2(vx, vz); }
				double d = want - sl.bodyYaw;
				d -= 6.283185307179586 * std::floor((d + 3.141592653589793) / 6.283185307179586);
				const double step = 3.141592653589793 / kHalfTurn * dtSim_;
				sl.bodyYaw += std::clamp(d, -step, step);
				if (!travel && std::fabs(d) <= step) { sl.bodyOn = false; }
				else { byaw = (float)sl.bodyYaw; }
			}
		}
		// a sim jump (evlog simjump): the wire segment drawn now moves faster than twice the run speed, which the interpolation shows as a pop
		if (sl.drawn && !posed && span > 0 && ib->tick != sl.jumpTick)
		{
			knav::Gait g;
			g.run = true;
			const double run = knav::gaitSpeed(g), v = std::hypot(wvx, wvz);
			if (v > kSimJump.get() * run)
			{
				sl.jumpTick = ib->tick;
				const double before = iz && ia->tick > iz->tick ? std::hypot(ia->pos[0] - iz->pos[0], ia->pos[2] - iz->pos[2]) / (double)(ia->tick - iz->tick) * kTickRate : -1.0;
				evlog::add("simjump", tick_, id, "the sim moved %.1f u in %.0f ticks (%.0f u/s, the segment before %.0f, run %.0f) from %.1f,%.1f to %.1f,%.1f%s", v * span / kTickRate, span, v, before,
					run, ia->pos[0], ia->pos[2], ib->pos[0], ib->pos[2], ib->flags & kCombat ? " in combat" : "");
			}
		}
		// the sim's interpolated position this frame (what gameplay reads, and what the drawn may sit slightly off) and last frame's drawn position
		const double baseX = p[0], baseZ = p[2], prevX = sl.pos[0], prevZ = sl.pos[2];
		const bool wasDrawn = sl.drawn;
		// catch-up / display slop (user 2026-11-05: never teleport on a position error, run to it). kenshi-k16.2.74, user 2026-11-10: "The client
		// should 'ease into correction'" — a drawn position slightly off the sim is fine while gameplay reads the sim. The drawn keeps an offset from
		// the sim; when the base jumps (the render clock snapped, a sample hitched) the offset absorbs the whole jump so the drawn stays put, then eases
		// to 0 exponentially, speed-capped so the legs walk the correction, and a small offset rides uncorrected. A spawn / reset and any offset past
		// kTeleport snap. MINE: kSlopTol. anim.slop_tau = 0 is the old capped chase.
		constexpr double kCatch = 1.5, kCatchMin = 100.0, kTeleport = 400.0, kSlopTol = 1.0;
		const double slopTau = kSlopTau.get();
		double catchV[2] = { 0, 0 };	// the correction's own velocity this frame (the body's speed less the wire's), so the legs walk it too
		// The offset applies on every drawn frame, frozen ones too (kenshi-k16.2.180.1): with dtSim 0 gating the whole block, a pause dropped the
		// drawn to the bare base for its duration and the resume put base + off back in one frame, so every pause toggle rocked the characters
		// that carried an offset (the user: "your character teleports back and forth in place, rocking"). Only the easing and the chase need a step.
		if (sl.drawn && !posed && id != local_.id)
		{
			if (resetSnap_) { sl.off[0] = 0; sl.off[1] = 0; }	// a load: the new world's positions are not the old ones (paused too)
			if (slopTau > 0)
			{
				if (dtSim_ > 0 && !resetSnap_)
				{
					const double walk = std::max(kCatchMin, kCatch * std::hypot(wvx, wvz)) * dtSim_;	// the furthest the drawn walks a correction in one frame
					bool snapped = false;
					// a base jump: keep the drawn where it was and let the offset hold the whole error, then ease it
					if (std::hypot(baseX - sl.basePrev[0], baseZ - sl.basePrev[1]) > walk) { sl.off[0] = sl.pos[0] - baseX; sl.off[1] = sl.pos[2] - baseZ; }
					double on = std::hypot(sl.off[0], sl.off[1]);
					if (on >= kTeleport)
					{
						evlog::add("teleport", tick_, id, "snapped %.0f u from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f (past the %.0f u limit)", on, sl.pos[0], sl.pos[1], sl.pos[2], p[0], p[1], p[2], kTeleport);
						sl.off[0] = 0; sl.off[1] = 0; on = 0; snapped = true;
					}
					if (on > kSlopTol)
					{
						const double old0 = sl.off[0], old1 = sl.off[1];
						const double m = std::min(on * (1.0 - std::exp(-dtSim_ / slopTau)), walk);
						sl.off[0] -= sl.off[0] / on * m;
						sl.off[1] -= sl.off[1] / on * m;
						catchV[0] = (sl.off[0] - old0) / dtSim_;
						catchV[1] = (sl.off[1] - old1) / dtSim_;
						if (sl.catchStart < 0) { sl.catchStart = tick_; sl.catchMax = 0; }
						sl.catchMax = std::max(sl.catchMax, on);
					}
					else if (sl.catchStart >= 0)
					{
						// the episode ended (eased within the tolerance, or snapped): one line, only when it was more than the frame-to-frame jitter
						if (sl.catchMax > 4.0) { evlog::add("catchup", tick_, id, "eased to the sim for %.1f ticks, largest error %.1f u%s", tick_ - sl.catchStart, sl.catchMax, snapped ? " (ended by a teleport)" : ""); }
						sl.catchStart = -1;
					}
				}
				p[0] = baseX + sl.off[0];
				p[2] = baseZ + sl.off[1];
			}
			else if (dtSim_ > 0)
			{
				const double walk = std::max(kCatchMin, kCatch * std::hypot(wvx, wvz)) * dtSim_;	// the furthest the drawn walks a correction in one frame
				const double e = std::hypot(p[0] - sl.pos[0], p[2] - sl.pos[2]);	// the correction still owed (the old chase's error)
				if (e > walk && e < kTeleport)
				{
					const double ex = p[0] - sl.pos[0], ez = p[2] - sl.pos[2];
					p[0] = sl.pos[0] + ex / e * walk;
					p[2] = sl.pos[2] + ez / e * walk;
					catchV[0] = ex / e * walk / dtSim_ - wvx;
					catchV[1] = ez / e * walk / dtSim_ - wvz;
					if (sl.catchStart < 0) { sl.catchStart = tick_; sl.catchMax = 0; }
					sl.catchMax = std::max(sl.catchMax, e);
				}
				else
				{
					if (e >= kTeleport)
					{
						evlog::add("teleport", tick_, id, "snapped %.0f u from %.0f,%.0f,%.0f to %.0f,%.0f,%.0f (past the %.0f u catch-up limit)", e, sl.pos[0], sl.pos[1], sl.pos[2],
							p[0], p[1], p[2], kTeleport);
					}
					// a catch-up episode ended (caught up or snapped): one line, only when it was more than the frame-to-frame jitter (MINE: 4 u)
					if (sl.catchStart >= 0 && sl.catchMax > 4.0)
					{
						evlog::add("catchup", tick_, id, "chased the sim for %.1f ticks, largest error %.1f u%s", tick_ - sl.catchStart, sl.catchMax, e >= kTeleport ? " (ended by a teleport)" : "");
					}
					sl.catchStart = -1;
				}
			}
			else
			{
				p[0] = sl.pos[0]; p[2] = sl.pos[2];	// paused, no slop: hold the drawn where it is (the base is frozen anyway)
			}
		}
		// kenshi-k16.2.345: display depenetration. The drawn body is pushed out of the client collision (walls, props, furniture) and out of the other drawn
		// standing bodies by a small offset (cap kNudgeCap = one body radius), eased in and out (tau kNudgeTau), then the standing ray re-runs at the nudged
		// x, z. Display only: sl.sim, the pick, orders and the wire never see it. Off (target 0, so it eases out) for the seated / asleep / KO / dead /
		// prone / carried / caged / mounted, and for anyone with the combat flag: a fighter in contact is the animation's clip (a lunge, a grapple), and
		// the wire's combat bit is the only contact test the client has, so a body in combat stands where the sim says.
		if (kBodyNudge.get() && sl.drawn && !posed && depen_)
		{
			const auto nudgeT0 = std::chrono::steady_clock::now();
			constexpr double kNudgeCap = 0.45, kNudgeTau = 0.15, kBodyR = 0.45;
			double target[2] = { 0, 0 };
			if (nudgeOk(b, id))
			{
				double push[2];
				if (depen_(p[0], p[2], p[1], kBodyR, kNudgeCap, push, nullptr)) { target[0] += push[0]; target[1] += push[1]; }
				for (int cx = -1; cx <= 1; cx++)
				{
					for (int cz = -1; cz <= 1; cz++)
					{
						const auto it = nudgeGrid_.find(nudgeKey((int)std::floor(p[0] / 0.9) + cx, (int)std::floor(p[2] / 0.9) + cz));
						if (it == nudgeGrid_.end()) { continue; }
						for (const uint64_t oid : it->second)
						{
							if (oid == id) { continue; }
							const Slot& os = slots_.at(oid);
							const double dx = p[0] - os.pos[0], dz = p[2] - os.pos[2];
							const double d = std::hypot(dx, dz);
							if (d >= 2 * kBodyR || std::fabs(os.pos[1] - p[1]) > 1.0) { continue; }
							// each of the pair takes half of the overlap; exactly stacked: the lower id steps +x, the other -x
							const double half = 0.5 * (2 * kBodyR - d);
							if (d < 1e-4) { target[0] += id < oid ? half : -half; }
							else { target[0] += dx / d * half; target[1] += dz / d * half; }
						}
					}
				}
				const double tl = std::hypot(target[0], target[1]);
				if (tl > kNudgeCap) { target[0] *= kNudgeCap / tl; target[1] *= kNudgeCap / tl; }
			}
			if (dtSim_ > 0)
			{
				const double k = 1.0 - std::exp(-dtSim_ / kNudgeTau);
				sl.nudge[0] += (target[0] - sl.nudge[0]) * k;
				sl.nudge[1] += (target[1] - sl.nudge[1]) * k;
			}
			const double nl = std::hypot(sl.nudge[0], sl.nudge[1]);
			if (!sl.nudgeOn && nl > 0.1) { sl.nudgeOn = true; std::string why; double pu[2]; depen_(p[0] - sl.nudge[0], p[2] - sl.nudge[1], p[1], kBodyR, kNudgeCap, pu, &why); evlog::add("nudge", tick_, id, "nudged %.2f u (%.2f, %.2f) at %.1f,%.1f y %.1f: target %.2f (%s)", nl, sl.nudge[0], sl.nudge[1], p[0], p[2], p[1], std::hypot(target[0], target[1]), why.empty() ? "another body" : why.c_str()); }
			else if (sl.nudgeOn && nl < 0.02) { sl.nudgeOn = false; }
			if (nl > 0.005)
			{
				p[0] += sl.nudge[0]; p[2] += sl.nudge[1];
				nudge_.frameNudged++; nudge_.frameMax = std::max(nudge_.frameMax, nl); nudge_.maxOffset = std::max(nudge_.maxOffset, nl);
				if ((ib->flags & kYValid) && groundRay_)
				{
					const double wy = simY - (id == local_.id ? predictYOff_ : 0.0);
					double ry = 0; const void* gen = nullptr;
					int res = groundRay_(p[0], p[2], wy, 1.5, 4.5, ry, &gen);
					res = solveGround(res, ry, p[0], p[2], ground(p[0], p[2]), wy, 1.5, kDrawnDown);
					if (res >= 1) { p[1] = ry; }
				}
			}
			nudge_.frameMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - nudgeT0).count();
		}
		else { sl.nudge[0] = sl.nudge[1] = 0; }
		// kenshi-k16.2.395.1 (k16.2.395; user 2026-11-19: "Walking up the stairs is a jumpy jittery mess, because every new stair rung makes the
		// character teleport to a new height. It needs to interpolate between current height and new requested height."). The drawn body's y eases
		// toward the stand height p[1] here (after the standing ray AND the nudge's re-ray), render side only: the sim's y (sl.sim / simPos), the
		// standing ray and its result stay exact, and the floor view (ff::FloorFollow) reads the WIRE y (kenshi-k16.2.236.8.4.3), never this. A tread
		// here is up to the flight's own rise (the Storm House's treads step the stand ray 3.9 u), so a short time constant (anim.stand_ease_tau)
		// turns a tread into a ramp of a few frames; past kStandSnap (8 u, twice a 3.9 u tread) the target is a fall, a ledge or a
		// spawn, not a tread, so it snaps (a delayed drop would read as floating). A paused frame holds it; posed
		// (seated / asleep) and the first drawn frame snap. The picks, the selection ring and the name label read this, as they draw it (sl.pos).
		// Foot IK (anim.foot_terrain) is terrain-only: it reads this smoothed body y (c.spec.pos) and sets the soles on the terrain's own height, so
		// a slope's lag (under a tenth of a unit at a run) is the leg reach's to take, not a fight with it.
		const double standTargetY = p[1];
		if (!posed)	// every drawn standing body (sl.drawn is the drawn WEAPON: not this); seated / asleep snap as the pose does
		{
			// the step that is a drop, not a tread (a tread is 3.9 u; twice that) and the ease's rate cap, over which only a teleport goes
			constexpr double kStandSnap = 8.0, kStandMax = 150.0;	// u, u/s
			const double tau = kStandEaseTau.get();
			if (!sl.drawYOn || tau <= 0) { sl.drawY = standTargetY; sl.drawYOn = true; }
			else if (dtSim_ > 0)	// a paused frame holds the drawn y where it is
			{
				const double d = standTargetY - sl.drawY;
				if (std::fabs(d) > kStandSnap) { sl.drawY = standTargetY; }
				else
				{
					const double step = std::min(std::fabs(d) * (1.0 - std::exp(-dtSim_ / tau)), kStandMax * dtSim_);
					sl.drawY += d > 0 ? step : -step;
				}
			}
			p[1] = sl.drawY;
		}
		else { sl.drawY = standTargetY; sl.drawYOn = false; }
#ifdef KCLIENT_DEV_TOOLS
		// kenshi-k16.2.395.1 (the stairs diagnosis): the log sits here, after the standing ray, the nudge's re-ray and the ease, so `drawn` is the y the
		// body is about to draw with and the appended `target` is the stand height it eased toward. Columns 1-9 are as kenshi-kyat.2 defined them
		// (id tick x z sim terrain drawn ray-result bilinear terrain).
		static const bool ylog = std::getenv("KYLOG") != nullptr;	// KYLOG=1: per character per frame: id tick x z sim terrain drawn ray-result (kenshi-kyat.2, k16.2.382.1)
		if (ylog) { fprintf(stderr, "YLOG %llu %.0f %.2f %.2f %.2f %.2f %.2f %d %.2f %.2f\n", (unsigned long long)id, tick_, p[0], p[2], simY, terrain, p[1], ylogRes, ylogAlt ? ylogAlt(p[0], p[2]) : 0.0, standTargetY); }
#endif
		sl.catchV[0] = catchV[0]; sl.catchV[1] = catchV[1];
		sl.basePrev[0] = baseX; sl.basePrev[1] = baseZ;	// the next frame's base-jump test
		sl.sim[0] = baseX; sl.sim[1] = (ib->flags & kYValid) ? simY : standTargetY; sl.sim[2] = baseZ;	// gameplay reads the sim, never the display offset: its y is the wire's, not the stand ray's (the floor latch saw a standing character jump 2.8 u up the flight onto the top tread, k16.2.236.8.4.3), and never the eased drawn y (k16.2.395.1: standTargetY is the stand y before the ease)
		// the legs take the sim's own velocity, in the body frame (forward (sin, cos), lateral (cos, -sin), as the sim's), plus the catch-up's (user:
		// nothing repositions without walking). No smoothing: the old drawn-motion derivative tailed off exponentially and froze the stride after the
		// sim stopped (docs/CLIENT_GOALS.md "Locomotion fidelity" 2). Frozen (dt 0): the last value holds.
		// anim.vel_blend (kenshi-k16.1.3): the legs' part of it blends from the previous segment's velocity by the same t. The pair's alone is constant
		// over a tick and steps at each sample (10 u/s a tick while running up), and the band sigmoid turned each step near a crossover into a clip
		// pop. Half a tick of lag; a stop still reads 0 one tick after the sim's (no tail). The catch-up and the stop steer's vSim keep the pair's.
		if (sl.drawn && dtSim_ > 0)
		{
			const double spanZ = iz ? (double)ia->tick - (double)iz->tick : 0.0;
			const bool blendV = kVelBlend.get() && spanZ > 0;
			const double vx = (blendV ? wvx * t + (ia->pos[0] - iz->pos[0]) / spanZ * kTickRate * (1.0 - t) : wvx) + catchV[0];
			const double vz = (blendV ? wvz * t + (ia->pos[2] - iz->pos[2]) / spanZ * kTickRate * (1.0 - t) : wvz) + catchV[1];
			const float fwd = (float)(vx * std::sin(byaw) + vz * std::cos(byaw)), lat = (float)(vx * std::cos(byaw) - vz * std::sin(byaw));
			const float raw[2] = { fwd, lat };
			for (int a = 0; a < 2; a++)
			{
				if (sl.speedHead < 0) { std::fill(std::begin(sl.speedWin[a]), std::end(sl.speedWin[a]), raw[a]); }
				sl.speedWin[a][std::max(sl.speedHead, 0)] = raw[a];
			}
			sl.speedHead = (std::max(sl.speedHead, 0) + 1) % 8;
			// kenshi-k16.2.237.1 (MINE): the character the player drives takes the predicted speed as it is, unfiltered: the window of 8 holds a start at
			// idle until 5 samples are in (a stop likewise) while the predicted body already moves, which showed as a slide on the idle pose.
			const bool driven = id == local_.id && local_.on;
			sl.vFwd = kSpeedFilter.get() && !driven ? trimmed_mean8(sl.speedWin[0]) : fwd;
			sl.vLat = kSpeedFilter.get() && !driven ? trimmed_mean8(sl.speedWin[1]) : lat;
			// ds/stop-fix: the sim's own forward speed without the catch-up's: a catch-up step's end reads as a big decel and fired the stop steer mid-run
			sl.vSim = (float)(wvx * std::sin(yaw) + wvz * std::cos(yaw));
#ifdef KCLIENT_DEV_TOOLS
			// kenshi-qaud.1: KSTEPLOG=1: per frame the sample pair behind the sim speed (what arms the stop steer), the route version, the drawn and the sim's position
			static const bool steplog = std::getenv("KSTEPLOG") != nullptr;
			if (steplog)
			{
				fprintf(stderr, "STEPLOG %llu render %.2f pair %llu..%llu iz %llu span %.0f vSim %.2f wire %.2f vFwd %.2f route %u drawn %.1f %.1f sim %.1f %.1f\n", (unsigned long long)id, tick_,
					(unsigned long long)ia->tick, (unsigned long long)ib->tick, iz ? (unsigned long long)iz->tick : 0ULL, span, (double)sl.vSim, (double)b.fwd, (double)sl.vFwd, c.route.version, p[0], p[2], ib->pos[0], ib->pos[2]);
			}
#endif
		}
		else if (!sl.drawn)	// the first frame: the sim's own fwd / lat (a teleport or load snap reads as one huge tick, clamped to 2x run speed)
		{
			knav::Gait g;
			g.run = true;
			const float cap = 2.0f * (float)knav::gaitSpeed(g);
			sl.vFwd = std::clamp(ib->fwd, -cap, cap);
			sl.vLat = std::clamp(ib->lat, -cap, cap);
			sl.vSim = sl.vFwd;
		}
		// drawjump (kenshi-k16.2.74): a drawn character that moved further in one frame than its sim speed allows is a pop. K = 2: the catch-up
		// draws at most 1.5x the sim's own speed, so 2x is beyond the design and reads as a slip; the 1.5 u floor is just under one frame of the old
		// catch-up's 100 u/s minimum at 60 fps (1.67 u) and above the display slop's first eased step (~0.5 u), so it counts the old chase and not
		// the ease. Posed (seated / asleep) snaps are state, not sim pops.
		if (wasDrawn && !posed && dtSim_ > 0)
		{
			constexpr double kDrawJump = 2.0, kDrawJumpMin = 1.5;
			const double dm = std::hypot(p[0] - prevX, p[2] - prevZ), simV = std::hypot(wvx, wvz);
			if (dm > kDrawJumpMin && dm > kDrawJump * simV * dtSim_)
			{
				evlog::add("drawjump", tick_, id, "drew %.1f u in one frame (%.0f u/s, the sim's %.0f u/s)%s%s from %.1f,%.1f to %.1f,%.1f", dm, dm / dtSim_, simV,
					clockSnapped_ ? ", the clock snapped this frame" : "", slopTau > 0 ? ", slop" : "", prevX, prevZ, p[0], p[2]);
			}
		}
		sl.drawn = true;
		if (!haveFocus_ || (!focusPlayer && (b.flags & kPlayer)))
		{
			for (int k = 0; k < 3; k++) { ground_[k] = p[k]; }
			haveFocus_ = true;
			focusPlayer = (b.flags & kPlayer) != 0;
		}
		for (int k = 0; k < 3; k++) { sl.pos[k] = p[k]; }
		sl.yaw = yaw;
		if (sl.index < 0) { continue; }
		const bool fresh = !sl.seen;	// its first drawn frame: there is no pose to fall from (one that enters already down is snapped)
		sl.seen = true;
		// KO / dead: the game ragdolls (PhysX), we have no physics; MINE: the skeleton's face-down `crawl idle down`, frozen, blended into over the fall
		// (Characters::fall, `anim.fall_blend`) and left with `standing up 3` once the KO clears (Characters::rise). Prone and moving: `crawl loop`
		// (the game's `crawling move`, locomotion category 5, is not modelled). The character is rebuilt on a change.
		// seated / asleep: the pose tail's clip at its snap point and yaw; an older server: `sitting chair` / `sleepinbed` at the sim's position and yaw.
		// carried (CHAR_DYNAMIC carriedBy): `carry me` on the carrier's shoulder (Characters::carryPlace), once the carrier is drawn carrying
		const auto carrier = b.carriedBy ? slots_.find(b.carriedBy) : slots_.end();
		const bool carried = carrier != slots_.end() && carrier->second.index >= 0 && carrier->second.carry != 0;
		// a KO'd character put into a bed (kenshi-k16.2.224.1: KO + asleep + the pose tail) lies in the bed, not as a ragdoll under the mattress; dead stays Lying
		const Down down = carried ? Carried : b.flags & kDead ? Lying : (b.flags & kKO) && (b.flags & kAsleep) && b.poseClip ? Asleep : b.flags & kKO ? Lying : b.flags & kSeated ? Seated : b.flags & kAsleep ? Asleep
			: !(b.flags & kProne) ? Up : b.fwd > 0.5f ? Crawling : Lying;
		const uint32_t poseClip = down == Seated || down == Asleep ? b.poseClip : 0;
		bool forced = false;	// a get-up cut by the sim (down again, carried, seated): the state rebuilds even when the down state's name is unchanged
		if (sl.rising && down != Up) { sl.rising = false; forced = true; }
		// a state whose rebuild failed: the old pose stays and there is no retry while the wire still reads it (the error prints once)
		if (sl.failed && (down != sl.failedDown || poseClip != sl.failedClip)) { sl.failed = false; }
		if (!sl.rising && (forced || (!sl.failed && (down != sl.down || poseClip != sl.poseClip))))
		{
			Characters::Spec s = specOf(id, chars);
			s.carry = 0;
			if (down == Carried) { s.carriedBy = carrier->second.index; s.drawn = false; s.weapons.clear(); }	// `carry me` deletes weapons (its ANIMATION `delete weapons`): none drawn, sheath or hand
			else if (down != Up)
			{
				s.loco = false;
				s.clip = down == Lying ? "crawl idle down" : down == Crawling ? "crawl loop" : poseClip ? dec_.str(poseClip) : down == Seated ? "sitting chair" : "sleepinbed";
				s.speed = down == Lying ? 0.0f : 1.0f;
				s.drawn = false;	// MINE: sheathed (the drawn weapon would stand up out of the pose's hand)
				s.posed = down == Seated || down == Asleep;	// furniture pose: no body-posture layers (Characters::Spec::posed)
				if (down == Seated || down == Asleep) { s.weapons.clear(); }	// the furniture's `delete weapons` (kinspect bed_seat_pose.md 4)
			}
			std::string err;
			CPUSTAGE_ACC("sim.add.pose");	// a down / seated / carried state change: fall, rise or a rebuild (kenshi-k16.2.205.12.76)
			bool ok = false;
			// the get-up only from the lying pose we built: a failed lying rebuild leaves a live spec while sl.down may say Lying (older builds
			// commit it), and rise() would hand locomote() its record-less layer (plans/layer-null); a live character rebuilds instead
			const bool lying = !chars.live((size_t)sl.index).loco;
			if (down == Up && sl.down == Lying && lying) { ok = chars.rise((size_t)sl.index, err); sl.rising = ok; }	// the KO clears: "standing up 3", once
			// MINE (animal down, no stand-up clip): the get-up from the ragdoll's pose into the live spec, blended; the character stays live throughout
			else if (down == Up && sl.down == Lying && sl.ragdollHeld)
			{
				Characters::Spec r = specOf(id, chars);
				r.carry = 0;
				ok = chars.riseLive((size_t)sl.index, r, err);
				if (ok) { charError("fallback", tick_, id, "the ragdoll's pose blends into the animation (no stand-up clip)"); }
			}
			else if (down == Lying && sl.down != Lying && !fresh) { ok = chars.fall((size_t)sl.index, s, err); }	// the fall: a blend into the lying pose
			if (!ok && !sl.rising) { ok = chars.replace((size_t)sl.index, s, err); sl.rising = false; }
			if (ok)
			{
				sl.carry = 0; sl.action = 0; sl.actionStart = 0;
				sl.failed = false; sl.ragdollHeld = false;
				sl.down = sl.rising ? Lying : down;	// the get-up keeps the lying state until it hands over (the root stays off locomotion)
			}
			else
			{
				// the old pose stays: sl.down is not committed for a state that was never built (its Lying would turn the next Up into a get-up
				// on a live character: the record-less layer BUG in locomote, plans/layer-null)
				sl.failed = true;
				sl.failedDown = down;
				sl.failedClip = poseClip;
				charError("pose", tick_, id, err);	// once: the old pose stays until the state changes again
			}
			sl.poseClip = poseClip;
		}
		Characters::Spec& s = chars.live((size_t)sl.index);
		for (int k = 0; k < 3; k++) { s.pos[k] = p[k]; }
		s.yaw = byaw;
		if (sl.rising)
		{
			// the get-up plays once on the lying character; at its end — or at once if the sim moves the character, the original's ragdoll yielding to
			// the walk — the character is rebuilt from the live spec. The rebuild is the hand-off that keeps every pose layer out of locomote() (a
			// loco flip would hand it the get-up clip, the record-less layer BUG, guard 729661b1).
			// MINE: a stand-up moves the body (the sim's spot is not the ragdoll's), which reads as a walk, so the blend's cross-fade is never cut:
			// the ragdoll's last pose must fade into the clip, not snap. Past the blend the walk takes the character as before.
			const bool moved = std::hypot(sl.vFwd, sl.vLat) > 5.0;
			if ((!moved || chars.risingBlend((size_t)sl.index)) && chars.rising((size_t)sl.index)) { continue; }
			Characters::Spec r = specOf(id, chars);
			r.carry = 0;
			std::string err;
			CPUSTAGE_ACC("sim.add.rise");
			if (!chars.replace((size_t)sl.index, r, err)) { charError("build", tick_, id, err); continue; }
			Characters::Spec& rs = chars.live((size_t)sl.index);
			for (int k = 0; k < 3; k++) { rs.pos[k] = p[k]; }
			rs.yaw = byaw;
			sl.rising = false;
			sl.down = Up;
			sl.poseClip = 0;
			sl.carry = 0;
			sl.action = 0;
			sl.actionStart = 0;
		}
		if (sl.down != Up) { continue; }
		s.move = sl.vFwd;	// the samples' own velocity (sl.vFwd), not the wire's fwd / lat fields: the displacement carries pushes
		s.moveSim = sl.vSim;	// the same without the catch-up (ds/stop-fix: the stop steer's decel)
		s.wireMove = b.fwd;	// the wire's own speed (its ramp); the stop steer reads it for the route end's dead stop (ds/run-stop-2)
		s.routeEnd = c.route.points.empty() ? -1.0f : (float)std::hypot(b.pos[0] - c.route.points.back()[0], b.pos[2] - c.route.points.back()[2]);
		s.wireGap = (float)std::hypot(p[0] - b.pos[0], p[2] - b.pos[2]);	// what the drawn body still has to walk to the newest sample
		s.strafe = sl.vLat;
		s.combat = (b.flags & kCombat) != 0;
		// kenshi-k16.2.237.8 (MINE): a direct-drive strafe (wire bits 22 / 23) at any combat state takes the `strafe lower` split (the lower body by the lateral
		// speed, as in combat; the only side-step record the data has, none other for walking out of combat: newstrafe / strafe 2 / sidestep 2 are bare clips
		// with no ANIMATION record), the forward walk / run clips carrying the forward part of the move. The user 2026-11-18: no strafe clip out of combat.
		s.strafeDrive = (b.flags & (kStrafeLeft | kStrafeRight)) != 0;
		if (sl.bodyOn && kDriveFacing.get() == "travel" && id == drivenId_ && !s.combat)
		{
			// kenshi-k16.2.237.10 (MINE): travel facing turns the body its way, so the forward walk / run at the travel speed (no strafe split, no backward clip)
			s.move = std::hypot(sl.vFwd, sl.vLat);
			s.strafe = 0;
			s.strafeDrive = false;
		}
		s.stealth = (b.flags & kStealth) != 0;
		s.legs[0] = b.legs[0];
		s.legs[1] = b.legs[1];
		// a new action (name or start tick) starts at its age. The wire's action going back to 0 ends ours, looped or not: a swing the sim cut mid-way
		// (the wire has no action but the clip plays on) left the character in a full-body swing while the body kept moving — the 2026-11-06 glide.
		if (b.action && (b.action != sl.action || b.actionStart != sl.actionStart))
		{
			sl.action = b.action;
			sl.actionStart = b.actionStart;
			sl.loop = b.actionFlags & 1;
			std::string err;
			const float age = (float)std::max(0.0, (tick_ - (double)b.actionStart) / kTickRate);
			if (!chars.action((size_t)sl.index, dec_.str(b.action), b.actionSpeed, age, sl.loop, err)) { charError("action", tick_, id, "action " + dec_.str(b.action) + ": " + err); }
		}
		else if (!b.action && sl.action)
		{
			sl.action = 0;
			sl.loop = false;
			chars.endAction((size_t)sl.index);	// its target goes to 0: it blends out at the layer rate
		}
		// anim.action_root (fix 2): the action's age on the wire's own clock, the phase source the drawn clip root replays at (Characters::actionPath)
		s.actionTime = b.action ? (float)std::max(0.0, (tick_ - (double)b.actionStart) / kTickRate) : 0.0f;
		s.actionSpeed = b.actionSpeed;
		s.aimYaw = b.aimYaw;
		s.hasAim = b.hasAim;
		// The run file's three case lines (kenshi-k16.2.264.1, the user's 2026-11-15 walk into Outpost s-IV: "running in place ... movement spurts
		// every second or two", the dip under the deck, then the body gone indoors with its tag still drawn; the run file had none of it). One line
		// each, rate-limited per character to one every 5 s, with the numbers the report needs: the legs' speed against the wire's own movement, the
		// drawn y against the wire's, and why the body is out of the frame's set. Every threshold is a cfg key so a check can force one (k16.2.264.1).
		constexpr double kCaseGap = 5.0 * kTickRate;	// ticks between that character's lines of one kind
		if (b.tick != sl.wireTick)	// the wire's newest sample (its own tick: the render clock stalling cannot fake this)
		{
			if (sl.wireTick == 0 || std::fabs(b.pos[0] - sl.wireLast[0]) + std::fabs(b.pos[2] - sl.wireLast[1]) > 0.05) { sl.wireMoveTick = b.tick; }
			sl.wireLast[0] = b.pos[0];
			sl.wireLast[1] = b.pos[2];
			sl.wireTick = b.tick;
		}
		const double still = (double)b.tick - (double)sl.wireMoveTick;
		// 1. The walk animation plays while the wire's position stands still: the legs' own input (spec.move = the drawn speed, above the locomotion's
		//    0.5 u/s rest cutoff) says walking while the sim has not moved the character for kWireStillTicks. A sim-side stall ("sim delayed?") reads so.
		if (sl.drawn && sl.down == Up && !posed && dtAnim_ > 0 && std::fabs(s.move) >= 0.5f && still >= kWireStillTicks.get() && tick_ - (double)sl.stallLogTick >= kCaseGap)
		{
			sl.stallLogTick = (uint64_t)tick_;
			evlog::add("walkstall", tick_, id, "walking at %.1f u/s while the wire's position has not moved for %.1f s (%.0f ticks; newest sample %llu at %.1f,%.1f,%.1f, drawn at %.1f,%.1f)",
				(double)s.move, still / kTickRate, still, (unsigned long long)b.tick, b.pos[0], b.pos[1], b.pos[2], p[0], p[2]);
		}
		// 2. The drawn y off the wire's y: the drawn y is the wire's y, so this fires only for a character drawn off it (the interpolation, the easing offset).
		const double dyOff = p[1] - simY;
		if (sl.drawn && !posed && dtSim_ > 0 && (b.flags & kYValid) && std::fabs(dyOff) > kDrawYOff.get() && tick_ - (double)sl.offLogTick >= kCaseGap)
		{
			sl.offLogTick = (uint64_t)tick_;
			evlog::add("drawnoff", tick_, id, "drawn y %.1f is %+.1f u off the wire's %.1f (terrain %.1f)", p[1], dyOff, simY, terrain);
		}
		// 3. Drawn with no body: the character is in the scene (its feet place, so its tag and marker draw) but the last prepare() left its body out of
		//    the set; the reason is that pass's draw state (one frame old). A changed reason is named at once, a held one every 5 s.
		const bool known = (size_t)sl.index < chars.drawStates();
		const Characters::DrawState ds = known ? chars.drawState((size_t)sl.index) : Characters::kBodyDrawn;
		if (known && ds != Characters::kBodyDrawn)
		{
			// two frames of the same reason before it is named: the draw state is one prepare behind, and an interest rebuild renumbers the indices
			if (sl.nobodyPending != (uint8_t)ds) { sl.nobodyPending = (uint8_t)ds; sl.nobodyPendingCount = 0; }
			if (sl.nobodyPendingCount < 255) { sl.nobodyPendingCount++; }
		}
		if (known && ds != Characters::kBodyDrawn && sl.nobodyPendingCount >= 2 && (sl.nobodyState != (uint8_t)ds || tick_ - (double)sl.nobodyLogTick >= kCaseGap))
		{
			sl.nobodyState = (uint8_t)ds;
			sl.nobodyLogTick = (uint64_t)tick_;
			evlog::add("nobody", tick_, id, "no body drawn: %s (feet %.1f,%.1f,%.1f, character %d of %d)", Characters::drawStateName(ds), p[0], p[1], p[2], sl.index,
				(int)chars.drawStates());	// %d: MinGW checks printf formats against msvcrt, which has no %zu
		}
		else if (ds == Characters::kBodyDrawn) { sl.nobodyState = (uint8_t)ds; sl.nobodyPendingCount = 0; }
	}
	answers();
	rollOk_.clear();
	std::set<std::pair<uint64_t, uint64_t>> shots;	// (shooter, target) of the landed shots of this batch: their event 1 is a projectile hit
	for (const Event& e : dec_.events) { if (e.kind == 49 && e.result == 0) { shots.insert({ e.a, e.b }); } if (e.kind == 20 && e.result) { rollOk_.insert(e.b); } }
	cueWorld(chars);
	for (const Event& e : dec_.events)
	{
		cueSounds(chars, e, shots);
		if (logHits && e.kind == 1)	// script `log hits`: the sim's own tick and the final (post-armour) damage values
		{
			printf("hit tick %llu victim %llu attacker %llu result %u damage %.1f %.1f %.1f %.1f at %.1f %.1f\n", (unsigned long long)e.tick, (unsigned long long)e.a,
				(unsigned long long)e.b, e.result, e.damage[0], e.damage[1], e.damage[2], e.damage[3], e.pos[0], e.pos[2]);
		}
		// a landed hit: the game's splash (140793b20 from receiveHit): direction = the hit direction turned by a random -50..50 deg, offset along it by
		// 20 x rand (UNSURE factor), size lerp(min(1, (cut + blunt + pierce) / 100), 0.05, 0.5), the victim race's blood colour. Our rand: a hash of
		// tick and victim, so a replay paints the same splash.
		if (e.kind == 2 || e.kind == 16 || e.kind == 17)
		{
			// MedicalSystem's ground blood (kinspect bleed_pool.md): 2 a drop while walking, size min(0.1 x rate, 0.5), a random direction; 16 the growing
			// pool under a KO'd or down character, size g; 17 a smear while crawling or dragged, size 0.2, alpha from the event. Tinted by the pool's
			// character, else the nearest one (drops and smears carry no id).
			const Slot* near = nullptr;
			if (e.kind == 16)
			{
				const auto it = slots_.find(e.a);
				if (it != slots_.end() && it->second.index >= 0) { near = &it->second; }
			} else {
				double best = 15 * 15;
				for (const auto& [id, s] : slots_)
				{
					const double d2 = std::pow(s.pos[0] - e.pos[0], 2) + std::pow(s.pos[2] - e.pos[2], 2);
					if (s.index >= 0 && d2 < best) { best = d2; near = &s; }
				}
			}
			const float* col = near ? chars.bloodColour((size_t)near->index) : nullptr;
			if (!col || e.amount <= 0) { continue; }
			Splash s = { e.pos[0], e.pos[2], 0, 0, {}, e.kind == 2 ? kDrop : e.kind == 16 ? kPool : kSmear, e.a };
			memcpy(s.colour, col, sizeof s.colour);
			if (e.kind == 2)
			{
				uint64_t h = (e.tick * 0x9e3779b97f4a7c15ull) ^ (uint64_t)(int64_t)(e.pos[0] * 64) ^ ((uint64_t)(int64_t)(e.pos[2] * 64) << 20);
				h ^= h >> 31; h *= 0x94d049bb133111ebull; h ^= h >> 29;
				s.size = std::min(0.1f * e.amount, 0.5f);
				s.dirRad = (float)((double)(h >> 11) / 9007199254740992.0 * 6.283185307179586);
			} else if (e.kind == 16) {
				s.size = std::min(e.amount, 2.0f);
			} else {
				s.size = 0.2f;
				s.dirRad = (float)(e.dir * (6.283185307179586 / 256));
				s.colour[3] *= std::clamp(e.amount, 0.0f, 1.0f);
			}
			splashes_.push_back(s);
			continue;
		}
		const auto sl = slots_.find(e.a);
		if (e.kind != 1 || e.result != 0 || sl == slots_.end() || sl->second.index < 0) { continue; }
		const float* col = chars.bloodColour((size_t)sl->second.index);
		if (!col) { continue; }
		uint64_t h = (e.tick * 0x9e3779b97f4a7c15ull) ^ (e.a * 0xbf58476d1ce4e5b9ull);
		auto rand01 = [&h]() { h ^= h >> 31; h *= 0x94d049bb133111ebull; h ^= h >> 29; return (double)(h >> 11) / 9007199254740992.0; };
		const double dir = e.dir * (6.283185307179586 / 256) + (rand01() - 0.5) * (100 * M_PI / 180);
		const double off = 20 * rand01();
		Splash s = { e.pos[0] + std::cos(dir) * off, e.pos[2] + std::sin(dir) * off, 0.05f + 0.45f * std::min(1.0f, (e.damage[0] + e.damage[1] + e.damage[2]) / 100), (float)dir, {} };
		memcpy(s.colour, col, sizeof s.colour);
		splashes_.push_back(s);
	}
	dec_.events.clear();	// MINE, first cut: no hit sparks
	resetSnap_ = false;	// the load's offset snap was applied to every drawn character above
	synced_ = true;	// a reset seen from now on is a load, not the connection's own
}

// Client frames are tiny (an order is < 2.1 KiB at 256 ids) next to the socket buffer, so a short write is treated as fatal rather than queued.
bool SimView::send(const std::vector<uint8_t>& out, const char* what)
{
	if (fd_ == kutil::net::kNoSocket) { return false; }
	// HELLO is always the first frame (SIMLINK: "expected HELLO as the first frame"): the server's own HELLO is not decoded yet, so an order
	// (a playtest load, a scenario frame) is held here whole and goes out right after ours, in order (kenshi-k16.2.406.1).
	if (mode_ == Live && !helloSent_) { preHello_.insert(preHello_.end(), out.begin(), out.end()); return true; }
	return sendNow(out, what);
}

bool SimView::sendNow(const std::vector<uint8_t>& out, const char* what)
{
	if (fd_ == kutil::net::kNoSocket) { return false; }
	if (!kutil::net::tcpSendAll(fd_, out.data(), out.size())) { fail(std::string("send ") + what + " failed"); return false; }
	lastSend_ = std::chrono::steady_clock::now();
	return true;
}

void SimView::answers()
{
	const auto say = [&](std::string r) { replies_.push_back(r); reply_ = std::move(r); };
	while (!popups_.empty() && tick_ - popups_.front().tick > 12 * kTickRate) { popups_.pop_front(); }
	std::erase_if(speech_, [&](const auto& kv) { return tick_ - kv.second.tick > kv.second.duration * kTickRate || !dec_.chars.count(kv.first); });
	for (const Event& e : dec_.events)
	{
		if (e.kind == 8 && isPlayer(e.a)) { reply_ = "drive blocked"; continue; }
		if (e.kind == 13)
		{
			static const char* const kResult[] = { "done", "refused on arrival", "unreachable", "target gone", "actor down", "cancelled", "partial", "full", "too hard" };
			if (e.result == 8)	// too hard (SIMLINK event 13 result 8; approved kenshi-k16.6.376, implemented k16.6.377): a lock pick reached the lock and its
			{					// skill is below the floor, so no pick ran. Only the cage pick still ends that way (action 18): since ksim 7fd41c98 (k16.6.501) a
								// door or container pick (3) below the floor is unplannable, so the sim drops the order and sends no event 13 at all (rwh.178);
								// a shackle pick (15) is always above the floor (its lock level is fixed at 20, ksim lock_tasks.cpp kShackleLockLevel).
				// The player's line is OURS: the original has no text for it (LOCKS_EQUIP_STOCK_STATIC D-lockpick-zero); the host's message box shows it.
				if (isPlayer(e.a) && e.dir == 18) { lockTooHard_++; }
				if (e.dir < 15 || e.dir > 17) { printf("interact done: %llu on %llu: action %u result 8 too hard\n", (unsigned long long)e.a, (unsigned long long)e.b, (unsigned)e.dir); }	// the shackle orders print their own line below
			}
			if (e.dir >= 15 && e.dir <= 17)	// the shackle orders (SIMLINK order 60 "Shackle orders"): the result as the player reads it; texts OURS
			{
				static const char* const kDone[] = { "Shackles unlocked", "Shackles cut", "Shackles broken!" };
				static const char* const kRefused[] = { "The shackles would not open", "No tool to cut the shackles with", "Not strong enough to break the shackles" };
				static const char* const kOther[] = { "", "", "Could not reach the shackles", "The shackles are gone", "Could not finish: down", "Stopped" };
				const char* text = e.result == 0 ? kDone[e.dir - 15] : e.result == 1 ? kRefused[e.dir - 15] : e.result == 8 ? kResult[8] : e.result < 6 ? kOther[e.result] : "?";
				say(text);
				printf("shackles: %llu on %llu: action %u result %u: %s\n", (unsigned long long)e.a, (unsigned long long)e.b, (unsigned)e.dir, (unsigned)e.result, text);
				continue;
			}
			say("interact " + std::to_string(e.dir) + " on " + std::to_string(e.b) + ": " + (e.result < 9 ? kResult[e.result] : "?"));
			continue;
		}
		if (e.kind == 14)
		{
			foreign_.insert(e.b);
			// the event itself, whoever the thief is: the HUD reply below (player thieves only) was the one trace (k16.2.443)
			printf("theft event: %llu took from %llu (owner %s) player %d\n", (unsigned long long)e.a, (unsigned long long)e.b, dec_.str(e.str).c_str(), isPlayer(e.a) ? 1 : 0);
			if (isPlayer(e.a)) { say("theft: took from " + std::to_string(e.b) + " (owner " + dec_.str(e.str) + ")"); }
			continue;
		}
		if (e.kind == 1) { printf("hit: %llu by %llu: result %u damage %.1f %.1f %.1f %.1f absorbed %.1f %.1f\n", (unsigned long long)e.a, (unsigned long long)e.b, e.result, e.damage[0], e.damage[1], e.damage[2], e.damage[3], e.absorbed[0], e.absorbed[1]); }
		if (e.kind == 1 && e.result == 0)	// a landed hit: the damage floater over the victim
		{
			Popup p = { e.a, e.kind, e.result, tick_, {}, e.b };
			std::copy(std::begin(e.damage), std::end(e.damage), p.damage);
			std::copy(std::begin(e.absorbed), std::end(e.absorbed), p.absorbed);
			p.part = e.str;
			float hull[3];
			if (placed(e.a, p.pos, hull)) { popups_.push_back(p); }
			continue;
		}
		if (e.kind == 20)	// lock_roll: "Pick success!" / "Pick failed" over the picker
		{
			Popup p = { e.a, e.kind, e.result, tick_ };
			float hull[3];
			if (placed(e.a, p.pos, hull)) { popups_.push_back(p); }
			printf("lock roll: %llu on %llu: %s\n", (unsigned long long)e.a, (unsigned long long)e.b, e.result ? "success" : "failed");
			continue;
		}
		if (e.kind == 51 || e.kind == 52)	// caught_stealing / alarm_raised: the original's HUD message-bar lines (the host posts them)
		{
			crimeLog_.push_back(e.kind);
			printf("%s: %llu / %llu\n", e.kind == 51 ? "caught stealing" : "alarm raised", (unsigned long long)e.a, (unsigned long long)e.b);
			continue;
		}
		if (e.kind == 19)	// discovered: the original's "Discovered {1}" log line (the host's)
		{
			discovered_.push_back(e.str);
			printf("discovered: %s\n", dec_.str(e.str).c_str());
			continue;
		}
		if (e.kind == 18)	// dismissed (order 75 applied): the character left the faction
		{
			const auto c = dec_.chars.find(e.a);
			const std::string who = c != dec_.chars.end() && c->second.look.name ? dec_.str(c->second.look.name) : "#" + std::to_string(e.a);
			say(who + " joined " + dec_.str(e.str));
			continue;
		}
		if (e.kind == 40)	// trade_closed (the decoder dropped the trade)
		{
			static const char* const kWhy[] = { "client", "order", "trader gone" };
			say("trade closed: " + std::string(e.result < 3 ? kWhy[e.result] : "?"));
			continue;
		}
		if (e.kind == 48)	// ragdoll (sim BG): logged until the ragdoll consumes it (CLIENT_GOALS "Ragdolls" P3)
		{
			printf("ragdoll: %llu cause %u dir %.2f %.2f %.2f strength %.1f part %s seed %u\n", (unsigned long long)e.a, e.result, e.pos[0], e.pos[1], e.pos[2], e.amount, dec_.str(e.str).c_str(), e.ref[0]);
			RagdollCue q;
			q.id = e.a;
			q.cause = e.result;
			for (int k = 0; k < 3; k++) { q.dir[k] = e.pos[k]; }
			q.strength = e.amount;
			q.part = dec_.str(e.str);
			ragdollCues.push_back(std::move(q));
		}
		if (e.kind == 49)	// shot (sim 00f1d450): one per fired bolt; the bolt's drawing waits on kenshi-rwh.32 (how the original draws its 'live ammo' mesh)
		{
			printf("shot: %llu at %llu from %.1f %.1f %.1f to %.1f %.1f %.1f %s weapon %s turret %llu\n", (unsigned long long)e.a, (unsigned long long)e.b, e.pos[0], e.pos[1], e.pos[2],
				e.to[0], e.to[1], e.to[2], e.result == 0 ? "hit" : e.result == 2 ? "missed" : "blocked", dec_.str(e.str).c_str(), (unsigned long long)e.c);
			continue;
		}
		if (e.kind == 50)	// stealth_ko (sim 15d0252b): one per attempt, at the roll (0/1, with the chance) or the abort (2, aware: no roll)
		{
			static const char* const kResult[] = { "landed", "roll failed", "aborted (the victim was aware)" };
			const char* what = e.result < 3 ? kResult[e.result] : "?";
			printf("stealth ko: %llu on %llu %s chance %.0f%% at tick %llu\n", (unsigned long long)e.a, (unsigned long long)e.b, what, e.amount, (unsigned long long)e.tick);
			evlog::add("ko", tick_, e.b, "stealth KO by %llu: %s, chance %.0f%%", (unsigned long long)e.a, what, e.amount);
			// the floater on the KO'ing character (the exe's ScreenLabel: s54_static.md 1b "attached to the KO'ing character (0,15,0)", the assassin's
			// own task, not the victim's); `other` = the victim (kenshi-k16.2.275.1)
			Popup p = { e.a, e.kind, e.result, tick_ };
			p.other = e.b;
			float hull[3];
			if (placed(e.a, p.pos, hull)) { popups_.push_back(p); }
			continue;
		}
		if (e.kind == 47)	// limb severed: the original has no message or sound for it (kinspect cq_sever_visual.md); the stump follows with CHAR_MEDICAL
		{
			printf("severed: %llu %s\n", (unsigned long long)e.a, dec_.str(e.str).c_str());
			continue;
		}
		if (e.kind == 41)
		{
			speech_[e.a] = { e.str, tick_, e.amount, e.b, e.dir };
			printf("speech: %llu to %llu: \"%s\" (%s / %s, %.1f s%s%s)\n", (unsigned long long)e.a, (unsigned long long)e.b, dec_.str(e.str).c_str(), dec_.str(e.ref[0]).c_str(), dec_.str(e.ref[1]).c_str(), e.amount, e.dir & 2 ? ", interjection" : "", e.dir & 8 ? ", large" : e.dir & 16 ? ", important" : "");
			continue;
		}
		if (e.kind == 42)
		{
			speech_.erase(e.a);
			printf("speech_end: %llu\n", (unsigned long long)e.a);
			continue;
		}
		if (e.kind == 43)	// reply_menu: the decoder keeps it in menus
		{
			printf("reply_menu: %llu asks %llu, %u options%s%s\n", (unsigned long long)e.a, (unsigned long long)e.b, e.result, e.dir & 1 ? ", resend" : "", e.dir & 4 ? ", freezes" : "");
			for (const auto& m : dec_.menus)
			{
				if (m.speaker != e.a || m.ch != e.b) { continue; }
				for (size_t i = 0; i < m.options.size(); i++) { printf("  %zu: \"%s\" (%s)\n", i, m.options[i].line ? dec_.str(m.options[i].text).c_str() : "[...]", dec_.str(m.options[i].line).c_str()); }
			}
			continue;
		}
		if (e.kind == 44)
		{
			static const char* const kWhy[] = { "finished", "?", "interrupted", "gone", "walked away", "no client" };
			printf("conversation_end: %llu with %llu: %s\n", (unsigned long long)e.a, (unsigned long long)e.b, e.result < 6 ? kWhy[e.result] : "?");
			continue;
		}
		if (e.kind == 15)	// noticed: a log line only (the original's barks, EV_I_SEE_ENEMY_PLAYER, wait for dialogue; they want observer = an enemy: it fires both ways)
		{
			printf("noticed: %llu by %llu\n", (unsigned long long)e.b, (unsigned long long)e.a);
			continue;
		}
		if (e.kind != 9 && e.kind != 10) { continue; }
		if (menuSent_.erase((uint32_t)e.a)) { menuAnswers_.push_back({ (uint32_t)e.a, e.kind == 9, e.kind == 9 ? std::string() : dec_.str(e.str) }); }	// order 99: the host takes it off takeMenuAnswers
		if (profileSent_.erase((uint32_t)e.a))	// order 120: the answer is the PROFILE frame; a refusal is the profiling overlay's own line, never a toast
		{
			if (e.kind == 10) { profileStatus_ = dec_.str(e.str); }
			sent_.erase((uint32_t)e.a);
			quiet_.erase((uint32_t)e.a);
			continue;
		}
		if (quiet_.erase((uint32_t)e.a) && e.kind == 9) { continue; }	// the camera's own set_view / set_interest: only a rejection is news
		const auto it = sent_.find((uint32_t)e.a);
		const std::string what = it != sent_.end() ? it->second : "order " + std::to_string(e.a);
		if (e.kind == 9) { reply_ = what + " applied at tick " + std::to_string(e.b); }
		else if (dec_.str(e.str) == "nothing_to_say") { say("nothing to say"); }	// talk's second answer (after its event 9 and 13): no dialogue opened
		else
		{
			say(what + " REJECTED: " + dec_.str(e.str));
			rejected_ = true;
			fprintf(stderr, "kclient: simlink: %s\n", reply_.c_str());
			evlog::add("rejected", tick_, 0, "%s", reply_.c_str());
		}
		orderResults_[(uint32_t)e.a] = e.kind == 9 ? "applied tick " + std::to_string(e.b) : "rejected " + dec_.str(e.str);	// the shot's route sidecar
		if (orderResults_.size() > 256)	// bounded: the oldest clientSeq's answer goes (a long session's map, never the move orders' own)
		{
			auto oldest = orderResults_.begin();
			for (auto i = orderResults_.begin(); i != orderResults_.end(); ++i) { if (i->first < oldest->first) { oldest = i; } }
			orderResults_.erase(oldest);
		}
		if (it != sent_.end())
		{
			orderAnswers_.push_back({ (uint32_t)e.a, e.kind == 9, e.kind == 9 ? std::string() : dec_.str(e.str) });
			sent_.erase(it);
		}
	}
}

uint64_t SimView::pick(const double origin[3], const double dir[3], double* tOut, const std::function<bool(uint64_t)>& skip, const std::function<int(uint64_t, double[][3])>& body) const
{
	// The original (kenshi-inspect picker.md): an axis-aligned box on the feet, the RACE hull size (5 x 20 x 5 for humanoids; it does not turn with
	// yaw), flattened to max(X, Z) high when prone; a ragdolled body is hit on its ragdoll bones instead. We have no ragdoll: a lying character is a
	// segment kLying long through the feet point along its yaw, kLift up, hit within kReach (MINE, about the bones' 1.8 u thickness and a margin).
	constexpr double kLying = 18, kLift = 1.5, kReach = 3, kBody = 4.5;	// kBody: a lying body's half thickness around its bone segments (MINE)
	uint64_t best = 0;
	double bestT = 1e30;
	for (const uint64_t id : order_)
	{
		const Slot& sl = slots_.at(id);
		if (sl.index < 0) { continue; }
		// gameplay reads the sim's x, z, never the display slop's drawn ones (kenshi-k16.2.74); the Y is the DRAWN body's (kenshi-k16.2.395.1): a click
		// must land on the character where it draws (the stand ray's floor / stair y, eased), as the ring and the name label already do (sl.pos)
		const double pickedY = sl.pos[1];
		if (sl.down != Lying && sl.down != Asleep)	// seated: the standing box (the original's flattening is for prone only); asleep: the lying segment
		{
			// slab test against the box
			const double lo[3] = { sl.sim[0] - sl.hull[0] * 0.5, pickedY, sl.sim[2] - sl.hull[2] * 0.5 };
			const double hi[3] = { sl.sim[0] + sl.hull[0] * 0.5, pickedY + (sl.down == Up || sl.down == Seated ? sl.hull[1] : std::max(sl.hull[0], sl.hull[2])), sl.sim[2] + sl.hull[2] * 0.5 };
			double t0 = 0, t1 = 1e30;
			for (int k = 0; k < 3 && t0 <= t1; k++)
			{
				if (std::fabs(dir[k]) < 1e-12)
				{
					if (origin[k] < lo[k] || origin[k] > hi[k]) { t0 = 1; t1 = 0; }
					continue;
				}
				double a = (lo[k] - origin[k]) / dir[k], b = (hi[k] - origin[k]) / dir[k];
				if (a > b) { std::swap(a, b); }
				t0 = std::max(t0, a);
				t1 = std::min(t1, b);
			}
			if (t0 <= t1 && t0 < bestT && !(skip && skip(id))) { bestT = t0; best = id; }
			continue;
		}
		if (body)	// the drawn body: its bones' segments (head-pelvis, pelvis-feet), each hit within kBody of the ray (k16.2.223)
		{
			double pts[8][3];
			const int n = body(id, pts);	// 2 points per segment
			if (n > 0)
			{
				double bt = 1e30;
				for (int j = 0; j < n; j++)
				{
					const double* p0 = pts[2 * j];
					const double* p1 = pts[2 * j + 1];
					double a[3], u[3];
					for (int k = 0; k < 3; k++) { a[k] = p0[k] - origin[k]; u[k] = p1[k] - p0[k]; }
					const double uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2], du = dir[0] * u[0] + dir[1] * u[1] + dir[2] * u[2];
					const double da = dir[0] * a[0] + dir[1] * a[1] + dir[2] * a[2], ua = u[0] * a[0] + u[1] * a[1] + u[2] * a[2];
					const double den = uu - du * du;
					double s = den > 1e-9 ? std::clamp((du * da - ua) / den, 0.0, 1.0) : 0.0;
					const double t = std::max(0.0, da + du * s);
					s = uu > 1e-9 ? std::clamp((t * du - ua) / uu, 0.0, 1.0) : 0.0;
					double d2 = 0;
					for (int k = 0; k < 3; k++) { const double e = dir[k] * t - a[k] - u[k] * s; d2 += e * e; }
					if (d2 < kBody * kBody) { bt = std::min(bt, t); }
				}
				if (bt < bestT && !(skip && skip(id))) { bestT = bt; best = id; }
				continue;
			}
		}
		double a[3], u[3];	// segment start (relative to the ray origin) and its full extent
		u[0] = std::sin(sl.yaw) * kLying; u[1] = 0; u[2] = std::cos(sl.yaw) * kLying;
		for (int k = 0; k < 3; k++) { a[k] = sl.sim[k] - origin[k] - u[k] * 0.5; }
		a[1] = pickedY - origin[1] + kLift;	// the drawn body's y (kenshi-k16.2.395.1), as the box above
		// closest points of the ray origin + dir t (t >= 0) and the segment a + u s (0 <= s <= 1)
		const double uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2], du = dir[0] * u[0] + dir[1] * u[1] + dir[2] * u[2];
		const double da = dir[0] * a[0] + dir[1] * a[1] + dir[2] * a[2], ua = u[0] * a[0] + u[1] * a[1] + u[2] * a[2];
		const double den = uu - du * du;
		double s = den > 1e-9 ? std::clamp((du * da - ua) / den, 0.0, 1.0) : 0.0;
		const double t = std::max(0.0, da + du * s);
		s = std::clamp((t * du - ua) / uu, 0.0, 1.0);
		double d2 = 0;
		for (int k = 0; k < 3; k++) { const double e = dir[k] * t - a[k] - u[k] * s; d2 += e * e; }
		if (d2 < kReach * kReach && t < bestT && !(skip && skip(id))) { bestT = t; best = id; }
	}
	if (tOut) { *tOut = bestT; }
	return best;
}

uint64_t SimView::pickBody(const double origin[3], const double dir[3], double* tOut, const std::function<bool(uint64_t)>& skip, const std::function<int(uint64_t, std::vector<Characters::PickCapsule>&)>& capsules) const
{
	constexpr double kBroad = 30;	// a body (lying, ragdolled) stays within this of its node: the cheap reject before the capsules are posed
	uint64_t best = 0;
	double bestT = 1e30;
	std::vector<Characters::PickCapsule> caps;
	const auto nearRay = [&](const double c[3])
	{
		const double w[3] = { c[0] - origin[0], c[1] - origin[1], c[2] - origin[2] };
		const double t = w[0] * dir[0] + w[1] * dir[1] + w[2] * dir[2];
		if (t < -kBroad) { return false; }
		return w[0] * w[0] + w[1] * w[1] + w[2] * w[2] - t * t < kBroad * kBroad;
	};
	for (const uint64_t id : order_)
	{
		const Slot& sl = slots_.at(id);
		if (sl.index < 0 || (!nearRay(sl.sim) && !nearRay(sl.pos)) || (skip && skip(id))) { continue; }
		caps.clear();
		if (capsules(id, caps) <= 0) { continue; }
		for (const Characters::PickCapsule& c : caps)
		{
			double a[3], u[3];
			for (int k = 0; k < 3; k++) { a[k] = c.a[k] - origin[k]; u[k] = c.b[k] - c.a[k]; }
			const double uu = u[0] * u[0] + u[1] * u[1] + u[2] * u[2], du = dir[0] * u[0] + dir[1] * u[1] + dir[2] * u[2];
			const double da = dir[0] * a[0] + dir[1] * a[1] + dir[2] * a[2], ua = u[0] * a[0] + u[1] * a[1] + u[2] * a[2];
			const double den = uu - du * du;
			double s = den > 1e-9 ? std::clamp((du * da - ua) / den, 0.0, 1.0) : 0.0;
			const double t = std::max(0.0, da + du * s);
			s = uu > 1e-9 ? std::clamp((t * du - ua) / uu, 0.0, 1.0) : 0.0;
			double d2 = 0;
			for (int k = 0; k < 3; k++) { const double e = dir[k] * t - a[k] - u[k] * s; d2 += e * e; }
			if (d2 >= c.r * c.r) { continue; }
			const double front = std::max(0.0, t - std::sqrt(c.r * c.r - d2));	// the ray's entry into the capsule
			if (front < bestT) { bestT = front; best = id; }
		}
	}
	if (tOut) { *tOut = bestT; }
	return best;
}

void SimView::view(double x, double z)
{
	if (mode_ != Live || !helloSent_ || fd_ == kutil::net::kNoSocket) { return; }
	std::vector<uint8_t> out;
	const double interest = kInterestRadiusCfg.get();	// render.interest_radius (kenshi-k16.2.258.10.1)
	if (!viewOnce_ || interest != interestRadiusSent_ || std::hypot(x - interestSent_[0], z - interestSent_[1]) > kInterestMove)
	{
		quiet_.insert(orders_.setInterest(out, (float)x, (float)z, (float)interest));
		interestSent_[0] = x;
		interestSent_[1] = z;
		interestRadiusSent_ = interest;
	}
	if (!viewOnce_ || std::hypot(x - viewSent_[0], z - viewSent_[1]) > kViewMove)
	{
		quiet_.insert(orders_.setView(out, (float)x, (float)z));
		viewSent_[0] = x;
		viewSent_[1] = z;
	}
	viewOnce_ = true;
	if (!out.empty()) { send(out, "view"); }
}

bool SimView::drive(uint64_t id, uint8_t drive, float heading, int strafe)
{
	if (mode_ != Live || !isPlayer(id)) { return false; }
	std::vector<uint8_t> out;
	quiet_.insert(orders_.drive(out, id, drive, heading, (int8_t)strafe));
	if (!send(out, "drive")) { return false; }
	if (local_.id != id) { local_ = Local{}; local_.id = id; }
	local_.drive = drive;
	local_.strafe = strafe;
	local_.heading = heading;
	return true;
}

// Direct drive prediction (SIMLINK 12, CLIENT.md "Prediction"): the driven character steps on the client's navmesh with our input at once
// (knav::Walker, the server's own step), pulled towards the server's newest state stepped on to now with the server's own drive. The speed is
// the one seen while the server drove it forward (none yet: no lead). MINE: the pull rate, the snap distance, the settle distance.
void SimView::predict(const Character& c, double p[3], float& yaw)
{
	// Both sides step knav's gait ramp (Walker::drive with a forward speed; the server's own code): the server's sample carries its ramped
	// speed (fwd), so the target is its position stepped on by the lead from that state, in server ticks. The gait speed needs athletics,
	// which the client lacks: learnt from steady forward samples (fwd unchanged between two), the race-default gait until then.
	constexpr double kPull = 4.0, kSnap = 30.0, kSettle = 0.5;
	const Dynamic& s = c.cur;
	const bool run = s.flags & kRun;
	const int wireStrafe = (s.flags & kStrafeRight ? 1 : 0) - (s.flags & kStrafeLeft ? 1 : 0);	// CHAR_DYNAMIC bits 22 / 23: the applied strafe
	if (run != local_.run) { local_.run = run; local_.gait = 0; }
	knav::Gait g;
	g.run = run;
	// fwd is the tick's displacement (sim task W): a snap or push reads as a spike, so a sample above 2x the default gait never teaches the gait
	if (c.samples > 1 && s.drive == 1 && c.prev.drive == 1 && s.fwd > 1 && s.fwd < 2 * knav::gaitSpeed(g) && std::fabs(s.fwd - c.prev.fwd) < 0.05f) { local_.gait = s.fwd; }
	const double gait = local_.gait > 0 ? local_.gait : knav::gaitSpeed(g);
	if (!walker_ || (!local_.drive && !local_.strafe && !s.drive && !wireStrafe && !local_.on)) { local_.on = false; local_.fwd = 0; return; }
	double lead = std::max(0.0, (tick_ + delayTicks() - (double)s.tick) / kTickRate), fwd = std::clamp((double)s.fwd, -gait, gait);	// the ramp state
	knav::StepResult target{ { s.pos[0], s.pos[1], s.pos[2] }, true, false, false };
	for (; lead > 1e-9 && target.onMesh; lead -= 1.0 / kTickRate)
	{
		target = walker_->drive(target.at, (knav::Drive)s.drive, s.heading, gait, fwd, std::min(lead, 1.0 / kTickRate), knav::kGaitAcceleration, wireStrafe);
	}
	if (!target.onMesh) { local_.on = false; return; }
	if (!local_.on) { for (int k = 0; k < 3; k++) { local_.pos[k] = p[k]; } local_.fwd = std::clamp((double)s.fwd, -gait, gait); local_.vel[0] = local_.vel[1] = 0; local_.on = true; }
	knav::StepResult mine = walker_->drive({ local_.pos[0], local_.pos[1], local_.pos[2] }, (knav::Drive)local_.drive, local_.heading, gait, local_.fwd, dtSim_, knav::kGaitAcceleration, local_.strafe);
	if (!mine.onMesh) { mine.at = target.at; }
	const double e = std::hypot(target.at.x - mine.at.x, target.at.z - mine.at.z);
	const double pull = e > kSnap ? 1.0 : std::min(1.0, kPull * dtSim_);
	const double prevX = local_.pos[0], prevZ = local_.pos[2];
	local_.pos[0] = mine.at.x + (target.at.x - mine.at.x) * pull;
	local_.pos[1] = mine.at.y + (target.at.y - mine.at.y) * pull;
	local_.pos[2] = mine.at.z + (target.at.z - mine.at.z) * pull;
	if (dtSim_ > 0) { const double a = std::min(1.0, dtSim_ / 0.1); local_.vel[0] += ((local_.pos[0] - prevX) / dtSim_ - local_.vel[0]) * a; local_.vel[1] += ((local_.pos[2] - prevZ) / dtSim_ - local_.vel[1]) * a; }	// the predicted velocity (kenshi-k16.2.237.10: the travel facing), 0.1 s smoothed
	if (!local_.drive && !local_.strafe && !s.drive && !wireStrafe && e < kSettle) { local_.on = false; }
	// The walker's y is the raw navmesh height; the server sends the ground ray's y (kenshi-k16.2.339, report k16.2.338 section 4 "Prediction"): carry the newest
	// sample's (wire y - navmesh y at the same x, z) onto the prediction so driving draws the height the server sends. Dropped (0) off the mesh or past 6 u.
	double yOff = 0;
	if (const auto ny = walker_->surfaceY(s.pos[0], s.pos[1], s.pos[2])) { yOff = s.pos[1] - *ny; if (std::fabs(yOff) > 6.0) { yOff = 0; } }
	for (int k = 0; k < 3; k++) { p[k] = local_.pos[k]; }
	predictYOff_ = yOff;
	p[1] += yOff;
	if (local_.drive == 1 || local_.strafe) { yaw = (float)local_.heading; }
}

bool SimView::pose(uint64_t id, double pos[3], double& yaw, const simlink::Route*& route) const
{
	const auto sl = slots_.find(id);
	const auto c = dec_.chars.find(id);
	if (sl == slots_.end() || sl->second.index < 0 || c == dec_.chars.end()) { return false; }
	for (int k = 0; k < 3; k++) { pos[k] = sl->second.pos[k]; }
	yaw = sl->second.yaw;
	route = &c->second.route;
	return true;
}

std::vector<SimView::Marker> SimView::markers() const
{
	std::vector<Marker> out;
	for (const uint64_t id : selected_)
	{
		const auto sl = slots_.find(id);
		const auto c = dec_.chars.find(id);
		if (sl == slots_.end() || c == dec_.chars.end()) { continue; }
		Marker m = { { sl->second.pos[0], sl->second.pos[1], sl->second.pos[2] }, &c->second.route };
		out.push_back(m);
	}
	return out;
}

int SimView::charIndex(uint64_t id, uint32_t* version) const
{
	const auto it = slots_.find(id);
	if (it == slots_.end()) { return -1; }
	if (version) { *version = it->second.version; }
	return it->second.index;
}

bool SimView::nudge(uint64_t id, double dx, double dz)
{
	const auto it = slots_.find(id);
	if (it == slots_.end() || !it->second.drawn) { return false; }
	it->second.pos[0] += dx;
	it->second.pos[2] += dz;
	it->second.off[0] += dx;	// display slop: the drawn's offset from the sim grows, so it eases back (slop) or the chase corrects it (slop off)
	it->second.off[1] += dz;
	return true;
}

bool SimView::placed(uint64_t id, double pos[3], float hull[3]) const
{
	const auto it = slots_.find(id);
	if (it == slots_.end() || it->second.index < 0) { return false; }
	for (int k = 0; k < 3; k++) { pos[k] = it->second.pos[k]; hull[k] = it->second.hull[k]; }
	return true;
}

bool SimView::simPos(uint64_t id, double pos[3]) const
{
	const auto it = slots_.find(id);
	if (it == slots_.end() || it->second.index < 0) { return false; }
	for (int k = 0; k < 3; k++) { pos[k] = it->second.sim[k]; }
	return true;
}

bool SimView::wireY(uint64_t id, double& y) const
{
	const auto it = slots_.find(id);
	if (it == slots_.end() || it->second.index < 0 || it->second.rayRes == -2) { return false; }
	y = it->second.rayWireY;
	return true;
}

bool SimView::stateAt(uint64_t id, simlink::Dynamic& out) const
{
	const auto it = dec_.chars.find(id);
	if (it == dec_.chars.end()) { return false; }
	const simlink::Character& c = it->second;
	// The newest sample at or before the render tick: the earlier end of the interpolation pair sync() draws the position from (the oldest sample when the
	// tick is before them all). A frame's floor, indoors and building bits then belong to the time of the position, never a later one.
	auto older = [&](int k) -> const simlink::Dynamic*	// the k-th sample back from the newest (0 = cur, 1 = prev, then past newest first)
	{
		if (k == 0) { return &c.cur; }
		if (c.samples < 2) { return nullptr; }
		if (k == 1) { return &c.prev; }
		return (size_t)(k - 2) < c.past.size() ? &c.past[c.past.size() - 1 - (size_t)(k - 2)] : nullptr;
	};
	const simlink::Dynamic* s = &c.cur;
	for (int k = 1; (double)s->tick > tick_; k++)
	{
		const simlink::Dynamic* o = older(k);
		if (!o) { break; }
		s = o;
	}
	out = *s;
	return true;
}

bool SimView::isPlayer(uint64_t id) const
{
	const auto it = dec_.chars.find(id);
	return it != dec_.chars.end() && (it->second.cur.flags & kPlayer);
}

// The selection is never emptied (the original): a click on the ground or a stranger keeps it, only another of ours replaces it.
void SimView::select(uint64_t id, bool add)
{
	if (!id || !isPlayer(id)) { return; }
	holdEmpty_ = false;
	if (!add) { selected_.clear(); }
	if (std::find(selected_.begin(), selected_.end(), id) == selected_.end()) { selected_.push_back(id); }
}

// kenshi-4bps.1: one row in the shot's route sidecar for one order this client just sent for id. Every order that sets a character's task goes
// in, not only move_to, so a capture can say whether an attack / interact / follow went out and what the wire answered (the move rows kept their
// shape: what is the click origin there, and the order's own word elsewhere).
void SimView::keepOrder(uint64_t id, uint32_t seq, const char* what, uint64_t target, const double* goal, bool yValid, bool queue)
{
	OrderRow o;
	o.seq = seq;
	o.id = id;
	o.tick = dec_.lastCompleteTick;
	o.what = what;
	o.target = target;
	if (goal) { o.goal[0] = goal[0]; o.goal[1] = goal[1]; o.goal[2] = goal[2]; }
	o.yValid = yValid && goal;
	o.queue = queue;
	std::deque<OrderRow>& kept = orderRows_[id];
	kept.push_back(std::move(o));
	if (kept.size() > kOrdersKept) { kept.pop_front(); }
}

// The wire position of a character's feet or an interactable's own point, for a targeted order's sidecar goal (false: the wire does not know it).
bool SimView::wirePos(uint64_t id, double pos[3]) const
{
	if (const auto c = dec_.chars.find(id); c != dec_.chars.end())
	{
		for (int k = 0; k < 3; k++) { pos[k] = c->second.cur.pos[k]; }
		return true;
	}
	if (const simlink::Interactable* t = thing(id))
	{
		for (int k = 0; k < 3; k++) { pos[k] = t->pos[k]; }
		return true;
	}
	return false;
}

bool SimView::moveTo(const double at[3], bool queue, bool yValid, const Click* click)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty()) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.moveTo(out, selected_, (float)at[0], (float)at[2], queue, (float)at[1], yValid);
	char what[80];
	snprintf(what, sizeof what, "move_to %.0f %.0f %.0f", at[0], at[1], at[2]);
	sent_[lastSentSeq_ = seq] = what;
	tasked(selected_, queue);
	if (!send(out, "move_to")) { return false; }
	// kenshi-k16.2.231.1: keep the order for the shot's route sidecar (the last 8 a character got), with where the click came from
	for (const uint64_t id : selected_)
	{
		keepOrder(id, seq, click ? click->what : "use point", 0, at, yValid, queue);	// no click: the walk to a container / counter / script op
		OrderRow& o = orderRows_[id].back();
		o.move = true;
		if (click) { o.building = click->building; o.buildingName = click->buildingName; o.floor = click->floor; }
	}
	return true;
}

const simlink::Route* SimView::routeOf(uint64_t id) const
{
	const auto it = dec_.chars.find(id);
	return it == dec_.chars.end() ? nullptr : &it->second.route;
}

std::string SimView::charName(uint64_t id) const
{
	const auto it = dec_.chars.find(id);
	return it != dec_.chars.end() && it->second.look.name ? dec_.str(it->second.look.name) : std::string();
}

std::vector<uint64_t> SimView::players() const
{
	std::vector<uint64_t> out;
	for (const auto& [id, c] : dec_.chars) { if (c.cur.flags & simlink::kPlayer) { out.push_back(id); } }
	std::sort(out.begin(), out.end());
	return out;
}

// The outcome as the wire gave it: the route that carried the order's clientSeq (a clear too), else event 9 / 10's answer, else nothing yet.
std::vector<SimView::OrderRow> SimView::orders(uint64_t id) const
{
	std::vector<OrderRow> out;
	const auto it = orderRows_.find(id);
	if (it == orderRows_.end()) { return out; }
	const simlink::Route* route = routeOf(id);
	for (const OrderRow& o : it->second)
	{
		out.push_back(o);
		OrderRow& v = out.back();
		if (route && o.seq && route->clientSeq == o.seq)
		{
			char text[128];
			snprintf(text, sizeof text, "route v%u%s at tick %llu (%zu waypoints%s)%s", route->version, route->points.empty() ? " cleared" : "",
				(unsigned long long)route->arrivedTick, route->points.size(), route->flags & 1 ? ", truncated" : "",
				route->flags & 2 ? ", approach point" : "");
			v.outcome = text;
		}
		else if (const auto r = orderResults_.find(o.seq); r != orderResults_.end()) { v.outcome = r->second; }
		else { v.outcome = "nothing"; }
	}
	return out;
}

uint64_t SimView::lastMoveTick(uint64_t id) const
{
	const auto it = orderRows_.find(id);
	if (it == orderRows_.end()) { return 0; }
	for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) { if (r->move) { return r->tick; } }
	return 0;
}

bool SimView::attack(uint64_t target)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty() || !target) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.attack(out, selected_, target);
	sent_[lastSentSeq_ = seq] = "attack " + std::to_string(target);
	tasked(selected_, false);
	if (!send(out, "attack")) { return false; }
	// kenshi-4bps.1: the row the route sidecar was missing (an attack silently absent from a capture reads as "never sent"); goal: where the
	// target stood at the send
	double at[3];
	const bool seen = wirePos(target, at);
	for (const uint64_t id : selected_) { keepOrder(id, seq, "attack", target, seen ? at : nullptr, seen, false); }
	return true;
}

bool SimView::interact(uint64_t target, uint8_t action, bool queue, uint8_t flags)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty() || !target) { return false; }
	static const char* const kNames[] = { "?", "open", "close", "lockpick", "use", "loot", "talk", "pick up", "carry", "sleep", "first aid", "unlock", "bash", "put down", "stealth KO", "pick shackles", "cut shackles", "break shackles", "pick cage", "cut cage", "break cage" };
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.interact(out, selected_, target, action, queue, flags);
	sent_[lastSentSeq_ = seq] = std::string(kNames[action <= 20 ? action : 0]) + (flags & 1 ? " open " : " ") + std::to_string(target);
	tasked(selected_, queue);
	if (!send(out, "interact")) { return false; }
	// kenshi-4bps.1: the sidecar row (what: the action's name, e.g. "stealth KO" / "pick up")
	double at[3];
	const bool seen = wirePos(target, at);
	for (const uint64_t id : selected_) { keepOrder(id, seq, kNames[action <= 20 ? action : 0], target, seen ? at : nullptr, seen, queue); }
	return true;
}

bool SimView::shackle(uint64_t actor, uint64_t wearer, uint8_t action)
{
	if (mode_ != Live || !dec_.chars.count(actor) || !dec_.chars.count(wearer) || action < 15 || action > 17) { return false; }
	static const char* const kNames[] = { "pick shackles", "cut shackles", "break shackles" };
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.interact(out, { actor }, wearer, action, false)] = std::string(kNames[action - 15]) + " " + std::to_string(wearer);
	tasked({ actor }, false);
	return send(out, "shackle");
}

bool SimView::equip(uint64_t id, uint32_t item, bool on)
{
	if (mode_ != Live || !dec_.chars.count(id) || !item) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.equip(out, id, dec_.str(item), on)] = std::string(on ? "equip " : "unequip ") + dec_.str(item);
	return send(out, "equip");
}

uint32_t SimView::ownFaction() const
{
	for (const simlink::Squad& q : dec_.squads)
	{
		for (const simlink::PlayerSquads::Squad& p : dec_.playerSquads.squads)
		{
			if (p.id == q.id) { return q.faction; }
		}
	}
	return 0;
}

bool SimView::foreign(uint64_t thing) const
{
	const auto it = dec_.things.find(thing);
	if (it != dec_.things.end() && it->second.hasOwner) { return it->second.owner != 0 && it->second.owner != ownFaction(); }
	return foreign_.count(thing) != 0;
}

bool SimView::transfer(uint64_t id, uint64_t container, uint32_t item, uint16_t count, bool take)
{
	if (mode_ != Live || !dec_.chars.count(id) || !container || !item || !count) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.transfer(out, id, container, dec_.str(item), count, take)] = std::string(take ? "take " : "put ") + dec_.str(item) + " x" + std::to_string(count);
	return send(out, "transfer");
}

bool SimView::drop(uint64_t id, uint32_t item, uint16_t count)
{
	if (mode_ != Live || !dec_.chars.count(id) || !item || !count) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.drop(out, id, dec_.str(item), count)] = "drop " + dec_.str(item) + " x" + std::to_string(count);
	return send(out, "drop");
}

bool SimView::moveItem(uint64_t id, uint32_t version, uint16_t index, uint8_t section, uint8_t x, uint8_t y, uint16_t count)
{
	if (mode_ != Live || !id) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.moveItem(out, id, version, index, section, x, y, count)] = "move item " + std::to_string(index) + " to " + std::to_string(section) + ":" + std::to_string(x) + "," + std::to_string(y);
	return send(out, "move item");
}

bool SimView::arrange(uint64_t id, uint8_t section)
{
	if (mode_ != Live || !id) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.arrange(out, id, section)] = "arrange " + std::to_string(section);
	return send(out, "arrange");
}

bool SimView::dropEntry(uint64_t id, uint32_t version, uint16_t index, uint16_t count)
{
	if (mode_ != Live || !dec_.chars.count(id) || !count) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.dropEntry(out, id, version, index, count)] = "drop entry " + std::to_string(index) + " x" + std::to_string(count);
	return send(out, "drop entry");
}

bool SimView::equipEntry(uint64_t id, uint32_t version, uint16_t index, bool on)
{
	if (mode_ != Live || !dec_.chars.count(id)) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.equipEntry(out, id, version, index, on)] = std::string(on ? "equip entry " : "unequip entry ") + std::to_string(index);
	return send(out, "equip entry");
}

bool SimView::saveGame(const std::string& name)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.saveGame(out, name)] = "save game " + name;
	return send(out, "save game");
}

bool SimView::loadGame(const std::string& name, bool resetPositions)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.loadGame(out, name, resetPositions);
	sent_[lastSentSeq_ = seq] = "load game " + name;
	dec_.resetSeq = seq;
	return send(out, "load game");
}

bool SimView::deleteSave(const std::string& name)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.deleteSave(out, name)] = "delete save " + name;
	return send(out, "delete save");
}

bool SimView::listSaves()
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	quiet_.insert(orders_.listSaves(out));	// only a refusal is news
	return send(out, "list saves");
}

bool SimView::newWorld()
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.newWorld(out);
	sent_[lastSentSeq_ = seq] = "new world";
	dec_.resetSeq = seq;
	return send(out, "new world");
}

bool SimView::newGame(const std::string& name, uint32_t startoff, const std::vector<simlink::OrderWriter::NewGameSpec>& specs)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	orders_.newGame(out, name, startoff, specs);
	return send(out, "new game");
}

bool SimView::scenarioList()
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	quiet_.insert(orders_.scenarioList(out));	// only a refusal is news
	return send(out, "scenario list");
}

bool SimView::scenarioLoad(const std::string& name, uint32_t seed)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.scenarioLoad(out, name, seed);
	sent_[lastSentSeq_ = seq] = "scenario " + name;
	dec_.resetSeq = seq;	// the load's event 9 drops the world, as a save's load does
	return send(out, "scenario load");
}

bool SimView::scenarioEnd()
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	quiet_.insert(orders_.scenarioEnd(out));	// only a refusal is news
	return send(out, "scenario end");
}

bool SimView::squadAdd()
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.squadAdd(out)] = "add squad";
	return send(out, "add squad");
}

bool SimView::squadDelete(uint64_t squad)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.squadDelete(out, squad)] = "delete squad " + std::to_string(squad);
	return send(out, "delete squad");
}

bool SimView::squadRename(uint64_t squad, const std::string& name)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.squadRename(out, squad, name)] = "rename squad " + std::to_string(squad);
	return send(out, "rename squad");
}

bool SimView::factionRename(const std::string& name)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.factionRename(out, name)] = "rename faction";
	return send(out, "rename faction");
}

bool SimView::squadMove(uint64_t id, uint64_t squad, uint16_t index, bool swap)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.squadMove(out, id, squad, index, swap)] = std::string(swap ? "swap " : "move ") + std::to_string(id) + " to squad " + std::to_string(squad);
	return send(out, "squad move");
}

bool SimView::squadReorder(uint64_t squad, uint16_t index)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.squadReorder(out, squad, index)] = "reorder squad " + std::to_string(squad);
	return send(out, "reorder squad");
}

bool SimView::dismiss(uint64_t id)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.dismiss(out, id)] = "dismiss " + std::to_string(id);
	return send(out, "dismiss");
}

bool SimView::transferEntry(uint64_t id, uint64_t container, uint32_t version, uint16_t index, uint16_t count, bool take, uint8_t section, uint8_t x, uint8_t y)
{
	if (mode_ != Live || !dec_.chars.count(id) || !container || !count) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.transferEntry(out, id, container, version, index, count, take, section, x, y)] = std::string(take ? "take entry " : "put entry ") + std::to_string(index) + " x" + std::to_string(count);
	return send(out, "transfer entry");
}

const simlink::Detect* SimView::detect(uint64_t id) const
{
	const auto it = dec_.detect.find(id);
	return it != dec_.detect.end() ? &it->second : nullptr;
}

const simlink::CharStats* SimView::charStats(uint64_t id) const
{
	const auto it = dec_.charStats.find(id);
	return it != dec_.charStats.end() ? &it->second : nullptr;
}

// The focus row is the player's own building only when its owner is the session's faction (k16.19.8.2): a focused producer row has owner 0 (a wild
// node) or an NPC faction's entity id, so the test is the owner, never owner == 0.
bool SimView::playerBuilding(const simlink::SimLinkDecoder::Building& b) const
{
	const uint64_t faction = dec_.playerSquads.faction;
	return b.owner != 0 && faction != 0 && b.owner == faction;
}

void SimView::focus(uint64_t id, bool producer)
{
	if (mode_ != Live || !helloSent_ || (id == focusSent_ && producer == focusProducer_)) { return; }
	// The row the previous focus brought (SIMLINK Order 87: the sim sends nothing when the focus moves away): drop it here. A row the player owns is one
	// of the player's buildings and stays in the map.
	if (focusSent_)
	{
		const auto it = dec_.buildings.find(focusSent_);
		if (it != dec_.buildings.end() && !playerBuilding(it->second)) { dec_.buildings.erase(it); dec_.buildingsSerial++; }
	}
	focusRow_ = {};
	focusProducer_ = producer;
	if (producer) { focusProducers_.insert(id); }	// its row is the focus row, never a player's building
	std::vector<uint8_t> out;
	quiet_.insert(orders_.focus(out, id));
	if (send(out, "focus")) { focusSent_ = id; }
}

const simlink::SimLinkDecoder::Building* SimView::focusBuilding() const
{
	if (!focusProducer_ || !focusSent_) { return nullptr; }
	if (focusRow_.id == focusSent_) { return &focusRow_; }
	const auto it = dec_.buildings.find(focusSent_);	// a player-owned producer: its row is a player's building and stays in the map
	return it != dec_.buildings.end() ? &it->second : nullptr;
}

bool SimView::progressLevel(uint8_t level)
{
	if (mode_ != Live || !helloSent_) { return false; }
	std::vector<uint8_t> out;
	quiet_.insert(orders_.setProgressLevel(out, level));	// only a refusal is news
	return send(out, "set progress level");
}

const simlink::Medical* SimView::medical(uint64_t id) const
{
	const auto it = dec_.medical.find(id);
	return it != dec_.medical.end() ? &it->second : nullptr;
}

float SimView::health(uint64_t id, const Characters& chars) const
{
	const Medical* m = medical(id);
	const auto c = dec_.chars.find(id);
	if (!m || c == dec_.chars.end()) { return 1.0f; }
	const std::string race = chars.nameOf(dec_.str(c->second.look.race));
	float sum = 0;
	int n = 0;
	for (const Medical::Part& p : m->parts)
	{
		const float max = chars.partMaxHealth(race, dec_.str(p.part));
		if (max <= 0) { continue; }
		sum += std::clamp(p.health / max, 0.0f, 1.0f);
		n++;
	}
	return n ? sum / n : 1.0f;
}

const simlink::Inventory* SimView::inventory(uint64_t id) const
{
	const auto it = dec_.inventories.find(id);
	return it != dec_.inventories.end() ? &it->second : nullptr;
}

uint64_t SimView::pickItem(const double origin[3], const double dir[3], double* tOut, const std::function<bool(uint64_t)>& skip) const
{
	uint64_t best = 0;
	double bestT = 1e30, bestEnter = 1e30;
	for (const auto& [id, t] : dec_.things)
	{
		if (t.kind != Interactable::kItem && t.kind != Interactable::kContainer) { continue; }
		const bool box = t.kind == Interactable::kContainer;
		const double reach = box ? kContainerReach : kItemReach;
		const double c[3] = { t.pos[0] - origin[0], thingY(id, t) + (box ? kContainerLift : kItemLift) - origin[1], t.pos[2] - origin[2] };
		const double along = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
		const double d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - along * along;
		// skip last, so a caller's test (a thing on a hidden floor: kenshi-k16.2.259) runs only for the things the ray is near
		if (along > 0 && d2 < reach * reach && along < bestT && !(skip && skip(id))) { bestT = along; best = id; bestEnter = std::max(0.0, along - std::sqrt(reach * reach - d2)); }
	}
	if (tOut) { *tOut = bestEnter; }	// the sphere's entry: the thing's own hit point, not the point level with its centre (kenshi-k16.2.259)
	return best;
}

double SimView::thingY(uint64_t id, const Interactable& t) const
{
	const void* gen = groundGen_ ? groundGen_() : nullptr;
	ThingY& c = thingY_[id];
	if (!c.set || c.x != t.pos[0] || c.y != t.pos[1] || c.z != t.pos[2] || c.gen != gen)
	{
		double y;
		// An item's pos y is not always a navmesh point: one on a table, shelf or upper ledge sits 10-47 u over the floor or terrain the solver finds, and the solver would sink it there
		// (measured on sunshafts_bar's bar items). So only the buried case is converted: the drawn ground ABOVE the pos y, within the coarse navmesh's own error (12-33 u under the hill
		// east of the Storm House); a point the ground is under keeps its y.
		c.drawn = groundAt(t.pos[0], t.pos[2], t.pos[1], y) >= 1 && y > t.pos[1] && y - t.pos[1] <= kBuriedMax ? y : t.pos[1];
		c.x = t.pos[0]; c.y = t.pos[1]; c.z = t.pos[2]; c.gen = gen; c.set = true;
	}
	return c.drawn;
}

const Interactable* SimView::thing(uint64_t id) const
{
	const auto it = dec_.things.find(id);
	return it != dec_.things.end() ? &it->second : nullptr;
}

std::string SimView::nameOf(uint32_t record, const Characters& chars) const
{
	return chars.nameOf(dec_.str(record));
}

bool SimView::setToggle(uint8_t which, uint8_t value)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty()) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.toggle(out, selected_, which, value)] = "toggle " + std::to_string(which) + " " + std::to_string(value);
	return send(out, "toggle");
}

bool SimView::jobAdd(uint16_t type, uint64_t target)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	std::vector<uint64_t> ids;
	std::copy_if(selected_.begin(), selected_.end(), std::back_inserter(ids), [&](uint64_t id) { return id != target; });
	if (mode_ != Live || ids.empty()) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.jobAdd(out, ids, type, target);
	sent_[lastSentSeq_ = seq] = "job " + std::to_string(type) + (target ? " " + std::to_string(target) : "");
	if (!send(out, "job add")) { return false; }
	double at[3];
	const bool seen = target && wirePos(target, at);
	for (const uint64_t id : ids) { keepOrder(id, seq, "job", target, seen ? at : nullptr, seen, false); }	// kenshi-4bps.1
	return true;
}

bool SimView::prospect(uint64_t id)
{
	if (mode_ != Live || !dec_.chars.count(id)) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.prospect(out, id)] = "prospect";
	return send(out, "prospect");
}

bool SimView::jobRemove(uint64_t id, uint32_t jobId)
{
	if (mode_ != Live || !dec_.chars.count(id)) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.jobRemove(out, id, jobId)] = "remove job " + std::to_string(jobId);
	return send(out, "job remove");
}

bool SimView::jobMove(uint64_t id, uint32_t jobId, uint16_t index)
{
	if (mode_ != Live || !dec_.chars.count(id)) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.jobMove(out, id, jobId, index)] = "move job " + std::to_string(jobId);
	return send(out, "job move");
}

bool SimView::stop(bool hold)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty()) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.stop(out, selected_, hold);
	sent_[lastSentSeq_ = seq] = hold ? "hold" : "stop";
	tasked(selected_, false);
	if (!send(out, "stop")) { return false; }
	for (const uint64_t id : selected_) { keepOrder(id, seq, hold ? "hold" : "stop", 0, nullptr, false, false); }	// kenshi-4bps.1
	return true;
}

bool SimView::pause(bool paused)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.pause(out, paused)] = paused ? "pause" : "unpause";
	return send(out, "pause");
}

bool SimView::askItemStats(uint64_t id, uint32_t version, uint16_t index)
{
	if (dec_.resetSerial != statsReset_) { statsAsked_.clear(); statsReset_ = dec_.resetSerial; }
	if (mode_ != Live || !statsAsked_.insert({ id, version, index }).second) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.itemStats(out, id, version, index);
	sent_[lastSentSeq_ = seq] = "item stats";
	quiet_.insert(seq);
	return send(out, "item stats");
}

const simlink::SimLinkDecoder::ItemStats* SimView::itemStats(uint64_t id, uint32_t version, uint16_t index) const
{
	const auto it = dec_.itemStats.find({ id, version, index });
	return it != dec_.itemStats.end() ? &it->second : nullptr;
}

bool SimView::profileQuery()
{
	if (mode_ != Live || !profileStatus_.empty()) { return false; }	// a refusal is final for this session: the overlay says it once, no retry
	const auto now = std::chrono::steady_clock::now();
	if (profileAt_.time_since_epoch().count() != 0 && now - profileAt_ < std::chrono::milliseconds(1000)) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.profileQuery(out);
	profileAt_ = now;
	sent_[lastSentSeq_ = seq] = "profile query";
	quiet_.insert(seq);	// its applied answer is not news (the PROFILE frame is); a refusal is left to the overlay
	profileSent_.insert(seq);
	if (!send(out, "profile query")) { sent_.erase(seq); quiet_.erase(seq); profileSent_.erase(seq); profileAt_ = {}; return false; }
	return true;
}

const simlink::SimLinkDecoder::Profile* SimView::profile() const
{
	return dec_.profile.valid ? &dec_.profile : nullptr;
}

bool SimView::reply(const simlink::SimLinkDecoder::ReplyMenu& m, uint8_t option)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.reply(out, m.ch, m.speaker, m.seq, option)] = "reply " + std::to_string(option);
	return send(out, "reply");
}

bool SimView::endTalk(const simlink::SimLinkDecoder::ReplyMenu& m)
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.endTalk(out, m.ch, m.speaker)] = "end talk";
	return send(out, "end talk");
}

bool SimView::tradeClose()
{
	if (mode_ != Live) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.tradeClose(out)] = "trade close";
	return send(out, "trade close");
}

// The acting character is the first selected one (the order 60 frame's first id, whose actions the server runs the same way).
uint32_t SimView::menuQuery(uint64_t target)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty() || !target) { return 0; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.menuQuery(out, selected_.front(), target);
	sent_[lastSentSeq_ = seq] = "menu_query " + std::to_string(target);
	quiet_.insert(seq);	// a read: its applied answer is not news (a refusal still is)
	menuSent_.insert(seq);
	if (!send(out, "menu query")) { sent_.erase(seq); quiet_.erase(seq); menuSent_.erase(seq); return 0; }
	return seq;
}

bool SimView::setSpeed(float speed)
{
	if (mode_ != Live || !(speed > 0 && speed <= 64)) { return false; }
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.setSpeed(out, speed)] = "speed " + std::to_string(speed);
	return send(out, "speed");
}

bool SimView::sneakOn() const
{
	return !selected_.empty()
		&& std::all_of(selected_.begin(), selected_.end(), [&](uint64_t id)
		{
			const auto c = dec_.chars.find(id);
			return c != dec_.chars.end() && (c->second.cur.flags & simlink::kStealth);
		});
}

bool SimView::sneak()
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	if (mode_ != Live || selected_.empty()) { return false; }
	const bool on = !std::all_of(selected_.begin(), selected_.end(), [&](uint64_t id) { return dec_.chars.at(id).cur.flags & simlink::kStealth; });
	std::vector<uint8_t> out;
	sent_[lastSentSeq_ = orders_.toggle(out, selected_, 1, on)] = on ? "sneak on" : "sneak off";
	return send(out, "toggle");
}

bool SimView::follow(uint64_t leader)
{
	std::erase_if(selected_, [&](uint64_t id) { return !dec_.chars.count(id); });
	std::vector<uint64_t> ids;
	std::copy_if(selected_.begin(), selected_.end(), std::back_inserter(ids), [&](uint64_t id) { return id != leader; });
	if (mode_ != Live || ids.empty() || !leader) { return false; }
	std::vector<uint8_t> out;
	const uint32_t seq = orders_.follow(out, ids, leader);
	sent_[lastSentSeq_ = seq] = "follow " + std::to_string(leader);
	tasked(ids, false);
	if (!send(out, "follow")) { return false; }
	double at[3];
	const bool seen = wirePos(leader, at);
	for (const uint64_t id : ids) { keepOrder(id, seq, "follow", leader, seen ? at : nullptr, seen, false); }	// kenshi-4bps.1
	return true;
}

bool SimView::focus(double out[3]) const
{
	if (order_.empty()) { return false; }
	for (int k = 0; k < 3; k++) { out[k] = ground_[k]; }
	return true;
}

std::string SimView::status() const
{
	char line[200];
	snprintf(line, sizeof line, "simlink %s  tick %.1f / %llu  characters %zu  things %zu  nav tiles %zu  selected %zu%s%s", mode_ == File ? "replay" : "live", tick_,
		(unsigned long long)dec_.lastCompleteTick, order_.size(), dec_.things.size(), dec_.navTiles.size(), selected_.size(), error_.empty() ? "" : "  ERROR: ", error_.c_str());
	return reply_.empty() || !error_.empty() ? line : std::string(line) + "  | " + reply_;
}

bool SimView::lying(uint64_t id) const
{
	const auto s = slots_.find(id);
	return s != slots_.end() && s->second.index >= 0 && s->second.down == Lying && !s->second.rising;
}

uint64_t SimView::carrier(uint64_t id) const
{
	const auto s = slots_.find(id);
	if (s == slots_.end() || s->second.index < 0 || s->second.down != Carried) { return 0; }
	const auto c = dec_.chars.find(id);
	return c == dec_.chars.end() ? 0 : c->second.cur.carriedBy;
}

bool SimView::lyingUnbuilt(uint64_t id) const
{
	const auto s = slots_.find(id);
	return s != slots_.end() && s->second.index >= 0 && s->second.failed && s->second.failedDown == Lying && s->second.down != Lying;
}

// plans/animal-down: a race with no lying clip (every animal rig) leaves sl.down uncommitted (layer-null); once a ragdoll actually started for the
// character it carries the pose, so the state is committed. Never before: a committed Lying on a live character is the record-less layer BUG.
bool SimView::ragdollStarted(uint64_t id)
{
	const auto it = slots_.find(id);
	if (it == slots_.end()) { return false; }
	Slot& sl = it->second;
	if (sl.index < 0 || sl.down == Lying || !sl.failed || sl.failedDown != Lying) { return false; }
	sl.failed = false;
	sl.down = Lying;
	sl.ragdollHeld = true;
	charError("fallback", tick_, id, "the race's ragdoll carries the lying pose (no lying clip)");
	return true;
}

bool SimView::restPoint(uint64_t id, double pos[3], float& yaw) const
{
	const auto c = dec_.chars.find(id);
	if (c == dec_.chars.end() || !c->second.cur.hasRest) { return false; }	// CHAR_DYNAMIC flag bit 5: x, y, z are the rest point
	for (int k = 0; k < 3; k++) { pos[k] = c->second.cur.pos[k]; }
	yaw = c->second.cur.restYaw;
	return true;
}

std::string SimView::raceOf(uint64_t id) const
{
	const auto c = dec_.chars.find(id);
	return c == dec_.chars.end() ? std::string() : dec_.str(c->second.look.race);
}
