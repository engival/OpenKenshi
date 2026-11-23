#pragma once
#include <cstdint>
#include <memory>
#include <vector>

// The road overlay's ribbons (kenshi-qjzb; the CPU-projected quads of kenshi-y8ni left buildings out of the occlusion, cost 1.7 ms at a far view and skipped a piece at the near plane).
// dev/roadedit.cpp builds the whole network's strip mesh ONCE (terrain-draped, world space; the terrain does not change) and hands it to the packet by pointer; this renderer uploads
// a new mesh when the pointer changes and draws it in the scene pass after the opaque world, twice: the visible part (depth LESS_EQUAL) and the part the scene depth hides (GREATER,
// dimmed, dashed). The GPU clips at the near plane, widens to a minimum width on screen, fades and darkens with distance. shaders/roadribbon.glsl.
namespace roadribbons
{
struct Mesh
{
	// per vertex 12 floats: x y z (the point, world), sx sz w (the unit xz normal of the strip's side, the strip's full width baked in), r g b, along (u from the road's start),
	// off (this vertex's offset from the centre line along the normal, u; 0..w, or an outline's inner/outer edge), hatch (1: a same-direction duplicate, drawn hatched)
	std::vector<float> verts;
	std::vector<uint32_t> idx;
};

struct Params
{
	bool ribbon = true;		// real width (else a thin line)
	float range = 12000.0f, fadeFrom = 8000.0f;	// u from the camera: alpha falls from fadeFrom to 0 at range
};

// The packet's block: null mesh = nothing drawn.
struct Frame
{
	std::shared_ptr<const Mesh> mesh;
	Params params;
};

// Render side, inside the scene pass after the opaque world (the depth buffer is bound). cam = the camera position, mvp = camera-relative, h = the framebuffer height (px).
void render(const Frame& f, const float mvp[16], const double cam[3], float h);
}
