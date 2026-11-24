#pragma once

// Region-owned, two-phase, multithreaded tick (ARCHITECTURE.md "Threading:
// regions with one owner each"). Simulation owns one engine and drives it
// once per tick; it is not used on its own.
//
// Every instance with a state::Position is an engine entity. The engine's
// double-buffered arrays are canonical for its hot state (position, move
// order, and the stand-in fields of state::Mover); at the tick boundary they
// are mirrored one way into flecs (state::Position, state::Mover, Region,
// Detail), and nothing else writes those components (ARCHITECTURE.md
// "Runtime state"). Saves and the state hash read the mirror, which is
// complete between ticks.
//
// Phase 1 workers read the frozen previous-tick state from any region and
// produce per-entity updates plus cross-border messages. Phase 2 workers apply
// writes only to the entities their region owns. Messages are sorted by a
// stable key at the tick boundary, never by thread arrival. Every ordering
// and tie-break uses state::Id, never flecs ids, so a loaded save continues
// exactly as the original run.
//
// The only behaviour is a stand-in workload (RegionConfig::standIn: wander,
// perceive, lock and hit a target, follow a stand-in group), which exercises
// the scheduler, ownership, messaging and migration before any AI exists.
// With it off, entities hold still.

#include "ksim/core/detail.hpp"
#include "ksim/core/state.hpp"

#include <flecs.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace ksim {

// The owning region index: a mirror component value, never a tag, derived
// from the position (not saved).
struct Region
{
	std::int32_t index = -1;
};

struct RegionConfig
{
	// Zone-aligned: a multiple of 4,608 (ARCHITECTURE.md "Threading").
	double regionSize = 4608.0;
	// "a pool of N worker threads"; the main thread is one of them. Results
	// never depend on it.
	int workerThreads = 1;
	// Squad level is ~1 s = 20 ticks at the 20 Hz tick.
	std::uint64_t squadIntervalTicks = 20;
	// Runs the stand-in workload below; off, entities hold still.
	bool standIn = false;
	double perceiveRadius = 300.0;
	double keepRadius = 900.0;
	double moveSpeed = 6.0;
	double squadSpeed = 3.0;
	// The camera focus's area is the square of regions within this Chebyshev radius.
	int areaRadiusRegions = 1;
	// A player character's area is the regions its +-characterBox box overlaps (the original's loaded-zone rule).
	double characterBox = 340.0;
	// Test/reference switch: every region is full detail and ticks every tick.
	bool fullDetailAll = false;
	// Test hook: every worker holds its first task of a pool run until all
	// workers (or all tasks, if fewer) are busy, so "N workers ran" does not
	// depend on thread scheduling. Never changes results.
	bool rendezvousWorkers = false;
};

// A merged full-detail area: the union of the region sets of overlapping
// player positions.
struct Area
{
	std::vector<std::int32_t> regions;
};

// The camera focus that loads its 3x3 zones: the saved state::ViewFocus, else the first player character (no client camera).
std::optional<state::ViewFocus> cameraFocus(flecs::world& ecs, const std::vector<state::GroundPoint>& centres);

// One entity's current state; id and target are flecs ids (for callers that
// hold flecs handles), stableId the state::Id.
struct EntityView
{
	flecs::entity_t id = 0;
	std::uint64_t stableId = 0;
	double x = 0.0;
	double z = 0.0;
	std::int32_t region = -1;
	std::int32_t group = -1;
	std::int32_t detail = 0;
	flecs::entity_t target = 0;
	std::uint32_t hits = 0;
	std::uint32_t steps = 0;
	std::uint64_t rng = 0;
	bool hasOrder = false;
	double orderX = 0.0;
	double orderZ = 0.0;
};

struct HitEvent
{
	std::uint64_t tick = 0;
	flecs::entity_t archer = 0;
	flecs::entity_t target = 0;
	std::int32_t region = -1;
	double targetX = 0.0;
	double targetZ = 0.0;
};

struct RegionStats
{
	std::uint64_t ticks = 0;
	std::uint64_t regionSteps = 0;
	std::uint64_t squadSteps = 0;
	std::uint64_t entityUpdates = 0;
	std::uint64_t hitMessages = 0;
	std::uint64_t orderMessages = 0;
	std::uint64_t migrationMessages = 0;
	std::uint64_t hitsDelivered = 0;
	std::uint64_t writes = 0;
	std::uint64_t writeViolations = 0;
	// Wall-clock nanoseconds per tick segment, accumulated. Serial segments
	// (copy, gather/sort, boundary) bound the speed-up from more workers.
	std::uint64_t nsPrepare = 0;
	std::uint64_t nsPhase1 = 0;
	std::uint64_t nsSort = 0;
	std::uint64_t nsPhase2 = 0;
	std::uint64_t nsBoundary = 0;
	// Highest number of distinct threads that ran a task in one pool run.
	std::uint64_t maxActiveWorkers = 0;
};

class WorkerPool;

// Holds no flecs pointer: the world is passed to each call that mirrors, so
// the owning Simulation stays movable.
class RegionEngine
{
public:
	RegionEngine(std::uint64_t seed, RegionConfig config);
	~RegionEngine();

	RegionEngine(const RegionEngine&) = delete;
	RegionEngine& operator=(const RegionEngine&) = delete;
	RegionEngine(RegionEngine&&) = delete;
	RegionEngine& operator=(RegionEngine&&) = delete;

	// Adopts every instance with a Position, in Id order, plus the
	// StandInGroups and PlayerAreas singletons. A positioned instance
	// without a Mover gets one with a fresh stream. Mirrors everything.
	void rebuild(flecs::world& ecs);

	// Adopts one more positioned instance (between ticks); its Id must be
	// above every Id adopted so far. group (-1 = none) is used when the
	// entity has no Mover yet.
	void add(flecs::world& ecs, flecs::entity entity, std::int32_t group = -1);

	// Command targets, applied at the start of a tick (command.hpp).
	// Player positions; areas whose region sets overlap merge into one.
	// `centres` are the player characters; the camera focus joins them in a started game.
	void setPlayerAreas(flecs::world& ecs, const std::vector<state::GroundPoint>& centres);
	// The zones the player characters and the camera focus would load, and the zones the areas hold now.
	std::set<std::int32_t> loadedZones(const std::vector<state::GroundPoint>& centres, const std::optional<state::ViewFocus>& view, double box = -1.0) const; // box < 0: config characterBox
	std::set<std::int32_t> fullZones() const;
	// False when the group does not exist.
	bool setGroupCentre(std::int32_t group, double x, double z);

	// Between ticks, from the one serial caller that owns the squad AI step
	// (Simulation): moves the entity with this state::Id, updating its region
	// and the flecs mirror. False when it is not an engine entity.
	bool place(flecs::world& ecs, std::uint64_t id, double x, double z);

	// Phases 1 and 2 at Clock tick `tick`, then the boundary mirror.
	void tick(flecs::world& ecs, std::uint64_t tick);

	const std::vector<Area>& areas() const noexcept { return areas_; }
	const RegionConfig& config() const noexcept { return config_; }
	std::size_t entityCount() const noexcept { return prev_.size(); }
	std::int32_t regionCount() const noexcept { return regionCount_; }
	std::int32_t regionsPerAxis() const noexcept { return regionsPerAxis_; }
	std::int32_t regionOf(double x, double z) const;
	// The regions (zones) a +-half box round (x, z) overlaps, clamped to the world; the original's loaded-zone rule is built from these.
	std::vector<std::int32_t> boxRegions(double x, double z, double half) const;
	// The region's world xz rectangle (kWorldMin origin, square regionSize).
	void regionBounds(std::int32_t region, double& x0, double& z0, double& x1, double& z1) const;
	// Which regions are Full detail (the player areas, or every region with fullDetailAll): the LOS hulls follow it.
	const std::vector<bool>& fullRegions() const noexcept { return fullRegions_; }
	std::size_t regionEntityCount(std::int32_t region) const;

	EntityView view(flecs::entity entity) const;
	double squadCentreX(std::int32_t group) const;
	double squadCentreZ(std::int32_t group) const;
	std::uint64_t squadSteps(std::int32_t group) const;

	const RegionStats& stats() const noexcept { return stats_; }
	void setHitObserver(std::function<void(const HitEvent&)> observer);

private:
	struct EntityState
	{
		std::uint64_t id = 0; // state::Id: ordering, references, rng seed
		flecs::entity_t entity = 0; // mirror target only
		double x = 0.0;
		double z = 0.0;
		std::int32_t region = -1;
		std::int32_t group = -1;
		std::int32_t detail = 1; // DetailLevel::Squad
		std::uint64_t target = 0; // state::Id; 0 = none
		std::uint32_t hits = 0;
		std::uint32_t steps = 0;
		std::uint64_t rng = 0;
		bool hasOrder = false;
		double orderX = 0.0;
		double orderZ = 0.0;
	};

	enum class MessageType : std::uint8_t
	{
		Hit = 0,
		Order = 1,
		Migrate = 2,
	};

	struct EntityUpdate
	{
		std::uint32_t slot = 0;
		double x = 0.0;
		double z = 0.0;
		std::uint64_t target = 0;
		std::uint64_t rng = 0;
		std::uint32_t steps = 0;
	};

	struct Message
	{
		MessageType type = MessageType::Hit;
		std::int32_t dest = -1;
		std::int32_t toRegion = -1;
		std::uint32_t slot = 0;
		std::uint32_t senderSlot = 0;
		std::uint64_t id = 0;
		std::uint64_t senderId = 0;
		double x = 0.0;
		double z = 0.0;
	};

	struct UnitOutput
	{
		std::vector<EntityUpdate> updates;
		std::vector<Message> messages;
	};

	struct RegionWrites
	{
		std::uint64_t writes = 0;
		std::uint64_t violations = 0;
		std::uint64_t hits = 0;
		std::uint64_t orders = 0;
		std::uint64_t migrations = 0;
	};

	void insert(const flecs::world& ecs, flecs::entity_t entity, std::uint64_t id,
		std::int32_t group);
	void writeMirror(flecs::world& ecs, const EntityState& state) const;
	state::Mover moverOf(const EntityState& state) const;
	void rendezvous(std::atomic<int>& arrived, std::size_t taskCount);
	void noteActiveWorkers(const std::vector<std::uint64_t>& taskCounts);
	void runRegionUnit(std::int32_t region);
	void runSquadUnit(std::int32_t group);
	std::uint64_t nearestTarget(std::uint32_t slot) const;
	bool withinKeep(const EntityState& self, const EntityState& other) const;
	std::int32_t detailForRegion(std::int32_t region) const;
	void applyRegionWrites(std::int32_t region, bool hasUpdates, std::uint64_t tick);
	void rebuildMembership();
	std::vector<std::vector<std::int32_t>> areaSets(const std::vector<state::GroundPoint>& centres, const std::optional<state::ViewFocus>& view, double box) const;
	void deriveAreas(const std::vector<state::GroundPoint>& centres, const std::optional<state::ViewFocus>& view);
	void syncEcsBoundary(flecs::world& ecs);
	void ensureGroups(std::size_t count, double x, double z);

	std::uint64_t seed_;
	RegionConfig config_;
	std::unique_ptr<WorkerPool> pool_;
	// Set when the test rendezvous gave up; later runs skip it.
	std::atomic<bool> rendezvousTimedOut_{false};

	std::int32_t regionsPerAxis_ = 1;
	std::int32_t regionCount_ = 1;

	std::vector<EntityState> prev_;
	std::vector<EntityState> next_;
	std::vector<state::StandInGroup> prevSquads_;
	std::vector<state::StandInGroup> nextSquads_;
	bool groupsDirty_ = false;
	bool membershipDirty_ = false;
	std::vector<std::vector<std::uint32_t>> squadMembers_;
	std::vector<std::vector<std::uint32_t>> regionMembers_;
	std::unordered_map<std::uint64_t, std::uint32_t> slotOf_;

	std::vector<Area> areas_;
	std::vector<bool> fullRegions_;

	std::vector<UnitOutput> regionOut_;
	std::vector<UnitOutput> squadOut_;
	std::vector<std::vector<Message>> regionMessages_;
	std::vector<std::int32_t> messageRegions_;
	std::vector<RegionWrites> regionWrites_;
	std::vector<std::vector<HitEvent>> regionHits_;

	std::function<void(const HitEvent&)> hitObserver_;
	RegionStats stats_;
};

} // namespace ksim
