#pragma once

// The real-economy overlay (TOWN_LIFE_SLICE 7, phases T4a-T4c): the `economy.mode` world flag, the aggregate TownStock seeded from
// the town's trade goods, the camps seeded from the placed production buildings, the hourly flow tick, and the supply board with the
// delivery caravans (7.4). `original` is the V1 default and creates none of this (7.1).
//
// The tick (7.5, OURS where 7 leaves it open): once a game hour, every town's stock moves by (production - consumption) x dt,
// every line clamped to [0, 2 x target]. A producer runs only while it has its inputs and its town has room for the output, so a
// full output stops its consumption (no dead input). A converter's rate is its output rate; its consumption is `consumes`.
// A camp (T4b) runs only while its town has a living staff squad (7.3), and a camp at Full detail runs its own machine
// (state::Producer) instead of the aggregate, at the same rate, so load and unload never double-count.

#include "ksim/economy/producers.hpp"
#include "ksim/core/state.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kdata {
class GameData;
struct Record;
}
namespace flecs {
class world;
}

namespace ksim {

enum class EconomyMode : std::uint8_t
{
	Original = state::kEconomyOriginal,
	Real = state::kEconomyReal,
};

namespace economy {

// One economy tick is one game hour (7.5).
inline constexpr double kTickHours = 1.0;
// A town line's target when its data names no price N (OURS: the VENDOR_LIST's usual weight).
inline constexpr double kDefaultTarget = 100.0;
// A line holds up to this multiple of its target (7.5 "clamped to [0, cap]"; the multiple is OURS).
inline constexpr double kCapMultiple = 2.0;
// One integration never spans more than this many hours (a loaded save's clock gap; the rest is dropped, OURS).
inline constexpr double kMaxCatchUpHours = 24.0;

// ---- pure rules ----

// The most a line holds: kCapMultiple x target (0 for a target of 0 or less).
double cap(const state::StockLine& line) noexcept;
// amount / target clamped to 0..1; 1 when the target is 0 or less (a line with no target is never short).
double fill(const state::StockLine& line) noexcept;
// The mean fill over the lines (1.0 without lines): the 0..1 prosperity input of 7.4.
double prosperity(const state::TownStock& stock) noexcept;
// The line for `itemSid`, or nullptr. Lines are kept sorted by itemSid.
state::StockLine* findLine(state::TownStock& stock, std::string_view itemSid) noexcept;
const state::StockLine* findLine(const state::TownStock& stock, std::string_view itemSid) noexcept;
// Inserts a line (sorted) when the item has none.
state::StockLine& lineFor(state::TownStock& stock, std::string_view itemSid);

// One producer running for `dt` hours against `stock` (7.5). Inputs are consumed up to what the town holds; the output is
// produced at the rate scaled by the input utilisation and stops at the cap. A producer whose output line is at the cap does not
// run (and consumes nothing). `dt <= 0` or a rate <= 0 does nothing.
void runProducer(state::TownStock& stock, const state::EconomyProducer& producer, double dt);

// One integration: every producer of the town (callers pass them in a deterministic order), then prosperity. `dt` is hours.
void integrate(state::TownStock& stock, const std::vector<const state::EconomyProducer*>& producers, double dt);

// The produced items and the input utilisation one producer would make over `dt` hours against `stock`: the lowest input
// availability scales the rate, the output line's room caps it. Pure (a missing line counts as empty at the default target).
struct Flow
{
	double amount = 0.0;
	double utilisation = 1.0;
};
Flow flow(const state::TownStock& stock, const state::EconomyProducer& producer, double dt) noexcept;
// Consumes `produced` items' worth of inputs only (flow()'s other half without the output).
void consumeInputs(state::TownStock& stock, const state::EconomyProducer& producer, double produced) noexcept;
// Consumes `produced` items' worth of inputs and adds them to the output line (flows the produced items).
void applyFlow(state::TownStock& stock, const state::EconomyProducer& producer, double produced);

// One camp machine step (T4b, 7.3): the flow() amount goes into the machine's progress (state::Producer) and every whole unit is
// emitted into the output line, so a camp at Full detail produces from the real machine's operation. The pending fraction is not
// stock yet. OURS: a coarse step keeps the fraction (progress -= 1) rather than dropping the overshoot at the exe's per-frame tick.
void runCampMachine(state::TownStock& stock, const state::EconomyProducer& producer, state::Producer& machine, double dt);

// ---- the camps (T4b) ----

// A camp entity's state::Id is kCampIdBase + its index in World::productionSites(); the staff gate applies to these only (a T4a
// producer entity made by a test or a script is not a camp). Under 1<<62 (producers) and 1<<40 above kCampIdBase.
inline constexpr std::uint64_t kCampIdBase = 1ull << 61;
bool isCamp(std::uint64_t id) noexcept;

// The camp's full-machine rate in items per game hour (7.3): the CRAFTING_SLICE 7.3 converter rate from producers::baseProgressPerTick
// at richness 1, power 1 (no power model) and skill 1 (the camp's workers are automatic operators, CQ31), times every operator slot
// (the tick is per operator). UNSURE: the two functionalities without `output per day` keep the K factor (CQ30, legacy scale).
double campRatePerHour(const producers::Spec& spec) noexcept;
// The inputs of one output item (CQ34: the output ITEM's `ingredients` win, x0.01; the functionality's `consumes` (v1 / 100) only
// when it `overrides ingredients` or the item has no ingredients), scaled to `perHour` by `ratePerHour`.
std::vector<state::ConsumeLine> campConsumes(const kdata::GameData& data, const kdata::Record& function, const std::string& output,
	double ratePerHour);

// ---- the supply board and delivery caravans (T4c, 7.4) ----

// An unclaimed board entry lives this long (OURS: 7.4 says a claim expires, not after how long).
inline constexpr double kBoardEntryHours = 6.0;
// The most one delivery carries (OURS: the overlay's delivery template is not built; the pack animal's bag is 20 x 20 cells).
inline constexpr double kCaravanCapacity = 200.0;
// A match below this many units is not worth a caravan.
inline constexpr double kMinDelivery = 1.0;

// The live board entry of `kind` for `itemSid`, or nullptr. A board keeps at most one of each kind per item.
state::BoardEntry* findEntry(state::SupplyBoard& board, std::uint8_t kind, std::string_view itemSid) noexcept;
const state::BoardEntry* findEntry(const state::SupplyBoard& board, std::uint8_t kind, std::string_view itemSid) noexcept;

// One posting pass for one town (7.4): drop the entries past `now`, then post (or update) the town's offer for every line over its
// target and its need for every line under it. `inbound` is the item -> amount already on its way here: a reserved shortfall is not
// re-posted (the destination half of "both ends reserved before the squad leaves"). A new entry expires kBoardEntryHours after it is
// posted; a live one keeps its clock and only its amount follows the stock. The entries are left sorted by (kind, itemSid, expires).
void postBoard(state::SupplyBoard& board, std::uint64_t town, const state::TownStock& stock, double now,
	const std::map<std::string, double>& inbound);

// What one delivery takes: the source line's current surplus (the amount over its target), the need, and one caravan's capacity.
// 0 when there is no surplus or no need.
double deliveryAmount(const state::StockLine& source, double need) noexcept;

// An order lives at most this long (game hours; OURS, 7.4 does not bound a delivery): a carrier that never arrives is released, the
// goods go back to the source (or are lost with a vanished source) and the destination's need re-posts. No layer may reach a terminal
// state, so the claim can never lie on the road forever.
inline constexpr double kOrderMaxHours = 48.0;

// ---- prices and the draw filter (T4d, 7.4) ----

// The T4 price rule: factor = clamp((target / max(amount, kPriceAmountFloor))^kPriceExponent, kPriceLow, kPriceHigh). 7.4 names the
// shape and says k, lo and hi are data; the data has no such fields, so these are OURS until T4e exposes the rule to a mod script.
inline constexpr double kPriceExponent = 1.0;
inline constexpr double kPriceLow = 0.5;
inline constexpr double kPriceHigh = 2.0;
inline constexpr double kPriceAmountFloor = 1.0; // an empty line is the scarce extreme, not a division by zero

// The factor the town's stock puts on an item's price: > 1 when it is scarce (the amount under the target), < 1 with a glut, exactly
// 1 for an item the town does not stock (or a line with no target: it is never short, 7.2). Both sides of a trade are scaled by it, so
// the spread stays.
double stockFactor(const state::TownStock& stock, std::string_view itemSid) noexcept;

// The T4 draw filter (7.4): a restock takes at most what the town holds. Returns the units taken and reduces the line by them; a town
// with none gives nothing and the stock never goes negative.
std::uint32_t takeFromTown(state::TownStock& stock, std::string_view itemSid, std::uint32_t want) noexcept;

// ---- the data-driven seed ----

// The lines a town starts with (OURS, 7.2 "seed from the town's trade goods / price N data"): the union of the `items` of every
// VENDOR_LIST its merchant squads sell from (`homeTown`, T1's merchant rule) and of the items its active TOWN record lists in
// `trade prices`. A price-listed item's target is that entry's v0 (read as the stocking level, UNSURE: the field is named
// "prices"), else kDefaultTarget; every line starts at its target. Sorted by itemSid.
std::vector<state::StockLine> seedTown(const std::vector<std::string>& merchantItems, const kdata::Record* record);
// The `items` of a VENDOR_LIST record, in list order.
std::vector<std::string> vendorItems(const kdata::GameData& data, const std::string& listSid);

} // namespace economy
} // namespace ksim
