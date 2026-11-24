#include "knav/assemble.hpp"

#include "knav/area.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>

namespace knav::assembly
{

namespace
{

using P3 = std::array<double, 3>;
using Poly = std::vector<P3>;

// A convex xz polygon (counter-clockwise in x right, z up... the sign of `area2` fixes it) with a face-data value.
struct Piece
{
	Poly pts;
	std::int32_t data = 0;
};

double area2(const Poly& p)
{
	double sum = 0.0;
	for (std::size_t i = 0; i < p.size(); ++i)
	{
		const P3& a = p[i];
		const P3& b = p[(i + 1) % p.size()];
		sum += a[0] * b[2] - b[0] * a[2];
	}
	return sum;
}

// Positive winding: area2 > 0.
void orient(Poly& p)
{
	if (area2(p) < 0.0)
		std::reverse(p.begin(), p.end());
}

// The point of segment a-b where f = 0, from the lower endpoint (in x, z, y order) so two polygons sharing the segment get one point.
template <class F> P3 crossing(P3 a, P3 b, const F& f)
{
	if (std::tie(b[0], b[2], b[1]) < std::tie(a[0], a[2], a[1]))
		std::swap(a, b);
	const double fa = f(a);
	const double fb = f(b);
	const double t = fa / (fa - fb);
	return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
}

// The part of the convex polygon with f > 0 (positive) or f <= 0 (not); f is linear in (x, z) (it may read y, which is linear along an edge too).
template <class F> Poly side(const Poly& in, const F& f, bool positive)
{
	Poly out;
	const std::size_t n = in.size();
	for (std::size_t i = 0; i < n; ++i)
	{
		const P3& a = in[i];
		const P3& b = in[(i + 1) % n];
		const bool ia = (f(a) > 0.0) == positive;
		const bool ib = (f(b) > 0.0) == positive;
		if (ia)
			out.push_back(a);
		if (ia != ib)
			out.push_back(crossing(a, b, f));
	}
	return out;
}

// A plane y = a x + b z + c through the polygon (least damage: the first three corners that are not collinear, else the mean height).
struct Plane
{
	double a = 0.0, b = 0.0, c = 0.0;
	double at(double x, double z) const { return a * x + b * z + c; }
};

Plane fit(const Poly& p)
{
	// Newell-free: solve through the corner triple of the largest area.
	double best = 1e-9;
	Plane plane;
	double mean = 0.0;
	for (const P3& v : p)
		mean += v[1];
	plane.c = mean / static_cast<double>(p.size());
	const std::size_t n = p.size();
	for (std::size_t i = 1; i + 1 < n; ++i)
	{
		const P3 &o = p[0], &u = p[i], &w = p[i + 1];
		const double det = (u[0] - o[0]) * (w[2] - o[2]) - (w[0] - o[0]) * (u[2] - o[2]);
		if (std::fabs(det) <= best)
			continue;
		best = std::fabs(det);
		plane.a = ((u[1] - o[1]) * (w[2] - o[2]) - (w[1] - o[1]) * (u[2] - o[2])) / det;
		plane.b = ((w[1] - o[1]) * (u[0] - o[0]) - (u[1] - o[1]) * (w[0] - o[0])) / det;
		plane.c = o[1] - plane.a * o[0] - plane.b * o[2];
	}
	return plane;
}

struct Box
{
	double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
	bool overlaps(const Box& o) const { return x0 < o.x1 && o.x0 < x1 && z0 < o.z1 && o.z0 < z1; }
};

Box boxOf(const Poly& p)
{
	Box b;
	for (const P3& v : p)
	{
		b.x0 = std::min(b.x0, v[0]);
		b.x1 = std::max(b.x1, v[0]);
		b.z0 = std::min(b.z0, v[2]);
		b.z1 = std::max(b.z1, v[2]);
	}
	return b;
}

void keep(std::vector<Piece>& out, Poly&& p, std::int32_t data, double minArea)
{
	if (p.size() >= 3 && 0.5 * std::fabs(area2(p)) >= minArea)
		out.push_back({ std::move(p), data });
}

// Removes from every piece the part inside the convex region (positive winding) where first(p) > 0 and second(p) > 0. The functions get a
// piece's plane so they can compare heights. Returns the removed xz area.
template <class Fn> double cut(std::vector<Piece>& pieces, const Poly& region, const Fn& makeFunctions, double minArea)
{
	const Box regionBox = boxOf(region);
	double removed = 0.0;
	std::vector<Piece> out;
	out.reserve(pieces.size());
	for (Piece& piece : pieces)
	{
		if (!boxOf(piece.pts).overlaps(regionBox))
		{
			out.push_back(std::move(piece));
			continue;
		}
		// the part inside the region, where both functions hold, is what goes; a piece with none stays whole
		Poly rest = piece.pts;
		for (std::size_t i = 0; i < region.size() && rest.size() >= 3; ++i)
		{
			const P3 &a = region[i], &b = region[(i + 1) % region.size()];
			rest = side(rest, [&](const P3& v) { return (b[0] - a[0]) * (v[2] - a[2]) - (b[2] - a[2]) * (v[0] - a[0]); }, true);
		}
		const auto [first, second] = makeFunctions(fit(piece.pts));
		const Poly in = rest.size() >= 3 ? side(rest, first, true) : Poly{};
		const Poly gone = in.size() >= 3 ? side(in, second, true) : Poly{};
		const double goneArea = gone.size() >= 3 ? 0.5 * std::fabs(area2(gone)) : 0.0;
		if (goneArea < minArea)
		{
			out.push_back(std::move(piece));
			continue;
		}
		removed += goneArea;
		// the rest of the piece: outside the region (right of each edge), then inside it where a function fails
		Poly remain = piece.pts;
		for (std::size_t i = 0; i < region.size() && remain.size() >= 3; ++i)
		{
			const P3 &a = region[i], &b = region[(i + 1) % region.size()];
			const auto left = [&](const P3& v) { return (b[0] - a[0]) * (v[2] - a[2]) - (b[2] - a[2]) * (v[0] - a[0]); };
			keep(out, side(remain, left, false), piece.data, minArea);
			remain = side(remain, left, true);
		}
		keep(out, side(remain, first, false), piece.data, minArea);
		keep(out, side(side(remain, first, true), second, false), piece.data, minArea);
	}
	pieces = std::move(out);
	return removed;
}

// A mesh's faces as pieces (convex in xz is assumed, as the generator and Recast give).
std::vector<Piece> piecesOf(const native::Mesh& mesh)
{
	std::vector<Piece> out;
	for (const native::Face& face : mesh.faces)
	{
		Poly p;
		for (std::uint32_t k = 0; k < face.numEdges; ++k)
			p.push_back(mesh.vertices[mesh.edges[face.startEdge + k].a]);
		orient(p);
		out.push_back({ std::move(p), face.data });
	}
	return out;
}

// Pieces to one mesh: corners welded on the lattice, T-junctions repaired (a corner on another piece's edge is added to it), opposite
// edges paired by their corner pair.
native::Mesh meshOf(std::vector<Piece>& pieces, double lattice)
{
	native::Mesh mesh;
	std::map<std::tuple<long long, long long, long long>, std::uint32_t> ids;
	const auto weld = [&](const P3& v)
	{
		const auto key = std::make_tuple(std::llround(v[0] / lattice), std::llround(v[2] / lattice), std::llround(v[1] / lattice));
		const auto [it, inserted] = ids.try_emplace(key, static_cast<std::uint32_t>(mesh.vertices.size()));
		if (inserted)
			mesh.vertices.push_back({ std::get<0>(key) * lattice, std::get<2>(key) * lattice, std::get<1>(key) * lattice });
		return it->second;
	};
	std::vector<std::vector<std::uint32_t>> loops;
	for (const Piece& piece : pieces)
	{
		std::vector<std::uint32_t> loop;
		for (const P3& v : piece.pts)
			if (const std::uint32_t id = weld(v); loop.empty() || loop.back() != id)
				loop.push_back(id);
		if (loop.size() > 1 && loop.front() == loop.back())
			loop.pop_back();
		loops.push_back(std::move(loop));
	}
	// T-junctions: every corner within 0.02 u of an edge's segment (in xz and height) splits that edge.
	std::vector<std::uint32_t> order(mesh.vertices.size());
	for (std::uint32_t i = 0; i < order.size(); ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return mesh.vertices[a][0] < mesh.vertices[b][0]; });
	constexpr double kOn = 0.02;
	for (auto& loop : loops)
	{
		std::vector<std::uint32_t> out;
		for (std::size_t i = 0; i < loop.size(); ++i)
		{
			const std::uint32_t ia = loop[i], ib = loop[(i + 1) % loop.size()];
			out.push_back(ia);
			const P3 &a = mesh.vertices[ia], &b = mesh.vertices[ib];
			const double lo = std::min(a[0], b[0]) - kOn, hi = std::max(a[0], b[0]) + kOn;
			const double length2 = (b[0] - a[0]) * (b[0] - a[0]) + (b[2] - a[2]) * (b[2] - a[2]);
			if (length2 < 1e-12)
				continue;
			std::vector<std::pair<double, std::uint32_t>> on;
			for (auto it = std::lower_bound(order.begin(), order.end(), lo, [&](std::uint32_t v, double x) { return mesh.vertices[v][0] < x; });
				it != order.end() && mesh.vertices[*it][0] <= hi; ++it)
			{
				const P3& v = mesh.vertices[*it];
				if (*it == ia || *it == ib)
					continue;
				const double t = ((v[0] - a[0]) * (b[0] - a[0]) + (v[2] - a[2]) * (b[2] - a[2])) / length2;
				if (t <= 0.0 || t >= 1.0)
					continue;
				const double dx = a[0] + (b[0] - a[0]) * t - v[0], dz = a[2] + (b[2] - a[2]) * t - v[2];
				if (dx * dx + dz * dz < kOn * kOn && std::fabs(a[1] + (b[1] - a[1]) * t - v[1]) < 0.5)
					on.emplace_back(t, *it);
			}
			std::sort(on.begin(), on.end());
			for (const auto& [t, id] : on)
				out.push_back(id);
		}
		loop = std::move(out);
	}
	std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>> directed; // (a, b) -> (face, edge)
	for (std::size_t f = 0; f < pieces.size(); ++f)
	{
		if (loops[f].size() < 3)
			continue;
		native::Face face;
		face.startEdge = static_cast<std::uint32_t>(mesh.edges.size());
		face.numEdges = static_cast<std::uint16_t>(loops[f].size());
		face.data = pieces[f].data;
		for (std::size_t k = 0; k < loops[f].size(); ++k)
		{
			native::Edge edge;
			edge.a = loops[f][k];
			edge.b = loops[f][(k + 1) % loops[f].size()];
			directed[{ edge.a, edge.b }] = { static_cast<std::uint32_t>(mesh.faces.size()), static_cast<std::uint32_t>(mesh.edges.size()) };
			mesh.edges.push_back(edge);
		}
		mesh.faces.push_back(face);
	}
	for (std::size_t e = 0; e < mesh.edges.size(); ++e)
	{
		const auto it = directed.find({ mesh.edges[e].b, mesh.edges[e].a });
		if (it != directed.end())
		{
			mesh.edges[e].oppositeFace = static_cast<std::int32_t>(it->second.first);
			mesh.edges[e].oppositeEdge = static_cast<std::int32_t>(it->second.second);
		}
	}
	return mesh;
}

} // namespace

RecastFaces recastFaces(const BakedTile& tile)
{
	RecastFaces out;
	if (tile.data.empty())
		return out;
	dtNavMesh nav;
	dtNavMeshParams params{};
	params.orig[0] = params.orig[1] = params.orig[2] = 0.0f;
	params.tileWidth = params.tileHeight = 1.0f;
	params.maxTiles = 1;
	params.maxPolys = 1 << 12;
	// The tile's header holds its own bounds; the navmesh only parses it, so a copy with the ids of the grid is fine.
	std::vector<std::uint8_t> copy = tile.data;
	if (dtStatusFailed(nav.init(&params)) || dtStatusFailed(nav.addTile(copy.data(), static_cast<int>(copy.size()), 0, 0, nullptr)))
		return out;
	const dtMeshTile* t = nav.getTileAt(tile.x, tile.z, 0);
	if (!t || !t->header)
		return out;
	std::map<std::uint16_t, DoorKey> doorOf;
	for (const DoorPoly& d : tile.doors)
		doorOf[d.poly] = d.door;
	out.mesh.tileX = tile.x;
	out.mesh.tileZ = tile.z;
	std::map<std::tuple<float, float, float>, std::uint32_t> ids;
	for (int ip = 0; ip < t->header->polyCount; ++ip)
	{
		const dtPoly& poly = t->polys[ip];
		if (poly.getType() != DT_POLYTYPE_GROUND)
			continue;
		native::Face face;
		face.startEdge = static_cast<std::uint32_t>(out.mesh.edges.size());
		face.numEdges = poly.vertCount;
		const bool door = poly.getArea() == static_cast<unsigned char>(Area::DoorOpen);
		face.data = door ? 5 : 0;
		const dtPolyDetail& detail = t->detailMeshes[ip];
		std::vector<P3> corners;
		for (int k = 0; k < poly.vertCount; ++k)
		{
			const float* v = &t->verts[poly.verts[k] * 3];
			const float y = k < detail.vertCount && detail.vertBase + static_cast<unsigned>(k) < static_cast<unsigned>(t->header->detailVertCount)
				? t->detailVerts[(detail.vertBase + static_cast<unsigned>(k)) * 3 + 1] : v[1];
			corners.push_back({ v[0], y, v[2] });
		}
		for (int k = 0; k < poly.vertCount; ++k)
		{
			const auto key = std::make_tuple(static_cast<float>(corners[k][0]), static_cast<float>(corners[k][1]), static_cast<float>(corners[k][2]));
			const auto [it, inserted] = ids.try_emplace(key, static_cast<std::uint32_t>(out.mesh.vertices.size()));
			if (inserted)
				out.mesh.vertices.push_back(corners[k]);
			native::Edge edge;
			edge.a = it->second;
			out.mesh.edges.push_back(edge);
		}
		for (int k = 0; k < poly.vertCount; ++k)
			out.mesh.edges[face.startEdge + k].b = out.mesh.edges[face.startEdge + (k + 1) % poly.vertCount].a;
		if (door)
		{
			if (const auto it = doorOf.find(static_cast<std::uint16_t>(ip)); it != doorOf.end())
				out.doors.push_back({ it->second, corners });
		}
		out.mesh.faces.push_back(face);
	}
	std::map<std::pair<std::uint32_t, std::uint32_t>, std::pair<std::uint32_t, std::uint32_t>> directed;
	for (std::size_t f = 0; f < out.mesh.faces.size(); ++f)
		for (std::uint32_t k = 0; k < out.mesh.faces[f].numEdges; ++k)
		{
			const native::Edge& e = out.mesh.edges[out.mesh.faces[f].startEdge + k];
			directed[{ e.a, e.b }] = { static_cast<std::uint32_t>(f), out.mesh.faces[f].startEdge + k };
		}
	for (native::Edge& e : out.mesh.edges)
		if (const auto it = directed.find({ e.b, e.a }); it != directed.end())
		{
			e.oppositeFace = static_cast<std::int32_t>(it->second.first);
			e.oppositeEdge = static_cast<std::int32_t>(it->second.second);
		}
	return out;
}

std::vector<std::array<std::array<double, 3>, 3>> terrainTriangles(const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1)
{
	constexpr int kStride = 2; // the bake's 36 u lattice (bake.cpp kTerrainStride)
	const double spacing = kworld::kHeightmapSpacing * kStride;
	const int lastCol = (terrain.sampleCountX() - 1) / kStride;
	const int lastRow = (terrain.sampleCountZ() - 1) / kStride;
	const int c0 = std::clamp(static_cast<int>(std::floor((x0 - kworld::kWorldMin) / spacing)), 0, lastCol);
	const int c1 = std::clamp(static_cast<int>(std::ceil((x1 - kworld::kWorldMin) / spacing)), 0, lastCol);
	const int r0 = std::clamp(static_cast<int>(std::floor((z0 - kworld::kWorldMin) / spacing)), 0, lastRow);
	const int r1 = std::clamp(static_cast<int>(std::ceil((z1 - kworld::kWorldMin) / spacing)), 0, lastRow);
	const auto vertex = [&](int col, int row) -> P3
	{
		return { kworld::kWorldMin + spacing * col, terrain.sampleHeight(col * kStride, row * kStride), kworld::kWorldMin + spacing * row };
	};
	std::vector<std::array<P3, 3>> out;
	for (int row = r0; row < r1; ++row)
		for (int col = c0; col < c1; ++col)
		{
			// the bake's diagonal (0,0)-(1,1), triangles (v00, v01, v11) and (v00, v11, v10)
			out.push_back({ vertex(col, row), vertex(col, row + 1), vertex(col + 1, row + 1) });
			out.push_back({ vertex(col, row), vertex(col + 1, row + 1), vertex(col + 1, row) });
		}
	return out;
}

native::Mesh assembleWorld(const Input& input, const Settings& s, Report* report)
{
	Report local;
	std::vector<Piece> recast;
	RecastFaces recastIn;
	if (input.recast)
	{
		recastIn = recastFaces(*input.recast);
		recast = piecesOf(recastIn.mesh);
	}
	local.recastFaces = recast.size();

	// generated surfaces that cut the Recast faces: the interiors' and the zone side's
	std::vector<Piece> generated;
	for (const native::Mesh* mesh : input.interiors)
		for (Piece& p : piecesOf(*mesh))
			generated.push_back(std::move(p));
	std::vector<Piece> zone;
	for (const native::Mesh* mesh : input.zoneSide)
		for (Piece& p : piecesOf(*mesh))
			zone.push_back(std::move(p));
	for (const Piece& p : zone)
		generated.push_back(p);
	local.generatedFaces = zone.size();

	const std::size_t before = recast.size();
	for (const Piece& g : generated)
	{
		const Plane gp = fit(g.pts);
		local.recastRemovedArea += cut(recast, g.pts,
			[&](const Plane& rp)
			{
				// d = generated height - recast height at the point
				const auto d = [gp, rp](const P3& v) { return gp.at(v[0], v[2]) - rp.at(v[0], v[2]); };
				return std::make_pair(
					[d, base = s.cutBase](const P3& v) { return d(v) + base; },
					[d, h = s.characterHeight](const P3& v) { return h - d(v); });
			}, s.minPieceArea);
	}
	for (const Cutter& c : input.cutters)
	{
		Poly region;
		for (const auto& v : c.footprint)
			region.push_back({ v[0], 0.0, v[1] });
		orient(region);
		local.recastRemovedArea += cut(recast, region,
			[&](const Plane& rp)
			{
				const auto h = [rp](const P3& v) { return rp.at(v[0], v[2]); };
				return std::make_pair(
					[h, low = c.low, up = s.characterHeight](const P3& v) { return h(v) - low + up; },
					[h, high = c.high](const P3& v) { return high - h(v); });
			}, s.minPieceArea);
	}
	local.recastCut = recast.size() > before ? recast.size() - before : 0;

	// terrain cuts the zone-side faces under ground
	const std::size_t zoneBefore = zone.size();
	for (const auto& tri : input.terrain)
	{
		Poly region(tri.begin(), tri.end());
		orient(region);
		const Plane tp = fit(region);
		cut(zone, region,
			[&](const Plane& gp)
			{
				const auto d = [tp, gp](const P3& v) { return tp.at(v[0], v[2]) - gp.at(v[0], v[2]) - 0.0; };
				return std::make_pair(
					[d, tol = s.undergroundTolerance](const P3& v) { return d(v) - tol; },
					[](const P3&) { return 1.0; });
			}, s.minPieceArea);
	}
	local.generatedUnderground = zone.size() > zoneBefore ? zone.size() - zoneBefore : 0;

	std::vector<Piece> all = std::move(recast);
	for (Piece& p : zone)
		all.push_back(std::move(p));
	native::Mesh mesh = meshOf(all, s.lattice);
	mesh.tileX = input.tile.x;
	mesh.tileZ = input.tile.z;
	local.worldFaces = mesh.faces.size();
	if (report)
		*report = local;
	return mesh;
}

std::vector<BakedTile> assembleTile(const Input& input, const Settings& s, Report* report, const NativeDoorKeys& doorKeys, NativeTileStats* stats)
{
	std::vector<native::Mesh> meshes;
	meshes.push_back(assembleWorld(input, s, report));
	for (const native::Mesh* interior : input.interiors)
		meshes.push_back(*interior);
	const RecastFaces recastIn = input.recast ? recastFaces(*input.recast) : RecastFaces{};
	const NativeDoorKeys keys = [&](const std::array<double, 3>& centre) -> DoorKey
	{
		for (const auto& [key, outline] : recastIn.doors)
		{
			bool inside = true;
			Poly p(outline.begin(), outline.end());
			orient(p);
			for (std::size_t i = 0; i < p.size() && inside; ++i)
			{
				const P3 &a = p[i], &b = p[(i + 1) % p.size()];
				inside = (b[0] - a[0]) * (centre[2] - a[2]) - (b[2] - a[2]) * (centre[0] - a[0]) >= -1e-3;
			}
			if (inside)
				return key;
		}
		return doorKeys ? doorKeys(centre) : 0;
	};
	return nativeTiles(*input.config, meshes, { input.tile }, keys, stats);
}

} // namespace knav::assembly
