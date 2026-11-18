#pragma once

#include <cstdint>

// The original's AI_TASK enum names (SIMLINK "CHAR_STATS", the AI tail rows 16 / 18), as docs/formats/AI_TASK_ENUMS.md lists them. The table
// is generated at build time by tools/gen_ai_task_names.py, so no name is typed in the client: the doc is the only place one lives.
const char* aiTaskName(uint16_t value);	// "MOVE_ON_FREE_WILL"; nullptr: 0xFFFF (no task) or a value past the table
