#pragma once

// Venge's sky beams (docs/reference/VENGE_BEAMS_STATIC.md, SIMLINK "BEAMS"): the burning wandering weather effects (EFFECT type 9) of a
// group's current weather, planned ahead and burning whoever stands under them.
//
// A weather's `effects` list says how many handlers (slots) run at once. Slot s of a weather instance is a chain of beams: beam k
// spawns where beam k - 1 ended (k = 0 at the instance's start tick) and lives for a random time to live, clipped to the end of the
// weather (a held debug pin is never clipped). Beam k is a pure function of (world seed, group, weather, start minute, s, k): the
// start cell, the heading and the heading re-rolls come from one counter hash, so nothing is saved and a load re-derives the same
// beams. The BeamField only caches the chain's current and next beam per slot.
//
// The path is exact and shared with the client (SIMLINK "BEAMS"): from (x, z) at spawnTick each tick moves speed / 20 along
// (sin yaw, cos yaw) of the segment that covers it, in float. The damage zone follows the same function.

#include "ksim/world/weather.hpp"

#include <flecs.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ksim {

// The lead: a beam goes to the client this many ticks before it strikes (3 s).
inline constexpr std::uint64_t kBeamLeadTicks = 60;
// The view range: a session whose camera or player character is inside the group's zone, or within this many units of a zone cell,
// gets every live beam of the group (user 2026-11-19, h2v.14.14: the sim runs all 40 handlers zone-wide, the client draws the closest
// few); no beam reaches a session farther out. A burn needs a beam within 283 u of a character, always inside the zone.
inline constexpr double kBeamRange = 10000.0;
// The visible burst of a beam (rwh.175 section 2.2): one per beam, 6.0 to 10.6 s from its spawn, whatever its time to live (the
// damage window, 10 to 30 s). The burst rises ~5 s, holds ~1.5 s and fades ~2 s (the client's particle script owns the shape).
inline constexpr double kBeamVisibleMinSeconds = 6.0;
inline constexpr double kBeamVisibleMaxSeconds = 10.6;

struct BeamSegment
{
	std::uint16_t ticks = 0;
	float yaw = 0.0f;
};

struct Beam
{
	std::uint32_t id = 0; // slot << 16 | k, unique inside one weather instance of one group
	const EffectDef* effect = nullptr; // null = no beam
	std::uint64_t spawnTick = 0;
	std::uint64_t endTick = 0; // the damage window: the beam burns for spawnTick <= t < endTick
	std::uint16_t visibleTicks = 0; // the visible burst: shown for spawnTick <= t < spawnTick + visibleTicks (<= the life); on the wire after endTick (protocol 7)
	float x = 0.0f; // the ground point at spawnTick
	float z = 0.0f;
	float speed = 0.0f; // u per sim-second
	float radius = 0.0f;
	float strength = 0.0f;
	std::vector<BeamSegment> segments; // the ticks sum to endTick - spawnTick
};

// Whether the beam's burst is still on screen at `tick` (the damage window may run on after it).
inline bool beamVisibleAt(const Beam& beam, std::uint64_t tick) noexcept
{
	return tick >= beam.spawnTick && tick < beam.spawnTick + beam.visibleTicks;
}
// One tick of the path: moves (x, z) by speed / 20 along the yaw (0 = +Z, positive toward +X).
void beamStep(float& x, float& z, float speed, float yaw) noexcept;
// The point at `tick` (spawnTick <= tick <= endTick), the same sum the BeamField keeps incrementally.
void beamPositionAt(const Beam& beam, std::uint64_t tick, float& x, float& z) noexcept;
// The burn strength at squared distance d2 from a beam: `strength` inside the radius, falling off linearly in d2 to 0 at radius^2 x 2.
double beamBurn(const Beam& beam, double d2) noexcept;

// What one weather instance plans from: its identity, its slot count and where the beams may start.
struct BeamPlanKey
{
	std::uint64_t seed = 0; // world seed
	std::string group;
	std::string weather;
	std::uint64_t startMinute = 0;
};

// Beam k of slot `slot`, spawning at `spawnTick`, clipped to `limitTick`. effect null or no room for a beam: effect left null.
// cells = the zone cells (kworld::ZoneCell index) the beams may start in; empty gives no beam.
Beam planBeam(const BeamPlanKey& key, const EffectDef& effect, const std::vector<int>& cells, std::uint32_t slot, std::uint32_t k,
	std::uint64_t spawnTick, std::uint64_t limitTick);

// A weather's slots: one entry per handler of every burning wandering EFFECT (type 9, affect type 3) in its `effects` list.
struct BeamSlot
{
	const EffectDef* effect = nullptr;
	Beam current; // alive or about to spawn (effect null = the slot has ended for this instance)
	Beam next; // planned to spawn at current.endTick (effect null = none)
	// The incremental position of `current` at posTick.
	float px = 0.0f;
	float pz = 0.0f;
	std::uint64_t posTick = 0;
	std::size_t segment = 0;
	std::uint32_t used = 0; // ticks already spent in `segment`
};

struct BeamGroup
{
	BeamPlanKey key; // key.weather empty = no beams
	std::uint64_t epoch = 0; // BeamField::changes() at the last replan: a session whose copy is older resets its client
	std::uint64_t limitTick = 0;
	std::uint32_t colour = 0;
	std::vector<BeamSlot> slots;
};

class BeamField
{
public:
	BeamField(std::shared_ptr<const WeatherSystem> weather, std::uint64_t worldSeed);

	// Called every tick after the weather has stepped; replans a group whose instance changed (a new weather, a pin, a load).
	void step(const flecs::world& ecs, std::uint64_t now);
	// Counts the replans: the server resends the BEAMS frame with the forget flag when it changes (a metric, not saved).
	std::uint64_t changes() const noexcept { return changes_; }
	const std::vector<BeamGroup>& groups() const noexcept { return groups_; }
	// The strongest burn at a world point at the last stepped tick (0 = none); beams do not stack. `type` is the strongest beam's
	// WeatherAffecting type (the protection and immunity type), 0 when none burns.
	double burnAt(double x, double z, int& type) const noexcept;
	// Whether (x, z) lies within `range` of any zone cell box of the group with this biome colour (inside the zone counts): the sessions
	// that get every live beam of the group (kenshi-h2v.14.14).
	bool nearZone(std::uint32_t colour, double x, double z, double range) const noexcept;

private:
	const std::vector<int>& cellsOf(std::uint32_t colour);
	void rebuild(BeamGroup& group, std::size_t index, const state::WeatherRegion& region, const WeatherDef& def, std::uint64_t now);
	void advance(BeamGroup& group, std::uint64_t now);

	std::shared_ptr<const WeatherSystem> weather_;
	std::uint64_t seed_;
	std::vector<BeamGroup> groups_;
	std::unordered_map<std::uint32_t, std::vector<int>> cells_;
	std::uint64_t changes_ = 0;
	std::uint64_t now_ = 0; // the last stepped tick: a staggered first beam burns only once it has spawned
};

} // namespace ksim
