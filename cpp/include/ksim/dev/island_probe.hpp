#pragma once
// DEV ONLY, KSIM_PROFILE only (kenshi-k16.6.305, docs/PROFILING.md "Scheduling measurements"): interaction islands of the combat arena.
// Union-find over the characters within one range (the sensor's longest sight, 1500 u) on a uniform cell grid, and over the target links.
// Measurement only: nothing in the sim reads it.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ksim::dev
{

struct IslandStats
{
	std::size_t components = 0;
	std::size_t largest = 0;
	std::size_t second = 0;
	std::size_t singletons = 0;
	std::size_t largestIndex = 0; // the root of the largest, for the share
	std::size_t nonSingleton = 0;
};

class IslandProbe
{
public:
	void reset(std::size_t n)
	{
		parent_.resize(n);
		for (std::size_t i = 0; i < n; ++i)
			parent_[i] = static_cast<std::uint32_t>(i);
	}
	std::size_t find(std::size_t i)
	{
		while (parent_[i] != i)
		{
			parent_[i] = parent_[parent_[i]];
			i = parent_[i];
		}
		return i;
	}
	void join(std::size_t a, std::size_t b)
	{
		a = find(a);
		b = find(b);
		if (a != b)
			parent_[a > b ? a : b] = static_cast<std::uint32_t>(a < b ? a : b);
	}
	// Joins every pair of `active` points closer than `range` (a cell grid of cell size `range`: the 3x3 neighbourhood).
	void joinByRange(const std::vector<double>& x, const std::vector<double>& z, const std::vector<char>& active, double range)
	{
		cells_.clear();
		const auto key = [range](double px, double pz)
		{
			return (static_cast<std::int64_t>(std::floor(px / range)) << 32) ^ (static_cast<std::int64_t>(std::floor(pz / range)) & 0xffffffff);
		};
		for (std::size_t i = 0; i < x.size(); ++i)
			if (active[i])
				cells_[key(x[i], z[i])].push_back(static_cast<std::uint32_t>(i));
		const double r2 = range * range;
		for (std::size_t i = 0; i < x.size(); ++i)
		{
			if (!active[i])
				continue;
			const auto cx = static_cast<std::int64_t>(std::floor(x[i] / range));
			const auto cz = static_cast<std::int64_t>(std::floor(z[i] / range));
			for (std::int64_t dx = -1; dx <= 1; ++dx)
				for (std::int64_t dz = -1; dz <= 1; ++dz)
				{
					const auto it = cells_.find(((cx + dx) << 32) ^ ((cz + dz) & 0xffffffff));
					if (it == cells_.end())
						continue;
					for (const std::uint32_t j : it->second)
						if (j > i && (x[i] - x[j]) * (x[i] - x[j]) + (z[i] - z[j]) * (z[i] - z[j]) <= r2)
							join(i, j);
				}
		}
	}
	// The components among the `active` points (an inactive one is not counted).
	IslandStats stats(const std::vector<char>& active)
	{
		IslandStats out;
		size_.assign(parent_.size(), 0);
		for (std::size_t i = 0; i < parent_.size(); ++i)
			if (active[i])
				++size_[find(i)];
		for (std::size_t i = 0; i < size_.size(); ++i)
		{
			const std::size_t s = size_[i];
			if (s == 0)
				continue;
			++out.components;
			out.singletons += s == 1 ? 1 : 0;
			if (s > out.largest)
			{
				out.second = out.largest;
				out.largest = s;
				out.largestIndex = i;
			} else if (s > out.second) {
				out.second = s;
			}
		}
		out.nonSingleton = out.components - out.singletons;
		return out;
	}

private:
	std::vector<std::uint32_t> parent_;
	std::vector<std::size_t> size_;
	std::unordered_map<std::int64_t, std::vector<std::uint32_t>> cells_;
};

} // namespace ksim::dev
