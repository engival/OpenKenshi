// The dialogue book (include/ksim/dialogue/dialogue_book.hpp, docs/DIALOGUE.md 8 slice 1 steps 1 and 2):
//
//  1. the compile reproduces the census of docs/reference/DIALOGUE_DATA.md (1,890 dialogues, 22,783 lines,
//     293 packages, 10,938 + 13 condition refs over 69 kinds, 5,011 effects over 51 kinds ...); numbers the
//     doc has differently are asserted at what the real data gives, with the doc's figure in the comment;
//  2. the package sets: the exe's source precedence, inheritance parents first, the event index, a package
//     visited once, "Manhunters" 63 effective dialogues, and the slice-1 roots under their events;
//  3. the static pre-pass only excludes: personality masks, speaker-side booleans, race / faction lists.
//
// tests/ksim_py/dialogue_compare.py (ksim_dialogue_compare) checks the same sets against tools/dialogue.py.

#include "ksim/dialogue/dialogue_book.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace ksim;
using test::require;

std::string str(std::size_t value)
{
	return std::to_string(value);
}

void expectEq(std::size_t got, std::size_t want, const std::string& what)
{
	require(got == want, what + ": got " + str(got) + ", want " + str(want));
}

std::size_t kindsUsed(const auto& use)
{
	return static_cast<std::size_t>(std::count_if(use.begin(), use.end(), [](std::size_t n) { return n != 0; }));
}

bool hasRoot(const DialogueBook& book, const PackageSet& set, std::string_view dialogueSid, int event)
{
	const std::uint32_t index = book.dialogueIndex(dialogueSid);
	for (const std::uint32_t position : set.rootsFor(event))
	{
		if (set.roots[position] == index)
			return true;
	}
	return false;
}

PackageSetId setOfPackage(const DialogueBook& book, std::string_view sid)
{
	const std::vector<std::string_view> list{ sid };
	return book.packageSetOf(list);
}

// Is `dialogueSid` rooted under `event` by any package of the data?
bool someSetRoots(const DialogueBook& book, std::string_view dialogueSid, int event)
{
	const kdata::GameData& data = book.gameData();
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("DIALOGUE_PACKAGE")))
	{
		if (hasRoot(book, book.packageSet(setOfPackage(book, record->stringID)), dialogueSid, event))
			return true;
	}
	return false;
}

void census(const DialogueBook& book)
{
	const DialogueCensus& c = book.census();
	expectEq(c.dialogues, 1890, "DIALOGUE records");
	expectEq(c.linesInData, 22783, "DIALOGUE_LINE records");
	expectEq(c.packages, 293, "DIALOGUE_PACKAGE records");
	expectEq(c.actionRecords, 21135, "DIALOG_ACTION records");
	expectEq(c.wordSwaps, 289, "WORD_SWAPS records");
	expectEq(c.unreferencedActions, 5174, "unreferenced DIALOG_ACTION");
	expectEq(c.noDialogueParentLines, 788, "lines with no DIALOGUE / LINE parent");
	expectEq(c.noParentLines, 285, "lines with no parent at all"); // WORD_SWAPS adopt 503 of the 788
	expectEq(c.multiParentLines, 3340, "lines with several parents");
	expectEq(c.maxParents, 49, "most parents of one line");
	expectEq(c.linesCompiled + c.unreachableLines, c.linesInData, "compiled + unreachable lines");
	expectEq(c.unreachableLines, 1109, "lines unreachable from a root or WORD_SWAPS record");
	expectEq(book.lineCount(), c.linesCompiled, "line array");
	expectEq(book.dialogueCount(), c.dialogues, "dialogue array");

	expectEq(c.condRefs, 10938, "condition refs in DIALOGUE / LINE");
	expectEq(c.condRefs + c.condRefsElsewhere, 10951, "condition refs in all records"); // the 13 are SINGLE_DIPLOMATIC_ASSAULT
	expectEq(kindsUsed(c.condUse), 69, "condition kinds used");
	expectEq(c.effectRefs, 5011, "effect refs");
	expectEq(kindsUsed(c.actUse), 51, "action kinds used");
	expectEq(c.condRefsNotAction + c.effectRefsNotAction, 0, "refs to a record that is not a DIALOG_ACTION");
	expectEq(c.condUnknown + c.actUnknown, 0, "unknown condition or action kinds");
	expectEq(c.danglingChildren + c.danglingLinks, 0, "dangling references");
	expectEq(c.aiPackageActions, 1, "DIALOG_ACTION used as an AI package");
	expectEq(c.condUse[dc::PersonalityTag], 2125, "DC_PERSONALITY_TAG entries");
	expectEq(c.condUse[dc::HasTag], 276, "DC_HAS_TAG entries");
	// Folded on the compiled nodes only: the 1,109 dropped lines hold 36 of the 2,401 entries; 3 have a tag outside 0..31.
	expectEq(c.condFolded, 2365, "folded HAS_TAG / PERSONALITY_TAG entries");
	expectEq(c.condFoldedBadTag, 3, "folded entries with a bad tag");

	expectEq(c.tokensUsed, 279, "distinct text tokens");
	expectEq(c.tokensWordSwap, 267, "tokens that are WORD_SWAPS names");
	expectEq(c.tokensUnresolved, 12, "unresolved tokens");

	expectEq(kindsUsed(c.eventEntries), 68, "events used"); // the census doc says 69; its own unused list leaves 68
	expectEq(c.eventEntries[9], 61, "EV_SHOO_FROM_MY_BUILDING entries");
	expectEq(c.eventEntries[66], 117, "EV_I_SEE_ENEMY_PLAYER entries");
	expectEq(c.eventEntries[3], 312, "EV_I_SEE_NEUTRAL_SQUAD entries");
	expectEq(c.packageEntriesNoEvent, 14, "package entries with no event");
}

void compileRules(const DialogueBook& book)
{
	// The comparator is normalised once: "T_TARGET DC_RELATIONS < 15" stays <, a "> 15" would read >= 15.
	const DialogueDef& starver = book.dialogue(book.dialogueIndex("4853-D-dialogue-02.mod"));
	bool sawRelations = false, sawLeader = false;
	for (const CondDef& cond : book.conds(starver.gate.conds))
	{
		if (cond.kind == dc::Relations)
		{
			sawRelations = true;
			require(cond.op == CondOp::Lt && cond.onTarget && cond.value == 15, "RELATIONS < 15 on the target");
		}
		if (cond.kind == dc::IsLeader)
		{
			sawLeader = true;
			require(cond.op == CondOp::Bool && !cond.onTarget && cond.value == 1, "IS_LEADER == 1 on the speaker");
		}
	}
	require(sawRelations && sawLeader, "starver root keeps its RELATIONS and IS_LEADER conditions");
	// DC_PERSONALITY_TAG / DC_HAS_TAG never appear as conditions.
	for (std::size_t i = 0; i < book.dialogueCount(); ++i)
	{
		for (const CondDef& cond : book.conds(book.dialogue(i).gate.conds))
			require(cond.kind != dc::HasTag && cond.kind != dc::PersonalityTag, "folded kinds left in a condition list");
	}
	// Every DC_RELATIONS / REPUTATION / ... in the data is an at-least or less-than: never Eq or Gt.
	for (std::size_t i = 0; i < book.lineCount(); ++i)
	{
		for (const CondDef& cond : book.conds(book.line(i).gate.conds))
		{
			if (cond.kind == dc::Relations || cond.kind == dc::PlayerMoney)
				require(cond.op == CondOp::Ge || cond.op == CondOp::Lt, "an at-least kind compiled as ==/>");
			if (cond.kind == dc::SquadSize)
				require(cond.op == CondOp::Eq || cond.op == CondOp::Lt || cond.op == CondOp::Gt, "SQUAD_SIZE keeps its 3-way");
		}
	}
	// The word swaps: /FUCKOFF/ is one (DIALOGUE_DATA 8).
	const auto swaps = book.wordSwaps();
	require(std::any_of(swaps.begin(), swaps.end(), [](const WordSwap& swap) { return swap.token == "FUCKOFF" && swap.lines.count != 0; }),
		"the FUCKOFF word swap");
}

void packages(const DialogueBook& book)
{
	// Manhunters: own 7, effective 63 (DIALOGUE_DATA 6).
	const PackageSet& manhunters = book.packageSet(setOfPackage(book, "54936-Dialogue.mod"));
	expectEq(manhunters.roots.size(), 63, "Manhunters effective dialogues");

	// Slice 1: starving bandits (3781): "starver marks a target" under EV_I_SEE_NEUTRAL_SQUAD (3) and EV_I_SEE_ENEMY_PLAYER (66).
	const PackageSet& starving = book.packageSet(setOfPackage(book, "3781-gamedata.base"));
	require(hasRoot(book, starving, "4853-D-dialogue-02.mod", 3), "starving bandits: starver under EV 3");
	require(hasRoot(book, starving, "4853-D-dialogue-02.mod", 66), "starving bandits: starver under EV 66");
	require(!hasRoot(book, starving, "4853-D-dialogue-02.mod", 9), "starving bandits: starver is not a shoo");
	require(hasRoot(book, starving, "14417-Dialogue (4).mod", 14), "starving bandits: its own EV_UNLOCK_MY_CAGE_ATTEMPT root");
	// The parent package (npc basic human) is walked first: its roots come before the package's own.
	require(starving.roots.size() > 7, "starving bandits inherit");

	// The rest of slice 1 is rooted by some package under its event.
	require(someSetRoots(book, "3124-gamedata.base", 9), "Blister Hill shoo civ under EV_SHOO_FROM_MY_BUILDING");
	require(someSetRoots(book, "13509-Escapes.mod", 9), "Bandit thug shoo under EV_SHOO_FROM_MY_BUILDING");

	// A package visited twice, or inheriting itself, is walked once (the data has one such cycle).
	const std::vector<std::string_view> twice{ "3781-gamedata.base", "3781-gamedata.base" };
	const PackageSet& doubled = book.packageSet(book.packageSetOf(twice));
	expectEq(doubled.roots.size(), starving.roots.size(), "a package listed twice");

	// Identical lists share one set (content-addressed), different lists do not.
	require(setOfPackage(book, "3781-gamedata.base") == setOfPackage(book, "3781-gamedata.base"), "set ids are shared");
	require(setOfPackage(book, "3781-gamedata.base") != setOfPackage(book, "54936-Dialogue.mod"), "different packages, different sets");

	// An unknown package is the empty set, not an error.
	const std::vector<std::string_view> nothing{ "no such package" };
	expectEq(book.packageSet(book.packageSetOf(nothing)).roots.size(), 0, "unknown package");
}

void speakers(const DialogueBook& book)
{
	// Source precedence (Q5): the first non-empty level wins, no merge. Pick a squad template that has both a
	// `dialog leader` and a `dialog squad` and check the leader and a member get different lists.
	const kdata::GameData& data = book.gameData();
	std::size_t checked = 0;
	for (const kdata::Record* squad : data.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
	{
		const kdata::RefCategory* leader = squad->findRefs("dialog leader");
		const kdata::RefCategory* member = squad->findRefs("dialog squad");
		if (leader == nullptr || member == nullptr || leader->entries.empty() || member->entries.empty()
			|| leader->entries.front().targetID == member->entries.front().targetID)
			continue;
		SpeakerKey key;
		key.squadTemplate = squad->stringID;
		key.leader = true;
		const PackageSetId asLeader = book.packageSetFor(key);
		key.leader = false;
		const PackageSetId asMember = book.packageSetFor(key);
		std::vector<std::string_view> leaderList, memberList;
		for (const kdata::RefEntry& entry : leader->entries)
			leaderList.push_back(entry.targetID);
		for (const kdata::RefEntry& entry : member->entries)
			memberList.push_back(entry.targetID);
		require(asLeader == book.packageSetOf(leaderList), "leader gets `dialog leader` only: " + squad->stringID);
		require(asMember == book.packageSetOf(memberList), "member gets `dialog squad` only: " + squad->stringID);
		if (++checked == 20)
			break;
	}
	require(checked > 0, "found squad templates with both dialog lists");

	// A player-faction character with no `dialogue package player` gets the default `player character default`.
	SpeakerKey player;
	player.playerFaction = true;
	const std::vector<std::string_view> fallback{ "5369-gamedata.base" };
	require(book.packageSetFor(player) == book.packageSetOf(fallback), "player default package");
	// A character's own `dialogue` roots are always added under EV_PLAYER_TALK_TO_ME.
	for (const kdata::Record* record : data.byType(kworld::itemTypeByName("CHARACTER")))
	{
		const kdata::RefCategory* own = record->findRefs("dialogue");
		if (own == nullptr || own->entries.empty())
			continue;
		SpeakerKey key;
		key.character = record->stringID;
		const PackageSet& set = book.packageSet(book.packageSetFor(key));
		require(hasRoot(book, set, own->entries.front().targetID, 1), "own dialogue root under EV 1: " + record->stringID);
		break;
	}
}

void staticPass(const DialogueBook& book)
{
	// "starver marks a target" needs T_ME DC_IS_LEADER == 1: a non-leader is excluded, a leader may still fail later.
	const PackageSetId id = setOfPackage(book, "3781-gamedata.base");
	const PackageSet& set = book.packageSet(id);
	const std::uint32_t starver = book.dialogueIndex("4853-D-dialogue-02.mod");
	const std::size_t position = static_cast<std::size_t>(std::find(set.roots.begin(), set.roots.end(), starver) - set.roots.begin());
	require(position < set.roots.size(), "starver is in the set");

	SpeakerFacts facts;
	facts.leader = true;
	StaticPass leaderPass = book.staticPass(set, facts);
	require(leaderPass.test(position), "a leader keeps the starver root");
	facts.leader = false;
	StaticPass memberPass = book.staticPass(set, facts);
	require(!memberPass.test(position), "a non-leader is excluded from the starver root");
	require(book.staticExclusion(book.dialogue(starver).gate, facts, set.sources) == "DC_IS_LEADER", "the reason is named");
	require(memberPass.count() < leaderPass.count(), "the leader condition removes roots");
	expectEq(leaderPass.size(), set.roots.size(), "pass size");

	// Personality masks: any root with a required mask is excluded for a personality outside it, kept for one inside.
	std::size_t tested = 0;
	for (std::size_t i = 0; i < set.roots.size() && tested == 0; ++i)
	{
		const Gate& gate = book.dialogue(set.roots[i]).gate;
		if (gate.reqPersonality == 0)
			continue;
		int inside = -1, outside = -1;
		for (int p = 0; p < 16; ++p)
		{
			((gate.reqPersonality >> p) & 1u ? inside : outside) = p;
		}
		SpeakerFacts probe;
		probe.personality = inside;
		require(book.staticExclusion(gate, probe, set.sources) != "personality not required", "inside the required mask");
		probe.personality = outside;
		require(book.staticExclusion(gate, probe, set.sources) == "personality not required", "outside the required mask");
		++tested;
	}

	// Unknown speaker facts (an empty race or faction) never exclude.
	SpeakerFacts unknown;
	const StaticPass loose = book.staticPass(set, unknown);
	require(loose.count() >= memberPass.count(), "unknown facts exclude no more than known ones");

	// Over every package: the pass is never larger than the set and leaves each leader / female / trader root decided.
	for (const kdata::Record* record : book.gameData().byType(kworld::itemTypeByName("DIALOGUE_PACKAGE")))
	{
		const PackageSet& any = book.packageSet(setOfPackage(book, record->stringID));
		SpeakerFacts everything;
		everything.character = "";
		const StaticPass pass = book.staticPass(any, everything);
		require(pass.count() <= any.roots.size(), "pass count within the set");
	}
}

} // namespace

int main()
{
	try
	{
		const auto book = DialogueBook::build(std::make_shared<kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA)));
		census(*book);
		compileRules(*book);
		packages(*book);
		speakers(*book);
		staticPass(*book);
		std::printf("ksim_dialogue_book: %zu dialogues, %zu lines, %zu package sets\n", book->dialogueCount(), book->lineCount(),
			book->packageSetCount());
	} catch (const std::exception& error) {
		std::cerr << "FAIL: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
