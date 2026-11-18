// Load files in order through GameData and write one TSV line per merged
// record: type, stringID, name, field count, reference count, instance count.
// Sorted by stringID.

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> readFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw std::runtime_error("cannot open " + path);
	in.seekg(0, std::ios::end);
	auto size = in.tellg();
	in.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty())
		in.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	if (!in)
		throw std::runtime_error("cannot read " + path);
	return bytes;
}

std::size_t fieldCount(const kdata::Record& record)
{
	return record.bools.size() + record.floats.size() + record.ints.size()
		+ record.vec3s.size() + record.vec4s.size() + record.strings.size()
		+ record.files.size();
}

std::size_t referenceCount(const kdata::Record& record)
{
	std::size_t count = 0;
	for (const auto& category : record.references)
		count += category.entries.size();
	return count;
}

std::string typeName(std::int32_t type)
{
	auto name = kdata::itemTypeName(type);
	return name ? name : std::to_string(type);
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::cerr << "usage: kmerge <out.tsv> <file>...\n";
		return 2;
	}
	try
	{
		kdata::GameData data;
		for (int i = 2; i < argc; ++i)
		{
			auto bytes = readFile(argv[i]);
			auto name = std::filesystem::path(argv[i]).filename().string();
			data.apply(kdata::parse(bytes, name));
		}

		std::vector<const kdata::Record*> records;
		records.reserve(data.size());
		for (const auto& record : data.records())
			records.push_back(&record);
		std::sort(records.begin(), records.end(),
			[](const kdata::Record* a, const kdata::Record* b) {
				return a->stringID < b->stringID;
			});

		std::ofstream out(argv[1], std::ios::binary);
		if (!out)
			throw std::runtime_error(std::string("cannot write ") + argv[1]);
		for (const auto* record : records)
			out << typeName(record->type) << '\t' << record->stringID << '\t'
				<< record->name << '\t' << fieldCount(*record) << '\t'
				<< referenceCount(*record) << '\t' << record->instances.size()
				<< '\n';
		std::cout << "merged records: " << records.size() << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "kmerge: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
