#pragma once

// Shopping (task CH group 3; kinspect dt24_shop.md, dt24_118.md): the SHOPPING goal 118 and its task 119 BUY_SHIT. The pieces here
// are pure; Simulation::stepShopping (shopping.cpp) runs the pulses and the item transfer.
//
// The list: the exe's town-owned vector (town vf+0x268, rebuilt by 140998e40) compiled from the residents' CHARACTER `shopping`
// refs, each (ITEM, want, chance). A character's own copy at AI+0x2b0 carries its bought counts; the transaction reads one record
// per pulse. Our list is per member, built from its own template at the first evaluation of a game day and rebuilt once the hour
// passes 08:00 on a later day (TQ24's live reading says 07:00, dt24_shop leaves it open; ours is 08:00, UNSURE). A record is per
// ITEM FUNCTION: the transaction filters the shop's stock by it, so two ITEM records of one function share a record.
//
// The transaction (1409a14b0, dt24_shop section 5): one pulse per second of standing at the counter; it walks the list, and for
// the first unmet record finds the shop's stock items of that function, takes ONE at random (uniform over the matching units) out
// of the shop and into the member's arms, and counts it. A pulse that takes nothing, after a pulse that did, ends the goal (the
// exe's latch + flagTaskDone). The money side is not modelled (dt24_shop UNSURE: the debit was not found in the function; the red
// price label and the "Sell_Item" event are presentation).

#include "ksim/combat_data.hpp"
#include "ksim/rng.hpp"
#include "ksim/state.hpp"
#include "ksim/templates.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ksim::shopping {

// The interaction timer: the exe's +0x98 starts at 1.0 and loses DAT_142133794 per frame (a .bss global, value not in the
// image); ours is one game-second between successful pulses (dt24_shop: "one transaction per ~1 s").
inline constexpr double kTimer = 1.0;
// The list resets daily once the hour passes this (TQ24: after 07:00; ours is 08:00, UNSURE; dt24_shop does not give it).
inline constexpr double kResetHour = 8.0;

// The member's list for `day` from its template's `shopping` refs: a ref whose `chance` the day's roll misses is left out; refs
// of one item function merge, their wants added (and their priorities to the lower); the list is kept in priority order (v2).
// A ref whose ITEM record or item function the data does not give is dropped.
// Pure: the rolls are RngStream(streamSeed(streamSeed(streamSeed(seed, kShoppingStream), diceKey), day)), one per ref in list order.
std::vector<state::ShoppingWant> buildList(std::span<const tmpl::ShoppingRef> refs, const CombatData& items, std::uint64_t seed,
	std::uint64_t diceKey, std::int64_t day);

// Whether any record still wants an item (taken < want).
bool unmet(const std::vector<state::ShoppingWant>& list) noexcept;
// The first record still wanting an item, nullptr when the list is satisfied.
const state::ShoppingWant* firstUnmet(const std::vector<state::ShoppingWant>& list) noexcept;

// One transaction pulse against a shop store: among the stock entries whose ITEM `item function` is `function`, take one unit
// (uniform over the matching units; `roll` in [0, 1) is the draw) and put it in `out`. A stack of two or more is split. False
// (and no change) when no stock entry matches.
bool takeOne(state::Inventory& stock, const CombatData& items, std::uint16_t function, double roll, state::InventoryEntry& out);

} // namespace ksim::shopping
