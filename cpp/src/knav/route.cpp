#include "knav/route.hpp"

#include "knav/travel.hpp"

#include <cmath>

namespace knav {

// A walker is stranded when its connected set holds under 1/kStrandedRatio of the goal set's polygons (PATHFINDING_STATIC 1.11).
static constexpr std::uint32_t kStrandedRatio = 4;

std::string_view pathFailureCheck(PathResult::Failure failure) noexcept
{
	switch (failure)
	{
	case PathResult::Failure::StartOffMesh: return "start: no walkable poly near the start";
	case PathResult::Failure::EndOffMesh: return "goal: no walkable poly near the goal";
	case PathResult::Failure::NoPath: return "path: goal poly not connected to the start (another island / layer, no stair link)";
	case PathResult::Failure::Closed: return "path: search closed short of the goal (a gap too narrow for the body or a closed door)";
	case PathResult::Failure::OutOfNodes: return "path: out of search nodes";
	case PathResult::Failure::None: break;
	}
	return "path: incomplete";
}

void RouteSets::build(const NavMesh& mesh)
{
	if (sets_ && revision_ == mesh.revision())
		return;
	// Doors count as ground: a locked gate is the door check's business, not a reason to seat a walker elsewhere.
	sets_.emplace(mesh, static_cast<std::uint16_t>(0xffff));
	revision_ = mesh.revision();
	sizes_.assign(sets_->count(), 0);
	sets_->forEach([&](int, int, int, std::uint32_t label)
	{
		if (label != PolyComponents::kNone)
			++sizes_[label];
	});
}

std::uint32_t RouteSets::of(const NavMesh& mesh, const PolyLoc& loc)
{
	build(mesh);
	return sets_->at(loc.tileX, loc.tileZ, loc.poly);
}

RoutePlan planRoute(const NavMesh& mesh, const NavQuery& query, RouteSets& sets, const RouteRequest& request)
{
	const Position& from = request.from;
	const double toX = request.toX;
	const double toZ = request.toZ;
	const std::optional<double>& toY = request.toY;
	const auto setOf = [&](const PolyLoc& loc) { return sets.of(mesh, loc); };
	// A planned route keeps locked doors in the search.
	TravelFilter travel;
	travel.radius = static_cast<float>(request.radius);
	if (request.water)
		travel.setWaterCost(*request.water);
	travel.straighten = request.straighten;
	if (request.planned)
		travel.throughLocks = true;
	RoutePlan route;
	const float reach = static_cast<float>(kClickReach);
	const Point start{static_cast<float>(from.x), static_cast<float>(from.y), static_cast<float>(from.z)};
	const Point end{static_cast<float>(toX), static_cast<float>(toY.value_or(from.y)), static_cast<float>(toZ)};
	// The end is looked up in a box of kClickReach: with a click height the nearest point must also be
	// within kClickReach in 3D; without one any height counts (a point on the terrain under a deck, or on it),
	// so the ground nearest in xz is meant, not the lowest ground in a box spanning every height.
	const Point extents{reach, toY ? reach : 1.0e5f, reach};
	Point snapped;
	// The start's own connected set (none when it is off the mesh): a goal with no height prefers ground in it.
	PolyLoc startLoc;
	const bool startOn = query.locate(start, {static_cast<float>(kEndReach), 40.0f, static_cast<float>(kEndReach)}, startLoc);
	const std::uint32_t startSet = startOn ? setOf(startLoc) : PolyComponents::kNone;
	bool snappedOk = toY ? query.nearestPoint(end, extents, snapped) : query.nearestPointXZ(end, reach, snapped);
	if (snappedOk && !toY && startSet != PolyComponents::kNone)
	{
		// Ground in the start's set no farther than 1 u (xz) than the nearest ground wins: a deck and the ground under it are both
		// at distance 0 and the walker's own level is meant, not the lowest floor.
		Point own;
		const auto sameSet = [&](const PolyLoc& loc) { return setOf(loc) == startSet; };
		if (query.nearestPointXZ(end, reach, own, sameSet)
			&& std::hypot(own[0] - end[0], own[2] - end[2]) <= std::hypot(snapped[0] - end[0], snapped[2] - end[2]) + 1.0)
			snapped = own;
	}
	if (!snappedOk
		|| (toY && std::sqrt((snapped[0] - end[0]) * (snapped[0] - end[0]) + (snapped[1] - end[1]) * (snapped[1] - end[1])
			+ (snapped[2] - end[2]) * (snapped[2] - end[2])) > reach))
	{
		route.failure = RoutePlan::Failure::OffMesh;
		route.check = snappedOk ? "goal: nearest poly is farther than the click reach in 3D (wrong layer / height)"
			: "goal: no walkable poly within the click reach";
		return route;
	}
	// A click without a height (`clickGoal`): the goal is the ground nearest in xz (`snapped`), at ITS height. findPath snaps its end in 3D from the walker's own
	// height, which where floors stack picked the floor level with the walker, ground beside the click: the Hub's ramp top at (-50802, 2756) is
	// 31 u over the ground 5.6 u away, and the goal push then took the approach 12 u from the click (kenshi-k16.8.10). When the snapped floor
	// cannot be reached the walker's own level is tried, as before.
	PathResult path;
	bool tried = false;
	if (request.clickGoal && !toY)
	{
		// Only where the walker's level is over 1 u farther from the click in xz than the ground nearest it: floors stacked over the click
		// (a room behind a shut door, a deck) stay with the walker's level, as before.
		Point level;
		const auto xz = [&](const Point& p) { return std::hypot(p[0] - end[0], p[2] - end[2]); };
		if (!query.nearestPoint(end, extents, level) || xz(level) > xz(snapped) + 1.0)
		{
			Point goal = end;
			goal[1] = snapped[1];
			path = query.findPath(start, goal, extents, travel);
			tried = true;
		}
	}
	if (!tried || !path.found || !path.complete || path.points.empty())
		path = query.findPath(start, end, extents, travel);
	route.clearanceNodes += static_cast<std::uint64_t>(path.clearanceNodes);
	// PATHFINDING_STATIC 1.11: a walker off the mesh, or in a set with no way to the goal, is seated on the nearest ground of the goal's
	// set, searched in 1000 u then 10000 u (the pocket / island / flipped edge polygon a character can land on has no way back otherwise).
	bool reseated = false;
	if ((!path.found || !path.complete || path.points.empty()) && path.failure != PathResult::Failure::EndOffMesh)
	{
		PolyLoc goalLoc;
		const std::uint32_t goalSet = query.locate(snapped, {2.0f, 40.0f, 2.0f}, goalLoc) ? setOf(goalLoc) : PolyComponents::kNone;
		// Only a walker that is stranded: off the mesh, or in a set under a quarter of the goal's (a pocket, an island, a deck). A goal on
		// an island, a roof, is no reason to move a walker standing on the mainland, and a walker in a big set walled off from the goal's
		// (a chase to an actor behind a wall) is not stranded: it gets NoRoute, so the give-up / retry / leave ladder runs.
		if (goalSet != PolyComponents::kNone
			&& (!startOn || (startSet != goalSet && sets.size(startSet) * kStrandedRatio < sets.size(goalSet))))
		{
			const auto inGoalSet = [&](const PolyLoc& loc) { return setOf(loc) == goalSet; };
			for (const float widen : {1000.0f, 10000.0f})
			{
				Point seat;
				if (!query.nearestPointXZ(start, widen, seat, inGoalSet))
					continue;
				const PathResult again = query.findPath(seat, end, extents, travel);
				route.clearanceNodes += static_cast<std::uint64_t>(again.clearanceNodes);
				if (again.found && again.complete && !again.points.empty())
				{
					path = again;
					reseated = true;
				}
				break;
			}
		}
	}
	if (!path.found || !path.complete || path.points.empty())
	{
		route.pathFailure = path.failure;
		route.failure = path.failure == PathResult::Failure::StartOffMesh
			|| path.failure == PathResult::Failure::EndOffMesh ? RoutePlan::Failure::OffMesh
			: RoutePlan::Failure::NoRoute;
		route.check = pathFailureCheck(path.failure);
		route.plainFailed = path.plainFailed;
		return route;
	}
	const Point& last = path.points.back();
	route.approach = std::hypot(last[0] - toX, last[2] - toZ) > kClickExact;
	// Nowhere to go: the snapped point is the spot the walker stands on.
	if (route.approach && path.points.size() < 2)
	{
		route.approach = false;
		route.failure = RoutePlan::Failure::NoRoute;
		route.check = "goal: only a partial path, ending on the start's own spot (the goal's poly is not connected)";
		return route;
	}
	route.found = true;
	route.reseated = reseated;
	route.points = std::move(path.points);
	return route;
}

} // namespace knav
