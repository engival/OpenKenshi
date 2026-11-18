#include "ksim/trade.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "ksim/combat.hpp"
#include "ksim/economy.hpp"
#include "ksim/economy_script.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/rng.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ksim::trade {

namespace {

constexpr int kMaxIngredientDepth = 16;
constexpr float kUniformPriceMult = 0.3f; // 142133ecc, faction-uniform armour (s72 section 4)

std::uint64_t fnv1a(const std::string& text) noexcept
{
	std::uint64_t h = 1469598103934665603ull;
	for (const unsigned char c : text)
		h = (h ^ c) * 1099511628211ull;
	return h;
}

// The exe works in 32-bit floats; so does the chain.
std::int32_t truncate(float value) noexcept
{
	return static_cast<std::int32_t>(value);
}

const kdata::Record* recordByName(const kdata::GameData& data, std::int32_t type, std::string_view name)
{
	for (const kdata::Record* record : data.byType(type))
	{
		if (record->name == name)
			return record;
	}
	return nullptr;
}

} // namespace

Book::Book(const kdata::GameData& data, std::uint64_t seed, economy::Hooks* hooks)
	: data_(data)
	, seed_(seed)
	, hooks_(hooks)
	, weaponType_(kworld::itemTypeByName("WEAPON"))
	, armourType_(kworld::itemTypeByName("ARMOUR"))
	, containerType_(kworld::itemTypeByName("CONTAINER"))
	, crossbowType_(kworld::itemTypeByName("CROSSBOW"))
	, limbType_(kworld::itemTypeByName("LIMB_REPLACEMENT"))
	, itemType_(kworld::itemTypeByName("ITEM"))
{
	if (const kdata::Record* c = recordByName(data, kworld::itemTypeByName("CONSTANTS"), "GLOBAL CONSTANTS"))
	{
		k_.global = c->floatOr("global price mult", static_cast<float>(k_.global));
		k_.trade = c->floatOr("trade price mult", static_cast<float>(k_.trade));
		k_.food = c->floatOr("food price mult", static_cast<float>(k_.food));
		k_.sword = c->floatOr("sword price mult", static_cast<float>(k_.sword));
		k_.armour = c->floatOr("armor price mult", static_cast<float>(k_.armour));
		k_.clothing = c->floatOr("clothing price mult", static_cast<float>(k_.clothing));
		k_.crossbow = c->floatOr("crossbow price mult", static_cast<float>(k_.crossbow));
		k_.robotics = c->floatOr("robotics price mult", static_cast<float>(k_.robotics));
		k_.loot = c->floatOr("loot price mult", static_cast<float>(k_.loot));
		k_.lootGear = c->floatOr("loot price mult GEAR", static_cast<float>(k_.lootGear));
		k_.margin = c->floatOr("trade profit margins", static_cast<float>(k_.margin));
	}
	// Town::vf71: only the VENDOR_LIST "all trade goods" items get a factor.
	if (const kdata::Record* list = recordByName(data, kworld::itemTypeByName("VENDOR_LIST"), "all trade goods"))
	{
		if (const kdata::RefCategory* items = list->findRefs("items"))
		{
			for (const kdata::RefEntry& entry : items->entries)
			{
				if (!entry.opaqueEntry)
					goods_.insert(entry.targetID);
			}
		}
	}
	// ItemsCulture::load (FUN_1407fd9a0), run on every ITEMS_CULTURE record and on the FACTION record itself (a faction is its own first culture).
	const auto load = [&](const kdata::Record& record)
	{
		if (const kdata::RefCategory* listed = record.findRefs("trade prices"))
		{
			std::map<std::string, double>& map = cultures_[record.stringID];
			for (const kdata::RefEntry& entry : listed->entries)
			{
				if (!entry.opaqueEntry)
					map[entry.targetID] = static_cast<double>(static_cast<float>(entry.v0 == 0 ? 100 : entry.v0) * 0.01f);
			}
		}
		if (const kdata::RefCategory* listed = record.findRefs("illegal goods"))
		{
			std::set<std::string>& set = illegalGoods_[record.stringID];
			for (const kdata::RefEntry& entry : listed->entries)
			{
				if (!entry.opaqueEntry)
					set.insert(entry.targetID);
			}
		}
	};
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("ITEMS_CULTURE")))
		load(*record);
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("FACTION")))
		load(*record);
}

Terms Book::termsOf(flecs::entity squad) const
{
	Terms terms;
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	if (instance == nullptr)
		return terms;
	if (const kdata::Record* record = data_.find(instance->templateSid))
	{
		terms.buyMult = record->floatOr("buy mult", 1.0f);
		terms.buysIllegal = record->boolOr("buys illegal", false);
	}
	flecs::world world = squad.world();
	if (instance->homeTown.id != 0)
	{
		const flecs::entity town(world.c_ptr(), instance->homeTown.id);
		if (const state::Id* id = town.is_valid() ? town.get<state::Id>() : nullptr)
			terms.townId = id->value;
		if (const state::TownState* state = town.is_valid() ? town.get<state::TownState>() : nullptr)
			terms.town = data_.find(state->activeSid.empty() ? state->rootSid : state->activeSid);
	}
	if (instance->faction.id != 0)
	{
		const flecs::entity faction(world.c_ptr(), instance->faction.id);
		if (const state::FactionState* state = faction.is_valid() ? faction.get<state::FactionState>() : nullptr)
			terms.faction = data_.find(state->templateSid);
	}
	// The T4 overlay (T4d): the town the trader's stock and prices read is its market town (the home town, else a wandering trader's
	// current stay). No TownStock (original mode) leaves the pointer null and the chain untouched.
	const flecs::entity market = trader::marketTown(squad);
	if (market.is_valid())
		terms.stock = market.get<state::TownStock>();
	terms.hooks = hooks_;
	return terms;
}

double Book::townFactor(std::uint64_t townId, const std::string& itemSid) const
{
	if (townId == 0 || !isTradeGood(itemSid))
		return 1.0;
	RngStream dice(splitMix64(streamSeed(seed_, kPriceStream) ^ (townId * 0x9E3779B97F4A7C15ull) ^ fnv1a(itemSid)));
	const float margin = static_cast<float>(k_.margin);
	const float low = 1.0f - margin;
	return static_cast<double>(low + static_cast<float>(dice.uniform01()) * ((1.0f + margin) - low));
}

std::int32_t Book::priceOf(const kdata::Record& item, int depth) const
{
	const float global = static_cast<float>(k_.global);
	if (item.type == weaponType_ || item.type == armourType_ || item.type == crossbowType_)
		return gearPrice(item, std::string(), 0);
	// UNSURE: LIMB_REPLACEMENT (0x6f) is not traced beyond its constant (`crossbow price mult`, s72 section 2b); `value` x that.
	if (item.type == limbType_)
		return truncate(static_cast<float>(item.intOr("value", 0)) * static_cast<float>(k_.crossbow) * global);
	if (item.type == containerType_)
		return item.intOr("value", 0);
	const kdata::RefCategory* ingredients = item.findRefs("ingredients");
	if (ingredients == nullptr || ingredients->entries.empty())
	{
		const int value = item.intOr("value", 0);
		if (item.boolOr("artifact", false))
			return value;
		const float mult = static_cast<float>(item.boolOr("food crop", false) ? k_.food : k_.trade) * global;
		return truncate(std::floor(static_cast<float>(value) * mult + 0.5f));
	}
	if (depth >= kMaxIngredientDepth)
		return 0;
	std::int32_t total = 0;
	const float profit = item.floatOr("profitability", 0.2f);
	for (const kdata::RefEntry& entry : ingredients->entries)
	{
		const kdata::Record* part = entry.opaqueEntry ? nullptr : data_.find(entry.targetID);
		if (part == nullptr)
			continue;
		const float sub = static_cast<float>(priceOf(*part, depth + 1));
		const float term = std::floor(sub * (static_cast<float>(entry.v0) * 0.01f) * (profit + 1.0f) + 0.5f);
		total = truncate(term + static_cast<float>(total));
	}
	return total;
}

// item+0x1f0, the exe's record pricer FUN_140896e20 (kinspect findings/s72_static.md section 2). Float32 throughout, int() where the exe truncates.
std::int32_t Book::gearPrice(const kdata::Record& item, const std::string& maker, int level) const
{
	const float global = static_cast<float>(k_.global);
	const float t = static_cast<float>(level) * 0.01f;
	if (item.type == weaponType_)
	{
		// 2a. int(int(L(level) x int(value x maker `price mod`)) x SWORDMULT), L the logistic lerp 15 -> 160.
		float v = static_cast<float>(item.intOr("value", 0));
		if (!maker.empty())
		{
			// UNSURE: a maker sid that is not in the data prices as "no maker" (the exe always has one).
			if (const kdata::Record* record = data_.find(maker))
				v = static_cast<float>(truncate(v * record->floatOr("price mod", 1.0f)));
		}
		const float x = ((2.0f * t - 1.0f) * 5.0f) * 0.8f;
		const float s = 1.0f / (1.0f + std::exp(-x));
		const float L = (160.0f - 15.0f) * s + 15.0f; // UNSURE [L]: MSVC expf may differ from std::exp by 1 ulp, which can move a result by 1
		const float p = static_cast<float>(truncate(L * v));
		return truncate(p * (static_cast<float>(k_.sword) * global));
	}
	if (item.type == crossbowType_)
	{
		// 2b. int(int((value 1 - value) x t^2 + value) x mult); a spawned crossbow has level 0 = its `value`. The exe reads the "robotics
		// price mult" slot here (the names look swapped; both 1.0 in the data).
		const float value = static_cast<float>(item.intOr("value", 0));
		const float p = static_cast<float>(truncate((static_cast<float>(item.intOr("value 1", 0)) - value) * (t * t) + value));
		return truncate(p * (static_cast<float>(k_.robotics) * global));
	}
	if (item.type != armourType_)
		return 0;
	// 2c. Armour. The class / material tables are hard-coded in the exe (ArmourStats 142134ce0..da0, s72 section 2c).
	static constexpr float kClassLo[4] = {100.0f, 400.0f, 800.0f, 400.0f};
	static constexpr float kClassHi[4] = {5000.0f, 30000.0f, 35000.0f, 40000.0f};
	static constexpr float kMaterialLo[4] = {0.2f, 0.5f, 1.0f, 1.5f};
	static constexpr float kMaterialHi[4] = {0.8f, 1.0f, 1.5f, 1.5f};
	const kdata::RefCategory* coverage = item.findRefs("part coverage");
	const bool covered = coverage != nullptr && !coverage->entries.empty();
	std::int32_t base = item.intOr("value", 0);
	if (level > 0 && covered)
	{
		const int cls = std::clamp(item.intOr("class", 0), 0, 3);
		const int mat = std::clamp(item.intOr("material type", 0), 0, 3);
		const float lo = kClassLo[cls] * kMaterialLo[mat];
		const float hi = kClassHi[cls] * kMaterialHi[mat];
		base = truncate((hi - lo) * (t * t) + lo); // the record `value` is ignored here
	}
	base = truncate(static_cast<float>(base) * (static_cast<float>(covered ? k_.armour : k_.clothing) * global));
	base = truncate(static_cast<float>(base) * item.floatOr("relative price mult", 1.0f));
	switch (item.intOr("slot", 0))
	{
	case 3: base = truncate(static_cast<float>(base) * 0.3f); break;
	case 6: base = truncate(static_cast<float>(base) * 0.5f); break;
	case 8: base = truncate(static_cast<float>(base) * 1.5f); break;
	case 9: base = truncate(static_cast<float>(base) * 0.2f); break;
	default: break; // 5 (body armour): x1.0
	}
	return base;
}

std::int32_t Book::basePrice(const kdata::Record& item) const
{
	return priceOf(item, 0);
}

double Book::cultureMult(const Terms& terms, const std::string& itemSid) const
{
	const auto find = [&](const std::string& culture, double& out)
	{
		const auto map = cultures_.find(culture);
		if (map == cultures_.end())
			return false;
		const auto at = map->second.find(itemSid);
		if (at == map->second.end())
			return false;
		out = at->second;
		return true;
	};
	double mult = 1.0;
	// The town's own ITEMS_CULTURE list overrides the faction's per item; later entries overwrite earlier ones.
	if (terms.town != nullptr)
	{
		if (const kdata::RefCategory* listed = terms.town->findRefs("trade culture"))
		{
			for (auto entry = listed->entries.rbegin(); entry != listed->entries.rend(); ++entry)
			{
				if (find(entry->targetID, mult))
					return mult;
			}
		}
	}
	if (terms.faction != nullptr)
	{
		if (const kdata::RefCategory* listed = terms.faction->findRefs("trade culture"))
		{
			for (auto entry = listed->entries.rbegin(); entry != listed->entries.rend(); ++entry)
			{
				if (find(entry->targetID, mult))
					return mult;
			}
		}
		if (find(terms.faction->stringID, mult))
			return mult;
	}
	return 1.0;
}

bool Book::illegal(const Terms& terms, const std::string& itemSid) const
{
	const auto listed = [&](const kdata::Record* owner)
	{
		if (owner == nullptr)
			return false;
		if (const kdata::RefCategory* cultures = owner->findRefs("trade culture"))
		{
			for (const kdata::RefEntry& entry : cultures->entries)
			{
				const auto set = illegalGoods_.find(entry.targetID);
				if (set != illegalGoods_.end() && set->second.count(itemSid) != 0)
					return true;
			}
		}
		const auto own = illegalGoods_.find(owner->stringID);
		return own != illegalGoods_.end() && own->second.count(itemSid) != 0;
	};
	return listed(terms.town) || listed(terms.faction);
}

Quote Book::quote(const Terms& terms, const Entry& entry) const
{
	Quote quote;
	const kdata::Record* record = data_.find(entry.itemSid);
	if (record == nullptr)
	{
		quote.refusal = Refusal::NotForSale;
		return quote;
	}
	const bool gear = record->type == weaponType_ || record->type == armourType_ || record->type == crossbowType_;
	const float extra = static_cast<float>(terms.buyMult) * static_cast<float>(cultureMult(terms, entry.itemSid));
	if (gear)
	{
		// s72 section 4b: weapons and crossbows are Value int(E x F), Sell int(E x F x GEAR); armour int(E x (F x GEAR)). No town factor, no
		// `trade item` test. A weapon is priced at its maker and level (the generated quality, percent); armour and crossbows at level 0
		// (spawned gear; the exe's crafted tiers are not modelled). Faction-uniform armour x0.3 on both (s72 section 4: Value int(E x F x 0.3),
		// Sell int(E x F x GEAR x 0.3)). UNSURE: no crafter mult (`loot price mult player weapons / armour`: all 0.25 = GEAR in the merged data).
		const bool weapon = record->type == weaponType_;
		const bool made = weapon && entry.quality.set() && !entry.quality.maker.empty();
		const std::int32_t F = gearPrice(*record, made ? entry.quality.maker : std::string(), made ? static_cast<int>(std::lround(entry.quality.level * 100.0)) : 0);
		const float lootGear = static_cast<float>(k_.lootGear);
		const float uniform = record->type == armourType_ && !entry.quality.uniform.empty() ? kUniformPriceMult : 1.0f;
		quote.buy = truncate(extra * (static_cast<float>(F) * uniform));
		quote.sell = record->type == armourType_ ? truncate(extra * (static_cast<float>(F) * lootGear * uniform)) : truncate(extra * static_cast<float>(F) * lootGear);
	} else {
		std::int32_t price = truncate(static_cast<float>(townFactor(terms.townId, entry.itemSid)) * static_cast<float>(basePrice(*record)));
		quote.buy = truncate(extra * static_cast<float>(price));
		// UNSURE: a `trade item` skips the loot mult only when not stolen; stolen goods are not modelled, so a trade item never gets it.
		std::int32_t sold = price;
		if (!record->boolOr("trade item", false))
			sold = truncate(static_cast<float>(sold) * static_cast<float>(k_.loot));
		quote.sell = truncate(extra * static_cast<float>(sold));
	}
	// T4d (TOWN_LIFE_SLICE 7.4): in economy mode the market town's own stock moves the price on top of the chain — scarce dearer, a glut
	// cheaper — and both sides take the same factor, so the spread holds. A null stock (original mode, no town, a hand-built Terms) is
	// the chain exactly as it was, bit for bit.
	if (terms.stock != nullptr)
	{
		// T4e: the script's economy.price when one answers, else the engine's stockFactor (economy_script.hpp).
		const double factor = ksim::economy::priceFactor(terms.hooks, terms.townId, *terms.stock, entry.itemSid);
		quote.buy = static_cast<std::int32_t>(static_cast<double>(quote.buy) * factor + 0.5);
		if (quote.sell > 0)
			quote.sell = static_cast<std::int32_t>(static_cast<double>(quote.sell) * factor + 0.5);
	}
	if (quote.buy <= 0 || quote.sell <= 0)
	{
		quote.sell = -1;
		quote.refusal = Refusal::NotForSale;
	} else if (terms.buysIllegal && !illegal(terms, entry.itemSid)) {
		quote.sell = -1;
		quote.refusal = Refusal::IllegalOnly;
	}
	return quote;
}

flecs::entity squadOf(flecs::entity member)
{
	if (!member.is_valid())
		return flecs::entity();
	return member.target<state::MemberOf>();
}

} // namespace ksim::trade

// ---- the simulation side: the stock as storage, the parties and the terms of an order 68, the TRADE frame's prices ----

namespace ksim {

namespace {

// The wire's entry list (simlink.cpp flatEntries): `items`, then the worn backpack's contents.
std::vector<const state::Inventory::Entry*> flatOf(const state::Inventory& inventory)
{
	std::vector<const state::Inventory::Entry*> flat;
	for (const state::Inventory::Entry& entry : inventory.items)
		flat.push_back(&entry);
	if (const state::Inventory::Entry* bag = inventory.wornBackpack())
	{
		for (const state::Inventory::Entry& entry : bag->contents)
			flat.push_back(&entry);
	}
	return flat;
}

// The faction a player character belongs to: the player faction whose template is its Allegiance, else (a scenario without allegiance) the first.
flecs::entity playerFactionOf(const flecs::world& ecs, flecs::entity character)
{
	const state::Allegiance* allegiance = character.get<state::Allegiance>();
	flecs::entity found;
	flecs::entity first;
	ecs.each([&](flecs::entity entity, const state::PlayerFaction&, const state::FactionState& faction)
	{
		if (!first.is_valid())
			first = entity;
		if (allegiance != nullptr && faction.templateSid == allegiance->factionSid && !found.is_valid())
			found = entity;
	});
	return found.is_valid() ? found : first;
}

} // namespace

const trade::Book& Simulation::tradeBook() const
{
	if (tradeBook_ == nullptr || tradeBookSeed_ != seed_)
	{
		tradeBook_ = std::make_shared<const trade::Book>(*gameData_, seed_, economyHooks_.get());
		tradeBookSeed_ = seed_;
	}
	return *tradeBook_;
}

std::uint64_t Simulation::traderSquad(std::uint64_t character) const
{
	if (combat_ == nullptr || combat_->find(character) == nullptr)
		return 0;
	const flecs::entity squad = trade::squadOf(fighterEntity(character));
	if (!squad.is_valid() || squad.get<state::TraderStock>() == nullptr || squad.get<state::Inventory>() == nullptr)
		return 0;
	const state::Id* id = squad.get<state::Id>();
	return id != nullptr ? id->value : 0;
}

std::string Simulation::tradeLost(std::uint64_t actor, std::uint64_t trader) const
{
	const state::Fighter* who = combat_ != nullptr ? combat_->find(trader) : nullptr;
	const state::Fighter* me = combat_ != nullptr ? combat_->find(actor) : nullptr;
	// MQ11: only a party that is gone closes the window; a KO or a death removes nothing in the exe. A sleeping keeper stays "gone" (a
	// shop that is shut for the night does not trade: ksim_town_beds pins it; UNSURE against the exe).
	if (who == nullptr || me == nullptr || who->asleep)
		return "gone";
	return traderSquad(trader) == 0 ? "gone" : std::string();
}

const Simulation::BuybackLine* Simulation::buybackLine(flecs::entity_t payer, flecs::entity_t squad, std::uint64_t window,
	const state::InventoryEntry& kind) const
{
	const auto list = buyback_.find({payer, squad});
	if (list == buyback_.end() || list->second.window != window)
		return nullptr;
	for (const BuybackLine& line : list->second.lines)
	{
		if (line.count > 0 && line.kind.sameKind(kind))
			return &line;
	}
	return nullptr;
}

bool Simulation::tradeView(std::uint64_t actor, std::uint64_t trader, std::uint64_t window, TradeView& out) const
{
	if (gameData_ == nullptr || !tradeLost(actor, trader).empty())
		return false;
	const flecs::entity squad = trade::squadOf(fighterEntity(trader));
	const state::TraderStock* wallet = squad.get<state::TraderStock>();
	const state::Inventory* held = squad.get<state::Inventory>();
	const state::Inventory* mine = fighterEntity(actor).get<state::Inventory>();
	if (squad.get<state::Id>() == nullptr || wallet == nullptr || held == nullptr || mine == nullptr)
		return false;
	out = TradeView();
	out.squad = squad.get<state::Id>()->value;
	out.templateSid = squad.get<state::SquadInstance>()->templateSid;
	out.traderMoney = wallet->money;
	const trade::Book& book = tradeBook();
	const trade::Terms terms = book.termsOf(squad);
	const state::Inventory stock = settledInventory(out.squad);
	const state::Inventory pack = settledInventory(actor);
	out.stockVersion = stock.version;
	out.actorVersion = pack.version;
	const auto stockFlat = flatOf(stock);
	for (std::size_t i = 0; i < stockFlat.size(); ++i)
	{
		const trade::Quote quote = book.quote(terms, *stockFlat[i]);
		out.stock.push_back({static_cast<std::uint16_t>(i), quote.buy, quote.sell});
	}
	// The buyback tail: only the selling faction's window sees its list (the list is keyed by the faction).
	if (const flecs::entity payer = playerFactionOf(world_.ecs(), fighterEntity(actor)); payer.is_valid())
	{
		std::vector<const state::Inventory::Entry*> counted; // one tail entry per list line: the first stock entry of its kind
		for (std::size_t i = 0; i < stockFlat.size(); ++i)
		{
			const BuybackLine* line = buybackLine(payer.id(), squad.id(), window, *stockFlat[i]);
			if (line == nullptr || std::find_if(counted.begin(), counted.end(), [&](const state::Inventory::Entry* e) { return e->sameKind(*stockFlat[i]); }) != counted.end())
				continue;
			counted.push_back(stockFlat[i]);
			out.buyback.push_back({static_cast<std::uint16_t>(i), std::min(line->count, stockFlat[i]->count), static_cast<std::int32_t>(line->price)});
		}
	}
	const auto packFlat = flatOf(pack);
	for (std::size_t i = 0; i < packFlat.size(); ++i)
	{
		const bool carried = packFlat[i]->slot == state::Inventory::kCarried;
		out.actor.emplace_back(static_cast<std::uint16_t>(i), carried ? book.quote(terms, *packFlat[i]).sell : -1);
	}
	// The chests (MQ12): every container the actor's faction owns that order 68's `source` accepts, sorted by id.
	if (const state::Fighter* him = combat_->find(trader); him != nullptr && playerFactionOf(world_.ecs(), fighterEntity(actor)).is_valid())
	{
		world_.ecs().each([&](const state::Container&, const state::Id& id)
		{
			if (tradeSourceFault(actor, id.value, *him).empty())
				out.chests.push_back({id.value, 0, {}});
		});
		std::sort(out.chests.begin(), out.chests.end(), [](const TradeView::ChestPrices& a, const TradeView::ChestPrices& b) { return a.container < b.container; });
		for (TradeView::ChestPrices& chest : out.chests)
		{
			const state::Inventory box = settledInventory(chest.container);
			chest.version = box.version;
			const auto boxFlat = flatOf(box);
			for (std::size_t i = 0; i < boxFlat.size(); ++i)
				chest.prices.emplace_back(static_cast<std::uint16_t>(i), book.quote(terms, *boxFlat[i]).sell);
		}
	}
	return true;
}

std::string Simulation::tradeSourceFault(std::uint64_t actor, std::uint64_t source, const state::Fighter& trader) const
{
	const Storage box = findStorage(source);
	if (box.entity == 0 || box.body || box.producer || box.trader || box.ownerSid.empty() || isForeignOwner(world_.ecs(), actor, box.ownerSid))
		return "not_allowed";
	const double dx = box.x - trader.x;
	const double dz = box.z - trader.z;
	return dx * dx + dz * dz >= trade::kRange * trade::kRange ? "out_of_range" : std::string();
}

std::string Simulation::tradeParties(const InventoryOrder& order, const Storage& stock) const
{
	if (gameData_ == nullptr || combat_ == nullptr)
		return "bad_target";
	const state::Fighter* me = combat_->find(order.id);
	const state::Fighter* who = order.trader != 0 ? combat_->find(order.trader) : nullptr;
	if (me == nullptr || who == nullptr)
		return "not_allowed"; // no open trade: the Session fills `trader` from its window
	if (me->dead || me->unconscious)
		return "not_allowed";
	const flecs::entity squad = trade::squadOf(fighterEntity(order.trader));
	if (!squad.is_valid() || squad.id() != stock.entity || who->dead || who->unconscious)
		return "not_allowed";
	if (!playerFactionOf(world_.ecs(), fighterEntity(order.id)).is_valid())
		return "not_allowed"; // only a player faction trades (the money is its)
	// MQ12: the goods may come from a container of the actor's own faction; it is then the range owner in the actor's place.
	const double fromX = me->x;
	const double fromZ = me->z;
	if (order.source != 0)
		return tradeSourceFault(order.id, order.source, *who);
	// MQ8: the squared XZ distance between the two owners, d^2 < 1e6.
	const double dx = fromX - who->x;
	const double dz = fromZ - who->z;
	if (dx * dx + dz * dz >= trade::kRange * trade::kRange)
		return "out_of_range";
	return {};
}

std::string Simulation::tradeTerms(const InventoryOrder& order, const Storage& stock, const state::InventoryEntry& entry, bool buying,
	std::uint32_t& count, Deal& deal) const
{
	const flecs::entity squad(world_.ecs().c_ptr(), stock.entity);
	const flecs::entity payer = playerFactionOf(world_.ecs(), fighterEntity(order.id));
	const trade::Book& book = tradeBook();
	const trade::Quote quote = book.quote(book.termsOf(squad), entry);
	const std::int64_t unit = buying ? quote.buy : quote.sell;
	if (unit <= 0)
		return !buying && quote.refusal == trade::Refusal::IllegalOnly ? "illegal_only" : "not_for_sale";
	const std::int64_t wallet = buying ? payer.get<state::PlayerFaction>()->money : squad.get<state::TraderStock>()->money;
	// A buy of units this faction sold in this window comes back at the price the merchant paid (the buyback list), the rest at the buy price.
	const BuybackLine* back = buying ? buybackLine(payer.id(), squad.id(), order.window, entry) : nullptr;
	const std::int64_t backPrice = back != nullptr ? back->price : 0;
	const std::uint32_t listable = back != nullptr ? std::min<std::uint32_t>({back->count, entry.count, count}) : 0;
	const std::uint32_t listed = backPrice > 0 ? static_cast<std::uint32_t>(std::min<std::int64_t>(listable, wallet / backPrice)) : 0;
	// The listed units go first; the rest at the buy price only once every listed unit that was asked for is paid.
	const std::int64_t extra = listed == listable ? std::min<std::int64_t>(count - listed, (wallet - backPrice * listed) / unit) : 0;
	if (listed + extra <= 0)
		return buying ? "cant_afford" : "shop_cant_afford";
	count = static_cast<std::uint32_t>(listed + extra);
	deal.amount = backPrice * listed + unit * extra;
	deal.buybackUnits = listed;
	deal.units = count;
	deal.buying = buying;
	deal.payer = payer.id();
	deal.squad = squad.id();
	deal.window = order.window;
	deal.kind = entry;
	deal.kind.contents.clear();
	deal.kind.planted.clear();
	if (!buying)
		deal.kind.stolen.clear(); // clearReturnedMark clears it on the way into the stock
	return {};
}

void Simulation::tradeSettle(const Deal& deal)
{
	flecs::world& ecs = ecsMutable();
	const flecs::entity payer = ecs.entity(deal.payer);
	const flecs::entity squad = ecs.entity(deal.squad);
	state::PlayerFaction faction = *payer.get<state::PlayerFaction>();
	state::TraderStock wallet = *squad.get<state::TraderStock>();
	const std::int64_t signedAmount = deal.buying ? -deal.amount : deal.amount;
	faction.money += signedAmount;
	wallet.money = static_cast<std::int32_t>(std::clamp<std::int64_t>(static_cast<std::int64_t>(wallet.money) - signedAmount,
		std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
	payer.set<state::PlayerFaction>(std::move(faction));
	squad.set<state::TraderStock>(std::move(wallet));
	// The buyback list: a sale lists its units at the price the merchant paid; a buy takes back the units it bought from the list.
	BuybackList& list = buyback_[{deal.payer, deal.squad}];
	if (list.window != deal.window)
		list = BuybackList{deal.window, {}}; // another window: the earlier one closed
	auto line = std::find_if(list.lines.begin(), list.lines.end(), [&](const BuybackLine& l) { return l.kind.sameKind(deal.kind); });
	if (deal.buying)
	{
		if (line != list.lines.end())
		{
			line->count -= std::min(line->count, deal.buybackUnits);
			if (line->count == 0)
				list.lines.erase(line);
		}
	} else if (deal.units > 0) {
		if (line == list.lines.end())
			line = list.lines.insert(list.lines.end(), BuybackLine{deal.kind, 0, 0});
		line->count += deal.units;
		line->price = deal.amount / deal.units;
	}
}

} // namespace ksim
