// Record count and per-type counts must equal tools/kmod.py's, file by file.

#include "kdata/format.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string shellQuote(const std::string& value)
{
	std::string out = "'";
	for (char c : value)
	{
		if (c == '\'')
			out += "'\\''";
		else
			out.push_back(c);
	}
	return out + "'";
}

std::string runKmod(const std::string& path)
{
	auto command = "python3 -I " + shellQuote(KDATA_KMOD) + " " + shellQuote(path)
		+ " 2>&1";
	FILE* pipe = popen(command.c_str(), "r");
	if (pipe == nullptr)
		throw test::Failure("cannot run python3");
	std::string output;
	char buffer[4096];
	while (auto count = std::fread(buffer, 1, sizeof(buffer), pipe))
		output.append(buffer, count);
	pclose(pipe);
	return output;
}

long parseRecordsLine(std::string_view output)
{
	auto position = output.find(" records=");
	if (position == std::string_view::npos)
		throw test::Failure("kmod output has no records= field:\n" + std::string(output));
	position += 9;
	long value = 0;
	while (position < output.size() && std::isdigit(static_cast<unsigned char>(output[position])))
		value = value * 10 + (output[position++] - '0');
	return value;
}

std::map<std::string, long> parseTypesLine(std::string_view output)
{
	auto position = output.find("   types: ");
	if (position == std::string_view::npos)
		throw test::Failure("kmod output has no types line:\n" + std::string(output));
	auto open = output.find('{', position);
	auto close = output.find('}', open);
	if (open == std::string_view::npos || close == std::string_view::npos)
		throw test::Failure("kmod types line is malformed");

	std::map<std::string, long> counts;
	std::size_t i = open + 1;
	while (i < close)
	{
		while (i < close && (output[i] == ' ' || output[i] == ','))
			++i;
		if (i >= close)
			break;
		std::string key;
		if (output[i] == '\'')
		{
			++i;
			while (i < close && output[i] != '\'')
				key.push_back(output[i++]);
			++i;
		}
		else
		{
			while (i < close && output[i] != ':')
				key.push_back(output[i++]);
			while (!key.empty() && key.back() == ' ')
				key.pop_back();
		}
		while (i < close && output[i] == ' ')
			++i;
		if (i < close && output[i] == ':')
			++i;
		while (i < close && output[i] == ' ')
			++i;
		bool negative = false;
		if (i < close && output[i] == '-')
		{
			negative = true;
			++i;
		}
		long value = 0;
		while (i < close && std::isdigit(static_cast<unsigned char>(output[i])))
			value = value * 10 + (output[i++] - '0');
		counts[key] = negative ? -value : value;
	}
	return counts;
}

std::string typeKey(std::int32_t type)
{
	auto name = kdata::itemTypeName(type);
	return name ? std::string(name) : std::to_string(type);
}

} // namespace

int main()
{
	const fs::path root = KDATA_DATA_DIR;
	std::error_code error;
	if (!fs::is_directory(root, error))
	{
		std::cerr << "parity: data directory not found: " << root << "\n";
		return 1;
	}

	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(root))
	{
		auto extension = entry.path().extension().string();
		if (entry.is_regular_file() && (extension == ".base" || extension == ".mod"))
			files.push_back(entry.path());
	}
	std::sort(files.begin(), files.end());
	if (files.empty())
	{
		std::cerr << "parity: no .base/.mod files in " << root << "\n";
		return 1;
	}

	bool ok = true;
	for (const auto& path : files)
	{
		auto output = runKmod(path.string());
		long expectedRecords = parseRecordsLine(output);
		auto expectedTypes = parseTypesLine(output);

		auto bytes = test::readFile(path.string());
		auto file = kdata::parse(bytes, path.filename().string());
		std::map<std::string, long> actualTypes;
		for (const auto& record : file.records)
			++actualTypes[typeKey(record.type)];

		bool same = expectedRecords == static_cast<long>(file.records.size())
			&& expectedTypes == actualTypes;
		std::cout << "parity " << path.filename().string() << ": records "
			<< expectedRecords << ", " << expectedTypes.size() << " types: "
			<< (same ? "ok" : "MISMATCH") << "\n";
		if (!same)
		{
			ok = false;
			std::cerr << "  expected records " << expectedRecords << ", got "
				<< file.records.size() << "\n";
			for (const auto& item : expectedTypes)
			{
				auto found = actualTypes.find(item.first);
				long actual = found == actualTypes.end() ? 0 : found->second;
				if (actual != item.second)
					std::cerr << "  type " << item.first << ": expected "
						<< item.second << ", got " << actual << "\n";
			}
			for (const auto& item : actualTypes)
				if (expectedTypes.find(item.first) == expectedTypes.end())
					std::cerr << "  unexpected type " << item.first << ": "
						<< item.second << "\n";
		}
	}
	return ok ? 0 : 1;
}
