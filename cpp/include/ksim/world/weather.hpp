#pragma once

// The weather cycle (docs/reference/WEATHER_CYCLE_LIVE.md "Ours", ARCHITECTURE.md "Weather").
//
// One saved region (state::WeatherRegion) per BIOME_GROUP that lists seasons. Every region ticks on the sim clock whether or
// not anyone is in the group:
//   - Seasons: the group's `seasons` references (v0 = order, v1 = length weight), sorted by v0 with list order breaking ties.
//     Length in days = round(v1 / sum(v1) x daysPerYear); a season with v1 < 1 gets daysPerYear / n; the last takes the
//     rounding remainder (at least 1). A season ends when day >= its end day; the weather is cut and re-picked.
//   - Pick: the SEASON's `weathers` references (v0 = chance, v1 / v2 = duration range in game minutes). Weighted by v0, only
//     v0 > 0, repeats allowed. A season with exactly one weather lasts ceil(2618.18 x seasonDays / 60) minutes instead.
//     A WEATHER record with an hour window (`start time` / `end time`, both 0 = none) is skipped outside it; a window that
//     wraps past midnight is tested correctly (the original's test always passes: not copied).
//   - Strength = clamp01(rand(season `weather strength limit` min, max) x rand(group `weather strength multiplier` min, max)),
//     rolled once per instance. "effect strength" has its own min / max (the original reads min twice: not copied).
//
// The rolls come from one seeded stream per group (kWeatherStream, state saved in the region), so a save, load and continue
// is identical. The model (WeatherModel) is derived from game data and never saved.
//
// The wind direction (windYaw) is its own seeded draw per instance, a pure function of the saved instance (OURS: the exe's pick is
// not traced); it goes on the WEATHER frame with the speed, for the camera group (SIMLINK "WEATHER").

#include "ksim/core/clock.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/core/state.hpp"

#include <flecs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
}

namespace ksim {

// SIMLINK WEATHER: the fog fade runs over this share of the new weather's lifetime, the sky fade over this many seconds
// of game-speed time (WEATHER_CYCLE_LIVE.md section 4).
inline constexpr double kWeatherFogFraction = 0.025;
inline constexpr double kWeatherSkySeconds = 30.0;
// A season with one weather: minutes = ceil(kSingleWeatherMinutes x seasonDays / 60) (4364 for 100 days; exact live).
inline constexpr double kSingleWeatherMinutes = 2618.18;

// A WEATHER `effects` reference: the EFFECT and how many handlers run at once (v0; v1 / v2 unused by the beams).
struct EffectRef
{
	std::string effect; // EFFECT stringID
	int count = 0;
};

// An EFFECT record as the wandering effects read it (docs/reference/VENGE_BEAMS_STATIC.md): type 9 wanders over the group's cells.
struct EffectDef
{
	std::string sid;
	int type = 0;
	int affectType = 0; // WeatherAffecting type the burn uses (3 for SkyBeam)
	double ttlMin = 0.0; // `min time to live`, `max time to live` (s)
	double ttlMax = 0.0;
	double speed = 0.0; // `wandering speed` (u/s)
	double radius = 0.0; // `effect radius` (u)
	double strengthMin = 0.0; // `effect strength min` / `max`
	double strengthMax = 0.0;
	std::string particles; // `particle system`
	std::string sound; // the `sound` reference's record name, empty = none
};

inline constexpr int kEffectWandering = 9;
// WeatherAffecting type 3: the burn (Venge's sky beams). The other wandering effects in the data (affect type 4, the poison gas clouds) are not
// planned or sent: their damage is not decoded (VENGE_BEAMS_STATIC.md Questions).
inline constexpr int kAffectBurn = 3;

struct WeatherDef
{
	std::string sid;
	double startHour = 0.0; // `start time`; start == end means no hour window
	double endHour = 0.0; // `end time`
	double effectMin = 1.0; // `effect strength min`
	double effectMax = 1.0; // `effect strength max`
	int affectType = 0; // `affect type`: 0 none, 1 dust, 2 acid (WeatherAffecting; UNSURE for others)
	double windMin = 0.0; // `wind speed min`
	double windMax = 0.0; // `wind speed max`: the wind is lerp(min, max, strength) (WEATHER_CYCLE_LIVE.md section 5)
	std::vector<EffectRef> effects; // the `effects` references
};

struct SeasonWeather
{
	std::string weather; // WEATHER stringID
	double chance = 0.0; // v0
	std::int64_t minMinutes = 0; // v1
	std::int64_t maxMinutes = 0; // v2
};

struct SeasonDef
{
	std::string sid;
	double order = 0.0; // v0 of the group's reference
	double lengthWeight = 0.0; // v1 of the group's reference
	double strengthMin = 0.0; // `weather strength limit min`
	double strengthMax = 1.0;
	std::vector<SeasonWeather> weathers;
};

struct WeatherGroupDef
{
	std::string sid;
	std::uint32_t colour = 0; // BIOME_GROUP `index`: the pixel colour in the areas map
	double multiplierMin = 1.0; // `weather strength multiplier min`
	double multiplierMax = 1.0;
	std::vector<SeasonDef> seasons; // in cycle order
};

// A region's current instance as the client sees it (SIMLINK WEATHER).
struct WeatherView
{
	std::string group;
	std::string weather;
	std::string previous;
	double fogBlend = 1.0;
	double skyBlend = 1.0;
	double strength = 0.0;
};

class WeatherModel
{
public:
	// groups: seasons already in cycle order. Computes the season lengths from daysPerYear.
	WeatherModel(std::vector<WeatherDef> weathers, std::vector<WeatherGroupDef> groups, std::int64_t daysPerYear);
	// From the merged game data: WEATHER, SEASON, BIOME_GROUP and GLOBAL CONSTANTS `days per year`.
	static WeatherModel fromGameData(const kdata::GameData& gameData);

	std::int64_t daysPerYear() const noexcept { return daysPerYear_; }
	const std::vector<WeatherGroupDef>& groups() const noexcept { return groups_; }
	// Season lengths in days, parallel to groups()[g].seasons.
	const std::vector<std::int64_t>& seasonDays(std::size_t group) const { return days_[group]; }
	const WeatherDef* weather(std::string_view sid) const;
	// The EFFECT records the weathers reference (fromGameData fills them; a hand-built model sets them with setEffects).
	void setEffects(std::vector<EffectDef> effects);
	const EffectDef* effect(std::string_view sid) const;
	// Index of the group, -1 when unknown.
	std::ptrdiff_t groupIndex(std::string_view sid) const;

	// A fresh, unstarted region for group g (its stream seeded from worldSeed).
	state::WeatherRegion freshRegion(std::size_t group, std::uint64_t worldSeed) const;
	// Brings a (loaded) region list to the model: one region per group in group order, an unknown season or weather id gives
	// season 0 and a fresh pick (started = 0), the stream kept.
	void reconcile(std::vector<state::WeatherRegion>& regions, std::uint64_t worldSeed) const;
	// One tick of one region at absolute tick `now`. True when it picked a new weather.
	bool step(std::size_t group, state::WeatherRegion& region, std::uint64_t now) const;

private:
	void startWeather(std::size_t group, state::WeatherRegion& region, std::uint64_t minute, bool keepPrevious) const;

	std::vector<WeatherDef> weathers_;
	std::unordered_map<std::string, std::size_t> weatherIndex_;
	std::vector<EffectDef> effects_;
	std::unordered_map<std::string, std::size_t> effectIndex_;
	std::vector<WeatherGroupDef> groups_;
	std::unordered_map<std::string, std::size_t> groupIndex_;
	std::vector<std::vector<std::int64_t>> days_;
	std::int64_t daysPerYear_ = 100;
};

// Whether `hour` (0..24) lies in the window [start, end). A window with start >= end wraps past midnight. start == end is no
// window at all (always true).
bool inHourWindow(double hour, double start, double end) noexcept;

// The first game minute after `minute` whose hour of day is `hour` (0..24, fractional hours round up to the minute).
std::uint64_t nextHourMinute(std::uint64_t minute, double hour) noexcept;

// The game minute of an absolute tick (clock.hpp's calendar).
std::uint64_t gameMinuteAt(std::uint64_t tick) noexcept;

// The direction a region's wind blows TOWARD, as a yaw in radians [0, 2 pi) on the character yaw convention (the direction is
// (sin yaw, cos yaw) in world XZ, 0 = +Z; SIMLINK "WEATHER"). A pure function of the saved instance (group, weather, start minute),
// so a load gives the same direction and a new weather a new one; 0 when the region has no started weather. OURS/UNSURE: the exe
// keeps a vec3 on the weather instance (kinspect weather_effects section 4, +0x1c..+0x24) but its pick is not traced.
double windYaw(std::string_view group, const state::WeatherRegion& region) noexcept;

class AreaLookup;

// The model plus the areas map: what the Simulation holds. Ticks the singleton state::WeatherRegions.
class WeatherSystem
{
public:
	// areas may be null (no gameplay lookup, the cycle still runs).
	WeatherSystem(WeatherModel model, std::shared_ptr<const AreaLookup> areas);
	~WeatherSystem();
	WeatherSystem(const WeatherSystem&) = delete;
	WeatherSystem& operator=(const WeatherSystem&) = delete;

	static std::shared_ptr<WeatherSystem> build(const kdata::GameData& gameData, std::string_view dataDir);

	const WeatherModel& model() const noexcept { return model_; }

	// Sets state::WeatherRegions when absent (one fresh region per group); reconciles it with the model when present (a
	// loaded save). Call between ticks.
	void attach(flecs::world& ecs, std::uint64_t worldSeed);
	// Called every tick with the new clock tick. Returns the number of regions that picked a new weather.
	std::size_t step(flecs::world& ecs, std::uint64_t now);
	// New picks since construction (a metric, not saved): the server resends the WEATHER frame when it changes.
	std::uint64_t picks() const noexcept { return picks_; }

	// Every region's state at tick `now`, in region order.
	std::vector<WeatherView> views(const flecs::world& ecs, std::uint64_t now) const;
	// The areas map the gameplay lookup reads (null when it did not load).
	const std::shared_ptr<const AreaLookup>& areas() const noexcept { return areas_; }
	// Scenario setup (scenario_file.hpp) and the debug view: makes `weatherSid` the current weather of the group's region from
	// tick `now` on, strength `strength` (clamped to 0..1). The scenario pin lasts ten game days (a season end still cuts it);
	// `hold` keeps it past both (the debug pin), and `keepPrevious` sets `previous` to the replaced weather (the debug pin's
	// fade). False when the group has no region or the weather is unknown. Between ticks.
	bool pin(flecs::world& ecs, std::string_view group, std::string_view weatherSid, std::uint64_t now,
		double strength = 1.0, bool hold = false, bool keepPrevious = false);
	// The debug pin's release: clears the hold and makes the region pick fresh from its current season at the next step.
	// False when the group has no region.
	bool releasePin(flecs::world& ecs, std::string_view group);
	// The BIOME_GROUP a world point lies in (the areas map's colour -> group); empty when there is no areas map or the point is
	// in no group with a region.
	std::string groupAt(double x, double z) const;
	// The current wind of a group's region: lerp(`wind speed min`, `wind speed max`, strength) of its weather; 0 when the group has no
	// region or no weather yet (the exe: 0.0 with no weather state). Raw units: the prospecting window prints x 0.22369 mph.
	double windOf(const flecs::world& ecs, std::string_view group) const;
	// The current wind direction of a group's region (windYaw of its region); 0 when the group has no region or no weather yet.
	double windYawOf(const flecs::world& ecs, std::string_view group) const;
	// Gameplay: WeatherAffecting type and strength at a world point (0, 0.0 when the point is in no region or the weather has
	// no affect type).
	void at(double x, double z, int& type, double& strength) const;

private:
	struct Cell
	{
		int type = 0;
		double strength = 0.0;
	};
	void refreshCells(const std::vector<state::WeatherRegion>& regions);

	WeatherModel model_;
	std::shared_ptr<const AreaLookup> areas_;
	std::unordered_map<std::uint32_t, std::size_t> byColour_; // colour -> group index
	std::vector<Cell> cells_;
	std::uint64_t picks_ = 0;
};

} // namespace ksim
