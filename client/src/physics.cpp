#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include "physics.hpp"
#include "ragdoll.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

// Precision (P0 decision, measured): the world spans +-147456 u (kworld::kWorldMin / kWorldExtent, ~29 km across at 0.1 m per unit: a human is
// 18 u, knav::NavConfig::agentHeight). A float holds a coordinate there to std::nextafterf (0.0156 u, printed in report()); double to 2.9e-11 u.
// Jolt's collision tolerance is Jolt's meters-sized default 0.02 m = 0.2 u (MINE: 1 u = 0.1 m), so float spends up to 8% of it on position
// quantisation alone, and it grows towards the edges from the Hub's 0.0039 u. JPH_DOUBLE_PRECISION (a single CMake flag, positions only, a few
// hundred bodies) removes that. A floating origin would too, but every static shape is already in world space (kworld collision, the heightmap) and
// would have to be re-posted whenever the origin moved; double precision needs no bookkeeping. Decided with the user's price in mind: the client
// stores all positions as doubles already.
static_assert(sizeof(JPH::RVec3) > sizeof(JPH::Vec3), "JPH_DOUBLE_PRECISION must be on (CMakeLists.txt)");

namespace
{
// Object layers: the static world (terrain patches, building hulls) and the moving bodies (ragdolls now, carried bodies later). They sit in their
// own broadphase layers so the static side is never re-broadphased.
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kBodyLayer = 1;
constexpr JPH::ObjectLayer kLayerCount = 2;
constexpr JPH::BroadPhaseLayer kStaticBpLayer(0);
constexpr JPH::BroadPhaseLayer kBodyBpLayer(1);
constexpr JPH::uint kBpLayerCount = 2;

class BpLayerInterface : public JPH::BroadPhaseLayerInterface
{
public:
	JPH::uint GetNumBroadPhaseLayers() const override { return kBpLayerCount; }
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override { return layer == kStaticLayer ? kStaticBpLayer : kBodyBpLayer; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override { return layer == kStaticBpLayer ? "static" : "body"; }
#endif
};

class ObjectVsBpFilter : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
	// Which broadphase layers an object layer may meet: statics never meet the static broadphase layer (their own), bodies meet both.
	bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override { return layer != kStaticLayer || bp != kStaticBpLayer; }
};

class LayerPairFilter : public JPH::ObjectLayerPairFilter
{
public:
	bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override { return a != kStaticLayer || b != kStaticLayer; }	// statics never touch each other
};

// 1 u = 0.1 m (MINE: a human is 18 u tall), so a metre is 10 u: gravity 9.81 m/s^2 is 98.1 u/s^2.
constexpr float kUnitsPerMetre = 10;
constexpr float kGravity = -9.81f * kUnitsPerMetre;
constexpr double kMaxSubstep = 1.0 / 60.0;	// PHYSICS.md 4: substeps of at most 1/60 s
constexpr int kJobThreads = 2;			// at most 2 (the task's budget); the calling thread works too
constexpr JPH::uint kMaxJobs = 2048, kMaxBarriers = 8;
constexpr JPH::uint kMaxBodies = 4096, kMaxBodyPairs = 8192, kMaxContacts = 4096;
constexpr size_t kTempAllocator = 10 * 1024 * 1024;
// The original's PhysX scene values (PHYSICS.md 6), in the world's units (1 u = 0.1 m): the material's dynamic friction 1.0 with the scene's
// 4.0 scaling, restitution 0.05, and 16..32 solver iterations per body. Jolt's default friction (0.2) and restitution (0) are metres-sized game
// values for a different scale.
constexpr float kFriction = 4.0f;
constexpr float kRestitution = 0.05f;
constexpr JPH::uint kMinVelocitySteps = 16, kMaxVelocitySteps = 32;
} // namespace

struct PhysicsWorld::Impl
{
	struct Rec
	{
		JPH::BodyID id;
		bool dynamic, valid = true;
		std::string desc;
		bool ragdoll = false;	// owned by a Ragdoll (removed with it, not one by one)
	};
	// A ragdoll instance: Jolt's Ragdoll owns its bodies and constraints once added to the system.
	struct Rag
	{
		JPH::Ref<JPH::Ragdoll> ragdoll;
		std::vector<uint32_t> bodies;	// our ids in def order (Ragdoll::GetBodyIDs is in skeleton order)
	};

	// Jolt's allocators must be registered before anything allocates through them, and the temp allocator allocates in its constructor: this member
	// is declared first so it runs first.
	struct Startup
	{
		Startup()
		{
			static std::once_flag once;
			std::call_once(once, [] {
				JPH::RegisterDefaultAllocator();
				JPH::Factory::sInstance = new JPH::Factory();
				JPH::RegisterTypes();
			});
		}
	};
	Startup startup;
	BpLayerInterface bpLayers;
	ObjectVsBpFilter objectVsBp;
	LayerPairFilter pairFilter;
	JPH::TempAllocatorImpl temp { kTempAllocator };
	JPH::JobSystemThreadPool jobs;
	JPH::PhysicsSystem system;
	std::vector<Rec> bodies;	// ids are index + 1
	std::vector<Rag> ragdolls;	// ids are index + 1
	JPH::uint32 nextGroup = 1;	// a collision group per ragdoll: DisableParentChildCollisions needs a unique one per instance
	uint64_t steps = 0;
	double acc = 0;		// the leftover game time not yet stepped (the fixed step's accumulator)
	bool capped = false;	// the step cap logged (once)
	std::string error;

	Impl()
	{
		jobs.Init(kMaxJobs, kMaxBarriers, kJobThreads);
		system.Init(kMaxBodies, 0, kMaxBodyPairs, kMaxContacts, bpLayers, objectVsBp, pairFilter);
		system.SetGravity(JPH::Vec3(0, kGravity, 0));
		// Jolt's defaults are metres: at 1 u = 0.1 m scale its slop and speculative distance by 10 (the original's skin width is 0.25 u), its
		// restitution threshold too, and the sleep threshold is the original's 1.5 u/s linear scene value (the world is in u).
		JPH::PhysicsSettings settings = system.GetPhysicsSettings();
		settings.mPenetrationSlop = 0.2f;
		settings.mSpeculativeContactDistance = 0.2f;
		settings.mPointVelocitySleepThreshold = 1.5f;
		settings.mMinVelocityForRestitution = 10.0f;	// 1 m/s: restitution (0.05) barely registers below it either
		system.SetPhysicsSettings(settings);
	}

	~Impl()
	{
		// Ragdolls first: their destructor destroys the bodies they still own (RemoveFromPhysicsSystem takes them out first).
		for (Rag& r : ragdolls) { if (r.ragdoll) { r.ragdoll->RemoveFromPhysicsSystem(); r.ragdoll = nullptr; } }
		JPH::BodyInterface& bi = system.GetBodyInterface();
		for (const Rec& r : bodies)
		{
			if (!r.valid || r.ragdoll) { continue; }
			bi.RemoveBody(r.id);
			bi.DestroyBody(r.id);
		}
	}

	uint32_t add(JPH::Shape* shape, const double centre[3], bool dynamic, const char* what)
	{
		const JPH::EMotionType type = dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static;
		JPH::BodyCreationSettings settings(shape, JPH::RVec3(centre[0], centre[1], centre[2]), JPH::Quat::sIdentity(), type, dynamic ? kBodyLayer : kStaticLayer);
		settings.mFriction = kFriction;
		settings.mRestitution = kRestitution;
		const JPH::BodyID id = system.GetBodyInterface().CreateAndAddBody(settings, dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
		if (id.IsInvalid()) { return 0; }
		bodies.push_back({ id, dynamic, true, what });
		return (uint32_t)bodies.size();
	}
};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;

uint32_t PhysicsWorld::addStaticBox(const double centre[3], const float half[3])
{
	if (!impl) { impl = std::make_unique<Impl>(); }
	JPH::Ref<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3(half[0], half[1], half[2]));
	return impl->add(shape.GetPtr(), centre, false, "box");
}

uint32_t PhysicsWorld::addBox(const double centre[3], const float half[3])
{
	if (!impl) { impl = std::make_unique<Impl>(); }
	JPH::Ref<JPH::Shape> shape = new JPH::BoxShape(JPH::Vec3(half[0], half[1], half[2]));
	return impl->add(shape.GetPtr(), centre, true, "box");
}

uint32_t PhysicsWorld::addSphere(const double centre[3], float radius)
{
	if (!impl) { impl = std::make_unique<Impl>(); }
	JPH::Ref<JPH::Shape> shape = new JPH::SphereShape(radius);
	return impl->add(shape.GetPtr(), centre, true, "sphere");
}

namespace
{
// The AABB centre of xyz triples (doubles), so a world-space shape is built from small float coordinates and its body sits at the centre.
void centreOf(const float* vertices, size_t count, double centre[3])
{
	double mn[3] = { vertices[0], vertices[1], vertices[2] }, mx[3] = { vertices[0], vertices[1], vertices[2] };
	for (size_t i = 1; i < count; i++)
	{
		for (int k = 0; k < 3; k++)
		{
			mn[k] = std::min(mn[k], (double)vertices[i * 3 + k]);
			mx[k] = std::max(mx[k], (double)vertices[i * 3 + k]);
		}
	}
	for (int k = 0; k < 3; k++) { centre[k] = (mn[k] + mx[k]) * 0.5; }
}
} // namespace

uint32_t PhysicsWorld::addMesh(const float* vertices, size_t vertexCount, const uint32_t* indices, size_t indexCount)
{
	if (!impl) { impl = std::make_unique<Impl>(); }
	if (!vertices || vertexCount < 3 || !indices || indexCount < 3) { return 0; }
	double centre[3];
	centreOf(vertices, vertexCount, centre);
	JPH::MeshShapeSettings settings;
	settings.mTriangleVertices.reserve(vertexCount);
	for (size_t i = 0; i < vertexCount; i++)
	{
		settings.mTriangleVertices.push_back(JPH::Float3((float)(vertices[i * 3] - centre[0]), (float)(vertices[i * 3 + 1] - centre[1]), (float)(vertices[i * 3 + 2] - centre[2])));
	}
	settings.mIndexedTriangles.reserve(indexCount / 3);
	for (size_t i = 0; i + 2 < indexCount; i += 3) { settings.mIndexedTriangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0)); }
	const JPH::ShapeSettings::ShapeResult r = settings.Create();
	if (r.HasError()) { fprintf(stderr, "physics: mesh shape: %s\n", r.GetError().c_str()); return 0; }
	JPH::Ref<JPH::Shape> shape = r.Get();
	return impl->add(shape.GetPtr(), centre, false, "mesh");
}

uint32_t PhysicsWorld::addConvex(const float* vertices, size_t vertexCount)
{
	if (!impl) { impl = std::make_unique<Impl>(); }
	if (!vertices || vertexCount < 4) { return 0; }
	double centre[3];
	centreOf(vertices, vertexCount, centre);
	JPH::ConvexHullShapeSettings settings;
	settings.mPoints.reserve(vertexCount);
	for (size_t i = 0; i < vertexCount; i++)
	{
		settings.mPoints.push_back(JPH::Vec3((float)(vertices[i * 3] - centre[0]), (float)(vertices[i * 3 + 1] - centre[1]), (float)(vertices[i * 3 + 2] - centre[2])));
	}
	const JPH::ShapeSettings::ShapeResult r = settings.Create();
	if (r.HasError()) { fprintf(stderr, "physics: convex shape: %s\n", r.GetError().c_str()); return 0; }
	JPH::Ref<JPH::Shape> shape = r.Get();
	return impl->add(shape.GetPtr(), centre, false, "convex");
}

bool PhysicsWorld::remove(uint32_t id)
{
	if (!impl || id == 0 || id > impl->bodies.size() || !impl->bodies[id - 1].valid || impl->bodies[id - 1].ragdoll) { return false; }	// a ragdoll's bodies go with it
	JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	bi.RemoveBody(impl->bodies[id - 1].id);
	bi.DestroyBody(impl->bodies[id - 1].id);
	impl->bodies[id - 1].valid = false;
	return true;
}

bool PhysicsWorld::body(uint32_t id, Body& out) const
{
	if (!impl || id == 0 || id > impl->bodies.size() || !impl->bodies[id - 1].valid) { return false; }
	const JPH::BodyLockRead lock(impl->system.GetBodyLockInterface(), impl->bodies[id - 1].id);
	if (!lock.Succeeded()) { return false; }
	const JPH::Body& b = lock.GetBody();
	const JPH::RVec3 p = b.GetPosition();
	const JPH::Vec3 v = b.GetLinearVelocity();
	const JPH::Quat q = b.GetRotation();
	const JPH::Vec3 w = b.GetAngularVelocity();
	out.p[0] = p.GetX(); out.p[1] = p.GetY(); out.p[2] = p.GetZ();
	out.v[0] = v.GetX(); out.v[1] = v.GetY(); out.v[2] = v.GetZ();
	out.q[0] = q.GetW(); out.q[1] = q.GetX(); out.q[2] = q.GetY(); out.q[3] = q.GetZ();
	out.w[0] = w.GetX(); out.w[1] = w.GetY(); out.w[2] = w.GetZ();
	out.awake = b.IsActive();
	return true;
}

std::vector<uint32_t> PhysicsWorld::dynamicBodies() const
{
	std::vector<uint32_t> out;
	if (!impl) { return out; }
	for (size_t i = 0; i < impl->bodies.size(); i++) { if (impl->bodies[i].valid && impl->bodies[i].dynamic) { out.push_back((uint32_t)i + 1); } }
	return out;
}

void PhysicsWorld::stepFixed(double dt, int maxSteps, const std::function<void(double)>& before)
{
	if (!impl || !(dt > 0) || maxSteps <= 0) { return; }
	// The accumulator: a frame shorter than one step runs no steps (a 144 Hz frame at 1x must not simulate 2.4x the game time) and the time
	// carries to the next frame; past the cap the excess is dropped.
	impl->acc += dt;
	int n = 0;
	while (impl->acc >= kMaxSubstep && n < maxSteps)
	{
		if (before) { before(kMaxSubstep); }
		impl->system.Update((float)kMaxSubstep, 1, &impl->temp, &impl->jobs);
		impl->steps++;
		impl->acc -= kMaxSubstep;
		n++;
	}
	if (impl->acc >= kMaxSubstep)
	{
		impl->acc = kMaxSubstep;	// the cap: the leftover stays one step (the next frame steps it) and the rest is dropped
		if (!impl->capped)
		{
			impl->capped = true;
			fprintf(stderr, "physics: ragdoll steps capped at %d a frame (game dt %.3f s)\n", maxSteps, dt);
		}
	}
}

std::string PhysicsWorld::report() const
{
	char line[512];
	if (!impl)
	{
		snprintf(line, sizeof line, "physics: not started (no bodies)\n");
		return line;
	}
	size_t statics = 0, dynamic = 0;
	for (const Impl::Rec& r : impl->bodies) { if (r.valid) { (r.dynamic ? dynamic : statics)++; } }
	// The precision measurement: the spacing of a position at the world's edge (+-147456 u), float vs double.
	const double edge = -kworld::kWorldMin;
	const double floatStep = (double)std::nextafterf((float)edge, 1e9f) - edge;
	const double doubleStep = std::nextafter(edge, 1e9) - edge;
	std::string s;
	snprintf(line, sizeof line, "physics: Jolt %d.%d.%d double precision, %zu bodies (%zu static, %zu dynamic), %llu steps, substep <= 1/60 s, %d job threads, gravity %.1f u/s^2\n",
		JPH_VERSION_MAJOR, JPH_VERSION_MINOR, JPH_VERSION_PATCH, statics + dynamic, statics, dynamic, (unsigned long long)impl->steps, kJobThreads, kGravity);
	s += line;
	snprintf(line, sizeof line, "physics precision: position spacing at the world edge %.0f u: float %.5f u, double %.2e u\n", (double)edge, floatStep, doubleStep);
	s += line;
	// The statics (hundreds of collider shapes) by kind; each dynamic body by id.
	std::map<std::string, size_t> kinds;
	for (const Impl::Rec& r : impl->bodies) { if (r.valid && !r.dynamic) { kinds[r.desc]++; } }
	s += "physics statics:";
	for (const auto& [kind, n] : kinds) { snprintf(line, sizeof line, " %s %zu", kind.c_str(), n); s += line; }
	if (kinds.empty()) { s += " none"; }
	s += "\n";
	snprintf(line, sizeof line, "physics ragdolls: %zu\n", ragdolls());
	s += line;
	for (size_t i = 0; i < impl->bodies.size(); i++)
	{
		Body b;
		if (!impl->bodies[i].valid || !impl->bodies[i].dynamic || !body((uint32_t)i + 1, b)) { continue; }
		snprintf(line, sizeof line, "physics body %zu %s at %.1f %.1f %.1f v %.2f %.2f %.2f awake %d\n", i + 1, impl->bodies[i].desc.c_str(),
			b.p[0], b.p[1], b.p[2], b.v[0], b.v[1], b.v[2], (int)b.awake);
		s += line;
	}
	return s;
}

namespace
{
// --- Ragdolls (.phs -> Jolt RagdollSettings; ragdoll.hpp has the format) ---
// The .phs gives a body's bind rotation as the model -> body matrix (the bind orientation's inverse) and a part's frame in model axes
// relative to its body; Jolt wants the body -> model rotation, the parts in body space, and each joint's anchor and axes where the bodies
// are created.
void transpose3(const float in[9], float out[9])
{
	for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) { out[i * 3 + j] = in[j * 3 + i]; } }
}

void multiply3(const float a[9], const float b[9], float out[9])
{
	float r[9];
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++) { r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j]; }
	}
	for (int i = 0; i < 9; i++) { out[i] = r[i]; }
}

JPH::Quat quatOf(const float rows[9])	// a rotation matrix (row-major): its columns are the basis images
{
	const JPH::Mat44 m(JPH::Vec4(rows[0], rows[3], rows[6], 0), JPH::Vec4(rows[1], rows[4], rows[7], 0), JPH::Vec4(rows[2], rows[5], rows[8], 0), JPH::Vec4(0, 0, 0, 1));
	return m.GetQuaternion().Normalized();
}

JPH::Vec3 vec3(const float v[3]) { return JPH::Vec3(v[0], v[1], v[2]); }

// The body's collision shape from its parts: each part's frame (model axes) through the body's model -> body rotation into body space. A part
// the reader does not know (or an empty hull) is skipped; a body with no parts at all has no shape. The .phs stores a part's position as an
// absolute model point, so the shape's frame relative to its body is part.pos - body.pos (the 2026-11-07 legs sunk fix).
JPH::RefConst<JPH::Shape> bodyShape(const ragdoll::Body& body)
{
	struct Placed
	{
		JPH::Ref<JPH::ShapeSettings> shape;
		JPH::Vec3 pos;
		JPH::Quat rot;
	};
	std::vector<Placed> placed;
	for (const ragdoll::Part& part : body.parts)
	{
		JPH::Ref<JPH::ShapeSettings> shape;
		switch (part.type)
		{
		case 3:	// box: dims are the full extents
			shape = new JPH::BoxShapeSettings(JPH::Vec3(part.dims[0], part.dims[1], part.dims[2]) * 0.5f);
			break;
		case 4:	// sphere: dims[0] is the diameter
			shape = new JPH::SphereShapeSettings(part.dims[0] * 0.5f);
			break;
		case 5:	// capsule: radius dims[0], length dims[1] including the caps, along the shape's y axis (Jolt's)
		{
			const float radius = part.dims[0] * 0.5f;
			const float half = (part.dims[1] - part.dims[0]) * 0.5f;
			shape = half > 0 ? static_cast<JPH::ShapeSettings*>(new JPH::CapsuleShapeSettings(half, radius)) : static_cast<JPH::ShapeSettings*>(new JPH::SphereShapeSettings(radius));
			break;
		}
		case 9:
		{
			if (part.hull.size() < 12) { fprintf(stderr, "physics: ragdoll: body %s: empty hull part\n", body.bone.c_str()); continue; }
			auto hull = new JPH::ConvexHullShapeSettings;
			for (size_t i = 0; i + 2 < part.hull.size(); i += 3) { hull->mPoints.push_back(JPH::Vec3(part.hull[i], part.hull[i + 1], part.hull[i + 2])); }
			shape = hull;
			break;
		}
		default:
			fprintf(stderr, "physics: ragdoll: body %s: part type %d skipped\n", body.bone.c_str(), part.type);
			continue;
		}
		const float rel[3] = { part.pos[0] - body.pos[0], part.pos[1] - body.pos[1], part.pos[2] - body.pos[2] };
		float rot[9], pos[3];
		multiply3(body.rot, part.rot, rot);
		for (int i = 0; i < 3; i++) { pos[i] = body.rot[i * 3] * rel[0] + body.rot[i * 3 + 1] * rel[1] + body.rot[i * 3 + 2] * rel[2]; }
		placed.push_back({ shape, vec3(pos), quatOf(rot) });
	}
	if (placed.empty()) { return nullptr; }
	JPH::Ref<JPH::ShapeSettings> outer;
	if (placed.size() == 1) { outer = new JPH::RotatedTranslatedShapeSettings(placed[0].pos, placed[0].rot, placed[0].shape.GetPtr()); }
	else
	{
		auto compound = new JPH::StaticCompoundShapeSettings;
		for (const Placed& p : placed) { compound->AddShape(p.pos, p.rot, p.shape.GetPtr()); }
		outer = compound;
	}
	JPH::ShapeSettings::ShapeResult result = outer->Create();
	if (result.HasError()) { fprintf(stderr, "physics: ragdoll: body %s: %s\n", body.bone.c_str(), result.GetError().c_str()); return nullptr; }
	return result.Get();
}

// A joint record as a Jolt constraint. The record's frame rows hold the joint axes in model space: row 1 is the hinge axis of a revolute joint
// and the swing axis of a spherical one, row 0 the normal (reference) axis and the twist axis (checked against the shipped hip/knee/elbow and
// shoulder limits; which PhysX member the exporter wrote them into is UNSURE). Limits are degrees in the record, radians in PhysX and Jolt.
JPH::Ref<JPH::TwoBodyConstraintSettings> jointSettings(const ragdoll::Joint& joint, const JPH::Quat& yaw, const double base[3])
{
	const JPH::Vec3 row0 = yaw * vec3(&joint.frame[0]);
	const JPH::Vec3 row1 = yaw * vec3(&joint.frame[3]);
	const JPH::Vec3 p = yaw * vec3(joint.pos);
	const JPH::RVec3 anchor(base[0] + p.GetX(), base[1] + p.GetY(), base[2] + p.GetZ());
	const bool limits = joint.limitsEnabled;
	switch (joint.type)
	{
	case 1:	// revolute -> hinge
	{
		auto h = new JPH::HingeConstraintSettings;
		h->mSpace = JPH::EConstraintSpace::WorldSpace;
		h->mPoint1 = h->mPoint2 = anchor;
		h->mHingeAxis1 = h->mHingeAxis2 = row1;
		h->mNormalAxis1 = h->mNormalAxis2 = row0;
		h->mLimitsMin = limits ? std::clamp(JPH::DegreesToRadians(joint.limits[1]), -JPH::JPH_PI, 0.0f) : -JPH::JPH_PI;
		h->mLimitsMax = limits ? std::clamp(JPH::DegreesToRadians(joint.limits[0]), 0.0f, JPH::JPH_PI) : JPH::JPH_PI;
		return h;
	}
	case 2:	// spherical -> swing-twist with a cone swing
	{
		auto s = new JPH::SwingTwistConstraintSettings;
		s->mSpace = JPH::EConstraintSpace::WorldSpace;
		s->mPosition1 = s->mPosition2 = anchor;
		s->mTwistAxis1 = s->mTwistAxis2 = row0;
		s->mPlaneAxis1 = s->mPlaneAxis2 = row1;
		s->mSwingType = JPH::ESwingType::Cone;
		const float cone = limits ? JPH::DegreesToRadians(std::clamp(joint.limits[2], 0.0f, 180.0f)) : JPH::JPH_PI;
		s->mNormalHalfConeAngle = cone;
		s->mPlaneHalfConeAngle = cone;
		s->mTwistMaxAngle = limits ? JPH::DegreesToRadians(std::clamp(joint.limits[0], -180.0f, 180.0f)) : JPH::JPH_PI;
		s->mTwistMinAngle = limits ? JPH::DegreesToRadians(std::clamp(joint.limits[1], -180.0f, 180.0f)) : -JPH::JPH_PI;
		return s;
	}
	case 5:
	{
		auto f = new JPH::FixedConstraintSettings;
		f->mSpace = JPH::EConstraintSpace::WorldSpace;
		f->mAutoDetectPoint = false;
		f->mPoint1 = f->mPoint2 = anchor;
		f->mAxisX1 = f->mAxisX2 = row0;
		f->mAxisY1 = f->mAxisY2 = row1;
		return f;
	}
	default:
		return nullptr;
	}
}
} // namespace

uint32_t PhysicsWorld::addRagdoll(const ragdoll::Def& def, const double pos[3], float yaw)
{
	if (!impl) { impl = std::make_unique<Impl>(); }
	const int n = (int)def.bodies.size();
	if (n == 0) { return 0; }
	// Joints connect body pairs; their parent/child direction is a tree in most rigs but not all (hu_m_ragdoll_5's neck carries two joints,
	// its head standing alone in the record). Jolt's ragdoll is a tree, so: span the joint graph from the body that reaches the most others
	// along the record's direction, one parent per body, and give every other joint to Jolt as an additional constraint.
	std::vector<std::vector<std::pair<int, int>>> links(n);	// body -> (other body, joint index)
	for (size_t i = 0; i < def.joints.size(); i++)
	{
		const ragdoll::Joint& j = def.joints[i];
		if (j.parent < 0 || j.parent >= n || j.child < 0 || j.child >= n || j.parent == j.child) { continue; }	// the root marker
		if (j.type != 1 && j.type != 2 && j.type != 5) { fprintf(stderr, "physics: ragdoll: joint type %u not supported\n", j.type); continue; }
		links[j.parent].push_back({ j.child, (int)i });
		links[j.child].push_back({ j.parent, (int)i });
	}
	int root = 0;
	size_t most = 0;
	for (int b = 0; b < n; b++)	// the record's tree root: the most bodies reachable from it
	{
		std::vector<char> seen(n, 0);
		std::vector<int> stack { b };
		seen[b] = 1;
		size_t reach = 0;
		while (!stack.empty())
		{
			const int at = stack.back();
			stack.pop_back();
			for (const auto& [other, j] : links[at])
			{
				if (seen[other] || def.joints[j].parent != at) { continue; }	// along the direction the record gives
				seen[other] = 1;
				stack.push_back(other);
				reach++;
			}
		}
		if (reach > most) { most = reach; root = b; }
	}
	std::vector<int> order, placed(n, -1), parent(n, -1), treeJoint(n, -1);
	std::vector<char> usedJoint(def.joints.size(), 0);
	order.reserve(n);
	auto grow = [&](int start)
	{
		placed[start] = (int)order.size();
		order.push_back(start);
		for (size_t k = placed[start]; k < order.size(); k++)
		{
			const int b = order[k];
			for (const auto& [other, j] : links[b])
			{
				if (placed[other] >= 0) { continue; }
				placed[other] = (int)order.size();
				order.push_back(other);
				parent[other] = b;
				treeJoint[other] = j;
				usedJoint[j] = 1;
			}
		}
	};
	grow(root);
	while ((int)order.size() < n) { for (int b = 0; b < n; b++) { if (placed[b] < 0) { grow(b); break; } } }
	const JPH::Quat yawQuat = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw);
	JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
	settings->mSkeleton = new JPH::Skeleton;
	for (int b : order)
	{
		const ragdoll::Body& body = def.bodies[b];
		JPH::RefConst<JPH::Shape> shape = bodyShape(body);
		if (shape == nullptr) { fprintf(stderr, "physics: ragdoll: body %d (%s) has no shape, not built\n", b, body.bone.c_str()); return 0; }
		float bind[9];	// the stored rotation is model -> body; Jolt's body rotation is body -> model
		transpose3(body.rot, bind);
		const JPH::Vec3 offset = yawQuat * vec3(body.pos);
		JPH::RagdollSettings::Part part;
		part.mPosition = JPH::RVec3(pos[0] + offset.GetX(), pos[1] + offset.GetY(), pos[2] + offset.GetZ());
		part.mRotation = (yawQuat * quatOf(bind)).Normalized();
		part.mMotionType = JPH::EMotionType::Dynamic;
		part.mObjectLayer = kBodyLayer;
		part.SetShape(shape.GetPtr());
		part.mAllowSleeping = true;
		part.mAllowDynamicOrKinematic = true;	// pinBody: a carried animal's anchor body is kinematic while it is held
		part.mFriction = kFriction;
		part.mRestitution = kRestitution;
		part.mLinearDamping = std::max(0.0f, body.linearDamping);
		part.mAngularDamping = std::max(0.0f, body.angularDamping * 10.0f);	// the record's f224 x 10 is the original's angular damping
		part.mNumVelocityStepsOverride = std::clamp(body.solverIterations, kMinVelocitySteps, kMaxVelocitySteps);
		if (body.mass > 0)	// u22c: an explicit mass overrides the density-derived mass (the inertia stays the shape's, scaled to it)
		{
			part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
			part.mMassPropertiesOverride.mMass = body.mass;
		}
		const int parentBody = parent[b];
		settings->mSkeleton->AddJoint(body.bone, parentBody < 0 ? -1 : placed[parentBody]);
		if (treeJoint[b] >= 0) { part.mToParent = jointSettings(def.joints[treeJoint[b]], yawQuat, pos); }
		settings->mParts.push_back(part);
	}
	// A joint the tree did not need: Jolt binds it as an additional constraint between its two parts.
	for (size_t i = 0; i < def.joints.size(); i++)
	{
		const ragdoll::Joint& j = def.joints[i];
		if (usedJoint[i] || j.parent < 0 || j.parent >= n || j.child < 0 || j.child >= n) { continue; }
		JPH::Ref<JPH::TwoBodyConstraintSettings> c = jointSettings(j, yawQuat, pos);
		if (c) { settings->mAdditionalConstraints.emplace_back(placed[j.parent], placed[j.child], c.GetPtr()); }
	}
	if (!settings->Stabilize()) { fprintf(stderr, "physics: ragdoll: stabilize failed\n"); return 0; }
	settings->DisableParentChildCollisions();
	JPH::Ragdoll* ragdoll = settings->CreateRagdoll(impl->nextGroup++, 0, &impl->system);
	if (!ragdoll) { fprintf(stderr, "physics: ragdoll: out of bodies\n"); return 0; }
	ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
	Impl::Rag rec;
	rec.ragdoll = ragdoll;
	rec.bodies.assign((size_t)n, 0);
	const JPH::Array<JPH::BodyID>& ids = ragdoll->GetBodyIDs();
	for (size_t k = 0; k < ids.size() && k < order.size(); k++)
	{
		impl->bodies.push_back({ ids[k], true, true, "ragdoll " + def.bodies[order[k]].bone, true });
		rec.bodies[order[k]] = (uint32_t)impl->bodies.size();
	}
	impl->ragdolls.push_back(std::move(rec));
	return (uint32_t)impl->ragdolls.size();
}

std::vector<uint32_t> PhysicsWorld::ragdollBodies(uint32_t id) const
{
	if (!impl || id == 0 || id > impl->ragdolls.size()) { return {}; }
	return impl->ragdolls[id - 1].bodies;
}

bool PhysicsWorld::removeRagdoll(uint32_t id)
{
	if (!impl || id == 0 || id > impl->ragdolls.size()) { return false; }
	Impl::Rag& r = impl->ragdolls[id - 1];
	if (!r.ragdoll) { return false; }
	for (uint32_t body : r.bodies) { if (body) { impl->bodies[body - 1].valid = false; } }
	r.ragdoll->RemoveFromPhysicsSystem();
	r.ragdoll = nullptr;	// destroys the bodies
	r.bodies.clear();
	return true;
}

bool PhysicsWorld::ragdollPose(uint32_t id, std::vector<Pose>& out) const
{
	if (!impl || id == 0 || id > impl->ragdolls.size() || !impl->ragdolls[id - 1].ragdoll) { return false; }
	const Impl::Rag& r = impl->ragdolls[id - 1];
	const JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	out.assign(r.bodies.size(), Pose());
	for (size_t k = 0; k < r.bodies.size(); k++)
	{
		if (!r.bodies[k]) { continue; }
		JPH::RVec3 p;
		JPH::Quat q;
		bi.GetPositionAndRotation(impl->bodies[r.bodies[k] - 1].id, p, q);
		out[k].p[0] = p.GetX(); out[k].p[1] = p.GetY(); out[k].p[2] = p.GetZ();
		out[k].q[0] = q.GetW(); out[k].q[1] = q.GetX(); out[k].q[2] = q.GetY(); out[k].q[3] = q.GetZ();
	}
	return true;
}

bool PhysicsWorld::setRagdollPose(uint32_t id, const std::vector<Pose>& in)
{
	if (!impl || id == 0 || id > impl->ragdolls.size() || !impl->ragdolls[id - 1].ragdoll) { return false; }
	const Impl::Rag& r = impl->ragdolls[id - 1];
	if (in.size() != r.bodies.size()) { return false; }
	JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	for (size_t k = 0; k < r.bodies.size(); k++)
	{
		if (!r.bodies[k]) { continue; }
		const JPH::BodyID b = impl->bodies[r.bodies[k] - 1].id;
		const JPH::Quat q = JPH::Quat(in[k].q[1], in[k].q[2], in[k].q[3], in[k].q[0]).Normalized();
		bi.SetPositionAndRotation(b, JPH::RVec3(in[k].p[0], in[k].p[1], in[k].p[2]), q, JPH::EActivation::Activate);
		bi.SetLinearAndAngularVelocity(b, JPH::Vec3::sZero(), JPH::Vec3::sZero());
	}
	return true;
}

bool PhysicsWorld::kick(uint32_t bodyId, const float dv[3])
{
	if (!impl || bodyId == 0 || bodyId > impl->bodies.size() || !impl->bodies[bodyId - 1].valid) { return false; }
	impl->system.GetBodyInterface().AddLinearVelocity(impl->bodies[bodyId - 1].id, JPH::Vec3(dv[0], dv[1], dv[2]));
	return true;
}

bool PhysicsWorld::kick(uint32_t bodyId, const float dv[3], const float dw[3])
{
	if (!impl || bodyId == 0 || bodyId > impl->bodies.size() || !impl->bodies[bodyId - 1].valid) { return false; }
	impl->system.GetBodyInterface().AddLinearAndAngularVelocity(impl->bodies[bodyId - 1].id, JPH::Vec3(dv[0], dv[1], dv[2]), JPH::Vec3(dw[0], dw[1], dw[2]));
	return true;
}

bool PhysicsWorld::pinBody(uint32_t bodyId, const double p[3], const float q[4], double dt)
{
	if (!impl || bodyId == 0 || bodyId > impl->bodies.size() || !impl->bodies[bodyId - 1].valid) { return false; }
	JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	const JPH::BodyID id = impl->bodies[bodyId - 1].id;
	if (bi.GetMotionType(id) != JPH::EMotionType::Kinematic) { bi.SetMotionType(id, JPH::EMotionType::Kinematic, JPH::EActivation::Activate); }
	bi.MoveKinematic(id, JPH::RVec3(p[0], p[1], p[2]), JPH::Quat(q[1], q[2], q[3], q[0]).Normalized(), (float)dt);
	return true;
}

bool PhysicsWorld::unpinBody(uint32_t bodyId)
{
	if (!impl || bodyId == 0 || bodyId > impl->bodies.size() || !impl->bodies[bodyId - 1].valid) { return false; }
	JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	const JPH::BodyID id = impl->bodies[bodyId - 1].id;
	if (bi.GetMotionType(id) == JPH::EMotionType::Kinematic) { bi.SetMotionType(id, JPH::EMotionType::Dynamic, JPH::EActivation::Activate); }
	return true;
}

bool PhysicsWorld::ragdollAwake(uint32_t id) const
{
	if (!impl || id == 0 || id > impl->ragdolls.size() || !impl->ragdolls[id - 1].ragdoll) { return false; }
	const JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	for (uint32_t b : impl->ragdolls[id - 1].bodies) { if (b && bi.IsActive(impl->bodies[b - 1].id)) { return true; } }
	return false;
}

void PhysicsWorld::sleepRagdoll(uint32_t id)
{
	if (!impl || id == 0 || id > impl->ragdolls.size() || !impl->ragdolls[id - 1].ragdoll) { return; }
	JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	for (uint32_t b : impl->ragdolls[id - 1].bodies) { if (b) { bi.DeactivateBody(impl->bodies[b - 1].id); } }
}

void PhysicsWorld::setRagdollFriction(uint32_t id, float friction)
{
	if (!impl || id == 0 || id > impl->ragdolls.size() || !impl->ragdolls[id - 1].ragdoll) { return; }
	JPH::BodyInterface& bi = impl->system.GetBodyInterface();
	for (uint32_t b : impl->ragdolls[id - 1].bodies) { if (b) { bi.SetFriction(impl->bodies[b - 1].id, friction < 0 ? kFriction : friction); } }
}

size_t PhysicsWorld::ragdolls() const
{
	size_t n = 0;
	if (impl) { for (const Impl::Rag& r : impl->ragdolls) { if (r.ragdoll) { n++; } } }
	return n;
}

namespace
{
constexpr int kPatchIntervals = 32;	// 33 samples a side: 576 u at the heightmap's 18 u spacing
constexpr int kSyncCalls = 30;		// the collider set is synced about every 0.5 s of frames, not each frame

// Distance from a point to a shape's world box, against the keep radius.
bool nearBox(const kworld::WorldShape& s, const double p[3], double radius)
{
	double d2 = 0;
	for (int k = 0; k < 3; k++)
	{
		const double d = p[k] < s.min[k] ? s.min[k] - p[k] : p[k] > s.max[k] ? p[k] - s.max[k] : 0;
		d2 += d * d;
	}
	return d2 <= radius * radius;
}
} // namespace

StaticColliders::StaticColliders(PhysicsWorld& world) : world_(world) {}

void StaticColliders::update(const std::shared_ptr<const std::vector<kworld::WorldShape>>& shapes, const kworld::Heightmap& terrain, const std::vector<uint32_t>& bodies,
	const std::vector<std::array<double, 3>>& starts, double radius)
{
	const bool regather = shapes != shapes_;
	// A new or removed body or start point (a ragdoll starting) must find the floor this frame, not half a second later: sync on a change too.
	const bool changed = bodies != bodies_ || starts != starts_;
	if (!regather && !changed && calls++ % kSyncCalls != 0) { return; }
	bodies_ = bodies;
	starts_ = starts;
	std::vector<std::array<double, 3>> points = starts;
	points.reserve(bodies.size() + starts.size());
	for (uint32_t id : bodies)
	{
		PhysicsWorld::Body b;
		if (world_.body(id, b)) { points.push_back({ b.p[0], b.p[1], b.p[2] }); }
	}
	// Building shapes. A new gather has different shape indices: drop the old set and re-post from the new one.
	if (regather)
	{
		for (const auto& [index, id] : postedShapes_) { world_.remove(id); }
		postedShapes_.clear();
		shapes_ = shapes;
	}
	if (shapes_)
	{
		for (size_t i = 0; i < shapes_->size(); i++)
		{
			const kworld::WorldShape& s = (*shapes_)[i];
			bool near = false;
			if (s.use != kworld::NavUse::Door && !s.indices.empty())	// door leaves move with their door: not static
			{
				for (const auto& p : points) { if (nearBox(s, p.data(), radius)) { near = true; break; } }
			}
			const auto it = postedShapes_.find(i);
			if (near && it == postedShapes_.end())
			{
				const uint32_t id = s.closed && s.vertices.size() / 3 <= 64 ? world_.addConvex(s.vertices.data(), s.vertices.size() / 3)
					: world_.addMesh(s.vertices.data(), s.vertices.size() / 3, s.indices.data(), s.indices.size());
				if (id) { postedShapes_[i] = id; }
			}
			else if (!near && it != postedShapes_.end())
			{
				world_.remove(it->second);
				postedShapes_.erase(it);
			}
		}
	}
	// Terrain: the patch meshes round the bodies (patch == centre cell plus its neighbours within the radius).
	const double spacing = kworld::kHeightmapSpacing;
	const double patch = spacing * kPatchIntervals;
	std::unordered_map<uint64_t, std::array<double, 3>> want;	// key -> the first sample's world position
	for (const auto& p : points)
	{
		const int px0 = (int)std::floor(p[0] / patch), pz0 = (int)std::floor(p[2] / patch);
		for (int dz = -1; dz <= 1; dz++)
		{
			for (int dx = -1; dx <= 1; dx++)
			{
				const double x0 = (px0 + dx) * patch, z0 = (pz0 + dz) * patch;
				if (x0 < kworld::kWorldMin || z0 < kworld::kWorldMin || x0 + patch > kworld::kWorldMin + kworld::kWorldExtent || z0 + patch > kworld::kWorldMin + kworld::kWorldExtent) { continue; }
				const double dx2 = p[0] < x0 ? x0 - p[0] : p[0] > x0 + patch ? p[0] - (x0 + patch) : 0;
				const double dz2 = p[2] < z0 ? z0 - p[2] : p[2] > z0 + patch ? p[2] - (z0 + patch) : 0;
				if (dx2 * dx2 + dz2 * dz2 > radius * radius) { continue; }
				want.emplace(((uint64_t)(uint32_t)(px0 + dx) << 32) | (uint32_t)(pz0 + dz), std::array<double, 3>{ x0, 0, z0 });
			}
		}
	}
	for (auto it = patches_.begin(); it != patches_.end();)
	{
		if (!want.count(it->first)) { world_.remove(it->second); it = patches_.erase(it); }
		else { want.erase(it->first); ++it; }
	}
	// A triangle mesh with the drawn mesh's split (terrain.cpp: {a, c, b}), not a Jolt height field: a height field splits
	// each cell along (i, j)-(i+1, j+1), the other diagonal, and on rough ground the two surfaces differ by up to 7 u at
	// the quad centres - a lying body would sink or float by that much.
	const int n = kPatchIntervals + 1;
	for (const auto& [key, origin] : want)
	{
		std::vector<float> verts;
		verts.reserve((size_t)n * n * 3);
		for (int row = 0; row < n; row++)
		{
			for (int col = 0; col < n; col++)
			{
				verts.push_back((float)(origin[0] + col * spacing));
				verts.push_back((float)terrain.height(origin[0] + col * spacing, origin[2] + row * spacing));
				verts.push_back((float)(origin[2] + row * spacing));
			}
		}
		std::vector<uint32_t> idx;
		idx.reserve((size_t)kPatchIntervals * kPatchIntervals * 6);
		for (int row = 0; row < kPatchIntervals; row++)
		{
			for (int col = 0; col < kPatchIntervals; col++)
			{
				const uint32_t a = (uint32_t)(row * n + col), b = a + 1, c = a + (uint32_t)n, d = c + 1;
				const uint32_t t[6] = { a, c, b, b, c, d };
				idx.insert(idx.end(), t, t + 6);
			}
		}
		const uint32_t id = world_.addMesh(verts.data(), verts.size() / 3, idx.data(), idx.size());
		if (id) { patches_[key] = id; }
	}
}

bool StaticColliders::covered(const double point[3], double radius) const
{
	if (!shapes_) { return false; }
	for (const auto& [index, id] : postedShapes_)
	{
		if (index < shapes_->size() && nearBox((*shapes_)[index], point, radius)) { return true; }
	}
	return false;
}
