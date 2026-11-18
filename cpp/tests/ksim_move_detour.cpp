// A short right-click move on open ground routes straight (kenshi-k16.6.16). The user's click, 2026-11-09: character 41906 at
// (-52165.3, 1307.6, 2607.2), move_to (-52155, 1307, 2653), 47 u away near the Hub; the route went down-left over three corners
// and came back. Detour places a search node at the middle of each polygon edge it crosses, so on a polygon edge 140 u long
// the cheapest corridor by those midpoints is a long way round the straight one; the funnel then pulls it tight round the
// corner the corridor forces. Planned here the way simlink move_to does it (MeshNavigator::routeFrom: the click height, the human
// clearance radius 4) on the runtime mesh, the server default: the user's own query, which plans straight on today's meshes
// (the desktop's navworld_v9 bake it came from is not on the dev box: `ksim_move_detour BAKE_DIR` plans it on that bake),
// and eight short moves near the Hub, open ground in a straight line, that ran 1.4-3.3x the straight distance before the fix.
// Each route must stay under 1.3x the distance to its own end. Needs the install (KENSHI_DATA); exit 77 without it.

#include "ksim/combat_data.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"
#include "ksim/simlink_scenario.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "knav/native.hpp"

#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

struct Move
{
	const char* what;
	double fromX, fromY, fromZ;
	double toX, toZ;
	std::optional<double> toY;
};

const Move kMoves[] = {
	{ "the user's click", -52165.3, 1307.6, 2607.2, -52155.0, 2653.0, 1307.0 },
	{ "seam, south", -52261.3, 1272.66, 2487.2, -52203.5, 2471.91, std::nullopt },
	{ "seam, west", -52201.3, 1286.26, 2595.2, -52248.9, 2583.85, std::nullopt },
	{ "seam z 2835", -52249.3, 1272.79, 2835.2, -52191.8, 2844.82, std::nullopt },
	{ "seam z 2847 a", -52249.3, 1273.02, 2847.2, -52199.9, 2839.13, std::nullopt },
	{ "seam z 2847 b", -52225.3, 1287.59, 2847.2, -52250.2, 2805.69, std::nullopt },
	{ "seam z 2847 c", -52189.3, 1312.05, 2847.2, -52234.4, 2849.66, std::nullopt },
	{ "seam z 2859", -52213.3, 1298.02, 2859.2, -52253.4, 2899.83, std::nullopt },
	{ "seam z 2871", -52189.3, 1314.66, 2871.2, -52230.8, 2883.81, std::nullopt },
};


// The "seam, west" move walked by the sim (kenshi-k16.8.8), with the click's ground height (1256: the client's click was on the
// ledge, the route ends on the click and bows round the north end of an obstacle line, not the tile seam): a player character at
// each of eight facings, the route as a move order. The velocity's heading must not change over 30 degrees in one tick from tick 2
// to 11 (the client saw 123 -> 127 -> 62 -> 144 degrees against a target near -166). The sim has no such flip; the bow is the route's.
void traceStart(const std::shared_ptr<ksim::MeshNavigator>& nav)
{
	using namespace ksim;
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	CombatSettings settings;
	settings.substeps = 2;
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(data, settings);
	sim.enableNavigation(nav);
	std::string sid;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			sid = r->stringID;
	const Move& move = kMoves[2];
	const double kPi = 3.14159265358979323846;
	for (int k = 0; k < 8; ++k)
	{
		Simulation sim = Simulation::load(KENSHI_DATA, 77);
		CombatSettings settings;
		settings.substeps = 2;
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		std::uint64_t rng = 12345;
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.playerControlled = true;
		f.yaw = k * kPi / 4.0;
		f.x = move.fromX;
		f.y = move.fromY;
		f.z = move.fromZ;
		const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		const Route route = nav->routeFrom({ move.fromX, move.fromY, move.fromZ }, move.toX, move.toZ, 1256.0, false, 4.0, true);
		sim.enqueue(Command::fighterMoveTo(state::Id{id}, route.points, route.heights, 1));
		double last = 0.0;
		double worst = 0.0;
		double length = 0.0;
		double px = move.fromX, pz = move.fromZ;
		double maxZ = move.fromZ;
		for (int tick = 0; tick < 100; ++tick)
		{
			sim.run(1);
			const state::Fighter& me = *sim.combat()->find(id);
			length += std::hypot(me.x - px, me.z - pz);
			px = me.x;
			pz = me.z;
			maxZ = std::max(maxZ, me.z);
			if (std::hypot(me.velX, me.velZ) < 1.0)
				continue;
			const double heading = std::atan2(me.velX, me.velZ) * 180.0 / kPi;
			double turn = std::fabs(heading - last);
			turn = turn > 180.0 ? 360.0 - turn : turn;
			if (tick >= 2 && tick < 12)
				worst = std::max(worst, turn);
			last = heading;
		}
		std::cout << "yaw0 " << k * 45 << ": worst turn in ticks 2-11 " << worst << " deg, path " << length << " of "
				  << std::hypot(px - move.fromX, pz - move.fromZ) << " straight, max z " << maxZ << "\n";
		test::require(worst < 30.0, "seam, west: the heading flips over 30 degrees in one tick at the route start");
	}
}

// The client's move_to of kenshi-k16.8.8 (route seq 18): four points from the walker's real start, which stands 2.15 u from the first one
// (it had arrived 2.6 u short of an earlier target), at rest. The walker used to pass the start 1 u off, steer back for a tick and turn
// 82 degrees; the start within a few units with a clear way to the second point is dropped now. Each of eight bearings of that 2.15 u:
// the velocity heading never turns over 30 degrees in one tick (the initial slew onto the first leg, ticks 0-1, aside).
void replayFirstPoint(const std::shared_ptr<ksim::MeshNavigator>& nav)
{
	using namespace ksim;
	const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
	const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
	std::string sid;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (r->name == "Escaped Servant")
			sid = r->stringID;
	const double kPi = 3.14159265358979323846;
	const std::vector<state::GroundPoint> points = { { -52201.45, 2594.79 }, { -52224.32, 2604.99 }, { -52243.88, 2602.12 }, { -52249.73, 2584.63 } };
	const std::vector<double> heights = { 1286.11, 1268.14, 1255.87, 1256.03 };
	for (int k = 0; k < 8; ++k)
	{
		Simulation sim = Simulation::load(KENSHI_DATA, 77);
		CombatSettings settings;
		settings.substeps = 2;
		sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		std::uint64_t rng = 12345;
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.playerControlled = true;
		f.yaw = k * kPi / 4.0;
		f.x = points[0].x + 2.15 * std::sin(k * kPi / 4.0);
		f.z = points[0].z + 2.15 * std::cos(k * kPi / 4.0);
		f.y = heights[0];
		const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
		sim.enqueue(Command::fighterMoveTo(state::Id{id}, points, heights, 1));
		double last = 0.0;
		double worst = 0.0;
		for (int tick = 0; tick < 120; ++tick)
		{
			sim.run(1);
			const state::Fighter& me = *sim.combat()->find(id);
			if (std::hypot(me.velX, me.velZ) < 1.0)
			{
				last = 0.0;
				continue;
			}
			const double heading = std::atan2(me.velX, me.velZ) * 180.0 / kPi;
			double turn = std::fabs(heading - last);
			turn = turn > 180.0 ? 360.0 - turn : turn;
			if (tick >= 2 && last != 0.0)
				worst = std::max(worst, turn);
			last = heading;
		}
		std::cout << "first point, bearing " << k * 45 << ": worst one-tick turn " << worst << " deg\n";
		test::require(worst < 30.0, "the walk turns over 30 degrees in one tick at the route start (bearing " + std::to_string(k * 45) + ")");
	}
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_move_detour: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz });
		const std::shared_ptr<ksim::MeshNavigator> nav = argc > 1 ? ksim::MeshNavigator::openWorld(argv[1], hub)
			: ksim::MeshNavigator::openNative(KENSHI_DATA, hub);
		// Twice: the player's click (straighten asked for) and an AI / follow / chase route (the default, k16.8.7).
		for (const Move& move : kMoves)
		for (const bool player : { true, false })
		{
			const ksim::Route route = player ? nav->routeFrom({ move.fromX, move.fromY, move.fromZ }, move.toX, move.toZ, move.toY, false, 4.0, true)
				: nav->routeFrom({ move.fromX, move.fromY, move.fromZ }, move.toX, move.toZ, move.toY, false, 4.0);
			test::require(route.found && route.points.size() >= 2, std::string(move.what) + (player ? " (click)" : " (AI)") + ": no route");
			double length = 0.0;
			double x = move.fromX;
			double z = move.fromZ;
			for (const auto& point : route.points)
			{
				length += std::hypot(point.x - x, point.z - z);
				x = point.x;
				z = point.z;
			}
			const double straight = std::hypot(x - move.fromX, z - move.fromZ);
			std::cout << move.what << (player ? " (click): " : " (AI): ") << route.points.size() << " points, " << length << " u, straight " << straight << " u, ratio "
					  << length / straight << "\n";
			test::require(length < 1.3 * straight, std::string(move.what) + (player ? " (click)" : " (AI)") + ": the route is over 1.3x the straight distance");
		}
		if (argc <= 1)
		{
			traceStart(nav);
			replayFirstPoint(nav);
		}
		// A click on the Hub's ramp top (-50802, 2756), kenshi-k16.8.10 (client live check, 2026-11-09). The mesh there exists only at y 1554.97,
		// 31 u over the ground beside it. Without a click height the goal is the ground nearest in xz, so the route ends on the ramp top (the
		// goal snap used to be redone in 3D from the walker's own height and picked the ground 5.6 u away, and the radius-4 goal push made it
		// 12 u from the click). With the client's heightmap y (1524, the terrain under the ramp) the nearest point in 3D is that ground:
		// UNSURE (ours, default "nearest fit"; deferred: playtesting): the route ends at the nearest point the body fits at that height
		// (12 u; no fitting ground is nearer, the nearest fitting polygon is 27 u off), not on the ramp 31 u higher.
		{
			const double fromX[] = { -50956.61, -50849.14 };
			const double fromY[] = { 1528.82, 1523.97 };
			const double fromZ[] = { 2979.38, 2740.23 };
			for (int i = 0; i < 2; ++i)
			{
				const ksim::Route top = nav->routeFrom({ fromX[i], fromY[i], fromZ[i] }, -50802.0, 2756.0, std::nullopt, false, 4.0, true, true);
				test::require(top.found && !top.approach, "the ramp-top click (no y) routes to the click itself");
				test::require(std::hypot(top.points.back().x + 50802.0, top.points.back().z - 2756.0) < 2.0 && std::fabs(top.heights.back() - 1554.97) < 1.0,
					"the ramp-top click (no y) ends within 2 u of the ramp top, on its floor");
				const ksim::Route ground = nav->routeFrom({ fromX[i], fromY[i], fromZ[i] }, -50802.0, 2756.0, 1524.0, false, 4.0, true);
				test::require(ground.found && std::hypot(ground.points.back().x + 50802.0, ground.points.back().z - 2756.0) < 13.0
						&& std::fabs(ground.heights.back() - 1524.0) < 3.0,
					"the ramp-top click at the terrain height ends on the ground, at the nearest fit");
			}
		}
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_move_detour: " << e.what() << "\n";
		return 1;
	}
}
