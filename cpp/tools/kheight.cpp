// Terrain height at a world (x, z) pair.
// Usage: kheight <x> <z> [KENSHI_DATA_ROOT]

#include "kworld/heightmap.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

bool parseCoordinate(const char* text, double& value)
{
	char* end = nullptr;
	value = std::strtod(text, &end);
	return end != text && *end == '\0' && std::isfinite(value);
}

} // namespace

int main(int argc, char** argv)
{
	if (argc < 3 || argc > 4)
	{
		std::cerr << "usage: kheight <x> <z> [KENSHI_DATA_ROOT]\n";
		return 2;
	}
	double x = 0.0;
	double z = 0.0;
	if (!parseCoordinate(argv[1], x) || !parseCoordinate(argv[2], z))
	{
		std::cerr << "kheight: coordinates must be finite numbers\n";
		return 2;
	}

	const std::string dataRoot = argc == 4 ? argv[3] : std::string(KENSHI_DATA);
	const std::string path = dataRoot + "/newland/land/fullmap.tif";
	try
	{
		const auto heightmap = kworld::Heightmap::open(path);
		std::cout << std::fixed << std::setprecision(6) << heightmap.height(x, z)
			<< "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "kheight: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
