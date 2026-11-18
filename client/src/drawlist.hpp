#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

// The 2D draw list (render hook 8; docs/RENDER_THREAD.md "UI (kui) draw", design review kenshi-k16.2.205.12.15 §4, kenshi-k16.2.205.12.28).
//
// The UI (kui) walks its widget tree on the logic thread; the render thread must not read that tree. So the UI's gfx backend records
// what it would have sent to sokol_gl into a List: vertices, and per sgl_begin/sgl_end group one Cmd with its scissor, texture,
// pipeline and layer. The list goes into the frame packet by value and the render side replays it into sokol_gl (dl::gpu::replay,
// drawlist_gpu.hpp), which issues the same sgl calls in the same order, so the pictures are bit for bit today's.
//
// Textures cross as a UI-side id (TexId), never as a sokol handle: logic never names an sg_view. Pictures the UI makes itself (glyph
// pages, icons) are rasterised on the CPU and travel as Uploads ahead of the draws: create with the pixels, update a dirty rect,
// destroy. The host applies a packet's uploads once (a resource job, renderqueue.hpp) before the packet is drawn; a destroy takes effect
// at the next packet's uploads, so the packet that last drew with a texture can still be drawn. Textures the host owns (the portrait
// atlas, the prospecting map, menu art) are bound to an id on the render side (dl::gpu::bind).
//
// This header has no sokol in it: the recorder runs on the logic thread, where an sg_* or sgl_* call aborts (dev/sgowner.cpp).
namespace dl
{
	using TexId = uint32_t;	// 0: untextured (sgl_disable_texture)
	// A fresh id, any thread. Ids are never reused: a re-baked page gets a new one.
	inline TexId newTex()
	{
		static std::atomic<uint32_t> next{ 1 };
		return next.fetch_add(1, std::memory_order_relaxed);
	}

	enum class Prim : uint8_t { kQuads, kTriangles };
	// The render side's pipelines by id (dl::gpu makes them): kUi is kui's alpha blend (gfx.cpp's "ui-blend").
	enum class Pipe : uint8_t { kUi };
	// The sampler a texture the list creates is drawn with: kLinear clamped (gfx's "ui-linear"), kTrilinear clamped (icons' "ui-picture").
	enum class Filter : uint8_t { kLinear, kTrilinear };
	constexpr uint8_t kLayerInherit = 0xFF;	// draw in whatever sgl layer is current (kui never sets one)

	// sgl's own colour packing (sokol_gl.h _sgl_pack_rgbaf), so a recorded colour is the byte sgl_c4f would have stored.
	inline uint32_t pack(float r, float g, float b, float a)
	{
		const auto c = [](float v) { return (uint32_t)(uint8_t)((v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v) * 255.0f); };
		return (c(a) << 24) | (c(b) << 16) | (c(g) << 8) | c(r);
	}

	struct Rect { float x = 0, y = 0, w = 0, h = 0; };
	struct Vertex { float x = 0, y = 0, u = 0, v = 0; uint32_t rgba = 0xFFFFFFFF; };

	struct Cmd
	{
		enum class Kind : uint8_t { kDraw, kHost } kind = Kind::kDraw;
		Prim prim = Prim::kQuads;
		Pipe pipe = Pipe::kUi;
		uint8_t layer = kLayerInherit;
		TexId tex = 0;
		Rect clip;		// the scissor (pixels, top-left origin) in force for this group
		uint32_t first = 0, count = 0;	// kDraw: the group's vertices in List::vertices()
		uint32_t host = 0;	// kHost: index into List::hosts()
	};

	// The host draws with sokol_gl inside r on the render side (gfx::host): it must read only render-side state or what it captured by value.
	struct Host
	{
		Rect r;
		std::function<void(float x, float y, float w, float h)> draw;
	};

	struct Upload
	{
		enum class Op : uint8_t { kCreate, kUpdate, kDestroy } op = Op::kCreate;
		TexId id = 0;
		int x = 0, y = 0, w = 0, h = 0;	// kCreate: 0, 0 and the size; kUpdate: the dirty rect in texels
		int mips = 1;		// kCreate: levels in rgba, largest first (each half the last, as icons.cpp builds them)
		Filter filter = Filter::kLinear;
		bool updatable = false;	// kCreate: kUpdate may follow (the render side keeps a CPU copy of level 0)
		const char* label = "ui";	// a static string
		std::vector<uint8_t> rgba;	// RGBA8, tightly packed: kCreate every level, kUpdate the rect
	};

	class List
	{
	public:
		// A new frame of draws in a w x h framebuffer (gfx::begin): the draws, hosts and state reset; uploads not yet taken stay.
		void begin(int w, int h);
		int width() const { return w_; }
		int height() const { return h_; }
		bool empty() const { return cmds_.empty(); }

		// State for the next begin_prim (as sgl's current state).
		void clip(const Rect& r) { clip_ = r; }
		const Rect& clip() const { return clip_; }
		void texture(TexId t) { tex_ = t; }	// 0: untextured
		void pipeline(Pipe p) { pipe_ = p; }
		void layer(uint8_t l) { layer_ = l; }

		// One sgl_begin_* .. sgl_end group.
		void beginPrim(Prim p);
		void vertex(float x, float y, float u, float v, uint32_t rgba) { verts_.push_back({ x, y, u, v, rgba }); }
		void endPrim();

		void host(const Rect& r, std::function<void(float x, float y, float w, float h)> draw);

		// Textures the list makes (logic side); they apply before this list's draws.
		void create(TexId id, int w, int h, std::vector<uint8_t> rgba, int mips = 1, Filter f = Filter::kLinear, bool updatable = false, const char* label = "ui");
		void update(TexId id, int x, int y, int w, int h, std::vector<uint8_t> rgba);
		void destroy(TexId id);
		// The uploads recorded since the last call, in order (the host hands them to a resource job).
		std::vector<Upload> takeUploads();
		const std::vector<Upload>& uploads() const { return ups_; }

		const std::vector<Cmd>& cmds() const { return cmds_; }
		const std::vector<Vertex>& vertices() const { return verts_; }
		const std::vector<Host>& hosts() const { return hosts_; }

	private:
		int w_ = 0, h_ = 0;
		Rect clip_;
		TexId tex_ = 0;
		Pipe pipe_ = Pipe::kUi;
		uint8_t layer_ = kLayerInherit;
		bool open_ = false;
		std::vector<Cmd> cmds_;
		std::vector<Vertex> verts_;
		std::vector<Host> hosts_;
		std::vector<Upload> ups_;
	};
}
