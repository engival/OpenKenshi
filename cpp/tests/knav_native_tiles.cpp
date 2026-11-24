// The original's shipped navtiles converted to Detour (knav/native_tiles.hpp), over the 3 x 3 zones round the Hub:
//  - every tile converts (none past kMaxPolysPerTile), and the converted surface covers the original's faces' xz area;
//  - the area reachable from a Hub street matches the original's (its faces joined by opposite faces and streaming links):
//    welding, clipping and slivers lose no connection;
//  - a route from outside the Hub bar climbs its back stair to floor 1 (the stair our 2 u recipe bake slits,
//    NAVMESH_CACHE "Stair seam").

#include "knav/area.hpp"
#include "knav/native_tiles.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"

#include <DetourNavMesh.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <queue>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr kworld::ZoneCell kMin{ 19, 31 };
constexpr kworld::ZoneCell kMax{ 21, 33 };

struct Window
{
	double x0, z0, x1, z1;
	bool contains(double x, double z) const { return x >= x0 && x < x1 && z >= z0 && z < z1; }
};

double faceArea(const knav::native::Mesh& mesh, const knav::native::Face& face, double& cx, double& cz)
{
	double area = 0.0;
	cx = cz = 0.0;
	for (std::uint32_t k = 0; k < face.numEdges; ++k)
	{
		const auto& a = mesh.vertices[mesh.edges[face.startEdge + k].a];
		const auto& b = mesh.vertices[mesh.edges[face.startEdge + k].b];
		area += a[0] * b[2] - b[0] * a[2];
		cx += a[0];
		cz += a[2];
	}
	cx /= face.numEdges;
	cz /= face.numEdges;
	return std::abs(area) * 0.5;
}

bool insideFace(const knav::native::Mesh& mesh, const knav::native::Face& face, double x, double z)
{
	bool inside = false;
	for (std::uint32_t k = 0; k < face.numEdges; ++k)
	{
		const auto& a = mesh.vertices[mesh.edges[face.startEdge + k].a];
		const auto& b = mesh.vertices[mesh.edges[face.startEdge + k].b];
		if ((a[2] > z) != (b[2] > z) && x < a[0] + (z - a[2]) * (b[0] - a[0]) / (b[2] - a[2]))
			inside = !inside;
	}
	return inside;
}

double polyArea(const dtMeshTile& tile, const dtPoly& poly, double& cx, double& cz)
{
	double area = 0.0;
	cx = cz = 0.0;
	for (int k = 0; k < poly.vertCount; ++k)
	{
		const float* a = &tile.verts[poly.verts[k] * 3];
		const float* b = &tile.verts[poly.verts[(k + 1) % poly.vertCount] * 3];
		area += static_cast<double>(a[0]) * b[2] - static_cast<double>(b[0]) * a[2];
		cx += a[0];
		cz += a[2];
	}
	cx /= poly.vertCount;
	cz /= poly.vertCount;
	return std::abs(area) * 0.5;
}

// The original's faces reachable from the face under (x, y, z) (the nearest in height of those containing x, z): over opposite
// faces, and over streaming links that overlap another mesh's link (within 1 u: each mesh keeps its own copy of a doorway's
// corners, up to 0.3 u apart, and a zone seam may split an edge differently on its two sides). Area summed for faces centred in the window;
// `goalReached` says whether the face under `goal` (found the same way) is among them.
double nativeReachable(const std::vector<knav::native::Mesh>& meshes, const Window& window, double x, double y, double z,
	const knav::Point& goal, bool& goalReached)
{
	struct Link
	{
		std::size_t mesh, face;
		std::array<double, 3> a, b;
	};
	constexpr double kCell = 16.0;
	constexpr double kNear = 1.0;
	std::map<std::pair<long, long>, std::vector<Link>> links;
	const auto cells = [&](const std::array<double, 3>& a, const std::array<double, 3>& b, const auto& each)
	{
		for (long cz = std::lround(std::floor((std::min(a[2], b[2]) - kNear) / kCell)); cz <= std::lround(std::floor((std::max(a[2], b[2]) + kNear) / kCell)); ++cz)
			for (long cx = std::lround(std::floor((std::min(a[0], b[0]) - kNear) / kCell)); cx <= std::lround(std::floor((std::max(a[0], b[0]) + kNear) / kCell)); ++cx)
				each(std::make_pair(cx, cz));
	};
	// Distance from p to segment a-b, and where along it (0..1).
	const auto toSegment = [](const std::array<double, 3>& p, const std::array<double, 3>& a, const std::array<double, 3>& b, double& t)
	{
		double ab2 = 0.0, dot = 0.0;
		for (int i = 0; i < 3; ++i)
		{
			ab2 += (b[i] - a[i]) * (b[i] - a[i]);
			dot += (p[i] - a[i]) * (b[i] - a[i]);
		}
		t = ab2 > 0.0 ? std::clamp(dot / ab2, 0.0, 1.0) : 0.0;
		double d2 = 0.0;
		for (int i = 0; i < 3; ++i)
			d2 += (a[i] + (b[i] - a[i]) * t - p[i]) * (a[i] + (b[i] - a[i]) * t - p[i]);
		return std::sqrt(d2);
	};
	// Overlap: the endpoints of each that lie on the other span more than kNear of shared length.
	const auto overlaps = [&](const Link& p, const Link& q)
	{
		std::vector<std::array<double, 3>> on;
		double t = 0.0;
		for (const auto* v : { &p.a, &p.b })
			if (toSegment(*v, q.a, q.b, t) < kNear)
				on.push_back(*v);
		for (const auto* v : { &q.a, &q.b })
			if (toSegment(*v, p.a, p.b, t) < kNear)
				on.push_back(*v);
		for (std::size_t i = 0; i < on.size(); ++i)
			for (std::size_t j = i + 1; j < on.size(); ++j)
				if (std::hypot(on[i][0] - on[j][0], on[i][2] - on[j][2]) > kNear)
					return true;
		return false;
	};
	std::vector<std::vector<bool>> seen(meshes.size());
	std::size_t seedMesh = 0, seedFace = 0, goalMesh = 0, goalFace = 0;
	double best = 1e30, bestGoal = 1e30;
	for (std::size_t m = 0; m < meshes.size(); ++m)
	{
		seen[m].assign(meshes[m].faces.size(), false);
		for (std::size_t f = 0; f < meshes[m].faces.size(); ++f)
		{
			const knav::native::Face& face = meshes[m].faces[f];
			for (std::uint32_t k = 0; k < face.numEdges; ++k)
			{
				const knav::native::Edge& edge = meshes[m].edges[face.startEdge + k];
				if (!edge.linked)
					continue;
				const Link link{ m, f, meshes[m].vertices[edge.a], meshes[m].vertices[edge.b] };
				cells(link.a, link.b, [&](const std::pair<long, long>& cell) { links[cell].push_back(link); });
			}
			if (insideFace(meshes[m], face, goal[0], goal[2]))
			{
				const double height = std::abs(meshes[m].vertices[meshes[m].edges[face.startEdge].a][1] - goal[1]);
				if (height < bestGoal)
				{
					bestGoal = height;
					goalMesh = m;
					goalFace = f;
				}
			}
			if (insideFace(meshes[m], face, x, z))
			{
				const double height = std::abs(meshes[m].vertices[meshes[m].edges[face.startEdge].a][1] - y);
				if (height < best)
				{
					best = height;
					seedMesh = m;
					seedFace = f;
				}
			}
		}
	}
	test::require(best < 30.0, "no original face under the seed");

	double area = 0.0;
	std::queue<std::pair<std::size_t, std::size_t>> open;
	open.push({ seedMesh, seedFace });
	seen[seedMesh][seedFace] = true;
	while (!open.empty())
	{
		const auto [m, f] = open.front();
		open.pop();
		const knav::native::Mesh& mesh = meshes[m];
		const knav::native::Face& face = mesh.faces[f];
		double cx = 0.0, cz = 0.0;
		const double a = faceArea(mesh, face, cx, cz);
		if (window.contains(cx, cz))
			area += a;
		const auto visit = [&](std::size_t nm, std::size_t nf)
		{
			if (!seen[nm][nf])
			{
				seen[nm][nf] = true;
				open.push({ nm, nf });
			}
		};
		for (std::uint32_t k = 0; k < face.numEdges; ++k)
		{
			const knav::native::Edge& edge = mesh.edges[face.startEdge + k];
			if (edge.oppositeFace >= 0 && static_cast<std::size_t>(edge.oppositeFace) < mesh.faces.size())
				visit(m, static_cast<std::size_t>(edge.oppositeFace));
			if (!edge.linked)
				continue;
			const Link link{ m, f, mesh.vertices[edge.a], mesh.vertices[edge.b] };
			cells(link.a, link.b, [&](const std::pair<long, long>& cell)
			{
				if (const auto it = links.find(cell); it != links.end())
					for (const Link& other : it->second)
						if (other.mesh != m && overlaps(link, other))
							visit(other.mesh, other.face);
			});
		}
	}
	goalReached = bestGoal < 30.0 && seen[goalMesh][goalFace];
	return area;
}

// Two squares side by side in one tile, meeting on x = 16: partner edges counted for a shared edge (opposite faces) and for a
// pair of wall edges on the same vertices (a sheet wall seen from both sides).
std::size_t partnerEdges(bool wall)
{
	const double x0 = kworld::kWorldMin + 20 * kworld::kZoneSize + 64.0, z0 = kworld::kWorldMin + 32 * kworld::kZoneSize + 64.0;
	knav::native::Mesh mesh;
	mesh.vertices = { { x0, 100.0, z0 }, { x0 + 16, 100.0, z0 }, { x0 + 16, 100.0, z0 + 16 }, { x0, 100.0, z0 + 16 },
		{ x0 + 32, 100.0, z0 }, { x0 + 32, 100.0, z0 + 16 } };
	const auto edge = [&](std::uint32_t a, std::uint32_t b, int oppositeFace, int oppositeEdge)
	{
		knav::native::Edge e;
		e.a = a;
		e.b = b;
		e.oppositeFace = oppositeFace;
		e.oppositeEdge = oppositeEdge;
		mesh.edges.push_back(e);
	};
	// Face 0 = vertices 0 1 2 3 (edge 1 is its x = 16 side), face 1 = vertices 1 4 5 2 (edge 7 is its x = 16 side).
	edge(0, 1, -1, -1); edge(1, 2, wall ? -1 : 1, wall ? -1 : 7); edge(2, 3, -1, -1); edge(3, 0, -1, -1);
	edge(1, 4, -1, -1); edge(4, 5, -1, -1); edge(5, 2, -1, -1); edge(2, 1, wall ? -1 : 0, wall ? -1 : 1);
	mesh.faces = { { 0, 4, 0 }, { 4, 4, 0 } };
	const knav::NavConfig config = knav::configForWorldZones({ 20, 32 }, { 20, 32 });
	std::vector<knav::TileCoord> coords;
	for (int z = config.window.z0; z <= config.window.z1; ++z)
		for (int x = config.window.x0; x <= config.window.x1; ++x)
			coords.push_back({ x, z });
	knav::NativeTileStats stats;
	knav::nativeTiles(config, { mesh }, coords, {}, &stats);
	return stats.partnered;
}

} // namespace

int main()
{
	try
	{
		test::require(partnerEdges(false) >= 2, "two faces sharing an edge are not linked");
		test::require(partnerEdges(true) == 0, "a wall edge on shared vertices linked its two faces");
		const std::string dir = knav::native::navtilesDir(KENSHI_DATA);
		std::vector<knav::native::Mesh> meshes;
		for (int j = kMin.z - 1; j <= kMax.z + 1; ++j)
			for (int i = kMin.x - 1; i <= kMax.x + 1; ++i)
				for (knav::native::Mesh& mesh : knav::native::loadTile(dir, i, j))
					meshes.push_back(std::move(mesh));
		// Converted over every zone loaded, so both meshes may leave the window and come back; measured inside it.
		const knav::NavConfig config = knav::configForWorldZones({ kMin.x - 1, kMin.z - 1 }, { kMax.x + 1, kMax.z + 1 });
		std::vector<knav::TileCoord> coords;
		for (int z = config.window.z0; z <= config.window.z1; ++z)
			for (int x = config.window.x0; x <= config.window.x1; ++x)
				coords.push_back({ x, z });
		// Door faces get their door from the placed buildings' door shapes.
		const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
		const knav::AreaGeometry area = knav::gatherArea(KENSHI_DATA, terrain, { kMin.x - 1, kMin.z - 1 }, { kMax.x + 1, kMax.z + 1 });
		knav::NativeTileStats stats;
		const knav::NavMesh mesh = knav::NavMesh::fromTiles(config,
			knav::nativeTiles(config, meshes, coords, knav::nativeDoorKeys(area.buildings), &stats));
		std::cout << mesh.tiles().size() << " tiles, " << stats.faces << " faces, " << stats.polys << " polys (fullest " << stats.maxPolys
				  << "), slivers " << stats.slivers << ", fanned " << stats.fanned << ", open links " << stats.openLinks << ", open shared "
				  << stats.openShared << ", partnered " << stats.partnered << ", door polys " << stats.doorPolys << " (" << stats.unkeyedDoors << " with no door), doors "
				  << mesh.doorKeys().size() << "\n";
		test::require(stats.overfull == 0, "a tile is past kMaxPolysPerTile");
		test::require(stats.doorPolys > 0 && stats.unkeyedDoors == 0, "a door face found no door");

		const Window window{ kworld::kWorldMin + kMin.x * kworld::kZoneSize, kworld::kWorldMin + kMin.z * kworld::kZoneSize,
			kworld::kWorldMin + (kMax.x + 1) * kworld::kZoneSize, kworld::kWorldMin + (kMax.z + 1) * kworld::kZoneSize };

		// Surface: the original's faces centred in the window against our polygons.
		double nativeArea = 0.0;
		for (const knav::native::Mesh& m : meshes)
			for (const knav::native::Face& face : m.faces)
			{
				double cx = 0.0, cz = 0.0;
				const double a = faceArea(m, face, cx, cz);
				if (window.contains(cx, cz))
					nativeArea += a;
			}
		std::uint16_t allAreas = 0;
		for (int area = 0; area < knav::kAreaCount; ++area)
			allAreas |= knav::areaFlag(static_cast<knav::Area>(area));
		const knav::PolyComponents components(mesh, allAreas);

		// Seed: a street west of the bar.
		const knav::NavQuery query(mesh, 65535);
		const knav::Point street{ -51320.0f, 1580.0f, 2700.0f };
		knav::PolyLoc seed;
		test::require(query.locate(street, { 8.0f, 40.0f, 8.0f }, seed), "no converted polygon at the street seed");
		const std::uint32_t seedLabel = components.at(seed.tileX, seed.tileZ, seed.poly);
		double ourArea = 0.0, ourReachable = 0.0;
		std::map<std::uint32_t, std::tuple<double, double, double>> byLabel;
		for (const knav::BakedTile& baked : mesh.tiles())
		{
			const dtMeshTile* tile = mesh.detour().getTileAt(baked.x, baked.z, 0);
			for (int p = 0; p < tile->header->polyCount; ++p)
			{
				double cx = 0.0, cz = 0.0;
				const double a = polyArea(*tile, tile->polys[p], cx, cz);
				if (!window.contains(cx, cz))
					continue;
				ourArea += a;
				if (components.at(baked.x, baked.z, p) == seedLabel)
					ourReachable += a;
				else if (std::getenv("KNAV_NATIVE_COMPONENTS"))
				{
					auto& [sum, x, z] = byLabel[components.at(baked.x, baked.z, p)];
					sum += a;
					x = cx;
					z = cz;
				}
			}
		}
		std::vector<std::tuple<double, double, double>> big;
		for (const auto& [label, entry] : byLabel)
			big.push_back(entry);
		std::sort(big.rbegin(), big.rend());
		for (std::size_t k = 0; k < big.size() && k < 12; ++k)
			std::cout << "cut off: " << std::get<0>(big[k]) << " u2 at " << std::get<1>(big[k]) << " " << std::get<2>(big[k]) << "\n";
		const knav::Point barFloor{ -51169.0f, 1647.0f, 2665.0f };
		bool nativeUp = false;
		const double nativeReach = nativeReachable(meshes, window, seed.point[0], seed.point[1], seed.point[2], barFloor, nativeUp);
		std::cout << "original: bar floor 1 " << (nativeUp ? "reachable" : "NOT reachable") << " from the street\n";
		std::cout << "area: original " << nativeArea << ", ours " << ourArea << " (" << 100.0 * ourArea / nativeArea << "%)\n"
				  << "reachable from the street: original " << nativeReach << ", ours " << ourReachable << " ("
				  << 100.0 * ourReachable / nativeReach << "%)\n";
		test::require(std::abs(ourArea / nativeArea - 1.0) < 0.005, "converted area is more than 0.5% off the original's");
		test::require(ourReachable >= 0.995 * nativeReach, "converted mesh reaches less than 99.5% of what the original reaches");

		// The bar's back stair: from the street to the middle of floor 1 (slab 20, y 1640..1647).
		const knav::PathResult up = query.findPath(seed.point, { -51169.0f, 1647.0f, 2665.0f }, { 8.0f, 6.0f, 8.0f });
		std::cout << "street -> bar floor 1: " <<  (up.complete ? "complete" : "INCOMPLETE") << " (failure " << static_cast<int>(up.failure) << "), " << up.length << " u, ends at y "
				  << (up.points.empty() ? 0.0f : up.points.back()[1]);
		for (const knav::Point& p : up.points)
			std::cout << " (" << p[0] << " " << p[1] << " " << p[2] << ")";
		std::cout << "\n";
		test::require(up.complete && up.points.back()[1] > 1640.0f, "no route up the bar's back stair");

		// Query-time clearance (NAVMESH_CACHE "Clearance"): a human (radius 4) still gets up, and both ends of the
		// route keep 4 u from the walls; a grown King Gorillo (radius 30) does not fit through the bar.
		auto clearRoute = [&](float radius)
		{
			knav::TravelFilter travel;
			travel.radius = radius;
			const auto began = std::chrono::steady_clock::now();
			const knav::PathResult route = query.findPath(seed.point, barFloor, { 8.0f, 6.0f, 8.0f }, travel);
			std::cout << "radius " << radius << ": " << (route.complete ? "complete" : "INCOMPLETE") << " (failure "
					  << static_cast<int>(route.failure) << "), " << route.length << " u, "
					  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count() << " ms";
			for (const knav::Point& p : route.points)
			{
				knav::PolyLoc at;
				std::cout << " (" << p[0] << " " << p[1] << " " << p[2] << " wall "
						  << (query.locate(p, { 1.0f, 2.0f, 1.0f }, at) ? query.wallDistance(at, 50.0f) : -1.0f) << ")";
			}
			std::cout << "\n";
			return route;
		};
		const knav::PathResult human = clearRoute(4.0f);
		test::require(human.complete && human.points.back()[1] > 1640.0f, "no route up the bar's back stair at radius 4");
		for (const knav::Point* end : { &human.points.front(), &human.points.back() })
		{
			knav::PolyLoc at;
			test::require(query.locate(*end, { 1.0f, 2.0f, 1.0f }, at) && query.wallDistance(at, 10.0f) >= 4.0f,
				"a radius 4 route ends closer than 4 u to a wall");
		}
		test::require(!clearRoute(30.0f).complete, "a radius 30 traveller got into the bar");
		std::cout << "knav_native_tiles: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_native_tiles: FAIL: " << e.what() << "\n";
		return 1;
	}
}
