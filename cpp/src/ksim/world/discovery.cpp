// Map discovery (SIMLINK.md DISCOVERED 37 and event 19; kinspect findings/map24_static.md section 5, exe 14092f6b0).
// Once per sim-second a place within 8500 of a player character or of the camera focus is discovered, within 400 visited. Both
// flags only turn on and are saved on the player faction (state::PlayerFaction::discovered / visited).

#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <unordered_set>

namespace ksim {

namespace {

constexpr double kDiscoverRadius = 8500.0;
constexpr double kTownRadius = 17000.0; // town type 2
constexpr double kSmallRadius = 4250.0; // types 0 (camp, nest) and 4 (ruin)
constexpr double kVisitRadius = 400.0;

} // namespace

// The TOWN records that can be discovered (not types 8 and 10, with a placement), in record order.
struct DiscoveryData
{
	struct Place
	{
		std::string sid;
		std::int32_t type = 0;
		double radius = kDiscoverRadius;
		std::vector<std::pair<double, double>> at; // every placement
	};
	std::vector<Place> places;
};

double Simulation::discoveryRadius(std::int32_t type)
{
	if (type == 8 || type == 10)
		return 0.0;
	return type == 2 ? kTownRadius : (type == 0 || type == 4) ? kSmallRadius : kDiscoverRadius;
}

std::shared_ptr<const DiscoveryData> Simulation::discoveryData() const
{
	if (discoveryData_ != nullptr)
		return discoveryData_;
	if (gameData_ == nullptr)
		return nullptr;
	try
	{
		const kworld::Towns towns = kworld::Towns::join(*gameData_, world_.dataDir());
		auto data = std::make_shared<DiscoveryData>();
		for (const kworld::TownRecord& town : towns.towns())
		{
			const kdata::Record* record = gameData_->find(town.stringID);
			if (record == nullptr || record->boolOr("REMOVED", false) || town.placementIndices.empty())
				continue;
			DiscoveryData::Place place;
			place.sid = town.stringID;
			place.type = record->intOr("type", 0);
			if (discoveryRadius(place.type) == 0.0)
				continue;
			place.radius = discoveryRadius(place.type);
			for (const std::size_t index : town.placementIndices)
				place.at.emplace_back(towns.placements()[index].x, towns.placements()[index].z);
			data->places.push_back(std::move(place));
		}
		discoveryData_ = std::move(data);
	} catch (const std::exception&) {
		return nullptr; // no leveldata: no discovery
	}
	return discoveryData_;
}

void Simulation::stepDiscovery(bool force)
{
	if (!force && clockTick() % kDiscoveryIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::PlayerFaction>() == 0) // ecs.each() below is uncached and walks every table
		return;
	flecs::entity entity{};
	std::uint64_t lowest = 0;
	ecs.each([&](flecs::entity e, const state::Id& id, const state::PlayerFaction&)
	{
		if (lowest == 0 || id.value < lowest)
		{
			lowest = id.value;
			entity = e;
		}
	});
	if (!entity.is_valid())
		return;
	const std::shared_ptr<const DiscoveryData> data = discoveryData();
	if (data == nullptr)
		return;
	std::vector<std::pair<double, double>> where;
	if (combat_ != nullptr)
	{
		for (const CombatArena::Member& m : combat_->members())
		{
			if (m.fighter.playerControlled && !m.fighter.dead)
				where.emplace_back(m.fighter.x, m.fighter.z);
		}
	}
	if (const state::ViewFocus* view = ecs.get<state::ViewFocus>())
		where.emplace_back(view->x, view->z);
	if (where.empty())
		return;
	state::PlayerFaction faction = *entity.get<state::PlayerFaction>();
	const std::unordered_set<std::string> knownDiscovered(faction.discovered.begin(), faction.discovered.end());
	const std::unordered_set<std::string> knownVisited(faction.visited.begin(), faction.visited.end());
	bool changed = false;
	for (const DiscoveryData::Place& place : data->places)
	{
		bool near = false;
		bool close = false;
		for (const auto& [px, pz] : place.at)
		{
			for (const auto& [x, z] : where)
			{
				const double d2 = (px - x) * (px - x) + (pz - z) * (pz - z);
				near = near || d2 < place.radius * place.radius;
				close = close || d2 < kVisitRadius * kVisitRadius;
			}
		}
		if ((near || close) && knownDiscovered.count(place.sid) == 0)
		{
			faction.discovered.push_back(place.sid);
			changed = true;
		}
		if (close && knownVisited.count(place.sid) == 0)
		{
			faction.visited.push_back(place.sid);
			changed = true;
		}
	}
	if (changed)
		entity.set<state::PlayerFaction>(std::move(faction));
}

std::vector<Simulation::DiscoveredPlace> Simulation::discoveredPlaces() const
{
	std::vector<DiscoveredPlace> list;
	const std::shared_ptr<const DiscoveryData> data = discoveryData();
	const state::PlayerFaction* faction = nullptr;
	std::uint64_t lowest = 0;
	world_.each([&](const state::Id& id, const state::PlayerFaction& f)
	{
		if (lowest == 0 || id.value < lowest)
		{
			lowest = id.value;
			faction = &f;
		}
	});
	if (faction == nullptr || data == nullptr)
		return list;
	const std::unordered_set<std::string> discovered(faction->discovered.begin(), faction->discovered.end());
	const std::unordered_set<std::string> visited(faction->visited.begin(), faction->visited.end());
	for (const DiscoveryData::Place& place : data->places)
	{
		if (discovered.count(place.sid) != 0 || visited.count(place.sid) != 0)
			list.push_back({place.sid, place.type, visited.count(place.sid) != 0});
	}
	return list;
}

} // namespace ksim
