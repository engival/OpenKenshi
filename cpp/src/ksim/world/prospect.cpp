#include "ksim/world/prospect.hpp"

#include "ksim/nav/placement.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ksim
{

namespace prospect
{

Env Env::fromRecord(const kdata::Record& record)
{
	Env env;
	env.sid = record.stringID;
	env.waterMin = record.floatOr("water min", 0.1f);
	env.waterMult = record.floatOr("water mult", 0.5f);
	env.waterAltMin = record.intOr("water altitude min", 0);
	env.waterAltMax = record.intOr("water altitude max", 500);
	env.waterAltFade = record.intOr("water altitude fade", 200);
	env.farmingMin = record.floatOr("farming min", 0.0f);
	env.farmingMult = record.floatOr("farming mult", 0.5f);
	env.farmingAltMin = record.intOr("farming altitude min", 0);
	env.farmingAltMax = record.intOr("farming altitude max", 800);
	env.farmingAltFade = record.intOr("farming altitude fade", 300);
	env.stoneMult = record.floatOr("stone mult", 1.0f);
	env.stoneNoiseZoom = record.floatOr("stone noise zoom", 1.0f);
	env.stoneNoiseCutoff = record.floatOr("stone noise cutoff", 0.5f);
	env.ironMult = record.floatOr("iron mult", 1.0f);
	env.copperMult = record.floatOr("copper mult", 1.0f);
	env.green = record.floatOr("green", 0.0f);
	env.arid = record.floatOr("arid", 0.0f);
	env.swamp = record.floatOr("swamp", 0.0f);
	return env;
}

double trap(double altitude, double min, double max, double fade) noexcept
{
	if (altitude >= min && altitude <= max)
		return 1.0;
	if (fade <= 0.0)
		return 0.0;
	if (altitude < min)
		return altitude <= min - fade ? 0.0 : (altitude - (min - fade)) / fade;
	return altitude >= max + fade ? 0.0 : ((max + fade) - altitude) / fade;
}

double waterAt(const Env& env, double altitude) noexcept
{
	return trap(altitude, env.waterAltMin, env.waterAltMax, env.waterAltFade) * env.waterMult + env.waterMin;
}

double fertilityAt(const Env& env, double altitude) noexcept
{
	return trap(altitude, env.farmingAltMin, env.farmingAltMax, env.farmingAltFade) * env.farmingMult + env.farmingMin;
}

// Hugo Elias integer noise at a lattice point: a double in (-1, 1].
double latticeHash(int x, int z) noexcept
{
	std::uint32_t n = static_cast<std::uint32_t>(x) + 57u * static_cast<std::uint32_t>(z);
	n ^= n << 13;
	std::uint32_t t = n * n * 60493u + 0x0131071Fu;
	t = t * n - 0x2DF722F3u;
	t &= 0x7FFFFFFFu;
	return 1.0 - static_cast<double>(static_cast<std::int32_t>(t)) * 9.313225746154785e-10; // 2^-30
}

namespace
{
// The stone noise below follows kinspect findings/stone26_static.md (item 47): the exe's FUN_1406cc0e0 and its callees FUN_1406cbf20 /
// FUN_1406cbba0, checked bit-exact there against the real code. The exe works in float with double intermediates; the same
// widths are kept here so the results match to the last bit.

// The decimal literal 3.1415927 as the exe stores it (_DAT_1417021b0, bits 0x400921FB5A7ED197), not pi.
constexpr double kCosPi = 3.1415927;

// The 3x3 smoothed lattice value (1/16 corners, 1/8 sides, 1/4 centre) at an integer-valued float point.
float smoothedLattice(float x, float z) noexcept
{
	const int xi = static_cast<int>(x), zi = static_cast<int>(z);
	const int xm = static_cast<int>(x - 1.0f), xp = static_cast<int>(x + 1.0f);
	const int zm = static_cast<int>(z - 1.0f), zp = static_cast<int>(z + 1.0f);
	const double corners = latticeHash(xm, zm) + latticeHash(xp, zm) + latticeHash(xm, zp) + latticeHash(xp, zp);
	const double sides = latticeHash(xp, zi) + latticeHash(xm, zi) + latticeHash(xi, zm) + latticeHash(xi, zp);
	const float side = static_cast<float>(sides * 0.125);
	const float corner = static_cast<float>(corners * 0.0625);
	const float centre = static_cast<float>(latticeHash(xi, zi) * 0.25);
	return (side + corner) + centre;
}

// One octave: the four surrounding smoothed lattice values blended with a cosine ease.
float sampleNoise(float px, float pz) noexcept
{
	const float fx = std::floor(px), fz = std::floor(pz);
	const double tx = (1.0 - std::cos(static_cast<double>(px - fx) * kCosPi)) * 0.5;
	const double tz = (1.0 - std::cos(static_cast<double>(pz - fz) * kCosPi)) * 0.5;
	const double h00 = smoothedLattice(fx, fz), h10 = smoothedLattice(fx + 1.0f, fz);
	const double h01 = smoothedLattice(fx, fz + 1.0f), h11 = smoothedLattice(fx + 1.0f, fz + 1.0f);
	const float r0 = static_cast<float>(h00 * (1.0 - tx) + h10 * tx);
	const float r1 = static_cast<float>(h01 * (1.0 - tx) + h11 * tx);
	return static_cast<float>(static_cast<double>(r0) * (1.0 - tz) + static_cast<double>(r1) * tz);
}
} // namespace

double stoneNoise(double x, double z, double zoom, double cutoff, double boost, double cap) noexcept
{
	double sum = 0.0;
	float frequency = 1.0f;
	double amplitude = 1.0;
	for (int octave = 0; octave < kStoneOctaves; ++octave)
	{
		const float px = static_cast<float>(static_cast<double>(frequency) * x / zoom);
		const float pz = static_cast<float>(static_cast<double>(frequency) * z / zoom);
		sum += static_cast<double>(sampleNoise(px, pz)) * amplitude;
		frequency *= 2.0f;
		amplitude *= 0.5;
	}
	const double v = sum * 0.5 + 0.5;
	const float floorLevel = static_cast<float>(cutoff);
	double r = v <= static_cast<double>(floorLevel) ? 0.0 : (static_cast<float>(v) - floorLevel) / (1.0f - floorLevel);
	r *= static_cast<double>(static_cast<float>(boost));
	r = std::max(r, 0.0);
	const float limit = static_cast<float>(cap);
	return r > static_cast<double>(limit) ? static_cast<double>(limit) : static_cast<double>(static_cast<float>(r));
}

double stoneAt(const Env& env, double x, double z) noexcept
{
	// the exe multiplies in float: zoom = float(field) * 1000.0f, value = float(noise) * float(stone mult)
	const double zoom = static_cast<double>(static_cast<float>(env.stoneNoiseZoom) * 1000.0f);
	const float noise = static_cast<float>(stoneNoise(x, z, zoom, env.stoneNoiseCutoff));
	return static_cast<double>(noise * static_cast<float>(env.stoneMult));
}

double depositRichness(const Env& env, std::uint8_t resource) noexcept
{
	return resource == kIron ? env.ironMult : resource == kCopper ? env.copperMult : 0.0;
}

double depositFactor(double d2, double radius) noexcept
{
	const double reach2 = kDepositReach * kDepositReach;
	const double inner2 = std::min(std::max(radius * radius, kDepositMinRadius * kDepositMinRadius), reach2);
	const double falloff = std::clamp((d2 - inner2) / (reach2 - inner2), 0.0, 1.0);
	return 1.0 - falloff;
}

namespace
{
double clampScience(double science) noexcept
{
	return std::clamp(science, 0.0, 100.0);
}
} // namespace

double areaHalfWidth(double science) noexcept
{
	return kRadiusBase + kRadiusPerScience * clampScience(science);
}

std::uint16_t mapSize(double science) noexcept
{
	return static_cast<std::uint16_t>(static_cast<int>(kMapBase + kMapPerScience * clampScience(science)));
}

std::uint8_t greyOf(double altitude) noexcept
{
	if (altitude <= kGreyLow)
		return 0;
	return static_cast<std::uint8_t>(std::lround(std::min((altitude - kGreyLow) / (kGreyHigh - kGreyLow), 1.0) * 255.0));
}

std::uint8_t planeByte(double value) noexcept
{
	return value <= kPaintFloor ? std::uint8_t{0} : static_cast<std::uint8_t>(std::lround(std::min(value, 1.0) * 255.0));
}

} // namespace prospect

// The data the maths read, derived from the game data and the data directory on first use (never saved).
struct ProspectData
{
	struct Group
	{
		std::string name;
		std::string sid;
		std::optional<prospect::Env> env;
	};
	explicit ProspectData(const std::string& dataDir) : terrain(kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif")) {}
	kworld::Heightmap terrain;
	std::unordered_map<std::uint32_t, Group> groups; // BIOME_GROUP `index` colour -> group (the first record with a colour)
};

std::shared_ptr<const ProspectData> Simulation::prospectData() const
{
	if (prospectData_ != nullptr)
		return prospectData_;
	if (gameData_ == nullptr)
		return nullptr;
	try
	{
		auto data = std::make_shared<ProspectData>(world_.dataDir());
		for (const kdata::Record* record : gameData_->byType(kworld::itemTypeByName("BIOME_GROUP")))
		{
			ProspectData::Group group{record->name, record->stringID, std::nullopt};
			if (const kdata::RefCategory* refs = record->findRefs("resources"); refs != nullptr && !refs->entries.empty())
			{
				const kdata::Record* env = refs->entries.front().opaqueEntry ? nullptr : gameData_->find(refs->entries.front().targetID);
				if (env != nullptr)
					group.env = prospect::Env::fromRecord(*env);
			}
			data->groups.emplace(static_cast<std::uint32_t>(record->intOr("index", 0)), std::move(group));
		}
		prospectData_ = std::move(data);
	} catch (const std::exception&) {
		return nullptr; // no heightmap: no prospecting (checkProspect says unsupported)
	}
	return prospectData_;
}

std::optional<double> Simulation::groundHeight(double x, double z) const
{
	const std::shared_ptr<const ProspectData> data = prospectData();
	if (data == nullptr || std::fabs(x) > kworld::kWorldExtent || std::fabs(z) > kworld::kWorldExtent)
		return std::nullopt;
	return data->terrain.terrainHeight(x, z);
}

std::optional<Simulation::ProspectZone> Simulation::prospectZone(double x, double z) const
{
	const std::shared_ptr<const ProspectData> data = prospectData();
	if (data == nullptr || weather_ == nullptr || weather_->areas() == nullptr)
		return std::nullopt;
	const auto found = data->groups.find(weather_->areas()->colourAt(x, z));
	if (found == data->groups.end())
		return ProspectZone{};
	return ProspectZone{found->second.name, found->second.sid, found->second.env};
}

std::string Simulation::checkProspect(std::uint64_t id) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr)
		return "unknown_id";
	if (!f->playerControlled)
		return "not_player_controlled";
	if (f->dead)
		return "dead";
	const std::shared_ptr<const ProspectData> data = prospectData();
	if (data == nullptr || weather_ == nullptr || weather_->areas() == nullptr)
		return "unsupported"; // no game data, heightmap or areas map
	// "I need to go outside to do this." / "I need to be stood on solid ground." / down / carried / in combat.
	if (f->building != 0 || f->carriedBy != 0 || f->down || f->unconscious || data->terrain.terrainHeight(f->x, f->z) < prospect::kWaterLevel)
		return "not_allowed";
	if (f->target != 0)
	{
		const state::Fighter* foe = combat_->find(f->target);
		if (foe != nullptr && !foe->dead && !foe->unconscious && foe->carriedBy == 0 && foe->team != f->team)
			return "not_allowed";
	}
	return "";
}

std::shared_ptr<const prospect::Result> Simulation::prospectNow(std::uint64_t id) const
{
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	const std::shared_ptr<const ProspectData> data = prospectData();
	if (f == nullptr || data == nullptr)
		return nullptr;
	return prospectAt(id, f->x, f->z, f->skills.science);
}

std::shared_ptr<const prospect::Result> Simulation::prospectAt(std::uint64_t id, double x, double z, double science) const
{
	const std::shared_ptr<const ProspectData> data = prospectData();
	if (data == nullptr)
		return nullptr;
	namespace p = prospect;
	const std::optional<ProspectZone> zone = prospectZone(x, z);
	const p::Env* env = zone && zone->env ? &*zone->env : nullptr;
	const kworld::Heightmap& terrain = data->terrain;

	auto result = std::make_shared<p::Result>();
	result->id = id;
	result->x = static_cast<float>(x);
	result->z = static_cast<float>(z);
	const double altitude = terrain.terrainHeight(x, z);
	result->altitude = static_cast<float>(altitude);
	result->science = static_cast<float>(science);
	const double r = p::areaHalfWidth(science);
	result->r = static_cast<float>(r);
	if (zone)
	{
		result->zone = zone->name;
		if (weather_ != nullptr)
			result->wind = static_cast<float>(weather_->windOf(world_.ecs(), zone->sid));
	}
	if (env != nullptr)
	{
		for (const auto& [cls, fraction] : {std::pair{p::kGreen, env->green}, {p::kArid, env->arid}, {p::kSwamp, env->swamp}})
		{
			if (fraction > 0.0)
				result->env.emplace_back(cls, static_cast<float>(fraction));
		}
	}

	// The list: point samples at the prospector. Iron and copper: the nearest deposit of the kind within 1500, richness x (1 - falloff).
	const auto deposits = [&](std::uint8_t resource)
	{
		double best = p::kDepositReach * p::kDepositReach;
		bool any = false;
		for (const DepositSite& site : world_.depositSites())
		{
			const double d2 = (site.x - x) * (site.x - x) + (site.z - z) * (site.z - z);
			if (site.resource == resource && d2 <= best && (!any || d2 < best))
			{
				best = d2;
				any = true;
			}
		}
		return any ? p::depositRichness(*env, resource) * p::depositFactor(best, 0.0) : 0.0;
	};
	for (const std::uint8_t resource : p::kListOrder)
	{
		double value = 0.0;
		if (env != nullptr)
		{
			switch (resource)
			{
			case p::kWater: value = p::waterAt(*env, altitude); break;
			case p::kFertility: value = p::fertilityAt(*env, altitude); break;
			case p::kStone: value = p::stoneAt(*env, x, z); break;
			default: value = deposits(resource); break;
			}
		}
		result->resources.emplace_back(resource, static_cast<float>(value));
	}

	// The map: a grey background from the altitude, the three sampled planes, then the deposit dots.
	const std::uint16_t n = p::mapSize(science);
	result->n = n;
	const std::size_t pixels = static_cast<std::size_t>(n) * n;
	result->grey.assign(pixels, 0);
	result->water.assign(pixels, 0);
	result->fertility.assign(pixels, 0);
	result->stone.assign(pixels, 0);
	const double step = 2.0 * r / n;
	for (std::size_t row = 0; row < n; ++row)
	{
		for (std::size_t col = 0; col < n; ++col)
		{
			const double wx = x - r + (static_cast<double>(col) + 0.5) * step;
			const double wz = z - r + (static_cast<double>(row) + 0.5) * step;
			const double alt = terrain.terrainHeight(wx, wz);
			const std::size_t at = row * n + col;
			result->grey[at] = p::greyOf(alt);
			if (env != nullptr)
			{
				result->water[at] = p::planeByte(p::waterAt(*env, alt));
				result->fertility[at] = p::planeByte(p::fertilityAt(*env, alt));
				result->stone[at] = p::planeByte(p::stoneAt(*env, wx, wz));
			}
		}
	}
	if (env != nullptr)
	{
		const double reach = p::kMapDepositReach * r;
		for (const DepositSite& site : world_.depositSites())
		{
			if ((site.x - x) * (site.x - x) + (site.z - z) * (site.z - z) <= reach * reach)
			{
				result->deposits.push_back({site.resource, static_cast<float>(site.x), static_cast<float>(site.z),
					static_cast<float>(p::depositRichness(*env, site.resource))});
			}
		}
	}
	return result;
}

void Simulation::addProspectObserver(const void* owner, std::function<void(const std::shared_ptr<const prospect::Result>&)> observer)
{
	prospectObservers_.emplace_back(owner, std::move(observer));
}

void Simulation::removeProspectObserver(const void* owner)
{
	prospectObservers_.erase(std::remove_if(prospectObservers_.begin(), prospectObservers_.end(),
		[owner](const auto& entry) { return entry.first == owner; }), prospectObservers_.end());
}

void Simulation::stepProspect()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		if (!actor.has<state::ProspectTask>())
			continue;
		const state::Fighter& f = members[i].fighter;
		if (f.dead || f.unconscious || f.down || f.carriedBy != 0)
		{
			actor.remove<state::ProspectTask>(); // a KO or death cancels it with no result
			continue;
		}
		const std::uint32_t ticks = actor.get<state::ProspectTask>()->ticks + 1;
		if (ticks < prospect::kTicks)
		{
			actor.set<state::ProspectTask>(state::ProspectTask{ticks});
			continue;
		}
		actor.remove<state::ProspectTask>();
		const std::shared_ptr<const prospect::Result> result = prospectNow(members[i].id);
		if (result != nullptr)
		{
			for (const auto& [owner, observer] : prospectObservers_)
				observer(result);
		}
	}
}

std::string Simulation::runProspect(std::uint64_t id, bool commit)
{
	std::string reason = checkProspect(id);
	if (!reason.empty() || !commit)
		return reason;
	// A replacing order: whatever ran stops, the character stands where it is.
	cancelOrders(id);
	state::Fighter* f = combat_->find(id);
	combat_->endFollow(*f);
	f->clearRoute();
	f->drive = state::DriveMode::Stop;
	f->driveStrafe = 0;
	f->orderTarget = 0;
	f->orderStop = true;
	f->hold = false;
	fighterEntity(id).set<state::ProspectTask>(state::ProspectTask{0});
	return "";
}

} // namespace ksim
