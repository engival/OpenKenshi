#include "render/interiorclip.hpp"
#include "render/drawstats.hpp"
#include "render/framestats.hpp"
#include "world/grass.hpp"
#include "app/config.hpp"
#include "world/foliagewind.hpp"
#include "render/renderqueue.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/foliage.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "grass.glsl.h"
#include "render/speccube.hpp"
#include "stb_image.h"
#include "render/ao.hpp"
#include "render/weathermap.hpp"

namespace
{

constexpr int kTilesPerAxis = 1152;			// 294912 / 256
constexpr int kGenThreads = 3;
constexpr size_t kQueueAhead = 8;			// tiles requested at once while streaming (nearest first)
constexpr int kUploadsPerFrame = 4;
constexpr double kOverlayUnit = 36.0;			// world units per overlay texel (1024^2 texels per 36,864-unit tile)
constexpr int kOverlayTexels = 1024;
constexpr int kOverlayTiles = 8;
constexpr float kMinScale = 9.0f, kMaxScale = 17.0f;	// exe constants: GRASS min/max width and height are multiplied by these (1406d0340)
constexpr float kDensityScale = 0.005f;		// blades per square unit per unit of GRASS `density` (1406d0340; times the `grass density` setting)
constexpr float kWindScale = 3.0f;			// sway length = `wind factor` x 3
constexpr float kAlphaTest = 0.6f;			// Grass_FP threshold (foliage.material)
constexpr float kHeadroom = 80.0f;			// blades reach this far above their tile's highest ground, for culling
constexpr float kClassRange[3] = { 500.0f, 1000.0f, 8000.0f };	// per grass class at `grass range` 1: 10 x {50, 100, 800} (exe table, 1406d3e30 -> addDetailLevel 1406e1210)
constexpr int kSlopeStep = 36;			// slope = max height difference over one 36-unit step (as kworld's foliage, 1407eae40)

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

uint64_t splitmix(uint64_t& s)
{
	uint64_t z = (s += 0x9E3779B97F4A7C15ull);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

float unit(uint64_t& s) { return (float)(splitmix(s) >> 40) * (1.0f / 16777216.0f); }

// The exe's noise (1406cbba0, 1406cbf20, 1406cc0e0; docs/CLIENT.md "Grass"): Hugo Elias' integer hash with the second constant set, smoothed over 3 x 3 (1/4, 1/8, 1/16),
// cosine-interpolated value noise, five octaves (frequency 2^i, amplitude 0.5^i).
float hash_noise(uint32_t n)
{
	n ^= n << 13;
	return (float)(1.0 - (double)((n * (n * n * 60493u + 19990303u) + 1376312589u) & 0x7fffffffu) * 9.313225746154785e-10);
}

float smooth_noise(float x, float z)
{
	const int xi = (int)x, zi = (int)z;
	const auto h = [&](int dx, int dz) { return hash_noise((uint32_t)(xi + dx + (zi + dz) * 57)); };
	const float corners = h(-1, -1) + h(1, -1) + h(-1, 1) + h(1, 1);
	const float sides = h(-1, 0) + h(1, 0) + h(0, -1) + h(0, 1);
	return (float)((double)corners * 0.0625 + (double)sides * 0.125 + (double)h(0, 0) * 0.25);
}

float interpolated_noise(float x, float z)
{
	const float fx = std::floor(x), fz = std::floor(z);
	const double sx = (1.0 - std::cos((double)(x - fx) * 3.14159265358979323846)) * 0.5, sz = (1.0 - std::cos((double)(z - fz) * 3.14159265358979323846)) * 0.5;
	const float i1 = (float)((double)smooth_noise(fx, fz) * (1.0 - sx) + (double)smooth_noise(fx + 1.0f, fz) * sx);
	const float i2 = (float)((double)smooth_noise(fx, fz + 1.0f) * (1.0 - sx) + (double)smooth_noise(fx + 1.0f, fz + 1.0f) * sx);
	return (float)((double)i1 * (1.0 - sz) + (double)i2 * sz);
}

// 1406cc0e0: the fractal sum mapped to 0..1, cut at `cutoff` and rescaled, times `boost`, limited to 0..cap.
double fractal_noise(double x, double z, double scale, float cutoff, float boost, float cap)
{
	double sum = 0.0;
	for (int i = 0; i < 5; i++)
	{
		const float freq = (float)(1 << i);
		sum += (double)interpolated_noise((float)(((double)freq * x) / scale), (float)(((double)freq * z) / scale)) * std::pow(0.5, i);
	}
	const double v = sum * 0.5 + 0.5;
	double out = v <= cutoff ? 0.0 : (float)((v - cutoff) / (1.0 - cutoff));
	out = std::max(out * boost, 0.0);
	return std::min(out, (double)cap);
}

std::vector<uint8_t> load_rgba(const std::string& path, int& w, int& h)
{
	int comp = 0;
	unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
	if (!px) { return {}; }
	std::vector<uint8_t> out(px, px + (size_t)w * h * 4);
	stbi_image_free(px);
	return out;
}

}

// One GRASS record as the exe's layer set-up (1406d0340) reads it.
struct Grass::Def
{
	std::string id, name;
	float minW = 0, minH = 0, maxW = 0, maxH = 0;	// world units (record value x 9 / x 17)
	float density = 0;				// blades per square unit (record density x 0.005), before the settings multiplier
	float minAlt = 0, maxAlt = 0, maxSlope = 0, wind = 0;
	bool cross = false, blackout = false;
	bool spot = false;				// the layer's `grass` ref has value 0 != 0: its density map is overlay byte 1 (G, the grass spots), not byte 2 (R) (1406d0340 `*piVar15`)
	float noiseScale = 1, blackoutScale = 1, cutoff = 0.5f, blackoutCutoff = 0.5f, boost = 1, cap = 1;	// the density paint (1406cd970, --grass-map paint)
	int cmap = -1;					// index into Context::cmaps, -1 = white
	int cls = 0;					// draw-distance class 0..2: the `visibility range` of the layer that lists it (the largest if several)
	DdsImage sprite;
	bool ok = false;
};

struct Grass::Context
{
	GrassSource src;
	std::vector<Def> defs;
	std::unordered_map<std::uint32_t, std::vector<int>> biomeDefs;	// BIOMES `index` -> defs of its FOLIAGE_LAYERs' `grass` refs, in order (a def listed twice runs twice)
	struct CMap { int w = 0, h = 0; std::vector<uint8_t> rgba; };
	std::vector<CMap> cmaps;
	struct Overlay
	{
		std::once_flag once;
		std::vector<uint8_t> rgba;	// per texel R (the grass density), G (the grass spots of the `spot` layers), B (dirt: above 70 the paint writes 0), A (road and town ground: above 10 the paint writes 0)
	};
	std::array<Overlay, kOverlayTiles * kOverlayTiles> overlay;
	std::string overlayDir;
	bool paintMap = false;	// the exe's noise paint instead of the overlay R byte as loaded

	const std::vector<uint8_t>& tile(int tx, int tz)
	{
		Overlay& o = overlay[tx + tz * kOverlayTiles];
		std::call_once(o.once, [&]
		{
			int w = 0, h = 0;
			const std::vector<uint8_t> px = load_rgba(overlayDir + "/new_overlay." + std::to_string(tx) + "." + std::to_string(tz) + ".png", w, h);
			o.rgba.assign((size_t)kOverlayTexels * kOverlayTexels * 4, 0);
			if (w != kOverlayTexels || h != kOverlayTexels) { return; }
			std::copy(px.begin(), px.begin() + (size_t)w * h * 4, o.rgba.begin());
		});
		return o.rgba;
	}

	// One GRASS layer's density paint (1406cd970) at a world point of height h: the noise (cut off, boosted, capped), times the blackout noise, times the altitude falloff
	// (300 units outside the layer's altitude limits), stored in a byte (the exe truncates; a value above 255 wraps).
	static uint8_t paint(const Def& d, double x, double z, float h)
	{
		float t = h >= d.minAlt ? (h > d.maxAlt ? (d.maxAlt + 300.0f - h) / 300.0f : 1.0f) : (h - (d.minAlt - 300.0f)) / 300.0f;
		t = std::clamp(t, 0.0f, 1.0f);
		if (t <= 0.0f) { return 0; }
		float v = (float)fractal_noise(x, z, d.noiseScale * (kOverlayUnit * 2.0), d.cutoff, d.boost, d.cap);
		if (d.blackout) { v *= (float)fractal_noise(x + 50007.0, z + 50007.0, d.blackoutScale * (kOverlayUnit * 2.0), d.blackoutCutoff, d.boost, d.cap); }
		return (uint8_t)(int64_t)(v * 255.0f * t);
	}

	// The density map the layers of a biome share (1406d0340 copies the zone's window of the overlay image; kinspect findings/grass_density_runtime.md): the overlay R
	// byte as loaded, which already is 0 on roads, town ground and dirt (the paint below wrote it so), and nothing where the texel belongs to another biome.
	// --grass-map paint: the exe's repaint instead (1406cd970, run only when a save-fixup rebuilds the foliage): per texel the strongest paint of the biome's layers
	// (it writes a layer's value only where it is larger), 0 where the road alpha is above 10 or the dirt channel above 70. Computed for the texels around one tile.
	struct Grid
	{
		int i0 = 0, j0 = 0, w = 0, h = 0;
		std::vector<float> v, spot;
		std::vector<std::uint32_t> biome;
	};

	Grid grid(double x0, double z0, double size)
	{
		constexpr int kN = kOverlayTexels * kOverlayTiles;
		Grid g;
		g.i0 = (int)std::floor((x0 - kworld::kWorldMin) / kOverlayUnit - 0.5);
		g.j0 = (int)std::floor((z0 - kworld::kWorldMin) / kOverlayUnit - 0.5);
		g.w = (int)std::floor((x0 + size - kworld::kWorldMin) / kOverlayUnit - 0.5) + 2 - g.i0;
		g.h = (int)std::floor((z0 + size - kworld::kWorldMin) / kOverlayUnit - 0.5) + 2 - g.j0;
		g.v.assign((size_t)g.w * g.h, 0.0f);
		g.spot.assign((size_t)g.w * g.h, 0.0f);
		g.biome.assign((size_t)g.w * g.h, 0xffffffffu);
		for (int j = 0; j < g.h; j++)
		{
			for (int i = 0; i < g.w; i++)
			{
				const int gx = g.i0 + i, gz = g.j0 + j;
				if (gx < 0 || gz < 0 || gx >= kN || gz >= kN) { continue; }
				const double cx = kworld::kWorldMin + (gx + 0.5) * kOverlayUnit, cz = kworld::kWorldMin + (gz + 0.5) * kOverlayUnit;
				const std::uint32_t b = src.biomes->biomeAt(cx, cz);
				g.biome[(size_t)j * g.w + i] = b;
				const auto it = biomeDefs.find(b);
				if (it == biomeDefs.end()) { continue; }
				const std::vector<uint8_t>& t = tile(gx / kOverlayTexels, gz / kOverlayTexels);
				const size_t k = ((size_t)(gz % kOverlayTexels) * kOverlayTexels + gx % kOverlayTexels) * 4;
				g.spot[(size_t)j * g.w + i] = t[k + 1] * (1.0f / 255.0f);
				if (!paintMap) { g.v[(size_t)j * g.w + i] = t[k] * (1.0f / 255.0f); continue; }
				if (t[k + 3] > 10 || t[k + 2] > 70) { continue; }
				const float h = (float)src.terrain->height(cx, cz);
				uint8_t best = 0;
				for (const int di : it->second) { if (defs[di].ok) { best = std::max(best, paint(defs[di], cx, cz, h)); } }
				g.v[(size_t)j * g.w + i] = best * (1.0f / 255.0f);
			}
		}
		return g;
	}

	// The nearest texel (140a6ba30: byte / 255), 0 where it belongs to another biome than `biome`.
	static float density(const Grid& g, double x, double z, std::uint32_t biome, bool spot)
	{
		const int i = (int)std::floor((x - kworld::kWorldMin) / kOverlayUnit) - g.i0, j = (int)std::floor((z - kworld::kWorldMin) / kOverlayUnit) - g.j0;
		const size_t k = (size_t)j * g.w + i;
		return i < 0 || j < 0 || i >= g.w || j >= g.h || g.biome[k] != biome ? 0.0f : (spot ? g.spot : g.v)[k];
	}

	// The colour map spans one zone (the layer's map bounds are the zone rectangle): the same 1024^2 picture over every 4608-unit zone.
	void colour(const Def& d, double x, double z, uint8_t out[3]) const
	{
		if (d.cmap < 0) { out[0] = out[1] = out[2] = 255; return; }
		const CMap& m = cmaps[d.cmap];
		const double u = std::fmod(x - kworld::kWorldMin, kworld::kZoneSize) / kworld::kZoneSize * m.w - 0.5, v = std::fmod(z - kworld::kWorldMin, kworld::kZoneSize) / kworld::kZoneSize * m.h - 0.5;
		const int i = (int)std::floor(u), j = (int)std::floor(v);
		const float fx = (float)(u - i), fz = (float)(v - j);
		const auto at = [&](int xx, int zz, int c) { return (float)m.rgba[((size_t)std::clamp(zz, 0, m.h - 1) * m.w + std::clamp(xx, 0, m.w - 1)) * 4 + c]; };
		for (int c = 0; c < 3; c++)
		{
			const float s = (at(i, j, c) * (1 - fx) + at(i + 1, j, c) * fx) * (1 - fz) + (at(i, j + 1, c) * (1 - fx) + at(i + 1, j + 1, c) * fx) * fz;
			out[c] = (uint8_t)std::clamp(s + 0.5f, 0.0f, 255.0f);
		}
	}
};

Grass::Grass() = default;
Grass::~Grass() = default;

void Grass::init(const ResourceIndex* res, const Foliage* foliage, const std::string& dataDir)
{
	res_ = res;
	foliage_ = foliage;
	dataDir_ = dataDir;
	loader_ = std::thread([this] { loader(); });
	for (int i = 0; i < kGenThreads; i++) { threads_.emplace_back([this] { worker(); }); }
}

void Grass::shutdown()
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		quit_ = true;
	}
	jobCv_.notify_all();
	for (std::thread& t : threads_) { t.join(); }
	threads_.clear();
	if (loader_.joinable()) { loader_.join(); }
	for (auto& [key, b] : gpu_) { sg_destroy_buffer(b); }
	gpu_.clear();
	tiles_.clear();
	if (texturesMade_)
	{
		for (sg_view v : views_) { if (v.id != SG_INVALID_ID) { sg_destroy_view(v); } }
		for (sg_image i : images_) { if (i.id != SG_INVALID_ID) { sg_destroy_image(i); } }
		sg_destroy_buffer(corners_);
		sg_destroy_buffer(indices_);
		sg_destroy_pipeline(pip_);
		sg_destroy_shader(shd_);
		sg_destroy_sampler(smp_);
	}
}

// ---- layer table (loader thread) ----

void Grass::loader()
{
	try
	{
		const auto t0 = Clock::now();
		while (!foliage_->ready() && !foliage_->failed() && !quit_) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
		if (quit_) { return; }
		const GrassSource src = foliage_->grassSource();
		if (!src.gd) { failed_ = true; return; }
		auto c = std::make_unique<Context>();
		c->src = src;
		c->overlayDir = dataDir_ + "/newland/land/overlaymaps";
		c->paintMap = paintMap;
		const kdata::GameData& gd = *src.gd;
		std::unordered_map<std::string, int> cmapOf, defOf;
		auto cmapIndex = [&](const std::string& path) -> int
		{
			const std::string file = path.empty() ? "" : res_->locate(path);
			if (file.empty()) { return -1; }
			const auto [it, fresh] = cmapOf.emplace(file, (int)c->cmaps.size());
			if (fresh)
			{
				c->cmaps.emplace_back();
				Context::CMap& m = c->cmaps.back();
				m.rgba = load_rgba(file, m.w, m.h);
				if (m.rgba.empty()) { it->second = -1; c->cmaps.pop_back(); }
			}
			return it->second;
		};
		auto defIndex = [&](const kdata::Record& r, int cls, bool spot) -> int
		{
			const auto [it, fresh] = defOf.emplace(spot ? r.stringID + "#spot" : r.stringID, (int)c->defs.size());
			if (!fresh) { c->defs[it->second].cls = std::max(c->defs[it->second].cls, cls); return it->second; }
			Def d;
			d.id = r.stringID;
			d.spot = spot;
			d.name = r.name;
			d.cls = cls;
			d.minW = r.floatOr("min width", 1.0f) * kMinScale;
			d.minH = r.floatOr("min height", 1.0f) * kMinScale;
			d.maxW = r.floatOr("max width", 1.0f) * kMaxScale;
			d.maxH = r.floatOr("max height", 1.0f) * kMaxScale;
			d.density = r.floatOr("density", 1.0f) * kDensityScale;
			d.minAlt = r.floatOr("min altitude", 0.0f);
			d.maxAlt = r.floatOr("max altitude", 1e9f);
			d.maxSlope = r.floatOr("max slope", 1e9f);
			d.wind = r.floatOr("wind factor", 1.0f) * kWindScale;
			d.cross = r.boolOr("cross quads", false);
			d.blackout = r.boolOr("blackout", false);
			d.noiseScale = r.floatOr("noise scale", 1.0f);
			d.blackoutScale = r.floatOr("blackout noise scale", 1.0f);
			d.cutoff = r.floatOr("zero cutoff", 0.5f);
			d.blackoutCutoff = r.floatOr("blackout zero cutoff", 0.5f);
			d.boost = r.floatOr("brightness boost", 1.0f);
			d.cap = r.floatOr("cap", 1.0f);
			d.cmap = cmapIndex(r.fileOr("color map"));
			std::string err;
			const std::string sprite = res_->locate(r.fileOr("grass sprite"));
			d.ok = !sprite.empty() && dds_load(sprite, d.sprite, err);
			if (!d.ok) { fprintf(stderr, "grass: %s: sprite %s not loaded%s%s\n", d.id.c_str(), r.fileOr("grass sprite").c_str(), err.empty() ? "" : ": ", err.c_str()); }
			c->defs.push_back(std::move(d));
			return it->second;
		};
		for (const kdata::Record* biome : gd.byType(kworld::itemTypeByName("BIOMES")))
		{
			const kdata::RefCategory* layers = biome->findRefs("foliage");
			if (!layers) { continue; }
			std::vector<int> defs;
			for (const auto& e : layers->entries)
			{
				const kdata::Record* layer = gd.find(e.targetID);
				const kdata::RefCategory* grass = layer ? layer->findRefs("grass") : nullptr;
				if (!grass) { continue; }
				for (const auto& g : grass->entries) { if (const kdata::Record* rec = gd.find(g.targetID)) { defs.push_back(defIndex(*rec, std::clamp((int)layer->intOr("visibility range", 1), 0, 2), g.v0 != 0)); } }
			}
			if (!defs.empty()) { c->biomeDefs[(std::uint32_t)biome->intOr("index", 0)] = std::move(defs); }	// the last BIOMES record of an index wins, as in kworld::generateFoliage
		}
		if (getenv("KGRASSLOG"))
		{
			for (const auto& [b, ds] : c->biomeDefs)
			{
				fprintf(stderr, "grass: biome %06x:", b);
				for (const int di : ds) { fprintf(stderr, " %s", c->defs[di].name.c_str()); }
				fprintf(stderr, "\n");
			}
		}
		stats_.layers = c->defs.size();
		stats_.biomes = c->biomeDefs.size();
		char buf[240];
		snprintf(buf, sizeof buf, "grass: %zu GRASS records in %zu biomes, %zu colour maps, loaded in %.0f ms", c->defs.size(), c->biomeDefs.size(), c->cmaps.size(), msSince(t0));
		report_ = buf;
		ctx_ = std::move(c);
		ready_ = true;
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "grass: setup failed: %s\n", e.what());
		failed_ = true;
	}
}

// ---- generation (worker threads) ----

void Grass::worker()
{
	for (;;)
	{
		int key;
		{
			std::unique_lock<std::mutex> lk(mu_);
			jobCv_.wait(lk, [this] { return quit_ || !jobs_.empty(); });
			if (quit_) { return; }
			key = jobs_.front();
			jobs_.pop_front();
		}
		const auto t0 = Clock::now();
		TileCpu out;
		out.key = key;
		out.minY = 1e30f;
		out.maxY = -1e30f;
		try
		{
			Context& c = *ctx_;
			const kworld::Heightmap& hm = *c.src.terrain;
			const kworld::BiomeMap& bm = *c.src.biomes;
			const int tx = key % kTilesPerAxis, tz = key / kTilesPerAxis;
			const double x0 = kworld::kWorldMin + tx * kTile, z0 = kworld::kWorldMin + tz * kTile;
			// biomes under the tile (a biome pixel is 288 units, so at most 2 x 2)
			std::vector<std::uint32_t> biomes;
			const double upp = bm.unitsPerPixel();
			for (int row = (int)((z0 - kworld::kWorldMin) / upp); row <= (int)((z0 + kTile - kworld::kWorldMin) / upp) && row < bm.height(); row++)
			{
				for (int col = (int)((x0 - kworld::kWorldMin) / upp); col <= (int)((x0 + kTile - kworld::kWorldMin) / upp) && col < bm.width(); col++)
				{
					const std::uint32_t b = bm.pixel(col, row);
					if (c.biomeDefs.count(b) && std::find(biomes.begin(), biomes.end(), b) == biomes.end()) { biomes.push_back(b); }
				}
			}
			const Context::Grid grid = c.grid(x0, z0, kTile);
			uint64_t seedBase = (uint64_t)key * 0x9E3779B97F4A7C15ull;
			for (const std::uint32_t biome : biomes)
			{
				int ordinal = 0;
				for (const int di : c.biomeDefs[biome])
				{
					const Def& d = c.defs[di];
					uint64_t s = seedBase ^ ((uint64_t)biome << 20) ^ (uint64_t)(ordinal++ * 7919 + di + 1);
					if (!d.ok) { continue; }
					const float want = d.density * density * (float)(kTile * kTile);
					const int n = (int)want + (unit(s) < want - (int)want ? 1 : 0);
					const int first = (int)out.data.size();
					for (int i = 0; i < n; i++)
					{
						// every candidate draws all its numbers whether it is kept or not, so the stream does not depend on the data
						const double x = x0 + unit(s) * kTile, z = z0 + unit(s) * kTile;
						const float accept = unit(s), size = unit(s), yaw = unit(s), phase = unit(s);
						if (bm.biomeAt(x, z) != biome) { continue; }
						const float y = (float)hm.height(x, z);
						if (y < d.minAlt || y > d.maxAlt) { continue; }
						const float slope = std::max(std::fabs(y - (float)hm.height(x - kSlopeStep, z)), std::fabs(y - (float)hm.height(x, z + kSlopeStep)));
						if (slope > d.maxSlope) { continue; }
						if (Context::density(grid, x, z, biome, d.spot) <= accept) { continue; }
						Inst b = {};
						b.x = (float)(x - x0);
						b.y = y;
						b.z = (float)(z - z0);
						c.colour(d, x, z, &b.r);
						b.a = 255;
						b.w = (uint16_t)std::min(65535.0f, (d.minW + (d.maxW - d.minW) * size) * 1024.0f);
						b.h = (uint16_t)std::min(65535.0f, (d.minH + (d.maxH - d.minH) * size) * 1024.0f);
						b.yaw = (uint16_t)(yaw * 65535.0f);
						b.phase = (uint16_t)(phase * 65535.0f);
						out.data.push_back(b);
						out.minY = std::min(out.minY, y);
						out.maxY = std::max(out.maxY, y);
					}
					if ((int)out.data.size() > first) { out.ranges.push_back({ di, first, (int)out.data.size() - first }); }
				}
			}
		}
		catch (const std::exception& e)
		{
			fprintf(stderr, "grass: tile %d failed: %s\n", key, e.what());
			out.data.clear();
			out.ranges.clear();
		}
		out.ms = msSince(t0);
		{
			std::lock_guard<std::mutex> lk(mu_);
			results_.push_back(std::move(out));
		}
		resCv_.notify_all();
	}
}

// ---- main thread: makePipeline (render side, from upload), accept / take (logic side, from prepare) ----

void Grass::makePipeline()
{
	const float corners[] = {	// x across, y up, z = which quad
		-0.5f, 0, 0,  0.5f, 0, 0,  0.5f, 1, 0,  -0.5f, 1, 0,
		-0.5f, 0, 1,  0.5f, 0, 1,  0.5f, 1, 1,  -0.5f, 1, 1,
	};
	const uint16_t idx[] = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7 };
	sg_buffer_desc bd = {};
	bd.data = SG_RANGE(corners);
	bd.label = "grass-corners";
	corners_ = sg_make_buffer(&bd);
	bd = {};
	bd.usage.index_buffer = true;
	bd.data = SG_RANGE(idx);
	bd.label = "grass-indices";
	indices_ = sg_make_buffer(&bd);
	shd_ = sg_make_shader(grass_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.buffers[0].stride = 12;
	pd.layout.buffers[1].stride = sizeof(Inst);
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.attrs[ATTR_grass_corner] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_grass_inst_pos] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_grass_inst_col] = { .buffer_index = 1, .offset = 12, .format = SG_VERTEXFORMAT_UBYTE4N };
	pd.layout.attrs[ATTR_grass_inst_size] = { .buffer_index = 1, .offset = 16, .format = SG_VERTEXFORMAT_USHORT2N };
	pd.layout.attrs[ATTR_grass_inst_rnd] = { .buffer_index = 1, .offset = 20, .format = SG_VERTEXFORMAT_USHORT2N };
	pd.index_type = SG_INDEXTYPE_UINT16;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "grass";
	pip_ = sg_make_pipeline(&pd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.max_anisotropy = 4;
	sd.mipmap_filter = SG_FILTER_LINEAR;
	smp_ = sg_make_sampler(&sd);
	for (const Def& d : ctx_->defs)
	{
		sg_image img = d.ok ? dds_make_image(d.sprite, "grass-sprite") : sg_image{};
		sg_view v = {};
		if (img.id != SG_INVALID_ID && sg_query_image_state(img) == SG_RESOURCESTATE_VALID)
		{
			sg_view_desc vd = {};
			vd.texture.image = img;
			v = sg_make_view(&vd);
		}
		images_.push_back(img);
		views_.push_back(v);
	}
	texturesMade_ = true;
}

// A worker's tile into the logic-side table (its blades go to the render side in a queued job, take()).
void Grass::accept(TileCpu& cpu)
{
	queued_.erase(cpu.key);
	Tile& t = tiles_[cpu.key];
	t.ranges = cpu.ranges;
	t.blades = cpu.data.size();
	t.hasBuf = !cpu.data.empty();
	t.minY = cpu.minY;
	t.maxY = cpu.maxY;
	t.lastUsed = frame_;
	stats_.generated++;
	stats_.genMs += cpu.ms;
	stats_.genMaxMs = std::max(stats_.genMaxMs, cpu.ms);
}

bool Grass::take(bool wait)
{
	TileCpu c;
	{
		std::unique_lock<std::mutex> lk(mu_);
		if (wait && results_.empty() && outstanding_ > 0) { resCv_.wait(lk, [this] { return !results_.empty(); }); }
		if (results_.empty()) { return false; }
		c = std::move(results_.front());
		results_.pop_front();
	}
	outstanding_--;
	accept(c);
	if (!c.data.empty())	// render side: the tile's instance buffer (frame lane: before this packet's draw)
	{
		const size_t bytes = c.data.size() * sizeof(Inst);
		rq::push("grass-tile", [this, key = c.key, data = std::move(c.data)] {
			sg_buffer_desc bd = {};
			bd.data = { data.data(), data.size() * sizeof(Inst) };
			bd.label = "grass-tile";
			gpu_[key] = sg_make_buffer(&bd);
		}, rq::Lane::kFrame, bytes);
	}
	return true;
}

double Grass::classRange(const Def& d) const
{
	return std::min(rangeScale * kClassRange[d.cls], kMaxReach);
}

Grass::Stats Grass::stats() const
{
	Stats s = stats_;
	const auto& g = framestats::stats().grass;
	s.bladesDrawn = g.bladesDrawn; s.draws = g.draws; s.tilesDrawn = g.tilesDrawn;
	return s;
}

// ---- logic side ----

void Grass::prepare(const double cam[3], const float mvp[16], bool blocking, FrameData& out)
{
	frame_++;
	out.tiles.clear();
	out.ranges.clear();
	out.ready = ready_;
	out.windStrength = foliagewind::g.strength;
	out.windDir[0] = foliagewind::g.dir[0];
	out.windDir[1] = foliagewind::g.dir[1];
	out.windPhase = foliagewind::g.phase;
	if (!out.ready) { return; }
	if (!pipelineQueued_)	// the first ready frame: the render side makes the pipeline and the sprites, before any tile buffer
	{
		pipelineQueued_ = true;
		rq::push("grass-pipeline", [this] { makePipeline(); });
	}
	if (!reported_) { reported_ = true; printf("%s\n", report_.c_str()); }
	for (int i = 0; i < 3; i++) { out.cam[i] = cam[i]; }

	// tiles whose nearest point is within the range, nearest first; one tile of margin so they are resident before the fade reaches them
	double maxRange = 0;
	for (const Def& d : ctx_->defs) { maxRange = std::max(maxRange, classRange(d)); }
	const double reach = maxRange + kTile;
	const int cx = (int)std::floor((cam[0] - kworld::kWorldMin) / kTile), cz = (int)std::floor((cam[2] - kworld::kWorldMin) / kTile);
	const int span = (int)std::ceil(reach / kTile) + 1;
	std::vector<std::pair<double, int>> wanted;
	const auto tileDistance = [&](int x, int z)
	{
		const double ex = std::max({ kworld::kWorldMin + x * kTile - cam[0], cam[0] - (kworld::kWorldMin + (x + 1) * kTile), 0.0 });
		const double ez = std::max({ kworld::kWorldMin + z * kTile - cam[2], cam[2] - (kworld::kWorldMin + (z + 1) * kTile), 0.0 });
		return std::hypot(ex, ez);
	};
	for (int dz = -span; dz <= span; dz++)
	{
		for (int dx = -span; dx <= span; dx++)
		{
			const int x = cx + dx, z = cz + dz;
			if (x < 0 || z < 0 || x >= kTilesPerAxis || z >= kTilesPerAxis) { continue; }
			const double d = tileDistance(x, z);
			if (d <= reach) { wanted.push_back({ d, x + kTilesPerAxis * z }); }
		}
	}
	std::sort(wanted.begin(), wanted.end());
	for (const auto& [d, key] : wanted)
	{
		if (!blocking && queued_.size() >= kQueueAhead) { break; }
		if (tiles_.count(key) || queued_.count(key)) { continue; }
		queued_.insert(key);
		outstanding_++;
		{
			std::lock_guard<std::mutex> lk(mu_);
			jobs_.push_back(key);
		}
		jobCv_.notify_one();
	}
	if (blocking) { while (take(true)) {} }
	else { for (int n = 0; n < kUploadsPerFrame && take(false); n++) {} }
	std::vector<int> frees;
	for (auto it = tiles_.begin(); it != tiles_.end();)	// free tiles that left the range (one tile of hysteresis)
	{
		if (tileDistance(it->first % kTilesPerAxis, it->first / kTilesPerAxis) > reach + kTile)
		{
			if (it->second.hasBuf) { frees.push_back(it->first); }
			it = tiles_.erase(it);
		} else { ++it; }
	}
	if (!frees.empty())	// render side: their buffers go, after this frame's creates (the lane's order)
	{
		rq::push("grass-free", [this, frees = std::move(frees)] {
			for (int key : frees)
			{
				const auto it = gpu_.find(key);
				if (it == gpu_.end()) { continue; }
				if (it->second.id != SG_INVALID_ID) { sg_destroy_buffer(it->second); }
				gpu_.erase(it);
			}
		});
	}

	// culling: range, then the view frustum (planes of the camera-relative projection * rotation matrix, column-major)
	float pl[6][4];
	for (int i = 0; i < 4; i++)
	{
		pl[0][i] = mvp[i * 4 + 3] + mvp[i * 4];
		pl[1][i] = mvp[i * 4 + 3] - mvp[i * 4];
		pl[2][i] = mvp[i * 4 + 3] + mvp[i * 4 + 1];
		pl[3][i] = mvp[i * 4 + 3] - mvp[i * 4 + 1];
		pl[4][i] = mvp[i * 4 + 3] + mvp[i * 4 + 2];
		pl[5][i] = mvp[i * 4 + 3] - mvp[i * 4 + 2];
	}
	stats_.tilesLoaded = tiles_.size();
	stats_.tilesPending = queued_.size();
	stats_.bladesLoaded = 0;
	for (auto& [key, t] : tiles_)
	{
		stats_.bladesLoaded += t.blades;
		if (!t.hasBuf) { continue; }
		const int tx = key % kTilesPerAxis, tz = key / kTilesPerAxis;
		const double dist = tileDistance(tx, tz);
		if (dist > maxRange) { continue; }
		const float lo[3] = { (float)(kworld::kWorldMin + tx * kTile - cam[0]), t.minY - (float)cam[1], (float)(kworld::kWorldMin + tz * kTile - cam[2]) };
		const float hi[3] = { lo[0] + (float)kTile, t.maxY + kHeadroom - (float)cam[1], lo[2] + (float)kTile };
		bool inside = true;
		for (int p = 0; p < 6 && inside; p++)
		{
			const float px = pl[p][0] >= 0 ? hi[0] : lo[0], py = pl[p][1] >= 0 ? hi[1] : lo[1], pz = pl[p][2] >= 0 ? hi[2] : lo[2];
			if (pl[p][0] * px + pl[p][1] * py + pl[p][2] * pz + pl[p][3] < 0) { inside = false; }
		}
		if (!inside) { continue; }
		t.lastUsed = frame_;
		DrawTile dt;
		dt.key = key;
		dt.base[0] = lo[0];
		dt.base[1] = (float)-cam[1];
		dt.base[2] = lo[2];
		dt.dist = dist;
		dt.firstRange = (int)out.ranges.size();
		dt.rangeCount = (int)t.ranges.size();
		for (const Range& r : t.ranges) { out.ranges.push_back({ r.def, r.first, r.count, classRange(ctx_->defs[r.def]) }); }
		out.tiles.push_back(dt);
	}
}

// ---- render side ----

const cfg::Bool kGrassLiveWind{ cfg::Group{ "Detail" }, "render.grass_live_wind", false, "Grass sways with the live wind: strength, rate and direction, like render.leaf_sway (off: the original, a constant rate along +X)" };

void Grass::render(const FrameData& d, const float mvp[16], const LightParams& light, double time)
{
	if (!ready_ || !texturesMade_ || d.tiles.empty()) { return; }
	drawstats::pipeline(pip_);
	static_assert(sizeof(light_params_t) == sizeof(LightParams));
	grass_vs_params_t vs = {};
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	for (int i = 0; i < 3; i++) { vs.camw[i] = (float)d.cam[i]; }
	const bool live = kGrassLiveWind.get();
	vs.camw[3] = live ? (float)d.windPhase : (float)time;
	grass_fs_params_t fs = {};
	fs.fade[2] = kAlphaTest;
	sg_bindings b = {};
	b.vertex_buffers[0] = corners_;
	b.index_buffer = indices_;
	b.samplers[SMP_grass_smp] = smp_;
	b.views[VIEW_shadow_tex] = sfd->view;
	b.samplers[SMP_shadow_smp] = sfd->sampler;
	b.samplers[SMP_shadow_raw] = sfd->rawSampler;
	b.views[VIEW_fog_map] = fog_map_binding().view;
	b.samplers[SMP_fog_smp] = fog_map_binding().smp;
	b.views[VIEW_ao_tex] = ao_binding().view;
	b.views[VIEW_il_tex] = ao_binding().il;
	b.samplers[SMP_ao_smp] = ao_binding().smp;
	b.views[VIEW_imask_tex] = interiorclip::view();
	b.samplers[SMP_imask_smp] = interiorclip::sampler();
	b.views[VIEW_irr_cube] = irr_cube_binding().view;	// light_irradiance's mp_irradiance.dds
	b.samplers[SMP_spec_smp] = spec_cube_binding().smp;	// the two cubes share the sampler
	for (const DrawTile& dt : d.tiles)
	{
		const auto g = gpu_.find(dt.key);
		if (g == gpu_.end() || g->second.id == SG_INVALID_ID) { continue; }
		for (int i = 0; i < 3; i++) { vs.base[i] = dt.base[i]; }
		b.vertex_buffers[1] = g->second;
		for (int ri = dt.firstRange; ri < dt.firstRange + dt.rangeCount; ri++)
		{
			const DrawRange& r = d.ranges[ri];
			const Def& def = ctx_->defs[r.def];
			if (views_[r.def].id == SG_INVALID_ID) { continue; }
			const double reach = r.reach;
			if (dt.dist > reach) { continue; }
			vs.fade[0] = fs.fade[0] = (float)(reach * 0.8);
			vs.fade[1] = fs.fade[1] = (float)reach;
			b.vertex_buffer_offsets[1] = r.first * (int)sizeof(Inst);
			b.views[VIEW_grass_tex] = views_[r.def];
			drawstats::bindings(&b);
			// The original: GrassLoader's direction is UNIT_X, written only in its ctor (kinspect foliage_wind.md §2), so the default sways along +X.
			// render.grass_live_wind: amplitude, rate and direction follow the live foliage wind, like the leaves.
			vs.wind[0] = live ? def.wind * d.windStrength : def.wind;
			vs.wind[1] = live ? d.windDir[0] : 1.0f;
			vs.wind[2] = live ? d.windDir[1] : 0.0f;
			vs.wind[3] = live ? 0.0f : 1.0f;	// shader mode: the original's sway (grass.glsl) unless live
			sg_apply_uniforms(UB_grass_vs_params, SG_RANGE(vs));
			sg_apply_uniforms(UB_grass_fs_params, SG_RANGE(fs));
			sg_apply_uniforms(UB_light_params, { &light, sizeof light });
			sg_apply_uniforms(UB_ao_params, SG_RANGE(ao_binding().params));
			sg_apply_uniforms(UB_shadow_params, { &sfd->params, sizeof(ShadowParams) });
			drawstats::draw(0, def.cross ? 12 : 6, r.count);
			framestats::addGrass(r.count, 1, 0);
		}
		framestats::addGrass(0, 0, 1);
	}
}
