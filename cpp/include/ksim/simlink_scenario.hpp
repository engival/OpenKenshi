#pragma once

// Startup scenarios for ksim_server (SIMLINK.md "First playable cut").

#include "ksim/combat_data.hpp"
#include "ksim/simlink.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ksim::simlink {

struct Duel
{
	std::uint64_t player = 0; // the player-controlled Engival-like character
	std::uint64_t enemy = 0; // the Escaped Servant
};

// The Hub (TOWN placement, tests/knav_hub.cpp).
inline constexpr double kHubX = -50978.8;
inline constexpr double kHubZ = 2932.48;
// The Hub's zone cell: the one zone the server bakes a navmesh for (~3 s, SIMLINK.md section 10).
inline constexpr int kHubZoneX = 20;
inline constexpr int kHubZoneZ = 32;

// `--scenario duel`: an Engival-like unarmed character (player-controlled, team 0, the skills
// and gear of the kinspect duel log) and an Escaped Servant with the Iron Club (team 1), 40
// units apart near the Hub, built the way the combat tests build them (CombatData::makeFighter,
// makeFists, makeArmour). The sim must have combat and a navmesh covering the Hub enabled (the
// duel takes orders: no navmesh, no nav); both are placed on its nearest walkable ground.
// Registers their appearance seeds. `katana` (`--scenario duel_katana`): the player starts with
// a Katana equipped instead of fists, so the servant's wounds bleed. `playerSquads` (default on) gives the player faction its squad
// (Nameless_1; the squad scenario two); off for fixtures that do not need them: the faction and squad instances take Ids, and a test whose
// RNG-dependent checks are seeded on the Ids of later spawns (ksim_gates) would shift. `enemyArmour` (`--scenario duel_armour`): the servant
// wears the heaviest plate in the data, one piece per slot (hat, body, legs, boots), so hits show an absorbed tail (event 1).
Duel spawnDuel(Simulation& sim, const kdata::GameData& gameData, const CombatData& data,
	ServerContext& context, bool katana = false, bool playerSquads = true, bool enemyArmour = false);

struct Squad
{
	std::vector<std::uint64_t> players; // Engival, Izumi: player-controlled, team 0
	std::vector<std::uint64_t> enemies; // three bandits, team 1
};

// `--scenario squad`: to watch follow and the fight join. Two player characters (Engival as in the duel,
// Izumi from the game data when the load order has her, else a second Engival-like body) 30 units apart
// at the Hub, and a squad of three bandits (team 1; Escaped Servant bodies, the bandit records list no race) 700 units
// east, facing away from the Hub so they have not noticed the squad (perception.hpp). Same requirements and placement as the duel.
Squad spawnSquad(Simulation& sim, const kdata::GameData& gameData, const CombatData& data,
	ServerContext& context, bool playerSquads = true);

} // namespace ksim::simlink
