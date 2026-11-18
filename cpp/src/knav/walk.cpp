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
// A hop's height change beyond this plus kMaxGradient per unit walked is a cliff the mesh's polygon links cross (a ledge's edge links to the floor below
// across a drop of 30-66 u), not a stair or ramp: stair risers measure up to about 8 u within 0.3 u walked (k16.8.27), and the hop stops at the edge.
constexpr double kHopRise = 20.0;
constexpr double kMaxGradient = 3.0;

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

StepResult Walker::step(const Position& from, double dx, double dz, bool grounded) const
{
	StepResult result;
	result.at = from;
	const double length = std::hypot(dx, dz);
	const int hops = std::max(1, static_cast<int>(std::ceil(length / kMaxHop)));
	for (int i = 1; i <= hops; ++i)
	{
		// Aim at the true remainder each hop: the target is from + (dx, dz) * i / hops.
		const Point target{ static_cast<float>(from.x + dx * i / hops), static_cast<float>(result.at.y),
			static_cast<float>(from.z + dz * i / hops) };
		Point out;
		bool blocked = false;
		if (!query_.slide(toPoint(result.at), target, kStand, out, blocked, travel_))
		{
			result.onMesh = false;
			return result;
		}
		const double walked = std::hypot(out[0] - result.at.x, out[2] - result.at.z);
		if (std::fabs(out[1] - result.at.y) > kHopRise + kMaxGradient * walked)
		{
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
	if (ground_ && grounded)
	{
		const double terrain = ground_(result.at.x, result.at.z);
		if (std::fabs(result.at.y - terrain) <= kGroundSnap)
			result.at.y = terrain;
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

StepResult Walker::drive(const Position& from, Drive mode, double heading, double gait, double& forwardSpeed, double dt, double accel, bool grounded, int strafe) const
{
	const DriveLocal local = driveLocal(mode, strafe);
	const double length = std::hypot(local.forward, local.side);
	if (length <= 0.0)
	{
		forwardSpeed = 0.0; // a stop is dead (the server's direct drive does the same); only a start ramps
		return drive(from, Drive::Stop, heading, 0.0, dt, grounded);
	}
	forwardSpeed = rampSpeed(forwardSpeed, local.forward < 0.0 ? -gait * length : gait * length, dt, accel);
	return drive(from, mode, heading, std::fabs(forwardSpeed) / length, dt, grounded, strafe);
}

StepResult Walker::drive(const Position& from, Drive mode, double heading, double speed, double dt, bool grounded, int strafe) const
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
	// Heading 0 = +z, increasing towards +x: forward = (sin, cos), right = (cos, -sin).
	const double dx = (sn * local.forward + cs * local.side) * speed * dt;
	const double dz = (cs * local.forward - sn * local.side) * speed * dt;
	return step(from, dx, dz, grounded);
}

bool Walker::onMesh(const Position& p) const
{
	return query_.onMesh(toPoint(p), kStand);
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

Position Walker::settle(Position p) const
{
	if (ground_)
	{
		const double terrain = ground_(p.x, p.z);
		if (std::fabs(p.y - terrain) <= kGroundSnap)
			p.y = terrain;
	}
	return p;
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
