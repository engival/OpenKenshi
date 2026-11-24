#pragma once

// The three AI layers (ARCHITECTURE.md "AI: three layers, no terminal
// states"): Faction -> Group -> Individual, as thin interfaces. The Faction
// layer is UtilityFactionLayer (NoFaction only without a FactionBook) and the
// Group layer is UtilityGroupLayer. There is no Individual interface: the
// decision-source registration design is kenshi-k16.6.40's (ARCHITECTURE.md).
// Per-character decisions run in Simulation, not here: the platoon's AI
// package stack (blackboard.cpp, G1) gives the goal rows; the goal selector
// (goalsel.cpp) scores them; the town scorer (routine_scorer.cpp scoreMember,
// from stepResidents) and the world-squad tasks (world_tasks.cpp, from
// stepWorldSquads) carry the winner out, with GOAP chains (goap.cpp,
// goap_sim.cpp); combat (combat.cpp), dialogue orders (individual_goals.cpp)
// and crime goals (crime_goals.cpp) run beside them. Map: docs/ARCHITECTURE.md
// "AI: three layers".
//
// Group layer: a squad holds one goal (state::GroupAI). Goals are picked by
// utility, with a commitment bonus that fades for the goal in hand and a
// cooldown (blacklist) for goals that failed. Idle is always possible and
// never blacklisted, so a step can always pick something: no terminal state.
// A goal that ends, fails (no route, stuck) or is replaced is followed by a
// new pick in the same step.
//
// The utility numbers and the speed are UNSURE guesses (ai.cpp), not data.
//
// Determinism: the group step is serial, in squad Id order, and reads only
// saved state plus the (immutable) behaviour registry and navigator. Its
// randomness is a hash of (world seed, squad Id, tick, salt), so no stream
// is shared and nothing depends on the worker thread count.

#include "ksim/ai/behaviours.hpp"
#include "ksim/nav/doors.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/region.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/script.hpp"
#include "ksim/core/state.hpp"
#include "ksim/economy/trader.hpp"

#include <flecs.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace ksim {

class AreaLookup;
class FactionBook;
class UtilityFactionLayer;
struct FactionStats;

// The tunables (guesses, UNSURE; docs/reference/FACTIONS.md).
struct FactionTuning
{
	// The exe's `raidFrequencyMult` option (FACTIONS.md 2.1): multiplies every
	// campaign's roll chance, `ignores chance mults` campaigns excepted.
	double raidFrequency = 1.0;
	// The exe's `raidSizeMult` option: the force is int(num forces x this).
	double raidSizeMult = 1.0;
	// Campaigns a faction may have running at once (the exe: one, and none
	// queued, for the roll to happen).
	std::size_t maxRaids = 1;
	// Cooldowns after a raid fails: its target, and the campaign (ours).
	double failedTargetHours = 48.0;
	double failedCampaignHours = 12.0;
	// After a try that could not be commissioned.
	double rejectedHours = 6.0;
	// Most squads one campaign raises.
	std::size_t maxSquads = 12;
	// world.npc_raids (FACTIONS.md 4; user 2026-11-12: "raids are a player town only type of thing. It's not something that happens between npcs"):
	// off (the original), a campaign's target candidates are towns a player faction owns; on, any other faction's town too (ours).
	bool npcRaids = false;
};


// Which scoring decides the factions' temperament and intent (ARCHITECTURE.md "Behaviour
// layer: Lua"): the default Lua script, or the native C++ code. The native code is also
// the fallback whenever the script cannot decide.
struct BehaviourScripts
{
	enum class Mode : std::uint8_t
	{
		Default, // the environment variable KSIM_BEHAVIOUR=native selects native, anything else the script
		Native,
		Scripted,
	};
	Mode mode = Mode::Default;
	// The script: `factionIntentSource` if not empty (a test's own script), else the file
	// `factionIntentPath`, else the shipped default (data/behaviour/default).
	std::string factionIntentSource;
	std::string factionIntentPath;
	std::string factionIntentName = "faction_intent.lua"; // for messages, with a source
	ScriptLimits limits;
};

// One town a squad may go to.
struct TownSite
{
	std::uint64_t id = 0; // state::Id
	flecs::entity_t entity = 0;
	double x = 0.0;
	double z = 0.0;
	double radius = 0.0; // the exe's Town radius (towncircle.hpp); 0 = not a town a token stands in (a marker town, or no game data)
};

struct FactionContext
{
	std::uint64_t id = 0; // state::Id of the FactionState
	const state::FactionState& faction;
	std::uint64_t tick = 0;
	flecs::world& ecs;
	flecs::entity entity; // the FactionState entity
	// Everything that flowed up since the last step (all factions'; the layer
	// takes its own).
	const std::vector<state::FactionEvent>& events;
	// (state::Id, entity) of every squad, in Id order.
	const std::vector<std::pair<std::uint64_t, flecs::entity_t>>& squads;
};

struct FactionResult
{
	// Squads were added or removed (the region engine and the squad list
	// must be rebuilt).
	bool instancesChanged = false;
};

// Policy and temperament (game-hours). The real layer is UtilityFactionLayer
// (factions.hpp); without a FactionBook this is a stub that does nothing.
class FactionLayer
{
public:
	virtual ~FactionLayer() = default;
	virtual FactionResult update(FactionContext& context) = 0;
};

// The bar tables of a staying wandering trader (TOWN_LIFE_SLICE 5.17, Simulation::attachBarTables). `claim` returns the squad's table (x, z) in the
// town entity, keeping the same claim across calls, or nothing when the town has no table or the simulation has no resident data; `release` gives it
// back when the squad leaves. Both run on the main thread inside squadStep.
struct BarTables
{
	std::function<std::optional<std::array<double, 2>>(flecs::entity squad, flecs::entity_t town)> claim;
	std::function<void(flecs::entity squad)> release;
};

// One candidate of the group layer's goal pick, as the debug view's decision sink sees it (docs/formats/SIMLINK.md "DEBUG VIEW").
// `target` is the scored target (a local goal's point has none); `score` is after the commitment bonus.
struct GoalChoice
{
	state::GoalKind kind = state::GoalKind::None;
	state::EntityRef target;
	double score = 0.0;
};

struct GroupContext
{
	std::uint64_t squadId = 0; // state::Id
	const state::SquadInstance& squad;
	const Mapping& mapping;
	state::GroupAI& ai;
	bool hasPosition = false;
	double x = 0.0; // in: current; out: where the squad stands now
	double z = 0.0;
	bool hasHome = false;
	double homeX = 0.0;
	double homeZ = 0.0;
	// The centre of local goals (patrol loop, territory): the home town, else
	// the wilderness den.
	bool hasAnchor = false;
	double anchorX = 0.0;
	double anchorZ = 0.0;
	// A gate crew (state::GateCrew): the gate's outside point, which the squad holds; postKey is the gate's door key.
	bool hasPost = false;
	double postX = 0.0;
	double postZ = 0.0;
	std::uint64_t postKey = 0;
	// The squad's package row for MAN_THE_GATE 180 (null: its package has none, so the crew offers no post goal), and the hour of day the
	// row's window reads. The post goal is scored through goalsel from this row: tier, weight, window and the task's utility.
	const routines::GoalRow* postRow = nullptr;
	double hour = 0.0;
	std::uint64_t now = 0;
	// The squad's faction layer state (intent flowing down); null without one.
	const state::FactionAI* factionAi = nullptr;
	// Which gate class the squad travels as (doors.hpp); null: class 0 everywhere.
	const DoorAccess* access = nullptr;
	// The squad is built as Fighters (residents.cpp, TOWN_LIFE_SLICE 5.14): its leader walks, so x, z is the leader's position and the
	// route progress (`along`) is a carrot that only advances while the leader keeps up. `holding`: a member is fighting, the carrot waits.
	bool carried = false;
	bool holding = false;
	// A delivering squad's slowest slave on foot (k16.6.22): how far the farthest owned slave that follows its captor stands from the leader.
	// The delivery's carrot waits while it is beyond kSlaveLeash.
	double slaveGap = 0.0;
	// A wandering trader (Behaviour::TradeCircuit): its circuit state, and the world's towns as the pick sees them (what trader::eligibleTown
	// reads), called only when a town is picked; `current` is the town the squad stands in, or null. Both null for any other squad.
	state::TraderTrip* trip = nullptr;
	const std::function<std::vector<trader::TownFacts>(const TownSite* current)>* townFacts = nullptr;
	// The trader's bar tables and its own entity (what `claim` / `release` take); null for any other squad.
	const BarTables* bar = nullptr;
	// A slaver squad shipping its slaves (kenshi-k16.6.14, the package 0xb / 0x1c class): the state::Id of the depot town its Blackboard
	// holds; 0 for any other squad. The squad walks there by the world route and holds there until the hand-over empties its set.
	std::uint64_t depot = 0;
	// The AiSystem::Config decision sink, when set (the debug view): the goal pick and the scored candidates it chose from.
	const std::function<void(std::uint64_t, const GoalChoice&, const std::vector<GoalChoice>&)>* decisionSink = nullptr;
	flecs::entity squadEntity{};
};

struct GroupResult
{
	bool moved = false; // x, z changed
	bool arrived = false;
	bool selected = false; // a new goal was chosen
	std::uint64_t brokeGate = 0; // the door key of a gate the squad bashed down this step (DoorSystem::breakGate)
	// The depot walk found no route this step (kenshi-k16.6.21): why. The squad fell back to its ordinary life and retries after the cooldown.
	state::AiReason deliveryFailed = state::AiReason::None;
	// kenshi-k16.6.21 step 2: with deliveryFailed, 1 = the squad walks toward the mesh point nearest the depot; 2 (alone) = it arrived there and holds.
	std::uint8_t deliveryEdge = 0;
	// Goals that failed this step, by reason.
	std::array<std::uint16_t, state::kAiReasonCount> failures{};
};

// The patrol loop has this many points; the first is the anchor itself. UNSURE.
inline constexpr std::uint32_t kPatrolPoints = 6;
// A patrol loop or territory stays within kLocalLeash radii of its anchor.
inline constexpr double kLocalLeash = 1.5;

// The radius of the behaviour's patrol loop or territory round a home town
// (homed) or a wilderness den; 0 for a behaviour without one. UNSURE: guesses.
double localRadius(Behaviour behaviour, bool homed) noexcept;

// Scenario goal (seconds). The one real layer in this slice.
class GroupLayer
{
public:
	virtual ~GroupLayer() = default;
	virtual GroupResult update(GroupContext& context) = 0;
};

class UtilityGroupLayer final : public GroupLayer
{
public:
	// navigator may be null (every squad then runs the idle fallback).
	// towns is the candidate list in Id order; it must outlive the layer.
	// `tokenSpeeds`: the off-screen token speeds below (the setting ai.token_speeds); off = the flat kWalkSpeed.
	UtilityGroupLayer(const Navigator* navigator, const std::vector<TownSite>& towns,
		std::uint64_t seed, bool tokenSpeeds = false);

	GroupResult update(GroupContext& context) override;

	// The squad's walk speed (world units per sim-second). UNSURE: RACE `walk
	// speed` default 15.0 (fcs.def); the min over members is not taken yet.
	static constexpr double kWalkSpeed = 15.0;
	// Off-screen token speeds (setting ai.token_speeds, COMBAT_OPTIONS.md 7; kenshi-k16.6.45). OURS: the exe's UnloadedPlatoon step
	// sets token speed 55 for move mode 1 and 25 for mode 0, both capped at AbstractMovementBase +0xb4 = 70 (rwh.37), but the original
	// never applies them to a whole leg (rwh.38: unloaded platoons do not walk), so using them for a leg is ours. A token (a squad
	// whose members are not built as Fighters) moves at kTokenTravelSpeed, or kTokenTownSpeed while it stands inside a town's circle.
	static constexpr double kTokenTravelSpeed = 55.0;
	static constexpr double kTokenTownSpeed = 25.0;
	static constexpr double kTokenSpeedCap = 70.0;
	// A squad within this of its home position is "at home". UNSURE.
	static constexpr double kHomeRadius = 300.0;
	// Game-hours without progress along a route before the goal fails. OURS (not from the original).
	static constexpr std::uint64_t kStuckHours = 6;
	// Game-hours a failed goal stays blacklisted. OURS (not from the original).
	static constexpr std::uint64_t kCooldownHours = 24;
	// The edge scan (deliveryEdge): the step along the line to the depot, and the least gain toward it that is worth a walk. UNSURE: guesses.
	static constexpr double kEdgeStep = 200.0;
	static constexpr double kEdgeMinProgress = 300.0;
	// The off-mesh leg (kenshi-k16.7.15): a squad at the mesh edge with an off-mesh goal town goes on as a token, in a straight line at the token
	// travel speed. The leg is a TravelTo goal with no route and a length (the straight distance at the start; `along` is what is walked).
	static bool tokenLeg(const state::GroupAI& ai) { return ai.goal == state::GoalKind::TravelTo && ai.route.empty() && ai.length > 0.0 && ai.target.id != 0; }
	// A leg's token promoted back onto the streamed mesh this far from where the leg started walks on the mesh again (closer, it is still leaving).
	static constexpr double kTokenLegFree = kEdgeStep;

private:
	struct Candidate;

	void select(GroupContext& context, GroupResult& result) const;
	bool adopt(GroupContext& context, const Candidate& candidate, GroupResult& result) const;
	void advance(GroupContext& context, GroupResult& result) const;
	// The end of a leg: the exe's move task is over (it reached its end, or the walk ended as an approach and the mover can
	// go no further). A patrol leg advances its point; a local goal stays in hand for the pick that follows. `approached`:
	// the walk ended short of the route's end (a post is not where the squad stands, so it is not moved there).
	void endLeg(GroupContext& context, GroupResult& result, bool approached) const;
	void fail(GroupContext& context, state::GoalKind kind, state::EntityRef target,
		state::AiReason reason, GroupResult& result) const;
	std::vector<Candidate> candidates(const GroupContext& context) const;
	double jitter(const GroupContext& context, std::uint64_t salt) const;
	// Local goals: a point of the patrol loop, a point of the territory.
	// Empty when the ground there cannot be reached by walking.
	std::optional<state::GroundPoint> patrolPoint(const GroupContext& context,
		std::uint32_t index, double radius) const;
	std::optional<state::GroundPoint> roamPoint(const GroupContext& context,
		std::uint64_t salt, double radius) const;
	// A raid order ends badly: record why, drop the goal (a successor is picked).
	void failOrder(GroupContext& context, state::AiReason reason, GroupResult& result) const;
	// The trade circuit (TOWN_LIFE_SLICE T2). Active for a TradeCircuit squad that can act (a position, the navigation window, the town facts)
	// and has no raid order; everything else is the ordinary group layer. circuitSelect replaces select() for it: arrival is stamped on the
	// no-town -> town transition only, the stay is picked once per arrival, and the next town is uniform among the 4 nearest eligible ones.
	bool circuitActive(const GroupContext& context) const;
	void circuitSelect(GroupContext& context, GroupResult& result) const;
	// The depot walk (kenshi-k16.6.14): true when the squad's goal this step is the delivery (the walk, or the hold inside the depot town).
	// False leaves the ordinary AI the step: no town of that Id, or no route to it (the squad keeps its ordinary life, the package stays).
	bool deliverySelect(GroupContext& context, GroupResult& result) const;
	// A depot the route cannot reach (off the mesh): the walk to the mesh point nearest it along the line from the squad (k16.6.21 step 2).
	// False when no such point is nearer the depot than the squad stands by kEdgeMinProgress.
	bool deliveryEdge(GroupContext& context, const TownSite& site, GroupResult& result) const;
	bool farLeg(GroupContext& context, const TownSite& site, GroupResult& result) const;
	bool offMeshLeg(GroupContext& context, const TownSite& site, GroupResult& result) const;
	// The off-mesh leg: start it toward `site` from where the squad stands, one straight step of it, and the wandering trader's token life (the
	// leg, the stay in an off-mesh town, the next pick). tokenCircuit is true when the squad is in that life this step.
	bool startTokenLeg(GroupContext& context, const TownSite& site, GroupResult& result) const;
	bool tokenStep(GroupContext& context, const TownSite& site, GroupResult& result) const;
	bool tokenCircuit(GroupContext& context, GroupResult& result) const;
	void raidArrived(GroupContext& context, GroupResult& result) const;
	void raidStay(GroupContext& context, GroupResult& result) const;

	const Navigator* navigator_;
	const std::vector<TownSite>& towns_;
	std::uint64_t seed_;
	bool tokenSpeeds_;
	// The token's speed this step: kTokenTownSpeed while (x, z) is inside some town's circle, else kTokenTravelSpeed, capped.
	double tokenSpeed(double x, double z) const;
};

// Runtime counters (not saved: metrics, not state).
struct AiStats
{
	std::uint64_t squadSteps = 0;
	std::uint64_t selections = 0;
	std::uint64_t arrivals = 0;
	std::uint64_t factionCalls = 0;
	std::array<std::uint64_t, state::kAiReasonCount> failures{};
	// Squads that switched to a fallback reason (logged once per change).
	std::array<std::uint64_t, state::kAiReasonCount> fallbackSwitches{};
};

// A snapshot of every squad's AI state (the soak's audit).
struct AiAudit
{
	std::size_t squads = 0;
	std::size_t goalless = 0; // GoalKind::None: must be 0 between steps
	std::array<std::size_t, state::kGoalKindCount> byGoal{}; // by state::GoalKind
	std::array<std::size_t, state::kAiReasonCount> byFallback{};
	std::array<std::size_t, kBehaviourCount> byBehaviour{};
	std::size_t moving = 0; // goals with a route in progress (travel, patrol, roam)
	std::size_t offRoute = 0; // moving squads whose position is not on their route
	// Town life when no player is near (TOWN_LIFE_SLICE 5.3 LOD, T3f): squads by their unloaded phase, and the window-valid leader rows
	// the scorer does not run.
	std::array<std::size_t, routines::kTownPhaseCount> byPhase{};
	std::size_t unmodelled = 0;
};

// Owns the three layers and runs them from Simulation::tick. Cheap when the
// squads have nothing to do: Idle goals cost one comparison per step.
class AiSystem
{
public:
	struct Config
	{
		std::shared_ptr<const BehaviourRegistry> registry;
		std::shared_ptr<const Navigator> navigator; // may be null
		// Faction layer data (factions.hpp). Null: the faction layer is a stub
		// that does nothing (the earlier slices' behaviour).
		std::shared_ptr<const FactionBook> factions;
		FactionTuning factionTuning;
		// Honour the campaigns' `victory AI` (the exe's dead data, rwh.61: 30 campaigns name "campaign victory occupy town" and the exe never
		// loads it): a force holding its target through the retreat state takes that package. Off (the default): the exe's behaviour, the
		// retreat package only. The setting world.campaign_victory_ai (COMBAT_OPTIONS.md 10).
		bool victoryAi = false;
		BehaviourScripts scripts;
		// Town gates (doors.hpp). Null: Simulation::enableAi builds them from the world; an
		// AiSystem made on its own has none. doorTuning.enabled = false switches them off.
		std::shared_ptr<const DoorData> doors;
		DoorTuning doorTuning;
		// The merged game data (towns' type and `is public`, factions' `not real`, `fundamental type`, default relation): the trade circuit's town
		// pick needs it. Simulation::enableAi fills it from the simulation's own when null; without it a TradeCircuit squad runs the fallback.
		std::shared_ptr<const kdata::GameData> data;
		// The areas map for `biomes` / `no-go zones` (FACTIONS.md 2.1). Null:
		// Simulation::enableAi loads it from the world's data directory (left
		// null if that fails); without it territory is not applied.
		std::shared_ptr<const AreaLookup> areas;
		// Receives one line per fallback switch (reason logged). May be empty.
		std::function<void(std::string_view)> log;
		// The debug view's decision sink (docs/formats/SIMLINK.md "DEBUG VIEW"): the group layer's pick for a squad, with the
		// candidates it chose from (the sorted list, the cooling ones included). Called only when set; reads only.
		std::function<void(std::uint64_t, const GoalChoice&, const std::vector<GoalChoice>&)> decision;
		// ai.token_speeds (COMBAT_OPTIONS.md 7): off-screen tokens move at UtilityGroupLayer::kTokenTravelSpeed / kTokenTownSpeed instead of the
		// flat kWalkSpeed. Default off (the flat 15, which is ours today).
		bool tokenSpeeds = false;
	};

	AiSystem(Config config, std::uint64_t seed);

	// The squad interval: group layer for every squad (and the unused
	// individual hook for those at full detail). Between ticks of the engine.
	void squadStep(flecs::world& ecs, RegionEngine& engine, std::uint64_t tick);
	// Simulation::cadenceScale, set before each squadStep (k16.6.633): 1 = the second's cadence.
	void setCadenceScale(std::uint32_t scale, std::set<std::uint64_t> fighting = {}) noexcept { cadenceScale_ = scale; fightingSquads_ = std::move(fighting); }
	// The step period of a squad outside every Full region, in ticks (k16.6.640): 100 = 5 s; 20 = the second's (the 1 Hz baseline, for tests).
	void setFarSquadPeriod(std::uint64_t ticks) noexcept { farSquadPeriod_ = ticks; }
	// The NEW_GAME settle (Simulation::settleNewGame) steps every squad at 1 Hz: it stands for a world that has been running (k16.6.670).
	void setSettling(bool on) noexcept { settling_ = on; }
	// The strategic interval: the faction layer for every faction. Returns true
	// when squads were added or removed (the caller rebuilds the region engine).
	bool strategicStep(flecs::world& ecs, std::uint64_t tick);

	// The wandering trader's table claims (Simulation::attachBarTables sets them before every squad step; they capture the simulation).
	void setBarTables(BarTables tables) { barTables_ = std::move(tables); }
	// The squads shipping slaves, by squad state::Id -> the depot town's state::Id (Simulation::attachSlaveDepots sets them before every squad step).
	void setSlaveDepots(std::map<std::uint64_t, std::uint64_t> depots) { slaveDepots_ = std::move(depots); }
	// A depot walk that found no route (squad state::Id, depot town state::Id, why), one entry per failed attempt since the last call
	// (Simulation logs them: the server prints only the task log).
	struct DeliveryFailure
	{
		std::uint64_t squad = 0;
		std::uint64_t depot = 0;
		state::AiReason reason = state::AiReason::None;
		std::uint8_t edge = 0; // GroupResult::deliveryEdge
	};
	std::vector<DeliveryFailure> takeDeliveryFailures() { return std::exchange(deliveryFailures_, {}); }

	void setFactionLayer(std::unique_ptr<FactionLayer> layer);

	const AiStats& stats() const noexcept { return stats_; }
	// Null unless a FactionBook was configured.
	const FactionStats* factionStats() const noexcept;
	// Null unless door data was configured.
	const DoorSystem* doorSystem() const noexcept { return doors_.get(); }
	// The same for a caller that must change a gate (a raid goal's bash breaks it: breakGate).
	DoorSystem* doorSystem() noexcept { return doors_.get(); }
	AiAudit audit(const flecs::world& ecs) const;
	// Squads whose members are built as Fighters (set by Simulation before squadStep): the squad id -> the leader's position and whether a
	// member is fighting. The group step then reads the position instead of advancing it (the Fighters carry the movement).
	struct Carry
	{
		double x = 0.0;
		double z = 0.0;
		bool holding = false;
		double slaveGap = 0.0; // GroupContext::slaveGap
	};
	std::map<std::uint64_t, Carry>& carried() noexcept { return carried_; }
	const std::map<std::uint64_t, Carry>& carried() const noexcept { return carried_; }
	// Every squad as (state::Id value, entity), in Id order: the cached list the squad step walks (k16.6.232: the per-slice passes use it).
	// The list is re-read when the squad count changed, at the second's first tick (not every tick: the count is a query over every table);
	// a squad that went in between is skipped by its callers (is_alive), one that came waits for the next second, as its first step did.
	using SquadList = std::vector<std::pair<std::uint64_t, flecs::entity_t>>;
	const SquadList& squadIndex(flecs::world& ecs, std::uint64_t tick)
	{
		if (tick % 20 == 0 || squads_.empty() || knownSquads_ == static_cast<std::size_t>(-1)) // an invalidate (a load) is read at once
			refresh(ecs);
		return squads_;
	}
	// The squads whose second-slice is `tick` (id % 20 == tick % 20), in Id order: a tick walks 1/20 of the list, not all of it to skip 19/20.
	const SquadList& squadSlice(flecs::world& ecs, std::uint64_t tick)
	{
		squadIndex(ecs, tick);
		return slices_[tick % 20];
	}
	// The point `lookahead` units past the squad's progress on its route (the carrot the leader walks to); false when it has no route.
	static bool carrot(const state::GroupAI& ai, double lookahead, double& x, double& z);
	// Whether the route's end is reached (progress at the end).
	static bool routeDone(const state::GroupAI& ai) { return !ai.route.empty() && ai.along >= ai.length; }
	const BehaviourRegistry& registry() const noexcept { return *config_.registry; }
	// The squad's applied package mapping: the registry's board pick with the squad's faction `AI fallback` (G1, dt19; blackboard.hpp).
	const Mapping& squadMapping(const flecs::world& ecs, const state::SquadInstance& instance) const;

	// Squads were added or removed: rebuild the squad list at the next step
	// (a count that happens to be the same does not show it).
	const Config& config() const noexcept { return config_; }
	void setVictoryAi(bool on) noexcept { config_.victoryAi = on; }
	void invalidate() noexcept { knownSquads_ = static_cast<std::size_t>(-1); }

private:
	std::uint32_t cadenceScale_ = 1; // Simulation::cadenceScale (k16.6.633)
	std::set<std::uint64_t> fightingSquads_; // squads with a member in a fight this tick: they keep the second at every speed
	std::uint64_t farSquadPeriod_ = 100; // setFarSquadPeriod
	bool settling_ = false; // setSettling
	// The unloaded phase of a squad (TOWN_LIFE_SLICE 5.3 LOD, T3f): what its leader's package says it should be doing at this game
	// hour (routines::TownPhase), with every finder assumed to succeed. Derived, never saved; `unmodelled` is the number of
	// window-valid leader rows the scorer does not run (5.5), the coverage report's "unmodelled goal" counter.
	struct TownLife
	{
		std::uint8_t phase = 0; // routines::TownPhase
		std::uint32_t unmodelled = 0;
	};

	void refresh(flecs::world& ecs);
	// T3f: the squad's unloaded phase, from the leader's list of its package and the clock (5.3 LOD). `package` may be empty (no
	// compiled package: the squad gets no entry). Kept off the entity so that adding it cannot move squads between archetypes and
	// reorder every other system's walk; keyed by the squad's state::Id and pruned with the squad list.
	void updateTownPhase(flecs::entity squad, std::string_view package, double hour);

	Config config_;
	mutable std::map<std::uint64_t, std::string> fallbackPackages_; // by faction entity id: its `AI fallback` package sid ("" = none)
	std::unique_ptr<routines::PackageTable> packages_; // the compiled AI_PACKAGE lists (T3f; null without game data)
	std::map<std::uint64_t, TownLife> townLife_; // by squad state::Id (T3f)
	std::vector<TownSite> towns_;
	// (state::Id, entity) of every squad, in Id order.
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> squads_;
	std::array<std::vector<std::pair<std::uint64_t, flecs::entity_t>>, 20> slices_; // squads_ by id % 20 (refresh)
	std::size_t knownSquads_ = SIZE_MAX;
	std::map<std::uint64_t, Carry> carried_;
	std::unique_ptr<FactionLayer> faction_;
	UtilityFactionLayer* utility_ = nullptr;
	std::unique_ptr<GroupLayer> group_;
	std::unique_ptr<DoorSystem> doors_;
	AiStats stats_;
	BarTables barTables_;
	std::map<std::uint64_t, std::uint64_t> slaveDepots_;
	std::vector<DeliveryFailure> deliveryFailures_;
};

} // namespace ksim
