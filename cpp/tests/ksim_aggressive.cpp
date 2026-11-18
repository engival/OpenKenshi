// MQ14 path 2: the squad goal ATTACK_ENEMIES_AND_NEUTRALS (AI_TASK enum 21; template 46046 "Starving bandit mob aggressive" runs package 46047,
// whose squad goal 3944 is it; kinspect findings/item59/item60_mq14_hostility.md). A fighter with Fighter::attacksNeutrals takes every aware
// fighter that is not of its own faction or allied to it (relation >= 50) as a target, on top of its enemies. Combat's enemy() is symmetric
// for an engaged aggressor: the attacked, and whoever sees it at work, answer. A neutral chase that never closes in ends after
// kChaseClearOrders (66 game minutes, kinspect s73_static.md section 3) and the target is skipped for kBlockedTarget (20 s). The state is Fighter::{faction, attacksNeutrals,
// aggroBlockId, aggroBlock} (save v52); a save and load mid-chase equals the straight run.

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kGameMinuteSeconds = kGameDaySeconds / 1440.0;
// Faction ids no real FACTION entity has (the arena's bonded set is built from real factions only): every pair is neutral.
constexpr std::uint64_t kAggressors = 9000001;
constexpr std::uint64_t kVictims = 9000002;
constexpr std::uint64_t kOthers = 9000003;

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::string servant;
	Simulation sim;
	std::size_t giveUps = 0;
	std::int32_t giveUpCode = -1;
	double giveUpTime = -1.0;

	Rig(std::shared_ptr<const kdata::GameData> gdIn, std::shared_ptr<const CombatData> dataIn, std::string servantIn, bool world = false)
		: gd(std::move(gdIn)), data(std::move(dataIn)), servant(std::move(servantIn)), sim(world ? Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA)) : Simulation::load(KENSHI_DATA, 5, RegionConfig{}))
	{
		sim.enableCombat(data, CombatSettings{});
		sim.setClockTick(static_cast<std::uint64_t>(kTicksPerGameDay * 0.5)); // noon: full daylight
	}

	std::uint64_t spawn(double x, std::uint64_t faction, bool aggressive, bool player, std::uint64_t squad = 0)
	{
		state::Fighter f = data->makeFighter(*gd, servant, "", rng);
		f.x = x;
		f.z = 0.0;
		f.yaw = 1.5707963267948966; // facing +x
		f.team = 0; // the same team: only the goal can make them enemies
		f.faction = faction;
		f.attacksNeutrals = aggressive;
		f.playerControlled = player;
		f.squad = squad;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	state::Fighter& me(std::uint64_t id) { return *const_cast<state::Fighter*>(sim.combat()->find(id)); }

	void watch(std::uint64_t hunter)
	{
		const_cast<CombatArena*>(sim.combat())->setObserver([this, hunter](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::GiveUpChase && e.who == hunter)
			{
				++giveUps;
				giveUpCode = e.code;
				giveUpTime = e.time;
			}
		});
	}

	std::uint64_t rng = 7;
};

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		std::string servant;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r->stringID;
		}
		test::require(!servant.empty(), "no Escaped Servant");

		// ---- the data: package 46047 carries goal 21 (the coverage tool counts them all) ----
		{
			const routines::PackageTable table(*gd);
			const routines::CompiledPackage* package = table.find("46047-rebirth.mod");
			check(package != nullptr && routines::carriesGoal(*package, routines::task::AttackEnemiesAndNeutrals),
				"AI package 46047 (Patrol map aggressive) carries goal 21");
			check(routines::goalHolds(*package, routines::Role::Squad, routines::task::AttackEnemiesAndNeutrals, 3.0)
				&& routines::goalHolds(*package, routines::Role::Squad, routines::task::AttackEnemiesAndNeutrals, 15.0),
				"its window is open all day");
			const routines::CompiledPackage* plain = table.find("44430-rebirth.mod");
			check(plain != nullptr && !routines::carriesGoal(*plain, routines::task::AttackEnemiesAndNeutrals),
				"the Squatters package 44430 does not");
		}

		// ---- an aggressive fighter attacks a neutral it sees; the same fighter without the goal does not ----
		{
			Rig rig(gd, data, servant);
			const std::uint64_t hunter = rig.spawn(0.0, kAggressors, true, false);
			const std::uint64_t quarry = rig.spawn(300.0, kVictims, false, true, 77);
			const std::uint64_t mate = rig.spawn(330.0, kVictims, false, false, 77); // the attacked one's squadmate
			const std::uint64_t cousin = rig.spawn(340.0, kVictims, false, false, 78); // same faction, another squad
			rig.sim.run(20 * 8);
			check(rig.me(hunter).target == quarry, "an aggressive fighter takes the nearest neutral it sees as its target");
			check(rig.me(mate).target == hunter, "K5: targeting flips the victim's whole squad: the squadmate answers the aggressor");
			const auto& list = rig.me(mate).hostileSquads;
			check(list.size() == 1 && list[0] == rig.me(hunter).squad && rig.me(quarry).hostileSquads == list,
				"... as a personal hostile entry (the attacker's squad) on every member of the victim's squad");
			check(rig.me(cousin).hostileSquads.empty() && rig.me(cousin).target == 0, "K5: another squad of the same faction is not flipped");
			// Task BZ: the same flip writes the map's AGGRESSOR bit (2, dt23) - the attacker's squad on the victim's squad table, which
			// goal 34 reads as "already fighting me".
			const auto& aggressors = rig.me(mate).aggressorSquads;
			check(aggressors.size() == 1 && aggressors[0] == rig.me(hunter).squad && rig.me(quarry).aggressorSquads == aggressors,
				"BZ: the attack tags the attacker's squad AGGRESSOR on every member of the victim's squad");
			check(rig.me(cousin).aggressorSquads.empty(), "BZ: another squad of the same faction is not tagged");
		}
		// K1: the personal list comes before the relation: a hostile entry makes a same-team, goal-less pair enemies.
		{
			Rig rig(gd, data, servant);
			const std::uint64_t watcher = rig.spawn(0.0, kAggressors, false, false, 90);
			const std::uint64_t stranger = rig.spawn(300.0, kVictims, false, true, 91);
			rig.me(watcher).hostileSquads = {91};
			rig.sim.run(20 * 8);
			check(rig.me(watcher).target == stranger, "K1: a personal hostile (type 4) squad entry is attacked without the goal");
		}
		// K2: unique squads are never targets of the goal; K4 / hour window: the goal runs only inside its window.
		{
			Rig rig(gd, data, servant);
			const std::uint64_t hunter = rig.spawn(0.0, kAggressors, true, false);
			const std::uint64_t quarry = rig.spawn(300.0, kVictims, false, true);
			rig.me(quarry).uniqueSquad = true;
			rig.sim.run(20 * 10);
			check(rig.me(hunter).target == 0, "K2: a unique (story) squad is skipped by goal 21");
			rig.me(quarry).uniqueSquad = false;
			rig.me(hunter).aggroStart = 0;
			rig.me(hunter).aggroEnd = 6; // it is noon
			rig.sim.run(20 * 10);
			check(rig.me(hunter).target == 0, "hour window: outside it (noon, window 0-6) the goal picks nobody");
			rig.me(hunter).aggroStart = 11;
			rig.me(hunter).aggroEnd = 13;
			rig.sim.run(20 * 8);
			check(rig.me(hunter).target == quarry, "... and inside it (11-13) it attacks");
			rig.me(hunter).aggroStart = 0;
			rig.me(hunter).aggroEnd = 6;
			for (int i = 0; i < 20 * 3; ++i)
			{
				// The victim's own answer (it targeted the hunter, which made the hunter hostile to it too: K5 both ways) is not under test.
				rig.me(hunter).hostileSquads.clear();
				rig.me(quarry).target = 0;
				rig.sim.run(1);
			}
			check(rig.me(hunter).target == 0, "a running goal-21 chase ends when the hour leaves the window");
		}
		// K4: the role lists. Package 3949 "Patrol map: Aggressive (short range)" has goal 21 for the leader only.
		{
			const routines::PackageTable table(*gd);
			const routines::CompiledPackage* patrol = table.find("3949-gamedata.base");
			check(patrol != nullptr, "package 3949 exists");
			bool leader = false;
			bool member = false;
			for (const routines::GoalRow& row : patrol->list(routines::Role::Leader))
				leader = leader || row.task == routines::task::AttackEnemiesAndNeutrals;
			for (const routines::GoalRow& row : patrol->list(routines::Role::Squad))
				member = member || row.task == routines::task::AttackEnemiesAndNeutrals;
			std::printf("    Leader list holds goal 21: %d, Squad list: %d (after the Squad <- Leader fallback)\n", leader, member);
			check(leader && !member, "K4: goal 21 is in the leader list of 3949 only: members do not run it (per-character role list)");
		}
		{
			Rig rig(gd, data, servant);
			const std::uint64_t hunter = rig.spawn(0.0, kAggressors, false, false);
			const std::uint64_t quarry = rig.spawn(300.0, kVictims, false, true);
			const std::uint64_t other = rig.spawn(330.0, kVictims, false, false);
			rig.sim.run(20 * 30);
			check(rig.me(hunter).target == 0 && rig.me(quarry).target == 0 && rig.me(other).target == 0,
				"without the goal the same neutrals (same relation, same team) leave each other alone for 30 s");
		}

		// ---- own faction and allies are not neutral (real factions) ----
		{
			Rig rig(gd, data, servant, true);
			flecs::world& ecs = const_cast<flecs::world&>(rig.sim.world().ecs());
			std::vector<std::pair<std::uint64_t, flecs::entity>> factions;
			ecs.each([&](flecs::entity entity, const state::FactionState&, const state::Id& id) { factions.emplace_back(id.value, entity); });
			std::uint64_t allyA = 0;
			std::uint64_t allyB = 0;
			std::uint64_t neutralB = 0;
			std::size_t alliedPairs = 0;
			for (const auto& [a, from] : factions)
			{
				std::uint64_t friendOf = 0;
				std::uint64_t strangerOf = 0;
				for (const auto& [b, to] : factions)
				{
					if (a == b)
						continue;
					const double relation = factionRelation(*rig.sim.gameData(), from, to);
					alliedPairs += relation >= FactionBook::kAlly ? 1 : 0;
					if (friendOf == 0 && relation >= FactionBook::kAlly)
						friendOf = b;
					if (strangerOf == 0 && relation > FactionBook::kHostile && relation < FactionBook::kAlly)
						strangerOf = b;
				}
				if (friendOf != 0 && strangerOf != 0)
				{
					allyA = a;
					allyB = friendOf;
					neutralB = strangerOf;
					break;
				}
			}
			std::printf("    %zu factions, %zu allied ordered pairs\n", factions.size(), alliedPairs);
			check(allyA != 0 && neutralB != 0, "the data has an allied faction pair and a neutral one for the same faction");
			const std::uint64_t hunter = rig.spawn(0.0, allyA, true, false);
			const std::uint64_t sibling = rig.spawn(250.0, allyA, false, false);
			const std::uint64_t allied = rig.spawn(280.0, allyB, false, false);
			rig.sim.run(20 * 20);
			check(rig.me(hunter).target == 0 && rig.me(sibling).target == 0 && rig.me(allied).target == 0,
				"an aggressor leaves its own faction and an allied one alone");
			const std::uint64_t stranger = rig.spawn(310.0, neutralB, false, false);
			rig.sim.run(20 * 8);
			check(rig.me(hunter).target == stranger, "... and takes the neutral faction's character (the nearest unbonded one)");
			// K2: a faction on the `coexistence` list is skipped although its relation is not an alliance.
			std::uint64_t coA = 0;
			std::uint64_t coB = 0;
			for (const auto& [a, from] : factions)
			{
				const kdata::Record* record = rig.sim.gameData()->find(from.get<state::FactionState>()->templateSid);
				const kdata::RefCategory* list = record != nullptr ? record->findRefs("coexistence") : nullptr;
				for (const auto& [b, to] : factions)
				{
					if (coA != 0 || list == nullptr || a == b || factionRelation(*rig.sim.gameData(), from, to) >= FactionBook::kAlly)
						continue;
					for (const kdata::RefEntry& entry : list->entries)
					{
						if (entry.targetID == to.get<state::FactionState>()->templateSid)
						{
							coA = a;
							coB = b;
						}
					}
				}
			}
			check(coA != 0, "the data has a coexistence pair that is not an alliance");
			Rig co(gd, data, servant, true);
			const std::uint64_t coHunter = co.spawn(0.0, coA, true, false);
			const std::uint64_t coQuarry = co.spawn(300.0, coB, false, false);
			co.sim.run(20 * 10);
			check(co.me(coHunter).target == 0 && co.me(coQuarry).target == 0, "K2: goal 21 skips a coexistence faction");
		}

		// ---- a neutral that stays in sight yet is never reached: the monitor clears the chase after 60 U (66 game minutes), the target is
		// skipped for 20 s (addBlockedTarget), then taken up again; s73_static.md section 3 ----
		{
			Rig rig(gd, data, servant);
			const std::uint64_t hunter = rig.spawn(0.0, kAggressors, true, false);
			const std::uint64_t quarry = rig.spawn(300.0, kVictims, false, true);
			rig.watch(hunter);
			double droppedAt = -1.0;
			double retargetedAt = -1.0;
			rig.sim.run(1); // the arena's clock is set from the sim clock on its first step
			const double start = rig.sim.combat()->time();
			bool had = false;
			for (int i = 0; i < static_cast<int>(80.0 * kGameMinuteSeconds * 20.0); ++i)
			{
				// The quarry runs ahead of the hunter at a constant gap, always in its cone: never reached, never lost.
				rig.me(quarry).x = rig.me(hunter).x + 300.0;
				rig.me(quarry).z = rig.me(hunter).z;
				rig.me(quarry).target = 0;
				rig.sim.run(1);
				const double t = rig.sim.combat()->time() - start;
				const bool has = rig.me(hunter).target == quarry;
				if (had && !has && droppedAt < 0.0)
					droppedAt = t;
				else if (!had && has && droppedAt >= 0.0 && retargetedAt < 0.0)
					retargetedAt = t;
				had = has;
			}
			std::printf("    dropped at %.2f game minutes, taken up again %.1f s later\n", droppedAt / kGameMinuteSeconds, retargetedAt - droppedAt);
			check(droppedAt > 0.0 && droppedAt / kGameMinuteSeconds > 66.0 && droppedAt / kGameMinuteSeconds < 68.0,
				"an unreachable neutral in sight: the chase is cleared after 66 game minutes (60 U), no stall");
			check(rig.giveUps == 0, "... without a GiveUpChase (the target was sensed all along)");
			check(retargetedAt - droppedAt >= 19.0 && retargetedAt - droppedAt < 24.0,
				"... skipped for the 20 s blocked-target timer, then taken up again (no terminal state)");
		}


		// ---- save and load mid-chase equals the straight run ----
		{
			const auto build = [&]()
			{
				auto rig = std::make_unique<Rig>(gd, data, servant);
				rig->spawn(0.0, kAggressors, true, false);
				rig->spawn(300.0, kVictims, false, true);
				return rig;
			};
			const int total = 20 * 40;
			std::uint64_t straightHash = 0;
			{
				auto rig = build();
				rig->sim.run(total);
				straightHash = rig->sim.stateHash();
			}
			auto rig = build();
			rig->sim.run(20 * 5);
			const state::Fighter& before = *rig->sim.combat()->find(1);
			check(before.attacksNeutrals && before.faction == kAggressors && before.target != 0 && before.chaseAge > 0.0,
				"mid-chase at the save: goal flag, faction and chase state set");
			std::ostringstream out;
			rig->sim.save(out);
			std::istringstream in(out.str());
			Simulation restored = Simulation::restore(World::empty(), in, RegionConfig{}, true);
			restored.setGameData(gd);
			restored.enableCombat(data, CombatSettings{});
			const state::Fighter* after = restored.combat()->find(1);
			check(after != nullptr && after->attacksNeutrals && after->faction == kAggressors && after->chaseAge == before.chaseAge
				&& after->squad == before.squad && after->aggroEnd == before.aggroEnd,
				"the goal flag, the faction, the squad key and the chase survive the save");
			const state::Fighter* victim = restored.combat()->find(2);
			check(victim != nullptr && victim->hostileSquads == rig->sim.combat()->find(2)->hostileSquads && !victim->hostileSquads.empty(),
				"the victim squad's personal hostile list survives the save");
			restored.run(total - 20 * 5);
			check(restored.stateHash() == straightHash, "the state hash after a save and load mid-chase equals the straight run");
		}
		std::printf("ksim_aggressive: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
