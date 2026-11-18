#include "trails.hpp"
#include "drawstats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <unistd.h>

#include "characters.hpp"
#include "config.hpp"
#include "simview.hpp"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "overlaylist.hpp"

// Debug trails (kenshi-k16.2.73; the rolling 30 s since k16.19.6.1): the user's foot tracking lines for the whole fight, with the character's centre plotted too. MINE: the
// decimation and cap, the pop rule's constants, and the two centre tracks as one sim (dashed) / drawn (solid) pair.
static const cfg::Bool kTrails{ cfg::Group{ "Trails" }, "debug.trails", false, "Debug: trails: every drawn character's feet, sim centre and drawn centre, floating in 3D (needs --debug)" };
// The bounded draw (k16.19.6.1; user 2026-11-12: "no matter the logging time, the actual points on screen shouldn't exceed ~30 seconds (probably)"):
// only the newest debug.trails_seconds of a track are drawn, in the trails' own sgl context. The history kept and the dump below are longer.
static const cfg::Float kTrailsSeconds{ cfg::Group{ "Trails" }, "debug.trails_seconds", 30, 1, Trails::kCapMax, "Debug trails: the rolling draw window in seconds (1-30): only the newest points are drawn; the kept history and the dump are longer" };
// Full-debug mode (k16.19.6.1): the points that drop out of memory go to segment files under the client's data home, so a long session keeps everything.
static const cfg::Bool kTrailsDump{ cfg::Group{ "Trails" }, "debug.trails_dump", false, "Debug trails: flush the history to disk in segments before the oldest kept points drop (a long session keeps everything; ~1 file a few minutes long)" };

static double draw_cap() { return std::clamp((double)kTrailsSeconds.get(), 1.0, Trails::kCapMax); }
double Trails::cap() { return draw_cap(); }

// Decimation (MINE): a point when the track moved over 0.5 u since the last kept one AND 0.05 s passed, or when 0.25 s passed (so an idle character
// still shows time). 0.5 u is under a pixel at the RTS camera and keeps a foot's swing readable; the 0.05 s floor holds a running fight to 20 points
// a second a track; 0.25 s bounds an idle one. With kMaxPoints this makes the worst case 64 x 4 x 640 points (~5 MB).
static constexpr float kKeepDist = 0.5f, kKeepMinDt = 0.05f, kKeepMaxDt = 0.25f;
// The pop rule (kenshi-k16.2.73, the lead's suggestion; per track since the review): a frame move over kJumpK x the faster of this track's own
// smoothed speed and the sim's, x dt, plus a floor. Both are per second, so the same rule holds at any frame rate. The track's own speed is the
// review's fix: a foot's stride swing is far faster than the body's, and the body yardstick flagged 181 L / 248 R foot points in one fight on an
// ordinary run. The old capped catch-up drew at 1.5x the sim speed and the display slop's walk is capped at max(100, 1.5x) u/s, so 2.5x plus the
// floor never flags either; a teleport or a snap does.
static constexpr double kJumpK = 2.5, kJumpMin = 4.0;
// The feet's floor is larger (the review: 181 / 248 foot warnings were too many to be signal). A change of animation clip snaps a foot to the new
// clip's pose in one frame: measured over a 12-character fight, 4 to 12.4 u with the poses frozen (scripted shots) and 5 to 9.9 u animating, so
// a 15 u floor sits just above them. A teleport is hundreds of units (the display slop snaps at 400 u), so the floor hides nothing worth seeing.
static constexpr double kJumpMinFoot = 15.0;
// A track's own speed is smoothed over this (s): long enough that one frame of a stride's swing does not lift the yardstick under it, short
// enough that a sustained burst (a hit, a shove) is not mistaken for a pop.
static constexpr double kRateTau = 0.25;

bool Trails::on() const { return kTrails.get(); }
void Trails::toggle() { kTrails.set(!kTrails.get()); }

// The trails' own sgl context (k16.19.6.1): the k16.19.6 bug was the default context filling with trail vertices, after which every later sgl command
// of the frame (the HUD text's background, the minimap, the name labels, the windows) was dropped silently. Sized for the whole draw window at the
// decimation's top rate and 64 characters: 64 x 4 x (cap x 20) x 6 + 64 x 4 x 4 vertices (the 6 is a pop's three 2-vertex lines) = 923,392 at the
// 30 s default, ~25 MB of sgl vertex buffer; the real draw is far smaller. The formats must match the overlay pass's (the default context's).
void Trails::initSgl(sg_pixel_format colour, sg_pixel_format depth, int samples)
{
	const int perTrack = (int)std::lround(draw_cap() * kTopRate);
	maxVertices_ = (int)kMaxChars * kTracks * perTrack * kVerticesPerPoint + (int)(kMaxChars * kTracks * 4);
	sgl_context_desc_t d = {};
	d.max_vertices = maxVertices_;
	d.color_format = colour;
	d.depth_format = depth;
	d.sample_count = samples;
	ctx_ = sgl_make_context(&d);
	sgl_set_context(ctx_);
	sg_pipeline_desc pd = {};	// the overlay's look (worldmap.cpp begin_overlay): alpha blending, no depth test
	pd.colors[0].blend.enabled = true;
	pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
	pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pip_ = sgl_make_pipeline(&pd);
	sgl_set_context(SGL_DEFAULT_CONTEXT);
}

Trails::~Trails() { dumpClose(); }
void Trails::shutdown() { dumpClose(); }

// -- the dump (debug.trails_dump): one plain-text line a dropped point, the columns the brief names (id, name, track, time, tick, x y z, pop), in
// segment files that sort in order: DIR/trails-<date>-<time>-<pid>-NNNN.tsv. A new segment every kSegmentPoints (250,000 points, ~22 MB).
static const char* const kKindNames[Trails::kTracks] = { "lfoot", "rfoot", "sim", "drawn" };

void Trails::dumpWrite(const Set& s, int track, const Pt& p)
{
	if (dumpDir_.empty() || !kTrailsDump.get()) { return; }
	if (!dump_ || dumpPoints_ >= kSegmentPoints)
	{
		dumpClose();
		std::error_code ec;
		std::filesystem::create_directories(dumpDir_, ec);
		if (dumpStamp_.empty())
		{
			const std::time_t now = std::time(nullptr);
			char stamp[64];
			std::strftime(stamp, sizeof stamp, "trails-%Y%m%d-%H%M%S", std::localtime(&now));
			dumpStamp_ = std::string(stamp) + "-" + std::to_string(getpid());
		}
		char name[96];
		snprintf(name, sizeof name, "%s-%04d.tsv", dumpStamp_.c_str(), ++dumpIndex_);
		const std::filesystem::path path = std::filesystem::path(dumpDir_) / name;
		dump_ = fopen(path.string().c_str(), "w");
		dumpPoints_ = 0;
		if (!dump_) { fprintf(stderr, "kclient: trails dump: cannot write %s\n", path.string().c_str()); return; }
		fputs("# id\tname\ttrack\tt\ttick\tx\ty\tz\tpop\n", dump_);
	}
	std::string name = s.name;
	for (char& ch : name) { if (ch == '\t' || ch == '\n' || ch == '\r') { ch = ' '; } }	// one row a line whatever the game's name
	fprintf(dump_, "%llu\t%s\t%s\t%.4f\t%.2f\t%.3f\t%.3f\t%.3f\t%d\n", (unsigned long long)s.id, name.c_str(), kKindNames[track], p.t, p.tick, p.p[0], p.p[1],
		p.p[2], p.warn ? 1 : 0);
	dumpPoints_++;
}

void Trails::dumpAll(const Set& s)
{
	if (dumpDir_.empty() || !kTrailsDump.get()) { return; }
	for (int k = 0; k < kTracks; k++) { for (const Pt& p : s.tr[k].pts) { dumpWrite(s, k, p); } }
}

void Trails::dumpClose()
{
	if (!dump_) { return; }
	fflush(dump_);
	fclose(dump_);
	dump_ = nullptr;
}

// A player character's kept span (kenshi-k16.2.231.1, the stuck-at-a-door shot): 60 s, or the whole time since its newest move_to order when that is
// longer, capped at 120 s. In sim ticks (the points' own clock, so a paused or scripted clock drops nothing either).
static double long_cut(double nowTick, uint64_t lastMoveTick)
{
	double span = std::max(Trails::kLongWindow, lastMoveTick != 0 && nowTick > (double)lastMoveTick ? nowTick - (double)lastMoveTick : 0.0);
	span = std::min(span, Trails::kLongMax);
	return nowTick - span * SimView::kTickRate;
}

void Trails::record(const SimView& sim, const Characters& chars, double dt)
{
	// A paused sim sends dt 0; record anyway (kenshi-h2v.23.1), but only where the drawn moved: nothing moves while paused, so a point here is
	// the bug, and t_ holds so the time gates below could only repeat the same point.
	const bool paused = dt <= 0;
	if (!paused) { t_ += dt; }
	for (const uint64_t id : sim.characterIds())
	{
		double drawn[3], simp[3];
		float hull[3];
		if (!sim.placed(id, drawn, hull) || !sim.simPos(id, simp)) { continue; }	// not drawn this frame
		Set* s = nullptr;
		for (Set& c : sets_) { if (c.id == id) { s = &c; break; } }
		if (!s)
		{
			if (sets_.size() >= kMaxChars)	// the least recently drawn goes (its trail too); the dump keeps its points (k16.19.6.1)
			{
				size_t oldest = 0;
				for (size_t i = 1; i < sets_.size(); i++) { if (sets_[i].lastT < sets_[oldest].lastT) { oldest = i; } }
				dumpAll(sets_[oldest]);
				sets_.erase(sets_.begin() + (long)oldest);
			}
			sets_.push_back(Set{});
			s = &sets_.back();
			s->id = id;
			const auto& dec = sim.decoder();
			const auto it = dec.chars.find(id);
			if (it != dec.chars.end()) { s->name = dec.str(it->second.look.name); }
		}
		s->lastT = t_;
		s->player = sim.isPlayer(id);	// kenshi-k16.2.231.1: a player character keeps the longer trail (60-120 s), the rest as before
		s->lastMoveTick = s->player ? sim.lastMoveTick(id) : 0;
		const double dsim = s->seen ? std::hypot(simp[0] - s->prevSim[0], simp[2] - s->prevSim[2]) : 0.0;
		// the points, world space: the two feet on their ground contact (the anim viewer's sole points, the lower of heel and ball a foot), and the
		// two centres at the body's mid height (feet + half the click box), so the sim / drawn offset shows as a horizontal step in 3D
		float pts[kTracks][3];
		double soles[2][2][3];
		const int index = sim.charIndex(id);
		if (index >= 0 && chars.solePoints((size_t)index, soles))
		{
			for (int f = 0; f < 2; f++)
			{
				const double* q = soles[f][1][1] <= soles[f][0][1] ? soles[f][1] : soles[f][0];	// the lower of ball and heel
				for (int k = 0; k < 3; k++) { pts[f][k] = (float)q[k]; }
			}
		}
		else
		{
			for (int f = 0; f < 2; f++) { for (int k = 0; k < 3; k++) { pts[f][k] = (float)drawn[k]; } }	// no body yet: the feet stay at the node
		}
		const float mid = hull[1] * 0.5f;
		pts[2][0] = (float)simp[0]; pts[2][1] = (float)simp[1] + mid; pts[2][2] = (float)simp[2];
		pts[3][0] = (float)drawn[0]; pts[3][1] = (float)drawn[1] + mid; pts[3][2] = (float)drawn[2];
		for (int k = 0; k < kTracks; k++)
		{
			Track& tr = s->tr[k];
			// the frame's move, then the pop rule: over kJumpK x the faster of this track's own smoothed speed and the sim's last frame, x dt,
			// plus the floor (a foot's is larger: clip switches snap a foot up to ~12 u). The track's own speed keeps a foot's stride swing (fast,
			// but its own) out of the warning; the sim's keeps a jittering foot from being a moving character. A character's first frame has no speed.
			const double m = tr.has ? std::hypot(std::hypot(pts[k][0] - tr.prev[0], pts[k][1] - tr.prev[1]), pts[k][2] - tr.prev[2]) : 0.0;
			// paused: no dt to scale a speed by, so the floor alone flags a move (the shimmer's snap cleared it; the hold's settle does not)
			const double thr = paused ? (k < 2 ? kJumpMinFoot : kJumpMin) : kJumpK * std::max((double)tr.rate, s->speed) * dt + (k < 2 ? kJumpMinFoot : kJumpMin);
			const bool warn = tr.has && m > thr;
			// a pop is not the track's speed: it does not enter the smoothing, or one teleport frame would lift the yardstick and hide the next
			if (!paused && tr.has && !warn) { tr.rate += (float)((m / dt - tr.rate) * std::min(1.0, dt / kRateTau)); }
			// keep: the first point, a warning (always, so the pop shows), a move past kKeepDist (at most every kKeepMinDt; paused: at once), or kKeepMaxDt
			const double since = t_ - tr.lastT;
			bool far = true;
			if (!tr.pts.empty())
			{
				const float* l = tr.pts.back().p;
				far = std::hypot(std::hypot(pts[k][0] - l[0], pts[k][1] - l[1]), pts[k][2] - l[2]) > kKeepDist;
			}
			if (tr.pts.empty() || warn || (far && (paused || since >= kKeepMinDt)) || (!paused && since >= kKeepMaxDt))
			{
				Pt pt;
				std::copy(pts[k], pts[k] + 3, pt.p);
				pt.t = t_;
				pt.tick = sim.tick();
				pt.warn = warn;
				tr.pts.push_back(pt);
				tr.lastT = t_;
				const size_t cap = s->player ? kMaxPointsLong : kMaxPoints;
				const double cut = s->player ? long_cut(sim.tick(), s->lastMoveTick) : 0;
				while (!tr.pts.empty() && (tr.pts.size() > cap || (s->player && tr.pts.front().tick < cut)))
				{
					dumpWrite(*s, k, tr.pts.front());	// the oldest drops; the dump writes it first (k16.19.6.1)
					tr.pts.pop_front();
				}
			}
			std::copy(pts[k], pts[k] + 3, tr.prev);
			tr.has = true;
		}
		s->prevSim[0] = simp[0]; s->prevSim[1] = simp[1]; s->prevSim[2] = simp[2];
		s->prevDrawn[0] = drawn[0]; s->prevDrawn[1] = drawn[1]; s->prevDrawn[2] = drawn[2];
		if (!paused) { s->speed = dsim / dt; }
		s->seen = true;
	}
}

static void hsv(float h, float s, float v, float out[3])
{
	const float c = v * s, x = c * (1 - std::fabs(std::fmod(h * 6, 2) - 1)), m = v - c;
	float r = 0, g = 0, b = 0;
	switch ((int)(h * 6) % 6)
	{
		case 0: r = c; g = x; break;
		case 1: r = x; g = c; break;
		case 2: g = c; b = x; break;
		case 3: g = x; b = c; break;
		case 4: r = x; b = c; break;
		default: r = c; b = x; break;
	}
	out[0] = r + m; out[1] = g + m; out[2] = b + m;
}

// The character's track hue: one colour per character, spread over green -> cyan -> blue -> violet so red, orange and magenta stay the warning's (the
// review: pink tracks read as the warning red at a glance, so the hues stop at violet). draw() and the panel's swatch share it.
static void char_hue(uint64_t id, float rgb[3]) { hsv(0.25f + 0.5f * (float)std::fmod((double)id * 0.61803398875, 1.0), 0.75f, 1.0f, rgb); }

// The trails' lines, played into their own context (k16.19.6.1) with the overlay pass's pixel setup, so the lines land where the default context's would.
// Render side, in the overlay pass: list is what draw() recorded on logic (render sitting 11, kenshi-k16.2.205.12.57).
void Trails::submit(const ovl::List& list, float width, float height)
{
	if (ctx_.id == SG_INVALID_ID) { return; }
	sgl_set_context(ctx_);
	sgl_defaults();
	sgl_load_pipeline(pip_);
	sgl_viewport(0, 0, (int)width, (int)height, true);
	sgl_matrix_mode_projection();
	sgl_ortho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
	lastListBytes_ = list.calls().size() * sizeof(ovl::Call) + list.text().size();	// the overlay list's size this frame (kenshi-k16.2.205.12.59)
	lastCalls_ = list.calls().size();
	ovl::play(list);
	lastVertices_ = sgl_num_vertices();	// what this frame's draw recorded (the budget check, k16.19.6.1)
	sgl_set_context(SGL_DEFAULT_CONTEXT);	// the rest of the overlay records into the default context as before
}

// Logic side: the trails' lines through ovl:: (record_overlay binds the packet's trails list), projected with mvp in width x height pixels.
void Trails::draw(const float mvp[16], const double cam[3], float width, float height)
{
	auto project = [&](const float* p, float& sx, float& sy)
	{
		const float v[3] = { p[0] - (float)cam[0], p[1] - (float)cam[1], p[2] - (float)cam[2] };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * v[0] + mvp[4 + i] * v[1] + mvp[8 + i] * v[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { return false; }
		sx = (c[0] / c[3] * 0.5f + 0.5f) * width;
		sy = (0.5f - c[1] / c[3] * 0.5f) * height;
		return true;
	};
	// The drawn window and the points drawn (k16.19.6.1): the newest win seconds of a track, and at most the cap's top-rate count (the context is
	// sized for kMaxChars of that, so a full history can never fill it; a live cap raised past the startup one stays inside the context's own allowance).
	const double cap = draw_cap();
	const double win = window_ > 0 ? std::min(window_, cap) : cap;	// window 0 = the cap now, not the whole fight
	const double cut = t_ - win;
	const size_t perTrack = std::min((size_t)std::llround(cap * kTopRate),
		maxVertices_ > 0 ? (size_t)((maxVertices_ - (long)kMaxChars * kTracks * 4) / ((long)kMaxChars * kTracks * kVerticesPerPoint)) : (size_t)std::llround(cap * kTopRate));
	ovl::disable_texture();
	ovl::begin_lines();
	for (const Set& s : sets_)
	{
		if (pick_ == Pick::One && s.id != pickId_) { continue; }
		if (pick_ == Pick::Name && s.name != pickName_) { continue; }
		float rgb[3];
		char_hue(s.id, rgb);
		for (int k = 0; k < kTracks; k++)
		{
			const Track& tr = s.tr[k];
			// feet dim, centres bright; the sim centre dashed so a coinciding drawn centre still reads and an offset shows both
			const float alpha = k < 2 ? (k == 0 ? 0.55f : 0.35f) : (k == 2 ? 0.85f : 1.0f);
			const bool dash = k == 2;
			// start at the first point inside the window, and never more than perTrack points (the newest ones)
			size_t start = 0;
			while (start < tr.pts.size() && tr.pts[start].t < cut) { start++; }
			if (tr.pts.size() - start > perTrack) { start = tr.pts.size() - perTrack; }
			float px = 0, py = 0;
			bool prevOk = false;
			size_t seg = start;	// the dash phase follows the point index, as before
			for (size_t at = start; at < tr.pts.size(); at++)
			{
				const Pt& p = tr.pts[at];
				float sx, sy;
				const bool ok = project(p.p, sx, sy);
				if (prevOk && ok && !(dash && !p.warn && (seg & 1)))
				{
					if (p.warn)	// the pop: warning orange-red, three lines 0.7 px apart so it is thicker than any character track at a glance
					{
						ovl::c4f(1.0f, 0.3f, 0.15f, 0.95f);
						const float dxs = sx - px, dys = sy - py, lens = std::sqrt(dxs * dxs + dys * dys);
						const float wx = lens > 1e-3f ? -dys / lens * 0.7f : 0.0f, wy = lens > 1e-3f ? dxs / lens * 0.7f : 0.7f;
						for (int o = -1; o <= 1; o++)
						{
							ovl::v2f(px + wx * (float)o, py + wy * (float)o);
							ovl::v2f(sx + wx * (float)o, sy + wy * (float)o);
						}
					}
					else
					{
						ovl::c4f(rgb[0], rgb[1], rgb[2], alpha);
						ovl::v2f(px, py);
						ovl::v2f(sx, sy);
					}
				}
				px = sx; py = sy;
				prevOk = ok;
				seg++;
			}
			// the current point: a small 3D cross, so the head of every track reads
			if (!tr.pts.empty() && project(tr.pts.back().p, px, py))
			{
				ovl::c4f(rgb[0], rgb[1], rgb[2], std::min(1.0f, alpha + 0.15f));
				const float* q = tr.pts.back().p;
				const float a[3] = { q[0] - 0.8f, q[1], q[2] }, b[3] = { q[0] + 0.8f, q[1], q[2] };
				const float c[3] = { q[0], q[1], q[2] - 0.8f }, d[3] = { q[0], q[1], q[2] + 0.8f };
				float ax, ay, bx, by, cx, cy, dx, dy;
				if (project(a, ax, ay) && project(b, bx, by)) { ovl::v2f(ax, ay); ovl::v2f(bx, by); }
				if (project(c, cx, cy) && project(d, dx, dy)) { ovl::v2f(cx, cy); ovl::v2f(dx, dy); }
			}
		}
	}
	ovl::count_lines();
	ovl::end();
}

bool Trails::op(const std::vector<std::string>& a, std::string& reply)
{
	if (a.empty()) { reply = "trails: all | id=N | name NAME... | window SECONDS | clear | off"; return false; }
	auto count = [&]()
	{
		size_t n = 0, pts = 0;
		for (const Set& s : sets_) { n++; for (const Track& t : s.tr) { pts += t.pts.size(); } }
		char buf[128];
		snprintf(buf, sizeof buf, "%zu characters, %zu points", n, pts);
		return std::string(buf);
	};
	if (a[0] == "all")
	{
		kTrails.set(true);
		pick_ = Pick::All;
		reply = "trails: all, " + count();
		return true;
	}
	if (a[0] == "off") { kTrails.set(false); reply = "trails: off (" + count() + " kept)"; return true; }
	if (a[0] == "clear")
	{
		for (Set& s : sets_)
		{
			dumpAll(s);	// the dump keeps the history a clear drops (k16.19.6.1)
			for (Track& t : s.tr) { t.pts.clear(); t.has = false; t.lastT = 0; }
			s.prevSim[0] = s.prevSim[1] = s.prevSim[2] = 0;
			s.prevDrawn[0] = s.prevDrawn[1] = s.prevDrawn[2] = 0;
			s.speed = 0;
			s.seen = false;
		}
		t_ = 0;
		reply = "trails: cleared";
		return true;
	}
	if (a[0] == "window" && a.size() == 2)
	{
		// a view filter, never above the cap (k16.19.6.1): 0 = the cap, debug.trails_seconds
		const double cap = draw_cap();
		const double want = std::max(0.0, std::atof(a[1].c_str()));
		window_ = std::min(want, cap);
		char buf[160];
		snprintf(buf, sizeof buf, "trails: window %g s of the %g s cap%s, %s", window_ > 0 ? window_ : cap, cap,
			want > cap ? " (clamped)" : "", count().c_str());
		reply = buf;
		return true;
	}
	std::string idArg;
	if (a.size() == 2 && a[0] == "id") { idArg = "id=" + a[1]; }
	else if (a.size() == 1 && a[0].rfind("id=", 0) == 0) { idArg = a[0]; }
	if (idArg.size() > 3)
	{
		kTrails.set(true);
		pick_ = Pick::One;
		pickId_ = std::strtoull(idArg.c_str() + 3, nullptr, 10);
		reply = "trails: id " + std::to_string(pickId_) + ", " + count();
		return true;
	}
	if (a[0] == "name" && a.size() >= 2)
	{
		pickName_ = a[1];
		for (size_t i = 2; i < a.size(); i++) { pickName_ += " " + a[i]; }	// a name may hold spaces
		kTrails.set(true);
		pick_ = Pick::Name;
		reply = "trails: name \"" + pickName_ + "\", " + count();
		return true;
	}
	reply = "trails: all | id=N | name NAME... | window SECONDS | clear | off";
	return false;
}

std::string Trails::status() const
{
	size_t n = 0, pts = 0, warn[kTracks] = {};
	const double cap = draw_cap();
	const double win = window_ > 0 ? std::min(window_, cap) : cap;
	const double cut = t_ - win;
	for (const Set& s : sets_)
	{
		n++;
		for (int k = 0; k < kTracks; k++)
		{
			for (const Pt& p : s.tr[k].pts)
			{
				if (p.t < cut) { continue; }	// the window's view of the points
				pts++;
				warn[k] += p.warn ? 1 : 0;	// each warning point is one drawn warning segment
			}
		}
	}
	std::string pick = pick_ == Pick::All ? "all" : pick_ == Pick::One ? "id " + std::to_string(pickId_) : "name " + pickName_;
	char buf[320];
	snprintf(buf, sizeof buf, "trails: %s, %s, window %g s of the %g s cap, %zu characters, %zu points; warnings L %zu R %zu sim %zu drawn %zu; last draw %d of %d vertices; overlay list %zu calls, %zu bytes",
		on() ? "on" : "off", pick.c_str(), win, cap, n, pts, warn[0], warn[1], warn[2], warn[3], lastVertices_, maxVertices_, lastCalls_, lastListBytes_);
	return buf;
}

std::vector<Trails::Row> Trails::rows() const
{
	std::vector<Row> out;
	out.reserve(sets_.size());
	for (const Set& s : sets_)
	{
		Row r;
		r.id = s.id;
		r.name = s.name;
		for (const Track& t : s.tr)
		{
			r.points += (uint32_t)t.pts.size();
			for (const Pt& p : t.pts) { r.pops += p.warn ? 1 : 0; }
		}
		out.push_back(std::move(r));
	}
	return out;
}

std::string Trails::colour(uint64_t id) const
{
	float rgb[3];
	char_hue(id, rgb);
	char b[8];
	snprintf(b, sizeof b, "#%02x%02x%02x", (int)std::lround(rgb[0] * 255.0f), (int)std::lround(rgb[1] * 255.0f), (int)std::lround(rgb[2] * 255.0f));
	return b;
}

uint64_t Trails::picked() const
{
	if (pick_ == Pick::One) { return pickId_; }
	if (pick_ == Pick::Name)
	{
		for (const Set& s : sets_) { if (s.name == pickName_) { return s.id; } }	// a name pick: its character, once it is recorded
	}
	return 0;
}

bool Trails::writeFile(const std::string& path) const
{
	FILE* f = fopen(path.c_str(), "w");
	if (!f) { return false; }
	fputs("id\tname\tt\ttick\tkind\tx\ty\tz\twarn\n", f);	// the columns by name (k16.2.73 review): one row a kept point
	static const char* const kKind[kTracks] = { "lfoot", "rfoot", "sim", "drawn" };
	for (const Set& s : sets_)
	{
		std::string name = s.name;
		for (char& ch : name) { if (ch == '\t' || ch == '\n' || ch == '\r') { ch = ' '; } }	// one row a line whatever the game's name
		for (int k = 0; k < kTracks; k++)
		{
			for (const Pt& p : s.tr[k].pts)	// every kept point of the whole fight, not the view window
			{
				fprintf(f, "%llu\t%s\t%.4f\t%.2f\t%s\t%.3f\t%.3f\t%.3f\t%d\n", (unsigned long long)s.id, name.c_str(), p.t, p.tick, kKind[k], p.p[0], p.p[1], p.p[2],
					p.warn ? 1 : 0);
			}
		}
	}
	const bool ok = ferror(f) == 0;
	fclose(f);
	return ok;
}
