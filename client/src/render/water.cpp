#include "render/water.hpp"
#include "render/drawstats.hpp"
#include "render/speccube.hpp"
#include "world/weathermap.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <string_view>
#include <unordered_map>

#include <strings.h>

#include "assets/dds.hpp"
#include "kworld/coords.hpp"	// kWorldMin: the chunk's world square for the coverage estimate
#include "kworld/gamedata.hpp"
#include "stb_image.h"
#include "water.glsl.h"

namespace
{

enum { kColour, kFlow, kNormal, kScumDif, kScumNml, kIndex, kTable, kTurb, kRain };
constexpr int kBiomeSize = 1024;
constexpr int kScumSize = 1024;		// scum layers: the 2048^2 files minus their top mip
constexpr int kScumLevels = 11;		// 1024 .. 1
constexpr int kTableWidth = 4;		// texels per biome row (water.glsl t0..t3)
constexpr int kNormalSize = 1024;	// water normal layers (the default water.png is 2048^2, Watercalm 512^2)
constexpr int kTurbSize = 512;		// turbulence layers

// Loads a PNG as RGBA8; empty on failure.
std::vector<unsigned char> load_png(const std::string& path, int& w, int& h)
{
	int comp = 0;
	unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (!px) { return {}; }
	std::vector<unsigned char> out(px, px + (size_t)w * h * 4);
	stbi_image_free(px);
	return out;
}

// An image as given (no mips built) and its view; slices > 0: a texture array.
sg_image make_plain(const void* data, size_t size, int w, int h, int slices, int mips, sg_pixel_format fmt, const char* label, sg_view* view)
{
	sg_image_desc id = {};
	id.type = slices > 0 ? SG_IMAGETYPE_ARRAY : SG_IMAGETYPE_2D;
	id.width = w;
	id.height = h;
	id.num_slices = std::max(slices, 1);
	id.num_mipmaps = mips;
	id.pixel_format = fmt;
	id.data = { data, size };
	id.label = label;
	const sg_image img = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = img;
	*view = sg_make_view(&vd);
	return img;
}

// Box-filtered mip chain of an RGBA8 image, tightly packed, largest first (just the image with mips = false).
std::vector<unsigned char> mip_chain(const unsigned char* rgba, int w, int h, bool mips, int& levels)
{
	std::vector<unsigned char> all(rgba, rgba + (size_t)w * h * 4);
	levels = 1;
	for (int lw = w, lh = h, off = 0; mips && (lw > 1 || lh > 1); levels++)
	{
		const int nw = std::max(1, lw / 2), nh = std::max(1, lh / 2);
		all.resize(all.size() + (size_t)nw * nh * 4);
		for (int y = 0; y < nh; y++)
		{
			for (int x = 0; x < nw; x++)
			{
				for (int c = 0; c < 4; c++)
				{
					int s = 0;
					for (int k = 0; k < 4; k++) { s += all[off + (((size_t)std::min(lh - 1, y * 2 + (k >> 1)) * lw + std::min(lw - 1, x * 2 + (k & 1)))) * 4 + c]; }
					all[off + (size_t)lw * lh * 4 + ((size_t)y * nw + x) * 4 + c] = (unsigned char)((s + 2) / 4);
				}
			}
		}
		off += lw * lh * 4;
		lw = nw;
		lh = nh;
	}
	return all;
}

sg_image make_rgba(const unsigned char* rgba, int w, int h, bool mips, const char* label, sg_view* view)
{
	int levels = 0;
	const std::vector<unsigned char> all = mip_chain(rgba, w, h, mips, levels);
	return make_plain(all.data(), all.size(), w, h, 0, levels, SG_PIXELFORMAT_RGBA8, label, view);
}

// A texture file as one size^2 RGBA8 layer: a PNG, or a BC DDS from its mip nearest above size (decoded); then box-halved or bilinearly doubled
// (wrapping: the layers tile) to size. Square power-of-two sources only. Empty with err set on failure.
std::vector<unsigned char> load_layer(const std::string& path, int size, std::string& err)
{
	std::vector<unsigned char> px;
	int w = 0, h = 0;
	if (path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".dds") == 0)
	{
		DdsImage d;
		if (!dds_load(path, d, err)) { return {}; }
		int level = 0;
		while (level + 1 < d.mips && (d.width >> (level + 1)) >= size) { level++; }
		if (!dds_decode(d, level, px, w, h)) { err = "not BC1/BC2/BC3"; return {}; }
	} else {
		px = load_png(path, w, h);
		if (px.empty()) { err = "unreadable PNG"; return {}; }
	}
	if (w != h || (w & (w - 1)) != 0) { err = "not a square power of two (" + std::to_string(w) + "x" + std::to_string(h) + ")"; return {}; }
	while (w > size)
	{
		int levels = 0;
		std::vector<unsigned char> half = mip_chain(px.data(), w, w, true, levels);
		px.assign(half.begin() + (ptrdiff_t)((size_t)w * w * 4), half.begin() + (ptrdiff_t)((size_t)w * w * 5));
		w /= 2;
	}
	while (w < size)
	{
		std::vector<unsigned char> dbl((size_t)w * w * 16);
		for (int y = 0; y < 2 * w; y++)
		{
			for (int x = 0; x < 2 * w; x++)
			{
				// the new texel centre in source texels: (x + 0.5) / 2 - 0.5, so a pair of source texels at 1/4 and 3/4
				const int x0 = (x - 1) / 2 + (x == 0 ? -1 : 0), y0 = (y - 1) / 2 + (y == 0 ? -1 : 0);
				const int fx = (x & 1) ? 1 : 3, fy = (y & 1) ? 1 : 3;	// weight of the second texel x4
				for (int c = 0; c < 4; c++)
				{
					auto at = [&](int sx, int sy) { return (int)px[(((size_t)((sy + w) % w) * w) + (size_t)((sx + w) % w)) * 4 + c]; };
					const int v = (4 - fx) * (4 - fy) * at(x0, y0) + fx * (4 - fy) * at(x0 + 1, y0) + (4 - fx) * fy * at(x0, y0 + 1) + fx * fy * at(x0 + 1, y0 + 1);
					dbl[((size_t)y * 2 * w + x) * 4 + c] = (unsigned char)((v + 8) / 16);
				}
			}
		}
		px.swap(dbl);
		w *= 2;
	}
	return px;
}

// Texture array layers built on the worker thread: one per distinct image (by content), each with its mip chain; blob() packs them mip-major.
struct LayerArray
{
	int size = 0;
	std::vector<std::vector<unsigned char>> chains;
	std::unordered_map<size_t, int> byHash;
	std::unordered_map<std::string, int> byPath;	// -1: failed (reported once)

	int add(const std::string& path, const char* what)
	{
		auto it = byPath.find(path);
		if (it != byPath.end()) { return it->second; }
		std::string err;
		const std::vector<unsigned char> px = load_layer(path, size, err);
		int layer = -1;
		if (px.empty()) { fprintf(stderr, "kclient: water: %s %s: %s\n", what, path.c_str(), err.c_str()); }
		else
		{
			const size_t h = std::hash<std::string_view>{}(std::string_view((const char*)px.data(), px.size()));
			auto same = byHash.find(h);
			if (same != byHash.end()) { layer = same->second; }
			else
			{
				int levels = 0;
				chains.push_back(mip_chain(px.data(), size, size, true, levels));
				layer = byHash[h] = (int)chains.size() - 1;
			}
		}
		return byPath[path] = layer;
	}
	std::vector<unsigned char> blob() const
	{
		std::vector<unsigned char> out;
		size_t off = 0;
		for (int s = size; s >= 1; s >>= 1)
		{
			const size_t n = (size_t)s * s * 4;
			for (const std::vector<unsigned char>& c : chains) { out.insert(out.end(), c.begin() + (ptrdiff_t)off, c.begin() + (ptrdiff_t)(off + n)); }
			off += n;
		}
		return out;
	}
};

int levels_of(int size)
{
	int n = 0;
	for (int s = size; s >= 1; s >>= 1) { n++; }
	return n;
}

// One biome's water uniforms as the exe sets them on its material (WATER.md 2.1, C230-C270), the fcs.def defaults without a record (r = nullptr):
// t0 gloss, invOpacity = 1 / max(10 visibility, 1), invStrength = 1 / max(strength, 1e-4), glow; t1 the VP scale = (water scale X, Y, scum scale X, Y) / 5000;
// t2 distortion = (water distortion, scum distortion / mean scum scale), the scum layer (-1: none), the normal layer; t3 the turbulence layer (-1: none).
void water_row(const kdata::Record* r, float scumLayer, float normalLayer, float turbLayer, float* out)
{
	auto f = [r](const char* key, float dflt) { return r ? r->floatOr(key, dflt) : dflt; };
	const float scumX = f("scum scale X", 20.0f), scumY = f("scum scale Y", 20.0f);
	const float row[kTableWidth * 4] = {
		f("water gloss", 0.9f), 1.0f / std::max(10.0f * f("water visibility", 10.0f), 1.0f), 1.0f / std::max(f("water strength", 1.0f), 1e-4f), f("water glow", 0.0f),
		f("water scale X", 20.0f) / 5000.0f, f("water scale Y", 20.0f) / 5000.0f, scumX / 5000.0f, scumY / 5000.0f,
		f("water distortion", 120.0f), f("scum distortion", 1.0f) / std::max((scumX + scumY) * 0.5f, 1e-4f), scumLayer, normalLayer,
		turbLayer, 0.0f, 0.0f, 0.0f };
	memcpy(out, row, sizeof row);
}

// Appends a scum DDS's levels from kScumSize down to the per-level arrays as BC3. A BC1 file gets an opaque alpha block per colour block; BC3 decodes every colour
// block in 4-colour mode, so a BC1 block that uses the 3-colour codes (c0 <= c1 with index 2 or 3) would change: those are counted and reported, the game's files have none.
bool scum_levels(const DdsImage& d, const std::string& path, std::vector<std::vector<uint8_t>>& levels)
{
	const bool bc1 = d.format == SG_PIXELFORMAT_BC1_RGBA;
	if ((!bc1 && d.format != SG_PIXELFORMAT_BC3_RGBA) || d.width != d.height || d.width < kScumSize || (d.width & (d.width - 1)) != 0)
	{
		fprintf(stderr, "kclient: water: scum %s is %dx%d format %d, needs a square power of two >= %d in BC1 or BC3; biome left without scum\n", path.c_str(), d.width, d.height, (int)d.format, kScumSize);
		return false;
	}
	int skip = 0;
	for (int w = d.width; w > kScumSize; w >>= 1) { skip++; }
	if (d.mips < skip + kScumLevels)
	{
		fprintf(stderr, "kclient: water: scum %s has %d mips, needs %d; biome left without scum\n", path.c_str(), d.mips, skip + kScumLevels);
		return false;
	}
	const size_t bb = bc1 ? 8 : 16;
	auto bytes = [bb](int w) { return (size_t)std::max(1, (w + 3) / 4) * std::max(1, (w + 3) / 4) * bb; };
	size_t off = 0;
	for (int l = 0; l < skip; l++) { off += bytes(d.width >> l); }
	int threeColour = 0;
	for (int l = 0; l < kScumLevels; l++)
	{
		const size_t n = bytes(kScumSize >> l);
		if (!bc1) { levels[l].insert(levels[l].end(), d.data.begin() + (ptrdiff_t)off, d.data.begin() + (ptrdiff_t)(off + n)); }
		for (size_t o = 0; bc1 && o < n; o += 8)
		{
			const uint8_t* blk = &d.data[off + o];
			const uint8_t alpha[8] = { 255, 255, 0, 0, 0, 0, 0, 0 };
			levels[l].insert(levels[l].end(), alpha, alpha + 8);
			levels[l].insert(levels[l].end(), blk, blk + 8);
			const unsigned c0 = blk[0] | blk[1] << 8, c1 = blk[2] | blk[3] << 8;
			const uint32_t idx = (uint32_t)blk[4] | (uint32_t)blk[5] << 8 | (uint32_t)blk[6] << 16 | (uint32_t)blk[7] << 24;
			if (c0 <= c1 && (idx & 0xAAAAAAAAu) != 0) { threeColour++; }
		}
		off += n;
	}
	if (threeColour) { fprintf(stderr, "kclient: water: scum %s: %d BC1 blocks use the 3-colour codes, drawn in 4-colour mode (BC3)\n", path.c_str(), threeColour); }
	return true;
}

Water::Bake bake(std::string dataDir)
{
	Water::Bake out;
	out.table.resize(kTableWidth * 4);
	water_row(nullptr, -1.0f, 0.0f, -1.0f, out.table.data());	// row 0: no record
	LayerArray normals, turb;
	normals.size = kNormalSize;
	turb.size = kTurbSize;
	if (normals.add(dataDir + "/materials/water.png", "default normal map") != 0) { fprintf(stderr, "kclient: water: no default normal map, the water is flat\n"); }
	int missingNormal = 0;
	std::unordered_map<uint32_t, int> rowOfIndex;
	std::vector<std::vector<uint8_t>> dif(kScumLevels), nml(kScumLevels);
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		const std::string root = dataDir + "/..";
		std::unordered_map<std::string, int> layerOf;	// "diffuse|normal" -> scum layer, -1 unusable
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("BIOMES")))
		{
			float layer = -1.0f;
			const std::string& d = r->fileOr("texture scum");
			const std::string& n = r->fileOr("texture scum normal");
			if (!d.empty() || !n.empty())
			{
				const std::string key = d + "|" + n;
				auto it = layerOf.find(key);
				if (it == layerOf.end())
				{
					int l = -1;
					const std::string paths[2] = { resolve_ci(root, d), resolve_ci(root, n) };
					DdsImage img[2];
					std::string err;
					bool good = true;
					for (int k = 0; k < 2 && good; k++)
					{
						good = !paths[k].empty() && dds_load(paths[k], img[k], err);
						if (!good) { fprintf(stderr, "kclient: water: biome '%s' scum %s unreadable (%s); left without scum\n", r->name.c_str(), (k ? n : d).c_str(), paths[k].empty() ? "no such file" : err.c_str()); }
					}
					std::vector<std::vector<uint8_t>> ld(kScumLevels), ln(kScumLevels);
					if (good && scum_levels(img[0], paths[0], ld) && scum_levels(img[1], paths[1], ln))
					{
						for (int k = 0; k < kScumLevels; k++)
						{
							dif[k].insert(dif[k].end(), ld[k].begin(), ld[k].end());
							nml[k].insert(nml[k].end(), ln[k].begin(), ln[k].end());
						}
						l = out.scumLayers++;
					}
					it = layerOf.emplace(key, l).first;
				}
				layer = (float)it->second;
			}
			// water normal: the material's water.png when empty or missing, as the exe's texture helper leaves the material default (59 biomes name
			// .\data\mainmats\water.png, which does not exist); turbulence: none (the material's flat.dds, zero) when empty, reported when missing
			int normal = 0;
			const std::string& wn = r->fileOr("water normal");
			const std::string wnPath = wn.empty() ? "" : resolve_ci(root, wn);
			if (!wnPath.empty()) { normal = std::max(normals.add(wnPath, "normal map"), 0); }
			else if (!wn.empty()) { missingNormal++; }
			int turbulence = -1;
			const std::string& tm = r->fileOr("turbulence map");
			if (!tm.empty())
			{
				const std::string tmPath = resolve_ci(root, tm);
				if (tmPath.empty()) { fprintf(stderr, "kclient: water: biome '%s' turbulence map %s: no such file; none\n", r->name.c_str(), tm.c_str()); }
				else { turbulence = turb.add(tmPath, "turbulence map"); }
			}
			for (const kdata::FieldInt& f : r->ints)
			{
				if (f.key != "index") { continue; }
				const size_t at = out.table.size();
				out.table.resize(at + kTableWidth * 4);
				water_row(r, layer, (float)normal, (float)turbulence, &out.table[at]);
				rowOfIndex[(uint32_t)f.value] = (int)(at / (kTableWidth * 4));	// the last definition wins
			}
		}
	}
	catch (const std::exception& e) { fprintf(stderr, "kclient: water: BIOMES not loaded (%s), default water everywhere\n", e.what()); }
	if (missingNormal) { fprintf(stderr, "kclient: water: %d BIOMES name a water normal that does not exist; the default water.png, as the game\n", missingNormal); }
	out.normalLayers = (int)normals.chains.size();
	out.normals = normals.blob();
	out.turbLayers = (int)turb.chains.size();
	out.turb = turb.blob();
	if (rowOfIndex.size() > 255) { fprintf(stderr, "kclient: water: %zu BIOMES rows, the index map holds 255; the rest draw default water\n", rowOfIndex.size()); }
	for (int k = 0; k < kScumLevels; k++)
	{
		out.scum[0].insert(out.scum[0].end(), dif[k].begin(), dif[k].end());
		out.scum[1].insert(out.scum[1].end(), nml[k].begin(), nml[k].end());
	}
	int w = 0, h = 0;
	std::vector<unsigned char> img = load_png(dataDir + "/newland/land/biomemap.png", w, h);
	out.index.assign((size_t)kBiomeSize * kBiomeSize, 0);
	if (w != kBiomeSize || h != kBiomeSize) { fprintf(stderr, "kclient: water: biomemap.png is %dx%d, not %d^2; default water everywhere\n", w, h, kBiomeSize); }
	for (size_t i = 0; w == kBiomeSize && h == kBiomeSize && i < out.index.size(); i++)
	{
		auto it = rowOfIndex.find((uint32_t)(img[i * 4] << 16 | img[i * 4 + 1] << 8 | img[i * 4 + 2]));
		if (it != rowOfIndex.end() && it->second <= 255) { out.index[i] = (unsigned char)it->second; }
	}
	return out;
}


}

void Water::useBake(const Bake& b)
{
	for (int i : { kNormal, kScumDif, kScumNml, kIndex, kTable, kTurb })
	{
		if (img_[i].id == SG_INVALID_ID) { continue; }
		sg_destroy_view(view_[i]);
		sg_destroy_image(img_[i]);
	}
	const int side = b.index.size() == 1 ? 1 : kBiomeSize;
	img_[kIndex] = make_plain(b.index.data(), b.index.size(), side, side, 0, 1, SG_PIXELFORMAT_R8, "water-biome-rows", &view_[kIndex]);
	img_[kTable] = make_plain(b.table.data(), b.table.size() * sizeof(float), kTableWidth, (int)(b.table.size() / (kTableWidth * 4)), 0, 1, SG_PIXELFORMAT_RGBA32F, "water-biome-table", &view_[kTable]);
	const unsigned char flat[4] = { 128, 128, 255, 255 };	// (0, 1, 0) after the swizzle: a flat layer before the bake or without water.png
	img_[kNormal] = b.normalLayers > 0 ? make_plain(b.normals.data(), b.normals.size(), kNormalSize, kNormalSize, b.normalLayers, levels_of(kNormalSize), SG_PIXELFORMAT_RGBA8, "water-normal", &view_[kNormal])
		: make_plain(flat, 4, 1, 1, 1, 1, SG_PIXELFORMAT_RGBA8, "water-normal", &view_[kNormal]);
	img_[kTurb] = b.turbLayers > 0 ? make_plain(b.turb.data(), b.turb.size(), kTurbSize, kTurbSize, b.turbLayers, levels_of(kTurbSize), SG_PIXELFORMAT_RGBA8, "water-turbulence", &view_[kTurb])
		: make_plain(flat, 4, 1, 1, 1, 1, SG_PIXELFORMAT_RGBA8, "water-turbulence", &view_[kTurb]);
	hasScum_ = b.scumLayers > 0;
	const char* labels[2] = { "water-scum", "water-scum-normal" };
	for (int k = 0; k < 2; k++)
	{
		const unsigned char none[4] = { 0, 0, 0, 0 };
		img_[kScumDif + k] = hasScum_ ? make_plain(b.scum[k].data(), b.scum[k].size(), kScumSize, kScumSize, b.scumLayers, kScumLevels, SG_PIXELFORMAT_BC3_RGBA, labels[k], &view_[kScumDif + k])
			: make_plain(none, 4, 1, 1, 1, 1, SG_PIXELFORMAT_RGBA8, labels[k], &view_[kScumDif + k]);
	}
}

Water::Prepared Water::prepare(const std::string& dataDir)	// CPU only (the PNG decodes): may run on a worker (kenshi-k16.2.26)
{
	Prepared out;
	const std::string overlay = dataDir + "/newland/land/overlaymaps/";
	out.path[0] = overlay + "watercolourmap.png";
	out.path[1] = overlay + "flowmap.png";
	for (int i = 0; i < 2; i++) { out.px[i] = load_png(out.path[i], out.w[i], out.h[i]); }
	return out;
}

bool Water::init(const std::string& dataDir) { return init(dataDir, prepare(dataDir)); }

bool Water::init(const std::string& dataDir, Prepared p)	// the sokol half, on the main thread
{
	bake_ = std::async(std::launch::async, bake, dataDir);
	const char* labels[] = { "water-colour", "water-flow" };
	for (int i = 0; i < 2; i++)
	{
		if (p.px[i].empty())
		{
			fprintf(stderr, "kclient: water texture %s missing\n", p.path[i].c_str());
			return false;
		}
		img_[i] = make_rgba(p.px[i].data(), p.w[i], p.h[i], i != kColour, labels[i], &view_[i]);
	}
	// the rain rings (water.material's rainMap, forward/water.hlsl: the packed ring map the world-space rainRipple layers sample); no mips, like the PNG
	{
		const std::string path = dataDir + "/materials/rain-ripples.png";
		int w = 0, h = 0;
		const std::vector<unsigned char> px = load_png(path, w, h);
		if (px.empty())
		{
			fprintf(stderr, "kclient: water texture %s missing\n", path.c_str());
			return false;
		}
		img_[kRain] = make_rgba(px.data(), w, h, false, "water-rain-ripples", &view_[kRain]);
	}
	// until the bake lands: one default row everywhere, no scum
	Bake none;
	none.index.assign(1, 0);
	none.table.resize(kTableWidth * 4);
	water_row(nullptr, -1.0f, 0.0f, -1.0f, none.table.data());
	useBake(none);

	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	clampSmp_ = sg_make_sampler(&sd);
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;
	sd.max_anisotropy = 8;
	repeatSmp_ = sg_make_sampler(&sd);
	sd = {};
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	nearSmp_ = sg_make_sampler(&sd);

	shd_ = sg_make_shader(water_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.attrs[ATTR_water_grid].format = SG_VERTEXFORMAT_FLOAT2;
	pd.index_type = SG_INDEXTYPE_UINT16;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = false;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.colors[0].blend.enabled = true;
	pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
	pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	pd.label = "water";
	pip_ = sg_make_pipeline(&pd);
	return true;
}

void Water::shutdown()
{
	if (bake_.valid()) { bake_.wait(); }
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
	sg_destroy_sampler(nearSmp_);
	sg_destroy_sampler(repeatSmp_);
	sg_destroy_sampler(clampSmp_);
	for (int i = 0; i < kImages; i++)
	{
		sg_destroy_view(view_[i]);
		sg_destroy_image(img_[i]);
	}
}

namespace
{
	struct ClipV { double x, y, w; };
	// The NDC area of a clip-space quad (0: empty), clipped to the front of the eye and the four sides: 4 is the whole view.
	double clipArea(std::array<ClipV, 4> in)
	{
		ClipV p[16], t[16];
		int n = 4;
		for (int k = 0; k < 4; k++) { p[k] = in[(size_t)k]; }
		for (int pl = 0; pl < 5 && n > 0; pl++)
		{
			auto f = [pl](const ClipV& v) -> double
			{
				switch (pl)
				{
				case 0: return v.w;		// the eye's plane (a point behind it has no pixel)
				case 1: return v.x + v.w;	// left
				case 2: return v.w - v.x;	// right
				case 3: return v.y + v.w;	// bottom
				default: return v.w - v.y;	// top
				}
			};
			int m = 0;
			for (int k = 0; k < n; k++)
			{
				const ClipV a = p[k], b = p[(k + 1) % n];
				const double fa = f(a), fb = f(b);
				if (fa >= 0) { t[m++] = a; }
				if ((fa >= 0) != (fb >= 0)) { const double u = fa / (fa - fb); t[m++] = { a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u, a.w + (b.w - a.w) * u }; }
			}
			n = m;
			for (int k = 0; k < n; k++) { p[k] = t[k]; }
		}
		if (n < 3) { return 0; }
		double a = 0;
		for (int k = 0; k < n; k++)
		{
			const ClipV& u = p[k];
			const ClipV& v = p[(k + 1) % n];
			a += (u.x / u.w) * (v.y / v.w) - (v.x / v.w) * (u.y / u.w);
		}
		return std::fabs(a) * 0.5;
	}
}

// The drawn water plane's share of the view (kenshi-k16.2.205.11), from this render's chunks and matrix: each chunk is a square of the plane (the LOD
// cell x Terrain::kCells); its corners are projected and clipped, and the area is scaled by the share of the square whose heightmap lies under the
// water (the shader discards the rest). Draws_ lists a chunk per visible LOD on top of each other, so the largest level's total is the estimate.
void Water::waterCoverage(const Terrain& terrain, const float mvp[16])
{
	const kworld::Heightmap* hm = terrain.heightmap();
	std::vector<std::pair<float, double>> levels;	// cell size -> summed NDC area
	for (const Terrain::WaterChunk& c : chunks_)
	{
		const double size = (double)c.origin[3] * Terrain::kCells;
		const double y = kLevel + c.origin[1];	// the plane, camera relative (vs.level[0] below)
		int wet = 0, n = 0;
		if (hm)
		{
			constexpr int kSamples = 6;
			for (int j = 0; j < kSamples; j++)
			{
				for (int i = 0; i < kSamples; i++)
				{
					const double x = kworld::kWorldMin + (double)c.chunk[2] + size * (i + 0.5) / kSamples;
					const double z = kworld::kWorldMin + (double)c.chunk[3] + size * (j + 0.5) / kSamples;
					if (hm->height(x, z) < kLevel) { wet++; }
					n++;
				}
			}
			if (wet == 0) { continue; }
		}
		auto proj = [&](double x, double z)
		{
			return ClipV{ mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12], mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13], mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15] };
		};
		const double x0 = c.origin[0], z0 = c.origin[2];
		const double area = clipArea({ proj(x0, z0), proj(x0 + size, z0), proj(x0 + size, z0 + size), proj(x0, z0 + size) }) * (n > 0 ? (double)wet / n : 1.0);
		const auto it = std::find_if(levels.begin(), levels.end(), [&](const auto& l) { return l.first == c.origin[3]; });
		if (it == levels.end()) { levels.push_back({ c.origin[3], area }); }
		else { it->second += area; }
	}
	double best = 0;
	for (const auto& l : levels) { best = std::max(best, l.second); }
	coverage_ = (float)std::clamp(best / 4.0, 0.0, 1.0);
}

void Water::render(const Terrain& terrain, const float mvp[16], const LightParams& light, const float sunRaw[3], double seconds, float rain, bool wait, sg_view sceneDepth, float nearZ, float farZ)
{
	if (bake_.valid() && (wait || bake_.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
	{
		const Bake b = bake_.get();
		useBake(b);
	}
	terrain.waterChunks(kLevel, chunks_);
	if (chunks_.empty()) { coverage_ = 0; return; }
	waterCoverage(terrain, mvp);
	drawstats::pipeline(pip_);
	fs_params_t fs = {};
	fs.misc[0] = (float)std::fmod(seconds, 100000.0);
	fs.misc[1] = hasScum_ ? 1.0f : 0.0f;
	fs.misc[2] = flat ? 1.0f : 0.0f;
	// water.hlsl lights with sunColour.rgb as is; the deferred pass (lighting.inc sun_col) has it x alpha x ambientMult.a x 2: the ratio scales sun_col back
	fs.misc[3] = light.v[1][0] > 1e-6f ? sunRaw[0] / light.v[1][0] : 0.0f;
	fs.refl[0] = reflOn_ ? 1.0f : 0.0f;
	fs.zinfo[0] = nearZ;
	fs.zinfo[1] = farZ;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	fs.zinfo[2] = 1;
#endif
	fs.zinfo[3] = sceneDepth.id ? 1.0f : 0.0f;
	// the game's SharedWaterParams rainAmount (WATER.md §5.5): the weather's rain intensity / 50, clamped 0..1. Our SkyWeather::rain is intensity / 100
	// (sky.cpp), so the rings scale with two times the rain the particles use; 0 = clear, no rings.
	fs.rain[0] = std::clamp(2.0f * rain, 0.0f, 1.0f);
	for (const Terrain::WaterChunk& c : chunks_)
	{
		vs_params_t vs = {};
		memcpy(vs.mvp, mvp, sizeof vs.mvp);
		memcpy(vs.origin, c.origin, sizeof vs.origin);
		memcpy(vs.chunk, c.chunk, sizeof vs.chunk);
		vs.level[0] = kLevel + c.origin[1];	// origin.y = -camera y
		sg_bindings b = {};
		b.vertex_buffers[0] = terrain.gridVerts();
		b.index_buffer = terrain.gridIndices();
		b.views[VIEW_hmap] = c.hmap;
		b.samplers[SMP_smp] = nearSmp_;
		b.views[VIEW_colour] = view_[kColour];
		b.views[VIEW_flow] = view_[kFlow];
		b.views[VIEW_wnormal] = view_[kNormal];
		b.views[VIEW_scum_dif] = view_[kScumDif];
		b.views[VIEW_scum_nml] = view_[kScumNml];
		b.views[VIEW_windex] = view_[kIndex];
		b.views[VIEW_wtable] = view_[kTable];
		b.views[VIEW_wturb] = view_[kTurb];
		b.views[VIEW_rain_map] = view_[kRain];
		b.samplers[SMP_wsmp] = clampSmp_;
		b.samplers[SMP_rsmp] = repeatSmp_;
		b.samplers[SMP_psmp] = nearSmp_;
		b.views[VIEW_fog_map] = fog_map_binding().view;
		b.views[VIEW_spec_cube] = water_cube_binding().view;
		b.views[VIEW_irr_cube] = irr_cube_binding().view;
		b.samplers[SMP_spec_smp] = water_cube_binding().smp;
		b.samplers[SMP_fog_smp] = fog_map_binding().smp;
		b.views[VIEW_reflection] = reflView_;
		b.views[VIEW_scene_depth] = sceneDepth.id ? sceneDepth : view_[kColour];	// unread when zinfo.w = 0 (needs a bound float texture)
		drawstats::bindings(&b);
		sg_apply_uniforms(UB_vs_params, SG_RANGE(vs));
		sg_apply_uniforms(UB_fs_params, SG_RANGE(fs));
		sg_apply_uniforms(UB_light_params, { &light, sizeof light });
		drawstats::draw(0, Terrain::kGridIndices, 1);
	}
}
