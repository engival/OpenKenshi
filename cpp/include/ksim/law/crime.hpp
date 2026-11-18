#pragma once

// Crime foundation: records, witnesses, bounties (docs/reference/CRIME_STATIC.md, CRIME_LIVE.md, TRESPASS_LIVE.md,
// STARTS_CAGES_STATIC.md, DIALOGUE_ACTIONS_STATIC.md). The parts the original does in direct code; everything that comes from
// dialogue lines (arrest, cages and jailing, the lines themselves, guards attacking) is NOT here. Where a dialogue action
// would plug in, the code says "HOOK".
//
// A character holds ONE pending crime (state::CrimeRecord): the CrimeEnum id, a timer in seconds, the victim faction. An
// unwitnessed record clears when the timer runs out (a 3 s theft needs a fresh sighting inside its 3 s). A witness (a character
// that perceives the criminal while the record is live and is not friendly with them) refreshes the timer to 20 s, fires a
// dialogue event, and, if it is law enforcement (or military in its own town) of a faction that offers bounties, commits the
// pending crime into a bounty for that faction. Everyone else only fires the dialogue event.
//
// The pure rules are here (tests); the world step is Simulation::stepCrime (crime.cpp). These are per character and per
// faction: nothing assumes one player faction. "Player" is the flag state::Fighter::playerControlled (a cache of the faction
// flag, MULTIPLAYER.md), never an identity.

#include "ksim/core/state.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace kdata { class GameData; }

namespace ksim::crime
{

// ---- CrimeEnum (CrimeEnum.cs; the exe's ids) ----
inline constexpr std::uint8_t kNone = 0;
inline constexpr std::uint8_t kEnslaving = 1; // never committed to a bounty (140853ec0 L18)
inline constexpr std::uint8_t kBurglary = 2; // LOCKPICKING
inline constexpr std::uint8_t kTheft = 3; // STEALING
inline constexpr std::uint8_t kMurder = 4; // no setter in the exe
inline constexpr std::uint8_t kAssault = 5;
inline constexpr std::uint8_t kTreason = 6; // ASSAULT_VIP, no setter
inline constexpr std::uint8_t kSlaveFreeing = 7; // shown as "Terrorism"
inline constexpr std::uint8_t kSmuggling = 8; // no setter
inline constexpr std::uint8_t kTerrorism = 9;
inline constexpr std::uint8_t kLooting = 10;
inline constexpr std::uint8_t kTrespassing = 11;
inline constexpr std::uint8_t kEscapingPrison = 12;
inline constexpr std::uint8_t kFencing = 13;
inline constexpr std::uint8_t kCropTheft = 14;
inline constexpr std::uint8_t kKidnapping = 15;
inline constexpr std::uint8_t kUniformTheft = 16;

// ---- dialogue events (EventTriggerEnum.cs) ----
inline constexpr int kEvThiefCaughtStealingFromMe = 8;
inline constexpr int kEvShooFromMyBuilding = 9;
inline constexpr int kEvIntruderFound = 0x11;
inline constexpr int kEvWitnessGenericAssault = 0x29;
inline constexpr int kEvWitnessLootingAlly = 0x2a;
inline constexpr int kEvWitnessThiefOrLockpick = 0x2b;
inline constexpr int kEvBountySpotted = 0x2c;

// ---- NPC classes (CharacterTypeEnum) ----
inline constexpr int kClassLaw = 1;
inline constexpr int kClassMilitary = 2;
inline constexpr int kClassTrader = 3;
inline constexpr int kClassDiplomat = 5;
inline constexpr int kClassSlave = 6;
inline constexpr int kClassSlaver = 7; // dt24_34: the slaver class goal 34 and 14099b820 never take as a troublemaker
inline constexpr int kClassBandit = 8;
inline constexpr int kClassAdventurer = 9;

// ---- measured / exe constants ----
inline constexpr double kStepSeconds = 1.0; // the crime tick runs about once a second (CRIME_LIVE.md b: dt 1.0)
inline constexpr double kMinTimer = 3.0; // setCrime tops the timer up to 3 s (DAT_14168a254)
inline constexpr double kBumpTimer = 20.0; // a refused (different) pending crime bumps the timer to 20 s
inline constexpr double kWitnessTimer = 20.0; // the witness refresh (DAT_141683188; measured: 20 s)
inline constexpr double kWitnessAgain = 5.0; // witness also runs while crime > 0 and timer > 5 (DAT_141685340)
inline constexpr double kTimerAssault = 30.0; // reportCrime timers
inline constexpr double kTimerTheftReport = 30.0;
inline constexpr double kTimerPutDownKidnap = 10.0; // 1405ce1e0: reportCrime(..., 10, 0xF) when a captive is set down (CRIME_STATIC 15, M)
inline constexpr double kTimerTrespassSensor = 20.0; // run 6: the sensor sets timer 20
inline constexpr double kExpiryHoursPerUnit = 0.04; // DAT_1416fb690: amount 1000 lasts exactly 40 h (measured)
inline constexpr std::int32_t kNeverExpires = 10000; // amounts at or above never expire (the UI: "Notorious")
inline constexpr double kSentenceHoursPerUnit = 0.02; // DAT_1416fbd98
inline constexpr std::int32_t kForeverBelow = 25001; // a bounty at or above this is sentenced FOREVER
inline constexpr double kForeverHours = 999999.0;
inline constexpr double kPrisonTime = 1.0; // GLOBAL CONSTANTS `prison time`
inline constexpr double kBountySpottedRange = 400.0; // sqrt(DAT_14171f330 = 160000)
inline constexpr double kFriendlyRelation = 50.0; // DAT_141683170
inline constexpr double kHostileRelation = -30.0; // DAT_1416ccd2c

// ---- pure rules ----
// The bounty one committed episode of `crime` adds (140851e40).
std::int32_t bountyAmount(std::uint8_t crime) noexcept;
// Hours until a bounty of `amount` expires (140851f10); -1 = never.
double expiryHours(std::int32_t amount) noexcept;
// The sentence in hours for a caged character with this bounty (140854950): int(amount x 0.02 x prison time), FOREVER from 25001.
// HOOK: nothing starts a sentence yet (cages and jailing come from dialogue actions).
double sentenceHours(std::int32_t amount, double prisonTime = kPrisonTime) noexcept;
// The class that counts: a player-controlled character is an adventurer (9) whatever its record says.
int effectiveClass(std::uint8_t npcClass, bool playerControlled) noexcept;
// Law enforcement (140599600): class 1, or class 2 military while in its own town. Such a character cannot be a criminal, and
// is the kind that assigns a bounty on its own.
bool isLaw(int cls, bool inOwnTown) noexcept;
// The bounty at which an observer reacts to a wanted character (1408520c0): law 1, military 2000 (4000 outside its own town),
// adventurer 1000, slave never (INT32_MAX), everyone else 10000.
std::int32_t wantedThreshold(int cls, bool inOwnTown) noexcept;
// The dialogue event a witness fires for `crime` (140854d10). 0 = none. Theft / fencing / looting by a thief whose victim is the
// witness itself is upgraded to EV 8 by the caller (witnessEvent).
int witnessEvent(std::uint8_t crime, bool victimIsWitness) noexcept;
// The witness reaction's case-4 block (140854d10 L348-L368, rwh138 "Betrayal case"): true for the crimes whose branch reaches
// it - theft / looting / fencing fall through to it, and the default branch covers Murder, Assault, Treason, Smuggling,
// Terrorism and Escaping prison. Kidnapping, burglary / slave freeing, trespass, crop theft and uniform theft have their own.
bool witnessBetrayal(std::uint8_t crime) noexcept;

// ---- the steal chance (FUN_1407944d0, kenshi-rwh.163; docs/reference/CRIME_STATIC.md "Steal chance") ----
// Everything the original's chance reads and nothing else: no lighting, observers, line of sight, sneak, item value or building type
// (those stay in the witness pipeline). `thievery` is the RAW stat (Fighter::skills.thievery, stat 10 at CharStats+0xac), no limb or
// hunger multipliers.
struct StealInput
{
	double thievery = 0.0;
	bool character = false; // the source is a character's inventory (pickpocket / loot a body); false = a building or container
	bool disabled = false; // character source only: KO'd, dead, carried or caged
	bool worn = false; // character source: the item is worn / not for sale (item+0x129)
	bool itemIsContainer = false; // character source, worn item that is itself a container (item vf+0x2e0): another x0.75
	double distance = 0.0; // container source: building to the nearest player-squad character
	bool ownPlatoon = false; // container source: the looter's context is the building's (x0.5 on the distance factor)
	bool posture3 = false; // container source: the resolved victim's posture is 3 (x2)
	bool floorMismatch = false; // container source: the victim's floor differs from the building's (x1.5)
};
// The chance that a take passes unnoticed: >= 1 always passes. The roll passes when rand()/32768 <= chance (FUN_140794810).
double stealChance(const StealInput& in) noexcept;
// The roll itself: true when the take is CAUGHT. `draw` is a uniform [0, 1) from the sim rng (rand() / 32768).
inline bool stealCaught(double chance, double draw) noexcept { return draw > chance; }

// The faction that holds this faction's bounties (Faction+0x38 `legal system`; the faction itself when its `offers bounties`
// (+0x40) is set; CRIME_STATIC "legal faction"). Empty for an unknown faction or one with neither.
std::string legalFaction(const kdata::GameData* data, std::string_view factionSid);

// ---- record operations ----
const state::Bounty* findBounty(const state::CrimeRecord& record, std::string_view factionSid) noexcept;
std::int32_t bountyOf(const state::CrimeRecord& record, std::string_view factionSid) noexcept;
// setCrime (140852c80), after the caller's refusals (law criminal, victim is its own faction, a victim faction that is "not
// real"): records `crime` against `victimFaction`. A pending different crime is not overwritten (the timer is bumped to 20 s, and
// false returned); the exception is an assault over a pending crime that was not yet committed to a bounty (UNSURE: the static
// pass says "Assault excepted"; the live run records an assault over a committed theft and adds no second bounty, so a committed
// crime is kept). The committed flag clears when the crime id changes. The timer is topped up to 3 s.
// `diplomatInTerritory` is 140796240(victim, 1) (kinspect dt43): the victim hand resolves to a character whose `NPC class` is 5 and who
// stands in a BIOME_GROUP of its faction's `biomes`. With it an Assault becomes Terrorism (9) and the pending-different-crime refusal
// is skipped (the exe takes the rewrite branch instead of the refusal logic). Other crimes ignore it.
bool setCrime(state::CrimeRecord& record, std::uint8_t crime, std::string_view victimFaction, std::uint64_t victimChar,
	bool diplomatInTerritory = false);
// reportCrime (140852e10): setCrime, and on success the timer is set to `timerSeconds`.
bool reportCrime(state::CrimeRecord& record, std::uint8_t crime, std::string_view victimFaction, std::uint64_t victimChar,
	double timerSeconds, bool diplomatInTerritory = false);
// The witness refresh (140851f40): the timer is raised to `seconds`.
void refreshTimer(state::CrimeRecord& record, double seconds = kWitnessTimer) noexcept;
// Commits the pending crime into the bounty held by `holder` (140853ec0): refused with no crime, when already committed, for a
// criminal of class 8 / 5 / 1, and for Enslaving. Adds the table amount, sets the crime bit, stamps `nowHours`.
bool commit(state::CrimeRecord& record, std::string_view holder, int criminalClass, double nowHours);
// A flat add (140853e20: spawn bounties, DA_ASSIGN_BOUNTY with an amount). Arms the committed flag.
void addFlat(state::CrimeRecord& record, std::string_view holder, std::int32_t amount, double nowHours);
// Erases the bounty `holder` holds (1408539f0: DA_CLEAR_BOUNTY, release, bail-out). True if there was one.
bool erase(state::CrimeRecord& record, std::string_view holder);
// DA_PAY_BOUNTY (140854080 / 140854170): the reward for handing in `holder`'s bounty. When it exists and was not claimed yet the
// claim flag is set and the amount to pay returned (halved for a dead prisoner, `C+0x5bc`); 0 otherwise, so a second hand-in pays
// nothing. The caller credits the bounty hunter's side.
std::int32_t claimReward(state::CrimeRecord& record, std::string_view holder, bool dead) noexcept;
// DA_CLEAR_BOUNTY's other half (setCrime(rec, 0) and the crime timer set to -1): erases the bounty and clears the pending crime.
// True when a bounty was erased. (The exe's 14067aca0 forgiveness of both sides' tags is a HOOK: no long-term tags yet.)
bool forgive(state::CrimeRecord& record, std::string_view holder) noexcept;
// Starts a sentence from `nowHours` (140854950). HOOK for the caging.
void startSentence(state::CrimeRecord& record, std::int32_t amount, double nowHours, double prisonTime = kPrisonTime);
// One crime tick of `dt` seconds (1405cbb80): the timer test comes before the decrement, so a record lives one tick past zero; at
// zero or below the pending crime clears. Then the first expired bounty is erased (one per tick): now - stamp > hours, or
// negative. `inCage` suspends expiry (UNSURE: no cage yet, always false). Returns true when the record holds nothing any more.
bool tick(state::CrimeRecord& record, double dt, double nowHours, bool inCage = false);
// Whether the record holds nothing (no pending crime, no bounty, no sentence) and can be dropped.
bool empty(const state::CrimeRecord& record) noexcept;

// ---- the dialogue hook ----
// What a dialogue event would start. The dialogue engine does not exist yet (docs/DIALOGUE.md): Simulation fires these through
// setDialogueObserver and counts them per event id (dialogueEventCount); nothing else happens. HOOK.
struct DialogueEvent
{
	std::uint64_t tick = 0;
	std::uint64_t speaker = 0; // the character that fires it (the witness)
	std::uint64_t subject = 0; // the target (the criminal)
	int event = 0;
};

} // namespace ksim::crime
