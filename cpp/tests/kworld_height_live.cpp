// Height lookup against the original game's own Terrain::getHeight
// (docs/reference/TERRAIN_HEIGHT.md).
//
// 1. tests/data/height_hub_walk.json: 238 [x, z, height] samples taken with the
//    camera near (every terrain patch at full detail, morph factor 1) at
//    walking characters in the Hub. Heightmap::terrainHeight (the 18-unit
//    lattice cut into triangles) must reproduce them to 1e-3.
// 2. tests/data/height_hub_westgate.json: a 65 x 65 grid at 8-unit spacing
//    around (-50757, 2615), measured with the camera far (morph factor 0).
//    There getHeight reads the coarse LOD: a 288-unit triangle lattice whose
//    corners are not all raw samples, up to 45 above fullmap.tif in the Hub
//    bowl. That is a measurement state, not terrain levelling. The grid must
//    be exactly piecewise linear on that lattice (checked per triangle).

#include "kworld/heightmap.hpp"
#include "kworld_test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

struct Point
{
	double x, z, y;
};

std::vector<Point> loadPoints(const std::string& name)
{
	std::ifstream in(std::string(KWORLD_SOURCE_DIR) + "/tests/data/" + name);
	test::require(in.good(), "cannot open fixture " + name);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::vector<Point> points;
	std::size_t at = text.find('[', text.find("\"points\""));
	while ((at = text.find('[', at + 1)) != std::string::npos)
	{
		char* end = nullptr;
		const double x = std::strtod(text.c_str() + at + 1, &end);
		const double z = std::strtod(end + 1, &end);
		const double y = std::strtod(end + 1, &end);
		points.push_back({x, z, y});
		at = static_cast<std::size_t>(end - text.c_str());
	}
	return points;
}

} // namespace

int main()
{
	try
	{
		if (!std::filesystem::exists(std::string(KWORLD_SOURCE_DIR) + "/tests/data/height_hub_walk.json"))
		{
			std::cout << "kworld_height_live: SKIPPED: the captured fixture is not in this tree (game-derived, not shipped)\n";
			return 77;
		}
		const auto terrain = kworld::Heightmap::open(test::dataPath("newland/land/fullmap.tif"));

		// 1. Full detail.
		{
			const auto walk = loadPoints("height_hub_walk.json");
			test::require(walk.size() == 238, "238 points in the walk fixture");
			double worst = 0.0;
			double worstBilinear = 0.0;
			for (const Point& p : walk)
			{
				worst = std::max(worst, std::fabs(terrain.terrainHeight(p.x, p.z) - p.y));
				worstBilinear = std::max(worstBilinear, std::fabs(terrain.height(p.x, p.z) - p.y));
			}
			std::cout << "walk: max |triangle - game| " << worst << " (bilinear " << worstBilinear << ")\n";
			test::require(worst < 1e-3, "terrainHeight differs from Terrain::getHeight at full detail");
		}

			// 2. Coarse LOD.
		{
			const auto grid = loadPoints("height_hub_westgate.json");
			test::require(grid.size() == 65 * 65, "65 x 65 points in the fixture");
			constexpr double cell = 288.0;

			// Per triangle of the lattice, every point must lie on one plane.
			// Points on a cell or diagonal edge belong to two triangles and are skipped.
			std::map<std::array<long, 3>, std::vector<Point>> triangles;
			for (const Point& p : grid)
			{
				const double gx = (p.x + 147456.0) / cell;
				const double gz = (p.z + 147456.0) / cell;
				const double u = gx - std::floor(gx);
				const double v = gz - std::floor(gz);
				if (u < 1e-9 || v < 1e-9 || std::fabs(u + v - 1.0) < 1e-9)
					continue;
				triangles[{static_cast<long>(std::floor(gx)), static_cast<long>(std::floor(gz)), u + v < 1.0 ? 0L : 1L}].push_back(p);
			}
			double worstPlane = 0.0;
			std::size_t checked = 0;
			for (const auto& [key, pts] : triangles)
			{
				bool fitted = false;
				double a = 0, b = 0, c = 0; // y = a + b x + c z
				for (std::size_t i = 1; i < pts.size() && !fitted; ++i)
				{
					for (std::size_t j = i + 1; j < pts.size() && !fitted; ++j)
					{
						const double dx1 = pts[i].x - pts[0].x, dz1 = pts[i].z - pts[0].z, dy1 = pts[i].y - pts[0].y;
						const double dx2 = pts[j].x - pts[0].x, dz2 = pts[j].z - pts[0].z, dy2 = pts[j].y - pts[0].y;
						const double det = dx1 * dz2 - dx2 * dz1;
						if (std::fabs(det) < 1.0)
							continue;
						b = (dy1 * dz2 - dy2 * dz1) / det;
						c = (dx1 * dy2 - dx2 * dy1) / det;
						a = pts[0].y - b * pts[0].x - c * pts[0].z;
						fitted = true;
					}
				}
				if (!fitted)
					continue;
				for (const Point& p : pts)
				{
					worstPlane = std::max(worstPlane, std::fabs(a + b * p.x + c * p.z - p.y));
					++checked;
				}
			}
			std::cout << "coarse grid: " << checked << " points on " << triangles.size()
				<< " triangles, max plane residual " << worstPlane << "\n";
			test::require(checked > 3000, "most grid points belong to a fitted triangle");
			test::require(worstPlane < 1e-2, "the 65 x 65 grid is not piecewise linear on the 288 lattice");
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << "kworld height live: " << error.what() << "\n";
		return 1;
	}
	std::cout << "kworld height live: ok\n";
	return 0;
}
