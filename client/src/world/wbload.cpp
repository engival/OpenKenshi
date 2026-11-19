#include "world/worldbuildings.hpp"
#include "world/worldbuildings_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "interiormask.glsl.h"
#include "kworld/coords.hpp"
#include "mesh.glsl.h"
#include "stb_image.h"

namespace
{
constexpr size_t kMaxMeshes = 2500, kMaxTextures = 1200;
constexpr int kWorkers = 4;

}

using namespace wbi;

void WorldBuildings::init(const ResourceIndex* res, const MaterialLib* mats)
{
	res_ = res;
	mats_ = mats;
	shd_ = sg_make_shader(building_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
	pd.layout.buffers[1].stride = kInstanceBytes;
	pd.layout.attrs[ATTR_building_position] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_building_normal] = { .buffer_index = 0, .offset = 12, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_building_texcoord0] = { .buffer_index = 0, .offset = 24, .format = SG_VERTEXFORMAT_FLOAT2 };
	pd.layout.attrs[ATTR_building_colour] = { .buffer_index = 0, .offset = 32, .format = SG_VERTEXFORMAT_UBYTE4N };
	pd.layout.attrs[ATTR_building_tangent] = { .buffer_index = 0, .offset = 36, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_building_inst_pos] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_building_inst_quat] = { .buffer_index = 1, .offset = 16, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_building_inst_stretch] = { .buffer_index = 1, .offset = 32, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.buffers[0].stride = kVertexBytes;
	pd.index_type = SG_INDEXTYPE_UINT32;
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "buildings";
	pip_ = sg_make_pipeline(&pd);
	// Single-sided parts (cullsBackFaces: the exterior shell with its window panes, and interior parts) cull back faces, as the exe's StaticObject does (kenshi-rwh.131,
	// Outpost panes kenshi-k16.2.211.3): the side drawn is the outside, so a camera outside sees a pane and one inside the room sees through it, and interior ceilings
	// and the wall faces turned away from a camera looking in stop hiding the room. The label keeps "interior" (that case came first).
	pd.cull_mode = SG_CULLMODE_BACK;
	pd.face_winding = SG_FACEWINDING_CCW;
	pd.label = "buildings-interior";
	pipInterior_ = sg_make_pipeline(&pd);
	// The interior mask (renderMask): the hulls into the RG16F mask target, back faces into G then front faces into R, no depth, no texture
	// (interiormask.material: two render_scene passes at the STENCIL queues, colour_write g / r).
	{
		maskShd_ = sg_make_shader(interiormask_shader_desc(sg_query_backend()));
		sg_pipeline_desc md = {};
		md.shader = maskShd_;
		md.layout.buffers[0].stride = kVertexBytes;
		md.layout.buffers[1].stride = kInstanceBytes;
		md.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
		md.layout.attrs[ATTR_interiormask_position] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
		md.layout.attrs[ATTR_interiormask_inst_pos] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT4 };
		md.layout.attrs[ATTR_interiormask_inst_quat] = { .buffer_index = 1, .offset = 16, .format = SG_VERTEXFORMAT_FLOAT4 };
		md.layout.attrs[ATTR_interiormask_inst_stretch] = { .buffer_index = 1, .offset = 32, .format = SG_VERTEXFORMAT_FLOAT4 };
		md.index_type = SG_INDEXTYPE_UINT32;
		md.colors[0].pixel_format = SG_PIXELFORMAT_RG16F;
		md.depth.pixel_format = SG_PIXELFORMAT_NONE;
		md.face_winding = SG_FACEWINDING_CCW;	// the meshes' convention, as pipInterior_
		// The decoded windings are mirrored against the game's, so the face sokol keeps as BACK is the hull's far side (measured on the Hub bar's mask:
		// G 129.0 against R 130.1 at the centre pixel before this swap). G = far, R = near, the order basic.hlsl CLIP_INTERIOR expects.
		md.cull_mode = SG_CULLMODE_BACK;	// the far faces of a hull
		md.colors[0].write_mask = SG_COLORMASK_G;
		md.label = "interior-mask-back";
		pipMaskBack_ = sg_make_pipeline(&md);
		md.cull_mode = SG_CULLMODE_FRONT;	// the near faces
		md.colors[0].write_mask = SG_COLORMASK_R;
		md.label = "interior-mask-front";
		pipMaskFront_ = sg_make_pipeline(&md);
	}
	// peeked parts (renderGlass): their nearest surface into the depth buffer first (any winding, like pip_), then blended over what is drawn at
	// exactly that depth, so one even layer and no walls stacked behind it
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.colors[0].write_mask = SG_COLORMASK_NONE;
	pd.label = "buildings-glass-depth";
	pipGlassDepth_ = sg_make_pipeline(&pd);
	pd.colors[0].write_mask = SG_COLORMASK_RGBA;
	pd.depth.write_enabled = false;
	pd.depth.compare = SG_COMPAREFUNC_EQUAL;
	pd.colors[0].blend = { .enabled = true, .src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA, .dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
		.src_factor_alpha = SG_BLENDFACTOR_ZERO, .dst_factor_alpha = SG_BLENDFACTOR_ONE };
	pd.label = "buildings-glass";
	pipGlass_ = sg_make_pipeline(&pd);
	// the cut-away flip fade (renderFlip): the faces pipInterior_ drops, blended in at the depth test, no depth write
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.cull_mode = SG_CULLMODE_FRONT;
	pd.label = "buildings-flip";
	pipFlip_ = sg_make_pipeline(&pd);
	pd.colors[0].blend = {};
	pd.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
	pd.depth.write_enabled = true;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.face_winding = SG_FACEWINDING_CCW;
	// the cursor ID pass (renderIds): the same vertices, only where they made the scene's depth (EQUAL), into the RGBA32F ID target
	{
		sg_pipeline_desc ip = pd;
		idShd_ = sg_make_shader(building_id_shader_desc(sg_query_backend()));
		ip.shader = idShd_;
		ip.depth.compare = SG_COMPAREFUNC_EQUAL;
		ip.depth.write_enabled = false;
		ip.colors[0].pixel_format = SG_PIXELFORMAT_RGBA32F;
		ip.colors[0].write_mask = SG_COLORMASK_RGBA;
		ip.label = "buildings-id";
		pipId_ = sg_make_pipeline(&ip);
		ip.cull_mode = SG_CULLMODE_BACK;
		ip.label = "buildings-id-interior";
		pipIdInterior_ = sg_make_pipeline(&ip);
	}
	shadowShd_ = sg_make_shader(building_shadow_shader_desc(sg_query_backend()));
	pd.shader = shadowShd_;
	pd.layout.attrs[ATTR_building_shadow_position] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_building_shadow_normal] = { .buffer_index = 0, .offset = 12, .format = SG_VERTEXFORMAT_FLOAT3 };
	pd.layout.attrs[ATTR_building_shadow_texcoord0] = { .buffer_index = 0, .offset = 24, .format = SG_VERTEXFORMAT_FLOAT2 };
	pd.layout.attrs[ATTR_building_shadow_colour] = { .buffer_index = 0, .offset = 32, .format = SG_VERTEXFORMAT_UBYTE4N };
	pd.layout.attrs[ATTR_building_shadow_tangent] = { .buffer_index = 0, .offset = 36, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_building_shadow_inst_pos] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_building_shadow_inst_quat] = { .buffer_index = 1, .offset = 16, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.layout.attrs[ATTR_building_shadow_inst_stretch] = { .buffer_index = 1, .offset = 32, .format = SG_VERTEXFORMAT_FLOAT4 };
	pd.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
	pd.color_count = 0;
	pd.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
	pd.sample_count = 1;
	pd.label = "buildings-shadow";
	shadowPip_ = sg_make_pipeline(&pd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;
	sd.max_anisotropy = 8;
	smp_ = sg_make_sampler(&sd);
	const uint32_t grey = 0xff9a9a9a;
	sg_image_desc id = {};
	id.width = id.height = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = { &grey, sizeof grey };
	greyImg_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = greyImg_;
	grey_ = sg_make_view(&vd);
	blood_ = grey_;
	const uint32_t flat = 0xffff8080;	// a flat normal map (0, 0, 1), alpha 1: for parts and submeshes without one
	id.data = { &flat, sizeof flat };
	flatImg_ = sg_make_image(&id);
	vd.texture.image = flatImg_;
	flat_ = sg_make_view(&vd);
	// the dust coat's noise (the game binds Turbulent.dds as the `dust` unit of every DUST material; kenshi-inspect findings/surface_dust_coat.md)
	{
		const std::string path = res_->find("Turbulent.dds");
		DdsImage dds;
		std::string err;
		if (path.empty() || !dds_load(path, dds, err)) { fprintf(stderr, "kclient: buildings: dust noise Turbulent.dds not loaded (%s): the dust coat draws grey noise\n", path.empty() ? "not found" : err.c_str()); }
		else { dustImg_ = dds_make_image(dds, "dust-noise"); }
		vd.texture.image = dustImg_;
		dust_ = sg_query_image_state(dustImg_) == SG_RESOURCESTATE_VALID ? sg_make_view(&vd) : grey_;
	}
	// a ground severed limb's blood map (character.material SeveredLimb: materials/bloody.png, wrap), the same file the body shader binds (characters.cpp)
	{
		const std::string path = res_->find("bloody.png");
		int w = 0, h = 0, comp = 0;
		stbi_uc* px = path.empty() ? nullptr : stbi_load(path.c_str(), &w, &h, &comp, 4);
		if (px)
		{
			id.width = w;
			id.height = h;
			id.data = { px, (size_t)w * h * 4 };
			id.label = "limb-blood";
			bloodImg_ = sg_make_image(&id);
			stbi_image_free(px);
			vd.texture.image = bloodImg_;
			blood_ = sg_make_view(&vd);
		}
		else { fprintf(stderr, "kclient: buildings: blood map bloody.png not loaded (%s): a severed limb draws without blood\n", path.empty() ? "not found" : "unreadable png"); }
	}
	sg_buffer_desc bd = {};
	bd.usage.vertex_buffer = true;
	bd.usage.write_transient = true;
	bd.size = kMaxInstances * kInstanceBytes;
	bd.label = "building-instances";
	instBuf_ = sg_make_buffer(&bd);
	for (int i = 0; i < kWorkers; i++) { threads_.emplace_back([this] { worker(); }); }
}

void WorldBuildings::stopThreads()
{
	{
		std::lock_guard<std::mutex> lk(mu_);
		quit_ = true;
	}
	jobCv_.notify_all();
	const auto t0 = std::chrono::steady_clock::now();
	lightbake::cancel();	// an in-flight probe/sun bake stops at its next chunk: the joins below take milliseconds
	for (std::thread& t : threads_) { if (t.joinable()) { t.join(); } }
	if (probeThread_.joinable()) { probeThread_.join(); }
	if (occThread_.joinable()) { occThread_.join(); }
	if (sunThread_.joinable()) { sunThread_.join(); }
	threads_.clear();
	fprintf(stderr, "kclient: worldbuildings threads stopped in %lld ms\n", (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

void WorldBuildings::shutdown()
{
	stopThreads();
	// cleanup(), the render thread after rq::flushAll: every create has run, every evict's destroy too (their Handles are not in the entries any more)
	for (MeshEntry& m : meshes_) { if (m.gpu.ready()) { sg_destroy_buffer(m.gpu.get().vbuf); sg_destroy_buffer(m.gpu.get().ibuf); } }
	for (TexEntry& t : textures_) { if (t.gpu.ready() && t.gpu.get().view.id != SG_INVALID_ID) { sg_destroy_view(t.gpu.get().view); sg_destroy_image(t.gpu.get().img); } }
	for (PartRt& rt : partRt_) { if (rt.openIbuf.ready()) { sg_destroy_buffer(rt.openIbuf.get()); } }
	uploading_.clear();
	meshes_.clear();
	textures_.clear();
	sg_destroy_buffer(instBuf_);
	sg_destroy_view(grey_);
	sg_destroy_image(greyImg_);
	sg_destroy_view(flat_);
	sg_destroy_image(flatImg_);
	if (dust_.id != grey_.id) { sg_destroy_view(dust_); }
	sg_destroy_image(dustImg_);
	if (blood_.id != grey_.id) { sg_destroy_view(blood_); }
	sg_destroy_image(bloodImg_);
	sg_destroy_sampler(smp_);
	sg_destroy_pipeline(shadowPip_);
	sg_destroy_shader(shadowShd_);
	sg_destroy_pipeline(pip_);
	sg_destroy_pipeline(pipInterior_);
	sg_destroy_pipeline(pipGlassDepth_);
	sg_destroy_pipeline(pipGlass_);
	sg_destroy_pipeline(pipFlip_);
	sg_destroy_shader(shd_);
	sg_destroy_pipeline(pipMaskFront_);
	sg_destroy_pipeline(pipMaskBack_);
	sg_destroy_shader(maskShd_);
	sg_destroy_pipeline(pipId_);
	sg_destroy_pipeline(pipIdInterior_);
	sg_destroy_shader(idShd_);
}

void WorldBuildings::setData(BuildingSet&& data)
{
	dataGen_++;
	data_ = std::move(data);
	for (PartRt& rt : partRt_) { if (rt.openHas) { rq::push("buildings.open-free", [h = rt.openIbuf] { sg_destroy_buffer(h.get()); }, rq::Lane::kFrame); } }
	partRt_.assign(data_.parts.size(), PartRt());
	partStamp_.assign(data_.parts.size(), 0);
	motion_.assign(data_.parts.size(), Motion());
	turning_.clear();
	for (uint32_t pid = 0; pid < data_.parts.size(); pid++)
	{
		if (data_.parts[pid].aimed || spinning(data_.parts[pid])) { turning_.push_back(pid); }
	}
	binfo_.clear();
	furnFloor_.assign(data_.instances.size(), -2);
	floor_ = FloorStatus();
	focus_ = -1;
	cuts_.clear();
	staticCount_ = data_.instances.size();
	propList_.clear();
	propMeshes_.clear();
	propLayout_.clear();
	furnGrid_.clear();
	layoutPieces_.clear();
	topLevel_.clear();
	topFloors_.clear();
	for (size_t i = 0; i < staticCount_; i++)
	{
		if (data_.instances[i].parent >= 0 && !data_.instances[i].door && data_.instances[i].partCount > 0) { furnGrid_[furnKey(data_.instances[i].x, data_.instances[i].z)].push_back((uint32_t)i); }
		if (data_.instances[i].parent >= 0 && data_.instances[i].key != 0) { layoutPieces_.emplace(std::make_pair(data_.instances[i].key, data_.instances[i].piece), (uint32_t)i); }	// the wire's container tail names it
		if (!data_.instances[i].feature && data_.instances[i].parent < 0)
		{
			topLevel_.push_back((uint32_t)i);
			if (data_.instances[i].maxFloor != 0) { topFloors_.push_back((uint32_t)i); }
		}
	}
}

void WorldBuildings::setProps(const std::vector<Prop>& props)
{
	dataGen_++;
	data_.instances.resize(staticCount_);
	propList_.clear();
	propMeshes_.clear();
	propLayout_.clear();
	std::erase_if(construction_, [this](const auto& kv) { return kv.first >= staticCount_; });	// the last call's player buildings
	for (const Prop& p : props)
	{
		if (p.hasPiece && p.pieceBuilding != 0)	// a container the sim names a layout piece for (SIMLINK kind 3 tail): that piece draws and picks it, never a mesh of its own
		{
			if (const int instance = layoutPieceInstance(p.pieceBuilding, p.pieceIndex); instance >= 0)
			{
				propMeshes_[p.id] = (uint32_t)instance;
				propLayout_.insert(p.id);
				continue;
			}
		}
		if (p.building)	// a player's building: the record's shell parts as a placement of it is drawn, its construction cut from the progress
	{
		const auto shell = data_.shells.find(p.sid);
		if (shell == data_.shells.end()) { continue; }
		BuildingInstance b;
		b.x = p.pos[0]; b.y = p.pos[1]; b.z = p.pos[2];
		b.ground = (float)p.pos[1];
		b.qw = p.rot[0]; b.qx = p.rot[1]; b.qy = p.rot[2]; b.qz = p.rot[3];
		b.firstPart = shell->second.first;
		b.partCount = shell->second.count;
		b.height = shell->second.height;
		b.nameId = shell->second.nameId;
		const uint32_t index = (uint32_t)data_.instances.size();
		propMeshes_[p.id] = index;
		propList_.push_back(index);
		data_.instances.push_back(b);
		setConstruction(index, p.progress);
		continue;
	}
		const auto cell = data_.cells.find(kworld::zoneIndexOf(kworld::zoneCellOf(p.pos[0], p.pos[2])));
		int placed = -1;
		if (cell != data_.cells.end() && !p.item)
		{
			for (const uint32_t i : cell->second)
			{
				const BuildingInstance& b = data_.instances[i];
				if ((!b.feature || p.placedOnly) && b.parent < 0 && std::hypot(b.x - p.pos[0], b.z - p.pos[2]) < 1.0) { placed = (int)i; break; }
			}
		}
		if (placed < 0 && !p.item && !p.placedOnly)	// a container the layout already draws (the Hub bar's counter, a cabinet): the sim sends it with yaw 0, a second copy at 90 degrees to the layout piece (kenshi-8vb.1)
		{
			for (int dx = -1; dx <= 1 && placed < 0; dx++)
			{
				for (int dz = -1; dz <= 1 && placed < 0; dz++)
				{
					const auto g = furnGrid_.find(furnKey(p.pos[0] + 4 * dx, p.pos[2] + 4 * dz));
					if (g == furnGrid_.end()) { continue; }
					for (const uint32_t i : g->second)
					{
						const BuildingInstance& f = data_.instances[i];
						if (std::hypot(f.x - p.pos[0], f.z - p.pos[2]) < 1.0 && std::fabs(f.y - p.pos[1]) < 2.0) { placed = (int)i; break; }
					}
				}
			}
		}
		if (placed >= 0) { propMeshes_[p.id] = (uint32_t)placed; continue; }
		if (p.placedOnly) { continue; }
		const auto& set = p.item ? data_.items : data_.storage;
		const auto range = set.find(p.sid);
		if (range == set.end()) { continue; }
		BuildingInstance b;
		b.x = p.pos[0]; b.y = p.pos[1]; b.z = p.pos[2];
		b.ground = (float)p.pos[1];
		b.prop = true;
		b.propCut = p.cut;
		if (p.limb)	// a severed limb: the wire's tint, and its bone scale as the per-axis stretch (mesh.glsl inst_vs applies it to the mesh)
		{
			b.limb = true;
			std::copy(p.limbSkin, p.limbSkin + 3, b.limbSkin);
			std::copy(p.limbBlood, p.limbBlood + 3, b.limbBlood);
			for (int k = 0; k < 3; k++) { b.stretch[k] = p.limbScale[k]; }
		}
		b.qw = std::cos(p.yaw * 0.5f);
		b.qy = std::sin(p.yaw * 0.5f);
		b.firstPart = range->second.first;
		b.partCount = range->second.count;
		propMeshes_[p.id] = (uint32_t)data_.instances.size();
		propList_.push_back((uint32_t)data_.instances.size());
		data_.instances.push_back(b);
	}
	if (editorOn_ && data_.editorPlatformRef >= 0)
	{
		BuildingInstance b;
		b.x = editorPos_[0]; b.y = editorPos_[1]; b.z = editorPos_[2];
		b.ground = (float)editorPos_[1];
		b.firstPart = (uint32_t)data_.editorPlatformRef;
		b.partCount = 1;
		propList_.push_back((uint32_t)data_.instances.size());
		data_.instances.push_back(b);
	}
}

// ---- workers ----

void WorldBuildings::queue(bool isMesh, int id, const std::string& path)
{
	outstanding_++;
	{
		std::lock_guard<std::mutex> lk(mu_);
		jobs_.push_back({ isMesh, id, path });
	}
	jobCv_.notify_one();
}

// Per-vertex tangents from the UVs, for meshes without a tangent element: each triangle's u direction summed (area-weighted), made
// orthogonal to the normal; w = +1 when cross(tangent, normal) runs along the triangles' v direction, else -1.
static void uvTangents(const ogre::Geometry& g, const std::vector<uint32_t>& tris, const std::vector<float>& normals, std::vector<float>& out)
{
	std::vector<float> bi((size_t)g.count * 3, 0.0f);
	for (size_t i = 0; i + 2 < tris.size(); i += 3)
	{
		const uint32_t ia = tris[i], ib = tris[i + 1], ic = tris[i + 2];
		const float* a = &g.pos[ia * 3], *b = &g.pos[ib * 3], *c = &g.pos[ic * 3];
		const float* ta = &g.uv[ia * 2], *tb = &g.uv[ib * 2], *tc = &g.uv[ic * 2];
		const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		const float du1 = tb[0] - ta[0], dv1 = tb[1] - ta[1], du2 = tc[0] - ta[0], dv2 = tc[1] - ta[1];
		const float det = du1 * dv2 - du2 * dv1;
		if (std::fabs(det) < 1e-12f) { continue; }
		const float r = 1.0f / det;
		for (int k = 0; k < 3; k++)
		{
			const float t = (e1[k] * dv2 - e2[k] * dv1) * r, u = (e2[k] * du1 - e1[k] * du2) * r;
			for (const uint32_t vi : { ia, ib, ic }) { out[vi * 4 + k] += t; bi[vi * 3 + k] += u; }
		}
	}
	for (uint32_t i = 0; i < g.count; i++)
	{
		float* t = &out[(size_t)i * 4];
		const float* n = &normals[(size_t)i * 3];
		const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (nl < 1e-12f) { t[0] = t[1] = t[2] = 0.0f; continue; }
		const float nn[3] = { n[0] / nl, n[1] / nl, n[2] / nl };
		const float d = t[0] * nn[0] + t[1] * nn[1] + t[2] * nn[2];
		for (int k = 0; k < 3; k++) { t[k] -= nn[k] * d; }
		const float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
		if (tl < 1e-12f) { t[0] = t[1] = t[2] = 0.0f; continue; }	// a degenerate frame: the shader keeps the geometric normal
		for (int k = 0; k < 3; k++) { t[k] /= tl; }
		const float c[3] = { t[1] * nn[2] - t[2] * nn[1], t[2] * nn[0] - t[0] * nn[2], t[0] * nn[1] - t[1] * nn[0] };
		const float* b = &bi[(size_t)i * 3];
		t[3] = c[0] * b[0] + c[1] * b[1] + c[2] * b[2] < 0.0f ? -1.0f : 1.0f;
	}
}

bool WorldBuildings::decodeMesh(const std::string& path, MeshCpu& out)
{
	ogre::Mesh mesh;
	std::string err;
	if (!ogre::load(path, mesh, err) || !mesh.vertexBounds(out.bmin, out.bmax)) { return false; }
	for (const ogre::Submesh& sm : mesh.submeshes)
	{
		const ogre::Geometry& g = mesh.geometry(sm);
		const std::vector<uint32_t> tris = sm.triangleList();
		if (tris.empty() || g.count == 0) { continue; }
		std::vector<float> normals = g.normal;
		if (normals.empty())	// area-weighted vertex normals
		{
			normals.assign((size_t)g.count * 3, 0.0f);
			for (size_t i = 0; i + 2 < tris.size(); i += 3)
			{
				const float* a = &g.pos[tris[i] * 3], *b = &g.pos[tris[i + 1] * 3], *c = &g.pos[tris[i + 2] * 3];
				const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				const float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
				for (int k = 0; k < 3; k++) { for (int j = 0; j < 3; j++) { normals[tris[i + k] * 3 + j] += n[j]; } }
			}
		}
		// tangent xyz + handedness w (objects.hlsl:40 binormal = cross(tangent, normal) x w): the mesh's own, else from the UVs
		std::vector<float> tangents((size_t)g.count * 4, 0.0f);
		if (g.tangentComps >= 3)
		{
			for (uint32_t i = 0; i < g.count; i++)
			{
				memcpy(&tangents[(size_t)i * 4], &g.tangent[(size_t)i * g.tangentComps], 12);
				tangents[(size_t)i * 4 + 3] = g.tangentComps == 4 ? g.tangent[(size_t)i * 4 + 3] : 1.0f;
			}
		}
		else if (!g.uv.empty()) { uvTangents(g, tris, normals, tangents); }
		Sub s;
		s.firstVertex = (int)(out.verts.size() / kVertexFloats);
		s.firstIndex = (int)out.indices.size();
		s.indexCount = (int)tris.size();
		s.material = sm.material;
		out.verts.resize(out.verts.size() + (size_t)g.count * kVertexFloats, 0.0f);
		for (uint32_t i = 0; i < g.count; i++)
		{
			float* v = &out.verts[((size_t)s.firstVertex + i) * kVertexFloats];
			memcpy(v, &g.pos[(size_t)i * 3], 12);
			memcpy(v + 3, &normals[(size_t)i * 3], 12);
			if (!g.uv.empty()) { memcpy(v + 6, &g.uv[(size_t)i * 2], 8); }
			const uint8_t white[4] = { 255, 255, 255, 255 };
			memcpy(v + 8, g.colour.empty() ? white : &g.colour[(size_t)i * 4], 4);
			memcpy(v + 9, &tangents[(size_t)i * 4], 16);
		}
		out.hasColour = out.hasColour || !g.colour.empty();
		out.indices.insert(out.indices.end(), tris.begin(), tris.end());
		// render.open_windows: the submesh's small detached islands (a window barrier is a 2-triangle quad welded to nothing else)
		if (openwindows::isPanePath(path)) { s.islands.push_back(openwindows::whole(out.verts, kVertexFloats, out.indices, (uint32_t)s.firstIndex, (uint32_t)s.indexCount)); }
		else { s.islands = openwindows::find(out.verts, kVertexFloats, out.indices, (uint32_t)s.firstIndex, (uint32_t)s.indexCount); }
		out.subs.push_back(s);
	}
	return !out.subs.empty();
}

void WorldBuildings::worker()
{
	for (;;)
	{
		Job job;
		{
			std::unique_lock<std::mutex> lk(mu_);
			jobCv_.wait(lk, [this] { return quit_ || !jobs_.empty(); });
			if (quit_) { return; }
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}
		Result r;
		r.isMesh = job.isMesh;
		r.id = job.id;
		std::string err;
		r.ok = job.isMesh ? decodeMesh(job.path, r.mesh) : dds_load(job.path, r.tex, err);
		{
			std::lock_guard<std::mutex> lk(mu_);
			results_.push_back(std::move(r));
		}
		resCv_.notify_all();
	}
}

// The logic half of a decoded mesh: the CPU fields (subs, bounds, the hull) here, the buffers in a render queue job that publishes them into m.gpu
// (RENDER_THREAD.md §7.2, kenshi-k16.2.205.12.35). Streaming: a stream-lane job, and the mesh is Uploading until promote() sees its Handle. Blocking:
// a frame-lane job, which runs before the packet being filled is drawn, so the mesh is Ready at once and this frame's pack may name it.
void WorldBuildings::uploadMesh(MeshEntry& m, MeshCpu& cpu, bool blocking)
{
	struct Data { std::vector<float> verts; std::vector<uint32_t> indices; };
	auto data = std::make_shared<Data>(Data{ std::move(cpu.verts), std::move(cpu.indices) });
	const size_t bytes = data->verts.size() * sizeof(float) + data->indices.size() * sizeof(uint32_t);
	m.gpu = rq::Handle<MeshGpu>();
	m.lane = blocking ? rq::Lane::kFrame : rq::Lane::kStream;
	rq::push("buildings.mesh", [data, h = m.gpu] {
		MeshGpu g;
		sg_buffer_desc bd = {};
		bd.data = { data->verts.data(), data->verts.size() * sizeof(float) };
		bd.label = "building-verts";
		g.vbuf = sg_make_buffer(&bd);
		bd = {};
		bd.usage.vertex_buffer = false;
		bd.usage.index_buffer = true;
		bd.data = { data->indices.data(), data->indices.size() * sizeof(uint32_t) };
		bd.label = "building-indices";
		g.ibuf = sg_make_buffer(&bd);
		h.publish(g);
	}, m.lane, bytes);
	const std::vector<float>& verts = data->verts;
	const std::vector<uint32_t>& indices = data->indices;
	if (m.keepHull)
	{
		const size_t n = verts.size() / kVertexFloats;
		m.hullPos.resize(n * 3);
		for (size_t i = 0; i < n; i++) { for (int k = 0; k < 3; k++) { m.hullPos[i * 3 + k] = verts[i * kVertexFloats + k]; } }
		m.hullIdx.clear();
		for (const Sub& sb : cpu.subs) { for (int i = 0; i < sb.indexCount; i++) { m.hullIdx.push_back((uint32_t)(sb.firstVertex + indices[sb.firstIndex + i])); } }
	}
	m.subs = std::move(cpu.subs);
	m.indices = indices;
	m.hasColour = cpu.hasColour;
	for (int k = 0; k < 3; k++)
	{
		m.bmin[k] = cpu.bmin[k];
		m.bmax[k] = cpu.bmax[k];
		m.center[k] = (cpu.bmin[k] + cpu.bmax[k]) / 2;
	}
	m.radius = std::sqrt((m.bmax[0] - m.bmin[0]) * (m.bmax[0] - m.bmin[0]) + (m.bmax[1] - m.bmin[1]) * (m.bmax[1] - m.bmin[1])
		+ (m.bmax[2] - m.bmin[2]) * (m.bmax[2] - m.bmin[2])) / 2;
	m.state = blocking ? Ready : Uploading;
	holdGen_++;	// the boxes and, blocking, Ready (bedRay's memo)
}

// A decoded texture: the image and its view in a job, as uploadMesh. A blocking create is Ready at once; if sokol refuses the image the draw binds
// the fallback (texView) and promote() marks it Failed on the next drain.
void WorldBuildings::uploadTexture(TexEntry& t, DdsImage&& tex, bool blocking)
{
	// render.open_windows: a luma thumbnail (<= 128 px) for the barrier quads' dark-patch test; a small mip, cheap to decode here
	{
		int level = 0;
		while (level + 1 < tex.mips && (std::max(tex.width, tex.height) >> level) > 128) { level++; }
		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		t.thumb.clear();
		t.tw = t.th = 0;
		if (dds_decode(tex, level, rgba, w, h) && w > 0 && h > 0 && rgba.size() >= (size_t)w * h * 4)
		{
			t.thumb.resize((size_t)w * h);
			for (size_t i = 0; i < t.thumb.size(); i++) { const uint8_t* p = &rgba[i * 4]; t.thumb[i] = (uint8_t)(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]); }
			t.tw = w;
			t.th = h;
		}
	}
	auto data = std::make_shared<DdsImage>(std::move(tex));
	const size_t bytes = data->data.size();
	t.gpu = rq::Handle<TexGpu>();
	t.lane = blocking ? rq::Lane::kFrame : rq::Lane::kStream;
	auto finish = [h = t.gpu](sg_image img) {
		TexGpu g;
		if (sg_query_image_state(img) == SG_RESOURCESTATE_VALID)
		{
			sg_view_desc vd = {};
			vd.texture.image = img;
			g.img = img;
			g.view = sg_make_view(&vd);
		}
		else if (img.id != SG_INVALID_ID) { sg_destroy_image(img); }
		h.publish(g);
	};
	if (blocking)	// the frame lane runs every job before this packet's draw: one create (a split would buy nothing)
	{
		rq::push("buildings.texture", [data, finish] { finish(dds_make_image(*data, "building-texture")); }, t.lane, bytes);
	} else {	// streamed: a large texture in slices over several drains (dds_queue_image, kenshi-k16.2.205.12.74); Ready once the last one ran
		dds_queue_image(data, "building-texture", dds_slice(), [](size_t b, std::function<void()> job) { rq::push("buildings.texture", std::move(job), rq::Lane::kStream, b); }, finish);
	}
	t.state = blocking ? Ready : Uploading;
}

// The creates that have run: an Uploading entry whose Handle is ready becomes Ready (a texture sokol refused: Failed); a frame-lane texture is
// checked for the same refusal. Logic side, at drain's start.
void WorldBuildings::promote()
{
	size_t keep = 0;
	for (size_t i = 0; i < uploading_.size(); i++)
	{
		const auto [isMesh, id] = uploading_[i];
		if (isMesh)
		{
			MeshEntry& m = meshes_[id];
			if (!m.gpu.ready()) { uploading_[keep++] = uploading_[i]; continue; }
			if (m.state == Uploading) { m.state = Ready; stateGen_++; holdGen_++; }
		} else {
			TexEntry& t = textures_[id];
			if (!t.gpu.ready()) { uploading_[keep++] = uploading_[i]; continue; }
			const bool ok = t.gpu.get().view.id != SG_INVALID_ID;
			if (t.state == Uploading || !ok) { t.state = ok ? Ready : Failed; stateGen_++; }
			if (ok && t.lane == rq::Lane::kStream) { stats_.uploadedTextures++; }
		}
	}
	uploading_.resize(keep);
}

sg_view WorldBuildings::texView(int t, sg_view fallback) const
{
	if (t < 0 || textures_[t].state != Ready || !textures_[t].gpu.ready()) { return fallback; }
	const sg_view v = textures_[t].gpu.get().view;
	return v.id != SG_INVALID_ID ? v : fallback;
}

// Uploads finished decodes. wait: block until at least one is available (or nothing is outstanding).
void WorldBuildings::drain(bool wait, int maxItems)
{
	promote();
	for (int n = 0; maxItems < 0 || n < maxItems;)
	{
		Result r;
		{
			std::unique_lock<std::mutex> lk(mu_);
			if (wait && results_.empty()) { resCv_.wait(lk, [this] { return !results_.empty() || outstanding_ == 0; }); }
			if (results_.empty()) { return; }
			r = std::move(results_.front());
			results_.pop_front();
		}
		outstanding_--;
		n++;
		stateGen_++;	// a load state moved: the pack's what-loads list changed (kenshi-k16.2.205.11)
		if (r.isMesh)
		{
			MeshEntry& m = meshes_[r.id];
			if (r.ok) { uploadMesh(m, r.mesh, wait); stats_.uploadedMeshes++; if (!wait) { uploading_.push_back({ true, r.id }); } }
			else { m.state = Failed; holdGen_++; }
		} else {
			TexEntry& t = textures_[r.id];
			if (r.ok)
			{
				uploadTexture(t, std::move(r.tex), wait);
				uploading_.push_back({ false, r.id });
				if (wait) { stats_.uploadedTextures++; }	// streamed: counted once sokol took it (promote)
			}
			else { t.state = Failed; }
		}
	}
}

// ---- caches ----

int WorldBuildings::meshFor(const std::string& recordPath)
{
	const std::string file = res_->locate(recordPath);
	if (file.empty()) { return -1; }
	auto [it, fresh] = meshIds_.emplace(file, (int)meshes_.size());
	if (fresh)
	{
		meshes_.emplace_back();
		meshes_.back().path = file;
	}
	return it->second;
}

int WorldBuildings::textureFor(const std::string& file)
{
	auto [it, fresh] = texIds_.emplace(file, (int)textures_.size());
	if (fresh)
	{
		textures_.emplace_back();
		textures_.back().file = file;
	}
	return it->second;
}

// The texture rule (unconfirmed, see docs/reference/BUILDING_MATERIALS.md): a submesh whose Ogre material has a real script texture
// keeps it; everything else (StaticObject and friends) gets the part's MATERIAL_SPEC `texture map`; else grey (empty string).
std::string WorldBuildings::textureFile(const BuildingPartDef& def, const std::string& submeshMaterial, bool* scriptTexture, bool missingAsBlack) const
{
	if (scriptTexture) { *scriptTexture = false; }
	// A feature record names its texture itself; a missing one keeps the no-albedo answer (rwh.160 measured the part-material path only, and
	// 92 placed feature instances of 8 FOLIAGE_MESH / MAP_FEATURES records would turn black on that guess; see recordTexture).
	if (def.featureMode >= 0 && !def.texture.empty()) { return res_->locate(def.texture); }
	if (lower(submeshMaterial) != "staticobject")
	{
		const std::string script = mats_->diffuse(submeshMaterial);
		if (!MaterialLib::placeholder(script)) { if (const std::string f = res_->find(script); !f.empty()) { if (scriptTexture) { *scriptTexture = true; } return f; } }
	}
	return recordTexture(def.texture, missingAsBlack);
}

// A BUILDING_PART's MATERIAL_SPEC `texture map` -> the file to bind. A name the install lacks binds the engine's default
// `black.dds` (16x16 RGBA 0), which is what the original does when a material names a texture the install does not ship (kenshi-rwh.160: the
// Black Desert City dome door's `Door_RESKIN_Texture.dds` is absent and the original's leaf binds black.dds - a dark plate, HDR luma 0.052, not
// the near-white plate a white/grey substitute draws). No name at all keeps "" (the material rule's grey): BUILDING_MATERIALS.md's 138
// MATERIAL_SPECs all set `texture map`, so that case is a part with no material record, which the original would fill from the BUILDING's or
// the town's material - no binding is recorded there, and rwh.160 measured none.
std::string WorldBuildings::recordTexture(const std::string& recordPath, bool missingAsBlack) const
{
	if (recordPath.empty()) { return ""; }
	const std::string f = res_->locate(recordPath);
	if (!f.empty() || !missingAsBlack) { return f; }
	return res_->locate("data/materials/black.dds");
}

void WorldBuildings::ensurePart(uint32_t pid)
{
	PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	if (rt.mesh == -2)
	{
		rt.mesh = def.marker && hideMarkers ? -1 : meshFor(def.mesh);
		holdGen_++;	// the part has its mesh (instanceHolds reads it: bedRay's memo)
	}
	if (rt.mesh < 0) { return; }
	MeshEntry& m = meshes_[rt.mesh];
	m.lastUsed = frame_;
	if (def.kind == 2 || def.stairs) { m.keepHull = true; }	// the mask hull (insideInterior) and a staircase (the floor hotspot reads its treads)
	if (m.state == Unloaded)
	{
		m.state = Loading;
		holdGen_++;	// every mesh state move bumps it (bedRay's memo, kenshi-k16.2.205.12.111)
		queue(true, rt.mesh, m.path);
	}
	if (m.state == Failed) { rt.openDone = true; return; }	// nothing to classify (and the bake's render.open_windows gate must not wait for it)
	if (m.state != Ready) { return; }
	if (def.kind == 2) { rt.resolved = true; resolveOpenWindows(pid); return; }	// interior mask: bounds only
	if (!rt.resolved)
	{
		rt.subTex.assign(m.subs.size(), -1);
		rt.subFlat.assign(m.subs.size(), 0);
		const std::string own = def.texture.empty() ? "" : res_->locate(def.texture);
		for (size_t i = 0; i < m.subs.size(); i++)
		{
			bool script = false;
			const std::string f = textureFile(def, m.subs[i].material, &script);
			if (!f.empty()) { rt.subTex[i] = textureFor(f); }
			rt.subFlat[i] = script;	// a script material's own texture: the part's normal map is not its, and no per-mesh script names a normal unit (k16.2.23)
		}
		auto extra = [&](const std::string& rec) { const std::string f = rec.empty() ? "" : res_->locate(rec); return f.empty() ? -1 : textureFor(f); };
		rt.normalTex = extra(def.normal);
		rt.tex2Tex = m.hasColour ? extra(def.texture2) : -1;	// DUAL needs vertex alpha
		rt.nrm2Tex = m.hasColour ? extra(def.normal2) : -1;
		rt.maskTex = def.limb ? extra(def.colorMap) : -1;	// a severed limb's body mask (skin tone mask), otherwise none
		rt.ownTex = def.limb && !own.empty() ? textureFor(own) : -1;	// the limb's own `texture map`; SeveredLimbItem ignores the submesh's script material
		rt.resolved = true;
	}
	std::vector<int> wanted = rt.subTex;
	wanted.push_back(rt.normalTex);
	wanted.push_back(rt.tex2Tex);
	wanted.push_back(rt.nrm2Tex);
	wanted.push_back(rt.maskTex);
	wanted.push_back(rt.ownTex);
	for (const int t : wanted)
	{
		if (t < 0) { continue; }
		TexEntry& te = textures_[t];
		te.lastUsed = frame_;
		if (te.state == Unloaded)
		{
			te.state = Loading;
			queue(false, t, te.file);
		}
	}
	resolveOpenWindows(pid);
}

// render.open_windows: classify the part's small detached islands (one finding per candidate: the islands are geometric, the verdict texture) and
// build its patched index buffer. A barrier island samples a dark patch of the part's own atlas (openwindows::dark), so it needs the albedo's luma
// thumbnail; until every submesh texture is ready (or failed) this waits for the next frame. The sub ranges are unchanged (barrier triangles become
// degenerate), so every draw site keeps using them; the extra buffer is only made for a part that has a barrier.
void WorldBuildings::resolveOpenWindows(uint32_t pid)
{
	PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	if (rt.openDone) { return; }
	// The cavity lining behind the windows (a black wall: a `black`-named shell part, kind 0, or a black interior part, kind 1) would shut the light out
	// again once the barrier quads are gone, so the draw and the shadow pass leave it out of a building whose windows this opens (the bake already
	// skips every blackCap part). It is only ever hidden together with an opened barrier of its own building (the visibility loop's anyOpen).
	{
		std::string lowered = def.mesh;
		for (char& c : lowered) { c = (char)tolower((unsigned char)c); }
		rt.openHide = (def.kind == 0 || def.kind == 1) && lowered.find("black") != std::string::npos;
	}
	if (rt.mesh < 0) { rt.openDone = true; return; }	// no mesh (a hidden position marker, a missing file): nothing to classify
	if (!rt.resolved) { return; }
	if (def.kind != 0 && def.kind != 1) { rt.openDone = true; return; }	// shell and interior parts; not furniture, features, props, masks
	if (def.blackCap || def.marker || def.materialType != 0) { rt.openDone = true; return; }	// the cut caps, position markers, glass / foliage
	const MeshEntry& m = meshes_[rt.mesh];
	if (m.state != Ready || m.indices.empty()) { return; }
	std::vector<uint32_t> drop;
	for (size_t si = 0; si < m.subs.size() && si < rt.subTex.size(); si++)
	{
		const std::vector<openwindows::Island>& islands = m.subs[si].islands;
		if (islands.empty()) { continue; }
		const int t = rt.subTex[si];
		if (t < 0) { continue; }
		const TexEntry& te = textures_[t];
		if (te.state == Unloaded || te.state == Loading) { return; }	// wait for the atlas; a failed one simply has no barriers
		if (te.state != Ready || te.thumb.empty()) { continue; }
		const openwindows::Texel tex = { te.thumb.data(), nullptr, te.tw, te.th };
		// the islands' triangle numbers are submesh-local; the patch below writes the mesh's whole index list
		const uint32_t base = (uint32_t)(m.subs[si].firstIndex / 3);
		for (const uint32_t tri : openwindows::triangles(islands, def.tileX, def.tileY, tex)) { drop.push_back(base + tri); }
	}
	if (!drop.empty())
	{
		std::vector<uint32_t> patched = m.indices;
		for (const uint32_t tri : drop)
		{
			const size_t at = (size_t)tri * 3;
			if (at + 2 >= patched.size()) { continue; }
			const uint32_t v = patched[at];
			patched[at] = patched[at + 1] = patched[at + 2] = v;
		}
		auto list = std::make_shared<std::vector<uint32_t>>(std::move(patched));
		const size_t bytes = list->size() * sizeof(uint32_t);
		rt.openIbuf = rq::Handle<sg_buffer>();
		rq::push("buildings.open-indices", [list, h = rt.openIbuf] {
			sg_buffer_desc bd = {};
			bd.usage.index_buffer = true;
			bd.data = { list->data(), list->size() * sizeof(uint32_t) };
			bd.label = "building-indices-open";
			h.publish(sg_make_buffer(&bd));
		}, rq::Lane::kFrame, bytes);
		rt.openHas = true;
	}
	rt.openDone = true;
}

void WorldBuildings::evict()
{
	size_t ready = 0;
	for (const MeshEntry& m : meshes_) { ready += m.state == Ready; }
	if (ready > kMaxMeshes)
	{
		for (size_t i = 0; i < meshes_.size(); i++)
		{
			MeshEntry& m = meshes_[i];
			if (m.state != Ready || m.lastUsed + 120 > frame_) { continue; }
			stateGen_++;
			// in its create's lane, behind the create; the lanes run a job only on the draw of the packet it was queued for or a later one, so a
			// packet that still names the mesh (drawn again: §7.1 rule 4) is never drawn after the destroy (renderqueue.hpp)
			rq::push("buildings.mesh-free", [h = m.gpu] { sg_destroy_buffer(h.get().vbuf); sg_destroy_buffer(h.get().ibuf); }, m.lane);
			m.gpu = rq::Handle<MeshGpu>();
			m.state = Unloaded;
			holdGen_++;
			m.hullPos.clear();
			m.hullIdx.clear();
			for (PartRt& rt : partRt_) { if (rt.mesh == (int)i) { rt.resolved = false; } }
		}
	}
	ready = 0;
	for (const TexEntry& t : textures_) { ready += t.state == Ready; }
	if (ready > kMaxTextures)
	{
		for (size_t i = 0; i < textures_.size(); i++)
		{
			TexEntry& t = textures_[i];
			if (t.state != Ready || t.lastUsed + 120 > frame_) { continue; }
			stateGen_++;
			rq::push("buildings.texture-free", [h = t.gpu] { sg_destroy_view(h.get().view); sg_destroy_image(h.get().img); }, t.lane);
			t.gpu = rq::Handle<TexGpu>();
			t.state = Unloaded;
		}
	}
}
