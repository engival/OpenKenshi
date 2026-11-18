#include "kworld/navhash.hpp"

#include <cmath>

namespace kworld {

std::uint32_t comb(std::uint32_t state, std::uint32_t value)
{
	return state ^ (value + 0x9e3779b9u + (state << 6) + (state >> 2));
}

std::uint32_t F(float value)
{
	// The exe reads an f32, subtract-wraps 2^32, then truncates toward zero and xores the deadbeef mask.
	double v = value;
	if (v >= 4294967296.0)
		v = std::fmod(v, 4294967296.0);
	return static_cast<std::uint32_t>(static_cast<std::int64_t>(v)) ^ 0xdeadbeefu;
}

std::uint32_t buildingHash(double x, double z, bool destroyed)
{
	// The engine's positions are f32; the hash sees the truncated int64 of that f32.
	std::uint32_t state = comb(0, F(static_cast<float>(x)));
	state = comb(state, F(static_cast<float>(z)));
	for (int orientation = 0; orientation < 4; ++orientation)
		state = comb(state, F(0.0f)); // all shipped orientation words truncate to 0 (navhash.md section 7)
	if (destroyed)
		state = comb(state, 1);
	return state;
}

std::uint32_t interiorHash(double x, double z, bool destroyed, bool destroyedGate, std::span<const std::uint32_t> children)
{
	std::uint32_t state = buildingHash(x, z, destroyedGate);
	for (const std::uint32_t child : children)
		state = comb(state, child);
	if (destroyed)
		state = comb(state, 0x100);
	return state;
}

std::uint32_t zoneHash(std::span<const std::uint32_t> buildingHashes, bool hasVegetation, std::uint32_t vegetation)
{
	std::uint32_t state = 0;
	for (const std::uint32_t hash : buildingHashes)
		state = comb(state, hash);
	if (hasVegetation)
		state = comb(state, vegetation);
	return state;
}

std::uint32_t vegetationWord(std::span<const FoliageInstance> trees)
{
	// `1406d2050` in the tree spawner, in spawn order: per instance that passes the mesh gate, the horizontal
	// position and the HEIGHT (world y, not z), as f32 truncated toward zero. The positions are the raw scatter
	// positions, before the loader's u16 page encoding (kinspect findings/navhash.md section 11).
	std::uint32_t state = 0;
	for (const FoliageInstance& tree : trees)
	{
		if (!tree.navFold)
			continue;
		state = comb(state, F(static_cast<float>(tree.x)));
		state = comb(state, F(static_cast<float>(tree.y)));
	}
	return state;
}

} // namespace kworld
