// Wide findNearestPoly queries (regression: a crash inside dtNavMesh::closestPointOnPoly at
// (-33812, -105859), region 6,2, with a half extent of 200). Cause: a tile polygon whose detail
// triangles flag no edge as a polygon boundary; Detour then dereferences a null edge. The loader
// repairs the flags (navmesh.cpp repairDetailEdges). argv[1] is the Hub cache: the test clears every
// detail edge flag of its tiles, loads them, and queries every tile with boxes up to 1000 wide.
// Without the repair this crashes.

#include "knav/navmesh.hpp"
#include "test_util.hpp"

#include <DetourCommon.h>
#include <DetourNavMesh.h>

#include <iostream>

namespace {

// Clears the boundary flag of every detail triangle edge in a tile's data.
void clearDetailFlags(knav::BakedTile& tile)
{
	unsigned char* data = tile.data.data();
	const auto* header = reinterpret_cast<const dtMeshHeader*>(data);
	std::size_t at = dtAlign4(sizeof(dtMeshHeader));
	at += dtAlign4(sizeof(float) * 3 * static_cast<std::size_t>(header->vertCount));
	at += dtAlign4(sizeof(dtPoly) * static_cast<std::size_t>(header->polyCount));
	at += dtAlign4(sizeof(dtLink) * static_cast<std::size_t>(header->maxLinkCount));
	at += dtAlign4(sizeof(dtPolyDetail) * static_cast<std::size_t>(header->detailMeshCount));
	at += dtAlign4(sizeof(float) * 3 * static_cast<std::size_t>(header->detailVertCount));
	for (int t = 0; t < header->detailTriCount; ++t)
		data[at + static_cast<std::size_t>(t) * 4 + 3] = 0;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc > 1, "usage: knav_snap HUB.knav");
		const knav::NavMesh hub = knav::NavMesh::load(argv[1]);
		std::vector<knav::BakedTile> tiles = hub.tiles();
		for (knav::BakedTile& tile : tiles)
			clearDetailFlags(tile);
		const knav::NavMesh mesh = knav::NavMesh::fromTiles(hub.config(), std::move(tiles));
		const knav::NavQuery query(mesh);

		long queries = 0;
		long hits = 0;
		for (const knav::BakedTile& tile : mesh.tiles())
		{
			const float cx = static_cast<float>(mesh.config().originX + (tile.x + 0.5) * mesh.config().tileSize);
			const float cz = static_cast<float>(mesh.config().originZ + (tile.z + 0.5) * mesh.config().tileSize);
			for (float reach : { 40.0f, 200.0f, 1000.0f })
				for (float dx : { -77.0f, 0.0f, 91.0f })
				{
					knav::Point out;
					++queries;
					if (query.nearestPoint({ cx + dx, 1500.0f, cz - dx }, { reach, 3000.0f, reach }, out))
						++hits;
				}
		}
		test::require(hits > queries / 2, "wide queries find the mesh");
		std::cout << "knav_snap OK: " << queries << " queries, " << hits << " hits\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "knav_snap FAILED: " << e.what() << "\n";
		return 1;
	}
}
