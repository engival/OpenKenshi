// Task BO (2026-11-07): the package goals a built world squad runs after a fight (kinspect dt3 28aefb0 / 3c670dd).
//   A — a slaver caravan whose package carries CAPTURE_NEW_SLAVES 167 catches a KO'd free character: one to five members walk to
//       it, the nearest strips its weapons (181) and restrains it (166, ~10 s), the shackles go on at the cuff, the squad gives it
//       first aid, and it is carried; awake, it follows its captor (171).
//   B — a squad whose package carries LOOT_THE_DEAD 177 (the starving bandits' LOOTING patrol) sends ONE member to a down body
//       for the food it carries (Bread moves, Raw Iron and the equipped club stay), then back to the follow (141).
//   C — task BP: the same catch with the squad AI on. A trade-circuit caravan keeps its state::TraderTrip, and at Full detail it
//       builds its people all the same (task BP) and catches what walks past; its awake captive follows (171) for a minute and the
//       captor's squad never re-engages it (the slave rule).
// The player is the only character ordered; no NPC is told what to do. Fixture A runs without the squad AI (the bare world-squad
// build) and B and C with it, on the server's Hub game. Needs the world bake (argv[1]); exit 77 without it.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "caravan_unhurt.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "sim_access.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

// A wilderness spawn point the other Hub tests use (ksim_after_fight): a colour slot with room for a squad.
constexpr double kSpawnX = -48880.0;
constexpr double kSpawnZ = -823.0;
// without walking on (task BP).

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { found = i.value == id ? e : found; });
	return found;
}

state::Inventory& packOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity entity = entityOf(sim, id);
	if (entity.get<state::Inventory>() == nullptr)
		entity.set<state::Inventory>(state::Inventory{});
	return *entity.get_mut<state::Inventory>();
}

struct Person
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

std::vector<Person> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<Person> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.push_back({id.value, member});
	});
	std::sort(found.begin(), found.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
	return found;
}

flecs::entity playerFaction(Simulation& sim)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { found = e; });
	return found;
}

const kdata::Record* recordNamed(Simulation& sim, const char* type, const char* name)
{
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName(type)))
	{
		if (record->name == name)
			return record;
	}
	return nullptr;
}

bool hasShackles(Simulation& sim, std::uint64_t id)
{
	const flecs::entity entity = entityOf(sim, id);
	const state::Inventory* pack = entity.get<state::Inventory>();
	if (pack == nullptr)
		return false;
	for (const state::Inventory::Entry& entry : pack->items)
	{
		if (entry.itemSid == "SHACKLES" && entry.slot == state::Inventory::kArmourBase + 9)
			return true;
	}
	return false;
}

bool weaponWorn(Simulation& sim, std::uint64_t id)
{
	const flecs::entity entity = entityOf(sim, id);
	const state::Inventory* pack = entity.get<state::Inventory>();
	if (pack != nullptr)
	{
		for (const state::Inventory::Entry& entry : pack->items)
		{
			if (entry.slot == state::Inventory::kWeapon)
				return true;
		}
	}
	const state::Fighter* f = sim.combat()->find(id);
	return f != nullptr && f->weapon.sid != sim.combat()->data().unarmed.sid; // the FISTS record's sid, not ""
}

// The furthest the chain has got among the members on the player (the member that arrived and restrains first).
double chainElapsedOf(Simulation& sim, const std::vector<Person>& members, std::uint64_t player)
{
	double elapsed = -2.0;
	for (const Person& p : members)
	{
		const state::Routine* r = entityOf(sim, p.id).get<state::Routine>();
		if (r != nullptr && r->chainTarget == player)
			elapsed = std::max(elapsed, r->chainElapsed);
	}
	return elapsed;
}

struct Game
{
	std::string world;
	kworld::LoadOrder order;
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	bool withAi = false;

	Game(const std::string& bake, bool ai)
		: world(bake)
		, order(kworld::resolveLoadOrder(KENSHI_DATA, ""))
		, withAi(ai)
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		mesh = test::openWorldNav(world, hubZones);
	}

	void newGame()
	{
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings s;
		s.substeps = 2;
		sim->enableCombat(combat, s);
		sim->enableNavigation(mesh);
		if (withAi)
		{
			AiSystem::Config ai;
			ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
			ai.factions = FactionBook::build(sim->gameData());
			sim->enableAi(std::move(ai));
		}
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
		{
			if (member.fighter.playerControlled)
				player = member.id;
		}
	}

	state::Fighter& me() { return *fighterOf(*sim, player); }

	void teleport(double x, double z)
	{
		state::Fighter& f = me();
		f.x = x;
		f.z = z;
		f.y = sim->groundHeight(x, z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
};

// Places a wilderness squad, makes it hostile to the player's faction and waits until its members are built (Fighters).
struct Placed
{
	flecs::entity squad;
	std::vector<Person> members;
};

Placed placeHostile(Game& game, const char* templateSid, std::size_t expected, double x = kSpawnX, double z = kSpawnZ)
{
	Simulation& sim = *game.sim;
	const flecs::entity squad = sim.placeWildernessSquad(templateSid, x, z);
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	test::require(instance != nullptr && instance->faction.id != 0, "the placed squad has a faction");
	sim.enqueue(Command::setFactionRelation(*ecsOf(sim).entity(instance->faction.id).get<state::Id>(),
		*playerFaction(sim).get<state::Id>(), -60));
	Placed placed;
	placed.squad = squad;
	for (int i = 0; i < 20 * 20; ++i)
	{
		sim.run(1);
		placed.members = membersOf(sim, squad);
		const bool built = std::all_of(placed.members.begin(), placed.members.end(), [&](const Person& p) { return sim.combat()->find(p.id) != nullptr; });
		if (built && placed.members.size() >= expected)
			break;
	}
	test::require(placed.members.size() >= expected, "the squad was built with its members");
	for (const Person& p : placed.members)
	{
		test::require(sim.combat()->find(p.id) != nullptr, "the world squad's members are Fighters (it was promoted)");
	}
	return placed;
}

void givePackets(Game& game, const kdata::Record* bread, const kdata::Record* iron, const kdata::Record* club)
{
	Simulation& sim = *game.sim;
	state::Inventory& pack = packOf(sim, game.player);
	pack.add(bread->stringID, 1);
	pack.add(iron->stringID, 1);
	pack.add(club->stringID, 1, state::Inventory::kCarried, sim.combat()->data().rollQuality(*sim.gameData(), *club, 0.75, 0.0));
	sim.enqueue(Command::fighterEquip(state::Id{game.player}, club->stringID, true, 1));
	sim.run(3);
	test::require(game.me().weapon.sid == club->stringID, "the player wears the club");
}

// Runs until the player is knocked out (or down); a player still standing after `seconds` is knocked out by the fixture (printed).
void runToKo(Game& game, double seconds, const char* what, const std::vector<Person>& unhurt = {})
{
	Simulation& sim = *game.sim;
	for (int i = 0; i < static_cast<int>(20 * seconds); ++i)
	{
		sim.run(1);
		for (const Person& p : unhurt)
			keepCaravanUnhurt(sim, p.id);
		const state::Fighter& me = game.me();
		if (me.unconscious || me.down)
			return;
	}
	state::Fighter& me = game.me();
	std::cout << what << ": the player was still up after " << seconds << " s; the fixture knocks it out\n";
	me.unconscious = true;
	me.koTimer = 600.0;
	sim.run(1);
}

// Fixture A: the slaver caravan's catch.
void slaverCapture(const std::string& bake)
{
	Game game(bake, false);
	game.newGame();
	Simulation& sim = *game.sim;
	sim.run(200);

	const kdata::Record* bread = recordNamed(sim, "ITEM", "Bread");
	const kdata::Record* iron = recordNamed(sim, "ITEM", "Raw Iron");
	const kdata::Record* club = recordNamed(sim, "WEAPON", "Iron Club");
	const kdata::Record* kit = recordNamed(sim, "ITEM", "Basic First Aid Kit");
	test::require(bread != nullptr && iron != nullptr && club != nullptr && kit != nullptr, "Bread, Raw Iron, the Iron Club and a kit exist in the data");
	givePackets(game, bread, iron, club);

	Placed caravan = placeHostile(game, "3387-gamedata.base", 2);
	// A kit for the squad's medic (the first aid is a goal of its own, 58 / 25; only its effect is modelled).
	std::size_t kits = 0;
	for (const Person& p : caravan.members)
	{
		if (packOf(sim, p.id).carried(kit->stringID) > 0)
			++kits;
	}
	for (const Person& p : caravan.members)
	{
		if (kits == 0)
			packOf(sim, p.id).add(kit->stringID, 1);
	}
	std::cout << "caravan " << caravan.members.size() << " members, kits " << kits << "\n";

	// The player walks up and attacks one member: the only order given.
	game.teleport(kSpawnX + 18.0, kSpawnZ);
	sim.run(2);
	sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{caravan.members.front().id}));
	runToKo(game, 90.0, "slaver capture", caravan.members);
	test::require(game.me().unconscious || game.me().down, "the caravan puts the player down");

	// The chain: watch every tick. The KO is held (a KO that outlasts the chain, as a fresh one does) until the lift.
	std::uint64_t captor = 0;
	double captorElapsed = -2.0;
	std::uint64_t chainTick = 0;
	std::uint64_t stripTick = 0;
	std::uint64_t shackleTick = 0;
	std::uint64_t carryTick = 0;
	double stripElapsed = -1.0;
	double shackleElapsed = -1.0;
	bool stripBeforeShackle = true;
	for (int i = 0; i < 20 * 120 && carryTick == 0; ++i)
	{
		state::Fighter& me = game.me();
		if (!me.unconscious && me.carriedBy == 0)
		{
			me.unconscious = true;
			me.koTimer = 600.0;
		}
		sim.run(1);
		for (const Person& p : caravan.members)
		{
			flecs::entity entity = entityOf(sim, p.id);
			const state::Routine* r = entity.get<state::Routine>();
			if (r != nullptr && r->goal == routines::task::CaptureSlaves && r->chainTarget == game.player)
			{
				if (chainTick == 0)
					chainTick = sim.clockTick();
				// The captor is the member that restrains (its chain elapsed reaches the cuff), not the first to take the goal:
				// the chain hands it to up to five members and whichever arrives first cuffs and carries.
				if (captor == 0 || r->chainElapsed > captorElapsed)
				{
					captor = p.id;
					captorElapsed = r->chainElapsed;
				}
			}
		}
		if (stripTick == 0 && !weaponWorn(sim, game.player))
		{
			stripTick = sim.clockTick();
			stripElapsed = chainElapsedOf(sim, caravan.members, game.player);
		}
		if (shackleTick == 0 && hasShackles(sim, game.player))
		{
			shackleTick = sim.clockTick();
			shackleElapsed = chainElapsedOf(sim, caravan.members, game.player);
			if (stripTick == 0 || stripTick >= shackleTick)
				stripBeforeShackle = false;
		}
		if (game.me().carriedBy != 0)
			carryTick = sim.clockTick();
	}
	const state::Fighter& me = game.me();
	std::cout << "capture: chain at " << chainTick << " (member " << captor << "), strip at " << stripTick
		<< " (chainElapsed " << stripElapsed << "), shackles at " << shackleTick << " (chainElapsed " << shackleElapsed
		<< "), carried at " << carryTick << " (by " << me.carriedBy << ")\n";

	std::vector<std::string> failures;
	const auto check = [&](bool ok, const std::string& what) { if (!ok) failures.push_back(what); };
	check(chainTick != 0 && captor != 0, "a caravan member took CAPTURE_NEW_SLAVES 167 on the player");
	check(stripTick != 0 && !weaponWorn(sim, game.player), "the club was stripped (no worn weapon, fists)");
	check(stripBeforeShackle && stripTick != 0 && stripTick < shackleTick, "the strip (181) comes before the cuff (166)");
	check(stripElapsed >= 0.0 && stripElapsed < kRestrainSeconds, "the strip happens at the restrain's start");
	// The bar is 12 s (kRestrainSeconds). Its own clock (Routine::chainElapsed) reads it while the chain lives; a world member's 167 routine now ends at
	// the cuff (task CB: 167's goal state is isChained, the lift is the caging goal's), so by the tick the shackles appear the routine
	// is gone and the tick delta is the witness.
	const double restrainSeconds = static_cast<double>(shackleTick - stripTick) * kTickSeconds;
	std::cout << "restrain: " << restrainSeconds << " s from strip to shackles (chainElapsed " << shackleElapsed << ")\n";
	check(shackleTick != 0 && (shackleElapsed >= kRestrainSeconds - 1.0e-6 || restrainSeconds >= kRestrainSeconds - 0.25),
		"the shackles go on when the restrain bar ends");
	check(me.slaveState == 1 && !me.captorSid.empty(), "the player is a slave with a captor");
	// The capture goal ends at the cuff (167's goal state, s58); the lift is the caging goal's (task CB), so any caravan member may
	// carry the player (the pack's own 168 row picks it up).
	const std::uint64_t carrier = me.carriedBy;
	check(carryTick != 0 && carrier != 0 && fighterOf(sim, carrier)->carrying == game.player, "a caravan member carries the player");
	check(hasShackles(sim, game.player), "the Prisoner Shackles are worn in the boots slot");
	bool armourSlot = false;
	for (const state::FighterArmour& piece : me.armour)
		armourSlot = armourSlot || piece.slot == 9;
	check(armourSlot, "the Fighter's armour numbers include the boots slot 9");
	const state::Inventory& pack = *entityOf(sim, game.player).get<state::Inventory>();
	check(pack.carried(bread->stringID) == 1 && pack.carried(iron->stringID) == 1, "Bread and Raw Iron stay with the captive");
	bool clubOnGround = false;
	ecsOf(sim).each([&](const state::GroundItem& g) { clubOnGround = clubOnGround || (g.itemSid == club->stringID); });
	check(clubOnGround, "the club lies on the ground");

	// First aid: the squad's medic spent kit charges on the captive.
	double kitCharges = -1.0;
	for (const Person& p : caravan.members)
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		if (f != nullptr && f->kitCharges > 0.0)
			kitCharges = std::max(kitCharges, f->kitCharges);
	}
	for (int i = 0; i < 20 * 10 && kitCharges < 0.0; ++i)
	{
		game.me().unconscious = true;
		game.me().koTimer = 600.0;
		sim.run(1);
		for (const Person& p : caravan.members)
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f != nullptr && f->kitCharges > 0.0)
				kitCharges = std::max(kitCharges, f->kitCharges);
		}
	}
	std::cout << "first aid: the medic's kit has " << kitCharges << " charges\n";
	check(kitCharges >= 0.0, "a squadmate gave the captive first aid (a kit was opened)");

	// The lift finishes first (the carry's phase 0 is 2.343 s): only then can a wake put the person down.
	for (int i = 0; i < 60; ++i)
	{
		const state::CarryStep* step = entityOf(sim, carrier).get<state::CarryStep>();
		if (step != nullptr && step->phase == 1)
			break;
		game.me().unconscious = true;
		game.me().koTimer = 600.0;
		sim.run(1);
	}
	const state::CarryStep* lifted = entityOf(sim, carrier).get<state::CarryStep>();
	test::require(lifted != nullptr && lifted->phase == 1, "the carrier finishes the lift (the carry is in hand)");

	// Awake (171): the captive is put down and follows its captor. The body is healed first (a wrecked one is knocked out again by
	// its own blood loss in the same tick) and then woken by the fixture.
	{
		state::Fighter& awake = game.me();
		awake.blood = sim.combat()->rules().maxBlood(awake);
		for (state::FighterPart& part : awake.parts)
			part.health = part.maxHealth - part.wear;
		awake.wounds.clear();
		awake.koTimer = 0.0;
		awake.unconscious = false;
		awake.down = false;
	}
	sim.run(1);
	const state::Fighter& after = game.me();
	std::cout << "awake: carriedBy " << after.carriedBy << ", followTarget " << after.followTarget << " (captor " << captor << ")\n";
	check(after.carriedBy == 0, "an awake captive is put down");
	check(after.followTarget == carrier, "the awake slave is handed to the follow (171), its carrier");
	// The captor's squad does not re-engage it (the slave rule, task BP): the follow is still there a second later.
	sim.run(20);
	std::cout << "awake, 1 s on: followTarget " << game.me().followTarget << ", the player's target " << game.me().target << "\n";
	check(game.me().followTarget == carrier && game.me().target == 0, "a shackled slave is not re-engaged by its carrier's squad");

	if (failures.empty())
	{
		std::cout << "slaver capture: ok\n";
		return;
	}
	for (const std::string& f : failures)
		std::cerr << "error: " << f << "\n";
	throw std::runtime_error("slaver capture: " + std::to_string(failures.size()) + " failure(s)");
}

// Fixture B: the winners loot the food off the player.
void foodLoot(const std::string& bake)
{
	Game game(bake, true);
	game.newGame();
	Simulation& sim = *game.sim;
	sim.run(200);

	const kdata::Record* bread = recordNamed(sim, "ITEM", "Bread");
	const kdata::Record* iron = recordNamed(sim, "ITEM", "Raw Iron");
	const kdata::Record* club = recordNamed(sim, "WEAPON", "Iron Club");
	test::require(bread != nullptr && iron != nullptr && club != nullptr, "Bread, Raw Iron and the Iron Club exist in the data");
	givePackets(game, bread, iron, club);

	Placed bandits = placeHostile(game, "46046-rebirth.mod", 3);
	std::cout << "starving bandits " << bandits.members.size() << " members\n";
	game.teleport(kSpawnX + 18.0, kSpawnZ);
	sim.run(2);
	sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{bandits.members.front().id}));
	runToKo(game, 90.0, "food loot");

	// The loot: one member takes LOOT_THE_DEAD 177; the KO is held while it walks there.
	std::uint64_t looter = 0;
	std::uint64_t lootTick = 0;
	std::uint64_t breadTick = 0;
	for (int i = 0; i < 20 * 120 && breadTick == 0; ++i)
	{
		state::Fighter& me = game.me();
		if (!me.unconscious)
		{
			me.unconscious = true;
			me.koTimer = 600.0;
		}
		sim.run(1);
		for (const Person& p : bandits.members)
		{
			const state::Routine* r = entityOf(sim, p.id).get<state::Routine>();
			if (r != nullptr && r->goal == routines::task::LootTheDead)
			{
				if (lootTick == 0)
					lootTick = sim.clockTick();
				looter = p.id;
			}
			if (packOf(sim, p.id).carried(bread->stringID) > 0 && breadTick == 0)
				breadTick = sim.clockTick();
		}
	}
	std::cout << "loot: 177 taken at " << lootTick << " (member " << looter << "), Bread moved at " << breadTick << "\n";

	std::vector<std::string> failures;
	const auto check = [&](bool ok, const std::string& what) { if (!ok) failures.push_back(what); };
	check(lootTick != 0 && looter != 0, "one bandit took LOOT_THE_DEAD 177");
	check(breadTick != 0 && packOf(sim, looter).carried(bread->stringID) == 1, "the Bread moved into the looter's pack");
	const state::Inventory& pack = *entityOf(sim, game.player).get<state::Inventory>();
	check(pack.carried(iron->stringID) == 1, "the Raw Iron stays on the body");
	std::cout << "the body still wields '" << game.me().weapon.sid << "' (" << pack.items.size() << " entries)\n";
	check(game.me().weapon.sid == club->stringID, "the equipped club stays on the body");
	check(entityOf(sim, looter).get<state::Routine>() == nullptr, "the looter's task ends");

	// The rejoin (141): the next world follow step takes the looter back to its leader.
	std::uint64_t leader = 0;
	for (const Person& p : bandits.members)
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		if (f != nullptr && !f->dead && !f->unconscious && !p.entity.has<state::Routine>())
		{
			leader = p.id;
			break;
		}
	}
	for (int i = 0; i < 20 * 5 && game.me().followTarget == 0; ++i)
		sim.run(1);
	sim.run(20);
	const state::Fighter* looterF = sim.combat()->find(looter);
	std::cout << "rejoin: looter " << looter << " follows " << (looterF != nullptr ? looterF->followTarget : 0) << " (leader " << leader << ")\n";
	check(looterF != nullptr && looterF->followTarget == leader, "the looter rejoins its squad (141)");
	bool lootingAgain = false;
	for (const Person& p : bandits.members)
	{
		const state::Routine* r = entityOf(sim, p.id).get<state::Routine>();
		lootingAgain = lootingAgain || (r != nullptr && r->goal == routines::task::LootTheDead);
	}
	check(!lootingAgain, "nobody returns to the body to loot again");

	if (failures.empty())
	{
		std::cout << "food loot: ok\n";
		return;
	}
	for (const std::string& f : failures)
		std::cerr << "error: " << f << "\n";
	throw std::runtime_error("food loot: " + std::to_string(failures.size()) + " failure(s)");
}

// Fixture C (task BP): the same catch with the squad AI ON. A trade-circuit caravan carries its `state::TraderTrip`, and at Full detail
// it is built into Fighters like any world squad while its circuit still walks it by the leader (5.14); it catches a KO'd player, and
// the awake captive walks along with it for a minute without being attacked.
void caravanWithAi(const std::string& bake)
{
	Game game(bake, true);
	game.newGame();
	Simulation& sim = *game.sim;
	sim.run(200);

	// The Hub: the caravan arrives, stays (its trip) and holds its table for hours, so the catch is not on the move.
	game.teleport(kHubX + 18.0, kHubZ);
	sim.run(2);
	Placed caravan = placeHostile(game, "3387-gamedata.base", 2, kHubX, kHubZ);
	const state::TraderTrip* trip = caravan.squad.get<state::TraderTrip>();
	const Detail* detail = caravan.squad.get<Detail>();
	test::require(trip != nullptr && trip->staying, "the AI gives the caravan a trade trip, staying in the Hub");
	test::require(detail != nullptr && detail->level == DetailLevel::Full, "the caravan is at Full detail next to the player");
	std::cout << "ai caravan: " << caravan.members.size() << " members, staying " << trip->staying << " (player " << game.player << ")\n";

	// This fixture pins the catch and the carry, not the aid: the guards' spawn kits (k16.6.287) would have the whole squad heal the slave
	// first (58 before 168), which wakes it, and the awake captive follows instead (171). ksim_slavers_capture pins the aid.
	for (const Person& p : caravan.members)
		std::erase_if(packOf(sim, p.id).items, [&](const state::Inventory::Entry& entry)
		{
			const ItemInfo* info = sim.combat()->data().item(entry.itemSid);
			return info != nullptr && info->function == kItemFunctionMedkit;
		});
	// The player walks up and attacks one member: the only order given.
	sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{caravan.members.front().id}));
	runToKo(game, 90.0, "ai caravan", caravan.members);
	test::require(game.me().unconscious || game.me().down, "the caravan puts the player down");

	// The catch, as in fixture A: the KO is held until the lift.
	std::uint64_t captor = 0;
	double captorElapsed = -2.0;
	std::uint64_t chainTick = 0;
	std::uint64_t carryTick = 0;
	for (int i = 0; i < 20 * 120 && carryTick == 0; ++i)
	{
		state::Fighter& me = game.me();
		if (!me.unconscious && me.carriedBy == 0)
		{
			me.unconscious = true;
			me.koTimer = 600.0;
		}
		sim.run(1);
		for (const Person& p : caravan.members)
		{
			const state::Routine* r = entityOf(sim, p.id).get<state::Routine>();
			if (r != nullptr && r->goal == routines::task::CaptureSlaves && r->chainTarget == game.player)
			{
				if (chainTick == 0)
					chainTick = sim.clockTick();
				if (captor == 0 || r->chainElapsed > captorElapsed)
				{
					captor = p.id;
					captorElapsed = r->chainElapsed;
				}
			}
		}
		if (game.me().carriedBy != 0)
			carryTick = sim.clockTick();
	}
	const state::Fighter& me = game.me();
	std::cout << "ai capture: chain at " << chainTick << " (member " << captor << "), carried at " << carryTick << " (by " << me.carriedBy << ") slave " << (int)me.slaveState << " unc " << (int)me.unconscious << " shackles " << hasShackles(sim, game.player) << "\n";
	for (const Person& p : caravan.members) { const state::Routine* r = entityOf(sim, p.id).get<state::Routine>(); if (r != nullptr) std::cout << "  member " << p.id << " goal " << r->goal << " chain " << r->chainTarget << " plan " << r->plan.size() << " step " << r->step << " carry " << (sim.combat()->find(p.id) != nullptr && sim.combat()->find(p.id)->carrying != 0) << "\n"; }
	std::vector<std::string> failures;
	const auto check = [&](bool ok, const std::string& what) { if (!ok) failures.push_back(what); };
	check(chainTick != 0 && captor != 0, "a caravan member took CAPTURE_NEW_SLAVES 167 on the player");
	// The lift is the caging goal's (task CB): any caravan member may carry the chained player.
	const std::uint64_t carrier = me.carriedBy;
	check(carryTick != 0 && carrier != 0 && fighterOf(sim, carrier)->carrying == game.player, "a caravan member carries the player");
	check(me.slaveState == 1 && !me.captorSid.empty() && hasShackles(sim, game.player), "the player is a shackled slave of the caravan's faction");
	check(caravan.squad.get<state::TraderTrip>() != nullptr, "the trip state stays on the squad through the build");

	// The lift first, then a healed and woken body (as in fixture A).
	for (int i = 0; i < 60; ++i)
	{
		const state::CarryStep* step = entityOf(sim, carrier).get<state::CarryStep>();
		if (step != nullptr && step->phase == 1)
			break;
		game.me().unconscious = true;
		game.me().koTimer = 600.0;
		sim.run(1);
	}
	const state::CarryStep* lifted = entityOf(sim, carrier).get<state::CarryStep>();
	test::require(lifted != nullptr && lifted->phase == 1, "the carrier finishes the lift (the carry is in hand)");
	// k16.6.3: the caravan walks on (the leader on a move route, its members following) with the KO held: the carrier stays with its
	// squad, still carrying, instead of standing at the yard the squad left (no cage to put the captive in).
	{
		const double fromX = game.me().x;
		const double fromZ = game.me().z;
		const std::uint64_t leaderId = caravan.members.front().id;
		const state::Fighter& leaderNow = *sim.combat()->find(leaderId);
		Route route = sim.navigation()->routeFrom({leaderNow.x, leaderNow.y, leaderNow.z}, leaderNow.x + 250.0, leaderNow.z);
		test::require(route.found, "the leader has a route to walk on");
		sim.enqueue(Command::fighterMoveTo(state::Id{leaderId}, std::move(route.points), std::move(route.heights), 5, false, route.approach));
		for (int i = 0; i < 20 * 60; ++i)
		{
			if (game.me().unconscious || game.me().carriedBy != 0)
			{
				game.me().unconscious = true;
				game.me().koTimer = 600.0;
			}
			sim.run(1);
		}
		const state::Fighter& carrierNow = *sim.combat()->find(carrier);
		const state::Fighter& leaderEnd = *sim.combat()->find(leaderId);
		const double leaderFar = std::hypot(leaderEnd.x - fromX, leaderEnd.z - fromZ);
		const double gap = std::hypot(leaderEnd.x - carrierNow.x, leaderEnd.z - carrierNow.z);
		std::cout << "walk on: leader " << leaderFar << " u from the capture, carrier " << gap << " u from the leader, carrying "
			<< carrierNow.carrying << " (player " << game.player << ")\n";
		test::require(leaderFar >= 200.0, "the caravan walked on");
		failures.push_back(gap <= 60.0 && carrierNow.carrying == game.player ? std::string() : "the carrier stays with the walking squad, still carrying");
		if (failures.back().empty())
			failures.pop_back();
	}
	{
		state::Fighter& awake = game.me();
		awake.blood = sim.combat()->rules().maxBlood(awake);
		for (state::FighterPart& part : awake.parts)
			part.health = part.maxHealth - part.wear;
		awake.wounds.clear();
		awake.koTimer = 0.0;
		awake.unconscious = false;
		awake.down = false;
	}
	sim.run(1);
	const double bloodAfterWake = game.me().blood;
	std::cout << "awake: carriedBy " << game.me().carriedBy << ", followTarget " << game.me().followTarget << " (carrier " << carrier << ")\n";
	check(game.me().carriedBy == 0, "an awake captive is put down");
	check(game.me().followTarget == carrier, "the awake slave follows its carrier (171)");

	// 60 s: the slave walks along with its captor's squad, which does not attack it (task BP's slave rule), and no one else does either.
	std::uint64_t engagedTick = 0;
	std::uint64_t droppedTick = 0;
	double bloodLow = bloodAfterWake;
	for (int i = 0; i < 20 * 60; ++i)
	{
		sim.run(1);
		const state::Fighter& now = game.me();
		bloodLow = std::min(bloodLow, now.blood);
		if (engagedTick == 0 && now.target != 0)
			engagedTick = sim.clockTick();
		if (droppedTick == 0 && now.followTarget != carrier)
			droppedTick = sim.clockTick();
		for (const Person& p : caravan.members)
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (engagedTick == 0 && f != nullptr && f->target == game.player)
				engagedTick = sim.clockTick();
		}
	}
	const state::Fighter* captorNow = sim.combat()->find(captor);
	std::cout << "60 s on: followTarget " << game.me().followTarget << ", the player's target " << game.me().target
		<< ", blood " << bloodLow << " of " << bloodAfterWake << ", engaged at " << engagedTick << ", follow dropped at " << droppedTick
		<< ", " << (captorNow != nullptr ? std::hypot(game.me().x - captorNow->x, game.me().z - captorNow->z) : -1.0)
		<< " u from the captor\n";
	check(engagedTick == 0, "nobody attacks the shackled slave (the captor's squad and its allies leave it alone)");
	check(droppedTick == 0, "the slave keeps following its carrier (the captor or the squad member that lifted it)");
	check(!game.me().unconscious && !game.me().down, "the slave is left standing");

	// And a rebellion still works: the slave the player orders onto its captor does attack it (the order target outranks the slave rule).
	sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{captor}));
	for (int i = 0; i < 20 && game.me().target == 0; ++i)
		sim.run(1);
	std::cout << "rebellion: the slave's target " << game.me().target << " (captor " << captor << ")\n";
	check(game.me().target == captor, "an ordered slave attacks its captor (a rebellion is not blocked by the slave rule)");

	if (failures.empty())
	{
		std::cout << "ai caravan: ok\n";
		return;
	}
	for (const std::string& f : failures)
		std::cerr << "error: " << f << "\n";
	throw std::runtime_error("ai caravan: " + std::to_string(failures.size()) + " failure(s)");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_slaver_caravan: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		slaverCapture(argv[1]);
		foodLoot(argv[1]);
		caravanWithAi(argv[1]);
		std::cout << "ksim_slaver_caravan: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
