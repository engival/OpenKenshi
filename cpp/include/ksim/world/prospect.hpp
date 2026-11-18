#pragma once

// Prospecting (SIMLINK.md order 79 and PROSPECT 35; kinspect findings/prospecting.md and prosp19_static.md). A character stands where
// it is for kTicks ticks (state::ProspectTask), then the sim computes everything the exe's results window shows and tells the
// sessions. The maths below are pure functions of an ENVIRONMENT_RESOURCES record, an altitude and a position, so the test checks
// each one; the Simulation (prospect.cpp) supplies the zone, the terrain, the deposits and the wind.
//
// Resource ids are the exe's: 1 Iron, 2 Stone, 3 Copper, 5 Water, 6 Fertility (4 Carbon exists and is never listed).

#include <cstdint>
#include <string>
#include <vector>

namespace kdata {
struct Record;
}

namespace ksim::prospect {

// Ticks of the task: 1.0 sim-second at 0.05 s per tick (kinspect live timing; the exe adds the frame dt to a progress and ends at 1.0).
inline constexpr std::uint32_t kTicks = 20;

// Stone: the exe's five-octave value noise (FUN_1406cc0e0), ported from kinspect findings/stone26_static.md (docs/formats/SIMLINK.md
// PROSPECT "Implemented"). No seed, so the same for every world. The exe shares two globals with the grass painter: its "brightness
// boost" and "cap", 3.0 and 1.0 until a FOLIAGE_LAYER changes them (not replayed here; the fcs.def defaults are used).
inline constexpr double kGrassBrightnessBoost = 3.0;
inline constexpr double kGrassCap = 1.0;
inline constexpr int kStoneOctaves = 5;

inline constexpr std::uint8_t kIron = 1;
inline constexpr std::uint8_t kStone = 2;
inline constexpr std::uint8_t kCopper = 3;
inline constexpr std::uint8_t kWater = 5;
inline constexpr std::uint8_t kFertility = 6;

// Terrain classes of the Environment line (the exe's zone map keys): 1 Green, 2 Arid, 3 Swamp.
inline constexpr std::uint8_t kGreen = 1;
inline constexpr std::uint8_t kArid = 2;
inline constexpr std::uint8_t kSwamp = 3;

// The list shows the resources in this order.
inline constexpr std::uint8_t kListOrder[5] = {kWater, kFertility, kStone, kIron, kCopper};

// The map: N = int(6 + 2.46 science) pixels a side, covering a square of half-width r = 2000 + 6000 science / 100 around the
// prospector. Science is clamped to 0..100 for both (the exe's skills stop there; a larger number would only grow the frame).
inline constexpr double kMapBase = 6.0;
inline constexpr double kMapPerScience = 2.46;
inline constexpr double kRadiusBase = 2000.0;
inline constexpr double kRadiusPerScience = 60.0; // 6000 / 100
// The grey background: 0 at or below 500, 255 at 10000.
inline constexpr double kGreyLow = 500.0;
inline constexpr double kGreyHigh = 10000.0;
// A sample at or below this is not painted.
inline constexpr double kPaintFloor = 0.01;
// Deposits: the list value looks within 1500 (squared 2.25e6, full inside max(25, node radius)); the map draws those within 1.2 r.
inline constexpr double kDepositReach = 1500.0;
inline constexpr double kDepositMinRadius = 25.0;
inline constexpr double kMapDepositReach = 1.2;
// Not on solid ground below this terrain height (the navmesh's water level, knav::NavMesh::waterLevel; the sim has no ground-type
// notion, the exe tests the character's ground type == 0xb). UNSURE: ours.
inline constexpr double kWaterLevel = 100.0;

// An ENVIRONMENT_RESOURCES record (type 99), the keys the prospecting reads. Defaults are fcs.def's.
struct Env
{
	std::string sid;
	double waterMin = 0.1;
	double waterMult = 0.5;
	std::int32_t waterAltMin = 0;
	std::int32_t waterAltMax = 500;
	std::int32_t waterAltFade = 200;
	double farmingMin = 0.0;
	double farmingMult = 0.5;
	std::int32_t farmingAltMin = 0;
	std::int32_t farmingAltMax = 800;
	std::int32_t farmingAltFade = 300;
	double stoneMult = 1.0;
	double stoneNoiseZoom = 1.0;
	double stoneNoiseCutoff = 0.5;
	double ironMult = 1.0;
	double copperMult = 1.0;
	double green = 0.0; // the Environment line's fractions
	double arid = 0.0;
	double swamp = 0.0;

	static Env fromRecord(const kdata::Record& record);
};

// The exe's altitude trapezoid: 0 below `min - fade`, a linear ramp up to 1 at `min`, 1 on [min, max], a ramp down to 0 at `max + fade`.
// A fade of 0 or less makes the edges sharp.
double trap(double altitude, double min, double max, double fade) noexcept;
// Water and Fertility: trap(altitude) x mult + min. Not clamped (the list prints int(v x 100), so it can pass 100).
double waterAt(const Env& env, double altitude) noexcept;
double fertilityAt(const Env& env, double altitude) noexcept;
// Stone: noise(x, z, zoom x 1000, cutoff) x `stone mult`. stoneNoise takes the final zoom (the field x 1000) and returns [0, cap];
// stoneAt does the float multiplies the exe does. The list shows int(value x 100) with no clamp (it reaches 110 at mult 1.1).
double stoneNoise(double x, double z, double zoom, double cutoff, double boost = kGrassBrightnessBoost, double cap = kGrassCap) noexcept;
// The lattice hash alone (Hugo Elias form, 32-bit wraparound), a double in (-1, 1]; exposed for the test.
double latticeHash(int x, int z) noexcept;
double stoneAt(const Env& env, double x, double z) noexcept;
// The deposit's richness (the exe's vf0x2e0). UNSURE: the zone record's `iron mult` / `copper mult` (kinspect prospecting.md: "M: they
// scale the deposit"); the deposit placements carry no number of their own.
double depositRichness(const Env& env, std::uint8_t resource) noexcept;
// 1 - falloff for a deposit at squared distance d2 with a node radius: falloff = clamp((d2 - rin2) / (R2 - rin2), 0, 1), R = 1500,
// rin2 = min(max(radius^2, 625), R2).
double depositFactor(double d2, double radius) noexcept;
// Half-width r, map size N, grey and plane bytes.
double areaHalfWidth(double science) noexcept;
std::uint16_t mapSize(double science) noexcept;
std::uint8_t greyOf(double altitude) noexcept;
std::uint8_t planeByte(double value) noexcept; // round(clamp(v, 0, 1) x 255), 0 at or below kPaintFloor

struct Deposit
{
	std::uint8_t resource = 0;
	float x = 0.0f;
	float z = 0.0f;
	float richness = 0.0f;
};

// What PROSPECT 35 carries (SIMLINK.md), computed once when the task ends.
struct Result
{
	std::uint64_t id = 0; // the prospector
	float x = 0.0f;
	float z = 0.0f;
	float altitude = 0.0f;
	float science = 0.0f;
	float r = 0.0f;
	std::string zone; // BIOME_GROUP name; "" outside every group
	float wind = 0.0f;
	std::vector<std::pair<std::uint8_t, float>> env; // class, fraction; only those above 0
	std::vector<std::pair<std::uint8_t, float>> resources; // kListOrder, the raw point samples
	std::uint16_t n = 0;
	std::vector<std::uint8_t> grey; // n x n, row 0 = lowest z
	std::vector<std::uint8_t> water;
	std::vector<std::uint8_t> fertility;
	std::vector<std::uint8_t> stone;
	std::vector<Deposit> deposits;
};

} // namespace ksim::prospect
