#include "bridge/ragdolldrive.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

#include "characters/characters.hpp"
#include "app/config.hpp"
#include "characters/ragdoll.hpp"
#include "kworld/heightmap.hpp"

namespace
{
const cfg::Int kRagdolls{ cfg::Group{ "KO and ragdolls" }, "physics.ragdolls", 1, 0, 1, "KO / death / put-down: Jolt ragdolls from the sim's ragdoll events (0 = the interim frozen lying pose)" };
// MINE (CLIENT_GOALS "Ragdolls" Budget): awake lying ragdolls at most; past it a KO keeps the interim lying pose and settled bodies go back to sleep
const cfg::Int kRagdollCap{ cfg::Group{ "KO and ragdolls" }, "physics.ragdoll_cap", 64, 0, 1024, "Ragdolls: at most this many lying bodies awake; past it a KO keeps the frozen lying pose and the oldest settled bodies sleep" };
// MINE: the event's strength (OURS 3.0 for a KO) as a velocity change of the struck part, u/s per unit (3.0 -> 15 u/s = 1.5 m/s); the
// original's force scale is untraced.
constexpr float kKickPerStrength = 5.0f;
// A ragdoll starting this far above the terrain needs a posted building shape under it (StaticColliders::covered): the gather follows the
// camera's zone cell, and a body on a floor the gather misses would fall through to the ground.
constexpr double kOffTerrain = 3.0;
constexpr int kTries = 5;	// updates a cue waits for its pose (a rebuild) or its floor (posted the next update)
constexpr double kLift = 3.0;	// u: the lowest bone's start height over the drawn ground (a limb's capsule radius is about 1.5-2.5 u)
// MINE: the settling (CLIENT_GOALS "Ragdolls" Settling). The body falls freely for kFreeFall, then its root body is pulled to the sim's
// rest point so it arrives at kSettle seconds and the bodies sleep. The pull is the root's velocity servo, its horizontal change given to
// every body, eased in and stopping dead at the target: joints, contacts and gravity still act, so the body drags along the ground rather
// than flying.
// A drag that is still short at kSettle (the speed cap and the limbs' weight hold it back: 5-7 u short from 10-18 u starts) holds the
// target until the root is within kReach of it across the ground, for at most kOvertime more. A body the ground or another body still holds
// short of it (kolanding: ~1 u short, stalled at 1 u/s) is then glided the rest of the way over kGlide, every body shifted by the same small
// step each substep (PhysicsWorld::shiftBody), so it lands on the rest point by kSettle + kOvertime + kGlide without a snap.
constexpr double kFreeFall = 0.5, kSettle = 1.5, kOvertime = 1.0, kGlide = 0.5;
constexpr double kReach = 0.5;			// u: horizontal
constexpr double kPullTop = 20.0;		// u/s
constexpr double kTrack = 0.05;			// s: the root's velocity servo's lag behind the path
constexpr double kSettledLift = 1.0;	// u: a late joiner's lowest bone over the floor (its capsule then lies on it)
// A carried animal is a live ragdoll (rwh.118): only its anchor body (the race's carry bone, else the root) is pinned, every step, to its place in the
// carrier's shoulder frame; physics runs the rest, so the limbs dangle and settle on their own. A carrier that jumps past kHangReset in one step
// (teleport, load, a re-entry) gets the whole body put back on the shoulder at rest.
constexpr double kHangReset = 40.0;	// u
constexpr int kDropWait = 5;	// updates a body whose carry ended waits to be drawn lying (the KO flag and the carry's end can be an update apart)

kworld::SkelQuat conj(const kworld::SkelQuat& q) { return { q.w, -q.x, -q.y, -q.z }; }

// The budget's range (P5): the draw range, i.e. the interest radius round the camera, horizontal as the send filter measures it.
bool pastRange(const SimView& sim, uint64_t id, const double eye[3])
{
	double feet[3];
	float hull[3];
	return sim.placed(id, feet, hull) && std::hypot(feet[0] - eye[0], feet[2] - eye[2]) > SimView::kInterestRadius;
}

// The floor at a rest point: the drawn terrain, or the sim's y where it is well above it (a deck). The blend of SimView's feet placement
// (simview.cpp): open ground's sim y is a coarse navmesh fit, so it is not trusted there.
double restFloor(const kworld::Heightmap* terrain, const double rest[3])
{
	if (!terrain) { return rest[1]; }
	const double t = terrain->terrainHeight(rest[0], rest[2]);
	return t + (rest[1] - t) * std::clamp((rest[1] - t - 2.0) / 2.0, 0.0, 1.0);
}

// The struck part's body: the LOCATIONAL_DAMAGE part name to the bone its ragdoll body is rigged to. mapped = the name is in the table (else the
// kick lands on the pelvis and the caller logs the name once per race, so the table can be grown). An animal's forelegs are its biped rig's arms.
int partBody(const ragdoll::Def& def, const std::string& part, bool& mapped)
{
	mapped = false;
	static const std::pair<const char*, const char*> kBone[] = { { "Head", "Bip01 Head" }, { "Chest", "Bip01 Spine1" }, { "Stomach", "Bip01 Spine" },
		{ "Left Arm", "Bip01 L UpperArm" }, { "Right Arm", "Bip01 R UpperArm" }, { "Left Foreleg", "Bip01 L UpperArm" }, { "Right Foreleg", "Bip01 R UpperArm" },
		{ "Left Leg", "Bip01 L Thigh" }, { "Right Leg", "Bip01 R Thigh" } };
	for (const auto& [name, bone] : kBone)
	{
		if (part != name) { continue; }
		mapped = true;
		for (size_t b = 0; b < def.bodies.size(); b++) { if (def.bodies[b].bone == bone) { return (int)b; } }
	}
	for (size_t b = 0; b < def.bodies.size(); b++) { if (def.bodies[b].bone == "Bip01 Pelvis") { return (int)b; } }
	return 0;
}

// The body the pull moves: the shared root (hu_m_ragdoll_5 names it "Bip01", the body every other joint hangs from).
int rootBody(const ragdoll::Def& def)
{
	for (size_t b = 0; b < def.bodies.size(); b++) { if (def.bodies[b].bone == "Bip01 Pelvis" || def.bodies[b].bone == "Bip01") { return (int)b; } }
	return 0;
}

// MINE (plans/animal-down): a race with no lying clip has no pose that lies, and a ragdoll started from its standing pose stays planted on its legs
// (four contacts hold a quadruped up; the pull cannot topple it). Lay the start pose down: pitch it -90 deg about the lateral axis of the sim's rest
// yaw (head and feet then run along the lying direction) about the root body, and move the root to the rest point. Jolt settles the limbs and the
// pull holds the result.
void layDown(std::vector<PhysicsWorld::Pose>& world, int root, const double rest[3], float restYaw)
{
	if (root < 0 || (size_t)root >= world.size()) { return; }
	const double axis[3] = { std::cos(restYaw), 0.0, -std::sin(restYaw) };	// lateral: across the lying direction (sin yaw, 0, cos yaw)
	const double h = std::sqrt(0.5), s = -h;	// the quaternion of -90 deg about `axis`: (cos 45, sin(-45) x axis)
	const double qr[4] = { h, s * axis[0], s * axis[1], s * axis[2] };
	const double pivot[3] = { world[(size_t)root].p[0], world[(size_t)root].p[1], world[(size_t)root].p[2] };
	for (PhysicsWorld::Pose& p : world)
	{
		double r[3] = { p.p[0] - pivot[0], p.p[1] - pivot[1], p.p[2] - pivot[2] };
		const double dot = r[0] * axis[0] + r[1] * axis[1] + r[2] * axis[2];
		const double c[3] = { axis[1] * r[2] - axis[2] * r[1], axis[2] * r[0] - axis[0] * r[2], axis[0] * r[1] - axis[1] * r[0] };
		for (int k = 0; k < 3; k++) { p.p[k] = -c[k] + axis[k] * dot + pivot[k]; }	// Rodrigues at -90 deg: v' = -(axis x v) + axis (axis . v)
		const double b0 = p.q[0], b1 = p.q[1], b2 = p.q[2], b3 = p.q[3];
		const double w = qr[0] * b0 - qr[1] * b1 - qr[2] * b2 - qr[3] * b3;
		const double x = qr[0] * b1 + qr[1] * b0 + qr[2] * b3 - qr[3] * b2;
		const double y = qr[0] * b2 - qr[1] * b3 + qr[2] * b0 + qr[3] * b1;
		const double z = qr[0] * b3 + qr[1] * b2 - qr[2] * b1 + qr[3] * b0;
		const double n = std::sqrt(w * w + x * x + y * y + z * z);
		if (n < 1e-9) { continue; }
		p.q[0] = (float)(w / n); p.q[1] = (float)(x / n); p.q[2] = (float)(y / n); p.q[3] = (float)(z / n);
	}
	const double dx = rest[0] - pivot[0], dz = rest[2] - pivot[2];
	for (PhysicsWorld::Pose& p : world) { p.p[0] += dx; p.p[2] += dz; }
}
}

Ragdolls::Ragdolls(PhysicsWorld& world, StaticColliders& statics) : world_(world), statics_(statics) {}
Ragdolls::~Ragdolls() = default;

const ragdoll::Def* Ragdolls::def(const std::string& path)
{
	const auto it = defs_.find(path);
	if (it != defs_.end()) { return it->second.get(); }
	auto d = std::make_unique<ragdoll::Def>();
	std::string err;
	if (!ragdoll::load(path, *d, err))
	{
		fprintf(stderr, "ragdolls: %s: %s\n", path.c_str(), err.c_str());
		d.reset();
	}
	return (defs_[path] = std::move(d)).get();
}

// The character's race ragdoll (Characters::ragdollFile). An empty field is the record saying the race has none: no ragdoll then, never a
// substitute - logged once per race.
std::string Ragdolls::fileFor(uint64_t id, SimView& sim, Characters& chars, size_t index)
{
	const std::string file = chars.ragdollFile(index);
	if (!file.empty()) { return file; }
	const std::string race = chars.nameOf(sim.raceOf(id));
	if (noRagdoll_.insert(race).second) { fprintf(stderr, "ragdolls: %s: the RACE record names no ragdoll: no lying pose (it stays in its animation)\n", race.c_str()); }
	return std::string();
}

void Ragdolls::commitLying(SimView& sim, Characters& chars, uint64_t id)
{
	if (!sim.ragdollStarted(id)) { return; }
	const int index = sim.charIndex(id);
	if (index < 0) { return; }
	// the state is committed this frame, after SimView filled the spec: the down character's speed is no longer updated, so it is zeroed here
	Characters::Spec& s = chars.live((size_t)index);
	s.move = 0;
	s.strafe = 0;
}

bool Ragdolls::start(uint64_t id, SimView& sim, Characters& chars, const kworld::Heightmap* terrain, bool& retry)
{
	const int index = sim.charIndex(id);
	const kworld::Skeleton* sk = index >= 0 ? chars.skeleton((size_t)index) : nullptr;
	if (!sk) { fprintf(stderr, "ragdolls: %llu: not built (index %d)\n", (unsigned long long)id, index); return false; }
	double at[3];
	chars.nodePos((size_t)index, at);
	if (terrain && at[1] > terrain->terrainHeight(at[0], at[2]) + kOffTerrain && !statics_.covered(at, 30.0))
	{
		waitingAt_.push_back({ at[0], at[1], at[2] });	// the colliders post the floor round it next update
		why_ = "no floor at " + std::to_string((int)at[0]) + "," + std::to_string((int)at[2]) + " (" + std::to_string(at[1] - terrain->terrainHeight(at[0], at[2]))
			+ " u over the terrain, " + std::to_string(statics_.posted()) + " shapes posted)";
		retry = true;
		return false;	// keeps the lying pose meanwhile
	}
	const std::string file = fileFor(id, sim, chars, (size_t)index);
	if (file.empty()) { return false; }
	const ragdoll::Def* d = def(file);
	if (!d) { return false; }
	Live l;
	l.file = file;
	l.bones = ragdoll::boneMap(*d, *sk);
	std::vector<Characters::RagBone> pose(l.bones.size());
	for (size_t b = 0; b < l.bones.size(); b++)
	{
		if (l.bones[b] < 0) { fprintf(stderr, "ragdolls: %s: body %s has no bone in the skeleton\n", file.c_str(), d->bodies[b].bone.c_str()); return false; }
		pose[b].bone = l.bones[b];
	}
	if (!chars.ragdollStart((size_t)index, pose)) { retry = true; why_ = "no pose"; return false; }	// rebuilt this update (the KO's fall): no pose yet
	const bool unbuilt = sim.lyingUnbuilt(id);	// a race with no lying clip (plans/animal-down): this body carries the pose, laid down below
	l.rag = world_.addRagdoll(*d, at, 0.0f);
	if (!l.rag) { return false; }
	std::vector<PhysicsWorld::Pose> world(pose.size());
	for (size_t b = 0; b < pose.size(); b++)
	{
		for (int k = 0; k < 3; k++) { world[b].p[k] = pose[b].p[k]; }
		world[b].q[0] = pose[b].q.w; world[b].q[1] = pose[b].q.x; world[b].q[2] = pose[b].q.y; world[b].q[3] = pose[b].q.z;
	}
	if (unbuilt)
	{
		double rest[3] = { at[0], at[1], at[2] };
		float restYaw = 0;
		sim.restPoint(id, rest, restYaw);	// the sim's own rest point and lying direction; the node and yaw 0 when it sent none
		layDown(world, rootBody(*d), rest, restYaw);
	}
	// MINE: a body already lying (the interim lying pose, or a late cue) starts with its parts inside the terrain, and Jolt pushes it down
	// through the height field; lift the pose so its lowest bone is kLift over the ground first (the laid-down pose included).
	double lift = 0;
	if (terrain) { for (const PhysicsWorld::Pose& r : world) { lift = std::max(lift, terrain->terrainHeight(r.p[0], r.p[2]) + kLift - r.p[1]); } }
	for (PhysicsWorld::Pose& r : world) { r.p[1] += lift; }
	world_.setRagdollPose(l.rag, world);
	l.started = updates_;
	live_[id] = std::move(l);
	started_++;
	// A lying pose the client could not build (a race with no lying clip, plans/animal-down) is carried by this ragdoll from now on: SimView commits
	// it as lying, which kept the layer-null rule until a body existed to draw.
	commitLying(sim, chars, id);
	return true;
}

// A body already lying when the client first sees it (a late joiner, CLIENT_GOALS "Ragdolls" Settling): the lying pose turned to the sim's
// rest yaw, at its rest point, laid on the floor and put to sleep at once - no fall, nothing to watch settle.
bool Ragdolls::startLying(uint64_t id, SimView& sim, Characters& chars, const kworld::Heightmap* terrain, const double rest[3], float restYaw)
{
	const int index = sim.charIndex(id);
	const kworld::Skeleton* sk = index >= 0 ? chars.skeleton((size_t)index) : nullptr;
	if (!sk || !std::isfinite(rest[0]) || !std::isfinite(rest[1]) || !std::isfinite(rest[2])) { return false; }
	const std::string file = fileFor(id, sim, chars, (size_t)index);
	if (file.empty()) { return false; }
	const ragdoll::Def* d = def(file);
	if (!d) { return false; }
	Live l;
	l.file = file;
	l.bones = ragdoll::boneMap(*d, *sk);
	std::vector<Characters::RagBone> pose(l.bones.size());
	for (size_t b = 0; b < l.bones.size(); b++)
	{
		if (l.bones[b] < 0) { return false; }
		pose[b].bone = l.bones[b];
	}
	const double floor = restFloor(terrain, rest);
	const double pos[3] = { rest[0], floor, rest[2] };
	if (!chars.ragdollStartAt((size_t)index, pose, restYaw, pos)) { return false; }	// rebuilt this update: no pose yet, retried next frame
	l.rag = world_.addRagdoll(*d, pos, restYaw);
	if (!l.rag) { return false; }
	double lift = 0;
	for (const Characters::RagBone& r : pose) { lift = std::max(lift, floor + kSettledLift - r.p[1]); }
	std::vector<PhysicsWorld::Pose> world(pose.size());
	for (size_t b = 0; b < pose.size(); b++)
	{
		for (int k = 0; k < 3; k++) { world[b].p[k] = pose[b].p[k] + (k == 1 ? lift : 0.0); }
		world[b].q[0] = pose[b].q.w; world[b].q[1] = pose[b].q.x; world[b].q[2] = pose[b].q.y; world[b].q[3] = pose[b].q.z;
	}
	world_.setRagdollPose(l.rag, world);
	world_.sleepRagdoll(l.rag);
	l.started = updates_;
	l.settled = true;
	live_[id] = std::move(l);
	started_++;
	late_++;
	return true;
}

// The rest-point pull: after the free fall, the root body's velocity is driven towards the sim's rest point so it arrives at kSettle (or
// reaches it in the overtime), then every body sleeps. The target's height is the floor under the rest point (the sim's y is a coarse fit, as for the feet) plus the root
// box's half thickness; a body the pull drags into the ground is stopped by the contact, so the y term cannot bury it.
// Returns false when the ragdoll's root has gone non-finite (a physics blow-up at a high game speed): the caller drops it - a NaN pose
// would drive the skeleton and the ground fit, and the heightmap throws on a non-finite point.
bool Ragdolls::pull(uint64_t id, Live& l, SimView& sim, const kworld::Heightmap* terrain, double h)
{
	l.age += h;
	if (l.settled || l.age < kFreeFall) { return true; }
	double rest[3];
	float restYaw;
	if (!sim.restPoint(id, rest, restYaw)) { return true; }	// no rest point on the wire (an older server): leave the body to itself
	const ragdoll::Def* d = def(l.file);
	const std::vector<uint32_t> bodies = world_.ragdollBodies(l.rag);
	PhysicsWorld::Body b;
	const int root = d ? rootBody(*d) : -1;
	if (!d || root < 0 || (size_t)root >= bodies.size() || !world_.body(bodies[(size_t)root], b)) { l.settled = true; return true; }
	if (!std::isfinite(rest[0]) || !std::isfinite(rest[1]) || !std::isfinite(rest[2]) || !std::isfinite(b.p[0]) || !std::isfinite(b.p[1])
		|| !std::isfinite(b.p[2]) || !std::isfinite(b.v[0]) || !std::isfinite(b.v[1]) || !std::isfinite(b.v[2]))
	{
		fprintf(stderr, "ragdolls: %llu went non-finite (root %.3f %.3f %.3f), dropping the ragdoll\n", (unsigned long long)id, b.p[0], b.p[1], b.p[2]);
		return false;
	}
	// x and z are the sim's rest point; the height keeps the root's own height over the floor (the rest point's height is the floor and
	// its sim y is a coarse fit), so a body dragged onto a deck rises with it and one on open ground is neither pressed down nor lifted.
	const double floor = restFloor(terrain, rest);
	const double over = std::clamp(b.p[1] - (terrain ? terrain->terrainHeight(b.p[0], b.p[2]) : 0.0), 0.5, 3.0);
	const double target[3] = { rest[0], floor + over, rest[2] };
	const double arc = std::sqrt((target[0] - b.p[0]) * (target[0] - b.p[0]) + (target[1] - b.p[1]) * (target[1] - b.p[1]) + (target[2] - b.p[2]) * (target[2] - b.p[2]));
	if (!l.pulling)
	{
		l.pulling = true;
		for (int k = 0; k < 3; k++) { l.pullFrom[k] = b.p[k]; }
		world_.setRagdollFriction(l.rag, 0);	// the drag: at the scene's friction 4 the ground pins the body and no pull moves it
		fprintf(stderr, "ragdolls: %llu pull starts %.2f u from the rest point (root %.2f %.2f %.2f)\n", (unsigned long long)id, arc, b.p[0], b.p[1], b.p[2]);
	}
	const double across = std::hypot(target[0] - b.p[0], target[2] - b.p[2]);
	constexpr double kEnd = kSettle + kOvertime + kGlide;
	if (l.age >= kSettle + kOvertime && across > kReach && l.age < kEnd)
	{
		const double f = std::min(1.0, h / (kEnd - l.age + h));
		const double d[3] = { (target[0] - b.p[0]) * f, 0.0, (target[2] - b.p[2]) * f };
		for (uint32_t body : bodies) { world_.shiftBody(body, d); }
		return true;
	}
	if (l.age >= kSettle && (across <= kReach || l.age >= kEnd))
	{
		world_.setRagdollFriction(l.rag, -1);
		world_.sleepRagdoll(l.rag);
		l.settled = true;
		fprintf(stderr, "ragdolls: %llu settled %.2f u from the rest point, %.2f across, at %.2f s (root %.2f %.2f %.2f, target %.2f %.2f %.2f)\n",
			(unsigned long long)id, arc, across, l.age, b.p[0], b.p[1], b.p[2], target[0], target[1], target[2]);
		return true;
	}
	// The path eases in and out (smoothstep) so the drag starts and stops without a jerk; the root follows it on a short velocity servo.
	const double u = std::clamp((l.age - kFreeFall) / (kSettle - kFreeFall), 0.0, 1.0);
	const double s = u * u * (3.0 - 2.0 * u);
	// The servo's lag spans ~3 fixed 1/60 s steps: the pull runs one substep at a time whatever the game speed (PhysicsWorld::stepFixed), so
	// the gain never goes over one step - a lag set from a whole 16x frame overshot the path each frame and pumped the joints.
	float dv[3];
	for (int k = 0; k < 3; k++)
	{
		const double want = std::clamp((l.pullFrom[k] + (target[k] - l.pullFrom[k]) * s - b.p[k]) / kTrack, -kPullTop, kPullTop);
		dv[k] = (float)(want - b.v[k]);
	}
	// The horizontal change goes to every body: on the root alone the joints shared it out among the heavier limbs and the frictionless body
	// slid downhill against the servo (wake2: the root held -10 u/s in x while each step asked +30, ending 15 u off). The same change on all
	// keeps their relative motion, so the limbs still swing and drag; the height term stays on the root and gravity keeps the rest.
	const float flat[3] = { dv[0], 0.0f, dv[2] };
	for (size_t k = 0; k < bodies.size(); k++) { world_.kick(bodies[k], (int)k == root ? dv : flat); }
	return true;
}

// A carried animal (P4): its race's ordinary ragdoll from this frame's pose, the anchor body's pose kept in the carrier's shoulder frame (the pin).
// False: not posed yet, no ragdoll or no shoulder (it keeps its frozen pose).
bool Ragdolls::startCarried(uint64_t id, uint64_t carrier, SimView& sim, Characters& chars)
{
	const int index = sim.charIndex(id), ci = sim.charIndex(carrier);
	const kworld::Skeleton* sk = index >= 0 ? chars.skeleton((size_t)index) : nullptr;
	double ap[3];
	kworld::SkelQuat aq;
	if (!sk || ci < 0 || !chars.carryFrame((size_t)ci, ap, aq)) { return false; }
	const std::string file = fileFor(id, sim, chars, (size_t)index);
	if (file.empty()) { return false; }
	const ragdoll::Def* d = def(file);
	if (!d) { return false; }
	Live l;
	l.file = file;
	l.carrier = carrier;
	l.bones = ragdoll::boneMap(*d, *sk);
	std::vector<Characters::RagBone> pose(l.bones.size());
	for (size_t b = 0; b < l.bones.size(); b++)
	{
		if (l.bones[b] < 0) { fprintf(stderr, "ragdolls: %s: body %s has no bone in the skeleton\n", file.c_str(), d->bodies[b].bone.c_str()); return false; }
		pose[b].bone = l.bones[b];
	}
	if (!chars.ragdollStart((size_t)index, pose)) { return false; }
	double at[3];
	chars.nodePos((size_t)index, at);
	l.rag = world_.addRagdoll(*d, at, 0.0f);
	if (!l.rag) { return false; }
	const kworld::SkelQuat inv = conj(aq);
	std::vector<PhysicsWorld::Pose> world(pose.size());
	l.hang.resize(pose.size());
	for (size_t b = 0; b < pose.size(); b++)
	{
		for (int k = 0; k < 3; k++) { world[b].p[k] = pose[b].p[k]; }
		world[b].q[0] = pose[b].q.w; world[b].q[1] = pose[b].q.x; world[b].q[2] = pose[b].q.y; world[b].q[3] = pose[b].q.z;
		const kworld::SkelVec3 o = kworld::rotate(inv, { (float)(pose[b].p[0] - ap[0]), (float)(pose[b].p[1] - ap[1]), (float)(pose[b].p[2] - ap[2]) });
		const kworld::SkelQuat q = kworld::multiply(inv, pose[b].q);
		l.hang[b].p[0] = o.x; l.hang[b].p[1] = o.y; l.hang[b].p[2] = o.z;
		l.hang[b].q[0] = q.w; l.hang[b].q[1] = q.x; l.hang[b].q[2] = q.y; l.hang[b].q[3] = q.z;
	}
	const int ab = sk->bone(chars.carryBone((size_t)index));
	l.anchor = rootBody(*d);
	for (size_t b = 0; b < l.bones.size(); b++) { if (ab >= 0 && l.bones[b] == ab) { l.anchor = (int)b; break; } }
	world_.setRagdollPose(l.rag, world);
	l.anchored = true;
	for (int k = 0; k < 3; k++) { l.anchorP[k] = ap[k]; }
	l.anchorQ = aq;
	l.started = updates_;
	live_[id] = std::move(l);
	started_++;
	carried_++;
	return true;
}

std::vector<PhysicsWorld::Pose> Ragdolls::hangPose(const Live& l) const
{
	std::vector<PhysicsWorld::Pose> out(l.hang.size());
	for (size_t b = 0; b < l.hang.size(); b++)
	{
		const PhysicsWorld::Pose& h = l.hang[b];
		const kworld::SkelVec3 o = kworld::rotate(l.anchorQ, { (float)h.p[0], (float)h.p[1], (float)h.p[2] });
		const kworld::SkelQuat q = kworld::multiply(l.anchorQ, { h.q[0], h.q[1], h.q[2], h.q[3] });
		out[b].p[0] = l.anchorP[0] + o.x; out[b].p[1] = l.anchorP[1] + o.y; out[b].p[2] = l.anchorP[2] + o.z;
		out[b].q[0] = q.w; out[b].q[1] = q.x; out[b].q[2] = q.y; out[b].q[3] = q.z;
	}
	return out;
}

// The anchor pin's one fixed step: the anchor body is moved (kinematic, over the step) to its place at the carrier's shoulder (this update's anchor);
// the other bodies are the physics world's.
void Ragdolls::hang(uint64_t id, Live& l, double h)
{
	l.age += h;
	const std::vector<uint32_t> bodies = world_.ragdollBodies(l.rag);
	PhysicsWorld::Body b;
	const size_t a = (size_t)l.anchor;
	if (!l.anchored || a >= bodies.size() || bodies.size() != l.hang.size() || !world_.body(bodies[a], b)) { return; }
	if (!std::isfinite(b.p[0]) || !std::isfinite(b.p[1]) || !std::isfinite(b.p[2]))
	{
		fprintf(stderr, "ragdolls: %llu (carried) went non-finite, dropping the ragdoll\n", (unsigned long long)id);
		world_.removeRagdoll(l.rag);
		l.dead = true;
		return;
	}
	const std::vector<PhysicsWorld::Pose> want = hangPose(l);
	const PhysicsWorld::Pose& t = want[a];
	const double gap = std::sqrt((t.p[0] - b.p[0]) * (t.p[0] - b.p[0]) + (t.p[1] - b.p[1]) * (t.p[1] - b.p[1]) + (t.p[2] - b.p[2]) * (t.p[2] - b.p[2]));
	if (gap > kHangReset)
	{
		world_.unpinBody(bodies[a]);
		world_.setRagdollPose(l.rag, want);
		l.resets++;
		fprintf(stderr, "ragdolls: %llu (carried) %.1f u off the shoulder: put back\n", (unsigned long long)id, gap);
	}
	world_.pinBody(bodies[a], t.p, t.q, h);
	const kworld::SkelQuat e = kworld::multiply({ t.q[0], t.q[1], t.q[2], t.q[3] }, conj({ b.q[0], b.q[1], b.q[2], b.q[3] }));
	l.gap = gap;
	l.angle = 2.0 * std::atan2(std::sqrt((double)e.x * e.x + (double)e.y * e.y + (double)e.z * e.z), std::fabs((double)e.w)) * 57.29578;
}

// The fixed physics step's hook: one 1/60 s pull step per live body, ahead of the substep it drives. The pair of a body that went non-finite is
// removed here, between two physics updates, so no step simulates it; update() clears its character.
void Ragdolls::substep(SimView& sim, const kworld::Heightmap* terrain, double h)
{
	for (auto& [id, l] : live_)
	{
		if (l.dead) { continue; }
		if (l.carrier) { hang(id, l, h); continue; }
		if (!sim.lying(id)) { continue; }
		if (!pull(id, l, sim, terrain, h))
		{
			world_.removeRagdoll(l.rag);
			l.dead = true;
		}
	}
}

// The budget (P5): past the range (horizontal, as the interest's send filter) or with the cap's worth of lying bodies awake. A race the client could
// not build lying is never refused: the ragdoll is its only lying pose.
bool Ragdolls::overBudget(uint64_t id, SimView& sim, const double eye[3]) const
{
	if (sim.lyingUnbuilt(id)) { return false; }
	if (pastRange(sim, id, eye)) { return true; }
	int awake = 0;
	for (const auto& [other, l] : live_)
	{
		if (!l.carrier && !l.dead && world_.ragdollAwake(l.rag)) { awake++; }
	}
	return awake >= kRagdollCap.get();
}

// Settled lying bodies woken again (a contact: a body dropped on the pile) sleep where they lie when past the range, and over the cap the oldest
// first. A body still falling or pulled is left to finish (1.5-3 s); carried bodies are not counted (their servo keeps them awake).
void Ragdolls::budget(SimView& sim, const double eye[3])
{
	std::vector<std::pair<uint64_t, Live*>> awake;	// started, settled and awake
	int count = 0;
	for (auto& [id, l] : live_)
	{
		if (l.carrier || l.dead || !world_.ragdollAwake(l.rag)) { continue; }
		count++;
		if (!l.settled) { continue; }
		if (pastRange(sim, id, eye))
		{
			world_.sleepRagdoll(l.rag);
			budgetSlept_++;
			count--;
			continue;
		}
		awake.push_back({ l.started, &l });
	}
	std::sort(awake.begin(), awake.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	for (size_t n = 0; n < awake.size() && count > kRagdollCap.get(); n++, count--)
	{
		world_.sleepRagdoll(awake[n].second->rag);
		budgetSlept_++;
	}
}

void Ragdolls::update(SimView& sim, Characters& chars, const kworld::Heightmap* terrain, const double eye[3])
{
	updates_++;
	std::vector<std::pair<SimView::RagdollCue, int>> cues = std::exchange(waiting_, {});
	waitingAt_.clear();
	for (const SimView::RagdollCue& cue : sim.ragdollCues) { cues.push_back({ cue, kTries }); }
	sim.ragdollCues.clear();
	if (!kRagdolls.get()) { cues.clear(); }
	std::set<uint64_t> cued;
	for (const auto& [cue, tries] : cues)
	{
		cued.insert(cue.id);
		bool retry = false;
		if (!live_.count(cue.id) && overBudget(cue.id, sim, eye))
		{
			unbudgeted_++;
			if (pastRange(sim, cue.id, eye)) { farKO_.insert(cue.id); }	// a late joiner once in range: a settled ragdoll at its rest point
			continue;
		}
		if (!live_.count(cue.id) && !start(cue.id, sim, chars, terrain, retry))
		{
			if (retry && tries > 1) { waiting_.push_back({ cue, tries - 1 }); }	// the cue waits for the character's first pose or its floor
			else if (retry)
			{
				refused_++;	// keeps the lying pose
				fprintf(stderr, "ragdolls: %llu refused: %s\n", (unsigned long long)cue.id, why_.c_str());
			}
			continue;
		}
		const Live& l = live_[cue.id];
		const ragdoll::Def* d = def(l.file);
		const std::vector<uint32_t> bodies = world_.ragdollBodies(l.rag);
		if (!d || bodies.empty() || cue.strength <= 0) { continue; }
		const float dv[3] = { cue.dir[0] * cue.strength * kKickPerStrength, cue.dir[1] * cue.strength * kKickPerStrength, cue.dir[2] * cue.strength * kKickPerStrength };
		bool mapped = false;
		const int body = partBody(*d, cue.part, mapped);
		// an animal's LOCATIONAL_DAMAGE part names are not in the human table yet: the kick lands on the pelvis, and each name is logged once per race
		if (!mapped && !cue.part.empty() && partLogged_.insert({ chars.nameOf(sim.raceOf(cue.id)), cue.part }).second)
		{
			fprintf(stderr, "ragdolls: %llu %s: hit part \"%s\" has no body: the kick lands on the pelvis\n", (unsigned long long)cue.id, chars.nameOf(sim.raceOf(cue.id)).c_str(), cue.part.c_str());
		}
		world_.kick(bodies[(size_t)body], dv);
	}
	// Late joiners (CLIENT_GOALS "Ragdolls" Settling): a body the client first sees already lying starts settled at the rest point and rest
	// yaw, no fall. One it saw standing keeps the interim pose if it goes down without an EVENT 48 (no re-settling on a re-entry: the ids
	// that left the client are forgotten below). A race with no lying clip has no pose to settle: its body falls from the pose it was built
	// in (start) and the pull lays it at the rest point, and the ragdoll then carries the lying state (SimView::ragdollStarted).
	const std::vector<uint64_t>& ids = sim.characterIds();
	for (auto it = seen_.begin(); it != seen_.end();)
	{
		if (std::find(ids.begin(), ids.end(), *it) == ids.end()) { it = seen_.erase(it); } else { ++it; }
	}
	for (auto it = farKO_.begin(); it != farKO_.end();)
	{
		if (std::find(ids.begin(), ids.end(), *it) == ids.end()) { it = farKO_.erase(it); } else { ++it; }
	}
	for (auto it = carriedSince_.begin(); it != carriedSince_.end();)
	{
		if (std::find(ids.begin(), ids.end(), it->first) == ids.end()) { it = carriedSince_.erase(it); } else { ++it; }
	}
	if (kRagdolls.get())
	{
		for (uint64_t id : ids)
		{
			// carried (P4): an animal's ordinary ragdoll starts from its frozen start pose, posed on the shoulder the update after the state is built
			if (const uint64_t carrier = sim.carrier(id))
			{
				seen_.insert(id);
				farKO_.erase(id);
				const auto lv = live_.find(id);
				// A carried HUMANOID is pose + bone pin, never simulation (rwh.98 1-3): `carry me` is its only clip and the node is set from the
				// carrier's upper arm each frame (Characters::carryPlace), so it does not swing or touch the ground. A ragdoll still live for the
				// body (the KO it was drawn as before the lift) goes and the animation takes it back. Only animals are a carried ragdoll (rwh.98 4).
				const int index = sim.charIndex(id);
				if (index < 0 || !chars.animal((size_t)index))
				{
					carriedSince_.erase(id);
					if (lv != live_.end())
					{
						world_.removeRagdoll(lv->second.rag);
						if (index >= 0) { chars.setRagdoll((size_t)index, {}); }
						live_.erase(lv);
					}
					continue;
				}
				if (lv != live_.end() && lv->second.carrier == carrier) { carriedSince_.erase(id); continue; }
				const auto since = carriedSince_.emplace(id, updates_).first;
				// past the range it keeps its frozen start pose (animated, so its ragdoll starts from the same pose once in range)
				if (lv == live_.end() && since->second < updates_ && !pastRange(sim, id, eye) && startCarried(id, carrier, sim, chars)) { carriedSince_.erase(since); }
				continue;
			}
			carriedSince_.erase(id);
			const bool unbuilt = sim.lyingUnbuilt(id);
			if (!sim.lying(id) && !unbuilt)
			{
				seen_.insert(id);
				double r[3];
				float y;
				if (!sim.restPoint(id, r, y)) { farKO_.erase(id); }	// up on the wire (not merely before SimView commits the KO): a later KO is cued afresh
				continue;
			}
			if (live_.count(id) || (seen_.count(id) && !farKO_.count(id)) || cued.count(id)) { continue; }
			if (pastRange(sim, id, eye)) { continue; }	// past the range: the lying pose stands (a ragdoll starts settled once it is in range)
			double rest[3];
			float restYaw;
			if (!sim.restPoint(id, rest, restYaw)) { continue; }
			if (!unbuilt) { if (startLying(id, sim, chars, terrain, rest, restYaw)) { farKO_.erase(id); } }	// no pose yet on its first frame: retried next update
			else
			{
				bool retry = false;
				start(id, sim, chars, terrain, retry);	// no lying clip: the standing pose falls and settles (retried while unbuilt)
			}
		}
	}
	budget(sim, eye);
	std::vector<PhysicsWorld::Pose> world;
	for (auto it = live_.begin(); it != live_.end();)
	{
		Live& l = it->second;
		const int index = sim.charIndex(it->first);
		if (l.dead)	// the root went non-finite in a physics step (substep): the lying animation carries the body instead
		{
			if (index >= 0) { chars.setRagdoll((size_t)index, {}); }
			it = live_.erase(it);
			continue;
		}
		// Carried (P4): the shoulder this update for the servo (the carrier's pose of the last drawn frame). The carry ended: put down or dropped,
		// the body becomes a lying ragdoll where it is (fall, then the rest-point pull, as a fresh KO); picked up by another or up: it goes.
		if (l.carrier && index >= 0)
		{
			const uint64_t carrier = sim.carrier(it->first);
			if (carrier == l.carrier)
			{
				const int ci = sim.charIndex(carrier);
				l.anchored = ci >= 0 && chars.carryFrame((size_t)ci, l.anchorP, l.anchorQ);
			}
			else if (!carrier && sim.lying(it->first))
			{
				fprintf(stderr, "ragdolls: %llu put down: the carried ragdoll falls\n", (unsigned long long)it->first);
				// The body falls from where it hangs, at rest: the shoulder servo's last velocities (~50 u/s where the carrier stands at a pile and
				// the servo pushes the body against it) would fling it 12-24 u off the rest point, past what the pull covers (kenshi-k16.2.38).
				for (const uint32_t body : world_.ragdollBodies(l.rag))
				{
					world_.unpinBody(body);
					PhysicsWorld::Body rb;
					if (!world_.body(body, rb)) { continue; }
					const float dv[3] = { (float)-rb.v[0], (float)-rb.v[1], (float)-rb.v[2] }, dw[3] = { (float)-rb.w[0], (float)-rb.w[1], (float)-rb.w[2] };
					world_.kick(body, dv, dw);
				}
				l.carrier = 0;
				l.hang.clear();
				l.anchored = false;
				l.age = 0;
				l.started = updates_;
			}
			else if (!carrier && ++l.dropping <= kDropWait) { }	// the KO flag may follow the carry's end by an update: keep the body meanwhile
			else
			{
				world_.removeRagdoll(l.rag);
				chars.setRagdoll((size_t)index, {});
				it = live_.erase(it);
				continue;
			}
		}
		// A body started before SimView saw the KO (the event can precede the flag): an unbuildable lying state is committed now instead of the
		// pair being dropped on the very frame SimView reports it (a race with a lying clip commits itself when the build succeeds).
		if (index >= 0 && !l.carrier && !sim.lying(it->first)) { commitLying(sim, chars, it->first); }
		// The sim no longer has it lying (woke, picked up, gone): animation takes it back (Characters::rise blends a get-up from the pose).
		// A cue that started it this update is kept even before SimView sees it lying (the event follows the KO in the same tick).
		if (index < 0 || (!l.carrier && !sim.lying(it->first) && l.started != updates_))
		{
			world_.removeRagdoll(l.rag);
			if (index >= 0) { chars.setRagdoll((size_t)index, {}); }
			it = live_.erase(it);
			continue;
		}
		if (world_.ragdollPose(l.rag, world))
		{
			std::vector<Characters::RagBone> v(world.size());
			for (size_t b = 0; b < world.size(); b++)
			{
				v[b].bone = l.bones[b];
				for (int k = 0; k < 3; k++) { v[b].p[k] = world[b].p[k]; }
				v[b].q = { world[b].q[0], world[b].q[1], world[b].q[2], world[b].q[3] };
			}
			chars.setRagdoll((size_t)index, std::move(v));
		}
		++it;
	}
}

void Ragdolls::clear()
{
	for (const auto& [id, l] : live_) { world_.removeRagdoll(l.rag); }
	live_.clear();
	waiting_.clear();
	waitingAt_.clear();
	carriedSince_.clear();
	seen_.clear();
	farKO_.clear();
}

std::string Ragdolls::report() const
{
	std::string out;
	char buf[400];
	snprintf(buf, sizeof buf, "ragdolls: %zu live, %llu started, %llu late (settled on arrival), %llu carried, %llu refused (no floor or pose), "
		"budget: %llu KOs kept the lying pose, %llu put back to sleep\n", live_.size(), (unsigned long long)started_, (unsigned long long)late_,
		(unsigned long long)carried_, (unsigned long long)refused_, (unsigned long long)unbudgeted_, (unsigned long long)budgetSlept_);
	out += buf;
	for (const auto& [id, l] : live_)
	{
		char carried[96] = "";
		if (l.carrier) { snprintf(carried, sizeof carried, " carried by %llu, %llu resets, off %.1f u %.0f deg", (unsigned long long)l.carrier, (unsigned long long)l.resets, l.gap, l.angle); }
		const std::vector<uint32_t> bodies = world_.ragdollBodies(l.rag);
		PhysicsWorld::Body b;
		char at[80] = "";
		if (!bodies.empty() && world_.body(bodies[0], b)) { snprintf(at, sizeof at, " body0 at %.1f %.1f %.1f", b.p[0], b.p[1], b.p[2]); }
		snprintf(buf, sizeof buf, "  %llu %s rag %u %s age %.2f%s%s%s\n", (unsigned long long)id, l.file.c_str(), l.rag,
			world_.ragdollAwake(l.rag) ? "awake" : "asleep", l.age, l.settled ? " settled" : "", carried, at);
		out += buf;
	}
	return out;
}
