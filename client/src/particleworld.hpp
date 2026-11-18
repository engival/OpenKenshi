#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "particles.hpp"
#include "worldbuildings.hpp"

// The placed world-effect driver (docs/CLIENT_GOALS.md "World effects plan"): takes this frame's visible building EFFECT instances, diffs
// them against the live systems by a stable key (placement, part, effect index, script), fades systems in and out, and hands the list to
// Particles. It owns no GPU state. A system that leaves the view is kept for a grace time (and WorldBuildings collects with a view margin),
// so turning the camera back does not rebuild it: no pop-in. The simulation itself (pre-warm, RNG, transforms) lives in Particles.
class ParticleWorld
{
public:
	// placed = WorldBuildings::placedEffects(); world = the set they came from (script name, the effect node's world orientation). `night` is the outdoor-light
	// gate (daylight <= 0.1): particles burn day and night (kenshi-inspect world_particles.md 2 Live) except the kenshi_sparks family, which the
	// original hides at night (vis 1 by day, 0 at night).
	void update(double dt, const std::vector<WorldBuildings::PlacedEffectRt>& placed, const WorldBuildings& world, bool night);
	const std::vector<Particles::PlacedSystem>& systems() const { return sys_; }
	std::string describe() const;	// one line per live system: script, world position, fade and alive/fading
	void clear();

private:
	static constexpr float kFadeIn = 0.5f;		// seconds
	static constexpr float kFadeOut = 0.6f;
	static constexpr double kGrace = 0.75;		// a system unseen this long starts to fade out
	static constexpr size_t kMaxSystems = 256;	// nearest first, so the instance-buffer budget goes to the fires around the camera; the rest wait
	// (a placed fire has no distance limit: kenshi-inspect world_particles.md 2 Live, maximum view distance 0 on every fire EFFECT)

	struct Live
	{
		Particles::PlacedSystem s;
		float fade = 0;
		float dist = 0;
		double unseen = 0;
	};
	std::map<uint64_t, Live> live_;
	std::vector<Particles::PlacedSystem> sys_;
};
