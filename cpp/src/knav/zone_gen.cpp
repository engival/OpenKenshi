#include "knav/zone_gen.hpp"

#include "knav/ground.hpp"
#include "knav/native_tiles.hpp"
#include "knav/surfgen.hpp"
#include "kworld/coords.hpp"

#include <clipper2/clipper.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <set>

namespace knav
{

std::vector<GeneratedBuilding> generateBuildings(const kworld::AreaGeometry& area, ZoneGenReport* report, int threads, const NavConfig* config)
{
	// Each building is generated on its own (a pure function of its shapes); the results keep the area's order whatever the thread count.
	std::vector<const kworld::AreaBuilding*> todo;
	for (const kworld::AreaBuilding& building : area.buildings)
	{
		if (!building.mask || !building.shapes || building.shapes->empty())
			continue;
		if (config != nullptr && !config->window.empty())
		{
			// a building whose box (plus the assembly's 16 u margin) misses the zone's tile window touches none of its tiles: its own zone generates it
			double lo[2] = { 1e30, 1e30 }, hi[2] = { -1e30, -1e30 };
			for (const kworld::WorldShape& shape : *building.shapes)
			{
				lo[0] = std::min<double>(lo[0], shape.min[0]);
				hi[0] = std::max<double>(hi[0], shape.max[0]);
				lo[1] = std::min<double>(lo[1], shape.min[2]);
				hi[1] = std::max<double>(hi[1], shape.max[2]);
			}
			const double wx0 = config->originX + config->window.x0 * config->tileSize - 16.0, wx1 = config->originX + (config->window.x1 + 1) * config->tileSize + 16.0;
			const double wz0 = config->originZ + config->window.z0 * config->tileSize - 16.0, wz1 = config->originZ + (config->window.z1 + 1) * config->tileSize + 16.0;
			if (hi[0] < wx0 || lo[0] > wx1 || hi[1] < wz0 || lo[1] > wz1)
				continue;
		}
		todo.push_back(&building);
	}
	const bool logBuildings = std::getenv("KSIM_NAV_GEN_LOG") != nullptr; // one stderr line per building as it starts: finds the slow one
	std::vector<GeneratedBuilding> made(todo.size());
	std::vector<char> ok(todo.size(), 0);
	std::vector<std::string> why(todo.size());
	const auto work = [&](std::size_t i)
	{
		const kworld::AreaBuilding& building = *todo[i];
		try
		{
			if (logBuildings)
				std::fprintf(stderr, "knav: surface generator building %s: %zu shapes\n", building.id.c_str(), building.shapes->size());
			surfgen::Input in;
			// in the building's own frame (surfgen.hpp Input::pose); a caller that set no local copies: world axes
			const bool local = building.localShapes && building.localMask;
			if (local)
				in.pose = surfgen::Input::poseOf(building.placed);
			for (const kworld::WorldShape& shape : local ? *building.localShapes : *building.shapes)
				in.shapes.push_back(&shape);
			for (const kworld::WorldShape& shape : local ? *building.localMask : *building.mask)
				in.mask.push_back(&shape);
			GeneratedBuilding& one = made[i];
			one.building = &building;
			one.interior = surfgen::generate(in);
			in.side = surfgen::Side::Zone;
			one.zoneSide = surfgen::generate(in);
			// Only a building with no walkable face on either side fails. An outdoor-only one (the floor all outside the mask hull: ramparts, platforms)
			// keeps its zone side; a door the generator painted no face for (its volume misses the floor) still leaves the building's floor: the Recast
			// pass that kept such a gate is gone, and dropping the building left it out of bounds (kenshi-k16.7.216).
			if (one.interior.faces.empty() && one.zoneSide.faces.empty())
				throw std::runtime_error("no walkable faces on either side (" + std::to_string(building.shapes->size()) + " shapes, mask " + std::to_string(building.mask->size()) + ")");
			one.interior.info = building.id;
			one.min[0] = one.min[1] = 1e30;
			one.max[0] = one.max[1] = -1e30;
			for (const kworld::WorldShape& shape : *building.shapes)
			{
				one.min[0] = std::min<double>(one.min[0], shape.min[0]);
				one.max[0] = std::max<double>(one.max[0], shape.max[0]);
				one.min[1] = std::min<double>(one.min[1], shape.min[2]);
				one.max[1] = std::max<double>(one.max[1], shape.max[2]);
			}
			ok[i] = 1;
		} catch (const std::exception& e) {
			why[i] = e.what();
		}
	};
	std::atomic<std::size_t> next{ 0 };
	const auto drain = [&]
	{
		for (std::size_t i = next++; i < todo.size(); i = next++)
			work(i);
	};
	std::vector<std::thread> pool;
	for (std::size_t t = 1; t < std::min<std::size_t>(static_cast<std::size_t>(std::max(threads, 1)), todo.size()); ++t)
		pool.emplace_back(drain);
	drain();
	for (std::thread& thread : pool)
		thread.join();
	std::vector<GeneratedBuilding> out;
	for (std::size_t i = 0; i < todo.size(); ++i)
	{
		if (ok[i])
			out.push_back(std::move(made[i]));
		else if (report != nullptr)
		{
			++report->buildingsFailed;
			report->failedIds.push_back(todo[i]->id);
			report->failedWhy.push_back(why[i]);
		}
	}
	if (report != nullptr)
		report->buildings = out.size();
	return out;
}

namespace
{

double msSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// A zone-side door face (data 4 / 5): its ring, height band and winding sign.
struct DoorRing
{
	std::int32_t data = 4;
	std::uint64_t key = 0;
	std::vector<std::array<double, 3>> ring;
	double lo = 1e30, hi = -1e30, sign = 1.0, mx = 0, mz = 0;
};

DoorRing doorRing(const native::Mesh& side, const native::Face& door)
{
	DoorRing out;
	out.data = door.data;
	out.key = door.door;
	for (std::uint32_t k = 0; k < door.numEdges; ++k)
		out.ring.push_back(side.vertices[side.edges[door.startEdge + k].a]);
	double area2 = 0;
	for (std::size_t k = 0; k < out.ring.size(); ++k)
	{
		const auto& a = out.ring[k];
		const auto& b = out.ring[(k + 1) % out.ring.size()];
		area2 += a[0] * b[2] - b[0] * a[2];
		out.lo = std::min(out.lo, a[1]);
		out.hi = std::max(out.hi, a[1]);
		out.mx += a[0] / out.ring.size();
		out.mz += a[2] / out.ring.size();
	}
	out.sign = area2 < 0 ? -1.0 : 1.0;
	return out;
}

std::array<double, 3> faceCentre(const native::Mesh& mesh, const native::Face& face)
{
	std::array<double, 3> c{};
	for (std::uint32_t k = 0; k < face.numEdges; ++k)
		for (int i = 0; i < 3; ++i)
			c[i] += mesh.vertices[mesh.edges[face.startEdge + k].a][i] / face.numEdges;
	return c;
}

bool centreInside(const DoorRing& d, double cx, double cz)
{
	for (std::size_t k = 0; k < d.ring.size(); ++k)
	{
		const auto& a = d.ring[k];
		const auto& b = d.ring[(k + 1) % d.ring.size()];
		if (d.sign * ((b[0] - a[0]) * (cz - a[2]) - (b[2] - a[2]) * (cx - a[0])) < -1e-3)
			return false;
	}
	return true;
}

// The walkable face nearest the door's middle within 3 u, in its height band (nullptr: none).
native::Face* nearestFace(native::Mesh& world, const DoorRing& d)
{
	native::Face* best = nullptr;
	double bestD = 3.0 * 3.0;
	for (native::Face& face : world.faces)
	{
		if (face.data == 4 || face.data == 5)
			continue;
		const auto c = faceCentre(world, face);
		const double dist = (c[0] - d.mx) * (c[0] - d.mx) + (c[2] - d.mz) * (c[2] - d.mz);
		if (c[1] < d.lo - 2.0 || c[1] > d.hi + 2.0 || dist >= bestD)
			continue;
		bestD = dist;
		best = &face;
	}
	return best;
}

// Cuts the door's own strip out of every floor face its ring crosses (in the door's height band) and keys the strip; the rest of the face stays as
// hole-free pieces. A face the cut would leave with a hole is left alone. Runs BEFORE weldMesh, so the weld shares the new vertices, splits the
// T-junctions and links the pieces; pieces keep the source face's winding, and an edge lying on one of its edges keeps that edge's flags and linked
// bit. Heights come from the face's plane. Returns strips keyed (kenshi-k16.7.218).
std::size_t cutDoorStrip(native::Mesh& world, const DoorRing& d)
{
	using namespace Clipper2Lib;
	constexpr double S = 1024.0;
	const auto toPath = [&](const std::vector<std::array<double, 3>>& pts)
	{
		Path64 path;
		for (const auto& v : pts)
			path.push_back(Point64(std::llround(v[0] * S), std::llround(v[2] * S)));
		if (Clipper2Lib::Area(path) < 0)
			std::reverse(path.begin(), path.end());
		return path;
	};
	// a door ring can be a sliver (0.2 u wide, ~0.005 u2): widen it by 0.3 u so the cut has an area
	const Paths64 clip = InflatePaths({ toPath(d.ring) }, 0.3 * S, JoinType::Miter, EndType::Polygon);
	const Rect64 clipBox = GetBounds(clip);
	std::size_t keyed = 0;
	const std::size_t count = world.faces.size();
	for (std::size_t f = 0; f < count; ++f)
	{
		const native::Face face = world.faces[f];
		if (face.data == 4 || face.data == 5 || face.numEdges < 3)
			continue;
		std::vector<std::array<double, 3>> pts;
		double ylo = 1e30, yhi = -1e30, twice = 0;
		for (std::uint32_t k = 0; k < face.numEdges; ++k)
		{
			pts.push_back(world.vertices[world.edges[face.startEdge + k].a]);
			ylo = std::min(ylo, pts.back()[1]);
			yhi = std::max(yhi, pts.back()[1]);
		}
		if (yhi < d.lo - 2.0 || ylo > d.hi + 2.0)
			continue;
		const Paths64 subject{ toPath(pts) };
		const Rect64 faceBox = GetBounds(subject);
		if (faceBox.right < clipBox.left || faceBox.left > clipBox.right || faceBox.bottom < clipBox.top || faceBox.top > clipBox.bottom)
			continue; // xz boxes apart: no overlap, skip the clipper call
		for (std::size_t k = 0; k < pts.size(); ++k)
			twice += pts[k][0] * pts[(k + 1) % pts.size()][2] - pts[(k + 1) % pts.size()][0] * pts[k][2];
		const Paths64 strip = Intersect(subject, clip, FillRule::NonZero);
		if (strip.empty())
			continue;
		const Paths64 rest = Difference(subject, clip, FillRule::NonZero);
		bool hole = false;
		for (const Path64& piece : rest)
			hole = hole || Clipper2Lib::Area(piece) < 0;
		if (hole)
			continue;
		// the face's plane: y = y0 + gx * (x - x0) + gz * (z - z0) from the corner triple with the largest area
		const auto& p0 = pts[0];
		double gx = 0, gz = 0, best = 0;
		for (std::size_t i = 1; i + 1 < pts.size(); ++i)
		{
			const double ax = pts[i][0] - p0[0], az = pts[i][2] - p0[2], bx = pts[i + 1][0] - p0[0], bz = pts[i + 1][2] - p0[2];
			const double det = ax * bz - az * bx;
			if (std::fabs(det) > best)
			{
				best = std::fabs(det);
				gx = ((pts[i][1] - p0[1]) * bz - (pts[i + 1][1] - p0[1]) * az) / det;
				gz = ((pts[i + 1][1] - p0[1]) * ax - (pts[i][1] - p0[1]) * bx) / det;
			}
		}
		const std::vector<native::Edge> source(world.edges.begin() + face.startEdge, world.edges.begin() + face.startEdge + face.numEdges);
		const std::vector<std::array<double, 3>> sourceCorners = pts;
		// the source edge a piece edge lies on (both ends within 2 / S of the segment), if any
		const auto onSource = [&](const std::array<double, 3>& p, const std::array<double, 3>& q) -> int
		{
			const auto near = [&](const std::array<double, 3>& r, const std::array<double, 3>& a, const std::array<double, 3>& b)
			{
				const double ex = b[0] - a[0], ez = b[2] - a[2], len2 = ex * ex + ez * ez;
				const double t = len2 > 0 ? std::clamp(((r[0] - a[0]) * ex + (r[2] - a[2]) * ez) / len2, 0.0, 1.0) : 0.0;
				return std::hypot(r[0] - (a[0] + t * ex), r[2] - (a[2] + t * ez)) < 3.0 / S;
			};
			for (std::size_t k = 0; k < sourceCorners.size(); ++k)
			{
				const auto& a = sourceCorners[k];
				const auto& b = sourceCorners[(k + 1) % sourceCorners.size()];
				if (near(p, a, b) && near(q, a, b))
					return static_cast<int>(k);
			}
			return -1;
		};
		std::vector<char> relinked(source.size(), 0);
		bool first = true;
		const auto emit = [&](Path64 path, std::int32_t data, std::uint64_t key)
		{
			if (twice < 0)
				std::reverse(path.begin(), path.end()); // the face's own winding (clipper's is positive)
			native::Face made = face;
			made.startEdge = static_cast<std::uint32_t>(world.edges.size());
			made.numEdges = static_cast<std::uint16_t>(path.size());
			made.data = data;
			made.door = key;
			const std::uint32_t base = static_cast<std::uint32_t>(world.vertices.size());
			std::vector<std::array<double, 3>> corners;
			for (const Point64& q : path)
			{
				const double x = q.x / S, z = q.y / S;
				corners.push_back({ x, p0[1] + gx * (x - p0[0]) + gz * (z - p0[2]), z });
			}
			world.vertices.insert(world.vertices.end(), corners.begin(), corners.end());
			for (std::size_t k = 0; k < path.size(); ++k)
			{
				native::Edge edge;
				edge.a = base + static_cast<std::uint32_t>(k);
				edge.b = base + static_cast<std::uint32_t>((k + 1) % path.size());
				if (const int sk = onSource(corners[k], corners[(k + 1) % path.size()]); sk >= 0)
				{
					const auto at = static_cast<std::size_t>(sk);
					edge.flags = source[at].flags;
					edge.linked = source[at].linked;
					// the whole source edge survives: it takes over the neighbour's link
					const auto& c0 = sourceCorners[at];
					const auto& c1 = sourceCorners[(at + 1) % sourceCorners.size()];
					if (source[at].oppositeEdge >= 0 && relinked[at] == 0 && std::hypot(corners[k][0] - c0[0], corners[k][2] - c0[2]) < 2.0 / S
						&& std::hypot(corners[(k + 1) % path.size()][0] - c1[0], corners[(k + 1) % path.size()][2] - c1[2]) < 2.0 / S)
					{
						relinked[at] = 1;
						edge.oppositeEdge = source[at].oppositeEdge;
						edge.oppositeFace = source[at].oppositeFace;
						native::Edge& neighbour = world.edges[static_cast<std::size_t>(source[at].oppositeEdge)];
						neighbour.oppositeEdge = static_cast<std::int32_t>(world.edges.size());
						neighbour.oppositeFace = first ? static_cast<std::int32_t>(f) : static_cast<std::int32_t>(world.faces.size());
					}
				}
				world.edges.push_back(edge);
			}
			if (first) { world.faces[f] = made; first = false; }
			else world.faces.push_back(made);
		};
		for (const Path64& path : strip)
			emit(path, d.data, d.key);
		for (const Path64& path : rest)
			emit(path, face.data, face.door);
		// a neighbour whose edge met only part of a source edge now has an open edge there (the weld splits and links it)
		for (std::size_t k = 0; k < source.size(); ++k)
			if (source[k].oppositeEdge >= 0 && relinked[k] == 0)
			{
				native::Edge& neighbour = world.edges[static_cast<std::size_t>(source[k].oppositeEdge)];
				neighbour.oppositeEdge = -1;
				neighbour.oppositeFace = -1;
			}
		++keyed;
	}
	return keyed;
}

// The convex hull (xz) of a door shape's vertices, with its height band, as a DoorRing keyed by the shape's door key (data 4: open).
bool shapeDoorRing(const kworld::WorldShape& shape, DoorRing& out)
{
	std::vector<std::array<double, 2>> points;
	for (std::size_t v = 0; v + 2 < shape.vertices.size(); v += 3)
		points.push_back({ shape.vertices[v], shape.vertices[v + 2] });
	std::sort(points.begin(), points.end());
	points.erase(std::unique(points.begin(), points.end()), points.end());
	if (points.size() < 3)
		return false;
	const auto turn = [](const auto& o, const auto& a, const auto& b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); };
	std::vector<std::array<double, 2>> hull(2 * points.size());
	std::size_t k = 0;
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
	if (hull.size() < 3)
		return false;
	out = DoorRing{};
	out.data = 4;
	out.key = shape.door;
	out.lo = shape.min[1];
	for (const auto& h : hull)
		out.ring.push_back({ h[0], shape.min[1], h[1] });
	out.sign = 1.0;
	return true;
}

// Before the weld: every door's ring is cut out of the floor faces it crosses and keyed, so a locked door covers its whole opening (a face keyed by
// centre alone leaves the rest of the opening walkable). A door the generator painted no face for (its volume misses the building's own floor: a
// wall gate over open ground) has no zone-side face; its shape's hull is cut (from `area`: a cached `generated` can outlive its building pointers). Returns the keys cut, and in `fromShapes` how many were shape-only.
std::set<std::uint64_t> cutUnkeyedDoors(native::Mesh& world, const std::vector<GeneratedBuilding>& generated, const kworld::AreaGeometry& area,
	std::size_t& fromShapes)
{
	std::set<std::uint64_t> cut, faced;
	for (const GeneratedBuilding& one : generated)
	{
		for (const native::Mesh* side : { &one.zoneSide, &one.interior })
			for (const native::Face& door : side->faces)
				if (door.data == 4 || door.data == 5)
					faced.insert(door.door);
		for (const native::Face& door : one.zoneSide.faces)
			if ((door.data == 4 || door.data == 5) && cutDoorStrip(world, doorRing(one.zoneSide, door)) > 0)
				cut.insert(door.door);
	}
	for (const kworld::AreaBuilding& building : area.buildings)
		for (std::size_t i = 0; building.shapes && i < building.shapes->size(); ++i)
		{
			const kworld::WorldShape& shape = (*building.shapes)[i];
			DoorRing d;
			if (shape.use != kworld::NavUse::Door || shape.door == 0 || faced.count(shape.door) != 0 || !shapeDoorRing(shape, d))
				continue;
			d.hi = shape.max[1];
			if (cutDoorStrip(world, d) > 0 && cut.insert(shape.door).second)
				++fromShapes;
		}
	return cut;
}

// Door faces of the zone sides (data 4 / 5), painted on the ground mesh's faces whose centre lies inside them (the zone side's tops come out of
// knav::ground as plain faces: kenshi-k16.7.188 D2); a door with none keys the face nearest its middle within 3 u. Returns {door faces, faces
// painted, doors that keyed nothing}; a door in `cut` was keyed by its strip before the weld.
std::array<std::size_t, 3> paintDoors(native::Mesh& world, const std::vector<GeneratedBuilding>& generated, const std::set<std::uint64_t>& cut)
{
	std::size_t doors = 0, painted = 0, unkeyed = 0;
	for (const GeneratedBuilding& one : generated)
	{
		for (const native::Face& door : one.zoneSide.faces)
		{
			if (door.data != 4 && door.data != 5)
				continue;
			++doors;
			if (cut.count(door.door) != 0)
			{
				++painted;
				continue;
			}
			const DoorRing d = doorRing(one.zoneSide, door);
			const std::size_t before = painted;
			for (native::Face& face : world.faces)
			{
				if (face.data == 4 || face.data == 5)
					continue;
				const auto c = faceCentre(world, face);
				if (c[1] < d.lo - 2.0 || c[1] > d.hi + 2.0 || !centreInside(d, c[0], c[2]))
					continue;
				face.data = door.data;
				face.door = door.door;
				++painted;
			}
			if (painted != before)
				continue;
			if (native::Face* best = nearestFace(world, d))
			{
				best->data = door.data;
				best->door = door.door;
				++painted;
			} else {
				++unkeyed;
			}
		}
	}
	return { doors, painted, unkeyed };
}

} // namespace

std::vector<std::array<double, 2>> zoneBakeSeeds(const kworld::Heightmap& terrain, const kworld::AreaGeometry& area,
	const std::vector<Point>& seedsDef, kworld::ZoneCell zone, float waterLevel)
{
	std::vector<Point> points = zoneSeeds(terrain, area, seedsDef, zone, zone, waterLevel);
	const std::vector<Point> origins = buildingSeeds(area);
	points.insert(points.end(), origins.begin(), origins.end());
	std::vector<std::array<double, 2>> seeds;
	for (const Point& point : points)
		seeds.push_back({ point[0], point[2] });
	return seeds;
}

struct GroundWindows::Entry
{
	native::Mesh mesh;
	ground::Stats stats;
	std::vector<GeneratedBuilding> generated; // moved out by the zone's own bake
	ZoneGenReport gen; // generateBuildings' counts for it
};

std::vector<BakedTile> bakeZone(const NavConfig& config, const kworld::Heightmap& terrain, const kworld::AreaGeometry& area,
	const std::vector<std::array<double, 2>>& seeds, int threads, ZoneGenReport* report, GroundWindows* windows, kworld::ZoneCell zone)
{
	ZoneGenReport local;
	const bool wantWelded = report != nullptr && report->wantWelded;
	std::vector<GeneratedBuilding> generated;
	std::shared_ptr<GroundWindows::Entry> own;
	if (windows != nullptr)
	{
		auto found = windows->entries.find({ zone.x, zone.z });
		if (found != windows->entries.end())
			own = found->second;
	}
	if (own != nullptr)
	{
		generated = std::move(own->generated);
		local = own->gen;
	} else {
		const auto surfStart = std::chrono::steady_clock::now();
		generated = generateBuildings(area, &local, threads, &config);
		local.msSurf = msSince(surfStart);
	}
	const ground::Params params;
	const bool logWindows = std::getenv("KSIM_NAV_GEN_LOG") != nullptr;
	const double wx0 = config.originX + config.window.x0 * config.tileSize, wx1 = config.originX + (config.window.x1 + 1) * config.tileSize;
	const double wz0 = config.originZ + config.window.z0 * config.tileSize, wz1 = config.originZ + (config.window.z1 + 1) * config.tileSize;
	const double width = wx1 - wx0, depth = wz1 - wz0;
	// One window's ground: the area's cutters (no re-gather: the placements are in `area`), the zone sides of the generated buildings that reach it.
	const auto window = [&](const std::vector<GeneratedBuilding>& generated, const std::vector<std::array<double, 2>>& seeds, const kworld::AreaGeometry& source,
		double x0, double z0, double x1, double z1, ground::Stats* stats) -> native::Mesh
	{
		const ground::Grid grid = ground::makeGrid(terrain, x0, z0, x1, z1, params.stride);
		std::vector<ground::Cutter> cutters = ground::cuttersOfArea(source, terrain, grid.x0, grid.z0, grid.x0 + (grid.nx - 1) * grid.step, grid.z0 + (grid.nz - 1) * grid.step);
		for (const GeneratedBuilding& one : generated)
			if (!one.zoneSide.faces.empty() && one.max[0] >= x0 - 16.0 && one.min[0] <= x1 + 16.0 && one.max[1] >= z0 - 16.0 && one.min[1] <= z1 + 16.0)
				cutters.push_back(ground::zoneSideCutter(one.zoneSide, one.building->id));
		if (!logWindows)
			return ground::generate(grid, seeds, cutters, params, stats);
		// one stderr line per window: what it was given and where its time went (kenshi-k16.7.212: finds the slow window and its phase)
		std::size_t tris = 0, biggest = 0;
		for (std::size_t i = 0; i < cutters.size(); ++i)
		{
			tris += cutters[i].tris.size() / 3;
			if (cutters[i].tris.size() > cutters[biggest].tris.size())
				biggest = i;
		}
		native::Mesh mesh = ground::generate(grid, seeds, cutters, params, stats);
		// face sizes in xz: a mesh of slivers shows here before it shows as a tile over the polygon cap
		std::size_t under1 = 0, under10 = 0;
		double area = 0;
		for (const native::Face& face : mesh.faces)
		{
			double area2 = 0;
			for (std::uint32_t k = 0; k < face.numEdges; ++k)
			{
				const native::Edge& e = mesh.edges[face.startEdge + k];
				area2 += mesh.vertices[e.a][0] * mesh.vertices[e.b][2] - mesh.vertices[e.b][0] * mesh.vertices[e.a][2];
			}
			area += std::fabs(area2) / 2;
			under1 += std::fabs(area2) < 2;
			under10 += std::fabs(area2) < 20;
		}
		const std::pair<const std::string, double>* worst = nullptr;
		for (const auto& dropped : stats->topDropped)
			if (worst == nullptr || dropped.second > worst->second)
				worst = &dropped;
		std::fprintf(stderr, "knav: ground window %.0f,%.0f: %zu cutters, %zu triangles (largest %zu: %s), %zu top shapes (%zu by surfgen), %zu top pieces, %zu faces"
			" (%zu under 1 u2, %zu under 10 u2), area %.0f u2 (tops %.0f; %zu top faces pruned, %.0f u2, most %.0f: %s), %zu links;"
			" ms grid %.0f, cut %.0f, trim %.0f, simplify %.0f, tops %.0f, link %.0f, merge %.0f, prune %.0f\n", x0, z0, cutters.size(), tris,
			cutters.empty() ? 0 : cutters[biggest].tris.size() / 3, cutters.empty() ? "" : cutters[biggest].label.c_str(), stats->topShapes, stats->topSurfgen,
			stats->topPieces, mesh.faces.size(), under1, under10, area, stats->topArea, stats->topDroppedFaces, stats->topDroppedArea, worst ? worst->second : 0.0,
			worst ? worst->first.c_str() : "", stats->topLinks, stats->msGrid, stats->msCut, stats->msTrim, stats->msSimplify, stats->msTops, stats->msLink, stats->msMerge - stats->msLink, stats->msPrune);
		return mesh;
	};
	const auto groundStart = std::chrono::steady_clock::now();
	// own window + four neighbours (E, W, S, N), the neighbours only for their rims: stitchRims makes the shared vertex sets equal.
	// With `windows` each is the zone's own window, built once for the whole run; without, the neighbours are built here from this zone's data.
	struct Side
	{
		double x0, z0, x1, z1;
		int axis;
		double edge;
		int dx, dz;
		std::shared_ptr<GroundWindows::Entry> entry;
	};
	std::array<Side, 4> sides = { {
		{ wx1, wz0, wx1 + width, wz1, 0, wx1, 1, 0, {} }, { wx0 - width, wz0, wx0, wz1, 0, wx0, -1, 0, {} },
		{ wx0, wz1, wx1, wz1 + depth, 2, wz1, 0, 1, {} }, { wx0, wz0 - depth, wx1, wz0, 2, wz0, 0, -1, {} } } };
	const auto inWorld = [](const Side& side) { return side.x0 >= kworld::kWorldMin && side.x1 <= -kworld::kWorldMin && side.z0 >= kworld::kWorldMin && side.z1 <= -kworld::kWorldMin; };
	// a window to build: its entry, the data it is built from, and its box
	struct Job
	{
		std::shared_ptr<GroundWindows::Entry> entry;
		const std::vector<GeneratedBuilding>* generated;
		std::vector<std::array<double, 2>> seeds;
		std::shared_ptr<const kworld::AreaGeometry> area;
		double x0, z0, x1, z1;
	};
	const std::shared_ptr<const kworld::AreaGeometry> ownArea(std::shared_ptr<void>(), &area); // not owned
	std::vector<Job> jobs;
	if (windows == nullptr)
	{
		own = std::make_shared<GroundWindows::Entry>();
		jobs.push_back({ own, &generated, seeds, ownArea, wx0, wz0, wx1, wz1 });
		for (Side& side : sides)
			if (inWorld(side))
			{
				side.entry = std::make_shared<GroundWindows::Entry>();
				jobs.push_back({ side.entry, &generated, seeds, ownArea, side.x0, side.z0, side.x1, side.z1 }); // this zone's data stands for the neighbour's
			}
	} else {
		if (own == nullptr)
		{
			own = std::make_shared<GroundWindows::Entry>();
			own->gen = local;
			windows->entries[{ zone.x, zone.z }] = own;
			jobs.push_back({ own, &generated, seeds, ownArea, wx0, wz0, wx1, wz1 });
		}
		for (Side& side : sides)
		{
			if (!inWorld(side))
				continue;
			const kworld::ZoneCell other{ zone.x + side.dx, zone.z + side.dz };
			auto found = windows->entries.find({ other.x, other.z });
			if (found != windows->entries.end())
			{
				side.entry = found->second;
				continue;
			}
			side.entry = std::make_shared<GroundWindows::Entry>();
			windows->entries[{ other.x, other.z }] = side.entry;
			auto otherArea = std::make_shared<const kworld::AreaGeometry>(windows->gather(other));
			const NavConfig otherConfig = configForWorldZones(other, other);
			side.entry->generated = generateBuildings(*otherArea, &side.entry->gen, threads, &otherConfig);
			jobs.push_back({ side.entry, &side.entry->generated, zoneBakeSeeds(terrain, *otherArea, windows->seedsDef, other, config.waterLevel), otherArea,
				side.x0, side.z0, side.x1, side.z1 });
		}
	}
	std::atomic<int> next{ 0 };
	const auto drain = [&]
	{
		for (std::size_t i = next++; i < jobs.size(); i = next++)
		{
			Job& job = jobs[i];
			job.entry->mesh = window(*job.generated, job.seeds, *job.area, job.x0, job.z0, job.x1, job.z1, &job.entry->stats);
		}
	};
	std::vector<std::thread> pool;
	for (int t = 1; t < std::max(1, std::min<int>(threads, jobs.size())); ++t)
		pool.emplace_back(drain);
	drain();
	for (std::thread& thread : pool)
		thread.join();
	ground::Stats stats = own->stats;
	native::Mesh world = own->mesh;
	if (windows != nullptr)
		windows->entries.erase({ zone.x, zone.z - 1 }); // every zone that needs it is baked by now (row-order scan)
	for (Side& side : sides)
		if (side.entry != nullptr && !side.entry->mesh.faces.empty())
			local.stitched += ground::stitchRims(world, side.entry->mesh, side.axis, side.edge);
	{
		std::vector<ground::JoinedSide> joined;
		for (std::size_t i = 0; i < sides.size(); ++i)
			if (sides[i].entry != nullptr && !sides[i].entry->mesh.faces.empty())
				joined.push_back({ &sides[i].entry->mesh, &sides[i].entry->stats.topFaceIndex, &sides[i].entry->stats.topTouching });
		local.seamPruned = ground::pruneJoined(world, stats.topFaceIndex, joined, &stats.topTouching);
		stats.topFaces -= local.seamPruned;
	}
	const std::set<std::uint64_t> cutDoors = cutUnkeyedDoors(world, generated, area, local.doorsFromShapes);
	if (wantWelded)
		local.preWeldWorld = world;
	const ground::WeldStats weld = ground::weldMesh(world);
	local.collinearDropped = ground::dropCollinearWallVertices(world);
	local.welded = weld.welded;
	local.weldSplit = weld.split;
	local.weldCutRefused = weld.cutRefused;
	local.weldPaired = weld.paired;
	local.weldDropped = weld.dropped;
	local.weldOrphanOpen = weld.orphanOpen;
	local.weldOrphanGap = weld.orphanGap;
	local.weldThrough = weld.through;
	for (int i = 0; i < 4; ++i)
		local.weldByLive[i] = weld.byLive[i];
	for (const native::Face& face : world.faces)
	{
		// reflex corner: a turn against the winding of the whole face; self overlap: the fan triangles' areas add up to more than the face's
		double twice = 0.0, fan = 0.0;
		for (std::uint32_t k = 0; k < face.numEdges; ++k)
		{
			const auto& a = world.vertices[world.edges[face.startEdge + k].a];
			const auto& b = world.vertices[world.edges[face.startEdge + (k + 1) % face.numEdges].a];
			twice += a[0] * b[2] - b[0] * a[2];
		}
		const double sign = twice < 0 ? -1.0 : 1.0;
		bool reflex = false;
		for (std::uint32_t k = 0; k < face.numEdges; ++k)
		{
			const auto& a = world.vertices[world.edges[face.startEdge + k].a];
			const auto& b = world.vertices[world.edges[face.startEdge + (k + 1) % face.numEdges].a];
			const auto& c = world.vertices[world.edges[face.startEdge + (k + 2) % face.numEdges].a];
			reflex = reflex || sign * ((b[0] - a[0]) * (c[2] - b[2]) - (b[2] - a[2]) * (c[0] - b[0])) < -0.02;
		}
		const auto& o = world.vertices[world.edges[face.startEdge].a];
		for (std::uint32_t k = 1; k + 1 < face.numEdges; ++k)
		{
			const auto& a = world.vertices[world.edges[face.startEdge + k].a];
			const auto& b = world.vertices[world.edges[face.startEdge + k + 1].a];
			fan += std::fabs((a[0] - o[0]) * (b[2] - o[2]) - (a[2] - o[2]) * (b[0] - o[0]));
		}
		local.reflexFaces += reflex;
		local.selfOverlapFaces += fan > std::fabs(twice) + 0.02;
	}
	if (wantWelded)
		local.weldedWorld = world;
	local.groundFaces = world.faces.size();
	local.topFaces = stats.topFaces;
	local.topNonConvex = stats.topNonConvex;
	local.topKept = stats.topKept;
	local.msGround = msSince(groundStart);
	const auto painted = paintDoors(world, generated, cutDoors);
	local.doorFaces = painted[0];
	local.doorFacesPainted = painted[1];
	local.doorsUnkeyed = painted[2];
	// one nativeTiles call over the world mesh and the interiors
	const auto tileStart = std::chrono::steady_clock::now();
	std::vector<native::Mesh> meshes;
	meshes.push_back(std::move(world));
	for (GeneratedBuilding& one : generated)
		if (!one.interior.faces.empty())
			meshes.push_back(one.interior);
	std::vector<TileCoord> coords;
	for (int z = config.window.z0; z <= config.window.z1; ++z)
		for (int x = config.window.x0; x <= config.window.x1; ++x)
			coords.push_back({ x, z });
	NativeTileStats tileStats;
	std::vector<BakedTile> tiles = nativeTiles(config, meshes, coords, nativeDoorKeys(area.buildings), &tileStats);
	local.tiles = tiles.size();
	local.overfullTiles = tileStats.overfull;
	local.maxPolys = tileStats.maxPolys;
	local.openLinks = tileStats.openLinks;
	local.openShared = tileStats.openShared;
	local.tilePolys = tileStats.tilePolys;
	local.topPolys = std::move(tileStats.topPolys);
	local.polyFace = std::move(tileStats.polyFace);
	local.nativeFanned = tileStats.fanned;
	local.nativeRepaired = tileStats.repaired;
	local.nativeRepairStep = tileStats.repairStep;
	local.fannedFaces = tileStats.fannedFaces;
	local.msTiles = msSince(tileStart);
	if (report != nullptr)
		*report = std::move(local);
	return tiles;
}

} // namespace knav
