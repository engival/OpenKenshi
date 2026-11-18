#include "ksim/world/spawn.hpp"

#include "ksim/world/buildings.hpp"
#include "ksim/ai/gatebind.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ksim {

namespace {

using state::SpawnSource;

// Lists of a TOWN that hold squads, in world_model.py's order.
constexpr std::array<std::pair<std::string_view, SpawnSource>, 4> kTownLists = {{
	{"residents", SpawnSource::TownResident},
	{"bar squads", SpawnSource::TownBar},
	{"default resident", SpawnSource::TownDefaultResident},
	{"roaming squads", SpawnSource::TownRoaming},
}};

// A nest town's lists, in world_model.py's build_spawn_sources order.
constexpr std::array<std::string_view, 4> kNestLists = {
	"residents", "default resident", "bar squads", "roaming squads"};

std::int32_t typeId(std::string_view name)
{
	return kworld::itemTypeByName(name);
}

// Calls func(entry) for every entry of one reference list.
template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (!entry.opaqueEntry)
			func(entry);
	}
}

std::string firstRef(const kdata::Record& record, std::string_view category)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr || refs->entries.empty())
		return {};
	return refs->entries.front().targetID;
}

} // namespace

SquadBuilder::SquadBuilder(flecs::world& ecs, const kdata::GameData& gameData, RngStream& rng)
	: ecs_(ecs)
	, data_(gameData)
	, rng_(rng)
	, squadType_(typeId("SQUAD_TEMPLATE"))
	, stateType_(typeId("WORLD_EVENT_STATE"))
{
	for (const char* name : {"CHARACTER", "HUMAN_CHARACTER", "ANIMAL_CHARACTER"})
		characterTypes_.insert(typeId(name));
}

// WORLD_MODEL.md a: an AND over every condition, the evaluator in world_state.cpp. The facts are read from the world on first use (a dead
// or caged character, no player relations): UNSURE `player involvement` is ignored (WORLD_MODEL.md "The player-involvement ambiguity"); the
// literal reading would start 9 towns destroyed or taken over.
// nullopt for a missing state, which then never matches a requirement.
std::optional<bool> SquadBuilder::evalState(std::string_view sid) const
{
	const kdata::Record* record = data_.find(sid);
	if (record == nullptr || record->type != stateType_)
		return std::nullopt;
	if (!facts_)
	{
		facts_.emplace();
		worldstate::collectStatuses(ecs_, *facts_);
	}
	return worldstate::stateHolds(data_, *facts_, *record);
}

// A record's own `world state` list: every entry's state must evaluate to
// its required value (v0: 1 true, 0 false). An empty list holds.
bool SquadBuilder::worldStateHolds(const kdata::Record& record) const
{
	bool holds = true;
	forEachRef(record, "world state", [&](const kdata::RefEntry& entry)
	{
		const std::optional<bool> value = evalState(entry.targetID);
		holds = holds && value.has_value() && *value == (entry.v0 != 0);
	});
	return holds;
}

flecs::entity SquadBuilder::build(const kdata::Record& squad, flecs::entity_t faction,
	flecs::entity_t homeTown, SpawnSource source, bool positioned, double x, double z)
{
	flecs::entity entity = state::createInstance(ecs_);
	entity.set<state::SquadInstance>(state::SquadInstance{squad.stringID,
		state::EntityRef{faction}, state::EntityRef{homeTown}, source});
	if (positioned)
		entity.set<state::Position>(state::Position{x, z});
	spawnMembers(squad, entity);
	return entity;
}

// A `unique` character ("will only ever spawn once", 155 in the data) that already exists spawns its `unique replacement spawn` instead,
// else nobody (GAMEDATA_SCHEMA CHARACTER). The original populates a town when it is first visited, so a unique several towns list lands in
// the first of them the player sees; ours populates every town at the new game, so it lands in the first in spawn order (deliberate: no
// visit-order rule, user 2026-11-04). UNSURE: a unique that died and was removed from the world counts as gone and could spawn again.
void SquadBuilder::addMember(const std::string& sid, flecs::entity squad, std::uint8_t list)
{
	std::string spawned = sid;
	const kdata::Record* record = data_.find(spawned);
	std::unordered_set<std::string> tried; // a replacement chain that loops back ends with nobody
	while (record != nullptr && characterTypes_.count(record->type) != 0 && record->boolOr("unique", false))
	{
		if (!uniques_)
		{
			uniques_.emplace();
			ecs_.each([&](const state::CharacterInstance& existing) { uniques_->insert(existing.templateSid); });
		}
		if (uniques_->insert(spawned).second)
			break;
		if (!tried.insert(spawned).second)
			return;
		spawned = firstRef(*record, "unique replacement spawn");
		if (spawned.empty())
			return;
		record = data_.find(spawned);
	}
	if (record == nullptr || characterTypes_.count(record->type) == 0)
		return;
	flecs::entity character = state::createInstance(ecs_);
	character.set<state::CharacterInstance>(state::CharacterInstance{spawned});
	character.set<state::MemberRole>(state::MemberRole{list});
	character.set<state::MemberSlot>(state::MemberSlot{slots_[squad.id()]++});
	character.add<state::MemberOf>(squad);
	++characters_;
}

// SQUAD_TEMPLATE member lists (GAMEDATA_SCHEMA.md "SQUAD_TEMPLATE").
// UNSURE: the exe's spawn arithmetic is untraced (GAMEDATA_SCHEMA.md
// "Gaps" 1); counts follow the fcs.def text, with no squad size
// multiplier, and `housemates`, `slaves` and `prisoners` are not spawned.
void SquadBuilder::spawnMembers(const kdata::Record& squad, flecs::entity entity)
{
	// `leader`: v0 is the count.
	forEachRef(squad, "leader", [&](const kdata::RefEntry& entry)
	{
		for (std::int32_t i = 0; i < entry.v0; ++i)
			addMember(entry.targetID, entity, state::kRoleLeader);
	});
	// `squad`, `squad2`: v0 min, v1 max, v1 = 0 means exactly v0.
	for (const std::string_view list : {"squad", "squad2"})
	{
		const std::uint8_t role = list == "squad" ? state::kRoleSquad : state::kRoleSquad2;
		forEachRef(squad, list, [&](const kdata::RefEntry& entry)
		{
			const std::int64_t count = entry.v1 == 0 ? entry.v0
				: rng_.between(entry.v0, std::max(entry.v0, entry.v1));
			for (std::int64_t i = 0; i < count; ++i)
				addMember(entry.targetID, entity, role);
		});
	}
	spawnChooseFrom(squad, entity);
	// `animals`, `animals2`: v0 min, v1 max (v2, the age, is not modelled).
	for (const std::string_view list : {"animals", "animals2"})
	{
		forEachRef(squad, list, [&](const kdata::RefEntry& entry)
		{
			const std::int64_t count = rng_.between(entry.v0,
				std::max(entry.v0, entry.v1));
			for (std::int64_t i = 0; i < count; ++i)
				addMember(entry.targetID, entity);
		});
	}
}

// `choosefrom list`: `num random chars`..`num random chars max` draws.
// UNSURE: each draw is a weighted pick by v0 ("chance"), with
// replacement; the 3 SQUAD_TEMPLATE targets are skipped.
void SquadBuilder::spawnChooseFrom(const kdata::Record& squad, flecs::entity entity)
{
	std::vector<std::pair<std::string, std::int64_t>> choices;
	std::int64_t total = 0;
	forEachRef(squad, "choosefrom list", [&](const kdata::RefEntry& entry)
	{
		const kdata::Record* record = data_.find(entry.targetID);
		if (record == nullptr || characterTypes_.count(record->type) == 0
			|| entry.v0 <= 0)
			return;
		choices.emplace_back(entry.targetID, entry.v0);
		total += entry.v0;
	});
	if (choices.empty())
		return;
	const std::int32_t low = squad.intOr("num random chars", 0);
	const std::int32_t high = squad.intOr("num random chars max", 0);
	const std::int64_t draws = rng_.between(low, std::max(low, high));
	for (std::int64_t i = 0; i < draws; ++i)
	{
		std::int64_t pick = rng_.between(0, total - 1);
		for (const auto& [sid, weight] : choices)
		{
			if (pick < weight)
			{
				addMember(sid, entity);
				break;
			}
			pick -= weight;
		}
	}
}

namespace {

class Spawner
{
public:
	Spawner(flecs::world& ecs, const kdata::GameData& gameData,
		const kworld::Towns& towns, std::uint64_t seed, const std::vector<GateSite>* gates, const std::vector<BuildingSite>* buildings)
		: ecs_(ecs)
		, data_(gameData)
		, towns_(towns)
		, gates_(gates)
		, buildings_(buildings)
		, rng_(streamSeed(seed, kSpawnStream))
		, builder_(ecs, gameData, rng_)
		, townType_(typeId("TOWN"))
		, squadType_(typeId("SQUAD_TEMPLATE"))
	{
	}

	SpawnReport run()
	{
		spawnFactions();
		spawnTowns();
		spawnWilderness();
		report_.characters = builder_.characters();
		return report_;
	}

private:
	struct PlacedTown
	{
		flecs::entity entity;
		const kdata::Record* active = nullptr;
		double x = 0.0;
		double z = 0.0;
		std::uint64_t hand = 0; // kworld::TownPlacement::hand
	};

	const kdata::Record* recordOf(std::string_view sid, std::int32_t type) const
	{
		const kdata::Record* record = data_.find(sid);
		return record != nullptr && record->type == type ? record : nullptr;
	}

	flecs::entity factionEntity(const std::string& sid) const
	{
		const auto found = factions_.find(sid);
		return found == factions_.end() ? flecs::entity{} : found->second;
	}

	void spawnFactions()
	{
		const std::vector<const kdata::Record*> records = data_.byType(typeId("FACTION"));
		for (const kdata::Record* record : records)
		{
			flecs::entity entity = state::createInstance(ecs_);
			entity.set<state::FactionState>(state::FactionState{record->stringID});
			factions_.emplace(record->stringID, entity);
			++report_.factions;
		}
		for (const kdata::Record* record : records)
		{
			const flecs::entity from = factionEntity(record->stringID);
			forEachRef(*record, "relations", [&](const kdata::RefEntry& entry)
			{
				const flecs::entity to = factionEntity(entry.targetID);
				if (!to.is_valid())
					return;
				from.set<state::Relation>(to, state::Relation{static_cast<double>(entry.v0)});
				++report_.relations;
			});
		}
	}

	// Follows `override town` in stored order; the first variant whose own
	// `world state` holds wins, and its chain is followed in turn.
	const kdata::Record* activeVariant(const kdata::Record& root) const
	{
		const kdata::Record* current = &root;
		std::unordered_set<std::string> seen;
		while (seen.insert(current->stringID).second)
		{
			const kdata::Record* next = nullptr;
			forEachRef(*current, "override town", [&](const kdata::RefEntry& entry)
			{
				if (next != nullptr)
					return;
				// UNSURE: a variant id with no TOWN record is skipped;
				// world_model.py would fail on it (none in the shipped data).
				const kdata::Record* variant = recordOf(entry.targetID, townType_);
				if (variant != nullptr && builder_.worldStateHolds(*variant))
					next = variant;
			});
			if (next == nullptr)
				break;
			current = next;
		}
		return current;
	}

	// The town each gate belongs to: the nearest town centre by xz, no cutoff (gatebind.hpp, shared with DoorSystem::bind). Returns,
	// per town in `centres` order, the gate indices.
	std::vector<std::vector<std::size_t>> bindGates(const std::vector<std::pair<double, double>>& centres) const
	{
		std::vector<std::vector<std::size_t>> bound(centres.size());
		if (gates_ == nullptr)
			return bound;
		for (std::size_t g = 0; g < gates_->size(); ++g)
		{
			const std::size_t best = nearestTownIndex((*gates_)[g].x, (*gates_)[g].z, centres.size(), [&](std::size_t i) { return centres[i]; });
			if (best != centres.size())
				bound[best].push_back(g);
		}
		return bound;
	}

	// The town each building and gate is populated by (townbind_static.md Q1, kinspect 2026-11-04): a new game's zones are all fresh, and in a
	// fresh zone the exe rebinds every non-player building by position, whatever hand its zone state stores: the nearest town centre among the
	// towns whose radius covers it (townRadius: the type's base x `town radius mult`; a marker town, type 8, is skipped), else the default
	// (wilderness) town, 0 here. A building with an `interior` the default town took falls back to the nearest town within 10000 u, ignoring the
	// radius (Q1 step 5). Gates are buildings and bind the same way. UNSURE: the exe scans only the 3x3 zone cells around the point (a radius of
	// 2500 x 5 can reach past them); the `vf 0x270` skip and the parent case (furniture binds through its outer building) are not modelled.
	static constexpr double kInteriorTownReach = 10000.0;

	struct TownCover
	{
		double x = 0.0;
		double z = 0.0;
		double radius = 0.0;
		bool marker = false;
		std::uint64_t hand = 0;
	};

	void bindTownHands(const std::vector<TownCover>& towns)
	{
		const auto bind = [&](double x, double z, bool interior) -> std::uint64_t
		{
			const TownCover* best = nullptr;
			double bestD = 0.0;
			for (const TownCover& town : towns)
			{
				const double d = std::hypot(x - town.x, z - town.z);
				if (!town.marker && d <= town.radius && (best == nullptr || d < bestD))
				{
					best = &town;
					bestD = d;
				}
			}
			if (best == nullptr && interior)
			{
				for (const TownCover& town : towns)
				{
					const double d = std::hypot(x - town.x, z - town.z);
					if (d <= kInteriorTownReach && (best == nullptr || d < bestD))
					{
						best = &town;
						bestD = d;
					}
				}
			}
			return best != nullptr ? best->hand : 0;
		};
		buildingHands_.clear();
		gateHands_.clear();
		for (std::size_t i = 0; buildings_ != nullptr && i < buildings_->size(); ++i)
			buildingHands_.push_back(bind((*buildings_)[i].cx, (*buildings_)[i].cz, (*buildings_)[i].hasInterior));
		for (std::size_t i = 0; gates_ != nullptr && i < gates_->size(); ++i)
			gateHands_.push_back(bind((*gates_)[i].x, (*gates_)[i].z, false));
	}

	void spawnTowns()
	{
		std::unordered_map<std::string, const kworld::TownRecord*> placedRoots;
		for (const kworld::TownRecord& town : towns_.towns())
		{
			if (!town.placementIndices.empty())
				placedRoots.emplace(town.stringID, &town);
		}

		const std::vector<const kdata::Record*> records = data_.byType(townType_);
		std::unordered_set<std::string> overrideTargets;
		for (const kdata::Record* record : records)
		{
			forEachRef(*record, "override town", [&](const kdata::RefEntry& entry)
			{
				overrideTargets.insert(entry.targetID);
			});
		}

		// The placed default roots in spawn order (the loop below), for binding the gates to towns.
		std::vector<std::pair<double, double>> centres;
		std::vector<TownCover> covers;
		for (const kdata::Record* root : records)
		{
			const auto placed = placedRoots.find(root->stringID);
			if (overrideTargets.count(root->stringID) != 0 || placed == placedRoots.end())
				continue;
			const kworld::TownPlacement& placement =
				towns_.placements()[placed->second->placementIndices.front()];
			centres.emplace_back(placement.x, placement.z);
			const kdata::Record& active = *activeVariant(*root);
			const int type = active.intOr("type", 2);
			covers.push_back({placement.x, placement.z, buildings::townRadius(type, active.floatOr("town radius mult", 1.0f)), type == buildings::kTownTypeMarker, placement.hand});
		}
		townGates_ = bindGates(centres);
		bindTownHands(covers);
		townOrdinal_ = 0;

		for (const kdata::Record* root : records)
		{
			if (overrideTargets.count(root->stringID) != 0)
				continue;
			const auto placed = placedRoots.find(root->stringID);
			if (placed == placedRoots.end())
				continue;
			// world_model.py's primary placement: the first in layer order.
			const kworld::TownPlacement& placement =
				towns_.placements()[placed->second->placementIndices.front()];

			PlacedTown town;
			town.active = activeVariant(*root);
			town.x = placement.x;
			town.z = placement.z;
			town.hand = placement.hand;
			std::string owner = firstRef(*town.active, "faction");
			if (owner.empty())
				owner = firstRef(*root, "faction");
			town.entity = state::createInstance(ecs_);
			town.entity.set<state::TownState>(state::TownState{root->stringID,
				town.active->stringID, state::EntityRef{factionEntity(owner).id()},
				town.x, town.z});
			++report_.towns;
			spawnTownSquads(town);
			++townOrdinal_;
		}
	}

	// TOWN populate (GRADES_GATESQUADS_KO_LIVE.md b, Town::populateBuildings): the resident entries go in priority order 5 to 0
	// (`v1`), each takes the best-scoring free building: base 1, x10 if the building type is in the template's `building` list,
	// x0.05 if in `building dislike`; a gate is eligible only if it is in the `building` list. Only the gates are modelled (the
	// sim has no other town building to home a resident in), so an entry whose template lists a gate type of this town's takes
	// the free listed gate that comes first in placement order (equal scores: UNSURE how the exe breaks the tie), v0 times.
	// Returns, per `residents` entry in list order, the gate indices it took (0, or up to v0 of them).
	std::vector<std::vector<std::size_t>> assignGates(const std::vector<const kdata::RefEntry*>& entries)
	{
		std::vector<std::vector<std::size_t>> taken(entries.size());
		if (gates_ == nullptr || townOrdinal_ >= townGates_.size() || townGates_[townOrdinal_].empty())
			return taken;
		std::vector<std::size_t> order(entries.size());
		for (std::size_t i = 0; i < order.size(); ++i)
			order[i] = i;
		std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return entries[a]->v1 > entries[b]->v1; });
		std::unordered_set<std::size_t> used;
		for (const std::size_t index : order)
		{
			const kdata::Record* squad = recordOf(entries[index]->targetID, squadType_);
			const kdata::RefCategory* listed = squad != nullptr ? squad->findRefs("building") : nullptr;
			if (listed == nullptr)
				continue;
			const kdata::RefCategory* disliked = squad->findRefs("building dislike");
			const auto names = [](const kdata::RefCategory* refs, const std::string& type)
			{
				return refs != nullptr && std::any_of(refs->entries.begin(), refs->entries.end(),
					[&](const kdata::RefEntry& entry) { return !entry.opaqueEntry && entry.targetID == type; });
			};
			const std::int32_t wanted = std::max(entries[index]->v0, 1);
			while (static_cast<std::int32_t>(taken[index].size()) < wanted)
			{
				std::size_t best = SIZE_MAX;
				double bestScore = 0.0;
				for (const std::size_t g : townGates_[townOrdinal_])
				{
					const std::string& type = (*gates_)[g].buildingSid;
					if (used.count(g) != 0 || !names(listed, type))
						continue;
					const double score = 10.0 * (names(disliked, type) ? 0.05 : 1.0);
					if (score > bestScore)
					{
						bestScore = score;
						best = g;
					}
				}
				if (best == SIZE_MAX)
					break;
				used.insert(best);
				taken[index].push_back(best);
			}
		}
		return taken;
	}

	// A town list as the original loads it (140936e60): the owner faction's entries first, then the town's own, unless the town record has
	// `residents override` (then only the town's).
	std::vector<const kdata::RefEntry*> townList(const PlacedTown& town, std::string_view list) const
	{
		std::vector<const kdata::RefEntry*> out;
		if (!town.active->boolOr("residents override", false))
		{
			std::string factionSid = firstRef(*town.active, "faction");
			if (const kdata::Record* faction = factionSid.empty() ? nullptr : data_.find(factionSid))
				forEachRef(*faction, list, [&](const kdata::RefEntry& entry) { out.push_back(&entry); });
		}
		forEachRef(*town.active, list, [&](const kdata::RefEntry& entry) { out.push_back(&entry); });
		return out;
	}

	// One building of a town's populate: a placed building (BuildingSite) or a gate (GateSite) whose zone state names the town.
	struct TownSite
	{
		bool gate = false;
		std::size_t index = 0; // into buildings_ or gates_
		std::uint64_t home = 0; // SquadInstance::homeBuilding
		const std::string* buildingSid = nullptr;
		const std::string* layout = nullptr; // the placement's `interior layout name` ("" = none; a gate has none)
		bool eligible = false; // the original's assign test: an interior, or a gate
		double x = 0.0;
		double z = 0.0;
		const kdata::Record* assigned = nullptr; // the SQUAD_TEMPLATE at the original's b+0xf0
	};

	// The town's buildings, buildings first then gates, each in World order (UNSURE: the order of the original's town building list, which
	// only breaks score ties). A building's preset template is its zone state's `squad template` (the editor's populate, saved in the data).
	std::vector<TownSite> townSites(const PlacedTown& town) const
	{
		static const std::string noLayout;
		std::vector<TownSite> sites;
		for (std::size_t i = 0; buildings_ != nullptr && i < buildings_->size(); ++i)
		{
			const BuildingSite& building = (*buildings_)[i];
			if (buildingHands_[i] != town.hand)
				continue;
			sites.push_back(TownSite{false, i, building.key, &building.buildingSid, &building.interiorLayout, building.hasInterior, building.cx, building.cz,
				recordOf(building.squadTemplate, squadType_)});
		}
		for (std::size_t i = 0; gates_ != nullptr && i < gates_->size(); ++i)
		{
			const GateSite& gate = (*gates_)[i];
			if (gateHands_[i] != town.hand)
				continue;
			sites.push_back(TownSite{true, i, gate.building, &gate.buildingSid, &noLayout, true, gate.outX, gate.outZ, recordOf(gate.squadTemplate, squadType_)});
		}
		return sites;
	}

	// Town::populateBuildings (GRADES_GATESQUADS_KO_STATIC_b "How a town assigns"; kinspect findings/town_populate.md), for any building. The
	// zone data has already given most buildings a template (TownSite::assigned, the GAMESTATE_BUILDING `squad template` the exe loads into
	// b+0xf0). Then the `residents` entries (the owner faction's first unless the town has `residents override`), priority 5 to 0 (`v1`; 100
	// counts as 0), each take up to `v0` buildings by selectBuildingForEntry: a building not taken this pass whose template is none or the
	// `default resident` (a preset building never counts toward an entry), score 1, x10 if its type is in the template's `building` list,
	// x0.05 if in `building dislike`; argmax, ties to the first. A gate is a candidate only for a template with a non-empty `building` list
	// that names it and no `layout interior`. Every eligible building still empty gets the `default resident`. Then every building with a
	// template spawns it (Building::spawnResidents): each `housemates` ref v0 times (rand v0..v1 when v1 > 0), then the template once, all
	// homed there: a gate's crew on the outside point (+ GateCrew), anyone else at the building's centre (the AI's first step snaps to the
	// navmesh). An entry that finds no building is dropped: the original only spawns residents through buildings, with no retry.
	// A template with a `layout interior` does not take a building placed with another interior layout (rwh.35, 14055ad40). UNSURE:
	// a building with no layout name is still taken (a strict test dropped the Guard town squads), the 0.4 base score ("layout exterior" test) and the exe's
	// exact layout comparison are not modelled. Town membership is
	// the zone state's town hand (the exe does not read it; where its town building list comes from is open).
	void populate(const PlacedTown& town, flecs::entity_t owner, std::vector<TownSite> sites)
	{
		const std::vector<const kdata::RefEntry*> residents = townList(town, "residents");
		std::string defaultSid;
		for (const kdata::RefEntry* entry : townList(town, "default resident"))
			defaultSid = entry->targetID; // the town's own wins over its faction's
		const kdata::Record* fallback = recordOf(defaultSid, squadType_);
		std::vector<bool> used(sites.size(), false);
		const auto lists = [](const kdata::RefCategory* refs, const std::string& type)
		{
			return refs != nullptr && std::any_of(refs->entries.begin(), refs->entries.end(),
				[&](const kdata::RefEntry& entry) { return !entry.opaqueEntry && entry.v0 != kdata::kReferenceRemoved && entry.targetID == type; });
		};
		for (std::int32_t priority = 5; priority >= 0; --priority)
		{
			for (const kdata::RefEntry* entry : residents)
			{
				const std::int32_t entryPriority = entry->v1 == 100 ? 0 : entry->v1;
				const kdata::Record* squad = recordOf(entry->targetID, squadType_);
				if (entryPriority != priority || squad == nullptr)
					continue;
				const kdata::RefCategory* listed = squad->findRefs("building");
				const kdata::RefCategory* disliked = squad->findRefs("building dislike");
				const bool listsAny = listed != nullptr && !listed->entries.empty();
				const std::string layoutName = squad->stringOr("layout interior", "");
				const bool hasLayout = !layoutName.empty();
				// A building already preset with this very template counts toward the entry (rwh.35: the Hub's one Bar blackmarket, one
				// Thieves Guild and 7 hub squatters are the zone data's presets; the entries add none).
				std::int32_t left = entry->v0;
				for (const TownSite& site : sites)
					if (site.assigned == squad)
						--left;
				for (; left > 0; --left)
				{
					std::size_t best = sites.size();
					double bestScore = 0.0;
					for (std::size_t i = 0; i < sites.size(); ++i)
					{
						const TownSite& site = sites[i];
						if (used[i] || !site.eligible || (site.assigned != nullptr && site.assigned != fallback))
							continue;
						if (hasLayout && !site.layout->empty() && *site.layout != layoutName)
							continue;
						if (site.gate && (!listsAny || hasLayout || !lists(listed, *site.buildingSid)))
							continue;
						const double score = (lists(listed, *site.buildingSid) ? 10.0 : 1.0) * (lists(disliked, *site.buildingSid) ? 0.05 : 1.0);
						if (score > bestScore)
						{
							bestScore = score;
							best = i;
						}
					}
					if (best == sites.size())
					{
						report_.residentsDropped += static_cast<std::size_t>(left);
						break;
					}
					used[best] = true;
					sites[best].assigned = squad;
				}
			}
		}
		for (TownSite& site : sites)
		{
			if (site.assigned == nullptr && site.eligible)
				site.assigned = fallback;
		}
		for (const TownSite& site : sites)
		{
			if (site.assigned == nullptr)
				continue;
			const SpawnSource source = site.assigned == fallback ? SpawnSource::TownDefaultResident : SpawnSource::TownResident;
			forEachRef(*site.assigned, "housemates", [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* housemate = recordOf(entry.targetID, squadType_);
				const std::int64_t count = entry.v1 > 0 ? rng_.between(entry.v0, std::max(entry.v0, entry.v1)) : entry.v0;
				for (std::int64_t i = 0; housemate != nullptr && i < count; ++i)
					spawnResident(*housemate, site, source, town, owner);
			});
			spawnResident(*site.assigned, site, source, town, owner);
		}
	}

	// One squad of a building's residents (spawnResidents' spawnSquad): faction = the template's, else the town owner's; none for a template with
	// no members (the Hub's "_ruined empty building (DEFAULT)": an empty building).
	void spawnResident(const kdata::Record& squad, const TownSite& site, SpawnSource source, const PlacedTown& town, flecs::entity_t owner)
	{
		const flecs::entity faction = factionEntity(firstRef(squad, "faction"));
		const std::size_t before = builder_.characters();
		flecs::entity built = builder_.build(squad, faction.is_valid() ? faction.id() : owner, town.entity.id(), source, true, site.x, site.z);
		if (builder_.characters() == before)
		{
			built.destruct();
			return;
		}
		state::SquadInstance& instance = *built.get_mut<state::SquadInstance>();
		instance.homeBuilding = site.home;
		instance.homeX = site.x;
		instance.homeZ = site.z;
		if (site.gate)
		{
			const GateSite& gate = (*gates_)[site.index];
			const double length = std::hypot(gate.outX - gate.inX, gate.outZ - gate.inZ);
			built.set<state::GateCrew>(state::GateCrew{gate.key, gate.outX, gate.outZ,
				(gate.outX - gate.inX) / length, (gate.outZ - gate.inZ) / length, gate.key});
			++report_.gateSquads;
		} else {
			++report_.buildingSquads;
		}
		++report_.squads;
		++report_.squadsBySource[static_cast<std::size_t>(source)];
	}

	void spawnTownSquads(const PlacedTown& town)
	{
		const flecs::entity_t owner = town.entity.get<state::TownState>()->owner.id;
		// A town with no loaded building (outside a partial world, e.g. the test samples) keeps the one-squad-per-entry path below, so its
		// residents still exist.
		std::vector<TownSite> sites = buildings_ != nullptr && town.hand != 0 ? townSites(town) : std::vector<TownSite>{};
		if (!sites.empty())
		{
			populate(town, owner, std::move(sites));
			// `bar squads` are not residents (their own spawner, 1409fcd20, kinspect findings/town_populate.md Q1): per entry (n = v0, p = v1)
			// int(p x 0.005 x n) squads when n > 2, then one more per rand(0..100) <= p until a roll fails or n is reached; they stand at a
			// town spawn node, else the town position (we have no town nodes: the town position). UNSURE: the exe repeats it on a timer (a
			// periodic town refresh) and only loads the list when town+0xd8 == 2 or the record is not type 10. `roaming squads`: one squad
			// each at the town point, as before.
			for (const kdata::RefEntry* entry : townList(town, "bar squads"))
			{
				const kdata::Record* squad = recordOf(entry->targetID, squadType_);
				if (squad == nullptr)
					continue;
				std::int32_t count = entry->v0 > 2 ? static_cast<std::int32_t>(entry->v1 * 0.5 * 0.01 * entry->v0) : 0;
				while (count < entry->v0 && rng_.between(0, 100) <= entry->v1)
					++count;
				const flecs::entity faction = factionEntity(firstRef(*squad, "faction"));
				for (std::int32_t i = 0; i < count; ++i)
					spawnSquad(*squad, faction.is_valid() ? faction.id() : owner, town.entity.id(), SpawnSource::TownBar, &town);
			}
			forEachRef(*town.active, "roaming squads", [&](const kdata::RefEntry& entry)
			{
				if (const kdata::Record* squad = recordOf(entry.targetID, squadType_))
				{
					const flecs::entity faction = factionEntity(firstRef(*squad, "faction"));
					spawnSquad(*squad, faction.is_valid() ? faction.id() : owner, town.entity.id(), SpawnSource::TownRoaming, &town);
				}
			});
			return;
		}
		std::vector<const kdata::RefEntry*> residents;
		forEachRef(*town.active, "residents", [&](const kdata::RefEntry& entry) { residents.push_back(&entry); });
		const std::vector<std::vector<std::size_t>> gateOf = assignGates(residents);
		for (const auto& [list, source] : kTownLists)
		{
			std::size_t entryIndex = 0;
			forEachRef(*town.active, list, [&](const kdata::RefEntry& entry)
			{
				const std::size_t index = entryIndex++;
				const kdata::Record* squad = recordOf(entry.targetID, squadType_);
				if (squad == nullptr)
					return;
				// UNSURE: the squad's own `world state` is not applied to town
				// lists (world_model.py DEFAULT_WORLD_SQUADS); fcs.def says it
				// gates spawning. Counted in the report.
				if (!builder_.worldStateHolds(*squad))
					++report_.townSquadsFailingWorldState;
				const flecs::entity faction = factionEntity(firstRef(*squad, "faction"));
				// A factionless resident takes the town's owner (GAMEDATA_SCHEMA.md
				// "Faction of a spawned NPC").
				const flecs::entity_t squadFaction = faction.is_valid() ? faction.id() : owner;
				if (source == SpawnSource::TownResident && !gateOf[index].empty())
				{
					// A gate crew: home = the gate, spawned on the outside point of its door (centre + 15 n; the leader's stand point).
					for (const std::size_t g : gateOf[index])
					{
						const GateSite& gate = (*gates_)[g];
						flecs::entity crew = builder_.build(*squad, squadFaction, town.entity.id(), source, true,
							gate.outX, gate.outZ);
						const double length = std::hypot(gate.outX - gate.inX, gate.outZ - gate.inZ);
						crew.set<state::GateCrew>(state::GateCrew{gate.key, gate.outX, gate.outZ,
							(gate.outX - gate.inX) / length, (gate.outZ - gate.inZ) / length, gate.key});
						++report_.squads;
						++report_.squadsBySource[static_cast<std::size_t>(source)];
						++report_.gateSquads;
					}
					return;
				}
				if (source == SpawnSource::TownResident && gates_ != nullptr && squad->findRefs("building") != nullptr
					&& templateListsGate(*squad))
					++report_.gateSquadsUnhoused;
				spawnSquad(*squad, squadFaction, town.entity.id(), source, &town);
			});
		}
	}

	bool templateListsGate(const kdata::Record& squad) const
	{
		const kdata::RefCategory* listed = squad.findRefs("building");
		return listed != nullptr && std::any_of(listed->entries.begin(), listed->entries.end(), [&](const kdata::RefEntry& entry)
		{
			return !entry.opaqueEntry && std::any_of(gates_->begin(), gates_->end(),
				[&](const GateSite& gate) { return gate.buildingSid == entry.targetID; });
		});
	}

	// world_model.py build_spawn_sources, without the town and campaign
	// sources: biome homeless spawns, biome nests, faction roaming squads.
	// Each default-active template spawns once, tagged with its first source.
	void spawnWilderness()
	{
		std::vector<std::pair<std::string, SpawnSource>> sources;
		const std::int32_t groupType = typeId("BIOME_GROUP");
		const std::int32_t biomeType = typeId("BIOMES");
		for (const std::int32_t type : {groupType, biomeType})
		{
			for (const kdata::Record* biome : data_.byType(type))
			{
				forEachRef(*biome, "homeless spawns", [&](const kdata::RefEntry& entry)
				{
					sources.emplace_back(entry.targetID, SpawnSource::BiomeHomeless);
				});
			}
		}
		for (const std::int32_t type : {groupType, biomeType})
		{
			for (const kdata::Record* biome : data_.byType(type))
			{
				forEachRef(*biome, "nests", [&](const kdata::RefEntry& nestEntry)
				{
					const kdata::Record* nest = recordOf(nestEntry.targetID, townType_);
					if (nest == nullptr)
						return;
					for (const std::string_view list : kNestLists)
					{
						forEachRef(*nest, list, [&](const kdata::RefEntry& entry)
						{
							sources.emplace_back(entry.targetID, SpawnSource::BiomeNest);
						});
					}
				});
			}
		}
		for (const kdata::Record* faction : data_.byType(typeId("FACTION")))
		{
			forEachRef(*faction, "roaming squads", [&](const kdata::RefEntry& entry)
			{
				sources.emplace_back(entry.targetID, SpawnSource::FactionRoaming);
			});
		}

		std::unordered_set<std::string> spawned;
		for (const auto& [sid, source] : sources)
		{
			const kdata::Record* squad = recordOf(sid, squadType_);
			if (squad == nullptr || !spawned.insert(sid).second)
				continue;
			if (!builder_.worldStateHolds(*squad))
				continue;
			spawnSquad(*squad, factionEntity(firstRef(*squad, "faction")).id(), 0,
				source, nullptr);
		}
	}

	void spawnSquad(const kdata::Record& squad, flecs::entity_t faction,
		flecs::entity_t homeTown, SpawnSource source, const PlacedTown* town)
	{
		builder_.build(squad, faction, homeTown, source, town != nullptr,
			town != nullptr ? town->x : 0.0, town != nullptr ? town->z : 0.0);
		++report_.squads;
		++report_.squadsBySource[static_cast<std::size_t>(source)];
	}

	flecs::world& ecs_;
	const kdata::GameData& data_;
	const kworld::Towns& towns_;
	const std::vector<GateSite>* gates_;
	const std::vector<BuildingSite>* buildings_;
	std::vector<std::vector<std::size_t>> townGates_; // per placed default town, in spawn order: the gates bound to it
	std::vector<std::uint64_t> buildingHands_; // per building / gate: the town hand that populates it (bindTownHands)
	std::vector<std::uint64_t> gateHands_;
	std::size_t townOrdinal_ = 0;
	RngStream rng_;
	SquadBuilder builder_;
	std::int32_t townType_;
	std::int32_t squadType_;
	std::unordered_map<std::string, flecs::entity> factions_;
	SpawnReport report_;
};

} // namespace

SpawnReport spawnDefaultWorld(flecs::world& ecs, const kdata::GameData& gameData,
	const kworld::Towns& towns, const SpawnOptions& options, const std::vector<GateSite>* gates, const std::vector<BuildingSite>* buildings)
{
	state::registerComponents(ecs);
	ecs.set<state::Clock>(state::Clock{options.startTick});
	return Spawner(ecs, gameData, towns, options.seed, gates, buildings).run();
}

} // namespace ksim
