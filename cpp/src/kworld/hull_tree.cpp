// Bounding-volume tree over collision triangles (kworld/hull_tree.hpp; task AZ). Deterministic: the median split on the
// longest centroid axis, ties by triangle index, so the same shapes always give the same tree.

#include "kworld/hull_tree.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kworld {

namespace {

constexpr std::uint32_t kLeafSize = 8;

// A segment / triangle test (Möller–Trumbore, two-sided). `t` is the fraction along o + t d; true when it lies in (0, best].
// A start on a face counts as no hit (t <= 0), so a ray leaving a surface it starts on is not blocked by it.
bool segmentTriangle(const float* v0, const float* v1, const float* v2, const std::array<double, 3>& o,
	const std::array<double, 3>& d, double& t)
{
	const double e1x = static_cast<double>(v1[0]) - v0[0];
	const double e1y = static_cast<double>(v1[1]) - v0[1];
	const double e1z = static_cast<double>(v1[2]) - v0[2];
	const double e2x = static_cast<double>(v2[0]) - v0[0];
	const double e2y = static_cast<double>(v2[1]) - v0[1];
	const double e2z = static_cast<double>(v2[2]) - v0[2];
	const double px = d[1] * e2z - d[2] * e2y;
	const double py = d[2] * e2x - d[0] * e2z;
	const double pz = d[0] * e2y - d[1] * e2x;
	const double det = e1x * px + e1y * py + e1z * pz;
	if (std::fabs(det) < 1.0e-12)
		return false; // parallel to the face
	const double inv = 1.0 / det;
	const double tx = o[0] - v0[0];
	const double ty = o[1] - v0[1];
	const double tz = o[2] - v0[2];
	const double u = (tx * px + ty * py + tz * pz) * inv;
	if (u < 0.0 || u > 1.0)
		return false;
	const double qx = ty * e1z - tz * e1y;
	const double qy = tz * e1x - tx * e1z;
	const double qz = tx * e1y - ty * e1x;
	const double v = (d[0] * qx + d[1] * qy + d[2] * qz) * inv;
	if (v < 0.0 || u + v > 1.0)
		return false;
	const double hit = (e2x * qx + e2y * qy + e2z * qz) * inv;
	if (hit <= 0.0 || hit > 1.0)
		return false;
	t = hit;
	return true;
}

// The segment's entry fraction into an (inclusive) box, NaN when it misses. Slab test on [0, 1].
double segmentBox(const float* bmin, const float* bmax, const std::array<double, 3>& o, const std::array<double, 3>& d)
{
	double t0 = 0.0;
	double t1 = 1.0;
	for (int axis = 0; axis < 3; ++axis)
	{
		const double lo = bmin[axis];
		const double hi = bmax[axis];
		if (std::fabs(d[axis]) < 1.0e-15)
		{
			if (o[axis] < lo || o[axis] > hi)
				return std::numeric_limits<double>::quiet_NaN();
			continue;
		}
		double a = (lo - o[axis]) / d[axis];
		double b = (hi - o[axis]) / d[axis];
		if (a > b)
			std::swap(a, b);
		t0 = std::max(t0, a);
		t1 = std::min(t1, b);
		if (t0 > t1)
			return std::numeric_limits<double>::quiet_NaN();
	}
	return t0;
}

} // namespace

void HullTree::clear()
{
	tris_.clear();
	order_.clear();
	nodes_.clear();
	min = {};
	max = {};
}

void HullTree::buildTriangles(const std::vector<float>& vertices, const std::vector<std::uint32_t>& indices)
{
	clear();
	const std::size_t vertexCount = vertices.size() / 3;
	tris_.reserve(indices.size() * 3);
	for (std::size_t i = 0; i + 2 < indices.size(); i += 3)
	{
		// A bad index skips its triangle (the reader validates; this is a guard, never a partial triangle).
		const std::uint32_t i0 = indices[i];
		const std::uint32_t i1 = indices[i + 1];
		const std::uint32_t i2 = indices[i + 2];
		if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
			continue;
		for (const std::uint32_t index : { i0, i1, i2 })
		{
			tris_.push_back(vertices[index * 3]);
			tris_.push_back(vertices[index * 3 + 1]);
			tris_.push_back(vertices[index * 3 + 2]);
		}
	}
	const std::size_t triangles = tris_.size() / 9;
	if (triangles == 0)
		return;
	std::array<double, 3> lo{ 1.0e300, 1.0e300, 1.0e300 };
	std::array<double, 3> hi{ -1.0e300, -1.0e300, -1.0e300 };
	for (std::size_t i = 0; i < triangles; ++i)
		for (int v = 0; v < 3; ++v)
			for (int k = 0; k < 3; ++k)
			{
				const double value = tris_[i * 9 + v * 3 + k];
				lo[k] = std::min(lo[k], value);
				hi[k] = std::max(hi[k], value);
			}
	min = lo;
	max = hi;
	order_.resize(triangles);
	for (std::uint32_t i = 0; i < triangles; ++i)
		order_[i] = i;
	nodes_.reserve(triangles / kLeafSize * 2 + 1);
	nodes_.assign(1, Node{});
	buildNode(0, 0, static_cast<std::uint32_t>(triangles));
}

void HullTree::build(const std::vector<WorldShape>& shapes)
{
	std::vector<float> vertices;
	std::vector<std::uint32_t> indices;
	for (const WorldShape& shape : shapes)
	{
		const std::uint32_t base = static_cast<std::uint32_t>(vertices.size() / 3);
		vertices.insert(vertices.end(), shape.vertices.begin(), shape.vertices.end());
		for (const std::uint32_t index : shape.indices)
			indices.push_back(base + index);
	}
	buildTriangles(vertices, indices);
}

void HullTree::buildNode(std::uint32_t node, std::uint32_t begin, std::uint32_t end)
{
	float lo[3]{ 1.0e30f, 1.0e30f, 1.0e30f };
	float hi[3]{ -1.0e30f, -1.0e30f, -1.0e30f };
	std::array<double, 3> centroidLo{ 1.0e300, 1.0e300, 1.0e300 };
	std::array<double, 3> centroidHi{ -1.0e300, -1.0e300, -1.0e300 };
	for (std::uint32_t i = begin; i < end; ++i)
	{
		const float* t = &tris_[static_cast<std::size_t>(order_[i]) * 9];
		std::array<double, 3> c{ 0.0, 0.0, 0.0 };
		for (int v = 0; v < 3; ++v)
			for (int k = 0; k < 3; ++k)
			{
				lo[k] = std::min(lo[k], t[v * 3 + k]);
				hi[k] = std::max(hi[k], t[v * 3 + k]);
				c[k] += t[v * 3 + k] / 3.0;
			}
		for (int k = 0; k < 3; ++k)
		{
			centroidLo[k] = std::min(centroidLo[k], c[k]);
			centroidHi[k] = std::max(centroidHi[k], c[k]);
		}
	}
	nodes_[node].min[0] = lo[0];
	nodes_[node].min[1] = lo[1];
	nodes_[node].min[2] = lo[2];
	nodes_[node].max[0] = hi[0];
	nodes_[node].max[1] = hi[1];
	nodes_[node].max[2] = hi[2];
	const std::uint32_t span = end - begin;
	if (span <= kLeafSize)
	{
		nodes_[node].first = begin;
		nodes_[node].count = span;
		return;
	}
	int axis = 0;
	double widest = -1.0;
	for (int k = 0; k < 3; ++k)
	{
		const double width = centroidHi[k] - centroidLo[k];
		if (width > widest)
		{
			widest = width;
			axis = k;
		}
	}
	const std::uint32_t mid = begin + span / 2;
	std::nth_element(order_.begin() + begin, order_.begin() + mid, order_.begin() + end, [&](std::uint32_t a, std::uint32_t b)
	{
		const double ca = (static_cast<double>(tris_[a * 9 + axis]) + tris_[a * 9 + 3 + axis] + tris_[a * 9 + 6 + axis]) / 3.0;
		const double cb = (static_cast<double>(tris_[b * 9 + axis]) + tris_[b * 9 + 3 + axis] + tris_[b * 9 + 6 + axis]) / 3.0;
		return ca != cb ? ca < cb : a < b; // ties by index: deterministic
	});
	// The two children are allocated together, so a node's `first` and `first + 1` are its children whatever their subtrees hold.
	const std::uint32_t left = static_cast<std::uint32_t>(nodes_.size());
	nodes_.resize(left + 2);
	nodes_[node].first = left;
	nodes_[node].count = 0;
	buildNode(left, begin, mid);
	buildNode(left + 1, mid, end);
}

HullTree::Work& HullTree::work() noexcept
{
	static thread_local Work counted;
	return counted;
}

void HullTree::traceNode(std::uint32_t node, const std::array<double, 3>& o,
	const std::array<double, 3>& d, double& best) const
{
	const Node& n = nodes_[node];
	Work& counted = work();
	++counted.nodes;
	const double entry = segmentBox(n.min, n.max, o, d);
	if (entry > best || std::isnan(entry))
		return;
	if (n.count == 0)
	{
		traceNode(n.first, o, d, best);
		traceNode(n.first + 1, o, d, best);
		return;
	}
	counted.triangles += n.count;
	for (std::uint32_t i = n.first; i < n.first + n.count; ++i)
	{
		const float* t = &tris_[static_cast<std::size_t>(order_[i]) * 9];
		double hit = 0.0;
		if (segmentTriangle(t, t + 3, t + 6, o, d, hit) && hit < best)
			best = hit;
	}
}

bool HullTree::traceBrute(const std::array<double, 3>& a, const std::array<double, 3>& b, double& t) const
{
	if (tris_.empty())
		return false;
	const std::size_t triangles = tris_.size() / 9;
	const std::array<double, 3> d{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	double best = 1.0;
	for (std::size_t i = 0; i < triangles; ++i)
	{
		const float* tri = &tris_[i * 9];
		double hit = 0.0;
		if (segmentTriangle(tri, tri + 3, tri + 6, a, d, hit) && hit < best)
			best = hit;
	}
	if (best >= 1.0)
		return false;
	t = best;
	return true;
}

bool HullTree::trace(const std::array<double, 3>& a, const std::array<double, 3>& b, double& t) const
{
	if (tris_.empty())
		return false;
	const std::array<double, 3> d{ b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	double best = 1.0;
	traceNode(0, a, d, best);
	if (best >= 1.0)
		return false; // a hit at b's own end does not block (the ray stops at the target)
	t = best;
	return true;
}

} // namespace kworld
