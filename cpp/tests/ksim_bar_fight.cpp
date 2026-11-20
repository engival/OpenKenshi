// The bar-fight loop (docs/reference/BAR_FIGHT_LOOP.md): the user's new-player strategy, run in process on the server's own game (seed 1, the Hub,
// wilderness by density, streaming navmesh, squad AI as `ksim_server --bake` runs it). Patrol, lure, pick off, run away, loot, sell, repeat.
// The steps that work today are asserted; the ones that do not are printed (set KSIM_BAR_FIGHT_VERBOSE=1) and written up as gaps in the doc.
// Needs the world bake (argv[1], like ksim_world_hub; exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trade.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <ostream>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// The Bar blackmarket: the Storm House placement 3384 whose squad template is Bar blackmarket, footprint centre (-50592, -1211) (zone GAMESTATE_BUILDING,
// TOWN_LIFE_SLICE 5.17). It is the Rebel Base's Bar (TOWN 44424-rebirth.mod "Rebel Base", a POI 187 u away; kinspect BQ13's fight), not the Hub's own
// Bar at (-51181, 2639) - the Hub's is ksim_aggro_drop's and ksim_town_bar's. Its barman and guards work there since the T3d follow-up (their home is the
// placement); the earlier (-50000, -1700) was where the 5.14 debug found them standing, before they had a home building.
constexpr double kBarX = -50592.0;
constexpr double kBarZ = -1211.0;

std::ostream& say()
{
	static std::ostream nowhere(nullptr);
	return std::getenv("KSIM_BAR_FIGHT_VERBOSE") != nullptr ? std::cout : nowhere;
}

std::string factionName(Simulation& sim, const state::SquadInstance& instance)
{
	if (instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id))
		return "";
	const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
	const kdata::Record* record = faction != nullptr ? sim.gameData()->find(faction->templateSid) : nullptr;
	return record != nullptr ? record->name : "";
}

[[maybe_unused]] double healthOf(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& part : f.parts)
		sum += part.health;
	return sum;
}

struct Person
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

std::vector<Person> membersOf(Simulation& sim, std::uint64_t squadId)
{
	std::vector<Person> found;
	flecs::entity squad;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::SquadInstance&)
	{
		if (id.value == squadId)
			squad = e;
	});
	if (!squad.is_valid())
		return found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.push_back({id.value, member});
	});
	std::sort(found.begin(), found.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
	return found;
}

// The whole in-process game: one place for the setup the checks share (a save and a load re-enable the same pieces).
struct Game
{
	std::string world;
	kworld::LoadOrder order;
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	Game(const std::string& bake)
		: world(bake)
		, order(kworld::resolveLoadOrder(KENSHI_DATA, ""))
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		mesh = test::openWorldNav(world, hubZones);
	}

	void enable()
	{
		sim->enableCombat(combat, settings());
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
		findPlayer();
	}

	static CombatSettings settings()
	{
		CombatSettings s;
		s.substeps = 2;
		// The watched-duel medical clock, not the default: this fixture's stages were authored and re-pinned at 2.0 (the
		// lure walk's KOs and deaths shape the survivors' positions in 3a / 3b), and medical timing is not what it measures.
		// At the synced default (~0.96) the fight lasts twice as long and the lure stage leaves a different scene: a bandit at
		// the Bar's door picks a crew member through the open door in the same pass the crew pick it back, ~1.8 s before a
		// bandit's membership is inside, and 3b "the crew joined before any bandit inside" fails. Re-authoring the fixture at
		// the default is a follow-up (task BT's report); the join rules themselves are unchanged.
		s.medicalClock = 2.0;
		return s;
	}

	void findPlayer()
	{
		player = 0;
		for (const CombatArena::Member& member : sim->combat()->members())
		{
			if (member.fighter.playerControlled)
				player = member.id;
		}
	}

	void newGame()
	{
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		// The patrol the scenario is about, placed on purpose (BAR_FIGHT_LOOP "Why Dust Bandits patrol").
		sim->placeWildernessSquad("1302-gamedata.base", -48880.0, -823.0, 15);
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		enable();
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		findPlayer();
	}

	void load(const std::string& path)
	{
		sim = std::make_unique<Simulation>(Simulation::loadGame(KENSHI_DATA, order, path, false, nullptr, RegionConfig{}));
		if (combat == nullptr)
			combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		enable();
	}

	state::Fighter& me() { return *fighterOf(*sim, player); }

	// Walk (or run) the player to a point on the navmesh; false when there is no route.
	bool walkTo(double x, double z)
	{
		const state::Fighter& f = me();
		Route route = sim->navigation()->routeFrom({f.x, f.y, f.z}, x, z);
		if (!route.found)
			return false;
		sim->enqueue(Command::fighterMoveTo(state::Id{player}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
		return true;
	}

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

// The walk-by: a hostile steps up to a patrolling (walking) bandit squad and must be attacked. Returns the number of bandits that target the player
// within 15 s, and prints the state of the squad the moment the player arrives.
struct WalkBy
{
	bool leaderWalking = false;
	std::size_t walkingMembers = 0;
	std::size_t followers = 0;
	double distance = 0.0;
	std::size_t aggro = 0;
	std::uint64_t firstAggroTick = 0;
};

WalkBy walkBy(Game& game, std::uint64_t banditSquad, double gap)
{
	Simulation& sim = *game.sim;
	WalkBy result;
	// Wait for a moment when the leader is walking its patrol leg.
	std::uint64_t leaderId = 0;
	for (std::uint64_t i = 0; i < 20000 && leaderId == 0; ++i)
	{
		sim.run(10);
		for (const Person& p : membersOf(sim, banditSquad))
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f != nullptr && !f->dead && !f->unconscious && f->walking() && f->target == 0)
			{
				leaderId = p.id;
				break;
			}
		}
	}
	test::require(leaderId != 0, "the patrol walks");
	const state::Fighter lead = *sim.combat()->find(leaderId);
	// Player 'gap' units to the side of the leader's heading.
	const double hx = std::sin(lead.yaw);
	const double hz = std::cos(lead.yaw);
	game.teleport(lead.x - hz * gap, lead.z + hx * gap);
	result.distance = std::hypot(game.me().x - lead.x, game.me().z - lead.z);
	for (const Person& p : membersOf(sim, banditSquad))
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		if (f == nullptr || f->dead)
			continue;
		result.walkingMembers += f->walking() ? 1 : 0;
		result.followers += f->followTarget != 0 ? 1 : 0;
	}
	result.leaderWalking = lead.walking();
	// The provocation (task BV, 2026-11-07): the Dust Bandits' own relation to a new player is -10 (neutral, FUN_14079c950 / kinspect
	// dt11), so stepping into their sight no longer starts a fight. The player attacks the nearest bandit: the original's personal
	// type-4 flip (Command::fighterAttack), and the patrol answers through onTargetedBy. Target picking under patrol is what the
	// walk-by reads.
	std::uint64_t mark = 0;
	double markDistance = 1.0e18;
	for (const Person& p : membersOf(sim, banditSquad))
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		if (f == nullptr || f->dead)
			continue;
		const double d = std::hypot(f->x - game.me().x, f->z - game.me().z);
		if (d < markDistance) { markDistance = d; mark = p.id; }
	}
	test::require(mark != 0, "the walk-by has a bandit to provoke");
	sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{mark}));
	const std::uint64_t t0 = sim.clockTick();
	for (int i = 0; i < 300; ++i)
	{
		sim.run(1);
		std::size_t n = 0;
		for (const Person& p : membersOf(sim, banditSquad))
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			n += f != nullptr && !f->dead && f->target == game.player ? 1 : 0;
		}
		if (n > 0 && result.firstAggroTick == 0)
			result.firstAggroTick = sim.clockTick() - t0;
		result.aggro = std::max(result.aggro, n);
	}
	return result;
}

// Whether `from`'s whole inventory would fit the pack of `to` (Simulation::lootBody's own steps on copies): the loot is all-or-partial by the grid, so the
// scenario loots a body the player can carry, not the nearest one whatever its shapes.
bool lootFits(Simulation& sim, std::uint64_t to, std::uint64_t from)
{
	const auto inventoryOf = [&](std::uint64_t id)
	{
		state::Inventory inventory;
		ecsOf(sim).each([&](flecs::entity entity, const state::Id& eid)
		{
			if (eid.value == id && entity.has<state::Fighter>() && entity.get<state::Inventory>() != nullptr)
				inventory = *entity.get<state::Inventory>();
		});
		return inventory;
	};
	const grid::Rules rules(*sim.gameData());
	state::Inventory source = inventoryOf(from);
	state::Inventory mine = inventoryOf(to);
	grid::settle(source, rules, from);
	grid::settle(mine, rules, to);
	for (state::Inventory::Entry entry : source.items)
	{
		entry.slot = state::Inventory::kCarried;
		entry.section = state::Inventory::Entry::kUnplaced;
		if (grid::autoplace(mine, rules, entry) != 0)
			return false;
	}
	return true;
}

struct Gear
{
	std::string item;
	std::string name;
	std::uint32_t count = 0;
	std::uint8_t slot = 0;
	int value = 0;
};

std::vector<Gear> gearOf(Simulation& sim, std::uint64_t id)
{
	std::vector<Gear> gear;
	flecs::entity e;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id& eid)
	{
		if (eid.value == id && entity.has<state::Fighter>())
			e = entity;
	});
	const state::Inventory* inv = e.is_valid() ? e.get<state::Inventory>() : nullptr;
	if (inv == nullptr)
		return gear;
	for (const state::Inventory::Entry& entry : inv->items)
	{
		const kdata::Record* record = sim.gameData()->find(entry.itemSid);
		gear.push_back({entry.itemSid, record != nullptr ? record->name : "?", entry.count, entry.slot, record != nullptr ? record->intOr("value", 0) : 0});
	}
	return gear;
}

// The fight itself: the player waits at the Bar, the patrol arrives, the player picks off the weakest bandit, loots, and asks the barman's prices.
void barFight(const std::string& bake, std::uint64_t banditSquad, bool engage)
{
	Game game(bake);
	game.newGame();
	Simulation& sim = *game.sim;
	sim.run(200);
	const state::Fighter start = game.me();
	game.teleport(kBarX + 80.0, kBarZ);
	const state::FighterSkills skillsBefore = game.me().skills;
	auto dist = [&](const state::Fighter& f) { return std::hypot(f.x - game.me().x, f.z - game.me().z); };
	double arenaX = kBarX; // where the fight is looked for (the player's place, or the resident the patrol is set down by)
	double arenaZ = kBarZ;
	auto count = [&](bool dead)
	{
		std::size_t n = 0;
		for (const Person& p : membersOf(sim, banditSquad))
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			n += f != nullptr && (dead ? f->dead : (!f->dead && !f->unconscious)) ? 1 : 0;
		}
		return n;
	};
	auto residentsNear = [&](double radius)
	{
		std::size_t n = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			n += m.fighter.team == 0 && !m.fighter.playerControlled && !m.fighter.dead && dist(m.fighter) < radius ? 1 : 0;
		return n;
	};
	// Who engages whom near the player: bandits with a team 0 NPC target, team 0 NPCs with a bandit target.
	auto engagement = [&](std::size_t& banditsOnResidents, std::size_t& residentsOnBandits)
	{
		banditsOnResidents = residentsOnBandits = 0;
		std::vector<std::uint64_t> ids;
		for (const Person& p : membersOf(sim, banditSquad))
			ids.push_back(p.id);
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (m.fighter.dead || m.fighter.target == 0 || std::hypot(m.fighter.x - arenaX, m.fighter.z - arenaZ) > 600.0)
				continue;
			const bool bandit = std::find(ids.begin(), ids.end(), m.id) != ids.end();
			const state::Fighter* t = sim.combat()->find(m.fighter.target);
			if (bandit && t != nullptr && t->team == 0 && !t->playerControlled)
				++banditsOnResidents;
			if (!bandit && m.fighter.team == 0 && !m.fighter.playerControlled && std::find(ids.begin(), ids.end(), m.fighter.target) != ids.end())
				++residentsOnBandits;
		}
	};
	sim.run(100);
	say() << "BAR: " << residentsNear(400.0) << " team 0 NPCs within 400 u of the player at the Bar\n";
	// ---- 3 the fight enters the Bar and the crew joins (BQ13, bq13_rebel_bar.md; BAR_FIGHT_LOOP "bar_crowd: join rules") ----
	// The Bar's crew (the barman and the Ninja Guards, Trade Ninjas_0) stand indoors on their nodes. Unaware bandits never alert them and neither does
	// a fight outside; they join when a bandit that holds a combat target comes into the building (its building membership becomes the Bar).
	std::vector<std::uint64_t> crew;
	std::uint64_t barBuilding = 0;
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		if (m.fighter.team == 0 && !m.fighter.playerControlled && !m.fighter.dead && m.fighter.homeBuilding != 0 && std::hypot(m.fighter.x - kBarX, m.fighter.z - kBarZ) < 120.0)
		{
			crew.push_back(m.id);
			barBuilding = m.fighter.homeBuilding;
		}
	}
	test::require(crew.size() >= 3, "3: the Bar's crew (barman and guards) stands at the Bar");
	for (const std::uint64_t id : crew)
		test::require(sim.combat()->find(id)->building == barBuilding, "3: the crew stands inside the Bar building");
	// The door pivot and the outside point (BQ13): the door at (-50619, -1156) faces +z.
	constexpr double kDoorX = -50619.0;
	constexpr double kDoorZ = -1156.0;
	const auto crewTargets = [&]
	{
		std::size_t n = 0;
		for (const std::uint64_t id : crew)
			n += sim.combat()->find(id)->target != 0 ? 1 : 0;
		return n;
	};
	const auto banditsInside = [&]
	{
		std::size_t n = 0;
		for (const Person& p : membersOf(sim, banditSquad))
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			n += f != nullptr && !f->dead && f->target != 0 && f->building == barBuilding ? 1 : 0;
		}
		return n;
	};
	const auto place = [&](double x, double z, double spread)
	{
		int k = 0;
		for (const Person& p : membersOf(sim, banditSquad))
		{
			state::Fighter* f = fighterOf(sim, p.id);
			if (f == nullptr)
				continue;
			f->x = x + spread * (k % 5);
			f->z = z + spread * (k / 5);
			f->y = sim.groundHeight(f->x, f->z).value_or(f->y);
			f->clearRoute();
			f->building = 0;
			++k;
		}
	};
	std::uint64_t fightTick = 0;
	if (engage)
	{
		// 3a/3b (task fight-goals, 2026-11-08, re-pinned on a8a41b7). The player provokes the patrol set down 150 u beyond the door (real
		// distances, not 30 u) and walks in at once: it kites the pack through the door. The crew joins the fight it SEES - goal 34
		// (ATTACK_TROUBLE_MAKERS) has NO building gate (a8a41b7: a criminal in my town, sameTown), so a targeting bandit the crew senses in
		// its own town draws it in whether or not that bandit is inside. The join therefore waits on sight, not on the building flag.
		// The earlier reading held the player blocking on the doorstep for 15 s and required the crew to stay out until a bandit was inside;
		// with the fight goals in the goal selector the pack's per-NPC phases decide which five bandits take the player's ring, and a ring
		// member stands ~50 u nearer the door - in the guard's notice - so the crew joined with the fight still on the street. That is the
		// original's own rule: the street fight is accepted here, not re-timed.
		game.teleport(kDoorX, kDoorZ + 70.0);
		// The player blocks (a player order): knocked out, it no longer holds the bandits, and the next target one senses is a guard in the
		// doorway, who defends itself (2026-11-04: the f86d2f2 populate re-roll KO'd the player at 13.5 s of the 15).
		sim.enqueue(Command::fighterBlock(state::Id{game.player}, true));
		place(kDoorX - 12.0, kDoorZ + 70.0 + 150.0, 6.0);
		// The provocation (task BV, 2026-11-07): the Dust Bandits' own relation to a new player is -10 (neutral, FUN_14079c950 / kinspect
		// dt11), so standing in their sight no longer starts a fight. The player orders an attack on the nearest bandit (the original's
		// personal type-4 flip), the squad answers, and the player stops there holding its block.
		{
			std::uint64_t mark = 0;
			double markDistance = 1.0e18;
			for (const Person& p : membersOf(sim, banditSquad))
			{
				const state::Fighter* f = sim.combat()->find(p.id);
				if (f == nullptr || f->dead)
					continue;
				const double d = std::hypot(f->x - game.me().x, f->z - game.me().z);
				if (d < markDistance) { markDistance = d; mark = p.id; }
			}
			test::require(mark != 0, "3a: the patrol has a bandit to provoke");
			sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{mark}));
			sim.run(10);
			sim.enqueue(Command::fighterStop(state::Id{game.player}, false));
		}
		arenaX = kDoorX;
		arenaZ = kDoorZ + 70.0;
		sim.enqueue(Command::fighterBlock(state::Id{game.player}, false)); // the player kites in; it does not hold ground
		const double inX = kDoorX;
		const double inZ = kDoorZ - 45.0;
		if (!game.walkTo(inX, inZ))
			game.teleport(inX, inZ);
		std::uint64_t entered = 0;
		std::uint64_t seenTick = 0;
		std::size_t aimed = 0;
		for (int i = 0; i < 400 && fightTick == 0; ++i)
		{
			sim.run(5);
			std::size_t n = 0;
			for (const Person& p : membersOf(sim, banditSquad))
			{
				const state::Fighter* f = sim.combat()->find(p.id);
				n += f != nullptr && !f->dead && f->target == game.player ? 1 : 0;
			}
			aimed = std::max(aimed, n);
			const bool inside = banditsInside() > 0;
			if (inside && entered == 0)
				entered = sim.clockTick();
			if (seenTick == 0)
			{
				for (const std::uint64_t id : crew)
				{
					const state::Fighter* g = sim.combat()->find(id);
					if (g == nullptr)
						continue;
					for (const Person& p : membersOf(sim, banditSquad))
					{
						const state::Fighter* b = sim.combat()->find(p.id);
						if (b != nullptr && !b->dead && b->target != 0 && perception::aware(*g, p.id))
						{
							seenTick = sim.clockTick();
							break;
						}
					}
					if (seenTick != 0)
						break;
				}
			}
			if (crewTargets() > 0)
			{
				for (const std::uint64_t id : crew)
				{
					const state::Fighter* g = sim.combat()->find(id);
					const state::Fighter* t = g != nullptr && g->target != 0 ? sim.combat()->find(g->target) : nullptr;
					if (g == nullptr || t == nullptr)
						continue;
					say() << "3b join: crew " << id << " (goal " << g->combatGoal << ") -> " << g->target << " team " << t->team
						<< " target " << t->target << " building " << t->building << " (bar " << barBuilding << ")"
						<< "; crew building " << g->building << " at (" << g->x << ", " << g->z << "), bandit at (" << t->x << ", " << t->z
						<< "), " << std::hypot(g->x - t->x, g->z - t->z) << " u, aware " << perception::aware(*g, g->target) << "\n";
				}
				test::require(seenTick != 0, "3b: the crew joins a fight it has not seen");
				fightTick = sim.clockTick();
			}
		}
		test::require(aimed >= 10, "3a: the provoked patrol turns on the player (>= 10 of 15)");
		test::require(fightTick != 0, "3b: the bandits coming to the door bring the crew in");
		say() << "3b join: " << aimed << " bandits target the player, first seen at tick " << seenTick << ", first bandit inside at " << entered
			<< ", crew joined at " << fightTick << " (" << (fightTick - seenTick) << " ticks after first sight), " << crewTargets() << " of "
			<< crew.size() << " have a target\n";
		test::require(fightTick - seenTick <= 200, "3b: the crew joins within 10 s of first seeing the fight");
		sim.run(200);
		std::size_t joined = crewTargets();
		say() << "3b: " << joined << " of " << crew.size() << " crew members target someone 10 s after the first join\n";
		test::require(joined * 2 >= crew.size(), "3b: most of the crew joins (the others follow within seconds)");
	} else {
		// The player keeps out of it (1750 u from the Bar). The patrol is set down 150 u from the door and its bandits are ordered on the
		// crew (the bar_crowd fixture's orders stand in for the lure): the crew's squad and the rest follow the join rules.
		const double dx = start.x - kBarX;
		const double dz = start.z - kBarZ;
		const double len = std::hypot(dx, dz);
		game.teleport(kBarX + dx / len * 1750.0, kBarZ + dz / len * 1750.0); // past the 1500 u sight range: a bandit that senses the player goes for it (goal 16 by relation)
		place(kDoorX - 12.0, kDoorZ + 150.0, 6.0);
		arenaX = kDoorX;
		arenaZ = kDoorZ;
		// Task BV: the order goes through Command::fighterAttack, the game's own scripted order, because a raw orderTarget on a faction the
		// bandits are not hostile to (the Bar's crew, now both team 0) is dropped by the enemy gate. The command writes the personal
		// type-4 entry a real attacker makes.
		int k = 0;
		for (const Person& p : membersOf(sim, banditSquad))
			sim.enqueue(Command::fighterAttack(state::Id{p.id}, state::Id{crew[static_cast<std::size_t>(k++) % crew.size()]}));
		for (std::uint64_t waited = 0; waited < 20 * kTicksPerGameHour / 20 && fightTick == 0; ++waited)
		{
			sim.run(20);
			std::size_t b = 0;
			std::size_t r = 0;
			engagement(b, r);
			if (b + r > 0)
			{
				test::require(b > 0 && r > 0, "3: both sides engage (bandits on the crew by order, the crew on the bandits by the join rules)");
				fightTick = sim.clockTick();
				say() << "3 fight under way at tick " << fightTick << ": bandits on residents " << b << ", residents on bandits " << r << "\n";
			}
		}
	}
	test::require(fightTick != 0, "3: the patrol fights the Bar's crew");
	sim.saveGame(std::string(KSIM_BINARY_DIR) + "/bar_fight_start.sav");
	sim.run(200);
	// ---- pick off the weakest bandit ----
	std::uint64_t victim = 0;
	if (engage)
	{
	double weakest = 1.0e18;
	double nearestUp = 1.0e18; // the patrol's health differs once the residents are at it: the nearest bandit still up is the one picked
	for (const Person& p : membersOf(sim, banditSquad))
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		if (f != nullptr && !f->dead && !f->unconscious && dist(*f) < 1200.0 && dist(*f) < nearestUp)
		{
			nearestUp = dist(*f);
			weakest = healthOf(*f);
			victim = p.id;
		}
	}
	say() << "3 pick off: weakest bandit " << victim << " health " << static_cast<long>(weakest) << " at " << (victim != 0 ? static_cast<long>(dist(*sim.combat()->find(victim))) : -1L) << " u; bandits alive " << count(false) << "\n";
	if (victim != 0)
	{
		sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{victim}));
		// The player closes on the victim (up to 60 s: the patrol may be most of a km away), then fights it for 5 s.
		for (int closing = 0; closing < 12 && dist(*sim.combat()->find(victim)) > 40.0; ++closing)
			sim.run(100);
		for (int s = 0; s < 1; ++s)
		{
			sim.run(100);
			// Stay in it until something lands on the player (the XP step below needs being hit), up to 30 s more.
			for (int more = 0; more < 6 && healthOf(game.me()) >= 700.0 && !game.me().unconscious; ++more)
				sim.run(100);
			const state::Fighter* v = sim.combat()->find(victim);
			say() << "   +" << (s + 1) * 5 << " s: player hp " << static_cast<long>(healthOf(game.me())) << " target " << game.me().target << " d " << static_cast<long>(dist(*v)) << ", victim hp "
				<< static_cast<long>(healthOf(*v)) << (v->dead ? " DEAD" : "") << (v->unconscious ? " KO" : "") << "\n";
		}
	}
	// ---- 4 run away: break off towards the Hub start and see who follows ----
	const state::FighterSkills afterPick = game.me().skills;
	game.me().orderTarget = 0;
	sim.enqueue(Command::fighterStop(state::Id{game.player}, false));
	const double fromX = game.me().x;
	const double fromZ = game.me().z;
	{
		// 2 km towards the Hub start (the mesh window does not route the whole 4.7 km at once).
		const double len = std::hypot(start.x - fromX, start.z - fromZ);
		double toX = fromX + (start.x - fromX) / len * 2000.0;
		double toZ = fromZ + (start.z - fromZ) / len * 2000.0;
		bool routed = game.walkTo(toX, toZ);
		for (double back = 200.0; !routed && back < 1800.0; back += 200.0)
		{
			toX = fromX + (start.x - fromX) / len * (2000.0 - back);
			toZ = fromZ + (start.z - fromZ) / len * (2000.0 - back);
			routed = game.walkTo(toX, toZ);
		}
		test::require(routed, "a route away from the fight");
		say() << "   running to (" << static_cast<long>(toX) << ", " << static_cast<long>(toZ) << ")\n";
	}
	double chaseEnded = -1.0;
	for (int s = 0; s < 36; ++s)
	{
		sim.run(100);
		std::size_t chasing = 0;
		double nearest = 1.0e18;
		for (const Person& p : membersOf(sim, banditSquad))
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f == nullptr || f->dead || f->unconscious)
				continue;
			chasing += f->target == game.player ? 1 : 0;
			nearest = std::min(nearest, dist(*f));
		}
		if (chasing == 0 && chaseEnded < 0.0)
			chaseEnded = (s + 1) * 5.0;
		if (s < 3)
			say() << "     player walking " << game.me().walking() << " route " << game.me().route.size() << "/" << game.me().routeNext << " state " << static_cast<int>(game.me().state) << " target " << game.me().target
				<< " down " << game.me().down << " ko " << game.me().unconscious << " clip " << game.me().technique << " drive " << static_cast<int>(game.me().drive) << "\n";
		say() << "   run +" << (s + 1) * 5 << " s: " << static_cast<long>(std::hypot(game.me().x - fromX, game.me().z - fromZ)) << " u from the fight, hp " << static_cast<long>(healthOf(game.me()))
			<< (game.me().dead ? " DEAD" : "") << ", bandits targeting the player " << chasing << ", nearest bandit " << static_cast<long>(nearest) << " u\n";
	}
	say() << "4 run away: nobody targets the player after " << chaseEnded << " s\n";
	// Re-pinned for task BV (2026-11-07), with both runs measured. Before BV this check passed vacuously: the player never left the
	// fight (0 u moved for 60 s, 0 chasers), so chaseEnded was its first sample (5 s). With the provoked fight the player really runs
	// the 2 km and the bandits hold the chase until the open-sight give-up (kChaseTrackOpen, 27.5 game minutes ~ 50 sim s) or until
	// the player breaks contact for good; here it ends at 45 s. The bound follows that rule, not the old accidental 30.
	// Re-pinned for the velocity-slew walker (kenshi-k16.8.1): the player now really runs the whole 2027 u at the speed model's commanded
	// 39.4 u/s (gaitSpeed of the hp ~410 body, falling 0.01 u/s per 5 s of bleeding; unchanged code, the master walker commands the same
	// function: 38.7 at its hp), which takes 51.6 s; the bandits hold the chase to the end of the run and drop it at the first sample
	// after it (55 s). Before, the player's walk stopped short and the 50 s bound was only the give-up rule. The bound is the run's own
	// length plus one sample.
	// Re-pinned for the eased route end (kenshi-k16.8.9), timing only: steps 1-3 ended with the player unhurt (hp 550, not ~410), so it runs the 2027 u at
	// the full 70 u/s, is at the end by 30 s, and the four chasers (240-300 u behind) reach it at 35 s and fight it to 65 s; the chase drops at 70 s.
	// Same rule as before (the chase drops at the first sample after the fighting ends); the bound is that fight's end plus one sample.
	test::require(chaseEnded >= 0.0 && chaseEnded <= 75.0, "4: running 2 km from the fight, the chase ends within 75 s");
	test::require(game.me().skills.defence > skillsBefore.defence || game.me().skills.toughness > skillsBefore.toughness, "7: a few seconds of being hit grew defence or toughness");
	say() << "7 XP (after 15 s of fighting and the run): attack " << skillsBefore.attack << " -> " << afterPick.attack << " -> " << game.me().skills.attack << ", defence " << skillsBefore.defence << " -> " << afterPick.defence
		<< " -> " << game.me().skills.defence << ", toughness " << skillsBefore.toughness << " -> " << afterPick.toughness << " -> " << game.me().skills.toughness << "\n";
	return;
	}
	// ---- the fight to its end (or 6 game hours) ----
	// Nobody dies of a hit after the KO, and the untreated sink takes hours (BAR_FIGHT_LOOP Finding 0, ksim_ko_care): every bandit that
	// dies does so at least 2 game hours after it first went down. The original's "about 60 KOs, 0 deaths" playtest window was under
	// an hour, so it is consistent with this.
	// A per-bandit model check (not a seed-sensitive "first death >= 2 h"): each bandit that dies must have gone down first (never died of a
	// hit), no hit may land on it after its KO, and it must outlast what the untreated sink needs to take its lowest vital part from its
	// value at the KO to the death line, at the rate ksim_ko_care pins: 0.05 x 1.5 x 1.2 x lerp(toughness / 100, 1.7, 0.03) per medical
	// second (10 % tolerance), and at least the 1 h sanity floor. The KO-time vital values are printed (live, deep KOs sit at -12 to -16).
	struct KoRecord
	{
		std::uint64_t koTick = 0;
		double vital = 0.0; // the lowest vital part's health at the KO
		double predictedHours = 0.0;
		std::uint64_t deathTick = 0;
		std::vector<std::pair<std::uint64_t, std::uint64_t>> hits; // (tick, hitter) of the hits that landed on it while it was down
		bool diedWithoutKo = false;
	};
	std::map<std::uint64_t, KoRecord> ko;
	std::set<std::uint64_t> bandits;
	for (const Person& p : membersOf(sim, banditSquad))
		bandits.insert(p.id);
	const double medicalClock = Game::settings().medicalClock;
	const auto recordKo = [&](std::uint64_t who)
	{
		KoRecord& r = ko[who];
		r.koTick = sim.clockTick();
		const state::Fighter* f = sim.combat()->find(who);
		if (f == nullptr)
			return;
		const double t5 = 1.7 + (0.03 - 1.7) * std::clamp(f->skills.toughness * 0.01, 0.0, 1.0);
		const double perSecond = 0.05 * 1.5 * 1.2 * t5 * medicalClock;
		double seconds = 1.0e30;
		r.vital = 1.0e9;
		for (const state::FighterPart& part : f->parts)
		{
			if (!part.vital)
				continue;
			r.vital = std::min(r.vital, part.health);
			if (part.health < 0.0)
				seconds = std::min(seconds, (part.maxHealth - part.wear + part.health) / perSecond);
		}
		r.predictedHours = seconds >= 1.0e29 ? 0.0 : seconds / kGameDaySeconds * 24.0;
	};
	// Bodies down with no Knockout event (the medical system drops a body when blood or a vital part runs out) are found by sampling
	// every game minute (and once at the start): the KO is then up to a minute late, which only shortens the model's time.
	std::map<std::uint64_t, std::vector<std::pair<std::uint64_t, std::uint64_t>>> pending;
	std::map<std::uint64_t, bool> wasDown;
	const auto sampleKos = [&]
	{
		for (const std::uint64_t id : bandits)
		{
			const state::Fighter* f = sim.combat()->find(id);
			const bool down = f != nullptr && f->unconscious && !f->dead;
			if (down && wasDown[id])
				for (const auto& hit : pending[id])
					ko[id].hits.push_back(hit);
			pending[id].clear();
			wasDown[id] = down;
			if (f != nullptr && (f->unconscious || f->dead) && ko[id].koTick == 0)
				recordKo(id);
		}
	};
	sampleKos();
	sim.setCombatObserver([&](const CombatEvent& e)
	{
		if (bandits.count(e.who) == 0)
			return;
		if (e.kind == CombatEvent::Kind::Hit)
		{
			// Every hit on a bandit is held until the next minute sample: it counts as a hit on a KO'd body only when the body was down at
			// the sample before and is still down at the one after. (A shallow KO wakes, fights, and is knocked out again, sometimes by a hit
			// that lands in the same tick the medical system has already put it down: that is a KO blow, not a hit after the KO.)
			pending[e.who].emplace_back(sim.clockTick(), e.other);
		} else if (e.kind == CombatEvent::Kind::Knockout) {
			// Hits held so far landed before this KO event (a KO blow, or hits on a woken body that is now put down again): not
			// hits on a body that was down. Every KO transition has its event now, medical ones included.
			pending[e.who].clear();
			// A second KO event is a body that woke (a shallow KO) and was put down again: the sink restarts from this KO, at the vital value it has
			// now (kenshi-k16.8.52: 41221 went down at -4, woke after 0.9 h at -25, took four more hits and was down again at -58, dead 1.5 h
			// later; measured from the first KO the model said 3.7 h, from the last it is within it). The sample's hit check still needs a body down at both samples.
			recordKo(e.who);
		} else if (e.kind == CombatEvent::Kind::Death) {
			KoRecord& r = ko[e.who];
			r.deathTick = sim.clockTick();
			r.diedWithoutKo = r.koTick == 0;
			{
				// What killed it: the body at the moment of death (blood, bleed rate, wounds, the worst limb and vital part).
				const state::Fighter& d = *sim.combat()->find(e.who);
				double worstLimb = 0.0;
				int severed = 0;
				double worstVital = 1.0e9;
				for (const state::FighterPart& part : d.parts)
				{
					if (part.vital)
						worstVital = std::min(worstVital, part.health);
					else
						worstLimb = std::min(worstLimb, part.health);
					severed += part.severed() ? 1 : 0;
				}
				say() << "5 death detail: bandit " << e.who << " blood " << d.blood << " bleed " << d.bleeding << " wounds " << d.wounds.size() << " worst limb " << worstLimb
					<< " worst vital " << worstVital << " severed limbs " << severed << " hunger " << d.hunger << "\n";
			}
			if (r.diedWithoutKo)
				say() << "5 bandit " << e.who << " died with no KO event, unconscious flag " << sim.combat()->find(e.who)->unconscious << " at tick " << sim.clockTick() << ", hit by " << e.other << "\n";
		}
	});
	for (int s = 0; s < 6 * 60 && count(false) > 0; ++s)
	{
		sim.run(kTicksPerGameHour / 60);
		sampleKos();
	}
	for (int s = 0; s < 4 * 60; ++s) // keep looking until a first death shows when it comes
	{
		bool anyDeath = false;
		for (const auto& [id, r] : ko)
			anyDeath = anyDeath || r.deathTick != 0;
		if (anyDeath)
			break;
		sim.run(kTicksPerGameHour / 60);
		sampleKos();
	}
	std::size_t deaths = 0;
	std::size_t bleedOuts = 0;
	std::size_t kos = 0;
	double deepest = 0.0;
	for (const auto& [id, r] : ko)
	{
		if (r.koTick == 0)
			continue;
		++kos;
		deepest = std::min(deepest, r.vital);
		say() << "5 KO bandit " << id << ": vital part " << r.vital << " at the KO, hits after the KO " << r.hits.size();
		if (!r.hits.empty())
			throw test::Failure("5: a hit landed on KO'd bandit " + std::to_string(id) + " (first by " + std::to_string(r.hits.front().second) + ", "
				+ std::to_string(r.hits.front().first - r.koTick) + " ticks after the KO)");
		if (r.deathTick == 0)
		{
			say() << ", alive\n";
			continue;
		}
		++deaths;
		const double actual = static_cast<double>(r.deathTick - r.koTick) / static_cast<double>(kTicksPerGameHour);
		say() << ", dead after " << actual << " game h (sink model predicts " << r.predictedHours << " h)\n";
		if (r.predictedHours <= 0.0)
		{
			// No vital part was below zero at the KO: the body went down of blood loss (a severed limb bleeding, kenshi-k16.7.110: bandit 41225 at vital +16.7,
			// blood -86, bleed 0.28/s) and the untreated sink has nothing to say about it; it bleeds out on its own clock. The hour floor and the sink
			// bound are claims about the sink; a bleed-out is counted, printed and held to the rest (down first, no hit after the KO).
			++bleedOuts;
			continue;
		}
		test::require(actual > 1.0, "5: bandit " + std::to_string(id) + " died within an hour of its KO");
		test::require(actual >= 0.9 * r.predictedHours - 1.0e-9, "5: bandit " + std::to_string(id) + " died sooner than the untreated sink allows ("
			+ std::to_string(actual) + " h, model " + std::to_string(r.predictedHours) + " h)");
	}
	for (const auto& [id, r] : ko)
		test::require(!r.diedWithoutKo, "5: bandit " + std::to_string(id) + " died of a hit without a KO first");
	say() << "5 bleed-outs (blood-loss KOs, not the vital sink): " << bleedOuts << "\n";
	say() << "5 deaths: " << deaths << " of " << kos << " KO'd bandits died, deepest vital part at a KO " << deepest << "\n";
	sim.setCombatObserver(nullptr);
	say() << "   fight ends at " << static_cast<double>(sim.clockTick()) / static_cast<double>(kTicksPerGameHour) << " h: bandits alive " << count(false) << " dead " << count(true) << ", player hp "
		<< static_cast<long>(healthOf(game.me())) << (game.me().dead ? " DEAD" : "") << (game.me().unconscious ? " KO" : "") << "\n";
	for (const std::uint64_t id : crew)
	{
		const state::Fighter& c = *sim.combat()->find(id);
		say() << "   crew " << id << (c.dead ? " DEAD" : c.unconscious ? " KO" : " up") << " at (" << static_cast<long>(c.x) << ", " << static_cast<long>(c.z) << ") building " << c.building << " target " << c.target << "\n";
	}
	for (const Person& p : membersOf(sim, banditSquad))
	{
		const state::Fighter& f = *sim.combat()->find(p.id);
		if (!f.dead)
			say() << "   bandit " << p.id << (f.unconscious ? " KO" : " up") << " at (" << static_cast<long>(f.x) << ", " << static_cast<long>(f.z) << ") building " << f.building << " target " << f.target << " " << static_cast<long>(dist(f)) << " u from the player\n";
	}
	const state::FighterSkills after = game.me().skills;
	say() << "7 XP: attack " << skillsBefore.attack << " -> " << after.attack << ", defence " << skillsBefore.defence << " -> " << after.defence << ", toughness " << skillsBefore.toughness << " -> " << after.toughness
		<< ", dodge " << skillsBefore.dodge << " -> " << after.dodge << "\n";

	if (engage)
		return;

	// ---- 5 loot ----
	std::vector<std::uint64_t> bodies; // dead or knocked out
	std::size_t dead = 0;
	for (const Person& p : membersOf(sim, banditSquad))
	{
		const state::Fighter* f = sim.combat()->find(p.id);
		if (f != nullptr && (f->dead || f->unconscious))
		{
			bodies.push_back(p.id);
			dead += f->dead ? 1 : 0;
		}
	}
	say() << "5 loot: " << dead << " dead and " << bodies.size() - dead << " knocked-out bandits; the player is " << (game.me().dead ? "dead" : game.me().unconscious ? "KO" : "up") << "\n";
	std::map<std::uint64_t, std::vector<Gear>> before;
	std::set<std::uint64_t> wasDead;
	for (const std::uint64_t b : bodies)
	{
		if (sim.combat()->find(b)->dead)
			wasDead.insert(b);
		before[b] = gearOf(sim, b);
		say() << "   body " << b << (sim.combat()->find(b)->dead ? " dead" : " KO") << " at (" << static_cast<long>(sim.combat()->find(b)->x) << ", " << static_cast<long>(sim.combat()->find(b)->z) << "):";
		for (const Gear& g : before[b])
			say() << " [" << g.name << " x" << g.count << " slot " << static_cast<int>(g.slot) << " value " << g.value << "]";
		say() << "\n";
	}
	// Down, not dead: the original's playtest saw about 60 KOs and no deaths (bar_fight_loop.md); our KO'd bandits dying is a known gap (KO care),
	// so death counts are not asserted (BAR_FIGHT_LOOP.md questions).
	test::require(bodies.size() >= 5, "5: the Bar's residents leave at least 5 bandits down (knocked out or dead)");
	for (const std::uint64_t b : bodies)
	{
		bool weapon = false;
		bool armour = false;
		for (const Gear& g : before[b])
		{
			weapon = weapon || g.slot == state::Inventory::kWeapon;
			armour = armour || g.slot >= state::Inventory::kArmourBase;
		}
		test::require(weapon && armour, "5: a built bandit carries its template's weapon and armour");
	}
	// The player walks to the nearest body and loots it (order 60 action 5 with a route, as a session sends it).
	std::uint64_t lootId = 0;
	for (const std::uint64_t b : bodies)
	{
		if (lootFits(sim, game.player, b) && (lootId == 0 || dist(*sim.combat()->find(b)) < dist(*sim.combat()->find(lootId))))
			lootId = b;
	}
	test::require(lootId != 0, "5: some body is one the player's pack can take whole");
	std::vector<Gear> mine = gearOf(sim, game.player);
	{
		const state::Fighter& body = *sim.combat()->find(lootId);
		Route route = sim.navigation()->routeFrom({game.me().x, game.me().y, game.me().z}, body.x, body.z, body.y);
		say() << "   loot route to body " << lootId << " found " << route.found << " from " << static_cast<long>(dist(body)) << " u\n";
		sim.enqueue(Command::fighterInteract(state::Id{game.player}, lootId, 5, 77, std::move(route.points), std::move(route.heights), route.approach));
	}
	std::uint64_t lootedAt = 0;
	for (int i = 1; i <= 600 && lootedAt == 0; ++i)
	{
		sim.run(10);
		lootedAt = gearOf(sim, game.player).size() > mine.size() ? i * 10 : 0;
		// A knocked-out body can wake while the player walks the minute to it, and the order is then refused (not_allowed: the rules, not a
		// walker fault; kenshi-k16.6.271). The player asks again once it is down and the walk is over, as a session would.
		const state::Fighter& me = game.me();
		const state::Fighter& body = *sim.combat()->find(lootId);
		if (lootedAt == 0 && i % 20 == 0 && me.routeNext >= me.route.size() && (body.unconscious || body.dead))
		{
			Route again = sim.navigation()->routeFrom({me.x, me.y, me.z}, body.x, body.z, body.y);
			sim.enqueue(Command::fighterInteract(state::Id{game.player}, lootId, 5, 77, std::move(again.points), std::move(again.heights), again.approach));
		}
	}
	{
		const state::Fighter& body = *sim.combat()->find(lootId);
		for (const Gear& g : gearOf(sim, lootId))
			say() << "   left on the body: " << g.name << " x" << g.count << " slot " << static_cast<int>(g.slot) << "\n";
		for (const Gear& g : gearOf(sim, game.player))
			say() << "   player has: " << g.name << " x" << g.count << " slot " << static_cast<int>(g.slot) << "\n";
		say() << "   body " << lootId << " after the loot wait: " << (body.dead ? "dead" : body.unconscious ? "KO" : "up") << ", " << static_cast<long>(dist(body)) << " u from the player, carried by " << body.carriedBy << ", gear " << gearOf(sim, lootId).size() << ", player " << (game.me().dead ? "dead" : game.me().unconscious ? "KO" : "up") << " route " << game.me().route.size() << "/" << game.me().routeNext << "\n";
	}
	const std::vector<Gear> now = gearOf(sim, game.player);
	test::require(lootedAt != 0 && now.size() > mine.size() && gearOf(sim, lootId).empty(), "5: the player walks to a body and loots it all (order 60 action 5)");
	say() << "5 loot: after " << lootedAt << " ticks the player carries " << now.size() << " entries (before " << mine.size() << "), the body keeps " << gearOf(sim, lootId).size() << "\n";
	for (const Gear& g : now)
		say() << "   player: " << g.name << " x" << g.count << " slot " << static_cast<int>(g.slot) << " value " << g.value << "\n";

	// ---- 6 sell: the barman's quotes for what was taken ----
	{
		flecs::entity barman;
		double best = 1.0e18;
		ecsOf(sim).each([&](flecs::entity squad, const state::TraderStock&, const state::Position& at)
		{
			const double d = std::hypot(at.x - kBarX, at.z - kBarZ);
			if (d < best)
			{
				best = d;
				barman = squad;
			}
		});
		say() << "6 sell: nearest trader squad " << best << " u from the Bar\n";
		if (barman.is_valid())
		{
			const trade::Book book(*sim.gameData(), 1);
			const trade::Terms terms = book.termsOf(barman);
			flecs::entity me;
			ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::Fighter&) { me = id.value == game.player ? e : me; });
			test::require(me.is_valid() && me.get<state::Inventory>() != nullptr, "the player has an inventory");
			for (const state::Inventory::Entry& entry : me.get<state::Inventory>()->items)
			{
				const trade::Quote q = book.quote(terms, entry);
				const kdata::Record* record = sim.gameData()->find(entry.itemSid);
				test::require(record != nullptr && q.refusal == trade::Refusal::None && q.sell > 0, "6: the barman buys looted gear");
				// The premises that hold for every grade: his price is the Book's own gear price for the instance (s72 2a/2c, the model
				// ksim_gear_price pins), and he pays less for it than he sells it for. The record's `value` is only the base of that price:
				// a weapon is priced at its maker and level, so quality gear quotes above its base (the looted Horse Chopper: 1491, paid 372).
				const bool weapon = record != nullptr && record->type == kworld::itemTypeByName("WEAPON");
				const bool made = weapon && entry.quality.set() && !entry.quality.maker.empty();
				const int level = made ? static_cast<int>(std::lround(entry.quality.level * 100.0)) : 0;
				const bool gear = record != nullptr
					&& (weapon || record->type == kworld::itemTypeByName("ARMOUR") || record->type == kworld::itemTypeByName("CROSSBOW"));
				// A faction-uniform armour piece is the one multiplier gearPrice does not carry (s72 4: the quote's x0.3); none of the bar's loot
				// is uniform, and the barman's terms carry no other mult (buy mult and culture 1.0), so the two numbers are the same.
				if (gear && entry.quality.uniform.empty())
					test::require(q.buy == book.gearPrice(*record, made ? entry.quality.maker : std::string(), level), "6: the barman's price is the Book's gear price for the loot");
				test::require(q.sell < q.buy, "6: the barman pays less for the loot than he sells it for");
				say() << "   " << (record != nullptr ? record->name : entry.itemSid) << ": value " << (record != nullptr ? record->intOr("value", 0) : 0) << " barman buys at " << q.sell << " (refusal " << static_cast<int>(q.refusal)
					<< "), would sell at " << q.buy << "\n";
			}
		}
	}

	// ---- 5 persistence: the game is saved and loaded at the bar, the bodies and their gear stay; and when the player leaves, the squad is
	// demoted and, BQ15 (b0's mapping of the exe's "corpse unloaded", ARCHITECTURE.md "Corpses"), the dead bodies are destroyed with
	// their gear (no player area or load range covers them any more); the KO'd are living characters and survive as tokens ----
	const auto bodiesSame = [&](Simulation& s, const char* when, bool playerAway)
	{
		std::size_t same = 0;
		std::size_t missing = 0;
		for (const std::uint64_t b : bodies)
		{
			if (b == lootId)
				continue;
			const state::Fighter* f = s.combat()->find(b);
			if (f == nullptr)
			{
				++missing;
				say() << "   body " << b << " (" << (wasDead.count(b) != 0 ? "was dead" : "was KO") << ") is gone from the arena\n";
				// With the player at the bar nothing may go. Away (14000+ u), the dead are destroyed ("corpse unloaded"); a KO'd body is
				// a living character, a token now, not in the arena.
				test::require(playerAway || wasDead.count(b) == 0, "5: a corpse vanished although the player is at the bar");
				continue;
			}
			const std::vector<Gear> g = gearOf(s, b);
			same += g.size() == before[b].size() && (f->dead || f->unconscious) ? 1 : 0;
			test::require(!playerAway || wasDead.count(b) == 0, "5: a dead body survived the player leaving the area (corpse unloaded)");
			test::require(wasDead.count(b) == 0 || (f->dead && g.size() == before[b].size()), "5: a corpse keeps its gear");
		}
		say() << "5 persistence (" << when << "): " << same << " of " << bodies.size() - 1 << " untouched bodies with their gear, " << missing << " gone from the arena\n";
		return std::make_pair(same, missing);
	};
	double farX = 0.0;
	double farZ = 0.0;
	{
		double best = 1.0e18;
		ecsOf(sim).each([&](const state::SquadInstance&, const state::Position& at)
		{
			const double d = std::hypot(at.x - kBarX, at.z - kBarZ);
			if (d >= 14000.0 && d < best)
			{
				best = d;
				farX = at.x;
				farZ = at.z;
			}
		});
		test::require(best < 1.0e17, "a far point to leave to");
		// Out of every squad's sight: a point 2500 u beside that squad with no other squad within 2000 u (the player only leaves; a hostile mob there
		// would be a second test, and since the join rules only the aggressive ones of it attack, but the one that does can keep a waking player down).
		std::vector<std::pair<double, double>> squads;
		ecsOf(sim).each([&](const state::SquadInstance&, const state::Position& at) { squads.push_back({at.x, at.z}); });
		bool found = false;
		const double baseX = farX;
		const double baseZ = farZ;
		for (int k = 0; k < 16 && !found; ++k)
		{
			const double px = baseX + 2500.0 * std::cos(k * 0.3927);
			const double pz = baseZ + 2500.0 * std::sin(k * 0.3927);
			found = std::none_of(squads.begin(), squads.end(), [&](const auto& q) { return std::hypot(q.first - px, q.second - pz) < 2000.0; });
			if (found)
			{
				farX = px;
				farZ = pz;
			}
		}
		test::require(found, "a far point with no squad within 2000 u");
	}
	const auto playerLine = [&](Simulation& s, const char* when, std::uint64_t who)
	{
		const state::Fighter* p = s.combat()->find(who);
		say() << "5 player " << when << ": tick " << s.clockTick() << " at " << p->x << "," << p->z << " dead " << p->dead << " KO " << p->unconscious << " koTimer " << p->koTimer
			<< " carriedBy " << p->carriedBy << " hp " << healthOf(*p) << " blood " << p->blood << " hash " << std::hex << s.stateHash() << std::dec << "\n";
	};
	playerLine(sim, "before the save", game.player);
	sim.saveGame(std::string(KSIM_BINARY_DIR) + "/bar_fight_bodies.sav");
	Game loaded(bake);
	loaded.load(std::string(KSIM_BINARY_DIR) + "/bar_fight_bodies.sav");
	playerLine(*loaded.sim, "right after the load", loaded.player);
	loaded.sim->run(10);
	playerLine(*loaded.sim, "10 ticks after the load", loaded.player);
	const auto afterLoad = bodiesSame(*loaded.sim, "after save and load", false);
	// The original game: the player leaves. The squad is demoted and the dead bodies are destroyed with their gear (corpse unloaded).
	const std::size_t demotedBefore = sim.residentStats().worldDemotedSquads;
	game.teleport(farX, farZ);
	sim.run(3000);
	say() << "5 persistence: world squads demoted " << sim.residentStats().worldDemotedSquads - demotedBefore << " after the player left\n";
	const auto afterDemotion = bodiesSame(sim, "after the demotion", true);
	// Back at the bodies, in the loaded game: a dead one if there is one, else a knocked-out one, is looted.
	std::uint64_t again = 0;
	double longest = -1.0; // among the knocked-out: the one that stays down longest (a KO'd bandit that wakes mid-loot is a person, not a body)
	for (const std::uint64_t b : bodies)
	{
		const state::Fighter* f = loaded.sim->combat()->find(b);
		if (b == lootId || f == nullptr || before[b].empty() || !(f->dead || f->unconscious))
			continue;
		const double stays = f->dead ? 1.0e18 : f->koTimer;
		if (stays > longest)
		{
			longest = stays;
			again = b;
		}
	}
	test::require(again != 0, "a body is still there to loot");
	// The saved player may be knocked out (the fight left it down); a KO'd player wakes by itself, and only an up player can loot.
	for (int s = 0; s < 4 * 60 && loaded.me().unconscious && !loaded.me().dead; ++s)
		loaded.sim->run(kTicksPerGameHour / 60);
	test::require(!loaded.me().dead && !loaded.me().unconscious, "5: the player is up again after the load");
	const state::Fighter& corpse = *loaded.sim->combat()->find(again);
	loaded.teleport(corpse.x + 4.0, corpse.z);
	loaded.sim->run(2);
	playerLine(*loaded.sim, "before the loot order", loaded.player);
	say() << "5 corpse " << again << " at " << corpse.x << "," << corpse.z << " dead " << corpse.dead << " ko " << corpse.unconscious << "\n";
	// Items, not entries: a second body with the same gear stacks into the player's existing entries.
	const auto itemCount = [&](std::uint64_t who)
	{
		std::uint32_t total = 0;
		for (const Gear& g : gearOf(*loaded.sim, who))
			total += g.count;
		return total;
	};
	const std::uint32_t carried = itemCount(loaded.player);
	loaded.sim->enqueue(Command::fighterInteract(state::Id{loaded.player}, again, 5, 78, {}, {}, false));
	loaded.sim->run(600);
	playerLine(*loaded.sim, "after the loot order", loaded.player);
	say() << "5 persistence: back at the bodies after the load, looting body " << again << ": the player carries " << gearOf(*loaded.sim, loaded.player).size() << " entries (before " << carried << "), the body keeps "
		<< gearOf(*loaded.sim, again).size() << " (had " << before[again].size() << ")\n";
	// Missing bodies: a corpse leaving the arena already failed above, body by body (strict). A knocked-out member is alive, so it leaves the arena with
	// its demoted squad and is rebuilt with it; counting those (the old "at most 1 gone") only made sense while the bodies were corpses.
	test::require(itemCount(loaded.player) > carried, "5: after the load the player loots a body");
	test::require(sim.residentStats().worldDemotedSquads > demotedBefore, "5: the squad was demoted while the corpses stayed");
	(void)afterDemotion;
	(void)afterLoad;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_bar_fight: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		Game game(argv[1]);
		game.newGame();
		Simulation& sim = *game.sim;
		test::require(game.player != 0, "the new game has a player character");
		sim.run(200);
		say() << "player start (" << game.me().x << ", " << game.me().z << ") team " << game.me().team << "\n";
		const state::Fighter start = game.me();

		// ---- the Dust Bandit squad nearest the start with a den ----
		std::uint64_t banditSquad = 0;
		double denX = 0.0;
		double denZ = 0.0;
		{
			double best = 1.0e18;
			ecsOf(sim).each([&](const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
			{
				const double d = std::hypot(at.x - start.x, at.z - start.z);
				if (factionName(sim, instance) == "Dust Bandits" && instance.hasDen && d < best)
				{
					best = d;
					banditSquad = id.value;
					denX = instance.denX;
					denZ = instance.denZ;
				}
			});
		}
		test::require(banditSquad != 0, "a Dust Bandit squad with a den");
		say() << "bandit squad " << banditSquad << " den (" << denX << ", " << denZ << ")\n";
		ecsOf(sim).each([&](const state::Id& id, const state::SquadInstance& instance)
		{
			if (id.value != banditSquad)
				return;
			const kdata::Record* tpl = sim.gameData()->find(instance.templateSid);
			const state::FactionState* faction = ecsOf(sim).is_alive(instance.faction.id) ? ecsOf(sim).entity(instance.faction.id).get<state::FactionState>() : nullptr;
			say() << "   squad template " << instance.templateSid << " (" << (tpl != nullptr ? tpl->name : "?") << "), faction " << (faction != nullptr ? faction->templateSid : "?") << ", source " << static_cast<int>(instance.source)
				<< ", slot " << instance.slot << ", nest " << instance.nest << "\n";
		});

		auto bandits = [&] { return membersOf(sim, banditSquad); };
		auto aliveBandits = [&]
		{
			std::vector<std::uint64_t> ids;
			for (const Person& p : bandits())
			{
				const state::Fighter* f = sim.combat()->find(p.id);
				if (f != nullptr && !f->dead && !f->unconscious)
					ids.push_back(p.id);
			}
			return ids;
		};
		auto leaderPos = [&](double& x, double& z)
		{
			for (const Person& p : bandits())
			{
				const state::Fighter* f = sim.combat()->find(p.id);
				if (f != nullptr && !f->dead)
				{
					x = f->x;
					z = f->z;
					return true;
				}
			}
			return false;
		};

		// ---- 1 patrol: after three game hours the squad is away from the den ----
		// Three: its first patrol point is past the leash by the way round (ksim_world_hub), so it leaves on the next one.
		sim.run(3 * kTicksPerGameHour);
		{
			double x = 0.0;
			double z = 0.0;
			test::require(leaderPos(x, z), "the bandits are built");
			// The first patrol point can be inside the leash for a given draw (kenshi-k16.6.33: the town populate's draws moved the squad's);
			// the squad leaves on the next ones, so give it up to six more hours.
			for (int extra = 0; extra < 6 && std::hypot(x - denX, z - denZ) <= 200.0; ++extra)
			{
				sim.run(kTicksPerGameHour);
				test::require(leaderPos(x, z), "the bandits are built");
			}
			const double away = std::hypot(x - denX, z - denZ);
			say() << "1 patrol: tick " << sim.clockTick() << ", leader (" << x << ", " << z << ") " << away << " u from the den, " << aliveBandits().size() << " alive\n";
			test::require(away > 200.0 && aliveBandits().size() == 15, "1: the patrol has left the den, all 15 alive");
		}

		// The walk-by (regression: a patrolling world squad is carried by a move order and follow tasks, which used to skip target picking; the
		// Dust Bandits ignored a player 40 to 500 u away): 100 u from the walking leader, the squad targets the player within seconds.
		{
			Game other(argv[1]);
			other.newGame();
			other.sim->run(200 + kTicksPerGameHour / 2);
			const WalkBy w = walkBy(other, banditSquad, 100.0);
			say() << "2 walk-by: leader walking " << w.leaderWalking << ", walking members " << w.walkingMembers << ", followers " << w.followers << ", distance " << w.distance << " u, bandits targeting the player "
				<< w.aggro << " (first after " << w.firstAggroTick << " ticks)\n";
			test::require(w.leaderWalking && w.followers >= 10, "2: the walk-by finds the squad carried (leader walking, the rest following)");
			test::require(w.aggro >= 10 && w.firstAggroTick < 100, "2: a provoked player 100 u from a walking patrol is targeted by most of it within 5 s");
		}
		// ---- 2 lure: walk up to the patrol until one of them has the player as its target (task BV: the player provokes first; the
		// Dust Bandits' own relation to a new player is -10, neutral, so the approach alone starts nothing) ----
		auto chasers = [&]
		{
			std::vector<std::uint64_t> ids;
			for (const std::uint64_t id : aliveBandits())
			{
				if (sim.combat()->find(id)->target == game.player)
					ids.push_back(id);
			}
			return ids;
		};
		auto nearestBandit = [&]
		{
			double best = 1.0e18;
			for (const std::uint64_t id : aliveBandits())
				best = std::min(best, std::hypot(sim.combat()->find(id)->x - game.me().x, sim.combat()->find(id)->z - game.me().z));
			return best;
		};
		// The provocation (task BV): the player attacks the nearest bandit; the order is the original's personal type-4 flip, and the
		// patrol answers. The walk orders in the loop below replace the attack order, the enmity stays.
		{
			std::uint64_t mark = 0;
			double markDistance = 1.0e18;
			for (const std::uint64_t id : aliveBandits())
			{
				const state::Fighter* f = sim.combat()->find(id);
				if (f == nullptr || f->dead)
					continue;
				const double d = std::hypot(f->x - game.me().x, f->z - game.me().z);
				if (d < markDistance) { markDistance = d; mark = id; }
			}
			test::require(mark != 0, "2 lure: the patrol has a bandit to provoke");
			sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{mark}));
			sim.run(2);
		}
		double closest = 1.0e18;
		const std::uint64_t lureStart = sim.clockTick();
		std::uint64_t noticedTick = 0;
		for (std::uint64_t i = 0; i < 40000 && noticedTick == 0; ++i)
		{
			if (i % 100 == 0)
			{
				double x = 0.0;
				double z = 0.0;
				if (leaderPos(x, z) && std::hypot(x - game.me().x, z - game.me().z) > 20.0)
					game.walkTo(x, z);
			}
			sim.run(1);
			closest = std::min(closest, nearestBandit());
			if (!chasers().empty())
				noticedTick = sim.clockTick();
			if (i % 400 == 0)
				say() << "  approach t+" << i / 20 << " s: player (" << static_cast<long>(game.me().x) << ", " << static_cast<long>(game.me().z) << ") nearest bandit " << static_cast<long>(nearestBandit()) << " u\n";
		}
		say() << "2 lure: noticed at tick " << noticedTick << " (" << (noticedTick - lureStart) / 20 << " s of walking), closest bandit " << static_cast<long>(closest) << " u, chasers " << chasers().size() << "\n";
		test::require(noticedTick != 0, "2: the provoked patrol targets the player");
		say() << "   at notice: nearest bandit " << static_cast<long>(nearestBandit()) << " u\n";

		// ---- lure to the Bar: run there and watch ----
		auto barDistance = [&] { return std::hypot(game.me().x - kBarX, game.me().z - kBarZ); };
		auto engagedResidents = [&]
		{
			std::size_t n = 0;
			const std::vector<Person> bs = bandits();
			for (const CombatArena::Member& m : sim.combat()->members())
			{
				if (m.fighter.team != 0 || m.fighter.playerControlled || m.fighter.dead || m.fighter.target == 0)
					continue;
				n += std::any_of(bs.begin(), bs.end(), [&](const Person& p) { return p.id == m.fighter.target; }) ? 1 : 0;
			}
			return n;
		};
		test::require(game.walkTo(kBarX, kBarZ), "a route to the Bar");
		const std::uint64_t runStart = sim.clockTick();
		std::uint64_t firstResidentFight = 0;
		std::uint64_t firstDeath = 0;
		for (std::uint64_t i = 0; i < 6000; ++i)
		{
			sim.run(1);
			if (firstResidentFight == 0 && engagedResidents() > 0)
				firstResidentFight = sim.clockTick() - runStart;
			if (firstDeath == 0 && aliveBandits().size() < 15)
				firstDeath = sim.clockTick() - runStart;
			if (i < 160 && i % 20 == 0)
			{
				for (const Person& p : bandits())
				{
					const state::Fighter* f = sim.combat()->find(p.id);
					if (f == nullptr || f->dead || (f->target == 0 && std::hypot(f->x - game.me().x, f->z - game.me().z) > 150.0))
						continue;
					say() << "     b" << p.id << " d " << static_cast<long>(std::hypot(f->x - game.me().x, f->z - game.me().z)) << " target " << f->target << " state " << static_cast<int>(f->state)
						<< " walking " << f->walking() << " chase " << f->routeChase << " follow " << f->followTarget << " seen " << perception::aware(*f, game.player) << "\n";
				}
			}
			if (i % 100 == 0)
			{
				say() << "  run t+" << i / 20 << " s: player bar+" << static_cast<long>(barDistance()) << " u hp " << static_cast<long>(healthOf(game.me())) << (game.me().unconscious ? " KO" : "")
					<< (game.me().dead ? " DEAD" : "") << ", bandits alive " << aliveBandits().size() << ", chasing the player " << chasers().size() << ", nearest " << static_cast<long>(nearestBandit())
					<< " u, residents fighting bandits " << engagedResidents() << "\n";
			}
		}
		say() << "2 lure to the bar: first resident-vs-bandit fight after " << firstResidentFight << " ticks, first bandit death after " << firstDeath << " ticks\n";

		barFight(argv[1], banditSquad, true);
		barFight(argv[1], banditSquad, false);
		std::cout << "ksim_bar_fight: ok (walk-by aggro, lure, bar fight, run away, loot, persistence, quotes)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_bar_fight: FAILED: " << e.what() << "\n";
		return 1;
	}
}
