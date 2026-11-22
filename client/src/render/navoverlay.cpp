#include "render/navoverlay.hpp"
#include "render/drawstats.hpp"
#include "render/overlaycolours.hpp"
#include "app/config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <vector>

#include <DetourNavMesh.h>

#include "knav/navmesh.hpp"
#include "knav/travel.hpp"
#include "sokol_gfx.h"
#include "render/overlaylist.hpp"	// the overlay is recorded on logic (render sitting 11) and played in the overlay pass

namespace
{
	constexpr float kNoLimit = 900;	// the fade's reach when the range is 0 (off): full strength within half of it, gone at it (MINE)
	// The range (user 2026-11-21, kenshi-k16.2.500): one radius for the navmesh and collision overlays, default one room. 0 = no limit.
	const cfg::Float kRange{ cfg::Group{ "Navmesh" }, "debug.overlay_range", 300, 0, 900,
		"Debug overlays: the radius (world units, 10 per metre) round the selected character, else the camera focus, within which the navmesh and collision overlays draw; the fade sits inside it (full to half, gone at it). 0: no limit (the fade at 900)" };
	constexpr float kLift = 2;	// over the polygon, so the lines sit on its surface rather than in it
	constexpr float kDepth = 4;	// below the band: gone this many bands down (MINE)
	constexpr double kHoverDy = 12;	// the hover polygon: within this of the ground hit in height (ClientNav's walkable extent 10, and a margin)
	constexpr float kStairRange = 4, kStairFill = 0.45f;	// stairs: shown at full strength within this many bands of the level; their fill alpha (MINE)
	constexpr float kAbove = 0.45f, kAboveCut = 0.25f, kBelow = 0.45f, kFill = 0.12f;	// weights one band past the level: above, above in a cut-away floor, below; the level's fill alpha
	// The overlay's share of the default sgl context's 262,144 vertices (k16.19.6.1, MINE): every polygon and edge inside the 900 u fade used to go out with
	// no bound (a dense town's navmesh is tens of thousands of polygons: hundreds of thousands of vertices), so a full context silently dropped the HUD,
	// minimap, labels and windows after it, the same k16.19.6 bug the trails had. Past the cap the overlay stops and says so; the rest of the frame keeps its vertices.
	constexpr int kMaxVertices = 56000;
}

float overlay_range() { const double r = kRange.get(); return r > 0 ? (float)r : kNoLimit; }

void draw_navmesh(const knav::NavMesh& mesh, const float mvp[16], const double cam[3], const double focus[3], float width, float height, const NavBands& bands)
{
	const dtNavMesh& nav = mesh.detour();
	const float kRadius = overlay_range();	// the range: the fade's reach, nothing drawn past it
	int skipped = 0;	// polygons and edges past kMaxVertices this frame (k16.19.6.1); the cap is reported once a run
	auto project = [&](const float* v, float& sx, float& sy)
	{
		const float p[3] = { (float)(v[0] - cam[0]), (float)(v[1] + kLift - cam[1]), (float)(v[2] - cam[2]) };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { return false; }
		sx = (c[0] / c[3] * 0.5f + 0.5f) * width;
		sy = (0.5f - c[1] / c[3] * 0.5f) * height;
		return true;
	};
	auto fade = [&](const float* a, const float* b)
	{
		const float d = (float)std::hypot((a[0] + b[0]) * 0.5 - focus[0], (a[2] + b[2]) * 0.5 - focus[2]);
		return std::clamp(2.0f - 2.0f * d / kRadius, 0.0f, 1.0f);
	};
	// the band's weight at height y: 1 within it, easing over the next band to kAbove (kAboveCut in a cut-away floor: still shown, so a stair's top
	// can be clicked to) or kBelow, then below it fading out by kDepth bands down; grey = how far below (so a stair reads as one ramp, no step)
	auto band = [&](const float* p, float& grey)
	{
		const double dy = p[1] - bands.ref;
		grey = 0;
		if (std::abs(dy) <= bands.band) { return 1.0f; }
		const float t = (float)((std::abs(dy) - bands.band) / bands.band);
		if (dy > 0)
		{
			const double at[3] = { p[0], p[1], p[2] };
			const float lo = bands.cut && bands.cut(at) ? kAboveCut : kAbove;
			return t < 1 ? 1 - t * (1 - lo) : lo;
		}
		grey = std::min(1.0f, t * 0.5f + std::max(0.0f, t - 1) / kDepth);
		if (t < 1) { return 1 - t * (1 - kBelow); }
		return std::max(0.0f, kBelow * (1 - (t - 1) / kDepth));
	};
	auto visible = [&](const dtMeshHeader& h)
	{
		return !(focus[0] < h.bmin[0] - kRadius || focus[0] > h.bmax[0] + kRadius || focus[2] < h.bmin[2] - kRadius || focus[2] > h.bmax[2] + kRadius);
	};
	// a polygon's centre and unit normal (Newell's method; y up)
	auto plane = [](const dtMeshTile* tile, const dtPoly& poly, float c[3], float n[3])
	{
		c[0] = c[1] = c[2] = n[0] = n[1] = n[2] = 0;
		for (int j = 0; j < poly.vertCount; j++)
		{
			const float* u = &tile->verts[poly.verts[j] * 3];
			const float* v = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
			n[0] += (u[1] - v[1]) * (u[2] + v[2]);
			n[1] += (u[2] - v[2]) * (u[0] + v[0]);
			n[2] += (u[0] - v[0]) * (u[1] + v[1]);
			for (int k = 0; k < 3; k++) { c[k] += u[k] / poly.vertCount; }
		}
		const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (l > 0) { for (int k = 0; k < 3; k++) { n[k] /= l; } }
		if (n[1] < 0) { for (int k = 0; k < 3; k++) { n[k] = -n[k]; } }
	};
	const float stairCos = std::cos(bands.stair * 0.0174533f), flatCos = std::cos(bands.flat * 0.0174533f);
	auto stairWeight = [&](const float* c) { return std::clamp(kStairRange + 1 - (float)(std::abs(c[1] - bands.ref) / bands.band), 0.0f, 1.0f); };
	// the surface: the polygon holding `at` (in XZ, the nearest in height within the band), flooded across links while the slope barely changes
	// the polygon holding p in XZ whose centre is nearest it in height, within maxDy; 0 none
	auto under = [&](const double p[3], double maxDy)
	{
		dtPolyRef found = 0;
		double best = maxDy;
		for (int t = 0; t < nav.getMaxTiles(); t++)
		{
			const dtMeshTile* tile = nav.getTile(t);
			if (!tile || !tile->header || !visible(*tile->header)) { continue; }
			for (int i = 0; i < tile->header->polyCount; i++)
			{
				const dtPoly& poly = tile->polys[i];
				if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.flags == 0) { continue; }	// flags 0: no seed reaches it (the bake's prune)
				bool in = false;
				for (int j = 0, k = poly.vertCount - 1; j < poly.vertCount; k = j++)
				{
					const float* u = &tile->verts[poly.verts[j] * 3];
					const float* v = &tile->verts[poly.verts[k] * 3];
					if ((u[2] > p[2]) != (v[2] > p[2]) && p[0] < (v[0] - u[0]) * (p[2] - u[2]) / (v[2] - u[2]) + u[0]) { in = !in; }
				}
				float c[3], n[3];
				plane(tile, poly, c, n);
				if (in && std::abs(c[1] - p[1]) <= best) { best = std::abs(c[1] - p[1]); found = nav.getPolyRefBase(tile) | (dtPolyRef)i; }
			}
		}
		return found;
	};
	std::unordered_set<dtPolyRef> surface;
	{
		const dtPolyRef start = under(bands.at, bands.band);
		std::vector<dtPolyRef> open;
		if (start) { open.push_back(start); surface.insert(start); }
		while (!open.empty())
		{
			const dtPolyRef r = open.back();
			open.pop_back();
			const dtMeshTile* tile;
			const dtPoly* poly;
			nav.getTileAndPolyByRefUnsafe(r, &tile, &poly);
			float c[3], n[3];
			plane(tile, *poly, c, n);
			for (unsigned k = poly->firstLink; k != DT_NULL_LINK; k = tile->links[k].next)
			{
				const dtPolyRef o = tile->links[k].ref;
				const dtMeshTile* ot;
				const dtPoly* op;
				if (surface.count(o) || dtStatusFailed(nav.getTileAndPolyByRef(o, &ot, &op)) || op->getType() == DT_POLYTYPE_OFFMESH_CONNECTION || op->flags == 0) { continue; }
				float oc[3], on[3];
				plane(ot, *op, oc, on);
				if (on[1] < stairCos || n[0] * on[0] + n[1] * on[1] + n[2] * on[2] < flatCos || fade(oc, oc) <= 0) { continue; }
				surface.insert(o);
				open.push_back(o);
			}
		}
	}
	ovl::disable_texture();
	// fills: the surface faintly (a fan each), so the floor being walked reads as one; stairs and ramps orange
	ovl::begin_triangles();
	for (int t = 0; t < nav.getMaxTiles(); t++)
	{
		const dtMeshTile* tile = nav.getTile(t);
		if (!tile || !tile->header || !visible(*tile->header)) { continue; }
		for (int i = 0; i < tile->header->polyCount; i++)
		{
			const dtPoly& poly = tile->polys[i];
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || poly.flags == 0) { continue; }
			float c[3], n[3];
			plane(tile, poly, c, n);
			const bool stair = n[1] < stairCos;
			const float f = fade(c, c) * (stair ? stairWeight(c) * kStairFill : surface.count(nav.getPolyRefBase(tile) | (dtPolyRef)i) ? kFill : 0.0f);
			if (bands.trace && stair)
			{
				float px = -1, py = -1;
				const bool front = project(c, px, py);
				printf("navdraw stair: tile %d.%d poly %d centre %.1f %.1f %.1f slope %.1f flags 0x%x screen %.0f %.0f%s fade %.2f weight %.2f fill %.3f\n", tile->header->x, tile->header->y, i, c[0], c[1], c[2],
					std::acos(std::min(1.0f, n[1])) * 57.2958f, poly.flags, px, py, front ? "" : " (behind)", fade(c, c), stairWeight(c), f);
			}
			if (f <= 0) { continue; }
			if (ovl::num_vertices() + (poly.vertCount - 2) * 3 > kMaxVertices) { skipped++; continue; }	// k16.19.6.1: never fill the default context
			float sx[DT_VERTS_PER_POLYGON], sy[DT_VERTS_PER_POLYGON];
			bool ok = true;
			for (int j = 0; j < poly.vertCount && ok; j++) { ok = project(&tile->verts[poly.verts[j] * 3], sx[j], sy[j]); }
			if (!ok) { continue; }
			if (stair) { ovl::c4f(1.0f, 0.55f, 0.1f, f); }
			else { ovl::c4f(0.1f, 0.9f, 1.0f, f); }
			for (int j = 1; j + 1 < poly.vertCount; j++)
			{
				ovl::v2f(sx[0], sy[0]);
				ovl::v2f(sx[j], sy[j]);
				ovl::v2f(sx[j + 1], sy[j + 1]);
			}
		}
	}
	ovl::count_triangles();
	ovl::end();
	// the hover: the polygon under the mouse's ground hit, white, over everything (a click through a cut floor shows where it lands)
	if (const dtPolyRef h = bands.hover ? under(bands.hoverAt, kHoverDy) : 0)
	{
		const dtMeshTile* tile;
		const dtPoly* poly;
		nav.getTileAndPolyByRefUnsafe(h, &tile, &poly);
		float sx[DT_VERTS_PER_POLYGON], sy[DT_VERTS_PER_POLYGON];
		bool ok = true;
		for (int j = 0; j < poly->vertCount && ok; j++) { ok = project(&tile->verts[poly->verts[j] * 3], sx[j], sy[j]); }
		if (ok)
		{
			ovl::begin_triangles();
			ovl::c4f(1.0f, 1.0f, 1.0f, 0.3f);
			for (int j = 1; j + 1 < poly->vertCount; j++) { ovl::v2f(sx[0], sy[0]); ovl::v2f(sx[j], sy[j]); ovl::v2f(sx[j + 1], sy[j + 1]); }
			ovl::count_triangles();
			ovl::end();
			ovl::begin_lines();
			ovl::c4f(1.0f, 1.0f, 1.0f, 1.0f);
			for (int j = 0; j < poly->vertCount; j++) { const int k = (j + 1) % poly->vertCount; ovl::v2f(sx[j], sy[j]); ovl::v2f(sx[k], sy[k]); }
			ovl::count_lines();
			ovl::end();
		}
	}
	ovl::begin_lines();
	for (int t = 0; t < nav.getMaxTiles(); t++)
	{
		const dtMeshTile* tile = nav.getTile(t);
		if (!tile || !tile->header || !visible(*tile->header)) { continue; }
		const dtMeshHeader& h = *tile->header;
		for (int i = 0; i < h.polyCount; i++)
		{
			const dtPoly& poly = tile->polys[i];
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION || (poly.flags == 0 && !bands.pruned)) { continue; }
			const knav::Area area = (knav::Area)poly.getArea();
			float pc[3], pn[3];
			plane(tile, poly, pc, pn);
			const float stair = pn[1] < stairCos ? stairWeight(pc) : 0.0f;
			for (int j = 0; j < poly.vertCount; j++)
			{
				const float* a = &tile->verts[poly.verts[j] * 3];
				const float* b = &tile->verts[poly.verts[(j + 1) % poly.vertCount] * 3];
				unsigned short nei = poly.neis[j];
				if (nei & DT_EXT_LINK)	// a portal: linked only when the next tile is there; unlinked = a gap in the client's tiles
				{
					bool linked = false;
					for (unsigned k = poly.firstLink; k != DT_NULL_LINK; k = tile->links[k].next) { linked |= tile->links[k].edge == j; }
					if (!linked) { nei = 0; }
				}
				// each inner edge once (from the lower polygon), seams and rims always
				if (nei != 0 && !(nei & DT_EXT_LINK) && nei - 1 < i) { continue; }
				const float mid[3] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f };
				float grey;
				const float bw = band(mid, grey);
				if (stair > bw) { grey = 0; }
				const float f = fade(a, b) * std::max(bw, stair);
				if (f <= 0) { continue; }
				if (ovl::num_vertices() + 2 > kMaxVertices) { skipped++; continue; }	// k16.19.6.1: never fill the default context
				auto set = [&](const OverlayRgba& c, float& r, float& g, float& bl, float& al) { r = c.r; g = c.g; bl = c.b; al = c.a; };
				float r, g, bl, al;
				set(overlay_colour::kNavInner, r, g, bl, al);	// inner: the cyan grid
				// an inner edge between two flat, level polygons of one area is most likely a split of one original face (the bake caps a face at 6 corners
				// and cuts at tile seams, and the cache keeps no face id): drawn fainter than a true face edge (kenshi-uuh8)
				bool split = false;
				if (nei != 0 && !(nei & DT_EXT_LINK) && nei - 1 < tile->header->polyCount)
				{
					const dtPoly& other = tile->polys[nei - 1];
					float oc[3], on[3];
					plane(tile, other, oc, on);
					split = other.getArea() == poly.getArea() && other.flags == poly.flags && pn[0] * on[0] + pn[1] * on[1] + pn[2] * on[2] > 0.9995f && std::abs(oc[1] - pc[1]) < 2.0f;
				}
				if (poly.flags == 0) { r = 0.8f; g = 0.15f; bl = 0.15f; al = 0.5f; }	// pruned (debug.navmesh_pruned)
				else if (area == knav::Area::DoorOpen) { r = 0.3f; g = 1.0f; bl = 0.3f; al = 0.9f; }
				else if (area == knav::Area::DoorClosed) { r = 1.0f; g = 0.25f; bl = 0.2f; al = 0.9f; }
				else if (nei == 0) { set(overlay_colour::kNavRim, r, g, bl, al); }	// the rim: walls, cliffs, gaps
				else if (nei & DT_EXT_LINK) { set(overlay_colour::kNavSeam, r, g, bl, al); }	// a tile seam
				else if (stair > 0) { set(overlay_colour::kNavStair, r, g, bl, al); }	// a stair or ramp
				else if (split && area != knav::Area::Edge && area != knav::Area::Shallow && area != knav::Area::Swim) { set(overlay_colour::kNavSplit, r, g, bl, al); }	// a split of one face
				else if (area == knav::Area::Edge) { r = 0.9f; g = 0.35f; bl = 0.75f; al = 0.45f; }	// the wall band (costlier: routes keep off walls)
				else if (area == knav::Area::Shallow || area == knav::Area::Swim) { r = 0.2f; g = 0.4f; bl = 1.0f; }
				float ax, ay, bx, by;
				if (!project(a, ax, ay) || !project(b, bx, by)) { continue; }
				const float lum = (r + g + bl) / 3;
				r += (lum - r) * grey; g += (lum - g) * grey; bl += (lum - bl) * grey;
				ovl::c4f(r, g, bl, al * f);
				ovl::v2f(ax, ay);
				ovl::v2f(bx, by);
			}
		}
	}
	ovl::count_lines();
	ovl::end();
	static bool warned = false;
	if (skipped && !warned) { warned = true; fprintf(stderr, "sgl: navmesh overlay capped at %d vertices: %d polygons and edges skipped this frame (k16.19.6.1)\n", kMaxVertices, skipped); }
	if (bands.trace) { printf("navdraw: %d vertices drawn, %d polygons and edges skipped (cap %d)\n", ovl::num_vertices(), skipped, kMaxVertices); }
}

#ifdef KCLIENT_DEV_TOOLS
void dump_navmesh_at(const knav::NavMesh& mesh, double x, double z)
{
	const dtNavMesh& nav = mesh.detour();
	int n = 0;
	for (int t = 0; t < nav.getMaxTiles(); t++)
	{
		const dtMeshTile* tile = nav.getTile(t);
		if (!tile || !tile->header || x < tile->header->bmin[0] || x > tile->header->bmax[0] || z < tile->header->bmin[2] || z > tile->header->bmax[2]) { continue; }
		for (int i = 0; i < tile->header->polyCount; i++)
		{
			const dtPoly& poly = tile->polys[i];
			if (poly.getType() == DT_POLYTYPE_OFFMESH_CONNECTION) { continue; }
			bool in = false;
			float c[3] = {}, nm[3] = {};
			for (int j = 0, k = poly.vertCount - 1; j < poly.vertCount; k = j++)
			{
				const float* u = &tile->verts[poly.verts[j] * 3];
				const float* v = &tile->verts[poly.verts[k] * 3];
				if ((u[2] > z) != (v[2] > z) && x < (v[0] - u[0]) * (z - u[2]) / (v[2] - u[2]) + u[0]) { in = !in; }
				nm[0] += (v[1] - u[1]) * (v[2] + u[2]);
				nm[1] += (v[2] - u[2]) * (v[0] + u[0]);
				nm[2] += (v[0] - u[0]) * (v[1] + u[1]);
				for (int m = 0; m < 3; m++) { c[m] += u[m] / poly.vertCount; }
			}
			if (!in) { continue; }
			const float l = std::sqrt(nm[0] * nm[0] + nm[1] * nm[1] + nm[2] * nm[2]);
			const float slope = l > 0 ? std::acos(std::min(1.0f, std::abs(nm[1]) / l)) * 57.2958f : 0;
			printf("nav at %.1f %.1f: tile %d.%d poly %d centre %.1f %.1f %.1f slope %.1f area %d flags 0x%x verts %d\n", x, z, tile->header->x, tile->header->y, i, c[0], c[1], c[2], slope, (int)poly.getArea(), poly.flags, poly.vertCount);
			n++;
		}
	}
	if (n == 0) { printf("nav at %.1f %.1f: no polygon\n", x, z); }
}
#endif
