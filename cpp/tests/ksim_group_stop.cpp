// A player group move stops quietly (kenshi-k16.6.597, rwh.136, k16.6.219.1 live): human + animal (the 'Guy with a Dog' start) and three humans
// get one multi-select Move with simlink's scatter. After the stop nobody moves, and no member ever steps faster than its walk speed.
// argv[1]: the Hub navmesh cache. With a second argument it prints the per-tick trace.
#include "ksim/nav/navigator.hpp"
#include "ksim/nav/stand_points.hpp"
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
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kTickDt = 0.05;
constexpr double kPi = 3.14159265358979323846;

std::string idOf(const kdata::GameData& gd, const char* name)
{
	for (const char* type : {"CHARACTER", "ANIMAL_CHARACTER"})
	{
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
		{
			if (r->name == name)
				return r->stringID;
		}
	}
	return "";
}

// One trial: members of `sids` 30 u apart on a line across `heading`, a click `reach` away. Returns an error text, empty when it holds.
std::string run(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav,
	const std::vector<std::string>& sids, double heading, double reach, bool print)
{
	const std::size_t n = sids.size();
	const double sx = std::cos(heading);
	const double sz = -std::sin(heading);
	const double clickX = kHubX + reach * std::sin(heading);
	const double clickZ = kHubZ + reach * std::cos(heading);
	RngStream scatter(streamSeed(kClickStream, n));
	Simulation sim = Simulation::load(KENSHI_DATA, 77);
	CombatSettings settings;
	settings.substeps = 2;
	sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute));
	sim.enableCombat(data, settings);
	sim.enableNavigation(nav);
	std::vector<std::uint64_t> ids;
	std::vector<Route> routes;
	std::vector<std::pair<double, double>> goals;
	std::vector<std::pair<std::pair<double, double>, double>> stands;
	std::uint64_t rng = 12345;
	for (std::size_t i = 0; i < n; ++i)
	{
		const double off = (static_cast<double>(i) - (static_cast<double>(n) - 1.0) / 2.0) * 30.0;
		const auto spot = nav->walker().snap({kHubX + off * sx, 1535.0, kHubZ + off * sz}, 200.0);
		if (!spot)
			return "skip";
		double gx = clickX;
		double gz = clickZ;
		state::Fighter f = data->makeFighter(gd, sids[i], "", rng);
		const double radius0 = data->pathRadius(f.raceSid);
		if (i != 0)
		{
			const double radius = scatter.uniform01() * std::sqrt(static_cast<double>(n) * 60.0) + 5.0;
			const double angle = scatter.uniform01() * 2.0 * kPi;
			gx += radius * std::sin(angle);
			gz += radius * std::cos(angle);
			std::tie(gx, gz) = standPoint({gx, gz}, radius0, stands, angle);
		}
		stands.push_back({{gx, gz}, radius0});
		f.team = 0;
		f.playerControlled = true;
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		if (!nav->walker().snap({gx, spot->y, gz}, 40.0))
			return "skip";
		Route route = nav->routeFrom({spot->x, spot->y, spot->z}, gx, gz, std::nullopt, false, data->body(f.raceSid), true, true);
		if (!route.found)
			return "skip";
		ids.push_back(sim.spawnFighter(std::move(f)).get<state::Id>()->value);
		routes.push_back(std::move(route));
		goals.push_back({gx, gz});
	}
	for (std::size_t i = 0; i < n; ++i)
		sim.enqueue(Command::fighterMoveTo(state::Id{ids[i]}, std::move(routes[i].points), std::move(routes[i].heights), 1, false, routes[i].approach));
	std::vector<std::vector<std::pair<double, double>>> track(n);
	const auto sample = [&]()
	{
		for (std::size_t i = 0; i < n; ++i)
		{
			const state::Fighter& f = *sim.combat()->find(ids[i]);
			track[i].push_back({f.x, f.z});
		}
	};
	sample();
	int lastWalk = 0;
	for (int t = 1; t < 20 * 60; ++t)
	{
		sim.run(1);
		sample();
		bool walking = false;
		for (const std::uint64_t id : ids)
			walking = walking || !sim.combat()->find(id)->route.empty();
		if (walking)
			lastWalk = t;
		else if (t > lastWalk + 200)
			break;
	}
	std::string err;
	for (std::size_t i = 0; i < n; ++i)
		for (std::size_t j = i + 1; j < n; ++j)
		{
			const double need = stands[i].second + stands[j].second;
			if (std::hypot(track[i].back().first - track[j].back().first, track[i].back().second - track[j].back().second) < need - 0.1)
				err += "members " + std::to_string(i) + "," + std::to_string(j) + " rest closer than R=" + std::to_string(need) + "\n";
		}
	if (lastWalk == 0 || lastWalk > 20 * 50)
		return "skip";
	for (std::size_t i = 0; i < n; ++i)
	{
		const auto& tr = track[i];
		double cruise = 0.0; // the walk's own step (u/tick): nothing, a settle included, steps faster than it
		for (std::size_t t = 20; t < 60 && t < tr.size(); ++t) // mid-walk
			cruise = std::max(cruise, std::hypot(tr[t].first - tr[t - 1].first, tr[t].second - tr[t - 1].second));
		const double cruiseLimit = 1.05 * cruise;
		for (std::size_t t = 1; t < tr.size(); ++t)
		{
			const double step = std::hypot(tr[t].first - tr[t - 1].first, tr[t].second - tr[t - 1].second);
			const bool post = static_cast<int>(t) > lastWalk + 20;
			if (print && (step > 0.01 && (post || static_cast<int>(t) > lastWalk - 30)))
				std::printf("m%zu t%zu step %.2f %s\n", i, t, step, post ? "POST" : "");
			if (step > cruiseLimit)
				err += "member " + std::to_string(i) + " jumped " + std::to_string(step) + " u in one tick (walk step " + std::to_string(cruise) + ") at tick " + std::to_string(t) + "\n";
			if (post && step > 0.01)
				err += "member " + std::to_string(i) + " moved " + std::to_string(step) + " u at tick " + std::to_string(t) + " (last walker stopped " + std::to_string(lastWalk) + ")\n";
		}
	}
	return err;
}

// Two idle players 3 u apart: the one the camera follows is never pushed, the other yields at no more than a walk step per tick.
std::string followed(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav, const std::string& human)
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
		state::Fighter f = data->makeFighter(gd, human, "", rng);
		f.team = 0;
		f.playerControlled = true;
		f.x = spot->x + 3.0 * i;
		f.y = spot->y;
		f.z = spot->z;
		ids[i] = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}
	const_cast<CombatArena*>(sim.combat())->setCameraFollow(ids[0]);
	const state::Fighter& a = *sim.combat()->find(ids[0]);
	const state::Fighter& b = *sim.combat()->find(ids[1]);
	const double ax = a.x;
	const double az = a.z;
	double px = b.x;
	double pz = b.z;
	double maxStep = 0.0;
	for (int t = 0; t < 40; ++t)
	{
		sim.run(1);
		maxStep = std::max(maxStep, std::hypot(b.x - px, b.z - pz));
		px = b.x;
		pz = b.z;
	}
	if (std::hypot(a.x - ax, a.z - az) > 1.0e-6)
		return "the camera's character was pushed\n";
	if (std::hypot(b.x - a.x, b.z - a.z) < 5.0)
		return "the neighbour did not yield\n";
	if (maxStep > 3.6)
		return "the neighbour jumped " + std::to_string(maxStep) + " u in a tick\n";
	return "";
}

// The kenshi-k16.17.13.8 shape: a standing player and an NPC patroller walking through it. The player never steps more than a walk step per tick.
std::string patrolled(const std::shared_ptr<const CombatData>& data, const kdata::GameData& gd, const std::shared_ptr<MeshNavigator>& nav, const std::string& human)
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
	state::Fighter pf = data->makeFighter(gd, human, "", rng);
	pf.team = 0;
	pf.playerControlled = true;
	pf.x = spot->x;
	pf.y = spot->y;
	pf.z = spot->z + 40.0;
	state::Fighter wf = data->makeFighter(gd, human, "", rng);
	wf.team = 0;
	wf.playerControlled = false;
	wf.x = spot->x;
	wf.y = spot->y;
	wf.z = spot->z;
	const std::uint64_t pid = sim.spawnFighter(std::move(pf)).get<state::Id>()->value;
	const std::uint64_t wid = sim.spawnFighter(std::move(wf)).get<state::Id>()->value;
	const state::Fighter& w = *sim.combat()->find(wid);
	Route route = nav->routeFrom({w.x, w.y, w.z}, spot->x, spot->z + 120.0);
	test::require(route.found, "no route for the patroller");
	sim.enqueue(Command::fighterMoveTo(state::Id{wid}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
	const state::Fighter& p = *sim.combat()->find(pid);
	double px = p.x;
	double pz = p.z;
	double worst = 0.0;
	double moved = 0.0;
	for (int t = 0; t < 20 * 8; ++t)
	{
		sim.run(1);
		const double step = std::hypot(p.x - px, p.z - pz);
		worst = std::max(worst, step);
		moved += step;
		px = p.x;
		pz = p.z;
	}
	if (moved < 0.1)
		return "the patroller never touched the player (the case is void)\n";
	if (worst > 3.6)
		return "the standing player was pushed " + std::to_string(worst) + " u in one tick by a patroller\n";
	return "";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc >= 2, "usage: ksim_group_stop HUB.knav [print]");
		const bool print = argc > 2;
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const std::string human = idOf(gd, "Escaped Servant");
		const std::string dog = idOf(gd, "Bonedog");
		test::require(!human.empty() && !dog.empty(), "no human / Bonedog record");
		std::string failed = followed(data, gd, nav, human) + patrolled(data, gd, nav, human);
		const std::vector<std::vector<std::string>> squads = {{human, dog}, {dog, human}, {human, human, human}};
		for (const auto& squad : squads)
		{
			bool ran = false;
			for (int k = 0; k < 16 && !ran; ++k)
			{
				const std::string e = run(data, gd, nav, squad, k * kPi / 8.0, 300.0, print);
				if (e != "skip")
				{
					ran = true;
					failed += e;
				}
			}
			test::require(ran, "no Hub room for a squad of " + std::to_string(squad.size()));
		}
		test::require(failed.empty(), "\n" + failed);
		std::cout << "ksim_group_stop ok\n";
		return 0;
	} catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
