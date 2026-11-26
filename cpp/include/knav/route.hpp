#pragma once

// The move_to route planner (docs/reference/NAVIGATION.md; kenshi-k16.6.354): from a start pose to a clicked goal over one NavMesh.
// The goal snap (click reach, same-set preference, click height), the clearance + door-class search (NavQuery::findPath), the
// stranded-walker reseat. MeshNavigator::routeFrom is a thin wrapper; a client with the same mesh calls planRoute directly for a
// live path preview. No simulation types: the sim's facts come in as plain inputs. Doors are the mesh's own paint
// (NavMesh::applyDoors): the caller's door state is whatever it painted, `planned` picks the "no class" view that keeps locked
// doors in the search. Self-contained like walk.hpp: needs a NavMesh and a NavQuery on it.

#include "knav/navmesh.hpp"
#include "knav/walk.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace knav {

// How far from the mesh a clicked destination may be and still be routed to its nearest point.
// MEASURED (docs/reference/MOVE_CLICK_LIVE.md): the original finds 347 u (diagonal) and 330 u (vertical), refuses 354 / 340.
inline constexpr double kClickReach = 340.0;
// A route end this close (xz) to the click counts as the clicked spot, not an approach point.
inline constexpr double kClickExact = 0.75;
// How far from the mesh a walker's own start may be and still be snapped on.
inline constexpr double kEndReach = 40.0;

// What a Detour search failure means for a log (RoutePlan::check).
std::string_view pathFailureCheck(PathResult::Failure failure) noexcept;

// The connected sets of one mesh (doors count as ground) and their sizes, rebuilt when the mesh revision changes.
// One per mesh; not thread-safe.
class RouteSets
{
public:
	std::uint32_t of(const NavMesh& mesh, const PolyLoc& loc);
	// Builds the sets for the mesh's current revision now (the whole world's, at load) instead of on the first route.
	void build(const NavMesh& mesh);
	std::uint32_t size(std::uint32_t label) const { return label < sizes_.size() ? sizes_[label] : 0; }

private:
	std::optional<PolyComponents> sets_;
	std::vector<std::uint32_t> sizes_;
	std::uint64_t revision_ = 0;
};

struct RouteRequest
{
	Position from; // the walker's pose (y = its level)
	double toX = 0.0;
	double toZ = 0.0;
	std::optional<double> toY; // the click's height, when the caller has one
	// Locked doors stay in the search.
	bool planned = false;
	// Body: clearance from the walls (0 = none) and the race's water cost scale (empty = the filter's default).
	double radius = 0.0;
	std::optional<float> water;
	bool straighten = true;
	// A player click without a height: the goal is the ground nearest the click in xz, at its height (kenshi-k16.8.10).
	bool clickGoal = false;
};

struct RoutePlan
{
	bool found = false;
	enum class Failure : std::uint8_t { None, OffMesh, NoRoute };
	Failure failure = Failure::None;
	std::string_view check; // a literal; why it failed (empty when found)
	PathResult::Failure pathFailure = PathResult::Failure::None; // the search's own failure, for counters
	bool plainFailed = false;
	std::vector<Point> points; // start to end, y = surface height
	bool approach = false; // ends at an approach point, not the clicked spot
	bool reseated = false; // the start was stranded: points begin at the nearest ground of the goal's set
	std::uint64_t clearanceNodes = 0; // polygons the clearance searches closed (cost accounting)
};

RoutePlan planRoute(const NavMesh& mesh, const NavQuery& query, RouteSets& sets, const RouteRequest& request);

} // namespace knav
