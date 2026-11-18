// The pickTarget target lock (kenshi-k16.6.134, rwh.113): the held target stands, unscored, while the +0x2f0 timer is under 1.0 s;
// the timer counts up and only a re-pick resets it. A 3v3 whose held targets drop out of the scan every 0.4 s (a better candidate wins
// only then): a re-pick swap never comes with the timer under 1.0 s, and one does come once it is up. At hold 0 it swaps at once.

#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;

// The lowest +0x2f0 (s) at which a fighter swapped one target for another by a re-pick, in a 3v3 in the open where each fighter's held target drops out of
// its scan every 0.4 s (the lapse stands in for sight lost; the current target's +5 / +10 otherwise beat every other enemy); -1: none in 6 s.
// Other paths (a hit's retaliation, joinFight) swap without a re-pick and leave the timer alone, so only a reset timer counts.
double lowestRepickAge(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, double hold, std::uint64_t seed)
{
	CombatSettings settings;
	settings.retargetHold = hold;
	CombatArena arena(data, settings, seed);
	const kdata::Record* servant = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			servant = r;
	test::require(servant != nullptr, "no Escaped Servant");
	std::uint64_t rng = seed;
	for (int i = 0; i < 6; ++i)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		const int team = i % 2;
		f.x = team == 0 ? -120.0 : 30.0;
		f.z = team == 0 ? 30.0 * (i / 2) - 30.0 : 3.0 * (i / 2);
		f.yaw = team == 0 ? 1.5707963267948966 : -1.5707963267948966;
		f.team = team;
		arena.add(i + 1, std::move(f));
	}
	std::uint64_t last[6] = {};
	double age[6] = {}; // +0x2f0 before the step
	double shortest = -1.0;
	for (int step = 0; step < 120; ++step)
	{
		if (step % 8 == 0)
			for (int i = 1; i <= 6; ++i)
			{
				state::Fighter* f = arena.find(i);
				if (f->target != 0)
				{
					f->aggroBlockId = f->target;
					f->aggroBlock = 2.0 * kDt;
				}
			}
		for (int i = 0; i < 6; ++i)
			age[i] = arena.find(i + 1)->targetAge;
		arena.step(kDt);
		for (int i = 0; i < 6; ++i)
		{
			const state::Fighter* f = arena.find(i + 1);
			// a re-pick (the timer back at 0) that swapped one target for another: the timer it had reached
			if (f->target != last[i] && last[i] != 0 && f->target != 0 && f->targetAge < kDt / 2 && (shortest < 0.0 || age[i] < shortest))
				shortest = age[i];
			last[i] = f->target;
		}
	}
	return shortest;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		test::require(CombatSettings{}.retargetHold == 1.0, "the default hold is not the exe's 1.0 s");
		double locked = -1.0;
		double free = -1.0;
		for (std::uint64_t seed = 11; seed < 14; ++seed)
		{
			const double l = lowestRepickAge(gd, data, 1.0, seed);
			const double z = lowestRepickAge(gd, data, 0.0, seed);
			if (l >= 0.0 && (locked < 0.0 || l < locked))
				locked = l;
			if (z >= 0.0 && (free < 0.0 || z < free))
				free = z;
		}
		std::cout << "ksim_target_lock: lowest timer at a re-pick swap: hold 1.0 -> " << locked << " s, hold 0 -> " << free << " s\n";
		test::require(locked >= 0.0, "hold 1.0 never re-scored");
		test::require(free >= 0.0 && free < 0.9, "hold 0 never re-picked within 0.9 s");
		test::require(locked >= 0.95, "a re-pick at timer " + std::to_string(locked) + " s: under the exe's 1.0 s");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_target_lock: " << e.what() << "\n";
		return 1;
	}
}
