#pragma once

// The wilderness spawner: homeless squads and nests by density, with respawn
// (ARCHITECTURE.md "AI", "Wilderness spawns"; the original's mechanism is in
// docs/reference/WILDERNESS_SPAWNS.md).
//
// One slot per BIOME_GROUP that has `homeless spawns` or `nests` and covers
// zones of the areas map. A slot's quotas follow the data:
//
//   homeless squads = round(zones x `homeless spawn amount` x population)
//   nests           = round(`num nests` x nest multiplier)
//
// (population and nest multiplier are new-game settings, SpawnOptions). The
// whole world is instanced from day 0, not around the player: newGame fills
// every quota, and from then on a squad that disappears (Command::RemoveSquad,
// later: killed) leaves a hole that is refilled after a respawn delay. A nest
// is a cluster: one point, and every squad of the nest TOWN's lists (they
// share SquadInstance::nest, and the nest counts once, alive while any of its
// squads lives). Squads are drawn by their chance weights in the slot's list,
// placed by WildGround (a dry point of the area, placement.hpp), and become
// ordinary squads with a den (patrol / territory AI).
//
// State (state::WildernessState, saved): the two multipliers, the stream, the
// nest counter, and per slot the due ticks of the pending respawns. The
// quotas are recomputed from data. A step runs at the Strategic interval
// (one game-hour), serially, in slot order, so results do not depend on the
// worker threads.
//
// A homeless squad is drawn by the exe's weight rule (kinspect s71_static.md
// 3.4): w - liveCopies x totalW / (zones x pop x amount), at least 0, where
// liveCopies are the slot's live squads of that template (our whole world is
// instanced, so "loaded" is "live"). Only BIOME_GROUP lists are read; BIOMES
// lists are dead data in the exe. Not adopted, by design (WILDERNESS_SPAWNS.md):
// nest budgets, the player-distance (1382 u) rule, destruction on unload.
//
// PROVISIONAL numbers (UNSURE, not from the exe): respawn delays and their
// jitter, the nest cluster shape. See WILDERNESS_SPAWNS.md.

#include "ksim/clock.hpp"
#include "ksim/placement.hpp"
#include "ksim/state.hpp"

#include <flecs.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kdata {
class GameData;
struct Record;
}

namespace ksim {

// Game-hours until a missing homeless squad / nest is replaced: uniform in
// [min, max]. UNSURE: guesses, a refresh cycle of a few game-days.
inline constexpr std::uint64_t kHomelessRespawnMinHours = 36;
inline constexpr std::uint64_t kHomelessRespawnMaxHours = 84;
inline constexpr std::uint64_t kNestRespawnMinHours = 120;
inline constexpr std::uint64_t kNestRespawnMaxHours = 240;

struct WildernessReport
{
	std::size_t squads = 0; // created
	std::size_t characters = 0;
	std::size_t homelessSquads = 0;
	std::size_t nests = 0;
	std::size_t queued = 0; // respawns scheduled
};

struct WildernessSlotCount
{
	std::string group; // BIOME_GROUP stringID
	std::int64_t homelessQuota = 0;
	std::int64_t homeless = 0; // live
	std::int64_t homelessPending = 0;
	std::int64_t homelessNonIntact = 0; // of `homeless`: not intact (squad_alive.hpp); they count 0.5 in the quota
	std::int64_t nestQuota = 0;
	std::int64_t nests = 0; // live (distinct nest instances)
	std::int64_t nestsPending = 0;
};

// One homeless-list entry of a slot with its live homeless copies (census).
struct WildernessTemplateCount
{
	std::string squad; // SQUAD_TEMPLATE stringID
	std::int64_t weight = 0; // the list's chance
	std::int64_t live = 0; // live homeless squads of this template in the slot
};

struct WildernessSlotCensus
{
	std::string group; // BIOME_GROUP stringID
	std::uint32_t colour = 0; // areasmap colour == BIOME_GROUP `index`
	double zones = 0.0;
	double amount = 0.0;
	std::int64_t totalWeight = 0;
	std::int64_t quota = 0; // homeless squads
	std::vector<WildernessTemplateCount> templates;
};

class WildernessSpawner
{
public:
	// Builds the slots from game data; gameData must outlive the spawner.
	WildernessSpawner(const kdata::GameData& gameData, std::string_view dataDir);
	~WildernessSpawner();
	WildernessSpawner(const WildernessSpawner&) = delete;
	WildernessSpawner& operator=(const WildernessSpawner&) = delete;

	std::size_t slotCount() const noexcept;

	// New game: sets state::WildernessState and instances every quota now.
	// Needs the factions (spawnDefaultWorld) in ecs.
	WildernessReport seed(flecs::world& ecs, std::uint64_t worldSeed, double population,
		double nestMultiplier);

	// Between ticks, at `now`: queues a respawn for each hole in a quota and
	// spawns those that are due. Needs WildernessState in ecs (seed or a
	// loaded save with the same game data; else it throws).
	WildernessReport step(flecs::world& ecs, std::uint64_t now);

	// Test / scenario hook: a homeless squad of `templateSid` with its den at
	// (x, z), built like a drawn one (slot of the point's area, counts in the
	// census and the weight rule, same den / patrol AI). Needs WildernessState
	// (seed or load); throws when the point is in no slot or the template is
	// unknown. `members` 0 keeps the template's own draw of its size; else the
	// squad is rolled again (from the saved stream) until it has exactly that
	// many members, and throws when none of 2000 rolls does. Call
	// Simulation::placeWildernessSquad, which also tells the regions.
	flecs::entity placeHomeless(flecs::world& ecs, std::string_view templateSid, double x,
		double z, std::size_t members = 0) const;

	// Live and pending counts per slot.
	std::vector<WildernessSlotCount> counts(const flecs::world& ecs) const;

	// Per slot, the homeless list with live copies of each template: the
	// census against the exe's expected proportions (tests/ksim_wilderness.cpp).
	std::vector<WildernessSlotCensus> census(const flecs::world& ecs) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace ksim
