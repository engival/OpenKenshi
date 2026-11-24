// Hunger and quiet medical on due-stamps (kenshi-k16.6.637, k16.6.607 item 6): hunger and a medically quiet member's medical run once a
// second on the member's own slice with the time since their stamp, so a game hour of hunger is the same at arena stride 1, 2 and 8 and
// equals the per-step update over the time the stamp has accounted; a quiet member's medical outcome equals the per-step one. The mirror's
// dirty flags: a stride-8 member that did not step is clean, a find / touch from outside marks it, a stepped one is marked.

#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/clock.hpp"

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

constexpr double kDt = 0.025;

struct Rig
{
	kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
	const kdata::Record* servant = nullptr;
	std::uint64_t rng = 11;

	Rig()
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "no Escaped Servant");
	}

	state::Fighter make(double x)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.x = x;
		f.faction = 10;
		f.skills.toughness = 50.0;
		return f;
	}
};

void near(double got, double want, double tol, const std::string& what)
{
	if (std::fabs(got - want) > tol)
		throw test::Failure(what + ": " + std::to_string(got) + ", wanted " + std::to_string(want));
	std::printf("ok  %-60s %.9g\n", what.c_str(), got);
}

// One game hour (kGameHourSeconds of sim time) of an idle member at the arena stride; returns its hunger lost, the time its stamp accounts.
struct HourRun
{
	double lost = 0.0;
	double accounted = 0.0;
	double perStep = 0.0; // hunger the per-step update loses over the same accounted time
	double blood = 0.0;
	double bloodPerStep = 0.0;
	double heat = 0.0;
	double heatPerStep = 0.0;
};

HourRun hour(Rig& rig, int stride)
{
	CombatSettings settings;
	settings.idleStride = stride;
	CombatArena arena(rig.data, settings, 5);
	state::Fighter f = rig.make(0.0);
	f.hunger = 3.0;
	const CombatRules rules(rig.data, settings);
	for (state::FighterPart& p : f.parts)
		p.heat = 3.0; // a quiet part still cools: the per-step and the slice must agree on it
	f.blood = rules.maxBlood(f) * 0.5; // recovering (no bleed): linear, clamped at the max
	state::Fighter twin = f;
	arena.add(1, std::move(f));
	test::require(rules.medicallyQuiet(*arena.find(1)), "the idle servant is medically quiet");
	const int steps = static_cast<int>(kGameHourSeconds / kDt);
	for (int i = 0; i < steps; ++i)
		arena.step(kDt);
	const state::Fighter& after = *std::as_const(arena).find(1);
	HourRun out;
	out.lost = 3.0 - after.hunger;
	// The first step's stamps were placed at phase 0; the accounted steps are those up to the last slice.
	const std::uint64_t medSteps = after.medStep;
	out.accounted = static_cast<double>(after.hungerStep) * kDt;
	test::require(after.hungerStep == after.medStep, "hunger and the quiet medical share their slice");
	for (std::uint64_t i = 0; i < medSteps; ++i)
		rules.updateMedical(twin, kDt);
	out.perStep = 3.0 - twin.hunger;
	out.blood = after.blood;
	out.bloodPerStep = twin.blood;
	out.heat = after.parts[0].heat;
	out.heatPerStep = twin.parts[0].heat;
	return out;
}

void testHunger()
{
	Rig rig;
	const HourRun one = hour(rig, 1);
	const HourRun two = hour(rig, 2);
	const HourRun eight = hour(rig, 8);
	std::printf("   lost: stride 1 %.9f, 2 %.9f, 8 %.9f; accounted %.3f s of %.3f\n", one.lost, two.lost, eight.lost, one.accounted, kGameHourSeconds);
	test::require(one.lost > 0.0, "the member got hungry");
	near(two.lost / one.lost, 1.0, 1.0e-3, "game-hour hunger, stride 2 vs stride 1 (within 0.1%)");
	near(eight.lost / one.lost, 1.0, 1.0e-3, "game-hour hunger, stride 8 vs stride 1 (within 0.1%)");
	for (const HourRun* r : {&one, &two, &eight})
	{
		near(r->lost / r->perStep, 1.0, 1.0e-9, "hunger equals the per-step update over the accounted time");
		near(r->blood, r->bloodPerStep, 1.0e-6, "a quiet member's blood recovery equals the per-step one");
		near(r->heat, r->heatPerStep, 1.0e-9, "a quiet member's heat decay equals the per-step one");
	}
}

void testQuiet()
{
	Rig rig;
	CombatSettings settings;
	const CombatRules rules(rig.data, settings);
	state::Fighter f = rig.make(0.0);
	test::require(rules.medicallyQuiet(f), "fresh servant is quiet");
	{
		state::Fighter g = f;
		g.parts[0].stun = 1.0;
		test::require(!rules.medicallyQuiet(g), "a stunned part is not quiet");
	}
	{
		state::Fighter g = f;
		g.parts[0].pool = 1.0;
		test::require(!rules.medicallyQuiet(g), "a bandage pool is not quiet");
	}
	{
		state::Fighter g = f;
		g.parts[0].health = g.parts[0].maxHealth * 0.7;
		test::require(!rules.medicallyQuiet(g), "a part over 20% damaged is not quiet");
	}
	{
		state::Fighter g = f;
		g.wounds.push_back(1.0);
		test::require(!rules.medicallyQuiet(g), "a wound is not quiet");
	}
	{
		state::Fighter g = f;
		g.unconscious = true;
		test::require(!rules.medicallyQuiet(g), "an unconscious member is not quiet");
	}
	std::printf("ok  medically quiet: no bleed, stun, pool, part over 20%%, conscious\n");

	// A hunger KO comes at the slice: a member at 0.1001 passes out within a second of the threshold.
	CombatArena arena(rig.data, settings, 5);
	state::Fighter hungry = rig.make(0.0);
	hungry.hunger = 0.1001;
	arena.add(1, std::move(hungry));
	for (int i = 0; i < static_cast<int>(kGameHourSeconds / kDt / 4); ++i)
		arena.step(kDt);
	test::require(std::as_const(arena).find(1)->hunger < 0.1 && std::as_const(arena).find(1)->unconscious, "the hunger KO is taken on the slice");
	std::printf("ok  hunger KO below 0.1 on the slice\n");
}

void testDirty()
{
	Rig rig;
	CombatSettings settings;
	settings.idleStride = 8;
	CombatArena arena(rig.data, settings, 5);
	constexpr int kMembers = 16;
	for (int i = 0; i < kMembers; ++i)
		arena.add(static_cast<std::uint64_t>(i + 1), rig.make(100.0 * i));
	for (int i = 0; i < 100; ++i)
	{
		arena.step(kDt);
		for (std::size_t k = 0; k < arena.members().size(); ++k)
			arena.takeDirty(k);
	}
	arena.step(kDt);
	int dirty = 0;
	int stepped = 0;
	for (std::size_t k = 0; k < arena.members().size(); ++k)
	{
		stepped += arena.members()[k].stepped ? 1 : 0;
		dirty += arena.takeDirty(k) ? 1 : 0;
	}
	std::printf("   stepped %d, dirty %d of %d\n", stepped, dirty, kMembers);
	test::require(stepped == 2, "stride 8 steps two of sixteen members a step");
	test::require(dirty <= stepped + 2, "the members that did not step are clean");
	test::require(dirty >= stepped, "the members that stepped are dirty");
	// An outside write marks the member (the mirror copies it); a read does not; touch marks.
	const std::uint64_t quiet = [&]
	{
		for (const CombatArena::Member& m : arena.members())
			if (!m.stepped)
				return m.id;
		return std::uint64_t{0};
	}();
	test::require(quiet != 0, "a member that did not step");
	arena.step(kDt);
	for (std::size_t k = 0; k < arena.members().size(); ++k)
		arena.takeDirty(k);
	const auto dirtyOf = [&](std::uint64_t id)
	{
		bool d = false;
		for (std::size_t k = 0; k < arena.members().size(); ++k)
			if (arena.members()[k].id == id)
				d = arena.takeDirty(k);
		return d;
	};
	test::require(std::as_const(arena).find(quiet) != nullptr && !dirtyOf(quiet), "a const find is a read");
	arena.find(quiet)->hunger = 1.5;
	test::require(dirtyOf(quiet), "a mutable find marks the member for the mirror");
	arena.touch(quiet);
	test::require(dirtyOf(quiet), "touch marks the member for the mirror");
	std::printf("ok  mirror dirty flags: stepped / written marked, the rest clean\n");
}

} // namespace

int main()
{
	try
	{
		testQuiet();
		testHunger();
		testDirty();
		std::cout << "ksim_hunger_stride: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_hunger_stride: FAILED: " << e.what() << "\n";
		return 1;
	}
}
