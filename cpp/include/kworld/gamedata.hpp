#pragma once

// Merged game data and small record helpers. The load order is the one
// documented in docs/reference/GAMEDATA_SCHEMA.md "Files and load order".

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace kworld {

inline constexpr std::array<std::string_view, 4> kGameDataLoadOrder = {
	"gamedata.base",
	"Newwworld.mod",
	"Dialogue.mod",
	"rebirth.mod",
};

// Applies the four shipped files in kGameDataLoadOrder (vanillaLoadOrder).
// Throws Error when a file is missing; parse errors propagate as
// kdata::ParseError.
kdata::GameData loadMergedGameData(std::string_view dataDir);

// Applies order.files in sequence; same errors as above.
kdata::GameData loadMergedGameData(const LoadOrder& order);

// -1 when the name is not in kdata's itemType table.
std::int32_t itemTypeByName(std::string_view name) noexcept;

} // namespace kworld
