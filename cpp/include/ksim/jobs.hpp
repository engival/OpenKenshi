#pragma once

// The original's per-character jobs list (ARCHITECTURE.md "V1: the original's per-character jobs", SIMLINK.md JOBS 34 and orders 76-78,
// source: kinspect findings/jobs17_static.md). The state is state::Jobs on the character's entity; the rules are the pure functions
// below (the exe's addTask and drag rules), the order checks and the executors are Simulation::checkJobOrder / runJobOrder / stepJobs
// (jobs.cpp).

#include "ksim/state.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ksim {

// One persistent-capable task type (jobs17 Appendix A: the types whose TaskData has `kind` != 0). `replacement` is the type a row is
// stored as (0 = itself), `companion` the type added directly after it (0 = none). `executor` is whether the sim can run it yet.
struct JobType
{
	std::uint16_t type = 0;
	const char* name = "";
	std::uint8_t kind = 0; // 1 normal, 2 turret
	bool targeted = false; // the row has a target; a duplicate is the same type and target (else the same type)
	std::uint16_t replacement = 0;
	std::uint16_t companion = 0;
	bool executor = false;
};

// The Appendix A entry of a type; null when the type has `kind` 0 or is unknown (a plain order, never a row).
const JobType* jobType(std::uint16_t type) noexcept;
// Types above this have no TaskData at all (job_add answers bad_value; the exe's registry ends at 289).
inline constexpr std::uint16_t kJobTypeMax = 300;

// Whether a row of this type still runs, and shows undimmed, with JOBS off: Attacking enemies, Staying close (both), Bodyguard,
// Robotics, Medic, Splint Rigging (FUN_14050c1c0).
bool jobRunsWhenOff(std::uint16_t type) noexcept;

// The exe's addTask for a persistent order: the replacement type is applied, a duplicate is dropped, the companion row follows the
// main row directly (at the end when the main row was a duplicate), and adding a row turns `enabled` on. Returns the rows added
// (0 = nothing changed). The caller checked the type with jobType; a targeted type's `target` is kept, any other is stored as 0.
int addJob(state::Jobs& jobs, std::uint16_t type, std::uint64_t target);
// The exe's drag rule: the row is taken out, then inserted at `index` of the shortened list (kLast or past the end = last). False when
// no row has that jobId.
bool moveJob(state::Jobs& jobs, std::uint32_t jobId, std::uint16_t index);
// Removes the row; false when no row has that jobId.
bool removeJob(state::Jobs& jobs, std::uint32_t jobId);

// The JOBS record's content for one character (SIMLINK.md section 6).
struct JobsView
{
	bool enabled = true;
	std::vector<state::JobRow> rows;

	bool operator==(const JobsView&) const = default;
};

// Ticks between two evaluations of a character's goal (0.5 s; not the exe's number, the sim has no per-character planner clock yet).
inline constexpr std::uint64_t kJobIntervalTicks = 10;
// How far an ally's wound is looked for by Medic (xz units). UNSURE: ours; the exe's reach is not read.
inline constexpr double kJobMedicRange = 600.0;
// How far a building is looked for by Engineer (xz units). UNSURE: ours; the exe's finder (BuildingFinderForEngineers) reaches the town
// point's grid cell, whose size is not read.
inline constexpr double kJobEngineerRange = 1000.0;

} // namespace ksim
