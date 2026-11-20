// Stealth breaks when a hostile targets the character (task sneak-break; the user's bar_lure / bar_lure_hostile playtests:
// the scenario starts the player with stealth on, a Dust Bandit patrol attacks, and the player stood in stealth without
// fighting back).
//
// The exe's AI::onTargetedBy (140999f00) clears Character+0xd4 through 1405ca9a0(c, 0), which also puts the speed mode back
// to the mover default (SQUAD_FOLLOW.md s9); ours does it in CombatArena::setTarget through clearSneak. joinFight no longer
// refuses every sneaking character: its own attacker is answered (self-defence), an ALLY's fight is still not joined while
// sneaking (SQUAD_FOLLOW.md s9 measured the blocker; UNSURE, see combat.cpp).
//
//  1. the player sneaking (stealth order) is targeted by an enemy: the sneak flag clears with the speed mode back to Run, and
//     it fights back (goal 32 sets its target on the attacker within the self-defence range);
//  2. the player sneaking on a move order: the sneak flag clears, the walk is kept (no target), and once the order ends the
//     player targets its attacker;
//  3. a sneaking player is NOT pulled into an ally's fight; with sneak off it joins (PROTECT_ALLIES);
//  4. an attacker's swing start clears its OWN sneak (the engagement notify 14043a730);
//  5. being hit clears it with no target set: a sneaking bystander splashed by a swing aimed at someone else loses stealth
//     while the attacker never targeted it (STEALTH_CLEAR.md section 4).
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/core/command.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kGroundX = -51085.0; // open ground east of the Hub buildings (ksim_indoors)
constexpr double kGroundZ = 3145.0;

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	const std::int32_t index = kworld::itemTypeByName(type);
	if (index < 0)
		return nullptr;
	for (const kdata::Record* record : gd.byType(index))
		if (record->name == name)
			return record;
	return nullptr;
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::unique_ptr<Simulation> sim;
	double x = kGroundX;
	double y = 0.0;
	double z = kGroundZ;
	std::uint64_t rng = 9001;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n)
		: gd(g)
		, data(d)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		const auto spot = nav->walker().snap({x, 1530.0, z}, 60.0);
		test::require(spot.has_value(), "no Hub mesh near the sneak-break ground");
		y = spot->y;
	}

	// A fighter from the Escaped Servant record: team 1 is the enemy, team 0 an ally of the player.
	std::uint64_t add(std::uint64_t team, double dx, double dz, bool playerControlled, std::uint64_t squad = 0)
	{
		const kdata::Record* servant = recordNamed(*gd, "CHARACTER", "Escaped Servant");
		test::require(servant != nullptr, "no Escaped Servant CHARACTER");
		state::Fighter f = data->makeFighter(*gd, servant->stringID, "", ++rng);
		f.x = x + dx;
		f.y = y;
		f.z = z + dz;
		f.team = team;
		f.playerControlled = playerControlled;
		f.squad = squad;
		f.attention = 0.5;
		f.moveMode = 0; // when sneak breaks it must go back to the default (2)
		return sim->spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	CombatArena& arena() { return *const_cast<CombatArena*>(sim->combat()); }
	state::Fighter* fighter(std::uint64_t id) { return arena().find(id); }
	state::Fighter& player(std::uint64_t id)
	{
		state::Fighter* f = fighter(id);
		test::require(f != nullptr, "the player is gone");
		return *f;
	}
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	// The first tick this becomes true, within n ticks; -1 never.
	int tickUntil(int n, const std::function<bool()>& reached)
	{
		for (int i = 0; i < n; ++i)
		{
			if (reached())
				return i;
			sim->tick();
		}
		return reached() ? n : -1;
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_sneak_break HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- 1. targeted while sneaking: the flag clears with the speed mode and the player answers ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t player = rig.add(0, 0.0, 0.0, true);
			const std::uint64_t enemy = rig.add(1, 0.0, 8.0, false);
			rig.sim->enqueue(Command::fighterSneak(state::Id{player}, true));
			rig.tick(2);
			state::Fighter& p = rig.player(player);
			test::require(p.sneak, "the stealth order did not set the player's sneak flag");
			test::require(p.moveMode == 0, "the player's walk mode did not stick before the targeting");
			// A hostile sets its target on the sneaking player (an NPC attack order is a new targeting).
			rig.sim->enqueue(Command::fighterAttack(state::Id{enemy}, state::Id{player}));
			const int targeted = rig.tickUntil(120, [&]() { return rig.fighter(enemy)->target == player; });
			test::require(targeted >= 0, "the enemy never set its target on the player");
			test::require(!p.sneak, "a targeted sneaking player kept its sneak flag");
			test::require(p.moveMode == 2, "clearing sneak did not put the speed mode back to Run (2): " + std::to_string(p.moveMode));
			// Self-defence: the player's own attacker, within kSelfDefenceRange, is answered.
			const int answered = rig.tickUntil(240, [&]() { return p.target == enemy; });
			test::require(answered >= 0, "the targeted player did not fight back (self-defence)");
			std::cout << "targeted while sneaking: known at tick " << targeted + 1 << ", answering at " << targeted + answered + 1
				<< ", sneak cleared, mode Run\n";
		}

		// ---- 2. a move order: the sneak flag clears but the walk is kept; the fight starts when the order ends ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t player = rig.add(0, 0.0, 0.0, true);
			const std::uint64_t enemy = rig.add(1, 0.0, 8.0, false);
			rig.sim->enqueue(Command::fighterSneak(state::Id{player}, true));
			rig.tick(2);
			state::Fighter& p = rig.player(player);
			const Route route = rig.nav->routeFrom({p.x, p.y, p.z}, p.x + 40.0, p.z);
			test::require(route.found, "no route for the player's move order on the Hub mesh");
			rig.sim->enqueue(Command::fighterMoveTo(state::Id{player}, route.points, route.heights, 1, false, route.approach));
			rig.tick(3);
			test::require(p.walking(), "the player is not walking its move order");
			rig.sim->enqueue(Command::fighterAttack(state::Id{enemy}, state::Id{player}));
			const int targeted = rig.tickUntil(120, [&]() { return rig.fighter(enemy)->target == player; });
			test::require(targeted >= 0, "the enemy never set its target on the walking player");
			test::require(!p.sneak, "a targeted sneaking player on a move order kept its sneak flag");
			// The player-order rule (task BE) stands: the walk is kept, no target while it runs.
			bool targetWhileWalking = false;
			int walked = -1;
			for (int i = 0; i < 60 && walked < 0; ++i)
			{
				if (!p.walking())
					walked = i;
				else if (p.target != 0)
					targetWhileWalking = true;
				rig.sim->tick();
			}
			test::require(walked >= 0, "the move order never ended");
			test::require(!targetWhileWalking, "the player took a target while its move order ran");
			const int answered = rig.tickUntil(240, [&]() { return p.target == enemy; });
			test::require(answered >= 0, "the player did not fight back once the move order ended");
			std::cout << "move order: sneak cleared at tick " << targeted + 1 << ", the walk kept until " << targeted + walked + 1
				<< ", answering " << answered << " ticks later\n";
		}

		// ---- 3. an unnoticed sneaking player is not pulled into an ally's fight; with sneak off it joins ----
		{
			Rig rig(gd, data, nav);
			// The player stands off the enemy-ally line: the enemy's route to the ally bends round a wall here, and the shrunk-portal route (k16.6.248)
			// is tighter than the old pushed corners, so the enemy engages from about 7 u and its 16 u swing box (no wall test, kenshi-n02.5) reached the
			// player in line behind the ally (a Hit, reason 2). Not a movement fault.
			const std::uint64_t player = rig.add(0, -18.0, 0.0, true, 7);
			const std::uint64_t ally = rig.add(0, 0.0, 10.0, false, 7);
			const std::uint64_t enemy = rig.add(1, 0.0, 18.0, false);
			rig.sim->enqueue(Command::fighterSneak(state::Id{player}, true));
			rig.tick(2);
			state::Fighter& p = rig.player(player);
			test::require(p.sneak, "the stealth order did not set the player's sneak flag");
			// The enemy attacks the ALLY, never the player: the player is not targeted, so its sneak stands.
			rig.sim->enqueue(Command::fighterAttack(state::Id{enemy}, state::Id{ally}));
			const int aimed = rig.tickUntil(120, [&]() { return rig.fighter(enemy)->target == ally; });
			test::require(aimed >= 0, "the enemy never set its target on the ally");
			rig.tick(60);
			test::require(p.sneak, "the player's sneak cleared although only the ally was targeted");
			test::require(p.target == 0, "a sneaking player joined its ally's fight");
			// Sneak off (the player's own toggle): PROTECT_ALLIES now joins the ally's fight.
			rig.sim->enqueue(Command::fighterSneak(state::Id{player}, false));
			const int joined = rig.tickUntil(240, [&]() { return p.target == enemy; });
			test::require(joined >= 0, "the player did not join its ally's fight with sneak off");
			std::cout << "ally's fight: a sneaking player stayed out; with sneak off it joined " << joined << " ticks later\n";
		}

		// ---- 4. the attacker's own swing start clears its stealth (the engagement notify) ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t player = rig.add(0, 0.0, 0.0, true);
			// A neutral dummy (the player's own team, attacksNeutrals off): it never targets the player, so the only clear
			// that can happen is the player's own swing start (STEALTH_CLEAR.md section 1, 14043a730 -> 140999d50(attacker, 1)).
			const std::uint64_t dummy = rig.add(0, 0.0, 8.0, false);
			rig.sim->enqueue(Command::fighterSneak(state::Id{player}, true));
			rig.tick(2);
			state::Fighter& p = rig.player(player);
			test::require(p.sneak, "the stealth order did not set the player's sneak flag");
			rig.sim->enqueue(Command::fighterAttack(state::Id{player}, state::Id{dummy}));
			const int swung = rig.tickUntil(400, [&]() { return p.technique >= 0; });
			test::require(swung >= 0, "the player never started a swing on the dummy");
			test::require(!p.sneak, "the attacker kept its sneak through its own swing");
			test::require(rig.fighter(dummy)->target != player, "the dummy targeted the player: the clear cannot be the targeting one");
			std::cout << "attacker's swing: sneak cleared at the swing start (" << swung + 1 << " ticks)\n";
		}

		// ---- 5. being hit clears it with no target set: a bystander splashed by someone else's swing ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t player = rig.add(0, 0.0, 0.0, true);
			const std::uint64_t ally = rig.add(0, 0.0, 10.0, false);
			const std::uint64_t enemy = rig.add(1, 0.0, 18.0, false);
			// The player stands beside the ally, inside the impact box of the swing aimed at it (lateral half-width 4.5 u).
			state::Fighter& p = rig.player(player);
			p.x += 2.5;
			p.z += 10.0; // level with the ally, 2.5 u to its side (the box's lateral half-width is 4.5)
			rig.sim->enqueue(Command::fighterSneak(state::Id{player}, true));
			rig.tick(2);
			test::require(p.sneak, "the stealth order did not set the player's sneak flag");
			rig.sim->enqueue(Command::fighterAttack(state::Id{enemy}, state::Id{ally}));
			bool reliedOnTargeting = false;
			int hit = -1;
			for (int i = 0; i < 600 && hit < 0; ++i)
			{
				rig.sim->tick();
				const state::Fighter* e = rig.fighter(enemy);
				if (e != nullptr && e->target == player)
					reliedOnTargeting = true; // the clear would then also be the targeting one; the spot must avoid it
				if (!p.sneak)
					hit = i;
			}
			test::require(hit >= 0, "a sneaking bystander splashed by a swing kept its sneak");
			test::require(!reliedOnTargeting, "the attacker targeted the bystander: the hit is not a no-target hit");
			const auto hitByEnemy = [&]
			{
				for (const CombatArena::Member& m : rig.sim->combat()->members())
					if (m.id == player)
						return std::find(m.hitBy.begin(), m.hitBy.end(), enemy) != m.hitBy.end();
				return false;
			};
			test::require(hitByEnemy(), "the bystander's sneak cleared without a landed hit from the attacker");
			std::cout << "hit with no target: the bystander's sneak cleared on the splash (" << hit + 1 << " ticks)\n";
		}

		// ---- 6. crime guards (not hostile to the squad, no goal 21) attack one member: the idle squadmates join (k16.6.238) ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t thief = rig.add(0, 0.0, 0.0, true, 7);
			const std::uint64_t idle = rig.add(0, 60.0, 0.0, true, 7);
			const std::uint64_t sneaker = rig.add(0, -60.0, 0.0, true, 7);
			const std::uint64_t guard = rig.add(0, 0.0, 30.0, false, 9); // same team, another squad, attacksNeutrals off
			rig.fighter(guard)->faction = 987654; // not the squad's faction, not bonded
			rig.sim->enqueue(Command::fighterSneak(state::Id{sneaker}, true));
			rig.tick(2);
			rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{thief}));
			const int aimed = rig.tickUntil(120, [&]() { return rig.fighter(guard)->target == thief; });
			test::require(aimed >= 0, "the guard never set its target on the thief");
			// 33 frames measured (SQUAD_FOLLOW.md s9) plus margin: ~2 s of 20 Hz ticks.
			const int joined = rig.tickUntil(60, [&]() { return rig.fighter(idle)->target == guard; });
			test::require(joined >= 0, "an idle squadmate did not defend the member the guards attack");
			test::require(rig.fighter(sneaker)->target == 0, "a sneaking squadmate joined the ally's fight");
			std::cout << "guards on one member: the idle squadmate joined " << joined << " ticks later; the sneaker stayed out\n";
		}

		// ---- 7. the same, guards of a faction BONDED (relation 60) to the squad's: the idle member still joins (rwh.148, k16.6.266) ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t thief = rig.add(0, 0.0, 0.0, true, 7);
			const std::uint64_t idle = rig.add(0, 60.0, 0.0, true, 8); // same faction, another squad: the join is faction-wide (rwh.148), and onTargetedBy makes only the victim's squad hostile to the guard's
			const std::uint64_t guard = rig.add(0, 0.0, 30.0, false, 9);
			rig.fighter(guard)->faction = 987654;
			const std::uint64_t squadFaction = 876543; // the rig's fighters have no faction (0, which is never bonded): give the squad one
			rig.fighter(thief)->faction = squadFaction;
			rig.fighter(idle)->faction = squadFaction;
			rig.tick(2); // the bond set is rebuilt from the faction entities when the faction set changes: set ours after that (next rebuild: 10 s)
			const_cast<CombatArena*>(rig.sim->combat())->setBonded({CombatArena::bondKey(squadFaction, 987654), CombatArena::bondKey(987654, squadFaction)});
			test::require(squadFaction != 0, "the squad has no faction: a bond with it cannot exist");
			test::require(rig.sim->combat()->bonded(squadFaction, 987654), "the guards' faction is not bonded to the squad's");
			rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{thief}));
			const int aimed = rig.tickUntil(120, [&]() { return rig.fighter(guard)->target == thief; });
			test::require(aimed >= 0, "the bonded guard never set its target on the thief");
			// The bond makes the guard the idle member's ally too, so once the thief hits back the member may pick the thief: any target is a join.
			const int joined = rig.tickUntil(60, [&]() { return rig.fighter(idle)->target != 0; });
			test::require(joined >= 0, "an idle squadmate did not defend against a bonded-faction attacker (rwh.148: no special case)");
			std::cout << "bonded guards on one member: the idle squadmate joined " << joined << " ticks later\n";
		}

		// ---- 8. a PASSIVE member does not join an ally's fight (rwh.148 3a: goal 15 utility 0), the control does ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t thief = rig.add(0, 0.0, 0.0, true, 7);
			const std::uint64_t passive = rig.add(0, 60.0, 0.0, true, 7);
			const std::uint64_t control = rig.add(0, -60.0, 0.0, true, 7);
			const std::uint64_t guard = rig.add(0, 0.0, 30.0, false, 9);
			rig.fighter(guard)->faction = 987654;
			rig.fighter(passive)->passive = true;
			rig.tick(2);
			rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{thief}));
			const int joined = rig.tickUntil(120, [&]() { return rig.fighter(control)->target == guard; });
			test::require(joined >= 0, "the control squadmate did not join: the passive check proves nothing");
			rig.tick(120);
			test::require(rig.fighter(passive)->target == 0, "a passive squadmate joined its ally's fight");
			std::cout << "passive: stayed out while the control joined " << joined << " ticks after the pick\n";
		}

		// ---- 9. (the holder stands 120 u out: the fight drifts toward it, the guard reached 40 u of a holder at 60 u and the join was then right, kenshi-k16.6.533) a HOLD member does not join an ally's fight, does not chase, and fights an attacker in close range ----
		{
			Rig rig(gd, data, nav);
			const std::uint64_t thief = rig.add(0, 0.0, 0.0, true, 7);
			const std::uint64_t holder = rig.add(0, 120.0, 0.0, true, 7);
			const std::uint64_t guard = rig.add(0, 0.0, 30.0, false, 9);
			rig.fighter(guard)->faction = 987654;
			rig.fighter(holder)->holdPosition = true;
			rig.fighter(holder)->ranged = state::FighterRanged{}; // melee only: out of close range it has nothing to act on
			rig.tick(2);
			rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{thief}));
			const int aimed = rig.tickUntil(120, [&]() { return rig.fighter(guard)->target == thief; });
			test::require(aimed >= 0, "the guard never set its target on the thief");
			rig.tick(120);
			test::require(rig.fighter(holder)->target == 0, "a melee-only holder joined an ally's fight out of close range");
			// An enemy that attacks the holder from 20 u (inside the close range) is fought.
			const std::uint64_t near = rig.add(1, 120.0, 20.0, false);
			rig.sim->enqueue(Command::fighterAttack(state::Id{near}, state::Id{holder}));
			const int answered = rig.tickUntil(240, [&]() { return rig.fighter(holder)->target == near; });
			test::require(answered >= 0, "a holding member did not fight an attacker in close range");
			std::cout << "hold: stayed out of the ally's fight; answered a close attacker " << answered << " ticks later\n";
		}
		// A holder that is attacked from beyond close range does not run to it (no chase): the attacker is 120 u away.
		{
			Rig rig(gd, data, nav);
			const std::uint64_t holder = rig.add(0, 0.0, 0.0, true, 7);
			const std::uint64_t far = rig.add(1, 0.0, 120.0, false);
			rig.fighter(holder)->holdPosition = true;
			rig.fighter(far)->attention = 0.5;
			rig.tick(2);
			rig.sim->enqueue(Command::fighterAttack(state::Id{far}, state::Id{holder}));
			const double z0 = rig.fighter(holder)->z;
			bool engagedFar = false;
			for (int i = 0; i < 40; ++i)
			{
				rig.sim->tick();
				const state::Fighter* h = rig.fighter(holder);
				const state::Fighter* e = rig.fighter(far);
				if (h->target != 0 && std::hypot(h->x - e->x, h->z - e->z) > 60.0)
					engagedFar = true;
			}
			test::require(!engagedFar, "a holding member kept a target beyond close range");
			test::require(std::abs(rig.fighter(holder)->z - z0) < 5.0, "a holding member walked towards a far attacker");
			std::cout << "hold: no target and no walk toward an attacker 120 u away\n";
		}

		// Members stand 150 u out: at 300 u a member never sees the guard (perception), so no join is possible for anyone (kenshi-k16.6.533).
		// ---- 10. HOLD joins an ally's fight (goal 15, rwh.179) but never leaves its anchor: a crossbow fires from there, a melee-only
		// holder does not swing; a non-holder joins and runs; a passive one never joins (kenshi-k16.6.371) ----
		{
			const kdata::Record* crossbow = nullptr;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CROSSBOW")))
				if (r->name == "Eagle's Cross")
					crossbow = r;
			test::require(crossbow != nullptr, "no Eagle's Cross");
			Rig rig(gd, data, nav);
			const std::uint64_t victim = rig.add(0, 0.0, 0.0, true, 7);
			const std::uint64_t shooter = rig.add(0, 150.0, 0.0, true, 7);
			const std::uint64_t melee = rig.add(0, -150.0, 0.0, true, 7);
			const std::uint64_t runner = rig.add(0, 0.0, 150.0, true, 7);
			const std::uint64_t passive = rig.add(0, 0.0, -150.0, true, 7);
			const std::uint64_t guard = rig.add(0, 0.0, 30.0, false, 9);
			rig.fighter(guard)->faction = 987654;
			rig.fighter(shooter)->holdPosition = true;
			rig.fighter(shooter)->ranged = data->makeRanged(*crossbow, 0.5);
			rig.fighter(shooter)->skills.crossbows = 100.0;
			rig.fighter(melee)->holdPosition = true;
			rig.fighter(melee)->ranged = state::FighterRanged{};
			rig.fighter(passive)->passive = true;
			for (const std::uint64_t id : {victim, runner, passive})
				rig.fighter(id)->ranged = state::FighterRanged{};
			test::require(CombatArena::inRangedReach(*rig.fighter(shooter), *rig.fighter(guard)), "the crossbow does not reach 150 u: the test proves nothing");
			rig.tick(2);
			const double sx = rig.fighter(shooter)->x, sz = rig.fighter(shooter)->z;
			const double mx = rig.fighter(melee)->x, mz = rig.fighter(melee)->z;
			rig.sim->enqueue(Command::fighterAttack(state::Id{guard}, state::Id{victim}));
			bool joined = false, fired = false, meleeSwung = false;
			double shooterMoved = 0.0, meleeMoved = 0.0, runnerMoved = 0.0;
			const double rx = rig.fighter(runner)->x, rz = rig.fighter(runner)->z;
			const std::int32_t bolts = rig.fighter(shooter)->ranged.ammo;
			for (int i = 0; i < 600; ++i)
			{
				rig.tick();
				const state::Fighter* h = rig.fighter(shooter);
				joined = joined || (h->target == guard && h->combatGoal == 15);
				fired = fired || h->ranged.ammo < bolts;
				shooterMoved = std::max(shooterMoved, std::hypot(h->x - sx, h->z - sz));
				const state::Fighter* ml = rig.fighter(melee);
				meleeMoved = std::max(meleeMoved, std::hypot(ml->x - mx, ml->z - mz));
				meleeSwung = meleeSwung || ml->target != 0;
				runnerMoved = std::max(runnerMoved, std::hypot(rig.fighter(runner)->x - rx, rig.fighter(runner)->z - rz));
			}
			test::require(joined, "the crossbow holder never took goal 15 on the attacker");
			test::require(fired, "the crossbow holder never fired from its anchor");
			test::require(shooterMoved < 1.0, "the crossbow holder left its anchor: " + std::to_string(shooterMoved));
			test::require(!meleeSwung && meleeMoved < 1.0, "the melee-only holder joined or moved while the attacker stayed out of reach");
			test::require(runnerMoved > 20.0, "the non-holder control did not run to the fight");
			test::require(rig.fighter(passive)->target == 0, "a passive squadmate joined");
			std::cout << "hold join: crossbow holder joined+fired, moved " << shooterMoved << " u; melee holder moved " << meleeMoved
				<< " u; control ran " << runnerMoved << " u\n";
		}

		std::cout << "ksim_sneak_break: all checks passed\n";
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "ksim_sneak_break: " << e.what() << "\n";
		return 1;
	}
}
