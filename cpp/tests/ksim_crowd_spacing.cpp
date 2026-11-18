// Crowd spacing in melee (kinspect bq9_crowd_spacing.md and s74_static.md; BAR_FIGHT_LOOP.md Finding 0e). Pins the rules of the crowd work, on a bare combat
// arena with no navmesh:
//  - the comfort radii and dead zones of CombatSteer's avoidance are rolled per character, deterministically (a pure function of the arena seed
//    and the id), inside the exe's ranges;
//  - a ring-state fighter is pushed away by its neighbours, but KO'd bodies neither push nor are avoided;
//  - the comfort radii re-roll at every melee task start (a new target), not for the same target (BQ12 a);
//  - depenetration is the exe's rule (BQ16, kinspect bq16_spacing_followups.md 2 / 4): every active character pushes itself 0.5 x the overlap with R the
//    footprints' sum unscaled (8 for two humans), in Id order, so one pass resolves 0.75 of it; bystanders are pushed too, sneak pushes x0.02, an exempt
//    (seated) character and a KO'd body are not pushed; fighters dip below R only by what a step moves them (live min 5.95, sb1 state 11);
//  - attack slots (BQ12 d, ENGAGEMENT.md 6): one slot per defender held by one attacker at a time, released at the swing's LAST IMPACT (before the
//    animation ends), and the defender's attackChanceRoll denies at 0.5 for equal skill;
//  - the threat list counts an attacker whose target is me OR who has me in its hit list (buildIncomingThreatList), not the target alone;
//  - an attacker left out of the 5-slot engaged list waits (state 6) instead of circling (ENGAGEMENT.md 4.4).

#include "ksim/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <set>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;

struct Rig
{
	const kdata::GameData& gd;
	std::shared_ptr<const CombatData> data;
	CombatArena arena;
	const kdata::Record* servant = nullptr;
	std::uint64_t rng = 7;
	std::uint64_t nextId = 1;

	Rig(const kdata::GameData& g, std::shared_ptr<const CombatData> d, std::uint64_t seed)
		: gd(g)
		, data(d)
		, arena(d, CombatSettings{}, seed)
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
	}

	// A fighter that picks no fights of its own (a player character) unless `fights`.
	std::uint64_t spawn(double x, double z, int team, bool fights)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
		f.x = x;
		f.z = z;
		f.yaw = 1.5707963267948966;
		f.team = team;
		f.playerControlled = !fights;
		const std::uint64_t id = nextId++;
		arena.add(id, std::move(f));
		return id;
	}

	state::Fighter& me(std::uint64_t id) { return *arena.find(id); }
	void run(int steps)
	{
		for (int i = 0; i < steps; ++i)
			arena.step(kDt);
	}
	double distance(std::uint64_t a, std::uint64_t b) { return std::hypot(me(a).x - me(b).x, me(a).z - me(b).z); }
};

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);

		// ---- comfort radii: per character, deterministic, in the exe's ranges ----
		{
			Rig a(gd, data, 11);
			Rig b(gd, data, 11);
			Rig c(gd, data, 12);
			std::set<double> distinct;
			bool seedMatters = false;
			for (std::uint64_t id = 1; id <= 40; ++id)
			{
				const CombatArena::Comfort x = a.arena.comfort(id);
				const CombatArena::Comfort y = b.arena.comfort(id);
				test::require(x.hostileRadius == y.hostileRadius && x.friendlyRadius == y.friendlyRadius && x.deadMoving == y.deadMoving
					&& x.deadStanding == y.deadStanding, "the same seed and id roll the same comfort radii (deterministic, nothing to save)");
				test::require(x.hostileRadius >= 15.0 && x.hostileRadius < 45.0, "hostile comfort radius in [15, 45)");
				test::require(x.friendlyRadius >= 3.9 && x.friendlyRadius < 33.9 + 1e-9, "friendly comfort radius in [3.9, 33.9)");
				test::require(x.deadMoving >= 3.0 && x.deadMoving < 6.0, "moving dead zone in [3, 6)");
				test::require(x.deadStanding >= 39.0 - 1e-9 && x.deadStanding <= 90.0 && x.deadStanding >= x.deadMoving + 10.0, "standing dead zone in [39, 66) clamped to [dead + 10, 90]");
				distinct.insert(x.hostileRadius);
				if (c.arena.comfort(id).hostileRadius != x.hostileRadius)
					seedMatters = true;
			}
			test::require(distinct.size() == 40, "every character has its own comfort radii");
			test::require(seedMatters, "another arena seed rolls other radii");
			std::printf("ok  comfort radii: 40 characters, distinct, deterministic, in range (e.g. id 1: %.1f / %.1f / %.2f / %.1f)\n",
				a.arena.comfort(1).hostileRadius, a.arena.comfort(1).friendlyRadius, a.arena.comfort(1).deadMoving, a.arena.comfort(1).deadStanding);
		}

		// ---- KO'd bodies neither push nor are avoided; live neighbours do push ----
		{
			double lateral[3] = {}; // standing neighbours, KO'd neighbours, none (the baseline: the fight itself moves the fighter sideways a little)
			for (int ko = 0; ko < 3; ++ko)
			{
				Rig rig(gd, data, 5);
				const std::uint64_t target = rig.spawn(60.0, 0.0, 0, false);
				const std::uint64_t walker = rig.spawn(0.0, 0.0, 1, true);
				rig.me(walker).orderTarget = target;
				for (int i = 0; i < (ko == 2 ? 0 : 5); ++i)
				{
					const std::uint64_t friendId = rig.spawn(0.2 * i, 3.0 + 0.2 * i, 1, false); // five friends close on its +z side
					rig.me(friendId).unconscious = ko == 1;
					rig.me(friendId).koTimer = ko == 1 ? 1.0e6 : 0.0; // the medical step recomputes `unconscious` from the timer
				}
				rig.run(40);
				lateral[ko] = rig.me(walker).z;
			}
			test::require(lateral[0] < lateral[2] - 1.0, "five standing friends on its +z side push the ring fighter towards -z");
			test::require(std::fabs(lateral[1] - lateral[2]) < 1.0e-9, "the same five KO'd bodies do not push it (neither avoided nor blocking), drift " + std::to_string(lateral[1]));
			std::printf("ok  avoidance: lateral drift %.2f u with standing neighbours, %.3f u with KO'd bodies, %.3f u alone\n", lateral[0], lateral[1], lateral[2]);
		}

		// ---- depenetration: one pass in Id order, bystanders included, posture and exemptions ----
		{
			// Two idle humans 1 u apart: the first pushes itself 0.5 x 7, the second 0.5 of what is left, so 0.75 of the overlap goes in one step.
			Rig idle(gd, data, 5);
			const std::uint64_t p = idle.spawn(0.0, 0.0, 0, false);
			const std::uint64_t q = idle.spawn(1.0, 0.0, 0, false);
			const double radius = 2.0 * idle.data->race(idle.me(p).raceSid)->footprintRadius; // R = r_s + r_n, 4 + 4 for humans (rebirth.mod), unscaled
			idle.run(1);
			const double first = idle.distance(p, q);
			test::require(std::fabs(first - (radius - 0.25 * (radius - 1.0))) < 1.0e-6, "one pass resolves 0.75 of the overlap (got " + std::to_string(first) + ", R " + std::to_string(radius) + ")");
			idle.run(10);
			test::require(idle.distance(p, q) >= radius - 0.04 - 1.0e-9, "idle bystanders are pushed apart to R (less the overlap whose half-push, under 0.02, is dropped), got " + std::to_string(idle.distance(p, q)));

			// Sneaking: the pusher's own push is x0.02 (posture != 0); the other one pushes in full.
			Rig sneak(gd, data, 5);
			const std::uint64_t s0 = sneak.spawn(0.0, 0.0, 0, false);
			const std::uint64_t s1 = sneak.spawn(1.0, 0.0, 0, false);
			sneak.me(s0).sneak = true;
			const double s0x = sneak.me(s0).x;
			sneak.run(1);
			test::require(std::fabs((s0x - sneak.me(s0).x) - 0.02 * 0.5 * (radius - 1.0)) < 1.0e-6, "a sneaking pusher moves 0.02 x its push");
			test::require(std::fabs(sneak.distance(s0, s1) - (1.0 + 0.01 * (radius - 1.0) + 0.5 * (radius - 1.0 - 0.01 * (radius - 1.0)))) < 1.0e-6, "the standing one still pushes in full");

			// Exempt (seated, operating): neither pushes nor is pushed.
			Rig seated(gd, data, 5);
			const std::uint64_t e0 = seated.spawn(0.0, 0.0, 0, false);
			const std::uint64_t e1 = seated.spawn(1.0, 0.0, 0, false);
			seated.arena.setPushExempt({e0});
			seated.run(5);
			test::require(seated.me(e0).x == 0.0 && seated.me(e0).z == 0.0 && seated.me(e1).x == 1.0, "an exempt character is out of the push, both ways");

			// A fight: a KO'd body is not pushed, and the pair dips below R only by what a step moves them (the push runs before the velocity step).
			Rig rig(gd, data, 5);
			const std::uint64_t x = rig.spawn(0.0, 0.0, 1, true);
			const std::uint64_t y = rig.spawn(1.0, 0.0, 0, true);
			const std::uint64_t body = rig.spawn(-1.5, 0.5, 0, true);
			rig.me(body).unconscious = true;
			rig.me(body).koTimer = 1.0e6;
			rig.me(x).orderTarget = y;
			rig.me(y).orderTarget = x;
			const double bodyX = rig.me(body).x;
			const double bodyZ = rig.me(body).z;
			double worst = 1.0e9;
			double worstSlack = 1.0e9; // distance - (R - the two fighters' moves this step)
			for (int i = 0; i < 200; ++i)
			{
				const double x0 = rig.me(x).x, z0 = rig.me(x).z, y0 = rig.me(y).x, w0 = rig.me(y).z;
				rig.run(1);
				if (i < 2)
					continue;
				const double moved = std::hypot(rig.me(x).x - x0, rig.me(x).z - z0) + std::hypot(rig.me(y).x - y0, rig.me(y).z - w0);
				worst = std::min(worst, rig.distance(x, y));
				worstSlack = std::min(worstSlack, rig.distance(x, y) - (radius - 0.04 - moved));
			}
			test::require(rig.me(body).x == bodyX && rig.me(body).z == bodyZ, "a KO'd body is not pushed");
			test::require(worstSlack >= -1.0e-6, "a fighter pair is never closer than R less the step's own moves (slack " + std::to_string(worstSlack) + ")");
			test::require(worst >= 5.95, "fighters stay above the live minimum 5.95 (sb1 state 11), worst " + std::to_string(worst));
			std::printf("ok  depenetration: 0.75 per pass (%.3f), sneak x0.02, exempt, fight worst %.2f (R %.2f, live min 5.95)\n", first, worst, radius);
		}

		// ---- the threat list: target OR hit list ----
		{
			Rig rig(gd, data, 5);
			const std::uint64_t attacker = rig.spawn(0.0, 0.0, 1, true);
			const std::uint64_t victim = rig.spawn(20.0, 0.0, 0, true);
			const std::uint64_t bystander = rig.spawn(20.0, 4.0, 0, true);
			state::Fighter& a = rig.me(attacker);
			a.state = state::CombatState::Attack;
			a.swingProgress = 0.6;
			a.target = victim;
			test::require(rig.arena.threatsOf(victim) == std::vector<std::uint64_t>{attacker}, "an attacker mid-swing at me is a threat");
			test::require(rig.arena.threatsOf(bystander).empty(), "an attacker whose target is another and whose hit list is empty is not");
			rig.arena.membersMutable()[0].fighter.hitList.push_back(bystander); // an impact of this swing already reached the bystander
			test::require(rig.arena.threatsOf(bystander) == std::vector<std::uint64_t>{attacker}, "an attacker with me in its hit list is a threat although it aims at another");
			a.swingProgress = 1.0;
			test::require(rig.arena.threatsOf(bystander).empty() && rig.arena.threatsOf(victim).empty(), "a finished swing (progress 1) is no threat");
			std::printf("ok  threats: target, hit list, finished swing\n");
		}

		// ---- an attacker the engaged list has no room for waits in state 6; it does not circle ----
		{
			Rig rig(gd, data, 5);
			const std::uint64_t victim = rig.spawn(0.0, 0.0, 0, false);
			std::vector<std::uint64_t> crowd;
			for (int i = 0; i < 9; ++i)
			{
				const double angle = 0.7 * i;
				const std::uint64_t id = rig.spawn(60.0 * std::cos(angle), 60.0 * std::sin(angle), 1, true);
				rig.me(id).orderTarget = victim;
				crowd.push_back(id);
			}
			int waiting = 0;
			int circling = 0;
			for (int i = 0; i < 20 * 20; ++i)
			{
				rig.run(1);
				if (i < 20 * 8)
					continue;
				for (const std::uint64_t id : crowd)
				{
					waiting += rig.me(id).state == state::CombatState::Wait ? 1 : 0;
					circling += rig.me(id).state == state::CombatState::Circle ? 1 : 0;
				}
			}
			test::require(waiting > 0, "some of nine attackers wait in state 6 (the engaged list holds 5)");
			test::require(waiting > circling, "the waiters outnumber the circlers (original: 57 % waiting, 16 % circling)");
			std::printf("ok  waiting: %d waiting vs %d circling fighter-samples among nine attackers on one victim\n", waiting, circling);
		}

		// ---- the comfort radii re-roll at every melee task start (BQ12 a) ----
		{
			Rig rig(gd, data, 5);
			test::require(rig.arena.comfort(1, 0).hostileRadius == rig.arena.comfort(1, 0).hostileRadius
				&& rig.arena.comfort(1, 0).hostileRadius != rig.arena.comfort(1, 1).hostileRadius
				&& rig.arena.comfort(1, 1).hostileRadius != rig.arena.comfort(1, 2).hostileRadius, "the same id rolls other radii for another task-start count, the same ones for the same");
			const std::uint64_t a = rig.spawn(0.0, 0.0, 1, true);
			const std::uint64_t t1 = rig.spawn(40.0, 0.0, 0, false);
			const std::uint64_t t2 = rig.spawn(-40.0, 0.0, 0, false);
			rig.me(a).orderTarget = t1;
			rig.run(20);
			const double first = rig.me(a).comfortRolls;
			test::require(first >= 1.0, "the first target is a melee task start");
			rig.run(40);
			test::require(rig.me(a).target == t1 && rig.me(a).comfortRolls == first, "the same target does not re-roll");
			rig.me(a).orderTarget = t2;
			rig.me(t1).unconscious = true;
			rig.me(t1).koTimer = 1.0e6;
			rig.run(20);
			test::require(rig.me(a).target == t2 && rig.me(a).comfortRolls > first, "a new target re-rolls");
			std::printf("ok  comfort re-roll: %.0f task start(s) for the first target (steady), %.0f after the switch\n", first, rig.me(a).comfortRolls);
		}

		// ---- attack slots: one holder, released at the last impact, the defender's deny roll ----
		{
			Rig rig(gd, data, 5);
			const std::uint64_t victim = rig.spawn(0.0, 0.0, 0, false);
			std::set<std::uint64_t> crowd;
			for (int i = 0; i < 6; ++i)
			{
				const double angle = 1.0 * i;
				const std::uint64_t id = rig.spawn(30.0 * std::cos(angle), 30.0 * std::sin(angle), 1, true);
				rig.me(id).orderTarget = victim;
				crowd.insert(id);
			}
			int releases = 0;
			int atLastImpact = 0; // released while the clip still plays, past the last impact
			int atClipEnd = 0; // released with the animation finished
			rig.arena.setObserver([&](const CombatEvent& e)
			{
				if (e.kind != CombatEvent::Kind::SlotRelease || e.who != victim || crowd.count(e.other) == 0)
					return;
				const state::Fighter& h = rig.me(e.other);
				if (h.state != state::CombatState::Attack || h.technique < 0)
					return;
				const Technique& t = rig.data->techniques[static_cast<std::size_t>(h.technique)];
				++releases;
				const double progress = h.clipTime / rig.data->clips[static_cast<std::size_t>(t.clip)].length;
				if (h.techniqueFinished)
					++atClipEnd;
				else if (!t.impacts.empty() && progress >= t.impacts.back().hitFrac && progress < 1.0)
					++atLastImpact;
			});
			int maxHeld = 0;
			int heldSamples = 0;
			int samples = 0;
			bool started = false;
			for (int i = 0; i < 20 * 40; ++i)
			{
				rig.run(1);
				int held = 0;
				for (const state::FighterSlot& slot : rig.me(victim).slots)
					held += slot.holder != 0 ? 1 : 0;
				maxHeld = std::max(maxHeld, held);
				if (rig.me(victim).unconscious)
					break;
				started = started || held > 0; // from the first acquire to the victim's KO (the original: held 99 % of that time)
				if (started)
				{
					++samples;
					heldSamples += held;
				}
			}
			test::require(maxHeld <= 1, "a defender has one attack slot (max num attack slots 1 + race extra 0): " + std::to_string(maxHeld) + " held at once");
			test::require(releases >= 3 && atLastImpact >= 1, "the slot is released by the swing (" + std::to_string(releases) + " releases, " + std::to_string(atLastImpact) + " at the last impact)");
			test::require(atClipEnd == 0, "no slot waits for the animation's end (" + std::to_string(atClipEnd) + " released at the clip end)");
			test::require(samples > 0 && heldSamples * 10 >= samples * 6, "with five attackers waiting, the slot is mostly held (no handover gap): " + std::to_string(heldSamples) + " of " + std::to_string(samples));
			std::printf("ok  slots: max held %d, %d releases (%d at the last impact, %d at the clip end), held %d of %d samples\n", maxHeld, releases, atLastImpact, atClipEnd, heldSamples, samples);

			// decideEngagement's deny roll: attackChanceRoll(defender, attacker) is true (circle) with probability 0.5 at equal skill.
			std::uint64_t seed = 99;
			int circled = 0;
			constexpr int kRolls = 4000;
			for (int i = 0; i < kRolls; ++i)
				circled += rig.arena.rules().attackChanceRoll(30.0, 30.0, seed) ? 1 : 0;
			test::require(circled > kRolls * 0.45 && circled < kRolls * 0.55, "the deny roll is 0.5 at equal skill: " + std::to_string(circled) + " of 4000");
			std::printf("ok  deny roll: %d of %d at equal skill\n", circled, kRolls);
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
