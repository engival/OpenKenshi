// h2v.2.7 (user playtest 2026-11-20, kenshi-k16.6.526): fighter B targets A, whose attack slots are full (5 engaged, the exe's cap), so B
// waits (state 6, WaitingStateAI). A free enemy C walks up and hits B for several seconds. Pinned, per ENGAGEMENT.md 2 / 3.5 / 4 (rwh113):
//  - B defends C's blows while it waits: the exit "any incoming threat (+0x228 != 0) -> state 4" and chooseCounterTechnique's gate
//    (selectTarget(list, -10): any attacker, not the target); B's defence outcomes (blocked / dodged / landed) are logged per impact;
//  - within the retarget hold plus a swing B's target is the free attacker C (the hub / defend re-pick; the trace shows when);
//  - the logged trace (B's target, state, outcomes) goes to stdout.
#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <iostream>
#include <map>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;

void testWaiterDefendsAndRepicks()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	CombatArena arena(CombatData::build(gd, KENSHI_DATA), CombatSettings{}, 11);
	const kdata::Record* servant = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			servant = r;
	test::require(servant != nullptr, "no Escaped Servant");
	std::uint64_t rng = 7;
	auto spawn = [&](std::uint64_t id, double x, double z, int team, bool fights)
	{
		state::Fighter f = CombatData::build(gd, KENSHI_DATA)->makeFighter(gd, servant->stringID, "", rng);
		f.ranged = state::FighterRanged{};
		f.x = x;
		f.z = z;
		f.yaw = 1.5707963267948966;
		f.team = team;
		f.playerControlled = !fights;
		arena.add(id, std::move(f));
	};
	const std::uint64_t A = 1, B = 2, C = 3;
	spawn(A, 60.0, 0.0, 1, false); // the crowded target: stands
	spawn(B, 0.0, 0.0, 0, true);
	spawn(C, -10.0, 8.0, 1, true); // the free enemy beside B
	arena.find(A)->engaged = {9001, 9002, 9003, 9004, 9005};
	arena.find(B)->target = A;
	perception::notice(*arena.find(B), A);
	perception::notice(*arena.find(B), C);
	perception::notice(*arena.find(C), B);
	std::map<int, int> outcomes; // Resolve code -> count, for B as the defender
	double firstOnC = 0.0;
	int vsC = 0;
	int bWait = 0, bTargetsC = 0, polls = 0, steps = 0;
	arena.setObserver([&](const CombatEvent& e)
	{
		if (e.kind == CombatEvent::Kind::Resolve && e.who == B)
		{
			++outcomes[e.code];
			vsC += e.other == C;
			std::cout << "  t=" << e.time << " B resolve " << e.code << " vs " << e.other << " (B target " << arena.find(B)->target << ")\n";
		}
		if (e.kind == CombatEvent::Kind::PickDefence && e.who == B)
			++polls;
	});
	for (int i = 0; i < 20 * 15; ++i)
	{
		arena.step(kDt);
		++steps;
		const state::Fighter& b = *arena.find(B);
		bWait += b.state == state::CombatState::Wait;
		bTargetsC += b.target == C;
		if (b.target == C && firstOnC == 0.0)
			firstOnC = steps * kDt;
		if (i % 20 == 0)
			std::cout << "  t=" << steps * kDt << " B target " << b.target << " state " << static_cast<int>(b.state) << " engagedOnA "
					  << arena.find(A)->engaged.size() << "\n";
	}
	std::cout << "B wait steps " << bWait << ", on C " << bTargetsC << ", defence polls " << polls << ", resolves blocked/dodged/hit "
			  << outcomes[5] << "/" << outcomes[0] << "/" << outcomes[2] << "\n";
	test::require(polls > 0, "B never picked a defence against C while it waited on a crowded A");
	test::require(vsC > 0, "none of C's blows reached a resolve against B (blocked, dodged or landed)");
	test::require(firstOnC > 0.0 && firstOnC < 3.0, "B did not turn to the free enemy hitting it within 3 s (first on C at " + std::to_string(firstOnC) + ")");
}

} // namespace

int main()
{
	try
	{
		testWaiterDefendsAndRepicks();
		std::cout << "ksim_wait_slot_aggro: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_wait_slot_aggro: FAILED: " << e.what() << "\n";
		return 1;
	}
}
