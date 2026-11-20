#include "render/lightbake.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <unordered_map>

#include "app/userdirs.hpp"

namespace lightbake
{
namespace
{

const float kPi = 3.14159265f;
const float kY0 = 0.282095f, kY1 = 0.488603f;	// SH basis constants, bands 0 and 1
const float kEps = 0.05f;			// ray origin offset off a surface (mesh units)
const float kNormalBias = 1.0f;		// bounce lookups step this many spacings off the surface (keeps them out of the wall)
const float kIndoor = 0.5f;			// enclosure above which a probe counts as indoors
const int kRaysOpen = 128;			// rays per probe in the open
const int kRaysIndoor = 1536;		// rays per probe indoors: the light comes through small openings
const int kRaysLate = 192;			// rays per probe for the second and later bounces (smooth light)
const int kBounces = 3;
const float kSunIndoor = 0.5f;		// enclosure above which a sunlit surface is handed to the virtual point lights (the lab's 0.7, on our 0..1)
const float kVplCell = 5.0f;		// sunlit indoor points are merged into one virtual point light per cell of this size (lab)
const int kSunGrid = 384;			// sun rays per side for finding the indoor sun patches (lab)
const uint32_t kMagic = 0x4b4c5042u, kVersion = 1;	// "KLPB"
const float kInf = std::numeric_limits<float>::infinity();

struct V3
{
	float x = 0, y = 0, z = 0;
	V3() = default;
	V3(float a, float b, float c) : x(a), y(b), z(c) {}
	explicit V3(const float* p) : x(p[0]), y(p[1]), z(p[2]) {}
};
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline V3 operator*(V3 a, V3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

std::atomic<bool> gCancel{ false };

template <class F>
void parallel_for(int threads, size_t n, F f)
{
	std::atomic<size_t> next{ 0 };
	auto work = [&]()
	{
		for (;;)
		{
			const size_t begin = next.fetch_add(32);
			if (begin >= n || gCancel.load(std::memory_order_relaxed)) { return; }	// cancel(): the remaining chunks are skipped, the join takes one chunk
			for (size_t i = begin; i < std::min(n, begin + 32); i++) { f(i); }
		}
	};
	std::vector<std::thread> pool;
	for (int t = 1; t < threads; t++) { pool.emplace_back(work); }
	work();
	for (std::thread& t : pool) { t.join(); }
}

uint32_t hash32(uint32_t x)
{
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
float rand01(uint32_t& state)
{
	state = hash32(state + 0x9e3779b9u);
	return (float)(state >> 8) * (1.0f / 16777216.0f);
}

// Evenly spread unit directions (Fibonacci sphere).
std::vector<V3> sphere_dirs(int n)
{
	std::vector<V3> d((size_t)n);
	for (int i = 0; i < n; i++)
	{
		const float z = 1.0f - (2.0f * (float)i + 1.0f) / (float)n;
		const float r = std::sqrt(std::max(0.0f, 1.0f - z * z)), phi = (float)i * 2.39996323f;
		d[(size_t)i] = { r * std::cos(phi), z, r * std::sin(phi) };
	}
	return d;
}

// A random rotation per probe, so neighbouring probes do not all miss the same window.
struct Rot
{
	V3 r[3];
	explicit Rot(uint32_t seed)
	{
		const float u1 = rand01(seed), u2 = rand01(seed) * 2 * kPi, u3 = rand01(seed) * 2 * kPi;
		const float a = std::sqrt(1 - u1), b = std::sqrt(u1);
		const float qx = a * std::sin(u2), qy = a * std::cos(u2), qz = b * std::sin(u3), qw = b * std::cos(u3);
		r[0] = { 1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw) };
		r[1] = { 2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw) };
		r[2] = { 2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy) };
	}
	V3 apply(V3 v) const { return { dot(r[0], v), dot(r[1], v), dot(r[2], v) }; }
};

struct Node
{
	float mn[3], mx[3];
	uint32_t first;	// leaf: first triangle; inner: left child (right = left + 1)
	uint32_t count;	// 0 = inner node
};
struct Hit
{
	float t;
	uint32_t tri;
};

// Moller-Trumbore; t in (1e-4, tMax).
inline bool hit_tri(const Tri& tri, V3 o, V3 d, float tMax, float& t)
{
	const V3 a(tri.p[0]), e1 = V3(tri.p[1]) - a, e2 = V3(tri.p[2]) - a;
	const V3 pv = cross(d, e2);
	const float det = dot(e1, pv);
	if (std::fabs(det) < 1e-12f) { return false; }
	const float inv = 1.0f / det;
	const V3 tv = o - a;
	const float u = dot(tv, pv) * inv;
	if (u < 0 || u > 1) { return false; }
	const V3 qv = cross(tv, e1);
	const float v = dot(d, qv) * inv;
	if (v < 0 || u + v > 1) { return false; }
	t = dot(e2, qv) * inv;
	return t > 1e-4f && t < tMax;
}

inline bool hit_box(const float mn[3], const float mx[3], const float o[3], const float inv[3], float tMax)
{
	float t0 = 0, t1 = tMax;
	for (int k = 0; k < 3; k++)
	{
		float a = (mn[k] - o[k]) * inv[k], b = (mx[k] - o[k]) * inv[k];
		if (a > b) { std::swap(a, b); }
		t0 = std::max(t0, a);
		t1 = std::min(t1, b);
	}
	return t0 <= t1;
}

inline V3 normalize(V3 a) { return a * (1.0f / std::sqrt(std::max(dot(a, a), 1e-30f))); }

class Baker
{
public:
	Baker(std::vector<Tri> tris, Grid& grid, float groundY, float groundAlbedo, int threads) : tris_(std::move(tris)), grid_(grid), groundY_(groundY), groundAlbedo_(groundAlbedo), threads_(threads) {}
	void run();			// the sky transfer
	void runSun(const float groundAlbedo[3], const float sunDir[3], int bounces);	// the sun transfer
	void buildTree() { buildBvh(); }	// the BVH alone
	bool blocked(const float from[3], const float to[3], float margin) const;	// anything between the two points (the last `margin` units before `to` do not count)

private:
	void buildBvh();
	void enclosure();		// fills aux: enclosure and usable (geometry only)
	void build(uint32_t node, uint32_t first, uint32_t count, int depth);
	bool trace(const float o[3], const float d[3], float tMax, Hit& hit) const;
	bool occluded(const float o[3], const float d[3], float tMax) const;
	bool indoors(const float p[3], const float n[3]) const;
	// the eight probes around p and their trilinear weights (unusable probes weigh 0, the rest renormalised)
	void cell(const float p[3], size_t idx[8], float w[8]) const;
	float irradiance(const std::vector<float>& field, const float p[3], const float n[3]) const;
	void irradianceRGB(const std::vector<float>& field, const float p[3], const float n[3], float out[3]) const;
	// the usable indoor probes softened with their neighbours (1-2-1 per axis); probes/ch = 4 (grey) or 12 (RGB)
	void soften(std::vector<float>& field, int probes) const;

	std::vector<Tri> tris_;
	std::vector<Node> nodes_;
	Grid& grid_;
	float groundY_, groundAlbedo_;
	int threads_;
};

void Baker::build(uint32_t node, uint32_t first, uint32_t count, int depth)
{
	float mn[3] = { kInf, kInf, kInf }, mx[3] = { -kInf, -kInf, -kInf }, cmn[3] = { kInf, kInf, kInf }, cmx[3] = { -kInf, -kInf, -kInf };
	for (uint32_t i = first; i < first + count; i++)
	{
		const Tri& t = tris_[i];
		for (int k = 0; k < 3; k++)
		{
			for (int v = 0; v < 3; v++) { mn[k] = std::min(mn[k], t.p[v][k]); mx[k] = std::max(mx[k], t.p[v][k]); }
			const float c = (t.p[0][k] + t.p[1][k] + t.p[2][k]) / 3;
			cmn[k] = std::min(cmn[k], c);
			cmx[k] = std::max(cmx[k], c);
		}
	}
	memcpy(nodes_[node].mn, mn, sizeof mn);
	memcpy(nodes_[node].mx, mx, sizeof mx);
	if (count <= 4 || depth > 48)
	{
		nodes_[node].first = first;
		nodes_[node].count = count;
		return;
	}
	int axis = 0;
	for (int k = 1; k < 3; k++) { if (cmx[k] - cmn[k] > cmx[axis] - cmn[axis]) { axis = k; } }
	const uint32_t half = count / 2;
	std::nth_element(tris_.begin() + first, tris_.begin() + first + half, tris_.begin() + first + count, [axis](const Tri& a, const Tri& b)
	{
		return a.p[0][axis] + a.p[1][axis] + a.p[2][axis] < b.p[0][axis] + b.p[1][axis] + b.p[2][axis];
	});
	const uint32_t left = (uint32_t)nodes_.size();
	nodes_.resize(nodes_.size() + 2);
	nodes_[node].first = left;
	nodes_[node].count = 0;
	build(left, first, half, depth + 1);
	build(left + 1, first + half, count - half, depth + 1);
}

bool Baker::trace(const float o[3], const float d[3], float tMax, Hit& hit) const
{
	float inv[3];
	for (int k = 0; k < 3; k++) { inv[k] = 1.0f / (std::fabs(d[k]) < 1e-20f ? 1e-20f : d[k]); }
	uint32_t stack[64];
	int sp = 0;
	stack[sp++] = 0;
	hit.t = tMax;
	bool found = false;
	while (sp)
	{
		const Node& n = nodes_[stack[--sp]];
		if (!hit_box(n.mn, n.mx, o, inv, hit.t)) { continue; }
		if (!n.count)
		{
			stack[sp++] = n.first;
			stack[sp++] = n.first + 1;
			continue;
		}
		for (uint32_t i = n.first; i < n.first + n.count; i++)
		{
			float t;
			if (hit_tri(tris_[i], V3(o), V3(d), hit.t, t)) { hit.t = t; hit.tri = i; found = true; }
		}
	}
	return found;
}

void Baker::cell(const float p[3], size_t idx[8], float w[8]) const
{
	int i0[3];
	float f[3];
	for (int k = 0; k < 3; k++)
	{
		const float g = std::clamp((p[k] - grid_.min[k]) / grid_.spacing, 0.0f, (float)(grid_.dim[k] - 1) - 1e-3f);
		i0[k] = (int)g;
		f[k] = g - (float)i0[k];
	}
	for (int c = 0; c < 8; c++)
	{
		const int dx = c & 1, dy = (c >> 1) & 1, dz = c >> 2;
		idx[c] = ((size_t)(i0[2] + dz) * grid_.dim[1] + (size_t)(i0[1] + dy)) * grid_.dim[0] + (size_t)(i0[0] + dx);
		w[c] = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]) * grid_.aux[idx[c] * 2 + 1];
	}
	const float sum = w[0] + w[1] + w[2] + w[3] + w[4] + w[5] + w[6] + w[7];
	if (sum > 1e-4f) { for (int c = 0; c < 8; c++) { w[c] /= sum; } }
}

// Cosine-convolved L1 irradiance of `field` (4 floats per probe) at p for the normal n, never below a tenth of the flat term.
float Baker::irradiance(const std::vector<float>& field, const float p[3], const float n[3]) const
{
	size_t idx[8];
	float w[8], c[4] = {};
	cell(p, idx, w);
	for (int k = 0; k < 8; k++) { for (int j = 0; j < 4; j++) { c[j] += field[idx[k] * 4 + j] * w[k]; } }
	const float flat = kPi * kY0 * c[0];
	return std::max(flat + (2.0f * kPi / 3.0f) * kY1 * (c[1] * n[1] + c[2] * n[2] + c[3] * n[0]), 0.1f * std::max(flat, 0.0f));
}

void Baker::buildBvh()
{
	nodes_.clear();
	nodes_.reserve(tris_.size());
	nodes_.resize(1);
	if (tris_.empty()) { nodes_[0] = {}; }
	else { build(0, 0, (uint32_t)tris_.size(), 0); }
}

// Enclosure and usability depend on the geometry only.
void Baker::enclosure()
{
	const size_t n = grid_.count();
	const float sp = grid_.spacing;
	const std::vector<V3> dirs = sphere_dirs(128);
	parallel_for(threads_, n, [&](size_t i)
	{
		const V3 o = V3(grid_.min[0] + sp * (float)(i % grid_.dim[0]), grid_.min[1] + sp * (float)(i / grid_.dim[0] % grid_.dim[1]),
			grid_.min[2] + sp * (float)(i / ((size_t)grid_.dim[0] * grid_.dim[1])));
		if (o.y < groundY_ || tris_.empty()) { return; }
		const Rot rot((uint32_t)i * 2654435761u + 17u);
		int upward = 0, blocked = 0, back = 0;
		for (const V3& dir : dirs)
		{
			const V3 d = rot.apply(dir);
			const float tGround = d.y < -1e-6f ? (groundY_ - o.y) / d.y : kInf;
			Hit h;
			const bool hit = trace(&o.x, &d.x, tGround, h);
			if (d.y > 0) { upward++; blocked += hit; }
			if (hit && dot(d, V3(tris_[h.tri].n)) > 0) { back++; }
		}
		grid_.aux[i * 2] = (float)blocked / (float)std::max(upward, 1);
		grid_.aux[i * 2 + 1] = back * 4 < 128 ? 1.0f : 0.0f;	// a probe that mostly sees the backs of faces is inside a wall
	});
}

bool Baker::occluded(const float o[3], const float d[3], float tMax) const
{
	float inv[3];
	for (int k = 0; k < 3; k++) { inv[k] = 1.0f / (std::fabs(d[k]) < 1e-20f ? 1e-20f : d[k]); }
	uint32_t stack[64];
	int sp = 0;
	stack[sp++] = 0;
	while (sp)
	{
		const Node& n = nodes_[stack[--sp]];
		if (!hit_box(n.mn, n.mx, o, inv, tMax)) { continue; }
		if (!n.count)
		{
			stack[sp++] = n.first;
			stack[sp++] = n.first + 1;
			continue;
		}
		for (uint32_t i = n.first; i < n.first + n.count; i++)
		{
			float t;
			if (hit_tri(tris_[i], V3(o), V3(d), tMax, t)) { return true; }
		}
	}
	return false;
}

// A surface point p with normal n counts as indoors when the probes just off it are enclosed (the lab's Baker::indoors).
bool Baker::indoors(const float p[3], const float n[3]) const
{
	const float q[3] = { p[0] + n[0] * grid_.spacing * kNormalBias, p[1] + n[1] * grid_.spacing * kNormalBias, p[2] + n[2] * grid_.spacing * kNormalBias };
	size_t idx[8];
	float w[8];
	cell(q, idx, w);
	float e = 0;
	for (int k = 0; k < 8; k++) { e += grid_.aux[idx[k] * 2] * w[k]; }
	return e > kSunIndoor;
}

bool Baker::blocked(const float from[3], const float to[3], float margin) const
{
	float d[3] = { to[0] - from[0], to[1] - from[1], to[2] - from[2] };
	const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	if (len <= margin + 1e-4f) { return false; }
	for (float& c : d) { c /= len; }
	Hit hit;
	return trace(from, d, len - margin, hit);
}

void Baker::run()
{
	const auto start = std::chrono::steady_clock::now();
	buildBvh();

	const size_t n = grid_.count();
	const float sp = grid_.spacing;
	grid_.aux.assign(n * 2, 0.0f);
	grid_.sh.assign(n * 4, 0.0f);
	auto probe_pos = [&](size_t i)
	{
		return V3(grid_.min[0] + sp * (float)(i % grid_.dim[0]), grid_.min[1] + sp * (float)(i / grid_.dim[0] % grid_.dim[1]),
			grid_.min[2] + sp * (float)(i / ((size_t)grid_.dim[0] * grid_.dim[1])));
	};

	enclosure();

	// The sky transfer: bounce 0 is the white sky seen straight from the probe; every later bounce is what the surfaces hit pass on from the
	// previous one, lit by the previous step's probes (albedo * E / pi).
	const std::vector<V3> dirsOpen = sphere_dirs(kRaysOpen), dirsIndoor = sphere_dirs(kRaysIndoor), dirsLate = sphere_dirs(kRaysLate);
	const float inGrid[3] = { grid_.min[0] + sp * (float)(grid_.dim[0] - 1), grid_.min[1] + sp * (float)(grid_.dim[1] - 1), grid_.min[2] + sp * (float)(grid_.dim[2] - 1) };
	std::vector<float> prev, next;
	for (int bounce = 0; bounce < kBounces; bounce++)
	{
		next.assign(n * 4, 0.0f);
		parallel_for(threads_, n, [&](size_t i)
		{
			if (grid_.aux[i * 2 + 1] < 0.5f) { return; }
			const V3 o = probe_pos(i);
			const bool indoorProbe = grid_.aux[i * 2] > kIndoor;
			const std::vector<V3>& dirs = bounce >= 2 ? dirsLate : indoorProbe ? dirsIndoor : dirsOpen;
			// Indoor probes share one set of directions: their error then changes smoothly from probe to probe instead of as blotches.
			const Rot rot(indoorProbe ? 7u + (uint32_t)bounce : (uint32_t)i * 2654435761u + (uint32_t)bounce * 977u);
			float acc[4] = {};
			for (const V3& dir : dirs)
			{
				const V3 d = rot.apply(dir);
				const float tGround = d.y < -1e-6f ? (groundY_ - o.y) / d.y : kInf;
				Hit h;
				V3 p, nrm;
				float albedo = 0;
				bool surface = true;
				if (!tris_.empty() && trace(&o.x, &d.x, tGround, h))
				{
					const Tri& tri = tris_[h.tri];
					nrm = V3(tri.n);
					if (dot(d, nrm) > 0) { continue; }	// the back of a face: no light
					p = o + d * h.t;
					albedo = tri.albedo;
				}
				else if (tGround < kInf)
				{
					p = o + d * tGround;
					nrm = V3(0, 1, 0);
					albedo = groundAlbedo_;
				}
				else { surface = false; }
				float l = 0;
				if (!surface) { l = bounce == 0 ? 1.0f : 0.0f; }
				else if (bounce > 0)
				{
					const V3 q = p + nrm * (sp * kNormalBias);
					const bool inside = q.x >= grid_.min[0] && q.y >= grid_.min[1] && q.z >= grid_.min[2] && q.x <= inGrid[0] && q.y <= inGrid[1] && q.z <= inGrid[2];
					const float e = inside ? irradiance(prev, &q.x, &nrm.x) : bounce == 1 ? kPi : 0.0f;	// outside the grid: the open sky's irradiance on level ground
					l = albedo * e / kPi;
				}
				const float y[4] = { kY0, kY1 * d.y, kY1 * d.z, kY1 * d.x };
				for (int k = 0; k < 4; k++) { acc[k] += l * y[k]; }
			}
			const float norm = 4 * kPi / (float)dirs.size();
			for (int k = 0; k < 4; k++) { next[i * 4 + k] = acc[k] * norm; }
		});
		for (size_t k = 0; k < n * 4; k++) { grid_.sh[k] += next[k]; }
		prev.swap(next);
	}

	// Indoors every probe sees the openings through a handful of its rays: soften with the usable indoor neighbours (1-2-1 per axis).
	soften(grid_.sh, 4);
	grid_.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// Indoors every probe sees the openings through a handful of its rays: soften with the usable indoor neighbours (1-2-1 per axis).
void Baker::soften(std::vector<float>& field, int probes) const
{
	const std::vector<float> src = field;
	const size_t n = grid_.count();
	parallel_for(threads_, n, [&](size_t i)
	{
		if (grid_.aux[i * 2 + 1] < 0.5f || grid_.aux[i * 2] < 0.5f) { return; }
		const int x = (int)(i % grid_.dim[0]), y = (int)(i / grid_.dim[0] % grid_.dim[1]), z = (int)(i / ((size_t)grid_.dim[0] * grid_.dim[1]));
		float sum[12] = {}, weight = 0;
		for (int c = 0; c < 27; c++)
		{
			const int dx = c % 3 - 1, dy = c / 3 % 3 - 1, dz = c / 9 - 1;
			const int nx = x + dx, ny = y + dy, nz = z + dz;
			if (nx < 0 || ny < 0 || nz < 0 || nx >= grid_.dim[0] || ny >= grid_.dim[1] || nz >= grid_.dim[2]) { continue; }
			const size_t j = ((size_t)nz * grid_.dim[1] + ny) * grid_.dim[0] + nx;
			if (grid_.aux[j * 2 + 1] < 0.5f || grid_.aux[j * 2] < 0.3f) { continue; }
			const float w = (float)((2 - std::abs(dx)) * (2 - std::abs(dy)) * (2 - std::abs(dz)));
			weight += w;
			for (int k = 0; k < probes; k++) { sum[k] += src[j * (size_t)probes + k] * w; }
		}
		for (int k = 0; k < probes; k++) { field[i * (size_t)probes + k] = sum[k] / weight; }
	});
}

// Cosine-convolved L1 irradiance of an RGB field (12 floats per probe), never below a tenth of the flat term (the lab's LightVolume::irradiance).
void Baker::irradianceRGB(const std::vector<float>& field, const float p[3], const float n[3], float out[3]) const
{
	size_t idx[8];
	float w[8];
	cell(p, idx, w);
	for (int c = 0; c < 3; c++)
	{
		float cc[4] = {};
		for (int k = 0; k < 8; k++) { for (int j = 0; j < 4; j++) { cc[j] += field[idx[k] * 12 + c * 4 + j] * w[k]; } }
		const float flat = kPi * kY0 * cc[0];
		out[c] = std::max(flat + (2.0f * kPi / 3.0f) * kY1 * (cc[1] * n[1] + cc[2] * n[2] + cc[3] * n[0]), 0.1f * std::max(flat, 0.0f));
	}
}

// The sun transfer alone: a white sun of strength 1 from sunDir (building-local). See lightbake.hpp; the lab's bake.cpp, sun parts only (its sky is the
// separate, sun-independent bake above; its lamp is not ported).
void Baker::runSun(const float groundAlb[3], const float sunDirIn[3], int bounces)
{
	const auto start = std::chrono::steady_clock::now();
	const size_t n = grid_.count();
	const float sp = grid_.spacing;
	V3 sun = normalize(V3(sunDirIn));
	grid_.sun.assign(n * 12, 0.0f);
	grid_.sunDir[0] = sun.x; grid_.sunDir[1] = sun.y; grid_.sunDir[2] = sun.z;
	for (int k = 0; k < 3; k++) { grid_.sunGround[k] = groundAlb[k]; }
	grid_.sunVpls = 0;
	buildBvh();
	grid_.aux.assign(n * 2, 0.0f);
	enclosure();
	if (sun.y <= 0.01f || tris_.empty()) { grid_.sunSeconds = 0; return; }	// below the horizon: the term is zero, no bake
	const V3 groundAlbV(groundAlb);
	const V3 bmin(nodes_[0].mn), bmax(nodes_[0].mx);
	const V3 c = (bmin + bmax) * 0.5f;
	const float radius = std::sqrt(dot(bmax - bmin, bmax - bmin)) * 0.5f;
	auto probe_pos = [&](size_t i)
	{
		return V3(grid_.min[0] + sp * (float)(i % grid_.dim[0]), grid_.min[1] + sp * (float)(i / grid_.dim[0] % grid_.dim[1]),
			grid_.min[2] + sp * (float)(i / ((size_t)grid_.dim[0] * grid_.dim[1])));
	};
	auto sun_visible = [&](V3 p, V3 nrm)
	{
		const V3 o = p + nrm * kEps;
		return !occluded(&o.x, &sun.x, kInf);
	};

	// Indoor sun patches -> virtual point lights. Uniform probe rays almost never find a small patch of sunlit floor, so those surfaces are found from
	// the sun's side instead and each probe connects to them directly.
	struct Vpl
	{
		V3 p, n, flux;
	};
	std::vector<Vpl> vpls;
	{
		const V3 lx = normalize(cross(std::fabs(sun.y) > 0.99f ? V3(1, 0, 0) : V3(0, 1, 0), sun)), ly = cross(sun, lx);
		const float step = 2 * radius / (float)kSunGrid;
		std::vector<Hit> hits((size_t)kSunGrid * kSunGrid);
		parallel_for(threads_, hits.size(), [&](size_t i)
		{
			const float u = ((float)(i % kSunGrid) + 0.5f) * step - radius, v = ((float)(i / kSunGrid) + 0.5f) * step - radius;
			const V3 o = c + sun * (2 * radius) + lx * u + ly * v, d = sun * -1.0f;
			if (!trace(&o.x, &d.x, kInf, hits[i])) { hits[i].t = -1; }
		});
		std::unordered_map<uint64_t, Vpl> cells;
		for (size_t i = 0; i < hits.size(); i++)
		{
			if (hits[i].t < 0) { continue; }
			const Tri& tri = tris_[hits[i].tri];
			const V3 nrm(tri.n);
			if (dot(nrm, sun) <= 0) { continue; }
			const float u = ((float)(i % kSunGrid) + 0.5f) * step - radius, v = ((float)(i / kSunGrid) + 0.5f) * step - radius;
			const V3 p = c + sun * (2 * radius - hits[i].t) + lx * u + ly * v;
			if (p.y < groundY_ || !indoors(&p.x, tri.n)) { continue; }
			const int axis = std::fabs(nrm.x) > std::fabs(nrm.y) ? (std::fabs(nrm.x) > std::fabs(nrm.z) ? 0 : 2) : (std::fabs(nrm.y) > std::fabs(nrm.z) ? 1 : 2);
			const uint64_t face = (uint64_t)axis * 2 + ((&nrm.x)[axis] > 0 ? 1 : 0);
			const uint64_t key = ((uint64_t)(int64_t)std::floor(p.x / kVplCell + 4096) << 40) ^ ((uint64_t)(int64_t)std::floor(p.y / kVplCell + 4096) << 24)
				^ ((uint64_t)(int64_t)std::floor(p.z / kVplCell + 4096) << 8) ^ face;
			const V3 flux = V3(tri.alb) * (step * step);	// step^2 is the ray's area perpendicular to the sun (strength 1)
			const float w = flux.x + flux.y + flux.z + 1e-12f;
			Vpl& cellv = cells[key];
			cellv.p = cellv.p + p * w;	// flux-weighted sums, normalised below
			cellv.n = cellv.n + nrm * w;
			cellv.flux = cellv.flux + flux;
		}
		for (auto& [key, cv] : cells)
		{
			const float w = cv.flux.x + cv.flux.y + cv.flux.z;
			if (w <= 0) { continue; }
			cv.n = normalize(cv.n);
			cv.p = cv.p * (1.0f / w) + cv.n * kEps * 4;
			vpls.push_back(cv);
		}
	}
	grid_.sunVpls = vpls.size();

	std::vector<float> total(n * 12, 0.0f), prev, next;
	const std::vector<V3> dirsOpen = sphere_dirs(kRaysOpen), dirsIndoor = sphere_dirs(kRaysIndoor), dirsLate = sphere_dirs(kRaysLate);
	const float inGrid[3] = { grid_.min[0] + sp * (float)(grid_.dim[0] - 1), grid_.min[1] + sp * (float)(grid_.dim[1] - 1), grid_.min[2] + sp * (float)(grid_.dim[2] - 1) };
	for (int bounce = 0; bounce < std::max(1, bounces); bounce++)
	{
		next.assign(n * 12, 0.0f);
		parallel_for(threads_, n, [&](size_t i)
		{
			if (grid_.aux[i * 2 + 1] < 0.5f) { return; }
			const V3 o = probe_pos(i);
			const bool indoorProbe = grid_.aux[i * 2] > kIndoor;
			const std::vector<V3>& dirs = bounce >= 2 ? dirsLate : indoorProbe ? dirsIndoor : dirsOpen;
			const Rot rot(indoorProbe ? 7u + (uint32_t)bounce : (uint32_t)i * 2654435761u + (uint32_t)bounce * 977u);
			float acc[3][4] = {};
			for (const V3& dir : dirs)
			{
				const V3 d = rot.apply(dir);
				const float tGround = d.y < -1e-6f ? (groundY_ - o.y) / d.y : kInf;
				Hit h;
				V3 p, nrm, alb;
				bool surface = true, building = false;
				if (!tris_.empty() && trace(&o.x, &d.x, tGround, h))
				{
					const Tri& tri = tris_[h.tri];
					nrm = V3(tri.n);
					if (dot(d, nrm) > 0) { continue; }	// the back of a face: no light
					p = o + d * h.t;
					alb = V3(tri.alb);
					building = true;
				}
				else if (tGround < kInf)
				{
					p = o + d * tGround;
					nrm = V3(0, 1, 0);
					alb = groundAlbV;
				}
				else { surface = false; }
				V3 l;
				if (!surface) { l = V3(); }	// the sun alone: the sky is the other bake
				else if (bounce == 0)
				{
					// first bounce of the sun; surfaces indoors are covered by the virtual point lights instead
					const float ndl = dot(nrm, sun);
					if (ndl > 0 && !(building && indoors(&p.x, &nrm.x)) && (dot(p - c, p - c) > 9 * radius * radius || sun_visible(p, nrm))) { l = alb * (ndl / kPi); }
				}
				else
				{
					const V3 q = p + nrm * (sp * kNormalBias);
					const bool inside = q.x >= grid_.min[0] && q.y >= grid_.min[1] && q.z >= grid_.min[2] && q.x <= inGrid[0] && q.y <= inGrid[1] && q.z <= inGrid[2];
					float e[3] = {};
					if (inside) { irradianceRGB(prev, &q.x, &nrm.x, e); }
					l = alb * (V3(e) * (1.0f / kPi));
				}
				const float y[4] = { kY0, kY1 * d.y, kY1 * d.z, kY1 * d.x };
				for (int k = 0; k < 4; k++) { acc[0][k] += l.x * y[k]; acc[1][k] += l.y * y[k]; acc[2][k] += l.z * y[k]; }
			}
			const float norm = 4 * kPi / (float)dirs.size();
			for (int c = 0; c < 3; c++) { for (int k = 0; k < 4; k++) { acc[c][k] *= norm; } }
			if (bounce == 0 && grid_.aux[i * 2] > 0.15f)
			{
				for (const Vpl& v : vpls)
				{
					const V3 to = v.p - o;
					const float r2 = dot(to, to), r = std::sqrt(r2);
					const V3 w = to * (1.0f / std::max(r, 1e-6f));
					const float cosv = -dot(v.n, w);
					if (cosv <= 0 || occluded(&o.x, &w.x, r - kEps)) { continue; }
					// a small Lambertian patch: intensity = flux cos / pi; the distance is clamped to the patch size
					const V3 l = v.flux * (cosv / (kPi * std::max(r2, kVplCell * kVplCell)));
					const float y[4] = { kY0, kY1 * w.y, kY1 * w.z, kY1 * w.x };
					acc[0][0] += l.x * y[0]; acc[0][1] += l.x * y[1]; acc[0][2] += l.x * y[2]; acc[0][3] += l.x * y[3];
					acc[1][0] += l.y * y[0]; acc[1][1] += l.y * y[1]; acc[1][2] += l.y * y[2]; acc[1][3] += l.y * y[3];
					acc[2][0] += l.z * y[0]; acc[2][1] += l.z * y[1]; acc[2][2] += l.z * y[2]; acc[2][3] += l.z * y[3];
				}
			}
			for (int c = 0; c < 3; c++) { for (int k = 0; k < 4; k++) { next[i * 12 + c * 4 + k] = acc[c][k]; } }
		});
		for (size_t k = 0; k < n * 12; k++) { total[k] += next[k]; }
		prev.swap(next);
	}
	soften(total, 12);
	grid_.sun = std::move(total);
	grid_.sunSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The per-user cache root (app/userdirs.hpp; kenshi-379.31.3: on Windows %LOCALAPPDATA%\OpenKenshi\cache, never the Linux HOME).
std::string cacheDir()
{
	return plat::userCacheDir() + "/kclient";
}

}

size_t Grid::usableCount() const
{
	size_t n = 0;
	for (size_t i = 0; i < count() && aux.size() >= (i + 1) * 2; i++) { n += usable(i); }
	return n;
}

void Grid::position(size_t i, float out[3]) const
{
	out[0] = min[0] + spacing * (float)(i % dim[0]);
	out[1] = min[1] + spacing * (float)(i / dim[0] % dim[1]);
	out[2] = min[2] + spacing * (float)(i / ((size_t)dim[0] * dim[1]));
}

void cancel() { gCancel.store(true); }
bool cancelled() { return gCancel.load(); }

Grid bake(std::vector<Tri> tris, const float mn[3], const float mx[3], float spacing, float groundY, float groundAlbedo, int threads)
{
	Grid g;
	g.spacing = spacing;
	g.triangles = tris.size();
	for (int k = 0; k < 3; k++)
	{
		g.min[k] = mn[k];
		g.dim[k] = std::max(2, (int)std::ceil((mx[k] - mn[k]) / spacing) + 1);
	}
	Baker b(std::move(tris), g, groundY, groundAlbedo, std::clamp(threads, 1, 4));
	b.run();
	return g;
}

struct Occluder::Impl
{
	Grid none;
	Baker baker;
	explicit Impl(std::vector<Tri> tris) : baker(std::move(tris), none, 0.0f, 0.0f, 1) { baker.buildTree(); }
};

Occluder::Occluder(std::vector<Tri> tris) : impl_(new Impl(std::move(tris))) {}
Occluder::~Occluder() { delete impl_; }

// One ray per probe per lamp, from the probe to the lamp. The lamp hangs on a ceiling or wall: the last kLampMargin units before it do not count, so the slab it is fixed to does not hide it.
std::vector<uint8_t> Occluder::lampVisibility(const Grid& g, const std::vector<std::array<float, 3>>& lamps) const
{
	constexpr float kLampMargin = 0.6f;
	std::vector<uint8_t> out(g.count() * kMaxLamps, 0);
	for (size_t i = 0; i < g.count(); i++)
	{
		if (!g.usable(i)) { continue; }
		float p[3];
		g.position(i, p);
		for (size_t k = 0; k < lamps.size() && k < (size_t)kMaxLamps; k++) { out[i * kMaxLamps + k] = impl_->baker.blocked(p, lamps[k].data(), kLampMargin) ? 0 : 255; }
	}
	return out;
}

Grid bakeSun(std::vector<Tri> tris, const float mn[3], const float mx[3], float spacing, float groundY, const float groundAlbedo[3], const float sunDir[3], int threads)
{
	Grid g;
	g.spacing = spacing;
	g.triangles = tris.size();
	for (int k = 0; k < 3; k++)
	{
		g.min[k] = mn[k];
		g.dim[k] = std::max(2, (int)std::ceil((mx[k] - mn[k]) / spacing) + 1);
	}
	Baker b(std::move(tris), g, groundY, 0.0f, std::clamp(threads, 1, 4));
	b.runSun(groundAlbedo, sunDir, kBounces);
	return g;
}

uint64_t hashBytes(uint64_t h, const void* data, size_t n)
{
	const uint8_t* p = (const uint8_t*)data;
	for (size_t i = 0; i < n; i++) { h = (h ^ p[i]) * 1099511628211ull; }
	return h;
}

std::string cachePath(const std::string& name, uint64_t key)
{
	std::string safe = name;
	for (char& c : safe) { if (!isalnum((unsigned char)c) && c != '-' && c != '_') { c = '_'; } }
	char hex[24];
	snprintf(hex, sizeof hex, "%016llx", (unsigned long long)key);
	return cacheDir() + "/lightbake." + safe + "." + hex + ".bin";
}

bool save(const std::string& path, const Grid& g)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
	const std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) { return false; }
	const uint32_t head[2] = { kMagic, kVersion };
	const uint64_t tris = g.triangles;
	bool ok = fwrite(head, sizeof head, 1, f) == 1 && fwrite(g.dim, sizeof g.dim, 1, f) == 1 && fwrite(g.min, sizeof g.min, 1, f) == 1 && fwrite(&g.spacing, sizeof g.spacing, 1, f) == 1
		&& fwrite(&g.seconds, sizeof g.seconds, 1, f) == 1 && fwrite(&tris, sizeof tris, 1, f) == 1
		&& fwrite(g.sh.data(), sizeof(float), g.sh.size(), f) == g.sh.size() && fwrite(g.aux.data(), sizeof(float), g.aux.size(), f) == g.aux.size();
	ok = fclose(f) == 0 && ok;
	if (ok) { std::filesystem::rename(tmp, path, ec); ok = !ec; }
	if (!ok) { std::filesystem::remove(tmp, ec); }
	return ok;
}

bool load(const std::string& path, Grid& g)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) { return false; }
	uint32_t head[2] = {};
	uint64_t tris = 0;
	Grid r;
	bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == kMagic && head[1] == kVersion && fread(r.dim, sizeof r.dim, 1, f) == 1 && fread(r.min, sizeof r.min, 1, f) == 1
		&& fread(&r.spacing, sizeof r.spacing, 1, f) == 1 && fread(&r.seconds, sizeof r.seconds, 1, f) == 1 && fread(&tris, sizeof tris, 1, f) == 1;
	if (ok)
	{
		ok = r.dim[0] > 1 && r.dim[1] > 1 && r.dim[2] > 1 && r.dim[0] < 4096 && r.dim[1] < 4096 && r.dim[2] < 4096 && r.count() < (size_t)1 << 26;
	}
	if (ok)
	{
		r.sh.resize(r.count() * 4);
		r.aux.resize(r.count() * 2);
		ok = fread(r.sh.data(), sizeof(float), r.sh.size(), f) == r.sh.size() && fread(r.aux.data(), sizeof(float), r.aux.size(), f) == r.aux.size();
	}
	fclose(f);
	if (!ok) { return false; }
	r.triangles = (size_t)tris;
	r.fromCache = true;
	g = std::move(r);
	return true;
}

}
