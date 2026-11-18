#include "world/worldbuildings.hpp"
#include "render/drawstats.hpp"
#include "dev/lightterms.hpp"
#include "render/ao.hpp"
#include "render/speccube.hpp"
#include "world/weathermap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "app/config.hpp"
#include "interiormask.glsl.h"
#include "kworld/coords.hpp"
#include "mesh.glsl.h"
#include "stb_image.h"

namespace
{

constexpr size_t kInstanceFloats = 12;	// position + scale, rotation quaternion, per-axis stretch
constexpr float kGlassBuild = 200000.0f;	// a peeked part: stretch w = -(kGlassBuild + opacity x 1000) (mesh.glsl inst_vs)
constexpr float kFlipFade = 0.25f;	// MINE (user 2026-11-18): seconds a wall that turned away from the cut-away camera takes to fade out
constexpr float kGlassAlpha = 0.25f;	// MINE: a fully peeked or ghosted part's opacity
constexpr double kEffectMargin = 1500.0;	// units of view margin for effect collection: a just-off-screen fire stays alive (ParticleWorld)
constexpr size_t kInstanceBytes = kInstanceFloats * 4;
constexpr int kVertexFloats = 13;	// MeshCpu::verts
constexpr int kVertexBytes = kVertexFloats * 4;
constexpr size_t kMaxInstances = 1 << 19;	// 16 MiB transient buffer
constexpr size_t kMaxMeshes = 2500, kMaxTextures = 1200;
constexpr int kUploadsPerFrame = 8;
const cfg::Bool kNormalMaps{ cfg::Group{ "Detail" }, "render.normal_maps", true, "Normal maps on buildings, props and foliage (off: their geometric normal; for comparisons)" };
// The far ring (kenshi-k16.2.65; ours, user 2026-11-09: the original's generic far-town stand-in is "WILDLY inaccurate"): past the load radius a
// building's own exterior parts stay drawn out to this distance (capped by the fog), dithered out over its last kFarFade; no interiors, lights or effects.
const cfg::Float kBuildingFar{ cfg::Group{ "Draw distance" }, "render.building_far", 60000, 0, 100000, "Towns and rocks: how far their exteriors are drawn (world units, 10 = 1 m; capped by the fog; 0 = only the load radius)" };
constexpr float kFarFade = 0.15f;	// MINE: the share of the far distance the exteriors dither out over
const cfg::Bool kSignsIndoors{ cfg::Group{ "Interiors" }, "render.signs_indoors", true, "Display building signs while indoors: on by default (the original): a building's outside signs (shop and bar signs) keep drawing while it is cut to a floor; off hides them until the roof view" };
[[maybe_unused]] const bool kRetiredCutShadows = (cfg::retire("render.cut_shadows", "render.cut_shadows is gone (kenshi-jj3.30): the cut-away floors and roof always cast their sun shadows"), true);
const cfg::Bool kOpenWindows{ cfg::Group{ "Indoor lighting (experimental)" }, "render.open_windows", false,
	"Experimental: leave out the small detached quads a building mesh closes its windows and openings with (textured from a dark patch of the atlas: the Storm House's four window barriers, the Watchtower's and Shack's alike), so the slits are see-through and the light bake sees the sky through them; default off" };
const cfg::Bool kLeafSway{ cfg::Group{ "Detail" }, "render.leaf_sway", false, "A tree's leaves sway in the wind (off: the original, whose leaves do not move; edits are picked up while the client runs)" };
constexpr int kWorkers = 4;
constexpr float kInteriorFade = 0.8f;	// the dithered fade of interior parts and furniture starts at this fraction of the interior radius
constexpr double kInteriorSlack = 600.0;	// world units added to the interior radius when testing a building's origin: its parts reach out this far
constexpr double kMinCasterRadius = 1.0;	// instances out of view cast shadows only if their bounding radius is at least this
constexpr float kTriplanarUnits = 5000.0f;	// world units per texture repeat at tile 1 for the triplanar modes (triplanar.hlsl triplanar_vs: / 5000.0)

// Authoring-frame vector -> mesh (Ogre, y-up) frame. Frame 1 is Z-up: (x, y, z) -> (x, z, -y).
void toMesh(int frame, const float v[3], double out[3])
{
	if (frame == 0) { out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; }
	else { out[0] = v[0]; out[1] = v[2]; out[2] = -v[1]; }
}

// Spinner parts (rotation function 1..3: fans, spinners, rotors) turn continuously; 4 aims at the wind with its nested parts
// (BuildingPartDef::aimed; mesh.glsl inst_vs, per instance), 5 (turrets) stays still.
bool spinning(const BuildingPartDef& d) { return d.rotFunction >= 1 && d.rotFunction <= 3 && d.rotSpeed != 0; }

// Vertex-shader uniforms of an instanced part (draw() and drawShadow()); mvp is the camera or the cascade matrix. swayTop: the part mesh's
// top y (mesh units) for a swaying leaves part, 0 for every other part (mesh.glsl inst_vs). spin: a spinner's angle (rad); turn: a wind-aimed part's
// turn (the wind it turns from x, z and to x, z, the seconds since it began, the rate in rad/s), null when it has no wind to face.
inst_vs_params_t instVsParams(const BuildingPartDef& def, int offsetFrame, const double cam[3], double time, const float base[3], const float mvp[16], float swayTop,
	double spin, const float* turn)
{
	inst_vs_params_t vs = {};
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	vs.tile[0] = def.tileX;
	vs.tile[1] = def.tileY;
	vs.tile[2] = swayTop;
	vs.camw[3] = (float)time;	// the wind clock of a swaying leaves part (grass.glsl has its own, the same animTime)
	double off[3], axis[3];
	toMesh(offsetFrame, def.offset, off);
	const float unit[3] = { def.rotAxis == 0 ? 1.0f : 0.0f, def.rotAxis == 1 ? 1.0f : 0.0f, def.rotAxis == 2 ? 1.0f : 0.0f };
	toMesh(offsetFrame, unit, axis);
	for (int k = 0; k < 3; k++)
	{
		vs.offset[k] = (float)off[k];
		vs.spin[k] = (float)axis[k];
		vs.base[k] = base[k];
		vs.camw[k] = (float)cam[k];
	}
	if (spinning(def)) { vs.spin[3] = (float)std::fmod(spin, 6.283185307179586); }
	vs.limb[0] = def.limb ? 1.0f : 0.0f;	// a severed limb: the vertex shader builds its cylindrical blood UVs (severed_limb_vs)
	if (def.aimed && turn)
	{
		double pivot[3];
		toMesh(offsetFrame, def.aimPivot, pivot);
		for (int k = 0; k < 3; k++) { vs.aim[k] = (float)pivot[k]; }
		vs.aim[3] = 1.0f;
		for (int k = 0; k < 4; k++) { vs.wind[k] = turn[k]; }
		vs.turn[0] = turn[4];
		vs.turn[1] = turn[5];
	}
	return vs;
}

void rotate(const float q[4], const double v[3], double out[3])	// q = w x y z; v' = v + 2 u x (u x v + w v)
{
	const double ux = q[1], uy = q[2], uz = q[3], w = q[0];
	const double cx = uy * v[2] - uz * v[1] + w * v[0], cy = uz * v[0] - ux * v[2] + w * v[1], cz = ux * v[1] - uy * v[0] + w * v[2];
	out[0] = v[0] + 2 * (uy * cz - uz * cy);
	out[1] = v[1] + 2 * (uz * cx - ux * cz);
	out[2] = v[2] + 2 * (ux * cy - uy * cx);
}

}

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

void WorldBuildings::setEditor(bool on, const double pos[3])
{
	editorOn_ = on;
	if (on) { for (int k = 0; k < 3; k++) { editorPos_[k] = pos[k]; } }
}

bool WorldBuildings::propRay(uint64_t id, const double origin[3], const double dir[3], double& t, bool drawn) const
{
	const auto it = propMeshes_.find(id);
	return it != propMeshes_.end() && instanceRay(it->second, origin, dir, t, drawn);
}

// The nearest drawn surface the ray meets within t in [tMin, tMax] on a building shell or a layout piece standing in the world box lo..hi (a collision solid's):
// a solid is backed by what is drawn there, or it is an undrawn proxy volume (the staircase body of Storm House reaches 10 u above its treads, k16.2.239.4.1).
bool WorldBuildings::drawnHitIn(const double origin[3], const double dir[3], const double lo[3], const double hi[3], double tMin, double tMax, double& t) const
{
	bool hit = false;
	const auto test = [&](uint32_t i)
	{
		double tt;
		if (instanceRay(i, origin, dir, tt, true, tMin, tMax) && (!hit || tt < t)) { t = tt; hit = true; }
	};
	for (const uint32_t i : topLevel_) { test(i); }
	for (double x = std::floor(lo[0] / 4) * 4 - 4; x <= hi[0] + 4; x += 4)
	{
		for (double z = std::floor(lo[2] / 4) * 4 - 4; z <= hi[2] + 4; z += 4)
		{
			const auto g = furnGrid_.find(furnKey(x, z));
			if (g != furnGrid_.end()) { for (const uint32_t i : g->second) { test(i); } }
		}
	}
	return hit;
}

bool WorldBuildings::groundDrawn(double x, double z, double wireY, double up, double down, double& y) const
{
	const double o[3] = { x, wireY, z }, dn[3] = { 0, -1, 0 }, upd[3] = { 0, 1, 0 };
	const double lo[3] = { x - 1, wireY - down, z - 1 }, hi[3] = { x + 1, wireY + up, z + 1 };
	double tDown = 0, tUp = 0;
	const bool d = drawnHitIn(o, dn, lo, hi, 0, down, tDown), u = drawnHitIn(o, upd, lo, hi, 0, up, tUp);
	if (!d && !u) { return false; }
	y = d && (!u || tDown <= tUp) ? wireY - tDown : wireY + tUp;
	return true;
}

int WorldBuildings::layoutPieceInstance(uint64_t building, uint32_t piece) const
{
	const auto it = layoutPieces_.find(std::make_pair(building, piece));
	return it == layoutPieces_.end() ? -1 : (int)it->second;
}

// One part's mesh box (the box instanceRay tests, mesh bmin/bmax plus the part's offset) as a world AABB. False: an effect part, an unloaded mesh.
bool WorldBuildings::partWorldBox(const BuildingInstance& b, uint32_t k, double lo[3], double hi[3]) const
{
	const uint32_t pid = data_.partRefs[b.firstPart + k];
	const BuildingPartDef& def = data_.parts[pid];
	const int mesh = partRt_[pid].mesh;
	if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { return false; }
	const MeshEntry& m = meshes_[mesh];
	double off[3];
	toMesh(offsetFrame, def.offset, off);
	for (int a = 0; a < 3; a++) { lo[a] = 1e30; hi[a] = -1e30; }
	for (int c = 0; c < 8; c++)	// the box's corners in the mesh frame, turned by the instance and moved to its world position
	{
		const double p[3] = { (c & 1 ? m.bmax[0] : m.bmin[0]) + off[0], (c & 2 ? m.bmax[1] : m.bmin[1]) + off[1], (c & 4 ? m.bmax[2] : m.bmin[2]) + off[2] };
		const float q[4] = { b.qw, b.qx, b.qy, b.qz };
		double r[3];
		rotate(q, p, r);
		for (int a = 0; a < 3; a++)
		{
			const double v = r[a] * b.scale * b.stretch[a] + (a == 0 ? b.x : a == 1 ? b.y : b.z);
			lo[a] = std::min(lo[a], v);
			hi[a] = std::max(hi[a], v);
		}
	}
	return true;
}

// The world box of everything a prop draws (the union of its parts' boxes, this frame), grown by `grow`. False: no such prop, or no part of it draws.
bool WorldBuildings::propBox(uint64_t id, double lo[3], double hi[3], double grow) const
{
	const int index = propInstance(id);
	if (index < 0) { return false; }
	const BuildingInstance& b = data_.instances[(size_t)index];
	bool any = false;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		double plo[3], phi[3];
		if (!partWorldBox(b, k, plo, phi)) { continue; }
		for (int a = 0; a < 3; a++) { lo[a] = any ? std::min(lo[a], plo[a]) : plo[a]; hi[a] = any ? std::max(hi[a], phi[a]) : phi[a]; }
		any = true;
	}
	if (!any) { return false; }
	for (int a = 0; a < 3; a++) { lo[a] -= grow; hi[a] += grow; }
	return true;
}

// The instance a prop is drawn and picked by and, per part, the mesh box the ray tests (instanceRay) as a world AABB
std::string WorldBuildings::propReport(uint64_t id) const
{
	const int index = propInstance(id);
	if (index < 0) { return "prop " + std::to_string(id) + ": no instance\n"; }
	const BuildingInstance& b = data_.instances[(size_t)index];
	std::string out = "prop " + std::to_string(id) + ": instance " + std::to_string(index) + " at " + std::to_string(b.x) + " " + std::to_string(b.y) + " " + std::to_string(b.z) +
		" scale " + std::to_string(b.scale) + " parts " + std::to_string(b.partCount) + (propFromLayout(id) ? " from-layout" : "") + "\n";
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		double lo[3], hi[3];
		if (!partWorldBox(b, k, lo, hi)) { continue; }
		char line[256];
		snprintf(line, sizeof line, "  part %u box %.3f %.3f %.3f - %.3f %.3f %.3f\n", k, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
		out += line;
	}
	return out;
}

// A bed (SIMLINK INTERACTABLE kind 5) is a layout furniture piece, not a prop: the piece whose oriented mesh box (grown by kBedSlack) holds the sleeping
// spot `at`, the nearest to it when several do; the ray enters its box at t. -1: no piece holds the spot, 0: the ray misses it, 1: hit.
int WorldBuildings::bedRay(const double at[3], const double origin[3], const double dir[3], double& t) const
{
	// The piece depends on the spot only (not the ray): bedPiece once per spot and generation, the ray test per call (kenshi-k16.2.205.12.111)
	uint64_t key = 1469598103934665603ull;
	for (int a = 0; a < 3; a++) { uint64_t bits; std::memcpy(&bits, &at[a], sizeof bits); key = (key ^ bits) * 1099511628211ull; }
	auto it = bedMemo_.find(key);
	if (it == bedMemo_.end() || it->second.dataGen != dataGen_ || it->second.holdGen != holdGen_ || it->second.at[0] != at[0] || it->second.at[1] != at[1] || it->second.at[2] != at[2])
	{
		if (it == bedMemo_.end() && bedMemo_.size() >= 4096) { bedMemo_.clear(); }	// beds of zones left behind
		BedMemo m = { { at[0], at[1], at[2] }, dataGen_, holdGen_, bedPiece(at) };
		it = bedMemo_.insert_or_assign(key, m).first;
	}
	const int found = it->second.piece;
	return found < 0 ? -1 : instanceRay((uint32_t)found, origin, dir, t) ? 1 : 0;
}

// Whether point p lies in one of the instance's part boxes, each grown by slack (mesh units)
bool WorldBuildings::instanceHolds(uint32_t index, const double p[3], double slack) const
{
	const BuildingInstance& b = data_.instances[index];
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	const double rel[3] = { p[0] - b.x, p[1] - b.y, p[2] - b.z };
	double o[3];
	rotate(conj, rel, o);
	for (int a = 0; a < 3; a++) { o[a] /= b.scale * b.stretch[a]; }
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		const int mesh = partRt_[pid].mesh;
		if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { continue; }
		const MeshEntry& m = meshes_[mesh];
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		bool in = true;
		for (int a = 0; a < 3; a++) { in = in && o[a] >= m.bmin[a] + off[a] - slack && o[a] <= m.bmax[a] + off[a] + slack; }
		if (in) { return true; }
	}
	return false;
}

// The nearest hit of the ray (mesh frame, already offset-free: `off` is the part's offset) on the mesh's triangles; a mesh whose triangles cannot be
// read keeps its box (true, t unchanged).
bool WorldBuildings::triangleRay(int mesh, const double o[3], const double d[3], const double off[3], double& t, double tMin, double tMax) const
{
	const std::string& path = meshes_[mesh].path;
	auto it = pickTris_.find(path);
	if (it == pickTris_.end())
	{
		std::vector<float> tris;
		MeshCpu cpu;
		if (decodeMesh(path, cpu))
		{
			for (const Sub& s : cpu.subs)
			{
				for (int i = 0; i + 2 < s.indexCount; i += 3)
				{
					for (int c = 0; c < 3; c++)
					{
						const float* v = &cpu.verts[((size_t)s.firstVertex + cpu.indices[(size_t)s.firstIndex + i + c]) * kVertexFloats];
						tris.insert(tris.end(), v, v + 3);
					}
				}
			}
		}
		it = pickTris_.emplace(path, std::move(tris)).first;
	}
	const std::vector<float>& tr = it->second;
	if (tr.empty()) { return true; }
	double best = 1e30;
	const double ox = o[0] - off[0], oy = o[1] - off[1], oz = o[2] - off[2];
	for (size_t i = 0; i + 8 < tr.size(); i += 9)	// Moeller-Trumbore, both faces (a prop's back faces are drawn or hidden by the box anyway)
	{
		const float* a = &tr[i];
		const double e1[3] = { tr[i + 3] - (double)a[0], tr[i + 4] - (double)a[1], tr[i + 5] - (double)a[2] };
		const double e2[3] = { tr[i + 6] - (double)a[0], tr[i + 7] - (double)a[1], tr[i + 8] - (double)a[2] };
		const double p[3] = { d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0] };
		const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
		if (std::abs(det) < 1e-14) { continue; }
		const double s[3] = { ox - a[0], oy - a[1], oz - a[2] };
		const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
		if (u < 0 || u > 1) { continue; }
		const double q[3] = { s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0] };
		const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
		if (v < 0 || u + v > 1) { continue; }
		const double tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
		if (tt >= tMin && tt <= tMax && tt < best) { best = tt; }
	}
	if (best >= 1e30) { return false; }
	t = best;
	return true;
}

bool WorldBuildings::instanceRay(uint32_t index, const double origin[3], const double dir[3], double& t, bool drawn, double tMin, double tMax) const
{
	const BuildingInstance& b = data_.instances[index];
	// the ray in the instance's mesh frame; a linear map, so t is the same along both
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	const double rel[3] = { origin[0] - b.x, origin[1] - b.y, origin[2] - b.z };
	double o[3], d[3];
	rotate(conj, rel, o);
	rotate(conj, dir, d);
	for (int a = 0; a < 3; a++) { o[a] /= b.scale * b.stretch[a]; d[a] /= b.scale * b.stretch[a]; }
	bool hit = false;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		const int mesh = partRt_[pid].mesh;
		if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { continue; }
		const MeshEntry& m = meshes_[mesh];
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		double t0 = 0, t1 = 1e30;
		for (int a = 0; a < 3 && t0 <= t1; a++)
		{
			const double lo = m.bmin[a] + off[a] - o[a], hi = m.bmax[a] + off[a] - o[a];
			if (std::abs(d[a]) < 1e-12) { if (lo > 0 || hi < 0) { t1 = -1; } continue; }
			const double ta = lo / d[a], tb = hi / d[a];
			t0 = std::max(t0, std::min(ta, tb));
			t1 = std::min(t1, std::max(ta, tb));
		}
		if (t0 > t1 || t1 < tMin || t0 > tMax) { continue; }	// the box's span along the ray misses the window
		if (drawn && !triangleRay(mesh, o, d, off, t0, tMin, tMax)) { continue; }	// the box is only the cheap test; the drawn triangles decide
		if (!hit || t0 < t) { t = t0; hit = true; }
	}
	return hit;
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

// ---- interiors: floors and the inside test ----

const WorldBuildings::BInfo* WorldBuildings::infoFor(uint32_t bi)
{
	if (const auto it = binfo_.find(bi); it != binfo_.end()) { return &it->second; }
	const BuildingInstance& b = data_.instances[bi];
	float maskMn[3] = { 1e30f, 1e30f, 1e30f }, maskMx[3] = { -1e30f, -1e30f, -1e30f }, groundMn[3] = { 1e30f, 1e30f, 1e30f }, groundMx[3] = { -1e30f, -1e30f, -1e30f };
	float interiorLow[4] = { 1e30f, 1e30f, 1e30f, 1e30f };	// lowest point of the interior parts of each floor
	bool mask = false, ground = false;
	BInfo info;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		const int mesh = partRt_[pid].mesh;
		if (mesh == -2) { return nullptr; }
		if (mesh < 0 || meshes_[mesh].state == Failed) { continue; }
		const MeshEntry& m = meshes_[mesh];
		if (m.state != Ready) { return nullptr; }
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		float lo[3], hi[3];
		for (int a = 0; a < 3; a++)
		{
			lo[a] = m.bmin[a] + (float)off[a];
			hi[a] = m.bmax[a] + (float)off[a];
		}
		for (int a = 0; a < 3; a++) { info.outerMn[a] = std::min(info.outerMn[a], lo[a]); info.outerMx[a] = std::max(info.outerMx[a], hi[a]); }
		if (def.kind == 2) { mask = true; for (int a = 0; a < 3; a++) { maskMn[a] = std::min(maskMn[a], lo[a]); maskMx[a] = std::max(maskMx[a], hi[a]); } }
		else if (def.kind == 1 || def.floor == 0) { ground = true; for (int a = 0; a < 3; a++) { groundMn[a] = std::min(groundMn[a], lo[a]); groundMx[a] = std::max(groundMx[a], hi[a]); } }
		if (def.kind == 1) { interiorLow[def.floor] = std::min(interiorLow[def.floor], lo[1]); }
	}
	info.hasBounds = mask || ground;
	for (int a = 0; a < 3; a++)
	{
		info.mn[a] = mask ? maskMn[a] : groundMn[a];
		info.mx[a] = mask ? maskMx[a] : groundMx[a];
	}
	// A floor starts at its interior slab. Most buildings have none (the interior mesh holds every storey with `building floor` 0, the shell parts of a floor start
	// at the ground), so the interior's height is split evenly between the floors instead.
	for (int f = 1; f <= b.maxFloor && f < 4; f++)
	{
		info.floorY[f] = interiorLow[f] < 1e29f ? interiorLow[f] : info.hasBounds ? info.mn[1] + (info.mx[1] - info.mn[1]) * f / (b.maxFloor + 1) : 1e30f;
	}
	return &binfo_.emplace(bi, info).first->second;
}

// Camera (or any world point) in a building's local frame, in mesh units before the building scale.
static void toLocal(const BuildingInstance& b, const double p[3], double out[3])
{
	const double rel[3] = { p[0] - b.x, p[1] - b.y, p[2] - b.z };
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	rotate(conj, rel, out);
	for (int a = 0; a < 3; a++) { out[a] /= b.scale; }
}

std::string WorldBuildings::recordOf(uint64_t key) const
{
	const auto it = key ? data_.byKey.find(key) : data_.byKey.end();
	return it == data_.byKey.end() ? std::string() : data_.names[data_.instances[it->second].nameId];
}

std::string WorldBuildings::nameOfInstance(int index) const
{
	if (index < 0 || (size_t)index >= data_.instances.size()) { return std::string(); }
	const uint32_t n = data_.instances[(size_t)index].nameId;
	return n < data_.names.size() ? data_.names[n] : std::string();
}

// The layout furniture piece a bed's sleeping spot `at` lies in: the piece whose oriented mesh box (grown by kBedSlack) holds the spot, the nearest
// to it when several do; -1 none. bedRay's search (through its memo) and the F12 shot's pick lines (kenshi-k16.2.234.1).
int WorldBuildings::bedPiece(const double at[3]) const
{
	constexpr double kBedSlack = 2, kBedSearch = 12;
	double bestD = 1e30;
	int found = -1;
	for (int dx = -3; dx <= 3; dx++)
	{
		for (int dz = -3; dz <= 3; dz++)
		{
			const auto g = furnGrid_.find(furnKey(at[0] + 4 * dx, at[2] + 4 * dz));
			if (g == furnGrid_.end()) { continue; }
			for (const uint32_t i : g->second)
			{
				const BuildingInstance& b = data_.instances[i];
				const double d = std::hypot(b.x - at[0], b.z - at[2]);
				if (d > kBedSearch || d >= bestD || !instanceHolds(i, at, kBedSlack)) { continue; }
				bestD = d;
				found = (int)i;
			}
		}
	}
	return found;
}

// One line on an instance for the F12 shot: its BUILDING name, placement, parent, kind and the part mesh files with the union of their loaded boxes
// in mesh units (the oriented box the pick uses, before the instance's rotation and scale)
std::string WorldBuildings::describeInstance(int index) const
{
	if (index < 0 || (size_t)index >= data_.instances.size()) { return "no instance"; }
	const BuildingInstance& b = data_.instances[(size_t)index];
	std::string name = nameOfInstance(index);
	if (name.empty()) { name = "no record name"; }
	char head[256];
	snprintf(head, sizeof head, "instance %d %s at %.1f %.1f %.1f scale %.2f%s%s%s", index, name.c_str(), b.x, b.y, b.z, b.scale, b.parent >= 0 ? " (layout piece in building instance " : "",
		b.parent >= 0 ? std::to_string(b.parent).c_str() : "", b.parent >= 0 ? ")" : b.prop ? " (prop)" : b.feature ? " (feature)" : "");
	std::string files;
	double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& def = data_.parts[pid];
		if (def.kind == 2) { continue; }
		if (files.size() < 160) { files += (files.empty() ? "" : ", ") + def.mesh; }
		const int mesh = partRt_[pid].mesh;
		if (mesh < 0 || meshes_[mesh].state != Ready) { continue; }
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		for (int a = 0; a < 3; a++) { lo[a] = std::min(lo[a], meshes_[mesh].bmin[a] + off[a]); hi[a] = std::max(hi[a], meshes_[mesh].bmax[a] + off[a]); }
	}
	char box[160] = "";
	if (lo[0] < 1e29) { snprintf(box, sizeof box, " mesh box %.0f %.0f %.0f - %.0f %.0f %.0f", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]); }
	return std::string(head) + "; mesh " + (files.empty() ? "none" : files) + box;
}

// The floorable building instance with the wire's `building` key, -1 = none (outside, or a building without floors)
int WorldBuildings::floorableOf(uint64_t key) const
{
	const auto it = key ? data_.byKey.find(key) : data_.byKey.end();
	return it != data_.byKey.end() && data_.instances[it->second].maxFloor > 0 ? (int)it->second : -1;
}

// ---- the staircases' flights (kenshi-k16.2.236.3; the floor model's ff::Flight, kenshi-k16.2.236.14) ----

// One staircase's top step from its own mesh (StairHot). The treads are the mesh's flat upward faces, grouped by height; a handrail's cap or a stringer is
// a narrow group at its own heights and drops out by width (the Storm House's stair mesh has a 6 u strip beside its 17 u treads). The treads' centres step
// along one horizontal axis: that is the flight's run, and the spacing is one tread. False: fewer than two full-width treads (a ramp, or a mesh we do not read this way).
bool WorldBuildings::stairTopStep(const MeshEntry& m, const float off[3], StairHot& hot)
{
	struct Face { float y, mnx, mxx, mnz, mxz; };
	std::vector<Face> faces;
	const size_t n = m.hullPos.size() / 3;
	for (size_t i = 0; i + 2 < m.hullIdx.size(); i += 3)
	{
		const uint32_t a = m.hullIdx[i], b = m.hullIdx[i + 1], c = m.hullIdx[i + 2];
		if (a >= n || b >= n || c >= n) { continue; }
		const float* A = &m.hullPos[a * 3];
		const float* B = &m.hullPos[b * 3];
		const float* C = &m.hullPos[c * 3];
		const double ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2];
		const double vx = C[0] - A[0], vy = C[1] - A[1], vz = C[2] - A[2];
		const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
		const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
		if (len < 1e-9 || ny / len < 0.98) { continue; }	// a tread is flat (a ramp's 30-40 degree top is out)
		faces.push_back({ std::max({ A[1], B[1], C[1] }), std::min({ A[0], B[0], C[0] }), std::max({ A[0], B[0], C[0] }),
			std::min({ A[2], B[2], C[2] }), std::max({ A[2], B[2], C[2] }) });
	}
	if (faces.size() < 2) { return false; }
	std::sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.y < r.y; });
	std::vector<Face> treads;
	for (const Face& f : faces)
	{
		if (!treads.empty() && f.y - treads.back().y <= 0.5f)	// the same tread's triangles
		{
			Face& t = treads.back();
			t.mnx = std::min(t.mnx, f.mnx); t.mxx = std::max(t.mxx, f.mxx);
			t.mnz = std::min(t.mnz, f.mnz); t.mxz = std::max(t.mxz, f.mxz);
			continue;
		}
		treads.push_back(f);
	}
	float widest = 0;
	for (const Face& t : treads) { widest = std::max({ widest, t.mxx - t.mnx, t.mxz - t.mnz }); }
	std::vector<const Face*> steps;
	for (const Face& t : treads) { if (std::max(t.mxx - t.mnx, t.mxz - t.mnz) >= 0.6f * widest) { steps.push_back(&t); } }
	if (steps.size() < 2) { return false; }
	const Face& foot = *steps.front();
	const Face& top = *steps.back();
	// the whole flight's footprint (every full-width tread: the top step's hotspot is only part of it) and the foot tread's own height
	float fmn0 = 1e30f, fmx0 = -1e30f, fmn1 = 1e30f, fmx1 = -1e30f;
	for (const Face* s : steps)
	{
		fmn0 = std::min(fmn0, s->mnx); fmx0 = std::max(fmx0, s->mxx);
		fmn1 = std::min(fmn1, s->mnz); fmx1 = std::max(fmx1, s->mxz);
	}
	hot.fmn[0] = fmn0 + off[0]; hot.fmx[0] = fmx0 + off[0];
	hot.fmn[1] = fmn1 + off[2]; hot.fmx[1] = fmx1 + off[2];
	hot.yLow = foot.y + off[1];
	const double fx = 0.5 * (foot.mnx + foot.mxx), fz = 0.5 * (foot.mnz + foot.mxz);
	const double tx = 0.5 * (top.mnx + top.mxx), tz = 0.5 * (top.mnz + top.mxz);
	const int axis = std::fabs(tx - fx) >= std::fabs(tz - fz) ? 0 : 1;	// the flight runs along x, or z
	const double run = axis == 0 ? tx - fx : tz - fz;
	hot.tread = (float)(std::fabs(run) / (double)(steps.size() - 1));	// one tread's run, mesh units
	hot.rise = (float)((top.y - foot.y) / (double)(steps.size() - 1));	// one tread's rise, the same spacing: the feet a rise under the top tread are one step down the flight
	if (hot.tread < 0.5f) { return false; }
	hot.y = top.y + off[1];
	// The two tread centres: the floor model's stair latch needs the flight's slope for its "on the flight" test (kenshi-k16.2.236.11; the
	// riser's height is measured above, for the down flip).
	hot.footC[0] = (float)fx + off[0]; hot.footC[1] = (float)fz + off[2];
	hot.topC[0] = (float)tx + off[0]; hot.topC[1] = (float)tz + off[2];
	return true;
}

std::vector<WorldBuildings::StairHot>* WorldBuildings::stairHots(uint32_t bi)
{
	if (const auto it = stairs_.find(bi); it != stairs_.end()) { return &it->second; }
	std::vector<StairHot> out;
	if (bi < data_.instances.size())
	{
		const BuildingInstance& b = data_.instances[bi];
		if (b.parent < 0 && b.maxFloor > 0)
		{
			for (uint32_t k = 0; k < b.partCount; k++)
			{
				const uint32_t pid = data_.partRefs[b.firstPart + k];
				const BuildingPartDef& def = data_.parts[pid];
				if (!def.stairs) { continue; }
				ensurePart(pid);
				const int mesh = partRt_[pid].mesh;
				if (mesh < 0) { continue; }	// no mesh to measure (and nothing drawn)
				if (meshes_[mesh].state != Ready) { return nullptr; }	// not loaded yet: retry next frame
				StairHot h;
				h.lower = std::min(def.floor, (int)b.maxFloor);	// BUILDING_PART `building floor`: the floor the flight starts on; the one above it is `upper`
				h.upper = std::min(def.floor + 1, (int)b.maxFloor);
				if (h.upper <= h.lower) { continue; }	// a staircase on the top floor leads to no floor this view has (the roof deck is not one)
				double off[3];
				toMesh(offsetFrame, def.offset, off);
				const float offset[3] = { (float)off[0], (float)off[1], (float)off[2] };
				if (stairTopStep(meshes_[mesh], offset, h))
				{
#ifdef KCLIENT_DEV_TOOLS
					if (logFloor)
					{
						printf("floor: stair flight building %u %s floors %d -> %d: tread %.1f u, rise %.1f u, footprint x %.1f..%.1f z %.1f..%.1f y %.1f..%.1f\n", bi,
							nameOfInstance((int)bi).c_str(), h.lower, h.upper, h.tread, h.rise, h.fmn[0], h.fmx[0], h.fmn[1], h.fmx[1], h.yLow, h.y);
					}
#endif
					out.push_back(h);
				}
			}
		}
	}
	return &stairs_.emplace(bi, std::move(out)).first->second;
}

void WorldBuildings::updateFocus()
{
#ifdef KCLIENT_DEV_TOOLS
	// kenshi-k16.2.236.10: one line per change of the shown level, whichever rule (or the player's R / F, restore, reset) wrote it; every return path prints it.
	struct ViewLog
	{
		WorldBuildings& w;
		~ViewLog()
		{
			if (w.viewLogged_ != w.visible_)
			{
				if (w.logFloor) { printf("floor: view %d -> %d (%s)\n", w.viewLogged_, w.visible_, w.viewWhy_); }
				w.viewLogged_ = w.visible_;
				w.viewWhy_ = "player";
			}
		}
	} viewLog{ *this };
#endif
	focus_ = -1;
	floor_ = FloorStatus();
	cuts_.clear();
	for (const Occupant& o : occupants)
	{
		const int bi = o.instance >= 0 ? o.instance : floorableOf(o.building);
		if (bi >= 0 && !sliced(bi)) { cuts_.push_back((uint32_t)bi); }
#ifdef KCLIENT_DEV_TOOLS
		if (bi < 0 && logFloor)	// kenshi-k16.2.355: a wire building the client has no floorable instance for (once per key)
		{
			static uint64_t last = 0;
			if (last != o.building)
			{
				last = o.building;
				const auto k = o.building ? data_.byKey.find(o.building) : data_.byKey.end();
				printf("floor: wire building %llu has no floorable instance (%s%s)\n", (unsigned long long)o.building, k == data_.byKey.end() ? "not a placement key" : "placement known, maxFloor ",
					k == data_.byKey.end() ? "" : std::to_string((int)data_.instances[k->second].maxFloor).c_str());
			}
		}
#endif
	}
	int in = -1, autoFloor = -1;
	if (hasLead)
	{
		in = lead.instance >= 0 ? lead.instance : floorableOf(lead.building);
	}
	const int leadFloor = leadId == 0 ? -1 : lead.instance >= 0 ? data_.instances[lead.instance].maxFloor : in >= 0 ? lead.floor : 0;	// the lead's own floor; 0 outdoors
	// The floor view (kenshi-k16.2.236.14; the fable review kenshi-k16.2.236.9 section 3): one sample of the camera subject per frame into the model,
	// and the level it returns is the one shown. The sample's building is the one the model's stair latch is on while latched, else the wire's
	// building, else the roof deck's; the position is the subject's sim position in that building's local frame, the flights that building's.
	// The old rules (the snap on engage, the auto threshold, the stair hotspot) are in client/retired/floor_threshold_rules.cpp.
	{
		ff::Sample s;
		s.following = following;
		s.subject = subjectId;
		s.known = subjectKnown;
		s.hasPos = hasSubjectPos;
		s.wireFloor = subjectFloor;
		s.indoors = subjectIndoors;
		s.roof = !subjectIndoors ? subjectRoof : -1;
		s.roofFloor = s.roof >= 0 ? topFloor(s.roof) : 0;
		const int wire = floorableOf(subjectBuilding);
		const int latched = view_.subject() == subjectId ? view_.latchBuilding() : -1;	// another subject's latch says nothing about this one
		s.building = latched >= 0 ? latched : wire >= 0 ? wire : s.roof;
		flights_.clear();
		if (s.building >= 0 && (size_t)s.building < data_.instances.size())
		{
			if (hasSubjectPos) { toLocal(data_.instances[s.building], subjectPos, s.pos); }
			if (const std::vector<StairHot>* hots = stairHots((uint32_t)s.building))	// null: a staircase mesh still loading (the model holds its latch)
			{
				for (const StairHot& h : *hots)
				{
					ff::Flight f;
					f.mn[0] = h.fmn[0]; f.mn[1] = h.fmn[1];
					f.mx[0] = h.fmx[0]; f.mx[1] = h.fmx[1];
					f.footY = h.yLow;
					f.topY = h.y;
					f.tread = h.tread;
					f.rise = h.rise;
					f.foot[0] = h.footC[0]; f.foot[1] = h.footC[1];
					f.top[0] = h.topC[0]; f.top[1] = h.topC[1];
					f.lower = h.lower;
					f.upper = h.upper;
					flights_.push_back(f);
				}
			}
		}
		s.flights = flights_.empty() ? nullptr : flights_.data();
		s.flightCount = (int)flights_.size();
		const ff::View v = view_.update(s);
		visible_ = v.level;
		viewWhy_ = ff::reasonName(v.reason);
		autoFloor = view_.autoFloor();
	}
	if (cuts_.empty())
	{
		const auto pk = std::find_if(peeks.begin(), peeks.end(), [](const auto& e) { return e.second.on; });
		if (pk == peeks.end()) { return; }
		focus_ = (int)pk->first;	// a peek: the selector keeps the player's level; the peek's cut is that level clamped to the building's top (update_peek, stepFloor)
		floor_ = { true, data_.names[data_.instances[focus_].nameId], std::min(visible_, topFloor(focus_)), topFloor(focus_), -1, true, leadFloor, following };
		return;
	}
	focus_ = in >= 0 && sliced(in) ? in : (int)cuts_[0];
	int top = 0;
	for (const uint32_t bi : cuts_) { top = std::max<int>(top, data_.instances[bi].maxFloor); }
	floor_ = { true, data_.names[data_.instances[focus_].nameId], std::min(visible_, top), top, autoFloor, false, leadFloor, following };
}

// Where the segment s..e (a building's local frame) enters and leaves the box mn..mx, as fractions of it; false: it misses.
static bool segmentBox(const double s[3], const double e[3], const float mn[3], const float mx[3], double& t0, double& t1)
{
	t0 = 0;
	t1 = 1;
	for (int a = 0; a < 3 && t0 <= t1; a++)
	{
		const double d = e[a] - s[a];
		if (std::abs(d) < 1e-9)
		{
			if (s[a] < mn[a] || s[a] > mx[a]) { return false; }
			continue;
		}
		double u0 = (mn[a] - s[a]) / d, u1 = (mx[a] - s[a]) / d;
		if (u0 > u1) { std::swap(u0, u1); }
		t0 = std::max(t0, u0);
		t1 = std::min(t1, u1);
	}
	return t0 <= t1;
}

int WorldBuildings::rayBuilding(const double origin[3], const double dir[3], double far, const double* at)
{
	int best = -1;
	double bestT = at ? -1 : far;
	for (const uint32_t i : topLevel_)	// the static instances that are not a feature or furniture
	{
		const BuildingInstance& b = data_.instances[i];
		if (std::abs(b.x - origin[0]) > far + kFocusRange || std::abs(b.z - origin[2]) > far + kFocusRange) { continue; }
		double t, leave;
		if (!rayBox((int)i, origin, dir, far, t, leave)) { continue; }
		if (at ? holds((int)i, at) && t > bestT : t < bestT) { bestT = t; best = (int)i; }
	}
	return best;
}

bool WorldBuildings::rayBox(int building, const double origin[3], const double dir[3], double far, double& enter, double& leave, bool interior)
{
	const BInfo* info = building >= 0 ? infoFor((uint32_t)building) : nullptr;
	if (!info || !info->hasBounds || info->outerMn[0] > info->outerMx[0]) { return false; }
	const float* mn = interior ? info->mn : info->outerMn;
	const float* mx = interior ? info->mx : info->outerMx;
	const BuildingInstance& b = data_.instances[building];
	const double end[3] = { origin[0] + dir[0] * far, origin[1] + dir[1] * far, origin[2] + dir[2] * far };
	double s[3], e[3];
	toLocal(b, origin, s);
	toLocal(b, end, e);
	if (!segmentBox(s, e, mn, mx, enter, leave)) { return false; }
	enter *= far;
	leave *= far;
	return true;
}

bool WorldBuildings::holds(int building, const double p[3], bool interior)
{
	const BInfo* info = building >= 0 ? infoFor((uint32_t)building) : nullptr;
	if (!info) { return false; }
	const float* mn = interior ? info->mn : info->outerMn;
	const float* mx = interior ? info->mx : info->outerMx;
	double l[3];
	toLocal(data_.instances[building], p, l);
	for (int a = 0; a < 3; a++) { if (l[a] < mn[a] || l[a] > mx[a]) { return false; } }
	return true;
}

int WorldBuildings::topFloor(int building) const { return building >= 0 ? data_.instances[building].maxFloor : 0; }

bool WorldBuildings::belowFloors(int building, const double p[3])
{
	constexpr double kSlab = 2;	// MINE: local units under the interior's bottom that still count as its floor
	const BInfo* info = building >= 0 ? infoFor((uint32_t)building) : nullptr;
	if (!info || !info->hasBounds) { return false; }
	double l[3];
	toLocal(data_.instances[building], p, l);
	return l[1] < info->mn[1] - kSlab;
}

int WorldBuildings::floorAt(int building, const double p[3])
{
	const BInfo* info = building >= 0 ? infoFor((uint32_t)building) : nullptr;
	if (!info) { return 0; }
	double l[3];
	toLocal(data_.instances[building], p, l);
	int floor = 0;
	for (int f = 1; f < 4; f++) { if (info->floorY[f] <= l[1]) { floor = f; } }
	return floor;
}

// The buildings the segment from the camera to seen's chest crosses (in their own frame, then their collision shapes: lineHits), with neither end inside,
// and not the one seen stands on.
void WorldBuildings::findOccluders(const double cam[3], std::string* report)
{
	occluders_.clear();
	if (!autoGhost || !hasSeen) { return; }
	const double chest[3] = { seen[0], seen[1] + 10.8, seen[2] };
	const double len = std::sqrt((chest[0] - cam[0]) * (chest[0] - cam[0]) + (chest[1] - cam[1]) * (chest[1] - cam[1]) + (chest[2] - cam[2]) * (chest[2] - cam[2]));
	const double reach = len + kFocusRange;	// MINE: a building's origin farther than this from the camera cannot reach the segment
	// "seen is inside": the wire's indoors bit and building key (CHAR_DYNAMIC flag 19): in a building's interior AND under a ceiling. The old interior-bounds
	// box said yes on a porch (inside the footprint, outside the mask), so a building the character merely stood beside did not ghost.
	const auto seenIn = seenIndoors && seenBuilding ? data_.byKey.find(seenBuilding) : data_.byKey.end();
	// a candidate instance i: the report walks every static instance (it lists the features and the furniture as skipped), the frame only the
	// top-level buildings (topLevel_, the same instances in the same order that pass the kind test)
	const auto visit = [&](uint32_t i)
	{
		const BuildingInstance& b = data_.instances[i];
		const char* kind = b.feature ? "feature" : b.parent >= 0 ? "child" : nullptr;	// never occluders; listed in the report
		if ((kind && !report) || std::abs(b.x - cam[0]) > reach || std::abs(b.z - cam[2]) > reach) { return; }
		const BInfo* info = infoFor((uint32_t)i);
		if (!info || info->outerMn[0] > info->outerMx[0]) { return; }
		double s[3], e[3];
		toLocal(b, cam, s);
		toLocal(b, chest, e);
		// crossing its parts' box; skipped while one of ours is in it (sliced: the floor cut shows that inside) or the camera is inside the interior bounds AND
		// the seen one is indoors in it (the wire's bit: the chest is inside, not merely within the bounds box as on a porch). The camera in the bounds alone
		// does not skip it: the follow camera inside the bar looking at a character KO'd outside its door saw only the wall (2026-11-05); nor the bit alone:
		// watching an NPC indoors from outside must ghost the wall (spectator). Under The Hub's gate walkway (2026-11-05) the feet are below the bounds and the
		// bit is clear, so the gate ghosts
		const auto in = [&](const double p[3]) { for (int a = 0; a < 3; a++) { if (p[a] < info->mn[a] || p[a] > info->mx[a]) { return false; } } return true; };
		double t0, t1;
		if (!segmentBox(s, e, info->outerMn, info->outerMx, t0, t1)) { return; }
		if (kind) { *report += describe((uint32_t)i) + ": skipped, " + kind + "\n"; return; }
		// nor one seen stands on (user 2026-11-05: walking into a ruin's rubble faded it; looking inside is the floor button's job): its shape under the feet
		constexpr double kStand = 3;	// MINE: the feet probe's reach above and below the feet, units (well under the gate walkway's height)
		const double over[3] = { seen[0], seen[1] + kStand, seen[2] }, under[3] = { seen[0], seen[1] - kStand, seen[2] };
		const bool seenHere = seenIn != data_.byKey.end() && seenIn->second == i;
		const char* why = sliced((int)i) ? "ours inside" : seenHere && info->hasBounds && in(s) ? "seen indoors, camera inside"
			: seenHere ? lineHits && !lineHits(cam, chest, (uint32_t)i) ? "line misses its shapes" : nullptr	// indoors in it: from outside the wall ghosts, not "stands on"
			: lineHits && lineHits(over, under, (uint32_t)i) ? "seen stands on it" : lineHits && !lineHits(cam, chest, (uint32_t)i) ? "line misses its shapes" : nullptr;
		if (report) { *report += describe((uint32_t)i) + (why ? std::string(": skipped, ") + why : ": occluder") + "\n"; }
		if (!why) { occluders_.insert((uint32_t)i); }
	};
	if (report) { for (size_t i = 0; i < staticCount_; i++) { visit((uint32_t)i); } }
	else { for (const uint32_t i : topLevel_) { visit(i); } }
}

std::string WorldBuildings::ghostState() const
{
	std::string r;
	for (const auto& [bi, a] : ghosts_)
	{
		char b[64];
		snprintf(b, sizeof b, " fade %.2f%s", a, occluders_.count(bi) ? " occluder" : " fading back");
		r += (r.empty() ? "" : "; ") + describe(bi) + b;
	}
	return r.empty() ? "none" : r;
}

void WorldBuildings::fadeGhosts(double dt, double in, double out)
{
	for (const uint32_t bi : occluders_) { ghosts_.emplace(bi, 0.0f); }
	for (auto it = ghosts_.begin(); it != ghosts_.end();)
	{
		const bool on = occluders_.count(it->first) > 0;
		it->second = (float)std::clamp(it->second + (on ? dt / in : -dt / out), 0.0, 1.0);
		it = !on && it->second <= 0 ? ghosts_.erase(it) : std::next(it);
	}
}

// p (its y capped at bottom) in the building's parts' box (xz), at or over the slab of floor s + 1
bool WorldBuildings::aboveSlab(uint32_t bi, int s, const double p[3], double bottom)
{
	constexpr double kSlab = 4;	// MINE: local units under the slab's low face that still count as the slab
	const BuildingInstance& b = data_.instances[bi];
	const BInfo* info = s < std::min<int>(b.maxFloor, 3) ? infoFor(bi) : nullptr;
	if (!info || !info->hasBounds) { return false; }
	const double q[3] = { p[0], std::min(p[1], bottom), p[2] };
	double l[3];
	toLocal(b, q, l);
	return l[0] >= info->outerMn[0] && l[0] <= info->outerMx[0] && l[2] >= info->outerMn[2] && l[2] <= info->outerMx[2] && l[1] >= info->floorY[s + 1] - kSlab;
}

bool WorldBuildings::cutAway(const double p[3], double bottom, std::string* why)
{
	const auto above = [&](uint32_t bi, int s) { return aboveSlab(bi, s, p, bottom); };
	const auto nameOf = [&](uint32_t bi) { const uint32_t n = data_.instances[bi].nameId; return n < data_.names.size() ? data_.names[n] : std::string("a building"); };
	const auto inside = [&](uint32_t bi)	// an occluder: anywhere in its parts' box
	{
		const BInfo* info = infoFor(bi);
		if (!info) { return false; }
		double l[3];
		toLocal(data_.instances[bi], p, l);
		for (int a = 0; a < 3; a++) { if (l[a] < info->outerMn[a] || l[a] > info->outerMx[a]) { return false; } }
		return true;
	};
	if (std::any_of(occluders_.begin(), occluders_.end(), inside))
	{
		if (why) { *why = "an occluder faded to glass"; }
		return true;
	}
	const bool byLevel = levelCuts;	// the click pick: the player's level, clamped to each building's top, cuts the buildings nobody is in
	if (peekPass && !byLevel && std::any_of(peeks.begin(), peeks.end(), [&](const auto& pk) { return pk.second.on && above(pk.first, pk.second.floor); }))
	{
		if (why) { for (const auto& pk : peeks) { if (pk.second.on && above(pk.first, pk.second.floor)) { *why = "above the peeked floor of " + nameOf(pk.first); break; } } }
		return true;
	}
	if (std::any_of(cuts_.begin(), cuts_.end(), [&](uint32_t bi) { return above(bi, shownOf(bi)); }))
	{
		if (why) { for (const uint32_t bi : cuts_) { if (above(bi, shownOf(bi))) { *why = "above the floor cut of " + nameOf(bi) + " (a building ours are in, showing floor " + std::to_string(shownOf(bi)) + ")"; break; } } }
		return true;
	}
	if (!byLevel) { return false; }
	for (const uint32_t bi : data_.floorable)
	{
		const BuildingInstance& b = data_.instances[bi];
		if (b.parent >= 0 || sliced((int)bi) || std::abs(b.x - p[0]) > kFocusRange || std::abs(b.z - p[2]) > kFocusRange) { continue; }
		if (above(bi, visible_))
		{
			if (why) { *why = "above the level cut of " + nameOf(bi) + " (nobody of ours inside; the level is the cut)"; }
			return true;
		}
	}
	return false;
}

bool WorldBuildings::cutHides(const double p[3], double bottom)
{
	return std::any_of(cuts_.begin(), cuts_.end(), [&](uint32_t bi) { return aboveSlab(bi, shownOf(bi), p, bottom); });
}

int WorldBuildings::peekCutFloor(const double p[3], double bottom)
{
	for (const auto& pk : peeks) { if (pk.second.on && aboveSlab(pk.first, pk.second.floor, p, bottom)) { return pk.second.floor; } }
	return -1;
}

bool WorldBuildings::stepFloor(int dir)
{
	// The floor model steps (kenshi-k16.2.236.14): from the floor the player sees, clamped to the building's (or the sliced ones') top, e.g. 3 on a
	// one-floor house steps down to 0; nothing sliced or peeked: the global level 0..3 alone (always live, the original's selector). Following, the
	// step is the player's manual level and holds until the subject's next threshold (a door, the stair latch, a roof, another building).
	const int top = floor_.valid ? floor_.top : 3;
	const int base = std::min(visible_, top);
	view_.step(dir, top);
	visible_ = view_.shown();
	if (floor_.valid && floor_.peek) { peeks[(uint32_t)focus_].floor = visible_; }	// at the top the roof deck shows uncut (as the inside view)
	if (floor_.valid) { floor_.shown = visible_; }
	return visible_ != base;
}

int WorldBuildings::roofBuilding(const double p[3])
{
	constexpr double kReach = 12;	// MINE: local units a roof deck's feet may stand over the parts' box top (a chimney, a parapet)
	for (const uint32_t bi : data_.floorable)
	{
		const BuildingInstance& b = data_.instances[bi];
		if (b.parent >= 0 || std::abs(b.x - p[0]) > kFocusRange || std::abs(b.z - p[2]) > kFocusRange) { continue; }
		const BInfo* info = infoFor(bi);
		if (!info || !info->hasBounds) { continue; }
		double l[3];
		toLocal(b, p, l);
		const int top = std::min<int>(b.maxFloor, 3);
		if (l[0] >= info->outerMn[0] && l[0] <= info->outerMx[0] && l[2] >= info->outerMn[2] && l[2] <= info->outerMx[2]
			&& l[1] >= info->floorY[top] && l[1] <= info->outerMx[1] + kReach) { return (int)bi; }
	}
	return -1;
}

// kenshi-k16.2.266 (measured, floortrace in main.cpp): at the top step the wire's indoors bit clears while building and floor still name the building and its top
// floor, and roofBuilding's box test misses for 2-31 sim ticks before it hits. That gap read as outdoors and wrote floor 0 between the stairs' 1 and the roof deck's 1.
// The wire never has the indoors bit clear with a building key set anywhere else (outdoors it is building 0), so the key is the roof deck's building here.
int WorldBuildings::roofBuilding(const double p[3], uint64_t wireBuilding)
{
	const int roof = roofBuilding(p);
	return roof >= 0 ? roof : floorableOf(wireBuilding);
}

void WorldBuildings::setConstruction(uint32_t instance, float progress)
{
	if (progress >= 1.0f) { construction_.erase(instance); }
	else { construction_[instance] = progress; }
}

std::string WorldBuildings::describe(uint32_t instance) const
{
	const BuildingInstance& b = data_.instances[instance];
	char line[256];
	snprintf(line, sizeof line, "%s at %.0f,%.0f,%.0f (height %.0f)", b.feature ? "(feature)" : data_.names[b.nameId].c_str(), b.x, b.y, b.z, b.height);
	return line;
}

std::string WorldBuildings::partsReport(uint32_t instance) const
{
	const BuildingInstance& b = data_.instances[instance];
	char head[256];
	snprintf(head, sizeof head, " rot wxyz %.4f %.4f %.4f %.4f scale %.2f ground %.1f\n", b.qw, b.qx, b.qy, b.qz, b.scale, b.ground);
	std::string out = describe(instance) + head;
	static const char* const kKind[] = { "shell", "interior", "mask", "furniture" };
	// What the part's draw binds per submesh (kenshi-k16.2.298.1): the resolved diffuse file and its load state, "none" for the material rule's grey.
	auto texState = [](State s) { return s == Ready ? "ready" : s == Loading ? "loading" : s == Uploading ? "uploading" : s == Failed ? "FAILED" : "unloaded"; };
	auto tex = [&](int t) { return t < 0 ? std::string("none") : textures_[t].file + " (" + texState(textures_[t].state) + ")"; };
	auto subs = [&](uint32_t pid) {
		std::string o;
		const PartRt& rt = partRt_[pid];
		const MeshEntry* m = rt.mesh >= 0 ? &meshes_[rt.mesh] : nullptr;
		for (size_t si = 0; si < rt.subTex.size(); si++)
		{
			o += "    sub " + std::to_string(si) + " " + (m && si < m->subs.size() ? m->subs[si].material : std::string("?")) + " diffuse " + tex(rt.subTex[si]) + "\n";
		}
		return o;
	};
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const BuildingPartDef& d = data_.parts[pid];
		const PartRt& rt = partRt_[pid];
		char line[1024];
		snprintf(line, sizeof line, "  part %u %s floor %d type %d mesh %s tex %s offset %.1f %.1f %.1f%s mesh state %d\n", pid, kKind[d.kind & 3], d.floor, d.materialType,
			d.mesh.c_str(), d.texture.empty() ? "-" : d.texture.c_str(), d.offset[0], d.offset[1], d.offset[2], d.marker ? " marker" : "",
			rt.mesh >= 0 ? (int)meshes_[rt.mesh].state : -1);
		out += line;
		out += subs(pid);
	}
	for (size_t i = 0; i < staticCount_; i++)	// its layout furniture: name, part meshes and their load state
	{
		const BuildingInstance& f = data_.instances[i];
		if (f.parent != (int)instance) { continue; }
		char line[512];
		// yaw about y, in degrees, of the furniture and relative to the building (both quaternions are y rotations for a level layout; q shows any tilt)
		const double yaw = 2.0 * std::atan2(f.qy, f.qw) * 180.0 / M_PI;
		const double rel = std::remainder(yaw - 2.0 * std::atan2(b.qy, b.qw) * 180.0 / M_PI, 360.0);
		snprintf(line, sizeof line, "  %s %zu at %.0f,%.0f,%.0f yaw %.1f (building %+.1f) q %.3f %.3f %.3f %.3f floor %d%s parts %u:", f.door ? "door leaf" : "furniture", i, f.x, f.y, f.z, yaw, rel,
			f.qw, f.qx, f.qy, f.qz, f.floor, f.ceiling ? " ceiling" : "", f.partCount);
		out += line;
		for (uint32_t k = 0; k < f.partCount; k++)
		{
			const uint32_t pid = data_.partRefs[f.firstPart + k];
			snprintf(line, sizeof line, " %s (%d)", data_.parts[pid].mesh.c_str(), partRt_[pid].mesh >= 0 ? (int)meshes_[partRt_[pid].mesh].state : partRt_[pid].mesh);
			out += line;
		}
		out += "\n";
		if (f.door) { for (uint32_t k = 0; k < f.partCount; k++) { out += subs(data_.partRefs[f.firstPart + k]); } }	// a door leaf's binds: its grey box is kenshi-k16.2.256.8.1
	}
	return out;
}

// One-off lookup (scenes, tools): a linear scan.
int WorldBuildings::nearestBuilding(double x, double z) const
{
	int best = -1;
	double bestD = kFocusRange;
	for (size_t i = 0; i < staticCount_; i++)
	{
		const BuildingInstance& b = data_.instances[i];
		if (b.feature || b.parent >= 0) { continue; }
		const double d = std::hypot(b.x - x, b.z - z);
		if (d < bestD) { bestD = d; best = (int)i; }
	}
	return best;
}

// dump feetfloor (kenshi-k16.17.7.24.4): the top of the drawn floor under (x, z). Every part the building draws (the floor cut leaves the ones above the
// shown floor out) has its mesh decoded and each triangle taken through the same transform the instance vertex shader uses: p = b.pos + qrot(q,
// (v + toMesh(offset)) * stretch * scale). The highest triangle containing (x, z) at or below fromY, within range, is the surface under the point; a
// turning / wind-aimed part is skipped (its pose needs the wind clock). Kinds: 'cut' a part hidden at the shown floor, 'spin' a part that turns.
std::string WorldBuildings::drawnFloorReport(uint64_t key, double x, double z, double fromY, double range) const
{
	char line[704];
	int bi = -1;
	const char* how = "nearest";
	if (key != 0)
	{
		const auto it = data_.byKey.find(key);
		if (it != data_.byKey.end() && it->second < data_.instances.size() && data_.instances[it->second].parent < 0) { bi = (int)it->second; how = "wire building"; }
	}
	if (bi < 0) { bi = nearestBuilding(x, z); }
	if (bi < 0)
	{
		snprintf(line, sizeof line, "drawn floor at %.2f %.2f: no building within %.0f\n", x, z, kFocusRange);
		return line;
	}
	const BuildingInstance& b = data_.instances[bi];
	const int shown = b.maxFloor > 0 ? shownOf((uint32_t)bi) : 3;
	const std::string name = b.nameId < data_.names.size() ? data_.names[b.nameId] : std::string("-");
	snprintf(line, sizeof line, "drawn floor at %.2f %.2f from %.3f: building %d \"%s\" (%s) at %.1f %.1f %.1f scale %.3f, shown floor %d/%d\n",
		x, z, fromY, bi, name.c_str(), how, b.x, b.y, b.z, (double)b.scale, shown, b.maxFloor);
	std::string out = line;
	// the client's own floor table at the point (kenshi-k16.2.264.1): floorAt's number and the slab heights it reads, so the wire's floor beside it
	// in `dump cut` shows whether the sim and the client number this building's floors the same way round.
	if (const auto it = binfo_.find((uint32_t)bi); it != binfo_.end() && it->second.hasBounds)	// the cached table (infoFor fills it; this report is const)
	{
		const BInfo* info = &it->second;
		const double at3[3] = { x, fromY, z };
		double l[3];
		toLocal(b, at3, l);
		int at = 0;
		for (int f = 1; f < 4; f++) { if (info->floorY[f] <= l[1]) { at = f; } }
		snprintf(line, sizeof line, "  floors: local y %.3f is client floor %d; slabs %.2f %.2f %.2f %.2f (outer y %.2f..%.2f)\n", (double)l[1], at,
			(double)info->floorY[0], (double)info->floorY[1], (double)info->floorY[2], (double)info->floorY[3], (double)info->outerMn[1], (double)info->outerMx[1]);
		out += line;
	}
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	std::unordered_map<int, MeshCpu> cache;	// this building's meshes, decoded once
	double topY = -1e30, topPart = -1;
	std::string topMesh;
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const uint32_t pid = data_.partRefs[b.firstPart + k];
		const PartRt& rt = partRt_[pid];
		const BuildingPartDef& def = data_.parts[pid];
		if (def.kind == 2 || def.marker) { continue; }	// mask hulls are not drawn, a marker is not a floor
		if (rt.mesh < 0 || !rt.resolved || meshes_[rt.mesh].state != Ready) { continue; }
		const std::string& meshPath = meshes_[rt.mesh].path;
		if (def.floor > shown)
		{
			snprintf(line, sizeof line, "  part %u %s: cut (floor %d, hidden at %d)\n", k, def.mesh.c_str(), def.floor, shown);
			out += line;
			continue;
		}
		if (spinning(def) || def.aimed)
		{
			snprintf(line, sizeof line, "  part %u %s: spin (rotation function %d, not measured)\n", k, def.mesh.c_str(), def.rotFunction);
			out += line;
			continue;
		}
		auto it = cache.find(rt.mesh);
		if (it == cache.end())
		{
			MeshCpu mesh;
			if (!decodeMesh(meshPath, mesh)) { continue; }
			it = cache.emplace(rt.mesh, std::move(mesh)).first;
		}
		const MeshCpu& mesh = it->second;
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		double best = -1e30, bestN[3] = {};
		int bestTri = -1;
		for (const Sub& s : mesh.subs)
		{
			for (int i = 0; i + 2 < s.indexCount; i += 3)
			{
				double p[3][3];
				for (int c = 0; c < 3; c++)
				{
					const float* v = &mesh.verts[((size_t)s.firstVertex + mesh.indices[(size_t)s.firstIndex + i + c]) * kVertexFloats];
					const double local[3] = { ((double)v[0] + off[0]) * b.stretch[0] * b.scale, ((double)v[1] + off[1]) * b.stretch[1] * b.scale,
						((double)v[2] + off[2]) * b.stretch[2] * b.scale };
					double r[3];
					rotate(q, local, r);
					p[c][0] = b.x + r[0];
					p[c][1] = b.y + r[1];
					p[c][2] = b.z + r[2];
				}
				// the point's barycentric coordinates in the triangle's xz projection
				const double ax = p[1][0] - p[0][0], az = p[1][2] - p[0][2], bx = p[2][0] - p[0][0], bz = p[2][2] - p[0][2];
				const double det = ax * bz - az * bx;
				if (std::abs(det) < 1e-12) { continue; }
				const double px = x - p[0][0], pz = z - p[0][2];
				const double u = (px * bz - pz * bx) / det, v = (ax * pz - az * px) / det;
				if (u < 0 || v < 0 || u + v > 1) { continue; }
				const double y = p[0][1] + u * (p[1][1] - p[0][1]) + v * (p[2][1] - p[0][1]);
				if (y > fromY || y < fromY - range) { continue; }
				if (y > best)
				{
					best = y;
					bestTri = (int)(i / 3);
					const double e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] }, e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
					double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
					const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					for (int j = 0; j < 3; j++) { bestN[j] = len > 1e-12 ? n[j] / len : 0; }
				}
			}
		}
		if (best > -1e29)
		{
			snprintf(line, sizeof line, "  part %u floor %d kind %d %s: y %.3f (tri %d n %.2f %.2f %.2f)\n", k, def.floor, def.kind, def.mesh.c_str(), best, bestTri,
				bestN[0], bestN[1], bestN[2]);
			out += line;
			if (best > topY) { topY = best; topPart = k; topMesh = def.mesh; }
		}
	}
	if (topPart >= 0) { snprintf(line, sizeof line, "  top: y %.3f part %.0f %s\n", topY, topPart, topMesh.c_str()); }
	else { snprintf(line, sizeof line, "  top: none\n"); }
	return out + line;
}

// Read accessors for the dev pick-box report (src/dev/pickboxes.cpp, kenshi-k16.2.260.1): the same instance transform the vertex shader and instanceRay use.
// partToWorld: p = origin + qrot(q, (v + off) * stretch * scale) (mesh.glsl inst_vs, no spin / aim: furniture, props); instanceRay applies its inverse.
void WorldBuildings::partToWorld(uint32_t index, const double v[3], const double off[3], double out[3]) const
{
	const BuildingInstance& b = data_.instances[index];
	const double local[3] = { (v[0] + off[0]) * b.stretch[0] * b.scale, (v[1] + off[1]) * b.stretch[1] * b.scale, (v[2] + off[2]) * b.stretch[2] * b.scale };
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	double r[3];
	rotate(q, local, r);
	for (int a = 0; a < 3; a++) { out[a] = (a == 0 ? b.x : a == 1 ? b.y : b.z) + r[a]; }
}

bool WorldBuildings::partGeometry(uint32_t index, uint32_t k, bool withVerts, PartGeometry& out) const
{
	if (index >= data_.instances.size()) { return false; }
	const BuildingInstance& b = data_.instances[index];
	if (k >= b.partCount) { return false; }
	const uint32_t pid = data_.partRefs[b.firstPart + k];
	const BuildingPartDef& def = data_.parts[pid];
	const int mesh = partRt_[pid].mesh;
	if (def.kind == 2 || mesh < 0 || meshes_[mesh].state != Ready) { return false; }
	const MeshEntry& m = meshes_[mesh];
	out.mesh = def.mesh;
	toMesh(offsetFrame, def.offset, out.off);
	for (int a = 0; a < 3; a++) { out.bmin[a] = m.bmin[a]; out.bmax[a] = m.bmax[a]; }
	out.xyz.clear();
	if (!withVerts) { return true; }
	MeshCpu cpu;
	if (!decodeMesh(m.path, cpu)) { return false; }
	for (const Sub& s : cpu.subs)
	{
		for (int i = 0; i + 2 < s.indexCount; i += 3)
		{
			for (int c = 0; c < 3; c++)
			{
				const float* v = &cpu.verts[((size_t)s.firstVertex + cpu.indices[(size_t)s.firstIndex + i + c]) * kVertexFloats];
				out.xyz.insert(out.xyz.end(), v, v + 3);
			}
		}
	}
	return true;
}

int WorldBuildings::instanceFloor(uint32_t index) { return index < data_.instances.size() && data_.instances[index].parent >= 0 ? floorOfInstance(data_.instances[index]) : -1; }
int WorldBuildings::instanceShown(uint32_t index) const { return index < data_.instances.size() && data_.instances[index].parent >= 0 ? shownOf((uint32_t)data_.instances[index].parent) : 3; }

// dump floorables X Z R (kenshi-k16.2.355): the floorable buildings within R of the point, nearest first.
std::string WorldBuildings::floorablesReport(double x, double z, double radius) const
{
	std::vector<std::pair<double, uint32_t>> near;
	for (const uint32_t bi : topFloors_)
	{
		const BuildingInstance& b = data_.instances[bi];
		const double d = std::hypot(b.x - x, b.z - z);
		if (d <= radius) { near.push_back({ d, bi }); }
	}
	std::sort(near.begin(), near.end());
	std::string out = "floorables: " + std::to_string(near.size()) + " within " + std::to_string((int)radius) + " u\n";
	for (const auto& [d, bi] : near)
	{
		const BuildingInstance& b = data_.instances[bi];
		char line[256];
		snprintf(line, sizeof line, "floorable %s at %.0f %.0f %.0f maxFloor %d dist %.0f\n", data_.names[b.nameId].c_str(), b.x, b.y, b.z, (int)b.maxFloor, d);
		out += line;
	}
	return out;
}

// dump cuts (kenshi-k16.2.332.1): every building the floor cut slices, its shown / top floor, the selector level.
std::string WorldBuildings::cutsReport() const
{
	std::string out = "cuts: " + std::to_string(cuts_.size()) + " level " + std::to_string(visible_) + " occupants " + std::to_string(occupants.size()) + "\n";
	for (const uint32_t bi : cuts_)
	{
		char row[320];
		snprintf(row, sizeof row, "cut %u \"%s\" shown %d top %d\n", bi, nameOfInstance((int)bi).c_str(), shownOf(bi), data_.instances[bi].maxFloor);
		out += row;
	}
	return out;
}

// Floors above the shown one are hidden: a sliced building shows up to the visible floor, any other all of its floors (updateFocus).
int WorldBuildings::shownOf(uint32_t bi) const
{
	const int top = data_.instances[bi].maxFloor;
	return sliced((int)bi) ? std::min(visible_, top) : top;
}

// The floor a piece of furniture stands on: its state's `floor`, else the highest floor whose slab lies at or below its height in the building, one less
// for a ceiling piece (its origin is the mount on the ceiling, the next floor's slab: the Hub bar's ceiling lamps were hidden with the roof cut). Unknown
// (the building's meshes are not loaded yet) counts as above everything, so it is hidden rather than shown on the wrong floor.
int WorldBuildings::floorOfInstance(const BuildingInstance& f)
{
	const BuildingInstance& p = data_.instances[f.parent];
	if (f.floor >= 0 || p.maxFloor == 0) { return std::max<int>(f.floor, 0); }
	int8_t& cache = furnFloor_[&f - data_.instances.data()];
	if (cache == -2)
	{
		const BInfo* info = infoFor((uint32_t)f.parent);
		if (!info) { return 4; }
		const double at[3] = { f.x, f.y, f.z };
		double l[3];
		toLocal(p, at, l);
		cache = 0;
		for (int k = 1; k < 4; k++) { if (info->floorY[k] <= l[1]) { cache = (int8_t)k; } }
		if (f.ceiling && cache > 0) { cache--; }
	}
	return cache;
}

// ---- per frame ----

// Rotation function 3 spins at `rotation speed` x the wind's ramp (W - min) / (max - min), clamped to 0..1 (kinspect b50p_static.md Q11, the
// exe's factor; MINE: that the factor scales the drawn spin is untraced). W is the live wind at the camera (the exe reads each building's
// zone); unknown (an offline scene) or a part with no wind fields: full speed, the angle from the clock as before. The angle is integrated,
// so a wind change alters the rate, not the angle.
// A wind-aimed part (function 4, kinspect rotation_function4_wind_aim.md): a new wind starts a turn from where the part faces (at the base
// rate; the shader adds the instance's 0.95..1.05 factor) to the new wind. The first known wind is faced at once (the exe turns from the
// authored pose on load; we skip it).
void WorldBuildings::advanceMotion()
{
	const double dt = time_ - motionTime_;
	motionTime_ = time_;
	const bool step = dt > 0 && dt < 1;	// a clock jump (a load, a scene) restarts the integration
	for (uint32_t pid : turning_)
	{
		const BuildingPartDef& d = data_.parts[pid];
		Motion& mo = motion_[pid];
		if (spinning(d))
		{
			const double rate = d.rotSpeed * 3.14159265358979 / 180.0;
			if (windSpeed < 0 || (d.windMin == 0 && d.windMax == 0)) { mo.spin = rate * time_; }
			else if (step)
			{
				const double ramp = d.windMax > d.windMin ? std::clamp((windSpeed - d.windMin) / (d.windMax - d.windMin), 0.0f, 1.0f) : windSpeed >= d.windMax ? 1.0 : 0.0;
				mo.spin = std::fmod(mo.spin + rate * ramp * dt, 6.283185307179586);
			}
		}
		if (!d.aimed || wind[2] < 0.5f) { continue; }
		if (!mo.aimed || time_ < mo.since)
		{
			mo.from[0] = mo.to[0] = wind[0];
			mo.from[1] = mo.to[1] = wind[1];
			mo.since = time_;
			mo.aimed = true;
		}
		else if (std::fabs(wind[0] - mo.to[0]) + std::fabs(wind[1] - mo.to[1]) > 1e-4f)
		{
			float turn[6];
			turnOf(pid, turn);
			const double a0 = std::atan2(turn[0], turn[1]), pi = 3.14159265358979;
			const double delta = std::remainder(std::atan2(turn[2], turn[3]) - a0, 2 * pi);
			const double rate = std::fabs(d.aimSpeed) * pi / 180.0;
			const double dur = rate > 0 ? std::fabs(delta) / rate : 1.0;
			const double a = a0 + delta * (dur > 0 ? std::clamp(turn[4] / dur, 0.0, 1.0) : 1.0);
			mo.from[0] = (float)std::sin(a);
			mo.from[1] = (float)std::cos(a);
			mo.to[0] = wind[0];
			mo.to[1] = wind[1];
			mo.since = time_;
		}
	}
}

// The turn a wind-aimed part's uniforms carry (instVsParams): from x, z, to x, z, the seconds since it began, the rate (rad/s); null when it
// faces no wind yet.
const float* WorldBuildings::turnOf(uint32_t pid, float out[6]) const
{
	const Motion& mo = motion_[pid];
	if (!data_.parts[pid].aimed || !mo.aimed) { return nullptr; }
	out[0] = mo.from[0]; out[1] = mo.from[1]; out[2] = mo.to[0]; out[3] = mo.to[1];
	out[4] = (float)(time_ - mo.since);
	out[5] = (float)(std::fabs(data_.parts[pid].aimSpeed) * 3.14159265358979 / 180.0);
	return out;
}

void WorldBuildings::resetTurns()
{
	for (Motion& mo : motion_) { mo.aimed = false; }
}

void WorldBuildings::update(const double cam[3], const float mvp[16], float fogDist, bool blocking, double time, double shadowFar, FrameData& out)
{
	struct SigOut { const WorldBuildings& wb; FrameData& out; ~SigOut() { out.shadowSig = wb.shadowSig(); } } sigOut{ *this, out };	// on every return, after the last change
	out.inst.clear();
	out.lightCount = 0;
	frame_++;
	time_ = time;
	advanceMotion();
	for (int k = 0; k < 3; k++) { cam_[k] = cam[k]; }
	// the eye ring of the cut-away flip fade, in wall-clock seconds (a paused game still orbits; a blocking scripted run takes the scripted time, frame / 60,
	// so its shots are deterministic): the eye at each of 8 times back over kFlipFade
	{
		const double now = blocking ? time : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (!eyeHist_.empty())
		{
			const double* l = eyeHist_.back().p;
			if (std::fabs(cam[0] - l[0]) + std::fabs(cam[1] - l[1]) + std::fabs(cam[2] - l[2]) > 200.0) { eyeHist_.clear(); }	// a jump (another character, a teleport) is not an orbit
		}
		eyeHist_.push_back({ now, { cam[0], cam[1], cam[2] } });
		while (eyeHist_.size() > 2 && eyeHist_[1].t < now - kFlipFade - 0.05) { eyeHist_.pop_front(); }
		flipMoving_ = false;
		for (int k = 0; k < 8; k++)
		{
			const double t = now - (k + 1) * (double)kFlipFade / 8.0;
			size_t i = eyeHist_.size() - 1;
			while (i > 0 && eyeHist_[i - 1].t >= t) { i--; }
			double p[3];
			if (i == 0 || eyeHist_[i].t <= t) { std::copy(eyeHist_[i].p, eyeHist_[i].p + 3, p); }
			else
			{
				const EyeSample& a = eyeHist_[i - 1], &b = eyeHist_[i];
				const double u = (t - a.t) / (b.t - a.t);
				for (int c = 0; c < 3; c++) { p[c] = a.p[c] + (b.p[c] - a.p[c]) * u; }
			}
			for (int c = 0; c < 3; c++) { flipEye_[k][c] = (float)(p[c] - cam[c]); }
			flipMoving_ = flipMoving_ || std::fabs(flipEye_[k][0]) + std::fabs(flipEye_[k][1]) + std::fabs(flipEye_[k][2]) > 1e-3f;
		}
	}
	fog_ = fogDist;
	if (frame_ % 120 == 0) { evict(); }
	const Stats prev = stats_;
	stats_ = Stats();
	stats_.uploadedMeshes = prev.uploadedMeshes;
	stats_.uploadedTextures = prev.uploadedTextures;
	needed_.clear();
	if (data_.instances.empty()) { active_.clear(); fitGround_.clear(); fitRel_.clear(); idDraws_.clear(); return; }
	updateFocus();	// first: findOccluders skips the sliced
	findOccluders(cam);
	cursorBuildings_.clear();
	if (cursorOn_)
	{
		constexpr double kFar = 20000;	// MINE: as far as a peek reaches
		for (const uint32_t i : topFloors_)	// the static instances that are not a feature or furniture and have floors
		{
			const BuildingInstance& b = data_.instances[i];
			double enter, leave;
			if (std::abs(b.x - cam[0]) > kFar || std::abs(b.z - cam[2]) > kFar) { continue; }
			if (rayBox((int)i, cam, cursorDir_, kFar, enter, leave)) { cursorBuildings_.push_back(i); }
		}
	}

	// Cells within the load radius, nearest first.
	const kworld::ZoneCell cc = kworld::zoneCellOf(cam[0], cam[2]);
	// Past radius, out to the far distance: the far ring, exterior parts only (kBuildingFar).
	struct CellRef { double dist; const std::vector<uint32_t>* list; bool far; };
	std::vector<CellRef> cells;
	const double nearDist = radius * kCellSize;
	farDist_ = radius > 0 ? std::max(nearDist, std::min((double)fogDist, (double)kBuildingFar.get())) : 0.0;
	if (drawLimit > 0) { farDist_ = std::min(farDist_, drawLimit); }
	const int cellReach = (int)std::ceil(farDist_ / kCellSize);
	for (int dz = -cellReach - 1; dz <= cellReach + 1; dz++)
	{
		for (int dx = -cellReach - 1; dx <= cellReach + 1; dx++)
		{
			const kworld::ZoneCell c = { cc.x + dx, cc.z + dz };
			if (c.x < 0 || c.z < 0 || c.x >= kworld::kZoneCellsPerAxis || c.z >= kworld::kZoneCellsPerAxis) { continue; }
			const auto it = data_.cells.find(kworld::zoneIndexOf(c));
			if (it == data_.cells.end()) { continue; }
			const double ex = std::max({ kworld::zoneMinX(c.x) - cam[0], cam[0] - kworld::zoneMinX(c.x + 1), 0.0 });
			const double ez = std::max({ kworld::zoneMinZ(c.z) - cam[2], cam[2] - kworld::zoneMinZ(c.z + 1), 0.0 });
			const double d = std::hypot(ex, ez);
			if (d <= farDist_) { cells.push_back({ d, &it->second, d > nearDist }); }
		}
	}
	// Furniture cells are not bound by the zone radius: any cell within the interior radius (plus the size of a building) is listed.
	for (const auto& [index, list] : data_.interiorCells)
	{
		const kworld::ZoneCell c = kworld::zoneCellOfIndex(index);
		const double ex = std::max({ kworld::zoneMinX(c.x) - cam[0], cam[0] - kworld::zoneMinX(c.x + 1), 0.0 });
		const double ez = std::max({ kworld::zoneMinZ(c.z) - cam[2], cam[2] - kworld::zoneMinZ(c.z + 1), 0.0 });
		if (std::hypot(ex, ez) <= interiorRadius + kInteriorSlack) { cells.push_back({ std::hypot(ex, ez), &list, false }); }
	}
	if (!propList_.empty()) { cells.push_back({ 0.0, &propList_, false }); }
	std::sort(cells.begin(), cells.end(), [](const CellRef& a, const CellRef& b) { return a.dist < b.dist; });
	// Interior parts of a building and its furniture are wanted only near the camera (origin distance; the slack covers the building's extent).
	const auto interiorNear = [&](const BuildingInstance& b)
	{
		if (!interiors) { return false; }
		const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], r = interiorRadius + kInteriorSlack;
		return dx * dx + dy * dy + dz * dz <= r * r;
	};
	hulls_.clear();
	for (const CellRef& c : cells)
	{
		for (const uint32_t ii : *c.list)
		{
			const BuildingInstance& b = data_.instances[ii];
			const bool near = !c.far && interiorNear(b);
			for (uint32_t k = 0; k < b.partCount; k++)
			{
				const uint32_t pid = data_.partRefs[b.firstPart + k];
				if (data_.parts[pid].kind != 0 && !near) { continue; }
				if (data_.parts[pid].kind == 2) { hulls_.push_back({ ii, pid }); }
				if (partStamp_[pid] != frame_) { partStamp_[pid] = (uint32_t)frame_; needed_.push_back(pid); }
			}
		}
	}
	for (;;)
	{
		for (const uint32_t pid : needed_) { ensurePart(pid); }
		if (!blocking) { drain(false, kUploadsPerFrame); break; }
		if (outstanding_ == 0) { break; }
		drain(true, -1);
	}
	stats_.pending = (size_t)outstanding_;	// decodes, and the creates queued in the stream lane that promote() has not seen run
	for (const auto& [isMesh, id] : uploading_) { stats_.pending += (isMesh ? meshes_[id].state : textures_[id].state) == Uploading; }

	// Dirty tracking (kenshi-k16.2.205.11): the cull / pack pass below is redone only when what it reads moved. Streaming above always runs
	// (meshes load and evict in drain), so a paused or still view keeps last frame's groups, instances, mask and placed effects; the instance
	// buffer still gets its per-frame write (a write_transient buffer must be written in the frame it is drawn) and the lights' phase is
	// refreshed (lightMinutes moves with the clock, and is not a pack input for that reason).
	const uint64_t sig = packSignature(cam, mvp, fogDist, shadowFar);
	if (packValid_ && sig == packSig_)
	{
		const Stats cur = stats_;
		stats_ = prev;
		stats_.uploadedMeshes = cur.uploadedMeshes;
		stats_.uploadedTextures = cur.uploadedTextures;
		stats_.pending = cur.pending;
		pulseLights();
		packLights(out);
		out.inst.assign(frameData_.begin(), frameData_.end());	// upload() writes it into the transient buffer, which must be written each frame it is drawn
		return;
	}
	packSig_ = sig;
	packValid_ = true;
	active_.clear();
	fitGround_.clear();
	fitRel_.clear();
	idDraws_.clear();

	// Frustum side planes (Gribb-Hartmann), normalised.
	float planes[4][4];
	for (int k = 0; k < 4; k++)
	{
		const int axis = k >> 1;
		const float s = (k & 1) ? -1.0f : 1.0f;
		for (int c = 0; c < 4; c++) { planes[k][c] = mvp[c * 4 + 3] + s * mvp[c * 4 + axis]; }
		const float l = std::sqrt(planes[k][0] * planes[k][0] + planes[k][1] * planes[k][1] + planes[k][2] * planes[k][2]);
		for (int c = 0; c < 4; c++) { planes[k][c] /= l; }
	}
	const double maxDist = drawLimit > 0 ? drawLimit : std::min((double)fogDist, nearDist);	// lights and interior parts; exterior parts: farDist_

	for (Group& g : groups_) { g.onlyShadow.clear(); g.inRange.clear(); g.outRange.clear(); g.glass.clear(); g.flip.clear(); }
	lightCands_.clear();
	placed_.clear();
	size_t total = 0;
	for (const CellRef& c : cells)
	{
		for (const uint32_t ii : *c.list)
		{
			// a door leaf draws at its door's pose: a moved copy (floorOfInstance indexes by address, but a leaf has floor 0 and never gets there)
			BuildingInstance moved;
			const BuildingInstance* src = &data_.instances[ii];
			if (src->door)
			{
				if (const auto off = doorOffsets_.find(src->door); off != doorOffsets_.end())
				{
					moved = *src;
					moved.x += off->second[0]; moved.y += off->second[1]; moved.z += off->second[2];
					src = &moved;
				}
			}
			const BuildingInstance& b = *src;
			const float q[4] = { b.qw, b.qx, b.qy, b.qz };
			const bool furniture = b.parent >= 0, near = !c.far && interiorNear(b);
			// Floors above the shown one are hidden: a building's parts by their own floor, furniture by its floor (a building without floors shows all)
			const int shown = furniture ? shownOf((uint32_t)b.parent) : b.maxFloor > 0 ? shownOf(ii) : 3;
			const int furnitureFloor = furniture && shown < data_.instances[b.parent].maxFloor ? floorOfInstance(b) : 0;
			// A prop (a sim item / container) has no floor of its own: one on a floor the cut hides goes with the building's own parts there, shadow and
			// light too (user playtest 2026-11-06: the roof deck's barrels and water storage in the floor 0 view). One on a stair counts by the floor the stair
			// starts on, as a character does (Prop::cut; playtest nitpick: a container on a stair culled by its height).
			// Over a peeked building's floor it turns to glass with that building's parts (playtest nitpick: a prop over a PEEK stayed solid).
			float propPeek = 0;
			if (b.prop)
			{
				const double at[3] = { b.x, b.y, b.z };
				if (b.propCut) { continue; }
				for (const auto& [bi, peek] : peeks) { if (peek.fade > propPeek && aboveSlab(bi, peek.floor, at, at[1])) { propPeek = peek.fade; } }
			}
			// under construction: the stretch w slot (the biome row, only read by TERRAIN-mode features) carries -(H + progress), H = whole building height >= 1 (mesh.glsl inst_vs)
			const auto cons = construction_.find(furniture ? (uint32_t)b.parent : ii);
			if (cons != construction_.end() && furniture) { continue; }
			const uint32_t owner = furniture ? (uint32_t)b.parent : ii;
			// EFFECTS on the building record itself (Campfire / Cooking Pit, the four MAP_FEATURES systems): building-local, no part frame.
			for (uint32_t e = 0; e < b.effectCount && !c.far; e++) { addEffect(b, ii, -1, nullptr, b.firstEffect + e, cam, planes); }
			// LIGHT instances on the building record (the campfire and Holy torch lights): same night rule as the outdoor part lights. The rule (kenshi-inspect
			// world_particles.md §8-10): a standalone placement (kind 11) lights outside [sunrise, sunset); furniture on a structure's hand always, once the
			// structure is complete. The original keeps a structure's furniture dark until its interior first streams in (+0x19d): a streaming artefact, not copied.
			if ((furniture || night) && !c.far) { for (uint32_t k = 0; k < b.ownLightCount; k++) { addOwnLight(b, ii, b.firstOwnLight + k, cam, planes, maxDist); } }
			const float build = cons == construction_.end() ? 0.0f : -(std::max(1.0f, std::round(b.height)) + std::clamp(cons->second, 0.0f, 0.999f));
			// glass at glassAlpha: a ghosted occluder (ghosts_) all of it, else a peeked one (peeks) the parts above its floor while it is not sliced
			const auto gh = build == 0.0f ? ghosts_.find(owner) : ghosts_.end();
			const auto pk = build == 0.0f && gh == ghosts_.end() && shown >= data_.instances[owner].maxFloor ? peeks.find(owner) : peeks.end();
			const bool peeked = pk != peeks.end() && pk->second.fade > 0, glassy = peeked || propPeek > 0 || (gh != ghosts_.end() && gh->second > 0);
			const float fade = peeked ? pk->second.fade : propPeek > 0 ? propPeek : glassy ? gh->second : 0.0f;
			const float glassAlpha = 1.0f - (1.0f - kGlassAlpha) * std::clamp(fade, 0.0f, 1.0f);
			const int peekFloor = peeked ? pk->second.floor : glassy ? -1 : 3;
			const int instFloor = glassy && furniture ? floorOfInstance(b) : 0;
			const bool cursorOwner = std::find(cursorBuildings_.begin(), cursorBuildings_.end(), owner) != cursorBuildings_.end();
			bool any = false, anyOpen = false;
			float lowest = 1e30f;
			if (kOpenWindows.get()) { for (uint32_t k = 0; k < b.partCount; k++) { if (partRt_[data_.partRefs[b.firstPart + k]].openHas) { anyOpen = true; break; } } }
			for (uint32_t k = 0; k < b.partCount; k++)
			{
				const uint32_t pid = data_.partRefs[b.firstPart + k];
				const PartRt& rt = partRt_[pid];
				const BuildingPartDef& def = data_.parts[pid];
				if (def.kind == 2 || (def.kind != 0 && !near)) { continue; }
				if (def.sign && !furniture && shown < b.maxFloor && !kSignsIndoors.get()) { continue; }
				if (rt.openHide && anyOpen) { continue; }	// the black cavity lining behind an opening this switch opened (render.open_windows; draw and shadow)
				// a part the floor cut hides is not drawn, but keeps casting its shadow (not the cut floor's black cap, not its lights / effects)
				const bool cutHidden = (furniture ? furnitureFloor : def.floor) > shown && !(def.sign && kSignsIndoors.get());	// a sign on a hidden floor still draws when the option is on (the original's signs ignore the cut)
				if (cutHidden && (def.blackCap || shadowFar <= 0)) { continue; }
				if (def.blackCap && def.floor == shown && shown < b.maxFloor) { continue; }	// the cut floor's black cap (BuildingPartDef::blackCap)
				if (!cutHidden && (furniture || def.kind == 1 || night) && !c.far) { for (uint32_t k = 0; k < def.lightCount; k++) { addLight(b, ii, def, def.firstLight + k, cam, planes, maxDist); } }
				for (uint32_t k = 0; k < def.effectCount && !c.far && !cutHidden; k++) { addEffect(b, ii, (int32_t)pid, &def, def.firstEffect + k, cam, planes); }
				if (!partTexturesReady(pid)) { continue; }	// mesh ready, a texture it draws still streaming: held out of the draw list, never a grey unlit frame (kenshi-k16.2.298.1)
				const MeshEntry& m = meshes_[rt.mesh];
				double off[3];
				toMesh(offsetFrame, def.offset, off);
				const bool spins = spinning(def) || def.aimed;	// turns about its origin: bounds about the origin
				const double lc[3] = { ((spins ? 0 : m.center[0]) + off[0]) * b.scale * b.stretch[0], ((spins ? 0 : m.center[1]) + off[1]) * b.scale * b.stretch[1],
					((spins ? 0 : m.center[2]) + off[2]) * b.scale * b.stretch[2] };
				double rc[3];
				rotate(q, lc, rc);
				const double rel[3] = { b.x - cam[0] + rc[0], b.y - cam[1] + rc[1], b.z - cam[2] + rc[2] };
				const double cr = spins ? std::sqrt((double)m.center[0] * m.center[0] + (double)m.center[1] * m.center[1] + (double)m.center[2] * m.center[2]) : 0.0;
				const double r = ((double)m.radius + cr) * b.scale, dist = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
				bool vis = !cutHidden && !(dist - r > (def.kind != 0 ? std::min(maxDist, interiorRadius) : farDist_) || (dist > r && r / dist < 1.0 / 400.0));
				for (int p = 0; p < 4 && vis; p++) { vis = planes[p][0] * rel[0] + planes[p][1] * rel[1] + planes[p][2] * rel[2] + planes[p][3] >= -r; }
				const bool casts = shadowFar > 0 && dist - r <= shadowFar && (vis || r >= kMinCasterRadius);
				if (!vis && !casts) { continue; }
				if (vis)
				{
					any = true;
					lowest = std::min(lowest, m.bmin[1] * b.scale);
				}
				for (size_t s = 0; s < m.subs.size(); s++)
				{
					// A tinted severed limb gets a group of its own (one per instance): its skin tone and blood colour come from the wire, per item. The
					// key sets bit 63 and carries the instance index; every other part keys by (part, submesh) and batches.
					const bool limb = b.limb && def.limb;
					const uint64_t key = limb ? (0x8000000000000000ull | ((uint64_t)ii << 8) | (uint64_t)(s & 0xff)) : (((uint64_t)pid << 16) | s);
					auto [it, fresh] = groupIds_.emplace(key, (int)groups_.size());
					if (fresh) { groups_.push_back({ pid, (int)s, {}, {}, {}, {}, {}, 0 }); }
					Group& g = groups_[it->second];
					if (limb)
					{
						g.limb = true;
						std::copy(b.limbSkin, b.limbSkin + 3, g.skin);
						std::copy(b.limbBlood, b.limbBlood + 3, g.blood);
					}
					const float inst[kInstanceFloats] = { (float)(b.x - cam[0]), (float)(b.y - cam[1]), (float)(b.z - cam[2]), b.scale, b.qx, b.qy, b.qz, b.qw,
						b.stretch[0], b.stretch[1], b.stretch[2], def.featureMode == 2 && detail ? detail->rowAt(b.x, b.z) : build };
					if (total >= kMaxInstances) { continue; }
					if ((furniture ? instFloor : def.floor) > peekFloor || (peeked && def.blackCap && def.floor == peekFloor))	// the cut floor's cap too
					{
						if (casts) { g.onlyShadow.insert(g.onlyShadow.end(), inst, inst + kInstanceFloats); }	// the glass keeps its shadow: it does not change with the peek (user 2026-11-05)
						if (!vis) { continue; }
						if (def.blackCap) { continue; }	// not drawn as glass: a near-opaque black layer over the stairwells early in the fade (user 2026-11-04)
						if (cursorOwner) { idDraws_.push_back({ it->second, 2, (uint32_t)(g.glass.size() / kInstanceFloats), owner }); }
						g.glass.insert(g.glass.end(), inst, inst + kInstanceFloats);
						g.glass.back() = -(kGlassBuild + glassAlpha * 1000.0f);
						total++;
						stats_.submeshDraws++;
						continue;
					}
					std::vector<float>& dst = !vis ? g.onlyShadow : casts ? g.inRange : g.outRange;
					if (cursorOwner && vis) { idDraws_.push_back({ it->second, (uint8_t)(casts ? 0 : 1), (uint32_t)(dst.size() / kInstanceFloats), owner }); }
					dst.insert(dst.end(), inst, inst + kInstanceFloats);
					total++;
					if (vis && flipMoving_ && !furniture && build == 0.0f && !glassy && shown < b.maxFloor && !cutHidden && cullsBackFaces(def) && !def.blackCap && !def.sign && total < kMaxInstances)
					{
						g.flip.insert(g.flip.end(), inst, inst + kInstanceFloats);	// the cut-away flip fade draws its back faces too (renderFlip)
						total++;
					}
					if (!vis) { continue; }
					stats_.submeshDraws++;
					stats_.realTextured += rt.subTex[s] >= 0 && textures_[rt.subTex[s]].state == Ready;
				}
			}
			if (any && furniture) { stats_.interiorsDrawn++; }
			else if (any)
			{
				stats_.instancesDrawn++;
				stats_.featuresDrawn += b.feature;
				fitGround_.push_back((float)(b.y - b.ground) + lowest);
				fitRel_.push_back((float)(b.y - b.ground));
			}
		}
	}
	pulseLights();
	// The interior mask (renderMask): the `interior mask` hulls (kind 2) of the buildings near the camera, while render.interior_mask is on (the terrain, grass and particle clips).
	// Same instance layout as the drawn parts; collected after them so their block does not disturb the groups' offsets. Mask parts load with the
	// interiors, so --no-interiors leaves the weather unclipped.
	maskGroups_.clear();
	if (maskWanted && interiors)
	{
		for (const CellRef& c : cells)
		{
			for (const uint32_t ii : *c.list)
			{
				const BuildingInstance& b = data_.instances[ii];
				if (b.parent >= 0 || !interiorNear(b)) { continue; }
				const float q[4] = { b.qw, b.qx, b.qy, b.qz };
				for (uint32_t k = 0; k < b.partCount; k++)
				{
					const uint32_t pid = data_.partRefs[b.firstPart + k];
					const PartRt& rt = partRt_[pid];
					const BuildingPartDef& def = data_.parts[pid];
					if (def.kind != 2 || rt.mesh < 0 || meshes_[rt.mesh].state != Ready) { continue; }
					const MeshEntry& m = meshes_[rt.mesh];
					double off[3];
					toMesh(offsetFrame, def.offset, off);
					const bool spins = spinning(def) || def.aimed;	// turns about its origin: bounds about the origin
					const double lc[3] = { ((spins ? 0 : m.center[0]) + off[0]) * b.scale * b.stretch[0], ((spins ? 0 : m.center[1]) + off[1]) * b.scale * b.stretch[1],
						((spins ? 0 : m.center[2]) + off[2]) * b.scale * b.stretch[2] };
					double rc[3];
					rotate(q, lc, rc);
					const double rel[3] = { b.x - cam[0] + rc[0], b.y - cam[1] + rc[1], b.z - cam[2] + rc[2] };
					const double cr = spins ? std::sqrt((double)m.center[0] * m.center[0] + (double)m.center[1] * m.center[1] + (double)m.center[2] * m.center[2]) : 0.0;
					const double r = ((double)m.radius + cr) * b.scale, dist = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
					bool vis = !(dist - r > interiorRadius || (dist > r && r / dist < 1.0 / 400.0));
					for (int p = 0; p < 4 && vis; p++) { vis = planes[p][0] * rel[0] + planes[p][1] * rel[1] + planes[p][2] * rel[2] + planes[p][3] >= -r; }
					if (!vis) { continue; }
					const float inst[kInstanceFloats] = { (float)(b.x - cam[0]), (float)(b.y - cam[1]), (float)(b.z - cam[2]), b.scale, b.qx, b.qy, b.qz, b.qw,
						b.stretch[0], b.stretch[1], b.stretch[2], 0.0f };
					for (size_t s = 0; s < m.subs.size(); s++)
					{
						MaskGroup* g = nullptr;
						for (MaskGroup& cand : maskGroups_) { if (cand.part == pid && cand.sub == (int)s) { g = &cand; break; } }
						if (!g) { maskGroups_.push_back({ pid, (int)s, 0, {} }); g = &maskGroups_.back(); }
						g->inst.insert(g->inst.end(), inst, inst + kInstanceFloats);
					}
				}
			}
		}
	}
	frameData_.clear();
	for (size_t i = 0; i < groups_.size(); i++)
	{
		Group& g = groups_[i];
		if (g.onlyShadow.empty() && g.inRange.empty() && g.outRange.empty() && g.glass.empty() && g.flip.empty()) { continue; }
		g.first = (int)(frameData_.size() / kInstanceFloats);
		frameData_.insert(frameData_.end(), g.onlyShadow.begin(), g.onlyShadow.end());
		frameData_.insert(frameData_.end(), g.inRange.begin(), g.inRange.end());
		frameData_.insert(frameData_.end(), g.outRange.begin(), g.outRange.end());
		frameData_.insert(frameData_.end(), g.glass.begin(), g.glass.end());
		frameData_.insert(frameData_.end(), g.flip.begin(), g.flip.end());
		active_.push_back((int)i);
	}
	for (MaskGroup& g : maskGroups_)
	{
		if (frameData_.size() / kInstanceFloats + g.inst.size() / kInstanceFloats > kMaxInstances) { break; }
		g.first = (int)(frameData_.size() / kInstanceFloats);
		frameData_.insert(frameData_.end(), g.inst.begin(), g.inst.end());
	}
	for (const int gi : active_) { stats_.groups += !groups_[gi].inRange.empty() || !groups_[gi].outRange.empty(); }
	for (const MeshEntry& m : meshes_) { stats_.meshesReady += m.state == Ready; stats_.meshesFailed += m.state == Failed; }
	for (const TexEntry& t : textures_) { stats_.texturesReady += t.state == Ready; stats_.texturesFailed += t.state == Failed; }
	packLights(out);
	if (lampOcclusion) { kickLamps(cam); }
	out.inst.assign(frameData_.begin(), frameData_.end());
}

// The top of render_frame (render hook 7a pattern): the packet's instances into the transient buffer, on every draw of the packet.
void WorldBuildings::upload(const FrameData& d)
{
	if (d.inst.empty()) { return; }
	sg_write_buffer_desc wd = {};
	wd.src.data = { d.inst.data(), d.inst.size() * sizeof(float) };
	wd.dst.buffer = instBuf_;
	sg_write_buffer_transient(&wd);
}

// Everything the cull / pack pass reads, hashed after the streaming pass: the camera (the pass is camera-relative: an identical frame is the same
// everything), the view matrix, the draw distances, the render flags, the floor / peek / ghost / construction state and the data and load-state
// generations. lightMinutes is deliberately out (pulseLights refreshes the lights every frame); animating parts turn in the shader (turnOf), so
// their motion is not an input here either.
uint64_t WorldBuildings::packSignature(const double cam[3], const float mvp[16], float fog, double shadowFar) const
{
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
	auto db = [](double x) { uint64_t u; std::memcpy(&u, &x, sizeof u); return u; };
	auto fb = [](float x) { uint32_t u; std::memcpy(&u, &x, sizeof u); return (uint64_t)u; };
	for (int k = 0; k < 3; k++) { mix(db(cam[k])); }
	for (int k = 0; k < 16; k++) { mix(fb(mvp[k])); }
	mix(fb(fog));
	mix(db(shadowFar));
	mix(db(farDist_));
	mix(db(interiorRadius));
	mix(fb((float)drawLimit));
	mix((uint64_t)(night ? 1u : 0u) | (uint64_t)(interiors ? 2u : 0u) | (uint64_t)(maskWanted ? 4u : 0u) | (uint64_t)(cursorOn_ ? 8u : 0u)
		| (uint64_t)(hideMarkers ? 16u : 0u) | (uint64_t)((radius & 0xffff) << 8));
	mix((uint64_t)(uintptr_t)detail);
	mix(lampOcclusion ? 1u : 0u);	// render.lamp_occlusion: the candidates carry (or not) the lamp registration, so a switch change repacks
	mix((uint64_t)offsetFrame);
	mix((uint64_t)visible_);
	for (int k = 0; k < 3; k++) { mix(db(cursorDir_[k])); }
	mix(cuts_.size());
	for (const uint32_t bi : cuts_) { mix(bi); }
	mix(ghosts_.size());
	for (const auto& [bi, fade] : ghosts_) { mix(bi); mix(fb(fade)); }
	mix(construction_.size());
	for (const auto& [bi, progress] : construction_) { mix(bi); mix(fb(progress)); }
	mix(peeks.size());
	for (const auto& [bi, pk] : peeks) { mix(bi); mix((uint64_t)pk.floor); mix(fb(pk.fade)); mix((uint64_t)(pk.on ? 1u : 0u)); }
	mix(dataGen_);
	mix(stateGen_);
	return h;
}

uint32_t WorldBuildings::addPart(const BuildingPartDef& def)
{
	dataGen_++;
	data_.parts.push_back(def);
	partRt_.emplace_back();
	partStamp_.push_back(0);
	motion_.emplace_back();	// per part, like partRt_ (setData sizes it): turnOf / spin read motion_[pid] for every drawn part
	return (uint32_t)data_.parts.size() - 1;
}

// One light of a drawn part: its sphere culled against the view, kept as a candidate (LightCand) with the power rolled once per instance
// (brightness + uniform(-variance, variance)); pulseLights applies the effect (PULSE 0.5 + 0.5 sin(4 t), off at <= 0; SHIMMER 0.95 + 0.05 sin(30 t)
// sin(17 t); t in game minutes) every frame, since it moves with the clock while the cull does not. The radius is the record's, unscaled.
void WorldBuildings::addLight(const BuildingInstance& b, uint32_t ii, const BuildingPartDef& def, uint32_t li, const double cam[3], const float planes[4][4], double maxDist)
{
	const LightDef& l = data_.lights[li];
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	const float lp[3] = { def.offset[0] + l.pos[0], def.offset[1] + l.pos[1], def.offset[2] + l.pos[2] };
	double m[3], r[3];
	toMesh(offsetFrame, lp, m);
	for (int k = 0; k < 3; k++) { m[k] *= b.scale * b.stretch[k]; }
	rotate(q, m, r);
	const double rel[3] = { b.x - cam[0] + r[0], b.y - cam[1] + r[1], b.z - cam[2] + r[2] };
	const double dist = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
	if (dist - l.radius > maxDist) { return; }
	for (int p = 0; p < 4; p++) { if (planes[p][0] * rel[0] + planes[p][1] * rel[1] + planes[p][2] * rel[2] + planes[p][3] < -l.radius) { return; } }
	uint32_t h = ii * 2654435761u ^ li * 2246822519u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	double d[3], rd[3];
	toMesh(offsetFrame, l.dir, d);
	rotate(q, d, rd);
	LightCand c;
	c.rel[0] = (float)rel[0]; c.rel[1] = (float)rel[1]; c.rel[2] = (float)rel[2]; c.rel[3] = l.radius;
	c.colour[0] = l.colour[0]; c.colour[1] = l.colour[1]; c.colour[2] = l.colour[2];
	c.base = l.brightness + l.variance * ((h & 0xffffff) / 8388608.0f - 1.0f);
	c.spot = l.spot ? 1.0f : 0.0f;
	c.dir[0] = (float)rd[0]; c.dir[1] = (float)rd[1]; c.dir[2] = (float)rd[2]; c.dir[3] = 0;
	c.cone[0] = l.cosInner; c.cone[1] = l.cosOuter; c.cone[2] = l.falloff; c.cone[3] = 0;
	c.near = (float)std::max(0.0, dist - l.radius);
	c.effect = l.effect;
	c.id = ((uint64_t)ii << 32) | li;
	if (lampOcclusion)
	{
		const double world[3] = { b.x + r[0], b.y + r[1], b.z + r[2] };
		registerLamp(b, ii, ((uint64_t)ii << 33) | li, world, c);
	}
	lightCands_.push_back(c);
}

// One LIGHT instance of the building record itself (no part frame). addLight without the part offset; the instance's own rotation is already
// in LightDef::dir.
void WorldBuildings::addOwnLight(const BuildingInstance& b, uint32_t ii, uint32_t li, const double cam[3], const float planes[4][4], double maxDist)
{
	const LightDef& l = data_.ownLights[li];
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	double m[3], r[3];
	toMesh(offsetFrame, l.pos, m);
	for (int k = 0; k < 3; k++) { m[k] *= b.scale * b.stretch[k]; }
	rotate(q, m, r);
	const double rel[3] = { b.x - cam[0] + r[0], b.y - cam[1] + r[1], b.z - cam[2] + r[2] };
	const double dist = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
	if (dist - l.radius > maxDist) { return; }
	for (int p = 0; p < 4; p++) { if (planes[p][0] * rel[0] + planes[p][1] * rel[1] + planes[p][2] * rel[2] + planes[p][3] < -l.radius) { return; } }
	uint32_t h = ii * 2654435761u ^ li * 2246822519u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	double d[3], rd[3];
	toMesh(offsetFrame, l.dir, d);
	rotate(q, d, rd);
	LightCand c;
	c.rel[0] = (float)rel[0]; c.rel[1] = (float)rel[1]; c.rel[2] = (float)rel[2]; c.rel[3] = l.radius;
	c.colour[0] = l.colour[0]; c.colour[1] = l.colour[1]; c.colour[2] = l.colour[2];
	c.base = l.brightness + l.variance * ((h & 0xffffff) / 8388608.0f - 1.0f);
	c.spot = l.spot ? 1.0f : 0.0f;
	c.dir[0] = (float)rd[0]; c.dir[1] = (float)rd[1]; c.dir[2] = (float)rd[2]; c.dir[3] = 0;
	c.cone[0] = l.cosInner; c.cone[1] = l.cosOuter; c.cone[2] = l.falloff; c.cone[3] = 0;
	c.near = (float)std::max(0.0, dist - l.radius);
	c.effect = l.effect;
	c.id = ((uint64_t)ii << 32) | 0x80000000u | li;
	if (lampOcclusion)
	{
		const double world[3] = { b.x + r[0], b.y + r[1], b.z + r[2] };
		registerLamp(b, ii, ((uint64_t)ii << 33) | (1ull << 32) | li, world, c);
	}
	lightCands_.push_back(c);
}

// The light list from this frame's candidates, with the PULSE / SHIMMER phase applied: every frame, also on a skipped pack (lightMinutes moves).
void WorldBuildings::pulseLights()
{
	const double t = lightMinutes;
	lights_.clear();
	lights_.reserve(lightCands_.size());
	for (const LightCand& c : lightCands_)
	{
		float power = c.base;
		if (c.effect == 1) { power *= (float)(0.5 + 0.5 * std::sin(4 * t)); }
		else if (c.effect == 3) { power *= (float)(0.95 + 0.05 * std::sin(30 * t) * std::sin(17 * t)); }
		if (power <= 0) { continue; }
		lights_.push_back({ { c.rel[0], c.rel[1], c.rel[2], c.rel[3] }, { c.colour[0] * power, c.colour[1] * power, c.colour[2] * power, c.spot },
			{ c.dir[0], c.dir[1], c.dir[2], c.dir[3] }, { c.cone[0], c.cone[1], c.cone[2], c.cone[3] }, c.near, c.id, c.owner, c.lamp });
	}
}

// One EFFECT instance of a drawn part (def != null: its offset is added) or of the building record itself (def null: building-local). The
// same transform chain as addLight: part offset, authoring frame -> mesh frame, instance scale/stretch, instance rotation, camera relative.
// The effect's node is a child of the part's (or building's) node in the original, so its orientation is the instance rotation times the
// EFFECT instance's own rotation: the script's emitter offset and `direction` turn with the post (kenshi-k16.2.208.5: a world-up offset put
// the torch flame beside the tip of a tilted post). The instance's own rotation does not move the position.
void WorldBuildings::addEffect(const BuildingInstance& b, uint32_t ii, int32_t part, const BuildingPartDef* def, uint32_t ei,
	const double cam[3], const float planes[4][4])
{
	const EffectDef& e = data_.effects[ei];
	float lp[3] = { e.pos[0], e.pos[1], e.pos[2] };
	if (def) { for (int k = 0; k < 3; k++) { lp[k] += def->offset[k]; } }
	double m[3], r[3];
	toMesh(offsetFrame, lp, m);
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	for (int k = 0; k < 3; k++) { m[k] *= b.scale * b.stretch[k]; }
	rotate(q, m, r);
	const double rel[3] = { b.x - cam[0] + r[0], b.y - cam[1] + r[1], b.z - cam[2] + r[2] };
	PlacedEffectRt out;
	out.def = ei;
	out.instance = ii;
	out.part = part;
	out.floor = def ? def->floor : 0;
	out.distance = (float)std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
	// No distance test: a building-attached fire has no view range in the original (`maximum view distance` 0 on every fire EFFECT, and none
	// vanished at any reachable distance: kenshi-inspect world_particles.md 2 Live). The part's own cell loading bounds what can exist.
	// A margin wider than the view keeps a burning system alive while it is just off screen (the driver also holds a grace time), so no
	// rebuild happens when the camera turns back: no pop-in.
	for (int p = 0; p < 4; p++) { if (planes[p][0] * rel[0] + planes[p][1] * rel[1] + planes[p][2] * rel[2] + planes[p][3] < -kEffectMargin) { return; } }
	const double base[3] = { b.x, b.y, b.z };
	for (int k = 0; k < 3; k++) { out.rel[k] = (float)rel[k]; out.world[k] = base[k] + r[k]; }
	// The effect node's world orientation: the instance's rotation, then its own (child * own), w x y z.
	const float* eq = e.quat;
	out.quat[0] = q[0] * eq[0] - q[1] * eq[1] - q[2] * eq[2] - q[3] * eq[3];
	out.quat[1] = q[0] * eq[1] + q[1] * eq[0] + q[2] * eq[3] - q[3] * eq[2];
	out.quat[2] = q[0] * eq[2] - q[1] * eq[3] + q[2] * eq[0] + q[3] * eq[1];
	out.quat[3] = q[0] * eq[3] + q[1] * eq[2] - q[2] * eq[1] + q[3] * eq[0];
	out.scale = b.scale;
	uint32_t h = ii * 2654435761u ^ ei * 2246822519u;
	h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
	out.seed = h;
	placed_.push_back(out);
}

std::vector<std::string> WorldBuildings::effectScripts() const
{
	std::vector<std::string> out;
	for (const EffectDef& e : data_.effects) { out.push_back(e.script); }
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

// The lamp of a light: its slot among the lamps of the building instance that owns it (a furniture instance belongs to its parent). Only buildings with interiors have a volume; the
// lamp's position goes into the owner's frame (the probe grid's: mesh units before the instance scale). A lamp past kMaxLamps stays without a slot (unoccluded).
void WorldBuildings::registerLamp(const BuildingInstance& b, uint32_t ii, uint64_t key, const double world[3], LightCand& out)
{
	const uint32_t owner = b.parent >= 0 ? (uint32_t)b.parent : ii;
	if (owner >= data_.instances.size()) { return; }
	const BuildingInstance& o = data_.instances[owner];
	if (o.parent >= 0 || o.feature || o.prop || o.maxFloor == 0) { return; }
	LampReg& reg = lampReg_[owner];
	size_t slot = 0;
	while (slot < reg.keys.size() && reg.keys[slot] != key) { slot++; }
	if (slot == reg.keys.size())
	{
		if (slot >= (size_t)lightbake::kMaxLamps) { return; }
		// world -> the owner's local frame: the inverse of origin + R(q) (local x scale x stretch)
		const float qi[4] = { o.qw, -o.qx, -o.qy, -o.qz };
		const double d[3] = { world[0] - o.x, world[1] - o.y, world[2] - o.z };
		double l[3];
		rotate(qi, d, l);
		reg.keys.push_back(key);
		reg.local.push_back({ (float)(l[0] / (o.scale * o.stretch[0])), (float)(l[1] / (o.scale * o.stretch[1])), (float)(l[2] / (o.scale * o.stretch[2])) });
		reg.version++;
	}
	out.owner = owner;
	out.lamp = (int)slot;
}

void WorldBuildings::packLights(FrameData& out) const
{
	std::vector<const LightRt*> order;
	for (const LightRt& l : lights_) { order.push_back(&l); }
	std::sort(order.begin(), order.end(), [](const LightRt* a, const LightRt* b) { return a->near < b->near; });
	{	// the light debugger: the lamps in view (nearest 64) by id, and the switched-off ones leave the list (dev/lightterms.hpp; no-ops in a release build)
		std::vector<uint64_t> ids;
		for (size_t i = 0; i < order.size() && i < 64; i++) { ids.push_back(order[i]->id); }
		dev::noteLamps(ids);
		order.erase(std::remove_if(order.begin(), order.end(), [](const LightRt* l) { return dev::lampOff(l->id); }), order.end());
	}
	const size_t n = std::min(order.size(), (size_t)LightParams::kMaxPoints);
	out.lightCount = (int)n;
	for (size_t i = 0; i < n; i++)
	{
		std::copy(order[i]->rel, order[i]->rel + 4, out.lights[4 * i]);
		std::copy(order[i]->colour, order[i]->colour + 4, out.lights[4 * i + 1]);
		std::copy(order[i]->dir, order[i]->dir + 4, out.lights[4 * i + 2]);
		std::copy(order[i]->cone, order[i]->cone + 4, out.lights[4 * i + 3]);
		out.lightOwner[i] = order[i]->owner;
		out.lightLamp[i] = order[i]->lamp;
	}
}

// render.open_windows: a part with barrier quads draws its patched index buffer (degenerate triangles in their place); every other part and the
// switch off use the mesh's own list, so a frame with the switch off is the one before it.
sg_buffer WorldBuildings::ibufFor(uint32_t pid) const
{
	const PartRt& rt = partRt_[pid];
	return kOpenWindows.get() && rt.openHas ? rt.openIbuf.get() : meshes_[rt.mesh].gpu.get().ibuf;
}

void WorldBuildings::fillLights(const FrameData& d, LightParams& light, const lightvol::Volumes* vols)
{
	light.plHead[0] = (float)d.lightCount;
	for (int i = 0; i < d.lightCount * 4; i++) { std::copy(d.lights[i], d.lights[i] + 4, light.pl[i]); }
	// render.lamp_occlusion: a light that has a visibility volume among the live ones carries its code in pl[4 i + 3].w (lighting.inc light_points_vol)
	if (vols) { for (int i = 0; i < d.lightCount; i++) { if (d.lightLamp[i] >= 0) { light.pl[4 * i + 3][3] = (float)vols->lampCode(d.lightOwner[i], d.lightLamp[i]); } } }
}

int WorldBuildings::draw(uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], const float mvp[16], const LightParams& light, float fadeStart, float fadeEnd, int fadeMode,
	const float* limbSkin, const float* limbBlood)
{
	const PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	auto view = [&](int t) { return texView(t, grey_); };
	const int mode = limbSkin ? 7 : modeFor(pid);	// 7: a severed limb with the wire's tint (mesh.glsl inst_fs); without one it draws as before
	const MeshEntry& m = meshes_[rt.mesh];
	const Sub& s = m.subs[sub];
	sg_bindings b = {};
	b.vertex_buffers[0] = m.gpu.get().vbuf;
	b.vertex_buffer_offsets[0] = s.firstVertex * kVertexBytes;
	b.vertex_buffers[1] = inst;
	b.vertex_buffer_offsets[1] = instByteOffset;
	b.index_buffer = ibufFor(pid);
	b.index_buffer_offset = s.firstIndex * 4;
	// a severed limb binds the item's own maps (SeveredLimbItem vf108), not the submesh's script material: its texture, the race body normal (DXT5)
	// and its color map; every other part keeps the rule above
	b.views[VIEW_inst_tex] = mode == 7 && rt.ownTex >= 0 ? view(rt.ownTex) : view(rt.subTex[sub]);
	b.views[VIEW_inst_nrm] = mode == 7 ? texView(rt.normalTex, flat_)
		: (rt.subFlat[sub] || !kNormalMaps.get()) && mode != 1 && mode != 2 ? flat_ : texView(rt.normalTex, flat_);
	b.views[VIEW_inst_tex2] = view(rt.tex2Tex);
	b.views[VIEW_inst_nrm2] = rt.subFlat[sub] || !kNormalMaps.get() || rt.nrm2Tex < 0 ? flat_ : texView(rt.nrm2Tex, flat_);
	b.views[VIEW_inst_ground] = groundView_;
	b.views[VIEW_dust_noise] = dust_;
	b.views[VIEW_inst_mask] = mode == 7 ? view(rt.maskTex) : grey_;	// a severed limb's `color map` (skin tone mask)
	b.views[VIEW_inst_blood] = blood_;	// materials/bloody.png, bound by every draw like the shared units (sokol drops an unbound slot)
	b.samplers[SMP_inst_smp] = smp_;
	b.samplers[SMP_ground_smp] = groundSmp_;
	b.views[VIEW_shadow_tex] = shadow->view();
	b.samplers[SMP_shadow_smp] = shadow->sampler();
	b.samplers[SMP_shadow_raw] = shadow->rawSampler();
	b.views[VIEW_fog_map] = fog_map_binding().view;
	b.views[VIEW_spec_cube] = spec_cube_binding().view;
	b.views[VIEW_irr_cube] = irr_cube_binding().view;
	b.samplers[SMP_spec_smp] = spec_cube_binding().smp;
	b.views[VIEW_ao_tex] = ao_binding().view;
	b.views[VIEW_vol_sh] = lightvol::binding().sh;
	b.views[VIEW_vol_use] = lightvol::binding().use;
	b.views[VIEW_vol_lamp] = lightvol::binding().lamp;
	b.views[VIEW_vol_sun0] = lightvol::binding().sun[0];	// render.sun_bounce
	b.views[VIEW_vol_sun1] = lightvol::binding().sun[1];
	b.views[VIEW_vol_sun2] = lightvol::binding().sun[2];
	b.samplers[SMP_vol_smp] = lightvol::binding().smp;
	b.views[VIEW_il_tex] = ao_binding().il;
	b.samplers[SMP_ao_smp] = ao_binding().smp;
	b.samplers[SMP_fog_smp] = fog_map_binding().smp;
	if (detail && detail->ok)	// always bound: sokol drops a draw whose shader has an unbound view slot (terrain.cpp does the same)
	{
		b.views[VIEW_diffuse_maps] = detail->diffuse;
		b.views[VIEW_normal_maps] = detail->normal;
		b.views[VIEW_overlay_map] = detail->overlay;
		b.views[VIEW_biome_table] = detail->biomeTable;
		b.views[VIEW_ground_map] = detail->groundView;
		b.views[VIEW_ground_alpha] = groundAlphaView_;
		b.samplers[SMP_asmp] = detail->arraySmp;
		b.samplers[SMP_lsmp] = detail->linearSmp;
		b.samplers[SMP_tsmp] = detail->tableSmp;
	}
	drawstats::bindings(&b);
	float turn[6];
	const inst_vs_params_t vs = instVsParams(def, offsetFrame, cam_, time_, base, mvp, def.sway && kLeafSway.get() ? m.bmax[1] : 0.0f, motion_[pid].spin, turnOf(pid, turn));
	inst_fs_params_t fs = {};
	static_assert(sizeof(light_params_t) == sizeof(LightParams));
	fs.misc[0] = cutOf(pid);	// features by record (building parts clip on the normal map alpha, mode 1)
	fs.misc[3] = def.featureMode == 4 ? 1.0f : 0.0f;
	fs.misc[1] = (float)mode;
	fs.tri[0] = def.tileX;
	fs.tri[1] = def.tileY;
	fs.tri[2] = def.localCoords ? 1.0f : 0.0f;
	fs.tri[3] = kTriplanarUnits;
	fs.fade[0] = fadeStart;
	fs.fade[1] = fadeEnd;
	fs.fade[2] = (float)fadeMode;
	std::copy(xray, xray + 4, fs.xray);
	fs.misc[2] = def.featureMode == 4 ? 1.0f : 0.0f;	// FOLIAGE mode: COLOURING (vertex colour); otherwise lit like any StaticObject
	if (flipPass_)
	{
		fs.flip[0] = kFlipFade / 8.0f;
		std::copy(&flipEye_[0][0], &flipEye_[0][0] + 32, &fs.flip_eye[0][0]);
	}
	fs.misc2[0] = def.interior ? 1.0f : 0.0f;	// the dust coat takes the weather's `dust inside` (objects.hlsl INTERIOR, k16.2.202.1)
	if (limbSkin)	// the wire's tint (SeveredLimbItem custom 2 / custom 3)
	{
		std::copy(limbSkin, limbSkin + 3, fs.skin);
		std::copy(limbBlood, limbBlood + 3, fs.blood);
		fs.blood[3] = 1.0f;	// bloodColour's alpha: the exe's AppearanceHuman stores the RACE `blood colour` as ARGB with alpha 1 (character.hlsl severed_limb_fs)
	}
	sg_apply_uniforms(UB_inst_vs_params, SG_RANGE(vs));
	sg_apply_uniforms(UB_inst_fs_params, SG_RANGE(fs));
	sg_apply_uniforms(UB_light_params, { &light, sizeof light });
	sg_apply_uniforms(UB_ao_params, SG_RANGE(ao_binding().params));
	sg_apply_uniforms(UB_light_volumes, SG_RANGE(lightvol::binding().params));
	sg_apply_uniforms(UB_shadow_params, { &shadow->params(), sizeof(ShadowParams) });
	drawstats::draw(0, s.indexCount, count);
	return mode;
}

// A feature's alpha test cutoff, 0..1 (0 = none). FOLIAGE (kinspect foliage_alpha_threshold.md: the leaves and type-4 trunks are a StaticObject clone,
// objects.hlsl:208 clip(normalTex.a - threshold), threshold = the record's int / 255): on the normal map's alpha, the diffuse alpha being gloss; with
// no normal map the game binds flat.dds (opaque), so nothing is cut. The other modes: on the texture alpha.
float WorldBuildings::cutOf(uint32_t pid) const
{
	const BuildingPartDef& def = data_.parts[pid];
	if (def.featureMode < 0) { return 0.0f; }
	if (def.featureMode == 4)
	{
		const int t = partRt_[pid].normalTex;
		if (t < 0 || textures_[t].state != Ready) { return 0.0f; }
	}
	return def.alphaCut / 255.0f;
}

int WorldBuildings::modeFor(uint32_t pid) const
{
	const PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	auto ready = [&](int t) { return t >= 0 && textures_[t].state == Ready; };
	// shader mode: 0 plain, 1 ALPHA, 2 EMISSIVE, 3 DUAL, 4 TRIPLANAR, 5 TERRAIN, 6 DUAL_TRIPLANAR; a mode needs its extra texture loaded.
	// Features by MapFeatureMode (UV_MAPPED, TRIPLANAR, TERRAIN, DUAL_TEXTURE, FOLIAGE, DUAL_TRIPLANAR, EMISSIVE); FOLIAGE is plain + alpha test.
	static constexpr int kFeatureShade[7] = { 0, 4, 5, 3, 0, 6, 2 };
	const int shade = def.featureMode >= 0 ? kFeatureShade[def.featureMode] : def.materialType == 1 || def.materialType == 2 ? 1 : def.materialType == 4 ? 2 : def.materialType == 3 ? 3 : 0;
	return shade == 5 && !(detail && detail->ok) ? 4 : (shade == 1 || shade == 2) && !ready(rt.normalTex) ? 0 : shade == 3 && !ready(rt.tex2Tex) ? 0 : shade == 6 ? (ready(rt.tex2Tex) ? 6 : 4) : shade;
}

bool WorldBuildings::partReady(uint32_t pid, float* radius, float* centreLen) const
{
	const PartRt& rt = partRt_[pid];
	if (rt.mesh < 0 || !rt.resolved || meshes_[rt.mesh].state != Ready) { return false; }
	const MeshEntry& m = meshes_[rt.mesh];
	*radius = m.radius;
	*centreLen = std::sqrt(m.center[0] * m.center[0] + m.center[1] * m.center[1] + m.center[2] * m.center[2]);
	return true;
}

// A part is named in a draw only once every texture that draw reads is ready (kenshi-k16.2.298.1, the user's BDC launch: "the buildings draw in, but have
// no lighting. They're all BRIGHT WHITE. A second later, the lighting applies to them, and they darken"): the mesh is Ready frames before its diffuse maps,
// and the draw bound the 1x1 grey fallback (0x9a, texView) meanwhile, which under the loading exposure reads as a flat unlit wall until the real albedo
// arrives. The building groups now wait like a character does ("no placeholder bodies and no fallback textures", docs/RENDER_THREAD.md "What is drawn
// before a body is ready"): a part appears whole, pop-in rather than lost fidelity. What does not hold the part:
//  - a Failed create: it never comes, and holding it would hide the part for good; it draws the fallback as before (stats_.texturesFailed counts it);
//  - no texture at all (subTex -1, the material rule's grey): the same, that grey is the part's rendering there;
//  - the normal maps: modeFor drops ALPHA / DUAL / DUAL_TRIPLANAR back while theirs is missing, so they change the shading only, never the albedo.
// A part's submeshes are waited for together, so a wall never draws half textured.
bool WorldBuildings::partTexturesReady(uint32_t pid) const
{
	const PartRt& rt = partRt_[pid];
	if (rt.mesh < 0 || !rt.resolved || meshes_[rt.mesh].state != Ready) { return false; }
	auto there = [&](int t) { return t < 0 || textures_[t].state == Ready || textures_[t].state == Failed; };
	const BuildingPartDef& def = data_.parts[pid];
	if (def.kind == 2) { return true; }	// the interior mask: its hull's bounds, no texture (and never drawn by render())
	if (!there(rt.tex2Tex) || !there(rt.ownTex) || !there(rt.maskTex)) { return false; }	// DUAL's second map; a severed limb's own maps
	for (const int t : rt.subTex) { if (!there(t)) { return false; } }
	return true;
}

// Depth-only draw of one (part, submesh) into a shadow cascade: the instance vertex shader with the cascade matrix as mvp; cut-out parts keep their alpha test.
void WorldBuildings::drawShadow(uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], const float lvp[16], const float bias[4], float fadeStart, float fadeEnd)
{
	const PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	const int mode = modeFor(pid);
	const MeshEntry& m = meshes_[rt.mesh];
	const Sub& s = m.subs[sub];
	auto view = [&](int t) { return texView(t, grey_); };
	const float alphaCut = mode > 3 ? 0.0f : cutOf(pid);
	sg_bindings b = {};
	b.vertex_buffers[0] = m.gpu.get().vbuf;
	b.vertex_buffer_offsets[0] = s.firstVertex * kVertexBytes;
	b.vertex_buffers[1] = inst;
	b.vertex_buffer_offsets[1] = instByteOffset;
	b.index_buffer = ibufFor(pid);
	b.index_buffer_offset = s.firstIndex * 4;
	b.views[VIEW_inst_tex] = alphaCut <= 0 ? grey_ : def.featureMode == 4 ? view(rt.normalTex) : view(rt.subTex[sub]);
	b.views[VIEW_inst_nrm] = mode == 1 ? view(rt.normalTex) : grey_;
	b.samplers[SMP_inst_smp] = smp_;
	drawstats::bindings(&b);
	float turn[6];
	const inst_vs_params_t vs = instVsParams(def, offsetFrame, cam_, time_, base, lvp, def.sway && kLeafSway.get() ? m.bmax[1] : 0.0f, motion_[pid].spin, turnOf(pid, turn));
	shadow_fs_params_t fs = {};
	fs.cut[0] = alphaCut;
	fs.cut[1] = mode == 1 ? 1.0f : 0.0f;
	fs.fade[0] = fadeStart;
	fs.fade[1] = fadeEnd;
	memcpy(fs.bias, bias, sizeof fs.bias);
	sg_apply_uniforms(UB_inst_vs_params, SG_RANGE(vs));
	sg_apply_uniforms(UB_shadow_fs_params, SG_RANGE(fs));
	drawstats::draw(0, s.indexCount, count);
}

bool WorldBuildings::renderShadow(const ShadowCascade& c, int which)
{
	const float noBase[3] = {};
	bool any = false;
	for (const int gi : active_)
	{
		const Group& g = groups_[gi];
		const BuildingPartDef& def = data_.parts[g.part];
		const bool animated = spinning(def) || def.aimed;
		if ((which == 1 && animated) || (which == 2 && !animated)) { continue; }
		const int n = (int)((g.onlyShadow.size() + g.inRange.size()) / kInstanceFloats);
		const float fadeEnd = def.kind != 0 ? (float)interiorRadius : 0.0f;
		if (n <= 0) { continue; }
		if (!any) { drawstats::pipeline(shadowPip_); any = true; }
		drawShadow(g.part, g.sub, instBuf_, g.first * (int)kInstanceBytes, n, noBase, c.lvp, c.bias, fadeEnd * kInteriorFade, fadeEnd);
	}
	return any;
}

// What the static groups' shadow draws read besides the camera (the pack signature's inputs minus the camera, the view and the fog): the instance and
// part set, the load states, the doors' poses, the floors / peeks / ghosts / construction. Swaying leaves turn with the clock, so with render.leaf_sway
// on the time is part of it and the cache redraws every frame. The camera is out on purpose: a membership edge moving with it is under one texel of
// the tile, which redraws when the snapped centre moves (Shadow::update).
uint64_t WorldBuildings::shadowSig() const
{
	uint64_t h = 1469598103934665603ull;
	auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
	auto db = [](double x) { uint64_t u; std::memcpy(&u, &x, sizeof u); return u; };
	auto fb = [](float x) { uint32_t u; std::memcpy(&u, &x, sizeof u); return (uint64_t)u; };
	mix(db(farDist_));
	mix(db(interiorRadius));
	mix((uint64_t)(interiors ? 2u : 0u) | (uint64_t)(hideMarkers ? 16u : 0u) | (uint64_t)((radius & 0xffff) << 8));
	mix((uint64_t)offsetFrame);
	mix((uint64_t)visible_);
	mix(cuts_.size());
	for (const uint32_t bi : cuts_) { mix(bi); }
	mix(ghosts_.size());
	for (const auto& [bi, fade] : ghosts_) { mix(bi); mix(fb(fade)); }
	mix(construction_.size());
	for (const auto& [bi, progress] : construction_) { mix(bi); mix(fb(progress)); }
	mix(peeks.size());
	for (const auto& [bi, pk] : peeks) { mix(bi); mix((uint64_t)pk.floor); mix(fb(pk.fade)); mix((uint64_t)(pk.on ? 1u : 0u)); }
	mix(dataGen_);
	mix(stateGen_);
	mix(doorGen_);
	if (kLeafSway.get()) { mix(db(time_)); }
	return h;
}

// A part's dithered distance fade: interior parts over the last 20 % of the interior radius, exterior parts over the far ring's last kFarFade.
void WorldBuildings::fadeOf(uint32_t pid, float& start, float& end) const
{
	end = data_.parts[pid].kind != 0 ? (float)interiorRadius : (float)farDist_;
	start = end * (data_.parts[pid].kind != 0 ? kInteriorFade : 1.0f - kFarFade);
}

void WorldBuildings::render(const float mvp[16], const LightParams& light)
{
	if (active_.empty()) { return; }
	for (size_t& n : stats_.modeGroups) { n = 0; }
	const float noBase[3] = {};
	drawstats::pipeline(pip_);
	bool culled = false;
	for (const int gi : active_)
	{
		const Group& g = groups_[gi];
		// Interior parts and every feature / FOLIAGE_MESH mesh except the FOLIAGE mode (leaves, `Foliage` material: cull_hardware none) cull back faces
		// like the game's mapfeature.material (no cull_hardware = clockwise); a camera inside a rock sees the terrain, not the rock's far wall.
		if (cullsBackFaces(data_.parts[g.part]) != culled)
		{
			culled = !culled;
			drawstats::pipeline(culled ? pipInterior_ : pip_);
		}
		const int onlyShadow = (int)(g.onlyShadow.size() / kInstanceFloats), n = (int)((g.inRange.size() + g.outRange.size()) / kInstanceFloats);
		float fadeStart, fadeEnd;
		fadeOf(g.part, fadeStart, fadeEnd);
		if (n > 0) { stats_.modeGroups[draw(g.part, g.sub, instBuf_, (g.first + onlyShadow) * (int)kInstanceBytes, n, noBase, mvp, light, fadeStart, fadeEnd, 0, g.limb ? g.skin : nullptr, g.limb ? g.blood : nullptr)]++; }
	}
}

// The interior mask (interiormask.glsl): back faces into G, then front faces into R of the open mask pass, every fragment its view depth + 1 - the
// game's two InteriorMask render_scene passes. The instance positions were made relative to this view's camera in update(); mvp is this view's.
void WorldBuildings::renderMask(const float mvp[16])
{
	if (maskGroups_.empty()) { return; }
	const float noBase[3] = {};
	for (const sg_pipeline pip : { pipMaskBack_, pipMaskFront_ })
	{
		drawstats::pipeline(pip);
		for (const MaskGroup& g : maskGroups_)
		{
			const MeshEntry& m = meshes_[partRt_[g.part].mesh];
			const Sub& s = m.subs[g.sub];
			imask_vs_params_t vs = {};
			std::copy(mvp, mvp + 16, vs.mvp);
			float turn[6];
			const inst_vs_params_t p = instVsParams(data_.parts[g.part], offsetFrame, cam_, time_, noBase, mvp, 0, motion_[g.part].spin, turnOf(g.part, turn));
			std::copy(p.offset, p.offset + 4, vs.offset);
			std::copy(p.spin, p.spin + 4, vs.spin);
			sg_apply_uniforms(UB_imask_vs_params, SG_RANGE(vs));
			sg_bindings b = {};
			b.vertex_buffers[0] = m.gpu.get().vbuf;
			b.vertex_buffer_offsets[0] = s.firstVertex * kVertexBytes;
			b.vertex_buffers[1] = instBuf_;
			b.vertex_buffer_offsets[1] = g.first * (int)kInstanceBytes;
			b.index_buffer = ibufFor(g.part);
			b.index_buffer_offset = s.firstIndex * 4;
			drawstats::bindings(&b);
			drawstats::draw(0, s.indexCount, (int)(g.inst.size() / kInstanceFloats));
		}
	}
}

bool WorldBuildings::insideInterior(const double p[3], const double* origin, const double* dir, double* exitT) const
{
	bool inside = false;
	for (const HullRef& h : hulls_)
	{
		const BuildingInstance& b = data_.instances[h.instance];
		const int mi = partRt_[h.part].mesh;
		if (mi < 0) { continue; }
		const MeshEntry& m = meshes_[mi];
		if (m.state != Ready || m.hullIdx.empty()) { continue; }
		// the point in the part's mesh frame: undo the instance (rotation, scale, stretch) and the part's offset
		double l[3], off[3];
		toLocal(b, p, l);
		toMesh(offsetFrame, data_.parts[h.part].offset, off);
		bool in = true;
		for (int a = 0; a < 3; a++) { l[a] = l[a] / b.stretch[a] - off[a]; in = in && l[a] >= m.bmin[a] && l[a] <= m.bmax[a]; }
		if (!in) { continue; }
		int above = 0;
		const float* v = m.hullPos.data();
		for (size_t t = 0; t + 2 < m.hullIdx.size(); t += 3)
		{
			const float* a = v + m.hullIdx[t] * 3, *bb = v + m.hullIdx[t + 1] * 3, *c = v + m.hullIdx[t + 2] * 3;
			const double d0 = (bb[0] - a[0]) * (l[2] - a[2]) - (bb[2] - a[2]) * (l[0] - a[0]);
			const double d1 = (c[0] - bb[0]) * (l[2] - bb[2]) - (c[2] - bb[2]) * (l[0] - bb[0]);
			const double d2 = (a[0] - c[0]) * (l[2] - c[2]) - (a[2] - c[2]) * (l[0] - c[0]);
			if (!((d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0))) { continue; }
			const double area = d0 + d1 + d2;
			if (area == 0) { continue; }
			above += (d1 * a[1] + d2 * bb[1] + d0 * c[1]) / area > l[1];
		}
		if (!(above & 1)) { continue; }
		inside = true;
		if (!exitT || !origin || !dir) { return true; }
		// kenshi-k16.2.254.1: where the ray leaves this hull's box (mesh units, as the point test above): a click inside searches the shapes out to
		// here for the drawn floor, so the buried terrain between the camera and the floor does not end it
		constexpr double kFar = 20000;	// the pick's own range (main.cpp ray_ground)
		double s[3], e[3];
		const double end[3] = { origin[0] + dir[0] * kFar, origin[1] + dir[1] * kFar, origin[2] + dir[2] * kFar };
		toLocal(b, origin, s);
		toLocal(b, end, e);
		for (int a = 0; a < 3; a++) { s[a] = s[a] / b.stretch[a] - off[a]; e[a] = e[a] / b.stretch[a] - off[a]; }
		double t0, t1;
		if (segmentBox(s, e, m.bmin, m.bmax, t0, t1)) { *exitT = std::max(*exitT, t1 * kFar); }
	}
	return inside;
}

bool WorldBuildings::hasGlass() const
{
	for (const int gi : active_) { if (!groups_[gi].glass.empty()) { return true; } }
	return false;
}

void WorldBuildings::renderGlass(const float mvp[16], const LightParams& light)
{
	const float noBase[3] = {};
	for (const sg_pipeline pip : { pipGlassDepth_, pipGlass_ })
	{
		bool applied = false;
		for (const int gi : active_)
		{
			const Group& g = groups_[gi];
			if (g.glass.empty()) { continue; }
			if (!applied) { drawstats::pipeline(pip); applied = true; }
			const int before = (int)((g.onlyShadow.size() + g.inRange.size() + g.outRange.size()) / kInstanceFloats);
			float fadeStart, fadeEnd;
			fadeOf(g.part, fadeStart, fadeEnd);
			draw(g.part, g.sub, instBuf_, (g.first + before) * (int)kInstanceBytes, (int)(g.glass.size() / kInstanceFloats), noBase, mvp, light, fadeStart, fadeEnd, 0, g.limb ? g.skin : nullptr, g.limb ? g.blood : nullptr);
		}
	}
}

void WorldBuildings::renderFlip(const float mvp[16], const LightParams& light)
{
	if (!flipMoving_) { return; }
	const float noBase[3] = {};
	bool applied = false;
	flipPass_ = true;
	for (const int gi : active_)
	{
		const Group& g = groups_[gi];
		if (g.flip.empty()) { continue; }
		if (!applied) { drawstats::pipeline(pipFlip_); applied = true; }
		const int before = (int)((g.onlyShadow.size() + g.inRange.size() + g.outRange.size() + g.glass.size()) / kInstanceFloats);
		float fadeStart, fadeEnd;
		fadeOf(g.part, fadeStart, fadeEnd);
		draw(g.part, g.sub, instBuf_, (g.first + before) * (int)kInstanceBytes, (int)(g.flip.size() / kInstanceFloats), noBase, mvp, light, fadeStart, fadeEnd, 0, nullptr, nullptr);
	}
	flipPass_ = false;
}

void WorldBuildings::cursorRay(const double dir[3], bool on)
{
	for (int k = 0; k < 3; k++) { cursorDir_[k] = dir[k]; }
	cursorOn_ = on;
}

void WorldBuildings::renderIds(const float mvp[16])
{
	const float noBase[3] = {};
	bool culled = false;
	drawstats::pipeline(pipId_);
	for (const IdDraw& d : idDraws_)
	{
		const Group& g = groups_[d.group];
		const BuildingPartDef& def = data_.parts[g.part];
		if (cullsBackFaces(def) != culled)
		{
			culled = !culled;
			drawstats::pipeline(culled ? pipIdInterior_ : pipId_);
		}
		const PartRt& rt = partRt_[g.part];
		const MeshEntry& m = meshes_[rt.mesh];
		const Sub& sub = m.subs[g.sub];
		const size_t lists[3] = { g.inRange.size(), g.outRange.size(), g.glass.size() };
		size_t at = g.onlyShadow.size() / kInstanceFloats + d.index;
		for (int l = 0; l < d.list; l++) { at += lists[l] / kInstanceFloats; }
		sg_bindings b = {};
		b.vertex_buffers[0] = m.gpu.get().vbuf;
		b.vertex_buffer_offsets[0] = sub.firstVertex * kVertexBytes;
		b.vertex_buffers[1] = instBuf_;
		b.vertex_buffer_offsets[1] = (g.first + (int)at) * (int)kInstanceBytes;
		b.index_buffer = ibufFor(g.part);
		b.index_buffer_offset = sub.firstIndex * 4;
		drawstats::bindings(&b);
		float turn[6];
		const inst_vs_params_t vs = instVsParams(def, offsetFrame, cam_, time_, noBase, mvp, def.sway && kLeafSway.get() ? m.bmax[1] : 0.0f, motion_[g.part].spin, turnOf(g.part, turn));
		sg_apply_uniforms(UB_inst_vs_params, SG_RANGE(vs));
		const id_fs_params_t fs = { { (float)d.owner + 1.0f, 0, 0, 0 } };
		sg_apply_uniforms(UB_id_fs_params, SG_RANGE(fs));
		drawstats::draw(0, sub.indexCount, 1);
	}
}

WorldBuildings::Stats WorldBuildings::stats() const { return stats_; }

std::string WorldBuildings::fitReport() const
{
	auto pct = [](std::vector<float> v, double p) { std::sort(v.begin(), v.end()); return v.empty() ? 0.0f : v[(size_t)(p * (v.size() - 1))]; };
	char buf[256];
	snprintf(buf, sizeof buf, "n=%zu; lowest mesh point - terrain under origin p10/p50/p90 = %.1f/%.1f/%.1f; record relative y p10/p50/p90 = %.1f/%.1f/%.1f",
		fitGround_.size(), pct(fitGround_, 0.1), pct(fitGround_, 0.5), pct(fitGround_, 0.9), pct(fitRel_, 0.1), pct(fitRel_, 0.5), pct(fitRel_, 0.9));
	return buf;
}

std::string WorldBuildings::materialReport() const
{
	std::string out;
	for (const MeshEntry& m : meshes_)
	{
		if (m.state != Ready) { continue; }
		bool odd = false;
		for (const Sub& s : m.subs) { odd = odd || lower(s.material) != "staticobject"; }
		if (!odd) { continue; }
		out += m.path + ":";
		for (const Sub& s : m.subs) { out += " " + s.material; }
		out += "\n";
	}
	return out;
}

// ---- light probes (kenshi-jj3.1.1) ----

namespace
{
constexpr float kProbeSpacing = 6.0f;	// mesh units between probes, as the lab (the Storm House came to 45 x 21 x 50)
constexpr float kProbeMargin = 12.0f;	// the grid reaches this far past the building's interior hull
constexpr float kProbeGroundAlbedo = 0.3f;	// what the bake's ground bounces; the shader's open-field reference (lightvol) is the same number
constexpr uint64_t kProbeFormat = 1;	// bump when the bake's output changes: it is part of the cache key
constexpr double kSunStep = 5.0 * 3.14159265358979 / 180.0;	// render.sun_bounce: re-bake when the sun moved this far (the task's 5 degrees; the lab dragged it by hand)

const uint8_t* texelAt(const std::vector<uint8_t>& rgba, int w, int h, float u, float v)
{
	if (rgba.empty()) { return nullptr; }
	const int x = std::clamp((int)((u - std::floor(u)) * (float)w), 0, w - 1), y = std::clamp((int)((v - std::floor(v)) * (float)h), 0, h - 1);
	return &rgba[((size_t)y * w + x) * 4];
}
float lumaAt(const std::vector<uint8_t>& rgba, int w, int h, float u, float v)
{
	const uint8_t* p = texelAt(rgba, w, h, u, v);
	return p ? (0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) / 255.0f : kProbeGroundAlbedo;
}

}

float WorldBuildings::lightGroundAlbedo() { return kProbeGroundAlbedo; }

// The parts of building bi that the lamp occlusion traces (probeParts' set); empty until every mesh is loaded.
std::vector<WorldBuildings::ProbePart> WorldBuildings::probePartsFor(uint32_t bi)
{
	std::vector<ProbePart> parts;
	std::array<float, 3> mn, mx;
	float groundY = 0;
	if (!probeParts(bi, parts, mn, mx, groundY)) { parts.clear(); }
	return parts;
}

// The baked sky transfer of building bi's type: null until its bake finishes. The first ask starts the bake on the worker thread (or loads the cache) once the meshes are loaded;
// one bake runs at a time, so a caller asking nearest first gets the nearest baked first.
std::shared_ptr<const lightbake::Grid> WorldBuildings::gridFor(uint32_t bi)
{
	const BuildingInstance& b = data_.instances[bi];
	std::shared_ptr<const lightbake::Grid> grid;
	bool known;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		const auto it = probes_.find(b.nameId);
		known = it != probes_.end() && it->second.openWindows == kOpenWindows.get();	// a render.open_windows change re-bakes
		if (known) { grid = it->second.grid; }
		else if (it != probes_.end() && !probeBusy_) { probes_.erase(it); }
	}
	if (!known && !probeBusy_)
	{
		std::vector<ProbePart> parts;
		std::array<float, 3> mn, mx;
		float groundY = 0;
		if (probeParts(bi, parts, mn, mx, groundY))
		{
			if (probeThread_.joinable()) { probeThread_.join(); }	// the previous one has finished (probeBusy_ was false)
			{
				std::lock_guard<std::mutex> lk(probeMu_);
				probes_[b.nameId] = {};
				probes_[b.nameId].openWindows = kOpenWindows.get();
			}
			probeBusy_ = true;
			probeThread_ = std::thread([this, id = b.nameId, name = data_.names[b.nameId], parts = std::move(parts), mn, mx, groundY]() mutable { bakeProbes(id, name, std::move(parts), mn, mx, groundY); });
		}
	}
	return grid;
}

// The parts a bake traces, the grid bounds and the ground height of building bi: the same set for the sky and the sun bake. False until every mesh is loaded.
bool WorldBuildings::probeParts(uint32_t bi, std::vector<ProbePart>& parts, std::array<float, 3>& mn, std::array<float, 3>& mx, float& groundY)
{
	const BuildingInstance& b = data_.instances[bi];
	for (uint32_t k = 0; k < b.partCount; k++) { ensurePart(data_.partRefs[b.firstPart + k]); }	// queue the meshes (update() does for the ones in view)
	const BInfo* info = infoFor(bi);	// null until every mesh is loaded
	if (!info) { return false; }
	// render.open_windows: the bake is keyed by the switch, not by what the parts turned out to hold, so it waits until every part's barrier
	// classification is final (resolveOpenWindows waits for the atlases); retried next call.
	bool anyOpen = false;
	if (kOpenWindows.get())
	{
		for (uint32_t k = 0; k < b.partCount; k++)
		{
			const PartRt& rt = partRt_[data_.partRefs[b.firstPart + k]];
			if (!rt.openDone) { return false; }
			if (rt.openHas) { anyOpen = true; }
		}
	}
	for (uint32_t k = 0; k < b.partCount; k++)
	{
		const BuildingPartDef& def = data_.parts[data_.partRefs[b.firstPart + k]];
		const int mesh = partRt_[data_.partRefs[b.firstPart + k]].mesh;
		// no mask hulls (kind 2), black cut caps, position markers, glass / foliage materials (they let the light through); furniture is not a part
		if (mesh < 0 || def.kind == 2 || def.blackCap || def.marker || def.featureMode >= 0 || def.materialType == 1 || def.materialType == 2) { continue; }
		if (anyOpen && partRt_[data_.partRefs[b.firstPart + k]].openHide) { continue; }	// the black lining behind an opening this switch opens
		double off[3];
		toMesh(offsetFrame, def.offset, off);
		parts.push_back({ meshes_[mesh].path, def, { (float)off[0], (float)off[1], (float)off[2] } });
	}
	const float* lo = info->hasBounds ? info->mn : info->outerMn;
	const float* hi = info->hasBounds ? info->mx : info->outerMx;
	groundY = info->outerMn[1];
	mn = { lo[0] - kProbeMargin, groundY, lo[2] - kProbeMargin };
	mx = { hi[0] + kProbeMargin, hi[1] + kProbeMargin, hi[2] + kProbeMargin };
	return true;
}

// The camera inside a building's interior bounds (BInfo, the mask hulls): the same bounds probeParts sizes the grid with.
bool WorldBuildings::camInBuilding(uint32_t bi, const double cam[3])
{
	const BInfo* info = infoFor(bi);
	if (!info || !info->hasBounds) { return false; }
	double l[3];
	toLocal(data_.instances[bi], cam, l);
	for (int k = 0; k < 3; k++) { if (l[k] < info->mn[k] || l[k] > info->mx[k]) { return false; } }
	return true;
}

// The volumes for render.interior_light (lightvol.hpp): the nearest buildings with interiors within `radius` of the camera whose bake has finished, nearest first, at most max.
// render.sun_bounce (sunOn): each volume also carries the sun transfer of its type (sunNew, and sunOld while the new bake fades in), and the buildings that hold the
// camera or a player character start a sun (re-)bake, one at a time, nearest first.
bool WorldBuildings::lightVolumes(const double cam[3], double radius, size_t max, std::vector<lightvol::Volume>& out, bool sunOn, const float* sunDir, const float* groundAlbedo)
{
	out.clear();
	if (data_.instances.empty()) { return false; }
	if (volScan_++ % 30 == 0)
	{
		std::vector<std::pair<double, uint32_t>> near;
		const double r2 = radius * radius;
		for (size_t i = 0; i < data_.instances.size(); i++)
		{
			const BuildingInstance& b = data_.instances[i];
			if (b.parent >= 0 || b.feature || b.prop || b.maxFloor == 0) { continue; }
			const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], d = dx * dx + dy * dy + dz * dz;
			if (d < r2) { near.push_back({ d, (uint32_t)i }); }
		}
		std::sort(near.begin(), near.end());
		volNear_.clear();
		for (size_t i = 0; i < near.size() && i < 3 * max; i++) { volNear_.push_back(near[i].second); }	// a few spare: a bake not finished yet is skipped
	}
	const bool sun = sunOn && sunDir && sunDir[1] > 0.0f && groundAlbedo;	// below the horizon the term is zero and nothing is baked
	// One sun bake at a time, for the nearest building that holds the camera or a player character (the task's scope: those are the rooms anyone sees).
	if (sun)
	{
		for (const uint32_t bi : volNear_)
		{
			if (bi >= data_.instances.size()) { continue; }
			bool held = camInBuilding(bi, cam);
			for (size_t k = 0; !held && k < occupants.size(); k++)
			{
				const int inst = occupants[k].instance >= 0 ? occupants[k].instance : floorableOf(occupants[k].building);
				held = inst == (int)bi;
			}
			if (!held) { continue; }
			std::shared_ptr<const lightbake::Grid> cur, old;
			sunFor(bi, sunDir, groundAlbedo, cur, old);
			if (sunBusy_) { break; }	// one at a time: the next candidate waits
		}
	}
	for (const uint32_t bi : volNear_)
	{
		if (bi >= data_.instances.size()) { continue; }
		const BuildingInstance& b = data_.instances[bi];
		std::shared_ptr<const lightbake::Grid> g = gridFor(bi);
		if (!g) { continue; }
		lightvol::Volume v;
		v.key = bi;
		v.grid = std::move(g);
		if (sun)
		{
			std::lock_guard<std::mutex> lk(sunMu_);
			const auto it = sun_.find(b.nameId);
			if (it != sun_.end()) { v.sunNew = it->second.cur; v.sunOld = it->second.old; }
		}
		v.origin[0] = b.x; v.origin[1] = b.y; v.origin[2] = b.z;
		v.q[0] = b.qw; v.q[1] = b.qx; v.q[2] = b.qy; v.q[3] = b.qz;
		for (int k = 0; k < 3; k++) { v.scale[k] = (float)(b.scale * b.stretch[k]); }
		if (lampOcclusion)	// the lamp visibility the worker published (kickLamps starts the jobs on the logic side; lampReg_ is never read here)
		{
			std::lock_guard<std::mutex> lk(probeMu_);
			const auto it = lampVis_.find(bi);
			if (it != lampVis_.end()) { v.lamps = it->second.data; v.lampCount = it->second.count; }
		}
		if (sliced((int)bi))	// the same height aboveSlab hides parts at: the air above it belongs to the floors the cut hides (kenshi-jj3.26.11)
		{
			const int s = shownOf(bi);
			const BInfo* info = s < std::min<int>(b.maxFloor, 3) ? infoFor(bi) : nullptr;
			if (info && info->hasBounds) { v.cutY = info->floorY[s + 1] - 4; }
		}
		out.push_back(std::move(v));
		if (out.size() >= max) { break; }
	}
	return !out.empty();
}

// Logic side (end of update()): the nearest building whose lamp list is newer than its published visibility gets a trace job (one at a time). The grid is the baked one
// (lightVolumes starts that bake); a building without it yet waits.
void WorldBuildings::kickLamps(const double cam[3])
{
	if (occBusy_) { return; }
	uint32_t best = 0xffffffffu;
	double bestD = 1e300;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		for (const auto& e : lampReg_)
		{
			const auto v = lampVis_.find(e.first);
			if (v != lampVis_.end() && v->second.version == e.second.version) { continue; }
			const BuildingInstance& b = data_.instances[e.first];
			const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], d = dx * dx + dy * dy + dz * dz;
			if (d < bestD) { bestD = d; best = e.first; }
		}
	}
	if (best == 0xffffffffu) { return; }
	const BuildingInstance& b = data_.instances[best];
	std::shared_ptr<const lightbake::Grid> grid;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		const auto it = probes_.find(b.nameId);
		if (it != probes_.end() && it->second.openWindows == kOpenWindows.get()) { grid = it->second.grid; }
	}
	if (!grid) { return; }
	std::vector<ProbePart> parts = probePartsFor(best);
	if (parts.empty()) { return; }
	const LampReg& reg = lampReg_[best];
	if (occThread_.joinable()) { occThread_.join(); }
	occBusy_ = true;
	occThread_ = std::thread([this, owner = best, id = b.nameId, parts = std::move(parts), grid = std::move(grid), lamps = reg.local, ver = reg.version]() mutable { lampJob(owner, id, std::move(parts), std::move(grid), std::move(lamps), ver); });
}

// The worker thread of the lamp occlusion: the type's occluder (kept for the last few types), then one ray per probe per lamp of the placement.
void WorldBuildings::lampJob(uint32_t owner, uint32_t nameId, std::vector<ProbePart> parts, std::shared_ptr<const lightbake::Grid> grid, std::vector<std::array<float, 3>> lamps, int version)
{
	std::shared_ptr<lightbake::Occluder> occ;
	const auto it = lampOccluders_.find(nameId);
	if (it != lampOccluders_.end()) { occ = it->second; }
	else
	{
		std::vector<lightbake::Tri> tris;
		tris = buildTris(parts, kOpenWindows.get());
		if (!tris.empty())
		{
			occ = std::make_shared<lightbake::Occluder>(std::move(tris));
			if (lampOccluders_.size() >= 4) { lampOccluders_.clear(); }
			lampOccluders_[nameId] = occ;
		}
	}
	LampVis out;
	out.version = version;
	out.count = (int)lamps.size();
	if (occ) { out.data = std::make_shared<const std::vector<uint8_t>>(occ->lampVisibility(*grid, lamps)); }
	if (out.data)
	{
		size_t seen[lightbake::kMaxLamps] = {};
		for (size_t i = 0; i < out.data->size(); i++) { seen[i % lightbake::kMaxLamps] += (*out.data)[i] > 127; }
		std::string per;
		for (size_t k = 0; k < lamps.size(); k++) { per += " " + std::to_string((int)(100.0 * (double)seen[k] / (double)std::max<size_t>(1, grid->usableCount()))) + "%"; }
		fprintf(stderr, "lamp_occlusion: %s: %zu lamps, share of usable probes that see each:%s\n", data_.names[nameId].c_str(), lamps.size(), per.c_str());
	}
	std::lock_guard<std::mutex> lk(probeMu_);
	lampVis_[owner] = std::move(out);
	occBusy_ = false;
}

// steady-clock seconds at the first call (the log's t= stamps)
static double sunClock0() { static const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); return t0; }

// The sun transfer of building bi's type for `sunDir` (world): copies the latest bake out; when it is missing, failed or more than kSunStep degrees off, starts a new
// one on sunThread_ (unless one runs). Returns false when there is nothing (yet).
bool WorldBuildings::sunFor(uint32_t bi, const float sunDir[3], const float groundAlbedo[3], std::shared_ptr<const lightbake::Grid>& cur, std::shared_ptr<const lightbake::Grid>& old)
{
	sunClock0();
	const BuildingInstance& b = data_.instances[bi];
	const float len = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
	if (len < 1e-6f) { return false; }
	const float wd[3] = { sunDir[0] / len, sunDir[1] / len, sunDir[2] / len };
	bool start = false;
	std::string trigger = "first bake";
	{
		std::lock_guard<std::mutex> lk(sunMu_);
		const auto it = sun_.find(b.nameId);
		if (it != sun_.end())
		{
			cur = it->second.cur;
			old = it->second.old;
			if (it->second.pending || it->second.failed) { return cur != nullptr; }
			const float c = std::clamp(it->second.dir[0] * wd[0] + it->second.dir[1] * wd[1] + it->second.dir[2] * wd[2], -1.0f, 1.0f);
			const float deg = std::acos(c) * 180.0f / 3.14159265f;
			const bool stepped = std::acos(c) > kSunStep, windows = it->second.openWindows != kOpenWindows.get();
			start = !cur || stepped || windows;	// a render.open_windows change re-bakes
			if (start && cur && !windows)
			{
				// duty cycle: a sun-step bake waits until 2x the last bake's duration has passed since it finished (at most a third of the time on 4 threads)
				const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
				const double wait = 2.0 * sunLastSeconds_.load() - (now - sunFinishedAt_.load());
				if (wait > 0)
				{
					if (!sunDeferLogged_) { sunDeferLogged_ = true; fprintf(stderr, "kclient: sun_bake %s: deferred %.1f s (duty cycle) [t=%.1f]\n", data_.names[b.nameId].c_str(), wait, now - sunClock0()); }
					return cur != nullptr;
				}
			}
			char tb[120];
			snprintf(tb, sizeof tb, "%s, sun moved %.2f deg since the last bake (step %.0f)", !cur ? "no bake yet (previous failed or empty)" : stepped ? "sun step" : "open_windows changed", deg, (double)kSunStep * 180.0 / 3.14159265);
			trigger = tb;
		}
		else { start = true; }
	}
	if (!start || sunBusy_ || probeBusy_) { return cur != nullptr; }	// one bake at a time, with the sky bakes: retried next call
	std::vector<ProbePart> parts;
	std::array<float, 3> mn, mx;
	float groundY = 0;
	if (!probeParts(bi, parts, mn, mx, groundY)) { return cur != nullptr; }	// meshes still loading: retried next call
	// the world sun in the building's local frame (the bake traces the local meshes)
	const float conj[4] = { b.qw, -b.qx, -b.qy, -b.qz };
	const double wdd[3] = { wd[0], wd[1], wd[2] };
	double local[3];
	rotate(conj, wdd, local);
	const float ld[3] = { (float)local[0], (float)local[1], (float)local[2] };
	float ground[3];
	for (int k = 0; k < 3; k++) { ground[k] = groundAlbedo[k]; }
	{
		std::lock_guard<std::mutex> lk(sunMu_);
		sun_[b.nameId].pending = true;
	}
	if (sunThread_.joinable()) { sunThread_.join(); }
	sunBusy_ = true;
	sunDeferLogged_ = false;
	sunThread_ = std::thread([this, id = b.nameId, name = data_.names[b.nameId], parts = std::move(parts), mn, mx, groundY, wd, ld, ground, trigger]() mutable
	{
		bakeSun(id, name, std::move(parts), mn, mx, groundY, wd, ld, ground, trigger);
	});
	return cur != nullptr;
}

// The baking thread: traces the sun transfer of the type and publishes it (the build starts fading in). The world direction it was baked for is what the next call
// measures the sun's movement against.
void WorldBuildings::bakeSun(uint32_t nameId, std::string name, std::vector<ProbePart> parts, std::array<float, 3> mn, std::array<float, 3> mx, float groundY,
	const float worldDir[3], const float localDir[3], const float ground[3], std::string trigger)
{
	const bool openWindows = kOpenWindows.get();
	std::vector<lightbake::Tri> tris = buildTris(parts, openWindows);
	lightbake::Grid g;
	if (tris.empty()) { g.sunDir[0] = localDir[0]; g.sunDir[1] = localDir[1]; g.sunDir[2] = localDir[2]; }
	else { g = lightbake::bakeSun(std::move(tris), mn.data(), mx.data(), kProbeSpacing, groundY, ground, localDir, 4); }
	if (lightbake::cancelled())
	{
		{ std::lock_guard<std::mutex> lk(sunMu_); sun_[nameId].pending = false; }	// nothing published
		sunBusy_ = false;
		fprintf(stderr, "kclient: sun_bake %s: cancelled (quit) after %.2f s, nothing published (trigger: %s)\n", name.c_str(), g.sunSeconds, trigger.c_str());
		return;
	}
	const bool hasSun = g.sunValid();	// before the move below empties g
	const double seconds = g.sunSeconds;
	const size_t vpls = g.sunVpls;
	{
		std::lock_guard<std::mutex> lk(sunMu_);
		SunEntry& e = sun_[nameId];
		if (e.cur) { e.old = e.cur; }	// the bake this one replaces fades out over the new one
		e.cur = std::make_shared<const lightbake::Grid>(std::move(g));
		for (int k = 0; k < 3; k++) { e.dir[k] = worldDir[k]; e.ground[k] = ground[k]; }
		e.pending = false;
		e.failed = false;
		e.openWindows = openWindows;
	}
	sunLastSeconds_ = seconds;
	sunFinishedAt_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	sunBakes_++;
	sunBusy_ = false;
	fprintf(stderr, "kclient: sun_bake %s: dir local %.2f %.2f %.2f, ground %.2f %.2f %.2f, %zu vpls, %.2f s on 4 threads%s; trigger: %s [finished t=%.1f]\n", name.c_str(),
		localDir[0], localDir[1], localDir[2], ground[0], ground[1], ground[2], vpls, seconds, hasSun ? "" : " (sun below the horizon: empty)", trigger.c_str(), sunFinishedAt_.load() - sunClock0());
}

bool WorldBuildings::lightProbes(const double cam[3], double radius, std::vector<ProbePoint>& out)
{
	out.clear();
	if (data_.instances.empty()) { return false; }
	if (probeScan_++ % 30 == 0 || probeTarget_ < 0)
	{
		probeTarget_ = -1;
		double best = (interiorRadius + kInteriorSlack) * (interiorRadius + kInteriorSlack);
		for (size_t i = 0; i < data_.instances.size(); i++)
		{
			const BuildingInstance& b = data_.instances[i];
			if (b.parent >= 0 || b.feature || b.prop || b.maxFloor == 0) { continue; }
			const double dx = b.x - cam[0], dy = b.y - cam[1], dz = b.z - cam[2], d = dx * dx + dy * dy + dz * dz;
			if (d < best) { best = d; probeTarget_ = (int)i; }
		}
	}
	if (probeTarget_ < 0) { return false; }
	const uint32_t bi = (uint32_t)probeTarget_;
	const BuildingInstance& b = data_.instances[bi];
	std::shared_ptr<const lightbake::Grid> grid;
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		probeAsked_ = b.nameId;
	}
	grid = gridFor(bi);
	if (!grid) { return false; }
	probeAt_[0] = b.x; probeAt_[1] = b.y; probeAt_[2] = b.z;
	probeDist_ = std::sqrt((b.x - cam[0]) * (b.x - cam[0]) + (b.y - cam[1]) * (b.y - cam[1]) + (b.z - cam[2]) * (b.z - cam[2]));
	const float q[4] = { b.qw, b.qx, b.qy, b.qz };
	const double r2 = radius * radius;
	for (size_t i = 0; i < grid->count(); i++)
	{
		float local[3];
		grid->position(i, local);
		const double lc[3] = { (double)local[0] * b.scale * b.stretch[0], (double)local[1] * b.scale * b.stretch[1], (double)local[2] * b.scale * b.stretch[2] };
		double rc[3];
		rotate(q, lc, rc);
		const double w[3] = { b.x + rc[0], b.y + rc[1], b.z + rc[2] };
		const double dx = w[0] - cam[0], dy = w[1] - cam[1], dz = w[2] - cam[2];
		if (dx * dx + dy * dy + dz * dz > r2) { continue; }
		out.push_back({ { w[0], w[1], w[2] }, grid->level(i), grid->usable(i) });
	}
	probeDrawn_ = out.size();
	return true;
}

std::string WorldBuildings::lightBakeSlice(int layer) const
{
	std::lock_guard<std::mutex> lk(probeMu_);
	const auto it = probes_.find(probeAsked_);
	if (it == probes_.end() || !it->second.grid) { return "light_slice: no baked building"; }
	const lightbake::Grid& g = *it->second.grid;
	if (layer < 0 || layer >= g.dim[1]) { return "light_slice: layer 0.." + std::to_string(g.dim[1] - 1); }
	std::string out = "light_slice: " + data_.names[probeAsked_] + " layer " + std::to_string(layer) + " (y " + std::to_string((int)(g.min[1] + g.spacing * (float)layer)) + " above the building origin's frame), x across, z down\n";
	for (int z = 0; z < g.dim[2]; z++)
	{
		for (int x = 0; x < g.dim[0]; x++)
		{
			const size_t i = ((size_t)z * g.dim[1] + (size_t)layer) * g.dim[0] + (size_t)x;
			out += !g.usable(i) ? '#' : (char)('0' + std::clamp((int)(g.level(i) * 9.0f + 0.5f), 0, 9));
		}
		out += '\n';
	}
	return out;
}

// `dump open_windows [MAXLUMA]` (render.open_windows survey, kenshi-jj3.10.1): every building type with interiors placed in the world, its shell and
// interior parts' small detached islands, the UV rectangle each samples, its brightest sampled texel and the dark verdict. Slow and synchronous: it
// decodes the meshes and atlases again (a findings run, not a per-frame query). One line per island; a building's header has its dark count.
std::string WorldBuildings::openWindowsReport(float maxLuma)
{
	struct Albedo { std::vector<uint8_t> rgba; int w = 0, h = 0; bool tried = false; };
	std::unordered_map<std::string, MeshCpu> meshes;
	std::unordered_map<std::string, Albedo> albedos;
	char buf[512];
	std::string out;
	size_t types = 0, islands = 0, dark = 0;
	std::vector<char> seen(data_.names.size(), 0);
	for (const uint32_t ii : data_.floorable)
	{
		const BuildingInstance& b = data_.instances[ii];
		if (b.parent >= 0 || b.feature || b.prop || b.nameId >= seen.size() || seen[b.nameId]) { continue; }
		seen[b.nameId] = 1;
		types++;
		const std::string& name = data_.names[b.nameId];
		size_t bi = 0, bd = 0;
		std::string detail;
		for (uint32_t k = 0; k < b.partCount; k++)
		{
			const uint32_t pid = data_.partRefs[b.firstPart + k];
			const BuildingPartDef& def = data_.parts[pid];
			if (def.kind != 0 && def.kind != 1) { continue; }
			if (def.blackCap || def.marker || def.materialType != 0 || def.featureMode >= 0) { continue; }
			const std::string file = res_->locate(def.mesh);
			if (file.empty()) { continue; }
			auto mi = meshes.find(file);
			if (mi == meshes.end())
			{
				MeshCpu mesh;
				if (!decodeMesh(file, mesh)) { continue; }
				mi = meshes.emplace(file, std::move(mesh)).first;
			}
			const MeshCpu& mesh = mi->second;
			for (size_t s = 0; s < mesh.subs.size(); s++)
			{
				const Sub& sub = mesh.subs[s];
				if (sub.islands.empty()) { continue; }
				// no black default here: a missing diffuse is no albedo to the barrier verdict, while black.dds would read every detached island dark and open it
				const std::string texFile = textureFile(def, sub.material, nullptr, false);
				Albedo* alb = nullptr;
				if (!texFile.empty())
				{
					auto ai = albedos.find(texFile);
					if (ai == albedos.end())
					{
						Albedo a;
						a.tried = true;
						DdsImage dds;
						std::string err;
						if (dds_load(texFile, dds, err))
						{
							int level = 0;
							while (level + 1 < dds.mips && (std::max(dds.width, dds.height) >> level) > 128) { level++; }
							if (!dds_decode(dds, level, a.rgba, a.w, a.h)) { a.rgba.clear(); }
						}
						ai = albedos.emplace(texFile, std::move(a)).first;
					}
					alb = &ai->second;
				}
				const openwindows::Texel tex = { nullptr, alb && !alb->rgba.empty() ? alb->rgba.data() : nullptr, alb ? alb->w : 0, alb ? alb->h : 0 };
				const std::vector<uint32_t> barrierTris = openwindows::triangles(sub.islands, def.tileX, def.tileY, tex, maxLuma);	// what the switch leaves out
				for (const openwindows::Island& is : sub.islands)
				{
					float mn[2] = { 1e30f, 1e30f }, mx[2] = { -1e30f, -1e30f }, worst = 0;
					for (size_t i = 0; i + 1 < is.uv.size(); i += 2)
					{
						const float u = is.uv[i] * (def.tileX > 0 ? def.tileX : 1.0f), v = is.uv[i + 1] * (def.tileY > 0 ? def.tileY : 1.0f);
						mn[0] = std::min(mn[0], u); mx[0] = std::max(mx[0], u);
						mn[1] = std::min(mn[1], v); mx[1] = std::max(mx[1], v);
					}
					for (int gj = 0; gj < 5; gj++)	// the grid openwindows::dark samples
					{
						for (int gi = 0; gi < 5; gi++)
						{
							const float u = mn[0] + (mx[0] - mn[0]) * ((float)gi + 0.5f) / 5.0f, v = mn[1] + (mx[1] - mn[1]) * ((float)gj + 0.5f) / 5.0f;
							worst = std::max(worst, tex.at(u, v));
						}
					}
					const bool isDark = openwindows::dark(is, def.tileX, def.tileY, tex, maxLuma);
					const bool isBarrier = std::find_first_of(is.tris.begin(), is.tris.end(), barrierTris.begin(), barrierTris.end()) != is.tris.end();
					islands++;
					bi++;
					bd += isBarrier;
					out += "    ";
					out += isDark ? "DARK " : isBarrier ? "back " : "bright ";
					const size_t slash = file.find_last_of('/');
					snprintf(buf, sizeof buf, "%s sub%zu tri", (slash == std::string::npos ? file : file.substr(slash + 1)).c_str(), s);
					out += buf;
					for (const uint32_t t : is.tris) { snprintf(buf, sizeof buf, " %u", t); out += buf; }
					snprintf(buf, sizeof buf, " (kind %d floor %d) %.1fx%.1fx%.1f u uv %.3f..%.3f x %.3f..%.3f max texel %.2f", def.kind, def.floor,
						is.mx[0] - is.mn[0], is.mx[1] - is.mn[1], is.mx[2] - is.mn[2], mn[0], mx[0], mn[1], mx[1], worst);
					out += buf;
					out += "\n";
				}
			}
		}
		snprintf(buf, sizeof buf, "open_windows: %s: %zu islands, %zu barrier\n", name.c_str(), bi, bd);
		out += buf;
		dark += bd;
	}
	std::string head;
	snprintf(buf, sizeof buf, "open_windows: %zu building types with interiors (first placement each), %zu islands, %zu barrier at max texel %.2f\n", types, islands, dark, maxLuma);
	head += buf;
	return head + out;
}

std::string WorldBuildings::lightBakeLine() const
{
	std::lock_guard<std::mutex> lk(probeMu_);
	const auto it = probes_.find(probeAsked_);
	if (it == probes_.end()) { return "light_bake: no building asked about yet (debug.light_probes on, a building with interiors near the camera)"; }
	const std::string& name = data_.names[probeAsked_];
	if (it->second.failed) { return "light_bake: " + name + " failed: " + it->second.why; }
	const std::shared_ptr<const lightbake::Grid>& g = it->second.grid;
	if (!g) { return "light_bake: " + name + " pending"; }
	// the sky level of the usable probes: in the open (enclosure < 0.1) it should be about 1, indoors (> 0.5) lower, near an opening above 0
	double open = 0, in = 0;
	size_t nOpen = 0, nIn = 0, bright = 0;
	for (size_t i = 0; i < g->count(); i++)
	{
		if (!g->usable(i)) { continue; }
		if (g->enclosure(i) < 0.1f) { open += g->level(i); nOpen++; }
		else if (g->enclosure(i) > 0.5f) { in += g->level(i); nIn++; bright += g->level(i) > 0.15f; }
	}
	char tail[160];
	snprintf(tail, sizeof tail, "; mean sky level open %.2f (%zu probes), indoors %.3f (%zu probes, %zu above 0.15)", nOpen ? open / (double)nOpen : 0.0, nOpen, nIn ? in / (double)nIn : 0.0, nIn, bright);
	char buf[560];
	snprintf(buf, sizeof buf, "light_bake: %s grid %dx%dx%d spacing %.0f probes %zu usable %zu triangles %zu seconds %.2f %s; %zu probes within the draw radius, the camera %.0f from the building at %.0f %.0f %.0f", name.c_str(), g->dim[0], g->dim[1], g->dim[2], g->spacing,
		g->count(), g->usableCount(), g->triangles, g->seconds, g->fromCache ? "(from the cache)" : "(baked)", probeDrawn_, probeDist_, probeAt_[0], probeAt_[1], probeAt_[2]);
	std::string sun;
	{
		std::lock_guard<std::mutex> slk(sunMu_);
		const auto si = sun_.find(probeAsked_);
		if (si == sun_.end()) { sun = "sun_bake: none yet"; }
		else if (si->second.pending) { sun = "sun_bake: pending"; }
		else if (!si->second.cur) { sun = "sun_bake: none"; }
		else
		{
			const lightbake::Grid& s = *si->second.cur;
			char sb[220];
			snprintf(sb, sizeof sb, "sun_bake: world dir %.2f %.2f %.2f, %zu probes, %zu vpls, %.2f s on 4 threads%s, %zu bakes since start", si->second.dir[0], si->second.dir[1], si->second.dir[2],
				s.count(), s.sunVpls, s.sunSeconds, s.sunValid() ? "" : " (below the horizon)", (size_t)sunBakes_);
			sun = sb;
		}
	}
	return std::string(buf) + tail + "; " + sun;
}

// The worker thread of one type's bake: the cache by the content hash of its mesh files, else decode every part, bake, save.
void WorldBuildings::bakeProbes(uint32_t nameId, std::string name, std::vector<ProbePart> parts, std::array<float, 3> mn, std::array<float, 3> mx, float groundY)
{
	const bool openWindows = kOpenWindows.get();	// render.open_windows: the barrier quads are left out of the bake (a separate cache entry)
	auto publish = [&](lightbake::Grid&& g, const std::string& why)
	{
		std::lock_guard<std::mutex> lk(probeMu_);
		ProbeEntry& e = probes_[nameId];
		e.openWindows = openWindows;
		if (why.empty()) { e.grid = std::make_shared<const lightbake::Grid>(std::move(g)); } else { e.failed = true; e.why = why; }
		probeBusy_ = false;
	};
	uint64_t key = lightbake::hashBytes(lightbake::kHashSeed, &kProbeFormat, sizeof kProbeFormat);
	key = lightbake::hashBytes(key, &kProbeSpacing, sizeof kProbeSpacing);
	key = lightbake::hashBytes(key, &openWindows, sizeof openWindows);
	key = lightbake::hashBytes(key, mn.data(), sizeof(float) * 3);
	key = lightbake::hashBytes(key, mx.data(), sizeof(float) * 3);
	for (const ProbePart& p : parts)
	{
		std::ifstream f(p.mesh, std::ios::binary);
		std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		key = lightbake::hashBytes(key, bytes.data(), bytes.size());
		key = lightbake::hashBytes(key, p.off, sizeof p.off);
		key = lightbake::hashBytes(key, p.def.texture.data(), p.def.texture.size());
		key = lightbake::hashBytes(key, &p.def.tileX, sizeof(float) * 2);
	}
	const std::string path = lightbake::cachePath(name, key);
	lightbake::Grid g;
	if (lightbake::load(path, g)) { publish(std::move(g), ""); return; }

	std::vector<lightbake::Tri> tris = buildTris(parts, openWindows);
	if (tris.empty()) { publish({}, "no triangles"); return; }
	g = lightbake::bake(std::move(tris), mn.data(), mx.data(), kProbeSpacing, groundY, kProbeGroundAlbedo, 4);
	if (lightbake::cancelled()) { fprintf(stderr, "kclient: light_bake %s: cancelled (quit), nothing published or cached\n", name.c_str()); probeBusy_ = false; return; }
	lightbake::save(path, g);
	publish(std::move(g), "");
}

// The bake's triangles of one building type: position, face normal (facing the vertex normals' way), the diffuse texture's luma (the sky transfer) and RGB
// (the sun transfer's tint).
std::vector<lightbake::Tri> WorldBuildings::buildTris(const std::vector<ProbePart>& parts, bool openWindows) const
{
	std::vector<lightbake::Tri> tris;
	struct Albedo { std::vector<uint8_t> rgba; int w = 0, h = 0; };
	std::unordered_map<std::string, Albedo> albedo;
	for (const ProbePart& p : parts)
	{
		MeshCpu mesh;
		if (!decodeMesh(p.mesh, mesh)) { continue; }
		for (const Sub& s : mesh.subs)
		{
			const std::string file = textureFile(p.def, s.material);
			Albedo noAlbedo, *alb = &noAlbedo;
			if (!file.empty())
			{
				auto [it, fresh] = albedo.emplace(file, Albedo());
				if (fresh)
				{
					DdsImage dds;
					std::string err;
					if (dds_load(file, dds, err))
					{
						int level = 0;
						while (level + 1 < dds.mips && (std::max(dds.width, dds.height) >> level) > 128) { level++; }
						if (!dds_decode(dds, level, it->second.rgba, it->second.w, it->second.h)) { it->second.rgba.clear(); }
					}
				}
				alb = &it->second;
			}
			// render.open_windows: the submesh's barrier quads are left out of the bake (the same islands and dark test the draw uses)
			std::vector<char> drop((size_t)s.indexCount / 3, 0);
			if (openWindows && !alb->rgba.empty())
			{
				const openwindows::Texel tex = { nullptr, alb->rgba.data(), alb->w, alb->h };
				for (const uint32_t tri : openwindows::triangles(s.islands, p.def.tileX, p.def.tileY, tex)) { if (tri < drop.size()) { drop[tri] = 1; } }
			}
			for (int i = 0; i + 2 < s.indexCount; i += 3)
			{
				if (drop[(size_t)i / 3]) { continue; }
				lightbake::Tri t = {};
				float vn[3] = {}, uv[2] = {};
				for (int c = 0; c < 3; c++)
				{
					const float* v = &mesh.verts[((size_t)s.firstVertex + mesh.indices[(size_t)s.firstIndex + i + c]) * kVertexFloats];
					for (int k = 0; k < 3; k++) { t.p[c][k] = v[k] + p.off[k]; vn[k] += v[3 + k]; }
					uv[0] += v[6] / 3;
					uv[1] += v[7] / 3;
				}
				const float u[3] = { t.p[1][0] - t.p[0][0], t.p[1][1] - t.p[0][1], t.p[1][2] - t.p[0][2] }, w[3] = { t.p[2][0] - t.p[0][0], t.p[2][1] - t.p[0][1], t.p[2][2] - t.p[0][2] };
				const float n[3] = { u[1] * w[2] - u[2] * w[1], u[2] * w[0] - u[0] * w[2], u[0] * w[1] - u[1] * w[0] };
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (len < 1e-9f) { continue; }
				const float flip = n[0] * vn[0] + n[1] * vn[1] + n[2] * vn[2] < 0 ? -1.0f : 1.0f;	// the winding cannot be trusted: face the way the vertex normals do
				for (int k = 0; k < 3; k++) { t.n[k] = n[k] / len * flip; }
				const float tu = uv[0] * (p.def.tileX > 0 ? p.def.tileX : 1.0f), tv = uv[1] * (p.def.tileY > 0 ? p.def.tileY : 1.0f);
				t.albedo = lumaAt(alb->rgba, alb->w, alb->h, tu, tv);
				if (const uint8_t* px = texelAt(alb->rgba, alb->w, alb->h, tu, tv)) { for (int k = 0; k < 3; k++) { t.alb[k] = px[k] / 255.0f; } }
				else { for (int k = 0; k < 3; k++) { t.alb[k] = kProbeGroundAlbedo; } }
				tris.push_back(t);
			}
		}
	}
	return tris;
}
