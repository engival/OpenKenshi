// Grid inventory orders 64-68, equip and the settled view (SIMLINK.md sections 6 and 7; rules in inventory_grid.hpp).

#include "ksim/core/simulation.hpp"
#include "ksim/law/crime.hpp"

#include "ksim/economy/inventory_grid.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace ksim {

namespace {

struct Owner
{
	flecs::entity entity;
	bool character = false;
};

// Why an item instance cannot be built into equipped numbers (CombatData::buildWeapon / buildArmour would throw), "" when it can:
// an instance nobody gave a quality, or one naming a manufacturer or model the data does not have. A command must be refused for it,
// never crash the server.
std::string equipBlocker(const kdata::GameData& gameData, const kdata::Record& record, const state::ItemQuality& quality)
{
	if (record.type == kworld::itemTypeByName("CONTAINER"))
		return {}; // a backpack has no combat numbers
	if (!quality.set())
		return "no quality";
	if (record.type != kworld::itemTypeByName("WEAPON"))
		return {};
	if (!quality.maker.empty() && gameData.find(quality.maker) == nullptr)
		return "unknown manufacturer " + quality.maker;
	if (!quality.model.empty() && gameData.find(quality.model) == nullptr)
		return "unknown model " + quality.model;
	return {};
}

constexpr std::int32_t kBackpackAttach = 12; // AttachSlot BACKPACK

// The inventory slot a wearable record goes in.
std::uint8_t slotOf(const kdata::Record& record)
{
	return record.type == kworld::itemTypeByName("WEAPON") ? state::Inventory::kWeapon
		: static_cast<std::uint8_t>(state::Inventory::kArmourBase + record.intOr("slot", 7));
}

bool wearable(const kdata::Record& record)
{
	return record.type == kworld::itemTypeByName("WEAPON") || record.type == kworld::itemTypeByName("ARMOUR")
		|| (record.type == kworld::itemTypeByName("CONTAINER") && record.intOr("slot", 7) == kBackpackAttach);
}

} // namespace

state::Inventory Simulation::settledInventory(std::uint64_t ownerId) const
{
	flecs::entity entity;
	if (combat_ != nullptr && combat_->find(ownerId) != nullptr)
		entity = fighterEntity(ownerId);
	else if (const Storage storage = findStorage(ownerId); storage.entity != 0)
		entity = flecs::entity(world_.ecs().c_ptr(), storage.entity);
	const state::Inventory* held = entity.is_valid() ? entity.get<state::Inventory>() : nullptr;
	if (held == nullptr)
		return {};
	state::Inventory copy = *held;
	if (gameData_ != nullptr)
		grid::settle(copy, grid::Rules(*gameData_), ownerId);
	return copy;
}

namespace
{

double recordNumber(const kdata::Record& record, std::string_view key, double fallback)
{
	if (const kdata::FieldFloat* field = record.findFloat(key))
		return kdata::decodeFloat(*field);
	if (const kdata::FieldInt* field = record.findInt(key))
		return field->value;
	return fallback;
}

} // namespace

Simulation::ItemStats Simulation::itemStats(const state::Inventory::Entry& entry) const
{
	ItemStats stats;
	stats.quality = entry.quality;
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(entry.itemSid) : nullptr;
	if (record == nullptr || combat_ == nullptr)
		return stats;
	// The tooltip "Value" (s72 section 4a): gear is the Book's item+0x1f0 with E = 1 (maker, level, type; condition is not priced); any other
	// item keeps the record's `value`.
	const bool priced = record->type == kworld::itemTypeByName("WEAPON") || record->type == kworld::itemTypeByName("ARMOUR") || record->type == kworld::itemTypeByName("CROSSBOW");
	stats.value = priced ? static_cast<float>(tradeBook().quote(trade::Terms(), entry).buy) : static_cast<float>(record->intOr("value", 0));
	const bool weapon = record->type == kworld::itemTypeByName("WEAPON");
	const bool armour = record->type == kworld::itemTypeByName("ARMOUR");
	if ((!weapon && !armour) || !equipBlocker(*gameData_, *record, entry.quality).empty())
		return stats; // UNSURE: an instance without a quality is answered as kind 0 (nothing to show), never refused
	if (weapon)
	{
		stats.kind = 2;
		stats.weapon = combat_->data().buildWeapon(*gameData_, *record, entry.quality);
		return stats;
	}
	stats.kind = 1;
	stats.armour = combat_->data().buildArmour(*record, entry.quality);
	stats.armourWeight = static_cast<float>(recordNumber(*record, "weight kg", 0.0)); // UNSURE: no maker / material factor for armour
	stats.armourClass = static_cast<std::uint8_t>(std::clamp(record->intOr("class", 0), 0, 3));
	stats.material = static_cast<std::uint8_t>(std::clamp(record->intOr("material type", 0), 0, 3));
	// UNSURE (ITEM_STATS (g), kinspect s66_static.md part B): the exe's loader scales stealth / athletics / combat speed by a material /
	// class table (FUN_1408a85b0, DAT_142134d20 / DAT_142134ce0) that the sim does not model, forces athletics 1.0 for weather-protection
	// types 5 / 8 / 3 and combat speed 1.0 for 3, and zeroes the melee bonuses for type 6. Only the stealth clamp (< 0 -> 1.0) is applied
	// here; the rest are the record's raw values (combatSpeedMult, attackBonus and defenceBonus are what the fight uses, equally raw).
	const double stealth = recordNumber(*record, "stealth mult", 1.0);
	stats.stealthMult = static_cast<float>(stealth < 0.0 ? 1.0 : stealth);
	stats.athleticsMult = static_cast<float>(recordNumber(*record, "athletics mult", 1.0));
	stats.dexterityMult = static_cast<float>(recordNumber(*record, "dexterity mult", 1.0));
	return stats;
}

void Simulation::dropPieces(std::uint64_t who, std::vector<state::Inventory::Entry> pieces)
{
	const state::Fighter& f = *combat_->find(who);
	// One ground pile per kind: stacks of a kind that the pack split (a cap of 1 makes every unit a stack) lie together.
	std::vector<state::Inventory::Entry> piles;
	for (state::Inventory::Entry& piece : pieces)
	{
		const auto same = std::find_if(piles.begin(), piles.end(), [&](const state::Inventory::Entry& pile) { return pile.sameKind(piece) && pile.quality == piece.quality && pile.contents.empty() && piece.contents.empty(); });
		if (same != piles.end())
			same->count += piece.count;
		else
			piles.push_back(std::move(piece));
	}
	for (state::Inventory::Entry& piece : piles)
	{
		state::GroundItem ground;
		ground.itemSid = piece.itemSid;
		ground.count = piece.count;
		ground.quality = piece.quality;
		ground.material = piece.material;
		ground.colour = piece.colour;
		ground.stolen = piece.stolen;
		ground.contents = std::move(piece.contents);
		ground.x = ground.useX = f.x;
		ground.y = ground.useY = f.y;
		ground.z = ground.useZ = f.z;
		spawnGroundItem(std::move(ground));
		if (interactObserver_)
		{
			InteractEvent dropped;
			dropped.kind = InteractEvent::Kind::Dropped;
			dropped.who = who;
			dropped.item = piece.itemSid;
			dropped.x = f.x;
			dropped.y = f.y;
			dropped.z = f.z;
			interactObserver_(dropped);
		}
	}
}

// A limb was severed (rwh42): the race's `severed limbs` item for the slot lies at the body, owned by nobody, not stolen, one of it, unless the sever was the
// eat-alive path's (`item` false). The first entry of the slot with an item wins; a race without the list drops nothing. OURS: the exe puts it at the
// part's bone (`bone name 2`), we have no bones and use the character's position, which is the exe's own fallback. The item's 24 h `timeout` field,
// which nothing reads, is not modelled.
void Simulation::onLimbSevered(std::uint64_t who, const std::string& part, bool item)
{
	const state::Fighter* found = combat_->find(who);
	if (found == nullptr || gameData_ == nullptr)
		return;
	const state::Fighter& f = *found;
	// Limb slot as the RACE `severed limbs` v0 numbers it: 0 left arm, 1 right arm, 2 left leg, 3 right leg.
	const bool left = part.find("Left") != std::string::npos;
	const int slot = part.find("Arm") != std::string::npos ? (left ? 0 : 1) : (left ? 2 : 3);
	if (const kdata::Record* race = item ? gameData_->find(f.raceSid) : nullptr)
	{
		if (const kdata::RefCategory* limbs = race->findRefs("severed limbs"))
		{
			for (const kdata::RefEntry& entry : limbs->entries)
			{
				if (entry.v0 != slot || entry.targetID.empty())
					continue;
				state::GroundItem ground;
				ground.itemSid = entry.targetID;
				// The tint the exe fits to a type-0x11 limb (1400ced10): skin = 1 - the victim's Skin Tone, blood = the race's colour.
				// UNSURE: our skin tone is one number, so the skin is grey; no bone sizes, so the scale is 1.
				const kdata::Record* limbRecord = gameData_->find(entry.targetID);
				if (limbRecord != nullptr && limbRecord->intOr("item function", 0) == 0x11)
				{
					const RaceCombat* raceData = combat_->data().race(f.raceSid);
					const std::int32_t blood = raceData != nullptr ? raceData->bloodColour : 0x800000;
					ground.tint.set = true;
					ground.tint.skinR = ground.tint.skinG = ground.tint.skinB = 1.0 - f.look.skin;
					ground.tint.bloodR = static_cast<double>((blood >> 16) & 0xff) / 255.0;
					ground.tint.bloodG = static_cast<double>((blood >> 8) & 0xff) / 255.0;
					ground.tint.bloodB = static_cast<double>(blood & 0xff) / 255.0;
				}
				ground.x = ground.useX = f.x;
				ground.y = ground.useY = f.y;
				ground.z = ground.useZ = f.z;
				spawnGroundItem(std::move(ground));
				break;
			}
		}
	}
	// Both legs gone: the boots (AttachSlot 9) fall off.
	int legs = 0;
	for (const state::FighterPart& p : f.parts)
		legs += p.severed() && p.name.find("Leg") != std::string::npos && p.name.find("Foreleg") == std::string::npos ? 1 : 0;
	if (legs < 2)
		return;
	flecs::entity actor = fighterEntity(who);
	const state::Inventory* held = actor.get<state::Inventory>();
	if (held == nullptr)
		return;
	state::Inventory inventory = *held;
	std::vector<state::Inventory::Entry> boots;
	for (auto it = inventory.items.begin(); it != inventory.items.end();)
	{
		if (it->slot == state::Inventory::kArmourBase + 9)
		{
			boots.push_back(std::move(*it));
			it = inventory.items.erase(it);
		} else {
			++it;
		}
	}
	if (boots.empty())
		return;
	for (const state::Inventory::Entry& piece : boots)
		unequipFromFighter(who, piece.itemSid);
	++inventory.version;
	actor.set<state::Inventory>(std::move(inventory));
	dropPieces(who, std::move(boots));
}

void Simulation::storeInventory(flecs::entity entity, std::uint64_t id, state::Inventory inventory, std::vector<state::Inventory::Entry> overflow)
{
	entity.set<state::Inventory>(std::move(inventory));
	if (overflow.empty())
		return;
	if (combat_ != nullptr && combat_->find(id) != nullptr)
	{
		dropPieces(id, std::move(overflow));
		return;
	}
	for (const state::Inventory::Entry& entry : overflow)
		std::cerr << "inventory of " << id << ": " << entry.count << " x " << entry.itemSid << " fit nowhere and are lost\n";
}

std::string Simulation::checkInventoryOrder(const InventoryOrder& order) const
{
	// A judging run changes nothing (commit false stores and spawns nothing), so the const_cast is only for the shared code path.
	return const_cast<Simulation*>(this)->runInventoryOrder(order, false);
}

// Wearing a piece (orders 67, and the quick move of 68 and 63): `worn` (one unit, already out of the pack) takes the slot its record
// names, the piece in that slot goes back to the pack, and on commit the fighter's weapon / armour numbers are rebuilt from the
// instance's quality.
std::string Simulation::wearOnto(state::Inventory& target, state::Inventory::Entry worn, const kdata::Record& record, std::uint64_t id, bool commit)
{
	const grid::Rules rules(*gameData_);
	const bool weapon = record.type == kworld::itemTypeByName("WEAPON");
	const bool bag = record.type == kworld::itemTypeByName("CONTAINER");
	const std::int32_t slot = record.intOr("slot", 7);
	const std::uint8_t inventorySlot = slotOf(record);
	const state::ItemQuality quality = worn.quality;
	// Equipping rebuilds the numbers from the instance's quality: one that cannot be built is refused (and named in the log).
	const std::string blocker = equipBlocker(*gameData_, record, quality);
	if (!blocker.empty())
	{
		if (!commit)
			std::cerr << "equip refused: " << record.name << " (" << worn.itemSid << "): " << blocker << "\n";
		return "not_allowed";
	}
	worn.slot = inventorySlot;
	worn.section = state::Inventory::Entry::kEquipped;
	// Whatever sits in that slot goes back to the pack (a weapon's slot also frees when the hands are bare: nothing there).
	for (auto it = target.items.begin(); it != target.items.end(); ++it)
	{
		if (it->slot != inventorySlot)
			continue;
		state::Inventory::Entry old = std::move(*it);
		target.items.erase(it);
		old.slot = state::Inventory::kCarried;
		old.section = state::Inventory::Entry::kUnplaced;
		if (grid::autoplace(target, rules, std::move(old)) > 0)
			return "no_room";
		break;
	}
	target.items.push_back(std::move(worn));
	if (commit)
	{
		state::Fighter& f = *combat_->find(id);
		if (weapon)
		{
			f.weapon = combat_->data().buildWeapon(*gameData_, record, quality);
		} else if (!bag) {
			state::FighterArmour built = combat_->data().buildArmour(record, quality);
			f.armour.erase(std::remove_if(f.armour.begin(), f.armour.end(), [&](const state::FighterArmour& p) { return p.slot == slot; }), f.armour.end());
			const auto at = std::find_if(f.armour.begin(), f.armour.end(), [&](const state::FighterArmour& other) { return other.slot > built.slot; });
			f.armour.insert(at, std::move(built));
		}
	}
	return {};
}

// A character takes `piece` in the original's quick-move order (orders 68 and 63 into a character with section 255): a free matching worn
// slot (the equip path of order 67), then main, then the worn backpack. "" or "no_room"; `dest` is the character's settled inventory.
std::string Simulation::quickMoveInto(state::Inventory& dest, state::Inventory::Entry piece, std::uint64_t id, bool commit)
{
	const grid::Rules rules(*gameData_);
	const kdata::Record* record = gameData_->find(piece.itemSid);
	const state::Fighter& me = *combat_->find(id);
	const bool busy = me.state == state::CombatState::Attack || me.state == state::CombatState::Defend || me.state == state::CombatState::Stumble;
	const bool slotFree = record != nullptr && wearable(*record) && piece.count == 1 && !busy
		&& std::none_of(dest.items.begin(), dest.items.end(), [&](const state::Inventory::Entry& e) { return e.slot == slotOf(*record); });
	state::Inventory trial = dest;
	if (slotFree && wearOnto(trial, piece, *record, id, commit).empty())
	{
		dest = std::move(trial);
		return {};
	}
	state::Inventory::Entry rest = piece;
	rest.count = grid::autoplace(dest, rules, piece);
	if (rest.count > 0 && grid::autoplace(dest, rules, rest, 1) > 0)
		return "no_room";
	return {};
}

std::string Simulation::runInventoryOrder(const InventoryOrder& o, bool commit)
{
	if (gameData_ == nullptr || combat_ == nullptr)
		return "bad_target";
	const grid::Rules rules(*gameData_);
	const auto resolve = [&](std::uint64_t id) -> Owner
	{
		Owner owner;
		if (combat_->find(id) != nullptr)
		{
			owner.entity = fighterEntity(id);
			owner.character = true;
		} else if (const Storage storage = findStorage(id); storage.entity != 0) {
			owner.entity = ecsMutable().entity(storage.entity);
		}
		return owner;
	};
	const Owner owner = resolve(o.id);
	if (!owner.entity.is_valid() || owner.entity.get<state::Inventory>() == nullptr)
		return "unknown_id";
	if (owner.character)
	{
		const state::Fighter& f = *combat_->find(o.id);
		if (f.dead || f.unconscious)
		{
			// A body: orders 64 / 65 arrange its pack like a container's, for a conscious player character within the use range
			// (they carry no actor, so any of them will do). Drop, equip and the character side of 68 stay for characters that are up.
			if (o.kind != 64 && o.kind != 65)
				return "not_allowed";
			const Storage body = findStorage(o.id);
			bool near = false;
			for (const CombatArena::Member& m : combat_->members())
				near = near || (m.fighter.playerControlled && !m.fighter.dead && !m.fighter.unconscious && m.id != o.id && inStorageReach(m.fighter, body));
			if (!near)
				return "out_of_reach";
		}
	}
	// A producer's slot cannot be rearranged or filled, nor a merchant's stock (trade.hpp): orders 64 / 65 are not_allowed on them.
	if ((o.kind == 64 || o.kind == 65) && !owner.character && (findStorage(o.id).producer || findStorage(o.id).trader))
		return "not_allowed";
	// Characters only: drop, equip, and the character side of a transfer.
	if ((o.kind == 66 || o.kind == 67 || o.kind == 68) && !owner.character)
		return "bad_target";

	state::Inventory inv = *owner.entity.get<state::Inventory>();
	std::vector<state::Inventory::Entry> overflow = grid::settle(inv, rules, o.id);
	const auto stale = [&](const state::Inventory& checked) { return o.version != InventoryOrder::kAnyVersion && o.version != checked.version; };

	switch (o.kind)
	{
	case 64:
	case 65:
	{
		if (stale(inv))
			return "stale";
		bool changed = true;
		const std::string reason = o.kind == 64 ? grid::moveEntry(inv, rules, o.index, o.section, o.x, o.y, o.count) : grid::arrange(inv, rules, o.section, &changed);
		if (!reason.empty())
			return reason;
		if (commit)
		{
			if (changed)
				++inv.version;
			storeInventory(owner.entity, o.id, std::move(inv), std::move(overflow));
		}
		return {};
	}
	case 66:
	{
		if (stale(inv))
			return "stale";
		const state::Inventory::Entry* entry = grid::entryAt(inv, o.index);
		if (entry == nullptr)
			return "bad_value";
		if (entry->slot != state::Inventory::kCarried || o.count == 0 || o.count > entry->count)
			return "not_allowed"; // an equipped entry is not droppable, nor more than the stack holds
		if (!commit)
			return {};
		std::vector<state::Inventory::Entry> pieces = grid::takeFrom(inv, o.index, o.count);
		++inv.version;
		storeInventory(owner.entity, o.id, std::move(inv), std::move(overflow));
		dropPieces(o.id, std::move(pieces));
		return {};
	}
	case 67:
	{
		if (stale(inv))
			return "stale";
		state::Fighter& f = *combat_->find(o.id);
		if (f.state == state::CombatState::Attack || f.state == state::CombatState::Defend || f.state == state::CombatState::Stumble)
			return "not_allowed"; // the clip and technique belong to the old weapon (UNSURE: the original equips in its own time)
		const bool equip = o.flag != 0;
		std::size_t index = o.index;
		if (o.index == InventoryOrder::kByRecord)
		{
			const std::ptrdiff_t found = equip ? grid::firstCarried(inv, o.item) : grid::firstEquipped(inv, o.item);
			if (found < 0)
				return "not_allowed";
			index = static_cast<std::size_t>(found);
		}
		const state::Inventory::Entry* entry = grid::entryAt(inv, index);
		if (entry == nullptr)
			return "bad_value";
		const kdata::Record* record = gameData_->find(entry->itemSid);
		const std::int32_t weaponType = kworld::itemTypeByName("WEAPON");
		const std::int32_t armourType = kworld::itemTypeByName("ARMOUR");
		const std::int32_t containerType = kworld::itemTypeByName("CONTAINER");
		const bool bag = record != nullptr && record->type == containerType && record->intOr("slot", 7) == kBackpackAttach;
		if (record == nullptr || (record->type != weaponType && record->type != armourType && !bag))
			return "bad_target";
		if (equip ? entry->slot != state::Inventory::kCarried : entry->slot == state::Inventory::kCarried)
			return "not_allowed";
		const std::string itemSid = entry->itemSid;
		if (equip)
		{
			std::vector<state::Inventory::Entry> taken = grid::takeFrom(inv, index, 1);
			const std::string reason = wearOnto(inv, std::move(taken.front()), *record, o.id, commit);
			if (!reason.empty())
				return reason;
		} else {
			if (index >= inv.items.size())
				return "not_allowed";
			state::Inventory::Entry old = std::move(inv.items[index]);
			inv.items.erase(inv.items.begin() + static_cast<std::ptrdiff_t>(index));
			old.slot = state::Inventory::kCarried;
			old.section = state::Inventory::Entry::kUnplaced;
			if (grid::autoplace(inv, rules, std::move(old)) > 0)
				return "no_room";
			if (commit)
				unequipFromFighter(o.id, itemSid);
		}
		if (commit)
		{
			++inv.version;
			storeInventory(owner.entity, o.id, std::move(inv), std::move(overflow));
		}
		return {};
	}
	case 68:
	{
		if (o.count == 0)
			return "bad_value";
		const Storage target = findStorage(o.container);
		if (target.entity == 0 || o.container == o.id)
			return "bad_target"; // no such container, or a character that is not down
		const state::Fighter& f = *combat_->find(o.id);
		if (o.source != 0 && (!target.trader || o.flag != 0))
			return "bad_value"; // a container source is for selling to a merchant
		if (target.trader)
		{
			// A merchant's stock: the parties and the range are the trade's (trade.hpp), not the container rule.
			const std::string why = tradeParties(o, target);
			if (!why.empty())
				return why;
		} else if (!inStorageReach(f, target)) {
			return "out_of_reach";
		}
		const flecs::entity box = ecsMutable().entity(target.entity);
		if (box.get<state::Inventory>() == nullptr)
			return "not_allowed";
		// A locked box takes and gives nothing until it is picked, as in order 63 (SIMLINK section 13, the Lock bullet).
		if (const state::Container* held = box.get<state::Container>(); held != nullptr && held->locked)
			return "locked";
		const bool take = o.flag != 0;
		state::Inventory other = *box.get<state::Inventory>();
		std::vector<state::Inventory::Entry> otherOverflow = grid::settle(other, rules, o.container);
		// MQ12: a sale from an owned container (tradeParties judged it): its inventory is the source, in the actor's place.
		flecs::entity holder = owner.entity;
		std::uint64_t holderId = o.id;
		if (o.source != 0)
		{
			holder = ecsMutable().entity(findStorage(o.source).entity);
			holderId = o.source;
			inv = *holder.get<state::Inventory>();
			overflow = grid::settle(inv, rules, holderId);
		}
		state::Inventory& source = take ? other : inv;
		state::Inventory& dest = take ? inv : other;
		if (stale(source))
			return "stale";
		const state::Inventory::Entry* entry = grid::entryAt(source, o.index);
		if (entry == nullptr)
			return "bad_value";
		// Stripping: from a body, an equipped (top-level) entry may be taken whole; it leaves the body's hands or back.
		const bool strip = take && target.body && entry->slot != state::Inventory::kCarried && o.index < source.items.size();
		if ((entry->slot != state::Inventory::kCarried && !strip) || o.count > entry->count)
			return "not_allowed";
		// A producer's output slot takes back only the item it produces (kenshi-k16.19.13, item59 MQ3); the stack cap and the slot's room
		// are the grid's (the put-back raises the same stack the stop-at-full reads).
		if (target.producer && !take)
		{
			const state::Producer* machine = box.get<state::Producer>();
			if (machine == nullptr || entry->itemSid != machine->output)
				return "not_allowed";
		}
		std::uint32_t count = o.count; // a trade with a short wallet moves fewer
		Deal deal;
		if (target.trader)
		{
			const std::string why = tradeTerms(o, target, *entry, take, count, deal);
			if (!why.empty())
				return why;
		}
		std::string stripped;
		std::vector<state::Inventory::Entry> pieces;
		if (strip)
		{
			stripped = entry->itemSid;
			state::Inventory::Entry worn = std::move(source.items[o.index]);
			source.items.erase(source.items.begin() + static_cast<std::ptrdiff_t>(o.index));
			worn.slot = state::Inventory::kCarried;
			worn.section = state::Inventory::Entry::kUnplaced;
			pieces.push_back(std::move(worn));
		} else {
			pieces = grid::takeFrom(source, o.index, count);
		}
		state::Inventory::Entry piece = std::move(pieces.front());
		// Provenance (ARCHITECTURE.md "Item provenance"): a take that is theft marks the stack with the victim faction (an earlier mark stays);
		// a put into another character's pack is a planting, which supersedes the units' earlier planters.
		const bool theft = take && isForeignOwner(world_.ecs(), o.id, target.ownerSid);
		if (theft && piece.stolen.empty())
			piece.stolen = target.ownerSid;
		const bool plant = !take && target.body && !target.trader && o.container != o.id;
		const std::string soldStolen = piece.stolen; // the fencing crime reads the mark the stack arrived with
		// The mark clears where it comes home or is laundered (dt43 D section 4): into the victim faction's own box or body or pack, into shop stock.
		clearReturnedMark(piece, take ? factionOf(world_.ecs(), o.id) : target.ownerSid, target.trader && !take);
		const std::uint32_t plantedUnits = piece.count;
		const std::string entryItem = piece.itemSid;
		if (plant)
			piece.planted = {{plantedUnits, o.id, clockTick()}};
		std::string reason;
		if (o.section != 255)
		{
			reason = grid::insertAt(dest, rules, piece, o.section, o.x, o.y);
		} else if (!take) {
			if (grid::autoplace(dest, rules, piece) > 0)
				reason = "no_room"; // a container takes main only
		} else {
			reason = quickMoveInto(dest, std::move(piece), o.id, commit);
		}
		if (!reason.empty())
			return reason;
		if (commit)
		{
			// The GUI window take rolls the steal chance (FUN_140713d20 -> 140794810); a caught take moves nothing and the order is spent.
			if (take && stealRollCaught(o.id, o.container, entryItem, target))
				return {};
			++inv.version;
			++other.version;
			storeInventory(holder, holderId, std::move(inv), std::move(overflow));
			storeInventory(box, o.container, std::move(other), std::move(otherOverflow));
			if (!stripped.empty())
				unequipFromFighter(o.container, stripped);
			if (theft)
				reportTheft(o.id, o.container, target.ownerSid);
			if (plant)
				firePlanted(o.id, o.container, entryItem);
			// Fencing (140793770 L45): a stolen stack sold to a merchant. UNSURE: the exe has the trader notice it; ours always does. The victim
			// is the faction the stack was stolen from, the timer 30 s, and the trader fires event 8 (thief caught stealing from me).
			if (target.trader && !take && !soldStolen.empty())
			{
				reportCrime(o.id, crime::kFencing, soldStolen, 0, crime::kTimerTheftReport);
				fireDialogue(o.trader, o.id, crime::kEvThiefCaughtStealingFromMe);
			}
			if (target.trader)
				tradeSettle(deal);
		}
		return {};
	}
	default:
		return "bad_value";
	}
}

void Simulation::firePlanted(std::uint64_t planter, std::uint64_t holder, const std::string& itemSid)
{
	// The "planted" event (planter, holder, item): on the task log until Lua is wired.
	std::ostringstream line;
	line << "planted " << planter << " " << holder << " " << itemSid;
	logTask(line.str());
}

void Simulation::unequipFromFighter(std::uint64_t id, const std::string& itemSid)
{
	state::Fighter& f = *combat_->find(id);
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(itemSid) : nullptr;
	if (record == nullptr)
		return;
	if (record->type == kworld::itemTypeByName("WEAPON"))
		f.weapon = combat_->data().unarmed;
	else if (record->type != kworld::itemTypeByName("CONTAINER"))
		f.armour.erase(std::remove_if(f.armour.begin(), f.armour.end(), [&](const state::FighterArmour& p) { return p.sid == itemSid; }), f.armour.end());
}

// Order 62 (by record): the first matching entry.
std::string Simulation::checkEquip(std::uint64_t who, const std::string& item, bool equip) const
{
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(item) : nullptr;
	const std::int32_t type = record != nullptr ? record->type : -1;
	if (type != kworld::itemTypeByName("WEAPON") && type != kworld::itemTypeByName("ARMOUR")
		&& !(type == kworld::itemTypeByName("CONTAINER") && record->intOr("slot", 7) == kBackpackAttach))
		return "bad_target";
	InventoryOrder order;
	order.kind = 67;
	order.id = who;
	order.version = InventoryOrder::kAnyVersion;
	order.index = InventoryOrder::kByRecord;
	order.flag = equip ? 1 : 0;
	order.item = item;
	const std::string reason = checkInventoryOrder(order);
	// The old order has no room or id reasons: a pack with no room for the swapped piece is "not_allowed" to it.
	if (reason == "unknown_id")
		return "bad_target";
	return reason == "no_room" ? "not_allowed" : reason;
}

void Simulation::equipItem(std::uint64_t who, const std::string& item, bool equip)
{
	InventoryOrder order;
	order.kind = 67;
	order.id = who;
	order.version = InventoryOrder::kAnyVersion;
	order.index = InventoryOrder::kByRecord;
	order.flag = equip ? 1 : 0;
	order.item = item;
	runInventoryOrder(order, true);
	// The order rebuilds the arena's fighter (weapon / armour numbers); the saved component follows, or a save taken now keeps the old gear
	// and a load runs the weapon the live fighter already put away (ksim_shopping).
	mirrorFighter(who);
}

} // namespace ksim
