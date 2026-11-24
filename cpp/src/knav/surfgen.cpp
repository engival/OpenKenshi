#include "knav/surfgen.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <utility>

namespace knav::surfgen
{

namespace
{

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

// p minus the convex c: the pieces of p outside c, each convex.
std::vector<Poly> subtract(const Poly& p, const Poly& c, double minArea)
{
	std::vector<Poly> pieces;
	Poly rest = p;
	for (std::size_t i = 0; i < c.size() && !rest.empty(); ++i)
	{
		const Line l = edgeLine(c[i], c[(i + 1) % c.size()]);
		Poly outside = clipLine(rest, -l.A, -l.B, -l.C);
		if (outside.size() >= 3 && area(outside) > minArea)
			pieces.push_back(std::move(outside));
		rest = clipLine(rest, l.A, l.B, l.C);
	}
	return pieces;
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

struct Piece
{
	Poly poly;
	Plane plane;
	std::size_t shape = 0;
	std::int32_t data = 1;
	kworld::DoorKey door = 0;
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
// `skipTriangle`: a triangle of an open shape that IS the face (never cuts itself).
void footprints(const Solid& solid, const Plane& plane, double base, double top, const P2& bmin, const P2& bmax, double halfWidth,
	std::vector<Poly>& out, std::size_t skipTriangle = SIZE_MAX)
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
		if (t / 3 == skipTriangle)
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

// Dedup key of a lattice corner.
struct Key
{
	std::int64_t x, z;
	bool operator<(const Key& o) const { return x != o.x ? x < o.x : z < o.z; }
	bool operator==(const Key& o) const { return x == o.x && z == o.z; }
};

Key keyOf(const P2& p, double lattice) { return { std::llround(p[0] / lattice), std::llround(p[1] / lattice) }; }

P2 snap(const P2& p, double lattice) { return { std::round(p[0] / lattice) * lattice, std::round(p[1] / lattice) * lattice }; }

// Walkable faces of one shape: a convex solid gives one polygon per coplanar group of up-facing triangles, an open mesh one per triangle.
void shapePieces(const Solid& solid, std::size_t shapeIndex, const Settings& cfg, std::vector<Piece>& out, std::vector<std::size_t>* sourceTriangle)
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
			out.push_back(std::move(piece));
			if (sourceTriangle != nullptr)
				sourceTriangle->push_back(t / 3);
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
		if (sourceTriangle != nullptr)
			sourceTriangle->push_back(SIZE_MAX);
	}
}

bool contains(const Poly& p, const P2& q)
{
	for (std::size_t i = 0; i < p.size(); ++i)
		if (cross2(p[i], p[(i + 1) % p.size()], q) < 0.0)
			return false;
	return true;
}

double maxHeightDifference(const Poly& region, const Plane& p, const Plane& q)
{
	double worst = 0.0;
	for (const P2& v : region)
		worst = std::max(worst, std::fabs(p.at(v[0], v[1]) - q.at(v[0], v[1])));
	return worst;
}

// ---- faces after the merge, with the matching state

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
};

P2 edgeEnd(const Face& f, std::size_t i) { return f.ring[(i + 1) % f.ring.size()].a; }

double faceArea(const Face& f)
{
	Poly p;
	for (const FaceEdge& e : f.ring)
		p.push_back(e.a);
	return area(p);
}

Poly faceXz(const Face& f)
{
	Poly p;
	for (const FaceEdge& e : f.ring)
		p.push_back(e.a);
	return p;
}

} // namespace

native::Mesh generate(const Input& input, const Settings& cfg, Report* report)
{
	Report local;
	Report& rep = report != nullptr ? *report : local;
	rep = Report{};

	std::vector<Solid> solids;
	for (const kworld::WorldShape* s : input.shapes)
		if (s != nullptr && s->use != kworld::NavUse::Door && s->indices.size() >= 3)
			solids.push_back(makeSolid(*s));

	// 1. walkable pieces, cut
	std::vector<Piece> pieces;
	std::vector<std::vector<Poly>> cutScratch;
	for (std::size_t si = 0; si < solids.size(); ++si)
	{
		if (solids[si].shape->use != kworld::NavUse::Walkable)
			continue;
		++rep.walkableShapes;
		std::vector<Piece> own;
		std::vector<std::size_t> source;
		shapePieces(solids[si], si, cfg, own, &source);
		for (std::size_t pi = 0; pi < own.size(); ++pi)
		{
			Piece& base = own[pi];
			P2 bmin{ 1e30, 1e30 }, bmax{ -1e30, -1e30 };
			for (const P2& v : base.poly)
				for (int k = 0; k < 2; ++k)
				{
					bmin[static_cast<std::size_t>(k)] = std::min(bmin[static_cast<std::size_t>(k)], v[static_cast<std::size_t>(k)]);
					bmax[static_cast<std::size_t>(k)] = std::max(bmax[static_cast<std::size_t>(k)], v[static_cast<std::size_t>(k)]);
				}
			std::vector<Poly> cutters;
			for (std::size_t sj = 0; sj < solids.size(); ++sj)
			{
				if (solids[sj].shape->use == kworld::NavUse::Door)
					continue;
				if (sj == si && solids[si].convex)
					continue;
				footprints(solids[sj], base.plane, cfg.cutBase, cfg.characterHeight, bmin, bmax, cfg.barrierHalfWidth, cutters, sj == si ? source[pi] : SIZE_MAX);
			}
			std::vector<Poly> remaining{ base.poly };
			// largest cutter first: fewer, bigger leftovers
			std::stable_sort(cutters.begin(), cutters.end(), [](const Poly& x, const Poly& y) { return area(x) > area(y); });
			for (const Poly& cutter : cutters)
			{
				std::vector<Poly> next;
				for (const Poly& r : remaining)
				{
					const Poly overlap = intersect(r, cutter);
					if (overlap.size() < 3 || area(overlap) < 1e-6)
					{
						next.push_back(r);
						continue;
					}
					for (Poly& part : subtract(r, cutter, cfg.minPieceArea))
						next.push_back(std::move(part));
				}
				remaining = std::move(next);
			}
			for (Poly& r : remaining)
				pieces.push_back({ std::move(r), base.plane, si, 1 });
		}
	}

	// 2. coplanar ownership: a piece loses what a lower shape index already holds within ownerTolerance in height
	{
		std::vector<Piece> kept;
		for (const Piece& q : pieces)
		{
			std::vector<Poly> remaining{ q.poly };
			for (const Piece& p : kept)
			{
				std::vector<Poly> next;
				for (const Poly& r : remaining)
				{
					const Poly overlap = intersect(r, p.poly);
					if (overlap.size() >= 3 && area(overlap) > 1e-6 && maxHeightDifference(overlap, q.plane, p.plane) <= cfg.ownerTolerance + 1e-3)
						for (Poly& part : subtract(r, p.poly, cfg.minPieceArea))
							next.push_back(std::move(part));
					else
						next.push_back(r);
				}
				remaining = std::move(next);
			}
			for (Poly& r : remaining)
				kept.push_back({ std::move(r), q.plane, q.shape, q.data, q.door });
		}
		pieces = std::move(kept);
	}

	// 3. keep inside the mask hull
	std::vector<std::array<float, 4>> hullPlanes;
	{
		std::vector<kworld::WorldShape> mask;
		for (const kworld::WorldShape* m : input.mask)
			if (m != nullptr)
				mask.push_back(*m);
		if (!mask.empty())
			hullPlanes = kworld::interiorMaskHull(mask);
	}
	// the hull's planes in a piece's frame: n . (x, a x + b z + c, z) <= d  ->  A x + B z + C >= 0
	const auto inHull = [&](Poly p, const Plane& plane, const std::vector<std::array<float, 4>>& planes)
	{
		for (const auto& h : planes)
		{
			p = clipLine(p, -(h[0] + h[1] * plane.a), -(h[2] + h[1] * plane.b), h[3] - h[1] * plane.c);
			if (p.size() < 3)
				break;
		}
		return p;
	};
	if (input.side == Side::Zone && hullPlanes.empty())
		pieces.clear();
	if (!hullPlanes.empty())
	{
		std::vector<Piece> kept;
		for (Piece& piece : pieces)
		{
			Poly inside = inHull(piece.poly, piece.plane, hullPlanes);
			if (inside.size() < 3 || area(inside) <= 1e-6)
				inside.clear();
			if (input.side == Side::Interior)
			{
				if (inside.size() >= 3 && area(inside) > cfg.minPieceArea)
				{
					piece.poly = std::move(inside);
					kept.push_back(std::move(piece));
				}
			} else if (inside.empty()) {
				kept.push_back(std::move(piece));
			} else {
				for (Poly& part : subtract(piece.poly, inside, cfg.minPieceArea))
				{
					Piece outside = piece;
					outside.poly = std::move(part);
					kept.push_back(std::move(outside));
				}
			}
		}
		pieces = std::move(kept);
	}

	// 3b. the door painter: what lies in a door's volume is cut out of its piece and carries the door's key (no geometry is added)
	for (const kworld::WorldShape* d : input.shapes)
	{
		if (!input.paintDoors || d == nullptr || d->use != kworld::NavUse::Door || d->indices.size() < 3)
			continue;
		const std::vector<std::array<float, 4>> volume = kworld::interiorMaskHull({ *d });
		if (volume.empty())
			continue;
		std::vector<Piece> painted;
		for (Piece& piece : pieces)
		{
			Poly inside = inHull(piece.poly, piece.plane, volume);
			if (inside.size() < 3 || area(inside) <= cfg.minPieceArea)
			{
				painted.push_back(std::move(piece));
				continue;
			}
			for (Poly& part : subtract(piece.poly, inside, cfg.minPieceArea))
			{
				Piece rest = piece;
				rest.poly = std::move(part);
				painted.push_back(std::move(rest));
			}
			piece.poly = std::move(inside);
			piece.data = 4; // the baked state is open (the capture's faces); the runtime door book repaints by the key
			piece.door = d->door;
			painted.push_back(std::move(piece));
		}
		pieces = std::move(painted);
	}

	// snap to the lattice, drop what collapsed
	for (Piece& piece : pieces)
	{
		for (P2& v : piece.poly)
			v = snap(v, cfg.lattice);
		Poly clean;
		for (const P2& v : piece.poly)
			if (clean.empty() || keyOf(clean.back(), cfg.lattice) != keyOf(v, cfg.lattice) )
				clean.push_back(v);
		while (clean.size() > 1 && keyOf(clean.front(), cfg.lattice) == keyOf(clean.back(), cfg.lattice))
			clean.pop_back();
		piece.poly = std::move(clean);
	}
	pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [&](const Piece& p) { return p.poly.size() < 3 || area(p.poly) < cfg.minPieceArea; }), pieces.end());
	rep.pieces = pieces.size();

	// 4. T-junction repair: a corner of one piece lying on another's edge becomes a corner there too
	for (Piece& target : pieces)
	{
		for (std::size_t i = 0; i < target.poly.size();)
		{
			const P2 a = target.poly[i], b = target.poly[(i + 1) % target.poly.size()];
			const double length = std::hypot(b[0] - a[0], b[1] - a[1]);
			bool split = false;
			if (length > 4 * cfg.lattice)
				for (const Piece& other : pieces)
				{
					if (&other == &target)
						continue;
					for (const P2& v : other.poly)
					{
						const double t = ((v[0] - a[0]) * (b[0] - a[0]) + (v[1] - a[1]) * (b[1] - a[1])) / (length * length);
						if (t <= 0.0 || t >= 1.0)
							continue;
						const double dist = std::fabs(cross2(a, b, v)) / length;
						if (dist > 2 * cfg.lattice || std::hypot(v[0] - a[0], v[1] - a[1]) < 2 * cfg.lattice || std::hypot(v[0] - b[0], v[1] - b[1]) < 2 * cfg.lattice)
							continue;
						if (std::fabs(other.plane.at(v[0], v[1]) - target.plane.at(v[0], v[1])) > cfg.planarTolerance)
							continue;
						target.poly.insert(target.poly.begin() + static_cast<std::ptrdiff_t>(i) + 1, v);
						split = true;
						break;
					}
					if (split)
						break;
				}
			if (!split)
				++i;
			// when split, re-test the shorter edge from the same i
		}
	}

	const auto polyKeys = [&](const Poly& p)
	{
		std::vector<Key> keys;
		for (const P2& v : p)
			keys.push_back(keyOf(v, cfg.lattice));
		return keys;
	};

	// 4b. Delaunay retriangulation of each coplanar group: the cut pieces are an exact but arbitrary partition (strips peeled along every
	// cutter); only the group's outer boundary and its holes matter. Triangulate the pieces, flip interior edges to Delaunay (the boundary
	// is never flipped), and let the merge below rebuild convex faces from fat triangles.
	{
		const auto planarPair = [&](const Piece& p, const Piece& q)
		{
			if (p.data != q.data || p.door != q.door)
				return false;
			for (const Piece* src : { &p, &q })
				for (const P2& v : src->poly)
					if (std::fabs(p.plane.at(v[0], v[1]) - q.plane.at(v[0], v[1])) > cfg.planarTolerance)
						return false;
			return true;
		};
		std::vector<std::vector<Key>> keys;
		for (const Piece& p : pieces)
			keys.push_back(polyKeys(p.poly));
		std::map<std::pair<Key, Key>, std::size_t> directed;
		for (std::size_t i = 0; i < pieces.size(); ++i)
			for (std::size_t e = 0; e < keys[i].size(); ++e)
				directed[{ keys[i][e], keys[i][(e + 1) % keys[i].size()] }] = i;
		std::vector<std::size_t> parent(pieces.size());
		std::iota(parent.begin(), parent.end(), std::size_t{ 0 });
		const auto root = [&](std::size_t x)
		{
			while (parent[x] != x)
				x = parent[x] = parent[parent[x]];
			return x;
		};
		for (std::size_t i = 0; i < pieces.size(); ++i)
			for (std::size_t e = 0; e < keys[i].size(); ++e)
			{
				const auto it = directed.find({ keys[i][(e + 1) % keys[i].size()], keys[i][e] });
				if (it != directed.end() && it->second != i && planarPair(pieces[i], pieces[it->second]))
					parent[root(i)] = root(it->second);
			}
		std::map<std::size_t, std::vector<std::size_t>> groups;
		for (std::size_t i = 0; i < pieces.size(); ++i)
			groups[root(i)].push_back(i);
		std::vector<Piece> rebuilt;
		for (const auto& [rootIndex, members] : groups)
		{
			if (members.size() == 1)
			{
				rebuilt.push_back(pieces[members[0]]);
				continue;
			}
			std::size_t biggest = members[0];
			for (const std::size_t m : members)
				if (area(pieces[m].poly) > area(pieces[biggest].poly))
					biggest = m;
			std::vector<P2> position;
			std::map<Key, int> idOf;
			const auto vertexId = [&](const P2& v)
			{
				const auto [it, fresh] = idOf.emplace(keyOf(v, cfg.lattice), static_cast<int>(position.size()));
				if (fresh)
					position.push_back(v);
				return it->second;
			};
			std::vector<std::array<int, 3>> tris;
			for (const std::size_t m : members)
			{
				const Poly& poly = pieces[m].poly;
				std::vector<int> ids;
				for (const P2& v : poly)
					ids.push_back(vertexId(v));
				const std::size_t n = ids.size();
				for (std::size_t apex = 0; apex < n; ++apex)
				{
					std::vector<std::array<int, 3>> fan;
					bool ok = true;
					for (std::size_t k = 1; k + 1 < n && ok; ++k)
					{
						const std::array<int, 3> t = { ids[apex], ids[(apex + k) % n], ids[(apex + k + 1) % n] };
						ok = cross2(position[static_cast<std::size_t>(t[0])], position[static_cast<std::size_t>(t[1])], position[static_cast<std::size_t>(t[2])]) > 1e-9;
						fan.push_back(t);
					}
					if (ok)
					{
						tris.insert(tris.end(), fan.begin(), fan.end());
						break;
					}
				}
			}
			const auto at = [&](int id) -> const P2& { return position[static_cast<std::size_t>(id)]; };
			for (std::size_t pass = 0; pass < 4000; ++pass)
			{
				std::map<std::pair<int, int>, std::size_t> dm;
				for (std::size_t t = 0; t < tris.size(); ++t)
					for (std::size_t k = 0; k < 3; ++k)
						dm[{ tris[t][k], tris[t][(k + 1) % 3] }] = t;
				bool flipped = false;
				for (std::size_t t = 0; t < tris.size() && !flipped; ++t)
					for (std::size_t k = 0; k < 3 && !flipped; ++k)
					{
						const int a = tris[t][k], b = tris[t][(k + 1) % 3], c = tris[t][(k + 2) % 3];
						const auto nb = dm.find({ b, a });
						if (nb == dm.end())
							continue;
						const std::size_t u = nb->second;
						int d = -1;
						for (const int v : tris[u])
							if (v != a && v != b)
								d = v;
						const P2 &pa = at(a), &pb = at(b), &pc = at(c), &pd = at(d);
						if (cross2(pa, pd, pc) <= 1e-9 || cross2(pd, pb, pc) <= 1e-9)
							continue; // the quad is not strictly convex: the diagonal stays
						const double ax = pa[0] - pd[0], az = pa[1] - pd[1], bx = pb[0] - pd[0], bz = pb[1] - pd[1], cx = pc[0] - pd[0], cz = pc[1] - pd[1];
						const double det = (ax * ax + az * az) * (bx * cz - cx * bz) - (bx * bx + bz * bz) * (ax * cz - cx * az) + (cx * cx + cz * cz) * (ax * bz - bx * az);
						if (det <= 1e-9)
							continue; // d is outside the circle of a, b, c: Delaunay already
						tris[t] = { a, d, c };
						tris[u] = { d, b, c };
						flipped = true;
					}
				if (!flipped)
					break;
			}
			for (const auto& t : tris)
			{
				Piece piece = pieces[biggest];
				piece.poly = { at(t[0]), at(t[1]), at(t[2]) };
				rebuilt.push_back(std::move(piece));
			}
		}
		pieces = std::move(rebuilt);
	}

	// 5. convex merge across shared edges
	for (;;)
	{
		struct Candidate
		{
			double length = -1.0;
			std::size_t i = 0, j = 0, ei = 0, ej = 0;
		} best;
		std::map<std::pair<Key, Key>, std::pair<std::size_t, std::size_t>> edgeMap;
		std::vector<std::vector<Key>> keys;
		for (const Piece& p : pieces)
			keys.push_back(polyKeys(p.poly));
		for (std::size_t i = 0; i < pieces.size(); ++i)
			for (std::size_t e = 0; e < keys[i].size(); ++e)
				edgeMap[{ keys[i][e], keys[i][(e + 1) % keys[i].size()] }] = { i, e };
		for (std::size_t i = 0; i < pieces.size(); ++i)
			for (std::size_t e = 0; e < keys[i].size(); ++e)
			{
				const auto it = edgeMap.find({ keys[i][(e + 1) % keys[i].size()], keys[i][e] });
				if (it == edgeMap.end() || it->second.first <= i)
					continue;
				const std::size_t j = it->second.first;
				if (pieces[i].data != pieces[j].data || pieces[i].door != pieces[j].door)
					continue;
				const P2 &a = pieces[i].poly[e], &b = pieces[i].poly[(e + 1) % pieces[i].poly.size()];
				const double length = std::hypot(b[0] - a[0], b[1] - a[1]);
				if (length <= best.length)
					continue;
				bool planar = true;
				for (const P2& v : pieces[j].poly)
					planar = planar && std::fabs(pieces[i].plane.at(v[0], v[1]) - pieces[j].plane.at(v[0], v[1])) <= cfg.planarTolerance;
				for (const P2& v : pieces[i].poly)
					planar = planar && std::fabs(pieces[i].plane.at(v[0], v[1]) - pieces[j].plane.at(v[0], v[1])) <= cfg.planarTolerance;
				if (!planar)
					continue;
				// merged ring: i's corners from the shared edge's end round to its start, then j's corners after its shared edge
				const std::size_t ni = pieces[i].poly.size(), nj = pieces[j].poly.size();
				Poly merged;
				for (std::size_t k = 0; k < ni; ++k)
					merged.push_back(pieces[i].poly[(e + 1 + k) % ni]);
				// i: starts at b (e+1) ... ends at a (e); j's shared edge runs b->a at index f: its corners f+2 .. f+nj-1 are the rest
				const std::size_t f = it->second.second;
				for (std::size_t k = 2; k < nj; ++k)
					merged.push_back(pieces[j].poly[(f + k) % nj]);
				bool convex = true;
				for (std::size_t k = 0; k < merged.size() && convex; ++k)
				{
					const P2 &p0 = merged[k], &p2 = merged[(k + 2) % merged.size()];
					// a corner that bends the wrong way by no more than the snap noise counts as straight
					convex = cross2(p0, merged[(k + 1) % merged.size()], p2) >= -cfg.collinearTolerance * std::hypot(p2[0] - p0[0], p2[1] - p0[1]);
				}
				if (!convex)
					continue;
				best = { length, i, j, e, f };
			}
		if (best.length < 0.0)
			break;
		Piece& A = pieces[best.i];
		const Piece& B = pieces[best.j];
		const std::size_t ni = A.poly.size(), nj = B.poly.size();
		Poly merged;
		for (std::size_t k = 0; k < ni; ++k)
			merged.push_back(A.poly[(best.ei + 1 + k) % ni]);
		for (std::size_t k = 2; k < nj; ++k)
			merged.push_back(B.poly[(best.ej + k) % nj]);
		// the plane of the larger piece
		if (area(B.poly) > area(A.poly))
			A.plane = B.plane;
		// drop the two shared end corners when collinear and no other piece uses them
		const P2 shared[2] = { A.poly[best.ei], A.poly[(best.ei + 1) % ni] };
		A.poly = std::move(merged);
		pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(best.j));
		for (const P2& s : shared)
		{
			const Key sk = keyOf(s, cfg.lattice);
			std::size_t uses = 0;
			for (const Piece& p : pieces)
				for (const P2& v : p.poly)
					uses += keyOf(v, cfg.lattice) == sk ? 1 : 0;
			if (uses != 1)
				continue;
			for (std::size_t k = 0; k < A.poly.size(); ++k)
				if (keyOf(A.poly[k], cfg.lattice) == sk)
				{
					const P2 &prev = A.poly[(k + A.poly.size() - 1) % A.poly.size()], &next = A.poly[(k + 1) % A.poly.size()];
					const double chord = std::hypot(next[0] - prev[0], next[1] - prev[1]);
					if (chord > 0.0 && std::fabs(cross2(prev, A.poly[k], next)) / chord < cfg.collinearTolerance && A.poly.size() > 3)
						A.poly.erase(A.poly.begin() + static_cast<std::ptrdiff_t>(k));
					break;
				}
		}
	}

	// 6. faces, then edge matching
	std::vector<Face> faces;
	int nextUid = 1;
	for (const Piece& p : pieces)
	{
		Face f;
		f.plane = p.plane;
		f.data = p.data;
		f.door = p.door;
		f.shape = p.shape;
		for (const P2& v : p.poly)
			f.ring.push_back({ v, nextUid++ });
		faces.push_back(std::move(f));
	}
	std::map<int, int> partner; // edge uid -> matched uid
	const auto splitEdge = [&](Face& f, std::size_t edge, const P2& at)
	{
		f.ring.insert(f.ring.begin() + static_cast<std::ptrdiff_t>(edge) + 1, FaceEdge{ at, nextUid++ });
	};
	for (int pass = 0; pass < 2; ++pass)
	{
		const double limit = pass == 0 ? cfg.matchSeparation : cfg.matchSeparationMax;
		for (std::size_t guard = 0; guard < 20000; ++guard)
		{
			struct Pair
			{
				double gap = 1e30;
				std::size_t fa = 0, ea = 0, fb = 0, eb = 0;
				double s0 = 0, s1 = 0; // overlap along A's edge
			} best;
			for (std::size_t fa = 0; fa < faces.size(); ++fa)
				for (std::size_t ea = 0; ea < faces[fa].ring.size(); ++ea)
				{
					if (partner.count(faces[fa].ring[ea].uid) != 0)
						continue;
					const P2 a0 = faces[fa].ring[ea].a, a1 = edgeEnd(faces[fa], ea);
					const double lenA = std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
					if (lenA < cfg.matchOverlap)
						continue;
					const double ux = (a1[0] - a0[0]) / lenA, uz = (a1[1] - a0[1]) / lenA;
					for (std::size_t fb = fa + 1; fb < faces.size(); ++fb)
						for (std::size_t eb = 0; eb < faces[fb].ring.size(); ++eb)
						{
							if (partner.count(faces[fb].ring[eb].uid) != 0)
								continue;
							const P2 b0 = faces[fb].ring[eb].a, b1 = edgeEnd(faces[fb], eb);
							const double lenB = std::hypot(b1[0] - b0[0], b1[1] - b0[1]);
							if (lenB < cfg.matchOverlap)
								continue;
							if (((b1[0] - b0[0]) * ux + (b1[1] - b0[1]) * uz) / lenB > -0.985)
								continue; // not antiparallel
							// B's corners along A's line, and their signed distance (positive: outside A, the right of the edge)
							const double t0 = (b0[0] - a0[0]) * ux + (b0[1] - a0[1]) * uz, t1 = (b1[0] - a0[0]) * ux + (b1[1] - a0[1]) * uz;
							const double d0 = (b0[0] - a0[0]) * uz - (b0[1] - a0[1]) * ux, d1 = (b1[0] - a0[0]) * uz - (b1[1] - a0[1]) * ux;
							const double s0 = std::max(0.0, std::min(t0, t1)), s1 = std::min(lenA, std::max(t0, t1));
							if (s1 - s0 < cfg.matchOverlap)
								continue;
							const double gap = 0.5 * (d0 + d1);
							if (std::fabs(d0) > cfg.matchSeparationMax || std::fabs(d1) > cfg.matchSeparationMax || gap < -0.05 || gap > limit)
								continue;
							const double mid = 0.5 * (s0 + s1);
							const double ya = faces[fa].plane.at(a0[0] + ux * mid, a0[1] + uz * mid);
							const double mx = a0[0] + ux * mid + uz * gap, mz = a0[1] + uz * mid - ux * gap;
							const double yb = faces[fb].plane.at(mx, mz);
							if (std::fabs(ya - yb) > cfg.matchStep)
								continue;
							if (gap < best.gap)
								best = { gap, fa, ea, fb, eb, s0, s1 };
						}
				}
			if (best.gap >= 1e29)
				break;
			// blocked by a solid standing between (a wall that is not a door)?
			{
				Face& A = faces[best.fa];
				const P2 a0 = A.ring[best.ea].a, a1 = edgeEnd(A, best.ea);
				const double lenA = std::hypot(a1[0] - a0[0], a1[1] - a0[1]);
				const double ux = (a1[0] - a0[0]) / lenA, uz = (a1[1] - a0[1]) / lenA;
				const double mid = 0.5 * (best.s0 + best.s1);
				bool blocked = false;
				if (best.gap > 0.05)
				{
					const P2 centre{ a0[0] + ux * mid + uz * best.gap * 0.5, a0[1] + uz * mid - ux * best.gap * 0.5 };
					std::vector<Poly> cut;
					const Plane lower = A.plane.at(centre[0], centre[1]) < faces[best.fb].plane.at(centre[0], centre[1]) ? A.plane : faces[best.fb].plane;
					for (const Solid& s : solids)
						if (s.shape->use != kworld::NavUse::Door)
							footprints(s, lower, cfg.cutBase, cfg.characterHeight, centre, centre, cfg.barrierHalfWidth, cut);
					for (const Poly& c : cut)
						if (contains(c, centre))
							blocked = true;
				}
				if (blocked)
				{
					// refuse this pair by splitting nothing and marking both edges unmatchable: partner = -uid
					partner[faces[best.fa].ring[best.ea].uid] = -1;
					partner[faces[best.fb].ring[best.eb].uid] = -1;
					continue;
				}
				// split A at the overlap ends, B at the projections of the same points
				const P2 pa0{ a0[0] + ux * best.s0, a0[1] + uz * best.s0 }, pa1{ a0[0] + ux * best.s1, a0[1] + uz * best.s1 };
				std::size_t ea = best.ea;
				if (best.s1 < lenA - 1e-6)
				{
					splitEdge(A, ea, pa1);
				}
				if (best.s0 > 1e-6)
				{
					splitEdge(A, ea, pa0);
					++ea;
				}
				Face& B = faces[best.fb];
				const P2 b0 = B.ring[best.eb].a, b1 = edgeEnd(B, best.eb);
				const double lenB = std::hypot(b1[0] - b0[0], b1[1] - b0[1]);
				const double vx = (b1[0] - b0[0]) / lenB, vz = (b1[1] - b0[1]) / lenB;
				// along B (antiparallel): A's end pa1 comes first, then pa0
				const double u0 = std::clamp((pa1[0] - b0[0]) * vx + (pa1[1] - b0[1]) * vz, 0.0, lenB);
				const double u1 = std::clamp((pa0[0] - b0[0]) * vx + (pa0[1] - b0[1]) * vz, 0.0, lenB);
				std::size_t eb = best.eb;
				if (u1 < lenB - 1e-6)
					splitEdge(B, eb, { b0[0] + vx * u1, b0[1] + vz * u1 });
				if (u0 > 1e-6)
				{
					splitEdge(B, eb, { b0[0] + vx * u0, b0[1] + vz * u0 });
					++eb;
				}
				partner[A.ring[ea].uid] = B.ring[eb].uid;
				partner[B.ring[eb].uid] = A.ring[ea].uid;
				++rep.matchedEdges;
			}
		}
	}

	// the seam: a face edge with both ends on one side plane of the mask hull (the floor and ceiling planes are not seams)
	const auto onSeam = [&](const Face& f, std::size_t k)
	{
		const P2 a = f.ring[k].a, b = edgeEnd(f, k);
		if (std::hypot(b[0] - a[0], b[1] - a[1]) < 0.05)
			return false; // snap noise round a corner, not a seam
		const double ya = f.plane.at(a[0], a[1]), yb = f.plane.at(b[0], b[1]);
		for (const auto& h : hullPlanes)
			if (std::fabs(h[1]) < 0.5f && std::fabs(h[0] * a[0] + h[1] * ya + h[2] * a[1] - h[3]) <= cfg.seamTolerance
				&& std::fabs(h[0] * b[0] + h[1] * yb + h[2] * b[1] - h[3]) <= cfg.seamTolerance)
				return true;
		return false;
	};

	// 7. components over matched edges, pruning to the seed
	std::map<int, std::size_t> faceByUid;
	for (std::size_t f = 0; f < faces.size(); ++f)
		for (const FaceEdge& e : faces[f].ring)
			faceByUid[e.uid] = f;
	std::vector<std::size_t> component(faces.size(), SIZE_MAX);
	std::size_t componentCount = 0;
	std::vector<double> componentArea;
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		if (component[f] != SIZE_MAX)
			continue;
		std::vector<std::size_t> stack{ f };
		component[f] = componentCount;
		componentArea.push_back(0.0);
		while (!stack.empty())
		{
			const std::size_t cur = stack.back();
			stack.pop_back();
			componentArea[componentCount] += faceArea(faces[cur]);
			for (const FaceEdge& e : faces[cur].ring)
			{
				const auto it = partner.find(e.uid);
				if (it == partner.end() || it->second < 0)
					continue;
				const std::size_t other = faceByUid[it->second];
				if (component[other] == SIZE_MAX)
				{
					component[other] = componentCount;
					stack.push_back(other);
				}
			}
		}
		++componentCount;
	}
	rep.components = componentCount;
	rep.componentAreas = componentArea;
	std::size_t seedComponent = SIZE_MAX;
	if (componentCount > 0)
	{
		std::optional<P3> seed = input.seed;
		if (seed)
		{
			// the lowest face containing the seed in xz; none contains it (a table stands there): the face nearest to it
			double bestDistance = 1e30, bestY = 1e30;
			for (std::size_t f = 0; f < faces.size(); ++f)
			{
				const Poly xz = faceXz(faces[f]);
				const P2 q{ (*seed)[0], (*seed)[2] };
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
				const double y = faces[f].plane.at(q[0], q[1]);
				if (inside)
					distance = 0.0;
				if (distance < bestDistance - 1e-9 || (distance < bestDistance + 1e-9 && y < bestY))
				{
					bestDistance = distance;
					bestY = y;
					seedComponent = component[f];
				}
			}
		}
		if (seedComponent == SIZE_MAX)
			seedComponent = static_cast<std::size_t>(std::max_element(componentArea.begin(), componentArea.end()) - componentArea.begin());
	}
	// zone side: a component stays when it reaches the seam (it joins the interior there); the rest is an island
	std::vector<char> reachesSeam(componentCount, 0);
	if (input.side == Side::Zone)
		for (std::size_t f = 0; f < faces.size(); ++f)
			for (std::size_t k = 0; k < faces[f].ring.size(); ++k)
				if (partner.count(faces[f].ring[k].uid) == 0 || partner[faces[f].ring[k].uid] < 0)
					reachesSeam[component[f]] |= onSeam(faces[f], k) ? 1 : 0;
	std::vector<Face> kept;
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		if (input.side == Side::Zone ? (reachesSeam[component[f]] != 0 || !cfg.pruneZoneIslands) : component[f] == seedComponent)
			kept.push_back(std::move(faces[f]));
		else
		{
			++rep.droppedFaces;
			rep.droppedArea += faceArea(faces[f]);
		}
	}
	faces = std::move(kept);

	// 8. the native mesh
	native::Mesh mesh;
	mesh.info = "surfgen";
	std::map<std::array<std::int64_t, 3>, std::uint32_t> vertexIds;
	std::map<int, std::pair<std::size_t, std::size_t>> edgeOfUid; // uid -> (face, edge index in mesh.edges)
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		native::Face face;
		face.startEdge = static_cast<std::uint32_t>(mesh.edges.size());
		face.numEdges = static_cast<std::uint16_t>(faces[f].ring.size());
		face.data = faces[f].data;
		face.door = faces[f].door;
		for (std::size_t k = 0; k < faces[f].ring.size(); ++k)
		{
			const auto vertexOf = [&](const P2& p)
			{
				const double y = faces[f].plane.at(p[0], p[1]);
				const std::array<std::int64_t, 3> key{ std::llround(p[0] / cfg.lattice), std::llround(y / 0.01), std::llround(p[1] / cfg.lattice) };
				const auto it = vertexIds.find(key);
				if (it != vertexIds.end())
					return it->second;
				mesh.vertices.push_back({ p[0], y, p[1] });
				return vertexIds[key] = static_cast<std::uint32_t>(mesh.vertices.size() - 1);
			};
			native::Edge edge;
			edge.linked = onSeam(faces[f], k) && (partner.count(faces[f].ring[k].uid) == 0 || partner[faces[f].ring[k].uid] < 0);
			if (edge.linked)
			{
				++rep.seamEdges;
				rep.seamLength += std::hypot(std::hypot(edgeEnd(faces[f], k)[0] - faces[f].ring[k].a[0], edgeEnd(faces[f], k)[1] - faces[f].ring[k].a[1]),
					faces[f].plane.at(edgeEnd(faces[f], k)[0], edgeEnd(faces[f], k)[1]) - faces[f].plane.at(faces[f].ring[k].a[0], faces[f].ring[k].a[1]));
			}
			edge.a = vertexOf(faces[f].ring[k].a);
			edge.b = vertexOf(edgeEnd(faces[f], k));
			edgeOfUid[faces[f].ring[k].uid] = { f, mesh.edges.size() };
			mesh.edges.push_back(edge);
		}
		mesh.faces.push_back(face);
	}
	for (const auto& [uid, other] : partner)
	{
		if (other < 0)
			continue;
		const auto a = edgeOfUid.find(uid);
		const auto b = edgeOfUid.find(other);
		if (a == edgeOfUid.end() || b == edgeOfUid.end())
			continue;
		mesh.edges[a->second.second].oppositeEdge = static_cast<std::int32_t>(b->second.second);
		mesh.edges[a->second.second].oppositeFace = static_cast<std::int32_t>(b->second.first);
	}
	for (const native::Edge& e : mesh.edges)
		rep.boundaryEdges += e.oppositeFace < 0 ? 1 : 0;
	rep.faces = faces.size();
	for (const Face& f : faces)
		rep.faceShape.push_back(f.shape);
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		rep.area += faceArea(faces[f]);
		if (faces[f].door != 0)
		{
			++rep.doorFaces;
			rep.doorArea += faceArea(faces[f]);
		}
		const double slope = std::atan(std::hypot(faces[f].plane.a, faces[f].plane.b)) * 180.0 / kPi;
		rep.steepFaces += slope > 48.0 ? 1.0 : 0.0;
		for (std::size_t g = f + 1; g < faces.size(); ++g)
		{
			const Poly overlap = intersect(faceXz(faces[f]), faceXz(faces[g]));
			if (overlap.size() >= 3 && maxHeightDifference(overlap, faces[f].plane, faces[g].plane) <= cfg.ownerTolerance + 1e-3)
				rep.overlapArea += std::max(0.0, area(overlap));
		}
	}
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
