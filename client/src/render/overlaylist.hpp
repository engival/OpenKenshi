#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "render/drawlist.hpp"

// The overlay call list (render sitting 11, kenshi-k16.2.205.12.57; docs/RENDER_THREAD.md 7): the world overlay's debug views, sim markers and map / HUD
// draw with sokol_gl and sokol_debugtext from logic-owned state (the nav mesh, the sim's markers, the map's towns and squads, the HUD's numbers). They
// run on the logic thread at the end of logic_frame and record here instead; the render side plays the list back in the overlay pass, the same calls in
// the same order (ovl::play), so the pictures are bit for bit the old ones.
//
// The drawing code calls the ovl:: functions below in place of sgl_* / sdtx_*. They go to the list bound on the calling thread (ovl::Record, a
// thread-local binding), else straight to sokol: the paths not moved yet (the early paths, the UI's host draws) call the same helpers (draw_text,
// draw_panel) on the render side and draw at once, as before.
//
// Unlike dl::List (kui's batches, replayed with their own pipeline, matrices and scissor), this list is the raw call stream inside the overlay's own
// state (begin_overlay's pipeline, viewport, ortho and debugtext canvas): it sets none of that. The 3D markers (the nav mesh, rings, routes, trails, the
// probes) are projected to pixels while recording, with the packet's camera (FramePacket::views[0] at near 1, far 1000, as build_mvp gave the old
// draws): the list carries no camera, its vertices are the frame's view. A stereo frame records one list per view.
//
// Textures go by dl::TexId (bound on the render side, dl::gpu::bind): the list never names a sokol handle. The drawstats counts (DRAWSTATS_*) are ops
// too: they read sgl_num_vertices() at play, in whatever context is current, as the old sites did. This header has no sokol in it.
namespace ovl
{
	enum class Op : uint8_t
	{
		kDisableTexture, kEnableTexture, kTexture,		// kTexture: a = the TexId
		kBeginLines, kBeginLineStrip, kBeginTriangles, kBeginQuads, kEnd,
		kC3f, kC4f, kV2f, kV2fT2f,				// f[] the arguments
		kCountQuads, kCountTriangles, kCountLines,		// DRAWSTATS_*(sgl_num_vertices()) at play
		kTextColor3f, kTextPos, kTextPuts,			// sdtx_color3f / sdtx_pos / sdtx_puts (a = offset into the text, b = its length)
	};

	struct Call
	{
		Op op;
		uint32_t a = 0, b = 0;
		float f[4] = {};
	};

	class List
	{
	public:
		void clear() { calls_.clear(); text_.clear(); vertices_ = 0; }
		bool empty() const { return calls_.empty(); }
		void push(Op op, float x = 0, float y = 0, float z = 0, float w = 0) { calls_.push_back({ op, 0, 0, { x, y, z, w } }); }
		void pushTex(dl::TexId t) { calls_.push_back({ Op::kTexture, t, 0, {} }); }
		void pushText(const char* s);
		void vertex() { vertices_++; }
		int vertices() const { return vertices_; }	// the vertices recorded (ovl::num_vertices while recording)
		const std::vector<Call>& calls() const { return calls_; }
		const std::string& text() const { return text_; }

	private:
		std::vector<Call> calls_;
		std::string text_;
		int vertices_ = 0;
	};

	// Binds list to the calling thread for the scope: every ovl:: call below records into it (the previous binding comes back at the end).
	class Record
	{
	public:
		explicit Record(List& list);
		~Record();
		Record(const Record&) = delete;
		Record& operator=(const Record&) = delete;

	private:
		List* prev_;
	};
	bool recording();	// a list is bound on this thread

	// sgl_* and sdtx_* by their names (recorded, or issued at once)
	void disable_texture();
	void enable_texture();
	void texture(dl::TexId t);	// the view and sampler dl::gpu bound to t (the immediate path too)
	void begin_lines();
	void begin_line_strip();
	void begin_triangles();
	void begin_quads();
	void end();
	void c3f(float r, float g, float b);
	void c4f(float r, float g, float b, float a);
	void v2f(float x, float y);
	void v2f_t2f(float x, float y, float u, float v);
	// sgl_num_vertices(): recording, the vertices recorded into the bound list (its budget checks; the overlay's lists start the default context's
	// frame, so the count is the one sgl had); else sokol's
	int num_vertices();
	void count_quads();	// DRAWSTATS_QUADS(sgl_num_vertices())
	void count_triangles();
	void count_lines();
	void text_color3f(float r, float g, float b);
	void text_pos(float x, float y);
	void text_puts(const char* s);

	// Render side, inside the overlay pass: the list's calls, in order.
	void play(const List& list);
}
