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
	const ground::WeldStats weld = ground::weldMesh(world);
	local.welded = weld.welded;
	local.weldSplit = weld.split;
	local.weldPaired = weld.paired;
	local.weldDropped = weld.dropped;
	local.weldOrphanOpen = weld.orphanOpen;
	local.weldOrphanGap = weld.orphanGap;
	local.weldThrough = weld.through;
	for (int i = 0; i < 4; ++i)
		local.weldByLive[i] = weld.byLive[i];
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
	local.msTiles = msSince(tileStart);
	if (report != nullptr)
		*report = std::move(local);
	return tiles;
}

} // namespace knav
