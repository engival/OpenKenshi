#pragma once

// Geometric navmesh generator for the surfaces of collision shapes: the original's interior job (docs/formats/NAVMESH_GENERATOR.md,
// stage 1, kenshi-k16.7.131). A building's shapes (shell, stairs, furniture) go in, a knav::native::Mesh of convex polygons with exact
// corners comes out: no voxels, no cell size. Offline form: no Detour, no tile clipping, no runtime use yet.
//
// Rules (the design note's section 0 and sections 3-6):
//  - walkable: an up-facing triangle of a walkable shape, at most `slopeDegrees` steep;
//  - cut: a solid above the face by (cutBase, characterHeight] cuts it by exact 2D clipping in the face's plane; a PROJECTED shape cuts at
//    every height; where two faces of different shapes lie within `ownerTolerance` of each other the lower shape index owns the overlap;
//  - keep: inside the interior-mask hull (everything when there is no mask);
//  - merge: convex polygons across shared edges, then edge matching (split at the overlap ends, nearest first), then pruning to the seed.

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
	double ownerTolerance = 0.1; // coplanar within this: the lower shape index owns the overlap
	double planarTolerance = 0.25; // two pieces merge when each lies within this of the other's plane
	double minWidth = 0.05; // a triangle narrower than this is degenerate (Havok degenerateWidthThreshold)
	double barrierHalfWidth = 0.05; // a sheet that cuts a line is widened to this
	double matchSeparation = 0.5; // edges closer than this (xz) match first; `matchSeparationMax` is the upper bound
	double matchSeparationMax = 2.0; // Havok maxSeparation
	double matchStep = 10.0; // Havok maxStepHeight
	double matchOverlap = 0.2; // edgeTraversibilityHorizontalEpsilon
	double islandDistance = 4.0; // a component farther than this from the seed's component is removed
	double minPieceArea = 0.01; // pieces under this xz area are dropped
	double collinearTolerance = 0.05; // a corner bending the wrong way by less than this (snap noise) is straight
	double lattice = 1.0 / 256.0; // output corners snap to this in xz
	bool pruneZoneIslands = false; // zone side: drop the components that do not reach the seam. Off: the rule is "joins terrain" (design note section 0, F5), and terrain arrives with stage 3
	double seamTolerance = 0.05; // an edge whose ends are this close to a hull side plane (|n.y| < 0.5) lies on the seam
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
	// The pruning seed; none: the largest component stays (a building has one interior: the seed child +0xf0 is not known to us yet).
	std::optional<std::array<double, 3>> seed;
	Side side = Side::Interior;
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
	double overlapArea = 0.0; // xz overlap of two faces within ownerTolerance in height (the invariant: 0)
	std::vector<double> componentAreas; // xz area per component before pruning
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
