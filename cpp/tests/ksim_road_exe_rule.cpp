// kenshi-k16.6.722: the R=0 road follower is the exe's hand-over (rwh.268 Q1): the Heft walk's trail passes within the road width of every marker of
// the order's chain. Derived from ksim_road_lookahead's walk rig. Needs the world navmesh with the Heft region.
// (old header) kenshi-k16.6.720 (user direction on kenshi-k16.6.719): the road follower walks straight to the furthest road-route point inside a radius R
// (a zone, not the marker) instead of marker by marker. Walked from a placed fighter with the look-ahead on (3 road widths) and off (the exe
// rule, rwh.295 / PATH_WALK 3d):
//  (a) road 925-Newwworld #0..#5: #2 sits off the #1 -> #3 line; the look-ahead walk never comes within 150 u of it, the exe rule does;
//  (b) road 152-Newwworld #19..#29, the switchback near Rebirth: the look-ahead walk still climbs it, within two road widths of the markers;
//  (c) the Heft gate -> stone camp walk, both rules: length, time, and the trail's distance to the original's (argv[2] = rwh295 frames tsv).
// Needs the world navmesh with the Heft region (pass its directory); the walks teleport the fighter to the road.
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"

#include "kworld/roads.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <set>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ksim;

namespace {

using Pt = std::pair<double, double>;

double segDist(double px, double pz, double ax, double az, double bx, double bz)
{
	const double dx = bx - ax;
	const double dz = bz - az;
	const double len2 = dx * dx + dz * dz;
	const double t = len2 > 0.0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / len2, 0.0, 1.0) : 0.0;
	return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

double lineDist(const std::vector<Pt>& line, double x, double z)
{
	double best = 1.0e30;
	for (std::size_t i = 0; i + 1 < line.size(); ++i)
		best = std::min(best, segDist(x, z, line[i].first, line[i].second, line[i + 1].first, line[i + 1].second));
	return best;
}

double lengthOf(const std::vector<Pt>& line)
{
	double len = 0.0;
	for (std::size_t i = 0; i + 1 < line.size(); ++i)
		len += std::hypot(line[i + 1].first - line[i].first, line[i + 1].second - line[i].second);
	return len;
}

struct Trail
{
	std::vector<Pt> pts;
	double seconds = 0.0;
	double endX = 0.0;
	double endZ = 0.0;
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
			std::cout << "ksim_road_exe_rule: SKIPPED: needs the world navmesh with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world);
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 1);
		test::require(!placed.players.empty(), "gate_heft places no player");
		const std::uint64_t id = placed.players.front();
		sim.run(40);
		const kworld::RoadNetwork net = kworld::RoadNetwork::load(KENSHI_DATA);
		std::uint32_t seq = 10;
		std::vector<kworld::RoadNetwork::Marker> orderMarkers;

		// Walks a far order from (sx, sz) to the goal with the look-ahead scale; the trail every 5 ticks.
		const auto walk = [&](double scale, double sx, double sy, double sz, double gx, double gz, double gy, double maxSeconds) -> Trail
		{
			sim.setRoadLookahead(scale);
			{
				state::Fighter* f = fighterOf(sim, id);
				f->x = sx;
				f->y = sy;
				f->z = sz;
			}
			sim.run(40);
			sim.enqueue(Command::fighterMoveFar(state::Id{id}, gx, gz, gy, ++seq));
			sim.run(2);
			if (const auto* held = sim.farOrder(id))
			{
				orderMarkers = held->markers;
				std::cout << "ksim_road_exe_rule: order markers " << held->markers.size() << ':';
				for (std::size_t i = 0; i < held->markers.size() && i < 14; ++i)
					std::cout << " (" << static_cast<int>(held->markers[i].x) << ',' << static_cast<int>(held->markers[i].z) << ')';
				std::cout << '\n';
			} else {
				std::cout << "ksim_road_exe_rule: no far order held (plain walk)\n";
			}
			Trail tr;
			tr.pts.push_back({sx, sz});
			for (std::uint64_t tick = 0; tick < static_cast<std::uint64_t>(maxSeconds * 20.0); ++tick)
			{
				sim.run(1);
				state::Fighter* f = fighterOf(sim, id);
				if (tick % 5 == 0)
					tr.pts.push_back({f->x, f->z});
				if (tick % 20 == 0)
				{
					f->blood = 150.0; // the scenario's walker starts wounded: the walk is what is measured
					f->bleeding = 0.0;
					f->hunger = 300.0;
					for (state::FighterPart& part : f->parts)
					{
						part.health = part.maxHealth;
						part.stun = 0.0;
					}
					f->koTimer = 0.0;
				}
				tr.seconds = static_cast<double>(tick + 1) / 20.0;
				if (tick > 60 && !f->walking() && sim.farOrder(id) == nullptr)
					break;
			}
			const state::Fighter* f = fighterOf(sim, id);
			tr.endX = f->x;
			tr.endZ = f->z;
			return tr;
		};

		const Trail tr = walk(0.0, 67047.0, 1269.0, -72097.0, 65406.0, -50249.0, 1088.0, 1500.0);
		test::require(orderMarkers.size() >= 56, "the Heft order's chain has " + std::to_string(orderMarkers.size()) + " markers");
		std::size_t missed = 0;
		// Chain #6, #26, #27: our mesh ends short of them and outside their width, so the exe's own rule applies ('Skip road node'): mesh holes (kenshi-k16.7.154).
		const std::set<std::size_t> meshHoles = {6, 26, 27};
		for (std::size_t i = 0; i < orderMarkers.size(); ++i)
		{
			if (meshHoles.count(i) != 0)
				continue;
			if (std::hypot(orderMarkers[i].x - 65406.0, orderMarkers[i].z + 50249.0) < 547.7)
				continue; // within 547.7 u of the goal the exe drops the route (rwh.268)
			const double width = orderMarkers[i].width > 0.0 ? orderMarkers[i].width : 100.0;
			const double d = lineDist(tr.pts, orderMarkers[i].x, orderMarkers[i].z);
			if (d >= width)
			{
				++missed;
				std::cout << "ksim_road_exe_rule: marker " << i << " (" << orderMarkers[i].x << ',' << orderMarkers[i].z << ") missed by " << d << " u (width " << width << ")\n";
			}
		}
		std::cout << "ksim_road_exe_rule: " << orderMarkers.size() << " markers, " << missed << " missed, walked " << lengthOf(tr.pts) << " u, ended "
			<< std::hypot(tr.endX - 65406.0, tr.endZ + 50249.0) << " u from the goal\n";
		test::require(missed == 0, std::to_string(missed) + " Heft markers not passed within the road width");
		test::require(std::hypot(tr.endX - 65406.0, tr.endZ + 50249.0) < 300.0, "the Heft walk did not arrive");
		std::cout << "ksim_road_exe_rule: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_road_exe_rule: " << e.what() << '\n';
		return 1;
	}
}
