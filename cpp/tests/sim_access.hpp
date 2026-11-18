// Shared test accessors: the flecs world behind a Simulation, a fighter by id, the first player, a squad's members, the Hub's town point.
// Tests used to carry their own copy of each; add new ones here instead.
#pragma once

#include "hub_point.hpp"
#include "ksim/core/simulation.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

inline flecs::world& ecsOf(ksim::Simulation& sim)
{
	return const_cast<flecs::world&>(sim.world().ecs());
}

inline ksim::state::Fighter* fighterOf(ksim::Simulation& sim, std::uint64_t id)
{
	return const_cast<ksim::state::Fighter*>(sim.combat()->find(id));
}

// The first player character, or null.
inline const ksim::state::Fighter* playerOf(ksim::Simulation& sim)
{
	for (const ksim::CombatArena::Member& member : sim.combat()->members())
		if (member.fighter.playerControlled)
			return &member.fighter;
	return nullptr;
}

// The members of a squad in character-id order.
inline std::vector<flecs::entity> membersOf(ksim::Simulation& sim, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).query_builder<const ksim::state::Id>().with<ksim::state::MemberOf>(squad).build().each(
		[&](flecs::entity member, const ksim::state::Id& id) { found.emplace_back(id.value, member); });
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<flecs::entity> out;
	for (const auto& [id, member] : found)
		out.push_back(member);
	return out;
}
