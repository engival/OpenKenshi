// kenshi-k16.6.481 (rwh208, LOCOMOTION.md D9): a far move order is never refused and walks as the exe does. From Heft's gate (67160, -72159) a move
// to (67406, -54491), 17.6k u south and over three zone borders, sets off at once, follows the roads and arrives; the plain route to it is the
// no_route the user saw (kenshi-k16.6.464). A second case: a far goal on ground with no mesh within 300 u sets off and ends silently with the goal
// blocked. Needs the world navmesh with the Heft region (exit 77 without).
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"

#include "kworld/roads.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace ksim;

namespace {

constexpr double kGateX = 67160.0;
constexpr double kGateY = 1269.0;
constexpr double kGateZ = -72159.0;

double segmentDistance(double px, double pz, double ax, double az, double bx, double bz)
{
	const double dx = bx - ax;
	const double dz = bz - az;
	const double len2 = dx * dx + dz * dz;
	const double t = len2 > 0.0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / len2, 0.0, 1.0) : 0.0;
	return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

double roadDistance(const kworld::RoadNetwork& net, double x, double z)
{
	double best = 1.0e30;
	for (const kworld::RoadNetwork::Road& road : net.roads())
	{
		for (std::size_t i = 0; i + 1 < road.markers.size(); ++i)
			best = std::min(best, segmentDistance(x, z, road.markers[i].x, road.markers[i].z, road.markers[i + 1].x, road.markers[i + 1].z));
	}
	return best;
}

struct Walk
{
	double walked = 0.0;
	std::size_t zones = 0; // zone borders crossed
	double endX = 0.0;
	double endZ = 0.0;
	double seconds = 0.0;
	std::vector<std::pair<double, double>> samples;
	bool setOff = false;
	// Per tick, while the far order is held and the walker is past its start-up (kenshi-k16.6.537): a leg handover must not brake or turn it.
	std::size_t slowTicks = 0; // under a quarter of the peak speed, not at the goal
	std::size_t reversals = 0; // a facing change over 90 degrees in one tick
	std::size_t emptyTicks = 0; // order held, route spent, walker not at the goal
	std::uint32_t bumps = 0; // routeVersion changes
	double peak = 0.0;
	// The corridor of each route as installed (kenshi-e9s9.5: the point portals and the bytes they save, 52 B a portal):
	std::size_t installs = 0, routePoints = 0, portals = 0, pointPortals = 0, pointRuns = 0;
	std::uint32_t legs = 0; // legs planned (the held order's count, the largest seen)
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/gate_heft.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_far_move: SKIPPED: needs the world navmesh with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world);
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.setRoadLookahead(0.0); // the exe's handover is what this test measures (rwh.268); the look-ahead is ksim_road_lookahead's
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 1);
		test::require(!placed.players.empty(), "gate_heft places no player");
		const std::uint64_t id = placed.players.front();
		sim.run(40);
		{
			state::Fighter* f = fighterOf(sim, id);
			f->x = kGateX;
			f->y = kGateY;
			f->z = kGateZ;
		}
		sim.run(60);

		// What a move order does with the goal (simlink move_to): a goal in a zone that is not loaded is a far order.
		const auto order = [&](double gx, double gz, double gy, std::uint32_t seq)
		{
			const state::Fighter* f = fighterOf(sim, id);
			test::require(sim.navigation()->farGoal(f->x, f->z, gx, gz), "the goal's zone is loaded: not a far goal");
			sim.enqueue(Command::fighterMoveFar(state::Id{id}, gx, gz, gy, seq));
		};
		const auto walk = [&](double seconds) -> Walk
		{
			Walk w;
			const state::Fighter* f = fighterOf(sim, id);
			double lastX = f->x;
			double lastZ = f->z;
			kworld::ZoneCell zone = kworld::zoneCellOf(lastX, lastZ);
			std::set<int> seen;
			double lastYaw = f->yaw;
			std::uint32_t lastVersion = f->routeVersion;
			for (double t = 0.0; t < seconds; t += 1.0)
			{
				for (int k = 0; k < 20; ++k)
				{
					sim.run(1);
					f = fighterOf(sim, id);
					w.bumps += f->routeVersion != lastVersion;
					if (f->routeVersion != lastVersion)
					{
						++w.installs;
						w.routePoints += f->route.size();
						w.portals += f->portals.size();
						std::size_t run = 0;
						for (const state::RoutePortal& p : f->portals)
						{
							w.pointPortals += p.isPoint();
							run = p.isPoint() ? run + 1 : 0;
							w.pointRuns += run == 2;
						}
					}
					lastVersion = f->routeVersion;
					w.peak = std::max(w.peak, f->forwardSpeed);
					if (const auto* held = sim.farOrder(id))
						w.legs = std::max(w.legs, held->legs);
					if (sim.farOrder(id) != nullptr && t >= 3.0)
					{
						w.slowTicks += f->forwardSpeed < 0.25 * w.peak; // corners dip to ~1/3 (a gait turn); a leg-end brake goes to 0
						w.emptyTicks += f->route.empty();
						double d = std::fabs(f->yaw - lastYaw);
						d = std::min(d, 2.0 * std::numbers::pi - d);
						w.reversals += d > std::numbers::pi / 2.0;
					}
					lastYaw = f->yaw;
				}
				f = fighterOf(sim, id);
				// gate_heft's Wanderer starts wounded and would collapse on a 5-minute walk. The walk is what is measured, not the medicine.
				if (state::Fighter* hurt = fighterOf(sim, id); !hurt->dead)
				{
					hurt->blood = 150.0;
					hurt->bleeding = 0.0;
					hurt->hunger = 300.0; // and fed
					for (state::FighterPart& part : hurt->parts)
					{
						part.health = part.maxHealth;
						part.stun = 0.0;
					}
					hurt->koTimer = 0.0;
				}
				w.walked += std::hypot(f->x - lastX, f->z - lastZ);
				lastX = f->x;
				lastZ = f->z;
				if (const kworld::ZoneCell now = kworld::zoneCellOf(f->x, f->z); !(now == zone))
				{
					zone = now;
					++w.zones;
				}
				w.samples.push_back({f->x, f->z});
				w.seconds = t + 1.0;
				if (t < 3.0 && f->walking())
					w.setOff = true;
				if (!f->walking() && sim.farOrder(id) == nullptr && t > 2.0)
					break;
			}
			w.endX = lastX;
			w.endZ = lastZ;
			return w;
		};

		// ---- the user's click: the plain route is the no_route of kenshi-k16.6.464; the order walks ----
		constexpr double kGoalX = 67406.0;
		constexpr double kGoalZ = -54491.0;
		{
			const state::Fighter* f = fighterOf(sim, id);
			const Route plain = sim.navigation()->routeFrom({f->x, f->y, f->z}, kGoalX, kGoalZ, 1088.0, false, sim.combat()->data().body(f->raceSid), true, true);
			test::require(!plain.found, "the plain route to the far goal was found: the case no longer shows the old refusal");
			order(kGoalX, kGoalZ, 1088.0, 1);
			sim.run(2);
			test::require(fighterOf(sim, id)->walking(), "the far order did not set off the tick it applied");
			const Walk w = walk(900.0);
			const double straight = std::hypot(kGoalX - kGateX, kGoalZ - kGateZ);
			const double off = std::hypot(w.endX - kGoalX, w.endZ - kGoalZ);
			std::vector<double> roads;
			const kworld::RoadNetwork net = kworld::RoadNetwork::load(KENSHI_DATA);
			for (const auto& s : w.samples)
				roads.push_back(roadDistance(net, s.first, s.second));
			std::sort(roads.begin(), roads.end());
			const double median = roads.empty() ? -1.0 : roads[roads.size() / 2];
			std::cout << "ksim_far_move: walked " << w.walked << " u of " << straight << " straight (" << w.walked / straight << "x, exe 1.28x), "
				<< w.zones << " zone borders, " << w.seconds << " s, ended " << off << " u from the click, median " << median << " u from the road (exe 37 u)\n";
			std::cout << "ksim_far_move: road factor used " << sim.roadFactor(id) << " (faction road preference, exe 0.7 for the Nameless)\n";
			std::cout << "ksim_far_move: peak " << w.peak << " u/s, slow ticks " << w.slowTicks << ", reversals " << w.reversals << ", empty-route ticks "
				<< w.emptyTicks << ", routeVersion bumps " << w.bumps << ", legs " << w.legs << ", zones " << w.zones << '\n';
			std::cout << "ksim_far_move: corridors installed " << w.installs << ", route points " << w.routePoints << ", portals " << w.portals << ", point portals "
				<< w.pointPortals << ", runs of 2+ point portals " << w.pointRuns << ", " << (w.installs ? static_cast<double>(w.pointPortals) * 52.0 / w.installs : 0.0)
				<< " B a corridor in point portals\n";
			// The exe's handover (rwh.268) cuts a road corner and takes it by the velocity slew: a sharp one dips under a quarter of the peak for a tick or two
			// (v cos(theta/2)); a brake to nothing at a marker would be dozens of ticks.
			test::require(w.slowTicks <= 6, std::to_string(w.slowTicks) + " ticks under a quarter speed during the far walk (a leg handover braked it)");
			test::require(w.bumps <= w.legs + 8, std::to_string(w.bumps) + " routeVersion bumps for " + std::to_string(w.legs) + " legs");
			test::require(w.reversals == 0, std::to_string(w.reversals) + " facing reversals during the far walk");
			test::require(w.emptyTicks == 0, std::to_string(w.emptyTicks) + " ticks with the order held and no route");
			test::require(w.setOff, "the walker did not set off within 3 s");
			test::require(w.zones >= 3, "crossed " + std::to_string(w.zones) + " zone borders, 3 wanted");
			test::require(off <= 10.0, "ended " + std::to_string(off) + " u from the goal, within 10 wanted");
			test::require(sim.farOrder(id) == nullptr, "the leg-by-leg order is still held after the arrival");
		}

		// ---- a save mid-walk, then the reload: the order continues to the goal ----
		{
			state::Fighter* f = fighterOf(sim, id);
			f->x = kGateX;
			f->y = kGateY;
			f->z = kGateZ;
			sim.run(40);
			order(kGoalX, kGoalZ, 1088.0, 3);
			walk(60.0); // into the first legs
			test::require(sim.farOrder(id) != nullptr, "no far order held 60 s into the walk");
			std::ostringstream out;
			sim.save(out);
			const std::string bytes = out.str();
			std::istringstream in(bytes);
			sim.reload(in);
			test::require(sim.farOrder(id) != nullptr, "the far order was not restored by the load");
			test::require(sim.farOrder(id)->seq == 3, "the restored far order lost its seq");
			const Walk w = walk(900.0);
			const double off = std::hypot(w.endX - kGoalX, w.endZ - kGoalZ);
			std::cout << "ksim_far_move: after the reload ended " << off << " u from the click\n";
			test::require(off <= 10.0, "after the reload ended " + std::to_string(off) + " u from the goal, within 10 wanted");
		}

		// ---- a save on the tick a leg ends (route empty, order held), then the reload: the walker goes on to the goal ----
		{
			state::Fighter* f = fighterOf(sim, id);
			f->x = kGateX;
			f->y = kGateY;
			f->z = kGateZ;
			sim.run(40);
			order(kGoalX, kGoalZ, 1088.0, 5);
			walk(60.0); // into the first legs
			test::require(sim.farOrder(id) != nullptr, "no far order held 60 s into the walk");
			// The leg end: the walker has used up its route this tick, the order still holds the rest (the next leg is planned by the next step).
			{
				state::Fighter* g = fighterOf(sim, id);
				g->clearRoute(state::Fighter::kClearArrived);
			}
			std::ostringstream out;
			sim.save(out);
			const std::string bytes = out.str();
			std::istringstream in(bytes);
			sim.reload(in);
			test::require(sim.farOrder(id) != nullptr, "the far order was not restored by the load at a leg end");
			const Walk w = walk(900.0);
			const double off = std::hypot(w.endX - kGoalX, w.endZ - kGoalZ);
			std::cout << "ksim_far_move: after the leg-end reload ended " << off << " u from the click\n";
			test::require(off <= 10.0, "after the leg-end reload ended " + std::to_string(off) + " u from the goal, within 10 wanted");
		}

		// ---- far unwalkable ground: sets off, ends silently, the goal blocked ----
		{
			state::Fighter* f = fighterOf(sim, id);
			f->x = kGateX;
			f->y = kGateY;
			f->z = kGateZ;
			sim.run(40);
			// The world's east edge, far past the coast: no mesh within 300 u of the goal, in a zone that is never loaded.
			const double kSeaX = 140000.0;
			const double kSeaZ = kGateZ;
			order(kSeaX, kSeaZ, 1000.0, 2);
			sim.run(2);
			test::require(fighterOf(sim, id)->walking(), "the order to unwalkable far ground did not set off: it was refused");
			const Walk w = walk(2400.0);
			test::require(!fighterOf(sim, id)->walking(), "the walker is still walking after 2400 s");
			const double off = std::hypot(w.endX - kSeaX, w.endZ - kSeaZ);
			std::cout << "ksim_far_move: unwalkable goal: walked " << w.walked << " u, " << w.zones << " zone borders, stopped " << off << " u short\n";
			test::require(off > 300.0, "the walker reached the unwalkable goal");
			const auto* held = sim.farOrder(id);
			test::require(held == nullptr || held->blocked, "the goal was not ended as a blocked target");
		}
		std::cout << "ksim_far_move: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_far_move: " << e.what() << '\n';
		return 1;
	}
}
