#pragma once

// Runtime save format v36 and the state hash (docs/formats/RUNTIME_SAVE.md).
//
// A save holds the Clock and Ids singletons, every instance entity (one with
// a state::Id) with its state::SavedComponents, every state::SavedRelations
// pair between instance entities, and the state::SavedSingletons present.
// Entity references are save-local indices (entities in Id order), so
// neither the file nor the hash depends on flecs entity ids. Templates are
// not saved: the loader rebuilds them from game data, and the header's load
// order says from what. Pending commands follow the body.
//
// stateHash() is FNV-1a over the same body bytes the save writes.

#include "ksim/command.hpp"

#include <flecs.h>

#include <cstdint>
#include <iosfwd>
#include <stdexcept>
#include <string>
#include <vector>

namespace ksim {

// 65: Fighter moveMode u8 + speedGroup u64 replace the run bool. 66: Fighter hitList + holdSteer, so a load
// mid-swing or mid-hold takes the same decisions. 67: Fighter aimYaw, the technique's look hand (ENGAGEMENT.md 9.4):
// the impact box and the root motion read it, so a mid-swing load resumes the same swing. 68: Fighter slaveState /
// captorSid / cageBuilding / cageSlot, Routine chainTarget / chainElapsed, SquadRoutine cagesLocked (the town slave
// trade, T3e2). 69: the real-economy overlay's components and world flag (TOWN_LIFE_SLICE 7, T4a): EconomySettings,
// TownStock, EconomyProducer, CaravanOrder, SupplyBoard. 70: FactionState::cash, the per-faction money the dialogue
// economy credits (DIALOGUE.md D5, task AK). 71: state::Container key + lock (task AL): the site key, `locked`,
// the lock level and its picking timer. 72: the WorldEventState singleton (task AN): the unique characters known dead and
// the ones the player killed or imprisoned. 73: CaravanOrder postedAt, the order's age clock (T4d), so a carrier that
// never arrives is released after a limit. 74: Fighter::combatGoal, the AI task whose goal picked the current target
// (task AT). 75: Fighter::met, the dialogue memory (task BB): who a character has seen and when, for
// DC_TARGET_LAST_SEEN_X_HOURS_AGO / DC_MET_TARGET_BEFORE. 76: Fighter::restYaw, restRolls and restYawValid, the
// ragdoll's lying pose (task BG), so a body loaded while prone keeps the yaw the client is drawing. 77: the WorldOptions
// singleton (task BT): the new-game settings cod / gdm / ht, which a world carries from its NEW_GAME. 78: Fighter::aggressorSquads,
// the squad map's AGGRESSOR bit (task BZ), which goal 34 reads. 79: Fighter::attackType, the animal's technique family (task BQ).
// 80: unused. 81: Fighter::diceSeed, the character's stable combat dice identity (task CC). 82: Routine::plan / step, the GOAP
// plan the slave chain runs (task CB). 83: Routine searchBuilding / searchPoint / searchLookUntil / searched, the 216 patrol's
// town search (task patrol). 84: state::TakeOut plan / step / liftOrdered / dropSet (task CH): the take-out runs 223's and 224's
// G4 chains; an older save loads with no plan and the executor rebuilds it from the live hands. 85: Fighter::indoors, the
// interior-and-ceiling state (task indoors). 86: Routine::shopping / shoppingDay / shoppingTimer / shoppingLatch / shoppingShop /
// shoppingX-Z, the member's shopping list and the 118/119 transaction timer (task CH group 3); an older save loads with no list and
// builds it at the next evaluation. 87: the LockStates singleton (task locks-save): which cage and shackle locks were picked, cut or forced open,
// their pick progress and remaining hardness; an older save loads every lock untouched. 88: PlayerBuilding ruin / dismantled / furniture / mounted and
// FighterSkills::engineer (task ds/construction); an older save loads every building whole and every character at engineer 1. 89: item provenance
// (task ds/provenance): InventoryEntry planted / stolen and GroundItem::stolen; an older save loads every item clean.
// 90: the CropPlot component (task ds/arrest-2): each farm building's growth and ripe food; an older save loads with none and the plots are
// made, ripe, at the first use. 91: Fighter::captive (task ds/kidnap): the captive-in-transit flag; an older save loads nobody captive.
// 92: the BoardStates singleton (task ds/board-save): each squad Board's runtime state (contract, change-AI table, applied package, running away,
// sticky cage, smell); an older save loads every board rebuilt from the data, as before.
// 93: the TalkHold component (task ds/town-port): a character the player talked to stands still until its tick;
// an older save loads nobody held.
// 94: BoardStates::Entry startedAt / traderState / occupyHeld (task ds/board-pick): the F package classes' per-package state;
// an older save restarts it at the load.
// 95: BoardStates::Entry owned / depot (task ds/slaver-deliver): the platoon's ownership set and the slaver packages' target town; an older
// save loads every squad owning nobody.
// 96: Fighter::ranged and the Crossbows skill (task ds/ranged S1): the carried crossbow's numbers, ammo and reload timer; an older save loads
// nobody armed with one and Crossbows 1.
// 97: Fighter::mount and the Turrets skill (task ds/turret S3): the turret a gunner mans, its post, facing and gun; an older save loads
// nobody mounted and Turrets 1.
// 98: Fighter::stealthKo and the Thievery / Assassination skills (task ds/stealth-ko S4): the stealth KO a character is carrying out; an older
// save loads nobody mid-KO and both skills 1.
// 99: the MemberDelta component (task ds/delta-store, k16.6.51): the persistent subset of a demoted member's Fighter (parts, blood, KO, slave state,
// gear, trained skills), written only for a member that differs from its template build; an older save loads every member untouched.
// 100: CorpseDecay deathTick / cause and MemberDelta's kit block (task ds/corpses, k16.6.52): a body that died while its squad was a token, and the
// first-aid kit a stored medic has started; an older save loads every corpse as one of unknown death and no kit started.
// 101: the MemberSlot component (task ds/member-slot, k16.6.55): a member's build slot, fixed at spawn, so a squadmate leaving the squad does not move
// the others' template dice; an older save loads every member without one (it takes its list index at its first build).
// 102: the StragglerOf component (task ds/stragglers, k16.6.53): the link of a member split off its squad at demotion to the squad it came from; an older
// save has no stragglers.
// 103: MemberDelta's look block (task ds/save-import2, kenshi-cym.2): the name and sex of an imported member whose squad is a token; no new component, an
// older save has no member with the block (its mask bit is never set).
// 104: GroundItem's tint (task ds/limb-tint, kenshi-h2v.13.9): a severed limb's skin colour, blood colour and scale; an older save's limbs load untinted.
// 106: Inventory slot 15, kSheathed (task ds/spectate-stagger, k16.6.124): a weapon sheathed on the back stays equipped in its own slot, not in the pack; no new
// field, an older save has none (its sheathed weapons sit in the pack as carried entries).
// 107: Fighter::slaveStamp and MemberDelta's slave block (task ds/slave-timers, k16.6.126): the world-clock game hour of the last change into slave state 2
// or 3, which the 12 h / 96 h timers run from; an older save loads every slave state 2 / 3 unstamped (stamped at its first look).
// 108: Container's building and piece (task ds/container-piece, k16.7.29): the owning building's doorKey and the layout piece's data index the wire's
// INTERACTABLE kind 3 tail names; an older save's boxes load with 0 / 0xFFFF and the furnish pass fills them in from the data by their key.
// 109: Fighter::walkHeading / walkFacing and the ArrestStates singleton (task ds/save-walk-arrest, k16.6.200): the walk's velocity memory and the guards' arrest
// orders, so a load mid-walk and a load while an arrest waits continue as the unbroken run; an older save loads the memory unset and no arrest order.
// 110: the RunStates singleton (task ds/unsaved-audit, k16.6.202): the animals' limb cooldown and limb play, the alarm board and the investigators' wait, so a
// load mid-run continues as the unbroken run; an older save loads them empty (an alarm gone, a 209 / 210 on it ends, a limb play re-picks).
// 111: Fighter::driveStrafe (task ds/drive-strafe, k16.8.17): order 20's strafe i8, so a load mid-strafe continues the sidestep; an older save loads 0.
// 112: Fighter::passive and Fighter::holdPosition (task ds/passive-hold, k16.6.267): the per-character passive and hold-position toggles; an older save loads both off.
inline constexpr std::uint32_t kSaveVersion = 112;

class SaveError : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

// What a listing shows about a save: the metadata block at the front of the file (RUNTIME_SAVE.md), read without
// restoring a world. readable = the file is this build's version and its head is intact (its recorded size equals the
// real one, so a truncated file is not readable); everything else is then empty / 0. version is 0 when the file is
// not a runtime save at all.
struct SaveInfo
{
	std::uint32_t version = 0;
	bool readable = false;
	std::uint32_t day = 0;
	float hour = 0.0f;
	std::string factionName; // the first player faction ("" = none)
	std::string leaderName; // the first member of its first squad that has one
	std::uint16_t members = 0; // living members of the player faction's squads
	std::int64_t money = 0; // state::PlayerFaction::money of the player faction (0 for a scenario's)
};

// The metadata block the world would be saved with.
SaveInfo describeState(const flecs::world& ecs);
// Reads only the head of a save; fileSize is the file's real byte count. Never throws.
SaveInfo readSaveInfo(std::istream& in, std::uint64_t fileSize);

// The instance state, encoded (the save body).
std::vector<std::uint8_t> encodeState(const flecs::world& ecs);

// version: the layout hashed; a migration test passes the previous one, whose bytes a build of that version hashes the same.
std::uint64_t stateHash(const flecs::world& ecs, std::uint32_t version = kSaveVersion);

// loadOrder: the game-data file names the templates came from, in order.
// pending: queued commands not yet applied (outside the hashed body).
// version: the layout to write; the current one by default. A migration test passes the previous one.
void writeSave(std::ostream& out, const flecs::world& ecs,
	const std::vector<std::string>& loadOrder,
	const std::vector<Command>& pending = {}, std::uint32_t version = kSaveVersion);
void writeSaveFile(const std::string& path, const flecs::world& ecs,
	const std::vector<std::string>& loadOrder,
	const std::vector<Command>& pending = {}, std::uint32_t version = kSaveVersion);

struct SaveLoadResult
{
	std::vector<std::string> savedLoadOrder;
	bool loadOrderMatches = true;
	std::vector<Command> pending;
};

// Reads a save into ecs, which must hold no instances yet (templates are
// fine). A load order other than expectedLoadOrder throws SaveError unless
// force is set; then it loads and reports the mismatch. Throws SaveError on
// a bad magic, an unknown version, an unknown component list or truncation.
SaveLoadResult readSave(std::istream& in, flecs::world& ecs,
	const std::vector<std::string>& expectedLoadOrder, bool force = false);
SaveLoadResult readSaveFile(const std::string& path, flecs::world& ecs,
	const std::vector<std::string>& expectedLoadOrder, bool force = false);

} // namespace ksim
