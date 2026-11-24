// Task ds/stealth-ko S4: the stealth knockout (dt41_C, row 228 Task_StealthKO). A sneaking character behind an unaware victim walks up, plays the
// knockout clip and rolls; success puts the victim down for the forced-KO time and pays Assassination XP, failure is an assault that makes the
// victim notice, and a victim that faces the assassin, or knows it, is not knocked out. The order answers the reasons an order 60 would.

#include "ksim/combat/combat.hpp"
#include "ksim/combat/carry.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/combat/stealth_ko.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

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

void require(bool ok, const std::string& what)
{
	test::require(ok, what);
	std::printf("ok  %s\n", what.c_str());
}

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		if (r->name == name)
			return r;
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

// The XP an addXp of `amount` gives a skill of `skill`.
double gained(double skill, double amount)
{
	const double q = std::pow((101.0 - skill) / 101.0, 2.0);
	return q * amount;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* bandit = recordNamed(gd, "CHARACTER", "Dust Bandit");
		std::uint64_t rng = 9;

		const auto weak = [&](state::Fighter& f)
		{
			f.skills.toughness = 1.0;
			f.skills.defence = 1.0;
			f.skills.thievery = 1.0;
			f.skills.stealth = 1.0;
			f.skills.assassination = 1.0;
		};
		// One arena: the assassin 1 at (0, -150) sneaking, the victim 2 at the origin facing `victimYaw` (0 = away from the assassin).
		std::uint64_t seed = 33; // the arena seed: the roll's draw comes from the assassin's own stream
		std::uint64_t key = 0; // the assassin's dice key: the noise roll hangs on it (perception.cpp), so it decides whether the approach is heard
		const auto arenaWith = [&](double assassination, double victimToughness, double victimYaw)
		{
			auto arena = std::make_unique<CombatArena>(data, CombatSettings{}, seed);
			std::uint64_t same = 9; // the fighters are the same every time (their stats are drawn), so the approach takes the same time
			state::Fighter assassin = data->makeFighter(gd, bandit->stringID, "", same);
			state::Fighter victim = data->makeFighter(gd, bandit->stringID, "", same);
			weak(assassin);
			weak(victim);
			assassin.team = 0;
			victim.team = 1;
			assassin.sneak = true;
			assassin.diceSeed = key;
			assassin.skills.assassination = assassination;
			assassin.skills.stealth = 100.0;
			victim.skills.toughness = victimToughness;
			assassin.x = 0.0;
			assassin.z = -150.0;
			victim.x = 0.0;
			victim.z = 0.0;
			victim.yaw = victimYaw;
			victim.diceSeed = 777 + key; // the victim's scan phase hangs on its key too: the keys cover both the roll and where the scans fall in the approach
			victim.attention = 0.5;
			arena->add(1, std::move(assassin));
			arena->add(2, std::move(victim));
			return arena;
		};

		// ---- the chance (dt41_C section 2: attack - defence, 0 when the victim knows the assassin) ----
		{
			state::Fighter a = data->makeFighter(gd, bandit->stringID, "", rng);
			state::Fighter v = data->makeFighter(gd, bandit->stringID, "", rng);
			weak(a);
			weak(v);
			a.skills.assassination = 60.0;
			require(stealthko::chance(a, v, 1) == 100.0, "a skilled assassin against an untrained victim has a certain roll");
			v.skills.toughness = 60.0;
			const double even = stealthko::chance(a, v, 1);
			require(even > 0.0 && even < 100.0, "a victim as tough as the assassin is skilled gives a middling chance");
			v.skills.toughness = 100.0;
			require(stealthko::chance(a, v, 1) == 0.0, "a far tougher victim gives no chance");
			v.skills.toughness = 1.0;
			perception::notice(v, 1);
			require(stealthko::chance(a, v, 1) == 0.0, "a victim that knows the assassin gives chance 0");
		}

		// ---- the order's answers ----
		{
			auto arena = arenaWith(50.0, 1.0, 0.0);
			require(arena->orderStealthKo(1, 9) == "bad_target" && arena->orderStealthKo(1, 1) == "bad_target", "a missing victim or itself is bad_target");
			arena->find(2)->unconscious = true;
			require(arena->orderStealthKo(1, 2) == "not_allowed", "a KO'd victim is not_allowed");
			arena->find(2)->unconscious = false;
			arena->find(1)->rightArmOk = false;
			arena->find(1)->leftArmOk = false;
			require(arena->orderStealthKo(1, 2) == "not_allowed", "an assassin with no working arm is not_allowed");
			arena->find(1)->rightArmOk = true;
			arena->find(1)->leftArmOk = true;
			require(arena->orderStealthKo(1, 2).empty() && arena->find(1)->stealthKo.active(), "a valid order starts the task");
			arena->cancelStealthKo(1);
			require(!arena->find(1)->stealthKo.active(), "cancelStealthKo stops it");
		}

		// ---- the noise roll is not an abort (rwh.152, k16.6.419: heard only halves the chance). A sneaking assassin within the victim's halved noise
		// range may be heard before it is at 14 u: that key still reaches the roll. A key that is never heard at all is the quiet one the later cases
		// use (a hash of the two dice keys). ----
		std::uint64_t heardKey = 0;
		std::uint64_t quietKey = 0;
		for (std::uint64_t k = 1; k < 200 && (heardKey == 0 || quietKey == 0); ++k)
		{
			key = k;
			auto arena = arenaWith(50.0, 1.0, 0.0);
			arena->orderStealthKo(1, 2);
			bool heard = false;
			for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
			{
				arena->step(0.05);
				heard = heard || perception::aware(*arena->find(2), 1);
			}
			const auto notes = arena->takeStealthKos();
			if (notes.empty())
				continue;
			if (heard && notes[0].result != 2)
				heardKey = heardKey == 0 ? k : heardKey;
			else if (!heard && notes[0].result == 0)
				quietKey = quietKey == 0 ? k : quietKey;
		}
		require(heardKey != 0, "no dice key makes the victim hear the approach and still reach the roll (heard must not abort)");
		require(quietKey != 0, "no dice key lets the assassin through unheard (the approach never works)");
		key = quietKey;

		// ---- success from behind on an unaware victim: KO for the forced-KO time, Assassination XP ----
		// A skill of 1 against an untrained victim is an even match (50 %, kChanceBound): the seed is moved until the roll comes up.
		const auto knockOut = [&](double assassination, double* koTimer, double* xp, double* wakes)
		{
			std::unique_ptr<CombatArena> arena;
			bool ko = false;
			double expected = 0.0;
			for (seed = 33; seed < 80 && !ko; ++seed)
			{
				arena = arenaWith(assassination, 1.0, 0.0);
				require(!perception::aware(*arena->find(2), 1), "the victim starts unaware");
				require(arena->orderStealthKo(1, 2).empty(), "the order starts");
				for (int i = 0; i < 20 * 30 && !ko && arena->find(1)->stealthKo.active(); ++i)
				{
					arena->step(0.05);
					if (arena->find(2)->unconscious)
					{
						ko = true;
						*koTimer = arena->find(2)->koTimer;
						const double missing = std::max(arena->rules().maxBlood(*arena->find(2)) - arena->find(2)->blood, 0.0);
						expected = forcedKoSecondsParam(data->k.knockoutTimeBase, data->k.knockoutMult1, data->k.knockoutMult99, missing,
							arena->find(2)->skills.toughness, assassination * 0.01);
					}
				}
			}
			require(ko, "the sneaking assassin behind an unaware victim did not knock it out");
			require(std::fabs(*koTimer - expected) < 0.3, "the KO is not the forced-KO time of the Assassination skill");
			const auto notes = arena->takeStealthKos();
			require(notes.size() == 1 && notes[0].result == 0 && notes[0].chance > 0.0, "one stealth KO note, a success");
			const auto attacks = arena->takeAttacks();
			require(attacks.size() == 1 && attacks[0].attacker == 1 && attacks[0].victim == 2, "the KO is recorded as an assault");
			for (int i = 0; i < 40; ++i)
				arena->step(0.05);
			require(!arena->find(1)->stealthKo.active(), "the task did not end");
			// The KO ends when its timer runs out: the seconds from the KO to the wake (2 s have passed).
			double since = 2.0;
			while (arena->find(2)->unconscious && since < 2000.0)
			{
				arena->step(0.05);
				since += 0.05;
			}
			*wakes = since;
			*xp = arena->find(1)->skills.assassination - assassination;
			require(std::fabs(*xp - gained(assassination, stealthko::kXpSuccess * stealthko::kXpScale)) < 1.0e-9, "the success XP is 100 x scale");
			return arena;
		};
		double lowKo = 0.0;
		double highKo = 0.0;
		double xp = 0.0;
		double lowWake = 0.0;
		double highWake = 0.0;
		knockOut(1.0, &lowKo, &xp, &lowWake);
		knockOut(60.0, &highKo, &xp, &highWake);
		require(lowWake > lowKo - 1.0 && lowWake < lowKo + 6.0 && highWake > highKo - 1.0 && highWake < highKo + 6.0, // (the wake lags the timer by a few ticks of the medical step)
			"the victim did not wake when the forced-KO time ran out");
		require(std::fabs(lowKo - kForcedKoMinSeconds) < 0.3, "a skill of 1 gets the 3 s minimum KO");
		require(highKo > 10.0 * lowKo, "a skill of 60 gets a far longer KO");
		// ---- failure: the roll fails against a tough victim: an assault, the victim notices and turns on the assassin, 300 XP ----
		{
			auto arena = arenaWith(1.0, 100.0, 0.0);
			arena->orderStealthKo(1, 2);
			for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
				arena->step(0.05);
			const auto notes = arena->takeStealthKos();
			require(notes.size() == 1 && notes[0].result == 1 && notes[0].chance == 0.0, "the roll against a far tougher victim did not fail");
			require(!arena->find(2)->unconscious, "the failed KO knocked the victim out");
			require(perception::aware(*arena->find(2), 1), "the failed KO did not make the victim notice the assassin");
			bool assault = false;
			for (const CombatArena::AttackNote& a : arena->takeAttacks()) // (the victim's own counter-attack follows)
				assault = assault || (a.attacker == 1 && a.victim == 2);
			require(assault, "the failed KO is not an assault");
			require(std::fabs(arena->find(1)->skills.assassination - 1.0 - gained(1.0, stealthko::kXpFailure * stealthko::kXpScale)) < 1.0e-9,
				"the failure XP is 300 x scale");
		}

		// ---- a victim that faces the assassin sees it coming: the task aborts before the roll, no assault, no XP ----
		{
			auto arena = arenaWith(50.0, 1.0, 3.14159265358979323846);
			arena->find(1)->skills.stealth = 1.0; // (an untrained sneaker: a Stealth 100 one fills the victim's meter in over 6 s, longer than the approach)
			arena->orderStealthKo(1, 2);
			for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
				arena->step(0.05);
			const auto notes = arena->takeStealthKos();
			require(notes.size() == 1 && notes[0].result == 2, "the victim facing the assassin did not abort the task");
			require(!arena->find(2)->unconscious && arena->find(1)->skills.assassination == 50.0 && arena->takeAttacks().empty(),
				"the aborted KO knocked out, paid XP or was an assault");
		}
		// ... and one that already knows it.
		{
			auto arena = arenaWith(50.0, 1.0, 0.0);
			perception::notice(*arena->find(2), 1);
			arena->orderStealthKo(1, 2);
			for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
				arena->step(0.05);
			const auto notes = arena->takeStealthKos();
			require(notes.size() == 1 && notes[0].result == 2 && !arena->find(2)->unconscious, "the victim that knew the assassin was knocked out");
		}

		// ---- a NEUTRAL victim (team 0 like the player's: Simulation::worldTeam gives every faction not hostile to the player team 0) that knows the
		// assassin: the abort is relation-blind (rwh.152: the victim's seen_now, 140855830) and silent (rwh.153: no roll, no crime, no XP, no popup, no
		// relation change; the exe's abort is flagImpossible, not the failed-roll branch). k16.6.281: the victim does NOT turn on the assassin. ----
		{
			auto arena = arenaWith(50.0, 1.0, 0.0);
			arena->find(2)->team = arena->find(1)->team;
			perception::notice(*arena->find(2), 1);
			require(stealthko::victimKnows(*arena->find(2), 1), "the neutral victim knows the assassin");
			arena->orderStealthKo(1, 2);
			for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
				arena->step(0.05);
			const auto notes = arena->takeStealthKos();
			require(notes.size() == 1 && notes[0].result == 2 && !arena->find(2)->unconscious, "the aware neutral victim did not abort the task");
			require(arena->takeAttacks().empty() && arena->find(1)->skills.assassination == 50.0, "the aborted neutral KO was an assault or paid XP");
			require(arena->find(2)->target != 1 && arena->find(1)->target != 2, "the aborted neutral KO started a fight");
		}

		// ---- rwh.152: heard is not known. A victim aware only through the noise roll (seen, not knownNow) does not abort the approach and halves the chance;
		// the neutral victim that SAW it (knownNow) aborts (above). ----
		{
			auto arena = arenaWith(50.0, 1.0, 0.0);
			perception::notice(*arena->find(2), 1);
			for (state::SightRecord& r : arena->find(2)->sight)
			{
				r.knownNow = false;
				r.sighted = true;
			}
			require(perception::aware(*arena->find(2), 1) && !stealthko::victimKnows(*arena->find(2), 1), "a heard-only victim is not knowing");
			const double heardChance = stealthko::chance(*arena->find(1), *arena->find(2), 1);
			arena->find(2)->sight.clear();
			const double unheardChance = stealthko::chance(*arena->find(1), *arena->find(2), 1);
			require(std::abs(heardChance * 2.0 - unheardChance) < 1e-9 && unheardChance > 0.0, "a heard-only victim halves the chance");
		}

		// ---- SIMLINK event 50: one CombatEvent::StealthKo per attempt, at the roll (0 KO, 1 failed, chance on it) or at the abort (2, chance 0); who = the
		// victim, other = the assassin ----
		{
			const auto attempt = [&](double victimToughness, bool knows, std::vector<CombatEvent>& events)
			{
				for (seed = 33; seed < 90; ++seed)
				{
					auto arena = arenaWith(100.0, victimToughness, 0.0);
					events.clear();
					arena->setObserver([&](const CombatEvent& e)
					{
						if (e.kind == CombatEvent::Kind::StealthKo)
							events.push_back(e);
					});
					if (knows)
						perception::notice(*arena->find(2), 1);
					arena->orderStealthKo(1, 2);
					for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
						arena->step(0.05);
					if (!events.empty() && (knows || events[0].code == (victimToughness > 1.0 ? 1 : 0)))
						return;
				}
			};
			std::vector<CombatEvent> events;
			attempt(1.0, false, events);
			require(events.size() == 1 && events[0].code == 0 && events[0].who == 2 && events[0].other == 1 && events[0].a > 0.0 && events[0].a <= 100.0,
				"a landed KO sends one StealthKo event, result 0, with its chance");
			attempt(100.0, false, events);
			require(events.size() == 1 && events[0].code == 1 && events[0].who == 2 && events[0].other == 1 && events[0].a > 0.0 && events[0].a < 100.0,
				"a failed roll sends one StealthKo event, result 1");
			attempt(1.0, true, events);
			require(events.size() == 1 && events[0].code == 2 && events[0].who == 2 && events[0].other == 1 && events[0].a == 0.0,
				"an aborted approach sends one StealthKo event, result 2, chance 0");
		}

		// ---- the client's live case (kenshi-k16.6.25): a sneaking assassin 30 u behind a victim that faces away (yaw 0), by day. The approach
		// is heard by the noise roll (p = 1 - d^2 / noise^2, the noise range halved for an awake observer: 225 u at Stealth 1, 40 u at Stealth 100)
		// within the first scans, which aborts the task after about a unit of walking: correct for that skill, not the gait. ----
		{
			struct Row { double stealth; int aborted = 0; int ko = 0; double walked = 0.0; };
			Row rows[3] = {{1.0}, {50.0}, {100.0}};
			for (Row& row : rows)
				for (std::uint64_t k = 1; k <= 100; ++k)
				{
					key = k;
					auto arena = arenaWith(50.0, 1.0, 0.0);
					arena->find(1)->z = -30.0;
					arena->find(1)->skills.stealth = row.stealth;
					arena->orderStealthKo(1, 2);
					for (int i = 0; i < 20 * 30 && arena->find(1)->stealthKo.active(); ++i)
						arena->step(0.05);
					const auto notes = arena->takeStealthKos();
					require(notes.size() == 1, "the 30 u case ends in exactly one stealth KO note");
					if (notes[0].result == 2)
					{
						++row.aborted;
						row.walked += arena->find(1)->z + 30.0;
					} else {
						++row.ko;
					}
				}
			for (const Row& row : rows)
				std::printf("30 u behind, Stealth %3.0f: %d of 100 dice keys abort (mean %.2f u walked), %d reach the roll\n", row.stealth, row.aborted,
					row.aborted > 0 ? row.walked / row.aborted : 0.0, row.ko);
			// rwh.152: the noise roll (heard) no longer aborts, only halves the chance; what still aborts is sight (the sneak meter past 1.0).
			require(rows[0].ko > rows[0].aborted && rows[1].ko > rows[1].aborted && rows[2].ko > rows[2].aborted,
				"a sneaker heard 30 u behind a victim that faces away mostly reaches the roll (heard never aborts)");
		}

		// ---- the Simulation: the order, and EV 0x38 when the roll fails ----
		{
			SpawnOptions options;
			options.seed = 12;
			options.wildernessByDensity = false;
			Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), options, nullptr, RegionConfig{});
			CombatSettings settings;
			settings.substeps = 2;
			sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
			const auto add = [&](double x, double z, std::uint64_t team)
			{
				state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
				weak(f);
				f.team = team;
				f.x = x;
				f.z = z;
				f.y = 0.0;
				return sim.spawnFighter(std::move(f));
			};
			const flecs::entity assassin = add(-3000.0, -3000.0, 0);
			const flecs::entity victim = add(-3000.0, -2850.0, 1);
			const std::uint64_t assassinId = assassin.get<state::Id>()->value;
			const std::uint64_t victimId = victim.get<state::Id>()->value;
			state::Fighter& v = *const_cast<CombatArena*>(sim.combat())->find(victimId);
			v.skills.toughness = 100.0;
			require(sim.orderStealthKo(assassinId, 999999999) == "bad_target", "Simulation::orderStealthKo answers bad_target");
			require(sim.orderStealthKo(assassinId, victimId).empty(), "Simulation::orderStealthKo starts the order");
			for (int n = 0; n < 20 * 30 && sim.combat()->find(assassinId)->stealthKo.active(); ++n)
				sim.tick();
			const std::uint64_t before = sim.dialogueEventCount(0x38);
			require(!sim.combat()->find(victimId)->unconscious, "the far tougher victim was knocked out");
			require(before >= 1 || perception::aware(*sim.combat()->find(victimId), assassinId),
				"the stealth KO ended with neither EV 0x38 (a failed roll) nor the victim hearing it (an abort)");
		}
		// ---- a landed KO alerts nobody else (kenshi-h2v.1.3, live 2026-11-11: the back row of a thrall group went down and the rest did not
		// notice). The victim's squadmate stands beside it, unaware; on a dice key where the approach is not heard and the KO lands, the
		// squadmate is still unaware of the assassin (not in its SEEN_BY set: victimKnows) and has no target afterwards. The assassin is a player
		// character: an NPC assassin's own AI picks the squadmate as its next fight, which notifies it (not the KO's doing). ----
		{
			SpawnOptions options;
			options.seed = 12;
			options.wildernessByDensity = false;
			Simulation sim = Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), options, nullptr, RegionConfig{});
			CombatSettings settings;
			settings.substeps = 2;
			sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
			CombatArena* arena = const_cast<CombatArena*>(sim.combat());
			int qualified = 0;
			for (std::uint64_t key = 1; key <= 40 && qualified == 0; ++key)
			{
				const double x = -3000.0 + 400.0 * static_cast<double>(key); // each trial in a spot of its own
				const auto add = [&](double dx, double dz, std::uint64_t team)
				{
					state::Fighter f = data->makeFighter(gd, bandit->stringID, "", rng);
					weak(f);
					f.team = team;
					f.x = x + dx;
					f.z = -3000.0 + dz;
					f.y = 0.0;
					f.yaw = 0.0; // facing away from the assassin
					f.attention = 0.5;
					return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
				};
				const std::uint64_t assassinId = add(0.0, -150.0, 0);
				const std::uint64_t victimId = add(0.0, 0.0, 1);
				const std::uint64_t mateId = add(40.0, 0.0, 1);
				state::Fighter& assassin = *arena->find(assassinId);
				assassin.sneak = true;
				assassin.playerControlled = true; // a player character: no AI picks its fights
				assassin.diceSeed = key;
				assassin.skills.stealth = 100.0;
				assassin.skills.assassination = 100.0;
				arena->find(victimId)->diceSeed = 777;
				arena->find(mateId)->diceSeed = 778;
				if (!sim.orderStealthKo(assassinId, victimId).empty())
					continue;
				bool mateUnawareBefore = true; // the squadmate's state the tick before the KO landed
				bool landed = false;
				for (int n = 0; n < 20 * 30 && !landed && arena->find(assassinId)->stealthKo.active(); ++n)
				{
					mateUnawareBefore = !stealthko::victimKnows(*arena->find(mateId), assassinId);
					sim.tick();
					landed = arena->find(victimId)->unconscious;
				}
				if (!landed || !mateUnawareBefore)
					continue; // an approach that was heard, a roll that failed, or a squadmate that heard it itself
				++qualified;
				for (int n = 0; n < 20 * 5; ++n)
					sim.tick();
				const state::Fighter& mate = *arena->find(mateId);
				require(!stealthko::victimKnows(mate, assassinId), "a landed KO alerted the victim's squadmate (it knows the assassin)");
				require(mate.target == 0, "a landed KO gave the victim's squadmate a target");
			}
			require(qualified > 0, "no dice key landed a KO with the squadmate unaware before it");
		}
		std::printf("ksim_stealth_ko: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
