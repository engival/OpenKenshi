// Far move orders as the exe walks them (kenshi-k16.6.481; docs/reference/LOCOMOTION.md D9; findings/rwh208_far_move_goal.md): the order keeps its
// true goal, the route goes to a leg end - the exit of the walker's loaded zone along the line to the goal (MeshNavigator::routeFarFrom), or a road
// marker - and the next leg is planned from there. Road markers are intermediate goals through the same clamp, never planned up front.

#include "ksim/core/simulation.hpp"

#include "ksim/nav/navigator.hpp"

#include <algorithm>
#include <cmath>

namespace ksim {

namespace {

// The next leg is appended while the walker has this much route left (the exe re-requests at 3.16 u of the leg's end, with the walker braked to
// nothing there; ours keeps the walk going). Beyond the braking distance of a run.
constexpr double kLegLookahead = 300.0;
// A failed goal is not asked for again for this long (addBlockedTarget, move_unreachable.md).
constexpr double kBlockedSeconds = 20.0;
// Legs appended in one tick (short road legs follow each other).
constexpr int kLegsPerTick = 4;

double routeLeft(const state::Fighter& f)
{
	if (f.routeNext >= f.route.size())
		return 0.0;
	double left = std::hypot(f.route[f.routeNext].x - f.x, f.route[f.routeNext].z - f.z);
	for (std::size_t i = f.routeNext; i + 1 < f.route.size(); ++i)
		left += std::hypot(f.route[i + 1].x - f.route[i].x, f.route[i + 1].z - f.route[i].z);
	return left;
}

} // namespace

bool Simulation::planFarLeg(const state::Fighter& f, FarOrder& order, const knav::Position& from, Route& leg) const
{
	const Body traveller = body(f);
	for (;;)
	{
		const bool atGoal = order.next >= order.markers.size();
		const double tx = atGoal ? order.goalX : order.markers[order.next].x;
		const double tz = atGoal ? order.goalZ : order.markers[order.next].z;
		if (!atGoal && std::hypot(tx - from.x, tz - from.z) < 1.0)
		{
			++order.next;
			continue;
		}
		// A player's order is not planned: a locked door is a wall to it. The goal keeps its click height; a marker has none.
		leg = navigation_->routeFarFrom(from, tx, tz, atGoal ? order.goalY : std::nullopt, false, traveller, true, atGoal && !order.goalY);
		if (!leg.found || leg.points.empty() || (!atGoal && leg.approach && !leg.clamped))
		{
			if (atGoal)
				return false;
			++order.next; // "Skip road node" (14065ffa0): a marker the mesh cannot reach is dropped
			continue;
		}
		++order.legs;
		if (leg.clamped)
			++order.clamps;
		else if (atGoal)
			order.done = true;
		else
			++order.next;
		return true;
	}
}

bool Simulation::startMoveOrder(std::uint64_t id, state::Fighter& f, double goalX, double goalZ, std::optional<double> goalY, std::uint32_t seq, bool far)
{
	if (navigation_ == nullptr)
		return false;
	farOrders_.erase(id);
	FarOrder order;
	order.seq = seq;
	order.goalX = goalX;
	order.goalZ = goalZ;
	order.goalY = goalY;
	// The road shortcut (14065f940): a move over 1000 u that is not an animal's, when the roads serve it (the use test is the router's).
	if (!f.animal && std::hypot(goalX - f.x, goalZ - f.z) > kworld::RoadNetwork::kLongMove)
	{
		ensureRoads();
		if (roads_ != nullptr)
			order.markers = roads_->route(f.x, f.z, goalX, goalZ, roadFactor(id));
	}
	if (order.markers.empty() && !far)
		return false; // a plain near move: the caller installs the route it made
	Route leg;
	if (!planFarLeg(f, order, {f.x, f.y, f.z}, leg))
		return false;
	f.clearRoute(state::Fighter::kClearNewOrder);
	for (std::size_t i = 0; i < leg.points.size(); ++i)
		f.route.push_back({leg.points[i].x, leg.heights[i], leg.points[i].z});
	f.routeNext = 0;
	++f.routeVersion;
	f.routeSeq = seq;
	f.routeChase = false;
	f.routeApproach = order.done && leg.approach;
	f.routeSource = state::Fighter::PlanSource::Order;
	f.routeTries = 0;
	if (!order.done)
		farOrders_[id] = std::move(order);
	mirrorFighter(id);
	return true;
}

void Simulation::stepFarOrders()
{
	if (farOrders_.empty() || combat_ == nullptr || navigation_ == nullptr)
		return;
	const std::uint64_t now = clockTick();
	for (auto it = farOrders_.begin(); it != farOrders_.end();)
	{
		FarOrder& order = it->second;
		state::Fighter* f = combat_->find(it->first);
		// The order stands while the walker walks its route (routeSeq is the live order), or has just arrived at a leg end with the next unplanned.
		if (order.restored && f != nullptr && !f->dead)
		{
			// A load drops routeSource and clearWhy: a walker still on this order's route (or stopped at its leg end) is adopted again.
			if (f->routeSource == state::Fighter::PlanSource::None && f->walking() && f->routeSeq == order.seq)
				f->routeSource = state::Fighter::PlanSource::Order;
			else if (f->route.empty() && !f->routeChase && (f->routeSeq == 0 || f->routeSeq == order.seq) && f->orderTarget == 0)
			{
				// Saved on the tick a leg ended: the route is spent but the seq is still this order's, and the walker has not braked (the exe brakes to nothing at a leg end).
				f->clearWhy = state::Fighter::kClearArrived;
				f->routeSeq = 0;
				f->forwardSpeed = 0.0;
			}
		}
		order.restored = false;
		const bool ran = f != nullptr && !f->dead && f->routeSource != state::Fighter::PlanSource::None && f->routeSeq == order.seq && f->walking();
		const bool arrived = f != nullptr && !f->dead && f->route.empty() && f->clearWhy == state::Fighter::kClearArrived && !f->routeChase
			&& f->routeSeq == 0 && f->forwardSpeed == 0.0 && f->orderTarget == 0;
		if (order.blocked)
		{
			if (!ran && now >= order.blockedUntil)
			{
				it = farOrders_.erase(it);
				continue;
			}
			if (ran)
			{
				++it;
				continue;
			}
		}
		if (!ran && !arrived)
		{
			it = farOrders_.erase(it); // another order, a stop, a task or a death replaced it
			continue;
		}
		if (order.blocked)
		{
			++it;
			continue;
		}
		bool changed = false;
		for (int k = 0; k < kLegsPerTick && !order.done; ++k)
		{
			if (ran && routeLeft(*f) > kLegLookahead)
				break;
			const state::RoutePoint end = f->route.empty() ? state::RoutePoint{f->x, f->y, f->z} : f->route.back();
			Route leg;
			if (!planFarLeg(*f, order, {end.x, end.y, end.z}, leg))
			{
				// The goal's own leg cannot be routed: the walk ends where its route ends, silently, and the goal is blocked for 20 s.
				order.blocked = true;
				order.blockedUntil = now + static_cast<std::uint64_t>(kBlockedSeconds / kTickSeconds);
				break;
			}
			const bool restart = f->route.empty() || f->routeNext >= f->route.size();
			if (restart)
			{
				f->route.clear();
				f->routeNext = 0;
			}
			for (std::size_t p = restart ? 0 : 1; p < leg.points.size(); ++p)
				f->route.push_back({leg.points[p].x, leg.heights[p], leg.points[p].z});
			f->routeSeq = order.seq;
			f->routeSource = state::Fighter::PlanSource::Order;
			f->routeChase = false;
			f->routeApproach = order.done && leg.approach;
			changed = true;
			if (restart)
				break; // the walker stands: it sets off, the rest are appended as it goes
		}
		if (changed)
		{
			++f->routeVersion;
			mirrorFighter(it->first);
		}
		if (order.done)
			it = farOrders_.erase(it);
		else
			++it;
	}
}

} // namespace ksim
