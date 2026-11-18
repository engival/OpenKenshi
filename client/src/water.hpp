#pragma once
#include <future>
#include <string>
#include <vector>

#include "sky.hpp"
#include "sokol_gfx.h"
#include "terrain.hpp"

// The global water plane (M4, docs/reference/WATER.md): one plane at y = 100 drawn over every terrain chunk that dips below it, alpha blended after the
// opaque world. Colour = overlaymaps/watercolourmap.png, flow + scum amount = overlaymaps/flowmap.png, normals scrolled as the game's forward/water.hlsl
// does. The per-biome uniforms (gloss, visibility, strength, glow, scales, distortion) and maps (water normal, turbulence, scum) come from the BIOMES
// records (WATER.md 2.1), baked on a worker thread: biomemap.png as table rows, a row of uniforms per biome, the maps as texture arrays.
class Water
{
public:
	static constexpr float kLevel = 100.0f;

	// The PNG decodes (the CPU half of init: ~40 ms of the 2048^2 flowmap): safe on a worker (kenshi-k16.2.26).
	struct Prepared
	{
		std::vector<unsigned char> px[2];	// colour, flow
		int w[2] = {}, h[2] = {};
		std::string path[2];
	};
	static Prepared prepare(const std::string& dataDir);

	// dataDir: the game's data/. A biome whose scum maps are missing or unusable is reported and drawn without scum.
	bool init(const std::string& dataDir);
	bool init(const std::string& dataDir, Prepared p);	// the sokol half (the loading step polls prepare() on a worker)
	void shutdown();

	// Draws after the opaque world. seconds = animation clock. rain = the effective rain the particles use (0..1): the game's water rainAmount (intensity / 50,
	// clamped) adds the rain ripple rings, none in clear weather. wait = block until the per-biome parameters are baked (scripted runs).
	void render(const Terrain& terrain, const float mvp[16], const LightParams& light, const float sunRaw[3], double seconds, float rain, bool wait, sg_view sceneDepth = {}, float nearZ = 1, float farZ = 1);	// sunRaw = Sky::sunColour()

	bool flat = false;	// debug: the unlit water colour only
	// the planar reflection for this frame's render (reflection.hpp); on = false: none drawn (rf = 1)
	void setReflection(sg_view view, bool on) { reflView_ = view; reflOn_ = on; }
	// whether the water was drawn last frame (the reflection pass is skipped without it)
	bool drawn() const { return !chunks_.empty(); }
	// The water plane's share of the view (0..1) as the last render saw it (kenshi-k16.2.205.11): the projected area of the drawn chunks, the largest
	// LOD's, clamped. main.cpp tips the planar reflection to sky-only below a threshold (the world in it is only visible through the water pixels).
	float coverage() const { return coverage_; }

	// the worker thread's result (water.cpp bake)
	struct Bake
	{
		std::vector<unsigned char> index;	// kBiomeSize^2 table rows (R8), 0 = no record
		std::vector<float> table;		// kTableWidth RGBA32F texels per row
		std::vector<uint8_t> scum[2];		// diffuse, normal: BC3 arrays, mip-major (all layers of a mip together)
		int scumLayers = 0;
		std::vector<unsigned char> normals, turb;	// water normal and turbulence maps: RGBA8 arrays, mip-major
		int normalLayers = 0, turbLayers = 0;
	};

private:
	static constexpr int kImages = 9;

	void useBake(const Bake& b);	// (re)creates the index, table and scum images
	void waterCoverage(const Terrain& terrain, const float mvp[16]);	// the last render's share of the view (coverage)

	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_sampler clampSmp_ = {}, repeatSmp_ = {}, nearSmp_ = {};
	sg_image img_[kImages] = {};	// colour, flow, normal maps, scum diffuse, scum normal, biome rows, biome table, turbulence maps, rain ripples
	sg_view view_[kImages] = {};
	bool hasScum_ = false;
	sg_view reflView_ = {};
	bool reflOn_ = false;
	float coverage_ = 0;
	std::future<Bake> bake_;
	std::vector<Terrain::WaterChunk> chunks_;
};
