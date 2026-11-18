// Truncated and corrupted inputs must raise ParseError (with an offset),
// never crash. Built a second time against an ASan/UBSan parser.

#include "kdata/format.hpp"
#include "test_util.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

std::array<std::byte, 4> floatBytes(std::uint32_t bits)
{
	return {
		static_cast<std::byte>(bits & 0xff),
		static_cast<std::byte>((bits >> 8) & 0xff),
		static_cast<std::byte>((bits >> 16) & 0xff),
		static_cast<std::byte>((bits >> 24) & 0xff),
	};
}

kdata::File makeSynthetic()
{
	kdata::File file;
	file.version = 17;
	file.hasHeader = true;
	file.header.version = 1;
	file.header.author = "au";
	file.header.dependencies = "a.mod,b.mod";
	file.header.hasSaveCounter = true;
	file.header.saveCounter = 3;
	file.header.lastMergeResolve = 4;
	file.header.merged = { { "m.mod", 1, 2 } };
	file.header.hasDeletes = true;
	file.header.deletes = { { "d.mod", 5, "1:2" } };
	file.header.extra = { std::byte{0x58}, std::byte{0x59} };
	file.lastID = 99;

	kdata::Record record;
	record.type = 1;
	record.id = 5;
	record.name = "Name";
	record.stringID = "5-x.mod";
	record.hasFlags = true;
	record.flags = 0x80000013;
	record.bools = { { "B", true }, { "C", false } };
	record.floats = { { "F", floatBytes(0x7fc00001) } };
	record.ints = { { "I", -7 } };
	record.vec3s = { { "V3", {} } };
	record.vec4s = { { "V4", {} } };
	record.strings = { { "S", "h\xc3\xa9llo" } };
	record.files = { { "P", "a/b" } };

	kdata::RefEntry reference;
	reference.targetID = "9-gamedata.base";
	reference.v0 = 1;
	reference.v1 = 0;
	reference.v2 = 100;
	reference.hasV1V2 = true;
	record.references = { { "cat", { reference } } };

	kdata::Instance instance;
	instance.id = "i1";
	instance.idIsString = true;
	instance.targetStringID = "9-gamedata.base";
	instance.states = { "s1", "s2" };
	record.instances = { instance };

	file.records = { record };
	return file;
}

void checkTruncations(const std::vector<std::byte>& bytes, const std::string& label)
{
	for (std::size_t length = 0; length < bytes.size(); ++length)
	{
		try
		{
			kdata::parse(std::span(bytes.data(), length), "x.mod");
			test::require(false, label + ": truncation at " + std::to_string(length)
				+ " was accepted");
		}
		catch (const kdata::ParseError& error)
		{
			test::require(error.offset() <= length, label + ": offset past truncation");
		}
	}
}

void runShipped()
{
	const std::filesystem::path path =
		std::filesystem::path(KDATA_DATA_DIR) / "gamedata.base";
	std::error_code error;
	if (!std::filesystem::is_regular_file(path, error))
	{
		std::cout << "malformed: gamedata.base not found, skipping shipped truncation\n";
		return;
	}
	auto bytes = test::readFile(path.string());
	const std::vector<std::size_t> offsets = {
		0, 1, 2, 3, 4, 5, 8, 12, 16, 20, 64, 100,
		bytes.size() / 4, bytes.size() / 2, bytes.size() * 3 / 4, bytes.size() - 1,
	};
	for (auto length : offsets)
	{
		try
		{
			kdata::parse(std::span(bytes.data(), length), "gamedata.base");
			test::require(false, "shipped truncation at " + std::to_string(length)
				+ " was accepted");
		}
		catch (const kdata::ParseError& error)
		{
			test::require(error.offset() <= length, "offset past shipped truncation");
		}
	}
}

void runFuzz(const std::vector<std::byte>& seed)
{
	std::mt19937 generator(0x5eed);
	for (int iteration = 0; iteration < 500; ++iteration)
	{
		auto bytes = seed;
		if (generator() % 3 == 0)
		{
			bytes.resize(generator() % (bytes.size() + 1));
		}
		else
		{
			auto position = generator() % bytes.size();
			auto bit = static_cast<std::uint8_t>(1u << (generator() % 8));
			bytes[position] = static_cast<std::byte>(
				std::to_integer<std::uint8_t>(bytes[position]) ^ bit);
		}
		try
		{
			kdata::parse(bytes, "x.mod");
		}
		catch (const kdata::ParseError&)
		{
		}
		catch (const std::exception& exception)
		{
			test::require(false, std::string("unexpected exception: ") + exception.what());
		}
	}
}

void run()
{
	auto file = makeSynthetic();
	auto bytes = kdata::write(file);
	auto parsed = kdata::parse(bytes, "x.mod");
	test::require(kdata::write(parsed) == bytes, "synthetic round trip");

	checkTruncations(bytes, "synthetic");
	runShipped();
	runFuzz(bytes);
}

} // namespace

int main()
{
	try
	{
		run();
	}
	catch (const std::exception& error)
	{
		std::cerr << "malformed: " << error.what() << "\n";
		return 1;
	}
	std::cout << "malformed: truncation and fuzz checks ok\n";
	return 0;
}
