#pragma once

// The live WORLD_EVENT_STATE set (WORLD_MODEL.md a; task AN). A state is the AND of its entries:
//   `NPC is` / `NPC is NOT`  a unique CHARACTER is alive (1), dead (0) or imprisoned (2): the entry's v0 compared equal (`NPC is`)
//                            or unequal (`NPC is NOT`) to the character's status now;
//   `player ally` / `player enemy`  the player faction's relation towards a FACTION at the sim's allied (>= 50) / hostile (<= -30)
//                            thresholds: the entry's v0 must match;
//   `player involvement`     tracked by the state::WorldEventState set but NOT used for truth: the literal reading (only a state the
//                            player took part in can be true) would start nine major towns destroyed or taken over
//                            (WORLD_MODEL.md "The player-involvement ambiguity", the chosen 69/152 evaluation).
// A unique character with no instance counts as alive (the initial snapshot: every unique NPC is alive). A record missing from the
// data makes its state false, so a requirement on it never holds.
//
// Consumed by the dialogue `world state` gate (DIALOGUE_DATA.md), by the spawn-time town variants / squads (SpawnBuilder) and, later,
// by the town swaps to ruins (FEATURES 410); Simulation::worldStateFacts() builds the live facts.

#include "kdata/gamedata.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace flecs { class world; }

namespace ksim::state { struct WorldEventState; }

namespace ksim::worldstate {

// `NPC is` v0 (a character with no instance, and none in the dead set, is kAlive).
inline constexpr std::int32_t kAlive = 1;
inline constexpr std::int32_t kDead = 0;
inline constexpr std::int32_t kImprisoned = 2;

// The relation thresholds (the sim's: door_plan.hpp kAlliedRelation / kHostileRelation).
inline constexpr double kAllyRelation = 50.0;
inline constexpr double kEnemyRelation = -30.0;

// What the evaluator reads of the world. Built once per batch, copied into the evaluator's callers.
struct Facts
{
	// A unique character's status by CHARACTER record sid; a sid absent from the map is kNotSeen (alive).
	std::unordered_map<std::string, std::int32_t> npcStatus;
	// The player faction's relation towards a FACTION record sid (-100..100); 0 when there is no player or no known faction.
	std::function<double(std::string_view)> playerRelation;
	// The unique characters the player has killed or imprisoned (record sids): the involvement set (UNSURE).
	std::unordered_set<std::string> playerTouched;
};

// The truth of one WORLD_EVENT_STATE record; false when the record is not a state.
bool stateHolds(const kdata::GameData& data, const Facts& facts, const kdata::Record& state);
bool stateHolds(const kdata::GameData& data, const Facts& facts, std::string_view sid);

// A record's own `world state` list (a town variant, a squad template, a faction campaign): every entry's state must equal the
// entry's required v0 (1 true / 0 false). An empty list holds; a missing state fails.
bool listHolds(const kdata::GameData& data, const Facts& facts, const kdata::Record& record,
	std::string_view category = "world state");

// Whether the player was involved in the state: it killed or imprisoned one of the state's NPC entries (the literal
// `player involvement` reading; kept for the town swaps, not used for truth).
bool involved(const kdata::GameData& data, const Facts& facts, const kdata::Record& state);

// Fills facts.npcStatus from the characters that exist now: a dead Fighter (or a record in the saved `events->dead` set; `events` null
// reads the world's own WorldEventState) is kDead, a caged body or a held slave is kImprisoned; a record with no instance is left out
// (not seen = alive).
void collectStatuses(const flecs::world& ecs, Facts& facts, const state::WorldEventState* events = nullptr);

// Every WORLD_EVENT_STATE record, and how many hold now (the model's fresh-world 69 / 152).
std::size_t countStates(const kdata::GameData& data);
std::size_t countTrue(const kdata::GameData& data, const Facts& facts);

} // namespace ksim::worldstate
