// The overlay call list (overlaylist.hpp; render sitting 11, kenshi-k16.2.205.12.57).
#include "overlaylist.hpp"

#include "drawlist_gpu.hpp"
#include "drawstats.hpp"

#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_debugtext.h"

#include <cstring>

namespace ovl
{
	namespace
	{
		thread_local List* bound = nullptr;

		// an id nothing bound gives the invalid handles, as the old call with a map that failed to load
		void tex(dl::TexId t) { sgl_texture(dl::gpu::view(t), dl::gpu::sampler(t)); }
	}

	void List::pushText(const char* s)
	{
		const size_t n = std::strlen(s);
		calls_.push_back({ Op::kTextPuts, (uint32_t)text_.size(), (uint32_t)n, {} });
		text_.append(s, n);
	}

	Record::Record(List& list) : prev_(bound) { bound = &list; }
	Record::~Record() { bound = prev_; }
	bool recording() { return bound != nullptr; }

	void disable_texture() { if (bound) { bound->push(Op::kDisableTexture); } else { sgl_disable_texture(); } }
	void enable_texture() { if (bound) { bound->push(Op::kEnableTexture); } else { sgl_enable_texture(); } }
	void texture(dl::TexId t) { if (bound) { bound->pushTex(t); } else { tex(t); } }
	void begin_lines() { if (bound) { bound->push(Op::kBeginLines); } else { sgl_begin_lines(); } }
	void begin_line_strip() { if (bound) { bound->push(Op::kBeginLineStrip); } else { sgl_begin_line_strip(); } }
	void begin_triangles() { if (bound) { bound->push(Op::kBeginTriangles); } else { sgl_begin_triangles(); } }
	void begin_quads() { if (bound) { bound->push(Op::kBeginQuads); } else { sgl_begin_quads(); } }
	void end() { if (bound) { bound->push(Op::kEnd); } else { sgl_end(); } }
	void c3f(float r, float g, float b) { if (bound) { bound->push(Op::kC3f, r, g, b); } else { sgl_c3f(r, g, b); } }
	void c4f(float r, float g, float b, float a) { if (bound) { bound->push(Op::kC4f, r, g, b, a); } else { sgl_c4f(r, g, b, a); } }
	void v2f(float x, float y) { if (bound) { bound->push(Op::kV2f, x, y); bound->vertex(); } else { sgl_v2f(x, y); } }
	void v2f_t2f(float x, float y, float u, float v) { if (bound) { bound->push(Op::kV2fT2f, x, y, u, v); bound->vertex(); } else { sgl_v2f_t2f(x, y, u, v); } }
	int num_vertices() { return bound ? bound->vertices() : sgl_num_vertices(); }
	void count_quads() { if (bound) { bound->push(Op::kCountQuads); } else { DRAWSTATS_QUADS(sgl_num_vertices()); } }
	void count_triangles() { if (bound) { bound->push(Op::kCountTriangles); } else { DRAWSTATS_TRIANGLES(sgl_num_vertices()); } }
	void count_lines() { if (bound) { bound->push(Op::kCountLines); } else { DRAWSTATS_LINES(sgl_num_vertices()); } }
	void text_color3f(float r, float g, float b) { if (bound) { bound->push(Op::kTextColor3f, r, g, b); } else { sdtx_color3f(r, g, b); } }
	void text_pos(float x, float y) { if (bound) { bound->push(Op::kTextPos, x, y); } else { sdtx_pos(x, y); } }
	void text_puts(const char* s) { if (bound) { bound->pushText(s); } else { sdtx_puts(s); } }

	void play(const List& list)
	{
		const std::string& text = list.text();
		std::string s;
		for (const Call& c : list.calls())
		{
			switch (c.op)
			{
			case Op::kDisableTexture: sgl_disable_texture(); break;
			case Op::kEnableTexture: sgl_enable_texture(); break;
			case Op::kTexture: tex(c.a); break;
			case Op::kBeginLines: sgl_begin_lines(); break;
			case Op::kBeginLineStrip: sgl_begin_line_strip(); break;
			case Op::kBeginTriangles: sgl_begin_triangles(); break;
			case Op::kBeginQuads: sgl_begin_quads(); break;
			case Op::kEnd: sgl_end(); break;
			case Op::kC3f: sgl_c3f(c.f[0], c.f[1], c.f[2]); break;
			case Op::kC4f: sgl_c4f(c.f[0], c.f[1], c.f[2], c.f[3]); break;
			case Op::kV2f: sgl_v2f(c.f[0], c.f[1]); break;
			case Op::kV2fT2f: sgl_v2f_t2f(c.f[0], c.f[1], c.f[2], c.f[3]); break;
			case Op::kCountQuads: DRAWSTATS_QUADS(sgl_num_vertices()); break;
			case Op::kCountTriangles: DRAWSTATS_TRIANGLES(sgl_num_vertices()); break;
			case Op::kCountLines: DRAWSTATS_LINES(sgl_num_vertices()); break;
			case Op::kTextColor3f: sdtx_color3f(c.f[0], c.f[1], c.f[2]); break;
			case Op::kTextPos: sdtx_pos(c.f[0], c.f[1]); break;
			case Op::kTextPuts: s.assign(text, c.a, c.b); sdtx_puts(s.c_str()); break;
			}
		}
	}
}
