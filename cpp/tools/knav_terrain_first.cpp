// Terrain-only first pass and straightening probe (kenshi-k16.7.213). Reads fullmap.tif only: no objects, tops or buildings.
//   knav_terrain_first [--data DIR] [--threads N] [--world] [--step S] [--eval] [--cfg SLOPY,SLOPXZ ...]
// One ground window per zone (the zone's own box, as the bake's ground pass), every configuration over the zones taken (every S-th in x and z, or
// all with --world). Per configuration: faces, vertices, wall ms; with --eval also the largest height error against the full-detail 18 u terrain
// samples (the polygon as a best-fit plane, and as the fan of its corners) and the outline drift in xz against the unsimplified outline.
// Configuration "prod" is the production Params; "SLOPY,SLOPXZ" turns Params::straighten on.
#include "knav/ground.hpp"
#include "knav/native_tiles.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{

namespace ground = knav::ground;
using Mesh = knav::native::Mesh;

struct Config
{
	std::string name;
	ground::Params params;
};

struct Total
{
	std::size_t faces = 0, verts = 0, edges = 0, maxFaceVerts = 0, zones = 0, empty = 0, over6 = 0;
	double ms = 0, maxPlane = 0, maxFan = 0, maxXZ = 0, maxXZRef = 0;
	std::size_t gsOver2 = 0, gsPoints = 0;
	double gsMax = 0;	// ground outline points not on a top face's edge
	std::size_t gOver2 = 0, gPoints = 0, tOver2 = 0, tPoints = 0;	// the same split by ground faces / top faces
	double gMax = 0, tMax = 0;
	std::size_t xzOver1 = 0, xzOver2 = 0, xzPoints = 0, outlineEdges = 0, refOver2 = 0, refGone = 0, refPoints = 0;
	double sumPlane = 0;
	std::size_t samples = 0;
	// plane error histogram (0.1 u bins, the last bin is >= 100 u) per face slope band
	std::array<std::vector<std::size_t>, 5> hist;
	// after nativeTiles (Detour polygons, tile 256 u)
	std::size_t tiles = 0, polys = 0, overfull = 0;
	int maxPolys = 0;
	void add(const Total& o)
	{
		for (std::size_t b = 0; b < hist.size(); ++b)
		{
			if (hist[b].size() < o.hist[b].size())
				hist[b].resize(o.hist[b].size());
			for (std::size_t i = 0; i < o.hist[b].size(); ++i)
				hist[b][i] += o.hist[b][i];
		}
	}
};

constexpr int kBands = 5;
const char* const kBandName[kBands] = { "0-5deg", "5-15deg", "15-30deg", "30-40deg", "all" };
constexpr double kBandMax[kBands - 1] = { 5, 15, 30, 90 };

// the error at fraction p (0..1) of a histogram, in u (bin upper edge)
double percentile(const std::vector<std::size_t>& h, double p, std::size_t* n = nullptr)
{
	std::size_t total = 0;
	for (const std::size_t c : h)
		total += c;
	if (n)
		*n = total;
	if (!total)
		return 0;
	std::size_t run = 0;
	for (std::size_t i = 0; i < h.size(); ++i)
	{
		run += h[i];
		if (run >= p * total)
			return (i + 1) * 0.1;
	}
	return h.size() * 0.1;
}

struct Segs
{
	std::vector<std::array<double, 4>> s;
	std::unordered_map<long long, std::vector<int>> cell;
	static constexpr double kCell = 16;
	static long long key(int cx, int cz) { return (long long)cx * 100003 + cz; }
	void add(double ax, double az, double bx, double bz)
	{
		s.push_back({ ax, az, bx, bz });
		const int i = (int)s.size() - 1;
		for (int cx = (int)std::floor(std::min(ax, bx) / kCell); cx <= (int)std::floor(std::max(ax, bx) / kCell); ++cx)
			for (int cz = (int)std::floor(std::min(az, bz) / kCell); cz <= (int)std::floor(std::max(az, bz) / kCell); ++cz)
				cell[key(cx, cz)].push_back(i);
	}
	double dist(double x, double z, double cap) const
	{
		double best = cap;
		const int r = (int)std::ceil(cap / kCell);
		for (int cx = (int)std::floor(x / kCell) - r; cx <= (int)std::floor(x / kCell) + r; ++cx)
			for (int cz = (int)std::floor(z / kCell) - r; cz <= (int)std::floor(z / kCell) + r; ++cz)
			{
				const auto it = cell.find(key(cx, cz));
				if (it == cell.end())
					continue;
				for (const int i : it->second)
				{
					const auto& g = s[i];
					const double dx = g[2] - g[0], dz = g[3] - g[1], l2 = dx * dx + dz * dz;
					const double t = l2 > 0 ? std::clamp(((x - g[0]) * dx + (z - g[1]) * dz) / l2, 0.0, 1.0) : 0.0;
					best = std::min(best, std::hypot(x - g[0] - t * dx, z - g[1] - t * dz));
				}
			}
		return best;
	}
};

// the outline: edges with no opposite face that are not a window rim
Segs outline(const Mesh& m, const std::vector<char>* top = nullptr, int want = 0)	// want: 0 all faces, 1 ground faces only, 2 top faces only
{
	Segs o;
	for (std::size_t fi = 0; fi < m.faces.size(); ++fi)
	{
		const knav::native::Face& f = m.faces[fi];
		if (top && !top->empty() && want && (*top)[fi] != (want == 2))
			continue;
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
		{
			const knav::native::Edge& e = m.edges[f.startEdge + k];
			if (e.oppositeFace < 0 && !e.linked)
				o.add(m.vertices[e.a][0], m.vertices[e.a][2], m.vertices[e.b][0], m.vertices[e.b][2]);
		}
	}
	return o;
}

double g_dump[3] = { 0, 0, 0 };	// --dump X,Z,R: print the outline segments within R of (X, Z), reference and each configuration
void dumpOutline(const char* tag, const Segs& o)
{
	for (const auto& g : o.s)
		if (std::min(std::hypot(g[0] - g_dump[0], g[1] - g_dump[1]), std::hypot(g[2] - g_dump[0], g[3] - g_dump[1])) < g_dump[2])
			std::fprintf(stderr, "dump %s (%.2f, %.2f)-(%.2f, %.2f)\n", tag, g[0], g[1], g[2], g[3]);
}

// the largest distance from points along `from`'s segments (every 1 u) to `to`; counts of points over 1 and 2 u
int g_outliers = 0;	// --outliers N: print the output outline points more than 2 u (and under 40) from the reference, at most N per call

double drift(const Segs& from, const Segs& to, std::size_t& over1, std::size_t& over2, std::size_t& points, std::size_t* gone = nullptr, const char* tag = nullptr, const Segs* seam = nullptr)
{
	int printed = 0;
	double worst = 0;
	for (const auto& g : from.s)
	{
		const double len = std::hypot(g[2] - g[0], g[3] - g[1]);
		const int n = std::max(1, (int)std::ceil(len));
		for (int i = 0; i <= n; ++i)
		{
			if (seam && seam->dist(g[0] + (g[2] - g[0]) * i / n, g[1] + (g[3] - g[1]) * i / n, 0.5) < 0.5)
				continue;	// a point on a top face's edge: a seam, not ground drift
			const double d = to.dist(g[0] + (g[2] - g[0]) * i / n, g[1] + (g[3] - g[1]) * i / n, 40.0);
			worst = std::max(worst, d);
			over1 += d > 1;
			over2 += d > 2;
			if (tag && d > 2 && d < 40 && printed < g_outliers)
			{
				++printed;
				std::fprintf(stderr, "outlier %s (%.1f, %.1f) d %.1f edge (%.1f, %.1f)-(%.1f, %.1f)\n", tag, g[0] + (g[2] - g[0]) * i / n, g[1] + (g[3] - g[1]) * i / n, d, g[0], g[1], g[2], g[3]);
			}
			if (gone)
				*gone += d >= 40.0;
			++points;
		}
	}
	return worst;
}

// every 18 u terrain sample under a face against the face
void heightError(const Mesh& m, const kworld::Heightmap& terrain, Total& t, const std::vector<char>* skip = nullptr)
{
	const double s = kworld::kHeightmapSpacing;
	for (std::size_t fi = 0; fi < m.faces.size(); ++fi)
	{
		if (skip && (*skip)[fi])
			continue;
		const knav::native::Face& f = m.faces[fi];
		std::vector<std::array<double, 3>> c;
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
			c.push_back(m.vertices[m.edges[f.startEdge + k].a]);
		// best-fit plane: area-weighted normal through the centroid
		std::array<double, 3> n = { 0, 0, 0 }, mid = { 0, 0, 0 };
		for (const auto& p : c)
			for (int k = 0; k < 3; ++k)
				mid[k] += p[k] / c.size();
		for (std::size_t k = 1; k + 1 < c.size(); ++k)
		{
			const double ux = c[k][0] - c[0][0], uy = c[k][1] - c[0][1], uz = c[k][2] - c[0][2], wx = c[k + 1][0] - c[0][0], wy = c[k + 1][1] - c[0][1], wz = c[k + 1][2] - c[0][2];
			n[0] += uy * wz - uz * wy;
			n[1] += uz * wx - ux * wz;
			n[2] += ux * wy - uy * wx;
		}
		if (std::fabs(n[1]) < 1e-9)
			continue;
		if (n[1] < 0)
			for (double& x : n)
				x = -x;
		const double slopeDeg = std::acos(std::min(1.0, n[1] / std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]))) * 180.0 / 3.14159265358979;
		int band = 0;
		while (band < kBands - 2 && slopeDeg >= kBandMax[band])
			++band;
		for (std::size_t k = 1; k + 1 < c.size(); ++k)
		{
			const auto &a = c[0], &b = c[k], &d = c[k + 1];
			const double det = (b[0] - a[0]) * (d[2] - a[2]) - (b[2] - a[2]) * (d[0] - a[0]);
			if (std::fabs(det) < 1e-9)
				continue;
			const double x0 = std::min({ a[0], b[0], d[0] }), x1 = std::max({ a[0], b[0], d[0] }), z0 = std::min({ a[2], b[2], d[2] }), z1 = std::max({ a[2], b[2], d[2] });
			for (int col = (int)std::ceil((x0 - kworld::kWorldMin) / s); col <= (int)std::floor((x1 - kworld::kWorldMin) / s); ++col)
				for (int row = (int)std::ceil((z0 - kworld::kWorldMin) / s); row <= (int)std::floor((z1 - kworld::kWorldMin) / s); ++row)
				{
					const double x = kworld::kWorldMin + col * s, z = kworld::kWorldMin + row * s;
					const double u = ((x - a[0]) * (d[2] - a[2]) - (z - a[2]) * (d[0] - a[0])) / det, v = ((b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0])) / det;
					if (u < -1e-9 || v < -1e-9 || u + v > 1 + 1e-9)
						continue;
					if (col >= terrain.sampleCountX() || row >= terrain.sampleCountZ())
						continue;
					const double y = terrain.sampleHeight(col, row);
					const double fan = a[1] + u * (b[1] - a[1]) + v * (d[1] - a[1]);
					const double plane = mid[1] - (n[0] * (x - mid[0]) + n[2] * (z - mid[2])) / n[1];
					t.maxFan = std::max(t.maxFan, std::fabs(y - fan));
					const double e = std::fabs(y - plane);
					t.maxPlane = std::max(t.maxPlane, e);
					t.sumPlane += e;
					++t.samples;
					const std::size_t bin = std::min<std::size_t>((std::size_t)(e * 10), 1000);
					for (const int b : { band, kBands - 1 })
					{
						if (t.hist[b].size() <= bin)
							t.hist[b].resize(bin + 1);
						++t.hist[b][bin];
					}
				}
		}
	}
}

} // namespace

int main(int argc, char** argv)
{
	std::string data = KENSHI_DATA;
	int threads = 4, step = 8;
	bool world = false, eval = false, tiles = false, cut = false;
	std::vector<kworld::ZoneCell> only;
	std::vector<Config> cfgs;
	const auto prod = []
	{
		Config c;
		c.name = "prod";
		return c;
	};
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--data" && i + 1 < argc)
			data = argv[++i];
		else if (a == "--threads" && i + 1 < argc)
			threads = std::atoi(argv[++i]);
		else if (a == "--step" && i + 1 < argc)
			step = std::atoi(argv[++i]);
		else if (a == "--world")
			world = true;
		else if (a == "--eval")
			eval = true;
		else if (a == "--outliers" && i + 1 < argc)
			g_outliers = std::atoi(argv[++i]);
		else if (a == "--dump" && i + 1 < argc)
			std::sscanf(argv[++i], "%lf,%lf,%lf", &g_dump[0], &g_dump[1], &g_dump[2]);
		else if (a == "--tiles")
			tiles = true;
		else if (a == "--cut")
			cut = true;
		else if (a == "--zones" && i + 1 < argc)	// X0,Z0,X1,Z1: the zone cells of a block (inclusive)
		{
			int x0, z0, x1, z1;
			if (std::sscanf(argv[++i], "%d,%d,%d,%d", &x0, &z0, &x1, &z1) != 4)
				return 2;
			for (int z = z0; z <= z1; ++z)
				for (int x = x0; x <= x1; ++x)
					only.push_back({ x, z });
		}
		else if (a == "--cfg" && i + 1 < argc)
		{
			// prod | SLOPY,SLOPXZ[,key=value...]  keys: trim (trimWidth), post (trimAfterSimplify 0|1), keep (keepRim 0|1)
			const std::string v = argv[++i];
			{
				Config c;
				c.name = v;
				c.params.straighten = false;
				c.params.straighten = v.rfind("prod", 0) != 0;
				if (c.params.straighten)
					std::sscanf(v.c_str(), "%lf,%lf", &c.params.slopY, &c.params.slopXZ);
				for (std::size_t at = v.find(','); at != std::string::npos; at = v.find(',', at + 1))
				{
					const std::string kv = v.substr(at + 1);
					if (kv.rfind("trim=", 0) == 0)
						c.params.trimWidth = std::atof(kv.c_str() + 5);
					else if (kv.rfind("post=", 0) == 0)
						c.params.trimAfterSimplify = std::atoi(kv.c_str() + 5) != 0;
				}
				cfgs.push_back(c);
			}
		}
		else {
			std::fprintf(stderr, "usage: knav_terrain_first [--data DIR] [--threads N] [--world | --step S | --zones X0,Z0,X1,Z1] [--cut] [--eval] [--tiles] [--cfg prod|SLOPY,SLOPXZ[,trim=W][,post=0|1]]...\n");
			return 2;
		}
	}
	if (cfgs.empty())
		cfgs.push_back(prod());
	const kworld::Heightmap terrain = kworld::Heightmap::open(data + "/newland/land/fullmap.tif");
	std::vector<kworld::ZoneCell> zones = only;
	if (zones.empty())
		for (int z = 0; z < kworld::kZoneCellsPerAxis; z += world ? 1 : step)
			for (int x = 0; x < kworld::kZoneCellsPerAxis; x += world ? 1 : step)
				zones.push_back({ x, z });
	// the reference: every removal rule off (a negative limit is never met: no vertex goes, not even a coplanar one; with limits of 0 the
	// reference had slits where a coplanar removal left a pinched star, and the drift counted those)
	ground::Params refParams;
	refParams.heightError = -1;
	refParams.borderArea = refParams.concaveArea = refParams.borderHeight = refParams.concaveHeight = -1;
	knav::NavConfig navCfg;
	navCfg.originX = navCfg.originZ = kworld::kWorldMin;
	navCfg.tilesX = navCfg.tilesZ = (int)(-2 * kworld::kWorldMin / navCfg.tileSize);

	std::vector<Total> total(cfgs.size());
	Total ref;
	std::mutex lock;
	std::atomic<std::size_t> next{ 0 };
	const auto wall0 = std::chrono::steady_clock::now();
	// one window's mesh: the terrain alone, or the cut window (cutters + tops); `skip` marks the top faces
	const auto make = [&](const kworld::ZoneCell zc, const ground::Grid& grid, const ground::Params& prm, std::vector<char>& skip)
	{
		if (!cut)
		{
			skip.clear();
			ground::Stats st;
			Mesh m = ground::generate(grid, {}, {}, prm, &st);
			if (g_dump[2] > 0)
				std::fprintf(stderr, "stats %s: faces %zu comps %zu droppedComps %zu droppedFaces %zu removed %zu trimmed %zu post %zu\n", &prm == &refParams ? "ref" : prm.straighten ? "straight" : "prod",
					m.faces.size(), st.components, st.droppedComponents, st.droppedFaces, st.removed, st.trimmedTips, st.postTrimmedTips);
			return m;
		}
		const double x0 = kworld::kWorldMin + kworld::kZoneSize * zc.x, z0 = kworld::kWorldMin + kworld::kZoneSize * zc.z;
		ground::Stats st;
		ground::CutterCounts counts;
		Mesh m = ground::generateBox(data, terrain, x0, z0, x0 + kworld::kZoneSize, z0 + kworld::kZoneSize, prm, {}, true, &st, &counts);
		if (&prm == &refParams)
			std::fprintf(stderr, "zone %d,%d: cutters %zu (buildings %zu, masks %zu, foliage %zu, features %zu), tops %zu\n", zc.x, zc.z, st.cutters, counts.buildingShapes, counts.maskShapes,
				counts.foliage, counts.features, st.topFaceIndex.size());
		skip.assign(m.faces.size(), 0);
		for (const std::uint32_t f : st.topFaceIndex)
			if (f < skip.size())
				skip[f] = 1;
		return m;
	};
	const auto work = [&]
	{
		for (;;)
		{
			const std::size_t zi = next++;
			if (zi >= zones.size())
				return;
			const kworld::ZoneCell zc = zones[zi];
			const double x0 = kworld::kWorldMin + kworld::kZoneSize * zc.x, z0 = kworld::kWorldMin + kworld::kZoneSize * zc.z;
			const ground::Grid grid = ground::makeGrid(terrain, x0, z0, x0 + kworld::kZoneSize, z0 + kworld::kZoneSize, 2);
			std::vector<char> skip;
			Segs refOutline, refOutlineNoTrim;	// the reference for the configurations with the trim on / off
			Segs refGround, refTop;
			if (eval)
			{
				ground::Params noTrim = refParams;
				noTrim.trimWidth = 0;
				refOutlineNoTrim = outline(make(zc, grid, noTrim, skip));
				Mesh r = make(zc, grid, refParams, skip);
				refOutline = outline(r);
				refGround = outline(r, &skip, 1);
				refTop = outline(r, &skip, 2);
				Total one;
				heightError(r, terrain, one, skip.empty() ? nullptr : &skip);
				std::lock_guard<std::mutex> g(lock);
				ref.faces += r.faces.size();
				ref.verts += r.vertices.size();
				ref.maxPlane = std::max(ref.maxPlane, one.maxPlane);
				ref.maxFan = std::max(ref.maxFan, one.maxFan);
				ref.add(one);
				++ref.zones;
			}
			for (std::size_t c = 0; c < cfgs.size(); ++c)
			{
				const auto t0 = std::chrono::steady_clock::now();
				Mesh m = make(zc, grid, cfgs[c].params, skip);
				const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				Total one;
				std::size_t mfv = 0, o6 = 0;
				for (const knav::native::Face& f : m.faces)
				{
					mfv = std::max<std::size_t>(mfv, f.numEdges);
					o6 += f.numEdges > 6;
				}
				if (eval)
				{
					heightError(m, terrain, one, skip.empty() ? nullptr : &skip);
					const Segs out = outline(m);
					if (g_dump[2] > 0)
					{
						dumpOutline("ref", refOutline);
						dumpOutline(cfgs[c].name.c_str(), out);
					}
					const Segs& rf = cfgs[c].params.trimWidth > 0 ? refOutline : refOutlineNoTrim;
					one.maxXZ = drift(out, rf, one.xzOver1, one.xzOver2, one.xzPoints, nullptr, cfgs[c].name.c_str());
					std::size_t a = 0, p = 0;
					one.maxXZRef = drift(rf, out, a, one.refOver2, p, &one.refGone);
					one.refPoints = p;
					one.outlineEdges = out.s.size();
					if (!skip.empty())
					{
						std::size_t o1 = 0, a2 = 0;
						one.gMax = drift(outline(m, &skip, 1), refGround, o1, one.gOver2, one.gPoints, nullptr, nullptr);
						one.tMax = drift(outline(m, &skip, 2), refTop, o1, one.tOver2, one.tPoints, nullptr, nullptr);
						Segs topEdges;
						for (std::size_t fi = 0; fi < m.faces.size(); ++fi)
							if (skip[fi])
								for (std::uint32_t k = 0; k < m.faces[fi].numEdges; ++k)
								{
									const knav::native::Edge& e = m.edges[m.faces[fi].startEdge + k];
									topEdges.add(m.vertices[e.a][0], m.vertices[e.a][2], m.vertices[e.b][0], m.vertices[e.b][2]);
								}
						one.gsMax = drift(outline(m, &skip, 1), refGround, a2, one.gsOver2, one.gsPoints, nullptr, nullptr, &topEdges);
					}
				}
				if (tiles)
				{
					std::vector<knav::TileCoord> coords;
					const int per = (int)(kworld::kZoneSize / navCfg.tileSize), tx0 = zc.x * per, tz0 = zc.z * per;
					for (int tz = 0; tz < per; ++tz)
						for (int tx = 0; tx < per; ++tx)
							coords.push_back({ tx0 + tx, tz0 + tz });
					knav::NativeTileStats ns;
					const auto built = knav::nativeTiles(navCfg, { m }, coords, {}, &ns);
					one.tiles = built.size();
					one.polys = ns.polys;
					one.overfull = ns.overfull;
					one.maxPolys = ns.maxPolys;
				}
				std::lock_guard<std::mutex> g(lock);
				Total& t = total[c];
				t.faces += m.faces.size();
				t.verts += m.vertices.size();
				t.edges += m.edges.size();
				t.maxFaceVerts = std::max(t.maxFaceVerts, mfv);
				t.over6 += o6;
				t.empty += m.faces.empty();
				++t.zones;
				t.ms += ms;
				t.maxPlane = std::max(t.maxPlane, one.maxPlane);
				t.maxFan = std::max(t.maxFan, one.maxFan);
				t.sumPlane += one.sumPlane;
				t.samples += one.samples;
				t.add(one);
				t.maxXZ = std::max(t.maxXZ, one.maxXZ);
				t.maxXZRef = std::max(t.maxXZRef, one.maxXZRef);
				t.xzOver1 += one.xzOver1;
				t.xzOver2 += one.xzOver2;
				t.xzPoints += one.xzPoints;
				t.outlineEdges += one.outlineEdges;
				t.refOver2 += one.refOver2;
				t.refGone += one.refGone;
				t.refPoints += one.refPoints;
				t.gOver2 += one.gOver2;
				t.gsOver2 += one.gsOver2;
				t.gsPoints += one.gsPoints;
				t.gsMax = std::max(t.gsMax, one.gsMax);
				t.gPoints += one.gPoints;
				t.tOver2 += one.tOver2;
				t.tPoints += one.tPoints;
				t.gMax = std::max(t.gMax, one.gMax);
				t.tMax = std::max(t.tMax, one.tMax);
				t.tiles += one.tiles;
				t.polys += one.polys;
				t.overfull += one.overfull;
				t.maxPolys = std::max(t.maxPolys, one.maxPolys);
			}
		}
	};
	std::vector<std::thread> pool;
	for (int i = 0; i < threads; ++i)
		pool.emplace_back(work);
	for (auto& t : pool)
		t.join();
	const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall0).count();
	std::printf("zones %zu, threads %d, wall %.1f s%s%s\n", zones.size(), threads, wall, cut ? " (cut windows)" : " (terrain only)", eval ? " (with eval and reference)" : "");
	const auto bands = [](const char* name, const Total& t)
	{
		for (int b = 0; b < kBands; ++b)
		{
			std::size_t n = 0;
			const double p99 = percentile(t.hist[b], 0.99, &n);
			std::printf("         %-8s plane-error band %-9s samples %zu p50 %.1f p90 %.1f p99 %.1f p99.9 %.1f over100u %zu\n", name, kBandName[b], n, percentile(t.hist[b], 0.5), percentile(t.hist[b], 0.9),
				p99, percentile(t.hist[b], 0.999), t.hist[b].size() > 1000 ? t.hist[b][1000] : 0);
		}
	};
	if (eval)
	{
		std::printf("ref (no removal rule): faces %zu verts %zu, 18u-sample error vs plane %.2f fan %.2f (the 36 u grid's own error)\n", ref.faces, ref.verts, ref.maxPlane, ref.maxFan);
		bands("ref", ref);
	}
	for (std::size_t c = 0; c < cfgs.size(); ++c)
	{
		const Total& t = total[c];
		std::printf("%-8s faces %zu verts %zu edges %zu maxFaceVerts %zu over6 %zu emptyZones %zu cpu-ms %.0f", cfgs[c].name.c_str(), t.faces, t.verts, t.edges, t.maxFaceVerts,
			t.over6, t.empty, t.ms);
		if (eval)
			std::printf(" | height max plane %.2f fan %.2f mean %.2f | xz drift out->ref %.2f ref->out %.2f; points >1u %zu >2u %zu of %zu", t.maxPlane, t.maxFan,
				t.samples ? t.sumPlane / t.samples : 0.0, t.maxXZ, t.maxXZRef, t.xzOver1, t.xzOver2, t.xzPoints);
		if (eval)
			std::printf("\n         outline edges %zu | ref->out points >2u %zu (outline gone, >=40 u: %zu) of %zu", t.outlineEdges, t.refOver2, t.refGone, t.refPoints);
		if (eval && t.gPoints + t.tPoints)
			std::printf("\n         drift split: ground-face outline max %.2f, >2u %zu of %zu | top-face outline max %.2f, >2u %zu of %zu", t.gMax, t.gOver2, t.gPoints, t.tMax, t.tOver2, t.tPoints);
		if (eval && t.gsPoints)
			std::printf("\n         ground outline off top-face edges: max %.2f, >2u %zu of %zu", t.gsMax, t.gsOver2, t.gsPoints);
		if (tiles)
			std::printf("\n         tiles %zu detour polys %zu (max %d per tile, over the cap %zu)", t.tiles, t.polys, t.maxPolys, t.overfull);
		std::printf("\n");
		if (eval)
			bands(cfgs[c].name.c_str(), t);
	}
	return 0;
}
