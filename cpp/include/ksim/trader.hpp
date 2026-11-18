#pragma once

// The merchant refresh, copied as-is (docs/reference/TOWN_LIFE_SLICE.md phase T1; kinspect item 53 / b53v): a per-squad timer
// wipes the shop's stock and rolls it again from the squad template's VENDOR_LISTs, and re-seeds the squad wallet. Nothing is
// produced or delivered into it, nothing happens while the squad is not loaded, and a long absence is one refresh, not many.
//
// The pure rules (counts, the split, the weights, the half-fill placement, the wallet) are free functions with no state; the
// refresh itself runs them on a squad entity (state::TraderStock + state::Inventory), reusing the grid inventory (inventory_grid.hpp)
// for placement. Stock lives in the squad's state::Inventory, one store grid of kStoreW x kStoreH (UNSURE, see below); a
// town shop's real containers and a wandering trader's backpacks are T2 and the building slices.
//
// UNSURE (marked again in trader.cpp): the store size; the value of a stack (the record's `value` x count, quality ignored); a
// blueprint or map is an entry named by its RESEARCH / MAP_ITEM record; the `money item` is rolled and counted in the wallet formula
// but no money ITEM record exists to hold it; crossbow / limb levels are stored as quality.level = L / 5; a weapon's manufacturer is
// picked from the list's makers by nearest model (not by their v0); a squad with no `vendors` list (the faction fallback lists)
// is not a merchant yet; TQ2 (the building+0x1f0 skip) never skips; TQ5 (loaded = DetailLevel::Full).

#include "ksim/inventory_grid.hpp"
#include "ksim/state.hpp"

#include <flecs.h>

#include <cstdint>
#include <string>
#include <vector>

namespace kdata {
class GameData;
struct Record;
}

namespace ksim::trade {
class Book;
}

namespace ksim::trader {

using Entry = state::Inventory::Entry;

inline constexpr double kNeverHours = 24000.0; // the exe's period when `vendors refresh time` <= 0
inline constexpr std::uint64_t kIntervalTicks = 10; // Simulation::stepTraders runs this often (TOWN_LIFE_SLICE 3.2 (1))
inline constexpr double kCountLow = 0.8; // N = round(U(0.8, 1.2) x F x wealth)
inline constexpr double kCountHigh = 1.2;
inline constexpr int kDefaultFill = 100; // `vendors fill total amount` 0 (or less) means 100
inline constexpr double kHalfFull = 0.5; // a container stops accepting once its occupancy reaches this
inline constexpr double kMoneyFraction = 0.25;
inline constexpr int kMoneyFloor = 3000;
inline constexpr int kMoneyCap = 25000;
inline constexpr double kMoneyJitter = 0.4; // `vendor money` V +- 0.4 V
// The shop store (UNSURE: the original fills the part-buildings of the shop's interior, whose sizes the data does not give).
inline constexpr std::uint8_t kStoreW = 20;
inline constexpr std::uint8_t kStoreH = 20;

// ---- pure rules ----

// Game hours (day x 24 + hour of day, fractional) at a Clock tick: the unit of `nextRefresh`.
double gameHours(std::uint64_t tick) noexcept;
// The period: `vendors refresh time`, kNeverHours when it is <= 0.
double refreshPeriod(float hours) noexcept;
// N = max(1, round(U(0.8, 1.2) x F)); `roll` in [0, 1) is the U. F < 1 means 100. The "town wealth" factor is hard-wired 1.0.
int stockCount(int fill, double roll) noexcept;
// k_i = round(w_i / W x count) for every list but the last; the last gets the remainder (never negative). One list gets all.
std::vector<int> splitCounts(int count, const std::vector<double>& v0);
// The weight of a pool entry: its v0, or 100 when v0 is 0.
double entryWeight(int v0) noexcept;
// Index of the first entry whose running weight exceeds roll x total; the last entry when rounding runs past. Empty: size().
std::size_t pickWeighted(const std::vector<double>& weights, double roll) noexcept;
// A crossbow's or robotics limb's level 1..5 from the template level L: weights L 100, L+-1 33, L-2 20, L+2 5 (crossbow; only L when
// L is 5) or L 100, L+-1 44, L+-2 22 (robotics); levels outside 1..5 do not exist.
int pickLevel(int level, bool crossbow, double roll) noexcept;
// The squad wallet: a shop with `vendor money` V > 0 gets V + randint(-0.4 V, +0.4 V) (the int part of 0.4 V each way); anything
// else clamp(0.25 x placed value, 3000, 25000). `roll` in [0, 1) is the randint.
std::int32_t walletFor(std::int32_t vendorMoney, bool shop, double placedValue, double roll) noexcept;
// The seed of one refresh's dice: a pure function of the world seed, the squad's Id and the refresh count (D1).
std::uint64_t refreshSeed(std::uint64_t seed, std::uint64_t squadId, std::uint32_t refreshes) noexcept;

// A stack to place and what it is worth.
struct Piece
{
	Entry entry;
	double value = 0.0;
};
struct Placement
{
	std::uint32_t placed = 0; // units placed
	std::uint32_t destroyed = 0; // units that did not fit (or came after the store was half full): destroyed
	double placedValue = 0.0;
};
// The share of the store's cells under entries in section 0 (a footprint per entry).
double occupancy(const state::Inventory& store, const grid::Rules& rules);
// Sorts by value, highest first (a stable sort: ties keep their draw order), then places each with grid::autoplace unless the store
// is already half full; what does not fit is destroyed and counted (D2). Equipped entries are not touched.
Placement placeStock(state::Inventory& store, const grid::Rules& rules, std::vector<Piece> pieces);
// The same over several containers (a wandering trader's backpacks, TQ14): the pieces (highest value first) go round the containers; a piece goes to the next
// one still under half full, what does not fit there tries the following, and what fits nowhere is destroyed. One container behaves exactly as the call above.
Placement placeStock(const std::vector<state::Inventory*>& stores, const grid::Rules& rules, std::vector<Piece> pieces);

// ---- wandering traders: the pure rules (TOWN_LIFE_SLICE phase T2, kinspect item 53 b53t and item 54 T2) ----

inline constexpr double kStayLow = 5.0; // the stay is U(5, 8) game hours on the global clock (TQ10)
inline constexpr double kStayHigh = 8.0;
inline constexpr int kWindowFirstHour = 6; // the squad leaves only while 5 < global hour < 21
inline constexpr int kWindowLastHour = 20;
inline constexpr std::size_t kNearestTowns = 4; // the next town is uniform among the 4 nearest eligible ones
inline constexpr double kWanderSpeed = 25.0; // u/s along the route, loaded or not (b53t)
inline constexpr double kSquadDeathProsperity = 10.0; // a dead squad costs its faction this much prosperity (item 53 / 54)
inline constexpr double kProsperityRegen = 0.1; // per faction update below the cap
inline constexpr double kHostileRelation = -30.0;
inline constexpr double kFriendlyRelation = 50.0; // needed from an owner whose default attitude is hostile

// U(5, 8) hours; `roll` in [0, 1).
double stayHours(double roll) noexcept;
// Whether the global hour of `hours` (game hours, fractional) is 6..20, the leave window.
bool inLeaveWindow(double hours) noexcept;
// The first moment (game hours) at or after arrivedAt + stay at which the squad may leave: the end of the stay when it falls in the window,
// else the next 06:00.
double departureTime(double arrivedAt, double stay) noexcept;

// What the town pick needs to know about a town (all read from the world by the caller; the rule itself is pure).
struct TownFacts
{
	std::uint64_t key = 0; // the town's state::Id (the tie-break)
	double x = 0.0;
	double z = 0.0;
	int type = 2; // TownType: 2 TOWN, 3 VILLAGE, 5 SLAVE_CAMP, 6 MILITARY are visited
	bool isPublic = false; // TOWN `is public`
	bool ownedByOwnFaction = false; // the town's owner is the squad's faction
	bool ownerNotReal = false; // FACTION `not real`
	bool ownerBandit = false; // FACTION `fundamental type` 8
	bool squadBandit = false; // the squad's own faction is a bandit one
	bool ownerIsPlayer = false; // a player faction's town passes the type test
	double relation = 0.0; // what the owner thinks of the squad's faction (100 for itself)
	double ownerAttitude = 0.0; // the owner's `default relation` (its default attitude to strangers)
	bool isCurrent = false; // the town the squad stands in
	bool owned = true; // has an owner at all (a marker town has none and is never visited)
};

// The b53t filter: public or own, not the current town, the owner is real and not hostile (relation <= -30), a bandit owner only for a bandit
// squad, an owner whose default attitude is below -30 needs relation >= 50, and the type is TOWN / VILLAGE / SLAVE_CAMP / MILITARY (a player
// faction's town passes the type test).
bool eligibleTown(const TownFacts& town) noexcept;
// The indices into `towns` of the (at most) 4 eligible towns nearest (squad x, z), nearest first; squared distance, ties by key.
std::vector<std::size_t> nearestEligible(const std::vector<TownFacts>& towns, double x, double z);
// The index into `towns` the pick chooses from nearestEligible: `roll` in [0, 1) is the uniform draw over the list; towns.size() when none.
std::size_t pickTown(const std::vector<TownFacts>& towns, double x, double z, double roll);

// ---- the refresh ----

struct Outcome
{
	std::uint32_t drawn = 0; // units drawn (N)
	std::uint32_t placed = 0;
	std::uint32_t destroyed = 0; // overflow (D2)
	std::uint32_t specials = 0; // unique units made (forced refresh only)
	std::int32_t money = 0;
	bool moneySet = false; // false: nothing was placed, the wallet is as it was
};

// Whether a SQUAD_TEMPLATE record is a merchant this phase serves: `is trader` and at least one `vendors` entry.
bool isMerchant(const kdata::Record& squadTemplate);

// The town whose T4 stock a restock draws from and whose prices it reads (TOWN_LIFE_SLICE 7.4, T4d): the squad's home town, else the town a
// wandering trader is staying in (its TraderTrip); invalid when neither, so there is no filter and no price factor.
flecs::entity marketTown(flecs::entity squad);

// The pack animals (TQ14, kinspect item 54). The backpack record an ANIMAL_CHARACTER wears: the first of its `backpack` list that exists (always
// worn, no chance roll); empty when it has none. A squad template has pack animals when one of its `animals` / `animals2` entries wears one;
// such a squad's refresh works on the backpacks of its living members (animals included), not on a store grid of its own.
std::string packBackpack(const kdata::GameData& data, const kdata::Record& character);
bool hasPackAnimals(const kdata::GameData& data, const kdata::Record& squadTemplate);
// Gives one squad member that is a pack animal its Inventory (the animal's own `inventory w` x `inventory h` grid, 4 x 8 for the Pack Beast; the
// backpack worn in the BACKPACK slot, its grid the item's `storage size`) and the spawn fill: each `vendors` list of the squad template draws
// its `items count` (default 40) items; without `vendors` the animal's `inventory` list; otherwise empty. 1 when the member was given one; 0 for
// a member that is not a pack animal or already has an Inventory. The dice are a pure function of (seed, the member's Id).
std::uint32_t seedPackAnimal(flecs::entity member, const kdata::GameData& data, const trade::Book& book, const grid::Rules& rules, std::uint64_t seed,
	const kdata::Record& squadTemplate);

// One refresh of a merchant squad entity (SquadInstance + TraderStock + Inventory): wipe the non-unique stock, (force: make the special
// items), roll N draws, place them, re-seed the wallet, re-arm `nextRefresh = now + P` and count it. Creates the Inventory
// (the empty store) and TraderStock when absent. `nowHours` is gameHours(tick). The stock is ordered and the wallet sized by `book`'s Value.
Outcome refresh(flecs::entity squad, const kdata::GameData& data, const trade::Book& book, const grid::Rules& rules, std::uint64_t seed, double nowHours, bool force);

} // namespace ksim::trader
