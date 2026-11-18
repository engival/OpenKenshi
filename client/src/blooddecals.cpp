#include "blooddecals.hpp"
#include "drawstats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

#include "dds.hpp"
#include "renderqueue.hpp"
#include "blooddecal.glsl.h"

namespace
{

constexpr double kHalfWorld = 147456.0;
const char* const kTypeNames[BloodDecals::kTypes] = { "line", "splash", "pool", "smear" };	// 1408dfaf0: the subdirectories of resource group Blood, in this index order

long long patchKey(int ix, int iz) { return (long long)ix << 32 | (unsigned)iz; }

}

int BloodDecals::typeByName(const std::string& name)
{
	for (int t = 0; t < kTypes; t++) { if (name == kTypeNames[t]) { return t; } }
	return -1;
}

void BloodDecals::init(const std::string& dataDir)
{
	for (long long& s : slotPatch_) { s = -1; }
	// resources.cfg [Blood]: materials/blood/{line,splash,pool,smear}; the game's image order is its scan order (UNSURE), ours is by file name
	for (int t = 0; t < kTypes; t++)
	{
		std::vector<std::string> files;
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(dataDir + "/materials/blood/" + kTypeNames[t], ec)) { if (e.path().extension() == ".dds") { files.push_back(e.path().string()); } }
		std::sort(files.begin(), files.end());
		for (const std::string& f : files)
		{
			DdsImage dds;
			std::string err;
			if (!dds_load(f, dds, err)) { fprintf(stderr, "kclient: blood: %s\n", err.c_str()); continue; }
			Image im;
			im.w = dds.width;
			im.h = dds.height;
			im.img = dds_make_image(dds, "blood-image");
			sg_view_desc vd = {};
			vd.texture.image = im.img;
			im.view = sg_make_view(&vd);
			images_[t].push_back(im);
			sizes_[t].push_back({ im.w, im.h });
		}
	}
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.label = "blood-image";
	imgSmp_ = sg_make_sampler(&sd);
	sd.mipmap_filter = SG_FILTER_NEAREST;
	sd.label = "blood-atlas";
	smp_ = sg_make_sampler(&sd);
	// the terrain binds something before any decal exists
	const uint32_t none = 0;
	sg_image_desc id = {};
	id.width = id.height = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = { &none, 4 };
	id.label = "blood-blank";
	blankImg_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = blankImg_;
	blank_ = sg_make_view(&vd);
	const float corners[8] = { -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };
	sg_buffer_desc bd = {};
	bd.data = SG_RANGE(corners);
	bd.label = "blood-quad";
	quad_ = sg_make_buffer(&bd);
	sg_pipeline_desc pd = {};
	pd.shader = sg_make_shader(blooddecal_shader_desc(sg_query_backend()));
	pd.layout.attrs[ATTR_blooddecal_corner].format = SG_VERTEXFORMAT_FLOAT2;
	pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
	pd.colors[0].blend = { .enabled = true, .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA, .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
		.src_factor_alpha = SG_BLENDFACTOR_ONE, .dst_factor_alpha = SG_BLENDFACTOR_ONE };	// setSeparateSceneBlending(0, 2): alpha added (UNSURE reading)
	pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
	pd.sample_count = 1;
	pd.label = "blood-decal";
	pip_ = sg_make_pipeline(&pd);
	size_t n = 0;
	for (const auto& v : images_) { n += v.size(); }
	printf("blood: %zu decal images\n", n);
}

void BloodDecals::shutdown()
{
	for (auto& v : images_) { for (Image& im : v) { sg_destroy_view(im.view); sg_destroy_image(im.img); } v.clear(); }
	for (auto& v : sizes_) { v.clear(); }
	sg_destroy_view(atlasView_);
	sg_destroy_view(atlasAtt_);
	sg_destroy_image(atlas_);
	sg_destroy_view(blank_);
	sg_destroy_image(blankImg_);
	sg_destroy_sampler(smp_);
	sg_destroy_sampler(imgSmp_);
	sg_destroy_pipeline(pip_);
	sg_destroy_buffer(quad_);
}

void BloodDecals::add(Type type, double x, double z, float size, float dirRad, const float colour[4], int image)
{
	const std::vector<Size>& ims = sizes_[type];
	if (ims.empty()) { return; }
	if (image < 0) { image = (int)((unsigned)(std::fabs(x) * 7.0 + std::fabs(z) * 13.0) % ims.size()); }
	const Size& im = ims[(size_t)image % ims.size()];
	Decal d = { type, (int)((size_t)image % ims.size()), x, z, 0, 0, dirRad, { colour[0], colour[1], colour[2], colour[3] } };
	// dc6b0: half extent = size * image px * 0.5 / 2048 * patch size
	d.halfW = (float)(size * im.w * kPatch / 4096.0);
	d.halfH = (float)(size * im.h * kPatch / 4096.0);
	decals_.push_back(d);
	dirty_ = true;
}

bool BloodDecals::pool(uint64_t key, double x, double z, float size, const float colour[4])
{
	for (auto it = decals_.rbegin(); it != decals_.rend(); ++it)
	{
		if (it->key != key) { continue; }
		// dcfa0: moving more than sqrt(10) from the pool starts a new one
		if ((it->x - x) * (it->x - x) + (it->z - z) * (it->z - z) > 10.0) { break; }
		const Size& im = sizes_[kPool][(size_t)it->image];
		it->halfW = (float)(size * im.w * kPatch / 4096.0);
		it->halfH = (float)(size * im.h * kPatch / 4096.0);
		it->life = 99.0f;
		dirty_ = true;
		return false;
	}
	const size_t n = decals_.size();
	add(kPool, x, z, size, (float)(std::fabs(x * 3.0 + z * 5.0)), colour);
	if (decals_.size() > n) { decals_.back().key = key; }
	return true;
}

void BloodDecals::clear()
{
	decals_.clear();
	dirty_ = true;
}

void BloodDecals::age(double dt, double speed)
{
	passClock_ += dt;
	if (passClock_ < 0.1) { return; }
	passClock_ = std::fmod(passClock_, 0.1);
	const float d = (float)(dt * speed);
	for (Decal& dec : decals_) { dec.life -= d; }
	const auto resident = [&](const Decal& dec)
	{
		const double r = std::hypot(dec.halfW, dec.halfH);
		for (int ix = (int)std::floor((dec.x - r + kHalfWorld) / kPatch); ix <= (int)std::floor((dec.x + r + kHalfWorld) / kPatch); ix++)
		{
			for (int iz = (int)std::floor((dec.z - r + kHalfWorld) / kPatch); iz <= (int)std::floor((dec.z + r + kHalfWorld) / kPatch); iz++)
			{
				if (std::find(slotPatch_, slotPatch_ + kSlots, patchKey(ix, iz)) != slotPatch_ + kSlots) { return true; }
			}
		}
		return false;
	};
	const size_t n = decals_.size();
	std::erase_if(decals_, [&](const Decal& dec) { return dec.life <= 0 && !resident(dec); });
	if (decals_.size() != n) { dirty_ = true; }
}

void BloodDecals::update(const double cam[3])
{
	// the patches each decal touches (dc6b0 inserts it into every patch its rotated quad overlaps; a bounding square here), counted for the slot choice
	std::map<long long, int> count;
	for (const Decal& d : decals_)
	{
		const double r = std::hypot(d.halfW, d.halfH);
		for (int ix = (int)std::floor((d.x - r + kHalfWorld) / kPatch); ix <= (int)std::floor((d.x + r + kHalfWorld) / kPatch); ix++)
		{
			for (int iz = (int)std::floor((d.z - r + kHalfWorld) / kPatch); iz <= (int)std::floor((d.z + r + kHalfWorld) / kPatch); iz++) { count[patchKey(ix, iz)]++; }
		}
	}
	// 1408dd860: patches within Decal Range, the most decals first, best 8 (the game also scores child patches; one level here)
	std::vector<std::pair<int, long long>> cand;
	for (const auto& [key, n] : count)
	{
		const double ox = (double)(int)(key >> 32) * kPatch - kHalfWorld, oz = (double)(int)(key & 0xffffffff) * kPatch - kHalfWorld;
		const double dx = std::max({ ox - cam[0], 0.0, cam[0] - ox - kPatch }), dz = std::max({ oz - cam[2], 0.0, cam[2] - oz - kPatch });
		if (dx * dx + dz * dz <= kRange * kRange) { cand.push_back({ -n, key }); }
	}
	std::sort(cand.begin(), cand.end());
	if (cand.size() > (size_t)kSlots) { cand.resize(kSlots); }
	// keep slots whose patch stays, give the free ones the rest
	long long next[kSlots];
	for (int s = 0; s < kSlots; s++) { next[s] = std::any_of(cand.begin(), cand.end(), [&](auto& c) { return c.second == slotPatch_[s]; }) ? slotPatch_[s] : -1; }
	for (const auto& c : cand)
	{
		if (std::find(next, next + kSlots, c.second) != next + kSlots) { continue; }
		for (long long& s : next) { if (s < 0) { s = c.second; break; } }
	}
	if (!std::equal(next, next + kSlots, slotPatch_)) { dirty_ = true; }
	std::copy(next, next + kSlots, slotPatch_);
	if (!dirty_) { return; }
	dirty_ = false;
	for (int s = 0; s < kSlots; s++)
	{
		const long long key = slotPatch_[s];
		patch_[s][3] = key < 0 ? 0.0f : 1.0f;
		if (key < 0) { continue; }
		patch_[s][0] = (float)((double)(int)(key >> 32) * kPatch - kHalfWorld);
		patch_[s][1] = (float)((double)(int)(key & 0xffffffff) * kPatch - kHalfWorld);
		patch_[s][2] = (float)(1.0 / kPatch);
	}
	// the repaint job: the slots and a copy of the decals that touch them (a packet is a pure snapshot; this one-shot paint travels in the queue)
	Paint p;
	std::copy(slotPatch_, slotPatch_ + kSlots, p.slotPatch);
	std::copy(&patch_[0][0], &patch_[0][0] + kSlots * 4, &p.patch[0][0]);
	if (!cand.empty())
	{
		for (const Decal& d : decals_)
		{
			const double r = std::hypot(d.halfW, d.halfH);
			for (int s = 0; s < kSlots; s++)
			{
				if (p.slotPatch[s] < 0) { continue; }
				const double ox = p.patch[s][0], oz = p.patch[s][1];
				if (d.x + r < ox || d.x - r > ox + kPatch || d.z + r < oz || d.z - r > oz + kPatch) { continue; }
				p.decals.push_back(d);
				break;
			}
		}
	}
	const size_t bytes = p.decals.size() * sizeof(Decal);
	rq::push("blood-paint", [this, p = std::move(p), draw = !cand.empty()] {
		std::copy(&p.patch[0][0], &p.patch[0][0] + kSlots * 4, &gpuPatch_[0][0]);
		if (draw) { this->paint(p); }
	}, rq::Lane::kFrame, bytes);
}

// ---- render side ----

void BloodDecals::paint(const Paint& pt)
{
	if (!atlas_.id)
	{
		sg_image_desc id = {};
		id.usage.color_attachment = true;
		id.width = 4 * kTile;
		id.height = 2 * kTile;
		id.pixel_format = SG_PIXELFORMAT_RGBA8;
		id.sample_count = 1;
		id.label = "blood-atlas";
		atlas_ = sg_make_image(&id);
		sg_view_desc vd = {};
		vd.texture.image = atlas_;
		atlasView_ = sg_make_view(&vd);
		vd = {};
		vd.color_attachment.image = atlas_;
		atlasAtt_ = sg_make_view(&vd);
	}
	// the game clears a slot when it is reassigned and paints new decals on top; here every slot is cleared and repainted whenever anything changed
	sg_pass p = {};
	p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	p.action.colors[0].clear_value = { 0, 0, 0, 0 };
	p.attachments.colors[0] = atlasAtt_;
	sg_begin_pass(&p);
	drawstats::pipeline(pip_);
	for (int s = 0; s < kSlots; s++)
	{
		if (pt.slotPatch[s] < 0) { continue; }
		// GL: framebuffer row 0 is texture v 0, so tile NDC y = 2 v - 1 with v = (z - origin z) / patch (the game's D3D quad flips y instead: 1 - 2 y)
		sg_apply_viewport((s % 4) * kTile, (s / 4) * kTile, kTile, kTile, false);
		const double ox = pt.patch[s][0], oz = pt.patch[s][1], k = 2.0 / kPatch;
		for (const Decal& d : pt.decals)
		{
			const double r = std::hypot(d.halfW, d.halfH);
			if (d.x + r < ox || d.x - r > ox + kPatch || d.z + r < oz || d.z - r > oz + kPatch) { continue; }
			// the image's u axis a along the direction, v axis b across it. MINE: the game rotates by rot - pi/2 with rot = atan2 of the direction (x / z order UNSURE);
			// the smear (line.dds, a 512 x 256 streak along u) then lies along the motion, a drag trail
			const double ax = std::cos(d.rot), az = std::sin(d.rot), bx = -az, bz = ax;
			decal_vs_params_t vs = {};
			vs.row0[0] = (float)(k * 2 * d.halfW * ax); vs.row0[1] = (float)(k * 2 * d.halfH * bx); vs.row0[2] = (float)(k * (d.x - ox) - 1);
			vs.row1[0] = (float)(k * 2 * d.halfW * az); vs.row1[1] = (float)(k * 2 * d.halfH * bz); vs.row1[2] = (float)(k * (d.z - oz) - 1);
			decal_fs_params_t fs = {};
			for (int c = 0; c < 4; c++) { fs.colour[c] = d.colour[c]; }
			fs.colour[3] = std::min(d.colour[3], d.life);
			sg_bindings b = {};
			b.vertex_buffers[0] = quad_;
			b.views[VIEW_decal_tex] = images_[d.type][(size_t)d.image].view;
			b.samplers[SMP_decal_smp] = imgSmp_;
			drawstats::bindings(&b);
			sg_apply_uniforms(UB_decal_vs_params, SG_RANGE(vs));
			sg_apply_uniforms(UB_decal_fs_params, SG_RANGE(fs));
			drawstats::draw(0, 4, 1);
		}
	}
	sg_end_pass();
}
