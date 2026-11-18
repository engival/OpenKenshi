#include "render/openwindows.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace openwindows
{
namespace
{
// A position welded at 0.01 mesh units (the meshes repeat a shared corner with the same coordinates; separate faces of a quad are welded here and
// nowhere else).
struct Key
{
	int32_t x, y, z;
	bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct KeyHash
{
	size_t operator()(const Key& k) const
	{
		uint64_t h = (uint64_t)(uint32_t)k.x * 0x9e3779b97f4a7c15ull;
		h ^= (uint64_t)(uint32_t)k.y * 0xc2b2ae3d27d4eb4full;
		h ^= (uint64_t)(uint32_t)k.z * 0x165667b19e3779f9ull;
		return (size_t)(h ^ (h >> 29));
	}
};
}

float Texel::at(float u, float v) const
{
	if (!luma && !rgba) { return 1.0f; }
	u -= std::floor(u);
	v -= std::floor(v);
	const int x = std::clamp((int)(u * (float)w), 0, w - 1), y = std::clamp((int)(v * (float)h), 0, h - 1);
	if (luma) { return (float)luma[(size_t)y * w + x] * (1.0f / 255.0f); }
	const uint8_t* p = &rgba[((size_t)y * w + x) * 4];
	return (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f;
}

std::vector<Island> find(const std::vector<float>& verts, int stride, const std::vector<uint32_t>& indices, uint32_t first, uint32_t count, float maxDiagonal, float minDiagonal)
{
	std::vector<Island> out;
	const uint32_t tris = count / 3;
	if (tris == 0) { return out; }

	// Weld every vertex position to the first vertex that had it; union the triangles sharing a welded vertex.
	std::unordered_map<Key, uint32_t, KeyHash> firstAt;
	firstAt.reserve((size_t)tris * 3);
	std::vector<uint32_t> weld((size_t)count);
	for (uint32_t i = 0; i < count; i++)
	{
		const uint32_t v = indices[first + i];
		const float* p = &verts[(size_t)v * stride];
		const Key k = { (int32_t)std::lround(p[0] * 100.0f), (int32_t)std::lround(p[1] * 100.0f), (int32_t)std::lround(p[2] * 100.0f) };
		auto [it, fresh] = firstAt.emplace(k, v);
		weld[i] = it->second;
	}
	std::unordered_map<uint32_t, uint32_t> triOfWeld;	// welded vertex -> a triangle that has it
	std::vector<uint32_t> parent(tris);
	for (uint32_t t = 0; t < tris; t++) { parent[t] = t; }
	auto root = [&](uint32_t a)
	{
		while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
		return a;
	};
	for (uint32_t t = 0; t < tris; t++)
	{
		for (uint32_t k = 0; k < 3; k++)
		{
			const uint32_t w = weld[(size_t)t * 3 + k];
			auto [it, fresh] = triOfWeld.emplace(w, t);
			if (!fresh)
			{
				const uint32_t a = root(t), b = root(it->second);
				if (a != b) { parent[b] = a; }
			}
		}
	}
	std::unordered_map<uint32_t, uint32_t> groupOf;	// root -> index in `groups`
	std::vector<std::vector<uint32_t>> groups;
	for (uint32_t t = 0; t < tris; t++)
	{
		const uint32_t r = root(t);
		auto [it, fresh] = groupOf.emplace(r, (uint32_t)groups.size());
		if (fresh) { groups.emplace_back(); }
		groups[it->second].push_back(t);
	}
	for (const std::vector<uint32_t>& g : groups)
	{
		if ((int)g.size() > kMaxTris) { continue; }
		Island is;
		is.tris = g;
		is.uv.resize((size_t)g.size() * 6);
		for (int k = 0; k < 3; k++) { is.mn[k] = 1e30f; is.mx[k] = -1e30f; }
		for (size_t ti = 0; ti < g.size(); ti++)
		{
			for (uint32_t k = 0; k < 3; k++)
			{
				const float* p = &verts[(size_t)indices[first + (size_t)g[ti] * 3 + k] * stride];
				for (int a = 0; a < 3; a++) { is.mn[a] = std::min(is.mn[a], p[a]); is.mx[a] = std::max(is.mx[a], p[a]); }
				is.uv[ti * 6 + (size_t)k * 2] = p[6];
				is.uv[ti * 6 + (size_t)k * 2 + 1] = p[7];
			}
		}
		const float dx = is.mx[0] - is.mn[0], dy = is.mx[1] - is.mn[1], dz = is.mx[2] - is.mn[2];
		const float diag2 = dx * dx + dy * dy + dz * dz;
		if (diag2 > maxDiagonal * maxDiagonal || diag2 < minDiagonal * minDiagonal) { continue; }
		out.push_back(std::move(is));
	}
	return out;
}

bool isPanePath(const std::string& path)
{
	std::string base = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
	for (char& c : base) { c = (char)std::tolower((unsigned char)c); }
	return base.find("window") != std::string::npos;
}

Island whole(const std::vector<float>& verts, int stride, const std::vector<uint32_t>& indices, uint32_t first, uint32_t count)
{
	Island is;
	const uint32_t tris = count / 3;
	is.tris.resize(tris);
	is.uv.resize((size_t)tris * 6);
	for (int k = 0; k < 3; k++) { is.mn[k] = 1e30f; is.mx[k] = -1e30f; }
	for (uint32_t t = 0; t < tris; t++)
	{
		is.tris[t] = t;
		for (uint32_t k = 0; k < 3; k++)
		{
			const float* p = &verts[(size_t)indices[first + (size_t)t * 3 + k] * stride];
			for (int a = 0; a < 3; a++) { is.mn[a] = std::min(is.mn[a], p[a]); is.mx[a] = std::max(is.mx[a], p[a]); }
			is.uv[(size_t)t * 6 + (size_t)k * 2] = p[6];
			is.uv[(size_t)t * 6 + (size_t)k * 2 + 1] = p[7];
		}
	}
	return is;
}

bool dark(const Island& island, float tileX, float tileY, const Texel& tex, float maxLuma)
{
	if (!tex.luma && !tex.rgba) { return false; }
	const float tx = tileX > 0 ? tileX : 1.0f, ty = tileY > 0 ? tileY : 1.0f;
	float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
	for (size_t i = 0; i + 1 < island.uv.size(); i += 2)
	{
		const float u = island.uv[i] * tx, w = island.uv[i + 1] * ty;
		mn[0] = std::min(mn[0], u); mx[0] = std::max(mx[0], u);
		mn[1] = std::min(mn[1], w); mx[1] = std::max(mx[1], w);
	}
	// A flat-coloured face pins its UVs to (about) one texel: no quad. In tile units against the 128-texel thumbnail: each side covers at least 2 texels
	// and the rectangle at least 16, so the verdict rests on a patch, not on whatever one blurred texel a flat panel's UVs land on.
	const float wu = mx[0] - mn[0], wv = mx[1] - mn[1];
	if (std::min(wu, wv) < 2.0f / 128.0f || wu * wv < 16.0f / (128.0f * 128.0f)) { return false; }
	// Every texel of the rectangle, not just the corners (a patch with a bright detail inside it is not the barrier's flat backing).
	constexpr int kN = 5;
	for (int j = 0; j < kN; j++)
	{
		for (int i = 0; i < kN; i++)
		{
			const float u = mn[0] + wu * ((float)i + 0.5f) / kN, w = mn[1] + wv * ((float)j + 0.5f) / kN;
			if (tex.at(u, w) > maxLuma) { return false; }
		}
	}
	return true;
}

bool twin(const Island& island, float tileX, float tileY, const Texel& tex, float maxLuma)
{
	if (!tex.luma && !tex.rgba) { return false; }
	const float tx = tileX > 0 ? tileX : 1.0f, ty = tileY > 0 ? tileY : 1.0f;
	float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f };
	for (size_t i = 0; i + 1 < island.uv.size(); i += 2)
	{
		const float u = island.uv[i] * tx, w = island.uv[i + 1] * ty;
		mn[0] = std::min(mn[0], u); mx[0] = std::max(mx[0], u);
		mn[1] = std::min(mn[1], w); mx[1] = std::max(mx[1], w);
	}
	if (mx[0] - mn[0] > 2.0f / 128.0f || mx[1] - mn[1] > 2.0f / 128.0f) { return false; }	// not flat-coloured
	return tex.at(mn[0], mn[1]) <= maxLuma && tex.at(mx[0], mx[1]) <= maxLuma;
}

std::vector<uint32_t> triangles(const std::vector<Island>& islands, float tileX, float tileY, const Texel& tex, float maxLuma, float pad)
{
	std::vector<uint32_t> out;
	std::vector<size_t> darkAt;
	for (size_t i = 0; i < islands.size(); i++)
	{
		if (!dark(islands[i], tileX, tileY, tex, maxLuma)) { continue; }
		darkAt.push_back(i);
		out.insert(out.end(), islands[i].tris.begin(), islands[i].tris.end());
	}
	for (size_t i = 0; i < islands.size() && !darkAt.empty(); i++)
	{
		if (dark(islands[i], tileX, tileY, tex, maxLuma) || !twin(islands[i], tileX, tileY, tex, maxLuma)) { continue; }
		bool shares = false;
		for (const size_t d : darkAt)
		{
			const Island& o = islands[d];
			if (islands[i].mn[0] > o.mx[0] + pad || islands[i].mx[0] < o.mn[0] - pad) { continue; }
			if (islands[i].mn[1] > o.mx[1] + pad || islands[i].mx[1] < o.mn[1] - pad) { continue; }
			if (islands[i].mn[2] > o.mx[2] + pad || islands[i].mx[2] < o.mn[2] - pad) { continue; }
			shares = true;
			break;
		}
		if (shares) { out.insert(out.end(), islands[i].tris.begin(), islands[i].tris.end()); }
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}
}
