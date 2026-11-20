// Standing height sweep (kenshi-k16.8.30, re-keyed by k16.8.31): the sim's y is the navmesh point (no terrain snap, no ground ray: the client
// places the body). Places a standing character at sample points on door thresholds, building floors (ground and upper), stairs / ramps, decks
// and slopes in the Hub and Heft and checks two things:
//  - wire y: the y the walker writes and the wire carries (a zero-length Walker::step from the face) is the navmesh face's y, 0 tolerance;
//  - level ("respecting the floor"): the collision surface the client will ray onto (the walkable collision and the terrain, from the face + 20
//    straight down: every hit) has its nearest hit to the face on the face's own storey, within kTolLevel (the sweep's measured spread, 2.0 u).
//    A face nearer another storey's surface than its own, or with no surface within that spread, is a bake gap.
// The spread per surface type is printed. A point that fails the level check must be in tests/data/height_sweep_known_gaps.tsv (surface TAB
// "x z" rounded to 16 u TAB note) or the test fails; a listed gap that now passes is reported, not failed. Every point goes to
// height_sweep.tsv in the working directory.
// argv[1]: the world navmesh bake directory. [2]: the known-gap file (default the checked-in one). Exit 77 without the navigation data.

#include "ksim/combat/combat_data.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/los.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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

namespace {

using namespace ksim;

// The tolerances (docs/reference/INTERIORS.md "Height sweep"): from the spread of the surfaces that pass.
constexpr double kTolWire = 1e-6; // |wire y - navmesh face y|: the same number
constexpr double kTolLevel = 2.00; // |face - nearest collision hit under it|: flat ground and slopes measure up to 1.8 (the detail mesh floats over the terrain, k16.7.14)
constexpr double kRayStart = 20.0; // the client's ray starts this far over the navmesh point (rwh.130)
constexpr double kTownRadius = 260.0; // u around the town centre that is sampled
constexpr double kGridStep = 16.0; // u: the open-ground and interior grids
constexpr double kStairGrad = 0.35; // navmesh slope above which a point is a stair / ramp
constexpr double kSlopeGrad = 0.25; // terrain slope above which open ground is a slope

state::Fighter* fighterOf(Simulation& sim, std::uint64_t id)
{
	return const_cast<CombatArena*>(sim.combat())->find(id);
}

struct Sample
{
	std::string town;
	std::string surface;
	double x = 0, z = 0;
	double wireY = 0, hitY = 0, navY = 0, terrainY = 0; // hitY: the collision hit nearest the face
	bool rayHit = false; // any hit under the face
	int hits = 0; // hits in the column (walkable collision and terrain)
	std::string note;
};

struct Game
{
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	explicit Game(const std::string& bake, const kworld::ZoneCell& centre)
	{
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({ centre.x + dx, centre.z + dz });
		mesh = test::openWorldNav(bake, zones);
	}

	void newGame()
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		std::string startoff;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				startoff = record->stringID;
		sim->enqueue(Command::newGame(startoff, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}
	state::Fighter& me() { return *fighterOf(*sim, player); }
};

double percentile(std::vector<double> v, double p)
{
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());
	return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

// The level check: a hit within kTolLevel of the face (no hit at all is a failure too: nothing for the client to stand the body on).
bool failsLevel(const Sample& s) { return !s.rayHit || std::fabs(s.hitY - s.navY) > kTolLevel; }
bool failsSample(const Sample& s) { return std::fabs(s.wireY - s.navY) > kTolWire || failsLevel(s); }

std::string cellKey(const Sample& s)
{
	std::ostringstream out;
	out << s.surface << "\t" << std::lround(s.x / 16.0) * 16 << " " << std::lround(s.z / 16.0) * 16;
	return out.str();
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_height_sweep: SKIPPED: needs the world navmesh\n";
			return 77;
		}
		const std::string gapsPath = argc > 2 ? argv[2] : KSIM_HEIGHT_SWEEP_GAPS;
		std::vector<Sample> samples;

		struct TownSpec { const char* name; double x = 0, z = 0; std::vector<std::array<double, 2>> extra; };
		std::vector<TownSpec> towns;
		{
			const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
			const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
			const kworld::Towns townSet = kworld::Towns::join(*gd, KENSHI_DATA);
			for (const char* wanted : { "The Hub", "Heft" })
				for (const kworld::TownRecord& record : townSet.towns())
					if (record.name == wanted && !record.placementIndices.empty())
					{
						const kworld::TownPlacement& p = townSet.placements()[record.placementIndices.front()];
						towns.push_back({ wanted, p.x, p.z, {} });
						break;
					}
		}
		test::require(towns.size() == 2, "the data has The Hub and Heft");
		// The Hub placement's own point is a town of its own away from the Storm House / Rebel Base (zone 21.31, k16.8.29): add them.
		towns[0].extra = { { -50621.5, -1148.7 }, { -50544.0, -1231.0 } };

		for (const TownSpec& town : towns)
		{
			Game game(argv[1], kworld::zoneCellOf(town.x, town.z));
			game.newGame();
			Simulation& sim = *game.sim;
			{
				state::Fighter& f = game.me();
				f.x = town.x;
				f.z = town.z;
				f.y = sim.groundHeight(town.x, town.z).value_or(f.y);
				f.clearRoute();
				f.building = 0;
			}
			sim.run(200);
			spawnMeshDoors(sim, *game.mesh);
			test::require(sim.lineOfSightEnabled(), "the collision (line of sight) is on");
			const LineOfSight* los = sim.lineOfSight();
			const knav::Walker& walker = game.mesh->walker();

			// One point: the navmesh position `p` (a Walker::snap result), classified and measured.
			std::set<std::pair<long, long>> seen;
			const auto measure = [&](const knav::Position& p, const std::string& forced)
			{
				if (!los->loaded(los->regionOf(p.x, p.z)))
					return;
				if (!seen.insert({ std::lround(p.x / 1.5), std::lround(p.z / 1.5) }).second)
					return;
				Sample s;
				s.town = std::hypot(p.x + 50544.0, p.z + 1231.0) < 100.0 ? "Rebel Base" : town.name;
				s.x = p.x;
				s.z = p.z;
				s.navY = p.y;
				s.terrainY = sim.groundHeight(p.x, p.z).value_or(p.y);
				const std::uint64_t building = sim.buildingAt(p.x, p.y, p.z);
				// What the walker writes and the wire carries: the mesh point, unchanged by a step.
				s.wireY = walker.step(p, 0.0, 0.0).at.y;
				// Every walkable-collision / terrain hit in the column the client rays down (the terrain ends it).
				double top = p.y + kRayStart;
				double nearest = 1e9;
				for (; s.hits < 8; ++s.hits)
				{
					const LineOfSight::Hit hit = los->traceGround({ p.x, top, p.z }, { p.x, top - 80.0, p.z });
					if (!hit.blocked)
						break;
					if (std::fabs(hit.x[1] - p.y) < nearest)
					{
						nearest = std::fabs(hit.x[1] - p.y);
						s.hitY = hit.x[1];
					}
					if (std::fabs(hit.x[1] - s.terrainY) < 0.3)
					{
						++s.hits;
						break; // the terrain: nothing below it
					}
					top = hit.x[1] - 0.05;
				}
				s.rayHit = s.hits > 0;
				// The navmesh gradient (stairs and ramps) and the terrain's.
				double gradNav = 0.0, gradTerrain = 0.0;
				for (const double d : { 2.0 })
				{
					const auto a = walker.surfaceY(p.x + d, p.y, p.z, 2.0);
					const auto b = walker.surfaceY(p.x - d, p.y, p.z, 2.0);
					const auto c = walker.surfaceY(p.x, p.y, p.z + d, 2.0);
					const auto e = walker.surfaceY(p.x, p.y, p.z - d, 2.0);
					if (a && b) gradNav = std::max(gradNav, std::fabs(*a - *b) / (2 * d));
					if (c && e) gradNav = std::max(gradNav, std::fabs(*c - *e) / (2 * d));
					gradTerrain = std::max(std::fabs(sim.groundHeight(p.x + d, p.z).value_or(0) - sim.groundHeight(p.x - d, p.z).value_or(0)),
						std::fabs(sim.groundHeight(p.x, p.z + d).value_or(0) - sim.groundHeight(p.x, p.z - d).value_or(0))) / (2 * d);
				}
				if (!forced.empty() && forced != "grid")
					s.surface = forced;
				else if (gradNav > kStairGrad)
					s.surface = "stair";
				else if (building != 0)
					s.surface = sim.buildingFloor(building, p.y) >= 1 ? "upper" : s.terrainY - p.y > 0.5 ? "sunk" : "floor0";
				else if (p.y - s.terrainY > 1.0)
					s.surface = "deck";
				else
					s.surface = gradTerrain > kSlopeGrad ? "slope" : "flat";
				if (forced == "door" && gradNav > kStairGrad)
					s.note = "door step on a stair";
				samples.push_back(s);
			};

			std::vector<std::array<double, 2>> centres{ { town.x, town.z } };
			centres.insert(centres.end(), town.extra.begin(), town.extra.end());
			const auto nearCentre = [&](double x, double z, double radius)
			{
				return std::any_of(centres.begin(), centres.end(), [&](const std::array<double, 2>& c) { return std::hypot(x - c[0], z - c[1]) < radius; });
			};
			// Door thresholds: along the door's axis, both sides.
			std::vector<state::Door> doors;
			sim.world().ecs().each([&](const state::Door& door)
			{
				if (door.building != 0 && !door.gate && (door.inX != 0.0 || door.inZ != 0.0) && nearCentre(door.x, door.z, 2500.0))
					doors.push_back(door);
			});
			std::printf("height_sweep: %s: %zu doors\n", town.name, doors.size());
			for (const state::Door& door : doors)
				for (const double along : { -8.0, -5.0, -2.5, 0.0, 2.5, 5.0, 8.0 })
					if (const auto spot = walker.snap({ door.x + door.inX * along, door.y, door.z + door.inZ * along }, 2.0, nullptr, 6.0))
						measure(*spot, "door");

			// Building interiors: a grid over each footprint at the ground and every upper floor.
			std::size_t buildings = 0;
			std::set<std::uint64_t> doneSites;
			std::vector<const BuildingSite*> sitePtrs;
			for (const BuildingSite& site : sim.world().buildingSites())
				if (nearCentre(site.cx, site.cz, kTownRadius) && doneSites.insert(site.key).second)
					sitePtrs.push_back(&site);
			for (const state::Door& door : doors)
				if (doneSites.insert(door.building).second)
					if (const BuildingSite* site = sim.world().buildingSite(door.building))
						sitePtrs.push_back(site);
			for (const BuildingSite* sitePtr : sitePtrs)
			{
				const BuildingSite& site = *sitePtr;
				++buildings;
				std::vector<double> levels{ site.groundY + 2.0 };
				for (const double floor : site.floors)
					levels.push_back(floor + 2.0);
				for (double a = -site.halfA; a <= site.halfA; a += kGridStep / 2)
					for (double b = -site.halfB; b <= site.halfB; b += kGridStep / 2)
						for (const double y : levels)
							if (const auto spot = walker.snap({ site.cx + a * site.axisX - b * site.axisZ, y, site.cz + a * site.axisZ + b * site.axisX }, 1.5, nullptr, 3.0))
								measure(*spot, "grid");
			}
			std::printf("height_sweep: %s: %zu buildings sampled\n", town.name, buildings);

			// Open ground, decks and slopes: a grid round each centre, snapped to the mesh at the terrain height and 3 / 8 / 15 u above.
			for (const std::array<double, 2>& c : centres)
				for (double dx = -kTownRadius; dx <= kTownRadius; dx += kGridStep)
					for (double dz = -kTownRadius; dz <= kTownRadius; dz += kGridStep)
					{
						const double x = c[0] + dx, z = c[1] + dz;
						const double t = sim.groundHeight(x, z).value_or(0.0);
						for (const double up : { 0.0, 3.0, 8.0, 15.0 })
							if (const auto spot = walker.snap({ x, t + up, z }, 2.0, nullptr, 2.0))
								measure(*spot, "grid");
					}
			// Diagnosis: the Wanderer's spot in the Storm House door (k16.8.29, F12 192943): the wire y and what the client's ray sees.
			if (!town.extra.empty())
			{
				const double dx = town.extra[0][0], dz = town.extra[0][1];
				const auto face = walker.snap({ dx, 1078.0, dz }, 1.0, nullptr, 6.0);
				const double terrain = sim.groundHeight(dx, dz).value_or(0.0);
				const double navY = face ? face->y : terrain;
				std::printf("height_sweep: DIAG storm house door (%.1f, %.1f): navmesh face y %.3f = wire y %.3f, terrain %.3f (face %.3f u above it)\n", dx, dz, navY,
					face ? walker.step(*face, 0.0, 0.0).at.y : terrain, terrain, navY - terrain);
				for (const bool groundOnly : { true, false })
				{
					double top = navY + kRayStart;
					std::printf("height_sweep: DIAG   %s hits down from face + 20:", groundOnly ? "walkable+terrain" : "all collision+terrain");
					for (int hits = 0; hits < 4; ++hits)
					{
						const auto hit = groundOnly ? los->traceGround({ dx, top, dz }, { dx, top - 80.0, dz }) : los->trace({ dx, top, dz }, { dx, top - 80.0, dz });
						if (!hit.blocked)
							break;
						std::printf(" %.3f", hit.x[1]);
						if (std::fabs(hit.x[1] - terrain) < 0.3)
							break; // the terrain: nothing below it
						top = hit.x[1] - 0.05;
					}
					std::printf("\n");
				}
			}
			// The Storm House door step (k16.8.29): a fine grid over the threshold.
			if (town.extra.size() > 0)
				for (double dx = -12.0; dx <= 12.0; dx += 1.5)
					for (double dz = -12.0; dz <= 12.0; dz += 1.5)
					{
						const double x = town.extra[0][0] + dx, z = town.extra[0][1] + dz;
						const double t = sim.groundHeight(x, z).value_or(0.0);
						for (const double up : { 0.0, 2.5 })
							if (const auto spot = walker.snap({ x, t + up, z }, 0.75, nullptr, 2.0))
								measure(*spot, "grid");
					}
		}

		// Spread per surface.
		std::map<std::string, std::vector<const Sample*>> bySurface;
		for (const Sample& s : samples)
			bySurface[s.surface].push_back(&s);
		std::printf("height_sweep: %zu points\n", samples.size());
		std::printf("height_sweep: %-8s %6s | %-30s | %s\n", "surface", "n", "|face-nearest hit|: mean p95 max", "wire != face | no hit | level fails");
		for (const auto& [surface, list] : bySurface)
		{
			std::vector<double> d;
			double sum = 0;
			std::size_t wireOff = 0, noHit = 0, levelFails = 0;
			for (const Sample* s : list)
			{
				d.push_back(s->rayHit ? std::fabs(s->hitY - s->navY) : 0.0);
				sum += d.back();
				wireOff += std::fabs(s->wireY - s->navY) > kTolWire ? 1 : 0;
				noHit += s->rayHit ? 0 : 1;
				levelFails += failsLevel(*s) ? 1 : 0;
			}
			std::printf("height_sweep: %-8s %6zu | %7.3f %7.3f %7.3f | %zu | %zu | %zu\n", surface.c_str(), list.size(), sum / list.size(), percentile(d, 0.95),
				*std::max_element(d.begin(), d.end()), wireOff, noHit, levelFails);
		}
		{
			std::map<std::string, std::array<double, 3>> bySite; // n, fails, max |face - nearest hit|
			for (const Sample& s : samples)
			{
				auto& row = bySite[s.town];
				row[0] += 1;
				row[1] += failsSample(s) ? 1 : 0;
				row[2] = std::max(row[2], s.rayHit ? std::fabs(s.hitY - s.navY) : 0.0);
			}
			for (const auto& [site, row] : bySite)
				std::printf("height_sweep: site %-11s n %5.0f failing %5.0f max|face-hit| %.3f\n", site.c_str(), row[0], row[1], row[2]);
		}
		{
			std::ofstream out("height_sweep.tsv");
			out << "town\tsurface\tx\tz\twire_y\thit_y\tnav_y\tterrain_y\n";
			for (const Sample& s : samples)
				out << s.town << "\t" << s.surface << "\t" << s.x << "\t" << s.z << "\t" << s.wireY << "\t" << s.hitY << "\t" << s.navY << "\t" << s.terrainY << "\n";
		}

		// The known gaps.
		std::map<std::string, std::string> known;
		{
			std::ifstream in(gapsPath);
			std::string line;
			while (std::getline(in, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				const std::size_t second = line.find('\t', line.find('\t') + 1);
				known[line.substr(0, second)] = line.substr(second + 1);
			}
		}
		std::map<std::string, const Sample*> failing;
		for (const Sample& s : samples)
			if (failsSample(s))
				failing.emplace(cellKey(s), &s);
		std::size_t unknown = 0;
		for (const auto& [key, s] : failing)
			if (known.count(key) == 0)
			{
				++unknown;
				std::printf("height_sweep: GAP %s (%s) wire %.3f nearest hit %.3f (%d hits) face %.3f terrain %.3f\n", key.c_str(), s->town.c_str(), s->wireY, s->hitY, s->hits, s->navY, s->terrainY);
			}
		for (const auto& [key, note] : known)
			if (failing.count(key) == 0)
				std::printf("height_sweep: known gap now passes: %s\n", key.c_str());
		std::printf("height_sweep: %zu failing cells, %zu not in the known-gap file\n", failing.size(), unknown);
		test::require(unknown == 0, "every failing cell is a known gap");
		std::cout << "ksim_height_sweep: OK\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_height_sweep: " << e.what() << "\n";
		return 1;
	}
}
