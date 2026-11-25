#pragma once

// Which `parts` variant a placed building shows, by the original's rule (kinspect findings/rwh206_town_prop_selection.md):
// MSVCR100 rand() seeded per placement from its world x/z, so the pick is the same in every load and new game. One function
// serves collision (the nav bake) and the client's draw. Our own code written from those notes.

#include <cstdint>
#include <string>
#include <vector>

namespace kdata {
class GameData;
struct Record;
} // namespace kdata

namespace kworld {

enum class PartStage
{
	Exterior,     // BUILDING `parts`
	Interior,     // BUILDING `interior` (seed reset first)
	InteriorMask, // BUILDING `interior mask` (seed reset first)
	Door          // a door record's `parts`, rolled with the parent placement's x/z (a door is created at the parent's position, rwh.246 section 2)
};

// The per-placement seed: comb(trunc(x) ^ 0xdeadbeef, trunc(z) ^ 0xdeadbeef). y and orientation do not enter it.
std::uint32_t partsSeed(float x, float z);

// The MSVCR100 rand(): state = state * 214013 + 2531011, returns (state >> 16) & 0x7fff.
class MsvcRand
{
public:
	explicit MsvcRand(std::uint32_t seed) : state_(seed) {}
	int next() { state_ = state_ * 214013u + 2531011u; return static_cast<int>((state_ >> 16) & 0x7fff); }
private:
	std::uint32_t state_;
};

// Layout furniture and interior pieces pass their OWN placement x/z (each is a Building that seeds itself, rwh.246 section 1).
// The part record ids `building` shows at world (x, z) for `stage`, in creation order: group-0 picks in data order, then one
// weighted pick per group > 0 (groups in reverse first-appearance order, rwh.246 section 3), each followed by its own `parts` on the same stream. `gameData` resolves the nested parts.
std::vector<std::string> selectBuildingParts(const kdata::GameData& gameData, const kdata::Record& building,
	float x, float z, PartStage stage);

// The same picks with the nesting kept: `depth` 0 is a pick of the building's own list, depth d + 1 a pick from the `parts` of the
// nearest earlier pick at depth d (pre-order, the order of selectBuildingParts). The client draws each part at its parent's offset.
struct PickedPart
{
	std::string id;
	int depth;
};
std::vector<PickedPart> selectBuildingPartTree(const kdata::GameData& gameData, const kdata::Record& building,
	float x, float z, PartStage stage);

} // namespace kworld
