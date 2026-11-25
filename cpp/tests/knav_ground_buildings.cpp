// Buildings' zone sides on our ground (GROUND_NAVGEN.md slice 3, kenshi-k16.7.188): surfgen's zone-side faces of a masked building join the
// ground as prebuilt tops (terrain cut, links, prune to the ground or the interior seam). The kenshi-5uua fixtures first (a hump splits a ramp, a
// ramp foot sunk below ground), then the Heft slaver camp's building nearest the longhouse, the Hub bar and the Storm House.

#include "knav/area.hpp"
#include "knav/ground.hpp"
#include "knav/surfgen.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{

namespace ground = knav::ground;
using knav::native::Mesh;
using P3 = std::array<double, 3>;

// ---- synthetic fixtures ----

// A mesh of convex polygons; the edges whose midpoint satisfies `seam` are marked linked (the interior seam).
template <typename F>
Mesh meshOf(const std::vector<std::vector<P3>>& polys, F seam)
{
	Mesh m;
	for (const auto& poly : polys)
	{
		knav::native::Face f;
		f.startEdge = (std::uint32_t)m.edges.size();
		f.numEdges = (std::uint16_t)poly.size();
		for (std::size_t k = 0; k < poly.size(); ++k)
		{
			const P3& a = poly[k], & b = poly[(k + 1) % poly.size()];
			knav::native::Edge e;
			e.a = (std::uint32_t)m.vertices.size() + (std::uint32_t)k;
			e.b = (std::uint32_t)m.vertices.size() + (std::uint32_t)((k + 1) % poly.size());
			e.linked = seam(P3{ (a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2 });
			m.edges.push_back(e);
		}
		for (const P3& p : poly)
			m.vertices.push_back(p);
		m.faces.push_back(f);
	}
	return m;
}

// The ramp: x from x0 to x1, z from z0 to z1, height h(x); counter-clockwise seen from above (x right, z up the page: cross2 > 0) is what surfgen
// writes; zoneSideCutter orients the triangles itself.
std::vector<std::vector<P3>> rampStrips(double x0, double x1, double z0, double z1, double (*h)(double), double strip)
{
	std::vector<std::vector<P3>> out;
	for (double x = x0; x < x1 - 1e-9; x += strip)
	{
		const double xe = std::min(x1, x + strip);
		out.push_back({ { x, h(x), z0 }, { xe, h(xe), z0 }, { xe, h(xe), z1 }, { x, h(x), z1 } });
	}
	return out;
}

// The solid under the ramp (a wedge down to `bottom`), triangles wound outward: the building's own shape, which cuts the ground.
ground::Cutter wedge(double x0, double x1, double z0, double z1, double (*h)(double), double bottom, const std::string& label)
{
	ground::Cutter c;
	c.label = label;
	const auto tri = [&](const P3& a, const P3& b, const P3& d, const P3& inside)
	{
		const P3 u = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
		const P3 n = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
		const P3 mid = { (a[0] + b[0] + d[0]) / 3 - inside[0], (a[1] + b[1] + d[1]) / 3 - inside[1], (a[2] + b[2] + d[2]) / 3 - inside[2] };
		// the cooker's winding is the one the ground reads as outward: up-facing surfaces have cross2 < 0 (ground.cpp tops())
		const bool outward = n[0] * mid[0] + n[1] * mid[1] + n[2] * mid[2] > 0;
		const bool upFacing = (b[0] - a[0]) * (d[2] - a[2]) - (b[2] - a[2]) * (d[0] - a[0]) < 0;
		(void)upFacing;
		c.tris.push_back(a);
		c.tris.push_back(outward ? b : d);
		c.tris.push_back(outward ? d : b);
	};
	const P3 in = { (x0 + x1) / 2, (h(x0) + h(x1)) / 2 + bottom / 2, (z0 + z1) / 2 };
	const P3 a0 = { x0, h(x0), z0 }, a1 = { x1, h(x1), z0 }, a2 = { x1, h(x1), z1 }, a3 = { x0, h(x0), z1 };
	const P3 b0 = { x0, bottom, z0 }, b1 = { x1, bottom, z0 }, b2 = { x1, bottom, z1 }, b3 = { x0, bottom, z1 };
	for (const auto& q : { std::array<P3, 4>{ a0, a1, a2, a3 }, { b0, b1, b2, b3 }, { a0, a1, b1, b0 }, { a1, a2, b2, b1 }, { a2, a3, b3, b2 }, { a3, a0, b0, b3 } })
	{
		tri(q[0], q[1], q[2], in);
		tri(q[0], q[2], q[3], in);
	}
	c.min = { 1e30, 1e30, 1e30 };
	c.max = { -1e30, -1e30, -1e30 };
	for (const P3& p : c.tris)
		for (int i = 0; i < 3; ++i)
		{
			c.min[i] = std::min(c.min[i], p[i]);
			c.max[i] = std::max(c.max[i], p[i]);
		}
	return c;
}

double at(const std::map<std::string, double>& m, const std::string& k)
{
	const auto it = m.find(k);
	return it == m.end() ? 0.0 : it->second;
}

struct Out
{
	Mesh mesh;
	ground::Stats stats;
	std::set<std::uint32_t> tops;
};

Out run(const ground::Grid& grid, const std::vector<ground::Cutter>& cutters)
{
	Out o;
	o.mesh = ground::generate(grid, { { 8, 8 } }, cutters, {}, &o.stats);
	o.tops.insert(o.stats.topFaceIndex.begin(), o.stats.topFaceIndex.end());
	return o;
}

// Top faces whose component over links reaches a ground face / count of top faces that link to ground directly.
std::size_t groundLinks(const Out& o)
{
	std::size_t n = 0;
	for (const std::uint32_t f : o.tops)
		for (std::uint32_t k = 0; k < o.mesh.faces[f].numEdges; ++k)
		{
			const auto& e = o.mesh.edges[o.mesh.faces[f].startEdge + k];
			n += e.oppositeFace >= 0 && !o.tops.count((std::uint32_t)e.oppositeFace);
		}
	return n;
}

double rampUp(double x) { return (x - 20) * 0.35; }
double rampSunk(double x) { return -6 + (x - 20) * 0.4; }

ground::Grid flatGrid(double hump)
{
	ground::Grid g;
	g.nx = g.nz = 9;
	g.x0 = g.z0 = 0;
	g.step = 18;
	g.y.assign(81, 0);
	if (hump > 0)
		for (const int i : { 3, 4 })
			for (const int j : { 3, 4 })
				g.y[(std::size_t)j * 9 + i] = hump;
	return g;
}

// every top face lies at least topTerrainTol under no terrain: a face centre is not under the (flat or humped) ground by more than 0.25
bool nothingUnderGround(const Out& o, const ground::Grid& g)
{
	for (const std::uint32_t f : o.tops)
	{
		const auto& face = o.mesh.faces[f];
		for (std::uint32_t k = 0; k < face.numEdges; ++k)
		{
			const P3& v = o.mesh.vertices[o.mesh.edges[face.startEdge + k].a];
			const int i = std::clamp((int)std::floor((v[0] - g.x0) / g.step), 0, g.nx - 2), j = std::clamp((int)std::floor((v[2] - g.z0) / g.step), 0, g.nz - 2);
			double top = -1e9;
			for (const int di : { 0, 1 })
				for (const int dj : { 0, 1 })
					top = std::max(top, g.at(i + di, j + dj));
			if (v[1] < -0.3 && top <= 0)	// flat ground at 0: a corner under it
				return false;
		}
	}
	return true;
}

void synthetic()
{
	// kenshi-5uua fixture 1: a hump splits the ramp. The foot reaches the ground; the head piece beyond the hump reaches only the seam.
	{
		const ground::Grid g = flatGrid(25);
		const auto strips = rampStrips(20, 80, 60, 76, rampUp, 4);
		const auto seamAtHead = [](const P3& m) { return std::fabs(m[0] - 80) < 1e-6; };
		for (const bool withSeam : { true, false })
		{
			const ground::Cutter solid = wedge(20, 80, 60, 76, rampUp, -8, "ramp");
			ground::Cutter side = ground::zoneSideCutter(meshOf(strips, seamAtHead), "ramp");
			if (!withSeam)
				side.seam.clear();
			const Out o = run(g, { solid, side });
			const double kept = at(o.stats.topKept, "ramp"), dropped = at(o.stats.topDropped, "ramp");
			std::cout << "hump splits the ramp, seam " << withSeam << ": kept " << kept << " u2, dropped " << dropped << " u2, ground links " << groundLinks(o) << "\n";
			test::require(groundLinks(o) > 0, "the ramp's foot joins the ground");
			test::require(kept > 250, "the foot piece stays (about 330 u2)");
			if (withSeam)
				test::require(dropped < 1e-6 && kept > 300, "the head piece touching the interior seam stays");
			else
				test::require(dropped > 30 && dropped < 100, "the head piece the hump cut off, reaching neither ground nor seam, is dropped and reported (about 64 u2)");
		}
	}
	// kenshi-5uua fixture 2: the foot sunk below the ground: the buried part is cut at the terrain, the rest links to the ground at the cut.
	{
		const ground::Grid g = flatGrid(0);
		const ground::Cutter solid = wedge(20, 80, 60, 76, rampSunk, -12, "sunk");
		const ground::Cutter side = ground::zoneSideCutter(meshOf(rampStrips(20, 80, 60, 76, rampSunk, 4), [](const P3& m) { return std::fabs(m[0] - 80) < 1e-6; }), "sunk");
		const Out o = run(g, { solid, side });
		const double kept = at(o.stats.topKept, "sunk"), dropped = at(o.stats.topDropped, "sunk");
		std::cout << "ramp foot sunk: kept " << kept << " u2, dropped " << dropped << " u2, ground links " << groundLinks(o) << "\n";
		test::require(nothingUnderGround(o, g), "no ramp face lies under the terrain");
		test::require(groundLinks(o) > 0 && dropped < 1e-6, "the ramp joins the ground where it comes out of it, nothing else is dropped");
		test::require(kept > 600 && kept < 760, "the ramp's emerged part stays (about 710 u2)");
	}
}

// ---- real buildings ----

struct Site
{
	const char* name;
	double x, z;	// where to look
	const char* buildingId;	// a substring of the placement's building record id, or null: the nearest masked building
	double reach;
};

struct Counts
{
	double kept = 0, dropped = 0;
	std::size_t topFaces = 0, groundLinks = 0, seamFaces = 0, seamLinkedToGround = 0, islands = 0, openPairs = 0;
};

Counts building(const std::string& dataDir, const kworld::Heightmap& terrain, const kworld::AreaBuilding& b)
{
	knav::surfgen::Input in;
	in.pose = knav::surfgen::Input::poseOf(b.placed);
	for (const kworld::WorldShape& s : *b.localShapes)
		in.shapes.push_back(&s);
	for (const kworld::WorldShape& m : *b.localMask)
		in.mask.push_back(&m);
	in.side = knav::surfgen::Side::Zone;
	const Mesh zoneSide = knav::surfgen::generate(in);
	test::require(!zoneSide.faces.empty(), "the generator makes the building's zone side");
	ground::Cutter side = ground::zoneSideCutter(zoneSide, b.id);
	double cx = b.placed.x, cz = b.placed.z;
	double reach = 60;
	for (const kworld::WorldShape& s : *b.shapes)
		reach = std::max({ reach, std::fabs(s.max[0] - cx), std::fabs(s.min[0] - cx), std::fabs(s.max[2] - cz), std::fabs(s.min[2] - cz) });
	reach += 90;
	const std::vector<ground::Cutter> extra = { side };
	Out o;
	o.mesh = ground::generateBox(dataDir, terrain, cx - reach, cz - reach, cx + reach, cz + reach, {}, {}, true, &o.stats, nullptr, nullptr, &extra);
	o.tops.insert(o.stats.topFaceIndex.begin(), o.stats.topFaceIndex.end());
	Counts c;
	c.kept = at(o.stats.topKept, b.id);
	c.dropped = at(o.stats.topDropped, b.id);
	const auto onSeam = [&](std::uint32_t f)
	{
		for (std::uint32_t k = 0; k < o.mesh.faces[f].numEdges; ++k)
		{
			const P3& u = o.mesh.vertices[o.mesh.edges[o.mesh.faces[f].startEdge + k].a], & w = o.mesh.vertices[o.mesh.edges[o.mesh.faces[f].startEdge + (k + 1) % o.mesh.faces[f].numEdges].a];
			const P3 m = { (u[0] + w[0]) / 2, (u[1] + w[1]) / 2, (u[2] + w[2]) / 2 };
			for (const auto& sg : side.seam)
			{
				const double dx = sg[1][0] - sg[0][0], dz = sg[1][2] - sg[0][2], l2 = dx * dx + dz * dz;
				const double t = l2 > 0 ? std::clamp(((m[0] - sg[0][0]) * dx + (m[2] - sg[0][2]) * dz) / l2, 0.0, 1.0) : 0.0;
				if (std::hypot(sg[0][0] + t * dx - m[0], sg[0][2] + t * dz - m[2]) <= 0.05 && std::fabs(sg[0][1] + t * (sg[1][1] - sg[0][1]) - m[1]) <= 1.0)
					return true;
			}
		}
		return false;
	};
	// this building's tops: those within its zone side's box
	std::vector<std::uint32_t> mine;
	for (const std::uint32_t f : o.tops)
	{
		const P3& v = o.mesh.vertices[o.mesh.edges[o.mesh.faces[f].startEdge].a];
		if (v[0] >= side.min[0] - 0.5 && v[0] <= side.max[0] + 0.5 && v[2] >= side.min[2] - 0.5 && v[2] <= side.max[2] + 0.5 && v[1] >= side.min[1] - 0.5 && v[1] <= side.max[1] + 0.5)
			mine.push_back(f);
	}
	c.topFaces = mine.size();
	// components of the tops over links: with a ground face (reach), with the seam; an island has neither
	std::vector<int> comp(o.mesh.faces.size(), -1);
	int nc = 0;
	std::vector<char> hasGround, hasSeam;
	for (const std::uint32_t start : mine)
	{
		if (comp[start] >= 0)
			continue;
		hasGround.push_back(0);
		hasSeam.push_back(0);
		std::vector<std::uint32_t> stack = { start };
		comp[start] = nc;
		while (!stack.empty())
		{
			const std::uint32_t f = stack.back();
			stack.pop_back();
			hasSeam.back() = hasSeam.back() || onSeam(f);
			for (std::uint32_t k = 0; k < o.mesh.faces[f].numEdges; ++k)
			{
				const auto& e = o.mesh.edges[o.mesh.faces[f].startEdge + k];
				if (e.oppositeFace < 0)
					continue;
				if (!o.tops.count((std::uint32_t)e.oppositeFace))
				{
					hasGround.back() = 1;
					++c.groundLinks;
				}
				else if (comp[e.oppositeFace] < 0)
				{
					comp[e.oppositeFace] = nc;
					stack.push_back((std::uint32_t)e.oppositeFace);
				}
			}
		}
		++nc;
	}
	for (int i = 0; i < nc; ++i)
		c.islands += !hasGround[i] && !hasSeam[i];
	for (const std::uint32_t f : mine)
		if (onSeam(f))
		{
			++c.seamFaces;
			c.seamLinkedToGround += hasGround[comp[f]];
		}
	// open pairs: an unlinked top edge beside an unlinked ground edge, close in the plan and the height (the matcher's rules), not across a wall
	std::vector<std::pair<P3, P3>> groundEdges;
	for (std::uint32_t f = 0; f < o.mesh.faces.size(); ++f)
		if (!o.tops.count(f))
			for (std::uint32_t k = 0; k < o.mesh.faces[f].numEdges; ++k)
			{
				const auto& e = o.mesh.edges[o.mesh.faces[f].startEdge + k];
				const P3& u = o.mesh.vertices[e.a];
				if (e.oppositeFace < 0 && !e.linked && u[0] > side.min[0] - 5 && u[0] < side.max[0] + 5 && u[2] > side.min[2] - 5 && u[2] < side.max[2] + 5)
					groundEdges.push_back({ u, o.mesh.vertices[e.b] });
			}
	for (const std::uint32_t f : mine)
		for (std::uint32_t k = 0; k < o.mesh.faces[f].numEdges; ++k)
		{
			const auto& e = o.mesh.edges[o.mesh.faces[f].startEdge + k];
			if (e.oppositeFace >= 0 || e.linked)
				continue;
			const P3& u = o.mesh.vertices[e.a], & w = o.mesh.vertices[e.b];
			const double len = std::hypot(w[0] - u[0], w[2] - u[2]);
			if (len < 0.5)
				continue;
			// the edge's two sub-points at a quarter and three quarters, each against the ground's boundary edges, running the opposite way
			bool open = false;
			for (const auto& [gu, gw] : groundEdges)
			{
				const double gdx = gw[0] - gu[0], gdz = gw[2] - gu[2], glen = std::hypot(gdx, gdz);
				if (glen < 0.5 || (((w[0] - u[0]) * gdx + (w[2] - u[2]) * gdz) / (len * glen)) > -0.9)
					continue;
				// the matcher's rule: the overlap of the two edges (along the top edge), its two ends within 1 u in the plan and 5 u in height
				const double dx = w[0] - u[0], dz = w[2] - u[2];
				const double s0 = ((gu[0] - u[0]) * dx + (gu[2] - u[2]) * dz) / (len * len), s1 = ((gw[0] - u[0]) * dx + (gw[2] - u[2]) * dz) / (len * len);
				const double lo = std::max(0.0, std::min(s0, s1)), hi = std::min(1.0, std::max(s0, s1));
				if ((hi - lo) * len < 0.2)
					continue;
				bool ends = true;
				for (const double sp : { lo, hi })
				{
					const P3 q = { u[0] + sp * dx, u[1] + sp * (w[1] - u[1]), u[2] + sp * dz };
					const double t = std::clamp(((q[0] - gu[0]) * gdx + (q[2] - gu[2]) * gdz) / (glen * glen), 0.0, 1.0);
					ends = ends && std::hypot(gu[0] + t * gdx - q[0], gu[2] + t * gdz - q[2]) <= 1.0 && std::fabs(gu[1] + t * (gw[1] - gu[1]) - q[1]) <= 5.0;
				}
				if (ends)
				{
					open = true;
					break;
				}
			}
			c.openPairs += open;
		}
	return c;
}

} // namespace

int main()
{
	try
	{
		synthetic();
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const std::vector<Site> sites = {
			{ "Heft slaver camp, nearest masked building to the longhouse", 68339, -71264, nullptr, 250 },
			{ "Hub bar", -51181.133, 2638.970, "4933-Newwworld", 5 },
			{ "Storm House 3384", -51192, 3192, "3384-TwoStorey", 40 },
		};
		for (const Site& site : sites)
		{
			const kworld::ZoneCell zone = kworld::zoneCellOf(site.x, site.z);
			const kworld::AreaGeometry area = knav::gatherArea(dataDir, terrain, zone, zone);
			const kworld::AreaBuilding* pick = nullptr;
			double best = 1e30;
			for (const kworld::AreaBuilding& b : area.buildings)
			{
				if (b.shapes->empty() || !b.mask || !b.localShapes || !b.localMask)
					continue;
				const double d = std::hypot(b.placed.x - site.x, b.placed.z - site.z);
				if (site.buildingId ? (b.placed.instanceID.find(site.buildingId) == std::string::npos && b.placed.buildingID.find(site.buildingId) == std::string::npos) : d > site.reach)
					continue;
				if (site.buildingId && d > site.reach + 200)
					continue;
				if (d < best)
				{
					best = d;
					pick = &b;
				}
			}
			test::require(pick != nullptr, "the site's masked building is placed");
			const Counts c = building(dataDir, terrain, *pick);
			std::cout << site.name << ": " << pick->id << " (" << pick->placed.buildingID << "): zone-side tops kept " << c.kept << " u2, dropped " << c.dropped
				<< " u2; " << c.topFaces << " faces, " << c.groundLinks << " links to the ground, " << c.seamFaces << " on the interior seam ("
				<< c.seamLinkedToGround << " in a component with ground), islands " << c.islands << ", open ramp / ground pairs " << c.openPairs << "\n";
			test::require(c.topFaces > 0 && c.kept > 1, "the zone side has walkable faces on the ground");
			test::require(c.groundLinks > 0, "the entrance reaches the ground");
			test::require(c.islands == 0, "no zone-side island (a component with neither ground nor interior seam) is left");
			test::require(c.seamFaces == 0 || c.seamLinkedToGround > 0, "one component from the ground into the interior seam");
			test::require(c.openPairs == 0, "no open edge between the ramp and the ground");
		}
		std::cout << "knav_ground_buildings ok\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
