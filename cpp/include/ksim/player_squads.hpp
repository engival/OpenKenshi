#pragma once

// Player squads: the player faction's ordered squad list (docs/reference/FACTION_SQUADS_LIVE.md, SIMLINK.md PLAYER_SQUADS 31 and
// orders 70-75). The state is state::PlayerFaction (on the FactionState entity) and state::PlayerSquad (one instance entity per
// squad); the rules and the order checks are Simulation::checkSquadOrder / runSquadOrder (player_squads.cpp).

#include "kdata/gamedata.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ksim {

// GLOBAL CONSTANTS `max faction size` / `max squad size` / `max squads` of the merged data (vanilla with rebirth.mod: 30 / 20 / 10;
// gamedata.base alone has squad size 10).
struct SquadLimits
{
	std::uint16_t faction = 0;
	std::uint16_t squad = 0;
	std::uint16_t squads = 0;
};

// Throws std::runtime_error when the record or a field is missing.
SquadLimits squadLimits(const kdata::GameData& gameData);

// A squad name is at most this many characters (code points); the exe's setMaxTextLength(0x20).
inline constexpr std::size_t kSquadNameMax = 32;
// Whether `text` is valid UTF-8 of at most kSquadNameMax code points.
bool validSquadName(const std::string& text);

// The PLAYER_SQUADS record's content (SIMLINK.md section 6).
struct PlayerSquadsView
{
	struct Squad
	{
		std::uint64_t id = 0;
		std::string name;
		std::vector<std::uint64_t> members; // leader first
	};
	std::uint64_t faction = 0; // state::Id of the FactionState; 0 = no player faction
	std::string factionSid; // FACTION record stringID
	std::string factionName;
	std::uint16_t factionCount = 0; // living members
	std::int64_t money = 0; // state::PlayerFaction::money (the trailing i64 of the record)
	SquadLimits limits;
	std::vector<Squad> squads; // list order = HUD tab order

	bool operator==(const PlayerSquadsView& other) const;
};

// A dismissal (order 75) applied: the character, the display name of the faction it joined, its new squad.
struct DismissEvent
{
	std::uint64_t tick = 0;
	std::uint64_t character = 0;
	std::string factionSid; // FACTION record stringID
	std::string factionName;
	std::uint64_t squad = 0;
};

} // namespace ksim
