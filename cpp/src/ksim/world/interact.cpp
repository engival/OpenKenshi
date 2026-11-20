#include "ksim/world/interact.hpp"
#include "ksim/economy/inventory_grid.hpp"

#include "knav/door.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/prospect.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trade.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <tuple>

namespace ksim {

bool isTargetDown(const state::Fighter& f)
{
	return f.unconscious || f.dead || f.carriedBy != 0 || f.cageBuilding != 0 || f.down || f.crouched;
}

InteractTarget findInteractTarget(const flecs::world& ecs, const CombatArena* arena, std::uint64_t id, const Simulation* sim)
{
	InteractTarget target;
	// A bed (order 60 action 9): a real layout bed's site id (routines::siteId, 2^62 + the site key). The site index lives on
	// the Simulation, so a caller without one cannot resolve it. A producer's id is also 2^62 + an index (producers::kIdBase), so
	// an id that is no bed falls through to the entity lookups below.
	if (sim != nullptr && (id & routines::kSiteIdFlag) != 0)
	{
		if (const routines::SiteEntry* bed = sim->bedOf(id))
		{
			target.kind = InteractKind::Bed;
			target.id = id;
			target.x = bed->x;
			target.y = bed->y;
			target.z = bed->z;
			target.reach = kReachBed;
			target.reachStopped = 2.0 * kReachBed;
			return target;
		}
	}
	if (arena != nullptr)
	{
		if (const state::Fighter* fighter = arena->find(id))
		{
			target.kind = InteractKind::Character;
			target.id = id;
			target.x = fighter->x;
			target.y = fighter->y;
			target.z = fighter->z;
			const RaceCombat* race = arena->data().race(fighter->raceSid);
			const double radius = race != nullptr ? race->footprintRadius : 2.6;
			const bool down = fighter->dead || fighter->unconscious;
			target.reach = characterReach(radius, down, false);
			target.reachStopped = characterReach(radius, down, true);
			return target;
		}
	}
	ecs.each([&](flecs::entity entity, const state::Id& instance, const state::Door& door)
	{
		if (instance.value != id)
			return;
		target = {InteractKind::Door, entity.id(), id, door.x, door.y, door.z, door.axisX, door.axisZ, door.halfLong, door.halfDeep};
	});
	if (target.kind != InteractKind::None)
		return target;
	ecs.each([&](flecs::entity entity, const state::Id& instance, const state::GroundItem& item)
	{
		if (instance.value != id)
			return;
		target = {InteractKind::GroundItem, entity.id(), id, item.useX, item.useY, item.useZ};
		target.reach = kReachGroundItem;
		target.reachStopped = 2.0 * kReachGroundItem;
	});
	if (target.kind != InteractKind::None)
		return target;
	ecs.each([&](flecs::entity entity, const state::Id& instance, const state::Container& container)
	{
		if (instance.value != id)
			return;
		target = {InteractKind::Container, entity.id(), id, container.useX, container.useY, container.useZ};
		target.reach = kReachStorage;
		target.reachStopped = 2.0 * kReachStorage;
	});
	if (target.kind != InteractKind::None)
		return target;
	ecs.each([&](flecs::entity entity, const state::Id& instance, const state::Producer& producer)
	{
		if (instance.value != id)
			return;
		// Reach is to the origin, not the use point (MQ9 / MQ16: the use point of a deposit is its origin snapped, often onto its own rock).
		target = {InteractKind::Producer, entity.id(), id, producer.x, producer.useY, producer.z};
		target.reach = producer.useRange;
		target.reachStopped = producer.useRange; // no stopped hysteresis for a producer (kenshi-k16.19.5: the original's arrival is one distance < use range)
		target.reachY = kProducerReachY;
		target.boxMinX = producer.boxMinX;
		target.boxMaxX = producer.boxMaxX;
		target.boxMinZ = producer.boxMinZ;
		target.boxMaxZ = producer.boxMaxZ;
	});
	return target;
}

double functionalityReach(const kdata::GameData& data, std::string_view pieceSid, std::int32_t function, bool stopped) noexcept
{
	// dt22 (the s69 pre-task gate): the fitting functionality record's `use range`, the producer's field (producers.cpp). 20 when
	// the piece's record or the field is missing (UNSURE: no data hits that path; the producer default stands in).
	double range = 20.0;
	const kdata::Record* piece = pieceSid.empty() ? nullptr : data.find(std::string(pieceSid));
	if (const kdata::RefCategory* functions = piece != nullptr ? piece->findRefs("functionality") : nullptr)
		for (const kdata::RefEntry& entry : functions->entries)
		{
			const kdata::Record* record = entry.v0 == kdata::kReferenceRemoved ? nullptr : data.find(entry.targetID);
			if (record == nullptr || record->intOr("function", 0) != function)
				continue;
			range = static_cast<double>(record->floatOr("use range", 20.0f));
			break;
		}
	return stopped ? 2.0 * range : range;
}

double bashDamage(double blunt, double cut) noexcept
{
	return std::clamp(2.0 * blunt + 0.5 * cut, 20.0, 60.0) * 0.015;
}

bool isObjectMine(const flecs::world& ecs, std::uint64_t actor, const state::Door& door)
{
	if (door.ownerSid.empty())
		return false;
	bool mine = false;
	ecs.each([&](const state::Id& id, const state::Allegiance& allegiance)
	{
		if (id.value == actor && allegiance.factionSid == door.ownerSid)
			mine = true;
	});
	return mine;
}

bool isForeignOwner(const flecs::world& ecs, std::uint64_t actor, const std::string& ownerSid)
{
	if (ownerSid.empty())
		return false;
	bool member = false;
	ecs.each([&](const state::Id& id, const state::Allegiance& allegiance)
	{
		if (id.value == actor && allegiance.factionSid == ownerSid)
			member = true;
	});
	return !member;
}

std::string factionOf(const flecs::world& ecs, std::uint64_t actor)
{
	std::string sid;
	ecs.each([&](const state::Id& id, const state::Allegiance& allegiance)
	{
		if (id.value == actor)
			sid = allegiance.factionSid;
	});
	return sid;
}

void clearReturnedMark(state::Inventory::Entry& piece, const std::string& holderSid, bool shopStock)
{
	if (shopStock || (!piece.stolen.empty() && piece.stolen == holderSid))
		piece.stolen.clear();
	for (state::Inventory::Entry& inner : piece.contents)
		clearReturnedMark(inner, holderSid, shopStock);
}

bool isForeignContainer(const flecs::world& ecs, std::uint64_t actor, const state::Container& container)
{
	return isForeignOwner(ecs, actor, container.ownerSid);
}

namespace
{

// The CharacterInstance entity of an Id (a talk target: the interact lookup of a character has no entity).
flecs::entity characterWithId(const flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found;
	ecs.each([&](flecs::entity entity, const state::Id& instance, const state::CharacterInstance&)
	{
		if (instance.value == id)
			found = entity;
	});
	return found;
}

} // namespace

std::string checkInteract(const flecs::world& ecs, const CombatArena& arena, std::uint64_t actor, const InteractTarget& target,
	std::uint8_t action, std::uint8_t flags, const kdata::GameData* data, const Simulation* sim)
{
	switch (static_cast<InteractAction>(action))
	{
	case InteractAction::Open:
	case InteractAction::Close:
	case InteractAction::Unlock:
	case InteractAction::Bash:
	{
		if (target.kind != InteractKind::Door)
			return "bad_target";
		const state::Door* door = flecs::entity(ecs.c_ptr(), target.entity).get<state::Door>();
		if (door == nullptr)
			return "bad_target";
		// Open needs a shut, unlocked door; a damaged one still opens (CURSORS_LIVE.md: its RMB is OPEN whatever its cursor).
		// A locked door refuses (it needs Unlock first). A broken door is open for ever: nothing applies to it.
		switch (static_cast<InteractAction>(action))
		{
		case InteractAction::Open:
			return door->state == 1 ? std::string() : "not_allowed";
		case InteractAction::Close:
			if ((flags & kInteractFlagLock) != 0) // the lock order: a shut door of the actor's own faction
				return door->state == 1 && isObjectMine(ecs, actor, *door) ? std::string() : "not_allowed";
			return door->state == 0 ? std::string() : "not_allowed";
		case InteractAction::Unlock:
			// Only the building's faction (isObjectMine, DOORS.md section 6); lockpicking is the others' way (unsupported).
			return door->state == 2 && isObjectMine(ecs, actor, *door) ? std::string() : "not_allowed";
		default:
			return door->state == 1 || door->state == 2 ? std::string() : "not_allowed";
		}
	}
	case InteractAction::PickUp:
		return target.kind == InteractKind::GroundItem ? std::string() : "bad_target";
	case InteractAction::Loot:
	{
		// A storage building: take everything (an empty one is allowed: nothing happens). Foreign storage is theft (event 14). A locked
		// one refuses until it is picked (open with the lock on is the same: no contents on show).
		if (target.kind == InteractKind::Container)
		{
			const state::Container* box = flecs::entity(ecs.c_ptr(), target.entity).get<state::Container>();
			return box != nullptr && box->locked ? "locked" : std::string();
		}
		// A KO'd or dead body. UNSURE: the original's cursor row is a KO'd enemy; any downed character is allowed here.
		if (target.kind != InteractKind::Character)
			return "bad_target";
		const state::Fighter* body = arena.find(target.id);
		if (body == nullptr || target.id == actor)
			return "bad_target";
		if (body->unconscious || body->dead)
			return {};
		return (flags & kInteractFlagOpen) != 0 ? "bad_target" : "not_allowed"; // opening a standing character is no loot at all
	}
	case InteractAction::FirstAid:
	{
		// A living character that bleeds or is hurt. UNSURE: the original's cursor row is a bleeding own character.
		if (target.kind != InteractKind::Character)
			return "bad_target";
		const state::Fighter* patient = arena.find(target.id);
		if (patient == nullptr || patient->dead)
			return "not_allowed";
		bool hurt = !patient->wounds.empty();
		for (const state::FighterPart& part : patient->parts)
			hurt = hurt || part.health < part.maxHealth - 1e-6;
		return hurt ? std::string() : "not_allowed";
	}
	case InteractAction::Lockpick:
	{
		// A locked door or container; any hands (the chance is the exe's, door_plan.hpp). The order is accepted whatever the chance and walks
		// to the lock; a chance below the floor never starts the pick there (stepDoorSteps: no crime, no roll, the order ends refused,
		// kenshi-k16.6.366). UNSURE: what the original's order does instead.
		if (target.kind == InteractKind::Container)
		{
			const state::Container* box = flecs::entity(ecs.c_ptr(), target.entity).get<state::Container>();
			return box != nullptr && box->locked ? std::string() : "not_allowed";
		}
		if (target.kind != InteractKind::Door)
			return "bad_target";
		const state::Door* door = flecs::entity(ecs.c_ptr(), target.entity).get<state::Door>();
		return door != nullptr && door->state == 2 ? std::string() : "not_allowed";
	}
	case InteractAction::Carry:
	{
		// A KO'd or dead character nobody carries, lifted by a standing one with free arms. UNSURE: the original's cursor row is
		// a KO'd character; an ally is allowed (the player's order has no target test, row 225).
		if (target.kind != InteractKind::Character)
			return "bad_target";
		const state::Fighter* body = arena.find(target.id);
		const state::Fighter* actorFighter = arena.find(actor);
		if (body == nullptr || actorFighter == nullptr || target.id == actor)
			return "bad_target";
		// A shackled slave that is down is lifted conscious too (kenshi-k16.6.28: a crawler the squad shot down).
		const bool liftable = (flags & kInteractFlagArrestLift) != 0 ? isTargetDown(*body)
			: body->unconscious || body->dead || (body->down && body->slaveState == 1);
		if (!liftable || body->carriedBy != 0 || actorFighter->carrying != 0 || actorFighter->dead
			|| actorFighter->unconscious || actorFighter->down || actorFighter->carriedBy != 0)
			return "not_allowed";
		return std::string();
	}
	case InteractAction::PutDown:
	{
		if (target.kind != InteractKind::Character)
			return "bad_target";
		const state::Fighter* body = arena.find(target.id);
		const state::Fighter* actorFighter = arena.find(actor);
		if (body == nullptr || actorFighter == nullptr || target.id == actor)
			return "bad_target";
		return actorFighter->carrying == target.id && body->carriedBy == actor && !actorFighter->dead && !actorFighter->unconscious
			? std::string() : "not_allowed";
	}
	case InteractAction::Use:
	{
		// A producer with a free operator slot and room in its output. The actor that already works it keeps its slot.
		if (target.kind != InteractKind::Producer)
			return "bad_target";
		const state::Producer* producer = flecs::entity(ecs.c_ptr(), target.entity).get<state::Producer>();
		if (producer == nullptr)
			return "bad_target";
		if (producer->prodState == 1)
			return "full";
		const bool working = std::find(producer->operators.begin(), producer->operators.end(), actor) != producer->operators.end();
		return working || producer->operators.size() < producer->maxOperators ? std::string() : "no_slot";
	}
	case InteractAction::Talk:
	{
		// M4: a merchant's character opens a trade (trade.hpp); a down, sleeping or hostile one refuses. Any other conscious character is
		// talked to: EV_PLAYER_TALK_TO_ME on arrival (DIALOGUE.md 13; nothing_to_say when no line passes).
		if (target.kind != InteractKind::Character)
			return "bad_target";
		const state::Fighter* who = arena.find(target.id);
		if (who == nullptr || target.id == actor)
			return "bad_target";
		if (who->dead || who->unconscious || who->down || who->asleep)
			return "not_allowed";
		const flecs::entity squad = trade::squadOf(characterWithId(ecs, target.id));
		if (!squad.is_valid() || squad.get<state::TraderStock>() == nullptr || squad.get<state::Inventory>() == nullptr)
			return std::string();
		if (data != nullptr)
		{
			// Hostile: what the merchant's faction thinks of the player faction is at or below the exe's threshold.
			const state::SquadInstance* instance = squad.get<state::SquadInstance>();
			const flecs::entity theirs(ecs.c_ptr(), instance != nullptr ? instance->faction.id : 0);
			flecs::entity player;
			ecs.each([&](flecs::entity entity, const state::PlayerFaction&, const state::FactionState&)
			{
				if (!player.is_valid())
					player = entity;
			});
			if (theirs.is_valid() && player.is_valid() && factionRelation(*data, theirs, player) <= FactionBook::kHostile)
				return "not_allowed";
		}
		return std::string();
	}
	case InteractAction::Sleep:
	{
		// A bed: down / carried / carrying / in a fight and an occupied one refuse (Simulation::sleepRefusal, the same words as
		// the order's own check).
		if (target.kind != InteractKind::Bed)
			return "bad_target";
		return sim != nullptr ? sim->sleepRefusal(actor, target.id) : std::string("unsupported");
	}
	}
	return "bad_value";
}

namespace
{

// The towns with an owner: where a building's faction comes from (the nearest within 1,500 units; UNSURE, the data has no link).
struct OwnedTown
{
	double x, z;
	std::string factionSid;
};

std::vector<OwnedTown> ownedTowns(const flecs::world& ecs)
{
	std::vector<OwnedTown> towns;
	ecs.each([&](const state::TownState& town)
	{
		if (town.owner.id == 0)
			return;
		const state::FactionState* faction = flecs::entity(ecs.c_ptr(), town.owner.id).get<state::FactionState>();
		if (faction != nullptr)
			towns.push_back({town.x, town.z, faction->templateSid});
	});
	return towns;
}

std::string nearestOwner(const std::vector<OwnedTown>& towns, double x, double z)
{
	std::string owner;
	double nearest = 1500.0;
	for (const OwnedTown& town : towns)
	{
		const double far = std::hypot(town.x - x, town.z - z);
		if (far < nearest)
		{
			nearest = far;
			owner = town.factionSid;
		}
	}
	return owner;
}

// splitmix64: the deterministic dice of lockpicking and container contents (no state of their own).
std::uint64_t mix64(std::uint64_t z)
{
	z += 0x9e3779b97f4a7c15ULL;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
	return z ^ (z >> 31);
}

double unit01(std::uint64_t z)
{
	return static_cast<double>(mix64(z) >> 11) * (1.0 / 9007199254740992.0);
}

// The level of a door's lock (LOCKS_EQUIP_STOCK_LIVE.md f, door_plan.hpp rollLockLevel): the faction's `doors lock level` +-
// its `lock level random`, plus the door's `lock level bonus`, rolled once from the door's key. The faction is the BUILDING's
// (GATES_CARRY_LIVE.md b: instance owner faction, else the town's; none = "nofac" 40 / 10), read from the merged data (rebirth.mod
// overrides: the Hub's Holy Nation Outlaws 30 / 15, United Cities 30 / 10, Nameless 75 / 0). A later change of owner never
// re-rolls (the level is on the door, saved).
std::int32_t doorLockLevel(const kdata::GameData* gd, const std::string& ownerSid, const std::string& doorSid, std::uint64_t key)
{
	std::int32_t base = 40; // FACTION `doors lock level` default
	std::int32_t random = 10; // `lock level random` default
	if (gd != nullptr && !ownerSid.empty())
	{
		if (const kdata::Record* faction = gd->find(ownerSid))
		{
			base = faction->intOr("doors lock level", base);
			random = faction->intOr("lock level random", random);
		}
	}
	std::int32_t bonus = 0;
	if (gd != nullptr)
	{
		if (const kdata::Record* door = gd->find(doorSid))
			bonus = door->intOr("lock level bonus", 0);
	}
	return rollLockLevel(base, random, bonus, key);
}

} // namespace

std::size_t spawnMeshDoors(Simulation& sim, const MeshNavigator& navigation)
{
	std::size_t count = 0;
	const std::vector<OwnedTown> towns = ownedTowns(sim.world().ecs());
	const std::shared_ptr<const kdata::GameData> gd = sim.gameData();
	// A door that already has its entity is left alone (a streaming mesh brings zones in again: Simulation::syncNavCoverage).
	std::set<std::uint64_t> have;
	sim.world().ecs().each([&](const state::Door& door) { have.insert(door.key); });
	for (const DoorSite& site : sim.world().doorSites())
	{
		if (have.count(site.key) != 0)
			continue;
		knav::Point centre;
		knav::NavMesh::DoorFootprint footprint;
		if (!navigation.mesh().doorCentre(site.key, centre, &footprint))
			continue;
		state::Door door;
		door.key = site.key;
		door.recordSid = site.recordSid;
		door.x = centre[0];
		door.y = centre[1];
		door.z = centre[2];
		door.axisX = footprint.axisX;
		door.axisZ = footprint.axisZ;
		door.halfLong = footprint.halfLong;
		door.halfDeep = footprint.halfDeep;
		if (gd != nullptr)
		{
			if (const kdata::Record* record = gd->find(site.recordSid))
			{
				door.maxHealth = door.health = doorMaxHealth(*record);
			} else {
				// A dangling door reference has no record to read: the original's zero-cost default (born complete, HP 1).
				std::cerr << "door " << site.key << ": record '" << site.recordSid << "' not found, max HP 1\n";
				door.maxHealth = door.health = 1.0;
			}
		}
		// A gate starts open and stays open until a command shuts it (DOORS.md sections 6 and 8, doors.hpp: the original never
		// closes a gate). Other doors start shut: closed, or locked when `always locks`.
		door.state = site.gate ? 0 : site.alwaysLocks ? 2 : 1;
		door.alwaysLocks = site.alwaysLocks;
		door.moveSpeed = site.moveSpeed;
		door.gate = site.gate;
		// The building's faction (GATES_CARRY_LIVE.md b): the instance's own `owner faction ID`, else its town's (UNSURE: the
		// nearest owned town within 1,500 units, the data has no building -> town link). Nobody's: the "nofac" defaults.
		door.ownerSid = !site.ownerFaction.empty() ? site.ownerFaction : nearestOwner(towns, site.x, site.z);
		door.lockLevel = doorLockLevel(gd.get(), door.ownerSid, site.recordSid, site.key);
		door.building = site.building;
		// The way in, one of the slab's two normals (state::Door::inX): probe the mesh a door-width either side. A side with no
		// walkable ground there is the building's solid body (a storm house has no walkable interior: its door is a passage in a
		// wall), so the way in is that side and nobody is ever inside; with ground on both sides the interior is walkable and the
		// way in is the side the parent placement's origin lies on (UNSURE: a pivot at the building's front gives the wrong side).
		// With ground on neither the door has no inside (inX = inZ = 0, no building membership). A town gate (its parent is a wall
		// gate / gateway, DoorSite::gate) is a passage, not a building: it has no inside either, and its wall placement is never a
		// membership (it would blind the guards to everyone who came in another way).
		const double normalX = -footprint.axisZ;
		const double normalZ = footprint.axisX;
		const auto walkable = [&](double along)
		{
			const auto spot = navigation.walker().snap({door.x + normalX * along, door.y, door.z + normalZ * along}, 40.0);
			return spot && std::hypot(spot->x - (door.x + normalX * along), spot->z - (door.z + normalZ * along)) < 4.0;
		};
		const bool plus = walkable(14.0) && walkable(30.0);
		const bool minus = walkable(-14.0) && walkable(-30.0);
		if ((plus || minus) && !site.gate)
		{
			const double toward = (site.x - door.x) * normalX + (site.z - door.z) * normalZ;
			const bool inPlus = plus && minus ? toward >= 0.0 : !plus;
			door.inX = inPlus ? normalX : -normalX;
			door.inZ = inPlus ? normalZ : -normalZ;
		}
		sim.spawnDoor(std::move(door));
		++count;
	}
	sim.syncDoorPaint();
	// Whoever already stands in a building when the doors appear is in it. Only when some did: after a load the doors are there and every
	// Fighter carries its saved building, and one standing outside a door within the footprint box would be taken in (the load hash drifts).
	if (count != 0)
		sim.placeInBuildings();
	return count;
}

namespace
{

// The original's container rule (rwh.146): a BUILDING with `has inventory` is an item container only when it has a functionality record (the
// class that makes the grid); function 2 (Resource storage) whitelists the grid to the `limit inventory` items, and the town fill only
// seeds functions 2 / 9 / 13. Returns the first functionality's `function`, -1 for none.
std::int32_t containerFunction(const kdata::GameData& data, const kdata::Record& record)
{
	if (const kdata::RefCategory* functions = record.findRefs("functionality"))
		for (const kdata::RefEntry& entry : functions->entries)
		{
			const kdata::Record* function = entry.v0 == kdata::kReferenceRemoved ? nullptr : data.find(entry.targetID);
			if (function != nullptr)
				return function->intOr("function", 0);
		}
	return -1;
}

std::vector<std::string> limitInventory(const kdata::Record& record)
{
	std::vector<std::string> items;
	if (const kdata::RefCategory* refs = record.findRefs("limit inventory"))
		for (const kdata::RefEntry& entry : refs->entries)
			if (entry.v0 != kdata::kReferenceRemoved)
				items.push_back(entry.targetID);
	return items;
}

bool isItemContainer(const kdata::GameData& data, const kdata::Record& record)
{
	return record.boolOr("has inventory", false) && containerFunction(data, record) >= 0;
}

// The weighted entries of a VENDOR_LIST's `items`: ITEM / ARMOUR / WEAPON records by v0.
struct ContentsList
{
	struct Entry
	{
		std::string sid;
		double weight;
	};
	std::vector<Entry> entries;
	double total = 0.0;

	explicit ContentsList(const kdata::Record* list)
	{
		const kdata::RefCategory* refs = list != nullptr ? list->findRefs("items") : nullptr;
		for (std::size_t i = 0; refs != nullptr && i < refs->entries.size(); ++i)
		{
			const kdata::RefEntry& entry = refs->entries[i];
			if (entry.opaqueEntry || entry.v0 <= 0)
				continue;
			entries.push_back({entry.targetID, static_cast<double>(entry.v0)});
			total += static_cast<double>(entry.v0);
		}
	}

	// The original's candidate test (FUN_14095a5b0): a function 2 / 9 / 13 box, with weapons / armour only when the list produces that item
	// type, and a `limit inventory` box only when the list produces one of its items. A water tank (limit Water) is dropped here.
	bool seeds(const kdata::GameData& data, const kdata::Record& record) const
	{
		const std::int32_t function = containerFunction(data, record);
		if (function != 2 && function != 9 && function != 13)
			return false;
		const std::int32_t limit = record.intOr("itemtype limit", 0);
		if (limit == 2 || limit == 3)
		{
			const std::int32_t want = limit == 2 ? kworld::itemTypeByName("WEAPON") : kworld::itemTypeByName("ARMOUR");
			return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry)
			{
				const kdata::Record* item = data.find(entry.sid);
				return item != nullptr && item->type == want;
			});
		}
		const std::vector<std::string> only = limitInventory(record);
		return only.empty() || std::any_of(entries.begin(), entries.end(), [&](const Entry& entry)
		{
			return std::find(only.begin(), only.end(), entry.sid) != only.end();
		});
	}

	// A weapon or armour piece is stocked with a generated quality (CombatData::rollQuality). A function 2 box takes only its whitelist.
	state::Inventory draw(const Simulation& sim, std::uint64_t key, const kdata::Record* box = nullptr) const
	{
		state::Inventory held;
		held.version = 1;
		const std::vector<std::string> only = box != nullptr && containerFunction(*sim.gameData(), *box) == 2 ? limitInventory(*box) : std::vector<std::string>{};
		for (int n = 0; n < kContainerDraws && total > 0.0 && (box == nullptr || sim.gameData() == nullptr || seeds(*sim.gameData(), *box)); ++n)
		{
			const std::uint64_t dice = key * 31 + static_cast<std::uint64_t>(n);
			double roll = unit01(dice) * total;
			for (const Entry& entry : entries)
			{
				roll -= entry.weight;
				if (roll < 0.0)
				{
					if (!only.empty() && std::find(only.begin(), only.end(), entry.sid) == only.end())
						break;
					state::ItemQuality quality;
					const kdata::Record* record = sim.gameData()->find(entry.sid);
					if (record != nullptr && sim.combat() != nullptr
						&& (record->type == kworld::itemTypeByName("WEAPON") || record->type == kworld::itemTypeByName("ARMOUR")))
						quality = sim.combat()->data().rollQuality(*sim.gameData(), *record, unit01(dice ^ 0x71a1), unit01(dice ^ 0x71a2));
					held.add(entry.sid, 1, state::Inventory::kCarried, quality);
					break;
				}
			}
		}
		held.version = 1;
		return held;
	}
};

// The owning faction of every placed building that names one (through its door sites), by building key.
std::unordered_map<std::uint64_t, std::string> buildingOwners(const World& world)
{
	std::unordered_map<std::uint64_t, std::string> owners;
	for (const DoorSite& door : world.doorSites())
		if (!door.ownerFaction.empty())
			owners.emplace(door.building, door.ownerFaction);
	return owners;
}

// The hand that puts a building's layout furniture (routine_scorer's): from the layout's real pieces, the ones that are a site of some kind.
double layoutFurnishHand(const routines::Layout& layout, const kdata::GameData& data, const BuildingSite& building)
{
	std::vector<std::array<double, 2>> furniture;
	for (const routines::LayoutPiece& piece : layout.pieces)
		if (const kdata::Record* record = data.find(piece.target); record != nullptr && routines::classifyPiece(data, *record) != routines::SiteKind::None)
			furniture.push_back({piece.x, piece.z});
	return layoutHand(building, furniture);
}

} // namespace

std::size_t spawnMeshContainers(Simulation& sim, const MeshNavigator& navigation, const kdata::Record* contents)
{
	std::size_t count = 0;
	const std::vector<OwnedTown> towns = ownedTowns(sim.world().ecs());
	const ContentsList list(contents);
	const std::shared_ptr<const kdata::GameData> gd = sim.gameData();
	// A container that already has its entity is left alone, as for doors: by its key (a site, a layout piece), or by its spot for a box
	// from a scenario (key 0).
	std::set<std::uint64_t> haveKeys;
	std::map<std::uint64_t, flecs::entity> haveBox;
	std::set<std::tuple<std::string, double, double, double>> have;
	sim.world().ecs().each([&](flecs::entity entity, const state::Container& box)
	{
		if (box.key != 0)
		{
			haveKeys.insert(box.key);
			haveBox.emplace(box.key, entity);
		} else {
			have.insert({box.recordSid, box.x, box.y, box.z});
		}
	});
	// A box from a save before v108 has no building / piece: filled in from the data by its key, and its version bumped so the client hears it.
	const auto adopt = [&](std::uint64_t key, std::uint64_t building, std::uint16_t piece)
	{
		const auto found = haveBox.find(key);
		if (found == haveBox.end())
			return;
		state::Container* box = found->second.get_mut<state::Container>();
		if (box->building == building && box->piece == piece)
			return;
		box->building = building;
		box->piece = piece;
		++box->version;
	};
	for (const ContainerSite& site : sim.world().containerSites())
	{
		if (haveKeys.count(site.key) != 0)
		{
			adopt(site.key, site.key, state::Container::kNoPiece);
			continue;
		}
		if (const kdata::Record* record = gd != nullptr ? gd->find(site.recordSid) : nullptr; record != nullptr && !isItemContainer(*gd, *record))
			continue;
		const auto spot = navigation.walker().snap({site.x, site.y, site.z}, kContainerSnap);
		if (!spot)
			continue;
		state::Container box;
		box.key = site.key;
		box.building = site.key;
		box.recordSid = site.recordSid;
		box.x = site.x;
		box.y = site.y;
		box.z = site.z;
		box.useX = spot->x;
		box.useY = spot->y;
		box.useZ = spot->z;
		// The building's faction, as for its doors (GATES_CARRY_LIVE.md b): the instance's own `owner faction ID`, else the town's.
		box.ownerSid = !site.ownerFaction.empty() ? site.ownerFaction : nearestOwner(towns, site.x, site.z);
		box.locked = site.locked;
		box.lockLevel = doorLockLevel(gd.get(), box.ownerSid, site.recordSid, site.key);
		const kdata::Record* siteRecord = gd != nullptr ? gd->find(site.recordSid) : nullptr;
		sim.spawnContainer(std::move(box), list.draw(sim, site.key, siteRecord));
		haveKeys.insert(site.key);
		++count;
	}
	// The interior layouts' storage pieces: almost every town chest is layout furniture, not a placed building (a shop counter, a barrel, a
	// weapon cabinet). A piece whose BUILDING record `has inventory` is a container at the piece's place in the building (Y3's full
	// quaternion, layoutToWorld), keyed by the building and the piece's index, so a zone that is furnished again finds the same box. They are
	// not world ContainerSites: 10,252 pieces in the base world would be entities everywhere at world build. A box exists when its zone's
	// navmesh is furnished, as the doors and the item nodes do (UNSURE: the original builds the interior's furniture when it is first shown).
	if (const routines::LayoutLibrary* layouts = sim.interiorLayouts(); layouts != nullptr && gd != nullptr)
	{
		const kdata::GameData& data = *gd;
		const std::unordered_map<std::uint64_t, std::string> owners = buildingOwners(sim.world());
		for (const BuildingSite& building : sim.world().buildingSites())
		{
			if (building.ruined || building.interiorLayout.empty())
				continue;
			const routines::Layout* layout = layouts->find(building.buildingSid + "-" + building.interiorLayout);
			if (layout == nullptr)
				continue;
			const double hand = layoutFurnishHand(*layout, data, building);
			for (std::size_t i = 0; i < layout->pieces.size(); ++i)
			{
				const routines::LayoutPiece& piece = layout->pieces[i];
				const kdata::Record* record = data.find(piece.target);
				if (record == nullptr || !isItemContainer(data, *record))
					continue;
				// The key keeps the position among the kept pieces (saves name boxes by it); the wire names the data's own index (piece.index).
				const std::uint64_t key = mix64(building.key ^ mix64(i + 1));
				if (haveKeys.count(key) != 0)
				{
					adopt(key, building.key, piece.index);
					continue;
				}
				const std::array<double, 3> at = layoutToWorld(building, hand, piece.x, piece.y, piece.z);
				const auto spot = navigation.walker().snap({at[0], at[1], at[2]}, kContainerSnap);
				if (!spot)
					continue;
				state::Container box;
				box.key = key;
				box.building = building.key;
				box.piece = piece.index;
				box.recordSid = record->stringID;
				box.x = at[0];
				box.y = at[1];
				box.z = at[2];
				box.useX = spot->x;
				box.useY = spot->y;
				box.useZ = spot->z;
				const auto owner = owners.find(building.key);
				box.ownerSid = owner != owners.end() ? owner->second : nearestOwner(towns, at[0], at[2]);
				box.locked = record->boolOr("has lock", false);
				box.lockLevel = doorLockLevel(gd.get(), box.ownerSid, box.recordSid, key);
				sim.spawnContainer(std::move(box), list.draw(sim, key, record));
				haveKeys.insert(key);
				++count;
			}
		}
	}
	return count;
}

namespace
{

// An ITEM_PLACEMENT_GROUP's lists as one pool, v0 a relative weight (0 counts as 100, negative drops the entry; v1 / v2 unread), and
// its respawn hours (the def's: the node state's override is not read; kinspect itemgroup_static.md a, b). A weapon is kept only when
// one of the group's `weapon manufacturers` lists it in `weapon types` (none: no weapons, so Luxury Weapons puts out nothing).
// UNSURE (ours): the maker and model come from CombatData::rollQuality over the group's makers, not the original's v0 / v1 picks.
struct PlacementGroup
{
	std::vector<std::pair<std::string, double>> entries;
	double total = 0.0;
	std::int32_t minHours = 24;
	std::int32_t maxHours = 48;
	std::vector<std::string> makers; // `weapon manufacturers`

	PlacementGroup(const kdata::GameData& data, const kdata::Record& group)
	{
		std::set<std::string> made; // the weapon types of the makers
		if (const kdata::RefCategory* refs = group.findRefs("weapon manufacturers"))
		{
			for (const kdata::RefEntry& entry : refs->entries)
			{
				if (entry.opaqueEntry || entry.v0 < 0)
					continue;
				makers.push_back(entry.targetID);
				const kdata::Record* maker = data.find(entry.targetID);
				const kdata::RefCategory* types = maker != nullptr ? maker->findRefs("weapon types") : nullptr;
				for (std::size_t i = 0; types != nullptr && i < types->entries.size(); ++i)
					made.insert(types->entries[i].targetID);
			}
		}
		for (const std::string_view category : {"items", "weapons", "armours", "clothing", "containers", "blueprints", "crossbows", "robotics"})
		{
			const kdata::RefCategory* refs = group.findRefs(category);
			for (std::size_t i = 0; refs != nullptr && i < refs->entries.size(); ++i)
			{
				const kdata::RefEntry& entry = refs->entries[i];
				if (entry.opaqueEntry || entry.v0 < 0 || (category == "weapons" && made.count(entry.targetID) == 0))
					continue;
				const double weight = entry.v0 == 0 ? 100.0 : static_cast<double>(entry.v0);
				entries.emplace_back(entry.targetID, weight);
				total += weight;
			}
		}
		minHours = group.intOr("min respawn time", minHours);
		maxHours = std::max(minHours, group.intOr("max respawn time", maxHours));
	}
};

// Puts the node's next pick out as an owned GroundItem on its spot. False when the group has nothing to pick.
bool fillItemNode(Simulation& sim, state::ItemNode& node)
{
	const kdata::Record* group = sim.gameData()->find(node.groupSid);
	if (group == nullptr)
		return false;
	const PlacementGroup list(*sim.gameData(), *group);
	if (list.total <= 0.0)
		return false;
	const std::uint64_t dice = mix64(node.key) + node.picks++;
	double roll = unit01(dice) * list.total;
	for (const auto& [sid, weight] : list.entries)
	{
		roll -= weight;
		if (roll >= 0.0)
			continue;
		state::GroundItem item;
		item.itemSid = sid;
		const kdata::Record* record = sim.gameData()->find(sid);
		// Armour takes a grade (itemgroup_static.md c), a weapon its maker and model.
		if (record != nullptr && record->type == kworld::itemTypeByName("ARMOUR"))
			item.quality = sim.combat()->data().rollQuality(*sim.gameData(), *record, unit01(dice ^ 0x71a1), unit01(dice ^ 0x71a2));
		else if (record != nullptr && record->type == kworld::itemTypeByName("WEAPON"))
			item.quality = sim.combat()->data().rollQuality(*sim.gameData(), *record, unit01(dice ^ 0x71a1), unit01(dice ^ 0x71a2), nullptr, &list.makers);
		item.x = node.x;
		item.y = node.y;
		item.z = node.z;
		item.useX = node.useX;
		item.useY = node.useY;
		item.useZ = node.useZ;
		item.ownerSid = node.ownerSid;
		node.item = sim.spawnGroundItem(std::move(item)).get<state::Id>()->value;
		node.refillTick = 0;
		return true;
	}
	return false;
}

} // namespace

std::size_t spawnMeshItemNodes(Simulation& sim, const MeshNavigator& navigation)
{
	const routines::LayoutLibrary* layouts = sim.interiorLayouts();
	if (layouts == nullptr || sim.gameData() == nullptr)
		return 0;
	const kdata::GameData& data = *sim.gameData();
	const std::int32_t groupType = kworld::itemTypeByName("ITEM_PLACEMENT_GROUP");
	std::set<std::uint64_t> have;
	sim.world().ecs().each([&](const state::ItemNode& node) { have.insert(node.key); });
	const std::unordered_map<std::uint64_t, std::string> buildingOwner = buildingOwners(sim.world());
	std::size_t count = 0;
	for (const BuildingSite& building : sim.world().buildingSites())
	{
		const routines::Layout* layout = building.interiorLayout.empty() ? nullptr : layouts->find(building.buildingSid + "-" + building.interiorLayout);
		if (layout == nullptr)
			continue;
		const double hand = layoutFurnishHand(*layout, data, building);
		for (std::size_t i = 0; i < layout->pieces.size(); ++i)
		{
			const routines::LayoutPiece& piece = layout->pieces[i];
			const kdata::Record* group = data.find(piece.target);
			if (group == nullptr || group->type != groupType)
				continue;
			const std::uint64_t key = mix64(building.key ^ mix64(i + 1));
			if (have.count(key) != 0)
				continue;
			const std::array<double, 3> at = layoutToWorld(building, hand, piece.x, piece.y, piece.z);
			const auto spot = navigation.walker().snap({at[0], at[1], at[2]}, kContainerSnap);
			if (!spot)
				continue;
			state::ItemNode node;
			node.key = key;
			node.groupSid = group->stringID;
			node.x = at[0];
			node.y = at[1];
			node.z = at[2];
			node.useX = spot->x;
			node.useY = spot->y;
			node.useZ = spot->z;
			const auto owner = buildingOwner.find(building.key);
			// The original hands it to the building's owning squad (its leader), else nobody (itemgroup_static.md c); ours is that
			// squad's faction, the placement's owner faction.
			if (owner != buildingOwner.end())
				node.ownerSid = owner->second;
			fillItemNode(sim, node);
			sim.spawnItemNode(std::move(node));
			++count;
		}
	}
	return count;
}

void Simulation::stepItemNodes(std::uint64_t now)
{
	if (gameData_ == nullptr || combat_ == nullptr || now % kTicksPerGameHour != 0)
		return;
	flecs::world& ecs = ecsMutable();
	std::set<std::uint64_t> lying;
	ecs.each([&](const state::Id& id, const state::GroundItem&) { lying.insert(id.value); });
	std::vector<flecs::entity> due;
	ecs.each([&](flecs::entity entity, state::ItemNode& node)
	{
		if (node.item != 0 && lying.count(node.item) != 0)
			return;
		if (node.item != 0 || node.refillTick == 0)
		{
			// Taken since the last look: the refill is randint(min, max) hours away.
			const kdata::Record* group = gameData_->find(node.groupSid);
			const PlacementGroup list = group != nullptr ? PlacementGroup(*gameData_, *group) : PlacementGroup(*gameData_, kdata::Record{});
			const double hours = list.minHours + unit01(mix64(node.key) ^ node.picks) * (list.maxHours - list.minHours);
			node.item = 0;
			node.refillTick = now + static_cast<std::uint64_t>(hours * static_cast<double>(kTicksPerGameHour));
			return;
		}
		if (now >= node.refillTick)
			due.push_back(entity);
	});
	for (flecs::entity entity : due)
	{
		state::ItemNode node = *entity.get<state::ItemNode>();
		if (!fillItemNode(*this, node))
			node.refillTick = now + kTicksPerGameHour * 24;
		entity.set<state::ItemNode>(std::move(node));
	}
}

flecs::entity spawnContainerAt(Simulation& sim, const MeshNavigator& navigation, const kdata::Record* contents,
	const std::string& recordSid, double x, double y, double z, std::uint64_t key)
{
	const auto spot = navigation.walker().snap({x, y, z}, kContainerSnap * 5.0);
	if (!spot)
		return {};
	state::Container box;
	box.recordSid = recordSid;
	box.x = x;
	box.y = y;
	box.z = z;
	box.useX = spot->x;
	box.useY = spot->y;
	box.useZ = spot->z;
	box.ownerSid = nearestOwner(ownedTowns(sim.world().ecs()), x, z);
	return sim.spawnContainer(std::move(box), ContentsList(contents).draw(sim, key, sim.gameData() != nullptr ? sim.gameData()->find(recordSid) : nullptr));
}

flecs::entity Simulation::spawnDoor(state::Door door)
{
	flecs::entity entity = state::createInstance(ecsMutable());
	entity.set<state::Door>(std::move(door));
	enterableValid_ = false;
	return entity;
}

flecs::entity Simulation::spawnContainer(state::Container container, state::Inventory contents)
{
	flecs::entity entity = state::createInstance(ecsMutable());
	if (gameData_ != nullptr)
	{
		// Its own grid: the BUILDING's `storage size width / height` (INVENTORY_GRID_STATIC (a)); a record with none keeps 8 x 10 (UNSURE).
		const grid::Rules rules(*gameData_);
		const grid::Size size = rules.buildingSize(container.recordSid);
		if (size.w > 0 && size.h > 0)
		{
			contents.gridW = static_cast<std::uint8_t>(std::min(size.w, 255));
			contents.gridH = static_cast<std::uint8_t>(std::min(size.h, 255));
		}
		const std::uint64_t id = entity.get<state::Id>()->value;
		for (const state::Inventory::Entry& lost : grid::settle(contents, rules, id))
			std::cerr << "container " << container.recordSid << ": " << lost.count << " x " << lost.itemSid << " do not fit its grid and are dropped\n";
	}
	entity.set<state::Container>(std::move(container));
	entity.set<state::Inventory>(std::move(contents));
	return entity;
}

flecs::entity Simulation::spawnGroundItem(state::GroundItem item)
{
	flecs::entity entity = state::createInstance(ecsMutable());
	entity.set<state::GroundItem>(std::move(item));
	return entity;
}

flecs::entity Simulation::spawnItemNode(state::ItemNode node)
{
	flecs::entity entity = state::createInstance(ecsMutable());
	entity.set<state::ItemNode>(std::move(node));
	return entity;
}

flecs::entity Simulation::fighterEntity(std::uint64_t id) const
{
	const auto& members = combat_->members();
	const auto at = std::lower_bound(members.begin(), members.end(), id,
		[](const CombatArena::Member& member, std::uint64_t key) { return member.id < key; });
	return flecs::entity(world_.ecs().c_ptr(), combatEntities_[static_cast<std::size_t>(at - members.begin())]);
}

namespace
{

// How long a door phase runs (state::DoorStep::phase), in seconds.
double doorPhaseSeconds(std::uint8_t phase, const state::Door& door)
{
	switch (phase)
	{
	case 0: case 6: return kUnlockSeconds; // 6 lock: UNSURE, taken as the unlock's time
	case 2: return kBashInterval;
	default: return doorMoveSeconds(door.moveSpeed); // 1 / `door move speed`; the state flips at kDoorToggleOpenness of it
	}
}

// A phase ends when its time is used up; this slack keeps 54 x 0.05 s from missing 2.7 s by a rounding error.
constexpr double kPhaseEpsilon = 1e-9;

// Distance from (px, pz) to the segment a-b, in xz.
double segmentDistance(double px, double pz, double ax, double az, double bx, double bz)
{
	const double dx = bx - ax;
	const double dz = bz - az;
	const double length2 = dx * dx + dz * dz;
	const double t = length2 > 0.0 ? std::clamp(((px - ax) * dx + (pz - az) * dz) / length2, 0.0, 1.0) : 0.0;
	return std::hypot(px - (ax + t * dx), pz - (az + t * dz));
}

} // namespace

Route routeToInteract(const MeshNavigator& nav, const state::Fighter& who, const InteractTarget& target, std::uint8_t action, bool planned,
	Body body)
{
	const knav::Position from{who.x, who.y, who.z};
	if (action == static_cast<std::uint8_t>(InteractAction::Talk) && target.kind == InteractKind::Character)
	{
		Route stand = nav.routeToStand(from, target.x, target.y, target.z, target.reachStopped, planned, body);
		if (stand.found)
			return stand;
	}
	if (target.kind == InteractKind::Producer)
	{
		// The approach point (kinspect item59 MQ9): the navmesh point nearest the spot kProducerApproach from the origin toward the
		// actor (search kProducerApproachSnap). ADDED (ours): when that point has no route (it snapped onto the deposit's own rock top,
		// an island), the same distance round the origin, nearest first; the arrival rule then decides (within).
		constexpr double kPi = 3.14159265358979323846;
		const double toward = std::atan2(who.x - target.x, who.z - target.z);
		for (int k = 0; k < 16; ++k)
		{
			const double angle = toward + (k % 2 == 0 ? 1.0 : -1.0) * ((k + 1) / 2) * (kPi / 8.0);
			const auto spot = nav.walker().snap({target.x + std::sin(angle) * kProducerApproach, target.y,
				target.z + std::cos(angle) * kProducerApproach}, kProducerApproachSnap);
			if (!spot)
				continue;
			Route route = nav.routeFrom(from, spot->x, spot->z, spot->y, planned, body);
			// A clearance radius pushes the end off the walls (radius + 0.5, NAVMESH_CACHE.md "Clearance"), so a use point
			// beside the deposit is an approach route that stops short of it: still arrived by the interact rule (the stopped
			// reach, or the deposit's own bounds x 1.2), which is what the order's arrival check asks.
			if (route.found && (!route.approach || target.nearEnough(route.points.back().x, route.heights.back(), route.points.back().z,
				target.reachStopped)))
				return route;
		}
	}
	Route route = nav.routeFrom(from, target.x, target.z, target.y, planned, body);
	// A slab's centre is a wall to the walk: the route ends at the nearest mesh point (an approach), and with a
	// clearance radius it can fail outright. Either way the face on the actor's side is the answer when it routes.
	if (target.kind == InteractKind::Door && target.halfDeep > 0.0 && (!route.found || route.approach))
	{
		// ADDED (ours; UNSURE: the original's door use point is not decoded): the centre of a shut slab is a wall to the walk, which
		// then ends at the mesh point nearest it. For a locked gate that can be the far face: from the near one the walk went round
		// the town (11.5 km at Stack's east gate, ksim_carry) and the order failed. The face on the actor's side, when it routes.
		const double nx = -target.axisZ;
		const double nz = target.axisX;
		const double side = (who.x - target.x) * nx + (who.z - target.z) * nz >= 0.0 ? 1.0 : -1.0;
		const double out = target.halfDeep + kDoorFaceOut;
		const auto face = nav.walker().snap({target.x + nx * side * out, target.y, target.z + nz * side * out}, kDoorFaceSnap);
		if (face && target.distanceTo(face->x, face->z) <= target.reach)
		{
			Route near = nav.routeFrom(from, face->x, face->z, face->y, planned, body);
			if (near.found && !near.approach)
				return near;
		}
	}
	return route;
}

void Simulation::reportInteract(std::uint64_t who, std::uint64_t target, std::uint8_t action, std::uint8_t result, std::uint8_t flags)
{
	if (!interactObserver_)
		return;
	InteractEvent done;
	done.kind = InteractEvent::Kind::Done;
	done.who = who;
	done.target = target;
	done.action = action;
	done.result = result;
	done.flags = flags;
	interactObserver_(done);
}

void Simulation::cancelOrders(std::uint64_t id)
{
	farOrders_.erase(id); // a leg-by-leg move order (far_orders.cpp) ends with any replacing order
	flecs::entity entity = fighterEntity(id);
	if (const state::InteractTask* task = entity.get<state::InteractTask>())
	{
		reportInteract(id, task->target, task->action, InteractEvent::kCancelled, task->flags);
		entity.remove<state::InteractTask>();
	}
	if (const auto lock = lockOrders_.find(id); lock != lockOrders_.end() && lock->second.action != 0)
	{
		reportInteract(id, lock->second.cage != 0 ? lock->second.cage : lock->second.victim, lock->second.action, InteractEvent::kCancelled, 0); // a player shackle or cage order (order 60 actions 15..20)
		lockOrders_.erase(lock);
	}
	entity.remove<state::DoorStep>(); // a replaced walk or door action: the saved route is dropped with it
	entity.remove<state::ProspectTask>(); // a replacing order cancels a running prospect with no result
	// A lift that a new order interrupts stops: before the body attached nothing happened, after it the carrier holds the body.
	if (const state::CarryStep* carry = entity.get<state::CarryStep>(); carry != nullptr && carry->phase == 0)
	{
		if (carry->attached)
		{
			state::CarryStep carrying = *carry;
			carrying.phase = 1;
			entity.set<state::CarryStep>(std::move(carrying));
		} else {
			entity.remove<state::CarryStep>();
		}
	}
	if (const state::OrderQueue* queue = entity.get<state::OrderQueue>())
	{
		for (const state::QueuedOrder& order : queue->orders)
		{
			if (order.kind == 2)
				reportInteract(id, order.target, order.action, InteractEvent::kCancelled, order.flags);
		}
		entity.remove<state::OrderQueue>();
	}
}

void Simulation::beginInteract(std::uint64_t id, std::uint64_t target, std::uint8_t action, std::uint32_t seq,
	std::vector<state::RoutePoint> route, bool approach, std::uint8_t flags)
{
	state::Fighter* f = combat_->find(id);
	f->clearRoute();
	if (!route.empty())
	{
		f->route = std::move(route);
		f->routeNext = 0;
		++f->routeVersion;
		f->routeSeq = seq;
		f->routeApproach = approach;
	}
	f->routeTries = 0;
	f->drive = state::DriveMode::Stop;
	f->driveStrafe = 0;
	f->orderTarget = 0;
	f->orderStop = true;
	f->hold = false;
	fighterEntity(id).set<state::InteractTask>(state::InteractTask{target, action, seq, flags});
}

void Simulation::stepOrderQueues()
{
	if (combat_ == nullptr || navigation_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		// A KO clears the character's orders (rwh.195, AITaskSytem::clearOrders): the queue and the running interact / door / prospect / carry step.
		if (members[i].fighter.unconscious && !members[i].fighter.dead)
			cancelOrders(members[i].id);
		if (!actor.has<state::OrderQueue>())
			continue;
		const std::uint64_t id = members[i].id;
		state::Fighter& fighter = *combat_->find(id);
		if (fighter.dead || fighter.unconscious)
		{
			cancelOrders(id);
			continue;
		}
		// The next order starts when the last has ended: no interact task, no walk, drive, follow or attack order.
		// A queued order that cannot start (no route, the target gone or refusing) ends at once and the next one is tried.
		while (actor.has<state::OrderQueue>() && !actor.has<state::InteractTask>() && !actor.has<state::DoorStep>()
			&& !actor.has<state::ProspectTask>() && !fighter.taskRunning()
			&& fighter.orderTarget == 0)
		{
			state::OrderQueue queue = *actor.get<state::OrderQueue>();
			const state::QueuedOrder next = queue.orders.front();
			queue.orders.erase(queue.orders.begin());
			if (queue.orders.empty())
				actor.remove<state::OrderQueue>();
			else
				actor.set<state::OrderQueue>(std::move(queue));
			// A far goal or a road move walks in legs (far_orders.cpp); a near one is one route.
			if (next.kind == 1)
			{
				const bool far = navigation_->farGoal(fighter.x, fighter.z, next.x, next.z);
				if ((far || std::hypot(next.x - fighter.x, next.z - fighter.z) > kworld::RoadNetwork::kLongMove)
					&& startMoveOrder(id, fighter, next.x, next.z, next.y, next.seq, far))
				{
					fighter.drive = state::DriveMode::Stop;
					fighter.driveStrafe = 0;
					fighter.orderTarget = 0;
					fighter.orderStop = true;
					fighter.hold = false;
					break;
				}
			}
			const Route route = [&]
			{
				if (next.kind == 1)
					return navigation_->routeFrom({fighter.x, fighter.y, fighter.z}, next.x, next.z, next.y, false, body(fighter));
				const InteractTarget target = findInteractTarget(ecs, combat_.get(), next.target, this);
				if (target.kind == InteractKind::None || !checkInteract(ecs, *combat_, id, target, next.action, next.flags, gameData_.get()).empty()
					|| target.arrived(fighter))
					return Route{true};
				return routeToInteract(*navigation_, fighter, target, next.action, false, body(fighter));
			}();
			if (next.kind == 2)
			{
				// Gone or refused: the arrival check says so with the proper event 13. A target out of reach by no route: unreachable.
				if (!route.found)
				{
					reportInteract(id, next.target, next.action, InteractEvent::kUnreachable, next.flags);
					continue;
				}
				std::vector<state::RoutePoint> points;
				for (std::size_t k = 0; k < route.points.size(); ++k)
					points.push_back({route.points[k].x, route.heights[k], route.points[k].z});
				beginInteract(id, next.target, next.action, next.seq, std::move(points), route.approach, next.flags);
				break;
			}
			if (!route.found || route.points.empty())
				continue; // UNSURE: a queued move with no route is dropped without a word (event 10 answered the order long ago)
			std::vector<state::RoutePoint> points;
			for (std::size_t k = 0; k < route.points.size(); ++k)
				points.push_back({route.points[k].x, route.heights[k], route.points[k].z});
			fighter.clearRoute();
			fighter.route = std::move(points);
			fighter.routeNext = 0;
			++fighter.routeVersion;
			fighter.routeSeq = next.seq;
			fighter.routeSource = state::Fighter::PlanSource::Order; // a queued move order, as the replacing one (simulation.cpp)
			fighter.routeChase = false;
			fighter.routeApproach = route.approach;
			fighter.routeTries = 0;
			fighter.drive = state::DriveMode::Stop;
			fighter.driveStrafe = 0;
			fighter.orderTarget = 0;
			fighter.orderStop = true;
			fighter.hold = false;
			break;
		}
	}
}

Simulation::Storage Simulation::findStorage(std::uint64_t id) const
{
	Storage storage;
	if (const InteractTarget target = findInteractTarget(world_.ecs(), nullptr, id); target.kind == InteractKind::Container)
	{
		storage.entity = target.entity;
		storage.x = target.x;
		storage.y = target.y;
		storage.z = target.z;
		storage.ownerSid = flecs::entity(world_.ecs().c_ptr(), target.entity).get<state::Container>()->ownerSid;
		return storage;
	}
	if (const InteractTarget target = findInteractTarget(world_.ecs(), nullptr, id); target.kind == InteractKind::Producer)
	{
		// A producer's output slot: taking from it is allowed, putting in only the item it produces (checkTransfer, runInventoryOrder). Wild ones have no owner.
		storage.entity = target.entity;
		storage.producer = true;
		// From the origin: the use range, or the rock's own half size x 1.2 when that is wider (the arrival box, MQ9).
		storage.reach = std::max({target.reach, kTransferReach, 0.6 * std::hypot(target.boxMaxX - target.boxMinX, target.boxMaxZ - target.boxMinZ)});
		storage.reachStopped = 2.0 * storage.reach;
		storage.x = target.x;
		storage.y = target.y;
		storage.z = target.z;
		storage.ownerSid = flecs::entity(world_.ecs().c_ptr(), target.entity).get<state::Producer>()->ownerSid;
		return storage;
	}
	if (combat_ != nullptr && combat_->find(id) == nullptr && world_.ecs().count<state::TraderStock>() != 0)
	{
		// A merchant squad's stock (trade.hpp): the squad's Id addresses its Inventory; only order 68 uses it, the range is the trader character's.
		world_.ecs().each([&](flecs::entity entity, const state::Id& instance, const state::TraderStock&, const state::Inventory&)
		{
			if (instance.value == id)
			{
				storage.entity = entity.id();
				storage.trader = true;
			}
		});
		if (storage.trader)
			return storage;
	}
	const state::Fighter* fighter = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (fighter == nullptr || !(fighter->dead || fighter->unconscious))
		return storage;
	const flecs::entity entity = fighterEntity(id);
	storage.entity = entity.id();
	storage.body = true;
	const RaceCombat* race = combat_->data().race(fighter->raceSid);
	storage.reach = characterReach(race != nullptr ? race->footprintRadius : 2.6, true, false);
	storage.reachStopped = characterReach(race != nullptr ? race->footprintRadius : 2.6, true, true);
	storage.x = fighter->x;
	storage.y = fighter->y;
	storage.z = fighter->z;
	if (const state::Allegiance* allegiance = entity.get<state::Allegiance>())
		storage.ownerSid = allegiance->factionSid;
	return storage;
}

bool Simulation::inStorageReach(const state::Fighter& who, const Storage& storage) noexcept
{
	return std::hypot(who.x - storage.x, who.z - storage.z) <= (who.routeNext < who.route.size() ? storage.reach : storage.reachStopped) && std::fabs(who.y - storage.y) <= kInteractReachY;
}

namespace
{

// One unit-count carried as a loose entry: what moves between inventories.
state::Inventory::Entry loose(state::Inventory::Entry entry)
{
	entry.slot = state::Inventory::kCarried;
	entry.section = state::Inventory::Entry::kUnplaced;
	return entry;
}

// Order 63 on a body: `count` units of `item`, its carried entries first, then its equipped ones (stripping; SIMLINK.md section 13).
// The pieces come out unplaced; `stripped` gets the record sids of the equipped ones. False when the body holds fewer (the caller
// works on a copy).
bool takeFromBody(state::Inventory& inventory, const std::string& item, std::uint32_t count, std::vector<state::Inventory::Entry>& pieces,
	std::vector<std::string>& stripped)
{
	const std::uint32_t carried = std::min(count, inventory.carried(item));
	if (carried > 0)
		pieces = inventory.takeCarried(item, carried);
	std::uint32_t rest = count - carried;
	for (auto it = inventory.items.begin(); it != inventory.items.end() && rest > 0;)
	{
		if (it->slot == state::Inventory::kCarried || it->itemSid != item)
		{
			++it;
			continue;
		}
		rest -= std::min(rest, it->count);
		stripped.push_back(it->itemSid);
		pieces.push_back(loose(std::move(*it)));
		it = inventory.items.erase(it);
	}
	if (!stripped.empty())
		++inventory.version;
	return rest == 0;
}

} // namespace

Simulation::LootResult Simulation::lootBody(std::uint64_t to, std::uint64_t from)
{
	LootResult result;
	flecs::entity body = fighterEntity(from);
	const state::Inventory* held = body.get<state::Inventory>();
	if (held == nullptr || held->items.empty() || gameData_ == nullptr)
		return result;
	const grid::Rules rules(*gameData_);
	state::Inventory source = *held;
	grid::settle(source, rules, from);
	flecs::entity actor = fighterEntity(to);
	state::Inventory mine;
	if (const state::Inventory* carried = actor.get<state::Inventory>())
		mine = *carried;
	std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, to);
	state::Fighter* victim = combat_->find(from);
	// Everything the body has, equipped pieces included, in entry order; what does not fit stays on the body (the original's loot
	// helper tries the pack and then drops; here the rest stays, and the order reports it).
	for (auto it = source.items.begin(); it != source.items.end();)
	{
		const std::uint32_t before = it->count;
		const std::uint32_t left = grid::autoplace(mine, rules, loose(*it));
		result.took += before - left;
		result.left += left;
		if (left == before)
		{
			++it;
			continue;
		}
		if (left > 0)
		{
			it->count = left;
			++it;
			continue;
		}
		// A worn piece that is taken leaves the body's hands or back: its look changes with it.
		if (it->slot == state::Inventory::kWeapon)
			victim->weapon = combat_->data().unarmed;
		else if (it->slot >= state::Inventory::kArmourBase && it->slot != state::Inventory::kBackpackSlot && it->slot != state::Inventory::kSheathed)
		{
			const std::int32_t slot = it->slot - state::Inventory::kArmourBase;
			victim->armour.erase(std::remove_if(victim->armour.begin(), victim->armour.end(), [&](const state::FighterArmour& p) { return p.slot == slot; }),
				victim->armour.end());
		}
		it = source.items.erase(it);
	}
	if (result.took > 0)
	{
		++mine.version;
		++source.version;
		storeInventory(actor, to, std::move(mine), std::move(overflow));
		body.set<state::Inventory>(std::move(source));
		// Task_Loot_AI (140355ad0 L113): looting a body that is not the looter's own faction's is crime 10 (reportCrime refuses a law looter).
		if (const state::Allegiance* side = body.get<state::Allegiance>(); side != nullptr && isForeignOwner(world_.ecs(), to, side->factionSid))
		{
			reportCrime(to, crime::kLooting, side->factionSid, from);
			noteCaughtAct(to, 1, from);
		}
	}
	return result;
}

void Simulation::dropItem(std::uint64_t who, const std::string& item, std::uint32_t count)
{
	flecs::entity actor = fighterEntity(who);
	const state::Inventory* held = actor.get<state::Inventory>();
	if (held == nullptr || count == 0)
		return;
	state::Inventory inventory = *held;
	std::vector<state::Inventory::Entry> pieces = inventory.takeCarried(item, count);
	if (pieces.empty())
		return;
	actor.set<state::Inventory>(std::move(inventory));
	// One ground stack per stack that was taken.
	dropPieces(who, std::move(pieces));
}

Simulation::LootResult Simulation::lootContainer(std::uint64_t to, flecs::entity container)
{
	LootResult result;
	const state::Inventory* held = container.get<state::Inventory>();
	if (held == nullptr || held->items.empty() || gameData_ == nullptr)
		return result;
	const grid::Rules rules(*gameData_);
	const state::Container* box = container.get<state::Container>();
	const std::uint64_t boxId = container.get<state::Id>()->value;
	state::Inventory source = *held;
	grid::settle(source, rules, boxId);
	flecs::entity actor = fighterEntity(to);
	state::Inventory mine;
	if (const state::Inventory* carried = actor.get<state::Inventory>())
		mine = *carried;
	std::vector<state::Inventory::Entry> overflow = grid::settle(mine, rules, to);
	// Provenance: what a thief carries off is marked with the victim faction (an earlier mark stays).
	const bool theft = box != nullptr && isForeignContainer(world_.ecs(), to, *box);
	const std::string owner = box != nullptr ? box->ownerSid : std::string(); // a copy: storeInventory / set below can move table rows under `box` (k16.5.14)
	for (auto it = source.items.begin(); it != source.items.end();)
	{
		const std::uint32_t before = it->count;
		state::Inventory::Entry moving = loose(*it);
		if (theft && moving.stolen.empty())
			moving.stolen = owner;
		clearReturnedMark(moving, factionOf(world_.ecs(), to), false);
		const std::uint32_t left = grid::autoplace(mine, rules, std::move(moving));
		result.took += before - left;
		result.left += left;
		if (left == 0)
		{
			it = source.items.erase(it);
			continue;
		}
		it->count = left;
		++it;
	}
	if (result.took == 0)
		return result;
	++mine.version;
	++source.version;
	storeInventory(actor, to, std::move(mine), std::move(overflow));
	container.set<state::Inventory>(std::move(source));
	if (theft)
		reportTheft(to, boxId, owner);
	return result;
}

bool Simulation::containerTakesType(const state::Container& box, const std::string& item) const
{
	// `itemtype limit` 3 armour, 2 weapon, 4 (or none) anything (Armour Stand, Weapon Display). UNSURE: the exe's use of the field is
	// not read; the field's other values allow anything here.
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(box.recordSid) : nullptr;
	const std::int32_t limit = record != nullptr ? record->intOr("itemtype limit", 0) : 0;
	if (record != nullptr && containerFunction(*gameData_, *record) == 2)
		if (const std::vector<std::string> only = limitInventory(*record); !only.empty() && std::find(only.begin(), only.end(), item) == only.end())
			return false; // a Resource storage grid is whitelisted to its `limit inventory` (rwh.146: a water tank takes only Water)
	if (limit != 2 && limit != 3)
		return true;
	const kdata::Record* itemRecord = gameData_ != nullptr ? gameData_->find(item) : nullptr;
	const std::int32_t type = itemRecord != nullptr ? itemRecord->type : -1;
	return limit == 2 ? type == kworld::itemTypeByName("WEAPON") : type == kworld::itemTypeByName("ARMOUR");
}

std::string Simulation::checkTransfer(std::uint64_t who, std::uint64_t container, const std::string& item, std::uint32_t count, bool take) const
{
	if (combat_ == nullptr || combat_->find(who) == nullptr)
		return "bad_target";
	const state::Fighter& f = *combat_->find(who);
	const Storage storage = findStorage(container);
	if (storage.entity == 0 || container == who)
		return "bad_target"; // no such container, or a body that is not down
	if (storage.trader)
		return "not_allowed"; // a stock is traded with order 68 only
	if (count == 0 || f.dead || f.unconscious)
		return "not_allowed"; // the actor's state
	// A producer's slot takes back only what it produces (kenshi-k16.19.13); the cap is the grid's (`full` below).
	if (storage.producer && !take)
	{
		const state::Producer* machine = flecs::entity(world_.ecs().c_ptr(), storage.entity).get<state::Producer>();
		if (machine == nullptr || machine->output != item)
			return "not_allowed";
	}
	if (!inStorageReach(f, storage))
		return "out_of_reach";
	const flecs::entity box(world_.ecs().c_ptr(), storage.entity);
	if (const state::Container* held = box.get<state::Container>())
	{
		// A locked box takes and gives nothing until it is picked.
		if (held->locked)
			return "locked";
		// A stand only takes its kind (containerTakesType); only a put is limited (what may go in); a take is not.
		if (!take && !containerTakesType(*held, item))
			return "item_type";
	}
	const flecs::entity actor = fighterEntity(who);
	const state::Inventory* from = (take ? box : actor).get<state::Inventory>();
	if (from == nullptr || (take ? actor : box).get<state::Inventory>() == nullptr)
		return "not_allowed";
	const bool stripping = take && storage.body; // a body also gives its equipped pieces (carried ones first)
	const auto equippedOf = [&]
	{
		std::uint32_t n = 0;
		for (const state::Inventory::Entry& e : from->items)
			n += e.slot != state::Inventory::kCarried && e.itemSid == item ? e.count : 0;
		return n;
	};
	if (from->carried(item) + (stripping ? equippedOf() : 0) < count)
		return "not_allowed";
	// There must be room for every unit (all or nothing; a stack of the order's record may span several entries).
	if (gameData_ != nullptr)
	{
		const grid::Rules rules(*gameData_);
		state::Inventory source = *from;
		state::Inventory dest = *(take ? actor : box).get<state::Inventory>();
		grid::settle(source, rules, take ? container : who);
		grid::settle(dest, rules, take ? who : container);
		std::vector<state::Inventory::Entry> pieces;
		std::vector<std::string> stripped;
		if (stripping)
			takeFromBody(source, item, count, pieces, stripped);
		else
			pieces = source.takeCarried(item, count);
		for (state::Inventory::Entry& piece : pieces)
		{
			// A character taking from a container or body uses the quick move (order 68's), a container takes main only. A full take
			// destination answers the quick move's own `no_room`; a full box is `full` (a reason of its own for the client).
			if (take)
			{
				const std::string why = const_cast<Simulation*>(this)->quickMoveInto(dest, loose(std::move(piece)), who, false);
				if (!why.empty())
					return why;
			} else if (grid::autoplace(dest, rules, loose(std::move(piece))) > 0) {
				return "full";
			}
		}
	}
	return {};
}

void Simulation::transferItem(std::uint64_t who, std::uint64_t container, const std::string& item, std::uint32_t count, bool take)
{
	const Storage storage = findStorage(container);
	flecs::entity box = ecsMutable().entity(storage.entity);
	flecs::entity actor = fighterEntity(who);
	state::Inventory from = *(take ? box : actor).get<state::Inventory>();
	state::Inventory to = *(take ? actor : box).get<state::Inventory>();
	const grid::Rules rules(*gameData_);
	std::vector<state::Inventory::Entry> overflowFrom = grid::settle(from, rules, take ? container : who);
	std::vector<state::Inventory::Entry> overflowTo = grid::settle(to, rules, take ? who : container);
	std::vector<state::Inventory::Entry> pieces;
	std::vector<std::string> stripped;
	if (take && storage.body)
		takeFromBody(from, item, count, pieces, stripped);
	else
		pieces = from.takeCarried(item, count);
	if (pieces.empty())
		return;
	if (take && stealRollCaught(who, container, item, storage))
		return; // a failed roll aborts the take (recordCaughtStealing)
	// All or nothing: a character taking the pieces uses the quick move (a free matching worn slot via the equip path, then main, then the
	// worn backpack); it is judged on a copy first so that a refusal leaves the fighter's numbers alone.
	const bool theft = take && isForeignOwner(world_.ecs(), who, storage.ownerSid);
	std::string plantedNow;
	if (theft)
	{
		for (state::Inventory::Entry& piece : pieces)
		{
			if (piece.stolen.empty())
				piece.stolen = storage.ownerSid;
		}
	}
	// The mark clears where it comes home: into the victim faction's own box or body, or into a pack of the victim's faction.
	const std::string landsWith = take ? factionOf(world_.ecs(), who) : storage.ownerSid;
	for (state::Inventory::Entry& piece : pieces)
		clearReturnedMark(piece, landsWith, false);
	if (take)
	{
		state::Inventory trial = to;
		for (const state::Inventory::Entry& piece : pieces)
		{
			if (!quickMoveInto(trial, loose(piece), who, false).empty())
				return; // no room for all of it: nothing moves (checkTransfer refused it when it was given)
		}
		for (state::Inventory::Entry& piece : pieces)
			quickMoveInto(to, loose(std::move(piece)), who, true);
	} else {
		const bool plant = storage.body && container != who;
		std::string plantedItem;
		for (state::Inventory::Entry& piece : pieces)
		{
			if (plant)
			{
				piece.planted = {{piece.count, who, clockTick()}}; // a planting supersedes the units' earlier planters
				plantedItem = piece.itemSid;
			}
			if (grid::autoplace(to, rules, loose(std::move(piece))) > 0)
				return; // as above
		}
		if (!plantedItem.empty())
			plantedNow = plantedItem;
	}
	for (const std::string& sid : stripped)
		unequipFromFighter(container, sid);
	++to.version;
	storeInventory(take ? box : actor, take ? container : who, std::move(from), std::move(overflowFrom));
	storeInventory(take ? actor : box, take ? who : container, std::move(to), std::move(overflowTo));
	if (theft)
		reportTheft(who, container, storage.ownerSid);
	if (!plantedNow.empty())
		firePlanted(who, container, plantedNow);
}

void Simulation::syncDoorPaint()
{
	if (navigation_ == nullptr)
		return;
	knav::DoorBook book;
	world_.ecs().each([&](const state::Door& door)
	{
		// state 0 open, 1 closed, 2 locked, 3 broken (state::Door); a closed door is passable (the walker's open step), a locked
		// one blocks, a broken one is open.
		if (door.state == 1 || door.state == 2)
			book.setState(door.key, door.state == 2 ? knav::DoorState::Locked : knav::DoorState::Closed);
	});
	if (!(book == navigation_->mesh().appliedDoors()))
		navigation_->applyDoors(book);
}

bool Simulation::orderLockDoor(std::uint64_t actor, std::uint64_t door, DoorSide side)
{
	return orderShutDoor(actor, door, side, true);
}

bool Simulation::orderShutDoor(std::uint64_t actor, std::uint64_t door, DoorSide side, bool lock)
{
	if (combat_ == nullptr)
		return false;
	state::Fighter* f = combat_->find(actor);
	if (f == nullptr || f->dead || f->unconscious)
		return false;
	const InteractTarget target = findInteractTarget(ecsMutable(), combat_.get(), door, this);
	const auto action = static_cast<std::uint8_t>(InteractAction::Close);
	const std::uint8_t flags = lock ? kInteractFlagLock : 0;
	if (target.kind != InteractKind::Door
		|| !checkInteract(ecsMutable(), *combat_, actor, target, action, flags, gameData_.get(), this).empty())
		return false;
	std::vector<state::RoutePoint> points;
	bool approach = false;
	if (!target.within(*f))
	{
		if (navigation_ == nullptr)
			return false; // no mesh to route on; a door within reach needs none
		Route route;
		if (side == DoorSide::Near)
		{
			route = routeToInteract(*navigation_, *f, target, action, false, body(*f));
		} else {
			// 162 / 198 lock from the inside / outside stand point (the door's own side, not the walker's).
			double x = 0.0;
			double z = 0.0;
			if (!doorWalkPoint(side == DoorSide::Inside ? goap::kMoveToDoorInside : goap::kMoveToDoorOutside, actor, door, x, z))
				return false;
			route = navigation_->routeFrom({f->x, f->y, f->z}, x, z, target.y, false, body(*f));
		}
		if (!route.found)
			return false;
		for (std::size_t k = 0; k < route.points.size(); ++k)
			points.push_back({route.points[k].x, route.heights[k], route.points[k].z});
		approach = route.approach;
	}
	beginInteract(actor, door, action, 0, std::move(points), approach, flags);
	return true;
}

bool Simulation::doorWalkPoint(std::uint16_t task, std::uint64_t actor, std::uint64_t door, double& x, double& z)
{
	if (combat_ == nullptr)
		return false;
	const state::Fighter* walker = combat_->find(actor);
	if (walker == nullptr)
		return false;
	const state::Door* found = nullptr;
	ecsMutable().each([&](const state::Id& id, const state::Door& candidate)
	{
		if (found != nullptr)
			return;
		if (task == goap::kMoveToCurrentDoor ? walker->building != 0 && candidate.building == walker->building : id.value == door)
			found = &candidate;
	});
	if (found == nullptr)
		return false;
	DoorSlab slab;
	slab.x = found->x;
	slab.z = found->z;
	slab.axisX = found->axisX;
	slab.axisZ = found->axisZ;
	slab.halfDeep = found->halfDeep;
	slab.inX = found->inX;
	slab.inZ = found->inZ;
	const DoorSide side = task == goap::kMoveToDoorInside ? DoorSide::Inside : task == goap::kMoveToDoorOutside ? DoorSide::Outside : DoorSide::Near;
	const DoorPoint point = doorStandPoint(slab, side, walker->x, walker->z, kDoorFaceOut);
	x = point.x;
	z = point.z;
	return true;
}

// One action at a time as the original (kenshi-k16.6.335, rwh.165): a character runs one task, and the combat AI ticks only inside the melee tasks, so a
// running player order (interact / lock / lift) is never fought alongside: Fighter::orderBusy keeps the combat pick off the body. Being targeted by a new
// attacker or hit while not in combat (Fighter::orderInterrupt, AI::onTargetedBy -> requestReplan) stops the running task for one update and the order starts
// again from its beginning: what lives in the task is lost (the door step's timer, the lift's timer), what lives on a shared object stays (the lock's elapsed,
// the cage / shackle lock's progress, the treatment). The order itself is never dropped by combat. One rule for every order kind, no special case.
// UNSURE: Bash, Use, Talk and Sleep orders are not in the oracle's list; they stay as they were.
void Simulation::stepOrderCombat()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		state::Fighter& fighter = *combat_->find(members[i].id);
		if (fighter.orderPause > 0)
			--fighter.orderPause;
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::InteractTask* task = actor.get<state::InteractTask>();
		const state::CarryStep* carry = actor.get<state::CarryStep>();
		bool busy = lockOrders_.count(members[i].id) != 0 || (carry != nullptr && carry->phase == 0);
		if (task != nullptr)
		{
			switch (static_cast<InteractAction>(task->action))
			{
			case InteractAction::Open: case InteractAction::Close: case InteractAction::Lockpick: case InteractAction::Unlock:
			case InteractAction::PickUp: case InteractAction::Loot: case InteractAction::FirstAid: case InteractAction::Carry:
			case InteractAction::PutDown:
				busy = true;
				break;
			default:
				break;
			}
		}
		fighter.orderBusy = busy && fighter.playerControlled && !fighter.dead && !fighter.unconscious;
		if (!fighter.orderInterrupt)
			continue;
		fighter.orderInterrupt = false;
		if (!fighter.orderBusy)
			continue;
		fighter.orderPause = 1;
		if (const state::DoorStep* step = actor.get<state::DoorStep>(); step != nullptr && step->route.empty() && !step->toggled)
			actor.remove<state::DoorStep>(); // stepInteractions starts it again from its beginning
		if (carry != nullptr && carry->phase == 0 && !carry->attached)
			actor.remove<state::CarryStep>();
	}
}

void Simulation::stepDoorSteps()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	const kdata::GameData* gd = gameData_.get();
	const std::int32_t weaponType = gd != nullptr ? kworld::itemTypeByName("WEAPON") : -1;
	// The carried melee weapon the planner would equip: the first carried WEAPON record.
	const auto carriedWeapon = [&](std::uint64_t actorId) -> std::string
	{
		if (gd == nullptr)
			return {};
		const state::Inventory* held = fighterEntity(actorId).get<state::Inventory>();
		if (held == nullptr)
			return {};
		for (const state::Inventory::Entry& entry : held->items)
		{
			const kdata::Record* record = entry.slot == state::Inventory::kCarried ? gd->find(entry.itemSid) : nullptr;
			if (record != nullptr && record->type == weaponType)
				return entry.itemSid;
		}
		return {};
	};
	// The planner's facts for `actorId` at a door it may not open (DOORS.md section 6, door_plan.hpp). The relation is the owner
	// faction's view of the actor; a factionless actor counts as hostile, a door nobody owns has no kind.
	const auto decide = [&](std::uint64_t actorId, const state::Fighter& actor, const state::Door& door, std::uint64_t doorId)
	{
		DoorFacts facts;
		facts.mine = isObjectMine(ecs, actorId, door);
		facts.locked = door.state == 2;
		facts.gate = door.gate;
		facts.lockLevel = door.lockLevel;
		facts.lockpicking = actor.skills.lockpicking;
		// hasOneWorkingArm (the pick's row): an arm that is not severed; a race with no arm part at all can still work a lock.
		bool anyArm = false;
		bool wholeArm = false;
		for (const state::FighterPart& part : actor.parts)
		{
			if (part.type != 2)
				continue;
			anyArm = true;
			wholeArm = wholeArm || !part.severed();
		}
		facts.workingArm = !anyArm || wholeArm;
		facts.weaponInHand = actor.weapon.weaponType != 5;
		facts.weaponCarried = !facts.weaponInHand && !carriedWeapon(actorId).empty();
		std::string actorSid;
		if (const state::Allegiance* allegiance = fighterEntity(actorId).get<state::Allegiance>())
			actorSid = allegiance->factionSid;
		if (!door.ownerSid.empty() && gd != nullptr)
		{
			facts.hasOwner = true;
			if (const kdata::Record* faction = gd->find(door.ownerSid))
				facts.ownerKind = faction->intOr("fundamental type", static_cast<std::int32_t>(FactionKind::Civilian));
			if (actorSid.empty())
			{
				facts.hostile = true;
			} else {
				flecs::entity owner;
				flecs::entity other;
				world_.each([&](flecs::entity entity, const state::FactionState& faction)
				{
					if (faction.templateSid == door.ownerSid)
						owner = entity;
					if (faction.templateSid == actorSid)
						other = entity;
				});
				if (owner.is_valid() && other.is_valid())
				{
					const double relation = factionRelation(*gd, owner, other);
					facts.hostile = relation <= kHostileRelation;
					facts.allied = relation >= kAlliedRelation;
				}
			}
		}
		for (std::size_t k = 0; k < members.size(); ++k)
		{
			const state::Fighter& other = members[k].fighter;
			if (members[k].id == actorId || other.dead)
				continue;
			// Squad-mates already on this door: same faction, bashing it. UNSURE: the squad is the faction here.
			if (const state::DoorStep* step = ecs.entity(combatEntities_[k]).get<state::DoorStep>())
			{
				const state::Allegiance* allegiance = ecs.entity(combatEntities_[k]).get<state::Allegiance>();
				if (step->door == doorId && step->phase == 2 && allegiance != nullptr && allegiance->factionSid == actorSid)
					++facts.mates;
			}
			if (other.target == actorId && other.state == state::CombatState::Attack)
				facts.underAttack = true;
		}
		return decideDoor(facts);
	};
	struct DoorRef
	{
		flecs::entity entity;
		std::uint64_t id;
	};
	std::vector<DoorRef> shut;
	world_.each([&](flecs::entity entity, const state::Id& id, const state::Door& door)
	{
		if (door.state == 1 || door.state == 2)
			shut.push_back({entity, id.value});
	});
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const std::uint64_t id = members[i].id;
		flecs::entity actor = ecs.entity(combatEntities_[i]);
		state::Fighter& fighter = *combat_->find(id);
		const bool down = fighter.dead || fighter.unconscious;
		if (const state::DoorStep* held = actor.get<state::DoorStep>())
		{
			if (down)
			{
				actor.remove<state::DoorStep>();
				continue;
			}
			state::DoorStep step = *held;
			// Ends the step: gives the walk back, and ends the interact order that was acting on this door.
			const auto end = [&](std::uint8_t result, bool resume)
			{
				if (resume && !step.route.empty())
				{
					fighter.clearRoute();
					fighter.route = step.route;
					fighter.routeNext = step.routeNext;
					++fighter.routeVersion;
					fighter.routeSeq = step.routeSeq;
					fighter.routeApproach = step.routeApproach;
					fighter.routeChase = false;
				}
				const state::InteractTask* task = actor.get<state::InteractTask>();
				if (step.route.empty() && task != nullptr && task->target == step.door)
				{
					reportInteract(id, task->target, task->action, result);
					actor.remove<state::InteractTask>();
				}
				actor.remove<state::DoorStep>();
			};
			const InteractTarget found = findInteractTarget(ecs, nullptr, step.door);
			if (found.kind == InteractKind::Container)
			{
				// Picking a container's lock (interact.hpp action 3): the door's timer and chance on the box's lock. A container's only
				// lock action is the pick; the order ends with event 13 once the box is open or somebody else picked it.
				state::Container* box = flecs::entity(ecs.c_ptr(), found.entity).get_mut<state::Container>();
				if (step.phase != 5 || box == nullptr)
				{
					end(InteractEvent::kGone, true);
					continue;
				}
				if (!box->locked)
				{
					end(InteractEvent::kDone, true);
					continue;
				}
				if (!sufficientLockSkill(box->lockLevel, fighter.skills.lockpicking, false))
				{
					logTask("lock_too_hard member " + std::to_string(id) + " container " + std::to_string(step.door)); // no crime, no roll (k16.6.366)
					end(InteractEvent::kTooHard, true);
					continue;
				}
				// Task_PickLock's base tick records crime 2 every tick before the roll (dt24_picklock section 7.2).
				if (isForeignOwner(world_.ecs(), id, box->ownerSid))
					{ reportCrime(id, crime::kBurglary, box->ownerSid, 0); noteCaughtAct(id, 2, step.door); }
				LockTimer lock{box->lockElapsed, box->lockInterval, box->lockAttempts};
				const PickTick tick = tickLock(lock, kTickSeconds, pickChance(box->lockLevel, fighter.skills.lockpicking), box->key, id);
				box->lockElapsed = lock.elapsed;
				box->lockInterval = lock.interval;
				box->lockAttempts = lock.attempts;
				if (tick != PickTick::Waiting)
				{
					++step.attempts;
					if (interactObserver_)
					{
						InteractEvent roll;
						roll.kind = InteractEvent::Kind::LockRoll;
						roll.who = id;
						roll.target = step.door;
						roll.action = static_cast<std::uint8_t>(InteractAction::Lockpick);
						roll.result = tick == PickTick::Unlocked ? 1 : 0;
						interactObserver_(roll);
					}
					combat_->rules().addXp(fighter.skills.lockpicking, tick == PickTick::Unlocked ? 1.2 : 0.24);
				}
				if (tick == PickTick::Unlocked)
				{
					box->locked = false;
					++box->version;
					end(InteractEvent::kDone, true);
					continue;
				}
				actor.set<state::DoorStep>(std::move(step));
				continue;
			}
			if (found.kind != InteractKind::Door)
			{
				end(InteractEvent::kGone, true);
				continue;
			}
			state::Door* door = flecs::entity(ecs.c_ptr(), found.entity).get_mut<state::Door>();
			const auto toPhase = [&](std::uint8_t phase)
			{
				step.phase = phase;
				step.remaining = doorPhaseSeconds(phase, *door);
			};
			// Starts what the planner chose against a locked door (the same choice at a walker's stop and in a wait).
			const auto startPlan = [&](const DoorDecision& decision, const state::Door&)
			{
				switch (decision.plan)
				{
				case DoorPlan::Unlock:
					step.thenOpen = true;
					toPhase(0);
					break;
				case DoorPlan::Bash:
					if (decision.equipFirst)
						equipItem(id, carriedWeapon(id), true); // UNSURE: instant
					toPhase(2);
					break;
				case DoorPlan::Pick:
					if (decision.unequipFirst)
						sheatheWeapon(id);
					step.thenOpen = true;
					step.attempts = 0;
					step.phase = 5; // the timer is on the lock (state::Door), not in the step
					break;
				case DoorPlan::Open:
					toPhase(1);
					break;
				case DoorPlan::Wait:
					toPhase(4);
					break;
				}
			};
			step.remaining -= kTickSeconds;
			bool running = true;
			switch (step.phase)
			{
			case 4: // waiting for a door the actor may not open
				step.waited += kTickSeconds;
				if (door->state == 0 || door->state == 3)
				{
					end(InteractEvent::kDone, true);
					running = false;
				} else if (door->state == 1) {
					toPhase(1);
				} else if (step.waited >= kDoorWaitCap) {
					end(InteractEvent::kUnreachable, false); // the walk is given up; the AI plans again
					running = false;
				} else if (static_cast<std::int64_t>(step.waited) != static_cast<std::int64_t>(step.waited - kTickSeconds)) {
					// Once a second the wait plans again: a gate crew may have thinned out, a weapon may have been picked up.
					startPlan(decide(id, fighter, *door, step.door), *door);
					if (step.phase != 4)
						step.waited = 0.0;
				}
				break;
			case 5: // pick the lock: one roll per attempt (door_plan.hpp)
				if (door->state != 2)
				{
					if (door->state == 1 && step.thenOpen)
						toPhase(1);
					else
					{
						end(door->state == 0 || door->state == 3 ? InteractEvent::kDone : InteractEvent::kRefused, true);
						running = false;
					}
				} else {
					if (!sufficientLockSkill(door->lockLevel, fighter.skills.lockpicking, false))
					{
						logTask("lock_too_hard member " + std::to_string(id) + " door " + std::to_string(step.door)); // no crime, no roll (k16.6.366)
						end(InteractEvent::kTooHard, true);
						running = false;
						break;
					}
					// Task_PickLock ticks the lock: its timer is the door's, shared with every picker and kept when this one stops.
					if (isForeignOwner(world_.ecs(), id, door->ownerSid))
						{ reportCrime(id, crime::kBurglary, door->ownerSid, 0); noteCaughtAct(id, 2, step.door); }
					LockTimer lock{door->lockElapsed, door->lockInterval, door->lockAttempts};
					const PickTick tick = tickLock(lock, kTickSeconds, pickChance(door->lockLevel, fighter.skills.lockpicking), door->key, id);
					door->lockElapsed = lock.elapsed;
					door->lockInterval = lock.interval;
					door->lockAttempts = lock.attempts;
					if (tick != PickTick::Waiting)
					{
						++step.attempts;
						if (interactObserver_)
						{
							InteractEvent roll;
							roll.kind = InteractEvent::Kind::LockRoll;
							roll.who = id;
							roll.target = step.door;
							roll.action = static_cast<std::uint8_t>(InteractAction::Lockpick);
							roll.result = tick == PickTick::Unlocked ? 1 : 0;
							interactObserver_(roll);
						}
						// XP per attempt: ((101 - s) / 101)^2 x race mult x (1.2 success / 0.24 fail); addXp is the square term.
						// OPEN: the race multiplier is not read yet (1.0).
						combat_->rules().addXp(fighter.skills.lockpicking, tick == PickTick::Unlocked ? 1.2 : 0.24);
					}
					if (tick == PickTick::Unlocked)
					{
						door->state = 1;
						++door->version;
						if (step.thenOpen)
						{
							toPhase(1);
						} else {
							end(InteractEvent::kDone, true);
							running = false;
						}
					}
				}
				break;
			case 6: // lock (LOCK_DOOR 77): the unlock's time run the other way. UNSURE: the exe's lock time is not read.
				if (door->state != 1)
				{
					end(door->state == 2 ? InteractEvent::kDone : InteractEvent::kRefused, true);
					running = false;
				} else if (step.remaining <= kPhaseEpsilon) {
					door->state = 2;
					++door->version;
					end(InteractEvent::kDone, true);
					running = false;
				}
				break;
			case 0: // unlock
				if (door->state != 2)
				{
					if (door->state == 1 && step.thenOpen)
						toPhase(1);
					else
					{
						end(door->state == 0 || door->state == 3 ? InteractEvent::kDone : InteractEvent::kRefused, true);
						running = false;
					}
				} else if (step.remaining <= kPhaseEpsilon) {
					door->state = 1;
					++door->version;
					if (step.thenOpen)
					{
						toPhase(1);
					} else {
						end(InteractEvent::kDone, true);
						running = false;
					}
				}
				break;
			case 1: // open
			case 3: // close
				if (!step.toggled && (step.phase == 1 ? door->state != 1 : door->state != 0))
				{
					end(step.phase == 1 && (door->state == 0 || door->state == 3) ? InteractEvent::kDone : InteractEvent::kRefused, true);
					running = false;
				} else {
					// The state (and the navmesh) flips when openness crosses 0.6; the slab keeps moving to 1.0 (1 / speed s in all).
					if (!step.toggled && step.remaining <= doorPhaseSeconds(step.phase, *door) * (1.0 - kDoorToggleOpenness) + kPhaseEpsilon)
					{
						// A close locks an `always locks` door again (gates).
						door->state = step.phase == 1 ? 0 : door->alwaysLocks ? 2 : 1;
						++door->version;
						step.toggled = true;
					}
					if (step.toggled && step.remaining <= kPhaseEpsilon)
					{
						end(InteractEvent::kDone, true);
						running = false;
					}
				}
				break;
			default: // bash
				if (door->state != 1 && door->state != 2)
				{
					end(door->state == 3 ? InteractEvent::kDone : InteractEvent::kRefused, true);
					running = false;
				} else if (step.remaining <= kPhaseEpsilon) {
					door->health -= bashDamage(fighter.weapon.blunt1, fighter.weapon.cut1);
					++door->version;
					// Task_BashDoor::vf2 (140348c10 L61): burglary against the building's faction, every blow.
					if (isForeignOwner(world_.ecs(), id, door->ownerSid))
						{ reportCrime(id, crime::kBurglary, door->ownerSid, 0); noteCaughtAct(id, 2, step.door); }
					if (door->health <= 0.0)
					{
						door->health = 0.0;
						door->state = 3; // broken: open for ever, the lock is gone
						end(InteractEvent::kDone, true);
						running = false;
					} else {
						step.remaining += kBashInterval;
					}
				}
				break;
			}
			if (running)
				actor.set<state::DoorStep>(std::move(step));
			continue;
		}
		if (down || !fighter.walking())
			continue;
		// A walker about to cross a shut door: the planner's step (DOORS.md section 6). It stops in front of the door when the
		// rest of its route runs through the door's centre. A character on its way to act on this very door is left alone.
		const state::InteractTask* task = actor.get<state::InteractTask>();
		for (const DoorRef& ref : shut)
		{
			if (task != nullptr && task->target == ref.id)
				continue;
			const state::Door* door = ref.entity.get<state::Door>();
			if (door == nullptr || (door->state != 1 && door->state != 2)
				|| std::hypot(fighter.x - door->x, fighter.z - door->z) > kDoorStopReach + 2.0 * std::fabs(fighter.forwardSpeed) * kTickSeconds
				|| std::fabs(fighter.y - door->y) > kInteractReachY)
				continue;
			if (door->state == 2 && fighter.playerControlled)
				continue; // a player's walk plans no unlock: the paint is a wall to it
			double nearest = segmentDistance(door->x, door->z, fighter.x, fighter.z,
				fighter.route[fighter.routeNext].x, fighter.route[fighter.routeNext].z);
			for (std::size_t k = fighter.routeNext + 1; k < fighter.route.size(); ++k)
				nearest = std::min(nearest, segmentDistance(door->x, door->z, fighter.route[k - 1].x, fighter.route[k - 1].z,
					fighter.route[k].x, fighter.route[k].z));
			if (nearest > kDoorPassReach)
				continue;
			state::DoorStep step;
			step.door = ref.id;
			{
				const DoorDecision decision = decide(id, fighter, *door, ref.id);
				switch (decision.plan)
				{
				case DoorPlan::Open:
					step.phase = 1;
					step.remaining = doorPhaseSeconds(1, *door);
					break;
				case DoorPlan::Unlock:
					step.phase = 0;
					step.thenOpen = true;
					step.remaining = doorPhaseSeconds(0, *door);
					break;
				case DoorPlan::Bash:
					if (decision.equipFirst)
						equipItem(id, carriedWeapon(id), true); // UNSURE: instant
					step.phase = 2;
					step.remaining = doorPhaseSeconds(2, *door);
					break;
				case DoorPlan::Pick:
					if (decision.unequipFirst)
						sheatheWeapon(id);
					step.phase = 5;
					step.thenOpen = true;
					break;
				case DoorPlan::Wait:
					step.phase = 4;
					step.remaining = doorPhaseSeconds(4, *door);
					break;
				}
			}
			step.route = fighter.route;
			step.routeNext = fighter.routeNext;
			step.routeSeq = fighter.routeSeq;
			step.routeApproach = fighter.routeApproach;
			fighter.clearRoute(state::Fighter::kClearDoorStep);
			actor.set<state::DoorStep>(std::move(step));
			break;
		}
	}
}

void Simulation::stepInteractions()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		const state::InteractTask* found = actor.get<state::InteractTask>();
		if (found == nullptr)
			continue;
		const state::InteractTask task = *found;
		const state::Fighter& fighter = members[i].fighter;
		InteractEvent done;
		done.kind = InteractEvent::Kind::Done;
		done.who = members[i].id;
		done.target = task.target;
		done.action = task.action;
		done.flags = task.flags;
		const auto finish = [&](std::uint8_t result)
		{
			done.result = result;
			actor.remove<state::InteractTask>();
			if (interactObserver_)
				interactObserver_(done);
		};
		if (fighter.dead || fighter.unconscious)
		{
			finish(InteractEvent::kIncapacitated);
			continue;
		}
		if (task.action == static_cast<std::uint8_t>(InteractAction::Talk) && navigation_ != nullptr)
		{
			// The target moved: a walk whose end is no longer within talk reach (or a walk that ended out of reach) picks its goal again.
			const InteractTarget now = findInteractTarget(ecs, combat_.get(), task.target, this);
			if (now.kind == InteractKind::Character
				&& (fighter.walking() ? std::hypot(fighter.route.back().x - now.x, fighter.route.back().z - now.z) > now.reachStopped
					: now.distanceTo(fighter.x, fighter.z) > now.reachStopped)
				&& checkInteract(ecs, *combat_, members[i].id, now, task.action, task.flags, gameData_.get(), this).empty())
			{
				const Route again = routeToInteract(*navigation_, fighter, now, task.action, false, body(fighter));
				if (again.found && !again.points.empty()
					&& std::hypot(again.points.back().x - now.x, again.points.back().z - now.z) <= now.reachStopped)
				{
					std::vector<state::RoutePoint> points;
					for (std::size_t k = 0; k < again.points.size(); ++k)
						points.push_back({again.points[k].x, again.heights[k], again.points[k].z});
					beginInteract(members[i].id, task.target, task.action, task.seq, std::move(points), again.approach, task.flags);
					continue;
				}
			}
		}
		if (task.action == static_cast<std::uint8_t>(InteractAction::Use) && navigation_ != nullptr && !fighter.walking())
		{
			// An operator moved out of the use range (pushed, teleported): it walks back to the approach point when that gets it in reach (kenshi-k16.19.5).
			const InteractTarget now = findInteractTarget(ecs, combat_.get(), task.target, this);
			const state::Producer* producer = now.kind == InteractKind::Producer ? flecs::entity(ecs.c_ptr(), now.entity).get<state::Producer>() : nullptr;
			if (producer != nullptr && !now.within(fighter)
				&& std::find(producer->operators.begin(), producer->operators.end(), members[i].id) != producer->operators.end())
			{
				const Route again = routeToInteract(*navigation_, fighter, now, task.action, false, body(fighter));
				if (again.found && !again.points.empty() && now.nearEnough(again.points.back().x, again.heights.back(), again.points.back().z, now.reachStopped))
				{
					std::vector<state::RoutePoint> points;
					for (std::size_t k = 0; k < again.points.size(); ++k)
						points.push_back({again.points[k].x, again.heights[k], again.points[k].z});
					beginInteract(members[i].id, task.target, task.action, task.seq, std::move(points), again.approach, task.flags);
					continue;
				}
			}
		}
		if (fighter.walking() || actor.has<state::DoorStep>())
			continue; // still on the way, or busy at a door (stepDoorSteps)
		if (const state::CarryStep* carry = actor.get<state::CarryStep>(); carry != nullptr && carry->phase != 1)
			continue; // lifting or putting down: stepCarry ends the order
		const InteractTarget target = findInteractTarget(ecs, combat_.get(), task.target, this);
		if (target.kind == InteractKind::None)
		{
			finish(InteractEvent::kGone);
			continue;
		}
		// The rules first: a door that locked while the actor walked refuses, whether or not the paint stopped the walk short.
		if (!checkInteract(ecs, *combat_, members[i].id, target, task.action, task.flags, gameData_.get(), this).empty())
		{
			finish(InteractEvent::kRefused);
			continue;
		}
		if (!target.arrived(fighter))
		{
			finish(InteractEvent::kUnreachable);
			continue;
		}
		flecs::entity entity(ecs.c_ptr(), target.entity);
		switch (static_cast<InteractAction>(task.action))
		{
		case InteractAction::Open:
		case InteractAction::Close:
		case InteractAction::Unlock:
		case InteractAction::Lockpick:
		case InteractAction::Bash:
		{
			// A timed step (stepDoorSteps); the order ends, with event 13, when it does.
			state::DoorStep step;
			step.door = task.target;
			if (entity.get<state::Container>() != nullptr)
			{
				// A container has one lock action, the pick (checkInteract refuses the door's others).
				step.phase = 5;
				step.remaining = 0.0;
			} else {
				const state::Door* door = entity.get<state::Door>();
				const auto action = static_cast<InteractAction>(task.action);
				step.phase = action == InteractAction::Open ? 1 : action == InteractAction::Close ? ((task.flags & kInteractFlagLock) != 0 ? 6 : 3)
					: action == InteractAction::Unlock ? 0 : action == InteractAction::Lockpick ? 5 : 2;
				step.remaining = step.phase == 5 ? 0.0 : doorPhaseSeconds(step.phase, *door); // a pick's timer is the lock's
			}
			actor.set<state::DoorStep>(std::move(step));
			continue;
		}
		case InteractAction::PickUp:
		{
			if (gameData_ == nullptr)
			{
				finish(InteractEvent::kRefused);
				continue;
			}
			const state::GroundItem item = *entity.get<state::GroundItem>();
			const grid::Rules rules(*gameData_);
			state::Inventory inventory;
			if (const state::Inventory* held = actor.get<state::Inventory>())
				inventory = *held;
			std::vector<state::Inventory::Entry> overflow = grid::settle(inventory, rules, members[i].id);
			state::Inventory::Entry stack;
			stack.itemSid = item.itemSid;
			stack.count = item.count;
			stack.quality = item.quality;
			stack.material = item.material;
			stack.colour = item.colour;
			stack.contents = item.contents;
			stack.stolen = item.stolen;
			if (stack.stolen.empty() && isForeignOwner(world_.ecs(), members[i].id, item.ownerSid))
				stack.stolen = item.ownerSid;
			clearReturnedMark(stack, factionOf(world_.ecs(), members[i].id), false);
			const std::uint32_t left = grid::autoplace(inventory, rules, std::move(stack));
			if (left == item.count)
			{
				// "I can't pick that up": the item stays on the ground.
				finish(InteractEvent::kRefused);
				continue;
			}
			// A material the record lists is picked once the instance is in a pack (settle seeds it from the carrier).
			grid::settle(inventory, rules, members[i].id);
			++inventory.version;
			storeInventory(actor, members[i].id, std::move(inventory), std::move(overflow));
			if (left > 0)
			{
				// A partial pick-up (stack of the same kind with room, a free cell): the rest stays on the ground.
				state::GroundItem rest = item;
				rest.count = left;
				++rest.version;
				entity.set<state::GroundItem>(std::move(rest));
			} else {
				entity.destruct();
			}
			if (isForeignOwner(world_.ecs(), members[i].id, item.ownerSid))
				reportTheft(members[i].id, task.target, item.ownerSid);
			if (interactObserver_)
			{
				InteractEvent picked;
				picked.kind = InteractEvent::Kind::PickedUp;
				picked.who = members[i].id;
				picked.target = task.target;
				picked.item = item.itemSid;
				picked.x = item.x;
				picked.y = item.y;
				picked.z = item.z;
				interactObserver_(picked);
			}
			if (left > 0)
			{
				finish(InteractEvent::kPartial);
				continue;
			}
			break;
		}
		case InteractAction::Loot:
		{
			if ((task.flags & kInteractFlagOpen) != 0)
				break; // open: nothing is taken; event 13 result 0 marks the arrival and the session starts sending the contents
			const LootResult loot = target.kind == InteractKind::Container ? lootContainer(members[i].id, entity) : lootBody(members[i].id, task.target);
			if (loot.left > 0)
			{
				finish(loot.took > 0 ? InteractEvent::kPartial : InteractEvent::kRefused);
				continue;
			}
			break;
		}
		case InteractAction::FirstAid:
			if (fighter.orderPause > 0 || treat(members[i].id, task.target))
				continue; // the treatment runs on, one tick at a time
			break;
		case InteractAction::Use:
		{
			// Operating a producer: the first arrival takes a slot and the order then stays until stepProducers ends it (full) or a
			// replacing order cancels it. The checks above already refused a full producer and a taken last slot.
			state::Producer producer = *entity.get<state::Producer>();
			// Facing (MQ16, operate_facing.md): every operate tick the facing target is normalise(building origin - operator), turned
			// at the normal 20 rad/s, not snapped. TODO: a producer with use nodes (benches, research, built mines) faces the nearest
			// free node's own heading instead; none of ours has nodes yet.
			if (state::Fighter* operatorBody = combat_->find(members[i].id);
				operatorBody != nullptr && std::hypot(producer.x - operatorBody->x, producer.z - operatorBody->z) > 1.0e-6)
				operatorBody->yaw = knav::turnToward(operatorBody->yaw, std::atan2(producer.x - operatorBody->x, producer.z - operatorBody->z),
					knav::kTurnRate, kTickSeconds);
			if (std::find(producer.operators.begin(), producer.operators.end(), members[i].id) == producer.operators.end())
			{
				producer.operators.push_back(members[i].id);
				producer.since[members[i].id] = clockTick();
				++producer.version;
				entity.set<state::Producer>(std::move(producer));
			}
			continue;
		}
		case InteractAction::Sleep:
		{
			// Arrived at the bed (order 60 action 9): lie down the way the resident routine does -- `Fighter::asleep`, the bed's
			// site as the routine target (the pose tail reads it), the response follows as event 13 result 0. A player character
			// with a Routine is skipped by the town scorer (5.1), so the component is safe here; the wake clears it.
			state::Fighter* sleeper = combat_->find(members[i].id);
			if (sleeper == nullptr)
				break;
			const routines::SiteEntry* bed = bedOf(task.target);
			if (bed == nullptr)
			{
				finish(InteractEvent::kGone);
				continue;
			}
			sleeper->clearRoute();
			sleeper->forwardSpeed = 0.0;
			sleeper->lateralSpeed = 0.0;
			if (sleeper->carrying != 0)
			{
				// A carrier at the bed (k16.6.217): the 99 put-in; the carrier itself stays up.
				putInBed(members[i].id, sleeper->carrying, *bed);
				mirrorFighter(members[i].id);
				break;
			}
			state::Routine routine;
			routine.target = {static_cast<std::uint8_t>(routines::SiteKind::Bed), bed->site.object, bed->site.slot};
			sleeper->asleep = true;
			actor.set<state::Routine>(routine);
			mirrorFighter(members[i].id);
			break;
		}
		case InteractAction::Carry:
		{
			// LIFT: the carrier stands still for the lift (stepCarry times it, attaches the body and sends event 13).
			state::Fighter* carrier = combat_->find(members[i].id);
			// The lift gate (Task_LiftSomething::vf1 14034f990, rwh.118): a target that is the carrier or whose RACE is not carriable
			// (Big-Bones, Leviathan) is flagImpossible - no carry, no event, no line; the order is dropped silently.
			const state::Fighter* lifted = combat_->find(task.target);
			const RaceCombat* liftedRace = lifted != nullptr ? combat_->data().race(lifted->raceSid) : nullptr;
			if (task.target == members[i].id || (liftedRace != nullptr && !liftedRace->carriable))
			{
				actor.remove<state::InteractTask>();
				continue;
			}
			carrier->clearRoute();
			carrier->forwardSpeed = 0.0;
			carrier->lateralSpeed = 0.0;
			carrier->yaw = std::atan2(target.x - carrier->x, target.z - carrier->z);
			actor.set<state::CarryStep>(state::CarryStep{task.target, 0, 0.0, false});
			continue;
		}
		case InteractAction::PutDown:
		{
			const state::CarryStep* carry = actor.get<state::CarryStep>();
			if (carry == nullptr || carry->phase != 1 || carry->target != task.target)
			{
				finish(InteractEvent::kRefused); // still lifting, or nobody carried
				continue;
			}
			state::CarryStep putting = *carry;
			putting.phase = 2;
			putting.elapsed = 0.0;
			state::Fighter* carrier = combat_->find(members[i].id);
			carrier->clearRoute();
			carrier->forwardSpeed = 0.0;
			carrier->lateralSpeed = 0.0;
			actor.set<state::CarryStep>(std::move(putting));
			continue;
		}
		case InteractAction::Talk:
		{
			// A merchant's character with the dialogue engine off: the trade opens on kDone (the Session). Anyone else hears the talk, a merchant
			// too when the engine is on (its trade reply opens the trade; no line to say falls back to the direct open).
			const flecs::entity squad = trade::squadOf(characterWithId(ecs, task.target));
			if (dialogue_ != nullptr || !squad.is_valid() || squad.get<state::TraderStock>() == nullptr || squad.get<state::Inventory>() == nullptr)
				talkTo(members[i].id, task.target, task.seq);
			break;
		}
		default:
			break;
		}
		finish(InteractEvent::kDone);
	}
}

} // namespace ksim

namespace ksim {

// SIMLINK TASK_PROGRESS 39: the timed tasks of living characters, the player's always and the NPCs' when `everyone`. A task is
// found by its state: a DoorStep (unlock, lock, lockpick), an InteractTask (a producer, first aid), a ProspectTask, an order
// (construction, a cage lock) or a Routine (restraining, processing). A row says by `flags` whether the original shows the bar.
std::vector<Simulation::TaskProgressRow> Simulation::taskProgress(bool everyone) const
{
	std::vector<TaskProgressRow> rows;
	if (combat_ == nullptr)
		return rows;
	const flecs::world& ecs = world_.ecs();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Fighter& fighter = members[i].fighter;
		if ((!fighter.playerControlled && !everyone) || fighter.dead || fighter.unconscious)
			continue;
		const flecs::entity actor = ecs.entity(combatEntities_[i]);
		const std::uint64_t id = members[i].id;
		TaskProgressRow row;
		row.id = id;
		row.flags = fighter.playerControlled ? kProgressPlayer : 0;
		const auto push = [&](TaskProgressRow& found)
		{
			found.flags |= row.flags;
			rows.push_back(found);
		};
		if (const state::DoorStep* step = actor.get<state::DoorStep>(); step != nullptr && (step->phase == 5 || step->phase == 0 || step->phase == 6))
		{
			if (step->phase != 5)
			{
				// The unlock and the lock of a shut door take kUnlockSeconds each (the lock's is OURS, UNSURE).
				row.kind = step->phase == 0 ? 2 : 3;
				row.progress = static_cast<float>(std::clamp(1.0 - step->remaining / kUnlockSeconds, 0.0, 1.0));
				row.rate = static_cast<float>(1.0 / kUnlockSeconds);
				row.target = step->door;
				row.flags |= kProgressOriginal;
				rows.push_back(row);
				continue;
			}
			const InteractTarget found = findInteractTarget(ecs, nullptr, step->door);
			const flecs::entity locked(ecs.c_ptr(), found.entity);
			if (const state::Container* box = found.kind == InteractKind::Container ? locked.get<state::Container>() : nullptr;
				box != nullptr && box->locked && box->lockInterval > 0.0)
			{
				row.kind = 1;
				row.progress = static_cast<float>(std::clamp(box->lockElapsed / box->lockInterval, 0.0, 1.0));
				row.rate = static_cast<float>(1.0 / box->lockInterval);
				row.value = static_cast<float>(pickChance(box->lockLevel, fighter.skills.lockpicking));
				row.target = step->door;
				row.flags |= kProgressOriginal;
				rows.push_back(row);
				continue;
			}
			const state::Door* door = found.kind == InteractKind::Door ? locked.get<state::Door>() : nullptr;
			if (door != nullptr && door->state == 2 && door->lockInterval > 0.0)
			{
				row.kind = 1;
				row.progress = static_cast<float>(std::clamp(door->lockElapsed / door->lockInterval, 0.0, 1.0));
				row.rate = static_cast<float>(1.0 / door->lockInterval);
				row.value = static_cast<float>(pickChance(door->lockLevel, fighter.skills.lockpicking));
				row.target = step->door;
				row.flags |= kProgressOriginal;
				rows.push_back(row);
				continue;
			}
		}
		if (const state::InteractTask* task = actor.get<state::InteractTask>(); task != nullptr && task->action == static_cast<std::uint8_t>(InteractAction::Use))
		{
			// A producer's bar is kind 9 when its functionality asks for one (`has progress bar when used`, MQ9; the original shows it),
			// else kind 10; the progress and rate are the producer's, shared by its operators.
			const InteractTarget found = findInteractTarget(ecs, nullptr, task->target);
			const state::Producer* producer = found.kind == InteractKind::Producer ? flecs::entity(ecs.c_ptr(), found.entity).get<state::Producer>() : nullptr;
			if (producer != nullptr && std::find(producer->operators.begin(), producer->operators.end(), id) != producer->operators.end())
			{
				row.kind = producer->progressBar ? 9 : 10;
				row.progress = static_cast<float>(producer->progress);
				row.rate = static_cast<float>(producerRate(*producer));
				row.target = task->target;
				if (producer->progressBar)
					row.flags |= kProgressOriginal;
				rows.push_back(row);
				continue;
			}
		}
		if (const state::InteractTask* task = actor.get<state::InteractTask>(); task != nullptr && task->action == static_cast<std::uint8_t>(InteractAction::FirstAid))
		{
			// The patient's bandage pools over the room they can take: how far along the treatment is. The rate is the kit's, and 0 while
			// the wounds are clotting (the bandage step runs after them).
			const state::Fighter* patient = combat_->find(task->target);
			if (patient != nullptr && !patient->dead)
			{
				double have = 0.0;
				double room = 0.0;
				for (const state::FighterPart& part : patient->parts)
				{
					have += part.health + part.pool;
					room += part.maxHealth - part.wear;
				}
				const ItemInfo* kit = fighter.kitCharges > 0.0 ? combat_->data().item(fighter.kitSid) : nullptr;
				row.kind = 16;
				row.progress = room > 0.0 ? static_cast<float>(std::clamp(have / room, 0.0, 1.0)) : 1.0f;
				if (kit != nullptr && patient->wounds.empty() && room > 0.0)
				{
					const CombatRules& rules = combat_->rules();
					row.rate = static_cast<float>(rules.bandageRate(CombatRules::aidSkill(fighter, patient == &fighter), kit->quality, false) / room);
				}
				row.target = task->target;
				rows.push_back(row);
				continue;
			}
		}
		if (const state::ProspectTask* task = actor.get<state::ProspectTask>())
		{
			row.kind = 4;
			row.progress = static_cast<float>(task->ticks) / static_cast<float>(prospect::kTicks);
			row.rate = static_cast<float>(1.0 / (prospect::kTicks * kTickSeconds));
			row.flags |= kProgressOriginal;
			rows.push_back(row);
			continue;
		}
		if (TaskProgressRow found; constructionProgress(id, found) || lockProgress(id, found))
		{
			push(found);
			continue;
		}
		if (const state::Routine* r = actor.get<state::Routine>(); r != nullptr && r->chainElapsed >= 0.0 && r->chainTarget != 0)
		{
			// Restraining (a capture chain's 166 step) and processing a new slave (goal 182): the elapsed seconds of the step.
			const bool restrain = r->step < r->plan.size() && r->plan[r->step] == goap::kChainTarget && r->chainElapsed < kRestrainSeconds;
			const bool process = r->goal == 182 && r->chainElapsed < kProcessSeconds;
			if (restrain || process)
			{
				const double seconds = restrain ? kRestrainSeconds : kProcessSeconds;
				row.kind = restrain ? 8 : 14;
				row.progress = static_cast<float>(r->chainElapsed / seconds);
				row.rate = static_cast<float>(1.0 / seconds);
				row.target = r->chainTarget;
				if (restrain)
					row.flags |= kProgressOriginal;
				rows.push_back(row);
			}
		}
	}
	std::sort(rows.begin(), rows.end(), [](const TaskProgressRow& a, const TaskProgressRow& b) { return a.id < b.id; });
	return rows;
}

double Simulation::cageLockLevel(std::uint64_t cageBuilding) const
{
	// The cage's lock is rolled like a door's from the building's faction (item28 b2 / GATES_CARRY_STATIC: UseableStuff ctor 1402a0900 runs the
	// same roll). The site's `owner faction ID`; a town-layout building without one takes the town's (not read here: "nofac" 40 / 10 - UNSURE).
	const BuildingSite* site = world_.buildingSite(cageBuilding);
	if (site == nullptr)
		return 40.0;
	return doorLockLevel(gameData_.get(), site->ownerFaction, site->buildingSid, cageBuilding);
}

double Simulation::cageLockHardness(std::uint64_t cageBuilding) const
{
	const BuildingSite* site = world_.buildingSite(cageBuilding);
	const kdata::Record* record = site != nullptr && gameData_ != nullptr ? gameData_->find(site->buildingSid) : nullptr;
	return record != nullptr ? static_cast<double>(record->intOr("hardness", 30)) : 30.0; // BUILDING `hardness` 15..90 (DOORS.md), 30 where unset
}

const state::DoorStep* Simulation::doorStep(std::uint64_t id) const
{
	return combat_ != nullptr && combat_->find(id) != nullptr ? fighterEntity(id).get<state::DoorStep>() : nullptr;
}

} // namespace ksim
