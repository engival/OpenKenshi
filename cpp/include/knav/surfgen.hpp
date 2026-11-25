#pragma once

// Geometric navmesh generator for the surfaces of collision shapes: the original's interior job (docs/formats/NAVMESH_GENERATOR.md,
// stage 1, kenshi-k16.7.131). A building's shapes (shell, stairs, furniture) go in, a knav::native::Mesh of convex polygons with exact
// corners comes out: no voxels, no cell size. Offline form: no Detour, no tile clipping, no runtime use yet.
//
// Rules (the design note's section 0 and sections 3-6; the rewrite on exact integer regions: kenshi-ffmi.4, helper docs/SURFGEN_REWRITE.md):
//  - walkable: an up-facing triangle of a walkable shape, at most `slopeDegrees` steep; pieces within `ownerTolerance` of one plane form a surface;
//  - cut: a solid above the surface by (cutBase, characterHeight] cuts it (exact integer booleans, Clipper2, 1 / `lattice` per unit); a PROJECTED
//    shape cuts at every height; where two surfaces cross within `ownerTolerance` the lower index owns the strip; cracks and slivers under 2 `weld`
//    are welded away;
//  - links: region edges facing each other across a gap and a step a character can take, nothing solid between; an edge with no link is a wall;
//  - narrow: floor no disc of `minPassage` reaches without touching a wall goes (an opening: centres eroded by the walls, grown back mitred);
//  - sides: the interior-mask hull splits each surface exactly (interior / zone side); door volumes paint their part;
//  - faces: constrained Delaunay triangles merged to convex faces; links between faces only where the regions linked; one graph over both sides and
//    the seam, pruned to the seed's component; the seam is a link between an interior face and a zone face that both stay.

#include "knav/native.hpp"
#include "kworld/collision.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace knav::surfgen
{

struct Settings
{
	double slopeDegrees = 60.0; // the building slope limit
	double characterHeight = 18.0; // a solid this close above a face cuts it
	double cutBase = 0.1; // ... but only above this rise (below it two surfaces are coplanar)
	double ownerTolerance = 0.1; // pieces this close to one plane form one surface; where two surfaces cross this close the lower index owns the strip
	double minWidth = 0.05; // a source triangle narrower than this is degenerate (Havok degenerateWidthThreshold)
	double barrierHalfWidth = 0.05; // a sheet that cuts a line is widened to this
	double weld = 0.02; // cracks and slivers narrower than twice this are closed / removed (float noise between shapes, not a gap anyone sees)
	double matchSeparationMax = 2.0; // Havok maxSeparation: two edges this far apart (xz) still link
	double matchStep = 5.0; // the unit rule (NAVMESH_GENERATOR.md section 6; section 0 says 10 u, rwh.289). 10 linked Heft's roof to its ramparts (8.8-9.5 u up); its real steps are <= 3 u (kenshi-ffmi.3)
	double matchOverlap = 0.2; // edgeTraversibilityHorizontalEpsilon
	double minPieceArea = 0.01; // a door paints a piece of at least this xz area
	double lattice = 1.0 / 256.0; // the integer grid of every corner in xz
	// Narrow cut (kenshi-ffmi.3 / .4): floor that no disc this wide reaches without touching a wall is removed, and what it cut off is then an
	// island. Havok's minCharacterWidth (BLOCK_EDGES: no agent passes a gap under 9 u); the original keeps the faces and blocks at query time,
	// ours does not keep them (user 2026-11-24: "keeping that data around benefits nobody"). 0: off.
	double minPassage = 9.0;
	// The opening grows the centres back with mitred corners: a corner whose mitre is at most this many radii long (2: corners of 60 degrees
	// and wider) comes back whole, a sharper tip is squared off at that length (the dagger between a back wall and furniture, user 2026-11-24).
	double cornerMitre = 2.0;
	// An island (faces joined edge to edge, before any step or gap) smaller than this in xz goes before the flood, keepIslands or not: specks a
	// split leaves behind (user 2026-11-24). 0: off.
	double minIslandArea = 1.0;
	bool keepIslands = false; // debug: every component stays on both sides (no pruning to the seed), so a viewer can show what would go
};

// Which side of the interior-mask hull the output keeps: the building's interior mesh, or its zone-side faces (walkable groups outside the hull; the
// seam lies on the hull's side planes, equal on both sides). Zone with no mask: nothing (the whole building is interior).
enum class Side
{
	Interior,
	Zone,
};

struct Input
{
	std::vector<const kworld::WorldShape*> shapes; // the building's shell, stairs and furniture (use Door shapes are ignored)
	std::vector<const kworld::WorldShape*> mask; // the interior-mask boxes / hulls (CollisionLibrary::addInteriorMask); empty: keep everything
	// An explicit pruning seed (a point on the floor, in the shapes' frame), overriding the lowest walkable piece: what the faces reach from it over
	// links, either side, is the walkable area. For tests and debugging.
	std::optional<std::array<double, 3>> seed;
	Side side = Side::Interior;
	// The building's pose: world = origin + rotation (w, x, y, z) * local. Set: `shapes` and `mask` are in the building's own frame
	// (kworld::AreaBuilding::localShapes / localMask), the seed is in the world, and the mesh comes out in the world. Bake buildings in their own
	// frame: there the walls lie on the lattice's axes, as in the lab. In world axes the pieces either side of the mask plane or a door volume (one
	// surface) miss each other's corners on the lattice and do not join, and a slope's edge lands off the floor's: the Hub bar, yawed -46 degrees,
	// lost its seam and its interior. Unset: the shapes' frame is the output's.
	struct Pose
	{
		std::array<double, 3> origin{};
		std::array<double, 4> rotation{ 1.0, 0.0, 0.0, 0.0 };
	};
	std::optional<Pose> pose;

	// A placement's pose (CollisionLibrary places the building's shapes by it).
	static Pose poseOf(const kworld::PlacedInstance& p) { return { { p.x, p.y, p.z }, { p.qw, p.qx, p.qy, p.qz } }; }
	// Door painting (both sides): a piece inside the door's volume (the hull of its shape's points, 0 u margin) is cut out of its surroundings and carries
	// the door's key: face data 5 closed / 4 open, `Face::door` = WorldShape::door. The Door shapes of `shapes` are used for this.
	bool paintDoors = true;
};

struct Report
{
	std::size_t walkableShapes = 0;
	std::size_t pieces = 0; // after cutting, before the merge
	std::size_t faces = 0;
	std::size_t matchedEdges = 0; // pairs
	std::size_t boundaryEdges = 0;
	std::size_t components = 0; // before pruning
	std::size_t droppedFaces = 0; // pruned islands
	double droppedArea = 0.0; // xz
	double area = 0.0; // xz area of the faces
	std::size_t seamEdges = 0; // boundary edges on a hull side plane (marked `linked`)
	double seamLength = 0.0; // their total length (3D)
	std::size_t doorFaces = 0;
	double doorArea = 0.0; // xz area painted
	double steepFaces = 0; // faces steeper than 48 degrees
	double overlapArea = 0.0; // xz overlap of two faces within characterHeight in height (the invariant: 0)
	std::size_t invalidFaces = 0; // faces that break the invariants: not convex, a corner twice, an edge under a grid step (the invariant: 0)
	std::size_t triangulationFailures = 0; // pieces the triangulation refused (their floor is missing; the invariant: 0)
	std::size_t shortLinks = 0; // region links dropped: their run between two surfaces is shorter than minPassage
	std::size_t narrowOutlines = 0; // removed polygons Clipper could not triangulate: in narrowQuads as their outline (viewers fan them)
	std::size_t narrowCuts = 0; // polygons the narrow cut removed
	double narrowArea = 0.0; // their xz area
	std::vector<std::vector<std::array<double, 3>>> narrowQuads; // the removed floor as triangles (x, y, z corners), for viewers
	struct Stage
	{
		const char* name;
		double ms;
	};
	std::vector<Stage> stages; // wall time per stage of the last build
	std::vector<std::size_t> seedFaces; // output faces (this side) the flood started from: the lowest walkable piece, or the seed's face
	std::vector<double> componentAreas; // xz area per component before pruning
	std::size_t tinyIslands = 0; // islands under minIslandArea, both sides (dropped before the flood)
	double tinyIslandArea = 0.0; // their xz area
	double reachedShare = 1.0; // xz area of the seeded components over all of the floor, both sides, before pruning
	bool seedSuspect = false; // a warning: a component the seed did not reach is larger than every one it did
	std::vector<std::size_t> faceShape; // per face: the index in Input::shapes of the shape that supplied its plane
};

// The generated interior surfaces of one building, world coordinates, faces convex in xz. Deterministic for one input.
native::Mesh generate(const Input& input, const Settings& settings = {}, Report* report = nullptr);

// How far two meshes' seams (their `linked` edges) differ: points are sampled every `step` u along a linked edge of one mesh and matched to the nearest
// linked edge of the other in the plan, both ways. xz = the largest plan distance, y = the largest height difference at the matched points (two surfaces
// meeting at a seam may differ in height: a step). A mesh with no seam against one with: xz is infinity; two meshes with none: 0.
struct SeamGap
{
	double xz = 0.0;
	double y = 0.0;
};
SeamGap seamMismatch(const native::Mesh& a, const native::Mesh& b, double step = 0.25);

} // namespace knav::surfgen
