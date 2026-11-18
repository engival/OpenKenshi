// The per-world caches behind Simulation::characterSquads / playerSquads / shopSquad (k16.6.279). Each is rebuilt only after one of the
// writes that changes it; the flecs observers in Simulation::registerSquadCaches set the dirty flags. The invalidation table and the writer
// list are in docs/PROFILING.md (k16.6.264 section, "squad caches").
#pragma once

#include "ksim/world/player_squads.hpp"

#include <cstdint>
#include <unordered_map>

#include <flecs.h>

namespace ksim
{

struct SquadCaches
{
	bool characterDirty = true; // characterSquads: PlayerSquad / MemberOf / Fighter / Id
	bool viewDirty = true; // playerSquads (all but the living count): PlayerFaction / PlayerSquad / Id
	bool shopDirty = true; // shopSquad: TraderStock / Inventory / MemberOf / Fighter / Id, and a member's homeBuilding
	std::unordered_map<std::uint64_t, std::uint64_t> characterSquads;
	PlayerSquadsView view;
	std::unordered_map<std::uint64_t, flecs::entity> shops; // building -> its merchant squad (invalid = none), filled on demand
};

} // namespace ksim
