#include "world/worldmap.hpp"
#include "render/drawstats.hpp"
#include "render/drawlist_gpu.hpp"
#include "render/overlaylist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "assets/dds.hpp"
#include "sokol_app.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"
#include "sokol_log.h"
#include "platform/windowgate.hpp"

namespace
{

constexpr double kWorld = 294912.0;
constexpr double kHalf = kWorld / 2;
constexpr float kMiniSize = 256.0f;
constexpr float kMargin = 16.0f;
constexpr float kPi = 3.14159265f;
constexpr float kWedgeHalfAngle = 35.0f * kPi / 180.0f;

float to_px(double world, double origin, double span, float px0, float side) { return px0 + (float)((world - origin) / span) * side; }

}

void begin_overlay()
{
	static sgl_pipeline blendPip = {};	// alpha blending, no depth test; destroyed by sgl_shutdown
	if (blendPip.id == SG_INVALID_ID)
	{
		sg_pipeline_desc pd = {};
		pd.colors[0].blend.enabled = true;
		pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
		pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
		blendPip = sgl_make_pipeline(&pd);
	}
	sgl_layer(0);
	sdtx_layer(0);
	sdtx_canvas(view::widthf() / text_scale(), view::heightf() / text_scale());
	sdtx_origin(0, 0);
	sdtx_home();
	sgl_defaults();
	sgl_load_pipeline(blendPip);
	sgl_viewport(0, 0, view::width(), view::height(), true);
	sgl_matrix_mode_projection();
	sgl_ortho(0.0f, view::widthf(), view::heightf(), 0.0f, -1.0f, 1.0f);
}

void overlay_top()
{
	sgl_layer(1);
	sdtx_layer(1);
}

void end_overlay()
{
	sgl_draw_layer(0);
	sdtx_draw_layer(0);
	sgl_draw_layer(1);
	sdtx_draw_layer(1);
}

int text_scale() { return std::max(1, (int)std::lround(view::heightf() / 720.0f)); }

void draw_text(float px, float py, float r, float g, float b, const char* s)
{
	const float tile = 8.0f * text_scale();
	ovl::text_color3f(0.0f, 0.0f, 0.0f);
	ovl::text_pos(px / tile + 1.0f / tile, py / tile + 1.0f / tile);
	ovl::text_puts(s);
	ovl::text_color3f(r, g, b);
	ovl::text_pos(px / tile, py / tile);
	ovl::text_puts(s);
}

void draw_panel(float x, float y, float w, float h)
{
	ovl::disable_texture();
	ovl::begin_quads();
	ovl::c4f(0.0f, 0.0f, 0.0f, 0.55f);
	ovl::v2f(x, y); ovl::v2f(x + w, y); ovl::v2f(x + w, y + h); ovl::v2f(x, y + h);
	ovl::count_quads();
	ovl::end();
}

bool WorldMap::init(const std::string& path)
{
	DdsImage dds;
	std::string err;
	if (!dds_load(path, dds, err) || dds.format != SG_PIXELFORMAT_BC1_RGBA || dds.width != 8192 || dds.height != 8192) { return false; }
	img_ = dds_make_image(dds, "world-map");
	if (sg_query_image_state(img_) != SG_RESOURCESTATE_VALID) { return false; }
	sg_view_desc vd = {};
	vd.texture.image = img_;
	view_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.max_lod = 2;	// GUI_Map.dds's coarser BC1 mips wash the colours out (the overview's whole-world page sampled mip ~3.2, pale); a little aliasing instead
	smp_ = sg_make_sampler(&sd);
	tex_ = dl::newTex();	// the overlay lists draw the map by this id (overlaylist.hpp): logic records it, the render side binds the view
	dl::gpu::bind(tex_, view_, smp_);
	return true;
}

void WorldMap::shutdown()
{
	if (tex_) { dl::gpu::unbind(tex_); }
	sg_destroy_sampler(smp_);
	sg_destroy_view(view_);
	sg_destroy_image(img_);
}

WorldMap::Rect WorldMap::fullRect() const
{
	const float side = std::min(view::widthf(), view::heightf()) - 2 * kMargin;
	return { (view::widthf() - side) / 2, (view::heightf() - side) / 2, side };
}

WorldMap::Rect WorldMap::miniRect() const
{
	// top right under the HUD's speed panel (0.08 of the height): the bottom right is the jobs list's (the original has no minimap)
	return { view::widthf() - kMiniSize - kMargin, view::heightf() * 0.09f, kMiniSize };
}

WorldMap::Region WorldMap::region(double cx, double cz, double span) const
{
	span = std::min(span, kWorld);
	const double x0 = std::clamp(cx - span / 2, -kHalf, kHalf - span), z0 = std::clamp(cz - span / 2, -kHalf, kHalf - span);
	Region r;
	r.view = view_;
	r.sampler = smp_;
	r.uv[0] = (float)((x0 + kHalf) / kWorld);
	r.uv[1] = (float)((z0 + kHalf) / kWorld);
	r.uv[2] = r.uv[0] + (float)(span / kWorld);
	r.uv[3] = r.uv[1] + (float)(span / kWorld);
	r.u = (float)((cx - x0) / span);
	r.v = (float)((cz - z0) / span);
	return r;
}

bool WorldMap::fullToWorld(float sx, float sy, double& x, double& z) const
{
	const Rect r = fullRect();
	const double u = (sx - r.x) / r.side, v = (sy - r.y) / r.side;
	if (u < 0 || u >= 1 || v < 0 || v >= 1) { return false; }
	x = -kHalf + u * kWorld;
	z = -kHalf + v * kWorld;
	return true;
}

// Camera dot plus a translucent view wedge; yaw 0 looks toward -z, which is up on the screen.
void WorldMap::marker(float sx, float sy, double yaw, float wedgeLen) const
{
	ovl::disable_texture();
	const float dx = (float)std::sin(yaw), dy = (float)-std::cos(yaw);
	const float ax = dx * std::cos(kWedgeHalfAngle) - dy * std::sin(kWedgeHalfAngle), ay = dx * std::sin(kWedgeHalfAngle) + dy * std::cos(kWedgeHalfAngle);
	const float bx = dx * std::cos(kWedgeHalfAngle) + dy * std::sin(kWedgeHalfAngle), by = -dx * std::sin(kWedgeHalfAngle) + dy * std::cos(kWedgeHalfAngle);
	ovl::begin_triangles();
	ovl::c4f(1.0f, 0.9f, 0.2f, 0.35f);
	ovl::v2f(sx, sy); ovl::v2f(sx + ax * wedgeLen, sy + ay * wedgeLen); ovl::v2f(sx + bx * wedgeLen, sy + by * wedgeLen);
	ovl::count_triangles();
	ovl::end();
	ovl::begin_lines();
	ovl::c4f(1.0f, 0.9f, 0.2f, 1.0f);
	ovl::v2f(sx, sy); ovl::v2f(sx + ax * wedgeLen, sy + ay * wedgeLen);
	ovl::v2f(sx, sy); ovl::v2f(sx + bx * wedgeLen, sy + by * wedgeLen);
	ovl::count_lines();
	ovl::end();
	ovl::begin_quads();
	ovl::c3f(1.0f, 1.0f, 1.0f);
	ovl::v2f(sx - 4, sy - 4); ovl::v2f(sx + 4, sy - 4); ovl::v2f(sx + 4, sy + 4); ovl::v2f(sx - 4, sy + 4);
	ovl::c3f(0.9f, 0.1f, 0.1f);
	ovl::v2f(sx - 3, sy - 3); ovl::v2f(sx + 3, sy - 3); ovl::v2f(sx + 3, sy + 3); ovl::v2f(sx - 3, sy + 3);
	ovl::count_quads();
	ovl::end();
}

void WorldMap::drawMinimap(double cx, double cz, double yaw, const TownSet* towns, const Squads* squads) const
{
	const Rect r = miniRect();
	const double span = 4608.0 * (double)(1 << zoom);
	const double x0 = cx - span / 2, z0 = cz - span / 2;

	draw_panel(r.x, r.y, r.side, r.side);
	// The part of the window inside the world, as map uv and screen coordinates.
	const double wx0 = std::max(x0, -kHalf), wx1 = std::min(x0 + span, kHalf);
	const double wz0 = std::max(z0, -kHalf), wz1 = std::min(z0 + span, kHalf);
	if (wx1 > wx0 && wz1 > wz0)
	{
		const float sx0 = to_px(wx0, x0, span, r.x, r.side), sx1 = to_px(wx1, x0, span, r.x, r.side);
		const float sy0 = to_px(wz0, z0, span, r.y, r.side), sy1 = to_px(wz1, z0, span, r.y, r.side);
		const float u0 = (float)((wx0 + kHalf) / kWorld), u1 = (float)((wx1 + kHalf) / kWorld);
		const float v0 = (float)((wz0 + kHalf) / kWorld), v1 = (float)((wz1 + kHalf) / kWorld);
		ovl::enable_texture();
		ovl::texture(tex_);
		ovl::begin_quads();
		ovl::c3f(1.0f, 1.0f, 1.0f);
		ovl::v2f_t2f(sx0, sy0, u0, v0); ovl::v2f_t2f(sx1, sy0, u1, v0); ovl::v2f_t2f(sx1, sy1, u1, v1); ovl::v2f_t2f(sx0, sy1, u0, v1);
		ovl::count_quads();
		ovl::end();
	}
	ovl::disable_texture();
	if (towns)
	{
		ovl::begin_quads();
		ovl::c3f(1.0f, 0.5f, 0.0f);
		for (const Town& t : towns->towns)
		{
			if (t.x < x0 || t.x >= x0 + span || t.z < z0 || t.z >= z0 + span) { continue; }
			const float sx = to_px(t.x, x0, span, r.x, r.side), sy = to_px(t.z, z0, span, r.y, r.side);
			ovl::v2f(sx - 2, sy - 2); ovl::v2f(sx + 2, sy - 2); ovl::v2f(sx + 2, sy + 2); ovl::v2f(sx - 2, sy + 2);
		}
		ovl::count_quads();
		ovl::end();
	}
	if (squads && squads->on)
	{
		ovl::begin_quads();
		for (const Squads::Dot& d : squads->dots())
		{
			if (d.x < x0 || d.x >= x0 + span || d.z < z0 || d.z >= z0 + span) { continue; }
			const float sx = to_px(d.x, x0, span, r.x, r.side), sy = to_px(d.z, z0, span, r.y, r.side);
			ovl::c3f(d.r, d.g, d.b);
			ovl::v2f(sx - 1.5f, sy - 1.5f); ovl::v2f(sx + 1.5f, sy - 1.5f); ovl::v2f(sx + 1.5f, sy + 1.5f); ovl::v2f(sx - 1.5f, sy + 1.5f);
		}
		ovl::count_quads();
		ovl::end();
	}
	marker(to_px(cx, x0, span, r.x, r.side), to_px(cz, z0, span, r.y, r.side), yaw, r.side * 0.22f);
	ovl::begin_line_strip();
	ovl::c3f(1.0f, 1.0f, 1.0f);
	ovl::v2f(r.x, r.y); ovl::v2f(r.x + r.side, r.y); ovl::v2f(r.x + r.side, r.y + r.side); ovl::v2f(r.x, r.y + r.side); ovl::v2f(r.x, r.y);
	ovl::count_quads();
	ovl::end();
	char buf[48];
	snprintf(buf, sizeof buf, "N  %.1f km across", span / 10000.0);
	draw_text(r.x + 4, r.y + 4, 1.0f, 1.0f, 1.0f, buf);
}

// The map square r showing the world square (x0, z0) .. + span: texture, towns, squad dots, the camera marker, town labels.
void WorldMap::drawMap(Rect r, double x0, double z0, double span, double cx, double cz, double yaw, const TownSet* towns, const Squads* squads, bool clipLabels) const
{
	ovl::enable_texture();
	ovl::texture(tex_);
	ovl::begin_quads();
	ovl::c3f(1.0f, 1.0f, 1.0f);
	const float u0 = (float)((x0 + kHalf) / kWorld), v0 = (float)((z0 + kHalf) / kWorld), u1 = u0 + (float)(span / kWorld), v1 = v0 + (float)(span / kWorld);
	ovl::v2f_t2f(r.x, r.y, u0, v0); ovl::v2f_t2f(r.x + r.side, r.y, u1, v0); ovl::v2f_t2f(r.x + r.side, r.y + r.side, u1, v1); ovl::v2f_t2f(r.x, r.y + r.side, u0, v1);
	ovl::count_quads();
	ovl::end();
	ovl::disable_texture();

	if (towns)
	{
		ovl::begin_quads();
		ovl::c3f(1.0f, 0.5f, 0.0f);
		for (const Town& t : towns->towns)
		{
			if (t.x < x0 || t.x >= x0 + span || t.z < z0 || t.z >= z0 + span) { continue; }
			const float sx = to_px(t.x, x0, span, r.x, r.side), sy = to_px(t.z, z0, span, r.y, r.side);
			ovl::v2f(sx - 2, sy - 2); ovl::v2f(sx + 2, sy - 2); ovl::v2f(sx + 2, sy + 2); ovl::v2f(sx - 2, sy + 2);
		}
		ovl::count_quads();
		ovl::end();
	}
	if (squads && squads->on)
	{
		ovl::begin_quads();
		for (const Squads::Dot& d : squads->dots())
		{
			if (d.x < x0 || d.x >= x0 + span || d.z < z0 || d.z >= z0 + span) { continue; }
			const float sx = to_px(d.x, x0, span, r.x, r.side), sy = to_px(d.z, z0, span, r.y, r.side);
			ovl::c3f(d.r, d.g, d.b);
			ovl::v2f(sx - 1.5f, sy - 1.5f); ovl::v2f(sx + 1.5f, sy - 1.5f); ovl::v2f(sx + 1.5f, sy + 1.5f); ovl::v2f(sx - 1.5f, sy + 1.5f);
		}
		ovl::count_quads();
		ovl::end();
	}
	marker(to_px(cx, x0, span, r.x, r.side), to_px(cz, z0, span, r.y, r.side), yaw, r.side * 0.05f);

	if (towns)
	{
		// Labels to the right of the dot, skipping any that would overlap one already placed.
		const float cw = 8.0f * text_scale(), ch = 8.0f * text_scale();
		struct Box { float x0, y0, x1, y1; };
		std::vector<Box> placed;
		for (const Town& t : towns->towns)
		{
			const float sx = to_px(t.x, x0, span, r.x, r.side) + 5, sy = to_px(t.z, z0, span, r.y, r.side) - ch / 2;
			const Box b = { sx, sy, sx + cw * (float)t.name.size(), sy + ch };
			bool hit = false;
			for (const Box& p : placed) { if (b.x0 < p.x1 && b.x1 > p.x0 && b.y0 < p.y1 && b.y1 > p.y0) { hit = true; break; } }
			// clipLabels: only labels inside the square (debug text is not scissored and draws over the UI)
			if (hit || (clipLabels && (b.x0 < r.x || b.x1 > r.x + r.side || b.y0 < r.y || b.y1 > r.y + r.side))) { continue; }
			placed.push_back(b);
			draw_text(sx, sy, 1.0f, 0.95f, 0.7f, t.name.c_str());
		}
	}
}

void WorldMap::drawFull(double cx, double cz, double yaw, const TownSet* towns, const Squads* squads, const std::string& closeKey) const
{
	const Rect r = fullRect();
	ovl::disable_texture();
	ovl::begin_quads();
	ovl::c3f(0.05f, 0.05f, 0.07f);
	ovl::v2f(0, 0); ovl::v2f(view::widthf(), 0); ovl::v2f(view::widthf(), view::heightf()); ovl::v2f(0, view::heightf());
	ovl::count_quads();
	ovl::end();
	drawMap(r, -kHalf, -kHalf, kWorld, cx, cz, yaw, towns, squads, false);
	draw_text(r.x + 8, r.y + r.side - 8.0f * text_scale() - 8, 1.0f, 1.0f, 1.0f, (closeKey + ": close   click: teleport").c_str());
}

void WorldMap::drawArea(float x, float y, float w, float h, double cx, double cz, double pxPerUnit) const
{
	sgl_disable_texture();
	sgl_begin_quads();
	sgl_c3f(0.05f, 0.05f, 0.07f);
	sgl_v2f(x, y); sgl_v2f(x + w, y); sgl_v2f(x + w, y + h); sgl_v2f(x, y + h);
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();
	// the whole map square; the caller clips to the rect
	const float side = (float)(kWorld * pxPerUnit);
	const float mx = (float)(x + w / 2 - (kHalf + cx) * pxPerUnit), my = (float)(y + h / 2 - (kHalf + cz) * pxPerUnit);
	sgl_enable_texture();
	sgl_texture(view_, smp_);
	sgl_begin_quads();
	sgl_c3f(1.0f, 1.0f, 1.0f);
	sgl_v2f_t2f(mx, my, 0, 0); sgl_v2f_t2f(mx + side, my, 1, 0); sgl_v2f_t2f(mx + side, my + side, 1, 1); sgl_v2f_t2f(mx, my + side, 0, 1);
	DRAWSTATS_QUADS(sgl_num_vertices());
	sgl_end();
	sgl_disable_texture();
}
