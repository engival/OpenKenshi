// Movement on the navmesh inside the simulation (docs/formats/SIMLINK.md section 12): a move order
// follows its route to the end and stays on the mesh, y comes from the surface, no navmesh means
// no move, an off-mesh character is snapped back with a counted event, direct drive slides and
// reports blocked, and the whole thing is deterministic across a save and load.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;


struct Counts
{
	int corrected = 0;
	int blocked = 0;
	double lastDistance = 0.0;
};

struct Rig
{
	Simulation sim;
	Counts counts;
	std::uint64_t id = 0;

	Rig(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd,
		const std::shared_ptr<MeshNavigator>& nav)
		: sim(Simulation::load(KENSHI_DATA, 77))
	{
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		if (nav != nullptr)
			sim.enableNavigation(nav);
		hook();
		std::uint64_t rng = 12345;
		std::string sid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
		f.team = 0;
		f.playerControlled = true;
		f.x = kHubX;
		f.z = kHubZ;
		if (nav != nullptr)
		{
			const auto spot = nav->walker().snap({kHubX, 1535.0, kHubZ}, 200.0);
			test::require(spot.has_value(), "no mesh near the Hub");
			f.x = spot->x;
			f.y = spot->y;
			f.z = spot->z;
		}
		id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	void hook()
	{
		sim.setCombatObserver([this](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::PositionCorrected)
			{
				++counts.corrected;
				counts.lastDistance = e.a;
			} else if (e.kind == CombatEvent::Kind::DriveBlocked) {
				++counts.blocked;
			}
		});
	}

	// A held enemy of team 1 standing at the mesh point nearest (x, z).
	std::uint64_t spawnEnemy(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd,
		const std::shared_ptr<MeshNavigator>& nav, double x, double z)
	{
		std::uint64_t rng = 999;
		std::string sid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
		state::Fighter e = data->makeFighter(gd, sid, "", rng);
		e.ranged = state::FighterRanged{}; // disarmed: a template's `crossbows` chance (k16.6.24) is not what this test pins
		e.team = 1;
		e.hold = true;
		const auto spot = nav->walker().snap({x, fighter().y, z}, 200.0);
		test::require(spot.has_value(), "no mesh near the enemy spot");
		e.x = spot->x;
		e.y = spot->y;
		e.z = spot->z;
		return sim.spawnFighter(std::move(e)).get<state::Id>()->value;
	}

	const state::Fighter& fighter() const { return *sim.combat()->find(id); }
	bool onMesh() const
	{
		const state::Fighter& f = fighter();
		return sim.navigation()->walker().onMesh({f.x, f.y, f.z});
	}
};

// A destination some way off that the mesh can route to.
Route routeSomewhere(const Rig& rig, double& tx, double& tz)
{
	const state::Fighter& f = rig.fighter();
	for (const double d : {160.0, 220.0, 120.0, 300.0, 90.0})
	{
		for (int k = 0; k < 8; ++k)
		{
			const double a = k * 0.785398;
			tx = f.x + std::sin(a) * d;
			tz = f.z + std::cos(a) * d;
			Route route = rig.sim.navigation()->routeFrom({f.x, f.y, f.z}, tx, tz);
			if (route.found && route.points.size() >= 2)
				return route;
		}
	}
	throw test::Failure("no routable destination near the Hub");
}

// nearestWalkable is given no height: wherever the Hub's own ground is on the mesh within reach, the snap must
// find it. A box that spans every height must not pick the polygon nearest in 3D from y = 0 instead (a lower
// slope under another part of the box; its closest point can fall outside the box and the snap returns nothing).
void nearestWalkableOverGround(const MeshNavigator& nav)
{
	const auto& config = nav.walker().mesh().config();
	const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
	const double x0 = config.originX + config.window.x0 * config.tileSize;
	const double z0 = config.originZ + config.window.z0 * config.tileSize;
	const double x1 = config.originX + (config.window.x1 + 1) * config.tileSize;
	const double z1 = config.originZ + (config.window.z1 + 1) * config.tileSize;
	constexpr double kStep = 128.0;
	constexpr double kOffset = kStep / 2.0; // not on a tile edge, where a point sits on two tiles' polygons
	int total = 0;
	for (const double reach : { MeshNavigator::kEndReach, 200.0 })
	{
		int sampled = 0;
		int missed = 0;
		for (double z = z0 + kOffset; z < z1; z += kStep)
		{
			for (double x = x0 + kOffset; x < x1; x += kStep)
			{
				const knav::Position ground{ x, terrain.height(x, z), z };
				const auto snapped = nav.walker().snap(ground, reach);
				// No mesh at the ground height here (inside a solid, in water, off the bake), or only at the
				// corner of the snap's square box, beyond the radius: either way nothing is within reach.
				if (!snapped || std::hypot(snapped->x - x, snapped->z - z) > reach)
					continue;
				++sampled;
				const auto at = nav.nearestWalkable(x, z, reach);
				if (at && std::hypot(at->x - x, at->z - z) <= reach)
					continue;
				if (missed++ == 0)
					std::cout << "  miss at " << x << " " << z << ", ground y " << ground.y << ", reach " << reach
						<< (at ? ", returned " + std::to_string(at->x) + " " + std::to_string(at->z) : std::string(", nothing"))
						<< "\n";
			}
		}
		test::require(sampled > 100, "too few Hub ground points to test the height-free snap");
		test::require(missed == 0, "nearestWalkable missed walkable ground under the point at reach "
			+ std::to_string(reach) + " (" + std::to_string(missed) + " of " + std::to_string(sampled) + ")");
		total += sampled;
	}
	std::cout << "height-free snap over the Hub ground: " << total << " points\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_move HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- a snap without a height finds the ground under the point ----
		nearestWalkableOverGround(*nav);

		// ---- a move order follows its route ----
		Rig rig(data, gd, nav);
		double tx = 0;
		double tz = 0;
		Route route = routeSomewhere(rig, tx, tz);
		const auto routePoints = route.points;
		rig.sim.enqueue(Command::fighterMoveTo(state::Id{rig.id}, std::move(route.points), std::move(route.heights), 7));
		rig.sim.tick();
		test::require(rig.fighter().routeVersion == 1 && rig.fighter().routeSeq == 7, "route version and seq after the order");
		test::require(rig.fighter().walking() || rig.fighter().route.empty(), "route state");
		test::require(rig.fighter().route.size() == routePoints.size() && rig.fighter().route[0].y > 1.0, "route waypoints carry y");
		int ticks = 1;
		while (rig.fighter().walking() && ticks < 4000)
		{
			rig.sim.tick();
			++ticks;
			test::require(rig.onMesh(), "left the mesh while walking, tick " + std::to_string(ticks));
		}
		const state::Fighter& walked = rig.fighter();
		test::require(!walked.walking() && walked.route.empty(), "the walk did not finish");
		test::require(walked.routeVersion == 2, "the finish bumps routeVersion (the wire's clear)");
		test::require(std::hypot(walked.x - routePoints.back().x, walked.z - routePoints.back().z) <= 1.0,
			"did not end on the route's last waypoint"); // the arrival reach (bq16_deadlocks.md Q2)
		test::require(walked.y > 1.0, "y set");
		test::require(rig.counts.corrected == 0, "a walk needed a position correction");
		std::cout << "walked " << std::hypot(walked.x - kHubX, walked.z - kHubZ) << " units in " << ticks << " ticks\n";

		// ---- walk as close as you can: an off-mesh click snaps, and the route is flagged as an approach ----
		{
			Rig rig2(data, gd, nav);
			const state::Fighter& f = rig2.fighter();
			test::require(!routeSomewhere(rig2, tx, tz).approach, "a click on the mesh is not an approach");
			bool found = false;
			double offX = 0.0;
			double offZ = 0.0;
			for (double d = 20.0; d < 140.0 && !found; d += 4.0)
			{
				for (int k = 0; k < 32 && !found; ++k)
				{
					const double a = k * 0.19635;
					offX = f.x + std::sin(a) * d;
					offZ = f.z + std::cos(a) * d;
					if (!nav->walker().onMesh({offX, f.y, offZ}))
					{
						const Route r = nav->routeFrom({f.x, f.y, f.z}, offX, offZ);
						found = r.found && r.approach && r.points.size() >= 2;
					}
				}
			}
			test::require(found, "no off-mesh click near the Hub routed as an approach");
			Route approach = nav->routeFrom({f.x, f.y, f.z}, offX, offZ);
			test::require(std::hypot(approach.points.back().x - offX, approach.points.back().z - offZ) <= MeshNavigator::kClickReach,
				"the approach point is beyond the click reach");
			rig2.sim.enqueue(Command::fighterMoveTo(state::Id{rig2.id}, std::move(approach.points), std::move(approach.heights), 9, false, true));
			rig2.sim.tick();
			test::require(rig2.fighter().routeApproach, "the approach flag did not reach the fighter");
			rig2.sim.run(2000);
			test::require(!rig2.fighter().walking() && !rig2.fighter().routeApproach && rig2.onMesh(), "the approach walk did not finish on the mesh");
			test::require(rig2.counts.corrected == 0, "an approach walk needed a position correction");
			// Nothing within the reach: no route.
			const Route far = nav->routeFrom({f.x, f.y, f.z}, kworld::kWorldExtent - 1.0, kworld::kWorldExtent - 1.0);
			test::require(!far.found && !far.approach, "a click far from any mesh found a route");
			// A click height snaps in 3D (MOVE_CLICK_LIVE.md): 100 above a walkable point still finds it, 400 above does not.
			// (The test point is 110 units from Storm House 3384, whose roof island is 120 above the ground and
			// inside the 340 reach of a click 400 up since the tilted-hull fix gave the house its roof slabs, so
			// the "does not" end is checked at 1000 above, clear of every roof.)
			Route near = routeSomewhere(rig2, tx, tz);
			const double groundY = near.heights.back();
			test::require(nav->routeFrom({f.x, f.y, f.z}, tx, tz, groundY + 100.0).found, "a click 100 above the mesh found no route");
			test::require(!nav->routeFrom({f.x, f.y, f.z}, tx, tz, groundY + 1000.0).found, "a click 1000 above the mesh found a route");
		}

		// ---- a long attack chase follows a navmesh route, re-planned when the target moves ----
		{
			Rig chase(data, gd, nav);
			double ex = 0.0;
			double ez = 0.0;
			const Route far = routeSomewhere(chase, ex, ez);
			const std::uint64_t enemy = chase.spawnEnemy(data, gd, nav, far.points.back().x, far.points.back().z);
			const auto enemyAt = [&]() -> state::Fighter& { return const_cast<state::Fighter&>(*chase.sim.combat()->find(enemy)); };
			const auto distance = [&]() { return std::hypot(chase.fighter().x - enemyAt().x, chase.fighter().z - enemyAt().z); };
			const double start = distance();
			test::require(start > 100.0, "the chase starts too close to test the route");
			chase.sim.enqueue(Command::fighterAttack(state::Id{chase.id}, state::Id{enemy}));
			chase.sim.run(10);
			const state::Fighter& me = chase.fighter();
			test::require(me.routeChase && me.routeApproach && me.routeSeq == 0 && me.route.size() >= 2 && me.routeVersion >= 1,
				"a far attack target did not get a chase route");
			test::require(!me.walking(), "a chase route is not a move order's walk");
			const std::uint32_t planned = me.routeVersion;
			chase.sim.run(30);
			// Move the target far from where the route was planned: re-planned.
			const auto away = nav->walker().snap({enemyAt().x + 60.0, enemyAt().y, enemyAt().z + 60.0}, 100.0);
			test::require(away.has_value(), "no mesh near the moved enemy");
			enemyAt().x = away->x;
			enemyAt().y = away->y;
			enemyAt().z = away->z;
			chase.sim.run(30);
			test::require(chase.fighter().routeVersion > planned, "the chase route was not re-planned after the target moved");
			for (int t = 0; t < 4000 && distance() > 60.0; ++t)
			{
				chase.sim.tick();
				test::require(chase.onMesh(), "a chasing character left the mesh");
			}
			test::require(distance() <= 60.0, "the chaser never closed in (" + std::to_string(distance()) + " from " + std::to_string(start) + ")");
			test::require(chase.counts.corrected == 0, "a chase needed a position correction");
			std::cout << "chase closed " << start << " to " << distance() << ", route version " << chase.fighter().routeVersion << "\n";
		}

		// ---- a blocked walk re-routes (bounded), it is not simply abandoned ----
		{
			Rig blocked(data, gd, nav);
			state::Fighter* f = const_cast<state::Fighter*>(blocked.sim.combat()->find(blocked.id));
			knav::Position hole{};
			bool placed = false;
			for (double d = 12.0; d < 140.0 && !placed; d += 2.0)
			{
				for (int k = 0; k < 16 && !placed; ++k)
				{
					const double a = k * 0.392699;
					hole = {f->x + std::sin(a) * d, f->y, f->z + std::cos(a) * d};
					placed = !nav->walker().onMesh(hole);
				}
			}
			test::require(placed, "no off-mesh point near the Hub spawn");
			// A hand-made route whose second waypoint is off the mesh: the walk is blocked there.
			f->route = {{f->x, f->y, f->z}, {hole.x, f->y, hole.z}};
			f->routeNext = 0;
			f->routeVersion = 1;
			blocked.sim.run(200);
			test::require(blocked.sim.combat()->reroutes() >= 1, "a blocked walk did not re-route");
			test::require(!blocked.fighter().walking() && blocked.onMesh(), "the re-routed walk did not finish on the mesh");
			test::require(blocked.counts.corrected == 0, "a re-routed walk needed a position correction");
		}

		// ---- a route with a steep leg is walked, not dropped (heights travel with the route) ----
		{
			Rig steep(data, gd, nav);
			const state::Fighter& f = steep.fighter();
			Route found;
			double legRise = 0.0;
			double legRun = 0.0;
			for (double d = 200.0; d <= 1400.0 && found.points.empty(); d += 100.0)
			{
				for (int k = 0; k < 16 && found.points.empty(); ++k)
				{
					const double a = k * 0.392699;
					Route route = nav->routeFrom({f.x, f.y, f.z}, f.x + std::sin(a) * d, f.z + std::cos(a) * d);
					for (std::size_t i = 1; route.found && i < route.heights.size(); ++i)
					{
						const double rise = std::fabs(route.heights[i] - route.heights[i - 1]);
						if (rise > 40.0)
						{
							legRise = rise;
							legRun = std::hypot(route.points[i].x - route.points[i - 1].x, route.points[i].z - route.points[i - 1].z);
							found = route;
							break;
						}
					}
				}
			}
			test::require(!found.points.empty(), "no Hub route with a leg steeper than 40 units to test");
			std::cout << "steep route: " << found.points.size() << " points, a leg rises " << legRise << " over " << legRun << " units (slope " << legRise / legRun << ")\n";
			// A real cliff would not be on the mesh (agentMaxSlope 40 degrees = 0.84); a steep leg is a long straight slope.
			test::require(legRise / legRun < 1.0, "the steep leg is a cliff, not a slope: bad route heights");
			steep.sim.enqueue(Command::fighterMoveTo(state::Id{steep.id}, found.points, found.heights, 11));
			steep.sim.tick();
			test::require(steep.fighter().routeVersion == 1 && steep.fighter().route.size() == found.points.size(),
				"a route with a steep leg was dropped");
			test::require(steep.fighter().route[0].y == found.heights[0], "the route heights are not the server's");
			test::require(steep.sim.badOrders() == 0, "a good order was counted as bad");
			// A caller that sends points without heights is counted, never walked.
			steep.sim.enqueue(Command::fighterMoveTo(state::Id{steep.id}, found.points, {}, 12));
			steep.sim.tick();
			test::require(steep.sim.badOrders() == 1 && steep.fighter().routeSeq == 11, "an order without heights was not refused");
		}

		// ---- the run toggle: walking is capped at the race's walk speed, running is not ----
		{
			double topSpeed[2] = {0.0, 0.0};
			double walkSpeed = 0.0;
			for (int run = 0; run < 2; ++run)
			{
				Rig gait(data, gd, nav);
				double tx = 0;
				double tz = 0;
				Route route = routeSomewhere(gait, tx, tz);
				walkSpeed = data->race(gait.fighter().raceSid)->walkSpeed;
				gait.sim.enqueue(Command::fighterRun(state::Id{gait.id}, run != 0));
				gait.sim.enqueue(Command::fighterMoveTo(state::Id{gait.id}, std::move(route.points), std::move(route.heights), 21));
				// 30 ticks: the run reaches its top speed in 7 (200 u/s^2), and the route (160 units) is still unfinished, so a
				// finished walk's routeVersion bump does not read as the toggle replacing the order.
				for (int t = 0; t < 30; ++t)
				{
					gait.sim.tick();
					topSpeed[run] = std::max(topSpeed[run], std::fabs(gait.fighter().forwardSpeed));
				}
				test::require(gait.fighter().moveMode == (run != 0 ? 2 : 0), "the run toggle did not reach the fighter");
				test::require(gait.fighter().routeVersion == 1, "the run toggle replaced the move order");
			}
			std::cout << "gait: walk top speed " << topSpeed[0] << " (race walk speed " << walkSpeed << "), run " << topSpeed[1] << "\n";
			test::require(topSpeed[0] > 1.0 && topSpeed[0] <= walkSpeed + 1e-6, "walking exceeded the walk speed");
			test::require(topSpeed[1] > walkSpeed + 1.0, "running is not faster than walking");
		}

		// ---- no navmesh, no move ----
		{
			Rig bare(data, gd, nullptr);
			bare.sim.enqueue(Command::fighterMoveTo(state::Id{bare.id}, {{kHubX, kHubZ}, {kHubX + 50, kHubZ}}, {0.0, 0.0}, 1));
			bare.sim.enqueue(Command::fighterDrive(state::Id{bare.id}, 1, 0.0, 2));
			bare.sim.run(5);
			test::require(!bare.fighter().walking() && bare.fighter().route.empty() && bare.fighter().routeVersion == 0,
				"a move was accepted without a navmesh");
			test::require(bare.fighter().drive == state::DriveMode::Stop, "a drive was accepted without a navmesh");
			test::require(bare.fighter().x == kHubX, "the character moved without a navmesh");
		}

		// ---- an off-mesh character is snapped back, once, and counted ----
		{
			Rig lost(data, gd, nav);
			state::Fighter* f = lost.sim.combat() != nullptr ? const_cast<state::Fighter*>(lost.sim.combat()->find(lost.id)) : nullptr;
			bool placed = false;
			double expected = 0.0;
			for (double d = 3.0; d < 120.0 && !placed; d += 1.5)
			{
				for (int k = 0; k < 16 && !placed; ++k)
				{
					const double a = k * 0.392699;
					const knav::Position probe{f->x + std::sin(a) * d, f->y, f->z + std::cos(a) * d};
					if (!nav->walker().onMesh(probe) && nav->walker().snap(probe, 256.0, &expected))
					{
						f->x = probe.x;
						f->z = probe.z;
						placed = true;
					}
				}
			}
			test::require(placed, "no off-mesh point found near the Hub spawn");
			lost.sim.tick();
			test::require(lost.counts.corrected == 1, "expected exactly one position correction, got " + std::to_string(lost.counts.corrected));
			test::require(lost.sim.combat()->positionCorrections() == 1 && lost.sim.combat()->uncorrectable() == 0, "correction counters");
			test::require(std::fabs(lost.counts.lastDistance - expected) < 0.05, "event distance is the snap distance");
			test::require(lost.onMesh(), "still off the mesh after the correction");
			lost.sim.run(20);
			test::require(lost.counts.corrected == 1, "corrected again though it was on the mesh");
		}

		// ---- direct drive: slides, never leaves the mesh, reports blocked at a wall ----
		{
			Rig drv(data, gd, nav);
			drv.sim.enqueue(Command::fighterDrive(state::Id{drv.id}, 1, 0.0, 11));
			drv.sim.tick();
			test::require(drv.fighter().drive == state::DriveMode::Forward && drv.fighter().driveSeq == 11 && drv.fighter().hold,
				"drive state and the AI-off hold");
			bool blocked = false;
			for (int heading = 0; heading < 16 && !blocked; ++heading)
			{
				drv.sim.enqueue(Command::fighterDrive(state::Id{drv.id}, 1, heading * 0.392699, 100 + static_cast<std::uint32_t>(heading)));
				for (int t = 0; t < 400 && drv.counts.blocked == 0; ++t)
				{
					drv.sim.tick();
					test::require(drv.onMesh(), "a driven character left the mesh");
				}
				blocked = drv.counts.blocked > 0;
			}
			test::require(blocked, "driving in 16 directions never met a wall");
			test::require(drv.counts.corrected == 0, "driving needed a position correction");
			const double x = drv.fighter().x;
			drv.sim.enqueue(Command::fighterDrive(state::Id{drv.id}, 0, 0.0, 300));
			drv.sim.run(40);
			test::require(drv.fighter().drive == state::DriveMode::Stop && drv.fighter().driveSeq == 300, "drive stop");
			test::require(std::fabs(drv.fighter().forwardSpeed) < 1e-9, "still moving after a stop");
			(void) x;
		}

		// ---- direct drive strafe (k16.8.17): a sidestep keeps the facing, W+A is the gait, S+A 0.707 x; saved (v111) ----
		{
			// Each case from a fresh rig; the speed along the move direction is read at its steady state (the gait ramp has finished).
			const auto run = [&](int mode, int strafe, double heading, double& speed, double& along, double& side)
			{
				Rig s(data, gd, nav);
				const double x0 = s.fighter().x, z0 = s.fighter().z;
				s.sim.enqueue(Command::fighterDrive(state::Id{s.id}, mode, heading, 2, strafe));
				s.sim.run(30);
				test::require(s.counts.blocked == 0 && s.onMesh(), "the strafe test ran into a wall");
				test::require(s.fighter().driveStrafe == strafe, "driveStrafe not applied");
				if (mode != 2 || strafe != 0)
					test::require(std::fabs(std::remainder(s.fighter().yaw - heading, 6.283185307179586)) < 1e-6, "the facing is not the heading");
				const double dx = s.fighter().x - x0, dz = s.fighter().z - z0;
				speed = std::fabs(s.fighter().forwardSpeed);
				along = dx * std::sin(heading) + dz * std::cos(heading);
				side = dz * std::sin(heading) - dx * std::cos(heading); // + = the body's right (-x at heading 0, k16.8.38)
			};
			bool done = false;
			for (int k = 0; k < 16 && !done; ++k)
			{
				const double heading = k * 0.392699;
				try
				{
					double fwd = 0, a = 0, sd = 0, wa = 0, wa1 = 0, wa2 = 0, sa = 0, sa1 = 0, sa2 = 0, st = 0, st1 = 0, st2 = 0;
					run(1, 0, heading, fwd, a, sd);
					run(0, 1, heading, st, st1, st2);
					run(1, -1, heading, wa, wa1, wa2);
					run(2, -1, heading, sa, sa1, sa2);
					test::require(std::fabs(st1) < 1e-3 * std::max(st2, 1.0) && st2 > 1.0, "a strafe alone is not a sidestep to the right of the facing");
					test::require(std::fabs(st - 0.5 * fwd) < 0.01 * fwd, "a strafe alone is not half the gait");
					test::require(std::fabs(wa - fwd) < 0.01 * fwd, "W+A is not the gait");
					test::require(wa1 > 0.0 && wa2 < 0.0, "W+A is not forward-left");
					test::require(std::fabs(sa / fwd - 0.70710678) < 0.01, "S+A is not 0.707 x the gait");
					test::require(sa1 < 0.0 && sa2 < 0.0, "S+A is not back-left");
					done = true;
				} catch (const std::exception&) {
					if (k == 15)
						throw;
				}
			}
			// Save and load keep driveStrafe, and the load continues identically.
			Rig sv(data, gd, nav);
			sv.sim.enqueue(Command::fighterDrive(state::Id{sv.id}, 0, 0.0, 5, -1));
			sv.sim.run(10);
			test::require(sv.fighter().driveStrafe == -1, "driveStrafe -1 not applied");
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_move_strafe.save";
			sv.sim.saveGame(path);
			Simulation back = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path);
			CombatSettings settings;
			settings.substeps = 2;
			back.enableCombat(data, settings);
			back.enableNavigation(nav);
			test::require(back.combat()->find(sv.id)->driveStrafe == -1, "driveStrafe lost in the save");
			sv.sim.run(20);
			back.run(20);
			test::require(back.stateHash() == sv.sim.stateHash(), "a load mid-strafe does not continue the unbroken run");
			std::remove(path.c_str());
			std::cout << "strafe: sidestep, W+A, S+A and the save agree\n";
		}

		// ---- the character's own clearance radius: the street to the bar's floor 1 at radius 4 (NAVMESH_CACHE.md
		// "Clearance"). Every character route asks for its race's footprint radius (CombatData::pathRadius); the
		// merged Greenlander is 4.0, the exe's own. A route that refuses a gap the body does not fit still climbs.
		{
			Rig bar(data, gd, nav);
			const state::Fighter& f = bar.fighter();
			const double radius = bar.sim.pathRadius(f);
			test::require(std::fabs(radius - 4.0) < 1e-9, "the merged Greenlander's path radius is not 4.0");
			const knav::Position floor1{ -51169.0, 1647.0, 2665.0 };
			Route up = nav->routeFrom({ f.x, f.y, f.z }, floor1.x, floor1.z, floor1.y, false, radius);
			test::require(up.found && up.heights.back() > 1640.0, "no route to the bar's floor 1 at radius 4");
			bar.sim.enqueue(Command::fighterMoveTo(state::Id{ bar.id }, std::move(up.points), std::move(up.heights), 11));
			bar.sim.tick();
			int ticks = 1;
			while (bar.fighter().walking() && ticks < 8000)
			{
				bar.sim.tick();
				++ticks;
				test::require(bar.onMesh(), "left the mesh on the way to the bar's floor 1, tick " + std::to_string(ticks));
			}
			test::require(!bar.fighter().walking(), "the walk to the bar's floor 1 did not finish");
			test::require(bar.fighter().y > 1640.0, "the walk ended below the bar's floor 1 (y " + std::to_string(bar.fighter().y) + ")");
			std::cout << "street -> bar floor 1 at radius " << radius << ": " << ticks << " ticks, y " << bar.fighter().y << "\n";
		}

		// ---- determinism and save: run A == run B, and save mid-route + load + rest == uninterrupted ----
		const auto scripted = [&](Rig& r, int ticksToRun)
		{
			if (r.sim.clockTick() == 0 || r.fighter().routeVersion == 0)
			{
				double x = 0;
				double z = 0;
				Route rt = routeSomewhere(r, x, z);
				r.sim.enqueue(Command::fighterMoveTo(state::Id{r.id}, std::move(rt.points), std::move(rt.heights), 3));
			}
			r.sim.run(static_cast<std::uint64_t>(ticksToRun));
		};
		Rig a(data, gd, nav);
		scripted(a, 150);
		Rig b(data, gd, nav);
		scripted(b, 150);
		test::require(a.sim.stateHash() == b.sim.stateHash(), "two identical runs differ");

		Rig split(data, gd, nav);
		scripted(split, 40);
		test::require(split.fighter().walking(), "the save point is not mid-route");
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_move.save";
		split.sim.saveGame(path);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, order, path);
		CombatSettings settings;
		settings.substeps = 2;
		loaded.enableCombat(data, settings);
		loaded.enableNavigation(nav);
		loaded.run(110);
		test::require(loaded.stateHash() == a.sim.stateHash(), "save mid-route + load + run differs from the uninterrupted run");
		std::remove(path.c_str());

		std::cout << "ksim_move ok, hash " << std::hex << a.sim.stateHash() << std::dec << "\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
