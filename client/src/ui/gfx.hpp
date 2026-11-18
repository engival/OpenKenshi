#pragma once
#include <functional>
#include <string>

#include "drawlist.hpp"

// Drawing primitives of the UI layer (docs/UI.md): one shared copy of the game's Kenshi_UI.png atlas, quads with alpha blending, scissor
// clipping. Coordinates here are framebuffer pixels, top-left origin. They record into a dl::List (drawlist.hpp, render hook 8): no sokol here;
// the render side replays the list (dl::gpu::replay) into the same sokol_gl calls the UI used to make itself.
namespace ui
{
	struct Rect
	{
		float x = 0, y = 0, w = 0, h = 0;

		float right() const { return x + w; }
		float bottom() const { return y + h; }
		bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
		Rect intersect(const Rect& o) const;
		Rect inset(float d) const { return { x + d, y + d, w - 2 * d, h - 2 * d }; }
	};

	struct Size { float w = 0, h = 0; };

	struct Color
	{
		float r = 1, g = 1, b = 1, a = 1;

		Color alpha(float k) const { return { r, g, b, a * k }; }
		bool operator==(const Color&) const = default;
		// "0.4 0.38 0.35" (MyGUI skins) or "#afa68b" / "afa68b" (colour files); false on anything else.
		static bool parse(const std::string& s, Color& out);
	};

	namespace gfx
	{
		constexpr float kAtlasW = 1920, kAtlasH = 1080;	// Kenshi_UI.png

		bool init();	// loads gfx/Kenshi_UI.png (res::file) and records its upload (uploads()); false (logged) when missing
		void shutdown();	// records the atlas's destroy; the render side frees it at the next apply (or dl::gpu::shutdown)
		dl::TexId atlas();	// drawn with the linear clamped sampler (dl::Filter::kLinear)

		// The list textures are recorded on: the one being recorded, else the pending list. Font pages, icons and the atlas are made
		// outside a record too (layout measures text, init loads the atlas); pending uploads move into the next list begin() opens.
		// The host applies the pending ones itself only before the first record (the atlas for the cursors: init_ui).
		dl::List& uploads();
		dl::List* recording();	// the list between begin and end, else null (draws outside a record are dropped)

		// Around one frame of UI draws (ui::record): list.begin, the pixel projection, the screen clip; end closes it.
		void begin(dl::List& list, int fbW, int fbH);
		// A host's own batch of UI draws (main.cpp's name labels, kui_demo's tags) records the same way, into its own list.
		void end();

		void fill(const Rect& r, const Color& c);
		void frame(const Rect& r, float thickness, const Color& c);	// outline inside r
		void gradient(const Rect& r, const Color& top, const Color& bottom);
		void gradientH(const Rect& r, const Color& left, const Color& right);
		void triangle(float x0, float y0, float x1, float y1, float x2, float y2, const Color& c);
		// r filled with corners of the given radius (clamped to half the shorter side), its edge smoothed over one pixel.
		void rounded(const Rect& r, float radius, const Color& c);
		// Part of the atlas (texels) stretched over r.
		void sprite(const Rect& r, const Rect& texels, const Color& c = {});
		// Part of the atlas over a w x h box centred on (cx, cy), turned by angle radians (clockwise on the screen).
		void spriteRotated(float cx, float cy, float w, float h, float angle, const Rect& texels, const Color& c = {});
		// The atlas rect repeated from r's top-left in tiles of tile.w x tile.h pixels; partial tiles at the edges are cut, not squeezed.
		void tiled(const Rect& r, const Rect& texels, float tileW, float tileH, const Color& c = {});
		// Any texture (font pages, icons, host refs) by its id: u/v in 0..1 (u0 > u1 mirrors); the id carries its sampler. 0 draws nothing.
		void textured(dl::TexId tex, const Rect& r, float u0, float v0, float u1, float v1, const Color& c);

		// Nested clip rectangles; each push intersects with the current one.
		void pushClip(const Rect& r);
		void popClip();
		const Rect& clip();

		// The host draws with sokol-gl inside r (pixels; the projection is the screen in pixels, y down), clipped to r and the current clip.
		// Pipeline, matrices and texture state are the UI's again afterwards, whatever the host left. draw is copied into the list and runs
		// on the render side at replay: it captures by value and reads only render-side state.
		void host(const Rect& r, std::function<void(float x, float y, float w, float h)> draw);
	}
}
