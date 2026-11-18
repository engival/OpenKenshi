#pragma once

// Deterministic helpers shared by the region scheduler tests.

#include "ksim/world/region.hpp"
#include "ksim/core/simulation.hpp"

#include "test_util.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace region_test {

inline std::uint64_t splitMix64(std::uint64_t value)
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

inline double unit(std::uint64_t value)
{
	return static_cast<double>(value >> 11) * 0x1.0p-53;
}

// Scattered positions across the central part of the world, deterministic.
inline std::vector<std::pair<double, double>> scattered(std::size_t count,
	std::uint64_t seed, double margin = 20000.0)
{
	constexpr double min = -147456.0;
	constexpr double extent = 294912.0;
	std::vector<std::pair<double, double>> positions;
	positions.reserve(count);
	std::uint64_t state = seed;
	for (std::size_t i = 0; i < count; ++i)
	{
		state = splitMix64(state);
		const double x = min + margin + unit(state) * (extent - 2.0 * margin);
		state = splitMix64(state);
		const double z = min + margin + unit(state) * (extent - 2.0 * margin);
		positions.emplace_back(x, z);
	}
	return positions;
}

// The stand-in workload on, so the engine has something to schedule.
inline ksim::RegionConfig config(int workers, bool fullDetail)
{
	ksim::RegionConfig value;
	value.workerThreads = workers;
	value.fullDetailAll = fullDetail;
	value.standIn = true;
	return value;
}

// A simulation over a template-free world; the Clock starts at tick 0.
inline ksim::Simulation simulation(std::uint64_t seed, const ksim::RegionConfig& config)
{
	return ksim::Simulation(ksim::World::empty(), seed, config);
}

} // namespace region_test
