// G2/G3 of the native AI runtime, phase C (task BW): the goal selector core (kinspect dt14) and the consumer resolvers a world
// squad runs (dt15/dt16). Pure: staged Fighters and sensor records, no game data and no world.

#include "ksim/law/crime.hpp"
#include "ksim/ai/goalsel.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/ai/sensor.hpp"

#include "../src/ksim/world/world_resolvers.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace ksim;

sensor::Context makeContext()
{
	sensor::Context ctx;
	ctx.playerFaction = 200;
	ctx.relation = [](std::uint64_t from, std::uint64_t to)
	{
		if (from == 100 && to == 300)
			return 60.0; // my ally
		if (from == 500 && to == 100)
			return -100.0; // their view of me: an enemy
		return 0.0;
	};
	return ctx;
}

state::Fighter fighter(std::uint64_t id, std::uint64_t faction, double z)
{
	state::Fighter f;
	f.faction = faction;
	f.squad = 1u << 20 | id;
	f.x = 0.0;
	f.z = z;
	return f;
}

state::SightRecord scanned(std::uint64_t target)
{
	state::SightRecord record;
	record.target = target;
	record.seen = true;
	record.sinceSeen = 0.0;
	record.sinceSensed = 0.0;
	return record;
}

goalsel::Offer offer(std::uint16_t task, int tier, double utility, double weight = 1.0)
{
	goalsel::Offer o;
	o.task = task;
	o.tier = tier;
	o.utility = utility;
	o.weight = weight;
	return o;
}

// ---- G2: the selector ----

void testTierOrder()
{
	// The first tier with any candidate wins, whatever the scores: a 0.1 urgent beats a 5.0 fluff.
	std::vector<goalsel::Offer> offers = {offer(10, 1, 5.0), offer(20, 2, 3.0), offer(30, 3, 0.1)};
	goalsel::Choice choice = goalsel::select(offers, {}, 13.0);
	test::require(choice.found && choice.task == 30 && choice.tier == 3, "urgent first");
	// No urgent: non-urgent wins over fluff.
	offers = {offer(10, 1, 5.0), offer(20, 2, 0.1)};
	choice = goalsel::select(offers, {}, 13.0);
	test::require(choice.found && choice.task == 20 && choice.tier == 2, "then non-urgent");
	// Inside a tier the highest score wins; a tie goes to the later offer.
	offers = {offer(10, 2, 0.5), offer(20, 2, 0.9), offer(30, 2, 0.9)};
	choice = goalsel::select(offers, {}, 13.0);
	test::require(choice.task == 30 && std::fabs(choice.score - 0.9) < 1e-9, "highest, later on a tie");
}

void testHysteresis()
{
	const goalsel::Current current{10, 77, true}; // the running goal, its target, inside its hold
	// The running goal keeps 1.25: 1.0 x 1.25 beats a competitor at 1.2.
	goalsel::Offer running = offer(10, 2, 1.0);
	running.target = 77;
	std::vector<goalsel::Offer> offers = {running, offer(20, 2, 1.2)};
	goalsel::Choice choice = goalsel::select(offers, current, 13.0);
	test::require(choice.task == 10 && std::fabs(choice.score - 1.25) < 1e-9, "the current goal's 1.25 bonus");
	// A different target of the same goal is not "the same goal": no bonus (0.5 < 1.2).
	goalsel::Offer other = offer(10, 2, 0.5);
	other.target = 88;
	offers = {other, offer(20, 2, 1.2)};
	choice = goalsel::select(offers, current, 13.0);
	test::require(choice.task == 20, "a retargeted goal is a competitor (the exe's same task AND hand test)");

	// The hold floor: inside the minimum a weak running goal is held at 0.4.
	goalsel::Offer held = offer(10, 2, 0.1);
	held.target = 77;
	held.holds = true;
	held.hold = {0.1, 0.5};
	offers = {held, offer(20, 2, 0.3)};
	choice = goalsel::select(offers, current, 13.0);
	test::require(choice.task == 10 && std::fabs(choice.score - goalsel::kHoldFloor) < 1e-9, "the 0.4 hold floor");

	// Once the hold has expired the re-chosen copy is capped at 0.05, so anything beats it.
	goalsel::Current expired{10, 77, false};
	choice = goalsel::select(offers, expired, 13.0);
	test::require(choice.task == 20, "after the hold a competitor displaces it");

	// A competitor under 1.0 is boosted too (the exe's "keep the running task competitive" branch, M).
	goalsel::Current none{}; // nothing running
	offers = {offer(20, 2, 0.5), offer(30, 2, 0.6)};
	choice = goalsel::select(offers, {10, 77, true}, 13.0);
	test::require(choice.task == 30, "the running task's competitors are boosted while one runs");
	(void)none;
}

void testCrowdAndWindows()
{
	test::require(std::fabs(goalsel::crowdFactor(0, 0.8) - 1.0) < 1e-12, "no squadmates: no decay");
	test::require(std::fabs(goalsel::crowdFactor(2, 0.8) - 0.64) < 1e-12, "two squadmates at 0.8");
	test::require(goalsel::crowdFactor(1, 0.0) == 0.0, "decay 0 (eat / kill) takes one mate off the table");
	goalsel::Offer crowded = offer(10, 2, 1.0);
	crowded.crowdDecay = 0.8;
	crowded.others = 2;
	test::require(std::fabs(goalsel::scoreOf(crowded, {}, 13.0) - 0.64) < 1e-12, "the crowd decay in the score");

	test::require(goalsel::windowHolds(12, 22, 13.0), "inside the window");
	test::require(!goalsel::windowHolds(12, 22, 22.0), "end is exclusive");
	test::require(goalsel::windowHolds(0, 0, 3.0) && goalsel::windowHolds(0, 0, 23.0), "start == end is always on");
	test::require(goalsel::windowHolds(22, 6, 2.0) && goalsel::windowHolds(22, 6, 23.0), "the wrap");
	test::require(!goalsel::windowHolds(22, 6, 12.0), "outside the wrap");
	test::require(goalsel::windowHolds(0, 280, 5.0), "an end over 24 is always on");
}

void testDefault()
{
	// Nothing scores (every utility 0, every window shut): the default task at 1.0, and the caller re-scores next update.
	std::vector<goalsel::Offer> offers = {offer(10, 3, 0.0), offer(20, 1, 0.5)};
	offers[1].start = 12;
	offers[1].end = 13;
	goalsel::Choice choice = goalsel::select(offers, {}, 20.0);
	test::require(!choice.found && choice.isDefault && choice.task == goalsel::kIdleTask, "the default_task fallback");
	test::require(std::fabs(choice.score - goalsel::kDefaultScore) < 1e-12, "at 1.0");
}

// The decision trace's verdicts (kenshi-k16.6.43): one per offer, and asking for them never changes the pick.
void testVerdicts()
{
	using goalsel::Why;
	// One offer of each verdict: the 0.1 urgent (3) beats everything of the lower tiers, so the tier-2 offers are LowerTier;
	// the same-tier loser is Outscored.
	goalsel::Offer shut = offer(50, 3, 1.0);
	shut.start = 1;
	shut.end = 2; // hour 13: closed
	std::vector<goalsel::Offer> offers = {offer(10, 3, 0.1), offer(11, 3, 0.05), offer(20, 2, 3.0), offer(30, 3, 0.0), shut};
	std::vector<goalsel::Verdict> verdicts;
	const goalsel::Choice plain = goalsel::select(offers, {}, 13.0);
	const goalsel::Choice traced = goalsel::select(offers, {}, 13.0, &verdicts);
	test::require(plain.found == traced.found && plain.task == traced.task && plain.score == traced.score && plain.tier == traced.tier
		&& plain.target == traced.target, "a verdict out-vector does not change the choice");
	test::require(verdicts.size() == offers.size(), "one verdict per offer");
	test::require(verdicts[0].why == Why::Chosen && std::fabs(verdicts[0].score - 0.1) < 1e-12, "Chosen: the urgent 0.1");
	test::require(verdicts[1].why == Why::Outscored, "Outscored: the same tier's lower score");
	test::require(verdicts[2].why == Why::LowerTier, "LowerTier: a higher tier had a candidate");
	test::require(verdicts[3].why == Why::NoTarget && verdicts[3].score == 0.0, "NoTarget: the resolver found nothing (utility 0)");
	test::require(verdicts[4].why == Why::OutOfWindow, "OutOfWindow: the hour window is shut");

	// HoldCapped: the running goal, hold expired, loses to a competitor at the same tier.
	goalsel::Offer held = offer(10, 2, 1.0);
	held.target = 77;
	held.holds = true;
	held.hold = {0.1, 0.5};
	offers = {held, offer(20, 2, 0.3)};
	goalsel::select(offers, goalsel::Current{10, 77, false}, 13.0, &verdicts);
	test::require(verdicts[0].why == Why::HoldCapped && std::fabs(verdicts[0].score - goalsel::kHoldCap) < 1e-12, "HoldCapped: the expired hold's 0.05 cap");
	test::require(verdicts[1].why == Why::Chosen, "the competitor takes the running goal's place");
	// Inside its hold the same goal is not capped.
	goalsel::select(offers, goalsel::Current{10, 77, true}, 13.0, &verdicts);
	test::require(verdicts[0].why == Why::Chosen, "inside the hold the running goal stays chosen");

	// Nothing scores: every verdict is NoTarget or OutOfWindow, the choice is the default task, and the vector is still filled.
	offers = {offer(10, 3, 0.0), shut};
	const goalsel::Choice none = goalsel::select(offers, {}, 13.0, &verdicts);
	test::require(none.isDefault && verdicts.size() == 2 && verdicts[0].why == Why::NoTarget && verdicts[1].why == Why::OutOfWindow,
		"no candidate: the default task, verdicts NoTarget and OutOfWindow");
	test::require(std::string(goalsel::whyName(Why::HoldCapped)) == "HoldCapped", "whyName");
}

// ---- G3: the resolvers ----

struct World
{
	sensor::Context ctx = makeContext();
	state::Fighter self = fighter(1, 100, 0.0);
	std::map<std::uint64_t, state::Fighter> bodies;

	std::uint64_t add(std::uint64_t id, std::uint64_t faction, double z)
	{
		auto [it, ok] = bodies.emplace(id, fighter(id, faction, z));
		test::require(ok, "unique body id");
		self.sight.push_back(scanned(id));
		return id;
	}
	const state::Fighter* lookup(std::uint64_t id) const
	{
		const auto found = bodies.find(id);
		return found != bodies.end() ? &found->second : nullptr;
	}
	state::Fighter& at(std::uint64_t id) { return bodies.at(id); }
};

void testLoot177()
{
	World world;
	const std::uint64_t far = world.add(2, 400, 101.0); // a corpse past the 100 u cap
	const std::uint64_t down = world.add(3, 400, 50.0); // a down body inside it
	const std::uint64_t corpse = world.add(4, 400, 90.0); // a corpse at 90 u
	world.at(2).dead = true;
	world.at(3).down = true;
	world.at(4).dead = true;
	auto none = [](std::uint64_t) { return false; };
	auto anyBody = [](std::uint64_t, const state::Fighter&) { return true; };
	auto lookup = [&](std::uint64_t id) { return world.lookup(id); };
	const std::uint64_t picked = worldres::loot177(world.self, world.ctx, lookup, none, anyBody);
	test::require(picked == corpse, "pass 1: the corpse, first in list order (not the nearer down body)");
	test::require(picked != far, "the 100 u cap");

	// Without a corpse in reach, pass 2 takes the down body.
	world.bodies.erase(4);
	world.self.sight.erase(world.self.sight.begin() + 2);
	test::require(worldres::loot177(world.self, world.ctx, lookup, none, anyBody) == down, "pass 2: a down body");
	// A squadmate on it, or a carrier's victim, is skipped.
	auto claimed = [&](std::uint64_t id) { return id == down; };
	test::require(worldres::loot177(world.self, world.ctx, lookup, claimed, anyBody) == 0, "a claimed body is skipped");
	// A body the member's task cannot use (ours: no food) is not a target.
	auto noBody = [](std::uint64_t, const state::Fighter&) { return false; };
	test::require(worldres::loot177(world.self, world.ctx, lookup, none, noBody) == 0, "a body with nothing to take is skipped");
	(void)far;
}

void testSlave183and168()
{
	World world;
	const std::uint64_t own = world.add(2, 400, 30.0); // a slave whose captor is my ally: process it
	world.at(2).slaveState = 1;
	world.at(2).captorFaction = 300;
	world.add(3, 400, 10.0); // my faction's slave, KO'd
	world.at(3).slaveState = 1;
	world.at(3).captorFaction = 100;
	world.at(3).unconscious = true;
	const std::uint64_t friendly = world.add(4, 400, 20.0); // a slave whose captor is my ally: processed already
	world.at(4).slaveState = 1;
	world.at(4).captorFaction = 300;
	auto none = [](std::uint64_t) { return false; };
	auto noLooks = [](std::uint64_t, const state::Fighter&) { return false; };
	auto lookup = [&](std::uint64_t id) { return world.lookup(id); };

	// 183: the first visible slave that still needs processing. Body 2 (no captor) comes first in Id order.
	test::require(worldres::slaveWatching183(world.self, world.ctx, lookup, none, noLooks, {}) == 2, "183: the first slave to process");
	// Claimed by a squadmate: not this one, then body 3 (my faction's own slave is not skipped by the friendly-captor test).
	auto claim2 = [](std::uint64_t id) { return id == 2; };
	test::require(worldres::slaveWatching183(world.self, world.ctx, lookup, claim2, noLooks, {}) == 3,
		"183: a claiming hunt moves on");
	// The exe's quirk (1408ca3d0): a slave with no captor or no owner faction is "still to process" before the one-catcher test,
	// so a claim does not take it off the list.
	world.at(2).captorFaction = 0;
	test::require(worldres::slaveWatching183(world.self, world.ctx, lookup, claim2, noLooks, {}) == 2,
		"183: an unattributed slave ignores a squadmate's claim (dt15)");
	world.at(2).captorFaction = 300;
	// A slave that already looks like one is not processed again.
	auto looksAll = [](std::uint64_t, const state::Fighter&) { return true; };
	test::require(worldres::slaveWatching183(world.self, world.ctx, lookup, claim2, looksAll, {}) == 0, "183: not one that already looks like a slave");

	// 168: my faction's own down slave, nearest.
	test::require(worldres::carryWounded168(world.self, world.ctx, lookup, none) == 3, "168: my faction's down slave");
	world.at(3).unconscious = false; // a conscious crawler: carried too (kenshi-k16.6.28; an armed caravan downs by bolt)
	world.at(3).down = true;
	test::require(worldres::carryWounded168(world.self, world.ctx, lookup, none) == 3, "168: a conscious crawler too");
	(void)own;
	(void)friendly;
}

// Task CH group 3 (dt24_38 section 3): 38's intruder hunt. Only the SENSED NEUTRALS that stand inside some OTHER building count; a
// sneaking one, one in my own home, one outdoors, an ally or an enemy, and the mask's exclusions (in a bed, a slave, law / military)
// do not. The whole list comes back - the shared node finder picks one at random, the caller's roll.
void testGuardNodeIntruders38()
{
	World world;
	world.ctx.relation = [](std::uint64_t from, std::uint64_t to)
	{
		if (from == 100 && to == 500)
			return -100.0; // my faction's enemy
		if (from == 100 && to == 300)
			return 60.0; // my ally
		return 0.0;
	};
	world.self.homeBuilding = 11;
	const std::uint64_t first = world.add(2, 400, 10.0); // a neutral inside building 12
	world.at(2).building = 12;
	const std::uint64_t sneaking = world.add(3, 400, 12.0);
	world.at(3).building = 12;
	world.at(3).sneak = true;
	const std::uint64_t home = world.add(4, 400, 14.0); // inside MY building
	world.at(4).building = 11;
	const std::uint64_t outside = world.add(5, 400, 16.0); // no interior
	const std::uint64_t sleeping = world.add(6, 400, 18.0); // in a bed: kInObject
	world.at(6).building = 12;
	world.at(6).asleep = true;
	const std::uint64_t slave = world.add(7, 400, 20.0);
	world.at(7).building = 12;
	world.at(7).slaveState = 1;
	const std::uint64_t law = world.add(8, 400, 22.0);
	world.at(8).building = 12;
	world.at(8).npcClass = crime::kClassLaw;
	const std::uint64_t hostile = world.add(9, 500, 24.0); // an enemy: never this goal's
	world.at(9).building = 12;
	const std::uint64_t ally = world.add(10, 300, 26.0);
	world.at(10).building = 12;
	const std::uint64_t second = world.add(11, 400, 28.0); // a second intruder, in list order after the first
	world.at(11).building = 13;
	auto lookup = [&](std::uint64_t id) { return world.lookup(id); };
	const std::vector<std::uint64_t> found = worldres::guardNodeIntruders38(world.self, world.ctx, lookup);
	test::require(found == std::vector<std::uint64_t>{first, second}, "38: the neutrals inside another building, in list order");

	// Nothing sensed: no list. A member with no home building never hunts.
	world.self.sight.clear();
	test::require(worldres::guardNodeIntruders38(world.self, world.ctx, lookup).empty(), "38: no records, no intruders");
	world.self.homeBuilding = 0;
	test::require(worldres::guardNodeIntruders38(world.self, world.ctx, lookup).empty(), "38: no home, no hunt");
	(void)sneaking;
	(void)home;
	(void)outside;
	(void)sleeping;
	(void)slave;
	(void)law;
	(void)hostile;
	(void)ally;
}

// Task CH turrets (dt24_turret_finder section 1): 149's distance bands. Engaged 0.8 within 600 u and 0.05 beyond; not engaged 1.0
// within 1500 u and 0.3 beyond. The test inputs are squared distances, the exe's own form.
void testTurretBands149()
{
	test::require(worldres::turretBand149(359999.0, true) == 0.8, "149 engaged at 599 u: 0.8");
	test::require(worldres::turretBand149(360000.0, true) == 0.05, "149 engaged at exactly 600 u: 0.05 (the strict test)");
	test::require(worldres::turretBand149(2249999.0, false) == 1.0, "149 not engaged at 1499 u: 1.0");
	test::require(worldres::turretBand149(2250000.0, false) == 0.3, "149 not engaged at exactly 1500 u: 0.3 (the strict test)");
	test::require(worldres::turretBand149(0.0, false) == 1.0 && worldres::turretBand149(4000000.0, true) == 0.05, "149 at 0 u and far away");
}

// Task CH town-residents-3 (TOWN_LIFE_SLICE 5.4, kinspect item 54 T3 / TQ9): the 153 / 239 bed ladder. The home first; else the
// nearest allied BARRACKS (designation 2) whose owner is not a civilian faction (4), then an allied BAR (3) not owned by a law (1),
// military (2), diplomat (5) or slave (6) faction, then any other allied building with beds.
void testBedLadder153()
{
	// {key, designation, own, beds, x, z}; the member stands at the origin.
	const std::uint64_t home = 10;
	const std::vector<worldres::BedBuilding> buildings = {
		{20, 3, false, 2, 100.0, 0.0}, // an allied bar
		{22, 2, false, 2, 300.0, 0.0}, // an allied armoury, far
		{23, 2, false, 2, 50.0, 0.0},  // an allied armoury, near
		{24, 2, false, 0, 10.0, 0.0},  // the nearest armoury has no bed: the rung hands over
		{26, 0, false, 1, 10.0, 0.0},  // any allied building with a bed
	};
	using Ladder = std::vector<std::uint64_t>;
	test::require(worldres::bedLadder153(home, 2, 0, false, buildings, 0.0, 0.0) == Ladder{home}, "the home's beds come first, and only they");
	test::require(worldres::bedLadder153(home, 2, 0, false, {}, 0.0, 0.0) == Ladder{home}, "a home alone is the whole ladder");
	// No home beds: the nearest designation-2 building WITH beds is not enough - the nearest one decides, and one without beds hands
	// over to the next rung (the bar), never to the next armoury.
	test::require(worldres::bedLadder153(0, 0, 0, false, buildings, 0.0, 0.0) == Ladder{20}, "a bedless nearest armoury hands to the bar rung");
	std::vector<worldres::BedBuilding> noEmpty = buildings;
	noEmpty.erase(noEmpty.begin() + 3);
	test::require(worldres::bedLadder153(0, 0, 0, false, noEmpty, 0.0, 0.0) == Ladder{23}, "the nearest armoury (23), not the far one");
	test::require(worldres::bedLadder153(0, 0, 4, false, noEmpty, 0.0, 0.0) == Ladder{20}, "npc_class 4 skips the armoury rung");
	test::require(worldres::bedLadder153(0, 0, 2, false, noEmpty, 0.0, 0.0) == Ladder{23}, "npc_class 2 still takes the armoury");
	std::vector<worldres::BedBuilding> noArmoury = {buildings[0], buildings[4]};
	test::require(worldres::bedLadder153(0, 0, 1, false, noArmoury, 0.0, 0.0) == Ladder{26}, "npc_class 1 skips the bar rung: any building next");
	test::require(worldres::bedLadder153(0, 0, 6, false, noArmoury, 0.0, 0.0) == Ladder{26}, "npc_class 6 skips the bar rung");
	test::require(worldres::bedLadder153(0, 0, 0, false, noArmoury, 0.0, 0.0) == Ladder{20}, "a plain NPC takes the bar");
	// Ties: the first in list order wins.
	const std::vector<worldres::BedBuilding> tie = {{31, 2, false, 1, 5.0, 0.0}, {32, 2, false, 1, 0.0, 5.0}};
	test::require(worldres::bedLadder153(0, 0, 0, false, tie, 0.0, 0.0) == Ladder{31}, "equal squared distance: the first wins");
	test::require(worldres::bedLadder153(0, 0, 0, false, {}, 0.0, 0.0).empty(), "no building, no pick");
	// The player faction scores: own 0.5 over allied -0.01, a bigger building over a one-bed one; the home joins the list.
	const std::vector<worldres::BedBuilding> mine = {{41, 0, false, 5, 1.0, 0.0}, {42, 0, true, 1, 900.0, 0.0}, {43, 0, true, 4, 900.0, 0.0}};
	test::require(worldres::bedLadder153(0, 0, 0, true, mine, 0.0, 0.0) == Ladder({43, 42, 41}), "player: own beats allied, more beds beat fewer");
	test::require(worldres::bedLadder153(44, 1, 0, true, mine, 0.0, 0.0) == Ladder({43, 42, 44, 41}), "player: the home building joins with the own score");
	test::require(worldres::bedLadder153(0, 0, 0, true, {}, 0.0, 0.0).empty(), "player with nothing: the NPC order (here nothing either)");
}

// Task CH town-residents-3 (dt24_patrol_town_pot sections 1-2): 36 PATROL_TOWN's leg. The town target: a uniform point within a
// square of half-side townRadius/4 around its centre (the move order's radius floor 500 gives a small town +/- 250). No town: the
// exe's +/-625 u random walk around the member.
void testPatrolSpot36()
{
	const worldres::PatrolSpot low = worldres::patrolSpot36(0.0, 0.0, true, 1000.0, 2000.0, 2500.0, 0.0, 0.0);
	test::require(low.x == 1000.0 - 625.0 && low.z == 2000.0 - 625.0, "36 in a 2500 u town: the corner of +/- 625 (townRadius/4)");
	const worldres::PatrolSpot high = worldres::patrolSpot36(0.0, 0.0, true, 1000.0, 2000.0, 2500.0, 1.0, 1.0);
	test::require(high.x == 1000.0 + 625.0 && high.z == 2000.0 + 625.0, "36 in a 2500 u town: the opposite corner");
	const worldres::PatrolSpot small = worldres::patrolSpot36(0.0, 0.0, true, 0.0, 0.0, 400.0, 0.0, 1.0);
	test::require(small.x == -250.0 && small.z == 250.0, "36 in a 400 u town: the order's 500 radius floor gives +/- 250");
	const worldres::PatrolSpot none = worldres::patrolSpot36(50.0, -70.0, false, 0.0, 0.0, 0.0, 0.0, 1.0);
	test::require(none.x == 50.0 - 625.0 && none.z == -70.0 + 625.0, "36 with no town: +/- 625 u around the member");
}

void testCages()
{
	World world;
	world.add(2, 900, 40.0); // slave state 0, awake: 260's target
	world.add(3, 900, 50.0); // unconscious: 260 skips it
	world.at(3).unconscious = true;
	world.add(4, 900, 60.0); // my faction's slave in a cage: 173's target
	world.at(4).slaveState = 1;
	std::vector<worldres::Cage> cages(3);
	cages[0].site = {routines::SiteKind::Cage, 7, 0};
	cages[0].occupant = 3;
	cages[0].x = 10.0;
	cages[0].building = 7;
	cages[1] = cages[0];
	cages[1].site.slot = 1;
	cages[1].occupant = 4;
	cages[1].x = 40.0;
	cages[1].myFaction = true;
	cages[1].claimed = false;
	cages[2] = cages[0];
	cages[2].site.slot = 2;
	cages[2].occupant = 2;
	cages[2].x = 5.0;
	cages[2].myHome = true;
	auto lookup = [&](std::uint64_t id) { return world.lookup(id); };

	// 260: the first cage whose occupant is awake and not yet a slave; no claim, no distance (the cage at x=5 wins over the nearer
	// first one because its occupant state is what counts, not proximity: slot order decides).
	test::require(worldres::newSlave260(cages, lookup) == 2, "260: the first fresh prisoner");
	std::vector<worldres::Cage> claimed = cages;
	claimed[2].claimed = true; // a claim is not read by 260
	test::require(worldres::newSlave260(claimed, lookup) == 2, "260 takes no squad claim (dt15)");

	// 173: the nearest occupied cage, its occupant (the cage at x = 5).
	test::require(worldres::gather173(world.self, cages) == 2, "173: the nearest occupied cage's occupant");

	// 112: the best free cage - my home's x100.
	routines::Site site;
	double score = 0.0;
	test::require(worldres::findCage112(world.self, cages, site, &score), "112: a cage");
	test::require(site.slot == 2 && score > 1.0, "112: the home building's cage");
	std::vector<worldres::Cage> none = cages;
	for (worldres::Cage& cage : none)
		cage.claimed = true;
	test::require(!worldres::findCage112(world.self, none, site), "112: no free cage");

	// 112's utility (dt24_112 section 3): the body's score, a separate decision from the cage pick. 2.0 living non-ally, 1.4 a slave
	// fenced by law with no positive relation, 0.1 with no town, 0 an ally or an animal, 0.5 a corpse.
	state::Fighter body;
	test::require(worldres::putInCage112(body, true, false, false, false) == 2.0, "112 utility: a living non-ally is 2.0");
	body.slaveState = 1;
	test::require(worldres::putInCage112(body, true, true, false, false) == 1.4, "112 utility: law + slave + no relation is 1.4");
	test::require(worldres::putInCage112(body, true, true, true, false) == 2.0, "112 utility: a positive relation drops the law bonus");
	test::require(worldres::putInCage112(body, false, false, false, false) == 0.1, "112 utility: no town is 0.1");
	body.slaveState = 0;
	test::require(worldres::putInCage112(body, true, false, false, true) == 0.0, "112 utility: an ally is 0");
	body.animal = true;
	test::require(worldres::putInCage112(body, true, false, false, false) == 0.0, "112 utility: an animal is 0 (the 0x248 virtual)");
	body.animal = false;
	body.dead = true;
	test::require(worldres::putInCage112(body, true, false, false, false) == 0.5, "112 utility: a corpse is 0.5");
}

// Task CH town-residents-3 (dt24_patrol_town_pot section 3): 137's machine finder and 138's corpse finder, pure.
void testPot137AndCorpse138()
{
	// 137: an ARGMAX of clamp(1 - d2/4608^2, 1e-5, 1) x100 my faction x100 my home building x1e-5 (law / military, non-public).
	state::Fighter self;
	self.x = 0.0;
	self.z = 0.0;
	std::vector<worldres::Pot> pots(3);
	pots[0].building = 10; pots[0].x = 2000.0; pots[0].z = 0.0; pots[0].myFaction = true;
	pots[1].building = 11; pots[1].x = 200.0; pots[1].z = 0.0; pots[1].myFaction = true;
	pots[2].building = 12; pots[2].x = 10.0; pots[2].z = 0.0; // neither my faction's nor mine (the caller would drop it)
	std::uint64_t picked = 0;
	double score = 0.0;
	test::require(worldres::findPot137(self, false, pots, picked, &score) && picked == 11,
		"137: the nearer of my faction's machines, not the nearer foreign one");
	test::require(std::fabs(score - 100.0 * (1.0 - 200.0 * 200.0 / 21233664.0)) < 1e-9, "137: the distance score x100");
	// My home building's machine wins from further off (x100 again).
	pots[0].myHome = true;
	test::require(worldres::findPot137(self, false, pots, picked) && picked == 10, "137: my home machine's x100 beats distance");
	// The law / military x1e-5 for a non-public interior drops a machine behind any public one.
	pots[0].myFaction = true;
	pots[0].publicBuilding = false;
	pots[1].publicBuilding = true;
	picked = 0;
	test::require(worldres::findPot137(self, true, pots, picked) && picked == 11, "137: a law member passes up a private machine");
	test::require(worldres::findPot137(self, false, pots, picked) && picked == 10, "137: ... and takes it otherwise");
	// Nothing admits.
	std::vector<worldres::Pot> none;
	test::require(!worldres::findPot137(self, false, none, picked), "137: no machine, no plan");
	pots[0].usable = false;
	const std::vector<worldres::Pot> unusable{pots[0]};
	test::require(!worldres::findPot137(self, false, unusable, picked), "137: an unusable machine is no machine");

	// 138 FIND_A_DEAD_GUY: its resolver cases are ksim_dead_guy.
}

} // namespace

int main()
{
	try
	{
		testTierOrder();
		testHysteresis();
		testCrowdAndWindows();
		testDefault();
		testVerdicts();
		testLoot177();
		testSlave183and168();
		testGuardNodeIntruders38();
		testTurretBands149();
		testBedLadder153();
		testPatrolSpot36();
		testCages();
		testPot137AndCorpse138();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_goal_select OK\n";
	return 0;
}
