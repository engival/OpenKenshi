#include "render/detail.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <thread>

#include "assets/dds.hpp"
#include "kworld/gamedata.hpp"
#include "stb_image.h"

namespace
{

constexpr int kThreads = 6;
constexpr int kBlendSize = 1024;
constexpr int kCells = 32;
constexpr int kTableWidth = 15;
constexpr double kHalfWorld = 147456.0;
constexpr double kBlendTexel = 2.0 * kHalfWorld / kBlendSize;	// 288 units
const char* const kSlot[6] = { "base", "slope", "vertical", "grass", "dirt", "road" };

template <class F> void parallel(int count, F fn)
{
	std::atomic<int> next{ 0 };
	std::vector<std::thread> pool;
	for (int t = 0; t < kThreads; t++) { pool.emplace_back([&]() { for (int i; (i = next++) < count;) { fn(i); } }); }
	for (auto& th : pool) { th.join(); }
}

std::vector<unsigned char> load_png(const std::string& path, int& w, int& h)
{
	int comp = 0;
	unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (!px) { return {}; }
	std::vector<unsigned char> out(px, px + (size_t)w * h * 4);
	stbi_image_free(px);
	return out;
}

size_t level_bytes(int w, int h, int blockBytes) { return (size_t)std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * blockBytes; }

struct BiomeRow
{
	float v[kTableWidth][4];
};

struct LayerSet
{
	std::unordered_map<std::string, int> index;	// resolved path -> layer (0 = neutral fallback)
	std::vector<std::string> paths{ "" };
	int add(const std::string& p)
	{
		if (p.empty()) { return 0; }
		auto it = index.find(p);
		if (it != index.end()) { return it->second; }
		paths.push_back(p);
		return index[p] = (int)paths.size() - 1;
	}
};

// Builds a texture array of kLayerSize^2 layers from 2048^2 BC DDS files by dropping their top mip (no recompression). Layer 0 is a
// constant block (neutral grey / flat normal) used for empty paths.
std::vector<uint8_t> read_array(const LayerSet& set, bool isNormal, const char* label)
{
	const int size = TerrainDetail::kLayerSize;
	const int bb = isNormal ? 8 : 16;
	int levels = 0;
	for (int s = size; s >= 1; s >>= 1) { levels++; }
	const int layers = (int)set.paths.size();
	std::vector<size_t> lsize(levels), loff(levels);
	for (int l = 0; l < levels; l++) { lsize[l] = level_bytes(size >> l, size >> l, bb); }
	std::vector<std::vector<uint8_t>> data(levels);
	for (int l = 0; l < levels; l++) { data[l].resize(lsize[l] * layers); }
	// constant blocks for layer 0
	uint8_t block[16] = {};
	if (isNormal) { block[0] = 0x1F; block[1] = 0x84; }	// both endpoints 565(128,128,255)... r=16,g=32,b=31 = 0x841F
	else { block[0] = 255; block[1] = 255; block[8] = 0x10; block[9] = 0x84; block[10] = 0x10; block[11] = 0x84; }	// alpha 255, colour 565(128,128,128)
	for (int l = 0; l < levels; l++)
	{
		for (size_t o = 0; o < lsize[l]; o += (size_t)bb) { memcpy(&data[l][o], block, (size_t)bb); }
	}
	std::atomic<int> failed{ 0 };
	parallel(layers - 1, [&](int i)
	{
		const int layer = i + 1;
		std::ifstream in(set.paths[layer], std::ios::binary);
		uint8_t hdr[128];
		bool good = in && in.read((char*)hdr, 128) && memcmp(hdr, "DDS ", 4) == 0;
		uint32_t w = 0, h = 0, mips = 0;
		memcpy(&h, hdr + 12, 4);
		memcpy(&w, hdr + 16, 4);
		memcpy(&mips, hdr + 28, 4);
		good = good && memcmp(hdr + 84, isNormal ? "DXT1" : "DXT5", 4) == 0 && w == 2048 && h == 2048 && mips >= (uint32_t)levels + 1;
		if (!good) { failed++; return; }
		size_t skip = level_bytes(2048, 2048, bb);
		size_t total = 0;
		for (int l = 0; l < levels; l++) { total += lsize[l]; }
		std::vector<uint8_t> raw(total);
		in.seekg((std::streamoff)(128 + skip));
		if (!in.read((char*)raw.data(), (std::streamsize)total)) { failed++; return; }
		size_t off = 0;
		for (int l = 0; l < levels; l++)
		{
			memcpy(&data[l][lsize[l] * layer], &raw[off], lsize[l]);
			off += lsize[l];
		}
	});
	if (failed) { fprintf(stderr, "kclient: detail: %d %s textures unreadable (expected 2048^2 %s with %d+ mips)\n", failed.load(), label, isNormal ? "DXT1" : "DXT5", levels + 1); }
	std::vector<uint8_t> blob;
	blob.reserve(lsize[0] * layers * 4 / 3);
	for (int l = 0; l < levels; l++) { blob.insert(blob.end(), data[l].begin(), data[l].end()); }
	return blob;
}

sg_image make_array(const std::vector<uint8_t>& blob, int layers, bool isNormal, const char* label, sg_view* view)
{
	const int size = TerrainDetail::kLayerSize;
	int levels = 0;
	for (int s = size; s >= 1; s >>= 1) { levels++; }
	sg_image_desc id = {};
	id.type = SG_IMAGETYPE_ARRAY;
	id.width = id.height = size;
	id.num_slices = layers;
	id.num_mipmaps = levels;
	id.pixel_format = isNormal ? SG_PIXELFORMAT_BC1_RGBA : SG_PIXELFORMAT_BC3_RGBA;
	id.data = { blob.data(), blob.size() };
	id.label = label;
	const sg_image img = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = img;
	*view = sg_make_view(&vd);
	return img;
}

// Box-filtered RGBA8 mip chain, tightly packed, largest first.
void build_mips(std::vector<uint8_t>& all, int w, int levels)
{
	size_t off = 0;
	for (int l = 1; l < levels; l++)
	{
		const int lw = w >> (l - 1), nw = lw / 2;
		const size_t dst = off + (size_t)lw * lw * 4;
		all.resize(dst + (size_t)nw * nw * 4);
		const uint8_t* src = all.data() + off;
		uint8_t* out = all.data() + dst;
		parallel(nw, [&](int y)
		{
			for (int x = 0; x < nw; x++)
			{
				for (int c = 0; c < 4; c++)
				{
					const uint8_t* p = src + ((size_t)(2 * y) * lw + 2 * x) * 4 + c;
					out[((size_t)y * nw + x) * 4 + c] = (uint8_t)((p[0] + p[4] + p[(size_t)lw * 4] + p[(size_t)lw * 4 + 4] + 2) >> 2);
				}
			}
		});
		off = dst;
	}
}

}

bool TerrainDetail::init(const std::string& dataDir)
{
	Prepared p = prepare(dataDir);
	return upload(p);
}

TerrainDetail::Prepared TerrainDetail::prepare(const std::string& dataDir)
{
	Prepared out;
	out.t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	const std::string root = dataDir + "/..";
	const std::string landDir = dataDir + "/newland/land";

	// blend map, biome map, blendinfo palette
	int bw = 0, bh = 0, mw = 0, mh = 0;
	const std::vector<uint8_t> blend = load_png(landDir + "/blendmap.png", bw, bh);
	const std::vector<uint8_t> bmap = load_png(landDir + "/biomemap.png", mw, mh);
	if (blend.empty() || bmap.empty() || bw != kBlendSize || bh != kBlendSize || mw != kBlendSize || mh != kBlendSize)
	{
		fprintf(stderr, "kclient: detail: blendmap.png / biomemap.png missing or not 1024^2\n");
		return out;
	}
	slots_.resize((size_t)kBlendSize * kBlendSize);
	for (size_t i = 0; i < slots_.size(); i++)
	{
		const uint8_t* p = &blend[i * 4];
		slots_[i] = p[0] == 255 && !p[1] && !p[2] && !p[3] ? 0 : p[1] == 255 && !p[0] && !p[2] && !p[3] ? 1 : p[2] == 255 && !p[0] && !p[1] && !p[3] ? 2 : p[3] == 255 && !p[0] && !p[1] && !p[2] ? 3 : 4;
	}
	{
		std::ifstream in(landDir + "/blendinfo.dat", std::ios::binary);
		char magic[4];
		int32_t n = 0, r = 0;
		if (!in || !in.read(magic, 4) || memcmp(magic, "KBI1", 4) != 0 || !in.read((char*)&n, 4) || !in.read((char*)&r, 4) || n != kCells)
		{
			fprintf(stderr, "kclient: detail: blendinfo.dat unreadable\n");
			return out;
		}
		keys_.resize((size_t)kCells * kCells * 5);
		in.read((char*)keys_.data(), (std::streamsize)keys_.size() * 4);
		if (!in) { return out; }
	}

	// biome records
	LayerSet diff, nrm;
	std::unordered_map<unsigned, std::array<uint8_t, 4>> groundOfKey;
	std::vector<BiomeRow> rows;
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		// the last BIOMES record with an index wins (water.cpp does the same)
		std::unordered_map<unsigned, const kdata::Record*> byKey;
		for (const kdata::Record* rec : gd.byType(kworld::itemTypeByName("BIOMES")))
		{
			if (rec->findInt("index")) { byKey[(unsigned)rec->findInt("index")->value] = rec; }
		}
		// only biomes the blend palette can reach need layers
		std::unordered_map<unsigned, bool> used;
		for (unsigned k : keys_) { if (k) { used[k] = true; } }
		for (const auto& [key, rec] : byKey)
		{
			const unsigned g = (unsigned)rec->intOr("ground colour", 0x808080);
			groundOfKey[key] = { (uint8_t)(g >> 16), (uint8_t)(g >> 8), (uint8_t)g, 255 };
			if (!used.count(key)) { continue; }
			BiomeRow row = {};
			auto f = [&](const char* name, float d) { return rec->floatOr(name, d); };
			auto set = [&](int slot, float a, float b, float c, float d) { row.v[slot][0] = a; row.v[slot][1] = b; row.v[slot][2] = c; row.v[slot][3] = d; };
			set(0, f("tiling X 0", 50), f("tiling Y 0", 50), f("tiling X grass", 40), f("tiling Y grass", 40));
			set(1, f("tiling X 1", 70), f("tiling Y 1", 70), f("tiling X 2", 40), f("tiling Y 2", 10));
			set(2, f("tiling X dirt", 40), f("tiling Y dirt", 40), f("tiling X road", 40), f("tiling Y road", 40));
			set(3, f("slope min 1", 0) * 0.01f, f("slope min 2", 0) * 0.01f, 0, 0);
			set(4, f("slope max 1", 90) * 0.01f, f("slope max 2", 90) * 0.01f, 0, 0);
			set(5, f("slope fade 1", 5) * 0.01f, f("slope fade 2", 5) * 0.01f, 0, 0);
			set(6, f("overlay mult vertical", 1), f("overlay mult grass", 1), f("overlay mult dirt", 1), f("overlay mult road", 1));
			// the records carry the same six values everywhere (gamedata.base 15 / 15, Newwworld.mod 60 / 72, rebirth.mod overrides leave them out and inherit); the fallback is that value
			set(13, f("absorbance 0", 0.55f), f("absorbance 1", 0.2f), f("absorbance 2", 0.5f), f("absorbance grass", 0.44f));
			set(14, f("absorbance dirt", 0.3f), f("absorbance road", 0.25f), 0, 0);
			const float fade = f("fade distance", 4000);
			set(7, 0, 0, 0, fade > 0 ? 1.0f / fade : 0.0f);
			const float bf = f("brightness fix", 1), wl = f("distort wavelength", 1000);
			set(8, bf == 0 ? 1.0f : bf, wl > 0 ? 1.0f / wl : 0.0f, f("distort amplitude", 0) * 0.01f, 0);
			for (int s = 0; s < 6; s++)
			{
				const std::string name = std::string("texture ") + kSlot[s];
				const int d = diff.add(resolve_ci(root, rec->fileOr(name)));
				const int n = nrm.add(resolve_ci(root, rec->fileOr(name + " normal")));
				row.v[9 + s / 4 * 1 + 0][s % 4] = (float)d;
				row.v[11 + s / 4][s % 4] = (float)n;
			}
			rowOfKey_[key] = (int)rows.size();
			rows.push_back(row);
		}
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "kclient: detail: BIOMES not loaded (%s)\n", e.what());
		return out;
	}
	biomeCount = (int)rows.size();
	layersDiffuse = (int)diff.paths.size();
	layersNormal = (int)nrm.paths.size();
	{
		int lim = sg_query_limits().max_image_array_layers;
		if (layersDiffuse > lim || layersNormal > lim) { fprintf(stderr, "kclient: detail: %d/%d layers exceed the GPU limit %d\n", layersDiffuse, layersNormal, lim); return out; }
	}

	// ground colour map: per biomemap texel the biome's ground colour (the game's distant.png equivalent)
	std::vector<uint8_t> ground((size_t)kBlendSize * kBlendSize * 4);
	for (size_t i = 0; i < (size_t)kBlendSize * kBlendSize; i++)
	{
		auto it = groundOfKey.find((unsigned)bmap[i * 4] << 16 | (unsigned)bmap[i * 4 + 1] << 8 | bmap[i * 4 + 2]);
		const std::array<uint8_t, 4> c = it == groundOfKey.end() ? std::array<uint8_t, 4>{ 128, 118, 100, 255 } : it->second;
		memcpy(&ground[i * 4], c.data(), 4);
	}

	// overlay tiles -> one 8192^2 RGBA8 image with mips
	int levels = 0;
	for (int s = kOverlaySize; s >= 1; s >>= 1) { levels++; }
	std::vector<uint8_t>& ov = out.overlay;
	ov.resize((size_t)kOverlaySize * kOverlaySize * 4);
	std::atomic<int> bad{ 0 };
	parallel(64, [&](int t)
	{
		const int tx = t % 8, ty = t / 8, ts = kOverlaySize / 8;
		int w = 0, h = 0;
		const std::vector<uint8_t> px = load_png(landDir + "/overlaymaps/new_overlay." + std::to_string(tx) + "." + std::to_string(ty) + ".png", w, h);
		if (px.empty() || w != ts || h != ts) { bad++; return; }
		for (int y = 0; y < ts; y++) { memcpy(&ov[((size_t)(ty * ts + y) * kOverlaySize + tx * ts) * 4], &px[(size_t)y * ts * 4], (size_t)ts * 4); }
	});
	if (bad) { fprintf(stderr, "kclient: detail: %d overlay tiles missing (treated as empty)\n", bad.load()); }
	build_mips(ov, kOverlaySize, levels);

	// array blobs, table
	out.diffBlob = read_array(diff, false, "detail-diffuse");
	out.nrmBlob = read_array(nrm, true, "detail-normal");
	out.table.resize((size_t)rows.size() * kTableWidth * 4);
	for (size_t r = 0; r < rows.size(); r++) { memcpy(&out.table[r * kTableWidth * 4], rows[r].v, sizeof rows[r].v); }
	out.rows = (int)rows.size();
	out.blend = blend;
	out.ground = std::move(ground);
	out.ok = true;
	return out;
}

int TerrainDetail::uploadPart(Prepared& p, int part)	// the uploads in three parts, so no frame blocks for the whole lot (kenshi-k16.2.26:
// measured 26 / 17 ms the two arrays, 101 the 8192^2 overlay, ~35 the rest). 0: the arrays, 1: the overlay, 2: the rest and the samplers.
// Returns 1 when more parts remain, 0 when done, -1 on failure. p must stay alive until done (Terrain::initDetailPart).
{
	if (!p.ok) { return -1; }
	const int levels = [] { int n = 0; for (int s = kOverlaySize; s >= 1; s >>= 1) { n++; } return n; }();
	std::vector<uint8_t>& ov = p.overlay;
	auto make2d = [&](int idx, const void* px, size_t bytes, int w, int h, int mips, sg_pixel_format fmt, const char* label, sg_view* view, const sg_range* all)
	{
		sg_image_desc id = {};
		id.width = w;
		id.height = h;
		id.num_mipmaps = mips;
		id.pixel_format = fmt;
		id.data = all ? *all : sg_range{ px, bytes };
		id.label = label;
		images_[idx] = sg_make_image(&id);
		sg_view_desc vd = {};
		vd.texture.image = images_[idx];
		*view = sg_make_view(&vd);
	};
	if (part == 0)
	{
		images_[0] = make_array(p.diffBlob, layersDiffuse, false, "detail-diffuse", &diffuse);
		images_[1] = make_array(p.nrmBlob, layersNormal, true, "detail-normal", &normal);
		return 1;
	}
	if (part == 1)
	{
		int skip = 0;
		while ((kOverlaySize >> skip) > sg_query_limits().max_image_size_2d) { skip++; }
		size_t skipBytes = 0;
		for (int l = 0; l < skip; l++) { skipBytes += (size_t)(kOverlaySize >> l) * (kOverlaySize >> l) * 4; }
		make2d(2, ov.data() + skipBytes, ov.size() - skipBytes, kOverlaySize >> skip, kOverlaySize >> skip, levels - skip, SG_PIXELFORMAT_RGBA8, "detail-overlay", &overlay, nullptr);
		return 1;
	}
	make2d(3, p.blend.data(), p.blend.size(), kBlendSize, kBlendSize, 1, SG_PIXELFORMAT_RGBA8, "detail-blendmap", &blendView, nullptr);
	make2d(4, p.table.data(), p.table.size() * sizeof(float), kTableWidth, (int)p.rows, 1, SG_PIXELFORMAT_RGBA32F, "detail-biomes", &biomeTable, nullptr);
	make2d(5, p.ground.data(), p.ground.size(), kBlendSize, kBlendSize, 1, SG_PIXELFORMAT_RGBA8, "detail-ground", &groundView, nullptr);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;
	sd.max_anisotropy = 8;
	arraySmp = sg_make_sampler(&sd);
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	linearSmp = sg_make_sampler(&sd);
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.mipmap_filter = SG_FILTER_NEAREST;
	sd.max_anisotropy = 1;
	tableSmp = sg_make_sampler(&sd);
	loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - p.t0;
	ok = true;
	return 0;
}

bool TerrainDetail::upload(Prepared& p)	// all parts at once (init(); the loading steps use uploadPart)
{
	for (int part = 0; part < 3; part++) { if (uploadPart(p, part) < 0) { return false; } }
	return ok;
}

void TerrainDetail::shutdown()
{
	if (!ok) { return; }
	sg_destroy_sampler(arraySmp);
	sg_destroy_sampler(linearSmp);
	sg_destroy_sampler(tableSmp);
	const sg_view views[6] = { diffuse, normal, overlay, blendView, biomeTable, groundView };
	for (int i = 0; i < 6; i++)
	{
		sg_destroy_view(views[i]);
		sg_destroy_image(images_[i]);
	}
	ok = false;
}

TerrainDetail::ChunkBiomes TerrainDetail::biomesFor(double x0, double z0, double size) const
{
	ChunkBiomes out;
	if (!ok || size > kMaxChunkSize) { return out; }
	// blend texels the chunk's bilinear samples can touch (one texel margin), within the chunk's own blendinfo cell
	auto texel = [](double w) { return (int)std::floor((w + kHalfWorld) / kBlendTexel); };
	const int tx0 = std::max(0, texel(x0) - 1), tx1 = std::min(kBlendSize - 1, texel(x0 + size) + 1);
	const int tz0 = std::max(0, texel(z0) - 1), tz1 = std::min(kBlendSize - 1, texel(z0 + size) + 1);
	const int cell = std::clamp(texel(z0 + size * 0.5) / 32, 0, kCells - 1) * kCells + std::clamp(texel(x0 + size * 0.5) / 32, 0, kCells - 1);
	unsigned mask = 0;
	for (int z = tz0; z <= tz1; z++)
	{
		for (int x = tx0; x <= tx1; x++) { mask |= 1u << slots_[(size_t)z * kBlendSize + x]; }
	}
	for (int slot = 4; slot >= 0 && out.count < kMaxChunkBiomes; slot--)
	{
		if (!(mask >> slot & 1)) { continue; }
		auto it = rowOfKey_.find(keys_[(size_t)cell * 5 + slot]);
		if (it == rowOfKey_.end()) { continue; }
		out.row[out.count] = (float)it->second;
		out.chan[out.count] = (float)slot;
		out.count++;
	}
	return out;
}

float TerrainDetail::rowAt(double x, double z) const
{
	if (!ok) { return 0.0f; }
	const int tx = std::clamp((int)std::floor((x + kHalfWorld) / kBlendTexel), 0, kBlendSize - 1);
	const int tz = std::clamp((int)std::floor((z + kHalfWorld) / kBlendTexel), 0, kBlendSize - 1);
	const auto it = rowOfKey_.find(keys_[(size_t)(tz / 32 * kCells + tx / 32) * 5 + slots_[(size_t)tz * kBlendSize + tx]]);
	return it == rowOfKey_.end() ? 0.0f : (float)it->second;
}
