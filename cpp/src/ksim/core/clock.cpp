#include "ksim/core/clock.hpp"

#include <cmath>

namespace ksim {

GameTime gameTimeAt(std::uint64_t tick) noexcept
{
	// The epsilon keeps tickAt()'s ceil from landing a rounding step short of
	// its minute; neighbouring ticks are 1/36.4 of a minute apart.
	const auto minutes = static_cast<std::uint64_t>(
		std::floor(static_cast<double>(tick) / kTicksPerGameMinute + 1e-9));
	GameTime time;
	time.day = minutes / 1440;
	time.hour = static_cast<int>(minutes % 1440 / 60);
	time.minute = static_cast<int>(minutes % 60);
	return time;
}

std::uint64_t tickAt(std::uint64_t day, int hour, int minute) noexcept
{
	const double minutes = static_cast<double>(day) * 1440.0 + hour * 60.0 + minute;
	return static_cast<std::uint64_t>(std::ceil(minutes * kTicksPerGameMinute));
}

} // namespace ksim
