// knav_ground_vs_shipped: dev tool (kenshi-k16.7.231). The ground window of a zone (terrain + cutters + tops, as the bake's ground pass) against the
// shipped navtile's world mesh: faces, vertices, wall edges, the share of our wall within 1 u (xz) of the shipped wall and the reverse, and the
// ground pass's time. Samples within 2 u of the zone box are left out (the window edge is no wall of the original's).
//   knav_ground_vs_shipped [--base] [--depth=U] [--nocorridor] I,J ...       --base: the rules before kenshi-k16.7.231 (10 u2 / 1 u, no loop guards, height-error order)
#include "knav/ground.hpp"
#include "knav/area.hpp"
#include "knav/native.hpp"
#include "knav/seeds.hpp"
#include "knav/zone_gen.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>

namespace
{

namespace ground = knav::ground;
using Mesh = knav::native::Mesh;

struct Segs
{
	std::vector<std::array<double, 4>> s;
	std::unordered_map<long long, std::vector<int>> cell;
	static constexpr double kCell = 8;
	static long long key(int cx, int cz) { return (long long)cx * 100003 + cz; }
	void add(double ax, double az, double bx, double bz)
	{
		s.push_back({ ax, az, bx, bz });
		for (int cx = (int)std::floor(std::min(ax, bx) / kCell); cx <= (int)std::floor(std::max(ax, bx) / kCell); ++cx)
			for (int cz = (int)std::floor(std::min(az, bz) / kCell); cz <= (int)std::floor(std::max(az, bz) / kCell); ++cz)
				cell[key(cx, cz)].push_back((int)s.size() - 1);
	}
	bool near(double x, double z, double r) const
	{
		for (int cx = (int)std::floor((x - r) / kCell); cx <= (int)std::floor((x + r) / kCell); ++cx)
			for (int cz = (int)std::floor((z - r) / kCell); cz <= (int)std::floor((z + r) / kCell); ++cz)
			{
				const auto it = cell.find(key(cx, cz));
				if (it == cell.end())
					continue;
				for (const int i : it->second)
				{
					const auto& g = s[i];
					const double dx = g[2] - g[0], dz = g[3] - g[1], l2 = dx * dx + dz * dz;
					const double t = l2 > 0 ? std::clamp(((x - g[0]) * dx + (z - g[1]) * dz) / l2, 0.0, 1.0) : 0.0;
					if (std::hypot(x - g[0] - t * dx, z - g[1] - t * dz) <= r)
						return true;
				}
			}
		return false;
	}
};

Segs walls(const Mesh& m, double x0, double z0, double x1, double z1, double& length, std::size_t& count, const std::vector<ground::Material>* mat = nullptr,
	std::size_t* byMat = nullptr, double* lenMat = nullptr)
{
	Segs out;
	length = 0;
	count = 0;
	for (std::size_t e = 0; e < m.edges.size(); ++e)
	{
		if (!m.wall(e))
			continue;
		const auto& a = m.vertices[m.edges[e].a];
		const auto& b = m.vertices[m.edges[e].b];
		++count;
		if (mat)
		{
			const int k = (int)(*mat)[e];
			++byMat[k];
			lenMat[k] += std::hypot(b[0] - a[0], b[2] - a[2]);
		}
		length += std::hypot(b[0] - a[0], b[2] - a[2]);
		out.add(a[0], a[2], b[0], b[2]);
	}
	(void)x0; (void)z0; (void)x1; (void)z1;
	return out;
}

// the share of `from`'s wall (sampled every 0.5 u, off the box edge) within 1 u of `to`'s
double share(const Segs& from, const Segs& to, double x0, double z0, double x1, double z1)
{
	std::size_t n = 0, hit = 0;
	for (const auto& g : from.s)
	{
		const double len = std::hypot(g[2] - g[0], g[3] - g[1]);
		const int k = std::max(1, (int)std::ceil(len / 0.5));
		for (int i = 0; i < k; ++i)
		{
			const double t = (i + 0.5) / k, x = g[0] + t * (g[2] - g[0]), z = g[1] + t * (g[3] - g[1]);
			if (x < x0 + 2 || x > x1 - 2 || z < z0 + 2 || z > z1 - 2)
				continue;
			++n;
			hit += to.near(x, z, 1.0);
		}
	}
	return n ? 100.0 * hit / n : 0;
}

// --buildings: our building-owned wall edges per cutter label (the one that decided the edge), split by the face kind that carries the edge, with
// the collinear join count (a wall vertex whose two wall edges turn < 0.6 deg: a weld / T-junction split point, no corner), the corner count (turn
// >= 20 deg), and the shipped wall edges (any material) whose midpoint lies in the cutter's xz box grown by 3 u.
void buildingReport(const std::map<std::string, std::string>& names, const Mesh& m, const std::vector<ground::Material>& mat, const std::vector<int>& owner, const std::vector<ground::Cutter>& cutters,
	const std::vector<Mesh>& shipped, int top)
{
	struct Row { std::size_t n = 0, onTop = 0, onDoor = 0, collinear = 0, corners = 0, shipped = 0, tiny = 0; double len = 0; };
	std::map<std::string, Row> rows;
	std::size_t hist[2][7] = {}, offHist[2][5] = {};
	const double offCut[4] = { 0.01, 0.1, 0.5, 1 };
	const double cut[6] = { 0.6, 5, 20, 45, 90, 135 };
	std::vector<std::size_t> faceOf(m.edges.size());
	for (std::size_t f = 0; f < m.faces.size(); ++f)
		for (std::uint32_t k = 0; k < m.faces[f].numEdges; ++k)
			faceOf[m.faces[f].startEdge + k] = f;
	std::unordered_map<std::uint32_t, std::vector<std::size_t>> startAt;
	if (const char* fs = std::getenv("KNAV_FACES"))	// "first last": the faces' vertices, '*' marks a wall edge
		for (std::size_t f = (std::size_t)std::atoi(fs), last = (std::size_t)std::atoi(std::strchr(fs, ' ') + 1); f <= last; ++f)
		{
			std::printf("#F %zu top=%d data=%d:", f, (int)m.faces[f].top, m.faces[f].data);
			for (std::uint32_t k = 0; k < m.faces[f].numEdges; ++k)
			{
				const std::size_t e = m.faces[f].startEdge + k;
				std::printf(" %s(%.2f,%.2f,%.2f)", m.wall(e) ? "*" : m.edges[e].linked ? "L" : "", m.vertices[m.edges[e].a][0], m.vertices[m.edges[e].a][1], m.vertices[m.edges[e].a][2]);
			}
			std::printf("\n");
		}
	std::vector<std::vector<int>> vertEdges(m.vertices.size());	// per vertex: the edges that start there (one per face holding it)
	std::size_t collinearBy[7][2] = {};
	for (std::size_t e = 0; e < m.edges.size(); ++e)
		vertEdges[m.edges[e].a].push_back((int)e);
	for (std::size_t e = 0; e < m.edges.size(); ++e)
		if (m.wall(e))
			startAt[m.edges[e].a].push_back(e);
	for (std::size_t e = 0; e < m.edges.size(); ++e)
	{
		if (!m.wall(e) || mat[e] != ground::Material::Building || owner[e] < 0)
			continue;
		Row& r = rows[cutters[owner[e]].label];
		if (const char* d = std::getenv("KNAV_DUMP"); d && cutters[owner[e]].label == d)
			std::printf("#E %d %zu %.2f %.2f %.2f %.2f %.2f\n", (int)m.faces[faceOf[e]].top, faceOf[e], m.vertices[m.edges[e].a][0], m.vertices[m.edges[e].a][2], m.vertices[m.edges[e].b][0], m.vertices[m.edges[e].b][2], m.vertices[m.edges[e].a][1]);
		const auto& a = m.vertices[m.edges[e].a];
		const auto& b = m.vertices[m.edges[e].b];
		const knav::native::Face& f = m.faces[faceOf[e]];
		++r.n;
		r.len += std::hypot(b[0] - a[0], b[2] - a[2]);
		r.onTop += f.top;
		r.tiny += std::hypot(b[0] - a[0], b[2] - a[2]) < 0.5;
		r.onDoor += f.data == 4 || f.data == 5;
		const auto it = startAt.find(m.edges[e].b);
		if (it == startAt.end() || it->second.size() != 1)
			continue;
		const auto& c = m.vertices[m.edges[it->second[0]].b];
		const double ux = b[0] - a[0], uz = b[2] - a[2], vx = c[0] - b[0], vz = c[2] - b[2];
		const double lu = std::hypot(ux, uz), lv = std::hypot(vx, vz);
		if (lu < 1e-9 || lv < 1e-9)
			continue;
		const double sn = std::fabs(ux * vz - uz * vx) / (lu * lv), cs = (ux * vx + uz * vz) / (lu * lv);
		const double ang = std::atan2(sn, cs) * 180 / 3.14159265358979;
		{
			const double off = std::fabs((c[0] - a[0]) * (b[2] - a[2]) - (c[2] - a[2]) * (b[0] - a[0])) / std::max(1e-9, std::hypot(c[0] - a[0], c[2] - a[2]));
			int ob = 0;
			while (ob < 4 && off >= offCut[ob])
				++ob;
			++offHist[f.top][ob];
		}
		int bk = 0;
		while (bk < 6 && ang >= cut[bk])
			++bk;
		++hist[f.top][bk];
		if (ang < 0.6)
		{
			++collinearBy[std::min<std::size_t>(vertEdges[m.edges[e].b].size(), 6)][f.top];
		}
		r.collinear += ang < 0.6;
		r.corners += ang >= 20;
	}
	for (auto& [label, r] : rows)
	{
		double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
		for (const auto& c : cutters)
			if (c.label == label)
			{
				x0 = std::min(x0, c.min[0]); z0 = std::min(z0, c.min[2]);
				x1 = std::max(x1, c.max[0]); z1 = std::max(z1, c.max[2]);
			}
		for (const Mesh& s : shipped)
			for (std::size_t e = 0; e < s.edges.size(); ++e)
			{
				if (!s.wall(e))
					continue;
				const auto& a = s.vertices[s.edges[e].a];
				const auto& b = s.vertices[s.edges[e].b];
				const double x = (a[0] + b[0]) / 2, z = (a[2] + b[2]) / 2;
				r.shipped += x >= x0 - 3 && x <= x1 + 3 && z >= z0 - 3 && z <= z1 + 3;
			}
	}
	std::vector<std::pair<std::string, Row>> sorted(rows.begin(), rows.end());
	std::sort(sorted.begin(), sorted.end(), [](const auto& l, const auto& r) { return l.second.n > r.second.n; });
	std::printf("#  building\tours\tonTop\ttiny<0.5\tonDoor\tcollinear\tcorners\tmean_u\tshipped_in_box3\tbox\n");
	for (int i = 0; i < (int)sorted.size() && i < top; ++i)
	{
		const Row& r = sorted[i].second;
		std::printf("#B %s [%s]\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%.1f\t%zu\n", sorted[i].first.c_str(), names.count(sorted[i].first) ? names.at(sorted[i].first).c_str() : "?", r.n, r.onTop, r.tiny, r.onDoor, r.collinear, r.corners, r.n ? r.len / r.n : 0.0, r.shipped);
	}
	std::printf("#  collinear wall vertices by number of faces holding the vertex (ground / top):");
	for (int i = 1; i < 7; ++i) std::printf(" %d: %zu / %zu;", i, collinearBy[i][0], collinearBy[i][1]);
	std::printf("\n");
	std::printf("#  wall vertex off the chord of its neighbours (<0.01 / <0.1 / <0.5 / <1 / more u): ground faces");
	for (int i = 0; i < 5; ++i) std::printf(" %zu", offHist[0][i]);
	std::printf("; top faces");
	for (int i = 0; i < 5; ++i) std::printf(" %zu", offHist[1][i]);
	std::printf("\n");
	std::printf("#  turn at wall vertices (<0.6 / <5 / <20 / <45 / <90 / <135 / more): ground faces");
	for (int i = 0; i < 7; ++i) std::printf(" %zu", hist[0][i]);
	std::printf("; top faces");
	for (int i = 0; i < 7; ++i) std::printf(" %zu", hist[1][i]);
	std::printf("\n");
	Row all;
	for (const auto& [l, r] : sorted)
	{
		all.n += r.n; all.onTop += r.onTop; all.tiny += r.tiny; all.onDoor += r.onDoor; all.collinear += r.collinear; all.corners += r.corners; all.shipped += r.shipped;
	}
	std::printf("#B TOTAL (%zu labels)\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t-\t%zu\n", sorted.size(), all.n, all.onTop, all.tiny, all.onDoor, all.collinear, all.corners, all.shipped);
}

// --features: per map-feature cutter (label "feature @ x, z"), box grown 3 u: our top faces and wall edges against the shipped world mesh's faces and
// walls in the same box. Our top face is "agreed" when a shipped face contains its xz centroid at a height within 1.5 u. Class a: shipped has no
// top there (< 10% of our top faces agreed), c: >= 70% agreed, b: between. Per feature also: top faces connected to a non-top face through opposite
// faces (the seed prune's reach), faces under the terrain (centroid y < terrain - 0.5), and faces inside another cutter's box (a building's, 0.5 u in).
bool inFace(const Mesh& s, const knav::native::Face& f, double x, double z)
{
	bool in = false;
	for (std::uint32_t i = 0, n = f.numEdges; i < n; ++i)
	{
		const auto& a = s.vertices[s.edges[f.startEdge + i].a];
		const auto& b = s.vertices[s.edges[f.startEdge + (i + 1) % n].a];
		if ((a[2] > z) != (b[2] > z) && x < (b[0] - a[0]) * (z - a[2]) / (b[2] - a[2]) + a[0])
			in = !in;
	}
	return in;
}

void featureReport(const Mesh& m, const std::vector<ground::Material>& mat, const std::vector<ground::Cutter>& cutters, const std::vector<Mesh>& shipped,
	const kworld::Heightmap& terrain)
{
	const std::size_t nf = m.faces.size();
	std::vector<std::size_t> faceOf(m.edges.size());
	for (std::size_t f = 0; f < nf; ++f)
		for (std::uint32_t k = 0; k < m.faces[f].numEdges; ++k)
			faceOf[m.faces[f].startEdge + k] = f;
	std::vector<int> comp(nf, -1);	// component over opposite faces; groundComp: has a non-top face
	std::vector<char> hasGround;
	for (std::size_t f = 0; f < nf; ++f)
	{
		if (comp[f] >= 0)
			continue;
		const int c = (int)hasGround.size();
		hasGround.push_back(0);
		std::vector<std::size_t> st{ f };
		comp[f] = c;
		while (!st.empty())
		{
			const std::size_t g = st.back();
			st.pop_back();
			hasGround[c] |= !m.faces[g].top;
			for (std::uint32_t k = 0; k < m.faces[g].numEdges; ++k)
			{
				const int o = m.edges[m.faces[g].startEdge + k].oppositeFace;
				if (o >= 0 && comp[o] < 0) { comp[o] = c; st.push_back(o); }
			}
		}
	}
	std::vector<std::array<double, 3>> cen(nf);
	for (std::size_t f = 0; f < nf; ++f)
	{
		std::array<double, 3> c{};
		for (std::uint32_t k = 0; k < m.faces[f].numEdges; ++k)
			for (int d = 0; d < 3; ++d)
				c[d] += m.vertices[m.edges[m.faces[f].startEdge + k].a][d] / m.faces[f].numEdges;
		cen[f] = c;
	}
	struct Row { std::string label; int cls; std::size_t top = 0, agree = 0, linked = 0, underTerrain = 0, inSolid = 0, ourWall = 0, shipFaces = 0, shipWall = 0; double x = 0, z = 0; };
	std::vector<Row> rows;
	for (const auto& c : cutters)
	{
		if (ground::materialOfLabel(c.label) != ground::Material::Feature)
			continue;
		Row r;
		r.label = c.label;
		r.x = (c.min[0] + c.max[0]) / 2;
		r.z = (c.min[2] + c.max[2]) / 2;
		const double x0 = c.min[0] - 3, z0 = c.min[2] - 3, x1 = c.max[0] + 3, z1 = c.max[2] + 3;
		for (std::size_t f = 0; f < nf; ++f)
		{
			if (cen[f][0] < x0 || cen[f][0] > x1 || cen[f][2] < z0 || cen[f][2] > z1)
				continue;
			if (m.faces[f].top)
			{
				++r.top;
				bool ag = false;
				for (const Mesh& s : shipped)
					for (const auto& sf : s.faces)
						if (!ag && inFace(s, sf, cen[f][0], cen[f][2]))
						{
							double y = 0;
							for (std::uint32_t k = 0; k < sf.numEdges; ++k)
								y += s.vertices[s.edges[sf.startEdge + k].a][1] / sf.numEdges;
							ag = std::fabs(y - cen[f][1]) < 1.5;
						}
				r.agree += ag;
				r.linked += hasGround[comp[f]];
				r.underTerrain += cen[f][1] < terrain.height(cen[f][0], cen[f][2]) - 0.5;
				for (const auto& o : cutters)
					if (&o != &c && ground::materialOfLabel(o.label) == ground::Material::Building && cen[f][0] > o.min[0] + 0.5 && cen[f][0] < o.max[0] - 0.5
						&& cen[f][2] > o.min[2] + 0.5 && cen[f][2] < o.max[2] - 0.5 && cen[f][1] > o.min[1] && cen[f][1] < o.max[1])
					{
						++r.inSolid;
						break;
					}
			}
		}
		for (std::size_t e = 0; e < m.edges.size(); ++e)
			if (m.wall(e) && mat[e] == ground::Material::Feature)
			{
				const double x = (m.vertices[m.edges[e].a][0] + m.vertices[m.edges[e].b][0]) / 2, z = (m.vertices[m.edges[e].a][2] + m.vertices[m.edges[e].b][2]) / 2;
				r.ourWall += x >= x0 && x <= x1 && z >= z0 && z <= z1;
			}
		for (const Mesh& s : shipped)
		{
			for (const auto& sf : s.faces)
			{
				const auto& v = s.vertices[s.edges[sf.startEdge].a];
				r.shipFaces += v[0] >= x0 && v[0] <= x1 && v[2] >= z0 && v[2] <= z1;
			}
			for (std::size_t e = 0; e < s.edges.size(); ++e)
				if (s.wall(e))
				{
					const double x = (s.vertices[s.edges[e].a][0] + s.vertices[s.edges[e].b][0]) / 2, z = (s.vertices[s.edges[e].a][2] + s.vertices[s.edges[e].b][2]) / 2;
					r.shipWall += x >= x0 && x <= x1 && z >= z0 && z <= z1;
				}
		}
		r.cls = r.top == 0 ? 0 : r.agree * 10 < r.top ? 1 : r.agree * 10 >= r.top * 7 ? 3 : 2;
		rows.push_back(r);
	}
	std::size_t n[4] = {}, tops[4] = {}, walls[4] = {}, linked[4] = {}, under[4] = {}, solid[4] = {};
	for (const Row& r : rows)
	{
		++n[r.cls]; tops[r.cls] += r.top; walls[r.cls] += r.ourWall; linked[r.cls] += r.linked; under[r.cls] += r.underTerrain; solid[r.cls] += r.inSolid;
	}
	std::size_t topAll = 0, topLinked = 0;
	for (std::size_t f = 0; f < nf; ++f)
		if (m.faces[f].top)
		{
			++topAll;
			topLinked += hasGround[comp[f]];
		}
	std::printf("#  zone: %zu top faces (unique), %zu of them in a component with a non-top face; %zu feature placements\n", topAll, topLinked, rows.size());
	const char* names[4] = { "no top of ours", "a: no shipped top", "b: differs", "c: agrees" };
	for (int k = 0; k < 4; ++k)
		std::printf("#K %s\tfeatures %zu\tour top faces %zu\tlinked to ground %zu\tunder terrain %zu\tin building box %zu\tour feature wall edges %zu\n", names[k], n[k], tops[k], linked[k], under[k], solid[k], walls[k]);
	std::sort(rows.begin(), rows.end(), [](const Row& l, const Row& r) { return l.ourWall + l.top > r.ourWall + r.top; });
	std::printf("#  worst by our wall edges + top faces: label\tclass\ttop\tagree\tlinked\tunderTerrain\tinBuilding\tourWall\tshipFaces\tshipWall\n");
	for (std::size_t i = 0; i < rows.size() && i < 10; ++i)
	{
		const Row& r = rows[i];
		std::printf("#W %s\t%d\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n", r.label.c_str(), r.cls, r.top, r.agree, r.linked, r.underTerrain, r.inSolid, r.ourWall, r.shipFaces, r.shipWall);
	}
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		bool base = false, noCorridor = false, noMaterial = false;
		int buildings = 0;
		bool weld = false, bake = false, features = false;
		double depth = -1;
		std::vector<std::pair<int, int>> zones;
		for (int i = 1; i < argc; ++i)
		{
			int a, b;
			if (!std::strcmp(argv[i], "--base"))
				base = true;
			else if (!std::strncmp(argv[i], "--depth=", 8))
				depth = std::atof(argv[i] + 8);
			else if (!std::strncmp(argv[i], "--buildings=", 12))
				buildings = std::atoi(argv[i] + 12);
			else if (!std::strcmp(argv[i], "--weld"))
				weld = true;
			else if (!std::strcmp(argv[i], "--features"))
				features = true;
			else if (!std::strcmp(argv[i], "--bake"))
				bake = true;
			else if (!std::strcmp(argv[i], "--nomaterial"))
				noMaterial = true;
			else if (!std::strcmp(argv[i], "--nocorridor"))
				noCorridor = true;
			else if (std::sscanf(argv[i], "%d,%d", &a, &b) == 2)
				zones.push_back({ a, b });
		}
		ground::Params prm;
		if (depth >= 0)
			prm.borderDepth = depth;
		if (noCorridor)
			prm.corridorMin = 0;
		if (noMaterial)
			prm.materialRule = false;
		if (base)
		{
			prm.borderArea = 10;
			prm.borderHeight = 1;
			prm.loopShrink = 0;
			prm.corridorMin = 0;
			prm.areaOrder = false;
		}
		const std::string data = KENSHI_DATA;
		const kworld::Heightmap terrain = kworld::Heightmap::open(data + "/newland/land/fullmap.tif");
		const std::string dir = knav::native::navtilesDir(KENSHI_DATA);
		std::printf("# rules: %s%s\nzone\tfaces\tverts\twallEdges\twallLen\tshippedFaces\tshippedWallEdges\tshippedWallLen\tours_near_shipped%%\tshipped_near_ours%%\tms\trefusedLoop\trefusedCorridor\trefusedDepth\n", base ? "base" : "branch", bake ? " (bakeZone welded world)" : weld ? " (weldMesh)" : "");
		for (const auto& [zx, zz] : zones)
		{
			const double x0 = kworld::kWorldMin + kworld::kZoneSize * zx, z0 = kworld::kWorldMin + kworld::kZoneSize * zz;
			const double x1 = x0 + kworld::kZoneSize, z1 = z0 + kworld::kZoneSize;
			ground::Stats st;
			std::map<std::string, std::string> names;
			const auto t0 = std::chrono::steady_clock::now();
			const ground::Grid grid = ground::makeGrid(terrain, x0, z0, x1, z1, prm.stride);
			const std::vector<ground::Cutter> cutters = ground::gatherCutters(data, terrain, grid.x0, grid.z0, grid.x0 + (grid.nx - 1) * grid.step,
				grid.z0 + (grid.nz - 1) * grid.step, nullptr);
			Mesh m = ground::generateBox(data, terrain, x0, z0, x1, z1, prm, {}, false, &st, nullptr, nullptr, &cutters);
			if (weld)
				ground::weldMesh(m);
			if (bake)	// the whole bake (surfgen zone sides, stitch, prune, weld): the welded world mesh; cutters above only attribute the walls
			{
				kworld::AreaGatherer gatherer(data, terrain);
				const knav::AreaGeometry area = gatherer.gather({ zx, zz }, { zx, zz });
				for (const auto& b : area.buildings)
					names[b.id] = b.placed.buildingID;
				const knav::NavConfig config = knav::configForWorldZones({ zx, zz }, { zx, zz });
				knav::ZoneGenReport report;
				report.wantWelded = true;
				if (buildings)	// the surfgen zone sides as generated, before the ground takes them as tops
				{
					std::size_t nb = 0, bd = 0, tiny = 0, faces = 0;
					for (const knav::GeneratedBuilding& g : knav::generateBuildings(area, nullptr, 4, &config))
					{
						std::size_t b = 0, t = 0;
						for (std::size_t e = 0; e < g.zoneSide.edges.size(); ++e)
							if (g.zoneSide.edges[e].oppositeFace < 0)
							{
								const auto& a = g.zoneSide.vertices[g.zoneSide.edges[e].a];
								const auto& c = g.zoneSide.vertices[g.zoneSide.edges[e].b];
								++b;
								t += std::hypot(a[0] - c[0], a[2] - c[2]) < 0.5;
							}
						if (g.zoneSide.faces.empty())
							continue;
						++nb;
						bd += b;
						tiny += t;
						faces += g.zoneSide.faces.size();
						if (std::getenv("KNAV_SIDES"))
							std::printf("#S %s\tfaces %zu\tboundary %zu\ttiny %zu\n", g.building->id.c_str(), g.zoneSide.faces.size(), b, t);
					}
					std::printf("#  surfgen zone sides: %zu buildings, %zu faces, %zu boundary edges (opposite-less, seam included), %zu under 0.5 u\n", nb, faces, bd, tiny);
				}
				knav::bakeZone(config, terrain, area, knav::zoneBakeSeeds(terrain, area, knav::readSeedsDef(data + "/navtiles"), { zx, zz }, config.waterLevel), 4, &report);
				m = std::move(report.weldedWorld);
				if (const char* rs = std::getenv("KNAV_REACH"))	// "x z x2 z2": is there a face at (x, z), and does the flood over shared edges reach (x2, z2)
				{
					double px[2], pz[2];
					std::sscanf(rs, "%lf %lf %lf %lf", &px[0], &pz[0], &px[1], &pz[1]);
					const auto faceAt = [&](double x, double z)
					{
						for (std::size_t f = 0; f < m.faces.size(); ++f)
						{
							bool pos = false, neg = false;
							for (std::uint32_t q = 0; q < m.faces[f].numEdges; ++q)
							{
								const auto& A = m.vertices[m.edges[m.faces[f].startEdge + q].a];
								const auto& B = m.vertices[m.edges[m.faces[f].startEdge + (q + 1) % m.faces[f].numEdges].a];
								const double cr = (B[0] - A[0]) * (z - A[2]) - (B[2] - A[2]) * (x - A[0]);
								pos |= cr > 0;
								neg |= cr < 0;
							}
							if (!(pos && neg))
								return (long)f;
						}
						return -1L;
					};
					const long f0 = faceAt(px[0], pz[0]), f1 = faceAt(px[1], pz[1]);
					std::size_t reached = 0;
					bool hit = false;
					if (f0 >= 0)
					{
						std::vector<char> seen(m.faces.size(), 0);
						std::vector<std::size_t> stack = { (std::size_t)f0 };
						seen[f0] = 1;
						while (!stack.empty())
						{
							const std::size_t f = stack.back();
							stack.pop_back();
							++reached;
							hit |= (long)f == f1;
							for (std::uint32_t q = 0; q < m.faces[f].numEdges; ++q)
							{
								const std::int32_t o = m.edges[m.faces[f].startEdge + q].oppositeFace;
								if (o >= 0 && !seen[o])
									seen[o] = 1, stack.push_back((std::size_t)o);
							}
						}
					}
					std::printf("#  reach: face at (%.0f,%.0f) %ld, at (%.0f,%.0f) %ld; flood %zu faces, second %s\n", px[0], pz[0], f0, px[1], pz[1], f1, reached, hit ? "REACHED" : "not reached");
				}
				std::printf("#  %d.%d collinear wall vertices dropped after the weld (the wall edges it saved): %zu\n", zx, zz, report.collinearDropped);
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			double ourLen = 0, shipLen = 0;
			std::size_t ourN = 0, shipN = 0, shipFaces = 0;
			std::size_t byMat[4] = {};
			double lenMat[4] = {};
			std::vector<int> owner;
			const std::vector<ground::Material> mat = ground::wallMaterials(m, cutters, prm, buildings ? &owner : nullptr);
			const Segs ours = walls(m, x0, z0, x1, z1, ourLen, ourN, &mat, byMat, lenMat);
			Segs shipped;
			const std::vector<Mesh> tile = knav::native::loadTile(dir, zx, zz);
			for (const Mesh& s : tile)
			{
				if (!s.info.empty())
					continue;
				shipFaces += s.faces.size();
				if (buildings)
				{
					std::size_t t = 0, w = 0;
					for (std::size_t e = 0; e < s.edges.size(); ++e)
						if (s.wall(e))
						{
							++w;
							t += std::hypot(s.vertices[s.edges[e].a][0] - s.vertices[s.edges[e].b][0], s.vertices[s.edges[e].a][2] - s.vertices[s.edges[e].b][2]) < 0.5;
						}
					std::printf("#  shipped wall edges %zu, under 0.5 u %zu\n", w, t);
				}
				double l;
				std::size_t n;
				const Segs w = walls(s, x0, z0, x1, z1, l, n);
				for (const auto& g : w.s)
					shipped.add(g[0], g[1], g[2], g[3]);
				shipLen += l;
				shipN += n;
			}
			std::printf("%d.%d\t%zu\t%zu\t%zu\t%.0f\t%zu\t%zu\t%.0f\t%.1f\t%.1f\t%.0f\t%zu\t%zu\t%zu\n", zx, zz, m.faces.size(), m.vertices.size(), ourN, ourLen, shipFaces, shipN, shipLen,
				share(ours, shipped, x0, z0, x1, z1), share(shipped, ours, x0, z0, x1, z1), ms, st.refusedLoop, st.refusedCorridor, st.refusedDepth);
			std::printf("#  %d.%d wall edges by material (count / mean u): building %zu / %.1f, terrain %zu / %.1f, feature %zu / %.1f, foliage %zu / %.1f; loops building %zu rim %zu terrain %zu, removed building %zu rim %zu terrain %zu, top collinear %zu\n",
				zx, zz, byMat[1], byMat[1] ? lenMat[1] / byMat[1] : 0.0, byMat[0], byMat[0] ? lenMat[0] / byMat[0] : 0.0, byMat[2], byMat[2] ? lenMat[2] / byMat[2] : 0.0,
				byMat[3], byMat[3] ? lenMat[3] / byMat[3] : 0.0, st.buildingLoops, st.rimLoops, st.terrainLoops, st.removedBuilding, st.removedRim, st.removedTerrain, st.topCollinearDropped);
			if (const char* at = std::getenv("KNAV_TRIS"))	// "label x z": the cutter triangles within 1.5 u (xz) of the point
			{
				char lab[128];
				double px, pz;
				if (std::sscanf(at, "%127s %lf %lf", lab, &px, &pz) == 3)
					for (const auto& c : cutters)
						if (c.label == lab)
							for (std::size_t t = 0; t + 2 < c.tris.size(); t += 3)
							{
								bool near = false;
								for (int k = 0; k < 3; ++k)
									near = near || (std::fabs(c.tris[t + k][0] - px) < 1.5 && std::fabs(c.tris[t + k][2] - pz) < 1.5);
								if (near)
									std::printf("#T top=%d (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f)\n", (int)c.top, c.tris[t][0], c.tris[t][1], c.tris[t][2], c.tris[t + 1][0], c.tris[t + 1][1], c.tris[t + 1][2], c.tris[t + 2][0], c.tris[t + 2][1], c.tris[t + 2][2]);
							}
			}
			if (features)
			{
				std::vector<Mesh> world;
				for (const Mesh& s : tile)
					if (s.info.empty())
						world.push_back(s);
				featureReport(m, mat, cutters, world, terrain);
			}
			if (buildings)
			{
				std::vector<Mesh> world;
				for (const Mesh& s : tile)
					if (s.info.empty())
						world.push_back(s);
				buildingReport(names, m, mat, owner, cutters, world, buildings);
			}
			std::fflush(stdout);
		}
	} catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
	return 0;
}
