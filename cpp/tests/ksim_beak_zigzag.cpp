// The beak zig-zag (task BQ; ROADMAP 7d, kinspect findings/beak_zigzag.md 6edd18a). A swing is one box test at its impact frame:
// ~48 frames (0.8 s) after the swing starts, a slab along the CURRENT facing that does not track the target (the beak turns 40-98
// deg in its animation, not onto the player). A player who runs sideways out of the slab before that frame whiffs; one still in it
// is hit. The slab is ~22 u wide for a beak: the attacker's body width (RaceCombat::bodyWidth: RACE `hull size X` / 2 + 1.5, 10.5)
// plus 0.5 each side, plus the target's 3.45 u capsule.
//
// A bare arena (no navmesh; the straight line is the path) with the real Beak Thing ANIMAL_CHARACTER and "Escaped Servant" as the
// player character pins, per case:
//  - the data: the beak carries its natural weapon (WEAPON `giraffe type` under the record's `weapon` list: length 65), its race's
//    body width is 10.5, its `attack type` picks its own COMBAT_TECHNIQUE family, and the family's length puts its first impact
//    0.8 s after the swing start;
//  - the swing: one impact, at 0.8 s, and the body holds its facing (no tracking through the swing);
//  - the dodge: the player slides sideways at 55 u/s (the jog speed). Standing, or sliding from 0.7 s after the start, it is inside
//    the slab at the impact frame and is hit; a slide begun at the start or at 0.5 s is 19+ u off the line by then and the swing
//    whiffs (nothing enters the swing's hit list).

#include "ksim/combat/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr double kPi = 3.14159265358979323846;
constexpr double kSlideSpeed = 55.0; // u/s, the jog gait (SIMLINK.md section 12: walk 15, jog 55)
constexpr double kSwingSeconds = 0.8; // the measured impact frame (kAnimalAttackSeconds)
constexpr double kCapsuleRadius = 3.45; // the target capsule at the impact test (CombatArena's kCapsuleRadius, combat.cpp)
constexpr int kImpactTick = static_cast<int>(kSwingSeconds / kDt + 0.5); // the impact frame, in ticks after the swing's start

std::string characterSid(const kdata::GameData& gd, std::string_view typeName, std::string_view name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(std::string(typeName))))
	{
		if (r->name == name)
			return r->stringID;
	}
	throw std::runtime_error("no " + std::string(typeName) + " " + std::string(name));
}

struct Outcome
{
	bool swung = false;
	bool hit = false;
	double delay = 0.0; // seconds after the swing start the slide began (a large value: never)
	int impactTick = -1; // ticks after the swing start's step to the first impact
	double lateral = 0.0; // the player's offset from the beak's forward line at the impact frame, u
	double yawDrift = 0.0; // the beak's facing change over the swing, rad
	double length = 0.0; // the technique's length, s (0 = a human clip)
	int animal = 0;
	double bodyWidth = 0.0;
};

// One fight: the beak at the origin facing +x, the player 30 u ahead; the beak is ordered onto it (a player attack order, its
// target for as long as the fight lasts). On the swing start the player's sideways run begins after `delay` seconds, one jog-speed
// step per tick, in +z (the beak's right).
Outcome run(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data,
	const std::string& beakSid, const std::string& playerSid, double delay)
{
	CombatArena arena(data, CombatSettings{}, 21);
	std::uint64_t rng = 3;
	state::Fighter beak = data->makeFighter(gd, beakSid, "", rng);
	beak.x = 0.0;
	beak.z = 0.0;
	beak.yaw = kPi / 2.0; // the player is at +x
	beak.team = 1;
	beak.orderTarget = 2;
	state::Fighter player = data->makeFighter(gd, playerSid, "", rng);
	player.x = 30.0;
	player.z = 0.0;
	player.team = 0;
	player.playerControlled = true;
	arena.add(1, std::move(beak));
	arena.add(2, std::move(player));

	Outcome out;
	out.delay = delay;
	int swingTick = -1;
	double swingYaw = 0.0;
	for (int i = 0; i < 400; ++i)
	{
		// The slide runs before the step, so the impact test of the tick sees the position the player runs to.
		if (swingTick >= 0 && (i - swingTick) * kDt + 1.0e-9 >= delay)
			arena.find(2)->z += kSlideSpeed * kDt;
		arena.step(kDt);
		state::Fighter& b = *arena.find(1);
		const state::Fighter& p = *arena.find(2);
		if (swingTick < 0)
		{
			if (b.technique < 0)
				continue;
			swingTick = i;
			swingYaw = b.yaw;
			out.swung = true;
			const Technique& t = data->techniques[static_cast<std::size_t>(b.technique)];
			out.length = t.length;
			out.animal = t.animal;
			continue;
		}
		// The player's offset from the forward line the box is built along (the body yaw the swing started on), at the impact
		// frame (or at the swing's end, when it ended before it).
		const double dx = p.x - b.x;
		const double dz = p.z - b.z;
		const double lateral = std::fabs(dx * std::cos(swingYaw) - dz * std::sin(swingYaw));
		if (i - swingTick == kImpactTick || (b.techniqueFinished && out.impactTick < 0 && i - swingTick < kImpactTick))
			out.lateral = lateral;
		if (!out.hit && std::find(b.hitList.begin(), b.hitList.end(), 2) != b.hitList.end())
		{
			out.hit = true;
			out.impactTick = i - swingTick;
		}
		double drift = b.yaw - swingYaw;
		while (drift > kPi)
			drift -= 2.0 * kPi;
		while (drift < -kPi)
			drift += 2.0 * kPi;
		out.yawDrift = std::fabs(drift);
		if (b.techniqueFinished || b.technique < 0)
			break; // the swing is over: a whiff is a whiff
	}
	if (swingTick >= 0 && out.bodyWidth == 0.0)
	{
		const RaceCombat* race = data->race(arena.find(1)->raceSid);
		out.bodyWidth = race != nullptr ? race->bodyWidth : 0.0;
	}
	return out;
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::string beakSid = characterSid(gd, "ANIMAL_CHARACTER", "Beak Thing");
		const std::string playerSid = characterSid(gd, "CHARACTER", "Escaped Servant");

		// The data the fixture stands on.
		std::uint64_t rng = 3;
		const state::Fighter beak = data->makeFighter(gd, beakSid, "", rng);
		test::require(beak.animal, "the beak is not an animal");
		test::require(beak.attackType >= 9, "the beak has no animal attack type");
		test::require(beak.weapon.length > 50.0, "the beak carries no natural weapon (the WEAPON `giraffe type`, length 65)");
		const RaceCombat* race = data->race(beak.raceSid);
		test::require(race != nullptr && race->bodyWidth > 10.0, "the beak race's body width is not 10.5 (hull size X 18)");
		bool family = false;
		for (const Technique& t : data->techniques)
		{
			if (t.animal != beak.attackType)
				continue;
			family = true;
			test::require(t.length > 0.0, "an animal technique has no length: " + t.name);
			test::require(std::fabs(t.impacts.front().hitFrac * t.length - kSwingSeconds) < 1.0e-6,
				"the first impact is not at kAnimalAttackSeconds: " + t.name);
		}
		test::require(family, "no COMBAT_TECHNIQUE carries the beak's animal category");

		const double halfWidth = race->bodyWidth + 0.5 + kCapsuleRadius;
		std::printf("data: attack type %d, weapon %s length %.0f, body width %.1f, slab half-width including the capsule %.2f\n",
			beak.attackType, beak.weapon.name.c_str(), beak.weapon.length, race->bodyWidth, halfWidth);

		struct Case
		{
			const char* name;
			double delay;
			bool hit;
		};
		const Case cases[] = {
			{"standing still", 1.0e9, true},
			{"sideways at the swing start", 0.0, false},
			{"sideways 0.5 s after the start", 0.5, false},
			{"sideways 0.7 s after the start", 0.7, true},
		};
		for (const Case& c : cases)
		{
			const Outcome out = run(gd, data, beakSid, playerSid, c.delay);
			test::require(out.swung, std::string(c.name) + ": the beak never swung");
			test::require(out.animal == beak.attackType, std::string(c.name) + ": the swing was not the beak's own technique");
			test::require(out.length > 0.0, std::string(c.name) + ": the animal technique has a clip length");
			test::require(out.yawDrift < 0.02, std::string(c.name) + ": the beak turned during its swing (the box would track)");
			if (c.hit)
			{
				test::require(out.hit, std::string(c.name) + ": the swing whiffed");
				test::require(out.lateral < halfWidth,
					std::string(c.name) + ": the hit landed with the player past the slab's lateral reach");
			} else {
				test::require(!out.hit, std::string(c.name) + ": the swing landed");
				test::require(out.lateral > halfWidth,
					std::string(c.name) + ": the whiff came with the player inside the slab's lateral reach");
			}
			if (out.hit)
			{
				test::require(std::fabs(out.impactTick * kDt - kSwingSeconds) <= 2.0 * kDt,
					std::string(c.name) + ": the impact frame is not 0.8 s after the swing start");
			} else {
				test::require(out.impactTick < 0, std::string(c.name) + ": a whiff still recorded an impact");
			}
			std::printf("ok  %-30s hit %d, lateral %.2f u at the impact frame (reach %.2f), impact %d ticks, length %.3f s, yaw drift %.3f\n",
				c.name, out.hit ? 1 : 0, out.lateral, halfWidth, out.impactTick, out.length, out.yawDrift);
		}
		std::printf("ok  beak zig-zag: the impact is one box test along the facing, %.1f u wide; a sideways run clears it\n",
			2.0 * halfWidth);
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "ksim_beak_zigzag: %s\n", e.what());
		return 1;
	}
	return 0;
}
