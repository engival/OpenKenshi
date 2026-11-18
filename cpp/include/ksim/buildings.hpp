#pragma once

// Player buildings (docs/reference/BUILDINGS_SLICE.md, Phases 1-2): the pure rules. State is state::PlayerBuilding / PlayerTown;
// the order checks, the entity creation and the per-tick step are Simulation::checkBuildPlace / runBuildPlace / stepConstruction
// (buildings.cpp). Everything here is a function of its arguments, so ksim_buildings_build checks each one on worked values.

#include "ksim/state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kdata {
struct Record;
class GameData;
}

namespace ksim::buildings {

// The live build-time constant (adv22 `DAT_142133f38`, read 6.0 live; its writer was not found in the exe, L). The sim's own copy of
// the build21 estimate and the construction rate: hours = T / (build speed mult * Ka * Kb), Ka the new-game `bs` knob.
inline constexpr double kKb = 6.0;
// The `bs` knob's range and default (ADVANCED_OPTIONS_STATIC.md).
inline constexpr double kKaDefault = 1.0;
inline constexpr double kKaMin = 0.5;
inline constexpr double kKaMax = 2.0;

// The item a record with no `construction` list but `build materials` N > 0 needs N of (BUILDINGS_SLICE.md section 4, build21: the
// "Building Material" row).
inline constexpr const char* kBuildingMaterialItem = "580-gamedata.base";

// The town test (item50_buildings.md Q2, Q3; H, FUN_1404dba40 / FUN_140926d50): a site is inside the nearest town T when
// dist < radius(T) x k or dist < kTownFloor; k is 1.0 for RUINS (4) and POI (9), else kTownK (2.5). A record whose `creates player
// town` is false is re-tested at k = 1.0 with no floor. The faction's own towns never block. UNSURE: the sub-town override (0.6 r /
// 0.9 r, M) only changes which town is reported and is not built.
inline constexpr double kTownFloor = 2000.0;
inline constexpr double kTownK = 2.5;
// A town's `type`: 0 NEST, 1 OUTPOST, 2 TOWN, 3 VILLAGE, 4 RUINS, 5 SLAVE_CAMP, 6 MILITARY, 7 PRISON, 8 NEST_MARKER (never a town for this
// test), 9 POI, 10 NULL.
inline constexpr int kTownTypeRuins = 4;
inline constexpr int kTownTypeMarker = 8;
inline constexpr int kTownTypePoi = 9;
// A player town (Q3, Q19; Town::vf75): radius = max over its buildings of (distance from the centre + 200), clamped to [1000, 20000];
// the centre is the mean of the buildings' positions; a radius at or below 1 reads as 2500. Another faction's player town counts as a
// TOWN-type town in the test above (UNSURE: its type is not read).
inline constexpr double kPlayerTownMargin = 200.0;
inline constexpr double kPlayerTownMin = 1000.0;
inline constexpr double kPlayerTownMax = 20000.0;
inline constexpr double kPlayerTownDefault = 2500.0;
// UNSURE: a faction's building joins its own nearest town when the site is within this of the town's centre; farther, a new town is made
// when the record `creates player town`.
inline constexpr double kOwnTownReach = 2000.0;
// Q4 (H): a part whose BUILDING_PART `ground type` is 1 (LAND) needs the footprint box centre's world Y at or above this (sea level 100 - 2).
inline constexpr double kLandMinY = 98.0;
inline constexpr int kGroundTypeLand = 1;
// Q5 (H): the terrain normal's y must be at least this, whatever the record says.
inline constexpr double kMinNormalY = 0.2;
// Q6 (H): builders add up with a damping of 0.75^k on the k-th (k from 0, at most 15): 1, 1.75, 2.31, 2.73, ... limit 4.
inline constexpr double kBuilderDamping = 0.75;
inline constexpr unsigned kBuilderMax = 16;
// build20 Q6: the terrain height may differ from the part's expected base by up to 40.0.
inline constexpr double kHeightTolerance = 40.0;
inline constexpr std::uint8_t kMaxFloor = 3;
// A quaternion off unit length by more than this is `bad_value`.
inline constexpr double kQuatTolerance = 0.05;
inline constexpr const char* kDefaultTownName = "Your Outpost"; // H, build20 Q5

// The section 7 enum so far: the construction states, and 5 / 6 for a producer's panel (Simulation::producerView; the rest come with Phases 5-7).
enum BuildStatus : std::uint8_t
{
	kWorking = 0,
	kBroken = 1,
	kUnderConstruction = 2,
	kNeedsMaterials = 3,
	kPaused = 4,
	kOutputFull = 5,
	kNoWorkers = 6,
};

// ---- the records ----

// `construction` (item, count) in list order; with none and `build materials` > 0 one kBuildingMaterialItem row; else no rows.
std::vector<state::BuildMaterial> buildingCost(const kdata::Record& building);
// T of the menu formula: the sum of the needs.
std::uint32_t totalMaterials(const std::vector<state::BuildMaterial>& mats) noexcept;
// door_plan.hpp doorMaxHealth: Σ construction counts, else `build materials`, else 1.0 (H for doors, M for every building; Q9).
double maxHp(const kdata::Record& building) noexcept;
// T / (build speed mult * Ka * Kb) game hours: what the HP rate is derived from. 0 when T is 0 (the site is born complete).
double buildHours(std::uint32_t total, double buildSpeedMult, double ka, double kb = kKb) noexcept;
// The build menu's "Estimated build time" (whole hours, round half up; build21).
int estimatedHours(std::uint32_t total, double buildSpeedMult, double ka, double kb = kKb) noexcept;
// Why a record cannot be placed yet: "unsupported_site" for a kind whose placement tests are not built (mines, farms, snapping and
// interior pieces, walls and gates, nodes), "" otherwise. `functionality` may be null.
const char* siteSupport(const kdata::Record& building, const kdata::Record* functionality) noexcept;

// ---- construction ----

// Progress 0..1 (1 when built).
double progressOf(const state::PlayerBuilding& b) noexcept;
// HP the delivered materials allow: min(maxHp, Σ min(need, have)) (Q8, H; a site with no material rows is not capped).
double materialCap(const state::PlayerBuilding& b) noexcept;
// HP gained per tick by `builders` working (Σ builder factors) on a site: (maxHp / buildHours) / ticksPerGameHour * builders.
double hpPerTick(double maxHp, double buildHours, double builders) noexcept;
// Whether construction is making progress this tick: not built, not paused, a builder present, HP below the cap.
bool working(const state::PlayerBuilding& b) noexcept;
// Progress per sim-second at the current state (the BUILDING frame's `rate`): 0 when stalled, paused or built.
double rateOf(const state::PlayerBuilding& b, double buildHours) noexcept;
// One tick of construction: HP gains hpPerTick up to the material cap; the site completes when hp reaches maxHp with every need
// delivered (built = true, hp = maxHp, paused cleared). A site with no HP to gain (maxHp 0 rows) is not touched here. Returns whether
// anything changed.
bool stepSite(state::PlayerBuilding& b, double buildHours, double builders) noexcept;
// ---- the Build / Add-materials / Dismantle tasks (ds/construction; b50c_static Q6-Q10, Q18) ----

// CS.rate's divisor: the construction rate is `build speed mult` / (G / 24), G = 2618.18 (DAT_1416fd540, exe), so one game hour is 109.09.
inline constexpr double kRateDivisor = 2618.18 / 24.0;
// The ruin revival (Q8, H): the first build call on a ruin sets HP to 0.4 max and keeps floor(delivered x U(0.3, 0.5) + 0.5) of each material.
inline constexpr double kRuinHpFraction = 0.4;
inline constexpr double kRuinKeepLow = 0.3;
inline constexpr double kRuinKeepHigh = 0.5;
// Task_AddMaterialsToBuilding's warm-up (Q7, H): progress += dt x 0.2 each tick, nothing moves before 1.0 (5 s).
inline constexpr double kAddMaterialsWarmup = 0.2;
// Task_Dismantle's work (Q18, H): dt x lerp(stat / 100, 0.05, 4.0) x 4.0, then x rate in removeHp.
inline constexpr double kDismantleLow = 0.05;
inline constexpr double kDismantleHigh = 4.0;
inline constexpr double kDismantleScale = 4.0;
// "min dismantle materials percentage" / 100 (GlobalConstants, base data 60): the refund is max(1, (int)(U(m, 1) x delivered)) per material.
inline constexpr double kMinDismantleFraction = 0.6;

// Whether a build tick has HP to add: not built (or damaged), the delivered materials still cover more than the HP (b50c Q8: FUN_14054a3f0
// is the "impossible" test). A ruin always can (its first build call revives it).
bool buildable(const state::PlayerBuilding& b) noexcept;
// A building the Build task has nothing left to do on: built, whole, not a ruin.
bool whole(const state::PlayerBuilding& b) noexcept;
// Material rows still short (need - have > 0), list order (FUN_14054a450).
std::vector<std::size_t> missingRows(const state::PlayerBuilding& b);
// The ruin revival, on the first build call (Q8). `u0` / `u1` ... are U(0.3, 0.5) draws taken from `draw`, one per material row.
template <typename Draw>
void revive(state::PlayerBuilding& b, Draw&& draw)
{
	b.ruin = false;
	b.built = false;
	b.broken = false;
	b.hp = kRuinHpFraction * b.maxHp;
	for (state::BuildMaterial& row : b.mats)
		row.have = static_cast<std::uint32_t>(row.have * (kRuinKeepLow + (kRuinKeepHigh - kRuinKeepLow) * draw()) + 0.5);
}
// vf73, remove HP (damage and dismantle share it; Q10, Q18): a built building at or over max drops to max - 1 and stops being built
// (production waits for a builder to finish it), then HP falls by `damage` x `rate`. Returns true when HP reached 0: the building is
// dismantled for good (`dismantled`, not built) and the caller drops the refund.
bool removeHp(state::PlayerBuilding& b, double damage, double rate) noexcept;
// The refund of a destroyed building (FUN_14029dc10): per material with units delivered, max(1, (int)(U x have)) with U in [fraction, 1).
// `draw` returns U(0, 1). Empty when the building was already dismantled or has no delivered material.
template <typename Draw>
std::vector<state::BuildMaterial> refundOf(const state::PlayerBuilding& b, double fraction, Draw&& draw)
{
	std::vector<state::BuildMaterial> out;
	for (const state::BuildMaterial& row : b.mats)
	{
		if (row.have == 0)
			continue;
		const double u = fraction + (1.0 - fraction) * draw();
		state::BuildMaterial one = row;
		one.have = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(u * row.have));
		out.push_back(one);
	}
	return out;
}
// Units of `item` the site still accepts (need - have); 0 for an unknown item or a built site.
std::uint32_t accepts(const state::PlayerBuilding& b, const std::string& item) noexcept;
// BuildStatus of the site now (the subset of section 7 that exists in Phases 1-2).
std::uint8_t statusOf(const state::PlayerBuilding& b) noexcept;
// A hash of everything the BUILDING record shows except hp: version bumps when it changes (the client extrapolates hp from the rate).
std::uint64_t signatureOf(const state::PlayerBuilding& b) noexcept;
// The BUILDING frame's flags word (section 12): bit 0 built, 1 paused, 2 broken, 3 switchedOn, 4 runOffBattery, 5 repairWanted,
// 6 dismantling. Bits 7-11 come with their phases.
std::uint32_t flagsOf(const state::PlayerBuilding& b) noexcept;
// The navmesh follows a building once it is built past `build threshold` percent (knav::affectsNavmesh's rule). The sim holds no
// NavWorld to tell (the placed building's collision shapes are not registered with one), so this is the number a future hook reads.
bool navActive(double progress, double buildThresholdPercent) noexcept;

// ---- the town and the ground ----

// The radius of a TOWN record's town: `town radius mult` (below 0.1 reads 1.0, then clamped to [0.1, 5]) times the base of its `type`.
double townBaseRadius(int type) noexcept;
double townRadius(int type, double radiusMult) noexcept;
// Whether a site at `distance` from a town of `type` and `radius` is refused (the rule above). `createsPlayerTown` is the record's flag.
bool townBlocks(int type, double radius, double distance, bool createsPlayerTown) noexcept;
// A player town's centre and radius from its buildings' (x, z): the mean and max (dist + 200) clamped; no buildings reads (0, 0) radius 2500.
struct TownShape
{
	double x = 0.0;
	double z = 0.0;
	double radius = kPlayerTownDefault;
};
TownShape playerTownShape(const std::vector<std::pair<double, double>>& buildings) noexcept;
// ---- the player town's size level (kenshi-51x.6; rwh93_town_level_static.md, FUN_14048c200) ----

// BUILDING_FUNCTIONALITY `function` ids the level reads (the game's BuildingFunction enum).
inline constexpr std::int32_t kFunctionResearch = 3;
inline constexpr std::int32_t kFunctionTurret = 12;
inline constexpr std::int32_t kFunctionGate = 21;
// The four counters over a town's finished buildings: T the highest research bench `tech level`, S the shells (an interior, not a wall, not a
// gate), U the turrets, G the gates.
struct TownCounters
{
	int research = 0;
	int shells = 0;
	int turrets = 0;
	int gates = 0;
};
// Adds one building of `record` to the counters.
void countTownBuilding(const kdata::GameData& data, const kdata::Record& record, TownCounters& counters);
// The level 0-3 from the counters: level 1 for T >= 2 || S >= 2 || G >= 1 || U >= 4; with a gate, 2 for T >= 4 || S >= 5 || U >= 5, and 3 for
// T >= 4 && S >= 5 && U >= 7.
int townLevelOf(const TownCounters& counters) noexcept;
// The level of the town the buildings `members` make: finished (built, not a ruin, not dismantled) buildings only.
int townLevel(const kdata::GameData& data, const std::vector<const state::PlayerBuilding*>& members);

// Q4: whether the ground refuses a site: any part with ground type LAND and `centreY` below kLandMinY.
bool groundBlocked(const std::vector<int>& partGroundTypes, double centreY) noexcept;
// Q5: whether the slope refuses a site (every failure says "Ground needs to be more level"). The terrain normal is the cross product of the
// footprint's two diagonals from its four corner heights, taken in the order (-a, -b), (+a, -b), (+a, +b), (-a, +b) about the centre; it is
// refused when normal.y < kMinNormalY, or when `matchSlope` and the tilt from up exceeds `maxSlopeDeg`. With `matchSlope` false the tilt is
// clamped to `maxSlopeDeg` instead (never refused). The slope factor is 1.0 (terrain-aligned, the exe's default).
bool slopeBlocked(const double cornerHeights[4], double halfA, double halfB, bool matchSlope, double maxSlopeDeg) noexcept;
// Σ 0.75^k for the k-th builder, k < 16.
double builderFactor(unsigned builders) noexcept;
// Q6 with the skill term: w = 0.3 + 1.7 x clamp(stat, 0, 100) / 100 per builder (the Engineering stat), summed as Sum w_k x 0.75^k in the given
// order. The live damping order is message order (kinspect 52); the Build task (Phase 3) must feed a stable one, by character Id.
double skillMult(double stat) noexcept;
double builderSum(const std::vector<double>& stats) noexcept;

// ---- placement geometry ----

// An oriented box on the plane, centre (cx, cz), unit axis A = (ax, az), B = (-az, ax), half extents along each, and a height range.
struct Box
{
	double cx = 0.0;
	double cz = 0.0;
	double ax = 1.0;
	double az = 0.0;
	double halfA = 0.0;
	double halfB = 0.0;
	double minY = 0.0;
	double maxY = 0.0;
};

// A building's footprint in its own frame (yaw 0 at the origin): the union box of its collision shapes. `valid` false = the record
// has no collision (path mode IGNORE or no parts): the placement uses a small disc (kNoFootprintHalf).
struct Footprint
{
	bool valid = false;
	double minX = 0.0;
	double maxX = 0.0;
	double minZ = 0.0;
	double maxZ = 0.0;
	double minY = 0.0;
	double maxY = 0.0;
};
inline constexpr double kNoFootprintHalf = 1.0;

// The yaw axis of a W-first quaternion: its rotated x axis projected on the plane, normalised ((1, 0) for a degenerate one).
void yawAxis(double qw, double qx, double qy, double qz, double& ax, double& az) noexcept;
// The footprint placed at (x, y, z) with that rotation (upright: the tilt is not applied, Q5).
Box placeBox(const Footprint& footprint, double x, double y, double z, double qw, double qx, double qy, double qz) noexcept;
// Separating-axis test of two boxes on the plane plus a height-range overlap.
bool boxesOverlap(const Box& a, const Box& b) noexcept;
bool pointInBox(const Box& box, double x, double y, double z) noexcept;

} // namespace ksim::buildings
