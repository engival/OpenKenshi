#include "ksim/ai/gatecrew.hpp"

#include "ksim/combat/combat.hpp"
#include "ksim/ai/gatebind.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

namespace ksim {

// MAN_THE_GATE (GATES_CARRY_LIVE.md a): the leader's task only moves it to the gate's outside point. It has no alert, enemy, time,
// relation or crime check and never closes the gate. Its precondition DOOR_IS_OPEN is what makes it re-plan when the gate is
// shut (by anyone): UNLOCK_DOOR (3.3 s, only if the leader's faction owns the door: isObjectMine) then OPEN_DOOR, then 180
// again, which walks back to the post. A locked door whose owner is someone else leaves it standing: it cannot plan an unlock.
// A leader that is fighting (it has a target) or is down does nothing here: goal 144 / the medical state own it, and the
// gate stays as it is.
void Simulation::stepGateCrews()
{
	if (combat_ == nullptr || combat_->empty() || navigation_ == nullptr)
		return;
	if (clockTick() % kGateCrewIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::GateDuty* found = actor.get<state::GateDuty>();
		if (found == nullptr)
			continue;
		state::GateDuty duty = *found;
		const std::uint64_t id = members[i].id;
		state::Fighter& f = *combat_->find(id);
		if (f.dead || f.unconscious || f.down || f.target != 0 || f.carriedBy != 0 || f.carrying != 0 || f.playerControlled)
			continue;
		if (actor.has<state::InteractTask>() || actor.has<state::DoorStep>() || actor.has<state::CarryStep>() || f.walking())
			continue;

		const state::Door* door = nullptr;
		std::uint64_t doorId = 0;
		const bool member = duty.phase == kGateMemberPhase;
		if (!member)
		{
			ecs.each([&](const state::Id& candidateId, const state::Door& candidate)
			{
				if (candidate.key == duty.gate)
				{
					door = &candidate;
					doorId = candidateId.value;
				}
			});
			if (door == nullptr)
				continue;
		}

		const auto walkTo = [&](double x, double y, double z)
		{
			const Route route = routeRetry(id, f, x, z, y, true);
			if (!route.found || route.points.empty())
				return false;
			f.clearRoute();
			for (std::size_t p = 0; p < route.points.size(); ++p)
				f.route.push_back({route.points[p].x, route.heights[p], route.points[p].z});
			f.routeNext = 0;
			++f.routeVersion;
			f.routeSeq = 0;
			f.routeChase = false;
			f.routeApproach = route.approach;
			f.routeTries = 0;
			mirrorFighter(id);
			return true;
		};

		if (member)
		{
			// A guard (STAND_AT_NODE): walks back to its slot like the leader; never touches the gate.
			if (std::hypot(f.x - duty.postX, f.z - duty.postZ) > kGatePostReach)
				walkTo(duty.postX, duty.postY, duty.postZ);
			continue;
		}
		if (door->state == 1 || door->state == 2)
		{
			// DOOR_IS_OPEN fails: a locked door needs the UNLOCK first, which only the door's faction plans.
			std::uint8_t action = static_cast<std::uint8_t>(InteractAction::Open);
			if (door->state == 2)
			{
				if (!isObjectMine(ecs, id, *door))
					continue;
				action = static_cast<std::uint8_t>(InteractAction::Unlock);
			}
			const InteractTarget target = findInteractTarget(ecs, combat_.get(), doorId);
			std::vector<state::RoutePoint> points;
			bool approach = false;
			if (target.distanceTo(f.x, f.z) > kInteractReach)
			{
				const Route route = navigation_->routeFrom({f.x, f.y, f.z}, target.x, target.z, target.y, true, body(f));
				if (!route.found)
					continue;
				for (std::size_t p = 0; p < route.points.size(); ++p)
					points.push_back({route.points[p].x, route.heights[p], route.points[p].z});
				approach = route.approach;
			}
			beginInteract(id, doorId, action, 0, std::move(points), approach);
			duty.phase = 1;
			actor.set<state::GateDuty>(duty);
			mirrorFighter(id);
			continue;
		}
		// Open or broken: MAN_THE_GATE walks to the outside point and stands there.
		const double away = std::hypot(f.x - duty.postX, f.z - duty.postZ);
		if (away > kGatePostReach)
		{
			if (walkTo(duty.postX, duty.postY, duty.postZ))
				duty.phase = duty.phase == 1 ? 2 : 0;
		} else {
			duty.phase = 0;
		}
		if (duty.phase != found->phase)
			actor.set<state::GateDuty>(duty);
	}
}

void Simulation::removeGate(std::uint64_t doorKey)
{
	flecs::world& ecs = ecsMutable();
	state::GatesGone gone;
	if (const state::GatesGone* current = ecs.get<state::GatesGone>())
		gone = *current;
	const auto at = std::lower_bound(gone.keys.begin(), gone.keys.end(), doorKey);
	if (at != gone.keys.end() && *at == doorKey)
		return;
	gone.keys.insert(at, doorKey);
	ecs.set<state::GatesGone>(std::move(gone));
}

// Once a second. A crew whose gate exists and is not held by a lower-Id crew keeps it. The others rebind in Id order through
// resolveGate (gatebind.hpp): the home building's gate, then the gateway of the crew's town nearest the squad that no other crew
// holds, else GateCrew is removed and the squad is an ordinary resident (goal ManGate ends, the AI selects again). A rebound crew
// takes the new gate's outside point as its post and re-selects, so it walks there.
void Simulation::stepGateBinding()
{
	if (clockTick() % kGateCrewIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::GateCrew>() == 0) // the uncached ecs.each() below walks every table; most worlds have no crews
		return;
	const state::GatesGone* goneList = ecs.get<state::GatesGone>();
	const std::vector<GateSite>& sites = world().gateSites(); // placement order
	const auto siteOf = [&](std::uint64_t key) -> const GateSite*
	{
		for (const GateSite& site : sites)
			if (site.key == key)
				return &site;
		return nullptr;
	};
	const auto exists = [&](std::uint64_t key)
	{
		return siteOf(key) != nullptr && (goneList == nullptr || !std::binary_search(goneList->keys.begin(), goneList->keys.end(), key));
	};
	std::vector<std::pair<std::uint64_t, flecs::entity>> crews; // (squad Id, entity)
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::GateCrew&) { crews.emplace_back(id.value, entity); });
	if (crews.empty())
		return;
	std::sort(crews.begin(), crews.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::set<std::uint64_t> held;
	std::vector<flecs::entity> orphans;
	for (const auto& [id, entity] : crews)
	{
		const std::uint64_t gate = entity.get<state::GateCrew>()->gate;
		if (exists(gate) && held.insert(gate).second)
			continue;
		orphans.push_back(entity);
	}
	if (orphans.empty())
		return;
	// The towns by Id, for the gate -> town rule (nearest centre, first on a tie: the same as spawn and DoorSystem::bind).
	std::vector<std::tuple<std::uint64_t, flecs::entity_t, double, double>> towns;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::TownState& town)
	{
		towns.emplace_back(id.value, entity.id(), town.x, town.z);
	});
	std::sort(towns.begin(), towns.end());
	const auto townOfGate = [&](const GateSite& site) -> flecs::entity_t
	{
		const std::size_t best = nearestTownIndex(site.x, site.z, towns.size(), [&](std::size_t i)
		{
			return std::pair<double, double>(std::get<2>(towns[i]), std::get<3>(towns[i]));
		});
		return best == towns.size() ? 0 : std::get<1>(towns[best]);
	};
	for (flecs::entity entity : orphans)
	{
		const state::GateCrew crew = *entity.get<state::GateCrew>();
		const flecs::entity_t town = entity.get<state::SquadInstance>()->homeTown.id;
		const state::Position* at = entity.get<state::Position>();
		const double fromX = at != nullptr ? at->x : crew.postX;
		const double fromZ = at != nullptr ? at->z : crew.postZ;
		const GateChoice choice = resolveGate(crew.gate, crew.home,
			[&](std::uint64_t key) { return exists(key) && held.count(key) == 0; },
			[&]() -> std::uint64_t
			{
				const GateSite* best = nullptr;
				double bestDistance = std::numeric_limits<double>::infinity();
				for (const GateSite& site : sites)
				{
					if (!exists(site.key) || held.count(site.key) != 0 || townOfGate(site) != town)
						continue;
					const double far = std::hypot(site.outX - fromX, site.outZ - fromZ);
					if (far < bestDistance)
					{
						bestDistance = far;
						best = &site;
					}
				}
				return best != nullptr ? best->key : 0;
			});
		if (state::GroupAI* ai = entity.get_mut<state::GroupAI>())
		{
			ai->goal = state::GoalKind::None;
			ai->route.clear();
		}
		if (choice.source == GateSource::None)
		{
			entity.remove<state::GateCrew>();
			continue;
		}
		const GateSite& site = *siteOf(choice.key);
		const double length = std::hypot(site.outX - site.inX, site.outZ - site.inZ);
		state::GateCrew rebound = crew;
		rebound.gate = site.key;
		rebound.postX = site.outX;
		rebound.postZ = site.outZ;
		rebound.faceX = (site.outX - site.inX) / length;
		rebound.faceZ = (site.outZ - site.inZ) / length;
		entity.set<state::GateCrew>(rebound);
		held.insert(site.key);
	}
}

} // namespace ksim
