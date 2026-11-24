// KSIM_PATH_WALK (kenshi-e9s9.5, docs/reference/PATH_WALK.md 3b-3e): the corridor walk on the Hub, the knob switched on by the test. Routes planned the way
// simlink move_to does (the click's straightened route, radius 4), walked by a player character. Prints and checks:
//  - the eight live street routes of rwh269 (Hub streets, start / goal as clicked there): no stall; the lowest speed once at cruise and more than 60 u from the goal is printed (live: corners 79-96 % of cruise; ours bottoms at the brake's sqrt(2a) floor at near-straight funnel corners, kenshi-e9s9.13);
//  - the bar door legs (rwh198 door, both ways): no stall away from the goal, the closest approach to the doorway's two leaf-end vertices, the speed at the door's corner;
//  - the walk-speed (15 u/s) miss distance at the goal; dt 0.2 and 0.4 (the fight/idle strides 4 and 8) on the same legs.
// [pin] = our own output. The live targets (rwh269) are printed beside. Needs the install (KENSHI_DATA); exit 77 without it.

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "knav/native.hpp"
#include "knav/walk.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDoorX = -50619.11; // the Hub bar door's plane and leaf ends (rwh198)
constexpr double kLeafLo = -1170.981;
constexpr double kLeafHi = -1141.101;
const double kJambX = -50622.0; // the doorway's north-west jamb vertex on the mesh (the corridor portals' end, 3 u beyond the door plane)
const double kJambZ = -1140.0;

struct Pt { double x, z; };

double segDist(Pt p, Pt a, Pt b)
{
	const double dx = b.x - a.x, dz = b.z - a.z;
	const double len2 = dx * dx + dz * dz;
	const double t = len2 > 0.0 ? std::clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / len2, 0.0, 1.0) : 0.0;
	return std::hypot(p.x - (a.x + t * dx), p.z - (a.z + t * dz));
}

struct Trace
{
	bool found = false;
	std::size_t points = 0;
	double peak = 0.0;
	double missAtGoal = 0.0; // where it ended, from the goal
	double seconds = 0.0;
	double minAway = 1.0e9; // the lowest speed while more than 4 u from the goal
	double cornerMin = 1.0e9; // the lowest speed within 10 u of a corner of 60 degrees or more
	double cornerTurn = 0.0; // that corner's turn, degrees
	double cleanMin = 1.0e9; // the same for a corner with 30 u before it and 40 u after it (cruise reached, no goal brake inside the window)
	double cleanTurn = 0.0;
	double cruiseMin = 1.0e9; // the lowest speed once at cruise, while more than 60 u from the goal (rwh269's measure)
	bool atCruise = false;
	double jamb = 1.0e9; // the closest approach to the jamb vertex
	double route = 0.0;
	double maxTurnTick = 0.0;
	Pt end{};
	Pt jambAt{};
	double jambTime = 0.0;
	double crossZ = 1.0e9; // where it crossed the door plane
};

std::vector<ksim::state::RoutePortal> g_portals; // the portals of the last walk's route (copied at tick 3)
std::vector<ksim::state::RoutePoint> g_route;

struct Env
{
	std::shared_ptr<ksim::MeshNavigator> nav;
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const ksim::CombatData> data;
	std::string sid;
};

Trace walk(const Env& env, Pt from, double fromY, Pt to, double toY, int moveMode, int stride)
{
	using namespace ksim;
	Trace t;
	g_portals.clear();
	const auto snapped = env.nav->walker().snap({ from.x, fromY, from.z }, 40.0, nullptr, fromY < -9000.0 ? 1.0e6 : -1.0); // (fromY < -9000: any floor)
	if (toY < -9000.0)
	{
		const auto g = env.nav->walker().snap({ to.x, 0.0, to.z }, 40.0, nullptr, 1.0e6);
		if (!g)
			return t;
		toY = g->y;
	}
	if (!snapped)
		return t;
	const Route route = env.nav->routeFrom(*snapped, to.x, to.z, toY, false, 4.0, true);
	if (!route.found || route.points.empty())
		return t;
	t.found = true;
	t.points = route.points.size();
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	CombatSettings settings;
	settings.substeps = 2;
	settings.fightStride = stride;
	settings.idleStride = stride;
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(env.data, settings);
	sim.enableNavigation(env.nav);
	std::uint64_t rng = 12345;
	state::Fighter f = env.data->makeFighter(*env.gd, env.sid, "", rng);
	f.playerControlled = true;
	f.moveMode = static_cast<std::uint8_t>(moveMode);
	f.x = snapped->x;
	f.y = snapped->y;
	f.z = snapped->z;
	const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	sim.enqueue(Command::fighterMoveTo(state::Id{id}, route.points, route.heights, 1));
	if (std::getenv("WALK_TRACE") != nullptr && stride == 1)
		for (std::size_t i = 0; i < route.points.size(); ++i)
			std::cout << "route " << route.points[i].x << " " << route.points[i].z << "\n";
	std::vector<Pt> poly = { { snapped->x, snapped->z } };
	std::vector<double> turn = { 0.0 };
	for (std::size_t i = 0; i < route.points.size(); ++i)
		if (std::hypot(route.points[i].x - poly.back().x, route.points[i].z - poly.back().z) > 1.0) // (a first point on the start is no corner)
			poly.push_back({ route.points[i].x, route.points[i].z });
	turn.resize(poly.size(), 0.0);
	for (std::size_t i = 1; i + 1 < poly.size(); ++i)
	{
		const double a = std::atan2(poly[i].x - poly[i - 1].x, poly[i].z - poly[i - 1].z);
		const double b = std::atan2(poly[i + 1].x - poly[i].x, poly[i + 1].z - poly[i].z);
		double d = std::fabs(b - a);
		d = std::min(d, 2.0 * kPi - d);
		turn[i] = d * 180.0 / kPi;
		t.route += std::hypot(poly[i].x - poly[i - 1].x, poly[i].z - poly[i - 1].z);
	}
	t.route += std::hypot(poly.back().x - poly[poly.size() - 2].x, poly.back().z - poly[poly.size() - 2].z);
	const Pt goal = poly.back();
	sim.run(2);
	Pt last = { snapped->x, snapped->z };
	double lastHeading = 0.0;
	const int maxTicks = 20 * 30;
	bool started = false;
	for (int tick = 0; tick < maxTicks; ++tick)
	{
		sim.run(1);
		const state::Fighter& me = *sim.combat()->find(id);
		if (g_portals.empty() && !me.portals.empty())
		{
			g_portals = me.portals;
			g_route = me.route;
		}
		const Pt at = { me.x, me.z };
		const double speed = me.forwardSpeed;
		if (std::getenv("WALK_TRACE") != nullptr && stride == 1)
			std::cout << "t" << tick << " v " << speed << " pos " << me.x << " " << me.z << "\n";
		t.peak = std::max(t.peak, speed);
		{
			const double j = segDist({ kJambX, kJambZ }, last, at);
			if (j < t.jamb)
			{
				t.jamb = j;
				t.jambAt = at;
				t.jambTime = (tick + 1) * 0.05;
			}
		}
		if ((last.x - kDoorX) * (at.x - kDoorX) < 0.0)
			t.crossZ = last.z + (at.z - last.z) * (kDoorX - last.x) / (at.x - last.x);
		const double toGoal = std::hypot(at.x - goal.x, at.z - goal.z);
		t.atCruise = t.atCruise || speed >= 0.99 * 70.0;
		if (t.atCruise && toGoal > 60.0 && me.walking())
			t.cruiseMin = std::min(t.cruiseMin, speed);
		if (me.walking() && toGoal > 4.0 && started)
			t.minAway = std::min(t.minAway, speed);
		for (std::size_t i = 1; i + 1 < poly.size(); ++i)
		{
			if (started && turn[i] >= 60.0 && std::hypot(poly[i].x - poly[i - 1].x, poly[i].z - poly[i - 1].z) >= 30.0 && std::hypot(poly[i + 1].x - poly[i].x, poly[i + 1].z - poly[i].z) >= 40.0
				&& std::hypot(at.x - poly[i].x, at.z - poly[i].z) < 10.0 && me.walking())
			{
				if (speed < t.cleanMin)
					t.cleanTurn = turn[i];
				t.cleanMin = std::min(t.cleanMin, speed);
			}
			if (started && turn[i] >= 60.0 && std::hypot(at.x - poly[i].x, at.z - poly[i].z) < 10.0 && me.walking())
			{
				if (speed < t.cornerMin)
					t.cornerTurn = turn[i];
				t.cornerMin = std::min(t.cornerMin, speed);
			}
		}
		if (speed > 1.0)
		{
			const double h = std::atan2(me.velX, me.velZ) * 180.0 / kPi;
			double d = std::fabs(h - lastHeading);
			d = d > 180.0 ? 360.0 - d : d;
			if (tick > 3)
				t.maxTurnTick = std::max(t.maxTurnTick, d);
			lastHeading = h;
		}
		last = at;
		t.seconds = (tick + 1) * 0.05;
		started = started || speed > 1.0;
		if (started && toGoal < 1.5 && speed < 1.0)
			break; // (a door in the way stashes the route and the walker opens it: not an end)
	}
	t.end = last;
	t.missAtGoal = std::hypot(last.x - goal.x, last.z - goal.z);
	return t;
}

void show(const char* what, const Trace& t)
{
	std::cout << "ksim_path_walk: " << what << ": " << t.points << " points, route " << t.route << " u, " << t.seconds << " s, peak " << t.peak
			  << " u/s, min away from goal " << t.minAway << ", corner min " << t.cornerMin << " (" << t.cornerTurn << " deg), jamb " << t.jamb
			  << " u [cruise min " << t.cruiseMin << ", clean corner min " << t.cleanMin << " (" << t.cleanTurn << " deg)] at (" << t.jambAt.x << ", " << t.jambAt.z << ") t " << t.jambTime << " s, door plane crossed at z " << t.crossZ << " (leaf " << kLeafLo << ".." << kLeafHi << "), ended " << t.missAtGoal << " (" << t.end.x << ", " << t.end.z << ")" << " u from the goal, worst tick turn " << t.maxTurnTick << " deg\n";
}

} // namespace

int main()
{
	::setenv("KSIM_PATH_WALK", "1", 0); // (KSIM_PATH_WALK=0 compares the knob off) before the first walkOrder: the knob is read once
	try
	{
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cerr << "ksim_path_walk: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> hub;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				hub.push_back({ ksim::simlink::kHubZoneX + dx, ksim::simlink::kHubZoneZ + dz });
		Env env;
		env.nav = ksim::MeshNavigator::openNative(KENSHI_DATA, hub);
		const kdata::GameData loaded = kworld::loadMergedGameData(KENSHI_DATA);
		env.gd = std::make_shared<const kdata::GameData>(loaded);
		env.data = ksim::CombatData::build(*env.gd, KENSHI_DATA);
		for (const kdata::Record* r : env.gd->byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				env.sid = r->stringID;
		const int kRun = 2;
		const int kWalk = 0;

		// The bar door, both ways (ksim_route_corners' Storm House legs: inside (-50612.7, -1158), outside (-50642.5, -1095.3)).
		for (const int stride : { 1, 4, 8 })
		{
			const Pt inside = { -50612.7, -1158.0 };
			const Pt outside = { -50642.5, -1095.3 };
			const Trace out = walk(env, inside, 1081.0, outside, 1089.0, kRun, stride);
			const Trace in = walk(env, outside, 1089.0, inside, 1081.0, kRun, stride);
			const std::string tag = stride == 1 ? "door" : "door dt " + std::to_string(stride * 0.05).substr(0, 4);
			show((tag + " out").c_str(), out);
			show((tag + " in").c_str(), in);
			for (const Trace* w : { &out, &in })
			{
				test::require(w->found, tag + ": no route");
				if (w == &out)
					test::require(w->minAway > 1.0, tag + ": the walker stopped away from the goal (min speed " + std::to_string(w->minAway) + ")");
				test::require(w->missAtGoal < 4.0, tag + ": it ended " + std::to_string(w->missAtGoal) + " u from the goal");
				test::require(w->jamb > 0.8, tag + ": the walker's centre passed within 0.8 u of the doorway vertex (" + std::to_string(w->jamb) + ")");
			}
		}

		// The funnel's cost: the door-out corridor, from the start, per call.
		{
			walk(env, { -50612.7, -1158.0 }, 1081.0, { -50642.5, -1095.3 }, 1089.0, kRun, 1);
			std::size_t points = 0, runs = 0, run = 0;
			for (const ksim::state::RoutePortal& p : g_portals)
			{
				points += p.isPoint();
				run = p.isPoint() ? run + 1 : 0;
				runs += run == 2;
			}
			std::cout << "ksim_path_walk: door-out corridor " << g_portals.size() << " portals for " << g_route.size() << " points, " << points << " point portals, "
					  << runs << " runs of 2+\n";
			const auto t0 = std::chrono::steady_clock::now();
			double sink = 0.0;
			constexpr int kCalls = 200000;
			for (int i = 0; i < kCalls; ++i)
			{
				const knav::FunnelCorner c = knav::firstCorner(g_portals, 0, -50612.7 - i * 1.0e-6, -1158.0, -50642.5, -1095.3);
				sink += c.x;
			}
			const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kCalls;
			std::cout << "ksim_path_walk: firstCorner " << us << " us per call over the whole corridor (target < 1 us; sink " << sink << ")\n";
			test::require(us < 1.0, "the per-tick funnel costs " + std::to_string(us) + " us (target < 1 us)");
		}

		// The live street routes of rwh269 (Hub streets round the bar; start / goal as clicked there), speed through their corners.
		int found = 0;
		const double live[8][4] = { { -51300, 2650, -51050, 2650 }, { -51050, 2650, -51300, 2650 }, { -51180, 2540, -51180, 2800 }, { -51180, 2800, -51180, 2540 },
			{ -51250, 2750, -51000, 2600 }, { -51000, 2600, -51250, 2750 }, { -51100, 2850, -51300, 2500 }, { -51300, 2500, -51100, 2850 } };
		for (int r = 0; r < 8; ++r)
		{
			const Trace t = walk(env, { live[r][0], live[r][1] }, -1.0e4, { live[r][2], live[r][3] }, -1.0e4, kRun, 1);
			if (!t.found)
			{
				std::cout << "ksim_path_walk: street live route " << r << ": no route\n";
				continue;
			}
			++found;
			show(("street live route " + std::to_string(r)).c_str(), t);
			// [pin] only no-stall: the live target is 79-96 % of cruise (rwh269); ours measures 54 % at an 85 degree corner and the 20 u/s floor at near-straight funnel corners (PATH_WALK.md 3c, kenshi-e9s9.13)
			test::require(t.cruiseMin >= 15.0, "a street corner stalled the walker (min speed " + std::to_string(t.cruiseMin) + ")");
		}
		std::cout << "ksim_path_walk: street corners found " << found << "\n";
		test::require(found >= 1, "no live street route found");

		// The walk speed (15 u/s): the miss distance at the goal on the door leg.
		const Trace slow = walk(env, { -50612.7, -1158.0 }, 1081.0, { -50642.5, -1095.3 }, 1089.0, kWalk, 1);
		show("door walk speed", slow);
				return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_path_walk: " << e.what() << "\n";
		return 1;
	}
}
