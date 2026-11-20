#include "frame/beamfield.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
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

uint64_t hashKey(const std::string& group, uint32_t id)
{
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char c : group) { h ^= c; h *= 1099511628211ull; }
	h ^= id;
	h *= 1099511628211ull;
	return h;
}
}

void BeamField::clear()
{
	beams_.clear();
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

void BeamField::systems(double tick, const kworld::Heightmap* hm, std::vector<Particles::PlacedSystem>& out) const
{
	for (const Beam& e : beams_)
	{
		const double spawn = (double)e.b.spawnTick, end = (double)std::max(e.b.spawnTick, e.b.endTick);
		if (tick < spawn) { continue; }			// the lead: nothing is drawn before the strike (the original has no wind-up visual)
		if (tick >= end + kParticleTailTicks) { continue; }	// the particles have finished
		float x, z;
		pointAt(e.b, std::min(tick, end), x, z);	// after endTick the beam is gone: the system waits at its last point while the particles die
		Particles::PlacedSystem p;
		p.key = e.key;
		p.script = e.particles;
		p.pos[0] = x;
		p.pos[1] = hm ? hm->terrainHeight(x, z) : 0.0;
		p.pos[2] = z;
		p.seed = (uint32_t)(e.key ^ (e.key >> 32));
		p.amount = 1;		// the script's own emission rate (the original's psys is not scaled by the damage strength)
		p.prewarm = false;	// an event: play from t = 0, no pre-warm that would eat the one-shot emitter
		out.push_back(std::move(p));
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
		const char* state = tick < (double)e.b.spawnTick ? "wind-up" : tick < (double)e.b.endTick ? "live" : "tail";
		snprintf(buf, sizeof buf, "  id %u (%s) group \"%s\" spawn %llu end %llu in %+.2f s point %.0f %.0f script \"%s\" radius %.0f strength %.2f\n",
			e.b.id, state, e.group.c_str(), (unsigned long long)e.b.spawnTick, (unsigned long long)e.b.endTick,
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
		const double end = (double)std::max(e.b.spawnTick, e.b.endTick);
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
		f.state = tick < (double)e.b.endTick ? 1 : 2;
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
