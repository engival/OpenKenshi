// The leg-sever forced KO and the hunger KO's refill (kinspect dt40_leg_sever_ko.md sections 1-4). A standing victim's leg sever starts the KO
// timer floor3(((max(0, maxBlood - blood) x 0.5(3 - 2.25t) + 40) x (1 - t) x 4)) x U(0,1); an arm sever, a leg sever of a victim already down
// or out, set none; an active hunger KO refills the timer to lerp(t, 60, 10) x U(0.25, 2) whenever it runs down to 1.

#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

state::FighterPart* part(state::Fighter& f, const char* name)
{
	for (state::FighterPart& p : f.parts)
		if (p.name == name)
			return &p;
	throw std::runtime_error(std::string("no part ") + name);
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* bandit = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Dust Bandit")
				bandit = r;
		test::require(bandit != nullptr, "no Dust Bandit");
		CombatSettings settings;
		settings.medicalClock = 1.0;
		const CombatRules rules(data, settings);
		std::uint64_t rng = 7;
		const auto make = [&]()
		{
			state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
			f.skills.toughness = 5.0; // t = 0.05
			f.rng = (rng += 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull; // the fighters' seeds are consecutive small numbers: mix them
			f.hunger = 3.0;
			return f;
		};
		const double t = 0.05;

		// A healthy standing victim: the timer is in (0, 160(1 - t)], the mean about half of that.
		double sum = 0.0;
		double biggest = 0.0;
		const int samples = 400;
		for (int i = 0; i < samples; ++i)
		{
			state::Fighter f = make();
			f.blood = rules.maxBlood(f);
			test::require(rules.severNow(f, *part(f, "Left Leg")), "the leg did not sever");
			test::require(f.koTimer > 0.0 && f.koTimer <= 160.0 * (1.0 - t) + 1e-9, "a healthy victim's timer left (0, 160(1 - t)]: " + std::to_string(f.koTimer));
			sum += f.koTimer;
			biggest = std::max(biggest, f.koTimer);
		}
		const double mean = sum / samples;
		test::require(mean > 60.0 && mean < 92.0, "the healthy mean is not about 76: " + std::to_string(mean));
		std::cout << "healthy: mean " << mean << ", max " << biggest << " (bound " << 160.0 * (1.0 - t) << ")\n";

		// A blood deficit lengthens it: D x (1.5 - 1.125t) inside the bracket, before the sever's own loss.
		double wounded = 0.0;
		for (int i = 0; i < samples; ++i)
		{
			state::Fighter f = make();
			f.blood = rules.maxBlood(f) - 60.0;
			rules.severNow(f, *part(f, "Right Leg"));
			const double bound = (60.0 * 0.5 * (3.0 - 2.25 * t) + 40.0) * (1.0 - t) * 4.0;
			test::require(f.koTimer > 0.0 && f.koTimer <= bound + 1e-9, "a wounded victim's timer left its bound " + std::to_string(f.koTimer));
			wounded = std::max(wounded, f.koTimer);
		}
		test::require(wounded > 160.0 * (1.0 - t), "the blood deficit did not lengthen the timer");

		// An arm sever, a victim already down or out: no timer.
		{
			state::Fighter f = make();
			test::require(rules.severNow(f, *part(f, "Left Arm")) && f.koTimer == 0.0, "an arm sever set the KO timer");
			state::Fighter lying = make();
			lying.down = true;
			rules.severNow(lying, *part(lying, "Left Leg"));
			test::require(lying.koTimer == 0.0, "a leg sever of a victim that is down set the timer");
			state::Fighter out = make();
			out.unconscious = true;
			out.koTimer = 12.0;
			rules.severNow(out, *part(out, "Left Leg"));
			test::require(out.koTimer == 12.0, "a leg sever of an unconscious victim rewrote the timer");
		}

		// The timer counts down while the victim is out and the save carries it (koTimer is a Fighter field the archive already writes).
		{
			state::Fighter f = make();
			f.blood = rules.maxBlood(f);
			rules.severNow(f, *part(f, "Left Leg"));
			const double start = f.koTimer;
			rules.updateMedical(f, 1.0);
			rules.updateMedical(f, 1.0);
			test::require(f.unconscious && f.koTimer < start, "the forced KO did not run down");
		}

		// The hunger KO refills: once below 0.1 the window renews at every expiry, so the character never wakes while it stays starved.
		{
			state::Fighter f = make();
			f.hunger = 0.05;
			double lowest = 1e9;
			for (int i = 0; i < 600; ++i)
			{
				rules.updateMedical(f, 1.0);
				test::require(f.unconscious && !f.dead, "the starved character woke or died");
				lowest = std::min(lowest, f.koTimer);
			}
			test::require(lowest > 0.0, "the starved character's timer ran out without a refill");
			std::cout << "hunger: 600 steps, the timer never ran out (lowest " << lowest << ")\n";
		}
		std::cout << "ksim_leg_sever_ko: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_leg_sever_ko: FAILED: " << e.what() << "\n";
		return 1;
	}
}
