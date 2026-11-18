// ksim_dialogue_coverage: what the dialogue compile (include/ksim/dialogue/dialogue_book.hpp) saw in the game data and
// what it dropped, in the shape of tools/dialogue.py stats / docs/reference/DIALOGUE_DATA.md.
//
//   ksim_dialogue_coverage [--data DIR]                 the census
//   ksim_dialogue_coverage [--data DIR] --sets          + the package sets of every squad template speaker
//   ksim_dialogue_coverage [--data DIR] --package SID_OR_NAME [--facts SPEC]
//                                                       the flattened set of one package (machine readable,
//                                                       compared with tools/dialogue.py by tests/ksim_py/dialogue_compare.py);
//                                                       SPEC = key=value,... of personality leader female robot trader
//                                                       race faction character: adds the static pass per root

#include "ksim/dialogue/dialogue_book.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace ksim;

void printCensus(const DialogueBook& book)
{
	const DialogueCensus& c = book.census();
	std::printf("records: DIALOGUE %zu  DIALOGUE_LINE %zu  DIALOG_ACTION %zu  DIALOGUE_PACKAGE %zu  WORD_SWAPS %zu\n",
		c.dialogues, c.linesInData, c.actionRecords, c.packages, c.wordSwaps);
	std::printf("lines: compiled %zu  dropped %zu (unreachable from a DIALOGUE / WORD_SWAPS record)\n", c.linesCompiled, c.unreachableLines);
	std::printf("  no parent of any kind %zu; no DIALOGUE / DIALOGUE_LINE parent %zu (the census doc's \"orphans\"; WORD_SWAPS adopt the rest)\n",
		c.noParentLines, c.noDialogueParentLines);
	std::printf("  more than one DIALOGUE / LINE parent %zu (max %zu)\n", c.multiParentLines, c.maxParents);
	std::printf("DIALOG_ACTION referenced by nothing (dropped): %zu\n", c.unreferencedActions);
	std::printf("conditions refs: %zu in DIALOGUE / LINE (+%zu in other records)  not a DIALOG_ACTION: %zu\n", c.condRefs,
		c.condRefsElsewhere, c.condRefsNotAction);
	std::printf("effects refs: %zu  not a DIALOG_ACTION: %zu\n", c.effectRefs, c.effectRefsNotAction);
	std::printf("compiled: HAS_TAG / PERSONALITY_TAG folded into masks %zu (bad tag %zu), no-handler kinds (Pass) %zu, unknown condition kinds %zu\n",
		c.condFolded, c.condFoldedBadTag, c.condPass, c.condUnknown);
	std::printf("compiled: DA_NONE entries %zu (of which dead kinds %zu), unknown action kinds %zu\n", c.actNone, c.actDead, c.actUnknown);
	std::printf("dangling: `lines` children %zu, lock/unlock/interrupt/crowd/package targets %zu; DIALOG_ACTION used as an AI package: %zu\n",
		c.danglingChildren, c.danglingLinks, c.aiPackageActions);

	std::size_t kinds = 0;
	for (const std::size_t n : c.condUse)
		kinds += n != 0 ? 1 : 0;
	std::printf("\ncondition kinds used: %zu\n", kinds);
	std::vector<std::pair<std::size_t, int>> order;
	for (int kind = 0; kind < kDialogueCondCount; ++kind)
	{
		if (c.condUse[kind] != 0)
			order.emplace_back(c.condUse[kind], kind);
	}
	std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
	for (const auto& [count, kind] : order)
		std::printf("  %-36s %5zu\n", dialogueCondName(kind), count);

	kinds = 0;
	for (const std::size_t n : c.actUse)
		kinds += n != 0 ? 1 : 0;
	std::printf("\naction kinds used: %zu\n", kinds);
	order.clear();
	for (int kind = 0; kind < kDialogueActionCount; ++kind)
	{
		if (c.actUse[kind] != 0)
			order.emplace_back(c.actUse[kind], kind);
	}
	std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
	for (const auto& [count, kind] : order)
		std::printf("  %-44s %5zu\n", dialogueActionName(kind), count);

	std::printf("\npackage `dialogs` entries per event (dialogues rooted): %zu entries name no event\n", c.packageEntriesNoEvent);
	std::vector<std::pair<std::size_t, int>> events;
	for (int event = 0; event < kDialogueEventCount; ++event)
	{
		if (c.eventEntries[event] != 0)
			events.emplace_back(c.eventEntries[event], event);
	}
	std::sort(events.begin(), events.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
	std::printf("events used: %zu\n", events.size());
	for (const auto& [count, event] : events)
		std::printf("  %2d %-46s %4zu entries %4zu dialogues\n", event, dialogueEventName(event), count, c.eventDialogues[event]);
	std::printf("\npackages: inheriting themselves %zu, named \"Unfinished\" (skipped by the exe) %zu\n", c.packagesInheritCycle,
		c.packagesUnfinished);
	std::printf("text tokens /NAME/: %zu distinct, %zu are WORD_SWAPS records, %zu unresolved:", c.tokensUsed, c.tokensWordSwap,
		c.tokensUnresolved);
	for (const std::string& token : c.unresolvedTokens)
		std::printf(" %s", token.c_str());
	std::printf("\n");
}

// Every character a squad template can field, in the roles it can hold there.
void printSets(const DialogueBook& book)
{
	const kdata::GameData& data = book.gameData();
	const std::int32_t squadType = kworld::itemTypeByName("SQUAD_TEMPLATE");
	std::set<PackageSetId> distinct;
	std::set<std::string> speakers;
	std::size_t withRoots = 0, empty = 0, speakerKeys = 0;
	std::map<PackageSetId, bool> seen;
	for (const kdata::Record* squad : data.byType(squadType))
	{
		std::string faction;
		if (const kdata::RefCategory* refs = squad->findRefs("faction"); refs != nullptr && !refs->entries.empty())
			faction = refs->entries.front().targetID;
		const struct { const char* category; bool leader; bool animal; } roles[] = { { "leader", true, false },
			{ "squad", false, false }, { "squad2", false, false }, { "animals", false, true }, { "animals2", false, true } };
		for (const auto& role : roles)
		{
			if (const kdata::RefCategory* refs = squad->findRefs(role.category))
			{
				for (const kdata::RefEntry& entry : refs->entries)
				{
					SpeakerKey key;
					key.character = entry.targetID;
					key.squadTemplate = squad->stringID;
					key.faction = faction;
					key.leader = role.leader;
					key.animal = role.animal;
					const PackageSetId id = book.packageSetFor(key);
					++speakerKeys;
					if (seen.emplace(id, true).second)
						(book.packageSet(id).roots.empty() ? empty : withRoots) += 1;
				}
			}
		}
	}
	std::printf("squad template speakers: %zu (character x squad x role); distinct package sets %zu (%zu with roots, %zu empty)\n",
		speakerKeys, seen.size(), withRoots, empty);
}

const kdata::Record* findPackage(const DialogueBook& book, const std::string& key)
{
	const kdata::GameData& data = book.gameData();
	const std::int32_t type = kworld::itemTypeByName("DIALOGUE_PACKAGE");
	if (const kdata::Record* record = data.find(key); record != nullptr && record->type == type)
		return record;
	for (const kdata::Record* record : data.byType(type))
	{
		if (record->name == key)
			return record;
	}
	return nullptr;
}

std::map<std::string, std::string> parseFacts(const std::string& spec)
{
	std::map<std::string, std::string> out;
	std::stringstream stream(spec);
	std::string item;
	while (std::getline(stream, item, ','))
	{
		const std::size_t eq = item.find('=');
		if (eq != std::string::npos)
			out[item.substr(0, eq)] = item.substr(eq + 1);
	}
	return out;
}

int printPackage(const DialogueBook& book, const std::string& key, const std::string& factsSpec)
{
	const kdata::Record* package = findPackage(book, key);
	if (package == nullptr)
	{
		std::fprintf(stderr, "no DIALOGUE_PACKAGE %s\n", key.c_str());
		return 1;
	}
	const std::string_view sid = package->stringID;
	const std::vector<std::string_view> list{ sid };
	const PackageSet& set = book.packageSet(book.packageSetOf(list));
	std::printf("package %s\nroots %zu\n", package->stringID.c_str(), set.roots.size());

	SpeakerFacts facts;
	std::map<std::string, std::string> spec = parseFacts(factsSpec);
	const bool useFacts = !factsSpec.empty();
	facts.personality = spec.count("personality") ? std::atoi(spec["personality"].c_str()) : 0;
	facts.leader = spec.count("leader") ? spec["leader"] != "0" : true;
	facts.female = spec["female"] == "1";
	facts.robotLimbs = spec["robot"] == "1";
	facts.trader = spec["trader"] == "1";
	facts.race = spec["race"];
	facts.faction = spec["faction"];
	facts.character = spec["character"];
	for (std::size_t i = 0; i < set.roots.size(); ++i)
	{
		const DialogueDef& root = book.dialogue(set.roots[i]);
		std::string events;
		for (int event = 0; event < kDialogueEventCount; ++event)
		{
			for (const std::uint32_t position : set.byEvent[event])
			{
				if (position == i)
					events += (events.empty() ? "" : ",") + std::to_string(event);
			}
		}
		std::printf("root %s events=%s conds=%u reqPT=%u forbPT=%u reqLT=%u forbLT=%u", std::string(book.sidOf(root.sid)).c_str(),
			events.c_str(), root.gate.conds.count, root.gate.reqPersonality, root.gate.forbPersonality, root.gate.reqMemory,
			root.gate.forbMemory);
		if (useFacts)
		{
			const std::string why = book.staticExclusion(root.gate, facts, set.sources);
			std::printf(" static=%s", why.empty() ? "pass" : why.c_str());
		}
		std::printf("\n");
	}
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	std::string dataDir = KENSHI_DATA;
	std::vector<std::string> packages;
	std::string facts;
	bool sets = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		const bool hasValue = i + 1 < argc;
		if (arg == "--data" && hasValue)
			dataDir = argv[++i];
		else if (arg == "--package" && hasValue)
			packages.emplace_back(argv[++i]);
		else if (arg == "--facts" && hasValue)
			facts = argv[++i];
		else if (arg == "--sets")
			sets = true;
		else
		{
			std::cerr << "usage: ksim_dialogue_coverage [--data DIR] [--sets] [--package SID_OR_NAME [--facts SPEC]]\n";
			return 2;
		}
	}
	try
	{
		const auto book = DialogueBook::build(std::make_shared<kdata::GameData>(kworld::loadMergedGameData(dataDir)));
		if (!packages.empty())
		{
			int status = 0;
			for (const std::string& package : packages)
				status |= printPackage(*book, package, facts);
			return status;
		}
		printCensus(*book);
		if (sets)
			printSets(*book);
	} catch (const std::exception& error) {
		std::cerr << "ksim_dialogue_coverage: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
