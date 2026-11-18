#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// A frame's most fixed 1/60 s steps: the player speeds are 1x, 2x and 5x (5 steps); past the cap the ragdoll clock lags the sim rather than
// taking one long step (the fast-forward speeds are for testing, and one 0.25 s step at 16x pumped the joints and flew the body off).
inline constexpr int kRagdollMaxSteps = 8;

namespace ragdoll { struct Def; }

// Client physics for ragdolls and carried bodies: Jolt v5.6.0 (third_party/JoltPhysics, MIT), client only (docs/reference/PHYSICS.md 2 and 4,
// docs/CLIENT_GOALS.md "Ragdolls and carried bodies"). Display only: the sim owns where a body rests, this owns how it looks. One PhysicsSystem
// with its own job system of at most 2 threads, started by the first body (a client without ragdolls never pays for it).
class PhysicsWorld
{
public:
	struct Body
	{
		double p[3] = {};	// the body's origin (a box's centre), world
		double v[3] = {};	// linear velocity, world
		float q[4] = { 1, 0, 0, 0 };	// rotation (w, x, y, z), world
		double w[3] = {};	// angular velocity, world (rad/s)
		bool awake = false;
	};

	PhysicsWorld();
	~PhysicsWorld();
	PhysicsWorld(const PhysicsWorld&) = delete;
	PhysicsWorld& operator=(const PhysicsWorld&) = delete;

	bool started() const { return impl != nullptr; }
	// Bodies from world-space geometry. Static bodies collide with the dynamic ones only; ids are 1-based (0 = failed). The world's units are the
	// game's (a human is 18 u: knav::NavConfig::agentHeight), so gravity is in units (physics.cpp).
	uint32_t addStaticBox(const double centre[3], const float half[3]);
	uint32_t addBox(const double centre[3], const float half[3]);
	uint32_t addSphere(const double centre[3], float radius);
	// Static world geometry (P1): a triangle mesh or a convex hull (xyz triples, world space; the mesh vertices are recentred around the body
	// so the shape holds small coordinates). remove() drops one again (the lazy collider set).
	uint32_t addMesh(const float* vertices, size_t vertexCount, const uint32_t* indices, size_t indexCount);
	uint32_t addConvex(const float* vertices, size_t vertexCount);
	bool remove(uint32_t id);
	bool body(uint32_t id, Body& out) const;
	// The dynamic bodies (the live ragdolls today): the collider set is posted around them.
	std::vector<uint32_t> dynamicBodies() const;
	// A ragdoll from a .phs def (ragdoll.hpp), started at its bind pose moved to pos and turned yaw: one Jolt body per def body and the
	// def's joints as hinge / swing-twist / fixed constraints against the bone names' skeleton. Returns an id (1-based), 0 when the def
	// cannot be built (a body without a shape, a joint cycle; the reason is logged). Ragdoll bodies are in the body layer: they meet the
	// static world and each other, and never an upright character (which has no Jolt body; PHYSICS.md 6 disables its groups 0/1 against
	// the ragdolls' 3/4).
	uint32_t addRagdoll(const ragdoll::Def& def, const double pos[3], float yaw);
	std::vector<uint32_t> ragdollBodies(uint32_t id) const;	// the body ids in def order (empty: no such ragdoll)
	// A ragdoll body's world transform: position and rotation (w, x, y, z; the body frame is its bone's model frame, ragdoll.hpp).
	struct Pose
	{
		double p[3] = {};
		float q[4] = { 1, 0, 0, 0 };
	};
	bool ragdollPose(uint32_t id, std::vector<Pose>& out) const;	// def order
	// Moves every body to the given pose (def order) at rest and wakes it: a ragdoll starts from the animated pose (PHYSICS.md 6).
	bool setRagdollPose(uint32_t id, const std::vector<Pose>& in);
	bool kick(uint32_t bodyId, const float dv[3]);	// adds a velocity change to one body (the event's impulse at its part)
	bool kick(uint32_t bodyId, const float dv[3], const float dw[3]);	// the same with an angular velocity change (rad/s): the carry servo
	// A carried animal's anchor body: kinematic, moved to the pose over dt so the joints see its velocity (no teleport); unpinBody makes it dynamic again.
	bool pinBody(uint32_t bodyId, const double p[3], const float q[4], double dt);
	bool unpinBody(uint32_t bodyId);
	bool ragdollAwake(uint32_t id) const;	// any of its bodies still moving
	void sleepRagdoll(uint32_t id);	// deactivates every body: the rest-point pull's end, the body frozen where it lies
	// The rest-point pull drags a ragdoll over the ground: the friction of all its bodies (contacts still hold it up). < 0: the scene default.
	void setRagdollFriction(uint32_t id, float friction);
	bool removeRagdoll(uint32_t id);
	size_t ragdolls() const;
	// A frame at the game speed (PHYSICS.md 4): whole 1/60 s substeps out of an accumulator, at most maxSteps of them (past the cap the
	// ragdoll clock lags the sim, the excess is dropped). A frame shorter than one step runs none and carries the time over, so the physics
	// keeps the game's clock at any frame rate. `before` runs ahead of each substep - the rest-point pull kicks the root there, so its servo
	// sees one step at a time whatever the game speed. Logs once when the cap bites.
	void stepFixed(double dt, int maxSteps, const std::function<void(double)>& before);
	std::string report() const;	// precision and unit measurement, layers, gravity, bodies, steps (script `dump physics`)
private:
	struct Impl;
	std::unique_ptr<Impl> impl;	// created by the first added body
};

namespace kworld { struct WorldShape; class Heightmap; }

// The static world the ragdolls land on (P1): the click pick's building shapes (world space) as convex hulls (closed, few points) or triangle
// meshes, and terrain height-field patches, posted only around live bodies and dropped when none is left near them. A new gather re-posts the
// building shapes (the pick rebuilds them when the camera enters another zone cell); terrain patches are keyed by their patch cell.
class StaticColliders
{
public:
	explicit StaticColliders(PhysicsWorld& world);
	// bodies: the live bodies (PhysicsWorld::dynamicBodies); starts: points where a ragdoll waits to start (Ragdolls::waitingAt), posted
	// for like a body; radius: how far round a body a shape is kept (u). Synced on the first call, when the body set, the start points or the
	// gather change (a ragdoll that starts must land on a floor that is already there) and every 30 calls (about 0.5 s).
	void update(const std::shared_ptr<const std::vector<kworld::WorldShape>>& shapes, const kworld::Heightmap& terrain, const std::vector<uint32_t>& bodies,
		const std::vector<std::array<double, 3>>& starts, double radius);
	size_t posted() const { return postedShapes_.size() + patches_.size(); }
	// True when a posted building shape is within radius of the point: the gather follows the camera's zone cell, so a body in another cell
	// has terrain only under it (a ragdoll must not start there; P3 refuses to).
	bool covered(const double point[3], double radius) const;
private:
	PhysicsWorld& world_;
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes_;
	std::unordered_map<size_t, uint32_t> postedShapes_;	// index in shapes_ -> body id
	std::unordered_map<uint64_t, uint32_t> patches_;	// (px, pz) -> body id
	std::vector<uint32_t> bodies_;	// the body ids of the last sync: a changed set syncs this frame
	std::vector<std::array<double, 3>> starts_;	// the start points of the last sync, likewise
	int calls = 0;
};
