#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/config.hpp"
#include "dev/devhooks.hpp"
#include "input/collisionpick.hpp"
#include "kworld/heightmap.hpp"
#include "render/overlaycolours.hpp"
#include "render/overlaylist.hpp"
#include "ui/keys.hpp"

// The collision debug overlay (debug.collision, Ctrl+F7; user 2026-11-18, kenshi-k16.2.383.1; wording kenshi-k16.2.382.4): the collision shapes and the
// drawn terrain, within the overlay range (debug.overlay_range, render/navoverlay.cpp overlay_range()) of the centre (the selected character, else the
// camera focus: host/logicframe.cpp) as wire lines over everything, fading from full at half the range to nothing at it exactly as the navmesh overlay
// does, so the two can be flipped over the same spot (coordinator 2026-11-18: the navmesh is too busy to read both at once).
// Each shape is drawn as its OUTLINE (user 2026-11-21, kenshi-u59y / kenshi-uuh8: "the staircase is rendered inside that collision box PERFECTLY", the
// original draws box outlines): the feature edges only, an edge held by two coplanar triangles (the diagonal of a quad) is not drawn, so a box is its
// twelve edges and a stair its steps. The fade is per vertex along each edge (an edge is clipped to the range and cut into pieces), not per triangle
// centroid: a floor or wall slab hundreds of units across has its centre by the camera and its corners far away, and the centroid fade drew all of it at
// full strength to the horizon (the user's shot, kenshi-k16.2.500).
// The colours are one table shared with the navmesh overlay and kinspect (render/overlaycolours.hpp, docs/CLIENT.md "Debug overlays"), by the shape's
// kworld::WorldShape fields: a doorway yellow, foliage brown, a stair / gateway body (`solid`) orange, furniture and thin props grey, an obstacle
// (barricade) red, a projected footprint magenta, any other building shape bluish white; the terrain green.
// The shapes are the CollisionPick gather's: the kworld::AreaGatherer's shapes the sim's navmesh bake and the collision queries read.
//
// Its own key and settings key, dev-only (this file compiles only with KCLIENT_DEV_TOOLS), --debug only, off by default; the game reaches it through the
// devhooks.hpp hooks (collisionOn / collisionToggle / collisionLegend / collisionOverlay), so an OFF build has none of it. It draws:
//   - the collision shapes' outlines (above). These block movement and line of sight, and no body stands on them (kenshi-k16.2.382.2).
//   - the drawn terrain, which bodies stand on: a green wire grid on the 18 u lattice, each cell's two edges and the split diagonal its two triangles
//     share. Sampled with kworld::Heightmap::terrainHeight (kenshi-k16.2.382.1: the stand ray lands on the drawn triangles, not the bilinear height();
//     the two agree on the lattice lines and differ by up to ~0.5 u inside a cell, which is where the diagonal is).
// The debug text's legend line (collisionLegend, host/hud.cpp) counts what the last draw put out. The vertices are capped (kMaxVertices, the same
// k16.19.6.1 rule the navmesh overlay follows): past it the overlay stops and the legend says "capped", so the HUD after it keeps its vertices.
namespace
{
	constexpr float kLift = 0.5;	// over the surface, so a line sits on it rather than in it
	constexpr int kMaxVertices = 90000;	// this overlay's share of the default sgl context: the terrain grid alone is ~47,000 near the focus at 900 u (k16.19.6.1)
	constexpr float kCoplanar = 0.9995f;	// cos of ~1.8 degrees: two triangles whose normals agree this closely are one face
	const cfg::Bool kCollision{ cfg::Group{ "Navmesh" }, "debug.collision", false,
		"Debug: collision and terrain, within debug.overlay_range of the selected character, else the camera focus: each collision shape's outline (feature edges only), coloured by use (walls and floors bluish white, doorways yellow, stairs orange, furniture and thin props grey, barricades red, foliage brown), the barricades that block movement and line of sight, which bodies do not stand on since k16.2.382.2, and the drawn terrain bodies stand on (Heightmap::terrainHeight, the drawn triangles) as a green wire grid on its 18 u lattice; the F10 height probe lists what a body stands on at a point" };
	struct Stats
	{
		int shapes = 0, edges = 0, cells = 0, skipped = 0;
	};
	Stats g_stats;	// the last draw's counts (the debug text's legend line; both run on the logic thread)

	// A shape's feature edges, 6 floats each (x y z, x y z): welded by position (the shapes repeat vertices per face), an edge of one triangle (a boundary), of
	// three or more, or of two that are not coplanar.
	std::vector<float> featureEdges(const kworld::WorldShape& s)
	{
		struct Key
		{
			long long x, y, z;
			bool operator<(const Key& o) const { return x != o.x ? x < o.x : y != o.y ? y < o.y : z < o.z; }
		};
		std::map<Key, int> weld;
		const size_t nv = s.vertices.size() / 3;
		std::vector<int> id(nv);
		for (size_t i = 0; i < nv; i++)
		{
			const Key k{ std::llround(s.vertices[i * 3] * 100.0), std::llround(s.vertices[i * 3 + 1] * 100.0), std::llround(s.vertices[i * 3 + 2] * 100.0) };
			id[i] = weld.try_emplace(k, (int)weld.size()).first->second;
		}
		struct Edge
		{
			int count = 0;
			float n[3] = {};
			bool flat = true;
			size_t a = 0, b = 0;	// the first triangle's vertex indices
		};
		std::map<std::pair<int, int>, Edge> edges;
		for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
		{
			const size_t v[3] = { s.indices[i], s.indices[i + 1], s.indices[i + 2] };
			if (v[0] >= nv || v[1] >= nv || v[2] >= nv) { continue; }
			float e1[3], e2[3];
			for (int k = 0; k < 3; k++)
			{
				e1[k] = s.vertices[v[1] * 3 + k] - s.vertices[v[0] * 3 + k];
				e2[k] = s.vertices[v[2] * 3 + k] - s.vertices[v[0] * 3 + k];
			}
			float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (len < 1e-9f) { continue; }	// a sliver: no plane, no edges
			for (float& c : n) { c /= len; }
			for (int k = 0; k < 3; k++)
			{
				const size_t p = v[k], q = v[(k + 1) % 3];
				const std::pair<int, int> key = std::minmax(id[p], id[q]);
				if (key.first == key.second) { continue; }
				Edge& e = edges[key];
				if (e.count == 0) { std::copy(n, n + 3, e.n); e.a = p; e.b = q; }
				else if (e.count == 1) { e.flat = e.n[0] * n[0] + e.n[1] * n[1] + e.n[2] * n[2] > kCoplanar; }
				e.count++;
			}
		}
		std::vector<float> out;
		for (const auto& kv : edges)
		{
			const Edge& e = kv.second;
			if (e.count == 2 && e.flat) { continue; }
			for (const size_t p : { e.a, e.b }) { out.insert(out.end(), s.vertices.begin() + p * 3, s.vertices.begin() + p * 3 + 3); }
		}
		return out;
	}

	// The outlines are static per gather: kept until the gather is replaced (the shared_ptr keeps the shapes, so a shape's address stays its own).
	struct EdgeCache
	{
		std::shared_ptr<const std::vector<kworld::WorldShape>> owner;
		std::unordered_map<const kworld::WorldShape*, std::vector<float>> edges;
	};
	EdgeCache g_cache;

	OverlayRgba colourOf(const kworld::WorldShape& s, const float box[6])
	{
		using kworld::NavUse;
		if (s.use == NavUse::Door) { return overlay_colour::kDoor; }
		if (s.foliage) { return overlay_colour::kFoliage; }
		if (s.solid) { return overlay_colour::kStairs; }
		if (s.furniture || CollisionPick::thin(box)) { return overlay_colour::kFurniture; }
		if (s.use == NavUse::Obstacle) { return overlay_colour::kBarricade; }
		if (s.use == NavUse::Projected) { return overlay_colour::kProjected; }
		return overlay_colour::kBuilding;
	}
}

namespace dev
{
	bool collisionOn() { return kCollision.get(); }
	void collisionToggle() { kCollision.set(!kCollision.get()); }

	bool collisionLegend(std::string& out)
	{
		if (!kCollision.get()) { return false; }
		char line[256];
		const std::string key = ui::keys::label("debug.collision");
		snprintf(line, sizeof line, "collision %d shapes, %d edges, terrain %d cells, range %.0f u%s (%s)", g_stats.shapes, g_stats.edges, g_stats.cells,
			(double)overlay_range(), g_stats.skipped ? ", capped" : "", key.c_str());
		out = line;
		return true;
	}

	void collisionOverlay(const CollisionPick& pick, const kworld::Heightmap* hm, const double focus[3], const float mvp[16], const double cam[3],
		float width, float height)
	{
		g_stats = {};
		if (!kCollision.get()) { return; }
		const double range = overlay_range();	// the hard cut, and the fade's reach: full to half of it, gone at it (the navmesh overlay's rule)
		auto project = [&](double x, double y, double z, float& sx, float& sy)
		{
			const float p[3] = { (float)(x - cam[0]), (float)(y + kLift - cam[1]), (float)(z - cam[2]) };
			float c[4];
			for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
			if (c[3] < 1.0f) { return false; }
			sx = (c[0] / c[3] * 0.5f + 0.5f) * width;
			sy = (0.5f - c[1] / c[3] * 0.5f) * height;
			return true;
		};
		auto fade = [&](double x, double z) { return std::clamp(2.0 - 2.0 * std::hypot(x - focus[0], z - focus[2]) / range, 0.0, 1.0); };
		// one world edge: clipped to the range disc (in XZ), cut into pieces so the fade follows the distance along it, a colour per vertex
		auto edge = [&](const float* a, const float* b, const OverlayRgba& c)
		{
			const double dx = (double)b[0] - a[0], dz = (double)b[2] - a[2];
			const double fx = (double)a[0] - focus[0], fz = (double)a[2] - focus[2];
			const double qa = dx * dx + dz * dz, qb = 2 * (fx * dx + fz * dz), qc = fx * fx + fz * fz - range * range;
			double t0 = 0, t1 = 1;
			if (qa < 1e-9)
			{
				if (qc > 0) { return; }
			}
			else
			{
				const double disc = qb * qb - 4 * qa * qc;
				if (disc <= 0) { return; }
				const double r = std::sqrt(disc);
				t0 = std::max(0.0, (-qb - r) / (2 * qa));
				t1 = std::min(1.0, (-qb + r) / (2 * qa));
				if (t0 >= t1) { return; }
			}
			const double len = std::sqrt(qa) * (t1 - t0);
			const int n = std::clamp((int)std::ceil(len / (range / 6)), 1, 12);
			if (ovl::num_vertices() + 2 * n > kMaxVertices) { g_stats.skipped++; return; }
			double prev[3];
			float psx = 0, psy = 0, pf = 0;
			bool pok = false;
			for (int i = 0; i <= n; i++)
			{
				const double t = t0 + (t1 - t0) * i / n;
				const double x = a[0] + (b[0] - a[0]) * t, y = a[1] + (b[1] - a[1]) * t, z = a[2] + (b[2] - a[2]) * t;
				float sx, sy;
				const bool ok = project(x, y, z, sx, sy);
				const float f = (float)fade(x, z);
				if (i > 0 && ok && pok)
				{
					ovl::c4f(c.r, c.g, c.b, c.a * pf);
					ovl::v2f(psx, psy);
					ovl::c4f(c.r, c.g, c.b, c.a * f);
					ovl::v2f(sx, sy);
				}
				(void)prev;
				psx = sx; psy = sy; pf = f; pok = ok;
			}
			g_stats.edges++;
		};
		ovl::disable_texture();
		// the collision shapes the gather holds (buildings first, then foliage): outlines, coloured by use
		ovl::begin_lines();
		if (const std::shared_ptr<const std::vector<kworld::WorldShape>> shapes = pick.gathered())
		{
			if (g_cache.owner != shapes) { g_cache.owner = shapes; g_cache.edges.clear(); }
			for (const kworld::WorldShape& s : *shapes)
			{
				if (s.max[0] < focus[0] - range || s.min[0] > focus[0] + range || s.max[2] < focus[2] - range || s.min[2] > focus[2] + range) { continue; }
				const float box[6] = { (float)s.min[0], (float)s.min[1], (float)s.min[2], (float)s.max[0], (float)s.max[1], (float)s.max[2] };
				auto it = g_cache.edges.find(&s);
				if (it == g_cache.edges.end()) { it = g_cache.edges.emplace(&s, featureEdges(s)).first; }
				const std::vector<float>& e = it->second;
				const int before = g_stats.edges;
				const OverlayRgba c = colourOf(s, box);
				for (size_t i = 0; i + 5 < e.size(); i += 6) { edge(&e[i], &e[i + 3], c); }
				if (g_stats.edges > before) { g_stats.shapes++; }
			}
		}
		ovl::count_lines();
		ovl::end();
		// the drawn terrain bodies stand on: a wire grid on the 18 u lattice, each cell's two edges and its split diagonal, so the grid's spacing
		// is the lattice spacing and its cells are the drawn triangles terrainHeight() interpolates over
		if (hm)
		{
			constexpr double spacing = kworld::kHeightmapSpacing;
			const long i0 = (long)std::floor((focus[0] - range - kworld::kWorldMin) / spacing);
			const long i1 = (long)std::ceil((focus[0] + range - kworld::kWorldMin) / spacing);
			const long j0 = (long)std::floor((focus[2] - range - kworld::kWorldMin) / spacing);
			const long j1 = (long)std::ceil((focus[2] + range - kworld::kWorldMin) / spacing);
			const long nx = i1 - i0 + 1, nz = j1 - j0 + 1;
			std::vector<double> ys((size_t)(nx * nz));	// a lattice node's height, sampled once: four cells share it
			for (long j = 0; j < nz; j++)
			{
				const double z = kworld::kWorldMin + (j0 + j) * spacing;
				for (long i = 0; i < nx; i++) { ys[(size_t)(j * nx + i)] = hm->terrainHeight(kworld::kWorldMin + (i0 + i) * spacing, z); }
			}
			const OverlayRgba tc = overlay_colour::kTerrain;
			ovl::begin_lines();
			for (long j = 0; j + 1 < nz; j++)
			{
				for (long i = 0; i + 1 < nx; i++)
				{
					const double x0 = kworld::kWorldMin + (i0 + i) * spacing, z0 = kworld::kWorldMin + (j0 + j) * spacing;
					const double x1 = x0 + spacing, z1 = z0 + spacing;
					const float f = (float)fade(x0 + spacing * 0.5, z0 + spacing * 0.5);
					if (f <= 0) { continue; }
					if (ovl::num_vertices() + 6 > kMaxVertices) { g_stats.skipped++; continue; }
					const double y00 = ys[(size_t)(j * nx + i)], y10 = ys[(size_t)(j * nx + i + 1)], y01 = ys[(size_t)((j + 1) * nx + i)];
					ovl::c4f(tc.r, tc.g, tc.b, tc.a * f);
					float ax, ay, bx, by;
					if (project(x0, y00, z0, ax, ay) && project(x1, y10, z0, bx, by)) { ovl::v2f(ax, ay); ovl::v2f(bx, by); }	// the cell's z0 edge
					if (project(x0, y00, z0, ax, ay) && project(x0, y01, z1, bx, by)) { ovl::v2f(ax, ay); ovl::v2f(bx, by); }	// its x0 edge
					// the split of the cell's two drawn triangles, (i+1,j)-(i,j+1) (heightmap.hpp terrainHeight): where the sampled surface creases
					if (project(x1, y10, z0, ax, ay) && project(x0, y01, z1, bx, by)) { ovl::v2f(ax, ay); ovl::v2f(bx, by); }
					g_stats.cells++;
				}
			}
			ovl::count_lines();
			ovl::end();
		}
	}
}
