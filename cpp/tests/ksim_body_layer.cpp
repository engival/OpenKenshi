// The body layer (task AW; OURS -- the original resolves bodies with ragdoll physics): a lying body has its own collision layer.
//   - bodies (KO'd, dead, ragdolled) push only each other: a pile put down at one point spreads to the body spacing, slowly, in Id
//     order and on the navmesh, and comes to rest (no jitter);
//   - an upright walks over a body: it is neither pushed by one nor pushes it (the same walk with and without it);
//   - a body in a bed or a cage keeps its place (they take no part in the pass);
//   - a body put down lands beside the pile, not on it;
//   - a body inside a building stays inside, and a save/load mid-spread continues the same.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/combat/combat.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
// The Hub's open ground (ksim_move's spot) and the Storm House interior (ksim_building_membership's).
constexpr double kHouseX = -51192.0;
constexpr double kHouseZ = 3192.0;
constexpr double kHouseFloorY = 1576.5;

std::string num(double v)
{
	char buffer[64];
	std::snprintf(buffer, sizeof buffer, "%.3f", v);
	return buffer;
}

// A bare combat arena: the body pass on its own, without a world to interfere.
struct ArenaRig
{
	const kdata::GameData& gd;
	std::shared_ptr<const CombatData> data;
	const kdata::Record* servant = nullptr;
	CombatArena arena;
	std::uint64_t rng = 7;
	std::uint64_t nextId = 1;

	ArenaRig(const kdata::GameData& g, std::shared_ptr<const CombatData> d, std::uint64_t seed,
		std::shared_ptr<const MeshNavigator> nav = nullptr)
		: gd(g)
		, data(std::move(d))
		, arena(data, CombatSettings{}, seed)
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
		if (nav != nullptr)
			arena.setNavigation(std::move(nav));
	}
	state::Fighter fighter(double x, double z, double y)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.x = x;
		f.z = z;
		f.y = y;
		f.playerControlled = true; // picks no fights: a stand-in body or a walker
		return f;
	}
	std::uint64_t spawnUpright(double x, double z, double y)
	{
		const std::uint64_t id = nextId++;
		arena.add(id, fighter(x, z, y));
		return id;
	}
	std::uint64_t spawnBody(double x, double z, double y)
	{
		state::Fighter f = fighter(x, z, y);
		f.unconscious = true;
		f.koTimer = 1.0e6;
		const std::uint64_t id = nextId++;
		arena.add(id, std::move(f));
		return id;
	}
	state::Fighter& f(std::uint64_t id) { return *arena.find(id); }
	double apart(std::uint64_t a, std::uint64_t b)
	{
		return std::hypot(f(a).x - f(b).x, f(a).z - f(b).z);
	}
	void run(int steps)
	{
		for (int i = 0; i < steps; ++i)
			arena.step(kDt);
	}
};

// The whole world on the Hub mesh: building membership, the put-down spot and a save/load across the spread.
struct WorldRig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::unique_ptr<Simulation> sim;
	const kdata::Record* servant = nullptr;
	std::uint64_t rng = 7;

	WorldRig(std::shared_ptr<const kdata::GameData> g, std::shared_ptr<const CombatData> d, std::shared_ptr<MeshNavigator> n)
		: gd(std::move(g))
		, data(std::move(d))
		, nav(std::move(n))
		, sim(std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 77)))
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim->setGameData(gd);
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				servant = r;
		}
		test::require(servant != nullptr, "no Escaped Servant");
	}
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	std::uint64_t spawnBody(double x, double y, double z)
	{
		state::Fighter fighter = data->makeFighter(*gd, servant->stringID, "", rng);
		fighter.x = x;
		fighter.y = y;
		fighter.z = z;
		fighter.playerControlled = true; // the residents have no business with a player character
		fighter.unconscious = true;
		fighter.koTimer = 1.0e6;
		return sim->spawnFighter(std::move(fighter)).get<state::Id>()->value;
	}
};

// An upright of our own at a point, as the carrier for the put-down test (playerControlled: the world leaves it alone).
std::uint64_t spawnUpright(WorldRig& rig, double x, double y, double z)
{
	state::Fighter fighter = rig.data->makeFighter(*rig.gd, rig.servant->stringID, "", rig.rng);
	fighter.x = x;
	fighter.y = y;
	fighter.z = z;
	fighter.playerControlled = true;
	return rig.sim->spawnFighter(std::move(fighter)).get<state::Id>()->value;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_body_layer HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		test::require(nav != nullptr, "the Hub mesh did not load");

		// ---- 1. five bodies put down at one point spread to the body spacing, then stop ----
		{
			ArenaRig rig(*gd, data, 5);
			std::vector<std::uint64_t> bodies;
			for (int i = 0; i < 5; ++i)
				bodies.push_back(rig.spawnBody(0.0, 0.0, 0.0));
			int steps = 0;
			const int kLimit = 600;
			bool settled = false;
			while (steps < kLimit && !settled)
			{
				std::vector<double> before;
				for (std::uint64_t id : bodies)
					before.push_back(rig.f(id).x);
				rig.run(1);
				++steps;
				settled = true;
				for (std::size_t i = 0; i < bodies.size() && settled; ++i)
					settled = rig.f(bodies[i]).x == before[i];
			}
			test::require(settled, "the pile never came to rest within " + std::to_string(kLimit) + " steps");
			double closest = 1.0e9;
			for (std::size_t i = 0; i < bodies.size(); ++i)
				for (std::size_t j = i + 1; j < bodies.size(); ++j)
					closest = std::min(closest, rig.apart(bodies[i], bodies[j]));
			// The pass rests a little short of the spacing: the push is dropped below 0.02, 0.13 of overlap at its rate.
			test::require(closest >= CombatArena::kBodySpacing - 0.2,
				"the bodies stack at " + num(closest) + ", body spacing " + num(CombatArena::kBodySpacing));
			// Rest means no further move: the pass pushes nothing at the spacing.
			std::vector<double> xs;
			for (std::uint64_t id : bodies)
				xs.push_back(rig.f(id).x);
			rig.run(50);
			for (std::size_t i = 0; i < bodies.size(); ++i)
				test::require(rig.f(bodies[i]).x == xs[i], "a settled body moved again (jitter)");
			std::printf("ok  bodies: five at one point, closest %.3f (spacing %.1f) after %d steps, then still\n", closest,
				CombatArena::kBodySpacing, steps);
			// Deterministic: the same pile (the pass has no random draw) ends exactly there.
			ArenaRig again(*gd, data, 5);
			for (int i = 0; i < 5; ++i)
				again.spawnBody(0.0, 0.0, 0.0);
			again.run(steps);
			for (std::size_t i = 0; i < bodies.size(); ++i)
				test::require(again.f(bodies[i]).x == rig.f(bodies[i]).x && again.f(bodies[i]).z == rig.f(bodies[i]).z,
					"the same pile is not deterministic in Id order");
		}

		// ---- 2. a body in a bed or a cage keeps its place ----
		{
			ArenaRig rig(*gd, data, 5);
			const std::uint64_t upright = rig.spawnUpright(0.0, 0.0, 0.0);
			const std::uint64_t asleep = rig.spawnBody(0.5, 0.0, 0.0);
			const std::uint64_t caged = rig.spawnBody(0.0, 0.5, 0.0);
			rig.f(asleep).asleep = true; // the bed stand-in (the +0x2f8 flag)
			rig.f(caged).cageBuilding = 42;
			rig.f(caged).cageSlot = 0;
			const double asleepX = rig.f(asleep).x;
			const double cagedZ = rig.f(caged).z;
			rig.run(100);
			test::require(rig.f(asleep).x == asleepX && rig.f(asleep).z == 0.0, "a body in a bed was moved");
			test::require(rig.f(caged).x == 0.0 && rig.f(caged).z == cagedZ, "a caged body was moved");
			test::require(rig.f(upright).x == 0.0 && rig.f(upright).z == 0.0, "an upright was moved by bodies");
			std::printf("ok  place: a bed body and a caged body keep their place\n");
		}

		// ---- 2b. a conscious down crawler is a depenetration neighbour, not a body (kinspect bar_pile_throwout.md 4) ----
		{
			ArenaRig rig(*gd, data, 5);
			const std::uint64_t walker = rig.spawnUpright(0.0, 0.0, 0.0);
			const std::uint64_t crawler = rig.spawnUpright(1.0, 0.0, 0.0);
			rig.f(crawler).down = true;
			const double radius = 2.0 * rig.data->race(rig.f(walker).raceSid)->footprintRadius; // R = 4 + 4 for humans
			rig.run(40);
			const double apart = rig.apart(walker, crawler);
			test::require(std::fabs(apart - radius) < 0.06,
				"a crawler holds a walker at the standing distance R (" + num(apart) + ", R " + num(radius) + ")");
			// A KO'd body at the same distance: no push either way, the walker stands on it.
			ArenaRig body(*gd, data, 5);
			const std::uint64_t w = body.spawnUpright(0.0, 0.0, 0.0);
			const std::uint64_t b = body.spawnBody(1.0, 0.0, 0.0);
			body.run(40);
			test::require(body.apart(w, b) == 1.0, "a KO'd body pushed or was pushed by an upright");
			std::printf("ok  crawler: held a walker at %.2f (R %.1f), a KO'd body did not\n", apart, radius);
		}

		// ---- 3. a walker crosses a body's spot without being pushed ----
		{
			const auto start = nav->walker().snap({kHubX, 1535.0, kHubZ}, 200.0);
			test::require(start.has_value(), "no mesh near the Hub");
			const auto end = nav->walker().snap({start->x + 20.0, start->y, start->z}, 40.0);
			test::require(end.has_value(), "no mesh 20 u east of the Hub spot");
			const double midX = 0.5 * (start->x + end->x);
			const double midZ = 0.5 * (start->z + end->z);
			const auto mid = nav->walker().snap({midX, start->y, midZ}, 8.0);
			test::require(mid.has_value(), "no mesh at the route's middle");

			std::vector<std::vector<double>> path[2]; // the walker's x, y, z each step: with a body, without
			double bodyX = 0.0, bodyZ = 0.0;
			for (int with = 0; with < 2; ++with)
			{
				ArenaRig rig(*gd, data, 5, nav);
				const std::uint64_t walker = rig.spawnUpright(start->x, start->z, start->y);
				state::Fighter& f = rig.f(walker);
				f.route = {{start->x, start->y, start->z}, {end->x, end->y, end->z}};
				f.routeNext = 0;
				if (with)
				{
					const std::uint64_t body = rig.spawnBody(mid->x, mid->z, mid->y);
					bodyX = rig.f(body).x;
					bodyZ = rig.f(body).z;
				}
				for (int i = 0; i < 60; ++i)
				{
					rig.run(1);
					path[with].push_back({rig.f(walker).x, rig.f(walker).y, rig.f(walker).z});
				}
				if (with)
				{
					state::Fighter* body = nullptr;
					for (const CombatArena::Member& m : rig.arena.members())
					{
						if (m.id != walker)
							body = const_cast<state::Fighter*>(&m.fighter);
					}
					test::require(body != nullptr, "the body vanished");
					test::require(body->x == bodyX && body->z == bodyZ, "the walker pushed the body");
				}
			}
			test::require(std::fabs(path[0].back()[0] - start->x) > 5.0, "the walker never moved");
			for (std::size_t i = 0; i < path[0].size(); ++i)
				for (int k = 0; k < 3; ++k)
					test::require(path[0][i][k] == path[1][i][k], "a body in the way changed the walk at step " + std::to_string(i));
			std::printf("ok  walker: crossed the body's spot, the same walk step for step with and without it, body unmoved\n");
		}

		// ---- 4. a body inside a building stays inside; a save/load mid-spread continues the same ----
		{
			WorldRig rig(gd, data, nav);
			const BuildingSite* house = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
			{
				if (std::hypot(site.cx - kHouseX, site.cz - kHouseZ) < 30.0)
					house = &site;
			}
			test::require(house != nullptr, "no building site at the Storm House");
			const std::uint64_t key = house->key;
			const auto spot = nav->walker().snap({kHouseX, kHouseFloorY, kHouseZ}, 60.0);
			test::require(spot.has_value(), "no mesh inside the Storm House");
			const std::uint64_t a = rig.spawnBody(spot->x, spot->y, spot->z);
			const std::uint64_t b = rig.spawnBody(spot->x, spot->y, spot->z);
			rig.tick(1);
			test::require(rig.f(a).building == key && rig.f(b).building == key,
				"a body spawned in the Storm House is not in it (" + std::to_string(rig.f(a).building) + ")");
			// The tick above already stepped the arena: the two are partway apart, not yet at the spacing (mid-spread).
			const double mid = std::hypot(rig.f(a).x - rig.f(b).x, rig.f(a).z - rig.f(b).z);
			test::require(mid > 0.0 && mid < CombatArena::kBodySpacing, "the two bodies did not start to spread");

			const std::uint64_t atSave = rig.sim->stateHash();
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_body_layer.ksav";
			rig.sim->saveGame(path);
			{
				Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path);
				CombatSettings settings;
				settings.substeps = 2;
				loaded.setGameData(gd);
				loaded.enableCombat(data, settings);
				loaded.enableNavigation(nav);
				test::require(loaded.stateHash() == atSave, "a save mid-spread loads to another state");
				for (int i = 0; i < 200; ++i)
				{
					loaded.tick();
					rig.sim->tick();
				}
				test::require(loaded.stateHash() == rig.sim->stateHash(), "a load mid-spread continues differently");
			}
			std::remove(path.c_str());

			test::require(rig.f(a).building == key && rig.f(b).building == key,
				"a body left the building while spreading");
			const double apart = std::hypot(rig.f(a).x - rig.f(b).x, rig.f(a).z - rig.f(b).z);
			test::require(apart >= CombatArena::kBodySpacing - 0.2, "the two spread to " + num(apart) + " inside the house");
			std::printf("ok  building: two bodies spread to %.2f inside the Storm House, building kept, save/load at %.2f apart\n", apart, mid);
		}

		// ---- 5. a body put down goes beside the pile, not on it ----
		{
			WorldRig rig(gd, data, nav);
			const auto spot = nav->walker().snap({kHubX, 1535.0, kHubZ}, 200.0);
			test::require(spot.has_value(), "no mesh near the Hub");
			const std::uint64_t lying = rig.spawnBody(spot->x, spot->y, spot->z);
			// A carrier stands right beside the body with a person over its shoulder; a put-down there would stack the pile.
			const std::uint64_t carrier = spawnUpright(rig, spot->x + 1.0, spot->y, spot->z);
			state::Fighter carried = rig.data->makeFighter(*rig.gd, rig.servant->stringID, "", rig.rng);
			carried.playerControlled = true;
			carried.unconscious = true;
			carried.koTimer = 1.0e6;
			carried.x = rig.f(carrier).x;
			carried.y = rig.f(carrier).y;
			carried.z = rig.f(carrier).z;
			carried.carriedBy = carrier;
			const std::uint64_t dropped = rig.sim->spawnFighter(std::move(carried)).get<state::Id>()->value;
			rig.f(carrier).carrying = dropped;
			// dropCarried is the one put-down path (removeFighter puts a carried person down the same way).
			rig.sim->removeFighter(carrier);
			test::require(rig.f(dropped).carriedBy == 0, "the dropped body is still carried");
			const double toBody = std::hypot(rig.f(dropped).x - rig.f(lying).x, rig.f(dropped).z - rig.f(lying).z);
			test::require(toBody >= CombatArena::kBodySpacing - 0.5,
				"the body was put down on the pile: " + num(toBody) + " from it");
			// Within the search (a 6 u radius, the snap reach a further 3).
			test::require(std::hypot(rig.f(dropped).x - spot->x, rig.f(dropped).z - spot->z) <= 9.0,
				"the put-down spot is not near the carrier");
			std::printf("ok  put-down: landed %.2f from the body lying there (spacing %.1f)\n", toBody, CombatArena::kBodySpacing);
		}

		std::cout << "ksim_body_layer: ok\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_body_layer: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
