#include "knav/ground.hpp"

#include "knav/surfgen.hpp"

#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"

#include <algorithm>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace knav::ground
{

namespace
{

using P3 = std::array<double, 3>;

// The signed loop area a corner removal takes away: the loop runs b -> v -> a (the boundary edge v -> a leaves v; a = the ring's first vertex)
double loopCut(const P3& a, const P3& v, const P3& b)
{
	return 0.5 * ((b[0] * v[2] - v[0] * b[2]) + (v[0] * a[2] - a[0] * v[2]) - (b[0] * a[2] - a[0] * b[2]));
}

double segDist(const P3& a, const P3& b, const P3& c)	// xz distance of point c to segment a-b
{
	const double dx = b[0] - a[0], dz = b[2] - a[2], len2 = dx * dx + dz * dz;
	const double u = len2 < 1e-12 ? 0 : std::clamp(((c[0] - a[0]) * dx + (c[2] - a[2]) * dz) / len2, 0.0, 1.0);
	return std::hypot(c[0] - (a[0] + u * dx), c[2] - (a[2] + u * dz));
}

// Orientation in xz: > 0 when a, b, c turn counter-clockwise (x right, z up on the page).
double cross2(const P3& a, const P3& b, const P3& c)
{
	return (b[0] - a[0]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[0] - a[0]);
}

// The triangle's normal, length = twice its area, y > 0 for a counter-clockwise triangle.
P3 normal(const P3& a, const P3& b, const P3& c)
{
	const P3 u = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	return { w[1] * u[2] - w[2] * u[1], w[2] * u[0] - w[0] * u[2], w[0] * u[1] - w[1] * u[0] };
}

double length(const P3& v)
{
	return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// The height of the triangle's plane at q (xz), and whether q lies in the triangle (with a small tolerance).
bool heightAt(const P3& a, const P3& b, const P3& c, const P3& q, double& y)
{
	const double d = cross2(a, b, c);
	if (d <= 0)
		return false;
	const double la = cross2(q, b, c) / d, lb = cross2(a, q, c) / d, lc = cross2(a, b, q) / d;
	constexpr double eps = -1e-7;
	if (la < eps || lb < eps || lc < eps)
		return false;
	y = la * a[1] + lb * b[1] + lc * c[1];
	return true;
}

// Segments p0-p1 and q0-q1 cross in xz at a point inside both (touching ends do not count).
bool properCross(const P3& p0, const P3& p1, const P3& q0, const P3& q1)
{
	const double d1 = cross2(p0, p1, q0), d2 = cross2(p0, p1, q1), d3 = cross2(q0, q1, p0), d4 = cross2(q0, q1, p1);
	return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
}

// The two triangles of a terrain cell a (i,j), b (i+1,j), c (i,j+1), d (i+1,j+1): always split on a-d, as the original does (every shipped
// vertex that tells the diagonals apart lies on it; kenshi-ffmi.24.1). Every place that splits a cell goes through here.
template <class T>
std::array<std::array<T, 3>, 2> cellSplit(const T& a, const T& b, const T& c, const T& d)
{
	return { { { a, b, d }, { a, d, c } } };
}

struct Tri
{
	std::array<int, 3> v;
	bool alive = true;
	std::vector<int> pts;	// removed samples (vertex ids) that lie over this triangle
};

using P2 = std::array<double, 2>;

// f(x, z) = a x + b z + c, |(a, b)| = 1: a signed distance in xz
struct Line
{
	double a, b, c;
	double at(const P2& p) const { return a * p[0] + b * p[1] + c; }
};

double area2(const std::vector<P2>& p)
{
	double s = 0;
	for (size_t i = 0; i < p.size(); ++i)
	{
		const P2& u = p[i], & w = p[(i + 1) % p.size()];
		s += u[0] * w[1] - u[1] * w[0];
	}
	return s;
}

// Splits a convex polygon by a line. False when the line does not cross it (out untouched). The crossing point is computed from the
// lexicographically smaller end, so both sides of a shared edge get the same bits.
bool splitBy(const std::vector<P2>& p, const Line& L, std::vector<P2>& neg, std::vector<P2>& pos)
{
	constexpr double eps = 1e-4;	// the vertex merge tolerance: a point that close to the line is on it
	std::vector<double> f(p.size());
	bool below = false, above = false;
	for (size_t i = 0; i < p.size(); ++i)
	{
		f[i] = L.at(p[i]);
		below = below || f[i] < -eps;
		above = above || f[i] > eps;
	}
	if (!below || !above)
		return false;
	neg.clear();
	pos.clear();
	for (size_t i = 0; i < p.size(); ++i)
	{
		const size_t j = (i + 1) % p.size();
		if (f[i] <= eps)
			neg.push_back(p[i]);
		if (f[i] >= -eps)
			pos.push_back(p[i]);
		if ((f[i] < -eps && f[j] > eps) || (f[i] > eps && f[j] < -eps))
		{
			const bool flip = p[j] < p[i];
			const P2& u = flip ? p[j] : p[i], & w = flip ? p[i] : p[j];
			const double fu = flip ? f[j] : f[i], fw = flip ? f[i] : f[j];
			const double t = fu / (fu - fw);
			const P2 x = { u[0] + t * (w[0] - u[0]), u[1] + t * (w[1] - u[1]) };
			neg.push_back(x);
			pos.push_back(x);
		}
	}
	return true;
}

struct Box2
{
	double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
	void add(double x, double z) { x0 = std::min(x0, x); z0 = std::min(z0, z); x1 = std::max(x1, x); z1 = std::max(z1, z); }
	bool overlaps(const Box2& o, double tol = 1e-6) const { return x0 <= o.x1 + tol && o.x0 <= x1 + tol && z0 <= o.z1 + tol && o.z0 <= z1 + tol; }
	bool contains(const P2& p) const { return p[0] >= x0 - 1e-9 && p[0] <= x1 + 1e-9 && p[1] >= z0 - 1e-9 && p[1] <= z1 + 1e-9; }
};

Box2 boxOf(const std::vector<P2>& p)
{
	Box2 b;
	for (const P2& q : p)
		b.add(q[0], q[1]);
	return b;
}

// A cutter triangle prepared for the cut: xz box, edge lines, its plane y = A x + B z + C (flat: false when it stands vertical in xz).
struct CutTri
{
	P3 v[3];
	Box2 box;
	bool flat = false;
	double A = 0, B = 0, C = 0;
	Line edge[3];
	double sign = 1;	// orientation of v in xz
};

// A Projected cutter's wall seen from above: the xz segment of a vertical triangle (deduplicated), the outline of its footprint.
struct Wall
{
	P2 a, b;
	Box2 box;
	Line L;
};

struct PreparedCutter
{
	const Cutter* src = nullptr;
	Box2 box;
	std::vector<CutTri> tris;
	std::vector<Wall> walls;	// Projected only
	bool band = false;	// cuts only where a surface lies in the rwh293 band (ground + cutBase .. + characterHeight): no solid inferred from hit parity or pairs
	// xz cells of 32 u -> triangles whose box touches the cell, built on first use (the deck test's vertical queries)
	mutable std::unordered_map<uint64_t, std::vector<int>> cells;
	mutable bool indexed = false;
};

constexpr double kIndexCell = 32;
uint64_t cellKeyOf(int64_t cx, int64_t cz)
{
	return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}

// The cell index of a cutter's flat triangles, built on first use.
void buildIndex(const PreparedCutter& pc)
{
	if (pc.indexed)
		return;
	pc.indexed = true;
	for (size_t k = 0; k < pc.tris.size(); ++k)
	{
		const CutTri& t = pc.tris[k];
		if (!t.flat)
			continue;
		for (int64_t cx = (int64_t)std::floor(t.box.x0 / kIndexCell); cx <= (int64_t)std::floor(t.box.x1 / kIndexCell); ++cx)
			for (int64_t cz = (int64_t)std::floor(t.box.z0 / kIndexCell); cz <= (int64_t)std::floor(t.box.z1 / kIndexCell); ++cz)
				pc.cells[cellKeyOf(cx, cz)].push_back((int)k);
	}
}

// The flat triangles whose box may touch `box` grown by `tol`, in the cutter's own order: the cells under the box, or every triangle when the
// cutter is small or the box covers more cells than it has triangles. The caller still tests each box (kenshi-k16.7.212: the cut and its drop
// test walked every triangle of a cutter for every piece).
void trisNear(const PreparedCutter& pc, const Box2& box, double tol, std::vector<int>& out)
{
	out.clear();
	const int64_t cx0 = (int64_t)std::floor((box.x0 - tol) / kIndexCell), cx1 = (int64_t)std::floor((box.x1 + tol) / kIndexCell);
	const int64_t cz0 = (int64_t)std::floor((box.z0 - tol) / kIndexCell), cz1 = (int64_t)std::floor((box.z1 + tol) / kIndexCell);
	if (pc.tris.size() <= 16 || (double)(cx1 - cx0 + 1) * (double)(cz1 - cz0 + 1) >= (double)pc.tris.size())
	{
		for (size_t k = 0; k < pc.tris.size(); ++k)
			if (pc.tris[k].flat)
				out.push_back((int)k);
		return;
	}
	buildIndex(pc);
	for (int64_t cx = cx0; cx <= cx1; ++cx)
		for (int64_t cz = cz0; cz <= cz1; ++cz)
		{
			const auto it = pc.cells.find(cellKeyOf(cx, cz));
			if (it != pc.cells.end())
				out.insert(out.end(), it->second.begin(), it->second.end());
		}
	if (cx0 != cx1 || cz0 != cz1)
	{
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
	}
}

PreparedCutter prepare(const Cutter& c)
{
	PreparedCutter pc;
	pc.src = &c;
	pc.band = !c.projected && (c.label == "foliage" || c.label.rfind("feature @", 0) == 0);
	pc.box.add(c.min[0], c.min[2]);
	pc.box.add(c.max[0], c.max[2]);
	for (size_t k = 0; k + 2 < c.tris.size(); k += 3)
	{
		CutTri t;
		for (int i = 0; i < 3; ++i)
		{
			t.v[i] = c.tris[k + i];
			t.box.add(t.v[i][0], t.v[i][2]);
		}
		const double d = cross2(t.v[0], t.v[1], t.v[2]);
		if (std::fabs(d) < 1e-6)
		{
			pc.tris.push_back(t);	// vertical: no hits, no edges (its neighbours' edges outline it)
			if (c.projected)
			{
				// the xz segment: the two vertices farthest apart
				int bi = 0, bj = 1;
				double best = -1;
				for (int i = 0; i < 3; ++i)
					for (int j = i + 1; j < 3; ++j)
					{
						const double l = std::hypot(t.v[i][0] - t.v[j][0], t.v[i][2] - t.v[j][2]);
						if (l > best) { best = l; bi = i; bj = j; }
					}
				Wall w;
				w.a = { t.v[bi][0], t.v[bi][2] };
				w.b = { t.v[bj][0], t.v[bj][2] };
				if (best < 1e-6)
					continue;
				if (w.b < w.a)
					std::swap(w.a, w.b);
				bool dup = false;
				for (const Wall& o : pc.walls)
					dup = dup || (std::fabs(o.a[0] - w.a[0]) < 1e-6 && std::fabs(o.a[1] - w.a[1]) < 1e-6 && std::fabs(o.b[0] - w.b[0]) < 1e-6 && std::fabs(o.b[1] - w.b[1]) < 1e-6);
				if (dup)
					continue;
				const double dx = w.b[0] - w.a[0], dz = w.b[1] - w.a[1];
				w.box.add(w.a[0], w.a[1]);
				w.box.add(w.b[0], w.b[1]);
				w.L = Line{ -dz / best, dx / best, (dz * w.a[0] - dx * w.a[1]) / best };
				pc.walls.push_back(w);
			}
			continue;
		}
		t.flat = true;
		t.sign = d > 0 ? 1 : -1;
		const P3 n = normal(t.v[0], t.v[1], t.v[2]);	// n . (p - v0) = 0  ->  y = v0.y - (n.x (x - v0.x) + n.z (z - v0.z)) / n.y
		t.A = -n[0] / n[1];
		t.B = -n[2] / n[1];
		t.C = t.v[0][1] - t.A * t.v[0][0] - t.B * t.v[0][2];
		for (int i = 0; i < 3; ++i)
		{
			const P3& p = t.v[i], & q = t.v[(i + 1) % 3];
			const double dx = q[0] - p[0], dz = q[2] - p[2], len = std::hypot(dx, dz);
			// cross2(p, q, x) / len: > 0 left of p -> q
			t.edge[i] = len > 0 ? Line{ -dz / len, dx / len, (dz * p[0] - dx * p[2]) / len } : Line{ 0, 0, 1 };
		}
		pc.tris.push_back(t);
	}
	return pc;
}

// The cutter's surface heights straight over / under q (xz).
void hitsAt(const PreparedCutter& pc, const P2& q, std::vector<double>& hits, std::vector<int>& near)
{
	hits.clear();
	Box2 at;
	at.add(q[0], q[1]);
	trisNear(pc, at, 1e-9, near);
	for (const int k : near)
	{
		const CutTri& t = pc.tris[k];
		if (!t.box.contains(q))
			continue;
		bool inside = true;
		for (int i = 0; i < 3 && inside; ++i)
			inside = t.sign * t.edge[i].at(q) >= -1e-9;
		if (inside)
			hits.push_back(t.A * q[0] + t.B * q[1] + t.C);
	}
	std::sort(hits.begin(), hits.end());
}

// A top face's edge linked to another face's edge: the sub-edge a0 -> a1 of face fa against b0 -> b1 of face fb (vertex ids, after the splits).
struct Link
{
	int fa, fb, a0, a1, b0, b1;
};

class Gen
{
public:
	Gen(const Grid& grid, const Params& params) : g_(grid), prm_(params), cosSlope_(std::cos(params.slopeDeg * 3.14159265358979 / 180.0)) {}

	void build(Stats& st);
	void cut(const std::vector<Cutter>& cutters, Stats& st);
	void trim(Stats& st);
	void simplify(Stats& st);
	void prepareCutters(const std::vector<Cutter>& cutters)
	{
		pcs_.clear();
		static const Cutter none;	// a prebuilt top is no solid: an empty cutter keeps the indexes
		for (const Cutter& c : cutters)
			{
			pcs_.push_back(prepare(c.prebuilt ? none : c));
			if (c.prebuilt)
				pcs_.back().box = Box2{};
		}
	}
	void tops(const std::vector<Cutter>& cutters, const Params& prm, Stats& st);
	bool buildingAt(const P2& q, double y) const;	// a building cutter takes the ground out at q (xz) over height y
	native::Mesh finish(const std::vector<std::array<double, 2>>& seeds, Stats& st);

private:
	struct Plan
	{
		double cost = 0;
		std::vector<int> ring;
		std::vector<std::array<int, 3>> tris;
		double key = 0;		// the queue order: an inner vertex by its height error, a border vertex after those by its cap area (smallest first, the original's order)
		double cut = 0;		// a border removal: the signed loop area it takes away
		bool border = false;
		int pa = -1, pb = -1;	// straighten: the border chord the removal makes and the original outline points it now stands for
		std::vector<P3> owned;
	};
	std::unordered_map<int64_t, std::vector<int>> outline_;	// straighten: the original outline vertices by 8 u cell (pinch guard)
	static int64_t cellKey(int cx, int cz) { return ((int64_t)cx << 32) ^ (uint32_t)cz; }
	std::unordered_map<int, std::vector<int>> vcomp_;	// straighten: vertex -> the pieces its triangles belong to
	std::unordered_set<int> pinchV_;	// straighten: vertices where two pieces touch (a zero-width pinch)
	void indexOutline();
	bool pinched(const Plan& plan, int v) const;
	bool loopGuard(int v, int a, int b, double cut) const;
	void indexLoops();
	void classifyBorder(Stats& st);
	std::vector<uint8_t> bld_, nearRim_;	// materialRule: per vertex, on a building-owned border loop / on a terrain loop within rimBand of the window rim
	void addLoopSeg(int s, int e, int loop);
	std::vector<int> loopOf_;	// vertex -> its border loop (-1: none, or a pinched one: no guard)
	std::vector<double> loopArea_;	// the loops' signed xz area, carried through the removals
	struct LoopSeg { int s, e, loop; };
	std::vector<LoopSeg> loopSegs_;	// the loops' edges, the removed ones skipped; a chord is added when a corner goes
	std::unordered_map<int64_t, std::vector<int>> loopCells_;	// 8 u cells -> loopSegs_ indexes
public:
	std::size_t refusedLoop_ = 0, refusedCorridor_ = 0, refusedDepth_ = 0;	// evaluations a guard refused (a vertex is evaluated more than once)
private:
	std::map<std::pair<int, int>, std::vector<P3>> dropped_;	// straighten: per border edge (smaller id first), the outline points dropped under it
	static std::pair<int, int> edgeKey(int a, int b) { return { std::min(a, b), std::max(a, b) }; }

	bool walkable(const P3& a, const P3& b, const P3& c) const
	{
		const P3 n = normal(a, b, c);
		const double len = length(n);
		return len > 1e-12 && n[1] / len >= cosSlope_;
	}
	bool earClip(const std::vector<int>& poly, std::vector<std::array<int, 3>>& out) const;
	bool evaluate(int v, Plan& plan);
	void apply(int v, const Plan& plan);
	void addTri(const std::array<int, 3>& v, std::vector<int> pts)
	{
		T_.push_back({ v, true, std::move(pts) });
		for (const int k : v)
			vt_[k].push_back((int)T_.size() - 1);
	}
	int addVertex(const P3& p)
	{
		P_.push_back(p);
		vt_.emplace_back();
		const double tol = 1e-6 * std::max(1.0, g_.step);
		const double x1 = g_.x0 + (g_.nx - 1) * g_.step, z1 = g_.z0 + (g_.nz - 1) * g_.step;
		const uint8_t rim = (std::fabs(p[0] - g_.x0) < tol ? 1 : 0) | (std::fabs(p[0] - x1) < tol ? 2 : 0) | (std::fabs(p[2] - g_.z0) < tol ? 4 : 0)
			| (std::fabs(p[2] - z1) < tol ? 8 : 0);
		rim_.push_back(rim);
		fixed_.push_back(rim != 0);	// a cut point on the rim: the neighbouring window makes the same one
		removed_.push_back(0);
		rimFree_.push_back(0);
		stamp_.push_back(0);
		return (int)P_.size() - 1;
	}

	const Grid& g_;
	const Params& prm_;
	const double cosSlope_;
	std::vector<P3> P_;
	std::vector<Tri> T_;
	std::vector<std::vector<int>> vt_;	// vertex -> triangles (dead ones dropped lazily)
	std::vector<uint8_t> fixed_, removed_, rimFree_, rim_;	// rim_: bit per window side
	std::vector<uint32_t> stamp_;
	std::vector<PreparedCutter> pcs_;
	std::vector<std::vector<int>> topPolys_;	// the walkable tops: convex polygons over P_, counter-clockwise
	std::vector<int> topLabel_;	// per top polygon: index into topLabels_
	std::vector<std::string> topLabels_;
	std::vector<const Cutter*> topCutter_;	// per label: the cutter it came from (its seam)
	std::deque<Cutter> madeTops_;	// tops(): an object's walkable faces as surfgen made them (stable addresses)
	std::unordered_map<uint64_t, std::vector<int>> topPos_;	// vertex hash for the tops (xz at 1e-3, searched 3 x 3, y within 1e-3)

	struct TerrTri
	{
		P3 v[3];
		double A, B, C;
		Line edge[3];
	};
	void terrTris(int i, int j, TerrTri out[2]) const;
	int topVertex(double x, double y, double z);
	void linkTops(std::vector<std::vector<int>>& poly, std::vector<uint8_t>& dead, std::vector<uint8_t>& isTop, std::unordered_map<uint64_t, int>& owner,
		std::vector<Link>& links, std::vector<std::pair<int, int>>& touching, Stats& st);
};

void Gen::build(Stats& st)
{
	const int nx = g_.nx, nz = g_.nz;
	P_.resize((size_t)nx * nz);
	vt_.resize(P_.size());
	fixed_.assign(P_.size(), 0);
	removed_.assign(P_.size(), 0);
	rimFree_.assign(P_.size(), 0);
	rim_.assign(P_.size(), 0);
	stamp_.assign(P_.size(), 0);
	for (int j = 0; j < nz; ++j)
		for (int i = 0; i < nx; ++i)
		{
			P_[(size_t)j * nx + i] = { g_.x0 + i * g_.step, g_.at(i, j), g_.z0 + j * g_.step };
			rim_[(size_t)j * nx + i] = (i == 0 ? 1 : 0) | (i == nx - 1 ? 2 : 0) | (j == 0 ? 4 : 0) | (j == nz - 1 ? 8 : 0);
		}
	for (int j = 0; j + 1 < nz; ++j)
	{
		for (int i = 0; i + 1 < nx; ++i)
		{
			const int a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;	// a (i,j)  b (i+1,j)  c (i,j+1)  d (i+1,j+1)
			++st.cells;
			const std::array<std::array<int, 3>, 2> two = cellSplit(a, b, c, d);
			for (const auto& t : two)
			{
				if (!walkable(P_[t[0]], P_[t[1]], P_[t[2]]))
					continue;
				addTri(t, {});
				++st.walkableTris;
			}
		}
	}

	// The rim: Douglas-Peucker on each side's own samples, so a neighbouring window keeps the same vertices. Kept ones are fixed, the rest
	// may go by the inner rule (a straight rim has no cap area).
	const auto side = [&](int count, const std::function<int(int)>& id)
	{
		std::vector<uint8_t> keep(count, 0);
		keep[0] = keep[count - 1] = 1;
		std::vector<std::pair<int, int>> stack = { { 0, count - 1 } };
		while (!stack.empty())
		{
			const auto [lo, hi] = stack.back();
			stack.pop_back();
			double worst = -1;
			int at = -1;
			for (int k = lo + 1; k < hi; ++k)
			{
				const double t = double(k - lo) / (hi - lo);
				const double e = std::fabs(P_[id(k)][1] - (P_[id(lo)][1] + t * (P_[id(hi)][1] - P_[id(lo)][1])));
				if (e > worst)
				{
					worst = e;
					at = k;
				}
			}
			if (at >= 0 && worst > prm_.heightError)
			{
				keep[at] = 1;
				stack.push_back({ lo, at });
				stack.push_back({ at, hi });
			}
		}
		for (int k = 0; k < count; ++k)
		{
			fixed_[id(k)] = fixed_[id(k)] | keep[k] | (prm_.fixWholeRim ? 1 : 0);
			rimFree_[id(k)] = !fixed_[id(k)];
		}
	};
	side(nx, [&](int k) { return k; });
	side(nx, [&](int k) { return (nz - 1) * nx + k; });
	side(nz, [&](int k) { return k * nx; });
	side(nz, [&](int k) { return k * nx + nx - 1; });
	for (size_t v = 0; v < P_.size(); ++v)
		st.rimKept += rim_[v] && fixed_[v];
}

// Whether the cutter takes the ground out at m (xz) over ground height g: the span rule of cutByCutter on the hits at m (sorted heights, a copy is
// edited).
bool cutterDrops(const PreparedCutter& pc, const P2& m, double g, double lo, double hi, std::vector<double>& hits)
{
	bool drop = false;
	if (pc.src->projected)
	{
		// any surface over it, or inside the walls' outline (an open shell, no caps): even-odd crossings of a ray towards +x
		bool in = false;
		for (const Wall& wall : pc.walls)
			if ((wall.a[1] > m[1]) != (wall.b[1] > m[1]) && m[0] < wall.a[0] + (m[1] - wall.a[1]) * (wall.b[0] - wall.a[0]) / (wall.b[1] - wall.a[1]))
				in = !in;
		drop = in || !hits.empty();
	} else {
		const double y0 = g + lo, y1 = g + hi;
		for (const double h : hits)
			drop = drop || (h > y0 && h < y1);
		if (pc.band)
			return drop;
		if (hits.size() % 2)
			hits.insert(hits.begin(), -1e300);	// a shell open at the bottom (a rock sunk in the ground)
		for (size_t k = 0; k + 1 < hits.size() && !drop; k += 2)
			drop = hits[k] < y1 && hits[k + 1] > y0;
	}
	return drop;
}

// Cuts `pieces` (convex xz polygons lying on the plane y = gA x + gB z + gC) by one cutter: split along its triangles' xz edges and the lines where
// its planes cross the plane + lo / + hi, each piece dropped by its centre (the span rule; `projected`: any hit). `skipSelf` >= 0: hits within it of
// the plane do not count (a top face does not cut itself). Returns the number of pieces dropped.
// A part of a piece that loses nothing comes back whole, not as the fragments the cut lines made of it.
std::size_t cutByCutter(const PreparedCutter& pc, std::vector<std::vector<P2>>& pieces, double gA, double gB, double gC, double lo, double hi,
	double skipSelf)
{
	// One piece under the cuts: a binary tree, each split line making two children of every leaf it crosses. A node all of whose leaves stay comes
	// out whole (kenshi-k16.7.212): the cut lines of a part that loses nothing leave no fragments (zone 52,52's tops were 4.4 M pieces, two thirds
	// of its faces under 1 u2, 31 tiles over the polygon cap).
	struct Node
	{
		std::vector<P2> poly;
		int kid[2] = { -1, -1 };
		bool split = false, kept = false;
	};
	std::size_t dropped = 0;
	std::vector<double> hits;
	std::vector<P2> neg, pos;
	std::vector<std::vector<P2>> next;
	std::vector<Node> nodes;
	std::vector<int> leaves, grown, near;
	for (std::vector<P2>& piece : pieces)
	{
		const Box2 pbox = boxOf(piece);
		if (!pc.box.overlaps(pbox))
		{
			next.push_back(std::move(piece));
			continue;
		}
		nodes.clear();
		nodes.push_back({ std::move(piece) });
		leaves = { 0 };
		const auto splitAll = [&](const Line& L, const Box2& within)
		{
			grown.clear();
			for (const int at : leaves)
			{
				if (!boxOf(nodes[at].poly).overlaps(within) || !splitBy(nodes[at].poly, L, neg, pos))
				{
					grown.push_back(at);
					continue;
				}
				nodes[at].split = true;
				int side = 0;
				for (const std::vector<P2>* half : { &neg, &pos })
				{
					if (std::fabs(area2(*half)) > 1e-8)
					{
						nodes[at].kid[side] = (int)nodes.size();
						grown.push_back((int)nodes.size());
						nodes.push_back({ *half });
					}
					++side;
				}
			}
			leaves.swap(grown);
		};
		for (const Wall& w : pc.walls)
			if (w.box.overlaps(pbox))
				splitAll(w.L, w.box);
		trisNear(pc, pbox, 1e-6, near);
		for (const int k : near)
		{
			const CutTri& t = pc.tris[k];
			if (!t.box.overlaps(pbox))
				continue;
			for (const Line& L : t.edge)
				splitAll(L, t.box);
			if (pc.src->projected)
				continue;
			// where the cutter plane is at ground + lo / + hi
			const double da = t.A - gA, db = t.B - gB, len = std::hypot(da, db);
			if (len < 1e-9)
				continue;
			for (const double h : { lo, hi })
				splitAll(Line{ da / len, db / len, (t.C - gC - h) / len }, t.box);
		}
		for (const int at : leaves)
		{
			const std::vector<P2>& w = nodes[at].poly;
			P2 m = { 0, 0 };
			for (const P2& p : w)
			{
				m[0] += p[0] / w.size();
				m[1] += p[1] / w.size();
			}
			hitsAt(pc, m, hits, near);
			const double g = gA * m[0] + gB * m[1] + gC;
			if (skipSelf >= 0)
				hits.erase(std::remove_if(hits.begin(), hits.end(), [&](double h) { return std::fabs(h - g) <= skipSelf; }), hits.end());
			const bool drop = cutterDrops(pc, m, g, lo, hi, hits);
			nodes[at].kept = !drop;
			dropped += drop;
		}
		// children come after their parent: bottom-up, a split node is kept when both halves are (a half with no area counts as lost)
		for (int at = (int)nodes.size() - 1; at >= 0; --at)
			if (nodes[at].split)
				nodes[at].kept = nodes[at].kid[0] >= 0 && nodes[at].kid[1] >= 0 && nodes[nodes[at].kid[0]].kept && nodes[nodes[at].kid[1]].kept;
		grown = { 0 };
		while (!grown.empty())
		{
			const int at = grown.back();
			grown.pop_back();
			if (nodes[at].kept)
				next.push_back(std::move(nodes[at].poly));
			else
				for (const int kid : { nodes[at].kid[1], nodes[at].kid[0] })
					if (kid >= 0)
						grown.push_back(kid);
		}
	}
	pieces = std::move(next);
	return dropped;
}

void Gen::cut(const std::vector<Cutter>& cutters, Stats& st)
{
	if (cutters.empty())
		return;
	const std::vector<PreparedCutter>& pcs = pcs_;
	st.cutters = pcs.size();
	const double lo = prm_.cutBase, hi = prm_.characterHeight;

	// vertices by xz (one level): a hash of 1e-3 cells, searched 3 x 3 with a 1e-4 tolerance
	const size_t gridCount = P_.size();
	std::unordered_map<uint64_t, std::vector<int>> byPos;
	const auto cellKey = [](int64_t cx, int64_t cz) { return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz; };
	const auto find = [&](double x, double z) -> int
	{
		const int64_t cx = (int64_t)std::floor(x * 1e3), cz = (int64_t)std::floor(z * 1e3);
		for (int64_t dx = -1; dx <= 1; ++dx)
			for (int64_t dz = -1; dz <= 1; ++dz)
			{
				const auto it = byPos.find(cellKey(cx + dx, cz + dz));
				if (it == byPos.end())
					continue;
				for (const int v : it->second)
					if (std::fabs(P_[v][0] - x) < 1e-4 && std::fabs(P_[v][2] - z) < 1e-4)
						return v;
			}
		return -1;
	};
	const auto insert = [&](int v) { byPos[cellKey((int64_t)std::floor(P_[v][0] * 1e3), (int64_t)std::floor(P_[v][2] * 1e3))].push_back(v); };

	std::vector<std::vector<int>> polys;	// the kept pieces as vertex ids, counter-clockwise
	const size_t triCount = T_.size();
	for (size_t ti = 0; ti < triCount; ++ti)
	{
		if (!T_[ti].alive)
			continue;
		const std::array<int, 3> tv = T_[ti].v;
		const P3& a = P_[tv[0]], & b = P_[tv[1]], & c = P_[tv[2]];
		Box2 box;
		double ymin = 1e300, ymax = -1e300;
		for (const P3* p : { &a, &b, &c })
		{
			box.add((*p)[0], (*p)[2]);
			ymin = std::min(ymin, (*p)[1]);
			ymax = std::max(ymax, (*p)[1]);
		}
		std::vector<const PreparedCutter*> near;
		for (const PreparedCutter& pc : pcs)
			if (pc.box.overlaps(box) && (pc.src->projected || (pc.src->min[1] < ymax + hi && pc.src->max[1] > ymin + lo)))
				near.push_back(&pc);
		if (near.empty())
			continue;
		// the ground plane y = gA x + gB z + gC
		const P3 n = normal(a, b, c);
		const double gA = -n[0] / n[1], gB = -n[2] / n[1], gC = a[1] - gA * a[0] - gB * a[2];
		std::vector<std::vector<P2>> pieces = { { { a[0], a[2] }, { b[0], b[2] }, { c[0], c[2] } } };
		bool changed = false;
		for (const PreparedCutter* pc : near)
		{
			const std::size_t dropped = cutByCutter(*pc, pieces, gA, gB, gC, lo, hi, -1);
			st.piecesDropped += dropped;
			changed = changed || dropped > 0;
			if (pieces.empty())
				break;
		}
		changed = changed || pieces.size() != 1;
		if (!changed)
			continue;
		++st.cutGroundTris;
		T_[ti].alive = false;
		for (const int v : tv)
			if (find(P_[v][0], P_[v][2]) < 0)
				insert(v);
		for (const std::vector<P2>& piece : pieces)
		{
			std::vector<int> ids;
			for (const P2& p : piece)
			{
				int v = find(p[0], p[1]);
				if (v < 0)
				{
					v = addVertex({ p[0], gA * p[0] + gB * p[1] + gC, p[1] });
					insert(v);
				}
				if (ids.empty() || (ids.back() != v && ids.front() != v))
					ids.push_back(v);
			}
			if (ids.size() >= 3)
				polys.push_back(std::move(ids));
			++st.piecesKept;
		}
	}
	st.cutVertices = P_.size() - gridCount;

	// T-junctions: a cut point on another polygon's edge (a neighbour's, or an untouched grid triangle's) goes into that edge
	constexpr double cell = 16;
	std::unordered_map<uint64_t, std::vector<int>> byCell;
	for (size_t v = gridCount; v < P_.size(); ++v)
		byCell[cellKey((int64_t)std::floor(P_[v][0] / cell), (int64_t)std::floor(P_[v][2] / cell))].push_back((int)v);
	const auto repair = [&](std::vector<int>& poly)
	{
		bool any = false;
		std::vector<int> out;
		for (size_t k = 0; k < poly.size(); ++k)
		{
			const int u = poly[k], w = poly[(k + 1) % poly.size()];
			out.push_back(u);
			const P3& U = P_[u], & W = P_[w];
			const double dx = W[0] - U[0], dz = W[2] - U[2], len2 = dx * dx + dz * dz;
			if (len2 < 1e-12)
				continue;
			std::vector<std::pair<double, int>> on;
			const int64_t cx0 = (int64_t)std::floor(std::min(U[0], W[0]) / cell), cx1 = (int64_t)std::floor(std::max(U[0], W[0]) / cell);
			const int64_t cz0 = (int64_t)std::floor(std::min(U[2], W[2]) / cell), cz1 = (int64_t)std::floor(std::max(U[2], W[2]) / cell);
			for (int64_t cx = cx0; cx <= cx1; ++cx)
				for (int64_t cz = cz0; cz <= cz1; ++cz)
				{
					const auto it = byCell.find(cellKey(cx, cz));
					if (it == byCell.end())
						continue;
					for (const int q : it->second)
					{
						if (q == u || q == w)
							continue;
						const double t = ((P_[q][0] - U[0]) * dx + (P_[q][2] - U[2]) * dz) / len2;
						if (t <= 1e-9 || t >= 1 - 1e-9)
							continue;
						if (std::fabs(cross2(U, W, P_[q])) / std::sqrt(len2) < 1e-4)
							on.push_back({ t, q });
					}
				}
			std::sort(on.begin(), on.end());
			for (const auto& [t, q] : on)
			{
				(void)t;
				out.push_back(q);
				++st.tjunctions;
				any = true;
			}
		}
		if (any)
			poly = std::move(out);
		return any;
	};
	for (std::vector<int>& poly : polys)
		repair(poly);
	for (size_t ti = 0; ti < triCount; ++ti)
	{
		if (!T_[ti].alive)
			continue;
		std::vector<int> poly = { T_[ti].v[0], T_[ti].v[1], T_[ti].v[2] };
		if (repair(poly))
		{
			T_[ti].alive = false;
			polys.push_back(std::move(poly));
		}
	}
	// a polygon that visits a vertex twice (a needle the merge pinched off) splits there into loops
	std::vector<std::vector<int>> loops;
	for (std::vector<int>& poly : polys)
	{
		std::vector<std::vector<int>> stack = { std::move(poly) };
		while (!stack.empty())
		{
			std::vector<int> p = std::move(stack.back());
			stack.pop_back();
			bool split = false;
			for (size_t i = 0; i < p.size() && !split; ++i)
				for (size_t j = i + 1; j < p.size() && !split; ++j)
					if (p[i] == p[j])
					{
						stack.push_back(std::vector<int>(p.begin() + i, p.begin() + j));
						std::vector<int> rest(p.begin() + j, p.end());
						rest.insert(rest.end(), p.begin(), p.begin() + i);
						stack.push_back(std::move(rest));
						split = true;
					}
			if (!split && p.size() >= 3)
				loops.push_back(std::move(p));
			else if (!split)
				++st.slivers;
		}
	}
	std::vector<std::array<int, 3>> tris;
	for (const std::vector<int>& poly : loops)
	{
		tris.clear();
		double a2 = 0;
		for (size_t k = 0; k < poly.size(); ++k)
			a2 += P_[poly[k]][0] * P_[poly[(k + 1) % poly.size()]][2] - P_[poly[(k + 1) % poly.size()]][0] * P_[poly[k]][2];
		if (std::fabs(a2) < 1e-3)
		{
			++st.slivers;	// collapsed by the 1e-4 vertex merge: its neighbours share its points, no gap
			continue;
		}
		if (!earClip(poly, tris))
		{
			++st.clipFailed;
			continue;
		}
		for (const auto& t : tris)
			addTri(t, {});
	}
}

// Spike trim: an outline corner (a vertex with one boundary edge in, one out) that is convex and sharper than trimAngleDeg is cut across
// where it is trimWidth wide: the line perpendicular to its bisector at d = W / (2 tan(angle / 2)) from the tip (at most trimMaxDepth).
// The triangles reached from the tip on the near side of that line are split by it and their tip side dropped (kept in st.culled). A tip
// whose near side reaches more than 1.5x the ideal tip triangle (the outline turns away before the cut line: not a spike) is left alone.
void Gen::trim(Stats& st)
{
	if (prm_.trimWidth <= 0 || prm_.trimAngleDeg <= 0)
		return;
	const auto key = [](int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; };
	const double maxCos = std::cos(prm_.trimAngleDeg * 3.14159265358979 / 180.0);
	for (int pass = 0; pass < 4; ++pass)
	{
		std::unordered_map<uint64_t, int> edgeTri;
		for (size_t t = 0; t < T_.size(); ++t)
			if (T_[t].alive)
				for (int k = 0; k < 3; ++k)
					edgeTri[key(T_[t].v[k], T_[t].v[(k + 1) % 3])] = (int)t;
		std::unordered_map<int, std::vector<int>> outB, inB;	// boundary edges by vertex
		for (const auto& [k, t] : edgeTri)
		{
			(void)t;
			const int a = (int)(k >> 32), b = (int)(k & 0xffffffffu);
			if (!edgeTri.count(key(b, a)))
			{
				outB[a].push_back(b);
				inB[b].push_back(a);
			}
		}
		struct Tip { double angle; int v, a, b; };
		std::vector<Tip> tips;
		for (const auto& [v, outs] : outB)
		{
			if (outs.size() != 1 || rim_[v])
				continue;
			const auto in = inB.find(v);
			if (in == inB.end() || in->second.size() != 1)
				continue;
			const int a = in->second[0], b = outs[0];
			const P3& V = P_[v], & A = P_[a], & B = P_[b];
			const double ux = A[0] - V[0], uz = A[2] - V[2], wx = B[0] - V[0], wz = B[2] - V[2];
			const double lu = std::hypot(ux, uz), lw = std::hypot(wx, wz);
			if (lu < 1e-9 || lw < 1e-9)
				continue;
			const double c = (ux * wx + uz * wz) / (lu * lw);
			if (c < maxCos || cross2(A, V, B) < -1e-9 * lu * lw)	// wider than the limit, or a reflex corner (a crack into the ground)
				continue;
			tips.push_back({ std::acos(std::clamp(c, -1.0, 1.0)), v, a, b });
		}
		if (tips.empty())
			break;
		std::sort(tips.begin(), tips.end(), [](const Tip& x, const Tip& y) { return x.angle < y.angle; });
		std::vector<uint8_t> touched(T_.size(), 0);
		std::unordered_map<uint64_t, int> cutPoint;
		size_t trimmedThisPass = 0;
		for (const Tip& tip : tips)
		{
			const P3 V = P_[tip.v];
			double ux = P_[tip.a][0] - V[0], uz = P_[tip.a][2] - V[2], wx = P_[tip.b][0] - V[0], wz = P_[tip.b][2] - V[2];
			const double lu = std::hypot(ux, uz), lw = std::hypot(wx, wz);
			double bx = ux / lu + wx / lw, bz = uz / lu + wz / lw;
			const double lb = std::hypot(bx, bz);
			if (lb < 1e-12)
				continue;
			bx /= lb;
			bz /= lb;
			const double half = tip.angle / 2;
			const double d = half > 1e-9 ? std::min(prm_.trimMaxDepth, prm_.trimWidth / (2 * std::tan(half))) : prm_.trimMaxDepth;
			const auto f = [&](int q) { return (P_[q][0] - V[0]) * bx + (P_[q][2] - V[2]) * bz - d; };

			// the triangles on the tip's side of the cut line, reached from the tip
			std::vector<int> set, stack;
			bool ok = true;
			for (const int t : vt_[tip.v])
				if (T_[t].alive)
					stack.push_back(t);
			while (!stack.empty() && ok)
			{
				const int t = stack.back();
				stack.pop_back();
				if (std::find(set.begin(), set.end(), t) != set.end())
					continue;
				if ((size_t)t >= touched.size() || touched[t] || set.size() >= 64)
				{
					ok = false;
					break;
				}
				set.push_back(t);
				for (int k = 0; k < 3; ++k)
				{
					const int a = T_[t].v[k], b = T_[t].v[(k + 1) % 3];
					if (f(a) < 0 || f(b) < 0)
					{
						const auto o = edgeTri.find(key(b, a));
						if (o != edgeTri.end() && T_[o->second].alive)
							stack.push_back(o->second);
					}
				}
			}
			if (!ok || set.empty())
			{
				++st.trimSkipped;
				continue;
			}
			const Line L = { bx, bz, -(V[0] * bx + V[2] * bz) - d };
			double area = 0;
			std::vector<P2> neg, pos;
			for (const int t : set)
			{
				const std::vector<P2> p = { { P_[T_[t].v[0]][0], P_[T_[t].v[0]][2] }, { P_[T_[t].v[1]][0], P_[T_[t].v[1]][2] },
					{ P_[T_[t].v[2]][0], P_[T_[t].v[2]][2] } };
				if (splitBy(p, L, neg, pos))
					area += std::fabs(area2(neg)) * 0.5;
				else if (L.at({ (p[0][0] + p[1][0] + p[2][0]) / 3, (p[0][1] + p[1][1] + p[2][1]) / 3 }) < 0)
					area += std::fabs(area2(p)) * 0.5;
			}
			if (area > 1.5 * 0.5 * d * prm_.trimWidth + 1e-6)
			{
				++st.trimSkipped;
				continue;
			}

			for (const int t : set)
			{
				touched[t] = 1;
				const std::array<int, 3> tv = T_[t].v;
				std::vector<int> keep;
				std::vector<P3> drop;
				for (int k = 0; k < 3; ++k)
				{
					const int i = tv[k], j = tv[(k + 1) % 3];
					const double fi = f(i), fj = f(j);
					if (fi <= 0)
						drop.push_back(P_[i]);
					if (fi >= 0)
						keep.push_back(i);
					if ((fi < 0 && fj > 0) || (fi > 0 && fj < 0))
					{
						const int lo = std::min(i, j), hi = std::max(i, j);
						int nv;
						const auto c = cutPoint.find(key(lo, hi));
						if (c != cutPoint.end())
						{
							nv = c->second;
						} else {
							const double flo = f(lo), fhi = f(hi), s = flo / (flo - fhi);
							const P3 q = { P_[lo][0] + s * (P_[hi][0] - P_[lo][0]), P_[lo][1] + s * (P_[hi][1] - P_[lo][1]), P_[lo][2] + s * (P_[hi][2] - P_[lo][2]) };
							nv = addVertex(q);
							cutPoint[key(lo, hi)] = nv;
						}
						keep.push_back(nv);
						drop.push_back(P_[nv]);
					}
				}
				T_[t].alive = false;
				for (size_t k = 1; k + 1 < keep.size(); ++k)
					if (cross2(P_[keep[0]], P_[keep[k]], P_[keep[k + 1]]) > 1e-12)
						addTri({ keep[0], keep[k], keep[k + 1] }, {});
				for (size_t k = 1; k + 1 < drop.size(); ++k)
					st.culled.push_back({ drop[0], drop[k], drop[k + 1] });
			}
			st.trimmedArea += area;
			++st.trimmedTips;
			++trimmedThisPass;
		}
		if (!trimmedThisPass)
			break;
	}
}

// Ear clipping in xz, counter-clockwise input; picks the ear with the best smallest angle each step. False when it gets stuck.
bool Gen::earClip(const std::vector<int>& poly, std::vector<std::array<int, 3>>& out) const
{
	std::vector<int> p = poly;
	const auto quality = [&](int a, int b, int c)
	{
		const P3& A = P_[a], & B = P_[b], & C = P_[c];
		const double ab = std::hypot(B[0] - A[0], B[2] - A[2]), bc = std::hypot(C[0] - B[0], C[2] - B[2]), ca = std::hypot(A[0] - C[0], A[2] - C[2]);
		const double longest = std::max({ ab, bc, ca });
		return longest > 0 ? cross2(A, B, C) / (longest * longest) : 0.0;
	};
	while (p.size() > 3)
	{
		const size_t n = p.size();
		int best = -1;
		double bestQ = 0;
		for (size_t i = 0; i < n; ++i)
		{
			const int a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
			if (cross2(P_[a], P_[b], P_[c]) <= 1e-9)
				continue;
			bool clear = true;
			for (size_t k = 0; k < n && clear; ++k)
			{
				const int q = p[k];
				if (q == a || q == b || q == c)
					continue;
				clear = !(cross2(P_[a], P_[b], P_[q]) >= -1e-9 && cross2(P_[b], P_[c], P_[q]) >= -1e-9 && cross2(P_[c], P_[a], P_[q]) >= -1e-9);
			}
			if (!clear)
				continue;
			const double q = quality(a, b, c);
			if (best < 0 || q > bestQ)
			{
				best = (int)i;
				bestQ = q;
			}
		}
		if (best < 0)
			return false;
		out.push_back({ p[(best + n - 1) % n], p[best], p[(best + 1) % n] });
		p.erase(p.begin() + best);
	}
	if (p.size() == 3)
	{
		if (cross2(P_[p[0]], P_[p[1]], P_[p[2]]) <= 1e-9)
			return false;
		out.push_back({ p[0], p[1], p[2] });
	}
	return true;
}

bool Gen::evaluate(int v, Plan& plan)
{
	if (fixed_[v] || removed_[v])
		return false;
	std::vector<int>& inc = vt_[v];
	inc.erase(std::remove_if(inc.begin(), inc.end(), [&](int t) { return !T_[t].alive; }), inc.end());
	if (inc.empty())
		return false;

	// the ring: each triangle (v, a, b) counter-clockwise gives the edge a -> b
	std::vector<std::pair<int, int>> edges;
	for (const int t : inc)
	{
		const auto& tv = T_[t].v;
		const int k = tv[0] == v ? 0 : tv[1] == v ? 1 : 2;
		edges.push_back({ tv[(k + 1) % 3], tv[(k + 2) % 3] });
	}
	const auto nextOf = [&](int a)
	{
		int found = -1;
		for (const auto& [x, y] : edges)
			if (x == a)
			{
				if (found >= 0)
					return -2;
				found = y;
			}
		return found;
	};
	int start = -1, starts = 0;
	for (const auto& [a, b] : edges)
	{
		(void)b;
		bool hasPrev = false;
		for (const auto& [x, y] : edges)
			hasPrev = hasPrev || y == a;
		if (!hasPrev)
		{
			start = a;
			++starts;
		}
	}
	if (starts > 1)
		return false;
	plan.ring.clear();
	plan.tris.clear();
	plan.pa = plan.pb = -1;
	const bool boundary = starts == 1;
	int cur = boundary ? start : edges[0].first;
	for (size_t guard = 0; guard <= edges.size() + 1; ++guard)
	{
		plan.ring.push_back(cur);
		const int nx = nextOf(cur);
		if (nx == -2)
			return false;
		if (nx < 0 || (!boundary && nx == plan.ring.front()))
			break;
		cur = nx;
	}
	if (plan.ring.size() != (boundary ? edges.size() + 1 : edges.size()))
		return false;

	const P3& V = P_[v];
	double capArea = 0;
	plan.border = boundary;
	plan.cut = 0;
	if (boundary)
	{
		const int p = plan.ring.front(), n = plan.ring.back();
		const P3& A = P_[p], & B = P_[n];
		const double turn = cross2(V, A, B);	// > 0: the walkable wedge at v is under 180 deg, a convex corner
		const double area = std::fabs(turn) * 0.5;
		capArea = area;
		const double dx = B[0] - A[0], dz = B[2] - A[2], len2 = dx * dx + dz * dz;
		if (len2 < 1e-6)
			return false;
		double t = ((V[0] - A[0]) * dx + (V[2] - A[2]) * dz) / len2;
		t = std::clamp(t, 0.0, 1.0);
		const double chord = std::fabs(V[1] - (A[1] + t * (B[1] - A[1])));
		if (rimFree_[v])
		{
			if (area > 1e-6 || chord > prm_.heightError)
				return false;
		} else if (prm_.materialRule && bld_[v]) {
			// a building-owned loop: only a corner on its chord goes (the original keeps every corner of a building shape's loop)
			if (std::sqrt(len2) < 1e-3 || std::fabs(turn) / std::sqrt(len2) > prm_.buildingTol || chord > prm_.buildingTol)
				return false;
		} else {
			const bool nearRim = prm_.materialRule && nearRim_[v];
			const double limArea = nearRim ? prm_.rimBorderArea : prm_.borderArea, limHeight = nearRim ? prm_.rimBorderHeight : prm_.borderHeight;
			bool bounded = false;	// straighten: the chord stands for the outline points it drops within slopXZ / slopY, and joins no pinch
			if (prm_.straighten)
			{
				plan.pa = p;
				plan.pb = n;
				plan.owned = { V };
				for (const auto& key : { edgeKey(p, v), edgeKey(v, n) })
				{
					const auto it = dropped_.find(key);
					if (it != dropped_.end())
						plan.owned.insert(plan.owned.end(), it->second.begin(), it->second.end());
				}
				if (pinched(plan, v))
					return false;
				// only at a convex corner: the chord then cuts ground away. A reflex corner lays ground over the cutter the outline hugs (a thin
				// cutter bridged whole, knav_ground zone 46.16), so it stays on the production area rule
				bounded = turn > 0;
				for (const P3& q : plan.owned)
				{
					const double u = std::clamp(((q[0] - A[0]) * dx + (q[2] - A[2]) * dz) / len2, 0.0, 1.0);
					bounded = bounded && std::hypot(q[0] - (A[0] + u * dx), q[2] - (A[2] + u * dz)) <= prm_.slopXZ
						&& std::fabs(q[1] - (A[1] + u * (B[1] - A[1]))) <= prm_.slopY;
				}
			}
			// a removal that drifts past the straighten bound may still pass the production rule (small area, small chord)
			if (!bounded && (area > (turn > 0 ? limArea : prm_.concaveArea) || chord > (turn > 0 ? limHeight : prm_.concaveHeight)))
				return false;
			// a long thin cut is under the area rule yet takes a corner of a passage off (the Hub bar's stoop: two 3 u cuts left 8 u of 14)
			if (turn > 0 && std::fabs(turn) > prm_.borderDepth * std::sqrt(len2))
			{
				++refusedDepth_;
				return false;
			}
			if (turn > 0 && !loopGuard(v, p, n, loopCut(A, V, B)))
				return false;
		}
		// the closing edge n -> p must not cross the ring
		for (size_t k = 1; k + 2 < plan.ring.size(); ++k)
			if (properCross(B, A, P_[plan.ring[k]], P_[plan.ring[k + 1]]))
				return false;
		if (plan.ring.size() >= 3 && !earClip(plan.ring, plan.tris))
			return false;
	} else if (!earClip(plan.ring, plan.tris)) {
		return false;
	}
	for (const auto& t : plan.tris)
		if (!walkable(P_[t[0]], P_[t[1]], P_[t[2]]))
			return false;

	// every sample the old triangles covered (and v) against the new surface
	double worst = 0;
	const auto check = [&](int q)
	{
		const P3& Q = P_[q];
		for (const auto& t : plan.tris)
		{
			double y;
			if (heightAt(P_[t[0]], P_[t[1]], P_[t[2]], Q, y))
			{
				worst = std::max(worst, std::fabs(Q[1] - y));
				return true;
			}
		}
		return boundary;	// a boundary removal may cut a sample off the walkable area
	};
	if (!check(v))
		return false;
	for (const int t : inc)
		for (const int q : T_[t].pts)
			if (!check(q) || worst > prm_.heightError)
				return false;
	if (worst > prm_.heightError)
		return false;
	plan.cost = worst;
	if (boundary)
	{
		const P3& A = P_[plan.ring.front()], & B = P_[plan.ring.back()];
		plan.cut = loopCut(A, V, B);
	}
	plan.key = boundary && prm_.areaOrder ? 1e6 + capArea : worst;
	return true;
}

void Gen::apply(int v, const Plan& plan)
{
	if (plan.border && v < (int)loopOf_.size() && loopOf_[v] >= 0)
	{
		loopArea_[loopOf_[v]] -= plan.cut;
		addLoopSeg(plan.ring.front(), plan.ring.back(), loopOf_[v]);
	}
	if (plan.pa >= 0)
	{
		dropped_.erase(edgeKey(plan.pa, v));
		dropped_.erase(edgeKey(v, plan.pb));
		dropped_[edgeKey(plan.pa, plan.pb)] = plan.owned;
	}
	std::vector<int> pts = { v };
	for (const int t : vt_[v])
	{
		if (!T_[t].alive)
			continue;
		T_[t].alive = false;
		pts.insert(pts.end(), T_[t].pts.begin(), T_[t].pts.end());
		T_[t].pts.clear();
		T_[t].pts.shrink_to_fit();
	}
	vt_[v].clear();
	std::vector<std::vector<int>> share(plan.tris.size());
	for (const int q : pts)
	{
		for (size_t k = 0; k < plan.tris.size(); ++k)
		{
			double y;
			const auto& t = plan.tris[k];
			if (heightAt(P_[t[0]], P_[t[1]], P_[t[2]], P_[q], y))
			{
				share[k].push_back(q);
				break;
			}
		}
	}
	for (size_t k = 0; k < plan.tris.size(); ++k)
		addTri(plan.tris[k], std::move(share[k]));
	removed_[v] = 1;
}

// The outline vertices as they stand before any removal, and the edge-connected piece (component) each belongs to: a hole's outline and the
// outer outline are one piece, an island is another. A vertex where two pieces touch is a pinch vertex.
void Gen::indexOutline()
{
	outline_.clear();
	pinchV_.clear();
	vcomp_.clear();
	std::map<std::pair<int, int>, int> dir;	// directed edge -> triangle
	for (int t = 0; t < (int)T_.size(); ++t)
		if (T_[t].alive)
			for (int k = 0; k < 3; ++k)
				dir[{ T_[t].v[k], T_[t].v[(k + 1) % 3] }] = t;
	std::vector<int> uf(T_.size());
	for (int t = 0; t < (int)uf.size(); ++t)
		uf[t] = t;
	const std::function<int(int)> root = [&](int t) { return uf[t] == t ? t : uf[t] = root(uf[t]); };
	for (const auto& [e, t] : dir)
	{
		const auto o = dir.find({ e.second, e.first });
		if (o != dir.end())
			uf[root(t)] = root(o->second);
	}
	std::unordered_map<int, int> outs;
	for (const auto& [e, t] : dir)
	{
		const int c = root(t);
		for (const int x : { e.first, e.second })
		{
			auto& cs = vcomp_[x];
			if (std::find(cs.begin(), cs.end(), c) == cs.end())
				cs.push_back(c);
		}
		if (!dir.count({ e.second, e.first }))
		{
			if (++outs[e.first] == 1)
				outline_[cellKey((int)std::floor(P_[e.first][0] / 8), (int)std::floor(P_[e.first][2] / 8))].push_back(e.first);
		}
	}
	for (const auto& [x, cs] : vcomp_)
		if (cs.size() > 1)
			pinchV_.insert(x);
}

// The chord A-B the removal of v makes must not join another piece of ground: no pinch vertex at its ends, no outline vertex of another piece within
// slopXZ of it (the prune drops islands; a chord across the pinch would hand them to the rim component).
bool Gen::pinched(const Plan& plan, int v) const
{
	if (pinchV_.count(plan.pa) || pinchV_.count(plan.pb))
		return true;
	const int mine = vcomp_.at(v).front();
	const P3& A = P_[plan.pa], & B = P_[plan.pb];
	const double r = prm_.slopXZ, dx = B[0] - A[0], dz = B[2] - A[2], len2 = dx * dx + dz * dz;
	const int c0x = (int)std::floor((std::min(A[0], B[0]) - r) / 8), c1x = (int)std::floor((std::max(A[0], B[0]) + r) / 8);
	const int c0z = (int)std::floor((std::min(A[2], B[2]) - r) / 8), c1z = (int)std::floor((std::max(A[2], B[2]) + r) / 8);
	for (int cx = c0x; cx <= c1x; ++cx)
		for (int cz = c0z; cz <= c1z; ++cz)
		{
			const auto it = outline_.find(cellKey(cx, cz));
			if (it == outline_.end())
				continue;
			for (const int x : it->second)
			{
				if (vcomp_.at(x).front() == mine)
					continue;
				const P3& q = P_[x];
				const double u = std::clamp(((q[0] - A[0]) * dx + (q[2] - A[2]) * dz) / len2, 0.0, 1.0);
				if (std::hypot(q[0] - (A[0] + u * dx), q[2] - (A[2] + u * dz)) < r)
					return true;
			}
		}
	return false;
}

// The border loops before the pass: each loop's signed xz area and its edges in an 8 u grid (the guards below). A vertex with two outgoing
// border edges (a pinch) puts its loop out of the guard: -1.
void Gen::indexLoops()
{
	loopOf_.assign(P_.size(), -1);
	loopArea_.clear();
	loopSegs_.clear();
	loopCells_.clear();
	std::set<std::pair<int, int>> dir;
	for (const Tri& t : T_)
		if (t.alive)
			for (int k = 0; k < 3; ++k)
				dir.insert({ t.v[k], t.v[(k + 1) % 3] });
	std::unordered_map<int, int> next;
	std::unordered_set<int> pinch;
	for (const auto& [a, b] : dir)
		if (!dir.count({ b, a }))
			if (!next.emplace(a, b).second)
				pinch.insert(a);
	std::unordered_set<int> seen;
	for (const auto& [start, first] : next)
	{
		if (seen.count(start))
			continue;
		std::vector<int> loop;
		bool ok = true;
		for (int u = start; !seen.count(u); )
		{
			seen.insert(u);
			loop.push_back(u);
			ok = ok && !pinch.count(u);
			const auto it = next.find(u);
			if (it == next.end())
			{
				ok = false;
				break;
			}
			u = it->second;
			if (u == start)
				break;
		}
		if (!ok || next.at(loop.back()) != start)
			continue;
		const int id = (int)loopArea_.size();
		double area = 0;
		for (size_t i = 0; i < loop.size(); ++i)
		{
			const P3& p = P_[loop[i]], & q = P_[loop[(i + 1) % loop.size()]];
			area += p[0] * q[2] - q[0] * p[2];
			loopOf_[loop[i]] = id;
			addLoopSeg(loop[i], loop[(i + 1) % loop.size()], id);
		}
		loopArea_.push_back(area * 0.5);
	}
}

void Gen::addLoopSeg(int s, int e, int loop)
{
	loopSegs_.push_back({ s, e, loop });
	const P3& S = P_[s], & E = P_[e];
	const int c0x = (int)std::floor(std::min(S[0], E[0]) / 8), c1x = (int)std::floor(std::max(S[0], E[0]) / 8);
	const int c0z = (int)std::floor(std::min(S[2], E[2]) / 8), c1z = (int)std::floor(std::max(S[2], E[2]) / 8);
	for (int cx = c0x; cx <= c1x; ++cx)
		for (int cz = c0z; cz <= c1z; ++cz)
			loopCells_[cellKey(cx, cz)].push_back((int)loopSegs_.size() - 1);
}

// The original's two border-loop guards on a convex removal of v (chord a-b; `cut` is the signed loop area it takes away), as the helper
// measured them (rwh291; FUN_140de3df0 / FUN_140de1e10): the loop through v may not shrink to 0.75 of its area or flip its sign, and a corridor
// between the loop's own stretches (min 4 / max 6 u) that the chord would narrow from within the maximum to under the minimum keeps its corner.
// UNSURE: the corridor test's exact form, and its "both sides the same region" condition (every ground loop here is one region, so it is not
// tested); only loop-internal corridors are seen. The loop area is carried, the corridor test looks at the loop's edges within reach of the chord.
bool Gen::loopGuard(int v, int a, int b, double cut) const
{
	if (v >= (int)loopOf_.size() || loopOf_[v] < 0)
		return true;
	const int loop = loopOf_[v];
	const double area = loopArea_[loop], after = area - cut;
	if (after * area <= 0 || std::fabs(after) <= prm_.loopShrink * std::fabs(area))
	{
		const_cast<Gen*>(this)->refusedLoop_++;
		return false;
	}
	const P3& A = P_[a], & B = P_[b], & V = P_[v];
	const double r = prm_.corridorMax;
	const int c0x = (int)std::floor((std::min(A[0], B[0]) - r) / 8), c1x = (int)std::floor((std::max(A[0], B[0]) + r) / 8);
	const int c0z = (int)std::floor((std::min(A[2], B[2]) - r) / 8), c1z = (int)std::floor((std::max(A[2], B[2]) + r) / 8);
	std::vector<int> seen;
	for (int cx = c0x; cx <= c1x; ++cx)
		for (int cz = c0z; cz <= c1z; ++cz)
		{
			const auto it = loopCells_.find(cellKey(cx, cz));
			if (it == loopCells_.end())
				continue;
			for (const int k : it->second)
			{
				const LoopSeg& g = loopSegs_[k];
				if (g.loop != loop || removed_[g.s] || removed_[g.e] || g.s == v || g.e == v || g.s == a || g.e == b
					|| std::find(seen.begin(), seen.end(), k) != seen.end())
					continue;
				seen.push_back(k);
				const P3& S = P_[g.s], & E = P_[g.e];
				const double before = std::min({ segDist(S, E, V), segDist(S, E, A), segDist(S, E, B) });
				const double now = std::min({ segDist(A, B, S), segDist(A, B, E), segDist(S, E, A), segDist(S, E, B) });
				if (before <= prm_.corridorMax && now < prm_.corridorMin && now < before)
				{
					const_cast<Gen*>(this)->refusedCorridor_++;
					return false;
				}
			}
		}
	return true;
}

bool Gen::buildingAt(const P2& q, double y) const
{
	static thread_local std::vector<double> hits;
	static thread_local std::vector<int> near;
	for (const PreparedCutter& pc : pcs_)
	{
		if (pc.src->prebuilt || !pc.box.contains(q) || materialOfLabel(pc.src->label) != Material::Building)
			continue;
		hitsAt(pc, q, hits, near);
		if (cutterDrops(pc, q, y, prm_.cutBase, prm_.characterHeight, hits))
			return true;
	}
	return false;
}

// materialRule: which rule each border vertex takes. A loop is building-owned when a building cutter takes the ground out just beside any of its
// edges (an edge on the window rim has no owner: the window ends there); the whole loop then keeps its corners. A loop (open at the rim or not) with a
// vertex within rimBand of the window rim takes the rim rule. A vertex on no loop (a pinch) goes by its own edges.
void Gen::classifyBorder(Stats& st)
{
	bld_.assign(P_.size(), 0);
	nearRim_.assign(P_.size(), 0);
	const double x1 = g_.x0 + (g_.nx - 1) * g_.step, z1 = g_.z0 + (g_.nz - 1) * g_.step;
	std::set<std::pair<int, int>> dir;
	for (const Tri& t : T_)
		if (t.alive)
			for (int k = 0; k < 3; ++k)
				dir.insert({ t.v[k], t.v[(k + 1) % 3] });
	std::map<int, std::vector<int>> byLoop;	// loop id -> its vertices
	std::vector<char> loopBld(loopArea_.size(), 0), loopRim(loopArea_.size(), 0), loopRimEdge(loopArea_.size(), 0);
	const auto near = [&](int v)
	{
		const P3& p = P_[v];
		return std::min({ p[0] - g_.x0, x1 - p[0], p[2] - g_.z0, z1 - p[2] }) < prm_.rimBand;
	};
	for (const Tri& t : T_)
	{
		if (!t.alive)
			continue;
		for (int k = 0; k < 3; ++k)
		{
			const int a = t.v[k], b = t.v[(k + 1) % 3];
			if (dir.count({ b, a }))
				continue;
			const P3& A = P_[a], & B = P_[b], & W = P_[t.v[(k + 2) % 3]];
			const double dx = B[0] - A[0], dz = B[2] - A[2], len = std::hypot(dx, dz);
			if (len < 1e-9)
				continue;
			const bool onRim = (rim_[a] & rim_[b]) != 0;
			bool bld = false;
			if (!onRim)
			{
				const double side = cross2(A, B, W) > 0 ? -1 : 1;	// away from the triangle
				const P2 q = { (A[0] + B[0]) / 2 + side * 0.05 * -dz / len, (A[2] + B[2]) / 2 + side * 0.05 * dx / len };
				bld = buildingAt(q, (A[1] + B[1]) / 2);
			}
			const int lp = loopOf_[a];
			for (const int v : { a, b })
			{
				bld_[v] = bld_[v] | bld;
				nearRim_[v] = nearRim_[v] | (uint8_t)near(v);
			}
			if (lp >= 0)
			{
				loopBld[lp] = loopBld[lp] | bld;
				loopRimEdge[lp] = loopRimEdge[lp] | onRim;
			}
		}
	}
	for (int v = 0; v < (int)P_.size(); ++v)
		if (loopOf_[v] >= 0)
			byLoop[loopOf_[v]].push_back(v);
	for (const auto& [lp, vs] : byLoop)
	{
		bool b = loopBld[lp], r = false;
		for (const int v : vs)
			r = r || nearRim_[v];
		if (loopRimEdge[lp])
			b = false;	// the window's outer loop: its building parts are flagged edge by edge
		for (const int v : vs)
		{
			if (!loopRimEdge[lp])
				bld_[v] = bld_[v] | b;
			nearRim_[v] = nearRim_[v] | r;
		}
		st.buildingLoops += b;
		st.rimLoops += !b && r;
		st.terrainLoops += !b && !r;
	}
}

void Gen::simplify(Stats& st)
{
	if (prm_.straighten)
		indexOutline();
	indexLoops();
	if (prm_.materialRule)
		classifyBorder(st);
	using Item = std::tuple<double, int, uint32_t>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
	Plan plan;
	for (int v = 0; v < (int)P_.size(); ++v)
		if (evaluate(v, plan))
			pq.push({ plan.key, v, stamp_[v] });
	while (!pq.empty())
	{
		const auto [cost, v, stamp] = pq.top();
		pq.pop();
		if (stamp != stamp_[v] || removed_[v])
			continue;
		if (!evaluate(v, plan))
			continue;
		if (plan.key > cost + 1e-9 && !pq.empty() && plan.key > std::get<0>(pq.top()))
		{
			pq.push({ plan.key, v, ++stamp_[v] });
			continue;
		}
		st.maxError = std::max(st.maxError, plan.cost);
		if (plan.border && prm_.materialRule)
		{
			st.removedBuilding += bld_[v];
			st.removedRim += !bld_[v] && nearRim_[v];
			st.removedTerrain += !bld_[v] && !nearRim_[v];
		}
		apply(v, plan);
		++st.removed;
		const std::vector<int> ring = plan.ring;
		for (const int r : ring)
		{
			++stamp_[r];
			if (evaluate(r, plan))
				pq.push({ plan.key, r, stamp_[r] });
		}
	}
	st.refusedLoop = refusedLoop_;
	st.refusedCorridor = refusedCorridor_;
	st.refusedDepth = refusedDepth_;
}

// ---- walkable tops (NAVMESH_GENERATOR.md section 12 steps 1-5) ----

// The two terrain triangles of grid cell (i, j), split as build() does (cellSplit).
void Gen::terrTris(int i, int j, TerrTri out[2]) const
{
	const int nx = g_.nx;
	const int a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;
	const auto pt = [&](int id) { return P3{ g_.x0 + (id % nx) * g_.step, g_.y[id], g_.z0 + (id / nx) * g_.step }; };
	const P3 pa = pt(a), pb = pt(b), pc = pt(c), pd = pt(d);
	const std::array<std::array<P3, 3>, 2> tri = cellSplit(pa, pb, pc, pd);
	for (int t = 0; t < 2; ++t)
	{
		TerrTri& o = out[t];
		for (int k = 0; k < 3; ++k)
			o.v[k] = tri[t][k];
		const P3 n = normal(o.v[0], o.v[1], o.v[2]);
		o.A = -n[0] / n[1];
		o.B = -n[2] / n[1];
		o.C = o.v[0][1] - o.A * o.v[0][0] - o.B * o.v[0][2];
		for (int k = 0; k < 3; ++k)
		{
			const P3& p = o.v[k], & q = o.v[(k + 1) % 3];
			const double dx = q[0] - p[0], dz = q[2] - p[2], len = std::hypot(dx, dz);
			o.edge[k] = Line{ -dz / len, dx / len, (dz * p[0] - dx * p[2]) / len };
		}
	}
}

int Gen::topVertex(double x, double y, double z)
{
	const int64_t cx = (int64_t)std::floor(x * 1e3), cz = (int64_t)std::floor(z * 1e3);
	for (int64_t dx = -1; dx <= 1; ++dx)
		for (int64_t dz = -1; dz <= 1; ++dz)
		{
			const auto it = topPos_.find(cellKeyOf(cx + dx, cz + dz));
			if (it == topPos_.end())
				continue;
			for (const int v : it->second)
				if (std::fabs(P_[v][0] - x) < 1e-4 && std::fabs(P_[v][2] - z) < 1e-4 && std::fabs(P_[v][1] - y) < 1e-3)
					return v;
		}
	const int v = addVertex({ x, y, z });
	topPos_[cellKeyOf(cx, cz)].push_back(v);
	return v;
}

void Gen::tops(const std::vector<Cutter>& cutters, const Params& prm, Stats& st)
{
	const auto t0 = std::chrono::steady_clock::now();
	const double x1 = g_.x0 + (g_.nx - 1) * g_.step, z1 = g_.z0 + (g_.nz - 1) * g_.step;
	const auto minNyOf = [](double deg) { return std::cos(deg * 3.14159265358979 / 180.0) - 1e-9; };
	const std::array<Line, 4> window = { Line{ 1, 0, -g_.x0 }, Line{ -1, 0, x1 }, Line{ 0, 1, -g_.z0 }, Line{ 0, -1, z1 } };
	const auto clipKeep = [](std::vector<P2>& p, const Line& L)
	{
		std::vector<P2> neg, pos;
		if (splitBy(p, L, neg, pos))
		{
			p = std::move(pos);
		} else {
			for (const P2& q : p)
				if (L.at(q) < -1e-4)
				{
					p.clear();
					break;
				}
		}
		return p.size() >= 3 && std::fabs(area2(p)) > 2e-3;
	};
	const double terrainMin = g_.y.empty() ? 0.0 : *std::min_element(g_.y.begin(), g_.y.end());
	for (size_t ci = 0; ci < cutters.size(); ++ci)
	{
		const Cutter& c = cutters[ci];
		if (c.top == Cutter::Top::None || (c.max[0] < g_.x0 - 200 || c.min[0] > x1 + 200 || c.max[2] < g_.z0 - 200 || c.min[2] > z1 + 200))
			continue;
		if (c.max[1] + prm.topTerrainTol < terrainMin)	// wholly under the lowest terrain of the window: keeps nothing
		{
			st.underTerrain.push_back(c.label);
			continue;
		}
		++st.topShapes;
		int label = -1;
		// a map feature is a plain Havok input: its top is walkable below 40 deg (kenshi-rwh.338), a building's by surfgen's limit
		const double slopeDeg = materialOfLabel(c.label) == Material::Feature ? prm.featureSlopeDeg : prm.topSlopeDeg;
		const double minNy = minNyOf(slopeDeg);
		// the up-facing faces of the shape (normal() is w x u, y > 0 for the xz-counter-clockwise order cross2 takes; a mesh's outward normal is
		// u x w, so up-facing is cross2 < 0), 200 u round the window so a deck's far end is known
		struct Up
		{
			size_t k;
			double fA, fB, fC, area;
			bool seed = false, ok = true, steep = false;
			double nyz = 1;
			double clearMax = 0;
		};
		std::vector<Up> ups;
		for (size_t k = 0; k + 2 < c.tris.size(); k += 3)
		{
			const P3& a = c.tris[k], & b = c.tris[k + 1], & d = c.tris[k + 2];
			const P3 un = normal(a, b, d);
			const P3 n = { -un[0], -un[1], -un[2] };
			const double len = length(n);
			if (len < 1e-9 || n[1] <= 0 || n[1] / len < minNy)
				continue;
			const double area = -cross2(a, b, d) * 0.5, longest = std::max({ std::hypot(b[0] - a[0], b[2] - a[2]), std::hypot(d[0] - b[0], d[2] - b[2]), std::hypot(a[0] - d[0], a[2] - d[2]) });
			if (area <= 0 || area * 2 / longest < 0.05)
				continue;
			if (std::max({ a[0], b[0], d[0] }) < g_.x0 - 200 || std::min({ a[0], b[0], d[0] }) > x1 + 200 || std::max({ a[2], b[2], d[2] }) < g_.z0 - 200
				|| std::min({ a[2], b[2], d[2] }) > z1 + 200)
				continue;
			Up u;
			u.k = k;
			u.fA = -n[0] / n[1];
			u.fB = -n[2] / n[1];
			u.fC = a[1] - u.fA * a[0] - u.fB * a[2];
			u.area = area;
			u.nyz = n[1] / len;
			ups.push_back(u);
		}
		// Step 1 by surfgen (kenshi-ffmi.7; section 12: "an object uses the same machinery as a building with no mask"): the kept up-facing
		// triangles are the walkable shape, the whole object an obstacle shape for its own cuts (railings, posts), no mask, no pruning (the prune
		// is step 4's). Its faces (merged, welded, the narrow cut) replace the raw triangles; steps 2-4 run on them as before. Without this every
		// source triangle stayed its own face, split again by its own span cut and the terrain cells: a Metal Arch Bridge deck came out 922 faces,
		// median 20 u2, against surfgen's 280 on the same collision. A prebuilt top (a zone side) is surfgen's already.
		const Cutter* src = &c;
		if (!c.prebuilt)
		{
			kworld::WorldShape walk, whole;
			walk.use = kworld::NavUse::Walkable;
			whole.use = kworld::NavUse::Obstacle;
			const auto addTri = [](kworld::WorldShape& s, const P3& a, const P3& b, const P3& d)
			{
				for (const P3* q : { &a, &b, &d })
				{
					s.indices.push_back((std::uint32_t)(s.vertices.size() / 3));
					for (int i = 0; i < 3; ++i)
						s.vertices.push_back((float)(*q)[i]);
				}
			};
			// A building's own other shapes cut its top as the original does (kenshi-k16.7.251, oracle rwh.337): on each INPUT triangle, whole:
			// 4 samples (the centroid and the 3 corners pulled in to 0.51) that must all fall under the solid; the triangle is never clipped, so a
			// thin plank across a long top leaves no hole. Before surfgen merges and cuts the top into other pieces.
			std::vector<double> hits;
			std::vector<int> near;
			for (const Up& u : ups)
			{
				if (!u.ok)
					continue;
				const P3& a = c.tris[u.k], & b = c.tris[u.k + 1], & d = c.tris[u.k + 2];
				const P3 n = normal(a, b, d);
				const double fA = -n[0] / n[1], fB = -n[2] / n[1], fC = a[1] - fA * a[0] - fB * a[2];
				const P2 cen = { (a[0] + b[0] + d[0]) / 3, (a[2] + b[2] + d[2]) / 3 };
				const P2 samples[4] = { cen, { cen[0] + 0.51 * (a[0] - cen[0]), cen[1] + 0.51 * (a[2] - cen[1]) },
					{ cen[0] + 0.51 * (b[0] - cen[0]), cen[1] + 0.51 * (b[2] - cen[1]) }, { cen[0] + 0.51 * (d[0] - cen[0]), cen[1] + 0.51 * (d[2] - cen[1]) } };
				// each sample's ray starts 0.01 u below the face and reaches 18 u up; it hits any triangle of the building's other shapes
				// (no span or inside-solid rule), and all 4 must hit (they may hit different shapes)
				bool cut = true;
				for (const P2& m : samples)
				{
					const double g = fA * m[0] + fB * m[1] + fC;
					bool hit = false;
					for (const PreparedCutter& pc : pcs_)
					{
						if (hit || pc.src == &c || pc.src->label != c.label || !pc.box.contains(m))
							continue;
						hitsAt(pc, m, hits, near);
						for (const double h : hits)
							hit = hit || (h >= g - 0.01 && h <= g - 0.01 + 18.0);
					}
					cut = cut && hit;
					if (!cut)
						break;
				}
				if (!cut)
					addTri(walk, a, b, d);
			}
			for (size_t k = 0; k + 2 < c.tris.size(); k += 3)
				addTri(whole, c.tris[k], c.tris[k + 1], c.tris[k + 2]);
			for (kworld::WorldShape* s : { &walk, &whole })
			{
				s->min = { 1e30f, 1e30f, 1e30f };
				s->max = { -1e30f, -1e30f, -1e30f };
				for (size_t i = 0; i < s->vertices.size(); ++i)
				{
					s->min[i % 3] = std::min(s->min[i % 3], s->vertices[i]);
					s->max[i % 3] = std::max(s->max[i % 3], s->vertices[i]);
				}
			}
			if (walk.indices.empty())
				continue;
			surfgen::Input in;
			in.shapes = { &walk, &whole };
			in.paintDoors = false;
			surfgen::Settings sg;
			sg.slopeDegrees = slopeDeg;
			sg.characterHeight = prm.characterHeight;
			sg.cutBase = prm.cutBase;
			sg.keepIslands = true;
			// no narrow cut: surfgen normalises the tops here, it does not decide what is walkable. The tops had no width rule before (a fallen
			// tree's trunk, TreeFall01 in knav_ground_objects, is under minPassage wide and lost its top with it); one is ksim's call.
			sg.minPassage = 0;
			const native::Mesh made = surfgen::generate(in, sg);
			madeTops_.push_back(zoneSideCutter(made, c.label));
			madeTops_.back().prebuilt = false;
			madeTops_.back().seam.clear();
			src = &madeTops_.back();
			++st.topSurfgen;
			ups.clear();
			for (size_t k = 0; k + 2 < src->tris.size(); k += 3)
			{
				const P3& a = src->tris[k], & b = src->tris[k + 1], & d = src->tris[k + 2];
				const P3 un = normal(a, b, d);
				const P3 n = { -un[0], -un[1], -un[2] };
				if (length(n) < 1e-9 || n[1] <= 0)
					continue;
				Up u;
				u.k = k;
				u.fA = -n[0] / n[1];
				u.fB = -n[2] / n[1];
				u.fC = a[1] - u.fA * a[0] - u.fB * a[2];
				u.area = -cross2(a, b, d) * 0.5;
				ups.push_back(u);
			}
		}
		for (const Up& u : ups)
		{
			if (!u.ok)
				continue;
			const P3& a = src->tris[u.k], & b = src->tris[u.k + 1], & d = src->tris[u.k + 2];
			const double fA = u.fA, fB = u.fB, fC = u.fC, area = u.area;
			(void)area;
			std::vector<P2> poly = { { a[0], a[2] }, { d[0], d[2] }, { b[0], b[2] } };
			bool alive = true;
			for (const Line& L : window)
				alive = alive && clipKeep(poly, L);
			if (!alive)
				continue;
			++st.topTris;
			// step 2: the terrain cut. Over each terrain triangle the face is a plane over a plane: keep where it is more than topTerrainTol above.
			const Box2 pb = boxOf(poly);
			const int i0 = std::clamp((int)std::floor((pb.x0 - g_.x0) / g_.step), 0, g_.nx - 2), i1 = std::clamp((int)std::floor((pb.x1 - g_.x0) / g_.step), 0, g_.nx - 2);
			const int j0 = std::clamp((int)std::floor((pb.z0 - g_.z0) / g_.step), 0, g_.nz - 2), j1 = std::clamp((int)std::floor((pb.z1 - g_.z0) / g_.step), 0, g_.nz - 2);
			std::vector<std::vector<P2>> pieces;
			// well above every terrain sample under its box (the 4 cell corners of each cell it touches): the face stays whole
			double terrMax = -1e300, faceMin = 1e300;
			for (int j = j0; j <= j1 + 1; ++j)
				for (int i = i0; i <= i1 + 1; ++i)
					terrMax = std::max(terrMax, g_.at(std::min(i, g_.nx - 1), std::min(j, g_.nz - 1)));
			for (const P2& p : poly)
				faceMin = std::min(faceMin, fA * p[0] + fB * p[1] + fC);
			if (faceMin > terrMax + prm.topTerrainTol)
				pieces.push_back(poly);
			else
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
				{
					TerrTri tt[2];
					terrTris(i, j, tt);
					for (const TerrTri& t : tt)
					{
						std::vector<P2> piece = poly;
						bool ok = true;
						for (int e = 0; e < 3 && ok; ++e)
							ok = clipKeep(piece, t.edge[e]);
						if (!ok)
							continue;
						const double da = fA - t.A, db = fB - t.B, dc = fC - t.C - prm.topTerrainTol, len = std::hypot(da, db);
						if (len < 1e-9)
							ok = dc >= 0;
						else
							ok = clipKeep(piece, Line{ da / len, db / len, dc / len });
						if (ok)
							pieces.push_back(std::move(piece));
					}
				}
			if (pieces.empty())
				continue;
			// F4: the cut rule against every other solid above the face (surfgen cut the object by its own shape)
			const Box2 fb = boxOf(poly);
			// A prebuilt top (a building's zone side) is cut by every solid that is not its own building's (kenshi-k16.7.234: a tall wall piece of another
			// building stood on the floor faces of Shark's wall complex and left a walkable lane under its body, round the end of the wall line)
			for (const PreparedCutter& pc : pcs_)
			{
				if (pieces.empty())
					break;
				if (pc.src == &c || !pc.box.overlaps(fb) || pc.src->label == c.label)	// a building's own shapes cut the source triangles above
					continue;
				const std::size_t before = pieces.size();
				const std::size_t dropped = cutByCutter(pc, pieces, fA, fB, fC, prm.cutBase, prm.characterHeight, 0.05);
				if (c.prebuilt && (dropped > 0 || pieces.size() != before))
					++st.topForeignCut;
			}
			for (const std::vector<P2>& piece : pieces)
			{
				double perimeter = 0;
				for (size_t k = 0; k < piece.size(); ++k)
					perimeter += std::hypot(piece[(k + 1) % piece.size()][0] - piece[k][0], piece[(k + 1) % piece.size()][1] - piece[k][1]);
				if (std::fabs(area2(piece)) < 0.04 || std::fabs(area2(piece)) / perimeter < 0.02)	// a sliver (the cuts leave them along a cell border)
					continue;
				std::vector<int> ids;
				for (const P2& p : piece)
				{
					const int v = topVertex(p[0], fA * p[0] + fB * p[1] + fC, p[1]);
					if (ids.empty() || (ids.back() != v && ids.front() != v))
						ids.push_back(v);
				}
				if (ids.size() < 3)
					continue;
				if (label < 0)
				{
					label = (int)topLabels_.size();
					topLabels_.push_back(c.label);
					topCutter_.push_back(&c);
				}
				topPolys_.push_back(std::move(ids));
				topLabel_.push_back(label);
				++st.topPieces;
			}
		}
	}
	if (prm.uniqueTops)
	{
		// One surface per covered area (kenshi-k16.7.207): two collision parts with the same flat top (or one object's overlapping triangles) made two
		// pieces over each other; the earlier one stays whole and a later piece keeps only what is not under an earlier piece at the same height
		// (within 0.3 u at the overlap's centre; a deck over the ground or a roof over a deck is another layer and stays).
		struct Kept
		{
			std::vector<P2> xz;
			size_t src; // the original piece whose surface it is
			Box2 box;
		};
		const std::vector<std::vector<int>> orig = topPolys_;
		// height of an original piece at (x, z): its fan triangle that holds the point (the pieces are not all planar), else the nearest triangle's plane
		const auto hAt = [&](size_t piece, double x, double z)
		{
			const std::vector<int>& ids = orig[piece];
			double best = 1e300, y = P_[ids[0]][1];
			for (size_t k = 1; k + 1 < ids.size(); ++k)
			{
				const P3& p0 = P_[ids[0]], & p1 = P_[ids[k]], & p2 = P_[ids[k + 1]];
				const double det = (p1[0] - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (p1[2] - p0[2]);
				if (std::fabs(det) < 1e-12)
					continue;
				const double u = ((x - p0[0]) * (p2[2] - p0[2]) - (p2[0] - p0[0]) * (z - p0[2])) / det;
				const double v = ((p1[0] - p0[0]) * (z - p0[2]) - (x - p0[0]) * (p1[2] - p0[2])) / det;
				const double out = std::max({ -u, -v, u + v - 1.0, 0.0 }); // how far outside the triangle (barycentric)
				if (out < best)
				{
					best = out;
					y = p0[1] + u * (p1[1] - p0[1]) + v * (p2[1] - p0[1]);
				}
			}
			return y;
		};
		std::vector<Kept> kept;
		std::unordered_map<uint64_t, std::vector<size_t>> cells;
		constexpr double cell = 32.0;
		const auto cellOf = [&](double v) { return (int64_t)std::floor(v / cell); };
		std::vector<std::vector<int>> outPolys;
		std::vector<int> outLabel;
		const auto edges = [](const std::vector<P2>& q)
		{
			std::vector<Line> lines;
			const double sg = area2(q) < 0 ? -1.0 : 1.0;
			for (size_t e = 0; e < q.size(); ++e)
			{
				const P2& u = q[e], & w = q[(e + 1) % q.size()];
				const double nx = -sg * (w[1] - u[1]), nz = sg * (w[0] - u[0]), l = std::hypot(nx, nz);
				if (l > 1e-12)
					lines.push_back({ nx / l, nz / l, -(nx * u[0] + nz * u[1]) / l });
			}
			return lines;
		};
		for (size_t i = 0; i < topPolys_.size(); ++i)
		{
			std::vector<P2> xz;
			for (const int v : topPolys_[i])
				xz.push_back({ P_[v][0], P_[v][2] });
			{
				const double sg = area2(xz) < 0 ? -1.0 : 1.0;
				for (size_t k = 0; k < xz.size(); ++k)
				{
					const P2& u = xz[k], & v = xz[(k + 1) % xz.size()], & w = xz[(k + 2) % xz.size()];
					if (sg * ((v[0] - u[0]) * (w[1] - v[1]) - (v[1] - u[1]) * (w[0] - v[0])) < -1e-3)
					{
						++st.topNonConvex;
						break;
					}
				}
			}
			const Box2 box = boxOf(xz);
			bool touched = false;
			std::vector<std::vector<P2>> frags = { xz };
			std::set<size_t> near;
			for (int64_t cx = cellOf(box.x0); cx <= cellOf(box.x1); ++cx)
				for (int64_t cz = cellOf(box.z0); cz <= cellOf(box.z1); ++cz)
					if (const auto it = cells.find(cellKeyOf(cx, cz)); it != cells.end())
						near.insert(it->second.begin(), it->second.end());
			for (const size_t j : near)
			{
				const Kept& k = kept[j];
				if (!k.box.overlaps(box))
					continue;
				const std::vector<Line> cover = edges(k.xz);
				std::vector<std::vector<P2>> next;
				for (std::vector<P2>& frag : frags)
				{
					// the part under k
					std::vector<P2> in = frag;
					std::vector<P2> neg, pos;
					bool ok = true;
					for (const Line& L : cover)
					{
						if (splitBy(in, L, neg, pos))
							in = pos;
						else
							for (const P2& q : in)
								if (L.at(q) < -1e-4)
								{
									ok = false;
									break;
								}
						if (!ok)
							break;
					}
					if (!ok || in.size() < 3 || std::fabs(area2(in)) / 2 <= 0.005)
					{
						next.push_back(std::move(frag));
						continue;
					}
					P2 mid = { 0, 0 };
					for (const P2& q : in)
						mid = { mid[0] + q[0] / in.size(), mid[1] + q[1] / in.size() };
					if (std::fabs(hAt(i, mid[0], mid[1]) - hAt(k.src, mid[0], mid[1])) > 0.3)
					{
						next.push_back(std::move(frag));
						continue;
					}
					touched = true;
					// subtract: the part outside each cover edge in turn
					std::vector<P2> rest = std::move(frag);
					for (const Line& L : cover)
					{
						if (splitBy(rest, L, neg, pos))
						{
							next.push_back(neg);
							rest = pos;
						} else {
							bool outside = false;
							for (const P2& q : rest)
								outside = outside || L.at(q) < -1e-4;
							if (outside)
							{
								next.push_back(std::move(rest));
								rest.clear();
								break;
							}
						}
					}
				}
				frags = std::move(next);
				if (frags.empty())
					break;
			}
			for (const std::vector<P2>& frag : frags)
			{
				if (frag.size() < 3)
					continue;
				double perimeter = 0;
				for (size_t k = 0; k < frag.size(); ++k)
					perimeter += std::hypot(frag[(k + 1) % frag.size()][0] - frag[k][0], frag[(k + 1) % frag.size()][1] - frag[k][1]);
				if (std::fabs(area2(frag)) < 0.04 || std::fabs(area2(frag)) / perimeter < 0.02)
					continue;
				std::vector<int> ids;
				if (!touched)
					ids = topPolys_[i]; // untouched
				else
					for (const P2& q : frag)
					{
						const int v = topVertex(q[0], hAt(i, q[0], q[1]), q[1]);
						if (ids.empty() || (ids.back() != v && ids.front() != v))
							ids.push_back(v);
					}
				if (ids.size() < 3)
					continue;
				Kept k{ frag, i, boxOf(frag) };
				kept.push_back(k);
				for (int64_t cx = cellOf(k.box.x0); cx <= cellOf(k.box.x1); ++cx)
					for (int64_t cz = cellOf(k.box.z0); cz <= cellOf(k.box.z1); ++cz)
						cells[cellKeyOf(cx, cz)].push_back(kept.size() - 1);
				outPolys.push_back(std::move(ids));
				outLabel.push_back(topLabel_[i]);
			}
		}
		st.topPieces = st.topPieces - topPolys_.size() + outPolys.size();
		topPolys_ = std::move(outPolys);
		topLabel_ = std::move(outLabel);
	}
	st.msTops = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Step 3: link the tops' boundary edges to the ground's and to each other. Two boundary edges of different faces link where they run against each
// other within matchSeparation in xz and matchStep in height over at least matchOverlap (the stretch of the overlap where both hold); an edge is split
// at the overlap ends (each side at its own end), and a stretch links once. A pair of ground edges never links here.
void Gen::linkTops(std::vector<std::vector<int>>& poly, std::vector<uint8_t>& dead, std::vector<uint8_t>& isTop,
	std::unordered_map<uint64_t, int>& owner, std::vector<Link>& links, std::vector<std::pair<int, int>>& touching, Stats& st)
{
	const auto t0 = std::chrono::steady_clock::now();
	const auto key = [](int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; };
	struct Edge
	{
		int f, k, u, w;
		double len;
		std::vector<std::pair<double, double>> claimed;	// fractions along the edge
		std::vector<double> cuts;
		std::map<double, int> at;	// fraction -> vertex id after the split
	};
	std::vector<Edge> E;
	for (size_t f = 0; f < poly.size(); ++f)
	{
		if (dead[f])
			continue;
		for (size_t k = 0; k < poly[f].size(); ++k)
		{
			const int u = poly[f][k], w = poly[f][(k + 1) % poly[f].size()];
			if (owner.count(key(w, u)))
				continue;
			const double len = std::hypot(P_[w][0] - P_[u][0], P_[w][2] - P_[u][2]);
			if (len < prm_.cutBase)
				continue;
			E.push_back({ (int)f, (int)k, u, w, len, {}, {}, {} });
		}
	}
	constexpr double cell = 8;
	std::unordered_map<uint64_t, std::vector<int>> byCell;
	for (size_t e = 0; e < E.size(); ++e)
	{
		const P3& U = P_[E[e].u], & W = P_[E[e].w];
		for (int64_t cx = (int64_t)std::floor(std::min(U[0], W[0]) / cell); cx <= (int64_t)std::floor(std::max(U[0], W[0]) / cell); ++cx)
			for (int64_t cz = (int64_t)std::floor(std::min(U[2], W[2]) / cell); cz <= (int64_t)std::floor(std::max(U[2], W[2]) / cell); ++cz)
				byCell[cellKeyOf(cx, cz)].push_back((int)e);
	}
	struct Cand
	{
		int a, b;
		double lo, hi;	// overlap, fractions of a
		double len;
	};
	std::vector<Cand> cands;
	// the edge that last tested each edge: an edge lies in several cells and is tested once (a list searched per test was quadratic in the edges near a
	// long edge: 52,52's ground took 1000 s on it, kenshi-k16.7.212)
	std::vector<int> seenBy(E.size(), -1);
	for (size_t ea = 0; ea < E.size(); ++ea)
	{
		const Edge& A = E[ea];
		if (!isTop[A.f])
			continue;
		const P3& a0 = P_[A.u], & a1 = P_[A.w];
		const double dax = a1[0] - a0[0], daz = a1[2] - a0[2];
		for (int64_t cx = (int64_t)std::floor((std::min(a0[0], a1[0]) - 2.5) / cell); cx <= (int64_t)std::floor((std::max(a0[0], a1[0]) + 2.5) / cell); ++cx)
			for (int64_t cz = (int64_t)std::floor((std::min(a0[2], a1[2]) - 2.5) / cell); cz <= (int64_t)std::floor((std::max(a0[2], a1[2]) + 2.5) / cell); ++cz)
			{
				const auto it = byCell.find(cellKeyOf(cx, cz));
				if (it == byCell.end())
					continue;
				for (const int eb : it->second)
				{
					if ((size_t)eb == ea || E[eb].f == A.f || (isTop[E[eb].f] && (size_t)eb < ea) || seenBy[eb] == (int)ea)
						continue;
					seenBy[eb] = (int)ea;
					const Edge& B = E[eb];
					const P3& b0 = P_[B.u], & b1 = P_[B.w];
					const double dbx = b1[0] - b0[0], dbz = b1[2] - b0[2];
					if ((dax * dbx + daz * dbz) / (A.len * B.len) > -0.9)
						continue;
					const double t0a = ((b0[0] - a0[0]) * dax + (b0[2] - a0[2]) * daz) / (A.len * A.len), t1a = ((b1[0] - a0[0]) * dax + (b1[2] - a0[2]) * daz) / (A.len * A.len);
					double lo = std::max(0.0, std::min(t0a, t1a)), hi = std::min(1.0, std::max(t0a, t1a));
					if ((hi - lo) * A.len < prm_.matchOverlap)
						continue;
					// the rule at a point of A: the nearest point of B within matchSeparation in xz and matchStep in height
					const auto meets = [&](double t)
					{
						const double px = a0[0] + t * dax, pz = a0[2] + t * daz, py = a0[1] + t * (a1[1] - a0[1]);
						const double s = std::clamp(((px - b0[0]) * dbx + (pz - b0[2]) * dbz) / (B.len * B.len), 0.0, 1.0);
						const double qx = b0[0] + s * dbx, qz = b0[2] + s * dbz, qy = b0[1] + s * (b1[1] - b0[1]);
						return std::hypot(qx - px, qz - pz) <= prm_.matchSeparation && std::fabs(qy - py) <= prm_.matchStep;
					};
					if (!meets(lo) || !meets(hi))
					{
						// Two edges that run against each other for part of the overlap link over that part (kenshi-k16.7.212: with both ends tested only,
						// a long edge linked nothing and the link hung on how finely the faces happened to be cut). Along A the distance to B's line and
						// the height over it are linear: the stretch where both are within the limits, checked by the rule at its ends.
						const auto off = [&](double t, double& gap, double& step)
						{
							const double px = a0[0] + t * dax, pz = a0[2] + t * daz;
							const double s = ((px - b0[0]) * dbx + (pz - b0[2]) * dbz) / (B.len * B.len);
							gap = ((px - b0[0]) * dbz - (pz - b0[2]) * dbx) / B.len;
							step = a0[1] + t * (a1[1] - a0[1]) - (b0[1] + s * (b1[1] - b0[1]));
						};
						double gap0, step0, gap1, step1, u0 = 0, u1 = 1;	// the stretch as fractions of lo .. hi
						off(lo, gap0, step0);
						off(hi, gap1, step1);
						const auto clip = [&](double f0, double f1, double limit)
						{
							for (const double sign : { 1.0, -1.0 })
							{
								// sign * f(u) <= limit, f linear from f0 to f1
								const double g0 = sign * f0 - limit, g1 = sign * f1 - limit;
								if (g0 > 0 && g1 > 0)
									u1 = -1;
								else if (g0 > 0)
									u0 = std::max(u0, g0 / (g0 - g1));
								else if (g1 > 0)
									u1 = std::min(u1, g0 / (g0 - g1));
							}
						};
						clip(gap0, gap1, prm_.matchSeparation);
						clip(step0, step1, prm_.matchStep);
						if (u1 <= u0)
							continue;
						// a hair inside, so the rule holds at the ends in floating point
						const double nlo = lo + (u0 > 0 ? u0 + 1e-9 : 0.0) * (hi - lo), nhi = lo + (u1 < 1 ? u1 - 1e-9 : 1.0) * (hi - lo);
						lo = nlo;
						hi = nhi;
						if ((hi - lo) * A.len < prm_.matchOverlap || !meets(lo) || !meets(hi))
							continue;
					}
					cands.push_back({ (int)ea, eb, lo, hi, (hi - lo) * A.len });
					touching.push_back({ A.f, B.f });
				}
			}
	}
	std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.len > y.len; });
	struct Piece
	{
		int ea, eb;
		double a0, a1, b0, b1;	// fractions: a0 < a1 along A, b0 < b1 along B
	};
	std::vector<Piece> pieces;
	// free parts of [lo, hi] (fractions of A) once the claimed stretches of both edges are taken out
	for (const Cand& cd : cands)
	{
		Edge& A = E[cd.a];
		Edge& B = E[cd.b];
		const P3& a0 = P_[A.u], & a1 = P_[A.w], & b0 = P_[B.u], & b1 = P_[B.w];
		const double dax = a1[0] - a0[0], daz = a1[2] - a0[2], dbx = b1[0] - b0[0], dbz = b1[2] - b0[2];
		const auto bOfA = [&](double t)
		{
			return std::clamp(((a0[0] + t * dax - b0[0]) * dbx + (a0[2] + t * daz - b0[2]) * dbz) / (B.len * B.len), 0.0, 1.0);
		};
		const auto aOfB = [&](double s)
		{
			return ((b0[0] + s * dbx - a0[0]) * dax + (b0[2] + s * dbz - a0[2]) * daz) / (A.len * A.len);
		};
		std::vector<std::pair<double, double>> taken = A.claimed;
		for (const auto& [s0, s1] : B.claimed)
			taken.push_back({ std::min(aOfB(s0), aOfB(s1)), std::max(aOfB(s0), aOfB(s1)) });
		std::sort(taken.begin(), taken.end());
		double at = cd.lo;
		std::vector<std::pair<double, double>> free;
		for (const auto& [x, y] : taken)
		{
			if (y <= at)
				continue;
			if (x >= cd.hi)
				break;
			if (x > at)
				free.push_back({ at, x });
			at = std::max(at, y);
		}
		if (at < cd.hi)
			free.push_back({ at, cd.hi });
		for (const auto& [p, q] : free)
		{
			if ((q - p) * A.len < prm_.matchOverlap)
				continue;
			const double sp = bOfA(p), sq = bOfA(q);
			Piece pc = { cd.a, cd.b, p, q, std::min(sp, sq), std::max(sp, sq) };
			// B's claims in B's own fractions: the projection of A's free part can reach a few hundredths into a stretch B already linked
			// (the edges are not parallel), and a cut inside that stretch leaves the earlier link's ends on two sub-edges, so it is lost
			// at emission (a Metal Arch Bridge deck fell in two at a 0.8 u step, kenshi-ffmi.7). Shrink the piece to B's free part.
			bool inside = false, lo = false, hi = false;
			for (const auto& [x, y] : B.claimed)
			{
				if (y <= pc.b0 || x >= pc.b1)
					continue;
				if (x <= pc.b0)
				{
					pc.b0 = y;
					lo = true;
				} else if (y >= pc.b1) {
					pc.b1 = x;
					hi = true;
				} else {
					inside = true;
				}
			}
			if (inside || pc.b1 <= pc.b0)
				continue;
			// the A end that faces a moved B end follows it (antiparallel: B's low end pairs with A's high end)
			const bool same = sp < sq;
			if (lo)
				*(same ? &pc.a0 : &pc.a1) = std::clamp(aOfB(pc.b0), p, q);
			if (hi)
				*(same ? &pc.a1 : &pc.a0) = std::clamp(aOfB(pc.b1), p, q);
			for (double* f : { &pc.a0, &pc.a1 })
			{
				if (*f * A.len < 0.01) *f = 0;
				if ((1 - *f) * A.len < 0.01) *f = 1;
			}
			for (double* f : { &pc.b0, &pc.b1 })
			{
				if (*f * B.len < 0.01) *f = 0;
				if ((1 - *f) * B.len < 0.01) *f = 1;
			}
			// an end that lands within 0.01 u of an earlier cut of the edge reuses it (no sliver edge between two vertices of one split)
			const auto snapCut = [](const Edge& ed, double& f)
			{
				for (const double c : ed.cuts)
					if (std::fabs(c - f) * ed.len < 0.01)
					{
						f = c;
						return;
					}
			};
			snapCut(A, pc.a0);
			snapCut(A, pc.a1);
			snapCut(B, pc.b0);
			snapCut(B, pc.b1);
			if ((pc.a1 - pc.a0) * A.len < prm_.matchOverlap || (pc.b1 - pc.b0) * B.len < prm_.matchOverlap)
				continue;
			A.claimed.push_back({ pc.a0, pc.a1 });
			B.claimed.push_back({ pc.b0, pc.b1 });
			for (const double f : { pc.a0, pc.a1 })
				if (f > 0 && f < 1)
					A.cuts.push_back(f);
			for (const double f : { pc.b0, pc.b1 })
				if (f > 0 && f < 1)
					B.cuts.push_back(f);
			pieces.push_back(pc);
		}
	}
	// split the edges at the cut fractions, each cut a new vertex on the edge's own line and height
	std::unordered_map<uint64_t, int> edgeAt;	// (f, k) -> edge index
	for (size_t e = 0; e < E.size(); ++e)
	{
		Edge& ed = E[e];
		ed.at[0.0] = ed.u;
		ed.at[1.0] = ed.w;
		std::sort(ed.cuts.begin(), ed.cuts.end());
		const P3 U = P_[ed.u], W = P_[ed.w];
		for (const double f : ed.cuts)
			if (!ed.at.count(f))
				ed.at[f] = addVertex({ U[0] + f * (W[0] - U[0]), U[1] + f * (W[1] - U[1]), U[2] + f * (W[2] - U[2]) });
		edgeAt[key(ed.f, ed.k)] = (int)e;
	}
	for (size_t f = 0; f < poly.size(); ++f)
	{
		if (dead[f])
			continue;
		std::vector<int> out;
		for (size_t k = 0; k < poly[f].size(); ++k)
		{
			out.push_back(poly[f][k]);
			const auto it = edgeAt.find(key((int)f, (int)k));
			if (it == edgeAt.end())
				continue;
			const Edge& ed = E[it->second];
			for (const auto& [frac, v] : ed.at)
				if (frac > 0 && frac < 1)
					out.push_back(v);
		}
		poly[f] = std::move(out);
	}
	for (const Piece& pc : pieces)
	{
		const Edge& A = E[pc.ea];
		const Edge& B = E[pc.eb];
		links.push_back({ A.f, B.f, A.at.at(pc.a0), A.at.at(pc.a1), B.at.at(pc.b0), B.at.at(pc.b1) });
	}
	st.topLinks = links.size();
	st.msLink = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	(void)isTop;
}

native::Mesh Gen::finish(const std::vector<std::array<double, 2>>& seeds, Stats& st)
{
	const auto t0 = std::chrono::steady_clock::now();
	// Hertel-Mehlhorn: polygons start as the live triangles; drop shared edges, longest first, while the union stays convex and planar enough
	std::vector<std::vector<int>> poly;
	std::vector<uint8_t> dead;
	std::unordered_map<uint64_t, int> owner;
	const auto key = [](int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; };
	for (const Tri& t : T_)
	{
		if (!t.alive)
			continue;
		poly.push_back({ t.v[0], t.v[1], t.v[2] });
		dead.push_back(0);
		for (int k = 0; k < 3; ++k)
			owner[key(t.v[k], t.v[(k + 1) % 3])] = (int)poly.size() - 1;
	}
	st.trisAfter = poly.size();
	std::vector<uint8_t> isTop(poly.size(), 0);
	std::vector<int> polyLabel(poly.size(), -1);
	for (size_t i = 0; i < topPolys_.size(); ++i)
	{
		const std::vector<int>& tp = topPolys_[i];
		for (size_t k = 0; k < tp.size(); ++k)
			owner[key(tp[k], tp[(k + 1) % tp.size()])] = (int)poly.size();
		poly.push_back(tp);
		dead.push_back(0);
		isTop.push_back(1);
		polyLabel.push_back(topLabel_[i]);
	}
	const auto polyNormal = [&](const std::vector<int>& p)
	{
		P3 n = { 0, 0, 0 };
		for (size_t k = 1; k + 1 < p.size(); ++k)
		{
			const P3 m = normal(P_[p[0]], P_[p[k]], P_[p[k + 1]]);
			for (int c = 0; c < 3; ++c)
				n[c] += m[c];
		}
		const double len = length(n);
		if (len > 0)
			for (double& c : n)
				c /= len;
		return n;
	};
	// Production merge first (aligned faces only, as before straighten), then the straighten pass over what is left: a union that spans the
	// same faces in another order leaves needles stranded (knav_ground zone 46.16)
	for (int pass = 0; pass < (prm_.straighten ? 2 : 1); ++pass)
	{
		std::vector<std::tuple<double, int, int>> shared;
		for (const auto& [k, f] : owner)
		{
			(void)f;
			const int a = (int)(k >> 32), b = (int)(k & 0xffffffffu);
			if (a < b && owner.count(key(b, a)))
				shared.push_back({ std::hypot(P_[a][0] - P_[b][0], P_[a][2] - P_[b][2]), a, b });
		}
		std::sort(shared.begin(), shared.end(), [](const auto& x, const auto& y) { return std::get<0>(x) > std::get<0>(y); });
		for (const auto& [len, a, b] : shared)
		{
			(void)len;
			const auto ia = owner.find(key(a, b)), ib = owner.find(key(b, a));
			if (ia == owner.end() || ib == owner.end())
				continue;
			const int fp = ia->second, fq = ib->second;
			if (fp == fq || dead[fp] || dead[fq])
				continue;
			const std::vector<int>& p = poly[fp], & q = poly[fq];
			const P3 np = polyNormal(p), nq = polyNormal(q);
			const bool aligned = np[0] * nq[0] + np[1] * nq[1] + np[2] * nq[2] >= prm_.planarCos;
			if (!aligned && (pass == 0 || isTop[fp] || isTop[fq]))	// a deck never merges across its slope limit
				continue;
			// p holds a -> b, q holds b -> a: p from b round to a, then q's vertices after a up to before b
			const size_t pb = std::find(p.begin(), p.end(), b) - p.begin(), qa = std::find(q.begin(), q.end(), a) - q.begin();
			std::vector<int> m;
			for (size_t k = 0; k < p.size(); ++k)
				m.push_back(p[(pb + k) % p.size()]);
			for (size_t k = 1; k + 1 < q.size(); ++k)
				m.push_back(q[(qa + k) % q.size()]);
			// two faces that share edges apart (a ring round a hole) would union into a ring that visits a corner twice
			bool convex = true;
			for (size_t k = 0; k < m.size() && convex; ++k)
				for (size_t l = k + 1; l < m.size() && convex; ++l)
					convex = m[k] != m[l];
			for (size_t k = 0; k < m.size() && convex; ++k)
				convex = cross2(P_[m[k]], P_[m[(k + 1) % m.size()]], P_[m[(k + 2) % m.size()]]) >= -1e-9;
			if (!convex || m.size() >= 255)	// the original's maxNumEdgesPerFace: nA + nB - 2 < 255 (kenshi-rwh.337)
				continue;
			if (!aligned)
			{
				// two corners on one spot (a zero-area triangle the union would carry in) keep the union out of the straighten pass
				bool twice = false;
				for (size_t k = 0; k < m.size() && !twice; ++k)
					for (size_t l = k + 1; l < m.size() && !twice; ++l)
						twice = std::hypot(P_[m[k]][0] - P_[m[l]][0], P_[m[k]][2] - P_[m[l]][2]) < 0.01;
				if (twice)
					continue;
				// straighten: the union's corners within slopY of its best-fit plane (area-weighted normal through the centroid)
				const P3 nm = polyNormal(m);
				P3 c = { 0, 0, 0 };
				for (const int x : m)
					for (int k = 0; k < 3; ++k)
						c[k] += P_[x][k] / m.size();
				double dev = 0;
				for (const int x : m)
					dev = std::max(dev, std::fabs(nm[0] * (P_[x][0] - c[0]) + nm[1] * (P_[x][1] - c[1]) + nm[2] * (P_[x][2] - c[2])));
				if (dev > prm_.slopY)
					continue;
			}
			owner.erase(key(a, b));
			owner.erase(key(b, a));
			for (size_t k = 0; k < q.size(); ++k)
			{
				const int x = q[k], y = q[(k + 1) % q.size()];
				if (!(x == b && y == a))
					owner[key(x, y)] = fp;
			}
			poly[fp] = std::move(m);
			dead[fq] = 1;
		}
	}
	std::vector<Link> links;
	std::vector<std::pair<int, int>> touching;
	if (!topPolys_.empty())
		linkTops(poly, dead, isTop, owner, links, touching, st);
	const auto t1 = std::chrono::steady_clock::now();

	// Components over shared edges and the tops' links; keep those with a seed point or a rim edge
	std::vector<int> parent(poly.size());
	for (size_t f = 0; f < poly.size(); ++f)
		parent[f] = (int)f;
	const std::function<int(int)> root = [&](int f) { while (parent[f] != f) f = parent[f] = parent[parent[f]]; return f; };
	for (const auto& [k, f] : owner)
	{
		const auto o = owner.find(key((int)(k & 0xffffffffu), (int)(k >> 32)));
		if (o != owner.end())
			parent[root(f)] = root(o->second);
	}
	for (const Link& l : links)
		parent[root(l.fa)] = root(l.fb);
	// Every pair of faces whose edges meet the matcher's rule is one component for the prune, linked or not: a stretch of an edge links once, so a
	// face that meets an edge already linked to another gets no link there, and a deck hung on that one meeting was dropped whole (zone 41,54,
	// kenshi-k16.7.212; before, the fragments of the cut left enough spare edges). nativeTiles pairs the edges still open.
	for (const auto& [fa, fb] : touching)
		parent[root(fa)] = root(fb);
	std::vector<uint8_t> keepRoot(poly.size(), 0);
	for (size_t f = 0; f < poly.size(); ++f)
	{
		if (dead[f])
			continue;
		const std::vector<int>& p = poly[f];
		bool keep = false;
		for (size_t k = 0; k < p.size() && !keep && prm_.keepRim; ++k)
			keep = (rim_[p[k]] & rim_[p[(k + 1) % p.size()]]) != 0;
		for (const auto& s : seeds)
		{
			if (keep || isTop[f])
				break;
			bool inside = true;
			const P3 q = { s[0], 0, s[1] };
			for (size_t k = 0; k < p.size() && inside; ++k)
				inside = cross2(P_[p[k]], P_[p[(k + 1) % p.size()]], q) >= -1e-9;
			keep = inside;
		}
		if (!keep && isTop[f] && !topCutter_[polyLabel[f]]->seam.empty())
			for (size_t k = 0; k < p.size() && !keep; ++k)
			{
				const P3& u = P_[p[k]], & w = P_[p[(k + 1) % p.size()]];
				const P3 m = { (u[0] + w[0]) / 2, (u[1] + w[1]) / 2, (u[2] + w[2]) / 2 };
				for (const auto& sg : topCutter_[polyLabel[f]]->seam)
				{
					const double dx = sg[1][0] - sg[0][0], dz = sg[1][2] - sg[0][2], l2 = dx * dx + dz * dz;
					const double t = l2 > 0 ? std::clamp(((m[0] - sg[0][0]) * dx + (m[2] - sg[0][2]) * dz) / l2, 0.0, 1.0) : 0.0;
					if (std::hypot(sg[0][0] + t * dx - m[0], sg[0][2] + t * dz - m[2]) <= 0.05 && std::fabs(sg[0][1] + t * (sg[1][1] - sg[0][1]) - m[1]) <= 1.0)
					{
						keep = true;
						break;
					}
				}
			}
		if (keep || (isTop[f] && prm_.keepLooseTops))
			keepRoot[root((int)f)] = 1;
	}
	std::vector<uint8_t> seenRoot(poly.size(), 0);
	for (size_t f = 0; f < poly.size(); ++f)
	{
		if (dead[f])
			continue;
		const int r = root((int)f);
		if (!seenRoot[r])
		{
			seenRoot[r] = 1;
			++st.components;
			st.droppedComponents += !keepRoot[r];
		}
		if (!keepRoot[r])
		{
			dead[f] = 1;
			if (isTop[f])
			{
				double a2 = 0;
				for (size_t k = 0; k < poly[f].size(); ++k)
					a2 += cross2(P_[poly[f][0]], P_[poly[f][k]], P_[poly[f][(k + 1) % poly[f].size()]]);
				++st.topDroppedFaces;
				st.topDroppedArea += a2 / 2;
				st.topDropped[topLabels_[polyLabel[f]]] += a2 / 2;
			} else {
				++st.droppedFaces;
			}
		}
	}
	const auto t2 = std::chrono::steady_clock::now();

	// materialRule: a building's top keeps every corner of its outline but the collinear ones (the original's one simplifier takes a building shape's
	// loop that way). A vertex goes from a top face when it belongs to that face alone, both its edges are walls (no face across, no seam, no rim link)
	// and it lies within buildingTol of the chord of its neighbours. Feature and foliage tops stay as they are (kenshi-k16.7.246).
	if (prm_.materialRule)
	{
		std::unordered_map<int, int> users;
		std::set<std::pair<int, int>> dirEdges;
		for (size_t f = 0; f < poly.size(); ++f)
			if (!dead[f])
				for (size_t k = 0; k < poly[f].size(); ++k)
				{
					++users[poly[f][k]];
					dirEdges.insert({ poly[f][k], poly[f][(k + 1) % poly[f].size()] });
				}
		std::unordered_set<int> linkEnds;
		for (const Link& l : links)
			linkEnds.insert({ l.a0, l.a1, l.b0, l.b1 });
		for (size_t f = 0; f < poly.size(); ++f)
		{
			if (dead[f] || !isTop[f] || materialOfLabel(topLabels_[polyLabel[f]]) != Material::Building || poly[f].size() < 4)
				continue;
			const Cutter& src = *topCutter_[polyLabel[f]];
			const auto wall = [&](int a, int b)
			{
				if (dirEdges.count({ b, a }) || (rim_[a] & rim_[b]))
					return false;
				const P3& u = P_[a], & w = P_[b];
				const P3 mid = { (u[0] + w[0]) / 2, (u[1] + w[1]) / 2, (u[2] + w[2]) / 2 };
				for (const auto& sg : src.seam)
				{
					const double dx = sg[1][0] - sg[0][0], dz = sg[1][2] - sg[0][2], l2 = dx * dx + dz * dz;
					const double t = l2 > 0 ? std::clamp(((mid[0] - sg[0][0]) * dx + (mid[2] - sg[0][2]) * dz) / l2, 0.0, 1.0) : 0.0;
					if (std::hypot(sg[0][0] + t * dx - mid[0], sg[0][2] + t * dz - mid[2]) <= 0.25 && std::fabs(sg[0][1] + t * (sg[1][1] - sg[0][1]) - mid[1]) <= 1.0)
						return false;
				}
				return true;
			};
			std::vector<int> ring = poly[f];
			for (size_t k = 0; k < ring.size() && ring.size() > 3;)
			{
				const int prev = ring[(k + ring.size() - 1) % ring.size()], v = ring[k], next = ring[(k + 1) % ring.size()];
				bool drop = false;
				if (users[v] == 1 && !linkEnds.count(v) && !rim_[v] && wall(prev, v) && wall(v, next))
				{
					const P3& A = P_[prev], & B = P_[next], & V = P_[v];
					const double dx = B[0] - A[0], dy = B[1] - A[1], dz = B[2] - A[2], l2 = dx * dx + dy * dy + dz * dz;
					const double t = l2 > 0 ? std::clamp(((V[0] - A[0]) * dx + (V[1] - A[1]) * dy + (V[2] - A[2]) * dz) / l2, 0.0, 1.0) : 0.0;
					drop = std::sqrt((A[0] + t * dx - V[0]) * (A[0] + t * dx - V[0]) + (A[1] + t * dy - V[1]) * (A[1] + t * dy - V[1])
						+ (A[2] + t * dz - V[2]) * (A[2] + t * dz - V[2])) <= prm_.topCollinearTol && l2 > 1e-6;
				}
				if (drop)
				{
					dirEdges.erase({ prev, v });
					dirEdges.erase({ v, next });
					dirEdges.insert({ prev, next });
					--users[v];
					ring.erase(ring.begin() + k);
					++st.topCollinearDropped;
				} else {
					++k;
				}
			}
			poly[f] = std::move(ring);
		}
	}

	native::Mesh mesh;
	std::vector<int> remap(P_.size(), -1), meshFace(poly.size(), -1);
	std::unordered_map<uint64_t, int> edgeAt;
	for (size_t f = 0; f < poly.size(); ++f)
	{
		if (dead[f])
			continue;
		native::Face face;
		face.top = isTop[f];
		face.startEdge = (uint32_t)mesh.edges.size();
		face.numEdges = (uint16_t)poly[f].size();
		for (size_t k = 0; k < poly[f].size(); ++k)
		{
			const int a = poly[f][k], b = poly[f][(k + 1) % poly[f].size()];
			for (const int x : { a, b })
				if (remap[x] < 0)
				{
					remap[x] = (int)mesh.vertices.size();
					mesh.vertices.push_back(P_[x]);
				}
			native::Edge e;
			e.a = (uint32_t)remap[a];
			e.b = (uint32_t)remap[b];
			e.linked = (rim_[a] & rim_[b]) != 0;
			// a zone side's seam to the interior (its mask boundary, the doorways among it): the world side of the streaming link nativeTiles pairs with the interior's
			if (!e.linked && isTop[f] && !topCutter_[polyLabel[f]]->seam.empty())
			{
				const P3& u = P_[a], & w = P_[b];
				const P3 mid = { (u[0] + w[0]) / 2, (u[1] + w[1]) / 2, (u[2] + w[2]) / 2 };
				for (const auto& sg : topCutter_[polyLabel[f]]->seam)
				{
					const double dx = sg[1][0] - sg[0][0], dz = sg[1][2] - sg[0][2], l2 = dx * dx + dz * dz;
					const double t = l2 > 0 ? std::clamp(((mid[0] - sg[0][0]) * dx + (mid[2] - sg[0][2]) * dz) / l2, 0.0, 1.0) : 0.0;
					if (std::hypot(sg[0][0] + t * dx - mid[0], sg[0][2] + t * dz - mid[2]) <= 0.05 && std::fabs(sg[0][1] + t * (sg[1][1] - sg[0][1]) - mid[1]) <= 1.0)
					{
						e.linked = true;
						break;
					}
				}
			}
			edgeAt[key(a, b)] = (int)mesh.edges.size();
			mesh.edges.push_back(e);
		}
		mesh.faces.push_back(face);
		meshFace[f] = (int)mesh.faces.size() - 1;
		if (isTop[f])
		{
			st.topFaceIndex.push_back((uint32_t)mesh.faces.size() - 1);
			++st.topFaces;
			for (size_t k = 0; k < poly[f].size(); ++k)
			{
				const double a2 = cross2(P_[poly[f][0]], P_[poly[f][k]], P_[poly[f][(k + 1) % poly[f].size()]]) / 2;
				st.topArea += a2;
				st.topKept[topLabels_[polyLabel[f]]] += a2;
			}
		}
	}
	// opposite edges: the reverse key, in the same map
	std::vector<int> faceOf(mesh.edges.size());
	for (size_t f = 0; f < mesh.faces.size(); ++f)
		for (uint32_t k = mesh.faces[f].startEdge; k < mesh.faces[f].startEdge + mesh.faces[f].numEdges; ++k)
			faceOf[k] = (int)f;
	for (const auto& [k, e] : edgeAt)
	{
		const auto o = edgeAt.find(key((int)(k & 0xffffffffu), (int)(k >> 32)));
		if (o != edgeAt.end())
		{
			mesh.edges[e].oppositeEdge = o->second;
			mesh.edges[e].oppositeFace = faceOf[o->second];
			mesh.edges[e].linked = false;
		}
	}
	for (const Link& l : links)
	{
		const auto a = edgeAt.find(key(l.a0, l.a1)), b = edgeAt.find(key(l.b0, l.b1));
		if (a == edgeAt.end() || b == edgeAt.end() || mesh.edges[a->second].oppositeEdge >= 0 || mesh.edges[b->second].oppositeEdge >= 0)
			continue;
		mesh.edges[a->second].oppositeEdge = b->second;
		mesh.edges[a->second].oppositeFace = faceOf[b->second];
		mesh.edges[b->second].oppositeEdge = a->second;
		mesh.edges[b->second].oppositeFace = faceOf[a->second];
		mesh.edges[a->second].linked = mesh.edges[b->second].linked = false;
	}
	for (const auto& [fa, fb] : touching)
		if (meshFace[fa] >= 0 && meshFace[fb] >= 0)
			st.topTouching.push_back({ (uint32_t)meshFace[fa], (uint32_t)meshFace[fb] });
	st.faces = mesh.faces.size();
	st.msMerge = std::chrono::duration<double, std::milli>(t1 - t0).count();
	st.msPrune = std::chrono::duration<double, std::milli>(t2 - t1).count();
	return mesh;
}

} // namespace

native::Mesh generate(const Grid& grid, const std::vector<std::array<double, 2>>& seeds, const std::vector<Cutter>& cutters, const Params& params,
	Stats* stats)
{
	Stats local;
	Stats& st = stats ? *stats : local;
	st = {};
	Gen gen(grid, params);
	const auto t0 = std::chrono::steady_clock::now();
	gen.build(st);
	const auto t1 = std::chrono::steady_clock::now();
	gen.prepareCutters(cutters);
	gen.cut(cutters, st);
	const auto tt = std::chrono::steady_clock::now();
	gen.trim(st);
	const auto tc = std::chrono::steady_clock::now();
	st.msTrim = std::chrono::duration<double, std::milli>(tc - tt).count();
	gen.simplify(st);
	if (params.trimAfterSimplify)
	{
		const std::size_t tips = st.trimmedTips;
		const double area = st.trimmedArea;
		gen.trim(st);
		st.postTrimmedTips = st.trimmedTips - tips;
		st.postTrimmedArea = st.trimmedArea - area;
	}
	const auto t2 = std::chrono::steady_clock::now();
	gen.tops(cutters, params, st);
	st.msGrid = std::chrono::duration<double, std::milli>(t1 - t0).count();
	st.msCut = std::chrono::duration<double, std::milli>(tt - t1).count();
	st.msSimplify = std::chrono::duration<double, std::milli>(t2 - tc).count();
	return gen.finish(seeds, st);
}

namespace
{

// Inserts into `m`'s boundary edges on the line the vertices `pts` (xz) that fall inside them. Returns the count.
std::size_t insertOnLine(native::Mesh& m, int axis, double edge, const std::vector<std::array<double, 3>>& pts)
{
	const int other = axis == 0 ? 2 : 0;
	std::vector<std::vector<std::uint32_t>> split(m.edges.size());	// per edge: new vertex ids, sorted along it
	std::size_t n = 0;
	std::map<std::array<long long, 3>, std::uint32_t> made;	// a vertex inserted once serves every edge that meets it
	for (std::uint32_t f = 0; f < m.faces.size(); ++f)
		for (std::uint32_t e = m.faces[f].startEdge; e < m.faces[f].startEdge + m.faces[f].numEdges; ++e)
		{
			const native::Edge& ed = m.edges[e];
			if (ed.oppositeEdge >= 0)
				continue;
			const std::array<double, 3> u = m.vertices[ed.a], w = m.vertices[ed.b];	// copies: the insert below grows m.vertices (references dangled, kenshi-k16.7.212)
			if (std::fabs(u[axis] - edge) > 1e-6 || std::fabs(w[axis] - edge) > 1e-6 || std::fabs(w[other] - u[other]) < 1e-3)
				continue;
			std::vector<std::pair<double, std::uint32_t>> at;
			for (const auto& p : pts)
			{
				const double t = (p[other] - u[other]) / (w[other] - u[other]);
				if (t * std::fabs(w[other] - u[other]) < 1e-3 || (1 - t) * std::fabs(w[other] - u[other]) < 1e-3 || t >= 1)
					continue;
				if (std::fabs(u[1] + t * (w[1] - u[1]) - p[1]) > 5.0)	// another surface over or under the edge (the matcher's step)
					continue;
				const std::array<long long, 3> key = { std::llround(p[0] * 1000), std::llround((u[1] + t * (w[1] - u[1])) * 100), std::llround(p[2] * 1000) };
				auto it = made.find(key);
				if (it == made.end())
				{
					std::array<double, 3> v = p;
					v[1] = u[1] + t * (w[1] - u[1]);
					m.vertices.push_back(v);
					it = made.emplace(key, (std::uint32_t)m.vertices.size() - 1).first;
					++n;
				}
				at.push_back({ t, it->second });
			}
			std::sort(at.begin(), at.end());
			for (const auto& [t, v] : at)
				split[e].push_back(v);
		}
	if (n == 0)
		return 0;
	native::Mesh out = m;
	out.faces.clear();
	out.edges.clear();
	std::vector<std::uint32_t> first(m.edges.size());	// the old edge's first new edge (its opposite keeps the index of the part that faces it)
	for (const native::Face& src : m.faces)
	{
		native::Face face = src;
		face.startEdge = (std::uint32_t)out.edges.size();
		for (std::uint32_t e = src.startEdge; e < src.startEdge + src.numEdges; ++e)
		{
			first[e] = (std::uint32_t)out.edges.size();
			native::Edge ed = m.edges[e];
			std::uint32_t from = ed.a;
			for (const std::uint32_t v : split[e])
			{
				native::Edge part = ed;
				part.a = from;
				part.b = v;
				out.edges.push_back(part);
				from = v;
			}
			ed.a = from;
			out.edges.push_back(ed);
		}
		face.numEdges = (std::uint16_t)(out.edges.size() - face.startEdge);
		out.faces.push_back(face);
	}
	// opposite edges: an edge that was split had none; the others point at the first of the (unsplit) new edge
	for (std::size_t e = 0; e < m.edges.size(); ++e)
		if (m.edges[e].oppositeEdge >= 0)
			out.edges[first[e]].oppositeEdge = (std::int32_t)first[(std::size_t)m.edges[e].oppositeEdge];
	m = std::move(out);
	return n;
}

} // namespace

std::size_t stitchRims(native::Mesh& a, native::Mesh& b, int axis, double edge)
{
	const auto onLine = [&](const native::Mesh& m)
	{
		std::vector<std::array<double, 3>> v;
		for (const native::Edge& e : m.edges)
			if (e.oppositeEdge < 0)
				for (const std::uint32_t x : { e.a, e.b })
					if (std::fabs(m.vertices[x][axis] - edge) < 1e-6)
						v.push_back(m.vertices[x]);
		return v;
	};
	const std::vector<std::array<double, 3>> va = onLine(a), vb = onLine(b);
	return insertOnLine(a, axis, edge, vb) + insertOnLine(b, axis, edge, va);
}

std::size_t pruneJoined(native::Mesh& world, const std::vector<std::uint32_t>& worldTops, const std::vector<JoinedSide>& sides,
	const std::vector<std::array<std::uint32_t, 2>>* worldTouching)
{
	// Faces of every mesh in one numbering; union over opposite edges, and over the rim edges that run over the same two points across a seam.
	std::vector<const native::Mesh*> meshes{ &world };
	std::vector<const std::vector<std::uint32_t>*> topLists{ &worldTops };
	std::vector<const std::vector<std::array<std::uint32_t, 2>>*> touching{ worldTouching };
	for (const JoinedSide& side : sides)
	{
		meshes.push_back(side.mesh);
		topLists.push_back(side.tops);
		touching.push_back(side.touching);
	}
	std::vector<std::size_t> base(meshes.size() + 1, 0);
	for (std::size_t i = 0; i < meshes.size(); ++i)
		base[i + 1] = base[i] + meshes[i]->faces.size();
	std::vector<std::size_t> parent(base.back());
	for (std::size_t i = 0; i < parent.size(); ++i)
		parent[i] = i;
	const std::function<std::size_t(std::size_t)> find = [&](std::size_t x) { while (parent[x] != x) x = parent[x] = parent[parent[x]]; return x; };
	const auto unite = [&](std::size_t a, std::size_t b) { parent[find(a)] = find(b); };
	for (std::size_t i = 0; i < meshes.size(); ++i)
		if (touching[i] != nullptr)
			for (const auto& pair : *touching[i])
				unite(base[i] + pair[0], base[i] + pair[1]);
	std::vector<uint8_t> open(base.back(), 0);	// a face with a way out of what is known: ground, a seam to an interior, a rim edge to a window not given
	std::map<std::array<std::int64_t, 4>, std::size_t> rimAt;	// (a, b) xz in 1/1000 u -> the face on it
	const auto q = [](double v) { return (std::int64_t)std::llround(v * 1000.0); };
	for (std::size_t i = 0; i < meshes.size(); ++i)
	{
		const native::Mesh& m = *meshes[i];
		std::vector<uint8_t> top(m.faces.size(), 0);
		for (const std::uint32_t f : *topLists[i])
			top[f] = 1;
		double lo[2] = { 1e300, 1e300 }, hi[2] = { -1e300, -1e300 };
		for (const native::Edge& e : m.edges)
			for (const std::uint32_t v : { e.a, e.b })
				for (int k = 0; k < 2; ++k)
				{
					lo[k] = std::min(lo[k], m.vertices[v][k * 2]);
					hi[k] = std::max(hi[k], m.vertices[v][k * 2]);
				}
		for (std::size_t f = 0; f < m.faces.size(); ++f)
		{
			open[base[i] + f] = !top[f];
			for (std::uint32_t k = m.faces[f].startEdge; k < m.faces[f].startEdge + m.faces[f].numEdges; ++k)
			{
				const native::Edge& e = m.edges[k];
				if (e.oppositeEdge >= 0)
				{
					unite(base[i] + f, base[i] + (std::size_t)e.oppositeFace);
					continue;
				}
				const auto& a = m.vertices[e.a];
				const auto& b = m.vertices[e.b];
				bool border = false;
				for (int c = 0; c < 2; ++c)
					for (const double edge : { lo[c], hi[c] })
						border = border || (std::fabs(a[c * 2] - edge) < 1e-6 && std::fabs(b[c * 2] - edge) < 1e-6);
				if (!border)
				{
					open[base[i] + f] |= e.linked;	// a seam to an interior keeps the top
					continue;
				}
				// a rim edge: its mirror on the other window joins the two faces (the other way round, the same points)
				const auto mirror = rimAt.find({ q(b[0]), q(b[2]), q(a[0]), q(a[2]) });
				if (mirror != rimAt.end())
				{
					unite(base[i] + f, mirror->second);
					open[base[i] + f] = open[mirror->second] = open[base[i] + f] || open[mirror->second];
				}
				else
					rimAt[{ q(a[0]), q(a[2]), q(b[0]), q(b[2]) }] = base[i] + f;
			}
		}
	}
	// a rim edge whose mirror never turned up (a window beyond, or the world's edge) leaves the face undecided: kept
	std::vector<uint8_t> rootOpen(base.back(), 0);
	for (const auto& [key, f] : rimAt)
	{
		bool matched = false;
		const auto mirror = rimAt.find({ key[2], key[3], key[0], key[1] });
		matched = mirror != rimAt.end();
		if (!matched)
			open[f] = 1;
	}
	for (std::size_t f = 0; f < open.size(); ++f)
		rootOpen[find(f)] |= open[f];
	std::vector<uint8_t> dead(world.faces.size(), 0);
	std::size_t dropped = 0;
	for (const std::uint32_t f : worldTops)
		if (!rootOpen[find(f)] && !dead[f])
		{
			dead[f] = 1;
			++dropped;
		}
	if (dropped == 0)
		return 0;
	native::Mesh out = world;
	out.faces.clear();
	out.edges.clear();
	std::vector<std::int32_t> newEdge(world.edges.size(), -1), newFace(world.faces.size(), -1);
	for (std::size_t f = 0; f < world.faces.size(); ++f)
	{
		if (dead[f])
			continue;
		native::Face face = world.faces[f];
		face.startEdge = (std::uint32_t)out.edges.size();
		newFace[f] = (std::int32_t)out.faces.size();
		for (std::uint32_t k = world.faces[f].startEdge; k < world.faces[f].startEdge + world.faces[f].numEdges; ++k)
		{
			newEdge[k] = (std::int32_t)out.edges.size();
			out.edges.push_back(world.edges[k]);
		}
		out.faces.push_back(face);
	}
	for (native::Edge& e : out.edges)
		if (e.oppositeEdge >= 0)
		{
			const std::int32_t to = newEdge[(std::size_t)e.oppositeEdge];
			e.oppositeFace = to >= 0 ? newFace[(std::size_t)e.oppositeFace] : -1;
			e.oppositeEdge = to;
		}
	world = std::move(out);
	return dropped;
}

WeldStats weldMesh(native::Mesh& m, double grid, double climb)
{
	WeldStats stats;
	const std::size_t nv = m.vertices.size();
	// (i) clusters (kenshi-ffmi.10): vertices within `grid` of each other in xz and `climb` in y join, transitively (a first-seen pick split a fan
	// apex's cluster: two corners 0.012 u apart ended 0.145 u apart, a 2.4 u2 overlap). A cluster sits at its root: a rim vertex (an end of a
	// `linked` edge: the window border and the interior seam stay put) if it has one, else its lowest index; two rim vertices join only along
	// one border line, and two clusters join only while their roots are within `grid` of each other (no chains).
	std::vector<std::uint32_t> rep(nv);
	for (std::size_t i = 0; i < nv; ++i)
		rep[i] = (std::uint32_t)i;
	std::vector<uint8_t> used(nv, 0), rim(nv, 0);
	for (const native::Edge& e : m.edges)
	{
		used[e.a] = used[e.b] = 1;
		if (e.linked)
			rim[e.a] = rim[e.b] = 1;
	}
	{
		std::vector<std::uint32_t> up(nv);
		for (std::size_t i = 0; i < nv; ++i)
			up[i] = (std::uint32_t)i;
		const std::function<std::uint32_t(std::uint32_t)> root = [&](std::uint32_t v) { while (up[v] != v) v = up[v] = up[up[v]]; return v; };
		const auto near = [&](std::uint32_t a, std::uint32_t b, double r)
		{
			const auto& p = m.vertices[a];
			const auto& q = m.vertices[b];
			return std::hypot(p[0] - q[0], p[2] - q[2]) <= r && std::fabs(p[1] - q[1]) <= climb;
		};
		// two clusters join only while their roots are within `reach` (so no corner moves farther than that)
		const auto join = [&](std::uint32_t a, std::uint32_t b, double reach)
		{
			const std::uint32_t ra = root(a), rb = root(b);
			if (ra == rb || !near(ra, rb, reach))
				return;
			// two rim vertices join only on one border line (one coordinate equal): the root stays on the border wherever the cluster sits
			const auto& pa = m.vertices[ra];
			const auto& pb = m.vertices[rb];
			if (rim[ra] && rim[rb] && pa[0] != pb[0] && pa[2] != pb[2])
				return;
			// the root: the rim vertex, else the lower index
			if (rim[rb] || (!rim[ra] && rb < ra))
				up[ra] = rb;
			else
				up[rb] = ra;
		};
		// opposite edges first: their ends are one vertex in nativeTiles (weldedVertices) and on the ground; linkTops keeps each side's own (up to
		// 0.125 u apart on surfgen's tops), and a pair whose ends stay apart is a sliver of overlap or gap along the whole edge
		for (const native::Edge& e : m.edges)
			if (e.oppositeEdge >= 0)
			{
				const native::Edge& o = m.edges[(std::size_t)e.oppositeEdge];
				if (near(e.a, o.b, grid))
					join(e.a, o.b, grid);
				if (near(e.b, o.a, grid))
					join(e.b, o.a, grid);
			}
		const double cell = grid;
		std::unordered_map<uint64_t, std::vector<std::uint32_t>> grid2;
		const auto cellOf = [&](double v) { return (int64_t)std::floor(v / cell); };
		for (std::size_t i = 0; i < nv; ++i)
			if (used[i])
				grid2[cellKeyOf(cellOf(m.vertices[i][0]), cellOf(m.vertices[i][2]))].push_back((std::uint32_t)i);
		for (std::size_t i = 0; i < nv; ++i)
		{
			if (!used[i])
				continue;
			const auto& v = m.vertices[i];
			const int64_t cx = cellOf(v[0]), cz = cellOf(v[2]);
			for (int64_t dx = -1; dx <= 1; ++dx)
				for (int64_t dz = -1; dz <= 1; ++dz)
				{
					const auto it = grid2.find(cellKeyOf(cx + dx, cz + dz));
					if (it == grid2.end())
						continue;
					for (const std::uint32_t o : it->second)
					{
						if (o > i && near((std::uint32_t)i, o, grid))
							join((std::uint32_t)i, o, grid);
					}
				}
		}
		for (std::size_t i = 0; i < nv; ++i)
			rep[i] = root((std::uint32_t)i);
	}
	// (i b) no face may come out of the merge worse than it went in: one that was convex (reflex depth under 0.05 u), wound one way and simple
	// must stay so, unless it collapses (the thin-face drop below takes it). A face that fails gets its moved corners back (rep = itself), and
	// the faces round them are checked again. Nothing re-checked after the first weld: 40 of 43 reflex tops on the Metal Arch Bridge were its moves.
	{
		const auto shape = [&](std::uint32_t f, bool welded, double& reflex, double& area2, bool& simple, double& perimeter, bool& crossing)
		{
			std::vector<std::uint32_t> ring;
			for (std::uint32_t k = m.faces[f].startEdge; k < m.faces[f].startEdge + m.faces[f].numEdges; ++k)
			{
				const std::uint32_t v = welded ? rep[m.edges[k].a] : m.edges[k].a;
				if (ring.empty() || ring.back() != v)
					ring.push_back(v);
			}
			while (ring.size() > 1 && ring.front() == ring.back())
				ring.pop_back();
			reflex = 0;
			area2 = 0;
			perimeter = 0;
			simple = true;
			crossing = false;
			const std::size_t n = ring.size();
			for (std::size_t k = 0; k < n; ++k)
			{
				const auto& p = m.vertices[ring[k]];
				const auto& q = m.vertices[ring[(k + 1) % n]];
				area2 += p[0] * q[2] - q[0] * p[2];
				perimeter += std::hypot(q[0] - p[0], q[2] - p[2]);
				for (std::size_t j = k + 1; j < n && simple; ++j)
					simple = ring[j] != ring[k];
			}
			// two non-adjacent edges crossing properly in xz (the fold the census counts as a self-crossing face)
			const auto orient = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c)
			{
				return (m.vertices[b][0] - m.vertices[a][0]) * (m.vertices[c][2] - m.vertices[a][2]) - (m.vertices[b][2] - m.vertices[a][2]) * (m.vertices[c][0] - m.vertices[a][0]);
			};
			for (std::size_t i = 0; i < n && !crossing; ++i)
				for (std::size_t j = i + 2; j < n && !crossing; ++j)
				{
					if (i == 0 && j == n - 1)
						continue;
					const std::uint32_t a = ring[i], b = ring[(i + 1) % n], c = ring[j], d = ring[(j + 1) % n];
					const double o1 = orient(a, b, c), o2 = orient(a, b, d), o3 = orient(c, d, a), o4 = orient(c, d, b);
					crossing = ((o1 > 1e-6 && o2 < -1e-6) || (o1 < -1e-6 && o2 > 1e-6)) && ((o3 > 1e-6 && o4 < -1e-6) || (o3 < -1e-6 && o4 > 1e-6));
				}
			const double s = area2 >= 0 ? 1 : -1;
			for (std::size_t k = 0; k < n && n >= 3; ++k)
			{
				const auto& a = m.vertices[ring[(k + n - 1) % n]];
				const auto& b = m.vertices[ring[k]];
				const auto& c = m.vertices[ring[(k + 1) % n]];
				const double len = std::hypot(c[0] - a[0], c[2] - a[2]);
				if (len < 1e-9)
					continue;
				const double turn = (b[0] - a[0]) * (c[2] - b[2]) - (b[2] - a[2]) * (c[0] - b[0]);	// > 0 with s > 0: a convex corner
				if (s * turn < 0)
					reflex = std::max(reflex, -s * turn / len);
			}
			return n;
		};
		std::vector<std::vector<std::uint32_t>> facesOf(nv);
		for (std::uint32_t f = 0; f < m.faces.size(); ++f)
			for (std::uint32_t k = m.faces[f].startEdge; k < m.faces[f].startEdge + m.faces[f].numEdges; ++k)
				facesOf[m.edges[k].a].push_back(f);
		struct Before
		{
			double reflex, area2;
			bool simple, crossing;
		};
		std::vector<Before> before(m.faces.size());
		std::vector<std::uint32_t> check;
		for (std::uint32_t f = 0; f < m.faces.size(); ++f)
		{
			double r, a, p;
			bool s, x;
			shape(f, false, r, a, s, p, x);
			before[f] = { r, a, s, x };
			check.push_back(f);
		}
		std::vector<uint8_t> queued(m.faces.size(), 1);
		while (!check.empty())
		{
			const std::uint32_t f = check.back();
			check.pop_back();
			queued[f] = 0;
			double r, a, p;
			bool s, x;
			const std::size_t n = shape(f, true, r, a, s, p, x);
			if (n < 3 || std::fabs(a) / 2 < p / 2 / 32)
				continue;	// collapsed: the thin-face drop's
			// 0.05 u: at 0.005 the given-back corners left Deadlands 189 open edge pairs; 0.05 keeps the worst merge-made reflex corner near 0.1 u
			const bool ok = (s || !before[f].simple) && (!x || before[f].crossing) && (a > 0) == (before[f].area2 > 0) && r <= std::max(0.05, before[f].reflex + 1e-9);
			if (ok)
				continue;
			++stats.refused;
			for (std::uint32_t k = m.faces[f].startEdge; k < m.faces[f].startEdge + m.faces[f].numEdges; ++k)
			{
				const std::uint32_t v = m.edges[k].a;
				if (rep[v] == v)
					continue;
				rep[v] = v;
				for (const std::uint32_t g : facesOf[v])
					if (!queued[g])
					{
						queued[g] = 1;
						check.push_back(g);
					}
			}
			if (!queued[f])
			{
				queued[f] = 1;
				check.push_back(f);
			}
		}
	}
	// (i c) vertices at one position are one (a corner given back above can sit exactly on another given back: no move, so no face changes)
	{
		std::map<std::array<double, 3>, std::uint32_t> at;
		for (std::size_t i = 0; i < nv; ++i)
			if (used[i] && rep[i] == i)
			{
				const auto [it, fresh] = at.try_emplace({ m.vertices[i][0], m.vertices[i][1], m.vertices[i][2] }, (std::uint32_t)i);
				if (!fresh)
					rep[i] = it->second;
			}
		for (std::size_t i = 0; i < nv; ++i)
			rep[i] = rep[rep[i]];
	}
	for (std::size_t i = 0; i < nv; ++i)
		stats.welded += used[i] && rep[i] != i;
	// the faces as loops of entries: the old edge each came from, -1 for a part made by a split
	struct Entry
	{
		std::uint32_t a, b;
		std::int32_t old;
		bool linked;
		std::uint8_t flags;
	};
	std::vector<std::vector<Entry>> loops(m.faces.size());
	for (std::size_t f = 0; f < m.faces.size(); ++f)
		for (std::uint32_t k = m.faces[f].startEdge; k < m.faces[f].startEdge + m.faces[f].numEdges; ++k)
		{
			const native::Edge& e = m.edges[k];
			if (rep[e.a] != rep[e.b])
				loops[f].push_back({ rep[e.a], rep[e.b], (std::int32_t)k, e.linked, e.flags });
		}
	// a face with no width left on the grid (area / half perimeter under a 32nd of a unit: collinear after the weld, or thinner than the tile's 1/8 u
	// quanta can keep apart) is dropped: nativeTiles could not place it, and its neighbours would stay open over its edges. Its neighbours' edges become
	// boundary edges and take part in the split and pair steps below.
	std::vector<std::int32_t> through(m.edges.size(), -1); // old edge -> the old edge it became the opposite of, across a dropped strip
	std::vector<std::int32_t> faceOfEdge(m.edges.size(), -1);
	for (std::size_t f = 0; f < m.faces.size(); ++f)
		for (std::uint32_t k = m.faces[f].startEdge; k < m.faces[f].startEdge + m.faces[f].numEdges; ++k)
			faceOfEdge[k] = (std::int32_t)f;
	std::vector<std::int32_t> droppedOpp; // old edges whose opposite went with a dropped face
	for (std::size_t f = 0; f < loops.size(); ++f)
	{
		double area = 0, perimeter = 0;
		for (const Entry& en : loops[f])
		{
			const auto& p = m.vertices[en.a];
			const auto& q = m.vertices[en.b];
			area += p[0] * q[2] - q[0] * p[2];
			perimeter += std::hypot(q[0] - p[0], q[2] - p[2]);
		}
		if (loops[f].size() >= 3 && std::fabs(area) / 2 >= perimeter / 2 / 32)
			continue;
		std::size_t live = 0;
		// the keep rule: two edges of the dropped face that run over the same two vertices, opposite ways (a collapsed strip: a digon, or a
		// triangle folded on itself) are one weld of their two neighbours, which become each other's opposite
		for (std::size_t i = 0; i < loops[f].size(); ++i)
		{
			const Entry& ei = loops[f][i];
			if (ei.old < 0 || m.edges[ei.old].oppositeEdge < 0 || through[(std::size_t)m.edges[ei.old].oppositeEdge] >= 0)
				continue;
			for (std::size_t j = i + 1; j < loops[f].size(); ++j)
			{
				const Entry& ej = loops[f][j];
				if (ej.old < 0 || m.edges[ej.old].oppositeEdge < 0 || ej.a != ei.b || ej.b != ei.a
					|| through[(std::size_t)m.edges[ej.old].oppositeEdge] >= 0)
					continue;
				const std::int32_t oi = m.edges[ei.old].oppositeEdge, oj = m.edges[ej.old].oppositeEdge;
				through[(std::size_t)oi] = oj;
				through[(std::size_t)oj] = oi;
				++stats.through;
				break;
			}
		}
		for (const Entry& en : loops[f])
			if (en.old >= 0 && m.edges[en.old].oppositeEdge >= 0)
			{
				droppedOpp.push_back(m.edges[en.old].oppositeEdge);
				++live;
			}
		++stats.byLive[std::min<std::size_t>(live, 3)];
		loops[f].clear();
		++stats.dropped;
	}
	std::vector<uint8_t> orphan(m.edges.size(), 0);
	for (const std::int32_t e : droppedOpp)
		orphan[(std::size_t)e] = 1;
	for (std::size_t e = 0; e < through.size(); ++e)
		if (through[e] >= 0)
			orphan[e] = 0;
	// (ii) vertices on boundary edges
	struct Ref
	{
		std::uint32_t face, at;
	};
	std::unordered_map<uint64_t, std::vector<Ref>> byCell;
	constexpr double cell = 4;
	const auto isBoundary = [&](const Entry& en) { return !en.linked && (en.old < 0 || m.edges[en.old].oppositeEdge < 0 || orphan[(std::size_t)en.old]); };
	for (std::uint32_t f = 0; f < loops.size(); ++f)
		for (std::uint32_t k = 0; k < loops[f].size(); ++k)
		{
			const Entry& en = loops[f][k];
			if (!isBoundary(en))
				continue;
			const auto& p = m.vertices[en.a];
			const auto& q = m.vertices[en.b];
			for (int64_t cx = (int64_t)std::floor((std::min(p[0], q[0]) - 2 * grid) / cell); cx <= (int64_t)std::floor((std::max(p[0], q[0]) + 2 * grid) / cell); ++cx)
				for (int64_t cz = (int64_t)std::floor((std::min(p[2], q[2]) - 2 * grid) / cell); cz <= (int64_t)std::floor((std::max(p[2], q[2]) + 2 * grid) / cell); ++cz)
					byCell[cellKeyOf(cx, cz)].push_back({ f, k });
		}
	std::vector<std::vector<std::pair<double, std::uint32_t>>> cuts(0);
	std::unordered_map<uint64_t, std::vector<std::pair<double, std::uint32_t>>> cutsOf; // (face, entry) -> (t, vertex)
	std::vector<uint8_t> done(nv, 0);
	std::unordered_set<uint64_t> insertedIn;	// (face, vertex)
	for (std::size_t i = 0; i < nv; ++i)
	{
		if (rep[i] != i || done[i] || !used[i])
			continue;
		const auto& v = m.vertices[i];
		const auto it = byCell.find(cellKeyOf((int64_t)std::floor(v[0] / cell), (int64_t)std::floor(v[2] / cell)));
		if (it == byCell.end())
			continue;
		for (const Ref& r : it->second)
		{
			const Entry& en = loops[r.face][r.at];
			if (en.a == i || en.b == i)
				continue;
			const auto& p = m.vertices[en.a];
			const auto& q = m.vertices[en.b];
			const double dx = q[0] - p[0], dz = q[2] - p[2], l2 = dx * dx + dz * dz;
			if (l2 < 1e-12)
				continue;
			const double t = ((v[0] - p[0]) * dx + (v[2] - p[2]) * dz) / l2;
			if (t <= 0 || t >= 1)
				continue;
			const double d = std::hypot(p[0] + t * dx - v[0], p[2] + t * dz - v[2]);
			if (d > grid || std::hypot(v[0] - p[0], v[2] - p[2]) <= grid || std::hypot(v[0] - q[0], v[2] - q[2]) <= grid
				|| std::fabs(p[1] + t * (q[1] - p[1]) - v[1]) > climb)
				continue;
			bool own = false;
			for (const Entry& other : loops[r.face])
				own = own || other.a == i;
			if (own)
				continue;
			// once per face (two edges of one strip: a pinch). A vertex inside the face is still a cut (refusing those left the bridge open edge
			// pairs for 26 fewer Deadlands corners over 0.01 u)
			if (!insertedIn.insert(((uint64_t)r.face << 32) | (std::uint32_t)i).second)
				continue;
			cutsOf[((uint64_t)r.face << 32) | r.at].push_back({ t, (std::uint32_t)i });
		}
	}
	for (auto& [k, list] : cutsOf)
	{
		std::sort(list.begin(), list.end());
		list.erase(std::unique(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second == y.second; }), list.end());
		stats.split += list.size();
	}
	// the new faces, and the old edge -> new edge map for the opposites that stay
	native::Mesh out = m;
	out.faces.clear();
	out.edges.clear();
	std::vector<std::int32_t> newOf(m.edges.size(), -1);
	std::vector<std::uint32_t> faceOfOld(m.edges.size(), 0);
	std::vector<std::int32_t> faceIndex(m.faces.size(), -1);
	for (std::uint32_t f = 0; f < loops.size(); ++f)
	{
		std::vector<Entry> list;
		for (std::uint32_t k = 0; k < loops[f].size(); ++k)
		{
			const Entry& en = loops[f][k];
			const auto it = cutsOf.find(((uint64_t)f << 32) | k);
			if (it == cutsOf.end())
			{
				list.push_back(en);
				continue;
			}
			std::uint32_t from = en.a;
			for (const auto& [t, v] : it->second)
			{
				list.push_back({ from, v, -1, false, en.flags });
				from = v;
			}
			list.push_back({ from, en.b, -1, false, en.flags });
		}
		// a cut that makes the face cross itself (a vertex beside a thin lobe): the face keeps its uncut edges
		if (list.size() != loops[f].size())
		{
			const auto crosses = [&](const std::vector<Entry>& ring)
			{
				const auto orient = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c)
				{
					return (m.vertices[b][0] - m.vertices[a][0]) * (m.vertices[c][2] - m.vertices[a][2]) - (m.vertices[b][2] - m.vertices[a][2]) * (m.vertices[c][0] - m.vertices[a][0]);
				};
				const std::size_t n = ring.size();
				for (std::size_t i = 0; i < n; ++i)
					for (std::size_t j = i + 2; j < n; ++j)
					{
						if (i == 0 && j == n - 1)
							continue;
						const double o1 = orient(ring[i].a, ring[i].b, ring[j].a), o2 = orient(ring[i].a, ring[i].b, ring[j].b);
						const double o3 = orient(ring[j].a, ring[j].b, ring[i].a), o4 = orient(ring[j].a, ring[j].b, ring[i].b);
						if (((o1 > 1e-6 && o2 < -1e-6) || (o1 < -1e-6 && o2 > 1e-6)) && ((o3 > 1e-6 && o4 < -1e-6) || (o3 < -1e-6 && o4 > 1e-6)))
							return true;
					}
				return false;
			};
			if (crosses(list) && !crosses(loops[f]))
			{
				list = loops[f];
				++stats.cutRefused;
			}
		}
		if (list.size() < 3)
			continue;
		native::Face face = m.faces[f];
		face.startEdge = (std::uint32_t)out.edges.size();
		face.numEdges = (std::uint16_t)list.size();
		faceIndex[f] = (std::int32_t)out.faces.size();
		for (const Entry& en : list)
		{
			native::Edge e;
			e.a = en.a;
			e.b = en.b;
			e.linked = en.linked;
			e.flags = en.flags;
			if (en.old >= 0)
			{
				newOf[en.old] = (std::int32_t)out.edges.size();
				if (through[(std::size_t)en.old] >= 0)
				{
					e.oppositeEdge = through[(std::size_t)en.old];
					e.oppositeFace = faceOfEdge[(std::size_t)e.oppositeEdge];
				}
				else if (!orphan[(std::size_t)en.old])
				{
					e.oppositeEdge = m.edges[en.old].oppositeEdge;
					e.oppositeFace = m.edges[en.old].oppositeFace;
				}
			}
			out.edges.push_back(e);
		}
		out.faces.push_back(face);
	}
	for (native::Edge& e : out.edges)
	{
		if (e.oppositeEdge < 0)
			continue;
		const std::int32_t to = newOf[(std::size_t)e.oppositeEdge];
		const std::int32_t toFace = e.oppositeFace >= 0 ? faceIndex[(std::size_t)e.oppositeFace] : -1;
		e.oppositeEdge = to;
		e.oppositeFace = to >= 0 ? toFace : -1;
	}
	// pair the boundary edges that now run over the same two vertices, opposite ways
	std::unordered_map<uint64_t, std::int32_t> at;
	std::vector<std::int32_t> faceOfNew(out.edges.size());
	for (std::size_t f = 0; f < out.faces.size(); ++f)
		for (std::uint32_t k = out.faces[f].startEdge; k < out.faces[f].startEdge + out.faces[f].numEdges; ++k)
			faceOfNew[k] = (std::int32_t)f;
	for (std::size_t k = 0; k < out.edges.size(); ++k)
	{
		native::Edge& e = out.edges[k];
		if (e.oppositeEdge >= 0 || e.linked)
			continue;
		const auto o = at.find(((uint64_t)e.b << 32) | e.a);
		if (o != at.end() && out.edges[(std::size_t)o->second].oppositeEdge < 0)
		{
			native::Edge& other = out.edges[(std::size_t)o->second];
			e.oppositeEdge = o->second;
			e.oppositeFace = faceOfNew[(std::size_t)o->second];
			other.oppositeEdge = (std::int32_t)k;
			other.oppositeFace = faceOfNew[k];
			++stats.paired;
		} else {
			at[((uint64_t)e.a << 32) | e.b] = (std::int32_t)k;
		}
	}
	// and those whose ends are apart by no more than the merge reach (a corner the shape check gave back, or two vertices the reach kept apart): a
	// declared pair, no vertex moved (nativeTiles' weldedVertices joins the ends within its own 0.25 u)
	{
		std::unordered_map<uint64_t, std::vector<std::int32_t>> byMid;
		const auto midCell = [&](const native::Edge& e)
		{
			const auto& p = out.vertices[e.a];
			const auto& q = out.vertices[e.b];
			return std::pair<std::int64_t, std::int64_t>{ (std::int64_t)std::floor((p[0] + q[0]) / 2 / 4), (std::int64_t)std::floor((p[2] + q[2]) / 2 / 4) };
		};
		for (std::size_t k = 0; k < out.edges.size(); ++k)
			if (out.edges[k].oppositeEdge < 0 && !out.edges[k].linked)
			{
				const auto [cx, cz] = midCell(out.edges[k]);
				byMid[cellKeyOf(cx, cz)].push_back((std::int32_t)k);
			}
		const auto close = [&](std::uint32_t a, std::uint32_t b)
		{
			const auto& p = out.vertices[a];
			const auto& q = out.vertices[b];
			return std::hypot(p[0] - q[0], p[2] - q[2]) <= grid && std::fabs(p[1] - q[1]) <= climb;
		};
		for (std::size_t k = 0; k < out.edges.size(); ++k)
		{
			native::Edge& e = out.edges[k];
			if (e.oppositeEdge >= 0 || e.linked || close(e.a, e.b))
				continue;
			const auto [cx, cz] = midCell(e);
			std::int32_t best = -1;
			double bestD = 1e300;
			for (std::int64_t dx = -1; dx <= 1; ++dx)
				for (std::int64_t dz = -1; dz <= 1; ++dz)
				{
					const auto it = byMid.find(cellKeyOf(cx + dx, cz + dz));
					if (it == byMid.end())
						continue;
					for (const std::int32_t j : it->second)
					{
						const native::Edge& o = out.edges[(std::size_t)j];
						if ((std::size_t)j == k || o.oppositeEdge >= 0 || faceOfNew[(std::size_t)j] == faceOfNew[k] || !close(e.a, o.b) || !close(e.b, o.a))
							continue;
						const double d = std::hypot(out.vertices[e.a][0] - out.vertices[o.b][0], out.vertices[e.a][2] - out.vertices[o.b][2])
							+ std::hypot(out.vertices[e.b][0] - out.vertices[o.a][0], out.vertices[e.b][2] - out.vertices[o.a][2]);
						if (d < bestD)
						{
							bestD = d;
							best = j;
						}
					}
				}
			if (best < 0)
				continue;
			native::Edge& other = out.edges[(std::size_t)best];
			e.oppositeEdge = best;
			e.oppositeFace = faceOfNew[(std::size_t)best];
			other.oppositeEdge = (std::int32_t)k;
			other.oppositeFace = faceOfNew[k];
			++stats.declared;
		}
	}
	(void)faceOfOld;
	std::unordered_map<uint64_t, std::vector<std::int32_t>> openCells; // the unpaired edges by the 4 u cells their endpoints are in
	for (std::size_t k = 0; k < out.edges.size(); ++k)
		if (out.edges[k].oppositeEdge < 0 && !out.edges[k].linked)
			for (const std::uint32_t v : { out.edges[k].a, out.edges[k].b })
			{
				auto& list = openCells[cellKeyOf((std::int64_t)std::floor(out.vertices[v][0] / 4), (std::int64_t)std::floor(out.vertices[v][2] / 4))];
				if (list.empty() || list.back() != (std::int32_t)k)
					list.push_back((std::int32_t)k);
			}
	for (std::size_t f = 0; f < loops.size(); ++f)
		for (std::uint32_t k = 0; k < loops[f].size(); ++k)
		{
			const Entry& en = loops[f][k];
			if (en.old < 0 || !orphan[(std::size_t)en.old] || newOf[(std::size_t)en.old] < 0)
				continue;
			const native::Edge& e = out.edges[(std::size_t)newOf[(std::size_t)en.old]];
			if (e.oppositeEdge >= 0)
				continue;
			++stats.orphanOpen;
			{
				// an unpaired edge running the other way along the same line (within 1/8 u, a climb in y), overlapping this one: a gap the weld left
				const auto& p = out.vertices[e.a];
				const auto& q = out.vertices[e.b];
				const double dx = q[0] - p[0], dz = q[2] - p[2], l = std::hypot(dx, dz);
				bool partner = false;
				const std::int64_t cx0 = (std::int64_t)std::floor((std::min(p[0], q[0]) - 0.25) / 4), cx1 = (std::int64_t)std::floor((std::max(p[0], q[0]) + 0.25) / 4);
				const std::int64_t cz0 = (std::int64_t)std::floor((std::min(p[2], q[2]) - 0.25) / 4), cz1 = (std::int64_t)std::floor((std::max(p[2], q[2]) + 0.25) / 4);
				for (std::int64_t cx = cx0; cx <= cx1 && !partner; ++cx)
				for (std::int64_t cz = cz0; cz <= cz1 && !partner; ++cz)
				{
				const auto cell = openCells.find(cellKeyOf(cx, cz));
				if (cell == openCells.end())
					continue;
				for (const std::int32_t j : cell->second)
				{
					const native::Edge& o = out.edges[(std::size_t)j];
					if (partner || &o == &e)
						continue;
					const auto& a2 = out.vertices[o.a];
					const auto& b2 = out.vertices[o.b];
					const auto along = [&](const std::array<double, 3>& v) { return ((v[0] - p[0]) * dx + (v[2] - p[2]) * dz) / l; };
					const auto off = [&](const std::array<double, 3>& v) { return std::fabs((v[0] - p[0]) * dz - (v[2] - p[2]) * dx) / l; };
					const double s0 = along(a2), s1 = along(b2);
					if (s1 < s0 && off(a2) < 0.125 && off(b2) < 0.125 && std::min(s0, l) - std::max(s1, 0.0) > 0.05 && std::fabs(a2[1] - p[1]) < 5)
						partner = true;
				}
				}
				if (partner)
					++stats.orphanGap;
			}
		}
	m = std::move(out);
	return stats;
}

std::size_t dropCollinearWallVertices(native::Mesh& m, double tol, double tolY)
{
	std::vector<std::uint16_t> holders(m.vertices.size(), 0);
	for (const native::Edge& e : m.edges)
		++holders[e.a];	// every face has one edge starting at each of its vertices
	std::vector<std::uint8_t> drop(m.edges.size(), 0);	// the edge ending at a dropped vertex
	std::size_t dropped = 0;
	for (const native::Face& f : m.faces)
	{
		if (f.top)	// a top's drop opened shared edges in knav_cutover (+1 / +6 / +6 on its three zones): the tops keep theirs (their link matcher reads the outline)
			continue;
		std::uint32_t keep = f.numEdges;
		for (std::uint32_t k = 0; k < f.numEdges && keep > 3; ++k)
		{
			const std::uint32_t e1 = f.startEdge + k, e2 = f.startEdge + (k + 1) % f.numEdges;
			const std::uint32_t pe = f.startEdge + (k + f.numEdges - 1) % f.numEdges;
			if (!m.wall(e1) || !m.wall(e2) || drop[e1] || drop[e2] || drop[pe] || holders[m.edges[e2].a] != 1)
				continue;
			const auto& a = m.vertices[m.edges[e1].a], & b = m.vertices[m.edges[e2].a], & c = m.vertices[m.edges[e2].b];
			const double len = std::hypot(c[0] - a[0], c[2] - a[2]);
			if (len < 1e-9)
				continue;
			const double dx = (c[0] - a[0]) / len, dz = (c[2] - a[2]) / len;
			const double t = (b[0] - a[0]) * dx + (b[2] - a[2]) * dz;
			if (t <= 0 || t >= len || std::fabs((b[0] - a[0]) * dz - (b[2] - a[2]) * dx) > tol || std::fabs(b[1] - (a[1] + (c[1] - a[1]) * t / len)) > tolY)
				continue;
			drop[e1] = 1;	// e1 (a -> b) is merged with e2 (b -> c): the merged edge is e2's slot, starting at a
			--keep;
			++dropped;
		}
	}
	if (dropped == 0)
		return 0;
	native::Mesh out = m;
	out.faces.clear();
	out.edges.clear();
	std::vector<std::int32_t> remap(m.edges.size(), -1);
	for (const native::Face& f : m.faces)
	{
		native::Face nf = f;
		nf.startEdge = (std::uint32_t)out.edges.size();
		nf.numEdges = 0;
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
		{
			const std::uint32_t e = f.startEdge + k;
			if (drop[e])
				continue;
			remap[e] = (std::int32_t)out.edges.size();
			native::Edge ne = m.edges[e];
			const std::uint32_t pe = f.startEdge + (k + f.numEdges - 1) % f.numEdges;
			if (drop[pe])
				ne.a = m.edges[pe].a;	// starts where the dropped edge did
			out.edges.push_back(ne);
			++nf.numEdges;
		}
		out.faces.push_back(nf);
	}
	for (native::Edge& e : out.edges)
		if (e.oppositeEdge >= 0)
			e.oppositeEdge = remap[e.oppositeEdge];
	m = std::move(out);
	return dropped;
}

Material materialOfLabel(const std::string& label)
{
	return label == "foliage" ? Material::Foliage : label.rfind("feature @", 0) == 0 ? Material::Feature : Material::Building;
}

std::vector<Material> wallMaterials(const native::Mesh& m, const std::vector<Cutter>& cutters, const Params& params, std::vector<int>* owner)
{
	std::vector<PreparedCutter> pcs;
	for (const Cutter& c : cutters)
		pcs.push_back(prepare(c));
	std::vector<Material> out(m.edges.size(), Material::Terrain);
	if (owner)
		owner->assign(m.edges.size(), -1);
	std::vector<double> hits;
	std::vector<int> near;
	for (const native::Face& f : m.faces)
	{
		P2 mid = { 0, 0 };
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
		{
			const auto& v = m.vertices[m.edges[f.startEdge + k].a];
			mid[0] += v[0] / f.numEdges;
			mid[1] += v[2] / f.numEdges;
		}
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
		{
			const std::uint32_t e = f.startEdge + k;
			if (!m.wall(e))
				continue;
			const auto& A = m.vertices[m.edges[e].a], & B = m.vertices[m.edges[e].b];
			const double dx = B[0] - A[0], dz = B[2] - A[2], len = std::hypot(dx, dz);
			if (len < 1e-9)
				continue;
			double nx = -dz / len, nz = dx / len;
			const P2 c = { (A[0] + B[0]) / 2, (A[2] + B[2]) / 2 };
			if (nx * (mid[0] - c[0]) + nz * (mid[1] - c[1]) > 0)
			{
				nx = -nx;
				nz = -nz;
			}
			const double y = (A[1] + B[1]) / 2;
			for (const PreparedCutter& pc : pcs)
			{
				if (pc.src->prebuilt && !f.top)
					continue;
				const bool topHit = f.top;
				const P2 q = topHit ? P2{ c[0] - 0.05 * nx, c[1] - 0.05 * nz } : P2{ c[0] + 0.05 * nx, c[1] + 0.05 * nz };
				if (!pc.box.contains(q) || (topHit && pc.src->top == Cutter::Top::None))
					continue;
				hitsAt(pc, q, hits, near);
				bool hit = false;
				if (topHit)
					for (const double h : hits)
						hit = hit || std::fabs(h - y) < 1.0;
				else
					hit = cutterDrops(pc, q, y, params.cutBase, params.characterHeight, hits);
				if (hit)
				{
					out[e] = materialOfLabel(pc.src->label);
					if (owner)
						(*owner)[e] = (int)(&pc - pcs.data());
					break;
				}
			}
		}
	}
	return out;
}

Cutter zoneSideCutter(const native::Mesh& m, const std::string& label)
{
	Cutter c;
	c.top = Cutter::Top::Zone;
	c.prebuilt = true;
	c.label = label;
	c.min = { 1e300, 1e300, 1e300 };
	c.max = { -1e300, -1e300, -1e300 };
	for (const native::Face& f : m.faces)
	{
		for (std::uint32_t k = 1; k + 1 < f.numEdges; ++k)
		{
			std::array<std::array<double, 3>, 3> t = { m.vertices[m.edges[f.startEdge].a], m.vertices[m.edges[f.startEdge + k].a], m.vertices[m.edges[f.startEdge + k + 1].a] };
			if (cross2(t[0], t[1], t[2]) > 0)	// up-facing is cross2 < 0 (tops())
				std::swap(t[1], t[2]);
			for (const auto& p : t)
			{
				c.tris.push_back(p);
				for (int i = 0; i < 3; ++i)
				{
					c.min[i] = std::min(c.min[i], p[i]);
					c.max[i] = std::max(c.max[i], p[i]);
				}
			}
		}
		for (std::uint32_t k = 0; k < f.numEdges; ++k)
		{
			const native::Edge& e = m.edges[f.startEdge + k];
			if (e.linked && e.oppositeEdge < 0)
				c.seam.push_back({ m.vertices[e.a], m.vertices[e.b] });
		}
	}
	return c;
}

Grid makeGrid(const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1, int stride)
{
	// the original's zone height grid: fullmap.tif every `stride` samples (2: 129 x 129 per zone, 36 u; docs/reference/FOLIAGE_EXE.md)
	const int k = std::max(1, stride);
	const double s = kworld::kHeightmapSpacing * k;
	const int lastX = (terrain.sampleCountX() - 1) / k, lastZ = (terrain.sampleCountZ() - 1) / k;
	const int c0 = std::max(0, static_cast<int>(std::floor((x0 - kworld::kWorldMin) / s))), r0 = std::max(0, static_cast<int>(std::floor((z0 - kworld::kWorldMin) / s)));
	const int c1 = std::min(lastX, static_cast<int>(std::ceil((x1 - kworld::kWorldMin) / s)));
	const int r1 = std::min(lastZ, static_cast<int>(std::ceil((z1 - kworld::kWorldMin) / s)));
	Grid g;
	g.nx = c1 - c0 + 1;
	g.nz = r1 - r0 + 1;
	g.x0 = kworld::kWorldMin + c0 * s;
	g.z0 = kworld::kWorldMin + r0 * s;
	g.step = s;
	g.y.resize(static_cast<std::size_t>(g.nx) * g.nz);
	for (int j = 0; j < g.nz; ++j)
		for (int i = 0; i < g.nx; ++i)
			g.y[static_cast<std::size_t>(j) * g.nx + i] = terrain.sampleHeight((c0 + i) * k, (r0 + j) * k);
	return g;
}

std::vector<Cutter> gatherCutters(const std::string& dataDir, const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1,
	CutterCounts* counts)
{
	const auto clampCell = [](kworld::ZoneCell c)
	{
		c.x = std::clamp(c.x, 0, kworld::kZoneCellsPerAxis - 1);
		c.z = std::clamp(c.z, 0, kworld::kZoneCellsPerAxis - 1);
		return c;
	};
	const kworld::AreaGeometry area = kworld::gatherArea(dataDir, terrain, clampCell(kworld::zoneCellOf(x0, z0)), clampCell(kworld::zoneCellOf(x1, z1)));
	return cuttersOfArea(area, terrain, x0, z0, x1, z1, counts);
}

std::vector<Cutter> cuttersOfArea(const kworld::AreaGeometry& area, const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1,
	CutterCounts* counts)
{
	CutterCounts n;
	std::vector<Cutter> cutters;
	// Walkable foliage whose every vertex is within the climb of the terrain under it is the ground's: no cut, no tops (kenshi-ffmi.14,
	// TreeFall01 at -77686, -83856: a slab 1-3 u over the terrain, walked straight across in the original; the shipped mesh there has
	// no hole). Taken from the full-detail terrain, not the window's grid, so every window decides alike.
	constexpr double kLowWalkable = 5;	// the original's climb (HAVOK_NAV.md)
	const auto lowWalkable = [&](const kworld::WorldShape& s)
	{
		if (!s.foliage || s.use != kworld::NavUse::Walkable)
			return false;
		for (std::size_t i = 0; i + 2 < s.vertices.size(); i += 3)
			if (s.vertices[i + 1] - terrain.terrainHeight(s.vertices[i], s.vertices[i + 2]) > kLowWalkable)
				return false;
		return true;
	};
	const auto add = [&](const kworld::WorldShape& s, std::size_t& count, Cutter::Top top, const std::string& label)
	{
		if (s.use == kworld::NavUse::Door)
		{
			++n.doorsSkipped;
			return;
		}
		if (s.max[0] < x0 - 1 || s.min[0] > x1 + 1 || s.max[2] < z0 - 1 || s.min[2] > z1 + 1 || s.indices.size() < 3)
			return;
		if (lowWalkable(s))
		{
			++n.lowWalkable;
			return;
		}
		Cutter c;
		c.projected = s.use == kworld::NavUse::Projected;
		c.top = s.use == kworld::NavUse::Walkable ? top : Cutter::Top::None;
		c.label = label;
		c.min = { s.min[0], s.min[1], s.min[2] };
		c.max = { s.max[0], s.max[1], s.max[2] };
		for (const std::uint32_t i : s.indices)
			c.tris.push_back({ s.vertices[i * 3], s.vertices[i * 3 + 1], s.vertices[i * 3 + 2] });
		if ((s.solid || s.closed) && c.top != Cutter::Top::None)
		{
			// a convex solid's triangles wound outward, as surfgen reads them (an open mesh keeps the cooker's winding)
			std::array<double, 3> m{};
			for (const auto& p : c.tris)
				for (int k = 0; k < 3; ++k)
					m[k] += p[k] / c.tris.size();
			for (std::size_t t = 0; t + 2 < c.tris.size(); t += 3)
			{
				const auto& a = c.tris[t], & b = c.tris[t + 1], & d = c.tris[t + 2];
				const std::array<double, 3> u = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
				const std::array<double, 3> n = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
				if (n[0] * (a[0] - m[0]) + n[1] * (a[1] - m[1]) + n[2] * (a[2] - m[2]) < 0)
					std::swap(c.tris[t + 1], c.tris[t + 2]);
			}
		}
		cutters.push_back(std::move(c));
		++count;
	};
	for (const kworld::AreaBuilding& b : area.buildings)
	{
		if (b.shapes)
			for (const kworld::WorldShape& s : *b.shapes)
				add(s, n.buildingShapes, b.mask ? Cutter::Top::None : Cutter::Top::Zone, b.id);	// a masked building's tops are surfgen's (sections 0-6)
		if (b.mask)
			for (const kworld::WorldShape& s : *b.mask)
				add(s, n.maskShapes, Cutter::Top::None, b.id);
	}
	if (area.foliage)
		for (const kworld::WorldShape& s : *area.foliage)
			add(s, n.foliage, Cutter::Top::Zone, "foliage");
	if (area.features)
		for (const kworld::WorldShape& s : *area.features)
			add(s, n.features, Cutter::Top::Zone, "feature @ " + std::to_string((int)((s.min[0] + s.max[0]) / 2)) + ", " + std::to_string((int)((s.min[2] + s.max[2]) / 2)));
	std::stable_sort(cutters.begin(), cutters.end(), [](const Cutter& a, const Cutter& b)
	{
		return (a.max[0] - a.min[0]) * (a.max[2] - a.min[2]) > (b.max[0] - b.min[0]) * (b.max[2] - b.min[2]);
	});
	if (counts)
		*counts = n;
	return cutters;
}

native::Mesh generateBox(const std::string& dataDir, const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1, const Params& params,
	const std::vector<std::array<double, 2>>& extraSeeds, bool cut, Stats* stats, CutterCounts* counts, double* msGather,
	const std::vector<Cutter>* extra)
{
	const Grid g = makeGrid(terrain, x0, z0, x1, z1, params.stride);
	std::vector<std::array<double, 2>> seeds = extraSeeds;
	const double s = g.step;
	for (const double fx : { 1.0 / 6, 0.5, 5.0 / 6 })
		for (const double fz : { 1.0 / 6, 0.5, 5.0 / 6 })
			seeds.push_back({ g.x0 + fx * (g.nx - 1) * s, g.z0 + fz * (g.nz - 1) * s });
	const auto tg = std::chrono::steady_clock::now();
	std::vector<Cutter> cutters = cut ? gatherCutters(dataDir, terrain, g.x0, g.z0, g.x0 + (g.nx - 1) * s, g.z0 + (g.nz - 1) * s, counts)
		: std::vector<Cutter>{};
	if (extra)
		cutters.insert(cutters.end(), extra->begin(), extra->end());
	if (msGather)
		*msGather = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tg).count();
	return generate(g, seeds, cutters, params, stats);
}

} // namespace knav::ground
