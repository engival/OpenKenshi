#include "particleworld.hpp"

#include <algorithm>
#include <cstdio>

#include "worldbuildings.hpp"

namespace
{
uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull)
{
	for (const unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
	return h;
}

// Stable key: placement, part, effect index and script (never the placedEffects() order).
uint64_t keyOf(const WorldBuildings::PlacedEffectRt& e, const std::string& script)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%u/%d/%u/", e.instance, e.part, e.def);
	return fnv(script, fnv(std::string(buf)));
}
}

void ParticleWorld::clear()
{
	live_.clear();
	sys_.clear();
}

void ParticleWorld::update(double dt, const std::vector<WorldBuildings::PlacedEffectRt>& placed, const WorldBuildings& world, bool night)
{
	std::vector<const WorldBuildings::PlacedEffectRt*> want;
	for (const WorldBuildings::PlacedEffectRt& e : placed) { want.push_back(&e); }	// every placed script that parses is drawn
	if (want.size() > kMaxSystems)
	{
		std::partial_sort(want.begin(), want.begin() + kMaxSystems, want.end(),
			[](const WorldBuildings::PlacedEffectRt* a, const WorldBuildings::PlacedEffectRt* b) { return a->distance < b->distance; });
		want.resize(kMaxSystems);
	}
	// Seen this frame: refresh the transform, fade in a new system.
	std::vector<uint64_t> seen;
	seen.reserve(want.size());
	for (const WorldBuildings::PlacedEffectRt* e : want)
	{
		const std::string& script = world.effectScript(e->def);
		const uint64_t key = keyOf(*e, script);
		seen.push_back(key);
		Live& l = live_[key];
		if (l.s.script.empty())
		{
			l.s.key = key;
			l.s.script = script;
			l.s.seed = (uint32_t)(fnv(std::string("seed/") + script) ^ (fnv(std::to_string(key)) >> 32));
			l.fade = dt > 0 ? 0.0f : 1.0f;	// a scripted run is frozen after the pre-warm: no fade to observe
		}
		for (int k = 0; k < 3; k++) { l.s.pos[k] = e->world[k]; }
		for (int k = 0; k < 4; k++) { l.s.quat[k] = e->quat[k]; }
		l.s.nodeScale = e->scale;
		l.dist = e->distance;
		l.unseen = 0;
	}
	std::sort(seen.begin(), seen.end());
	// Fade and retire. In a scripted run (dt 0) a system that is no longer reported goes at once: shots stay deterministic.
	for (auto it = live_.begin(); it != live_.end();)
	{
		Live& l = it->second;
		const bool here = std::binary_search(seen.begin(), seen.end(), it->first);
		if (!here)
		{
			l.unseen += dt;
			if (dt <= 0 || l.unseen > kGrace + kFadeOut) { it = live_.erase(it); continue; }
			if (l.unseen >= kGrace) { l.fade = std::max(0.0f, l.fade - (float)(dt / kFadeOut)); }
		}
		else { l.fade = std::min(1.0f, l.fade + (dt > 0 ? (float)(dt / kFadeIn) : 1.0f)); }
		++it;
	}
	sys_.clear();
	std::vector<std::pair<float, Particles::PlacedSystem>> order;
	order.reserve(live_.size());
	for (const auto& [key, l] : live_)
	{
		if (l.fade <= 0) { continue; }
		// kenshi_sparks is the one effect family the original time-switches: vis 1 by day, 0 at night (world_particles.md 2 Live). Not handed to
		// the simulation at night, so Particles drops it at once, like the handler's setVisible(false).
		if (night && l.s.script.rfind("kenshi_sparks", 0) == 0) { continue; }
		order.push_back({ l.dist, l.s });
		order.back().second.fade = l.fade;
	}
	// Nearest first: Particles' instance budget goes to the fires around the camera when a dense view exceeds it.
	std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	sys_.reserve(order.size());
	for (auto& [dist, s] : order) { sys_.push_back(std::move(s)); }
}

std::string ParticleWorld::describe() const
{
	std::string out;
	char buf[256];
	for (const auto& [key, l] : live_)
	{
		snprintf(buf, sizeof buf, "%s at %.1f %.1f %.1f fade %.2f %s", l.s.script.c_str(), l.s.pos[0], l.s.pos[1], l.s.pos[2], l.fade,
			l.fade < 0.999f || l.unseen >= kGrace ? "fading" : "alive");
		out += buf;
		out += '\n';
	}
	return out;
}
