// kenshi-k16.6.253 (playtest cage_prisoner, Heft Longhouse): a character never stands inside a solid prop. cage_prisoner loaded as scenario files load
// it, then run 60 game s of the fight; at the spawn and every second no character (the caged prisoner excepted: its slot is inside its cage by design)
// is inside the collision of a placed Obstacle / Projected solid (prop_clip.hpp) grown by its body: the centre stays at least its path radius less
// kTolerance from every solid (k16.6.321; the runtime mesh has no erosion, so 0 u slides along a prop would show here). A character is held to that
// margin once it has moved (k16.6.383: on by default since the bake's prop clearance ring, k16.7.89, gives the follow slots their clearance). Needs the world navmesh bake (argv[1], exit 77 without the Heft
// region); a moved sample inside the margin on a cell the bake deliberately hands back (k16.7.102 gap lane, k16.7.89 stair, use-node spare: ExemptProbe re-bakes the zone's tiles with knav::recordExemptCells) is counted apart, not a clip (k16.6.383). KSIM_NATIVE_NAV=1 runs it on the runtime mesh (the shipped tiles), else on the bake.
#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/heightmap.hpp"
#include "prop_clip.hpp"
#include "scenario_rig.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>


using namespace ksim;

// The body-width margin may be eaten by this much (a step across a mesh edge, the box test's square corners).
constexpr double kTolerance = 1.5;
// How far (xz, u) a handed-back cell centre may be from a sample: the body rim, a cell and a half.
constexpr double kExemptReach = 2.5;

// k16.6.383: a sample inside the ring that stands on a cell the bake deliberately hands back (k16.7.102 gap lane, k16.7.89 stair, use-node spare) is walkable
// by design, not a clip. The zone's tiles round the sample are baked here with the bake's own side table on (knav::recordExemptCells), so the predicate is the
// bake's, not a copy; a cell within kExemptReach of the sample (xz) and a storey of its height counts.
class ExemptProbe
{
public:
	ExemptProbe() : terrain_(kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif")) {}

	// "" when the sample is on no handed-back cell, else the kinds found; walkable says the re-baked mesh has the sample's floor.
	std::string kinds(double x, double y, double z, bool& walkable)
	{
		const kworld::ZoneCell zone = kworld::zoneCellOf(x, z);
		auto found = zones_.find({ zone.x, zone.z });
		if (found == zones_.end())
		{
			Zone built;
			built.area = knav::gatherArea(KENSHI_DATA, terrain_, zone, zone);
			built.shapes = built.area.flatShapes();
			built.config = knav::configForWorldZones(zone, zone);
			found = zones_.emplace(std::make_pair(zone.x, zone.z), std::move(built)).first;
		}
		Zone& z0 = found->second;
		std::vector<const kworld::WorldShape*> pointers;
		std::set<std::pair<int, int>> tiles;
		for (const kworld::WorldShape& shape : z0.shapes)
		{
			pointers.push_back(&shape);
			if (shape.max[0] < x - 24.0 || shape.min[0] > x + 24.0 || shape.max[2] < z - 24.0 || shape.min[2] > z + 24.0)
				continue;
			for (const knav::TileCoord& t : knav::tilesTouching(z0.config, shape))
				tiles.insert({ t.x, t.z });
		}
		std::vector<knav::TileCoord> coords;
		for (const auto& [tx, tz] : tiles)
			coords.push_back({ tx, tz });
		knav::recordExemptCells(true);
		const knav::NavMesh mesh = knav::NavMesh::fromTiles(z0.config, knav::bakeTileSet(z0.config, terrain_, pointers, coords, 2));
		const std::vector<knav::ExemptCell> cells = knav::takeExemptCells();
		knav::recordExemptCells(false);
		knav::Point floor{};
		walkable = knav::NavQuery(mesh).nearestPoint({ static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) }, { 0.6f, 3.0f, 0.6f }, floor);
		std::string out;
		for (const knav::ExemptCell& c : cells)
			if (std::hypot(c.x - x, c.z - z) <= kExemptReach && std::fabs(c.y - y) <= 6.0 && out.find(c.kind) == std::string::npos)
				out += c.kind;
		return out;
	}

private:
	struct Zone
	{
		knav::AreaGeometry area;
		std::vector<kworld::WorldShape> shapes;
		knav::NavConfig config;
	};
	kworld::Heightmap terrain_;
	std::map<std::pair<int, int>, Zone> zones_;
};

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/cage_prisoner.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_prop_clip: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		prop_clip::PropClip clip(*rig.gameData);
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t prisoner = placed.idOf.at(2);
		std::size_t bad = 0;
		std::size_t exempted = 0;
		ExemptProbe exempt;
		std::map<std::uint64_t, std::uint32_t> lastRoute; // routeVersion per member: a follower held against a prop must not re-route every tick
		std::size_t routeSteps = 0;
		std::map<std::uint64_t, std::size_t> perMember;
		// A character is held to the body margin once it has moved: a spawn position is authored (the scenario's cast stands 2 u off the cage by
		// design, the town's idle NPCs on world data's own spots), while a clearance lost after a step is the steer's (k16.6.321).
		std::map<std::uint64_t, std::array<double, 2>> spawnAt;
		const auto check = [&](const std::string& when)
		{
			for (const CombatArena::Member& m : sim.combat()->members())
			{
				if (m.id == prisoner || m.fighter.dead || m.fighter.cageBuilding != 0) // k16.6.400: an unconscious body is checked too (a take-out's drop point must be open ground); a caged one lies in its cage's own box by design (imprison puts it on the cage's occupant slot, ksim_cage_inside)
					continue;
				const auto [at, first] = spawnAt.try_emplace(m.id, std::array<double, 2>{ m.fighter.x, m.fighter.z });
				const bool moved = std::hypot(m.fighter.x - at->second[0], m.fighter.z - at->second[1]) > 1.0;
				const std::string in = moved ? clip.touches(m.fighter.x, m.fighter.y, m.fighter.z, std::max(0.0, sim.pathRadius(m.fighter) - kTolerance))
					: clip.inside(m.fighter.x, m.fighter.y, m.fighter.z);
				bool rebakedWalkable = false;
				if (!in.empty() && moved)
				{
					bool& walkable = rebakedWalkable;
					const std::string kinds = exempt.kinds(m.fighter.x, m.fighter.y, m.fighter.z, walkable);
					if (!kinds.empty())
					{
						++exempted;
						std::cout << when << ": char " << m.id << " at (" << m.fighter.x << ", " << m.fighter.y << ", " << m.fighter.z << ") exempt: on a cell the bake hands back (" << kinds << "), walkable " << walkable << ", within its body of " << in << "\n";
						continue;
					}
				}
				if (!in.empty())
				{
					std::cout << when << ": char " << m.id << " at (" << m.fighter.x << ", " << m.fighter.y << ", " << m.fighter.z << ") is inside (or, once moved, within its body of) " << in << (moved ? (rebakedWalkable ? " [no handed-back cell; rebaked mesh walkable here]" : " [no handed-back cell; rebaked mesh NOT walkable here]") : "") << "\n";
					++bad;
				}
			}
		};
		check("spawn");
		sim.enqueue(Command::fighterAttack(state::Id{placed.idOf.at(1)}, state::Id{placed.idOf.at(3)}));
		for (int tick = 1; tick <= 1200; ++tick)
		{
			sim.tick();
			for (const CombatArena::Member& m : sim.combat()->members())
			{
				const auto [at, fresh] = lastRoute.emplace(m.id, m.fighter.routeVersion);
				if (!fresh && at->second != m.fighter.routeVersion)
				{
					++routeSteps;
					++perMember[m.id];
				}
				at->second = m.fighter.routeVersion;
			}
			if (tick % 20 == 0)
				check("t=" + std::to_string(tick / 20) + "s");
		}
		// kenshi-k16.6.330: the hugsWall path cleared the route before undoing the shift, so chaseStep replanned (and the wire resent CHAR_ROUTE) every tick.
		std::size_t worst = 0;
		for (const auto& [id, n] : perMember)
		{
			worst = std::max(worst, n);
			if (n > 100)
				std::cout << "busy member " << id << ": " << n << " route version steps\n";
		}
		std::cout << "route version steps over 1200 ticks, all members: " << routeSteps << ", the most for one: " << worst << "\n";
		// A replan every tick is 800+ steps (772 / 800 measured before k16.6.360's askedRecently reached the take-out walker, 503 / 1028 for followers on the k16.6.253 base); one clear and one replan per kChaseMinPlan (0.5 s) is 240 in 60 s, 292 measured on the bake mesh. The cap sits between.
		test::require(worst < 400, "a member's route version stepped " + std::to_string(worst) + " times in 1200 ticks (a replan every tick while held on the wall)");
		std::cout << "samples exempt (on a gap-lane / stair / use-node cell the bake hands back): " << exempted << "\n";
		test::require(bad == 0, std::to_string(bad) + " (character, time) samples stood inside a solid prop");
		std::cout << "ksim_prop_clip ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
