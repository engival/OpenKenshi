#include "ksim/ai.hpp"
#include "ksim/routines.hpp"
#include "ksim/simdata.hpp"

#include "ksim/clock.hpp"
#include "ksim/factions.hpp"
#include "ksim/rng.hpp"
#include "ksim/towncircle.hpp"

#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <tuple>

namespace ksim {

namespace {

using state::AiReason;
using state::GoalKind;
using state::GroundPoint;

constexpr double kTicksPerHour = static_cast<double>(kTicksPerGameHour);

// Commitment: the goal in hand scores this much more when re-evaluated,
// fading to nothing over kCommitHours game-hours. OURS (not from the original).
constexpr double kCommitBonus = 0.3;
constexpr double kCommitHours = 12.0;
// A squad further than this from where it was last placed was moved by
// something else: its route is stale. UNSURE.
constexpr double kDisplaced = 8.0;
// A carried squad (the Fighters walk it): the leader may be this far from the route's progress before the progress waits, and this far
// before the goal is planned again from where the leader stands (it chased something off the route).
constexpr double kCarryLeash = 60.0;
constexpr double kSlaveLeash = 100.0; // a delivery's carrot waits while its slave on foot is farther than this from the leader
constexpr double kCarryDisplaced = 150.0;
// A carried leader that cannot close to its carrot (a mesh gap or a wall between the group route and the walkable
// ground) freezes the route progress for the rest of the leg. Waiting for the kStuckHours timeout leaves the squad
// standing for hours; after this long the leg fails and the layer re-plans (TOWN_LIFE_SLICE.md 5.14; UNSURE: the
// exe's own timeout).
constexpr std::uint64_t kCarryLeashFreezeTicks = static_cast<std::uint64_t>(5.0 * kTicksPerGameMinute);
// Nothing to do outside the navigation window or without a position: look
// again after this many game-hours.
constexpr double kBlindHours = 6.0;

// Utility numbers (UNSURE: guesses, tuned only to give ordinary life).
struct Weights
{
	double idle;
	double goHome; // when away from home
	double goHomeGrowth; // per game-hour away, capped at 48
	double travel; // base of a town visit; 0 = never
	double spread; // random part of a town visit
	double distanceScale; // a visit loses distance / this
	double range; // farthest visit
	double idleHoursMin;
	double idleHoursMax;
	double idleSpread; // random part of resting, for behaviours that rest between legs
	double local; // base of a patrol leg or a territory walk; 0 = none
	double localSpread;
	double radiusHomed; // loop / territory radius round a home town
	double radiusWild; // ... round a wilderness den
};

constexpr Weights kWeights[kBehaviourCount] = {
	// Fallback: ordinary life. Home, idle, an occasional visit to a near town.
	{0.4, 1.2, 0.0, 0.25, 0.3, 40000.0, 8000.0, 2.0, 8.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// Travel: town to town, home only after a long time away.
	{0.3, 0.3, 0.04, 0.9, 0.3, 30000.0, 1.0e9, 1.0, 3.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// GoHome and StayHome: home-bound.
	{0.5, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 2.0, 8.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// PatrolLocal: walks the loop, rests now and then. "Short range" patrols
	// of the wilderness cover more ground than a town guard's rounds.
	{0.5, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 0.5, 3.0, 0.8, 0.9, 0.3, 250.0, 1200.0},
	// AnimalTerritory: wanders its range, rests longer.
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 6.0, 0.8, 0.8, 0.3, 250.0, 1500.0},
	// Raid: ordinary life like the fallback; a faction's order outranks it.
	{0.4, 1.2, 0.0, 0.25, 0.3, 40000.0, 8000.0, 2.0, 8.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// ManTheGate: a crew holds its post (kManGateScore); without a gate it is a post holder like StayHome.
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// TradeCircuit: the circuit picks its towns itself (circuitSelect); these rows only serve a squad that cannot act (blind: rest, look again).
	{0.4, 1.2, 0.0, 0.0, 0.0, 1.0, 0.0, 2.0, 8.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// ShopKeeper and ResidentLife: home-bound exactly like StayHome (the town scorer owns what they do at Full detail).
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// BarHangout (T3d): home-bound like StayHome.
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// GuardShift (T3e1): home-bound like StayHome; the town scorer runs the watch (posts, nodes, beds, intruders) at Full detail.
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
	// CageWarden (T3e2): home-bound like StayHome; the town scorer runs the slave shop (catch, cage, watch, lock) at Full detail.
	{0.6, 1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 4.0, 12.0, 0.0, 0.0, 0.0, 0.0, 0.0},
};

// MAN_THE_GATE has no precondition but the gate itself (GATES_CARRY_LIVE.md a): no alert, enemy, time, relation or crime
// check, hours 0-24. A crew always wants its post, above every ordinary goal; only a faction's order outranks it.
constexpr double kManGateScore = 5.0;
// A crew standing within this of its post holds it (xz units).
constexpr double kPostReach = 2.0;
// A staying trader squad within this of its table (xz) stays where it stands (TOWN_LIFE_SLICE 5.17). The table is cut from the navmesh
// (layout furniture, 2026-11-03), so the walk ends on its rim: half the table plus the agent radius from the centre, 6.7 to 17 u in the
// v8 soak. Under that the squad re-walked to the table every step without moving (88 stuck episodes, 27 on v7).
constexpr double kBarReach = 20.0;
constexpr std::uint64_t kBarRetryTicks = 20; // a trader with no SquadRoutine yet asks for its table again after this many ticks

// Local goals offered per selection (a point that cannot be reached by walking
// falls to the next one). A walk may stray kLocalLeash radii from the anchor (a
// path that detours further is refused: a loop or territory never leaves far).
constexpr int kLocalTries = 3;
// A wilderness squad is put on the navmesh at its first step, from as far as this.
constexpr double kSnapReach = 800.0;

// An order outranks every ordinary goal.
constexpr double kOrderScore = 10.0;

// What the faction's intent does to a squad's visits (intent flowing down):
// a faction that is defending, or mourning, keeps its squads closer to home.
double visitFactor(const state::FactionAI* faction)
{
	if (faction == nullptr)
		return 1.0;
	switch (faction->intent)
	{
	case state::Intent::Defend:
		return 0.4;
	case state::Intent::Mourn:
		return 0.7;
	default:
		return 1.0;
	}
}

// The towns as the wandering trader's pick sees them (trader::TownFacts): type and `is public` from the town's active record, the owner's
// `not real` / `fundamental type` (8 = bandit) / `default relation`, and what the owner thinks of the squad's faction.
std::vector<trader::TownFacts> circuitTowns(flecs::world& ecs, const kdata::GameData& data, const state::SquadInstance& squad,
	const std::vector<TownSite>& towns, const TownSite* current)
{
	const flecs::entity own = squad.faction.id != 0 ? flecs::entity(ecs.c_ptr(), squad.faction.id) : flecs::entity{};
	const state::FactionState* ownState = own.is_valid() ? own.get<state::FactionState>() : nullptr;
	const kdata::Record* ownRecord = ownState != nullptr ? data.find(ownState->templateSid) : nullptr;
	const bool squadBandit = ownRecord != nullptr && ownRecord->intOr("fundamental type", 0) == 8;
	std::vector<trader::TownFacts> out;
	out.reserve(towns.size());
	for (const TownSite& site : towns)
	{
		trader::TownFacts facts;
		facts.key = site.id;
		facts.x = site.x;
		facts.z = site.z;
		facts.isCurrent = current != nullptr && current->id == site.id;
		facts.squadBandit = squadBandit;
		const state::TownState* town = flecs::entity(ecs.c_ptr(), site.entity).get<state::TownState>();
		const kdata::Record* record = town != nullptr ? data.find(town->activeSid) : nullptr;
		facts.type = record != nullptr ? record->intOr("type", 2) : 2;
		facts.isPublic = record != nullptr && record->boolOr("is public", false);
		const flecs::entity owner = town != nullptr && town->owner.id != 0 ? flecs::entity(ecs.c_ptr(), town->owner.id) : flecs::entity{};
		const state::FactionState* ownerState = owner.is_valid() ? owner.get<state::FactionState>() : nullptr;
		facts.owned = ownerState != nullptr;
		if (ownerState != nullptr)
		{
			const kdata::Record* ownerRecord = data.find(ownerState->templateSid);
			facts.ownedByOwnFaction = owner.id() == squad.faction.id;
			facts.ownerNotReal = ownerRecord != nullptr && ownerRecord->boolOr("not real", false);
			facts.ownerBandit = ownerRecord != nullptr && ownerRecord->intOr("fundamental type", 0) == 8;
			facts.ownerAttitude = ownerRecord != nullptr ? ownerRecord->intOr("default relation", 0) : 0.0;
			facts.ownerIsPlayer = owner.has<state::PlayerFaction>();
			facts.relation = own.is_valid() ? factionRelation(data, owner, own) : facts.ownerAttitude;
		}
		out.push_back(facts);
	}
	return out;
}

bool isLocal(GoalKind kind)
{
	return kind == GoalKind::Patrol || kind == GoalKind::Roam;
}

double distance(double ax, double az, double bx, double bz)
{
	return std::hypot(ax - bx, az - bz);
}

double routeLength(const std::vector<GroundPoint>& route)
{
	double length = 0.0;
	for (std::size_t i = 1; i < route.size(); ++i)
		length += distance(route[i - 1].x, route[i - 1].z, route[i].x, route[i].z);
	return length;
}

// The point `along` units into the polyline, clamped to its ends.
GroundPoint pointAlong(const std::vector<GroundPoint>& route, double along)
{
	if (route.empty())
		return {};
	for (std::size_t i = 1; i < route.size(); ++i)
	{
		const double leg = distance(route[i - 1].x, route[i - 1].z, route[i].x, route[i].z);
		if (along <= leg)
		{
			const double t = leg > 0.0 ? along / leg : 0.0;
			return {route[i - 1].x + (route[i].x - route[i - 1].x) * t,
				route[i - 1].z + (route[i].z - route[i - 1].z) * t};
		}
		along -= leg;
	}
	return route.back();
}

double unit(std::uint64_t bits)
{
	return static_cast<double>(bits >> 11) * (1.0 / 9007199254740992.0);
}

} // namespace

double localRadius(Behaviour behaviour, bool homed) noexcept
{
	const Weights& weights = kWeights[static_cast<int>(behaviour)];
	return weights.local <= 0.0 ? 0.0 : homed ? weights.radiusHomed : weights.radiusWild;
}

struct UtilityGroupLayer::Candidate
{
	GoalKind kind = GoalKind::Idle;
	state::EntityRef target;
	double score = 0.0;
	double x = 0.0; // the target's position
	double z = 0.0;
	double hours = 0.0; // Idle: dwell
	std::uint64_t targetId = 0; // state::Id, the tie-break
	std::uint32_t index = 0; // Patrol: the loop point
	double leash = 0.0; // local goals: no route point may be further than this from the anchor
	// Local goals: the point is found (a Detour snap) only when the candidate is tried.
	bool lazy = false;
	double radius = 0.0;
	bool returning = false; // Raid: the way back to the origin
};

UtilityGroupLayer::UtilityGroupLayer(const Navigator* navigator,
	const std::vector<TownSite>& towns, std::uint64_t seed, bool tokenSpeeds)
	: navigator_(navigator)
	, towns_(towns)
	, seed_(seed)
	, tokenSpeeds_(tokenSpeeds)
{
}

double UtilityGroupLayer::tokenSpeed(double x, double z) const
{
	// "Inside a town" is the exe's town circle (towncircle.hpp: the Town radius), the rule every other consumer of getTown uses.
	const bool inTown = std::any_of(towns_.begin(), towns_.end(), [&](const TownSite& town)
	{
		return distance(x, z, town.x, town.z) <= town.radius;
	});
	return std::min(inTown ? kTokenTownSpeed : kTokenTravelSpeed, kTokenSpeedCap);
}

Traveller UtilityGroupLayer::traveller(const GroupContext& context, double toX, double toZ) const
{
	return Traveller{context.access == nullptr ? 0
		: context.access->travellerClass(context.squad.faction.id, context.x, context.z, toX, toZ)};
}

double UtilityGroupLayer::jitter(const GroupContext& context, std::uint64_t salt) const
{
	return unit(splitMix64(seed_ ^ splitMix64(context.squadId)
		^ splitMix64(context.ai.since * 0x9E3779B97F4A7C15ULL + salt)));
}

// The loop is the anchor, then points alternating between the inner (0.55) and
// outer ring at evenly spaced angles from a per-squad phase; every point is
// moved to the nearest walkable ground, so the loop follows the navmesh round
// the buildings. Pure function of (seed, squad, anchor, index).
std::optional<state::GroundPoint> UtilityGroupLayer::patrolPoint(const GroupContext& context,
	std::uint32_t index, double radius) const
{
	const std::uint32_t slot = index % kPatrolPoints;
	double x = context.anchorX;
	double z = context.anchorZ;
	if (slot != 0)
	{
		const double phase = unit(splitMix64(seed_ ^ splitMix64(context.squadId) ^ 0x9a7));
		const double angle = 2.0 * std::numbers::pi * (phase + static_cast<double>(slot) / kPatrolPoints);
		const double ring = radius * (slot % 2 == 1 ? 1.0 : 0.55);
		x += ring * std::cos(angle);
		z += ring * std::sin(angle);
	}
	const std::optional<state::GroundPoint> ground =
		navigator_->nearestReachableFor(traveller(context, x, z), x, z, radius * 0.4 + 32.0,
			context.x, context.z);
	if (!ground || distance(ground->x, ground->z, context.anchorX, context.anchorZ) > radius * kLocalLeash)
		return std::nullopt;
	return ground;
}

// A uniform point of the disc round the anchor, per selection (ai.since) and try.
std::optional<state::GroundPoint> UtilityGroupLayer::roamPoint(const GroupContext& context,
	std::uint64_t salt, double radius) const
{
	const std::uint64_t base = splitMix64(seed_ ^ splitMix64(context.squadId)
		^ splitMix64(context.ai.since * 0x9E3779B97F4A7C15ULL + 0x40a3 + salt));
	const double ring = radius * std::sqrt(unit(base));
	const double angle = 2.0 * std::numbers::pi * unit(splitMix64(base));
	const double goalX = context.anchorX + ring * std::cos(angle);
	const double goalZ = context.anchorZ + ring * std::sin(angle);
	const std::optional<state::GroundPoint> ground = navigator_->nearestReachableFor(
		traveller(context, goalX, goalZ), goalX, goalZ, radius * 0.2 + 32.0, context.x, context.z);
	if (!ground || distance(ground->x, ground->z, context.anchorX, context.anchorZ) > radius * kLocalLeash)
		return std::nullopt;
	return ground;
}

std::vector<UtilityGroupLayer::Candidate> UtilityGroupLayer::candidates(
	const GroupContext& context) const
{
	const state::GroupAI& ai = context.ai;
	std::vector<Candidate> list;

	// Behaviour in force: the mapped one only when the squad can act on it.
	const bool blind = !context.hasPosition || navigator_ == nullptr
		|| !navigator_->covers(context.x, context.z);
	const Behaviour behaviour = ai.fallback == AiReason::None
		? context.mapping.behaviour : Behaviour::Fallback;
	const Weights& weights = kWeights[static_cast<int>(behaviour)];

	Candidate idle;
	idle.kind = GoalKind::Idle;
	idle.score = weights.idle + weights.idleSpread * jitter(context, 0x1d1e);
	idle.hours = blind ? kBlindHours
		: weights.idleHoursMin + (weights.idleHoursMax - weights.idleHoursMin)
			* unit(splitMix64(seed_ ^ splitMix64(context.squadId) ^ splitMix64(context.now)));
	list.push_back(idle);
	if (blind)
		return list;

	if (context.hasPost)
	{
		Candidate post;
		post.kind = GoalKind::ManGate;
		post.target = state::EntityRef{0};
		post.score = kManGateScore;
		post.x = context.postX;
		post.z = context.postZ;
		post.targetId = context.postKey;
		list.push_back(post);
	}

	if (ai.order.status == state::OrderStatus::Issued)
	{
		const auto town = std::find_if(towns_.begin(), towns_.end(),
			[&](const TownSite& site) { return site.entity == ai.order.target.id; });
		if (town != towns_.end() && navigator_->covers(town->x, town->z))
		{
			Candidate raid;
			raid.kind = GoalKind::Raid;
			raid.target = ai.order.target;
			raid.score = kOrderScore;
			raid.x = town->x;
			raid.z = town->z;
			raid.targetId = town->id;
			list.push_back(raid);
		}
	}

	const bool away = context.hasHome
		&& distance(context.x, context.z, context.homeX, context.homeZ) > kHomeRadius;
	const double hoursAway = away
		? gameHoursIn(ticksSince(context.now, ai.homeTick)) : 0.0;

	if (away && context.squad.homeTown.id != 0
		&& navigator_->covers(context.homeX, context.homeZ))
	{
		Candidate home;
		home.kind = GoalKind::GoHome;
		home.target = context.squad.homeTown;
		home.score = weights.goHome + weights.goHomeGrowth * std::min(hoursAway, 48.0);
		home.x = context.homeX;
		home.z = context.homeZ;
		home.targetId = home.target.id;
		list.push_back(home);
	}
	if (weights.travel > 0.0)
	{
		for (const TownSite& town : towns_)
		{
			const double far = distance(context.x, context.z, town.x, town.z);
			// Not the town it stands in; not out of range; only where routes can end.
			if (far <= kHomeRadius || far > weights.range
				|| !navigator_->covers(town.x, town.z))
				continue;
			Candidate visit;
			visit.kind = GoalKind::TravelTo;
			visit.target = state::EntityRef{town.entity};
			visit.score = weights.travel * visitFactor(context.factionAi)
				+ weights.spread * jitter(context, town.id) - far / weights.distanceScale;
			visit.x = town.x;
			visit.z = town.z;
			visit.targetId = town.id;
			list.push_back(visit);
		}
	}

	if (weights.local > 0.0 && context.hasAnchor)
	{
		const double radius = context.hasHome ? weights.radiusHomed : weights.radiusWild;
		for (int i = 0; i < kLocalTries; ++i)
		{
			Candidate walk;
			walk.leash = radius * kLocalLeash;
			walk.score = weights.local + weights.localSpread * jitter(context, 0x100 + i)
				- 0.01 * i;
			walk.lazy = true;
			walk.radius = radius;
			if (behaviour == Behaviour::PatrolLocal)
			{
				walk.kind = GoalKind::Patrol;
				walk.index = ai.leg + static_cast<std::uint32_t>(i);
				walk.targetId = walk.index;
			} else {
				walk.kind = GoalKind::Roam;
				walk.targetId = static_cast<std::uint64_t>(i);
			}
			list.push_back(walk);
		}
	}

	// Commitment: the goal in hand keeps a bonus that fades with its age.
	const double age = gameHoursIn(ticksSince(context.now, ai.since));
	for (Candidate& candidate : list)
	{
		if (candidate.kind == ai.goal && candidate.target.id == ai.target.id
			&& candidate.kind != GoalKind::Idle)
			candidate.score += kCommitBonus * std::max(0.0, 1.0 - age / kCommitHours);
	}
	return list;
}

void UtilityGroupLayer::fail(GroupContext& context, GoalKind kind,
	state::EntityRef target, AiReason reason, GroupResult& result) const
{
	std::vector<state::GoalCooldown>& cooldowns = context.ai.cooldowns;
	// A blocked local point is skipped for this selection only: the next
	// one is offered next time (a cooldown on the kind would stop all of them).
	if (isLocal(kind))
	{
		++result.failures[static_cast<std::size_t>(reason)];
		return;
	}
	const std::uint64_t until = context.now + kCooldownHours * kTicksPerGameHour;
	const auto same = std::find_if(cooldowns.begin(), cooldowns.end(),
		[&](const state::GoalCooldown& entry)
		{
			return entry.kind == kind && entry.target.id == target.id;
		});
	if (same != cooldowns.end())
		same->until = until;
	else
		cooldowns.push_back(state::GoalCooldown{kind, target, until});
	++result.failures[static_cast<std::size_t>(reason)];
}

bool UtilityGroupLayer::adopt(GroupContext& context, const Candidate& candidate,
	GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	if (candidate.kind == GoalKind::Idle)
	{
		ai.route.clear();
		ai.length = 0.0;
		ai.along = 0.0;
		ai.until = context.now
			+ static_cast<std::uint64_t>(candidate.hours * kTicksPerHour);
	} else {
		const Traveller who = traveller(context, candidate.x, candidate.z);
		Route route = candidate.lazy
			? navigator_->localRouteFor(who, context.x, context.z, candidate.x, candidate.z)
			: navigator_->routeFor(who, context.x, context.z, candidate.x, candidate.z);
		ai.bashGate = 0;
		ai.bashUntil = 0;
		if (!route.found && route.failure == AiReason::DoorRefused && candidate.kind == GoalKind::Raid
			&& !candidate.returning && context.access != nullptr)
		{
			// A gate refuses the raid: it does not give up, it bashes the gate (DOORS.md section 6). The route ignores the
			// gates; the squad stops at the gate for the time the bashing takes, and the gate stays broken afterwards.
			// (The old rule, a refused route failing the order with a cooldown, is retired/ai_raid_door_refused.txt.)
			Traveller basher = who;
			basher.bash = true;
			const GateBash gate = context.access->bashing(context.squad.faction.id, candidate.x, candidate.z);
			route = navigator_->routeFor(basher, context.x, context.z, candidate.x, candidate.z);
			if (route.found)
			{
				ai.bashGate = gate.key;
				ai.bashSeconds = gate.seconds;
				// The squad meets the gate at the route point nearest to it.
				double along = 0.0;
				double best = 1.0e300;
				ai.bashAt = 0.0;
				for (std::size_t i = 0; i < route.points.size(); ++i)
				{
					if (i > 0)
						along += distance(route.points[i - 1].x, route.points[i - 1].z, route.points[i].x, route.points[i].z);
					const double far = distance(route.points[i].x, route.points[i].z, gate.x, gate.z);
					if (far < best)
					{
						best = far;
						ai.bashAt = along;
					}
				}
				// The route's own length sums the legs again and can differ in the last bit: a gate nearest the final point is
				// met at the end of the route (the walk clamps there, so an unclamped bashAt would never be reached).
				ai.bashAt = std::min(ai.bashAt, routeLength(route.points));
			}
		}
		if (!route.found)
		{
			if (candidate.kind == GoalKind::Raid)
			{
				// The order cannot be carried out: the faction reads Failed back.
				ai.order.status = state::OrderStatus::Failed;
				ai.order.failure = route.failure;
				++result.failures[static_cast<std::size_t>(route.failure)];
				ai.goal = GoalKind::None;
				return false;
			}
			fail(context, candidate.kind, candidate.target, route.failure, result);
			return false;
		}
		// A squad already past the leash (moved by something else) may walk
		// back, so the limit is never tighter than where it starts.
		const double limit = std::max(candidate.leash, 100.0 + distance(route.points.front().x,
			route.points.front().z, context.anchorX, context.anchorZ));
		if (candidate.leash > 0.0 && std::any_of(route.points.begin(), route.points.end(),
			[&](const GroundPoint& point)
			{
				return distance(point.x, point.z, context.anchorX, context.anchorZ) > limit;
			}))
		{
			fail(context, candidate.kind, candidate.target, AiReason::NoRoute, result);
			return false;
		}
		ai.route = route.points;
		ai.length = routeLength(ai.route);
		ai.along = 0.0;
		ai.until = 0;
		ai.goalX = candidate.x;
		ai.goalZ = candidate.z;
		if (candidate.kind == GoalKind::Patrol)
			ai.leg = candidate.index;
		if (candidate.kind == GoalKind::Raid)
			ai.order.status = candidate.returning ? state::OrderStatus::Returning
				: state::OrderStatus::Outbound;
		// The squad stands on the walkable point nearest where it was.
		if (context.x != ai.route.front().x || context.z != ai.route.front().z)
		{
			context.x = ai.route.front().x;
			context.z = ai.route.front().z;
			result.moved = true;
		}
		ai.progressTick = context.now;
	}
	ai.goal = candidate.kind;
	ai.target = candidate.target;
	ai.since = context.now;
	ai.lastStep = context.now;
	result.selected = true;
	return true;
}

void UtilityGroupLayer::select(GroupContext& context, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	// Why the generic fallback runs, if it does.
	if (!context.hasPosition)
		ai.fallback = AiReason::NoPosition;
	else if (navigator_ == nullptr)
		ai.fallback = AiReason::NoNavigation;
	else if (!navigator_->covers(context.x, context.z))
		ai.fallback = AiReason::OutsideWindow;
	else
		ai.fallback = context.mapping.behaviour == Behaviour::Fallback
			? context.mapping.fallback : AiReason::None;

	// An order the squad cannot act on (blind) fails now, so the faction knows.
	if (ai.order.status == state::OrderStatus::Issued
		&& (ai.fallback == AiReason::NoPosition || ai.fallback == AiReason::NoNavigation
			|| ai.fallback == AiReason::OutsideWindow))
	{
		ai.order.status = state::OrderStatus::Failed;
		ai.order.failure = ai.fallback;
		++result.failures[static_cast<std::size_t>(ai.fallback)];
	}

	// A raid whose target town is no longer in the world (a stale target, rwh.53 section 4.9) is not sent anywhere: the order fails, the
	// faction sees it and ends the campaign. The target is the order's own entity, never a pinned or re-resolved name.
	if (ai.order.status == state::OrderStatus::Issued && ai.fallback == AiReason::None
		&& std::none_of(towns_.begin(), towns_.end(), [&](const TownSite& site) { return site.entity == ai.order.target.id; }))
	{
		ai.order.status = state::OrderStatus::Failed;
		ai.order.failure = AiReason::NoTarget;
		++result.failures[static_cast<std::size_t>(AiReason::NoTarget)];
	}

	if (circuitActive(context))
	{
		circuitSelect(context, result);
		return;
	}

	std::vector<Candidate> list = candidates(context);
	std::sort(list.begin(), list.end(), [](const Candidate& left, const Candidate& right)
	{
		return std::tuple(-left.score, static_cast<int>(left.kind), left.targetId)
			< std::tuple(-right.score, static_cast<int>(right.kind), right.targetId);
	});
	for (Candidate& candidate : list)
	{
		const bool cooling = candidate.kind != GoalKind::Idle && std::any_of(
			ai.cooldowns.begin(), ai.cooldowns.end(),
			[&](const state::GoalCooldown& entry)
			{
				return entry.kind == candidate.kind && entry.target.id == candidate.target.id;
			});
		if (cooling)
			continue;
		if (candidate.lazy)
		{
			// Only the candidates tried pay for their snap; one on ground that
			// cannot be walked to is skipped.
			const std::optional<state::GroundPoint> point = candidate.kind == GoalKind::Patrol
				? patrolPoint(context, candidate.index, candidate.radius)
				: roamPoint(context, candidate.targetId, candidate.radius);
			if (!point)
				continue;
			candidate.x = point->x;
			candidate.z = point->z;
		}
		if (adopt(context, candidate, result))
		{
			// The debug view's sink (docs/formats/SIMLINK.md "DEBUG VIEW"): the pick and the top of the list it chose from.
			if (context.decisionSink != nullptr)
			{
				std::vector<GoalChoice> top;
				top.reserve(4);
				for (const Candidate& entry : list)
				{
					if (top.size() == 4)
						break;
					top.push_back({entry.kind, entry.target, entry.score});
				}
				(*context.decisionSink)(context.squadId, GoalChoice{candidate.kind, candidate.target, candidate.score}, top);
			}
			return;
		}
	}
	// Unreachable: Idle is always a candidate and cannot fail.
}

void UtilityGroupLayer::advance(GroupContext& context, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	const GroundPoint expected = pointAlong(ai.route, ai.along);
	if (distance(context.x, context.z, expected.x, expected.z) > (context.carried ? kCarryDisplaced : kDisplaced))
	{
		// Moved by something else: plan again from where it is, for the same goal.
		const state::EntityRef target = ai.target;
		const GoalKind kind = ai.goal;
		const auto town = std::find_if(towns_.begin(), towns_.end(),
			[&](const TownSite& site) { return site.entity == target.id; });
		Candidate again;
		again.kind = kind;
		again.target = target;
		if (kind == GoalKind::Raid)
		{
			// Outbound: the target town; returning: the origin.
			again.returning = ai.order.status == state::OrderStatus::Returning;
			again.x = again.returning ? ai.order.originX
				: town != towns_.end() ? town->x : ai.goalX;
			again.z = again.returning ? ai.order.originZ
				: town != towns_.end() ? town->z : ai.goalZ;
		} else if (isLocal(kind))
		{
			again.x = ai.goalX;
			again.z = ai.goalZ;
			again.index = ai.leg;
			again.lazy = true; // a local route, the point is already known
		} else if (kind == GoalKind::ManGate) {
			again.x = context.postX;
			again.z = context.postZ;
		} else if (kind == GoalKind::TravelTo && context.depot != 0 && town != towns_.end() && town->id == context.depot) {
			// The delivery's own goal point: the depot, or the mesh point toward it when the depot is off the mesh (k16.6.21 step 2).
			again.x = ai.goalX;
			again.z = ai.goalZ;
		} else {
			again.x = town != towns_.end() ? town->x : context.homeX;
			again.z = town != towns_.end() ? town->z : context.homeZ;
		}
		if (!adopt(context, again, result))
			select(context, result);
		return;
	}

	const double dt = static_cast<double>(ticksSince(context.now, ai.lastStep)) * kTickSeconds;
	const double before = ai.along;
	// A wandering trader keeps its own pace; a carried squad's carrot keeps the Fighters' pace (combat.cpp paces them at kWalkSpeed).
	const double speed = context.mapping.behaviour == Behaviour::TradeCircuit ? trader::kWanderSpeed
		: tokenSpeeds_ && !context.carried ? tokenSpeed(context.x, context.z) : kWalkSpeed;
	// Carried (the Fighters walk the squad): the progress is a carrot, advanced only while the leader is within the leash and nobody fights.
	// A delivery also waits for its slave on foot (k16.6.22): an awake slave follows its captor (171) at its own gait, which a wounded one cannot
	// keep at the caravan's pace, and it fell km behind and was lost. UNSURE: the exe's leader does not wait (rwh.9, a gap of 0.4-1.5 km).
	const bool keepsUp = !context.carried || (distance(context.x, context.z, expected.x, expected.z) <= kCarryLeash
		&& (context.depot == 0 || context.slaveGap <= kSlaveLeash));
	if (!context.holding && keepsUp)
		ai.along = std::min(ai.length, ai.along + speed * dt);
	if (context.holding)
		ai.progressTick = context.now;
	ai.lastStep = context.now;
	// A leader that cannot close to its carrot freezes `along` until the 6 h stuck timeout. After a few game-minutes of frozen
	// progress its walk has ended as an approach: it cannot get any closer (a detour round a mesh gap, a wall, the ground ending
	// short of the goal), and the exe's move ends where its path ends, not at the original point (MOVE_CLICK_LIVE.md "Arrival"),
	// so the leg ends here too and the layer moves on (a patrol leg to its next point). A raid order is not ended by a walk that
	// fell short: it fails as before. (A chase or a fight refreshes progressTick; so does walking.)
	if (!keepsUp && ticksSince(context.now, ai.progressTick) >= kCarryLeashFreezeTicks)
	{
		// A delivery waits for its leader (a capture task, the first aid of the captive, pulled it back from the carrot); the 6 h stuck rule
		// still ends it (k16.6.21 step 2: the leash fail put the depot on the 24 h cooldown and the caravan never left).
		if (context.depot != 0)
			return;
		if (ai.goal == GoalKind::Raid)
		{
			failOrder(context, AiReason::Stuck, result);
			return;
		}
		ai.along = ai.length;
		endLeg(context, result, true);
		return;
	}
	if (ai.bashGate != 0 && ai.along >= ai.bashAt)
	{
		// At the gate: bash until it breaks, then go on.
		ai.along = ai.bashAt;
		if (ai.bashUntil == 0)
			ai.bashUntil = context.now + static_cast<std::uint64_t>(ai.bashSeconds / kTickSeconds);
		if (context.now < ai.bashUntil)
		{
			// The squad stands at the gate: its position is the gate's point, or the next step reads it as displaced and re-plans
			// (dropping the bash) when the last walk step stopped short of it.
			const GroundPoint gate = pointAlong(ai.route, ai.along);
			if (!context.carried && (gate.x != context.x || gate.z != context.z))
			{
				context.x = gate.x;
				context.z = gate.z;
				result.moved = true;
			}
			ai.progressTick = context.now;
			return;
		}
		result.brokeGate = ai.bashGate;
		ai.bashGate = 0;
		ai.bashUntil = 0;
	}
	const GroundPoint here = pointAlong(ai.route, ai.along);
	if (!context.carried && (here.x != context.x || here.z != context.z))
	{
		context.x = here.x;
		context.z = here.z;
		result.moved = true;
	}
	if (ai.along - before > 0.5)
		ai.progressTick = context.now;

	// A carried squad has arrived when the leader is at the end too.
	if (ai.along >= ai.length && (!context.carried || distance(context.x, context.z, here.x, here.z) <= kCarryLeash))
	{
		endLeg(context, result, false);
	} else if (ticksSince(context.now, ai.progressTick) >= kStuckHours * kTicksPerGameHour) {
		if (ai.goal == GoalKind::Raid)
		{
			failOrder(context, AiReason::Stuck, result);
			return;
		}
		fail(context, ai.goal, ai.target, AiReason::Stuck, result);
		select(context, result);
	}
}

// The end of a leg. The exe's move task ends when the mover stops: its path ran out at the goal, or at the nearest point it
// can reach (MOVE_CLICK_LIVE.md "Arrival": HC+0x88, set when the path runs out; no distance to the original click is ever
// compared). Ours ends the leg here either way, so a patrol goes on to its next point instead of retrying a point the ground
// cannot deliver; a local goal stays in hand for that pick and any other goal is chosen afresh (Idle is its own goal, as the
// exe's IDLE fallback is).
void UtilityGroupLayer::endLeg(GroupContext& context, GroupResult& result, bool approached) const
{
	state::GroupAI& ai = context.ai;
	result.arrived = true;
	if (ai.goal == GoalKind::Raid)
	{
		raidArrived(context, result);
		return;
	}
	if (ai.goal == GoalKind::ManGate)
	{
		// At the post: hold it. The goal has no end (update() only watches for a squad moved off it).
		ai.route.clear();
		ai.length = 0.0;
		ai.along = 0.0;
		ai.progressTick = context.now;
		if (!approached)
		{
			ai.goalX = context.x; // where it stands (the post snapped to the mesh)
			ai.goalZ = context.z;
		}
		// An approach end leaves the post where it is: update() walks the crew back when it is outside kPostReach.
		return;
	}
	if (ai.goal == GoalKind::Patrol)
		++ai.leg;
	// The leg is over; a local goal in hand is not: the layer picks the next step with the goal's commitment still in force, so
	// a patrol goes on to its next point instead of drawing a fresh race against Idle (on the runtime mesh that race left the
	// watched squad Idle for the rest of a 4 h run after one arrival). Any other goal is picked afresh.
	if (!isLocal(ai.goal))
		ai.goal = GoalKind::None;
	ai.route.clear();
	if (context.depot != 0 && deliverySelect(context, result))
		return; // arrived at the depot: the hold, not an ordinary pick
	select(context, result);
}

void UtilityGroupLayer::failOrder(GroupContext& context, AiReason reason,
	GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	ai.order.status = state::OrderStatus::Failed;
	ai.order.failure = reason;
	++result.failures[static_cast<std::size_t>(reason)];
	ai.goal = GoalKind::None;
	ai.route.clear();
	select(context, result);
}

// The end of a raid leg: outbound -> stay at the target; returning -> home.
void UtilityGroupLayer::raidArrived(GroupContext& context, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	if (ai.order.status == state::OrderStatus::Outbound)
	{
		raidStay(context, result);
		return;
	}
	ai.order.status = state::OrderStatus::Done;
	ai.goal = GoalKind::None;
	ai.route.clear();
	select(context, result);
}

void UtilityGroupLayer::raidStay(GroupContext& context, GroupResult&) const
{
	state::GroupAI& ai = context.ai;
	ai.order.status = state::OrderStatus::Staying;
	ai.route.clear();
	ai.length = 0.0;
	ai.along = 0.0;
	ai.until = context.now + ai.order.stay;
	ai.progressTick = context.now;
}

bool UtilityGroupLayer::deliverySelect(GroupContext& context, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	if (navigator_ == nullptr || !context.hasPosition)
		return false;
	const auto site = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.id == context.depot; });
	if (site == towns_.end())
		return false;
	// Inside the depot town the squad holds (the leader is then free to walk to the storage keeper): a short idle, renewed every step until
	// the package finishes. A circuit squad stamps this arrival when its stay is picked afterwards (circuitSelect).
	if (distance(context.x, context.z, site->x, site->z) <= kHomeRadius)
	{
		ai.route.clear();
		ai.length = 0.0;
		ai.along = 0.0;
		ai.goal = GoalKind::Idle;
		ai.target = state::EntityRef{site->entity};
		ai.until = context.now + static_cast<std::uint64_t>(kTicksPerHour / 60);
		ai.since = context.now;
		ai.lastStep = context.now;
		ai.progressTick = context.now;
		if (context.trip != nullptr)
		{
			context.trip->staying = false;
			context.trip->town = ai.target;
		}
		return true;
	}
	if (ai.goal == GoalKind::TravelTo && ai.target.id == site->entity && !ai.route.empty())
	{
		advance(context, result);
		return true;
	}
	if (tokenLeg(ai) && ai.target.id == site->entity && tokenStep(context, *site, result))
		return true;
	// A failed walk (no route, or the depot off the mesh) cools down like any travel goal: the squad lives its ordinary life meanwhile
	// instead of routing again every step.
	const bool cooling = std::any_of(ai.cooldowns.begin(), ai.cooldowns.end(), [&](const state::GoalCooldown& entry)
	{
		return entry.kind == GoalKind::TravelTo && entry.target.id == site->entity;
	});
	const bool holding = ai.goal == GoalKind::Idle && ai.target.id == site->entity; // at the mesh edge (the in-town hold returned above)
	if (cooling)
	{
		if (holding || (ai.goal == GoalKind::None && ai.target.id == site->entity))
		{
			// The edge walk arrived (the goal was just cleared, its target the depot): hold there, renewed every step like the town hold.
			result.deliveryFailed = holding ? AiReason::None : AiReason::OutsideWindow;
			result.deliveryEdge = holding ? 0 : 2;
			// An off-mesh depot: the squad goes on from the edge as a token (k16.7.15).
			if (!navigator_->covers(site->x, site->z) && startTokenLeg(context, *site, result))
				return true;
			ai.route.clear();
			ai.length = 0.0;
			ai.along = 0.0;
			ai.goal = GoalKind::Idle;
			ai.until = context.now + static_cast<std::uint64_t>(kTicksPerHour / 60);
			ai.since = context.now;
			ai.lastStep = context.now;
			ai.progressTick = context.now;
			return true;
		}
		return false;
	}
	Candidate delivery;
	delivery.kind = GoalKind::TravelTo;
	delivery.target = state::EntityRef{site->entity};
	delivery.x = site->x;
	delivery.z = site->z;
	delivery.targetId = site->id;
	const auto failedBefore = result.failures;
	if (!adopt(context, delivery, result))
	{
		result.deliveryFailed = AiReason::NoRoute;
		for (std::size_t i = 0; i < failedBefore.size(); ++i)
		{
			if (result.failures[i] != failedBefore[i])
				result.deliveryFailed = static_cast<AiReason>(i);
		}
		// The retry after the cooldown of a squad holding at the edge that finds nothing nearer: it keeps holding (silently, the line was logged).
		if (deliveryEdge(context, *site, result))
		{
			result.deliveryEdge = 1;
			return true;
		}
		if (!navigator_->covers(site->x, site->z) && startTokenLeg(context, *site, result))
			return true;
		if (holding)
		{
			result.deliveryFailed = AiReason::None;
			ai.until = context.now + static_cast<std::uint64_t>(kTicksPerHour / 60);
			ai.goal = GoalKind::Idle;
			return true;
		}
		return false;
	}
	if (context.trip != nullptr)
	{
		context.trip->staying = false;
		context.trip->town = delivery.target;
	}
	return true;
}

bool UtilityGroupLayer::deliveryEdge(GroupContext& context, const TownSite& site, GroupResult& result) const
{
	const double toDepot = distance(context.x, context.z, site.x, site.z);
	if (toDepot <= kEdgeMinProgress)
		return false;
	const double dx = (site.x - context.x) / toDepot;
	const double dz = (site.z - context.z) / toDepot;
	const Traveller who = traveller(context, site.x, site.z);
	std::optional<GroundPoint> best;
	double bestProgress = kEdgeMinProgress;
	for (double along = kEdgeStep; along < toDepot; along += kEdgeStep)
	{
		const double px = context.x + dx * along;
		const double pz = context.z + dz * along;
		if (!navigator_->covers(px, pz))
			continue;
		const std::optional<GroundPoint> ground = navigator_->nearestReachableFor(who, px, pz, kEdgeStep * 0.5, context.x, context.z);
		if (!ground)
			continue;
		const double progress = toDepot - distance(ground->x, ground->z, site.x, site.z);
		if (progress > bestProgress)
		{
			bestProgress = progress;
			best = ground;
		}
	}
	if (!best)
		return false;
	Candidate edge;
	edge.kind = GoalKind::TravelTo;
	edge.target = state::EntityRef{site.entity};
	edge.x = best->x;
	edge.z = best->z;
	edge.targetId = site.id;
	if (!adopt(context, edge, result))
		return false;
	if (context.trip != nullptr)
	{
		context.trip->staying = false;
		context.trip->town = edge.target;
	}
	return true;
}

static void stampStay(state::TraderTrip& trip, state::EntityRef town, double hours, std::uint64_t seed, std::uint64_t squadId);

bool UtilityGroupLayer::startTokenLeg(GroupContext& context, const TownSite& site, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	if (!context.hasPosition)
		return false;
	ai.route.clear();
	ai.goal = GoalKind::TravelTo;
	ai.target = state::EntityRef{site.entity};
	ai.goalX = site.x;
	ai.goalZ = site.z;
	ai.length = std::max(1.0, distance(context.x, context.z, site.x, site.z));
	ai.along = 0.0;
	ai.since = context.now;
	ai.lastStep = context.now;
	ai.progressTick = context.now;
	result.selected = true;
	return true;
}

bool UtilityGroupLayer::tokenStep(GroupContext& context, const TownSite& site, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	if (context.carried)
	{
		// Still built: the demote pass takes it as a token (residents.cpp). Promoted back onto the mesh away from where the leg began, the squad
		// walks the mesh again and the leg is dropped.
		if (navigator_->covers(context.x, context.z) && ai.along > kTokenLegFree)
		{
			ai.goal = GoalKind::None;
			ai.length = 0.0;
			ai.along = 0.0;
			if (deliveryEdge(context, site, result))
				return true;
			startTokenLeg(context, site, result); // nothing nearer on this ground: on as a token again (it is demoted next pass)
			ai.along = kTokenLegFree;
			return true;
		}
		ai.lastStep = context.now;
		ai.progressTick = context.now;
		return true;
	}
	const double dt = static_cast<double>(ticksSince(context.now, ai.lastStep)) * kTickSeconds;
	const double left = distance(context.x, context.z, site.x, site.z);
	// The token speed of the leg (setting ai.token_speeds only chooses the speed of the legs on the mesh).
	const double step = std::min(left, std::min(tokenSpeed(context.x, context.z), kTokenSpeedCap) * dt);
	if (step > 0.0)
	{
		context.x += (site.x - context.x) / left * step;
		context.z += (site.z - context.z) / left * step;
		ai.along += step;
		result.moved = true;
		ai.progressTick = context.now;
	}
	ai.lastStep = context.now;
	if (distance(context.x, context.z, site.x, site.z) > 1.0e-6)
		return true;
	// Arrived: the leg ends in the town.
	ai.goal = GoalKind::None;
	ai.length = 0.0;
	ai.along = 0.0;
	result.arrived = true;
	return true;
}

bool UtilityGroupLayer::tokenCircuit(GroupContext& context, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	if (context.mapping.behaviour != Behaviour::TradeCircuit || context.trip == nullptr || context.townFacts == nullptr || !context.hasPosition
		|| navigator_ == nullptr || ai.order.status == state::OrderStatus::Issued || ai.goal == GoalKind::Raid)
		return false;
	state::TraderTrip& trip = *context.trip;
	const auto site = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.entity == trip.town.id; });
	if (site == towns_.end())
		return false;
	const bool onMesh = navigator_->covers(context.x, context.z);
	const bool leg = tokenLeg(ai) && ai.target.id == site->entity;
	// The token's life: the leg to an off-mesh town, or the stay in one the squad stands off the mesh in.
	if (!leg && !(trip.staying && !onMesh && !navigator_->covers(site->x, site->z)))
		return false;
	if (leg)
	{
		// A leg that came back onto the mesh toward a covered town is walked by the mesh from here.
		if (!context.carried && onMesh && navigator_->covers(site->x, site->z))
		{
			ai.goal = GoalKind::None;
			ai.length = 0.0;
			ai.along = 0.0;
			return false;
		}
		if (!tokenStep(context, *site, result))
			return false;
		if (ai.goal == GoalKind::None && result.arrived)
		{
			// The stay starts where the token arrived, as the circuit's arrival does (circuitSelect).
			const double hours = trader::gameHours(context.now);
			stampStay(trip, state::EntityRef{site->entity}, hours, seed_, context.squadId);
			ai.goal = GoalKind::Idle;
			ai.target = trip.town;
			ai.until = context.now + std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(trip.stayHours * kTicksPerHour)));
			ai.since = context.now;
			ai.lastStep = context.now;
			ai.progressTick = context.now;
		}
		return true;
	}
	// The stay: until the departure, then the next pick, uniform among the 4 nearest eligible towns, as a leg from here.
	const double hours = trader::gameHours(context.now);
	const double leave = trader::departureTime(trip.arrivedAt, trip.stayHours);
	if (hours < leave)
	{
		ai.goal = GoalKind::Idle;
		ai.target = trip.town;
		ai.until = context.now + std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((leave - hours) * kTicksPerHour)));
		ai.since = context.now;
		ai.lastStep = context.now;
		return true;
	}
	const std::vector<trader::TownFacts> facts = (*context.townFacts)(&*site);
	const std::vector<std::size_t> nearest4 = trader::nearestEligible(facts, context.x, context.z);
	if (!nearest4.empty())
	{
		const std::size_t first = std::min(nearest4.size() - 1, static_cast<std::size_t>(
			unit(splitMix64(seed_ ^ splitMix64(context.squadId) ^ splitMix64(0xc17c0000ULL + trip.arrivals))) * static_cast<double>(nearest4.size())));
		const std::uint64_t key = facts[nearest4[first]].key;
		const auto pick = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.id == key; });
		if (pick != towns_.end() && startTokenLeg(context, *pick, result))
		{
			trip.lastTown = trip.town;
			trip.staying = false;
			trip.town = state::EntityRef{pick->entity};
			return true;
		}
	}
	ai.goal = GoalKind::Idle;
	ai.until = context.now + static_cast<std::uint64_t>(kTicksPerHour);
	ai.since = context.now;
	ai.lastStep = context.now;
	return true;
}

// The arrival stamp of a trade circuit: the stay of the town (TOWN_LIFE_SLICE 4.2) starts now.
static void stampStay(state::TraderTrip& trip, state::EntityRef town, double hours, std::uint64_t seed, std::uint64_t squadId)
{
	trip.staying = true;
	trip.town = town;
	trip.arrivedAt = hours;
	trip.stayHours = trader::stayHours(unit(splitMix64(seed ^ splitMix64(squadId) ^ splitMix64(0x57a10000ULL + trip.arrivals))));
	++trip.arrivals;
}

bool UtilityGroupLayer::circuitActive(const GroupContext& context) const
{
	return context.mapping.behaviour == Behaviour::TradeCircuit && context.trip != nullptr && context.townFacts != nullptr
		&& context.hasPosition && navigator_ != nullptr && navigator_->covers(context.x, context.z)
		&& context.ai.order.status != state::OrderStatus::Issued && context.ai.goal != GoalKind::Raid;
}

void UtilityGroupLayer::circuitSelect(GroupContext& context, GroupResult& result) const
{
	state::GroupAI& ai = context.ai;
	state::TraderTrip& trip = *context.trip;
	ai.fallback = AiReason::None;
	const double hours = trader::gameHours(context.now);

	// The town it stands in.
	const TownSite* here = nullptr;
	double nearest = UtilityGroupLayer::kHomeRadius;
	for (const TownSite& town : towns_)
	{
		const double far = distance(context.x, context.z, town.x, town.z);
		if (far <= nearest)
		{
			nearest = far;
			here = &town;
		}
	}
	// A staying squad at its table may stand outside the town radius (the bar finder takes a BAR placement up to kPlacementReach from the town
	// point): the town it stays in keeps it for that distance (TOWN_LIFE_SLICE 5.17).
	if (here == nullptr && trip.staying)
	{
		const auto stay = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.entity == trip.town.id; });
		if (stay != towns_.end() && distance(context.x, context.z, stay->x, stay->z)
			<= std::max(UtilityGroupLayer::kHomeRadius + routines::kPlacementReach, stay->radius)) // the bar finder's reach is the town circle (k16.6.254)
			here = &*stay;
	}
	// The no-town -> town transition is the only thing that stamps an arrival (kinspect item 54 TQ10): the squad is in the town it was heading
	// for (or has no target yet). Passing through another town on the way is not one, and a failed departure from a town does not re-stamp.
	if (here != nullptr && !trip.staying && (trip.town.id == 0 || trip.town.id == here->entity))
	{
		stampStay(trip, state::EntityRef{here->entity}, hours, seed_, context.squadId);
	}
	if (trip.staying && here == nullptr)
		trip.staying = false; // moved out of its town by something else: no stay left to keep
	if (trip.staying)
	{
		const double leave = trader::departureTime(trip.arrivedAt, trip.stayHours);
		if (hours < leave)
		{
			// The bar hangout (TOWN_LIFE_SLICE 5.17, T3d): the stay is spent at a table of the town, one claimed per squad. A squad that cannot
			// get one (no table in the town, or no resident data) stays where it stands, as in T2; no walk the navigator refuses is retried.
			if (context.bar != nullptr && context.hasPosition)
			{
				const std::optional<std::array<double, 2>> table = context.bar->claim(context.squadEntity, here->entity);
				if (table && distance(context.x, context.z, (*table)[0], (*table)[1]) > kBarReach)
				{
					Candidate walk;
					walk.kind = GoalKind::TravelTo;
					walk.target = trip.town;
					walk.x = (*table)[0];
					walk.z = (*table)[1];
					walk.targetId = here->id;
					const std::size_t cooldowns = ai.cooldowns.size();
					if (adopt(context, walk, result))
						return;
					// A refused route put the town on the destination cooldown; the table is not a destination, so take that back.
					ai.cooldowns.resize(cooldowns);
					context.bar->release(context.squadEntity);
				}
			}
			ai.route.clear();
			ai.length = 0.0;
			ai.along = 0.0;
			ai.until = context.now + std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil((leave - hours) * kTicksPerHour)));
			// A squad stamped on its first step (spawned in a town: a scenario, a load) has no SquadRoutine yet (attachBarTables adds it a step
			// later), so the claim above found nothing for that reason alone: look again at the next squad step instead of standing the whole stay
			// (k16.6.254).
			if (context.bar != nullptr && !context.squadEntity.has<state::SquadRoutine>())
				ai.until = std::min(ai.until, context.now + kBarRetryTicks);
			ai.goal = GoalKind::Idle;
			ai.target = trip.town;
			ai.since = context.now;
			ai.lastStep = context.now;
			ai.progressTick = context.now;
			result.selected = true;
			return;
		}
	}

	// An off-mesh town the squad is already heading for (the edge walk ended): it goes on to it, it does not pick again (k16.7.15).
	if (!trip.staying && trip.town.id != 0)
	{
		const auto goal = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.entity == trip.town.id; });
		if (goal != towns_.end() && !navigator_->covers(goal->x, goal->z) && (here == nullptr || here->entity != goal->entity)
			&& (deliveryEdge(context, *goal, result) || startTokenLeg(context, *goal, result)))
			return;
	}
	// Leaving, or in the open: the table is given back (the claim lives as long as the stay), then one of the 4 nearest eligible towns, uniform.
	// The others are the fallbacks if the route cannot be found.
	if (context.bar != nullptr)
		context.bar->release(context.squadEntity);
	// A delivery (T4c, TOWN_LIFE_SLICE 7.4): a caravan carrying an order leaves for the town that needs the goods, not a neighbour.
	// It is the circuit's own walk and arrival stamp; a route that cannot be found falls through to the ordinary pick, so the order
	// stays with it and the next departure tries again.
	if (const state::CaravanOrder* order = context.squadEntity.get<state::CaravanOrder>();
		order != nullptr && order->phase != state::kCaravanUnload && order->to.id != 0
			&& (here == nullptr || here->entity != order->to.id))
	{
		const auto site = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.entity == order->to.id; });
		if (site != towns_.end() && navigator_->covers(site->x, site->z))
		{
			Candidate delivery;
			delivery.kind = GoalKind::TravelTo;
			delivery.target = state::EntityRef{site->entity};
			delivery.x = site->x;
			delivery.z = site->z;
			delivery.targetId = site->id;
			if (adopt(context, delivery, result))
			{
				if (trip.staying)
					trip.lastTown = trip.town;
				trip.staying = false;
				trip.town = delivery.target;
				return;
			}
		}
	}
	const std::vector<trader::TownFacts> facts = (*context.townFacts)(here);
	const std::vector<std::size_t> nearest4 = trader::nearestEligible(facts, context.x, context.z);
	if (!nearest4.empty())
	{
		const std::size_t first = std::min(nearest4.size() - 1, static_cast<std::size_t>(
			unit(splitMix64(seed_ ^ splitMix64(context.squadId) ^ splitMix64(0xc17c0000ULL + trip.arrivals))) * static_cast<double>(nearest4.size())));
		for (std::size_t k = 0; k < nearest4.size(); ++k)
		{
			const std::uint64_t key = facts[nearest4[(first + k) % nearest4.size()]].key;
			const auto site = std::find_if(towns_.begin(), towns_.end(), [&](const TownSite& town) { return town.id == key; });
			if (site == towns_.end())
				continue;
			const bool cooling = std::any_of(ai.cooldowns.begin(), ai.cooldowns.end(), [&](const state::GoalCooldown& entry)
			{
				return entry.kind == GoalKind::TravelTo && entry.target.id == site->entity;
			});
			if (cooling)
				continue;
			Candidate visit;
			visit.kind = GoalKind::TravelTo;
			visit.target = state::EntityRef{site->entity};
			visit.x = site->x;
			visit.z = site->z;
			visit.targetId = site->id;
			// A town off the streamed mesh (k16.7.12): no route exists, so the walk goes to the mesh point nearest it (the depot's edge walk). The
			// pick is not steered to covered towns; a squad that already stands at the edge finds nothing nearer and tries the next.
			const state::TraderTrip before = trip;
			if (navigator_->covers(site->x, site->z) ? adopt(context, visit, result)
				: deliveryEdge(context, *site, result) || startTokenLeg(context, *site, result))
			{
				if (before.staying)
					trip.lastTown = before.town;
				trip.staying = false;
				trip.town = visit.target;
				return;
			}
		}
	}
	// No eligible town, or no route to any: rest and look again in a game-hour (never a terminal state).
	Candidate rest;
	rest.kind = GoalKind::Idle;
	rest.hours = 1.0;
	adopt(context, rest, result);
}

GroupResult UtilityGroupLayer::update(GroupContext& context)
{
	GroupResult result;
	state::GroupAI& ai = context.ai;
	const std::uint64_t previousStep = ai.lastStep;
	if (context.hasHome && context.hasPosition
		&& distance(context.x, context.z, context.homeX, context.homeZ) <= kHomeRadius)
		ai.homeTick = context.now;
	ai.cooldowns.erase(std::remove_if(ai.cooldowns.begin(), ai.cooldowns.end(),
		[&](const state::GoalCooldown& entry) { return entry.until <= context.now; }),
		ai.cooldowns.end());
	// Not by target: EntityRef::id is the flecs entity, which a load renumbers. The vector is saved in order, so a stable sort on
	// (kind, until) is the same before and after a load.
	std::stable_sort(ai.cooldowns.begin(), ai.cooldowns.end(), [](const auto& left, const auto& right)
	{
		return std::tuple(static_cast<int>(left.kind), left.until) < std::tuple(static_cast<int>(right.kind), right.until);
	});

	// A new order interrupts whatever the squad is doing (the faction's intent
	// outranks ordinary goals); None falls through to a fresh selection.
	if (ai.order.status == state::OrderStatus::Issued && ai.goal != GoalKind::Raid
		&& ai.goal != GoalKind::None)
	{
		ai.goal = GoalKind::None;
		ai.route.clear();
	}

	if (context.depot != 0 && deliverySelect(context, result))
		return result;

	if (tokenCircuit(context, result))
		return result;

	if (circuitActive(context))
	{
		// A wandering trader: the circuit's goals are a walk to the picked town (TravelTo) and the stay (Idle until the departure).
		switch (ai.goal)
		{
		case GoalKind::TravelTo:
			advance(context, result);
			break;
		case GoalKind::Idle:
			if (context.now >= ai.until)
				select(context, result);
			break;
		default:
			// None (the first step, a goal that ended), or a goal left over from before it became a circuit squad.
			if (ai.since == 0 && navigator_ != nullptr)
			{
				const std::optional<GroundPoint> ground = navigator_->nearestWalkable(context.x, context.z, kSnapReach);
				if (ground && (ground->x != context.x || ground->z != context.z))
				{
					context.x = ground->x;
					context.z = ground->z;
					result.moved = true;
				}
			}
			ai.goal = GoalKind::None;
			ai.route.clear();
			select(context, result);
			break;
		}
		return result;
	}

	switch (ai.goal)
	{
	case GoalKind::None:
		// First step: stand on the navmesh (a wilderness den is only a point
		// of the biome map, a town centre may be inside a building).
		if (ai.since == 0 && context.hasPosition && navigator_ != nullptr)
		{
			const std::optional<GroundPoint> ground =
				navigator_->nearestWalkable(context.x, context.z, kSnapReach);
			if (ground && (ground->x != context.x || ground->z != context.z))
			{
				context.x = ground->x;
				context.z = ground->z;
				result.moved = true;
			}
		}
		select(context, result);
		break;
	case GoalKind::Idle:
		if (context.now >= ai.until)
			select(context, result);
		break;
	case GoalKind::Raid:
		if (ai.order.status == state::OrderStatus::Failed
			|| ai.order.status == state::OrderStatus::Done
			|| ai.order.status == state::OrderStatus::None)
		{
			// The order was withdrawn or cleared from outside: ordinary life.
			ai.goal = GoalKind::None;
			ai.route.clear();
			select(context, result);
		} else if (context.now >= ai.order.deadline) {
			failOrder(context, AiReason::Expired, result);
		} else if (ai.order.status == state::OrderStatus::Staying
			|| ai.order.status == state::OrderStatus::Retreating) {
			if (ai.order.status == state::OrderStatus::Staying && context.now >= ai.until
				&& ai.order.retreat > 0)
			{
				// On-site time over: a short retreat hold, then the way home.
				ai.order.status = state::OrderStatus::Retreating;
				ai.until = context.now + ai.order.retreat;
			} else if (context.now >= ai.until) {
				// Home again: the way back is a route like any other.
				Candidate back;
				back.kind = GoalKind::Raid;
				back.target = ai.order.target;
				back.returning = true;
				back.x = ai.order.originX;
				back.z = ai.order.originZ;
				// A route that cannot be found marks the order Failed (adopt).
				if (!adopt(context, back, result))
					select(context, result);
			}
		} else {
			advance(context, result);
		}
		break;
	case GoalKind::ManGate:
		if (!context.hasPost)
		{
			// The crew is gone (a squad no longer a gate crew): ordinary life.
			ai.goal = GoalKind::None;
			ai.route.clear();
			select(context, result);
		} else if (ai.route.empty()) {
			// Holding the post. Moved off it by something else: walk back.
			if (distance(context.x, context.z, ai.goalX, ai.goalZ) > kPostReach)
			{
				ai.goal = GoalKind::None;
				select(context, result);
			}
		} else {
			advance(context, result);
		}
		break;
	case GoalKind::GoHome:
	case GoalKind::TravelTo:
	case GoalKind::Patrol:
	case GoalKind::Roam:
		advance(context, result);
		// Once a game-hour on the way: has staying home grown more urgent than
		// the trip (commitment keeps the trip unless it clearly has)?
		if (ai.goal == GoalKind::TravelTo && !result.selected && !result.arrived
			&& context.now / kTicksPerGameHour != previousStep / kTicksPerGameHour)
		{
			const std::vector<Candidate> list = candidates(context);
			const Candidate* home = nullptr;
			const Candidate* trip = nullptr;
			for (const Candidate& candidate : list)
			{
				if (candidate.kind == GoalKind::GoHome)
					home = &candidate;
				if (candidate.kind == ai.goal && candidate.target.id == ai.target.id)
					trip = &candidate;
			}
			const bool cooling = home != nullptr && std::any_of(ai.cooldowns.begin(),
				ai.cooldowns.end(), [](const state::GoalCooldown& entry)
				{
					return entry.kind == GoalKind::GoHome;
				});
			if (home != nullptr && trip != nullptr && !cooling && home->score > trip->score)
				adopt(context, *home, result);
		}
		break;
	}
	return result;
}

namespace {

// The script text the faction layer runs, or none (empty source) for the native scoring.
FactionScriptSetup factionScript(const BehaviourScripts& scripts,
	const std::function<void(std::string_view)>& log)
{
	FactionScriptSetup setup;
	setup.log = log;
	setup.limits = scripts.limits;
	bool scripted = scripts.mode == BehaviourScripts::Mode::Scripted;
	if (scripts.mode == BehaviourScripts::Mode::Default)
	{
		const char* choice = std::getenv("KSIM_BEHAVIOUR");
		scripted = choice == nullptr || std::string_view(choice) != "native";
	}
	if (!scripted)
		return setup;
	if (!scripts.factionIntentSource.empty())
	{
		setup.name = scripts.factionIntentName;
		setup.source = scripts.factionIntentSource;
		return setup;
	}
	const std::string path = !scripts.factionIntentPath.empty() ? scripts.factionIntentPath
		: simDataBehaviourDir() + "/faction_intent.lua";
	setup.name = path.substr(path.find_last_of('/') + 1);
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		const std::string message = "faction script " + path + " cannot be read; the native scoring is used";
		if (log)
			log(message);
		else
			std::cerr << "ksim: " << message << "\n";
		return setup;
	}
	setup.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return setup;
}

} // namespace

AiSystem::AiSystem(Config config, std::uint64_t seed)
	: config_(std::move(config))
	, seed_(seed)
	, group_(std::make_unique<UtilityGroupLayer>(config_.navigator.get(), towns_, seed, config_.tokenSpeeds))
{
	struct NoFaction final : FactionLayer
	{
		FactionResult update(FactionContext&) override { return {}; }
	};
	if (config_.factions != nullptr)
	{
		auto utility = std::make_unique<UtilityFactionLayer>(config_.factions,
			config_.navigator.get(), towns_, seed, config_.factionTuning, config_.areas,
			factionScript(config_.scripts, config_.log));
		utility_ = utility.get();
		faction_ = std::move(utility);
	} else {
		faction_ = std::make_unique<NoFaction>();
	}
	if (config_.doors != nullptr)
		doors_ = std::make_unique<DoorSystem>(config_.doors, config_.factions, config_.doorTuning);
	// The town routines' compiled lists (T3f): the unloaded squads' phase reads them.
	if (config_.data != nullptr)
		packages_ = std::make_unique<routines::PackageTable>(*config_.data);
}

void AiSystem::updateTownPhase(flecs::entity squad, std::string_view package, double hour)
{
	const state::Id* id = squad.get<state::Id>();
	if (id == nullptr)
		return;
	const routines::CompiledPackage* compiled = packages_ != nullptr && !package.empty() ? packages_->find(package) : nullptr;
	if (compiled == nullptr)
	{
		townLife_.erase(id->value);
		return;
	}
	std::size_t unmodelled = 0;
	const routines::TownPhase phase = routines::townPhase(*compiled, hour, &unmodelled);
	TownLife& life = townLife_[id->value];
	life.phase = static_cast<std::uint8_t>(phase);
	life.unmodelled = static_cast<std::uint32_t>(unmodelled);
}

const FactionStats* AiSystem::factionStats() const noexcept
{
	return utility_ != nullptr ? &utility_->stats() : nullptr;
}

void AiSystem::setFactionLayer(std::unique_ptr<FactionLayer> layer)
{
	faction_ = std::move(layer);
	utility_ = nullptr;
}

void AiSystem::refresh(flecs::world& ecs)
{
	const std::size_t count = static_cast<std::size_t>(ecs.count<state::SquadInstance>());
	if (count == knownSquads_)
		return;
	knownSquads_ = count;
	squads_.clear();
	ecs.each([this](flecs::entity entity, const state::Id& id, const state::SquadInstance&)
	{
		squads_.emplace_back(id.value, entity.id());
	});
	std::sort(squads_.begin(), squads_.end());
	// The unloaded phases (T3f) follow the squad list: a squad that is gone loses its entry.
	for (auto life = townLife_.begin(); life != townLife_.end();)
	{
		const auto squad = std::lower_bound(squads_.begin(), squads_.end(), life->first,
			[](const std::pair<std::uint64_t, flecs::entity_t>& row, std::uint64_t id) { return row.first < id; });
		life = squad != squads_.end() && squad->first == life->first ? std::next(life) : townLife_.erase(life);
	}
	towns_.clear();
	ecs.each([this](flecs::entity entity, const state::Id& id, const state::TownState& town)
	{
		const double radius = config_.data != nullptr ? towncircle::of(*config_.data, town).radius : 0.0;
		towns_.push_back(TownSite{id.value, entity.id(), town.x, town.z, radius});
	});
	std::sort(towns_.begin(), towns_.end(),
		[](const TownSite& left, const TownSite& right) { return left.id < right.id; });
}

bool AiSystem::carrot(const state::GroupAI& ai, double lookahead, double& x, double& z)
{
	if (ai.route.empty())
		return false;
	// Not past a gate that is being bashed.
	const double limit = ai.bashGate != 0 ? ai.bashAt : ai.length;
	const GroundPoint point = pointAlong(ai.route, std::min(limit, ai.along + lookahead));
	x = point.x;
	z = point.z;
	return true;
}

const Mapping& AiSystem::squadMapping(const flecs::world& ecs, const state::SquadInstance& instance) const
{
	if (instance.faction.id == 0 || config_.data == nullptr)
		return config_.registry->squad(instance.templateSid);
	auto found = fallbackPackages_.find(instance.faction.id);
	if (found == fallbackPackages_.end())
	{
		// 140271fd0: the faction's `AI fallback`, the first entry.
		std::string sid;
		const flecs::entity faction(ecs.c_ptr(), instance.faction.id);
		const state::FactionState* state = faction.is_valid() ? faction.get<state::FactionState>() : nullptr;
		const kdata::Record* record = state != nullptr ? config_.data->find(state->templateSid) : nullptr;
		const kdata::RefCategory* fallback = record != nullptr ? record->findRefs("AI fallback") : nullptr;
		if (fallback != nullptr && !fallback->entries.empty())
			sid = fallback->entries.front().targetID;
		found = fallbackPackages_.emplace(instance.faction.id, std::move(sid)).first;
	}
	return config_.registry->squad(instance.templateSid, found->second);
}

void AiSystem::squadStep(flecs::world& ecs, RegionEngine& engine, std::uint64_t tick)
{
	refresh(ecs);
	if (doors_ != nullptr)
		doors_->sync(ecs, tick, towns_, config_.navigator.get());
	for (const auto& [id, raw] : squads_)
	{
		const flecs::entity entity(ecs.c_ptr(), raw);
		if (entity.has<state::StragglerOf>())
			continue; // a straggler token is walked by Simulation::stepStragglers (k16.6.53)
		if (entity.get<state::GroupAI>() == nullptr)
			entity.set<state::GroupAI>(state::GroupAI{});
		const Mapping& resolved = squadMapping(ecs, *entity.get<state::SquadInstance>());
		// T3f: the unloaded phase, at the squad interval (5.3 LOD: a Squad-level squad costs one table lookup a second; the value only
		// changes with the game hour). A Full-detail squad has real Routines instead and keeps no unloaded phase, so a promotion
		// cannot leave a stale one behind.
		if (const Detail* detail = entity.get<Detail>(); detail != nullptr)
		{
			if (detail->level == DetailLevel::Squad)
				updateTownPhase(entity, resolved.package, std::fmod(static_cast<double>(tick), kTicksPerGameDay) / kTicksPerGameDay * 24.0);
			else if (detail->level == DetailLevel::Full)
			{
				if (const state::Id* id = entity.get<state::Id>(); id != nullptr)
					townLife_.erase(id->value);
			}
		}
		if (resolved.behaviour == Behaviour::TradeCircuit && entity.get<state::TraderTrip>() == nullptr)
			entity.set<state::TraderTrip>(state::TraderTrip{});
		state::GroupAI& ai = *entity.get_mut<state::GroupAI>();
		// Resting: nothing to look at until the dwell is over (only an
		// individual layer, for squads in full detail, still wants a call).
		if (ai.goal == GoalKind::Idle && tick < ai.until
			&& ai.order.status != state::OrderStatus::Issued)
		{
			const Detail* resting = entity.get<Detail>();
			if (resting == nullptr || resting->level != DetailLevel::Full)
				continue;
		}
		const state::SquadInstance& squad = *entity.get<state::SquadInstance>();
		const state::Position* position = entity.get<state::Position>();

		GroupContext context{id, squad, resolved, ai};
		context.decisionSink = config_.decision ? &config_.decision : nullptr;
		context.now = tick;
		if (position != nullptr)
		{
			context.hasPosition = true;
			context.x = position->x;
			context.z = position->z;
		}
		if (squad.homeTown.id != 0)
		{
			const flecs::entity home(ecs.c_ptr(), squad.homeTown.id);
			if (const state::TownState* town = home.get<state::TownState>())
			{
				context.hasHome = true;
				context.homeX = town->x;
				context.homeZ = town->z;
			}
		}
		if (squad.homeBuilding != 0)
		{
			// A town resident's home is the building the populate gave it (spawn.cpp `populate`), not the town point.
			context.hasHome = true;
			context.homeX = squad.homeX;
			context.homeZ = squad.homeZ;
		}
		if (const state::GateCrew* crew = entity.get<state::GateCrew>())
		{
			// A gate crew's home is its gate, not the town (GATES_CARRY_LIVE.md a, spawn.hpp).
			context.hasPost = true;
			context.postX = crew->postX;
			context.postZ = crew->postZ;
			context.postKey = crew->gate;
			context.hasHome = true;
			context.homeX = crew->postX;
			context.homeZ = crew->postZ;
		}
		if (context.hasHome)
		{
			context.hasAnchor = true;
			context.anchorX = context.homeX;
			context.anchorZ = context.homeZ;
		} else if (squad.hasDen) {
			context.hasAnchor = true;
			context.anchorX = squad.denX;
			context.anchorZ = squad.denZ;
		}

		context.access = doors_.get();
		if (const auto carry = carried_.find(id); carry != carried_.end() && position != nullptr)
		{
			context.carried = true;
			context.holding = carry->second.holding;
			context.slaveGap = carry->second.slaveGap;
			context.x = carry->second.x;
			context.z = carry->second.z;
		}
		std::function<std::vector<trader::TownFacts>(const TownSite*)> townFacts;
		if (resolved.behaviour == Behaviour::TradeCircuit && config_.data != nullptr)
		{
			townFacts = [&](const TownSite* current) { return circuitTowns(ecs, *config_.data, squad, towns_, current); };
			context.trip = entity.get_mut<state::TraderTrip>();
			context.townFacts = &townFacts;
			context.squadEntity = entity;
			if (barTables_.claim)
				context.bar = &barTables_;
		}
		if (squad.faction.id != 0)
			context.factionAi = flecs::entity(ecs.c_ptr(), squad.faction.id).get<state::FactionAI>();
		if (const auto depot = slaveDepots_.find(id); depot != slaveDepots_.end())
		{
			context.depot = depot->second;
			context.squadEntity = entity;
			context.trip = entity.get_mut<state::TraderTrip>(); // a wandering caravan's circuit learns the town it is bound for
		}

		const AiReason before = ai.fallback;
		const GroupResult result = group_->update(context);
		if (result.brokeGate != 0 && doors_ != nullptr)
			doors_->breakGate(ecs, result.brokeGate);
		if (result.deliveryFailed != AiReason::None)
			deliveryFailures_.push_back(DeliveryFailure{id, context.depot, result.deliveryFailed, result.deliveryEdge});
		++stats_.squadSteps;
		if (result.selected)
			++stats_.selections;
		if (result.arrived)
			++stats_.arrivals;
		for (std::size_t i = 0; i < result.failures.size(); ++i)
			stats_.failures[i] += result.failures[i];
		if (result.moved)
			engine.place(ecs, id, context.x, context.z);
		if (ai.fallback != before)
		{
			++stats_.fallbackSwitches[static_cast<std::size_t>(ai.fallback)];
			if (config_.log)
			{
				const Mapping& mapping = context.mapping;
				config_.log("tick " + std::to_string(tick) + " squad " + std::to_string(id)
					+ " (" + squad.templateSid + ") generic fallback: "
					+ std::string(state::aiReasonName(ai.fallback)) + " [package "
					+ (mapping.packageName.empty() ? "(none)" : mapping.packageName)
					+ (mapping.wanted.empty() ? "" : ", wants " + mapping.wanted) + "]");
			}
		}
	}
}

bool AiSystem::strategicStep(flecs::world& ecs, std::uint64_t tick)
{
	refresh(ecs);
	// T3f: the unloaded phase of the squads the squad step does not visit, once a game hour (5.3 LOD).
	{
		const double hour = std::fmod(static_cast<double>(tick), kTicksPerGameDay) / kTicksPerGameDay * 24.0;
		for (const auto& [id, raw] : squads_)
		{
			const flecs::entity squad(ecs.c_ptr(), raw);
			const Detail* detail = squad.get<Detail>();
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			if (detail == nullptr || detail->level != DetailLevel::Strategic || instance == nullptr)
				continue;
			updateTownPhase(squad, squadMapping(ecs, *instance).package, hour);
		}
	}
	// What flowed up since the last step: every faction reads its own.
	std::vector<state::FactionEvent> events;
	if (const state::FactionEvents* pending = ecs.get<state::FactionEvents>())
	{
		events = pending->pending;
		if (utility_ != nullptr && !events.empty())
			ecs.get_mut<state::FactionEvents>()->pending.clear();
	}
	bool changed = false;
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> factions;
	ecs.each([&factions](flecs::entity entity, const state::Id& id, const state::FactionState&)
	{
		factions.emplace_back(id.value, entity.id());
	});
	std::sort(factions.begin(), factions.end());
	for (const auto& [id, raw] : factions)
	{
		const flecs::entity entity(ecs.c_ptr(), raw);
		// Before the context takes a reference into the entity's storage.
		if (utility_ != nullptr && entity.get<state::FactionAI>() == nullptr)
			entity.set<state::FactionAI>(state::FactionAI{});
		FactionContext context{id, *entity.get<state::FactionState>(), tick, ecs, entity,
			events, squads_};
		const FactionResult result = faction_->update(context);
		if (result.instancesChanged)
		{
			changed = true;
			// The squad list is stale for the factions still to come.
			invalidate();
			refresh(ecs);
		}
		++stats_.factionCalls;
	}
	if (doors_ != nullptr)
		doors_->update(ecs, tick, towns_, config_.navigator.get());
	return changed;
}

AiAudit AiSystem::audit(const flecs::world& ecs) const
{
	AiAudit audit;
	ecs.each([&](flecs::entity entity, const state::SquadInstance& squad)
	{
		++audit.squads;
		// Town life when no player is near (T3f): the squad's unloaded phase, counted whatever its group goal.
		if (const state::Id* id = entity.get<state::Id>(); id != nullptr)
		{
			if (const auto life = townLife_.find(id->value); life != townLife_.end())
			{
				++audit.byPhase[std::min<std::size_t>(life->second.phase, routines::kTownPhaseCount - 1)];
				audit.unmodelled += life->second.unmodelled;
			}
		}
		const state::GroupAI* ai = entity.get<state::GroupAI>();
		if (ai == nullptr || ai->goal == GoalKind::None)
		{
			++audit.goalless;
			return;
		}
		++audit.byGoal[static_cast<std::size_t>(ai->goal)];
		++audit.byFallback[static_cast<std::size_t>(ai->fallback)];
		const Mapping& mapping = squadMapping(ecs, squad);
		++audit.byBehaviour[static_cast<std::size_t>(
			ai->fallback == AiReason::None ? mapping.behaviour : Behaviour::Fallback)];
		if (ai->goal != GoalKind::Idle
			&& !(ai->goal == GoalKind::ManGate && ai->route.empty()) // holding its post
			&& !UtilityGroupLayer::tokenLeg(*ai) // a token's straight leg off the mesh: no route to be off (k16.7.15)
			&& !(ai->goal == GoalKind::Raid && (ai->order.status == state::OrderStatus::Staying
				|| ai->order.status == state::OrderStatus::Retreating)))
		{
			++audit.moving;
			const state::Position* position = entity.get<state::Position>();
			const GroundPoint expected = pointAlong(ai->route, ai->along);
			if (position == nullptr
				|| distance(position->x, position->z, expected.x, expected.z) > kDisplaced)
				++audit.offRoute;
		}
	});
	return audit;
}

} // namespace ksim
