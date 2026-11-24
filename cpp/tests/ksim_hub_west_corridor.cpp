// kenshi-e9s9.30 (rwh.308): the Hub click A (-49729.2, 4882.8) -> B (-49868.6, 5181.9). The original's corridor goes west round the buildings to
// x -49968; ours must not cut through the one-body gap near (-49896, 4892).
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"

#include "knav/native.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_hub_west_corridor: SKIPPED: no shipped navtiles\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz });
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, hub);
		const auto from = nav->walker().snap({ -49729.2, 0.0, 4882.8 }, 40.0, nullptr, 1.0e6);
		const auto goal = nav->walker().snap({ -49868.6, 0.0, 5181.9 }, 40.0, nullptr, 1.0e6);
		test::require(goal.has_value(), "goal off the mesh");
		test::require(from.has_value(), "start off the mesh");
		double minX = 1.0e9;
		for (const double radius : { 4.0, 0.0 })
		{
			const ksim::Route r = nav->routeFrom(radius == 4.0 ? knav::Position{ -49729.2, 1519.0, 4882.8 } : *from, -49868.6, 5181.9, radius == 4.0 ? std::nullopt : std::optional<double>(goal->y), false, radius, true, radius == 4.0);
			std::cout << "radius " << radius << " found " << r.found << " points " << r.points.size() << " check " << r.check << " from " << from->x << " " << from->y << " " << from->z << "\n";
			for (const auto& p : r.points)
				std::cout << "  " << p.x << " " << p.z << "\n";
			if (radius == 4.0 && r.found)
				for (const auto& p : r.points)
					minX = std::min(minX, p.x);
		}
		test::require(minX < -49950.0, "the route does not go west round the buildings (min x " + std::to_string(minX) + ")");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_hub_west_corridor: " << e.what() << "\n";
		return 1;
	}
}
