// The real-economy overlay's seed and tick (TOWN_LIFE_SLICE 7.2-7.5, T4a-T4b). The rules are the pure functions in economy.hpp; this
// file reads the world (towns, their merchant squads, the placed camps, the data) and runs them once a game hour.

#include "ksim/economy/economy.hpp"
#include "ksim/economy/economy_script.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/world/region.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/squad_alive.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/world/world.hpp"

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace ksim::economy {

double cap(const state::StockLine& line) noexcept
{
	return line.target > 0.0 ? line.target * kCapMultiple : 0.0;
}

double fill(const state::StockLine& line) noexcept
{
	if (line.target <= 0.0)
		return 1.0;
	return std::clamp(line.amount / line.target, 0.0, 1.0);
}

double prosperity(const state::TownStock& stock) noexcept
{
	if (stock.lines.empty())
		return 1.0;
	double total = 0.0;
	for (const state::StockLine& line : stock.lines)
		total += fill(line);
	return total / static_cast<double>(stock.lines.size());
}

state::StockLine* findLine(state::TownStock& stock, std::string_view itemSid) noexcept
{
	const auto it = std::lower_bound(stock.lines.begin(), stock.lines.end(), itemSid,
		[](const state::StockLine& line, std::string_view sid) { return line.itemSid < sid; });
	return it != stock.lines.end() && it->itemSid == itemSid ? &*it : nullptr;
}

const state::StockLine* findLine(const state::TownStock& stock, std::string_view itemSid) noexcept
{
	const auto it = std::lower_bound(stock.lines.begin(), stock.lines.end(), itemSid,
		[](const state::StockLine& line, std::string_view sid) { return line.itemSid < sid; });
	return it != stock.lines.end() && it->itemSid == itemSid ? &*it : nullptr;
}

state::StockLine& lineFor(state::TownStock& stock, std::string_view itemSid)
{
	if (state::StockLine* line = findLine(stock, itemSid))
		return *line;
	const auto at = std::lower_bound(stock.lines.begin(), stock.lines.end(), itemSid,
		[](const state::StockLine& line, std::string_view sid) { return line.itemSid < sid; });
	return *stock.lines.insert(at, state::StockLine{std::string(itemSid), 0.0, kDefaultTarget});
}

Flow flow(const state::TownStock& stock, const state::EconomyProducer& producer, double dt) noexcept
{
	Flow out;
	if (dt <= 0.0 || producer.ratePerHour <= 0.0)
		return out;
	// The output must have room: a full town makes nothing more and consumes nothing (7.5's clamp with no dead input).
	const state::StockLine* output = findLine(stock, producer.itemSid);
	const state::StockLine absent{producer.itemSid, 0.0, kDefaultTarget}; // lineFor() would create it at these
	const double room = cap(output != nullptr ? *output : absent) - (output != nullptr ? output->amount : 0.0);
	if (room <= 0.0)
		return out;
	// Inputs: the producer runs at the lowest utilisation its consumptions allow.
	for (const state::ConsumeLine& input : producer.consumes)
	{
		if (input.perHour <= 0.0)
			continue;
		const state::StockLine* line = findLine(stock, input.itemSid);
		const double available = line != nullptr ? line->amount : 0.0;
		out.utilisation = std::min(out.utilisation, std::min(1.0, available / (input.perHour * dt)));
	}
	if (out.utilisation <= 0.0)
		return out;
	out.amount = std::min(room, producer.ratePerHour * dt * out.utilisation);
	return out;
}

void consumeInputs(state::TownStock& stock, const state::EconomyProducer& producer, double produced) noexcept
{
	if (produced <= 0.0 || producer.ratePerHour <= 0.0)
		return;
	// The inputs of the items actually made: perHour / ratePerHour each (so a rate cut by the output's room wastes nothing).
	for (const state::ConsumeLine& input : producer.consumes)
	{
		if (input.perHour <= 0.0)
			continue;
		state::StockLine* line = findLine(stock, input.itemSid);
		if (line != nullptr)
			line->amount = std::max(0.0, line->amount - input.perHour * produced / producer.ratePerHour);
	}
}

void applyFlow(state::TownStock& stock, const state::EconomyProducer& producer, double produced)
{
	if (produced <= 0.0)
		return;
	consumeInputs(stock, producer, produced);
	lineFor(stock, producer.itemSid).amount += produced;
}

void runProducer(state::TownStock& stock, const state::EconomyProducer& producer, double dt)
{
	applyFlow(stock, producer, flow(stock, producer, dt).amount);
}

void runCampMachine(state::TownStock& stock, const state::EconomyProducer& producer, state::Producer& machine, double dt)
{
	if (dt <= 0.0 || producer.ratePerHour <= 0.0)
		return;
	// The room left for a unit the machine will emit: the pending fraction already counts against the cap.
	state::StockLine& output = lineFor(stock, producer.itemSid);
	const double pending = cap(output) - output.amount - machine.progress;
	if (pending <= 0.0)
	{
		machine.prodState = 1; // full
		return;
	}
	Flow made = flow(stock, producer, dt);
	made.amount = std::min(made.amount, pending);
	machine.prodState = made.utilisation <= 0.0 ? 3 : 2; // 3 idle (out of resource), 2 working
	if (made.amount <= 0.0)
		return;
	consumeInputs(stock, producer, made.amount);
	machine.progress += made.amount;
	// Progress >= 0.999 emits one item and the fraction is kept (OURS: the exe's per-frame tick drops the overshoot; a coarse
	// economy step must not lose up to a whole item an hour).
	while (machine.progress >= 0.999)
	{
		machine.progress -= 1.0;
		output.amount += 1.0;
	}
	++machine.version;
}

void integrate(state::TownStock& stock, const std::vector<const state::EconomyProducer*>& producers, double dt)
{
	if (dt <= 0.0)
		return;
	for (const state::EconomyProducer* producer : producers)
		runProducer(stock, *producer, dt);
	for (state::StockLine& line : stock.lines)
		line.amount = std::clamp(line.amount, 0.0, cap(line));

	stock.prosperity = prosperity(stock);
}

bool isCamp(std::uint64_t id) noexcept
{
	// The camp Id range: above the camp base, below the foliage producers' 1<<40 offset (producers kIdBase is 1<<62).
	return id >= kCampIdBase && id < kCampIdBase + (1ull << 40);
}

double campRatePerHour(const producers::Spec& spec) noexcept
{
	// baseProgressPerTick is one operator's rate when maxOperators > 1 (the tick runs per operator); the machine's full rate is
	// every slot working. A machine with no slot (maxOperators 0) is automatic and needs none.
	const double slots = spec.maxOperators > 1 ? static_cast<double>(spec.maxOperators) : 1.0;
	return producers::baseProgressPerTick(spec) * static_cast<double>(kTicksPerGameHour) * slots;
}

std::vector<state::ConsumeLine> campConsumes(const kdata::GameData& data, const kdata::Record& function, const std::string& output,
	double ratePerHour)
{
	std::vector<state::ConsumeLine> consumes;
	const kdata::Record* item = data.find(output);
	// CQ34: the output ITEM's `ingredients` win (v0 / 100 per output); the functionality's `consumes` only when it overrides them or
	// the item has none (its rate is the ref's v1 / 100).
	const kdata::RefCategory* ingredients = function.boolOr("overrides ingredients", false) || item == nullptr
		? nullptr : item->findRefs("ingredients");
	bool listed = false;
	if (ingredients != nullptr)
		for (const kdata::RefEntry& entry : ingredients->entries)
		{
			if (entry.opaqueEntry || entry.v0 == kdata::kReferenceRemoved || entry.v0 <= 0)
				continue;
			consumes.push_back(state::ConsumeLine{entry.targetID, static_cast<double>(entry.v0) / 100.0 * ratePerHour});
			listed = true;
		}
	if (listed)
		return consumes;
	if (const kdata::RefCategory* machine = function.findRefs("consumes"))
		for (const kdata::RefEntry& entry : machine->entries)
		{
			if (entry.opaqueEntry || entry.v0 == kdata::kReferenceRemoved || entry.v1 <= 0)
				continue;
			consumes.push_back(state::ConsumeLine{entry.targetID, static_cast<double>(entry.v1) / 100.0 * ratePerHour});
		}
	return consumes;
}

std::vector<std::string> vendorItems(const kdata::GameData& data, const std::string& listSid)
{
	std::vector<std::string> items;
	const kdata::Record* list = data.find(listSid);
	if (list == nullptr)
		return items;
	if (const kdata::RefCategory* listed = list->findRefs("items"))
		for (const kdata::RefEntry& entry : listed->entries)
			if (!entry.opaqueEntry)
				items.push_back(entry.targetID);
	return items;
}

namespace {

// The index of the live (kind, item) entry, or npos. The board is kept sorted by (kind, itemSid, expires).
std::size_t entryIndex(const state::SupplyBoard& board, std::uint8_t kind, std::string_view itemSid) noexcept
{
	const auto it = std::lower_bound(board.entries.begin(), board.entries.end(), std::pair<std::uint8_t, std::string_view>{kind, itemSid},
		[](const state::BoardEntry& entry, const std::pair<std::uint8_t, std::string_view>& key)
		{
			return entry.kind < key.first || (entry.kind == key.first && entry.itemSid < key.second);
		});
	if (it != board.entries.end() && it->kind == kind && it->itemSid == itemSid)
		return static_cast<std::size_t>(it - board.entries.begin());
	return std::string::npos;
}

} // namespace

state::BoardEntry* findEntry(state::SupplyBoard& board, std::uint8_t kind, std::string_view itemSid) noexcept
{
	const std::size_t at = entryIndex(board, kind, itemSid);
	return at != std::string::npos ? &board.entries[at] : nullptr;
}

const state::BoardEntry* findEntry(const state::SupplyBoard& board, std::uint8_t kind, std::string_view itemSid) noexcept
{
	const std::size_t at = entryIndex(board, kind, itemSid);
	return at != std::string::npos ? &board.entries[at] : nullptr;
}

void postBoard(state::SupplyBoard& board, std::uint64_t town, const state::TownStock& stock, double now,
	const std::map<std::string, double>& inbound)
{
	// Expired claims go first: nobody holds an entry past its hour.
	board.entries.erase(std::remove_if(board.entries.begin(), board.entries.end(),
		[now](const state::BoardEntry& entry) { return entry.expires <= now; }), board.entries.end());
	const auto post = [&](std::uint8_t kind, const std::string& item, double amount)
	{
		const std::size_t at = entryIndex(board, kind, item);
		if (amount <= 0.0)
		{
			if (at != std::string::npos)
				board.entries.erase(board.entries.begin() + static_cast<std::ptrdiff_t>(at));
			return;
		}
		if (at != std::string::npos)
		{
			board.entries[at].amount = amount; // the live entry keeps its clock: it expires kBoardEntryHours after its post
			return;
		}
		board.entries.push_back(state::BoardEntry{kind, state::EntityRef{town}, state::EntityRef{}, item, amount, now, now + kBoardEntryHours});
	};
	// Offers (kind 0) then needs (kind 1), each in item order, so appending keeps the board sorted.
	for (const state::StockLine& line : stock.lines)
		post(state::kBoardOffer, line.itemSid, line.amount - line.target);
	for (const state::StockLine& line : stock.lines)
	{
		const auto found = inbound.find(line.itemSid);
		post(state::kBoardNeed, line.itemSid, line.target - line.amount - (found != inbound.end() ? found->second : 0.0));
	}
	std::stable_sort(board.entries.begin(), board.entries.end(), [](const state::BoardEntry& left, const state::BoardEntry& right)
	{
		return std::tie(left.kind, left.itemSid, left.expires) < std::tie(right.kind, right.itemSid, right.expires);
	});
}

double deliveryAmount(const state::StockLine& source, double need) noexcept
{
	return std::max(0.0, std::min({source.amount - source.target, need, kCaravanCapacity}));
}

double stockFactor(const state::TownStock& stock, std::string_view itemSid) noexcept
{
	const state::StockLine* line = findLine(stock, itemSid);
	if (line == nullptr || line->target <= 0.0)
		return 1.0; // the town does not stock it: no T4 price
	const double ratio = line->target / std::max(line->amount, kPriceAmountFloor);
	return std::clamp(std::pow(ratio, kPriceExponent), kPriceLow, kPriceHigh);
}

std::uint32_t takeFromTown(state::TownStock& stock, std::string_view itemSid, std::uint32_t want) noexcept
{
	state::StockLine* line = findLine(stock, itemSid);
	if (line == nullptr || want == 0)
		return 0;
	const std::uint32_t available = static_cast<std::uint32_t>(std::floor(std::max(0.0, line->amount)));
	const std::uint32_t taken = std::min(want, available);
	line->amount -= static_cast<double>(taken);
	return taken;
}

std::vector<state::StockLine> seedTown(const std::vector<std::string>& items, const kdata::Record* record)
{
	// The TOWN record's `trade prices`: an item's v0 is read as its stocking level (UNSURE: the field is named "prices").
	std::map<std::string, double> targets;
	std::vector<std::string> listed;
	if (record != nullptr)
	{
		if (const kdata::RefCategory* prices = record->findRefs("trade prices"))
			for (const kdata::RefEntry& entry : prices->entries)
			{
				if (entry.opaqueEntry)
					continue;
				targets[entry.targetID] = static_cast<double>(entry.v0);
				listed.push_back(entry.targetID);
			}
	}
	// The town's trade goods: the merchant items plus whatever its `trade prices` names (the two data sources of the T4a row).
	std::vector<std::string> goods = items;
	goods.insert(goods.end(), listed.begin(), listed.end());
	std::sort(goods.begin(), goods.end());
	goods.erase(std::unique(goods.begin(), goods.end()), goods.end());
	std::vector<state::StockLine> lines;
	for (const std::string& item : goods)
	{
		const auto override = targets.find(item);
		const double target = override != targets.end() && override->second > 0.0 ? override->second : kDefaultTarget;
		lines.push_back(state::StockLine{item, target, target}); // a town starts at its target (OURS)
	}
	return lines;
}

} // namespace ksim::economy

namespace ksim {

namespace {

// Full detail at a point (region.hpp): its region is in a player area, or every region is at fullDetailAll. A camp's machine runs
// only there (7.5); everywhere else the aggregate does.
bool fullDetailAt(const RegionEngine& regions, double x, double z)
{
	if (regions.config().fullDetailAll)
		return true;
	const std::int32_t region = regions.regionOf(x, z);
	for (const Area& area : regions.areas())
		if (std::find(area.regions.begin(), area.regions.end(), region) != area.regions.end())
			return true;
	return false;
}

} // namespace

void Simulation::seedEconomy(EconomyMode mode)
{
	if (mode != EconomyMode::Real || gameData_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	ecs.set<state::EconomySettings>(state::EconomySettings{state::kEconomyReal});
	const double now = trader::gameHours(clockTick());
	// The trade goods of every town: the items its merchant squads sell from (their VENDOR_LISTs), per the T1 merchant rule.
	std::map<flecs::entity_t, std::set<std::string>> goods;
	ecs.each([&](const state::SquadInstance& squad)
	{
		if (squad.homeTown.id == 0)
			return;
		const kdata::Record* record = gameData_->find(squad.templateSid);
		if (record == nullptr || !trader::isMerchant(*record))
			return;
		if (const kdata::RefCategory* vendors = record->findRefs("vendors"))
			for (const kdata::RefEntry& entry : vendors->entries)
			{
				if (entry.opaqueEntry)
					continue;
				const std::vector<std::string> items = economy::vendorItems(*gameData_, entry.targetID);
				goods[squad.homeTown.id].insert(items.begin(), items.end());
			}
	});
	std::vector<std::pair<flecs::entity_t, state::TownState>> towns;
	ecs.each([&](flecs::entity entity, const state::TownState& town)
	{
		towns.emplace_back(entity.id(), town);
	});
	std::sort(towns.begin(), towns.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	for (const auto& [key, town] : towns)
	{
		const kdata::Record* record = gameData_->find(town.activeSid.empty() ? town.rootSid : town.activeSid);
		std::vector<std::string> items;
		if (const auto found = goods.find(key); found != goods.end())
			items.assign(found->second.begin(), found->second.end());
		state::TownStock stock;
		stock.lines = economy::seedTown(items, record);
		stock.prosperity = economy::prosperity(stock);
		stock.lastTick = now;
		flecs::entity(ecs, key).set<state::TownStock>(std::move(stock));
		flecs::entity(ecs, key).set<state::SupplyBoard>(state::SupplyBoard{});
	}
	// The camps (T4b, 7.3): every placed production BUILDING as an EconomyProducer whose town is the nearest one (OURS: a placement's
	// zone town hand is not retained by World after the load, and the data homes the workers in the town, not on the camp). A
	// converter (an input list) also gets the real machine (state::Producer): its Full-detail tick is the camp's output path.
	for (std::size_t index = 0; index < world_.productionSites().size(); ++index)
	{
		const ProductionSite& site = world_.productionSites()[index];
		const kdata::Record* building = gameData_->find(site.recordSid);
		producers::Spec spec;
		if (building == nullptr || !producers::productionSpecOf(*gameData_, *building, spec))
			continue;
		const kdata::Record* function = producers::productionFunctionality(*gameData_, *building);
		const double rate = economy::campRatePerHour(spec);
		if (function == nullptr || rate <= 0.0 || towns.empty())
			continue;
		flecs::entity_t nearest = towns.front().first;
		double best = 1e30;
		for (const auto& [key, town] : towns)
		{
			const double distance = (town.x - site.x) * (town.x - site.x) + (town.z - site.z) * (town.z - site.z);
			if (distance < best)
			{
				best = distance;
				nearest = key;
			}
		}
		const flecs::entity camp = ecs.entity();
		camp.set<state::Id>(state::Id{economy::kCampIdBase + index});
		camp.set<state::EconomyProducer>(state::EconomyProducer{state::EntityRef{nearest}, spec.output, rate, 0.0,
			economy::campConsumes(*gameData_, *function, spec.output, rate)});
		if (camp.get<state::EconomyProducer>()->consumes.empty())
			continue; // no inputs (a mine, a well, a stone camp): the aggregate is its only production node
		state::Producer machine;
		machine.recordSid = site.recordSid;
		machine.ownerSid = site.ownerFaction;
		machine.output = spec.output;
		machine.cap = spec.cap;
		machine.maxOperators = static_cast<std::uint8_t>(std::clamp(spec.maxOperators, 0, 255));
		machine.useRange = spec.useRange;
		machine.progressBar = spec.progressBar;
		machine.x = site.x;
		machine.y = site.y;
		machine.z = site.z;
		machine.useX = site.x;
		machine.useY = site.y;
		machine.useZ = site.z;
		machine.progress = 0.0;
		machine.prodState = 3;
		machine.version = 1;
		camp.set<state::Producer>(machine);
	}
}

void Simulation::setEconomyHooks(std::shared_ptr<economy::Hooks> hooks)
{
	economyHooks_ = std::move(hooks);
	tradeBook_ = nullptr; // the terms carry the hook pointer: rebuilt on the next quote
}

void Simulation::stepEconomy()
{
	flecs::world& ecs = ecsMutable();
	const state::EconomySettings* settings = ecs.get<state::EconomySettings>();
	if (settings == nullptr || settings->mode != state::kEconomyReal)
		return;
	const double now = trader::gameHours(clockTick());
	// Nothing to do between ticks: one scan of the towns, no allocation.
	bool due = false;
	ecs.each([&](const state::TownStock& stock) { due = due || now - stock.lastTick >= economy::kTickHours; });
	if (!due)
		return;
	// The staffed towns (T4b, 7.3): a camp runs only while its town has a living staff squad. The staff is the town's resident
	// squads (OURS: the data homes a camp's workers in the town, not on the camp placement); a squad with no effective member (all
	// its built fighters dead or down) does not staff.
	std::set<flecs::entity_t> staffed;
	ecs.each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		if (instance.homeTown.id != 0 && squadVitals(ecs, squad, gameData_.get()).effective > 0)
			staffed.insert(instance.homeTown.id);
	});
	// The producers of every town, in state::Id order (a camp's Id is the same after a load; a raw entity id is not). An entity
	// without one (a T4a-style producer) sorts by its raw id, after every instance.
	std::map<flecs::entity_t, std::vector<std::pair<std::uint64_t, flecs::entity>>> producers;
	ecs.each([&](flecs::entity entity, const state::EconomyProducer& producer)
	{
		if (producer.town.id != 0)
		{
			const state::Id* id = entity.get<state::Id>();
			producers[producer.town.id].emplace_back(id != nullptr ? id->value : entity.id(), entity);
		}
	});
	for (auto& [town, list] : producers)
		std::sort(list.begin(), list.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	std::vector<std::pair<flecs::entity_t, state::TownStock*>> stocks;
	ecs.each([&](flecs::entity entity, state::TownStock& stock) { stocks.emplace_back(entity.id(), &stock); });
	std::sort(stocks.begin(), stocks.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	for (const auto& [key, stock] : stocks)
	{
		double dt = now - stock->lastTick;
		if (dt < economy::kTickHours)
			continue;
		dt = std::min(dt, economy::kMaxCatchUpHours);
		stock->lastTick = now;
		// T4e: the script's economy.demand sets this town's target levels before the tick consumes them (a target off the stock moves
		// it, and the board posts what the new targets make surplus or short).
		if (economyHooks_ != nullptr)
		{
			const flecs::entity town(ecs, key);
			const state::Id* townId = town.get<state::Id>();
			economyHooks_->demand(townId != nullptr ? townId->value : static_cast<std::uint64_t>(key), now, *stock);
		}
		// One path per camp for this span, so loading or unloading a camp never double-counts: a camp at Full detail runs its own
		// machine (state::Producer) into the same stock, every other camp (and every T4a-style producer) the aggregate.
		std::vector<const state::EconomyProducer*> running;
		const bool staff = staffed.count(key) != 0;
		if (const auto found = producers.find(key); found != producers.end())
			for (const auto& [entityId, entity] : found->second)
			{
				const state::EconomyProducer* producer = entity.get<state::EconomyProducer>();
				const state::Id* id = entity.get<state::Id>();
				state::Producer* machine = entity.get_mut<state::Producer>();
				if (!staff && id != nullptr && economy::isCamp(id->value))
				{
					if (machine != nullptr && machine->prodState != 3)
					{
						machine->prodState = 3; // idle, no staff
						++machine->version;
					}
					continue;
				}
				if (machine != nullptr && regions_ != nullptr && fullDetailAt(*regions_, machine->x, machine->z))
					economy::runCampMachine(*stock, *producer, *machine, dt);
				else
					running.push_back(producer);
			}
		economy::integrate(*stock, running, dt);
	}

	// ---- the supply board and the delivery caravans (T4c, 7.4) ----
	struct Spot
	{
		flecs::entity_t key = 0;
		state::TownStock* stock = nullptr;
		state::SupplyBoard* board = nullptr;
		double x = 0.0;
		double z = 0.0;
	};
	std::vector<Spot> spots;
	for (const auto& [key, stock] : stocks)
	{
		flecs::entity town(ecs, key);
		state::SupplyBoard* board = town.get_mut<state::SupplyBoard>();
		const state::TownState* townState = town.get<state::TownState>();
		if (board != nullptr && townState != nullptr)
			spots.push_back(Spot{key, stock, board, townState->x, townState->z});
	}
	// The goods on the road, per destination and item: the destination half of "both ends reserved" (the source's goods left the
	// stock when the order was matched), so the shortfall they cover is not posted again.
	std::map<flecs::entity_t, std::map<std::string, double>> inbound;
	ecs.each([&](const state::CaravanOrder& order)
	{
		if (order.phase != state::kCaravanUnload && order.to.id != 0)
			inbound[order.to.id][order.itemSid] += order.amount;
	});
	const std::map<std::string, double> none;
	for (const Spot& spot : spots)
	{
		const auto found = inbound.find(spot.key);
		economy::postBoard(*spot.board, spot.key, *spot.stock, now, found != inbound.end() ? found->second : none);
	}
	// Matching: a trade-circuit squad staying in a town carries one offer an hour. OURS: the delivery caravan is the T2 trade-circuit
	// squad (7.4's overlay template family is not built), and the offered surplus is the town's stock over its target (a producer's
	// output feeds the stock directly in T4a/T4b).
	std::vector<std::pair<std::uint64_t, flecs::entity>> caravans;
	ecs.each([&](flecs::entity entity, const state::SquadInstance&, const state::TraderTrip&)
	{
		if (entity.get<state::CaravanOrder>() != nullptr)
			return;
		const state::Id* id = entity.get<state::Id>();
		caravans.emplace_back(id != nullptr ? id->value : entity.id(), entity);
	});
	std::sort(caravans.begin(), caravans.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	const auto take = [](state::SupplyBoard& board, std::uint8_t kind, const std::string& item, double amount)
	{
		state::BoardEntry* entry = economy::findEntry(board, kind, item);
		if (entry == nullptr)
			return;
		entry->amount -= amount;
		if (entry->amount > 0.0)
			return;
		const std::ptrdiff_t at = entry - board.entries.data();
		board.entries.erase(board.entries.begin() + at);
	};
	for (const auto& [id, entity] : caravans)
	{
		const state::TraderTrip* trip = entity.get<state::TraderTrip>();
		if (trip == nullptr || !trip->staying || trip->town.id == 0)
			continue;
		const auto source = std::find_if(spots.begin(), spots.end(), [&](const Spot& spot) { return spot.key == trip->town.id; });
		if (source == spots.end())
			continue;
		// What it could carry, as the board stands this hour (the entries in item order).
		std::vector<std::string> offers;
		for (const state::BoardEntry& entry : source->board->entries)
		{
			const state::StockLine* line = economy::findLine(*source->stock, entry.itemSid);
			if (entry.kind == state::kBoardOffer && line != nullptr && line->amount > line->target)
				offers.push_back(entry.itemSid);
		}
		for (const std::string& item : offers)
		{
			// The nearest other town needing the item.
			const Spot* buyer = nullptr;
			double nearest = 0.0;
			for (const Spot& spot : spots)
			{
				if (spot.key == source->key)
					continue;
				const state::BoardEntry* need = economy::findEntry(*spot.board, state::kBoardNeed, item);
				if (need == nullptr || need->amount <= 0.0)
					continue;
				const double distance = (spot.x - source->x) * (spot.x - source->x) + (spot.z - source->z) * (spot.z - source->z);
				if (buyer == nullptr || distance < nearest)
				{
					buyer = &spot;
					nearest = distance;
				}
			}
			if (buyer == nullptr)
				continue;
			const state::BoardEntry* need = economy::findEntry(*buyer->board, state::kBoardNeed, item);
			state::StockLine* line = economy::findLine(*source->stock, item);
			const double amount = economy::deliveryAmount(*line, need->amount);
			if (amount < economy::kMinDelivery)
				continue;
			// Both ends of the claim leave the boards with the goods.
			line->amount -= amount;
			take(*source->board, state::kBoardOffer, item, amount);
			take(*buyer->board, state::kBoardNeed, item, amount);
			state::CaravanOrder order;
			order.from = state::EntityRef{source->key};
			order.to = state::EntityRef{buyer->key};
			order.itemSid = item;
			order.amount = amount;
			order.phase = state::kCaravanLoad;
			order.postedAt = now;
			entity.set<state::CaravanOrder>(order);
			break;
		}
	}
	// The phases: load -> go once the goods are aboard, go -> unload on the circuit's arrival at the destination (the T2 `staying`
	// stamp, so the delivery reuses the wandering trader's own arrival), unload -> gone.
	std::vector<std::pair<std::uint64_t, flecs::entity>> loaded;
	ecs.each([&](flecs::entity entity, const state::CaravanOrder&)
	{
		const state::Id* id = entity.get<state::Id>();
		loaded.emplace_back(id != nullptr ? id->value : entity.id(), entity);
	});
	std::sort(loaded.begin(), loaded.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
	for (const auto& [id, entity] : loaded)
	{
		state::CaravanOrder* order = entity.get_mut<state::CaravanOrder>();
		if (order->phase != state::kCaravanUnload && now - order->postedAt >= economy::kOrderMaxHours)
		{
			// A stale order (T4d, OURS): the carrier never arrived. The goods go back to the source (a full source destroys the excess),
			// or are lost if the source is gone; dropping the order frees the destination's reservation, so its need re-posts next tick.
			const auto from = std::find_if(spots.begin(), spots.end(), [&](const Spot& spot) { return spot.key == order->from.id; });
			if (from != spots.end())
			{
				state::StockLine& line = economy::lineFor(*from->stock, order->itemSid);
				line.amount = std::min(economy::cap(line), line.amount + order->amount);
			}
			entity.remove<state::CaravanOrder>();
			continue;
		}
		if (order->phase == state::kCaravanLoad)
		{
			order->phase = state::kCaravanGo;
			continue;
		}
		if (order->phase == state::kCaravanGo)
		{
			const state::TraderTrip* trip = entity.get<state::TraderTrip>();
			if (trip == nullptr || !trip->staying || trip->town.id != order->to.id)
				continue;
			const auto at = std::find_if(spots.begin(), spots.end(), [&](const Spot& spot) { return spot.key == order->to.id; });
			if (at != spots.end())
			{
				state::StockLine& line = economy::lineFor(*at->stock, order->itemSid);
				line.amount = std::min(economy::cap(line), line.amount + order->amount); // a full town destroys the rest (D2)
			}
			if (economyHooks_ != nullptr)
				economyHooks_->onDelivery(*order);
			order->phase = state::kCaravanUnload;
			continue;
		}
		if (order->phase == state::kCaravanUnload)
			entity.remove<state::CaravanOrder>();
	}
}

} // namespace ksim
