#include "knav/native_tiles.hpp"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

namespace knav {

namespace {

constexpr double kCell = 0.125; // xz and y quantum of the tile vertices: a power of two, so tile borders stay exact
constexpr double kCollapse = 0.25; // native edges shorter than this become one corner
constexpr double kLinkSnap = 1.0; // doorway corners of an interior and its outside closer than this become one
constexpr int kMaxCorners = 6; // Detour's DT_VERTS_PER_POLYGON
constexpr unsigned short kNoNeighbour = 0xffff; // RC_MESH_NULL_IDX

// What an edge of a piece was in the original face: it decides what an edge left without a partner becomes.
enum class EdgeKind : std::uint8_t
{
	Wall, // a native wall
	Shared, // a native edge with an opposite face
	Linked, // a native streaming link (zone seam, interior <-> world)
	Diagonal, // inside the face: the ear clipping's
	Border, // made by the clip to the tile square
};

// An edge's kind and, for a native edge, which one (its mesh and index): the original's opposite edges need not share
// corners (edge matching joins edges up to a 5 u step apart, the Hub bar's back stair top), so they are paired by identity.
struct EdgeTag
{
	EdgeKind kind = EdgeKind::Diagonal;
	std::uint32_t mesh = 0;
	std::int32_t edge = -1;

	EdgeTag(EdgeKind k = EdgeKind::Diagonal, std::uint32_t m = 0, std::int32_t e = -1) : kind(k), mesh(m), edge(e) {}
};

struct Point3
{
	double x = 0.0, y = 0.0, z = 0.0;
};

// Twice the signed xz area of a, b, c. Detour's polygons run with this negative (Recast's corner order).
double cross(const Point3& a, const Point3& b, const Point3& c)
{
	return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}

// A polygon with the kind of the edge leaving each corner.
struct Outline
{
	std::vector<Point3> points;
	std::vector<EdgeTag> kinds;
};

// A corner that turns by more than kStraight (sine of the angle): one on a straight edge would make a zero-area ear, which
// quantizes away and leaves its corner a T-junction in the neighbour's edge.
bool convexCorner(const Point3& a, const Point3& b, const Point3& c)
{
	constexpr double kStraight = 1e-3;
	return cross(a, b, c) < -kStraight * std::hypot(b.x - a.x, b.z - a.z) * std::hypot(c.x - b.x, c.z - b.z);
}

// Inside or on the border of triangle a, b, c and not one of its corners: a corner on the diagonal c-a would be cut off.
bool blocksEar(const Point3& p, const Point3& a, const Point3& b, const Point3& c)
{
	const auto same = [&p](const Point3& v) { return p.x == v.x && p.z == v.z; };
	return !same(a) && !same(b) && !same(c) && cross(a, b, p) <= 0.0 && cross(b, c, p) <= 0.0 && cross(c, a, p) <= 0.0;
}

// Twice the signed xz area of an outline (negative for Detour's winding).
double outlineArea(const Outline& o)
{
	double area = 0.0;
	for (std::size_t i = 1; i + 1 < o.points.size(); ++i)
		area += cross(o.points[0], o.points[i], o.points[i + 1]);
	return area;
}

// Ear clipping of a negative-wound simple outline. False when no ear is left over a remainder that still has area (the outline is not simple):
// `out` is then empty and the face is left out of the tile, never fanned (a fan over a non-simple outline lays triangles over its neighbours,
// kenshi-k16.7.207). A remainder with no area (corners on one line) is cut down to zero-width triangles, the bridges they are.
bool earClip(Outline outline, std::vector<Outline>& out)
{
	while (outline.points.size() > 3)
	{
		const std::size_t n = outline.points.size();
		std::size_t ear = n;
		for (std::size_t i = 0; i < n && ear == n; ++i)
		{
			const Point3& a = outline.points[(i + n - 1) % n];
			const Point3& b = outline.points[i];
			const Point3& c = outline.points[(i + 1) % n];
			if (!convexCorner(a, b, c))
				continue;
			bool empty = true;
			for (std::size_t k = 0; k < n && empty; ++k)
				if (k != i && k != (i + n - 1) % n && k != (i + 1) % n)
					empty = !blocksEar(outline.points[k], a, b, c);
			if (empty)
				ear = i;
		}
		if (ear == n)
		{
			if (std::fabs(outlineArea(outline)) > 1e-3)
			{
				out.clear();
				return false;
			}
			ear = 1; // a remainder with no area (corners on one line): cut down to triangles as before, they are zero-width bridges
		}
		const std::size_t prev = (ear + n - 1) % n;
		const std::size_t next = (ear + 1) % n;
		out.push_back({ { outline.points[prev], outline.points[ear], outline.points[next] },
			{ outline.kinds[prev], outline.kinds[ear], EdgeKind::Diagonal } });
		outline.kinds[prev] = EdgeKind::Diagonal;
		outline.points.erase(outline.points.begin() + static_cast<std::ptrdiff_t>(ear));
		outline.kinds.erase(outline.kinds.begin() + static_cast<std::ptrdiff_t>(ear));
	}
	out.push_back(std::move(outline));
	return true;
}

// An outline made simple (kenshi-k16.7.207): corners where the outline folds back onto itself (a spike: two edges on one line, running the same way
// out and back) are dropped, and an outline that touches itself at a repeated corner (a pinch) is split there into one outline per lobe. False when a
// lobe winds the other way (a hole) or two edges still cross: the face is not simple and is left out.
bool simplify(Outline outline, std::vector<Outline>& loops)
{
	std::vector<Outline> work;
	work.push_back(std::move(outline));
	while (!work.empty())
	{
		Outline o = std::move(work.back());
		work.pop_back();
		// spikes
		for (bool again = true; again && o.points.size() >= 3;)
		{
			again = false;
			const std::size_t n = o.points.size();
			for (std::size_t i = 0; i < n; ++i)
			{
				const Point3& a = o.points[(i + n - 1) % n];
				const Point3& b = o.points[i];
				const Point3& c = o.points[(i + 1) % n];
				const double la = std::hypot(a.x - b.x, a.z - b.z), lc = std::hypot(c.x - b.x, c.z - b.z);
				if (la <= 0.0 || lc <= 0.0)
					continue;
				if (std::fabs(cross(a, b, c)) <= 1e-3 * la * lc && (a.x - b.x) * (c.x - b.x) + (a.z - b.z) * (c.z - b.z) > 0.0)
				{
					o.points.erase(o.points.begin() + static_cast<std::ptrdiff_t>(i));
					o.kinds.erase(o.kinds.begin() + static_cast<std::ptrdiff_t>(i));
					again = true;
					break;
				}
			}
		}
		if (o.points.size() < 3)
			continue;
		// a pinch: a corner repeated
		const std::size_t n = o.points.size();
		bool split = false;
		for (std::size_t i = 0; i < n && !split; ++i)
			for (std::size_t j = i + 1; j < n && !split; ++j)
				if (o.points[i].x == o.points[j].x && o.points[i].z == o.points[j].z)
				{
					Outline inner, outer;
					for (std::size_t k = i; k < j; ++k)
					{
						inner.points.push_back(o.points[k]);
						inner.kinds.push_back(o.kinds[k]);
					}
					for (std::size_t k = 0; k < n; ++k)
						if (k < i || k >= j)
						{
							outer.points.push_back(o.points[k]);
							outer.kinds.push_back(o.kinds[k]);
						}
					work.push_back(std::move(inner));
					work.push_back(std::move(outer));
					split = true;
				}
		if (split)
			continue;
		const double area = outlineArea(o);
		if (area > 1e-3)
			return false; // a lobe the other way round: a hole
		if (std::fabs(area) <= 1e-3)
		{
			loops.push_back(std::move(o)); // no area: kept as it is (the zero-width bridge between its neighbours)
			continue;
		}
		// no two edges cross
		for (std::size_t i = 0; i < n; ++i)
			for (std::size_t j = i + 2; j < n; ++j)
			{
				if (i == 0 && j == n - 1)
					continue;
				const Point3& a = o.points[i]; const Point3& b = o.points[(i + 1) % n]; const Point3& c = o.points[j]; const Point3& d = o.points[(j + 1) % n];
				const double o1 = cross(a, b, c), o2 = cross(a, b, d), o3 = cross(c, d, a), o4 = cross(c, d, b);
				if (((o1 > 1e-6 && o2 < -1e-6) || (o1 < -1e-6 && o2 > 1e-6)) && ((o3 > 1e-6 && o4 < -1e-6) || (o3 < -1e-6 && o4 > 1e-6)))
					return false;
			}
		loops.push_back(std::move(o));
	}
	return true;
}

// The crossing of segment p-q with the line axis = value, interpolated from the lower endpoint, so the two faces that
// share the segment get the same point.
Point3 crossing(Point3 p, Point3 q, int axis, double value)
{
	const auto key = [](const Point3& v) { return std::array<double, 3>{ v.x, v.z, v.y }; };
	if (key(q) < key(p))
		std::swap(p, q);
	const double from = axis == 0 ? p.x : p.z;
	const double to = axis == 0 ? q.x : q.z;
	const double t = (value - from) / (to - from);
	Point3 r{ p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t };
	(axis == 0 ? r.x : r.z) = value;
	return r;
}

// Sutherland-Hodgman against one side of the tile square: keeps axis >= value (keepAbove) or <= value.
Outline clipSide(const Outline& in, int axis, double value, bool keepAbove)
{
	Outline out;
	const std::size_t n = in.points.size();
	const auto inside = [&](const Point3& v)
	{
		const double c = axis == 0 ? v.x : v.z;
		return keepAbove ? c >= value : c <= value;
	};
	for (std::size_t i = 0; i < n; ++i)
	{
		const Point3& p = in.points[i];
		const Point3& q = in.points[(i + 1) % n];
		const bool pIn = inside(p);
		const bool qIn = inside(q);
		// out.kinds[k] is the edge leaving out.points[k]: set when the next corner is known.
		if (pIn)
		{
			out.points.push_back(p);
			out.kinds.push_back(in.kinds[i]);
			if (!qIn)
			{
				out.points.push_back(crossing(p, q, axis, value));
				out.kinds.push_back(EdgeKind::Border);
			}
		} else if (qIn) {
			out.points.push_back(crossing(p, q, axis, value));
			out.kinds.push_back(in.kinds[i]);
		}
	}
	return out;
}

struct Piece
{
	std::vector<int> corners; // tile vertex ids, negative winding
	std::vector<EdgeTag> kinds; // the edge leaving each corner
	std::vector<std::array<int, 3>> triangles; // vertex ids
	// No area left on the tile's vertices: kept as its own polygon (never merged), a zero-width bridge between the faces on
	// either side, which would otherwise meet only at a T-junction.
	bool sliver = false;
};

struct QuantVertex
{
	int x = 0, y = 0, z = 0;
};

struct TileBuilder
{
	double x0 = 0.0, z0 = 0.0, floor = 0.0;
	std::vector<QuantVertex> verts;
	std::unordered_map<std::uint64_t, int> ids;

	int vertex(const Point3& p)
	{
		const QuantVertex q{ static_cast<int>(std::lround((p.x - x0) / kCell)), static_cast<int>(std::lround((p.y - floor) / kCell)),
			static_cast<int>(std::lround((p.z - z0) / kCell)) };
		const std::uint64_t key = static_cast<std::uint64_t>(q.x) << 42 | static_cast<std::uint64_t>(q.y) << 21 | static_cast<std::uint64_t>(q.z);
		const auto [it, added] = ids.try_emplace(key, static_cast<int>(verts.size()));
		if (added)
			verts.push_back(q);
		return it->second;
	}
	long long cross(int a, int b, int c) const
	{
		const QuantVertex& va = verts[static_cast<std::size_t>(a)];
		const QuantVertex& vb = verts[static_cast<std::size_t>(b)];
		const QuantVertex& vc = verts[static_cast<std::size_t>(c)];
		return static_cast<long long>(vb.x - va.x) * (vc.z - va.z) - static_cast<long long>(vb.z - va.z) * (vc.x - va.x);
	}
};

// The piece of a clipped convex outline on the tile's vertices, its zero-length edges gone; empty when nothing with area
// is left. Seven corners (a triangle cut by all four sides) are split in two.
void addPieces(TileBuilder& tile, const Outline& outline, std::vector<Piece>& out)
{
	Piece piece;
	for (std::size_t i = 0; i < outline.points.size(); ++i)
	{
		const int id = tile.vertex(outline.points[i]);
		if (!piece.corners.empty() && piece.corners.back() == id)
		{
			piece.kinds.back() = outline.kinds[i];
			continue;
		}
		piece.corners.push_back(id);
		piece.kinds.push_back(outline.kinds[i]);
	}
	while (piece.corners.size() > 1 && piece.corners.back() == piece.corners.front())
	{
		piece.corners.pop_back();
		piece.kinds.pop_back();
	}
	const std::size_t n = piece.corners.size();
	if (n < 3)
		return;
	long long area = 0;
	for (std::size_t i = 1; i + 1 < n; ++i)
		area += tile.cross(piece.corners[0], piece.corners[i], piece.corners[i + 1]);
	piece.sliver = area >= 0;
	const auto fan = [&tile](Piece& convex)
	{
		for (std::size_t i = 1; i + 1 < convex.corners.size(); ++i)
			if (convex.sliver || tile.cross(convex.corners[0], convex.corners[i], convex.corners[i + 1]) < 0)
				convex.triangles.push_back({ convex.corners[0], convex.corners[i], convex.corners[i + 1] });
	};
	if (n <= kMaxCorners)
	{
		fan(piece);
		out.push_back(std::move(piece));
		return;
	}
	// Corners 0..3 and 3..n-1, 0: the diagonal 3-0 between them.
	Piece first;
	Piece second;
	first.sliver = second.sliver = piece.sliver;
	first.corners.assign(piece.corners.begin(), piece.corners.begin() + 4);
	first.kinds.assign(piece.kinds.begin(), piece.kinds.begin() + 4);
	first.kinds[3] = EdgeKind::Diagonal;
	second.corners.assign(piece.corners.begin() + 3, piece.corners.end());
	second.corners.push_back(piece.corners[0]);
	second.kinds.assign(piece.kinds.begin() + 3, piece.kinds.end());
	second.kinds.push_back(EdgeKind::Diagonal);
	fan(first);
	fan(second);
	out.push_back(std::move(first));
	out.push_back(std::move(second));
}

// Merges pieces of one face pairwise across a shared edge while the result stays convex with at most six corners, longest
// shared edge first (Recast's merge rule).
void mergePieces(const TileBuilder& tile, std::vector<Piece>& pieces)
{
	for (;;)
	{
		long long best = -1;
		std::size_t bestA = 0, bestB = 0, edgeA = 0, edgeB = 0;
		for (std::size_t a = 0; a < pieces.size(); ++a)
			for (std::size_t b = a + 1; b < pieces.size(); ++b)
			{
				const Piece& pa = pieces[a];
				const Piece& pb = pieces[b];
				const std::size_t na = pa.corners.size();
				const std::size_t nb = pb.corners.size();
				if (na + nb - 2 > static_cast<std::size_t>(kMaxCorners) || pa.sliver || pb.sliver)
					continue;
				for (std::size_t i = 0; i < na; ++i)
					for (std::size_t j = 0; j < nb; ++j)
					{
						if (pa.corners[i] != pb.corners[(j + 1) % nb] || pa.corners[(i + 1) % na] != pb.corners[j])
							continue;
						// Convex at the two ends of the shared edge (the other corners are the pieces' own).
						const int va = pa.corners[i];
						const int vb = pa.corners[(i + 1) % na];
						if (tile.cross(pa.corners[(i + na - 1) % na], va, pb.corners[(j + 2) % nb]) > 0
							|| tile.cross(pb.corners[(j + nb - 1) % nb], vb, pa.corners[(i + 2) % na]) > 0)
							continue;
						const QuantVertex& p = tile.verts[static_cast<std::size_t>(va)];
						const QuantVertex& q = tile.verts[static_cast<std::size_t>(vb)];
						const long long length = static_cast<long long>(p.x - q.x) * (p.x - q.x) + static_cast<long long>(p.z - q.z) * (p.z - q.z);
						if (length > best)
						{
							best = length;
							bestA = a;
							bestB = b;
							edgeA = i;
							edgeB = j;
						}
					}
			}
		if (best < 0)
			return;
		Piece& pa = pieces[bestA];
		Piece& pb = pieces[bestB];
		const std::size_t na = pa.corners.size();
		const std::size_t nb = pb.corners.size();
		Piece merged;
		for (std::size_t k = 0; k + 1 < na; ++k)
		{
			merged.corners.push_back(pa.corners[(edgeA + 1 + k) % na]);
			merged.kinds.push_back(pa.kinds[(edgeA + 1 + k) % na]);
		}
		for (std::size_t k = 0; k + 1 < nb; ++k)
		{
			merged.corners.push_back(pb.corners[(edgeB + 1 + k) % nb]);
			merged.kinds.push_back(pb.kinds[(edgeB + 1 + k) % nb]);
		}
		merged.triangles = pa.triangles;
		merged.triangles.insert(merged.triangles.end(), pb.triangles.begin(), pb.triangles.end());
		pa = std::move(merged);
		pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(bestB));
	}
}

struct TilePoly
{
	Piece piece;
	Area area = Area::Land;
	DoorKey door = 0;
	bool top = false; // from the source face (census only)
	std::uint32_t srcFace = 0; // ... and its index in its mesh (census only)
	std::uint32_t srcMesh = 0;
};

// A mesh's vertex positions with the two ends of every opposite-edge pair moved onto one vertex (the original's opposite
// edges hold their own copies of a corner, which can sit across a 1/8 u quantum from each other), and the ends of every edge
// shorter than kCollapse too: a sliver face would quantize to nothing and leave its two neighbours a crack apart. Only
// corners closer than kCollapse join: some opposite edges run the same way as their partner (a folded pair of faces).
std::vector<std::array<double, 3>> weldedVertices(const native::Mesh& mesh)
{
	std::vector<std::uint32_t> parent(mesh.vertices.size());
	for (std::size_t v = 0; v < parent.size(); ++v)
		parent[v] = static_cast<std::uint32_t>(v);
	const auto root = [&parent](std::uint32_t v)
	{
		while (parent[v] != v)
			v = parent[v] = parent[parent[v]];
		return v;
	};
	const auto near = [&mesh](std::uint32_t a, std::uint32_t b)
	{
		const auto& p = mesh.vertices[a];
		const auto& q = mesh.vertices[b];
		return std::hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]) < kCollapse;
	};
	const auto join = [&](std::uint32_t a, std::uint32_t b)
	{
		if (!near(a, b))
			return;
		a = root(a);
		b = root(b);
		if (a != b)
			parent[std::max(a, b)] = std::min(a, b);
	};
	for (const native::Edge& edge : mesh.edges)
	{
		if (edge.oppositeEdge < 0 || static_cast<std::size_t>(edge.oppositeEdge) >= mesh.edges.size())
			continue;
		const native::Edge& opposite = mesh.edges[static_cast<std::size_t>(edge.oppositeEdge)];
		join(edge.a, opposite.b);
		join(edge.b, opposite.a);
	}
	for (const native::Edge& edge : mesh.edges)
	{
		join(edge.a, edge.b);
	}
	std::vector<std::array<double, 3>> out(mesh.vertices.size());
	for (std::size_t v = 0; v < out.size(); ++v)
		out[v] = mesh.vertices[root(static_cast<std::uint32_t>(v))];
	return out;
}

// The corners of an interior's streaming-link edges and the matching corners of the mesh it opens onto moved onto one point
// (the first seen within kLinkSnap): each mesh holds its own copy of the doorway's corners, up to 0.3 u apart (the Hub bar's
// front door), and only corners on one tile vertex join. Zone seams are left alone: they lie on tile borders, where
// Detour's portals join them.
void snapLinks(const std::vector<native::Mesh>& meshes, std::vector<std::vector<std::array<double, 3>>>& vertices)
{
	const auto cellOf = [](double v) { return static_cast<long long>(std::floor(v / kLinkSnap)); };
	const auto cellKey = [](long long x, long long y, long long z)
	{
		return static_cast<std::uint64_t>(x & 0x1fffff) << 42 | static_cast<std::uint64_t>(y & 0x1fffff) << 21 | static_cast<std::uint64_t>(z & 0x1fffff);
	};
	struct Corner
	{
		std::size_t mesh;
		std::array<double, 3> at;
	};
	std::unordered_map<std::uint64_t, std::vector<Corner>> grid;
	for (std::size_t m = 0; m < meshes.size(); ++m)
		for (const native::Edge& edge : meshes[m].edges)
		{
			if (!edge.linked)
				continue;
			for (const std::uint32_t v : { edge.a, edge.b })
			{
				std::array<double, 3>& at = vertices[m][v];
				const long long cx = cellOf(at[0]), cy = cellOf(at[1]), cz = cellOf(at[2]);
				const Corner* match = nullptr;
				for (long long dx = -1; dx <= 1 && !match; ++dx)
					for (long long dy = -1; dy <= 1 && !match; ++dy)
						for (long long dz = -1; dz <= 1 && !match; ++dz)
							if (const auto it = grid.find(cellKey(cx + dx, cy + dy, cz + dz)); it != grid.end())
								for (const Corner& corner : it->second)
									if (corner.mesh != m && (meshes[m].interior() || meshes[corner.mesh].interior())
										&& std::hypot(corner.at[0] - at[0], corner.at[1] - at[1], corner.at[2] - at[2]) < kLinkSnap)
									{
										match = &corner;
										break;
									}
				if (match)
					at = match->at;
				else
					grid[cellKey(cx, cy, cz)].push_back({ m, at });
			}
		}
}

// Streaming-link edges of an interior and of the mesh it opens onto run along the same line but not always over the same span
// (the Hub-side Bars' doorways: the interior's door edge is 2 u shorter at one end), so snapLinks' corner match leaves the
// shorter edge's end on the middle of the longer one and the two never join (the interior is an island). Each such end is a
// point the longer edge is split at, so the overlapping stretches share their corners. Key: mesh << 32 | edge index.
using LinkSplits = std::unordered_map<std::uint64_t, std::vector<std::array<double, 3>>>;

LinkSplits splitLinkedEdges(const std::vector<native::Mesh>& meshes, const std::vector<std::vector<std::array<double, 3>>>& vertices)
{
	constexpr double kLine = 0.4; // an end this close to the other edge's line (xz) is on it
	constexpr double kEnd = 0.05; // ... and this far from that edge's own ends is a split, not a corner
	constexpr double kLevel = 3.0; // the edges must be at one height there
	struct Link
	{
		std::size_t mesh = 0;
		std::size_t edge = 0;
		std::array<double, 3> a{}, b{};
	};
	std::vector<Link> links;
	std::vector<std::size_t> interiorLinks;
	for (std::size_t m = 0; m < meshes.size(); ++m)
		for (std::size_t e = 0; e < meshes[m].edges.size(); ++e)
		{
			const native::Edge& edge = meshes[m].edges[e];
			if (!edge.linked || edge.oppositeFace >= 0)
				continue;
			if (meshes[m].interior())
				interiorLinks.push_back(links.size());
			links.push_back({ m, e, vertices[m][edge.a], vertices[m][edge.b] });
		}
	LinkSplits splits;
	const auto lengthOf = [](const Link& l) { return std::hypot(l.b[0] - l.a[0], l.b[2] - l.a[2]); };
	// The end `p` of `from` as a split of `onto`, when it lies on onto's line strictly inside it.
	const auto offer = [&](const Link& onto, const std::array<double, 3>& p)
	{
		const double length = lengthOf(onto);
		if (length < 1e-6)
			return;
		const double dx = (onto.b[0] - onto.a[0]) / length, dz = (onto.b[2] - onto.a[2]) / length;
		const double along = (p[0] - onto.a[0]) * dx + (p[2] - onto.a[2]) * dz;
		const double off = std::fabs((p[0] - onto.a[0]) * dz - (p[2] - onto.a[2]) * dx);
		if (off > kLine || along < kEnd || along > length - kEnd)
			return;
		const double y = onto.a[1] + (onto.b[1] - onto.a[1]) * along / length;
		if (std::fabs(y - p[1]) > kLevel)
			return;
		std::vector<std::array<double, 3>>& list = splits[static_cast<std::uint64_t>(onto.mesh) << 32 | onto.edge];
		for (const std::array<double, 3>& q : list)
			if (std::hypot(q[0] - p[0], q[2] - p[2]) < kEnd)
				return;
		list.push_back(p);
	};
	for (const std::size_t i : interiorLinks)
		for (const Link& other : links)
		{
			const Link& mine = links[i];
			if (other.mesh == mine.mesh || meshes[other.mesh].interior())
				continue;
			// Cheap box test before the line tests.
			if (std::max(mine.a[0], mine.b[0]) < std::min(other.a[0], other.b[0]) - kLine || std::min(mine.a[0], mine.b[0]) > std::max(other.a[0], other.b[0]) + kLine
				|| std::max(mine.a[2], mine.b[2]) < std::min(other.a[2], other.b[2]) - kLine || std::min(mine.a[2], mine.b[2]) > std::max(other.a[2], other.b[2]) + kLine)
				continue;
			offer(other, mine.a);
			offer(other, mine.b);
			offer(mine, other.a);
			offer(mine, other.b);
		}
	for (auto& [key, list] : splits)
	{
		const Link* link = nullptr;
		for (const Link& l : links)
			if ((static_cast<std::uint64_t>(l.mesh) << 32 | l.edge) == key)
				link = &l;
		std::sort(list.begin(), list.end(), [&](const auto& l, const auto& r)
		{
			return (l[0] - link->a[0]) * (link->b[0] - link->a[0]) + (l[2] - link->a[2]) * (link->b[2] - link->a[2])
				< (r[0] - link->a[0]) * (link->b[0] - link->a[0]) + (r[2] - link->a[2]) * (link->b[2] - link->a[2]);
		});
	}
	return splits;
}

// One face, as the outline of its corners with Detour's winding.
Outline faceOutline(const native::Mesh& mesh, std::uint32_t meshIndex, const std::vector<std::array<double, 3>>& vertices,
	const native::Face& face, const LinkSplits& splits)
{
	Outline outline;
	for (std::uint32_t k = 0; k < face.numEdges; ++k)
	{
		const std::size_t e = face.startEdge + k;
		const auto& v = vertices[mesh.edges[e].a];
		const EdgeTag kind(mesh.wall(e) ? EdgeKind::Wall : mesh.edges[e].linked ? EdgeKind::Linked : EdgeKind::Shared, meshIndex,
			static_cast<std::int32_t>(e));
		// A collapsed edge: one corner, leaving by the next edge.
		if (!outline.points.empty() && outline.points.back().x == v[0] && outline.points.back().y == v[1] && outline.points.back().z == v[2])
		{
			outline.kinds.back() = kind;
			continue;
		}
		outline.points.push_back({ v[0], v[1], v[2] });
		outline.kinds.push_back(kind);
		// The points a streaming link is split at (splitLinkedEdges), along the edge, each leaving by the same edge.
		if (const auto split = splits.find(static_cast<std::uint64_t>(meshIndex) << 32 | e); split != splits.end())
			for (const std::array<double, 3>& p : split->second)
			{
				outline.points.push_back({ p[0], p[1], p[2] });
				outline.kinds.push_back(kind);
			}
	}
	if (outline.points.size() > 1 && outline.points.back().x == outline.points.front().x && outline.points.back().y == outline.points.front().y
		&& outline.points.back().z == outline.points.front().z)
	{
		outline.points.pop_back();
		outline.kinds.pop_back();
	}
	double area = 0.0;
	const std::size_t n = outline.points.size();
	for (std::size_t i = 1; i + 1 < n; ++i)
		area += cross(outline.points[0], outline.points[i], outline.points[i + 1]);
	if (area > 0.0)
	{
		// Reversed: corner i is old n-1-i, and the edge leaving it the old edge into that corner.
		Outline reversed;
		for (std::size_t i = 0; i < n; ++i)
		{
			reversed.points.push_back(outline.points[n - 1 - i]);
			reversed.kinds.push_back(outline.kinds[(2 * n - 2 - i) % n]);
		}
		return reversed;
	}
	return outline;
}

struct FaceRef
{
	std::uint32_t mesh = 0;
	std::uint32_t face = 0;
};

std::optional<BakedTile> buildTile(const NavConfig& c, const std::vector<native::Mesh>& meshes,
	const std::vector<std::vector<std::array<double, 3>>>& vertices, const LinkSplits& splits, const std::vector<FaceRef>& faces, int tx, int tz, const NativeDoorKeys& doorKeys, NativeTileStats& stats)
{
	const double x0 = c.originX + tx * static_cast<double>(c.tileSize);
	const double z0 = c.originZ + tz * static_cast<double>(c.tileSize);
	const double x1 = x0 + c.tileSize;
	const double z1 = z0 + c.tileSize;

	// Clipped triangles per face, in world coordinates; the tile floor is their lowest point.
	struct Clipped
	{
		std::size_t face = 0;
		std::vector<Outline> outlines;
	};
	std::vector<Clipped> clipped;
	double floor = std::numeric_limits<double>::max();
	double top = std::numeric_limits<double>::lowest();
	for (std::size_t f = 0; f < faces.size(); ++f)
	{
		const native::Mesh& mesh = meshes[faces[f].mesh];
		const native::Face& face = mesh.faces[faces[f].face];
		if (face.numEdges < 3)
			continue;
		std::vector<Outline> triangles;
		std::vector<Outline> loops;
		bool placed = simplify(faceOutline(mesh, faces[f].mesh, vertices[faces[f].mesh], face, splits), loops);
		for (const Outline& loop : loops)
			placed = placed && earClip(loop, triangles);
		if (!placed)
		{
			// not a simple outline: left out of the tile (a hole is better than a fold), counted and listed
			triangles.clear();
			++stats.fanned;
			if (faces[f].mesh == 0)
				stats.fannedFaces.push_back(faces[f].face);
		}
		Clipped entry{ f, {} };
		for (const Outline& triangle : triangles)
		{
			Outline piece = clipSide(clipSide(clipSide(clipSide(triangle, 0, x0, true), 0, x1, false), 1, z0, true), 1, z1, false);
			if (piece.points.size() < 3)
				continue;
			for (const Point3& p : piece.points)
			{
				floor = std::min(floor, p.y);
				top = std::max(top, p.y);
			}
			entry.outlines.push_back(std::move(piece));
		}
		if (!entry.outlines.empty())
			clipped.push_back(std::move(entry));
	}
	if (clipped.empty())
		return std::nullopt;

	TileBuilder tile;
	tile.x0 = x0;
	tile.z0 = z0;
	tile.floor = std::floor(floor / kCell) * kCell;
	std::vector<TilePoly> polys;
	for (const Clipped& entry : clipped)
	{
		std::vector<Piece> pieces;
		for (const Outline& outline : entry.outlines)
			addPieces(tile, outline, pieces);
		mergePieces(tile, pieces);
		if (!pieces.empty())
			++stats.faces;
		const native::Face& face = meshes[faces[entry.face].mesh].faces[faces[entry.face].face];
		for (Piece& piece : pieces)
		{
			stats.slivers += piece.sliver ? 1 : 0;
			TilePoly poly;
			poly.top = face.top;
			poly.srcFace = faces[entry.face].face;
			poly.srcMesh = faces[entry.face].mesh;
			std::array<double, 3> centre{};
			for (const int id : piece.corners)
			{
				const QuantVertex& v = tile.verts[static_cast<std::size_t>(id)];
				centre[0] += x0 + v.x * kCell;
				centre[1] += tile.floor + v.y * kCell;
				centre[2] += z0 + v.z * kCell;
			}
			for (double& axis : centre)
				axis /= static_cast<double>(piece.corners.size());
			if (face.data == 4 || face.data == 5)
			{
				poly.area = Area::DoorOpen;
				poly.door = face.door != 0 ? face.door : doorKeys ? doorKeys(centre) : 0; // the generator keys its door faces itself
				++stats.doorPolys;
				if (poly.door == 0)
					++stats.unkeyedDoors;
			} else {
				// The bake's water rule, on the polygon's mean height.
				const double depth = c.waterLevel - centre[1];
				poly.area = depth >= c.swimDepth ? Area::Swim : depth >= c.wadeDepth ? Area::Shallow : Area::Land;
			}
			poly.piece = std::move(piece);
			polys.push_back(std::move(poly));
		}
	}
	// Coincident polygons (the same corners, any winding: a door piece the interior and the zone side both hold, a zone-side face over its twin)
	// are one: two or four copies neighbour each other over every edge and Detour's raycast, which has no iteration cap, bounces between them for
	// ever when its ray ends outside the pair (ksim_newgame_starts_0 ran 25 min in a door route; kenshi-k16.7.145). The door copy keeps its key.
	{
		std::map<std::vector<int>, std::size_t> seen;
		std::vector<TilePoly> unique;
		for (TilePoly& poly : polys)
		{
			std::vector<int> key = poly.piece.corners;
			std::sort(key.begin(), key.end());
			const auto [at, inserted] = seen.try_emplace(std::move(key), unique.size());
			if (inserted)
			{
				unique.push_back(std::move(poly));
				continue;
			}
			TilePoly& kept = unique[at->second];
			if (poly.area == Area::DoorOpen && kept.area != Area::DoorOpen)
			{
				kept.area = poly.area;
				kept.door = poly.door;
			}
			++stats.duplicates;
		}
		polys = std::move(unique);
	}
	if (polys.empty())
		return std::nullopt;
	stats.maxPolys = std::max(stats.maxPolys, static_cast<int>(polys.size()));
	stats.tilePolys.push_back({ tx, tz, static_cast<int>(polys.size()) });
	{
		std::vector<std::uint8_t>& marks = stats.topPolys[{ tx, tz }];
		std::vector<std::uint32_t>& src = stats.polyFace[{ tx, tz }];
		for (const TilePoly& poly : polys)
		{
			marks.push_back(poly.top);
			src.push_back(poly.srcMesh == 0 ? poly.srcFace : 0xffffffffu); // the world mesh is mesh 0
		}
	}
	if (polys.size() > static_cast<std::size_t>(kMaxPolysPerTile) || tile.verts.size() >= 0xffff)
	{
		++stats.overfull;
		return std::nullopt;
	}
	stats.polys += polys.size();

	// Neighbours: the reversed edge; else, for a native edge with an opposite, the polygon edge cut from that opposite which
	// overlaps this one most along it; else (not a wall) another polygon with the same edge the same way round (a folded
	// pair of faces, opposite in the original); else a portal on the tile side both ends lie on; else a wall.
	std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> edgeOwners;
	std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint32_t>>> nativeOwners; // (poly, edge)
	const auto edgeKey = [](std::uint32_t a, std::uint32_t b) { return static_cast<std::uint64_t>(a) << 32 | b; };
	for (std::size_t p = 0; p < polys.size(); ++p)
	{
		const auto& corners = polys[p].piece.corners;
		for (std::size_t k = 0; k < corners.size(); ++k)
		{
			edgeOwners[edgeKey(static_cast<std::uint32_t>(corners[k]), static_cast<std::uint32_t>(corners[(k + 1) % corners.size()]))].push_back(
				static_cast<std::uint32_t>(p));
			const EdgeTag& tag = polys[p].piece.kinds[k];
			if (tag.edge >= 0)
				nativeOwners[edgeKey(tag.mesh, static_cast<std::uint32_t>(tag.edge))].push_back(
					{ static_cast<std::uint32_t>(p), static_cast<std::uint32_t>(k) });
		}
	}
	const auto overlap = [&](int a, int b, int c, int d)
	{
		// Length of c-d's projection onto a-b within a-b, in quanta; none when c-d runs more than 2 u off the line.
		const QuantVertex& va = tile.verts[static_cast<std::size_t>(a)];
		const QuantVertex& vb = tile.verts[static_cast<std::size_t>(b)];
		const double ux = vb.x - va.x, uz = vb.z - va.z;
		const double length = std::hypot(ux, uz);
		if (length == 0.0)
			return 0.0;
		double t0 = 0.0, t1 = 0.0;
		for (const int v : { c, d })
		{
			const QuantVertex& q = tile.verts[static_cast<std::size_t>(v)];
			const double along = ((q.x - va.x) * ux + (q.z - va.z) * uz) / length;
			const double off = std::abs((q.z - va.z) * ux - (q.x - va.x) * uz) / length;
			if (off * kCell > 2.0)
				return 0.0;
			(v == c ? t0 : t1) = along;
		}
		return std::max(0.0, std::min(length, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1)));
	};
	const auto partnerOf = [&](std::size_t p, int a, int b, const EdgeTag& tag) -> int
	{
		if (tag.kind == EdgeKind::Wall)
			return -1;
		if (const auto it = edgeOwners.find(edgeKey(static_cast<std::uint32_t>(b), static_cast<std::uint32_t>(a))); it != edgeOwners.end())
		{
			// A wall on the other face's reversed edge does not partner; a Shared edge may still find its opposite below.
			const auto& other = polys[it->second.front()].piece;
			bool wallThere = false;
			for (std::size_t k = 0; k < other.corners.size(); ++k)
				wallThere |= other.corners[k] == b && other.corners[(k + 1) % other.corners.size()] == a && other.kinds[k].kind == EdgeKind::Wall;
			if (!wallThere)
				return static_cast<int>(it->second.front());
		}
		if (tag.kind == EdgeKind::Shared)
		{
			const native::Edge& edge = meshes[tag.mesh].edges[static_cast<std::size_t>(tag.edge)];
			if (edge.oppositeEdge >= 0)
			{
				int best = -1;
				double most = 0.0;
				if (const auto it = nativeOwners.find(edgeKey(tag.mesh, static_cast<std::uint32_t>(edge.oppositeEdge))); it != nativeOwners.end())
					for (const auto& [other, k] : it->second)
					{
						const auto& corners = polys[other].piece.corners;
						const double shared = overlap(a, b, corners[k], corners[(k + 1) % corners.size()]);
						if (other != p && shared > most)
						{
							most = shared;
							best = static_cast<int>(other);
						}
					}
				if (best < 0)
				{
					// The opposite face is not in this tile: it collapsed on the 1/8 u grid (a sliver thinner than a quantum). Look through it:
					// the faces across its other edges are this edge's neighbours, the one overlapping most along it first (breadth first, a few faces deep).
					const native::Mesh& source = meshes[tag.mesh];
					std::vector<std::int32_t> queue{ edge.oppositeFace };
					std::vector<std::int32_t> visited;
					for (std::size_t head = 0; head < queue.size() && head < 8 && best < 0; ++head)
					{
						const std::int32_t face = queue[head];
						if (face < 0 || std::find(visited.begin(), visited.end(), face) != visited.end())
							continue;
						visited.push_back(face);
						const native::Face& far = source.faces[static_cast<std::size_t>(face)];
						for (std::uint32_t e2 = far.startEdge; e2 < far.startEdge + far.numEdges; ++e2)
						{
							const native::Edge& edge2 = source.edges[e2];
							if (edge2.oppositeEdge < 0 || static_cast<std::int32_t>(e2) == edge.oppositeEdge)
								continue;
							const auto owners = nativeOwners.find(edgeKey(tag.mesh, static_cast<std::uint32_t>(edge2.oppositeEdge)));
							if (owners == nativeOwners.end())
							{
								queue.push_back(edge2.oppositeFace);
								continue;
							}
							for (const auto& [other, k2] : owners->second)
							{
								const auto& corners = polys[other].piece.corners;
								const double shared = overlap(a, b, corners[k2], corners[(k2 + 1) % corners.size()]);
								if (other != p && shared > most)
								{
									most = shared;
									best = static_cast<int>(other);
								}
							}
						}
					}
				}
				if (best >= 0)
					return best;
			}
		}
		if (tag.kind != EdgeKind::Wall)
			for (const std::uint32_t other : edgeOwners[edgeKey(static_cast<std::uint32_t>(a), static_cast<std::uint32_t>(b))])
				if (other != p)
					return static_cast<int>(other);
		if (tag.kind != EdgeKind::Wall)
		{
			// Last: whatever polygon of the tile has an edge running the other way along this one (the face across was collapsed or
			// clipped away on the grid): the one overlapping most, within the climb in height.
			const QuantVertex& va = tile.verts[static_cast<std::size_t>(a)];
			const QuantVertex& vb = tile.verts[static_cast<std::size_t>(b)];
			int best = -1;
			double most = 0.0;
			for (std::size_t q = 0; q < polys.size(); ++q)
			{
				const auto& corners = polys[q].piece.corners;
				for (std::size_t k = 0; q != p && k < corners.size(); ++k)
				{
					const QuantVertex& vc = tile.verts[static_cast<std::size_t>(corners[k])];
					const QuantVertex& vd = tile.verts[static_cast<std::size_t>(corners[(k + 1) % corners.size()])];
					if ((vb.x - va.x) * (vd.x - vc.x) + (vb.z - va.z) * (vd.z - vc.z) >= 0)
						continue;
					if (std::abs((vc.y + vd.y) - (va.y + vb.y)) * 0.5 * kCell > 5.0)
						continue;
					const double shared = overlap(a, b, corners[k], corners[(k + 1) % corners.size()]);
					if (shared > most)
					{
						most = shared;
						best = static_cast<int>(q);
					}
				}
			}
			return best;
		}
		return -1;
	};
	const int side = static_cast<int>(std::lround(c.tileSize / kCell));
	const auto portal = [&](const QuantVertex& a, const QuantVertex& b) -> int
	{
		if (a.x == 0 && b.x == 0)
			return 0;
		if (a.z == side && b.z == side)
			return 1;
		if (a.x == side && b.x == side)
			return 2;
		if (a.z == 0 && b.z == 0)
			return 3;
		return -1;
	};
	std::vector<unsigned short> verts;
	for (const QuantVertex& v : tile.verts)
		verts.insert(verts.end(), { static_cast<unsigned short>(v.x), static_cast<unsigned short>(v.y), static_cast<unsigned short>(v.z) });
	std::vector<unsigned short> polyData(polys.size() * 2 * kMaxCorners, kNoNeighbour);
	std::vector<unsigned char> areas;
	std::vector<unsigned short> flags;
	std::vector<unsigned int> detailMeshes;
	std::vector<float> detailVerts;
	std::vector<unsigned char> detailTris;
	BakedTile out{ tx, tz, {}, {} };
	for (std::size_t p = 0; p < polys.size(); ++p)
	{
		const TilePoly& poly = polys[p];
		const auto& corners = poly.piece.corners;
		const std::size_t n = corners.size();
		unsigned short* row = &polyData[p * 2 * kMaxCorners];
		for (std::size_t k = 0; k < n; ++k)
		{
			const int a = corners[k];
			const int b = corners[(k + 1) % n];
			row[k] = static_cast<unsigned short>(a);
			const EdgeKind kind = poly.piece.kinds[k].kind;
			if (const int partner = partnerOf(p, a, b, poly.piece.kinds[k]); partner >= 0)
			{
				++stats.partnered;
				row[kMaxCorners + k] = static_cast<unsigned short>(partner);
				continue;
			}
			if (kind == EdgeKind::Wall)
				continue;
			const int dir = portal(tile.verts[static_cast<std::size_t>(a)], tile.verts[static_cast<std::size_t>(b)]);
			if (dir >= 0)
				row[kMaxCorners + k] = static_cast<unsigned short>(0x8000 | dir);
			else if (kind == EdgeKind::Linked)
				++stats.openLinks;
			else if (kind == EdgeKind::Shared)
				++stats.openShared;
		}
		areas.push_back(static_cast<unsigned char>(poly.area));
		flags.push_back(areaFlag(poly.area));
		if (poly.area == Area::DoorOpen && poly.door != 0)
			out.doors.push_back({ static_cast<std::uint16_t>(p), poly.door });

		// Detail mesh: the corners (Detour sizes the polygon's tree node from them), the triangles on them.
		detailMeshes.insert(detailMeshes.end(), { static_cast<unsigned int>(detailVerts.size() / 3), static_cast<unsigned int>(n),
			static_cast<unsigned int>(detailTris.size() / 4), static_cast<unsigned int>(poly.piece.triangles.size()) });
		for (const int id : corners)
		{
			const QuantVertex& v = tile.verts[static_cast<std::size_t>(id)];
			detailVerts.insert(detailVerts.end(), { static_cast<float>(x0 + v.x * kCell), static_cast<float>(tile.floor + v.y * kCell),
				static_cast<float>(z0 + v.z * kCell) });
		}
		for (const auto& triangle : poly.piece.triangles)
		{
			std::array<std::size_t, 3> local{};
			for (int k = 0; k < 3; ++k)
				local[static_cast<std::size_t>(k)] = static_cast<std::size_t>(std::find(corners.begin(), corners.end(), triangle[static_cast<std::size_t>(k)]) - corners.begin());
			unsigned char edgeFlags = 0;
			for (int k = 0; k < 3; ++k)
			{
				const std::size_t a = local[static_cast<std::size_t>(k)];
				const std::size_t b = local[static_cast<std::size_t>((k + 1) % 3)];
				if ((a + 1) % n == b || (b + 1) % n == a)
					edgeFlags |= static_cast<unsigned char>(DT_DETAIL_EDGE_BOUNDARY << (k * 2));
			}
			detailTris.insert(detailTris.end(), { static_cast<unsigned char>(local[0]), static_cast<unsigned char>(local[1]),
				static_cast<unsigned char>(local[2]), edgeFlags });
		}
	}

	dtNavMeshCreateParams params{};
	params.verts = verts.data();
	params.vertCount = static_cast<int>(tile.verts.size());
	params.polys = polyData.data();
	params.polyAreas = areas.data();
	params.polyFlags = flags.data();
	params.polyCount = static_cast<int>(polys.size());
	params.nvp = kMaxCorners;
	params.detailMeshes = detailMeshes.data();
	params.detailVerts = detailVerts.data();
	params.detailVertsCount = static_cast<int>(detailVerts.size() / 3);
	params.detailTris = detailTris.data();
	params.detailTriCount = static_cast<int>(detailTris.size() / 4);
	params.tileX = tx;
	params.tileY = tz;
	params.bmin[0] = static_cast<float>(x0);
	params.bmin[1] = static_cast<float>(tile.floor);
	params.bmin[2] = static_cast<float>(z0);
	params.bmax[0] = static_cast<float>(x1);
	params.bmax[1] = static_cast<float>(top + c.agentHeight);
	params.bmax[2] = static_cast<float>(z1);
	params.walkableHeight = c.agentHeight;
	params.walkableRadius = c.agentRadius;
	params.walkableClimb = c.agentClimb;
	params.cs = static_cast<float>(kCell);
	params.ch = static_cast<float>(kCell);
	params.buildBvTree = true;
	unsigned char* data = nullptr;
	int size = 0;
	if (!dtCreateNavMeshData(&params, &data, &size))
		return std::nullopt;
	out.data.assign(data, data + size);
	dtFree(data);
	return out;
}

} // namespace

NativeDoorKeys nativeDoorKeys(const std::vector<kworld::AreaBuilding>& buildings, double reach, double below)
{
	struct Door
	{
		DoorKey key;
		std::vector<std::array<double, 2>> hull; // xz, counter-clockwise
		double bottom;
		double top;
	};
	std::vector<Door> doors;
	for (const kworld::AreaBuilding& building : buildings)
	{
		for (std::size_t i = 0; building.shapes && i < building.shapes->size(); ++i)
		{
			const kworld::WorldShape& shape = (*building.shapes)[i];
			if (shape.use != kworld::NavUse::Door || shape.door == 0)
				continue;
			std::vector<std::array<double, 2>> points;
			for (std::size_t v = 0; v + 2 < shape.vertices.size(); v += 3)
				points.push_back({ shape.vertices[v], shape.vertices[v + 2] });
			std::sort(points.begin(), points.end());
			points.erase(std::unique(points.begin(), points.end()), points.end());
			// Monotone chain.
			std::vector<std::array<double, 2>> hull(2 * points.size());
			std::size_t k = 0;
			auto turn = [](const auto& o, const auto& a, const auto& b)
			{
				return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
			};
			for (std::size_t p = 0; p < points.size(); ++p)
			{
				while (k >= 2 && turn(hull[k - 2], hull[k - 1], points[p]) <= 0.0)
					--k;
				hull[k++] = points[p];
			}
			for (std::size_t p = points.size() - 1, lower = k + 1; p-- > 0;)
			{
				while (k >= lower && turn(hull[k - 2], hull[k - 1], points[p]) <= 0.0)
					--k;
				hull[k++] = points[p];
			}
			hull.resize(k > 1 ? k - 1 : k);
			if (!hull.empty())
				doors.push_back({ shape.door, std::move(hull), shape.min[1], shape.max[1] });
		}
	}
	return [doors = std::move(doors), reach, below](const std::array<double, 3>& centre)
	{
		DoorKey best = 0;
		double bestDistance = reach;
		for (const Door& door : doors)
		{
			if (centre[1] < door.bottom - below || centre[1] > door.top)
				continue;
			// Distance to a convex polygon: 0 inside, else the nearest edge's.
			bool inside = door.hull.size() >= 3;
			double nearest = std::numeric_limits<double>::infinity();
			for (std::size_t i = 0; i < door.hull.size(); ++i)
			{
				const auto& a = door.hull[i];
				const auto& b = door.hull[(i + 1) % door.hull.size()];
				const double ex = b[0] - a[0];
				const double ez = b[1] - a[1];
				if (ex * (centre[2] - a[1]) - ez * (centre[0] - a[0]) < 0.0)
					inside = false;
				const double length2 = ex * ex + ez * ez;
				const double t = length2 > 0.0
					? std::clamp(((centre[0] - a[0]) * ex + (centre[2] - a[1]) * ez) / length2, 0.0, 1.0) : 0.0;
				nearest = std::min(nearest, std::hypot(centre[0] - a[0] - t * ex, centre[2] - a[1] - t * ez));
			}
			const double distance = inside ? 0.0 : nearest;
			if (distance <= bestDistance)
			{
				bestDistance = distance;
				best = door.key;
			}
		}
		return best;
	};
}

std::vector<BakedTile> nativeTiles(const NavConfig& config, const std::vector<native::Mesh>& meshes,
	const std::vector<TileCoord>& coords, const NativeDoorKeys& doorKeys, NativeTileStats* stats)
{
	std::map<TileCoord, std::vector<FaceRef>> buckets;
	for (const TileCoord& coord : coords)
		buckets[coord];
	const double size = config.tileSize;
	for (std::size_t m = 0; m < meshes.size(); ++m)
	{
		const native::Mesh& mesh = meshes[m];
		for (std::size_t f = 0; f < mesh.faces.size(); ++f)
		{
			const native::Face& face = mesh.faces[f];
			double minX = std::numeric_limits<double>::max(), maxX = std::numeric_limits<double>::lowest();
			double minZ = minX, maxZ = maxX;
			for (std::uint32_t k = 0; k < face.numEdges; ++k)
			{
				const auto& v = mesh.vertices[mesh.edges[face.startEdge + k].a];
				minX = std::min(minX, v[0]);
				maxX = std::max(maxX, v[0]);
				minZ = std::min(minZ, v[2]);
				maxZ = std::max(maxZ, v[2]);
			}
			const int tx0 = static_cast<int>(std::floor((minX - config.originX) / size));
			const int tx1 = static_cast<int>(std::floor((maxX - config.originX) / size));
			const int tz0 = static_cast<int>(std::floor((minZ - config.originZ) / size));
			const int tz1 = static_cast<int>(std::floor((maxZ - config.originZ) / size));
			for (int tz = tz0; tz <= tz1; ++tz)
				for (int tx = tx0; tx <= tx1; ++tx)
					if (const auto it = buckets.find({ tx, tz }); it != buckets.end())
						it->second.push_back({ static_cast<std::uint32_t>(m), static_cast<std::uint32_t>(f) });
		}
	}

	std::vector<std::vector<std::array<double, 3>>> vertices;
	for (const native::Mesh& mesh : meshes)
		vertices.push_back(weldedVertices(mesh));
	snapLinks(meshes, vertices);
	const LinkSplits splits = splitLinkedEdges(meshes, vertices);
	NativeTileStats local;
	std::vector<BakedTile> tiles;
	for (const TileCoord& coord : coords)
		if (auto tile = buildTile(config, meshes, vertices, splits, buckets[coord], coord.x, coord.z, doorKeys, local))
			tiles.push_back(std::move(*tile));
	if (stats)
		*stats = local;
	return tiles;
}

} // namespace knav
