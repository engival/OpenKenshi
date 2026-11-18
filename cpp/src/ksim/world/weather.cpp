#include "ksim/world/weather.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "ksim/nav/placement.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace ksim {

namespace {

double clamp01(double value) noexcept
{
	return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

// FNV-1a: a stable stringID hash, the same on every platform (as trade.cpp's).
std::uint64_t fnv1a(std::string_view text) noexcept
{
	std::uint64_t hash = 1469598103934665603ull;
	for (const unsigned char c : text)
		hash = (hash ^ c) * 1099511628211ull;
	return hash;
}

// rand(low, high) as the original uses it: low + (high - low) x [0, 1).
double rollRange(RngStream& rng, double low, double high)
{
	return low + (high - low) * rng.uniform01();
}

} // namespace

bool inHourWindow(double hour, double start, double end) noexcept
{
	if (start == end)
		return true;
	if (start < end)
		return hour >= start && hour < end;
	return hour >= start || hour < end; // wraps past midnight
}

std::uint64_t nextHourMinute(std::uint64_t minute, double hour) noexcept
{
	const auto target = static_cast<std::uint64_t>(std::ceil(hour * 60.0)) % 1440;
	const std::uint64_t today = minute / 1440 * 1440 + target;
	return today > minute ? today : today + 1440;
}

std::uint64_t gameMinuteAt(std::uint64_t tick) noexcept
{
	const GameTime time = gameTimeAt(tick);
	return time.day * 1440 + static_cast<std::uint64_t>(time.hour) * 60 + static_cast<std::uint64_t>(time.minute);
}

double windYaw(std::string_view group, const state::WeatherRegion& region) noexcept
{
	if (region.started == 0 || region.weather.empty())
		return 0.0;
	// One draw per instance. Each part is mixed before the xor so no field can cancel another; the saved (group, weather,
	// startMinute) is all it reads, so a load gives the same direction and a new weather a new one.
	const std::uint64_t key = splitMix64(fnv1a(group)) ^ splitMix64(fnv1a(region.weather)) ^ splitMix64(region.startMinute);
	return RngStream(streamSeed(key, kWeatherWindStream)).uniform01() * 2.0 * std::numbers::pi;
}

// ---- WeatherModel ----

WeatherModel::WeatherModel(std::vector<WeatherDef> weathers, std::vector<WeatherGroupDef> groups, std::int64_t daysPerYear)
	: weathers_(std::move(weathers))
	, daysPerYear_(daysPerYear < 1 ? 1 : daysPerYear)
{
	for (std::size_t i = 0; i < weathers_.size(); ++i)
		weatherIndex_.emplace(weathers_[i].sid, i);
	for (WeatherGroupDef& group : groups)
	{
		if (!group.seasons.empty())
			groups_.push_back(std::move(group));
	}
	for (std::size_t g = 0; g < groups_.size(); ++g)
	{
		groupIndex_.emplace(groups_[g].sid, g);
		const std::vector<SeasonDef>& seasons = groups_[g].seasons;
		const std::int64_t n = static_cast<std::int64_t>(seasons.size());
		double total = 0.0;
		for (const SeasonDef& season : seasons)
			total += std::max(season.lengthWeight, 0.0);
		std::vector<std::int64_t> days;
		std::int64_t used = 0;
		for (std::int64_t i = 0; i < n; ++i)
		{
			std::int64_t length;
			if (i == n - 1)
				length = daysPerYear_ - used; // the rounding remainder
			else if (seasons[i].lengthWeight < 1.0)
				length = static_cast<std::int64_t>(std::llround(static_cast<double>(daysPerYear_) / static_cast<double>(n)));
			else
				length = static_cast<std::int64_t>(std::llround(seasons[i].lengthWeight / total * static_cast<double>(daysPerYear_)));
			length = std::max<std::int64_t>(length, 1);
			used += length;
			days.push_back(length);
		}
		days_.push_back(std::move(days));
	}
}

WeatherModel WeatherModel::fromGameData(const kdata::GameData& gameData)
{
	std::vector<WeatherDef> weathers;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("WEATHER")))
	{
		WeatherDef def;
		def.sid = record->stringID;
		def.startHour = record->floatOr("start time", 0.0f);
		def.endHour = record->floatOr("end time", 0.0f);
		def.effectMin = record->floatOr("effect strength min", 1.0f);
		def.effectMax = record->floatOr("effect strength max", 1.0f);
		def.affectType = record->intOr("affect type", 0);
		def.windMin = record->floatOr("wind speed min", 0.0f);
		def.windMax = record->floatOr("wind speed max", 0.0f);
		if (const kdata::RefCategory* list = record->findRefs("effects"))
		{
			for (const kdata::RefEntry& entry : list->entries)
				def.effects.push_back({entry.targetID, entry.v0});
		}
		weathers.push_back(std::move(def));
	}
	std::vector<EffectDef> effects;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("EFFECT")))
	{
		EffectDef def;
		def.sid = record->stringID;
		def.type = record->intOr("type", 0);
		def.affectType = record->intOr("affect type", 0);
		def.ttlMin = record->floatOr("min time to live", 0.0f);
		def.ttlMax = record->floatOr("max time to live", 0.0f);
		def.speed = record->floatOr("wandering speed", 0.0f);
		def.radius = record->floatOr("effect radius", 0.0f);
		def.strengthMin = record->floatOr("effect strength min", 0.0f);
		def.strengthMax = record->floatOr("effect strength max", 0.0f);
		def.particles = record->stringOr("particle system");
		if (const kdata::RefCategory* list = record->findRefs("sound"); list != nullptr && !list->entries.empty())
		{
			if (const kdata::Record* sound = gameData.find(list->entries.front().targetID))
				def.sound = sound->name;
		}
		effects.push_back(std::move(def));
	}
	std::int64_t daysPerYear = 100;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("CONSTANTS")))
	{
		if (record->name == "GLOBAL CONSTANTS")
			daysPerYear = record->intOr("days per year", 100);
	}
	std::vector<WeatherGroupDef> groups;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("BIOME_GROUP")))
	{
		const kdata::RefCategory* refs = record->findRefs("seasons");
		if (refs == nullptr)
			continue;
		WeatherGroupDef group;
		group.sid = record->stringID;
		group.colour = static_cast<std::uint32_t>(record->intOr("index", 0));
		group.multiplierMin = record->floatOr("weather strength multiplier min", 1.0f);
		group.multiplierMax = record->floatOr("weather strength multiplier max", 1.0f);
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* seasonRecord = entry.opaqueEntry ? nullptr : gameData.find(entry.targetID);
			if (seasonRecord == nullptr)
				continue;
			SeasonDef season;
			season.sid = seasonRecord->stringID;
			season.order = entry.v0;
			season.lengthWeight = entry.v1;
			season.strengthMin = seasonRecord->floatOr("weather strength limit min", 0.0f);
			season.strengthMax = seasonRecord->floatOr("weather strength limit max", 1.0f);
			if (const kdata::RefCategory* list = seasonRecord->findRefs("weathers"))
			{
				for (const kdata::RefEntry& w : list->entries)
				{
					if (!w.opaqueEntry)
						season.weathers.push_back({w.targetID, static_cast<double>(w.v0), w.v1, w.v2});
				}
			}
			group.seasons.push_back(std::move(season));
		}
		// Cycle order is v0; a tie keeps the list order (the exe's pointer order is not reproducible: UNSURE).
		std::stable_sort(group.seasons.begin(), group.seasons.end(),
			[](const SeasonDef& a, const SeasonDef& b) { return a.order < b.order; });
		groups.push_back(std::move(group));
	}
	WeatherModel model(std::move(weathers), std::move(groups), daysPerYear);
	model.setEffects(std::move(effects));
	return model;
}

void WeatherModel::setEffects(std::vector<EffectDef> effects)
{
	effects_ = std::move(effects);
	effectIndex_.clear();
	for (std::size_t i = 0; i < effects_.size(); ++i)
		effectIndex_.emplace(effects_[i].sid, i);
}

const EffectDef* WeatherModel::effect(std::string_view sid) const
{
	const auto found = effectIndex_.find(std::string(sid));
	return found == effectIndex_.end() ? nullptr : &effects_[found->second];
}

const WeatherDef* WeatherModel::weather(std::string_view sid) const
{
	const auto found = weatherIndex_.find(std::string(sid));
	return found == weatherIndex_.end() ? nullptr : &weathers_[found->second];
}

std::ptrdiff_t WeatherModel::groupIndex(std::string_view sid) const
{
	const auto found = groupIndex_.find(std::string(sid));
	return found == groupIndex_.end() ? -1 : static_cast<std::ptrdiff_t>(found->second);
}

state::WeatherRegion WeatherModel::freshRegion(std::size_t group, std::uint64_t worldSeed) const
{
	state::WeatherRegion region;
	region.group = groups_[group].sid;
	region.rng = RngStream(streamSeed(streamSeed(worldSeed, kWeatherStream), groups_[group].colour)).state();
	return region;
}

void WeatherModel::reconcile(std::vector<state::WeatherRegion>& regions, std::uint64_t worldSeed) const
{
	std::vector<state::WeatherRegion> out;
	out.reserve(groups_.size());
	for (std::size_t g = 0; g < groups_.size(); ++g)
	{
		const auto found = std::find_if(regions.begin(), regions.end(),
			[&](const state::WeatherRegion& r) { return r.group == groups_[g].sid; });
		if (found == regions.end())
		{
			out.push_back(freshRegion(g, worldSeed));
			continue;
		}
		state::WeatherRegion region = *found;
		const bool known = region.season < groups_[g].seasons.size()
			&& (region.weather.empty() || weather(region.weather) != nullptr);
		if (!known)
		{
			region.season = 0;
			region.seasonEndDay = 0;
			region.weather.clear();
			region.previous.clear();
			region.started = 0;
		}
		if (!region.previous.empty() && weather(region.previous) == nullptr)
			region.previous.clear();
		out.push_back(std::move(region));
	}
	regions = std::move(out);
}

void WeatherModel::startWeather(std::size_t group, state::WeatherRegion& region, std::uint64_t minute, bool keepPrevious) const
{
	const SeasonDef& season = groups_[group].seasons[region.season];
	RngStream rng(region.rng);
	if (keepPrevious)
		region.previous = region.weather;
	else
		region.previous.clear();

	// Candidates: v0 > 0, a known record, and the current hour inside the weather's hour window (the exe's picker, kinspect
	// rwh48_venge_beams_static.md section 2). Nothing eligible gives the null "off" weather for 5 to 60 minutes, then a new pick.
	const double hour = static_cast<double>(minute % 1440) / 60.0;
	std::vector<const SeasonWeather*> candidates;
	for (const SeasonWeather& entry : season.weathers)
	{
		const WeatherDef* def = weather(entry.weather);
		if (def != nullptr && entry.chance > 0.0 && inHourWindow(hour, def->startHour, def->endHour))
			candidates.push_back(&entry);
	}
	region.startMinute = minute;
	if (candidates.empty())
	{
		region.weather.clear();
		region.endMinute = minute + static_cast<std::uint64_t>(rng.between(5, 60));
		region.strength = 0.0;
		region.effectStrength = 0.0;
		region.rng = rng.state();
		return;
	}

	const SeasonWeather* chosen = candidates.front();
	if (candidates.size() > 1)
	{
		double total = 0.0;
		for (const SeasonWeather* entry : candidates)
			total += entry->chance;
		double roll = rng.uniform01() * total;
		chosen = candidates.back();
		for (const SeasonWeather* entry : candidates)
		{
			if (roll < entry->chance)
			{
				chosen = entry;
				break;
			}
			roll -= entry->chance;
		}
	}

	std::int64_t duration;
	if (season.weathers.size() == 1)
		duration = static_cast<std::int64_t>(std::ceil(kSingleWeatherMinutes * static_cast<double>(days_[group][region.season]) / 60.0));
	else
		duration = rng.between(chosen->minMinutes, chosen->maxMinutes);
	duration = std::max<std::int64_t>(duration, 1);

	const WeatherDef& def = *weather(chosen->weather);
	const WeatherGroupDef& g = groups_[group];
	region.effectStrength = rollRange(rng, def.effectMin, def.effectMax);
	const double limit = rollRange(rng, season.strengthMin, season.strengthMax);
	const double multiplier = rollRange(rng, g.multiplierMin, g.multiplierMax);
	region.strength = clamp01(limit * multiplier);
	region.weather = chosen->weather;
	region.endMinute = minute + static_cast<std::uint64_t>(duration);
	// A weather with an hour window ends no later than the next end hour (the exe clamps the instance end to it).
	if (def.startHour != def.endHour)
		region.endMinute = std::min(region.endMinute, nextHourMinute(minute, def.endHour));
	region.rng = rng.state();
}

bool WeatherModel::step(std::size_t group, state::WeatherRegion& region, std::uint64_t now) const
{
	const std::uint64_t minute = gameMinuteAt(now);
	const std::uint64_t day = minute / 1440;
	const std::vector<std::int64_t>& days = days_[group];
	if (!region.started)
	{
		// A fresh region is in season 0; a released debug pin is not, and picks from the season the clock is in.
		region.seasonEndDay = day + static_cast<std::uint64_t>(days[region.season]);
		region.started = 1;
		startWeather(group, region, minute, false);
		return true;
	}
	if (region.held)
	{
		// A debug pin holds the weather past its season end and its own lifetime (SIMLINK "DEBUG_SET_WEATHER"). The season
		// still advances with the calendar, so a release picks from the season the clock is in.
		if (day >= region.seasonEndDay)
		{
			region.season = static_cast<std::uint32_t>((region.season + 1) % groups_[group].seasons.size());
			region.seasonEndDay = day + static_cast<std::uint64_t>(days[region.season]);
		}
		return false;
	}
	if (day >= region.seasonEndDay)
	{
		// At most one advance per tick; a season change cuts the weather.
		region.season = static_cast<std::uint32_t>((region.season + 1) % groups_[group].seasons.size());
		region.seasonEndDay = day + static_cast<std::uint64_t>(days[region.season]);
		startWeather(group, region, minute, true);
		return true;
	}
	if (minute >= region.endMinute)
	{
		startWeather(group, region, minute, true);
		return true;
	}
	return false;
}

// ---- WeatherSystem ----

WeatherSystem::WeatherSystem(WeatherModel model, std::shared_ptr<const AreaLookup> areas)
	: model_(std::move(model))
	, areas_(std::move(areas))
{
	for (std::size_t g = 0; g < model_.groups().size(); ++g)
		byColour_.emplace(model_.groups()[g].colour, g);
	cells_.resize(model_.groups().size());
}

WeatherSystem::~WeatherSystem() = default;

std::shared_ptr<WeatherSystem> WeatherSystem::build(const kdata::GameData& gameData, std::string_view dataDir)
{
	std::shared_ptr<const AreaLookup> areas;
	try
	{
		areas = std::make_shared<const AreaLookup>(dataDir);
	} catch (const std::exception&) {
		// No areas map: the cycle still runs, only the gameplay lookup is off.
	}
	return std::make_shared<WeatherSystem>(WeatherModel::fromGameData(gameData), std::move(areas));
}

void WeatherSystem::attach(flecs::world& ecs, std::uint64_t worldSeed)
{
	state::WeatherRegions regions;
	if (const state::WeatherRegions* saved = ecs.get<state::WeatherRegions>())
		regions = *saved;
	model_.reconcile(regions.regions, worldSeed);
	refreshCells(regions.regions);
	ecs.set<state::WeatherRegions>(std::move(regions));
}

std::size_t WeatherSystem::step(flecs::world& ecs, std::uint64_t now)
{
	state::WeatherRegions* regions = ecs.get_mut<state::WeatherRegions>();
	if (regions == nullptr || regions->regions.size() != model_.groups().size())
		return 0;
	std::size_t picked = 0;
	for (std::size_t g = 0; g < regions->regions.size(); ++g)
	{
		if (model_.step(g, regions->regions[g], now))
			++picked;
	}
	if (picked != 0)
	{
		picks_ += picked;
		refreshCells(regions->regions);
	}
	return picked;
}

bool WeatherSystem::pin(flecs::world& ecs, std::string_view group, std::string_view weatherSid, std::uint64_t now,
	double strength, bool hold, bool keepPrevious)
{
	state::WeatherRegions* regions = ecs.get_mut<state::WeatherRegions>();
	const std::ptrdiff_t g = model_.groupIndex(group);
	if (regions == nullptr || g < 0 || regions->regions.size() != model_.groups().size() || model_.weather(weatherSid) == nullptr)
		return false;
	state::WeatherRegion& region = regions->regions[static_cast<std::size_t>(g)];
	if (!region.started)
		model_.step(static_cast<std::size_t>(g), region, now); // the season and its end day
	const std::uint64_t minute = gameMinuteAt(now);
	if (keepPrevious)
		region.previous = region.weather;
	else
		region.previous.clear();
	region.weather = std::string(weatherSid);
	region.startMinute = minute;
	region.endMinute = minute + 10 * 1440;
	region.strength = std::clamp(strength, 0.0, 1.0);
	region.effectStrength = region.strength;
	region.started = 1;
	region.held = hold ? 1 : 0;
	++picks_;
	refreshCells(regions->regions);
	return true;
}

bool WeatherSystem::releasePin(flecs::world& ecs, std::string_view group)
{
	state::WeatherRegions* regions = ecs.get_mut<state::WeatherRegions>();
	const std::ptrdiff_t g = model_.groupIndex(group);
	if (regions == nullptr || g < 0 || regions->regions.size() != model_.groups().size())
		return false;
	state::WeatherRegion& region = regions->regions[static_cast<std::size_t>(g)];
	region.held = 0;
	region.started = 0; // the next step picks fresh, from the region's current season
	return true;
}

std::string WeatherSystem::groupAt(double x, double z) const
{
	if (areas_ == nullptr)
		return {};
	const auto found = byColour_.find(areas_->colourAt(x, z));
	return found == byColour_.end() ? std::string() : model_.groups()[found->second].sid;
}

void WeatherSystem::refreshCells(const std::vector<state::WeatherRegion>& regions)
{
	cells_.assign(regions.size(), Cell{});
	for (std::size_t g = 0; g < regions.size() && g < cells_.size(); ++g)
	{
		const WeatherDef* def = model_.weather(regions[g].weather);
		if (def == nullptr)
			continue;
		// `affect type` is WeatherAffecting directly for 1 (dust) and 2 (acid), the only values in the data; rain intensity
		// does not map to type 5 (UNSURE), so those stay 0.
		cells_[g].type = std::clamp(def->affectType, 0, 5);
		cells_[g].strength = regions[g].strength;
	}
}

std::vector<WeatherView> WeatherSystem::views(const flecs::world& ecs, std::uint64_t now) const
{
	std::vector<WeatherView> out;
	const state::WeatherRegions* regions = ecs.get<state::WeatherRegions>();
	if (regions == nullptr)
		return out;
	const double minutes = static_cast<double>(now) / kTicksPerGameMinute;
	for (const state::WeatherRegion& region : regions->regions)
	{
		WeatherView view;
		view.group = region.group;
		view.weather = region.weather;
		view.previous = region.previous;
		view.strength = region.strength;
		if (!region.weather.empty())
		{
			const double start = static_cast<double>(region.startMinute);
			const double span = static_cast<double>(region.endMinute) - start;
			view.fogBlend = span > 0.0 ? clamp01((minutes - start) / (kWeatherFogFraction * span)) : 1.0;
			view.skyBlend = clamp01((static_cast<double>(now) - start * kTicksPerGameMinute) * kTickSeconds / kWeatherSkySeconds);
		}
		out.push_back(std::move(view));
	}
	return out;
}

double WeatherSystem::windOf(const flecs::world& ecs, std::string_view group) const
{
	const state::WeatherRegions* regions = ecs.get<state::WeatherRegions>();
	if (regions == nullptr)
		return 0.0;
	for (const state::WeatherRegion& region : regions->regions)
	{
		if (region.group != group)
			continue;
		const WeatherDef* def = region.started != 0 ? model_.weather(region.weather) : nullptr;
		return def != nullptr ? def->windMin + (def->windMax - def->windMin) * region.strength : 0.0;
	}
	return 0.0;
}

double WeatherSystem::windYawOf(const flecs::world& ecs, std::string_view group) const
{
	const state::WeatherRegions* regions = ecs.get<state::WeatherRegions>();
	if (regions == nullptr)
		return 0.0;
	for (const state::WeatherRegion& region : regions->regions)
	{
		if (region.group == group)
			return windYaw(group, region);
	}
	return 0.0;
}

void WeatherSystem::at(double x, double z, int& type, double& strength) const
{
	type = 0;
	strength = 0.0;
	if (areas_ == nullptr)
		return;
	const auto found = byColour_.find(areas_->colourAt(x, z));
	if (found == byColour_.end() || found->second >= cells_.size())
		return;
	type = cells_[found->second].type;
	strength = cells_[found->second].strength;
}

} // namespace ksim
