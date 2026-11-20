#pragma once
#include <string>
#include <vector>

#include "sokol_gfx.h"

// Ground colour: `overlaymaps/colour.X.Y.png` (8x8 tiles of 2048^2, X = column, Y = row, no flips;
// 16384^2 over the whole world, 18 units/px). Too big to upload raw (1 GiB), so the first run builds a
// BC1 + full mip chain cache (~171 MiB) under the per-user cache root's kclient/ (app/userdirs.hpp: $XDG_CACHE_HOME or ~/.cache; Windows
// %LOCALAPPDATA%\OpenKenshi\cache), keyed by the
// source files' sizes and mtimes. Later runs just read the cache.
class GroundColour
{
public:
	static constexpr int kSize = 16384;
	static constexpr int kTiles = 8;
	static constexpr int kMips = 15;	// 16384 .. 1

	// overlayDir: the game's data/newland/land/overlaymaps. Returns false when tiles are missing or the cache can't be used.
	bool init(const std::string& overlayDir);
	bool load(const std::string& overlayDir);	// the CPU half (file reads, cache build, low-mip decode): safe on a worker (kenshi-k16.2.26)
	bool upload();	// the sokol half, on the main thread; false when load() found no usable cache
	void shutdown();

	sg_image img = {};
	sg_view view = {};
	sg_sampler smp = {};
	sg_image alphaImg = {};	// R8, the colour map's alpha (gloss multiplier), 4096^2 + mips, sampled with `smp`
	sg_view alphaView = {};
	double buildSeconds = 0;	// 0 when the cache already existed
	size_t cacheBytes = 0;
	int uploadedSize = 0;		// 16384, or smaller when the GPU's texture limit is lower

	// The colour at a world position from a CPU copy of mip kLowMip (bilinear; raw, as the terrain shades it): the dynamic ambient's ground albedo.
	static constexpr int kLowMip = 4;	// 1024^2, 288 world units a texel
	bool albedoAt(double x, double z, float rgb[3]) const;

private:
	std::vector<uint8_t> low_;	// RGB8, (kSize >> kLowMip)^2, decoded from the cache's BC1 at init
	std::vector<uint8_t> file_, alpha_;	// load()'s cache bytes, freed by upload()
	bool alphaOk_ = false;
};
