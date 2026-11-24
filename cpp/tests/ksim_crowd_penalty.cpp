// The crowd penalty of target selection (crowd_melee.md §10, task AU), on a bare combat arena with no navmesh:
//  - pickTarget scores each candidate as the distance term ((1 - d/200) x 2, 0.01 per unit) minus the candidate's crowd (its engaged
//    list + the refusals it made LAST step, less one when it is my current target), +5 for the current target. So a free enemy
//    within 100 u beats a nearer one with five attackers on it (task AU; the "down candidate distance x20 - 10" this test was built
//    with was a misread, kinspect ai_target_hand.md Q3 task BE: the x20 belongs to the picker's posture, and the task target also
//    wins +10 while fewer than 2 attack it);
//  - the current target keeps its +5 / crowd-1, so a held fight is not dropped for a free enemy just outside that margin;
//  - the refusal counter is a per-step double buffer: readers see the previous step's count only, so a waiter refused every step
//    reads 1, not a running total, and stops reading it one step after the refusals stop;
//  - the state-6 wait ring takes n = the TARGET's engaged list + its last-step refusals (never the waiter's own slots): 7 on 1 with
//    5 engaged and 4 refused gives n = 9 (inner 17.25, outer 52 before the follow offset).

#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>

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

	// `fights` false: a player character (it never picks a fight of its own), so a fixture stays put.
	std::uint64_t spawn(double x, double z, int team, bool fights)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
		f.x = x;
		f.z = z;
		f.yaw = 1.5707963267948966; // facing +x
		f.team = team;
		f.playerControlled = !fights;
		f.passive = !fights; // a fixture never joins an ally's fight (PASSIVE gates the join, rwh.228 s4)
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
};

// Five attackers in a candidate's engaged list (the exe's cap: `engaged.size() > 4` refuses).
void crowd(state::Fighter& target)
{
	target.engaged = {9001, 9002, 9003, 9004, 9005};
}

// notice() plus the last scan's FRESH flag (a notice alone leaves sinceSensed stale: goal 16 needs a sensed record, rwh.212).
void sensed(state::Fighter& observer, std::uint64_t target)
{
	perception::notice(observer, target);
	const_cast<state::SightRecord*>(perception::find(observer, target))->sinceSensed = 0.0;
}

void testCrowdLosesToFree()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 3);
	// The picker does not fight yet, so its first pick is made with BOTH enemies already in sight (notice(): the sensor record an
	// enemy's targeting would leave): otherwise it locks the first one it sees and the +5 keeps it there.
	const std::uint64_t picker = rig.spawn(0.0, 0.0, 0, false);
	const std::uint64_t crowded = rig.spawn(40.0, 0.0, 1, false);
	const std::uint64_t free = rig.spawn(90.0, 0.0, 1, false);
	crowd(rig.me(crowded));
	sensed(rig.me(picker), crowded);
	sensed(rig.me(picker), free);
	rig.me(picker).playerControlled = false; // from now on it picks fights
	rig.run(4);
	test::require(rig.me(picker).target == free,
		"the pick took the nearer crowded enemy (" + std::to_string(rig.me(picker).target) + ") over the free one at 90 u");
}

void testCurrentTargetKeepsItsBonus()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 4);
	const std::uint64_t picker = rig.spawn(0.0, 0.0, 0, false);
	const std::uint64_t crowded = rig.spawn(40.0, 0.0, 1, false);
	const std::uint64_t free = rig.spawn(90.0, 0.0, 1, false);
	crowd(rig.me(crowded));
	sensed(rig.me(picker), crowded);
	sensed(rig.me(picker), free);
	rig.me(picker).target = crowded;
	rig.me(picker).targetAge = 2.0; // the exe holds a normal NPC for 0 s: the pick re-scores every tick
	rig.me(picker).playerControlled = false;
	rig.run(4);
	// crowded: 1.6 - (5 - 1) + 5 = 2.6; the free one at 90 u: 1.1 (the case the first test shows it loses).
	test::require(rig.me(picker).target == crowded,
		"the held (crowded) target was dropped for the free enemy: " + std::to_string(rig.me(picker).target));
}

void testRefusalsReadLastStepOnly()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 5);
	const std::uint64_t attacker = rig.spawn(0.0, 0.0, 0, true);
	const std::uint64_t defender = rig.spawn(20.0, 0.0, 1, false);
	crowd(rig.me(defender));
	rig.me(defender).blockMode = true; // a defensive fighter never runs an attack, so it cannot stagger the attacker
	rig.run(40);
	test::require(rig.arena.lastRefusals(defender) == 1,
		"a refusal every step reads " + std::to_string(rig.arena.lastRefusals(defender)) + ", not 1 (the counter accumulates)");
	// No more refusals: the defender joins the attacker's team, so it is no longer an enemy to pick.
	rig.me(defender).team = 0;
	rig.run(2);
	test::require(rig.arena.lastRefusals(defender) == 0,
		"the refusal count outlived its step: " + std::to_string(rig.arena.lastRefusals(defender)));
	test::require(rig.me(attacker).target == 0, "the attacker still holds a target that stopped being an enemy");
}

void testWaitRingN()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 6);
	const std::uint64_t waiter = rig.spawn(0.0, 0.0, 0, true);
	const std::uint64_t target = rig.spawn(60.0, 0.0, 1, false);
	crowd(rig.me(target));
	rig.run(20);
	rig.me(waiter).target = target;
	// The last-step refusals: set both phases, so the read sees 4 whichever phase the arena is in (a step would clear one).
	CombatArena& arena = const_cast<CombatArena&>(rig.arena);
	for (CombatArena::Member& member : arena.membersMutable())
	{
		if (member.id == target)
			member.refused[0] = member.refused[1] = 4;
	}
	const std::optional<std::pair<double, double>> ring = rig.arena.waitRingOf(waiter);
	test::require(ring.has_value(), "no wait ring for a waiter with a live target");
	// n = 5 engaged + 4 refused = 9: inner 0.25 x 9 + 15 = 17.25, outer 3 x 9 + 25 = 52, plus the follow offset the steer adds
	// (2 x char width - 5.6 = 2.4), the same on every ring.
	const double offset = 2.0 * CombatRules::kCharWidth - 5.6;
	test::require(std::fabs(ring->first - (17.25 + offset)) < 1e-9,
		"inner " + std::to_string(ring->first) + " is not 17.25 + " + std::to_string(offset));
	test::require(std::fabs(ring->second - (52.0 + offset)) < 1e-9,
		"outer " + std::to_string(ring->second) + " is not 52 + " + std::to_string(offset));
	rig.me(waiter).target = 0;
	test::require(!rig.arena.waitRingOf(waiter).has_value(), "a waiter with no target still has a ring");
}

} // namespace

int main()
{
	try
	{
		testCrowdLosesToFree();
		testCurrentTargetKeepsItsBonus();
		testRefusalsReadLastStepOnly();
		testWaitRingN();
		std::cout << "ksim_crowd_penalty: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_crowd_penalty: FAILED: " << e.what() << "\n";
		return 1;
	}
}
