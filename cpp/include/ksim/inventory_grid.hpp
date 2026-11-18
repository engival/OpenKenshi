#pragma once

// The grid inventory (SIMLINK.md sections 6 and 7, docs/reference/INVENTORY_GRID_STATIC.md = kinspect 33): footprints, stack caps,
// the autoplace scan, merges, moves and the arrange repack. Pure functions over a state::Inventory and the game data; no
// simulation, no wire.
//
// A carried entry is one stack at (section, x, y). Section 0 is `main` (state::Inventory::gridW x gridH: 8 x 10 for a human, a
// container building's `storage size`); section 1 is the backpack_content of the worn backpack, whose entries live in that entry's
// `contents` (they move with the item). The flat index of the wire (INVENTORY entry index) is `items` followed by the worn
// backpack's contents. Functions that can refuse return the SIMLINK reason ("" = done): stale is the caller's (it owns the
// version), the rest are no_room, occupied, not_allowed, bad_value, bad_target.

#include "kdata/gamedata.hpp"
#include "ksim/state.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ksim::grid {

using Entry = state::Inventory::Entry;

struct Size
{
	int w = 1;
	int h = 1;
};

// What the records say. The game data must outlive it.
class Rules
{
public:
	explicit Rules(const kdata::GameData& data);

	// `inventory footprint width / height` of any item record (ITEM, WEAPON, ARMOUR, CONTAINER ...), at least 1 x 1.
	Size footprint(const std::string& sid) const;
	// The stack cap of an item in a section: ITEM `stackable` (1 when absent) for plain items, 1 for weapons, armour, containers, maps,
	// crossbows and limbs; inside the container item `bag` it is max(base, `stackable bonus minimum`) x `stackable bonus mult`
	// (INVENTORY_GRID_STATIC (c)). `bag` null = main or a building's grid (no bonus). `whitelisted` (a section with a non-empty
	// whitelist) forces the base to 1 before the min / mult (INVENTORY_GRID_LIVE (c)); no section modelled here has a whitelist.
	int stackCap(const std::string& sid, const Entry* bag, bool whitelisted = false) const;
	bool isContainer(const std::string& sid) const;
	// CONTAINER `storage size width / height` of a backpack item (0 x 0 when the record has none).
	Size bagSize(const std::string& sid) const;
	// BUILDING `storage size width / height` (0 x 0 when none).
	Size buildingSize(const std::string& sid) const;
	// The MATERIAL_SPECS_CLOTHING records of the `material` reference list (empty for a weapon, or a record without one).
	std::vector<std::string> materials(const std::string& sid) const;
	const kdata::GameData& data() const noexcept { return *data_; }

private:
	const kdata::GameData* data_;
	std::int32_t weapon_, armour_, item_, container_, map_, crossbow_, limb_, building_;
};

// Puts the inventory in order: gives the instances a material (a pick seeded by `ownerKey`, the record and a running count),
// marks equipped entries, keeps valid placements, caps stacks (the excess becomes new stacks), and autoplaces every entry
// that has no valid place (unplaced, out of its grid, overlapping an earlier one, a non-empty container in a bag). Entries
// that fit nowhere are returned (the caller drops them on the ground or discards them); the inventory has no change of its
// own to report, so `version` is left alone. Idempotent.
std::vector<Entry> settle(state::Inventory& inventory, const Rules& rules, std::uint64_t ownerKey);

// Adds an entry (count units) the way the original does into ONE section: `main` (0, the default), or the worn backpack's (1) only when
// the caller names it (a non-empty container never goes into a backpack). Nothing overflows from main into the backpack
// (INVENTORY_GRID_LIVE (b)): what does not fit is returned. Within a section the existing stacks of the same kind (record, manufacturer,
// material; colour is not part of it) fill up first, then new stacks go to the first free cell scanning row by row from the top
// left, no rotation. Equipment sections are not tried here (the fighter's single wielded weapon and armour list are filled by the
// equip orders only; UNSURE, the original auto-equips on pick-up). The one place that equips on the way in is order 68's quick move
// into a character (section 255: a free matching worn slot, then main, then the worn backpack; Simulation::runInventoryOrder); this
// function stays the pure main-or-one-section placement under it. Returns the units that did not fit (0 = all placed).
std::uint32_t autoplace(state::Inventory& inventory, const Rules& rules, Entry entry, std::uint8_t section = 0);

// The entries of the flat index space: `items`, then the worn backpack's contents. Null when the index is past the end.
Entry* entryAt(state::Inventory& inventory, std::size_t index);
std::size_t entryCount(const state::Inventory& inventory);
// The flat index of the first carried entry of the item record, or -1.
std::ptrdiff_t firstCarried(const state::Inventory& inventory, const std::string& sid);
// The flat index of the first equipped entry of the item record, or -1.
std::ptrdiff_t firstEquipped(const state::Inventory& inventory, const std::string& sid);

// Order 64: moves the carried entry `index` to (x, y) of `section` (0 main, 1 the worn backpack's). The rect must lie in the
// grid and the section accept the entry (no_room); onto exactly one entry of the same stackable kind with room the stacks merge
// (what does not fit stays where it was); onto exactly one other entry the two swap, the other going to the moved entry's old cell
// (INVENTORY_GRID_LIVE (e); the exe parks it on the cursor), occupied when it does not fit there; onto 2+ entries occupied.
// `count` 0, or at least the entry's count, moves the whole stack; otherwise `count` units are split off to (x, y) (an empty rect, or a
// merge up to the cap; never a swap) and the rest stays; any refusal leaves both cells unchanged. An equipped entry is not_allowed (equip orders move those).
std::string moveEntry(state::Inventory& inventory, const Rules& rules, std::size_t index, std::uint8_t section, std::uint8_t x, std::uint8_t y,
	std::uint32_t count = 0);

// Puts `entry` at (x, y) of `section` (order 68 with a rect): the rect must be in the grid and free, or hold exactly one entry
// of the same kind that takes all of it (a merge); otherwise no_room. All or nothing (the entry is untouched on failure).
std::string insertAt(state::Inventory& inventory, const Rules& rules, Entry entry, std::uint8_t section, std::uint8_t x, std::uint8_t y);

// Order 65: the repack of InventorySection::vf4 (INVENTORY_GRID_STATIC (e)); the exe arranges main only, section 1 is our extension (INVENTORY_GRID_LIVE (e)): entries ordered by x then y, then stable-sorted
// descending by max(w, h), then by area, then by w, each pass re-adding every entry with autoplace (stacks merge); the first
// pass that fits everything wins, if none does the section stays as it was (`changed` false). Section 0 or 1.
std::string arrange(state::Inventory& inventory, const Rules& rules, std::uint8_t section, bool* changed = nullptr);

// Takes `count` units out of the carried entry `index` (a whole entry when count covers it). The piece is unplaced and keeps the
// instance data; empty when index is wrong, not carried, or fewer units are there.
std::vector<Entry> takeFrom(state::Inventory& inventory, std::size_t index, std::uint32_t count);

} // namespace ksim::grid
