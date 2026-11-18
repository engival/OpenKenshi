// Farm crops (task ds/arrest-2): the per-plot state the sim lacked (EconomyProducer is a town supply node with none), 232
// FIND_CROPS_TO_EAT's finder and 231 EAT_CROPS's tick, whose bite is crime 14. Decodes: dt24_eat section 2 (1408cb810), item25_static
// (Crop Theft = Task_EatingCrops::vf2 140349290, function id 0xD, victim = the farm's faction), FARM_DATA (fcs_coverage).

#include "ksim/simulation.hpp"

#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/crime.hpp"
#include "ksim/factions.hpp"
#include "ksim/producers.hpp"
#include "ksim/towncircle.hpp"

#include "world_resolvers.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace ksim {

namespace {

// A plot's id (state::Id): above the camps' range (1 << 61), under nothing else's.
constexpr std::uint64_t kCropIdBase = 1ull << 60;
// OURS (UNSURE): the finder's search radius around the eater, how close it stands to eat, how often the plots grow, how often a hungry
// member checks (the order's own steps run every tick), and the crime timer the bite sets (the exe's 140349290 L54 constant is not decoded).
constexpr double kFindRange = 1500.0;
constexpr double kEatReach = 60.0;
constexpr std::uint64_t kGrowIntervalTicks = 200;
constexpr double kCropCrimeTimer = crime::kTimerTheftReport;
// OURS (UNSURE): the unit a bite takes from the plot, and the full level a bitten member stops at (medical.cpp's `fed`).
constexpr double kBiteUnits = 1.0;
constexpr double kFedHunger = 3.0;

struct CropSpec
{
	double growthHours = 0.0; // FARM_DATA `growth time` (UNSURE: game hours)
	double capacity = 0.0; // `amount` plants x `output per plant`
	std::string output; // the crop's ITEM
};

// The FARM_DATA of a farm building, with the output item its functionality produces. False for a building with no `farm data`.
bool cropSpecOf(const kdata::GameData& data, const kdata::Record& building, CropSpec& out)
{
	const kdata::RefCategory* refs = building.findRefs("farm data");
	if (refs == nullptr || refs->entries.empty())
		return false;
	const kdata::Record* farm = data.find(refs->entries.front().targetID);
	if (farm == nullptr)
		return false;
	producers::Spec spec;
	if (!producers::productionSpecOf(data, building, spec))
		return false;
	out.growthHours = std::max(1.0, double(farm->floatOr("growth time", 1.0)));
	out.capacity = std::max(0.0, farm->intOr("amount", 0) * double(farm->floatOr("output per plant", 1.0)));
	out.output = spec.output;
	return true;
}

}

void Simulation::ensureCrops()
{
	if (cropsReady_ || gameData_ == nullptr)
		return;
	cropsReady_ = true;
	flecs::world& ecs = ecsMutable();
	bool any = false;
	ecs.each([&](const state::CropPlot&) { any = true; });
	if (any)
		return; // a save that has them
	std::vector<std::pair<flecs::entity_t, const state::TownState*>> towns;
	ecs.each([&](flecs::entity entity, const state::TownState& town) { towns.emplace_back(entity.id(), &town); });
	for (std::size_t index = 0; index < world_.productionSites().size(); ++index)
	{
		const ProductionSite& site = world_.productionSites()[index];
		const kdata::Record* building = gameData_->find(site.recordSid);
		CropSpec spec;
		if (building == nullptr || !cropSpecOf(*gameData_, *building, spec))
			continue;
		state::CropPlot plot;
		plot.buildingSid = site.recordSid;
		plot.ownerFaction = site.ownerFaction;
		plot.x = site.x;
		plot.y = site.y;
		plot.z = site.z;
		double best = 1e30;
		for (const auto& [key, town] : towns)
		{
			const double distance = (town->x - site.x) * (town->x - site.x) + (town->z - site.z) * (town->z - site.z);
			if (distance < best)
			{
				best = distance;
				plot.town = state::EntityRef{key};
			}
		}
		// A town's farms are mature when the world starts (UNSURE: the original plants and harvests with workers; no workers here).
		plot.growth = spec.growthHours;
		plot.food = spec.capacity;
		const flecs::entity entity = ecs.entity();
		entity.set<state::Id>(state::Id{kCropIdBase + index});
		entity.set<state::CropPlot>(std::move(plot));
	}
}

// The plots are made by the first query (ensureCrops), not by the tick: an untouched plot is mature and does not change, so a world nobody
// asked about carries no plot entities (and the hash of a world with a farm nobody ate from does not depend on when it was looked at).
void Simulation::stepCrops()
{
	if (clockTick() % kGrowIntervalTicks != 0 || gameData_ == nullptr)
		return;
	const double hours = gameHoursIn(kGrowIntervalTicks);
	ecsMutable().each([&](state::CropPlot& plot)
	{
		const kdata::Record* building = gameData_->find(plot.buildingSid);
		CropSpec spec;
		if (building == nullptr || !cropSpecOf(*gameData_, *building, spec))
			return;
		if (plot.growth < spec.growthHours)
		{
			plot.growth += hours;
			if (plot.growth >= spec.growthHours)
			{
				plot.growth = spec.growthHours;
				plot.food = spec.capacity; // ripe: the whole stand
			}
		} else if (plot.food <= 0.0) {
			plot.growth = 0.0; // eaten out: replanted (UNSURE: the original needs a worker to replant)
		}
	});
}

double Simulation::cropFood(std::uint64_t plotId)
{
	ensureCrops();
	double food = -1.0;
	ecsMutable().each([&](const state::Id& id, const state::CropPlot& plot)
	{
		if (id.value == plotId)
			food = plot.food;
	});
	return food;
}

std::vector<std::uint64_t> Simulation::cropPlots()
{
	ensureCrops();
	std::vector<std::uint64_t> ids;
	ecsMutable().each([&](const state::Id& id, const state::CropPlot&) { ids.push_back(id.value); });
	std::sort(ids.begin(), ids.end());
	return ids;
}

bool Simulation::cropEdible(const state::CropPlot& plot) const
{
	// dt24_eat names the building (function 0xD), not the item: every farm's plot can be eaten from, whatever it grows (UNSURE: the exe's
	// hunger effect of a raw crop is not decoded; the member gets the output item's charges as food).
	const kdata::Record* building = gameData_ != nullptr ? gameData_->find(plot.buildingSid) : nullptr;
	CropSpec spec;
	return building != nullptr && cropSpecOf(*gameData_, *building, spec);
}

// 232 FIND_CROPS_TO_EAT (1408cb810). The town is the member's squad's home (the package's home base), else the town it stands in. A town
// whose faction the member's faction is friendly to (or is) gives nothing, except a plot the member already works on (the held target);
// otherwise the nearest plot of that town with food on it. 0 = none.
std::uint64_t Simulation::findCrops(std::uint64_t id)
{
	ensureCrops();
	const state::Fighter* f = combat_ != nullptr ? combat_->find(id) : nullptr;
	if (f == nullptr || gameData_ == nullptr)
		return 0;
	const auto held = cropOrders_.find(id);
	std::vector<towncircle::Circle> circles;
	std::vector<flecs::entity_t> towns;
	world_.ecs().each([&](flecs::entity entity, const state::TownState& town)
	{
		circles.push_back(towncircle::of(*gameData_, town));
		towns.push_back(entity.id());
	});
	flecs::entity_t town = 0;
	const flecs::entity squad = fighterEntity(id).target<state::MemberOf>();
	if (const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr; instance != nullptr)
		town = instance->homeTown.id;
	if (town == 0)
		if (const int at = towncircle::at(circles, f->x, f->z); at >= 0)
			town = towns[static_cast<std::size_t>(at)];
	bool friendly = false; // no eating crops in a town that likes me (232's veto, resolved by worldres::findCrops232)
	if (town != 0)
	{
		const state::TownState* record = flecs::entity(world_.ecs().c_ptr(), town).get<state::TownState>();
		const flecs::entity owner = record != nullptr && record->owner.id != 0 ? flecs::entity(world_.ecs().c_ptr(), record->owner.id) : flecs::entity();
		const flecs::entity mine = f->faction != 0 ? flecs::entity(world_.ecs().c_ptr(), f->faction) : flecs::entity();
		friendly = owner.is_valid() && mine.is_valid()
			&& (owner == mine || factionRelation(*gameData_, mine, owner) >= crime::kFriendlyRelation);
	}
	std::vector<worldres::CropPick> plots;
	world_.ecs().each([&](const state::Id& plotId, const state::CropPlot& plot)
	{
		if (plot.food >= kBiteUnits && cropEdible(plot))
			plots.push_back({plotId.value, plot.x, plot.z, plot.town.id});
	});
	return worldres::findCrops232(plots, f->x, f->z, town, friendly, held != cropOrders_.end() ? held->second.plot : 0, kFindRange);
}

// 231 EAT_CROPS as an order: the finder's plot, a walk to it, then bites from it (the member's food charges, which the auto-eat bites
// down) until it is fed or the plot is empty. The first bite is Crop Theft against the farm's faction. The goal rides a Routine (a code-only
// goal, as animal_goals.cpp's) that stepCropOrders ends with the order.
bool Simulation::orderEatCrops(std::uint64_t id)
{
	const std::uint64_t plot = findCrops(id);
	if (plot == 0)
		return false;
	cropOrders_[id] = CropOrder{plot, clockTick(), false};
	state::Routine routine;
	routine.goal = routines::task::EatCrops;
	routine.since = clockTick();
	routine.chainTarget = plot;
	routine.plan = {routines::task::EatCrops};
	routine.nextScore = clockTick() + 100000; // the scorer leaves the member alone; endGoalRoutine hands it back
	fighterEntity(id).set<state::Routine>(std::move(routine));
	mirrorFighter(id);
	return true;
}

void Simulation::stepCropOrders(const std::function<bool(std::uint64_t, double, double)>& walk)
{
	for (auto it = cropOrders_.begin(); it != cropOrders_.end();)
	{
		const std::uint64_t id = it->first;
		CropOrder& order = it->second;
		state::Fighter* f = combat_->find(id);
		flecs::entity plotEntity;
		world_.ecs().each([&](flecs::entity entity, const state::Id& plotId, const state::CropPlot&)
		{
			if (plotId.value == order.plot)
				plotEntity = entity;
		});
		const state::CropPlot* plot = plotEntity.is_valid() ? plotEntity.get<state::CropPlot>() : nullptr;
		// Done, or not possible any more: the order ends (no layer reaches a terminal state, the goal just is not served).
		if (f == nullptr || f->dead || f->unconscious || f->down || f->carriedBy != 0 || f->cageBuilding != 0 || plot == nullptr
			|| plot->food < kBiteUnits || (order.ate && f->hunger >= kFedHunger && f->foodCharges <= 0.0))
		{
			endGoalRoutine(id, routines::task::EatCrops);
			it = cropOrders_.erase(it);
			continue;
		}
		if (std::hypot(plot->x - f->x, plot->z - f->z) > kEatReach)
		{
			if (clockTick() % kSquadIntervalTicks == 0)
				walk(id, plot->x, plot->z);
			++it;
			continue;
		}
		if (f->foodCharges <= 0.0 && f->hunger < kFedHunger)
		{
			CropSpec spec;
			const kdata::Record* building = gameData_->find(plot->buildingSid);
			if (building != nullptr && cropSpecOf(*gameData_, *building, spec))
			{
				const ItemInfo* item = combat_->data().item(spec.output);
				state::CropPlot eaten = *plot;
				eaten.food = std::max(0.0, eaten.food - kBiteUnits);
				plotEntity.set<state::CropPlot>(eaten);
				f->foodCharges = (item != nullptr ? item->charges : 1.0) * kFoodChargesPerUnit;
				std::string victim = plot->ownerFaction;
				if (victim.empty())
					if (const state::TownState* town = plot->town.id != 0 ? flecs::entity(world_.ecs().c_ptr(), plot->town.id).get<state::TownState>() : nullptr;
						town != nullptr && town->owner.id != 0)
						if (const state::FactionState* faction = flecs::entity(world_.ecs().c_ptr(), town->owner.id).get<state::FactionState>())
							victim = faction->templateSid;
				if (!order.ate)
					reportCrime(id, crime::kCropTheft, victim, 0, kCropCrimeTimer);
				order.ate = true;
				std::ostringstream line;
				line << "eat crops member " << id << " plot " << order.plot << " food " << eaten.food << " (victim " << victim << ")";
				logTask(line.str());
			}
		}
		++it;
	}
}

}
