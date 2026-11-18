// kenshi-k16.7.89 tool (not a ctest): for points read from stdin ("label x y z" per line) print the xz distance to the nearest furniture footprint (the set
// the bake's ring uses) and whether the point is inside that footprint's ring (footprint + kFurnitureBodyClearance + half a cell, inside the furniture's height
// window). Used to classify ksim_height_sweep cells, ksim_route_walk legs and ksim_nav_islands regions against the ring.
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "prop_clip.hpp"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

int main()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	prop_clip::PropClip clip(gd);
	std::string line;
	while (std::getline(std::cin, line))
	{
		std::istringstream in(line);
		std::string label;
		double x = 0, y = 0, z = 0;
		if (!(in >> label >> x >> y >> z))
			continue;
		bool inRing = false;
		double distance = 0.0;
		const std::string nearest = clip.ring(x, y, z, inRing, distance);
		std::printf("%s\t%.1f %.1f %.1f\tdist %.1f\t%s\t%s\n", label.c_str(), x, y, z, distance, inRing ? "IN-RING" : "outside", nearest.empty() ? "no furniture within 64 u" : nearest.c_str());
	}
	return 0;
}
