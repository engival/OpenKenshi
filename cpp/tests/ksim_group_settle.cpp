// A player group move settles like the original (kenshi-k16.6.231, oracle kenshi-rwh.136, SQUAD_FOLLOW.md s12): N = 3 / 5 / 8 characters on a
// line 70 u apart get one multi-select Move, the scatter of simlink.cpp. Each walks at its own goal through the others (no avoidance); everyone
// stops within 0.5 s of the first; nobody moves after its stop; only pairs whose goals are closer than R = 8 end 8 apart (the self-depenetration
// of the last moving frames); the facing at rest is the travel direction.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache). With a second argument "table" it prints the oracle's tables.

#include "ksim/nav/navigator.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "hub_point.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTickDt = 0.05;
constexpr double kAthletics = 100.0;

struct Pos { double x; double z; double yaw; };

struct Trial
{
	std::size_t n = 0;
	std::vector<std::pair<double, double>> goals;
	std::vector<std::vector<Pos>> track; // per member, per tick (index 0 = the tick the orders applied)
	std::vector<std::size_t> stopTick;   // the first tick the member is under 0.5 u/s after it walked
	std::vector<double> after;           // path length after its own first stop
	std::vector<double> travelYaw;       // the heading 0.7 s to 0.3 s before the stop
	std::vector<double> length;          // the planned route's length (u)
	double cruise = 0.0;                 // the speed all of them hold mid-walk (u/s)
};

double pairDist(const Pos& a, const Pos& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// One trial: `n` members on a line 70 u apart across `heading`, a click `reach` away along it. False when the Hub has no room for it.
bool run(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav,
	const std::string& sid, std::size_t n, double heading, double reach, double sideShift, Trial& out)
{
	const double sx = std::cos(heading); // the line's direction, perpendicular to the walk
	const double sz = -std::sin(heading);
	const double cx = kHubX + sideShift * sx;
	const double cz = kHubZ + sideShift * sz;
	const double clickX = cx + reach * std::sin(heading);
	const double clickZ = cz + reach * std::cos(heading);
	RngStream scatter(streamSeed(kClickStream, n));
	out = Trial();
	out.n = n;
	// Plan on the mesh alone first (a candidate line costs no simulation): the spots, the scatter goals and the routes.
	std::vector<state::GroundPoint> starts;
	std::vector<double> startY;
	std::vector<Route> routes;
	for (std::size_t i = 0; i < n; ++i)
	{
		const double off = (static_cast<double>(i) - (static_cast<double>(n) - 1.0) / 2.0) * 70.0;
		const double x = cx + off * sx;
		const double z = cz + off * sz;
		const auto spot = nav->walker().snap({x, 1535.0, z}, 200.0);
		if (!spot || std::hypot(spot->x - x, spot->z - z) > 3.0)
			return false;
		double gx = clickX;
		double gz = clickZ;
		if (i != 0)
		{
			const double radius = scatter.uniform01() * std::sqrt(static_cast<double>(n) * 60.0) + 5.0;
			const double angle = scatter.uniform01() * 2.0 * kPi;
			gx += radius * std::sin(angle);
			gz += radius * std::cos(angle);
		}
		if (!nav->walker().snap({gx, spot->y, gz}, 40.0))
			return false;
		Route route = nav->routeFrom({spot->x, spot->y, spot->z}, gx, gz);
		if (!route.found)
			return false;
		double length = 0.0; // open ground, as the oracle's: a route that bends round a building is not the group's walk
		for (std::size_t k = 1; k < route.points.size(); ++k)
			length += std::hypot(route.points[k].x - route.points[k - 1].x, route.points[k].z - route.points[k - 1].z);
		if (length > 1.06 * std::hypot(gx - spot->x, gz - spot->z))
			return false;
		out.goals.push_back({gx, gz});
		out.length.push_back(length);
		starts.push_back({spot->x, spot->z});
		startY.push_back(spot->y);
		routes.push_back(std::move(route));
	}
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	CombatSettings settings;
	settings.substeps = 2;
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(data, settings);
	sim.enableNavigation(nav);
	std::vector<std::uint64_t> ids;
	std::uint64_t rng = 12345;
	for (std::size_t i = 0; i < n; ++i)
	{
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.team = 0;
		f.playerControlled = true;
		f.skills.athletics = kAthletics; // the oracle's characters ran ~110 u/s
		f.yaw = std::atan2(out.goals[i].first - starts[i].x, out.goals[i].second - starts[i].z); // facing its own goal: a turn on the way off costs time the oracle's walk did not show
		f.x = starts[i].x;
		f.y = startY[i];
		f.z = starts[i].z;
		ids.push_back(sim.spawnFighter(std::move(f)).get<state::Id>()->value);
	}
	for (std::size_t i = 0; i < n; ++i)
		sim.enqueue(Command::fighterMoveTo(state::Id{ids[i]}, std::move(routes[i].points), std::move(routes[i].heights), 1, false, routes[i].approach));
	out.track.assign(n, {});
	const auto sample = [&]()
	{
		for (std::size_t i = 0; i < n; ++i)
		{
			const state::Fighter& f = *sim.combat()->find(ids[i]);
			out.track[i].push_back({f.x, f.z, f.yaw});
		}
	};
	sample();
	for (int t = 0; t < 20 * 40; ++t)
	{
		sim.run(1);
		sample();
		bool walking = false;
		for (const std::uint64_t id : ids)
			walking = walking || !sim.combat()->find(id)->route.empty();
		if (!walking && t > 40)
		{
			for (int k = 0; k < 40; ++k) // 2 s more: anything that moves after the routes ended shows
			{
				sim.run(1);
				sample();
			}
			break;
		}
	}
	for (std::size_t i = 0; i < n; ++i)
		out.cruise += pairDist(out.track[i][60], out.track[i][40]) / (20.0 * kTickDt) / static_cast<double>(n);
	out.stopTick.assign(n, 0);
	out.after.assign(n, 0.0);
	out.travelYaw.assign(n, 0.0);
	for (std::size_t i = 0; i < n; ++i)
	{
		const std::vector<Pos>& tr = out.track[i];
		bool moved = false;
		std::size_t stop = 0;
		for (std::size_t t = 1; t < tr.size(); ++t)
		{
			const double v = pairDist(tr[t], tr[t - 1]) / kTickDt;
			if (v > 5.0)
				moved = true;
			if (moved && v < 0.5)
			{
				stop = t;
				break;
			}
		}
		if (stop == 0)
			return false; // never stopped
		out.stopTick[i] = stop;
		for (std::size_t t = stop + 1; t < tr.size(); ++t)
			out.after[i] += pairDist(tr[t], tr[t - 1]);
		// the travel direction: 0.7 s to 0.3 s before the stop (the stop tick itself carries the settle's step)
		const std::size_t to = stop > 6 ? stop - 6 : 0;
		const std::size_t from = stop > 14 ? stop - 14 : 0;
		out.travelYaw[i] = std::atan2(tr[to].x - tr[from].x, tr[to].z - tr[from].z);
	}
	return true;
}

// Who the push reaches: an idle player A stands 3 u from a player B that walks a route. B on a move order's route (Order) is out of the push both
// ways, so A is not moved; the same route as a plan (a follow task's or a world task's walk) pushes as before, so A is moved.
double idleMoved(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav,
	const std::string& sid, state::Fighter::PlanSource source)
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
	std::uint64_t ids[2];
	for (int i = 0; i < 2; ++i)
	{
		state::Fighter f = data->makeFighter(gd, sid, "", rng);
		f.team = 0;
		f.playerControlled = true;
		f.x = spot->x + 3.0 * i;
		f.y = spot->y;
		f.z = spot->z;
		ids[i] = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}
	const state::Fighter& b = *sim.combat()->find(ids[1]);
	Route route = nav->routeFrom({b.x, b.y, b.z}, b.x, b.z + 80.0);
	test::require(route.found, "no route for the walker");
	sim.enqueue(Command::fighterMoveTo(state::Id{ids[1]}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
	sim.run(1);
	fighterOf(sim, ids[1])->routeSource = source;
	const double ax = sim.combat()->find(ids[0])->x;
	const double az = sim.combat()->find(ids[0])->z;
	sim.run(3);
	return std::hypot(sim.combat()->find(ids[0])->x - ax, sim.combat()->find(ids[0])->z - az);
}

struct Summary
{
	double spread;       // last first-stop minus first first-stop, s
	double maxAfter;     // the most any member moved after its stop
	double restMin, restMed, restMax;
	double crowdedMin;   // the smallest rest distance over pairs whose goals are closer than 8 (1e9 when none)
	double facingMax;    // deg, the facing at rest against the travel direction
	double walkMin;      // the smallest pair distance while walking
	double isolatedOff;  // the farthest a member with no other goal within 8 u rests from its own goal
	double fromGoalMax;  // the farthest any member rests from its own goal
};

Summary summarize(const Trial& t)
{
	Summary s{};
	// The stops differ by the members' route lengths (they all run the same speed: the oracle's 603-659 u gave its 0.1-0.5 s): the spread is
	// what is left of the stop times once each member's own length / cruise speed is taken off.
	double lo = 1e9;
	double hi = -1e9;
	for (std::size_t i = 0; i < t.n; ++i)
	{
		const double late = static_cast<double>(t.stopTick[i]) * kTickDt - t.length[i] / t.cruise;
		lo = std::min(lo, late);
		hi = std::max(hi, late);
		s.maxAfter = std::max(s.maxAfter, t.after[i]);
		const Pos& rest = t.track[i].back();
		s.fromGoalMax = std::max(s.fromGoalMax, std::hypot(rest.x - t.goals[i].first, rest.z - t.goals[i].second));
		s.facingMax = std::max(s.facingMax, std::fabs(std::remainder(rest.yaw - t.travelYaw[i], 2.0 * kPi)) * 180.0 / kPi);
	}
	s.spread = hi - lo;
	for (std::size_t i = 0; i < t.n; ++i)
	{
		bool crowded = false;
		for (std::size_t j = 0; j < t.n; ++j)
			crowded = crowded || (j != i && std::hypot(t.goals[i].first - t.goals[j].first, t.goals[i].second - t.goals[j].second) < 8.0);
		if (!crowded)
			s.isolatedOff = std::max(s.isolatedOff, std::hypot(t.track[i].back().x - t.goals[i].first, t.track[i].back().z - t.goals[i].second));
	}
	s.restMin = 1e9;
	s.crowdedMin = 1e9;
	s.walkMin = 1e9;
	std::vector<double> rests;
	for (std::size_t i = 0; i < t.n; ++i)
	{
		for (std::size_t j = i + 1; j < t.n; ++j)
		{
			const double d = pairDist(t.track[i].back(), t.track[j].back());
			rests.push_back(d);
			s.restMin = std::min(s.restMin, d);
			s.restMax = std::max(s.restMax, d);
			if (std::hypot(t.goals[i].first - t.goals[j].first, t.goals[i].second - t.goals[j].second) < 8.0)
				s.crowdedMin = std::min(s.crowdedMin, d);
			for (std::size_t k = 0; k < t.track[i].size() && k < t.stopTick[i] && k < t.stopTick[j]; ++k)
				s.walkMin = std::min(s.walkMin, pairDist(t.track[i][k], t.track[j][k]));
		}
	}
	std::sort(rests.begin(), rests.end());
	s.restMed = rests[rests.size() / 2];
	return s;
}

void table(const Trial& t, const Summary& s)
{
	std::printf("N=%zu goals closer than 8:", t.n);
	for (std::size_t i = 0; i < t.n; ++i)
		for (std::size_t j = i + 1; j < t.n; ++j)
		{
			const double d = std::hypot(t.goals[i].first - t.goals[j].first, t.goals[i].second - t.goals[j].second);
			if (d < 8.0)
				std::printf(" (%zu,%zu)=%.2f", i, j, d);
		}
	std::printf("\n  pairwise min / median every 0.5 s while walking:");
	for (std::size_t k = 0; k < t.track[0].size(); k += 10)
	{
		std::vector<double> d;
		for (std::size_t i = 0; i < t.n; ++i)
			for (std::size_t j = i + 1; j < t.n; ++j)
				d.push_back(pairDist(t.track[i][k], t.track[j][k]));
		std::sort(d.begin(), d.end());
		std::printf(" [%.1fs %.1f/%.1f]", static_cast<double>(k) * kTickDt, d.front(), d[d.size() / 2]);
		if (k > 12 * 20)
			break;
	}
	std::size_t lo = t.stopTick[0];
	std::size_t hi = lo;
	for (const std::size_t st : t.stopTick)
	{
		lo = std::min(lo, st);
		hi = std::max(hi, st);
	}
	std::printf("\n  first stop %.2f s, last stop %.2f s (spread after route length / %.0f u/s: %.2f s); walk pair min %.2f u; mean speed member 0 %.0f u/s over %.0f u\n", static_cast<double>(lo) * kTickDt, static_cast<double>(hi) * kTickDt, t.cruise, s.spread, s.walkMin, pairDist(t.track[0][0], t.track[0][t.stopTick[0]]) / (static_cast<double>(t.stopTick[0]) * kTickDt), pairDist(t.track[0][0], t.track[0][t.stopTick[0]]));
	std::printf("  post-stop movement per member:");
	for (const double a : t.after)
		std::printf(" %.2f", a);
	if (std::getenv("SETTLE_TRACE"))
		for (std::size_t i = 0; i < t.n; ++i)
		{
			std::printf("\n  m%zu stop %.2f straight %.1f travelYaw %.1f:", i, static_cast<double>(t.stopTick[i]) * kTickDt, pairDist(t.track[i][0], {t.goals[i].first, t.goals[i].second, 0.0}), t.travelYaw[i] * 180.0 / kPi);
			for (std::size_t k = t.stopTick[i] > 12 ? t.stopTick[i] - 12 : 0; k < t.track[i].size() && k < t.stopTick[i] + 6; ++k)
				std::printf(" [%.1f v%.0f]", t.track[i][k].yaw * 180.0 / kPi, k ? pairDist(t.track[i][k], t.track[i][k - 1]) / kTickDt : 0.0);
		}
	std::printf("\n  rest pairwise min/median/max %.2f/%.2f/%.2f; crowded-goal pairs min %s%.2f; farthest from own goal %.2f\n", s.restMin, s.restMed, s.restMax,
		s.crowdedMin > 1e8 ? "none " : "", s.crowdedMin > 1e8 ? 0.0 : s.crowdedMin, s.fromGoalMax);
	std::printf("  facing at rest vs travel: max %.1f deg;", s.facingMax);
	std::printf(" per member:");
	for (std::size_t i = 0; i < t.n; ++i)
		std::printf(" %.1f", std::fabs(std::remainder(t.track[i].back().yaw - t.travelYaw[i], 2.0 * kPi)) * 180.0 / kPi);
	std::printf("\n");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc >= 2, "usage: ksim_group_settle HUB.knav [table]");
		const bool print = argc > 2;
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		std::string sid;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				sid = r->stringID;
		}
		test::require(idleMoved(data, gd, nav, sid, state::Fighter::PlanSource::Order) < 0.01, "an idle player was pushed by a player's move-order walk");
		test::require(idleMoved(data, gd, nav, sid, state::Fighter::PlanSource::Plan) > 0.05, "a player on a plan walk (follow / world task) no longer pushes its neighbour");
		std::string failed;
		const auto check = [&failed](bool ok, const std::string& what) { if (!ok) failed += what + "\n"; };
		for (const std::size_t n : {3, 5, 8})
		{
			Trial t;
			bool ok = false;
			// the longest walk the Hub gives (the oracle's was ~620 u), over a few headings and line positions
			for (const double reach : {620.0, 450.0, 300.0})
			{
				for (const double side : {0.0})
					for (int k = 0; k < 16 && !ok; ++k)
						ok = run(data, gd, nav, sid, n, k * kPi / 8.0, reach, side, t);
				if (ok)
					break;
			}
			test::require(ok, "no Hub room for a line of " + std::to_string(n));
			const Summary s = summarize(t);
			if (print)
				table(t, s);
			check(s.spread <= 0.6, "N=" + std::to_string(n) + ": not everyone stopped within 0.6 s of the first (route length taken off)");
			check(s.isolatedOff <= 1.0, "N=" + std::to_string(n) + ": a member with no goal within 8 u of another did not rest on its own goal (the settle moved it)");
			check(s.maxAfter <= 0.5, "N=" + std::to_string(n) + ": a member moved more than 0.5 u after its stop");
			check(s.crowdedMin >= 7.9, "N=" + std::to_string(n) + ": a pair whose goals are closer than 8 rests closer than 7.9");
			check(s.walkMin < 7.9, "N=" + std::to_string(n) + ": the walkers avoided each other (the original walks through)");
			check(s.facingMax <= 5.0, "N=" + std::to_string(n) + ": the facing at rest is more than 5 degrees off the travel direction");
		}
		test::require(failed.empty(), "\n" + failed);
		std::cout << "ksim_group_settle ok\n";
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
