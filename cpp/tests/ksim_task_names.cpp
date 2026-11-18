// Goal display names (kenshi-k16.6.206): every AI_TASK enum in the game data has an exe display name (ksim/ai/task_names.hpp), except the ids
// listed below. The table must be sorted and unique, and the Goal row never falls back to the data record's own name.

#include "ksim/ai/task_names.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <iostream>
#include <set>
#include <string>
#include <utility>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	try
	{
		constexpr std::int32_t kAiTaskType = 22;
		using namespace ksim;
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);

		const auto table = exeTaskNames();
		test::require(table.size() > 200, "the table is the exe's full list, not a subset");
		for (std::size_t i = 1; i < table.size(); ++i)
			test::require(table[i - 1].task < table[i].task, "the table is sorted by id, one name per id");

		std::set<std::int32_t> missing;
		std::size_t walked = 0;
		for (const kdata::Record* record : gd.byType(kAiTaskType))
		{
			const std::int32_t task = record->intOr("enum", -1);
			if (task <= 0) continue;
			++walked;
			if (exeTaskName(static_cast<std::uint16_t>(task)) == nullptr)
				missing.insert(task);
		}
		test::require(walked > 100, "AI_TASK records were walked");

		// Dead enum: the exe registers no name for it, so it is the only id allowed to be nameless.
		const std::set<std::int32_t> pendingOrNoName = {
			18, // RAID_TOWN: dead enum (AI_COVERAGE.md), the exe has no string for it; the old table's "Raiding" was a guess
		};
		// The 14 ids kenshi-rwh.132 decoded (kenshi-k16.6.224), pinned to the exe's strings.
		const std::pair<std::uint16_t, const char*> decoded[] = {
			{ 1, "Moving" }, { 13, "Attacking attackers of" }, { 17, "Protection" }, { 30, "Holding position" },
			{ 45, "Bodyguard" }, { 55, "Recruiting" }, { 91, "Unjamming machine" }, { 94, "Try filling machine" },
			{ 101, "Assault" }, { 106, "Find and put in bed" }, { 112, "Find and put in cage" },
			{ 113, "Try emptying machine" }, { 114, "Ditching to make space" }, { 115, "Finding building mats" },
		};
		for (const auto& [task, name] : decoded)
		{
			const char* got = exeTaskName(task);
			test::require(got != nullptr && std::string(got) == name, "a decoded id carries the exe's display name");
		}
		for (const std::int32_t task : missing)
			if (pendingOrNoName.count(task) == 0)
				std::cerr << "AI_TASK " << task << " has no exe display name\n";
		for (const std::int32_t task : missing)
			test::require(pendingOrNoName.count(task) != 0, "every AI_TASK enum has an exe name or is listed as pending / nameless");
		std::cout << "ksim_task_names: " << walked << " AI_TASK enums, " << missing.size() << " listed nameless\n";
		return 0;
	} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << "\n"; return 1; }
}
