#include "knav/surfgen.hpp"

#include "clipper2/clipper.h"
#include "clipper2/clipper.triangulation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace knav::surfgen
{

namespace
{

namespace c2 = Clipper2Lib;

using P2 = std::array<double, 2>; // x, z
using P3 = std::array<double, 3>;
using Poly = std::vector<P2>; // convex, counter-clockwise (positive shoelace area over x, z)

constexpr double kPi = 3.14159265358979323846;

double cross2(const P2& a, const P2& b, const P2& c) { return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]); }

double area(const Poly& p)
{
	double sum = 0.0;
	for (std::size_t i = 0; i < p.size(); ++i)
	{
		const P2& a = p[i];
		const P2& b = p[(i + 1) % p.size()];
		sum += a[0] * b[1] - b[0] * a[1];
	}
	return 0.5 * sum;
}

// Keeps the part of `p` where A x + B z + C >= 0 (Sutherland-Hodgman against one line).
Poly clipLine(const Poly& p, double A, double B, double C)
{
	Poly out;
	const std::size_t n = p.size();
	for (std::size_t i = 0; i < n; ++i)
	{
		const P2& a = p[i];
		const P2& b = p[(i + 1) % n];
		const double sa = A * a[0] + B * a[1] + C, sb = A * b[0] + B * b[1] + C;
		if (sa >= 0.0)
			out.push_back(a);
		if ((sa >= 0.0) != (sb >= 0.0))
		{
			const double t = sa / (sa - sb);
			out.push_back({ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t });
		}
	}
	return out;
}

// The line of edge a->b normalised: side >= 0 is the left of the edge (the inside of a counter-clockwise polygon).
struct Line
{
	double A, B, C;
};

Line edgeLine(const P2& a, const P2& b)
{
	const double length = std::hypot(b[0] - a[0], b[1] - a[1]);
	const double dx = (b[0] - a[0]) / length, dz = (b[1] - a[1]) / length;
	return { -dz, dx, dz * a[0] - dx * a[1] };
}

Poly intersect(Poly p, const Poly& c)
{
	for (std::size_t i = 0; i < c.size() && !p.empty(); ++i)
	{
		const Line l = edgeLine(c[i], c[(i + 1) % c.size()]);
		p = clipLine(p, l.A, l.B, l.C);
	}
	return p;
}

Poly hull(std::vector<P2> points)
{
	std::sort(points.begin(), points.end());
	points.erase(std::unique(points.begin(), points.end(), [](const P2& a, const P2& b) { return std::fabs(a[0] - b[0]) < 1e-9 && std::fabs(a[1] - b[1]) < 1e-9; }), points.end());
	if (points.size() < 3)
		return {};
	Poly h(2 * points.size());
	std::size_t k = 0;
	for (std::size_t i = 0; i < points.size(); ++i)
	{
		while (k >= 2 && cross2(h[k - 2], h[k - 1], points[i]) <= 1e-12)
			--k;
		h[k++] = points[i];
	}
	for (std::size_t i = points.size() - 1, t = k + 1; i > 0; --i)
	{
		while (k >= t && cross2(h[k - 2], h[k - 1], points[i - 1]) <= 1e-12)
			--k;
		h[k++] = points[i - 1];
	}
	h.resize(k - 1);
	return h;
}

// A hull of points that came out flat (a wall's sheet, a vertical triangle): a rectangle of `half` either side of its longest chord.
Poly thick(const std::vector<P2>& points, double half)
{
	P2 best0{}, best1{};
	double longest = -1.0;
	for (const P2& a : points)
		for (const P2& b : points)
		{
			const double d = std::hypot(a[0] - b[0], a[1] - b[1]);
			if (d > longest)
			{
				longest = d;
				best0 = a;
				best1 = b;
			}
		}
	if (longest < 1e-6)
		return {};
	const double dx = (best1[0] - best0[0]) / longest * half, dz = (best1[1] - best0[1]) / longest * half;
	return hull({ { best0[0] - dx - dz, best0[1] - dz + dx }, { best0[0] - dx + dz, best0[1] - dz - dx },
		{ best1[0] + dx - dz, best1[1] + dz + dx }, { best1[0] + dx + dz, best1[1] + dz - dx } });
}

struct Plane
{
	double a = 0, b = 0, c = 0; // y = a x + b z + c
	double at(double x, double z) const { return a * x + b * z + c; }
};

// A walkable piece of one shape: a coplanar group of a convex solid's up-facing triangles, or one triangle of an open mesh.
struct Piece
{
	Poly poly;
	Plane plane;
	std::size_t shape = 0;
	std::size_t triangle = SIZE_MAX; // open mesh: the source triangle
};

struct Solid
{
	const kworld::WorldShape* shape = nullptr;
	std::vector<P3> v;
	std::vector<std::array<std::uint32_t, 2>> edges; // unique, for a convex solid
	bool convex = false; // closed || solid: a convex body
	bool projected = false;
	P3 min{}, max{};
};

Solid makeSolid(const kworld::WorldShape& s)
{
	Solid out;
	out.shape = &s;
	out.convex = s.closed || s.solid;
	out.projected = s.use == kworld::NavUse::Projected;
	out.min = { 1e30, 1e30, 1e30 };
	out.max = { -1e30, -1e30, -1e30 };
	for (std::size_t i = 0; i + 2 < s.vertices.size(); i += 3)
	{
		out.v.push_back({ s.vertices[i], s.vertices[i + 1], s.vertices[i + 2] });
		for (std::size_t k = 0; k < 3; ++k)
		{
			out.min[k] = std::min(out.min[k], out.v.back()[k]);
			out.max[k] = std::max(out.max[k], out.v.back()[k]);
		}
	}
	if (out.convex)
	{
		std::vector<std::array<std::uint32_t, 2>> edges;
		for (std::size_t t = 0; t + 2 < s.indices.size(); t += 3)
			for (std::size_t k = 0; k < 3; ++k)
			{
				std::uint32_t a = s.indices[t + k], b = s.indices[t + (k + 1) % 3];
				edges.push_back({ std::min(a, b), std::max(a, b) });
			}
		std::sort(edges.begin(), edges.end());
		edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
		out.edges = std::move(edges);
	}
	return out;
}

// The xz regions of `solid` that obstruct a face on `plane` within [base, top] above it, as convex polygons. `bounds` (xz) rejects early.
// `skip` (sorted): triangles of an open shape that ARE the surface (never cut it).
void footprints(const Solid& solid, const Plane& plane, double base, double top, const P2& bmin, const P2& bmax, double halfWidth,
	std::vector<Poly>& out, const std::vector<std::size_t>* skip = nullptr)
{
	if (solid.max[0] < bmin[0] || solid.min[0] > bmax[0] || solid.max[2] < bmin[1] || solid.min[2] > bmax[1])
		return;
	if (solid.projected)
	{
		if (solid.convex)
		{
			std::vector<P2> pts;
			for (const P3& v : solid.v)
				pts.push_back({ v[0], v[2] });
			Poly h = hull(pts);
			if (h.size() >= 3)
				out.push_back(std::move(h));
		} else {
			for (std::size_t t = 0; t + 2 < solid.shape->indices.size(); t += 3)
			{
				std::vector<P2> pts;
				for (std::size_t k = 0; k < 3; ++k)
					pts.push_back({ solid.v[solid.shape->indices[t + k]][0], solid.v[solid.shape->indices[t + k]][2] });
				Poly h = hull(pts);
				if (h.size() < 3 || area(h) < 1e-4)
					h = thick(pts, halfWidth);
				if (h.size() >= 3)
					out.push_back(std::move(h));
			}
		}
		return;
	}
	const auto rise = [&](const P3& v) { return v[1] - plane.at(v[0], v[2]); };
	if (solid.convex)
	{
		std::vector<P2> pts;
		std::vector<double> r(solid.v.size());
		for (std::size_t i = 0; i < solid.v.size(); ++i)
		{
			r[i] = rise(solid.v[i]);
			if (r[i] > base && r[i] <= top)
				pts.push_back({ solid.v[i][0], solid.v[i][2] });
		}
		for (const auto& e : solid.edges)
		{
			const double r0 = r[e[0]], r1 = r[e[1]];
			for (const double level : { base, top })
				if ((r0 - level) * (r1 - level) < 0.0)
				{
					const double t = (level - r0) / (r1 - r0);
					pts.push_back({ solid.v[e[0]][0] + (solid.v[e[1]][0] - solid.v[e[0]][0]) * t, solid.v[e[0]][2] + (solid.v[e[1]][2] - solid.v[e[0]][2]) * t });
				}
		}
		if (pts.size() < 3)
			return;
		Poly h = hull(pts);
		if (h.size() < 3 || area(h) < 1e-4)
			h = thick(pts, halfWidth);
		if (h.size() >= 3)
			out.push_back(std::move(h));
		return;
	}
	for (std::size_t t = 0; t + 2 < solid.shape->indices.size(); t += 3)
	{
		if (skip != nullptr && std::binary_search(skip->begin(), skip->end(), t / 3))
			continue;
		struct V
		{
			double x, z, r;
		};
		std::vector<V> poly;
		for (std::size_t k = 0; k < 3; ++k)
		{
			const P3& v = solid.v[solid.shape->indices[t + k]];
			poly.push_back({ v[0], v[2], rise(v) });
		}
		const auto clipRise = [](const std::vector<V>& in, double level, bool keepAbove)
		{
			std::vector<V> res;
			for (std::size_t i = 0; i < in.size(); ++i)
			{
				const V& a = in[i];
				const V& b = in[(i + 1) % in.size()];
				const bool ina = keepAbove ? a.r > level : a.r <= level, inb = keepAbove ? b.r > level : b.r <= level;
				if (ina)
					res.push_back(a);
				if (ina != inb)
				{
					const double u = (level - a.r) / (b.r - a.r);
					res.push_back({ a.x + (b.x - a.x) * u, a.z + (b.z - a.z) * u, level });
				}
			}
			return res;
		};
		poly = clipRise(poly, base, true);
		if (poly.size() >= 3)
			poly = clipRise(poly, top, false);
		if (poly.size() < 3)
			continue;
		std::vector<P2> pts;
		for (const V& v : poly)
			pts.push_back({ v.x, v.z });
		Poly h = hull(pts);
		if (h.size() < 3 || area(h) < 1e-4)
			h = thick(pts, halfWidth);
		if (h.size() >= 3)
			out.push_back(std::move(h));
	}
}

// Walkable faces of one shape: a convex solid gives one polygon per coplanar group of up-facing triangles, an open mesh one per triangle.
void shapePieces(const Solid& solid, std::size_t shapeIndex, const Settings& cfg, std::vector<Piece>& out)
{
	const kworld::WorldShape& s = *solid.shape;
	const double minNy = std::cos(cfg.slopeDegrees * kPi / 180.0) - 1e-9;
	P3 centre{};
	for (const P3& v : solid.v)
		for (std::size_t k = 0; k < 3; ++k)
			centre[k] += v[k] / static_cast<double>(solid.v.size());
	struct Cluster
	{
		P3 n;
		double d;
		std::vector<P2> pts;
	};
	std::vector<Cluster> clusters;
	for (std::size_t t = 0; t + 2 < s.indices.size(); t += 3)
	{
		const P3 &p0 = solid.v[s.indices[t]], &p1 = solid.v[s.indices[t + 1]], &p2 = solid.v[s.indices[t + 2]];
		const P3 u = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] }, w = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
		P3 n = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
		const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (len < 1e-9)
			continue;
		for (double& c : n)
			c /= len;
		if (solid.convex && n[0] * (p0[0] - centre[0]) + n[1] * (p0[1] - centre[1]) + n[2] * (p0[2] - centre[2]) < 0.0)
			for (double& c : n)
				c = -c; // outward
		if (n[1] < minNy || n[1] <= 0.0)
			continue;
		// degenerate: the triangle's narrowest altitude, xz
		const P2 a{ p0[0], p0[2] }, b{ p1[0], p1[2] }, c{ p2[0], p2[2] };
		const double twiceArea = std::fabs(cross2(a, b, c));
		const double longestEdge = std::max({ std::hypot(b[0] - a[0], b[1] - a[1]), std::hypot(c[0] - b[0], c[1] - b[1]), std::hypot(a[0] - c[0], a[1] - c[1]) });
		if (twiceArea < 1e-9 || twiceArea / longestEdge < cfg.minWidth)
			continue;
		const double d = n[0] * p0[0] + n[1] * p0[1] + n[2] * p0[2];
		if (!solid.convex)
		{
			Piece piece;
			piece.poly = { a, b, c };
			if (area(piece.poly) < 0.0)
				std::reverse(piece.poly.begin(), piece.poly.end());
			piece.plane = { -n[0] / n[1], -n[2] / n[1], d / n[1] };
			piece.shape = shapeIndex;
			piece.triangle = t / 3;
			out.push_back(std::move(piece));
			continue;
		}
		Cluster* found = nullptr;
		for (Cluster& cl : clusters)
			if (n[0] * cl.n[0] + n[1] * cl.n[1] + n[2] * cl.n[2] > 0.9998 && std::fabs(d - cl.d) < 0.05)
			{
				found = &cl;
				break;
			}
		if (found == nullptr)
		{
			clusters.push_back({ n, d, {} });
			found = &clusters.back();
		}
		found->pts.insert(found->pts.end(), { a, b, c });
	}
	for (const Cluster& cl : clusters)
	{
		Piece piece;
		piece.poly = hull(cl.pts);
		if (piece.poly.size() < 3)
			continue;
		piece.plane = { -cl.n[0] / cl.n[1], -cl.n[2] / cl.n[1], cl.d / cl.n[1] };
		piece.shape = shapeIndex;
		out.push_back(std::move(piece));
	}
}

// ---- exact integer regions (Clipper2): corners on the lattice, x -> X, z -> Y

struct Grid
{
	double s; // grid steps per unit
	c2::Point64 to(const P2& p) const { return c2::Point64(std::llround(p[0] * s), std::llround(p[1] * s)); }
	P2 from(const c2::Point64& q) const { return { static_cast<double>(q.x) / s, static_cast<double>(q.y) / s }; }
	c2::Path64 path(const Poly& p) const
	{
		c2::Path64 out;
		for (const P2& v : p)
			out.push_back(to(v));
		return out;
	}
};

using Key = std::pair<std::int64_t, std::int64_t>;
Key keyOf(const c2::Point64& p) { return { p.x, p.y }; }

std::int64_t cross64(const c2::Point64& a, const c2::Point64& b, const c2::Point64& c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// A convex polygon moved in by w on every side.
Poly inset(Poly p, double w)
{
	const Poly ring = p;
	for (std::size_t i = 0; i < ring.size() && p.size() >= 3; ++i)
	{
		const Line l = edgeLine(ring[i], ring[(i + 1) % ring.size()]);
		p = clipLine(p, l.A, l.B, l.C - w);
	}
	return p;
}

Poly box(const P2& lo, const P2& hi) { return { lo, { hi[0], lo[1] }, hi, { lo[0], hi[1] } }; }

// The part of `area` where lo <= A x + B z + C <= hi.
Poly strip(Poly area, double A, double B, double C, double lo, double hi)
{
	area = clipLine(area, A, B, C - lo);
	if (area.size() >= 3)
		area = clipLine(area, -A, -B, hi - C);
	return area;
}

// A convex volume's section (n . p <= d planes, inside) in a plane, clipped to `area`.
Poly section(Poly area, const Plane& plane, const std::vector<std::array<float, 4>>& planes)
{
	for (const auto& h : planes)
	{
		area = clipLine(area, -(h[0] + h[1] * plane.a), -(h[2] + h[1] * plane.b), h[3] - h[1] * plane.c);
		if (area.size() < 3)
			return {};
	}
	return area;
}

c2::Paths64 inflate(const c2::Paths64& p, double delta, c2::JoinType join, double mitre = 2.0, double arc = 0.0)
{
	if (p.empty())
		return {};
	return c2::InflatePaths(p, delta, join, c2::EndType::Polygon, mitre, arc);
}

// The weld: a closing (cracks narrower than 2 w filled) then an opening (slivers narrower than 2 w removed), mitred so corners stay.
c2::Paths64 weld(const c2::Paths64& r, double w, bool close)
{
	if (w <= 0.0)
		return r;
	c2::Paths64 out = close ? inflate(inflate(r, w, c2::JoinType::Miter), -w, c2::JoinType::Miter) : r;
	return inflate(inflate(out, -w, c2::JoinType::Miter), w, c2::JoinType::Miter);
}

// Corners a grid step or so off the line through their neighbours go: the staircase a boolean's rounding leaves at a shallow bend (one-step
// edges). Then a union, in case the simplification crossed an edge.
c2::Paths64 tidy(const c2::Paths64& r)
{
	c2::Paths64 out;
	for (c2::Path64& p : c2::SimplifyPaths(r, 1.5))
		if (p.size() >= 3)
			out.push_back(std::move(p));
	return c2::Union(out, c2::FillRule::NonZero);
}

// One plane's walkable region.
struct Surface
{
	Plane plane;
	std::size_t shape = SIZE_MAX; // the lowest shape index among its pieces
	std::vector<std::size_t> pieces;
	P2 bmin{ 1e30, 1e30 }, bmax{ -1e30, -1e30 };
	c2::Paths64 region;
};

// ---- links between edges

using Interval = std::array<double, 2>;
using Intervals = std::vector<Interval>;

void subtractInterval(Intervals& set, double a, double b)
{
	Intervals out;
	for (const Interval& v : set)
	{
		if (v[1] <= a || v[0] >= b)
		{
			out.push_back(v);
			continue;
		}
		if (v[0] < a)
			out.push_back({ v[0], a });
		if (v[1] > b)
			out.push_back({ b, v[1] });
	}
	set = std::move(out);
}

Intervals intersectIntervals(const Intervals& x, const Intervals& y)
{
	Intervals out;
	for (const Interval& a : x)
		for (const Interval& b : y)
			if (std::min(a[1], b[1]) > std::max(a[0], b[0]))
				out.push_back({ std::max(a[0], b[0]), std::min(a[1], b[1]) });
	std::sort(out.begin(), out.end());
	return out;
}

// Keeps the part of [t0, t1] where v0 + (v1 - v0) (t - t0) / (t1 - t0) lies in [lo, hi] (v linear in t).
bool clipLinear(double& t0, double& t1, double v0, double v1, double lo, double hi)
{
	const double dv = v1 - v0, dt = t1 - t0;
	if (std::fabs(dv) < 1e-12)
		return v0 >= lo && v0 <= hi;
	double a = t0 + (lo - v0) / dv * dt, b = t0 + (hi - v0) / dv * dt;
	if (a > b)
		std::swap(a, b);
	const double n0 = std::max(t0, a), n1 = std::min(t1, b);
	t0 = n0;
	t1 = n1;
	return t1 > t0;
}

// A directed boundary edge, its floor on the left.
struct Seg
{
	P2 a, b;
	const Plane* plane;
	std::size_t surface;
};

// Two edges linked over [t0, t1] of i (length from i.a) and [u0, u1] of j; i's t0 end meets j's u1 end.
struct Link
{
	std::size_t i, j;
	double t0, t1, u0, u1;
};

// Links between edges that face each other (antiparallel, B outside A by -0.05 .. matchSeparationMax), over the part where the height step is at
// most matchStep and nothing solid stands on the path between them (footprints rising more than cutBase above the plane through both edges:
// a bevelled riser lies on that path, a wall does not). Nearest first, then the smaller step; each part of an edge links once, both ways.
// `allowed`: when set, the parts of edge i that may link to edge j.
std::vector<Link> matchEdges(const std::vector<Seg>& segs, const std::vector<Solid>& solids, const Settings& cfg,
	const std::function<Intervals(std::size_t, std::size_t)>& allowed)
{
	constexpr double kSlack = 0.05; // B may overlap A this much (snap noise)
	constexpr double kAntiparallel = -0.985;
	constexpr double kCell = 4.0;
	const auto cellOf = [&](double v) { return static_cast<std::int64_t>(std::floor(v / kCell)); };
	std::map<Key, std::vector<std::size_t>> grid;
	for (std::size_t i = 0; i < segs.size(); ++i)
		for (std::int64_t cx = cellOf(std::min(segs[i].a[0], segs[i].b[0])); cx <= cellOf(std::max(segs[i].a[0], segs[i].b[0])); ++cx)
			for (std::int64_t cz = cellOf(std::min(segs[i].a[1], segs[i].b[1])); cz <= cellOf(std::max(segs[i].a[1], segs[i].b[1])); ++cz)
				grid[{ cx, cz }].push_back(i);
	struct Candidate
	{
		double gap, step;
		std::size_t i, j;
		double t0, t1;
	};
	std::vector<Candidate> candidates;
	std::vector<std::size_t> near;
	for (std::size_t i = 0; i < segs.size(); ++i)
	{
		const Seg& A = segs[i];
		const double li = std::hypot(A.b[0] - A.a[0], A.b[1] - A.a[1]);
		if (li < cfg.matchOverlap)
			continue;
		const double ux = (A.b[0] - A.a[0]) / li, uz = (A.b[1] - A.a[1]) / li;
		near.clear();
		const double reach = cfg.matchSeparationMax;
		for (std::int64_t cx = cellOf(std::min(A.a[0], A.b[0]) - reach); cx <= cellOf(std::max(A.a[0], A.b[0]) + reach); ++cx)
			for (std::int64_t cz = cellOf(std::min(A.a[1], A.b[1]) - reach); cz <= cellOf(std::max(A.a[1], A.b[1]) + reach); ++cz)
			{
				const auto cell = grid.find({ cx, cz });
				if (cell != grid.end())
					for (const std::size_t j : cell->second)
						if (j > i)
							near.push_back(j);
			}
		std::sort(near.begin(), near.end());
		near.erase(std::unique(near.begin(), near.end()), near.end());
		for (const std::size_t j : near)
		{
			const Seg& B = segs[j];
			const double lj = std::hypot(B.b[0] - B.a[0], B.b[1] - B.a[1]);
			if (lj < cfg.matchOverlap || ((B.b[0] - B.a[0]) * ux + (B.b[1] - B.a[1]) * uz) / lj > kAntiparallel)
				continue;
			// B's ends along A and their distance to A's right (outside)
			const double tb0 = (B.a[0] - A.a[0]) * ux + (B.a[1] - A.a[1]) * uz, tb1 = (B.b[0] - A.a[0]) * ux + (B.b[1] - A.a[1]) * uz;
			const double db0 = (B.a[0] - A.a[0]) * uz - (B.a[1] - A.a[1]) * ux, db1 = (B.b[0] - A.a[0]) * uz - (B.b[1] - A.a[1]) * ux;
			const auto gapAt = [&](double t) { return db0 + (db1 - db0) * (t - tb0) / (tb1 - tb0); };
			const auto onA = [&](double t) { return P2{ A.a[0] + ux * t, A.a[1] + uz * t }; };
			const auto onB = [&](double t) { const double d = gapAt(t); return P2{ A.a[0] + ux * t + uz * d, A.a[1] + uz * t - ux * d }; };
			const auto stepAt = [&](double t) { const P2 p = onA(t), q = onB(t); return B.plane->at(q[0], q[1]) - A.plane->at(p[0], p[1]); };
			double s0 = std::max(0.0, std::min(tb0, tb1)), s1 = std::min(li, std::max(tb0, tb1));
			if (s1 - s0 < cfg.matchOverlap)
				continue;
			if (!clipLinear(s0, s1, gapAt(s0), gapAt(s1), -kSlack, cfg.matchSeparationMax) || s1 - s0 < cfg.matchOverlap)
				continue;
			if (!clipLinear(s0, s1, stepAt(s0), stepAt(s1), -cfg.matchStep, cfg.matchStep) || s1 - s0 < cfg.matchOverlap)
				continue;
			Intervals parts{ { s0, s1 } };
			if (allowed)
				parts = intersectIntervals(parts, allowed(i, j));
			// blocked by a solid on the path between the edges?
			if (!parts.empty() && std::max(gapAt(s0), gapAt(s1)) > kSlack)
			{
				const P2 p0 = onA(s0), p1 = onA(s1), q = onB(0.5 * (s0 + s1));
				const P3 a3{ p0[0], A.plane->at(p0[0], p0[1]), p0[1] }, b3{ p1[0], A.plane->at(p1[0], p1[1]), p1[1] }, c3{ q[0], B.plane->at(q[0], q[1]), q[1] };
				// y = a x + b z + c through the three points
				const double det = (b3[0] - a3[0]) * (c3[2] - a3[2]) - (c3[0] - a3[0]) * (b3[2] - a3[2]);
				Plane path = *A.plane;
				if (std::fabs(det) > 1e-9)
				{
					path.a = ((b3[1] - a3[1]) * (c3[2] - a3[2]) - (c3[1] - a3[1]) * (b3[2] - a3[2])) / det;
					path.b = ((b3[0] - a3[0]) * (c3[1] - a3[1]) - (c3[0] - a3[0]) * (b3[1] - a3[1])) / det;
					path.c = a3[1] - path.a * a3[0] - path.b * a3[2];
				}
				const Poly quad = hull({ p0, p1, onB(s0), onB(s1) });
				if (quad.size() >= 3)
				{
					P2 lo{ 1e30, 1e30 }, hi{ -1e30, -1e30 };
					for (const P2& v : quad)
						for (std::size_t k = 0; k < 2; ++k)
						{
							lo[k] = std::min(lo[k], v[k]);
							hi[k] = std::max(hi[k], v[k]);
						}
					std::vector<Poly> cut;
					for (const Solid& s : solids)
						footprints(s, path, cfg.cutBase, cfg.characterHeight, lo, hi, cfg.barrierHalfWidth, cut);
					for (const Poly& c : cut)
					{
						const Poly over = intersect(quad, c);
						if (over.size() < 3 || area(over) < 1e-6)
							continue;
						double b0 = 1e30, b1 = -1e30;
						for (const P2& v : over)
						{
							const double t = (v[0] - A.a[0]) * ux + (v[1] - A.a[1]) * uz;
							b0 = std::min(b0, t);
							b1 = std::max(b1, t);
						}
						subtractInterval(parts, b0, b1);
					}
				}
			}
			for (const Interval& part : parts)
				if (part[1] - part[0] >= cfg.matchOverlap)
				{
					const double mid = 0.5 * (part[0] + part[1]);
					candidates.push_back({ std::max(0.0, gapAt(mid)), std::fabs(stepAt(mid)), i, j, part[0], part[1] });
				}
		}
	}
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& x, const Candidate& y)
	{
		if (x.gap != y.gap)
			return x.gap < y.gap;
		if (x.step != y.step)
			return x.step < y.step;
		if (x.i != y.i)
			return x.i < y.i;
		if (x.j != y.j)
			return x.j < y.j;
		return x.t0 < y.t0;
	});
	std::vector<Intervals> taken(segs.size());
	const auto freeOf = [&](std::size_t s, double a, double b)
	{
		Intervals out{ { a, b } };
		for (const Interval& t : taken[s])
			subtractInterval(out, t[0], t[1]);
		return out;
	};
	std::vector<Link> links;
	for (const Candidate& c : candidates)
	{
		const Seg &A = segs[c.i], &B = segs[c.j];
		const double li = std::hypot(A.b[0] - A.a[0], A.b[1] - A.a[1]), lj = std::hypot(B.b[0] - B.a[0], B.b[1] - B.a[1]);
		const double ux = (A.b[0] - A.a[0]) / li, uz = (A.b[1] - A.a[1]) / li, vx = (B.b[0] - B.a[0]) / lj, vz = (B.b[1] - B.a[1]) / lj;
		// i's t -> j's u: the foot of A's point on B's line (B antiparallel: u falls as t rises); linear, so two points fix it
		const auto uOf = [&](double t) { const P2 p{ A.a[0] + ux * t, A.a[1] + uz * t }; return (p[0] - B.a[0]) * vx + (p[1] - B.a[1]) * vz; };
		const double uAt0 = uOf(0.0), uSlope = uOf(1.0) - uAt0;
		if (std::fabs(uSlope) < 1e-12)
			continue;
		for (const Interval& f : freeOf(c.i, c.t0, c.t1))
		{
			const double ua = std::clamp(uAt0 + uSlope * f[1], 0.0, lj), ub = std::clamp(uAt0 + uSlope * f[0], 0.0, lj);
			for (const Interval& g : freeOf(c.j, std::min(ua, ub), std::max(ua, ub)))
			{
				if (g[1] - g[0] < cfg.matchOverlap)
					continue;
				double t0 = (g[1] - uAt0) / uSlope, t1 = (g[0] - uAt0) / uSlope;
				if (t0 > t1)
					std::swap(t0, t1);
				t0 = std::max(t0, f[0]);
				t1 = std::min(t1, f[1]);
				if (t1 - t0 < cfg.matchOverlap)
					continue;
				links.push_back({ c.i, c.j, t0, t1, g[0], g[1] });
				taken[c.i].push_back({ t0, t1 });
				taken[c.j].push_back({ g[0], g[1] });
			}
		}
	}
	return links;
}

// ---- faces

struct FaceEdge
{
	P2 a; // start corner
	int uid = 0;
};

struct Face
{
	std::vector<FaceEdge> ring; // counter-clockwise; edge i runs ring[i].a -> ring[i+1].a
	Plane plane;
	std::int32_t data = 1;
	kworld::DoorKey door = 0;
	std::size_t shape = 0;
	std::size_t surface = 0;
	Side side = Side::Interior;
};

P2 edgeEnd(const Face& f, std::size_t i) { return f.ring[(i + 1) % f.ring.size()].a; }

Poly faceXz(const Face& f)
{
	Poly p;
	for (const FaceEdge& e : f.ring)
		p.push_back(e.a);
	return p;
}

double faceArea(const Face& f) { return area(faceXz(f)); }

// A piece of one surface on one side, painted or not, before it is cut into faces.
struct Part
{
	std::size_t surface;
	Side side;
	std::int32_t data = 1;
	kworld::DoorKey door = 0;
	c2::Paths64 paths;
};

// One integer face during the decomposition.
struct IFace
{
	c2::Path64 ring;
	std::size_t part;
};

// Convex faces of a region: constrained Delaunay triangles merged across their longest shared edges first while the union stays convex
// (Hertel-Mehlhorn; exact integer tests).
bool decompose(const c2::Paths64& paths, std::size_t part, std::vector<IFace>& out)
{
	c2::Paths64 tris;
	if (c2::Triangulate(paths, tris, true) != c2::TriangulateResult::success)
		return false;
	std::vector<c2::Path64> faces;
	for (c2::Path64& t : tris)
	{
		if (t.size() != 3)
			continue;
		const std::int64_t c = cross64(t[0], t[1], t[2]);
		if (c == 0)
			continue;
		if (c < 0)
			std::reverse(t.begin(), t.end());
		faces.push_back(std::move(t));
	}
	// the interior edges, longest first
	std::map<std::pair<Key, Key>, std::size_t> owner; // directed edge -> face
	for (std::size_t f = 0; f < faces.size(); ++f)
		for (std::size_t k = 0; k < faces[f].size(); ++k)
			owner[{ keyOf(faces[f][k]), keyOf(faces[f][(k + 1) % faces[f].size()]) }] = f;
	struct Diagonal
	{
		double length;
		Key a, b;
	};
	std::vector<Diagonal> diagonals;
	for (const auto& [edge, f] : owner)
		if (edge.first < edge.second && owner.count({ edge.second, edge.first }) != 0)
			diagonals.push_back({ std::hypot(static_cast<double>(edge.second.first - edge.first.first), static_cast<double>(edge.second.second - edge.first.second)), edge.first, edge.second });
	std::sort(diagonals.begin(), diagonals.end(), [](const Diagonal& x, const Diagonal& y) { return x.length != y.length ? x.length > y.length : std::tie(x.a, x.b) < std::tie(y.a, y.b); });
	std::vector<char> alive(faces.size(), 1);
	for (const Diagonal& d : diagonals)
	{
		const auto ia = owner.find({ d.a, d.b }), ib = owner.find({ d.b, d.a });
		if (ia == owner.end() || ib == owner.end() || ia->second == ib->second)
			continue;
		const std::size_t A = ia->second, B = ib->second;
		const c2::Path64 &ra = faces[A], &rb = faces[B];
		// the shared chain: A's edges that B has reversed, consecutive; merged = A from the chain's end round to its start, then B's rest
		std::vector<char> sharedA(ra.size(), 0);
		std::size_t count = 0;
		for (std::size_t k = 0; k < ra.size(); ++k)
		{
			const auto it = owner.find({ keyOf(ra[(k + 1) % ra.size()]), keyOf(ra[k]) });
			sharedA[k] = it != owner.end() && it->second == B ? 1 : 0;
			count += sharedA[k];
		}
		std::size_t start = 0; // first shared edge of the run
		while (!(sharedA[start] && !sharedA[(start + ra.size() - 1) % ra.size()]))
			++start;
		bool run = true;
		for (std::size_t k = 0; k < count; ++k)
			run = run && sharedA[(start + k) % ra.size()];
		if (!run)
			continue;
		const c2::Point64 chainStart = ra[start], chainEnd = ra[(start + count) % ra.size()];
		c2::Path64 merged;
		for (std::size_t k = 0; k <= ra.size() - count; ++k)
			merged.push_back(ra[(start + count + k) % ra.size()]); // chainEnd ... chainStart
		std::size_t bStart = 0;
		while (!(rb[bStart] == chainStart))
			++bStart;
		for (std::size_t k = 1; k < rb.size(); ++k)
		{
			const c2::Point64& v = rb[(bStart + k) % rb.size()];
			if (v == chainEnd)
				break;
			merged.push_back(v);
		}
		bool convex = merged.size() >= 3;
		for (std::size_t k = 0; k < merged.size() && convex; ++k)
			convex = cross64(merged[(k + merged.size() - 1) % merged.size()], merged[k], merged[(k + 1) % merged.size()]) >= 0;
		std::vector<Key> seen;
		for (const c2::Point64& v : merged)
			seen.push_back(keyOf(v));
		std::sort(seen.begin(), seen.end());
		if (!convex || std::adjacent_find(seen.begin(), seen.end()) != seen.end())
			continue;
		for (std::size_t f : { A, B })
			for (std::size_t k = 0; k < faces[f].size(); ++k)
				owner.erase({ keyOf(faces[f][k]), keyOf(faces[f][(k + 1) % faces[f].size()]) });
		faces[A] = std::move(merged);
		alive[B] = 0;
		for (std::size_t k = 0; k < faces[A].size(); ++k)
			owner[{ keyOf(faces[A][k]), keyOf(faces[A][(k + 1) % faces[A].size()]) }] = A;
	}
	for (std::size_t f = 0; f < faces.size(); ++f)
		if (alive[f])
			out.push_back({ std::move(faces[f]), part });
	return true;
}

using Clock = std::chrono::steady_clock;

// q * v * conj(q), q = (w, x, y, z) normalised; conj = true rotates by the inverse.
P3 rotate(const std::array<double, 4>& q, const P3& v, bool conj)
{
	const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
	const double w = q[0] / n, s = conj ? -1.0 : 1.0;
	const double x = s * q[1] / n, y = s * q[2] / n, z = s * q[3] / n;
	// v + 2 w (u x v) + 2 u x (u x v), u = (x, y, z)
	const double cx = y * v[2] - z * v[1], cy = z * v[0] - x * v[2], cz = x * v[1] - y * v[0];
	return { v[0] + 2.0 * (w * cx + y * cz - z * cy), v[1] + 2.0 * (w * cy + z * cx - x * cz), v[2] + 2.0 * (w * cz + x * cy - y * cx) };
}

} // namespace

native::Mesh generate(const Input& input, const Settings& cfg, Report* report)
{
	if (input.pose)
	{
		// shapes in the building's frame: generate there, the mesh (and the narrow cut's triangles) out into the world; the seed comes in
		const Input::Pose& pose = *input.pose;
		const auto toWorld = [&](const P3& p)
		{
			const P3 r = rotate(pose.rotation, p, false);
			return P3{ r[0] + pose.origin[0], r[1] + pose.origin[1], r[2] + pose.origin[2] };
		};
		Input in = input;
		in.pose.reset();
		if (input.seed)
			in.seed = rotate(pose.rotation, { (*input.seed)[0] - pose.origin[0], (*input.seed)[1] - pose.origin[1], (*input.seed)[2] - pose.origin[2] }, true);
		native::Mesh mesh = generate(in, cfg, report);
		for (P3& v : mesh.vertices)
			v = toWorld(v);
		if (report != nullptr)
			for (auto& tri : report->narrowQuads)
				for (P3& v : tri)
					v = toWorld(v);
		return mesh;
	}
	Report local;
	Report& rep = report != nullptr ? *report : local;
	rep = Report{};
	const Grid grid{ 1.0 / cfg.lattice };
	auto stageStart = Clock::now();
	const auto stage = [&](const char* name)
	{
		const auto now = Clock::now();
		rep.stages.push_back({ name, std::chrono::duration<double, std::milli>(now - stageStart).count() });
		stageStart = now;
	};

	std::vector<Solid> solids;
	for (const kworld::WorldShape* s : input.shapes)
		if (s != nullptr && s->use != kworld::NavUse::Door && s->indices.size() >= 3)
			solids.push_back(makeSolid(*s));

	// 1. walkable pieces, grouped into surfaces (one plane each, against the first piece's plane)
	std::vector<Piece> pieces;
	for (std::size_t si = 0; si < solids.size(); ++si)
		if (solids[si].shape->use == kworld::NavUse::Walkable)
		{
			++rep.walkableShapes;
			shapePieces(solids[si], si, cfg, pieces);
		}
	rep.pieces = pieces.size();
	std::vector<Surface> surfaces;
	for (std::size_t pi = 0; pi < pieces.size(); ++pi)
	{
		const Piece& piece = pieces[pi];
		Surface* home = nullptr;
		for (Surface& s : surfaces)
		{
			bool near = true;
			for (const P2& v : piece.poly)
				near = near && std::fabs(s.plane.at(v[0], v[1]) - piece.plane.at(v[0], v[1])) <= cfg.ownerTolerance;
			if (near)
			{
				home = &s;
				break;
			}
		}
		if (home == nullptr)
		{
			home = &surfaces.emplace_back();
			home->plane = piece.plane;
		}
		home->pieces.push_back(pi);
		home->shape = std::min(home->shape, piece.shape);
		for (const P2& v : piece.poly)
			for (std::size_t k = 0; k < 2; ++k)
			{
				home->bmin[k] = std::min(home->bmin[k], v[k]);
				home->bmax[k] = std::max(home->bmax[k], v[k]);
			}
	}
	stage("pieces");

	// 2. regions: the surface's pieces minus every solid's footprint on its plane (a convex solid never cuts its own top, a triangle of an open
	// shape never cuts the surface it is part of)
	for (Surface& s : surfaces)
	{
		c2::Paths64 own;
		std::vector<std::size_t> ownSolids;
		std::map<std::size_t, std::vector<std::size_t>> ownTriangles;
		for (const std::size_t pi : s.pieces)
		{
			own.push_back(grid.path(pieces[pi].poly));
			if (pieces[pi].triangle == SIZE_MAX)
				ownSolids.push_back(pieces[pi].shape);
			else
				ownTriangles[pieces[pi].shape].push_back(pieces[pi].triangle);
		}
		for (auto& [shape, tris] : ownTriangles)
			std::sort(tris.begin(), tris.end());
		std::vector<Poly> cutters;
		for (std::size_t sj = 0; sj < solids.size(); ++sj)
		{
			if (std::find(ownSolids.begin(), ownSolids.end(), sj) != ownSolids.end())
				continue;
			const auto skip = ownTriangles.find(sj);
			footprints(solids[sj], s.plane, cfg.cutBase, cfg.characterHeight, s.bmin, s.bmax, cfg.barrierHalfWidth, cutters,
				skip != ownTriangles.end() ? &skip->second : nullptr);
		}
		c2::Paths64 cut;
		for (const Poly& c : cutters)
			cut.push_back(grid.path(c));
		s.region = c2::Difference(c2::Union(own, c2::FillRule::NonZero), c2::Union(cut, c2::FillRule::NonZero), c2::FillRule::NonZero);
	}
	stage("cut");

	// 3. ownership where two surfaces cross (within ownerTolerance of each other): the lower index keeps the strip; then the weld
	for (std::size_t q = 0; q < surfaces.size(); ++q)
	{
		for (std::size_t p = 0; p < q && !surfaces[q].region.empty(); ++p)
		{
			const Surface &P = surfaces[p], &Q = surfaces[q];
			const P2 lo{ std::max(P.bmin[0], Q.bmin[0]), std::max(P.bmin[1], Q.bmin[1]) }, hi{ std::min(P.bmax[0], Q.bmax[0]), std::min(P.bmax[1], Q.bmax[1]) };
			if (P.region.empty() || lo[0] >= hi[0] || lo[1] >= hi[1])
				continue;
			const Poly band = strip(box(lo, hi), P.plane.a - Q.plane.a, P.plane.b - Q.plane.b, P.plane.c - Q.plane.c, -cfg.ownerTolerance, cfg.ownerTolerance);
			if (band.size() < 3)
				continue;
			const c2::Paths64 owned = c2::Intersect(P.region, { grid.path(band) }, c2::FillRule::NonZero);
			if (!owned.empty())
				surfaces[q].region = c2::Difference(surfaces[q].region, owned, c2::FillRule::NonZero);
		}
	}
	for (Surface& s : surfaces)
		s.region = tidy(weld(s.region, cfg.weld * grid.s, true));
	stage("own+weld");

	// 4. links between the regions (whole building, before the hull): what is not linked is a wall
	std::vector<Seg> regionSegs;
	for (std::size_t si = 0; si < surfaces.size(); ++si)
		for (const c2::Path64& path : surfaces[si].region)
			for (std::size_t k = 0; k < path.size(); ++k)
				regionSegs.push_back({ grid.from(path[k]), grid.from(path[(k + 1) % path.size()]), &surfaces[si].plane, si });
	std::vector<Link> regionLinks = matchEdges(regionSegs, solids, cfg, nullptr);
	if (cfg.minPassage > 0.0)
	{
		// A passage between two surfaces is as wide as its run of links: links of one surface pair whose ends touch chain into a run, and a run
		// shorter than minPassage is a wall (user 2026-11-24, a panel beside a ramp linked over 1.2 u at its corner: "It's only a hop up from the
		// extreme corner. It should be rejected because the far end is too much of a hop"). The same rule as the narrow cut: no passage narrower
		// than a character.
		struct End
		{
			std::pair<std::size_t, std::size_t> pair; // (lower surface, higher surface)
			P2 a, b; // the link's ends on the lower surface's boundary
		};
		const auto at = [](const Seg& s, double t)
		{
			const double len = std::hypot(s.b[0] - s.a[0], s.b[1] - s.a[1]);
			return P2{ s.a[0] + (s.b[0] - s.a[0]) * t / len, s.a[1] + (s.b[1] - s.a[1]) * t / len };
		};
		std::vector<End> ends;
		for (const Link& l : regionLinks)
		{
			const Seg &A = regionSegs[l.i], &B = regionSegs[l.j];
			if (A.surface <= B.surface)
				ends.push_back({ { A.surface, B.surface }, at(A, l.t0), at(A, l.t1) });
			else
				ends.push_back({ { B.surface, A.surface }, at(B, l.u0), at(B, l.u1) });
		}
		std::vector<std::size_t> run(ends.size());
		for (std::size_t k = 0; k < run.size(); ++k)
			run[k] = k;
		const std::function<std::size_t(std::size_t)> root = [&](std::size_t k) { return run[k] == k ? k : run[k] = root(run[k]); };
		const double touch = 2.0 * cfg.weld;
		const auto near = [&](const P2& p, const P2& q) { return std::hypot(p[0] - q[0], p[1] - q[1]) <= touch; };
		for (std::size_t k = 0; k < ends.size(); ++k)
			for (std::size_t m = k + 1; m < ends.size(); ++m)
				if (ends[k].pair == ends[m].pair
					&& (near(ends[k].a, ends[m].a) || near(ends[k].a, ends[m].b) || near(ends[k].b, ends[m].a) || near(ends[k].b, ends[m].b)))
					run[root(k)] = root(m);
		std::vector<double> length(ends.size(), 0.0);
		for (std::size_t k = 0; k < ends.size(); ++k)
			length[root(k)] += std::hypot(ends[k].b[0] - ends[k].a[0], ends[k].b[1] - ends[k].a[1]);
		std::vector<Link> kept;
		for (std::size_t k = 0; k < ends.size(); ++k)
			if (length[root(k)] >= cfg.minPassage)
				kept.push_back(regionLinks[k]);
			else
				++rep.shortLinks;
		regionLinks = std::move(kept);
	}
	// per surface pair, the linked stretches of the first's boundary (start -> end along it), for the face links
	struct Stretch
	{
		P2 a, b;
	};
	std::map<std::pair<std::size_t, std::size_t>, std::vector<Stretch>> linkedStretches;
	std::vector<Intervals> linkedOn(regionSegs.size());
	for (const Link& l : regionLinks)
	{
		const Seg &A = regionSegs[l.i], &B = regionSegs[l.j];
		const auto at = [](const Seg& s, double t)
		{
			const double len = std::hypot(s.b[0] - s.a[0], s.b[1] - s.a[1]);
			return P2{ s.a[0] + (s.b[0] - s.a[0]) * t / len, s.a[1] + (s.b[1] - s.a[1]) * t / len };
		};
		linkedStretches[{ A.surface, B.surface }].push_back({ at(A, l.t0), at(A, l.t1) });
		linkedStretches[{ B.surface, A.surface }].push_back({ at(B, l.u0), at(B, l.u1) });
		linkedOn[l.i].push_back({ l.t0, l.t1 });
		linkedOn[l.j].push_back({ l.u0, l.u1 });
	}
	stage("region links");

	// 5. the narrow cut: an opening of the floor by a disc of minPassage. Centres: each region minus its walls grown round by r (the walls of every
	// surface within characterHeight in height there: a disc on a stair tread meets the next tread's side wall). The kept floor: the centres of
	// every surface within characterHeight, grown back by r with mitred corners (a room corner comes back whole, a tip sharper than cornerMitre is
	// squared off), cut to the region. A link is not a wall: a tread is narrow only across the stair.
	if (cfg.minPassage > 0.0)
	{
		const double r = 0.5 * cfg.minPassage;
		struct Wall
		{
			c2::Path64 path;
			std::size_t surface;
			P2 mid;
		};
		std::vector<Wall> walls;
		for (std::size_t i = 0; i < regionSegs.size(); ++i)
		{
			const Seg& s = regionSegs[i];
			const double len = std::hypot(s.b[0] - s.a[0], s.b[1] - s.a[1]);
			Intervals open{ { 0.0, len } };
			for (const Interval& l : linkedOn[i])
				subtractInterval(open, l[0], l[1]);
			for (const Interval& w : open)
			{
				if (w[1] - w[0] < 2.0 / grid.s)
					continue;
				const P2 a{ s.a[0] + (s.b[0] - s.a[0]) * w[0] / len, s.a[1] + (s.b[1] - s.a[1]) * w[0] / len };
				const P2 b{ s.a[0] + (s.b[0] - s.a[0]) * w[1] / len, s.a[1] + (s.b[1] - s.a[1]) * w[1] / len };
				walls.push_back({ { grid.to(a), grid.to(b) }, s.surface, { 0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1]) } });
			}
		}
		const auto nearBox = [](const Surface& s, const P2& p, double margin)
		{
			return p[0] >= s.bmin[0] - margin && p[0] <= s.bmax[0] + margin && p[1] >= s.bmin[1] - margin && p[1] <= s.bmax[1] + margin;
		};
		std::vector<c2::Paths64> grown(surfaces.size());
		for (std::size_t q = 0; q < surfaces.size(); ++q)
		{
			const Surface& Q = surfaces[q];
			if (Q.region.empty())
				continue;
			c2::Paths64 mine;
			for (const Wall& w : walls)
			{
				const double halfLength = 0.5 * std::hypot(static_cast<double>(w.path[1].x - w.path[0].x), static_cast<double>(w.path[1].y - w.path[0].y)) / grid.s;
				if (!nearBox(Q, w.mid, r + halfLength))
					continue;
				if (std::fabs(surfaces[w.surface].plane.at(w.mid[0], w.mid[1]) - Q.plane.at(w.mid[0], w.mid[1])) >= cfg.characterHeight)
					continue;
				mine.push_back(w.path);
			}
			const c2::Paths64 reach = mine.empty() ? c2::Paths64{} : c2::InflatePaths(mine, r * grid.s, c2::JoinType::Round, c2::EndType::Round, 2.0, cfg.weld * grid.s);
			const c2::Paths64 centres = c2::Difference(Q.region, reach, c2::FillRule::NonZero);
			grown[q] = inflate(centres, r * grid.s, c2::JoinType::Miter, cfg.cornerMitre);
		}
		for (std::size_t p = 0; p < surfaces.size(); ++p)
		{
			Surface& P = surfaces[p];
			if (P.region.empty())
				continue;
			c2::Paths64 cover = grown[p];
			for (std::size_t s = 0; s < surfaces.size(); ++s)
			{
				const Surface& S = surfaces[s];
				if (s == p || grown[s].empty() || S.bmax[0] + r < P.bmin[0] || S.bmin[0] - r > P.bmax[0] || S.bmax[1] + r < P.bmin[1] || S.bmin[1] - r > P.bmax[1])
					continue;
				const Poly band = strip(box(P.bmin, P.bmax), S.plane.a - P.plane.a, S.plane.b - P.plane.b, S.plane.c - P.plane.c, -cfg.characterHeight, cfg.characterHeight);
				if (band.size() < 3)
					continue;
				// a neighbour's floor reaches into this one only across a link between them, at most r past it (a tread is narrow only across
				// the stair); elsewhere an overlap in xz is two levels stacked, and the one above must not keep a sliver below it (user
				// 2026-11-24, Heft roof face at -91.8, 72.3, 17.8 kept by a level 10.6 u above)
				const auto stretches = linkedStretches.find({ p, s });
				if (stretches == linkedStretches.end())
					continue;
				c2::Paths64 lines;
				for (const Stretch& st : stretches->second)
					lines.push_back(grid.path({ st.a, st.b }));
				const c2::Paths64 nearLink = c2::InflatePaths(lines, r * grid.s, c2::JoinType::Round, c2::EndType::Round, 2.0, cfg.weld * grid.s);
				const c2::Paths64 part = c2::Intersect(c2::Intersect(grown[s], { grid.path(band) }, c2::FillRule::NonZero), nearLink, c2::FillRule::NonZero);
				if (!part.empty())
					cover = c2::Union(cover, part, c2::FillRule::NonZero);
			}
			const c2::Paths64 kept = tidy(weld(c2::Intersect(P.region, cover, c2::FillRule::NonZero), cfg.weld * grid.s, false));
			const c2::Paths64 removed = c2::Difference(P.region, kept, c2::FillRule::NonZero);
			for (const c2::Path64& path : removed)
			{
				const double a = c2::Area(path) / (grid.s * grid.s);
				rep.narrowArea += a; // a hole counts negative
				if (a > 0.0)
					++rep.narrowCuts;
			}
			// the removed floor as triangles: outlines can be non-convex and hold holes, which a viewer cannot fill as drawn. Viewer output only:
			// when the whole set fails, one polygon (with its holes) at a time; one that still fails goes out as its outline
			c2::Paths64 tris;
			if (!removed.empty() && c2::Triangulate(removed, tris, false) != c2::TriangulateResult::success)
			{
				tris.clear();
				c2::PolyTree64 tree;
				c2::Clipper64 clipper;
				clipper.AddSubject(removed);
				clipper.Execute(c2::ClipType::Union, c2::FillRule::NonZero, tree);
				std::function<void(const c2::PolyPath64&)> walk = [&](const c2::PolyPath64& node)
				{
					for (const auto& child : node)
					{
						c2::Paths64 one{ child->Polygon() }, part;
						for (const auto& hole : *child)
							one.push_back(hole->Polygon());
						if (c2::Triangulate(one, part, false) == c2::TriangulateResult::success)
							tris.insert(tris.end(), part.begin(), part.end());
						else
						{
							++rep.narrowOutlines;
							tris.push_back(child->Polygon());
						}
						for (const auto& hole : *child)
							walk(*hole);
					}
				};
				walk(tree);
			}
			for (const c2::Path64& path : tris)
			{
				std::vector<std::array<double, 3>> ring;
				for (const c2::Point64& v : path)
				{
					const P2 q = grid.from(v);
					ring.push_back({ q[0], P.plane.at(q[0], q[1]), q[1] });
				}
				rep.narrowQuads.push_back(std::move(ring));
			}
			P.region = kept;
		}
	}
	stage("narrow");

	// 6. sides and doors: the hull's section splits each region exactly (zone = region minus interior); a door's volume paints its part
	std::vector<std::array<float, 4>> hullPlanes;
	{
		std::vector<kworld::WorldShape> mask;
		for (const kworld::WorldShape* m : input.mask)
			if (m != nullptr)
				mask.push_back(*m);
		if (!mask.empty())
			hullPlanes = kworld::interiorMaskHull(mask);
	}
	std::vector<std::pair<const kworld::WorldShape*, std::vector<std::array<float, 4>>>> doors;
	if (input.paintDoors)
		for (const kworld::WorldShape* d : input.shapes)
			if (d != nullptr && d->use == kworld::NavUse::Door && d->indices.size() >= 3)
			{
				std::vector<std::array<float, 4>> volume = kworld::interiorMaskHull({ *d });
				if (!volume.empty())
					doors.push_back({ d, std::move(volume) });
			}
	std::vector<Part> parts;
	for (std::size_t si = 0; si < surfaces.size(); ++si)
	{
		const Surface& S = surfaces[si];
		if (S.region.empty())
			continue;
		const Poly area0 = box({ S.bmin[0] - 10.0, S.bmin[1] - 10.0 }, { S.bmax[0] + 10.0, S.bmax[1] + 10.0 });
		c2::Paths64 inside = S.region, outside;
		if (!hullPlanes.empty())
		{
			const Poly h = section(area0, S.plane, hullPlanes);
			inside = h.size() >= 3 ? tidy(c2::Intersect(S.region, { grid.path(h) }, c2::FillRule::NonZero)) : c2::Paths64{};
			outside = c2::Difference(S.region, inside, c2::FillRule::NonZero);
		}
		for (auto& [paths, side] : { std::pair{ inside, Side::Interior }, std::pair{ outside, Side::Zone } })
		{
			c2::Paths64 rest = paths;
			for (const auto& [shape, volume] : doors)
			{
				if (rest.empty())
					break;
				const Poly v = section(area0, S.plane, volume);
				if (v.size() < 3)
					continue;
				const c2::Paths64 painted = tidy(c2::Intersect(rest, { grid.path(v) }, c2::FillRule::NonZero));
				if (c2::Area(painted) / (grid.s * grid.s) <= cfg.minPieceArea)
					continue;
				rest = c2::Difference(rest, painted, c2::FillRule::NonZero);
				parts.push_back({ si, side, 4, shape->door, painted }); // the baked state is open (the capture's faces); the runtime door book repaints by the key
			}
			if (!rest.empty())
				parts.push_back({ si, side, 1, 0, rest });
		}
	}
	stage("sides");

	// 7. convex faces per part; straight corners no other face of the surface uses are dropped; a corner of one face lying exactly on another's
	// edge (same surface) becomes a corner there too
	std::vector<IFace> iFaces;
	for (std::size_t p = 0; p < parts.size(); ++p)
		if (!decompose(parts[p].paths, p, iFaces))
		{
			// one polygon (with its holes) at a time
			c2::PolyTree64 tree;
			c2::Clipper64 clipper;
			clipper.AddSubject(parts[p].paths);
			clipper.Execute(c2::ClipType::Union, c2::FillRule::NonZero, tree);
			std::function<void(const c2::PolyPath64&)> walk = [&](const c2::PolyPath64& node)
			{
				for (const auto& child : node)
				{
					c2::Paths64 one{ child->Polygon() };
					for (const auto& hole : *child)
						one.push_back(hole->Polygon());
					if (!decompose(one, p, iFaces))
						++rep.triangulationFailures;
					for (const auto& hole : *child)
						walk(*hole);
				}
			};
			walk(tree);
		}
	{
		std::map<std::pair<std::size_t, Key>, std::size_t> uses;
		for (const IFace& f : iFaces)
			for (const c2::Point64& v : f.ring)
				++uses[{ parts[f.part].surface, keyOf(v) }];
		for (IFace& f : iFaces)
			for (std::size_t k = 0; k < f.ring.size() && f.ring.size() > 3;)
			{
				const std::size_t n = f.ring.size();
				if (cross64(f.ring[(k + n - 1) % n], f.ring[k], f.ring[(k + 1) % n]) == 0 && uses[{ parts[f.part].surface, keyOf(f.ring[k]) }] == 1)
					f.ring.erase(f.ring.begin() + static_cast<std::ptrdiff_t>(k));
				else
					++k;
			}
		std::map<std::size_t, std::vector<c2::Point64>> corners;
		for (const IFace& f : iFaces)
			for (const c2::Point64& v : f.ring)
				corners[parts[f.part].surface].push_back(v);
		for (auto& [s, list] : corners)
		{
			std::sort(list.begin(), list.end(), [](const c2::Point64& x, const c2::Point64& y) { return keyOf(x) < keyOf(y); });
			list.erase(std::unique(list.begin(), list.end()), list.end());
		}
		for (IFace& f : iFaces)
		{
			const std::vector<c2::Point64>& list = corners[parts[f.part].surface];
			c2::Path64 ring;
			for (std::size_t k = 0; k < f.ring.size(); ++k)
			{
				const c2::Point64 a = f.ring[k], b = f.ring[(k + 1) % f.ring.size()];
				ring.push_back(a);
				std::vector<std::pair<std::int64_t, c2::Point64>> on;
				const std::int64_t len2 = (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
				const auto first = std::lower_bound(list.begin(), list.end(), c2::Point64(std::min(a.x, b.x), std::numeric_limits<std::int64_t>::min()),
					[](const c2::Point64& x, const c2::Point64& y) { return keyOf(x) < keyOf(y); });
				for (auto it = first; it != list.end() && it->x <= std::max(a.x, b.x); ++it)
				{
					const c2::Point64& v = *it;
					if (v == a || v == b || v.y < std::min(a.y, b.y) || v.y > std::max(a.y, b.y) || cross64(a, b, v) != 0)
						continue;
					const std::int64_t t = (v.x - a.x) * (b.x - a.x) + (v.y - a.y) * (b.y - a.y);
					if (t > 0 && t < len2)
						on.push_back({ t, v });
				}
				std::sort(on.begin(), on.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
				for (const auto& [t, v] : on)
					ring.push_back(v);
			}
			f.ring = std::move(ring);
		}
	}
	stage("faces");

	// 8. face links: within a surface the exact shared edges; across surfaces (and across a crack in one) the matcher, only on stretches the regions
	// linked, so nothing links across floor the narrow cut removed
	std::vector<Face> faces;
	for (const IFace& f : iFaces)
	{
		const Part& part = parts[f.part];
		Face face;
		face.plane = surfaces[part.surface].plane;
		face.data = part.data;
		face.door = part.door;
		face.shape = surfaces[part.surface].shape;
		face.surface = part.surface;
		face.side = part.side;
		for (const c2::Point64& v : f.ring)
			face.ring.push_back({ grid.from(v), 0 });
		faces.push_back(std::move(face));
	}
	struct Attach
	{
		double t0, t1;
		std::size_t link; // index into the pairs below
		int end; // 0 or 1
	};
	std::vector<std::vector<std::vector<Attach>>> attach(faces.size());
	for (std::size_t f = 0; f < faces.size(); ++f)
		attach[f].resize(faces[f].ring.size());
	std::vector<std::array<int, 2>> pairUids; // per link: the uid of each end's sub-edge
	{
		std::map<std::pair<std::size_t, std::pair<Key, Key>>, std::pair<std::size_t, std::size_t>> exact;
		for (std::size_t f = 0; f < iFaces.size(); ++f)
			for (std::size_t k = 0; k < iFaces[f].ring.size(); ++k)
				exact[{ faces[f].surface, { keyOf(iFaces[f].ring[k]), keyOf(iFaces[f].ring[(k + 1) % iFaces[f].ring.size()]) } }] = { f, k };
		std::vector<std::vector<char>> paired(faces.size());
		for (std::size_t f = 0; f < faces.size(); ++f)
			paired[f].assign(faces[f].ring.size(), 0);
		for (std::size_t f = 0; f < iFaces.size(); ++f)
			for (std::size_t k = 0; k < iFaces[f].ring.size(); ++k)
			{
				if (paired[f][k])
					continue;
				const auto it = exact.find({ faces[f].surface, { keyOf(iFaces[f].ring[(k + 1) % iFaces[f].ring.size()]), keyOf(iFaces[f].ring[k]) } });
				if (it == exact.end() || it->second.first == f)
					continue;
				const auto [g, h] = it->second;
				const double len = std::hypot(edgeEnd(faces[f], k)[0] - faces[f].ring[k].a[0], edgeEnd(faces[f], k)[1] - faces[f].ring[k].a[1]);
				attach[f][k].push_back({ 0.0, len, pairUids.size(), 0 });
				attach[g][h].push_back({ 0.0, len, pairUids.size(), 1 });
				pairUids.push_back({ 0, 0 });
				paired[f][k] = paired[g][h] = 1;
			}
		std::vector<Seg> segs;
		std::vector<std::pair<std::size_t, std::size_t>> segEdge;
		for (std::size_t f = 0; f < faces.size(); ++f)
			for (std::size_t k = 0; k < faces[f].ring.size(); ++k)
				if (!paired[f][k])
				{
					segs.push_back({ faces[f].ring[k].a, edgeEnd(faces[f], k), &faces[f].plane, faces[f].surface });
					segEdge.push_back({ f, k });
				}
		const double onLine = 2.0 / grid.s;
		const auto allowed = [&](std::size_t i, std::size_t j)
		{
			Intervals out;
			const auto it = linkedStretches.find({ segs[i].surface, segs[j].surface });
			if (it == linkedStretches.end())
				return out;
			const Seg& s = segs[i];
			const double len = std::hypot(s.b[0] - s.a[0], s.b[1] - s.a[1]);
			const double ux = (s.b[0] - s.a[0]) / len, uz = (s.b[1] - s.a[1]) / len;
			for (const Stretch& st : it->second)
			{
				const double da = (st.a[0] - s.a[0]) * uz - (st.a[1] - s.a[1]) * ux, db = (st.b[0] - s.a[0]) * uz - (st.b[1] - s.a[1]) * ux;
				const double ta = (st.a[0] - s.a[0]) * ux + (st.a[1] - s.a[1]) * uz, tb = (st.b[0] - s.a[0]) * ux + (st.b[1] - s.a[1]) * uz;
				if (std::fabs(da) > onLine || std::fabs(db) > onLine || tb <= ta)
					continue;
				const double lo = std::max(0.0, ta), hi = std::min(len, tb);
				if (hi > lo)
					out.push_back({ lo, hi });
			}
			std::sort(out.begin(), out.end());
			return out;
		};
		for (const Link& l : matchEdges(segs, solids, cfg, allowed))
		{
			const auto [f, k] = segEdge[l.i];
			const auto [g, h] = segEdge[l.j];
			attach[f][k].push_back({ l.t0, l.t1, pairUids.size(), 0 });
			attach[g][h].push_back({ l.u0, l.u1, pairUids.size(), 1 });
			pairUids.push_back({ 0, 0 });
		}
	}
	// split each edge at its links' ends (ends closer than two grid steps are one), each piece a new uid
	int nextUid = 1;
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		std::vector<FaceEdge> ring;
		for (std::size_t k = 0; k < faces[f].ring.size(); ++k)
		{
			const P2 a = faces[f].ring[k].a, b = edgeEnd(faces[f], k);
			const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
			std::vector<double> cuts{ 0.0, len };
			for (const Attach& at : attach[f][k])
				for (const double t : { at.t0, at.t1 })
					cuts.push_back(std::clamp(t, 0.0, len));
			std::sort(cuts.begin(), cuts.end());
			std::vector<double> merged{ 0.0 };
			for (const double t : cuts)
				if (t - merged.back() >= 2.0 / grid.s)
					merged.push_back(t);
			if (len - merged.back() < 2.0 / grid.s && merged.size() > 1)
				merged.back() = len;
			else if (merged.back() != len)
				merged.push_back(len);
			for (std::size_t c = 0; c + 1 < merged.size(); ++c)
			{
				const int uid = nextUid++;
				ring.push_back({ { a[0] + (b[0] - a[0]) * merged[c] / len, a[1] + (b[1] - a[1]) * merged[c] / len }, uid });
				const double mid = 0.5 * (merged[c] + merged[c + 1]);
				for (const Attach& at : attach[f][k])
					if (mid > at.t0 && mid < at.t1)
						pairUids[at.link][static_cast<std::size_t>(at.end)] = uid;
			}
		}
		faces[f].ring = std::move(ring);
	}
	std::map<int, std::size_t> faceByUid;
	for (std::size_t f = 0; f < faces.size(); ++f)
		for (const FaceEdge& e : faces[f].ring)
			faceByUid[e.uid] = f;
	std::map<int, int> partner, seam; // same side / interior-zone
	for (const auto& [u, v] : pairUids)
	{
		if (u == 0 || v == 0 || u == v)
			continue;
		auto& to = faces[faceByUid[u]].side == faces[faceByUid[v]].side ? partner : seam;
		to[u] = v;
		to[v] = u;
	}
	stage("face links");

	// 9a. islands before jumping (user 2026-11-24: "identify the islands before jumping. The final merge counts the jump, but the total island size
	// can be used to discard an island"): faces joined by edges that coincide (xz within 2 weld, height within cutBase: a shared edge, the seam, a
	// ramp meeting a floor flush), not by a step or a gap. An island under minIslandArea (both sides) goes before the flood, and no jump crosses it.
	std::vector<char> tinyIsland(faces.size(), 0);
	{
		const auto ends = [&](int uid)
		{
			const Face& face = faces[faceByUid[uid]];
			std::size_t k = 0;
			while (face.ring[k].uid != uid)
				++k;
			const P2 a = face.ring[k].a, b = face.ring[(k + 1) % face.ring.size()].a;
			return std::array<std::array<double, 3>, 2>{ { { a[0], face.plane.at(a[0], a[1]), a[1] }, { b[0], face.plane.at(b[0], b[1]), b[1] } } };
		};
		const auto same = [&](const std::array<double, 3>& p, const std::array<double, 3>& q)
		{
			return std::hypot(p[0] - q[0], p[2] - q[2]) <= 2.0 * cfg.weld && std::fabs(p[1] - q[1]) <= cfg.cutBase;
		};
		const auto joined = [&](int u, int v)
		{
			const auto e = ends(u), o = ends(v);
			return same(e[0], o[1]) && same(e[1], o[0]);
		};
		std::vector<std::size_t> island(faces.size(), SIZE_MAX);
		std::vector<double> islandArea;
		for (std::size_t f = 0; f < faces.size(); ++f)
		{
			if (island[f] != SIZE_MAX)
				continue;
			const std::size_t c = islandArea.size();
			islandArea.push_back(0.0);
			std::vector<std::size_t> stack{ f };
			island[f] = c;
			while (!stack.empty())
			{
				const std::size_t cur = stack.back();
				stack.pop_back();
				islandArea[c] += faceArea(faces[cur]);
				for (const FaceEdge& e : faces[cur].ring)
					for (const std::map<int, int>* links : { &partner, &seam })
					{
						const auto it = links->find(e.uid);
						if (it == links->end() || !joined(e.uid, it->second))
							continue;
						const std::size_t other = faceByUid[it->second];
						if (island[other] == SIZE_MAX)
						{
							island[other] = c;
							stack.push_back(other);
						}
					}
			}
		}
		for (std::size_t f = 0; f < faces.size(); ++f)
			if (islandArea[island[f]] < cfg.minIslandArea)
			{
				tinyIsland[f] = 1;
				rep.tinyIslandArea += faceArea(faces[f]);
			}
		for (std::size_t c = 0; c < islandArea.size(); ++c)
			if (islandArea[c] < cfg.minIslandArea)
				++rep.tinyIslands;
	}

	// 9. one graph over both sides (same-side links and the seam): the component holding the lowest walkable piece (or the seed) stays; every other component
	// goes, interior and zone side alike (user 2026-11-24: "Any navmesh tile that can be reached from this point is the walkable area"). The
	// building's only way in is through its own faces (the entrance stair meets the ground), so nothing outside the seed's component is reachable.
	std::vector<std::size_t> component(faces.size(), SIZE_MAX);
	std::vector<std::array<double, 2>> componentArea; // per side: interior, zone
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		if (component[f] != SIZE_MAX || tinyIsland[f])
			continue;
		const std::size_t c = componentArea.size();
		componentArea.push_back({ 0.0, 0.0 });
		std::vector<std::size_t> stack{ f };
		component[f] = c;
		while (!stack.empty())
		{
			const std::size_t cur = stack.back();
			stack.pop_back();
			componentArea[c][faces[cur].side == Side::Interior ? 0 : 1] += faceArea(faces[cur]);
			for (const FaceEdge& e : faces[cur].ring)
				for (const std::map<int, int>* links : { &partner, &seam })
				{
					const auto it = links->find(e.uid);
					if (it == links->end())
						continue;
					const std::size_t other = faceByUid[it->second];
					if (component[other] == SIZE_MAX && !tinyIsland[other])
					{
						component[other] = c;
						stack.push_back(other);
					}
				}
		}
	}
	// The flood starts at the lowest walkable piece (user 2026-11-24: "All buildings are designed to have a low point of entry, and are designed
	// to sink into the ground. So it's literally the lowest walkable piece that counts"; a centre or origin point has no floor under it in the
	// Deadlands warehouse). An explicit seed overrides (tests).
	std::vector<std::size_t> seedFaces;
	if (input.seed)
	{
		// the lowest face (either side) containing the seed in xz; none contains it (a table stands there): the nearest
		double bestDistance = 1e30, bestY = 1e30;
		std::size_t best = SIZE_MAX;
		const P2 q{ (*input.seed)[0], (*input.seed)[2] };
		for (std::size_t f = 0; f < faces.size(); ++f)
		{
			const Poly xz = faceXz(faces[f]);
			bool inside = true;
			double distance = 1e30;
			for (std::size_t k = 0; k < xz.size(); ++k)
			{
				const P2 &a = xz[k], &b = xz[(k + 1) % xz.size()];
				inside = inside && cross2(a, b, q) >= 0.0;
				const double length2 = (b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]);
				const double t = length2 > 0.0 ? std::clamp(((q[0] - a[0]) * (b[0] - a[0]) + (q[1] - a[1]) * (b[1] - a[1])) / length2, 0.0, 1.0) : 0.0;
				distance = std::min(distance, std::hypot(q[0] - a[0] - t * (b[0] - a[0]), q[1] - a[1] - t * (b[1] - a[1])));
			}
			if (tinyIsland[f])
				continue;
			const double y = faces[f].plane.at(q[0], q[1]);
			if (inside)
				distance = 0.0;
			if (distance < bestDistance - 1e-9 || (distance < bestDistance + 1e-9 && y < bestY))
			{
				bestDistance = distance;
				bestY = y;
				best = f;
			}
		}
		if (best != SIZE_MAX)
			seedFaces.push_back(best);
	} else {
		// the face with the lowest corner, either side
		double lowest = 1e30;
		std::size_t best = SIZE_MAX;
		for (std::size_t f = 0; f < faces.size(); ++f)
			for (const FaceEdge& e : faces[f].ring)
				if (!tinyIsland[f] && faces[f].plane.at(e.a[0], e.a[1]) < lowest)
				{
					lowest = faces[f].plane.at(e.a[0], e.a[1]);
					best = f;
				}
		if (best != SIZE_MAX)
			seedFaces.push_back(best);
	}
	std::vector<char> seeded(componentArea.size(), 0);
	for (const std::size_t f : seedFaces)
		seeded[component[f]] = 1;
	// Sanity check (user 2026-11-24: "After flood check, warn if it's less than 60% of all the face-up mesh tiles you're working with. Just by
	// pure numbers, the biggest should win"; then: "I pulled 60% out of my ass"): no share threshold (the Deadlands warehouse is right at 62 %, its
	// deck sealed off); the warning is a seeded component that is not the largest. The share is reported. A warning only; the seed still decides.
	{
		double total = 0.0, reached = 0.0, largest = 0.0, largestSeeded = 0.0;
		for (std::size_t c = 0; c < componentArea.size(); ++c)
		{
			const double a = componentArea[c][0] + componentArea[c][1];
			total += a;
			largest = std::max(largest, a);
			if (seeded[c])
			{
				reached += a;
				largestSeeded = std::max(largestSeeded, a);
			}
		}
		rep.reachedShare = total > 0.0 ? reached / total : 1.0;
		rep.seedSuspect = largestSeeded < largest;
	}
	std::vector<char> keep(faces.size(), 0);
	for (std::size_t f = 0; f < faces.size(); ++f)
		keep[f] = !tinyIsland[f] && (seeded[component[f]] || cfg.keepIslands) ? 1 : 0;
	if (input.side == Side::Zone && hullPlanes.empty())
		std::fill(keep.begin(), keep.end(), 0);
	const int sideIndex = input.side == Side::Interior ? 0 : 1;
	for (const std::array<double, 2>& area : componentArea)
		if (area[sideIndex] > 0.0)
		{
			++rep.components;
			rep.componentAreas.push_back(area[sideIndex]);
		}
	stage("prune");

	// 10. the native mesh of the asked side
	std::vector<std::size_t> out;
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		if (faces[f].side != input.side)
			continue;
		if (keep[f])
		{
			if (std::find(seedFaces.begin(), seedFaces.end(), f) != seedFaces.end())
				rep.seedFaces.push_back(out.size());
			out.push_back(f);
		}
		else
		{
			++rep.droppedFaces;
			rep.droppedArea += faceArea(faces[f]);
		}
	}
	native::Mesh mesh;
	mesh.info = "surfgen";
	std::map<std::array<std::int64_t, 3>, std::uint32_t> vertexIds;
	std::map<int, std::pair<std::size_t, std::size_t>> edgeOfUid; // uid -> (mesh face, mesh edge)
	for (const std::size_t f : out)
	{
		native::Face face;
		face.startEdge = static_cast<std::uint32_t>(mesh.edges.size());
		face.numEdges = static_cast<std::uint16_t>(faces[f].ring.size());
		face.data = faces[f].data;
		face.door = faces[f].door;
		const auto vertexOf = [&](const P2& p)
		{
			const double y = faces[f].plane.at(p[0], p[1]);
			const std::array<std::int64_t, 3> key{ std::llround(p[0] * grid.s), std::llround(y / 0.01), std::llround(p[1] * grid.s) };
			const auto it = vertexIds.find(key);
			if (it != vertexIds.end())
				return it->second;
			mesh.vertices.push_back({ p[0], y, p[1] });
			return vertexIds[key] = static_cast<std::uint32_t>(mesh.vertices.size() - 1);
		};
		for (std::size_t k = 0; k < faces[f].ring.size(); ++k)
		{
			native::Edge edge;
			const auto s = seam.find(faces[f].ring[k].uid);
			edge.linked = s != seam.end() && keep[faceByUid[s->second]];
			const P2 a = faces[f].ring[k].a, b = edgeEnd(faces[f], k);
			if (edge.linked)
			{
				++rep.seamEdges;
				rep.seamLength += std::hypot(std::hypot(b[0] - a[0], b[1] - a[1]), faces[f].plane.at(b[0], b[1]) - faces[f].plane.at(a[0], a[1]));
			}
			edge.a = vertexOf(a);
			edge.b = vertexOf(b);
			edgeOfUid[faces[f].ring[k].uid] = { mesh.faces.size(), mesh.edges.size() };
			mesh.edges.push_back(edge);
		}
		mesh.faces.push_back(face);
	}
	for (const auto& [uid, other] : partner)
	{
		const auto a = edgeOfUid.find(uid), b = edgeOfUid.find(other);
		if (a == edgeOfUid.end() || b == edgeOfUid.end())
			continue;
		mesh.edges[a->second.second].oppositeEdge = static_cast<std::int32_t>(b->second.second);
		mesh.edges[a->second.second].oppositeFace = static_cast<std::int32_t>(b->second.first);
		rep.matchedEdges += uid < other ? 1 : 0;
	}
	for (const native::Edge& e : mesh.edges)
		rep.boundaryEdges += e.oppositeFace < 0 ? 1 : 0;

	// the invariants: convex, no corner twice, no edge under a grid step; no xz overlap within characterHeight in height beyond the weld (two
	// linked surfaces may share their boundary within a few grid steps: each region's outline was simplified on its own)
	rep.faces = out.size();
	std::vector<Poly> insets;
	for (const std::size_t f : out)
		insets.push_back(inset(faceXz(faces[f]), cfg.weld));
	for (std::size_t i = 0; i < out.size(); ++i)
	{
		const Face& F = faces[out[i]];
		rep.faceShape.push_back(F.shape);
		rep.area += faceArea(F);
		if (F.door != 0)
		{
			++rep.doorFaces;
			rep.doorArea += faceArea(F);
		}
		rep.steepFaces += std::atan(std::hypot(F.plane.a, F.plane.b)) * 180.0 / kPi > 48.0 ? 1.0 : 0.0;
		const Poly xz = faceXz(F);
		bool valid = xz.size() >= 3;
		std::vector<P2> sorted = xz;
		std::sort(sorted.begin(), sorted.end());
		valid = valid && std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
		for (std::size_t k = 0; k < xz.size() && valid; ++k)
		{
			const P2 &p0 = xz[(k + xz.size() - 1) % xz.size()], &p1 = xz[k], &p2 = xz[(k + 1) % xz.size()];
			valid = std::hypot(p2[0] - p1[0], p2[1] - p1[1]) >= 1.0 / grid.s - 1e-9 && cross2(p0, p1, p2) >= -1e-9 * std::hypot(p2[0] - p0[0], p2[1] - p0[1]);
		}
		rep.invalidFaces += valid ? 0 : 1;
		for (std::size_t j = i + 1; j < out.size(); ++j)
		{
			if (insets[i].size() < 3 || insets[j].size() < 3)
				continue;
			const Poly over = intersect(insets[i], insets[j]);
			if (over.size() < 3)
				continue;
			bool close = false;
			for (const P2& v : over)
				close = close || std::fabs(F.plane.at(v[0], v[1]) - faces[out[j]].plane.at(v[0], v[1])) < cfg.characterHeight;
			if (close)
				rep.overlapArea += std::max(0.0, area(over));
		}
	}
	stage("mesh");
	return mesh;
}

SeamGap seamMismatch(const native::Mesh& a, const native::Mesh& b, double step)
{
	const auto segments = [](const native::Mesh& m)
	{
		std::vector<std::array<P3, 2>> out;
		for (const native::Edge& e : m.edges)
			if (e.linked)
				out.push_back({ m.vertices[e.a], m.vertices[e.b] });
		return out;
	};
	// nearest point of a segment in xz: the xz distance and the height difference there
	const auto probe = [](const P3& p, const std::array<P3, 2>& s)
	{
		const double dx = s[1][0] - s[0][0], dz = s[1][2] - s[0][2];
		const double l2 = dx * dx + dz * dz;
		const double t = l2 > 0.0 ? std::clamp(((p[0] - s[0][0]) * dx + (p[2] - s[0][2]) * dz) / l2, 0.0, 1.0) : 0.0;
		return SeamGap{ std::hypot(p[0] - s[0][0] - t * dx, p[2] - s[0][2] - t * dz), std::fabs(p[1] - (s[0][1] + t * (s[1][1] - s[0][1]))) };
	};
	const auto one = [&](const std::vector<std::array<P3, 2>>& from, const std::vector<std::array<P3, 2>>& to)
	{
		SeamGap worst;
		for (const auto& s : from)
		{
			const double length = std::hypot(s[1][0] - s[0][0], s[1][2] - s[0][2]);
			const int n = std::max(1, static_cast<int>(std::ceil(length / step)));
			for (int k = 0; k <= n; ++k)
			{
				const double t = static_cast<double>(k) / n;
				const P3 p{ s[0][0] + t * (s[1][0] - s[0][0]), s[0][1] + t * (s[1][1] - s[0][1]), s[0][2] + t * (s[1][2] - s[0][2]) };
				SeamGap best{ std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity() };
				for (const auto& o : to)
				{
					const SeamGap g = probe(p, o);
					if (g.xz < best.xz)
						best = g;
				}
				worst.xz = std::max(worst.xz, best.xz);
				worst.y = std::max(worst.y, best.y);
			}
		}
		return worst;
	};
	const SeamGap x = one(segments(a), segments(b)), y = one(segments(b), segments(a));
	return { std::max(x.xz, y.xz), std::max(x.y, y.y) };
}

} // namespace knav::surfgen
