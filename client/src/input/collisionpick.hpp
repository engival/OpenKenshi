#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "kworld/collision.hpp"

namespace kworld { class Heightmap; }

// The click ray's collision layer (docs/CLIENT.md "Live sim link", SIMLINK 13): the building and foliage collision shapes the navmesh bakes from,
// gathered by kworld::AreaGatherer for the zone cells round the camera on a worker thread (sokol_gfx has no depth readback). Doorway shapes are kept
// apart: a world click passes through them (it is a click on what is behind), the door picker rays only them. update() each frame; ray() any time,
// against the newest finished gather.
class CollisionPick
{
public:
	~CollisionPick();
	void start(const std::string& dataDir, const kworld::Heightmap* terrain);
	void update(double x, double z);	// asks for a new gather when the camera enters another zone cell
	// Nearest shape hit along the ray (origin, unit dir) closer than maxT; t is its distance. False: none, or nothing gathered yet.
	// bounds: the hit shape's world box (min xyz, max xyz). door: only the door leaf shapes (NavUse::Door) instead of every other shape, each moved
	// by the offset door() gives it (false: leave it out); key: the hit leaf's door; solid: the hit shape is a WorldShape::solid (stairs, gateway bodies);
	// building: the hit shape is a building's, not foliage (a rock, a tree). hitName: where it came from (the placement id, its BUILDING record and
	// the shape's index in the placement; foliage: the instance's index), for dump feetfloor (kenshi-k16.17.7.24.4).
	using DoorOffset = std::function<bool(const kworld::WorldShape&, double offset[3])>;
	bool ray(const double origin[3], const double dir[3], double maxT, double& t, float bounds[6] = nullptr, const DoorOffset& door = {}, kworld::DoorKey* key = nullptr,
		bool* solid = nullptr, bool* building = nullptr, std::string* hitName = nullptr) const;
	// Every building collision triangle the vertical line through (x, z) crosses with y in [lo, hi] (the height probe, dev/heightprobe.cpp): the retired stand ray's
	// geometry without its filters, each hit flagged by the filters the retired stand ray applied (retired/stand_ray_collision_groundy.cpp) (door: a NavUse::Door leaf, skipped; thin: a thin() prop, skipped).
	// Returns 1, 0 when there is none, -1 when the collision under (x, z) is not gathered. y ascending is not promised.
	struct ColumnHit { double y; bool door, thin; std::string name; };
	int column(double x, double z, double lo, double hi, std::vector<ColumnHit>& out) const;
	// Display depenetration (kenshi-k16.2.345): the horizontal push that takes a body of the given radius at (x, z), feet at y, out of the building
	// collision (not foliage, not door leaves, not thin props, not floors): out = the (dx, dz) to add, capped at maxPush. Only the wall-like faces
	// (|normal y| < 0.7) between y + 0.35 and y + 1.5 count; the face's outward side is away from its shape's box centre. False: nothing to push from
	// or the collision under (x, z) is not gathered.
	bool depenetrate(double x, double z, double y, double radius, double maxPush, double out[2], std::string* why = nullptr) const;
	// The direct-drive camera boom (kenshi-k16.2.289.1): a sphere of the given radius swept from origin along dir (unit); the first contact within maxT
	// (t: the centre's distance travelled). Building shapes that thin() calls thin are passed (the occluder fade dithers those); foliage shapes and
	// everything else block. A shape the sphere already overlaps at the start blocks only when the sweep moves into it. False: nothing in the way.
	bool sweep(const double origin[3], const double dir[3], double maxT, double radius, double& t, float bounds[6] = nullptr, std::string* hitName = nullptr) const;
	// A thin prop by its world box (sorted extents d0 <= d1 <= d2): a bar (d1 < 10: pole, rail, frame member) or a panel (d0 < 3 and d2 < 60: a sign
	// board). The camera passes these and the occluder fade dithers them away; everything bigger is solid.
	static bool thin(const float bounds[6]);
	size_t shapes() const;
	// A listing of the gathered shapes whose box reaches within radius of (x, z): thin / solid / foliage, name, box (dump shapes; kenshi-k16.2.289.2).
	std::string report(double x, double z, double radius, bool thinOnly) const;
	// The newest finished gather (buildings then foliage, world space) for the static colliders; null before the first one. A new gather is a new
	// vector, so its address says the set changed.
	std::shared_ptr<const std::vector<kworld::WorldShape>> gathered() const;
	void wait() const;	// scripted runs: until the asked-for cell is gathered (or loading failed)
	bool failed() const;

private:
	struct NudgeCell;
	void worker(std::string dataDir, const kworld::Heightmap* terrain);
	std::thread thread_;
	mutable std::mutex m_;
	std::condition_variable cv_;
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes_;
	std::shared_ptr<const std::vector<std::string>> names_;	// shapes_ order: the placement id, record and shape index of each (dump feetfloor)
	size_t buildingShapes_ = 0;	// shapes_ holds the buildings' shapes first, then foliage
	int want_[2] = { -1, -1 }, have_[2] = { -1, -1 }, done_[2] = { -1, -1 };	// zone cell asked for / being gathered / gathered
	bool stop_ = false, failed_ = false;
	// depenetrate()'s per-cell candidate cache (kenshi-k16.2.350): the shapes and wall faces near a 8 u grid cell, built once per cell and gather (a new gather
	// is a new shapes_ vector, so its address drops the cache)
	mutable std::mutex nudgeM_;
	mutable const void* nudgeGen_ = nullptr;
	mutable std::unordered_map<int64_t, std::shared_ptr<NudgeCell>> nudgeCells_;
};
