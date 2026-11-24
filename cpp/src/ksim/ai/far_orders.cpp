// Far move orders as the exe walks them (kenshi-k16.6.481; docs/reference/LOCOMOTION.md D9; findings/rwh208_far_move_goal.md): the order keeps its
// true goal, the route goes to a leg end - the exit of the walker's loaded zone along the line to the goal (MeshNavigator::routeFarFrom), or a road
// marker - and the next leg is planned from there. Road markers are intermediate goals through the same clamp, never planned up front.
// A leg that ends at a road marker is not walked to its end: the exe hands over the next marker when the walker is within the road's width of
// this one, with a new request from the walker's position (rwh.268; PATH_WALK.md 3d). Within 547.7 u of the true goal the markers are dropped.

#include "ksim/core/simulation.hpp"

#include "ksim/nav/navigator.hpp"

#include <algorithm>
#include <cmath>

namespace ksim {

namespace {

// A zone-exit clamp leg's next leg is appended while the walker has this much route left (the exe re-requests at 3.16 u of the leg's end, with the
// walker braked to nothing there; ours keeps the walk going, LOCOMOTION D9). Beyond the braking distance of a run. Road legs do not use it.
constexpr double kLegLookahead = 300.0;
// A failed goal is not asked for again for this long (addBlockedTarget, move_unreachable.md).
constexpr double kBlockedSeconds = 20.0;
// Legs appended in one tick (short road legs follow each other).
constexpr int kLegsPerTick = 4;
// Within this xz distance of the true goal the road route is dropped (300000 u^2, rwh.268).
constexpr double kRoadDropRange = 547.7;
// A marker without a width (a restored order) hands over at the median ROAD width.
constexpr double kDefaultRoadWidth = 100.0;

double routeLeft(const state::Fighter& f)
{
	if (f.routeNext >= f.route.size())
		return 0.0;
	double left = std::hypot(f.route[f.routeNext].x - f.x, f.route[f.routeNext].z - f.z);
	for (std::size_t i = f.routeNext; i + 1 < f.route.size(); ++i)
		left += std::hypot(f.route[i + 1].x - f.route[i].x, f.route[i + 1].z - f.route[i].z);
	return left;
}

double routePointsLength(const Route& leg, const knav::Position& from)
{
	double len = 0.0;
	double x = from.x;
	double z = from.z;
	for (const auto& p : leg.points)
	{
		len += std::hypot(p.x - x, p.z - z);
		x = p.x;
		z = p.z;
	}
	return len;
}

// A candidate target on the road route: the point, and the index of the first marker beyond it.
struct RoadPick
{
	double x, z;
	std::size_t next;
	double arc; // along the route from marker 0
};

// The look-ahead candidates, best first: the furthest route point inside the circle R (within a window of 8R of route ahead of order.lo), the point
// where the route first leaves the circle, the next marker. A walker with no route inside R cuts to the sample minimising |walker-p| + |p-goal|.
std::vector<RoadPick> lookaheadPicks(const std::vector<kworld::RoadNetwork::Marker>& m, std::size_t lo, double goalX, double goalZ, std::optional<RoadPick> held, double reachedArc, const knav::Position& from, double R)
{
	std::vector<RoadPick> picks;
	constexpr double kStep = 20.0;
	std::optional<RoadPick> furthest;
	std::optional<RoadPick> firstExit;
	bool inRun = false;
	bool runEnded = false;
	std::optional<RoadPick> cut;
	double cutCost = 1.0e30;
	double arc = 0.0; // arc length ahead of markers[lo]
	double base = 0.0; // arc length of markers[lo] from marker 0
	for (std::size_t i = 0; i < lo && i + 1 < m.size(); ++i)
		base += std::hypot(m[i + 1].x - m[i].x, m[i + 1].z - m[i].z);
	for (std::size_t i = lo; i + 1 < m.size() && arc < 8.0 * R; ++i)
	{
		const double len = std::hypot(m[i + 1].x - m[i].x, m[i + 1].z - m[i].z);
		const int steps = std::max(1, static_cast<int>(std::ceil(len / kStep)));
		for (int k = 1; k <= steps; ++k)
		{
			const double t = static_cast<double>(k) / steps;
			const RoadPick p{m[i].x + t * (m[i + 1].x - m[i].x), m[i].z + t * (m[i + 1].z - m[i].z), i + 1, base + arc + t * len};
			const double d = std::hypot(p.x - from.x, p.z - from.z);
			if (d < R)
			{
				furthest = p;
				if (!runEnded)
					inRun = true, firstExit = p;
			} else if (inRun) {
				runEnded = true;
			}
			const double cost = d + 0.3 * std::hypot(p.x - goalX, p.z - goalZ);
			if (cost < cutCost)
				cutCost = cost, cut = p;
		}
		arc += len;
	}
	if (furthest)
	{
		// The target only moves forward along the route: a walker the navmesh path turns aside must not flip between two cuts.
		picks.push_back(*furthest);
		if (firstExit && (firstExit->x != furthest->x || firstExit->z != furthest->z))
			picks.push_back(*firstExit);
	} else if (!held && cut) {
		picks.push_back(*cut); // joining: nothing of the route is within R; the cut toward the road in the travel direction
	}
	if (lo + 1 < m.size())
		picks.push_back({m[lo + 1].x, m[lo + 1].z, lo + 1, base + std::hypot(m[lo + 1].x - m[lo].x, m[lo + 1].z - m[lo].z)});
	std::erase_if(picks, [&](const RoadPick& p) { return p.arc <= reachedArc + 1.0; }); // already arrived at (inside its zone)
	if (held && held->arc > reachedArc + 1.0)
	{
		// The fallbacks never go back behind the held target; it is the last resort, taken whatever the path to it.
		std::erase_if(picks, [&](const RoadPick& p) { return p.arc < held->arc - 1.0; });
		picks.push_back(*held);
	}
	return picks;
}

} // namespace

// Look-ahead leg: straight (by navmesh) to the zone round the chosen road-route point. false = nothing routable (the caller drops a marker as before).
bool Simulation::planRoadLookahead(const state::Fighter& f, FarOrder& order, const knav::Position& from, Route& leg) const
{
	const Body traveller = body(f);
	const double R = order.lookR;
	std::optional<RoadPick> held;
	if (order.tgtArc >= 0.0)
		held = RoadPick{order.tgtX, order.tgtZ, order.next, order.tgtArc};
	const std::vector<RoadPick> picks = lookaheadPicks(order.markers, order.lo, order.goalX, order.goalZ, held, order.reachedArc, from, R);
	for (std::size_t c = 0; c < picks.size(); ++c)
	{
		const RoadPick& p = picks[c];
		const double d = std::hypot(p.x - from.x, p.z - from.z);
		// The goal is the zone, not its centre: the point Z toward the walker (the pathfinder walks round what is in the way).
		if (d <= kRoadZone)
		{
			// Inside this point's zone: it counts as arrived; the next look-ahead starts beyond it.
			order.reachedArc = std::max(order.reachedArc, p.arc);
			order.lo = std::max(order.lo, p.next - 1);
			order.tgtArc = -1.0;
			continue;
		}
		const double shift = std::min(kRoadZone, d);
		const double gx = d > 0.0 ? p.x + (from.x - p.x) * shift / d : p.x;
		const double gz = d > 0.0 ? p.z + (from.z - p.z) * shift / d : p.z;
		Route r = navigation_->routeFarFrom(from, gx, gz, std::nullopt, false, traveller, true, false);
		if (!r.found || r.points.empty())
			continue;
		// A goal the mesh cannot reach (the path ends short of the zone): not a target, whatever the arc.
		if (!r.clamped && std::hypot(r.points.back().x - gx, r.points.back().z - gz) > 2.0 * kRoadZone)
			continue;
		// A switchback: the path to the cut is much longer than the cut (a real climb): fall back to the nearer candidate.
		if (!r.clamped && c + 1 < picks.size() && routePointsLength(r, from) > 2.0 * d + 2.0 * kRoadZone)
			continue;
		leg = std::move(r);
		order.radius = R;
		const kworld::RoadNetwork::Marker& last = order.markers.back();
		order.next = p.next >= order.markers.size() && std::hypot(p.x - last.x, p.z - last.z) > 1.0 ? order.markers.size() - 1 : p.next;
		order.lo = std::max(order.lo, order.next - 1);
		order.tgtArc = p.arc;
		order.tgtX = p.x;
		order.tgtZ = p.z;
		order.planX = from.x;
		order.planZ = from.z;
		++roadGoalStats_.goals;
		if (shift > 0.0 && d > kRoadZone)
		{
			++roadGoalStats_.offCentre;
			roadGoalStats_.offSum += shift;
			roadGoalStats_.offMax = std::max(roadGoalStats_.offMax, shift);
		}
		return true;
	}
	order.tgtArc = -1.0; // nothing routable: the held target goes too
	order.next = std::max(order.next, order.lo + 1); // the marker rule carries on from the progress made
	return false;
}

bool Simulation::planFarLeg(const state::Fighter& f, FarOrder& order, const knav::Position& from, Route& leg) const
{
	const Body traveller = body(f);
	for (;;)
	{
		order.onMarker = false;
		order.radius = 0.0;
		const bool atGoal = order.next >= order.markers.size();
		const double tx = atGoal ? order.goalX : order.markers[order.next].x;
		const double tz = atGoal ? order.goalZ : order.markers[order.next].z;
		if (!atGoal && order.scale > 0.0 && planRoadLookahead(f, order, from, leg))
		{
			++order.legs;
			order.onMarker = true;
			if (order.next >= order.markers.size())
				order.next = order.markers.size(); // the route's end is the target: the next leg is the goal
			return true;
		}
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
		{
			order.cur = order.next;
			order.onMarker = true;
			// The exe's follower holds the marker until the walker is within its width (stepFarOrders hands the next over); a leg that ends
			// short of it (a zone-exit clamp) is asked for again toward the same marker. The look-ahead's fallback hands the next over at plan time.
			if (order.scale > 0.0)
				++order.next;
		}
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
	order.scale = roadLookahead_;
	// The road shortcut (14065f940): a move over 1000 u that is not an animal's, when the roads serve it (the use test is the router's).
	if (!f.animal && std::hypot(goalX - f.x, goalZ - f.z) > kworld::RoadNetwork::kLongMove)
	{
		ensureRoads();
		if (roads_ != nullptr)
			order.markers = roads_->route(f.x, f.z, goalX, goalZ, roadFactor(id));
		// R: the scale x the route's median road width (one value per order, so a width change between roads does not jump the circle).
		std::vector<double> widths;
		for (const kworld::RoadNetwork::Marker& m : order.markers)
			widths.push_back(m.width > 0.0 ? m.width : kDefaultRoadWidth);
		if (!widths.empty())
		{
			std::nth_element(widths.begin(), widths.begin() + widths.size() / 2, widths.end());
			order.lookR = order.scale * widths[widths.size() / 2];
		}
	}
	if (order.markers.empty() && !far)
		return false; // a plain near move: the caller installs the route it made
	Route leg;
	if (!planFarLeg(f, order, {f.x, f.y, f.z}, leg))
		return false;
	f.clearRoute(state::Fighter::kClearNewOrder);
	std::vector<state::RoutePoint> points;
	for (std::size_t i = 0; i < leg.points.size(); ++i)
		points.push_back({leg.points[i].x, leg.heights[i], leg.points[i].z});
	setRoute(f, std::move(points), navigation_.get(), body(f));
	++f.routeVersion;
	f.routeSeq = seq;
	f.routeChase = false;
	f.routeApproach = order.done && leg.approach;
	f.routeMoreLegs = !order.done;
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
			f->routeMoreLegs = !order.done; // not saved: the order that is back says the route's end is a leg end
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
		// Road follower (rwh.268): near the true goal the markers are dropped; within the current marker's width the next is handed over, planned from the walker.
		if (!order.done && order.next < order.markers.size() && std::hypot(order.goalX - f->x, order.goalZ - f->z) < kRoadDropRange)
			order.next = order.markers.size();
		if (!order.done && order.onMarker)
		{
			const kworld::RoadNetwork::Marker& m = order.markers[std::min(order.cur, order.markers.size() - 1)];
			const double width = m.width > 0.0 ? m.width : kDefaultRoadWidth;
			const bool dropped = order.next >= order.markers.size() && std::hypot(order.goalX - f->x, order.goalZ - f->z) < kRoadDropRange;
			// Look-ahead: re-planned from the walker every third of R it has advanced, or when it is in the target's zone.
			const bool handOver = order.radius > 0.0
				? std::hypot(order.planX - f->x, order.planZ - f->z) > order.radius / 3.0 || std::hypot(order.tgtX - f->x, order.tgtZ - f->z) < kRoadZone
				: std::hypot(m.x - f->x, m.z - f->z) < width;
			if (dropped || handOver || !ran)
			{
				if (order.scale <= 0.0 && handOver && order.next < order.markers.size())
					++order.next; // one marker per tick, never skipping ahead
				Route leg;
				if (!planFarLeg(*f, order, {f->x, f->y, f->z}, leg))
				{
					order.blocked = true;
					order.blockedUntil = now + static_cast<std::uint64_t>(kBlockedSeconds / kTickSeconds);
				} else {
					// The route is replaced; the walker's velocity is kept.
					std::vector<state::RoutePoint> points;
					for (std::size_t p = 0; p < leg.points.size(); ++p)
						points.push_back({leg.points[p].x, leg.heights[p], leg.points[p].z});
					setRoute(*f, std::move(points), navigation_.get(), body(*f));
					f->routeSeq = order.seq;
					f->routeSource = state::Fighter::PlanSource::Order;
					f->routeChase = false;
					f->routeApproach = order.done && leg.approach;
					++f->routeVersion;
					mirrorFighter(it->first);
				}
			}
		}
		for (int k = 0; k < kLegsPerTick && !order.done && !order.onMarker; ++k)
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
			std::vector<state::RoutePoint> points = restart ? std::vector<state::RoutePoint>{} : f->route;
			for (std::size_t p = restart ? 0 : 1; p < leg.points.size(); ++p)
				points.push_back({leg.points[p].x, leg.heights[p], leg.points[p].z});
			setRoute(*f, std::move(points), navigation_.get(), body(*f), restart ? 0 : f->routeNext);
			f->routeSeq = order.seq;
			f->routeSource = state::Fighter::PlanSource::Order;
			f->routeChase = false;
			f->routeApproach = order.done && leg.approach;
			f->routeMoreLegs = !order.done;
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
