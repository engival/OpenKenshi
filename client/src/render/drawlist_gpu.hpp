#pragma once
#include <cstdint>
#include <vector>

#include "render/drawlist.hpp"
#include "sokol_gfx.h"

// The render side of the 2D draw list (drawlist.hpp; render hook 8, kenshi-k16.2.205.12.28): the texture table behind dl::TexId, the
// uploads, and the replay into sokol_gl. Render thread only (on one thread, inside render_frame). Its pipeline and samplers are made on
// first use, so a run whose UI records nothing creates no sokol resource here.
namespace dl::gpu
{
	// A texture the host owns (not destroyed here) under id: the portrait atlas, the prospecting map, menu art, the UI atlas for the cursors.
	void bind(TexId id, sg_view view, sg_sampler smp);
	void unbind(TexId id);
	sg_view view(TexId id);		// id SG_INVALID_ID when unknown
	sg_sampler sampler(TexId id);

	// One packet's uploads, in order (a resource job queued by logic for that packet, so it runs once before the packet is drawn).
	// The destroys this packet recorded, and the images an update replaced, are destroyed at the next call: the packet that last drew
	// with them can still be drawn. Call it once per packet, uploads or not.
	void apply(std::vector<Upload> ups);

	// Inside the overlay pass: the list's sgl calls, as gfx::begin .. gfx::end would have made them. An empty list issues nothing.
	void replay(const List& list);

	void shutdown();	// before sgl_shutdown: every owned texture, the pipeline and the samplers

	struct Stats
	{
		uint32_t draws = 0, vertices = 0;	// the last replay's groups and vertices
		uint32_t missing = 0;		// the last replay's groups skipped for an unknown texture id
		uint32_t textures = 0;		// owned textures alive
		uint64_t uploadBytes = 0;	// pixels uploaded so far
	};
	const Stats& stats();
}
