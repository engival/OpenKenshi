#pragma once

// The sim-owned clock (ARCHITECTURE.md "Runtime state": the sim owns the
// clock). A fixed tick; the calendar is derived from the absolute tick count
// in state::Clock. Game speed changes ticks per wall second, never these.

#include <cstdint>

namespace ksim {

inline constexpr double kTickSeconds = 0.05;

// One game day at 1x speed: 24 / 0.0091666663 game-hours per second
// (docs/reference/SKY_EXE.md, C15: DAT_1416fd540 = 2618.1819).
inline constexpr double kGameDaySeconds = 2618.1819;
inline constexpr double kTicksPerGameDay = kGameDaySeconds / kTickSeconds;
inline constexpr double kTicksPerGameMinute = kTicksPerGameDay / 1440.0;

// 2181.8 ticks rounded: the Strategic interval (ARCHITECTURE.md "Simulation").
inline constexpr std::uint64_t kTicksPerGameHour =
	static_cast<std::uint64_t>(kTicksPerGameDay / 24.0 + 0.5);

// The ticks from `stamp` to `now`, 0 when the stamp is ahead of the clock: an unsigned `now - stamp` wraps to ~1.8e19 when a stamp outlives a
// clock that went back (Simulation::reload to an earlier save, a debug rewind), which fires every give-up at once (kenshi-k16.6.65).
// `static_cast<double>(now - stamp)` is NOT safe: the subtraction wraps first.
inline constexpr std::uint64_t ticksSince(std::uint64_t now, std::uint64_t stamp) noexcept
{
	return now > stamp ? now - stamp : 0;
}

// The one game-hour length. Integer tick counts (intervals, deadlines, holds) use kTicksPerGameHour; fractional game-hour math (the
// hour of the day, a stamp in hours, an age in hours) uses gameHoursIn, which divides by the exact kTicksPerGameDay / 24, so the two
// agree to under half a tick per hour. No other file writes an hour length.
inline constexpr double kGameHourSeconds = kGameDaySeconds / 24.0; // wall seconds of one game hour at 1x
inline constexpr double gameHoursIn(std::uint64_t ticks) noexcept { return static_cast<double>(ticks) / (kTicksPerGameDay / 24.0); }

// Kenshi's calendar: game minute 0 is day 0, 00:00; the UI's "Day 1" is
// day 1 (docs/formats/SAVE_FORMAT.md "Clock unit").
struct GameTime
{
	std::uint64_t day = 0;
	int hour = 0;
	int minute = 0;
};

GameTime gameTimeAt(std::uint64_t tick) noexcept;

// First tick at or after the start of that game minute.
std::uint64_t tickAt(std::uint64_t day, int hour, int minute) noexcept;

// A new game starts on day 1 at 10:00: squads already in town at world start
// carry PLATOON `towntime` 34.0 h = 24 * 1 + 10 (SAVE_FORMAT.md, PLATOON).
// Inferred from saves, not from the exe; spawnDefaultWorld takes the start
// tick as a parameter.
inline constexpr std::uint64_t kNewGameDay = 1;
inline constexpr int kNewGameHour = 10;
inline constexpr int kNewGameMinute = 0;

} // namespace ksim
