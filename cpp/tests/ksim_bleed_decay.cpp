// Bleeding clots while a character lies KO'd (task AB, BQ11). The live sample (~/git/kenshi/kenshi-inspect/findings/,
// exe oracle: 269 KO'd bandits over 10 bar_crowd runs) has the total rate at the KO at median 0.22 (max 0.44); it
// halves in 28.7 s (p90 39.6), is down to ~0.001 at median 55 s, and 96 of the 269 fell below 0 blood (lowest
// -0.53 x max); none died (death is at blood <= -max).
//
// Ours: each wound loses `bleeding clot rate` per MEDICAL second (CombatRules::updateMedical). The medical clock is
// the exe's update cadence: 1.0 far from the camera (1 update per 50 frames, where the bar_crowd sample was taken)
// up to 2.0 with the camera on the character (MEDICAL_HUNGER_LIVE.md). So the live 28.7 s is the cadence-1.0
// figure of a multi-wound body; a single 0.22 entry clots much more slowly than the same total split in four,
// which is why a KO'd body's rate is set by its wound count as well as its rate.
//
// The profile here is the live 0.22 body: four wounds summing 0.183 x 1.2 = 0.22, the sizes a fight in bar_crowd
// leaves (our instrumented runs: 3..5 wounds of 0.02..0.06, the 0.22 bodies at 5). The watched-duel medicalClock
// is 2.0, and at 2.0 the same body halves in half the time: the clock, not the clot rule, is what separates us
// from that far-camera sample. (Since task BT the default is the original's nominal 1/1.04; both clocks stay pinned.) Checked:
//   - at cadence 1.0 the rate halves in 20-40 s and the blood bottoms out in 40-75 s (live: 28.7 and 55);
//   - the blood stays above -maxBlood, and a 0.22 body never dies of it;
//   - medicalClock 2.0 halves the time to half (the knob is the clock);
//   - the rate reaches 0 and the body then regenerates: an intact negative limb adds no floor (only a severed
//     limb leaks: ksim_limb_leak).
// UNSURE: the exe's decay mechanism is a fit (a linear per-wound clot at the cadence above); the data names
// (`bleeding clot rate`, `bleed rate`) are the likely inputs, not a traced path.

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

state::FighterPart* part(state::Fighter& f, const char* name)
{
	for (state::FighterPart& p : f.parts)
	{
		if (p.name == name)
			return &p;
	}
	throw std::runtime_error(std::string("no part ") + name);
}

void require(bool ok, const char* what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what);
}

// One KO'd body with the given wounds, stepping the medical rules at the sim tick; no order, no enemy, no part
// sinking faster than the wound allows. Times are in sim seconds from the first step.
struct Decay
{
	double rate0 = 0.0;
	double halfAt = -1.0;
	double stopAt = -1.0;
	double bottomAt = -1.0;
	double blood0 = 0.0;
	double bloodLow = 0.0;
	double bloodMax = 0.0;
	double bloodEnd = 0.0;
	double bleedingEnd = 0.0;
};

Decay decay(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, const std::string& sid,
	double clock, const std::vector<double>& wounds, double seconds)
{
	CombatSettings settings;
	settings.medicalClock = clock;
	CombatRules rules(data, settings);
	std::uint64_t rng = 4242;
	state::Fighter f = data->makeFighter(gd, sid, "", rng);
	f.wounds = wounds;
	f.koTimer = 1.0e9; // the KO holds on the timer alone: no order and no enemy is involved
	f.unconscious = true;
	rules.updateMedical(f, ksim::kTickSeconds);
	Decay d;
	d.rate0 = f.bleeding;
	d.blood0 = f.blood;
	d.bloodLow = f.blood;
	d.bloodMax = f.maxBloodRace;
	const std::uint64_t ticks = static_cast<std::uint64_t>(seconds / ksim::kTickSeconds);
	for (std::uint64_t i = 0; i < ticks; ++i)
	{
		rules.updateMedical(f, ksim::kTickSeconds);
		const double t = static_cast<double>(i + 2) * ksim::kTickSeconds;
		if (d.halfAt < 0.0 && f.bleeding <= 0.5 * d.rate0)
			d.halfAt = t;
		if (d.stopAt < 0.0 && d.halfAt >= 0.0 && f.bleeding <= 0.001)
			d.stopAt = t;
		if (f.blood < d.bloodLow)
		{
			d.bloodLow = f.blood;
			d.bottomAt = t;
		}
	}
	d.bleedingEnd = f.bleeding;
	d.bloodEnd = f.blood;
	return d;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* servant = recordNamed(gd, "CHARACTER", "Escaped Servant");
		std::printf("    clot %.5f per wound per medical second (data %.5f x 0.1)\n", data->k.bleedClotRate,
			data->k.bleedRate);

		// The live 0.22 body: four wounds, 1.2 x their sum.
		const std::vector<double> live = {0.030, 0.045, 0.055, 0.053};
		const Decay farCamera = decay(gd, data, servant->stringID, 1.0, live, 150.0);
		const Decay watched = decay(gd, data, servant->stringID, 2.0, live, 150.0);
		std::printf("measure: bleeding %.4f (blood %.1f of %.1f); at cadence 1.0 halves %.1f s, <=0.001 at %.1f s,"
			" blood bottom %.1f at %.1f s, ends bleeding %.4f blood %.1f\n", farCamera.rate0, farCamera.blood0,
			farCamera.bloodMax, farCamera.halfAt, farCamera.stopAt, farCamera.bloodLow, farCamera.bottomAt,
			farCamera.bleedingEnd, farCamera.bloodEnd);
		std::printf("measure: at medicalClock 2.0 (the watched duels) halves %.1f s, <=0.001 at %.1f s, bottom %.1f at %.1f s\n",
			watched.halfAt, watched.stopAt, watched.bloodLow, watched.bottomAt);

		require(std::fabs(farCamera.rate0 - 0.22) < 0.01 && std::fabs(watched.rate0 - 0.22) < 0.01,
			"the four wounds bleed at 0.22");
		require(farCamera.halfAt >= 20.0 && farCamera.halfAt <= 40.0,
			"at the far-camera cadence the rate halves in 20-40 s (live 28.7, p90 39.6)");
		require(farCamera.bottomAt >= 40.0 && farCamera.bottomAt <= 75.0,
			"and the blood bottoms out in 40-75 s (live 55)");
		require(farCamera.bloodLow > -farCamera.bloodMax && watched.bloodLow > -watched.bloodMax,
			"a 0.22 body's blood stays above -maxBlood: it never dies of it");
		require(watched.halfAt > 0.4 * farCamera.halfAt && watched.halfAt < 0.6 * farCamera.halfAt,
			"medicalClock 2.0 halves the time to half: the clock is the knob");
		require(farCamera.bleedingEnd == 0.0 && watched.bleedingEnd == 0.0,
			"the wounds clot away to exactly 0: no floor");
		require(farCamera.bloodEnd > farCamera.bloodLow && watched.bloodEnd > watched.bloodLow,
			"and the body then regenerates");

		// The floor check against an intact limb on this same KO'd body: a limb below 0 that is not severed adds
		// nothing (only a severed limb leaks; ksim_limb_leak covers the sever side).
		{
			CombatSettings settings;
			settings.medicalClock = 1.0;
			CombatRules rules(data, settings);
			std::uint64_t rng = 4242;
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.wounds.clear();
			f.koTimer = 1.0e9;
			f.unconscious = true;
			state::FighterPart* arm = part(f, "Left Arm");
			arm->health = -30.0;
			for (int i = 0; i < 100; ++i)
				rules.updateMedical(f, ksim::kTickSeconds);
			std::printf("measure: Left Arm at -30, not severed: bleeding %.4f, blood %.1f\n", f.bleeding, f.blood);
			require(f.bleeding == 0.0 && f.blood > 0.0, "an intact limb below 0 adds no leak and the body regenerates");
		}
		std::printf("ksim_bleed_decay: pass\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
