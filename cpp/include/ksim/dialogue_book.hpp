#pragma once

// The compiled dialogue data (docs/DIALOGUE.md section 1; census and exe behaviour:
// docs/reference/DIALOGUE_DATA.md, DIALOGUE_CONDITIONS_STATIC.md, DIALOGUE_SELECTION_STATIC.md).
//
// A DialogueBook is built once from the merged game data and is immutable, shared like the FactionBook.
// It holds flat arrays of dialogues (roots), lines, conditions and effects, plus the package sets: the
// package list a character gets, flattened through `inheritsFrom`, with the event index
// (event -> candidate roots). Nothing here evaluates a condition or runs a conversation; that is the
// dialogue system built on top (DIALOGUE.md 8, steps 3 on).
//
// Compile rules that matter to a reader of the data (DIALOGUE.md 1.1, 1.3):
//   - A DIALOG_ACTION's role comes from the list that references it (`conditions` reads `condition
//     name`, `effects` reads `action name`); the operand is the reference's v0.
//   - `who` collapses to one bit: T_ME is the speaker, everything else is the line target.
//   - Comparators are normalised per condition kind (CondOp): booleans ignore `compare by`, the
//     "at least" kinds read `==` and `>` as `>=` (so `DC_RELATIONS > 15` is `>= 15`, as in the exe).
//   - DC_HAS_TAG / DC_PERSONALITY_TAG are not conditions at run time: they fold into four masks.
//   - Kinds the exe has no handler for (NONE, FACTION_VARIABLE, ...) compile to CondOp::Pass.
//   - Dropped: lines no root or WORD_SWAPS record reaches, the dead actions (DA_NONE), refs whose
//     target is the wrong record type; every drop is counted in the census.
//
// The package sets are lazily interned (packageSetFor, under a mutex); a PackageSetId is a handle that
// depends on call order, never save it or hash it. A set itself is content-addressed.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
}

namespace ksim {

inline constexpr int kDialogueEventCount = 76; // EventTriggerEnum, EV_NONE 0 .. EV_MAX 75
inline constexpr int kDialogueCondCount = 81; // DialogConditionEnum, DC_NONE 0 .. DC_END 80
inline constexpr int kDialogueActionCount = 60; // DialogActionEnum, DA_NONE 0 .. DA_END 59
inline constexpr std::uint32_t kNoIndex = 0xffffffffu;

// Names of the data's numbering (the FCS C# enums); "?" out of range.
const char* dialogueEventName(int event) noexcept;
const char* dialogueCondName(int kind) noexcept;
const char* dialogueActionName(int kind) noexcept;

// The condition kinds the compiler treats specially or the static pass knows (DialogConditionEnum ids).
namespace dc
{
inline constexpr int None = 0, Relations = 1, PlayerMoney = 2, Reputation = 3, FactionVariable = 6;
inline constexpr int ImprisonedByTarget = 7, ImprisonedByOther = 8, IsATrader = 9;
inline constexpr int FactionRank = 10, PlayerTechLevel = 12, NumDialogEventRepeats = 13, IsImprisoned = 14, ImprisonmentIsDeathrow = 15;
inline constexpr int TargetLastSeenXHoursAgo = 18, IsLeader = 19, MetTargetBefore = 20, HasTag = 23, PersonalityTag = 26;
inline constexpr int TargetSquadSizeDisabled = 34, SquadSize = 35, NumBackpacks = 37, BountyAmountPerceived = 40;
inline constexpr int IsFemale = 45, BountyAmountActual = 47, MyMissionIsFriendly = 51, WithinTownWalls = 57, HasIllegalItem = 69;
inline constexpr int TownLevelCurrentLocation = 73, PlayersBestTownLevel = 74, InAPlayerTown = 75, IsRecruitable = 77, HasRobotLimbs = 79,
	End = 80;
} // namespace dc

// DA_* ids the compiler treats specially.
namespace da
{
inline constexpr int None = 0;
}

// How a condition compares (normalised at compile time, DIALOGUE.md 1.3).
enum class CondOp : std::uint8_t
{
	Pass, // no handler in the exe: true
	Bool, // actual == (value > 0); `compare by` ignored
	Eq, // strict 3-way kinds
	Lt,
	Gt,
	Ge, // the "at least" kinds: `==` and `>` both
};
const char* condOpName(CondOp op) noexcept;

struct CondDef
{
	std::uint8_t kind = 0;
	CondOp op = CondOp::Pass;
	bool onTarget = false; // false: the speaker (T_ME)
	std::int32_t value = 0; // the reference's v0
};

struct ActDef
{
	std::uint8_t kind = 0; // dead actions are already DA_NONE
	std::int32_t operand = 0; // only where the exe reads it, else 0
};

struct Span
{
	std::uint32_t first = 0;
	std::uint32_t count = 0;
};

struct ChildRef
{
	std::uint32_t line = kNoIndex;
	std::uint8_t edgeV0 = 0; // `lines` v0 (0/50/66/100), kept and unused (DIALOGUE_DATA Q2)
};

struct RelChange
{
	std::uint32_t faction = kNoIndex; // interned sid
	std::int32_t amount = 0;
};

struct ItemGive
{
	std::uint32_t item = kNoIndex; // interned sid
	std::int32_t count = 0;
};

struct AiContract
{
	std::uint32_t package = kNoIndex; // interned sid
	std::int32_t hours = 0;
};

struct CampaignTrigger
{
	std::uint32_t campaign = kNoIndex; // interned sid
	std::int32_t minHours = 0, maxHours = 0, chance = 0;
};

struct WorldReq
{
	std::uint32_t state = kNoIndex; // interned sid
	bool want = true; // v0 1 = must be true, 0 = must be false
};

// The reference lists that gate a line or a root (all but conditions and effects). Each is a span of
// interned sids in the book's pool; an empty span means "no restriction".
enum GateList : std::uint8_t
{
	GlInTownOf, // FACTION: the speaker's current town's faction
	GlMyFaction,
	GlTargetFaction,
	GlIsCharacter, // CHARACTER: the speaker's template
	GlTargetCarrying,
	GlHasPackage, // DIALOGUE_PACKAGE
	GlTargetHasItem,
	GlTargetHasItemType,
	GlTargetRace, // OR
	GlNoTargetRace, // AND (declared, unused in the data)
	GlMyRace, // OR
	GlMySubrace, // OR
	GlCount
};
const char* gateListName(int list) noexcept;

// What a root (DIALOGUE) and a line (DIALOGUE_LINE) share: the exe's one gate function (DIALOGUE.md 1.3).
struct Gate
{
	float chancePerm = 100.0f;
	float chanceTemp = 100.0f;
	std::uint8_t repetition = 0; // DialogRepetitionEnum
	std::uint8_t targetType = 0; // CharacterTypeEnum
	bool locked = false;
	bool oneAtATime = false;
	std::int16_t scoreBonus = 0;
	// DC_PERSONALITY_TAG: the speaker's PersonalityTags value must be in `reqPersonality` when it is
	// non-zero (several required entries are an OR) and not in `forbPersonality`. DC_HAS_TAG: every bit
	// of `reqMemory` set and none of `forbMemory` in the speaker's memory of the target. Bit = 1 << tag.
	std::uint32_t reqPersonality = 0, forbPersonality = 0, reqMemory = 0, forbMemory = 0;
	Span conds; // CondDef pool, data order (side effects)
	std::array<Span, GlCount> lists{};
	Span worldState; // WorldReq pool
};

struct LineDef
{
	std::uint32_t sid = kNoIndex; // interned
	Gate gate;
	Span text; // TextRef pool: the alternative wordings
	std::uint8_t speaker = 0; // TalkerEnum
	bool interjection = false;
	Span effects; // ActDef pool
	Span children; // ChildRef pool
	Span relChanges, giveItems, contracts, campaigns; // RelChange / ItemGive / AiContract / CampaignTrigger pools
	Span locks, unlocks, unlockKeepMe; // dialogue indices (u32 pool)
	std::uint32_t interrupt = kNoIndex; // dialogue index
	std::uint32_t crowdTrigger = kNoIndex; // dialogue index
	std::uint8_t crowdPct = 0;
	std::uint32_t changeAi = kNoIndex; // interned AI_PACKAGE sid (the first entry only, the exe ignores the rest)
};

struct DialogueDef
{
	std::uint32_t sid = kNoIndex; // interned
	std::string name;
	bool forEnemies = false;
	bool monologue = true;
	Gate gate;
	Span firstLines; // ChildRef pool
};

struct TextRef
{
	std::uint32_t offset = 0, length = 0;
};

struct WordSwap
{
	std::string token; // the record's name, as written between slashes in a text
	bool persistent = false;
	Span lines; // line indices (u32 pool): each line's texts are the alternatives
};

// Everything the compile saw and dropped (the ksim_dialogue_coverage tool prints it; the numbers the
// DIALOGUE_DATA census gives are asserted against it by ksim_dialogue_book).
struct DialogueCensus
{
	std::size_t dialogues = 0, linesInData = 0, linesCompiled = 0, packages = 0, wordSwaps = 0, actionRecords = 0;
	std::size_t noParentLines = 0; // no ref of any kind (true orphans)
	std::size_t noDialogueParentLines = 0; // no DIALOGUE / DIALOGUE_LINE `lines` entry (the census doc's "orphans")
	std::size_t unreachableLines = 0; // not reachable from a DIALOGUE or WORD_SWAPS record: dropped
	std::size_t multiParentLines = 0, maxParents = 0; // parents among DIALOGUE / DIALOGUE_LINE
	std::size_t unreferencedActions = 0; // DIALOG_ACTION referenced by no record: never compiled
	std::size_t condRefs = 0, condRefsElsewhere = 0; // `conditions` entries of DIALOGUE / DIALOGUE_LINE; of other records
	std::size_t effectRefs = 0;
	std::size_t condRefsNotAction = 0, effectRefsNotAction = 0; // refs whose target is not a DIALOG_ACTION: dropped
	std::size_t condFolded = 0; // HAS_TAG / PERSONALITY_TAG entries folded into masks
	std::size_t condFoldedBadTag = 0; // folded entries whose tag is outside 0..31: dropped
	std::size_t condPass = 0; // entries of kinds with no handler (compile to CondOp::Pass)
	std::size_t condUnknown = 0; // kind outside DialogConditionEnum: compiled as Pass, counted
	std::size_t actNone = 0, actDead = 0, actUnknown = 0; // DA_NONE entries; dead kinds mapped to DA_NONE; out of range
	std::size_t danglingChildren = 0, danglingLinks = 0; // `lines` / locks / unlocks / interrupt / crowd targets that do not resolve
	std::size_t aiPackageActions = 0; // DIALOG_ACTION records used as an AI package (`AI contract`)
	std::array<std::size_t, kDialogueCondCount> condUse{}; // per kind, DIALOGUE + LINE entries (folded ones included)
	std::array<std::size_t, kDialogueActionCount> actUse{}; // per kind, as written in the data (before the dead map)
	std::array<std::size_t, kDialogueEventCount> eventEntries{}; // package `dialogs` entries naming the event
	std::array<std::size_t, kDialogueEventCount> eventDialogues{}; // distinct dialogues rooted by the event
	std::size_t packageEntriesNoEvent = 0; // `dialogs` entries with no event in any slot
	std::size_t packagesInheritCycle = 0, packagesUnfinished = 0;
	std::size_t tokensUsed = 0, tokensWordSwap = 0, tokensUnresolved = 0; // distinct /TOKEN/ names in the texts
	std::vector<std::string> unresolvedTokens; // sorted
};

// A flattened package list: the roots a character with this list can fire, indexed by event.
struct PackageSet
{
	std::vector<std::uint32_t> sources; // packages walked (the list plus everything inherited), sorted; `has package`
	std::vector<std::uint32_t> roots; // dialogue indices, registration order (parents first), unique
	std::array<std::vector<std::uint32_t>, kDialogueEventCount> byEvent; // positions into `roots`, registration order

	std::span<const std::uint32_t> rootsFor(int event) const noexcept;
};
using PackageSetId = std::uint32_t;

// What decides a character's package list (DIALOGUE_SELECTION_STATIC Q5: the first non-empty source wins).
struct SpeakerKey
{
	std::string_view character; // CHARACTER / ANIMAL_CHARACTER sid
	std::string_view squadTemplate; // "" = none
	std::string_view faction; // the speaker's faction sid; "" = unknown
	bool leader = false; // squad leader (a lone character counts as one)
	bool animal = false;
	bool playerFaction = false; // a character of a player faction
};

// The speaker-side facts the static pre-pass needs. The record-derived ones come from
// DialogueBook::characterFacts; the rolled ones (personality, female, robot limbs) are the caller's.
struct SpeakerFacts
{
	std::string_view character;
	std::string_view faction;
	std::string_view race;
	std::string_view subrace; // not derivable from the data; "" = unknown (my-subrace lists then pass)
	int personality = 0; // PersonalityTags value, 0 = PT_NONE
	bool female = false;
	bool robotLimbs = false;
	bool trader = false;
	bool leader = true;
};

// One bit per PackageSet::roots entry: false = statically excluded, true = may still fail at run time.
class StaticPass
{
public:
	StaticPass() = default;
	explicit StaticPass(std::size_t roots)
		: words_((roots + 63) / 64, ~std::uint64_t{ 0 })
		, size_(roots)
	{
	}
	bool test(std::size_t root) const noexcept { return (words_[root / 64] >> (root % 64)) & 1u; }
	void clear(std::size_t root) noexcept { words_[root / 64] &= ~(std::uint64_t{ 1 } << (root % 64)); }
	std::size_t size() const noexcept { return size_; }
	std::size_t count() const noexcept;

private:
	std::vector<std::uint64_t> words_;
	std::size_t size_ = 0;
};

class DialogueBook
{
public:
	static std::shared_ptr<const DialogueBook> build(std::shared_ptr<const kdata::GameData> gameData);

	const DialogueCensus& census() const noexcept { return census_; }

	std::size_t dialogueCount() const noexcept { return dialogues_.size(); }
	std::size_t lineCount() const noexcept { return lines_.size(); }
	const DialogueDef& dialogue(std::size_t index) const { return dialogues_[index]; }
	const LineDef& line(std::size_t index) const { return lines_[index]; }
	// kNoIndex when the sid is not a compiled dialogue / line.
	std::uint32_t dialogueIndex(std::string_view sid) const;
	std::uint32_t lineIndex(std::string_view sid) const;

	// Interned record sids (every sid the lists above refer to).
	std::string_view sidOf(std::uint32_t id) const { return sids_[id]; }
	std::uint32_t idOf(std::string_view sid) const; // kNoIndex when never interned

	std::span<const CondDef> conds(Span span) const { return slice(conds_, span); }
	std::span<const ActDef> effects(Span span) const { return slice(acts_, span); }
	std::span<const ChildRef> children(Span span) const { return slice(children_, span); }
	std::span<const std::uint32_t> ids(Span span) const { return slice(ids_, span); }
	std::span<const WorldReq> worldStates(Span span) const { return slice(worldReqs_, span); }
	std::span<const RelChange> relChanges(Span span) const { return slice(relChanges_, span); }
	std::span<const ItemGive> giveItems(Span span) const { return slice(gives_, span); }
	std::span<const AiContract> contracts(Span span) const { return slice(contracts_, span); }
	std::span<const CampaignTrigger> campaigns(Span span) const { return slice(campaigns_, span); }
	std::string_view text(const TextRef& ref) const { return std::string_view(textPool_).substr(ref.offset, ref.length); }
	std::span<const TextRef> texts(Span span) const { return slice(texts_, span); }
	std::span<const WordSwap> wordSwaps() const { return swaps_; }

	// ---- package sets (DIALOGUE.md 1.2) ----
	// The set a speaker gets. Thread-safe; an unknown record yields the empty set.
	PackageSetId packageSetFor(const SpeakerKey& key) const;
	// A set by name list (the package sids as written in the data), for tools and tests.
	PackageSetId packageSetOf(std::span<const std::string_view> packages, std::span<const std::string_view> extraRoots = {}) const;
	const PackageSet& packageSet(PackageSetId id) const;
	std::size_t packageSetCount() const;
	// The packages `set` was built from as walked (names), for printing.
	std::string_view packageName(std::uint32_t id) const { return sidOf(id); }

	// The record-derived part of a speaker's facts (race, trader flag); the rest is the caller's.
	SpeakerFacts characterFacts(std::string_view characterSid) const;
	// The roots of `set` that survive everything about the speaker that is fixed at spawn (DIALOGUE.md 1.2,
	// 4 lever 3): the personality masks, my race / my faction / is character / has package, and the
	// speaker-side DC_IS_FEMALE / HAS_ROBOT_LIMBS / IS_A_TRADER / IS_LEADER. It only ever excludes: a
	// condition that holds statically is still evaluated at run time, because some (IS_FEMALE) retarget.
	StaticPass staticPass(const PackageSet& set, const SpeakerFacts& facts) const;
	// The same test for one root, with the reason for an exclusion ("" = passes). `packages` = PackageSet::sources.
	std::string staticExclusion(const Gate& gate, const SpeakerFacts& facts, std::span<const std::uint32_t> packages) const;

	const kdata::GameData& gameData() const { return *data_; }

private:
	DialogueBook() = default;
	template <typename T>
	static std::span<const T> slice(const std::vector<T>& pool, Span span)
	{
		return std::span<const T>(pool).subspan(span.first, span.count);
	}

	friend class DialogueCompiler;

	std::shared_ptr<const kdata::GameData> data_;
	DialogueCensus census_;

	std::vector<std::string> sids_;
	std::unordered_map<std::string, std::uint32_t> sidIds_;
	std::vector<DialogueDef> dialogues_;
	std::vector<LineDef> lines_;
	std::unordered_map<std::uint32_t, std::uint32_t> dialogueOf_; // sid id -> dialogue index
	std::unordered_map<std::uint32_t, std::uint32_t> lineOf_;
	std::vector<CondDef> conds_;
	std::vector<ActDef> acts_;
	std::vector<ChildRef> children_;
	std::vector<std::uint32_t> ids_;
	std::vector<WorldReq> worldReqs_;
	std::vector<RelChange> relChanges_;
	std::vector<ItemGive> gives_;
	std::vector<AiContract> contracts_;
	std::vector<CampaignTrigger> campaigns_;
	std::vector<TextRef> texts_;
	std::string textPool_;
	std::vector<WordSwap> swaps_;

	// package id (interned) -> own inheritsFrom list and own (dialogue index, events) entries, in data order
	struct PackageDef
	{
		bool skipped = false; // name contains "Unfinished": the exe skips the package and what it inherits
		std::vector<std::uint32_t> inherits; // interned package sids
		struct Entry
		{
			std::uint32_t dialogue = kNoIndex;
			std::array<std::uint8_t, 3> events{};
		};
		std::vector<Entry> entries;
	};
	std::unordered_map<std::uint32_t, PackageDef> packages_;

	mutable std::mutex setMutex_;
	mutable std::deque<PackageSet> sets_;
	mutable std::unordered_map<std::string, PackageSetId> setIds_;

	PackageSetId internSet(std::span<const std::uint32_t> list, std::span<const std::uint32_t> extraRoots) const;
};

} // namespace ksim
