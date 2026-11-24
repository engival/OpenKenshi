// kenshi-k16.6.720 (user direction on kenshi-k16.6.719): the road follower walks straight to the furthest road-route point inside a radius R
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

#include <algorithm>
#include <cmath>
#include <cstdlib>
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
			std::cout << "ksim_road_lookahead: SKIPPED: needs the world navmesh with the Heft region (pass its directory)\n";
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
				std::cout << "ksim_road_lookahead: order markers " << held->markers.size() << ':';
				for (std::size_t i = 0; i < held->markers.size() && i < 14; ++i)
					std::cout << " (" << static_cast<int>(held->markers[i].x) << ',' << static_cast<int>(held->markers[i].z) << ')';
				std::cout << '\n';
			} else {
				std::cout << "ksim_road_lookahead: no far order held (plain walk)\n";
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

		// The road and marker index: roads are named by the data's numbering; find the one with a marker at (x, z).
		const auto findMarker = [&](double x, double z, const kworld::RoadNetwork::Road*& road, std::size_t& at)
		{
			for (const kworld::RoadNetwork::Road& r : net.roads())
			{
				for (std::size_t i = 0; i < r.markers.size(); ++i)
				{
					if (std::hypot(r.markers[i].x - x, r.markers[i].z - z) < 2.0)
					{
						road = &r;
						at = i;
						return true;
					}
				}
			}
			return false;
		};

		// ---- (a) 925 #2 zig-zag ----
		{
			const kworld::RoadNetwork::Road* road = nullptr;
			std::size_t at = 0;
			test::require(findMarker(-37250.4, -67029.5, road, at), "no road marker at 925 #1");
			test::require(at >= 1 && road->markers.size() > at + 4, "925 markers #0..#5 not where expected");
			const std::size_t i0 = at - 1;
			const auto& m = road->markers;
			std::cout << "ksim_road_lookahead: (a) road markers " << i0 << ".." << i0 + 5 << " of " << m.size() << ", #2 at " << m[i0 + 2].x << ',' << m[i0 + 2].z << '\n';
			const double zx = m[i0 + 2].x;
			const double zz = m[i0 + 2].z;
			double minDist[2];
			for (int rule = 0; rule < 2; ++rule)
			{
				// A far walk along it: from #0 to the road's far end past #5 (over 1000 u).
				const std::size_t gi = std::min(m.size() - 1, i0 + 12);
				const Trail tr = walk(rule == 0 ? 3.0 : 0.0, m[i0].x, m[i0].y, m[i0].z, m[gi].x, m[gi].z, m[gi].y, 600.0);
				double best = 1.0e30;
				for (const Pt& p : tr.pts)
					best = std::min(best, std::hypot(p.first - zx, p.second - zz));
				minDist[rule] = best;
				std::cout << "ksim_road_lookahead: (a) " << (rule == 0 ? "look-ahead" : "exe rule") << ": closest approach to #2 " << best << " u, walked "
					<< lengthOf(tr.pts) << " u in " << tr.seconds << " s\n";
			}
			// The world bake joins the steep polygon under #2 to its neighbour across the shared edge (kenshi-k16.7.167): the exe rule reaches the spur
			// (within 150 u of #2), the look-ahead cuts it off (never within 250 u). On the shipped navtiles (the default runtime mesh) the mesh ends
			// ~316 u short of #2 and the way back is the spur itself, so neither rule gets near and only parity holds (kenshi-k16.6.720).
			const char* bake = std::getenv("KSIM_NAVWORLD_BAKE");
			if (bake != nullptr && *bake != '\0')
			{
				test::require(minDist[1] < 150.0, "the exe rule did not reach the 925 #2 spur (" + std::to_string(minDist[1]) + " u from #2)");
				test::require(minDist[0] > 250.0, "the look-ahead walk did not cut the 925 #2 spur (" + std::to_string(minDist[0]) + " u from #2)");
			} else {
				test::require(minDist[0] >= minDist[1] - 50.0, "look-ahead walk came closer to #2 (" + std::to_string(minDist[0]) + " u) than the exe rule (" + std::to_string(minDist[1]) + " u)");
			}
		}

		// ---- (b) 152 switchback near Rebirth ----
		{
			const kworld::RoadNetwork::Road* road = nullptr;
			std::size_t at = 0;
			test::require(findMarker(-50711.9, -55038.5, road, at), "no road marker at 152 #21");
			test::require(at >= 2 && road->markers.size() > at + 8, "152 markers #19..#29 not where expected");
			const std::size_t i0 = at - 2;
			const auto& m = road->markers;
			const double width = road->width > 0.0 ? road->width : 100.0;
			const std::size_t gi = std::min(m.size() - 1, i0 + 14);
			std::vector<Pt> line;
			for (std::size_t i = i0; i <= gi; ++i)
				line.push_back({m[i].x, m[i].z});
			const Trail tr = walk(3.0, m[i0].x, m[i0].y, m[i0].z, m[gi].x, m[gi].z, m[gi].y, 600.0);
			double worst = 0.0;
			double climb = 0.0;
			for (const Pt& p : tr.pts)
			{
				worst = std::max(worst, lineDist(line, p.first, p.second));
			}
			climb = lengthOf(tr.pts);
			std::cout << "ksim_road_lookahead: (b) road width " << width << ", worst trail distance to markers #19..#29 " << worst << " u, walked " << climb << " u, ended "
				<< std::hypot(tr.endX - m[gi].x, tr.endZ - m[gi].z) << " u from the goal\n";
			test::require(worst <= 2.0 * width + 25.0, "the switchback walk strayed " + std::to_string(worst) + " u from the markers");
			test::require(std::hypot(tr.endX - m[gi].x, tr.endZ - m[gi].z) < 300.0, "the switchback walk did not reach the goal");
		}

		// ---- (c) the Heft walk ----
		{
			std::vector<Pt> orig;
			if (argc > 2)
			{
				std::ifstream in(argv[2]);
				std::string line;
				std::getline(in, line);
				while (std::getline(in, line))
				{
					std::istringstream ss(line);
					double t, plan, x, z;
					if (ss >> t >> plan >> x >> z)
						orig.push_back({x, z});
				}
			}
			const double origLen = lengthOf(orig);
			const state::Fighter* f0 = fighterOf(sim, id);
			(void)f0;
			for (int rule = 0; rule < 2; ++rule)
			{
				const Trail tr = walk(rule == 0 ? 3.0 : 0.0, 67047.0, 1269.0, -72097.0, 65406.0, -50249.0, 1088.0, 1500.0);
				std::vector<double> dist;
				for (const Pt& p : tr.pts)
					dist.push_back(orig.empty() ? 0.0 : lineDist(orig, p.first, p.second));
				std::sort(dist.begin(), dist.end());
				const double len = lengthOf(tr.pts);
				const double end = std::hypot(tr.endX - 65406.0, tr.endZ + 50249.0);
				std::cout << "ksim_road_lookahead: (c) " << (rule == 0 ? "look-ahead" : "exe rule") << ": " << len << " u (original " << origLen << ") in " << tr.seconds
					<< " s (original 255.4), ended " << end << " u from the goal; trail to the original's: median " << dist[dist.size() / 2] << " p90 "
					<< dist[dist.size() * 9 / 10] << " max " << dist.back() << '\n';
				test::require(end < 300.0, "the Heft walk did not arrive");
				if (rule == 0)
					test::require(len < 1.25 * (origLen > 0.0 ? origLen : 28618.0), "the look-ahead Heft walk is " + std::to_string(len) + " u long");
			}
		}
		const auto& st = sim.roadGoalStats();
		std::cout << "ksim_road_lookahead: look-ahead goals " << st.goals << ", off-centre " << st.offCentre << ", mean shift "
			<< (st.offCentre ? st.offSum / st.offCentre : 0.0) << " u, max " << st.offMax << " u\n";
		test::require(st.goals > 0, "no look-ahead goal was planned");
		std::cout << "ksim_road_lookahead: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_road_lookahead: " << e.what() << '\n';
		return 1;
	}
}
