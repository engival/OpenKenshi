#include "kdata/format.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <utility>

namespace kdata {

namespace {

constexpr const char* kItemTypes[] = {
	"BUILDING", "CHARACTER", "WEAPON", "ARMOUR", "ITEM",
	"ANIMAL_ANIMATION", "ATTACHMENT", "RACE", "LOCATION", "WAR_SAVESTATE",
	"FACTION", "NULL_ITEM", "ZONE_MAP", "TOWN", "WORLDMAP_CHARACTER",
	"CHARACTER_APPEARANCE_OLD", "LOCATIONAL_DAMAGE", "COMBAT_TECHNIQUE",
	"DIALOGUE", "DIALOGUE_LINE", "TECHTREE", "RESEARCH", "AI_TASK", "AI_STATE",
	"ANIMATION", "STATS", "PERSONALITY", "CONSTANTS", "BIOMES",
	"BUILDING_PART", "INSTANCE_COLLECTION", "DIALOG_ACTION", "TEMPORARY_INFO",
	"MOD_FILENAME", "PLATOON", "GAMESTATE_BUILDING", "GAMESTATE_CHARACTER",
	"GAMESTATE_FACTION", "GAMESTATE_TOWN_INSTANCE_LIST", "STATE", "SAVED_STATE",
	"INVENTORY_STATE", "INVENTORY_ITEM_STATE",
	"REPEATABLE_BUILDING_PART_SLOT", "MATERIAL_SPEC",
	"MATERIAL_SPECS_COLLECTION", "CONTAINER", "MATERIAL_SPECS_CLOTHING",
	"GAMESTATE_BUILDING_INTERIOR", "VENDOR_LIST", "MATERIAL_SPECS_WEAPON",
	"WEAPON_MANUFACTURER", "SQUAD_TEMPLATE", "ROAD", "LOCATION_NODE",
	"COLOR_DATA", "CAMERA", "MEDICAL_STATE", "MEDICAL_PART_STATE",
	"FOLIAGE_LAYER", "FOLIAGE_MESH", "GRASS", "BUILDING_FUNCTIONALITY",
	"DAY_SCHEDULE", "NEW_GAME_STARTOFF", "GAMESTATE_CRAFTING",
	"CHARACTER_APPEARANCE", "GAMESTATE_AI", "WILDLIFE_BIRDS", "MAP_FEATURES",
	"DIPLOMATIC_ASSAULTS", "SINGLE_DIPLOMATIC_ASSAULT", "AI_PACKAGE",
	"DIALOGUE_PACKAGE", "GUN_DATA", "HUMAN_CHARACTER", "ANIMAL_CHARACTER",
	"UNIQUE_SQUAD_TEMPLATE", "FACTION_TEMPLATE", "AI_SCHEDULE", "WEATHER",
	"SEASON", "EFFECT", "ITEM_PLACEMENT_GROUP", "WORD_SWAPS", "NEST",
	"NEST_ITEM", "CHARACTER_PHYSICS_ATTACHMENT", "LIGHT", "HEAD", "BLUEPRINT",
	"SHOP_TRADER_CLASS", "FOLIAGE_BUILDING", "FACTION_CAMPAIGN",
	"GAMESTATE_TOWN", "BIOME_GROUP", "EFFECT_FOG_VOLUME", "FARM_DATA",
	"FARM_PART", "ENVIRONMENT_RESOURCES", "RACE_GROUP", "ARTIFACTS",
	"MAP_ITEM", "BUILDINGS_SWAP", "ITEMS_CULTURE", "ANIMATION_EVENT",
	"TUTORIAL", "CROSSBOW", "TERRAIN_DECALS", "AMBIENT_SOUND",
	"WORLD_EVENT_STATE", "LIMB_REPLACEMENT", "ANIMATION_FILE", "____XXX___",
	"OBJECT_TYPE_MAX",
};

class Reader
{
public:
	explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

	std::size_t position() const noexcept { return position_; }
	std::size_t total() const noexcept { return bytes_.size(); }
	std::size_t remaining() const noexcept { return bytes_.size() - position_; }

	std::span<const std::byte> take(std::size_t count)
	{
		if (count > remaining())
			throw ParseError("unexpected end of file", position_);
		auto out = bytes_.subspan(position_, count);
		position_ += count;
		return out;
	}

	std::int32_t i32()
	{
		auto b = take(4);
		return static_cast<std::int32_t>(decode(b));
	}

	std::uint32_t u32()
	{
		return decode(take(4));
	}

	std::uint8_t u8()
	{
		return std::to_integer<std::uint8_t>(take(1)[0]);
	}

	bool boolean()
	{
		auto value = u8();
		// kmod.py rejects anything but 0/1; MOD_FORMAT.md's primitive table
		// says ReadBoolean accepts any non-zero. kmod.py is the parity oracle.
		if (value > 1)
			throw ParseError("invalid bool byte", position_ - 1);
		return value != 0;
	}

	std::string str()
	{
		auto length = i32();
		// MOD_FORMAT.md "Primitives": byteLen <= 0 reads as "".
		if (length <= 0)
			return {};
		auto bytes = take(static_cast<std::size_t>(length));
		return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	}

private:
	static std::uint32_t decode(std::span<const std::byte> b)
	{
		return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[0]))
			| (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[1])) << 8)
			| (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[2])) << 16)
			| (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(b[3])) << 24);
	}

	std::span<const std::byte> bytes_;
	std::size_t position_ = 0;
};

std::int32_t readCount(Reader& reader, const char* what)
{
	auto count = reader.i32();
	if (count < 0)
		throw ParseError(std::string(what) + ": negative count", reader.position() - 4);
	// A necessary bound: every entry is at least one byte. Also keeps a
	// corrupt count from driving a huge allocation.
	if (static_cast<std::size_t>(count) > reader.remaining())
		throw ParseError(std::string(what) + ": count exceeds remaining bytes",
			reader.position() - 4);
	return count;
}

template <typename T, typename Read>
std::vector<T> readList(Reader& reader, const char* what, Read read)
{
	auto count = readCount(reader, what);
	std::vector<T> list;
	list.reserve(std::min<std::size_t>(static_cast<std::size_t>(count), 1024));
	for (std::int32_t i = 0; i < count; ++i)
		list.push_back(read());
	return list;
}

template <typename Array>
void copyInto(Array& out, std::span<const std::byte> bytes)
{
	std::copy(bytes.begin(), bytes.end(), out.begin());
}

Header readHeader(Reader& reader, std::int32_t version)
{
	Header header;
	std::size_t headerEnd = 0;
	if (version >= 17)
	{
		auto sizePosition = reader.position();
		header.size = reader.u32();
		headerEnd = reader.position() + header.size;
		if (headerEnd > reader.total())
			throw ParseError("header size extends past end of file", sizePosition);
	}

	header.version = reader.i32();
	header.author = reader.str();
	header.description = reader.str();
	header.dependencies = reader.str();
	header.referenced = reader.str();

	// MOD_FORMAT.md "Header": each optional v17 block is guarded by
	// position < headerEnd. v16 has no headerEnd and no optional blocks.
	if (headerEnd != 0)
	{
		if (reader.position() < headerEnd)
		{
			header.hasSaveCounter = true;
			header.saveCounter = reader.u32();
			header.lastMergeResolve = reader.u32();
			auto mergedCount = reader.u8();
			header.merged.reserve(mergedCount);
			for (std::uint8_t i = 0; i < mergedCount; ++i)
				header.merged.push_back(MergedEntry{ reader.str(), reader.u32(), reader.u32() });
		}
		if (reader.position() < headerEnd)
		{
			header.hasDeletes = true;
			auto deleteCount = reader.u8();
			header.deletes.reserve(deleteCount);
			for (std::uint8_t i = 0; i < deleteCount; ++i)
				header.deletes.push_back(DeletedEntry{ reader.str(), reader.u32(), reader.str() });
		}
		if (reader.position() > headerEnd)
			throw ParseError("optional header block overruns header size", headerEnd);
		// The spec says the reader seeks to headerEnd; preserve the bytes.
		if (reader.position() < headerEnd)
		{
			auto extra = reader.take(headerEnd - reader.position());
			header.extra.assign(extra.begin(), extra.end());
		}
	}
	return header;
}

Record readRecord(Reader& reader, std::int32_t version, std::string_view filename)
{
	Record record;
	record.size = reader.i32();
	record.type = reader.i32();
	record.id = reader.i32();
	record.name = reader.str();
	if (version >= 7)
		record.stringID = reader.str();
	else
		// MOD_FORMAT.md "Version branches": v1..6 synthesise the key.
		record.stringID = std::to_string(record.id) + "-" + std::string(filename);

	if (version >= 15)
	{
		record.hasFlags = true;
		record.flags = reader.u32();
	}
	else if (version >= 11)
	{
		record.hasTags = true;
		record.tags = readList<Tag>(reader, "tag table", [&] {
			return Tag{ reader.str(), reader.boolean() };
		});
	}

	record.bools = readList<FieldBool>(reader, "bool fields", [&] {
		return FieldBool{ reader.str(), reader.boolean() };
	});
	record.floats = readList<FieldFloat>(reader, "float fields", [&] {
		FieldFloat field;
		field.key = reader.str();
		copyInto(field.value, reader.take(4));
		return field;
	});
	record.ints = readList<FieldInt>(reader, "int fields", [&] {
		return FieldInt{ reader.str(), reader.i32() };
	});
	// MOD_FORMAT.md "Version branches": vec3/vec4 sections exist when > 8.
	if (version > 8)
	{
		record.vec3s = readList<FieldVec3>(reader, "vec3 fields", [&] {
			FieldVec3 field;
			field.key = reader.str();
			copyInto(field.value, reader.take(12));
			return field;
		});
		record.vec4s = readList<FieldVec4>(reader, "vec4 fields", [&] {
			FieldVec4 field;
			field.key = reader.str();
			copyInto(field.value, reader.take(16));
			return field;
		});
	}
	record.strings = readList<FieldString>(reader, "string fields", [&] {
		return FieldString{ reader.str(), reader.str() };
	});
	record.files = readList<FieldFile>(reader, "file fields", [&] {
		return FieldFile{ reader.str(), reader.str() };
	});

	record.references = readList<RefCategory>(reader, "reference categories", [&] {
		RefCategory category;
		category.category = reader.str();
		category.entries = readList<RefEntry>(reader, "references", [&] {
			RefEntry entry;
			if (version < 8)
			{
				// v1..7: eight opaque bytes (an i64 FCS discards).
				entry.opaqueEntry = true;
				copyInto(entry.opaque, reader.take(8));
				return entry;
			}
			entry.targetID = reader.str();
			entry.v0 = reader.i32();
			if (version >= 10)
			{
				entry.v1 = reader.i32();
				entry.v2 = reader.i32();
				entry.hasV1V2 = true;
			}
			return entry;
		});
		return category;
	});

	record.instances = readList<Instance>(reader, "instances", [&] {
		Instance instance;
		if (version >= 15)
		{
			instance.id = reader.str();
			instance.idIsString = true;
		}
		else
		{
			instance.idInt = reader.i32();
		}
		// MOD_FORMAT.md "Version branches": instance ref string from v8.
		if (version >= 8)
			instance.targetStringID = reader.str();
		copyInto(instance.transform, reader.take(28));
		// States list from v7.
		if (version > 6)
		{
			auto stateCount = readCount(reader, "instance states");
			auto reserve = std::min<std::size_t>(
				static_cast<std::size_t>(stateCount), 1024);
			instance.states.reserve(version >= 15 ? reserve : 0);
			instance.stateInts.reserve(version >= 15 ? 0 : reserve);
			for (std::int32_t i = 0; i < stateCount; ++i)
			{
				if (version >= 15)
					instance.states.push_back(reader.str());
				else
					instance.stateInts.push_back(reader.i32());
			}
		}
		return instance;
	});
	return record;
}

class Writer
{
public:
	void i32(std::int32_t value) { putU32(static_cast<std::uint32_t>(value)); }
	void u32(std::uint32_t value) { putU32(value); }

	void u8(std::uint8_t value)
	{
		bytes_.push_back(static_cast<std::byte>(value));
	}

	void raw(std::span<const std::byte> bytes)
	{
		bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
	}

	void str(std::string_view value)
	{
		i32(static_cast<std::int32_t>(value.size()));
		if (!value.empty())
		{
			auto first = reinterpret_cast<const std::byte*>(value.data());
			bytes_.insert(bytes_.end(), first, first + value.size());
		}
	}

	std::size_t size() const noexcept { return bytes_.size(); }

	void patchU32(std::size_t position, std::uint32_t value)
	{
		bytes_[position + 0] = static_cast<std::byte>(value & 0xff);
		bytes_[position + 1] = static_cast<std::byte>((value >> 8) & 0xff);
		bytes_[position + 2] = static_cast<std::byte>((value >> 16) & 0xff);
		bytes_[position + 3] = static_cast<std::byte>((value >> 24) & 0xff);
	}

	std::vector<std::byte> finish() { return std::move(bytes_); }

private:
	void putU32(std::uint32_t value)
	{
		u8(static_cast<std::uint8_t>(value & 0xff));
		u8(static_cast<std::uint8_t>((value >> 8) & 0xff));
		u8(static_cast<std::uint8_t>((value >> 16) & 0xff));
		u8(static_cast<std::uint8_t>((value >> 24) & 0xff));
	}

	std::vector<std::byte> bytes_;
};

void writeHeader(Writer& writer, const Header& header, std::int32_t version)
{
	std::size_t sizePosition = 0;
	if (version >= 17)
	{
		sizePosition = writer.size();
		writer.u32(0);
	}
	writer.i32(header.version);
	writer.str(header.author);
	writer.str(header.description);
	writer.str(header.dependencies);
	writer.str(header.referenced);
	if (header.hasSaveCounter)
	{
		writer.u32(header.saveCounter);
		writer.u32(header.lastMergeResolve);
		writer.u8(static_cast<std::uint8_t>(header.merged.size()));
		for (const auto& entry : header.merged)
		{
			writer.str(entry.modName);
			writer.u32(entry.item);
			writer.u32(entry.merge);
		}
	}
	if (header.hasDeletes)
	{
		writer.u8(static_cast<std::uint8_t>(header.deletes.size()));
		for (const auto& entry : header.deletes)
		{
			writer.str(entry.modName);
			writer.u32(entry.version);
			writer.str(entry.items);
		}
	}
	if (!header.extra.empty())
		writer.raw(header.extra);
	if (version >= 17)
		writer.patchU32(sizePosition,
			static_cast<std::uint32_t>(writer.size() - sizePosition - 4));
}

void writeRecord(Writer& writer, const Record& record, std::int32_t version)
{
	writer.i32(record.size);
	writer.i32(record.type);
	writer.i32(record.id);
	writer.str(record.name);
	if (version >= 7)
		writer.str(record.stringID);
	if (version >= 15)
		writer.u32(record.flags);
	else if (version >= 11)
	{
		writer.i32(static_cast<std::int32_t>(record.tags.size()));
		for (const auto& tag : record.tags)
		{
			writer.str(tag.key);
			writer.u8(tag.keep ? 1 : 0);
		}
	}

	writer.i32(static_cast<std::int32_t>(record.bools.size()));
	for (const auto& field : record.bools)
	{
		writer.str(field.key);
		writer.u8(field.value ? 1 : 0);
	}
	writer.i32(static_cast<std::int32_t>(record.floats.size()));
	for (const auto& field : record.floats)
	{
		writer.str(field.key);
		writer.raw(field.value);
	}
	writer.i32(static_cast<std::int32_t>(record.ints.size()));
	for (const auto& field : record.ints)
	{
		writer.str(field.key);
		writer.i32(field.value);
	}
	if (version > 8)
	{
		writer.i32(static_cast<std::int32_t>(record.vec3s.size()));
		for (const auto& field : record.vec3s)
		{
			writer.str(field.key);
			writer.raw(field.value);
		}
		writer.i32(static_cast<std::int32_t>(record.vec4s.size()));
		for (const auto& field : record.vec4s)
		{
			writer.str(field.key);
			writer.raw(field.value);
		}
	}
	writer.i32(static_cast<std::int32_t>(record.strings.size()));
	for (const auto& field : record.strings)
	{
		writer.str(field.key);
		writer.str(field.value);
	}
	writer.i32(static_cast<std::int32_t>(record.files.size()));
	for (const auto& field : record.files)
	{
		writer.str(field.key);
		writer.str(field.value);
	}

	writer.i32(static_cast<std::int32_t>(record.references.size()));
	for (const auto& category : record.references)
	{
		writer.str(category.category);
		writer.i32(static_cast<std::int32_t>(category.entries.size()));
		for (const auto& entry : category.entries)
		{
			if (version < 8)
			{
				writer.raw(entry.opaque);
				continue;
			}
			writer.str(entry.targetID);
			writer.i32(entry.v0);
			if (version >= 10)
			{
				writer.i32(entry.v1);
				writer.i32(entry.v2);
			}
		}
	}

	writer.i32(static_cast<std::int32_t>(record.instances.size()));
	for (const auto& instance : record.instances)
	{
		if (version >= 15)
			writer.str(instance.id);
		else
			writer.i32(instance.idInt);
		if (version >= 8)
			writer.str(instance.targetStringID);
		writer.raw(instance.transform);
		if (version > 6)
		{
			if (version >= 15)
			{
				writer.i32(static_cast<std::int32_t>(instance.states.size()));
				for (const auto& state : instance.states)
					writer.str(state);
			}
			else
			{
				writer.i32(static_cast<std::int32_t>(instance.stateInts.size()));
				for (auto state : instance.stateInts)
					writer.i32(state);
			}
		}
	}
}

} // namespace

const char* itemTypeName(std::int32_t type) noexcept
{
	if (type < 0 || type >= static_cast<std::int32_t>(std::size(kItemTypes)))
		return nullptr;
	return kItemTypes[type];
}

ParseError::ParseError(std::string_view message, std::size_t offset)
	: std::runtime_error(std::string(message) + " (byte offset " + std::to_string(offset) + ")")
	, offset_(offset)
{
}

File parse(std::span<const std::byte> bytes, std::string_view filename)
{
	Reader reader(bytes);
	File file;
	file.version = reader.i32();
	if (file.version < kMinVersion || file.version > kMaxVersion)
		throw ParseError("invalid mod file version", 0);

	// MOD_FORMAT.md "File layout": header only for v16/v17.
	if (file.version >= 16)
	{
		file.hasHeader = true;
		file.header = readHeader(reader, file.version);
	}

	file.lastID = reader.i32();
	auto recordCount = readCount(reader, "record count");
	file.records.reserve(
		std::min<std::size_t>(static_cast<std::size_t>(recordCount), 4096));
	for (std::int32_t i = 0; i < recordCount; ++i)
	{
		auto start = reader.position();
		auto record = readRecord(reader, file.version, filename);
		record.encodedLength = reader.position() - start;
		file.records.push_back(std::move(record));
	}

	auto rest = reader.take(reader.remaining());
	file.leftover.assign(rest.begin(), rest.end());
	return file;
}

std::vector<std::byte> write(const File& file)
{
	Writer writer;
	writer.i32(file.version);
	if (file.hasHeader)
		writeHeader(writer, file.header, file.version);
	writer.i32(file.lastID);
	writer.i32(static_cast<std::int32_t>(file.records.size()));
	for (const auto& record : file.records)
		writeRecord(writer, record, file.version);
	writer.raw(file.leftover);
	return writer.finish();
}

namespace {

template <typename Field>
const Field* findLast(const std::vector<Field>& fields, std::string_view key) noexcept
{
	for (auto it = fields.rbegin(); it != fields.rend(); ++it)
		if (it->key == key)
			return &*it;
	return nullptr;
}

} // namespace

const FieldInt* Record::findInt(std::string_view key) const noexcept
{
	return findLast(ints, key);
}

const FieldBool* Record::findBool(std::string_view key) const noexcept
{
	return findLast(bools, key);
}

const FieldFloat* Record::findFloat(std::string_view key) const noexcept
{
	return findLast(floats, key);
}

const FieldString* Record::findString(std::string_view key) const noexcept
{
	return findLast(strings, key);
}

const FieldFile* Record::findFile(std::string_view key) const noexcept
{
	return findLast(files, key);
}

const RefCategory* Record::findRefs(std::string_view category) const noexcept
{
	for (const auto& entry : references)
		if (entry.category == category)
			return &entry;
	return nullptr;
}

std::int32_t Record::intOr(std::string_view key, std::int32_t fallback) const noexcept
{
	const auto* field = findInt(key);
	return field != nullptr ? field->value : fallback;
}

bool Record::boolOr(std::string_view key, bool fallback) const noexcept
{
	const auto* field = findBool(key);
	return field != nullptr ? field->value : fallback;
}

float Record::floatOr(std::string_view key, float fallback) const noexcept
{
	const auto* field = findFloat(key);
	return field != nullptr ? decodeFloat(*field) : fallback;
}

std::string Record::stringOr(std::string_view key, std::string_view fallback) const
{
	const auto* field = findString(key);
	return field != nullptr ? field->value : std::string(fallback);
}

const std::string& Record::fileOr(std::string_view key) const noexcept
{
	static const std::string none;
	const auto* field = findFile(key);
	return field != nullptr ? field->value : none;
}

float decodeFloat(const FieldFloat& field) noexcept
{
	const std::uint32_t bits = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.value[0]))
		| static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.value[1])) << 8
		| static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.value[2])) << 16
		| static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(field.value[3])) << 24;
	return std::bit_cast<float>(bits);
}

} // namespace kdata
