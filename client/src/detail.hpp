#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "sokol_gfx.h"

// Terrain detail splatting data (docs/reference/TERRAIN_SPLAT.md): per-biome tiled layer textures in two texture arrays
// (diffuse BC3, normal BC1; one layer per distinct file), a per-biome parameter table, the overlay tiles (grass/dirt/road
// weights), the biome blend map and the ground colour map. Chunks ask biomesFor() which biomes they blend.
class TerrainDetail
{
public:
	static constexpr int kLayerSize = 1024;		// layers are the 2048^2 files minus the top mip
	static constexpr int kOverlaySize = 8192;	// 8x8 tiles of 1024^2
	static constexpr int kMaxChunkBiomes = 4;
	static constexpr double kMaxChunkSize = 9216.0;	// one blendinfo cell (32 blend texels of 288 units); bigger chunks get no detail

	struct ChunkBiomes
	{
		int count = 0;		// 0 = no detail for this chunk
		float row[4] = {};	// biome table row
		float chan[4] = {};	// blend map channel (biome 0: unused, it takes the remainder)
	};

	// dataDir: <install>/data. Returns false when a required file is missing (terrain then runs without detail).
	bool init(const std::string& dataDir);	// = upload(prepare(dataDir))

	// init in two halves: prepare() is all CPU work (file reads, decode, mips; no sokol calls) and may run on a worker thread, upload() makes the GPU
	// resources on the main thread. Do not touch this object between the two. A failed prepare has ok = false.
	struct Prepared
	{
		bool ok = false;
		std::vector<uint8_t> diffBlob, nrmBlob, blend, ground, overlay;	// array blobs are mip-major, all slices of a mip together
		std::vector<float> table;
		int rows = 0;
		double t0 = 0;		// steady_clock seconds when prepare started
	};
	Prepared prepare(const std::string& dataDir);
	int uploadPart(Prepared& p, int part);	// one frame's uploads (kenshi-k16.2.26): 0 arrays, 1 overlay, 2 the rest; 1 more, 0 done, -1 failed
	bool upload(Prepared& p);
	void shutdown();

	// x0/z0/size: chunk rectangle in world units.
	ChunkBiomes biomesFor(double x0, double z0, double size) const;

	// Biome table row of the blend map texel under (x, z): the one biome a map feature (TERRAIN mode, mapfeature_fs) is textured with.
	float rowAt(double x, double z) const;

	sg_view diffuse = {}, normal = {}, overlay = {}, blendView = {}, biomeTable = {}, groundView = {};
	sg_sampler arraySmp = {}, linearSmp = {}, tableSmp = {};
	bool ok = false;
	double loadSeconds = 0;
	int layersDiffuse = 0, layersNormal = 0, biomeCount = 0;

private:
	sg_image images_[6] = {};
	std::vector<unsigned char> slots_;		// blend map slot per texel (0..4), 1024^2
	std::vector<unsigned> keys_;			// palette key per cell and slot: [cell * 5 + slot]
	std::unordered_map<unsigned, int> rowOfKey_;
};
