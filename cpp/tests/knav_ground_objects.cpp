// Objects on our ground (NAVMESH_GENERATOR.md section 12, kenshi-k16.7.197): one fixture site per census class (docs/reviews/2026-11-25-object-census,
// sections 1-3), each window built with and without its obstacles. A row of the table shows as a hole (the ground lost under the object) and
// walkable tops linked to the ground (a top face whose component reaches a ground face over a link). The Pontoon Bridge placements 41,000 u under the
// world are reported and inert; the generation time of a window is reported.

#include "knav/ground.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace
{

namespace ground = knav::ground;
using knav::native::Mesh;

struct Fixture
{
	const char* cls; // the census class
	const char* name;
	double x, z;
	bool hole; // the row: the ground is lost under the object
	bool tops; // walkable tops that link to the ground
	double reach = 12; // the object's xz reach for the sampling box
	double window = 300; // half the window built round the site
};

bool covers(const Mesh& m, const knav::native::Face& f, double x, double z)
{
	for (std::uint32_t k = 0; k < f.numEdges; ++k)
	{
		const auto& a = m.vertices[m.edges[f.startEdge + k].a];
		const auto& b = m.vertices[m.edges[f.startEdge + (k + 1) % f.numEdges].a];
		if ((b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]) < -1e-9)
			return false;
	}
	return true;
}

// Sample points of the box covered by a ground (non-top) face.
int groundPoints(const Mesh& m, const std::set<std::uint32_t>& tops, double x, double z, double r)
{
	std::vector<std::uint32_t> near;
	for (std::uint32_t f = 0; f < m.faces.size(); ++f)
	{
		if (tops.count(f))
			continue;
		double x0 = 1e30, x1 = -1e30, z0 = 1e30, z1 = -1e30;
		for (std::uint32_t k = 0; k < m.faces[f].numEdges; ++k)
		{
			const auto& v = m.vertices[m.edges[m.faces[f].startEdge + k].a];
			x0 = std::min(x0, v[0]);
			x1 = std::max(x1, v[0]);
			z0 = std::min(z0, v[2]);
			z1 = std::max(z1, v[2]);
		}
		if (x1 >= x - r && x0 <= x + r && z1 >= z - r && z0 <= z + r)
			near.push_back(f);
	}
	int n = 0;
	for (double px = x - r; px <= x + r; px += 2)
		for (double pz = z - r; pz <= z + r; pz += 2)
			for (const std::uint32_t f : near)
				if (covers(m, m.faces[f], px, pz))
				{
					++n;
					break;
				}
	return n;
}

struct Observed
{
	int lost = 0, total = 0;
	std::size_t topFaces = 0;
	bool linked = false;
	double ms = 0;
	ground::Stats stats;
	Mesh mesh;
};

Observed observe(const std::string& dataDir, const kworld::Heightmap& terrain, const Fixture& fx)
{
	Observed o;
	const double w = fx.window;
	ground::Params params;
	params.keepLooseTops = std::getenv("KEEP_LOOSE") != nullptr;
	if (const char* e = std::getenv("DECK_SLOPE"))
		params.deckMaxSlopeDeg = std::atof(e);
	if (const char* e = std::getenv("DECK_SHAPE"))
		params.deckShapeShare = std::atof(e);
	const auto t0 = std::chrono::steady_clock::now();
	const Mesh cut = ground::generateBox(dataDir, terrain, fx.x - w, fx.z - w, fx.x + w, fx.z + w, params, {}, true, &o.stats);
	o.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	o.mesh = cut;
	const Mesh open = ground::generateBox(dataDir, terrain, fx.x - w, fx.z - w, fx.x + w, fx.z + w, params, {}, false);
	const std::set<std::uint32_t> tops(o.stats.topFaceIndex.begin(), o.stats.topFaceIndex.end());
	const int before = groundPoints(open, {}, fx.x, fx.z, fx.reach), after = groundPoints(cut, tops, fx.x, fx.z, fx.reach);
	o.total = before;
	o.lost = before - after;
	// tops within reach, and whether the component of one of them reaches a ground face over a link
	std::vector<char> seen(cut.faces.size(), 0);
	std::vector<std::uint32_t> stack;
	for (const std::uint32_t f : tops)
	{
		// a corner within reach, or a point of the reach box (a 7 x 7 grid over it) inside the face: a face is as big as its surface, its
		// corners can all lie outside the box (kenshi-k16.7.212)
		bool in = false;
		const std::uint32_t n = cut.faces[f].numEdges, e0 = cut.faces[f].startEdge;
		for (std::uint32_t k = 0; k < n; ++k)
		{
			const auto& v = cut.vertices[cut.edges[e0 + k].a];
			in = in || (std::fabs(v[0] - fx.x) <= fx.reach && std::fabs(v[2] - fx.z) <= fx.reach);
		}
		for (int i = -3; i <= 3 && !in; ++i)
			for (int j = -3; j <= 3 && !in; ++j)
			{
				const double x = fx.x + i * fx.reach / 3, z = fx.z + j * fx.reach / 3;
				double turn = 0;
				bool inside = true;
				for (std::uint32_t k = 0; k < n && inside; ++k)
				{
					const auto& v = cut.vertices[cut.edges[e0 + k].a];
					const auto& w = cut.vertices[cut.edges[e0 + k].b];
					const double side = (w[0] - v[0]) * (z - v[2]) - (w[2] - v[2]) * (x - v[0]);
					inside = side * turn >= 0;
					if (side != 0)
						turn = side;
				}
				in = inside;
			}
		if (!in)
			continue;
		++o.topFaces;
		if (!seen[f])
		{
			seen[f] = 1;
			stack.push_back(f);
		}
	}
	while (!stack.empty())
	{
		const std::uint32_t f = stack.back();
		stack.pop_back();
		for (std::uint32_t k = 0; k < cut.faces[f].numEdges; ++k)
		{
			const auto& e = cut.edges[cut.faces[f].startEdge + k];
			if (e.oppositeFace < 0)
				continue;
			if (!tops.count(e.oppositeFace))
				o.linked = true;
			else if (!seen[e.oppositeFace])
			{
				seen[e.oppositeFace] = 1;
				stack.push_back(e.oppositeFace);
			}
		}
	}
	return o;
}

} // namespace

int main()
{
	try
	{
		const std::string dataDir = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		// class: the census class (sections 1-3), site: its README's first sample site; row: the table of section 12
		const std::vector<Fixture> fixtures = {
			{ "building without mask (walkable)", "Swamp Lookout Tower", -61697, 33427, true, true, 40 },
			{ "wall / fence (an isolated wall-walk reaches no ground)", "Defensive Wall III Short", -97261, 46611, true, false, 30, 400 },
			{ "wall gate (masked: surfgen's tops)", "Defensive Gate III", -79465, 114501, true, false, 30 },
			{ "wall ramp", "Defensive Wall Ramp IV", -83399, -38971, true, true, 200, 400 },
			{ "ore node (no parts)", "Iron Resource", -100815, -29562, false, false, 15 },
			{ "mine / quarry machine", "Quarry Mine B", -48381, -57820, true, false, 100, 400 },
			{ "windmill", "Wind Generator", -83250, -37395, true, false, 30 },
			{ "generator", "Generator II", -80511, 114759, true, false, 15 },
			{ "outhouse", "Outhouse", -83113, -37463, true, true, 15 },
			{ "turret (IGNORE: no hole; the top is the wall-walk it stands on)", "Mounted Crossbow mkII", -47907, 47512, false, true, 6 },
			{ "camp (IGNORE)", "Camp Bed", -60567, -39281, false, false, 6 },
			{ "light / lamp (IGNORE)", "Torch Post", -71821, -107506, false, false, 4 },
			{ "farm (IGNORE)", "Cactus Farm XL", -24383, -69968, false, false, 15 },
			{ "cage / training / camp prop", "Prisoner Cage", 81808, 46963, true, false, 15 },
			{ "town prop pm0 (IGNORE)", "Shade Cover", -12548, -90272, false, false, 6 },
			{ "town prop pm1", "Barrel A", -80108, 114776, true, false, 6 },
			{ "town prop pm2", "Market Stall A", -82816, -37762, true, false, 15 },
			{ "town prop pm3", "Swamp Walkboard", -62079, 33594, true, true, 20 },
			{ "foliage with collision (Obstacle)", "ResourceRock-IRON01", -12833, -144021, true, false, 15 },
			{ "foliage walkable collision, level with the ground (no cutter: walked across in the original, kenshi-ffmi.14)", "TreeFall01", -77686, -83856, false, false, 15 },
			{ "foliage walkable collision over the climb (the shipped mesh lies on its top)", "TreeFall03", -75884, -88229, true, true, 15 },
			{ "foliage cutter disc only (Projected: the footprint of its walls cuts the ground)", "FOLIAGE_Boulder01", -71596, 616, true, false, 8 },
			{ "foliage neither (grass)", "PhantomGrass001", 82019, -51239, false, false, 15 },
			{ "feature: bridge", "Metal Arch Bridge", -54433, -95121, true, true, 600, 900 },
			{ "feature: bridge", "Metal Arch Bridge", 12399, -29427, true, true, 600, 900 },
			{ "feature: bridge", "Trestle Bridge001", -45980, -55948, true, true, 400, 700 },
			{ "feature: cliff block (F1: an overhanging underside passes the face test; the shape-level share drops its tops)", "Canyonland Cliffblock 01", -85611, -130729, true, false, 300, 700 },
			{ "feature: rock arch (a top only as a deck)", "ROCK-Arch Type 01", -30922, -18912, true, false, 300, 700 },
			{ "feature: wreck", "Barge WreckPiece02", 93381, 50112, true, false, 100, 400 },
		};
		if (const char* one = std::getenv("OBJ_SITE"))
		{
			Fixture fx{ "debug", "site", 0, 0, false, false };
			std::sscanf(one, "%lf,%lf,%lf,%lf", &fx.x, &fx.z, &fx.reach, &fx.window);
			const Observed o = observe(dataDir, terrain, fx);
			std::cout << "lost " << o.lost << " of " << o.total << ", tops " << o.topFaces << (o.linked ? " linked" : " not linked") << ", " << o.stats.topFaces << " top faces in the window, "
				<< o.stats.topDroppedFaces << " dropped (" << o.stats.topDroppedArea << " u2), deck tris " << o.stats.deckTris << " rejected " << o.stats.deckRejected << ", links " << o.stats.topLinks << ", ms tops " << o.stats.msTops << " link " << o.stats.msLink << "\n";
			for (const auto& [label, area] : o.stats.topDropped)
				std::cout << "  dropped " << area << " u2: " << label << "\n";
			if (std::getenv("EDGES"))
			{
				const Mesh& m = o.mesh;
				const std::set<std::uint32_t> tops(o.stats.topFaceIndex.begin(), o.stats.topFaceIndex.end());
				for (const std::uint32_t f : tops)
					for (std::uint32_t k = 0; k < m.faces[f].numEdges; ++k)
					{
						const auto& e = m.edges[m.faces[f].startEdge + k];
						const auto& a = m.vertices[e.a];
						const auto& b = m.vertices[e.b];
						if (e.oppositeFace >= 0 || std::fabs(a[0] - fx.x) > fx.reach || std::fabs(a[2] - fx.z) > fx.reach)
							continue;
						double best = 1e9;
						std::array<double, 3> ga{}, gb{};
						for (std::uint32_t g = 0; g < m.faces.size(); ++g)
						{
							if (tops.count(g))
								continue;
							for (std::uint32_t j = 0; j < m.faces[g].numEdges; ++j)
							{
								const auto& ge = m.edges[m.faces[g].startEdge + j];
								if (ge.oppositeFace >= 0)
									continue;
								const auto& p = m.vertices[ge.a];
								const auto& q = m.vertices[ge.b];
								const double d = std::hypot((p[0] + q[0] - a[0] - b[0]) / 2, (p[2] + q[2] - a[2] - b[2]) / 2);
								if (d < best) { best = d; ga = p; gb = q; }
							}
						}
						std::cout << "  top edge (" << a[0] << "," << a[1] << "," << a[2] << ")->(" << b[0] << "," << b[1] << "," << b[2] << ") nearest ground edge mid " << best << " away: ("
							<< ga[0] << "," << ga[1] << "," << ga[2] << ")->(" << gb[0] << "," << gb[1] << "," << gb[2] << ")\n";
					}
			}
			return 0;
		}
		int bad = 0;
		for (const Fixture& fx : fixtures)
		{
			const Observed o = observe(dataDir, terrain, fx);
			const bool hole = o.lost > 2, tops = o.topFaces > 0 && o.linked;
			std::cout << (hole == fx.hole && tops == fx.tops ? "ok   " : "ROW  ") << fx.cls << " / " << fx.name << " (" << fx.x << ", " << fx.z << "): ground lost " << o.lost
				<< " of " << o.total << " points (" << (hole ? "hole" : "no hole") << "), tops " << o.topFaces << (tops ? " linked" : o.topFaces ? " NOT linked" : "") << ", window "
				<< o.ms << " ms (tops " << o.stats.msTops << " + link " << o.stats.msLink << ", " << o.stats.topFaces << " top faces, " << o.stats.topDroppedFaces << " dropped)\n";
			bad += !(hole == fx.hole && tops == fx.tops);
		}
		test::require(bad == 0, std::to_string(bad) + " fixture sites differ from their row");

		// The 6 Pontoon Bridge placements 41,000 .. 92,000 u under the world (census outliers.tsv): their tops lie under the terrain, so no piece is kept;
		// they are reported and inert (a shape wholly under the terrain cuts nothing and keeps no face).
		const double pontoon[6][2] = { { -100842.5, 101313.1 }, { -92582.9, 44505.5 }, { -92724.7, 44462.9 }, { -19717.9, 38299.4 }, { -19522.2, 38162.9 }, { -106973.9, 96528.4 } };
		for (const auto& site : pontoon)
		{
			Fixture fx{ "outlier", "Pontoon Bridge", site[0], site[1], false, false, 40 };
			const Observed o = observe(dataDir, terrain, fx);
			std::cout << "outlier Pontoon Bridge (" << fx.x << ", " << fx.z << "): " << o.stats.underTerrain.size() << " shape(s) wholly under the terrain:";
			for (const std::string& label : o.stats.underTerrain)
				std::cout << " " << label;
			std::cout << "\n";
			test::require(!o.stats.underTerrain.empty(), "a Pontoon Bridge placed under the world is reported");
		}
		std::cout << "knav_ground_objects ok\n";
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
