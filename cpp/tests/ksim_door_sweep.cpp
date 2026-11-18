// Door sweep on the runtime mesh (kenshi-k16.6.251, docs/formats/NAVMESH_CACHE.md "Door sweep"): every placed building with a door that has a
// way in, in the towns' zones, is routed into from outside, on the mesh the player walks on (MeshNavigator::openNative over KENSHI_DATA with the
// nav cache on: shipped tiles, stale zones rebaked with our recipe). The Heft Longhouse hole (k16.6.228: the interior was an island not joined to its
// entrance stairs) was found live; this is the test that would have found it.
// Per building (the sim's door entities, spawnMeshDoors; every door open, as the gates are at the Hub; the player's clearance radius):
//  - "floor0": from a mesh point 30-60 u outside a door to a floor-0 point inside it (30-60 u in through the same door);
//  - "deep0": to the floor-0 sample farthest from the door that stands on the mesh inside the building (the Longhouse's interior was far side);
//  - "floorN": to a point of each upper floor (BuildingSite::floors: one per storey, reached by the interior stairs);
//  - a leg that is found but leaves the building after it first entered it is "left" (the route goes round the outside to get in).
// The towns: The Hub, Heft, Squin and Stack, 3 x 3 zones each (the navigator holds 25 zones at most, so one navigator per town; the cache makes the
// second run cheap). A failing leg (building key, leg) must be in the known-gap file (tests/data/door_sweep_known_gaps.tsv) or the test fails;
// a known gap that now passes is reported, not failed.
// argv: [1] the output directory (the nav cache tree and door_sweep.tsv live there), default ".";
//       [2] the known-gap file, default the checked-in one;
//       `--expect X Z R` after: instead of the gap file, fail unless a failing building lies within R of (X, Z) (the self-check: the Longhouse
//       on a build without k16.6.229's recipe 5).
// Cold and cached wall times and the peak RSS are printed. Exit 77 without the navtiles.

#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_DOOR_SWEEP_GAPS
#error KSIM_DOOR_SWEEP_GAPS must be defined
#endif

namespace {

using namespace ksim;

constexpr double kNear = 30.0; // u from the door: the nearest and farthest the outside / inside probe points lie
constexpr double kFar = 60.0;
constexpr std::size_t kMaxOutsides = 64; // outside candidates kept per building
constexpr double kFarOut = 150.0; // the outside probe reaches further when a hull or neighbour covers the doorstep
constexpr double kSnap = 6.0; // u: how far a probe point may be from the mesh and still count as standing on it
constexpr double kGridStep = 48.0; // u: the interior sample grid
constexpr double kLeaveSlack = 8.0; // u: a route point this far out of the footprint box is outside (the hull's own edge is fuzzy)

std::size_t statusKb(const char* key)
{
	std::ifstream status("/proc/self/status");
	std::string line;
	const std::string prefix = std::string(key) + ":";
	while (std::getline(status, line))
		if (line.rfind(prefix, 0) == 0)
			return static_cast<std::size_t>(std::stoul(line.substr(prefix.size())));
	return 0;
}

struct Failure
{
	std::uint64_t building = 0;
	std::string leg;
	std::string name;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	std::string why;
};

struct Town
{
	std::string name;
	double x = 0.0;
	double z = 0.0;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_door_sweep: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::string outDir = ".";
		std::string gapsPath = KSIM_DOOR_SWEEP_GAPS;
		bool expect = false;
		double expectX = 0.0, expectZ = 0.0, expectR = 0.0;
		int positional = 0;
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			if (arg == "--expect" && i + 3 < argc)
			{
				expect = true;
				expectX = std::stod(argv[++i]);
				expectZ = std::stod(argv[++i]);
				expectR = std::stod(argv[++i]);
			}
			else if (positional++ == 0)
				outDir = arg;
			else
				gapsPath = arg;
		}
		std::filesystem::create_directories(outDir);
		const auto start = std::chrono::steady_clock::now();

		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gameData = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gameData, KENSHI_DATA);
		Simulation sim = Simulation::load(KENSHI_DATA, 77);
		NativeCacheOptions cache;
		cache.dir = outDir + "/navcache";
		cache.worldKey = kworld::dataHash(order);

		// The player's body: the human race's clearance, as the walks of ksim_stair_walks take it.
		std::string sid;
		for (const kdata::Record* r : gameData->byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		std::uint64_t rng = 12345;
		const double radius = data->pathRadius(data->makeFighter(*gameData, sid, "", rng).raceSid);

		std::vector<Town> towns;
		const kworld::Towns townSet = kworld::Towns::join(*gameData, KENSHI_DATA);
		for (const char* wanted : { "The Hub", "Heft", "Squin", "Stack" })
		{
			for (const kworld::TownRecord& record : townSet.towns())
			{
				if (record.name != wanted || record.placementIndices.empty())
					continue;
				const kworld::TownPlacement& placement = townSet.placements()[record.placementIndices.front()];
				towns.push_back({ wanted, placement.x, placement.z });
				break;
			}
		}
		test::require(towns.size() == 4, "the data has The Hub, Heft, Squin and Stack");

		std::vector<Failure> failures;
		std::set<std::uint64_t> swept;
		std::size_t legs = 0;
		std::size_t doors = 0;
		std::size_t pockets = 0;
		for (const Town& town : towns)
		{
			const kworld::ZoneCell centre = kworld::zoneCellOf(town.x, town.z);
			std::vector<kworld::ZoneCell> zones;
			for (int dz = -1; dz <= 1; ++dz)
				for (int dx = -1; dx <= 1; ++dx)
					zones.push_back({ centre.x + dx, centre.z + dz });
			const std::shared_ptr<MeshNavigator> nav = MeshNavigator::openNative(KENSHI_DATA, zones, [](const std::string&) {}, gameData.get(), cache);
			sim.enableNavigation(nav);
			nav->applyDoors(knav::DoorBook{}); // every door open
			std::set<int> window;
			for (const kworld::ZoneCell& zone : zones)
				window.insert(kworld::zoneIndexOf(zone));

			// A mesh point at least kNear and at most kFar from the door along `dir`, standing on the mesh (probed every 5 u).
			const auto probe = [&](const state::Door& door, double dirX, double dirZ, bool wantInside, std::uint64_t building,
				double wantFloor) -> std::optional<knav::Position>
			{
				for (double along = kNear; along <= (wantInside ? kFar : kFarOut); along += 5.0)
				{
					const double px = door.x + dirX * along;
					const double pz = door.z + dirZ * along;
					const auto spot = nav->walker().snap({ px, door.y, pz }, 12.0);
					if (!spot || std::hypot(spot->x - px, spot->z - pz) > (wantInside ? kSnap : 12.0) || std::fabs(spot->y - door.y) > (wantInside ? 30.0 : 20.0))
						continue;
					const std::uint64_t at = sim.buildingAt(spot->x, spot->y, spot->z);
					if (wantInside ? at != building || sim.buildingFloor(building, spot->y) != static_cast<int>(wantFloor) : at != 0)
						continue;
					return knav::Position{ spot->x, spot->y, spot->z };
				}
				return std::nullopt;
			};

			std::vector<state::Door> doorList;
			sim.world().ecs().each([&](const state::Door& door)
			{
				const kworld::ZoneCell cell = kworld::zoneCellOf(door.x, door.z);
				if (door.building != 0 && !door.gate && (door.inX != 0.0 || door.inZ != 0.0) && window.count(kworld::zoneIndexOf(cell)) != 0)
					doorList.push_back(door);
			});
			doors += doorList.size();
			const auto streetRef = nav->walker().snap({ town.x, 0.0, town.z }, 300.0);
			// One door per building: the first with an outside point and a floor-0 inside point.
			std::set<std::uint64_t> done;
			for (const state::Door& door : doorList)
			{
				if (done.count(door.building) != 0 || swept.count(door.building) != 0)
					continue;
				const BuildingSite* site = sim.world().buildingSite(door.building);
				if (site == nullptr)
					continue;
				// The way in the sim recorded (state::Door::inX) is a guess for a building whose pivot is not at its front: try both sides.
				auto inside = probe(door, door.inX, door.inZ, true, door.building, 0);
				if (!inside)
					inside = probe(door, -door.inX, -door.inZ, true, door.building, 0);
				// Outside candidates: mesh points 30-60 u from the door, in the open (no building), on both sides of its axis and all round.
				// A leg passes when any candidate routes (a candidate in a walled yard is no street).
				std::vector<knav::Position> outsides;
				// A doorstep up a stair or a rise (the Stack armory: the door 40 u above its street) has its approach below the door: each ray
				// is probed at the door's height and two lower, or the only candidates are ledges beside the door that no stair joins.
				for (int k = 0; k < 34; ++k)
				{
					const double angle = k == 0 ? std::atan2(-door.inZ, -door.inX) : k == 1 ? std::atan2(door.inZ, door.inX) : (k - 2) * 3.14159265358979 / 16.0;
					for (double along = kNear; along <= kFarOut; along += 10.0)
					{
						const double px = door.x + std::cos(angle) * along;
						const double pz = door.z + std::sin(angle) * along;
						for (const double drop : { 0.0, 24.0, 48.0 })
						{
							const auto spot = nav->walker().snap({ px, door.y - drop, pz }, 12.0);
							if (spot && std::hypot(spot->x - px, spot->z - pz) <= 8.0 && std::fabs(spot->y - door.y) <= 60.0
								&& sim.buildingAt(spot->x, spot->y, spot->z) == 0
								&& std::none_of(outsides.begin(), outsides.end(), [&](const knav::Position& o) { return std::hypot(o.x - spot->x, o.z - spot->z) < 6.0 && std::fabs(o.y - spot->y) < 6.0; }))
								outsides.push_back({ spot->x, spot->y, spot->z });
						}
					}
				}
				// Each candidate costs a street route and, on a failing leg, a route per leg: thin a long list evenly.
				if (outsides.size() > kMaxOutsides)
				{
					std::vector<knav::Position> thinned;
					for (std::size_t k = 0; k < kMaxOutsides; ++k)
						thinned.push_back(outsides[k * outsides.size() / kMaxOutsides]);
					outsides.swap(thinned);
				}
				// Only candidates on the street network count: one that cannot route to the town's centre is a walled pocket (counted apart).
				if (streetRef)
				{
					const std::size_t before = outsides.size();
					outsides.erase(std::remove_if(outsides.begin(), outsides.end(), [&](const knav::Position& from)
					{
						const Route r = nav->routeFrom(from, streetRef->x, streetRef->z, streetRef->y, false, radius, true);
						return !r.found || std::hypot(r.points.back().x - streetRef->x, r.points.back().z - streetRef->z) > 30.0;
					}), outsides.end());
					pockets += before - outsides.size();
				}
				const bool outside = !outsides.empty();
				if (!outside || !inside)
				{
					if (std::getenv("SWEEP_VERBOSE") != nullptr)
					{
						std::cout << "  skip " << door.building << " " << site->buildingSid << " " << site->label << " door " << door.key << (outside ? "" : " no-outside") << (inside ? "" : " no-inside") << " at " << door.x << " " << door.y << " " << door.z << " in " << door.inX << " " << door.inZ;
						const auto at = nav->walker().snap({ door.x, door.y, door.z }, 40.0);
						if (at)
							std::cout << " snap " << at->x << " " << at->y << " " << at->z;
						std::cout << "\n";
					}
					continue;
				} // a door with no ground on one side: a passage in a solid body, nothing to route
				done.insert(door.building);
				if (std::getenv("SWEEP_VERBOSE") != nullptr)
					std::cout << "  swept " << town.name << " " << door.building << " " << site->buildingSid << " " << site->label << " floors " << site->floors.size() << "\n";
				swept.insert(door.building);
				const std::string name = site->buildingSid + (site->label.empty() ? "" : " " + site->label);
				const auto fail = [&](const std::string& leg, const std::string& why, double y)
				{
					failures.push_back({ door.building, leg, name, site->originX, y, site->originZ, why });
				};
				// The legs: route from the outside point to `to`; fails when not found / incomplete, or when it leaves the building.
				const auto leg = [&](const std::string& tag, const knav::Position& to)
				{
					++legs;
					std::string firstWhy;
					for (const knav::Position& from : outsides)
					{
						const Route route = nav->routeFrom(from, to.x, to.z, to.y, false, radius, true);
						std::string why;
						if (!route.found)
						{
							why = std::string("no route (") + std::string(route.check) + ")";
						}
						else if (const double gap = std::hypot(route.points.back().x - to.x, route.points.back().z - to.z); gap > 15.0)
						{
							why = "incomplete (ends " + std::to_string(static_cast<int>(gap)) + " u short)";
						}
						else
						{
							bool entered = false;
							for (std::size_t k = 0; k < route.points.size() && why.empty(); ++k)
							{
								const double y = k < route.heights.size() ? route.heights[k] : to.y;
								const bool in = sim.buildingAt(route.points[k].x, y, route.points[k].z) == door.building;
								if (in)
									entered = true;
								else if (entered)
								{
									// outside again: only a fault when well clear of the footprint box (the doorway's own edge is fuzzy)
									const double dx = route.points[k].x - site->cx;
									const double dz = route.points[k].z - site->cz;
									const double a = std::fabs(dx * site->axisX + dz * site->axisZ);
									const double b = std::fabs(-dx * site->axisZ + dz * site->axisX);
									if (a > site->halfA + kLeaveSlack || b > site->halfB + kLeaveSlack)
										why = "left: the route leaves the building after entering it";
								}
							}
						}
						if (why.empty())
							return;
						if (firstWhy.empty())
							firstWhy = why;
					}
					fail(tag, firstWhy + " from all " + std::to_string(outsides.size()) + " outside points", to.y);
					if (std::getenv("SWEEP_VERBOSE") != nullptr)
					{
						// Why: the target, the building's other doors (an interior door between?), and the same walk from the entrance's inside point and at smaller radii.
						std::cout << "  diag " << door.building << " " << tag << " target " << to.x << " " << to.y << " " << to.z << " entrance door " << door.x << " " << door.y << " " << door.z << " inside " << inside->x << " " << inside->y << " " << inside->z << "\n";
						sim.world().ecs().each([&](const state::Door& other)
						{
							if (other.building == door.building && other.key != door.key)
								std::cout << "    other door " << other.key << " " << other.recordSid << " at " << other.x << " " << other.y << " " << other.z << ", " << std::hypot(other.x - to.x, other.z - to.z) << " u from the target\n";
						});
						for (const double r : { radius, 3.0, 2.0, 1.0, 0.5 })
						{
							const Route inner = nav->routeFrom(*inside, to.x, to.z, to.y, false, r, true);
							const Route outer = nav->routeFrom(outsides.front(), to.x, to.z, to.y, false, r, true);
							std::cout << "    radius " << r << ": from inside " << (inner.found ? "found" : std::string("no route (") + std::string(inner.check) + ")") << ", from outside[0] " << (outer.found ? "found" : std::string("no route (") + std::string(outer.check) + ")") << "\n";
						}
					}
				};
				leg("floor0", *inside);

				// The floor-0 sample farthest from the door, and one sample per upper floor (the nearest to the door).
				const double reach = std::hypot(site->halfA, site->halfB);
				std::map<int, knav::Position> best;
				double farthest = -1.0;
				knav::Position deep{};
				std::map<int, double> nearest;
				for (double gx = -reach; gx <= reach; gx += kGridStep)
				{
					for (double gz = -reach; gz <= reach; gz += kGridStep)
					{
						const double px = site->cx + gx;
						const double pz = site->cz + gz;
						const auto spot = nav->walker().snap({ px, site->originY, pz }, 12.0);
						if (!spot || std::hypot(spot->x - px, spot->z - pz) > kSnap)
							continue;
						if (sim.buildingAt(spot->x, spot->y, spot->z) != door.building)
							continue;
						const int floor = sim.buildingFloor(door.building, spot->y);
						const double far = std::hypot(spot->x - door.x, spot->z - door.z);
						if (floor == 0 && far > farthest && std::fabs(spot->y - inside->y) < 25.0)
						{
							farthest = far;
							deep = { spot->x, spot->y, spot->z };
						}
						if (floor > 0 && (nearest.count(floor) == 0 || far < nearest[floor]))
						{
							nearest[floor] = far;
							best[floor] = { spot->x, spot->y, spot->z };
						}
					}
				}
				if (farthest > kFar)
					leg("deep0", deep);
				for (const auto& [floor, spot] : best)
					leg("floor" + std::to_string(floor), spot);
			}
		}
		const double wallS = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

		std::ofstream report(outDir + "/door_sweep.tsv");
		report << "building\tleg\tname\tx\ty\tz\twhy\n";
		for (const Failure& f : failures)
			report << f.building << "\t" << f.leg << "\t" << f.name << "\t" << f.x << "\t" << f.y << "\t" << f.z << "\t" << f.why << "\n";
		std::cout << "ksim_door_sweep: " << swept.size() << " buildings (" << doors << " doors) in " << towns.size() << " towns, " << legs << " legs, " << pockets << " outside points dropped as walled pockets, "
				  << failures.size() << " failing; " << std::llround(wallS) << " s, peak RSS " << statusKb("VmHWM") / 1024 << " MB\n";
		for (const Failure& f : failures)
			std::cout << "  FAIL " << f.building << " " << f.leg << " " << f.name << " at (" << f.x << ", " << f.z << "): " << f.why << "\n";

		if (expect)
		{
			for (const Failure& f : failures)
				if (std::hypot(f.x - expectX, f.z - expectZ) <= expectR)
				{
					std::cout << "ksim_door_sweep: expected failure found (building " << f.building << " " << f.name << ")\n";
					return 0;
				}
			std::cerr << "ksim_door_sweep: no failing building within " << expectR << " of (" << expectX << ", " << expectZ << ")\n";
			return 1;
		}

		std::set<std::pair<std::uint64_t, std::string>> known;
		std::ifstream gaps(gapsPath);
		std::string line;
		while (std::getline(gaps, line))
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::istringstream row(line);
			std::string key, leg;
			std::getline(row, key, '\t');
			std::getline(row, leg, '\t');
			known.insert({ std::stoull(key), leg });
		}
		std::set<std::pair<std::uint64_t, std::string>> seen;
		int fresh = 0;
		for (const Failure& f : failures)
		{
			seen.insert({ f.building, f.leg });
			if (known.count({ f.building, f.leg }) == 0)
			{
				std::cerr << "ksim_door_sweep: NEW gap: " << f.building << " " << f.leg << " " << f.name << " at (" << f.x << ", " << f.z << "): " << f.why << "\n";
				++fresh;
			}
		}
		for (const auto& gap : known)
			if (seen.count(gap) == 0)
				std::cout << "ksim_door_sweep: known gap now passes: " << gap.first << " " << gap.second << " (remove it from the gap file)\n";
		test::require(fresh == 0, std::to_string(fresh) + " new failing door legs (see above; fix them or add them to the gap file with a note)");
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_door_sweep: " << e.what() << "\n";
		return 1;
	}
}
