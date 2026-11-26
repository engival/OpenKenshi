#pragma once

// The original's baked navmesh (data/newland/land/navtiles/tileI.J.hkt): Havok 2014 binary tagfiles, one per zone, holding the
// zone's world mesh and its building interiors. The runtime base mesh (knav/native_tiles.hpp converts it to Detour; the sim
// streams it through MeshNavigator::openNative), and the reference our bake is measured against (knav_compare). Format and
// coordinates: the kinspect session's findings/navmesh.md ("Tagfile format", "Tile contents and coordinates"); this reader is
// our own code written from those notes.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace knav::native {

// A face's edges are edges[startEdge .. startEdge + numEdges), each running from vertex a to vertex b.
struct Face
{
	std::uint32_t startEdge = 0;
	std::uint16_t numEdges = 0;
	std::int32_t data = 0; // faceData: 4 = an open door, 5 = closed, others material ids (UNSURE)
	bool top = false; // an object top the ground generator made (provenance for the fold census; not stored, not part of the mesh)
	std::uint64_t door = 0; // a door face (data 4 / 5) painted by the generator: the door's kworld::DoorKey; 0 = none (shipped faces carry no id)
};

struct Edge
{
	std::uint32_t a = 0;
	std::uint32_t b = 0;
	std::int32_t oppositeEdge = -1;
	std::int32_t oppositeFace = -1; // -1: a boundary of this mesh
	std::uint8_t flags = 0;
	bool linked = false; // a streaming connection (tile seam, interior <-> world): a boundary a walker crosses
};

struct Mesh
{
	int tileX = 0;
	int tileZ = 0;
	std::string info; // an interior's building id (the Info "hexid;x;y;z"); empty for the zone's world mesh
	std::array<double, 3> base{}; // world position of the mesh origin
	std::vector<std::array<double, 3>> vertices; // world coordinates
	std::vector<Face> faces;
	std::vector<Edge> edges;
	std::vector<std::pair<std::string, double>> scalars; // the hkaiNavMesh's own int / real members (erosionRadius, ...), for dev tools

	bool interior() const noexcept { return !info.empty(); }
	// A wall: an edge with no opposite face and no streaming link.
	bool wall(std::size_t edge) const noexcept { return edges[edge].oppositeFace < 0 && !edges[edge].linked; }
};

// The stored hashes a shipped tile carries beside its meshes (the original's stale-tile check, kinspect
// findings/navhash.md section 5): a "Hash" string before each interior's Info variant, and the zone's after the interiors.
struct StoredHashes
{
	std::vector<std::optional<std::uint32_t>> interiors; // one per interior mesh, in mesh order; nullopt: none stored
	std::uint32_t zone = 0;
	bool hasZone = false; // the tile stores a zone hash (the last Hash variant)
};

// The navtiles directory of a game data directory.
std::string navtilesDir(const std::string& dataDir);

// Every mesh of tileI.J.hkt in `dir`, in file order (the world mesh first); empty when the tile has no file.
// `stored`, when given, receives the tile's stored hash strings. Throws std::runtime_error on a file that is not a
// tagfile or does not parse.
std::vector<Mesh> loadTile(const std::string& dir, int i, int j, StoredHashes* stored = nullptr);

// Every mesh of a KNM1 file: kinspect's export of a live game's meshes (`tools/knative.py` write_knm; e.g.
// `findings/navbuild/hub_live.knm`, the Hub after its load rebake). A mesh's tile is the zone holding its vertices'
// centre. Throws std::runtime_error on a file that is not KNM1 or is truncated.
std::vector<Mesh> loadKnm(const std::string& path);

} // namespace knav::native
