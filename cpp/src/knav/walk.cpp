#include "knav/walk.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace knav {

namespace {

// Where a standing character is looked up: the point is on the surface after every step, so a
// small box finds its polygon. Looser than that and a character a unit off the mesh is "on" it.
constexpr Point kStand{ 1.0f, 12.0f, 1.0f };
// A step longer than this is cut up: Detour's surface walk visits a bounded number of polygons.
constexpr double kMaxHop = 8.0;
// The body's wall room may be this much under the radius or the start's room (the float32 mesh, a slide along a wall).
constexpr float kClearanceSlack = 0.002f;
// A hop's height change beyond this plus kMaxGradient per unit walked is a cliff the mesh's polygon links cross (a ledge's edge links to the floor below
// across a drop of 30-66 u), not a stair or ramp: stair risers measure up to about 8 u within 0.3 u walked (k16.8.27), and the hop stops at the edge.
constexpr double kHopRise = 20.0;
constexpr double kHopOvershoot = 1.5; // the start snaps to the mesh by up to kStand (1 u a side, 1.42 diagonal) before the slide, which a hop may walk back; the float32 mesh blurs a few hundredths more
constexpr double kMaxGradient = 3.0;
// Layer continuity: a hop that lands more than this above or below where it started, with a surface within this reach of the starting height under the end
// point, stays on that surface (the rule surfaceY uses). A recoil step at a stair's edge took the floor layer under the stair, 9 u down (kenshi-e9s9.18).
constexpr double kLayerReach = 8.0;

Point toPoint(const Position& p)
{
	return { static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
}

} // namespace

Walker::Walker(const NavMesh& mesh, TravelFilter travel)
	: mesh_(mesh)
	, travel_(travel)
	, query_(mesh)
{
}

StepResult Walker::step(const Position& from, double dx, double dz, double radius, const std::vector<DoorKey>& crossing) const
{
	StepResult result;
	result.at = from;
	const double length = std::hypot(dx, dz);
	const int hops = std::max(1, static_cast<int>(std::ceil(length / kMaxHop)));
	// The wall room a hop must keep: the body's radius, or the start's own when it began nearer (a spawn beside a prop may move away or along, not in).
	float room = -1.0f;
	if (radius > 0.0)
	{
		const float start = query_.clearanceAt(toPoint(from), kStand, static_cast<float>(radius));
		room = start < 0.0f ? -1.0f : start - kClearanceSlack;
	}
	for (int i = 1; i <= hops; ++i)
	{
		// Aim at the true remainder each hop: the target is from + (dx, dz) * i / hops.
		const Point target{ static_cast<float>(from.x + dx * i / hops), static_cast<float>(result.at.y),
			static_cast<float>(from.z + dz * i / hops) };
		Point out;
		bool blocked = false;
		if (!query_.slide(toPoint(result.at), target, kStand, out, blocked, travel_, crossing))
		{
			result.onMesh = false;
			return result;
		}
		const double walked = std::hypot(out[0] - result.at.x, out[2] - result.at.z);
		// A hop never walks farther than it was aimed: the slide from a polygon corner has returned a point 8 u off a 0.7 u hop (ksim_fight_levels
		// seed 5, a backpedal into the Hub bar's ledge corner, k16.8.50); that result is a block at where the hop started.
		if (walked > std::hypot(target[0] - result.at.x, target[2] - result.at.z) + kHopOvershoot)
		{
			result.blocked = true;
			break;
		}
		if (std::fabs(out[1] - result.at.y) > kLayerReach)
		{
			Point near;
			if (query_.nearestPoint(toPoint({ out[0], result.at.y, out[2] }), { 1.0f, static_cast<float>(kLayerReach), 1.0f }, near))
				out[1] = near[1];
		}
		if (std::fabs(out[1] - result.at.y) > kHopRise + kMaxGradient * walked)
		{
			result.blocked = true;
			break;
		}
		if (room >= 0.0f && query_.clearanceAt(out, kStand, static_cast<float>(radius)) < room)
		{
			// Cut the hop back to the largest fraction that keeps the room (the slide is re-run to each trial end, so it stays on the mesh).
			Point best = toPoint(result.at);
			float lo = 0.0f;
			float hi = 1.0f;
			for (int k = 0; k < 6; ++k)
			{
				const float mid = 0.5f * (lo + hi);
				const Point trial{ best[0] + (out[0] - best[0]) * mid, best[1], best[2] + (out[2] - best[2]) * mid };
				Point at;
				bool hit = false;
				if (query_.slide(best, trial, kStand, at, hit, travel_, crossing) && query_.clearanceAt(at, kStand, static_cast<float>(radius)) >= room)
					lo = mid;
				else
					hi = mid;
			}
			if (lo > 0.0f)
			{
				const Point trial{ best[0] + (out[0] - best[0]) * lo, best[1], best[2] + (out[2] - best[2]) * lo };
				Point at;
				bool hit = false;
				if (query_.slide(best, trial, kStand, at, hit, travel_, crossing))
				{
					result.moved += std::hypot(at[0] - result.at.x, at[2] - result.at.z);
					result.at = { at[0], at[1], at[2] };
				}
			}
			result.blocked = true;
			break;
		}
		result.moved += walked;
		result.at = { out[0], out[1], out[2] };
		if (blocked)
		{
			result.blocked = true;
			break;
		}
	}
	return result;
}

double runSpeed(const Gait& gait) noexcept
{
	const double t = std::fmin(std::fmax((gait.athletics - 1.0) / 99.0, 0.0), 1.0);
	const double base = gait.speedMinSkill + (gait.speedMaxSkill - gait.speedMinSkill) * t;
	// The encumbrance ratio and the leg factor share the one product on the excess over the floor (task BH, `140884970`):
	// either at 0 leaves kSpeedFloor exactly (a leg at 0 health still above the KO point stands and goes at 11 u/s, live).
	const double ratio = std::fmin(std::fmax(gait.encumbrance, 0.0), 1.0) * std::fmin(std::fmax(gait.legs, 0.0), 1.0);
	const double run = (base - kSpeedFloor) * ratio + kSpeedFloor;
	if (gait.sneak < 0.0)
		return run;
	// x = run - 11; x' = min(x, 12 + t (x - 12)), t = max(0, (k - 0.1) / 0.8) unclamped above; speed x' + 11.
	const double x = run - kSpeedFloor;
	return std::fmin(x, 12.0 + std::fmax(0.0, (gait.sneak - 0.1) / 0.8) * (x - 12.0)) + kSpeedFloor;
}

double gaitSpeed(const Gait& gait) noexcept
{
	const double running = runSpeed(gait);
	const double speed = gait.run ? running : std::fmin(running, gait.walkSpeed);
	return gait.cap > 0.0 ? std::fmin(speed, gait.cap) : speed;
}

double rampSpeed(double current, double target, double dt, double accel) noexcept
{
	const double step = accel * dt;
	return current + std::fmin(std::fmax(target - current, -step), step);
}

Velocity slewVelocity(Velocity v, double tx, double tz, double accel, double dt) noexcept
{
	const double dx = tx - v.x;
	const double dz = tz - v.z;
	const double length = std::hypot(dx, dz);
	const double limit = accel * dt;
	if (length <= limit)
		return { tx, tz };
	return { v.x + dx / length * limit, v.z + dz / length * limit };
}

double turnToward(double yaw, double target, double rate, double dt) noexcept
{
	constexpr double kPi = 3.14159265358979323846;
	const auto wrap = [](double a)
	{
		while (a > kPi)
			a -= 2.0 * kPi;
		while (a <= -kPi)
			a += 2.0 * kPi;
		return a;
	};
	const double limit = rate * dt;
	return wrap(yaw + std::fmin(std::fmax(wrap(target - yaw), -limit), limit));
}

double cornerSpeed(double turn, double accel, double clearance) noexcept
{
	constexpr double kStraight = 0.1; // radians: a bend this small is no corner (collinear route points)
	if (turn < kStraight)
		return std::numeric_limits<double>::infinity();
	return std::sqrt(2.0 * accel * clearance * std::fmax(std::cos(turn * 0.5), 0.0));
}

double arcCornerSpeed(double turn, double accel, double clearance) noexcept
{
	constexpr double kStraight = 0.1; // radians: a bend this small is no corner (collinear route points)
	if (turn < kStraight)
		return std::numeric_limits<double>::infinity();
	const double half = std::fmin(turn, 3.14159265358979323846) * 0.5;
	const double overshoot = turn <= 1.57079632679489662 ? 2.0 * std::sin(half) * std::cos(half) * std::cos(half) : 0.5 / std::sin(half);
	return std::sqrt(accel * clearance / overshoot);
}

DriveLocal driveLocal(Drive mode, int strafe) noexcept
{
	DriveLocal v;
	v.forward = mode == Drive::Forward ? 1.0 : mode == Drive::Backward ? -kBackpedalFactor : 0.0;
	v.side = strafe < 0 ? -kStrafeFactor : strafe > 0 ? kStrafeFactor : 0.0;
	const double length = std::hypot(v.forward, v.side);
	if (length > 1.0)
	{
		v.forward /= length;
		v.side /= length;
	}
	return v;
}

StepResult Walker::drive(const Position& from, Drive mode, double heading, double gait, double& forwardSpeed, double dt, double accel, int strafe, double radius) const
{
	const DriveLocal local = driveLocal(mode, strafe);
	const double length = std::hypot(local.forward, local.side);
	if (length <= 0.0)
	{
		forwardSpeed = 0.0; // a stop is dead (the server's direct drive does the same); only a start ramps
		return drive(from, Drive::Stop, heading, 0.0, dt, 0, radius);
	}
	forwardSpeed = rampSpeed(forwardSpeed, local.forward < 0.0 ? -gait * length : gait * length, dt, accel);
	return drive(from, mode, heading, std::fabs(forwardSpeed) / length, dt, strafe, radius);
}

StepResult Walker::drive(const Position& from, Drive mode, double heading, double speed, double dt, int strafe, double radius) const
{
	const DriveLocal local = driveLocal(mode, strafe);
	if (local.forward == 0.0 && local.side == 0.0)
	{
		StepResult still;
		still.at = from;
		still.onMesh = onMesh(from);
		return still;
	}
	const double sn = std::sin(heading);
	const double cs = std::cos(heading);
	// Heading 0 = +z, increasing towards +x: forward = (sin, cos), and the body's right
	// (forward x up, Y-up right-handed) = (-cos, sin): -x at heading 0.
	const double dx = (sn * local.forward - cs * local.side) * speed * dt;
	const double dz = (cs * local.forward + sn * local.side) * speed * dt;
	return step(from, dx, dz, radius);
}

bool Walker::onMesh(const Position& p) const
{
	return query_.onMesh(toPoint(p), kStand);
}

bool Walker::inside(const Position& p) const
{
	constexpr float kInsideHair = 0.02f;
	Point out;
	return query_.nearestPoint(toPoint(p), { kInsideHair, 12.0f, kInsideHair }, out)
		&& std::hypot(out[0] - p.x, out[2] - p.z) <= kInsideHair;
}

std::optional<Position> Walker::snap(const Position& p, double reach, double* distance, double reachY) const
{
	const float r = static_cast<float>(reach);
	const float ry = reachY >= 0.0 ? static_cast<float>(reachY) : r;
	Point out;
	if (!query_.nearestPoint(toPoint(p), { r, ry, r }, out))
		return std::nullopt;
	if (distance != nullptr)
		*distance = std::sqrt((out[0] - p.x) * (out[0] - p.x) + (out[1] - p.y) * (out[1] - p.y)
			+ (out[2] - p.z) * (out[2] - p.z));
	return Position{ out[0], out[1], out[2] };
}

std::optional<double> Walker::surfaceY(double x, double y, double z, double reach) const
{
	const float r = static_cast<float>(reach);
	Point out;
	if (!query_.nearestPoint(toPoint({ x, y, z }), { 1.0f, r, 1.0f }, out))
		return std::nullopt;
	return out[1];
}

} // namespace knav
