// Fights across floors (tasks AQ and AS, ENGAGEMENT.md "Fights across floors"): a fight's distance is the walk, not
// the crow, and a chaser takes the stair to a target on another floor.
// The Watchtower (4104-D-TwoStorey.mod, four floors and a winding staircase) and a Storm House (3384-TwoStorey.mod,
// whose roof is a walkless island) on knav_tower's flat site, baked here.
//  1. no hit through a floor (AQ): the player on the ground floor and an enemy directly above it on floor 1 (128 u up;
//     the walk is the stair, about 520 u). The player provokes it: no hit lands through the ceiling.
//  2. the NPC climbs (AS): the player stands on floor 1 and provokes an NPC on the ground floor (only the player is
//     ordered). The NPC's chase plans to the player's 3D point, so it walks the stair up and the two fight.
//  3. no route (AS): the player on the Storm House's roof island, an NPC on the ground: no walk reaches the player,
//     so the NPC drops the target within a few seconds instead of standing under it.
//
// The AQ part used to order the player up the stair; that is now the fighter's own chase (routeFrom with the target's
// click height), which is the AS change.

#include "ksim/combat.hpp"
#include "ksim/command.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include "knav/world.hpp"
#include "kworld/collision.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// knav_tower's flat, dry, open site; the Watchtower's floor heights are checked against the bake below, and the
// Storm House stands 400 u north-east of it (its roof was found walkable and unreachable there on 2026-11-06).
constexpr double kSiteX = -85800.0;
constexpr double kSiteZ = -28400.0;
constexpr double kFloorGapMin = 120.0; // ground floor to floor 1 (the tower's 127), at the same x and z
constexpr double kHouseX = 400.0;
constexpr double kHouseZ = 400.0;
constexpr double kOneFloor = 20.0; // kFloorApart: a hit within this height gap is on one floor

const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		if (r->name == name)
			return r;
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

// The walk between two navmesh points: the route sum with the gap from its end added.
double routeLength(const Route& route, const knav::Position& from, const knav::Position& to)
{
	if (!route.found)
		return std::numeric_limits<double>::infinity();
	double px = from.x;
	double py = from.y;
	double pz = from.z;
	double length = 0.0;
	for (std::size_t i = 0; i < route.points.size(); ++i)
	{
		length += std::hypot(std::hypot(route.points[i].x - px, route.heights[i] - py), route.points[i].z - pz);
		px = route.points[i].x;
		py = route.heights[i];
		pz = route.points[i].z;
	}
	return length + std::hypot(std::hypot(to.x - px, to.y - py), to.z - pz);
}

// The game data and the baked mesh, shared by the three cases (one bake, three fresh simulations).
struct Tower
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	double siteY = 0.0;
	knav::Position low; // the ground floor spot
	knav::Position high; // the floor-1 spot directly above it
	knav::Position roof; // the Storm House's roof island

	Tower()
	{
		const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		siteY = terrain.height(kSiteX, kSiteZ);
		gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		data = CombatData::build(*gd, KENSHI_DATA);
		kworld::CollisionLibrary library(KENSHI_DATA, *gd);
		const auto place = [&](const char* sid, const char* id, double x, double z, std::vector<kworld::WorldShape>& shapes)
		{
			const kdata::Record* record = gd->find(sid);
			test::require(record != nullptr, std::string("the building record ") + sid);
			kworld::PlacedInstance placed;
			placed.instanceID = id;
			placed.buildingID = record->stringID;
			placed.scale = record->floatOr("scale", 1.0f);
			placed.x = kSiteX + x;
			placed.y = siteY;
			placed.z = kSiteZ + z;
			kworld::CollisionCoverage coverage;
			const auto nav = library.addBuilding(placed, shapes, coverage);
			test::require(nav.has_value(), std::string("collision for ") + sid);
			return *nav;
		};
		std::vector<kworld::WorldShape> towerShapes;
		std::vector<kworld::WorldShape> houseShapes;
		const kworld::BuildingNav towerNav = place("4104-D-TwoStorey.mod", "aq-tower", 0.0, 0.0, towerShapes);
		const kworld::BuildingNav houseNav = place("3384-TwoStorey.mod", "aq-house", kHouseX, kHouseZ, houseShapes);

		const kworld::ZoneCell cell = kworld::zoneCellOf(kSiteX, kSiteZ);
		knav::NavWorld world(knav::configForZones(cell, cell), terrain,
			std::make_shared<const std::vector<kworld::WorldShape>>());
		world.bakeAll(4);
		world.setBuilding("tower", std::make_shared<const std::vector<kworld::WorldShape>>(std::move(towerShapes)), towerNav);
		world.setBuilding("house", std::make_shared<const std::vector<kworld::WorldShape>>(std::move(houseShapes)), houseNav);
		auto job = world.prepareRebuild();
		job->run(4);
		world.apply(*job);
		nav = std::make_shared<MeshNavigator>(knav::NavMesh::fromTiles(world.mesh().config(), world.mesh().tiles()));

		// The two floors share x and z (measured 2026-11-06: every probe point of the tower has both walkable).
		const auto a = nav->walker().snap({ static_cast<float>(kSiteX), static_cast<float>(siteY + 5.0), static_cast<float>(kSiteZ - 20.0) }, 30.0);
		const auto b = nav->walker().snap({ static_cast<float>(kSiteX), static_cast<float>(siteY + 132.0), static_cast<float>(kSiteZ - 20.0) }, 30.0);
		test::require(a.has_value() && b.has_value(), "the tower's ground floor and floor 1 are on the mesh at x 0, z -20");
		low = *a;
		high = *b;
		test::require(high.y - low.y > kFloorGapMin, "floor 1 is a real floor above the ground below it");
		// The Storm House's roof: walkable over the middle of the building, and no walk reaches it from the ground.
		const auto r = nav->walker().snap({ static_cast<float>(kSiteX + kHouseX), static_cast<float>(siteY + 40.0),
			static_cast<float>(kSiteZ + kHouseZ) }, 40.0);
		test::require(r.has_value() && r->y - siteY > 40.0, "the Storm House has a walkable roof island");
		roof = *r;
		const Route reach = nav->routeFrom(low, roof.x, roof.z, roof.y);
		test::require(!reach.found, "no walk reaches the Storm House's roof (the island is the point of case 3)");
	}
};

struct Rig
{
	const Tower& tower;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	std::uint64_t enemy = 0;
	std::uint32_t seq = 0;
	std::vector<CombatEvent> events;

	explicit Rig(const Tower& t)
		: tower(t)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(tower.gd);
		sim->setCombatObserver([this](const CombatEvent& e) { events.push_back(e); });
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(tower.data, settings);
		sim->enableNavigation(tower.nav);
	}

	std::uint64_t add(const char* character, std::uint64_t team, const knav::Position& at, bool playerControlled)
	{
		static std::uint64_t rng = 4242;
		state::Fighter f = tower.data->makeFighter(*tower.gd, recordNamed(*tower.gd, "CHARACTER", character)->stringID, "", ++rng);
		f.x = at.x;
		f.y = at.y;
		f.z = at.z;
		f.team = team;
		f.playerControlled = playerControlled;
		f.attention = 0.5;
		flecs::entity e = sim->spawnFighter(std::move(f));
		return e.get<state::Id>()->value;
	}

	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }

	void tick()
	{
		sim->tick();
	}

	// Landed hits on `victim` by `attacker`.
	int hits(std::uint64_t victim, std::uint64_t attacker) const
	{
		int n = 0;
		for (const CombatEvent& e : events)
			if (e.kind == CombatEvent::Kind::Hit && e.who == victim && e.other == attacker)
				++n;
		return n;
	}

	int hitsBetween() const { return hits(player, enemy) + hits(enemy, player); }

	// The height gap at every hit of the pair, in order: a hit through a floor would be a gap over kOneFloor.
	std::vector<double> hitGaps(std::size_t from)
	{
		std::vector<double> gaps;
		for (std::size_t i = from; i < events.size(); ++i)
		{
			const CombatEvent& e = events[i];
			if (e.kind == CombatEvent::Kind::Hit
				&& ((e.who == player && e.other == enemy) || (e.who == enemy && e.other == player)))
				gaps.push_back(std::fabs(f(player).y - f(enemy).y));
		}
		return gaps;
	}

	double up(std::uint64_t id) { return f(id).y - tower.siteY; }
};

} // namespace

int main()
{
	try
	{
		Tower tower;
		std::printf("tower baked at y %.1f; ground %.1f, floor 1 %.1f, storm house roof %.1f\n",
			tower.siteY, tower.low.y - tower.siteY, tower.high.y - tower.siteY, tower.roof.y - tower.siteY);
		{
			const double walk = routeLength(tower.nav->routeFrom(tower.low, tower.high.x, tower.high.z, tower.high.y),
				tower.low, tower.high);
			std::printf("the tower's walk between the floors: %.1f u\n", walk);
			test::require(walk > 400.0, "the walk between the floors is the stair (measured 515 u on this tower)");
		}

		// 1. No hit through a floor (AQ): the player below an enemy directly above, provoked. Under the old
		// straight-line gates the horizontal distance was 0 and the two fought through the floor. The chasers may
		// now walk the stair to each other; every hit must still land with the two within one floor.
		{
			Rig rig(tower);
			rig.player = rig.add("Escaped Servant", 0, tower.low, true);
			rig.enemy = rig.add("Escaped Servant", 1, tower.high, false);
			rig.sim->enqueue(Command::fighterAttack(state::Id{ rig.player }, state::Id{ rig.enemy }));
			std::vector<double> gaps;
			for (int n = 0; n < 400 && gaps.empty(); ++n)
			{
				const std::size_t before = rig.events.size();
				rig.tick();
				const auto fresh = rig.hitGaps(before);
				gaps.insert(gaps.end(), fresh.begin(), fresh.end());
			}
			const double firstGap = gaps.empty() ? -1.0 : gaps.front();
			std::printf("  1. provoked from the floor below: first hit %s (height gap %.1f), player up %.1f, enemy up %.1f\n",
				gaps.empty() ? "none in 20 s" : "landed", firstGap, rig.up(rig.player), rig.up(rig.enemy));
			test::require(gaps.empty() || firstGap <= kOneFloor,
				"no hit lands through the floor (every hit is within one floor of height)");
		}

		// 2. The NPC climbs (AS): the player on floor 1 provokes an NPC on the ground; its own chase takes the stair.
		{
			Rig rig(tower);
			rig.player = rig.add("Escaped Servant", 0, tower.high, true);
			rig.enemy = rig.add("Escaped Servant", 1, tower.low, false);
			rig.sim->enqueue(Command::fighterAttack(state::Id{ rig.player }, state::Id{ rig.enemy }));
			int climbedAt = -1;
			int firstHit = -1;
			for (int n = 0; n < 4000 && firstHit < 0; ++n)
			{
				rig.tick();
				if (climbedAt < 0 && rig.up(rig.enemy) > 60.0)
					climbedAt = n;
				if (rig.hitsBetween() > 0)
					firstHit = n;
			}
			const auto gaps = rig.hitGaps(0);
			std::printf("  2. the NPC on the ground: up at tick %d, first hit at tick %d, enemy up %.1f, first gap %.1f\n",
				climbedAt, firstHit, rig.up(rig.enemy), gaps.empty() ? -1.0 : gaps.front());
			test::require(climbedAt >= 0, "the NPC takes the stair to the player on floor 1");
			test::require(firstHit >= 0, "the two fight once the NPC is up");
			test::require(gaps.front() <= kOneFloor, "the first hit is on one floor (the stair connected them)");
		}

		// 3. No route (AS): the player on the Storm House's roof island; no walk reaches it, so the NPC gives the
		// target up instead of standing under it.
		{
			Rig rig(tower);
			rig.player = rig.add("Escaped Servant", 0, tower.roof, true);
			rig.enemy = rig.add("Escaped Servant", 1, tower.low, false);
			rig.sim->enqueue(Command::fighterAttack(state::Id{ rig.player }, state::Id{ rig.enemy }));
			int droppedAt = -1;
			for (int n = 0; n < 400 && droppedAt < 0; ++n)
			{
				rig.tick();
				if (rig.f(rig.enemy).target == 0)
					droppedAt = n;
			}
			const state::Fighter& npc = rig.f(rig.enemy);
			std::printf("  3. the player on a roof island: the NPC dropped it at tick %d (%.1f s), blocked for %.0f s, hits %d\n",
				droppedAt, droppedAt * Simulation::kTickSeconds, npc.aggroBlock, rig.hitsBetween());
			test::require(droppedAt >= 0, "the NPC gives up a target no walk reaches, instead of standing under it");
			test::require(npc.aggroBlockId == rig.player && npc.aggroBlock > 0.0, "the unreachable target is blocked (addBlockedTarget)");
			test::require(rig.hitsBetween() == 0, "no hit lands on a roof island");
		}

		std::printf("ksim_melee_floors ok\n");
	} catch (const std::exception& e)
	{
		std::cerr << "ksim_melee_floors FAILED: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
