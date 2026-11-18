// The bash / pick / wait decision (door_plan.hpp, DOORS.md section 6) and the lockpicking chance, as pure functions.

#include "ksim/door_plan.hpp"
#include "ksim/goap.hpp"
#include "ksim/interact.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

using namespace ksim;

void check(bool condition, const char* what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what);
}

DoorFacts base()
{
	DoorFacts facts;
	facts.hasOwner = true;
	facts.ownerKind = static_cast<std::int32_t>(FactionKind::Civilian);
	facts.lockLevel = 30;
	facts.lockpicking = 1.0;
	return facts;
}

} // namespace

int main()
{
	try
	{
		// The kinds: law, military, slave, slaver, bandit score 1.0; the rest 0.
		for (int kind = 0; kind <= 9; ++kind)
		{
			const bool want = kind == 1 || kind == 2 || kind == 6 || kind == 7 || kind == 8;
			check(kindBashesDoors(kind) == want, ("faction kind " + std::to_string(kind)).c_str());
		}

		// The owner's faction unlocks, whatever else holds.
		{
			DoorFacts facts = base();
			facts.mine = true;
			check(decideDoor(facts).plan == DoorPlan::Unlock, "owner unlocks");
		}
		// The bash row is "not mine, not allied" (dt43 G section 1): an armed hostile actor never bashes its own faction's door; the lock rows are
		// the owner's only, the pick has no owner atom (an unarmed actor picks an ally's door).
		{
			DoorFacts facts = base();
			facts.mine = true;
			facts.hostile = true;
			facts.weaponInHand = true;
			check(decideDoor(facts).plan == DoorPlan::Unlock, "own door, hostile, armed: unlock, never bash");
			facts = base();
			facts.allied = true;
			const DoorDecision d = decideDoor(facts);
			check(d.plan == DoorPlan::Pick && !d.unequipFirst, "allied door, empty hands: pick");
		}
		// A shut door that is not locked plans OPEN_DOOR alone, whoever the owner is.
		{
			DoorFacts facts = base();
			facts.locked = false;
			facts.hostile = true;
			facts.weaponInHand = true;
			check(decideDoor(facts).plan == DoorPlan::Open, "an unlocked door: open it");
		}
		// A hostile owner's door is bashed, with a weapon in hand; a carried weapon is equipped first.
		{
			DoorFacts facts = base();
			facts.hostile = true;
			facts.weaponInHand = true;
			DoorDecision d = decideDoor(facts);
			check(d.plan == DoorPlan::Bash && d.utility == 1.0 && !d.equipFirst, "hostile + weapon in hand: bash");
			facts.weaponInHand = false;
			facts.weaponCarried = true;
			d = decideDoor(facts);
			check(d.plan == DoorPlan::Bash && d.equipFirst, "hostile + carried weapon: bash after an equip step");
		}
		// Empty hands, no weapon anywhere: bash is dead, the pick wins if the skill allows (level - skill < 41.7).
		{
			DoorFacts facts = base();
			facts.hostile = true;
			DoorDecision d = decideDoor(facts);
			check(d.plan == DoorPlan::Pick && d.utility == kPickUtility, "hostile, empty hands: pick the lock (0.1)");
			facts.lockLevel = 60;
			check(decideDoor(facts).plan == DoorPlan::Wait, "level 60 - skill 1 >= 41.7: wait");
			facts.lockpicking = 18.4;
			check(decideDoor(facts).plan == DoorPlan::Pick, "level 60 - skill 18.4 < 41.7: pick");
			facts.lockpicking = 18.2;
			check(decideDoor(facts).plan == DoorPlan::Wait, "level 60 - skill 18.2 = 41.8 >= 41.7: wait");
		}
		// A wielded weapon blocks the pick and 7 UNEQUIP_WEAPON sheathes it (dt43 G: the pick has no owner gate; re-pinned from "wait", which
		// held while the door table had no row 7).
		{
			DoorFacts facts = base(); // a civilian owner, not hostile: bash scores 0
			facts.weaponInHand = true;
			const DoorDecision armed = decideDoor(facts);
			check(armed.plan == DoorPlan::Pick && armed.unequipFirst && armed.utility == kPickUtility, "civilian owner, weapon in hand: sheathe, pick (0.1)");
			facts.weaponInHand = false;
			const DoorDecision d = decideDoor(facts);
			check(d.plan == DoorPlan::Pick && d.utility == kPickUtility, "civilian owner, empty hands: pick (0.1)");
			facts.workingArm = false;
			check(decideDoor(facts).plan == DoorPlan::Wait, "no working arm: no pick");
		}
		// Non-hostile owners: only the bashing kinds score.
		{
			DoorFacts facts = base();
			facts.weaponInHand = true;
			facts.ownerKind = static_cast<std::int32_t>(FactionKind::Military);
			check(decideDoor(facts).plan == DoorPlan::Bash, "military owner (not hostile): bash");
			facts.ownerKind = static_cast<std::int32_t>(FactionKind::Trader);
			check(decideDoor(facts).plan == DoorPlan::Pick, "trader owner (not hostile): no bash score, weapon in hand: sheathe, pick");
			facts.ownerKind = static_cast<std::int32_t>(FactionKind::Bandit);
			facts.allied = true;
			const DoorDecision allyDoor = decideDoor(facts);
			check(allyDoor.plan == DoorPlan::Pick && allyDoor.unequipFirst, "allied owner: never bashed, weapon in hand: sheathe, pick (no owner gate on the pick)");
			facts.lockpicking = 1.0;
			facts.lockLevel = 80;
			check(decideDoor(facts).plan == DoorPlan::Wait, "allied owner, lock beyond the skill: wait");
			facts.lockLevel = 30;
			facts.hasOwner = false;
			facts.allied = false;
			facts.ownerKind = static_cast<std::int32_t>(FactionKind::Bandit);
			check(decideDoor(facts).plan == DoorPlan::Pick, "a door nobody owns has no kind: no bash, weapon in hand: sheathe, pick");
		}
		// Gates: shared by up to 8, divided by the mates, not under melee attack; no weapon needed (UNSURE).
		{
			DoorFacts facts = base();
			facts.hostile = true;
			facts.gate = true;
			DoorDecision d = decideDoor(facts);
			check(d.plan == DoorPlan::Bash && d.utility == 1.0 && !d.equipFirst, "gate, unarmed, alone: bash 1.0");
			facts.mates = 3;
			d = decideDoor(facts);
			check(d.plan == DoorPlan::Bash && std::fabs(d.utility - 0.25) < 1e-12, "gate with 3 mates on it: 1/4");
			facts.mates = 8;
			check(decideDoor(facts).plan == DoorPlan::Pick, "gate with 8 on it: bash dead, pick");
			facts.mates = 0;
			facts.underAttack = true;
			check(decideDoor(facts).plan == DoorPlan::Pick, "gate under melee attack: bash dead, pick");
			facts.weaponInHand = true;
			check(decideDoor(facts).plan == DoorPlan::Bash, "gate under attack, weapon in hand: BASH_DOOR still scores");
		}
		// The walk to the door (82 / 83 / 163 / 199): a plan whose door step is not yet at the door gets the walk in front of it.
		{
			struct Away final : goap::World
			{
				bool holds(goap::Sym, std::uint64_t, double, double) const override { return false; } // nothing holds: not at the door, nothing locked
			} away;
			const goap::Plan plan = goap::plan(away, 86, 7, 0.0, 0.0);
			check(plan.found() && plan.steps.size() >= 2 && plan.steps[0].task == goap::kMoveToBuildingDoor && plan.steps[1].task == goap::kOpenDoor,
				"OPEN_UP_SHOP_DOORS away from the door plans 82 then 72");
		}
		// The lock rows (77 / 79 / 162 / 198): a shut door of mine is locked from where the actor stands; the lock goals of 155 / 197 plan
		// through 162 / 198 (town-port 11 dropped their orderOnly: the lock step is the plan's, then the goal node).
		{
			struct Mine final : goap::World
			{
				bool outside = false;
				bool holds(goap::Sym sym, std::uint64_t, double, double) const override
				{
					return sym == goap::Sym::IsObjectMine || (outside ? sym == goap::Sym::AtDoorOutside : sym == goap::Sym::AtDoorInside || sym == goap::Sym::IAmInsideTargetBuilding);
				}
			} mine;
			const goap::Plan plan155 = goap::plan(mine, 155, 7, 0.0, 0.0);
			std::string steps;
			for (const goap::Step& step : plan155.steps)
				steps += std::to_string(step.task) + " ";
			std::printf("plan(155) with a door of mine, inside: %s\n", steps.c_str());
			check(steps == "162 155 ", "the lock goal 155 plans the inside lock 162, then itself");
			mine.outside = true;
			const goap::Plan plan197 = goap::plan(mine, 197, 7, 0.0, 0.0);
			steps.clear();
			for (const goap::Step& step : plan197.steps)
				steps += std::to_string(step.task) + " ";
			std::printf("plan(197) with a door of mine, outside: %s\n", steps.c_str());
			check(steps == "198 197 ", "the lock goal 197 plans the outside lock 198, then itself");
			bool has[4] = {};
			for (const goap::TaskRow& row : goap::rows())
				for (int k = 0; k < 4; ++k)
					has[k] = has[k] || row.task == (k == 0 ? goap::kLockDoor : k == 1 ? goap::kLockDoorHere : k == 2 ? goap::kLockDoorFromInside : goap::kLockDoorFromOutside);
			check(has[0] && has[1] && has[2] && has[3], "rows 77 / 79 / 162 / 198 exist");
		}
		// The stand points: inside and outside are the two faces of the slab, the near one is the walker's.
		{
			DoorSlab slab;
			slab.x = 100.0;
			slab.z = 50.0;
			slab.axisX = 1.0;
			slab.axisZ = 0.0; // a slab along x: its normals are +-z
			slab.halfDeep = 1.0;
			slab.inX = 0.0;
			slab.inZ = 1.0; // the building lies at +z
			const DoorPoint in = doorStandPoint(slab, DoorSide::Inside, 100.0, 0.0, kDoorFaceOut);
			const DoorPoint out = doorStandPoint(slab, DoorSide::Outside, 100.0, 90.0, kDoorFaceOut);
			const DoorPoint near = doorStandPoint(slab, DoorSide::Near, 100.0, 0.0, kDoorFaceOut);
			check(std::fabs(in.z - (50.0 + 1.0 + kDoorFaceOut)) < 1e-9 && std::fabs(out.z - (50.0 - 1.0 - kDoorFaceOut)) < 1e-9,
				"163 stands on the building's face, 199 on the other, whichever side the walker is on");
			check(std::fabs(near.z - out.z) < 1e-9, "82 / 83 take the walker's own face");
			slab.inZ = 0.0;
			check(std::fabs(doorStandPoint(slab, DoorSide::Inside, 100.0, 0.0, kDoorFaceOut).z - near.z) < 1e-9,
				"a door with no known building has no inside: the near face");
		}
		// The chance curve: 0.9 at or below the skill, halving every 10 levels, 5 % near 41.7 levels over.
		check(std::fabs(pickChance(10, 20) - 0.9) < 1e-12, "chance at skill above level: 0.9");
		check(std::fabs(pickChance(31, 1) - 0.9 / 8.0) < 1e-12, "30 levels over: 0.9 / 8");
		check(std::fabs(pickChance(1 + 41.7, 1) - 0.05) < 0.001, "5 % at 41.7 levels over (the planner's limit)");

		// ---- item 19: max HP from the door record ----
		{
			kdata::Record door;
			door.ints.push_back({"build materials", 16});
			check(doorMaxHealth(door) == 16.0, "HP: the legacy build materials (storm house door 16)");
			door.ints[0].value = 90;
			check(doorMaxHealth(door) == 90.0, "HP: build materials 90 (Defensive Gate level 4 Door)");
			kdata::RefCategory construction;
			construction.category = "construction";
			kdata::RefEntry plank;
			plank.targetID = "a-plank";
			plank.v0 = 5;
			kdata::RefEntry nail;
			nail.targetID = "a-nail";
			nail.v0 = 7;
			construction.entries = {plank, nail};
			door.references.push_back(construction);
			check(doorMaxHealth(door) == 12.0, "HP: a non-empty construction list wins, the sum of its counts (5 + 7)");
			door.references[0].entries.clear();
			check(doorMaxHealth(door) == 90.0, "HP: an empty construction list falls back to build materials");
			kdata::Record none;
			check(doorMaxHealth(none) == 1.0, "HP: neither: the original's zero-cost default 1 (born complete)");
		}

		// ---- item 19: open / close duration = 1 / door move speed ----
		{
			kdata::Record door;
			check(doorMoveSpeed(door) == 0.5, "move speed: the ctor default 0.5 when the record has none");
			const float quarter = 0.25f;
			kdata::FieldFloat field;
			field.key = "door move speed";
			std::memcpy(field.value.data(), &quarter, 4);
			door.floats.push_back(field);
			check(doorMoveSpeed(door) == 0.25, "move speed: read from the record");
			check(doorMoveSeconds(1.0) == 1.0 && doorMoveSeconds(0.5) == 2.0 && doorMoveSeconds(0.25) == 4.0 && std::fabs(doorMoveSeconds(0.2) - 5.0) < 1e-12,
				"duration: 1 / speed (houses 1 s, storm 2 s, gate panels 4 s, Defensive Gate 5 5 s)");
			check(doorMoveSeconds(0.0) == 10.0, "duration: speed 0 is floored at 0.1 (10 s)");
		}

		// ---- item 19: the lock level roll ----
		{
			int low = 1000, high = -1000;
			std::int32_t seen[21] = {};
			for (std::uint64_t key = 0; key < 20000; ++key)
			{
				const std::int32_t level = rollLockLevel(40, 10, 0, key);
				low = std::min(low, level);
				high = std::max(high, level);
				++seen[level - 30];
			}
			check(low == 30 && high == 50, "lock level: F 40 R 10 B 0 spans exactly 30..50");
			bool uniform = true;
			for (const std::int32_t count : seen)
				uniform = uniform && count > 20000 / 21 * 0.8 && count < 20000 / 21 * 1.2;
			check(uniform, "lock level: one uniform integer roll over the 2R + 1 values");
			check(rollLockLevel(40, 10, 30, 7) >= 60 && rollLockLevel(40, 10, 30, 7) <= 80, "lock level: the door's bonus adds (Outpost Door III +30)");
			check(rollLockLevel(10, 0, -10, 1) == 1, "lock level: clamped up to 1");
			check(rollLockLevel(80, 10, 40, 1) == 100, "lock level: clamped down to 100");
			check(rollLockLevel(40, 10, 0, 12345) == rollLockLevel(40, 10, 0, 12345), "lock level: deterministic in the key");
		}

		// ---- item 19: the lockpick interval schedule ----
		{
			// The first attempt at 5 game-s (100 steps of 0.05), nothing before.
			LockTimer lock;
			int steps = 0;
			PickTick tick = PickTick::Waiting;
			while (tick == PickTick::Waiting && steps < 1000)
			{
				tick = tickLock(lock, 0.05, 0.0, 99, 1);
				++steps;
			}
			check(steps == 100 && tick == PickTick::Failed, "lockpick: the first attempt comes after 5 game-s");
			check(lock.attempts == 1 && lock.elapsed == 0.0, "lockpick: an attempt resets the elapsed time and counts");
			check(lock.interval >= kPickIntervalMin && lock.interval < kPickIntervalMax, "lockpick: a failure re-rolls the interval in [5, 20)");
			// Re-rolls: many failures stay inside [5, 20) and spread over it; each wait is the interval just rolled.
			double lowest = 100.0, highest = 0.0;
			for (int round = 0; round < 400; ++round)
			{
				const double wanted = lock.interval;
				double waited = 0.0;
				PickTick again = PickTick::Waiting;
				while (again == PickTick::Waiting)
				{
					again = tickLock(lock, 0.05, 0.0, 99, 1);
					waited += 0.05;
				}
				if (std::fabs(waited - wanted) > 0.06)
					check(false, "lockpick: an attempt came at the rolled interval");
				lowest = std::min(lowest, lock.interval);
				highest = std::max(highest, lock.interval);
			}
			check(lowest < 6.0 && highest > 19.0 && lowest >= 5.0 && highest < 20.0, "lockpick: the re-rolled intervals spread over [5, 20)");
			// A sure roll unlocks at the first attempt.
			LockTimer sure;
			PickTick got = PickTick::Waiting;
			for (int i = 0; i < 100 && got == PickTick::Waiting; ++i)
				got = tickLock(sure, 0.05, 1.0, 5, 2);
			check(got == PickTick::Unlocked, "lockpick: chance 1 unlocks at the first attempt");
			// Shared and surviving interruption: two pickers on one lock, with a gap in the middle, equal one picker's time.
			LockTimer shared;
			for (int i = 0; i < 40; ++i)
				tickLock(shared, 0.05, 0.0, 77, 1); // picker 1: 2 s, then stops
			check(shared.attempts == 0 && std::fabs(shared.elapsed - 2.0) < 1e-9, "lockpick: an interrupted pick leaves its elapsed time on the lock");
			PickTick resumed = PickTick::Waiting;
			int more = 0;
			while (resumed == PickTick::Waiting)
			{
				resumed = tickLock(shared, 0.05, 0.0, 77, 2); // picker 2 carries on from 2 s
				++more;
			}
			check(more == 60, "lockpick: another picker resumes the shared timer (3 s more, not 5)");
			// The dice depend only on (lock, picker, attempts): the same state gives the same sequence.
			LockTimer a, b;
			bool same = true;
			for (int i = 0; i < 4000; ++i)
				same = same && tickLock(a, 0.05, 0.3, 4242, 9) == tickLock(b, 0.05, 0.3, 4242, 9) && a.elapsed == b.elapsed && a.interval == b.interval;
			check(same, "lockpick: deterministic; a saved timer (elapsed, interval, attempts) resumes the same sequence");
			// Copy the timer mid-wait (a save and load of the three numbers) and continue both.
			LockTimer saved;
			for (int i = 0; i < 130; ++i)
				tickLock(saved, 0.05, 0.3, 31, 3);
			LockTimer loaded{saved.elapsed, saved.interval, saved.attempts};
			bool match = true;
			for (int i = 0; i < 3000; ++i)
				match = match && tickLock(saved, 0.05, 0.3, 31, 3) == tickLock(loaded, 0.05, 0.3, 31, 3) && saved.attempts == loaded.attempts;
			check(match, "lockpick: save / load mid-pick continues identically");
		}
		std::printf("ksim_door_plan: OK\n");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
