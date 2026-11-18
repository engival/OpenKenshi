#pragma once

// data/globalPathing.path reader (docs/formats/GLOBAL_PATHING_FORMAT.md).
// Reference data only; the portal/edge semantics are only partly known.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace kworld {

struct PathEdge
{
	std::uint32_t target = 0; // node id
	float cost = 0.0f; // UNSURE: formula not specified; kept verbatim
	std::uint32_t edgeClass = 0; // UNSURE: 20 or 27; meanings unknown
};

struct PathNode
{
	std::uint32_t id = 0;
	std::uint32_t cellX = 0; // header field `a`, block column
	std::uint32_t cellZ = 0; // header field `b`, block row
	std::uint32_t neighbourX = 0; // `na`
	std::uint32_t neighbourZ = 0; // `nb`
	float minX = 0.0f;
	float minZ = 0.0f;
	float maxX = 0.0f;
	float maxZ = 0.0f;
	std::vector<PathEdge> edges;
};

class GlobalPathing
{
public:
	static GlobalPathing load(const std::string& path);
	static GlobalPathing parse(std::span<const std::byte> bytes);

	std::uint32_t version = 0;
	std::uint32_t count = 0;
	// UNSURE: equals count in the shipped file; meaning unknown.
	std::uint32_t count2 = 0;
	std::vector<PathNode> nodes;
	// UNSURE: four u32 after the node blocks; meaning unknown, kept verbatim.
	std::array<std::uint32_t, 4> trailer{};
	std::size_t byteSize = 0;

	std::size_t directedEdgeCount() const noexcept;
	std::vector<std::string> validate() const;
};

} // namespace kworld
