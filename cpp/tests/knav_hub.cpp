// Hub navmesh: loads the baked cache (argv[1]) and checks paths against the
// building and foliage collision it was baked from.

#include "knav/area.hpp"
#include "knav/navmesh.hpp"
#include "kworld/heightmap.hpp"
#include "test_util.hpp"
#include "hub_point.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using knav::Point;
using kworld::WorldShape;

// The Hub (TOWN placement) and the two "Defensive Gate IV" placements near
// it (zone 20,32: instances 4400- and 4617-Newwworld-INGAME).
constexpr double kGates[][2] = { { -51562.0, 2780.0 }, { -50757.0, 2615.0 } };
constexpr float kOutsideGate = 120.0f; // how far past the gate the walk starts
// A path may stand on a solid's top face: the baked surface is a few voxels
// off the exact one (2-unit cells, the 3-cell band kept below the top).
constexpr float kSurfaceSlack = 5.0f;
const Point kReach ={ 40.0f, 60.0f, 40.0f };

Point groundPoint(const kworld::Heightmap& terrain, double x, double z)
{
	return { static_cast<float>(x), static_cast<float>(terrain.height(x, z)), static_cast<float>(z) };
}

// Strictly inside a closed convex solid: on the same side of every face
// (winding-agnostic), by more than `margin` units.
bool insideSolid(const WorldShape& shape, const Point& p, float margin)
{
	if (p[0] < shape.min[0] || p[0] > shape.max[0] || p[1] < shape.min[1] || p[1] > shape.max[1]
		|| p[2] < shape.min[2] || p[2] > shape.max[2])
		return false;
	bool allBelow = true;
	bool allAbove = true;
	for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
	{
		const float* a = &shape.vertices[3ull * shape.indices[t]];
		const float* b = &shape.vertices[3ull * shape.indices[t + 1]];
		const float* c = &shape.vertices[3ull * shape.indices[t + 2]];
		const float e0[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		const float e1[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		float n[3] = { e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2],
			e0[0] * e1[1] - e0[1] * e1[0] };
		const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length < 1e-6f)
			continue;
		const float d = ((p[0] - a[0]) * n[0] + (p[1] - a[1]) * n[1] + (p[2] - a[2]) * n[2]) / length;
		allBelow = allBelow && d < -margin;
		allAbove = allAbove && d > margin;
		if (!allBelow && !allAbove)
			return false;
	}
	return allBelow || allAbove;
}


// Within kSurfaceSlack of a shape: inside a closed one, in the padded box of a surface.
bool nearSurface(const WorldShape& shape, const Point& p)
{
	if (shape.closed)
		return insideSolid(shape, p, -kSurfaceSlack);
	for (int axis = 0; axis < 3; ++axis)
		if (p[axis] < shape.min[axis] - kSurfaceSlack || p[axis] > shape.max[axis] + kSurfaceSlack)
			return false;
	return true;
}

// Inside a solid, and not standing on another shape's surface: a gate's hull box overlaps its open ramp and deck shapes, which the
// bake walks (the surface generator's zone side of a building without a mask), as test (c) allows for the same overlap.
bool insideAnySolid(const std::vector<WorldShape>& shapes, const std::vector<const WorldShape*>& solids, const Point& p)
{
	for (const WorldShape* s : solids)
		if (insideSolid(*s, p, kSurfaceSlack)
			&& !std::any_of(shapes.begin(), shapes.end(), [&](const WorldShape& other) { return &other != s && nearSurface(other, p); }))
			return true;
	return false;
}

// Samples every segment of the path every 2 units, each sample at the height of the mesh under it: a straight line between two
// corners at different heights dips under a floor step (a ruin's doorway sill, 7d6ac14) where the walker stands on the floor.
bool pathAvoids(const std::vector<WorldShape>& shapes, const std::vector<const WorldShape*>& solids, const knav::PathResult& path, const knav::NavQuery& query)
{
	for (std::size_t i = 1; i < path.points.size(); ++i)
	{
		const Point& a = path.points[i - 1];
		const Point& b = path.points[i];
		const float length = std::max(1.0f, std::hypot(b[0] - a[0], b[2] - a[2]));
		for (float s = 0.0f; s <= length; s += 2.0f)
		{
			const float u = s / length;
			Point p = { a[0] + (b[0] - a[0]) * u, a[1] + (b[1] - a[1]) * u, a[2] + (b[2] - a[2]) * u };
			Point surface;
			if (query.nearestPoint(p, { 0.5f, 8.0f, 0.5f }, surface))
				p[1] = surface[1];
			if (insideAnySolid(shapes, solids, p))
			{
				std::cout << "  inside a solid at " << p[0] << " " << p[1] << " " << p[2] << " (segment " << i << ")\n";
				return false;
			}
		}
	}
	return true;
}

Point vertexCentroid(const WorldShape& shape)
{
	Point sum = { 0, 0, 0 };
	const std::size_t count = shape.vertices.size() / 3;
	for (std::size_t i = 0; i < count; ++i)
		for (int axis = 0; axis < 3; ++axis)
			sum[axis] += shape.vertices[3 * i + axis];
	for (float& v : sum)
		v /= static_cast<float>(count);
	return sum;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: knav_hub CACHE");
		const auto mesh = knav::NavMesh::load(argv[1]);
		const std::string dataDir = KENSHI_DATA;
		const auto terrain = kworld::Heightmap::open(dataDir + "/newland/land/fullmap.tif");
		const auto start = std::chrono::steady_clock::now();
		const knav::AreaGeometry area = knav::gatherArea(dataDir, terrain, { 19, 31 }, { 21, 33 });
		std::cout << "gather " << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
			<< " s: " << area.buildings.size() << " buildings\n";
		const std::vector<WorldShape> shapes = area.flatShapes();

		const auto& c = area.coverage;
		std::cout << "tiles " << mesh.tiles().size() << " of " << mesh.config().tilesX * mesh.config().tilesZ
			<< ", polys " << mesh.polyCount() << "\n"
			<< "placements " << c.placements << ": " << c.placementsWithCollision << " with collision, "
			<< c.placementsRenderOnly << " render only, " << c.placementsNoGeometry << " no geometry, "
			<< c.placementsMissingTarget << " missing target; parts " << c.parts << ": "
			<< c.partsWithCollision << " collision, " << c.partsRenderOnly << " render only\n"
			<< "foliage " << c.foliageInstances << ", with collision " << c.foliageWithCollision << "\n";
		test::require(mesh.tiles().size() > 1000 && mesh.polyCount() > 1000, "navmesh is nearly empty");
		test::require(c.placementsWithCollision > 50, "too few placements with collision");

		std::vector<const WorldShape*> solids;
		for (const WorldShape& shape : shapes)
			if (shape.closed)
				solids.push_back(&shape);

		const knav::NavQuery query(mesh);

		// (a) Outside a gate to the town centre, and (b) not through a building.
		const Point centre = groundPoint(terrain, kHubX, kHubZ);
		test::require(query.onMesh(centre, kReach), "town centre is not on the navmesh");
		int walked = 0;
		for (const auto& gate : kGates)
		{
			const double dx = gate[0] - kHubX;
			const double dz = gate[1] - kHubZ;
			const double length = std::hypot(dx, dz);
			const Point outside = groundPoint(terrain, gate[0] + dx / length * kOutsideGate,
				gate[1] + dz / length * kOutsideGate);
			const knav::PathResult path = query.findPath(outside, centre, kReach);
			const float straight = std::hypot(centre[0] - outside[0], centre[2] - outside[2]);
			std::cout << "gate (" << gate[0] << ", " << gate[1] << "): found " << path.found << " complete "
				<< path.complete << " points " << path.points.size() << " length " << path.length
				<< " straight " << straight << "\n";
			// Both gates, at the original's 5-unit step: the ground under a gate deck is solid
			// (volume fill), so the walk goes over the deck by its ramp and stairs.
			test::require(path.complete, "no complete path from outside a Hub gate to the town centre");
			++walked;
			test::require(pathAvoids(shapes, solids, path, query), "a path from the gate runs through a building");
		}
		test::require(walked == 2, "no path from outside a Hub gate to the town centre");

		// A deck is standable and reached by its stairs: gate A's inner staircase (kinspect: terrain
		// (-50796, 2814) y 1531 up to the deck (-50748, 2763) y 1555) and gate B's outer one.
		const Point stairs[][2] = { { { -50796.0f, 1531.0f, 2814.0f }, { -50748.0f, 1555.0f, 2763.0f } },
			{ { -51765.0f, 1548.0f, 2772.0f }, { -51680.0f, 1595.0f, 2718.0f } } };
		for (const auto& pair : stairs)
			test::require(query.findPath(pair[0], pair[1], kReach).complete, "a gate staircase is not walkable");

		// (b) A building between two points: the path has to go around it.
		int detours = 0;
		for (const WorldShape* solid : solids)
		{
			const float width = solid->max[0] - solid->min[0];
			const float depth = solid->max[2] - solid->min[2];
			const float height = solid->max[1] - solid->min[1];
			if (width < 30 || depth < 30 || width > 200 || depth > 200 || height < 15)
				continue;
			const Point middle = vertexCentroid(*solid);
			const Point from = groundPoint(terrain, solid->min[0] - 40, middle[2]);
			const Point to = groundPoint(terrain, solid->max[0] + 40, middle[2]);
			const knav::PathResult path = query.findPath(from, to, { 8.0f, 60.0f, 8.0f });
			if (!path.complete)
				continue;
			test::require(pathAvoids(shapes, solids, path, query), "a path runs through a building");
			if (path.length > std::hypot(to[0] - from[0], to[2] - from[2]) * 1.02f)
				++detours;
			if (detours >= 5)
				break;
		}
		std::cout << "paths that detour around a building: " << detours << "\n";
		test::require(detours > 0, "no path detoured around a building");

		// (c) A point inside a solid building is not on the mesh.
		int tested = 0;
		int offMesh = 0;
		int onOtherSolid = 0;
		for (const WorldShape* solid : solids)
		{
			const float width = solid->max[0] - solid->min[0];
			const float depth = solid->max[2] - solid->min[2];
			if (width < 24 || depth < 24 || solid->max[1] - solid->min[1] < 16)
				continue;
			const Point middle = vertexCentroid(*solid);
			if (!insideSolid(*solid, middle, 2.0f))
				continue;
			++tested;
			Point snapped;
			if (!query.nearestPoint(middle, { 2.0f, 3.0f, 2.0f }, snapped))
				++offMesh;
			else if (std::any_of(shapes.begin(), shapes.end(), [&](const WorldShape& other)
				{ return &other != solid && nearSurface(other, snapped); }))
				++onOtherSolid; // overlapping shapes: standing on another one's top face
			if (tested == 100)
				break;
		}
		std::cout << "solid interiors tested " << tested << ", off the mesh " << offMesh << ", on another shape " << onOtherSolid << "\n";
		test::require(tested >= 20, "too few solid buildings to test");
		test::require(offMesh + onOtherSolid == tested, "a point inside a solid building is on the navmesh");
	} catch (const std::exception& e) {
		std::cerr << "knav_hub: " << e.what() << "\n";
		return 1;
	}
	std::cout << "knav_hub ok\n";
	return 0;
}
