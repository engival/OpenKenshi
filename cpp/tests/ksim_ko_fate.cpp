// The fate of an untreated KO'd character against kinspect BQ7 (~/git/kenshi/kenshi-inspect/findings/bq7_ko_fate_live.md; docs/reference/
// BAR_FIGHT_LOOP.md finding 0d). Drives CombatRules::updateMedical on a Dust Bandit with medicalClock 1.0, so one step of
// dt is dt medical units. The live logs count in KO-timer units, which fall 116-134 per game hour, so "per game hour" below
// means 125 units. Checked: blood regenerates while KO'd; a shallow KO wakes and is re-KO'd without a hit and dies; a deep KO
// never wakes and dies; a vital part above 0 sinks at 1/4 of the negative rate.

#include "ksim/combat.hpp"
#include "ksim/navigator.hpp"
#include "ksim/perception.hpp"
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

constexpr double kUnitsPerHour = 125.0; // live: the KO timer falls 116-134 per game hour

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

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const CombatConstants& k = data->k;
		const kdata::Record* bandit = recordNamed(gd, "CHARACTER", "Dust Bandit");
		CombatSettings settings;
		settings.medicalClock = 1.0;
		const CombatRules rules(data, settings);
		std::uint64_t rng = 11;
		const auto make = [&]()
		{
			state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
			f.skills.toughness = 5.0; // the join records: toughness 3-7
			f.hunger = 3.0;
			return f;
		};
		const double koPoint = -(k.minToughnessKoPoint + (k.maxToughnessKoPoint - k.minToughnessKoPoint) * 0.05);

		// ---- 1. blood regenerates while KO'd (0.04 per medical unit x the race factor; live about 3 per game hour) ----
		{
			state::Fighter f = make();
			f.unconscious = true;
			f.koTimer = 1.0e6;
			f.blood = 20.0;
			const double b0 = f.blood;
			for (int i = 0; i < 1250; ++i)
				rules.updateMedical(f, 1.0);
			const double perHour = (f.blood - b0) / 10.0;
			std::printf("    blood +%.2f per 125 units (factor %.3f, rate %.3f)\n", perHour, f.bloodFactor, k.bloodRecoveryRate);
			require(perHour > 0.0 && std::fabs(perHour - k.bloodRecoveryRate * f.bloodFactor * kUnitsPerHour) < 1e-6,
				"blood regenerates while KO'd at bloodRecoveryRate x race factor per medical unit");
			require(f.unconscious, "KO'd throughout");
		}

		// ---- 3. a vital part above 0 sinks at 1/4 of the negative rate ----
		{
			state::Fighter a = make();
			state::Fighter b = make();
			state::FighterPart* pa = part(a, "Chest");
			state::FighterPart* pb = part(b, "Chest");
			pa->health = -10.0;
			pb->health = 0.5; // above 0 but over 90 % damaged, so the damage ramp is at its cap on both bodies
			a.unconscious = b.unconscious = true;
			a.koTimer = b.koTimer = 1.0e6;
			const double a0 = pa->health;
			const double b0 = pb->health;
			rules.updateMedical(a, 1.0);
			rules.updateMedical(b, 1.0);
			const double negRate = a0 - pa->health;
			const double posRate = b0 - pb->health;
			std::printf("    sink negative %.4f, positive %.4f per unit\n", negRate, posRate);
			require(std::fabs(negRate - 4.0 * posRate) < 1e-9, "a vital part above 0 sinks at 1/4 of the negative rate");
			require(negRate > 0.139 && negRate < 0.148, "negative sink 0.139-0.148 per medical unit (live 0.139-0.146)");
		}

		// ---- 2a. deep KO: never wakes, dies 3-5.2 h later (live 3.0-5.2 h) ----
		for (int deep = 0; deep < 2; ++deep)
		{
			state::Fighter f = make();
			part(f, "Chest")->health = koPoint - 1.0 - deep;
			f.unconscious = true;
			f.koTimer = 300.0;
			int units = 0;
			bool woke = false;
			while (!f.dead && units < 20000)
			{
				rules.updateMedical(f, 1.0);
				++units;
				if (!f.unconscious)
					woke = true;
			}
			const double hours = units / kUnitsPerHour;
			std::printf("    deep KO (chest %.1f, KO point %.1f): dead after %.2f h\n", koPoint - 1.0 - deep, koPoint, hours);
			require(!woke && f.dead, "deep KO never wakes and dies");
			require(hours > 3.0 && hours < 5.4, "deep KO death 3.0-5.2 game h after the KO");
		}

		// ---- 2b. shallow KO: wakes when the timer is out, sinks, is re-KO'd without a hit, dies 5-13 h after the first KO ----
		{
			state::Fighter f = make();
			// A vital part still above 0 (live: "H100 C100 S66" and similar; a bad vital above 0, the KO came from the hit's timer).
			// Below 0 it would sink 4x faster and cross the KO point inside the KO timer, which is a deep KO in effect.
			part(f, "Stomach")->health = 15.0;
			f.unconscious = true;
			f.koTimer = 300.0;
			f.blood = 100.0;
			int units = 0;
			int wakes = 0;
			int reKos = 0;
			bool prev = true;
			double firstWake = 0.0;
			while (!f.dead && units < 40000)
			{
				rules.updateMedical(f, 1.0);
				++units;
				if (prev && !f.unconscious)
				{
					++wakes;
					if (wakes == 1)
						firstWake = units / kUnitsPerHour;
				}
				if (!prev && f.unconscious)
					++reKos;
				prev = f.unconscious;
			}
			const double hours = units / kUnitsPerHour;
			std::printf("    shallow KO: woke %.2f h, %d wake(s), %d re-KO(s), dead after %.2f h\n", firstWake, wakes, reKos, hours);
			require(wakes >= 1 && reKos >= 1, "shallow KO wakes when the timer is out, then is re-KO'd with no hit");
			require(f.dead && hours > 5.0 && hours < 13.0, "untreated shallow KO dies 5-13 game h after the first KO");
		}
		// ---- 4. the Knockout event: once per KO transition, attacker 0 for a KO with no hit ----
		{
			CombatArena arena(data, settings, 5);
			state::Fighter a = make();
			state::Fighter b = make();
			a.x = 0.0;
			b.x = 1.0;
			arena.add(1, a);
			arena.add(2, b);
			std::vector<CombatEvent> kos;
			arena.setObserver([&](const CombatEvent& e)
			{
				if (e.kind == CombatEvent::Kind::Knockout)
					kos.push_back(e);
			});
			// medical-only: a vital part past the KO point, no hit
			part(*arena.find(1), "Chest")->health = koPoint - 5.0;
			for (int i = 0; i < 20; ++i)
				arena.step(0.05);
			require(arena.find(1)->unconscious && kos.size() == 1 && kos[0].who == 1 && kos[0].other == 0,
				"a medical-only KO emits exactly one Knockout with attacker 0");
			for (int i = 0; i < 200; ++i)
				arena.step(0.05);
			require(kos.size() == 1, "a body staying down emits no second Knockout");
			// re-KO after waking: back up (as if healed), then down again
			state::Fighter& up = *arena.find(1);
			part(up, "Chest")->health = 50.0;
			up.koLatch = false;
			up.koTimer = 0.0;
			arena.step(0.05);
			require(!up.unconscious, "the body woke");
			part(up, "Chest")->health = koPoint - 5.0;
			arena.step(0.05);
			require(up.unconscious && kos.size() == 2 && kos[1].who == 1 && kos[1].other == 0, "a re-KO after waking emits again, attacker 0");
		}
		// ---- 5. the KO duration measured live (kinspect findings/ko_duration.md, task BA) ----
		// Bark-like: toughness 6.7. The install's constants are the live ones (knockout time base 20, knockout mult 3 / 0.75,
		// min/max toughness ko point 10 / 85), so f = lerp(0.067, 3, 0.75) = 2.84925, g = 0.25 f, and the KO point is
		// -(10 + 75 x 0.067) = -15.04.
		{
			state::Fighter bark = make();
			bark.skills.toughness = 6.7;
			bark.blood = rules.maxBlood(bark);
			for (state::FighterPart& p : bark.parts)
			{
				p.wear = 0.0;
				p.stun = 0.0;
				p.maxHealth = std::max(p.maxHealth, 100.0);
				p.health = p.maxHealth;
			}
			state::FighterPart* head = part(bark, "Head");
			head->maxHealth = 100.0;
			const auto timer = [&](double stun)
			{
				head->stun = stun;
				return rules.knockoutTime(bark);
			};
			const double koPoint = rules.koPoint(bark);
			std::printf("    toughness 6.7: KO point %.3f, T(108) %.2f, T(115.04) %.2f, T(118) %.2f, T(125) %.2f\n",
				koPoint, timer(108.0), timer(115.04), timer(118.0), timer(125.0));
			require(std::fabs(koPoint + 15.044) < 0.02, "the live KO point -15.044 at toughness 6.7");
			require(std::fabs(timer(108.0) + 11.34) < 0.05, "live: head stun 108 gives T -11.34");
			require(std::fabs(timer(115.04) - 23.8) < 0.05, "live: head stun 115.04 gives T +23.8 (corrected)");
			require(std::fabs(timer(118.0) - 38.5) < 0.1, "live: head stun 118 gives T +38.5");
			require(std::fabs(timer(125.0) - 73.4) < 0.1, "live: head stun 125 gives T +73.4 (corrected)");
			// Both sums are the vitals': a limb's stun and damage leave the timer alone (the limbs' KO mult is 0.5 and the live
			// sum skips them).
			state::FighterPart* arm = nullptr;
			for (state::FighterPart& p : bark.parts)
			{
				if (!p.vital && !p.severed())
				{
					arm = &p;
					break;
				}
			}
			require(arm != nullptr, "no non-vital part");
			const double before = timer(110.0);
			arm->stun = 100.0;
			arm->health = -60.0;
			require(std::fabs(rules.knockoutTime(bark) - before) < 1e-12, "a limb's stun and damage do not lengthen the timer");
			arm->stun = 0.0;
			arm->health = arm->maxHealth;

			// Live 1: head stun 108 (v = -8, above the KO point), T -11.34: woke on the next medical update, no latch.
			{
				state::Fighter f = bark;
				head = part(f, "Head");
				head->stun = 108.0;
				f.koTimer = rules.knockoutTime(f);
				f.unconscious = true;
				require(f.koTimer < 0.0, "live 1: the timer is negative");
				rules.updateMedical(f, 1.0);
				require(!f.unconscious && !f.koLatch, "live 1: a negative timer wakes at the next medical update, without a latch");
			}
			// Live 2: sub-latch, timer 50 at speed 1: woke exactly at timer 0 (stun above health, but not past the KO point).
			{
				state::Fighter f = bark;
				head = part(f, "Head");
				head->stun = 110.0; // v = -10, above -15.04
				f.unconscious = true;
				f.koLatch = false;
				f.koTimer = 50.0;
				int units = 0;
				while (f.unconscious && units < 1000)
				{
					rules.updateMedical(f, 1.0);
					++units;
				}
				std::printf("    live 2: woke after %d units (timer 50)\n", units);
				require(units == 50 && !f.koLatch, "live 2: without the latch the KO ends at timer 0");
			}
			// Live 3: latched (head stun 118, v = -18), T 38.5: the timer ran out, the body stayed down until the stun decayed
			// to the health (v >= 0) - the wait is (s - h) / decay. 9.65 game-hours live implies a healing rate 1.19 (decay
			// 0.0149 per unit against the nominal 0.0125).
			{
				state::Fighter f = bark;
				f.healFactor = 1.19;
				head = part(f, "Head");
				head->stun = 118.0;
				f.unconscious = true;
				f.koLatch = false;
				f.koTimer = rules.knockoutTime(f);
				const double decay = 0.05 * k.stunRecoveryRate * k.healRateMult * (head->maxHealth / 100.0) * f.healFactor;
				const int expected = static_cast<int>(std::lround(18.0 / decay));
				int units = 0;
				int timerOut = -1;
				bool latched = false;
				while (f.unconscious && units < 5000)
				{
					rules.updateMedical(f, 1.0);
					++units;
					latched = latched || f.koLatch;
					if (timerOut < 0 && f.koTimer <= 0.0)
						timerOut = units;
				}
				std::printf("    live 3: timer out at %d units, woke at %d (expected %d, %.2f game h at 125 units/h), stun %.2f\n",
					timerOut, units, expected, units / kUnitsPerHour, head->stun);
				require(latched, "live 3: the latch was set");
				require(timerOut > 0 && timerOut < units, "live 3: the latch held past the timer");
				require(units - expected >= -1 && units - expected <= 1, "live 3: the latched wait is (s - h) / decay");
				require(units > 1000 && units < 1350, "live 3: about the live 9.65 game-hours");
			}
			// Live 4: a vital 35.5 past the KO point with no first aid sinks untreated and dies; it never gets up.
			{
				state::Fighter f = bark;
				head = part(f, "Head");
				head->health = rules.koPoint(f) - 35.5;
				head->stun = 0.0;
				f.unconscious = true;
				f.koTimer = 1.0e6;
				int units = 0;
				bool woke = false;
				while (!f.dead && units < 20000)
				{
					rules.updateMedical(f, 1.0);
					++units;
					if (!f.unconscious)
						woke = true;
				}
				const double hours = units / kUnitsPerHour;
				std::printf("    live 4: head %.2f (35.5 past the KO point), dead after %.2f game h (live 3.37), woke %d\n",
					rules.koPoint(f) - 35.5, hours, woke ? 1 : 0);
				require(f.dead && !woke, "live 4: a vital 35.5 past the KO point sinks and dies without waking");
				require(hours > 2.4 && hours < 4.2, "live 4: death by sink near the live 3.37 game h");
			}
		}
		// ---- 6. a fist duel: a timer-only KO (no latch) wakes within T ----
		// argv[1]: the Hub navmesh cache (the ctest fixture knav_cache). The duel runs on the mesh so the two close the ring
		// the way the original's fighters do.
		{
			const std::shared_ptr<MeshNavigator> nav = argc > 1 ? MeshNavigator::load(argv[1]) : nullptr;
			CombatSettings duel = settings;
			duel.ringInner = 6.0; // fists: the default 16 / 26 ring is past the fists' pick reach (TEST setting only)
			duel.ringOuter = 14.0;
			CombatArena arena(data, duel, 17);
			if (nav != nullptr)
				arena.setNavigation(nav);
			const auto spot = nav != nullptr ? nav->walker().snap({-50978.8, 1535.0, 2932.48}, 200.0) : std::nullopt;
			state::Fighter a = make();
			state::Fighter b = make();
			a.weapon = data->unarmed;
			b.weapon = data->unarmed;
			a.skills.unarmed = 40.0; // a brisk brawl: the fists' stun lands (a low-skill pair takes many minutes)
			b.skills.unarmed = 40.0;
			a.skills.strength = 30.0;
			b.skills.strength = 30.0;
			a.team = 1;
			b.team = 2; // different teams are enemies
			if (spot)
			{
				a.x = spot->x;
				a.y = spot->y;
				a.z = spot->z;
				const auto other = nav->walker().snap({spot->x + 12.0, spot->y, spot->z}, 40.0);
				b.x = other ? other->x : spot->x + 12.0;
				b.y = other ? other->y : spot->y;
				b.z = other ? other->z : spot->z;
			} else {
				a.x = 0.0;
				a.z = 0.0;
				b.x = 12.0;
				b.z = 0.0;
			}
			a.yaw = std::atan2(b.x - a.x, b.z - a.z);
			b.yaw = std::atan2(a.x - b.x, a.z - b.z);
			arena.add(1, a);
			arena.add(2, b);
			perception::notice(*arena.find(1), 2);
			perception::notice(*arena.find(2), 1);
			// One fist hit's damage before armour and toughness (both wear nothing): the blunt is the stun the part takes; the
			// live servant's fists took a head to stun 108 over a duel, so a hit of a few points at unarmed 40 is in band.
			{
				double dmg[6];
				rules.computeDamage(a, b, dmg);
				std::printf("    fist hit: cut %.2f blunt %.2f (blunt is the vital's stun) at unarmed 40, strength 30\n",
					dmg[0], dmg[1]);
			}
			std::vector<CombatEvent> kos;
			int starts = 0;
			int hits = 0;
			arena.setObserver([&](const CombatEvent& e)
			{
				if (e.kind == CombatEvent::Kind::Knockout)
					kos.push_back(e);
				else if (e.kind == CombatEvent::Kind::TechniqueStart)
					++starts;
				else if (e.kind == CombatEvent::Kind::Hit)
					++hits;
			});
			int ticks = 0;
			while (kos.empty() && ticks < 20 * 600)
			{
				arena.step(0.05);
				++ticks;
			}
			if (kos.empty())
			{
				const state::Fighter& a2 = *arena.find(1);
				const state::Fighter& b2 = *arena.find(2);
				std::printf("    duel debug: %d starts %d hits, %.1f u apart, states %d / %d\n", starts, hits,
					std::hypot(a2.x - b2.x, a2.z - b2.z), static_cast<int>(a2.state), static_cast<int>(b2.state));
			}
			require(!kos.empty(), "the fist duel knocked somebody out");
			const std::uint64_t who = kos.front().who;
			state::Fighter& kof = *arena.find(who);
			const double t = kof.koLatch ? -1.0 : kof.koTimer;
			// The vital the hit took past 0: its v must be above the KO point (the timer-only case; past it the KoLatch holds).
			const state::FighterPart* koVital = nullptr;
			double deepest = 0.0;
			const double koPoint = rules.koPoint(kof);
			for (const state::FighterPart& p : kof.parts)
			{
				if (p.vital && CombatRules::partEffective(p) < deepest)
				{
					deepest = CombatRules::partEffective(p);
					koVital = &p;
				}
			}
			std::printf("    fist duel: %d ticks (%.1f s), %d starts %d hits, KO T %.1f, %s v %.1f (KO point %.1f), latch %d\n",
				ticks, ticks * 0.05, starts, hits, kof.koTimer, koVital != nullptr ? koVital->name.c_str() : "-", deepest,
				koPoint, kof.koLatch ? 1 : 0);
			if (koVital != nullptr)
			{
				require(!kof.koLatch, "the fist KO is timer-only (no vital past the KO point)");
				require(deepest > koPoint, "the KO'd vital is past 0 but not past the KO point");
			} else {
				// No vital below 0: the first KO is a part collapse's forced KO (task BL, down_ko_trigger fb0248d), whose timer is
				// exactly 3.0 medical units and needs no vital. The wake below still has to come within its own T.
				require(std::fabs(t - CombatRules::kCollapseKoTime) < 0.06, "a collapse KO's timer is its 3.0 medical units (less the step that saw it)");
			}
			require(t > 0.0, "the fist KO has a positive timer");
			int wakeTicks = 0;
			while (kof.unconscious && wakeTicks < 20 * 600)
			{
				arena.step(0.05);
				++wakeTicks;
			}
			const double wokeAt = wakeTicks * 0.05;
			std::printf("    fist duel: woke %.1f s after the KO (T %.1f)\n", wokeAt, t);
			require(!kof.unconscious, "the fist KO wakes");
			require(wokeAt <= t + 1.0, "the fist KO wakes within its own T");
		}
		// The hit-KO side (one event with the attacker, no second one from the medical step) is checked on the armed duels in ksim_combat_armed.
		std::printf("ksim_ko_fate: pass\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
