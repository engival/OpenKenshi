#include "ksim/economy/machines.hpp"

#include "ksim/core/simulation.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/world/buildings.hpp"
#include "ksim/world/world.hpp"

#include <algorithm>

namespace {

using namespace ksim::machines;

const kdata::Record* firstFunction(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return nullptr;
	for (const kdata::RefEntry& entry : functionality->entries)
	{
		if (entry.v0 != kdata::kReferenceRemoved)
			return data.find(entry.targetID);
	}
	return nullptr;
}

bool hasLive(const kdata::Record& record, const char* category)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return false;
	return std::any_of(refs->entries.begin(), refs->entries.end(), [](const kdata::RefEntry& e) { return e.v0 != kdata::kReferenceRemoved; });
}

std::string firstItem(const kdata::Record& function, const char* category)
{
	const kdata::RefCategory* refs = function.findRefs(category);
	if (refs != nullptr)
	{
		for (const kdata::RefEntry& entry : refs->entries)
		{
			if (entry.v0 != kdata::kReferenceRemoved)
				return entry.targetID;
		}
	}
	return "";
}

StatLine statOf(std::int32_t resource) noexcept
{
	switch (resource)
	{
	case 1: return StatLine::IronQuality;
	case 2: return StatLine::StoneRichness;
	case 3: return StatLine::CopperQuality;
	case 5: return StatLine::WaterQuality;
	default: return StatLine::None;
	}
}

} // namespace

namespace ksim::machines {

std::optional<Kind> kindOf(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::Record* function = firstFunction(data, building);
	if (function == nullptr)
		return std::nullopt;
	switch (function->intOr("function", 0))
	{
	case 5: return Kind::Power;
	case 12: return Kind::Turret;
	case 14: return Kind::Furnace;
	default: break;
	}
	if (!hasLive(*function, "produces"))
		return std::nullopt;
	return hasLive(*function, "consumes") ? Kind::Converter : Kind::Producer;
}

std::optional<Static> staticOf(const kdata::GameData& data, const kdata::Record& building)
{
	const std::optional<Kind> kind = kindOf(data, building);
	const kdata::Record* function = kind ? firstFunction(data, building) : nullptr;
	if (function == nullptr)
		return std::nullopt;
	Static out;
	out.kind = *kind;
	out.powerOutput = building.intOr("power output", function->intOr("power output", 0));
	out.input = firstItem(*function, "consumes");
	out.output = firstItem(*function, "produces");
	// Workers: `max operators` of the BUILDING, else the functionality's (producers.hpp Spec); a furnace, a container that melts, has
	// no operator slot.
	const std::int32_t own = building.intOr("max operators", -1);
	out.maxWorkers = *kind == Kind::Furnace ? 0 : own >= 0 ? own : std::max(function->intOr("max operators", 1), 0);
	return out;
}

} // namespace ksim::machines

namespace ksim {

std::vector<machines::Record> Simulation::machineRecords() const
{
	std::vector<machines::Record> out;
	if (gameData_ == nullptr)
		return out;
	for (const MachineSite& site : world_.machineSites())
	{
		const kdata::Record* building = gameData_->find(site.recordSid);
		const std::optional<machines::Kind> kind = building != nullptr ? machines::kindOf(*gameData_, *building) : std::nullopt;
		if (!kind)
			continue;
		const kdata::Record* function = firstFunction(*gameData_, *building);
		machines::Record r;
		r.recordSid = site.recordSid;
		r.kind = *kind;
		r.ownerFaction = site.ownerFaction;
		r.x = site.x;
		r.y = site.y;
		r.z = site.z;
		r.producerId = site.producerId;
		if (const state::TownState* town = townStateAt(site.x, site.z))
		{
			if (const kdata::Record* root = gameData_->find(town->rootSid))
				r.town = root->name;
		}
		r.maxHp = buildings::maxHp(*building);
		r.hp = r.maxHp;
		const machines::Static& info = world_.machineInfo(site);
		r.powerOutput = info.powerOutput;
		r.powerNeeded = r.powerOutput < 0 && *kind != machines::Kind::Power;
		r.input = info.input;
		r.output = info.output;
		r.maxWorkers = info.maxWorkers;
		r.statValue = 1.0;
		double outputRate = building->floatOr("output rate", 1.0f);
		if (const std::optional<BuildingView> view = r.producerId != 0 ? producerView(r.producerId) : std::nullopt)
		{
			r.workers = view->workers; // materialised: the live operators and the zone's richness
			r.statValue = view->quality;
		}
		r.stat = *kind == machines::Kind::Producer ? statOf(function->intOr("world resource mining", 0)) : machines::StatLine::None;
		r.maxEfficiency = r.statValue * outputRate;
		r.efficiency = r.maxEfficiency * (r.maxWorkers == 0 ? 1.0 : std::min(1.0, static_cast<double>(r.workers) / r.maxWorkers));
		// An automatic machine, and a furnace, always work (the original's Item Furnace, Workers "-"); a machine with a slot works when
		// operated; a turret nobody mans, or a source of power with a slot, reads Normal until its model lands (UNSURE).
		const bool automatic = r.maxWorkers == 0 && *kind != machines::Kind::Turret;
		r.status = automatic || r.workers > 0 ? machines::Status::Working
			: *kind == machines::Kind::Turret || *kind == machines::Kind::Power ? machines::Status::Normal : machines::Status::NoWorkers;
		out.push_back(std::move(r));
	}
	return out;
}

} // namespace ksim

namespace ksim {

namespace {

flecs::entity_t factionIdOf(flecs::world& ecs, const std::string& sid)
{
	flecs::entity_t found = 0;
	if (!sid.empty())
	{
		ecs.each([&](flecs::entity faction, const state::FactionState& state)
		{
			if (found == 0 && state.templateSid == sid)
				found = faction.id();
		});
	}
	return found;
}

} // namespace

std::optional<Simulation::BuildingView> Simulation::machineView(const MachineSite& site, bool mounted) const
{
	const kdata::Record* building = gameData_ != nullptr ? gameData_->find(site.recordSid) : nullptr;
	if (building == nullptr)
		return std::nullopt;
	const machines::Static& info = world_.machineInfo(site);
	BuildingView v;
	if (site.producerId != 0)
	{
		std::optional<BuildingView> own = producerView(site.producerId);
		if (!own)
			return std::nullopt;
		v = std::move(*own);
	} else {
		v.id = site.id;
		v.record = site.recordSid;
		v.owner = factionIdOf(const_cast<flecs::world&>(world_.ecs()), site.ownerFaction);
		v.x = site.x;
		v.y = site.y;
		v.z = site.z;
		v.flags = 1; // built
		v.progress = 1.0;
		v.maxHp = buildings::maxHp(*building);
		v.hp = v.maxHp;
		v.maxWorkers = static_cast<std::uint8_t>(std::min(info.maxWorkers, 255));
		v.quality = 1.0;
		v.maxEfficiency = building->floatOr("output rate", 1.0f);
		const bool automatic = info.maxWorkers == 0 && info.kind != machines::Kind::Turret;
		if (automatic)
			v.flags |= 1u << 11;
		v.efficiency = automatic ? v.maxEfficiency : 0.0;
		// A turret nobody mans and a power source read Normal (0) until their models land; an operated machine with nobody at it, No workers.
		v.status = automatic || info.kind == machines::Kind::Turret || info.kind == machines::Kind::Power ? buildings::kWorking : buildings::kNoWorkers;
		if (mounted)
		{
			v.workers = 1;
			v.status = buildings::kWorking;
		}
		v.version = mounted ? 2 : 1;
	}
	v.powerValue = info.powerOutput;
	return v;
}

std::optional<Simulation::BuildingView> Simulation::containerView(std::uint64_t id) const
{
	const InteractTarget found = findInteractTarget(world_.ecs(), nullptr, id);
	const flecs::entity entity = found.kind == InteractKind::Container ? flecs::entity(world_.ecs().c_ptr(), found.entity) : flecs::entity();
	const state::Container* box = entity.is_valid() ? entity.get<state::Container>() : nullptr;
	const state::Inventory* stock = box != nullptr ? entity.get<state::Inventory>() : nullptr;
	const kdata::Record* building = box != nullptr && gameData_ != nullptr ? gameData_->find(box->recordSid) : nullptr;
	if (building == nullptr || stock == nullptr)
		return std::nullopt;
	BuildingView v;
	v.id = id;
	v.record = box->recordSid;
	v.owner = factionIdOf(const_cast<flecs::world&>(world_.ecs()), box->ownerSid);
	v.x = box->x;
	v.y = box->y;
	v.z = box->z;
	v.flags = 1 | (stock->items.empty() ? 0u : 1u << 7); // built, hasItems
	v.progress = 1.0;
	v.maxHp = buildings::maxHp(*building);
	v.hp = v.maxHp;
	v.version = box->version + stock->version;
	return v;
}

} // namespace ksim
