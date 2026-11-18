#include "ksim/rng.hpp"

namespace ksim {

RngStream::RngStream(std::uint64_t seed)
	: state_(seed != 0 ? seed : 0x9e3779b97f4a7c15ULL)
{
}

std::uint64_t RngStream::next()
{
	std::uint64_t x = state_;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	state_ = x;
	return x * 2685821657736338717ULL;
}

double RngStream::uniform01()
{
	return static_cast<double>(next() >> 11) * 0x1.0p-53;
}

std::int64_t RngStream::between(std::int64_t low, std::int64_t high)
{
	if (high <= low)
		return low;
	// Modulo bias is below 2^-50 for the small ranges used here.
	const std::uint64_t span = static_cast<std::uint64_t>(high - low) + 1;
	return low + static_cast<std::int64_t>(next() % span);
}

std::uint64_t splitMix64(std::uint64_t value) noexcept
{
	value += 0x9e3779b97f4a7c15ULL;
	value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
	value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
	return value ^ (value >> 31);
}

std::uint64_t streamSeed(std::uint64_t seed, std::size_t stream) noexcept
{
	const std::uint64_t salt = 0x9e3779b97f4a7c15ULL
		* static_cast<std::uint64_t>(stream + 1);
	return splitMix64(seed ^ salt);
}

} // namespace ksim
