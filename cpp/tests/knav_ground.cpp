// The ground navmesh generator (knav/ground.hpp, kenshi-k16.7.186) on zone 46.16 (Heft) and a 800 u patch of it: invariants of the output, and the
// face counts and wall times reported (not asserted) against the prototype's table (docs/reference/GROUND_NAVGEN.md). Never graded against the
// shipped mesh (user 2026-11-24).

#include "knav/ground.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

using P3 = std::array<double, 3>;
namespace ground = knav::ground;

constexpr double kMergeTolerance = 1e-4; // the generator's vertex merge
constexpr double kSlopeSlackDeg = 0.5;

struct Run
{
	knav::native::Mesh mesh;
	ground::Stats stats;
	ground::CutterCounts counts;
	std::vector<ground::Cutter> cutters;
	ground::Grid grid;
	double msGather = 0, msTotal = 0;
};

std::vector<P3> corners(const knav::native::Mesh& mesh, const knav::native::Face& f)
{
	std::vector<P3> c;
	for (std::uint32_t k = 0; k < f.numEdges; ++k)
		c.push_back(mesh.vertices[mesh.edges[f.startEdge + k].a]);
	return c;
}

Run build(const std::string& dataDir, const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1)
{
	Run r;
	ground::Params params;
	const auto t0 = std::chrono::steady_clock::now();
	r.mesh = ground::generateBox(dataDir, terrain, x0, z0, x1, z1, params, {}, true, &r.stats, &r.counts, &r.msGather);
	r.msTotal = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	r.grid = ground::makeGrid(terrain, x0, z0, x1, z1, params.stride);
	r.cutters = ground::gatherCutters(dataDir, terrain, r.grid.x0, r.grid.z0, r.grid.x0 + (r.grid.nx - 1) * r.grid.step, r.grid.z0 + (r.grid.nz - 1) * r.grid.step);
	return r;
}

// The ground under (x, z): the 36 u grid's own triangle (the diagonal along the smaller height step, as the generator splits its cells).
double groundAt(const ground::Grid& g, double x, double z)
{
	const double fx = (x - g.x0) / g.step, fz = (z - g.z0) / g.step;
	const int i = std::clamp(static_cast<int>(std::floor(fx)), 0, g.nx - 2), j = std::clamp(static_cast<int>(std::floor(fz)), 0, g.nz - 2);
	const double u = std::clamp(fx - i, 0.0, 1.0), v = std::clamp(fz - j, 0.0, 1.0);
	const double h00 = g.at(i, j), h10 = g.at(i + 1, j), h01 = g.at(i, j + 1), h11 = g.at(i + 1, j + 1);
	if (std::fabs(h00 - h11) <= std::fabs(h10 - h01)) // diagonal (0,0)-(1,1)
		return u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10) : h00 + u * (h11 - h01) + v * (h01 - h00);
	return u + v <= 1 ? h00 + u * (h10 - h00) + v * (h01 - h00) : h11 + (1 - u) * (h01 - h11) + (1 - v) * (h10 - h11);
}

// The solid spans of a cutter over (x, z): vertical hits on its triangles; an odd count is open at the bottom.
bool blocks(const ground::Cutter& c, double x, double z, double lo, double hi)
{
	if (x < c.min[0] || x > c.max[0] || z < c.min[2] || z > c.max[2])
		return false;
	std::vector<double> hits;
	for (std::size_t t = 0; t + 2 < c.tris.size(); t += 3)
	{
		const P3 &a = c.tris[t], &b = c.tris[t + 1], &d = c.tris[t + 2];
		const double det = (b[0] - a[0]) * (d[2] - a[2]) - (b[2] - a[2]) * (d[0] - a[0]);
		if (std::fabs(det) < 1e-12)
			continue;
		const double l1 = ((x - a[0]) * (d[2] - a[2]) - (z - a[2]) * (d[0] - a[0])) / det;
		const double l2 = ((b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0])) / det;
		if (l1 < 0 || l2 < 0 || l1 + l2 > 1)
			continue;
		hits.push_back(a[1] + l1 * (b[1] - a[1]) + l2 * (d[1] - a[1]));
	}
	std::sort(hits.begin(), hits.end());
	if (c.projected && !hits.empty())
		return true;
	if (hits.size() % 2)
		hits.insert(hits.begin(), -1e30);
	for (std::size_t k = 0; k + 1 < hits.size(); k += 2)
		if (hits[k] < hi && hits[k + 1] > lo)
			return true;
	for (const double h : hits)
		if (h > lo && h < hi)
			return true;
	return false;
}

void invariants(const char* name, const Run& r)
{
	const knav::native::Mesh& m = r.mesh;
	std::size_t underCutter = 0, needleUnder = 0, steep = 0, topSteep = 0;
	double worstSlope = 0, worstTopSlope = 0;
	test::require(!m.faces.empty(), std::string(name) + ": the ground has faces");
	std::set<std::uint32_t> tops(r.stats.topFaceIndex.begin(), r.stats.topFaceIndex.end());
	for (std::uint32_t fi = 0; fi < m.faces.size(); ++fi)
	{
		const knav::native::Face& f = m.faces[fi];
		const std::vector<P3> c = corners(m, f);
		test::require(c.size() >= 3, "a face has three corners or more");
		if (tops.count(fi))
		{
			// a walkable top (section 12): convex, at most the building slope (60 degrees), over the terrain by the cut rule
			double nx = 0, ny = 0, nz = 0, minTurn = 0, maxTurn = 0;
			for (std::size_t k = 0; k < c.size(); ++k)
			{
				const P3 &a = c[k], &b = c[(k + 1) % c.size()], &d = c[(k + 2) % c.size()];
				const double turn = (b[0] - a[0]) * (d[2] - b[2]) - (b[2] - a[2]) * (d[0] - b[0]);
				minTurn = std::min(minTurn, turn);
				maxTurn = std::max(maxTurn, turn);
				nx += (a[1] - b[1]) * (a[2] + b[2]);
				ny += (a[2] - b[2]) * (a[0] + b[0]);
				nz += (a[0] - b[0]) * (a[1] + b[1]);
			}
			if (!(minTurn >= -1e-6 || maxTurn <= 1e-6))
			{
				std::cout << "  non-convex top " << fi << ", turns " << minTurn << " .. " << maxTurn << ", " << c.size() << " corners:";
				for (const P3& p : c)
					std::cout << " (" << p[0] << "," << p[1] << "," << p[2] << ")";
				std::cout << "\n";
			}
			test::require(minTurn >= -1e-6 || maxTurn <= 1e-6, "a top is convex");
			const double slope = std::acos(std::fabs(ny) / std::sqrt(nx * nx + ny * ny + nz * nz)) * 180.0 / 3.14159265358979;
			worstTopSlope = std::max(worstTopSlope, slope);
			topSteep += slope > 60 + kSlopeSlackDeg ? 1 : 0;
			continue;
		}
		// no repeated corner, no edge under the merge tolerance
		for (std::size_t k = 0; k < c.size(); ++k)
		{
			const P3 &a = c[k], &b = c[(k + 1) % c.size()];
			if (std::hypot(a[0] - b[0], a[2] - b[2]) <= kMergeTolerance)
				std::cout << "  short edge in face " << fi << " (" << c.size() << " corners) at " << a[0] << " " << a[1] << " " << a[2] << " -> " << b[0] << " " << b[1] << " " << b[2] << "\n";
			test::require(std::hypot(a[0] - b[0], a[2] - b[2]) > kMergeTolerance, "no edge under the merge tolerance (no repeated corner next to itself)");
			for (std::size_t l = k + 1; l < c.size(); ++l)
				test::require(std::hypot(a[0] - c[l][0], a[2] - c[l][2]) > kMergeTolerance, "no corner repeats in a face");
		}
		// convex
		double cx = 0, cz = 0, nx = 0, ny = 0, nz = 0, minTurn = 0, maxTurn = 0, area2 = 0, perimeter = 0;
		for (std::size_t k = 0; k < c.size(); ++k)
		{
			const P3 &a = c[k], &b = c[(k + 1) % c.size()], &d = c[(k + 2) % c.size()];
			const double turn = (b[0] - a[0]) * (d[2] - b[2]) - (b[2] - a[2]) * (d[0] - b[0]);
			area2 += a[0] * b[2] - b[0] * a[2];
			perimeter += std::hypot(b[0] - a[0], b[2] - a[2]);
			minTurn = std::min(minTurn, turn);
			maxTurn = std::max(maxTurn, turn);
			cx += a[0];
			cz += a[2];
			nx += (a[1] - b[1]) * (a[2] + b[2]);
			ny += (a[2] - b[2]) * (a[0] + b[0]);
			nz += (a[0] - b[0]) * (a[1] + b[1]);
		}
		test::require(minTurn >= -1e-6 || maxTurn <= 1e-6, "faces are convex (every corner turns the same way)");
		cx /= c.size();
		cz /= c.size();
		const double slope = std::acos(std::fabs(ny) / std::sqrt(nx * nx + ny * ny + nz * nz)) * 180.0 / 3.14159265358979;
		worstSlope = std::max(worstSlope, slope);
		steep += slope > 40 + kSlopeSlackDeg ? 1 : 0;
		// no face under a cutter between ground + 0.1 and + 18 (at its centre; the ground is the grid's, not the simplified face's)
		const double g = groundAt(r.grid, cx, cz);
		for (const ground::Cutter& cut : r.cutters)
			if (blocks(cut, cx, cz, g + 0.1, g + 18))
			{
				// a needle (inradius under 1 u: the simplifier's border rule moves an outline by about that) can reach under the cutter it abuts
				if (std::fabs(area2) / perimeter < 1.0)
				{
					++needleUnder;
					break;
				}
				if (++underCutter <= 5)
					std::cout << "  under a cutter: face of " << c.size() << " corners, centre " << cx << " " << cz << ", grid ground " << g << ", face corner heights "
						<< c[0][1] << " " << c[1][1] << " " << c[2][1] << ", corners (" << c[0][0] << "," << c[0][2] << ") (" << c[1][0] << "," << c[1][2] << ") (" << c[2][0] << "," << c[2][2] << ")" << ", cutter min y " << cut.min[1] << " max y " << cut.max[1] << (cut.projected ? " projected" : "") << "\n";
				break;
			}
	}
	std::cout << name << ": tops " << tops.size() << ", worst top slope " << worstTopSlope << " deg\n";
	test::require(topSteep == 0, std::string(name) + ": no top over 60 degrees");
	std::cout << name << ": worst face slope " << worstSlope << " deg, faces over 40 deg " << steep << ", face centres under a cutter " << underCutter << " (needles, inradius < 1 u: " << needleUnder << ")\n";
	test::require(steep == 0, std::string(name) + ": no face over 40 degrees");
	test::require(underCutter == 0, std::string(name) + ": no face under a cutter between ground + 0.1 and + 18 u");
}

void report(const char* name, const Run& r, const char* prototype)
{
	const ground::Stats& s = r.stats;
	std::cout << name << ": grid " << r.grid.nx << " x " << r.grid.nz << ", walkable tris " << s.walkableTris << ", cutters " << r.cutters.size() << " (building shapes "
		<< r.counts.buildingShapes << ", mask " << r.counts.maskShapes << ", foliage " << r.counts.foliage << ", doors skipped " << r.counts.doorsSkipped << "), ground tris cut "
		<< s.cutGroundTris << ", pieces kept / dropped " << s.piecesKept << " / " << s.piecesDropped << ", trimmed tips " << s.trimmedTips << " (" << s.trimmedArea << " u2, "
		<< s.culled.size() << " tris), faces " << r.mesh.faces.size() << " (tops " << s.topFaces << ", dropped " << s.topDroppedFaces << ", links " << s.topLinks
		<< ", ms tops " << s.msTops << " + link " << s.msLink << ")" << "\n  ms: gather " << r.msGather << ", cut " << s.msCut << ", trim " << s.msTrim << ", simplify " << s.msSimplify
		<< ", total " << r.msTotal << "; prototype: " << prototype << "\n";
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const auto zoneBox = [](int zi, int zj, double& x0, double& z0) { x0 = kworld::kWorldMin + zi * kworld::kZoneSize; z0 = kworld::kWorldMin + zj * kworld::kZoneSize; };

		double x0, z0;
		zoneBox(46, 16, x0, z0);
		const Run zone = build(dataDir, terrain, x0, z0, x0 + kworld::kZoneSize, z0 + kworld::kZoneSize);
		report("zone 46.16", zone, "1930 cutters, 3730 ground tris cut, 57523 / 72252 pieces, 49 tips, 1811 faces; cut 374, trim 161, simplify 711 ms");
		invariants("zone 46.16", zone);

		// the Heft patch: +-400 u around the slaver camp longhouse
		const Run heft = build(dataDir, terrain, 68339 - 400, -71264 - 400, 68339 + 400, -71264 + 400);
		report("Heft patch", heft, "273 cutters, 559 ground tris cut, 7645 / 9781 pieces, 9 tips, 202 faces; cut 39, simplify 84 ms");
		invariants("Heft patch", heft);

		// rim vertices of both neighbouring windows: zone 47.16 shares x = 46.16's east edge, 46.17 its south edge
		for (const int dz : { 0, 1 })
		{
			double nx0, nz0;
			zoneBox(46 + (dz ? 0 : 1), 16 + dz, nx0, nz0);
			const Run nb = build(dataDir, terrain, nx0, nz0, nx0 + kworld::kZoneSize, nz0 + kworld::kZoneSize);
			const double edge = dz ? z0 + kworld::kZoneSize : x0 + kworld::kZoneSize;
			const int axis = dz ? 2 : 0;
			const auto rim = [&](const Run& run)
			{
				std::set<std::array<long long, 3>> out;
				for (const P3& v : run.mesh.vertices)
					if (std::fabs(v[axis] - edge) < 1e-6)
						out.insert({ std::llround(v[0] * 1000), std::llround(v[1] * 1000), std::llround(v[2] * 1000) });
				return out;
			};
			knav::native::Mesh sa = zone.mesh, sb = nb.mesh;
			const std::size_t inserted = knav::ground::stitchRims(sa, sb, axis, edge);
			const auto a = rim(zone), b = rim(nb);
			Run za = zone, zb = nb;
			za.mesh = sa;
			zb.mesh = sb;
			const auto sta = rim(za), stb = rim(zb);
			std::cout << "  stitched: " << inserted << " vertices inserted; rim vertices now " << sta.size() << " / " << stb.size() << "\n";
			// a T-junction: a vertex of one window on the seam that lies strictly inside a boundary edge of the other
			const auto tjunctions = [&](const knav::native::Mesh& m, const knav::native::Mesh& o)
			{
				const int other = axis == 0 ? 2 : 0;
				std::size_t n = 0;
				std::vector<std::array<double, 3>> rimV;	// the ends of boundary edges on the seam line
				for (const auto& e : m.edges)
					for (const std::uint32_t x : { e.a, e.b })
						if (e.oppositeEdge < 0 && std::fabs(m.vertices[x][axis] - edge) <= 1e-6)
							rimV.push_back(m.vertices[x]);
				std::set<std::pair<long long, long long>> own;	// o's own vertices on the line: a vertex it already has is no junction (a sliver of two of its surfaces)
				for (const auto& x : o.vertices)
					if (std::fabs(x[axis] - edge) <= 1e-6)
						own.insert({ std::llround(x[0] * 100), std::llround(x[2] * 100) });
				for (const auto& v : rimV)
				{
					if (own.count({ std::llround(v[0] * 100), std::llround(v[2] * 100) }))
						continue;
					for (const auto& e : o.edges)
					{
						const auto& u = o.vertices[e.a], & w = o.vertices[e.b];
						if (e.oppositeEdge >= 0 || std::fabs(u[axis] - edge) > 1e-6 || std::fabs(w[axis] - edge) > 1e-6)
							continue;
						const double lo = std::min(u[other], w[other]) + 1e-3, hi = std::max(u[other], w[other]) - 1e-3;
						const double t = (v[other] - u[other]) / (w[other] - u[other]);
						if (v[other] > lo && v[other] < hi && std::fabs(u[1] + t * (w[1] - u[1]) - v[1]) <= 5.0)
						{
							++n;
							break;
						}
					}
				}
				return n;
			};
			const std::size_t before = tjunctions(zone.mesh, nb.mesh) + tjunctions(nb.mesh, zone.mesh), after = tjunctions(sa, sb) + tjunctions(sb, sa);
			std::cout << "  T-junctions across the seam: " << before << " -> " << after << "\n";
			test::require(after == 0, "after stitchRims no T-junction across the seam between two neighbouring windows");
			std::cout << "rim " << (dz ? "46.16 / 46.17" : "46.16 / 47.16") << ": " << a.size() << " vertices vs " << b.size() << "\n";
			std::size_t shared = 0, onlyA = 0, onlyB = 0;
			for (const auto& v : a)
				(b.count(v) ? shared : onlyA)++;
			for (const auto& v : b)
				onlyB += a.count(v) ? 0 : 1;
			std::cout << "  shared " << shared << ", only in 46.16 " << onlyA << ", only in the neighbour " << onlyB << "\n";
			// Not all identical (every rim sample stays now; what differs is the cut points and clipped tops, which exist only where a window's own
			// ground or top is kept there); stitchRims (kenshi-k16.7.188 item 4) closes the T-junctions, asserted above.
			test::require(shared > 0 && shared * 10 >= std::min(a.size(), b.size()) * 8, "two neighbouring windows share at least 80 % of their rim vertices");
		}
		std::cout << "knav_ground ok\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
