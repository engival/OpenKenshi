#pragma once

// Gate -> town binding, shared by the spawn rule (spawn.cpp) and the door layer (DoorSystem::bind): a gate belongs to the nearest
// town centre by xz with NO cutoff (GATE_CREWS_LIVE.md section 3: the exe's 140927f10 starts at FLT_MAX and has no 1,500
// constant). The exe takes the arg-max with strict <, so on a tie the first town in the given order wins; callers pass the towns
// in placement order.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ksim {

// The index of the town nearest to (x, z); `count` when there are no towns. `centre(i)` returns the i-th town's {x, z}.
template <typename Centre>
std::size_t nearestTownIndex(double x, double z, std::size_t count, Centre&& centre)
{
	std::size_t best = count;
	double bestDistance = std::numeric_limits<double>::infinity();
	for (std::size_t i = 0; i < count; ++i)
	{
		const auto [cx, cz] = centre(i);
		const double far = std::hypot(x - cx, z - cz);
		if (far < bestDistance)
		{
			bestDistance = far;
			best = i;
		}
	}
	return best;
}

// A gate crew's binding (GATE_CREWS_LIVE.md sections 3 and 5). The exe's findGate (140338690) tries the task's own gate, then the
// home building's (Ownerships+0x38), then Town::vf97, the town's gateway nearest the character; it never asks whether a gate is
// free. Ours does: one crew per gate (4a/4b are the original's bug), so a step that would double a gate is skipped.
enum class GateSource : std::uint8_t
{
	Own,
	Home,
	Town,
	None // no gate: the crew stands down and is an ordinary resident squad
};

struct GateChoice
{
	std::uint64_t key = 0;
	GateSource source = GateSource::None;
};

// `usable(key)`: the gate exists and no other crew holds it. `nearestFree()`: the town's usable gateway nearest the squad, 0 for none.
template <typename Usable, typename NearestFree>
GateChoice resolveGate(std::uint64_t own, std::uint64_t home, Usable&& usable, NearestFree&& nearestFree)
{
	if (own != 0 && usable(own))
		return {own, GateSource::Own};
	if (home != 0 && home != own && usable(home))
		return {home, GateSource::Home};
	if (const std::uint64_t key = nearestFree(); key != 0)
		return {key, GateSource::Town};
	return {};
}

} // namespace ksim
