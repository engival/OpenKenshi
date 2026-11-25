#include "kworld/zones.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "io.hpp"
#include "layer_stack.hpp"
#include "le.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace kworld {

namespace {

std::string typeName(std::int32_t type)
{
	const char* name = kdata::itemTypeName(type);
	return name != nullptr ? std::string(name) : std::to_string(type);
}

void bumpTypeCount(std::vector<ZoneTypeCount>& counts, const std::string& type)
{
	for (auto& entry : counts)
		if (entry.type == type)
		{
			++entry.count;
			return;
		}
	counts.push_back({ type, 1 });
}

bool allDigits(std::string_view text)
{
	if (text.empty())
		return false;
	return std::all_of(text.begin(), text.end(),
		[](char c) { return c >= '0' && c <= '9'; });
}

ZoneCell parseZoneCellName(std::string_view filename)
{
	const std::string base = std::filesystem::path(filename).filename().string();
	constexpr std::string_view prefix = "zone.";
	constexpr std::string_view suffix = ".zone";
	if (base.size() <= prefix.size() + suffix.size()
		|| !base.starts_with(prefix) || !base.ends_with(suffix))
		throw Error("zone: unexpected file name " + base);

	const std::string middle = base.substr(prefix.size(),
		base.size() - prefix.size() - suffix.size());
	const auto dot = middle.find('.');
	if (dot == std::string::npos || middle.find('.', dot + 1) != std::string::npos)
		throw Error("zone: unexpected file name " + base);
	const std::string_view xText(middle.data(), dot);
	const std::string_view zText(middle.data() + dot + 1, middle.size() - dot - 1);
	if (!allDigits(xText) || !allDigits(zText))
		throw Error("zone: unexpected file name " + base);

	int x = 0;
	int z = 0;
	const auto xResult = std::from_chars(xText.data(), xText.data() + xText.size(), x);
	const auto zResult = std::from_chars(zText.data(), zText.data() + zText.size(), z);
	if (xResult.ec != std::errc{} || zResult.ec != std::errc{})
		throw Error("zone: cell number out of range in " + base);
	return { x, z };
}

// WORLD_LAYOUT.md "Trailing block": i32 n + n ascending unique i32.
void readTrailingBlock(ZoneFile& zone, const std::vector<std::byte>& leftover)
{
	if (leftover.empty())
		return;
	zone.trailingBytes = leftover;
	if (leftover.size() < 4)
		throw Error("zone " + zone.path + ": trailing block shorter than 4 bytes");
	const std::int32_t count = detail::readI32(leftover.data());
	if (count < 0
		|| static_cast<std::size_t>(count) > (leftover.size() - 4) / 4
		|| leftover.size() != 4 + 4 * static_cast<std::size_t>(count))
		throw Error("zone " + zone.path + ": trailing block size mismatch");
	zone.trailingValues.reserve(static_cast<std::size_t>(count));
	for (std::int32_t i = 0; i < count; ++i)
	{
		const std::int32_t value = detail::readI32(leftover.data() + 4 + 4 * i);
		if (!zone.trailingValues.empty() && value <= zone.trailingValues.back())
			throw Error("zone " + zone.path
				+ ": trailing block is not ascending and unique");
		zone.trailingValues.push_back(value);
	}
}

} // namespace

ZoneFile parseZoneFile(std::span<const std::byte> bytes,
	std::string_view filename, std::string_view layer)
{
	const ZoneCell cell = parseZoneCellName(filename);
	auto file = kdata::parse(bytes, std::filesystem::path(filename).filename().string());

	ZoneFile zone;
	zone.layer = std::string(layer);
	zone.cellX = cell.x;
	zone.cellZ = cell.z;
	zone.version = file.version;
	zone.recordCount = file.records.size();

	// The local GAMESTATE_BUILDING records: stringID -> `owner faction ID`.
	std::unordered_map<std::string, std::string> owners;
	struct BuildingState
	{
		std::string squadTemplate;
		std::string interiorLayout;
		std::string label;
		std::uint64_t townHand = 0;
		bool complete = true, destroyed = false, broken = false, paused = false, powerOn = true;
		double progress = 1.0;
	};
	std::unordered_map<std::string, BuildingState> buildingStates;
	for (const auto& record : file.records)
	{
		if (typeName(record.type) == "GAMESTATE_BUILDING")
		{
			if (const auto* owner = record.findString("owner faction ID"))
				owners.emplace(record.stringID, owner->value);
			BuildingState state;
			if (const auto* value = record.findString("squad template"))
				state.squadTemplate = value->value;
			if (const auto* value = record.findString("interior layout name"))
				state.interiorLayout = value->value;
			if (const auto* value = record.findString("name"))
				state.label = value->value;
			const auto* townType = record.findInt("townTYPE");
			const auto* townC = record.findInt("townC");
			const auto* townCS = record.findInt("townCS");
			if (townType != nullptr && townType->value == kTownHandType && townC != nullptr && townCS != nullptr)
				state.townHand = townHand(townC->value, townCS->value);
			const auto flag = [&](const char* key, bool fallback)
			{
				if (const auto* value = record.findBool(key))
					return value->value;
				if (const auto* value = record.findInt(key))
					return value->value != 0;
				return fallback;
			};
			state.complete = flag("is complete", true);
			state.destroyed = flag("destroyed", false);
			state.broken = flag("broken", false);
			state.paused = flag("pause", false);
			state.powerOn = flag("power on", true);
			if (const auto* value = record.findFloat("construction progress"))
				state.progress = kdata::decodeFloat(*value);
			buildingStates.emplace(record.stringID, std::move(state));
		}
	}

	for (const auto& record : file.records)
	{
		bumpTypeCount(zone.typeCounts, typeName(record.type));
		for (const auto& instance : record.instances)
		{
			ZoneInstance entry;
			entry.instanceID = instance.idIsString
				? instance.id : std::to_string(instance.idInt);
			entry.targetID = instance.targetStringID;
			entry.recordType = record.type;
			entry.collectionID = record.stringID;
			entry.x = detail::readF32(instance.transform.data());
			entry.y = detail::readF32(instance.transform.data() + 4);
			entry.z = detail::readF32(instance.transform.data() + 8);
			entry.qw = detail::readF32(instance.transform.data() + 12);
			entry.qx = detail::readF32(instance.transform.data() + 16);
			entry.qy = detail::readF32(instance.transform.data() + 20);
			entry.qz = detail::readF32(instance.transform.data() + 24);
			entry.states = instance.states;
			for (const std::string& state : entry.states)
			{
				const auto found = buildingStates.find(state);
				if (found != buildingStates.end() && entry.squadTemplate.empty() && entry.interiorLayout.empty() && entry.label.empty())
				{
					entry.squadTemplate = found->second.squadTemplate;
					entry.interiorLayout = found->second.interiorLayout;
					entry.label = found->second.label;
					entry.townHand = found->second.townHand;
					entry.complete = found->second.complete;
					entry.destroyed = found->second.destroyed;
					entry.broken = found->second.broken;
					entry.paused = found->second.paused;
					entry.powerOn = found->second.powerOn;
					entry.progress = found->second.progress;
				}
			}
			for (const std::string& state : entry.states)
			{
				const auto owner = owners.find(state);
				if (owner != owners.end() && !owner->second.empty())
				{
					entry.ownerFaction = owner->second;
					break;
				}
			}

			// Same order as tools/world_survey.py.
			if (entry.targetID.empty())
			{
				entry.kind = ZoneInstanceKind::Deleted;
				++zone.deletionCount;
			}
			else if (entry.x == 0.0 && entry.y == 0.0 && entry.z == 0.0)
			{
				entry.kind = ZoneInstanceKind::InventoryRow;
				++zone.inventoryRowCount;
			}
			else
			{
				entry.kind = ZoneInstanceKind::Placement;
				++zone.placementCount;
			}
			zone.instances.push_back(std::move(entry));
		}
	}

	zone.path = std::string(filename);
	readTrailingBlock(zone, file.leftover);
	return zone;
}

Zones Zones::load(std::string_view dataDir)
{
	namespace fs = std::filesystem;

	Zones zones;
	const fs::path root = fs::absolute(fs::path(dataDir)) / "newland" / "leveldata";
	std::error_code error;
	if (!fs::is_directory(root, error))
		throw Error("zones: no leveldata directory at " + root.string());

	std::vector<fs::path> paths;
	for (fs::recursive_directory_iterator it(root, error), end; it != end;
		it.increment(error))
	{
		if (error)
			throw Error("zones: cannot walk " + root.string() + ": " + error.message());
		if (!it->is_regular_file(error))
			continue;
		if (it->path().extension() == ".zone")
			paths.push_back(it->path());
	}
	if (error)
		throw Error("zones: cannot walk " + root.string() + ": " + error.message());

	std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
		return a.string() < b.string();
	});

	zones.levelDataRoot_ = root.string();
	// UNSURE: precedence between layers for the same zone cell is not
	// specified; every file is kept and placed independently.
	zones.files_.reserve(paths.size());
	for (const auto& path : paths)
	{
		std::string layer = path.parent_path().lexically_relative(root).generic_string();
		if (layer.empty())
			layer = ".";
		auto bytes = detail::readFile(path.string());
		ZoneFile zone = parseZoneFile(bytes, path.filename().string(), layer);
		zone.path = path.string();
		zones.placementCount_ += zone.placementCount;
		zones.deletionCount_ += zone.deletionCount;
		zones.inventoryRowCount_ += zone.inventoryRowCount;
		zones.files_.push_back(std::move(zone));
	}
	return zones;
}

int zoneLayerRank(const std::string& layer)
{
	if (layer == ".")
		return 0;
	if (layer == "Newwworld")
		return 1;
	if (layer == "Dialogue")
		return 2;
	if (layer == "rebirth")
		return 3;
	throw Error("zones: unknown layer \"" + layer + "\"");
}

double zoneBuildingY(const Heightmap& terrain, double x, double z, double nodeY, bool doorBuilding)
{
	// The original's Building::createPhysical (Steam 1405609e0; findings/bridge_placement.md): y = node y + max(100, ground).
	// ground is the zone cell's water-body field (129 x 129, 36 u, bilinear) when it has one, else Terrain::getHeight. UNSURE:
	// the water-body field is not loaded here, so this is always the terrain fallback (our bilinear height(): the Greenlands
	// bridge's span B reads 107.4 against the field's 107.54).
	return nodeY + (doorBuilding ? terrain.height(x, z) : std::max(kSeaPlane, terrain.height(x, z)));
}

namespace {

bool isBuildingList(const ZoneInstance& instance, std::int32_t collectionType)
{
	return instance.recordType == collectionType
		&& instance.collectionID == "0-buildinglist";
}

PlacedInstance makePlaced(const ZoneFile& file, const ZoneInstance& instance,
	const Heightmap& terrain, const kdata::GameData& gameData)
{
	PlacedInstance entry;
	entry.instanceID = instance.instanceID;
	entry.buildingID = instance.targetID;
	entry.x = instance.x;
	entry.z = instance.z;
	entry.relativeY = instance.y;
	const kdata::Record* building = gameData.find(instance.targetID);
	entry.y = zoneBuildingY(terrain, instance.x, instance.z, instance.y, building != nullptr && building->boolOr("is door", false));
	entry.qw = instance.qw;
	entry.qx = instance.qx;
	entry.qy = instance.qy;
	entry.qz = instance.qz;
	entry.scale = 1.0f;
	entry.hasScale = false;
	if (building != nullptr)
	{
		// COLLISION_FORMAT.md section 3.1: uniform scale on the
		// BUILDING record. UNSURE: a missing key defaults to 1.0;
		// all 622 shipped BUILDING records carry it.
		if (const auto* field = building->findFloat("scale"))
		{
			entry.scale = kdata::decodeFloat(*field);
			entry.hasScale = true;
		}
	}
	entry.cell = { file.cellX, file.cellZ };
	entry.positionCell = zoneCellOf(entry.x, entry.z);
	entry.layer = file.layer;
	entry.zonePath = file.path;
	entry.ownerFaction = instance.ownerFaction;
	entry.squadTemplate = instance.squadTemplate;
	entry.interiorLayout = instance.interiorLayout;
	entry.label = instance.label;
	entry.townHand = instance.townHand;
	return entry;
}

} // namespace

Zones Zones::fromFiles(std::vector<ZoneFile> files)
{
	Zones zones;
	for (const auto& file : files)
	{
		zoneLayerRank(file.layer);
		zones.placementCount_ += file.placementCount;
		zones.deletionCount_ += file.deletionCount;
		zones.inventoryRowCount_ += file.inventoryRowCount;
	}
	zones.files_ = std::move(files);
	return zones;
}

std::vector<PlacedInstance> Zones::place(const Heightmap& terrain,
	const kdata::GameData& gameData) const
{
	const std::int32_t collectionType = itemTypeByName("INSTANCE_COLLECTION");
	if (collectionType < 0)
		throw Error("zones: INSTANCE_COLLECTION missing from the item type table");

	std::vector<PlacedInstance> placed;
	placed.reserve(placementCount_);
	for (const auto& file : files_)
		for (const auto& instance : file.instances)
			if (isBuildingList(instance, collectionType)
				&& instance.kind == ZoneInstanceKind::Placement)
				placed.push_back(makePlaced(file, instance, terrain, gameData));
	return placed;
}

std::vector<ResolvedInstance> Zones::resolveLayers(LayerStats* stats) const
{
	const std::int32_t collectionType = itemTypeByName("INSTANCE_COLLECTION");
	if (collectionType < 0)
		throw Error("zones: INSTANCE_COLLECTION missing from the item type table");

	std::vector<const ZoneFile*> ordered;
	ordered.reserve(files_.size());
	for (const auto& file : files_)
	{
		zoneLayerRank(file.layer);
		ordered.push_back(&file);
	}
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const ZoneFile* a, const ZoneFile* b) {
			return zoneLayerRank(a->layer) < zoneLayerRank(b->layer);
		});

	LayerStats local;
	detail::LayerStack<ResolvedInstance> stack;
	for (const ZoneFile* file : ordered)
	{
		for (const auto& instance : file->instances)
		{
			if (!isBuildingList(instance, collectionType))
				continue;
			++local.entries;
			const std::string key = std::to_string(file->cellX) + ","
				+ std::to_string(file->cellZ) + "/" + instance.instanceID;
			if (instance.kind == ZoneInstanceKind::Deleted)
			{
				if (stack.remove(key))
					++local.deletions;
				else
					++local.orphanDeletions;
			}
			else if (stack.put(key, { file, &instance }))
			{
				++local.overrides;
			}
		}
	}

	std::vector<ResolvedInstance> resolved = std::move(stack).survivors();
	local.survivors = resolved.size();
	for (const auto& entry : resolved)
		if (entry.instance->kind == ZoneInstanceKind::Placement)
			++local.placements;
	if (stats != nullptr)
		*stats = local;
	return resolved;
}

std::vector<PlacedInstance> Zones::placeMerged(const Heightmap& terrain,
	const kdata::GameData& gameData, LayerStats* stats) const
{
	std::vector<PlacedInstance> placed;
	for (const auto& entry : resolveLayers(stats))
		if (entry.instance->kind == ZoneInstanceKind::Placement)
			placed.push_back(makePlaced(*entry.file, *entry.instance, terrain, gameData));
	return placed;
}

} // namespace kworld
