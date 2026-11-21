#include "ksim/world/region.hpp"

#include "ksim/core/rng.hpp"

#include "kworld/coords.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace ksim {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kWorldMax = kworld::kWorldMin + kworld::kWorldExtent;
// Above this many region cells a nearest query scans every entity instead of
// walking the strip (ARCHITECTURE.md "Threading": global lookup fallback).
constexpr long long kMaxStripCells = 64;

// One step of a saved stream state: the new state and an angle from its
// output (rng.hpp's RngStream).
std::pair<std::uint64_t, double> stepAngle(std::uint64_t state)
{
	RngStream stream(state);
	const double angle = stream.uniform01() * 2.0 * kPi;
	return {stream.state(), angle};
}

double clampWorld(double value)
{
	if (value < kworld::kWorldMin)
		return kworld::kWorldMin;
	if (value >= kWorldMax)
		return std::nextafter(kWorldMax, kworld::kWorldMin);
	return value;
}

RegionConfig validate(RegionConfig config)
{
	if (config.workerThreads < 1 || config.workerThreads > 8)
		throw std::invalid_argument("region workerThreads must be 1..8");
	if (config.squadIntervalTicks == 0)
		throw std::invalid_argument("squadIntervalTicks must be positive");
	if (config.perceiveRadius < 0.0 || config.keepRadius < 0.0)
		throw std::invalid_argument("region radii must be non-negative");
	if (config.areaRadiusRegions < 0)
		throw std::invalid_argument("areaRadiusRegions must be non-negative");

	const double zones = config.regionSize / kworld::kZoneSize;
	const double cells = kworld::kWorldExtent / config.regionSize;
	if (config.regionSize < kworld::kZoneSize || std::fabs(zones - std::round(zones)) > 1e-6
		|| std::fabs(cells - std::round(cells)) > 1e-6
		|| std::round(cells) < 1.0)
		throw std::invalid_argument(
			"regionSize must be a multiple of 4608 that divides the world");
	return config;
}

bool intersects(const std::vector<std::int32_t>& left,
	const std::vector<std::int32_t>& right)
{
	std::size_t i = 0;
	std::size_t j = 0;
	while (i < left.size() && j < right.size())
	{
		if (left[i] == right[j])
			return true;
		if (left[i] < right[j])
			++i;
		else
			++j;
	}
	return false;
}

using Clock = std::chrono::steady_clock;

std::uint64_t nsSince(Clock::time_point& mark)
{
	const Clock::time_point now = Clock::now();
	const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark);
	mark = now;
	return static_cast<std::uint64_t>(elapsed.count());
}

} // namespace

// N participants: the calling thread plus N-1 worker threads. Every run()
// releases both barriers; the work itself is distributed dynamically.
class WorkerPool
{
public:
	explicit WorkerPool(int threads)
		: threads_(threads)
	{
		if (threads_ < 2)
			return;
		start_ = std::make_unique<std::barrier<>>(threads_);
		end_ = std::make_unique<std::barrier<>>(threads_);
		workers_.reserve(threads_ - 1);
		try
		{
			for (int index = 1; index < threads_; ++index)
				workers_.emplace_back([this, index] { loop(index); });
		} catch (...) {
			// A thread that failed to start never arrives: drop its share of the start barrier, then stop the ones that did.
			for (int missing = threads_ - 1 - static_cast<int>(workers_.size()); missing > 0; --missing)
				start_->arrive_and_drop();
			shutdown();
			throw;
		}
	}

	~WorkerPool()
	{
		if (threads_ < 2)
			return;
		shutdown();
	}

	void run(const std::function<void(int)>& job)
	{
		if (threads_ < 2)
		{
			job(0);
			return;
		}
		job_ = &job;
		start_->arrive_and_wait();
		job(0);
		end_->arrive_and_wait();
	}

private:
	// Release the workers parked at the start barrier; they observe stop and return. std::thread, not std::jthread (Apple's
	// libc++ has none), so they are joined here, before the barriers are destroyed.
	void shutdown()
	{
		stop_.store(true, std::memory_order_release);
		start_->arrive_and_wait();
		for (std::thread& worker : workers_)
			worker.join();
	}

	void loop(int index)
	{
		for (;;)
		{
			start_->arrive_and_wait();
			if (stop_.load(std::memory_order_acquire))
				return;
			(*job_)(index);
			end_->arrive_and_wait();
		}
	}

	int threads_;
	std::unique_ptr<std::barrier<>> start_;
	std::unique_ptr<std::barrier<>> end_;
	const std::function<void(int)>* job_ = nullptr;
	std::atomic<bool> stop_{false};
	std::vector<std::thread> workers_;
};

RegionEngine::RegionEngine(std::uint64_t seed, RegionConfig config)
	: seed_(seed)
	, config_(validate(config))
	, pool_(std::make_unique<WorkerPool>(config_.workerThreads))
{
	regionsPerAxis_ = static_cast<std::int32_t>(
		std::llround(kworld::kWorldExtent / config_.regionSize));
	regionCount_ = regionsPerAxis_ * regionsPerAxis_;
	regionMembers_.resize(static_cast<std::size_t>(regionCount_));
	regionOut_.resize(static_cast<std::size_t>(regionCount_));
	regionMessages_.resize(static_cast<std::size_t>(regionCount_));
	regionWrites_.resize(static_cast<std::size_t>(regionCount_));
	regionHits_.resize(static_cast<std::size_t>(regionCount_));
	fullRegions_.assign(static_cast<std::size_t>(regionCount_),
		config_.fullDetailAll);
}

RegionEngine::~RegionEngine() = default;

std::int32_t RegionEngine::regionOf(double x, double z) const
{
	const auto axis = [this](double value)
	{
		double cell = std::floor((value - kworld::kWorldMin) / config_.regionSize);
		if (cell < 0.0)
			return 0;
		if (cell >= static_cast<double>(regionsPerAxis_))
			return regionsPerAxis_ - 1;
		return static_cast<std::int32_t>(cell);
	};
	return axis(x) + regionsPerAxis_ * axis(z);
}

std::vector<std::int32_t> RegionEngine::boxRegions(double x, double z, double half) const
{
	const std::int32_t lo = regionOf(x - half, z - half);
	const std::int32_t hi = regionOf(x + half, z + half);
	std::vector<std::int32_t> out;
	for (std::int32_t rz = lo / regionsPerAxis_; rz <= hi / regionsPerAxis_; ++rz)
	{
		for (std::int32_t rx = lo % regionsPerAxis_; rx <= hi % regionsPerAxis_; ++rx)
			out.push_back(rx + regionsPerAxis_ * rz);
	}
	return out;
}

void RegionEngine::regionBounds(std::int32_t region, double& x0, double& z0, double& x1, double& z1) const
{
	const std::int32_t cx = region % regionsPerAxis_;
	const std::int32_t cz = region / regionsPerAxis_;
	x0 = kworld::kWorldMin + static_cast<double>(cx) * config_.regionSize;
	z0 = kworld::kWorldMin + static_cast<double>(cz) * config_.regionSize;
	x1 = x0 + config_.regionSize;
	z1 = z0 + config_.regionSize;
}

std::int32_t RegionEngine::detailForRegion(std::int32_t region) const
{
	if (config_.fullDetailAll
		|| fullRegions_[static_cast<std::size_t>(region)])
		return static_cast<std::int32_t>(DetailLevel::Full);
	return static_cast<std::int32_t>(DetailLevel::Squad);
}

void RegionEngine::ensureGroups(std::size_t count, double x, double z)
{
	const std::uint64_t groupSeed = streamSeed(seed_, kRegionGroupStream);
	while (prevSquads_.size() < count)
	{
		const state::StandInGroup group{x, z, streamSeed(groupSeed, prevSquads_.size()), 0};
		prevSquads_.push_back(group);
		nextSquads_.push_back(group);
		squadMembers_.emplace_back();
		squadOut_.emplace_back();
		groupsDirty_ = true;
	}
}

void RegionEngine::insert(const flecs::world& ecs, flecs::entity_t raw, std::uint64_t id,
	std::int32_t group)
{
	const flecs::entity entity(ecs.c_ptr(), raw);
	const state::Position* position = entity.get<state::Position>();
	EntityState state;
	state.id = id;
	state.entity = raw;
	state.x = clampWorld(position->x);
	state.z = clampWorld(position->z);
	state.region = regionOf(state.x, state.z);
	state.detail = detailForRegion(state.region);
	if (const state::Mover* mover = entity.get<state::Mover>())
	{
		state.group = mover->group;
		state.hasOrder = mover->hasOrder;
		state.orderX = mover->orderX;
		state.orderZ = mover->orderZ;
		if (mover->target.id != 0)
		{
			const state::Id* target = flecs::entity(ecs.c_ptr(), mover->target.id)
				.get<state::Id>();
			state.target = target != nullptr ? target->value : 0;
		}
		state.hits = mover->hits;
		state.steps = mover->steps;
		state.rng = mover->rng;
	} else {
		state.group = group;
		state.rng = streamSeed(streamSeed(seed_, kRegionEntityStream), id);
	}

	const std::uint32_t slot = static_cast<std::uint32_t>(prev_.size());
	prev_.push_back(state);
	next_.push_back(state);
	slotOf_.emplace(state.id, slot);
	regionMembers_[static_cast<std::size_t>(state.region)].push_back(slot);
	if (state.group >= 0)
	{
		ensureGroups(static_cast<std::size_t>(state.group) + 1, state.x, state.z);
		squadMembers_[static_cast<std::size_t>(state.group)].push_back(slot);
	}
}

state::Mover RegionEngine::moverOf(const EntityState& state) const
{
	state::Mover mover;
	mover.group = state.group;
	mover.hasOrder = state.hasOrder;
	mover.orderX = state.orderX;
	mover.orderZ = state.orderZ;
	if (state.target != 0)
		mover.target.id = prev_[slotOf_.at(state.target)].entity;
	mover.hits = state.hits;
	mover.steps = state.steps;
	mover.rng = state.rng;
	return mover;
}

void RegionEngine::writeMirror(flecs::world& ecs, const EntityState& state) const
{
	flecs::entity entity(ecs.c_ptr(), state.entity);
	entity.set<state::Position>(state::Position{state.x, state.z});
	entity.set<state::Mover>(moverOf(state));
	entity.set<Region>(Region{state.region});
	entity.set<Detail>(Detail{static_cast<DetailLevel>(state.detail)});
}

void RegionEngine::rebuild(flecs::world& ecs)
{
	prev_.clear();
	next_.clear();
	slotOf_.clear();
	for (std::vector<std::uint32_t>& members : regionMembers_)
		members.clear();
	prevSquads_.clear();
	nextSquads_.clear();
	squadMembers_.clear();
	squadOut_.clear();
	groupsDirty_ = false;

	if (const state::StandInGroups* groups = ecs.get<state::StandInGroups>())
	{
		prevSquads_ = groups->groups;
		nextSquads_ = groups->groups;
		squadMembers_.resize(prevSquads_.size());
		squadOut_.resize(prevSquads_.size());
	}
	const state::PlayerAreas* players = ecs.get<state::PlayerAreas>();
	deriveAreas(players != nullptr ? players->centres : std::vector<state::GroundPoint>{});

	// Collect first: adding Mover while iterating would move tables.
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> positioned;
	ecs.each([&positioned](flecs::entity entity, const state::Id& id, const state::Position&)
	{
		positioned.emplace_back(id.value, entity.id());
	});
	std::sort(positioned.begin(), positioned.end());
	for (const auto& [id, raw] : positioned)
		insert(ecs, raw, id, -1);
	for (const EntityState& state : prev_)
		writeMirror(ecs, state);
	if (groupsDirty_)
		ecs.set<state::StandInGroups>(state::StandInGroups{prevSquads_});
	groupsDirty_ = false;
}

void RegionEngine::add(flecs::world& ecs, flecs::entity entity, std::int32_t group)
{
	const std::uint64_t id = entity.get<state::Id>()->value;
	if (!prev_.empty() && id <= prev_.back().id)
		throw std::invalid_argument("region entities must be added in Id order");
	insert(ecs, entity.id(), id, group);
	const EntityState& state = prev_.back();
	// A group's centre starts at its first member.
	if (state.group >= 0 && squadMembers_[static_cast<std::size_t>(state.group)].size() == 1)
	{
		state::StandInGroup& group = prevSquads_[static_cast<std::size_t>(state.group)];
		group.x = state.x;
		group.z = state.z;
		nextSquads_[static_cast<std::size_t>(state.group)] = group;
		groupsDirty_ = true;
	}
	writeMirror(ecs, state);
	if (groupsDirty_)
		ecs.set<state::StandInGroups>(state::StandInGroups{prevSquads_});
	groupsDirty_ = false;
}

bool RegionEngine::place(flecs::world& ecs, std::uint64_t id, double x, double z)
{
	const auto found = slotOf_.find(id);
	if (found == slotOf_.end())
		return false;
	EntityState& state = prev_[found->second];
	state.x = clampWorld(x);
	state.z = clampWorld(z);
	const flecs::entity entity(ecs.c_ptr(), state.entity);
	state::Position* position = entity.get_mut<state::Position>();
	position->x = state.x;
	position->z = state.z;
	const std::int32_t region = regionOf(state.x, state.z);
	if (region != state.region)
	{
		state.region = region;
		state.detail = detailForRegion(region);
		entity.set<Region>(Region{region});
		entity.set<Detail>(Detail{static_cast<DetailLevel>(state.detail)});
		membershipDirty_ = true;
	}
	return true;
}

void RegionEngine::setPlayerAreas(flecs::world& ecs,
	const std::vector<state::GroundPoint>& centres)
{
	ecs.set<state::PlayerAreas>(state::PlayerAreas{centres});
	deriveAreas(centres);

	// Promotion/demotion is a component value change, applied outside the
	// phases; the boundary diff would not see it, so mirror it here.
	for (EntityState& state : prev_)
	{
		const std::int32_t detail = detailForRegion(state.region);
		if (detail == state.detail)
			continue;
		state.detail = detail;
		flecs::entity(ecs.c_ptr(), state.entity)
			.set<Detail>(Detail{static_cast<DetailLevel>(detail)});
	}
}

bool RegionEngine::setGroupCentre(std::int32_t group, double x, double z)
{
	if (group < 0 || static_cast<std::size_t>(group) >= prevSquads_.size())
		return false;
	prevSquads_[static_cast<std::size_t>(group)].x = clampWorld(x);
	prevSquads_[static_cast<std::size_t>(group)].z = clampWorld(z);
	groupsDirty_ = true;
	return true;
}

void RegionEngine::deriveAreas(const std::vector<state::GroundPoint>& centres)
{
	std::vector<Area> areas;
	for (const state::GroundPoint& point : centres)
	{
		const std::int32_t centre = regionOf(clampWorld(point.x), clampWorld(point.z));
		const std::int32_t cx = centre % regionsPerAxis_;
		const std::int32_t cz = centre / regionsPerAxis_;
		Area area;
		for (int dz = -config_.areaRadiusRegions; dz <= config_.areaRadiusRegions;
			++dz)
		{
			for (int dx = -config_.areaRadiusRegions;
				dx <= config_.areaRadiusRegions; ++dx)
			{
				const int rx = cx + dx;
				const int rz = cz + dz;
				if (rx < 0 || rz < 0 || rx >= regionsPerAxis_
					|| rz >= regionsPerAxis_)
					continue;
				area.regions.push_back(rx + regionsPerAxis_ * rz);
			}
		}
		std::sort(area.regions.begin(), area.regions.end());
		if (!area.regions.empty())
			areas.push_back(std::move(area));
	}

	// Merge until no two areas share a region (overlapping areas become one).
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (std::size_t i = 0; i < areas.size() && !changed; ++i)
		{
			for (std::size_t j = i + 1; j < areas.size(); ++j)
			{
				if (!intersects(areas[i].regions, areas[j].regions))
					continue;
				std::vector<std::int32_t> merged;
				std::set_union(areas[i].regions.begin(), areas[i].regions.end(),
					areas[j].regions.begin(), areas[j].regions.end(),
					std::back_inserter(merged));
				areas[i].regions = std::move(merged);
				areas.erase(areas.begin() + static_cast<std::ptrdiff_t>(j));
				changed = true;
				break;
			}
		}
	}
	for (Area& area : areas)
		std::sort(area.regions.begin(), area.regions.end());
	std::sort(areas.begin(), areas.end(),
		[](const Area& left, const Area& right)
		{
			return left.regions < right.regions;
		});
	areas_ = std::move(areas);

	fullRegions_.assign(static_cast<std::size_t>(regionCount_),
		config_.fullDetailAll);
	for (const Area& area : areas_)
	{
		for (const std::int32_t region : area.regions)
			fullRegions_[static_cast<std::size_t>(region)] = true;
	}
}

void RegionEngine::rebuildMembership()
{
	for (std::vector<std::uint32_t>& members : regionMembers_)
		members.clear();
	for (std::size_t slot = 0; slot < prev_.size(); ++slot)
	{
		regionMembers_[static_cast<std::size_t>(prev_[slot].region)]
			.push_back(static_cast<std::uint32_t>(slot));
	}
}

bool RegionEngine::withinKeep(const EntityState& self,
	const EntityState& other) const
{
	const double dx = other.x - self.x;
	const double dz = other.z - self.z;
	return dx * dx + dz * dz
		<= config_.keepRadius * config_.keepRadius;
}

std::uint64_t RegionEngine::nearestTarget(std::uint32_t slot) const
{
	const EntityState& self = prev_[slot];
	const double radius = config_.perceiveRadius;
	if (radius <= 0.0)
		return 0;
	const double radius2 = radius * radius;

	const auto cellOf = [this](double value)
	{
		double cell = std::floor((value - kworld::kWorldMin) / config_.regionSize);
		if (cell < 0.0)
			return 0;
		if (cell >= static_cast<double>(regionsPerAxis_))
			return regionsPerAxis_ - 1;
		return static_cast<std::int32_t>(cell);
	};

	const std::int32_t x0 = cellOf(self.x - radius);
	const std::int32_t x1 = cellOf(self.x + radius);
	const std::int32_t z0 = cellOf(self.z - radius);
	const std::int32_t z1 = cellOf(self.z + radius);
	const long long cells = static_cast<long long>(x1 - x0 + 1)
		* static_cast<long long>(z1 - z0 + 1);

	double bestDist2 = radius2;
	std::uint64_t bestId = 0;
	const auto consider = [&](std::uint32_t candidate)
	{
		if (candidate == slot)
			return;
		const EntityState& other = prev_[candidate];
		const double dx = other.x - self.x;
		const double dz = other.z - self.z;
		const double distance2 = dx * dx + dz * dz;
		if (distance2 > radius2)
			return;
		// Total order: distance, then state::Id (never thread arrival).
		if (distance2 < bestDist2
			|| (distance2 == bestDist2 && (bestId == 0 || other.id < bestId)))
		{
			bestDist2 = distance2;
			bestId = other.id;
		}
	};

	if (cells > kMaxStripCells)
	{
		for (std::uint32_t candidate = 0;
			candidate < static_cast<std::uint32_t>(prev_.size()); ++candidate)
			consider(candidate);
	}
	else
	{
		for (std::int32_t rz = z0; rz <= z1; ++rz)
		{
			for (std::int32_t rx = x0; rx <= x1; ++rx)
			{
				const std::int32_t region = rx + regionsPerAxis_ * rz;
				for (const std::uint32_t candidate :
					regionMembers_[static_cast<std::size_t>(region)])
					consider(candidate);
			}
		}
	}
	return bestId;
}

void RegionEngine::runRegionUnit(std::int32_t region)
{
	UnitOutput& out = regionOut_[static_cast<std::size_t>(region)];
	const std::vector<std::uint32_t>& members
		= regionMembers_[static_cast<std::size_t>(region)];
	out.updates.reserve(out.updates.size() + members.size());

	for (const std::uint32_t slot : members)
	{
		const EntityState& state = prev_[slot];

		// Locked targets persist by id until they leave the larger radius
		// (ARCHITECTURE.md "Threading": references are entity IDs).
		std::uint64_t target = state.target;
		if (target != 0)
		{
			const auto found = slotOf_.find(target);
			if (found == slotOf_.end()
				|| !withinKeep(state, prev_[found->second]))
				target = 0;
		}
		if (target == 0)
			target = nearestTarget(slot);

		EntityUpdate update;
		update.slot = slot;
		update.target = target;
		update.rng = state.rng;
		update.x = state.x;
		update.z = state.z;
		if (state.hasOrder)
		{
			const double dx = state.orderX - state.x;
			const double dz = state.orderZ - state.z;
			const double distance = std::sqrt(dx * dx + dz * dz);
			if (distance > 0.0)
			{
				const double step = std::min(config_.moveSpeed, distance);
				update.x = clampWorld(state.x + dx / distance * step);
				update.z = clampWorld(state.z + dz / distance * step);
			}
		}
		else if (target == 0)
		{
			// Seeded per-entity wander.
			const auto [rng, angle] = stepAngle(state.rng);
			update.rng = rng;
			update.x = clampWorld(state.x + std::cos(angle) * config_.moveSpeed);
			update.z = clampWorld(state.z + std::sin(angle) * config_.moveSpeed);
		}
		update.steps = state.steps + 1;
		out.updates.push_back(update);

		if (target != 0)
		{
			const auto found = slotOf_.find(target);
			const EntityState& victim = prev_[found->second];
			Message message;
			message.type = MessageType::Hit;
			message.dest = victim.region;
			message.slot = found->second;
			message.id = victim.id;
			message.senderSlot = slot;
			message.senderId = state.id;
			out.messages.push_back(message);
		}

		const std::int32_t destination = regionOf(update.x, update.z);
		if (destination != state.region)
		{
			Message message;
			message.type = MessageType::Migrate;
			message.dest = state.region;
			message.toRegion = destination;
			message.slot = slot;
			message.id = state.id;
			out.messages.push_back(message);
		}
	}
}

void RegionEngine::runSquadUnit(std::int32_t squad)
{
	const state::StandInGroup& previous = prevSquads_[static_cast<std::size_t>(squad)];
	const auto [rng, angle] = stepAngle(previous.rng);
	state::StandInGroup& next = nextSquads_[static_cast<std::size_t>(squad)];
	next.x = clampWorld(previous.x + std::cos(angle) * config_.squadSpeed);
	next.z = clampWorld(previous.z + std::sin(angle) * config_.squadSpeed);
	next.rng = rng;
	next.steps = previous.steps + 1;

	UnitOutput& out = squadOut_[static_cast<std::size_t>(squad)];
	const std::vector<std::uint32_t>& members
		= squadMembers_[static_cast<std::size_t>(squad)];
	out.messages.reserve(out.messages.size() + members.size());
	for (const std::uint32_t slot : members)
	{
		const EntityState& member = prev_[slot];
		Message message;
		message.type = MessageType::Order;
		message.dest = member.region;
		message.slot = slot;
		message.id = member.id;
		message.x = next.x;
		message.z = next.z;
		out.messages.push_back(message);
	}
}

void RegionEngine::applyRegionWrites(std::int32_t region, bool hasUpdates,
	std::uint64_t tick)
{
	RegionWrites writes;
	const UnitOutput& out = regionOut_[static_cast<std::size_t>(region)];
	if (hasUpdates)
	{
		for (const EntityUpdate& update : out.updates)
		{
			// Ownership decides who writes (ARCHITECTURE.md "Threading").
			if (prev_[update.slot].region != region)
			{
				++writes.violations;
				continue;
			}
			EntityState& next = next_[update.slot];
			next.x = update.x;
			next.z = update.z;
			next.target = update.target;
			next.rng = update.rng;
			next.steps = update.steps;
			++writes.writes;
		}
	}

	for (const Message& message : regionMessages_[static_cast<std::size_t>(region)])
	{
		if (prev_[message.slot].region != region)
		{
			++writes.violations;
			continue;
		}
		switch (message.type)
		{
		case MessageType::Hit:
			++next_[message.slot].hits;
			++writes.hits;
			if (hitObserver_)
			{
				const EntityState& victim = prev_[message.slot];
				regionHits_[static_cast<std::size_t>(region)].push_back(
					HitEvent{tick, prev_[message.senderSlot].entity, victim.entity,
						region, victim.x, victim.z});
			}
			break;
		case MessageType::Order:
			next_[message.slot].hasOrder = true;
			next_[message.slot].orderX = message.x;
			next_[message.slot].orderZ = message.z;
			++writes.orders;
			break;
		case MessageType::Migrate:
			next_[message.slot].region = message.toRegion;
			++writes.migrations;
			break;
		}
		++writes.writes;
	}
	regionWrites_[static_cast<std::size_t>(region)] = writes;
}

void RegionEngine::tick(flecs::world& ecs, std::uint64_t tick)
{
	++stats_.ticks;
	Clock::time_point mark = Clock::now();
	if (membershipDirty_)
	{
		rebuildMembership();
		membershipDirty_ = false;
	}

	// Without the stand-in workload no unit runs and no message is ever sent: the
	// boundary diff would find prev_ == next_. Skip the O(entities) copy, swap and
	// sync (they cost more than the whole squad AI at thousands of squads).
	if (!config_.standIn && messageRegions_.empty() && !groupsDirty_)
		return;

	// Frozen previous-tick state: every worker reads prev_ for the whole tick.
	next_ = prev_;
	nextSquads_ = prevSquads_;

	// The interval is on the absolute Clock tick, so a loaded save keeps
	// its phase.
	const bool squadTick = tick % config_.squadIntervalTicks == 0;
	std::vector<std::int32_t> regionTasks;
	regionTasks.reserve(static_cast<std::size_t>(regionCount_));
	for (std::int32_t region = 0; config_.standIn && region < regionCount_; ++region)
	{
		if (regionMembers_[static_cast<std::size_t>(region)].empty())
			continue;
		const bool full = config_.fullDetailAll
			|| fullRegions_[static_cast<std::size_t>(region)];
		// Squad-level regions tick at ~1 s (ARCHITECTURE.md "Simulation").
		if (!full && !squadTick)
			continue;
		regionTasks.push_back(region);
	}

	std::vector<std::int32_t> squadTasks;
	if (config_.standIn && squadTick)
	{
		for (std::int32_t squad = 0;
			squad < static_cast<std::int32_t>(prevSquads_.size()); ++squad)
		{
			if (!squadMembers_[static_cast<std::size_t>(squad)].empty())
				squadTasks.push_back(squad);
		}
	}

	for (const std::int32_t region : regionTasks)
	{
		UnitOutput& out = regionOut_[static_cast<std::size_t>(region)];
		out.updates.clear();
		out.messages.clear();
	}
	for (const std::int32_t squad : squadTasks)
		squadOut_[static_cast<std::size_t>(squad)].messages.clear();

	const std::size_t regionTaskCount = regionTasks.size();
	const std::size_t taskCount = regionTaskCount + squadTasks.size();
	std::atomic<std::size_t> nextTask{0};
	std::atomic<int> arrivedPhase1{0};
	std::vector<std::uint64_t> taskCounts(static_cast<std::size_t>(config_.workerThreads), 0);
	stats_.nsPrepare += nsSince(mark);
	pool_->run([&](int worker)
	{
		for (;;)
		{
			const std::size_t index = nextTask.fetch_add(1,
				std::memory_order_relaxed);
			if (index >= taskCount)
				return;
			if (taskCounts[static_cast<std::size_t>(worker)]++ == 0)
				rendezvous(arrivedPhase1, taskCount);
			if (index < regionTaskCount)
				runRegionUnit(regionTasks[index]);
			else
				runSquadUnit(squadTasks[index - regionTaskCount]);
		}
	});

	stats_.nsPhase1 += nsSince(mark);
	noteActiveWorkers(taskCounts);

	// Cross-border effects are ordered by a stable key at the boundary.
	std::vector<Message> messages;
	for (const std::int32_t region : regionTasks)
	{
		const UnitOutput& out = regionOut_[static_cast<std::size_t>(region)];
		messages.insert(messages.end(), out.messages.begin(),
			out.messages.end());
	}
	for (const std::int32_t squad : squadTasks)
	{
		const UnitOutput& out = squadOut_[static_cast<std::size_t>(squad)];
		messages.insert(messages.end(), out.messages.begin(),
			out.messages.end());
	}
	std::sort(messages.begin(), messages.end(),
		[](const Message& left, const Message& right)
		{
			return std::tuple(left.dest, static_cast<int>(left.type), left.id,
				left.senderId, left.toRegion,
				std::bit_cast<std::uint64_t>(left.x),
				std::bit_cast<std::uint64_t>(left.z))
				< std::tuple(right.dest, static_cast<int>(right.type), right.id,
					right.senderId, right.toRegion,
					std::bit_cast<std::uint64_t>(right.x),
					std::bit_cast<std::uint64_t>(right.z));
		});

	std::uint64_t hitMessages = 0;
	std::uint64_t orderMessages = 0;
	std::uint64_t migrationMessages = 0;
	for (const Message& message : messages)
	{
		switch (message.type)
		{
		case MessageType::Hit:
			++hitMessages;
			break;
		case MessageType::Order:
			++orderMessages;
			break;
		case MessageType::Migrate:
			++migrationMessages;
			break;
		}
	}
	stats_.hitMessages += hitMessages;
	stats_.orderMessages += orderMessages;
	stats_.migrationMessages += migrationMessages;

	for (const std::int32_t region : messageRegions_)
		regionMessages_[static_cast<std::size_t>(region)].clear();
	messageRegions_.clear();
	for (const Message& message : messages)
	{
		std::vector<Message>& inbox
			= regionMessages_[static_cast<std::size_t>(message.dest)];
		if (inbox.empty())
			messageRegions_.push_back(message.dest);
		inbox.push_back(message);
	}

	std::vector<char> updateFlags(static_cast<std::size_t>(regionCount_), 0);
	for (const std::int32_t region : regionTasks)
		updateFlags[static_cast<std::size_t>(region)] = 1;

	std::vector<std::int32_t> writeTasks;
	writeTasks.reserve(regionTasks.size() + messageRegions_.size());
	for (std::int32_t region = 0; region < regionCount_; ++region)
	{
		if (updateFlags[static_cast<std::size_t>(region)] != 0
			|| !regionMessages_[static_cast<std::size_t>(region)].empty())
			writeTasks.push_back(region);
	}
	for (const std::int32_t region : writeTasks)
	{
		regionWrites_[static_cast<std::size_t>(region)] = RegionWrites{};
		if (hitObserver_)
			regionHits_[static_cast<std::size_t>(region)].clear();
	}

	const std::size_t writeTaskCount = writeTasks.size();
	std::atomic<std::size_t> nextWrite{0};
	std::atomic<int> arrivedPhase2{0};
	std::vector<std::uint64_t> writeCounts(taskCounts.size(), 0);
	stats_.nsSort += nsSince(mark);
	pool_->run([&](int worker)
	{
		for (;;)
		{
			const std::size_t index = nextWrite.fetch_add(1,
				std::memory_order_relaxed);
			if (index >= writeTaskCount)
				return;
			if (writeCounts[static_cast<std::size_t>(worker)]++ == 0)
				rendezvous(arrivedPhase2, writeTaskCount);
			const std::int32_t region = writeTasks[index];
			applyRegionWrites(region,
				updateFlags[static_cast<std::size_t>(region)] != 0, tick);
		}
	});

	stats_.nsPhase2 += nsSince(mark);
	noteActiveWorkers(writeCounts);

	// Tick boundary: publish the next state, then update the owning region.
	prev_.swap(next_);
	prevSquads_.swap(nextSquads_);
	rebuildMembership();
	syncEcsBoundary(ecs);
	stats_.nsBoundary += nsSince(mark);

	if (hitObserver_)
	{
		std::vector<HitEvent> hits;
		for (const std::int32_t region : writeTasks)
		{
			const auto& regionEvents = regionHits_[static_cast<std::size_t>(region)];
			hits.insert(hits.end(), regionEvents.begin(), regionEvents.end());
		}
		std::sort(hits.begin(), hits.end(),
			[](const HitEvent& left, const HitEvent& right)
			{
				return std::tuple(left.region, left.target, left.archer,
					std::bit_cast<std::uint64_t>(left.targetX),
					std::bit_cast<std::uint64_t>(left.targetZ))
					< std::tuple(right.region, right.target, right.archer,
						std::bit_cast<std::uint64_t>(right.targetX),
						std::bit_cast<std::uint64_t>(right.targetZ));
			});
		for (const HitEvent& event : hits)
			hitObserver_(event);
	}

	stats_.regionSteps += regionTasks.size();
	stats_.squadSteps += squadTasks.size();
	for (const std::int32_t region : writeTasks)
	{
		const RegionWrites& writes = regionWrites_[static_cast<std::size_t>(region)];
		stats_.hitsDelivered += writes.hits;
		stats_.writes += writes.writes;
		stats_.writeViolations += writes.violations;
	}
	for (const std::int32_t region : regionTasks)
		stats_.entityUpdates
			+= regionOut_[static_cast<std::size_t>(region)].updates.size();
}

// Test hook (config_.rendezvousWorkers): a worker that has just claimed its
// first task of a pool run holds it until min(workers, tasks) workers each hold
// one, so a loaded machine cannot let one thread drain the queue. Waiting
// workers each hold a distinct task, so the target is always reachable.
void RegionEngine::rendezvous(std::atomic<int>& arrived, std::size_t taskCount)
{
	if (!config_.rendezvousWorkers || rendezvousTimedOut_.load(std::memory_order_relaxed))
		return;
	const int target = static_cast<int>(std::min<std::size_t>(
		static_cast<std::size_t>(config_.workerThreads), taskCount));
	arrived.fetch_add(1, std::memory_order_acq_rel);
	// Bounded, so a pool that lost its threads fails the test, not hangs it.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	while (arrived.load(std::memory_order_acquire) < target
		&& std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	if (arrived.load(std::memory_order_acquire) < target)
		rendezvousTimedOut_.store(true, std::memory_order_relaxed);
}

void RegionEngine::noteActiveWorkers(const std::vector<std::uint64_t>& taskCounts)
{
	const auto active = static_cast<std::uint64_t>(std::count_if(taskCounts.begin(),
		taskCounts.end(), [](std::uint64_t count) { return count != 0; }));
	stats_.maxActiveWorkers = std::max(stats_.maxActiveWorkers, active);
}

void RegionEngine::syncEcsBoundary(flecs::world& ecs)
{
	// next_ still holds the previous-tick state after the swap.
	for (std::size_t slot = 0; slot < prev_.size(); ++slot)
	{
		EntityState& now = prev_[slot];
		const EntityState& before = next_[slot];
		flecs::entity entity(ecs.c_ptr(), now.entity);
		if (now.x != before.x || now.z != before.z)
		{
			state::Position* position = entity.get_mut<state::Position>();
			position->x = now.x;
			position->z = now.z;
		}
		if (now.region != before.region)
		{
			now.detail = detailForRegion(now.region);
			entity.set<Region>(Region{now.region});
		}
		if (now.detail != before.detail)
			entity.set<Detail>(Detail{static_cast<DetailLevel>(now.detail)});
		if (now.group != before.group || now.hasOrder != before.hasOrder
			|| now.orderX != before.orderX || now.orderZ != before.orderZ
			|| now.target != before.target || now.hits != before.hits
			|| now.steps != before.steps || now.rng != before.rng)
			*entity.get_mut<state::Mover>() = moverOf(now);
	}

	// Group states change only on squad ticks or by command.
	if (groupsDirty_ || prevSquads_ != nextSquads_)
		ecs.set<state::StandInGroups>(state::StandInGroups{prevSquads_});
	groupsDirty_ = false;
}

EntityView RegionEngine::view(flecs::entity entity) const
{
	const state::Id* id = entity.get<state::Id>();
	const auto found = id != nullptr ? slotOf_.find(id->value) : slotOf_.end();
	if (found == slotOf_.end())
	{
		EntityView missing;
		missing.id = entity.id();
		return missing;
	}
	const EntityState& state = prev_[found->second];
	EntityView result;
	result.id = state.entity;
	result.stableId = state.id;
	result.x = state.x;
	result.z = state.z;
	result.region = state.region;
	result.group = state.group;
	result.detail = state.detail;
	result.target = state.target != 0 ? prev_[slotOf_.at(state.target)].entity : 0;
	result.hits = state.hits;
	result.steps = state.steps;
	result.rng = state.rng;
	result.hasOrder = state.hasOrder;
	result.orderX = state.orderX;
	result.orderZ = state.orderZ;
	return result;
}

std::size_t RegionEngine::regionEntityCount(std::int32_t region) const
{
	if (region < 0 || region >= regionCount_)
		return 0;
	return regionMembers_[static_cast<std::size_t>(region)].size();
}

double RegionEngine::squadCentreX(std::int32_t group) const
{
	if (group < 0 || static_cast<std::size_t>(group) >= prevSquads_.size())
		return 0.0;
	return prevSquads_[static_cast<std::size_t>(group)].x;
}

double RegionEngine::squadCentreZ(std::int32_t group) const
{
	if (group < 0 || static_cast<std::size_t>(group) >= prevSquads_.size())
		return 0.0;
	return prevSquads_[static_cast<std::size_t>(group)].z;
}

std::uint64_t RegionEngine::squadSteps(std::int32_t group) const
{
	if (group < 0 || static_cast<std::size_t>(group) >= prevSquads_.size())
		return 0;
	return prevSquads_[static_cast<std::size_t>(group)].steps;
}

void RegionEngine::setHitObserver(
	std::function<void(const HitEvent&)> observer)
{
	hitObserver_ = std::move(observer);
}

} // namespace ksim
