#include "knav/ground.hpp"

#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <tuple>
#include <unordered_map>

namespace knav::ground
{

namespace
{

using P3 = std::array<double, 3>;

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
	// xz cells of 32 u -> triangles whose box touches the cell, built on first use (the deck test's vertical queries)
	mutable std::unordered_map<uint64_t, std::vector<int>> cells;
	mutable bool indexed = false;
};

constexpr double kIndexCell = 32;
uint64_t cellKeyOf(int64_t cx, int64_t cz)
{
	return ((uint64_t)(uint32_t)cx << 32) | (uint32_t)cz;
}

// The cutter's surface heights straight over / under q, by the cell index.
// Each hit with whether it faces up (the cooker's winding: cross2 < 0 in the generator's order, CutTri::sign < 0).
void hitsIndexed(const PreparedCutter& pc, const P2& q, std::vector<std::pair<double, bool>>& hits)
{
	if (!pc.indexed)
	{
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
	const auto it = pc.cells.find(cellKeyOf((int64_t)std::floor(q[0] / kIndexCell), (int64_t)std::floor(q[1] / kIndexCell)));
	if (it == pc.cells.end())
		return;
	for (const int k : it->second)
	{
		const CutTri& t = pc.tris[k];
		if (!t.box.contains(q))
			continue;
		bool inside = true;
		for (int i = 0; i < 3 && inside; ++i)
			inside = t.sign * t.edge[i].at(q) >= -1e-9;
		if (inside)
			hits.push_back({ t.A * q[0] + t.B * q[1] + t.C, t.sign < 0 });
	}
}

PreparedCutter prepare(const Cutter& c)
{
	PreparedCutter pc;
	pc.src = &c;
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
void hitsAt(const PreparedCutter& pc, const P2& q, std::vector<double>& hits)
{
	hits.clear();
	for (const CutTri& t : pc.tris)
	{
		if (!t.flat || !t.box.contains(q))
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
	native::Mesh finish(const std::vector<std::array<double, 2>>& seeds, Stats& st);

private:
	struct Plan
	{
		double cost = 0;
		std::vector<int> ring;
		std::vector<std::array<int, 3>> tris;
	};

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
	std::unordered_map<uint64_t, std::vector<int>> topPos_;	// vertex hash for the tops (xz at 1e-3, searched 3 x 3, y within 1e-3)

	struct TerrTri
	{
		P3 v[3];
		double A, B, C;
		Line edge[3];
	};
	void terrTris(int i, int j, TerrTri out[2]) const;
	double terrainAt(double x, double z) const;
	int topVertex(double x, double y, double z);
	double clearUnder(const P2& q, double y) const;
	void linkTops(std::vector<std::vector<int>>& poly, std::vector<uint8_t>& dead, std::vector<uint8_t>& isTop, std::unordered_map<uint64_t, int>& owner,
		std::vector<Link>& links, Stats& st);
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
			// the diagonal with the smaller height step follows ridges and valleys
			const bool ad = std::fabs(P_[a][1] - P_[d][1]) <= std::fabs(P_[b][1] - P_[c][1]);
			const std::array<std::array<int, 3>, 2> two = ad ? std::array<std::array<int, 3>, 2>{ { { a, b, d }, { a, d, c } } }
				: std::array<std::array<int, 3>, 2>{ { { a, b, c }, { b, d, c } } };
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

// Cuts `pieces` (convex xz polygons lying on the plane y = gA x + gB z + gC) by one cutter: split along its triangles' xz edges and the lines where
// its planes cross the plane + lo / + hi, each piece dropped by its centre (the span rule; `projected`: any hit). `skipSelf` >= 0: hits within it of
// the plane do not count (a top face does not cut itself). Returns the number of pieces dropped.
std::size_t cutByCutter(const PreparedCutter& pc, std::vector<std::vector<P2>>& pieces, double gA, double gB, double gC, double lo, double hi,
	double skipSelf)
{
	std::size_t dropped = 0;
	std::vector<double> hits;
	std::vector<P2> neg, pos;
	std::vector<std::vector<P2>> next;
	for (std::vector<P2>& piece : pieces)
	{
		const Box2 pbox = boxOf(piece);
		if (!pc.box.overlaps(pbox))
		{
			next.push_back(std::move(piece));
			continue;
		}
		std::vector<std::vector<P2>> work = { std::move(piece) };
		const auto splitAll = [&](const Line& L, const Box2& within)
		{
			std::vector<std::vector<P2>> out;
			for (std::vector<P2>& w : work)
			{
				if (boxOf(w).overlaps(within) && splitBy(w, L, neg, pos))
				{
					if (std::fabs(area2(neg)) > 1e-8)
						out.push_back(neg);
					if (std::fabs(area2(pos)) > 1e-8)
						out.push_back(pos);
				} else {
					out.push_back(std::move(w));
				}
			}
			work = std::move(out);
		};
		for (const Wall& w : pc.walls)
			if (w.box.overlaps(pbox))
				splitAll(w.L, w.box);
		for (const CutTri& t : pc.tris)
		{
			if (!t.flat || !t.box.overlaps(pbox))
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
		for (std::vector<P2>& w : work)
		{
			P2 m = { 0, 0 };
			for (const P2& p : w)
			{
				m[0] += p[0] / w.size();
				m[1] += p[1] / w.size();
			}
			hitsAt(pc, m, hits);
			const double g = gA * m[0] + gB * m[1] + gC;
			if (skipSelf >= 0)
				hits.erase(std::remove_if(hits.begin(), hits.end(), [&](double h) { return std::fabs(h - g) <= skipSelf; }), hits.end());
			bool drop = false;
			if (pc.src->projected)
			{
				// any surface over it, or inside the walls' outline (an open shell, no caps): even-odd crossings of a ray towards +x
				bool in = false;
				for (const Wall& w : pc.walls)
					if ((w.a[1] > m[1]) != (w.b[1] > m[1]) && m[0] < w.a[0] + (m[1] - w.a[1]) * (w.b[0] - w.a[0]) / (w.b[1] - w.a[1]))
						in = !in;
				drop = in || !hits.empty();
			} else {
				const double y0 = g + lo, y1 = g + hi;
				for (const double h : hits)
					drop = drop || (h > y0 && h < y1);
				if (hits.size() % 2)
					hits.insert(hits.begin(), -1e300);	// a shell open at the bottom (a rock sunk in the ground)
				for (size_t k = 0; k + 1 < hits.size() && !drop; k += 2)
					drop = hits[k] < y1 && hits[k + 1] > y0;
			}
			if (drop)
				++dropped;
			else
				next.push_back(std::move(w));
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
	if (boundary)
	{
		const int p = plan.ring.front(), n = plan.ring.back();
		const P3& A = P_[p], & B = P_[n];
		const double turn = cross2(V, A, B);	// > 0: the walkable wedge at v is under 180 deg, a convex corner
		const double area = std::fabs(turn) * 0.5;
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
		} else if (area > (turn > 0 ? prm_.borderArea : prm_.concaveArea) || chord > prm_.borderHeight) {
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
	return true;
}

void Gen::apply(int v, const Plan& plan)
{
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

void Gen::simplify(Stats& st)
{
	using Item = std::tuple<double, int, uint32_t>;
	std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
	Plan plan;
	for (int v = 0; v < (int)P_.size(); ++v)
		if (evaluate(v, plan))
			pq.push({ plan.cost, v, stamp_[v] });
	while (!pq.empty())
	{
		const auto [cost, v, stamp] = pq.top();
		pq.pop();
		if (stamp != stamp_[v] || removed_[v])
			continue;
		if (!evaluate(v, plan))
			continue;
		if (plan.cost > cost + 1e-9 && !pq.empty() && plan.cost > std::get<0>(pq.top()))
		{
			pq.push({ plan.cost, v, ++stamp_[v] });
			continue;
		}
		st.maxError = std::max(st.maxError, plan.cost);
		apply(v, plan);
		++st.removed;
		const std::vector<int> ring = plan.ring;
		for (const int r : ring)
		{
			++stamp_[r];
			if (evaluate(r, plan))
				pq.push({ plan.cost, r, stamp_[r] });
		}
	}
}

// ---- walkable tops (NAVMESH_GENERATOR.md section 12 steps 1-5) ----

// The two terrain triangles of grid cell (i, j), the diagonal that follows the ground, as build() splits them.
void Gen::terrTris(int i, int j, TerrTri out[2]) const
{
	const int nx = g_.nx;
	const int a = j * nx + i, b = a + 1, c = a + nx, d = c + 1;
	const auto pt = [&](int id) { return P3{ g_.x0 + (id % nx) * g_.step, g_.y[id], g_.z0 + (id / nx) * g_.step }; };
	const P3 pa = pt(a), pb = pt(b), pc = pt(c), pd = pt(d);
	const bool ad = std::fabs(pa[1] - pd[1]) <= std::fabs(pb[1] - pc[1]);
	const std::array<std::array<P3, 3>, 2> tri = ad ? std::array<std::array<P3, 3>, 2>{ { { pa, pb, pd }, { pa, pd, pc } } }
		: std::array<std::array<P3, 3>, 2>{ { { pa, pb, pc }, { pb, pd, pc } } };
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

double Gen::terrainAt(double x, double z) const
{
	const int i = std::clamp((int)std::floor((x - g_.x0) / g_.step), 0, g_.nx - 2), j = std::clamp((int)std::floor((z - g_.z0) / g_.step), 0, g_.nz - 2);
	TerrTri tt[2];
	terrTris(i, j, tt);
	const P2 q = { x, z };
	const bool first = tt[0].edge[0].at(q) >= -1e-9 && tt[0].edge[1].at(q) >= -1e-9 && tt[0].edge[2].at(q) >= -1e-9;
	const TerrTri& t = first ? tt[0] : tt[1];
	return t.A * x + t.B * z + t.C;
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

// The clear space straight under a feature face at height y over q (xz): going down, the first surface under the face tells what the face is.
// An underside (down-facing): the face is the top of a solid, which ends there: the clear space is from that underside to the next top surface or
// the terrain, and none when the underside is not above the terrain (a solid standing in the ground). A top surface (up-facing) or nothing: the
// face is a sheet, clear down to that surface or the terrain. (The cooker's outward winding, as surfgen reads open meshes.)
double Gen::clearUnder(const P2& q, double y) const
{
	std::vector<std::pair<double, bool>> hits, one;	// height, up-facing
	for (const PreparedCutter& pc : pcs_)
	{
		if (!pc.box.contains(q))
			continue;
		one.clear();
		hitsIndexed(pc, q, one);
		for (const auto& h : one)
			if (h.first < y - 0.05)
				hits.push_back(h);
	}
	const double t = terrainAt(q[0], q[1]);
	std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	if (hits.empty())
		return y - t;
	if (hits[0].second)
		return y - std::max(t, hits[0].first);
	if (hits[0].first <= t)
		return 0;
	double next = t;
	for (size_t i = 1; i < hits.size(); ++i)
		if (hits[i].second)
		{
			next = std::max(t, hits[i].first);
			break;
		}
	return hits[0].first - next;
}

void Gen::tops(const std::vector<Cutter>& cutters, const Params& prm, Stats& st)
{
	const auto t0 = std::chrono::steady_clock::now();
	const double x1 = g_.x0 + (g_.nx - 1) * g_.step, z1 = g_.z0 + (g_.nz - 1) * g_.step;
	const double minNy = std::cos(prm.topSlopeDeg * 3.14159265358979 / 180.0) - 1e-9;
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
		if (c.top == Cutter::Top::Deck)
		{
			// the deck rule: a face is a deck face when more than characterHeight is clear under at least half of it. A face of the same shape
			// that touches a deck face and has clear space under any part of it is the deck's end (the span that meets the ground): it stays too,
			// a face that stands solid on the ground (a cliff block's top) never qualifies. Second condition (F1): a shape whose seed faces cover less than
			// deckShapeShare of its up-facing area is no deck at all (a cliff block overhangs the terrain but its tops are rock, not a span).
			for (Up& u : ups)
			{
				const P3& a = c.tris[u.k], & b = c.tris[u.k + 1], & d = c.tris[u.k + 2];
				const int sub = std::clamp((int)std::ceil(std::sqrt(u.area / 100)), 1, 4);
				int clear = 0, total = 0;
				for (int i = 0; i < sub; ++i)
					for (int j = 0; i + j < sub; ++j)
						for (int down = 0; down < 2 && i + j + down < sub; ++down)
						{
							const double s1 = (i + (down ? 2.0 : 1.0) / 3) / sub, s2 = (j + (down ? 2.0 : 1.0) / 3) / sub;
							const P2 q = { a[0] + s1 * (b[0] - a[0]) + s2 * (d[0] - a[0]), a[2] + s1 * (b[2] - a[2]) + s2 * (d[2] - a[2]) };
							const double room = clearUnder(q, u.fA * q[0] + u.fB * q[1] + u.fC);
							++total;
							clear += room > prm.characterHeight;
							u.clearMax = std::max(u.clearMax, room);
						}
				u.seed = clear >= prm.deckShare * total - 1e-9 && clear > 0;
				u.ok = u.seed;
			}
			// second conditions (F1): a slope cap for deck faces, and a shape-level share of seed area
			const double capNy = std::cos(prm.deckMaxSlopeDeg * 3.14159265358979 / 180.0) - 1e-9;
			double seedArea = 0, upArea = 0;
			for (Up& u : ups)
			{
				u.steep = u.nyz < capNy;
				if (u.steep)
					u.seed = u.ok = false;
				upArea += u.area;
				seedArea += u.seed ? u.area : 0;
			}
			const bool shapeOk = seedArea >= prm.deckShapeShare * upArea - 1e-9 && seedArea > 0;
			if (!shapeOk)
				for (Up& u : ups)
					u.seed = u.ok = false;
			std::unordered_map<uint64_t, std::vector<size_t>> at;	// vertex (xz, y at 1e-2) -> faces
			const auto vkey = [](const P3& p) { return cellKeyOf((int64_t)std::llround(p[0] * 1e2) ^ ((int64_t)std::llround(p[1] * 1e2) << 20), (int64_t)std::llround(p[2] * 1e2)); };
			for (size_t i = 0; i < ups.size(); ++i)
				for (int v = 0; v < 3; ++v)
					at[vkey(c.tris[ups[i].k + v])].push_back(i);
			std::vector<size_t> stack;
			for (size_t i = 0; i < ups.size(); ++i)
				if (ups[i].seed)
					stack.push_back(i);
			while (!stack.empty())
			{
				const size_t i = stack.back();
				stack.pop_back();
				for (int v = 0; v < 3; ++v)
					for (const size_t j : at[vkey(c.tris[ups[i].k + v])])
						if (!ups[j].ok && !ups[j].steep && ups[j].clearMax > prm.cutBase)
						{
							ups[j].ok = true;
							stack.push_back(j);
						}
			}
			for (const Up& u : ups)
			{
				st.deckTris += u.ok;
				st.deckRejected += !u.ok;
			}
		}
		for (const Up& u : ups)
		{
			if (!u.ok)
				continue;
			const P3& a = c.tris[u.k], & b = c.tris[u.k + 1], & d = c.tris[u.k + 2];
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
			// F4: the cut rule against every solid above the face (its own shape's other faces included)
			const Box2 fb = boxOf(poly);
			for (const PreparedCutter& pc : pcs_)
			{
				if (pieces.empty() || c.prebuilt)
					break;
				if (!pc.box.overlaps(fb))
					continue;
				cutByCutter(pc, pieces, fA, fB, fC, prm.cutBase, prm.characterHeight, 0.05);
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
	st.msTops = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// Step 3: link the tops' boundary edges to the ground's and to each other. Two boundary edges of different faces link where they run against each
// other within matchSeparation in xz over at least matchOverlap, and no step above matchStep at either end of the overlap; an edge is split
// at the overlap ends (each side at its own end), and a stretch links once. A pair of ground edges never links here.
void Gen::linkTops(std::vector<std::vector<int>>& poly, std::vector<uint8_t>& dead, std::vector<uint8_t>& isTop,
	std::unordered_map<uint64_t, int>& owner, std::vector<Link>& links, Stats& st)
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
	for (size_t ea = 0; ea < E.size(); ++ea)
	{
		const Edge& A = E[ea];
		if (!isTop[A.f])
			continue;
		const P3& a0 = P_[A.u], & a1 = P_[A.w];
		const double dax = a1[0] - a0[0], daz = a1[2] - a0[2];
		std::vector<int> seen;
		for (int64_t cx = (int64_t)std::floor((std::min(a0[0], a1[0]) - 2.5) / cell); cx <= (int64_t)std::floor((std::max(a0[0], a1[0]) + 2.5) / cell); ++cx)
			for (int64_t cz = (int64_t)std::floor((std::min(a0[2], a1[2]) - 2.5) / cell); cz <= (int64_t)std::floor((std::max(a0[2], a1[2]) + 2.5) / cell); ++cz)
			{
				const auto it = byCell.find(cellKeyOf(cx, cz));
				if (it == byCell.end())
					continue;
				for (const int eb : it->second)
				{
					if ((size_t)eb == ea || E[eb].f == A.f || (isTop[E[eb].f] && (size_t)eb < ea) || std::find(seen.begin(), seen.end(), eb) != seen.end())
						continue;
					seen.push_back(eb);
					const Edge& B = E[eb];
					const P3& b0 = P_[B.u], & b1 = P_[B.w];
					const double dbx = b1[0] - b0[0], dbz = b1[2] - b0[2];
					if ((dax * dbx + daz * dbz) / (A.len * B.len) > -0.9)
						continue;
					const double t0a = ((b0[0] - a0[0]) * dax + (b0[2] - a0[2]) * daz) / (A.len * A.len), t1a = ((b1[0] - a0[0]) * dax + (b1[2] - a0[2]) * daz) / (A.len * A.len);
					const double lo = std::max(0.0, std::min(t0a, t1a)), hi = std::min(1.0, std::max(t0a, t1a));
					if ((hi - lo) * A.len < prm_.matchOverlap)
						continue;
					bool ok = true;
					for (const double t : { lo, hi })
					{
						const double px = a0[0] + t * dax, pz = a0[2] + t * daz, py = a0[1] + t * (a1[1] - a0[1]);
						const double s = std::clamp(((px - b0[0]) * dbx + (pz - b0[2]) * dbz) / (B.len * B.len), 0.0, 1.0);
						const double qx = b0[0] + s * dbx, qz = b0[2] + s * dbz, qy = b0[1] + s * (b1[1] - b0[1]);
						ok = ok && std::hypot(qx - px, qz - pz) <= prm_.matchSeparation && std::fabs(qy - py) <= prm_.matchStep;
					}
					if (ok)
						cands.push_back({ (int)ea, eb, lo, hi, (hi - lo) * A.len });
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
		if (np[0] * nq[0] + np[1] * nq[1] + np[2] * nq[2] < prm_.planarCos)
			continue;
		// p holds a -> b, q holds b -> a: p from b round to a, then q's vertices after a up to before b
		const size_t pb = std::find(p.begin(), p.end(), b) - p.begin(), qa = std::find(q.begin(), q.end(), a) - q.begin();
		std::vector<int> m;
		for (size_t k = 0; k < p.size(); ++k)
			m.push_back(p[(pb + k) % p.size()]);
		for (size_t k = 1; k + 1 < q.size(); ++k)
			m.push_back(q[(qa + k) % q.size()]);
		bool convex = true;
		for (size_t k = 0; k < m.size() && convex; ++k)
			convex = cross2(P_[m[k]], P_[m[(k + 1) % m.size()]], P_[m[(k + 2) % m.size()]]) >= -1e-9;
		if (!convex)
			continue;
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
	std::vector<Link> links;
	if (!topPolys_.empty())
		linkTops(poly, dead, isTop, owner, links, st);
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

	native::Mesh mesh;
	std::vector<int> remap(P_.size(), -1);
	std::unordered_map<uint64_t, int> edgeAt;
	for (size_t f = 0; f < poly.size(); ++f)
	{
		if (dead[f])
			continue;
		native::Face face;
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
			const auto& u = m.vertices[ed.a], & w = m.vertices[ed.b];
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

WeldStats weldMesh(native::Mesh& m, double grid, double climb)
{
	WeldStats stats;
	const std::size_t nv = m.vertices.size();
	// (i) vertices within grid in xz and climb in y: the first seen stays
	std::vector<std::uint32_t> rep(nv);
	for (std::size_t i = 0; i < nv; ++i)
		rep[i] = (std::uint32_t)i;
	{
		const double cell = grid * 2;
		std::unordered_map<uint64_t, std::vector<std::uint32_t>> grid2;
		const auto cellOf = [&](double v) { return (int64_t)std::floor(v / cell); };
		std::vector<uint8_t> used(nv, 0);
		for (const native::Edge& e : m.edges)
			used[e.a] = used[e.b] = 1;
		for (std::size_t i = 0; i < nv; ++i)
		{
			if (!used[i])
				continue;
			const auto& v = m.vertices[i];
			const int64_t cx = cellOf(v[0]), cz = cellOf(v[2]);
			bool found = false;
			for (int64_t dx = -1; dx <= 1 && !found; ++dx)
				for (int64_t dz = -1; dz <= 1 && !found; ++dz)
				{
					const auto it = grid2.find(cellKeyOf(cx + dx, cz + dz));
					if (it == grid2.end())
						continue;
					for (const std::uint32_t o : it->second)
					{
						const auto& w = m.vertices[o];
						if (std::fabs(w[0] - v[0]) <= grid && std::fabs(w[2] - v[2]) <= grid && std::fabs(w[1] - v[1]) <= climb)
						{
							rep[i] = o;
							found = true;
							break;
						}
					}
				}
			if (found)
				++stats.welded;
			else
				grid2[cellKeyOf(cx, cz)].push_back((std::uint32_t)i);
		}
	}
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
		for (const Entry& en : loops[f])
			if (en.old >= 0 && m.edges[en.old].oppositeEdge >= 0)
				droppedOpp.push_back(m.edges[en.old].oppositeEdge);
		loops[f].clear();
		++stats.dropped;
	}
	std::vector<uint8_t> orphan(m.edges.size(), 0);
	for (const std::int32_t e : droppedOpp)
		orphan[(std::size_t)e] = 1;
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
	for (std::size_t i = 0; i < nv; ++i)
	{
		if (rep[i] != i || done[i])
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
				if (!orphan[(std::size_t)en.old])
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
	(void)faceOfOld;
	m = std::move(out);
	return stats;
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
	return cuttersOfArea(area, x0, z0, x1, z1, counts);
}

std::vector<Cutter> cuttersOfArea(const kworld::AreaGeometry& area, double x0, double z0, double x1, double z1, CutterCounts* counts)
{
	CutterCounts n;
	std::vector<Cutter> cutters;
	const auto add = [&](const kworld::WorldShape& s, std::size_t& count, Cutter::Top top, const std::string& label)
	{
		if (s.use == kworld::NavUse::Door)
		{
			++n.doorsSkipped;
			return;
		}
		if (s.max[0] < x0 - 1 || s.min[0] > x1 + 1 || s.max[2] < z0 - 1 || s.min[2] > z1 + 1 || s.indices.size() < 3)
			return;
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
			add(s, n.features, Cutter::Top::Deck, "feature @ " + std::to_string((int)((s.min[0] + s.max[0]) / 2)) + ", " + std::to_string((int)((s.min[2] + s.max[2]) / 2)));
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
