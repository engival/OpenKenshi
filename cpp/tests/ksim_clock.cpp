// Game clock: tick <-> day/hour/minute round-trips, the new-game start time
// (Day 1, 10:00) and the derived intervals (clock.hpp). No game data needed.

#include "ksim/clock.hpp"

#include "test_util.hpp"

#include <iostream>
#include <string>

int main()
{
	try
	{
		test::require(ksim::kTicksPerGameHour == 2182, "game-hour is not 2182 ticks");
		test::require(ksim::gameTimeAt(0).day == 0 && ksim::gameTimeAt(0).hour == 0
			&& ksim::gameTimeAt(0).minute == 0, "tick 0 is not day 0 00:00");

		const std::uint64_t start = ksim::tickAt(ksim::kNewGameDay,
			ksim::kNewGameHour, ksim::kNewGameMinute);
		const ksim::GameTime startTime = ksim::gameTimeAt(start);
		test::require(startTime.day == 1 && startTime.hour == 10 && startTime.minute == 0,
			"new-game start is not Day 1 10:00");
		const ksim::GameTime before = ksim::gameTimeAt(start - 1);
		test::require(before.hour == 9 && before.minute == 59,
			"tick before the start is not 09:59");

		// Every minute of the first 40 days round-trips, and is the first
		// tick showing that minute.
		for (std::uint32_t minutes = 0; minutes < 40 * 24 * 60; ++minutes)
		{
			const std::uint64_t day = minutes / (24 * 60);
			const int hour = static_cast<int>(minutes / 60 % 24);
			const int minute = static_cast<int>(minutes % 60);
			const std::uint64_t tick = ksim::tickAt(day, hour, minute);
			const ksim::GameTime time = ksim::gameTimeAt(tick);
			test::require(time.day == day && time.hour == hour && time.minute == minute,
				"round-trip failed at minute " + std::to_string(minutes));
			if (tick > 0)
			{
				const ksim::GameTime earlier = ksim::gameTimeAt(tick - 1);
				test::require(earlier.minute != minute || earlier.hour != hour
					|| earlier.day != day,
					"tickAt is not the first tick of minute " + std::to_string(minutes));
			}
		}
		std::cout << "ksim_clock: start tick " << start << ", ticks per game-day "
			<< ksim::kTicksPerGameDay << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_clock: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
