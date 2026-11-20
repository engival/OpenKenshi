#include "frame/beamfield.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "kworld/heightmap.hpp"

namespace
{
// SIMLINK "BEAMS": one tick moves speed / 20 u along the segment's (sin yaw, cos yaw), the character yaw convention (0 = +Z, positive toward +X).
void step(float& x, float& z, float speed, float yaw)
{
	const float length = speed / 20.0f;
	x += std::sin(yaw) * length;
	z += std::cos(yaw) * length;
}

void pointAt(const simlink::Beam& b, double tick, float& x, float& z)
{
	x = b.x;
	z = b.z;
	const double n = tick > (double)b.spawnTick ? tick - (double)b.spawnTick : 0.0;
	const uint64_t full = (uint64_t)std::floor(n);
	const float frac = (float)(n - (double)full);
	uint64_t left = full;
	size_t si = 0;
	for (; si < b.segments.size() && left >= b.segments[si].ticks; si++)
	{
		for (uint16_t k = 0; k < b.segments[si].ticks; k++) { step(x, z, b.speed, b.segments[si].yaw); }
		left -= b.segments[si].ticks;
	}
	if (left > 0 && si < b.segments.size())
	{
		for (uint64_t k = 0; k < left; k++) { step(x, z, b.speed, b.segments[si].yaw); }
		left = 0;
	}
	if (frac > 0 && si < b.segments.size())	// between ticks: the segment that covers the next tick
	{
		x += std::sin(b.segments[si].yaw) * (b.speed / 20.0f) * frac;
		z += std::cos(b.segments[si].yaw) * (b.speed / 20.0f) * frac;
	}
}

// The tick the beam stops being seen: spawnTick + visibleTicks (the sim's burst, h2v.14.11), at most endTick; visibleTicks 0 = its whole life.
double burstEnd(const simlink::Beam& b)
{
	const uint64_t end = std::max(b.spawnTick, b.endTick);
	return (double)(b.visibleTicks ? std::min<uint64_t>(b.spawnTick + b.visibleTicks, end) : end);
}

uint64_t hashKey(const std::string& group, uint32_t id)
{
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char c : group) { h ^= c; h *= 1099511628211ull; }
	h ^= id;
	h *= 1099511628211ull;
	return h;
}

// The original's zone grid (kenshi-rwh.211, the accessor 1408f56b0): 64 x 64 cells of 4608 u; cell = clamp(int((x + 147465) / 4608), 0, 63) per
// axis (x and z). The same grid as the zone records and the biome sectors.
constexpr double kCellSize = 4608.0, kGridOrigin = -147465.0;
constexpr int kCellMax = 63;
// The focus cell re-centres only when the camera is more than 0.9 cell (4147.2 u, DAT_14174a140) from the focus cell's centre per axis (140a0f820).
constexpr double kFocusRecentre = 4147.2;
// The +-340 u box round a player character whose cells are loaded (140a0f640, DAT_14174a100). The "Fast zone hopping" radius (DAT_142133518) is 0
// in the default config (assumed: rwh.211 did not read it), so just the cells that box touches, no block round each character.
constexpr double kPlayerBox = 340.0;
int cellOf(double v) { return std::clamp((int)((v - kGridOrigin) / kCellSize), 0, kCellMax); }

// SkyBeam.pu: the beam is a 200 x 16 000 u additive strip standing on its ground point.
constexpr double kColumnHeight = 16000.0, kColumnRadius = 100.0;
// The camera's 4 side clip planes in camera-relative space (Gribb-Hartmann: row 3 + s x row 0 or 1 of the camera-relative projection x view,
// normalised). The near and far planes are the forward distance (Camera's nearZ / farZ), so the backend's z clip range does not matter.
void viewPlanes(const Camera& cam, float planes[4][4])
{
	float m[16];
	cam.mvp(m);
	for (int k = 0; k < 4; k++)
	{
		const int axis = k >> 1;				// 0 x, 1 y
		const float s = (k & 1) ? -1.0f : 1.0f;	// the positive plane, then the negative one
		for (int c = 0; c < 4; c++) { planes[k][c] = m[c * 4 + 3] + s * m[c * 4 + axis]; }
		const float l = std::sqrt(planes[k][0] * planes[k][0] + planes[k][1] * planes[k][1] + planes[k][2] * planes[k][2]);
		if (l > 0) { for (int c = 0; c < 4; c++) { planes[k][c] /= l; } }
	}
}

// A beam's column against the view: the strip from its ground point up kColumnHeight, sampled (a column taller than the screen is seen where its
// ends alone are not) with the strip's half width as the margin. Only the inview statistic (KBEAMSLOG); the draw set does not use it.
bool columnInView(const float planes[4][4], const Camera& cam, double x, double z, double foot)
{
	constexpr int kSamples = 64;
	for (int i = 0; i <= kSamples; i++)
	{
		const double y = foot + kColumnHeight * (double)i / kSamples;
		const double rel[3] = { x - cam.pos[0], y - cam.pos[1], z - cam.pos[2] };
		const double depth = rel[0] * cam.f[0] + rel[1] * cam.f[1] + rel[2] * cam.f[2];
		if (depth < cam.nearZ - kColumnRadius || depth > cam.farZ + kColumnRadius) { continue; }
		bool inside = true;
		for (int p = 0; p < 4 && inside; p++) { inside = planes[p][0] * rel[0] + planes[p][1] * rel[1] + planes[p][2] * rel[2] + planes[p][3] >= -kColumnRadius; }
		if (inside) { return true; }
	}
	return false;
}
}

void BeamField::clear()
{
	beams_.clear();
	state_.clear();
	focusX_ = focusZ_ = -1;
	loaded_.clear();
	serial_ = 0;
}

bool BeamField::sync(const simlink::SimLinkDecoder& d)
{
	if (d.beamsSerial == serial_) { return false; }
	serial_ = d.beamsSerial;
	beams_.clear();
	for (const simlink::BeamGroup& g : d.beamGroups)
	{
		const std::string group = d.str(g.group);
		for (const simlink::Beam& b : g.beams)
		{
			// The particle tail is over: the client can forget it (a beam is sent once per session and never updated after it ends).
			if ((double)b.endTick + kParticleTailTicks <= (double)d.beamsTick) { continue; }
			Beam e;
			e.group = group;
			e.particles = d.str(b.particles);
			e.key = hashKey(group, b.id);
			e.b = b;
			beams_.push_back(std::move(e));
		}
	}
	return true;
}

void BeamField::cells(const double cam[3], const std::vector<std::array<double, 3>>& players)
{
	// The focus is the cell under the camera (the camera rig node in the original, not its selection), re-centred only past the 0.9-cell
	// hysteresis: the block is square and does not follow every step of the camera (rwh.211, 140a0f820).
	const int cx = cellOf(cam[0]), cz = cellOf(cam[2]);
	if (focusX_ < 0) { focusX_ = cx; focusZ_ = cz; }
	else
	{
		const double fx = kGridOrigin + ((double)focusX_ + 0.5) * kCellSize, fz = kGridOrigin + ((double)focusZ_ + 0.5) * kCellSize;
		if (std::fabs(cam[0] - fx) > kFocusRecentre || std::fabs(cam[2] - fz) > kFocusRecentre) { focusX_ = cx; focusZ_ = cz; }
	}
	loaded_.clear();
	for (int x = std::max(focusX_ - 1, 0); x <= std::min(focusX_ + 1, kCellMax); x++)
	{
		for (int z = std::max(focusZ_ - 1, 0); z <= std::min(focusZ_ + 1, kCellMax); z++) { loaded_.push_back(x * 64 + z); }
	}
	// Player-faction characters keep their own cells loaded (140a0f640): the cells their +-340 u box touches also count for the gate, so a squad
	// far from the camera keeps its beams.
	for (const std::array<double, 3>& p : players)
	{
		for (int x = cellOf(p[0] - kPlayerBox); x <= cellOf(p[0] + kPlayerBox); x++)
		{
			for (int z = cellOf(p[2] - kPlayerBox); z <= cellOf(p[2] + kPlayerBox); z++)
			{
				const int idx = x * 64 + z;
				if (std::find(loaded_.begin(), loaded_.end(), idx) == loaded_.end()) { loaded_.push_back(idx); }
			}
		}
	}
	stats_.focusX = focusX_;
	stats_.focusZ = focusZ_;
	stats_.cells = loaded_.size();
}

bool BeamField::cellLoaded(double x, double z) const
{
	return std::find(loaded_.begin(), loaded_.end(), cellOf(x) * 64 + cellOf(z)) != loaded_.end();
}

void BeamField::systems(double tick, const kworld::Heightmap* hm, const Camera& cam, const std::vector<std::array<double, 3>>& players, int maxBeams, bool gate, std::vector<Particles::PlacedSystem>& out)
{
	const double* camPos = cam.pos;
	// The beams in their burst, ranked by horizontal distance to the camera (the column is 16 000 u tall: height does not matter).
	struct Cand { const Beam* e; float x, z; double d; bool in, open; };
	std::vector<Cand> cand;
	stats_ = {};
	cells(camPos, players);
	std::vector<uint64_t> seen;
	for (const Beam& e : beams_)
	{
		stats_.wire++;
		if (std::find(seen.begin(), seen.end(), e.key) != seen.end()) { stats_.dup++; } else { seen.push_back(e.key); }
		if (tick >= (double)e.b.endTick + kParticleTailTicks) { stats_.stale++; }
		const double spawn = (double)e.b.spawnTick, end = burstEnd(e.b);
		if (tick < spawn) { continue; }			// the lead: nothing is drawn before the strike (the original has no wind-up visual)
		// After the burst the system leaves the list: with `outlive` it stops emitting and its particles die out where they are (the
		// tail, measured from the burst's end). Nothing is placed from then on; the decoder keeps the beam to endTick for the damage.
		if (tick >= end) { continue; }
		Cand c{ &e, 0, 0, 0, false, false };
		pointAt(e.b, tick, c.x, c.z);
		c.d = std::hypot(c.x - camPos[0], c.z - camPos[2]);
		// The original's gate: only a beam whose own zone cell is loaded is drawn (kenshi-k16.2.409.3, rwh.211, 140100ba0). A beam it shuts out
		// leaves the drawn set through the same 1 s fade as one the cap drops, never pops.
		c.open = !gate || cellLoaded(c.x, c.z);
		const auto it = state_.find(e.key);
		c.in = it != state_.end() && it->second.in;
		cand.push_back(c);
	}
	stats_.burst = cand.size();
	for (const Cand& c : cand) { if (c.d <= 3000.0) { stats_.burstNear++; } }
	float vplanes[4][4];
	viewPlanes(cam, vplanes);
	// the sim's bursting beams whose column is in the frustum, whatever the cap or gate says (KBEAMSLOG burstview), and those the gate lets in
	for (const Cand& c : cand)
	{
		const double f = hm ? hm->terrainHeight(c.x, c.z) : 0.0;
		if (columnInView(vplanes, cam, c.x, c.z, f)) { stats_.burstView++; if (c.open) { stats_.burstOpenView++; } }
	}
	std::stable_sort(cand.begin(), cand.end(), [](const Cand& a, const Cand& b) { return a.d < b.d; });
	// The drawn set: at most maxBeams among the beams the gate lets in. A beam already in it stays until it is clearly beyond the X-th nearest
	// (kHysteresis); a beam joins only into a free slot, so the set never flaps at the boundary. The X-th nearest is the X-th of the open ones.
	const size_t X = maxBeams > 0 ? (size_t)maxBeams : 0;
	double limit = 1e300;
	if (X > 0)
	{
		size_t k = 0;
		for (const Cand& c : cand) { if (c.open && ++k == X) { limit = c.d * kHysteresis; break; } }
	}
	std::vector<char> pick(cand.size(), 0);
	size_t n = 0;
	for (size_t i = 0; i < cand.size() && n < X; i++) { if (cand[i].open && cand[i].in && cand[i].d <= limit) { pick[i] = 1; n++; } }
	for (size_t i = 0; i < cand.size() && n < X; i++) { if (cand[i].open && !pick[i]) { pick[i] = 1; n++; } }
	float planes[4][4];
	viewPlanes(cam, planes);
	std::unordered_map<uint64_t, State> next;
	for (size_t i = 0; i < cand.size(); i++)
	{
		const Beam& e = *cand[i].e;
		State st;
		const auto it = state_.find(e.key);
		if (it != state_.end()) { st = it->second; }
		else { st.fade = tick - (double)e.b.spawnTick < 2.0 && pick[i] ? 1.0f : 0.0f; st.last = tick; }	// a beam seen at its strike appears as the script's own curve does; one that joins by rank fades in
		const double dt = std::clamp(tick - st.last, 0.0, (double)kFadeTicks);
		st.last = tick;
		st.in = pick[i] != 0;
		st.fade = std::clamp(st.fade + (float)(dt / kFadeTicks) * (st.in ? 1.0f : -1.0f), 0.0f, 1.0f);
		const double foot = hm ? hm->terrainHeight(cand[i].x, cand[i].z) : 0.0;
		if (st.in)
		{
			stats_.drawn++;
			if (cand[i].d <= 3000.0) { stats_.drawnNear++; }
			stats_.drawnFar = std::max(stats_.drawnFar, cand[i].d);
			if (stats_.drawnNearest < 0 || cand[i].d < stats_.drawnNearest) { stats_.drawnNearest = cand[i].d; }
			if (columnInView(planes, cam, cand[i].x, cand[i].z, foot)) { stats_.inview++; viewSeen_.insert(e.key * 1000003ull + e.b.spawnTick); }	// the ask's "visible at once" (rwh.175: 0-2)
		}
		if (st.fade <= 0.0f && !st.in) { continue; }	// gone: its particles run out (outlive)
		if (!st.in) { stats_.fading++; }
		next[e.key] = st;
		Particles::PlacedSystem p;
		p.key = e.key;
		p.script = e.particles;
		p.pos[0] = cand[i].x;
		p.pos[1] = foot;
		p.pos[2] = cand[i].z;
		p.seed = (uint32_t)(e.key ^ (e.key >> 32));
		p.amount = 1;		// the script's own emission rate (the original's psys is not scaled by the damage strength)
		p.fade = st.fade;
		p.outlive = true;	// leaving the list at the burst's end stops the emission, the particles finish on their own
		p.prewarm = false;	// an event: play from t = 0, no pre-warm that would eat the one-shot emitter
		out.push_back(std::move(p));
	}
	state_ = std::move(next);
}

void BeamField::fadeLog(double tick, long frame) const
{
	for (const auto& kv : state_)
	{
		if (kv.second.fade > 0.0f && kv.second.fade < 1.0f) { fprintf(stderr, "beamfade frame %ld tick %.2f key %016llx %s fade %.3f\n", frame, tick, (unsigned long long)kv.first, kv.second.in ? "join" : "leave", kv.second.fade); }
	}
}

std::vector<std::string> BeamField::scripts() const
{
	std::vector<std::string> out;
	for (const Beam& e : beams_)
	{
		if (e.particles.empty()) { continue; }
		if (std::find(out.begin(), out.end(), e.particles) == out.end()) { out.push_back(e.particles); }
	}
	return out;
}

std::string BeamField::dump(double tick) const
{
	std::string out;
	char buf[320];
	snprintf(buf, sizeof buf, "beams: %zu held, render tick %.2f\n", beams_.size(), tick);
	out += buf;
	for (const Beam& e : beams_)
	{
		float x, z;
		pointAt(e.b, tick, x, z);
		const double bend = burstEnd(e.b);
		const char* state = tick < (double)e.b.spawnTick ? "wind-up" : tick < bend ? "burst" : "after-burst";
		const char* dmg = tick < (double)e.b.spawnTick ? "pending" : tick < (double)e.b.endTick ? "live" : tick < (double)e.b.endTick + kParticleTailTicks ? "tail" : "over";
		snprintf(buf, sizeof buf, "  id %u (%s, damage %s, burst %u ticks) group \"%s\" spawn %llu end %llu in %+.2f s point %.0f %.0f script \"%s\" radius %.0f strength %.2f\n",
			e.b.id, state, dmg, (unsigned)e.b.visibleTicks, e.group.c_str(), (unsigned long long)e.b.spawnTick, (unsigned long long)e.b.endTick,
			((double)e.b.spawnTick - tick) / 20.0, x, z, e.particles.c_str(), e.b.radius, e.b.strength);
		out += buf;
	}
	return out;
}

size_t BeamField::live(double tick) const
{
	size_t n = 0;
	for (const Beam& e : beams_) { n += tick >= (double)e.b.spawnTick && tick < (double)e.b.endTick ? 1 : 0; }
	return n;
}

std::vector<BeamField::Foot> BeamField::feet(double tick) const
{
	std::vector<Foot> out;
	out.reserve(beams_.size());
	for (const Beam& e : beams_)
	{
		const double end = burstEnd(e.b);
		if (tick < (double)e.b.spawnTick) { continue; }			// the lead draws nothing (systems())
		if (tick >= end + (double)kParticleTailTicks) { continue; }	// the particles are over
		Foot f;
		f.key = e.key;
		f.id = e.b.id;
		f.spawnTick = e.b.spawnTick;
		f.endTick = e.b.endTick;
		f.particles = e.particles;
		// the same point systems() places the system at: the live point, held at endTick while the tail plays out
		pointAt(e.b, std::min(tick, end), f.x, f.z);
		f.state = tick < end ? 1 : 2;
		out.push_back(std::move(f));
	}
	return out;
}

bool BeamField::aim(double tick, uint32_t id, int phase, const double cam[3], float& x, float& z, Aim* out)
{
	if (phase == 3) { id = lastAim_; }	// `same`: the beam the last call picked, so a shot pair watches one wind-up and strike
	const Beam* pick = nullptr;
	for (const Beam& e : beams_)
	{
		if (id && e.b.id != id) { continue; }
		const double lead = (double)e.b.spawnTick - tick, ttl = (double)e.b.endTick - (double)e.b.spawnTick;
		// wind-up: 0.6-3 s out (the most notice first) with a TTL that outlives the strike shot; a strike: 5-7 s in, when the strip's
		// particles reach their white (SkyBeam.pu time_colour 0.46-0.75 of a 10 s life)
		if (phase == 1 && !(lead > 12.0 && lead <= 60.0 && ttl >= 200.0)) { continue; }
		if (phase == 2 && !(lead < -100.0 && lead > -140.0 && (double)e.b.endTick - tick > 100.0)) { continue; }
		if (!pick) { pick = &e; continue; }
		if (id) { break; }
		// no id: a beam in its lead wins over a live one (of those, the one announced longest ago: the shot pair wants the full wind-up);
		// among equals the nearest wins
		const auto rank = [&](const Beam& c) {
			const bool lead = tick < (double)c.b.spawnTick;
			float cx, cz;
			pointAt(c.b, tick, cx, cz);
			const double d = (cx - cam[0]) * (cx - cam[0]) + (cz - cam[2]) * (cz - cam[2]);
			return std::tuple<int, double, double>{ lead ? 0 : 1, lead ? tick - (double)c.b.spawnTick : 0.0, d };
		};
		if (rank(e) < rank(*pick)) { pick = &e; }
	}
	if (!pick) { return false; }
	pointAt(pick->b, tick, x, z);
	lastAim_ = pick->b.id;
	if (out) { out->id = pick->b.id; out->spawnTick = pick->b.spawnTick; out->endTick = pick->b.endTick; }
	return true;
}
