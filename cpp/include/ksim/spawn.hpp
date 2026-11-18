#pragma once

// The spawner: game-data templates -> instance entities for a new game,
// following the default-world rules of docs/reference/WORLD_MODEL.md
// (tools/world_model.py, ported; the TSVs are only the test oracle).
//
// - Every FACTION gets a FactionState, with its listed `relations` as
//   state::Relation pairs.
// - Every placed default town root (a TOWN no `override town` points at,
//   with a 0-townlist placement) gets a TownState; the active variant is the
//   first `override town` entry whose `world state` holds, followed to the
//   end of the chain. World states are evaluated at the initial snapshot:
//   every unique NPC alive, no player allies/enemies, `player involvement`
//   ignored (WORLD_MODEL.md a, the "chosen" mode).
// - Town populate (WORLD_MODEL.md "Who lives where"): the exe's populateBuildings, from data only. The town's buildings (zone
//   `townC`/`townCS` = the town's hand) and gates are its sites; zone `squad template` presets, then `residents` by priority and
//   score, then `default resident` for every empty eligible building; leftovers are dropped. Each squad is homed in its building
//   (SquadInstance homeBuilding / homeX / homeZ) and spawns at its centre; a gate squad spawns at outside + 1.1 (outside - inside)
//   of the gate's first door and carries a state::GateCrew (GRADES_GATESQUADS_KO_LIVE.md b, DOORS.md section 8). `bar squads`
//   (count rule) and `roaming squads` stand at the town position (we have no town nodes). A town without a hand or loaded sites (the
//   ksim_spawn oracle, towns outside a partial world) keeps the old one-squad-per-entry path at the town position.
// - Each default-active wilderness template (a biome `homeless spawns`
//   entry, a biome `nests` town's squad lists, or FACTION `roaming squads`,
//   whose own `world state` holds) is one squad with no home town.
// - Members are CharacterInstances drawn from the template's `leader`,
//   `squad`, `squad2`, `choosefrom list` and `animals*` lists with the
//   "spawn" RNG stream.
//
// Other squads with a home town get that town's position; wilderness squads get no
// Position here: Simulation::newGame places them afterwards (placement.hpp,
// provisional).

#include "ksim/clock.hpp"
#include "ksim/economy.hpp"
#include "ksim/rng.hpp"
#include "ksim/state.hpp"
#include "ksim/world_state.hpp"

#include <flecs.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace kdata {
class GameData;
struct Record;
}

namespace kworld {
class Towns;
}

namespace ksim {

struct GateSite;
struct BuildingSite;

struct SpawnOptions
{
	std::uint64_t seed = 1;
	std::uint64_t startTick = tickAt(kNewGameDay, kNewGameHour, kNewGameMinute);
	// Simulation::newGame only (spawnDefaultWorld ignores both). Wilderness
	// squads and nests are spawned by density with respawn (wilderness.hpp)
	// instead of one persistent squad per template. `wildernessPopulation`
	// scales the homeless squads per area, `nestMultiplier` the nests per
	// area (the exe's "Number of nests multiplier" new-game option). 0 turns
	// that kind off.
	bool wildernessByDensity = true;
	double wildernessPopulation = 1.0;
	double nestMultiplier = 1.0;
	// The world's economy (TOWN_LIFE_SLICE 7.1): `original` (the V1 default) runs no T4 system and creates no T4 state; `real`
	// seeds the aggregate TownStock at new game. The mode is saved with the world (the EconomySettings singleton); a load reads
	// it from the save, not from this option.
	EconomyMode economy = EconomyMode::Original;
	// Simulation::newGame only: seconds of sim time the world runs headless after a NEW_GAME command, before the first frame the player sees, so
	// the residents are at their sites and busy (TOWN_LIFE_SLICE 5.40); newGame builds the world that much before `startTick`, so a NEW_GAME applied on its first tick shows the start time at the first frame (the clock never goes back).
	// 0 = off (the tests that pin the t0 state).
	double settleSeconds = 30.0;
};

struct SpawnReport
{
	std::size_t factions = 0;
	std::size_t relations = 0;
	std::size_t towns = 0;
	std::size_t squads = 0;
	std::size_t characters = 0;
	std::array<std::size_t, state::kSpawnSourceCount> squadsBySource{};
	// Town-list squads whose own `world state` is false at the default; they
	// are spawned anyway, as world_model.py does (see spawn.cpp, UNSURE).
	std::size_t townSquadsFailingWorldState = 0;
	// Gate crews (see the gate rule above): squads homed at a town gate, and resident entries whose template lists a gate
	// type that found no free gate in their town (spawned as ordinary residents at the town; counted so it is not silent).
	std::size_t gateSquads = 0;
	std::size_t gateSquadsUnhoused = 0;
	// The building populate (`buildings` given): squads homed in a town building (not a gate), and `residents` squads that found no free
	// building and were dropped, as the original drops them.
	std::size_t buildingSquads = 0;
	std::size_t residentsDropped = 0;
};

// Creates squad instances and their members from SQUAD_TEMPLATE records, with
// the "spawn" rules above. Shared by the default-world spawner and the
// wilderness spawner (wilderness.hpp). `rng` must outlive the builder.
class SquadBuilder
{
public:
	SquadBuilder(flecs::world& ecs, const kdata::GameData& gameData, RngStream& rng);

	// A record's own `world state` list at the initial snapshot (see above).
	bool worldStateHolds(const kdata::Record& record) const;

	// A squad with members; a Position only when `positioned`. Members come
	// from the stream.
	flecs::entity build(const kdata::Record& squad, flecs::entity_t faction,
		flecs::entity_t homeTown, state::SpawnSource source, bool positioned = false,
		double x = 0.0, double z = 0.0);

	// Characters created so far.
	std::size_t characters() const noexcept { return characters_; }

private:
	std::optional<bool> evalState(std::string_view sid) const;
	// `list`: the state::MemberRole the template list gives (state::kRole*); choosefrom and animal draws read the Squad list.
	void addMember(const std::string& sid, flecs::entity squad, std::uint8_t list = state::kRoleSquad);
	void spawnMembers(const kdata::Record& squad, flecs::entity entity);
	void spawnChooseFrom(const kdata::Record& squad, flecs::entity entity);

	flecs::world& ecs_;
	const kdata::GameData& data_;
	RngStream& rng_;
	std::int32_t squadType_ = 0;
	std::int32_t stateType_ = 0;
	std::unordered_set<std::int32_t> characterTypes_;
	std::size_t characters_ = 0;
	// The `unique` characters that exist (any CharacterInstance of the sid, read from the world on first use): one never spawns twice.
	std::optional<std::unordered_set<std::string>> uniques_;
	// The members spawned into each squad so far: the next one's state::MemberSlot.
	std::unordered_map<flecs::entity_t, std::uint32_t> slots_;
	// The world-state facts, read from the world on first use: no death can happen during a build, and a character spawned later is new
	// and so alive, so the cache cannot go stale within one run.
	mutable std::optional<worldstate::Facts> facts_;
};

// Spawns into ecs (which may already hold templates) and sets the Clock to
// options.startTick. Call once, on a world with no instances. `gates` (World::gateSites()), when given, homes the gate-guard
// residents at the town gates (see the gate rule above); without it every resident spawns at its town. `buildings` (World::buildingSites(),
// with `gates`) switches a town whose placement has a hand to the building populate (spawn.cpp `populate`): every squad of `residents` /
// `default resident` / the zone data's preset templates spawns in its building; `bar squads` / `roaming squads` stay at the town point.
SpawnReport spawnDefaultWorld(flecs::world& ecs, const kdata::GameData& gameData,
	const kworld::Towns& towns, const SpawnOptions& options = {}, const std::vector<GateSite>* gates = nullptr,
	const std::vector<BuildingSite>* buildings = nullptr);

} // namespace ksim
