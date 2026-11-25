#pragma once

// Perception: the measured sensor of the original (docs/reference/PERCEPTION_LIVE.md items 17 and 18, PERCEPTION_STATIC.md),
// the one place the sim decides who notices whom. It replaces the 1500 / 600 / 880 unit "notices" distances of the combat
// arena (target pick, fight join, the follow task's "leader perceived").
//
// Every character at full detail has a sensor (state::Fighter::sight, senseClock). A scan runs once per
// `scanInterval(n)` seconds per character (the original: 8 full character ticks per frame, round-robin, so 0.5-1.5 s),
// staggered by the character's dice key (Fighter::diceSeed, else the Id; task CC), and passes the seconds since the last
// scan as dt. A scan, for every other living character:
//
//   Sight   R = 1500 x race `vision range mult` x (0.5 + 0.5 daylight) x weather (humans only), 500 base indoors.
//           c = (dot(facing, dir) - 0.2) / 0.8 is the cone (half angle 78.5 degrees); v = c, x 0.5 crouched, and when
//           the target sneaks x (1 - 0.4 s). The target is in reach when dist <= R v A B (light terms A, B), the cap
//           of 0.5 R in combat, plus line of sight. Observer and target must be in the same building (or both outdoors).
//   Noise   for a target that moves (or fights) and was not seen: the "sighted" roll with the noise range x 0.5, at
//           most once per 2 s per pair. A target that stands still out of combat makes none.
//   Sneak   a sneaking target in reach fills a meter per observer: dt / ((0.5 + 5 s (1 - v)) x (4 - 3 attn)); aware at
//           meter > 1. Line of sight lost: 0. Out of reach: drains by dt / 260.
//   Memory  a record is kept 30 s after the last sighting (60 s when the observer fights).
//   List    the record list is the sensor list of sensor.hpp: the radius query returns every character inside the scan range, seen or
//           not (an unseen one is "remembered", record bit 8), so a record exists for it without a sighting; out of range a record the
//           scan did not sense is dropped 2 s after its last roll. The memory above clears `seen` (awareness), not the record.
//
// Aggro has no range field in the original: a hostile target the observer is aware of ("seen") makes the squad goal attack.
// The arena asks `aware`. Pure functions first (tests), the scan last. All of it is deterministic: the one random draw (the
// noise roll) is a hash of the pair and the roll count, which the record saves.

#include "ksim/combat/fighter_state.hpp"
#include <array>
#include "ksim/world/towncircle.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ksim::perception {

// ---- constants (PERCEPTION_STATIC.md; measured ones say so) ----
inline constexpr double kBaseRange = 1500.0; // measured: no candidate past 1500 on a clear day
inline constexpr double kIndoorRange = 500.0;
inline constexpr double kSunrise = 5.0 / 24.0; // GOG day clock, live
inline constexpr double kSunset = 23.0 / 24.0;
inline constexpr double kRamp = 1.0 / 12.0; // 2 h
inline constexpr double kConeEdge = 0.2; // measured
inline constexpr double kConeMin = 0.01; // measured: the v threshold
inline constexpr double kDustFactor = 0.4;
inline constexpr double kRainFactor = 0.75;
inline constexpr double kDarkLightK = 0.4; // B's floor; measured at night (175-264 u)
inline constexpr double kCombatReachCap = 0.5;
inline constexpr double kNoiseWalk = 350.0; // wf x 350 at ratio 0 .. wf x 600 at ratio 1
inline constexpr double kNoiseRun = 600.0;
inline constexpr double kNoiseSneakLow = 450.0; // wf x 450 at s = 0 .. 80 at s = 1
inline constexpr double kNoiseSneakFloor = 80.0;
inline constexpr double kWalkRatio = 0.1; // measured: walking reads 365-379 (ratio 0.06-0.12); UNSURE for other races
inline constexpr double kSightedAwake = 0.5; // the roll uses noise x 0.5 for an awake observer
inline constexpr double kSightedThrottle = 2.0; // seconds per pair
inline constexpr double kPlayerPairSight = 1000.0; // both player-controlled: sighted within 1000 (static 2.7)
inline constexpr double kMemory = 30.0;
inline constexpr double kMemoryCombat = 60.0;
inline constexpr double kRecordFresh = 5.0; // the record's memory timer (x2 on the next noise roll)
inline constexpr double kMeterDrain = 260.0; // seconds for a full meter to empty out of the cone
inline constexpr double kNoticeMeter = 1.0;
inline constexpr double kFillBase = 0.5; // fillTime = (0.5 + 5 s (1 - v)) x (4 - 3 attn)
inline constexpr double kFillStealth = 5.0;
inline constexpr double kScanBudgetPerSecond = 480.0; // 8 full character ticks per frame at 60 fps
inline constexpr double kScanMin = 0.5;
inline constexpr double kScanMax = 1.5;

// What the world supplies to perception. Configuration, never saved (the arena's clock-driven part is set every tick).
struct Environment
{
	// The sim's absolute tick (the Simulation sets it every step): the stamp a sighting writes into the observer's dialogue memory
	// (state::Fighter::met, DC_TARGET_LAST_SEEN_X_HOURS_AGO).
	std::uint64_t tick = 0;
	// Decision-cadence scale (Simulation::cadenceScale: min(game speed, 3), 1 at speed 1); the scan interval of a non-player, non-fighting
	// character is multiplied by it (k16.6.633).
	std::uint32_t cadenceScale = 1;
	double dayFraction = 0.5; // time of day, 0..1 (hours / 24); noon by default
	int weatherType = 0; // 0 none, 1 dust storm, 2 acid, 3 burning, 4 gas, 5 rain (WeatherAffecting)
	double weatherStrength = 0.0; // 0..1
	// The walled regions, set by the Simulation. The sight range is the short 500 only where the observer's navmesh gate-region id
	// is > 0, i.e. standing inside a region closed off by gates (a walled town); everywhere else (the Hub has no gates, nor have
	// POIs and outposts without them) it is race `vision range mult` x 1500. Kinspect findings/bq7_sight_region.md (live: the walled
	// UC town reads 1 in street and shops, outlaws outside the gate 0, the whole Hub 0 or -1), correcting s73_static.md section 1.
	// UNSURE / APPROXIMATION: the sim has no region partition or wall polygon (the bake has no region ids), only the gate sites
	// (World::gateSites). A walled region is modelled as the circle round a town centre whose radius is its farthest gate, cut by
	// the inner side of each of that town's gates (gate -> nearest town centre, gatebind.hpp). A faithful version needs the
	// navmesh region id per polygon (the exe's gate-region graph, DOORS.md "GateCodes region graph").
	struct WalledRegion
	{
		struct Gate
		{
			double x = 0.0; // the gate's door centre
			double z = 0.0;
			double nx = 0.0; // unit normal pointing outside
			double nz = 0.0;
		};
		double x = 0.0;
		double z = 0.0;
		double radius = 0.0;
		std::vector<Gate> gates;
	};
	std::vector<WalledRegion> walled;
	std::uint64_t walledTick = ~std::uint64_t{0}; // the sim tick `walled` was built at (all ones = never)
	// The index of the walled region (x, z) stands in, -1 outdoors. Two characters with the same index stand in the same walled town: the
	// goal-34 "same town" test (task BZ). UNSURE: the exe's test is the character's Town object, not a gate region (BAR_FIGHT_LOOP.md).
	int walledRegion(double x, double z) const noexcept
	{
		for (std::size_t i = 0; i < walled.size(); ++i)
		{
			const WalledRegion& r = walled[i];
			if ((x - r.x) * (x - r.x) + (z - r.z) * (z - r.z) > r.radius * r.radius)
				continue;
			bool inside = true;
			for (const WalledRegion::Gate& g : r.gates)
				inside = inside && (x - g.x) * g.nx + (z - g.z) * g.nz <= 0.0;
			if (inside)
				return static_cast<int>(i);
		}
		return -1;
	}
	bool inWalls(double x, double z) const noexcept { return walledRegion(x, z) >= 0; }
	// The towns' circles: the exe's current town (towncircle.hpp, `Character vf+0x78` - dt24_34's `getTown(t) == myTown` and
	// dt24_144's home ring), the exe's radius vf+0x2a0. Built by the Simulation from state::TownState, refreshed with `walled`;
	// the walled regions are the gate circles, these are the town circles (a town without gates has one of these only).
	using TownCircle = towncircle::Circle;
	std::vector<TownCircle> towns;
	// The index of the town circle (x, z) stands in, -1 outdoors: the nearest centre among the containing circles (towncircle::at).
	int townOf(double x, double z) const noexcept { return towncircle::at(towns, x, z); }
	// The weather at a place (the region of the BIOME_GROUP under it, weather.hpp). Null = the two fields above stay as set.
	// The arena calls it with each observer's position before that observer's scan and stores the answer in them.
	std::function<void(double x, double z, int& type, double& strength)> weatherAt;
	// The burn of a wandering weather effect at a place (beams.hpp BeamField::burnAt): the strongest beam's affect type and
	// strength, strength 0 = none. Used by the arena's burn damage only. Null = no beams.
	std::function<void(double x, double z, int& type, double& strength)> beamAt;
	// Line of sight from one character's eye to the other's (the original: a Havok ray, eye +18), with their ids for the
	// per-pair cache. Null = nothing blocks sight. Set by the Simulation from ksim::LineOfSight (task AZ phase 2; the trees
	// exist only for regions at Full detail, so away from them it answers "visible").
	std::function<bool(std::uint64_t, const state::Fighter&, std::uint64_t, const state::Fighter&)> lineOfSight;
	// A shot's first blocker, muzzle point -> the target's body: the fraction of the segment (0..1) where a hull or the terrain stops it, or
	// a negative number when it is clear (ranged fire, task ranged). Null = nothing blocks a shot. Set with `lineOfSight`.
	std::function<double(const std::array<double, 3>&, const state::Fighter&)> shotBlocker;
	// The lamp light at a place: the sum of the lit building lights and carried lights in reach (the original's local
	// light sampler, 1.5 x power x (1 - d^2 / range^2)). Null = none. OPEN: the sim has no building or carried lights.
	std::function<double(double x, double z, std::uint64_t building)> lamps;
};

// ---- pure rules ----
// Daylight 0..1: 0 before sunrise and after sunset, a linear 2 h ramp after sunrise and before sunset.
double daylight(double dayFraction) noexcept;
// Vision range. `weatherAffected` is false for animals; `protection` is the clothing weather protection 0..1 (OPEN: 0).
double visionRange(double raceMult, bool indoor, const Environment& env, bool weatherAffected, double protection = 0.0) noexcept;
// Stealth fraction s: clamp(skill x (crouched ? 2 : 1) x (carrying ? 0.75 : 1) x mult / 100, 0, 1).
double stealthFraction(double skill, bool crouched, bool carrying = false, double mult = 1.0) noexcept;
// The cone factor c = (dot - 0.2) / 0.8, 0 outside the cone.
double coneFactor(double dot) noexcept;
// Visibility v for a cone dot product. `sneakish` is (sneak flag and not in combat) or crouched; `moving` the target's.
double visibility(double dot, bool sneakish, bool crouched, bool moving, double s) noexcept;
// A character's light: daylight when it is above 0.75 outdoors, else 1 when it is above 0.5, else 2 x ambient + lamps.
double characterLight(double ambient, bool indoor, double lamps) noexcept;
// The light terms A x B of the reach for a target light and an observer light.
double lightTerm(double targetLight, double observerLight) noexcept;
// The reach R v A B, capped at 0.5 R for an observer in combat.
double reach(double range, double v, double lightTerms, bool observerInCombat) noexcept;
// The target's noise range. 0 when it stands still out of combat. `run`: the run gait (else walking).
double noiseRange(const state::Fighter& target, const Environment& env, bool moving, bool inCombat) noexcept;
// The sneak meter's fill time in seconds: (0.5 + 5 s (1 - v)) x (4 - 3 attn).
double fillTime(double s, double v, double attention) noexcept;
// Seconds between two scans of one character when `characters` are loaded.
double scanInterval(std::size_t characters) noexcept;

// Moving / fighting, as perception reads them.
bool isMoving(const state::Fighter& f) noexcept;
bool inCombat(const state::Fighter& f) noexcept;
// (sneak flag and not in combat) or crouched.
bool isSneakish(const state::Fighter& f) noexcept;

// The fraction 0..1 of a scan interval a character's first scan is staggered by (a hash of its dice key).
double stagger(std::uint64_t id) noexcept;

// ---- records ----
const state::SightRecord* find(const state::Fighter& observer, std::uint64_t target) noexcept;
// The observer learns of the target outside a scan (an attacker targeting it: onTargetedBy). The record becomes noticed (state 1) whatever it
// was, a state-2 one included (140858f40). True when it was not noticed before (new, or promoted from suspicious / remembered).
bool notice(state::Fighter& observer, std::uint64_t target, bool known = true);
// The observer is aware of the target (a hostile one makes the squad attack).
bool aware(const state::Fighter& observer, std::uint64_t target) noexcept;
// The exe's SensorRecord seen_now: the last scan knew the target by sight (or a force-notice), never by the noise roll alone.
bool knownNow(const state::Fighter& observer, std::uint64_t target) noexcept;
// The exe's FRESH strength (rwh.212): SensorRecord +0x38 (seen_now) || +0x39 (sensed this update; sight, else hearing). The fight-start
// resolvers (goals 16, 15, 143) gate on it; a remembered record that the observer's last scan did not sense does not pass. The +0x39 flag
// is per scan: sinceSensed is zeroed by a scan that senses the target and only grows at that observer's next scan, so it reads 0 exactly
// until then (saved with the record: no extra state).
bool sensedNow(const state::Fighter& observer, std::uint64_t target) noexcept;
// Which sense holds the target in the observer's record (CHAR_STATS row 34 engageSense; values match SIMLINK.md): 0 no aware record, 1 sight
// (seen_now: known by sight or force-noticed), 2 hearing (sensed by the last scan's noise roll, not seen_now), 3 remembered (aware, but the
// last scan sensed nothing). 4, a shot from an unsensed attacker, is the fight's own and never returned here. sensedNow == (1 or 2).
enum class Sense : std::uint8_t { None = 0, Sight = 1, Hearing = 2, Remembered = 3 };
Sense senseOf(const state::Fighter& observer, std::uint64_t target) noexcept;
// Aware and sighted within `maxAge` seconds (the follow task's "leader perceived").
bool seenWithin(const state::Fighter& observer, std::uint64_t target, double maxAge) noexcept;
// A scan itself sensed the target (known or sighted) within `maxAge` seconds; the attacker's `notice` does not count. The chase
// give-up monitor's "sensed" (s73_static.md section 3: record bytes +0x38 / +0x39).
bool sensedWithin(const state::Fighter& observer, std::uint64_t target, double maxAge) noexcept;
// The observer drops its awareness of the target (the chase gave up). A scan that senses it again makes it aware again.
void forget(state::Fighter& observer, std::uint64_t target) noexcept;
// The player's detection meter for `target` over the observers (hostile to it): the highest meter, clamped to 0..1, and the
// state (0 none, 2 being noticed, 1 noticed; 1 reports meter 1).
struct Detection
{
	float meter = 0.0f;
	std::uint8_t state = 0;
};

struct Other
{
	std::uint64_t id = 0;
	const state::Fighter* fighter = nullptr;
};

struct Noticed
{
	std::uint64_t observer = 0;
	std::uint64_t target = 0;
};

// One scan by `observerId` after `dt` seconds. `others` is every character at full detail sorted by id (the observer may be
// among them). `visionMult` is the observer's race `vision range mult`. Appends the new "noticed" transitions.
void scan(std::uint64_t observerId, state::Fighter& observer, double visionMult, double dt, const std::vector<Other>& others,
	const Environment& env, std::vector<Noticed>& noticed);

// The detection of one record, for the wire.
Detection detection(const state::SightRecord& record) noexcept;

} // namespace ksim::perception
