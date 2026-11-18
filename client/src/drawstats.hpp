#pragma once

// Per-pass submit counters (kenshi-k16.2.101.1): draw calls, instances, triangles, pipeline changes and binding changes, counted at the client's
// own submit wrappers - no driver queries (sokol's GL frame stats carry no draw count, and the GPU timer only gives time). GpuTimer::begin(s)
// makes s the bucket; submits outside a timed section (the untimed passes: squads, arrows, the fade band, the world map) go to no bucket.
// Triangles assume a triangle list per draw (sg_draw's index count / 3). sokol_gl batches (the UI layer and the debug views) submit inside sgl, past
// these wrappers: the DRAWSTATS_QUADS / TRIANGLES / LINES macros count those at their sgl_end sites, over their vertex count; sgl's own pipeline and
// binding changes are not visible, so the UI's pipelines/bindings read 0. KCLIENT_PROFILE=0: the wrappers are plain pass-throughs and the macros
// compile to nothing.
#include <cstdint>
#include "sokol_gfx.h"
#include "gputimer.hpp"

namespace drawstats
{
struct Pass
{
	std::uint32_t draws = 0, instances = 0, triangles = 0, pipelines = 0, bindings = 0;
};

#if KCLIENT_PROFILE
inline Pass passes[GpuTimer::kSections];
inline int current = -1;
inline void reset() { for (Pass& p : passes) { p = Pass{}; } }
inline void section(int s) { current = s; }
inline void pipeline(sg_pipeline p)
{
	if (current >= 0) { passes[current].pipelines++; }
	sg_apply_pipeline(p);
}
inline void bindings(const sg_bindings* b)
{
	if (current >= 0) { passes[current].bindings++; }
	sg_apply_bindings(b);
}
inline void draw(int base, int elements, int instances, int baseInstance = 0)
{
	if (current >= 0)
	{
		Pass& p = passes[current];
		p.draws++;
		p.instances += (std::uint32_t)instances;
		p.triangles += (std::uint32_t)(elements / 3) * (std::uint32_t)instances;
	}
	sg_draw_ex(base, elements, instances, 0, baseInstance);
}

// The UI layer (src/ui, the kui library) submits through sokol_gl, not through the wrappers above: its batches are counted here instead, one draw per
// non-empty sgl_end (sgl flushes the batch as one sg_draw). sgl's own pipeline and binding changes are not visible to us, so those stay 0 for the UI.
inline void quads(int vertices)
{
	if (current < 0 || vertices <= 0) { return; }
	Pass& p = passes[current];
	p.draws++;
	p.instances++;
	p.triangles += (std::uint32_t)(vertices / 4) * 2;
}
inline void triangles(int vertices)
{
	if (current < 0 || vertices <= 0) { return; }
	Pass& p = passes[current];
	p.draws++;
	p.instances++;
	p.triangles += (std::uint32_t)(vertices / 3);
}
inline void lines(int vertices)
{
	if (current < 0 || vertices <= 0) { return; }
	Pass& p = passes[current];
	p.draws++;
	p.instances++;
}
#else
inline void reset() {}
inline void section(int) {}
inline void pipeline(sg_pipeline p) { sg_apply_pipeline(p); }
inline void bindings(const sg_bindings* b) { sg_apply_bindings(b); }
inline void draw(int base, int elements, int instances, int baseInstance = 0) { sg_draw_ex(base, elements, instances, 0, baseInstance); }
#endif
}	// namespace drawstats

// sokol_gl batch counting at the UI's sgl_end sites (kenshi-k16.2.101.1); compiled out with KCLIENT_PROFILE=0
#if KCLIENT_PROFILE
#define DRAWSTATS_QUADS(vertices) drawstats::quads(vertices)
#define DRAWSTATS_TRIANGLES(vertices) drawstats::triangles(vertices)
#define DRAWSTATS_LINES(vertices) drawstats::lines(vertices)
#else
#define DRAWSTATS_QUADS(vertices) ((void)0)
#define DRAWSTATS_TRIANGLES(vertices) ((void)0)
#define DRAWSTATS_LINES(vertices) ((void)0)
#endif
