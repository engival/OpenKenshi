#pragma once

// PhysX collision files and placed collision geometry
// (docs/formats/COLLISION_FORMAT.md). Port of tools/collision.py and
// tools/building_geometry.py. Standard library only.

#include "kworld/foliage.hpp"
#include "kworld/layouts.hpp"
#include "kworld/zones.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
struct Record;
} // namespace kdata

namespace kworld {

// One shape of a collision file, actor and shape poses applied, in Ogre y-up
// axes (the z-up fold (x, y, z) -> (x, z, -y) is done), before placement.
struct CollisionShape
{
	// Boxes, capsules, spheres and convex hulls: a closed solid. Triangle
	// meshes are surfaces (their winding is whatever the cooker wrote).
	bool closed = false;
	std::vector<float> vertices; // xyz triples
	std::vector<std::uint32_t> indices; // triangle triples, outward = CCW
	// The shape's frame, folded: R = actor globalPose * shape localPose; axes holds R's columns
	// (the shape's local x, y, z) at unit length. actor is the actor's origin. half: a box's half
	// extents along those axes (0 for other kinds).
	std::array<float, 9> axes{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	std::array<float, 3> actor{};
	std::array<float, 3> half{};
};

struct CollisionModel
{
	std::vector<CollisionShape> shapes;
	std::size_t planes = 0; // infinite ground planes: skipped
};

// Parses an NXUSTREAM2 XML collision file. Throws Error when the text is
// not a collision collection or a cooked blob does not decode.
CollisionModel parseCollisionXml(std::string_view text);

// BUILDING `path mode` (fcs.def: IGNORE, PROJECTED, OBSTACLE, WALKABLE; the
// integer order is that of the description, confirmed by the data: 0 holds
// signs and tents, 1 small tables and barrels, 2 doors and ovens, 3 houses,
// walls, ramps).
enum class PathMode
{
	Ignore = 0,
	Projected = 1,
	Obstacle = 2,
	Walkable = 3,
};

// How a placed shape enters the navmesh.
enum class NavUse
{
	Walkable, // rasterised walkable; closed shapes also fill their interior
	Obstacle, // solid, not standable on top
	Projected, // 2D footprint cut through all heights, not rasterised
	Door, // a doorway: never geometry, its footprint marks the passage (knav/door.hpp)
};

// A door's identity across the bake, the cache and the runtime: FNV-1a 64 of its id string.
using DoorKey = std::uint64_t;
DoorKey doorKey(std::string_view id) noexcept;
// "cellX,cellZ/instanceID" of a placement; a door is that plus "#n", n the index in the
// building's `doors` reference list.
std::string placementId(const PlacedInstance& placed);
std::string doorId(const PlacedInstance& placed, std::size_t index);
// A placement spawned as a ruin: its squad template's SQUAD `building ruined` (the Hub's 21). A ruin has no doors; its doorways
// stay (kinspect: building setup spawns the `doors` refs only while not destroyed). UNSURE: the exe's extra vf 0x348 gate.
bool placementRuined(const kdata::GameData& gameData, const PlacedInstance& placed);

// One shape placed in the world.
struct WorldShape
{
	NavUse use = NavUse::Walkable;
	DoorKey door = 0; // NavUse::Door only; 0 = a doorway with no door (a ruin's)
	bool closed = false;
	// A convex solid whose interior the bake fills by column (bake.cpp "volume fill") instead of
	// by footprint: the shapes `closed` leaves open because they must keep a sloped top (stairs,
	// gateway bodies). The ground inside them is not walkable; their top surface is. On an open
	// foliage obstacle shell (a rock) it means: cut the xz cover of its triangles (knav bake.cpp rockShell).
	bool solid = false;
	bool foliage = false; // a generated foliage instance (a rock, a tree), not a placed building
	std::vector<float> vertices;
	std::vector<std::uint32_t> indices;
	std::array<float, 3> min{};
	std::array<float, 3> max{};
	// CollisionShape's frame after placement (axes rotated, actor placed, half scaled); the door
	// pick moves a leaf by it (kinspect door_pick.md).
	std::array<float, 9> axes{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	std::array<float, 3> actor{};
	std::array<float, 3> half{};
	// Walkable only inside this convex region (planes nx, ny, nz, d; inside: n . p <= d), cut outside: the upper floors and
	// stairs of a building with an interior mask (the original bakes them only in the building's interior mesh, carved to
	// its mask hull; kinspect findings/navgroups_static.md, navinterior_cut.md). Null: no clip.
	std::shared_ptr<const std::vector<std::array<float, 4>>> keepInside;
	// A shape of an upper-floor (`building floor` >= 1) or stairs part, whether or not the mask clip (keepInside) could be built: a
	// mask with no box (Outpost s-IV's) has no planes, so keepInside stays null there. ksim's BuildingSite floors read it (kenshi-k16.6.277).
	bool upperPart = false;
	// The building's interior volume, on every shape of a walkable building with an interior mask whose BUILDING `interior terrain` is 0: one convex
	// plane set per mask box, the volume being their union (a Longhouse's mask is three boxes of different yaw and height). The bake does not rasterise
	// the terrain inside it, so the interior floor stands alone (kenshi-rwh.90: "building interior includes terrain for generating navmesh", default
	// off; a sunk house is walkable under the dune). Null: the terrain stays.
	std::shared_ptr<const std::vector<std::vector<std::array<float, 4>>>> carveTerrain;
	// The world positions of this shape's building's use nodes (`is node`, `node type` 4: where a user stands at a bed, a counter, a seat). The
	// bake spares the spans within reach of them from a prop's body clearance (knav bake.cpp), so a furniture piece stays usable. Null: none.
	std::shared_ptr<const std::vector<std::array<float, 3>>> useNodes;
	// A separately placed furnishing (barrel, counter, bed, crate, table, rack: a layout item, or a building record with `is interior furniture`), never a building's shell (walls, floors, stairs). Only these get the bake's hard
	// body clearance (kenshi-k16.7.89).
	bool furniture = false;
};

// The BUILDING fields that decide how a placement affects the navmesh.
struct BuildingNav
{
	PathMode pathMode = PathMode::Obstacle;
	float buildThreshold = 0.0f; // percent built before walls enter the navmesh
	bool destroyedNavmesh = false; // a destroyed building leaves the navmesh
	// Spawned as a ruin (placementRuined): no doors, but the doorways stay open: their shapes are still NavUse::Door,
	// with door key 0 and not in `doors`.
	bool ruined = false;
	bool gateway = false; // walk under it on the terrain: its solids do not fill
	bool enforceCeiling = false; // the exe's building+0xc3: indoors on every floor but the ground, ray or no ray (indoors_state.md section 1)
	// The exe's building+0xc2: a cave, a lab (Ancient Lab, Metal Warehouse, Ash Dome). The weapon `indoors mod` is skipped inside one
	// (indoors_state.md section 2); it does not change the indoors state itself.
	bool cavernous = false;
	// The BUILDING record's `is shelter`: its roof stops acid rain for someone under it (weather_acid_rain.md section 5).
	bool shelter = false;
	bool interiorTerrain = false; // BUILDING `interior terrain`: the terrain stays inside the interior volume (WorldShape::carveTerrain)
	std::vector<DoorKey> doors; // the doors of this placement; their shapes are NavUse::Door
};

// What building and foliage lookup found, for the coverage report.
struct CollisionCoverage
{
	std::size_t placements = 0;
	std::size_t placementsWithCollision = 0; // at least one collision part
	std::size_t placementsRenderOnly = 0; // parts, none with collision, some render mesh
	std::size_t placementsNoGeometry = 0; // no parts, or nothing but empty parts
	std::size_t placementsMissingTarget = 0;
	std::size_t parts = 0; // parts the placements selected
	std::size_t partsWithCollision = 0;
	std::size_t partsRenderOnly = 0; // skipped: render-mesh fallback not done
	std::size_t partsNoGeometry = 0;
	std::size_t partsFlagged = 0; // `passable` or `no collision`
	std::size_t partsRuinRemoved = 0; // a ruin's upper-floor and interior-stair parts with no `destroyed mesh`
	std::size_t partsFileUnreadable = 0; // named collision file missing or bad
	std::array<std::size_t, 4> pathModes{}; // placements by PathMode
	std::size_t gatewayPlacements = 0;
	std::size_t doors = 0; // door children placed
	std::size_t doorsWithoutCollision = 0; // door children with no collision file: no doorway
	std::size_t foliageInstances = 0;
	std::size_t foliageWithCollision = 0;
	std::size_t foliageWalkable = 0; // FOLIAGE_MESH `walkable` collisions
	std::size_t foliageCutters = 0; // `navmesh cutter` discs (no collision)
	std::size_t furniture = 0; // interior layout pieces placed
	std::size_t furnitureWithCollision = 0; // ... that added shapes
};

// Resolves record file names (case-insensitively, under dataDir), caches
// parsed collision files and places buildings and foliage in the world.
class CollisionLibrary
{
public:
	CollisionLibrary(std::string_view dataDir, const kdata::GameData& gameData);

	// Appends the collision shapes of one building placement (terrain-relative
	// y already resolved in `placed`). Part selection follows
	// tools/building_geometry.py select_parts, with a stable hash of the
	// instance id standing in for the game's random roll.
	// Returns the building's navmesh fields, nullopt for a missing target.
	// PathMode IGNORE appends nothing.
	// `interiors` also adds the collision of the `interior` parts (floors, staircases) of a WALKABLE
	// building; on by default (interiors are seamless world space, docs/formats/NAVMESH_CACHE.md
	// "Positions on several floors"). Pass false only to compare against the old exterior-only bake.
	// `furniture` (or the record's `is interior furniture`): PROJECTED parts go in as OBSTACLE, which stays within the piece's
	// own height band; a projected cut runs through every floor, so a table on floor 1 would cut floor 0.
	std::optional<BuildingNav> addBuilding(const PlacedInstance& placed,
		std::vector<WorldShape>& out, CollisionCoverage& coverage, bool interiors = true, bool furniture = false);

	// Appends the collision of a placement's interior layout furniture (layouts.hpp): each item placed in the building's
	// frame and added as furniture. Counts go to coverage.furniture*, not the placement counts. Returns the pieces placed.
	std::size_t addFurniture(const PlacedInstance& building, const InteriorLayout& layout,
		std::vector<WorldShape>& out, CollisionCoverage& coverage);

	// Appends the collision of the building's `interior mask` parts: the indoors trigger hull (INTERIORS.md, the exe's "am I
	// indoors" test; a box in the Storm House). Nothing and false when the building has no mask or its file is unreadable.
	bool addInteriorMask(const PlacedInstance& placed, std::vector<WorldShape>& out);

	// Appends the collision of one foliage instance, if its FOLIAGE_MESH
	// names a collision file (walkable only if `walkable`), else a disc of
	// `navmesh cutter` radius times the instance scale. Returns whether it added shapes.
	bool addFoliage(const FoliageInstance& instance, std::vector<WorldShape>& out,
		CollisionCoverage& coverage);

private:
	// nullptr when the record names no file, the file is absent or bad.
	const CollisionModel* model(const std::string& recordPath, bool& unreadable);
	bool partHasMesh(const kdata::Record& part) const;

	const kdata::GameData& gameData_;
	std::unordered_map<std::string, std::string> files_; // lower-case relative path -> path
	std::map<std::string, std::unique_ptr<CollisionModel>> models_; // null = unreadable
};

// World placement of one model: pos + R(qw, qx, qy, qz) * (scale * v).
void placeModel(const CollisionModel& model, const std::array<double, 3>& pos,
	const std::array<double, 4>& quatWxyz, double scale, NavUse use, bool fill,
	std::vector<WorldShape>& out);

} // namespace kworld
