#include "ksim/beams.hpp"

#include "ksim/clock.hpp"
#include "ksim/placement.hpp"
#include "kworld/coords.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ksim {

namespace {

// FNV-1a: a stable stringID hash, the same on every platform (as weather.cpp's).
std::uint64_t fnv1a(std::string_view text) noexcept
{
	std::uint64_t hash = 1469598103934665603ull;
	for (const unsigned char c : text)
		hash = (hash ^ c) * 1099511628211ull;
	return hash;
}

std::uint64_t startTickOf(std::uint64_t minute) noexcept
{
	return tickAt(minute / 1440, static_cast<int>(minute % 1440 / 60), static_cast<int>(minute % 60));
}

// A time in seconds as ticks, at least one.
std::uint64_t ticksOf(double seconds) noexcept
{
	return static_cast<std::uint64_t>(std::max(1.0, std::round(seconds / kTickSeconds)));
}

double rollRange(RngStream& rng, double low, double high) noexcept
{
	return high > low ? low + rng.uniform01() * (high - low) : low;
}

constexpr double kTwoPi = 2.0 * std::numbers::pi;
// The cell is entered 10 % in from each edge (the exe's random point in a random cell, rwh48 section 3).
constexpr double kCellInset = 0.1;
// A heading re-roll turns the beam by up to this much either way (OURS, UNSURE: the exe's turn is not traced).
constexpr double kMaxTurn = 0.5 * std::numbers::pi;

} // namespace

void beamStep(float& x, float& z, float speed, float yaw) noexcept
{
	const float length = speed / 20.0f;
	x += std::sin(yaw) * length;
	z += std::cos(yaw) * length;
}

void beamPositionAt(const Beam& beam, std::uint64_t tick, float& x, float& z) noexcept
{
	x = beam.x;
	z = beam.z;
	std::uint64_t left = tick > beam.spawnTick ? tick - beam.spawnTick : 0;
	for (const BeamSegment& segment : beam.segments)
	{
		for (std::uint16_t i = 0; i < segment.ticks && left > 0; ++i, --left)
			beamStep(x, z, beam.speed, segment.yaw);
	}
}

double beamBurn(const Beam& beam, double d2) noexcept
{
	const double r2 = static_cast<double>(beam.radius) * beam.radius;
	if (d2 <= r2)
		return beam.strength;
	if (d2 >= 2.0 * r2)
		return 0.0;
	return beam.strength * (1.0 - (d2 - r2) / r2);
}

Beam planBeam(const BeamPlanKey& key, const EffectDef& effect, const std::vector<int>& cells, std::uint32_t slot, std::uint32_t k,
	std::uint64_t spawnTick, std::uint64_t limitTick)
{
	Beam beam;
	if (cells.empty() || limitTick <= spawnTick)
		return beam;
	// Each part mixed before the xor so no field can cancel another (as windYaw).
	const std::uint64_t instance = splitMix64(key.seed) ^ splitMix64(fnv1a(key.group)) ^ splitMix64(fnv1a(key.weather))
		^ splitMix64(key.startMinute);
	RngStream rng(splitMix64(streamSeed(instance, kBeamStream) ^ splitMix64((static_cast<std::uint64_t>(slot) << 32) | k)));

	const std::uint64_t life = std::min(ticksOf(rollRange(rng, effect.ttlMin, effect.ttlMax)), limitTick - spawnTick);
	const kworld::ZoneCell cell = kworld::zoneCellOfIndex(cells[rng.next() % cells.size()]);
	const double inset = kCellInset + (1.0 - 2.0 * kCellInset) * rng.uniform01();
	const double insetZ = kCellInset + (1.0 - 2.0 * kCellInset) * rng.uniform01();

	beam.id = (slot << 16) | k;
	beam.effect = &effect;
	beam.spawnTick = spawnTick;
	beam.endTick = spawnTick + life;
	beam.x = static_cast<float>(kworld::zoneMinX(cell.x) + kworld::kZoneSize * inset);
	beam.z = static_cast<float>(kworld::zoneMinZ(cell.z) + kworld::kZoneSize * insetZ);
	beam.speed = static_cast<float>(effect.speed);
	beam.radius = static_cast<float>(effect.radius);
	beam.strength = static_cast<float>(rollRange(rng, effect.strengthMin, effect.strengthMax));

	// The heading is re-rolled every rand(ttl min, ttl max) x 0.1 s (1 to 3 s for SkyBeam).
	double yaw = rng.uniform01() * kTwoPi;
	for (std::uint64_t left = life; left > 0;)
	{
		const std::uint64_t ticks = std::min<std::uint64_t>(ticksOf(rollRange(rng, effect.ttlMin, effect.ttlMax) * 0.1), left);
		beam.segments.push_back({static_cast<std::uint16_t>(std::min<std::uint64_t>(ticks, 0xFFFF)), static_cast<float>(yaw)});
		left -= beam.segments.back().ticks;
		yaw = std::fmod(yaw + (2.0 * rng.uniform01() - 1.0) * kMaxTurn + kTwoPi, kTwoPi);
	}
	return beam;
}

// ---- BeamField ----

BeamField::BeamField(std::shared_ptr<const WeatherSystem> weather, std::uint64_t worldSeed)
	: weather_(std::move(weather))
	, seed_(worldSeed)
{
	groups_.resize(weather_->model().groups().size());
}

const std::vector<int>& BeamField::cellsOf(std::uint32_t colour)
{
	const auto found = cells_.find(colour);
	if (found != cells_.end())
		return found->second;
	// The group's zone cells: those whose centre lies in the group (the areas map, so a cell straddling the border counts by its centre).
	std::vector<int> cells;
	if (weather_->areas() != nullptr)
	{
		for (int index = 0; index < kworld::kZoneCount; ++index)
		{
			const kworld::ZoneCell cell = kworld::zoneCellOfIndex(index);
			const double cx = kworld::zoneMinX(cell.x) + 0.5 * kworld::kZoneSize;
			const double cz = kworld::zoneMinZ(cell.z) + 0.5 * kworld::kZoneSize;
			if (weather_->areas()->colourAt(cx, cz) == colour)
				cells.push_back(index);
		}
	}
	return cells_.emplace(colour, std::move(cells)).first->second;
}

void BeamField::rebuild(BeamGroup& group, std::size_t index, const state::WeatherRegion& region, const WeatherDef& def, std::uint64_t now)
{
	const WeatherModel& model = weather_->model();
	group.key = {seed_, region.group, region.weather, region.startMinute};
	group.colour = model.groups()[index].colour;
	group.limitTick = UINT64_MAX;
	if (!region.held)
	{
		const std::uint64_t end = std::min(region.endMinute, region.seasonEndDay * 1440);
		group.limitTick = startTickOf(end);
	}
	group.slots.clear();
	const BeamPlanKey& key = group.key;
	const std::vector<int>& cells = cellsOf(group.colour);
	const std::uint64_t start = startTickOf(region.startMinute);
	std::uint32_t slot = 0;
	for (const EffectRef& ref : def.effects)
	{
		const EffectDef* effect = model.effect(ref.effect);
		if (effect == nullptr || effect->type != kEffectWandering || effect->affectType != kAffectBurn)
			continue;
		for (int i = 0; i < ref.count; ++i, ++slot)
		{
			BeamSlot& entry = group.slots.emplace_back();
			entry.effect = effect;
			entry.current = planBeam(key, *effect, cells, slot, 0, start, group.limitTick);
			entry.next = planBeam(key, *effect, cells, slot, 1, entry.current.endTick, group.limitTick);
		}
	}
	for (BeamSlot& entry : group.slots)
	{
		entry.px = entry.current.x;
		entry.pz = entry.current.z;
		entry.posTick = entry.current.spawnTick;
	}
	advance(group, now);
	group.epoch = ++changes_;
}

void BeamField::advance(BeamGroup& group, std::uint64_t now)
{
	const BeamPlanKey& key = group.key;
	const std::vector<int>& cells = cellsOf(group.colour);
	for (std::size_t s = 0; s < group.slots.size(); ++s)
	{
		BeamSlot& slot = group.slots[s];
		// A beam that ended hands over to the planned one, which plans its successor; a chain that reaches the weather's end stays null.
		while (slot.current.effect != nullptr && now >= slot.current.endTick)
		{
			const std::uint32_t k = (slot.current.id & 0xFFFF) + 2;
			slot.current = std::move(slot.next);
			slot.next = slot.current.effect != nullptr
				? planBeam(key, *slot.effect, cells, static_cast<std::uint32_t>(s), k, slot.current.endTick, group.limitTick)
				: Beam{};
			slot.px = slot.current.x;
			slot.pz = slot.current.z;
			slot.posTick = slot.current.spawnTick;
			slot.segment = 0;
			slot.used = 0;
		}
		if (slot.current.effect == nullptr)
			continue;
		// Walk to `now` one tick at a time, the same sum beamPositionAt does (the first call after a load walks the whole life so far).
		while (slot.posTick < now && slot.segment < slot.current.segments.size())
		{
			const BeamSegment& segment = slot.current.segments[slot.segment];
			beamStep(slot.px, slot.pz, slot.current.speed, segment.yaw);
			++slot.posTick;
			if (++slot.used >= segment.ticks)
			{
				++slot.segment;
				slot.used = 0;
			}
		}
	}
}

void BeamField::step(const flecs::world& ecs, std::uint64_t now)
{
	const state::WeatherRegions* regions = ecs.get<state::WeatherRegions>();
	if (regions == nullptr)
		return;
	const WeatherModel& model = weather_->model();
	for (const state::WeatherRegion& region : regions->regions)
	{
		const std::ptrdiff_t index = model.groupIndex(region.group);
		if (index < 0)
			continue;
		BeamGroup& group = groups_[static_cast<std::size_t>(index)];
		const WeatherDef* def = region.started != 0 && !region.weather.empty() ? model.weather(region.weather) : nullptr;
		const bool wanders = def != nullptr && std::any_of(def->effects.begin(), def->effects.end(), [&](const EffectRef& ref)
			{
				const EffectDef* effect = model.effect(ref.effect);
				return effect != nullptr && effect->type == kEffectWandering && effect->affectType == kAffectBurn && ref.count > 0;
			});
		if (!wanders)
		{
			if (!group.key.weather.empty())
			{
				group = BeamGroup{};
				group.epoch = ++changes_;
			}
			continue;
		}
		const std::uint64_t limit = region.held ? UINT64_MAX
			: startTickOf(std::min(region.endMinute, region.seasonEndDay * 1440));
		if (group.key.weather != region.weather || group.key.startMinute != region.startMinute || group.limitTick != limit)
			rebuild(group, static_cast<std::size_t>(index), region, *def, now);
		else
			advance(group, now);
	}
}

double BeamField::burnAt(double x, double z, int& type) const noexcept
{
	double best = 0.0;
	type = 0;
	for (const BeamGroup& group : groups_)
	{
		for (const BeamSlot& slot : group.slots)
		{
			const Beam& beam = slot.current;
			if (beam.effect == nullptr || slot.posTick < beam.spawnTick || slot.posTick >= beam.endTick)
				continue;
			const double dx = x - slot.px;
			const double dz = z - slot.pz;
			const double burn = beamBurn(beam, dx * dx + dz * dz);
			if (burn > best)
			{
				best = burn;
				type = beam.effect->affectType;
			}
		}
	}
	return best;
}

} // namespace ksim
