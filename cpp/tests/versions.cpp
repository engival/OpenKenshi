// Every version branch in MOD_FORMAT.md "Version branches" must parse and
// write back byte-identically. Shipped files only cover v15..v17.

#include "kdata/format.hpp"
#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

std::array<std::byte, 4> raw4(std::uint32_t value)
{
	return {
		static_cast<std::byte>(value & 0xff),
		static_cast<std::byte>((value >> 8) & 0xff),
		static_cast<std::byte>((value >> 16) & 0xff),
		static_cast<std::byte>((value >> 24) & 0xff),
	};
}

kdata::File makeFile(std::int32_t version)
{
	kdata::File file;
	file.version = version;
	file.hasHeader = version >= 16;
	file.header.version = 1;
	file.header.author = "author";
	file.header.dependencies = "dep.mod";
	if (version >= 17)
	{
		file.header.hasSaveCounter = true;
		file.header.saveCounter = 2;
		file.header.merged = { { "merged.mod", 3, 4 } };
		file.header.hasDeletes = true;
		file.header.deletes = { { "gone.mod", 5, "1:2" } };
	}
	file.lastID = 42;

	kdata::Record record;
	record.type = 4;
	record.id = 7;
	record.name = "record";
	record.stringID = version >= 7 ? "7-test.mod" : "";
	if (version >= 15)
	{
		record.hasFlags = true;
		record.flags = 0x80000013;
	}
	else if (version >= 11)
	{
		record.hasTags = true;
		record.tags = { { "keep", true }, { "drop", false } };
	}
	record.bools = { { "flag", true } };
	record.floats = { { "f", raw4(0x80000000) } }; // -0
	record.ints = { { "i", -123 } };
	if (version > 8)
	{
		record.vec3s = { { "v3", {} } };
		record.vec4s = { { "v4", {} } };
	}
	record.strings = { { "s", "text" } };
	record.files = { { "p", "a/b" } };

	kdata::RefEntry reference;
	if (version < 8)
	{
		reference.opaqueEntry = true;
		reference.opaque = { std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
			std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8} };
	}
	else
	{
		reference.targetID = "9-base";
		reference.v0 = 11;
		if (version >= 10)
		{
			reference.v1 = 22;
			reference.v2 = -33;
			reference.hasV1V2 = true;
		}
	}
	record.references = { { "cat", { reference } } };

	kdata::Instance instance;
	if (version >= 15)
	{
		instance.id = "inst";
		instance.idIsString = true;
	}
	else
	{
		instance.idInt = 9;
	}
	if (version >= 8)
		instance.targetStringID = "9-base";
	if (version > 6)
	{
		if (version >= 15)
			instance.states = { "state" };
		else
			instance.stateInts = { 5 };
	}
	record.instances = { instance };

	file.records = { record };
	return file;
}

void checkVersion(std::int32_t version)
{
	auto file = makeFile(version);
	auto bytes = kdata::write(file);
	auto parsed = kdata::parse(bytes, "test.mod");
	test::require(kdata::write(parsed) == bytes,
		"v" + std::to_string(version) + ": not byte-exact");
	test::require(parsed.version == version, "version");
	test::require(parsed.hasHeader == (version >= 16), "header presence");
	test::require(parsed.lastID == 42, "lastID");
	test::require(parsed.records.size() == 1, "record count");

	const auto& record = parsed.records[0];
	test::require(record.type == 4 && record.id == 7, "record id/type");
	test::require(record.name == "record", "record name");
	if (version >= 7)
		test::require(record.stringID == "7-test.mod", "stringID");
	else
		test::require(record.stringID == "7-test.mod",
			"pre-v7 synthesized stringID");
	test::require(record.hasFlags == (version >= 15), "flags presence");
	test::require(record.hasTags == (version >= 11 && version < 15),
		"tag table presence");
	if (record.hasTags)
		test::require(record.tags.size() == 2 && record.tags[0].key == "keep"
			&& record.tags[0].keep && !record.tags[1].keep, "tags");
	test::require(record.vec3s.size() == (version > 8 ? 1u : 0u), "vec3 branch");
	test::require(record.vec4s.size() == (version > 8 ? 1u : 0u), "vec4 branch");

	const auto& reference = record.references[0].entries[0];
	test::require(reference.opaqueEntry == (version < 8), "opaque ref branch");
	if (version >= 8)
		test::require(reference.targetID == "9-base" && reference.v0 == 11,
			"reference target/v0");
	if (version >= 10)
		test::require(reference.hasV1V2 && reference.v1 == 22
			&& reference.v2 == -33, "reference v1/v2");
	else if (version >= 8)
		test::require(!reference.hasV1V2, "v8/v9 has no v1/v2");

	const auto& instance = record.instances[0];
	test::require(instance.idIsString == (version >= 15), "instance id branch");
	if (version >= 15)
		test::require(instance.id == "inst", "string instance id");
	else
		test::require(instance.idInt == 9, "integer instance id");
	test::require((instance.targetStringID == "9-base") == (version >= 8),
		"instance ref branch");
	test::require(instance.states.size() == (version >= 15 ? 1u : 0u), "states");
	test::require(instance.stateInts.size()
		== (version > 6 && version < 15 ? 1u : 0u), "int states");
}

} // namespace

int main()
{
	try
	{
		for (std::int32_t version = kdata::kMinVersion;
			version <= kdata::kMaxVersion; ++version)
			checkVersion(version);
	}
	catch (const std::exception& error)
	{
		std::cerr << "versions: " << error.what() << "\n";
		return 1;
	}
	std::cout << "versions: 1..17 byte-exact\n";
	return 0;
}
