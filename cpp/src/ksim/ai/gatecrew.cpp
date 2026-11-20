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
