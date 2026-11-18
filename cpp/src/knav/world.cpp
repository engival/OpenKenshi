#include "knav/world.hpp"

#include "kworld/error.hpp"

#include <utility>

namespace knav {

bool affectsNavmesh(const kworld::BuildingNav& nav, const BuildingState& state)
{
	if (nav.pathMode == kworld::PathMode::Ignore)
		return false;
	if (state.destroyed && nav.destroyedNavmesh)
		return false;
	return state.builtFraction * 100.0f >= nav.buildThreshold;
}

void NavWorld::Rebuild::run(int threads)
{
	result_ = bakeTileSet(config_, *terrain_, shapes_, coords_, threads);
	done_ = true;
}

NavWorld::NavWorld(const NavConfig& config, const kworld::Heightmap& terrain, Shapes fixedShapes)
	: config_(config), terrain_(terrain), fixed_(std::move(fixedShapes)),
	mesh_(NavMesh::fromTiles(config, {}))
{
}

void NavWorld::markDirty(const Building& building)
{
	for (const kworld::WorldShape& shape : *building.shapes)
		for (const TileCoord& tile : tilesTouching(config_, shape))
			dirty_.insert(tile);
}

void NavWorld::setBuilding(const std::string& id, Shapes shapes, const kworld::BuildingNav& nav,
	BuildingState state)
{
	Building next{ std::move(shapes), nav, state };
	const auto found = buildings_.find(id);
	if (found != buildings_.end())
	{
		if (affectsNavmesh(found->second.nav, found->second.state))
			markDirty(found->second);
		found->second = std::move(next);
		if (affectsNavmesh(found->second.nav, found->second.state))
			markDirty(found->second);
		return;
	}
	const auto inserted = buildings_.emplace(id, std::move(next)).first;
	if (affectsNavmesh(inserted->second.nav, inserted->second.state))
		markDirty(inserted->second);
}

void NavWorld::setBuildingState(const std::string& id, BuildingState state)
{
	const auto found = buildings_.find(id);
	if (found == buildings_.end())
		throw kworld::Error("unknown building " + id);
	Building& building = found->second;
	const bool was = affectsNavmesh(building.nav, building.state);
	building.state = state;
	if (was != affectsNavmesh(building.nav, building.state))
		markDirty(building);
}

void NavWorld::removeBuilding(const std::string& id)
{
	const auto found = buildings_.find(id);
	if (found == buildings_.end())
		return;
	if (affectsNavmesh(found->second.nav, found->second.state))
		markDirty(found->second);
	buildings_.erase(found);
}

void NavWorld::markAllDirty()
{
	for (int z = config_.window.z0; z <= config_.window.z1; ++z)
		for (int x = config_.window.x0; x <= config_.window.x1; ++x)
			dirty_.insert({ x, z });
}

std::unique_ptr<NavWorld::Rebuild> NavWorld::prepareRebuild()
{
	if (dirty_.empty())
		return nullptr;
	auto job = std::make_unique<Rebuild>();
	job->config_ = config_;
	job->terrain_ = &terrain_;
	for (const auto& [id, building] : buildings_)
		if (affectsNavmesh(building.nav, building.state))
			job->keep_.push_back(building.shapes);
	job->keep_.push_back(fixed_);
	for (const Shapes& group : job->keep_)
		for (const kworld::WorldShape& shape : *group)
			job->shapes_.push_back(&shape);
	job->coords_.assign(dirty_.begin(), dirty_.end()); // ascending (z, x)
	dirty_.clear();
	return job;
}

void NavWorld::apply(Rebuild& job)
{
	if (!job.done_)
		throw kworld::Error("rebuild applied before it ran");
	mesh_.replaceTiles(job.coords_, std::move(job.result_));
	if (seeds_.empty())
		return;
	std::vector<Point> seeds = seeds_;
	const std::vector<Point> doors = doorSeeds(mesh_, doorSeeds_, config_.waterLevel);
	seeds.insert(seeds.end(), doors.begin(), doors.end());
	mesh_.prune(seeds, kSeedReach, keepWindowEdge_);
}

void NavWorld::bakeAll(int threads)
{
	markAllDirty();
	const auto job = prepareRebuild();
	job->run(threads);
	apply(*job);
}

} // namespace knav
