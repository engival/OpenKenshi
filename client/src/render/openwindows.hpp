#pragma once
#include <cstdint>
#include <string>
#include <vector>

// render.open_windows (experimental, kenshi-jj3.10.1): the faces a building mesh closes its openings with, so the light bake and the draw can leave
// them out. The original does not skip them by colour: the Storm House's four window barrier quads are in Storm_House_01_Extras.mesh, textured from a
// dark grey patch of _New-House_components_DIF.dds (all but one sample 0.18..0.32 raw luma), and the game draws them (that is the black window seen
// from outside). The other session's "faces with a 0,0,0 colour do not render" is false for them.
//
// What they are, measured over every Storm House / Watchtower / Shack `Extras` mesh: a detached island of 2 triangles (a quad), sometimes two quads
// sharing a corner (4 triangles), welded to nothing else in the mesh, whose three vertices all sample that dark patch. Some families close the
// opening with a box instead: the Watchtower's barrier is the dark quad plus a second quad right behind it pinned to one dark texel of the atlas
// (a flat-coloured face), which has to go too or it still shuts the light out. So the rule is geometric plus texture: a small detached island, all of
// its texels dark (plus the flat twin a dark island's box holds). `find` gets the islands, `dark` / `twin` the texture verdict, `triangles` the
// list the draw and the bake leave out. Pure CPU, no sokol (the bake and the mesh decode both use it).
namespace openwindows
{
// One detached island: the triangle numbers (index i / 3 in the submesh's own range, the numbering the bake's per-submesh drop uses) and its
// building-local bounding box.
struct Island
{
	std::vector<uint32_t> tris;
	std::vector<float> uv;	// 2 floats per vertex, triangle by triangle: triangle tris[k], vertex j -> uv[6k + 2j], uv[6k + 2j + 1]
	float mn[3] = {}, mx[3] = {};
};

// The detached islands of 1..kMaxTris triangles in indices[first..first+count): vertices welded by position (0.01 mesh units), the island's
// bounding-box diagonal between minDiagonal and maxDiagonal (an opening at least ~30 cm across, at most ~1.7 m: a window, not a rivet or a wall).
// A large mesh's union-find runs once per call; islands overlap no triangle.
constexpr int kMaxTris = 4;
std::vector<Island> find(const std::vector<float>& verts, int stride, const std::vector<uint32_t>& indices, uint32_t first, uint32_t count, float maxDiagonal = 26.0f,
	float minDiagonal = 4.0f);

// The "window panes" part records (the Outpost's bravo-*-windows, hpg-*-windows ...): a mesh whose file name says "window" holds nothing but
// the panes, so by part record (not island size: s-III's are 60 / 208-triangle strips, s-IV's separate 2-triangle quads) its whole submesh is the one
// candidate, left out when its texels are dark like any barrier (kenshi-jj3.10.1 rework, lead comment 2026-11-14).
bool isPanePath(const std::string& path);
Island whole(const std::vector<float>& verts, int stride, const std::vector<uint32_t>& indices, uint32_t first, uint32_t count);

// A decode of the part's albedo map as one luma byte per texel (rows top first, Ogre v = 0 at the top, like the client samples it), or RGBA8.
struct Texel
{
	const uint8_t* luma = nullptr;	// one byte per texel; used when set
	const uint8_t* rgba = nullptr;	// else RGBA8
	int w = 0, h = 0;
	float at(float u, float v) const;	// repeat-wrapped, as the shader samples it
};

// True when the island is textured from a dark patch: the island's UV rectangle is a real area (a flat-coloured face pins its UVs to one texel and is
// no quad) and every texel of a 5x5 grid across that rectangle is at most maxLuma. u, v are multiplied by the part's tile factors, as the draw does.
bool dark(const Island& island, float tileX, float tileY, const Texel& tex, float maxLuma = 0.35f);

// A flat-coloured island (its UV rectangle inside one or two texels) whose texels are dark: the back face of a barrier some families model as a closed
// box (Watchtower: each opening is a dark-textured front quad plus a back quad pinned to one dark texel of the atlas).
bool twin(const Island& island, float tileX, float tileY, const Texel& tex, float maxLuma = 0.35f);

// The triangles (submesh-local, like Island::tris) of a submesh's barrier islands: every dark island, plus every twin whose bounding box a dark
// island's box reaches within pad mesh units (the Watchtower's box partner sits 4.5 units back, across the wall cavity). Sorted, duplicates removed.
// This is the one list the draw, the shadow pass and the bake leave out.
std::vector<uint32_t> triangles(const std::vector<Island>& islands, float tileX, float tileY, const Texel& tex, float maxLuma = 0.35f, float pad = 5.0f);
}
