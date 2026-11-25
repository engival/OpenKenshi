#pragma once

// Original save -> our world (docs/reference/SAVE_IMPORT.md "Stage 2", kenshi-cym.2): every PLATOON record of `quick.save` and the content file it names
// (never the folder) becomes a squad with its members: the player's as controlled Fighters, the squads near them as Full-detail world squads, the rest as
// tokens whose members keep their state in MemberDelta and Inventory. Field mapping: SCENARIO_IMPORT.md (scenario::applyChar). The world must be built
// (Simulation::newGame) with combat enabled. Stage 3a (kenshi-cym.3) adds the world-wide state that already has a home in ours: the clock, faction relations
// and prosperity, town owners, completed research, the player's money, bounties and the weather picks (Report::world).

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ksim::saveimport {

struct Options
{
	// Full squads' members (and the player's) are put on the navmesh's nearest walkable ground within 100 u of their saved spot (needs a navigation attached before the
	// import, else no effect); off (--no-snap), they stand where the save put them. Tokens are never snapped.
	bool snapFull = true;
	// A squad becomes Full detail by the original's loaded-zone rule (findings/rwh37, rwh211): its +-platoonBox box overlaps a loaded zone, the 3x3 zones
	// round the camera focus plus the zones a +-characterBox box round each player character overlaps. No radius.
	double platoonBox = 170.0;
	double characterBox = 340.0;
};

struct PlatoonResult
{
	std::string name; // the PLATOON record's
	std::string file; // its `content file`, "" for a never-activated one
	std::string outcome; // player / full / token / token (never activated) / skipped
	std::string reason; // for a skipped one
	std::size_t members = 0;
};

// What stage 3a read from the save and where it landed (SAVE_IMPORT.md "Stage 3a").
struct WorldReport
{
	// CAMERA's clock, applied before any squad is built.
	std::uint64_t day = 0;
	int hour = 0;
	int minute = 0;
	// GAMESTATE_FACTION: relation values set (a pair per record row; self rows skipped) and prosperity values applied.
	std::size_t relations = 0;
	std::size_t relationsNonZero = 0;
	std::size_t factions = 0;
	std::size_t prosperity = 0;
	// GAMESTATE_TOWN records of towns (not nests): the owner set on the town of ours at that placement, the records with no such town (the later
	// placements of a TOWN record, the player's town), owners that differ from the world's own, and the `replacementTown` values read (instance -> TOWN sid;
	// parsed and checked against the data, not applied).
	std::size_t towns = 0;
	std::size_t owners = 0;
	std::size_t ownersChanged = 0;
	std::size_t townsWithoutPlacement = 0;
	std::map<std::string, std::string> replacements;
	// Stage 3b (save v105): the town records that got an ExtraTown (the 137 with no TownState: `townsWithoutPlacement`), how many stand on a townlist
	// placement and how many have an owner; `townsInfo` records whose flags were kept (TownInfo), and of those the public, discovered and explored ones
	// among the towns with a TownState; replacements applied as the active variant and refused (not a TOWN record).
	std::size_t extraTowns = 0;
	std::size_t extraTownsPlaced = 0;
	std::size_t extraTownsOwned = 0;
	std::size_t townsInfo = 0;
	std::size_t townsPublic = 0;
	std::size_t townsDiscovered = 0;
	std::size_t townsExplored = 0;
	std::size_t replacementsApplied = 0;
	std::size_t replacementsRefused = 0;
	// Faction rank (factions with a non-zero one) and the non-zero trust rows kept; the unique NPCs listed (CAMERA `usedUniques#`) and the ones whose
	// handle resolved to an imported character; the characters with a sentence (`sentence` / `psts`).
	std::size_t ranked = 0;
	std::size_t trustRows = 0;
	std::size_t uniques = 0;
	std::size_t uniquesResolved = 0;
	std::size_t sentences = 0;
	// RESEARCH `finished#`: sids kept (level suffixes folded) and entries dropped (not a live RESEARCH record).
	std::size_t research = 0;
	std::size_t researchDropped = 0;
	std::int64_t money = 0;
	bool playerFaction = false; // money and research landed
	// Bounties: characters holding one (CrimeRecord set), the bounty rows, rows of a character that was not imported.
	std::size_t bountyCharacters = 0;
	std::size_t bounties = 0;
	std::size_t bountiesLost = 0;
	// BIOMES slots: read, set (slot -> BIOME_GROUP sid), and the ones left by reason.
	std::size_t weatherSlots = 0;
	std::map<std::size_t, std::string> weatherSet;
	std::map<std::string, std::size_t> weatherLeft;
};

struct Report
{
	WorldReport world;
	std::vector<PlatoonResult> platoons; // one per PLATOON record
	std::map<std::string, std::size_t> byOutcome;
	std::vector<std::string> staleFiles; // platoon files no record names
	std::size_t playerBuildings = 0; // of those, the player faction's own: a PlayerBuilding entity each (owner, state)
	std::size_t playerTowns = 0; // PlayerTown entities made from the player buildings' town hand (townC / townCS)
	std::size_t savedBuildings = 0; // the save's own buildings (a player town) the loaded zones lack, stood in the world (World::addPlacedBuilding)
	std::size_t savedBuildingsNav = 0; // ... of them with collision and a patch bake (needs a native mesh)
	std::size_t savedBuildingTiles = 0; // ... the nav tiles the patch replaced
	std::size_t furnitureCages = 0; // the save's zone placements of a cage building (a Peeler Machine) the loaded zones lack, added as cage sites
	std::size_t generatedRemoved = 0; // the world's own squads removed before the import (Simulation::importClearSquads)
	std::size_t members = 0; // characters imported
	std::size_t membersPlayer = 0;
	std::size_t membersFull = 0;
	std::size_t membersToken = 0;
	std::size_t membersSkipped = 0; // a template the combat data cannot build
	std::size_t dead = 0; // bodies
	std::size_t slaves = 0; // slave state 1..3 imported
	std::size_t captors = 0; // slaves whose `slaver` handle resolved to a captor faction
	// kenshi-cym.14: a player member's saved orders (GAMESTATE_AI `pjob#`, `tjob#`). `ordersSkipped`: task id -> how many orders of it the sim cannot issue
	// (a one-shot that is not a move to a point, a persistent type the jobs list cannot hold, a target that did not resolve).
	std::size_t jobsImported = 0; // persistent rows added
	std::size_t movesImported = 0; // one-shot move orders queued
	std::map<std::uint16_t, std::size_t> ordersSkipped;
	std::size_t unplaced = 0; // Full members with no ground within 100 u when `snapFull`
	// What the save holds that was not applied -> how many. Counted per character unless the key says otherwise.
	std::map<std::string, std::size_t> gaps;
	std::map<std::string, std::size_t> statsUnmapped; // STATS name -> characters that hold it
	std::set<std::string> appearanceSids; // the unresolved hair / head mod sids (distinct)
	std::size_t appearanceOccurrences = 0;
	// The handle map: the original's platoon pool index -> the squad's entity id (the player's: the player squad's), and (platoon pool index, member
	// serial) -> the member's entity id (a skipped or never-activated one has none).
	std::map<std::int32_t, std::uint64_t> squadOfPool;
	std::map<std::pair<std::int32_t, std::int32_t>, std::uint64_t> memberOfHandle;
	// CAMERA: the camera and the selected character (resolved through the handle map; 0 when it is none).
	bool hasCamera = false;
	std::array<double, 3> cameraPos{};
	std::array<double, 4> cameraRot{}; // w x y z
	double cameraZoom = 0.0;
	// CAMERA `floor num` (the floor-slice level, clamped 0..3 as the exe's setter does); hasCameraFloor false = the key is absent (not 0).
	bool hasCameraFloor = false;
	int cameraFloor = 0;
	std::uint64_t selected = 0;
	std::vector<std::uint64_t> selection;
	std::size_t unresolvedHandles = 0; // slaver / selection handles that name no imported member
};

// Imports `dir` (a save folder: quick.save and platoon/). Throws std::runtime_error on a file it cannot read and on every sid the game data does not hold
// (listed together, nothing substituted); the hair / head sids of the two hair mods are counted instead (Report::appearanceSids).
Report importSave(Simulation& sim, const kdata::GameData& gameData, const CombatData& combatData, const std::string& dir, const Options& options = {});

} // namespace ksim::saveimport
