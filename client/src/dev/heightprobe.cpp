// The vertical height probe (kenshi-k16.2.384.1): dev-only, built with KCLIENT_DEV_TOOLS, used under --debug. At one world x, z it lists every
// surface the vertical line crosses, top to bottom, each with its y and source, and what the stand ray (SimView::standRayAt, update()'s rule over the
// window [wire - kRayDown, wire + kRayUp]) makes of it for a character standing there. Set by a click in probe mode (Ctrl+F10) or the script op
// `probe height X Z` (which also prints the listing). Shown as lines in the debug text, as a vertical line with a tick per crossing in the scene
// (heightProbeDraw, recorded through ovl::) and in the F12 shot sidecar (heightProbeReport).
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#include "input/collisionpick.hpp"
#include "kworld/heightmap.hpp"
#include "bridge/simview.hpp"
#include "render/overlaylist.hpp"
#include "world/clientnav.hpp"
#include "world/worldbuildings.hpp"

namespace dev
{
namespace
{
	enum Src { kDrawn, kCollision, kSkipped, kNav, kTerrain, kTerrainDrawn, kWire, kWindow, kPick, kSrcCount };
	struct SrcInfo { const char* name; float rgb[3]; };
	constexpr SrcInfo kSrc[kSrcCount] = {
		{ "drawn", { 0.3f, 0.9f, 1.0f } }, { "collision", { 1.0f, 0.6f, 0.2f } }, { "coll-skip", { 0.6f, 0.5f, 0.4f } }, { "navmesh", { 0.3f, 1.0f, 0.4f } },
		{ "terr-bilin", { 0.9f, 0.9f, 0.3f } }, { "terr-tri", { 1.0f, 0.4f, 0.3f } }, { "wire y", { 1.0f, 1.0f, 1.0f } }, { "window", { 0.7f, 0.7f, 1.0f } },
		{ "STAND", { 1.0f, 0.3f, 1.0f } } };

	struct Row { double y; Src src; std::string note; bool inWindow = false; int count = 1; };
	struct State
	{
		bool mode = false, active = false;
		double x = 0, z = 0;
		std::vector<Row> rows;
		std::string header;
		std::vector<std::string> lines;
		int sinceStatic = 1000;	// frames since the static rows (the drawn meshes may still be loading) were last gathered
		std::vector<Row> staticRows;
		double terrain = 0;
		bool collLoaded = true;
	};
	State g;

	std::string clip(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + "~"; }

	// the rows that do not change while the probe stays put: buildings (drawn, collision), the navmesh, the terrain
	void gatherStatic(const Context& cx)
	{
		g.staticRows.clear();
		const double x = g.x, z = g.z;
		g.terrain = cx.hm ? cx.hm->terrainHeight(x, z) : 0.0;	// what the stand ray uses since k16.2.382.1: the drawn lattice triangles
		const double lo = g.terrain - 40, hi = g.terrain + 100;
		// the terrain: the drawn lattice triangle (Heightmap::terrainHeight, what the stand ray stands on since kenshi-k16.2.382.1) and, as its own row, the
		// bilinear Heightmap::height (what it used before; the two differ by up to ~0.5 u on rough ground)
		g.staticRows.push_back({ g.terrain, kTerrainDrawn, "Heightmap::terrainHeight (drawn triangle; the stand ray's)" });
		if (cx.hm) { g.staticRows.push_back({ cx.hm->height(x, z), kTerrain, "Heightmap::height (bilinear, not the stand ray's)" }); }
		// drawn building / layout triangles: the nearest hit going down, again from just below it
		{
			const double o[3] = { x, hi, z }, dn[3] = { 0, -1, 0 }, blo[3] = { x - 1, lo, z - 1 }, bhi[3] = { x + 1, hi, z + 1 };
			double from = 0;
			for (int i = 0; i < 64; i++)
			{
				double t = 0;
				if (!cx.buildings.drawnHitIn(o, dn, blo, bhi, from, hi - lo, t)) { break; }
				g.staticRows.push_back({ hi - t, kDrawn, "building / layout triangle" });
				from = t + 0.005;
			}
		}
		// building collision triangles, flagged as groundY treats them
		std::vector<CollisionPick::ColumnHit> hits;
		g.collLoaded = cx.collide.column(x, z, lo, hi, hits) >= 0;
		for (const CollisionPick::ColumnHit& h : hits)
		{
			std::string flags = h.door ? "door " : h.thin ? "thin " : "";
			g.staticRows.push_back({ h.y, h.door || h.thin ? kSkipped : kCollision, flags + h.name });
		}
		// the navmesh polygons over (x, z) on every storey
		if (const knav::NavMesh* mesh = cx.nav.mesh())
		{
			static dtNavMeshQuery* q = dtAllocNavMeshQuery();
			static const dtNavMesh* inited = nullptr;
			if (inited != &mesh->detour()) { q->init(&mesh->detour(), 256); inited = &mesh->detour(); }
			dtQueryFilter filter;
			filter.setIncludeFlags(0xffff);
			filter.setExcludeFlags(0);
			const float c[3] = { (float)x, (float)((lo + hi) * 0.5), (float)z }, ext[3] = { 0.02f, (float)((hi - lo) * 0.5), 0.02f };
			dtPolyRef polys[64];
			int n = 0;
			q->queryPolygons(c, ext, &filter, polys, &n, 64);
			for (int i = 0; i < n; i++)
			{
				float h = 0;
				const float pos[3] = { (float)x, 0, (float)z };
				if (dtStatusFailed(q->getPolyHeight(polys[i], pos, &h))) { continue; }
				const dtMeshTile* tile = nullptr;
				const dtPoly* poly = nullptr;
				mesh->detour().getTileAndPolyByRef(polys[i], &tile, &poly);
				char note[96] = "";
				if (tile && poly) { snprintf(note, sizeof note, "tile layer %d, area %d flags %#x", tile->header->layer, (int)poly->getArea(), (unsigned)poly->flags); }
				if (h >= lo && h <= hi) { g.staticRows.push_back({ h, kNav, note }); }
			}
		}
		else { g.staticRows.push_back({ g.terrain, kNav, "(no navmesh streamed)" }); }
	}

	void rebuild(const Context& cx)
	{
		g.rows = g.staticRows;
		const double x = g.x, z = g.z;
		// a character within 1 u of x, z: its wire y, the stand ray's window and what the ray makes of it
		std::vector<std::string> standLines;
		for (const uint64_t id : cx.sim.characterIds())
		{
			double p[3];
			if (!cx.sim.simPos(id, p) || std::hypot(p[0] - x, p[2] - z) > 1.0) { continue; }
			double wy = p[1];	// simPos's y is the drawn y (after the ray); the ray's input is the wire y
			cx.sim.wireY(id, wy);
			const double top = wy + SimView::kRayUp, bottom = wy - SimView::kRayDown;
			const double terrain = cx.hm ? cx.hm->terrainHeight(p[0], p[2]) : 0.0;	// update()'s ground argument
			double ry = 0;
			const char* kind = "";
			const int res = cx.sim.standRayAt(p[0], p[2], wy, terrain, ry, &kind);
			char note[128];
			snprintf(note, sizeof note, "character %llu at %.1f %.1f", (unsigned long long)id, p[0], p[2]);
			g.rows.push_back({ wy, kWire, note });
			if (std::fabs(p[1] - wy) > 0.005) { g.rows.push_back({ p[1], kWire, "  its drawn y this frame (the ray's result)" }); }
			g.rows.push_back({ top, kWindow, "stand ray window top (wire + kRayUp)" });
			g.rows.push_back({ bottom, kWindow, "stand ray window bottom (wire - kRayDown)" });
			if (res >= 1) { snprintf(note, sizeof note, "stand ray -> %s (%+.2f from the wire)", kind, ry - wy); g.rows.push_back({ ry, kPick, note }); }
			else { snprintf(note, sizeof note, "stand ray -> MISS (%s): drawn at the wire y", kind); g.rows.push_back({ wy, kPick, note }); }
		}
		// the window marks: every row inside [bottom, top] of the (first) character's window
		double wTop = 0, wBottom = 0;
		bool haveWindow = false;
		for (const Row& r : g.rows) { if (r.src == kWindow) { if (!haveWindow) { wTop = wBottom = r.y; haveWindow = true; } wTop = std::max(wTop, r.y); wBottom = std::min(wBottom, r.y); } }
		for (Row& r : g.rows) { r.inWindow = haveWindow && r.src != kWindow && r.src != kWire && r.y <= wTop + 1e-6 && r.y >= wBottom - 1e-6; }
		std::stable_sort(g.rows.begin(), g.rows.end(), [](const Row& a, const Row& b) { return a.y > b.y; });
		// the same surface from many triangles (a plank floor): one row with a count
		std::vector<Row> merged;
		for (const Row& r : g.rows)
		{
			bool done = false;
			for (Row& m : merged) { if (m.src == r.src && std::fabs(m.y - r.y) < 0.005 && m.note == r.note) { m.count++; done = true; break; } }
			if (!done) { merged.push_back(r); }
		}
		g.rows = merged;
		char head[160];
		snprintf(head, sizeof head, "height probe at %.1f %.1f  terrain (triangle) %.2f%s", x, z, g.terrain, g.collLoaded ? "" : "  (collision not gathered here)");
		g.header = head;
		g.lines.clear();
		g.lines.push_back(g.header);
		g.lines.push_back("       y  source     note   (W = inside the stand ray window)");
		for (const Row& r : g.rows)
		{
			char line[200];
			snprintf(line, sizeof line, "%9.2f  %-10s %s%s%s", r.y, kSrc[r.src].name, r.inWindow ? "W " : "", clip(r.note, 70).c_str(), r.count > 1 ? (" x" + std::to_string(r.count)).c_str() : "");
			g.lines.push_back(line);
		}
	}
}

bool heightProbeMode() { return g.mode; }
void heightProbeToggle() { g.mode = !g.mode; printf("height probe mode %s\n", g.mode ? "on: click a point" : "off"); }

void heightProbeSet(const Context& cx, double x, double z, bool print)
{
	g.active = true;
	g.x = x;
	g.z = z;
	gatherStatic(cx);
	g.sinceStatic = 0;
	rebuild(cx);
	if (print) { for (const std::string& l : g.lines) { printf("probe: %s\n", l.c_str()); } }
}

void heightProbeFrame(const Context& cx)
{
	if (!g.active) { return; }
	if (++g.sinceStatic >= 30) { gatherStatic(cx); g.sinceStatic = 0; }	// the drawn meshes decode lazily: look again twice a second
	rebuild(cx);
}

std::vector<std::string> heightProbeLines() { return g.active ? g.lines : std::vector<std::string>(); }

std::string heightProbeReport()
{
	std::string s;
	if (!g.active) { return s; }
	for (const std::string& l : g.lines) { s += "probe: " + l + "\n"; }
	return s;
}

void heightProbeDraw(const float mvp[16], const double camPos[3], float width, float height)
{
	if (!g.active || g.rows.empty()) { return; }
	auto project = [&](double y, float& sx, float& sy)
	{
		const float v[3] = { (float)(g.x - camPos[0]), (float)(y - camPos[1]), (float)(g.z - camPos[2]) };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * v[0] + mvp[4 + i] * v[1] + mvp[8 + i] * v[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { return false; }
		sx = (c[0] / c[3] * 0.5f + 0.5f) * width;
		sy = (0.5f - c[1] / c[3] * 0.5f) * height;
		return true;
	};
	const double top = g.rows.front().y + 3, bottom = g.rows.back().y - 3;
	ovl::disable_texture();
	ovl::begin_lines();
	float ax, ay, bx, by;
	if (project(top, ax, ay) && project(bottom, bx, by)) { ovl::c4f(1, 1, 1, 0.8f); ovl::v2f(ax, ay); ovl::v2f(bx, by); }
	for (const Row& r : g.rows)
	{
		if (ovl::num_vertices() + 8 > 56000) { break; }
		if (!project(r.y, ax, ay)) { continue; }
		const float* c = kSrc[r.src].rgb;
		const float w = r.src == kPick ? 14.0f : r.src == kWindow ? 11.0f : 8.0f;
		ovl::c4f(c[0], c[1], c[2], 1.0f);
		ovl::v2f(ax - w, ay); ovl::v2f(ax + w, ay);
		ovl::v2f(ax, ay - 1); ovl::v2f(ax + w + (r.inWindow ? 5.0f : 0.0f), ay - 1);
	}
	ovl::count_lines();
	ovl::end();
}

bool heightProbeOp(const Context& cx, const std::vector<std::string>& a)
{
	if (a.size() == 2 && a[0] == "height" && a[1].starts_with("id="))	// probe height id=N: at character N's wire x, z
	{
		double p[3];
		if (!cx.sim.simPos(strtoull(a[1].c_str() + 3, nullptr, 10), p)) { fprintf(stderr, "kclient: script: probe height %s: no such drawn character\n", a[1].c_str()); return true; }
		heightProbeSet(cx, p[0], p[2], true);
		return true;
	}
	if (a.size() != 3 || a[0] != "height") { return false; }
	heightProbeSet(cx, atof(a[1].c_str()), atof(a[2].c_str()), true);
	return true;
}

}
