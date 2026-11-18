#pragma once

// Data model and byte-level reader/writer for the Kenshi game-data record
// format. Mirrors tools/kmod.py; section names follow
// docs/formats/MOD_FORMAT.md ("File layout", "Record", "Header").

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {

// MOD_FORMAT.md "Record": Reference.Removed.
inline constexpr std::int32_t kReferenceRemoved = 0x7fffffff;
inline constexpr std::int32_t kMinVersion = 1;
inline constexpr std::int32_t kMaxVersion = 17;

// The itemType table from MOD_FORMAT.md; nullptr when out of range.
const char* itemTypeName(std::int32_t type) noexcept;

class ParseError : public std::runtime_error
{
public:
	ParseError(std::string_view message, std::size_t offset);
	std::size_t offset() const noexcept { return offset_; }

private:
	std::size_t offset_;
};

struct FieldBool
{
	std::string key;
	bool value = false;
};

// Float payloads are kept as raw bytes so NaN payloads and -0 round-trip.
struct FieldFloat
{
	std::string key;
	std::array<std::byte, 4> value{};
};

struct FieldInt
{
	std::string key;
	std::int32_t value = 0;
};

struct FieldVec3
{
	std::string key;
	std::array<std::byte, 12> value{};
};

struct FieldVec4
{
	std::string key;
	std::array<std::byte, 16> value{};
};

struct FieldString
{
	std::string key;
	std::string value;
};

// FCS `File` type; encoded exactly like a string, only the section differs.
struct FieldFile
{
	std::string key;
	std::string value;
};

// v11..v14 per-record {key -> keep} filter.
struct Tag
{
	std::string key;
	bool keep = false;
};

struct RefEntry
{
	// v8+: target stringID. Empty for the opaque pre-v8 form.
	std::string targetID;
	std::int32_t v0 = 0;
	std::int32_t v1 = 0;
	std::int32_t v2 = 0;
	// v10+ has v1/v2; v8/v9 only v0.
	bool hasV1V2 = false;
	// v1..v7: eight opaque bytes (ReadInt64, discarded by FCS).
	bool opaqueEntry = false;
	std::array<std::byte, 8> opaque{};
};

struct RefCategory
{
	std::string category;
	std::vector<RefEntry> entries;
};

struct Instance
{
	std::string id; // v15+
	std::int32_t idInt = 0; // v1..v14
	bool idIsString = false;
	std::string targetStringID; // v8+; "" = instance deleted
	// x y z qw qx qy qz, each a raw 4-byte float; rotation is W-first.
	std::array<std::byte, 28> transform{};
	std::vector<std::string> states; // v15+
	std::vector<std::int32_t> stateInts; // v7..v14
};

struct Record
{
	std::int32_t size = 0; // stored field; reader ignores it
	std::int32_t type = 0;
	std::int32_t id = 0;
	std::string name;
	std::string stringID;

	bool hasFlags = false; // v15+
	std::uint32_t flags = 0;
	bool hasTags = false; // v11..v14
	std::vector<Tag> tags;

	std::vector<FieldBool> bools;
	std::vector<FieldFloat> floats;
	std::vector<FieldInt> ints;
	std::vector<FieldVec3> vec3s;
	std::vector<FieldVec4> vec4s;
	std::vector<FieldString> strings;
	std::vector<FieldFile> files;
	std::vector<RefCategory> references;
	std::vector<Instance> instances;

	// Bytes this record occupied when parsed, including its size field.
	// Parse artefact used by kdump; not written back.
	std::size_t encodedLength = 0;

	// Typed field lookup. Within a section the last matching key wins (the
	// merged-record convention); lookups never cross sections, and a missing
	// key returns nullptr / the fallback.
	const FieldInt* findInt(std::string_view key) const noexcept;
	const FieldBool* findBool(std::string_view key) const noexcept;
	const FieldFloat* findFloat(std::string_view key) const noexcept;
	const FieldString* findString(std::string_view key) const noexcept;
	const FieldFile* findFile(std::string_view key) const noexcept;
	// First category with this name (merged records hold one per name).
	const RefCategory* findRefs(std::string_view category) const noexcept;

	std::int32_t intOr(std::string_view key, std::int32_t fallback) const noexcept;
	bool boolOr(std::string_view key, bool fallback) const noexcept;
	float floatOr(std::string_view key, float fallback) const noexcept;
	std::string stringOr(std::string_view key, std::string_view fallback = {}) const;
	const std::string& fileOr(std::string_view key) const noexcept; // "" if absent
};

// Float payloads are raw little-endian bytes; decode them.
float decodeFloat(const FieldFloat& field) noexcept;

struct MergedEntry
{
	std::string modName;
	std::uint32_t item = 0;
	std::uint32_t merge = 0;
};

struct DeletedEntry
{
	std::string modName;
	std::uint32_t version = 0;
	std::string items;
};

struct Header
{
	std::uint32_t size = 0; // v17 size prefix (recomputed on write)
	std::int32_t version = 0;
	std::string author;
	std::string description;
	std::string dependencies;
	std::string referenced;

	bool hasSaveCounter = false;
	std::uint32_t saveCounter = 0;
	std::uint32_t lastMergeResolve = 0;
	std::vector<MergedEntry> merged;

	bool hasDeletes = false;
	std::vector<DeletedEntry> deletes;

	// v17 only, unknown bytes before headerEnd (the spec says the reader
	// seeks over them; kmod.py instead errors). Kept so writing round-trips.
	std::vector<std::byte> extra;
};

struct File
{
	std::int32_t version = 0;
	bool hasHeader = false;
	Header header;
	std::int32_t lastID = 0;
	std::vector<Record> records;
	// Bytes after the last record. v15 zone/level files carry their
	// "trailing block" here; kmod.py reports but does not write it.
	std::vector<std::byte> leftover;
};

// `filename` is only used by versions < 7 to synthesise
// "{id}-{filename}" (MOD_FORMAT.md "Version branches"). Pass a basename.
File parse(std::span<const std::byte> bytes, std::string_view filename = {});

// Byte layout follows kmod.py's write(), including the stored record size
// field and, unlike kmod.py, File::leftover.
std::vector<std::byte> write(const File& file);

} // namespace kdata
