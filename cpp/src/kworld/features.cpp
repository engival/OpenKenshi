#include "kworld/features.hpp"

#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/zones.hpp"
#include "io.hpp"
#include "le.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace kworld {

namespace {

constexpr std::size_t kMagicBytes = 4;
constexpr std::size_t kTableOffset = 8;
constexpr std::size_t kTableEntryBytes = 8;
// name length byte + 3 pos + 3 scale + 4 rotation floats
constexpr std::size_t kMinRecordBytes = 1 + 40;
constexpr double kMaxCoordinate = 1.0e9;

FeatureMode modeOf(int value)
{
	return value >= 0 && value <= static_cast<int>(FeatureMode::Emissive)
		? static_cast<FeatureMode>(value) : FeatureMode::UvMapped;
}

} // namespace

bool FeaturePlacement::rotationFinite() const noexcept
{
	return std::isfinite(qw) && std::isfinite(qx) && std::isfinite(qy)
		&& std::isfinite(qz);
}

FeatureFile parseFeatureFile(std::span<const std::byte> bytes,
	std::string_view layer)
{
	constexpr std::size_t headerBytes = kTableOffset
		+ kTableEntryBytes * static_cast<std::size_t>(kZoneCount);
	if (bytes.size() < headerBytes)
		throw Error("features: file shorter than the MF01 header");
	if (std::string_view(reinterpret_cast<const char*>(bytes.data()), kMagicBytes) != "MF01")
		throw Error("features: bad magic");
	if (detail::readI32(bytes.data() + 4) != kZoneCount)
		throw Error("features: zone count is not " + std::to_string(kZoneCount));

	FeatureFile file;
	std::size_t expected = headerBytes; // blobs are contiguous in zone order
	for (int zone = 0; zone < kZoneCount; ++zone)
	{
		const std::byte* entry = bytes.data() + kTableOffset
			+ kTableEntryBytes * static_cast<std::size_t>(zone);
		const std::int32_t offset = detail::readI32(entry);
		const std::int32_t size = detail::readI32(entry + 4);
		if (offset == 0 && size == 0)
			continue;
		const std::string where = "features: zone " + std::to_string(zone);
		if (offset < 0 || size < 4 || static_cast<std::size_t>(offset) > bytes.size()
			|| static_cast<std::size_t>(size) > bytes.size() - static_cast<std::size_t>(offset))
			throw Error(where + ": blob outside the file");
		if (static_cast<std::size_t>(offset) != expected)
			throw Error(where + ": blob not contiguous with the previous one");
		expected += static_cast<std::size_t>(size);

		const std::byte* cursor = bytes.data() + offset;
		const std::byte* end = cursor + size;
		const std::int32_t count = detail::readI32(cursor);
		cursor += 4;
		if (count < 0 || static_cast<std::size_t>(count) > static_cast<std::size_t>(end - cursor) / kMinRecordBytes)
			throw Error(where + ": record count does not fit the blob");

		++file.zoneCount;
		const ZoneCell cell = zoneCellOfIndex(zone);
		for (std::int32_t i = 0; i < count; ++i)
		{
			const std::size_t nameLength = std::to_integer<std::size_t>(*cursor);
			++cursor;
			if (nameLength == 0)
				throw Error(where + ": empty record name");
			if (static_cast<std::size_t>(end - cursor) < nameLength + 40)
				throw Error(where + ": truncated record");

			FeaturePlacement placement;
			placement.recordID.assign(reinterpret_cast<const char*>(cursor), nameLength);
			cursor += nameLength;
			placement.x = detail::readF32(cursor);
			placement.y = detail::readF32(cursor + 4);
			placement.z = detail::readF32(cursor + 8);
			// Bounds keep zoneCellOf's int cast defined (NaN fails the comparison).
			if (!(std::fabs(placement.x) < kMaxCoordinate && std::fabs(placement.y) < kMaxCoordinate
				&& std::fabs(placement.z) < kMaxCoordinate))
				throw Error(where + ": non-finite or huge position");
			placement.scaleX = detail::readF32(cursor + 12);
			placement.scaleY = detail::readF32(cursor + 16);
			placement.scaleZ = detail::readF32(cursor + 20);
			placement.qw = detail::readF32(cursor + 24);
			placement.qx = detail::readF32(cursor + 28);
			placement.qy = detail::readF32(cursor + 32);
			placement.qz = detail::readF32(cursor + 36);
			cursor += 40;
			placement.cell = cell;
			placement.positionCell = zoneCellOf(placement.x, placement.z);
			placement.layer = std::string(layer);
			file.placements.push_back(std::move(placement));
		}
		if (cursor != end)
			throw Error(where + ": blob not consumed exactly");
	}
	if (expected != bytes.size())
		throw Error("features: trailing bytes after the last blob");
	return file;
}

std::vector<RoadFoliagePoint> collectRoadFoliage(const kdata::File& file,
	std::string_view layer)
{
	const std::int32_t collectionType = itemTypeByName("INSTANCE_COLLECTION");
	if (collectionType < 0)
		throw Error("features: INSTANCE_COLLECTION missing from the item type table");

	std::vector<RoadFoliagePoint> points;
	for (const auto& record : file.records)
	{
		if (record.type != collectionType || record.stringID != "5-roadfoliage")
			continue;
		for (const auto& instance : record.instances)
		{
			RoadFoliagePoint point;
			point.instanceID = instance.idIsString
				? instance.id : std::to_string(instance.idInt);
			point.targetID = instance.targetStringID;
			point.x = detail::readF32(instance.transform.data());
			point.y = detail::readF32(instance.transform.data() + 4);
			point.z = detail::readF32(instance.transform.data() + 8);
			point.u = detail::readF32(instance.transform.data() + 12);
			point.layer = std::string(layer);
			point.deleted = instance.targetStringID.empty();
			points.push_back(std::move(point));
		}
	}
	return points;
}

std::vector<RoadFoliagePoint> overlayRoadFoliage(
	std::span<const std::vector<RoadFoliagePoint>> layers)
{
	std::vector<RoadFoliagePoint> merged;
	std::unordered_map<std::string, std::size_t> index;
	std::vector<bool> removed;
	for (const auto& layer : layers)
		for (const auto& point : layer)
		{
			const auto [slot, fresh] = index.emplace(point.instanceID, merged.size());
			if (fresh)
			{
				merged.push_back(point);
				removed.push_back(point.deleted);
				continue;
			}
			merged[slot->second] = point;
			removed[slot->second] = point.deleted;
		}

	std::vector<RoadFoliagePoint> result;
	for (std::size_t i = 0; i < merged.size(); ++i)
		if (!removed[i])
			result.push_back(std::move(merged[i]));
	return result;
}

std::vector<RoadFoliagePoint> loadRoadFoliage(std::string_view dataDir)
{
	namespace fs = std::filesystem;

	const fs::path root = fs::absolute(fs::path(dataDir)) / "newland";
	std::error_code error;
	if (!fs::is_directory(root / "leveldata", error))
		throw Error("features: no leveldata directory at " + (root / "leveldata").string());

	struct Source
	{
		int rank;
		std::string layer;
		fs::path path;
	};
	std::vector<Source> sources;
	if (fs::is_regular_file(root / "leveldata.level", error))
		sources.push_back({ zoneLayerRank("."), ".", root / "leveldata.level" });
	for (fs::directory_iterator it(root / "leveldata", error), end; it != end;
		it.increment(error))
	{
		if (error)
			throw Error("features: cannot walk leveldata: " + error.message());
		const fs::path candidate = it->path() / "leveldata.level";
		if (!it->is_directory(error) || !fs::is_regular_file(candidate, error))
			continue;
		const std::string layer = it->path().filename().string();
		sources.push_back({ zoneLayerRank(layer), layer, candidate });
	}
	if (error)
		throw Error("features: cannot walk leveldata: " + error.message());
	std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) {
		return a.rank != b.rank ? a.rank < b.rank : a.layer < b.layer;
	});

	std::vector<std::vector<RoadFoliagePoint>> layers;
	for (const auto& source : sources)
	{
		auto bytes = detail::readFile(source.path.string());
		const auto file = kdata::parse(bytes, source.path.filename().string());
		layers.push_back(collectRoadFoliage(file, source.layer));
	}
	return overlayRoadFoliage(layers);
}

FeatureRecord resolveFeatureRecord(const kdata::GameData& gameData,
	std::string_view recordID)
{
	FeatureRecord result;
	result.recordID = std::string(recordID);
	const kdata::Record* record = gameData.find(recordID);
	if (record == nullptr)
		return result;
	result.found = true;
	const char* typeName = kdata::itemTypeName(record->type);
	result.recordType = typeName != nullptr ? typeName : std::to_string(record->type);

	if (record->type == itemTypeByName("MAP_FEATURES"))
	{
		result.hidden = record->boolOr("hidden", false);
		result.localCoordinates = record->boolOr("local coordinates", false);
		result.distance = record->floatOr("distance", 2000.0f);
		if (result.hidden || record->fileOr("mesh").empty())
			return result;
		FeatureMesh mesh;
		mesh.mesh = record->fileOr("mesh");
		mesh.texture = record->fileOr("texture map");
		mesh.normalMap = record->fileOr("normal map");
		mesh.texture2 = record->fileOr("texture map 2");
		mesh.normalMap2 = record->fileOr("normal map 2");
		mesh.tileX = record->floatOr("tile X", 1.0f);
		mesh.tileY = record->floatOr("tile Y", 1.0f);
		mesh.specularMult = record->floatOr("specular mult", 1.0f);
		mesh.mode = modeOf(record->intOr("texture mode", 0));
		mesh.alphaThreshold = record->intOr("alpha threshold", 0);
		result.meshes.push_back(std::move(mesh));
	}
	else if (record->type == itemTypeByName("FOLIAGE_MESH"))
	{
		if (record->fileOr("mesh").empty())
			return result;
		FeatureMesh mesh;
		mesh.mesh = record->fileOr("mesh");
		mesh.texture = record->fileOr("texture map");
		mesh.normalMap = record->fileOr("normal map");
		mesh.texture2 = record->fileOr("texture map 2");
		mesh.normalMap2 = record->fileOr("normal map 2");
		mesh.tileX = record->floatOr("tile X", 1.0f);
		mesh.tileY = record->floatOr("tile Y", 1.0f);
		mesh.specularMult = record->floatOr("specular mult", 0.0f);
		mesh.mode = modeOf(record->intOr("material type", 0));
		// BUILDING_MATERIALS.md: `alpha threshold` (default 128) applies to
		// the FOLIAGE mode only.
		mesh.alphaThreshold = mesh.mode == FeatureMode::Foliage
			? record->intOr("alpha threshold", 128) : 0;
		result.meshes.push_back(mesh);

		const std::string& leaves = record->fileOr("leaves mesh");
		if (!leaves.empty())
		{
			FeatureMesh second;
			second.mesh = leaves;
			second.texture = record->fileOr("leaves texture");
			second.normalMap = record->fileOr("leaves normal");
			second.tileX = mesh.tileX;
			second.tileY = mesh.tileY;
			second.mode = FeatureMode::Foliage;
			second.alphaThreshold = record->intOr("leaves alpha threshold", 80);
			second.leaves = true;
			result.meshes.push_back(std::move(second));
		}
	}
	return result;
}

Features Features::join(const kdata::GameData& gameData, std::string_view dataDir)
{
	namespace fs = std::filesystem;

	Features features;
	const fs::path path = fs::absolute(fs::path(dataDir)) / "newland" / "land" / "features.dat";
	auto bytes = detail::readFile(path.string());
	FeatureFile file = parseFeatureFile(bytes, ".");
	features.zoneCount_ = file.zoneCount;
	features.placements_ = std::move(file.placements);

	std::unordered_map<std::string, int> index;
	for (auto& placement : features.placements_)
	{
		const auto [slot, fresh] = index.emplace(placement.recordID,
			static_cast<int>(features.records_.size()));
		if (fresh)
			features.records_.push_back(resolveFeatureRecord(gameData, placement.recordID));
		placement.record = slot->second;
	}
	features.roadFoliage_ = loadRoadFoliage(dataDir);
	features.roadOverlay_ = std::make_shared<const RoadOverlay>(std::string(dataDir));
	return features;
}

Features Features::load(std::string_view dataDir)
{
	return join(loadMergedGameData(dataDir), dataDir);
}

std::size_t Features::unresolvedPlacements() const noexcept
{
	std::size_t count = 0;
	for (const auto& placement : placements_)
		if (placement.record < 0 || records_[static_cast<std::size_t>(placement.record)].meshes.empty())
			++count;
	return count;
}

} // namespace kworld
