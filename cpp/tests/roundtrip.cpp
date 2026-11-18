// Parse+write must reproduce every shipped record-format file byte-for-byte.

#include "kdata/format.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main()
{
	const fs::path root = KDATA_DATA_DIR;
	std::error_code error;
	if (!fs::is_directory(root, error))
	{
		std::cerr << "roundtrip: data directory not found: " << root << "\n";
		return 1;
	}

	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(root))
	{
		auto extension = entry.path().extension().string();
		if (entry.is_regular_file() && (extension == ".base" || extension == ".mod"))
			files.push_back(entry.path());
	}
	auto gameDataFiles = files.size();
	auto levelData = root / "newland" / "leveldata";
	if (fs::is_directory(levelData, error))
	{
		for (const auto& entry : fs::recursive_directory_iterator(levelData))
		{
			auto extension = entry.path().extension().string();
			if (entry.is_regular_file() && (extension == ".zone" || extension == ".level"))
				files.push_back(entry.path());
		}
	}
	std::sort(files.begin(), files.end());

	std::size_t exact = 0;
	std::vector<std::string> failures;
	double parseSeconds = 0.0;
	double writeSeconds = 0.0;
	for (const auto& path : files)
	{
		try
		{
			auto bytes = test::readFile(path.string());
			auto started = std::chrono::steady_clock::now();
			auto file = kdata::parse(bytes, path.filename().string());
			auto parsed = std::chrono::steady_clock::now();
			auto output = kdata::write(file);
			auto written = std::chrono::steady_clock::now();
			parseSeconds += std::chrono::duration<double>(parsed - started).count();
			writeSeconds += std::chrono::duration<double>(written - parsed).count();
			if (output == bytes)
				++exact;
			else
				failures.push_back(path.string());
		}
		catch (const std::exception& exception)
		{
			failures.push_back(path.string() + ": " + exception.what());
		}
	}

	std::cout << "roundtrip: " << exact << "/" << files.size()
		<< " byte-exact (.base/.mod " << gameDataFiles
		<< ", .zone/.level " << files.size() - gameDataFiles << ")\n";
	std::cout << std::fixed << std::setprecision(3)
		<< "parse " << parseSeconds << " s, write " << writeSeconds << " s\n";
	for (std::size_t i = 0; i < failures.size() && i < 10; ++i)
		std::cerr << "MISMATCH: " << failures[i] << "\n";
	if (!failures.empty())
	{
		std::cerr << "roundtrip: " << failures.size() << " failures\n";
		return 1;
	}
	return 0;
}
