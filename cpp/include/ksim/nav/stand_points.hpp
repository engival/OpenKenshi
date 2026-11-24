// Stand points of a group move order (kenshi-k16.6.597, user 2026-11-21): a member's goal is moved out until it lies at least both footprints
// (and a hair) from every earlier member's stand point, so arriving needs no settle push. Generic: any squad, any race.
#pragma once

#include <cmath>
#include <utility>
#include <vector>

namespace ksim {

// `goal` is the member's scattered goal, `radius` its footprint; `others` the earlier members' stand points with their footprints.
// `fallback` (radians) splits an exactly coincident pair.
inline std::pair<double, double> standPoint(std::pair<double, double> goal, double radius,
	const std::vector<std::pair<std::pair<double, double>, double>>& others, double fallback)
{
	for (int pass = 0; pass < 6; ++pass) // moving off one neighbour can land on another
	{
		bool moved = false;
		for (const auto& [at, r] : others)
		{
			const double need = radius + r + 0.5;
			double dx = goal.first - at.first;
			double dz = goal.second - at.second;
			const double d = std::hypot(dx, dz);
			if (d >= need)
				continue;
			if (d < 1.0e-6)
			{
				dx = std::sin(fallback);
				dz = std::cos(fallback);
			} else {
				dx /= d;
				dz /= d;
			}
			goal = {at.first + dx * need, at.second + dz * need};
			moved = true;
		}
		if (!moved)
			break;
	}
	return goal;
}

} // namespace ksim
