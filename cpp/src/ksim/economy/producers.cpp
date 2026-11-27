#include "ksim/economy/producers.hpp"

#include "ksim/world/buildings.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/prospect.hpp"
#include "ksim/save/save.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/format.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/features.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <unordered_set>
#include <cmath>
#include <iostream>
#include <set>
#include <unordered_map>

namespace ksim::producers {

namespace {

bool hasLive(const kdata::Record& record, const char* category)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return false;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (entry.v0 != kdata::kReferenceRemoved)
			return true;
	}
	return false;
}

} // namespace

const kdata::Record* producerFunctionality(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return nullptr;
	for (const kdata::RefEntry& entry : functionality->entries)
	{
		if (entry.v0 == kdata::kReferenceRemoved)
			continue;
		const kdata::Record* function = data.find(entry.targetID);
		// A furnace (function 14) is a machine row of its own (kind 12, machines.hpp), not a producer: no operator, a container's contents.
		if (function != nullptr && function->intOr("function", 0) != 14 && hasLive(*function, "produces") && !hasLive(*function, "consumes"))
			return function;
		return nullptr;
	}
	return nullptr;
}

namespace {

// The Spec of a BUILDING and one of its functionalities that `produces`.
Spec specFrom(const kdata::GameData& data, const kdata::Record& building, const kdata::Record& function)
{
	const kdata::RefCategory* produces = function.findRefs("produces");
	const kdata::RefEntry* made = nullptr;
	for (const kdata::RefEntry& entry : produces->entries)
	{
		if (entry.v0 != kdata::kReferenceRemoved)
		{
			made = &entry;
			break;
		}
	}
	Spec out;
	out.recordSid = building.stringID;
	out.output = made->targetID;
	out.cap = static_cast<std::uint32_t>(std::max(made->v0, 1));
	const std::int32_t own = building.intOr("max operators", -1);
	out.maxOperators = own >= 0 ? own : std::max(function.intOr("max operators", 1), 0);
	out.productionMult = static_cast<double>(function.floatOr("production mult", 1.0f));
	out.outputRate = static_cast<double>(building.floatOr("output rate", 1.0f));
	out.outputPerDay = function.boolOr("output per day", false);
	out.statUsed = function.intOr("stat used", 0);
	out.resource = function.intOr("world resource mining", 0);
	out.useRange = static_cast<double>(function.floatOr("use range", 20.0f));
	out.progressBar = function.boolOr("has progress bar when used", false);
	out.animation = kDefaultClip;
	if (const kdata::RefCategory* clip = function.findRefs("animation"))
	{
		for (const kdata::RefEntry& entry : clip->entries)
		{
			if (entry.v0 == kdata::kReferenceRemoved)
				continue;
			const kdata::Record* animation = data.find(entry.targetID);
			if (animation != nullptr && !animation->name.empty())
				out.animation = animation->name;
			break;
		}
	}
	return out;
}

} // namespace

const kdata::Record* productionFunctionality(const kdata::GameData& data, const kdata::Record& building)
{
	const kdata::RefCategory* functionality = building.findRefs("functionality");
	if (functionality == nullptr)
		return nullptr;
	for (const kdata::RefEntry& entry : functionality->entries)
	{
		if (entry.v0 == kdata::kReferenceRemoved)
			continue;
		const kdata::Record* function = data.find(entry.targetID);
		if (function != nullptr && hasLive(*function, "produces"))
			return function;
	}
	return nullptr;
}

bool specOf(const kdata::GameData& data, const kdata::Record& building, Spec& out)
{
	const kdata::Record* function = producerFunctionality(data, building);
	if (function == nullptr)
		return false;
	out = specFrom(data, building, *function);
	return true;
}

bool productionSpecOf(const kdata::GameData& data, const kdata::Record& building, Spec& out)
{
	const kdata::Record* function = productionFunctionality(data, building);
	if (function == nullptr)
		return false;
	out = specFrom(data, building, *function);
	return true;
}

Book::Book(const kdata::GameData& data)
{
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("BUILDING")))
	{
		Spec spec;
		if (specOf(data, *record, spec))
			specs_.emplace(record->stringID, std::move(spec));
	}
}

const Spec* Book::find(const std::string& recordSid) const
{
	const auto found = specs_.find(recordSid);
	return found == specs_.end() ? nullptr : &found->second;
}

double baseProgressPerTick(const Spec& spec) noexcept
{
	double perDay = spec.productionMult * spec.outputRate;
	if (spec.maxOperators > 1)
		perDay /= spec.maxOperators;
	if (spec.outputPerDay)
		return perDay / kTicksPerGameDay * kProductionSpeedOption;
	return perDay * kProductionSpeedK * kTickSeconds * kProductionSpeedOption;
}

double skillMult(double stat) noexcept
{
	return 0.5 + 0.9 * (std::clamp(stat, 0.0, 100.0) / 100.0);
}

double& statRef(state::FighterSkills& skills, std::int32_t statUsed) noexcept
{
	switch (statUsed)
	{
	case 4: return skills.science;
	default: return skills.labouring; // 3, and every stat the sim does not carry yet
	}
}

double statOf(const state::FighterSkills& skills, std::int32_t statUsed) noexcept
{
	return statRef(const_cast<state::FighterSkills&>(skills), statUsed);
}

double powerFraction(const Spec&) noexcept
{
	// HOOK: no power grid yet (BUILDINGS_SLICE section 6), so every machine counts as fully powered; an automatic one
	// (maxOperators 0) runs on its own at rate x richness x 1 (kenshi-gzto: the original shows it Working, Workers "-").
	return 1.0;
}

double operatorProgress(const Spec& spec, double richness, const state::FighterSkills& skills) noexcept
{
	return powerFraction(spec) * richness * baseProgressPerTick(spec) * skillMult(statOf(skills, spec.statUsed));
}

} // namespace ksim::producers

namespace ksim {

std::shared_ptr<const producers::Book> Simulation::producerBook() const
{
	if (producerBook_ == nullptr && gameData_ != nullptr)
		producerBook_ = std::make_shared<const producers::Book>(*gameData_);
	return producerBook_;
}

void Simulation::spawnProducer(const ProducerSite& site, double useX, double useY, double useZ)
{
	const std::shared_ptr<const producers::Book> book = producerBook();
	const producers::Spec* spec = book != nullptr ? book->find(site.recordSid) : nullptr;
	if (spec == nullptr)
		return;
	state::Producer producer;
	producer.recordSid = site.recordSid;
	producer.ownerSid = site.ownerFaction;
	producer.output = spec->output;
	producer.cap = spec->cap;
	producer.maxOperators = static_cast<std::uint8_t>(std::min(spec->maxOperators, 255));
	producer.progressBar = spec->progressBar;
	producer.useRange = spec->useRange;
	producer.x = site.x;
	producer.y = site.y;
	producer.z = site.z;
	producer.useX = useX;
	producer.useY = useY;
	producer.useZ = useZ;
	producer.boxMinX = site.boxMinX;
	producer.boxMaxX = site.boxMaxX;
	producer.boxMinZ = site.boxMinZ;
	producer.boxMaxZ = site.boxMaxZ;
	// A resource node is as rich as its zone says (prospect.hpp depositRichness); a resource the prospecting does not know, or no
	// zone data, gives 1.0.
	if (site.placement && (spec->resource == 1 || spec->resource == 2 || spec->resource == 3))
		producer.richness = placementRichness(site.x, site.z, spec->resource); // the panel's richness (machines.cpp)
	else if (spec->resource != 0)
	{
		if (const auto zone = prospectZone(site.x, site.z); zone && zone->env)
		{
			// The zone's `iron mult` / `copper mult`: floor 0.1, no cap (kinspect item 59 MQ1). A resource the prospecting does not know
			// (stone: noise; water) reads 0 here and keeps 1.0.
			const double rich = prospect::depositRichness(*zone->env, static_cast<std::uint8_t>(spec->resource));
			if (rich > 0.0)
				producer.richness = std::max(rich, 0.1);
		}
	}
	state::Inventory stock;
	stock.stackLimit = spec->cap;
	stock.version = 1;
	const grid::Size slot = grid::Rules(*gameData_).footprint(spec->output);
	stock.gridW = static_cast<std::uint8_t>(slot.w);
	stock.gridH = static_cast<std::uint8_t>(slot.h);
	flecs::entity entity = ecsMutable().entity();
	entity.set<state::Id>(state::Id{site.id});
	entity.set<state::Producer>(std::move(producer));
	entity.set<state::Inventory>(std::move(stock));
}

namespace {

bool pristine(const state::Producer& producer, const state::Inventory& stock)
{
	return producer.operators.empty() && producer.progress == 0.0 && stock.items.empty();
}

} // namespace

// The inputs of the foliage generator, loaded on first use, and the producer sites of every zone cell asked for.
struct ProducerFoliage
{
	kworld::Heightmap terrain;
	kworld::BiomeMap biomes;
	kworld::Features features;
	kworld::Towns towns;
	std::unordered_map<std::string, std::string> buildingOfMesh; // FOLIAGE_MESH -> producer BUILDING
	std::unordered_map<int, std::vector<ProducerSite>> cells;
	kworld::CollisionLibrary collision; // a deposit's bounds: its rock's collision as the navmesh bake places it

	ProducerFoliage(const kdata::GameData& data, const std::string& dir)
		: terrain(kworld::Heightmap::open(dir + "/newland/land/fullmap.tif"))
		, biomes(kworld::BiomeMap::load(dir + "/newland/land/biomemap.png"))
		, features(kworld::Features::join(data, dir))
		, towns(kworld::Towns::join(data, dir))
		, collision(dir, data)
	{
		for (const kdata::Record* mesh : data.byType(kworld::itemTypeByName("FOLIAGE_MESH")))
		{
			const kdata::RefCategory* refs = mesh->findRefs("building type");
			if (refs == nullptr)
				continue;
			for (const kdata::RefEntry& entry : refs->entries)
			{
				const kdata::Record* building = entry.v0 == kdata::kReferenceRemoved ? nullptr : data.find(entry.targetID);
				if (building != nullptr && producers::producerFunctionality(data, *building) != nullptr)
				{
					buildingOfMesh.emplace(mesh->stringID, building->stringID);
					break;
				}
			}
		}
	}
};

const std::vector<ProducerSite>& Simulation::foliageProducers(int cellX, int cellZ)
{
	static const std::vector<ProducerSite> none;
	if (producerFoliageFailed_ || gameData_ == nullptr || cellX < 0 || cellZ < 0 || cellX >= kworld::kZoneCellsPerAxis || cellZ >= kworld::kZoneCellsPerAxis)
		return none;
	if (producerFoliage_ == nullptr)
	{
		try
		{
			producerFoliage_ = std::make_shared<ProducerFoliage>(*gameData_, world_.dataDir());
		} catch (const std::exception& e) {
			std::cerr << "producers: no foliage deposits (" << e.what() << ")\n";
			producerFoliageFailed_ = true;
			return none;
		}
	}
	ProducerFoliage& foliage = *producerFoliage_;
	const int index = kworld::zoneIndexOf({cellX, cellZ});
	const auto found = foliage.cells.find(index);
	if (found != foliage.cells.end())
		return found->second;
	std::vector<ProducerSite> sites;
	if (!foliage.buildingOfMesh.empty())
	{
		for (const kworld::FoliageInstance& tree : kworld::generateFoliage({cellX, cellZ}, foliage.terrain, foliage.biomes, *gameData_, foliage.features, foliage.towns))
		{
			const auto building = foliage.buildingOfMesh.find(tree.recordID);
			if (building == foliage.buildingOfMesh.end())
				continue;
			ProducerSite site{producers::kIdBase + producers::kFoliageIdBase + (static_cast<std::uint64_t>(index) << 20) + sites.size(),
				building->second, std::string(), tree.x, tree.y, tree.z};
			std::vector<kworld::WorldShape> shapes;
			kworld::CollisionCoverage coverage;
			if (foliage.collision.addFoliage(tree, shapes, coverage) && !shapes.empty())
			{
				site.boxMinX = site.boxMinZ = 1e30;
				site.boxMaxX = site.boxMaxZ = -1e30;
				for (const kworld::WorldShape& shape : shapes)
				{
					site.boxMinX = std::min(site.boxMinX, static_cast<double>(shape.min[0]));
					site.boxMaxX = std::max(site.boxMaxX, static_cast<double>(shape.max[0]));
					site.boxMinZ = std::min(site.boxMinZ, static_cast<double>(shape.min[2]));
					site.boxMaxZ = std::max(site.boxMaxZ, static_cast<double>(shape.max[2]));
				}
			}
			sites.push_back(std::move(site));
		}
	}
	return foliage.cells.emplace(index, std::move(sites)).first->second;
}

bool producerSaved(const flecs::entity& entity)
{
	const state::Producer* producer = entity.get<state::Producer>();
	const state::Inventory* stock = entity.get<state::Inventory>();
	return producer == nullptr || stock == nullptr || !pristine(*producer, *stock);
}

// Progress per sim-second of one producer from the operators it has now (TASK_PROGRESS rate).
double Simulation::producerRate(const state::Producer& producer) const
{
	const std::shared_ptr<const producers::Book> book = producerBook();
	const producers::Spec* spec = book != nullptr ? book->find(producer.recordSid) : nullptr;
	if (spec == nullptr || combat_ == nullptr)
		return 0.0;
	double perTick = 0.0;
	for (const std::uint64_t id : producer.operators)
	{
		if (const state::Fighter* who = combat_->find(id))
			perTick += producers::operatorProgress(*spec, producer.richness, who->skills);
	}
	return perTick / kTickSeconds;
}

std::optional<Simulation::BuildingView> Simulation::producerView(std::uint64_t id) const
{
	const InteractTarget found = findInteractTarget(world_.ecs(), nullptr, id);
	const flecs::entity entity = found.kind == InteractKind::Producer ? flecs::entity(world_.ecs().c_ptr(), found.entity) : flecs::entity();
	const state::Producer* producer = entity.is_valid() ? entity.get<state::Producer>() : nullptr;
	const kdata::Record* record = producer != nullptr && gameData_ != nullptr ? gameData_->find(producer->recordSid) : nullptr;
	const state::Inventory* stock = producer != nullptr ? entity.get<state::Inventory>() : nullptr;
	if (record == nullptr || stock == nullptr)
		return std::nullopt;
	producers::Spec spec;
	if (!producers::specOf(*gameData_, *record, spec))
		return std::nullopt;
	BuildingView v;
	v.id = id;
	v.record = producer->recordSid;
	world_.ecs().each([&](flecs::entity faction, const state::FactionState& state)
	{
		if (!producer->ownerSid.empty() && state.templateSid == producer->ownerSid)
			v.owner = faction.id();
	});
	v.x = producer->x;
	v.y = producer->y;
	v.z = producer->z;
	// A fully automatic producer (no operator slot) that is not full reads Working (the original's Workers "-", kenshi-k16.5.26).
	v.status = producer->prodState == 1 ? buildings::kOutputFull : producer->prodState == 2 || producer->maxOperators == 0 ? buildings::kWorking : buildings::kNoWorkers;
	v.flags = 1; // built
	v.progress = 1.0;
	v.maxHp = buildings::maxHp(*record);
	v.hp = v.maxHp;
	v.workers = static_cast<std::uint8_t>(std::min<std::size_t>(producer->operators.size(), 255));
	v.maxWorkers = producer->maxOperators;
	v.quality = producer->richness;
	// BUILDINGS_SLICE "Stats list": max = richness x the BUILDING's `output rate` (+0x1a8) x research (1.0: no research model, unowned or owned);
	// efficiency = power (1: a node uses none) x HP fraction (1) x worker fraction x max. Fully automatic: no worker term.
	v.maxEfficiency = producer->richness * spec.outputRate;
	v.efficiency = std::min(v.maxEfficiency, v.maxEfficiency * (v.maxWorkers == 0 ? 1.0 : std::min(1.0, static_cast<double>(v.workers) / v.maxWorkers)));
	if (v.maxWorkers == 0)
		v.flags |= 1u << 11;
	v.version = producer->version + stock->version;
	return v;
}

std::optional<Simulation::OperateAction> Simulation::operateAction(std::uint64_t id) const
{
	const flecs::entity actor = fighterEntity(id);
	const state::InteractTask* task = actor.is_valid() ? actor.get<state::InteractTask>() : nullptr;
	if (task == nullptr || task->action != static_cast<std::uint8_t>(InteractAction::Use))
		return std::nullopt;
	const InteractTarget found = findInteractTarget(world_.ecs(), nullptr, task->target);
	const state::Producer* producer = found.kind == InteractKind::Producer ? flecs::entity(world_.ecs().c_ptr(), found.entity).get<state::Producer>() : nullptr;
	if (producer == nullptr || std::find(producer->operators.begin(), producer->operators.end(), id) == producer->operators.end())
		return std::nullopt;
	const std::shared_ptr<const producers::Book> book = producerBook();
	const producers::Spec* spec = book != nullptr ? book->find(producer->recordSid) : nullptr;
	const auto since = producer->since.find(id);
	return OperateAction{spec != nullptr ? spec->animation : producers::kDefaultClip, since != producer->since.end() ? since->second : clockTick(),
		spec != nullptr && spec->statUsed == 4 ? std::uint8_t{17} : std::uint8_t{18}}; // Science / Labouring, as statRef
}

void Simulation::stepProducers()
{
	if (combat_ == nullptr || gameData_ == nullptr)
		return;
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = clockTick();
	const std::shared_ptr<const producers::Book> book = producerBook();
	if (book == nullptr)
		return;

	// ---- once a second: materialise the sites near a player character, drop the unused far ones ----
	if (now % producers::kIntervalTicks == 0)
	{
		std::vector<std::pair<double, double>> players;
		for (const CombatArena::Member& m : combat_->members())
		{
			if (m.fighter.playerControlled && !m.fighter.dead)
				players.emplace_back(m.fighter.x, m.fighter.z);
		}
		std::unordered_map<std::uint64_t, flecs::entity> live;
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::Producer&) { live.emplace(id.value, entity); });
		const auto near = [&](double x, double z, double radius)
		{
			for (const auto& [px, pz] : players)
			{
				if ((px - x) * (px - x) + (pz - z) * (pz - z) <= radius * radius)
					return true;
			}
			return false;
		};
		const auto consider = [&](const ProducerSite& site)
		{
			if (live.count(site.id) != 0 || !near(site.x, site.z, producers::kMaterialiseRadius))
				return;
			if (navigation_ != nullptr)
			{
				// A streaming mesh may not hold the site's zone yet: try again once it does (not off-mesh for good).
				if (!navigation_->covers(site.x, site.z))
					return;
				// Every placement exists (kenshi-k16.5.30, rwh.299: the original has no work-spot drop). The use point (hauling only) is the
				// walkable point nearest the origin when there is one (a foliage rock cuts its footprint out of the mesh: its edge), else the
				// origin; operators walk to the approach point (routeToInteract), and a deposit with no mesh near fails the order there.
				const bool bounded = site.boxMaxX > site.boxMinX;
				const auto spot = navigation_->walker().snap({site.x, site.y, site.z}, bounded
					? std::max(producers::kUseSnap, 0.6 * std::hypot(site.boxMaxX - site.boxMinX, site.boxMaxZ - site.boxMinZ))
					: producers::kUseSnap);
				if (spot)
					spawnProducer(site, spot->x, spot->y, spot->z);
				else
					spawnProducer(site, site.x, site.y, site.z);
			} else {
				spawnProducer(site, site.x, site.y, site.z);
			}
		};
		for (const ProducerSite& site : world_.producerSites())
			consider(site);
		// The furnaces (function 14): a container entity under the machine's wire id (machines.hpp kMachineIdBase), made once near a player;
		// its contents are the INVENTORY of kind 12. Never dropped: what is put in stays (a saved game keeps the entity and its id).
		std::unordered_set<std::uint64_t> furnaces;
		ecs.each([&](const state::Id& id, const state::Container&)
		{
			if (id.value >= machines::kMachineIdBase)
				furnaces.insert(id.value);
		});
		for (const MachineSite& site : world_.machineSites())
		{
			if (world_.machineInfo(site).kind != machines::Kind::Furnace || furnaces.count(site.id) != 0 || !near(site.x, site.z, producers::kMaterialiseRadius))
				continue;
			state::Container box;
			box.key = site.key;
			box.building = site.key;
			box.recordSid = site.recordSid;
			box.ownerSid = site.ownerFaction;
			box.x = box.useX = site.x;
			box.y = box.useY = site.y;
			box.z = box.useZ = site.z;
			if (navigation_ != nullptr)
			{
				if (!navigation_->covers(site.x, site.z))
					continue;
				if (const auto spot = navigation_->walker().snap({site.x, site.y, site.z}, producers::kUseSnap))
				{
					box.useX = spot->x;
					box.useY = spot->y;
					box.useZ = spot->z;
				}
			}
			state::Inventory contents;
			contents.version = 1;
			const grid::Size size = grid::Rules(*gameData_).buildingSize(site.recordSid);
			if (size.w > 0 && size.h > 0)
			{
				contents.gridW = static_cast<std::uint8_t>(std::min(size.w, 255));
				contents.gridH = static_cast<std::uint8_t>(std::min(size.h, 255));
			}
			flecs::entity entity = ecs.entity();
			entity.set<state::Id>(state::Id{site.id});
			entity.set<state::Container>(std::move(box));
			entity.set<state::Inventory>(std::move(contents));
		}
		// The wild deposits: foliage meshes with a `building type`, generated per zone cell around each player character.
		std::set<std::pair<int, int>> cells;
		for (const auto& [px, pz] : players)
		{
			const kworld::ZoneCell low = kworld::zoneCellOf(px - producers::kMaterialiseRadius, pz - producers::kMaterialiseRadius);
			const kworld::ZoneCell high = kworld::zoneCellOf(px + producers::kMaterialiseRadius, pz + producers::kMaterialiseRadius);
			for (int cz = low.z; cz <= high.z; ++cz)
			{
				for (int cx = low.x; cx <= high.x; ++cx)
					cells.insert({cx, cz});
			}
		}
		for (const auto& [cx, cz] : cells)
		{
			for (const ProducerSite& site : foliageProducers(cx, cz))
				consider(site);
		}
		for (auto& [id, entity] : live)
		{
			const state::Producer& p = *entity.get<state::Producer>();
			if (producerSaved(entity) || near(p.x, p.z, producers::kDropRadius))
				continue;
			entity.destruct();
		}
	}

	// ---- every tick: operators, progress, units ----
	if (ecs.count<state::Producer>() == 0)
		return;
	std::vector<std::pair<std::uint64_t, flecs::entity>> all;
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::Producer&) { all.emplace_back(id.value, entity); });
	std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	const grid::Rules rules(*gameData_);
	std::vector<std::string> playerSids;
	ecs.each([&](const state::FactionState& faction, const state::PlayerFaction&) { playerSids.push_back(faction.templateSid); });
	for (const auto& [pid, entity] : all)
	{
		state::Producer producer = *entity.get<state::Producer>();
		const producers::Spec* spec = book->find(producer.recordSid);
		if (spec == nullptr)
			continue;
		state::Inventory stock = *entity.get<state::Inventory>();
		const std::uint8_t beforeState = producer.prodState;
		const std::size_t beforeOps = producer.operators.size();
		// Operators are the characters that still hold their Use order on this producer, standing and up (one walked back to the approach point has left its slot).
		std::vector<std::uint64_t> kept;
		for (const std::uint64_t id : producer.operators)
		{
			const state::Fighter* who = combat_->find(id);
			const flecs::entity actor = fighterEntity(id);
			const state::InteractTask* task = who != nullptr ? actor.get<state::InteractTask>() : nullptr;
			if (who != nullptr && !who->dead && !who->unconscious && !who->walking() && task != nullptr && task->target == pid
				&& task->action == static_cast<std::uint8_t>(InteractAction::Use))
				kept.push_back(id);
		}
		producer.operators = std::move(kept);
		std::erase_if(producer.since, [&](const auto& entry) { return std::find(producer.operators.begin(), producer.operators.end(), entry.first) == producer.operators.end(); });
		for (const std::uint64_t id : producer.operators)
			producer.since.try_emplace(id, now);
		const auto stopOperators = [&]
		{
			for (const std::uint64_t id : producer.operators)
			{
				const flecs::entity actor = fighterEntity(id);
				const std::uint8_t flags = actor.get<state::InteractTask>()->flags;
				actor.remove<state::InteractTask>();
				reportInteract(id, pid, static_cast<std::uint8_t>(InteractAction::Use), InteractEvent::kFull, flags);
			}
			producer.operators.clear();
			producer.since.clear();
		};
		std::uint32_t held = stock.carried(producer.output);
		bool stockChanged = false;
		if (held >= producer.cap)
		{
			producer.prodState = 1;
			producer.progress = 0.0;
			stopOperators();
		} else {
			double dp = 0.0;
			for (const std::uint64_t id : producer.operators)
				dp += producers::operatorProgress(*spec, producer.richness, combat_->find(id)->skills);
			// A fully automatic producer (maxOperators 0: drill, Well II). A player's runs on its own at rate x richness x power
			// (HOOK: powerFraction is 1 until the power grid, BUILDINGS_SLICE section 6); no worker, so no skill term (UNSURE: the
			// original's skill term for an automatic machine is not decoded). An NPC's produces nothing (user, 2026-11-03: the
			// original's NPC drills cheat the power and make no goods).
			const bool automatic = spec->maxOperators == 0;
			const bool npcDrill = automatic && std::find(playerSids.begin(), playerSids.end(), producer.ownerSid) == playerSids.end();
			if (automatic && !npcDrill)
				dp += producers::powerFraction(*spec) * producer.richness * producers::baseProgressPerTick(*spec);
			// XP for the governing skill, per operator, every tick the order runs (a stalled producer trains too).
			for (const std::uint64_t id : producer.operators)
			{
				state::Fighter* who = combat_->find(id);
				combat_->rules().addXp(producers::statRef(who->skills, spec->statUsed), producers::kXpPerSecond * kTickSeconds);
				mirrorFighter(id);
			}
			if (dp > 0.0)
			{
				producer.progress += dp;
				if (producer.progress >= 0.999)
				{
					// One unit; the overshoot is lost (CRAFTING_SLICE 7.3).
					producer.progress = 0.0;
					stock.add(producer.output, 1);
					const std::vector<state::Inventory::Entry> lost = grid::settle(stock, rules, pid);
					for (const state::Inventory::Entry& entry : lost)
						std::cerr << "producer " << producer.recordSid << ": " << entry.count << " x " << entry.itemSid << " do not fit its slot\n";
					stockChanged = true;
					held = stock.carried(producer.output);
				}
			}
			if (held >= producer.cap)
			{
				producer.prodState = 1;
				producer.progress = 0.0;
				stopOperators();
			} else {
				producer.prodState = producer.operators.empty() && dp <= 0.0 && !npcDrill ? 3 : 2;
			}
		}
		if (producer.prodState != beforeState || producer.operators.size() != beforeOps || stockChanged)
			++producer.version;
		entity.set<state::Producer>(std::move(producer));
		if (stockChanged)
			entity.set<state::Inventory>(std::move(stock));
	}
}

} // namespace ksim
