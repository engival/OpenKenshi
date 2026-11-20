#include "render/colourmap.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include "app/userdirs.hpp"
#include "stb_dxt.h"
#include "stb_image.h"

namespace
{

namespace fs = std::filesystem;

constexpr int kTile = GroundColour::kSize / GroundColour::kTiles;	// 2048
constexpr int kTileMips = 10;			// tile levels 0..9 (2048..4 px) are block-aligned inside the mosaic
constexpr int kBuildThreads = 8;
constexpr uint32_t kVersion = 1;

struct Header
{
	char magic[4];
	uint32_t version;
	uint32_t size;
	uint32_t mips;
};

size_t level_bytes(int level)
{
	const size_t blocks = std::max(1, (GroundColour::kSize >> level) / 4);
	return blocks * blocks * 8;
}

size_t level_offset(int level)
{
	size_t o = sizeof(Header);
	for (int l = 0; l < level; l++) { o += level_bytes(l); }
	return o;
}

// RGBA8 box filter, w x h -> w/2 x h/2 (alpha forced opaque; BC1 here carries colour only).
std::vector<uint8_t> halve(const std::vector<uint8_t>& src, int w)
{
	const int n = w / 2;
	std::vector<uint8_t> dst((size_t)n * n * 4);
	for (int y = 0; y < n; y++)
	{
		for (int x = 0; x < n; x++)
		{
			for (int c = 0; c < 3; c++)
			{
				const size_t i = ((size_t)(2 * y) * w + 2 * x) * 4 + c;
				dst[((size_t)y * n + x) * 4 + c] = (uint8_t)((src[i] + src[i + 4] + src[i + (size_t)w * 4] + src[i + (size_t)w * 4 + 4] + 2) >> 2);
			}
			dst[((size_t)y * n + x) * 4 + 3] = 255;
		}
	}
	return dst;
}

// Writes the BC1 blocks of an RGBA image (w >= 4) into a mosaic level buffer at pixel position (px0, py0).
void compress_into(uint8_t* levelBuf, int levelW, const std::vector<uint8_t>& img, int w, int px0, int py0)
{
	const size_t blocksPerRow = (size_t)levelW / 4;
	for (int by = 0; by < w / 4; by++)
	{
		for (int bx = 0; bx < w / 4; bx++)
		{
			uint8_t blk[64];
			for (int y = 0; y < 4; y++) { memcpy(blk + y * 16, &img[((size_t)(by * 4 + y) * w + bx * 4) * 4], 16); }
			stb_compress_dxt_block(levelBuf + ((size_t)(py0 / 4 + by) * blocksPerRow + px0 / 4 + bx) * 8, blk, 0, STB_DXT_HIGHQUAL);
		}
	}
}

// The per-user cache root (app/userdirs.hpp; kenshi-379.31.3: on Windows %LOCALAPPDATA%\OpenKenshi\cache, never the Linux HOME).
std::string cache_dir()
{
	return plat::userCacheDir() + "/kclient";
}

std::string tile_path(const std::string& dir, int x, int y) { return dir + "/colour." + std::to_string(x) + "." + std::to_string(y) + ".png"; }

// Hash of every tile's size and mtime, so a changed install rebuilds the cache.
bool source_key(const std::string& dir, uint64_t& key)
{
	key = 1469598103934665603ull;
	std::error_code ec;
	for (int y = 0; y < GroundColour::kTiles; y++)
	{
		for (int x = 0; x < GroundColour::kTiles; x++)
		{
			const std::string p = tile_path(dir, x, y);
			const uint64_t v[2] = { (uint64_t)fs::file_size(p, ec), (uint64_t)fs::last_write_time(p, ec).time_since_epoch().count() };
			if (ec) { return false; }
			for (uint64_t w : v) { key = (key ^ w) * 1099511628211ull; }
		}
	}
	return true;
}

bool build_cache(const std::string& dir, const std::string& outPath)
{
	std::vector<uint8_t> file(level_offset(GroundColour::kMips));
	Header h = { { 'K', 'C', 'O', 'L' }, kVersion, GroundColour::kSize, GroundColour::kMips };
	memcpy(file.data(), &h, sizeof h);
	std::vector<uint8_t> top(32 * 32 * 4, 255);	// level 9 of the mosaic: 4x4 px per tile
	std::atomic<int> next{ 0 };
	std::atomic<bool> failed{ false };
	auto worker = [&]()
	{
		for (int t; (t = next++) < GroundColour::kTiles * GroundColour::kTiles && !failed;)
		{
			const int tx = t % GroundColour::kTiles, ty = t / GroundColour::kTiles;
			int w = 0, hh = 0, comp = 0;
			stbi_uc* src = stbi_load(tile_path(dir, tx, ty).c_str(), &w, &hh, &comp, 4);
			if (!src || w != kTile || hh != kTile) { fprintf(stderr, "kclient: bad colour tile %d.%d\n", tx, ty); failed = true; if (src) { stbi_image_free(src); } continue; }
			std::vector<uint8_t> img(src, src + (size_t)kTile * kTile * 4);
			stbi_image_free(src);
			for (size_t i = 3; i < img.size(); i += 4) { img[i] = 255; }
			for (int l = 0; l < kTileMips; l++)
			{
				const int lw = kTile >> l;
				compress_into(file.data() + level_offset(l), GroundColour::kSize >> l, img, lw, tx * lw, ty * lw);
				if (l == kTileMips - 1)
				{
					for (int y = 0; y < 4; y++) { memcpy(&top[((size_t)(ty * 4 + y) * 32 + tx * 4) * 4], &img[(size_t)y * 4 * 4], 16); }
				}
				else { img = halve(img, lw); }
			}
		}
	};
	std::vector<std::thread> pool;
	for (int i = 0; i < kBuildThreads; i++) { pool.emplace_back(worker); }
	for (auto& th : pool) { th.join(); }
	if (failed) { return false; }

	// Levels 10..14 (16 px down to 1) come from the assembled level 9; sub-block levels replicate into one 4x4 block.
	std::vector<uint8_t> img = top;
	int w = 32;
	for (int l = GroundColour::kMips - 5 + 0; l < GroundColour::kMips; l++)
	{
		img = halve(img, w);
		w /= 2;
		std::vector<uint8_t> padded((size_t)std::max(w, 4) * std::max(w, 4) * 4, 255);
		for (int y = 0; y < std::max(w, 4); y++)
		{
			for (int x = 0; x < std::max(w, 4); x++) { memcpy(&padded[((size_t)y * std::max(w, 4) + x) * 4], &img[((size_t)(y % w) * w + x % w) * 4], 4); }
		}
		compress_into(file.data() + level_offset(l), std::max(GroundColour::kSize >> l, 4), padded, std::max(w, 4), 0, 0);
	}
	std::error_code ec;
	fs::create_directories(fs::path(outPath).parent_path(), ec);
	const std::string tmp = outPath + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) { return false; }
	const bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
	fclose(f);
	if (!ok) { return false; }
	fs::rename(tmp, outPath, ec);
	return !ec;
}

// Alpha of the colour map (the game's `colourMap.Sample().a`, a gloss multiplier: terrainfp4.hlsl computeBiome scales every layer's gloss with it). The BC1 mosaic drops it,
// so a second R8 mosaic holds only the alpha, 4x4-box reduced (4096^2, 72 units per texel) with a box mip chain, cached next to the colour cache.
constexpr int kAlphaSize = 4096;
constexpr int kAlphaMips = 13;
constexpr int kAlphaTile = kAlphaSize / GroundColour::kTiles;	// 512

size_t alpha_level_offset(int level)
{
	size_t o = 0;
	for (int l = 0; l < level; l++) { o += (size_t)(kAlphaSize >> l) * (kAlphaSize >> l); }
	return o;
}

bool build_alpha_cache(const std::string& dir, const std::string& outPath)
{
	std::vector<uint8_t> file(alpha_level_offset(kAlphaMips));
	std::atomic<int> next{ 0 };
	std::atomic<bool> failed{ false };
	auto worker = [&]()
	{
		for (int t; (t = next++) < GroundColour::kTiles * GroundColour::kTiles && !failed;)
		{
			const int tx = t % GroundColour::kTiles, ty = t / GroundColour::kTiles;
			int w = 0, hh = 0, comp = 0;
			stbi_uc* src = stbi_load(tile_path(dir, tx, ty).c_str(), &w, &hh, &comp, 4);
			if (!src || w != kTile || hh != kTile) { failed = true; if (src) { stbi_image_free(src); } continue; }
			const int k = kTile / kAlphaTile;
			for (int y = 0; y < kAlphaTile; y++)
			{
				for (int x = 0; x < kAlphaTile; x++)
				{
					int sum = 0;
					for (int j = 0; j < k; j++) { for (int i = 0; i < k; i++) { sum += src[((size_t)(y * k + j) * kTile + x * k + i) * 4 + 3]; } }
					file[(size_t)(ty * kAlphaTile + y) * kAlphaSize + tx * kAlphaTile + x] = (uint8_t)((sum + k * k / 2) / (k * k));
				}
			}
			stbi_image_free(src);
		}
	};
	std::vector<std::thread> pool;
	for (int i = 0; i < kBuildThreads; i++) { pool.emplace_back(worker); }
	for (std::thread& th : pool) { th.join(); }
	if (failed) { return false; }
	for (int l = 1; l < kAlphaMips; l++)
	{
		const int n = kAlphaSize >> l;
		const uint8_t* up = file.data() + alpha_level_offset(l - 1);
		uint8_t* dst = file.data() + alpha_level_offset(l);
		for (int y = 0; y < n; y++)
		{
			for (int x = 0; x < n; x++)
			{
				const uint8_t* q = up + (size_t)(2 * y) * (2 * n) + 2 * x;
				dst[(size_t)y * n + x] = (uint8_t)((q[0] + q[1] + q[2 * n] + q[2 * n + 1] + 2) >> 2);
			}
		}
	}
	std::error_code ec;
	fs::create_directories(fs::path(outPath).parent_path(), ec);
	const std::string tmp = outPath + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) { return false; }
	const bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
	fclose(f);
	if (!ok) { return false; }
	fs::rename(tmp, outPath, ec);
	return !ec;
}

}

bool GroundColour::load(const std::string& overlayDir)	// CPU only (file reads, the cache build when missing, the low-mip decode): may run on a worker (kenshi-k16.2.26)
{
	uint64_t key;
	if (!source_key(overlayDir, key)) { return false; }
	char name[64];
	snprintf(name, sizeof name, "/colour-%016llx.bc1", (unsigned long long)key);
	const std::string path = cache_dir() + name;
	std::error_code ec;
	if (!fs::exists(path, ec))
	{
		const auto t0 = std::chrono::steady_clock::now();
		printf("colour: building BC1 cache %s (one-time) ...\n", path.c_str());
		fflush(stdout);
		if (!build_cache(overlayDir, path)) { return false; }
		buildSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}
	file_.assign(level_offset(kMips), 0);
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) { return false; }
	const bool ok = fread(file_.data(), 1, file_.size(), f) == file_.size();
	fclose(f);
	Header h;
	memcpy(&h, file_.data(), sizeof h);
	if (!ok || memcmp(h.magic, "KCOL", 4) != 0 || h.version != kVersion || h.size != (uint32_t)kSize || h.mips != (uint32_t)kMips) { return false; }
	cacheBytes = file_.size();
	{
		// CPU copy of mip kLowMip (albedoAt): BC1 colour blocks, row-major, 8 bytes each (two RGB565 endpoints, 2-bit indices)
		const int n = kSize >> kLowMip, bpr = n / 4;
		const uint8_t* lv = file_.data() + level_offset(kLowMip);
		low_.assign((size_t)n * n * 3, 0);
		for (int by = 0; by < bpr; by++)
		{
			for (int bx = 0; bx < bpr; bx++)
			{
				const uint8_t* b = lv + ((size_t)by * bpr + bx) * 8;
				const unsigned e0 = b[0] | b[1] << 8, e1 = b[2] | b[3] << 8;
				int pal[4][3];
				for (int e = 0; e < 2; e++)
				{
					const unsigned v = e ? e1 : e0;
					pal[e][0] = (int)((v >> 11) * 255 / 31); pal[e][1] = (int)(((v >> 5) & 63) * 255 / 63); pal[e][2] = (int)((v & 31) * 255 / 31);
				}
				for (int c = 0; c < 3; c++)
				{
					pal[2][c] = e0 > e1 ? (2 * pal[0][c] + pal[1][c]) / 3 : (pal[0][c] + pal[1][c]) / 2;
					pal[3][c] = e0 > e1 ? (pal[0][c] + 2 * pal[1][c]) / 3 : 0;
				}
				const uint32_t bits = b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24;
				for (int i = 0; i < 16; i++)
				{
					const int* px = pal[(bits >> (2 * i)) & 3];
					uint8_t* o = &low_[((size_t)(by * 4 + i / 4) * n + bx * 4 + i % 4) * 3];
					for (int c = 0; c < 3; c++) { o[c] = (uint8_t)px[c]; }
				}
			}
		}
	}
	// alpha mosaic; failure is not fatal (a 1x1 opaque texture stands in: gloss multiplier 1)
	char aname[64];
	snprintf(aname, sizeof aname, "/colour-%016llx.a8", (unsigned long long)key);
	const std::string apath = cache_dir() + aname;
	if (!fs::exists(apath, ec))
	{
		printf("colour: building alpha cache %s (one-time) ...\n", apath.c_str());
		fflush(stdout);
		build_alpha_cache(overlayDir, apath);
	}
	alpha_.assign(alpha_level_offset(kAlphaMips), 0);
	FILE* af = fopen(apath.c_str(), "rb");
	alphaOk_ = af && fread(alpha_.data(), 1, alpha_.size(), af) == alpha_.size();
	if (af) { fclose(af); }
	return true;
}

bool GroundColour::upload()	// the sokol calls, on the main thread: ~60 ms the colour image, ~30 the alpha mosaic (measured, kenshi-k16.2.26)
{
	if (file_.empty()) { return false; }
	int skip = 0;	// drop top mips when the GPU can't hold 16384^2
	while ((kSize >> skip) > sg_query_limits().max_image_size_2d) { skip++; }
	uploadedSize = kSize >> skip;
	sg_image_desc id = {};
	id.width = uploadedSize;
	id.height = uploadedSize;
	id.num_mipmaps = kMips - skip;
	id.pixel_format = SG_PIXELFORMAT_BC1_RGBA;
	id.data = { file_.data() + level_offset(skip), file_.size() - level_offset(skip) };
	id.label = "ground-colour";
	img = sg_make_image(&id);
	if (sg_query_image_state(img) != SG_RESOURCESTATE_VALID) { return false; }
	sg_view_desc vd = {};
	vd.texture.image = img;
	view = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.max_anisotropy = 8;
	smp = sg_make_sampler(&sd);

	sg_image_desc ad = {};
	const uint8_t one = 255;
	ad.width = alphaOk_ ? kAlphaSize : 1;
	ad.height = alphaOk_ ? kAlphaSize : 1;
	ad.num_mipmaps = alphaOk_ ? kAlphaMips : 1;
	ad.pixel_format = SG_PIXELFORMAT_R8;
	ad.data = alphaOk_ ? sg_range{ alpha_.data(), alpha_.size() } : sg_range{ &one, 1 };
	ad.label = "ground-colour-alpha";
	alphaImg = sg_make_image(&ad);
	sg_view_desc avd = {};
	avd.texture.image = alphaImg;
	alphaView = sg_make_view(&avd);
	std::vector<uint8_t>().swap(file_);	// ~50 MB of cache bytes: the low mip is its own copy and the GPU has them now
	std::vector<uint8_t>().swap(alpha_);
	return true;
}

bool GroundColour::init(const std::string& overlayDir) { return load(overlayDir) && upload(); }

void GroundColour::shutdown()
{
	sg_destroy_view(alphaView);
	sg_destroy_image(alphaImg);
	sg_destroy_sampler(smp);
	sg_destroy_view(view);
	sg_destroy_image(img);
}

bool GroundColour::albedoAt(double x, double z, float rgb[3]) const
{
	if (low_.empty()) { return false; }
	const int n = kSize >> kLowMip;
	// the terrain's v_uv: (world + 147456) / 294912 over the whole map, texel centres at (i + 0.5) / n
	const double u = std::clamp((x + 147456.0) / 294912.0 * n - 0.5, 0.0, n - 1.0), v = std::clamp((z + 147456.0) / 294912.0 * n - 0.5, 0.0, n - 1.0);
	const int i0 = (int)u, j0 = (int)v, i1 = std::min(i0 + 1, n - 1), j1 = std::min(j0 + 1, n - 1);
	const double fu = u - i0, fv = v - j0;
	for (int c = 0; c < 3; c++)
	{
		auto at = [&](int i, int j) { return low_[((size_t)j * n + i) * 3 + c] / 255.0; };
		rgb[c] = (float)((at(i0, j0) * (1 - fu) + at(i1, j0) * fu) * (1 - fv) + (at(i0, j1) * (1 - fu) + at(i1, j1) * fu) * fv);
	}
	return true;
}
