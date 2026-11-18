// Bilinear heights must match the reference terrain.py TIFF reader.
// Sample points include the four world corners and zone-cell borders.

#include "kworld/heightmap.hpp"
#include "kworld_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

namespace {

const std::vector<std::pair<double, double>> kPoints = {
	// world corners
	{ -147456.0, -147456.0 },
	{ -147456.0, 147456.0 },
	{ 147456.0, -147456.0 },
	{ 147456.0, 147456.0 },
	// cell borders and cell origins
	{ 0.0, 0.0 },
	{ 4608.0, 0.0 },
	{ 0.0, 4608.0 },
	{ -4608.0, -4608.0 },
	{ 4608.0, 4608.0 },
	{ -147456.0, 0.0 },
	{ 0.0, -147456.0 },
	{ 142848.0, 0.0 }, // 31 * 4608
	{ 0.0, 142848.0 },
	{ -9216.0, 23040.0 },
	{ 138240.0, -138240.0 },
	// interior points that exercise the interpolation weights
	{ 2304.5, 6912.25 },
	{ -1.25, 4607.75 },
	{ 147455.75, -147456.0 },
	{ 72.0, -72.0 },
	{ 90000.125, 90000.5 },
};

std::vector<double> referenceHeights(const std::vector<std::pair<double, double>>& points)
{
	const std::string pointsPath = (fs::temp_directory_path()
		/ ("kworld_terrain_points_" + std::to_string(::getpid()) + ".txt")).string();
	{
		std::ofstream out(pointsPath);
		if (!out)
			throw test::Failure("cannot write " + pointsPath);
		out.precision(17);
		for (const auto& point : points)
			out << point.first << " " << point.second << "\n";
	}

	const std::string command = "python3 -I "
		+ test::shellQuote(test::pythonScript("height_expected.py")) + " "
		+ test::shellQuote(test::refTool("terrain.py")) + " "
		+ test::shellQuote(test::dataPath("newland/land")) + " "
		+ test::shellQuote(pointsPath) + " 2>&1";
	std::string output;
	try
	{
		output = test::runCommand(command);
	}
	catch (...)
	{
		std::error_code ignored;
		fs::remove(pointsPath, ignored);
		throw;
	}
	std::error_code ignored;
	fs::remove(pointsPath, ignored);

	std::vector<double> heights;
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
	{
		if (line.empty())
			continue;
		std::istringstream fields(line);
		std::string x;
		std::string z;
		std::string h;
		if (!std::getline(fields, x, '\t') || !std::getline(fields, z, '\t')
			|| !std::getline(fields, h, '\t'))
			throw test::Failure("unexpected height line: " + line);
		heights.push_back(std::stod(h));
	}
	return heights;
}

} // namespace

int main()
{
	try
	{
		const auto heightmap = kworld::Heightmap::open(
			test::dataPath("newland/land/fullmap.tif"));
		test::require(heightmap.sampleCountX() == 16385
			&& heightmap.sampleCountZ() == 16385, "heightmap is not 16385x16385");

		const auto expected = referenceHeights(kPoints);
		test::require(expected.size() == kPoints.size(),
			"height_expected.py returned " + std::to_string(expected.size())
			+ " lines for 20 points");

		double worst = 0.0;
		for (std::size_t i = 0; i < kPoints.size(); ++i)
		{
			const double actual = heightmap.height(kPoints[i].first, kPoints[i].second);
			const double difference = std::abs(actual - expected[i]);
			worst = std::max(worst, difference);
			test::require(difference < 1e-5,
				"height mismatch at point " + std::to_string(i) + " ("
					+ std::to_string(kPoints[i].first) + ", "
					+ std::to_string(kPoints[i].second) + "): expected "
					+ std::to_string(expected[i]) + ", got " + std::to_string(actual));
		}
		std::cout << "terrain: heightmap " << heightmap.sampleCountX() << "x"
			<< heightmap.sampleCountZ() << "; " << kPoints.size()
			<< " sample heights match terrain.py (max |diff| " << worst << ")\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "terrain: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
