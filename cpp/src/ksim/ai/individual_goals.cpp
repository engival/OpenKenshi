// The individual goals (docs/DIALOGUE.md 5, step 6b): the orders a dialogue effect gives one character, carried out here so that the routines
// and the squad AI (which yield to a running goal) do not overwrite them. kinspect `findings/dlgtasks_static.md` (10671bd) decoded the tasks.
//
//   (280 DA_RUN_AWAY is no individual goal any more: orderRunAwayGoal puts RUN_AWAY_FORCED in the member's Routine, like 164 / 19, and
//   stepRunAwayGoals runs it: away from the threats' centroid, takes no fight (Fighter::fleeing), re-rolls, ends after kFleeTicks.)
//   GoHome    DA_GO_HOME, task 19 GO_HOMEBUILDING: walks to the squad's home building (SquadInstance home point), else to its home town.
//             Ends inside the home building (exe isAtHome) or within kArrive of the town point. No home at all: not given (exe: impossible).
//             The exe has no timer; ours ends after kGoHomeTicks.
//   Summoned  DA_SUMMON_MY_SQUAD, task 67 MOVE_ON_FREE_WILL_FAST: runs to the summoner, re-targeted as it moves. Ends within kSummonReach
//             (exe isWithin10Meters) or when it is gone or down. The exe gives up as Task_Move does (lost S, 16 retries); ours after kSummonTicks.
//
// Not kept: the AGGRESSOR tag (2) run-away and go-home set on the target (its readers are not decoded). A goal whose walk has no route, or
// a sim with no navmesh, ends it at once: it never waits for ever.

#include "ksim/core/simulation.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/core/rng.hpp"

#include "kworld/roadfixups.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace ksim
{

namespace
{

// The install's roads, parsed once per process and data directory (a 23 MB level file); null when the install has none.
std::shared_ptr<const kworld::RoadNetwork> sharedRoadNetwork(const std::string& dataDir)
{
	static std::mutex lock;
	static std::map<std::string, std::shared_ptr<const kworld::RoadNetwork>> cache;
	const std::lock_guard<std::mutex> guard(lock);
	auto [slot, fresh] = cache.emplace(dataDir, nullptr);
	if (fresh)
	{
		try { slot->second = std::make_shared<const kworld::RoadNetwork>(kworld::RoadNetwork::load(dataDir, kworld::defaultRoadFixupsPath())); }
		catch (const std::exception& e) { std::cerr << "ksim: no road network: " << e.what() << "\n"; }
	}
	return slot->second;
}

constexpr std::uint64_t kGoalLookTicks = 20; // a walk that ended is re-planned once a second
constexpr double kFleeMin = 2000.0;
constexpr double kFleeSpread = 400.0;
constexpr double kArrive = 100.0;
constexpr double kRerollNear = 200.0; // Task_Runaway re-targets when this close to its destination (or not moving)
constexpr double kRerollMin = 1000.0;
constexpr double kRerollSpread = 400.0;
constexpr int kRerollTries = 4; // random points tried in one look before the run gives up (a stuck runner never holds its goal)
constexpr double kAggressionRange = 3000.0; // OURS/UNSURE, 280 only: the dialogue flee reads whoever targets the runner this far out
constexpr double kSummonReach = 100.0;
constexpr double kNoProgress = 20.0; // a route that ends this close to the walker gets it nowhere
constexpr std::uint64_t kFleeTicks = static_cast<std::uint64_t>(120.0 / kTickSeconds);
constexpr std::uint64_t kSummonTicks = static_cast<std::uint64_t>(120.0 / kTickSeconds);
constexpr std::uint64_t kGoHomeTicks = static_cast<std::uint64_t>(600.0 / kTickSeconds);

bool upright(const state::Fighter* f)
{
	return f != nullptr && !f->dead && !f->unconscious && !f->down && f->carriedBy == 0;
}

// Task_Runaway's threats: the live members whose target is this one (the exe reads the runner's known threats, category 6 with danger > 0).
// Returns how many there are and sets the centroid and the nearest.
int threatsOf(const CombatArena& arena, std::uint64_t id, const state::Fighter& self, double& cx, double& cz, std::uint64_t& nearest)
{
	int count = 0;
	double near = 0.0;
	cx = 0.0;
	cz = 0.0;
	nearest = 0;
	for (const CombatArena::Member& m : arena.members())
	{
		const state::Fighter& foe = m.fighter;
		if (m.id == id || foe.target != id || foe.dead || foe.down || foe.unconscious || foe.carriedBy != 0)
			continue;
		const double d = std::hypot(foe.x - self.x, foe.z - self.z);
		if (d > kAggressionRange)
			continue;
		cx += foe.x;
		cz += foe.z;
		++count;
		if (nearest == 0 || d < near)
		{
			nearest = m.id;
			near = d;
		}
	}
	if (count > 0)
	{
		cx /= count;
		cz /= count;
	}
	return count;
}

// The mover's "moving" flag (CharMovement vf+0x48). A crawler (down) always reads not moving - the original's quirk: the away leg is re-rolled
// from the first tick (flee_behavior.md section 4, a possible later deliberate difference; FEATURES). Dead code here while a down member ends
// the goal, kept so a modelled crawl inherits it.
bool moving(const state::Fighter& f)
{
	return f.walking() && !f.down;
}

} // namespace

void Simulation::ensureRoads()
{
	if (!roadsTried_)
	{
		roadsTried_ = true;
		roads_ = navigation_ != nullptr ? sharedRoadNetwork(world_.dataDir()) : nullptr;
	}
}

double Simulation::roadFactor(std::uint64_t id) const
{
	const state::Allegiance* allegiance = fighterEntity(id).get<state::Allegiance>();
	const kdata::Record* faction = allegiance != nullptr && gameData_ != nullptr ? gameData_->find(allegiance->factionSid) : nullptr;
	return kworld::RoadNetwork::factorFor(faction != nullptr ? faction->floatOr("road preference", 0.4f) : 0.4);
}

Route Simulation::routeRetry(std::uint64_t id, const state::Fighter& f, double toX, double toZ, std::optional<double> toY, bool planned)
{
	// A door or gate changed state since the last ask: routability changed, every ladder starts over (the exe's invalidate, C57; ksim 2026-11-10).
	if (navigation_ != nullptr && navigation_->doorPaints() != retriesDoorPaints_)
	{
		retriesDoorPaints_ = navigation_->doorPaints();
		pathRetries_.resetAll();
	}
	const PathRetries::Ask ask = pathRetries_.ask(id, f.x, f.z, toX, toZ);
	ensureRoads();
	Route route;
	if (navigation_ == nullptr || ask.step == PathRetries::Step::Stuck)
	{
		route.failure = state::AiReason::NoRoute;
	} else if (ask.step == PathRetries::Step::RoadNet) {
		route = roadNetworkRoute(f, toX, toZ, toY, planned);
	} else {
		// A move over 1000 u takes the road shortcut first (the exe's 14065f940); when no road serves it, or a leg fails, the plain search.
		if (std::hypot(ask.x - f.x, ask.z - f.z) > kworld::RoadNetwork::kLongMove)
			route = roadNetworkRoute(f, ask.x, ask.z, ask.step == PathRetries::Step::Direct ? toY : std::nullopt, planned, roadFactor(id));
		// A goal in a zone that is not loaded is clamped to the exit of the walker's zone (D9): the route is found, ends there, and the owner's next ask
		// goes on from it. A sideways goal has no known height: the walker's own level is the fallback.
		if (!route.found)
			route = navigation_->routeFarFrom({f.x, f.y, f.z}, ask.x, ask.z, ask.step == PathRetries::Step::Direct ? toY : std::nullopt, planned, body(f));
	}
	pathRetries_.result(id, route.found);
	// PATHFINDING_STATIC 1.11: the walker was off the mesh or stranded; the route starts on the ground it is re-seated on.
	state::Fighter* seat = route.found && route.reseated && !route.points.empty() && combat_ != nullptr ? combat_->find(id) : nullptr;
	if (seat != nullptr)
	{
		seat->x = route.points[0].x;
		seat->y = route.heights[0];
		seat->z = route.points[0].z;
		mirrorFighter(id);
	}
	return route;
}

Route Simulation::roadNetworkRoute(const state::Fighter& f, double toX, double toZ, std::optional<double> toY, bool planned, double factor) const
{
	// PATHFINDING_STATIC.md 1.13 / 2.8; roads_static.md "Pathing use": the road's markers are intermediate move targets and the navmesh
	// mover walks between them (14065f940); a blocked node is dropped ("Skip road node", 14065ffa0). The goal's own leg must succeed.
	Route none;
	if (navigation_ == nullptr || roads_ == nullptr)
		return none;
	const std::vector<kworld::RoadNetwork::Marker> markers = roads_->route(f.x, f.z, toX, toZ, factor);
	if (markers.empty())
		return none;
	const Body traveller = body(f);
	Route whole;
	knav::Position at{f.x, f.y, f.z};
	const auto append = [&](const Route& leg)
	{
		for (std::size_t p = whole.points.empty() ? 0 : 1; p < leg.points.size(); ++p)
		{
			whole.points.push_back(leg.points[p]);
			whole.heights.push_back(leg.heights[p]);
		}
		at = {leg.points.back().x, leg.heights.back(), leg.points.back().z};
	};
	for (const kworld::RoadNetwork::Marker& m : markers)
	{
		if (std::hypot(m.x - at.x, m.z - at.z) < 1.0)
			continue;
		// Each leg goes through the zone clamp (D9): a marker outside the loaded zones is not a failure, the route ends at the exit of the zone
		// the walker is in and its owner asks again from there. No leg beyond a clamp is planned (its ground is not loaded).
		const Route leg = navigation_->routeFarFrom(at, m.x, m.z, std::nullopt, planned, traveller);
		if (leg.found && !leg.points.empty() && (!leg.approach || leg.clamped))
		{
			append(leg);
			if (leg.clamped)
			{
				whole.found = true;
				whole.clamped = true;
				return whole;
			}
		}
	}
	const Route last = navigation_->routeFarFrom(at, toX, toZ, toY, planned, traveller);
	if (!last.found || last.points.empty())
		return none;
	append(last);
	whole.found = true;
	whole.approach = last.approach;
	whole.clamped = last.clamped;
	return whole;
}

bool Simulation::walkGoal(std::uint64_t id, state::Fighter& f, double x, double z)
{
	// The walk is followed only on a navmesh (CombatArena drops a route it cannot follow).
	if (navigation_ == nullptr)
		return false;
	const Route route = routeRetry(id, f, x, z, std::nullopt, true);
	if (!route.found || route.points.empty() || std::hypot(route.points.back().x - f.x, route.points.back().z - f.z) <= kNoProgress)
		return false;
	f.clearRoute();
	std::vector<state::RoutePoint> points;
	for (std::size_t p = 0; p < route.points.size(); ++p)
		points.push_back({route.points[p].x, route.heights[p], route.points[p].z});
	setRoute(f, std::move(points), navigation_.get(), body(f));
	++f.routeVersion;
	f.routeSeq = 0;
	f.routeChase = false;
	f.routeApproach = route.approach;
	f.routeTries = 0;
	mirrorFighter(id);
	return true;
}

// Task_Runaway::start: the destination is 2000..2400 past the centroid of the runner's threats, on the line from it through the runner; the
// runner's own position when it has none (then straight along the heading). OURS/UNSURE for 280: with no aggressor the order's target stands
// in for the centroid (existing behaviour; the exe uses its own position).
bool Simulation::fleePoint(std::uint64_t id, const state::Fighter& f, const state::Fighter& from, double& x, double& z) const
{
	double cx = 0.0;
	double cz = 0.0;
	std::uint64_t nearest = 0;
	if (threatsOf(*combat_, id, f, cx, cz, nearest) == 0)
	{
		cx = from.x;
		cz = from.z;
	}
	const double dx = f.x - cx;
	const double dz = f.z - cz;
	const double d = std::hypot(dx, dz);
	RngStream dice(streamSeed(streamSeed(streamSeed(seed_, kGoalStream), static_cast<std::size_t>(id)), static_cast<std::size_t>(clockTick())));
	const double reach = kFleeMin + kFleeSpread * dice.uniform01();
	x = cx + (d > 1.0 ? dx / d : std::sin(f.yaw)) * reach;
	z = cz + (d > 1.0 ? dz / d : std::cos(f.yaw)) * reach;
	return true;
}

// 280 RUN_AWAY_FORCED (DA_RUN_AWAY) as an order-named Routine (kenshi-k16.6.12.5; the route of 164 / 19): the member's Routine names the goal and
// the one fled from, stepRunAwayGoals runs it. A replace=1 order: the current task ends first (Character::giveOrder 1405d20d0).
bool Simulation::orderRunAwayGoal(std::uint64_t id, std::uint64_t target)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	const state::Fighter* from = combat_ != nullptr ? combat_->find(target) : nullptr;
	if (!upright(f) || f->playerControlled || navigation_ == nullptr || !upright(from) || target == id)
		return false;
	RunAwayRun run;
	fleePoint(id, *f, *from, run.x, run.z);
	CombatArena::endFollow(*f);
	combat_->dropTarget(id); // a walking NPC never re-picks, so a fight it held would run under the walk
	f->clearRoute();
	f->hold = false;
	f->orderTarget = 0;
	f->asleep = false;
	f->goal = state::IndividualGoal{};
	if (!walkGoal(id, *f, run.x, run.z) && !rerollFlee(id, *f, run.x, run.z))
	{
		mirrorFighter(id);
		return false;
	}
	run.until = clockTick() + kFleeTicks; // OURS: nothing else would give the NPC its next order
	runAway_[id] = run;
	f->fleeing = true;
	state::Routine routine;
	routine.goal = routines::task::RunAwayForced;
	routine.since = clockTick();
	routine.chainTarget = target;
	routine.plan = {routines::task::RunAwayForced};
	routine.nextScore = clockTick() + 100000; // the scorer does not re-score the member while the goal runs; endGoalRoutine hands it back
	fighterEntity(id).set<state::Routine>(std::move(routine));
	mirrorFighter(id);
	return true;
}

// Task_Runaway never ends at its point: a new random one when idle or within 200 u of it. 280 has no end of its own in the exe (ours: kFleeTicks).
void Simulation::stepRunAwayGoals()
{
	if (combat_ == nullptr || runAway_.empty())
		return;
	const std::uint64_t now = clockTick();
	std::vector<std::uint64_t> running;
	for (const auto& [id, run] : runAway_)
		running.push_back(id);
	for (const std::uint64_t id : running)
	{
		state::Fighter* f = combat_->find(id);
		const state::Routine* routine = f != nullptr ? fighterEntity(id).get<state::Routine>() : nullptr;
		RunAwayRun& run = runAway_[id];
		bool done = routine == nullptr || routine->goal != routines::task::RunAwayForced || !upright(f) || f->playerControlled || now >= run.until;
		if (!done && now % kGoalLookTicks == 0 && (!moving(*f) || std::hypot(f->x - run.x, f->z - run.z) <= kRerollNear))
			done = !rerollFlee(id, *f, run.x, run.z);
		if (done)
			endRunAway(id);
	}
}

void Simulation::endRunAway(std::uint64_t id)
{
	runAway_.erase(id);
	if (state::Fighter* f = combat_->find(id))
		f->fleeing = false;
	endGoalRoutine(id, routines::task::RunAwayForced);
	mirrorFighter(id);
}

bool Simulation::giveIndividualGoal(std::uint64_t id, state::IndividualKind kind, std::uint64_t target)
{
	state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (!upright(f) || f->playerControlled || navigation_ == nullptr)
		return false;
	state::IndividualGoal goal;
	goal.kind = kind;
	goal.target = target;
	const std::uint64_t now = clockTick();
	switch (kind)
	{
	case state::IndividualKind::RunAway:
	{
		const state::Fighter* from = combat_->find(target);
		if (!upright(from) || target == id || !fleePoint(id, *f, *from, goal.x, goal.z))
			return false;
		goal.until = std::numeric_limits<std::uint64_t>::max(); // a 35 ends with the aggression
		break;
	}
	case state::IndividualKind::Summoned:
		if (target == id || !upright(combat_->find(target)))
			return false;
		goal.until = now + kSummonTicks;
		break;
	case state::IndividualKind::GoHome:
	{
		const flecs::entity squad = fighterEntity(id).target<state::MemberOf>();
		const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
		if (instance == nullptr)
			return false;
		if (instance->homeBuilding != 0)
		{
			goal.x = instance->homeX;
			goal.z = instance->homeZ;
		} else if (instance->homeTown.id != 0 && ecsMutable().is_alive(instance->homeTown.id)) {
			const state::TownState* town = ecsMutable().entity(instance->homeTown.id).get<state::TownState>();
			if (town == nullptr)
				return false;
			goal.x = town->x;
			goal.z = town->z;
		} else {
			return false;
		}
		goal.until = now + kGoHomeTicks;
		break;
	}
	case state::IndividualKind::None:
		return false;
	}
	// A replace=1 order: the current task ends first (Character::giveOrder 1405d20d0).
	CombatArena::endFollow(*f);
	if (kind == state::IndividualKind::RunAway)
		combat_->dropTarget(id); // a walking NPC never re-picks, so a fight it held would run under the walk
	f->clearRoute();
	f->hold = false;
	f->orderTarget = 0;
	f->asleep = false;
	f->goal = goal;
	// 35 is the plan step under 32: with no way to run at all there is no step, and 32 stays with the fight (the caller's goal)
	if (kind == state::IndividualKind::RunAway && !walkGoal(id, *f, goal.x, goal.z) && !rerollFlee(id, *f, goal.x, goal.z))
	{
		f->goal = state::IndividualGoal{};
		return false;
	}
	mirrorFighter(id);
	return true;
}

// The re-target helper (GOG 0x3361c0): a random xz direction, 1000..1400 u from the runner, no bias away from the threats. Tries a few draws
// (a point with no route is skipped); false when none works, so the caller ends the goal instead of leaving a stuck runner holding it.
bool Simulation::rerollFlee(std::uint64_t id, state::Fighter& f, double& outX, double& outZ)
{
	const std::uint64_t now = clockTick();
	RngStream dice(streamSeed(streamSeed(streamSeed(streamSeed(seed_, kGoalStream), 1), static_cast<std::size_t>(id)), static_cast<std::size_t>(now)));
	for (int t = 0; t < kRerollTries; ++t)
	{
		const double a = dice.uniform01() * 6.283185307179586;
		const double reach = kRerollMin + kRerollSpread * dice.uniform01();
		const double x = f.x + std::sin(a) * reach;
		const double z = f.z + std::cos(a) * reach;
		if (walkGoal(id, f, x, z))
		{
			outX = x;
			outZ = z;
			return true;
		}
	}
	return false;
}

void Simulation::stepIndividualGoals()
{
	if (combat_ == nullptr)
		return;
	const std::uint64_t now = clockTick();
	std::vector<std::uint64_t> running;
	for (const CombatArena::Member& m : combat_->members())
	{
		if (m.fighter.goal.kind != state::IndividualKind::None)
			running.push_back(m.id);
	}
	for (const std::uint64_t id : running)
	{
		state::Fighter& f = *combat_->find(id);
		const state::IndividualGoal& goal = f.goal;
		const state::Fighter* target = goal.target != 0 ? combat_->find(goal.target) : nullptr;
		bool done = !upright(&f) || f.playerControlled || now >= goal.until;
		double toX = goal.x;
		double toZ = goal.z;
		switch (goal.kind)
		{
		case state::IndividualKind::RunAway:
		{
			// Task_Runaway never ends at its point: it picks a new random one when idle or within 200 u of it. 35 ends when nobody is
			// aggressing against the runner any more (nobody targets it).
			double cx = 0.0;
			double cz = 0.0;
			std::uint64_t nearest = 0;
			done = done || threatsOf(*combat_, id, f, cx, cz, nearest) == 0;
			if (!done && now % kGoalLookTicks == 0 && (!moving(f) || std::hypot(f.x - goal.x, f.z - goal.z) <= kRerollNear))
				done = !rerollFlee(id, f, f.goal.x, f.goal.z);
			break;
		}
		case state::IndividualKind::Summoned:
			if (!upright(target) || std::hypot(f.x - target->x, f.z - target->z) <= kSummonReach)
			{
				done = true;
				break;
			}
			toX = target->x;
			toZ = target->z;
			// the summoner moved off the route's end: re-planned now
			if (f.walking() && std::hypot(f.route.back().x - toX, f.route.back().z - toZ) > kSummonReach && now % kGoalLookTicks == 0)
				f.clearRoute();
			break;
		case state::IndividualKind::GoHome:
		{
			const flecs::entity squad = fighterEntity(id).target<state::MemberOf>();
			const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
			const bool building = instance != nullptr && instance->homeBuilding != 0;
			if (building ? f.building == instance->homeBuilding : std::hypot(f.x - goal.x, f.z - goal.z) <= kArrive)
				done = true;
			break;
		}
		case state::IndividualKind::None:
			break;
		}
		// A walk that ended short is planned again; one with no way on ends the goal (it must not wait for ever).
		const bool runs = goal.kind == state::IndividualKind::RunAway; // re-rolled above
		if (!done && !runs && !f.walking() && now % kGoalLookTicks == 0)
			done = !walkGoal(id, f, toX, toZ);
		if (done)
		{
			f.goal = state::IndividualGoal{};
			f.clearRoute();
			mirrorFighter(id);
		}
	}
}

} // namespace ksim
