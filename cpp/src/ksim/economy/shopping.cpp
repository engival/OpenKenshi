#include "ksim/economy/shopping.hpp"

#include <algorithm>
#include <cmath>

namespace ksim::shopping {

std::vector<state::ShoppingWant> buildList(std::span<const tmpl::ShoppingRef> refs, const CombatData& items, std::uint64_t seed,
	std::uint64_t diceKey, std::int64_t day)
{
	// The roll runs over the refs in data order (the dice sequence is per ref, whatever the later order); the list is then kept in
	// priority order (v2, lower first) as the exe walks it.
	struct Pending
	{
		std::uint16_t function = 0;
		std::uint32_t want = 0;
		std::int32_t priority = 0;
	};
	std::vector<Pending> pending;
	RngStream rolls(streamSeed(streamSeed(streamSeed(seed, kShoppingStream), static_cast<std::size_t>(diceKey)),
		static_cast<std::size_t>(day)));
	for (const tmpl::ShoppingRef& ref : refs)
	{
		const double roll = rolls.uniform01();
		if (roll * 100.0 >= static_cast<double>(ref.chance))
			continue; // not wanted today (the ref's chance is the daily percent; 100 always)
		const ItemInfo* info = items.item(ref.itemSid);
		if (info == nullptr)
			continue; // a dangling ref or a record the combat data does not name (WEAPON / ARMOUR refs would land here)
		const std::uint16_t function = static_cast<std::uint16_t>(std::max(0, info->function));
		const std::uint16_t want = static_cast<std::uint16_t>(std::clamp(ref.want, 0, 65535));
		if (want == 0)
			continue;
		const auto found = std::find_if(pending.begin(), pending.end(), [&](const Pending& entry) { return entry.function == function; });
		if (found != pending.end())
		{
			found->want = std::min<std::uint32_t>(65535, found->want + want);
			found->priority = std::min(found->priority, ref.priority);
		}
		else
			pending.push_back({function, want, ref.priority});
	}
	std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) { return a.priority < b.priority; });
	std::vector<state::ShoppingWant> out;
	out.reserve(pending.size());
	for (const Pending& entry : pending)
		out.push_back({entry.function, static_cast<std::uint16_t>(entry.want), 0});
	return out;
}

bool unmet(const std::vector<state::ShoppingWant>& list) noexcept
{
	return firstUnmet(list) != nullptr;
}

const state::ShoppingWant* firstUnmet(const std::vector<state::ShoppingWant>& list) noexcept
{
	for (const state::ShoppingWant& entry : list)
		if (entry.taken < entry.want)
			return &entry;
	return nullptr;
}

bool takeOne(state::Inventory& stock, const CombatData& items, std::uint16_t function, double roll, state::InventoryEntry& out)
{
	const auto matches = [&](const state::InventoryEntry& entry)
	{
		const ItemInfo* info = items.item(entry.itemSid);
		return info != nullptr && static_cast<std::uint16_t>(std::max(0, info->function)) == function;
	};
	std::uint64_t units = 0;
	for (const state::InventoryEntry& entry : stock.items)
		if (matches(entry))
			units += entry.count;
	if (units == 0)
		return false;
	// Uniform over the matching units (dt24_shop section 5: the transaction lists the items of the function and picks ONE at random).
	std::uint64_t pick = std::min<std::uint64_t>(static_cast<std::uint64_t>(roll * static_cast<double>(units)), units - 1);
	std::size_t index = 0;
	for (std::size_t i = 0; i < stock.items.size(); ++i)
	{
		const state::InventoryEntry& entry = stock.items[i];
		if (!matches(entry))
			continue;
		if (pick < entry.count)
		{
			index = i;
			break;
		}
		pick -= entry.count;
	}
	// Extract it (14074a630): split a stack of two or more, else the entry leaves.
	state::InventoryEntry piece = stock.items[index];
	piece.count = 1;
	piece.section = state::InventoryEntry::kUnplaced;
	piece.x = 0;
	piece.y = 0;
	piece.contents.clear(); // a unit of a stack carries no container contents of its own
	if (stock.items[index].count > 1)
		--stock.items[index].count;
	else
		stock.items.erase(stock.items.begin() + static_cast<std::ptrdiff_t>(index));
	++stock.version;
	out = std::move(piece);
	return true;
}

} // namespace ksim::shopping
