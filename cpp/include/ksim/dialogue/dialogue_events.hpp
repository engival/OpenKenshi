#pragma once

// Dialogue events, condition evaluation and root selection (docs/DIALOGUE.md section 8 slice 1, steps 3 and 4; the gate
// is DIALOGUE_CONDITIONS_STATIC.md 1.3, the selector DIALOGUE_SELECTION_STATIC.md Q1).
//
// The sim's producers (perception, buildings, the crime hooks) push DialogueEvent messages; DialogueSystem::step sorts the
// queue (tick, speaker, event, target, seq), drops the stale ones, and for each event picks the root and the first line
// the speaker would say: the book's package set of the speaker, its event index, the static pass, then the exe's one gate
// (masks, locked, chance, the reference lists, target type, the condition list in data order) and the selector (candidates
// ordered by static score, the pool of the best passing lines, a uniform pick from a hash). The conversation state machine
// (step 5) is the Simulation's (dialogue_sim.cpp, state::Conversation): it starts one per Picked event, asks next() for each
// following line and speech() for its text and timer.
//
// The evaluator reads the world only through DialogueWorld (a per-step snapshot built by the Simulation), so it is pure
// and testable on its own. Facts the sim does not have yet are caller inputs (DialogueInputs: personality, sex, robot
// limbs, the squad-memory tags) or evaluate to what the sim truthfully has (no reputation value yet). A condition kind
// that is not implemented FAILS CLOSED and is counted per kind (DialogueCoverage); the exe's own no-handler ids pass.
//
// Nothing here is saved or hashed: the queue and the log are transient, the package set id is derived (call-order
// dependent, DialogueBook), the rolls are hashes of (seed, speaker, tick, record).

#include "ksim/dialogue/dialogue_book.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ksim {

// ---- event ids used by the producers (EventTriggerEnum) ----
namespace ev
{
inline constexpr int PlayerTalkToMe = 1;
inline constexpr int ISeeNeutralSquad = 3;
inline constexpr int ThiefCaughtStealingFromMe = 8;
inline constexpr int ShooFromMyBuilding = 9;
inline constexpr int IntruderFound = 17;
inline constexpr int ISeeAnimalSquad = 27;
inline constexpr int WitnessGenericAssault = 41;
inline constexpr int WitnessLootingAlly = 42;
inline constexpr int WitnessThiefOrLockpick = 43;
inline constexpr int BountySpotted = 44;
inline constexpr int ISeeEnemyPlayer = 66;
inline constexpr int ISeeAllyPlayer = 67;
} // namespace ev

// DIALOGUE.md 2.1: the message a producer pushes. `reason` is the crime kind for the witness events (0 otherwise).
struct DialogueEvent
{
	std::uint8_t event = 0;
	std::uint64_t speaker = 0; // state::Id of the character that speaks
	std::uint64_t target = 0; // 0 = none
	std::uint8_t reason = 0;
	std::uint64_t tick = 0;
	std::uint32_t seq = 0; // assigned by push
};

// What a speaker or target is to the gate: one snapshot per character per step.
struct DialogueActor
{
	bool exists = false;
	std::uint64_t id = 0;
	std::string character; // CHARACTER / ANIMAL_CHARACTER sid ("" = a fighter with no CharacterInstance)
	std::string squadTemplate;
	std::string faction; // FACTION sid ("" = none)
	std::string race; // RACE sid
	std::string subrace; // not derivable from the data: "" = unknown
	std::string townFaction; // the faction owning the town the character stands in ("" = none)
	int npcClass = 0; // CharacterTypeEnum (a player-controlled character is an adventurer, 9)
	int personality = 0; // PersonalityTags (an input)
	bool playerFaction = false; // Fighter::playerControlled, the flag (MULTIPLAYER.md): never an identity
	bool animal = false;
	bool leader = true;
	bool female = false; // an input
	bool robotLimbs = false; // an input
	bool trader = false;
	bool dead = false, ko = false, down = false, carried = false;
	bool inCombat = false, sneaking = false, unarmed = false, starving = false;
	// Captivity (TOWN_LIFE_SLICE 5.19): the original's slave-state enum (0 not a slave, 1 slave, 2 escaping, 3 ex-slave) and the
	// faction that chained it; the person in its hand (Fighter::carrying), 0 = none. DC_IS_SLAVE / _ESCAPED_SLAVE /
	// TARGET_IS_SLAVE_OF_MY_FACTION and the CARRYING_BOUNTY pair read them.
	int slaveState = 0;
	std::string captor; // FACTION sid ("" = never chained)
	std::uint64_t carries = 0; // state::Id of the person in hand
	bool running = false; // moving away at a run (IS_RUNNING's reading, UNSURE)
	bool brokenLeg = false, brokenArm = false, damagedHead = false;
	bool inNonPlayerTown = false;
	// Location (the exe's character location state 2, char+0x2f8): `imprisoned` = in a cage (Fighter::cageBuilding != 0, not a held
	// slave), `imprisonedBy` the faction holding it (Fighter::captorSid, else the town owning the cage building). DC_IS_IMPRISONED
	// and the IMPRISONED_BY_* pair read them. `playerTown`: the town the character stands in is a player's (TOWN state or PlayerTown).
	// `inWalls`: inside a walled region (perception.hpp; DC_WITHIN_TOWN_WALLS, an approximation, UNSURE).
	bool imprisoned = false;
	std::string imprisonedBy; // FACTION sid ("" = unknown)
	bool playerTown = false;
	bool inWalls = false;
	// The medical reading of DC_NEARLY_KO / DC_IS_NEARLY_KO (DIALOGUE_CONDITIONS_LIVE (d)): the worst organic part's
	// (health - stun) / max is below 0.1. `shackles`: a locked shackle piece is worn (WEARING_LOCKED_SHACKLES). `cageLocked`:
	// caged, and the warden squad whose home building is the cage has its cages locked (IS_IN_LOCKED_CAGE). `backpacks`: the
	// NUM_BACKPACKS count over the squad (a CONTAINER worn in the BACKPACK slot).
	bool nearlyKO = false;
	bool shackles = false;
	bool cageLocked = false;
	bool houseLocked = false; // the squad AI package's +0xe8 (DC_BUILDING_IS_CLOSED_AND_SECURED): Fighter::houseLocked
	int backpacks = 0;
	double x = 0.0, z = 0.0;
	std::uint64_t building = 0, homeBuilding = 0;
	double stat = 1.0; // the strength-like float STRONGER / WEAKER compare (toughness, Q7 open)
	int squadSize = 1, squadAble = 1;
	bool squadOnlyAnimals = false;
};

// DIALOGUE.md 6: the player faction a conversation's target belongs to, the exe's playerContext. Resolved from T only (a target that
// is not a player faction's character has no context: the money conditions are false, a recruit joins nothing).
struct DialoguePlayerContext
{
	std::int64_t money = 0; // the faction's wallet (cats)
	int roster = 0; // its living squad members
	int cap = 0; // the global faction cap (GLOBAL CONSTANTS `max faction size`)
};

// The squad-memory and threat facts the sim cannot give as a snapshot of the actor.
class DialogueWorld
{
public:
	virtual ~DialogueWorld() = default;
	// Fills `out` for the character; false (out.exists false) when it is gone.
	virtual bool actor(std::uint64_t id, DialogueActor& out) const = 0;
	// DC_PLAYERMONEY and DC_IS_RECRUITABLE's roster cap (DIALOGUE.md 6); false = no player context for `target`.
	virtual bool playerContext(std::uint64_t target, DialoguePlayerContext& out) const { (void)target; (void)out; return false; }
	// DC_TOWN_LEVEL_CURRENT_LOCATION: the size level (0-3) of the player-owned town `speaker` stands in; false = no town, or not a player's.
	virtual bool currentTownLevel(std::uint64_t speaker, int& level) const { (void)speaker; (void)level; return false; }
	// DC_PLAYERS_BEST_TOWN_LEVEL: the highest size level over `target`'s player faction's towns (-1 with none); false = no player context.
	virtual bool bestTownLevel(std::uint64_t target, int& level) const { (void)target; (void)level; return false; }
	// The relation of the faction `from` towards `to`: 100 for the same faction, -100..100.
	virtual double relation(std::string_view from, std::string_view to) const = 0;
	// DC_COPS_AROUND (UNSURE: no radius in the exe; here a law / in-town military character S is aware of).
	virtual bool copsAround(std::uint64_t speaker) const = 0;
	// DC_IS_OUTNUMBERED inputs: how many enemies and able allies S is aware of.
	virtual void threat(std::uint64_t speaker, int& enemies, int& allies) const = 0;
	// When the record (an interned sid of the book) was last shown, for the repetition limit; false = never.
	virtual bool lastShown(std::uint32_t record, std::uint64_t& tick) const { (void)record; (void)tick; return false; }
	// Who speaks an interjection node for the speaker (DIALOGUE.md 3.3, Q10 PROVISIONAL): the nearest able squad mate within talking
	// range that is not in a conversation; 0 = none (the node is skipped).
	virtual std::uint64_t interjector(std::uint64_t speaker) const { (void)speaker; return 0; }
	// The character is in a conversation: a new event for it is dropped (DIALOGUE_SELECTION_LIVE: no queue).
	virtual bool busy(std::uint64_t id) const { (void)id; return false; }
	// The speaker's lock map entry for a root (an interned sid of the book; DA_LOCK_THIS_DIALOG, a line's locks / unlocks): true and `locked`
	// set when it has one, false = the record's own `locked` holds.
	virtual bool lockState(std::uint64_t speaker, std::uint32_t record, bool& locked) const { (void)speaker; (void)record; (void)locked; return false; }
	// A `world state` reference (an interned sid of the book): true and `out` set, false = the state cannot be decided (unknown record).
	virtual bool worldState(std::uint32_t state, bool& out) const { (void)state; (void)out; return false; }
	// The bounty (cats) the faction holds on the character (DC_CARRYING_BOUNTY_*, DC_BOUNTY_AMOUNT_ACTUAL), 0 for none.
	virtual std::int32_t bounty(std::uint64_t id, std::string_view factionSid) const { (void)id; (void)factionSid; return 0; }
	// The speaker's dialogue memory of `target` (the exe's squad memory entry, DIALOGUE_CONDITIONS_STATIC 18 / 20): true and the whole
	// game hours since it last saw it, false when it has no entry (never seen, or evicted).
	virtual bool metHours(std::uint64_t speaker, std::uint64_t target, int& hours) const
	{
		(void)speaker;
		(void)target;
		(void)hours;
		return false;
	}
	// Whether the speaker's sight table holds a live record of `other` (the exe's threat table): the item and carrying lists
	// and DC_HAS_ILLEGAL_ITEM only look at squad members the speaker knows.
	virtual bool aware(std::uint64_t speaker, std::uint64_t other) const
	{
		(void)speaker;
		(void)other;
		return false;
	}
	// The `target carrying character` list (DIALOGUE_CONDITIONS_STATIC 1.3 step 7): some member of the target's squad (the target
	// alone when it has none) carries a person whose CHARACTER is one of `ids`; true and `member` = the carrier. A carrier other
	// than the target must be in the speaker's threat table.
	virtual bool targetSquadCarries(std::uint64_t target, std::uint64_t speaker, std::span<const std::uint32_t> ids, std::uint64_t& member) const
	{
		(void)target;
		(void)speaker;
		(void)ids;
		(void)member;
		return false;
	}
	// The `target has item` list (step 8): some member of the target's squad holds the item record; true and `member` = the holder.
	virtual bool targetSquadHasItem(std::uint64_t target, std::span<const std::uint32_t> ids, std::uint64_t& member) const
	{
		(void)target;
		(void)ids;
		(void)member;
		return false;
	}
	// The `target has item type` list (step 8): some member of the target's squad holds, in the flat main inventory (not a
	// backpack), an item whose `item function` equals the FIRST listed record's (140678f30; findings/bread.md "the loot line").
	virtual bool targetSquadHasItemType(std::uint64_t target, std::span<const std::uint32_t> ids, std::uint64_t& member) const
	{
		(void)target;
		(void)ids;
		(void)member;
		return false;
	}
	// DC_HAS_ILLEGAL_ITEM (69): `who` or a squad member the speaker knows holds an item on the law faction's illegal list (the
	// speaker's town's faction, else the speaker's); true and `member` = the first hit (the dialogue retargets to it).
	virtual bool illegalItem(std::uint64_t speaker, std::uint64_t who, std::uint64_t& member) const
	{
		(void)speaker;
		(void)who;
		(void)member;
		return false;
	}
};

// The inputs of a character the sim does not derive yet (the rolled personality, sex, robot limbs).
struct DialogueInputs
{
	int personality = 0; // PersonalityTags; 0 = PT_NONE
	bool female = false;
	bool robotLimbs = false;
};

// What the evaluator could and could not decide. Per condition kind and per reference list.
struct DialogueCoverage
{
	std::array<std::uint64_t, kDialogueCondCount> condEvaluated{};
	std::array<std::uint64_t, kDialogueCondCount> condFailClosed{};
	std::array<std::uint64_t, GlCount> listFailClosed{};
	std::uint64_t worldStateFailClosed = 0; // a world-state reference the sim cannot decide (unknown record)
	std::uint64_t lockedFails = 0; // locked roots / lines (the record's flag, or the speaker's lock map)
	std::uint64_t oneAtATimeIgnored = 0; // gates with the flag (no session state yet: passes)
	std::uint64_t repetitionFails = 0; // gates within their repetition limit
	std::uint64_t noTargetFails = 0; // a T_TARGET condition with no target
	std::uint64_t interjectionsSkipped = 0; // interjection nodes with no interjector
	std::uint64_t replyLinesSkipped = 0; // target-spoken lines with no one to say them (no able target, an interjector slot, T_TARGET_IF_PLAYER for a non-player)
	// The effects of spoken lines (DIALOGUE.md 5, step 6): per DA_* kind, applied or not implemented yet (counted, nothing happens); and the
	// line lists the executor reads that the sim does not apply yet (interrupt, give item, AI contract, change AI, crowd trigger).
	std::array<std::uint64_t, kDialogueActionCount> actApplied{};
	std::array<std::uint64_t, kDialogueActionCount> actSkipped{};
	std::uint64_t lineListsSkipped = 0;
	// The `give item` entries added to a target's inventory (a line-level list, applied with the spoken line).
	std::uint64_t itemsGiven = 0;
	// DA_JOIN_SQUAD_WITH_EDIT joins applied without the character editor: the editor window is the client's (DIALOGUE.md 5, UNSURE: no
	// OPEN_WINDOW frame is sent for it yet).
	std::uint64_t editorWindowsSkipped = 0;
};

// Which condition kinds the evaluator implements (everything else fails closed); the ids that have no handler in the exe
// always pass and are not "implemented".
bool dialogueCondImplemented(int kind) noexcept;
bool dialogueListImplemented(int list) noexcept;

// What a step decided for one event.
struct DialoguePick
{
	enum class Result : std::uint8_t
	{
		Picked, // dialogue / line are set
		NoCandidates, // the speaker's package set roots nothing under the event (or the static pass excluded all)
		NoneSurvived, // candidates existed, none passed the gate
		Stale, // dropped: older than kMaxAgeTicks
		SpeakerGone, // the speaker is gone, down or a player character
		Busy, // dropped: the speaker is in a conversation
	};
	DialogueEvent event;
	Result result = Result::NoCandidates;
	std::uint32_t dialogue = kNoIndex; // the root's dialogue index
	std::uint32_t line = kNoIndex; // the first plain line it would say (a line index)
	std::uint64_t voice = 0; // who says it: the speaker, or the interjector of an interjection node
	std::uint32_t candidates = 0; // roots offered after the static pass
	std::uint32_t passed = 0; // pool size at the pick
};

// The long-term SubjectiveTags hate mask 0x2b0 (LT_STOLE_FROM_ME | LT_MY_CAPTOR | LT_DEFEATED_MY_SQUAD_ONCE | LT_KILLED_MY_FRIEND): DC 53's test and the 276 cage
// finder's prisoner gate (rwh.100, 1405995d0).
inline constexpr std::uint32_t kLongTermHateTags = 0x2b0;

class DialogueSystem
{
public:
	static constexpr std::size_t kEvalsPerTick = 64; // DIALOGUE.md 2.2 (kDialogueEvalsPerTick)
	static constexpr std::uint64_t kMaxAgeTicks = 40; // 2 s at 20 Hz: an older event is dropped
	static constexpr std::size_t kLogCap = 4096;

	DialogueSystem(std::shared_ptr<const DialogueBook> book, std::uint64_t seed);

	const DialogueBook& book() const noexcept { return *book_; }

	// Queues an event (assigns its seq). Events pushed while a step runs wait for the next one.
	void push(DialogueEvent event);
	std::size_t queued() const noexcept { return queue_.size(); }

	// Consumes the queue: sorts it, evaluates at most kEvalsPerTick events, keeps the rest (they age), appends the results to
	// the log and returns this step's. `tick` is the current sim tick.
	std::vector<DialoguePick> step(const DialogueWorld& world, std::uint64_t tick);

	// The next line of a conversation (DIALOGUE.md 3.3, monologues): the selector over the children of `line`, gated for `speaker` (the
	// conversation's owner) and `target`. Picked (line, voice set), NoneSurvived (the conversation ends) or SpeakerGone.
	DialoguePick next(const DialogueWorld& world, std::uint64_t speaker, std::uint64_t target, std::uint32_t dialogue, std::uint32_t line,
		std::uint64_t tick);

	// The interrupt of a busy speaker's conversation (DIALOGUE_SELECTION_LIVE Q3, the wrappers 140685660 / 140685730): `event` evaluated
	// against the one root `dialogue` (the running line's `interrupt`), the speaker's busy state ignored. Picked or NoneSurvived.
	DialoguePick interrupt(const DialogueWorld& world, const DialogueEvent& event, std::uint32_t dialogue);

	// The reply menu after `line` (DIALOGUE_SELECTION_STATIC Q8, 140680130): every child that passes the gate for `speaker` and `target`,
	// in selector order; a T_ME child ends the list as one "[...]" continue entry (kNoIndex). Interjection nodes are not offered (UNSURE).
	// Empty: no menu (the caller ends the conversation, as the exe does). `target` must exist.
	std::vector<std::uint32_t> menu(const DialogueWorld& world, std::uint64_t speaker, std::uint64_t target, std::uint32_t line);

	// What a line says when `voice` speaks it at `tick` (DIALOGUE.md 3.4 step 1): a text variant by hash, the /TOKEN/s of WORD_SWAPS
	// replaced (by a hash of the voice when the swap is `persistent use`, else per utterance; other tokens stay as written), and the speech
	// timer in game hours ((length x 0.12 + 2.0) / 100: 14067fd80 writes hundredths of a game hour; live, line "..." = 2.36 ->
	// 0.0236 game h; kinspect findings/dialogue_pacing).
	struct Speech
	{
		std::string text;
		double hours = 0.0;
	};
	Speech speech(std::uint32_t line, std::uint64_t voice, std::uint64_t tick) const;

	// DialogRepetitionEnum -> game hours (DIALOGUE_SELECTION_LIVE Q4); 0 = no limit.
	static double repetitionHours(int repetition) noexcept;

	// The log of results, oldest first, capped at kLogCap.
	const std::deque<DialoguePick>& log() const noexcept { return log_; }
	void clearLog() { log_.clear(); }

	// The producers' re-fire throttle (SHOO every 2 s, I_SEE every 30 s): true, and noted, when `interval` ticks have passed since this key
	// last fired (or it never did); forgetFired makes the next one due at once. A transient map, not saved.
	static std::uint64_t fireKey(int kind, std::uint64_t a, std::uint64_t b) noexcept { return (a * 0x9e3779b97f4a7c15ull ^ b) * 31 + static_cast<std::uint64_t>(kind); }
	bool due(std::uint64_t fireKey, std::uint64_t tick, std::uint64_t interval);
	void forgetFired(std::uint64_t fireKey) { fired_.erase(fireKey); }
	// The world it served was replaced (Simulation::reload): the queue, the re-fire stamps, the memory, the inputs and the speakers' caches
	// belong to its ids and clock, so they go; the book, the forced line, the coverage and the log are the process's.
	std::size_t worldEntries() const noexcept { return queue_.size() + fired_.size() + memory_.size() + inputs_.size() + speakers_.size(); }
	void resetWorld()
	{
		queue_.clear();
		fired_.clear();
		memory_.clear();
		inputs_.clear();
		speakers_.clear();
		seq_ = 0;
	}

	// Evaluates one event now without touching the queue or the log (tests, tools).
	DialoguePick evaluate(const DialogueWorld& world, const DialogueEvent& event);

	// ---- caller inputs (the seams) ----
	void setInputs(std::uint64_t id, DialogueInputs inputs) { inputs_[id] = inputs; }
	DialogueInputs inputs(std::uint64_t id) const;
	// The speaker's long-term memory bits about the target (LT_*), for the sim's perception tags later.
	void setMemory(std::uint64_t speaker, std::uint64_t target, std::uint32_t tags) { memory_[key(speaker, target)] = tags; }
	bool memory(std::uint64_t speaker, std::uint64_t target, std::uint32_t& tags) const;
	// Testing only (ksim_server --force-dialogue): a speaker of CHARACTER record `character` whose event roots `dialogue` takes it as its
	// only candidate, with the chance rolls of its tree skipped (the rest of every gate still applies). kNoIndex clears it. `log`, when
	// set, hears every evaluation of an event that roots it for such a speaker: picked, or why not (busy, the static pass, a gate step).
	void force(std::string character, std::uint32_t dialogue, std::function<void(const std::string&)> log = {})
	{
		forcedCharacter_ = std::move(character);
		forced_ = dialogue;
		forcedLog_ = std::move(log);
	}

	const DialogueCoverage& coverage() const noexcept { return coverage_; }
	DialogueCoverage& coverageMutable() noexcept { return coverage_; }
	std::uint64_t staleDropped() const noexcept { return staleDropped_; }
	std::uint64_t eventCount(int event) const { return event >= 0 && event < kDialogueEventCount ? eventCounts_[static_cast<std::size_t>(event)] : 0; }

	// The static score of a root or line (DIALOGUE_SELECTION_STATIC Q1.4), and score + the best child's (the sort key).
	int score(const Gate& gate, std::uint8_t speaker, bool interjection, bool locked) const;
	// Whether every condition kind and reference list of the gate is one the evaluator implements.
	bool gateEvaluable(const Gate& gate) const;

private:
	static std::uint64_t key(std::uint64_t a, std::uint64_t b) noexcept { return a * 0x9e3779b97f4a7c15ull ^ b; }

	struct SpeakerCache
	{
		std::string signature;
		PackageSetId set = 0;
		StaticPass pass;
	};
	struct Selection;
	const SpeakerCache& speakerCache(const DialogueActor& speaker);
	void applyInputs(DialogueActor& actor) const;
	int lineKey(std::uint32_t line);
	int rootKey(std::uint32_t dialogue);

	std::shared_ptr<const DialogueBook> book_;
	std::uint64_t seed_;
	std::uint32_t seq_ = 0;
	std::vector<DialogueEvent> queue_;
	std::deque<DialoguePick> log_;
	std::unordered_map<std::uint64_t, DialogueInputs> inputs_;
	std::unordered_map<std::uint64_t, std::uint32_t> memory_;
	std::unordered_map<std::uint64_t, SpeakerCache> speakers_;
	std::string forcedCharacter_;
	std::uint32_t forced_ = kNoIndex;
	std::function<void(const std::string&)> forcedLog_;
	std::unordered_map<std::uint64_t, std::uint64_t> fired_;
	std::vector<int> lineKeys_; // memo of lineKey, INT_MIN = not computed, INT_MAX = in progress
	DialogueCoverage coverage_;
	std::uint64_t staleDropped_ = 0;
	std::array<std::uint64_t, kDialogueEventCount> eventCounts_{};
};

// The roll of a record for a speaker at a tick: a hash, never a stream (results do not depend on call order).
std::uint64_t dialogueRoll(std::uint64_t seed, std::uint64_t speaker, std::uint64_t tick, std::uint32_t record, std::uint32_t salt) noexcept;

} // namespace ksim
