#pragma once

// A bounding-volume tree over world-space collision triangles (task AZ, line of sight phase 1; the tree is plain geometry so
// the physics work can share it). Built from the shapes CollisionLibrary places (WorldShape) or plain triangle soup, queried
// by a segment: the first hit's fraction along the segment. Two-sided (any face blocks). Deterministic: a median split on the
// longest centroid axis, no randomness, so the same shapes always build the same tree.

#include "kworld/collision.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kworld {

class HullTree
{
public:
	// Appends the triangles of every shape (WorldShape::vertices / indices are world coordinates; a shape with no triangles
	// is skipped). Replaces any earlier content.
	void build(const std::vector<WorldShape>& shapes);
	void buildTriangles(const std::vector<float>& vertices, const std::vector<std::uint32_t>& indices);
	void clear();

	// True when the segment a -> b hits a triangle; `t` is the first hit as a fraction (0 at a, 1 at b). The bounds check
	// runs first, so a segment outside the tree costs a few comparisons.
	bool trace(const std::array<double, 3>& a, const std::array<double, 3>& b, double& t) const;
	// The same answer without the tree (a linear scan of every triangle): the tests' reference for `trace`.
	bool traceBrute(const std::array<double, 3>& a, const std::array<double, 3>& b, double& t) const;

	bool empty() const noexcept { return tris_.empty(); }
	std::size_t triangleCount() const noexcept { return tris_.size() / 9; }
	// The bytes the tree holds: the triangles and the nodes (the caller's own storage is not counted).
	std::size_t memoryBytes() const noexcept { return tris_.capacity() * sizeof(float) + nodes_.capacity() * sizeof(Node); }
	std::array<double, 3> min{}; // empty: 0, 0, 0
	std::array<double, 3> max{};

private:
	struct Node
	{
		float min[3]{};
		float max[3]{};
		std::uint32_t first = 0; // a leaf: the first triangle; internal: the left child (the right is first + 1)
		std::uint32_t count = 0; // 0 = internal, else the leaf's triangles
	};
	void buildNode(std::uint32_t node, std::uint32_t begin, std::uint32_t end);
	// `o` and `d` are the origin and the direction (b - a), the form segmentBox and segmentTriangle take.
	void traceNode(std::uint32_t node, const std::array<double, 3>& o, const std::array<double, 3>& d, double& best) const;

	std::vector<float> tris_; // 9 floats per triangle
	std::vector<std::uint32_t> order_; // triangle indices while building
	std::vector<Node> nodes_;
};

} // namespace kworld
