// debug.pick_mask: see pickmask.hpp.
#include "dev/pickmask.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "app/config.hpp"
#include "ui/gfx.hpp"
#include "platform/windowgate.hpp"

namespace dev::pickmask
{
namespace
{
const cfg::Bool kPickMask{ cfg::Group{ "Pick mask" }, "debug.pick_mask", false, "Debug: draws the hovered object's whole pick area (every 4 px cell where a click picks it) as a flat translucent cyan mask, so pick coverage and leaks show. Runs the real pick per cell (dev builds)" };

constexpr int kCell = 4;	// px
constexpr float kMargin = 32;	// px round the thing's drawn box, so a leak past the drawn model shows
constexpr double kSliceMs = 2.0;
constexpr float kFallback = 160;	// px: the square about the cursor for a thing with no drawn box

// The pass refines 16 -> 8 -> 4 px: a cell is picked (the real pick at its centre) only when the parent cell or one of the parent's 8 neighbours
// differs from it, so the flat inside and outside cost nothing and the edge, where a leak lives, gets the 4 px cells. Each level shows as it finishes.
constexpr int kLevels = 3;	// factors 4, 2, 1 of kCell

struct State
{
	uint64_t id = 0, stamp = 0;
	bool working = false;
	int x0 = 0, y0 = 0, nx = 0, ny = 0;	// the grid's origin (px) and size in kCell cells
	int level = 0, next = 0;	// the level being filled, its next cell
	std::vector<int8_t> val[kLevels];	// per level: 1 a click there picks id, 0 not
	std::vector<uint8_t> shown;	// the finest finished level, at kCell
	int shownX0 = 0, shownY0 = 0, shownNx = 0, shownNy = 0;
	double ms = 0;
	int frames = 0;
	unsigned picks = 0;
};
State g;

int factor(int lv) { return 1 << (kLevels - 1 - lv); }
int cellsX(int lv) { return (g.nx + factor(lv) - 1) / factor(lv); }
int cellsY(int lv) { return (g.ny + factor(lv) - 1) / factor(lv); }
}

bool on() { return kPickMask.get(); }

void update(const Host& host, uint64_t hovered, double mx, double my)
{
	if (!on() || !hovered)
	{
		g.id = 0;
		g.working = false;
		g.shown.clear();
		g.shownNx = g.shownNy = 0;
		return;
	}
	const uint64_t stamp = host.cameraStamp();
	if (hovered != g.id || stamp != g.stamp)
	{
		g.id = hovered;
		g.stamp = stamp;
		float b[4];
		if (!host.screenBox(hovered, b)) { b[0] = (float)mx - kFallback; b[1] = (float)my - kFallback; b[2] = (float)mx + kFallback; b[3] = (float)my + kFallback; }
		const float w = view::widthf(), h = view::heightf();
		const int x0 = (int)std::floor(std::max(0.0f, b[0] - kMargin) / kCell), y0 = (int)std::floor(std::max(0.0f, b[1] - kMargin) / kCell);
		const int x1 = (int)std::ceil(std::min(w, b[2] + kMargin) / kCell), y1 = (int)std::ceil(std::min(h, b[3] + kMargin) / kCell);
		g.x0 = x0 * kCell;
		g.y0 = y0 * kCell;
		g.nx = std::max(0, x1 - x0);
		g.ny = std::max(0, y1 - y0);
		for (int lv = 0; lv < kLevels; lv++) { g.val[lv].assign((size_t)cellsX(lv) * cellsY(lv), 0); }
		g.level = g.next = 0;
		g.ms = 0;
		g.frames = 0;
		g.picks = 0;
		g.working = g.nx > 0 && g.ny > 0;
	}
	if (!g.working) { return; }
	const auto t0 = std::chrono::steady_clock::now();
	const auto since = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
	while (g.level < kLevels)
	{
		const int lv = g.level, f = factor(lv), cx = cellsX(lv), cy = cellsY(lv), total = cx * cy;
		while (g.next < total)
		{
			const int ix = g.next % cx, iy = g.next / cx;
			bool pick = true;
			int8_t v = 0;
			if (lv > 0)	// the parent and its 8 neighbours agree: this cell is the same
			{
				const int pcx = cellsX(lv - 1), pcy = cellsY(lv - 1), px = ix / 2, py = iy / 2;
				v = g.val[lv - 1][(size_t)py * pcx + px];
				pick = false;
				for (int dy = -1; dy <= 1 && !pick; dy++)
				{
					for (int dx = -1; dx <= 1; dx++)
					{
						const int qx = px + dx, qy = py + dy;
						if (qx >= 0 && qy >= 0 && qx < pcx && qy < pcy && g.val[lv - 1][(size_t)qy * pcx + qx] != v) { pick = true; break; }
					}
				}
			}
			if (pick)
			{
				const double sx = g.x0 + (ix * f + f * 0.5) * kCell, sy = g.y0 + (iy * f + f * 0.5) * kCell;
				v = host.pickId(sx, sy) == g.id ? 1 : 0;
				g.picks++;
			}
			g.val[lv][g.next++] = v;
			if (since() >= kSliceMs) { break; }
		}
		if (g.next < total) { break; }
		g.shown.assign((size_t)g.nx * g.ny, 0);	// this level, spread to kCell cells
		for (int y = 0; y < g.ny; y++) { for (int x = 0; x < g.nx; x++) { g.shown[(size_t)y * g.nx + x] = (uint8_t)g.val[lv][(size_t)(y / f) * cx + x / f]; } }
		g.shownX0 = g.x0;
		g.shownY0 = g.y0;
		g.shownNx = g.nx;
		g.shownNy = g.ny;
		printf("pickmask thing %llu: level %d (%d px) done, %u real picks so far, %.1f ms over %d frame(s)\n", (unsigned long long)g.id, lv, f * kCell, g.picks, g.ms + since(), g.frames + 1);
		g.level++;
		g.next = 0;
	}
	g.ms += since();
	g.frames++;
	if (g.level >= kLevels)
	{
		g.working = false;
		printf("pickmask thing %llu: %d x %d cells of %d px, %u real picks, %.2f ms over %d frame(s) (slices of %.0f ms)\n", (unsigned long long)g.id, g.nx, g.ny, kCell, g.picks, g.ms, g.frames, kSliceMs);
		fflush(stdout);
	}
}

void draw()
{
	if (!on() || g.shown.empty()) { return; }
	const ui::Color c{ 0.0f, 0.9f, 1.0f, 0.4f };
	for (int y = 0; y < g.shownNy; y++)	// a run of picked cells along a row is one rectangle
	{
		for (int x = 0; x < g.shownNx;)
		{
			if (!g.shown[(size_t)y * g.shownNx + x]) { x++; continue; }
			int e = x;
			while (e < g.shownNx && g.shown[(size_t)y * g.shownNx + e]) { e++; }
			ui::gfx::fill(ui::Rect{ (float)(g.shownX0 + x * kCell), (float)(g.shownY0 + y * kCell), (float)((e - x) * kCell), (float)kCell }, c);
			x = e;
		}
	}
}
}
