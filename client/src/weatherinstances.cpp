#include "weatherinstances.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "weathermap.hpp"
#include "kworld/heightmap.hpp"

namespace
{
constexpr double kDrawRadius = 2000.0;	// the reach when an EFFECT has no view distance (and the cap on it): the sim's interest radius (SimView::kInterestRadius)
constexpr double kFadeIn = 2.0;		// game seconds for an instance to reach full strength after spawn
constexpr double kFadeOut = 3.0;	// game seconds: an instance fades over the last kFadeOut of its TTL
constexpr double kChangeFade = 3.0;	// game seconds: the previous weather's instances fade out after a weather change

uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull)
{
	for (const unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
	return h;
}
}

void WeatherInstances::clear()
{
	groups_.clear();
	olds_.clear();
	weather_.clear();
	serial_ = 0;
}

// A new weather: its effects become the groups. The anchored classes are drawn by Particles::setEffects (types 1/5/6 camera, 4 the player
// volume), so only the world classes are spawned here: types 2/3/7/8/9 on their group timer (7 spawns by cell too but places the bolt near
// the nearest player character, spawnBolt, weather_lightning.md) and type 10 (GlobalPoint) on the per-frame band refill (rwh.81 §2).
void WeatherInstances::reset(const SkyWeather& w)
{
	groups_.clear();
	weather_ = w.name;
	rng_.seed((uint32_t)(fnv(w.name) ^ 0x9e3779b97f4a7c15ull));
	for (const SkyWeather::Effect& e : w.effects)
	{
		if (e.anchor() != SkyWeather::World) { continue; }
		Group g;
		g.e = e;
		g.timer = e.delay;	// v1: the first spawn delay
		groups_.push_back(std::move(g));
	}
}

// The exe's group tick (140102370) and the instance update (§4). dtGame is game-speed seconds; 0 is frozen (no spawns, no drift).
// Type 10 keeps the exe's timerless tick (1401048b0) whatever the speed: its refill is position-driven, not time-driven.
void WeatherInstances::update(double dtGame, const SkyWeather& w, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup,
	float windX, float windZ, const double cam[3], const double pivot[3], const std::vector<std::array<double, 3>>& players)
{
	if (weather_ != w.name) { retire(); reset(w); }
	auto u01 = [&] { return (double)rng_() / ((double)rng_.max() + 1.0); };
	for (Group& g : groups_) { if (g.e.type == 10) { refill_global_point(g, atlas, hm, ownerGroup, pivot); } }
	if (dtGame > 0)
	{
		for (Old& o : olds_) { o.fade -= (float)(dtGame / kChangeFade); }
		olds_.erase(std::remove_if(olds_.begin(), olds_.end(), [](const Old& o) { return o.fade <= 0; }), olds_.end());
		for (Group& g : groups_)
		{
			if (g.e.type == 10)	// no timer, no drift: the age and the TTL are all that advance
			{
				for (Inst& in : g.inst) { in.age += dtGame; }
				g.inst.erase(std::remove_if(g.inst.begin(), g.inst.end(), [](const Inst& in) { return in.ttl > 0 && in.age >= in.ttl; }), g.inst.end());
				continue;
			}
			g.timer -= dtGame;
			// while (timer <= 0 && (v0 == 0 || handlers < v0) && spawn()) timer += rand(v1, v2)
			for (int guard = 0; g.timer <= 0 && (g.e.cap == 0 || (int)g.inst.size() < g.e.cap) && guard < 4096; guard++)
			{
				Inst in;
				if (!spawn(g, in, atlas, hm, ownerGroup, players)) { break; }
				g.inst.push_back(in);
				const double lo = std::min(g.e.delay, g.e.respawnMax), hi = std::max(g.e.delay, g.e.respawnMax);
				const double wait = lo + (hi - lo) * u01();
				if (g.e.cap == 0 && wait <= 0) { break; }	// v0 = 0 with v1 = v2 = 0 would never end (not in the data)
				g.timer += wait;
			}
			const bool wanders = g.e.type == 3 || g.e.type == 8 || g.e.type == 9;
			const bool bolt = g.e.type == 7;
			for (Inst& in : g.inst)
			{
				in.age += dtGame;
				if (bolt)
				{
					// weather_lightning.md: every 1-2 s the handler drops the psys when the cell under the bolt left the owner biome
					// group (a town of another group has none). The bolt never moves. Our cells are the areasmap's 1152 u (the exe's
					// grid is 4608 u); the exe's "a loaded cell within maxViewDist/4608 +-2 cells" has no counterpart here.
					in.reroll -= dtGame;
					if (in.reroll <= 0)
					{
						in.reroll = 1.0 + u01();
						if (atlas.groupIndexAt(in.pos[0], in.pos[2]) != ownerGroup) { in.kill = true; }
					}
					continue;
				}
				if (!wanders) { continue; }
				in.reroll -= dtGame;
				if (in.reroll <= 0)
				{
					// every rand(TTL min, TTL max) x 0.1 s: a new heading, yaw = rand(-0.5, 0.5) x pi
					const double lo = std::min(g.e.ttlMin, g.e.ttlMax), hi = std::max(g.e.ttlMin, g.e.ttlMax);
					in.reroll = std::max(0.1, (lo + (hi - lo) * u01()) * 0.1);
					const double yaw = (u01() * 2.0 - 1.0) * 0.5 * M_PI;
					in.heading[0] = (float)std::sin(yaw);
					in.heading[1] = (float)std::cos(yaw);
				}
				in.pos[0] += (double)in.heading[0] * g.e.wanderSpeed * dtGame;
				in.pos[2] += (double)in.heading[1] * g.e.wanderSpeed * dtGame;
				if (g.e.windAffected) { in.pos[0] += windX * dtGame; in.pos[2] += windZ * dtGame; }
				if (hm) { in.pos[1] = hm->terrainHeight(in.pos[0], in.pos[2]); }	// MINE: every frame in place of the exe's ground-follow timer
			}
			g.inst.erase(std::remove_if(g.inst.begin(), g.inst.end(), [](const Inst& in) { return in.kill || (in.ttl > 0 && in.age >= in.ttl); }), g.inst.end());
		}
	}
	// Visibility (W2d): no pop-in. Fade in over kFadeIn after spawn, out over the last kFadeOut of the TTL, and over the last 20 % of
	// the EFFECT maximum view distance rather than at a hard cut. The instance keeps living (age, drift, respawn) at fade 0.
	auto vis = [&](const SkyWeather::Effect& e, Inst& in)
	{
		double reach = kDrawRadius;
		if (e.type == 10) { reach = std::min(4.0 * (e.viewDistance > 0 ? (double)e.viewDistance : 1000.0), kDrawRadius); }	// its 4M kill radius, not a view distance
		else if (e.viewDistance > 0) { reach = std::min((double)e.viewDistance, kDrawRadius); }
		const double d = std::hypot(in.pos[0] - cam[0], in.pos[2] - cam[2]);
		const float far = d >= reach ? 0.0f : d > reach * 0.8 ? (float)((reach - d) / (reach * 0.2)) : 1.0f;
		const bool bolt = e.type == 7;	// ~1 s bolts: the script's own colour curve is the flash, no spawn or end-of-life ramp
		const float born = bolt ? 1.0f : (float)std::min(1.0, in.age / kFadeIn);
		const float dying = bolt || in.ttl <= 0 ? 1.0f : (float)std::min(1.0, (in.ttl - in.age) / kFadeOut);
		in.fade = std::max(0.0f, std::min(far, std::min(born, dying)));
		in.drawn = in.fade > 0.001f;
	};
	for (Group& g : groups_)
	{
		for (Inst& in : g.inst) { vis(g.e, in); }
	}
	for (Old& o : olds_)
	{
		o.in.fade = 1;	// the change fade is o.fade; the distance ramp still applies as the camera moves
		vis(o.e, o.in);
	}
}

void WeatherInstances::retire()
{
	for (Group& g : groups_)
	{
		for (Inst& in : g.inst)
		{
			Old o;
			o.e = g.e;
			o.in = in;
			o.fade = in.fade;
			olds_.push_back(std::move(o));
		}
	}
	groups_.clear();
}

// The per-instance rolls at a placed point: TTL (max <= 0 = eternal), strength, the wandering heading, and the stable key.
void WeatherInstances::init_instance(const Group& g, Inst& out, double x, double y, double z)
{
	auto u01 = [&] { return (double)rng_() / ((double)rng_.max() + 1.0); };
	out = Inst();
	out.pos[0] = x;
	out.pos[1] = y;
	out.pos[2] = z;
	const double ttlLo = std::min(g.e.ttlMin, g.e.ttlMax), ttlHi = std::max(g.e.ttlMin, g.e.ttlMax);
	out.ttl = ttlHi > 0 ? ttlLo + (ttlHi - ttlLo) * u01() : 0.0;
	out.strength = g.e.strengthMin + (g.e.strengthMax - g.e.strengthMin) * (float)u01();
	const double yaw = (u01() * 2.0 - 1.0) * 0.5 * M_PI;
	out.heading[0] = (float)std::sin(yaw);
	out.heading[1] = (float)std::cos(yaw);
	out.reroll = std::max(0.1, (ttlLo + (ttlHi - ttlLo) * u01()) * 0.1);
	out.key = fnv(weather_ + "|" + g.e.system + "|" + std::to_string(serial_++));
}

// §3: a random cell of the owner group, a random point inset 10 % per side, y = terrain height, retried while the EFFECT altitude or slope
// rejects it (up to 10; a rejected spawn is skipped and the group timer retries - the exe returns the last attempt). Type 7 is the exception:
// it draws the same cell point but places the bolt near a player character (spawnBolt).
bool WeatherInstances::spawn(Group& g, Inst& out, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup,
	const std::vector<std::array<double, 3>>& players)
{
	if (g.e.type == 7) { return spawnBolt(g, out, atlas, hm, ownerGroup, players); }
	if (!hm || ownerGroup < 0 || (size_t)ownerGroup >= atlas.groupCells.size()) { return false; }
	const std::vector<std::pair<uint16_t, uint16_t>>& cells = atlas.groupCells[(size_t)ownerGroup];
	if (cells.empty()) { return false; }
	auto u01 = [&] { return (double)rng_() / ((double)rng_.max() + 1.0); };
	const double cellUnits = WeatherAtlas::kWorld / WeatherAtlas::kN;
	for (int attempt = 0; attempt < 10; attempt++)
	{
		const std::pair<uint16_t, uint16_t>& cell = cells[(size_t)(u01() * (double)cells.size())];
		const double x = -WeatherAtlas::kWorld * 0.5 + ((double)cell.first + 0.1 + 0.8 * u01()) * cellUnits;
		const double z = -WeatherAtlas::kWorld * 0.5 + ((double)cell.second + 0.1 + 0.8 * u01()) * cellUnits;
		const double y = hm->terrainHeight(x, z);
		if (g.e.maxAltitude >= g.e.minAltitude && (y < g.e.minAltitude || y > g.e.maxAltitude)) { continue; }
		const double gx = (hm->terrainHeight(x + 2, z) - hm->terrainHeight(x - 2, z)) / 4.0;
		const double gz = (hm->terrainHeight(x, z + 2) - hm->terrainHeight(x, z - 2)) / 4.0;
		const double ny = 1.0 / std::sqrt(1.0 + gx * gx + gz * gz);
		if (g.e.maxSlope >= g.e.minSlope && (ny < g.e.minSlope || ny > g.e.maxSlope)) { continue; }
		init_instance(g, out, x, y, z);
		return true;
	}
	return false;
}

// rwh.81 §2, the type 10 tick (1401048b0): kill handlers past 4M from the pivot, count the disc (d <= M) and the annulus (M < d <= 2M), then spawn
// max(0, min(3T - n, T - inside)) inside at rand(0, M) and max(0, min(3T - n - first, T - annulus)) in [M, 2M]; n and T are the live count and v0.
// No timer. A point is uniform over the disc/annulus, terrain y, retried against the EFFECT altitude and slope, and its cell must be the owner group's.
void WeatherInstances::refill_global_point(Group& g, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup, const double pivot[3])
{
	if (!hm || ownerGroup < 0) { return; }
	auto u01 = [&] { return (double)rng_() / ((double)rng_.max() + 1.0); };
	const double M = g.e.viewDistance > 0 ? (double)g.e.viewDistance : 1000.0;
	const double inner2 = M * M, annulus2 = 4.0 * M * M, kill2 = 16.0 * M * M;
	g.inst.erase(std::remove_if(g.inst.begin(), g.inst.end(), [&](const Inst& in) {
		const double dx = in.pos[0] - pivot[0], dz = in.pos[2] - pivot[2];
		return dx * dx + dz * dz >= kill2;
	}), g.inst.end());
	int inside = 0, annulus = 0;
	for (const Inst& in : g.inst)
	{
		const double dx = in.pos[0] - pivot[0], dz = in.pos[2] - pivot[2], d2 = dx * dx + dz * dz;
		if (d2 <= inner2) { inside++; }
		else if (d2 <= annulus2) { annulus++; }
	}
	const int n = (int)g.inst.size(), T = std::max(0, g.e.cap);
	const auto place = [&](double rmin, double rmax, Inst& out) {
		for (int attempt = 0; attempt < 10; attempt++)
		{
			const double a = u01() * 2.0 * M_PI, r = std::sqrt(u01()) * (rmax - rmin) + rmin;
			const double x = pivot[0] + std::cos(a) * r, z = pivot[2] + std::sin(a) * r;
			const double y = hm->terrainHeight(x, z);
			if (g.e.maxAltitude >= g.e.minAltitude && (y < g.e.minAltitude || y > g.e.maxAltitude)) { continue; }
			const double gx = (hm->terrainHeight(x + 2, z) - hm->terrainHeight(x - 2, z)) / 4.0;
			const double gz = (hm->terrainHeight(x, z + 2) - hm->terrainHeight(x, z - 2)) / 4.0;
			const double ny = 1.0 / std::sqrt(1.0 + gx * gx + gz * gz);
			if (g.e.maxSlope >= g.e.minSlope && (ny < g.e.minSlope || ny > g.e.maxSlope)) { continue; }
			if (atlas.group.empty() || atlas.groupIndexAt(x, z) != ownerGroup) { continue; }
			init_instance(g, out, x, y, z);
			return true;
		}
		return false;
	};
	const int first = std::max(0, std::min(3 * T - n, T - inside));
	for (int i = 0; i < first; i++) { Inst in; if (!place(0.0, M, in)) { break; } g.inst.push_back(in); }
	const int second = std::max(0, std::min(3 * T - n - first, T - annulus));
	for (int i = 0; i < second; i++) { Inst in; if (!place(M, 2.0 * M, in)) { break; } g.inst.push_back(in); }
}

// weather_lightning.md (EFFECT type 7): a random point of a random cell of the owner biome group is drawn and thrown away, then the nearest
// PLAYER-faction character to it (any distance) takes the bolt at pos + rand(-2000, 2000) in x and z, y = terrain height + 10. With no player
// character the drawn point is kept. The bolt never moves; the handler lives for the EFFECT's TTL (rebirth: 1 s), and its psys outlives it
// (PlacedSystem::outlive: rwh.120 answer 1). The seeded point is our areasmap's cell (1152 u; the exe draws a 4608 u cell) - the same draw
// as the other world classes.
bool WeatherInstances::spawnBolt(Group& g, Inst& out, const WeatherAtlas& atlas, const kworld::Heightmap* hm, int ownerGroup,
	const std::vector<std::array<double, 3>>& players)
{
	if (!hm || ownerGroup < 0 || (size_t)ownerGroup >= atlas.groupCells.size()) { return false; }
	const std::vector<std::pair<uint16_t, uint16_t>>& cells = atlas.groupCells[(size_t)ownerGroup];
	if (cells.empty()) { return false; }
	auto u01 = [&] { return (double)rng_() / ((double)rng_.max() + 1.0); };
	const double cellUnits = WeatherAtlas::kWorld / WeatherAtlas::kN;
	const std::pair<uint16_t, uint16_t>& cell = cells[(size_t)(u01() * (double)cells.size())];
	double x = -WeatherAtlas::kWorld * 0.5 + ((double)cell.first + 0.1 + 0.8 * u01()) * cellUnits;
	double z = -WeatherAtlas::kWorld * 0.5 + ((double)cell.second + 0.1 + 0.8 * u01()) * cellUnits;
	const std::array<double, 3>* near = nullptr;
	double best = 0;
	for (const std::array<double, 3>& p : players)
	{
		const double d = (p[0] - x) * (p[0] - x) + (p[2] - z) * (p[2] - z);
		if (!near || d < best) { near = &p; best = d; }
	}
	double y;
	if (near)
	{
		x = (*near)[0] + (u01() * 4000.0 - 2000.0);
		z = (*near)[2] + (u01() * 4000.0 - 2000.0);
		y = hm->terrainHeight(x, z) + 10.0;
	}
	else { y = hm->terrainHeight(x, z); }
	out = Inst();
	out.pos[0] = x;
	out.pos[1] = y;
	out.pos[2] = z;
	const double ttlLo = std::min(g.e.ttlMin, g.e.ttlMax), ttlHi = std::max(g.e.ttlMin, g.e.ttlMax);
	out.ttl = ttlHi > 0 ? ttlLo + (ttlHi - ttlLo) * u01() : 0.0;	// max TTL <= 0: eternal
	out.strength = g.e.strengthMin + (g.e.strengthMax - g.e.strengthMin) * (float)u01();
	out.reroll = 1.0 + u01();	// the handler's first biome-cell check, then every 1-2 s
	out.key = fnv(weather_ + "|" + g.e.system + "|" + std::to_string(serial_++));
	return true;
}

std::vector<Particles::PlacedSystem> WeatherInstances::systems(const std::vector<std::string>& suppressedScripts) const
{
	const auto suppressed = [&](const std::string& script) { return std::find(suppressedScripts.begin(), suppressedScripts.end(), script) != suppressedScripts.end(); };
	std::vector<Particles::PlacedSystem> out;
	for (const Group& g : groups_)
	{
		if (suppressed(g.e.system)) { continue; }	// the sim's BEAMS schedule draws this effect (its position is the sim's, not a local roll)
		for (const Inst& in : g.inst)
		{
			if (!in.drawn) { continue; }
			Particles::PlacedSystem p;
			p.key = in.key;
			p.script = g.e.system;
			for (int k = 0; k < 3; k++) { p.pos[k] = in.pos[k]; }
			p.seed = (uint32_t)(in.key ^ (in.key >> 32));
			p.amount = g.e.amount * in.strength;	// §5: the weather strength x the instance's rolled EFFECT strength
			p.prewarm = g.e.type != 7;		// a bolt is an event: play its 1 s one-shot emitter from t = 0, not pre-warmed
			// rwh.120 answer 1: the exe removes the 1 s EFFECT handler but destroys the psys only when no emitted particle is left, so a
			// bolt's flash (and its Flames/SPARKS tail) outlives the instance. Every other world class is re-handed every frame it lives.
			p.outlive = g.e.type == 7;
			p.fade = in.fade;
			out.push_back(std::move(p));
		}
	}
	for (const Old& o : olds_)
	{
		if (suppressed(o.e.system)) { continue; }
		const float fade = std::min(o.fade, o.in.fade);
		if (fade <= 0.001f) { continue; }
		Particles::PlacedSystem p;
		p.key = o.in.key;
		p.script = o.e.system;
		for (int k = 0; k < 3; k++) { p.pos[k] = o.in.pos[k]; }
		p.seed = (uint32_t)(o.in.key ^ (o.in.key >> 32));
		p.amount = o.e.amount * o.in.strength;
		p.outlive = o.e.type == 7;
		p.fade = fade;
		out.push_back(std::move(p));
	}
	return out;
}

std::string WeatherInstances::describe() const
{
	std::string out;
	char buf[256];
	for (const Group& g : groups_)
	{
		size_t drawn = 0;
		for (const Inst& in : g.inst) { drawn += in.drawn ? 1 : 0; }
		snprintf(buf, sizeof buf, "  %s type %d: %zu live (%zu drawn), cap %d, timer %.1f s\n", g.e.system.c_str(), g.e.type, g.inst.size(), drawn, g.e.cap, g.timer);
		out += buf;
		for (const Inst& in : g.inst)
		{
			snprintf(buf, sizeof buf, "    %s %.1f %.1f %.1f age %.1f ttl %.1f str %.2f fade %.2f\n", in.drawn ? "drawn" : "far  ", in.pos[0], in.pos[1], in.pos[2], in.age, in.ttl, in.strength, in.fade);
			out += buf;
		}
	}
	for (const Old& o : olds_)
	{
		const float fade = std::min(o.fade, o.in.fade);
		if (fade <= 0.001f) { continue; }
		snprintf(buf, sizeof buf, "  %s type %d: weather change fade %.2f at %.1f %.1f %.1f\n", o.e.system.c_str(), o.e.type, fade, o.in.pos[0], o.in.pos[1], o.in.pos[2]);
		out += buf;
	}
	return out;
}

size_t WeatherInstances::live() const
{
	size_t n = 0;
	for (const Group& g : groups_) { n += g.inst.size(); }
	return n;
}
