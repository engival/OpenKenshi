#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/config.hpp"
#include "dev/devhooks.hpp"
#include "input/collisionpick.hpp"
#include "kworld/heightmap.hpp"
#include "render/overlaylist.hpp"
#include "ui/keys.hpp"

// The collision debug overlay (debug.collision, Ctrl+F7; user 2026-11-18, kenshi-k16.2.383.1; wording kenshi-k16.2.382.4): the building collision shapes
// and the drawn terrain, round the camera focus as wire lines over everything, fading with the distance from it exactly as the navmesh overlay does
// (navoverlay.cpp, kRadius) so the two can be flipped over the same spot (coordinator 2026-11-18: the navmesh is too busy to read both at once).
// The collision shapes are the "y axis barricades" that block movement and line of sight (user 2026-11-18); bodies do not stand on them since
// kenshi-k16.2.382.2 (the stand ray reads no collision mesh). Bodies stand on the drawn floors, stairs and ramps a building's own triangles draw, and on
// the drawn terrain; the height probe (Ctrl+F10, `probe height X Z`, dev/heightprobe.cpp) lists what a body actually stands on at a point.
//
// Its own key and settings key, dev-only (this file compiles only with KCLIENT_DEV_TOOLS), --debug only, off by default; the game reaches it through the
// devhooks.hpp hooks (collisionOn / collisionToggle / collisionLegend / collisionOverlay), so an OFF build has none of it. It draws:
//   - the building collision triangles the gather holds, as wire edges: white (a solid shape: a wall, a floor, a stair); yellow: a doorway leaf
//     (NavUse::Door); grey, fainter: a thin prop (CollisionPick::thin: a rail, a pole, a sign). These block movement and line of sight, and no body
//     stands on them (kenshi-k16.2.382.2).
//   - the drawn terrain, which bodies stand on: a green wire grid on the 18 u lattice, each cell's two edges and the split diagonal its two triangles
//     share. Sampled with kworld::Heightmap::terrainHeight (kenshi-k16.2.382.1: the stand ray lands on the drawn triangles, not the bilinear height();
//     the two agree on the lattice lines and differ by up to ~0.5 u inside a cell, which is where the diagonal is).
// The debug text's legend line (collisionLegend, host/hud.cpp) counts what the last draw put out. The vertices are capped (kMaxVertices, the same
// k16.19.6.1 rule the navmesh overlay follows): past it the overlay stops and the legend says "capped", so the HUD after it keeps its vertices.
namespace
{
	constexpr float kRadius = 900;	// the fade: full strength within half of it, gone at it, as the navmesh overlay (MINE)
	constexpr float kLift = 0.5;	// over the surface, so a line sits on it rather than in it
	constexpr int kMaxVertices = 90000;	// this overlay's share of the default sgl context: the terrain grid alone is ~47,000 near the focus (k16.19.6.1)
	const cfg::Bool kCollision{ cfg::Group{ "Navmesh" }, "debug.collision", false,
		"Debug: collision and terrain, round the camera focus: the building collision shapes as wire in white (yellow: doorways, grey: the thin props), the barricades that block movement and line of sight, which bodies do not stand on since k16.2.382.2, and the drawn terrain bodies stand on (Heightmap::terrainHeight, the drawn triangles) as a green wire grid on its 18 u lattice; the F10 height probe lists what a body stands on at a point" };
	struct Stats
	{
		int shapes = 0, triangles = 0, cells = 0, skipped = 0;
	};
	Stats g_stats;	// the last draw's counts (the debug text's legend line; both run on the logic thread)
}

namespace dev
{
	bool collisionOn() { return kCollision.get(); }
	void collisionToggle() { kCollision.set(!kCollision.get()); }

	bool collisionLegend(std::string& out)
	{
		if (!kCollision.get()) { return false; }
		char line[224];
		const std::string key = ui::keys::label("debug.collision");
		snprintf(line, sizeof line, "collision %d shapes, %d triangles, terrain %d cells%s (%s)", g_stats.shapes, g_stats.triangles, g_stats.cells,
			g_stats.skipped ? ", capped" : "", key.c_str());
		out = line;
		return true;
	}

	void collisionOverlay(const CollisionPick& pick, const kworld::Heightmap* hm, const double focus[3], const float mvp[16], const double cam[3],
		float width, float height)
	{
		g_stats = {};
		if (!kCollision.get()) { return; }
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
		auto fade = [&](double x, double z) { return std::clamp(2.0 - 2.0 * std::hypot(x - focus[0], z - focus[2]) / kRadius, 0.0, 1.0); };
		ovl::disable_texture();
		// the building collision triangles the gather holds (buildings first, then foliage, which the overlay skips)
		ovl::begin_lines();
		if (const std::shared_ptr<const std::vector<kworld::WorldShape>> shapes = pick.gathered())
		{
			for (const kworld::WorldShape& s : *shapes)
			{
				if (s.foliage) { continue; }
				if (s.max[0] < focus[0] - kRadius || s.min[0] > focus[0] + kRadius || s.max[2] < focus[2] - kRadius || s.min[2] > focus[2] + kRadius) { continue; }
				const float box[6] = { (float)s.min[0], (float)s.min[1], (float)s.min[2], (float)s.max[0], (float)s.max[1], (float)s.max[2] };
				const bool door = s.use == kworld::NavUse::Door;
				const bool thin = !door && CollisionPick::thin(box);	// a thin prop: the camera sweeps past these too
				float r = 1.0f, g = 1.0f, b = 1.0f, a = 0.9f;	// white: a building's collision
				if (door) { r = 1.0f; g = 0.85f; b = 0.15f; }	// yellow: a doorway
				else if (thin) { r = g = b = 0.55f; a = 0.7f; }	// grey: a thin prop
				bool any = false;
				for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
				{
					float vx[3], vy[3], vz[3];
					for (int k = 0; k < 3; k++)
					{
						const size_t at = (size_t)s.indices[i + k] * 3;
						vx[k] = s.vertices[at]; vy[k] = s.vertices[at + 1]; vz[k] = s.vertices[at + 2];
					}
					const float f = (float)fade((vx[0] + vx[1] + vx[2]) / 3.0, (vz[0] + vz[1] + vz[2]) / 3.0);
					if (f <= 0) { continue; }
					if (ovl::num_vertices() + 6 > kMaxVertices) { g_stats.skipped++; continue; }
					float sx[3], sy[3];
					bool ok = true;
					for (int k = 0; k < 3 && ok; k++) { ok = project(vx[k], vy[k], vz[k], sx[k], sy[k]); }
					if (!ok) { continue; }
					ovl::c4f(r, g, b, a * f);
					for (int k = 0; k < 3; k++)
					{
						const int m = (k + 1) % 3;
						ovl::v2f(sx[k], sy[k]);
						ovl::v2f(sx[m], sy[m]);
					}
					g_stats.triangles++;
					any = true;
				}
				if (any) { g_stats.shapes++; }
			}
		}
		ovl::count_lines();
		ovl::end();
		// the drawn terrain bodies stand on: a wire grid on the 18 u lattice, each cell's two edges and its split diagonal, so the grid's spacing
		// is the lattice spacing and its cells are the drawn triangles terrainHeight() interpolates over
		if (hm)
		{
			constexpr double spacing = kworld::kHeightmapSpacing;
			const long i0 = (long)std::floor((focus[0] - kRadius - kworld::kWorldMin) / spacing);
			const long i1 = (long)std::ceil((focus[0] + kRadius - kworld::kWorldMin) / spacing);
			const long j0 = (long)std::floor((focus[2] - kRadius - kworld::kWorldMin) / spacing);
			const long j1 = (long)std::ceil((focus[2] + kRadius - kworld::kWorldMin) / spacing);
			const long nx = i1 - i0 + 1, nz = j1 - j0 + 1;
			std::vector<double> ys((size_t)(nx * nz));	// a lattice node's height, sampled once: four cells share it
			for (long j = 0; j < nz; j++)
			{
				const double z = kworld::kWorldMin + (j0 + j) * spacing;
				for (long i = 0; i < nx; i++) { ys[(size_t)(j * nx + i)] = hm->terrainHeight(kworld::kWorldMin + (i0 + i) * spacing, z); }
			}
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
					ovl::c4f(0.3f, 1.0f, 0.35f, 0.55f * f);
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
