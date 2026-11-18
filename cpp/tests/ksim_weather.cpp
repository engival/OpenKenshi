// The weather cycle (weather.hpp, docs/reference/WEATHER_CYCLE_LIVE.md "Ours"): season lengths, the weighted pick,
// durations, the single-weather rule, hour windows (including the wrap past midnight), strength, regions ticking
// unattended, a season change cutting the weather, the fallback for unknown ids on load, the wind direction of an
// instance, and save, load and continue.

#include "ksim/clock.hpp"
#include "ksim/simulation.hpp"
#include "ksim/weather.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using ksim::SeasonDef;
using ksim::SeasonWeather;
using ksim::WeatherDef;
using ksim::WeatherGroupDef;
using ksim::WeatherModel;
using ksim::state::WeatherRegion;

WeatherDef weatherDef(const std::string& sid, double start = 0.0, double end = 0.0, double effMin = 1.0, double effMax = 1.0)
{
	WeatherDef def;
	def.sid = sid;
	def.startHour = start;
	def.endHour = end;
	def.effectMin = effMin;
	def.effectMax = effMax;
	return def;
}

SeasonDef season(const std::string& sid, double order, double lengthWeight, std::vector<SeasonWeather> weathers,
	double strengthMin = 0.0, double strengthMax = 1.0)
{
	SeasonDef def;
	def.sid = sid;
	def.order = order;
	def.lengthWeight = lengthWeight;
	def.strengthMin = strengthMin;
	def.strengthMax = strengthMax;
	def.weathers = std::move(weathers);
	return def;
}

WeatherGroupDef group(std::vector<SeasonDef> seasons, double multMin = 1.0, double multMax = 1.0)
{
	WeatherGroupDef def;
	def.sid = "G";
	def.colour = 7;
	def.multiplierMin = multMin;
	def.multiplierMax = multMax;
	def.seasons = std::move(seasons);
	return def;
}

std::uint64_t tickOfMinute(std::uint64_t minute)
{
	return ksim::tickAt(minute / 1440, static_cast<int>(minute / 60 % 24), static_cast<int>(minute % 60));
}

// Steps one region minute by minute and calls onPick(region) for every new pick.
template <typename F>
void runMinutes(const WeatherModel& model, WeatherRegion& region, std::uint64_t from, std::uint64_t to, F&& onPick)
{
	for (std::uint64_t minute = from; minute < to; ++minute)
	{
		if (model.step(0, region, tickOfMinute(minute)))
			onPick(region);
	}
}

bool near(double a, double b, double tolerance)
{
	return std::fabs(a - b) <= tolerance;
}

void testSeasonLengths()
{
	const std::vector<WeatherDef> defs = {weatherDef("A")};
	const std::vector<SeasonWeather> list = {{"A", 1.0, 10, 20}};
	// 900 : 300 over 100 days.
	{
		const WeatherModel model(defs, {group({season("s1", 0, 900, list), season("s2", 1, 300, list)})}, 100);
		test::require(model.seasonDays(0) == std::vector<std::int64_t>({75, 25}), "900:300 is not 75 + 25 days");
	}
	// The last season takes the rounding remainder: 33 + 33 + 34.
	{
		const WeatherModel model(defs, {group({season("a", 0, 1, list), season("b", 1, 1, list), season("c", 2, 1, list)})}, 100);
		test::require(model.seasonDays(0) == std::vector<std::int64_t>({33, 33, 34}), "equal thirds are not 33 + 33 + 34");
	}
	// v1 < 1: daysPerYear / n (round(33.3) = 33); the next is round(5 / 10 x 100) = 50; the last is what is left.
	{
		const WeatherModel model(defs, {group({season("a", 0, 0, list), season("b", 1, 5, list), season("c", 2, 5, list)})}, 100);
		test::require(model.seasonDays(0) == std::vector<std::int64_t>({33, 50, 17}), "v1 < 1 season length is wrong");
	}
	// A short year never gives a zero-day season.
	{
		const WeatherModel model(defs, {group({season("a", 0, 1, list), season("b", 1, 1000, list)})}, 3);
		for (const std::int64_t days : model.seasonDays(0))
			test::require(days >= 1, "a season shorter than a day");
	}
	// Cycle order is v0; the tie keeps list order (WeatherModel::fromGameData sorts; the model keeps the order it is given).
	// Real data: every group's lengths add up to days per year.
	const kdata::GameData data = kworld::loadMergedGameData(KENSHI_DATA);
	const WeatherModel real = WeatherModel::fromGameData(data);
	test::require(real.daysPerYear() == 100, "days per year is not 100");
	test::require(!real.groups().empty(), "no weather groups in the data");
	for (std::size_t g = 0; g < real.groups().size(); ++g)
	{
		std::int64_t sum = 0;
		for (const std::int64_t days : real.seasonDays(g))
			sum += days;
		test::require(sum >= real.daysPerYear(), "season lengths of " + real.groups()[g].sid + " fall short of the year");
	}
	// The Desert group: two seasons, 900 : 300.
	bool desert = false;
	for (std::size_t g = 0; g < real.groups().size(); ++g)
	{
		if (real.groups()[g].seasons.size() == 2 && real.groups()[g].colour == 16777040)
		{
			test::require(real.seasonDays(g) == std::vector<std::int64_t>({75, 25}), "the Desert is not 75 + 25 days");
			desert = true;
		}
	}
	test::require(desert, "no Desert group with two seasons");
}

void testPickWeights()
{
	const std::vector<WeatherDef> defs = {weatherDef("A"), weatherDef("B"), weatherDef("C"), weatherDef("Z")};
	const WeatherModel model(defs, {group({season("s", 0, 1,
		{{"A", 1.0, 10, 10}, {"B", 2.0, 10, 10}, {"C", 7.0, 10, 10}, {"Z", 0.0, 10, 10}})})}, 100000);
	WeatherRegion region = model.freshRegion(0, 42);
	std::map<std::string, int> counts;
	int total = 0;
	runMinutes(model, region, 0, 120000, [&](const WeatherRegion& r)
	{
		++counts[r.weather];
		++total;
	});
	test::require(total > 10000, "too few picks: " + std::to_string(total));
	test::require(counts["Z"] == 0, "a v0 = 0 weather was picked");
	test::require(near(counts["A"] / double(total), 0.1, 0.02), "A is not about 10 %");
	test::require(near(counts["B"] / double(total), 0.2, 0.02), "B is not about 20 %");
	test::require(near(counts["C"] / double(total), 0.7, 0.02), "C is not about 70 %");
	// Repeats are allowed: a fresh instance of the same weather (previous == weather happens).
	WeatherRegion again = model.freshRegion(0, 43);
	bool repeat = false;
	runMinutes(model, again, 0, 5000, [&](const WeatherRegion& r) { repeat = repeat || r.weather == r.previous; });
	test::require(repeat, "no repeated weather in 500 picks");
}

void testDurations()
{
	const std::vector<WeatherDef> defs = {weatherDef("A"), weatherDef("B")};
	const WeatherModel model(defs, {group({season("s", 0, 1, {{"A", 1.0, 30, 120}, {"B", 1.0, 45, 60}})})}, 100000);
	WeatherRegion region = model.freshRegion(0, 5);
	std::int64_t lowest = 1 << 30;
	std::int64_t highest = 0;
	bool bad = false;
	runMinutes(model, region, 0, 200000, [&](const WeatherRegion& r)
	{
		const std::int64_t length = static_cast<std::int64_t>(r.endMinute - r.startMinute);
		const std::int64_t low = r.weather == "A" ? 30 : 45;
		const std::int64_t high = r.weather == "A" ? 120 : 60;
		bad = bad || length < low || length > high;
		if (r.weather == "A")
		{
			lowest = std::min(lowest, length);
			highest = std::max(highest, length);
		}
	});
	test::require(!bad, "a duration outside [v1, v2]");
	test::require(lowest <= 32 && highest >= 118, "the duration range is not covered (inclusive ends)");
}

void testSingleWeather()
{
	const std::vector<WeatherDef> defs = {weatherDef("A")};
	// One season of 100 days, one weather: v1 / v2 are ignored.
	const WeatherModel model(defs, {group({season("s", 0, 1, {{"A", 1.0, 5, 6}})})}, 100);
	WeatherRegion region = model.freshRegion(0, 1);
	std::vector<std::int64_t> lengths;
	runMinutes(model, region, 0, 20000, [&](const WeatherRegion& r)
	{
		lengths.push_back(static_cast<std::int64_t>(r.endMinute - r.startMinute));
	});
	test::require(lengths.size() >= 4, "a single-weather season did not repick");
	for (const std::int64_t length : lengths)
		test::require(length == 4364, "single-weather duration is " + std::to_string(length) + ", not 4364");
}

void testHourWindows()
{
	test::require(ksim::inHourWindow(8.0, 7.0, 22.0), "08:00 not in 07-22");
	test::require(!ksim::inHourWindow(23.0, 7.0, 22.0), "23:00 in 07-22");
	test::require(!ksim::inHourWindow(22.0, 7.0, 22.0), "22:00 in [07, 22)");
	// Wraps past midnight: 22 to 6.
	test::require(ksim::inHourWindow(23.0, 22.0, 6.0), "23:00 not in 22-06");
	test::require(ksim::inHourWindow(3.0, 22.0, 6.0), "03:00 not in 22-06");
	test::require(!ksim::inHourWindow(12.0, 22.0, 6.0), "12:00 in 22-06 (the original's bug: always true)");
	test::require(ksim::inHourWindow(12.0, 0.0, 0.0), "0-0 is no window");

	const std::vector<WeatherDef> defs = {weatherDef("Night", 22.0, 6.0), weatherDef("Any")};
	const WeatherModel model(defs, {group({season("s", 0, 1, {{"Night", 1.0, 10, 10}, {"Any", 1.0, 10, 10}})})}, 100000);
	int nightAtNoon = 0;
	int nightAtMidnight = 0;
	for (std::uint64_t seed = 1; seed <= 300; ++seed)
	{
		WeatherRegion noon = model.freshRegion(0, seed);
		model.step(0, noon, tickOfMinute(5 * 1440 + 12 * 60));
		nightAtNoon += noon.weather == "Night";
		WeatherRegion midnight = model.freshRegion(0, seed);
		model.step(0, midnight, tickOfMinute(5 * 1440 + 23 * 60));
		nightAtMidnight += midnight.weather == "Night";
	}
	test::require(nightAtNoon == 0, "a night weather started at noon");
	test::require(nightAtMidnight > 100 && nightAtMidnight < 200, "the night weather is not about half the picks at 23:00");
}

void testStrength()
{
	const std::vector<WeatherDef> defs = {weatherDef("A", 0.0, 0.0, 0.2, 0.4)};
	// limit in [0.5, 0.8], multiplier in [0.5, 1.0]: strength in [0.25, 0.8].
	const WeatherModel model(defs, {group({season("s", 0, 1, {{"A", 1.0, 10, 10}, {"A", 1.0, 10, 10}}, 0.5, 0.8)}, 0.5, 1.0)}, 100000);
	WeatherRegion region = model.freshRegion(0, 9);
	double lowest = 2.0;
	double highest = -1.0;
	double effLow = 2.0;
	double effHigh = -1.0;
	runMinutes(model, region, 0, 30000, [&](const WeatherRegion& r)
	{
		lowest = std::min(lowest, r.strength);
		highest = std::max(highest, r.strength);
		effLow = std::min(effLow, r.effectStrength);
		effHigh = std::max(effHigh, r.effectStrength);
	});
	test::require(lowest >= 0.25 && highest <= 0.8, "strength outside [0.25, 0.8]");
	test::require(lowest < 0.32 && highest > 0.72, "strength range not covered");
	test::require(effLow >= 0.2 && effHigh <= 0.4, "effect strength outside its own [min, max]");
	test::require(effHigh - effLow > 0.15, "effect strength does not vary (min read twice?)");
	// Constant for the instance's whole life.
	WeatherRegion held = model.freshRegion(0, 3);
	model.step(0, held, tickOfMinute(0));
	const double strength = held.strength;
	for (std::uint64_t minute = 1; minute < held.endMinute; ++minute)
		model.step(0, held, tickOfMinute(minute));
	test::require(held.strength == strength, "the strength changed within an instance");
	// Clamped to 1.
	const WeatherModel big(defs, {group({season("s", 0, 1, {{"A", 1.0, 10, 10}}, 1.0, 1.0)}, 3.0, 3.0)}, 100);
	WeatherRegion clamped = big.freshRegion(0, 1);
	big.step(0, clamped, tickOfMinute(0));
	test::require(clamped.strength == 1.0, "strength is not clamped to 1");
}

void testSeasonChangeCuts()
{
	const std::vector<WeatherDef> defs = {weatherDef("A"), weatherDef("B")};
	// Two seasons of 1 day each (dpy 2); A would last 5000 minutes.
	const WeatherModel model(defs, {group({season("s1", 0, 1, {{"A", 1.0, 5000, 5000}, {"A", 1.0, 5000, 5000}}),
		season("s2", 1, 1, {{"B", 1.0, 5000, 5000}, {"B", 1.0, 5000, 5000}})})}, 2);
	test::require(model.seasonDays(0) == std::vector<std::int64_t>({1, 1}), "two one-day seasons expected");
	WeatherRegion region = model.freshRegion(0, 2);
	test::require(model.step(0, region, tickOfMinute(10)), "first step did not pick");
	test::require(region.weather == "A" && region.season == 0 && region.seasonEndDay == 1, "first pick wrong");
	test::require(!model.step(0, region, tickOfMinute(1000)), "picked mid-weather");
	test::require(model.step(0, region, tickOfMinute(1440)), "the season end did not cut the weather");
	test::require(region.weather == "B" && region.previous == "A" && region.season == 1 && region.seasonEndDay == 2,
		"the new season did not start B after A");
	test::require(region.endMinute == 1440 + 5000, "the new instance runs from the cut");
	// The year wraps back to season 0.
	test::require(model.step(0, region, tickOfMinute(2 * 1440)), "second season end did not advance");
	test::require(region.season == 0 && region.weather == "A", "the cycle did not wrap to season 0");
	// A jump of many days advances once per tick.
	test::require(model.step(0, region, tickOfMinute(9 * 1440)), "no advance after a long jump");
	test::require(region.season == 1 && region.seasonEndDay == 10, "more than one advance in one step");
}

void testLoadFallback()
{
	const std::vector<WeatherDef> defs = {weatherDef("A")};
	const WeatherModel model(defs, {group({season("s", 0, 1, {{"A", 1.0, 10, 20}, {"A", 1.0, 10, 20}})})}, 100);
	std::vector<WeatherRegion> regions;
	WeatherRegion bad = model.freshRegion(0, 1);
	bad.season = 9;
	bad.weather = "Gone";
	bad.started = 1;
	bad.seasonEndDay = 77;
	const std::uint64_t rng = bad.rng;
	regions.push_back(bad);
	regions.push_back(WeatherRegion{"Unknown group"});
	model.reconcile(regions, 1);
	test::require(regions.size() == 1, "reconcile did not drop the unknown group");
	test::require(regions[0].started == 0 && regions[0].season == 0 && regions[0].weather.empty(),
		"an unknown season / weather id did not reset");
	test::require(regions[0].rng == rng, "the stream was not kept");
	test::require(model.step(0, regions[0], tickOfMinute(1440 * 5)), "no fresh pick after the fallback");
	test::require(regions[0].weather == "A" && regions[0].seasonEndDay == 105, "fallback pick wrong");
	// A missing group is added.
	std::vector<WeatherRegion> none;
	model.reconcile(none, 1);
	test::require(none.size() == 1 && none[0].group == "G", "a missing group was not added");
}

ksim::Simulation makeSim(const std::shared_ptr<const kdata::GameData>& data, std::uint64_t seed)
{
	ksim::Simulation sim = ksim::Simulation::load(KENSHI_DATA, seed);
	sim.setGameData(data);
	sim.setClockTick(ksim::tickAt(ksim::kNewGameDay, ksim::kNewGameHour, ksim::kNewGameMinute));
	sim.enableWeather(*data);
	return sim;
}

std::vector<WeatherRegion> regionsOf(const ksim::Simulation& sim)
{
	const ksim::state::WeatherRegions* regions = sim.world().ecs().get<ksim::state::WeatherRegions>();
	test::require(regions != nullptr, "no WeatherRegions singleton");
	return regions->regions;
}

void testUnattended(const std::shared_ptr<const kdata::GameData>& data)
{
	ksim::Simulation sim = makeSim(data, 11);
	test::require(sim.weather() != nullptr, "weather is not on");
	const std::size_t groups = sim.weather()->model().groups().size();
	test::require(regionsOf(sim).size() == groups, "one region per group expected");
	// Nobody is anywhere: no instances at all, yet every region ticks.
	sim.run(1);
	std::vector<std::string> first;
	for (const WeatherRegion& r : regionsOf(sim))
	{
		test::require(r.started == 1 && !r.weather.empty(), "region " + r.group + " has no weather after the first tick");
		first.push_back(r.weather);
	}
	const std::uint64_t picksBefore = sim.weather()->picks();
	sim.run(ksim::kTicksPerGameHour * 24 * 3); // three game days
	test::require(sim.weather()->picks() > picksBefore + groups, "the regions did not re-pick in three game days");
	std::size_t changed = 0;
	std::size_t index = 0;
	for (const WeatherRegion& r : regionsOf(sim))
		changed += r.weather != first[index++];
	test::require(changed > 0, "no region's weather changed in three game days");
	// Views: fade fractions in range, strengths in range.
	for (const ksim::WeatherView& view : sim.weather()->views(sim.world().ecs(), sim.clockTick()))
	{
		test::require(view.fogBlend >= 0.0 && view.fogBlend <= 1.0 && view.skyBlend >= 0.0 && view.skyBlend <= 1.0,
			"a blend outside 0..1");
		test::require(view.strength >= 0.0 && view.strength <= 1.0, "strength outside 0..1");
	}
}

void testSeasonsAdvanceInTheSim(const std::shared_ptr<const kdata::GameData>& data)
{
	// The shortest real season is the Desert's 25 days; the cycle clock is the sim clock, so check the end-day bookkeeping
	// against the day without running 25 game days: set the clock near the end day of the first started regions.
	ksim::Simulation sim = makeSim(data, 12);
	sim.run(1);
	std::uint64_t nearest = ~0ull;
	for (const WeatherRegion& r : regionsOf(sim))
		nearest = std::min(nearest, r.seasonEndDay);
	test::require(nearest != ~0ull && nearest > 0, "no season end day");
	sim.setClockTick(ksim::tickAt(nearest, 0, 0) - 1);
	sim.run(1); // the tick that reaches the end day
	bool advanced = false;
	for (const WeatherRegion& r : regionsOf(sim))
		advanced = advanced || (r.seasonEndDay > nearest && r.startMinute == nearest * 1440);
	test::require(advanced, "no season advanced on its end day");
}

void testSaveContinue(const std::shared_ptr<const kdata::GameData>& data)
{
	constexpr std::uint64_t kBefore = 150000; // about 1.1 game days: crosses weather picks
	constexpr std::uint64_t kAfter = 120000;
	ksim::Simulation straight = makeSim(data, 21);
	straight.run(kBefore + kAfter);

	ksim::Simulation first = makeSim(data, 21);
	first.run(kBefore);
	std::ostringstream saved;
	first.save(saved);
	std::istringstream in(saved.str());
	ksim::Simulation loaded = ksim::Simulation::restore(ksim::World::load(KENSHI_DATA), in, {}, false, nullptr, data);
	test::require(loaded.weather() != nullptr, "a loaded save with weather regions did not turn the weather on");
	test::require(loaded.stateHash() == first.stateHash(), "the hash changed across save and load");
	test::require(regionsOf(loaded) == regionsOf(first), "the regions changed across save and load");
	loaded.run(kAfter);
	test::require(regionsOf(loaded) == regionsOf(straight), "weather after save, load and continue differs from the straight run");
	test::require(loaded.stateHash() == straight.stateHash(), "state hash after save, load and continue differs");
	// A second save of the loaded run is byte-identical to the straight run's.
	std::ostringstream a;
	std::ostringstream b;
	loaded.save(a);
	straight.save(b);
	test::require(a.str() == b.str(), "the saves differ");
	// Different seed, different weather.
	ksim::Simulation other = makeSim(data, 22);
	other.run(kBefore + kAfter);
	test::require(regionsOf(other) != regionsOf(straight), "two seeds gave the same weather");
}

void testWindDirection(const std::shared_ptr<const kdata::GameData>& data)
{
	// The direction belongs to the weather instance and is a pure function of what the save holds: the same instance gives the
	// same yaw, a load the same, a new weather a new one. The values themselves are OURS (the exe's pick is not traced).
	const WeatherModel model({weatherDef("A"), weatherDef("B")},
		{group({season("s", 0, 1, {{"A", 1.0, 10, 10}, {"B", 1.0, 10, 10}})})}, 100);
	WeatherRegion region = model.freshRegion(0, 5);
	test::require(ksim::windYaw("G", region) == 0.0, "an unstarted region has a direction");
	model.step(0, region, tickOfMinute(600));
	const double yaw = ksim::windYaw("G", region);
	test::require(yaw >= 0.0 && yaw < 2.0 * std::numbers::pi, "a wind yaw outside [0, 2 pi)");
	test::require(ksim::windYaw("G", region) == yaw, "the same instance gave a different direction");
	WeatherRegion later = region;
	later.startMinute += 1;
	test::require(ksim::windYaw("G", later) != yaw, "a new instance kept the direction");
	WeatherRegion otherWeather = region;
	otherWeather.weather = region.weather == "A" ? "B" : "A";
	test::require(ksim::windYaw("G", otherWeather) != yaw, "another weather kept the direction");
	test::require(ksim::windYaw("H", region) != yaw, "another group kept the direction");

	// In the sim: only after a pick, in range for every region, the same on the same seed, the same across save and load.
	constexpr std::uint64_t kBefore = 150000; // crosses weather picks
	ksim::Simulation first = makeSim(data, 41);
	first.run(kBefore);
	const std::vector<WeatherRegion> regions = regionsOf(first);
	test::require(regions.size() > 1, "too few regions to compare");
	std::set<double> distinct;
	std::vector<double> yaws;
	for (const WeatherRegion& r : regions)
	{
		const double windYaw = ksim::windYaw(r.group, r);
		test::require(windYaw >= 0.0 && windYaw < 2.0 * std::numbers::pi, "a wind yaw outside [0, 2 pi)");
		test::require(windYaw == first.weather()->windYawOf(first.world().ecs(), r.group), "windYawOf differs from windYaw");
		const ksim::WeatherDef* def = first.weather()->model().weather(r.weather);
		test::require(def != nullptr, "a started region has no weather record");
		const double wind = first.weather()->windOf(first.world().ecs(), r.group);
		test::require(wind >= std::min(def->windMin, def->windMax) && wind <= std::max(def->windMin, def->windMax),
			"the wind is outside the weather's range");
		distinct.insert(windYaw);
		yaws.push_back(windYaw);
	}
	test::require(distinct.size() > 1, "every region has the same wind direction");
	test::require(first.weather()->windYawOf(first.world().ecs(), "no such group") == 0.0, "an unknown group has a direction");

	ksim::Simulation again = makeSim(data, 41);
	again.run(kBefore);
	const std::vector<WeatherRegion> same = regionsOf(again);
	test::require(same.size() == regions.size(), "the region count changed between two runs of one seed");
	for (std::size_t i = 0; i < same.size(); ++i)
		test::require(ksim::windYaw(same[i].group, same[i]) == yaws[i], "the same seed gave a different wind direction");

	std::ostringstream saved;
	first.save(saved);
	std::istringstream in(saved.str());
	ksim::Simulation loaded = ksim::Simulation::restore(ksim::World::load(KENSHI_DATA), in, {}, false, nullptr, data);
	test::require(loaded.weather() != nullptr, "a loaded save has no weather");
	const std::vector<WeatherRegion> after = regionsOf(loaded);
	test::require(after.size() == regions.size(), "the regions changed across save and load");
	for (std::size_t i = 0; i < after.size(); ++i)
		test::require(ksim::windYaw(after[i].group, after[i]) == yaws[i], "the wind direction changed across save and load");
}

void testGameplayLookup(const std::shared_ptr<const kdata::GameData>& data)
{
	ksim::Simulation sim = makeSim(data, 31);
	sim.run(1);
	int typed = 0;
	int strong = 0;
	for (double x = -640000.0; x < 640000.0; x += 18000.0)
	{
		for (double z = -640000.0; z < 640000.0; z += 18000.0)
		{
			int type = -1;
			double strength = -1.0;
			sim.weather()->at(x, z, type, strength);
			test::require(type >= 0 && type <= 5 && strength >= 0.0 && strength <= 1.0, "the lookup returned garbage");
			typed += type != 0;
			strong += strength > 0.0;
		}
	}
	test::require(strong > 0, "no point of the map has a weather strength");
	std::cout << "ksim_weather: " << strong << " sampled points with weather, " << typed << " with an affect type\n";
}

} // namespace

int main()
{
	try
	{
		testSeasonLengths();
		testPickWeights();
		testDurations();
		testSingleWeather();
		testHourWindows();
		testStrength();
		testSeasonChangeCuts();
		testLoadFallback();
		const auto data = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		testUnattended(data);
		testSeasonsAdvanceInTheSim(data);
		testSaveContinue(data);
		testWindDirection(data);
		testGameplayLookup(data);
	} catch (const std::exception& error) {
		std::cerr << "ksim_weather FAILED: " << error.what() << "\n";
		return 1;
	}
	std::cout << "ksim_weather passed\n";
	return 0;
}
