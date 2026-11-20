// Player squads (docs/reference/FACTION_SQUADS_LIVE.md): the player faction's ordered squad list, orders 70-75 (SIMLINK.md section 7)
// and the PLAYER_SQUADS view (section 6).

#include "ksim/core/simulation.hpp"
#include "ksim/save/newgame.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/world/squad_caches.hpp"
#include "ksim/ai/perception.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace ksim {

namespace {

// The FACTION a dismissed character joins (the exe's string-id 1233-gamedata.base, squads13_static.md; "Adventurers Guild" in
// gamedata.base, renamed "Tech Hunters" by rebirth.mod) and the squad template its new squad has (5006-squad.mod, "Wandering Squad").
constexpr const char* kDismissFaction = "1233-gamedata.base";
constexpr const char* kDismissSquadTemplate = "5006-squad.mod";
// The faction name reported when the merged data has no such record.
constexpr const char* kNoFaction = "nofac";

int intField(const kdata::Record& record, std::string_view key)
{
	if (const kdata::FieldInt* field = record.findInt(key))
		return field->value;
	throw std::runtime_error("squad limits: GLOBAL CONSTANTS has no key '" + std::string(key) + "'");
}

bool inList(const std::vector<std::uint64_t>& list, std::uint64_t id)
{
	return std::find(list.begin(), list.end(), id) != list.end();
}

} // namespace

SquadLimits squadLimits(const kdata::GameData& gameData)
{
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("CONSTANTS")))
	{
		if (record->name != "GLOBAL CONSTANTS")
			continue;
		SquadLimits limits;
		limits.faction = static_cast<std::uint16_t>(intField(*record, "max faction size"));
		limits.squad = static_cast<std::uint16_t>(intField(*record, "max squad size"));
		limits.squads = static_cast<std::uint16_t>(intField(*record, "max squads"));
		return limits;
	}
	throw std::runtime_error("squad limits: no GLOBAL CONSTANTS record");
}

bool validSquadName(const std::string& text)
{
	std::size_t characters = 0;
	for (std::size_t i = 0; i < text.size();)
	{
		const unsigned char lead = static_cast<unsigned char>(text[i]);
		std::size_t length = 1;
		std::uint32_t code = lead;
		std::uint32_t least = 0;
		if (lead >= 0xF0 && lead <= 0xF4)
		{
			length = 4;
			code = lead & 0x07u;
			least = 0x10000;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			length = 3;
			code = lead & 0x0Fu;
			least = 0x800;
		} else if (lead >= 0xC2 && lead <= 0xDF) {
			length = 2;
			code = lead & 0x1Fu;
			least = 0x80;
		} else if (lead >= 0x80) {
			return false;
		}
		if (i + length > text.size())
			return false;
		for (std::size_t k = 1; k < length; ++k)
		{
			const unsigned char next = static_cast<unsigned char>(text[i + k]);
			if ((next & 0xC0u) != 0x80u)
				return false;
			code = (code << 6) | (next & 0x3Fu);
		}
		if (code < least || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF))
			return false;
		i += length;
		if (++characters > kSquadNameMax)
			return false;
	}
	return true;
}

bool PlayerSquadsView::operator==(const PlayerSquadsView& other) const
{
	if (faction != other.faction || factionName != other.factionName || factionCount != other.factionCount
		|| money != other.money || limits.faction != other.limits.faction || limits.squad != other.limits.squad || limits.squads != other.limits.squads
		|| squads.size() != other.squads.size())
		return false;
	for (std::size_t i = 0; i < squads.size(); ++i)
	{
		if (squads[i].id != other.squads[i].id || squads[i].name != other.squads[i].name || squads[i].members != other.squads[i].members)
			return false;
	}
	return true;
}

SquadLimits Simulation::squadLimits() const
{
	if (gameData_ == nullptr)
		throw std::logic_error("squadLimits: the simulation has no game data");
	if (squadLimitsFor_ != gameData_.get())
	{
		squadLimitsCache_ = ksim::squadLimits(*gameData_);
		squadLimitsFor_ = gameData_.get();
	}
	return squadLimitsCache_;
}

namespace {

struct FactionHandle
{
	flecs::entity entity;
	state::PlayerFaction faction;
	std::uint64_t id = 0;
};

// The first (lowest Id) player faction: this server has one session, which owns it (MULTIPLAYER.md: one PlayerFaction per user).
FactionHandle firstPlayerFaction(const flecs::world& ecs)
{
	FactionHandle found;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::PlayerFaction& faction)
	{
		if (found.id == 0 || id.value < found.id)
		{
			found.entity = entity;
			found.faction = faction;
			found.id = id.value;
		}
	});
	return found;
}

FactionHandle firstPlayerFaction(const World& world)
{
	FactionHandle found;
	world.each([&](flecs::entity entity, const state::Id& id, const state::PlayerFaction& faction)
	{
		if (found.id == 0 || id.value < found.id)
		{
			found.entity = entity;
			found.faction = faction;
			found.id = id.value;
		}
	});
	return found;
}

flecs::entity squadEntity(const flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found{};
	ecs.each([&](flecs::entity entity, const state::Id& key, const state::PlayerSquad&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

} // namespace

std::uint64_t Simulation::createPlayerFaction(const std::string& factionSid)
{
	flecs::world& ecs = ecsMutable();
	flecs::entity found{};
	ecs.each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (faction.templateSid == factionSid)
			found = entity;
	});
	if (!found.is_valid())
	{
		// A world without faction instances (Simulation::load, the tests' bare worlds): the player's faction gets its own.
		if (gameData_ == nullptr || gameData_->find(factionSid) == nullptr)
			throw std::runtime_error("createPlayerFaction: no faction " + factionSid);
		found = state::createInstance(ecs);
		found.set<state::FactionState>(state::FactionState{factionSid});
	}
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(factionSid) : nullptr;
	state::PlayerFaction faction;
	faction.name = record != nullptr ? record->name : factionSid;
	if (gameData_ != nullptr)
		faction.research = startResearch(*gameData_, {});
	found.set<state::PlayerFaction>(std::move(faction));
	seedKnownFactions(found);
	return found.get<state::Id>()->value;
}

void Simulation::markPlayerFactionLive()
{
	FactionHandle handle = firstPlayerFaction(ecsMutable());
	if (handle.id == 0 || !handle.faction.startoff.empty())
		return;
	handle.faction.startoff = std::string(state::kScenarioStartoff);
	handle.entity.set<state::PlayerFaction>(std::move(handle.faction));
}

std::uint64_t Simulation::addPlayerSquad(const std::optional<std::string>& name)
{
	flecs::world& ecs = ecsMutable();
	FactionHandle handle = firstPlayerFaction(ecs);
	if (handle.id == 0)
		throw std::logic_error("addPlayerSquad: no player faction");
	state::PlayerSquad squad;
	if (name)
		squad.name = *name;
	else
		squad.name = handle.faction.name + "_" + std::to_string(handle.faction.squadCounter++);
	flecs::entity entity = state::createInstance(ecs);
	const std::uint64_t id = entity.get<state::Id>()->value;
	entity.set<state::PlayerSquad>(std::move(squad));
	handle.faction.squads.push_back(id);
	handle.entity.set<state::PlayerFaction>(std::move(handle.faction));
	return id;
}

void Simulation::addPlayerMember(std::uint64_t squad, std::uint64_t character)
{
	flecs::entity entity = squadEntity(world_.ecs(), squad);
	if (!entity.is_valid())
		throw std::logic_error("addPlayerMember: no player squad " + std::to_string(squad));
	state::PlayerSquad copy = *entity.get<state::PlayerSquad>();
	copy.members.push_back(character);
	entity.set<state::PlayerSquad>(std::move(copy));
}

std::string Simulation::checkSquadOrder(const SquadOrder& order) const
{
	const flecs::world& ecs = world_.ecs();
	const FactionHandle handle = firstPlayerFaction(ecs);
	if (handle.id == 0)
		return "not_allowed";
	const auto& squads = handle.faction.squads;
	const auto ownSquad = [&](std::uint64_t id) { return inList(squads, id); };
	const auto membersOf = [&](std::uint64_t id) -> const std::vector<std::uint64_t>&
	{
		return squadEntity(ecs, id).get<state::PlayerSquad>()->members;
	};
	// The squad that holds a character, 0 = none.
	const auto squadOf = [&](std::uint64_t character) -> std::uint64_t
	{
		for (const std::uint64_t squad : squads)
		{
			if (inList(membersOf(squad), character))
				return squad;
		}
		return 0;
	};
	// A character the order may name: a living fighter in one of the player's squads.
	const auto characterReason = [&](std::uint64_t character) -> std::string
	{
		const state::Fighter* fighter = combat_ != nullptr ? combat_->find(character) : nullptr;
		if (fighter == nullptr)
			return "unknown_id";
		if (squadOf(character) == 0)
			return "not_player_controlled";
		return fighter->dead ? "dead" : "";
	};

	switch (order.kind)
	{
	case 70:
		return squads.size() >= squadLimits().squads ? "not_allowed" : "";
	case 71:
		if (!ownSquad(order.squad))
			return "bad_target";
		return !membersOf(order.squad).empty() || squads.size() <= 1 ? "not_allowed" : "";
	case 72:
		if (!ownSquad(order.squad))
			return "bad_target";
		return validSquadName(order.name) ? "" : "bad_value";
	case 73:
	{
		if (const std::string reason = characterReason(order.character); !reason.empty())
			return reason;
		if (!ownSquad(order.squad))
			return "bad_target";
		if (order.swap > 1)
			return "bad_value";
		const std::vector<std::uint64_t>& target = membersOf(order.squad);
		if (order.swap == 1)
			return order.index < target.size() ? "" : "bad_value";
		if (squadOf(order.character) != order.squad && target.size() >= squadLimits().squad)
			return "not_allowed";
		return "";
	}
	case 74:
		return ownSquad(order.squad) ? "" : "bad_target";
	case 75:
	{
		if (const std::string reason = characterReason(order.character); !reason.empty())
			return reason;
		return playerSquads().factionCount <= 1 ? "not_allowed" : "";
	}
	case 85:
	{
		// NEW_GAME's bad_name rules (wire safety: the exe has no length, trim or empty check), then the exe's one check: an exact,
		// case-sensitive match with another faction's display name (a rename to the current name passes, as in the exe).
		if (std::string reason = checkFactionName(order.name); !reason.empty())
			return reason;
		if (order.name == handle.faction.name || gameData_ == nullptr)
			return "";
		const state::FactionState* own = handle.entity.get<state::FactionState>();
		for (const kdata::Record* record : gameData_->byType(kworld::itemTypeByName("FACTION")))
		{
			if (record->name == order.name && (own == nullptr || record->stringID != own->templateSid))
				return "name_taken";
		}
		return "";
	}
	default:
		return "bad_value";
	}
}

std::string Simulation::runSquadOrder(const SquadOrder& order, bool commit)
{
	std::string reason = checkSquadOrder(order);
	if (!reason.empty() || !commit)
		return reason;
	flecs::world& ecs = ecsMutable();
	FactionHandle handle = firstPlayerFaction(ecs);
	const auto load = [&](std::uint64_t id) { return *squadEntity(ecs, id).get<state::PlayerSquad>(); };
	const auto store = [&](std::uint64_t id, state::PlayerSquad squad) { squadEntity(ecs, id).set<state::PlayerSquad>(std::move(squad)); };
	const auto squadOf = [&](std::uint64_t character) -> std::uint64_t
	{
		for (const std::uint64_t squad : handle.faction.squads)
		{
			if (inList(load(squad).members, character))
				return squad;
		}
		return 0;
	};

	switch (order.kind)
	{
	case 70:
		addPlayerSquad();
		break;
	case 71:
	{
		squadEntity(ecs, order.squad).destruct();
		auto& squads = handle.faction.squads;
		squads.erase(std::find(squads.begin(), squads.end(), order.squad));
		handle.entity.set<state::PlayerFaction>(std::move(handle.faction));
		break;
	}
	case 72:
	{
		state::PlayerSquad squad = load(order.squad);
		squad.name = order.name;
		store(order.squad, std::move(squad));
		break;
	}
	case 85:
		handle.faction.name = order.name; // an identical rename sets the same value; squads keep the names they have
		handle.entity.set<state::PlayerFaction>(std::move(handle.faction));
		break;
	case 73:
	{
		const std::uint64_t from = squadOf(order.character);
		state::PlayerSquad source = load(from);
		if (order.swap == 1)
		{
			// Swaps with the member at the index: the two exchange squads and slots (the same squad: positions).
			if (from == order.squad)
			{
				std::swap(*std::find(source.members.begin(), source.members.end(), order.character), source.members[order.index]);
				store(from, std::move(source));
			} else {
				state::PlayerSquad target = load(order.squad);
				std::swap(*std::find(source.members.begin(), source.members.end(), order.character), target.members[order.index]);
				store(from, std::move(source));
				store(order.squad, std::move(target));
			}
			break;
		}
		// Insert: out of its squad first (the same squad: the index counts the others), then in at the index.
		source.members.erase(std::find(source.members.begin(), source.members.end(), order.character));
		state::PlayerSquad target = from == order.squad ? source : load(order.squad);
		const std::size_t at = std::min<std::size_t>(order.index, target.members.size());
		target.members.insert(target.members.begin() + static_cast<std::ptrdiff_t>(at), order.character);
		if (from != order.squad)
			store(from, std::move(source));
		store(order.squad, std::move(target));
		break;
	}
	case 74:
	{
		auto& squads = handle.faction.squads;
		squads.erase(std::find(squads.begin(), squads.end(), order.squad));
		const std::size_t at = std::min<std::size_t>(order.index, squads.size());
		squads.insert(squads.begin() + static_cast<std::ptrdiff_t>(at), order.squad);
		handle.entity.set<state::PlayerFaction>(std::move(handle.faction));
		break;
	}
	case 75:
	{
		const std::uint64_t from = squadOf(order.character);
		state::PlayerSquad source = load(from);
		source.members.erase(std::find(source.members.begin(), source.members.end(), order.character));
		store(from, std::move(source));

		// The character stops being the player's: no orders, nobody following it, a faction and a squad of its own.
		state::Fighter* fighter = combat_->find(order.character);
		fighter->playerControlled = false;
		cancelOrders(order.character);
		CombatArena::endFollow(*fighter);
		fighter->clearRoute();
		fighter->drive = state::DriveMode::Stop;
		fighter->driveStrafe = 0;
		fighter->orderTarget = 0;
		fighter->orderStop = true;
		fighter->hold = false;
		for (const CombatArena::Member& member : combat_->members())
		{
			if (member.fighter.followTarget == order.character)
				CombatArena::endFollow(*combat_->find(member.id));
		}
		flecs::entity character = fighterEntity(order.character);
		character.set<state::Fighter>(*fighter);

		const kdata::Record* faction = gameData_ != nullptr ? gameData_->find(kDismissFaction) : nullptr;
		flecs::entity factionState{};
		ecs.each([&](flecs::entity entity, const state::FactionState& state)
		{
			if (faction != nullptr && state.templateSid == faction->stringID)
				factionState = entity;
		});
		if (faction != nullptr)
			character.set<state::Allegiance>(state::Allegiance{faction->stringID});
		else
			character.remove<state::Allegiance>();

		const bool templated = gameData_ != nullptr && gameData_->find(kDismissSquadTemplate) != nullptr;
		flecs::entity squad = state::createInstance(ecs);
		state::SquadInstance instance;
		instance.templateSid = templated ? kDismissSquadTemplate : "";
		instance.faction = state::EntityRef{factionState.is_valid() ? factionState.id() : 0};
		instance.source = state::SpawnSource::Dismissed;
		squad.set<state::SquadInstance>(std::move(instance));
		squad.set<state::Position>(state::Position{fighter->x, fighter->z});
		character.add<state::MemberOf>(squad);
		removed_ = true; // the region engine and the AI pick the new squad up (instancesChanged after the commands)

		if (dismissObserver_)
		{
			DismissEvent event;
			event.tick = clockTick();
			event.character = order.character;
			event.factionSid = faction != nullptr ? faction->stringID : kNoFaction;
			event.factionName = faction != nullptr ? faction->name : kNoFaction;
			event.squad = squad.get<state::Id>()->value;
			dismissObserver_(event);
		}
		break;
	}
	default:
		break;
	}
	return {};
}

// DA_JOIN_SQUAD_* (DIALOGUE.md 5, 6): the character leaves its NPC squad and becomes a member of the player faction's squad. The
// effect's command and the checks re-run at apply; the player faction is this server's one (MULTIPLAYER.md).
std::string Simulation::checkChangeSquad(std::uint64_t character, std::uint64_t squad) const
{
	const flecs::world& ecs = world_.ecs();
	const FactionHandle handle = firstPlayerFaction(ecs);
	if (handle.id == 0 || handle.faction.squads.empty())
		return "not_allowed";
	const state::Fighter* fighter = combat_ != nullptr ? combat_->find(character) : nullptr;
	if (fighter == nullptr)
		return "unknown_id";
	if (fighter->dead)
		return "dead";
	if (fighter->unconscious || fighter->down || fighter->carriedBy != 0)
		return "not_allowed";
	if (!inList(handle.faction.squads, squad))
	{
		if (squad != 0)
			return "bad_target";
		squad = handle.faction.squads.front();
	}
	const state::PlayerSquad& target = *squadEntity(ecs, squad).get<state::PlayerSquad>();
	// A character the player already has in one of its squads: joining the same one is nothing; joining another is a move, which does not
	// grow the roster.
	bool joined = false;
	for (const std::uint64_t id : handle.faction.squads)
	{
		if (id == squad)
		{
			if (inList(target.members, character))
				return "";
			continue;
		}
		joined = joined || inList(squadEntity(ecs, id).get<state::PlayerSquad>()->members, character);
	}
	if (!joined && playerSquads().factionCount >= squadLimits().faction)
		return "not_allowed";
	if (target.members.size() >= squadLimits().squad)
		return "not_allowed";
	return "";
}

std::string Simulation::runChangeSquad(std::uint64_t character, std::uint64_t squad, bool commit)
{
	const std::string reason = checkChangeSquad(character, squad);
	if (!reason.empty() || !commit)
		return reason;
	flecs::world& ecs = ecsMutable();
	const FactionHandle handle = firstPlayerFaction(ecs);
	if (squad == 0)
		squad = handle.faction.squads.front();
	const auto load = [&](std::uint64_t id) { return *squadEntity(ecs, id).get<state::PlayerSquad>(); };
	const auto store = [&](std::uint64_t id, state::PlayerSquad copy) { squadEntity(ecs, id).set<state::PlayerSquad>(std::move(copy)); };
	// Out of a player squad first (a move), then in at the end of the target's list.
	std::uint64_t from = 0;
	for (const std::uint64_t id : handle.faction.squads)
	{
		if (inList(load(id).members, character))
		{
			from = id;
			break;
		}
	}
	if (from == squad)
		return {};

	state::Fighter* fighter = combat_->find(character);
	flecs::entity entity = fighterEntity(character);
	if (from != 0)
	{
		state::PlayerSquad source = load(from);
		source.members.erase(std::find(source.members.begin(), source.members.end(), character));
		store(from, std::move(source));
	}
	state::PlayerSquad target = load(squad);
	target.members.push_back(character);
	store(squad, std::move(target));

	if (!fighter->playerControlled)
	{
		// A weapon sheathed on the back (Inventory::kSheathed) comes back to the hand first: a player character is never sheathed, so its INVENTORY
		// never sends slot 15, which SIMLINK does not define (k16.6.124; a client-approved back slot would replace this).
		if (const state::Inventory* held = entity.get<state::Inventory>())
			if (std::any_of(held->items.begin(), held->items.end(), [](const state::Inventory::Entry& e) { return e.slot == state::Inventory::kSheathed; }))
				drawWeapon(character);
		// A player member's AI: no squad package, no routine, no job list and no orders; the character leaves the NPC squad.
		entity.remove<state::MemberOf>(flecs::Wildcard);
		entity.remove<state::Routine>();
		entity.remove<state::MemberRole>();
		entity.remove<state::Jobs>();
		entity.remove<state::OrderQueue>();
		entity.remove<state::InteractTask>();
		entity.remove<state::ProspectTask>();
		cancelOrders(character);
		CombatArena::endFollow(*fighter);
		fighter->clearRoute();
		fighter->drive = state::DriveMode::Stop;
		fighter->driveStrafe = 0;
		fighter->orderTarget = 0;
		fighter->orderStop = true;
		fighter->hold = false;
		fighter->playerControlled = true;
		fighter->team = 0;
		fighter->faction = 0;
		fighter->squad = (std::uint64_t{1} << 62) | character;
		fighter->attacksNeutrals = false;
		fighter->uniqueSquad = false;
		fighter->hostileSquads.clear();
		fighter->aggroStart = 0;
		fighter->aggroEnd = 24;
		fighter->homeBuilding = 0;
		touchHomes();
		fighter->houseLocked = false;
		entity.set<state::Fighter>(*fighter);
		const state::FactionState* own = handle.entity.get<state::FactionState>();
		entity.set<state::Allegiance>(state::Allegiance{own != nullptr ? own->templateSid : std::string()});
		removed_ = true; // the squad's composition changed: the region engine and the AI rebuild their groups
	}
	return {};
}

bool Simulation::adjustPlayerMoney(std::int64_t delta)
{
	flecs::world& ecs = ecsMutable();
	const FactionHandle handle = firstPlayerFaction(ecs);
	if (handle.id == 0)
		return false;
	state::PlayerFaction player = handle.faction;
	player.money += delta;
	handle.entity.set<state::PlayerFaction>(std::move(player));
	return true;
}

bool Simulation::adjustFactionCash(const std::string& factionSid, std::int64_t delta)
{
	if (factionSid.empty())
		return false;
	flecs::entity found{};
	ecsMutable().each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (faction.templateSid == factionSid)
			found = entity;
	});
	if (!found.is_valid())
		return false;
	state::FactionState copy = *found.get<state::FactionState>();
	copy.cash += delta;
	found.set<state::FactionState>(std::move(copy));
	return true;
}

bool Simulation::separateToOwnSquad(std::uint64_t character)
{
	const state::Fighter* held = combat_ != nullptr ? combat_->find(character) : nullptr;
	if (held == nullptr || held->dead)
		return false;
	flecs::world& ecs = ecsMutable();
	// A player member: a new player squad of its own (the exe's split for a player character; T when S is the player).
	FactionHandle handle = firstPlayerFaction(ecs);
	for (const std::uint64_t id : handle.faction.squads)
	{
		const state::PlayerSquad& squad = *squadEntity(ecs, id).get<state::PlayerSquad>();
		if (!inList(squad.members, character))
			continue;
		if (squad.members.size() <= 1)
			return true; // already alone: the exe is a no-op
		if (handle.faction.squads.size() >= squadLimits().squads)
			return false;
		state::PlayerSquad source = squad;
		source.members.erase(std::find(source.members.begin(), source.members.end(), character));
		squadEntity(ecs, id).set<state::PlayerSquad>(std::move(source));
		addPlayerMember(addPlayerSquad(), character);
		return true;
	}
	// An NPC: a new SquadInstance of its own, under its own side (the old squad's template and source, no home: a split is not the old
	// squad's home). UNSURE: the template and source the exe gives the new squad.
	flecs::entity entity = fighterEntity(character);
	const flecs::entity old = entity.target<state::MemberOf>();
	if (old.is_valid())
	{
		std::size_t mates = 0;
		ecs.query_builder<const state::Id>().with<state::MemberOf>(old).build().each([&](const state::Id&) { ++mates; });
		if (mates <= 1)
			return true; // already alone
	}
	state::SquadInstance instance;
	if (old.is_valid())
	{
		if (const state::SquadInstance* previous = old.get<state::SquadInstance>())
		{
			instance.templateSid = previous->templateSid;
			instance.source = previous->source;
		}
	}
	if (const state::Allegiance* side = entity.get<state::Allegiance>(); side != nullptr && !side->factionSid.empty())
	{
		ecs.each([&](flecs::entity faction, const state::FactionState& state)
		{
			if (state.templateSid == side->factionSid)
				instance.faction = state::EntityRef{faction.id()};
		});
	}
	state::Fighter* fighter = combat_->find(character);
	flecs::entity squad = state::createInstance(ecs);
	squad.set<state::SquadInstance>(std::move(instance));
	squad.set<state::Position>(state::Position{fighter->x, fighter->z});
	entity.remove<state::MemberOf>(flecs::Wildcard);
	entity.add<state::MemberOf>(squad);
	fighter->squad = squad.get<state::Id>()->value;
	mirrorFighter(character);
	removed_ = true; // the region engine and the AI rebuild their groups
	return true;
}

void Simulation::stepPlayerSquads()
{
	if (combat_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::PlayerSquad>() == 0) // the uncached ecs.each() below walks every table
		return;
	std::vector<std::pair<flecs::entity, state::PlayerSquad>> changed;
	ecs.each([&](flecs::entity entity, const state::PlayerSquad& squad)
	{
		state::PlayerSquad kept = squad;
		kept.members.erase(std::remove_if(kept.members.begin(), kept.members.end(), [&](std::uint64_t id)
		{
			const state::Fighter* fighter = combat_->find(id);
			return fighter == nullptr || fighter->dead;
		}), kept.members.end());
		if (kept.members.size() != squad.members.size())
			changed.emplace_back(entity, std::move(kept));
	});
	for (auto& [entity, squad] : changed)
		entity.set<state::PlayerSquad>(std::move(squad));
}

void Simulation::registerSquadCaches()
{
	squadCaches_ = std::make_shared<SquadCaches>();
	flecs::world& ecs = ecsMutable();
	const std::shared_ptr<SquadCaches> caches = squadCaches_;
	// One observer per component, each marking what reads it. Id and the player components carry data (OnSet fires on the first set too);
	// Fighter, MemberOf, TraderStock and Inventory are only read for their presence, so add / remove (an entity's deletion included) is the event.
	ecs.observer<const state::Id>().event(flecs::OnSet).event(flecs::OnRemove).each([caches](const state::Id&)
	{
		caches->characterDirty = caches->viewDirty = caches->shopDirty = true;
	});
	ecs.observer<const state::PlayerSquad>().event(flecs::OnSet).event(flecs::OnRemove).each([caches](const state::PlayerSquad&)
	{
		caches->characterDirty = caches->viewDirty = true;
	});
	ecs.observer<const state::PlayerFaction>().event(flecs::OnSet).event(flecs::OnRemove).each([caches](const state::PlayerFaction&)
	{
		caches->viewDirty = true;
	});
	ecs.observer<const state::Fighter>().event(flecs::OnAdd).event(flecs::OnRemove).each([caches](const state::Fighter&)
	{
		caches->characterDirty = caches->shopDirty = true;
	});
	ecs.observer().with<state::MemberOf>(flecs::Wildcard).event(flecs::OnAdd).event(flecs::OnRemove).each([caches](flecs::entity)
	{
		caches->characterDirty = caches->shopDirty = true;
	});
	ecs.observer<const state::TraderStock>().event(flecs::OnAdd).event(flecs::OnRemove).each([caches](const state::TraderStock&)
	{
		caches->shopDirty = true;
	});
	ecs.observer<const state::Inventory>().event(flecs::OnAdd).event(flecs::OnRemove).each([caches](const state::Inventory&)
	{
		caches->shopDirty = true;
	});
}

void Simulation::touchHomes() noexcept
{
	squadCaches_->shopDirty = true;
}

PlayerSquadsView Simulation::playerSquads() const
{
	SquadCaches& caches = *squadCaches_;
	if (caches.viewDirty)
	{
		caches.viewDirty = false;
		caches.view = PlayerSquadsView{};
		const FactionHandle handle = firstPlayerFaction(world_);
		if (handle.id != 0)
		{
			PlayerSquadsView& view = caches.view;
			view.faction = handle.entity.get<state::Id>()->value;
			view.factionName = handle.faction.name;
			view.money = handle.faction.money;
			view.factionSid = handle.entity.get<state::FactionState>()->templateSid;
			// One pass over the squads, not one per listed squad (squadEntity scans them all).
			std::unordered_map<std::uint64_t, const state::PlayerSquad*> byId;
			world_.each([&](const state::Id& key, const state::PlayerSquad& squad) { byId.emplace(key.value, &squad); });
			for (const std::uint64_t id : handle.faction.squads)
			{
				const state::PlayerSquad& squad = *byId.at(id);
				view.squads.push_back({id, squad.name, squad.members});
			}
		}
	}
	PlayerSquadsView view = caches.view;
	if (view.faction == 0)
		return view;
	view.limits = squadLimits();
	// The living count reads the arena's dead flags, which no ECS event tells of: counted on every call (members only).
	for (const PlayerSquadsView::Squad& squad : view.squads)
	{
		for (const std::uint64_t member : squad.members)
		{
			const state::Fighter* fighter = combat_ != nullptr ? combat_->find(member) : nullptr;
			if (fighter == nullptr || !fighter->dead)
				++view.factionCount;
		}
	}
	return view;
}

const std::unordered_map<std::uint64_t, std::uint64_t>& Simulation::characterSquads() const
{
	SquadCaches& caches = *squadCaches_;
	if (!caches.characterDirty)
		return caches.characterSquads;
	caches.characterDirty = false;
	if (!playerSquadQuery_)
	{
		flecs::world& writable = const_cast<flecs::world&>(world_.ecs()); // building a query registers it; nothing is changed
		playerSquadQuery_.emplace(writable.query<const state::Id, const state::PlayerSquad>());
		fighterIdQuery_.emplace(writable.query<const state::Id, const state::Fighter>());
	}
	std::unordered_map<std::uint64_t, std::uint64_t>& squads = caches.characterSquads;
	squads.clear();
	playerSquadQuery_->each([&](const state::Id& id, const state::PlayerSquad& squad)
	{
		for (const std::uint64_t member : squad.members)
			squads[member] = id.value;
	});
	fighterIdQuery_->each([&](flecs::entity character, const state::Id& id, const state::Fighter&)
	{
		const flecs::entity squad = character.target<state::MemberOf>();
		if (squad.is_valid() && squad.get<state::Id>() != nullptr)
			squads[id.value] = squad.get<state::Id>()->value;
	});
	return squads;
}

} // namespace ksim

namespace ksim
{

namespace
{

// Which factions FACTION_RELATIONS lists (SIMLINK.md, kinspect 49 findings/ui_captures/faction.md): the player faction's known set,
// minus FACTION records with the bool `not real`, minus the player faction itself, minus GENERATION_TEMPLATE_FACTION. This is the ONE
// place that decides; swap the rule here and nowhere else.
bool listsRelation(const kdata::Record& record, const std::string& playerSid, const state::PlayerFaction& player)
{
	return std::find(player.known.begin(), player.known.end(), record.stringID) != player.known.end()
		&& record.stringID != playerSid && record.stringID != "GENERATION_TEMPLATE_FACTION" && !record.boolOr("not real", false);
}

// Case-insensitive (ASCII) by display name, ties shorter first (the exe's FACTION tab order).
bool nameBefore(const std::string& a, const std::string& b)
{
	const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
	const std::size_t common = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < common; ++i)
	{
		if (lower(static_cast<unsigned char>(a[i])) != lower(static_cast<unsigned char>(b[i])))
			return lower(static_cast<unsigned char>(a[i])) < lower(static_cast<unsigned char>(b[i]));
	}
	return a.size() < b.size();
}

} // namespace

std::vector<Simulation::FactionRelationRow> Simulation::factionRelations() const
{
	std::vector<FactionRelationRow> rows;
	const flecs::world& ecs = world_.ecs();
	const FactionHandle handle = firstPlayerFaction(ecs);
	const state::FactionState* own = handle.id == 0 ? nullptr : handle.entity.get<state::FactionState>();
	if (own == nullptr || gameData_ == nullptr)
		return rows;
	std::unordered_map<std::string, flecs::entity> byTemplate;
	ecs.each([&](flecs::entity entity, const state::FactionState& faction) { byTemplate.emplace(faction.templateSid, entity); });
	for (const kdata::Record* record : gameData_->byType(kworld::itemTypeByName("FACTION")))
	{
		const auto found = byTemplate.find(record->stringID);
		if (found != byTemplate.end() && listsRelation(*record, own->templateSid, handle.faction))
			rows.push_back({record->stringID, record->name, factionRelation(*gameData_, found->second, handle.entity)});
	}
	std::stable_sort(rows.begin(), rows.end(), [](const FactionRelationRow& a, const FactionRelationRow& b) { return nameBefore(a.name, b.name); });
	return rows;
}

void Simulation::seedKnownFactions(flecs::entity player)
{
	const state::PlayerFaction* held = player.get<state::PlayerFaction>();
	if (held == nullptr)
		return;
	state::PlayerFaction copy = *held;
	ecsMutable().each([&](flecs::entity other, const state::FactionState& faction)
	{
		if (other.id() != player.id() && player.get<state::Relation>(other) != nullptr
			&& std::find(copy.known.begin(), copy.known.end(), faction.templateSid) == copy.known.end())
			copy.known.push_back(faction.templateSid);
	});
	if (copy.known.size() != held->known.size())
		player.set<state::PlayerFaction>(std::move(copy));
}

void Simulation::stepKnownFactions()
{
	if (clockTick() % kFactionRelationsIntervalTicks != 0 || combat_ == nullptr)
		return;
	const FactionHandle handle = firstPlayerFaction(world_.ecs());
	if (handle.id == 0)
		return;
	std::vector<std::pair<double, double>> where;
	for (const CombatArena::Member& m : combat_->members())
	{
		if (m.fighter.playerControlled && !m.fighter.dead)
			where.emplace_back(m.fighter.x, m.fighter.z);
	}
	if (where.empty())
		return;
	const state::FactionState* own = handle.entity.get<state::FactionState>();
	std::vector<std::string> found;
	for (const CombatArena::Member& m : combat_->members())
	{
		if (m.fighter.playerControlled || m.fighter.dead)
			continue;
		const state::Allegiance* allegiance = fighterEntity(m.id).get<state::Allegiance>();
		if (allegiance == nullptr || allegiance->factionSid.empty() || (own != nullptr && allegiance->factionSid == own->templateSid)
			|| std::find(handle.faction.known.begin(), handle.faction.known.end(), allegiance->factionSid) != handle.faction.known.end()
			|| std::find(found.begin(), found.end(), allegiance->factionSid) != found.end())
			continue;
		for (const auto& [x, z] : where)
		{
			if ((m.fighter.x - x) * (m.fighter.x - x) + (m.fighter.z - z) * (m.fighter.z - z) < perception::kBaseRange * perception::kBaseRange)
			{
				found.push_back(allegiance->factionSid);
				break;
			}
		}
	}
	if (found.empty())
		return;
	state::PlayerFaction copy = handle.faction;
	copy.known.insert(copy.known.end(), found.begin(), found.end());
	handle.entity.set<state::PlayerFaction>(std::move(copy));
}

} // namespace ksim
