// The exe's Goal-row display names for the AI_TASK ids (kinspect findings/inspect_goal.md, task_names.tsv).

#pragma once

#include <cstdint>
#include <span>

namespace ksim {

struct ExeTaskName
{
	std::uint16_t task;
	const char* name;
};

// The exe's display name for an AI_TASK enum, or nullptr when the exe has none (the 14 ids not decoded yet are left out of the table).
const char* exeTaskName(std::uint16_t task) noexcept;

// The whole table, sorted by id (the test walks it).
std::span<const ExeTaskName> exeTaskNames() noexcept;

}
