#include "render/roadribbons.hpp"
#include "render/drawstats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "roadribbon.glsl.h"
#include "sokol_gfx.h"

namespace roadribbons
{
namespace
{
constexpr float kMinHalfPx = 1.5f, kThinHalfPx = 0.5f, kDash = 40.0f;
constexpr float kGain = 1.0f;	// the display colour of the old overlay through the scene's exposure and tone map

struct Gpu
{
	sg_shader shd = {};
	sg_pipeline vis = {}, hid = {};
	sg_buffer vb = {}, ib = {};
	std::shared_ptr<const Mesh> held;	// the uploaded mesh, kept so its address is never reused by another
	int indexCount = 0;
	bool ready = false;
};
Gpu g;

sg_pipeline make(sg_compare_func cmp, const char* label)
{
	sg_pipeline_desc pd = {};
	pd.shader = g.shd;
	pd.layout.attrs[ATTR_roadribbon_pos] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_roadribbon_side] = { .buffer_index = 0, .offset = 12, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_roadribbon_col] = { .buffer_index = 0, .offset = 24, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_roadribbon_aux] = { .buffer_index = 0, .offset = 40, .format = SG_VERTEXFORMAT_FLOAT2 };
	pd.layout.buffers[0].stride = 48;
	pd.index_type = SG_INDEXTYPE_UINT32;
	pd.depth.compare = cmp;
	pd.depth.write_enabled = false;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.colors[0].blend.enabled = true;
	pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
	pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	pd.label = label;
	return sg_make_pipeline(&pd);
}
}

void render(const Frame& f, const float mvp[16], const double cam[3], float h)
{
	if (!f.mesh || f.mesh->idx.empty()) { return; }
	if (!g.ready)
	{
		g.shd = sg_make_shader(roadribbon_shader_desc(sg_query_backend()));
		g.vis = make(SG_COMPAREFUNC_LESS_EQUAL, "road-ribbons-visible");
		g.hid = make(SG_COMPAREFUNC_GREATER, "road-ribbons-hidden");
		g.ready = true;
	}
	if (g.held != f.mesh)	// a new mesh (the network loaded, or rebuilt): once, not per frame
	{
		sg_destroy_buffer(g.vb);
		sg_destroy_buffer(g.ib);
		sg_buffer_desc bd = {};
		bd.data = { f.mesh->verts.data(), f.mesh->verts.size() * sizeof(float) };
		bd.label = "road-ribbons-verts";
		g.vb = sg_make_buffer(&bd);
		bd = {};
		bd.usage.index_buffer = true;
		bd.data = { f.mesh->idx.data(), f.mesh->idx.size() * sizeof(uint32_t) };
		bd.label = "road-ribbons-idx";
		g.ib = sg_make_buffer(&bd);
		g.indexCount = (int)f.mesh->idx.size();
		g.held = f.mesh;
	}
	const float fy = std::sqrt(mvp[1] * mvp[1] + mvp[5] * mvp[5] + mvp[9] * mvp[9]);
	const float perW = 1.0f / std::max(0.5f * h * fy, 1.0f);	// world units per pixel per clip w
	rr_vs_params_t vs = {};
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	vs.cam[0] = (float)cam[0]; vs.cam[1] = (float)cam[1]; vs.cam[2] = (float)cam[2];
	vs.p0[0] = (f.params.ribbon ? kMinHalfPx : kThinHalfPx) * perW;
	vs.p0[1] = f.params.ribbon ? 1.0f : 0.0f;
	vs.p0[2] = f.params.range;
	vs.p0[3] = f.params.fadeFrom;
	vs.p1[0] = kDash;
	vs.p1[2] = kGain;
	vs.p1[3] = 8.0f * perW;	// a dash at least 8 px
	sg_bindings b = {};
	b.vertex_buffers[0] = g.vb;
	b.index_buffer = g.ib;
	for (int pass = 0; pass < 2; pass++)
	{
		drawstats::pipeline(pass ? g.hid : g.vis);
		vs.p1[1] = (float)pass;
		sg_apply_uniforms(UB_rr_vs_params, SG_RANGE(vs));
		rr_fs_params_t fs = {};
		fs.q0[0] = (float)pass;
		sg_apply_uniforms(UB_rr_fs_params, SG_RANGE(fs));
		drawstats::bindings(&b);
		drawstats::draw(0, g.indexCount, 1);
	}
}
}
