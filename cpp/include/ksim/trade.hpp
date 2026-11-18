#pragma once

// Trade (docs/reference/MINE_SELL_SLICE.md M4-M5, SIMLINK.md TRADE 53, order 68 against a trader's stock, events 13 action 6 and 40;
// kinspect item 59 MQ5 / MQ6 / MQ8 and findings/prices15_static.md for the price chain, items14_static.md for price()).
//
// Talk (order 60 action 6) on a character of a merchant squad (state::TraderStock, TOWN_LIFE_SLICE T1) opens a trade for the talker's
// session: the Session keeps the window (actor -> trader character), the sim keeps nothing but the stock (the squad's state::Inventory)
// and the two wallets. Order 68 against the squad's Id buys (take 1) or sells (take 0) at the prices below.
//
// The price chain (prices15 section 0, H; the pieces marked UNSURE are not in the exe's chain or not traced):
//   price(item)  = the exe's price() with applyMults: a leaf ITEM floor(value x mult + 0.5), mult = `trade price mult` (live 0.6) or, for
//                  a `food crop`, `food price mult`, both x `global price mult`; an ITEM with `ingredients` is the sum of its ingredients'
//                  prices x v0 / 100 x (`profitability` + 1), each term rounded; a CONTAINER its plain `value`. A WEAPON, ARMOUR or CROSSBOW
//                  is gearPrice() (item+0x1f0; kinspect findings/s72_static.md section 2: level, maker, type; docs/reference/MINE_SELL_SLICE.md
//                  "Gear prices"). UNSURE: a LIMB_REPLACEMENT is `value` x `crossbow price mult` x global (not traced).
//   getValue     = int(townFactor x price) for the trade goods (none for gear, s72 section 4); for the trader's buying price (sell) x
//                  `loot price mult` (`... GEAR` for a weapon, armour or crossbow: weapon int(E x F x GEAR), armour int(E x (F x GEAR)))
//                  unless the item is a `trade item`; then int(E x ...), E = the squad template's `buy mult` x the town's
//                  (else the faction's) ITEMS_CULTURE `trade prices` percent.
//   townFactor   = (1 - M) + u x 2M per (town, item), u a pure function of (world seed, town Id, item): nothing is saved. UNSURE: the exe
//                  draws it once at town creation and pulls it to within d x M x 1e-5 of every earlier town's; neither the pull nor the
//                  smoothing over time is modelled, and a wandering trader (no home town) has no factor.
//   stockFactor  = the T4 overlay's own factor (TOWN_LIFE_SLICE 7.4, T4d), on top of the chain: the market town's TownStock line gives
//                  clamp((target / max(amount, 1))^k, lo, hi), and both sides of the quote are scaled by it, so the spread stays. Exactly
//                  1 in `original` mode (no TownStock) and for an item the town does not stock. With the T4e economy hooks on, the
//                  script's economy.price answers instead (economy_script.hpp); the engine rule stands for a call it fails.
// Not modelled (UNSURE): the charges term of a single partly used item, stolen goods (no ownership marks on items), item quality.

#include "ksim/state.hpp"

namespace ksim::economy
{
class Hooks; // economy_script.hpp, when the hooks are on
}

#include <flecs.h>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace kdata {
class GameData;
struct Record;
}

namespace ksim::trade {

using Entry = state::Inventory::Entry;

// MQ8: the transfer range, squared XZ distance d^2 < 1e6 centre to centre, in raw world units (= ours).
inline constexpr double kRange = 1000.0;

// The CONSTANTS keys the chain reads (GLOBAL CONSTANTS, merged data). The defaults are the schema's.
struct Constants
{
	double global = 1.0; // `global price mult`
	double trade = 1.0; // `trade price mult` (rebirth 0.6)
	double food = 1.0; // `food price mult`
	double sword = 1.0; // `sword price mult`
	double armour = 1.0; // `armor price mult`
	double clothing = 1.0; // `clothing price mult` (armour without `part coverage`)
	double crossbow = 1.0; // `crossbow price mult`
	double robotics = 1.0; // `robotics price mult`
	double loot = 0.4; // `loot price mult`
	double lootGear = 0.15; // `loot price mult GEAR`
	double margin = 0.5; // `trade profit margins` M (rebirth 0.3)
};

// One item's price pair for a trader: `buy` is what the player pays for one unit, `sell` what the trader pays for it, -1 when he will
// not buy it (see Refusal). Both are the exe's getValue(item, false) and getValue(item, true).
enum class Refusal : std::uint8_t
{
	None = 0,
	NotForSale, // no value at all: "That's not for sale." (code 0xc)
	IllegalOnly, // a smuggler (`buys illegal`) buys only illegal goods: "Smugglers only buy illegal goods." (code 0xe). UNSURE: the reading of the flag
};
struct Quote
{
	std::int32_t buy = 0;
	std::int32_t sell = -1;
	Refusal refusal = Refusal::None; // why `sell` is -1
};

// What one trader squad does to prices, read from the world once per TRADE frame / order.
struct Terms
{
	double buyMult = 1.0; // SQUAD_TEMPLATE `buy mult`
	bool buysIllegal = false; // SQUAD_TEMPLATE `buys illegal`
	std::uint64_t townId = 0; // the home town's state::Id, 0 = none (no factor)
	const kdata::Record* town = nullptr; // the home town's TOWN record in force
	const kdata::Record* faction = nullptr; // the squad's FACTION record
	// The T4 economy overlay (TOWN_LIFE_SLICE 7.4): the market town's TownStock, nullptr in `original` mode (no overlay state), with no
	// town, or in a hand-built Terms (a plain base price). With it, quote() scales both sides by economy::stockFactor.
	const state::TownStock* stock = nullptr;
	// The T4e economy hooks (economy_script.hpp), nullptr unless a script is loaded: its economy.price answers for stockFactor.
	economy::Hooks* hooks = nullptr;
};

class Book
{
public:
	// `hooks`: the T4e economy hooks (economy_script.hpp), nullptr = the engine rules only. Not owned.
	Book(const kdata::GameData& data, std::uint64_t seed, economy::Hooks* hooks = nullptr);

	const Constants& constants() const noexcept { return k_; }
	// The terms of a merchant squad entity (SquadInstance).
	Terms termsOf(flecs::entity squad) const;
	// The pair for one entry (stack count does not matter: a per-unit price).
	Quote quote(const Terms& terms, const Entry& entry) const;
	// The town's factor for an item: 1 when the town is unknown or the item is not a trade good.
	double townFactor(std::uint64_t townId, const std::string& itemSid) const;
	// item+0x1f0 of one gear instance (s72 section 2): a WEAPON with its maker (`price mod`) and level (percent 0..100), a CROSSBOW, an
	// ARMOUR (level 0 = `value` path). `maker` is a WEAPON_MANUFACTURER sid ("" = none). Any other record type: 0.
	std::int32_t gearPrice(const kdata::Record& item, const std::string& maker, int level) const;
	// price(): the exe's base price of a record (gear: an instance with no maker at level 0).
	std::int32_t basePrice(const kdata::Record& item) const;
	// The ITEMS_CULTURE percent / 100 for an item at a town / faction, 1 when none lists it.
	double cultureMult(const Terms& terms, const std::string& itemSid) const;
	bool isTradeGood(const std::string& itemSid) const { return goods_.count(itemSid) != 0; }

private:
	bool illegal(const Terms& terms, const std::string& itemSid) const;
	std::int32_t priceOf(const kdata::Record& item, int depth) const;
	const std::map<std::string, double>* prices(const std::string& cultureSid) const;

	const kdata::GameData& data_;
	std::uint64_t seed_;
	economy::Hooks* hooks_ = nullptr; // the T4e economy hooks: economy.price replaces stockFactor
	Constants k_;
	std::set<std::string> goods_; // VENDOR_LIST "all trade goods" `items`
	// ITEMS_CULTURE (and FACTION) records: item sid -> `trade prices` percent / 100.
	std::map<std::string, std::map<std::string, double>> cultures_;
	std::map<std::string, std::set<std::string>> illegalGoods_; // the same records' `illegal goods`
	std::int32_t weaponType_, armourType_, containerType_, crossbowType_, limbType_, itemType_;
};

// The squad entity's character `member` belongs to, or an invalid entity.
flecs::entity squadOf(flecs::entity member);

} // namespace ksim::trade
