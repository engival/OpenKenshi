#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "render/particles.hpp"
#include "net/simlink.hpp"

namespace kworld { class Heightmap; }

// Venge's sky beams drawn from the sim's schedule (SIMLINK "BEAMS", docs/reference/VENGE_BEAMS_STATIC.md, kenshi-rwh.48): the sim owns the
// burning wandering weather effects (EFFECT type 9), plans each a few seconds ahead and sends its exact path. This class keeps those beams and
// hands the drawn ones to Particles at the path point of the render tick, y snapped to the terrain under it (the client's snap, SIMLINK
// "BEAMS"), as the EFFECT's particle system (SkyBeam.pu: a 200 x 16 000 u additive strip, the base flame, the ground blast and the mist).
// A beam plays as an event (no pre-warm: the script's own colour curve is the strike and a pre-warm would eat its one-shot emitter). The
// advance notice is the wind-up: the beam is registered before it strikes so it appears exactly at its spawnTick, and nothing is drawn
// during the lead (the original has no pre-strike visual). The client drops a beam at endTick; the particle system it started is kept a
// little longer at that last point, because in the original the psys outlives the handler's damage window (rwh.48 4.4).
class BeamField
{
public:
	// Sim ticks the particle system can still live after a beam's end. The decoder uses the same value to drop an ended beam (simlink::kBeamParticleTailTicks).
	static constexpr uint64_t kParticleTailTicks = simlink::kBeamParticleTailTicks;

	void clear();
	// Copies the decoder's beams when its BEAMS serial moved (a BEAMS frame is at most a few ticks old). Returns true when it changed.
	bool sync(const simlink::SimLinkDecoder& d);
	// The placed systems of the beams drawn at `tick` (the render tick), appended to `out`. Nothing before a beam's spawnTick. Every beam
	// the sim sends is placed, whatever its distance: the original has no view distance for these (rwh.48: `maximum view distance` 0), and
	// a 16 000 u column is seen from across the biome (the user's standing direction: match the look, not the draw ranges). Cost is the
	// particle count `dump beams` / the HUD report; the instance cap only ever truncates the farthest scripts.
	void systems(double tick, const kworld::Heightmap* hm, std::vector<Particles::PlacedSystem>& out) const;
	// The particle scripts the sim is drawing: the weather's own local placement of them must not run as well (the sim's schedule is the truth).
	std::vector<std::string> scripts() const;
	std::string dump(double tick) const;	// a line per beam (id, group, spawn/end, the ground point at `tick`, scripts) (stdout)
	size_t live(double tick) const;		// spawnTick <= tick < endTick
	// One row per beam held: its particle-system key (what systems() places, so a placed system pairs with its beam), the state at
	// `tick` and the ground point systems() places it at. `dump beamfeet` reads it for the foot-fire check (kenshi-h2v.14.9.1).
	struct Foot
	{
		uint64_t key = 0, spawnTick = 0, endTick = 0;
		uint32_t id = 0;
		int state = 0;		// 1 live, 2 tail (the particles after endTick; the wind-up is not placed, so it is not a foot)
		std::string particles;
		float x = 0, z = 0;
	};
	std::vector<Foot> feet(double tick) const;
	struct Aim
	{
		uint32_t id = 0;
		uint64_t spawnTick = 0, endTick = 0;
	};
	// The camera op (`camera beam [id=N] [windup|live|same]`): the named beam else the next to strike else the live one nearest the camera,
	// restricted to the beams still in their lead (`phase` 1) or already striking (`phase` 2), the beam the last call picked (`phase` 3:
	// the shot pair watches one beam from its wind-up into its strike), any (`phase` 0); its ground point at `tick` (the spawn point while
	// it is in the lead) and which beam it was. False when there is no such beam.
	bool aim(double tick, uint32_t id, int phase, const double cam[3], float& x, float& z, Aim* out = nullptr);

private:
	struct Beam
	{
		uint64_t key = 0;
		std::string group, particles;
		simlink::Beam b;
	};
	std::vector<Beam> beams_;
	uint32_t serial_ = 0;
	uint32_t lastAim_ = 0;	// the beam the last aim() picked (`camera beam same`)
};
