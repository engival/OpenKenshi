#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "render/colourmap.hpp"
#include "render/detail.hpp"
#include "kworld/heightmap.hpp"
#include "render/blooddecals.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "sokol_gfx.h"

// Chunked quadtree terrain (CDLOD-like, without morphing; LOD seams are hidden by skirts).
// Every node is a 64x64 cell grid whose heights live in a small R32F texture (67x67, 1 texel
// border for normals) point-sampled from the heightmap at stride 2^level. Textures are created
// on demand around the camera and evicted LRU, so memory stays bounded.
class Terrain
{
public:
	static constexpr int kCells = 64;
	static constexpr int kTex = kCells + 3;
	static constexpr int kMaxLevel = 8;		// 64 * 2^8 = 16384 samples: one root node covers the world
	static constexpr size_t kMaxResident = 2048;	// ~18 KB each

	// biomePath: RGBA PNG covering the whole world (colour source). Returns false on load failure.
	// overlayDir: data/newland/land/overlaymaps (colour tiles; missing = falls back to the biome map).
	// dataDir (<install>/data): BIOMES records and detail textures for the splat shading; without them the colour map alone is used.
	// Startup is three calls, in order, so a loading screen can present between them: init (biome map, grid), initColour, initDetail (+ pipelines).
	bool init(const kworld::Heightmap* hm, const std::string& biomePath);
	bool initColour(bool loaded);
	int initDetailPart(TerrainDetail::Prepared& prepared, int part);	// 1: more parts, 0: done (pipelines made)
	bool initDetail(TerrainDetail::Prepared& prepared);	// detail.prepare(dataDir) may run on a worker thread meanwhile
	void shutdown();

	// cam is the world position; mvp the column-major projection*rotation-only-view matrix.
	// maxLoads < 0 means unlimited (screenshots).
	void render(const double cam[3], const float mvp[16], const LightParams& light, int maxLoads, int view);	// view: the packet's copy of viewMode
	// Draws the last render()'s chunk selection again with another matrix (the water reflection: a fresh selection for its frustum would fall back to
	// coarse ancestors where no chunk is loaded, whose surface lids the river channels).
	void renderView(const float mvp[16], const LightParams& light, int view);

	// Sun shadow map to sample (set once; must outlive render()); renderShadow() draws the terrain into one cascade of it (inside Shadow::beginPass/endPass).
	const Shadow* shadow = nullptr;
	const BloodDecals* blood = nullptr;	// ground blood (set once, before render())
	void renderShadow(const double cam[3], const ShadowCascade& c, int maxLoads);
	uint64_t shadowGen() const { return chunkGen_; }	// changes when the resident chunk set does: part of the shadow cache's key (kenshi-k16.2.205.12.2)

	int viewMode = 0;	// logic's (the draws take FramePacket::scene.terrainView); 0 detail splat (colour map when unavailable), 1 biome map, 2 plain, 3 colour map only (F2 cycles)
	TerrainDetail detail;
	GroundColour colour;
	bool hasColour = false;
	bool detailFailed_ = false;	// initDetailPart's upload failed: the remaining parts are skipped (viewMode 3 set)

	size_t resident() const { return chunks_.size(); }
	int drawn() const { return drawn_; }

	// The chunks the last render() drew whose heights dip below `level` (the water pass draws a plane over exactly these).
	// origin/chunk are the vs_params fields of terrain.glsl (camera-relative origin + cell size, grid cells + world offset).
	struct WaterChunk
	{
		sg_view hmap;
		float origin[4];
		float chunk[4];
	};
	void waterChunks(float level, std::vector<WaterChunk>& out) const;
	const kworld::Heightmap* heightmap() const { return hm_; }	// the water's coverage estimate reads it (Water::coverage)
	sg_buffer gridVerts() const { return vbuf_; }
	sg_buffer gridIndices() const { return ibuf_; }
	static constexpr int kGridIndices = (kCells + 2) * (kCells + 2) * 6;

private:
	struct Chunk
	{
		sg_image image;
		sg_view view;
		uint64_t lastUsed;
		float minH;
	};
	struct Draw
	{
		int level, ix, iz;
	};

	static uint64_t key(int level, int ix, int iz) { return ((uint64_t)level << 48) | ((uint64_t)ix << 24) | (uint64_t)iz; }
	Chunk* load(int level, int ix, int iz);
	void select(int level, int ix, int iz);
	void evict();
	void placement(const Draw& d, float origin[4], float chunk[4]) const;
	void drawList(const std::vector<Draw>& list, const float mvp[16], const LightParams& light, int view);

	const kworld::Heightmap* hm_ = nullptr;
	sg_buffer vbuf_ = {};
	sg_buffer ibuf_ = {};
	sg_sampler smp_ = {};
	sg_image biomeImg_ = {};
	sg_view biomeView_ = {};
	sg_sampler biomeSmp_ = {};
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_shader depthShd_ = {};
	sg_pipeline depthPip_ = {};
	std::unordered_map<uint64_t, Chunk> chunks_;
	std::vector<Draw> draws_;
	std::vector<Draw> viewDraws_;	// render()'s selection, for renderView
	uint64_t frame_ = 0;
	uint64_t chunkGen_ = 0;	// a chunk became resident or was evicted (Terrain::shadowGen)
	int budget_ = 0;
	int drawn_ = 0;
	double cam_[3] = {};
	float planes_[4][4] = {};
	float groundAtCam_ = 0;
};
