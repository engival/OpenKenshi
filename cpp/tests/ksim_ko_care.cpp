// KO'd characters and why they die (docs/reference/BAR_FIGHT_LOOP.md step 5; kinspect findings/s73_static.md section 6,
// ko_death_model.md, item51_ko_causes.md). Three things are checked:
//   1. Nobody strikes a downed character: an enemy next to a KO'd body picks no target and lands no hit on it (the original's
//      attack goals end on isTargetDown, s73 section 6 item 1; nothing in the data executes a KO'd enemy).
//   2. The sink that kills the untreated is the original's own (ko_death_model.md "Recovery": a vital part below 20 % health
//      loses dt x 0.05 x 1.5 x 1.2 x T5, T5 = lerp(toughness, 1.7, 0.03), per medical second, verified live in clog_sink1;
//      death at h - s < -E). The medical clock runs 2 per sim second. So a KO'd body with a vital part past its KO point never
//      wakes, and dies hours of game time later if nobody treats it: this is the original's rule, not a bug.
//   3. Treatment ends the sink: a bandaged body does not die in the same time.

#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

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

double lerp(double t, double a, double b)
{
	return a + (b - a) * t;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const CombatConstants& k = data->k;
		const kdata::Record* servant = recordNamed(gd, "CHARACTER", "Escaped Servant");
		std::uint64_t rng = 7;
		std::printf("constants: degeneration %g / %g, bodypart rate %g, medical clock default %g\n", k.degenerationMult1,
			k.degenerationMult99, k.bodypartDegenerationRate, CombatSettings{}.medicalClock);

		// ---- 1. a downed body is left alone ----
		{
			Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
			sim.enableCombat(data, CombatSettings{});
			const auto spawn = [&](double x, int team)
			{
				state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
				f.x = x;
				f.z = 0.0;
				f.team = team;
				return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
			};
			const std::uint64_t victim = spawn(0.0, 0);
			const std::uint64_t attacker = spawn(25.0, 1);
			const std::uint64_t bystander = spawn(40.0, 1);
			state::Fighter* v = const_cast<state::Fighter*>(sim.combat()->find(victim));
			part(*v, "Chest")->health = -15.0; // a typical vital KO (item51: the trigger hit leaves -13.8)
			v->koTimer = 1.0e6;
			v->unconscious = true;
			std::size_t hits = 0;
			const_cast<CombatArena*>(sim.combat())->setObserver([&](const CombatEvent& e)
			{
				if (e.kind == CombatEvent::Kind::Hit && e.who == victim)
					++hits;
			});
			std::size_t targeted = 0;
			for (int i = 0; i < 20 * 60; ++i) // a minute: enemies 25 and 40 u from a defenceless body
			{
				sim.run(1);
				for (const std::uint64_t id : {attacker, bystander})
					targeted += sim.combat()->find(id)->target == victim ? 1 : 0;
			}
			test::require(hits == 0, "no hit lands on a KO'd body");
			test::require(targeted == 0, "an enemy beside a KO'd body does not target it");
			std::printf("ok  enemies beside a KO'd body: %zu hits, %zu ticks targeting it in a minute\n", hits, targeted);
		}

		// ---- 2. the sink, against the documented formula ----
		const CombatSettings settings;
		CombatRules rules(data, settings);
		for (const double toughness : {1.0, 30.0, 60.0})
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			f.skills.toughness = toughness;
			state::FighterPart* stomach = part(f, "Stomach");
			const double E = stomach->maxHealth - stomach->wear;
			stomach->health = -30.0;
			f.koTimer = 1.0e6;
			f.unconscious = true;
			const double t5 = lerp(std::clamp(toughness * 0.01, 0.0, 1.0), 1.7, 0.03);
			const double perMedSecond = 0.05 * 1.5 * 1.2 * t5; // ko_death_model.md, r = 1 (damage past 90 %), option 1
			const double start = stomach->health;
			rules.updateMedical(f, 1.0);
			const double got = (start - stomach->health) / settings.medicalClock;
			char what[96];
			std::snprintf(what, sizeof what, "sink of a KO'd stomach at toughness %g per medical second", toughness);
			if (std::fabs(got - perMedSecond) > 0.02 * perMedSecond)
			{
				std::printf("FAIL %s: %.5f, documented %.5f\n", what, got, perMedSecond);
				throw test::Failure(what);
			}
			std::printf("ok  %s: %.5f (documented %.5f)\n", what, got, perMedSecond);
			// How long an untreated body lasts: the death line is -E; the clock is the sim second x medicalClock.
			double seconds = 0.0;
			while (!f.dead && seconds < 40.0 * kGameHourSeconds)
			{
				rules.updateMedical(f, 1.0);
				seconds += 1.0;
			}
			const double gameHours = seconds / kGameDaySeconds * 24.0;
			const double expected = (E - 30.0) / (perMedSecond * settings.medicalClock) / kGameDaySeconds * 24.0;
			std::printf("    toughness %g: dead after %.2f game hours (documented sink alone: %.2f)\n", toughness, gameHours, expected);
			test::require(f.dead, "an untreated KO'd body with a vital part at -30 dies");
			test::require(std::fabs(gameHours - expected) < 0.1 * expected + 0.05, "time to death matches the documented sink");
			test::require(gameHours > 1.0, "the sink takes game hours, not minutes: a one-hour observation sees no death");
		}

		// ---- 3. a bandaged body does not sink ----
		{
			state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
			state::FighterPart* stomach = part(f, "Stomach");
			stomach->health = -30.0;
			stomach->pool = stomach->maxHealth - stomach->wear - stomach->health; // the bandage step filled the pool
			f.koTimer = 1.0e6;
			f.unconscious = true;
			for (int i = 0; i < static_cast<int>(6.0 * kGameHourSeconds); ++i) // six game hours
				rules.updateMedical(f, 1.0);
			test::require(!f.dead, "a treated body does not die");
			test::require(stomach->health > -30.0, "the pool heals the stomach");
			std::printf("ok  bandaged: stomach -30 -> %.1f after six game hours, alive\n", stomach->health);
		}
		std::printf("ksim_ko_care: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
