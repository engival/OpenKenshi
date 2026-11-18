// Simulation's side of the original-save import (save_import.hpp, kenshi-cym.2): squads and members built from a save's records.
#include "ksim/simulation.hpp"
#include "ksim/clock.hpp"

#include "ksim/factions.hpp"
#include "ksim/inventory_grid.hpp"
#include "ksim/jobs.hpp"
#include "ksim/navigator.hpp"
#include "resident_data.hpp"
#include "ksim/rng.hpp"
#include "ksim/spawn.hpp"
#include "ksim/weather.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/towns.hpp"

namespace ksim {

state::EntityRef Simulation::importFaction(const std::string& factionSid)
{
	flecs::world& ecs = ecsMutable();
	state::EntityRef faction;
	ecs.each([&](flecs::entity entity, const state::FactionState& state)
	{
		if (state.templateSid == factionSid)
			faction.id = entity.id();
	});
	if (faction.id == 0 && !factionSid.empty() && gameData_->find(factionSid) != nullptr)
	{
		const flecs::entity made = state::createInstance(ecs);
		made.set<state::FactionState>(state::FactionState{factionSid});
		faction.id = made.id();
	}
	return faction;
}

state::EntityRef Simulation::importTown(const std::string& townName, std::optional<state::Position> near)
{
	state::EntityRef town;
	double best = 1.0e18;
	ecsMutable().each([&](flecs::entity entity, const state::TownState& state)
	{
		const kdata::Record* record = gameData_->find(state.rootSid);
		if (record == nullptr || record->name != townName)
			return;
		const double d = near ? std::hypot(state.x - near->x, state.z - near->z) : 0.0;
		if (town.id == 0 || d < best)
		{
			best = d;
			town.id = entity.id();
		}
	});
	return town;
}

bool Simulation::scenarioTownToPlayer(const std::string& townName, double x, double z)
{
	const state::EntityRef town = importTown(townName, state::Position{x, z});
	const state::EntityRef player = importFaction(playerFactionSid());
	if (town.id == 0 || player.id == 0)
		return false;
	flecs::entity entity(ecsMutable().c_ptr(), town.id);
	state::TownState state = *entity.get<state::TownState>();
	state.owner = player;
	entity.set<state::TownState>(std::move(state));
	return true;
}

bool Simulation::scenarioRaid(const std::string& factionSid, const std::string& campaignSid, const std::string& townName, double x, double z,
	const std::vector<flecs::entity>& squads, std::uint64_t stayTicks, std::uint64_t retreatTicks)
{
	const state::EntityRef town = importTown(townName, state::Position{x, z});
	const state::EntityRef faction = importFaction(factionSid);
	if (town.id == 0 || faction.id == 0 || squads.empty())
		return false;
	const std::uint64_t now = clockTick();
	RaidOrder order;
	order.campaign = campaignSid;
	order.target = town;
	order.stay = stayTicks;
	order.retreat = retreatTicks;
	order.deadline = now + 24 * kTicksPerGameHour;
	order.now = now;
	flecs::entity owner(ecsMutable().c_ptr(), faction.id);
	state::FactionAI ai = owner.has<state::FactionAI>() ? *owner.get<state::FactionAI>() : state::FactionAI{};
	// Nothing changes unless every squad can take the order.
	if (!issueRaid(ai, order, squads))
		return false;
	owner.set<state::FactionAI>(std::move(ai));
	return true;
}

flecs::entity Simulation::importTemplateSquad(const std::string& squadTemplateSid, state::EntityRef faction, state::EntityRef homeTown, state::SpawnSource source, double x, double z, std::uint64_t seed, std::size_t* members)
{
	const kdata::Record* record = gameData_->find(squadTemplateSid);
	if (record == nullptr)
		return flecs::entity();
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	RngStream rng(streamSeed(seed_, static_cast<std::size_t>(seed)));
	SquadBuilder builder(ecsMutable(), *gameData_, rng);
	const flecs::entity squad = builder.build(*record, faction.id, homeTown.id, source, true, x, z);
	if (members != nullptr)
		*members = static_cast<std::size_t>(squad.world().query_builder<const state::Id>().with<state::MemberOf>(squad).build().count());
	return squad;
}

flecs::entity Simulation::importSquad(const state::SquadInstance& instance, state::Position at)
{
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	const flecs::entity squad = state::createInstance(ecsMutable());
	squad.set<state::SquadInstance>(instance);
	squad.set<state::Position>(at);
	return squad;
}

std::optional<flecs::entity> Simulation::importMember(flecs::entity squad, const std::string& characterSid, bool leader, std::size_t slot,
	const std::function<void(state::Fighter&, state::Inventory&)>& apply, double deadX, double deadZ)
{
	flecs::world& ecs = ecsMutable();
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	flecs::entity member = state::createInstance(ecs);
	member.set<state::CharacterInstance>(state::CharacterInstance{characterSid});
	member.set<state::MemberRole>(state::MemberRole{leader ? state::kRoleLeader : state::kRoleSquad});
	member.add<state::MemberOf>(squad);
	std::uint64_t base = 0;
	const std::optional<state::Fighter> built = templateFighter(member, instance, slot, base);
	if (!built)
	{
		member.destruct();
		return std::nullopt;
	}
	state::Fighter now = *built;
	state::Inventory pack;
	apply(now, pack);
	// What it wears and wields, as adoptFighter puts it; the carried items are already in the pack.
	if (!now.weapon.sid.empty() && now.weapon.weaponType != 5)
		pack.add(now.weapon.sid, 1, state::Inventory::kWeapon, now.weapon.quality);
	for (const state::FighterArmour& piece : now.armour)
	{
		if (!piece.sid.empty())
			pack.add(piece.sid, 1, static_cast<std::uint8_t>(state::Inventory::kArmourBase + piece.slot), piece.quality);
	}
	pack.version = 1;
	grid::settle(pack, grid::Rules(*gameData_), member.get<state::Id>()->value);
	member.set<state::Inventory>(std::move(pack));
	state::MemberDelta delta = state::MemberDelta::between(now, *built).value_or(state::MemberDelta{});
	if (now.dead)
	{
		const state::Position where{deadX, deadZ};
		leaveCorpse(squad, member, slot, std::move(delta), state::CorpseDecay::kCauseOther, &where);
	} else if (delta.mask != 0) {
		member.set<state::MemberDelta>(std::move(delta));
	}
	return member;
}

std::size_t Simulation::importPromote(flecs::entity squad, const std::vector<ImportSpot>& spots, bool snap)
{
	if (combat_ == nullptr)
		return 0;
	promoteWorldSquad(squad);
	std::size_t unplaced = 0;
	for (const ImportSpot& spot : spots)
	{
		state::Fighter* fighter = combat_->find(spot.member.get<state::Id>()->value);
		if (fighter == nullptr || fighter->dead)
			continue;
		fighter->x = spot.x;
		fighter->y = spot.y;
		fighter->z = spot.z;
		if (snap && navigation_ != nullptr)
		{
			if (const std::optional<knav::Position> ground = placeOnGround(spot.x, spot.y, spot.z))
			{
				fighter->x = ground->x;
				fighter->y = ground->y;
				fighter->z = ground->z;
			} else {
				++unplaced;
			}
		}
		fighter->building = 0;
		setLocation(*fighter, buildingAt(fighter->x, fighter->y, fighter->z), fighter->y);
		spot.member.set<state::Fighter>(*fighter);
	}
	return unplaced;
}

void Simulation::importDone()
{
	instancesChanged();
}

std::map<std::string, state::EntityRef> Simulation::importTownIndex()
{
	std::map<std::string, state::EntityRef> byInstance;
	if (gameData_ == nullptr)
		return byInstance;
	std::map<std::string, std::vector<std::pair<state::EntityRef, state::TownState>>> byRoot;
	ecsMutable().each([&](flecs::entity entity, const state::TownState& state)
	{
		byRoot[state.rootSid].push_back({state::EntityRef{entity.id()}, state});
	});
	const kworld::Towns towns = kworld::Towns::join(*gameData_, world_.dataDir());
	for (const kworld::TownRecord& town : towns.towns())
	{
		const auto roots = byRoot.find(town.stringID);
		if (roots == byRoot.end() || town.placementIndices.empty())
			continue;
		// The TownState stands at the primary (first) placement.
		const kworld::TownPlacement& placement = towns.placements()[town.placementIndices.front()];
		for (const auto& [ref, state] : roots->second)
		{
			if (state.x == placement.x && state.z == placement.z)
				byInstance[placement.instanceID] = ref;
		}
	}
	return byInstance;
}

void Simulation::importTownOwner(state::EntityRef town, state::EntityRef owner)
{
	if (state::TownState* state = ecsMutable().entity(town.id).get_mut<state::TownState>())
		state->owner = owner;
}

void Simulation::importProsperity(state::EntityRef faction, double prosperity)
{
	state::FactionState* state = ecsMutable().entity(faction.id).get_mut<state::FactionState>();
	if (state != nullptr)
		state->prosperityLost = std::max(0.0, factionMaxProsperity(*gameData_, state->templateSid) - prosperity);
}

void Simulation::importRelation(state::EntityRef from, state::EntityRef to, double value)
{
	flecs::world& ecs = ecsMutable();
	ecs.entity(from.id).set<state::Relation>(ecs.entity(to.id), state::Relation{value});
}

bool Simulation::importPlayerFaction(std::int64_t money, const std::vector<std::string>& research)
{
	bool done = false;
	ecsMutable().each([&](flecs::entity entity, const state::PlayerFaction& held)
	{
		if (done)
			return;
		state::PlayerFaction faction = held; // copy + set: OnSet fires, so the cached playerSquads view sees the new money
		faction.money = money;
		faction.research = research;
		faction.startoff = state::kImportedStartoff; // a living world around the player: stepPlayerAreas keeps the full-detail areas on the player's characters
		entity.set<state::PlayerFaction>(std::move(faction));
		done = true;
	});
	return done;
}

std::size_t Simulation::importOrders(std::uint64_t id, const std::vector<std::pair<std::uint16_t, std::uint64_t>>& jobs, const std::vector<std::array<double, 3>>& moves)
{
	flecs::entity entity = fighterEntity(id);
	std::size_t added = 0;
	if (!jobs.empty())
	{
		state::Jobs list;
		if (const state::Jobs* held = entity.get<state::Jobs>())
			list = *held;
		for (const auto& [type, target] : jobs)
			added += static_cast<std::size_t>(addJob(list, type, target));
		if (added != 0)
			entity.set<state::Jobs>(std::move(list));
	}
	if (!moves.empty())
	{
		state::OrderQueue queue;
		if (const state::OrderQueue* held = entity.get<state::OrderQueue>())
			queue = *held;
		for (const std::array<double, 3>& at : moves)
			queue.orders.push_back({1, 0, 0, 0, at[0], at[1], at[2]});
		entity.set<state::OrderQueue>(std::move(queue));
	}
	return added;
}

std::size_t Simulation::importCrimes(const std::map<std::uint64_t, state::CrimeRecord>& records)
{
	// Collected first: setting the component moves the entity to another table, which the running each would visit again.
	std::vector<std::pair<flecs::entity, const state::CrimeRecord*>> holders;
	ecsMutable().each([&](flecs::entity entity, const state::Id& id)
	{
		const auto found = records.find(id.value);
		if (found != records.end())
			holders.push_back({entity, &found->second});
	});
	for (const auto& [entity, record] : holders)
		entity.set<state::CrimeRecord>(*record);
	return holders.size();
}

bool Simulation::placeFurnitureCage(const kworld::PlacedInstance& instance)
{
	return gameData_ != nullptr && world_.addFurnitureCage(*gameData_, instance);
}

std::map<std::string, Simulation::ImportPlacement> Simulation::importPlacements()
{
	std::map<std::string, ImportPlacement> byInstance;
	if (gameData_ == nullptr)
		return byInstance;
	const kworld::Towns towns = kworld::Towns::join(*gameData_, world_.dataDir());
	for (const kworld::TownPlacement& placement : towns.placements())
		byInstance[placement.instanceID] = {placement.targetID, placement.x, placement.z};
	return byInstance;
}

bool Simulation::importTownInfo(state::EntityRef town, const state::TownInfo& info, const std::string& replacement)
{
	state::TownState* state = ecsMutable().entity(town.id).get_mut<state::TownState>();
	if (state == nullptr)
		return false;
	state->info = info;
	const kdata::Record* variant = replacement.empty() || gameData_ == nullptr ? nullptr : gameData_->find(replacement);
	if (variant == nullptr || variant->type != kworld::itemTypeByName("TOWN"))
		return false;
	state->activeSid = replacement;
	return true;
}

state::EntityRef Simulation::importExtraTown(const state::ExtraTown& town)
{
	flecs::entity entity = state::createInstance(ecsMutable());
	entity.set<state::ExtraTown>(town);
	return state::EntityRef{entity.id()};
}

void Simulation::importFactionStanding(state::EntityRef faction, std::int32_t rank, double globalTrust, std::vector<state::FactionState::Trust> trust)
{
	state::FactionState* state = ecsMutable().entity(faction.id).get_mut<state::FactionState>();
	if (state == nullptr)
		return;
	std::sort(trust.begin(), trust.end(), [](const state::FactionState::Trust& l, const state::FactionState::Trust& r) { return l.other < r.other; });
	state->rank = rank;
	state->globalTrust = globalTrust;
	state->trust = std::move(trust);
}

void Simulation::importUniques(std::vector<state::WorldEventState::Used> used)
{
	flecs::world& ecs = ecsMutable();
	state::WorldEventState events = ecs.has<state::WorldEventState>() ? *ecs.get<state::WorldEventState>() : state::WorldEventState{};
	events.used = std::move(used);
	ecs.set<state::WorldEventState>(std::move(events));
}

std::vector<Simulation::ImportWeatherResult> Simulation::importWeather(const std::vector<ImportWeather>& slots)
{
	std::vector<ImportWeatherResult> results(slots.size());
	flecs::world& ecs = ecsMutable();
	state::WeatherRegions* regions = ecs.get_mut<state::WeatherRegions>();
	if (weather_ == nullptr || regions == nullptr || regions->regions.size() != weather_->model().groups().size())
	{
		for (ImportWeatherResult& result : results)
			result.why = "weather is off";
		return results;
	}
	const WeatherModel& model = weather_->model();
	// A slot's group is not saved (SAVE_FORMAT 3): what a slot says is the season its group is in. The groups of ours that hold that season and the slots
	// that name it pair off in ascending order when there are as many of the one as of the other (the unclaimed ones: a group has one season at a time, so
	// each pairing frees the others); a lone season pairs outright. The order inside a season is UNSURE (deferred: playtesting): the save does not keep
	// which slot is which group's, and the original's slot order is not known to follow the data's.
	std::vector<std::ptrdiff_t> group(slots.size(), -1);
	std::vector<bool> claimed(model.groups().size(), false);
	std::map<std::string, std::vector<std::size_t>> slotsOf; // season -> slots naming it, ascending
	for (std::size_t i = 0; i < slots.size(); ++i)
	{
		if (slots[i].season.empty())
			results[i].why = "no season";
		else
			slotsOf[slots[i].season].push_back(i);
	}
	const auto holders = [&](const std::string& season)
	{
		std::vector<std::size_t> free;
		for (std::size_t g = 0; g < model.groups().size(); ++g)
			if (!claimed[g] && std::any_of(model.groups()[g].seasons.begin(), model.groups()[g].seasons.end(), [&](const SeasonDef& def) { return def.sid == season; }))
				free.push_back(g);
		return free;
	};
	for (bool progress = true; progress;)
	{
		progress = false;
		for (const auto& [season, named] : slotsOf)
		{
			std::vector<std::size_t> pending;
			for (const std::size_t i : named)
				if (group[i] < 0)
					pending.push_back(i);
			const std::vector<std::size_t> free = holders(season);
			if (pending.empty() || free.size() != pending.size())
				continue;
			for (std::size_t n = 0; n < pending.size(); ++n)
			{
				group[pending[n]] = static_cast<std::ptrdiff_t>(free[n]);
				claimed[free[n]] = true;
			}
			progress = true;
		}
	}
	for (std::size_t i = 0; i < slots.size(); ++i)
	{
		if (group[i] >= 0 || !results[i].why.empty())
			continue;
		const std::size_t free = holders(slots[i].season).size();
		results[i].why = free == 0 ? "no group of ours holds the season" : "the season is shared by " + std::to_string(free) + " free groups and named by more or fewer slots";
	}
	for (std::size_t i = 0; i < slots.size(); ++i)
	{
		if (group[i] < 0)
			continue;
		if (model.weather(slots[i].weather) == nullptr)
		{
			results[i].why = "unknown weather " + slots[i].weather;
			continue;
		}
		const ImportWeather& slot = slots[i];
		const WeatherGroupDef& def = model.groups()[static_cast<std::size_t>(group[i])];
		state::WeatherRegion& region = regions->regions[static_cast<std::size_t>(group[i])];
		for (std::size_t s = 0; s < def.seasons.size(); ++s)
			region.season = def.seasons[s].sid == slot.season ? static_cast<std::uint32_t>(s) : region.season;
		region.seasonEndDay = slot.seasonEndDay;
		region.weather = slot.weather;
		region.previous.clear();
		region.startMinute = slot.startMinute;
		region.endMinute = slot.endMinute;
		region.strength = slot.strength;
		region.effectStrength = slot.effectStrength;
		region.started = 1;
		results[i].group = def.sid;
	}
	weather_->attach(ecs, seed_);
	return results;
}

} // namespace ksim
