// Summary dumper. The line formats mirror tools/kmod.py's __main__ block so
// the two outputs can be compared as text.

#include "kdata/format.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
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

// Python repr() for a str; UTF-8 bytes are emitted unchanged like Python 3.
std::string pyQuote(std::string_view value)
{
	char quote = '\'';
	if (value.find('\'') != std::string_view::npos
		&& value.find('"') == std::string_view::npos)
		quote = '"';
	std::string out(1, quote);
	for (unsigned char c : value)
	{
		if (c == static_cast<unsigned char>(quote) || c == '\\')
		{
			out.push_back('\\');
			out.push_back(static_cast<char>(c));
		}
		else if (c == '\n')
			out += "\\n";
		else if (c == '\r')
			out += "\\r";
		else if (c == '\t')
			out += "\\t";
		else if (c < 0x20 || c == 0x7f)
		{
			static const char* hex = "0123456789abcdef";
			out += "\\x";
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 0xf]);
		}
		else
			out.push_back(static_cast<char>(c));
	}
	out.push_back(quote);
	return out;
}

std::string pyInt(std::int64_t value)
{
	return std::to_string(value);
}

std::string pyList(const std::vector<std::string>& items)
{
	std::string out = "[";
	for (std::size_t i = 0; i < items.size(); ++i)
	{
		if (i)
			out += ", ";
		out += items[i];
	}
	out += "]";
	return out;
}

std::string pyDict(const std::vector<std::pair<std::string, std::string>>& items)
{
	std::string out = "{";
	for (std::size_t i = 0; i < items.size(); ++i)
	{
		if (i)
			out += ", ";
		out += items[i].first + ": " + items[i].second;
	}
	out += "}";
	return out;
}

std::string headerRepr(const kdata::Header& header, std::int32_t version)
{
	std::vector<std::pair<std::string, std::string>> items;
	if (version >= 17)
		items.emplace_back(pyQuote("size"), pyInt(header.size));
	items.emplace_back(pyQuote("version"), pyInt(header.version));
	items.emplace_back(pyQuote("author"), pyQuote(header.author));
	items.emplace_back(pyQuote("description"), pyQuote(header.description));
	items.emplace_back(pyQuote("dependencies"), pyQuote(header.dependencies));
	items.emplace_back(pyQuote("referenced"), pyQuote(header.referenced));
	if (header.hasSaveCounter)
	{
		items.emplace_back(pyQuote("saveCounter"), pyInt(header.saveCounter));
		items.emplace_back(pyQuote("lastMergeResolve"), pyInt(header.lastMergeResolve));
		std::vector<std::string> merged;
		for (const auto& entry : header.merged)
			merged.push_back("(" + pyQuote(entry.modName) + ", " + pyInt(entry.item)
				+ ", " + pyInt(entry.merge) + ")");
		items.emplace_back(pyQuote("merged"), pyList(merged));
	}
	if (header.hasDeletes)
	{
		std::vector<std::string> deletes;
		for (const auto& entry : header.deletes)
			deletes.push_back("(" + pyQuote(entry.modName) + ", " + pyInt(entry.version)
				+ ", " + pyQuote(entry.items) + ")");
		items.emplace_back(pyQuote("deletes"), pyList(deletes));
	}
	return pyDict(items);
}

template <typename Field>
std::vector<std::string> keysOf(const std::vector<Field>& fields)
{
	std::vector<std::string> keys;
	keys.reserve(fields.size());
	for (const auto& field : fields)
		keys.push_back(field.key);
	return keys;
}

template <typename Key>
std::vector<std::pair<Key, std::int64_t>> countFirstSeen(
	const std::vector<Key>& occurrences)
{
	std::vector<std::pair<Key, std::int64_t>> counts;
	for (const auto& key : occurrences)
	{
		auto found = std::find_if(counts.begin(), counts.end(),
			[&](const auto& item) { return item.first == key; });
		if (found == counts.end())
			counts.emplace_back(key, 1);
		else
			++found->second;
	}
	return counts;
}

// Python Counter.most_common: stable sort by count descending, so ties keep
// first-seen order.
template <typename Key>
std::vector<std::pair<Key, std::int64_t>> countMostCommon(
	const std::vector<Key>& occurrences)
{
	auto counts = countFirstSeen(occurrences);
	std::stable_sort(counts.begin(), counts.end(),
		[](const auto& a, const auto& b) { return a.second > b.second; });
	return counts;
}

std::string typeKey(std::int32_t type)
{
	auto name = kdata::itemTypeName(type);
	return name ? pyQuote(name) : pyInt(type);
}

void dumpOne(const std::string& path)
{
	auto bytes = readFile(path);
	auto name = std::filesystem::path(path).filename().string();
	auto file = kdata::parse(bytes, name);
	auto roundTrip = kdata::write(file) == bytes;

	std::cout << "== " << name << "  size=" << bytes.size()
		<< " ver=" << file.version << " lastID=" << file.lastID
		<< " records=" << file.records.size()
		<< " leftover=" << file.leftover.size() << "\n";
	if (file.hasHeader)
		std::cout << "   header: " << headerRepr(file.header, file.version) << "\n";
	std::cout << "   roundtrip byte-exact: " << (roundTrip ? "True" : "False") << "\n";

	std::vector<std::int32_t> types;
	for (const auto& record : file.records)
		types.push_back(record.type);
	std::vector<std::pair<std::string, std::string>> typeItems;
	for (const auto& item : countMostCommon(types))
		typeItems.emplace_back(typeKey(item.first), pyInt(item.second));
	std::cout << "   types: " << pyDict(typeItems) << "\n";

	std::vector<std::string> flagOccurrences;
	for (const auto& record : file.records)
	{
		if (!record.hasFlags)
			flagOccurrences.push_back("0x0");
		else
		{
			char buffer[16];
			std::snprintf(buffer, sizeof(buffer), "0x%x", record.flags);
			flagOccurrences.push_back(buffer);
		}
	}
	auto flagCounts = countMostCommon(flagOccurrences);
	if (flagCounts.size() > 12)
		flagCounts.resize(12);
	std::vector<std::pair<std::string, std::string>> flagItems;
	for (const auto& item : flagCounts)
		flagItems.emplace_back(pyQuote(item.first), pyInt(item.second));
	std::cout << "   flags: " << pyDict(flagItems) << "\n";

	std::vector<std::string> sizeOccurrences;
	std::vector<std::int32_t> distinctSizes;
	for (const auto& record : file.records)
	{
		if (record.size == 0)
			sizeOccurrences.push_back("zero");
		else
			sizeOccurrences.push_back(
				record.size == static_cast<std::int32_t>(record.encodedLength)
					? "True" : "False");
		distinctSizes.push_back(record.size);
	}
	std::sort(distinctSizes.begin(), distinctSizes.end());
	distinctSizes.erase(std::unique(distinctSizes.begin(), distinctSizes.end()),
		distinctSizes.end());
	if (distinctSizes.size() > 5)
		distinctSizes.resize(5);
	std::vector<std::pair<std::string, std::string>> sizeItems;
	for (const auto& item : countFirstSeen(sizeOccurrences))
	{
		auto key = item.first;
		if (key == "True" || key == "False")
			sizeItems.emplace_back(key, pyInt(item.second));
		else
			sizeItems.emplace_back(pyQuote(key), pyInt(item.second));
	}
	std::vector<std::string> distinctRepr;
	for (auto size : distinctSizes)
		distinctRepr.push_back(pyInt(size));
	std::cout << "   size field: " << pyDict(sizeItems)
		<< " distinct sizes sample: " << pyList(distinctRepr) << "\n";

	std::int64_t fieldCounts[7] = {};
	for (const auto& record : file.records)
	{
		fieldCounts[0] += static_cast<std::int64_t>(record.bools.size());
		fieldCounts[1] += static_cast<std::int64_t>(record.floats.size());
		fieldCounts[2] += static_cast<std::int64_t>(record.ints.size());
		fieldCounts[3] += static_cast<std::int64_t>(record.vec3s.size());
		fieldCounts[4] += static_cast<std::int64_t>(record.vec4s.size());
		fieldCounts[5] += static_cast<std::int64_t>(record.strings.size());
		fieldCounts[6] += static_cast<std::int64_t>(record.files.size());
	}
	std::vector<std::pair<std::string, std::string>> fieldItems;
	static const char* kinds[] = {
		"bool", "float", "int", "vec3", "vec4", "string", "file"
	};
	for (int i = 0; i < 7; ++i)
		fieldItems.emplace_back(pyQuote(kinds[i]), pyInt(fieldCounts[i]));
	std::int64_t refCount = 0;
	std::int64_t instCount = 0;
	std::int64_t stateCount = 0;
	for (const auto& record : file.records)
	{
		for (const auto& category : record.references)
			refCount += static_cast<std::int64_t>(category.entries.size());
		instCount += static_cast<std::int64_t>(record.instances.size());
		for (const auto& instance : record.instances)
			stateCount += static_cast<std::int64_t>(
				instance.states.size() + instance.stateInts.size());
	}
	std::cout << "   fields:  " << pyDict(fieldItems)
		<< " refs " << refCount << " inst " << instCount
		<< " states " << stateCount << "\n";

	// kmod.py classifies only on v0; keep that for textual parity.
	std::vector<std::string> refOccurrences;
	for (const auto& record : file.records)
		for (const auto& category : record.references)
			for (const auto& entry : category.entries)
				refOccurrences.push_back(
					entry.v0 == kdata::kReferenceRemoved ? "removed(MAX)" : "normal");
	std::vector<std::pair<std::string, std::string>> refItems;
	for (const auto& item : countFirstSeen(refOccurrences))
		refItems.emplace_back(pyQuote(item.first), pyInt(item.second));
	std::cout << "   refs: " << pyDict(refItems) << "\n";

	std::int64_t unsorted = 0;
	std::int64_t totalLists = 0;
	std::int64_t unsortedCategories = 0;
	for (const auto& record : file.records)
	{
		const std::vector<std::vector<std::string>> sections = {
			keysOf(record.bools), keysOf(record.floats), keysOf(record.ints),
			keysOf(record.vec3s), keysOf(record.vec4s), keysOf(record.strings),
			keysOf(record.files),
		};
		for (const auto& keys : sections)
		{
			++totalLists;
			if (!std::is_sorted(keys.begin(), keys.end()))
				++unsorted;
		}
		bool categoriesSorted = true;
		for (std::size_t i = 1; i < record.references.size(); ++i)
			if (record.references[i].category < record.references[i - 1].category)
				categoriesSorted = false;
		if (!categoriesSorted)
			++unsortedCategories;
	}
	std::cout << "   field lists not in ordinal order: " << unsorted << "/"
		<< totalLists << "; ref-category lists unsorted: "
		<< unsortedCategories << "\n";

	std::int64_t emptyRef = 0;
	std::int64_t removedRecords = 0;
	for (const auto& record : file.records)
	{
		for (const auto& instance : record.instances)
			if (instance.targetStringID.empty())
				++emptyRef;
		for (const auto& field : record.bools)
			if (field.key == "REMOVED")
			{
				++removedRecords;
				break;
			}
	}
	std::cout << "   instances with empty ref (deleted): " << emptyRef
		<< "  records with REMOVED bool: " << removedRecords << "\n";
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::cerr << "usage: kdump <file>...\n";
		return 2;
	}
	int status = 0;
	for (int i = 1; i < argc; ++i)
	{
		try
		{
			dumpOne(argv[i]);
		}
		catch (const kdata::ParseError& error)
		{
			std::cerr << "kdump: " << argv[i] << ": " << error.what() << "\n";
			status = 1;
		}
		catch (const std::exception& error)
		{
			std::cerr << "kdump: " << argv[i] << ": " << error.what() << "\n";
			status = 1;
		}
	}
	return status;
}
