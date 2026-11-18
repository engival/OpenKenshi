#include "ksim/dialogue_book.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace ksim {

namespace {

// The numbering the data stores (the FCS C# enums; tools/dialogue.py carries the same tables).
constexpr const char* kEventNames[] =
{
	"EV_NONE", "EV_PLAYER_TALK_TO_ME", "EV_ANNOUNCEMENT", "EV_I_SEE_NEUTRAL_SQUAD", "EV_I_SEE_RAGDOLL",
	"EV_______", "EV_SOUND_THE_ALARM", "EV_I_________", "EV_THIEF_CAUGHT_STEALING_FROM_ME",
	"EV_SHOO_FROM_MY_BUILDING", "EV_MARKED_FOR_DEATH", "EV_SCREAMING_TORTURE", "EV_BAR_TALK",
	"EV_UNLOCK_MY_CAGE_OR_SHACKLES", "EV_UNLOCK_MY_CAGE_ATTEMPT", "EV_I_DEFEATED_SQUAD", "EV_LAUNCH_ATTACK",
	"EV_INTRUDER_FOUND", "EV_HEALING_OTHER_START", "EV_BEING_HEALED_START", "EV_HEALING_OTHER_FINISHED",
	"EV_BEING_HEALED_FINISHED", "EV_FIRSTAID_KIT_EMPTY", "EV_GET_UP_PEACE", "EV_GET_UP_FIGHT",
	"EV_GET_UP_UNNECCESSARY_FIGHT", "EV_HARRASSMENT_SHOUTS", "EV_I_SEE_ANIMAL_SQUAD",
	"EV_SPEECH_INTERRUPTED_ATTACKED_BY_TARGET", "EV_SPEECH_INTERRUPTED_ATTACKED_BY_STRANGERS",
	"EV_CONTRACT_JOB_ENDED", "EV_BETRAYAL", "EV_LOOTING_WEAPON_ONLY", "EV_LOOTING_EVERYTHING",
	"EV_I_SEE_UNIFORM_IMPOSTER", "EV_INTRODUCING_NEW_SLAVE", "EV_ESCAPING_SLAVE_SPOTTED",
	"EV_RECAPTURED_A_SLAVE", "EV_SHOUT_AT_SLAVE_WORKER", "EV_SLAVE_DELIVERY", "EV_ESCAPED_EX_SLAVE_SPOTTED",
	"EV_WITNESS_GENERIC_ASSAULT", "EV_WITNESS_LOOTING_ALLY", "EV_WITNESS_THIEF_OR_LOCKPICK",
	"EV_BOUNTY_SPOTTED", "EV_ESCAPED_PRISONER_SPOTTED", "EV_PRISONER_FREE_TO_GO", "EV_ALMOST_WOKE_UP",
	"EV_ENTER_BIOME", "EV_ENTER_TOWN", "EV_SQUAD_BROKEN", "EV_BOUGHT_ME_FROM_SLAVERY",
	"EV_EATING_SOMETHING_SOUNDS", "EV_WORSHIPING_SOMETHING", "EV_SLAVE_ESCAPE_OPPORTUNITY_SAVIOR",
	"EV_SLAVE_ESCAPE_OPPORTUNITY_ALONE", "EV_ASSASSINATION_FAILED", "EV_EATING_MY_CROPS",
	"EV_KIDNAPPING_MY_ALLY", "EV_USING_MY_TRAINING_EQUIPMENT", "EV_GIVE_UP_CHASE", "EV_ACID_FEET",
	"EV_ACID_RAIN", "EV_ACID_WATER", "EV_WINDY", "EV_POISON_GAS", "EV_I_SEE_ENEMY_PLAYER",
	"EV_I_SEE_ALLY_PLAYER", "EV_I_SEE_ILLEGAL_PLAYER_BUILDING", "EV_BURNING", "EV_LOST_LEG", "EV_LOST_ARM",
	"EV_I_SEE_PLAYER_NICE_BUILDING", "EV_TAKEN_OVER_PLAYER_TOWN", "EV_CROWD_TRIGGERED", "EV_MAX",
};
constexpr const char* kCondNames[] =
{
	"DC_NONE", "DC_RELATIONS", "DC_PLAYERMONEY", "DC_REPUTATION", "DC_CARRYING_BOUNTY_ALIVE",
	"DC_CARRYING_BOUNTY_DEAD", "DC_FACTION_VARIABLE", "DC_IMPRISONED_BY_TARGET", "DC_IMPRISONED_BY_OTHER",
	"DC_IS_A_TRADER", "DC_FACTION_RANK", "DC_BUILDING_IS_CLOSED_AND_SECURED", "DC_PLAYER_TECH_LEVEL",
	"DC_NUM_DIALOG_EVENT_REPEATS", "DC_IS_IMPRISONED", "DC_IMPRISONMENT_IS_DEATHROW",
	"DC_TARGET_IN_TALKING_RANGE", "DC_IN_MY_BUILDING", "DC_TARGET_LAST_SEEN_X_HOURS_AGO", "DC_IS_LEADER",
	"DC_MET_TARGET_BEFORE", "DC_WEAKER_THAN_ME", "DC_STRONGER_THAN_ME", "DC_HAS_TAG", "DC_IS_ALLY",
	"DC_IS_ENEMY", "DC_PERSONALITY_TAG", "DC_BROKEN_LEG", "DC_BROKEN_ARM", "DC_DAMAGED_HEAD", "DC_NEARLY_KO",
	"DC_IN_A_NON_PLAYER_TOWN", "DC_IS_RUNNING", "DC_COPS_AROUND", "NULL_NULL_____DC_TARGET_SQUAD_SIZE",
	"DC_SQUAD_SIZE", "DC_IS_PLAYER", "DC_NUM_BACKPACKS", "DC_SQUAD_ONLY_ANIMALS", "DC_IS_OUTNUMBERED",
	"DC_BOUNTY_AMOUNT_PERCEIVED", "DC_IS_KO", "DC_IS_NEARLY_KO", "DC_SQUAD_IS_DOWN", "DC_IS_DEAD",
	"DC_IS_FEMALE", "DC_CARRYING_SOMEONE_TO_ENSLAVE", "DC_BOUNTY_AMOUNT_ACTUAL", "DC_IM_UNARMED",
	"DC_TOWN_HAS_FORTIFICATIONS_WALLS", "DC_TARGET_IS_MY_MISSION_TARGET", "DC_MY_MISSION_IS_FRIENDLY",
	"DC_I_LOVE_THIS_GUY", "DC_I_HATE_THIS_GUY", "DC_I_SHOULD_SCREW_THIS_GUY_OVER", "DC_I_SHOULD_HELP_THIS_GUY",
	"DC_IN_COMBAT", "DC_WITHIN_TOWN_WALLS", "DC_TOWN_WALLS_LOCKED_UP", "DC_IS_SLAVE", "DC_HAS_A_BASE_NEARBY",
	"DC_TARGET_IS_SLAVE_OF_MY_FACTION", "DC_IS_ESCAPED_SLAVE", "DC_IS_IN_LOCKED_CAGE",
	"DC_WEARING_LOCKED_SHACKLES", "DC_IS_SAME_RACE_AS_ME", "DC_CAN_AFFORD_BOUNTY", "DC_IS_SNEAKING",
	"DC_IS_INDOORS", "DC_HAS_ILLEGAL_ITEM", "DC_USING_MY_TRAINING_EQUIPMENT", "DC_STARVING",
	"DC_MIXED_GENDER_GROUP", "DC_TOWN_LEVEL_CURRENT_LOCATION", "DC_PLAYERS_BEST_TOWN_LEVEL",
	"DC_IN_A_PLAYER_TOWN", "DC_TARGET_CHARACTER_EXISTS", "DC_IS_RECRUITABLE", "DC_HAS_AI_CONTRACT",
	"DC_HAS_ROBOT_LIMBS", "DC_END",
};
constexpr const char* kActionNames[] =
{
	"DA_NONE", "DA_TRADE", "DA_TALK_TO_LEADER", "DA_JOIN_SQUAD_WITH_EDIT", "DA_AFFECT_RELATIONS",
	"DA_AFFECT_REPUTATION", "DA_ATTACK_CHASE_FOREVER", "DA_GO_HOME", "DA_TAKE_MONEY", "DA_GIVE_MONEY",
	"DA_PAY_BOUNTY", "DA_CHARACTER_EDITOR", "DA_FORCE_SPEECH_TIMER", "DA_DECLARE_WAR", "DA_END_WAR",
	"DA_CLEAR_AI", "DA_FOLLOW_WHILE_TALKING", "DA_THUG_HUNTER", "DA_JOIN_SQUAD_FAST", "DA_REMEMBER_CHARACTER",
	"DA_FLAG_TEMP_ALLY", "DA_FLAG_TEMP_ENEMY", "DA_MATES_KILL_ME", "DA_MAKE_TARGET_RUN_FASTER",
	"DA_GIVE_TARGET_MY_SLAVES", "DA_TAG_ESCAPED_SLAVE", "DA_FREE_TARGET_SLAVE", "DA_MERGE_WITH_SIMILAR_SQUADS",
	"DA_SEPARATE_TO_MY_OWN_SQUAD", "DA_ARREST_TARGET", "DA_ARREST_TARGETS_CARRIED_PERSON", "DA_ATTACK_TOWN",
	"DA_ASSIGN_BOUNTY", "DA_CRIME_ALARM", "DA_RUN_AWAY", "DA_INCREASE_FACTION_RANK", "DA_LOCK_THIS_DIALOG",
	"DA_ASSAULT_PHASE", "DA_RETREAT_PHASE", "DA_VICTORY_PHASE", "DA_ENSLAVE_TARGETS_CARRIED_PERSON",
	"CHOOSE_SLAVES_SELLING", "CHOOSE_SLAVES_BUYING", "CHOOSE_PRISONER_BAIL", "CHOOSE_CONSCRIPTION",
	"CHOOSE_RECRUITING", "CHOOSE_HIRING_CONTRACT", "SURRENDER_NON_HUMANS", "CHOOSE_ANIMALS_BUYING",
	"DA_CLEAR_BOUNTY", "DA_PLAYER_SELL_PRISONERS", "DA_PLAYER_SURRENDER_MEMBER_DIFFERENT_RACE",
	"DA_SUMMON_MY_SQUAD", "DA_REMOVE_SLAVE_STATUS", "DA_OPEN_NEAREST_GATE", "DA_ATTACK_STAY_NEAR_HOME",
	"DA_MASSIVE_ALARM", "DA_ATTACK_IF_NO_COEXIST", "DA_KNOCKOUT", "DA_END",
};

template <std::size_t N>
const char* nameOf(const char* const (&table)[N], int index) noexcept
{
	return index >= 0 && static_cast<std::size_t>(index) < N ? table[index] : "?";
}

// DialogConditionEnum ids that are not conditions at run time or have no handler.
bool isPassKind(int kind) noexcept
{
	return kind == dc::None || kind == dc::FactionVariable || kind == dc::ImprisonmentIsDeathrow
		|| kind == dc::TargetSquadSizeDisabled || kind == dc::MyMissionIsFriendly || kind == dc::End;
}

// Strict 3-way comparators (DIALOGUE_CONDITIONS_STATIC 1.2).
bool isStrictKind(int kind) noexcept
{
	return kind == dc::SquadSize || kind == dc::NumBackpacks || kind == dc::FactionRank || kind == dc::PlayerTechLevel
		|| kind == dc::NumDialogEventRepeats || kind == dc::TownLevelCurrentLocation || kind == dc::PlayersBestTownLevel;
}

// "Less than, else at least": `==` and `>` both read as `>=`.
bool isAtLeastKind(int kind) noexcept
{
	return kind == dc::Relations || kind == dc::PlayerMoney || kind == dc::Reputation || kind == dc::BountyAmountPerceived
		|| kind == dc::BountyAmountActual || kind == dc::TargetLastSeenXHoursAgo || kind == dc::HasIllegalItem;
}

// The ten relation conditions whose T_ME is redirected to the target (1406717c0): 0x10 0x12 0x14 0x32 0x34-0x37 0x3d 0x41.
bool isRelationRedirect(int kind) noexcept
{
	return kind == 16 || kind == 18 || kind == 20 || kind == 50 || (kind >= 52 && kind <= 55) || kind == 61 || kind == 65;
}

// Actions whose operand the executor reads (DIALOGUE_ACTIONS_STATIC "Operand verdict"); the rest compile to 0.
bool keepsOperand(int kind) noexcept
{
	switch (kind)
	{
	case 4: // AFFECT_RELATIONS
	case 5: // AFFECT_REPUTATION
	case 8: // TAKE_MONEY
	case 9: // GIVE_MONEY
	case 12: // FORCE_SPEECH_TIMER
	case 32: // ASSIGN_BOUNTY
	case 35: // INCREASE_FACTION_RANK
	case 41: // CHOOSE_SLAVES_SELLING
	case 42: // CHOOSE_SLAVES_BUYING
	case 48: // CHOOSE_ANIMALS_BUYING
	case 58: // KNOCKOUT
		return true;
	default:
		return false;
	}
}

// Kinds the exe has no executor for (DIALOGUE.md 1.1): DA_MATES_KILL_ME, FREE_TARGET_SLAVE, MERGE_WITH_SIMILAR_SQUADS,
// ATTACK_TOWN, CHOOSE_CONSCRIPTION / RECRUITING / HIRING_CONTRACT, PLAYER_SELL_PRISONERS, PLAYER_SURRENDER_MEMBER_DIFFERENT_RACE, END.
bool isDeadAction(int kind) noexcept
{
	return kind == 22 || kind == 26 || kind == 27 || kind == 31 || (kind >= 44 && kind <= 46) || kind == 50 || kind == 51
		|| kind == 59;
}

// Events a `dialogs` slot may name; 0 (unused) and 100 are skipped (140678fab0).
bool isEventSlot(std::int32_t value) noexcept
{
	return value > 0 && value < kDialogueEventCount && value != 100;
}

template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (!entry.opaqueEntry)
			func(entry);
	}
}

// `compare by` is an int, or in 880 records a string ('==', '>'); FCS methodToInt: '<' 1, '>' 2, else 0.
int compareBy(const kdata::Record& action)
{
	if (const kdata::FieldInt* number = action.findInt("compare by"))
		return number->value >= 0 && number->value <= 2 ? number->value : 0;
	if (const kdata::FieldString* text = action.findString("compare by"))
		return text->value == "<" ? 1 : text->value == ">" ? 2 : 0;
	return 0;
}

bool isTextKey(const std::string& key, int& number)
{
	if (key.size() < 5 || key.compare(0, 4, "text") != 0)
		return false;
	number = 0;
	for (std::size_t i = 4; i < key.size(); ++i)
	{
		if (key[i] < '0' || key[i] > '9')
			return false;
		number = number * 10 + (key[i] - '0');
	}
	return true;
}

// The names written between slashes in a text (/NAME/), as the data's tokens: [A-Z0-9_]+.
void collectTokens(std::string_view text, std::set<std::string>& out)
{
	std::size_t at = 0;
	while ((at = text.find('/', at)) != std::string_view::npos)
	{
		std::size_t end = at + 1;
		while (end < text.size() && ((text[end] >= 'A' && text[end] <= 'Z') || (text[end] >= '0' && text[end] <= '9') || text[end] == '_'))
			++end;
		if (end > at + 1 && end < text.size() && text[end] == '/')
		{
			out.emplace(text.substr(at + 1, end - at - 1));
			at = end + 1;
		} else {
			++at;
		}
	}
}

} // namespace

const char* dialogueEventName(int event) noexcept { return nameOf(kEventNames, event); }
const char* dialogueCondName(int kind) noexcept { return nameOf(kCondNames, kind); }
const char* dialogueActionName(int kind) noexcept { return nameOf(kActionNames, kind); }

const char* condOpName(CondOp op) noexcept
{
	switch (op)
	{
	case CondOp::Pass: return "pass";
	case CondOp::Bool: return "bool";
	case CondOp::Eq: return "==";
	case CondOp::Lt: return "<";
	case CondOp::Gt: return ">";
	case CondOp::Ge: return ">=";
	}
	return "?";
}

const char* gateListName(int list) noexcept
{
	static constexpr const char* kNames[GlCount] = { "in town of", "my faction", "target faction", "is character",
		"target carrying character", "has package", "target has item", "target has item type", "target race",
		"no target race", "my race", "my subrace" };
	return list >= 0 && list < GlCount ? kNames[list] : "?";
}

std::span<const std::uint32_t> PackageSet::rootsFor(int event) const noexcept
{
	if (event < 0 || event >= kDialogueEventCount)
		return {};
	return byEvent[event];
}

std::size_t StaticPass::count() const noexcept
{
	std::size_t total = 0;
	for (std::size_t i = 0; i < size_; ++i)
		total += test(i) ? 1 : 0;
	return total;
}

// ---------------------------------------------------------------------------
// The compile
// ---------------------------------------------------------------------------

class DialogueCompiler
{
public:
	DialogueCompiler(DialogueBook& book, const kdata::GameData& data)
		: book_(book)
		, data_(data)
		, census_(book.census_)
		, dialogueType_(kworld::itemTypeByName("DIALOGUE"))
		, lineType_(kworld::itemTypeByName("DIALOGUE_LINE"))
		, actionType_(kworld::itemTypeByName("DIALOG_ACTION"))
		, packageType_(kworld::itemTypeByName("DIALOGUE_PACKAGE"))
		, swapType_(kworld::itemTypeByName("WORD_SWAPS"))
	{
	}

	void run()
	{
		scanData();
		assignIndexes();
		compileDialogues();
		compileLines();
		compileSwaps();
		compilePackages();
		scanTextTokens();
	}

private:
	DialogueBook& book_;
	const kdata::GameData& data_;
	DialogueCensus& census_;
	const std::int32_t dialogueType_, lineType_, actionType_, packageType_, swapType_;
	std::vector<const kdata::Record*> dialogueRecs_, lineRecs_;

	std::uint32_t intern(std::string_view sid)
	{
		auto [it, added] = book_.sidIds_.try_emplace(std::string(sid), static_cast<std::uint32_t>(book_.sids_.size()));
		if (added)
			book_.sids_.emplace_back(sid);
		return it->second;
	}

	const kdata::Record* typed(std::string_view sid, std::int32_t type) const
	{
		const kdata::Record* record = data_.find(sid);
		return record != nullptr && record->type == type ? record : nullptr;
	}

	// ---- the census over the whole merged data ----
	void scanData()
	{
		std::unordered_map<std::string, std::size_t> parents; // line sid -> parents among DIALOGUE / DIALOGUE_LINE
		std::set<std::string> anyParent, referencedActions;
		for (const kdata::Record& record : data_.records())
		{
			const bool rooted = record.type == dialogueType_ || record.type == lineType_;
			for (const kdata::RefCategory& category : record.references)
			{
				for (const kdata::RefEntry& entry : category.entries)
				{
					if (entry.opaqueEntry)
						continue;
					const kdata::Record* target = data_.find(entry.targetID);
					if (target == nullptr)
						continue;
					if (target->type == actionType_)
					{
						referencedActions.insert(entry.targetID);
						if (category.category == "conditions" && !rooted)
							++census_.condRefsElsewhere;
						if (category.category == "AI contract" && record.type == lineType_)
							++census_.aiPackageActions;
					}
					if (target->type == lineType_)
					{
						anyParent.insert(entry.targetID);
						if (category.category == "lines" && rooted)
							++parents[entry.targetID];
					}
				}
			}
		}
		for (const kdata::Record* record : data_.byType(dialogueType_))
		{
			++census_.dialogues;
			countUse(*record);
		}
		for (const kdata::Record* record : data_.byType(lineType_))
		{
			++census_.linesInData;
			countUse(*record);
			if (anyParent.count(record->stringID) == 0)
				++census_.noParentLines;
			auto parent = parents.find(record->stringID);
			if (parent == parents.end())
				++census_.noDialogueParentLines;
			else if (parent->second > 1)
				++census_.multiParentLines;
			if (parent != parents.end())
				census_.maxParents = std::max(census_.maxParents, parent->second);
		}
		census_.packages = data_.byType(packageType_).size();
		census_.wordSwaps = data_.byType(swapType_).size();
		const std::vector<const kdata::Record*> actions = data_.byType(actionType_);
		census_.actionRecords = actions.size();
		for (const kdata::Record* action : actions)
			census_.unreferencedActions += referencedActions.count(action->stringID) == 0 ? 1 : 0;
	}

	void countUse(const kdata::Record& record)
	{
		forEachRef(record, "conditions", [&](const kdata::RefEntry& entry)
		{
			const kdata::Record* action = typed(entry.targetID, actionType_);
			if (action == nullptr)
			{
				++census_.condRefsNotAction;
				return;
			}
			++census_.condRefs;
			const int kind = action->intOr("condition name", 0);
			if (kind >= 0 && kind < kDialogueCondCount)
				++census_.condUse[kind];
		});
		if (record.type != lineType_)
			return;
		forEachRef(record, "effects", [&](const kdata::RefEntry& entry)
		{
			const kdata::Record* action = typed(entry.targetID, actionType_);
			if (action == nullptr)
			{
				++census_.effectRefsNotAction;
				return;
			}
			++census_.effectRefs;
			const int kind = action->intOr("action name", 0);
			if (kind >= 0 && kind < kDialogueActionCount)
				++census_.actUse[kind];
		});
	}

	// ---- indexes: every DIALOGUE, then the lines reachable from a DIALOGUE or WORD_SWAPS record, in discovery order ----
	void assignIndexes()
	{
		for (const kdata::Record* record : data_.byType(dialogueType_))
		{
			book_.dialogueOf_.emplace(intern(record->stringID), static_cast<std::uint32_t>(dialogueRecs_.size()));
			dialogueRecs_.push_back(record);
		}
		std::vector<const kdata::Record*> stack;
		auto visitChildren = [&](const kdata::Record& parent)
		{
			forEachRef(parent, "lines", [&](const kdata::RefEntry& entry)
			{
				const std::uint32_t id = intern(entry.targetID);
				if (typed(entry.targetID, lineType_) == nullptr || book_.lineOf_.count(id) != 0)
					return;
				book_.lineOf_.emplace(id, static_cast<std::uint32_t>(lineRecs_.size()));
				lineRecs_.push_back(data_.find(entry.targetID));
				stack.push_back(lineRecs_.back());
			});
		};
		auto drain = [&]()
		{
			while (!stack.empty())
			{
				const kdata::Record* line = stack.back();
				stack.pop_back();
				visitChildren(*line);
			}
		};
		for (const kdata::Record* record : dialogueRecs_)
		{
			visitChildren(*record);
			drain();
		}
		for (const kdata::Record* record : data_.byType(swapType_))
		{
			visitChildren(*record);
			drain();
		}
		census_.linesCompiled = lineRecs_.size();
		census_.unreachableLines = census_.linesInData - census_.linesCompiled;
	}

	// ---- pools ----
	Span poolIds(const std::vector<std::uint32_t>& values)
	{
		Span span{ static_cast<std::uint32_t>(book_.ids_.size()), static_cast<std::uint32_t>(values.size()) };
		book_.ids_.insert(book_.ids_.end(), values.begin(), values.end());
		return span;
	}

	CondDef condition(const kdata::Record& action, std::int32_t value)
	{
		CondDef def;
		const int kind = action.intOr("condition name", 0);
		const int who = action.intOr("who", 0);
		def.value = value;
		def.onTarget = who != 0 || isRelationRedirect(kind);
		if (kind < 0 || kind >= kDialogueCondCount)
		{
			++census_.condUnknown;
			def.kind = dc::None;
			def.op = CondOp::Pass;
			return def;
		}
		def.kind = static_cast<std::uint8_t>(kind);
		const int compare = compareBy(action);
		if (isPassKind(kind))
		{
			++census_.condPass;
			def.op = CondOp::Pass;
		} else if (isStrictKind(kind)) {
			def.op = compare == 1 ? CondOp::Lt : compare == 2 ? CondOp::Gt : CondOp::Eq;
		} else if (isAtLeastKind(kind)) {
			def.op = compare == 1 ? CondOp::Lt : CondOp::Ge;
		} else {
			def.op = CondOp::Bool;
		}
		return def;
	}

	Gate compileGate(const kdata::Record& record, bool isRoot)
	{
		Gate gate;
		gate.chancePerm = record.floatOr("chance permanent", 100.0f);
		gate.chanceTemp = record.floatOr("chance temporary", 100.0f);
		gate.repetition = static_cast<std::uint8_t>(std::clamp(record.intOr("repetition limit", 0), 0, 255));
		// `once only` is not in fcs.def; assumed to be DR_ONCE_ONLY (DIALOGUE_DATA Q6, UNSURE).
		if (isRoot && record.boolOr("once only", false))
			gate.repetition = 5;
		gate.targetType = static_cast<std::uint8_t>(std::clamp(record.intOr("target is type", 0), 0, 255));
		gate.locked = record.boolOr("locked", false);
		gate.oneAtATime = record.boolOr("one at a time", false);
		gate.scoreBonus = static_cast<std::int16_t>(std::clamp(record.intOr("score bonus", 0), -32768, 32767));

		gate.conds.first = static_cast<std::uint32_t>(book_.conds_.size());
		forEachRef(record, "conditions", [&](const kdata::RefEntry& entry)
		{
			const kdata::Record* action = typed(entry.targetID, actionType_);
			if (action == nullptr)
				return;
			const int kind = action->intOr("condition name", 0);
			if (kind == dc::HasTag || kind == dc::PersonalityTag)
			{
				// Folded into masks; cmp and who are not read (1.1). v0 == 1 requires, anything else forbids.
				++census_.condFolded;
				const int tag = action->intOr("tag", -1);
				if (tag < 0 || tag > 31)
				{
					++census_.condFoldedBadTag;
					return;
				}
				std::uint32_t& mask = kind == dc::HasTag ? (entry.v0 == 1 ? gate.reqMemory : gate.forbMemory)
					: (entry.v0 == 1 ? gate.reqPersonality : gate.forbPersonality);
				mask |= 1u << tag;
				return;
			}
			book_.conds_.push_back(condition(*action, entry.v0));
		});
		gate.conds.count = static_cast<std::uint32_t>(book_.conds_.size()) - gate.conds.first;

		for (int list = 0; list < GlCount; ++list)
		{
			std::vector<std::uint32_t> values;
			forEachRef(record, gateListName(list), [&](const kdata::RefEntry& entry) { values.push_back(intern(entry.targetID)); });
			gate.lists[list] = poolIds(values);
		}
		gate.worldState.first = static_cast<std::uint32_t>(book_.worldReqs_.size());
		forEachRef(record, "world state", [&](const kdata::RefEntry& entry)
		{
			book_.worldReqs_.push_back(WorldReq{ intern(entry.targetID), entry.v0 != 0 });
		});
		gate.worldState.count = static_cast<std::uint32_t>(book_.worldReqs_.size()) - gate.worldState.first;
		return gate;
	}

	Span compileChildren(const kdata::Record& parent)
	{
		Span span{ static_cast<std::uint32_t>(book_.children_.size()), 0 };
		forEachRef(parent, "lines", [&](const kdata::RefEntry& entry)
		{
			auto it = book_.lineOf_.find(intern(entry.targetID));
			if (it == book_.lineOf_.end())
			{
				++census_.danglingChildren;
				return;
			}
			book_.children_.push_back(ChildRef{ it->second, static_cast<std::uint8_t>(std::clamp(entry.v0, 0, 255)) });
		});
		span.count = static_cast<std::uint32_t>(book_.children_.size()) - span.first;
		return span;
	}

	void compileDialogues()
	{
		book_.dialogues_.reserve(dialogueRecs_.size());
		for (const kdata::Record* record : dialogueRecs_)
		{
			DialogueDef def;
			def.sid = intern(record->stringID);
			def.name = record->name;
			def.forEnemies = record->boolOr("for enemies", false);
			def.monologue = record->boolOr("monologue", true);
			def.gate = compileGate(*record, true);
			def.firstLines = compileChildren(*record);
			book_.dialogues_.push_back(std::move(def));
		}
	}

	Span dialogueLinks(const kdata::Record& line, std::string_view category)
	{
		Span span{ static_cast<std::uint32_t>(book_.ids_.size()), 0 };
		forEachRef(line, category, [&](const kdata::RefEntry& entry)
		{
			auto it = book_.dialogueOf_.find(intern(entry.targetID));
			if (it == book_.dialogueOf_.end())
				++census_.danglingLinks;
			else
				book_.ids_.push_back(it->second);
		});
		span.count = static_cast<std::uint32_t>(book_.ids_.size()) - span.first;
		return span;
	}

	void compileLines()
	{
		book_.lines_.reserve(lineRecs_.size());
		for (const kdata::Record* record : lineRecs_)
		{
			LineDef def;
			def.sid = intern(record->stringID);
			def.gate = compileGate(*record, false);
			def.speaker = static_cast<std::uint8_t>(std::clamp(record->intOr("speaker", 0), 0, 255));
			def.interjection = record->boolOr("interjection", false);

			std::vector<std::pair<int, const std::string*>> variants;
			for (const kdata::FieldString& field : record->strings)
			{
				int number = 0;
				if (isTextKey(field.key, number) && !field.value.empty())
					variants.emplace_back(number, &field.value);
			}
			std::sort(variants.begin(), variants.end());
			def.text.first = static_cast<std::uint32_t>(book_.texts_.size());
			for (const auto& [number, value] : variants)
			{
				book_.texts_.push_back(TextRef{ static_cast<std::uint32_t>(book_.textPool_.size()), static_cast<std::uint32_t>(value->size()) });
				book_.textPool_ += *value;
			}
			def.text.count = static_cast<std::uint32_t>(variants.size());

			def.effects.first = static_cast<std::uint32_t>(book_.acts_.size());
			forEachRef(*record, "effects", [&](const kdata::RefEntry& entry)
			{
				const kdata::Record* action = typed(entry.targetID, actionType_);
				if (action == nullptr)
					return;
				int kind = action->intOr("action name", 0);
				if (kind < 0 || kind >= kDialogueActionCount)
				{
					++census_.actUnknown;
					kind = da::None;
				} else if (isDeadAction(kind)) {
					++census_.actDead;
					kind = da::None;
				}
				if (kind == da::None)
					++census_.actNone;
				book_.acts_.push_back(ActDef{ static_cast<std::uint8_t>(kind), keepsOperand(kind) ? entry.v0 : 0 });
			});
			def.effects.count = static_cast<std::uint32_t>(book_.acts_.size()) - def.effects.first;
			def.children = compileChildren(*record);

			def.relChanges.first = static_cast<std::uint32_t>(book_.relChanges_.size());
			forEachRef(*record, "change relations", [&](const kdata::RefEntry& entry)
			{
				book_.relChanges_.push_back(RelChange{ intern(entry.targetID), entry.v0 });
			});
			def.relChanges.count = static_cast<std::uint32_t>(book_.relChanges_.size()) - def.relChanges.first;
			def.giveItems.first = static_cast<std::uint32_t>(book_.gives_.size());
			forEachRef(*record, "give item", [&](const kdata::RefEntry& entry)
			{
				book_.gives_.push_back(ItemGive{ intern(entry.targetID), entry.v0 });
			});
			def.giveItems.count = static_cast<std::uint32_t>(book_.gives_.size()) - def.giveItems.first;
			def.contracts.first = static_cast<std::uint32_t>(book_.contracts_.size());
			forEachRef(*record, "AI contract", [&](const kdata::RefEntry& entry)
			{
				// One entry points at a DIALOG_ACTION (a mis-typed AI package, DIALOGUE_DATA 2): dropped, counted by scanData.
				if (typed(entry.targetID, actionType_) == nullptr)
					book_.contracts_.push_back(AiContract{ intern(entry.targetID), entry.v0 });
			});
			def.contracts.count = static_cast<std::uint32_t>(book_.contracts_.size()) - def.contracts.first;
			def.campaigns.first = static_cast<std::uint32_t>(book_.campaigns_.size());
			forEachRef(*record, "trigger campaign", [&](const kdata::RefEntry& entry)
			{
				book_.campaigns_.push_back(CampaignTrigger{ intern(entry.targetID), entry.v0, entry.v1, entry.v2 });
			});
			def.campaigns.count = static_cast<std::uint32_t>(book_.campaigns_.size()) - def.campaigns.first;

			def.locks = dialogueLinks(*record, "locks");
			def.unlocks = dialogueLinks(*record, "unlocks");
			def.unlockKeepMe = dialogueLinks(*record, "unlock but keep me");
			if (const Span interrupt = dialogueLinks(*record, "interrupt"); interrupt.count != 0)
				def.interrupt = book_.ids_[interrupt.first];
			forEachRef(*record, "crowd trigger", [&](const kdata::RefEntry& entry)
			{
				auto it = book_.dialogueOf_.find(intern(entry.targetID));
				if (it == book_.dialogueOf_.end())
				{
					++census_.danglingLinks;
				} else if (def.crowdTrigger == kNoIndex) {
					def.crowdTrigger = it->second;
					def.crowdPct = static_cast<std::uint8_t>(std::clamp(entry.v0, 0, 100));
				}
			});
			forEachRef(*record, "change AI", [&](const kdata::RefEntry& entry)
			{
				if (def.changeAi == kNoIndex)
					def.changeAi = intern(entry.targetID);
			});
			book_.lines_.push_back(std::move(def));
		}
	}

	void compileSwaps()
	{
		for (const kdata::Record* record : data_.byType(swapType_))
		{
			WordSwap swap;
			swap.token = record->name;
			swap.persistent = record->boolOr("persistent use", false);
			swap.lines.first = static_cast<std::uint32_t>(book_.ids_.size());
			forEachRef(*record, "lines", [&](const kdata::RefEntry& entry)
			{
				auto it = book_.lineOf_.find(intern(entry.targetID));
				if (it != book_.lineOf_.end())
					book_.ids_.push_back(it->second);
			});
			swap.lines.count = static_cast<std::uint32_t>(book_.ids_.size()) - swap.lines.first;
			book_.swaps_.push_back(std::move(swap));
		}
	}

	// ---- packages ----
	void compilePackages()
	{
		std::array<std::set<std::uint32_t>, kDialogueEventCount> eventDialogues;
		for (const kdata::Record* record : data_.byType(packageType_))
		{
			DialogueBook::PackageDef def;
			def.skipped = record->name.find("Unfinished") != std::string::npos;
			if (def.skipped)
				++census_.packagesUnfinished;
			forEachRef(*record, "inheritsFrom", [&](const kdata::RefEntry& entry)
			{
				if (typed(entry.targetID, packageType_) != nullptr)
					def.inherits.push_back(intern(entry.targetID));
			});
			forEachRef(*record, "dialogs", [&](const kdata::RefEntry& entry)
			{
				auto it = book_.dialogueOf_.find(intern(entry.targetID));
				if (it == book_.dialogueOf_.end())
				{
					++census_.danglingLinks;
					return;
				}
				DialogueBook::PackageDef::Entry item;
				item.dialogue = it->second;
				const std::int32_t slots[3] = { entry.v0, entry.v1, entry.v2 };
				bool any = false;
				for (int slot = 0; slot < 3; ++slot)
				{
					if (!isEventSlot(slots[slot]))
						continue;
					any = true;
					item.events[slot] = static_cast<std::uint8_t>(slots[slot]);
					++census_.eventEntries[slots[slot]];
					eventDialogues[slots[slot]].insert(it->second);
				}
				if (!any)
					++census_.packageEntriesNoEvent;
				def.entries.push_back(item);
			});
			book_.packages_.emplace(intern(record->stringID), std::move(def));
		}
		for (int event = 0; event < kDialogueEventCount; ++event)
			census_.eventDialogues[event] = eventDialogues[event].size();

		// A package that inherits itself, directly or through others (the exe has no guard; ours skips the revisit).
		for (const auto& [id, def] : book_.packages_)
		{
			std::set<std::uint32_t> seen;
			std::vector<std::uint32_t> stack(def.inherits.begin(), def.inherits.end());
			bool cycle = false;
			while (!stack.empty() && !cycle)
			{
				const std::uint32_t at = stack.back();
				stack.pop_back();
				if (at == id)
				{
					cycle = true;
				} else if (seen.insert(at).second) {
					auto next = book_.packages_.find(at);
					if (next != book_.packages_.end())
						stack.insert(stack.end(), next->second.inherits.begin(), next->second.inherits.end());
				}
			}
			census_.packagesInheritCycle += cycle ? 1 : 0;
		}
	}

	// ---- text tokens (/NAME/): WORD_SWAPS names resolve; the rest are runtime substitutions or typos ----
	void scanTextTokens()
	{
		std::set<std::string> used;
		for (const kdata::Record* record : data_.byType(lineType_))
		{
			for (const kdata::FieldString& field : record->strings)
			{
				int number = 0;
				if (isTextKey(field.key, number))
					collectTokens(field.value, used);
			}
		}
		std::set<std::string> swaps;
		for (const kdata::Record* record : data_.byType(swapType_))
			swaps.insert(record->name);
		census_.tokensUsed = used.size();
		for (const std::string& token : used)
		{
			if (swaps.count(token) != 0)
				++census_.tokensWordSwap;
			else
				census_.unresolvedTokens.push_back(token);
		}
		census_.tokensUnresolved = census_.unresolvedTokens.size();
	}
};

std::shared_ptr<const DialogueBook> DialogueBook::build(std::shared_ptr<const kdata::GameData> gameData)
{
	auto book = std::shared_ptr<DialogueBook>(new DialogueBook);
	book->data_ = std::move(gameData);
	DialogueCompiler(*book, *book->data_).run();
	return book;
}

std::uint32_t DialogueBook::idOf(std::string_view sid) const
{
	auto it = sidIds_.find(std::string(sid));
	return it == sidIds_.end() ? kNoIndex : it->second;
}

std::uint32_t DialogueBook::dialogueIndex(std::string_view sid) const
{
	auto it = dialogueOf_.find(idOf(sid));
	return it == dialogueOf_.end() ? kNoIndex : it->second;
}

std::uint32_t DialogueBook::lineIndex(std::string_view sid) const
{
	auto it = lineOf_.find(idOf(sid));
	return it == lineOf_.end() ? kNoIndex : it->second;
}

} // namespace ksim
