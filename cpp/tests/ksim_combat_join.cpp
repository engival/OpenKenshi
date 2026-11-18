// The combat-joining goals (task CF; kinspect dt24_143 / dt24_15 / dt24_16), on a bare arena and with NO NPC order anywhere: a
// package-driven character joins a fight because
//  - a squadmate is fighting (143 PROTECT_OWN_SQUAD: resolver 14099d510, plan 4 -> 13 -> 143),
//  - an ally is fighting (15 PROTECT_ALLIES: resolver 14099ca10, plan 4 -> 13 -> 15),
//  - an enemy stands in sight (16 ATTACK_ENEMIES: resolver 14059f3a0, plan 4 -> 16).
// The hand the exe's resolvers return is the fighting member / ally; its attacker is the fighter we walk to (task 13). The goal a
// pick runs is the row's task type, which the arena reports as a Goal event (the fixture reads every pick, because the joined
// attacker can answer in the same tick and the joiner then runs SELF_PRESERVATION 32 - the exe's outranking, not a failure).

#include "ksim/combat.hpp"
#include "ksim/join_rules.hpp"
#include "ksim/perception.hpp"
#include "ksim/routines.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr std::uint64_t kMyFaction = 10;
constexpr std::uint64_t kFoeFaction = 20;
constexpr std::uint64_t kNeutralFaction = 30; // relation 0 both ways: the sensor's kNeutral bit
// The slavemaster's faction for the 41 fixture (dt24_41): hostile toward kFoeFaction, nothing else.
constexpr std::uint64_t kMasterFaction = 40;

// A seen record the resolvers accept: aware and sensed by the last scan (a `notice` alone leaves sinceSensed stale, the kNotSeen bit).
void seen(state::Fighter& observer, std::uint64_t target)
{
	perception::notice(observer, target);
	state::SightRecord* record = const_cast<state::SightRecord*>(perception::find(observer, target));
	test::require(record != nullptr, "no sight record after notice");
	record->sinceSensed = 0.0;
}

struct Rig
{
	const kdata::GameData& gd;
	std::shared_ptr<const CombatData> data;
	CombatArena arena;
	const kdata::Record* servant = nullptr;
	std::uint64_t rng = 11;
	std::uint64_t nextId = 1;
	std::vector<std::pair<std::uint64_t, int>> picks; // (who, goal) of every target pick

	Rig(const kdata::GameData& g, std::shared_ptr<const CombatData> d, std::uint64_t seed)
		: gd(g)
		, data(std::move(d))
		, arena(data, CombatSettings{}, seed)
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "no Escaped Servant");
		// Two factions, hostile both ways: the 16 resolver's kHostile bit is the faction relation (sensor::enemy), not the team.
		sensor::Context ctx;
		ctx.relation = [](std::uint64_t from, std::uint64_t to)
		{
			if ((from == kMyFaction && to == kFoeFaction) || (from == kFoeFaction && to == kMyFaction))
				return -100.0;
			if (from == kMasterFaction && to == kFoeFaction) // the slavemaster's own view: the 41 fixture
				return -100.0;
			return 0.0;
		};
		arena.setSensorContext(ctx);
		arena.setObserver([this](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::Goal)
				picks.push_back({e.who, static_cast<int>(e.code)});
		});
	}

	std::uint64_t spawn(double x, double z, std::uint64_t faction, std::uint64_t squad, int team, bool player = false)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.x = x;
		f.z = z;
		f.yaw = 0.0;
		f.faction = faction;
		f.squad = squad;
		f.team = team;
		f.playerControlled = player;
		const std::uint64_t id = nextId++;
		arena.add(id, std::move(f));
		return id;
	}

	state::Fighter& me(std::uint64_t id) { return *arena.find(id); }

	// A town's circle (the exe's Town object, task CF batch 2): goal 34's "my town" and goal 144's home ring.
	void town(double x, double z, double radius) { arena.environmentMutable().towns.push_back({x, z, radius}); }

	void fight(std::uint64_t a, std::uint64_t b)
	{
		me(a).target = b;
		me(b).target = a;
		seen(me(a), b);
		seen(me(b), a);
	}

	void run(int steps)
	{
		for (int i = 0; i < steps; ++i)
			arena.step(kDt);
	}

	// The first goal `who` picked, or 0.
	int firstPick(std::uint64_t who) const
	{
		for (const auto& pick : picks)
			if (pick.first == who)
				return pick.second;
		return 0;
	}
};

// 143: A and B are one squad; B is fighting a bandit; A carries only PROTECT_OWN_SQUAD and joins B's attacker.
void testProtectOwnSquad()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 3);
	const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
	const std::uint64_t b = rig.spawn(20.0, 0.0, kMyFaction, 7, 0);
	const std::uint64_t bandit = rig.spawn(30.0, 0.0, kFoeFaction, 9, 1);
	rig.fight(b, bandit);
	seen(rig.me(a), b); // the resolver reads A's own sensor list: the squadmate must be seen
	seen(rig.me(a), bandit); // and the picker's awareness of the candidate
	test::require(rig.arena.setJoinGoals(a, true, joins::kProtectSquad), "no member A");
	rig.run(3);
	test::require(rig.me(a).target == bandit, "143: A did not join B's fight");
	test::require(rig.firstPick(a) == 143, "143: A's first pick was goal " + std::to_string(rig.firstPick(a))
		+ ", not PROTECT_OWN_SQUAD");
}

// BQ18 (kinspect findings/bq18_leftover_bandits.md; patrol_aggro_town.md a8a41b7): 143 joins on the JOINER's own sight record of the
// fighting squadmate, not on a squad-wide flag. An idle squadmate with no record of a fight inside a building stays; the same fight in
// its view draws it in with 143. That record is the perception gate behind the bar fixture's "7 of 8 join, 1 outside never does".
void testProtectOwnSquadNeedsTheRecord()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	{
		// The fight inside: A knows the bandit (it has the doorway in view) but holds no record of B, behind the wall.
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 29);
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t b = rig.spawn(200.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t bandit = rig.spawn(220.0, 0.0, kFoeFaction, 9, 1);
		rig.fight(b, bandit);
		seen(rig.me(a), bandit); // the candidate is aware to A...
		test::require(perception::find(rig.me(a), b) == nullptr, "BQ18: the fixture left a record of B");
		test::require(rig.arena.setJoinGoals(a, true, joins::kProtectSquad), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == 0, "BQ18: a squadmate with no record of the fight joined it");
	}
	{
		// The same fight out in the open: A sees B fighting, and 143 hands it B's attacker.
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 31);
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t b = rig.spawn(20.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t bandit = rig.spawn(30.0, 0.0, kFoeFaction, 9, 1);
		rig.fight(b, bandit);
		seen(rig.me(a), b);
		seen(rig.me(a), bandit);
		test::require(rig.arena.setJoinGoals(a, true, joins::kProtectSquad), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == bandit, "BQ18: A did not join the fight in its view");
		test::require(rig.firstPick(a) == 143, "BQ18: A's first pick was goal " + std::to_string(rig.firstPick(a)) + ", not PROTECT_OWN_SQUAD");
	}
}

// 101 DEFEAT_SQUAD / 248 DEFEAT_SQUAD_LIMIT_CHASE_RANGE (kinspect dt24_101, resolver 140793eb0): the squad of my current target (A, a foe of my faction). B is neutral
// to me (so 16 admits nothing) but shares squad 9 with A, whom I already hold; 101 admits it. Another squad is not admitted; 248 is the
// same with the precondition iAmBeingMeleeAttacked = 0 (a member targeting me within reach blocks it) and the chase-limit lock pair.
void testDefeatSquad()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const auto fixture = [&](Rig& rig, std::uint16_t bits, std::uint64_t squadOfB, std::uint64_t& me, std::uint64_t& b)
	{
		me = rig.spawn(0.0, 0.0, kMasterFaction, 7, 0);
		const std::uint64_t a = rig.spawn(60.0, 0.0, kFoeFaction, 9, 1);
		b = rig.spawn(10.0, 0.0, kNeutralFaction, squadOfB, 1);
		seen(rig.me(me), a);
		seen(rig.me(me), b);
		rig.me(a).unconscious = true; // my target is down: the squad it belonged to is what is left to defeat
		rig.me(a).koTimer = 600.0;
		rig.me(me).target = a;
		test::require(rig.arena.setJoinGoals(me, true, bits), "no member me");
	};
	{
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 61);
		std::uint64_t me = 0, b = 0;
		fixture(rig, joins::kDefeatSquad, 9, me, b);
		rig.run(1);
		test::require(rig.me(me).target == b, "101: the squadmate of my target was not taken");
		test::require(rig.firstPick(me) == 101, "101: the first pick was goal " + std::to_string(rig.firstPick(me)));
	}
	{
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 62);
		std::uint64_t me = 0, b = 0;
		fixture(rig, joins::kDefeatSquad, 11, me, b);
		rig.run(3);
		test::require(rig.me(me).target != b, "101: a member of another squad was taken");
	}
	{
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 63);
		std::uint64_t me = 0, b = 0;
		fixture(rig, 0, 9, me, b);
		rig.run(3);
		test::require(rig.me(me).target != b, "101: taken without the goal in the package");
	}
	{
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 64);
		std::uint64_t me = 0, b = 0;
		fixture(rig, joins::kDefeatSquadChase, 9, me, b);
		rig.run(3);
		test::require(rig.me(me).target == b, "248: the squadmate of my target was not taken");
		test::require(rig.firstPick(me) == 248, "248: the first pick was goal " + std::to_string(rig.firstPick(me)));
	}
	{
		// iAmBeingMeleeAttacked: someone in reach targets me, so 248 has no plan (101 would not care).
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 65);
		std::uint64_t me = 0, b = 0;
		fixture(rig, joins::kDefeatSquadChase, 9, me, b);
		const std::uint64_t attacker = rig.spawn(3.0, 0.0, kFoeFaction, 13, 1);
		rig.me(attacker).target = me;
		rig.run(1);
		test::require(rig.firstPick(me) != 248, "248: picked while being melee attacked");
	}
	test::require(routines::commitOf(routines::task::DefeatSquadLimitChase).base == 0.25
		&& routines::commitOf(routines::task::DefeatSquadLimitChase).span == 2.0, "248: the chase-limit pair is not the commit lock");
	test::require(routines::commitOf(routines::task::DefeatSquad).base == 0.0, "101: has no chase-limit pair");
	std::cout << "  defeat squad: 101 / 248 take a squadmate of the current target\n";
}

// 41 ATTACK_ENEMIES_OF_MY_SLAVEMASTER (task raid-goals; kinspect dt24_41, resolver 14059ee50): a slave does not fight its own enemies
// only - it takes the enemies of its slavemaster (dt24_41's port note: the master's relations, not the slave's). The fixture's slave is
// NEUTRAL to the candidate (kNeutralFaction vs kFoeFaction: no relation), so 16 admits nothing and only 41 can make the pick. The
// candidates are admitted like 16's (the picker chooses): the platoon test needs another squad, a fellow slave of the same master is
// spared, and a fighter the master is not hostile to is not a target.
void testSlavemasterEnemies()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	// The data: AI package 2672 (Patrol map, short range) carries 41 in its SLAVE list, open all day - the row a slave of its squad runs.
	{
		const routines::PackageTable table(gd);
		const routines::CompiledPackage* package = table.find("2672-gamedata.base");
		test::require(package != nullptr, "no AI package 2672");
		test::require(routines::goalHolds(*package, routines::Role::Slave, routines::task::AttackSlavemasterEnemies, 12.0),
			"41: package 2672's slave list does not carry ATTACK_ENEMIES_OF_MY_SLAVEMASTER");
	}
	struct Slave
	{
		Rig rig;
		Slave(const kdata::GameData& g, std::uint64_t seed) : rig(g, CombatData::build(g, KENSHI_DATA), seed) {}
		// A slave of the master faction: the cuff's own fields (slaveState 1, captorSid the master; the sim resolves captorFaction).
		std::uint64_t spawnSlave(double x, std::uint64_t captor)
		{
			const std::uint64_t id = rig.spawn(x, 0.0, kNeutralFaction, 7, 0);
			rig.me(id).slaveState = 1;
			rig.me(id).captorSid = "master";
			rig.me(id).captorFaction = captor;
			return id;
		}
	};
	{
		// The master's enemy in sight, in another squad: the slave takes it under 41.
		Slave s(gd, 37);
		const std::uint64_t slave = s.spawnSlave(0.0, kMasterFaction);
		const std::uint64_t bandit = s.rig.spawn(20.0, 0.0, kFoeFaction, 9, 1);
		seen(s.rig.me(slave), bandit);
		test::require(s.rig.arena.setJoinGoals(slave, true, joins::kMasterEnemies), "no member slave");
		s.rig.run(3);
		test::require(s.rig.me(slave).target == bandit, "41: the slave did not attack its slavemaster's enemy");
		test::require(s.rig.firstPick(slave) == 41, "41: the slave's first pick was goal "
			+ std::to_string(s.rig.firstPick(slave)) + ", not ATTACK_ENEMIES_OF_MY_SLAVEMASTER");
	}
	{
		// No master (no captor resolved): the goal has no hand and nothing is picked.
		Slave s(gd, 41);
		const std::uint64_t free = s.spawnSlave(0.0, 0);
		const std::uint64_t bandit = s.rig.spawn(20.0, 0.0, kFoeFaction, 9, 1);
		seen(s.rig.me(free), bandit);
		test::require(s.rig.arena.setJoinGoals(free, true, joins::kMasterEnemies), "no member slave");
		s.rig.run(3);
		test::require(s.rig.me(free).target == 0, "41: a slave with no captor took a target");
	}
	{
		// A fellow slave of the same master is not an enemy of the master's enemies: not taken.
		Slave s(gd, 43);
		const std::uint64_t slave = s.spawnSlave(0.0, kMasterFaction);
		const std::uint64_t fellow = s.rig.spawn(20.0, 0.0, kFoeFaction, 9, 1);
		s.rig.me(fellow).slaveState = 1;
		s.rig.me(fellow).captorFaction = kMasterFaction;
		seen(s.rig.me(slave), fellow);
		test::require(s.rig.arena.setJoinGoals(slave, true, joins::kMasterEnemies), "no member slave");
		s.rig.run(3);
		test::require(s.rig.me(slave).target == 0, "41: the slave attacked a fellow slave of its master");
	}
	{
		// Not a slave (state 0), the same master-enemy in sight: the goal does not apply.
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 47);
		const std::uint64_t freeman = rig.spawn(0.0, 0.0, kNeutralFaction, 7, 0);
		const std::uint64_t bandit = rig.spawn(20.0, 0.0, kFoeFaction, 9, 1);
		seen(rig.me(freeman), bandit);
		test::require(rig.arena.setJoinGoals(freeman, true, joins::kMasterEnemies), "no member freeman");
		rig.run(3);
		test::require(rig.me(freeman).target == 0, "41: a character that is no slave used ATTACK_ENEMIES_OF_MY_SLAVEMASTER");
	}
	{
		// A candidate the master is not hostile to (no relation): not an "enemy of my slavemaster".
		Slave s(gd, 53);
		const std::uint64_t slave = s.spawnSlave(0.0, kMasterFaction);
		const std::uint64_t drifter = s.rig.spawn(20.0, 0.0, kNeutralFaction, 9, 1);
		seen(s.rig.me(slave), drifter);
		test::require(s.rig.arena.setJoinGoals(slave, true, joins::kMasterEnemies), "no member slave");
		s.rig.run(3);
		test::require(s.rig.me(slave).target == 0, "41: the slave attacked a fighter its slavemaster is neutral to");
	}
}

// 15: an ally of my faction, another squad, is fighting; A carries only PROTECT_ALLIES and joins its attacker.
void testProtectAllies()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 5);
	const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
	const std::uint64_t ally = rig.spawn(20.0, 0.0, kMyFaction, 9, 0); // another squad: 143 cannot apply
	const std::uint64_t bandit = rig.spawn(30.0, 0.0, kFoeFaction, 9, 1, true);
	rig.fight(ally, bandit);
	seen(rig.me(a), ally);
	seen(rig.me(a), bandit);
	test::require(rig.arena.setJoinGoals(a, true, joins::kProtectAllies), "no member A");
	rig.run(3);
	test::require(rig.me(a).target == bandit, "15: A did not join its ally's fight");
	test::require(rig.firstPick(a) == 15, "15: A's first pick was goal " + std::to_string(rig.firstPick(a))
		+ ", not PROTECT_ALLIES");
}

// k16.6.269 (rwh139 2): a member running a follow order does not take goal 15 while the order runs; the same member with the order
// ended does. (The faction-wide join, another squad, is testProtectAllies above: the ally is in squad 9, A in squad 7.)
void testFollowBlocksProtectAllies()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 5);
	const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0, true);
	const std::uint64_t leader = rig.spawn(-30.0, 0.0, kMyFaction, 7, 0, true);
	const std::uint64_t ally = rig.spawn(20.0, 0.0, kMyFaction, 9, 0);
	const std::uint64_t bandit = rig.spawn(30.0, 0.0, kFoeFaction, 9, 1);
	rig.fight(ally, bandit);
	seen(rig.me(a), ally);
	seen(rig.me(a), bandit);
	rig.me(a).followTarget = leader;
	test::require(rig.arena.setJoinGoals(a, true, joins::kProtectAllies), "no member A");
	rig.run(3);
	test::require(rig.firstPick(a) != 15, "15: a following member took PROTECT_ALLIES");
	CombatArena::endFollow(rig.me(a));
	rig.run(3);
	test::require(rig.firstPick(a) == 15 || rig.me(a).target == bandit, "15: the member did not join once the follow ended");
}

// 16: a hostile fighter in sight, nothing else going on; A carries only ATTACK_ENEMIES and takes it on sight.
void testAttackEnemies()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 7);
	const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
	const std::uint64_t bandit = rig.spawn(20.0, 0.0, kFoeFaction, 9, 1);
	rig.me(a).yaw = 1.5707963267948966; // facing +x, at the bandit
	seen(rig.me(a), bandit);
	test::require(rig.arena.setJoinGoals(a, true, joins::kAttackEnemies), "no member A");
	rig.run(3);
	test::require(rig.me(a).target == bandit, "16: A did not attack the enemy in sight");
	test::require(rig.firstPick(a) == 16, "16: A's first pick was goal " + std::to_string(rig.firstPick(a))
		+ ", not ATTACK_ENEMIES");
}

// 21: a neutral faction (`kNeutral`), nothing else going on; A carries only ATTACK_ENEMIES_AND_NEUTRALS and takes it on sight.
void testAttackNeutrals()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 9);
	const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
	const std::uint64_t drifter = rig.spawn(20.0, 0.0, kNeutralFaction, 9, 0); // team 0 like A: only the sensor's neutral bit
	rig.me(a).yaw = 1.5707963267948966;
	seen(rig.me(a), drifter);
	test::require(rig.arena.setJoinGoals(a, true, joins::kAttackNeutrals), "no member A");
	rig.run(3);
	test::require(rig.me(a).target == drifter, "21: A did not attack the neutral in sight");
	test::require(rig.firstPick(a) == 21, "21: A's first pick was goal " + std::to_string(rig.firstPick(a))
		+ ", not ATTACK_ENEMIES_AND_NEUTRALS");
}

// 34: A and a neutral run through the same town; A carries only ATTACK_TROUBLE_MAKERS. The neutral is the troublemaker because it runs
// an attack task on a third fighter; a neutral that does nothing is not picked.
void testTroubleMakers()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	{
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 11);
		rig.town(0.0, 0.0, 300.0);
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t bystander = rig.spawn(40.0, 0.0, kNeutralFaction, 10, 0);
		const std::uint64_t troublemaker = rig.spawn(20.0, 0.0, kNeutralFaction, 10, 0);
		rig.me(troublemaker).target = bystander; // it runs an attack task: goal 34's criminal test
		seen(rig.me(a), bystander);
		seen(rig.me(a), troublemaker);
		test::require(rig.arena.setJoinGoals(a, true, joins::kTroubleMakers), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == troublemaker, "34: A did not join the troublemaker in its town");
		test::require(rig.firstPick(a) == 34, "34: A's first pick was goal " + std::to_string(rig.firstPick(a))
			+ ", not ATTACK_TROUBLE_MAKERS");
	}
	{
		// The same pair, but the neutral does nothing: it is not a troublemaker, and A has nothing to join.
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 13);
		rig.town(0.0, 0.0, 300.0);
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t idle = rig.spawn(20.0, 0.0, kNeutralFaction, 10, 0);
		seen(rig.me(a), idle);
		test::require(rig.arena.setJoinGoals(a, true, joins::kTroubleMakers), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == 0, "34: A joined a neutral that is no troublemaker");
	}
}

// 144: A stands in its town and carries only TERRITORIAL_AGGRESSION. A hostile inside the home ring (radius x1.3) is taken; one far
// outside is not - unless it is already fighting A (the leash exception, OURS).
void testTerritorial()
{
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	{
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 17);
		rig.town(0.0, 0.0, 100.0); // the ring is 130 u
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t near = rig.spawn(20.0, 0.0, kFoeFaction, 9, 1);
		seen(rig.me(a), near);
		test::require(rig.arena.setJoinGoals(a, true, joins::kTerritorial), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == near, "144: A did not take the hostile inside its home ring");
		test::require(rig.firstPick(a) == 144, "144: A's first pick was goal " + std::to_string(rig.firstPick(a))
			+ ", not TERRITORIAL_AGGRESSION_BUT_DONT_LEAVE_HOME");
	}
	{
		// Outside the ring, not fighting A: no pick.
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 19);
		rig.town(0.0, 0.0, 100.0);
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t far = rig.spawn(200.0, 0.0, kFoeFaction, 9, 1);
		seen(rig.me(a), far);
		test::require(rig.arena.setJoinGoals(a, true, joins::kTerritorial), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == 0, "144: A left its home ring for a hostile that is not fighting it");
	}
	{
		// Outside the ring but fighting A: the leash exception keeps it.
		Rig rig(gd, CombatData::build(gd, KENSHI_DATA), 23);
		rig.town(0.0, 0.0, 100.0);
		const std::uint64_t a = rig.spawn(0.0, 0.0, kMyFaction, 7, 0);
		const std::uint64_t far = rig.spawn(200.0, 0.0, kFoeFaction, 9, 1);
		rig.me(far).target = a; // it already fights A
		seen(rig.me(a), far);
		test::require(rig.arena.setJoinGoals(a, true, joins::kTerritorial), "no member A");
		rig.run(3);
		test::require(rig.me(a).target == far, "144: the leash exception did not keep the attacker outside the ring");
	}
}

} // namespace

int main()
{
	try
	{
		testProtectOwnSquad();
		testProtectOwnSquadNeedsTheRecord();
		testSlavemasterEnemies();
		testDefeatSquad();
		testProtectAllies();
		testFollowBlocksProtectAllies();
		testAttackEnemies();
		testAttackNeutrals();
		testTroubleMakers();
		testTerritorial();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_combat_join OK\n";
	return 0;
}
