#include "render/interiorclip.hpp"
#include "render/drawstats.hpp"
#include "render/terrain.hpp"
#include "render/ao.hpp"
#include "render/speccube.hpp"
#include "render/weathermap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "stb_image.h"
#include "terrain.glsl.h"
#include "kworld/coords.hpp"

namespace
{

constexpr double kUnit = kworld::kHeightmapSpacing;
constexpr double kHalf = -kworld::kWorldMin;
constexpr float kMaxHeight = (float)kworld::kMaxHeight;
constexpr double kLodFactor = 2.0;	// a node of size S is split while the camera is closer than S * this
constexpr float kSkirtCells = 6.0f;	// skirt depth in cells of the node's own resolution

}

bool Terrain::init(const kworld::Heightmap* hm, const std::string& biomePath)
{
	hm_ = hm;
	int bw = 0, bh = 0, comp = 0;
	stbi_uc* bpx = stbi_load(biomePath.c_str(), &bw, &bh, &comp, 4);
	if (!bpx) { return false; }

	// Vertex grid -1..cells+1 on each axis; the outer ring is the skirt.
	const int n = kCells + 3;
	std::vector<float> verts;
	verts.reserve((size_t)n * n * 2);
	for (int j = 0; j < n; j++)
	{
		for (int i = 0; i < n; i++) { verts.push_back((float)(i - 1)); verts.push_back((float)(j - 1)); }
	}
	std::vector<uint16_t> idx;
	idx.reserve((size_t)(n - 1) * (n - 1) * 6);
	for (int j = 0; j < n - 1; j++)
	{
		for (int i = 0; i < n - 1; i++)
		{
			uint16_t a = (uint16_t)(j * n + i), b = (uint16_t)(a + 1), c = (uint16_t)(a + n), d = (uint16_t)(c + 1);
			uint16_t t[6] = { a, c, b, b, c, d };
			idx.insert(idx.end(), t, t + 6);
		}
	}

	sg_buffer_desc bd = {};
	bd.data = { verts.data(), verts.size() * sizeof(float) };
	bd.label = "terrain-verts";
	vbuf_ = sg_make_buffer(&bd);
	bd = {};
	bd.usage.index_buffer = true;
	bd.data = { idx.data(), idx.size() * sizeof(uint16_t) };
	bd.label = "terrain-indices";
	ibuf_ = sg_make_buffer(&bd);

	sg_sampler_desc sd = {};	// nearest, clamp: heights are fetched with texelFetch
	sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
	sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	smp_ = sg_make_sampler(&sd);

	sg_image_desc bid = {};
	bid.width = bw;
	bid.height = bh;
	bid.pixel_format = SG_PIXELFORMAT_RGBA8;
	bid.data ={ bpx, (size_t)bw * bh * 4 };
	bid.label = "biomemap";
	biomeImg_ = sg_make_image(&bid);
	stbi_image_free(bpx);
	sg_view_desc bvd = {};
	bvd.texture.image = biomeImg_;
	biomeView_ = sg_make_view(&bvd);
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	biomeSmp_ = sg_make_sampler(&sd);

	return true;
}

bool Terrain::initColour(bool loaded)	// loaded: GroundColour::load's result (a worker ran it, kenshi-k16.2.26); the uploads are here, on the main thread
{
	hasColour = loaded && colour.upload();
	if (!hasColour)
	{
		fprintf(stderr, "kclient: ground colour unavailable, using the biome map\n");
		viewMode = 1;
		colour.view = biomeView_;	// any valid view: the shader never samples it in this mode
		colour.smp = biomeSmp_;
		colour.alphaView = biomeView_;
	}

	return true;
}

int Terrain::initDetailPart(TerrainDetail::Prepared& prepared, int part)	// one part a frame (kenshi-k16.2.26): 0-2 the uploads, 3 the pipelines
{
	if (part < 3)
	{
		if (hasColour && !detailFailed_ && detail.uploadPart(prepared, part) < 0) { detailFailed_ = true; fprintf(stderr, "kclient: terrain detail unavailable, colour map only\n"); viewMode = 3; }
		return 1;
	}
	shd_ = sg_make_shader(terrain_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.attrs[ATTR_terrain_grid].format = SG_VERTEXFORMAT_FLOAT2;
	pd.index_type = SG_INDEXTYPE_UINT16;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "terrain";
	pip_ = sg_make_pipeline(&pd);
	depthShd_ = sg_make_shader(terrain_depth_shader_desc(sg_query_backend()));
	pd.shader = depthShd_;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
	pd.color_count = 0;
	pd.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
	pd.sample_count = 1;
	pd.label = "terrain-shadow";
	depthPip_ = sg_make_pipeline(&pd);
	return 0;
}

bool Terrain::initDetail(TerrainDetail::Prepared& prepared)	// all parts at once (the one-call form)
{
	for (int part = 0; part < 4; part++) { if (initDetailPart(prepared, part) == 0) { return true; } }
	return false;
}

void Terrain::shutdown()
{
	for (auto& [k, c] : chunks_)
	{
		sg_destroy_view(c.view);
		sg_destroy_image(c.image);
	}
	chunks_.clear();
	detail.shutdown();
	if (hasColour) { colour.shutdown(); }
	sg_destroy_sampler(biomeSmp_);
	sg_destroy_view(biomeView_);
	sg_destroy_image(biomeImg_);
	sg_destroy_pipeline(depthPip_);
	sg_destroy_shader(depthShd_);
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
	sg_destroy_sampler(smp_);
	sg_destroy_buffer(ibuf_);
	sg_destroy_buffer(vbuf_);
}

Terrain::Chunk* Terrain::load(int level, int ix, int iz)
{
	auto it = chunks_.find(key(level, ix, iz));
	if (it != chunks_.end()) { return &it->second; }
	if (budget_ == 0) { return nullptr; }
	if (budget_ > 0) { budget_--; }

	const int stride = 1 << level;
	const int col0 = ix * kCells * stride, row0 = iz * kCells * stride;
	std::vector<float> h((size_t)kTex * kTex);
	float minH = kMaxHeight;
	for (int j = 0; j < kTex; j++)
	{
		for (int i = 0; i < kTex; i++) { minH = std::min(minH, h[(size_t)j * kTex + i] = (float)hm_->sampleHeight(std::clamp(col0 + (i - 1) * stride, 0, hm_->sampleCountX() - 1), std::clamp(row0 + (j - 1) * stride, 0, hm_->sampleCountZ() - 1))); }
	}
	sg_image_desc id = {};
	id.width = kTex;
	id.height = kTex;
	id.pixel_format = SG_PIXELFORMAT_R32F;
	id.data = { h.data(), h.size() * sizeof(float) };
	id.label = "terrain-chunk";
	Chunk c = {};
	c.minH = minH;
	c.image = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = c.image;
	c.view = sg_make_view(&vd);
	chunkGen_++;	// the shadow cache (kenshi-k16.2.205.12.2): a chunk became resident, the cascades' terrain draw lists may change
	return &chunks_.emplace(key(level, ix, iz), c).first->second;
}

void Terrain::select(int level, int ix, int iz)
{
	const double size = (double)kCells * kUnit * (double)(1 << level);
	const double x0 = -kHalf + ix * size, z0 = -kHalf + iz * size;

	// Frustum cull against side planes (camera-relative box, full height range).
	const float lo[3] = { (float)(x0 - cam_[0]), (float)(0.0 - cam_[1]), (float)(z0 - cam_[2]) };
	const float hi[3] = { (float)(x0 + size - cam_[0]), (float)(kMaxHeight - cam_[1]), (float)(z0 + size - cam_[2]) };
	for (const auto& p : planes_)
	{
		float px = p[0] > 0 ? hi[0] : lo[0], py = p[1] > 0 ? hi[1] : lo[1], pz = p[2] > 0 ? hi[2] : lo[2];
		if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0) { return; }
	}

	const double dx = std::max({ x0 - cam_[0], 0.0, cam_[0] - (x0 + size) });
	const double dz = std::max({ z0 - cam_[2], 0.0, cam_[2] - (z0 + size) });
	const double dy = std::max(0.0, cam_[1] - groundAtCam_);
	const double dist = std::sqrt(dx * dx + dz * dz + dy * dy);
	if (level > 0 && dist < size * kLodFactor)
	{
		for (int k = 0; k < 4; k++) { select(level - 1, ix * 2 + (k & 1), iz * 2 + (k >> 1)); }
		return;
	}

	Chunk* c = load(level, ix, iz);
	int l = level, x = ix, z = iz;
	while (!c && l < kMaxLevel)	// not loaded yet: borrow the nearest resident ancestor
	{
		l++; x >>= 1; z >>= 1;
		auto it = chunks_.find(key(l, x, z));
		c = it == chunks_.end() ? nullptr : &it->second;
	}
	if (!c) { return; }
	c->lastUsed = frame_;
	for (const Draw& d : draws_) { if (d.level == l && d.ix == x && d.iz == z) { return; } }
	draws_.push_back({ l, x, z });
}

void Terrain::evict()
{
	if (chunks_.size() <= kMaxResident) { return; }
	std::vector<std::pair<uint64_t, uint64_t>> order;
	for (auto& [k, c] : chunks_) { if (c.lastUsed < frame_) { order.push_back({ c.lastUsed, k }); } }
	std::sort(order.begin(), order.end());
	for (size_t i = 0; i < order.size() && chunks_.size() > kMaxResident * 3 / 4; i++)
	{
		Chunk& c = chunks_[order[i].second];
		sg_destroy_view(c.view);
		sg_destroy_image(c.image);
		chunks_.erase(order[i].second);
		chunkGen_++;
	}
}

void Terrain::placement(const Draw& d, float origin[4], float chunk[4]) const
{
	const double size = (double)kCells * kUnit * (double)(1 << d.level);
	const float cell = (float)(kUnit * (double)(1 << d.level));
	origin[0] = (float)(-kHalf + d.ix * size - cam_[0]);
	origin[1] = (float)(-cam_[1]);
	origin[2] = (float)(-kHalf + d.iz * size - cam_[2]);
	origin[3] = cell;
	chunk[0] = cell * kSkirtCells;
	chunk[1] = (float)kCells;
	chunk[2] = (float)(d.ix * size);
	chunk[3] = (float)(d.iz * size);
}

void Terrain::waterChunks(float level, std::vector<WaterChunk>& out) const
{
	out.clear();
	for (const Draw& d : draws_)
	{
		const Chunk& c = chunks_.at(key(d.level, d.ix, d.iz));
		if (c.minH >= level) { continue; }
		WaterChunk w = { c.view, {}, {} };
		placement(d, w.origin, w.chunk);
		out.push_back(w);
	}
}

void Terrain::render(const double cam[3], const float m[16], const LightParams& light, int maxLoads, int view)
{
	frame_++;
	budget_ = maxLoads;
	memcpy(cam_, cam, sizeof cam_);
	groundAtCam_ = hm_->height(cam[0], cam[2]);
	// Gribb-Hartmann side planes from the column-major matrix (row i = m[i], m[4+i], m[8+i], m[12+i]).
	for (int k = 0; k < 4; k++)
	{
		const int axis = k >> 1;
		const float s = (k & 1) ? -1.0f : 1.0f;
		for (int c = 0; c < 4; c++) { planes_[k][c] = m[c * 4 + 3] + s * m[c * 4 + axis]; }
	}
	evict();
	draws_.clear();
	{
		budget_ = -1;	// the root is the universal fallback, it must always exist
		load(kMaxLevel, 0, 0)->lastUsed = frame_;
		budget_ = maxLoads;
	}
	select(kMaxLevel, 0, 0);
	viewDraws_ = draws_;
	drawList(draws_, m, light, view);
	drawn_ = (int)draws_.size();
}

void Terrain::renderView(const float m[16], const LightParams& light, int view)
{
	drawList(viewDraws_, m, light, view);
}

void Terrain::drawList(const std::vector<Draw>& list, const float m[16], const LightParams& light, int view)
{
	drawstats::pipeline(pip_);
	static_assert(sizeof(light_params_t) == sizeof(LightParams));
	const bool splat = detail.ok && view == 0;
	for (const Draw& d : list)
	{
		const auto chunk = chunks_.find(key(d.level, d.ix, d.iz));
		if (chunk == chunks_.end()) { continue; }	// renderView: evicted since the selection
		vs_params_t vs = {};
		memcpy(vs.mvp, m, sizeof vs.mvp);
		placement(d, vs.origin, vs.chunk);
		fs_params_t fs = {};
		fs.mode[0] = (float)(view == 0 && !detail.ok ? 3 : view);
		if (splat)
		{
			const double size = (double)kCells * kUnit * (double)(1 << d.level);
			const TerrainDetail::ChunkBiomes cb = detail.biomesFor(-kHalf + d.ix * size, -kHalf + d.iz * size, size);
			fs.mode[1] = (float)cb.count;
			memcpy(fs.brow, cb.row, sizeof fs.brow);
			memcpy(fs.bchan, cb.chan, sizeof fs.bchan);
		}
		sg_bindings b = {};
		b.vertex_buffers[0] = vbuf_;
		b.index_buffer = ibuf_;
		b.views[VIEW_hmap] = chunk->second.view;
		b.samplers[SMP_smp] = smp_;
		b.views[VIEW_shadow_tex] = shadow->view();
		b.samplers[SMP_shadow_smp] = shadow->sampler();
		b.samplers[SMP_shadow_raw] = shadow->rawSampler();
		b.views[VIEW_biome] = biomeView_;
		b.samplers[SMP_bsmp] = biomeSmp_;
		b.views[VIEW_ground] = colour.view;
		b.samplers[SMP_gsmp] = colour.smp;
		b.views[VIEW_ground_alpha] = colour.alphaView;
		b.views[VIEW_imask_tex] = interiorclip::view();
		b.samplers[SMP_imask_smp] = interiorclip::sampler();
		b.views[VIEW_fog_map] = fog_map_binding().view;
		b.views[VIEW_spec_cube] = spec_cube_binding().view;
		b.views[VIEW_irr_cube] = irr_cube_binding().view;
		b.samplers[SMP_spec_smp] = spec_cube_binding().smp;
		b.views[VIEW_ao_tex] = ao_binding().view;
		b.views[VIEW_il_tex] = ao_binding().il;
		b.samplers[SMP_ao_smp] = ao_binding().smp;
		b.samplers[SMP_fog_smp] = fog_map_binding().smp;
		b.views[VIEW_blood_atlas] = blood->atlas();
		b.samplers[SMP_blood_smp] = blood->sampler();
		memcpy(fs.blood_patch, blood->patches(), sizeof fs.blood_patch);
		if (detail.ok)
		{
			b.views[VIEW_diffuse_maps] = detail.diffuse;
			b.views[VIEW_normal_maps] = detail.normal;
			b.views[VIEW_overlay_map] = detail.overlay;
			b.views[VIEW_blend_map] = detail.blendView;
			b.views[VIEW_biome_table] = detail.biomeTable;
			b.views[VIEW_ground_map] = detail.groundView;
			b.samplers[SMP_asmp] = detail.arraySmp;
			b.samplers[SMP_lsmp] = detail.linearSmp;
			b.samplers[SMP_tsmp] = detail.tableSmp;
		}
		drawstats::bindings(&b);
		sg_apply_uniforms(UB_vs_params, SG_RANGE(vs));
		sg_apply_uniforms(UB_fs_params, SG_RANGE(fs));
		sg_apply_uniforms(UB_light_params, { &light, sizeof light });
		sg_apply_uniforms(UB_ao_params, SG_RANGE(ao_binding().params));
		sg_apply_uniforms(UB_shadow_params, { &shadow->params(), sizeof(ShadowParams) });
		drawstats::draw(0, (kCells + 2) * (kCells + 2) * 6, 1);
	}
}

// Same chunk selection as render() (so the geometry matches what receivers see where both are drawn), but culled by the cascade's box instead of the camera frustum.
void Terrain::renderShadow(const double cam[3], const ShadowCascade& c, int maxLoads)
{
	memcpy(cam_, cam, sizeof cam_);
	groundAtCam_ = hm_->height(cam[0], cam[2]);
	std::vector<Draw> saved;
	saved.swap(draws_);
	float savedPlanes[4][4];
	memcpy(savedPlanes, planes_, sizeof planes_);
	memcpy(planes_, c.planes, sizeof planes_);
	budget_ = -1;	// the root is the universal fallback, it must always exist
	load(kMaxLevel, 0, 0)->lastUsed = frame_;
	budget_ = maxLoads;
	select(kMaxLevel, 0, 0);
	drawstats::pipeline(depthPip_);
	depth_fs_params_t fs = {};
	memcpy(fs.bias, c.bias, sizeof fs.bias);
	for (const Draw& d : draws_)
	{
		vs_params_t vs = {};
		memcpy(vs.mvp, c.lvp, sizeof vs.mvp);
		placement(d, vs.origin, vs.chunk);
		sg_bindings b = {};
		b.vertex_buffers[0] = vbuf_;
		b.index_buffer = ibuf_;
		b.views[VIEW_hmap] = chunks_[key(d.level, d.ix, d.iz)].view;
		b.samplers[SMP_smp] = smp_;
		drawstats::bindings(&b);
		sg_apply_uniforms(UB_vs_params, SG_RANGE(vs));
		sg_apply_uniforms(UB_depth_fs_params, SG_RANGE(fs));
		drawstats::draw(0, (kCells + 2) * (kCells + 2) * 6, 1);
	}
	draws_.swap(saved);
	memcpy(planes_, savedPlanes, sizeof planes_);
}
