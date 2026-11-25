#include "knav/zone_gen.hpp"

#include "knav/ground.hpp"
#include "knav/native_tiles.hpp"
#include "knav/surfgen.hpp"
#include "kworld/coords.hpp"

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
			if (one.interior.faces.empty())
				throw std::runtime_error("no interior faces");
			// A building the runtime opens doors of needs door faces to paint: a gate whose door the generator did not make stays Recast, whose
			// door polygons carry the key (kenshi-k16.7.145 A2: the locked gate of the raid test had no door polygon).
			if (!building.nav.doors.empty() && std::none_of(one.interior.faces.begin(), one.interior.faces.end(), [](const native::Face& f) { return f.data == 4 || f.data == 5; })
				&& std::none_of(one.zoneSide.faces.begin(), one.zoneSide.faces.end(), [](const native::Face& f) { return f.data == 4 || f.data == 5; }))
				throw std::runtime_error("the building has doors and the generator made no door face");
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
		} catch (const std::exception&) {
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

// Door faces of the zone sides (data 4 / 5), painted on the ground mesh's faces whose centre lies inside them (the zone side's tops come out of
// knav::ground as plain faces: kenshi-k16.7.188 D2). Returns {door faces, faces painted}.
std::pair<std::size_t, std::size_t> paintDoors(native::Mesh& world, const std::vector<GeneratedBuilding>& generated)
{
	std::size_t doors = 0, painted = 0;
	for (const GeneratedBuilding& one : generated)
	{
		for (const native::Face& door : one.zoneSide.faces)
		{
			if (door.data != 4 && door.data != 5)
				continue;
			++doors;
			std::vector<std::array<double, 3>> ring;
			for (std::uint32_t k = 0; k < door.numEdges; ++k)
				ring.push_back(one.zoneSide.vertices[one.zoneSide.edges[door.startEdge + k].a]);
			double area2 = 0, lo = 1e30, hi = -1e30;
			for (std::size_t k = 0; k < ring.size(); ++k)
			{
				const auto& a = ring[k];
				const auto& b = ring[(k + 1) % ring.size()];
				area2 += a[0] * b[2] - b[0] * a[2];
				lo = std::min(lo, a[1]);
				hi = std::max(hi, a[1]);
			}
			const double sign = area2 < 0 ? -1.0 : 1.0;
			for (native::Face& face : world.faces)
			{
				if (face.data == 4 || face.data == 5)
					continue;
				double cx = 0, cy = 0, cz = 0;
				for (std::uint32_t k = 0; k < face.numEdges; ++k)
				{
					const auto& v = world.vertices[world.edges[face.startEdge + k].a];
					cx += v[0] / face.numEdges;
					cy += v[1] / face.numEdges;
					cz += v[2] / face.numEdges;
				}
				if (cy < lo - 2.0 || cy > hi + 2.0)
					continue;
				bool inside = true;
				for (std::size_t k = 0; k < ring.size() && inside; ++k)
				{
					const auto& a = ring[k];
					const auto& b = ring[(k + 1) % ring.size()];
					inside = sign * ((b[0] - a[0]) * (cz - a[2]) - (b[2] - a[2]) * (cx - a[0])) >= -1e-3;
				}
				if (!inside)
					continue;
				face.data = door.data;
				face.door = door.door;
				++painted;
			}
		}
	}
	return { doors, painted };
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

std::vector<BakedTile> bakeZone(const NavConfig& config, const kworld::Heightmap& terrain, const kworld::AreaGeometry& area,
	const std::vector<std::array<double, 2>>& seeds, int threads, ZoneGenReport* report)
{
	ZoneGenReport local;
	const auto surfStart = std::chrono::steady_clock::now();
	std::vector<GeneratedBuilding> generated = generateBuildings(area, &local, threads, &config);
	local.msSurf = msSince(surfStart);
	const ground::Params params;
	const double wx0 = config.originX + config.window.x0 * config.tileSize, wx1 = config.originX + (config.window.x1 + 1) * config.tileSize;
	const double wz0 = config.originZ + config.window.z0 * config.tileSize, wz1 = config.originZ + (config.window.z1 + 1) * config.tileSize;
	const double width = wx1 - wx0, depth = wz1 - wz0;
	// One window's ground: the area's cutters (no re-gather: the placements are in `area`), the zone sides of the generated buildings that reach it.
	const auto window = [&](double x0, double z0, double x1, double z1, ground::Stats* stats) -> native::Mesh
	{
		const ground::Grid grid = ground::makeGrid(terrain, x0, z0, x1, z1, params.stride);
		std::vector<ground::Cutter> cutters = ground::cuttersOfArea(area, grid.x0, grid.z0, grid.x0 + (grid.nx - 1) * grid.step, grid.z0 + (grid.nz - 1) * grid.step);
		for (const GeneratedBuilding& one : generated)
			if (!one.zoneSide.faces.empty() && one.max[0] >= x0 - 16.0 && one.min[0] <= x1 + 16.0 && one.max[1] >= z0 - 16.0 && one.min[1] <= z1 + 16.0)
				cutters.push_back(ground::zoneSideCutter(one.zoneSide, one.building->id));
		return ground::generate(grid, seeds, cutters, params, stats);
	};
	const auto groundStart = std::chrono::steady_clock::now();
	// own window + four neighbours (E, W, S, N), the neighbours only for their rims: stitchRims makes the shared vertex sets equal
	struct Side
	{
		double x0, z0, x1, z1;
		int axis;
		double edge;
		native::Mesh mesh;
	};
	std::array<Side, 4> sides = { {
		{ wx1, wz0, wx1 + width, wz1, 0, wx1, {} }, { wx0 - width, wz0, wx0, wz1, 0, wx0, {} },
		{ wx0, wz1, wx1, wz1 + depth, 2, wz1, {} }, { wx0, wz0 - depth, wx1, wz0, 2, wz0, {} } } };
	ground::Stats stats;
	native::Mesh world;
	std::atomic<int> next{ 0 };
	const auto drain = [&]
	{
		for (int i = next++; i < 5; i = next++)
		{
			if (i == 4)
				world = window(wx0, wz0, wx1, wz1, &stats);
			else
			{
				const bool inWorld = sides[i].x0 >= kworld::kWorldMin && sides[i].x1 <= -kworld::kWorldMin && sides[i].z0 >= kworld::kWorldMin && sides[i].z1 <= -kworld::kWorldMin;
				if (inWorld)
					sides[i].mesh = window(sides[i].x0, sides[i].z0, sides[i].x1, sides[i].z1, nullptr);
			}
		}
	};
	std::vector<std::thread> pool;
	for (int t = 1; t < std::max(1, std::min(threads, 5)); ++t)
		pool.emplace_back(drain);
	drain();
	for (std::thread& thread : pool)
		thread.join();
	for (Side& side : sides)
		if (!side.mesh.faces.empty())
			local.stitched += ground::stitchRims(world, side.mesh, side.axis, side.edge);
	const ground::WeldStats weld = ground::weldMesh(world);
	local.welded = weld.welded;
	local.weldSplit = weld.split;
	local.weldPaired = weld.paired;
	local.weldDropped = weld.dropped;
	local.groundFaces = world.faces.size();
	local.topFaces = stats.topFaces;
	local.topKept = stats.topKept;
	local.msGround = msSince(groundStart);
	const auto painted = paintDoors(world, generated);
	local.doorFaces = painted.first;
	local.doorFacesPainted = painted.second;
	// one nativeTiles call over the world mesh and the interiors
	const auto tileStart = std::chrono::steady_clock::now();
	std::vector<native::Mesh> meshes;
	meshes.push_back(std::move(world));
	for (GeneratedBuilding& one : generated)
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
	local.msTiles = msSince(tileStart);
	if (report != nullptr)
		*report = std::move(local);
	return tiles;
}

} // namespace knav
