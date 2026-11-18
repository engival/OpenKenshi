#include "ksim/inventory_grid.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>

namespace ksim::grid {

namespace {

constexpr std::uint8_t kMain = 0;
constexpr std::uint8_t kBag = 1;

// A grid: the list its entries are in, its size and the container item it belongs to (null for main).
struct View
{
	std::vector<Entry>* list;
	int w;
	int h;
	const Entry* bag;
	std::uint32_t limit = 0; // state::Inventory::stackLimit of main (a producer's slot); 0 = the item's own cap
};

// The cap of one stack of `sid` in the view.
std::uint32_t viewCap(const Rules& rules, const View& view, const std::string& sid)
{
	return view.limit > 0 ? view.limit : static_cast<std::uint32_t>(rules.stackCap(sid, view.bag));
}

bool inGrid(const Entry& entry)
{
	return entry.slot == state::Inventory::kCarried && entry.section == kMain;
}

bool overlaps(int x, int y, Size a, int bx, int by, Size b)
{
	return x < bx + b.w && bx < x + a.w && y < by + b.h && by < y + a.h;
}

// The entries of the view that overlap the rect, except `ignore`.
std::vector<Entry*> overlapping(const View& view, const Rules& rules, int x, int y, Size fp, const Entry* ignore)
{
	std::vector<Entry*> found;
	for (Entry& other : *view.list)
	{
		if (&other == ignore || !inGrid(other))
			continue;
		if (overlaps(x, y, fp, other.x, other.y, rules.footprint(other.itemSid)))
			found.push_back(&other);
	}
	return found;
}

// The first free cell for a footprint: y outer, x inner (InventorySection::vf2). -1 when none.
bool firstFree(const View& view, const Rules& rules, Size fp, int& outX, int& outY)
{
	if (fp.w > view.w || fp.h > view.h)
		return false;
	std::vector<char> occupied(static_cast<std::size_t>(view.w * view.h), 0);
	for (const Entry& other : *view.list)
	{
		if (!inGrid(other))
			continue;
		const Size size = rules.footprint(other.itemSid);
		for (int cy = other.y; cy < std::min(view.h, other.y + size.h); ++cy)
		{
			for (int cx = other.x; cx < std::min(view.w, other.x + size.w); ++cx)
				occupied[static_cast<std::size_t>(cy * view.w + cx)] = 1;
		}
	}
	for (int y = 0; y + fp.h <= view.h; ++y)
	{
		for (int x = 0; x + fp.w <= view.w; ++x)
		{
			bool free = true;
			for (int cy = y; cy < y + fp.h && free; ++cy)
			{
				for (int cx = x; cx < x + fp.w; ++cx)
				{
					if (occupied[static_cast<std::size_t>(cy * view.w + cx)] != 0)
					{
						free = false;
						break;
					}
				}
			}
			if (free)
			{
				outX = x;
				outY = y;
				return true;
			}
		}
	}
	return false;
}

// Fills the same-kind stacks of the view, then places new ones; returns the units left over.
std::uint32_t addTo(const View& view, const Rules& rules, Entry entry)
{
	const std::uint32_t cap = viewCap(rules, view, entry.itemSid);
	const Size fp = rules.footprint(entry.itemSid);
	std::uint32_t remaining = entry.count;
	if (cap > 1)
	{
		for (Entry& other : *view.list)
		{
			if (remaining == 0)
				break;
			if (inGrid(other) && other.count < cap && other.sameKind(entry))
			{
				const std::uint32_t part = std::min(remaining, cap - other.count);
				other.takePlantedFrom(entry, part);
				other.count += part;
				remaining -= part;
			}
		}
	}
	while (remaining > 0)
	{
		int x = 0;
		int y = 0;
		if (!firstFree(view, rules, fp, x, y))
			break;
		Entry stack = entry;
		stack.count = std::min(remaining, cap);
		if (stack.count != entry.count)
			stack.contents.clear();
		stack.planted.clear();
		stack.takePlantedFrom(entry, stack.count);
		stack.slot = state::Inventory::kCarried;
		stack.section = kMain;
		stack.x = static_cast<std::uint8_t>(x);
		stack.y = static_cast<std::uint8_t>(y);
		remaining -= stack.count;
		view.list->push_back(std::move(stack));
	}
	return remaining;
}

View mainView(state::Inventory& inventory)
{
	return {&inventory.items, inventory.gridW, inventory.gridH, nullptr, inventory.stackLimit};
}

// The view of the worn backpack; bag size from the record. False when none is worn or its record has no size.
bool bagView(state::Inventory& inventory, const Rules& rules, View& out)
{
	Entry* bag = inventory.wornBackpack();
	if (bag == nullptr)
		return false;
	const Size size = rules.bagSize(bag->itemSid);
	if (size.w <= 0 || size.h <= 0)
		return false;
	out = {&bag->contents, size.w, size.h, bag};
	return true;
}

bool canHoldInBag(const Rules& rules, const Entry& entry)
{
	return !(rules.isContainer(entry.itemSid) && !entry.contents.empty());
}

std::uint64_t mix(std::uint64_t v)
{
	v += 0x9e3779b97f4a7c15ULL;
	v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
	v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
	return v ^ (v >> 31);
}

std::uint64_t fnv(const std::string& text)
{
	std::uint64_t h = 1469598103934665603ULL;
	for (const char c : text)
		h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
	return h;
}

// A material for every instance whose record lists some (a seeded pick, made once: it is saved on the entry).
void assignMaterials(std::vector<Entry>& list, const Rules& rules, std::uint64_t ownerKey, std::uint32_t& serial)
{
	for (Entry& entry : list)
	{
		if (entry.material.empty() && entry.quality.model.empty())
		{
			const std::vector<std::string> choices = rules.materials(entry.itemSid);
			if (!choices.empty())
				entry.material = choices[mix(ownerKey ^ mix(fnv(entry.itemSid) + serial)) % choices.size()];
			++serial;
		}
		assignMaterials(entry.contents, rules, ownerKey, serial);
	}
}

// Keeps the valid placements of one grid, caps the stacks, and moves the rest to `pending`.
void validate(const View& view, const Rules& rules, std::vector<Entry>& pending)
{
	std::vector<char> occupied(static_cast<std::size_t>(view.w * view.h), 0);
	for (auto it = view.list->begin(); it != view.list->end();)
	{
		if (view.bag != nullptr)
			it->slot = state::Inventory::kCarried; // a bag holds only carried entries
		if (it->slot != state::Inventory::kCarried)
		{
			++it;
			continue;
		}
		if (it->count == 0)
		{
			it = view.list->erase(it);
			continue;
		}
		const Size fp = rules.footprint(it->itemSid);
		bool ok = it->section == kMain && it->x + fp.w <= view.w && it->y + fp.h <= view.h
			&& (view.bag == nullptr || canHoldInBag(rules, *it));
		for (int cy = it->y; ok && cy < it->y + fp.h; ++cy)
		{
			for (int cx = it->x; cx < it->x + fp.w; ++cx)
			{
				if (occupied[static_cast<std::size_t>(cy * view.w + cx)] != 0)
				{
					ok = false;
					break;
				}
			}
		}
		if (!ok)
		{
			it->section = Entry::kUnplaced;
			pending.push_back(std::move(*it));
			it = view.list->erase(it);
			continue;
		}
		for (int cy = it->y; cy < it->y + fp.h; ++cy)
		{
			for (int cx = it->x; cx < it->x + fp.w; ++cx)
				occupied[static_cast<std::size_t>(cy * view.w + cx)] = 1;
		}
		const std::uint32_t cap = viewCap(rules, view, it->itemSid);
		if (it->count > cap)
		{
			Entry excess = *it;
			excess.count -= cap;
			excess.section = Entry::kUnplaced;
			excess.contents.clear();
			it->count = cap;
			pending.push_back(std::move(excess));
		}
		++it;
	}
}

} // namespace

Rules::Rules(const kdata::GameData& data)
	: data_(&data)
	, weapon_(kworld::itemTypeByName("WEAPON"))
	, armour_(kworld::itemTypeByName("ARMOUR"))
	, item_(kworld::itemTypeByName("ITEM"))
	, container_(kworld::itemTypeByName("CONTAINER"))
	, map_(kworld::itemTypeByName("MAP_ITEM"))
	, crossbow_(kworld::itemTypeByName("CROSSBOW"))
	, limb_(kworld::itemTypeByName("LIMB_REPLACEMENT"))
	, building_(kworld::itemTypeByName("BUILDING"))
{
}

Size Rules::footprint(const std::string& sid) const
{
	const kdata::Record* record = data_->find(sid);
	if (record == nullptr)
		return {};
	return {std::max(1, record->intOr("inventory footprint width", 1)), std::max(1, record->intOr("inventory footprint height", 1))};
}

int Rules::stackCap(const std::string& sid, const Entry* bag, bool whitelisted) const
{
	const kdata::Record* record = data_->find(sid);
	if (record == nullptr)
		return 1;
	const std::int32_t type = record->type;
	if (type == weapon_ || type == armour_ || type == container_ || type == map_ || type == crossbow_ || type == limb_)
		return 1;
	// INVENTORY_GRID_LIVE (c): a whitelisted section forces the base to 1 before the min / mult.
	int cap = whitelisted ? 1 : std::max(1, record->intOr("stackable", 1));
	if (bag != nullptr)
	{
		if (const kdata::Record* holder = data_->find(bag->itemSid))
		{
			const int minimum = holder->intOr("stackable bonus minimum", 1);
			const float mult = holder->floatOr("stackable bonus mult", 1.0f);
			cap = std::max(1, static_cast<int>(static_cast<float>(std::max(cap, minimum)) * mult));
		}
	}
	return cap;
}

bool Rules::isContainer(const std::string& sid) const
{
	const kdata::Record* record = data_->find(sid);
	return record != nullptr && record->type == container_;
}

Size Rules::bagSize(const std::string& sid) const
{
	const kdata::Record* record = data_->find(sid);
	if (record == nullptr || record->type != container_)
		return {0, 0};
	return {record->intOr("storage size width", 0), record->intOr("storage size height", 0)};
}

Size Rules::buildingSize(const std::string& sid) const
{
	const kdata::Record* record = data_->find(sid);
	if (record == nullptr || record->type != building_)
		return {0, 0};
	return {record->intOr("storage size width", 0), record->intOr("storage size height", 0)};
}

std::vector<std::string> Rules::materials(const std::string& sid) const
{
	std::vector<std::string> out;
	const kdata::Record* record = data_->find(sid);
	if (record == nullptr || (record->type != armour_ && record->type != item_ && record->type != container_))
		return out;
	if (const kdata::RefCategory* refs = record->findRefs("material"))
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (!entry.opaqueEntry && !entry.targetID.empty())
				out.push_back(entry.targetID);
		}
	}
	return out;
}

std::uint32_t autoplace(state::Inventory& inventory, const Rules& rules, Entry entry, std::uint8_t section)
{
	// INVENTORY_GRID_LIVE (b): nothing overflows from main into the worn backpack; the backpack is a target only when named.
	if (section == kBag)
	{
		View bag;
		if (!canHoldInBag(rules, entry) || !bagView(inventory, rules, bag))
			return entry.count;
		return addTo(bag, rules, std::move(entry));
	}
	return addTo(mainView(inventory), rules, std::move(entry));
}

std::vector<Entry> settle(state::Inventory& inventory, const Rules& rules, std::uint64_t ownerKey)
{
	std::uint32_t serial = 0;
	assignMaterials(inventory.items, rules, ownerKey, serial);
	for (Entry& entry : inventory.items)
	{
		if (entry.slot != state::Inventory::kCarried)
		{
			entry.section = Entry::kEquipped;
			entry.x = entry.y = 0;
		}
	}
	std::vector<Entry> pending;
	validate(mainView(inventory), rules, pending);
	// The backpack's own excess goes back into the backpack first (a stack over its cap splits there), the rest as any other entry.
	View bag;
	std::vector<Entry> overflow;
	if (bagView(inventory, rules, bag))
	{
		std::vector<Entry> bagPending;
		validate(bag, rules, bagPending);
		for (Entry& entry : bagPending)
		{
			if (!canHoldInBag(rules, entry))
			{
				pending.push_back(std::move(entry));
				continue;
			}
			const std::uint32_t left = addTo(bag, rules, entry);
			if (left > 0)
			{
				entry.count = left;
				pending.push_back(std::move(entry));
			}
		}
	}
	for (Entry& entry : pending)
	{
		const std::uint32_t left = autoplace(inventory, rules, entry);
		if (left > 0)
		{
			entry.count = left;
			overflow.push_back(std::move(entry));
		}
	}
	// An equipped backpack that the data gives no size: its contents have nowhere to be; they go with the overflow.
	if (Entry* worn = inventory.wornBackpack(); worn != nullptr && !worn->contents.empty() && !bagView(inventory, rules, bag))
	{
		for (Entry& entry : worn->contents)
			overflow.push_back(std::move(entry));
		worn->contents.clear();
	}
	return overflow;
}

Entry* entryAt(state::Inventory& inventory, std::size_t index)
{
	if (index < inventory.items.size())
		return &inventory.items[index];
	index -= inventory.items.size();
	Entry* bag = inventory.wornBackpack();
	return bag != nullptr && index < bag->contents.size() ? &bag->contents[index] : nullptr;
}

std::size_t entryCount(const state::Inventory& inventory)
{
	const Entry* bag = inventory.wornBackpack();
	return inventory.items.size() + (bag != nullptr ? bag->contents.size() : 0);
}

std::ptrdiff_t firstCarried(const state::Inventory& inventory, const std::string& sid)
{
	state::Inventory& view = const_cast<state::Inventory&>(inventory);
	for (std::size_t i = 0, n = entryCount(inventory); i < n; ++i)
	{
		const Entry* entry = entryAt(view, i);
		if (entry->slot == state::Inventory::kCarried && entry->itemSid == sid)
			return static_cast<std::ptrdiff_t>(i);
	}
	return -1;
}

std::ptrdiff_t firstEquipped(const state::Inventory& inventory, const std::string& sid)
{
	for (std::size_t i = 0; i < inventory.items.size(); ++i)
	{
		if (inventory.items[i].slot != state::Inventory::kCarried && inventory.items[i].itemSid == sid)
			return static_cast<std::ptrdiff_t>(i);
	}
	return -1;
}

std::string moveEntry(state::Inventory& inventory, const Rules& rules, std::size_t index, std::uint8_t section, std::uint8_t x, std::uint8_t y, std::uint32_t count)
{
	Entry* entry = entryAt(inventory, index);
	if (entry == nullptr)
		return "bad_value";
	if (entry->slot != state::Inventory::kCarried)
		return "not_allowed";
	View dest = mainView(inventory);
	if (section == kBag)
	{
		if (!bagView(inventory, rules, dest) || !canHoldInBag(rules, *entry))
			return "no_room";
	} else if (section != kMain) {
		return "bad_value";
	}
	const Size fp = rules.footprint(entry->itemSid);
	if (x + fp.w > dest.w || y + fp.h > dest.h)
		return "no_room";
	// Which list the entry is in (main's `items` or the bag's contents) and where. Looked up again after any push_back: a push into
	// `items` moves the worn backpack entry and with it its contents.
	const bool inMain = index < inventory.items.size();
	const std::size_t position = inMain ? index : index - inventory.items.size();
	const auto source = [&]() -> std::vector<Entry>& { return inMain ? inventory.items : inventory.wornBackpack()->contents; };
	const bool sameList = dest.list == &source();
	const std::uint32_t cap = viewCap(rules, dest, entry->itemSid);
	if (count > 0 && count < entry->count)
	{
		// A split (order 64 `count`): `count` units go to the target cell, the rest stays at the source. The source entry stays in
		// the grid, so it blocks its own cells. Merge up to the cap or take an empty rect; anything else leaves both cells unchanged.
		const std::vector<Entry*> under = overlapping(dest, rules, x, y, fp, nullptr);
		if (under.empty())
		{
			if (count > cap)
				return "no_room";
			Entry piece = *entry;
			piece.count = count;
			piece.contents.clear();
			piece.planted.clear();
			piece.takePlantedFrom(*entry, count);
			piece.section = kMain;
			piece.x = x;
			piece.y = y;
			entry->count -= count;
			dest.list->push_back(std::move(piece));
			return {};
		}
		if (under.size() != 1 || under[0] == entry || cap <= 1 || !under[0]->sameKind(*entry) || under[0]->count >= cap)
			return "occupied";
		const std::uint32_t part = std::min(count, cap - under[0]->count);
		under[0]->takePlantedFrom(*entry, part);
		under[0]->count += part;
		entry->count -= part;
		return {};
	}
	const std::vector<Entry*> hit = overlapping(dest, rules, x, y, fp, entry);
	if (hit.empty())
	{
		if (entry->count > cap)
			return "no_room";
		if (sameList)
		{
			entry->section = kMain;
			entry->x = x;
			entry->y = y;
			return {};
		}
		Entry moved = std::move(*entry);
		moved.section = kMain;
		moved.x = x;
		moved.y = y;
		dest.list->push_back(std::move(moved));
		source().erase(source().begin() + static_cast<std::ptrdiff_t>(position));
		return {};
	}
	if (hit.size() != 1)
		return "occupied";
	Entry* other = hit[0];
	// INVENTORY_GRID_LIVE (e): onto one item of the same kind (and stackable) the stacks merge up to the cap, the rest stays here.
	if (cap > 1 && other->sameKind(*entry))
	{
		if (other->count >= cap)
			return "occupied"; // UNSURE: the exe's drop of a stack onto a full one of its kind
		const std::uint32_t part = std::min(entry->count, cap - other->count);
		other->takePlantedFrom(*entry, part);
		other->count += part;
		entry->count -= part;
		if (entry->count == 0)
			source().erase(source().begin() + static_cast<std::ptrdiff_t>(position));
		return {};
	}
	if (entry->count > cap)
		return "no_room";
	// Otherwise swap: the other entry goes to the moved entry's old cell of its old section, if it fits there once the move is done.
	View home = mainView(inventory);
	if (!inMain && !bagView(inventory, rules, home))
		return "occupied";
	const Size ofp = rules.footprint(other->itemSid);
	const int ox = entry->x;
	const int oy = entry->y;
	if (ox + ofp.w > home.w || oy + ofp.h > home.h || (!inMain && !canHoldInBag(rules, *other))
		|| other->count > viewCap(rules, home, other->itemSid))
		return "occupied";
	if (sameList && overlaps(ox, oy, ofp, x, y, fp))
		return "occupied";
	for (const Entry* blocker : overlapping(home, rules, ox, oy, ofp, entry))
	{
		if (blocker != other)
			return "occupied";
	}
	if (sameList)
	{
		other->x = static_cast<std::uint8_t>(ox);
		other->y = static_cast<std::uint8_t>(oy);
		entry->x = x;
		entry->y = y;
		return {};
	}
	// Across the two lists: take both out by value, then put each in the other's list (the lists are looked up again after every
	// erase: a change in `items` moves the worn backpack entry).
	Entry moved = std::move(*entry);
	Entry swapped = std::move(*other);
	const auto listOf = [&](bool main) -> std::vector<Entry>& { return main ? inventory.items : inventory.wornBackpack()->contents; };
	const std::size_t otherPos = static_cast<std::size_t>(other - dest.list->data());
	listOf(!inMain).erase(listOf(!inMain).begin() + static_cast<std::ptrdiff_t>(otherPos));
	listOf(inMain).erase(listOf(inMain).begin() + static_cast<std::ptrdiff_t>(position));
	moved.section = kMain;
	moved.x = x;
	moved.y = y;
	swapped.section = kMain;
	swapped.x = static_cast<std::uint8_t>(ox);
	swapped.y = static_cast<std::uint8_t>(oy);
	listOf(!inMain).push_back(std::move(moved));
	listOf(inMain).push_back(std::move(swapped));
	return {};
}

std::string insertAt(state::Inventory& inventory, const Rules& rules, Entry entry, std::uint8_t section, std::uint8_t x, std::uint8_t y)
{
	View dest = mainView(inventory);
	if (section == kBag)
	{
		if (!bagView(inventory, rules, dest) || !canHoldInBag(rules, entry))
			return "no_room";
	} else if (section != kMain) {
		return "bad_value";
	}
	const Size fp = rules.footprint(entry.itemSid);
	if (x + fp.w > dest.w || y + fp.h > dest.h)
		return "no_room";
	const std::uint32_t cap = viewCap(rules, dest, entry.itemSid);
	const std::vector<Entry*> hit = overlapping(dest, rules, x, y, fp, nullptr);
	if (hit.empty())
	{
		if (entry.count > cap)
			return "no_room";
		entry.slot = state::Inventory::kCarried;
		entry.section = kMain;
		entry.x = x;
		entry.y = y;
		dest.list->push_back(std::move(entry));
		return {};
	}
	if (hit.size() == 1 && hit[0]->sameKind(entry) && hit[0]->count + entry.count <= cap)
	{
		hit[0]->takePlantedFrom(entry, entry.count);
		hit[0]->count += entry.count;
		return {};
	}
	return "no_room";
}

std::string arrange(state::Inventory& inventory, const Rules& rules, std::uint8_t section, bool* changed)
{
	if (changed != nullptr)
		*changed = false;
	View view = mainView(inventory);
	// INVENTORY_GRID_LIVE (e): the exe rearranges only "main"; arranging the worn backpack (section 1) is OUR extension (MINE).
	if (section == kBag)
	{
		if (!bagView(inventory, rules, view))
			return "bad_target";
	} else if (section != kMain) {
		return "bad_value";
	}
	std::vector<Entry> snapshot;
	for (const Entry& entry : *view.list)
	{
		if (inGrid(entry))
			snapshot.push_back(entry);
	}
	// The original's order: by x, then y of the current position (a std::set<SectionItem>), the sort then keeps ties in that order.
	std::stable_sort(snapshot.begin(), snapshot.end(), [](const Entry& a, const Entry& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
	for (int mode = 0; mode < 3; ++mode)
	{
		const auto key = [&](const Entry& entry)
		{
			const Size fp = rules.footprint(entry.itemSid);
			return mode == 0 ? std::max(fp.w, fp.h) : mode == 1 ? fp.w * fp.h : fp.w;
		};
		std::vector<Entry> order = snapshot;
		std::stable_sort(order.begin(), order.end(), [&](const Entry& a, const Entry& b) { return key(a) > key(b); });
		std::vector<Entry> placed;
		const View scratch{&placed, view.w, view.h, view.bag};
		bool fits = true;
		for (const Entry& entry : order)
		{
			if (addTo(scratch, rules, entry) != 0)
			{
				fits = false;
				break;
			}
		}
		if (!fits)
			continue;
		view.list->erase(std::remove_if(view.list->begin(), view.list->end(), [](const Entry& entry) { return inGrid(entry); }), view.list->end());
		for (Entry& entry : placed)
			view.list->push_back(std::move(entry));
		if (changed != nullptr)
			*changed = true;
		return {};
	}
	return {}; // none of the three fits: the section stays as it was
}

std::vector<Entry> takeFrom(state::Inventory& inventory, const std::size_t index, std::uint32_t count)
{
	std::vector<Entry> taken;
	Entry* entry = entryAt(inventory, index);
	if (entry == nullptr || entry->slot != state::Inventory::kCarried || count == 0 || entry->count < count)
		return taken;
	Entry piece = *entry;
	piece.count = count;
	piece.section = Entry::kUnplaced;
	if (count < entry->count)
		piece.contents.clear();
	piece.planted.clear();
	piece.takePlantedFrom(*entry, count);
	taken.push_back(std::move(piece));
	if (count == entry->count)
	{
		if (index < inventory.items.size())
			inventory.items.erase(inventory.items.begin() + static_cast<std::ptrdiff_t>(index));
		else
		{
			std::vector<Entry>& list = inventory.wornBackpack()->contents;
			list.erase(list.begin() + static_cast<std::ptrdiff_t>(index - inventory.items.size()));
		}
	} else {
		entry->count -= count;
	}
	return taken;
}

} // namespace ksim::grid
