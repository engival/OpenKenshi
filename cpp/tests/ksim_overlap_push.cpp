// A passing runner pushes the bodies it overlaps (kenshi-k16.6.425, oracle kenshi-rwh.188). A player on a move-order walk through idle bodies:
// at walk speed every overlapped body is pushed radially out of the 8 u contact distance; at run speed only the share whose contact lasts long
// enough (kContactTicks) is; the runner's own line and time are undisturbed (it is not pushed back).
// argv[1]: the Hub navmesh cache (ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "hub_point.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kPi = 3.14159265358979323846;
constexpr int kBodies = 12;
constexpr double kSpacing = 24.0; // along the route; the lateral offset cycles 0..+-6 u, every body inside R = 8 of the runner's line

struct Result
{
	double time = 0.0;      // s to the route's end
	double lateral = 0.0;   // the runner's largest distance off the planned route
	int overlapped = 0;
	int moved = 0;
	double minMove = 1e9;
	double maxMove = 0.0;
	double minRadial = 1.0; // the least cosine between a body's per-tick move and the direction away from the runner's position at that tick's start
};

Result pass(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav,
	const std::string& sid, std::uint8_t mode, bool crowd)
{
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	CombatSettings settings;
	settings.substeps = 2;
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(data, settings);
	sim.enableNavigation(nav);
	const auto spot = nav->walker().snap({kHubX, 1535.0, kHubZ}, 200.0);
	test::require(spot.has_value(), "no Hub spot");
	std::uint64_t rng = 12345;
	const double length = kBodies * kSpacing + 40.0;
	state::Fighter r = data->makeFighter(gd, sid, "", rng);
	r.team = 0;
	r.playerControlled = true;
	r.skills.athletics = 100.0;
	r.moveMode = mode;
	r.x = spot->x;
	r.y = spot->y;
	r.z = spot->z;
	const std::uint64_t runner = sim.spawnFighter(std::move(r)).get<state::Id>()->value;
	Route route = nav->routeFrom({spot->x, spot->y, spot->z}, spot->x, spot->z + length);
	test::require(route.found, "no route");
	struct Body { std::uint64_t id; double x, z, off; };
	const std::vector<state::GroundPoint> line = route.points;
	// The point `along` u down the route polyline, and its unit direction.
	const auto at = [&line](double along, double& dx, double& dz)
	{
		for (std::size_t k = 1; k < line.size(); ++k)
		{
			const double seg = std::hypot(line[k].x - line[k - 1].x, line[k].z - line[k - 1].z);
			if (along <= seg || k + 1 == line.size())
			{
				dx = (line[k].x - line[k - 1].x) / seg;
				dz = (line[k].z - line[k - 1].z) / seg;
				return state::GroundPoint{line[k - 1].x + dx * along, line[k - 1].z + dz * along};
			}
			along -= seg;
		}
		return line.back();
	};
	std::vector<Body> bodies;
	for (int i = 0; crowd && i < kBodies; ++i)
	{
		const double off = -6.5 + i * 13.0 / (kBodies - 1); // -6.5 .. 6.5: a spread of passes between a dead-on and a graze
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		// A body with a wall within its own clearance on any side cannot be pushed that way (the sim's depenetration is a mesh step with the
		// body's path radius, so a blocked step moves nothing: kenshi-tpvx, body 8 at 3 u beside a Hub wall). The scenario is bodies with room to
		// give way, so a spot without it slides along the route (2 u a try) to the nearest one that has it.
		const double room = data->pathRadius(f.raceSid);
		std::optional<knav::Position> ground;
		for (int slide = 0; slide < 30 && !ground; ++slide)
		{
			double dx = 0.0;
			double dz = 1.0;
			const double along = 30.0 + i * kSpacing + ((slide & 1) ? -1 : 1) * 2.0 * ((slide + 1) / 2);
			const state::GroundPoint c = at(along, dx, dz);
			const auto cand = nav->walker().snap({c.x - dz * off, spot->y, c.z + dx * off}, 40.0);
			bool free = cand.has_value();
			for (int k = 0; free && k < 16; ++k)
			{
				const double a = k * (kPi / 8.0);
				free = nav->walker().step({cand->x, cand->y, cand->z}, std::cos(a) * room, std::sin(a) * room, room).moved > room - 0.05;
			}
			if (free)
				ground = cand;
		}
		test::require(ground.has_value(), "no ground with room for a body");
		f.team = 0;
		f.x = ground->x;
		f.y = ground->y;
		f.z = ground->z;
		bodies.push_back({sim.spawnFighter(std::move(f)).get<state::Id>()->value, ground->x, ground->z, off});
	}
	sim.run(3);
	for (Body& b : bodies) { b.x = sim.combat()->find(b.id)->x; b.z = sim.combat()->find(b.id)->z; }
	sim.enqueue(Command::fighterMoveTo(state::Id{runner}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
	Result out;
	int ticks = 0;
	const double x0 = spot->x;
	const double z0 = spot->z;
	std::vector<std::pair<double, double>> last(bodies.size());
	for (std::size_t i = 0; i < bodies.size(); ++i)
		last[i] = {bodies[i].x, bodies[i].z};
	std::vector<bool> pushedOnce(bodies.size(), false);
	double px = x0;
	double pz = z0;
	for (; ticks < 20 * 60; ++ticks)
	{
		sim.run(1);
		const state::Fighter& f = *sim.combat()->find(runner);
		// The push's direction per step: away from where the runner stood at the step's start (the push reads the positions the last step left).
		for (std::size_t i = 0; i < bodies.size(); ++i)
		{
			const state::Fighter& bf = *sim.combat()->find(bodies[i].id);
			const double mx = bf.x - last[i].first;
			const double mz = bf.z - last[i].second;
			const double mv = std::hypot(mx, mz);
			const double ax = last[i].first - px;
			const double az = last[i].second - pz;
			const double al = std::hypot(ax, az);
			if (mv > 0.05 && al > 1e-6 && !pushedOnce[i]) // the first push of each body: later steps can be a neighbour's shove (a body bulldozed 20 u into the next)
			{
				pushedOnce[i] = true;
				out.minRadial = std::min(out.minRadial, (mx * ax + mz * az) / (mv * al));
			}
			last[i] = {bf.x, bf.z};
		}
		px = f.x;
		pz = f.z;
		double nearest = 1e9; // the distance to the planned polyline
		for (std::size_t k = 1; k < line.size(); ++k)
		{
			const double sx = line[k].x - line[k - 1].x;
			const double sz = line[k].z - line[k - 1].z;
			const double t = std::clamp(((f.x - line[k - 1].x) * sx + (f.z - line[k - 1].z) * sz) / (sx * sx + sz * sz), 0.0, 1.0);
			nearest = std::min(nearest, std::hypot(f.x - line[k - 1].x - t * sx, f.z - line[k - 1].z - t * sz));
		}
		out.lateral = std::max(out.lateral, nearest);
		if (ticks > 10 && f.route.empty())
			break;
	}
	out.time = ticks * 0.05;
	for (const Body& b : bodies)
	{
		const state::Fighter& f = *sim.combat()->find(b.id);
		++out.overlapped;
		const double mv = std::hypot(f.x - b.x, f.z - b.z);
		if (mv > 0.3)
		{
			++out.moved;
			out.minMove = std::min(out.minMove, mv);
			out.maxMove = std::max(out.maxMove, mv);
		}
	}
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc >= 2, "usage: ksim_overlap_push HUB.knav");
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		std::string sid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
		const Result runBase = pass(data, gd, nav, sid, 2, false);
		const Result run = pass(data, gd, nav, sid, 2, true);
		const Result walk = pass(data, gd, nav, sid, 0, true);
		const Result walkBase = pass(data, gd, nav, sid, 0, false);
		std::printf("run: overlapped %d moved %d move %.2f-%.2f cos %.2f time %.2f (alone %.2f) lateral %.2f\n", run.overlapped, run.moved, run.minMove, run.maxMove, run.minRadial, run.time, runBase.time, run.lateral);
		std::printf("walk: overlapped %d moved %d move %.2f-%.2f cos %.2f lateral %.2f\n", walk.overlapped, walk.moved, walk.minMove, walk.maxMove, walk.minRadial, walk.lateral);
		test::require(run.overlapped >= 3, "fewer than 3 overlapped bodies");
		test::require(walk.moved == walk.overlapped, "at walk speed not every overlapped body was pushed");
		test::require(walk.minRadial > 0.8, "the walk push is not radial");
		test::require(run.moved >= 1 && run.moved < run.overlapped, "at run speed the pushed share is not a part of the overlapped bodies");
		test::require(run.moved * 100 >= run.overlapped * 10 && run.moved * 100 <= run.overlapped * 60, "the run-speed pushed share is outside 10-60 % (live 29 %)");
		test::require(run.minMove >= 0.3 && run.maxMove <= 8.0 && run.minRadial > 0.8, "a run-speed push is outside 0.3-8 u or not radial");
		test::require(std::fabs(run.time - runBase.time) <= 0.15, "the crowd changed the runner's time");
		// The deflection the crowd adds, over what the walker's own line does with no crowd: the corridor walk cuts and pushes off the route's bends (PATH_WALK.md, kenshi-e9s9.23:
		// 3.6 u off the planned polyline alone), which is not the crowd's doing (kenshi-e9s9.36).
		std::printf("lateral alone: run %.2f walk %.2f\n", runBase.lateral, walkBase.lateral);
		test::require(run.lateral <= runBase.lateral + 1.0 && walk.lateral <= walkBase.lateral + 1.0, "the crowd deflected the runner");
		std::cout << "ksim_overlap_push ok\n";
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
