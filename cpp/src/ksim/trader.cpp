#include "ksim/trader.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/detail.hpp"
#include "ksim/economy.hpp"
#include "ksim/rng.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trade.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

namespace ksim::trader {

// ---- pure rules ----

double gameHours(std::uint64_t tick) noexcept
{
	return gameHoursIn(tick);
}

double refreshPeriod(float hours) noexcept
{
	return hours <= 0.0f ? kNeverHours : static_cast<double>(hours);
}

int stockCount(int fill, double roll) noexcept
{
	const int base = fill < 1 ? kDefaultFill : fill;
	const double scaled = (kCountLow + (kCountHigh - kCountLow) * roll) * static_cast<double>(base);
	return std::max(1, static_cast<int>(std::floor(scaled + 0.5)));
}

std::vector<int> splitCounts(int count, const std::vector<double>& v0)
{
	std::vector<int> parts(v0.size(), 0);
	if (parts.empty())
		return parts;
	double total = 0.0;
	for (const double weight : v0)
		total += weight;
	int given = 0;
	for (std::size_t i = 0; i + 1 < parts.size(); ++i)
	{
		parts[i] = total > 0.0 ? static_cast<int>(std::floor(v0[i] / total * static_cast<double>(count) + 0.5)) : 0;
		given += parts[i];
	}
	parts.back() = std::max(0, count - given);
	return parts;
}

double entryWeight(int v0) noexcept
{
	return v0 == 0 ? 100.0 : static_cast<double>(v0);
}

std::size_t pickWeighted(const std::vector<double>& weights, double roll) noexcept
{
	double total = 0.0;
	for (const double weight : weights)
		total += weight;
	if (total <= 0.0)
		return weights.size();
	double left = roll * total;
	std::size_t last = weights.size();
	for (std::size_t i = 0; i < weights.size(); ++i)
	{
		if (weights[i] <= 0.0)
			continue;
		last = i;
		left -= weights[i];
		if (left < 0.0)
			return i;
	}
	return last;
}

int pickLevel(int level, bool crossbow, double roll) noexcept
{
	level = std::clamp(level, 1, 5);
	if (crossbow && level == 5)
		return 5;
	// Offsets -2 .. +2 from the template level.
	static constexpr double kCrossbow[5] = {20.0, 33.0, 100.0, 33.0, 5.0};
	static constexpr double kRobotics[5] = {22.0, 44.0, 100.0, 44.0, 22.0};
	std::vector<double> weights(5, 0.0);
	for (int d = -2; d <= 2; ++d)
	{
		if (level + d >= 1 && level + d <= 5)
			weights[static_cast<std::size_t>(d + 2)] = (crossbow ? kCrossbow : kRobotics)[d + 2];
	}
	const std::size_t at = pickWeighted(weights, roll);
	return level + static_cast<int>(at) - 2;
}

std::int32_t walletFor(std::int32_t vendorMoney, bool shop, double placedValue, double roll) noexcept
{
	if (shop && vendorMoney > 0)
	{
		const int spread = static_cast<int>(kMoneyJitter * static_cast<double>(vendorMoney));
		const int offset = std::min(2 * spread, static_cast<int>(std::floor(roll * static_cast<double>(2 * spread + 1))));
		return vendorMoney - spread + offset;
	}
	return static_cast<std::int32_t>(std::clamp(placedValue * kMoneyFraction, static_cast<double>(kMoneyFloor), static_cast<double>(kMoneyCap)));
}

std::uint64_t refreshSeed(std::uint64_t seed, std::uint64_t squadId, std::uint32_t refreshes) noexcept
{
	return splitMix64(splitMix64(streamSeed(seed, kTraderStream) ^ squadId) + refreshes);
}

double stayHours(double roll) noexcept
{
	return kStayLow + (kStayHigh - kStayLow) * std::clamp(roll, 0.0, 1.0);
}

namespace {

int hourOfDay(double hours) noexcept
{
	const double day = std::floor(hours / 24.0);
	return static_cast<int>(std::floor(hours - day * 24.0));
}

} // namespace

bool inLeaveWindow(double hours) noexcept
{
	const int hour = hourOfDay(hours);
	return hour >= kWindowFirstHour && hour <= kWindowLastHour;
}

double departureTime(double arrivedAt, double stay) noexcept
{
	const double end = arrivedAt + stay;
	if (inLeaveWindow(end))
		return end;
	const double day = std::floor(end / 24.0);
	const double dayStart = day * 24.0;
	// Before 06:00: the same day's 06:00; from 21:00: the next day's.
	return end - dayStart < static_cast<double>(kWindowFirstHour) ? dayStart + kWindowFirstHour : dayStart + 24.0 + kWindowFirstHour;
}

bool eligibleTown(const TownFacts& town) noexcept
{
	if (town.isCurrent || !town.owned || town.ownerNotReal)
		return false;
	if (!town.isPublic && !town.ownedByOwnFaction)
		return false;
	if (town.relation <= kHostileRelation)
		return false;
	if (town.ownerBandit && !town.squadBandit)
		return false;
	if (town.ownerAttitude < kHostileRelation && town.relation < kFriendlyRelation)
		return false;
	const bool visited = town.type == 2 || town.type == 3 || town.type == 5 || town.type == 6;
	return visited || town.ownerIsPlayer;
}

std::vector<std::size_t> nearestEligible(const std::vector<TownFacts>& towns, double x, double z)
{
	std::vector<std::size_t> order;
	for (std::size_t i = 0; i < towns.size(); ++i)
	{
		if (eligibleTown(towns[i]))
			order.push_back(i);
	}
	const auto far = [&](std::size_t i)
	{
		return (towns[i].x - x) * (towns[i].x - x) + (towns[i].z - z) * (towns[i].z - z);
	};
	std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b)
	{
		const double da = far(a);
		const double db = far(b);
		return da != db ? da < db : towns[a].key < towns[b].key;
	});
	if (order.size() > kNearestTowns)
		order.resize(kNearestTowns);
	return order;
}

std::size_t pickTown(const std::vector<TownFacts>& towns, double x, double z, double roll)
{
	const std::vector<std::size_t> near = nearestEligible(towns, x, z);
	if (near.empty())
		return towns.size();
	return near[std::min(near.size() - 1, static_cast<std::size_t>(std::clamp(roll, 0.0, 1.0) * static_cast<double>(near.size())))];
}

double occupancy(const state::Inventory& store, const grid::Rules& rules)
{
	long used = 0;
	for (const Entry& entry : store.items)
	{
		if (entry.slot == state::Inventory::kCarried && entry.section == 0)
		{
			const grid::Size size = rules.footprint(entry.itemSid);
			used += static_cast<long>(size.w) * size.h;
		}
	}
	return static_cast<double>(used) / static_cast<double>(std::max(1, store.gridW * store.gridH));
}

Placement placeStock(state::Inventory& store, const grid::Rules& rules, std::vector<Piece> pieces)
{
	return placeStock(std::vector<state::Inventory*>{&store}, rules, std::move(pieces));
}

Placement placeStock(const std::vector<state::Inventory*>& stores, const grid::Rules& rules, std::vector<Piece> pieces)
{
	std::stable_sort(pieces.begin(), pieces.end(), [](const Piece& a, const Piece& b) { return a.value > b.value; });
	Placement result;
	std::size_t turn = 0; // the pieces go round the containers, highest value first; a full one is skipped
	for (Piece& piece : pieces)
	{
		const std::uint32_t count = piece.entry.count;
		std::uint32_t left = count;
		for (std::size_t k = 0; k < stores.size(); ++k)
		{
			state::Inventory* store = stores[(turn + k) % stores.size()];
			if (left == 0)
				break;
			if (occupancy(*store, rules) >= kHalfFull)
				continue;
			Entry rest = piece.entry;
			rest.count = left;
			left = grid::autoplace(*store, rules, std::move(rest));
		}
		++turn;
		result.placed += count - left;
		result.destroyed += left;
		result.placedValue += count > 0 ? piece.value * static_cast<double>(count - left) / static_cast<double>(count) : 0.0;
	}
	return result;
}

// ---- the refresh ----

namespace {

enum class Kind : std::uint8_t
{
	Plain,
	Weapon,
	Armour,
	Crossbow,
	Robotics,
	Once, // a blueprint or a map: at most once per list per refresh
};

struct PoolItem
{
	std::string sid;
	const kdata::Record* record;
	Kind kind;
};

struct Category
{
	const char* name;
	Kind kind;
};
// TOWN_LIFE_SLICE 3.2 (7): the combined pool of one VENDOR_LIST.
constexpr Category kCategories[] = {
	{"items", Kind::Plain},
	{"weapons", Kind::Weapon},
	{"armours", Kind::Armour},
	{"clothing", Kind::Armour},
	{"containers", Kind::Plain},
	{"blueprints", Kind::Once},
	{"armour blueprints", Kind::Once},
	{"crossbows", Kind::Crossbow},
	{"crossbow blueprints", Kind::Once},
	{"backpack blueprints", Kind::Once},
	{"robotics", Kind::Robotics},
	{"maps", Kind::Once},
};

// The faction's `armors 0..5` weights (the schema defaults 50 / 100 / 50 / 2 / 0 / 0 when the faction record is missing or lacks them;
// the original's no-faction defaults 50 / 80 / 25 / 5 apply to a squad with no faction, UNSURE which a factionless merchant gets).
void gradeWeights(const kdata::Record* faction, double out[6])
{
	static constexpr int kSchema[6] = {50, 100, 50, 2, 0, 0};
	static constexpr int kNoFaction[6] = {50, 80, 25, 5, 0, 0};
	for (int g = 0; g < 6; ++g)
		out[g] = faction != nullptr ? faction->intOr("armors " + std::to_string(g), kSchema[g]) : kNoFaction[g];
}

struct Pool
{
	std::vector<PoolItem> items;
	std::vector<double> weights;
	std::vector<std::string> makers; // the `weapon manufacturers` records
};

Pool buildPool(const kdata::GameData& data, const kdata::Record& list)
{
	Pool pool;
	std::set<std::string> weaponTypes; // what the list's makers can make
	if (const kdata::RefCategory* makers = list.findRefs("weapon manufacturers"))
	{
		for (const kdata::RefEntry& entry : makers->entries)
		{
			const kdata::Record* maker = entry.opaqueEntry ? nullptr : data.find(entry.targetID);
			if (maker == nullptr)
				continue;
			pool.makers.push_back(entry.targetID);
			if (const kdata::RefCategory* types = maker->findRefs("weapon types"))
			{
				for (const kdata::RefEntry& type : types->entries)
				{
					if (!type.opaqueEntry)
						weaponTypes.insert(type.targetID);
				}
			}
		}
	}
	for (const Category& category : kCategories)
	{
		const kdata::RefCategory* refs = list.findRefs(category.name);
		if (refs == nullptr)
			continue;
		for (const kdata::RefEntry& entry : refs->entries)
		{
			const kdata::Record* record = entry.opaqueEntry ? nullptr : data.find(entry.targetID);
			if (record == nullptr)
				continue; // a dangling reference has no record to stock
			// A weapon counts only when a listed maker makes its type.
			if (category.kind == Kind::Weapon && weaponTypes.count(entry.targetID) == 0)
				continue;
			pool.items.push_back({entry.targetID, record, category.kind});
			pool.weights.push_back(entryWeight(entry.v0));
		}
	}
	return pool;
}

Entry makeEntry(const std::string& sid, std::uint32_t count, const state::ItemQuality& quality = {})
{
	Entry entry;
	entry.itemSid = sid;
	entry.count = count;
	entry.slot = state::Inventory::kCarried;
	entry.quality = quality;
	return entry;
}

void addPiece(std::vector<Piece>& pieces, const grid::Rules& rules, Entry entry)
{
	// Stackables merge up to `stackable` before a new item is made.
	const std::uint32_t cap = static_cast<std::uint32_t>(rules.stackCap(entry.itemSid, nullptr));
	for (Piece& piece : pieces)
	{
		if (piece.entry.itemSid == entry.itemSid && piece.entry.quality == entry.quality && piece.entry.count < cap)
		{
			piece.entry.count += entry.count;
			return;
		}
	}
	pieces.push_back({std::move(entry), 0.0});
}

// The unique special items (forced refresh): stacks of the item's `stackable`, each flagged unique, placed with plain autoplace
// (no half-full rule; what does not fit is destroyed, the exe warns "refreshBuildingInventory").
void makeSpecials(const kdata::GameData& data, const kdata::Record& squadTemplate, const grid::Rules& rules, state::Inventory& store, Outcome& out)
{
	for (const char* list : {"special items", "special map items"})
	{
		const kdata::RefCategory* refs = squadTemplate.findRefs(list);
		if (refs == nullptr)
			continue;
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (entry.opaqueEntry || data.find(entry.targetID) == nullptr)
				continue;
			const std::uint32_t cap = static_cast<std::uint32_t>(rules.stackCap(entry.targetID, nullptr));
			for (std::uint32_t left = static_cast<std::uint32_t>(std::max(0, entry.v0)); left > 0;)
			{
				const std::uint32_t stack = std::min(cap, left);
				left -= stack;
				Entry unit = makeEntry(entry.targetID, stack);
				unit.unique = true;
				const std::uint32_t over = grid::autoplace(store, rules, std::move(unit));
				out.specials += stack - over;
				out.destroyed += over;
			}
		}
	}
}

} // namespace

bool isMerchant(const kdata::Record& squadTemplate)
{
	const kdata::RefCategory* vendors = squadTemplate.findRefs("vendors");
	return squadTemplate.boolOr("is trader", false) && vendors != nullptr && !vendors->entries.empty();
}

flecs::entity marketTown(flecs::entity squad)
{
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	if (instance == nullptr)
		return flecs::entity();
	if (instance->homeTown.id != 0)
		return flecs::entity(squad.world().c_ptr(), instance->homeTown.id);
	// A wandering trader's market is the town it is staying in (7.4); on the road it has none.
	const state::TraderTrip* trip = squad.get<state::TraderTrip>();
	if (trip != nullptr && trip->staying && trip->town.id != 0)
		return flecs::entity(squad.world().c_ptr(), trip->town.id);
	return flecs::entity();
}

std::string packBackpack(const kdata::GameData& data, const kdata::Record& character)
{
	const kdata::RefCategory* backpacks = character.findRefs("backpack");
	if (backpacks == nullptr)
		return {};
	for (const kdata::RefEntry& entry : backpacks->entries)
	{
		if (!entry.opaqueEntry && data.find(entry.targetID) != nullptr)
			return entry.targetID;
	}
	return {};
}

bool hasPackAnimals(const kdata::GameData& data, const kdata::Record& squadTemplate)
{
	for (const char* list : {"animals", "animals2"})
	{
		const kdata::RefCategory* animals = squadTemplate.findRefs(list);
		if (animals == nullptr)
			continue;
		for (const kdata::RefEntry& entry : animals->entries)
		{
			const kdata::Record* animal = entry.opaqueEntry ? nullptr : data.find(entry.targetID);
			if (animal != nullptr && !packBackpack(data, *animal).empty())
				return true;
		}
	}
	return false;
}

namespace {

struct Listed
{
	const kdata::Record* record;
	double v0;
};

std::vector<Listed> vendorLists(const kdata::GameData& data, const kdata::Record& squadTemplate)
{
	std::vector<Listed> lists;
	if (const kdata::RefCategory* vendors = squadTemplate.findRefs("vendors"))
	{
		for (const kdata::RefEntry& entry : vendors->entries)
		{
			const kdata::Record* list = entry.opaqueEntry ? nullptr : data.find(entry.targetID);
			if (list != nullptr)
				lists.push_back({list, static_cast<double>(entry.v0)});
		}
	}
	return lists;
}

// `count` draws from one list's pool (the weighted pick, the quality rolls); the stacks merge while they can. The dice order is part of the
// refresh's determinism: the grade and pick rolls of a weapon or armour come right after its item pick.
void drawFrom(std::vector<Piece>& pieces, const kdata::GameData& data, const grid::Rules& rules, const kdata::Record& list, int count,
	const double weights6[6], int crossbowLevel, int roboticsLevel, RngStream& dice)
{
	Pool pool = buildPool(data, list);
	for (int draw = 0; draw < count; ++draw)
	{
		const std::size_t at = pickWeighted(pool.weights, dice.uniform01());
		if (at >= pool.items.size())
			break; // the pool is empty (or only spent blueprints are left)
		const PoolItem& item = pool.items[at];
		state::ItemQuality quality;
		switch (item.kind)
		{
		case Kind::Weapon:
		case Kind::Armour:
		{
			const double grade = dice.uniform01();
			const double pick = dice.uniform01();
			quality = CombatData::rollQuality(data, *item.record, grade, pick, weights6, item.kind == Kind::Weapon ? &pool.makers : nullptr);
			break;
		}
		case Kind::Crossbow:
		case Kind::Robotics:
		{
			// UNSURE: where the level lives on the item. Stored as the item level L / 5 and the display grade L - 1.
			const int level = pickLevel(item.kind == Kind::Crossbow ? crossbowLevel : roboticsLevel, item.kind == Kind::Crossbow, dice.uniform01());
			quality.level = static_cast<double>(level) / 5.0;
			quality.grade = static_cast<std::uint8_t>(level - 1);
			break;
		}
		case Kind::Once:
			pool.weights[at] = 0.0; // never twice in one list
			break;
		case Kind::Plain:
			break;
		}
		addPiece(pieces, rules, makeEntry(item.sid, 1, quality));
	}
}

// A stack is worth its Value (the tooltip price with no merchant terms: gear at its maker / level / uniform, s72) x its count.
// UNSURE: the exe's order / wallet may read another price (condition, the merchant's own terms).
void priceStock(std::vector<Piece>& pieces, const trade::Book& book)
{
	for (Piece& piece : pieces)
		piece.value = static_cast<double>(book.quote(trade::Terms(), piece.entry).buy) * static_cast<double>(piece.entry.count);
}

const kdata::Record* factionRecord(flecs::entity squad, const kdata::GameData& data, const state::SquadInstance& instance)
{
	if (instance.faction.id == 0)
		return nullptr;
	const flecs::entity owner = squad.world().entity(instance.faction.id);
	if (owner.is_alive() && owner.get<state::FactionState>() != nullptr)
		return data.find(owner.get<state::FactionState>()->templateSid);
	return nullptr;
}

// The living members whose Inventory wears a backpack with a grid: the containers of a wandering trader (TQ14), in Id order.
std::vector<flecs::entity> packCarriers(flecs::entity squad, const grid::Rules& rules)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	squad.world().query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		const state::Inventory* inventory = member.get<state::Inventory>();
		if (inventory == nullptr)
			return;
		const state::Fighter* fighter = member.get<state::Fighter>();
		if (fighter != nullptr && fighter->dead)
			return;
		const state::Inventory::Entry* bag = inventory->wornBackpack();
		const grid::Size size = bag != nullptr ? rules.bagSize(bag->itemSid) : grid::Size{0, 0};
		if (size.w > 0 && size.h > 0)
			found.emplace_back(id.value, member);
	});
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<flecs::entity> carriers;
	for (const auto& [id, member] : found)
		carriers.push_back(member);
	return carriers;
}

} // namespace

Outcome refresh(flecs::entity squad, const kdata::GameData& data, const trade::Book& book, const grid::Rules& rules, std::uint64_t seed, double nowHours, bool force)
{
	Outcome out;
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	const kdata::Record* squadTemplate = instance != nullptr ? data.find(instance->templateSid) : nullptr;
	const state::Id* id = squad.get<state::Id>();
	if (squadTemplate == nullptr || id == nullptr)
		return out;

	// A wandering trader (a squad with pack animals) keeps its stock in the backpacks of its living members, animals included; any other
	// merchant in the squad's one store grid (D13). All members dead: no container, so no refresh and the timer is left as it is.
	const bool packs = hasPackAnimals(data, *squadTemplate);
	std::vector<flecs::entity> carriers;
	if (packs)
	{
		carriers = packCarriers(squad, rules);
		if (carriers.empty())
			return out;
	}

	state::TraderStock stock = squad.get<state::TraderStock>() != nullptr ? *squad.get<state::TraderStock>() : state::TraderStock{};
	state::Inventory store;
	std::vector<state::Inventory> bags; // the backpacks' grids, one per carrier
	if (packs)
	{
		for (const flecs::entity carrier : carriers)
		{
			const state::Inventory::Entry& bag = *carrier.get<state::Inventory>()->wornBackpack();
			const grid::Size size = rules.bagSize(bag.itemSid);
			state::Inventory& view = bags.emplace_back();
			view.gridW = static_cast<std::uint8_t>(size.w);
			view.gridH = static_cast<std::uint8_t>(size.h);
			view.items = bag.contents;
		}
	} else if (const state::Inventory* held = squad.get<state::Inventory>()) {
		store = *held;
	} else {
		store.gridW = kStoreW;
		store.gridH = kStoreH;
	}
	std::vector<state::Inventory*> targets;
	if (packs)
	{
		for (state::Inventory& view : bags)
			targets.push_back(&view);
	} else {
		targets.push_back(&store);
	}
	// The wipe: everything that is not unique goes, what the player sold in included.
	for (state::Inventory* target : targets)
		std::erase_if(target->items, [](const Entry& entry) { return !entry.unique; });

	RngStream dice(refreshSeed(seed, id->value, stock.refreshes));
	if (force)
		makeSpecials(data, *squadTemplate, rules, *targets.front(), out);

	const kdata::Record* faction = factionRecord(squad, data, *instance);
	double weights6[6];
	gradeWeights(faction, weights6);

	// The lists and their v0 (the share of N).
	const std::vector<Listed> lists = vendorLists(data, *squadTemplate);

	out.drawn = lists.empty() ? 0 : static_cast<std::uint32_t>(stockCount(squadTemplate->intOr("vendors fill total amount", 15), dice.uniform01()));
	// A `money item` roll per list (UNSURE: no ITEM record holds physical money, so the amount only joins the wallet formula's value).
	double moneyItems = 0.0;
	for (const Listed& listed : lists)
	{
		const double roll = dice.uniform01();
		if (roll < static_cast<double>(listed.record->floatOr("money item prob", 0.0f)))
		{
			const std::int64_t low = listed.record->intOr("money item min", 0);
			const std::int64_t high = listed.record->intOr("money item max", 0);
			moneyItems += static_cast<double>(dice.between(low, std::max(low, high)));
		}
	}

	std::vector<double> shares;
	for (const Listed& listed : lists)
		shares.push_back(listed.v0);
	const std::vector<int> counts = splitCounts(static_cast<int>(out.drawn), shares);
	const int crossbowLevel = squadTemplate->intOr("crossbow levels", 1);
	const int roboticsLevel = squadTemplate->intOr("robotics levels", 1);

	std::vector<Piece> pieces;
	for (std::size_t n = 0; n < lists.size(); ++n)
		drawFrom(pieces, data, rules, *lists[n].record, counts[n], weights6, crossbowLevel, roboticsLevel, dice);
	// The T4 draw filter (TOWN_LIFE_SLICE 7.4, T4d): in economy mode a town's shops draw only what its stock holds, and every unit drawn
	// leaves the line, so shop stock is bounded by the town and never makes the stock negative. No TownStock (original mode) or no
	// market town: the roll is exactly T1's.
	const flecs::entity market = marketTown(squad);
	if (state::TownStock* townStock = market.is_valid() ? market.get_mut<state::TownStock>() : nullptr)
	{
		std::vector<Piece> kept;
		kept.reserve(pieces.size());
		for (Piece& piece : pieces)
		{
			const std::uint32_t taken = economy::takeFromTown(*townStock, piece.entry.itemSid, piece.entry.count);
			if (taken == 0)
				continue;
			piece.entry.count = taken;
			kept.push_back(std::move(piece));
		}
		pieces = std::move(kept);
	}
	priceStock(pieces, book);

	const Placement placement = placeStock(targets, rules, std::move(pieces));
	out.placed = placement.placed;
	out.destroyed += placement.destroyed;
	if (packs)
	{
		// Back into each carrier's backpack; the member's inventory is put in order (materials, caps) and what settle cannot keep is destroyed too.
		for (std::size_t i = 0; i < carriers.size(); ++i)
		{
			state::Inventory inventory = *carriers[i].get<state::Inventory>();
			inventory.wornBackpack()->contents = std::move(bags[i].items);
			for (const Entry& over : grid::settle(inventory, rules, carriers[i].get<state::Id>()->value))
				out.destroyed += over.count;
			++inventory.version;
			carriers[i].set<state::Inventory>(std::move(inventory));
		}
	} else {
		// The store's materials (a pick seeded by the squad and the record); what settle cannot keep is destroyed too.
		for (const Entry& over : grid::settle(store, rules, id->value))
			out.destroyed += over.count;
		++store.version;
	}

	// The wallet (same call). Skipped when nothing was generated.
	const bool anything = out.placed > 0 || moneyItems > 0.0;
	const double wallet = dice.uniform01();
	if (anything)
	{
		const bool shop = !packs && instance->homeTown.id != 0;
		stock.money = walletFor(squadTemplate->intOr("vendor money", 0), shop, placement.placedValue + moneyItems, wallet);
		out.moneySet = true;
	}
	out.money = stock.money;

	stock.nextRefresh = nowHours + refreshPeriod(squadTemplate->floatOr("vendors refresh time", 24.0f));
	++stock.refreshes;
	stock.seeded = stock.seeded || force;
	stock.destroyed += out.destroyed;
	if (!packs)
		squad.set<state::Inventory>(std::move(store));
	squad.set<state::TraderStock>(stock);
	return out;
}

std::uint32_t seedPackAnimal(flecs::entity member, const kdata::GameData& data, const trade::Book& book, const grid::Rules& rules, std::uint64_t seed,
	const kdata::Record& squadTemplate)
{
	const state::CharacterInstance* character = member.get<state::CharacterInstance>();
	const state::Id* id = member.get<state::Id>();
	const kdata::Record* animal = character != nullptr ? data.find(character->templateSid) : nullptr;
	if (animal == nullptr || id == nullptr || member.get<state::Inventory>() != nullptr)
		return 0;
	const std::string backpack = packBackpack(data, *animal);
	if (backpack.empty())
		return 0;
	const grid::Size size = rules.bagSize(backpack);

	// The animal's own grid is `inventory w` x `inventory h` (4 x 8 by default); the backpack is always worn, no roll (TQ14).
	state::Inventory inventory;
	inventory.gridW = static_cast<std::uint8_t>(animal->intOr("inventory w", 4));
	inventory.gridH = static_cast<std::uint8_t>(animal->intOr("inventory h", 8));
	Entry worn = makeEntry(backpack, 1);
	worn.slot = state::Inventory::kBackpackSlot;
	worn.section = Entry::kEquipped;
	inventory.items.push_back(std::move(worn));

	// The spawn fill: the squad template's `vendors` lists (each list's `items count` draws, default 40), else the animal's own `inventory`
	// list (UNSURE: no record in the data has one), else empty.
	state::Inventory bag;
	bag.gridW = static_cast<std::uint8_t>(std::max(0, size.w));
	bag.gridH = static_cast<std::uint8_t>(std::max(0, size.h));
	const std::vector<Listed> lists = vendorLists(data, squadTemplate);
	if (size.w > 0 && size.h > 0)
	{
		std::vector<Piece> pieces;
		if (!lists.empty())
		{
			RngStream dice(refreshSeed(seed, id->value, 0));
			// The faction's grade weights are not needed for the fill's order of magnitude, but the quality rolls want them: the squad's
			// faction is not at hand here, so the no-faction defaults apply (UNSURE).
			double weights6[6];
			gradeWeights(nullptr, weights6);
			for (const Listed& listed : lists)
				drawFrom(pieces, data, rules, *listed.record, listed.record->intOr("items count", 40), weights6,
					squadTemplate.intOr("crossbow levels", 1), squadTemplate.intOr("robotics levels", 1), dice);
			priceStock(pieces, book);
		} else if (const kdata::RefCategory* own = animal->findRefs("inventory")) {
			for (const kdata::RefEntry& entry : own->entries)
			{
				if (!entry.opaqueEntry && data.find(entry.targetID) != nullptr && entry.v0 > 0)
					pieces.push_back({makeEntry(entry.targetID, static_cast<std::uint32_t>(entry.v0)), 0.0});
			}
		}
		placeStock(bag, rules, std::move(pieces));
	}
	inventory.wornBackpack()->contents = std::move(bag.items);
	grid::settle(inventory, rules, id->value);
	member.set<state::Inventory>(std::move(inventory));
	return 1;
}

} // namespace ksim::trader

namespace ksim {

std::uint32_t Simulation::seedPackAnimals()
{
	packsDirty_ = false;
	if (gameData_ == nullptr)
		return 0;
	flecs::world& ecs = ecsMutable();
	std::unordered_map<std::string, bool> packSquad; // per template
	std::vector<std::pair<flecs::entity, const kdata::Record*>> squads;
	ecs.each([&](flecs::entity entity, const state::SquadInstance& squad)
	{
		const auto found = packSquad.try_emplace(squad.templateSid, false);
		const kdata::Record* record = gameData_->find(squad.templateSid);
		if (found.second)
			found.first->second = record != nullptr && trader::hasPackAnimals(*gameData_, *record);
		if (found.first->second)
			squads.emplace_back(entity, record);
	});
	if (squads.empty())
		return 0;
	const grid::Rules rules(*gameData_);
	std::uint32_t given = 0;
	for (const auto& [squad, record] : squads)
	{
		std::vector<flecs::entity> members;
		ecs.query_builder<const state::CharacterInstance>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::CharacterInstance&)
		{
			if (member.get<state::Inventory>() == nullptr)
				members.push_back(member);
		});
		for (const flecs::entity member : members)
			given += trader::seedPackAnimal(member, *gameData_, tradeBook(), rules, seed_, *record);
	}
	return given;
}

void Simulation::seedTraders()
{
	if (gameData_ == nullptr)
		return;
	seedPackAnimals(); // before the forced refresh, which works on the backpacks
	flecs::world& ecs = ecsMutable();
	std::vector<flecs::entity> merchants;
	ecs.each([&](flecs::entity entity, const state::SquadInstance& squad)
	{
		const kdata::Record* record = gameData_->find(squad.templateSid);
		if (record != nullptr && entity.get<state::TraderStock>() == nullptr && trader::isMerchant(*record))
			merchants.push_back(entity);
	});
	if (merchants.empty())
		return;
	const grid::Rules rules(*gameData_);
	const double now = trader::gameHours(clockTick());
	for (const flecs::entity merchant : merchants)
	{
		const trader::Outcome out = trader::refresh(merchant, *gameData_, tradeBook(), rules, seed_, now, true);
		++traderStats_.forced;
		traderStats_.placed += out.placed;
		traderStats_.overflowDestroyed += out.destroyed;
	}
}

void Simulation::stepTraders()
{
	if (gameData_ == nullptr || clockTick() % trader::kIntervalTicks != 0)
		return;
	if (packsDirty_)
		seedPackAnimals(); // a squad was added (a respawned caravan): its pack animals get their backpacks
	flecs::world& ecs = ecsMutable();
	const double now = trader::gameHours(clockTick());
	// TQ5 default: loaded = full detail, whether or not a player is in the shop. A squad at any other level keeps its timer and its
	// stock, so a long absence is one refresh on return.
	std::vector<std::pair<flecs::entity, bool>> due;
	ecs.each([&](flecs::entity entity, const state::TraderStock& stock, const state::SquadInstance&, const Detail& detail)
	{
		if (detail.level == DetailLevel::Full && (!stock.seeded || now > stock.nextRefresh))
			due.emplace_back(entity, !stock.seeded);
	});
	if (due.empty())
		return;
	// Held while a player character is inside the shop building (kinspect item 54, live: a timer 0.65 h overdue did not fire until the
	// player left; TQ2 / TQ5 answered). The shop is the building the squad's members call home (Fighter::homeBuilding); a merchant with
	// no such building (a wandering trader, or one that has no fighters yet) has no hold.
	std::set<std::uint64_t> occupied;
	if (combat_ != nullptr)
	{
		for (const auto& member : combat_->members())
		{
			if (member.fighter.playerControlled && !member.fighter.dead && member.fighter.building != 0)
				occupied.insert(member.fighter.building);
		}
	}
	const grid::Rules rules(*gameData_);
	for (const auto& [merchant, force] : due)
	{
		// item 54 blocker 3: a squad with no members is not refreshed. Blockers 4-6 are the merchant test and the player-owned
		// squads having no SquadInstance; 9-11 (building type, child building, no shop containers) have no state to read (UNSURE).
		bool members = false;
		bool held = false;
		ecs.query_builder<const state::Id>().with<state::MemberOf>(merchant).build().each([&](const state::Id& member)
		{
			members = true;
			const state::Fighter* fighter = combat_ != nullptr ? combat_->find(member.value) : nullptr;
			held = held || (fighter != nullptr && fighter->homeBuilding != 0 && occupied.count(fighter->homeBuilding) != 0);
		});
		if (!members || held)
			continue; // the timer is NOT re-armed: it retries at the next check and fires once the hold clears
		const trader::Outcome out = trader::refresh(merchant, *gameData_, tradeBook(), rules, seed_, now, force);
		++(force ? traderStats_.forced : traderStats_.refreshes);
		traderStats_.placed += out.placed;
		traderStats_.overflowDestroyed += out.destroyed;
	}
}

} // namespace ksim
