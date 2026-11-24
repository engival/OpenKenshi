// knav_stalegeom: calibration of the staleness-by-agreement rule (kenshi-k16.7.128, docs/formats/NAVMESH_CACHE.md "Staleness by
// agreement"). One line per tile: the agreement counts of the shipped tile against our fresh bake of its zone.
//
//   knav_stalegeom --control [N]   the tiles of N fresh-hash zones with a zone file (default 60), plus every stale-hash-free
//                                  interior's tiles of the same zones: what a right shipped tile scores
//   knav_stalegeom --stale         the tiles of every zone whose hash is stale, and of each stale interior of a fresh zone
//   knav_stalegeom --zone X Z      every tile of one zone
// Line: <kind> <x>,<z> tile <tx>,<tz> kept <n> hit <n> ours <n> nonative <n> covered <pct> surplus <pct> agree <0|1>
#include "ksim/nav/navigator.hpp"

#include "knav/navgeom.hpp"
#include "knav/native.hpp"
#include "knav/navmesh.hpp"

#include "kworld/collision_area.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/navhash_source.hpp"
#include "knav/travel.hpp"

#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <tuple>
#include <unordered_set>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

void emit(const char* kind, kworld::ZoneCell cell, const std::vector<knav::BakedTile>& bake, const std::vector<knav::native::Mesh>& meshes,
	const knav::NavConfig& config, const std::set<knav::TileCoord>& only = {})
{
	for (const double tol : { knav::kAgreeTol })
	{
	const std::vector<knav::TileAgreement> rows = knav::tileAgreements(meshes, bake, config, only, tol);
	for (const knav::TileAgreement& row : rows)
		std::printf("tol %g %s %d,%d tile %d,%d kept %zu hit %zu ours %zu nonative %zu covered %.2f surplus %.2f bridges %zu cuts %zu offh %zu/%zu agree %d\n", tol, kind, cell.x,
			cell.z, row.tile.x, row.tile.z, row.nativeKept, row.nativeHit, row.oursTotal, row.oursNoNative, row.covered(),
			row.surplus(), row.bridges, row.cuts, row.offHeight, row.eroded, row.agrees() ? 1 : 0);
	}
	std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
	namespace fs = std::filesystem;
	const std::string dir = knav::native::navtilesDir(KENSHI_DATA);
	if (!fs::is_directory(dir) || argc < 2)
		return argc < 2 ? 2 : 77;
	const std::string mode = argv[1];
	if (mode == "--leg" && argc == 10)
	{
		// Routes between an inside point (IX IY IZ) and the points 35 u around the door at X, Z, at radius 0 and 0.5, and the
		// clearance of each portal of the one that works (KNAV_NO_AGREE=1 for the all-rebake mesh).
		const double dx = std::atof(argv[2]), dz = std::atof(argv[3]);
		const double ix = std::atof(argv[7]), iy = std::atof(argv[8]), iz = std::atof(argv[9]); // the target; argv[4..6] the inside point
		const double px = std::atof(argv[4]), py = std::atof(argv[5]), pz = std::atof(argv[6]);
		kworld::ZoneCell centre = kworld::zoneCellOf(dx, dz);
		if (const char* env = std::getenv("KNAV_CENTRE_CELL")) // the zone window of the sweep: its town's cell
			std::sscanf(env, "%d,%d", &centre.x, &centre.z);
		std::vector<kworld::ZoneCell> zones;
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
				zones.push_back({ centre.x + i, centre.z + j });
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, zones, [](const std::string&) {});
		nav->applyDoors(knav::DoorBook{});
		{
			// The flag-respecting flood fill (what a filter allows) from the inside point and from the target.
			const knav::NavMesh& mesh = nav->mesh();
			const dtNavMesh& dt = mesh.detour();
			dtNavMeshQuery query;
			query.init(&dt, 2048);
			dtQueryFilter filter;
			filter.setIncludeFlags(0xffff);
			filter.setExcludeFlags(0);
			const float extent[3] = { 6.0f, 20.0f, 6.0f };
			std::vector<std::array<double, 3>> ends{ { px, py, pz }, { ix, iy, iz } };
			if (const char* from = std::getenv("KNAV_FROM"))
			{
				double fx, fy, fz;
				std::sscanf(from, "%lf %lf %lf", &fx, &fy, &fz);
				ends.push_back({ fx, fy, fz });
			}
			for (const auto& end : ends)
			{
				const float pt[3] = { static_cast<float>(end[0]), static_cast<float>(end[1]), static_cast<float>(end[2]) };
				dtPolyRef start = 0;
				float nearest[3];
				query.findNearestPoly(pt, extent, &filter, &start, nearest);
				std::deque<dtPolyRef> queue;
				std::unordered_set<dtPolyRef> seen;
				if (start != 0)
				{
					queue.push_back(start);
					seen.insert(start);
				}
				std::map<std::pair<int, int>, std::size_t> perTile;
				std::size_t blocked = 0;
				while (!queue.empty())
				{
					const dtPolyRef ref = queue.front();
					queue.pop_front();
					const dtMeshTile* tile = nullptr;
					const dtPoly* poly = nullptr;
					dt.getTileAndPolyByRefUnsafe(ref, &tile, &poly);
					++perTile[{ tile->header->x, tile->header->y }];
					for (unsigned k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
					{
						const dtMeshTile* nt = nullptr;
						const dtPoly* np = nullptr;
						dt.getTileAndPolyByRefUnsafe(tile->links[k].ref, &nt, &np);
						if (np->flags == 0)
						{
							++blocked;
							continue;
						}
						if (seen.insert(tile->links[k].ref).second)
							queue.push_back(tile->links[k].ref);
					}
				}
				std::size_t total = 0;
				for (const auto& [t, n] : perTile)
					total += n;
				std::printf("flood from %.0f %.0f %.0f: poly %s, %zu polys in %zu tiles, %zu links into flags-0 polys\n", end[0], end[1], end[2],
					start ? "found" : "NONE", total, perTile.size(), blocked);
			}
		}
		if (const char* from = std::getenv("KNAV_FROM"))
		{
			// Plain Detour A* both ways between the sweep's outside point and the inside point, with a large node budget.
			double fx, fy, fz;
			std::sscanf(from, "%lf %lf %lf", &fx, &fy, &fz);
			dtNavMeshQuery big;
			big.init(&nav->mesh().detour(), 65535);
			dtQueryFilter all;
			all.setIncludeFlags(0xffff);
			all.setExcludeFlags(0);
			const float ext[3] = { 6.0f, 20.0f, 6.0f };
			const float a[3] = { static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(fz) };
			const float b[3] = { static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz) };
			dtPolyRef ra = 0, rb = 0;
			float na[3], nb[3];
			big.findNearestPoly(a, ext, &all, &ra, na);
			big.findNearestPoly(b, ext, &all, &rb, nb);
			for (int way = 0; way < 2; ++way)
			{
				dtPolyRef path[4096];
				int count = 0;
				const dtStatus st = way == 0 ? big.findPath(ra, rb, na, nb, &all, path, &count, 4096) : big.findPath(rb, ra, nb, na, &all, path, &count, 4096);
				std::printf("plain A* %s: status %x polys %d (partial %d)\n", way == 0 ? "outside->inside" : "inside->outside", st, count, (st & DT_PARTIAL_RESULT) ? 1 : 0);
				if (way == 0 && (st & DT_PARTIAL_RESULT) && count > 0)
				{
					const dtMeshTile* tile = nullptr;
					const dtPoly* poly = nullptr;
					nav->mesh().detour().getTileAndPolyByRefUnsafe(path[count - 1], &tile, &poly);
					std::printf("  stops in tile %d,%d poly %u\n", tile->header->x, tile->header->y, nav->mesh().detour().decodePolyIdPoly(path[count - 1]));
				}
			}
		}
		for (int k = 0; k < 8; ++k)
		{
			double ox = dx + 70.0 * std::cos(k * 0.785398), oz = dz + 70.0 * std::sin(k * 0.785398), oy = iy;
			if (const char* from = std::getenv("KNAV_FROM")) // the sweep's own outside point: "x y z"
			{
				if (k > 0)
					break;
				std::sscanf(from, "%lf %lf %lf", &ox, &oy, &oz);
			}
			const auto spot = nav->walker().snap({ ox, oy, oz }, 20.0);
			if (!spot)
				continue;
			for (const double radius : { 0.0, 0.05, 0.1, 0.25, 0.5 })
			{
				const ksim::Route in = nav->routeFrom({ spot->x, spot->y, spot->z }, ix, iz, iy, false, radius, true);
				const ksim::Route out = nav->routeFrom({ px, py, pz }, spot->x, spot->z, spot->y, false, radius, true);
				if (radius == 0.0 && in.found)
				{
					knav::NavQuery nq(nav->mesh());
					std::vector<knav::Point> pts;
					for (const auto& g : in.points)
						if (const auto at = nav->walker().snap({ g.x, iy, g.z }, 40.0))
							pts.push_back({ static_cast<float>(at->x), static_cast<float>(at->y), static_cast<float>(at->z) });
					const std::vector<knav::CorridorPortal> portals = nq.corridor(pts, 6.0f, knav::TravelFilter{});
					for (const knav::CorridorPortal& portal : portals)
						if (std::min(portal.clearLeft, portal.clearRight) < 1.0f)
						{
							const dtMeshTile* tile = nullptr;
							const dtPoly* poly = nullptr;
							nav->mesh().detour().getTileAndPolyByRefUnsafe(static_cast<dtPolyRef>(portal.ref), &tile, &poly);
							std::printf("   narrow portal in tile %d,%d (%s) poly area %d: left %.1f %.1f %.1f right %.1f %.1f %.1f clear %.2f %.2f\n", tile->header->x,
								tile->header->y, "?", poly->getArea(), portal.left[0], portal.left[1], portal.left[2], portal.right[0],
								portal.right[1], portal.right[2], portal.clearLeft, portal.clearRight);
						}
				}
				std::printf("dir %d out(%.0f %.0f %.0f) radius %.1f: outside->inside %s %zu pts (%.*s), inside->outside %s %zu pts\n", k,
					spot->x, spot->y, spot->z, radius, in.found ? "found" : "NONE", in.points.size(), static_cast<int>(in.check.size()),
					in.check.data(), out.found ? "found" : "NONE", out.points.size());
			}
		}
		return 0;
	}
	if (mode == "--native" && argc == 5)
	{
		// The shipped faces within R of X, Z (all meshes of the zone and its neighbours), edge by edge: opposite face (w = wall) and the link flag.
		const double dx = std::atof(argv[2]), dz = std::atof(argv[3]), reach = std::atof(argv[4]);
		const kworld::ZoneCell centre = kworld::zoneCellOf(dx, dz);
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
				for (const knav::native::Mesh& mesh : knav::native::loadTile(dir, centre.x + i, centre.z + j))
					for (std::size_t f = 0; f < mesh.faces.size(); ++f)
					{
						const knav::native::Face& face = mesh.faces[f];
						bool near = false;
						for (std::uint16_t e = 0; e < face.numEdges; ++e)
						{
							const auto& v = mesh.vertices[mesh.edges[face.startEdge + e].a];
							near = near || std::hypot(v[0] - dx, v[2] - dz) < reach;
						}
						if (!near)
							continue;
						std::printf("mesh %s tile %d,%d face %zu data %d:", mesh.interior() ? mesh.info.c_str() : "world", mesh.tileX, mesh.tileZ, f, face.data);
						for (std::uint16_t e = 0; e < face.numEdges; ++e)
						{
							const knav::native::Edge& edge = mesh.edges[face.startEdge + e];
							const auto& v = mesh.vertices[edge.a];
							std::printf(" (%.2f %.2f %.2f)%s", v[0], v[1], v[2], edge.oppositeFace >= 0 ? "n" : edge.linked ? "L" : "w");
						}
						std::printf("\n");
					}
		return 0;
	}
	if (mode == "--polys" && argc == 5)
	{
		// Every polygon with a vertex within R of X, Z: tile, index, area, vertices, and per edge the neighbour (n) or wall (w).
		const double dx = std::atof(argv[2]), dz = std::atof(argv[3]), reach = std::atof(argv[4]);
		kworld::ZoneCell centre = kworld::zoneCellOf(dx, dz);
		if (const char* env = std::getenv("KNAV_CENTRE_CELL"))
			std::sscanf(env, "%d,%d", &centre.x, &centre.z);
		std::vector<kworld::ZoneCell> zones;
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
				zones.push_back({ centre.x + i, centre.z + j });
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, zones, [](const std::string&) {});
		const dtNavMesh& dt = nav->mesh().detour();
		for (const knav::BakedTile& baked : nav->mesh().tiles())
		{
			const dtMeshTile* tile = dt.getTileAt(baked.x, baked.z, 0);
			for (int p = 0; tile != nullptr && p < tile->header->polyCount; ++p)
			{
				const dtPoly& poly = tile->polys[p];
				bool near = false;
				for (int v = 0; v < poly.vertCount; ++v)
					near = near || std::hypot(tile->verts[poly.verts[v] * 3] - dx, tile->verts[poly.verts[v] * 3 + 2] - dz) < reach;
				if (!near)
					continue;
				std::printf("poly %d,%d/%d area %d flags %u:", baked.x, baked.z, p, poly.getArea(), poly.flags);
				for (int v = 0; v < poly.vertCount; ++v)
					std::printf(" (%.2f %.2f %.2f)%c", tile->verts[poly.verts[v] * 3], tile->verts[poly.verts[v] * 3 + 1], tile->verts[poly.verts[v] * 3 + 2],
						poly.neis[v] == 0 ? 'w' : (poly.neis[v] & 0x8000) ? 'x' : 'n');
				std::printf("\n");
			}
		}
		return 0;
	}
	if (mode == "--orphans" && argc == 4)
	{
		// Door-area polygons (area 3 / 4) that belong to no door key, within 200 u of X, Z: a door the sim cannot open.
		const double dx = std::atof(argv[2]), dz = std::atof(argv[3]);
		kworld::ZoneCell centre = kworld::zoneCellOf(dx, dz);
		if (const char* env = std::getenv("KNAV_CENTRE_CELL")) // the zone window of the sweep: its town's cell
			std::sscanf(env, "%d,%d", &centre.x, &centre.z);
		std::vector<kworld::ZoneCell> zones;
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
				zones.push_back({ centre.x + i, centre.z + j });
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, zones, [](const std::string&) {});
		nav->applyDoors(knav::DoorBook{});
		const knav::NavMesh& mesh = nav->mesh();
		const dtNavMesh& dt = mesh.detour();
		std::set<std::tuple<int, int, int>> keyed;
		for (const knav::DoorKey key : mesh.doorKeys())
			for (const knav::PolyLoc& loc : mesh.doorPolys(key))
				keyed.insert({ loc.tileX, loc.tileZ, loc.poly });
		for (const knav::BakedTile& baked : mesh.tiles())
		{
			const dtMeshTile* tile = dt.getTileAt(baked.x, baked.z, 0);
			for (int p = 0; tile != nullptr && p < tile->header->polyCount; ++p)
			{
				const dtPoly& poly = tile->polys[p];
				if ((poly.getArea() != 3 && poly.getArea() != 4) || keyed.count({ baked.x, baked.z, p }) != 0)
					continue;
				float cx = 0, cy = 0, cz = 0;
				for (int v = 0; v < poly.vertCount; ++v)
				{
					cx += tile->verts[poly.verts[v] * 3] / poly.vertCount;
					cy += tile->verts[poly.verts[v] * 3 + 1] / poly.vertCount;
					cz += tile->verts[poly.verts[v] * 3 + 2] / poly.vertCount;
				}
				if (std::hypot(cx - dx, cz - dz) < 250.0)
					std::printf("orphan door poly tile %d,%d poly %d area %d at %.1f %.1f %.1f (%s)\n", baked.x, baked.z, p, poly.getArea(),
						cx, cy, cz, "key 0");
			}
		}
		return 0;
	}
	if (mode == "--door" && argc == 4)
	{
		// Where a door's polygons are joined: the flood fill over Detour links from the door at (X, Z), per tile, with the
		// tile's source (agreed = shipped, else our bake). Run with KNAV_NO_AGREE=1 for the all-rebake mesh.
		const double dx = std::atof(argv[2]), dz = std::atof(argv[3]);
		kworld::ZoneCell centre = kworld::zoneCellOf(dx, dz);
		if (const char* env = std::getenv("KNAV_CENTRE_CELL")) // the zone window of the sweep: its town's cell
			std::sscanf(env, "%d,%d", &centre.x, &centre.z);
		std::vector<kworld::ZoneCell> zones;
		for (int j = -1; j <= 1; ++j)
			for (int i = -1; i <= 1; ++i)
				zones.push_back({ centre.x + i, centre.z + j });
		const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, zones, [](const std::string&) {});
		nav->applyDoors(knav::DoorBook{});
		const knav::NavMesh& mesh = nav->mesh();
		const dtNavMesh& dt = mesh.detour();
		std::set<std::pair<int, int>> shipped;
		for (const kworld::ZoneCell& zone : zones)
			if (const auto* report = nav->nativeReport(zone))
				for (const knav::TileCoord& t : report->agreed)
					shipped.insert({ t.x, t.z });
		for (const knav::DoorKey key : mesh.doorKeys())
		{
			knav::Point at;
			if (!mesh.doorCentre(key, at) || std::hypot(at[0] - dx, at[2] - dz) > 120.0)
				continue;
			std::printf("door %llu at %.1f %.1f %.1f\n", static_cast<unsigned long long>(key), at[0], at[1], at[2]);
			std::deque<dtPolyRef> queue;
			std::unordered_set<dtPolyRef> seen;
			for (const knav::PolyLoc& loc : mesh.doorPolys(key))
			{
				const dtMeshTile* tile = dt.getTileAt(loc.tileX, loc.tileZ, 0);
				const dtPolyRef ref = dt.getPolyRefBase(tile) | static_cast<dtPolyRef>(loc.poly);
				std::printf("  door poly tile %d,%d poly %d (%s)\n", loc.tileX, loc.tileZ, loc.poly,
					shipped.count({ loc.tileX, loc.tileZ }) ? "shipped" : "ours");
				if (seen.insert(ref).second)
					queue.push_back(ref);
				const dtPoly& dp = tile->polys[loc.poly];
				for (unsigned k = dp.firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
				{
					const dtLink& link = tile->links[k];
					const float* a = &tile->verts[dp.verts[link.edge] * 3];
					const float* b = &tile->verts[dp.verts[(link.edge + 1) % dp.vertCount] * 3];
					float width = std::hypot(b[0] - a[0], b[2] - a[2]);
					if (link.side != 0xff)
						width *= (link.bmax - link.bmin) / 255.0f;
					const dtMeshTile* nt = nullptr;
					const dtPoly* np = nullptr;
					dt.getTileAndPolyByRefUnsafe(link.ref, &nt, &np);
					std::printf("    link edge %d to tile %d,%d poly %u area %d side %d width %.2f y %.2f/%.2f\n", link.edge,
						nt->header->x, nt->header->y, dt.decodePolyIdPoly(link.ref), np->getArea(), link.side, width, a[1], b[1]);
				}
			}
			std::map<std::pair<int, int>, std::size_t> perTile;
			while (!queue.empty())
			{
				const dtPolyRef ref = queue.front();
				queue.pop_front();
				const dtMeshTile* tile = nullptr;
				const dtPoly* poly = nullptr;
				dt.getTileAndPolyByRefUnsafe(ref, &tile, &poly);
				++perTile[{ tile->header->x, tile->header->y }];
				for (unsigned k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
					if (seen.insert(tile->links[k].ref).second)
						queue.push_back(tile->links[k].ref);
			}
			std::size_t total = 0;
			for (const auto& [t, n] : perTile)
				total += n;
			std::printf("  component: %zu polys in %zu tiles\n", total, perTile.size());
			const knav::PolyLoc first = mesh.doorPolys(key).front();
			for (int tz = first.tileZ - 1; tz <= first.tileZ + 1; ++tz)
				for (int tx = first.tileX - 1; tx <= first.tileX + 1; ++tx)
				{
					const dtMeshTile* tile = dt.getTileAt(tx, tz, 0);
					if (tile == nullptr)
						continue;
					const auto in = perTile.find({ tx, tz });
					std::printf("   tile %d,%d %s: %zu of %d polys in the door's component\n", tx, tz,
						shipped.count({ tx, tz }) ? "shipped" : "ours", in == perTile.end() ? std::size_t{ 0 } : in->second,
						tile->header->polyCount);
				}
		}
		return 0;
	}
	const auto nav = ksim::MeshNavigator::openNative(KENSHI_DATA, { { 10, 10 } }, [](const std::string&) {});
	kworld::Heightmap terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
	kworld::AreaGatherer gatherer(KENSHI_DATA, terrain);
	kworld::NavHashSource source(KENSHI_DATA, gatherer.sources());
	std::set<std::pair<int, int>> cells;
	if (mode == "--zone" && argc == 4)
		cells.insert({ std::atoi(argv[2]), std::atoi(argv[3]) });
	else
		for (const fs::directory_entry& entry : fs::directory_iterator(dir))
		{
			int x, z;
			if (std::sscanf(entry.path().filename().string().c_str(), "tile%d.%d.hkt", &x, &z) == 2)
				cells.insert({ x, z });
		}
	int controlLeft = argc > 2 && mode == "--control" ? std::atoi(argv[2]) : 60;
	const double stride = mode == "--control" ? static_cast<double>(cells.size()) / 400.0 : 1.0; // spread the control over the map
	double nextPick = 0.0;
	std::size_t seen = 0;
	for (const auto& [x, z] : cells)
	{
		const kworld::ZoneCell cell{ x, z };
		knav::native::StoredHashes stored;
		const std::vector<knav::native::Mesh> meshes = knav::native::loadTile(dir, x, z, &stored);
		if (meshes.empty())
			continue;
		const std::optional<std::uint32_t> storedZone = stored.hasZone ? std::optional(stored.zone) : std::nullopt;
		const kworld::NavHashSource::ZoneHashes hashes = source.zone(cell);
		const kworld::StaleVerdict verdict = kworld::staleVerdict(storedZone, hashes.hash);
		const bool stale = verdict != kworld::StaleVerdict::Fresh && !kworld::forestOnlyStale(storedZone, hashes);
		const knav::NavConfig config = knav::configForWorldZones(cell, cell);
		if (mode == "--zone")
		{
			emit("zone", cell, nav->rebakeZone(cell), meshes, config);
			continue;
		}
		if (mode == "--control")
		{
			if (stale || controlLeft <= 0 || !kworld::navZoneFileExists(KENSHI_DATA, cell) || static_cast<double>(seen++) < nextPick)
				continue;
			nextPick += stride;
			--controlLeft;
			emit("fresh", cell, nav->rebakeZone(cell), meshes, config);
			continue;
		}
		// --stale: the zone's whole square, or the tiles of each stale interior of a fresh zone
		std::set<knav::TileCoord> only;
		if (!stale)
		{
			std::size_t interiorIndex = 0;
			for (const knav::native::Mesh& mesh : meshes)
			{
				if (!mesh.interior())
					continue;
				const std::optional<std::uint32_t>& storedHash = stored.interiors[interiorIndex++];
				std::uint64_t uid = 0;
				try { uid = std::stoull(mesh.info, nullptr, 16); } catch (const std::exception&) {}
				const kworld::NavHashSource::InteriorHashes ours = source.interior(cell, uid);
				if (kworld::staleVerdict(storedHash, ours.hash, ours.basis) == kworld::StaleVerdict::Fresh)
					continue;
				double minX = 1e30, maxX = -1e30, minZ = 1e30, maxZ = -1e30;
				for (const std::array<double, 3>& v : mesh.vertices)
				{
					minX = std::min(minX, v[0]);
					maxX = std::max(maxX, v[0]);
					minZ = std::min(minZ, v[2]);
					maxZ = std::max(maxZ, v[2]);
				}
				const auto tileOf = [&](double value, double origin) { return static_cast<int>(std::floor((value - origin) / config.tileSize)); };
				for (int tz = tileOf(minZ, config.originZ); tz <= tileOf(maxZ, config.originZ); ++tz)
					for (int tx = tileOf(minX, config.originX); tx <= tileOf(maxX, config.originX); ++tx)
						only.insert({ tx, tz });
			}
			if (only.empty())
				continue;
		}
		const std::vector<knav::BakedTile> bake = nav->rebakeZone(cell);
		if (stale)
		{
			emit("stale", cell, bake, meshes, config);
		} else {
			emit("interior", cell, bake, meshes, config, only);
		}
	}
	return 0;
}
