#include "ksim/nav/placement.hpp"

#include "ksim/core/rng.hpp"
#include "ksim/core/state.hpp"
#include "ksim/world/nest_sources.hpp"
#include "ksim/world/spawn.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/error.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace ksim {

namespace {

// WATER.md: one flat plane for the whole map.
constexpr double kWaterLevel = 100.0;
// UNSURE: a slope the walkable mesh can hold; the AI snap decides the rest.
constexpr double kMaxSlope = 0.6;
constexpr int kTries = 48;

// data/newland/land/areasmap.tga (TERRAIN_FORMAT.md): 256^2 uncompressed 24-bit
// TGA, pixel 0xRRGGBB == BIOME_GROUP `index`. Rows are stored from the bottom
// (descriptor bit 5 clear); the map has row 0 = z minimum like the others.
kworld::BiomeMap readAreasMap(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
	constexpr std::size_t kHeader = 18;
	if (bytes.size() < kHeader || data[2] != 2 || data[16] != 24)
		throw kworld::Error("areasmap: not an uncompressed 24-bit TGA: " + path);
	const int width = data[12] | (data[13] << 8);
	const int height = data[14] | (data[15] << 8);
	const bool topFirst = (data[17] & 0x20) != 0;
	const std::size_t first = kHeader + data[0];
	if (width <= 0 || height <= 0
		|| bytes.size() < first + static_cast<std::size_t>(width) * height * 3)
		throw kworld::Error("areasmap: truncated TGA " + path);
	std::vector<std::uint32_t> rgb(static_cast<std::size_t>(width) * height);
	for (int row = 0; row < height; ++row)
	{
		const std::uint8_t* p = data + first
			+ static_cast<std::size_t>(topFirst ? row : height - 1 - row) * width * 3;
		for (int col = 0; col < width; ++col, p += 3)
			rgb[static_cast<std::size_t>(row) * width + col] =
				(std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0];
	}
	return kworld::BiomeMap::fromPixels(width, height, std::move(rgb));
}

// One map: pixel lists by colour.
struct AreaMap
{
	kworld::BiomeMap map;
	std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> pixels; // row * width + col
	double pixelsPerZone = 1.0;

	explicit AreaMap(kworld::BiomeMap source)
		: map(std::move(source))
	{
		for (int row = 0; row < map.height(); ++row)
		{
			for (int col = 0; col < map.width(); ++col)
				pixels[map.pixel(col, row)].push_back(static_cast<std::uint32_t>(row * map.width() + col));
		}
		const double perZone = map.width() / static_cast<double>(kworld::kZoneCellsPerAxis);
		pixelsPerZone = perZone * perZone;
	}

	double zones(std::uint32_t colour) const
	{
		const auto found = pixels.find(colour);
		return found == pixels.end() ? 0.0 : static_cast<double>(found->second.size()) / pixelsPerZone;
	}
};

// Where a squad template may be, and how many instances the data expects there.
struct Candidate
{
	std::uint32_t colour = 0;
	double weight = 0.0;
};

// Dry land above the water plane and not on a cliff (heightmap only).
bool isDry(const kworld::Heightmap& terrain, double x, double z)
{
	if (terrain.height(x, z) < kWaterLevel)
		return false;
	const double dx = terrain.height(x + 18.0, z) - terrain.height(x - 18.0, z);
	const double dz = terrain.height(x, z + 18.0) - terrain.height(x, z - 18.0);
	return std::hypot(dx, dz) / 36.0 <= kMaxSlope;
}

template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (!entry.opaqueEntry)
			func(entry);
	}
}

std::int64_t totalChance(const kdata::Record& record, std::string_view category)
{
	std::int64_t total = 0;
	forEachRef(record, category, [&](const kdata::RefEntry& entry)
	{
		total += std::max(entry.v0, 0);
	});
	return total;
}

class Placer
{
public:
	Placer(const kdata::GameData& gameData, const worldstate::Facts& facts, std::string_view dataDir, std::uint64_t seed)
		: data_(gameData)
		, facts_(facts)
		, areas_(readAreasMap(std::string(dataDir) + "/newland/land/areasmap.tga"))
		, terrain_(kworld::Heightmap::open(std::string(dataDir) + "/newland/land/fullmap.tif"))
		, rng_(streamSeed(seed, kPlacementStream))
		, groupType_(kworld::itemTypeByName("BIOME_GROUP"))
		, townType_(kworld::itemTypeByName("TOWN"))
		, factionType_(kworld::itemTypeByName("FACTION"))
	{
		// BIOME_GROUP only: the exe never reads a BIOMES record's lists
		// (retired/placement_biomes_homeless_lists.txt).
		for (const kdata::Record* record : data_.byType(groupType_))
			collect(*record);
	}

	PlacementReport run(flecs::world& ecs)
	{
		std::vector<std::pair<std::uint64_t, flecs::entity_t>> squads;
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::SquadInstance& squad)
		{
			if (squad.homeTown.id == 0 && entity.get<state::Position>() == nullptr)
				squads.emplace_back(id.value, entity.id());
		});
		std::sort(squads.begin(), squads.end());
		for (const auto& [id, raw] : squads)
		{
			const flecs::entity entity(ecs.c_ptr(), raw);
			state::SquadInstance squad = *entity.get<state::SquadInstance>();
			const kdata::Record* record = data_.find(squad.templateSid);
			const state::GroundPoint den = pick(record);
			squad.hasDen = true;
			squad.denX = den.x;
			squad.denZ = den.z;
			entity.set<state::SquadInstance>(squad);
			entity.set<state::Position>(state::Position{den.x, den.z});
			++report_.squads;
		}
		return report_;
	}

private:
	void add(const std::string& squad, std::uint32_t colour, double weight)
	{
		if (weight > 0.0)
			candidates_[squad].push_back(Candidate{colour, weight});
	}

	// The expected-instance weights of one BIOME_GROUP record.
	void collect(const kdata::Record& record)
	{
		const std::uint32_t colour = static_cast<std::uint32_t>(record.intOr("index", 0));
		const double zones = areas_.zones(colour);
		if (zones <= 0.0)
			return;
		const double amount = record.floatOr("homeless spawn amount", 1.0f);
		const double homeless = static_cast<double>(totalChance(record, "homeless spawns"));
		forEachRef(record, "homeless spawns", [&](const kdata::RefEntry& entry)
		{
			const double expected = zones * amount * std::max(entry.v0, 0) / homeless;
			add(entry.targetID, colour, expected);
			report_.densitySquads += expected;
		});
		const double nests = static_cast<double>(totalChance(record, "nests"));
		const double count = record.intOr("num nests", 6);
		forEachLiveNest(data_, facts_, townType_, record, [&](const kdata::Record& nest, const kdata::RefEntry& nestEntry)
		{
			const double expected = count * nestEntry.v0 / nests;
			for (const std::string_view list : kNestSquadLists)
			{
				forEachRef(nest, list, [&](const kdata::RefEntry& entry)
				{
					add(entry.targetID, colour, expected);
				});
			}
		});
	}

	// The area to place `record` in, as (map, colour); false when there is none.
	bool chooseArea(const kdata::Record* record, const AreaMap*& map, std::uint32_t& colour)
	{
		const auto found = record != nullptr ? candidates_.find(record->stringID) : candidates_.end();
		if (found != candidates_.end())
		{
			double total = 0.0;
			for (const Candidate& candidate : found->second)
				total += candidate.weight;
			double pick = rng_.uniform01() * total;
			for (const Candidate& candidate : found->second)
			{
				pick -= candidate.weight;
				if (pick < 0.0 || &candidate == &found->second.back())
				{
					map = &areas_;
					colour = candidate.colour;
					++report_.byBiomeGroup;
					return true;
				}
			}
		}
		// Faction roaming squads with no biome row: the faction's territory,
		// then the areas where its other squads spawn.
		std::string faction;
		forEachRef(*record, "faction", [&](const kdata::RefEntry& entry)
		{
			if (faction.empty())
				faction = entry.targetID;
		});
		std::set<std::uint32_t> colours;
		const kdata::Record* factionRecord = data_.find(faction);
		if (factionRecord != nullptr && factionRecord->type == factionType_)
		{
			forEachRef(*factionRecord, "biomes", [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* group = data_.find(entry.targetID);
				if (group != nullptr && group->type == groupType_)
					colours.insert(static_cast<std::uint32_t>(group->intOr("index", 0)));
			});
			if (chooseColour(colours, map, colour))
			{
				++report_.byFactionTerritory;
				return true;
			}
			colours.clear();
			for (const kdata::Record* other : data_.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
			{
				const auto own = candidates_.find(other->stringID);
				bool sameFaction = false;
				forEachRef(*other, "faction", [&](const kdata::RefEntry& entry)
				{
					sameFaction = sameFaction || entry.targetID == faction;
				});
				if (!sameFaction || own == candidates_.end())
					continue;
				for (const Candidate& candidate : own->second)
					colours.insert(candidate.colour);
			}
			if (chooseColour(colours, map, colour))
			{
				++report_.byFactionSpawns;
				return true;
			}
		}
		++report_.anywhere;
		return false;
	}

	// One of `colours` of the areas map, by the zones it covers.
	bool chooseColour(const std::set<std::uint32_t>& colours, const AreaMap*& map,
		std::uint32_t& colour)
	{
		double total = 0.0;
		for (const std::uint32_t candidate : colours)
			total += areas_.zones(candidate);
		if (total <= 0.0)
			return false;
		double pick = rng_.uniform01() * total;
		for (const std::uint32_t candidate : colours)
		{
			pick -= areas_.zones(candidate);
			if (pick < 0.0 && areas_.zones(candidate) > 0.0)
			{
				map = &areas_;
				colour = candidate;
				return true;
			}
		}
		return false;
	}

	bool dry(double x, double z) const { return isDry(terrain_, x, z); }

	state::GroundPoint pick(const kdata::Record* record)
	{
		const AreaMap* map = nullptr;
		std::uint32_t colour = 0;
		const bool chosen = record != nullptr && chooseArea(record, map, colour);
		if (!chosen)
		{
			map = &areas_;
			colour = 0;
		}
		const double unit = map->map.unitsPerPixel();
		state::GroundPoint point;
		for (int attempt = 0; attempt < kTries; ++attempt)
		{
			std::int64_t pixel = 0;
			if (chosen)
			{
				const std::vector<std::uint32_t>& list = map->pixels.at(colour);
				pixel = list[static_cast<std::size_t>(
					rng_.between(0, static_cast<std::int64_t>(list.size()) - 1))];
			} else {
				pixel = rng_.between(0, static_cast<std::int64_t>(map->map.width())
					* map->map.height() - 1);
			}
			point.x = kworld::kWorldMin + (static_cast<double>(pixel % map->map.width())
				+ rng_.uniform01()) * unit;
			point.z = kworld::kWorldMin + (static_cast<double>(pixel / map->map.width())
				+ rng_.uniform01()) * unit;
			if (dry(point.x, point.z))
				return point;
		}
		++report_.onWater;
		return point;
	}

	const kdata::GameData& data_;
	const worldstate::Facts& facts_; // the game's: placement judges world states as the spawners do
	AreaMap areas_;
	kworld::Heightmap terrain_;
	RngStream rng_;
	std::int32_t groupType_;
	std::int32_t townType_;
	std::int32_t factionType_;
	std::map<std::string, std::vector<Candidate>> candidates_;
	PlacementReport report_;
};

} // namespace

struct WildGround::Impl
{
	explicit Impl(std::string_view dataDir)
		: areas(readAreasMap(std::string(dataDir) + "/newland/land/areasmap.tga"))
		, terrain(kworld::Heightmap::open(std::string(dataDir) + "/newland/land/fullmap.tif"))
	{
	}

	AreaMap areas;
	kworld::Heightmap terrain;
};

WildGround::WildGround(std::string_view dataDir)
	: impl_(std::make_unique<Impl>(dataDir))
{
}

WildGround::~WildGround() = default;

double WildGround::zones(std::uint32_t colour) const
{
	return impl_->areas.zones(colour);
}

std::uint32_t WildGround::colourAt(double x, double z) const
{
	return impl_->areas.map.biomeAt(x, z);
}

state::GroundPoint WildGround::pick(std::uint32_t colour, RngStream& rng) const
{
	const std::vector<std::uint32_t>& list = impl_->areas.pixels.at(colour);
	const double unit = impl_->areas.map.unitsPerPixel();
	state::GroundPoint point;
	for (int attempt = 0; attempt < kTries; ++attempt)
	{
		const std::uint32_t pixel = list[static_cast<std::size_t>(
			rng.between(0, static_cast<std::int64_t>(list.size()) - 1))];
		point.x = kworld::kWorldMin + (static_cast<double>(pixel % impl_->areas.map.width())
			+ rng.uniform01()) * unit;
		point.z = kworld::kWorldMin + (static_cast<double>(pixel / impl_->areas.map.width())
			+ rng.uniform01()) * unit;
		if (isDry(impl_->terrain, point.x, point.z))
			return point;
	}
	return point;
}

struct AreaLookup::Impl
{
	explicit Impl(std::string_view dataDir)
		: map(readAreasMap(std::string(dataDir) + "/newland/land/areasmap.tga"))
	{
	}

	kworld::BiomeMap map;
};

AreaLookup::AreaLookup(std::string_view dataDir)
	: impl_(std::make_unique<Impl>(dataDir))
{
}

AreaLookup::~AreaLookup() = default;

std::uint32_t AreaLookup::colourAt(double x, double z) const
{
	return impl_->map.biomeAt(x, z);
}

PlacementReport placeWilderness(flecs::world& ecs, const kdata::GameData& gameData,
	std::string_view dataDir, std::uint64_t seed)
{
	worldstate::Facts facts;
	worldstate::collectStatuses(ecs, facts);
	return Placer(gameData, facts, dataDir, seed).run(ecs);
}

} // namespace ksim
