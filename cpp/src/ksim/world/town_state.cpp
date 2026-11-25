// The world-map town tooltip's data (SIMLINK.md TOWN_STATE 66; survey kenshi-k16.6.540, approved on kenshi-k16.6.610).

#include "ksim/core/simulation.hpp"
#include "ksim/world/factions.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <unordered_map>

namespace ksim {

namespace {

constexpr std::uint8_t kFlagKnown = 1;
constexpr std::uint8_t kFlagDead = 2;
constexpr std::uint8_t kFlagTownClass = 4;
constexpr std::int32_t kTownType = 2; // the TOWN `type` of a town (UNSURE: stands for the exe's class 0xd)

// live bits: 0 residentsMax and 2 roamingMax stay clear (no capacity / quota model yet, SIMLINK.md "Sim gaps").
constexpr std::uint16_t kLiveBits = 0x7a;

} // namespace

std::vector<Simulation::TownStateRow> Simulation::townStates() const
{
	std::vector<TownStateRow> rows;
	const std::vector<DiscoveredPlace> places = discoveredPlaces();
	if (std::none_of(places.begin(), places.end(), [](const DiscoveredPlace& place) { return place.visited; }) || gameData_ == nullptr)
		return rows;
	const flecs::world& ecs = world_.ecs();
	std::unordered_map<std::string, std::pair<flecs::entity_t, const state::TownState*>> towns;
	ecs.each([&](flecs::entity town, const state::TownState& state)
	{
		towns.emplace(state.rootSid, std::pair{town.id(), &state});
		towns.emplace(state.activeSid, std::pair{town.id(), &state});
	});
	std::unordered_map<flecs::entity_t, const state::FactionState*> factionOf; // by entity
	std::unordered_map<std::uint64_t, flecs::entity_t> factionById; // state::Id value -> entity
	ecs.each([&](flecs::entity faction, const state::Id& id, const state::FactionState& state)
	{
		factionOf.emplace(faction.id(), &state);
		factionById.emplace(id.value, faction.id());
	});
	std::unordered_map<std::string, std::size_t> rank; // FACTION record order
	for (const kdata::Record* record : gameData_->byType(kworld::itemTypeByName("FACTION")))
		rank.emplace(record->stringID, rank.size());
	const auto sidOf = [&](flecs::entity_t faction) { const auto f = factionOf.find(faction); return f != factionOf.end() ? f->second->templateSid : std::string(); };

	struct Count
	{
		std::int32_t residents = 0;
		std::int32_t roaming = 0;
		std::int32_t roamingSquads = 0;
		std::vector<flecs::entity_t> factions;
	};
	std::unordered_map<flecs::entity_t, Count> counts;
	ecs.each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		const bool resident = instance.source == state::SpawnSource::TownResident || instance.source == state::SpawnSource::TownDefaultResident;
		if (instance.homeTown.id == 0 || (!resident && instance.source != state::SpawnSource::TownRoaming))
			return;
		std::int32_t alive = 0;
		world_.eachMember(squad, [&](flecs::entity member, const state::Id& id)
		{
			// A materialised member lives in the arena; a token member has no Fighter in the ECS, only a MemberDelta when it differs from its template (neither = alive).
			const state::Fighter* fighter = combat_ != nullptr ? combat_->find(id.value) : nullptr;
			if (fighter == nullptr)
				fighter = member.get<state::Fighter>();
			const state::MemberDelta* delta = fighter == nullptr ? member.get<state::MemberDelta>() : nullptr; // a token member that died
			const bool dead = fighter != nullptr ? fighter->dead : delta != nullptr && (delta->mask & state::MemberDelta::kBody) != 0 && delta->dead;
			alive += dead ? 0 : 1;
		});
		Count& count = counts[instance.homeTown.id];
		if (resident)
		{
			count.residents += alive;
			if (alive > 0 && instance.faction.id != 0 && std::find(count.factions.begin(), count.factions.end(), instance.faction.id) == count.factions.end())
				count.factions.push_back(instance.faction.id);
		} else if (alive > 0) {
			count.roaming += alive;
			++count.roamingSquads;
		}
	});

	for (const DiscoveredPlace& place : places)
	{
		const auto found = place.visited ? towns.find(place.sid) : towns.end();
		if (found == towns.end())
			continue;
		const auto& [id, state] = found->second;
		TownStateRow row;
		row.town = place.sid;
		row.owner = sidOf(state->owner.id);
		row.flags = kFlagKnown | (state->info.ded ? kFlagDead : 0) | (place.type == kTownType ? kFlagTownClass : 0);
		row.live = kLiveBits;
		const Count& count = counts[id];
		row.residentsActual = count.residents;
		row.roamingPop = count.roaming;
		row.roamingSquads = count.roamingSquads;
		std::vector<flecs::entity_t> here = count.factions;
		if (state->owner.id != 0 && std::find(here.begin(), here.end(), state->owner.id) == here.end())
			here.push_back(state->owner.id);
		for (const flecs::entity_t faction : here)
			row.residentFactions.push_back(sidOf(faction));
		std::sort(row.residentFactions.begin(), row.residentFactions.end(), [&](const std::string& a, const std::string& b)
		{
			const auto ra = rank.find(a);
			const auto rb = rank.find(b);
			return (ra != rank.end() ? ra->second : rank.size()) < (rb != rank.end() ? rb->second : rank.size());
		});
		// Hostile occupiers: the factions holding the town whose relation toward the owner is below 0.
		const auto held = occupiers_.find(id);
		if (held != occupiers_.end() && state->owner.id != 0)
		{
			const flecs::entity owner(ecs.c_ptr(), state->owner.id);
			for (const std::uint64_t occupier : held->second)
			{
				const auto entity = factionById.find(occupier);
				if (entity == factionById.end() || entity->second == state->owner.id)
					continue;
				const std::string sid = sidOf(entity->second);
				if (factionRelation(*gameData_, flecs::entity(ecs.c_ptr(), entity->second), owner) < 0.0
					&& std::find(row.hostile.begin(), row.hostile.end(), sid) == row.hostile.end())
					row.hostile.push_back(sid);
			}
		}
		rows.push_back(std::move(row));
	}
	return rows;
}

} // namespace ksim
