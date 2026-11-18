// The sim's debug view (docs/formats/SIMLINK.md "DEBUG VIEW"): the AI fields of the SQUADS set (DEBUG_SQUADS), one query's
// state tree (DEBUG_TREE), the spawn order (DEBUG_SPAWN_SQUAD/DEBUG_SPAWNED) and the weather / time commands
// (DEBUG_SET_WEATHER / DEBUG_SET_TIME, answered with DEBUG_ACK). Everything but the commands only reads the simulation as it
// stands; a command changes the world, so a run with one is not identical.

#include "ksim/debugview/view.hpp"
#include "ksim/debugview/decisions.hpp"

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat.hpp"
#include "ksim/decision_trace.hpp"
#include "ksim/detail.hpp"
#include "ksim/player_squads.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"
#include "ksim/squad_alive.hpp"
#include "ksim/state.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

namespace ksim::debugview {
namespace {

// The wire's enums mirror the sim's (enums.hpp is the client's copy; this is where the sim checks the mirror).
static_assert(static_cast<int>(GoalKind::None) == static_cast<int>(state::GoalKind::None)
	&& static_cast<int>(GoalKind::ManGate) == static_cast<int>(state::GoalKind::ManGate), "GoalKind mirror");
static_assert(static_cast<int>(AiReason::None) == static_cast<int>(state::AiReason::None)
	&& static_cast<int>(AiReason::DoorRefused) == static_cast<int>(state::AiReason::DoorRefused), "AiReason mirror");
static_assert(static_cast<int>(OrderStatus::None) == static_cast<int>(state::OrderStatus::None)
	&& static_cast<int>(OrderStatus::Retreating) == static_cast<int>(state::OrderStatus::Retreating), "OrderStatus mirror");
static_assert(static_cast<int>(Detail::Full) == static_cast<int>(DetailLevel::Full)
	&& static_cast<int>(Detail::Strategic) == static_cast<int>(DetailLevel::Strategic), "DetailLevel mirror");

std::string number(double value)
{
	char text[40];
	std::snprintf(text, sizeof(text), "%.2f", value);
	return text;
}

std::string integer(std::uint64_t value) { return std::to_string(value); }

// A wire str (u16 length, then its bytes) at `at`, advancing it. False when it runs past the frame.
bool readString(const std::uint8_t* payload, std::size_t size, std::size_t& at, std::string& out)
{
	if (size - at < 2)
		return false;
	std::uint16_t length = 0;
	std::memcpy(&length, payload + at, sizeof(length));
	at += 2;
	if (size - at < length)
		return false;
	out.assign(reinterpret_cast<const char*>(payload + at), length);
	at += length;
	return true;
}

// ---- the decision sink's store (the last goal pick per squad; not saved, not in the ECS) ----

struct StoredDecision
{
	GoalChoice chosen;
	std::vector<GoalChoice> top;
};

struct DecisionStore
{
	std::mutex mutex;
	std::map<std::uint64_t, StoredDecision> last; // by squad state::Id
	std::map<std::uint64_t, std::deque<trace::Pick>> picks; // by member state::Id: the last kPickRing picks, newest last
};

constexpr std::size_t kPickRing = 8;

DecisionStore& store()
{
	static DecisionStore instance;
	return instance;
}

std::optional<StoredDecision> lastDecision(std::uint64_t squadId)
{
	std::lock_guard<std::mutex> lock(store().mutex);
	const auto found = store().last.find(squadId);
	if (found == store().last.end())
		return std::nullopt;
	return found->second;
}

std::vector<trace::Pick> picksOf(std::uint64_t member)
{
	std::lock_guard<std::mutex> lock(store().mutex);
	const auto found = store().picks.find(member);
	if (found == store().picks.end())
		return {};
	return std::vector<trace::Pick>(found->second.rbegin(), found->second.rend()); // newest first
}

// ---- the SQUADS set (the same rows writeSquads sends) ----

struct SquadRow
{
	std::uint64_t id = 0;
	flecs::entity_t entity = 0; // 0: a player squad whose entity the walk below did not find
	std::vector<std::uint64_t> members; // player squads: PlayerSquadsView's list (leader first)
};

// The member ids memberIds reports, from a squad's grouped MemberOf entities: characters (CharacterInstance or Fighter) with an Id.
std::vector<std::uint64_t> memberIds(const std::vector<flecs::entity>& entities)
{
	std::vector<std::uint64_t> ids;
	for (const flecs::entity member : entities)
	{
		const state::Id* id = member.get<state::Id>();
		if (id != nullptr && (member.has<state::CharacterInstance>() || member.has<state::Fighter>()))
			ids.push_back(id->value);
	}
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

std::vector<std::uint64_t> memberIds(const flecs::world& ecs, flecs::entity_t squad)
{
	std::vector<std::uint64_t> ids;
	const auto collect = [&](flecs::entity character)
	{
		const flecs::entity owner = character.target<state::MemberOf>();
		const state::Id* id = character.get<state::Id>();
		if (!owner.is_valid() || owner.id() != squad || id == nullptr)
			return;
		if (std::find(ids.begin(), ids.end(), id->value) == ids.end())
			ids.push_back(id->value);
	};
	ecs.each([&](flecs::entity character, const state::CharacterInstance&) { collect(character); });
	ecs.each([&](flecs::entity character, const state::Fighter&) { collect(character); });
	std::sort(ids.begin(), ids.end());
	return ids;
}

std::vector<SquadRow> squadRows(const Simulation& sim, std::size_t limit)
{
	const flecs::world& ecs = sim.world().ecs();
	std::vector<SquadRow> rows;
	ecs.each([&](flecs::entity squad, const state::SquadInstance&, const state::Position&)
	{
		if (const state::Id* id = squad.get<state::Id>())
			rows.push_back({id->value, squad.id(), {}});
	});
	std::sort(rows.begin(), rows.end(), [](const SquadRow& a, const SquadRow& b) { return a.id < b.id; });
	// A player squad is an instance entity with state::PlayerSquad, not a SquadInstance (player_squads.hpp): its entity is found
	// by id so that the row's position, detail and tree resolve like any other squad's.
	std::map<std::uint64_t, flecs::entity_t> playerEntities;
	ecs.each([&](flecs::entity squad, const state::PlayerSquad&)
	{
		if (const state::Id* id = squad.get<state::Id>())
			playerEntities[id->value] = squad.id();
	});
	const PlayerSquadsView player = sim.playerSquads();
	if (limit != 0 && rows.size() + player.squads.size() > limit)
		rows.resize(limit > player.squads.size() ? limit - player.squads.size() : 0);
	for (const PlayerSquadsView::Squad& squad : player.squads)
	{
		const auto entity = playerEntities.find(squad.id);
		rows.push_back({squad.id, entity != playerEntities.end() ? entity->second : 0, squad.members});
	}
	return rows;
}

// The DEBUG_SQUADS target of a squad's goal (the same fields the tree shows).
struct Target
{
	std::uint8_t kind = static_cast<std::uint8_t>(TargetKind::None);
	float x = 0.0f;
	float z = 0.0f;
	std::uint64_t id = 0;
	std::string townSid;
};

Target squadTarget(const flecs::world& ecs, const state::GroupAI& ai)
{
	Target target;
	const auto town = [&](state::EntityRef ref)
	{
		const flecs::entity entity(ecs.c_ptr(), ref.id);
		const state::TownState* state = entity.get<state::TownState>();
		if (state == nullptr)
			return;
		target.kind = static_cast<std::uint8_t>(TargetKind::Town);
		target.x = static_cast<float>(state->x);
		target.z = static_cast<float>(state->z);
		const state::Id* id = entity.get<state::Id>();
		target.id = id != nullptr ? id->value : 0;
		target.townSid = state->activeSid.empty() ? state->rootSid : state->activeSid;
	};
	switch (ai.goal)
	{
	case state::GoalKind::Raid:
		if (ai.order.target.id != 0)
			town(ai.order.target);
		break;
	case state::GoalKind::GoHome:
	case state::GoalKind::TravelTo:
		town(ai.target);
		break;
	case state::GoalKind::Patrol:
	case state::GoalKind::Roam:
	case state::GoalKind::ManGate:
		target.kind = static_cast<std::uint8_t>(TargetKind::Point);
		target.x = static_cast<float>(ai.goalX);
		target.z = static_cast<float>(ai.goalZ);
		break;
	default:
		break;
	}
	return target;
}

std::uint8_t detailOf(flecs::entity squad)
{
	const ksim::Detail* detail = squad.get<ksim::Detail>();
	return detail != nullptr ? static_cast<std::uint8_t>(detail->level)
		: static_cast<std::uint8_t>(DetailLevel::Strategic);
}

// ---- the tree: flat rows in pre-order ----

struct Row
{
	std::uint16_t depth = 0;
	std::string key;
	std::string value;
};

struct Tree
{
	std::vector<Row> rows;
	void add(std::uint16_t depth, std::string key, std::string value)
	{
		rows.push_back({depth, std::move(key), std::move(value)});
	}
};

constexpr std::size_t kMaxRows = 4096;

void addChoice(Tree& tree, std::uint16_t depth, std::string prefix, const GoalChoice& choice)
{
	tree.add(depth, prefix, std::string(goalKindName(static_cast<std::uint8_t>(choice.kind))) + " target "
		+ integer(choice.target.id) + " score " + number(choice.score));
}

const flecs::entity* entityOf(const flecs::world& ecs, std::uint64_t id, flecs::entity& storage)
{
	ecs.each([&](flecs::entity entity, const state::Id& value)
	{
		if (value.value == id)
			storage = entity;
	});
	return storage.is_valid() ? &storage : nullptr;
}

// The decisions subtree (SIMLINK.md "DEBUG VIEW"): the member's ring, newest first; a pick row, then one row per candidate.
void addDecisions(std::uint64_t id, Tree& tree)
{
	const std::vector<trace::Pick> picks = picksOf(id);
	if (picks.empty())
		return;
	tree.add(1, "decisions", integer(picks.size()) + " picks");
	for (const trace::Pick& pick : picks)
	{
		tree.add(2, "pick", "tick " + integer(pick.tick) + " " + trace::sourceName(pick.source) + " running " + integer(pick.runningTask)
			+ " hold " + trace::holdName(pick.hold) + " -> " + trace::resultName(pick.result) + " " + integer(pick.task)
			+ " target " + integer(pick.target) + " score " + number(pick.score) + " tier " + integer(static_cast<std::uint64_t>(pick.tier)));
		for (const trace::Candidate& c : pick.candidates)
			tree.add(3, "candidate " + integer(c.task), goalsel::whyName(c.verdict) + std::string(" tier ") + integer(static_cast<std::uint64_t>(c.tier))
				+ " target " + integer(c.target) + " utility " + number(c.utility) + " weight " + number(c.weight)
				+ " crowd " + number(c.crowd) + " score " + number(c.score));
	}
}

void addMemberTree(const Simulation& sim, std::uint64_t id, std::uint16_t depth, Tree& tree)
{
	const flecs::world& ecs = sim.world().ecs();
	tree.add(depth, "member", integer(id));
	const state::Fighter* fighter = sim.combat() != nullptr ? sim.combat()->find(id) : nullptr;
	flecs::entity storage;
	const flecs::entity* character = entityOf(ecs, id, storage);
	if (character != nullptr)
	{
		if (const state::CharacterInstance* instance = character->get<state::CharacterInstance>())
			tree.add(static_cast<std::uint16_t>(depth + 1), "template", instance->templateSid);
		if (const state::Inventory* inventory = character->get<state::Inventory>())
		{
			std::size_t carried = 0;
			for (const state::InventoryEntry& entry : inventory->items)
				carried += entry.slot == state::Inventory::kCarried ? entry.count : 0;
			tree.add(static_cast<std::uint16_t>(depth + 1), "inventory",
				integer(inventory->items.size()) + " entries, " + integer(carried) + " carried");
		}
		if (const state::Routine* routine = character->get<state::Routine>())
			tree.add(static_cast<std::uint16_t>(depth + 1), "routine",
				"goal " + integer(routine->goal) + " since " + integer(routine->since));
	}
	if (fighter != nullptr)
	{
		tree.add(static_cast<std::uint16_t>(depth + 1), "name", fighter->name);
		tree.add(static_cast<std::uint16_t>(depth + 1), "position",
			number(fighter->x) + " " + number(fighter->y) + " " + number(fighter->z));
		tree.add(static_cast<std::uint16_t>(depth + 1), "state",
			(fighter->dead ? "dead" : fighter->unconscious ? "unconscious" : fighter->down ? "down" : "up"));
		tree.add(static_cast<std::uint16_t>(depth + 1), "blood",
			number(fighter->blood) + " of " + number(fighter->maxBloodRace));
		tree.add(static_cast<std::uint16_t>(depth + 1), "target", integer(fighter->target));
		if (fighter->goal.kind != state::IndividualKind::None)
			tree.add(static_cast<std::uint16_t>(depth + 1), "individual goal",
				integer(static_cast<std::uint8_t>(fighter->goal.kind)) + " target " + integer(fighter->goal.target)
				+ " until " + integer(fighter->goal.until));
		if (fighter->carrying != 0)
			tree.add(static_cast<std::uint16_t>(depth + 1), "carrying", integer(fighter->carrying));
		// The combat body (what a frozen fighter is doing): state, the swing, reach, speed, who engages it.
		static constexpr const char* kStates[] = {"attack", "defend", "2", "between", "hub", "circle", "wait", "7", "stumble", "done", "10", "approach"};
		const auto stateIndex = static_cast<std::size_t>(fighter->state);
		tree.add(static_cast<std::uint16_t>(depth + 1), "combat",
			std::string(stateIndex < std::size(kStates) ? kStates[stateIndex] : "?") + " technique " + integer(fighter->technique)
			+ " clip " + number(fighter->clipTime) + " reach " + number(fighter->cachedReach));
		tree.add(static_cast<std::uint16_t>(depth + 1), "velocity",
			number(fighter->velX) + " " + number(fighter->velZ) + " (" + number(std::hypot(fighter->velX, fighter->velZ)) + " u/s)");
		if (!fighter->engaged.empty())
		{
			std::string ids;
			for (const std::uint64_t attacker : fighter->engaged)
				ids += (ids.empty() ? "" : " ") + integer(attacker);
			tree.add(static_cast<std::uint16_t>(depth + 1), "engaged", ids);
		}
		const Simulation::CharStats stats = sim.charStats(id);
		tree.add(static_cast<std::uint16_t>(depth + 1), "encumbrance", number(stats.encumbrance) + "%");
		const state::FighterSkills& skills = fighter->skills;
		tree.add(static_cast<std::uint16_t>(depth + 1), "skills",
			"str " + number(skills.strength) + " dex " + number(skills.dexterity) + " tou " + number(skills.toughness)
			+ " ath " + number(skills.athletics) + " atk " + number(skills.attack) + " def " + number(skills.defence));
	}
	if (sim.combat() != nullptr)
	{
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			if (member.id != id)
				continue;
			if (!member.hitBy.empty())
			{
				std::string ids;
				for (const std::uint64_t attacker : member.hitBy)
					ids += (ids.empty() ? "" : " ") + integer(attacker);
				tree.add(static_cast<std::uint16_t>(depth + 1), "hitBy", ids);
			}
			if (!member.fighter.hitList.empty())
			{
				std::string ids;
				for (const std::uint64_t hit : member.fighter.hitList)
					ids += (ids.empty() ? "" : " ") + integer(hit);
				tree.add(static_cast<std::uint16_t>(depth + 1), "hitList", ids);
			}
			break;
		}
	}
	if (depth == 0) // the character query only: a squad tree would carry 8 picks per member
		addDecisions(id, tree);
}

void addSquadTree(const Simulation& sim, const flecs::entity& squad, Tree& tree)
{
	const flecs::world& ecs = sim.world().ecs();
	const state::Id* id = squad.get<state::Id>();
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	tree.add(0, "squad", integer(id != nullptr ? id->value : 0));
	const state::PlayerSquad* player = squad.get<state::PlayerSquad>(); // not a SquadInstance: the squad AI never sees it
	if (player != nullptr)
		tree.add(1, "name", player->name);
	if (instance != nullptr)
	{
		tree.add(1, "template", instance->templateSid);
		if (sim.ai() != nullptr)
		{
			const Mapping& mapping = sim.ai()->squadMapping(ecs, *instance);
			tree.add(1, "package", mapping.package);
			tree.add(1, "packageName", mapping.packageName);
			tree.add(1, "behaviour", std::string(behaviourName(mapping.behaviour)));
			tree.add(1, "rule", mapping.rule);
		}
		if (instance->faction.id != 0)
		{
			const flecs::entity faction(ecs.c_ptr(), instance->faction.id);
			if (const state::FactionState* state = faction.get<state::FactionState>())
				tree.add(1, "faction", state->templateSid);
		}
		if (instance->homeTown.id != 0)
		{
			const flecs::entity home(ecs.c_ptr(), instance->homeTown.id);
			if (const state::TownState* state = home.get<state::TownState>())
			{
				tree.add(1, "home", state->activeSid.empty() ? state->rootSid : state->activeSid);
				tree.add(1, "homePoint", number(state->x) + " " + number(state->z));
			}
		}
	}
	if (const state::Position* position = squad.get<state::Position>())
		tree.add(1, "position", number(position->x) + " " + number(position->z));
	tree.add(1, "detail", detailName(detailOf(squad)));
	const SquadVitals vitals = squadVitals(ecs, squad, sim.gameData().get());
	tree.add(1, "vitals", "members " + integer(vitals.members) + " effective " + integer(vitals.effective)
		+ " dead " + integer(vitals.dead) + (vitals.intact ? " intact" : " not intact"));
	if (const state::GroupAI* ai = squad.get<state::GroupAI>())
	{
		tree.add(1, "goal", goalKindName(static_cast<std::uint8_t>(ai->goal)));
		tree.add(1, "since", integer(ai->since));
		tree.add(1, "until", integer(ai->until));
		tree.add(1, "fallback", aiReasonName(static_cast<std::uint8_t>(ai->fallback)));
		if (ai->goal == state::GoalKind::Raid || ai->order.status != state::OrderStatus::None)
		{
			tree.add(1, "order", orderStatusName(static_cast<std::uint8_t>(ai->order.status)));
			tree.add(1, "orderFailure", aiReasonName(static_cast<std::uint8_t>(ai->order.failure)));
			tree.add(1, "orderOrigin", number(ai->order.originX) + " " + number(ai->order.originZ));
			tree.add(1, "orderDeadline", integer(ai->order.deadline));
		}
		const Target target = squadTarget(ecs, *ai);
		tree.add(1, "target", targetKindName(target.kind));
		if (target.kind != static_cast<std::uint8_t>(TargetKind::None))
		{
			tree.add(2, "point", number(target.x) + " " + number(target.z));
			if (!target.townSid.empty())
				tree.add(2, "town", target.townSid);
			if (target.id != 0)
				tree.add(2, "id", integer(target.id));
		}
		tree.add(1, "route", ai->route.empty() ? std::string("none")
			: number(ai->along) + " of " + number(ai->length) + " (" + integer(ai->route.size()) + " points)");
		if (id != nullptr)
		{
			if (const std::optional<StoredDecision> decision = lastDecision(id->value))
			{
				addChoice(tree, 1, "decision", decision->chosen);
				for (std::size_t i = 0; i < decision->top.size() && tree.rows.size() < kMaxRows; ++i)
					addChoice(tree, 2, "candidate " + integer(i + 1), decision->top[i]);
			}
		}
	}
	const std::vector<std::uint64_t> members = player != nullptr ? player->members : memberIds(ecs, squad.id());
	for (const std::uint64_t member : members)
	{
		if (tree.rows.size() + 24 > kMaxRows)
			break;
		addMemberTree(sim, member, 1, tree);
	}
}

// ---- frame writers ----

void writeDebugInfo(simlink::RecordWriter& out, bool enabled)
{
	out.begin(kDebugInfo);
	out.u8("debugVersion", kDebugVersion);
	out.u8("enabled", enabled ? kEnabled : kDisabled);
	out.end();
}

void writeDebugSquads(simlink::RecordWriter& out, std::uint64_t tick, const Simulation& sim, std::size_t limit,
	const std::function<std::uint32_t(const std::string&)>& strid)
{
	const flecs::world& ecs = sim.world().ecs();
	const std::vector<SquadRow> rows = squadRows(sim, limit);
	// Every world squad's members in one pass (MemberOf is exclusive). Per row, the member ids and the vitals each scanned the
	// world: with ~4,900 squads that stalled the sim to 0-2 ticks a second while a client was subscribed.
	std::unordered_map<flecs::entity_t, std::vector<flecs::entity>> membersOf;
	ecs.query_builder<>().with<state::MemberOf>(flecs::Wildcard).build().each([&](flecs::entity member)
	{
		membersOf[member.target<state::MemberOf>().id()].push_back(member);
	});
	const std::vector<flecs::entity> none;
	out.begin(kDebugSquads);
	out.u64("tick", tick);
	out.list("squads", rows.size(), 32);
	for (const SquadRow& row : rows)
	{
		const flecs::entity squad(ecs.c_ptr(), row.entity);
		const bool world = squad.is_valid() && squad.get<state::PlayerSquad>() == nullptr;
		const auto found = world ? membersOf.find(row.entity) : membersOf.end();
		const std::vector<flecs::entity>& entities = found != membersOf.end() ? found->second : none;
		out.item();
		out.u64("id", row.id);
		const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
		std::uint32_t package = 0;
		if (instance != nullptr && sim.ai() != nullptr)
			package = strid(sim.ai()->squadMapping(sim.world().ecs(), *instance).package);
		out.strid("template", instance != nullptr ? strid(instance->templateSid) : 0);
		out.strid("package", package);
		const state::GroupAI* ai = squad.is_valid() ? squad.get<state::GroupAI>() : nullptr;
		const Target target = ai != nullptr ? squadTarget(ecs, *ai) : Target{};
		out.u8("goalKind", ai != nullptr ? static_cast<std::uint8_t>(ai->goal) : static_cast<std::uint8_t>(GoalKind::None));
		out.u8("fallbackReason", ai != nullptr ? static_cast<std::uint8_t>(ai->fallback) : 0);
		out.u8("orderStatus", ai != nullptr ? static_cast<std::uint8_t>(ai->order.status) : 0);
		out.u8("detail", squad.is_valid() ? detailOf(squad) : static_cast<std::uint8_t>(Detail::Strategic));
		SquadVitals vitals;
		if (world)
			vitals = squadVitals(squad, entities, sim.gameData().get());
		else if (squad.is_valid())
			vitals = squadVitals(ecs, squad, sim.gameData().get()); // a player squad: its member list (a handful a session)
		else
			vitals.members = row.members.size();
		out.u8("members", static_cast<std::uint8_t>(std::min<std::size_t>(vitals.members, 255)));
		out.u8("effective", static_cast<std::uint8_t>(std::min<std::size_t>(vitals.effective, 255)));
		out.u8("dead", static_cast<std::uint8_t>(std::min<std::size_t>(vitals.dead, 255)));
		out.u8("intact", vitals.intact ? 1 : 0);
		out.u8("targetKind", target.kind);
		out.f32("tx", target.x);
		out.f32("tz", target.z);
		out.u64("targetId", target.id);
		out.strid("townSid", target.townSid.empty() ? 0 : strid(target.townSid));
		out.u8("routeValid", ai != nullptr && !ai->route.empty() ? 1 : 0);
		out.f32("along", ai != nullptr ? static_cast<float>(ai->along) : 0.0f);
		out.f32("length", ai != nullptr ? static_cast<float>(ai->length) : 0.0f);
		// A player squad has no MemberOf: its members are the PlayerSquadsView list.
		const std::vector<std::uint64_t> members = world ? memberIds(entities) : row.members;
		out.u8("memberCount", static_cast<std::uint8_t>(std::min<std::size_t>(members.size(), 255)));
		for (std::size_t i = 0; i < members.size() && i < 255; ++i)
			out.u64("member", members[i]);
	}
	out.listEnd();
	out.end();
}

void writeDebugTree(simlink::RecordWriter& out, std::uint64_t tick, const Simulation& sim, std::uint8_t kind, std::uint64_t id)
{
	Tree tree;
	if (kind == static_cast<std::uint8_t>(QueryKind::Squad))
	{
		flecs::entity storage;
		// A world squad is a SquadInstance, a player squad a state::PlayerSquad (player_squads.hpp): both answer.
		const flecs::entity* squad = entityOf(sim.world().ecs(), id, storage);
		if (squad != nullptr && (squad->get<state::SquadInstance>() != nullptr || squad->get<state::PlayerSquad>() != nullptr))
			addSquadTree(sim, *squad, tree);
	} else if (kind == static_cast<std::uint8_t>(QueryKind::Character)) {
		addMemberTree(sim, id, 0, tree);
	} else if (kind == static_cast<std::uint8_t>(QueryKind::World)) {
		// The new-game options (ADVANCED_OPTIONS_STATIC.md "Ours"): one {key, value} row each, flattened.
		for (const auto& [key, value] : sim.gameOptionRows())
			tree.add(0, key, value);
	}
	out.begin(kDebugTree);
	out.u64("tick", tick);
	out.u8("kind", kind);
	out.u64("id", id);
	out.u16("rows", static_cast<std::uint16_t>(tree.rows.size()));
	for (const Row& row : tree.rows)
	{
		out.u16("depth", row.depth);
		out.str("key", row.key);
		out.str("value", row.value);
	}
	out.end();
}

} // namespace

DecisionSink decisionSink()
{
	return [](std::uint64_t squadId, const GoalChoice& chosen, const std::vector<GoalChoice>& top)
	{
		std::lock_guard<std::mutex> lock(store().mutex);
		store().last[squadId] = StoredDecision{chosen, top};
	};
}

trace::Sink traceSink(std::function<bool(std::uint64_t)> wants, std::function<void(const trace::Pick&)> also)
{
	trace::Sink sink;
	sink.wants = std::move(wants);
	sink.record = [also = std::move(also)](const trace::Pick& pick)
	{
		{
			std::lock_guard<std::mutex> lock(store().mutex);
			std::deque<trace::Pick>& ring = store().picks[pick.member];
			ring.push_back(pick);
			if (ring.size() > kPickRing)
				ring.pop_front();
		}
		if (also)
			also(pick);
	};
	return sink;
}

bool View::frame(Simulation& sim, bool enabled, std::uint16_t type, const std::uint8_t* payload, std::size_t size)
{
	if (type == kDebugSubscribe)
	{
		if (size != 1)
			return true; // malformed: dropped like any other debug frame
		subscribed_ = payload[0] != 0;
		enabled_ = enabled;
		pendingInfo_ = true;
		return true;
	}
	if (type == kDebugQuery && size == 9 && enabled)
	{
		Query query;
		query.kind = payload[0];
		std::memcpy(&query.id, payload + 1, sizeof(query.id));
		if (query.kind <= static_cast<std::uint8_t>(QueryKind::World) && pending_.size() < 160) // a screenshot asks for up to 64 characters plus their squads at once
			pending_.push_back(query);
	}
	if (type == kDebugSpawnSquad)
	{
		// u32 clientSeq, str template, f32 sx, sz, tx, tz (SIMLINK.md "DEBUG VIEW"). A malformed frame is dropped: with no
		// clientSeq there is nothing to answer. A well-formed one always answers, gate or not.
		if (size < 4)
			return true;
		SpawnReply reply;
		std::memcpy(&reply.clientSeq, payload, sizeof(reply.clientSeq));
		std::size_t at = 4;
		std::uint16_t length = 0;
		if (size - at < 2)
			return true;
		std::memcpy(&length, payload + at, sizeof(length));
		at += 2;
		if (size - at < static_cast<std::size_t>(length) + 4 * sizeof(float))
			return true;
		const std::string templateSid(reinterpret_cast<const char*>(payload + at), length);
		at += length;
		float spawn[2];
		float goal[2];
		std::memcpy(spawn, payload + at, sizeof(spawn));
		std::memcpy(goal, payload + at + sizeof(spawn), sizeof(goal));
		const bool finite = std::isfinite(spawn[0]) && std::isfinite(spawn[1])
			&& std::isfinite(goal[0]) && std::isfinite(goal[1]);
		if (!finite)
			return true;
		if (!enabled)
			reply.reason = "the debug view is off (ksim_server --debug-view, a loopback peer)";
		else
		{
			const Simulation::DebugSpawn placed = sim.debugSpawnSquad(templateSid, spawn[0], spawn[1], goal[0], goal[1]);
			reply.squadId = placed.squadId;
			reply.reason = placed.reason;
		}
		if (pendingSpawns_.size() < 32)
			pendingSpawns_.push_back(std::move(reply));
	}
	if (type == kDebugCheat)
	{
		// u32 clientSeq, u64 character, u8 op (SIMLINK.md "DEBUG VIEW"). Malformed: dropped, like the other commands. One log line per cheat.
		if (size != 13)
			return true;
		AckReply reply;
		reply.frameType = type;
		std::uint64_t character = 0;
		std::memcpy(&reply.clientSeq, payload, sizeof(reply.clientSeq));
		std::memcpy(&character, payload + 4, sizeof(character));
		const unsigned op = payload[12];
		reply.reason = enabled
			? sim.debugCheat(character, op)
			: std::string("the debug view is off (ksim_server --debug-view, a loopback peer)");
		reply.accepted = reply.reason.empty();
		std::fprintf(stderr, "debug cheat: op %u character %llu: %s\n", op, static_cast<unsigned long long>(character),
			reply.accepted ? "applied" : reply.reason.c_str());
		if (pendingAcks_.size() < 32)
			pendingAcks_.push_back(std::move(reply));
		return true;
	}
	if (type == kDebugSetWeather || type == kDebugSetTime || type == kDebugSetOption)
	{
		// u32 clientSeq, then the command's fields (SIMLINK.md "DEBUG VIEW"). A malformed frame is dropped: with no
		// clientSeq there is nothing to answer. A well-formed one always answers, gate or not.
		if (size < 4)
			return true;
		AckReply reply;
		reply.frameType = type;
		std::memcpy(&reply.clientSeq, payload, sizeof(reply.clientSeq));
		std::size_t at = 4;
		if (type == kDebugSetWeather)
		{
			std::string weatherSid;
			std::string groupSid;
			float strength = 0.0f;
			if (!readString(payload, size, at, weatherSid) || !readString(payload, size, at, groupSid)
				|| size - at < sizeof(strength))
				return true;
			std::memcpy(&strength, payload + at, sizeof(strength));
			reply.reason = enabled
				? sim.debugSetWeather(groupSid, weatherSid, strength)
				: std::string("the debug view is off (ksim_server --debug-view, a loopback peer)");
		} else if (type == kDebugSetOption) {
			// u32 clientSeq, str key, f32 value: a new-game option (cod / gdm / ht, saved) or the medical clock (dev only).
			std::string key;
			float value = 0.0f;
			if (!readString(payload, size, at, key) || size - at != sizeof(value))
				return true;
			std::memcpy(&value, payload + at, sizeof(value));
			reply.reason = enabled
				? sim.setGameOption(key, value)
				: std::string("the debug view is off (ksim_server --debug-view, a loopback peer)");
		} else {
			float hour = 0.0f;
			if (size - at < sizeof(hour) + 1)
				return true;
			std::memcpy(&hour, payload + at, sizeof(hour));
			const std::uint8_t extraDays = payload[at + sizeof(hour)];
			if (!enabled)
			{
				reply.reason = "the debug view is off (ksim_server --debug-view, a loopback peer)";
			} else {
				reply.reason = sim.debugSetTime(hour, extraDays);
				if (reply.reason.empty())
					clockDue_ = true; // the tick jumped: this send carries CLOCK (writeClock's change check would miss it)
			}
		}
		reply.accepted = reply.reason.empty();
		if (pendingAcks_.size() < 32)
			pendingAcks_.push_back(std::move(reply));
	}
	return true; // every frame in 200-219 is consumed; the rest is dropped
}

void View::write(simlink::RecordWriter& out, std::uint64_t tick, const Simulation& sim, std::size_t squadLimit,
	const std::function<std::uint32_t(const std::string&)>& strid, bool squadsDue) const
{
	if (pendingInfo_)
		writeDebugInfo(out, enabled_);
	if (subscribed_ && enabled_ && squadsDue)
		writeDebugSquads(out, tick, sim, squadLimit, strid);
	for (const Query& query : pending_)
		writeDebugTree(out, tick, sim, query.kind, query.id);
	for (const SpawnReply& reply : pendingSpawns_)
	{
		out.begin(kDebugSpawned);
		out.u32("clientSeq", reply.clientSeq);
		out.u64("squad", reply.squadId);
		out.str("reason", reply.reason);
		out.end();
	}
	for (const AckReply& reply : pendingAcks_)
	{
		out.begin(kDebugAck);
		out.u32("clientSeq", reply.clientSeq);
		out.u16("frameType", reply.frameType);
		out.u8("accepted", reply.accepted ? 1 : 0);
		out.str("reason", reply.reason);
		out.end();
	}
}

} // namespace ksim::debugview
