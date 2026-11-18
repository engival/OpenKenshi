#pragma once

// The movement step (docs/formats/SIMLINK.md section 12): advance a character along a route, or
// along a heading (direct drive), staying on the navmesh. Sliding along edges, stopping at
// walls and cliffs, and the surface height all come from the mesh.
//
// Self-contained on purpose: it needs a NavMesh and nothing of the simulation (no flecs, no world),
// so a client can link it and predict a driven character with the very code the server runs.
// Deterministic: a pure function of the mesh and its arguments. A Walker holds Detour query state
// (one per thread); the NavMesh must outlive it and stay unchanged while it steps.

#include "knav/navmesh.hpp"

#include <cmath>
#include <cstddef>
#include <functional>
#include <optional>

namespace knav {

struct Position
{
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct StepResult
{
	Position at; // where the character ended up (the start when it was off the mesh)
	bool onMesh = true; // false: the start was not on the mesh and nothing moved (see Walker::snap)
	bool blocked = false; // an edge, wall or cliff stopped some of the move
	double moved = 0.0; // xz distance actually covered
};

enum class Drive : std::uint8_t
{
	Stop = 0,
	Forward = 1,
	Backward = 2,
};

// Backward speed as a fraction of the forward speed. UNSURE: not measured (SIMLINK.md section 12).
inline constexpr double kBackpedalFactor = 0.5;

// Sideways speed as a fraction of the gait (a strafe step). MINE / UNSURE, like the backpedal's: no per-human strafe factor in the
// game data; tuned on playtest (SIMLINK.md section 12, k16.8.15).
inline constexpr double kStrafeFactor = 0.5;

// The local move vector of a drive state and a strafe (-1 left, 0, +1 right of the heading) as a fraction of the gait:
// forward +1 / back -kBackpedalFactor, side +-kStrafeFactor, normalised to length <= 1 so a diagonal is never faster than the gait.
struct DriveLocal
{
	double forward = 0.0;
	double side = 0.0; // towards heading + 90 degrees (right)
};
DriveLocal driveLocal(Drive mode, int strafe) noexcept;

// ---- the gait: how fast a character walks, and how fast it gets there ----
//
// One definition for both sides of the wire: the server steps with it and the client predicts with it, so a restart
// (a stop-start) does not run ahead of the server. The speed does not jump to the gait speed; it ramps at the
// acceleration: the original integrates a velocity vector clamped to `accel x dt` per frame, accel = the RACE `pathfind
// acceleration` (Human 20 Havok = 200 u/s^2), with no separate deceleration (kinspect s70 section 2, move_kinematics.md;
// docs/ARCHITECTURE.md "Walker kinematics"). Retired: the fitted 15 u/s^2 ramp.
inline constexpr double kGaitAcceleration = 200.0; // units/s per second (Human)
inline constexpr double kAccelerationScale = 10.0; // RACE `pathfind acceleration` is in Havok units (world x 0.1)

// The body yaw chases its target at 20 rad/s, 4 in the combat-ready stance (AnimationClassHuman::vf8, s70 section 3).
inline constexpr double kTurnRate = 20.0;
inline constexpr double kTurnRateCombat = 4.0;

// The funnel pushes a corner out by the footprint radius + 0.5 Havok (s70 section 4): 4 + 5 = 9 units for a human (the live radius
// is 0.4 Havok, havokchar README).
inline constexpr double kCornerMargin = 5.0;

// What fixes the target speed. `athletics` is the effective athletics skill already (hunger, limbs and the encumbrance term
// of StatsBlock::getEffectiveStat applied by the caller); the race supplies the rest.
struct Gait
{
	double speedMinSkill = 70.0; // RACE `speed min skill`: run speed at athletics 1
	double speedMaxSkill = 120.0; // RACE `speed max skill`: run speed at athletics 100
	double walkSpeed = 15.0; // RACE `walk speed`
	double athletics = 1.0; // the effective athletics skill
	double acceleration = kGaitAcceleration; // RACE `pathfind acceleration` x kAccelerationScale
	double footprint = 4.0; // RACE `pathfind footprint radius` (live human 4.0 = HavokCharacter radius 0.4 x 10): the corner clearance is footprint + kCornerMargin
	bool run = true; // the run flag (CHAR_DYNAMIC flag bit 12); clear = the walk speed, never faster than the run
	// CharStats+0x190, the encumbrance ratio 0..1 (1 = unladen): the run speed is (base - kSpeedFloor) x ratio + kSpeedFloor
	// (GATES_CARRY_STATIC.md section 5). Only a carried person sets it so far (Simulation::stepCarry); the default changes nothing.
	double encumbrance = 1.0;
	// The legs (task BH; kinspect `140884970`): min(left, right leg effective health / max health), clamped to 0..1. It
	// multiplies the encumbrance ratio on the excess over kSpeedFloor, so a leg at 0 (still above the KO point, the body
	// standing) leaves the 11 u/s floor exactly (BF live side fact); the default changes nothing.
	double legs = 1.0;
	// Sneaking (Character+0xd4, not in combat): k = clamp(stealth x mods x 0.01, 0, 1) (FUN_140884850); < 0 = not sneaking.
	// The run speed is capped by it (kinspect findings/sprint_anim.md "Sneak speed").
	double sneak = -1.0;
	// A hard cap on the aimed-for speed (u/s), 0 = none: Jog (55) and Group (the slowest member's run speed), SIMLINK
	// order 10 `which` 5. Walk uses `run` clear; Run has none.
	double cap = 0.0;
};

// The floor of the encumbered run speed (the exe's DAT_141723f4c, GATES_CARRY_STATIC.md section 5).
inline constexpr double kSpeedFloor = 11.0;

// Run speed: lerp((athletics - 1) / 99, speed min, speed max) (UNSURE: the athletics curve, ENGAGEMENT.md 9.3).
double runSpeed(const Gait& gait) noexcept;
// The speed a character aims for: the run speed, or min(run, walk speed) when the run flag is clear.
double gaitSpeed(const Gait& gait) noexcept;
// One step of the ramp: `current` moves towards `target` by at most accel * dt.
double rampSpeed(double current, double target, double dt, double accel = kGaitAcceleration) noexcept;
// `yaw` turned towards `target` (both radians, 0 = +z) by at most rate * dt; the result is wrapped to (-pi, pi].
double turnToward(double yaw, double target, double rate, double dt) noexcept;
// The walker's velocity slew (HavokCharacter::updateVelocity, HAVOKCHAR_EXE.md section 3): `vx, vz` moves towards the target
// velocity (`tx`, `tz`) by at most accel * dt, as a vector clamp on the difference (not a separate speed and turn rate).
struct Velocity
{
	double x = 0.0;
	double z = 0.0;
};
Velocity slewVelocity(Velocity v, double tx, double tz, double accel, double dt) noexcept;
// The speed a corner of `turn` radians (0 = straight on) may be taken at: sqrt(2 accel L) with L the lateral offset of the
// funnel's pushed-out corner from the heading line, clearance x cos(turn / 2). A bend under ~6 degrees is straight (0 =
// no limit, returned as +inf); a reversal is 0.
double cornerSpeed(double turn, double accel, double clearance) noexcept;

// The speed a corner of `turn` radians may be taken at by the vector-slew walker (D5, LOCOMOTION.md section 9). It steers at the vertex and turns after it:
// the velocity moves from v along the leg in to v along the leg out in a straight line in the vector plane (the slew's clamp), so it overshoots the
// vertex along the leg in by v^2 / accel x g(turn), g = 2 sin(turn/2) cos^2(turn/2) up to 90 degrees and 1 / (2 sin(turn/2)) above it (the velocity
// goes through zero: the stop distance at a reversal). The speed is the one whose overshoot is `clearance`, the route's own clearance beyond a vertex.
// A near-straight corner is no limit (+inf).
double arcCornerSpeed(double turn, double accel, double clearance) noexcept;

class Walker
{
public:
	explicit Walker(const NavMesh& mesh, TravelFilter travel = {});

	const NavMesh& mesh() const noexcept { return mesh_; }

	// Open ground: `ground(x, z)` is the terrain height (the heightmap the client draws with). A step that ends on mesh
	// within kGroundSnap of it has the terrain height as y, not the baked detail mesh's (which floats 0..4 u above a
	// slope, kenshi-k16.7.14); a floor, deck or stair further off keeps the mesh height. Unset: the mesh height always.
	static constexpr double kGroundSnap = 5.0;
	void setGround(std::function<double(double, double)> ground) { ground_ = std::move(ground); }
	// A placed point (a Walker::snap result) on open ground: the same rule a step applies. Within kGroundSnap of the terrain it takes
	// the terrain's height; a floor, deck or stair further off, or no ground set, keeps `p` (kenshi-k16.6.273).
	Position settle(Position p) const;

	// Moves by (dx, dz) in the xz plane, sliding along the mesh boundary. y is the mesh surface
	// height where it ends (or the terrain's on open ground, see setGround; `grounded` false, a character inside a building's
	// footprint, never takes it: floors keep the mesh height).
	StepResult step(const Position& from, double dx, double dz, bool grounded = true) const;

	// Direct drive for dt seconds at `speed` units/s along `heading` (radians, 0 = +z, increasing
	// towards +x, as the sim's yaw). Backward moves against the heading at kBackpedalFactor of the
	// speed; the facing is the caller's business. `strafe` (-1 left, +1 right of the heading, SIMLINK order 20) adds a sideways
	// move at kStrafeFactor, the whole normalised by driveLocal; Stop with a strafe is a pure sidestep. `speed` is the gait speed.
	StepResult drive(const Position& from, Drive mode, double heading, double speed, double dt, bool grounded = true, int strafe = 0) const;

	// The same with the gait ramp: `forwardSpeed` (signed, units/s along the facing; negative when backing up) is ramped
	// towards the gait speed (towards -gait x kBackpedalFactor backward; Stop zeroes it at once) and the step is taken at the new
	// speed. `forwardSpeed` is then the speed along the move direction (gait x the length of driveLocal), negative with a
	// backward component. The server and a predicting client both call this.
	StepResult drive(const Position& from, Drive mode, double heading, double gait, double& forwardSpeed, double dt,
		double accel = kGaitAcceleration, bool grounded = true, int strafe = 0) const;

	// Follows a route (any indexable sequence of points with .x and .z) for `distance` units of
	// path from waypoint `next`, which advances past every waypoint reached. Stops early when
	// blocked. A waypoint counts as reached when the step lands on it, so the route's end is
	// reached exactly, not approximately.
	template <typename Route>
	StepResult follow(const Position& from, const Route& route, std::size_t& next, double distance) const
	{
		StepResult result;
		result.at = from;
		while (next < route.size() && distance > 0.0)
		{
			const double dx = route[next].x - result.at.x;
			const double dz = route[next].z - result.at.z;
			const double toGo = std::hypot(dx, dz);
			if (toGo <= distance)
			{
				const StepResult hop = step(result.at, dx, dz);
				result.onMesh = hop.onMesh;
				result.at = hop.at;
				result.moved += hop.moved;
				if (!hop.onMesh)
					return result;
				if (hop.blocked)
				{
					result.blocked = true;
					return result;
				}
				distance -= toGo;
				++next;
			} else {
				const StepResult hop = step(result.at, dx / toGo * distance, dz / toGo * distance);
				result.onMesh = hop.onMesh;
				result.at = hop.at;
				result.moved += hop.moved;
				result.blocked = hop.blocked;
				return result;
			}
		}
		return result;
	}

	// The speed limit now, for a character at `from` heading for waypoint `next`: the lowest of vmax and, for every corner and the
	// end of the route within braking reach, sqrt(vCorner^2 + 2 accel distance) (the end's vCorner is 0), so it brakes at exactly
	// `accel` on a straight approach and slows for a sharp corner. Pure function of the route; does not move anything.
	// `firstLeg` (0..1) scales the first leg to a corner (see the loop): the share of the slew's per-tick budget left for braking while the velocity
	// is still turning towards it; `speed` is the walker's speed.
	template <typename Route>
	static double speedLimit(const Position& from, const Route& route, std::size_t next, double vmax, double accel, double clearance,
		bool arc = false, double lookahead = 0.0, double firstLeg = 1.0, double speed = 0.0)
	{
		double limit = vmax;
		double px = from.x;
		double pz = from.z;
		double along = 0.0;
		double turnLoss = 0.0;
		const double reach = vmax * vmax / (2.0 * accel) + clearance;
		for (std::size_t i = next; i < route.size(); ++i)
		{
			const double dx = route[i].x - px;
			const double dz = route[i].z - pz;
			const double seg = std::hypot(dx, dz);
			along += seg;
			double corner = 0.0;
			if (i + 1 < route.size())
			{
				const double ox = route[i + 1].x - route[i].x;
				const double oz = route[i + 1].z - route[i].z;
				const double outLen = std::hypot(ox, oz);
				if (seg < 1.0e-6 || outLen < 1.0e-6)
				{
					px = route[i].x;
					pz = route[i].z;
					continue; // a repeated point is no corner
				}
				const double turn = std::fabs(std::atan2(dx * oz - dz * ox, dx * ox + dz * oz));
				corner = cornerSpeed(turn, accel, clearance);
				// `arc`: the speed into the corner is the vertex-arc one (arcCornerSpeed), the turn read from the route's own legs, not from the line from
				// where the walker stands (which is off the leg once it cuts a corner and would read a milder turn).
				if (arc)
				{
					const double lx = i > 0 ? route[i].x - route[i - 1].x : dx;
					const double lz = i > 0 ? route[i].z - route[i - 1].z : dz;
					const double legTurn = std::hypot(lx, lz) > 1.0e-6 ? std::fabs(std::atan2(lx * oz - lz * ox, lx * ox + lz * oz)) : turn;
					corner = arcCornerSpeed(legTurn, accel, clearance);
				}
			}
			// `lookahead`: a tick's travel, taken off the distance to a corner or to the end: the brake is applied a tick late, so a corner
			// would otherwise be entered a tick of braking (accel x dt) too fast, and the end reached at that speed (kenshi-k16.8.9: the walk
			// arrived at ~26 u/s and the stop snap took it to 0 in one tick).
			// The first leg to a corner counts for `firstLeg` of its length while the walker (at `speed`) is above that corner's speed and has to brake
			// (below it nothing brakes, and the share would only throttle the run-up); the end of the route is not scaled: a stop is a stop however
			// the velocity points. The loss comes off every corner after it as well.
			if (i == next && i + 1 < route.size() && speed > corner)
				turnLoss = seg * (1.0 - firstLeg);
			limit = std::fmin(limit, std::sqrt(corner * corner + 2.0 * accel * std::fmax(0.0, along - turnLoss - lookahead)));
			if (along > reach)
				break;
			px = route[i].x;
			pz = route[i].z;
		}
		return limit;
	}

	// True when `p` is on the mesh (within a unit horizontally).
	bool onMesh(const Position& p) const;

	// The mesh surface point nearest `p` within `reach` on each axis, or empty. `distance` (if
	// given) receives the 3D distance from p to it. `reachY` >= 0 narrows the vertical window
	// (the nearest point on p's own level); negative = `reach` on every axis.
	std::optional<Position> snap(const Position& p, double reach, double* distance = nullptr, double reachY = -1.0) const;

	// The surface height at (x, z) nearest to height y within reach; empty off the mesh.
	std::optional<double> surfaceY(double x, double y, double z, double reach = 8.0) const;

private:
	const NavMesh& mesh_;
	TravelFilter travel_;
	NavQuery query_;
	std::function<double(double, double)> ground_;
};

} // namespace knav
