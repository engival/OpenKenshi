// Crowd-fight spacing, measured (kenshi-h2v.26 part 1; no assertion on the numbers beyond sanity: kenshi-2yx / BQ16 owns any tuning).
// 15 melee fighters, two teams (8 and 7), two lines 60 u apart on open ground (a bare arena), fought for 60 s. Every second from 3 s while both
// teams still stand it samples: the distance of each fighter with a target to that target (attacker -> target), the nearest-neighbour distance
// of every standing fighter, and how many attackers each target has. Prints the median / p10 / p90 over all samples and the largest crowd on one
// target. The reference is the user's vanilla shot (findings/fight_spacing/vanilla_good_spacing_tower_lab.jpg).
// Run with KSIM_SPREAD_VERBOSE=1 to print the numbers; the test itself only checks that the fight ran and that nobody stacked.

#include "ksim/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;

double percentile(std::vector<double> v, double p)
{
	std::sort(v.begin(), v.end());
	return v.empty() ? 0.0 : v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5)];
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
		std::vector<double> pair;
		std::vector<double> nearest;
		std::vector<double> crowd; // attackers on each targeted fighter, one entry per target per sample
		std::size_t largest = 0;
		std::size_t samples = 0;
		for (std::uint64_t seed = 5; seed < 9; ++seed)
		{
			CombatArena arena(data, CombatSettings{}, seed);
			std::uint64_t rng = seed;
			std::vector<std::uint64_t> ids;
			for (int i = 0; i < 15; ++i)
			{
				state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
				f.ranged = state::FighterRanged{};
				const int team = i < 8 ? 0 : 1;
				const int index = team == 0 ? i : i - 8;
				f.team = team;
				f.x = team == 0 ? -30.0 : 30.0;
				f.z = (index - 3.5) * 14.0;
				f.yaw = team == 0 ? 1.5707963267948966 : -1.5707963267948966;
				f.playerControlled = false;
				arena.add(static_cast<std::uint64_t>(i + 1), std::move(f));
				ids.push_back(static_cast<std::uint64_t>(i + 1));
			}
			for (int step = 1; step <= 1200; ++step)
			{
				arena.step(kDt);
				if (step < 60 || step % 20 != 0)
					continue;
				int alive[2] = {};
				for (std::uint64_t id : ids)
				{
					const state::Fighter& f = *arena.find(id);
					if (!f.dead && !f.unconscious)
						++alive[f.team];
				}
				if (alive[0] == 0 || alive[1] == 0)
					break;
				++samples;
				std::map<std::uint64_t, int> attackers;
				for (std::uint64_t id : ids)
				{
					const state::Fighter& f = *arena.find(id);
					if (f.dead || f.unconscious)
						continue;
					double near = 1.0e9;
					for (std::uint64_t other : ids)
					{
						const state::Fighter& o = *arena.find(other);
						if (other != id && !o.dead && !o.unconscious)
							near = std::min(near, std::hypot(f.x - o.x, f.z - o.z));
					}
					nearest.push_back(near);
					const state::Fighter* t = f.target != 0 ? arena.find(f.target) : nullptr;
					if (t != nullptr && !t->dead && !t->unconscious)
					{
						pair.push_back(std::hypot(f.x - t->x, f.z - t->z));
						++attackers[f.target];
					}
				}
				for (const auto& [target, n] : attackers)
				{
					crowd.push_back(n);
					largest = std::max<std::size_t>(largest, static_cast<std::size_t>(n));
				}
			}
		}
		test::require(samples >= 40 && pair.size() >= 100, "the fight did not run long enough to sample");
		test::require(percentile(nearest, 0.0) > 1.0, "two fighters stacked (nearest neighbour under 1 u)");
		if (std::getenv("KSIM_SPREAD_VERBOSE") != nullptr)
		{
			std::printf("samples %zu (4 seeds x 15 fighters)\n", samples);
			std::printf("attacker -> target: median %.1f  p10 %.1f  p90 %.1f  (n %zu)\n", percentile(pair, 0.5), percentile(pair, 0.1), percentile(pair, 0.9), pair.size());
			std::printf("nearest neighbour:  median %.1f  p10 %.1f  p90 %.1f  min %.1f\n", percentile(nearest, 0.5), percentile(nearest, 0.1), percentile(nearest, 0.9), percentile(nearest, 0.0));
			std::printf("attackers per target: median %.1f  p90 %.1f  max %zu\n", percentile(crowd, 0.5), percentile(crowd, 0.9), largest);
		}
		std::printf("ksim_fight_spread: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
