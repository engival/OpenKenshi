// Building membership (Fighter::building, SIMLINK.md section 6, INTERIORS.md "Building membership"): a character is in a
// building when it stands in the building's interior mask hull, whichever way it got there.
//
//  1. the Hub Storm House (3384 at -51192, 3192; the report: `building` stayed 0 when a character walked in): a walk in from the
//     east ramp sets the building's key, a walk out clears it, a move order does not need the door's inside normal (it points the
//     wrong way here), a teleport in and out follows at the next tick, a save made inside loads inside;
//  2. every Hub building with an interior mask: a character placed in its hull is in it (or in a smaller building inside it),
//     one placed well outside is not;
//  3. a second building type, a Small Shack, walked into and out of through its own door.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr double kHouseX = -51192.0;
constexpr double kHouseZ = 3192.0;
constexpr double kHouseFloorY = 1576.5; // origin + 4.5 (STORM_HOUSE_INTERIOR_LIVE.md)
constexpr double kBarX = -51181.0; // the Hub Bar (3384-TwoStorey, placement 4933); floor 1 is y 1647 (ksim_native_nav)
constexpr double kBarZ = 2639.0;
constexpr double kRampX = -51085.0;     // where the live walks entered, on the ground
constexpr double kRampZ = 3145.0;

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	ks::Duel duel;
	std::uint32_t seq = 0;

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, const kworld::LoadOrder& order)
		: gd(g)
		, data(d)
		, nav(n)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		// Nobody else in the arena, so the test reads one character.
		sim->removeFighter(duel.enemy);
	}

	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	state::Fighter& f(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	state::Fighter& player() { return f(duel.player); }
	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->tick();
	}
	void put(double x, double y, double z)
	{
		state::Fighter& p = player();
		p.x = x;
		p.y = y;
		p.z = z;
	}
	// Every door open, so a move order is not stopped at a shut one (the building is the subject here, not the lock).
	void openDoors()
	{
		ecs().each([&](state::Door& door) { door.state = 0; });
	}
	// A move order as a session makes it; true when the walk ends with the character standing.
	void walkTo(double x, double z, double y)
	{
		const state::Fighter& p = player();
		const Route route = nav->routeFrom({p.x, p.y, p.z}, x, z, y);
		test::require(route.found, "no route to " + std::to_string(x) + ", " + std::to_string(z));
		sim->enqueue(Command::fighterMoveTo(state::Id{duel.player}, route.points, route.heights, ++seq));
		tick(2);
		for (int n = 0; n < 4000 && player().walking(); ++n)
			tick();
		test::require(!player().walking(), "the walk never ended");
	}
	void groundY(double x, double z, double near, double& y)
	{
		const auto spot = nav->walker().snap({x, near, z}, 60.0);
		test::require(spot.has_value(), "no mesh near " + std::to_string(x) + ", " + std::to_string(z));
		y = spot->y;
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_building_membership HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// ---- 1. the Storm House ----
		{
			Rig rig(gd, data, nav, order);
			const BuildingSite* house = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
			{
				if (std::hypot(site.cx - kHouseX, site.cz - kHouseZ) < 30.0)
					house = &site;
			}
			test::require(house != nullptr, "no building site at the Storm House");
			test::require(house->hull, "the Storm House has no interior mask hull");
			const std::uint64_t key = house->key;
			std::cout << "storm house: key " << key << ", hull half " << house->hullHalfA << " x " << house->hullHalfB << ", y "
				<< house->hullMinY << ".." << house->hullMaxY << ", footprint half " << house->halfA << " x " << house->halfB << "\n";

			// The cause of the report: the door's inside normal. Whatever it is, membership does not read it.
			rig.ecs().each([&](const state::Door& door)
			{
				if (door.building == key)
				{
					const double toward = (house->cx - door.x) * door.inX + (house->cz - door.z) * door.inZ;
					std::cout << "storm house door " << door.recordSid << " at " << door.x << ", " << door.z << ", inX/inZ " << door.inX << ", " << door.inZ
						<< ", toward the centre: " << toward << "\n";
				}
			});

			double rampY = 0.0;
			rig.groundY(kRampX, kRampZ, 1530.0, rampY);
			rig.put(kRampX, rampY, kRampZ);
			rig.openDoors();
			rig.tick(2);
			test::require(rig.player().building == 0, "outside the house the character has building " + std::to_string(rig.player().building));

			// Walk in by the east steps, as the live game does.
			rig.walkTo(kHouseX, kHouseZ, kHouseFloorY);
			const state::Fighter& in = rig.player();
			std::cout << "walked in: ends at " << in.x << ", " << in.y << ", " << in.z << ", building " << in.building << "\n";
			test::require(std::hypot(in.x - kHouseX, in.z - kHouseZ) < 15.0 && std::fabs(in.y - kHouseFloorY) < 6.0,
				"the walk did not end inside the house");
			test::require(in.building == key, "walked into the Storm House: building " + std::to_string(in.building) + ", not " + std::to_string(key));

			// A save made inside loads inside.
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_building_membership.save";
			rig.sim->saveGame(path);
			{
				Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
				CombatSettings settings;
				settings.substeps = 2;
				loaded.enableCombat(data, settings);
				loaded.enableNavigation(nav);
				test::require(loaded.combat()->find(rig.duel.player)->building == key, "a save made inside the house loaded outside it");
				loaded.run(5);
				test::require(loaded.combat()->find(rig.duel.player)->building == key, "the loaded character left the house by standing still");
			}
			std::remove(path.c_str());

			// And out again.
			rig.walkTo(kRampX, kRampZ, rampY);
			std::cout << "walked out: ends at " << rig.player().x << ", " << rig.player().y << ", " << rig.player().z << ", building " << rig.player().building << "\n";
			test::require(rig.player().building == 0, "walked out of the Storm House: building " + std::to_string(rig.player().building));

			// A teleport (a spawn, a debug move) is read at the next tick, in and out.
			rig.put(kHouseX, kHouseFloorY, kHouseZ);
			rig.tick();
			test::require(rig.player().building == key, "placed inside the house: building " + std::to_string(rig.player().building));
			rig.put(kRampX, rampY, kRampZ);
			rig.tick();
			test::require(rig.player().building == 0, "placed outside the house: building " + std::to_string(rig.player().building));

			// The ground under the house (below the hull's floor and its slack) is not inside it.
			test::require(rig.sim->buildingAt(kHouseX, house->hullMinY - 20.0, kHouseZ) == 0, "ground under the house counts as inside");
			test::require(rig.sim->buildingAt(kHouseX, house->hullMaxY + 40.0, kHouseZ) == 0, "the air above the house counts as inside");

			// The floors (CHAR_DYNAMIC `floor`, SIMLINK.md section 6): ground 0. This house is a ruin with no upper floor (live trace,
			// docs/reference/STORM_HOUSE_INTERIOR_LIVE.md: nothing above y 1577); floor 1 is checked on the Bar's roof (1b).
			// Re-pinned 2026-11-05 when a ruin dropped its interior stairs (their slope was the "upper floor" here).
			std::cout << "storm house floors:";
			for (const double surface : house->floors)
				std::cout << " " << surface;
			std::cout << " (hull y " << house->hullMinY << ".." << house->hullMaxY << ")\n";
			test::require(house->floors.empty(), "the ruined Storm House reports an upper floor");
			rig.put(kHouseX, kHouseFloorY, kHouseZ);
			rig.tick();
			test::require(rig.player().floor == 0, "the Storm House ground floor is not floor 0: " + std::to_string(rig.player().floor));
			rig.put(kRampX, rampY, kRampZ);
			rig.tick();
		}

		// ---- 1b. the Hub Bar: inside is floor 0, the roof floor 1 (SIMLINK.md section 6) ----
		{
			Rig rig(gd, data, nav, order);
			const BuildingSite* bar = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
				if (std::hypot(site.cx - kBarX, site.cz - kBarZ) < 30.0)
					bar = &site;
			test::require(bar != nullptr, "no building site at the Hub Bar");
			test::require(bar->hull, "the Bar has no interior mask hull");
			std::cout << "bar: key " << bar->key << ", hull y " << bar->hullMinY << ".." << bar->hullMaxY << ", floors:";
			for (const double surface : bar->floors)
				std::cout << " " << surface;
			std::cout << "\n";
			test::require(bar->floors.size() == 1, "the Bar does not report one floor above the ground");
			rig.tick(); // the first look is taken where the duel spawned; only a later move is tested for a building
			rig.put(bar->hullCx, bar->hullMinY + 5.0, bar->hullCz);
			rig.tick();
			test::require(rig.player().building == bar->key, "inside the Bar: building " + std::to_string(rig.player().building));
			test::require(rig.player().floor == 0, "inside the Bar is not floor 0: " + std::to_string(rig.player().floor));
			rig.put(bar->hullCx, bar->floors.front(), bar->hullCz);
			rig.tick();
			test::require(rig.player().building == bar->key, "on the Bar roof: building " + std::to_string(rig.player().building));
			test::require(rig.player().floor == 1, "the Bar roof is not floor 1: " + std::to_string(rig.player().floor));
			// Halfway up the stairs the climber is on floor 1 already; just below halfway it is still on 0 (SIMLINK
			// section 6 `floor`; client item 6).
			const double halfway = 0.5 * (bar->originY + bar->floors.front());
			std::cout << "bar: ground " << bar->originY << ", halfway " << halfway << "\n";
			test::require(bar->floorAt(halfway + 0.5) == 1, "halfway up to the Bar roof is not floor 1");
			test::require(bar->floorAt(halfway - 0.5) == 0, "below halfway to the Bar roof is not floor 0");
		}

		// ---- 2. every Hub building with an interior mask ----
		{
			Rig rig(gd, data, nav, order);
			std::size_t hulls = 0;
			std::size_t insideHits = 0;
			std::size_t outsideHits = 0;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
			{
				if (!site.hull)
					continue;
				++hulls;
				const std::uint64_t centre = rig.sim->buildingAt(site.hullCx, 0.5 * (site.hullMinY + site.hullMaxY), site.hullCz);
				// The smallest building holding the point: this one, or one inside it (a hull in a hull).
				if (centre == site.key)
					++insideHits;
				else
					std::cout << "hull " << site.key << ": its centre is in " << centre << "\n";
				const double far = site.hullHalfA + site.hullHalfB + 60.0;
				if (rig.sim->buildingAt(site.hullCx + site.axisX * far, 0.5 * (site.hullMinY + site.hullMaxY), site.hullCz + site.axisZ * far) == site.key)
					++outsideHits;
			}
			std::cout << "hulls: " << hulls << " with a mask, " << insideHits << " hold their centre, " << outsideHits << " claim a point far outside\n";
			test::require(hulls >= 5, "too few Hub buildings with an interior mask: " + std::to_string(hulls));
			test::require(insideHits * 10 >= hulls * 9, "fewer than 90% of the hulls hold their own centre");
			test::require(outsideHits == 0, "a building claims a point 60+ units outside its hull");

			// A second building type: a Small Shack (Storm Shack Door, 4093) walked into through its own door.
			const state::Door* shackDoor = nullptr;
			rig.ecs().each([&](const state::Door& door)
			{
				if (shackDoor == nullptr && door.recordSid == "4093-TwoStorey.mod" && std::hypot(door.inX, door.inZ) > 0.5)
					shackDoor = &door;
			});
			test::require(shackDoor != nullptr, "no Small Shack door in the Hub");
			const BuildingSite* shack = nullptr;
			for (const BuildingSite& site : rig.sim->world().buildingSites())
			{
				if (site.key == shackDoor->building)
					shack = &site;
			}
			test::require(shack != nullptr && shack->hull, "the Small Shack has no hull");
			// Out: 14 units off the door on the side away from the building; in: its hull centre's floor.
			const double outsideSide = (shack->cx - shackDoor->x) * shackDoor->inX + (shack->cz - shackDoor->z) * shackDoor->inZ > 0.0 ? -1.0 : 1.0;
			double outY = 0.0;
			rig.groundY(shackDoor->x + outsideSide * shackDoor->inX * 14.0, shackDoor->z + outsideSide * shackDoor->inZ * 14.0, shackDoor->y, outY);
			rig.put(shackDoor->x + outsideSide * shackDoor->inX * 14.0, outY, shackDoor->z + outsideSide * shackDoor->inZ * 14.0);
			rig.openDoors();
			rig.tick(2);
			test::require(rig.player().building == 0, "outside a Small Shack the character has a building");
			double inY = 0.0;
			rig.groundY(shack->hullCx, shack->hullCz, shackDoor->y, inY);
			rig.walkTo(shack->hullCx, shack->hullCz, inY);
			std::cout << "small shack: walked to " << rig.player().x << ", " << rig.player().y << ", " << rig.player().z << ", building " << rig.player().building << "\n";
			test::require(rig.player().building == shack->key, "walked into the Small Shack: building " + std::to_string(rig.player().building));
			rig.walkTo(shackDoor->x + outsideSide * shackDoor->inX * 14.0, shackDoor->z + outsideSide * shackDoor->inZ * 14.0, outY);
			test::require(rig.player().building == 0, "walked out of the Small Shack: building " + std::to_string(rig.player().building));
		}

		// ---- 4. what the streamed navmesh reaches: from outside each Hub door to the middle of its building's hull ----
		{
			Rig rig(gd, data, nav, order);
			std::map<std::string, std::pair<int, int>> byDoor; // door record -> {routes found, tried}
			rig.ecs().each([&](const state::Door& door)
			{
				if (door.gate || door.building == 0 || std::hypot(door.x - kHouseX, door.z - kHouseZ) > 4000.0)
					return;
				const BuildingSite* site = nullptr;
				for (const BuildingSite& s : rig.sim->world().buildingSites())
				{
					if (s.key == door.building)
						site = &s;
				}
				if (site == nullptr || !site->hull)
					return;
				// 20 units off the door on the side of its slab that is farther from the hull's centre.
				const double nx = -door.axisZ;
				const double nz = door.axisX;
				const double away = (site->hullCx - door.x) * nx + (site->hullCz - door.z) * nz > 0.0 ? -1.0 : 1.0;
				const auto from = nav->walker().snap({door.x + away * nx * 20.0, door.y, door.z + away * nz * 20.0}, 40.0);
				const auto to = nav->walker().snap({site->hullCx, site->hullMinY + 8.0, site->hullCz}, 30.0); // the floor, not the roof island above it
				auto& tally = byDoor[door.recordSid];
				++tally.second;
				const char* why = !from ? "no ground outside the door" : !to ? "no ground near the hull centre"
					: std::hypot(to->x - site->hullCx, to->z - site->hullCz) > 25.0 ? "the hull centre is solid (nearest ground 25+ away)" : nullptr;
				if (why == nullptr)
				{
					const Route route = nav->routeFrom(*from, to->x, to->z, to->y, true);
					if (route.found && !route.approach)
					{
						++tally.first;
						return;
					}
					why = !route.found ? "no route" : "route ends at an approach point";
					// From the open ground round the hull instead of the door: is the floor reachable at all?
					const double reach = site->hullHalfA + site->hullHalfB + 40.0;
					for (int k = 0; k < 8 && std::string(why) != "reachable from open ground (not through this door)"; ++k)
					{
						const double bearing = k * 0.785398;
						const auto ring = nav->walker().snap({site->hullCx + std::sin(bearing) * reach, door.y, site->hullCz + std::cos(bearing) * reach}, 60.0);
						if (!ring || rig.sim->buildingAt(ring->x, ring->y, ring->z) != 0)
							continue;
						const Route other = nav->routeFrom(*ring, to->x, to->z, to->y, true);
						if (other.found && !other.approach)
							why = "reachable from open ground (not through this door)";
					}
				}
				std::cout << "interior routes: door " << door.recordSid << " at " << door.x << ", " << door.z << ": " << why << "\n";
			});
			int found = 0;
			int tried = 0;
			for (const auto& [record, tally] : byDoor)
			{
				const kdata::Record* r = gd->find(record);
				std::cout << "interior routes: door " << (r != nullptr ? r->name : record) << ": " << tally.first << " of " << tally.second << " reach the hull centre\n";
				found += tally.first;
				tried += tally.second;
			}
			// Measured 2026-11-02: 19 of 24 (the others: two Storm House shells whose floor is not on the mesh, one whose door side
			// has no connection). The Small Shacks and the Storm House 3384 all route in.
			test::require(byDoor["4093-TwoStorey.mod"].first == byDoor["4093-TwoStorey.mod"].second && byDoor["4093-TwoStorey.mod"].second > 0,
				"a Small Shack's floor is not reachable through its door");
			test::require(found * 10 >= tried * 7, "fewer than 70% of the Hub interiors are reachable: " + std::to_string(found) + " of " + std::to_string(tried));
		}

		std::cout << "building membership: ok\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
