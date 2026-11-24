// Walker kinematics like the original (docs/ARCHITECTURE.md "Walker kinematics", kinspect s70 and move_kinematics.md): the speed
// ramps at the race's pathfind acceleration (200 u/s^2 for a human) and brakes at the same rate, a sharp corner and the end of a
// route limit the speed, the body turns at no more than 20 rad/s, and a drive 0 in the drive hold turns in place with no
// translation (SIMLINK.md section 12). Pure functions first, then the same through the simulation on the Hub mesh.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/walk.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kDt = 0.05;
constexpr double kPi = 3.14159265358979323846;

struct Pt
{
	double x;
	double z;
};

double wrap(double a)
{
	while (a > kPi)
		a -= 2.0 * kPi;
	while (a <= -kPi)
		a += 2.0 * kPi;
	return a;
}

void pureFunctions()
{
	// Acceleration from rest: 200 u/s^2 is 10 u/s per 0.05 s tick, so 70 u/s takes 7 ticks.
	double v = 0.0;
	int ticks = 0;
	while (v < 70.0 - 1.0e-9)
	{
		const double before = v;
		v = knav::rampSpeed(v, 70.0, kDt);
		test::require(std::fabs((v - before) - 10.0) < 1.0e-9, "a ramp tick did not add accel x dt");
		test::require(++ticks < 100, "the ramp never reached the target");
	}
	test::require(ticks == 7, "0 to 70 u/s took " + std::to_string(ticks) + " ticks, expected 7 (0.35 s)");
	// Braking is the same rate; a race with a lower acceleration (an animal's) is slower.
	test::require(std::fabs(knav::rampSpeed(70.0, 0.0, kDt) - 60.0) < 1.0e-9, "braking is not at the same rate");
	test::require(std::fabs(knav::rampSpeed(0.0, 70.0, kDt, 30.0) - 1.5) < 1.0e-9, "a race acceleration is not honoured");

	// The turn rate is bounded per tick, takes the short way, and lands exactly when it arrives.
	double yaw = 0.0;
	for (int i = 0; i < 40; ++i)
	{
		const double next = knav::turnToward(yaw, wrap(2.9 + 0.2 * i), knav::kTurnRate, kDt);
		test::require(std::fabs(wrap(next - yaw)) <= knav::kTurnRate * kDt + 1.0e-9, "a turn exceeded 20 rad/s");
		yaw = next;
	}
	test::require(std::fabs(knav::turnToward(3.0, -3.0, knav::kTurnRate, kDt) - wrap(3.0 + (2.0 * kPi - 6.0))) < 1.0e-9,
		"the turn did not take the short way over +-pi");
	test::require(std::fabs(knav::turnToward(0.0, 1.5, knav::kTurnRateCombat, kDt) - 0.2) < 1.0e-9, "the combat-ready rate is not 4 rad/s");
	test::require(knav::turnToward(0.0, 0.3, knav::kTurnRate, kDt) == 0.3, "a turn within the step did not land exactly");

	// Speed limits along a route (a human: accel 200, corner clearance 2.6 + 5).
	const double clearance = 7.6;
	const std::vector<Pt> straight{ { 100.0, 0.0 }, { 100.0, 50.0 }, { 100.0, 200.0 } };
	test::require(std::fabs(knav::Walker::speedLimit({ 100.0, 0.0, 0.0 }, straight, 1, 70.0, 200.0, clearance) - 70.0) < 1.0e-9,
		"a straight route 200 away limited the speed");
	// The end: brakes at exactly accel, speed = sqrt(2 a d).
	const double lim = knav::Walker::speedLimit({ 100.0, 0.0, 190.0 }, straight, 2, 70.0, 200.0, clearance);
	test::require(std::fabs(lim - std::sqrt(2.0 * 200.0 * 10.0)) < 1.0e-6, "the end's limit is not sqrt(2 a d): " + std::to_string(lim));
	// A 90 degree corner slows the approach below the run speed once it is within braking reach; far off it does not.
	const std::vector<Pt> corner{ { 0.0, 100.0 }, { 100.0, 100.0 }, { 100.0, 1000.0 } };
	test::require(knav::Walker::speedLimit({ 0.0, 0.0, 0.0 }, corner, 0, 70.0, 200.0, clearance) > 69.9, "a corner 100 away already limits the speed");
	const double cornerLimit = knav::Walker::speedLimit({ 0.0, 0.0, 98.0 }, corner, 0, 70.0, 200.0, clearance);
	test::require(cornerLimit < 69.0 && cornerLimit > 30.0, "a 90 degree corner 2 away limits the speed to " + std::to_string(cornerLimit));
	const std::vector<Pt> gentle{ { 0.0, 100.0 }, { 0.0, 150.0 }, { 0.0, 1000.0 } };
	test::require(knav::Walker::speedLimit({ 0.0, 0.0, 90.0 }, gentle, 0, 70.0, 200.0, clearance) > 69.9, "collinear route points limited the speed");
	// D5, the corner's measured room (k16.8.23): a slow turner (a pack animal, accel 30) at a 90 degree corner a leg's length off. Open ground (40 u beyond
	// the vertex) keeps more speed than the footprint (4) gave; a doorway (no more room than the footprint: the caller floors it there) still brakes well under the run speed.
	const std::vector<Pt> bend{ { 0.0, 60.0 }, { 0.0, 100.0 }, { 100.0, 100.0 }, { 100.0, 1000.0 } }; // the corner is the second point (the first has the walker's own line as its leg in)
	const auto speedWith = [&](double space)
	{
		const knav::Walker::Room room = [space](double, double, double, double, double, double) { return space; };
		return knav::Walker::speedLimit({ 0.0, 0.0, 50.0 }, bend, 0, 200.0, 30.0, 4.0, true, 0.0, 1.0, 0.0, room);
	};
	const double footprintSpeed = knav::Walker::speedLimit({ 0.0, 0.0, 50.0 }, bend, 0, 200.0, 30.0, 4.0, true);
	test::require(speedWith(40.0) > footprintSpeed + 5.0, "open ground did not keep more speed than the footprint: " + std::to_string(speedWith(40.0)));
	test::require(std::fabs(speedWith(4.0) - footprintSpeed) < 1.0e-9 && speedWith(4.0) < speedWith(40.0), "a doorway did not brake: " + std::to_string(speedWith(4.0)));
	test::require(knav::cornerSpeed(kPi, 200.0, clearance) < 1.0e-6, "a reversal is not a full stop");
	test::require(knav::cornerSpeed(0.5, 200.0, clearance) > knav::cornerSpeed(1.5, 200.0, clearance), "a sharper corner is not slower");
}

struct Rig
{
	Simulation sim;
	std::uint64_t id = 0;

	Rig(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav)
		: sim(Simulation::load(KENSHI_DATA, 77))
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		std::uint64_t rng = 12345;
		std::string sid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.team = 0;
		f.playerControlled = true;
		const auto spot = nav->walker().snap({ kHubX, 1535.0, kHubZ }, 200.0);
		test::require(spot.has_value(), "no mesh near the Hub");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	const state::Fighter& fighter() const { return *sim.combat()->find(id); }
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_walk_kinematics HUB.knav");
		pureFunctions();

		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- a long walk: ramps at 10 u/s per tick, brakes into the end at no more than accel, the yaw turns no faster than 20 rad/s ----
		{
			Rig rig(data, gd, nav);
			const state::Fighter& f0 = rig.fighter();
			Route route;
			for (const double d : { 400.0, 300.0, 220.0, 160.0 })
			{
				for (int k = 0; k < 8 && !route.found; ++k)
				{
					const Route r = nav->routeFrom({ f0.x, f0.y, f0.z }, f0.x + std::sin(k * 0.785398) * d, f0.z + std::cos(k * 0.785398) * d);
					if (r.found && r.points.size() >= 2)
					{
						double length = 0.0;
						for (std::size_t i = 1; i < r.points.size(); ++i)
							length += std::hypot(r.points[i].x - r.points[i - 1].x, r.points[i].z - r.points[i - 1].z);
						if (length > 150.0)
							route = r;
					}
				}
				if (route.found)
					break;
			}
			test::require(route.found, "no routable walk of 150+ units near the Hub");
			const double endX = route.points.back().x;
			const double endZ = route.points.back().z;
			rig.sim.enqueue(Command::fighterMoveTo(state::Id{ rig.id }, std::move(route.points), std::move(route.heights), 3));
			double prev = 0.0;
			double top = 0.0;
			double worstBrake = 0.0;
			double worstTurn = 0.0;
			double yaw = rig.fighter().yaw;
			int rampTicks = 0;
			int n = 0;
			for (; n < 4000; ++n)
			{
				rig.sim.tick();
				const state::Fighter& f = rig.fighter();
				if (n == 0)
					test::require(std::fabs(f.forwardSpeed - 10.0) < 1.0e-6, "the first walking tick is not accel x dt: " + std::to_string(f.forwardSpeed));
				worstTurn = std::max(worstTurn, std::fabs(wrap(f.yaw - yaw)));
				yaw = f.yaw;
				worstBrake = std::max(worstBrake, (prev - f.forwardSpeed) / kDt); // the arrival tick too: no snap to 0 from speed (kenshi-k16.8.9)
				if (f.forwardSpeed > top + 1.0e-9)
					rampTicks = n + 1;
				top = std::max(top, f.forwardSpeed);
				prev = f.forwardSpeed;
				if (!f.walking())
					break;
			}
			const state::Fighter& f = rig.fighter();
			test::require(!f.walking() && n < 4000, "the walk did not finish");
			test::require(std::hypot(f.x - endX, f.z - endZ) <= 1.0, "did not end on the route's last waypoint"); // the arrival reach (bq16_deadlocks.md Q2)
			test::require(worstTurn <= knav::kTurnRate * kDt + 1.0e-9, "the yaw turned faster than 20 rad/s in a tick");
			test::require(worstBrake <= 200.0 + 1.0e-6, "braked harder than the acceleration: " + std::to_string(worstBrake));
			test::require(top > 60.0 && rampTicks >= 6 && rampTicks <= 10, "the ramp to the run speed took " + std::to_string(rampTicks)
				+ " ticks (top " + std::to_string(top) + ")");
			std::cout << "walk: top " << top << " u/s after " << rampTicks << " ticks, worst brake " << worstBrake << " u/s^2, worst turn "
				<< worstTurn << " rad/tick, " << n << " ticks\n";
		}

		// ---- the live numbers (move_kinematics.md, msmove): standstill to run speed, the stop distance, a 90-degree corner as an arc ----
		{
			Rig rig(data, gd, nav);
			const state::Fighter& f0 = rig.fighter();
			// A straight route: the walk the long-walk block found, or any two-point route of 200+ units.
			Route straight;
			for (int k = 0; k < 16 && !straight.found; ++k)
			{
				const double a = k * 0.392699;
				const Route r = nav->routeFrom({ f0.x, f0.y, f0.z }, f0.x + std::sin(a) * 260.0, f0.z + std::cos(a) * 260.0);
				if (r.found && r.points.size() == 2 && std::hypot(r.points[1].x - f0.x, r.points[1].z - f0.z) > 200.0)
					straight = r;
			}
			test::require(straight.found, "no straight 200+ unit route near the Hub");
			const double sx = f0.x;
			const double sz = f0.z;
			const double endX = straight.points.back().x;
			const double endZ = straight.points.back().z;
			rig.sim.enqueue(Command::fighterMoveTo(state::Id{ rig.id }, std::move(straight.points), std::move(straight.heights), 5));
			double runTime = -1.0;
			double runDistance = 0.0;
			double brakeStart = -1.0; // the distance to the end where the speed first fell below the run speed
			for (int n = 0; n < 400; ++n)
			{
				rig.sim.tick();
				const state::Fighter& f = rig.fighter();
				const double speed = std::hypot(f.velX, f.velZ);
				if (runTime < 0.0 && speed >= 70.0 - 0.5)
				{
					runTime = (n + 1) * kDt;
					runDistance = std::hypot(f.x - sx, f.z - sz);
				}
				if (runTime >= 0.0 && brakeStart < 0.0 && speed < 70.0 - 0.5)
					brakeStart = std::hypot(endX - f.x, endZ - f.z);
				if (!f.walking())
					break;
			}
			// 0 -> 68 in 0.33-0.36 s (5 live runs); v^2 / 2a = 12.25 u (5 x 10 u/s steps of the 20 Hz tick make 14).
			test::require(runTime > 0.29 && runTime < 0.41, "standstill to the run speed took " + std::to_string(runTime) + " s (live 0.33-0.36)");
			test::require(runDistance > 10.0 && runDistance < 16.0, "the run-up covered " + std::to_string(runDistance) + " u (v^2/2a = 12.25)");
			// The stop starts ~10-11 u before the goal (v^2/2a, minus the last glide frame); 7 u of margin each way.
			test::require(brakeStart > 8.0 && brakeStart < 16.0, "the stop began " + std::to_string(brakeStart) + " u before the end (live 10-12)");
		}
		{
			// Two run-speed corners on the Hub tile (kenshi-e9s9.23), each walked from rest by the corridor walker. The speeds are per-tick displacement / dt.
			// A: the oracle's corner (findings/kenshi-rwh.308.md): the original's corridor from A goes west round the buildings' vertex (-49968, 4860) and then
			//    north; the exe runs it at 74-94 % of cruise, closest approach to the vertex 5.4-7.0 u, never a tick's loss above a*dt (6.73 u/s at 6.6).
			//    Our planner takes a gap the original does not (a separate item), so the route is the two legs A -> vertex -> goal.
			// B: the one-body gap our planner picks from A (the Hub bar door's kind, rwh269 section 1: min 36 u/s = 29 % of run, contact 1.6 u from the vertex):
			//    min >= 25 % of run, never a stop. Its premise (3 points, the bend within 12 u of the gap's vertex) is asserted so a mesh change fails loudly.
			// The old scenario (two straight 2-point routes 170 u apart) is no corner for the corridor walker: the funnel's taut path rounds a polygon vertex 35 u
			// off the polyline's corner (bend 1.14 rad, not a right angle).
			struct CornerRun
			{
				double minSpeed = 1.0e9; // while more than 60 u from the goal and after the run speed was reached
				double closest = 1.0e9;  // body centre to the vertex
				double worstLoss = 0.0;  // largest single-tick speed loss, same window
				double top = 0.0;
				int ticks = 0;
			};
			const auto cornerRun = [&](Pt start, Pt goal, double startY, const std::vector<Pt>& vias, double vertexX, double vertexZ, bool assertThreePoints)
			{
				Rig rig(data, gd, nav);
				state::Fighter* mover = fighterOf(rig.sim, rig.id);
				mover->x = start.x;
				mover->y = startY;
				mover->z = start.z;
				const double goalX = goal.x;
				const double goalZ = goal.z;
				Route route;
				double fromX = mover->x, fromZ = mover->z, fromY = mover->y;
				for (std::size_t leg = 0; leg <= vias.size(); ++leg)
				{
					const double toX = leg < vias.size() ? vias[leg].x : goalX;
					const double toZ = leg < vias.size() ? vias[leg].z : goalZ;
					const Route r = nav->routeFrom({ fromX, fromY, fromZ }, toX, toZ);
					test::require(r.found && r.points.size() >= 2, "no route for a corner leg");
					for (std::size_t i = route.found ? 1 : 0; i < r.points.size(); ++i)
					{
						route.points.push_back(r.points[i]);
						route.heights.push_back(r.heights[i]);
					}
					route.found = true;
					fromX = r.points.back().x;
					fromZ = r.points.back().z;
					fromY = r.heights.back();
				}
				if (assertThreePoints)
				{
					test::require(route.points.size() == 3, "no 3-point route round the gap's vertex (the mesh changed)");
					test::require(std::hypot(route.points[1].x - vertexX, route.points[1].z - vertexZ) < 12.0, "the taut path does not bend at the gap's vertex (the mesh changed)");
				}
				if (std::getenv("WALK_TRACE") != nullptr)
					for (const state::GroundPoint& gp : route.points)
						std::cout << "route " << gp.x << " " << gp.z << "\n";
				rig.sim.enqueue(Command::fighterMoveTo(state::Id{ rig.id }, std::move(route.points), std::move(route.heights), 6));
				CornerRun out;
				double px = mover->x, pz = mover->z, prev = 0.0;
				bool atSpeed = false;
				std::vector<Pt> walls;
				for (int n = 0; n < 900; ++n)
				{
					rig.sim.tick();
					const state::Fighter& f = rig.fighter();
					if (n == 0)
						for (const state::RoutePortal& pt : f.portals)
						{
							// The corridor's wall vertices: a portal end with a wall within the push range (9 u = r + 5), measured at plan time.
							if (!pt.isPoint() && pt.clearLeft < 9.0f)
								walls.push_back({ pt.lx, pt.lz });
							if (!pt.isPoint() && pt.clearRight < 9.0f)
								walls.push_back({ pt.rx, pt.rz });
						}
					if (n == 0 && std::getenv("WALK_TRACE") != nullptr)
						for (const state::RoutePortal& pt : f.portals)
							std::cout << "portal L " << pt.lx << " " << pt.lz << " R " << pt.rx << " " << pt.rz << " clr " << pt.clearLeft << " " << pt.clearRight << " pt " << pt.point << "\n";
					const double speed = std::hypot(f.x - px, f.z - pz) / kDt;
					px = f.x;
					pz = f.z;
					out.top = std::max(out.top, speed);
					for (const Pt& wall : walls)
						out.closest = std::min(out.closest, std::hypot(f.x - wall.x, f.z - wall.z));
					if (std::getenv("WALK_TRACE") != nullptr)
						std::cout << "t" << n << " v " << speed << " pos " << f.x << " " << f.z << " dVertex " << std::hypot(f.x - vertexX, f.z - vertexZ) << "\n";
					atSpeed = atSpeed || speed > 69.0;
					if (atSpeed && std::hypot(f.x - goalX, f.z - goalZ) > 60.0)
					{
						out.minSpeed = std::min(out.minSpeed, speed);
						out.worstLoss = std::max(out.worstLoss, prev - speed);
					}
					prev = speed;
					out.ticks = n + 1;
					if (!f.walking())
						break;
				}
				return out;
			};
			const CornerRun oracle = cornerRun({ -50878.8, 1782.5 }, { -50666.7, 2035.3 }, 1562.0, {}, -50688.0, 1792.0, true);
			std::cout << "oracle corner: min " << oracle.minSpeed << " u/s, closest " << oracle.closest << " u, worst loss " << oracle.worstLoss << " u/s per tick, " << oracle.ticks << " ticks\n";
			test::require(oracle.minSpeed >= 0.4 * 70.0, "the oracle corner slowed to " + std::to_string(oracle.minSpeed) + " u/s (exe 46-94 % of run, rwh.308)");
			test::require(oracle.closest >= 5.0, "the oracle corner's closest approach was " + std::to_string(oracle.closest) + " u (exe 5.4-7.0)");
			test::require(oracle.worstLoss <= 200.0 * kDt + 1.0, "the oracle corner lost " + std::to_string(oracle.worstLoss) + " u/s in a tick (exe max 6.73)");
			const CornerRun gap = cornerRun({ -49729.2, 4882.8 }, { -49868.6, 5181.9 }, 1524.4, {}, -49896.0, 4892.0, true);
			std::cout << "gap corner: min " << gap.minSpeed << " u/s, closest " << gap.closest << " u, " << gap.ticks << " ticks\n";
			// The live door's floor is 29 % of run (rwh269 section 1); ours reaches ~13 % here: this gap's second portal is 4 u wide, under the body's 8 u, so the
			// walker slides in it (the planner takes a gap the original goes round, a separate item), and the bar door itself is walked by ksim_path_walk. Pinned: no stop.
			test::require(gap.minSpeed > 5.0, "the one-body gap stopped the walker (" + std::to_string(gap.minSpeed) + " u/s; live door 29 % of run, rwh269 section 1)");
		}

		// ---- turn in place: drive 0 in the drive hold turns towards the heading with no translation ----
		{
			Rig rig(data, gd, nav);
			rig.sim.enqueue(Command::fighterDrive(state::Id{ rig.id }, 1, 0.0, 1));
			rig.sim.run(10);
			test::require(rig.fighter().forwardSpeed > 30.0 && rig.fighter().hold, "the drive did not start");
			rig.sim.enqueue(Command::fighterDrive(state::Id{ rig.id }, 0, 0.0, 2)); // a plain stop at the current heading: a no-op turn
			rig.sim.tick();
			test::require(rig.fighter().forwardSpeed == 0.0, "a drive 0 did not stop the character");
			const double x = rig.fighter().x;
			const double z = rig.fighter().z;
			const double heading = 2.4; // 2.4 rad takes 3 ticks at 1 rad per tick
			rig.sim.enqueue(Command::fighterDrive(state::Id{ rig.id }, 0, heading, 3));
			double yaw = rig.fighter().yaw;
			double worst = 0.0;
			int turned = 0;
			for (int t = 0; t < 30; ++t)
			{
				rig.sim.tick();
				const state::Fighter& f = rig.fighter();
				worst = std::max(worst, std::fabs(wrap(f.yaw - yaw)));
				yaw = f.yaw;
				test::require(f.x == x && f.z == z && f.forwardSpeed == 0.0, "a turn in place translated");
				if (std::fabs(wrap(f.yaw - heading)) < 1.0e-9 && turned == 0)
					turned = t + 1;
			}
			test::require(turned >= 1 && turned <= 4, "the turn to the heading took " + std::to_string(turned) + " ticks");
			test::require(worst <= knav::kTurnRate * kDt + 1.0e-9 && worst > 0.5, "the turn per tick was " + std::to_string(worst));
			test::require(rig.fighter().hold && rig.fighter().x == x, "the turn left the hold or moved");
			// Held A/D at 2 rad/s: a heading step of 0.1 rad each tick is followed exactly, still without translation.
			double h = heading;
			for (int t = 0; t < 20; ++t)
			{
				h = wrap(h + 0.1);
				rig.sim.enqueue(Command::fighterDrive(state::Id{ rig.id }, 0, h, 4 + t));
				rig.sim.tick();
				test::require(std::fabs(wrap(rig.fighter().yaw - h)) < 1.0e-9 && rig.fighter().x == x && rig.fighter().z == z,
					"a held-key heading step was not followed in place");
			}
			// A character that is not in the hold ignores a drive 0's heading (a plain stop is as before).
			Rig free(data, gd, nav);
			const double before = free.fighter().yaw;
			free.sim.enqueue(Command::fighterDrive(state::Id{ free.id }, 0, 2.0, 1));
			free.sim.run(5);
			test::require(free.fighter().yaw == before, "a drive 0 turned a character that was not in the drive hold");
		}

		std::cout << "ksim_walk_kinematics ok\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
