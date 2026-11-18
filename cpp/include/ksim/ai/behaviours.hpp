#pragma once

// The behaviour registry: original AI package (and package family) -> one of
// our behaviours (ARCHITECTURE.md "Version 1": the package name is the
// contract; its steps are ours). The mapping is data: a rule table in a TSV
// (cpp/data/ksim_ai_behaviours.tsv), easy to extend.
//
// Rules, tried in this order for one AI_PACKAGE:
//   1. `package`  key = the package's name (exact, case-insensitive)
//   2. `unloaded` key = the UnloadedPlatoonJob enum name of the package, or
//      of the nearest ancestor (`inherits from`, depth first) that sets one
//      other than NONE. UNSURE: whether the original inherits this field
//      (ORIGINAL_AI.md H10); we assume it does.
//   3. `keyword`  key = a substring of the package's name, then of its
//      ancestors' names (nearest first, `addon` mix-ins skipped); file order.
//      The key `*` matches any name (test tables only).
//   4. otherwise: the generic fallback, reason Unmapped.
// A squad runs the behaviour of the package its platoon's board applies (G1, dt19): the highest `AI packages` priority, ties in data
// order, then the faction's `AI fallback`. Contracts and campaigns are not modelled at template level.
//
// A rule may name a behaviour this build does not implement yet: the squad
// then runs the fallback with reason Planned, so progress shows in the
// coverage report (`ksim_ai_coverage`) and nothing is silently dropped.

#include "ksim/core/state.hpp"
#include "ksim/world/world.hpp"

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ksim {

// Behaviours with an implementation. Fallback is the generic ordinary life.
enum class Behaviour : std::uint8_t
{
	Fallback = 0,
	Travel, // town to town; unloaded TRAVEL_TARGET[_FAST], wander and travel
	GoHome, // return to the home town and idle there; unloaded GOHOME
	StayHome, // a post holder: home-bound (guards, shops, residents)
	PatrolLocal, // a loop of points round the home town (or den), with rests
	AnimalTerritory, // wander within a radius of the home (or den), with rests
	Raid, // raid and assault packages: ordinary life until a faction orders a raid
	ManTheGate, // a gate crew (Gate guard packages): the squad holds its gate's outside point, state::GateCrew
	TradeCircuit, // a wandering trader (TOWN_LIFE_SLICE T2): one of the 4 nearest eligible towns, a U(5, 8) h stay, leaves 06:00-21:00; state::TraderTrip
	ShopKeeper, // a shop's staff (TOWN_LIFE_SLICE T3b2): home-bound like StayHome; at Full detail the town scorer runs their Shopkeeper goal
	ResidentLife, // a town resident (T3b2): home-bound like StayHome; at Full detail the town scorer runs their package
	BarHangout, // `hang out in a bar` (T3d): home-bound like StayHome; the scorer runs RELAX 214 (a claimed table), patrol and pretend SHOPPING 118
	GuardShift, // a guard watch (TOWN_LIFE_SLICE T3e1): home-bound like StayHome; the scorer runs the halves' post / node / bed windows and the intruder pickup (223)
	CageWarden, // a slave shop's warden (T3e2): home-bound like StayHome; the scorer runs slave catching (167), caging (112), watching (183) and the cage lock (195)
};

inline constexpr int kBehaviourCount = 14;

std::string_view behaviourName(Behaviour behaviour) noexcept;

// How a package or squad template resolved.
struct Mapping
{
	Behaviour behaviour = Behaviour::Fallback;
	// None for a real behaviour; Unmapped or Planned for the fallback.
	state::AiReason fallback = state::AiReason::Unmapped;
	std::string wanted; // the behaviour name the rule gave (planned or real)
	std::string rule; // "package", "unloaded", "keyword", "none"
	std::string key; // the rule's key that matched
	std::string package; // the package stringID resolved (squads: the default one)
	std::string packageName;
};

struct BehaviourRule
{
	std::string match; // package | unloaded | keyword
	std::string key;
	std::string behaviour;
	std::string note;
};

// One row of the coverage report.
struct CoverageRow
{
	std::string package; // stringID
	std::string name;
	Mapping mapping;
	std::string routine; // the loaded-routine status (routines.hpp routineStatus): modelled / fallback / none; empty = not filled
	std::size_t squadTemplates = 0; // templates whose default package this is
	std::size_t squads = 0; // spawned squads running it as their default
};

struct Coverage
{
	std::vector<CoverageRow> packages; // by squads desc, then name
	std::size_t squads = 0;
	std::size_t squadsReal = 0;
	std::size_t squadsPlanned = 0;
	std::size_t squadsUnmapped = 0;
	std::size_t squadsNoPackage = 0; // template with no AI package at all
	std::size_t packagesTotal = 0;
	std::size_t packagesReal = 0;
	std::size_t packagesPlanned = 0;
	std::size_t packagesUnmapped = 0;
	// Spawned squads per behaviour name (wanted, planned included).
	std::vector<std::pair<std::string, std::size_t>> byBehaviour;
};

class BehaviourRegistry
{
public:
	// Throws std::runtime_error on an unreadable or malformed table.
	static std::vector<BehaviourRule> readRules(const std::string& path);

	// Resolves every AI_PACKAGE and SQUAD_TEMPLATE prefab of world.
	BehaviourRegistry(const World& world, std::vector<BehaviourRule> rules);

	// The mapping of a squad template's applied package, the G1 board's pick (blackboard.hpp): the highest `AI packages` priority, ties
	// in data order. `fallbackPackage` is the squad's faction `AI fallback` package (its sid, "" = unknown): it applies when the template
	// leaves the ladder empty at priority 0. An unknown template or one with no package is the fallback, Unmapped, rule "none".
	const Mapping& squad(std::string_view templateSid, std::string_view fallbackPackage = {}) const;
	const Mapping& package(std::string_view packageSid) const;

	const std::vector<BehaviourRule>& rules() const noexcept { return rules_; }

	// Counts over the squad instances in ecs (the default world after
	// spawnDefaultWorld).
	Coverage coverage(const flecs::world& ecs) const;

private:
	Mapping resolvePackage(const World& world, flecs::entity package) const;
	Behaviour implemented(std::string_view name) const;

	std::vector<BehaviourRule> rules_;
	std::unordered_map<std::string, Mapping> packages_;
	std::unordered_map<std::string, Mapping> squads_;
	std::unordered_set<std::string> openToFallback_; // templates whose board pick is empty and has no priority-0 package
	Mapping none_;
};

void printCoverage(std::ostream& out, const Coverage& coverage, std::size_t topPackages);

} // namespace ksim
