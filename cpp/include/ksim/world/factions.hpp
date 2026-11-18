#pragma once

// The Faction layer (ARCHITECTURE.md "AI: three layers", "Faction layer, first
// slice"; data and exe: docs/reference/FACTIONS.md). The roll, the lifecycle
// timings, the hostile/ally thresholds and the relation reasons follow the
// exe (FACTIONS.md 2.1, 3.1); the layer above them (intent, targets among the
// other factions' towns, the temperament) is ours and PROVISIONAL.
//
// A faction step runs every game-hour, serially, in faction Id order. It
//   1. reads the events that flowed up since the last step (a squad lost, a
//      town lost or gained: state::FactionEvent) and decays its temperament
//      variables (alarm, grief, confidence);
//   2. follows its raids: squads that left, arrived, came home or failed, and
//      the outcome of each raid (follow-up campaigns, cooldowns); a force
//      whose alive fraction is under 0.2 is defeated;
//   3. scores a small goal set by utility (Consolidate, Defend, Raid, Mourn)
//      over its state, with the intent in hand keeping a fading bonus;
//   4. every 6 game-hours, when it has no running campaign and no queued
//      follow-up and its intent is Raid, rolls each available
//      FACTION_CAMPAIGN (chance per day x 0.25 x range falloff x the raid
//      frequency option) and commissions the one picked by weight: squads
//      from the campaign's `squads to use` are spawned at the faction's
//      nearest town and given a state::RaidOrder (intent, not a command
//      stream; the group layer executes it as GoalKind::Raid and may fail it).
//
// Data (FactionBook, from the merged game data): FACTION `campaigns`, and per
// FACTION_CAMPAIGN the fields of docs/reference/GAMEDATA_SCHEMA.md. The
// relation matrix is the live state::Relation pairs seeded from FACTION
// `relations`, an unlisted pair being the source's `default relation`.
//
// No terminal states: a raid that cannot start, whose squads fail, or whose
// target is gone ends with a cooldown on the campaign and the target and the
// next step scores everything again. Consolidate is always possible.
//
// Determinism: serial; randomness is a hash of (world seed, faction Id, tick,
// salt); spawned members draw from a stream seeded the same way. Nothing
// depends on the worker thread count. All state is state::FactionAI,
// state::FactionEvents and the squads' state::GroupAI::order (saved).

#include "ksim/ai/ai.hpp"
#include "ksim/core/script.hpp"
#include "ksim/core/state.hpp"

#include <flecs.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
}

namespace ksim {

class AreaLookup;
class FactionScript;

// Constants of the exe (FACTIONS.md 2.1, 3.1).
inline constexpr double kUnitsPerMetre = 10.0; // `range near` / `range far` are metres
inline constexpr double kRollHours = 6.0; // each faction rolls its campaigns this often
inline constexpr double kRollShare = 0.25; // that is a quarter of the day: chance per day -> per roll
inline constexpr double kRetreatHours = 3.0; // the "over" state before the force is cleaned up
inline constexpr double kDefeatedFraction = 0.2; // alive fraction under which a force is defeated
// On-site time by campaign `key` (0 assault 4 h, 2 12 h, 3 1 h; any other value behaves as 0).
double onSiteHours(std::int32_t key) noexcept;

// A raid to hand out: the order every squad of the force carries and the faction's record of it.
struct RaidOrder
{
	std::string campaign; // FACTION_CAMPAIGN stringID; empty for a scenario force with none
	state::EntityRef target; // TownState
	state::EntityRef origin; // TownState the force leaves from, when there is one
	std::uint64_t stay = 0;
	std::uint64_t retreat = 0;
	std::uint64_t deadline = 0;
	std::uint64_t now = 0; // issue tick
	std::optional<state::Position> originAt; // where the squads set out; absent = each squad's own position
};

// The one place a raid order is written: GroupAI order (Issued) on every squad, one Raid record and `planned` on the faction's AI. False
// (nothing changed) when a squad has no Position or Id. The faction planner, the scenario loader and the tests all issue through it.
bool issueRaid(state::FactionAI& ai, const RaidOrder& order, const std::vector<flecs::entity>& squads);

// One FACTION_CAMPAIGN, resolved for a faction (docs/reference/FACTIONS.md).
struct CampaignInfo
{
	struct Follow
	{
		std::string campaign;
		double minHours = 0.0;
		double maxHours = 0.0;
		double percent = 100.0;
	};

	std::string sid;
	std::string name;
	std::int32_t key = 0; // WarCampaignEnum: 0 ASSAULT_TOWN, 2 DEFEND_TOWN, 3 TRADER_VISIT
	bool hostile = true;
	bool territorial = false;
	bool targetCharacters = false;
	bool ignoresNoGo = false;
	bool ignoresChanceMults = false;
	// `world state` holds at the initial snapshot (as for squads).
	bool eligible = true;
	std::int32_t numForces = 20;
	double chancePerDay = 0.0;
	// Metres, as in the data; the engine (and our world) compares x kUnitsPerMetre.
	double rangeNear = 0.0;
	double rangeFar = 0.0;
	// `target population min`: a player target needs at least this many members over the player's squads (FACTIONS.md 1).
	std::int32_t targetPopulationMin = 0;
	// `target tech level >=`: a player town's size level (buildings::townLevel) must be at least this, else its chance is 0 (strict <).
	std::int32_t targetTechLevel = 0;
	double repeatDays = 4.0;
	// Templates to raise the force from (`squads to use`, else the nearest
	// ancestor's, else the owner's roaming squads): sid and relative weight.
	std::vector<std::pair<std::string, std::int32_t>> squads;
	bool squadsFromFallback = false;
	std::vector<Follow> onVictory;
	std::vector<Follow> onLoss;

	// The phase packages (rwh.53 section 2.1): the AI_PACKAGE sid of `pt1 AI` (kPhaseAssemble), `pt2 AI` (kPhaseAssault) and `retreat AI`
	// (kPhaseRetreat), for the leader squad and for the others. Each list comes from the record's own refs, else the nearest ancestor's;
	// an empty `others` list takes the leader's (1409c7af0).
	struct Phase
	{
		std::string leader;
		std::string others;
	};
	std::array<Phase, 3> phases;
	// `victory AI leader` / `others`, read the same way. The exe never loads them (dead data, rwh.53 section 4.1); ours honours them behind
	// AiSystem::Config::victoryAi: a force that holds its target through the retreat state takes this package there (FACTIONS.md 2.1b).
	Phase victory;
};

// The campaign instance's states (CampaignInstance, FACTIONS.md 2.1) that carry a package.
inline constexpr std::size_t kPhaseAssemble = 0;
inline constexpr std::size_t kPhaseAssault = 1;
inline constexpr std::size_t kPhaseRetreat = 2;

struct FactionInfo
{
	std::string sid;
	std::string name;
	std::int32_t defaultRelation = 0;
	// Indices into FactionBook::campaigns(), in `campaigns` list order.
	std::vector<std::size_t> campaigns;
	// BIOME_GROUP `index` colours of `biomes` (territory) and `no-go zones`.
	std::vector<std::uint32_t> biomes;
	std::vector<std::uint32_t> noGo;
};

// Immutable faction-layer data built from the merged game data. Configuration
// like the behaviour registry: load a save with the same one.
class FactionBook
{
public:
	static std::shared_ptr<const FactionBook> build(
		std::shared_ptr<const kdata::GameData> gameData);

	const FactionInfo* faction(std::string_view sid) const;
	const CampaignInfo* campaign(std::string_view sid) const;
	const std::vector<CampaignInfo>& campaigns() const noexcept { return campaigns_; }
	const kdata::GameData& data() const noexcept { return *data_; }

	// How many campaigns the faction layer can run on its own: eligible at the
	// default, owned, territorial with a range and a force to raise.
	std::size_t runnable() const;

	// Global constants of the exe (FACTIONS.md 3.1): a relation at or below
	// kHostile is hostile, at or above kAlly allied.
	static constexpr double kHostile = -30.0;
	static constexpr double kAlly = 50.0;

private:
	std::shared_ptr<const kdata::GameData> data_;
	std::vector<CampaignInfo> campaigns_;
	std::unordered_map<std::string, std::size_t> campaignIndex_;
	std::vector<FactionInfo> factions_;
	std::unordered_map<std::string, std::size_t> factionIndex_;
};

// Relation changes by reason (the exe's FactionRelations::vf5, FACTIONS.md 3.1).
// The victim's faction changes its own opinion of the aggressor's. Relations are
// floats in [-100, 100] that stay where events leave them (no decay).
enum class RelationReason : std::uint8_t
{
	Aggression = 1, // the start of a hit: -0.05 x A, floor -20
	Hit = 2, // the hit, attacker not "standing": -2.5 x A, floor -50
	HeavyHit = 3, // the hit, the other branch: -10 x A, floor -50
	Unprovoked = 4, // extra after a hit on a flagged victim: -20 x A, floor -100
	Kill = 5, // a faction member killed: -20 x A, floor -75
	Treatment = 8, // per tick while treating a member: +0.15 x A x dt, ceiling +5
	Capture = 12, // put in a cage (UNSURE): -10 x A, floor -60
	Betrayal = 13, // OURS: the witness case-4 block (140854d10 L348-L368) calls FactionRelations::vf4 directly, no reason code: one -20 per firing
};

// The amount A of a character's action: W x its `faction importance`. W is
// kRelationUnheld for a character with no holder (the only case we model),
// 1.0 for one held by its own ownership slot, 0.6 when held by a friendly
// faction's member (UNSURE what the holder is).
inline constexpr double kRelationUnheld = 0.025;

// The new relation after one change of `reason` (1, 2, 3, 4, 5, 8, 12; any
// other code changes nothing - 13 Betrayal is ours, the witness case-4 write). A negative step applies only while the
// relation is above the reason's floor, a positive one while below its
// ceiling (so one step may overshoot the floor by one delta), then the result
// is clamped to [-100, 100]. dt is the game frame delta for Treatment.
double relationAfter(double relation, RelationReason reason, double amount, double dt = 1.0) noexcept;

// A faction's prosperity (TOWN_LIFE_SLICE 4.5): FACTION `max prosperity` (default 1000) less state::FactionState::prosperityLost. The cap is the
// start value; a dead squad takes trader::kSquadDeathProsperity off it and every faction-layer update gives trader::kProsperityRegen back.
double factionMaxProsperity(const kdata::GameData& data, std::string_view factionSid);
double factionProsperity(const kdata::GameData& data, flecs::entity faction);

// What `from` thinks of `to`: 100 for itself, the live Relation pair, else
// from's `default relation` in the data.
double factionRelation(const kdata::GameData& data, flecs::entity from, flecs::entity to);

// The enemy test between two factions (task BV), THE one place its direction is chosen (FUN_14079c950, kinspect dt11 + dt21): an NPC
// faction's stored relation toward the target's, at or below FactionBook::kHostile, is hostile. A player-faction observer forwards the
// lookup to the target faction's relations, so a player <-> NPC pair reads the NPC's own record toward the player either way (a Dust
// Bandit reads its -10, neutral; a Bloodraider its -100, hostile; the Nameless faction's -100 entry is never consulted). This matches
// dt10's live 0-of-5 unprovoked; sensor::enemy (sim's enemy test) follows the same rule.
bool factionsHostile(const kdata::GameData& data, flecs::entity from, flecs::entity to);

// How the player faction stands with `other`, as the game reports it (kinspect dt21): the NPC faction's own record toward the player,
// never the player's map. UI and relation reports read this. The player map's other entries look seeded from the targets' defaults live
// (UNSURE, docs/FEATURES.md); we do not seed them.
double playerStanding(const kdata::GameData& data, flecs::entity player, flecs::entity other);

// victim changes its opinion of aggressor by `reason` and writes the pair.
// Returns the new value. Nothing happens (returns the current value) for the
// same faction or an invalid entity.
double applyRelationReason(const kdata::GameData& data, flecs::entity victim,
	flecs::entity aggressor, RelationReason reason, double amount, double dt = 1.0);

// Pushes a RelationCrossed event to victim's faction for each of the exe's thresholds
// (hostile at or below -30, ally at or above 50) that before -> after crossed. Called by
// applyRelationReason and by the SetFactionRelation command; nothing for a move that
// stays on one side.
void noteRelationChange(flecs::world& ecs, flecs::entity victim, flecs::entity other,
	double before, double after);

// Information flowing up. Producers: Simulation (RemoveSquad = a kill,
// SetTownOwner), later combat and conquest. Consumed by the next faction step.
void pushFactionEvent(flecs::world& ecs, const state::FactionEvent& event);

struct FactionStats
{
	std::uint64_t steps = 0;
	std::uint64_t raidsPlanned = 0;
	std::uint64_t raidsRejected = 0;
	std::uint64_t squadsSpawned = 0;
	std::uint64_t squadsRetired = 0; // removed after Done / Failed
	std::uint64_t eventsConsumed = 0;
	std::uint64_t triggersFired = 0;
	std::uint64_t rolls = 0; // 6-hour roll passes taken (no running campaign, none queued)
	std::uint64_t rollCandidates = 0; // campaigns that had a target and a chance on those passes
	std::uint64_t rollPassed = 0; // ... whose draw passed
	std::uint64_t defeated = 0; // forces under the 0.2 alive fraction
	std::uint64_t territoryRefused = 0; // targets refused by `biomes` / `no-go zones`
	// The faction-intent script (LUA_API.md). Per faction step: woken and applied,
	// not woken (no subscription fired), or fallen back to the native scoring.
	bool scriptLoaded = false;
	std::uint64_t scriptApplied = 0;
	std::uint64_t scriptSleeping = 0;
	std::uint64_t scriptFallbacks = 0; // = the five below
	std::uint64_t scriptErrors = 0;
	std::uint64_t scriptBudget = 0; // instruction budget
	std::uint64_t scriptMemory = 0; // memory budget
	std::uint64_t scriptUnusable = 0; // result could not cross, or its commands were refused
	std::uint64_t scriptMissing = 0;
	std::uint64_t scriptEvents = 0; // events delivered to the script
	std::size_t scriptPeakBytes = 0;
	std::array<std::uint64_t, state::kIntentCount> intentSteps{};
};

// The faction-intent script (a source text; the caller reads the file). An empty
// source: no script, the native scoring.
struct FactionScriptSetup
{
	std::string name; // for messages: the file name
	std::string source;
	ScriptLimits limits;
	// Receives every message (load failures, rate-limited call failures). Empty: stderr.
	std::function<void(std::string_view)> log;
};

class UtilityFactionLayer final : public FactionLayer
{
public:
	// book and navigator may be null (a faction then only scores Consolidate /
	// Defend / Mourn: no raid is possible). towns must outlive the layer.
	UtilityFactionLayer(std::shared_ptr<const FactionBook> book, const Navigator* navigator,
		const std::vector<TownSite>& towns, std::uint64_t seed, FactionTuning tuning = {},
		std::shared_ptr<const AreaLookup> areas = nullptr, FactionScriptSetup script = {});
	~UtilityFactionLayer() override;

	FactionResult update(FactionContext& context) override;

	const FactionStats& stats() const noexcept { return stats_; }

private:
	struct Candidate;

	void ensure(FactionContext& context, state::FactionAI& ai) const;
	// Counts this faction's events (bookkeeping; the temperament is decide's).
	void absorb(FactionContext& context, state::FactionAI& ai);
	// The temperament and the intent: the script's, or when it is absent, errs, runs
	// out of budget or returns something unusable, the native scoring below.
	void decide(FactionContext& context, state::FactionAI& ai, bool canRaid);
	void nativeDecide(FactionContext& context, state::FactionAI& ai, bool canRaid);
	bool follow(FactionContext& context, state::FactionAI& ai);
	void conclude(FactionContext& context, state::FactionAI& ai, const state::Raid& raid,
		bool defeated);
	bool inTerritory(const FactionInfo& info, const CampaignInfo& campaign, double x, double z) const;
	// The commissionable campaigns: due follow-ups, and on a roll pass the
	// territorial ones with a target and a chance.
	std::vector<Candidate> candidates(FactionContext& context, const state::FactionAI& ai,
		bool rolling);
	bool commission(FactionContext& context, state::FactionAI& ai, const Candidate& candidate);
	double roll(const FactionContext& context, std::uint64_t salt) const;

	std::shared_ptr<const FactionBook> book_;
	const Navigator* navigator_;
	const std::vector<TownSite>& towns_;
	std::uint64_t seed_;
	FactionTuning tuning_;
	std::shared_ptr<const AreaLookup> areas_;
	std::unique_ptr<FactionScript> script_;
	FactionStats stats_;
};

} // namespace ksim
