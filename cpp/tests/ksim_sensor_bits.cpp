// G5 sensor list (task BU): the record flags of kinspect s59_static.md section 1 on a staged cast, the masks' iterator (140853960), and
// the 167 capture resolver reading the sensor list (capture_target.hpp): the ally and dead mask bits, the per-body tests, a remembered
// body (bit 8 not excluded) and the memory's expiry. Pure: a relation table stands in for the faction layer, no game data and no world.

#include "ksim/perception.hpp"
#include "ksim/sensor.hpp"

#include "../src/ksim/capture_target.hpp"

#include "test_util.hpp"

#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ksim;

// The cast's factions.
constexpr std::uint64_t kMine = 100; // the observer's
constexpr std::uint64_t kPlayer = 200;
constexpr std::uint64_t kFriend = 300;
constexpr std::uint64_t kNeutral = 400;
constexpr std::uint64_t kEnemy = 500;
constexpr std::uint64_t kHatedByMe = 600;
constexpr std::uint64_t kBandit = 700; // a Dust Bandit's stand-in: -10 at the player, the player's map at it -100 (never read)
constexpr std::uint64_t kRaider = 800; // a Bloodraider's stand-in: -100 at the player

sensor::Context makeContext()
{
	std::map<std::pair<std::uint64_t, std::uint64_t>, double> table = {
		{{kMine, kFriend}, 60.0}, // my view: >= 50, an ally
		{{kFriend, kMine}, 60.0},
		{{kEnemy, kMine}, -100.0}, // their view: <= -30, an enemy of mine
		{{kMine, kEnemy}, 100.0}, // ... though I like them: dt11 reads THEIR view
		{{kMine, kHatedByMe}, -100.0}, // I hate them...
		{{kHatedByMe, kMine}, 0.0}, // ... and they are neutral towards me: not an enemy (dt11)
		{{kBandit, kPlayer}, -10.0}, // a Dust Bandit's own view of the player (its default)
		{{kPlayer, kBandit}, -100.0}, // the Nameless entry - never read by isEnemy (dt21)
		{{kRaider, kPlayer}, -100.0}, // a Bloodraider's default
		{{kPlayer, kRaider}, 0.0}, // the player map has no entry: still an enemy, by the raider's record (dt21)
	};
	sensor::Context ctx;
	ctx.playerFaction = kPlayer;
	ctx.relation = [table](std::uint64_t from, std::uint64_t to)
	{
		const auto found = table.find({from, to});
		return found != table.end() ? found->second : 0.0;
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

// A record a scan just wrote: seen and freshly sensed (bit 8 clear).
state::SightRecord scannedRecord()
{
	state::SightRecord record;
	record.seen = true;
	record.sinceSeen = 0.0;
	record.sinceSensed = 0.0;
	return record;
}

std::uint32_t of(const sensor::Context& ctx, const state::Fighter& observer, const state::Fighter& target,
	const state::SightRecord& record = scannedRecord())
{
	return sensor::flags(ctx, observer, target, record);
}

std::string mask(std::uint32_t bits)
{
	return " (bits 0x" + [bits] { char buffer[8]; std::snprintf(buffer, sizeof buffer, "%04x", bits); return std::string(buffer); }() + ")";
}

void testFactionBits()
{
	const sensor::Context ctx = makeContext();
	const state::Fighter observer = fighter(1, kMine, 0.0);

	const state::Fighter friendBody = fighter(2, kFriend, 10.0);
	const std::uint32_t ally = of(ctx, observer, friendBody);
	test::require((ally & sensor::kAlly) != 0, "an ally" + mask(ally));
	test::require((ally & sensor::kNeutral) == 0 && (ally & sensor::kHostile) == 0, "an ally is neither neutral nor hostile" + mask(ally));
	test::require((ally & sensor::kSameFaction) == 0, "another faction" + mask(ally));

	const std::uint32_t same = of(ctx, observer, fighter(3, kMine, 10.0));
	test::require((same & sensor::kAlly) != 0 && (same & sensor::kSameFaction) != 0, "the same faction is an ally and carries bit 10" + mask(same));

	const std::uint32_t neutral = of(ctx, observer, fighter(4, kNeutral, 10.0));
	test::require((neutral & sensor::kNeutral) != 0, "a neutral" + mask(neutral));
	test::require((neutral & (sensor::kAlly | sensor::kHostile | sensor::kSameFaction)) == 0, "no ally or hostile bit" + mask(neutral));

	// dt21's direction: an NPC reads its OWN faction's relation toward the target.
	const std::uint32_t hostile = of(ctx, observer, fighter(6, kHatedByMe, 10.0));
	test::require((hostile & sensor::kHostile) != 0, "an enemy: my faction counts the target <= -30" + mask(hostile));
	test::require((hostile & (sensor::kAlly | sensor::kNeutral)) == 0, "not ally or neutral" + mask(hostile));
	const std::uint32_t oneWay = of(ctx, observer, fighter(5, kEnemy, 10.0));
	test::require((oneWay & sensor::kHostile) == 0 && (oneWay & sensor::kAlly) != 0,
		"the target's -100 at me does not make an enemy: an NPC reads its own view (mine is +100, an ally)" + mask(oneWay));
	// dt21, the player's side: PlayerFactionRelations forwards the lookup to the target faction's record, so a player <-> NPC pair reads
	// the NPC's view in both orders. The live values (2026-11-07): Dust Bandits -10 at Nameless, Bloodraiders -100.
	const state::Fighter playerObserver = fighter(20, kPlayer, 0.0);
	const std::uint32_t banditSeesPlayer = of(ctx, fighter(21, kBandit, 0.0), playerObserver);
	test::require((banditSeesPlayer & sensor::kHostile) == 0 && (banditSeesPlayer & sensor::kNeutral) != 0,
		"a Dust Bandit observer does not find the player an enemy (its own -10)" + mask(banditSeesPlayer));
	const std::uint32_t playerSeesBandit = of(ctx, playerObserver, fighter(22, kBandit, 10.0));
	test::require((playerSeesBandit & sensor::kHostile) == 0 && (playerSeesBandit & sensor::kNeutral) != 0,
		"a player observer does not find a Dust Bandit an enemy (the bandit's -10, not Nameless's -100)" + mask(playerSeesBandit));
	const std::uint32_t raiderSeesPlayer = of(ctx, fighter(23, kRaider, 0.0), playerObserver);
	test::require((raiderSeesPlayer & sensor::kHostile) != 0, "a Bloodraider observer finds the player an enemy (its own -100)" + mask(raiderSeesPlayer));
	const std::uint32_t playerSeesRaider = of(ctx, playerObserver, fighter(24, kRaider, 10.0));
	test::require((playerSeesRaider & sensor::kHostile) != 0, "a player observer finds a Bloodraider an enemy (the raider's -100)" + mask(playerSeesRaider));

	// A personal type-4 tag (dt11 step 2): the observer's own list.
	state::Fighter tagged = fighter(7, kNeutral, 10.0);
	state::Fighter tagger = observer;
	tagger.hostileSquads.push_back(tagged.squad);
	const std::uint32_t personal = of(ctx, tagger, tagged);
	test::require((personal & sensor::kHostile) != 0, "a personal enemy tag" + mask(personal));

	const std::uint32_t playerBody = of(ctx, observer, fighter(8, kPlayer, 10.0));
	test::require((playerBody & sensor::kPlayerFaction) != 0, "a player faction member" + mask(playerBody));
}

void testStateBits()
{
	const sensor::Context ctx = makeContext();
	const state::Fighter observer = fighter(1, kMine, 0.0);

	state::Fighter down = fighter(2, kNeutral, 10.0);
	down.down = true;
	const std::uint32_t downBits = of(ctx, observer, down);
	test::require((downBits & sensor::kDowned) != 0, "a down crawler" + mask(downBits));
	test::require((downBits & sensor::kIncapacitated) == 0, "a conscious crawler is not incapacitated" + mask(downBits));

	state::Fighter ko = fighter(3, kNeutral, 10.0);
	ko.unconscious = true;
	const std::uint32_t koBits = of(ctx, observer, ko);
	test::require((koBits & (sensor::kDowned | sensor::kIncapacitated)) != 0, "a KO" + mask(koBits));

	state::Fighter dead = fighter(4, kEnemy, 10.0);
	dead.dead = true;
	state::SightRecord seenRecord;
	test::require(of(ctx, observer, dead, seenRecord) == sensor::kDead, "a dead body's record carries only bit 4");

	state::Fighter slave = fighter(5, kNeutral, 10.0);
	slave.slaveState = 1;
	slave.down = true;
	test::require((of(ctx, observer, slave) & sensor::kSlave) != 0, "slave state 1");
	state::Fighter escapee = fighter(6, kNeutral, 10.0);
	escapee.slaveState = 2;
	test::require((of(ctx, observer, escapee) & sensor::kEscapingSlave) != 0, "slave state 2");

	state::Fighter carried = fighter(7, kNeutral, 10.0);
	carried.carriedBy = 9;
	const std::uint32_t carriedBits = of(ctx, observer, carried);
	test::require((carriedBits & sensor::kCarried) != 0 && (carriedBits & sensor::kIncapacitated) != 0,
		"a carried character is carried and incapacitated" + mask(carriedBits));

	state::Fighter bed = fighter(8, kNeutral, 10.0);
	bed.asleep = true;
	test::require((of(ctx, observer, bed) & sensor::kInObject) != 0, "a character in a bed is in an object");
	state::Fighter caged = fighter(9, kNeutral, 10.0);
	caged.cageBuilding = 77;
	test::require((of(ctx, observer, caged) & sensor::kInObject) != 0, "a caged character is in an object");

	state::Fighter law = fighter(10, kNeutral, 10.0);
	law.npcClass = crime::kClassLaw;
	test::require((of(ctx, observer, law) & sensor::kLawMilitary) != 0, "a law enforcer");

	// Bit 8: remembered only (the last scan did not sense it).
	state::SightRecord fresh;
	fresh.sinceSensed = 0.0;
	state::SightRecord remembered;
	remembered.sinceSensed = 3.0;
	const state::Fighter plain = fighter(11, kNeutral, 10.0);
	test::require((of(ctx, observer, plain, fresh) & sensor::kNotSeen) == 0, "a freshly sensed record is not 'not seen'");
	test::require((of(ctx, observer, plain, remembered) & sensor::kNotSeen) != 0, "a remembered-only record carries bit 8");

	// The undecoded bits (robot 11, job target 12) are never set.
	for (std::uint32_t bits : {downBits, koBits, of(ctx, observer, slave)})
		test::require((bits & (sensor::kRobot | sensor::kJobTarget)) == 0, "bits 11 and 12 are never set" + mask(bits));
}

// One scanned scene: the observer's sensor list holds every body below (seen, fresh).
struct Scene
{
	sensor::Context ctx = makeContext();
	state::Fighter observer;
	std::map<std::uint64_t, state::Fighter> bodies;

	Scene()
		: observer(fighter(1, kMine, 0.0))
	{
		bodies.emplace(2, fighter(2, kNeutral, 15.0)); // down: the nearest catch
		bodies.emplace(3, fighter(3, kFriend, 5.0)); // down, but an ally: mask exclude bit 0
		bodies.emplace(4, fighter(4, kEnemy, 8.0)); // dead: mask exclude bit 4
		bodies.emplace(5, fighter(5, kNeutral, 10.0)); // a down slave: per-body test 7
		bodies.emplace(6, fighter(6, kNeutral, 30.0)); // a KO: the next catch
		bodies.emplace(7, fighter(7, kNeutral, 3.0)); // carried
		bodies.emplace(8, fighter(8, kNeutral, 4.0)); // in a bed
		bodies.emplace(9, fighter(9, kNeutral, 100.0)); // down, past the catcher's sight
		bodies.emplace(10, fighter(10, kMine, 6.0)); // down, same faction
		bodies.at(2).down = true;
		bodies.at(3).down = true;
		bodies.at(4).dead = true;
		bodies.at(5).down = true;
		bodies.at(5).slaveState = 1;
		bodies.at(6).unconscious = true;
		bodies.at(7).carriedBy = 20;
		bodies.at(8).asleep = true;
		bodies.at(9).down = true;
		bodies.at(10).down = true;
		for (const auto& [id, body] : bodies)
		{
			state::SightRecord record = scannedRecord();
			record.target = id;
			observer.sight.push_back(record);
		}
	}
};

void testMasksAndResolver()
{
	Scene scene;
	// The mask's iterator: bits 3 or 14 in, bits 0 and 4 out. The dead, the allies (bit 0) and the same faction are gone; the per-body
	// tests are the resolver's, not the iterator's, so the slave, the carried and the bed are still listed.
	std::vector<std::uint64_t> passed;
	sensor::sensed(scene.observer, capture::kInclude, capture::kExclude, scene.ctx,
		[&](std::uint64_t id) { return &scene.bodies.at(id); },
		[&](std::uint64_t id, const state::Fighter&, std::uint32_t) { passed.push_back(id); });
	test::require(passed == std::vector<std::uint64_t>({2, 5, 6, 7, 9}),
		"the masks pass the down, KO, slave, carried and far bodies only (got " + std::to_string(passed.size()) + ")");

	// The resolver on the sensor list: nearest, the per-body tests, the claimed callback and the sight cap.
	auto lookup = [&](std::uint64_t id) -> const state::Fighter* { return &scene.bodies.at(id); };
	auto none = [](std::uint64_t) { return false; };
	test::require(capture::nearest(1, scene.observer, scene.ctx, 60.0, lookup, none) == 2, "the nearest down body");
	auto claimTwo = [](std::uint64_t id) { return id == 2; };
	test::require(capture::nearest(1, scene.observer, scene.ctx, 60.0, lookup, claimTwo) == 6,
		"a claimed body is skipped for the next one");
	test::require(capture::nearest(1, scene.observer, scene.ctx, 20.0, lookup, claimTwo) == 0,
		"the cap: the next body is 30 units out");
	// Both choices are skip proofs: 3 (ally, 5 u) and 10 (same faction, 6 u) are out by mask bit 0; 7 (carried, 3 u) and 5 (a slave,
	// 10 u) by the per-body tests. All are nearer than 2 (15 u) or 6 (30 u).
}

void testMemory()
{
	// An empty scene, then perception's own scan: a down body in plain sight, LOS lost, the memory, the expiry.
	const sensor::Context ctx = makeContext();
	const std::uint64_t observerId = 1;
	const std::uint64_t targetId = 2;
	state::Fighter observer = fighter(observerId, kMine, 0.0);
	state::Fighter target = fighter(targetId, kNeutral, 10.0);
	target.down = true;
	std::map<std::uint64_t, state::Fighter*> world = {{observerId, &observer}, {targetId, &target}};
	auto lookup = [&](std::uint64_t id) -> const state::Fighter*
	{
		const auto found = world.find(id);
		return found != world.end() ? found->second : nullptr;
	};
	auto none = [](std::uint64_t) { return false; };
	perception::Environment env;
	std::vector<perception::Noticed> noticed;

	const std::vector<perception::Other> others = {{observerId, &observer}, {targetId, &target}};
	perception::scan(observerId, observer, 1.0, 0.5, others, env, noticed);
	const state::SightRecord* record = perception::find(observer, targetId);
	test::require(record != nullptr && record->seen, "the first scan sees the down body");
	test::require((sensor::flags(ctx, observer, target, *record) & sensor::kNotSeen) == 0, "sensed this scan: bit 8 clear");
	test::require(capture::nearest(observerId, observer, ctx, 60.0, lookup, none) == targetId, "the catch is in the list");

	// LOS lost: the record is remembered (bit 8), and the resolver still takes it (167 does not exclude bit 8).
	env.lineOfSight = [](std::uint64_t, const state::Fighter&, std::uint64_t, const state::Fighter&) { return false; };
	perception::scan(observerId, observer, 1.0, 1.0, others, env, noticed);
	record = perception::find(observer, targetId);
	test::require(record != nullptr && record->seen, "the record survives the lost line of sight");
	test::require((sensor::flags(ctx, observer, target, *record) & sensor::kNotSeen) != 0, "remembered only: bit 8 set");
	test::require(capture::nearest(observerId, observer, ctx, 60.0, lookup, none) == targetId,
		"a remembered body is still the catch");

	// Past the memory (30 s out of combat): the record loses `seen` (awareness) but stays, because the target is still inside the scan
	// radius (the exe's radius query returns it every pass), and a remembered body still counts for 167.
	perception::scan(observerId, observer, 1.0, 31.0, others, env, noticed);
	record = perception::find(observer, targetId);
	test::require(record != nullptr && !record->seen, "the memory's expiry clears seen, not the record");
	test::require((sensor::flags(ctx, observer, target, *record) & sensor::kNotSeen) != 0, "still remembered: bit 8");
	test::require(capture::nearest(observerId, observer, ctx, 60.0, lookup, none) == targetId, "a remembered body counts");

	// Out of the scan radius, the spent record is dropped and the catch is gone.
	target.x = 2000.0;
	perception::scan(observerId, observer, 1.0, 1.0, others, env, noticed);
	test::require(perception::find(observer, targetId) == nullptr, "a spent record out of range is dropped");
	test::require(capture::nearest(observerId, observer, ctx, 60.0, lookup, none) == 0, "and the resolver finds no body");
}

} // namespace

int main()
{
	try
	{
		testFactionBits();
		testStateBits();
		testMasksAndResolver();
		testMemory();
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_sensor_bits OK\n";
	return 0;
}
