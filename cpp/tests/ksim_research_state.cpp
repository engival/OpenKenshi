// Research state from the FCS data (kenshi-h2v.17.7, CRAFTING_SLICE 2.3, 3, 9.2) against the base data: a new game (`_Default Start`) unlocks only Dried
// Meat among items and the Cooking Stove can craft [Dried Meat]; completing one research adds exactly its enables; a `blueprint only` record is
// locked with that reason, a record above the tech level is locked with that one; a requirement chain lists only the first link; a level upgrade
// raises the tech level. KSIM_RESEARCH_VERBOSE=1 prints the census.

#include "ksim/save/newgame.hpp"
#include "ksim/economy/recipes.hpp"
#include "ksim/economy/research.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;
namespace rs = ksim::research;

namespace {

std::string nameOf(const kdata::GameData& data, const std::string& sid)
{
	const kdata::Record* record = data.find(sid);
	return record != nullptr ? record->name : "?";
}

std::vector<std::string> refs(const kdata::Record& record, std::string_view category)
{
	std::vector<std::string> out;
	if (const kdata::RefCategory* list = record.findRefs(category))
		for (const kdata::RefEntry& entry : list->entries)
			if (!entry.opaqueEntry && !entry.targetID.empty())
				out.push_back(entry.targetID);
	return out;
}

// The crafting BUILDING_FUNCTIONALITY of the building named `name`.
const kdata::Record* benchOf(const kdata::GameData& data, const std::string& name)
{
	for (const kdata::Record* building : data.byType(kworld::itemTypeByName("BUILDING")))
	{
		if (building->name != name || building->boolOr("REMOVED", false))
			continue;
		for (const std::string& sid : refs(*building, "functionality"))
			if (const kdata::Record* functionality = data.find(sid); functionality != nullptr && functionality->intOr("function", -1) == 10)
				return functionality;
	}
	return nullptr;
}

} // namespace

int main()
try
{
	const kdata::GameData data = kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA));
	const rs::Book book(data);
	const std::vector<std::string> start = startResearch(data, {});
	test::require(start.size() == 1, "the default start completes one research");
	const rs::Snapshot now = book.evaluate(start);
	const auto item = static_cast<std::size_t>(rs::Kind::Item);
	std::map<rs::State, int> census;
	std::map<rs::Reason, int> why;
	for (const rs::Row& row : now.rows)
	{
		++census[row.state];
		++why[row.reason];
	}
	if (std::getenv("KSIM_RESEARCH_VERBOSE") != nullptr)
		std::cout << "start: tech level " << now.techLevel << ", rows " << now.rows.size() << ", completed " << census[rs::State::Completed] << " (listed as finished " << now.completed.size()
			<< "), listable " << census[rs::State::Listable] << ", locked " << census[rs::State::Locked] << " (requirement " << why[rs::Reason::Requirement]
			<< ", tech level " << why[rs::Reason::TechLevel] << ", blueprint only " << why[rs::Reason::BlueprintOnly] << ")\n";

	// 1. The start: only Dried Meat among items, and the Cooking Stove makes it.
	test::require(now.unlocked[item].size() == 1 && nameOf(data, now.unlocked[item].front()) == "Dried Meat", "a new game unlocks only Dried Meat among items (" + std::to_string(now.unlocked[item].size()) + " unlocked)");
	const kdata::Record* stove = benchOf(data, "Cooking Stove");
	test::require(stove != nullptr, "no crafting functionality for the Cooking Stove");
	const std::vector<recipes::Craftable> stoveList = recipes::craftable(data, *stove, now);
	test::require(stoveList.size() == 1 && stoveList.front().kind == rs::Kind::Item && nameOf(data, stoveList.front().sid) == "Dried Meat", "the Cooking Stove's list is [Dried Meat] (" + std::to_string(stoveList.size()) + " entries)");
	test::require(now.techLevel == 1, "a new game's tech level is 1");

	// 2. A blueprint-only record is locked with that reason; a record above the tech level, with that one.
	bool blueprint = false;
	bool above = false;
	for (const rs::Row& row : now.rows)
	{
		const kdata::Record* record = data.find(row.sid);
		if (row.state == rs::State::Completed)
			continue; // the start's own record is blueprint-only and finished
		if (record->boolOr("blueprint only", false) && row.state == rs::State::Locked && row.reason == rs::Reason::BlueprintOnly)
			blueprint = true;
		if (!record->boolOr("blueprint only", false) && row.level > now.techLevel)
		{
			test::require(row.state == rs::State::Locked && row.reason == rs::Reason::TechLevel, "a record above the tech level is locked as TechLevel: " + row.sid);
			above = true;
		}
		if (record->boolOr("blueprint only", false))
			test::require(row.state == rs::State::Locked && row.reason == rs::Reason::BlueprintOnly, "a blueprint-only record is locked as BlueprintOnly: " + row.sid);
	}
	test::require(blueprint && above, "the base data has a blueprint-only and an above-tech-level record");
	test::require(census[rs::State::Listable] > 0, "a new game has research to list");

	// 3. Completing one listable research adds exactly its enables.
	const rs::Row* picked = nullptr;
	for (const rs::Row& row : now.rows)
	{
		const kdata::Record* record = data.find(row.sid);
		if (row.state != rs::State::Listable)
			continue;
		bool enables = false;
		for (std::size_t kind = 0; kind < rs::kKindCount; ++kind)
			enables = enables || !refs(*record, rs::kindField(static_cast<rs::Kind>(kind))).empty();
		if (enables && !record->boolOr("is level upgrade", false))
		{
			picked = &row;
			break;
		}
	}
	test::require(picked != nullptr, "no listable research enables anything");
	std::vector<std::string> after = start;
	after.push_back(picked->sid);
	const rs::Snapshot next = book.evaluate(after);
	const kdata::Record& record = *data.find(picked->sid);
	for (std::size_t kind = 0; kind < rs::kKindCount; ++kind)
	{
		std::set<std::string> added(next.unlocked[kind].begin(), next.unlocked[kind].end());
		for (const std::string& sid : now.unlocked[kind])
			added.erase(sid);
		const std::vector<std::string> list = refs(record, rs::kindField(static_cast<rs::Kind>(kind)));
		std::set<std::string> expected(list.begin(), list.end());
		for (const std::string& sid : now.unlocked[kind])
			expected.erase(sid);
		test::require(added == expected, "completing " + record.name + " adds exactly its '" + std::string(rs::kindField(static_cast<rs::Kind>(kind))) + "' (" + std::to_string(added.size()) + " vs " + std::to_string(expected.size()) + ")");
	}
	test::require(next.row(picked->sid)->state == rs::State::Completed, "the completed research reads Completed");
	if (std::getenv("KSIM_RESEARCH_VERBOSE") != nullptr)
		std::cout << "completed '" << record.name << "'\n";

	// 4. A level upgrade raises the tech level (the first one the base data lists as researchable once its requirements are in).
	for (const rs::Row& row : now.rows)
	{
		const kdata::Record* upgrade = data.find(row.sid);
		if (!upgrade->boolOr("is level upgrade", false) || row.state != rs::State::Listable)
			continue;
		std::vector<std::string> done = start;
		done.push_back(row.sid);
		const rs::Snapshot raised = book.evaluate(done);
		test::require(raised.techLevel == row.level + 1, "finishing the level upgrade " + upgrade->name + " raises the tech level to " + std::to_string(row.level + 1) + " (got " + std::to_string(raised.techLevel) + ")");
		break;
	}

	// 5. With every level upgrade finished (tech level 6) the tech-level gate is open: a record with an unfinished requirement that has an
	// unfinished requirement of its own is locked as Requirement (the exe lists a record one link away from finished, not two).
	std::vector<std::string> upgraded = start;
	for (const rs::Row& row : now.rows)
		if (data.find(row.sid)->boolOr("is level upgrade", false))
			upgraded.push_back(row.sid);
	const rs::Snapshot open = book.evaluate(upgraded);
	test::require(open.techLevel > now.techLevel, "the level upgrades raise the tech level");
	int chained = 0;
	for (const rs::Row& row : open.rows)
	{
		if (row.state != rs::State::Locked || row.reason != rs::Reason::Requirement)
			continue;
		++chained;
		bool deep = false;
		for (const std::string& requirement : refs(*data.find(row.sid), "requirements"))
		{
			const rs::Row* needed = open.row(requirement);
			if (needed == nullptr || needed->state == rs::State::Completed)
				continue;
			for (const std::string& second : refs(*data.find(requirement), "requirements"))
			{
				const rs::Row* link = open.row(second);
				deep = deep || link == nullptr || link->state != rs::State::Completed;
			}
		}
		test::require(deep, "a Requirement row has an unfinished requirement with an unfinished requirement of its own: " + row.sid);
	}
	if (std::getenv("KSIM_RESEARCH_VERBOSE") != nullptr)
		std::cout << "tech level " << open.techLevel << ": locked as Requirement " << chained << "\n";
	test::require(chained > 0, "no record is locked as Requirement with the tech-level gate open");
	std::cout << "ksim_research_state: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_research_state: FAIL: " << e.what() << "\n";
	return 1;
}
