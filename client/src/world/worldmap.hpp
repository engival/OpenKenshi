#pragma once
#include <string>

#include "render/drawlist.hpp"
#include "sokol_gfx.h"
#include "world/squads.hpp"
#include "world/towns.hpp"

// Text via sokol_debugtext on a canvas of framebuffer size / scale (scale 1 at 720p, 2 at 1440p). draw_text, draw_panel and the map's draws go
// through ovl:: (overlaylist.hpp): into the overlay list bound on the calling thread (logic_frame's record_overlay), else to sokol at once.
// begin_overlay() must run once per frame before any overlay call; end_overlay() flushes sokol_gl and
// debugtext inside the current pass. Two layers: everything before overlay_top() goes under, so the debug text there
// stays below the UI windows drawn after it.
void begin_overlay();
void overlay_top();
void end_overlay();
int text_scale();
void draw_text(float px, float py, float r, float g, float b, const char* s);	// framebuffer pixels, with shadow
void draw_panel(float x, float y, float w, float h);					// translucent dark rect

// The game's own world map (GUI_Map.dds, BC1 + mips uploaded as is): full-screen map and the overview's MAP page image (the corner minimap is ui::Minimap's window).
class WorldMap
{
public:

	// Returns false when the DDS is missing or not an 8192^2 BC1 image.
	bool init(const std::string& path);
	void shutdown();

	bool full = false;	// full-screen map shown

	// Draw into the overlay (recorded on logic into the packet's overlay list, overlaylist.hpp). towns may be null while loading.
	void drawFull(double cx, double cz, double yaw, const TownSet* towns, const Squads* squads, const std::string& closeKey) const;	// closeKey: for the hint line

	// The map in a rectangle (the overview's MAP page, whose view the UI keeps): world (cx, cz) at the rect's centre, pxPerUnit pixels a world
	// unit, north up; dark outside the world. The page draws the markers itself.
	void drawArea(float x, float y, float w, float h, double cx, double cz, double pxPerUnit) const;

	// The map texture's square of span world units around (cx, cz), moved inside the world: its view, sampler and uv (u0, v0, u1, v1), and
	// (cx, cz) in it as u, v (0..1). For windows that draw the map themselves (the prospecting window).
	struct Region { sg_view view = {}; sg_sampler sampler = {}; float uv[4] = { 0, 0, 1, 1 }; float u = 0, v = 0; };
	Region region(double cx, double cz, double span) const;

	// Full-screen map pixel -> world x,z; false when outside the map square.
	bool fullToWorld(float sx, float sy, double& x, double& z) const;

private:
	struct Rect { float x, y, side; };
	Rect fullRect() const;
	void marker(float sx, float sy, double yaw, float wedgeLen) const;
	void drawMap(Rect r, double x0, double z0, double span, double cx, double cz, double yaw, const TownSet* towns, const Squads* squads, bool clipLabels) const;

	sg_image img_ = {};
	sg_view view_ = {};
	sg_sampler smp_ = {};
	dl::TexId tex_ = 0;	// view_ / smp_ bound for the overlay lists (init, render side)
};
