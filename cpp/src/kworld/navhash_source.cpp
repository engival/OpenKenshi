#include "kworld/navhash_source.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/layouts.hpp"
#include "kworld/navhash.hpp"
#include "kworld/towns.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>

namespace kworld {

namespace {

namespace fs = std::filesystem;

constexpr const char* kLayers[] = { "", "Newwworld", "Dialogue", "rebirth" };

std::vector<std::byte> readFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	std::vector<std::byte> bytes;
	char buffer[8192];
	while (in.read(buffer, sizeof buffer) || in.gcount() > 0)
		for (std::streamsize i = 0; i < in.gcount(); ++i)
			bytes.push_back(static_cast<std::byte>(buffer[i]));
	return bytes;
}

std::string zonePath(const std::string& dataDir, const char* layer, ZoneCell cell)
{
	std::string dir = dataDir + "/newland/leveldata";
	if (*layer != 0)
		dir += std::string("/") + layer;
	return dir + "/zone." + std::to_string(cell.x) + "." + std::to_string(cell.z) + ".zone";
}

} // namespace

bool navZoneFileExists(const std::string& dataDir, ZoneCell cell)
{
	for (const char* layer : kLayers)
		if (fs::exists(zonePath(dataDir, layer, cell)))
			return true;
	return false;
}

std::vector<NavPlaced> loadNavZone(const std::string& dataDir, ZoneCell cell, const kdata::GameData& gameData)
{
	std::vector<NavPlaced> placed;
	std::map<std::string, std::size_t> index;
	for (const char* layer : kLayers)
	{
		const std::string path = zonePath(dataDir, layer, cell);
		if (!fs::exists(path))
			continue;
		const auto file = kdata::parse(readFile(path), fs::path(path).filename().string());
		std::map<std::string, const kdata::Record*> bySid;
		for (const auto& record : file.records)
			bySid[record.stringID] = &record;
		for (const auto& collection : file.records)
		{
			const char* type = kdata::itemTypeName(collection.type);
			if (type == nullptr || std::strcmp(type, "INSTANCE_COLLECTION") != 0 || collection.stringID != "0-buildinglist")
				continue;
			for (const auto& instance : collection.instances)
			{
				NavPlaced entry;
				entry.instanceID = instance.idIsString ? instance.id : std::to_string(instance.idInt);
				entry.buildingID = instance.targetStringID;
				if (entry.buildingID.empty())
				{
					index.erase(entry.instanceID); // a tombstone
					continue;
				}
				float transform[7];
				std::memcpy(transform, instance.transform.data(), sizeof transform);
				entry.placed.x = transform[0];
				entry.placed.y = transform[1];
				entry.placed.z = transform[2];
				entry.placed.qw = transform[3];
				entry.placed.qx = transform[4];
				entry.placed.qy = transform[5];
				entry.placed.qz = transform[6];
				entry.placed.scale = 1.0f;
				if (const kdata::Record* building = gameData.find(entry.buildingID))
					if (const auto* scale = building->findFloat("scale"))
						entry.placed.scale = kdata::decodeFloat(*scale);
				const kdata::Record* state = nullptr;
				for (const std::string& sid : instance.states)
				{
					const auto found = bySid.find(sid);
					if (found != bySid.end())
						state = found->second;
				}
				if (state != nullptr)
				{
					entry.destroyed = state->findBool("destroyed") != nullptr && state->findBool("destroyed")->value;
					if (const auto* name = state->findString("interior layout name"))
						entry.interiorLayout = name->value;
					if (const auto* name = state->findString("exterior layout name"))
						entry.exteriorLayout = name->value;
					const auto* handleI = state->findInt("handleI");
					const auto* handleC = state->findInt("handleC");
					if (handleI != nullptr && handleC != nullptr)
						entry.uid = (static_cast<std::uint64_t>(handleI->value + 1) << 16) | static_cast<std::uint64_t>(handleC->value & 0xffff);
				}
				if (const kdata::Record* building = gameData.find(entry.buildingID))
				{
					entry.isGate = building->findBool("is wall gate") != nullptr && building->findBool("is wall gate")->value;
					const auto* pathMode = building->findInt("path mode");
					entry.hasPath = pathMode != nullptr && pathMode->value != 0;
				}
				entry.placed.interiorLayout = entry.interiorLayout;
				entry.placed.buildingID = entry.buildingID;
				const auto found = index.find(entry.instanceID);
				if (found != index.end())
					placed[found->second] = std::move(entry);
				else
				{
					index.emplace(entry.instanceID, placed.size());
					placed.push_back(std::move(entry));
				}
			}
		}
	}
	return placed;
}

std::vector<const NavPlaced*> navZoneObjects(const std::vector<NavPlaced>& placed)
{
	std::vector<const NavPlaced*> objects;
	for (const NavPlaced& entry : placed)
		if (entry.hasPath)
			objects.push_back(&entry);
	return objects;
}

std::uint32_t navZoneBuildingHash(const NavPlaced& entry)
{
	return buildingHash(entry.placed.x, entry.placed.z, entry.destroyed && entry.isGate);
}

std::uint32_t navInteriorBuildingHash(const NavPlaced& entry)
{
	return buildingHash(entry.placed.x, entry.placed.z, false);
}

std::vector<PlacedInstance> navInteriorChildren(const NavPlaced& entry, const kdata::GameData& gameData,
	const InteriorLayouts& layouts)
{
	std::vector<PlacedInstance> children;
	const auto add = [&](const std::string& sid)
	{
		const InteriorLayout* layout = layouts.find(sid);
		if (layout == nullptr)
			return;
		for (const LayoutItem& item : layout->items)
		{
			const kdata::Record* record = gameData.find(item.target);
			if (record == nullptr || std::strcmp(kdata::itemTypeName(record->type), "BUILDING") != 0)
				continue;
			if (record->findBool("is node") != nullptr && record->findBool("is node")->value)
				continue;
			children.push_back(placeFurniture(entry.placed, item, gameData));
		}
	};
	add(entry.buildingID + "-" + entry.interiorLayout);
	add("EXT-" + entry.buildingID + "-" + entry.exteriorLayout);
	return children;
}

NavHashSource::NavHashSource(std::string dataDir, const AreaSources& sources)
	: dataDir_(std::move(dataDir))
	, gameData_(sources.gameData)
	, layouts_(sources.layouts)
	, terrain_(sources.terrain)
	, biomes_(sources.biomes)
	, features_(sources.features)
	, towns_(sources.towns)
{
}

NavHashSource::ZoneData& NavHashSource::data(ZoneCell cell)
{
	ZoneData& zone = zones_[zoneIndexOf(cell)];
	if (!zone.loaded)
	{
		zone.loaded = true;
		try
		{
			zone.placed = loadNavZone(dataDir_, cell, gameData_);
		}
		catch (const std::exception&)
		{
			zone.failure = "zone data unreadable";
			return zone;
		}
		for (std::size_t i = 0; i < zone.placed.size(); ++i)
			if (zone.placed[i].uid != 0)
				zone.byUid.emplace(zone.placed[i].uid, i);
	}
	return zone;
}

NavHashSource::ZoneHashes NavHashSource::zone(ZoneCell cell)
{
	ZoneData& zone = data(cell);
	if (zone.hashes)
		return *zone.hashes;
	ZoneHashes hashes;
	if (zone.failure != nullptr)
	{
		hashes.reason = zone.failure;
		zone.hashes = hashes;
		return hashes;
	}
	std::vector<std::uint32_t> buildingHashes;
	for (const NavPlaced* entry : navZoneObjects(zone.placed))
	{
		buildingHashes.push_back(navZoneBuildingHash(*entry));
		hashes.fold = comb(hashes.fold, buildingHashes.back());
	}
	hashes.buildings = buildingHashes.size();
	hashes.forest = zoneHasTreeLayer(cell, biomes_, gameData_);
	try
	{
		FoliageParams params;
		params.unquantisedPositions = true; // the fold runs on the spawner's position, before the u16 page encoding
		const std::vector<FoliageInstance> instances = generateFoliage(cell, terrain_, biomes_, gameData_, features_, towns_, params);
		hashes.trees = instances.size();
		for (const FoliageInstance& instance : instances)
		{
			hashes.treesFolded += instance.navFold ? 1 : 0;
			hashes.treesGrass += instance.grassLimited ? 1 : 0;
		}
		hashes.vegetation = vegetationWord(instances);
		hashes.hash = zoneHash(buildingHashes, hashes.forest, hashes.vegetation);
	}
	catch (const std::exception&)
	{
		hashes.reason = "foliage scatter failed";
	}
	zone.hashes = hashes;
	return hashes;
}

const NavPlaced* NavHashSource::placement(ZoneCell cell, std::uint64_t uid)
{
	ZoneData& zone = data(cell);
	if (zone.failure != nullptr)
		return nullptr;
	auto found = zone.byUid.find(uid);
	if (found == zone.byUid.end())
		found = zone.byUid.find(uid & 0xffffff);
	return found == zone.byUid.end() ? nullptr : &zone.placed[found->second];
}

NavHashSource::InteriorHashes NavHashSource::interior(ZoneCell cell, std::uint64_t uid)
{
	InteriorHashes hashes;
	ZoneData& zone = data(cell);
	if (zone.failure != nullptr)
	{
		hashes.basis = Basis::Unpaired; // no placements to pair with
		return hashes;
	}
	auto found = zone.byUid.find(uid);
	if (found == zone.byUid.end())
		found = zone.byUid.find(uid & 0xffffff); // a few shipped uids carry extra high bits
	if (found == zone.byUid.end())
	{
		hashes.basis = Basis::Unpaired;
		return hashes;
	}
	const NavPlaced& entry = zone.placed[found->second];
	const bool named = !entry.interiorLayout.empty() || !entry.exteriorLayout.empty();
	if (named)
	{
		bool joined = false;
		if (!entry.interiorLayout.empty())
			joined = joined || layouts_.find(entry.buildingID + "-" + entry.interiorLayout) != nullptr;
		if (!entry.exteriorLayout.empty())
			joined = joined || layouts_.find("EXT-" + entry.buildingID + "-" + entry.exteriorLayout) != nullptr;
		if (!joined)
		{
			hashes.basis = Basis::Unjoined;
			return hashes;
		}
	}
	const std::vector<PlacedInstance> children = navInteriorChildren(entry, gameData_, layouts_);
	std::vector<std::uint32_t> childHashes;
	for (const PlacedInstance& child : children)
		childHashes.push_back(buildingHash(child.x, child.z, false));
	hashes.hash = interiorHash(entry.placed.x, entry.placed.z, entry.destroyed, entry.destroyed && entry.isGate, childHashes);
	hashes.basis = children.empty() ? Basis::Exact : Basis::ChildOrder;
	return hashes;
}

StaleVerdict staleVerdict(std::optional<std::uint32_t> stored, std::optional<std::uint32_t> ours,
	NavHashSource::Basis basis)
{
	if (!stored || !ours)
		return StaleVerdict::Unverifiable;
	if (*stored == *ours)
		return StaleVerdict::Fresh;
	return basis == NavHashSource::Basis::Exact ? StaleVerdict::Stale : StaleVerdict::Unverifiable;
}

bool forestOnlyStale(std::optional<std::uint32_t> stored, const NavHashSource::ZoneHashes& ours)
{
	return stored && ours.hash && *stored != *ours.hash && ours.buildings == 0;
}

const char* staleReason(NavHashSource::Basis basis)
{
	switch (basis)
	{
	case NavHashSource::Basis::Exact: return "hash differs";
	case NavHashSource::Basis::ChildOrder: return "child gather order unknown";
	case NavHashSource::Basis::Unpaired: return "no placement for the uid";
	case NavHashSource::Basis::Unjoined: return "layout not in interiors.level";
	}
	return "unknown";
}

} // namespace kworld
