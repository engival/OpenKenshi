#pragma once

// The one tick driver (ARCHITECTURE.md "Simulation", "Runtime state").
//
// The simulation owns the loaded World and the region engine (region.hpp).
// Outside code may only write simulation state by enqueueing a Command
// (command.hpp). One tick, in order:
//
//   1. state::Clock advances; it is the only tick counter.
//   2. Commands due at this tick apply, in enqueue order.
//   3. The detail-level systems whose interval divides the Clock tick run.
//   4. The region engine runs phases 1 and 2 and mirrors its hot state into
//      flecs at the boundary; the scheduler state is mirrored too.
//
// Randomness: one RngStream per subsystem, all derived from the one world
// seed by stream number (rng.hpp). The state hash and saves read flecs only
// (save.hpp); between ticks it holds every value that affects a later tick,
// so a loaded save continues exactly as the original run, at any worker
// thread count.
//
// Scheduling follows the detail-level table (ARCHITECTURE.md "Simulation"):
// Full every tick, Squad every ~1 s, Strategic every game-hour. The system
// bodies are stubs; the scheduler and its tick-rate accounting are real. The
// squad AI (ai.hpp, enableAi) hooks the Squad and Strategic intervals; with
// it on, step 3 also runs its group layer (squads) and faction layer, before
// the region engine's tick.

#include "ksim/economy/research.hpp"
#include "ksim/ai/decision_trace.hpp"
#include "ksim/ai/ai.hpp"
#include "ksim/law/alarm.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/world/towncircle.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/core/command.hpp"
#include "ksim/nav/los.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/nav/door_plan.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/save/creator.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/economy/economy.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/save/newgame.hpp"
#include "ksim/nav/path_retry.hpp"
#include "ksim/core/profile.hpp"
#include "ksim/ai/jobs.hpp"
#include "ksim/world/player_squads.hpp"
#include "ksim/economy/machines.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/world/prospect.hpp"
#include "ksim/world/region.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/save/save.hpp"
#include "ksim/ai/sensor.hpp"
#include "ksim/world/world_state.hpp"
#include "ksim/world/spawn.hpp"
#include "ksim/economy/trade.hpp"
#include "ksim/world/beams.hpp"
#include "ksim/world/weather.hpp"
#include "ksim/world/wilderness.hpp"
#include "ksim/world/world.hpp"

#include "kworld/loadorder.hpp"
#include "kworld/roads.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ksim {

// Also the RNG stream numbers (rng.hpp).
enum class Subsystem : std::size_t
{
	FullDetail = 0,
	SquadDetail = 1,
	Strategic = 2,
};

inline constexpr std::size_t kSubsystemCount = 3;

struct SystemState
{
	std::string name;
	DetailLevel level = DetailLevel::Full;
	std::uint64_t intervalTicks = 1;
	std::uint64_t runs = 0;
	std::uint64_t checksum = 0;
	std::uint64_t lastRunTick = 0;
};

class MeshNavigator;

namespace economy
{
class Hooks; // economy_script.hpp, the Lua economy hooks (T4e)
}

// The timed tasks' lengths that TASK_PROGRESS bars read: the restraining bar of a capture chain (12 s: the original's two live runs read 12.3 and
// 12.0, kenshi-rwh.89; the data's task description says 9.9) and the 3 s processing of a new slave (dt15).
inline constexpr double kRestrainSeconds = 12.0;
// rwh.99 section 6: a slave farther than this from its captor (squared 12,500,000 u) escapes; a captor gone for 45 s with nobody to take over it too;
// an escaped slave's heat (the hud16 "Time remaining" float) is 250 s. rwh.106: the state timers run on the world clock from the stamp of the last
// change into state 2 or 3 (Fighter::slaveStamp): 2 -> 3 after 12.0 game hours, 3 -> 0 after 4.0 game days (DAT_141686480 is 24.0 h/day).
inline constexpr double kEscapeDistance = 3535.0;
inline constexpr double kCaptorGoneSeconds = 45.0;
inline constexpr double kEscapeHeatSeconds = 250.0;
inline constexpr double kEscapedSlaveGameHours = 12.0;
inline constexpr double kExSlaveGameHours = 96.0;
inline constexpr double kProcessSeconds = 3.0;

// The Simulation's derived, never-saved runtime state of one world: stamps, order maps, memos and dialogue answers keyed by the Ids and
// ticks of the world they were made in. Simulation derives from it privately and reload() assigns a fresh one, so a member added here is
// dropped on a reload by default, as a fresh process that loads the save has it (kenshi-k16.6.65). A member that is a setting of the
// process or is rebuilt on purpose by reload() stays in Simulation.
struct WorldRuntime
{
	// The original's roads (kworld::RoadNetwork), loaded from the game data on the first long walk, or set by a test. Never saved.
	std::shared_ptr<const kworld::RoadNetwork> roads_;
	bool roadsTried_ = false;
	double roadLookahead_ = 3.0;
	struct RoadGoalStats { std::uint64_t goals = 0, offCentre = 0; double offSum = 0.0, offMax = 0.0; };
	mutable RoadGoalStats roadGoalStats_;
	// A move order that walks in legs (far goal, or a road): the true goal, the road markers still to visit, and where the planned route ends
	// (LOCOMOTION.md D9). Runtime only: a loaded game's walker finishes the route it holds and stops.
	struct FarOrder
	{
		std::uint32_t seq = 0; // the order's clientSeq: the walker's routeSeq while the order stands
		double goalX = 0.0;
		double goalZ = 0.0;
		std::optional<double> goalY;
		std::vector<kworld::RoadNetwork::Marker> markers;
		std::size_t next = 0; // the marker the next leg is planned to; markers.size() is the goal
		std::size_t cur = 0; // the marker the walker's route ends at while onMarker
		bool onMarker = false; // the route is a whole leg to markers[cur]: handed over at the road's width, re-planned from the walker (rwh.268)
		bool done = false; // the last leg is planned: nothing more to add
		bool blocked = false; // the goal could not be routed: a 20 s blocked target, no more asks
		std::uint64_t blockedUntil = 0;
		std::uint32_t legs = 0; // legs planned so far (a clamp or a marker each)
		std::uint32_t clamps = 0; // of them, clamped at a zone exit
		double scale = 0.0; // road look-ahead radius in road widths (0 = the exe's marker-by-marker rule); set at the order's start
		double lookR = 0.0; // look-ahead radius R in u for this order (scale x the route's median road width)
		double radius = 0.0; // the look-ahead radius R of the leg being walked (xz u); 0 = exe rule
		std::size_t lo = 0; // look-ahead: the route segment the walker has reached (never goes back)
		double reachedArc = -1.0; // route arc length up to which the walker has been inside a point's zone
		double tgtArc = -1.0; // route arc length of the target (the target only moves forward); -1 = none yet
		double tgtX = 0.0; // look-ahead: the leg's target on the road route, and where it was planned from
		double tgtZ = 0.0;
		double planX = 0.0;
		double planZ = 0.0;
		bool restored = false; // loaded from a save: the walker's routeSource / clearWhy are transient, so the first step adopts it by routeSeq alone; runtime only
	};
	std::unordered_map<std::uint64_t, FarOrder> farOrders_;
	std::unordered_map<std::uint64_t, double> selfAidSince_; // sim time at which 269 first applied to each down character; derived, never saved
	bool locationsChecked_ = false; // stepBuildings has re-derived every fighter's floor once since this Simulation was built or loaded; runtime only
	PathRetries pathRetries_; // the failed-route ladder per walker (path_retry.hpp); runtime only
	std::uint64_t retriesDoorPaints_ = 0; // MeshNavigator::doorPaints() the ladders were last valid for
	// The aggressors' bond set (CombatArena::setBonded) was built for these faction ids at this tick; rebuilt when the ids change or 200 ticks
	// have passed. Derived, never saved.
	std::vector<std::uint64_t> bondedFactions_;
	std::uint64_t bondedTick_ = 0;
	// refreshJoinGoals ran at this tick for this many arena members (BQ13 / BQ14 join rules, join_rules.hpp); derived, never saved.
	std::uint64_t joinGoalsTick_ = 0;
	std::size_t joinGoalsCount_ = 0;
	bool joinGoalsDone_ = false;
	std::shared_ptr<void> wtPrelude_; // stepWorldTasks' town circles and fortifications, kept while the towns, gate sites and door binding are unchanged (kenshi-k16.6.233)
	std::map<std::uint64_t, std::uint32_t> talkSeqs_; // actor -> the clientSeq of its talk waiting for the pick
	// The trade buyback lists (kenshi-k16.20.16, SIMLINK TRADE 53's tail): (selling player faction entity, merchant squad entity) -> the units
	// that faction sold to that merchant in one trade window, which it buys back at the price the merchant paid. Runtime only: never saved,
	// reset by a load; the window id comes with each order 68 (InventoryOrder::window), so a replay of the order log rebuilds it.
	struct BuybackLine
	{
		state::InventoryEntry kind; // the merge key (sameKind); count and contents unused
		std::uint32_t count = 0; // units still listed
		std::int64_t price = 0; // per unit, what the merchant paid for the latest of them
	};
	struct BuybackList
	{
		std::uint64_t window = 0;
		std::vector<BuybackLine> lines;
	};
	std::map<std::pair<flecs::entity_t, flecs::entity_t>, BuybackList> buyback_;
	// Replies and ends applied this tick, consumed by stepDialogue (they speak through its lambdas); never live across a tick.
	struct DialogueAnswer
	{
		std::uint64_t speaker = 0;
		std::int32_t option = -1; // -1: end, with `reason`
		std::uint8_t reason = 0;
	};
	std::vector<DialogueAnswer> dialogueAnswers_;
	// liveRole's squad leader, once per squad per tick (derived, never saved; cleared when the tick moves on): squad entity -> leader Id.
	mutable std::unordered_map<flecs::entity_t, std::uint64_t> leaderMemo_;
	// The platoon facts the package classes read (blackboard::Situation), as the squad selector last fed them to the board, with the tick it did
	// (derived, never saved). appliedPackage re-reads the ladder from this with advance off, so a reader never moves a class's own state.
	struct SituationMemo
	{
		blackboard::Situation situation;
		std::uint64_t tick = 0;
	};
	mutable std::unordered_map<flecs::entity_t, SituationMemo> situations_;
	// The town occupancy map (town+0x118, rwh.53 section 3): per TownState entity, the Fighter::faction of every squad whose Package_OccupyTown holds
	// it. Derived from the boards each pass, never saved; goal 273's "the town is ours" reads it beside the owner (the owner moves only by SetTownOwner: conquest, scenarios).
	std::unordered_map<flecs::entity_t, std::vector<std::uint64_t>> occupiers_;
	bool occupiersBuilt_ = false; // false after a load or at a start: stepWorldTasks builds occupiers_ whole at once, else at each second's tick (k16.6.320)
	std::unordered_map<std::string, std::uint64_t> raidLeaders_;
	// Victory AI (raid_phases.cpp watchRaidVictory, FACTIONS.md 2.1b): a force that won its town holds it, GroupAI::until and the order's deadline at
	// kRaidHold, until the occupation ends or the force is killed. `raidWon_`: the forces that won in this pass (applied at the next); `raidHolds_`:
	// per held force, when it was first seen held and when a member last occupied (the end rule). Derived, never saved: a loaded hold starts its
	// grace again.
	struct RaidHold
	{
		std::uint64_t seen = 0;
		std::uint64_t occupied = 0;
		std::uint64_t looked = 0; // the last pass a built held squad was looked at (an unbuilt force is not judged)
	};
	std::unordered_map<std::string, RaidHold> raidHolds_;
	std::set<std::string> raidWon_;
	std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> fightPairs_; // squad Ids (low, high) -> the tick of their last fight (not saved)
	std::map<std::uint64_t, double> memberScores_; // member Id -> its template's score, for a member with no record (token_fights.cpp; derived, not saved)
	// Task BO: the (member, body) pairs a world task gave up on and the tick they may be tried again (derived, not saved).
	std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> worldTaskBlocks_;
	bool routeTrace_ = false; // setRouteTrace
	// A walking interact that has not moved: where it stood and since which tick (stepInteractions' stall watch; derived, not saved).
	struct InteractStall { double x = 0.0; double z = 0.0; std::uint64_t since = 0; double left = 0.0; bool onRoute = false; };
	std::map<std::uint64_t, InteractStall> interactStall_;
	// A slave's captor hand and its timers (slave_state.cpp; the captor is saved as the 187 row's target, the rest is derived).
	struct SlaveTrack
	{
		std::uint64_t captor = 0;
		std::uint64_t captorSquad = 0; // the captor's squad when it was set: who may take over when it is gone
		std::uint64_t goneSince = 0;   // the tick the captor was first found unresolvable (0: it resolves)
	};
	std::map<std::uint64_t, SlaveTrack> slaveTrack_;
	// The bodies members have on goal 58 (body Id -> how many): the lift (168 / 170) and the squad's delivery wait while it is held. Rebuilt from the
	// members' Routines each tick in stepSlaveTrade (rwh.111; derived, not saved).
	std::map<std::uint64_t, int> medicsOn_;
	// The tick up to which a freshly cuffed body's lift waits for the squad's next selector pass to put its medics on it (body Id -> tick; derived, not saved).
	std::map<std::uint64_t, std::uint64_t> aidGrace_;
	// The tick a member began carrying a body its package has no placing row for (world_tasks.cpp: a person is put down after 30 s; derived, not saved).
	std::unordered_map<std::uint64_t, std::uint64_t> carryIdleSince_;
	// Slaves whose Routine the graft started (rwh.99); only these are released when the graft ends. Not saved.
	std::set<std::uint64_t> graftRoutines_;
	// Task dt27 (230 EAT_A_RANDOM_DEAD_BODY): the bites a corpse has taken (MedicalSystem "eaten" counter ms+0x15c, dt27 section 2.3) and
	// the corpses whose last bite landed, removed at the next pass (the executor loop is mid-iteration over the arena). Derived, not saved.
	std::map<std::uint64_t, CombatArena::EatState> eatenBites_;
	std::vector<std::uint64_t> eatenQueue_;
	// Task fog-eat (139 / 237): the live victims carrying the "being fed on" mark (char+0x279, dt24_237 section 2), by the tick it lapses. Derived, not saved.
	std::map<std::uint64_t, std::uint64_t> eatenUntil_;
	// TortureBuilding+0x92: the armour-strip accumulator (game seconds), per peeler building; reset when a piece goes. Derived, not saved.
	std::map<std::uint64_t, double> tortureStrip_;
	struct LockOrder
	{
		std::uint16_t task = 0;
		std::uint64_t victim = 0;
		std::uint8_t action = 0; // the order 60 action (15..20) event 13 names when the order ends; 0 = the planner's, no event
		std::uint64_t cage = 0; // actions 18..20: the cage key event 13 and its progress bar name as the target (victim = an occupant)
	};
	std::map<std::uint64_t, LockOrder> lockOrders_; // actor -> the lock task it was ordered to run
	struct ConstructionOrder
	{
		std::uint16_t task = 0;
		std::uint64_t building = 0;
		std::uint64_t since = 0; // the tick it was given: the walk gives up after kGiveUpSeconds
		double elapsed = 0.0; // 71's warm-up progress (0..1)
	};
	std::map<std::uint64_t, ConstructionOrder> buildOrders_; // actor -> its construction order (unsaved)
	std::map<std::uint64_t, double> buildPower_; // building -> this tick's summed builder power (Sum skillMult x 0.75^k, stepConstruction reads it)
	std::map<std::uint64_t, unsigned> buildersOn_; // building -> builders counted this tick (the damping index k)
	struct ArrestOrder
	{
		std::uint16_t lift = 240;
		std::uint64_t victim = 0;
		std::uint64_t since = 0;
		bool started = false; // Task_Kidnapping's start (246 only) ran
		bool snatched = false; // 213 took the body off its carrier once; after that a carried body is only waited for
		bool gaveUp = false; // it failed (task+0x80): the next tick notices once more and ends the order
	};
	std::map<std::uint64_t, ArrestOrder> arrestOrders_; // guard -> its arrest order (saved: ArrestStates)
	struct SmashOrder
	{
		std::uint64_t building = 0;
		std::uint64_t since = 0;
		double elapsed = 0.0; // the time since the last blow
	};
	std::map<std::uint64_t, SmashOrder> smashOrders_; // actor -> its smash order (unsaved)
	struct CropOrder
	{
		std::uint64_t plot = 0;
		std::uint64_t since = 0;
		bool ate = false; // the first bite (the crime) was taken
	};
	std::map<std::uint64_t, CropOrder> cropOrders_; // member -> its 231 order (unsaved)
	bool cropsReady_ = false; // the plots exist (made, or found in a save)
	struct StandingRun
	{
		double x = 0.0; // where the goal walks to (30's hand, 24's town centre, 19's home building)
		double z = 0.0;
		double radius = 0.0; // inside this the walk is over
	};
	std::map<std::uint64_t, StandingRun> standingRun_; // member -> its standing goal's place (unsaved)
	struct LimbPlay
	{
		double x = 0.0; // 278's current destination
		double z = 0.0;
		std::uint32_t picks = 0;
	};
	std::map<std::uint64_t, LimbPlay> limbPlay_; // member -> its 278 run (saved: RunStates)
	alarm::Board alarms_; // saved: RunStates
	std::map<std::uint64_t, double> alarmDwell_; // member -> seconds left of the 209 / 210 wait at the alarm (this+0x7c; saved: RunStates)
	std::set<std::uint64_t> announced_; // 103 / 129: members that made their announcement at the package target (derived, unsaved)
	mutable std::uint64_t leaderMemoTick_ = ~std::uint64_t(0);
};

class Simulation : private WorldRuntime
{
public:
	// Fixed tick of 0.05 sim-seconds (clock.hpp).
	static constexpr double kTickSeconds = ksim::kTickSeconds;
	// ARCHITECTURE.md table: squad ~1 s, strategic one game-hour (2182 ticks
	// = 109.1 s at the exe's 1x day length, clock.hpp).
	static constexpr std::uint64_t kFullIntervalTicks = 1;
	static constexpr std::uint64_t kSquadIntervalTicks = 20;
	// The once-a-second passes spread over the second (OURS, k16.6.232): an id is stepped on the tick whose phase is id % kSquadIntervalTicks, so
	// each squad / member keeps its 1 s period but a tick carries about 1/20 of the work, not all of it.
	static constexpr bool inSlice(std::uint64_t now, std::uint64_t id) { return now % kSquadIntervalTicks == id % kSquadIntervalTicks; }
	// Game speed is a command (k16.6.633): above 1 the decision cadences of NPCs not in a fight stretch by min(speed, 3), whole steps only
	// (1.5 -> 1, 2 -> 2, 5 -> 3). Speed 1 leaves every cadence as it was. Not saved; the client re-sends it.
	void setGameSpeed(double speed) noexcept;
	double gameSpeed() const noexcept { return gameSpeed_; }
	std::uint32_t cadenceScale() const noexcept { return cadence_; }
	// The routine scorer's period for a member: stretched by the cadence scale unless it is a player character or in a fight.
	std::uint64_t scoreInterval(const state::Fighter& f) const noexcept
	{
		return routines::kScoreIntervalTicks * (f.playerControlled || inFight(f) ? 1u : cadence_);
	}
	// inSlice at the scaled period; an exempt member (player faction, in a fight) keeps the plain second.
	bool inSliceScaled(std::uint64_t now, std::uint64_t id, bool exempt) const noexcept
	{
		const std::uint64_t period = exempt ? kSquadIntervalTicks : kSquadIntervalTicks * cadence_;
		return now % period == id % period;
	}
	static constexpr std::uint64_t kStrategicIntervalTicks = kTicksPerGameHour;

	// Templates only, vanilla load order, no instances.
	static Simulation load(std::string_view dataDir, std::uint64_t seed = 1,
		const RegionConfig& regions = {});

	// Templates from order, then the default world (spawn.hpp); the seed is
	// options.seed.
	static Simulation newGame(std::string_view dataDir, const kworld::LoadOrder& order,
		const SpawnOptions& options = {}, SpawnReport* report = nullptr,
		const RegionConfig& regions = {});

	// Templates from order, then a runtime save (save.hpp). A save made with
	// another load order throws SaveError unless force; result (if given)
	// says what the save recorded.
	static Simulation loadGame(std::string_view dataDir, const kworld::LoadOrder& order,
		const std::string& path, bool force = false, SaveLoadResult* result = nullptr,
		const RegionConfig& regions = {});

	// A save stream over the templates of world (which holds no instances).
	// gameData (the one the templates came from) lets a save with wilderness
	// state keep respawning (wilderness.hpp); without it the wilderness
	// spawner stays off and the holes stay open.
	static Simulation restore(World world, std::istream& in,
		const RegionConfig& regions = {}, bool force = false,
		SaveLoadResult* result = nullptr,
		std::shared_ptr<const kdata::GameData> gameData = nullptr);

	// Replaces every instance of this simulation with a save stream's (SIMLINK load_game): templates, combat, navigation,
	// AI configuration and observers stay, the scheduler, regions, arena, AI and weather are rebuilt from the loaded state
	// as `restore` + the same `enable*` calls would. Between ticks. A save that cannot be read (SaveError) is found out on
	// a scratch world first and changes nothing. The clock jumps to the save's tick.
	SaveLoadResult reload(std::istream& in, bool force = false);

	// Between ticks. Writes the pending commands too.
	void save(std::ostream& out) const;
	void saveGame(const std::string& path) const;

	// Takes ownership of a world and adopts its state. A world holding a
	// saved state::Scheduler keeps its saved seed; seed is used otherwise.
	explicit Simulation(World world, std::uint64_t seed = 1,
		const RegionConfig& regions = {});

	void tick();
	// CHAR_MEDICAL's portrait bits (SIMLINK.md; kenshi-k16.6.160). putIn: lying in a real bed (asleep by its own bed routine, or put in one
	// by a carrier: a floor sleeper and a caged character never). hurt: the portrait's red background, see the .cpp.
	bool lyingInBed(std::uint64_t id, const state::Fighter& f) const;
	bool portraitHurt(std::uint64_t id, const state::Fighter& f) const;
	void run(std::uint64_t ticks);

	// Turns the squad AI on (ai.hpp): the group layer runs at the Squad
	// interval, the faction layer at the Strategic one, both before the
	// region engine's tick. Squads then hold a state::GroupAI (saved). The
	// registry and navigator are configuration like the load order, not
	// state: load a save with the same ones. Off by default (the region
	// engine's stand-in workload and the AI would both move squads).
	void enableAi(AiSystem::Config config);
	const AiSystem* ai() const noexcept { return ai_.get(); }
	// The merged game data the templates came from; null for a simulation
	// built from a bare World (Simulation::load). FactionBook::build wants it.
	std::shared_ptr<const kdata::GameData> gameData() const noexcept { return gameData_; }
	// For a simulation made with load() (no game data kept): the faction records the door steps read relations from.
	void setGameData(std::shared_ptr<const kdata::GameData> gameData);

	// The character layer (combat.hpp): turns the combat arena on. Every instance already
	// holding a state::Fighter (a loaded save) joins it. Configuration like enableAi: a
	// loaded save needs the same data and settings. Only fighters in the arena cost anything.
	void enableCombat(std::shared_ptr<const CombatData> data, CombatSettings settings = {});
	// The navmesh the full-detail characters live on (SIMLINK.md section 12): their moves slide
	// along it, move orders are routed on it, an off-mesh character is snapped back with an
	// event. Configuration like enableCombat (derived data, never saved); call before the
	// characters spawn, in either order with enableCombat. Without one, move and drive orders
	// are refused: no navmesh, no nav.
	void enableNavigation(std::shared_ptr<const MeshNavigator> navigation);
	const MeshNavigator* navigation() const noexcept { return navigation_.get(); }
	// MeshNavigator::routeFrom for a walker whose owner asks again when it fails (LOCOMOTION.md 9, D8): after a failed ask for the same goal the
	// next ask is offset sideways, then the road-network hook, then not made at all (path_retry.hpp). `routeStuck`: every rung is used up, the
	// failure an owner acts on (drops the goal, picks another). The first ask is plain routeFrom.
	Route routeRetry(std::uint64_t id, const state::Fighter& f, double toX, double toZ, std::optional<double> toY, bool planned);
	// The leg-by-leg order of a walker, or null (tests: far_orders).
	const FarOrder* farOrder(std::uint64_t id) const { const auto it = farOrders_.find(id); return it == farOrders_.end() ? nullptr : &it->second; }
	bool routeStuck(std::uint64_t id) const { return pathRetries_.stuck(id); }
	// Every ladder starts over, as after a load (the ladder is runtime only, never saved): a caller that wants the running game to continue exactly as its
	// own save would (ksim_world_follow) calls this at the save point.
	void forgetPathRetries() { pathRetries_.resetAll(); }
	// The road layer outside the navmesh search (PATHFINDING_STATIC.md 1.13, 2.8; roads_static.md). roadNetworkRoute is the ladder's road
	// rung (14065f940, factor 0.2): navmesh legs through the road's markers to the goal, not found when no road serves the move. A move over
	// kLongMove takes the same shortcut first (factor = roadFactor) when the use test passes.
	Route roadNetworkRoute(const state::Fighter& f, double toX, double toZ, std::optional<double> toY = std::nullopt, bool planned = true,
		double factor = kworld::RoadNetwork::kStuckFactor) const;
	// The traveller's road use-test factor: 1 - the road preference of its faction's record (0.6 when it has none).
	double roadFactor(std::uint64_t id) const;
	// Tests: use this network instead of loading the install's.
	void setRoadNetwork(std::shared_ptr<const kworld::RoadNetwork> roads) { roads_ = std::move(roads); roadsTried_ = true; }
	// The same navigator as its shared owner (server_ai.hpp): the squad AI's navigator when the server has no world bake of its own.
	std::shared_ptr<const Navigator> navigationOwner() const noexcept { return navigation_; }
	// Line of sight (task AZ; docs/reference/LINE_OF_SIGHT.md): the placed buildings' collision as per-region hull trees,
	// built for a region when it is Full detail and freed when it leaves; `lineOfSight()->trace` answers a segment against
	// the hulls and the terrain. Phase 2 (task AZ) turns it on by itself once the world has placements and combat is on, and
	// gives perception the `lineOfSight` hook (only where both characters are at Full detail). `disableLineOfSight` is for
	// tests that want the old coarse sight. Needs the world's placements and the game data directory; call between ticks.
	void enableLineOfSight();
	void disableLineOfSight() noexcept;
	bool lineOfSightEnabled() const noexcept { return los_ != nullptr; }
	const LineOfSight* lineOfSight() const noexcept { return los_.get(); }
	// Builds the trees of the Full regions and frees the rest. enableLineOfSight, a player-areas change and the end of every
	// tick (after the region engine's own tick, where the areas' Full set moves) call it; a tick that changes nothing keeps
	// the pair cache.
	void syncLineOfSight(bool spread = false); // `spread`: at most one new region's hull trees a tick (the per-tick call)
	// A new full-detail character: an instance holding `fighter`, owned by the arena.
	// A fighter that is not in a building (`building` 0) and not carried is placed by its position (buildingAt).
	flecs::entity spawnFighter(state::Fighter fighter);
	// The same on an entity that already exists (a resident's CharacterInstance): its Inventory is kept, the fighter joins the arena
	// in Id order. Combat must be enabled.
	void adoptFighter(flecs::entity entity, state::Fighter fighter);
	// Takes a fighter out of the arena (a despawn, a region hand-off): what it carries is put down where it stands, whoever
	// carries it is freed, take-outs and lifts aimed at it end. Its entity stays, without the Fighter component. Nothing if absent.
	void removeFighter(std::uint64_t id);
	// The building (BuildingSite::key) that holds this point, 0 outdoors: the smallest whose interior mask hull contains it, or,
	// for a building with no mask, whose footprint does and which has a door with a way in (state::Door::inX). stepBuildings
	// applies it each tick to every fighter that moved since its last look.
	std::uint64_t buildingAt(double x, double y, double z);
	// The one placement for a character put at a saved/authored spot (residents, scenario, squad import, save import): the navmesh's
	// nearest walkable ground within `reach`, then, outdoors only, settled to the terrain (Walker::settle, the runtime mesh floats up
	// to 1.14 u above it); a floor inside a building keeps the mesh height. Nullopt off the mesh or with no navigation (kenshi-k16.6.296).
	std::optional<knav::Position> placeOnGround(double x, double y, double z, double reach = 100.0);
	// The floor of `building` at height y (CHAR_DYNAMIC `floor`): 0 on the ground, 1 the first upper floor (BuildingSite::floorAt).
	// 0 outside a building or for a building whose floors the data does not give.
	int buildingFloor(std::uint64_t building, double y);
	// Puts a character in a building at height y: building, floor and the indoors state (under a ceiling: BuildingSite::indoorsAt)
	// with the building's `cavernous`. Every path that sets a fighter's building goes through it (spawn, placeInBuildings,
	// stepBuildings, a resident placed at its spot).
	void setLocation(state::Fighter& fighter, std::uint64_t building, double y);
	// Under the roof of an `is shelter` building (the exe's acid-rain down ray, weather_acid_rain.md section 5): true when the
	// point lies in the footprint of a shelter building whose top is above y + kRoofClearance (the topmost one at that column,
	// as the ray finds it; a top deck's own surface is not 30 u up, so standing on a roof does not shelter).
	bool shelteredAt(double x, double y, double z) const;
	// The routing clearance of a character: its race's `pathfind footprint radius` (CombatData::pathRadius; the merged
	// Greenlander is 4.0, the exe's own). Every route a character asks for passes it (MeshNavigator::routeFrom `radius`,
	// docs/formats/NAVMESH_CACHE.md "Clearance"); with no combat data there are no characters and it is never asked.
	double pathRadius(const state::Fighter& f) const;
	// pathRadius and the race water cost scale: what a route is planned for (Body).
	Body body(const state::Fighter& f) const;
	// Places every fighter with no building by its position (buildingAt): the fighters that spawned before the doors existed.
	void placeInBuildings();
	const CombatArena* combat() const noexcept { return combat_.get(); }
	bool townOursForTest(double x, double z, std::uint64_t faction) const { return townOursAt(x, z, faction); } // the occupancy map's reader (ksim_slice_load_parity)
	// New interactables (interact.hpp): instance entities with a state::Id from the characters' counter.
	flecs::entity spawnDoor(state::Door door);
	// A town gate ceases to exist (state::GatesGone): its crew rebinds on the next second (gatebind.hpp). Idempotent.
	void removeGate(std::uint64_t doorKey);
	flecs::entity spawnGroundItem(state::GroundItem item);
	flecs::entity spawnItemNode(state::ItemNode node);
	// The interiors.level layouts, loaded on first use; nullptr when there are none (routine_scorer.cpp).
	const routines::LayoutLibrary* interiorLayouts() const;
	// The scorer's site index entry of a site (position, facing); null before the index knows the building.
	const routines::SiteEntry* siteEntry(const routines::Site& site) const;
	// A storage building: the Container component and its contents (a state::Inventory, all carried) on one new instance.
	flecs::entity spawnContainer(state::Container container, state::Inventory contents);
	// The reason an equip order (SIMLINK order 62) is refused, "" when it is fine: "bad_target" (not an item the character has
	// that fits a slot), "not_allowed" (the item is not carried / not equipped, or the fighter is down or mid-swing).
	std::string checkEquip(std::uint64_t who, const std::string& item, bool equip) const;
	// The reason a transfer order (SIMLINK order 63) is refused, "": "bad_target" (no such container), "not_allowed" (too
	// far, or fewer than `count` there). take: container -> character; else character -> container.
	std::string checkTransfer(std::uint64_t who, std::uint64_t container, const std::string& item, std::uint32_t count, bool take) const;
	// What holds items for orders 63 / 64 / 65 / 68 and the loot-open feed (SIMLINK.md section 13): a storage container, or a KO'd or
	// dead character (a body; a character that is up is not storage). `entity` 0 = no such storage. x, y, z: the container's use
	// point, or where the body lies. ownerSid: the container's owner faction, or the body's faction (state::Allegiance); a take
	// by a member of another faction is theft.
	struct Storage
	{
		flecs::entity_t entity = 0;
		bool body = false;
		bool producer = false; // a state::Producer's output slot: take, or put back the produced item
		bool trader = false; // a merchant squad's stock (state::TraderStock + Inventory): orders 68 only (trade.hpp); the range is the trader character's
		double reach = kTransferReach; // a producer's is its `use range`: an operator may stand that far from it
		double reachStopped = 2.0 * kTransferReach; // once the actor stands still (interact.hpp, the shared reach rule)
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		std::string ownerSid;
	};
	Storage findStorage(std::uint64_t id) const;
	// How many entries the derived, never-saved runtime state holds (WorldRuntime: the world task blocks, the order maps, the dialogue throttle ...): 0 in a
	// world just loaded or reset, so a test can tell that a reload dropped what the world before it left (kenshi-k16.6.65). Not counted: the squad
	// situation memo, which the reload itself refills at once. Observation only.
	std::size_t worldRuntimeEntries() const;
	// Whether an unsaved order of the runtime state serves this member now (arrest, smash, construction, lock, crop or standing goal): a goal
	// that rides a Routine with no order behind it is one a load left stuck (kenshi-k16.6.73). Observation only.
	bool holdsOrder(std::uint64_t id) const;
	// ---- trade (trade.hpp, SIMLINK TRADE 53; the window itself is the Session's) ----
	// The price pairs of a trade for the TRADE frame: `stock` prices the trader squad's INVENTORY entries (the wire's flat order), `actor`
	// the actor's own (the buying price, -1 when the trader will not buy it; an equipped entry is -1).
	struct TradeView
	{
		std::uint64_t squad = 0;
		std::string templateSid;
		std::int64_t traderMoney = 0;
		std::uint32_t stockVersion = 0;
		std::uint32_t actorVersion = 0;
		struct StockPrice
		{
			std::uint16_t index = 0;
			std::int32_t buy = 0;
			std::int32_t sell = -1;
		};
		std::vector<StockPrice> stock;
		std::vector<std::pair<std::uint16_t, std::int32_t>> actor;
		// The buyback tail (kenshi-k16.20.16): the stock entries (the same flat order) that hold units the actor's faction sold in this window,
		// how many and the per-unit price they come back at. Empty for any other faction or window.
		struct BuybackUnits
		{
			std::uint16_t index = 0;
			std::uint32_t count = 0;
			std::int32_t price = 0;
		};
		std::vector<BuybackUnits> buyback;
		// Every container of the actor's player faction that order 68's `source` accepts (tradeSourceFault), by container id: its INVENTORY
		// version and the trader's price per entry (the wire's flat order, -1 = he will not buy it).
		struct ChestPrices
		{
			std::uint64_t container = 0;
			std::uint32_t version = 0;
			std::vector<std::pair<std::uint16_t, std::int32_t>> prices;
		};
		std::vector<ChestPrices> chests;
	};
	// The squad Id of a character that belongs to a merchant squad (TraderStock + Inventory); 0 for any other.
	std::uint64_t traderSquad(std::uint64_t character) const;
	// Whether `trader` can still trade with `actor`: "" yes; "gone" a party is not in the arena, or the trader is asleep (a KO or a death does not count, MQ11).
	// Distance never closes a trade (MQ8): it refuses each transfer.
	std::string tradeLost(std::uint64_t actor, std::uint64_t trader) const;
	// The prices of the trade between `actor` and the character `trader` in trade window `window` (the Session's OpenTrade::window, 0 when
	// it has none); false when either is gone.
	bool tradeView(std::uint64_t actor, std::uint64_t trader, std::uint64_t window, TradeView& out) const;
	const trade::Book& tradeBook() const;
	// The fighter's numbers after its equipped `itemSid` left it (order 67 unequip, stripping a body by 63 / 68): a weapon
	// becomes fists, an armour piece leaves the fighter; a backpack changes nothing.
	void unequipFromFighter(std::uint64_t id, const std::string& itemSid);
	// Whether the character `who` stands within the use range of the storage (kTransferReach in xz, kInteractReachY in height).
	static bool inStorageReach(const state::Fighter& who, const Storage& storage) noexcept;
	// Grid inventory orders 64-68 (inventory_orders.cpp). The reason it is refused, "" when it would apply: "stale" (the INVENTORY
	// version is not the client's), "no_room", "occupied", "not_allowed", "bad_value", "bad_target", "unknown_id", and the
	// container reasons of task AX: "locked" (a `has lock` box, state 2), "item_type" (its `itemtype limit` refuses the kind),
	// "out_of_reach" (no use range to the box) and "full" (the box's grid has no room for the stack). Changes nothing.
	std::string checkInventoryOrder(const InventoryOrder& order) const;
	// Order 68 against a merchant's stock (trade.hpp). Judges the order's parties and prices; `count` comes back as the units that can move
	// (a short wallet moves fewer) and `deal` as the money. The reasons are SIMLINK TRADE's: out_of_range, cant_afford, shop_cant_afford,
	// not_for_sale, illegal_only; "" when something moves.
	struct Deal
	{
		std::int64_t amount = 0; // the money that changes hands
		bool buying = false; // the player pays
		flecs::entity_t payer = 0; // the player faction entity
		flecs::entity_t squad = 0; // the merchant squad entity
		std::uint64_t window = 0; // the order's trade window (the buyback list's)
		std::uint32_t units = 0; // the units that move
		std::uint32_t buybackUnits = 0; // a buy: how many of them come back from the buyback list, at the listed price
		state::InventoryEntry kind; // the stack's merge key, as the merchant's stock holds it
	};
	std::string tradeParties(const InventoryOrder& order, const Storage& stock) const;
	// Whether `source` may feed a sale by `actor` to the character `trader` (order 68's `source`, and the TRADE frame's chest prices):
	// "" yes, "not_allowed" (not an owned storage container of the actor's faction), "out_of_range" (XZ container to trader >= 1000).
	std::string tradeSourceFault(std::uint64_t actor, std::uint64_t source, const state::Fighter& trader) const;
	std::string tradeTerms(const InventoryOrder& order, const Storage& stock, const state::InventoryEntry& entry, bool buying, std::uint32_t& count, Deal& deal) const;
	void tradeSettle(const Deal& deal);
	// The units of `kind` the buyback list of (payer, squad) holds in `window` (a list of another window is not this trade's: 0) and their price.
	const BuybackLine* buybackLine(flecs::entity_t payer, flecs::entity_t squad, std::uint64_t window, const state::InventoryEntry& kind) const;
	// Player squads (player_squads.hpp, player_squads.cpp; SIMLINK PLAYER_SQUADS 31, orders 70-75). The limits of the merged data's GLOBAL
	// CONSTANTS; throws when there is no game data or the record lacks a field.
	SquadLimits squadLimits() const;
	// Makes the FactionState with this template sid a player faction (state::PlayerFaction, no squads yet); returns its Id.
	// Between ticks. Throws when there is no such faction.
	std::uint64_t createPlayerFaction(const std::string& factionSid);
	// Setup (scenarios, tests): appends a squad to the player faction (default name `<faction>_<N>`, counter + 1); addPlayerMember appends a character at a squad's end. Between ticks; unchecked against the limits (orders 70-75 are the checked path).
	std::uint64_t addPlayerSquad(const std::optional<std::string>& name = std::nullopt);
	void addPlayerMember(std::uint64_t squad, std::uint64_t character);
	// The reason a squad order (70-75) is refused for the player faction, "" when it would apply: "not_allowed", "bad_target",
	// "bad_value", "unknown_id", "not_player_controlled", "dead". Changes nothing.
	std::string checkSquadOrder(const SquadOrder& order) const;
	// The reason a DA_JOIN_SQUAD_* join (Command::ChangeSquad) is refused, "" when it would apply: "unknown_id", "not_allowed" (no
	// player faction or squad, the roster or the squad is full, the character is down), "bad_target" (not the player's squad).
	// `squad` 0 = the player faction's first squad. Changes nothing.
	std::string checkChangeSquad(std::uint64_t character, std::uint64_t squad) const;
	// DA_TAKE_MONEY / DA_GIVE_MONEY (DIALOGUE.md 5): credits (positive) or debits (negative) the first player faction's wallet
	// (state::PlayerFaction::money). False when there is no player faction.
	bool adjustPlayerMoney(std::int64_t delta);
	// The dialogue economy's other half (DIALOGUE.md D5): credits (positive) or debits (negative) a FACTION instance's cash
	// (state::FactionState::cash, save v70). False when there is no such faction instance.
	bool adjustFactionCash(const std::string& factionSid, std::int64_t delta);
	// DA_SEPARATE_TO_MY_OWN_SQUAD (DIALOGUE_ACTIONS_STATIC 28): `character` splits into a squad of its own (a new player squad for a
	// player member, a new SquadInstance of its own faction otherwise); already alone = nothing to do, true. False for a dead character
	// or a full player squad list.
	bool separateToOwnSquad(std::uint64_t character);
	// DA_REMOVE_SLAVE_STATUS (53): not a slave, no captor, out of its cage slot, its orders cleared. False for an unknown character.
	bool removeSlaveStatus(std::uint64_t character);
	// The world's events (WORLD_EVENT_STATE, world_state.hpp; task AN). Facts builds the live evaluator input (every unique
	// character's status, the player faction's relations, the involvement set); worldEventState is the saved set (null while no
	// unique character has died or been touched); playerFactionSid is the first player-owned FACTION ("" = none).
	worldstate::Facts worldStateFacts() const;
	const state::WorldEventState* worldEventState() const;
	std::string playerFactionSid() const;
	// ---- the new-game advanced options (ADVANCED_OPTIONS_STATIC.md "Ours"; save v77, task BT) ----
	// The world's options when it has them (a NEW_GAME that carried options made the singleton, or a setGameOption); null
	// while it does not, which keeps the server's CombatSettings values. The saved ones are cod / gdm / ht; medicalClock is
	// the dev override, never saved.
	const state::WorldOptions* worldOptions() const;
	// What DEBUG_QUERY kind 2 and the report show: the effective cod / gdm / ht, the medical clock, and whether they are saved.
	std::vector<std::pair<std::string, std::string>> gameOptionRows() const;
	// DEBUG_SET_OPTION's body: key `cod` / `gdm` / `ht` (the world's options, each range-checked, out of range refused) or
	// `medicalClock` (the dev override). "" applied, else the reason. Makes the singleton on the first saved key; every
	// change is pushed into the combat arena's copy at once.
	std::string setGameOption(std::string_view key, double value);
	// The options applied to the combat arena (its CombatSettings copy): the singleton's cod / gdm / ht when there is one,
	// else the settings as they are, plus the clock override. Called by a new game, a load and every setGameOption.
	void applyGameOptions();
	// `combat.retarget_hold_s` (COMBAT_OPTIONS.md 6): the arena's copy and ours, seconds >= 0.
	void setRetargetHold(double seconds);
	double retargetHold() const noexcept { return combatSettings_.retargetHold; }
	// G5 (task BU): the sensor flags' world inputs (sensor.hpp): the live faction relation (factionRelation over the loaded data) and
	// the player faction's Id (0 = none). Built per call; the flags read it at the resolver.
	sensor::Context sensorContext() const;
	// The player's involvement hook for the cage / capture paths: the victim is a unique character the captor holds; recorded when
	// `captorSid` is the player's faction (the routine cage path is NPC-driven, so this is the seam a player capture will use).
	void noteNpcImprisoned(std::uint64_t victim, const std::string& captorSid);
	// DA_GIVE_TARGET_MY_SLAVES (24): every non-carried slave owned by `ownerSid` (Fighter::captorSid) is re-assigned to `target`'s
	// faction; the carried are dropped. False when the target or its side is unknown.
	bool giveSlavesTo(const std::string& ownerSid, std::uint64_t target);
	// DA_KNOCKOUT (58): the character's knockout timer is forced to at least `seconds` and it is unconscious. False for gone or dead.
	bool knockOut(std::uint64_t character, double seconds);
	// Row 228 STEALTH_KNOCKOUT (task ds/stealth-ko S4, stealth_ko.hpp, STEALTH_KO.md): `actor` walks up to `victim` and knocks it out if the roll
	// allows. A replacing order (a running walk, interact, queue and follow end). Answers "" or the reason an order 60 would: "bad_target"
	// (no such character, or itself), "not_allowed" (see CombatArena::orderStealthKo). The wire order is order 60 action 14 (checkStealthKo is its pre-check).
	std::string orderStealthKo(std::uint64_t actor, std::uint64_t victim);
	std::string checkStealthKo(std::uint64_t actor, std::uint64_t victim) const;
	// ---- jobs (jobs.hpp, jobs.cpp; SIMLINK JOBS 34, orders 76-78, toggle 10 which 4) ----
	// The reason a job order is refused, "" when it would apply: "unsupported" (kind 0 or no executor yet), "bad_value" (no such type),
	// "bad_target" (a targeted type with no valid target; no such jobId), "unknown_id", "not_player_controlled", "dead". Changes nothing.
	std::string checkJobOrder(const JobOrder& order) const;
	// A character's jobs list as the wire has it (an empty enabled list when it has none).
	JobsView jobsOf(std::uint64_t id) const;
	// ---- prospecting (prospect.hpp, prospect.cpp; SIMLINK order 79, PROSPECT 35) ----
	// The reason a prospect order is refused, "" when it would apply: "unknown_id", "not_player_controlled", "dead", "unsupported" (no
	// game data, heightmap or areas map), "not_allowed" (indoors, not on solid ground, carried, down, or in combat). Changes nothing.
	std::string checkProspect(std::uint64_t id) const;
	// The biome group at a point: its name and sid ("" when the point is in no group), and the first ENVIRONMENT_RESOURCES record of its
	// `resources` list (none = the group has no record: everything reads 0). nullopt without game data, heightmap or areas map.
	struct ProspectZone
	{
		std::string name;
		std::string sid;
		std::optional<prospect::Env> env;
	};
	std::optional<ProspectZone> prospectZone(double x, double z) const;
	// A zone-placed mine's richness (kenshi-rwh.298 / rwh.318; the machine panel's and the mining rate's one source): the stone noise at x,z x the
	// zone's `stone mult` for resource 2 (an iron / copper lookup reads 0), and a lookup of 0 gives 0.5. Other resources (water) keep 1.0.
	double placementRichness(double x, double z, std::int32_t resource) const;
	// What the results window shows for a prospector standing at (x, z) with that Science (null without the data). prospectNow reads the
	// character's own position and Science. Pure: changes nothing.
	std::shared_ptr<const prospect::Result> prospectAt(std::uint64_t id, double x, double z, double science) const;
	std::shared_ptr<const prospect::Result> prospectNow(std::uint64_t id) const;
	// Called from inside tick() for every finished prospect task, one callback per owner (the sessions). Observation only.
	void addProspectObserver(const void* owner, std::function<void(const std::shared_ptr<const prospect::Result>&)> observer);
	void removeProspectObserver(const void* owner);
	// ---- player buildings (buildings.hpp, buildings.cpp; BUILDINGS_SLICE.md Phases 1-2; SIMLINK orders 90, 91, 94, BUILDING 47) ----
	// The reason a placement (order 91, or the dry run 90) is refused, "" when it would apply: "no_faction", "locked", "bad_value",
	// "unsupported" (no game data or heightmap), "unsupported_site" (a record whose placement tests are not built, section 3),
	// "too_close_town", "characters_in_way", "invalid_ground", "not_level", "overlap". Changes nothing.
	std::string checkBuildPlace(const BuildOrder& order) const;
	// KSCEN `building` (scenario_file.cpp): a BUILDING of any record on the ground at x, z (y from the terrain), yaw about the vertical. A producer
	// with `built` is a finished producers.hpp entity; any other is a player building with the scenario placement (no unlock, town, overlap or
	// characters-in-way test), its materials already delivered, finished with `built`. Returns its Id; 0 with `reason` (the order 91 refusal text) when it cannot be placed.
	// KSCEN `task=` / `shackled=`: starts the task a player gives now ("pick_cage" / "cut_cage" on its own cage, "pick_shackles" / "cut_shackles" on
	// the wearer `target`, "operate" the producer `target`, "build" the site `target`), or "shackle" puts locked shackles on `actor`. "" = started,
	// else the refusal. The walk is not planned: the character stands at its work.
	std::string scenarioTask(std::uint64_t actor, const std::string& task, std::uint64_t target);
	std::uint64_t placeScenarioBuilding(const std::string& recordSid, double x, double z, double yaw, bool built, const std::string& faction, std::string& reason);
	// The town that refuses a site (the rule of buildings.hpp: the nearest town T, k 2.5 or 1.0, the 2000 floor, the narrow re-test for a record
	// whose `creates player town` is false; `faction` is the placing player faction's Id, whose own towns never block): the TOWN sid, or
	// "player:<id>" for another faction's player town; "" when nothing blocks. Pure.
	std::string townBlocker(double x, double z, bool createsPlayerTown, std::uint64_t faction) const;
	// The reason a pause order is refused, "": "bad_target" (no such building, not the player faction's, or already built).
	std::string checkBuildPause(std::uint64_t building) const;
	// Ka, the new-game `bs` knob (ADVANCED_OPTIONS_STATIC.md), clamped to 0.5..2; 1.0 until the options slice carries it. Configuration
	// like the combat settings (not saved): a loaded save needs the same value.
	double buildSpeed() const noexcept { return buildSpeed_; }
	void setBuildSpeed(double ka) noexcept;
	// The interface the hauler and the builders use (Phase 3): delivers up to `count` units of `item` to a site; returns the units
	// accepted (0 for a built site, an item it does not need, or one already complete). Between ticks or from a system.
	std::uint32_t addMaterials(std::uint64_t building, const std::string& item, std::uint32_t count);
	// Phase 2's stand-in for the Build task (Phase 3 recounts it from the characters): the builders working on a site (PlayerBuilding::workers);
	// their rates add with the 0.75^k damping (buildings.hpp builderFactor), no skill term yet. False for an unknown building.
	bool setBuilders(std::uint64_t building, std::uint16_t count);
	// What the BUILDING frame carries for one building, as the first player faction owns it, in Id order.
	struct BuildingView
	{
		std::uint64_t id = 0;
		std::string record;
		std::uint64_t owner = 0;
		std::uint64_t town = 0;
		double x = 0.0, y = 0.0, z = 0.0;
		double qw = 1.0, qx = 0.0, qy = 0.0, qz = 0.0;
		std::uint8_t floor = 0;
		std::uint8_t status = 0;
		std::uint32_t flags = 0;
		double progress = 0.0; // now
		double rate = 0.0; // per sim-second
		double hp = 0.0, maxHp = 1.0;
		std::vector<state::BuildMaterial> mats; // empty once built
		// A producer's panel values (producerView); a player building leaves them neutral (0 workers, quality 1) until its phases.
		std::uint8_t workers = 0, maxWorkers = 0;
		double efficiency = 0.0, maxEfficiency = 0.0;
		double quality = 1.0;
		double powerValue = 0.0; // the BUILDING's `power output` (negative = consumption); machines only
		std::uint32_t version = 0;
	};
	std::vector<BuildingView> playerBuildings() const;
	// The BUILDING row of a producer (any owner, owner 0 = wild) for the focus panel (SIMLINK Order 87); nullopt when `id` is no producer.
	std::optional<BuildingView> producerView(std::uint64_t id) const;
	// The BUILDING row (SIMLINK "BUILDING", kind 4 / 12 / 3 join) of a placed machine, any owner: a producer's own view while its entity
	// exists, else built from the data (workers 0). `mounted` = a turret somebody mans. nullopt: a producer with no entity yet.
	/// Machine id -> members holding a Use order on it (a converter's workers on its BUILDING row, k16.6.839).
	std::unordered_map<std::uint64_t, std::uint32_t> machineOperators() const;
	std::optional<BuildingView> machineView(const MachineSite& site, int workers) const;
	// The BUILDING row of a placed container (World ContainerSite; INTERACTABLE kind 3, id = the entity's Id), any owner; nullopt when `id` is none.
	std::optional<BuildingView> containerView(std::uint64_t id) const;
	// The machine record of every placed machine BUILDING (machines.hpp), in placement order; the inspect panel's fields (kenshi-gzto).
	std::vector<machines::Record> machineRecords() const;
	// Construction hours of a placed building at the current Ka (buildings.hpp buildHours); 0 for an unknown one.
	double buildHoursOf(std::uint64_t building) const;
	// The ground height under a point (the full map's terrain), nullopt without game data or the heightmap.
	std::optional<double> groundHeight(double x, double z) const;
	// ---- new game (newgame.hpp, newgame.cpp; SIMLINK NEW_GAME 111 / STARTS 33 / order 84) ----
	// The NEW_GAME_STARTOFF records of the game data, read on first use; null without game data.
	const StartCatalog* startCatalog() const;
	// Every start-off in record order and whether a NEW_GAME with it would be placed now: one of its places has walkable ground on
	// the navigation mesh within the click reach. Without navigation nothing is ready.
	struct StartReady
	{
		struct Member
		{
			std::string race; // RACE sid
			std::uint8_t female = 0;
			std::string name;
			std::uint8_t flags = 0; // bit 0 editable
			std::string character; // the CHARACTER sid the member was expanded from
			std::vector<std::string> armour; // the ARMOUR sids it wears at NEW_GAME, equip order
			std::string weapon; // the WEAPON sid it carries ("" = none)
			bool operator==(const Member&) const = default;
		};
		std::string sid;
		bool ready = false;
		std::vector<std::string> races; // `force race`, narrowed to the creator's races; empty = any creator race
		std::vector<Member> members; // startSquad, leader first
		bool operator==(const StartReady&) const = default;
	};
	// The start-off's squad as the sim expands it: the members of its `squad` refs with the ranges, `choosefrom list` and races drawn from
	// the server seed and the start-off's sid alone (kStartSquadStream), so STARTS and NEW_GAME see the same squad. The one expansion.
	std::vector<StartMember> startSquad(const StartOff& start) const;
	std::vector<StartReady> startStatuses() const;
	// The reason a NEW_GAME is refused, "" when it would apply: "unsupported" (no combat, game data or Nameless faction),
	// "unknown_startoff", "has_faction" (a player faction exists), "bad_name", "squad_size" (specs != 0 and not startSquad's size), "not_ready", then the Specs against the character creator (CreatorCatalog::check: "unknown_race",
	// "race_not_playable", "race_forced", "sex_not_available", "bad_head", "bad_hair", "bad_beard", "bad_hair_colour", "bad_slider",
	// "slider_range", "empty_name", "bad_character_name"). Changes nothing.
	std::string checkNewGame(const std::string& startoff, const std::string& name, const std::vector<CharacterSpec>& specs) const;
	// What the character creator offers, read on first use; null without game data.
	const CreatorCatalog* creatorCatalog() const;
	// Called from inside tick() for every applied NewGame command, one callback per owner (the sessions). Observation only.
	void addNewGameObserver(const void* owner, std::function<void(const NewGameOutcome&)> observer);
	void removeNewGameObserver(const void* owner);
	// The fresh world (SIMLINK order 84): markPristine remembers the instances as they are now (the world a Simulation::newGame, or a
	// server with the bare templates, starts with: no player faction, no scenario), newWorld puts them back in place (reload: the
	// clock, scheduler, seed and every instance; combat, navigation, AI and observers stay). Between ticks. newWorld throws
	// std::logic_error without a mark.
	void markPristine();
	bool hasPristine() const noexcept { return pristine_ != nullptr; }
	void newWorld();
	// The first player faction's squads as the PLAYER_SQUADS record has them; faction 0 = no player faction.
	PlayerSquadsView playerSquads() const;
	// The first player faction's completed RESEARCH sids (SIMLINK UNLOCKS 36); empty without a player faction.
	std::vector<std::string> completedResearch() const;
	// The first player faction's research state (research.hpp, CRAFTING_SLICE 2.3, 9.2): tech level, each record's state, the unlocked sets. Derived from
	// completedResearch() and the data, never saved; recomputed when the list or the game data changes. Empty without game data.
	const research::Snapshot& researchState() const;
	// The BUILDING sids the research enables: the union of their `enable buildings`, deduplicated, in BUILDING record order,
	// REMOVED records skipped. Empty without game data.
	std::vector<std::string> unlockedBuildings(const std::vector<std::string>& research) const;
	// The first player faction's discovered places (SIMLINK DISCOVERED 37) in TOWN record order; empty without a player faction.
	struct DiscoveredPlace
	{
		std::string sid;
		std::int32_t type = 0; // the TOWN record's `type`
		bool visited = false;
		bool operator==(const DiscoveredPlace&) const = default;
	};
	std::vector<DiscoveredPlace> discoveredPlaces() const;
	// The first player faction's visited towns for the map tooltip (SIMLINK TOWN_STATE 66), in TOWN record order (town_state.cpp). `flags`:
	// bit 0 known, 1 dead, 2 isTownClass; `live` bit n = value n is live (SIMLINK.md); the counts of a clear bit are 0.
	struct TownStateRow
	{
		std::string town;
		std::string owner; // the live owner's FACTION sid, empty = none
		std::uint8_t flags = 0;
		std::uint16_t live = 0;
		std::int32_t residentsActual = 0;
		std::int32_t roamingPop = 0;
		std::int32_t roamingSquads = 0;
		std::vector<std::string> residentFactions;
		std::vector<std::string> hostile;
		bool operator==(const TownStateRow&) const = default;
	};
	std::vector<TownStateRow> townStates() const;
	static constexpr std::uint64_t kTownStateIntervalTicks = 20; // checked once per sim-second
	static constexpr std::uint64_t kTownStateMinGapTicks = 100; // at most one frame per 5 sim-seconds per session
	// The timed tasks of characters (SIMLINK TASK_PROGRESS 39), by character id. progress 0..1 now, rate per sim-second, value = the
	// lockpick chance, the cut's remaining hardness (else 0), `target` the id of what is worked on (0 = none). Every player character's
	// tasks, kinds 1-16; `everyone` adds the NPCs' (the session's level 3). A row's `flags` say whether the original shows its bar.
	static constexpr std::uint8_t kProgressOriginal = 1; // flags bit 0: the original shows this bar
	static constexpr std::uint8_t kProgressPlayer = 2; // flags bit 1: the character is the player faction's
	struct TaskProgressRow
	{
		std::uint64_t id = 0;
		std::uint8_t kind = 0; // 1 lockpick, 2 unlock, 3 lock, 4 prospect, 5 add materials, 7 cut, 8 restrain, 9 operate, 10 produce, 11 build, 12 repair, 13 dismantle, 14 process, 16 first aid (6 and 15 are not sent)
		float progress = 0.0f;
		float rate = 0.0f;
		float value = 0.0f;
		std::uint8_t flags = 0;
		std::uint64_t target = 0;
	};
	std::vector<TaskProgressRow> taskProgress(bool everyone = false) const;
	// The bar of an actor's construction order (add materials, build, repair, dismantle: buildings.cpp) or lock task (a cage's cut or
	// pick, or a pick / cut of worn shackles: lock_tasks.cpp), when it is working now.
	bool constructionProgress(std::uint64_t id, TaskProgressRow& row) const;
	bool lockProgress(std::uint64_t id, TaskProgressRow& row) const;
	// A producer's progress per sim-second from its operators now (TASK_PROGRESS kind 9 rate).
	double producerRate(const state::Producer& producer) const;
	// The operate clip and start tick of a character that is operating a producer now (CHAR_DYNAMIC action), else nothing.
	struct OperateAction
	{
		std::string clip;
		std::uint64_t start = 0;
		std::uint8_t skill = 0xFF; // the skill id (SIMLINK.md row 8) the producer's `stat used` trains
	};
	// Whether the fighter is in combat mode (CHAR_DYNAMIC flag bit 0, and CHAR_STATS row 6 kind 5): the one test for both.
	bool combatMode(const state::Fighter& fighter) const;
	std::optional<OperateAction> operateAction(std::uint64_t id) const;
	// The clip and start tick of a capturer at work now (CHAR_DYNAMIC action, rwh.114): "medic" through the strip 181 and the cuff 166 (one kneel; the clip
	// loops), "shoulder lift left" through a lift (plays once). Else nothing. `loop` is the actionFlags bit 0.
	struct CaptureAction
	{
		std::string clip;
		std::uint64_t start = 0;
		bool loop = false;
	};
	std::optional<CaptureAction> captureAction(std::uint64_t id) const;
	// The wild deposits (foliage meshes with a `building type` that produce) of a zone cell, generated on first use. For tests and tools.
	const std::vector<ProducerSite>& wildProducerSites(int cellX, int cellZ) { return foliageProducers(cellX, cellZ); }
	// The first player faction's relations (SIMLINK FACTION_RELATIONS 38, kinspect 49): one row per listed faction, case-insensitively
	// by display name (ties shorter first); `value` is THAT faction's relation toward the player. Empty without a player faction. Which
	// factions are listed is ONE function, listsRelation (player_squads.cpp).
	struct FactionRelationRow
	{
		std::string sid;
		std::string name;
		double value = 0.0;
	};
	std::vector<FactionRelationRow> factionRelations() const;
	static constexpr std::uint64_t kFactionRelationsIntervalTicks = 20; // checked once per sim-second
	static constexpr double kFactionRelationsStep = 0.5; // a value must move this far from the last send to be resent
	// Discovery runs once per sim-second (the exe's accumulated-dt check, map24_static.md section 5).
	static constexpr std::uint64_t kDiscoveryIntervalTicks = 20;
	// The discovery radius of a TOWN `type`: 17000 for 2, 4250 for 0 and 4, 8500 otherwise; 0 for 8 and 10 (never discovered).
	static double discoveryRadius(std::int32_t type);
	// character Id -> Id of its squad: the player squads, and the squad a dismissed character joined (MemberOf).
	const std::unordered_map<std::uint64_t, std::uint64_t>& characterSquads() const; // cached: valid until the next squad write (squad_caches.hpp)
	// Called from inside tick() for every dismissal (order 75). Observation only.
	void setDismissObserver(std::function<void(const DismissEvent&)> observer) { dismissObserver_ = std::move(observer); }
	// The inventory of an owner (state::Id of a character or container) as it is once grid::settle has run on it: positions,
	// stack caps and materials filled in. A copy; the session sends this. Empty inventory when it has none.
	state::Inventory settledInventory(std::uint64_t ownerId) const;
	// The numbers an item entry fights with (SIMLINK.md ITEM_STATS, order 69): the weapon / armour as CombatData::buildWeapon /
	// buildArmour make it from the entry's quality (what equipping rebuilds), plus the record reads the tooltip needs. kind 0 = neither
	// weapon nor armour, or an instance that cannot be built (no quality, an unknown maker / model). Read-only.
	struct ItemStats
	{
		std::uint8_t kind = 0; // 0 none, 1 armour, 2 weapon
		state::ItemQuality quality; // the entry's own (the common block's grade / level)
		state::FighterArmour armour;
		state::FighterWeapon weapon;
		float armourWeight = 0.0f; // ARMOUR `weight kg`
		std::uint8_t armourClass = 0;
		std::uint8_t material = 0;
		float stealthMult = 1.0f;
		float athleticsMult = 1.0f;
		float dexterityMult = 1.0f;
		float value = 0.0f; // the record's `value`, per unit
	};
	ItemStats itemStats(const state::Inventory::Entry& entry) const;
	// The character panels of one fighter (SIMLINK.md CHAR_STATS, type 49; char_stats.cpp). Read-only; a missing fighter = defaults.
	struct CharStats
	{
		struct Skill
		{
			std::uint8_t id = 0; // SIMLINK.md "Skill ids"
			float value = 0.0f;
			float bonus = 0.0f;
			bool operator==(const Skill&) const = default;
		};
		std::uint8_t state = 0; // 0 Normal, 1 Defensive, 2 Sleeping, 3 Unconscious, 4 Dead, 5 Locked, 6 Unlocked
		std::uint8_t lockLevel = 0;
		std::string goal; // "" = Aimless
		float encumbrance = 0.0f; // percent
		std::uint8_t extraKind = 0; // 0 none, 1 Stealth, 2 Athletics, 3 job skill, 4 ranged (never), 5 melee weapon, 6 swimming (never: no swim state)
		std::uint8_t extraSkill = 0xFF; // the row-8 skill id of the kind, 0xFF = none
		float extra = 0.0f;
		std::uint8_t weaponClass = 0; // 0 martial arts, 1 attack + defence, 2 crossbows
		std::vector<Skill> skills;
		float athleticsXp = 0.0f;
		float strengthXp = 0.0f;
		float toughnessXp = 0.0f;
		float koPoint = 0.0f;
		float runSpeed = 0.0f;
		float runPenalty = 0.0f;
		float hungerRate = 0.0f;
		float light = 0.0f;
		std::vector<std::pair<std::string, std::int32_t>> bounties; // FACTION sid, amount
		float sentenceHours = 0.0f;
		// The AI tail (spectator mode, SIMLINK.md rows 16-20): the enum behind `goal`, where it came from, the plan step running and the
		// thing it is about. Additive after row 15, no protocol bump.
		std::uint16_t goalEnum = 0xFFFF; // the AI_TASK enum behind row 4; 0xFFFF = none
		std::uint8_t source = 0; // 0 none, 1 player order, 2 package / job row, 3 routine slot, 4 group AI, 5 combat, 6 carry chain, 7 flee
		std::uint16_t stepEnum = 0xFFFF; // the G4 plan step being executed
		std::uint8_t stepIndex = 0;
		std::uint8_t stepCount = 0;
		std::uint64_t targetId = 0; // a character, else a building / site object, else 0
		bool hasTargetPoint = false; // false = the wire's NaN pair (a real float member keeps the change test exact: NaN != NaN)
		float targetX = 0.0f;
		float targetZ = 0.0f;
		// The stats window tail (rows 21-23, kenshi-k16.6.54): the Next Level bar per row-8 skill, the pack weight against the capacity,
		// and the toughness damage resistance as a fraction.
		std::vector<std::pair<std::uint8_t, float>> progress;
		float carryKg = 0.0f;
		float carryMaxKg = 0.0f;
		float damageResist = 0.0f;
		// The HUD tag row tail (rows 24-32, kenshi-k16.6.215): additive after row 23, no protocol bump.
		struct BountyTag
		{
			std::uint32_t crimes = 0; // CrimeEnum bits
			std::uint8_t claim = 0;
			float hoursLeft = -1.0f; // -1 = never
			bool operator==(const BountyTag&) const = default;
		};
		std::uint16_t tagFlags = 0; // b0 squad leader, b2 captive, b4 slaveSeen (only with slaveLook != 255), b6 obedient; b1 / b3 / b5 unmodelled (0)
		std::uint8_t slaveState = 0;
		std::string captor; // FACTION sid, "" = none
		float slaveHoursLeft = -1.0f;
		std::uint8_t slaveLook = 255; // not modelled
		std::uint8_t crime = 0; // CrimeEnum, 0 = none
		float crimeTimer = 0.0f;
		float escapeHoursLeft = -1.0f; // not modelled
		std::string localFaction; // the town owner at the character's position, "" = none
		std::vector<BountyTag> bountyTags; // one per row-14 entry, same order
		// The engage trigger (row 34, k16.5.37): additive after row 32, no protocol bump. See Fighter::engageBasis.
		std::uint8_t engageBasis = 0; // 0 none; 1 player order; 2 attacked by the target; 3 ally / squad attacked; 4 faction / team enemy; 5 personal hostile entry; 6 troublemaker; 7 other package goal; 8 selector commit; 9 counter-flip
		std::uint8_t engageSense = 0; // 0 n/a; 1 sight; 2 hearing; 3 remembered record; 4 shot from unsensed
		std::uint64_t engageCause = 0; // the character whose attack / fight pulled this one in (2: the attacker, 3: the attacked ally, 9: the asker), else 0
		std::uint64_t engageTick = 0; // the tick the current target was set, 0 = no fight
		bool operator==(const CharStats&) const = default;
	};
	CharStats charStats(std::uint64_t id) const;
	// The stat the body's bulk reads (CHAR_APPEARANCE "muscle", rwh.240 section 3): max(strength, weapon smith, armour smith); 1 if unknown.
	double bodyBulkStat(std::uint64_t id) const;
	// The body's size-muscle m (BODY_SHAPE.md; CHAR_APPEARANCE "muscle", unclamped) and skinny (CHAR_APPEARANCE "skinny", 1/20 steps) from hunger.
	double bodyMuscle(std::uint64_t id) const;
	static double bodySkinny(double hunger);
	// Called from inside tick() for every pick-up and finished or failed interaction. Observation only.
	void setInteractObserver(std::function<void(const InteractEvent&)> observer) { interactObserver_ = std::move(observer); }
	// The tick until which a world task / Use order keeps (member, target) blocked (addBlockedTarget, 20 s); 0 = no block. For tests.
	std::uint64_t worldTaskBlockUntil(std::uint64_t member, std::uint64_t target) const
	{
		const auto found = worldTaskBlocks_.find({member, target});
		return found == worldTaskBlocks_.end() ? 0 : found->second;
	}
	// A crime the player should hear of (SIMLINK events 51 and 52). Called from inside tick(). Observation only.
	struct CrimeEvent
	{
		enum class Kind : std::uint8_t
		{
			CaughtStealing, // `who` (thief) was seen by `other` (the first witness of the record); cause 1 take, 2 lockpick; container id
			AlarmRaised, // `who` raised an alarm of crime `cause` set off by `other` (the criminal) for `faction`, at x, z
		};
		Kind kind = Kind::CaughtStealing;
		std::uint64_t who = 0;
		std::uint64_t other = 0;
		std::uint8_t cause = 0;
		std::uint64_t container = 0;
		std::string faction;
		double x = 0.0;
		double z = 0.0;
	};
	void setCrimeObserver(std::function<void(const CrimeEvent&)> observer) { crimeObserver_ = std::move(observer); }
	// Called from inside tick() for every combat event (replaces the arena's observer; survives
	// enableCombat). Observation only: it must not touch the simulation.
	void setCombatObserver(std::function<void(const CombatEvent&)> observer);
	// The world-squad log (world_squad_log.cpp, server --log-world-squads): every `everyGameMinutes` one line per built world squad (id, template,
	// faction, team, hostile, members, leader, token, goal) to `sink`, and one line at each promote / demote. An empty sink or 0 turns it off.
	void setWorldSquadLog(std::uint64_t everyGameMinutes, std::function<void(const std::string&)> sink);
	// Task dt27 (176 HUNTING_BLOODSMELL): the platoon Blackboard's smell position (Blackboard+0xf8 -> the first ZoneSmell, dt27 section 4.2).
	// The exe's writer is unknown (U); this is the hook a smell system calls. `clear` withdraws it. A squad with no built Blackboard
	// yet gets one. Derived state, never saved. False for an entity that is not a world squad.
	bool setSquadSmell(flecs::entity squad, double x, double z);
	void clearSquadSmell(flecs::entity squad);
	// The world task log (ksim_server, tasks BR2 / slaver-crime, so a live run can confirm what the client cannot see): one line
	// when a member starts and ends the capture chain (167, from world_tasks), at each phase of stepSlaveTrade - the strip 181, the
	// restrain 166, the cuff and lift 168 - the 112 put-in (member, body, cage, the reach that allowed it), when a woken captive
	// starts following its captor (171), and one line per item a LOOT_THE_DEAD 177 take moves off a body (the item, the count, the
	// body, the taker). Observation only; never saved.
	void setTaskLog(std::function<void(const std::string&)> sink) { taskLog_ = std::move(sink); }
	// The player-order route trace (--ai-routes, kenshi-k16.6.821): the approach point, the route points and the end reason of a player's
	// interact and move orders, as task-log lines "route: ...".
	void setRouteTrace(bool on) { routeTrace_ = on; }
	bool routeTrace() const { return routeTrace_; }
	void logTask(const std::string& line) const
	{
		if (taskLog_)
			taskLog_(line);
	}

	// The AI decision trace (decision_trace.hpp, kenshi-k16.6.43): while a sink with a `record` is set, each goalsel pick of a member it
	// `wants` is reported with its candidates and verdicts. Observation only; never saved. A default Sink turns it off.
	void setDecisionTrace(trace::Sink sink)
	{
		decisionTrace_ = std::move(sink);
		installFightTrace();
	}
	bool tracing(std::uint64_t member) const { return decisionTrace_.record && (!decisionTrace_.wants || decisionTrace_.wants(member)); }

	// The wilderness spawner (null for a world without density-spawned
	// wilderness). Runs at the Strategic interval, before the AI.
	const WildernessSpawner* wilderness() const noexcept { return wild_.get(); }
	// Test / scenario hook: a homeless wilderness squad of `templateSid` with its den at (x, z), as a drawn one (see
	// WildernessSpawner::placeHomeless); `members` 0 = the template's own size. Between ticks. Throws without a wilderness spawner.
	flecs::entity placeWildernessSquad(std::string_view templateSid, double x, double z, std::size_t members = 0);
	// Test hook: a town squad of `templateSid` at (x, z) the way a town's `bar squads` entry spawns one (spawnTownSquads: the
	// template's own faction; SpawnSource::TownBar; no order, no den, no raid). Between ticks. Throws for an unknown template.
	flecs::entity placeTownSquad(std::string_view templateSid, double x, double z);
	// The debug view's DEBUG_SPAWN_SQUAD (SIMLINK.md "DEBUG VIEW"), between ticks: a wilderness squad of `templateSid` ("" =
	// 1302-gamedata.base) at (sx, sz), with the order the command asks for. A town within 2,000 u of (tx, tz) gives the squad
	// the raid order its own faction planner would (ai.cpp raid); otherwise it walks to (tx, tz) and then runs its own AI.
	// `squadId` 0 is a refusal, with the reason: an unknown template, no navmesh at the spawn, or the spawner's own error.
	struct DebugSpawn
	{
		std::uint64_t squadId = 0;
		std::string reason;
	};
	DebugSpawn debugSpawnSquad(std::string_view templateSid, double sx, double sz, double tx, double tz);
	// Test hook, between ticks: the squad's token to (x, z), through the regions (which keep their own copy of the position, so setting Position
	// alone leaves the squad in its old region and detail); a homeless squad's den moves with it (the AI leashes it there).
	void moveSquad(flecs::entity squad, double x, double z);
	// Scenario setup (scenario_file.hpp), between ticks: removes every world squad (SquadInstance with its members) whose position is within `radius` of
	// (x, z) and tells the regions; returns how many. The area the scenario file describes replaces what the default world put there.
	// `keep`: squads (entity ids) that stay (the ones scenarioSquad claimed).
	std::size_t removeWorldSquadsNear(double x, double z, double radius, const std::set<flecs::entity_t>& keep = {});
	// The scenario's hold (task BM): after the file's own removal, every world squad that APPEARS inside the area later - a wilderness
	// den's camp drawn at the next strategic step, a town population on a later load, a faction's raiders - is removed at the first tick
	// it stands there, and so is one whose PEOPLE stand there (a town squad's token stays at its town: the Hub's bar crowd materialised at
	// flat1's spot). `keep` is what the file owns (scenarioSquad's claimed and new squads). The area is the file's centre and radius; a
	// camp that lands inside it would push the file's fighters apart. A world that never calls this is untouched, and the hold ends with
	// the world (reload clears it).
	void holdScenarioArea(double x, double z, double radius, const std::set<flecs::entity_t>& keep);
	// Scenario setup: the world squad that stands for one squad of a scenario file, so its characters run the normal world AI (residents' routines,
	// perception, the squad AI), the same path as a new game's squads. A world squad of the same template inside the area (centre `areaX`, `areaZ`,
	// `radius`; nearest to (x, z), not in `taken`) is CLAIMED: it keeps its town, source, den and traders; its characters are deleted and (x, z)
	// becomes its position (a town squad's token stays at its town). With none, a new SquadInstance is made (the faction by sid; a squad that is not
	// `hostile` and stands within 2500 u of a town is that town's resident, else a homeless wanderer with its den at (x, z): UNSURE, the file does not
	// say where a squad lives). The caller adds the characters with scenarioMember and then calls removeWorldSquadsNear with the claimed set.
	flecs::entity scenarioSquad(std::string_view templateSid, std::string_view factionSid, double x, double z, double areaX, double areaZ, double radius,
		bool hostile, const std::set<flecs::entity_t>& taken, bool* claimed = nullptr);
	// A character of such a squad: a CharacterInstance member entity holding `fighter` (adoptFighter). `leader` = MemberRole leader. Its position stays
	// what the file says (no ring, no routine placement); a town resident gets its Routine at the next resident pass (promoteResidents).
	flecs::entity scenarioMember(flecs::entity squad, const std::string& characterSid, bool leader, state::Fighter fighter);
	// Original-save import (save_import.hpp, kenshi-cym.2), between ticks. importSquad makes a squad token of `instance` at `at`. importMember adds the
	// member of its slot `slot` (the template dice key): the template's Fighter is built as buildMember would, `apply` turns it into the saved character,
	// and the difference is stored as the MemberDelta (nothing when none), the pack as the Inventory. A dead member becomes a body at (`deadX`, `deadZ`)
	// at once (leaveCorpse; no navmesh needed). Nullopt (nothing made) when the template cannot be built. importPromote makes the squad Full detail
	// (promoteWorldSquad) and stands each living member at its saved spot, snapped to the navmesh within 100 u when `snap`; a member with no ground near
	// keeps the spot it was built at and is counted in the return. importDone tells the regions and the AI about the new squads.
	// The faction instance of that FACTION sid (made, as scenarioSquad does, when the world has none); the town of that TOWN record name (none: invalid).
	// A name can be a record twice (two "Old Village" towns): with `near` the one nearest that point, else the first.
	state::EntityRef importFaction(const std::string& factionSid);
	state::EntityRef importTown(const std::string& townName, std::optional<state::Position> near = std::nullopt);
	// KSCEN `town` / `raid` (scenario_file.hpp): the TOWN record's town nearest (x, z) (the file's centre) changes hands to the player faction (false: no
	// such town or no player faction).
	// scenarioRaid gives each squad of `squads` the order UtilityFactionLayer::commission writes (Issued against `townName`, its own position as the
	// origin, `stayTicks` / `retreatTicks`, the campaign) and records the raid in `factionSid`'s FactionAI, so the group layer marches the force
	// and the faction layer follows it. False (nothing changed) when the town, faction or a squad is missing.
	bool scenarioTownToPlayer(const std::string& townName, double x, double z);
	bool scenarioRaid(const std::string& factionSid, const std::string& campaignSid, const std::string& townName, double x, double z,
		const std::vector<flecs::entity>& squads, std::uint64_t stayTicks, std::uint64_t retreatTicks);
	// A token squad of a SQUAD_TEMPLATE, its members drawn from the template as the default world's spawner does (SquadBuilder) with `seed`: the platoon
	// records of an original save that were never activated have no content file. Invalid (nothing made) when the sid is no squad template.
	flecs::entity importTemplateSquad(const std::string& squadTemplateSid, state::EntityRef faction, state::EntityRef homeTown, state::SpawnSource source, double x, double z, std::uint64_t seed, std::size_t* members = nullptr);
	flecs::entity importSquad(const state::SquadInstance& instance, state::Position at);
	// Before the first importSquad: the world's own squads go (members, not their bodies) and the wilderness spawner is off, so the save's platoons are the
	// whole population (the original's wilderness is in them; nothing tops it up). Returns the squads removed.
	std::size_t importClearSquads();
	// `apply` gets the template's Fighter and an empty pack (the carried items go there; the worn weapon and armour are added to it after).
	std::optional<flecs::entity> importMember(flecs::entity squad, const std::string& characterSid, bool leader, std::size_t slot,
		const std::function<void(state::Fighter&, state::Inventory&)>& apply, double deadX, double deadZ);
	struct ImportSpot
	{
		flecs::entity member;
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
	};
	std::size_t importPromote(flecs::entity squad, const std::vector<ImportSpot>& spots, bool snap);
	// An order names a character that is a member of a token squad (no Fighter yet; an imported hostile the pass has not built, kenshi-k16.6.679):
	// its squad is built now, as the pass would (residents or world squad), so the order can reach it. True when the id has a Fighter afterwards;
	// false for an unknown id, a corpse-less dead one, or a squad whose ground is not on the loaded mesh (the streaming brings it first).
	bool promoteForOrder(std::uint64_t id);
	// A character that exists in the world and is not dead (order 52 target check): in the arena by its Fighter, else by its CharacterInstance. Promotes nothing.
	bool liveCharacter(std::uint64_t id);
	void importDone();
	// Stage 3a, the world-wide state (save_import.hpp), between ticks; each lands where our own save keeps it and queues no event (a load is not a conquest).
	// importTownIndex: the TownState of every 0-townlist placement of the level, by placement instance id; a placement with no TownState of ours (a town
	// record's later placements, the player's town) is absent. importTownOwner / importProsperity (`prosperity` is the original's, 1000 = full: the loss
	// below the faction's cap, never negative) / importRelation (from's opinion of to). importPlayerFaction: money and the completed RESEARCH sids of the first
	// player faction; false without one. importCrimes: a CrimeRecord on each member entity by Id (a token member too: the crime step reads any holder);
	// returns how many were set. importWeather: for each slot the region of the one BIOME_GROUP whose seasons hold `season` (a slot's group is not saved), set to
	// the saved weather and times; `group` of the result is "" with `why` when the slot is left (no season, no group, several groups hold it, an unknown weather).
	std::map<std::string, state::EntityRef> importTownIndex();
	void importTownOwner(state::EntityRef town, state::EntityRef owner);
	void importProsperity(state::EntityRef faction, double prosperity);
	void importRelation(state::EntityRef from, state::EntityRef to, double value);
	bool importPlayerFaction(std::int64_t money, const std::vector<std::string>& research);
	// kenshi-cym.14: a player member's saved orders. `jobs`: persistent rows (type, target id) added as the player's job order does (the exe's addTask rules:
	// a type the jobs list cannot hold adds nothing; returns how many rows were added over all); `moves`: one-shot move orders queued as a right-click queued
	// move is, started on the first tick that finds the member free. Between ticks, after the member is spawned.
	std::size_t importOrders(std::uint64_t id, const std::vector<std::pair<std::uint16_t, std::uint64_t>>& jobs, const std::vector<std::array<double, 3>>& moves);
	std::size_t importCrimes(const std::map<std::uint64_t, state::CrimeRecord>& records);
	// Stage 3b (kenshi-cym.6, save v105). importPlacements: every townlist placement of the level by instance id (its TOWN record and place), the answer for a
	// town record that has no TownState of ours. importTownInfo: the town's flags, and `replacement` (a TOWN sid, "" = none) made its active variant
	// (`activeSid`; the bars and the town's records derive from it at the next look, so the rebuild is the existing cache key); false when the replacement is
	// no TOWN record. importExtraTown: an ExtraTown entity for a record with no TownState. importFactionStanding: rank, global trust and the trust rows.
	// importUniques: WorldEventState::used.
	struct ImportPlacement
	{
		std::string rootSid;
		double x = 0.0;
		double z = 0.0;
	};
	std::map<std::string, ImportPlacement> importPlacements();
	// A placed building the loaded zones lack (the save's runtime-spawned Peeler Machine, kenshi-cym.28): World::addFurnitureCage. Before the first AI tick.
	bool placeFurnitureCage(const kworld::PlacedInstance& instance);
	// A building the zone files do not carry stands in the world (kenshi-k16.7.125): the placement, its door and building sites (World::addPlacedBuilding)
	// and, on a native mesh, its collision and a patch bake of the tiles it covers (MeshNavigator::standBuilding). One path for a save's own building and
	// for a construction. `placed` needs buildingID, instanceID, x, y (world), z and the quaternion; the cell and scale are filled here.
	struct Stood
	{
		std::size_t placed = 0; // in the world's placements
		std::size_t nav = 0; // with collision and a patch bake done
		std::size_t navTiles = 0; // tiles replaced
	};
	Stood standBuilding(kworld::PlacedInstance placed) { return standBuildings({std::move(placed)}); }
	// Several at once (one zone bake per zone, not per building).
	Stood standBuildings(std::vector<kworld::PlacedInstance> placed);
	// Stands every PlayerBuilding with a `placement` the world lacks (a load: the placements are derived, the buildings are saved; kenshi-k16.7.156). Returns the count stood. `navOnly`: a new navigator over a world that already holds the placements (enableNavigation).
	std::size_t restandBuildings(bool navOnly = false);
	// A save's own player building (import): the PlayerBuilding entity for a placement `standBuildings` stood, from its GAMESTATE_BUILDING state. Returns its Id (0 = unknown record).
	struct ImportBuildingState
	{
		bool complete = true, destroyed = false, broken = false, paused = false, powerOn = true;
		double progress = 1.0;
	};
	std::uint64_t importPlayerBuilding(const kworld::PlacedInstance& placed, std::uint64_t ownerFaction, const ImportBuildingState& state);
	// The PlayerTown of a save's town instance: `buildings` (importPlayerBuilding Ids, one town hand) become its members and get `town` set; centre and radius from them. Returns the town Id (0 = no buildings).
	std::uint64_t importPlayerTown(std::uint64_t ownerFaction, const std::string& name, const std::vector<std::uint64_t>& buildings);
	bool importTownInfo(state::EntityRef town, const state::TownInfo& info, const std::string& replacement);
	state::EntityRef importExtraTown(const state::ExtraTown& town);
	void importFactionStanding(state::EntityRef faction, std::int32_t rank, double globalTrust, std::vector<state::FactionState::Trust> trust);
	void importUniques(std::vector<state::WorldEventState::Used> used);
	struct ImportWeather
	{
		std::string season;
		std::string weather;
		double strength = 0.0;
		double effectStrength = 0.0;
		std::uint64_t startMinute = 0;
		std::uint64_t endMinute = 0;
		std::uint64_t seasonEndDay = 0;
	};
	struct ImportWeatherResult
	{
		std::string group;
		std::string why;
	};
	std::vector<ImportWeatherResult> importWeather(const std::vector<ImportWeather>& slots);
	// The team a world fighter of this faction gets (worldTeam, residents.cpp): 0 without a player faction or when the faction is not hostile to it.
	std::int32_t teamOfFaction(const std::string& factionSid);
	// Makes `weatherSid` the current weather of the BIOME_GROUP `groupSid` now (WeatherSystem::pin). False when weather is off or either sid is unknown.
	bool pinWeather(std::string_view groupSid, std::string_view weatherSid);
	// The debug view's DEBUG_SET_WEATHER (SIMLINK.md "DEBUG VIEW"), between ticks: `groupSid` "" is the group under the first
	// player character, "*" every group with a region; `weatherSid` "" releases the pin. The pin holds past season ends and
	// lifetimes. "" when applied, else the DEBUG_ACK reason.
	std::string debugSetWeather(std::string_view groupSid, std::string_view weatherSid, double strength);
	// The debug view's DEBUG_SET_TIME: forward to the next time the clock reads `hour` (0 <= hour < 24; today if still ahead,
	// else tomorrow) plus `extraDays` (0..7). "" when applied, else the DEBUG_ACK reason.
	std::string debugSetTime(double hour, unsigned extraDays);
	// The debug view's DEBUG_CHEAT (SIMLINK.md "DEBUG VIEW"), between ticks, on one character (0 = the first player character): op 0 heal
	// (parts to max, blood full, bleeding stopped, KO cleared, up; chains and cages kept), 1 knock out (the KO timer a damage KO sets; the
	// arena's next update does the rest, so the Knockout event and the ragdoll come as for a KO with no hit), 2 wake (KO cleared, wounds
	// kept), 3 kill (blood below its death line: the medical update kills it). Nothing is saved. "" when applied, else the DEBUG_ACK reason.
	std::string debugCheat(std::uint64_t character, unsigned op);

	// The weather cycle (weather.hpp): null in a world built with Simulation::load until enableWeather. newGame turns it on;
	// a loaded save with weather regions turns it on when the game data is known (restore).
	const WeatherSystem* weather() const noexcept { return weather_.get(); }
	const BeamField* beams() const noexcept { return beams_.get(); }
	// Turns the weather on from the merged game data: creates the regions when the world has none, otherwise reconciles the
	// loaded ones with the data. Between ticks; nothing if already on. Needs the data directory for the areas map.
	void enableWeather(const kdata::GameData& gameData);

	// Stamps command.tick (next tick at the earliest) and queues it.
	void enqueue(Command command);
	// Two factions at war: relation -100 both ways, queued like enqueue.
	void setWar(state::Id a, state::Id b);
	// Applies a command now, between ticks, with the code a tick runs for it (a paused window move, SIMLINK.md section 7). The clock does not move;
	// the caller logs it for the tick after (Session::order), ahead of that tick's queued commands.
	void applyNow(const Command& command);
	std::size_t pendingCommands() const noexcept { return pending_.size(); }
	// Route orders applied with heights that do not match their points (a caller bug; the order was not walked). Metric, not state.
	std::uint64_t badOrders() const noexcept { return badOrders_; }

	// Merchant refreshes (trader.hpp, TOWN_LIFE_SLICE T1). seedTraders gives every merchant squad (SQUAD_TEMPLATE `is trader` with
	// `vendors`) a TraderStock and runs its forced spawn refresh at the current clock (the unique special items, the first stock, the
	// timer `now + P`): newGame calls it once. Squads that already have a TraderStock are left. Needs the game data. Between ticks.
	// It first gives every pack animal (an ANIMAL_CHARACTER with a `backpack`, a member of any squad) its worn backpack and the spawn fill
	// (T2, TQ14): a Caravan, which is not `is trader`, keeps that fill for life and never refreshes.
	void seedTraders();
	// The pack animals alone (seedTraders calls it; stepTraders again after squads were added, e.g. a respawned caravan). Members that already
	// have an Inventory are left. Returns how many were given one.
	std::uint32_t seedPackAnimals();
	// Counters over this process's life (metrics, not state, not saved); `overflowDestroyed` is D2, units that did not fit the store.
	struct TraderStats
	{
		std::uint64_t refreshes = 0; // timer refreshes
		std::uint64_t forced = 0; // spawn refreshes
		std::uint64_t placed = 0;
		std::uint64_t overflowDestroyed = 0;
	};
	const TraderStats& traderStats() const noexcept { return traderStats_; }

	// The real-economy overlay (economy.hpp, TOWN_LIFE_SLICE 7). seedEconomy (newGame, `mode` real) sets the EconomySettings
	// singleton and gives every town a TownStock seeded from its trade goods and a SupplyBoard; in `original` it creates nothing.
	// stepEconomy runs the aggregate tick: every town, once a game hour, production from its EconomyProducers minus their
	// consumption, clamped to [0, 2 x target]. Between ticks.
	void seedEconomy(EconomyMode mode);
	void stepEconomy();
	// The economy's Lua hooks (economy_script.hpp, TOWN_LIFE_SLICE 7.8 / 7.13): nullptr -- the default, and always in `original` mode,
	// which calls no hook -- runs the engine's rules. A process setting, never saved; the caller loads the script (Hooks::load).
	void setEconomyHooks(std::shared_ptr<economy::Hooks> hooks);
	economy::Hooks* economyHooks() const noexcept { return economyHooks_.get(); }

	// The in-town routines (residents.cpp, TOWN_LIFE_SLICE 5): the reservation book (derived, never saved: rebuilt at load from every
	// member's Routine::target and every SquadRoutine::barTable), and counters over this process's life.
	const routines::Reservations& routineBook() const noexcept { return routineBook_; }
	struct ResidentStats
	{
		std::uint64_t promotedSquads = 0;
		std::uint64_t demotedSquads = 0;
		std::uint64_t promotedMembers = 0;
		std::uint64_t demotedMembers = 0;
		std::uint64_t leaderWarps = 0; // a carried leader with no route to its carrot for 10 s, placed at the carrot
		std::uint64_t skippedMembers = 0; // makeFighter threw, or no ground under the member
		// World squads (5.14): squads promoted / demoted, members built / removed, and the members given a hostile team (not team 0).
		std::uint64_t worldPromotedSquads = 0;
		std::uint64_t worldDemotedSquads = 0;
		std::uint64_t worldPromotedMembers = 0;
		std::uint64_t worldDemotedMembers = 0;
		std::uint64_t worldHostileMembers = 0;
		std::uint64_t stragglerSplits = 0; // members split off their squad at demotion (k16.6.53), and tokens merged back
		std::uint64_t stragglerMerges = 0;
	};
	const ResidentStats& residentStats() const noexcept { return residentStats_; }
	// Rebuilds the book from the loaded components (enableCombat calls it; a test may call it again). Returns the claims lost to a conflict.
	std::size_t rebuildRoutineBook();
	// The town goal scorer (routine_scorer.cpp, TOWN_LIFE_SLICE 5.3, T3b2). On by default; off, a promoted resident only stands on its ring
	// (the T3b1 layer, which ksim_town_fighters pins). Not saved: a setting of the process.
	void setTownScorer(bool on) noexcept { townScorer_ = on; }
	// Road look-ahead (kenshi-k16.6.720, user direction on k16.6.719): a far order's road follower walks straight to the furthest road-route point within
	// R = scale x the road's width of the walker, to the zone (kRoadZone) round it, instead of marker by marker. 0 = the exe's marker rule (the walked-path
	// compare only). Read when an order starts. Default 3.
	void setRoadLookahead(double widths) noexcept { roadLookahead_ = widths; }
	double roadLookahead() const noexcept { return roadLookahead_; }
	// Look-ahead goals planned so far, how many landed off the target's centre, the sum and max of the shift (u); runtime only.
	const RoadGoalStats& roadGoalStats() const noexcept { return roadGoalStats_; }
	static constexpr double kRoadZone = 25.0; // a road point is "about the area you want to be in": reaching within it is arrival
	// Turret manning (task ds/turret S3): the 149 / 222 finders see the world's turret placements and a gunner mounts one. Off by default: a town
	// with turrets then sends its guards to them (the exe's 149 does), which moves ksim_town_bar / ksim_slaver_caravan / ksim_capture_live, so
	// the switch is ksim's to flip. Not saved: a setting of the process.
	void setTurretManning(bool on) noexcept { turretManning_ = on; }
	// world.offscreen_medical (COMBAT_OPTIONS.md 8, k16.6.52): the stored members of a token squad keep bleeding, healing and dying, a death leaves a
	// corpse that persists and ages off-screen. Off (the original): nobody steps off-screen and an NPC corpse no player area covers is destroyed.
	// Not saved: a setting of the process.
	void setOffscreenMedical(bool on) noexcept { offscreenMedical_ = on; }
	bool offscreenMedical() const noexcept { return offscreenMedical_; }
	// Whether the member's goal list (its role in the squad's applied package) holds 58 JOB_MEDIC: the only members that treat a downed ally
	// (rwh.204 / dt45). Any kit-carrier treats itself (269). Not for a member whose squad has no board.
	bool canTreatAllies(flecs::entity member);
	// world.offscreen_fights (COMBAT_OPTIONS.md 9, k16.6.46): two hostile tokens (neither built) that meet fight a stats roll, once a game hour per
	// pair; the loser's half falls and it routs (token_fights.cpp). Off (the original: nothing happens off-screen). Not saved: a setting of the
	// process (the pair clock is not saved either).
	void setOffscreenFights(bool on) noexcept { offscreenFights_ = on; }
	bool offscreenFights() const noexcept { return offscreenFights_; }
	struct FightStats
	{
		std::uint64_t fights = 0;
		std::uint64_t cooling = 0; // meetings skipped: the pair fought within the game hour, or a side fought this step
		std::uint64_t empty = 0; // meetings skipped: a side has no living, conscious stored member
		std::uint64_t steps = 0; // steps that ran with the setting on
		std::uint64_t tokensMax = 0; // the most tokens (squads not in detail, not the player's) seen in one step
		std::uint64_t meetings = 0; // hostile pairs within the radius, summed over the steps (a pair counts at each step it is close)
	};
	const FightStats& fightStats() const noexcept { return fightStats_; }
#if KSIM_PROFILE
	// The per-system tick timings (docs/PROFILING.md); only in a KSIM_PROFILE build. Sim thread only.
	profile::Profiler& profiler() const noexcept { return profiler_; }
#endif
	struct TownScorerStats
	{
		std::uint64_t passes = 0; // member evaluations
		std::uint64_t placed = 0; // seeded placements (promotion)
		std::uint64_t adopted = 0; // goal or target changes
		std::uint64_t blocked = 0; // (goal, target) pairs blocked after a failed finder or route
		std::uint64_t noRoute = 0; // walks the navigator refused
		std::uint64_t unreached = 0; // claims released after kReservationMaxHours without arriving
		std::uint64_t held = 0; // passes that kept the running goal
		std::uint64_t fallbacks = 0; // passes where only the STAY_IN_HOME fallback was plannable
		std::uint64_t slept = 0; // members that lay down in a bed (T3c)
		std::uint64_t doorsLocked = 0; // home doors a resident locked
		std::uint64_t doorsOpened = 0; // home doors a resident opened
		std::uint64_t shoos = 0; // shoo goals adopted
		std::uint64_t tablesBar = 0; // table claims taken in a BAR building (T3d)
		std::uint64_t tablesPublic = 0; // ... by the any-public-table rule
		std::uint64_t tablesNone = 0; // finders that found no table at all (the goal drops out)
		std::uint64_t shoppingTrips = 0; // pretend SHOPPING goals adopted
		std::uint64_t traderTables = 0; // wandering-trader stays given a table
		std::uint64_t cagesLocked = 0; // home cage lock tasks applied (T3e2)
		std::uint64_t slavesCaught = 0; // RESTRAINING chains finished (T3e2)
		std::uint64_t slavesCaged = 0; // characters put into a cage (T3e2)
		std::uint64_t corpsesDisposed = 0; // bodies fed to a disposal machine and consumed (137, the corpse pot)
		std::map<std::uint16_t, std::uint64_t> byGoal; // adoptions per AI_TASK enum
	};
	const TownScorerStats& townScorerStats() const noexcept { return townStats_; }

	// Between ticks: a new instance at (x, z), owned by the region engine.
	// group is a stand-in group index (-1 = none). For the region tests'
	// stand-in workload; later, whatever creates positioned instances.
	flecs::entity spawnMover(double x, double z, std::int32_t group = -1);

	// The state::Clock tick (absolute, since game day 0 00:00).
	std::uint64_t clockTick() const;
	// Sets the clock before the first tick (a Simulation::load world starts at day 0 00:00, which is night: perception's
	// sight range and light read the time of day). Scenarios and tests start at `tickAt(kNewGameDay, kNewGameHour, 0)`.
	void setClockTick(std::uint64_t tick);
	double simSeconds() const { return static_cast<double>(clockTick()) * kTickSeconds; }
	std::uint64_t seed() const noexcept { return seed_; }

	const World& world() const noexcept { return world_; }
	const std::vector<SystemState>& systems() const noexcept { return systems_; }
	std::uint64_t rngState(Subsystem subsystem) const;
	const RegionEngine& regions() const noexcept { return *regions_; }
	void setHitObserver(std::function<void(const HitEvent&)> observer);

	// ksim::stateHash over the world: instances, relationships, clock,
	// scheduler, region-engine hot state (through its mirror). Independent
	// of flecs entity ids and of the worker thread count; pending commands
	// are not state yet.
	std::uint64_t stateHash() const;
#if KSIM_DEV
	std::vector<StatePart> stateParts() const; // dev: stateHash's input per instance and component (k16.6.390)
#endif

	// Marks (or clears) a building as a skin peeler by hand: the data names it by a BUILDING_FUNCTIONALITY record ("skin peeler", a mod's, in
	// rebirth.mod), which a test world without that mod lacks. OURS.
	void setPeeler(std::uint64_t buildingKey, bool on) { peelerSites_[buildingKey] = on; }
	// A cage a scenario put a prisoner into reads Locked whatever its warden squad did (imprisonScenario; cageDoorLocked): the warden that streams in later has not
	// run its lock task. Transient (not saved), cleared by newWorld.
	void setScenarioLocked(std::uint64_t cageBuilding) { scenarioLockedCages_.insert(cageBuilding); }
	// Whether a cage building's door is locked: its warden squad locked its cages (195; OURS - one flag per squad), unless a lock task opened it
	// since the last prisoner went in. 207 / 256 / 133's precondition isDoorLocked = 0.
	bool cageDoorLocked(std::uint64_t cageBuilding) const;
	// The lock fields of the shackles `wearer` has on (their ARMOUR record's `lock level` / `hardness`; lock_tasks.cpp).
	const kdata::Record* shackleRecord(std::uint64_t wearer) const;
	double shackleLockLevel(std::uint64_t wearer) const;
	double shackleHardness(std::uint64_t wearer) const;
	// ---- the lock tasks (lock_tasks.cpp; task ds/shackles-locks) ----
	// One Lock model for a cage and for worn shackles (door_plan.hpp; dt24_picklock section 8): `open` is the unlocked/broken state, the
	// timer is the picking timer shared by every picker, `hardness` the work value the cut drains (lock +4, dt24_shackles section 2).
	using LockRecord = state::LockStates::Entry; // saved: the LockStates singleton (v87)
	enum class LockStep : std::uint8_t
	{
		Working, // the task continues next tick
		Done, // the task finished (flagTaskDone): the lock is open, or there was nothing to do
		Refused, // flagImpossible: the precondition failed (no tools, not strong enough, nobody to release)
		TooHard, // a pick at the lock with the skill below the floor: ends like Refused, event 13 result 8 (k16.6.377)
	};
	// One tick of a lock task by `id` on `victim` (0 = the actor itself, its own cage or shackles): 76 PICK_LOCK, 285 CUT_LOCK and 286
	// BRUTE_FORCE_LOCK on the cage the target is in; 201 PICK_LOCK_ON_SHACKLES, 185 CUT_SHACKLES and 186 BRUTE_FORCE_SHACKLES on the target's
	// worn shackles; 219 TAKE_OFF_MY_SHACKLES; 110 RELEASE_PRISONER (victim = the prisoner); 133 GET_OUT_OF_CAGE_LEGIT. The caller walks.
	LockStep stepLockTask(std::uint64_t id, std::uint16_t task, std::uint64_t victim);
	// A player order for one of those tasks (the planner reaches them only through 207 / 256): the actor walks to the target and runs it.
	bool orderLockTask(std::uint64_t id, std::uint16_t task, std::uint64_t victim);
	// The player's shackle orders (order 60 actions 15 / 16 / 17, kenshi-h2v.15.1): `actor` picks (201), cuts (185) or breaks (186) the locked
	// shackles `wearer` (itself, or another character) has on. check: "" or "bad_target" (no such character), "not_allowed" (the actor cannot
	// act, or the wearer has no locked shackles); no tool and too little strength are run-time ends (event 13 result 1). order replaces the actor's
	// other orders (cancelOrders) and runs the task; event 13 (the action, target = wearer) ends it: 0 done, 1 refused, 3 wearer gone, 4 actor
	// KO'd or dead, 5 cancelled.
	// The same three on a cage's lock (actions 18 / 19 / 20, kenshi-k16.5.7): `wearer` is then the cage key (a building key some character is caged in;
	// anything else is bad_target) and the lock must be locked. The actor walks to an occupant (a caged actor works its own cage from inside); event 13
	// names the cage key, and 3 also reads: no occupant left in it.
	std::string checkShackleAction(std::uint64_t actor, std::uint64_t wearer, std::uint8_t action) const;
	std::string orderShackleAction(std::uint64_t actor, std::uint64_t wearer, std::uint8_t action);
	// The cage a character is caged in as the CAGES frame shows it (SIMLINK CAGES, kenshi-k16.5.7); false when `cage` is not a caged-in building.
	struct CageView
	{
		std::uint64_t cage = 0;
		std::string record; // the cage building's BUILDING record
		bool locked = false;
		double level = 0.0; // cageLockLevel
		double hardness = 0.0; // what is left of it (a cut drains it)
		double maxHardness = 0.0; // cageLockHardness
	};
	bool cageView(std::uint64_t cage, CageView& view) const;
	// Whether a pick, cut or break already opened the cage's lock (cheap; the CAGES change test).
	bool cageLockOpened(std::uint64_t cage) const;
	// Order 99 on a cage key: the MENU_ENTRIES bits for the cage (16 pick lock, 17 use tools, 18 use strength), 0 when `cage` is not a cage key; `chance` is
	// the actor's pick chance per roll (pickChance(level, lockpicking) less the caged-picker penalty). A read.
	std::uint32_t cageMenuEntries(std::uint64_t actor, std::uint64_t cage, float& chance) const;
	// Order 99 on a door id: bit 19 Lock when order 60 action 21 would pass its gates, bit 20 Unlock for action 11; 0 for a non-door or an actor that cannot act.
	std::uint32_t doorMenuEntries(std::uint64_t actor, std::uint64_t door) const;
	// The character a cage order walks to: the actor itself when it is caged there, else the lowest id caged in `cage`; 0 = none.
	std::uint64_t cageOccupant(std::uint64_t actor, std::uint64_t cage) const;
	// What the wearer's shackles look like on the wire (SIMLINK ITEM_LOCK): worn and locked, the lock's level and the wearer's own pick chance,
	// the hardness now and at the start, and whether the wearer could cut (a carried tool) or break them (strength) right now.
	struct ShackleLockView
	{
		bool worn = false;
		bool locked = false;
		bool canCut = false;
		bool canBreak = false;
		double level = 0.0;
		double chance = 0.0;
		double hardness = 0.0;
		double maxHardness = 0.0;
	};
	ShackleLockView shackleLock(std::uint64_t wearer) const;
	// The reach to a character a lock task acts on (lock_tasks.cpp); `stopped` = the actor stands still (twice the moving reach).
	double lockReach(const state::Fighter& target, bool stopped) const;
	// A player order for a construction task on a player building (routines::task::isConstructionTask): 2 BUILD, 71 ADD_MATERIALS, 95 REPAIR,
	// 96 DISMANTLE, 97 USE_TRAINING_DUMMY, 156 ENTER_BUILDING, 200 LEAVE_BUILDING. The actor (a character of the player's faction) walks to the
	// building and runs it; it replaces the actor's previous such order. False for an unknown actor or building, another faction's building or
	// a task that does not apply (a dismantled building, 97 on a building that is not a training dummy). HOOK for the client's build menu:
	// order 2 / 71 / 95 / 96 are what a Build / Add materials / Repair / Dismantle button sends. Orders are not saved (as lock orders).
	bool orderConstructionTask(std::uint64_t id, std::uint16_t task, std::uint64_t building);
	std::size_t constructionOrderCount() const noexcept { return buildOrders_.size(); }
	// 203 ARREST_TARGET as one generic order (arrest.cpp; the crime system, the dialogue actions DA_ARREST_TARGET / DA_PAY_BOUNTY and a
	// player share it): `guard` waits for `victim` to be down, lifts it (`lift` 240 = a person lying there, 213 = also snatches it from its
	// carrier), carries it to the nearest free cage its faction may use and puts it in (imprison: the sentence starts there). False for an
	// unknown character. Orders are not saved.
	bool orderArrest(std::uint64_t guard, std::uint64_t victim, std::uint16_t lift);
	std::size_t arrestOrderCount() const noexcept { return arrestOrders_.size(); }
	bool arrestOrdered(std::uint64_t guard) const noexcept { return arrestOrders_.count(guard) != 0; }
	// The town alarm manager (alarm.hpp; kinspect dt41_E; crime_goals.cpp). raiseAlarm is the writer 1409335e0 for the exe's two call sites
	// (Task_Kidnapping's failed stealth roll, not modelled; the dialogue effect DA_CRIME_ALARM): an alarm of `kind` at (x, z) on the town
	// the point stands in, raised by `raiser` (its building and faction are the record's); null when the point is in no town. pickAlarm is the
	// selection 14092a810 for `id` (the 209 / 210 tick's and ALARMS_IN_THE_VICINITY's): the nearest alarm of its town within 375 u;
	// `alliesOnly` is 210's filter (UNSURE: the raiser is an ally of `id`).
	const alarm::Alarm* raiseAlarm(std::uint8_t kind, double x, double z, std::uint64_t raiser, std::uint64_t criminal = 0);
	const alarm::Alarm* pickAlarm(std::uint64_t id, bool alliesOnly) const;
	std::size_t alarmCount() const noexcept { return alarms_.size(); }
	// The crime goals' finders (crime_goals.cpp): 211's prisoner (1408cf3c0), 212's looted prisoner (1408cf6a0), 204's bounty target
	// (14099bfc0). Each names one character or 0; the scorer adopts the goal on a hit and stepCrimeGoals runs it.
	std::uint64_t prisonerToFree211(std::uint64_t id) const;
	std::uint64_t lootedPrisoner212(std::uint64_t id) const;
	std::uint64_t bountyTarget204(std::uint64_t id) const;
	// 212's take: every stolen item of the occupant goes into the looter's own inventory; returns the units moved.
	std::uint32_t confiscate212(std::uint64_t looter, std::uint64_t occupant);
	// The crime goals 209 / 210 / 211 / 212 / 204 as task handlers (crime_goals.cpp): the registry finder for the goal's task type (an alarm id
	// or a character id, 0 = none) and the point the scorer walks to. isCrimeGoal is the table's membership test.
	static bool isCrimeGoal(std::uint16_t task);
	std::uint64_t crimeFinder(std::uint16_t task, std::uint64_t id) const;
	bool crimeWalkPoint(std::uint16_t task, std::uint64_t id, std::uint64_t target, double& x, double& z) const;
	// 272 SMASH_BUILDING: the actor walks to the player building and bashes it down (Task_BashDoor's blow through removeHp). False for an
	// unknown actor or a building that is not a live player building.
	bool orderSmash(std::uint64_t id, std::uint64_t building);
	std::size_t smashOrderCount() const noexcept { return smashOrders_.size(); }
	// Farm crops (task ds/arrest-2, crops.cpp): the saved plot of every building with `farm data` (made ripe at the first use), 232's finder
	// (the nearest edible plot with food in the member's town, none in a town whose faction likes it), and 231 as an order: walk, bite,
	// the plot's food goes down, the first bite is crime 14 against the farm's faction. cropFood is -1 for an unknown plot.
	std::vector<std::uint64_t> cropPlots();
	double cropFood(std::uint64_t plotId);
	std::uint64_t findCrops(std::uint64_t id);
	bool orderEatCrops(std::uint64_t id);
	std::size_t cropOrderCount() const noexcept { return cropOrders_.size(); }
	// The animal and food goals of the generic path (task port-animals, animal_goals.cpp): the goal's finder (worldres) picks the target, a Routine
	// carries the goal (a code-only goal: an order or a plan names it, no package row does), stepAnimalGoals executes it. 231 goes through
	// orderEatCrops, 261 is 175's floor sleep (world_tasks.cpp). `goal`: 231, 238, 259, 261, 277, 278 or 279. False when the finder names nothing.
	bool orderAnimalGoal(std::uint64_t id, std::uint16_t goal);
	// The standing goals of the generic path (task port-standing, standing_goals.cpp): 24 WANDERER (the nearest town, walk in), 19 GO_HOMEBUILDING (the
	// member's home building, walk there), 30 HOLD_POSITION (walk to (x, z), then stand) and 62 STAND_STILL (stop where it is). No package names them (an
	// order or a plan does), so the order runs the finder and puts the goal in the member's Routine; stepStandingGoals executes it. 30 and 62 have no
	// finish (Task_Blank / Task_StandStill): they hold until a fight, a fall or a lift ends them. False when the finder names nothing.
	bool orderStandingGoal(std::uint64_t id, std::uint16_t goal, double x = 0.0, double z = 0.0);
	// Whether `id` holds a severed limb in its main inventory (the "mouth limb" of 278 / 279, 1405c82d0).
	bool carriesLimb(std::uint64_t id) const;
	// The talk goals 102 / 159 / 164 / 165 (talk_goals.cpp): the Routine naming the hand `target`; false for another goal, an unable member or a
	// target that is gone. world_tasks.cpp's executor runs it.
	bool orderTalkGoal(std::uint64_t id, std::uint16_t goal, std::uint64_t target);
	// 280 RUN_AWAY_FORCED (DA_RUN_AWAY): the member runs from `from`, taking no fight; a Routine goal 280, executed by stepRunAwayGoals. False
	// for a player character, a downed one, no navmesh or no way to run at all.
	bool orderRunAwayGoal(std::uint64_t id, std::uint64_t from);
	std::uint16_t routineGoalOf(std::uint64_t id) const
	{
		const state::Routine* r = fighterEntity(id).get<state::Routine>();
		return r != nullptr ? r->goal : 0;
	}
	// The point `id`'s 280 run holds; false when it has no 280 Routine.
	bool runAwayPoint(std::uint64_t id, double& x, double& z) const
	{
		const auto run = runAway_.find(id);
		if (run == runAway_.end())
			return false;
		x = run->second.x;
		z = run->second.z;
		return true;
	}
	// 278's run: the destination it holds and how many it has picked (the start point is the first). {0, 0} / 0 when it has none.
	std::pair<double, double> limbPlayDestination(std::uint64_t id) const
	{
		const auto found = limbPlay_.find(id);
		return found == limbPlay_.end() ? std::pair<double, double>{0.0, 0.0} : std::pair<double, double>{found->second.x, found->second.z};
	}
	// The 209 / 210 wait's seconds left: 0 when the member has none.
	double alarmDwellLeft(std::uint64_t id) const
	{
		const auto found = alarmDwell_.find(id);
		return found == alarmDwell_.end() ? 0.0 : found->second;
	}
	std::uint32_t limbPlayPicks(std::uint64_t id) const
	{
		const auto found = limbPlay_.find(id);
		return found == limbPlay_.end() ? 0 : found->second.picks;
	}
	// The put-in of a carried person into a cage (the shared end of 108 and the arrest): sets it in, locks the cage, notes the captor and
	// starts the sentence from the cage owner's legal faction's bounty. Whether the prisoner has served it (FUN_1405c9f00).
	void imprison(std::uint64_t carrier, std::uint64_t body, const routines::SiteEntry& cage);
	// The bed put-in (99 PUT_SOMEONE_IN_BED; order 60 action 9 from a carrier): arms freed, the body lies asleep on the bed's spot, the bed claimed for it.
	void putInBed(std::uint64_t carrier, std::uint64_t body, const routines::SiteEntry& bed);
	// A scenario file's `cage=` key (kenshi-k16.6.36): puts `body` in a free cage through imprison (the body is its own carrier: its faction is
	// the captor, a bounty starts a sentence only when a CrimeRecord holds one). `which` is "near" (the nearest free cage within
	// kScenarioCageReach of the body) or a building's placement key (decimal). `bounty` > 0 first puts that bounty on the body with the cage owner's
	// legal faction, so the put-in starts a sentence (without one the prisoner counts as served and the law frees it). False when no free cage matches.
	static constexpr double kScenarioCageReach = 80.0;
	bool imprisonScenario(std::uint64_t body, const std::string& which, std::int32_t bounty = 0, bool peeler = false);
	bool prisonerServed(std::uint64_t id) const;
	// The character stands in its squad's home town (the exe's isAtHomeTown; DA_ARREST_TARGET's gate).
	bool atHomeTown(std::uint64_t id) const;
	// Whether the character wears the SHACKLES armour and its lock is still shut / the wearer's shackles at all.
	bool shacklesLocked(std::uint64_t wearer) const;
	bool shacklesWorn(std::uint64_t wearer) const;
	// The lock level (door_plan.hpp rollLockLevel through interact.cpp's doorLockLevel) and the hardness (BUILDING `hardness`) of a cage.
	double cageLockLevel(std::uint64_t cageBuilding) const;
	double cageLockHardness(std::uint64_t cageBuilding) const;
	// Takes the worn shackles off (item and armour numbers); a slave in state 1 becomes ESCAPING. UNSURE: the exe's action record has no reader.
	void removeShackles(std::uint64_t wearer);
	// Task_GetOutOfCage's shared action 1403554b0 (dt24_207 section 2): the occupant slot is released; `escape` is mode 1 (207 / 256:
	// a chained slave becomes ESCAPING), mode 0 the plain leave of 133 and of a released prisoner.
	void leaveCage(std::uint64_t id, std::uint16_t goal, bool escape);
	// The lock records (state::LockStates, saved): null when nobody touched that lock. ensure makes one; erase forgets it.
	const LockRecord* lockFind(bool shackles, std::uint64_t id) const;
	LockRecord* lockFind(bool shackles, std::uint64_t id);
	LockRecord& lockEnsure(bool shackles, std::uint64_t id);
	void lockErase(bool shackles, std::uint64_t id);
	std::size_t lockOrderCount() const noexcept { return lockOrders_.size(); }

	// LOCK_DOOR 77 / LOCK_DOOR_FROM_INSIDE 162 / _OUTSIDE 198 (goap.cpp door rows): walk to the door (its inside / outside stand point for
	// 162 / 198), then lock it (DoorStep phase 6). False when the door is not a shut door of the actor's faction, or there is no route. The
	// town scorer and the world executor lock a home through orderHomeDoorLock (155 / 197).
	bool orderLockDoor(std::uint64_t actor, std::uint64_t door, DoorSide side);
	// 77 LOCK_DOOR's shut step on a door: `lock` locks a shut door, else it closes an open one (the plan's own order for a door left open).
	bool orderShutDoor(std::uint64_t actor, std::uint64_t door, DoorSide side, bool lock);
	// Where the walk-to-the-door tasks 82 / 83 / 163 / 199 stand (door_plan.hpp doorStandPoint): `door` is the plan hand (0 for 83 = the door of the
	// building the actor is in). false when no such door exists.
	bool doorWalkPoint(std::uint16_t task, std::uint64_t actor, std::uint64_t door, double& x, double& z);

private:
	void applyCommands(std::uint64_t tick);
	// RemoveSquad's work (the faction's loss and opinion, then the squad goes; dead members stay as bodies); killerFaction 0 = none.
	void retireSquad(flecs::world& ecs, flecs::entity squad, std::uint64_t killerFaction);
	// Retires every spawner squad whose members are all dead (before the wilderness top-up counts); returns how many.
	std::size_t retireWipedSquads(flecs::world& ecs);
	void apply(const Command& command);
	void runSystem(SystemState& system, std::uint64_t tick);
	void mirrorScheduler();
	// Takes the seed, RNG streams and run counters from a saved state::Scheduler, if the world holds one.
	void adoptScheduler();
	void stepCombat(std::uint64_t now);
	// After the combat step: finishes the interact orders whose walk has ended (interact.cpp).
	void stepInteractions();
	// medical.cpp, before stepInteractions: a down NPC that carries a kit starts the first aid interact on itself (goal 269).
	void stepSelfAid();
	static constexpr double kSelfAidOnsetSeconds = 6.0; // game s from 269 applying to its first aid (dt45 section 1, ~1.2 s wall at speed 5)
	// After the interactions: starts the next queued order (state::OrderQueue) of every character that has finished its last
	// (interact.cpp). A character that is down loses its queue.
	void stepOrderQueues();
	// After the combat step: a walker on a leg-by-leg move order (farOrders_) gets the next leg appended to its route when the end is near, or
	// planned afresh when the route ran out (interact.cpp's neighbour, far_orders.cpp).
	void stepFarOrders();
	// Starts a move order for `f` towards the true goal: far goals and road moves walk in legs, anything else is one route. False: no first leg
	// (no navmesh, the start off it, no exit with ground): the order is not started. `fromRoute`: the route already planned for a near goal.
	bool startMoveOrder(std::uint64_t id, state::Fighter& f, double goalX, double goalZ, std::optional<double> goalY, std::uint32_t seq, bool far);
	// Plans the next leg of `order` from `from`: skips road nodes that cannot be routed (Skip road node), advances the marker when the leg is
	// not clamped. False: the goal's leg is not routable (order.blocked is set by the caller).
	bool planFarLeg(const state::Fighter& f, FarOrder& order, const knav::Position& from, Route& leg) const;
	bool planRoadLookahead(const state::Fighter& f, FarOrder& order, const knav::Position& from, Route& leg) const;
	void ensureRoads();
	// After the combat step: advances the door steps (state::DoorStep) and stops walkers at shut doors on their route
	// (interact.hpp, DOORS.md section 6).
	void stepDoorSteps();
	void stepOrderCombat();
	// After the combat step (carry.cpp): sets Fighter::building from where every fighter stands (buildingAt), whichever way it got
	// there. A dead or carried fighter keeps what it has. (The door-crossing rule is retired: retired/building_door_crossing.txt.)
	void stepBuildings();
	// After the interactions (carry.cpp): the lift, carry and put-down timers (state::CarryStep), the carried following its
	// carrier, the carrier's encumbrance.
	void stepCarry();
	// After the combat step, before stepCrime (stealth_ko.cpp): the stealth KOs that ended this tick; a failed one fires EV 0x38 at the victim.
	void stepStealthKo();
	// After stepCarry (carry.cpp): the NPC residents taking KO'd intruders out of their building (state::TakeOut).
	void stepIntruders();
	// After stepIntruders (carry.cpp): the slave chain (T3e2). A member on CAPTURE_NEW_SLAVES 167 restrains the body its scorer pass
	// picked (Routine::chainTarget), chains it (sets Fighter::slaveState 1 and the captor when empty) and lifts it; a member on
	// FIND_CAGE_AND_PUT_IN 112 carrying it puts it in the cage on arrival (Fighter::cageBuilding / cageSlot). Caged characters are
	// pinned in their cage.
	void stepSlaveTrade();
	// After stepSlaveTrade (carry.cpp; task CH town-residents-3): the corpse pot, DEAD_GUYS_GO_IN_THE_POT 137 (dt24_patrol_town_pot
	// section 3). The scorer picked the dead body (Routine::chainTarget) and the disposal machine (Routine::target, SiteKind::Disposal);
	// the chain lifts the body at 138, walks it to the pot and feeds it at 136 (the body is consumed: the exe's "incinerated or
	// cooked"), which leaves the member's hands empty - 137's goal 100. No layer reaches a terminal state: a chain that cannot finish
	// puts the body down and the member is freed to re-score.
	void stepCorpsePot();
	// The SHOPPING 118 / 119 BUY_SHIT executor (task CH group 3; shopping_sim.cpp): a member on the shopping goal pulses one shop
	// transaction per second of standing at its counter (one stock item of the first unmet record's item function, at random, into
	// the member's inventory), and a failed pulse after a successful one ends the goal (the exe's latch).
	void stepShopping();
	// The member's day's shopping list, built from its character template's `shopping` refs at the first evaluation of a game day
	// and rebuilt once the hour passes 08:00 on a later one (shopping.hpp buildList). False when the list was not due to be rebuilt.
	bool refreshShoppingList(state::Routine& routine, const state::CharacterInstance& character, std::uint64_t diceKey);
	// The merchant squad whose members call `building` home (its state::Inventory is the shop's stock; the same link stepTraders holds
	// its refresh on). The lowest squad Id when several. Invalid when the building has no merchant.
	flecs::entity shopSquad(std::uint64_t building) const;
	// G4 (goap.cpp, kinspect dt18): the task chain that reaches `goal` for `actor` against `body`, with the live evaluators. `cage`
	// is the free cage a caging goal will use (its position is the plan's position; nullptr = none free, so CAGES_ARE_ALL_FULL holds).
	// An empty plan (score 0) is the exe's "the goal scores 0 this update": the caller must not run the goal.
	goap::Plan planSlaveChain(std::uint64_t actor, std::uint16_t goal, std::uint64_t body, const routines::Site* cage) const;
	// G4, the intruder eviction (task CH; kinspect dt24_223): the chain PICKUP_INTRUDERS 223 plans for `actor` against `body` -
	// 67 Running -> 68 LIFT_PERSON (the registry rows in goap.cpp). TAKE_INTRUDER_OUTSIDE 224 is the goal that follows once the body is
	// in the arms; its tick is the take-out's moveTo, not part of this chain.
	goap::Plan planIntruder223(std::uint64_t actor, std::uint64_t body) const;
	// G4, 224's chain once the body is in the arms: 67 Running to the outside point (x, z), then TAKE_INTRUDER_OUTSIDE 224 (its tick
	// is the moveTo; the drop is the arrival hook). An empty plan means no way out: the caller puts the body down where it stands.
	goap::Plan planIntruder224(std::uint64_t actor, std::uint64_t body, double x, double z) const;
	// The town scope (task CH group 3; dt24_273): 273 plans like 223 (walk -> 68 LIFT_PERSON -> the goal node); 274 plans like 224, its
	// goal state INTRUDER_IS_OUTSIDE_GATES true once the carrier stands outside the town's gates (the body has no town counts too).
	void releaseWorldIntruderGoal(flecs::entity actor);
	goap::Plan planIntruder273(std::uint64_t actor, std::uint64_t body) const;
	goap::Plan planIntruder274(std::uint64_t actor, std::uint64_t body, double x, double z) const;
	// Whether the town the point stands in is owned by `faction` (a state::Id of a FACTION): the exe's "town is ours" test
	// (1406211e0, UNSURE; ours reads TownState::owner). The current town is towncircle.hpp's (the exe's getTown). Goal 273 only.
	bool townOursAt(double x, double z, std::uint64_t faction) const;
	// Goal 273's resolver (world_resolvers.hpp pickupIntrudersTown273) behind its gate: nullopt when `self` is in no town or the town is not
	// its faction's (townOursAt); else the body's id, 0 when there is none. The scorer, the executor and the world resolver all ask it.
	std::optional<std::uint64_t> townIntruder273(const state::Fighter& self, const sensor::Context& ctx) const;
	// G4, the shoo goals 158 / 283 (dt24_131 / dt24_283): [161 SEND_DIALOGUE_SIGNAL_WITHOUT_MOVING, the goal]. A member outside a
	// building has no plan: its I_AM_INSIDE_TARGET_BUILDING precondition has no provider until it is in.
	goap::Plan planShoo(std::uint64_t actor, std::uint16_t goal) const;
	// G4, the town-routine goals (task ds/town-port): 43 / 255 / 20 / 86 / 155 / 197 against the member's live homebase symbols. Each is a goal
	// node that is its own run, so the plan is the providers of its preconditions (7 UNEQUIP_WEAPON ahead of 43 / 255, 283 shoo ahead of
	// 155 / 197 while strangers are in the house) and then the node. `strangers` is the scorer's stranger test; 155 reads "inside"
	// and 197 "outside" as holding (OURS: the package lists the one that applies).
	// `doors`: the lock goals' door facts (155 / 197): the symbols the lock rows 162 / 198 and the walks 163 / 199 provide. Null keeps the
	// goal symbols holding, so the plan is the goal node alone.
	struct LockFacts
	{
		bool locked = true; // every lockable door is locked
		bool at = true; // standing within reach of a door
	};
	goap::Plan planTownGoal(std::uint64_t actor, std::uint16_t goal, bool strangers, const LockFacts* doors = nullptr) const;
	// G4 (task CG, dt24_58.md): the chain for JOB_MEDIC 58 against `patient`: [25 FIRST_AID_ORDER, 58] while the patient still needs
	// aid (the goal 58 row is Task_Blank), [58] when it does not; an empty plan is "the goal scores 0 this update".
	goap::Plan planMedicChain(std::uint64_t actor, std::uint64_t patient) const;
	// G4 (k16.6.4, MEDICAL_HUNGER_LIVE.md (f)): [269 HEAL_MY_LEGS] while a down, conscious character carries a kit that reaches its own
	// injury; empty when the goal does not apply (stepSelfAid runs the plan's one step, the first aid interact on itself).
	goap::Plan planSelfAid(std::uint64_t actor) const;
	// G4, SHOPPING 118 (task CH group 3; kinspect dt24_118 / dt24_shop): [67 Running to the counter (x, z), 119 BUY_SHIT, 118].
	// `done` is the goal state `?local_2e60` (the member's list has nothing left to buy); atATown is the member's position's town.
	goap::Plan planShopping(std::uint64_t actor, bool done, double x, double z) const;
	// G4, the corpse pot 137 (task CH town-residents-3; dt24_patrol_town_pot section 3): the chain against `body` with (x, z) the
	// pot's position. Empty-handed it is the fetch half [67 Running to the body, 68 LIFT_PERSON, 138 FIND_A_DEAD_GUY]; carrying the
	// body it is [67 Running to the pot, 136 FEED_CORPSE_INTO_MACHINE, 137]. An empty plan is "the goal scores 0 this pass".
	// Whether a cage building's door is locked: its warden squad (the squad whose home is that building) has locked its cages (195;
	// OURS - one flag per squad, not a per-cage lock, as dialogue_sim's IS_IN_LOCKED_CAGE). 207 / 256's precondition isDoorLocked = 0.
	goap::Plan planCorpsePot(std::uint64_t actor, std::uint64_t body, double x, double z,
		std::uint16_t goal = routines::task::DeadGuysInThePot) const;
	// After stepSlaveTrade (world_tasks.cpp, task BO): the package goals a built world squad runs outside the town scorer. A squad of
	// residentData_->world whose template links an AI package carrying CAPTURE_NEW_SLAVES 167 sends up to five idle members to the
	// nearest KO'd free character (the chain is stepSlaveTrade's, its walk here) and has a squadmate with a kit give it first aid; one
	// whose package carries LOOT_THE_DEAD 177 sends one idle member to a down body for its carried food, then hands it back to the
	// follow. A world task member has a Routine and no SquadRoutine (the scorer skips those squads), so the town schedules never
	// override it; it is cleared when the task ends, and the follow moves the member again (TOWN_LIFE_SLICE 5.20).
	void stepWorldTasks();
	// Whether faction instance `from`'s relation to `to` (Fighter::faction ids, state::Id values) is at or above 50, read live from the relation
	// components: the bond set only refreshes while an aggressor exists (world_tasks.cpp; rwh.100's cage-owner gate).
	bool factionAllied(std::uint64_t from, std::uint64_t to) const;
	// TortureBuilding::vf28 (dt39 section 6): every tick, each living occupant of a skin-peeler building is torn limb by limb.
	void stepTorture();
	// The peeler's armour step (rwh42 5 step 2): true while the occupant still wears armour (one piece destroyed, not dropped, per accumulator pass; no body damage then).
	bool stripArmour(std::uint64_t id);
	// G1 (task BY, dt19): the squads' package Blackboards, built from the SQUAD_TEMPLATE's `AI packages` (val0 = priority) and the
	// faction's `AI fallback` at priority 0; afterwards the dialog's contracts / `change AI` own them. ensureBlackboards creates every
	// missing one in Id order (tick start, after the world passes' spawns, after a load): never lazily from a reader, see its comment.
	void ensureBlackboards();
	// The save half of the Boards (state::BoardStates, v92): the runtime state of every Board in Id order, written to the singleton just
	// before a save is encoded (the table is derived and never saved). restoreBoards is the load half, run before the end-of-load
	// ensureBlackboards: it builds each saved squad's Board from the data and lays the saved state over it.
	// The singleton exists only around an encode: it is removed again after the write and after a load, so the state hash of a running
	// world and of the same world loaded agree (the Boards are runtime state, not hashed).
	// The same pair carries the arrest orders (state::ArrestStates, v109): restoreBoards rebuilds arrestOrders_ from the singleton.
	// And limbPlay_, alarms_ and alarmDwell_ (state::RunStates, v110).
	void syncBoardStates() const;
	void dropBoardStates() const;
	void restoreBoards();
	void createBlackboard(flecs::entity squad);
	// 140796240(victim, 1) (kinspect dt43): the victim is a character of `NPC class` 5 standing in a BIOME_GROUP listed in its faction's
	// FACTION `biomes`; setCrime then turns an Assault into Terrorism. False with no areas map (the faction layer's rule: no map, no territory).
	bool diplomatInTerritory(std::uint64_t victimChar, const std::string& victimFaction);
	std::shared_ptr<const AreaLookup> crimeAreas_; // the areas map as a point lookup, built at the first use
	bool crimeAreasTried_ = false;
	std::map<std::string, std::vector<std::uint32_t>> factionBiomes_; // FACTION sid -> sorted BIOME_GROUP `index` colours of `biomes`
	// The squad's applied package (G1): the board after one tick, its CompiledPackage; null with no board or none applied. The one
	// lookup every reader of a squad's package takes (the town scorer, the join goals, the fight-goal window), world squad or resident.
	const routines::CompiledPackage* appliedPackage(flecs::entity squad);
	// After stepTorture (residents.cpp), once a second: the residents of a town squad at Full detail in routine mode become Fighters
	// (promoteResidents), and those of a squad that left Full or routine mode go back to the token (demoteResidents); a dead resident
	// loses its Routine and its reservation (TOWN_LIFE_SLICE 5.3, T3b1).
	void stepResidents();
	// The squads a stepResidents pass chose to promote and has not built: kPromotesPerTick of them (ResidentData::queued).
	void promoteQueued();
	std::size_t promoteSquadsPerTick_ = routines::kPromotesPerTick; // derived (the test knob below)
	std::uint64_t promoteMembersPerTick_ = routines::kPromoteMembersPerTick;
	// After stepResidents (routine_scorer.cpp), every kLookTicks: the town goal scorer for every promoted resident whose nextScore is due.
	void stepRoutines();
	// 140345620, the get-out-of-bed action shared by 116 / 208 / 257 (dt24_208 section 2): the sleeper is up, its bed claim released and its
	// routine cleared so it re-scores at once.
	void getOutOfBed(flecs::entity member, std::uint64_t id);
	// Runs the plan's 7 in front of `goal` where the planner puts it (planHomebase: a goal row with hasWeaponEquipped = 0); true = sheathed.
	bool runWeaponPrefix(std::uint64_t id, std::uint16_t goal);
	// Every tick: a fighter with a target and a bare hand that carries a weapon draws it (the plan's 6 before an attack).
	void stepRearm();
	// One scorer pass for one promoted member (its squad is `squad`); `placement` puts it at the winner's site instead of walking
	// (promotion, residents.cpp). Writes the Routine, the reservation and the member's route.
	void scoreMember(flecs::entity member, flecs::entity squad, bool placement);
	// The bar table finder (routine_scorer.cpp, TOWN_LIFE_SLICE 5.17, T3d), shared by the scorer's RELAX 214 and the wandering trader's stay.
	// A squad's table: keep `current` (SquadRoutine::barTable) while the book still holds it for the squad; else claim a free table of the town's
	// BAR buildings from a start index drawn from hash(seed, squad Id, RELAX, hour index), else any public table of the town, else fail.
	// Takes the book claim; the CALLER writes SquadRoutine::barTable from `key`. `town` is the town entity.
	struct BarTable
	{
		bool ok = false;
		std::uint64_t key = 0; // Site.object of the claim (slot 0), 0 when not ok
		double x = 0.0;
		double z = 0.0;
		bool bar = false; // a table of a BAR building (else: the any-public-table rule)
		std::vector<std::array<double, 2>> seats; // the layout's chairs and stools at the table, nearest first (may be empty)
		bool hasY = false; // a real layout table: y is its floor (a ring spot must be walkable on it); synthetic: the walk aims at the terrain
		double y = 0.0;
	};
	BarTable claimBarTable(flecs::entity squad, flecs::entity town, std::uint64_t current);
	// Gives the squad's table back (the book and SquadRoutine::barTable). A trader squad that has none is left alone.
	void releaseBarTable(flecs::entity squad);
public:
	// SIMLINK flag 15 (T3d): the member is on RELAX at a layout seat of its squad's table (within kSeatTolerance of one). Derived, not saved. False for the
	// ring fallback (no seat in the data), a walking member, and any member of a squad that holds no table.
	// `faceYaw`, when given and the member is seated, receives the yaw that faces the table it serves (yaw 0 = +z toward +x).
	bool seated(std::uint64_t memberId, double* faceYaw = nullptr) const;
	// The machine the member is in (SIMLINK CHAR_DYNAMIC bit 26, k16.6.730): the fighter's stored `machineUse` (bed, cage, turret), else the
	// real layout chair it sits at (a seat has no enter / leave event: it is the position test of seatedAt). Bar-table chairs carry no piece
	// key, so a bar sitter has none yet. A floor-sit and a synthetic bed are no machine.
	std::optional<state::FighterMachineUse> machineUse(std::uint64_t memberId) const;
	// SIMLINK CHAR_DYNAMIC pose tail (kinspect bed_seat_pose.md): where a seated (flag 15) or asleep (flag 16) member's body lies, its
	// facing and clip. A seat: the furniture origin, its +Z (or its table, for a `chair` / `sit on floor` seat), its functionality clip.
	// A real bed: its use node, the node's -X, its clip. A synthetic bed (yards, no furniture): the member's own spot and yaw, sleeponfloor.
	// nullopt when the member is neither.
	struct Pose
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		double yaw = 0.0; // 0 = +z, toward +x (as Fighter::yaw)
		std::string clip;
	};
	std::optional<Pose> pose(std::uint64_t memberId) const;
	// Order 60 action 9 (a bed; SIMLINK INTERACTABLE kind 5 is pending client approval, so nothing emits it yet): the bed a
	// site id (routines::siteId) names, or nullptr. The site index holds the town buildings' layouts, so a player bed is one
	// of those (a real bed piece; a synthetic yard bed has no id: it is not furniture to order).
	const routines::SiteEntry* bedOf(std::uint64_t id) const;
	// Order 60 on a cage building's key (kenshi-k16.6.739): the building's first free cage slot, else its first slot (taken), else nullptr
	// (no cage there). `cageTaken` says whether a character is caged in that slot.
	const routines::SiteEntry* cageOf(std::uint64_t key) const;
	bool cageTaken(const routines::SiteEntry& cage) const;
	// The carrier's put-in order on a cage key: "" yes, else an event 10 word (not_allowed: not carrying a live body, or down; occupied: no free slot;
	// bad_target: no cage there). The lock is not tested (per building; the AI's put-in ignores it).
	std::string putInCageRefusal(std::uint64_t actor, std::uint64_t key) const;
	// Whether `actor` may lie in the bed `id` names now: "" yes, else an event 10 word, "occupied" / "bad_target" / "not_allowed"
	// (UNSURE: the exe's own words). A bed is occupied while any other character's routine targets it, asleep or walking to it.
	std::string sleepRefusal(std::uint64_t actor, std::uint64_t id) const;
	// Order 60 action 4 on a kind 12 machine (kenshi-k16.6.788): the placed converter or turret a wire id names, or nullptr (a producer, a
	// furnace, a container and every id that is no machine are found by their entity).
	const MachineSite* machineSiteOf(std::uint64_t id) const;
	// "" when `actor` may operate / man the machine now, else an event 10 word: "not_allowed" (another faction's), "bad_target" (an automatic
	// or power machine: 152 is not ordered), "full" (the turret is manned) / "no_slot" (every operator slot of a converter is taken).
	std::string machineRefusal(std::uint64_t actor, const MachineSite& site) const;
	// A turret's manning spot (its use node), facing and GUN_DATA row, from its placement; nullptr for a key that is no turret.
	struct TurretPost
	{
		double x = 0.0, y = 0.0, z = 0.0, yaw = 0.0;
		std::string gunSid;
	};
	const TurretPost* turretPostOf(std::uint64_t key) const;
	// The machine site a character holds a Use order on once it stands at it (the converter it operates, the turret it mans), else nullptr.
	const MachineSite* machineOperated(std::uint64_t actor) const;
	// SIMLINK INTERACTABLE kind 5 (client-approved 2026-11-06): every real bed, with the owning building (the doorKey the wire's
	// `building` carries), its furniture BUILDING record, the lie-down spot and facing, its clip and its occupant (0 free). The id
	// is routines::siteId of the layout placement's site key, never a runtime index: it survives a save/load and a restart.
	struct BedView
	{
		std::uint64_t id = 0;
		std::uint64_t building = 0;
		std::string record;
		double x = 0.0, y = 0.0, z = 0.0; // the floor point (the walk's end)
		double useY = 0.0; // the use node's own height: where the body lies (the kind 5 `use` y)
		double yaw = 0.0; // the lie-down facing (the pose tail's rule)
		std::string clip;
		std::uint64_t occupant = 0;
	};
	std::vector<BedView> bedViews() const;
	// The same list without the copy: cached while the site index and the ruin set stand, the occupants refreshed per call. Valid until the next call.
	const std::vector<BedView>& bedViewsRef() const;
	// The same by index into the combat arena's members (no search).
	bool seatedAt(std::size_t index, double* faceYaw = nullptr) const;
	// k16.6.30: the member (combat index) is a STAY_IN_HOME resident that sits: at its reserved home chair (`seat`, when given) or on the floor.
	bool homeSitAt(std::size_t index, const routines::SiteEntry** seat = nullptr) const;
	// The indexed site a Routine targets (its point and, for a real layout piece, its floor), nullptr when its town is not indexed yet.
	const routines::SiteEntry* routineSite(const state::RoutineSite& site) const;
	// Indexes every town's bars (seats) when a squad holds a table the index lacks: the index is derived and built lazily by the scorer, so after a
	// load it is empty until each town is scored, and seatedAt (the depenetration's exemption) must not depend on that order.
	void knowBarSeats();
	// The same for the chairs of STAY_IN_HOME residents (k16.6.37): once per world, indexes the building of every Routine that targets a Seat, so a
	// loaded game's first depenetration exempts a seated resident as the straight run does.
	void knowHomeSeats();
	// G4, a worker at a machine (dt24_emptymachine_source section 3): the chain `goal` (87 OPERATE_MACHINERY itself, or the goals
	// whose state is IS_WORKING) plans against producer `producerId` - [92 COLLECT_OUTPUT_RESOURCE, 87, the goal] when the output is
	// full, [87, the goal] when it is not, and the goal alone while the actor already operates it. A storage box (284, or 243
	// AQUIRE_FOOD_AT_HOMEBASE after the dt24_243 decode) plans the same way: [67, 244 GRAB_ONE_FOOD, 243] while the actor holds no
	// food. An empty plan = not the building's.
	// Public for ksim_town_scorer's live worker handover and ksim_empty_machine's / ksim_homebase's live plan cases.
	goap::Plan planWorkChain(std::uint64_t actor, std::uint16_t goal, std::uint64_t producerId) const;
	bool inTownCircle(const state::Fighter& who) const;
	// G4, the occupied-town / animal rows (dt24_243 / dt24_275 / dt24_175): a plan against (x, z) with the homebase symbols live -
	// AtATown (the town circle the actor stands in), AtHomeTown (that town is the squad's home town), HasWeaponEquipped,
	// IsCrouchingOrLying, IsCarryingSomething, NoEnemiesInVicinity (the actor is in no fight), IsGeneralAggressionLevel (not
	// modelled: false). 275 SIT_AROUND plans [190 AUTO_LABOURING_MINES_PRETEND, 275] (190 the only writer of IS_WORKING_PRETEND);
	// 175 SLEEP_ON_FLOOR is terminal: [175] while its preconditions hold, empty when one does not.
	goap::Plan planHomebase(std::uint64_t actor, std::uint16_t goal, double x, double z) const;
	// The shared weapon plan steps (task ds/sheathe; GATES_CARRY_STATIC "peaceful tasks"): 7 UNEQUIP_WEAPON puts the weapon in the hands into
	// the pack (true = one moved), 6 EQUIP_WEAPON takes the first carried weapon into the hands (its record sid, "" = none moved). Both leave
	// the player's characters and an animal's natural weapon alone. weaponInHands is the hasWeaponEquipped atom, as planHomebase reads it.
	bool sheatheWeapon(std::uint64_t id, bool player = false); // player: a player character too (the exe's operate start sheathes whoever it is, rwh.272)
	// A fighter in a fight keeps its weapon in hand (k16.6.181): a target, a technique, a swing / block / stumble in progress or an attacker
	// engaged on it. The scorer yields to it and 7 UNEQUIP_WEAPON refuses it, so the draw / sheathe cannot flicker between two targets.
	static bool inFight(const state::Fighter& f);
	static bool inFightNow(const state::Fighter& f); // inFight minus a finished clip's stale technique
	std::string drawWeapon(std::uint64_t id);
	bool weaponInHands(std::uint64_t id) const;
	// The WEAPON record sid of the sheathed weapon (Inventory::kSheathed), "" = none: CHAR_APPEARANCE shows it on the back while kFlagDrawn is clear.
	std::string sheathedWeaponOf(std::uint64_t id) const;
private:
	// Builds the site index of every placed building with an interior layout (and notes the ruins): a function of the data alone, so a
	// save and a load list the same sites, whatever the scorer happened to index before. A no-op after the first call.
	void primeSiteIndex() const;
	// Before the squad AI step: the wandering trader's stay asks for its table here (ai.hpp BarTables).
	void attachBarTables();
	// Before stepResidents (residents.cpp), once a second: the full-detail areas (state::PlayerAreas) follow the living player-controlled characters,
	// one area per region they stand in. Nothing changes while there is no such character (a test may set the areas by command), and nothing
	// is written while the set of regions is the same. MULTIPLAYER.md 4c item 4: until now only the SetPlayerAreas command made an area. Only for a game started by NEW_GAME (a player
	// faction with a start-off); scenario factions keep their commanded areas.
	void stepPlayerAreas();
public:
	// A scenario file with player characters: marks the player faction so stepPlayerAreas keeps the area (and the nav tiles) on them as they walk.
	void markPlayerFactionLive();
	// Tests: the per-tick promotion budget (squads, members) in place of routines::kPromotesPerTick / kPromoteMembersPerTick, to keep a queue in flight.
	void setPromoteBudget(std::size_t squads, std::uint64_t members) { promoteSquadsPerTick_ = squads; promoteMembersPerTick_ = members; }
public:
	// The zones ahead of the player's heading are prefetched (syncNavCoverage). A fight harness turns it off: its heading flips with the fight, the zones
	// load and drop again, and every mesh change restarts the clearance-component build (3-8 s each; kenshi-q0ou).
	void setNavPrefetch(bool on) { navPrefetch_ = on; }

private:
	// corpses.cpp, before dropGoneFighters: decay of dead characters (corpses.hpp, ARCHITECTURE.md "Corpses"; state::CorpseDecay, save v54).
	void stepCorpses();
	// Streaming navmesh (navcoverage.cpp; a MeshNavigator::openWorld mesh only, else nothing): the mesh holds the zones of the full-detail areas
	// (RegionEngine::areas), loaded and dropped at a tick boundary. Called after the commands, after stepPlayerAreas (before anyone is built or
	// routed on the new areas), when the navigation is enabled and after a load. While there are no areas the mesh keeps what it holds. Derived,
	// never saved: a load rebuilds it from the saved PlayerAreas.
	// `spread`: the per-tick call, which loads at most kNavZonesPerTick missing zones (nearest to the player first) and drops the zones no area
	// wants; the rest of a promotion follows on the next ticks. The travel's heading also asks for the zones ahead (prefetch). Without it
	// (a load, a command that moved the areas, enabling the navigation) everything wanted is loaded now.
	void syncNavCoverage(bool spread = false);
	static constexpr std::size_t kNavZonesPerTick = 1;
	// The doors and containers of the zones the mesh holds and has not furnished yet get their entities (never twice: spawnMeshDoors skips a key
	// that has one). `furnishedZones_` is reset by a load, whose entities come from the save.
	void furnishNavCoverage();
	// Before the combat step (residents.cpp): a fighter whose entity was destroyed (its squad was removed) leaves the arena and the
	// reservation book. Free when no resident is promoted.
	void dropGoneFighters();
	// residents.cpp: the two halves of stepResidents for one squad entity.
	void promoteResidents(flecs::entity squad);
	void demoteResidents(flecs::entity squad);
	// World squads as Fighters (residents.cpp, TOWN_LIFE_SLICE 5.14): any squad at Full detail that is not a town resident group.
	// Members get team worldTeam(faction); demotion waits while a member fights. The marker is derived (no saved state).
	void promoteWorldSquad(flecs::entity squad);
	// MQ14 path 2 / BQ10: the squad's package holds the goal ATTACK_ENEMIES_AND_NEUTRALS (21) in the role list (state::kRole*) of one character;
	// its hour window comes back in start / end.
	bool attackGoalWindow(flecs::entity squad, std::uint8_t role, std::int32_t& start, std::int32_t& end);
	void demoteWorldSquad(flecs::entity squad);
	// Built world squads move by their Fighters (TOWN_LIFE_SLICE 5.14 follow). Before the squad AI, once a second: the token takes the leader's
	// position (the leader is the first living, non-player member), and the AI is told which squads are carried and which hold (a member fights).
	void prepareWorldCarry();
	// Calls `visit` for every squad whose second-slice is `now` (inSlice, k16.6.232), in Id order, from the AI's cached squad list (an ecs.each
	// without one: a sim with no AI). `all`: every squad, not just the slice's.
	void forSquadsInSlice(std::uint64_t now, const std::function<void(flecs::entity, const state::Id&, const state::SquadInstance&)>& visit, bool all = false);
	// Rebuilds ResidentData::world from the arena once after a load (residents.cpp).
	void knowWorldSquads();
	// After the squad AI, once a second: the leader walks to a carrot ahead of the group's route progress, the other members follow the leader;
	// a blocked route or a lost follow is retried the next second (never a terminal state).
	void stepWorldSquads();
	// world_squad_log.cpp: the line for one squad, one event line (promote / demote), the periodic dump (from stepResidents).
	std::string worldSquadLine(const char* what, flecs::entity squad) const;
	void logWorldSquadEvent(const char* what, flecs::entity squad) const;
	void logWorldSquads();
	// Builds one member's Fighter on its own CharacterInstance entity (dice keyed by its Id, placement on a ring round `at`, snapped to
	// the navmesh). False (and remembered in ResidentData::skipped) when it cannot be built or has no ground. `base` = the dice seed.
	// `body` (leaveCorpse): a dead member is put exactly at `at`, with no navmesh snap (a body needs no walkable ground).
	bool buildMember(flecs::entity member, const state::SquadInstance& instance, state::Position at, std::size_t k, std::size_t n,
		std::int32_t team, std::uint64_t& base, bool world = false, bool body = false);
	// The Fighter makeFighter gives member k of `instance` from its template and dice (no placement, team or squad): what an untouched member
	// is rebuilt as, and what demoteWorldSquad compares a member to (state::MemberDelta). Nullopt when the template cannot be built.
	std::optional<state::Fighter> templateFighter(flecs::entity member, const state::SquadInstance& instance, std::size_t index, std::uint64_t& base);
	// Before the squad AI (gatecrew.cpp), once a second: every gate crew's binding is checked; one whose gate is gone rebinds
	// (own gate, the home building's gate, the town's nearest free gateway, else it stands down: GateCrew removed).
	void stepGateBinding();
	// Once a second after it (gatecrew.cpp): a crew at full detail: guards on the gate's slots, the rest off duty on the town scorer (rwh.207).
	void stepGateCrewPosts();
	std::vector<int> postSlots(flecs::entity squad, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members); // gatecrew.cpp
	// Applies an inventory order (commit) or only judges it; the reason as checkInventoryOrder.
	std::string runInventoryOrder(const InventoryOrder& order, bool commit);
	// Applies a squad order (commit) or only judges it; the reason as checkSquadOrder (player_squads.cpp).
	std::string runSquadOrder(const SquadOrder& order, bool commit);
	// Applies a join (commit) or only judges it; the reason as checkChangeSquad (player_squads.cpp).
	std::string runChangeSquad(std::uint64_t character, std::uint64_t squad, bool commit);
	// After the combat step: dead characters leave their player squad.
	void stepPlayerSquads();
	// Before the commands (newgame.cpp): a player faction whose completed-research set is empty completes `_Default Start`.
	void stepUnlocks();
	// Applies a job order (commit) or only judges it; the reason as checkJobOrder (jobs.cpp).
	std::string runJobOrder(const JobOrder& order, bool commit);
	// After the order queues (jobs.cpp): the executors of the jobs lists. Every kJobIntervalTicks each character that no direct
	// order and no fight holds picks the first row it can plan (ARCHITECTURE "V1: the original's per-character jobs").
	void stepJobs();
	// After the jobs (trader.cpp), every trader::kIntervalTicks: a merchant squad at full detail whose `nextRefresh` has passed
	// (or whose forced spawn refresh has not run) is refreshed once (TOWN_LIFE_SLICE T1); no catch-up.
	void stepTraders();
	// After the commands (discovery.cpp): every kDiscoveryIntervalTicks (or `force`: a new game's first check) the places near a player
	// character or the camera focus become discovered / visited on the first player faction.
	void stepDiscovery(bool force);
	// player_squads.cpp: the player faction's known set (PlayerFaction::known, SIMLINK FACTION_RELATIONS 38). seedKnownFactions adds every
	// faction the player has a Relation pair toward (a new game: what the template and the start-off set explicitly); stepKnownFactions,
	// once per sim-second, adds the faction of any character within perception::kBaseRange of a player character.
	void seedKnownFactions(flecs::entity player);
	void stepKnownFactions();
	// The TOWN records with their positions, built on first use (null without game data).
	std::shared_ptr<const struct DiscoveryData> discoveryData() const;
	// producers.cpp, after stepInteractions: once a second the producer sites near a player character become entities (spawnProducer) and
	// unused far ones are dropped; every tick the operators of each producer advance it and a full one stops them (event 13 result 7).
	void stepProducers();
	// Refills the empty placement item nodes whose time has come (hourly; interact.cpp).
	void stepItemNodes(std::uint64_t now);
	void spawnProducer(const ProducerSite& site, double useX, double useY, double useZ);
	std::uint64_t scenarioProducers_ = 0; // placeScenarioBuilding: the next scenario producer's Id offset
	// The foliage deposits of a zone cell (cached; empty when the foliage inputs cannot be read).
	const std::vector<ProducerSite>& foliageProducers(int cellX, int cellZ);
	std::shared_ptr<const producers::Book> producerBook() const;
	// After the crime step (prospect.cpp): counts the prospect tasks; the one that reaches kTicks computes its result and tells the observers.
	void stepProspect();
	// Applies a prospect order (commit) or only judges it; the reason as checkProspect.
	std::string runProspect(std::uint64_t id, bool commit);
	// The derived data the prospecting reads, built on first use (null without game data or heightmap).
	std::shared_ptr<const struct ProspectData> prospectData() const;
	// buildings.cpp: applies a placement / pause (commit) or only judges it; the reason as checkBuildPlace / checkBuildPause.
	std::string runBuildPlace(const BuildOrder& order, bool commit);
	std::string runBuildPause(std::uint64_t building, bool paused, bool commit);
	// After the jobs (buildings.cpp): every unfinished site gains HP from its builders, up to what its delivered materials allow.
	void stepConstruction();
	std::string judgeBuild(const BuildOrder& order, struct BuildJudge& judge) const;
	// The derived footprint data the placement tests read, built on first use (null without game data).
	std::shared_ptr<struct BuildingGeometry> buildingGeometry() const;
	// Applies a NewGame command (newgame.cpp): checks it again, builds the faction and the squad, tells the observers.
	void runNewGame(const Command& command);
	// Stores a settled inventory on its entity; what fit nowhere is put on the ground at the character's feet (a container's is lost, logged).
	void storeInventory(flecs::entity entity, std::uint64_t id, state::Inventory inventory, std::vector<state::Inventory::Entry> overflow);
	// Wearing a piece (inventory_orders.cpp): `worn` takes the slot its record names in `target`, the piece there goes back to the pack; on
	// commit the fighter `id`'s weapon / armour numbers follow. "" or the reason.
	std::string wearOnto(state::Inventory& target, state::Inventory::Entry worn, const kdata::Record& record, std::uint64_t id, bool commit);
	// The quick move into a character (orders 68 and 63 with section 255): a free matching worn slot, then main, then the worn backpack.
	std::string quickMoveInto(state::Inventory& dest, state::Inventory::Entry piece, std::uint64_t id, bool commit);
	// Puts pieces on the ground at the character's feet (event 6 each).
	void dropPieces(std::uint64_t who, std::vector<state::Inventory::Entry> pieces);
	// A limb of `who` was severed (CombatArena::takeSevers): the victim race's `severed limbs` item for that limb slot lands on the
	// ground at the victim (UNSURE: the original puts it at the Forearm / Calf bone with the hit impulse and tints it; a race without
	// the list drops nothing), and with both legs gone the worn boots drop too (one leg: they stay; weapons stay on an arm sever).
	void onLimbSevered(std::uint64_t who, const std::string& part, bool item = true);
	// A unique character died (CombatArena::takeDeaths): the world event set's dead list, and the involvement set when the killer is
	// the player's (task AN).
	void onNpcDeath(std::uint64_t victim, std::uint64_t killer);
	// Frees the character `carrier` carries (it lies where its carrier stands) and ends the carry. No forced KO timer.
	void dropCarried(std::uint64_t carrier);
	// Task BO, the slave chain's two effects (world_tasks.cpp). The strip (181 STRIP_TARGETS_WEAPONS) sends every worn weapon of the
	// victim to the ground at its feet and leaves it with fists; the cuff (166 CHAIN_TARGET) puts a pair of Prisoner Shackles on it, in
	// the boots slot, worn. Both work on a KO'd character (the inventory-order path refuses one), so they write the inventory and the
	// Fighter's numbers directly.
	void stripTargetWeapons(std::uint64_t victim);
	void shackleTarget(std::uint64_t victim);
	// An EVENT 48 from outside the combat step (task BG): a put-down or a drop (cause 2). The body's rest yaw is rolled like the
	// combat step's own starts; the session's combat observer carries it to the wire.
	void emitRagdoll(std::uint64_t id, std::uint32_t cause);
	// Copies the arena's copy of a fighter to its flecs component (the later steps change fighters after the combat step's mirror).
	void mirrorFighter(std::uint64_t id);
	// The weight of what a character owns (state::Inventory, ITEM `weight kg`; equipped pieces count), 0 without game data.
	double packWeight(std::uint64_t id) const;
	// Whether two characters are allies: the same team or faction, or a faction relation of at least 50.
	bool alliedTo(std::uint64_t a, std::uint64_t b) const;
	// 1405ce1e0's notify (the generic put-down): a kidnap captive let go while awake, no friend of its carrier, force-notices the carrier
	// (perception::notice: the red / green arrow's DETECTED_BY record). Call it before the captive flag is cleared.
	void captiveNoticesCarrier(std::uint64_t captiveId, std::uint64_t carrierId);
	// Ends the running interact order and empties the queue of `id`; each interact among them gets event 13 result 5
	// (cancelled). Called when an order that is not queued replaces them, and when the character is down.
	void cancelOrders(std::uint64_t id);
	// Starts an interact order now: sets the InteractTask and the walk (an empty route = already at the use point).
	void beginInteract(std::uint64_t id, std::uint64_t target, std::uint8_t action, std::uint32_t seq,
		std::vector<state::RoutePoint> route, bool approach, std::uint8_t flags = 0);
	// Reports the end of an interact order (event 13).
	void reportInteract(std::uint64_t who, std::uint64_t target, std::uint8_t action, std::uint8_t result, std::uint8_t flags = 0);
	// After the interactions: every hungry fighter that carries food eats (medical.cpp).
	void stepMeals();
	// One tick of first aid by `medic` on `patient` (medical.cpp): picks and spends a kit, fills the bandage pools. False when
	// there is nothing more to do (no kit, nothing to treat).
	bool treat(std::uint64_t medic, std::uint64_t patient);
	// treat() on bodies the caller holds, `dt` game seconds (medical.cpp); the off-screen medical step runs it on the stored members.
	bool treatBodies(state::Fighter& medic, state::Fighter& patient, bool self, state::Inventory* inventory, bool& taken, double dt, std::uint64_t aidTick = ~std::uint64_t{0}) const;
	// Off-screen medical step (stored_medical.cpp, k16.6.52; behind setOffscreenMedical): once a game minute, the stored members of a world squad
	// bleed, clot, wake, die and are bandaged by a conscious squadmate, with the in-detail functions (updateMedical, treatBodies), then stored again
	// as the difference from their template; a member back to its template loses its record.
	void stepStoredMedical();
	// Stragglers (k16.6.53, residents.cpp): a member apart from its squad at demotion is a token of its own that walks back and merges.
	void stepStragglers();
	void splitStraggler(flecs::entity parent, flecs::entity member, const state::SquadInstance& instance, double x, double z);
	void mergeStraggler(flecs::entity token, flecs::entity parent);
	void mergeStragglersNear(flecs::entity parent);
	// Off-screen fights (token_fights.cpp, k16.6.46; behind setOffscreenFights): once per group-layer step, hostile tokens within the encounter radius.
	void stepTokenFights();
	// Whether two squads of these factions fight: a factionless (wild) squad is hostile to every other, else either faction's relation to the other is hostile.
	bool squadsHostile(const state::SquadInstance& a, const state::SquadInstance& b) const;
	// A stored member's death: its record is written dead and the member is built as a body at the token's position (buildMember `body`), with a
	// CorpseDecay stamped at the death (`cause`: CorpseDecay::kCause*). Entry point for a token fight's death too (k16.6.46).
	// `where`: the body's spot when it is not the squad's (the import's members stand apart).
	void leaveCorpse(flecs::entity squad, flecs::entity member, std::size_t k, state::MemberDelta delta, std::uint8_t cause, const state::Position* where = nullptr);
	// Task CG: whether `id` has a kit of item function `function` for goal 58 - carried (or worn) in an inventory, or the one already
	// in use (Fighter::kitCharges, whose kitSid names its function).
	bool carriesKit(std::uint64_t id, std::int32_t function) const;
	// 184 PUT_LOOT_IN_STORAGE (loot_storage.cpp, dt24_184). carryingExcessLoot is the CARRYING_EXCESS_LOOT evaluator: a carried entry that
	// is loot (not food, a kit or a tool). lootStorage184 is the resolver both the world selector and the residents call: the Id of the
	// storage box (a state::Container) the member's loot goes to, or 0. putLoot184 is the executor's put: every loot stack the box
	// accepts goes in through the container transfer path (checkTransfer / transferItem, so marks and locks hold); returns the units moved.
	bool carryingExcessLoot(std::uint64_t id) const;
	std::uint64_t lootStorage184(std::uint64_t id);
	std::uint32_t putLoot184(std::uint64_t id, std::uint64_t container);
	// Whether the box's `itemtype limit` lets `item` in (a stand takes only its kind; checkTransfer's rule).
	bool containerTakesType(const state::Container& box, const std::string& item) const;
	// Takes `count` carried units of `item` out of the fighter's inventory and puts them on the ground at its feet (a
	// GroundItem, event 6). Nothing when it does not carry that many.
	void dropItem(std::uint64_t who, const std::string& item, std::uint32_t count);
	// Loot: moves every item of the downed fighter `from` into the inventory of `to` (all carried there); the body keeps
	// nothing: its weapon is fists again and it wears no armour. Bumps both inventories' versions.
	// Units that went into the looter's grid inventory and units that did not fit (they stay with the source).
	struct LootResult
	{
		std::uint32_t took = 0;
		std::uint32_t left = 0;
	};
	LootResult lootBody(std::uint64_t to, std::uint64_t from);
	// Moves a carried item to the weapon / armour slot (equip) or an equipped one back to the pack, and applies it to the
	// state::Fighter: the weapon's numbers from the WEAPON record, an armour piece as makeFighter builds it (interact.cpp).
	void equipItem(std::uint64_t who, const std::string& item, bool equip);
	// Moves `count` of `item` between a container's inventory and the character's pack; taking from a foreign container is
	// theft (InteractEvent::Theft).
	void transferItem(std::uint64_t who, std::uint64_t container, const std::string& item, std::uint32_t count, bool take);
	// Loot on a container: takes everything.
	LootResult lootContainer(std::uint64_t to, flecs::entity container);
	// Theft by `who` from a foreign container owned by the faction `ownerSid` (crime.cpp): the crime record (CrimeEnum 3 with the
	// setCrime 3 s timer) and the interact observer's event 14.
	void reportTheft(std::uint64_t who, std::uint64_t container, const std::string& ownerSid);
	// The GUI take's roll (FUN_140794810 over the chance below): true when `who` is CAUGHT and the take must be aborted. Only a player
	// character's take from a foreign container or character rolls; the order paths (loot, ground pick-up) never do. Draws from the
	// FullDetail stream. A failure goes through recordCaughtStealing.
	bool stealRollCaught(std::uint64_t who, std::uint64_t container, const std::string& item, const Storage& storage);
	// THE one place a caught act is recorded (kenshi-k16.6.329 hooks the "Got caught stealing" message, SIMLINK event 51, here): crime 3 with
	// the 30 s timer against the victim faction, and event 8 EV_THIEF_CAUGHT_STEALING_FROM_ME from `victim` (0 = nobody of the owner is
	// near: no event). Also: event 0x2f for a victim asleep in a bed, the victim's crouch zeroed, TEMP_ENEMY 4 (the victim's squad lists the thief).
	void recordCaughtStealing(std::uint64_t thief, std::uint64_t victim, std::uint64_t container, const std::string& ownerSid);
	// Thievery XP of a roll (40 pass / 160 caught, chance < 0.98, not caged).
	void stealXp(std::uint64_t thief, double chance, bool caught);
public:
	// The steal chance of `who` taking `item` out of `container` (a container or a character), 0..2.5: the number a "Stealing chance: N%"
	// tooltip shows (int(chance * 100)) and the roll uses. 1.0 for a take that needs no roll (own or ownerless container, a disabled body).
	// Observation only.
	double stealChance(std::uint64_t who, std::uint64_t container, const std::string& item) const;
	// The percent SIMLINK ITEM_GRID's steal tail carries: int(stealChance * 100) clamped to 0..100, or kStealNoRoll (255) when the take
	// is not rolled (a non-player taker, an own or ownerless container, no such container).
	static constexpr std::uint8_t kStealNoRoll = 255;
	std::uint8_t stealPercent(std::uint64_t who, std::uint64_t container, const std::string& item) const;
private:
	// Starts the EVENT 51 bookkeeping for the crime just reported (cause 1 take, 2 lockpick); the first witness sends it (stepCrime).
	void noteCaughtAct(std::uint64_t who, std::uint8_t cause, std::uint64_t container);
	// The "planted" event (item provenance): `planter` put `itemSid` into `holder`'s pack. Logged only, Lua is not wired.
	void firePlanted(std::uint64_t planter, std::uint64_t holder, const std::string& itemSid);
public:
	// The home building's doors, as the door rows (155/197 lock, 86 open) and the door step read them. A yard has no doors; a gate or a
	// broken door is nobody's to lock or open (a gate belongs to the gate system). `nearest` is the xz distance from the asker.
	struct HomeDoors
	{
		bool any = false;
		bool canLock = false; // a door that is not locked yet (worldres::firstDoorToLock155)
		bool needOpen = false; // a door that is closed or locked (worldres::firstDoorToOpen86)
		bool anyLocked = false;
		double nearest = 1.0e30;
		std::vector<std::uint8_t> states; // the doors' state::Door::state in list order
		std::vector<flecs::entity> doors;
	};
	HomeDoors homeDoorsOf(std::uint64_t homeBuilding, double fromX, double fromZ);
	// A stranger in the building: a conscious player character inside it (UNSURE: 5.6 says every perceived, non-slave, non-ally one).
	bool strangerIn(std::uint64_t building) const;
	// The door rows' shared resolver (155 / 197 / 86; dt24_155): true while the candidate stands. Locking needs a lockable door, an
	// unlocked house and a one-step plan (a stranger inside grows the plan with the 283 shoo, and the lock waits); opening needs a
	// closed door and a plan. Both the town scorer and the world selector call it with their own context.
	// The lock (state 2) or the open (0) on every door of the list that is not broken; the 155 / 197 / 86 steps, the scorer's and the world's.
	void applyHomeDoors(const std::vector<flecs::entity>& doors, bool lock);
	// 155 / 197's planned steps [163 / 199 walk, 162 / 198 lock] as real orders: the nearest home door that is not locked gets one
	// order (the lock of a shut door, the close of an open one, through the interact step); true while a lock order is running or was
	// just issued, false when no door is left to lock. The scorer and the world executor both step the goal with it.
	bool orderHomeDoorLock(std::uint64_t id, const HomeDoors& doors, DoorSide side);
	bool resolveDoorRow(std::uint64_t id, std::uint16_t task, const HomeDoors& doors, bool homeLocked, bool stranger) const;
	// A free site (or my own) of one kind in one building, from the keyed roll (rows 20 / 255 and the bed / guard finders; the town
	// scorer's findSite and the world selector's Resolved switch both call it). `nearestTo` takes the free site nearest that point;
	// `realOnly` drops synthetic sites; Work sites come from the producers within reach of (anchorX, anchorZ). Null when none is free.
	struct SiteChoice
	{
		routines::Site site;
		double x = 0.0;
		double z = 0.0;
		std::optional<double> y;
		std::size_t free = 0; // how many sites of the kind were free to pick from
	};
	bool cageTaken(const routines::Site& site) const;
	// The owner permission of a faction-owned part or producer (k16.6.513 / 687 / 758): a slave, an unowned part, the member's own faction
	// or an ally of it. The member's faction is its Allegiance sid (a resident has no Fighter::faction entity with a FactionState).
	bool ownerPermits(std::uint64_t id, const std::string& ownerFaction) const;
	std::optional<SiteChoice> pickSite(const state::Routine& r, std::uint64_t id, std::uint16_t goal, std::uint64_t key, routines::SiteKind kind,
		double pickRoll, std::uint64_t now, double anchorX, double anchorZ, std::optional<std::array<double, 2>> nearestTo = std::nullopt,
		bool realOnly = false) const;
	// The squad's Blackboard; null for a squad without one (no SquadInstance, or born since the last ensureBlackboards pass).
	blackboard::Board* blackboardOf(flecs::entity squad);
	// kenshi-k16.6.14 (kinspect rwh.8 / rwh.9): the slaver shipping package. noteOwnedSlave puts a freshly cuffed slave in its captor's squad
	// Ownerships set; pruneOwned drops the set's dead or freed entries; findSlaveDepot is the class' town finder (the nearest non-hostile
	// SLAVE_CAMP, unless a TOWN_TOWN with a slave-storage building is nearer than 0.837 x as far: signal 0x1c only; 0xb is camps alone), run
	// once when the package starts; giveOwnedSlaves is the hand-over (every owned slave re-owned by the receiver's side, the set cleared);
	// attachSlaveDepots feeds the AI layer the squads that are shipping and the town they walk to.
	void noteOwnedSlave(std::uint64_t captor, std::uint64_t slave);
	// kenshi-k16.6.111 (jobs17 'Clear-all', Appendix A row 187): a player character that becomes a slave loses its jobs and gets the one row 187 Obedient
	// Slave (Task_Blank: it never plans anything; the player's de-queue is the way to "stop taking their orders").
	void enslaveJobs(std::uint64_t slave);
	// The 187 row's target is the captor hand (rwh.99 section 3), and the row is what a save keeps of it (slave_state.cpp). 0 removes the pointing.
	void pointObedientRow(std::uint64_t slave, std::uint64_t captor);
	// The captor a loaded slave had: its 187 row's target, else its follow target.
	std::uint64_t savedCaptor(std::uint64_t slave) const;
	std::optional<state::GroundPoint> storedCharacterPoint(std::uint64_t id) const; // a character out of the arena: its (token) position
	// The slave state machine (slave_state.cpp, rwh.99 section 6): the one writer of Fighter::slaveState (the exe's 1405a4940). 0 -> 1 with the captor
	// (the hand at char+0x328; 0 keeps the one it has): orders wiped, the 187 row added; leaving 1 removes only that row; 2 sets the 250 s heat.
	void setSlaveState(std::uint64_t id, std::uint8_t state, std::uint64_t captor = 0);
	// Seconds of escape heat left on an escaped slave (state 2; 0 otherwise).
	double slaveHeatSeconds(std::uint64_t id) const;
	// The slave's captor hand (0: none known).
	std::uint64_t slaveCaptor(std::uint64_t id) const;
	void stepSlaveState();
	// Whether a member holds goal 58 on this body (medicsOn_).
	bool aided(std::uint64_t body) const { return medicsOn_.count(body) != 0; }
	void pruneOwned(blackboard::Board& board);
	std::uint64_t findSlaveDepot(flecs::entity squad, const state::Fighter& lead, std::int32_t signalFunc) const;
	std::size_t giveOwnedSlaves(blackboard::Board& board, std::uint64_t receiver);
	void attachSlaveDepots();
	// Logs the depot walks that found no route since the last call (AiSystem::takeDeliveryFailures; kenshi-k16.6.21).
	void logDeliveryFailures();
	// The package list a member's goals read: its MemberRole list, the squad leader forced to Leader (dt19 section 2).
	routines::Role liveRole(flecs::entity member) const;
	// ---- crime (crime.hpp) ----
	// The dialogue hook: every dialogue event a witness would fire (the dialogue engine does not exist yet). Observation only.
	void setDialogueObserver(std::function<void(const crime::DialogueEvent&)> observer) { dialogueObserver_ = std::move(observer); }
	// How many times a dialogue event id was fired (a metric, not state, not saved).
	std::uint64_t dialogueEventCount(int event) const;
	// The character's crime record, null if it holds nothing.
	const state::CrimeRecord* crimeRecord(std::uint64_t id) const;
	// Faction `from`'s relation toward `to` by template sid (the live relation components; the record's default without entities). Tests.
	double factionRelationBetween(const std::string& from, const std::string& to);
	// The bounty the faction `factionSid` holds on the character, 0 for none.
	std::int32_t bountyOn(std::uint64_t id, const std::string& factionSid) const;
	// Records a crime by `who` against `victimFaction` (the exe's setCrime, or reportCrime when timerSeconds > 0), applying the
	// exe's refusals: a law criminal, a victim of the criminal's own faction or without a faction, a "not real" faction, a
	// criminal that is down. False when refused. The theft and assault hooks and the trespass sensor go through here.
	bool reportCrime(std::uint64_t who, std::uint8_t crimeId, const std::string& victimFaction, std::uint64_t victimChar,
		double timerSeconds = 0.0);
	// The cuff's crime (kinspect dt5, Task_CuffTarget vf2 14034af20): an NPC's restrain of a free (not a slave), unbountied,
	// player-owned person records crime 1 ENSLAVING on the cuffer, against the victim's faction. It is a write and the cuff goes
	// on; only witnesses that are friends of the victim react. reportCrime's own refusals still apply. True when recorded.
	// UNSURE: the static read only; the bark live run confirmed the guards' silence, not the record.
	bool reportEnslaving(std::uint64_t cuffer, std::uint64_t victim);
	// A flat bounty add on the character for the faction (spawn bounties, DA_ASSIGN_BOUNTY with an amount).
	void addBounty(std::uint64_t id, const std::string& factionSid, std::int32_t amount);
	// Erases the bounty (release, bail-out). True if it existed.
	bool clearBounty(std::uint64_t id, const std::string& factionSid);
	// DA_PAY_BOUNTY (10): the faction's bounty on the character, claimed now (a second hand-in pays nothing), halved when it is dead.
	// Returns the amount to pay the bounty hunter, 0 for nothing unclaimed. The caller credits the hunter's side.
	std::int32_t claimBounty(std::uint64_t id, const std::string& factionSid, bool dead);
	// DA_CLEAR_BOUNTY (49): erases the faction's bounty and clears the pending crime (the exe's 1408539f0, setCrime(rec, 0) and the
	// timer set to -1). True when a bounty was erased. The tag forgiveness both ways is a HOOK.
	bool forgiveCrime(std::uint64_t id, const std::string& factionSid);
private:
	// After stepGateCrews (crime.cpp): the queued combat events (assault), then once a second the record timers and bounty expiry,
	// the trespass sensor, the witnesses and the wanted-reaction hook.
	void stepCrime(std::uint64_t now);
	void fireDialogue(std::uint64_t speaker, std::uint64_t subject, int event, int reason = 0);
public:
	// ---- dialogue (dialogue_events.hpp, docs/DIALOGUE.md 8 slice 1 steps 3-5; dialogue_sim.cpp) ----
	// Turns the dialogue pipeline on: the producers (shoo, I_SEE_*, and the crime hooks' events) push into the queue, every tick the
	// queue is evaluated against the book (DialogueSystem::log()), and a picked root opens a state::Conversation on the speaker that
	// speaks its lines (monologues; replies are slice 2). Off by default. Saved: the conversations and the shown-at stamps.
	void enableDialogue(std::shared_ptr<const DialogueBook> book);
	DialogueSystem* dialogue() noexcept { return dialogue_.get(); }
	const DialogueSystem* dialogue() const noexcept { return dialogue_.get(); }
	// A line spoken or a speech cut short (SIMLINK events 41 SPEECH / 42 SPEECH_END).
	struct SpeechEvent
	{
		bool end = false; // SPEECH_END: the bubble closes early (the speaker went down or is gone)
		std::uint64_t speaker = 0; // who speaks (an interjector speaks for the conversation's owner)
		std::uint64_t target = 0;
		std::string dialogue; // sid
		std::string line; // sid
		std::string text; // resolved
		double seconds = 0.0; // the speech timer, or the forced timer when longer
		std::uint8_t flags = 0; // bit 1 interjection, bit 3 large (the event draws a large bubble), bit 4 important (large ones too); bits 0 conversation, 2 spoken by the target: not in slice 1
	};
	void setSpeechObserver(std::function<void(const SpeechEvent&)> observer) { speechObserver_ = std::move(observer); }
	// The character's open conversation, null = none (or not in the arena).
	const state::Conversation* conversation(std::uint64_t id) const;
	// The door step the character is in (state::DoorStep), or nullptr: the CHAR_ROUTE diag tail reports it (status 4).
	const state::DoorStep* doorStep(std::uint64_t id) const;
	// The character's dialogue lock map (step 6), null when nothing was locked or unlocked for it.
	const state::DialogueMemory* dialogueMemory(std::uint64_t id) const;

	// ---- reply menus (DIALOGUE.md 3.5 and 13, slice 2) ----
	// Whether a player-controlled target gets a reply menu (the server turns it on while a client controls the faction; a sim without
	// clients, a soak or a fixture, never opens one, so no menu waits without someone to answer it). Configuration, not saved. Off: the
	// target's lines are skipped (slice 1).
	void setReplyMenus(bool on) noexcept { replyMenus_ = on; }
	bool replyMenus() const noexcept { return replyMenus_; }
	// A newGame world's resident squads promoted in its first seconds walk in from their token (routines::kArriveTicks, TOWN_LIFE_SLICE 5.34);
	// false gives the seeded placement (5.3), which the tests of the placement pass pin. Configuration, not saved.
	void setResidentsArrive(bool walkIn);
	// A NEW_GAME command settles the world before the player sees it (TOWN_LIFE_SLICE 5.40): at the end of the tick that applied it, the world runs
// `seconds` more with the residents walking in (so they are at their sites and busy at the first frame). The clock only moves forward: newGame builds
// the world that much before the start time. Set from SpawnOptions::settleSeconds by newGame; 0 = off. Configuration, not saved.
	void setNewGameSettle(double seconds);
	// Testing only (ksim_server --money): NEW_GAME gives the player faction this many cats instead of the start's `money`.
	// Negative = the start's own. Configuration, not saved (the wallet it sets is).
	void setNewGameMoney(std::int64_t cats) noexcept { newGameMoney_ = cats; }
	// Testing only (ksim_server --give-item): every NEW_GAME's player characters get these items (ITEM record sid, count) in
	// their main inventory, after the start's own gear. Configuration, not saved (the packs it fills are).
	void setNewGameItems(std::vector<std::pair<std::string, std::int32_t>> items) { newGameItems_ = std::move(items); }
	// A menu opened (or re-sent: `resend`, the same seq and texts) or ended (`end`, with `reason`: 0 finished, 2 interrupted, 3 speaker
	// gone, 4 walked away, 5 no client). SIMLINK REPLY_MENU / CONVERSATION_END (pending the wire).
	struct ReplyMenuEvent
	{
		bool end = false;
		bool resend = false;
		std::uint8_t reason = 0;
		std::uint64_t speaker = 0; // the conversation's owner
		std::uint64_t target = 0; // the player character who answers
		std::uint32_t menuSeq = 0;
		std::vector<std::pair<std::string, std::string>> options; // {DIALOGUE_LINE sid ("" = "[...]"), text}
	};
	void setReplyMenuObserver(std::function<void(const ReplyMenuEvent&)> observer) { replyMenuObserver_ = std::move(observer); }
	// The open menu of `speaker`'s conversation as a resend (after a load or a reconnect); nullopt when none is open.
	std::optional<ReplyMenuEvent> replyMenu(std::uint64_t speaker) const;
	// The player's talk (order 60 action 6 on a non-merchant, on arrival): EV_PLAYER_TALK_TO_ME from `actor` to `target`. When it opens
	// nothing (no line passes, the target is busy with no interrupt, gone, or dialogue is off) the talk observer hears (actor, target,
	// seq): event 10 nothing_to_say for the order's clientSeq `seq`. The seq is transient (a save between the talk and its pick drops it).
	void talkTo(std::uint64_t actor, std::uint64_t target, std::uint32_t seq = 0);
	// The right-click entries that apply from `actor` to `target` (SIMLINK order 99 menu_query): bit 0 talk = the root selection a talk would run picks
	// one (a busy speaker: its running line's interrupt root does); with the engine off, a merchant (the direct open). Read-only: no event, no state.
	std::uint32_t menuEntries(std::uint64_t actor, std::uint64_t target);
	void setTalkObserver(std::function<void(std::uint64_t actor, std::uint64_t target, std::uint32_t seq)> observer) { talkObserver_ = std::move(observer); }
	// A client-side window a dialogue asks for (DIALOGUE.md 13.5; kind 1 = trade: a DA_TRADE, run after the dialogue ended; a root without one opens
	// nothing, DIALOGUE.md 13.5): the session of `target` opens it with `speaker`. A merchant's talk (dialogue on) runs the dialogue; this observer is how its
	// trade reply opens the same TRADE record the direct open does. Never saved.
	void setWindowObserver(std::function<void(std::uint64_t speaker, std::uint64_t target, std::uint8_t kind)> observer) { windowObserver_ = std::move(observer); }
	// Every open reply menu as a resend, by speaker Id (a session's first tick after a connect or a load).
	std::vector<ReplyMenuEvent> openMenus() const;
	// The reason a reply (order reply -> Command::DialogueReply) is refused, "" when it applies: "bad_target" (`speaker` has no open menu
	// for `character`), "stale" (the menu's seq is not `menuSeq`, it was answered this tick, or it has no such option).
	std::string checkDialogueReply(std::uint64_t character, std::uint64_t speaker, std::uint32_t menuSeq, std::uint32_t option) const;
	// The reason an end (order end_talk, or the server's forced end -> Command::EndConversation) is refused: "bad_target" (no open menu of
	// `speaker` for `character`). Always allowed otherwise in V1 (DIALOGUE.md 13.9).
	std::string checkEndConversation(std::uint64_t character, std::uint64_t speaker) const;
private:
	std::string runDialogueReply(std::uint64_t character, std::uint64_t speaker, std::uint32_t menuSeq, std::uint32_t option, bool commit);
	std::string runEndConversation(std::uint64_t character, std::uint64_t speaker, std::uint8_t reason, bool commit);
	// Ends every open reply menu addressed to `character` with `reason` (a move order under Hold, DIALOGUE.md 13.4).
	void endMenusOf(std::uint64_t character, std::uint8_t reason);
	ReplyMenuEvent menuEvent(std::uint64_t speaker, const state::Conversation& talk) const;
	// After stepCrime: the producers (every kDialogueProduceSeconds), then the queue.
	// A talk probe (menuEntries): stepDialogue builds its world adapter and answers whether a talk from `actor` to `target` would pick a root,
	// touching nothing (no producers, no queued answers, no event).
	struct TalkProbe
	{
		std::uint64_t actor = 0;
		std::uint64_t target = 0;
		bool passes = false;
	};
	void stepDialogue(std::uint64_t now, TalkProbe* probe = nullptr);
	// stepIndividualGoals carries out the individual goals (individual_goals.cpp) and ends them.
	void stepIndividualGoals();
	bool walkGoal(std::uint64_t id, state::Fighter& f, double x, double z);
	bool rerollFlee(std::uint64_t id, state::Fighter& f, double& outX, double& outZ);
	bool fleePoint(std::uint64_t id, const state::Fighter& f, const state::Fighter& from, double& x, double& z) const;
	void stepRunAwayGoals(); // individual_goals.cpp: 280 RUN_AWAY_FORCED (a Routine named by orderRunAwayGoal)
	void endRunAway(std::uint64_t id);
	struct RunAwayRun
	{
		double x = 0.0; // the point it runs to (re-rolled when idle or near)
		double z = 0.0;
		std::uint64_t until = 0; // OURS: the exe's 280 has no end
	};
	std::map<std::uint64_t, RunAwayRun> runAway_; // member -> its 280 run (unsaved: a loaded game's run ends)
public:
	// An individual goal (individual_goals.cpp, DIALOGUE.md 5 step 6b; the dialogue effects give them): replaces the character's task with
	// the goal. False when it does not apply (down, a player character, the target gone, no home).
	bool giveIndividualGoal(std::uint64_t id, state::IndividualKind kind, std::uint64_t target);
	// Paints the navmesh's door polygons from the state::Door entities (open, closed, locked: knav::DoorBook) when they differ
	// from what is painted; the changed tiles get a new tileVersion and a patch (the NAV_POLY_FLAGS source). Runs every tick
	// (twice) and after the scenario spawns its doors; a no-op without navigation or doors.
	void syncDoorPaint();
private:
	// The flecs entity of the fighter with this Id (combat enabled, fighter present).
	flecs::entity fighterEntity(std::uint64_t id) const;

	flecs::world& ecsMutable() noexcept { return world_.ecsMutable(); }
	void attachWilderness(std::shared_ptr<const kdata::GameData> gameData,
		std::unique_ptr<WildernessSpawner> spawner);
	// After instances were added or removed outside the region engine.
	void instancesChanged();
	// The scenario hold (holdScenarioArea): removes the world squads whose token or people stand inside the area and that the file does
	// not own. Runs at the end of every tick, so a squad the spawners or the resident pass built this tick is gone before the next
	// tick's AI; a no-op without a hold.
	void stepScenarioHold();

	World world_;
	std::uint64_t seed_ = 1;
	// Order 52: session key -> the character that session's camera follows. Session state, so it outlives a reload (not in WorldRuntime); not saved.
	std::unordered_map<std::uint64_t, std::uint64_t> cameraFollow_;
	std::array<RngStream, kSubsystemCount> rng_;
	std::vector<SystemState> systems_;
	std::vector<Command> pending_;
	std::unique_ptr<RegionEngine> regions_;
	std::unique_ptr<AiSystem> ai_;
	std::unique_ptr<CombatArena> combat_;
	std::shared_ptr<const MeshNavigator> navigation_;
	std::optional<flecs::query<const state::TownState>> wtTownQuery_;
	std::optional<flecs::query<const state::Door>> doorQuery_; // the doors the sight rays read (a cached query: syncLineOfSight, k16.13.4)
	std::unique_ptr<LineOfSight> los_; // the per-region collision hulls (task AZ); null until enabled
	void rebuildLineOfSight(); // after a building stood: the sight index is built from the placements, so a running one is rebuilt (kenshi-k16.7.156)
	void maybeEnableLineOfSight(); // enables it once the game data, the region grid and the world's placements are all there
	void refreshLineOfSightHook(); // the arena's perception::Environment::lineOfSight, or null when LOS is off
	bool navCoverageNow_ = false; // a SetPlayerAreas command ran this tick: the next syncNavCoverage loads everything (teleport); derived
	double travelX_ = 0.0, travelZ_ = 0.0; // the first player area's centre at the last stepPlayerAreas pass, and the unit heading since (prefetch); derived
	double headX_ = 0.0, headZ_ = 0.0;
	bool travelKnown_ = false;
	bool navPrefetch_ = true; // setNavPrefetch; never saved
	// What a v126 save held of the region promotion's work in flight (RunStates), read by reload and used by the first resident pass, syncNavCoverage and
	// syncLineOfSight that follow; empty = nothing saved: those load everything wanted at once.
	struct PromotionSaved
	{
		std::vector<state::RunStates::Promotion> queue;
		bool nav = false;
		std::vector<std::uint32_t> zones;
		std::vector<std::uint32_t> furnished;
		bool los = false;
		std::vector<std::uint32_t> regions;
	};
	PromotionSaved promotionSaved_;
	void readPromotionSaved();
	std::set<int> furnishedZones_; // zone indices whose doors and containers were spawned (furnishNavCoverage); derived
	std::shared_ptr<const CombatData> combatData_;
	std::vector<flecs::entity_t> combatEntities_; // parallel to the arena's members
	// Scope guard of save / saveGame / stateHash: the ECS Fighters are the arena's until it ends (simulation.cpp ArenaMirror).
	struct ArenaMirror
	{
		explicit ArenaMirror(const Simulation& simulation);
		~ArenaMirror();
		ArenaMirror(const ArenaMirror&) = delete;
		ArenaMirror& operator=(const ArenaMirror&) = delete;
		const Simulation& sim;
		std::vector<state::Fighter> held;
	};
	void refreshJoinGoals(std::uint64_t now);
	CombatSettings combatSettings_;
	std::function<void(const CombatEvent&)> combatObserver_;
	std::function<void(const std::string&)> worldSquadLog_; // empty = off
	void installFightTrace(); // hands the arena the Combat source's hook (decision_trace.hpp), or clears it when the trace is off
	trace::Sink decisionTrace_; // setDecisionTrace; empty = off
	std::function<void(const std::string&)> taskLog_; // the world task lines (setTaskLog); empty = off
	std::uint64_t worldSquadLogTicks_ = 0;
	std::uint64_t worldSquadLogNext_ = 0;
	std::function<void(const InteractEvent&)> interactObserver_;
	std::function<void(const CrimeEvent&)> crimeObserver_;
	struct CaughtAct
	{
		std::uint64_t container = 0;
		std::uint8_t cause = 0;
		bool sent = false;
	};
	std::map<std::uint64_t, CaughtAct> caughtActs_; // per thief while its theft record lives (wire bookkeeping; restore() seeds `sent` from the saved CrimeRecord::announced || committed; an older save has no announced bit)
	std::function<void(const DismissEvent&)> dismissObserver_;
	std::function<void(const crime::DialogueEvent&)> dialogueObserver_;
	std::map<int, std::uint64_t> dialogueCounts_;
	std::unique_ptr<DialogueSystem> dialogue_;
	std::function<void(const SpeechEvent&)> speechObserver_;
	std::function<void(const ReplyMenuEvent&)> replyMenuObserver_;
	bool replyMenus_ = false;
	// Resident promotions before this tick skip the placement pass (5.34); set by newGame, never saved: 0 in a save loaded into a fresh process. A
	// reload keeps it: new_world returns to the new game's start (the walk-in holds again), and a loaded save's clock is past it.
	std::uint64_t arriveUntil_ = 0;
	// The new-game settle (setNewGameSettle): its length in ticks, whether the NEW_GAME command just applied asks for it, and whether it is running
	// (the settle's own ticks never start another).
	std::uint64_t settleTicks_ = 0;
	bool settleDue_ = false;
#if KSIM_PROFILE
	mutable profile::Profiler profiler_;
	std::optional<flecs::query<const state::Fighter>> profileFighters_; // cached: counted every tick
	std::optional<flecs::query<const state::SquadInstance>> profileSquads_;
	void profileTickEnd();
#endif
	bool settling_ = false;
	void settleNewGame();
	bool homeSeatsKnown_ = false; // knowHomeSeats has run for this world; derived, never saved
	std::int64_t newGameMoney_ = -1;
	std::vector<std::pair<std::string, std::int32_t>> newGameItems_; // --give-item
	std::function<void(std::uint64_t, std::uint64_t, std::uint32_t)> talkObserver_;
	std::function<void(std::uint64_t, std::uint64_t, std::uint8_t)> windowObserver_;
	std::shared_ptr<const kdata::GameData> gameData_;
	// researchState()'s book and snapshot, and what they were made from (a pure function of both: nothing to reset on a load).
	mutable std::shared_ptr<const research::Book> researchBook_;
	mutable std::shared_ptr<const research::Snapshot> researchSnapshot_;
	mutable std::vector<std::string> researchFor_;
	mutable const kdata::GameData* researchData_ = nullptr;
	// The scenario's hold (holdScenarioArea, task BM): derived from the file, never saved.
	struct ScenarioHold
	{
		double x = 0.0;
		double z = 0.0;
		double radius = 0.0;
		std::set<flecs::entity_t> keep;
	};
	std::optional<ScenarioHold> scenarioHold_;
	std::unique_ptr<WildernessSpawner> wild_;
	std::shared_ptr<WeatherSystem> weather_;
	std::shared_ptr<BeamField> beams_; // Venge's sky beams, derived from weather_ (never saved)
	std::vector<std::uint64_t> enterable_; // buildings with a door that has a way in, sorted (derived; see buildingAt)
	bool enterableValid_ = false;
	std::unordered_map<std::int64_t, std::vector<std::uint32_t>> buildingGrid_; // building sites by 128-unit cell (derived, see buildingAt)
	bool buildingGridValid_ = false;
	bool removed_ = false; // a RemoveSquad applied this tick
	std::uint64_t badOrders_ = 0;
	TraderStats traderStats_;
	routines::Reservations routineBook_;
	// The Blackboards by squad state::Id: not an entity component, so that adding one never moves squads between archetypes (ARCHITECTURE
	// "archetype churn", TOWN_LIFE_SLICE "deliberately not an entity component", AiSystem::townLife_), and a std::map so walks are in Id order.
	std::map<std::uint64_t, blackboard::Board> boards_;
	// The town circles (towncircle.hpp) with their TownState entities, rebuilt when the number of towns changes.
	mutable std::vector<std::pair<towncircle::Circle, flecs::entity_t>> townCircles_;
	mutable std::size_t townCirclesFor_ = ~std::size_t(0);
	const state::TownState* townStateAt(double x, double z) const;
	flecs::entity_t townEntityAt(double x, double z) const; // the TownState's entity of the town circle holding the point, 0 = none
	// The raid phase packages (raid_phases.cpp): the campaign hook of a campaign squad's Blackboard, and the force's leader squad (the lowest
	// Id of the squads with the same campaign and target and a live order), rebuilt once per selector pass (derived, never saved).
	void refreshRaidLeaders();
	blackboard::Campaign raidCampaign(flecs::entity squad, std::uint64_t squadId) const;
	void watchRaidVictory(flecs::entity squad, std::uint64_t squadId, const blackboard::Situation& situation,
		const std::vector<std::pair<std::uint64_t, flecs::entity>>& members, std::uint64_t now);
	void applyRaidVictory(std::uint64_t now);
	bool raidWon(const state::GroupAI& group, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members) const;
	std::optional<flecs::query<const state::Id, const state::GroupAI>> raidQuery_;
	// dt41 F: what the squad's package classes read of the platoon and its surroundings; `advance` is set (the selector's own tick).
	blackboard::Situation situationOf(flecs::entity squad, const blackboard::Board* board, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members, std::uint64_t nowTick) const;
	// ensureBlackboards walks every squad, so it runs only when the squad set may have changed: instancesChanged, a load (restoreBoards),
	// a different SquadInstance count, or the periodic resync (kBoardResyncTicks) for a birth and a death that cancel in the count.
	bool squadsDirty_ = true;
	std::size_t boardsSquadCount_ = ~std::size_t(0);
	ResidentStats residentStats_;
	TownScorerStats townStats_;
	bool townScorer_ = true;
	bool offscreenMedical_ = false;
	bool offscreenFights_ = false;
	FightStats fightStats_;
	bool turretManning_ = false;
	mutable std::unordered_map<std::uint64_t, std::optional<TurretPost>> turretPosts_; // by door key; derived from the placements, not saved
	bool barTablesDue_ = false; // enableAi set it: the next tick attaches the bar tables, so no squad step runs without them
	bool barBookKnown_ = false; // the book was rebuilt from the loaded claims by the first trader table step (a game with no combat never calls enableCombat's rebuild)
	mutable std::shared_ptr<struct ResidentData> residentData_;
	// characterSquads, called every tick by each session: cached queries (an ecs.each builds one per call)
	mutable std::optional<flecs::query<const state::Id, const state::PlayerSquad>> playerSquadQuery_;
	std::shared_ptr<struct SquadCaches> squadCaches_; // shared with the flecs observers (which outlive a move of this object)
	void registerSquadCaches();
	void touchHomes() noexcept; // a Fighter::homeBuilding was written: the shopSquad cache is stale
	mutable std::optional<flecs::query<const state::Id, const state::Fighter>> fighterIdQuery_;
	mutable const kdata::GameData* squadLimitsFor_ = nullptr; // squadLimits(): the constants of this game data, read once
	mutable SquadLimits squadLimitsCache_;
	struct SensorCache // sensorContext(): the faction map and relation, kept for one tick and one faction count
	{
		std::uint64_t tick = ~std::uint64_t{0};
		std::int64_t factions = -1;
		std::int64_t players = -1;
		sensor::Context ctx;
	};
	mutable SensorCache sensorCache_;
	mutable std::vector<BedView> bedCache_; // bedViewsRef: the beds, in site order, and the (object, slot) each claims
	mutable std::vector<std::pair<std::uint64_t, std::uint16_t>> bedCacheSites_;
	mutable std::uint64_t bedCacheRevision_ = 0; // the SiteIndex revision and ruin count the cache was built for
	mutable std::size_t bedCacheRuins_ = 0;
	bool packsDirty_ = false; // squads were added since the pack animals were last given their backpacks (stepTraders)
	mutable std::shared_ptr<const StartCatalog> starts_;
	mutable std::shared_ptr<const CreatorCatalog> creator_;
	mutable std::shared_ptr<struct ProspectData> prospectData_;
	mutable std::shared_ptr<const producers::Book> producerBook_;
	mutable std::shared_ptr<const trade::Book> tradeBook_;
	std::shared_ptr<economy::Hooks> economyHooks_; // the T4e Lua hooks: process state, never saved
	mutable std::uint64_t tradeBookSeed_ = 0;
	std::shared_ptr<struct ProducerFoliage> producerFoliage_;
	bool producerFoliageFailed_ = false;
	// Whether a building (key) is a skin peeler: its BUILDING_FUNCTIONALITY record named "skin peeler" (function 8, like a cage). Derived cache.
	mutable std::map<std::uint64_t, bool> peelerSites_;
	std::set<std::uint64_t> scenarioLockedCages_;
	enum class ConstructionStep { Working, Done, Refused };
	// One tick of an order; `walk` routes the actor to (x, z) (the world tasks' walkToPoint).
	ConstructionStep stepConstructionTask(std::uint64_t id, ConstructionOrder& order, const std::function<bool(double, double)>& walk);
	void dropRefund(const state::PlayerBuilding& b, std::uint64_t buildingId);
	void stepConstructionOrders(const std::function<bool(std::uint64_t, double, double)>& walk);
	void ensureCrops();
	void stepCrops();
	bool cropEdible(const state::CropPlot& plot) const;
	void stepCropOrders(const std::function<bool(std::uint64_t, double, double)>& walk);
	void stepArrestOrders(const std::function<bool(std::uint64_t, double, double)>& walk);
	void stepSmashOrders(const std::function<bool(std::uint64_t, double, double)>& walk);
	void stepPrisonRelease(); // 211
	void stepAnimalGoals(const std::function<bool(std::uint64_t, double, double)>& walk); // animal_goals.cpp: 238 / 259 / 277 / 278 / 279
	void stepStandingGoals(const std::function<bool(std::uint64_t, double, double)>& walk); // standing_goals.cpp: 19 / 24 / 30 / 62
	void endGoalRoutine(std::uint64_t id, std::uint16_t goal); // the goal's Routine goes (a resident's is handed back to the scorer)
	void stepCrimeGoals(); // crime_goals.cpp: the alarm expiry (14092bfe0), the 209 / 210 dwell, 212 confiscation, 211 / 204 orders
	void noticeAlliesNear(std::uint64_t id); // crime_goals.cpp: the 210 tick's ally notify (1405cb0a0)
	struct CrimeTask; // crime_goals.cpp: one row of the crime goals' handler table
	static const CrimeTask* crimeTask(std::uint16_t task);
	std::uint64_t alarmFinder209(std::uint64_t id) const;
	std::uint64_t alarmFinder210(std::uint64_t id) const;
	bool investigateStep(std::uint64_t id, const state::Fighter& f, state::Routine& r, bool notifyAllies, double dt);
	bool stepInvestigate209(std::uint64_t id, const state::Fighter& f, state::Routine& r, double dt);
	bool stepInvestigate210(std::uint64_t id, const state::Fighter& f, state::Routine& r, double dt);
	bool stepLoot212(std::uint64_t id, const state::Fighter& f, state::Routine& r, double dt);
	bool stepRelease211(std::uint64_t id, const state::Fighter& f, state::Routine& r, double dt);
	bool stepArrest204(std::uint64_t id, const state::Fighter& f, state::Routine& r, double dt);
	// 125 JOB_BUILDER (dt24_101 section 3): the task (71 / 2 / 95) and player building an engineer should work on next; {0, 0} = none.
	std::pair<std::uint16_t, std::uint64_t> planEngineer(std::uint64_t id) const;
	mutable std::shared_ptr<struct BuildingGeometry> buildingGeometry_;
	double buildSpeed_ = 1.0;
	double gameSpeed_ = 1.0;
	std::uint32_t cadence_ = 1; // min(floor(gameSpeed_), 3), at least 1
	mutable std::shared_ptr<struct DiscoveryData> discoveryData_;
	std::vector<std::pair<const void*, std::function<void(const std::shared_ptr<const prospect::Result>&)>>> prospectObservers_;
	std::shared_ptr<const std::string> pristine_; // the markPristine save
	std::vector<std::pair<const void*, std::function<void(const NewGameOutcome&)>>> newGameObservers_;
};

} // namespace ksim
