#pragma once
// knav::ground: a bespoke terrain navmesh generator (kenshi-ffmi.2), no Recast, no voxels. The terrain is a height grid, one level by
// construction, so the job is 2.5D:
//   1. the grid as triangles (2 per cell, the diagonal that follows the ground), the walkable ones kept (slope <= 40 deg, the original's
//      zone setting, navbake_static C42);
//   2. greedy vertex removal, cheapest first: an inner vertex goes when every original sample it covered stays within `heightError` of the
//      new surface; a border vertex (walkable / not) by the original's border rule (rwh291): projected cap area under `borderArea`
//      (concave: `concaveArea`) and height off the chord within `borderHeight`, plus the same sample check;
//   3. planar merge of the triangles into convex polygons (Hertel-Mehlhorn, normals within `planarCos`);
//   4. seed pruning: a component survives if it holds a seed point (the original's 3x3 probe grid per zone, C31 / C42) or touches the rim.
// The window rim is pre-simplified on its own samples (Douglas-Peucker, `heightError`) so two neighbouring windows keep the same rim vertices.
// Before step 2 the walkable triangles are cut by obstacles (Cutter, below): each touched grid triangle splits into convex pieces along the
// cutters' xz edges and the lines where a cutter plane crosses ground + cutBase / + characterHeight, each piece kept or dropped by its centre,
// T-junctions repaired, the pieces re-triangulated; the outlines then simplify by the border rule like any other border.
// Walkable tops of objects (section 12): faces cut against the terrain grid, linked to the ground and each other, pruned to what reaches the ground.
// Buildings' zone-side surfaces (surfgen) join as prebuilt tops (zoneSideCutter): terrain cut, links to the ground, prune to ground or seam.
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "knav/native.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/heightmap.hpp"

namespace knav::ground
{

struct Grid
{
	int nx = 0, nz = 0;			// samples
	double x0 = 0, z0 = 0, step = 18;	// world position of sample (0, 0), spacing
	std::vector<double> y;		// nx * nz heights, row-major in z
	double at(int i, int j) const { return y[(std::size_t)j * nx + i]; }
};

// An obstacle the ground is cut by: one collision shape (a building part, a mask box, a rock), world triangles. The ground under it is
// removed where any of its solid spans reaches into (ground + cutBase, ground + characterHeight): hits on its triangles straight above /
// below the point, paired into spans, an odd count meaning a shell open at the bottom (a rock), each hit also a span of its own.
// `projected`: the xz cover cuts at every height (NavUse::Projected).
// `top`: the walkable tops it keeps (NAVMESH_GENERATOR.md section 12, kenshi-k16.7.197). Zone: the up-facing faces of a Walkable shape
// (a no-mask building's walkable parts, walkable foliage); Deck: a map feature's faces, only where the clear space under them exceeds
// characterHeight (the deck rule). The triangles of a closed solid are wound outward.
struct Cutter
{
	enum class Top { None, Zone, Deck };
	std::vector<std::array<double, 3>> tris;	// 3 points per triangle
	bool projected = false;
	Top top = Top::None;
	std::string label;	// where it came from (a building id, "foliage", "feature @ x, z"): the report's key
	// A building's zone side from surfgen (zoneSideCutter): faces already cut and linked as surfgen does. They keep their tops (terrain cut, links,
	// prune like any top) but cut nothing: not the ground (the building's own shapes did), not another top. A top component that touches one of
	// `seam` (the segments where the zone side meets the interior) stays too, though it reaches no ground.
	bool prebuilt = false;
	std::vector<std::array<std::array<double, 3>, 2>> seam;
	std::array<double, 3> min{}, max{};
};

struct Params
{
	double slopeDeg = 40;
	double heightError = 15;	// inner vertices: max vertical distance of any covered sample to the simplified surface
	double borderArea = 10;		// border vertices: projected cap area (u2), convex corner; the zone job's override (rwh291) is 10 u2 / 1 u
	double concaveArea = 1.5;	// concave corner
	double borderHeight = 1;	// vertical distance of the border vertex to its chord
	double planarCos = 0.99;	// merge: polygon normals at least this aligned
	bool keepRim = true;		// pruning keeps components that reach the window rim
	int stride = 2;				// the caller's sampling: fullmap.tif every 2nd sample is the original's 36 u zone grid
	double cutBase = 0.1;		// cutters: the span over the ground that blocks (knav surfgen's cutBase / characterHeight)
	double characterHeight = 18;
	// Spike trim (after the cut, before the simplifier): a convex outline corner sharper than `trimAngleDeg` is cut across where it is
	// `trimWidth` wide, the tip dropped (a gap between two collision shapes that no character fits). 0 = off.
	double trimWidth = 2;
	double trimAngleDeg = 30;
	double trimMaxDepth = 60;	// a needle (angle near 0) is cut at most this far from its tip
	// Walkable tops (section 12 steps 1-5)
	double topSlopeDeg = 60;	// surfgen's building slope limit
	double topTerrainTol = 0.25;	// a top piece this close to the terrain (or under it) is the ground's
	double deckShare = 0.5;		// a feature face is a deck when more than characterHeight is clear under this share of its area
	double deckMaxSlopeDeg = 90;	// a feature deck face is no steeper than this (90 = off)
	double deckShapeShare = 0.2;	// a feature shape's tops count only when its seed faces cover this share of its up-facing area (0 = off; F1: a cliff block 0.05-0.1, a Trestle Bridge >= 0.3)
	double matchSeparation = 2;	// the edge matcher of surfgen.hpp (Settings::matchSeparationMax / matchStep / matchOverlap)
	double matchStep = 5;
	double matchOverlap = 0.2;
	bool fixWholeRim = true;	// no rim sample goes by the inner rule: two neighbouring windows then keep the same rim vertices (no T-junction across the seam)
	bool keepLooseTops = false;	// debug: tops that reach no ground stay, so a viewer or a test can see what the prune would drop
};

struct Stats
{
	std::size_t cells = 0, walkableTris = 0, rimKept = 0, removed = 0, trisAfter = 0, faces = 0, components = 0, droppedComponents = 0,
		droppedFaces = 0;
	double maxError = 0;		// the largest sample error accepted
	double msGrid = 0, msSimplify = 0, msMerge = 0, msPrune = 0;
	std::size_t cutters = 0, cutGroundTris = 0, piecesKept = 0, piecesDropped = 0, cutVertices = 0, tjunctions = 0, slivers = 0, clipFailed = 0;
	double msCut = 0;
	std::size_t trimmedTips = 0, trimSkipped = 0;
	double trimmedArea = 0, msTrim = 0;
	std::vector<std::array<std::array<double, 3>, 3>> culled;	// the trim's dropped triangles, world
	// Walkable tops
	std::size_t topShapes = 0, topTris = 0, deckTris = 0, deckRejected = 0, topPieces = 0, topFaces = 0, topLinks = 0, topDroppedFaces = 0;
	double topArea = 0, topDroppedArea = 0;	// xz, of the output faces / of the faces the prune dropped
	double msTops = 0, msLink = 0;
	std::map<std::string, double> topDropped;	// dropped xz area per Cutter::label
	std::map<std::string, double> topKept;	// kept xz area per Cutter::label
	std::vector<std::string> underTerrain;	// the shapes of walkable objects that lie wholly under the terrain (the Pontoon Bridge outliers): reported, inert
	std::vector<std::uint32_t> topFaceIndex;	// the output mesh's faces that are tops
};

// Seeds: xz points. Output in world coordinates; rim edges are marked `linked` (a seam to the next window). Cutters in the order given
// (bigger first splits less).
native::Mesh generate(const Grid& grid, const std::vector<std::array<double, 2>>& seeds, const std::vector<Cutter>& cutters, const Params& params,
	Stats* stats = nullptr);

// A surfgen zone-side mesh (surfgen::Side::Zone, world coordinates) as a prebuilt top cutter: the faces fanned into up-facing triangles, the
// `linked` boundary edges (the seam to the interior) as `seam`.
Cutter zoneSideCutter(const native::Mesh& zoneSide, const std::string& label);

// Two neighbouring windows' meshes meet on the line coordinate `edge` of axis `axis` (0: x, 2: z). Every vertex of one on that line that lies inside a
// boundary edge of the other on it is inserted there (the height along that edge), both ways: no T-junction across the seam, the rim vertex sets equal
// in xz. Returns the vertices inserted. (Each window's cut points on the rim exist only where its own ground is kept; the grid samples are the same.)
std::size_t stitchRims(native::Mesh& a, native::Mesh& b, int axis, double edge);

struct WeldStats
{
	std::size_t welded = 0; // vertices merged into another (within `grid` in xz and `climb` in y)
	std::size_t split = 0; // vertices inserted into a boundary edge they lie on
	std::size_t dropped = 0; // faces left with no width (area / half perimeter < 1/32 u)
	std::size_t paired = 0; // boundary edge pairs that became opposites (counted once per pair)
};

// One weld pass over a window's mesh (kenshi-k16.7.189, ksim 2026-11-25): vertices within `grid` in xz and `climb` in y become one; every vertex lying on
// another face's boundary edge (xz distance <= grid, y within `climb` at that point; not a `linked` rim edge, not an edge with an opposite) splits that
// edge; boundary edges that then run over the same two vertices pair up as opposites. No T-junction is left between ground, zone-side and top faces, so
// Detour's shared-edge links hold. Generic: no building case. Faces keep their order (a face left with fewer than three edges is dropped).
WeldStats weldMesh(native::Mesh& m, double grid = 0.125, double climb = 5.0);

// What gatherCutters collected.
struct CutterCounts
{
	std::size_t buildingShapes = 0, maskShapes = 0, foliage = 0, features = 0, doorsSkipped = 0;
};

// The original's zone height grid over the world box: fullmap.tif every `stride` samples (2: 36 u), the samples covering [x0, x1] x [z0, z1].
Grid makeGrid(const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1, int stride);

// The cutters over the world box: every gathered shape of the zones it touches (buildings' collision shapes but their doors, the buildings'
// interior-mask shapes, foliage, map features' collision), biggest xz cover first.
std::vector<Cutter> gatherCutters(const std::string& dataDir, const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1,
	CutterCounts* counts = nullptr);

// The same from an area already gathered (the runtime bake's: it holds the placements the data files do not).
std::vector<Cutter> cuttersOfArea(const kworld::AreaGeometry& area, double x0, double z0, double x1, double z1, CutterCounts* counts = nullptr);

// The whole job over a world box: grid, a 3 x 3 seed grid (the original's per-zone probes) plus `extraSeeds`, cutters (`cut`), generate.
// `msGather` receives the cutter gather's wall time. `extra`: cutters added after the gathered ones (zoneSideCutter).
native::Mesh generateBox(const std::string& dataDir, const kworld::Heightmap& terrain, double x0, double z0, double x1, double z1, const Params& params,
	const std::vector<std::array<double, 2>>& extraSeeds, bool cut, Stats* stats = nullptr, CutterCounts* counts = nullptr, double* msGather = nullptr,
	const std::vector<Cutter>* extra = nullptr);

} // namespace knav::ground
