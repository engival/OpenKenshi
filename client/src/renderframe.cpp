// The draw half (renderframe.hpp; review kenshi-k16.2.205.12.15 3.1, kenshi-k16.2.205.12.27). No App here: everything comes from the packet,
// the RenderState and the render side's own modules. The blocks not moved yet are RenderState::Legacy's entries (main.cpp).
#include "renderframe.hpp"

#include "arrows.hpp"
#include "characters.hpp"
#include "cpustage.hpp"
#include "creatorscene.hpp"
#include "cursors.hpp"
#include "inputqueue.hpp"
#include "drawstats.hpp"
#include "dev/ablate.hpp"
#include "dev/devhooks.hpp"
#include "drawlist_gpu.hpp"
#include "dust.hpp"
#include "foliage.hpp"
#include "framestats.hpp"
#include "gputimer.hpp"
#include "grass.hpp"
#include "hdr.hpp"
#include "idbuffer.hpp"
#include "interiorclip.hpp"
#include "lightvol.hpp"
#include "loadscreen.hpp"
#include "overlaylist.hpp"
#include "portraits.hpp"
#include "post.hpp"
#include "dev/lightterms.hpp"
#include "readback.hpp"
#include "reflection.hpp"
#include "renderqueue.hpp"
#include "shadow.hpp"
#include "squads.hpp"
#include "sunshafts.hpp"
#include "terrain.hpp"
#include "trails.hpp"
#include "water.hpp"
#include "windowgate.hpp"
#include "worldbuildings.hpp"
#include "worldmap.hpp"

#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_glue.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

// worldSetup: the world frame up to the sky (the uploads, the shadow pass, the portraits, the reflection, the light volumes, the mask, the scene pass
// opened); moved from main.cpp in sitting 9 (kenshi-k16.2.205.12.54), same order. Returns whether the interior mask was drawn (the particles sample it).
static bool world_setup(const FramePacket& pk, RenderState& rs)
{
	const double dt = pk.dt;
	const bool editor = pk.editor;
	const FramePacket::Scene& sc0 = pk.scene;
	const Camera& cam = pk.views[0];	// mono: view 0 only (a stereo frame loops the views here)
	float mvp[16];
	cam.mvp(mvp);
	rs.buildings.upload(pk.buildings);	// render hook 7a, before any pass (the particle instances went up just before): the building instances (its mesh and texture creates are resource jobs, rq::drain)
	GPU_BEGIN_FRAME(rs.gpu);
	framestats::fillGpu(rs.gpu, pk.width, pk.height);	// the return slot: the collected ms and the applied size (render hook 5)
	drawstats::reset();	// per-pass submit counters (kenshi-k16.2.101.1): zeroed every frame
	framestats::slot().shadowMs = 0;
	{
		// the characters' frame data (palettes, records, the instance lists of every pass; chars.prepare built it in logic_frame, kenshi-k16.2.205.12.26)
		// into the GPU buffers, once, before the first pass that draws them (kenshi-k16.2.205.8)
		CPUSTAGE_ACC("chars.upload");
		rs.chars.upload(pk.chars);
	}
	if (rs.shadow.active() && !pk.worldMap && !editor)	// the creator's Deferred_RTT pass has no shadows (rtt.hlsl)
	{
		const auto ts = std::chrono::steady_clock::now();
		const int loads = sc0.terrainLoads;
		// The world's casters first, then the characters in their own section (kenshi-k16.2.101.1): the shadow pass stays open across both.
		// With render.shadow_cache the static casters went to the cache's atlas (own pass, only the tiles whose key changed) and the live atlas gets each
		// rendering cascade's tile restored from it, then the wind-turned building parts and the characters over it (kenshi-k16.2.205.12.2).
		uint64_t sig = 0x9e3779b97f4a7c15ull;
		if (rs.shadow.cacheOn())
		{
			auto mix = [&sig](uint64_t v) { sig ^= v + 0x9e3779b97f4a7c15ull + (sig << 6) + (sig >> 2); };
			mix(rs.terrain.shadowGen());
			mix(pk.foliage.gen);
			mix(sc0.bldSet ? pk.buildings.shadowSig : 0);	// the packet's copy, from buildings.update's end (kenshi-k16.2.205.12.95)
			mix((uint64_t)(editor ? 1u : 0u) | (uint64_t)(sc0.foliageOn ? 2u : 0u) | (uint64_t)(sc0.bldSet ? 4u : 0u));
		}
		rs.shadow.resolve(sig);
		const bool cached = rs.shadow.cacheOn();
		GPU_BEGIN(rs.gpu, GpuTimer::Shadow);
		if (cached)
		{
			if (rs.shadow.anyStaticDraw())
			{
				rs.shadow.beginStaticPass();
				for (int c = 0; c < Shadow::kCascades; c++)
				{
					if (!rs.shadow.staticDraw(c)) { continue; }
					const ShadowCascade& sc = rs.shadow.cascade(c);
					rs.shadow.selectStatic(c);
					{ CPUSTAGE_ACC("shadow.terrain"); if (!editor) { rs.terrain.renderShadow(sc0.pos, sc, loads); } }
					{ CPUSTAGE_ACC("shadow.buildings"); if (sc0.bldSet) { rs.buildings.renderShadow(sc, 1); } }
					{ CPUSTAGE_ACC("shadow.foliage"); if (!editor && sc0.foliageOn && sc0.bldSet) { rs.foliage.renderShadow(pk.foliage, sc); } }
				}
				rs.shadow.endStaticPass();
			}
			rs.shadow.beginPass();
			for (int c = 0; c < Shadow::kCascades; c++)
			{
				if (!rs.shadow.renders(c)) { continue; }
				rs.shadow.restoreCascade(c);
				rs.shadow.selectTile(c);
				CPUSTAGE_ACC("shadow.buildings");
				if (sc0.bldSet && rs.buildings.renderShadow(rs.shadow.cascade(c), 2)) { rs.shadow.markDirty(c); }
			}
		}
		else
		{
			rs.shadow.beginPass();
			for (int c = 0; c < Shadow::kCascades; c++)
			{
				if (!rs.shadow.renders(c)) { continue; }
				const ShadowCascade& sc = rs.shadow.cascade(c);
				rs.shadow.selectCascade(c);
				{ CPUSTAGE_ACC("shadow.terrain"); if (!editor) { rs.terrain.renderShadow(sc0.pos, sc, loads); } }
				{ CPUSTAGE_ACC("shadow.buildings"); if (sc0.bldSet) { rs.buildings.renderShadow(sc); } }
				{ CPUSTAGE_ACC("shadow.foliage"); if (!editor && sc0.foliageOn && sc0.bldSet) { rs.foliage.renderShadow(pk.foliage, sc); } }
			}
		}
		GPU_END(rs.gpu);
		GPU_BEGIN(rs.gpu, GpuTimer::CharacterShadows);
		for (int c = 0; c < Shadow::kCascades; c++)
		{
			if (!rs.shadow.renders(c)) { continue; }
			rs.shadow.selectTile(c);	// the world's casters already cleared (or restored) this tile: no second clear
			if (ablate::on("charshadow")) { continue; }	// KCLIENT_ABL: no character shadow casters (an upper-bound measurement)
			CPUSTAGE_ACC("shadow.chars");
			if (rs.chars.renderShadow(c, rs.shadow.cascade(c)) && cached) { rs.shadow.markDirty(c); }
		}
		GPU_END(rs.gpu);
		rs.shadow.endPass();
		framestats::slot().shadowMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
	} else { rs.shadow.invalidate(); }
	CPUSTAGE("shadow.pass");
	// after the shadow pass: the portraits use the world's light and shadows (portraits.hpp); the packet's jobs (one per frame, all of them in a
	// scripted run), their results to the mailbox logic takes (render thread: Portraits; logic thread: PortraitTable)
	if (sc0.simActive) { rs.portraits.update(rs.chars, pk.portraitJobs); }
#ifdef KCLIENT_DEV_TOOLS
	dev::labRender(pk.lab);	// the animation lab: its own pass on the portrait light, outside every world pass; only while the browser is open or a script drives it
#endif

	// the water's planar reflection (reflection.hpp): the sky, then the world with the reflect-pass light (unlit albedo luma), from the mirrored camera.
	// The renderers draw what they culled for the main view (MINE: the game culls for the reflection camera; things only the reflection sees are missing).
	// kenshi-k16.2.205.11: when the water covers little of the view (Water::coverage, the plane's projected share last frame) the pass is cheapened to
	// the sky alone: the world in the reflection can only be seen through the water pixels, so below kReflSkyOn there is no room for it to show, and
	// above it the full pass runs. Hysteresis (kReflSkyOff) keeps a shoreline shimmer from flipping the content between frames; the pixels it flips
	// over are a sliver of the frame. The water key's behaviour (render.water / --no-water) is unchanged: no water, no pass.
	constexpr float kReflSkyOn = 0.01f, kReflSkyOff = 0.005f;
	const float wcov = rs.water.coverage();
	const bool fullRefl = wcov >= (rs.reflSkyOnly ? kReflSkyOn : kReflSkyOff);
	rs.reflSkyOnly = !fullRefl;
	bool reflOn = false;
	if (!editor && sc0.waterOn && !pk.worldMap && rs.water.drawn() && rs.refl.setup(cam))
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Reflection);
		const Camera& rc = rs.refl.camera();
		const float* rm = rs.refl.mvp();
		LightParams rl = pk.sky.light;
		rl.plHead[0] = 0;
		rl.plHead[1] = LightParams::kReflectPass;
		rl.plHead[2] = (float)(Reflection::kPlane - cam.pos[1]);
		sg_begin_pass(rs.refl.pass());
		rs.sky.render(pk.sky, rc.f, rc.r, rc.u, rc.fovY, rc.aspect, Reflection::kSize);
		if (fullRefl)
		{
			if (sc0.bldSet) { rs.buildings.render(rm, rl); }
			rs.terrain.renderView(rm, rl, sc0.terrainView);
			if (sc0.foliageOn && sc0.bldSet) { rs.foliage.render(pk.foliage, rm, rl); }
			if (sc0.grassOn && sc0.bldSet) { rs.grass.render(pk.grass, rm, rl, sc0.animTime); }
			rs.chars.render(rm, sc0.pos, rl);
		}
		sg_end_pass();
		GPU_END(rs.gpu);
		reflOn = true;
#ifdef KCLIENT_DEV_TOOLS
		if (cpustage::on() && framestats::drawing().frames % 30 == 0) { fprintf(stderr, "watercov: %.4f reflection %s\n", wcov, fullRefl ? "full" : "sky"); }
#endif
	}
	rs.water.setReflection(rs.refl.view(), reflOn);
	CPUSTAGE("portraits+reflection");
	rs.arrows.update(pk.arrows, sc0.pos);	// the packet's marks (logic_frame): a transient buffer write, outside every pass
	rs.post.beginScene(cam, !editor && !pk.worldMap);	// the world reads last frame's AO (post.ao)
	{
		// render.interior_light: the nearest baked buildings as volumes for the scene pass (the packet's strength = 1 - the option, so 0 = the original lighting:
		// none asked for, no bake starts)
		const float strength = sc0.interiorLight;
		const bool shafts = ((sc0.sunShafts > 0.0f || sc0.dustMotes > 0.0f || sc0.lampBeams > 0.0f) && !editor && !pk.worldMap && sc0.bldSet) || (sc0.lampOcclusion && !editor && !pk.worldMap && sc0.bldSet);	// the volumes stay live (strength 0: the lit programs read factor 1) for the shafts and the lamps
		// render.sun_bounce: needs render.interior_light below 1 (its term is part of that light); the sun direction is the shadow's (clamped to the horizon), and
		// the ground under the bake is the colour map at the camera, as the sky ambient reads it
		const bool sunBounce = sc0.sunBounce > 0.0f && strength > 0.0f && !editor && !pk.worldMap && sc0.bldSet;
		if (sunBounce && pk.sky.light.v[0][1] > 0.0f)
		{
			const float* sd = pk.sky.light.v[0];
			float ground[3] = { 0.3f, 0.3f, 0.3f };
			if (sc0.groundOk) { for (int k = 0; k < 3; k++) { ground[k] = sc0.ground[k]; } }
			if ((strength > 0.0f || shafts) && !editor && !pk.worldMap && sc0.bldSet) { rs.buildings.lightVolumes(cam.pos, 500.0, lightvol::kMax, rs.lightVols, true, sd, ground); }
		}
		else if ((strength > 0.0f || shafts) && !editor && !pk.worldMap && sc0.bldSet) { rs.buildings.lightVolumes(cam.pos, 500.0, lightvol::kMax, rs.lightVols); }
		else { rs.lightVols.clear(); }
		rs.lightVolumes.begin(cam.pos, rs.lightVols, strength, sunBounce ? sc0.sunBounce : 0.0f, WorldBuildings::lightGroundAlbedo(), (float)dt, shafts);
		auto& lv = framestats::slot().lightVols;	// the dev ops' copy (render thread writes the list; logic reads only these counts, a frame late)
		lv.volumes = rs.lightVols.size();
		lv.sunVolumes = 0;
		for (const lightvol::Volume& v : rs.lightVols) { lv.sunVolumes += v.sunNew != nullptr; }
	}
	rs.hdr.clearOpaqueDepth();
	// The interior mask (the game's rt_interiormask, materials/common/interiormask.material): the interior-mask hulls near the camera, back faces into
	// G and front faces into R, rendered with this view's mvp before the scene pass: the terrain and the grass drop the fragments inside a hull (a sunk
	// building's interior is not filled by the dune; interiorclip.inc), and the CLIP_INTERIOR particle materials (ash, rain, dust...) after the scene.
	const bool maskOn = sc0.interiorMask && sc0.bldSet && !pk.worldMap && !editor && rs.buildings.hasMask();
	if (maskOn)
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Mask);
		sg_begin_pass(rs.hdr.maskPass(pk.width, pk.height));	// the applied size: a held window change does not rebuild them (k16.2.222)
		rs.buildings.renderMask(mvp);
		sg_end_pass();
		GPU_END(rs.gpu);
		interiorclip::use(rs.hdr.maskTex());
	}
	CPUSTAGE("post.begin+mask");
	sg_begin_pass(rs.hdr.scenePass(pk.width, pk.height));	// the applied size (k16.2.222)
	if (editor) { rs.creator.drawBackdrop(); }	// the original's editor draws its sand quad, no sky (creatorscene.hpp)
	return maskOn;	// the sky: render_frame
}

// Never silent again (k16.19.6.1): once an sgl context's vertex buffer fills, sokol-gl drops every later command of the frame without a word, which is
// how the debug trails hid the HUD, minimap, name labels and windows (k16.19.6). After the frame's draws (the errors reset at sg_commit), log each
// context's state once per change. Returns this frame's flags (1 vertices full, 2 commands full, 4 uniforms full, 8 matrix stack, 16 no
// context) for the stats slot (render hook 8).
static uint32_t sgl_check(const char* which, const sgl_error_t& e, uint32_t& shown)
{
	uint32_t flags = 0;
	if (e.vertices_full) { flags |= 1; }
	if (e.commands_full) { flags |= 2; }
	if (e.uniforms_full) { flags |= 4; }
	if (e.stack_overflow || e.stack_underflow) { flags |= 8; }
	if (e.no_context) { flags |= 16; }
	if (flags == shown) { return flags; }
	shown = flags;
	if (!flags) { fprintf(stderr, "sgl: %s context: ok\n", which); return flags; }
	std::string what;
	for (const auto& [bit, text] : std::initializer_list<std::pair<uint32_t, const char*>>{ { 1, "vertices full" }, { 2, "commands full" }, { 4, "uniforms full" },
			{ 8, "matrix stack error" }, { 16, "no context" } })
	{
		if (!(flags & bit)) { continue; }
		if (!what.empty()) { what += ", "; }
		what += text;
	}
	fprintf(stderr, "sgl: %s in the %s context\n", what.c_str(), which);
	return flags;
}

// worldOverlay: the world frame in the swapchain pass render_frame opened after the exposure and the post chain: the debug overlays, the sim markers, the
// minimap / full map / HUD (the packet's overlay lists, recorded in logic: sitting 11, kenshi-k16.2.205.12.57), the UI, the load screen, the cursor, up to
// framestats::fillDraw. Moved from main.cpp in sitting 10 (kenshi-k16.2.205.12.55), same order and GPU sections.
static void world_overlay(const FramePacket& pk, RenderState& rs)
{
	const FramePacket::Overlay& ov = pk.overlay;
	GPU_BEGIN(rs.gpu, GpuTimer::Overlay);
	begin_overlay();
	const FramePacket::OverlayLists& ol = pk.overlayLists;
	if (ov.debug && !pk.worldMap && !ov.title)	// the nav overlay, the light probes, the trails: --debug views, off the full map and the title
	{
		ovl::play(ol.debug);
		if (ol.trailsOn)	// the debug trails: their own context (k16.19.6.1), at the same layer and order under the markers
		{
			rs.trails.submit(ol.trails, ol.width, ol.height);
			if (rs.trails.context().id != SG_INVALID_ID) { sgl_context_draw_layer(rs.trails.context(), 0); }
		}
	}
	if (ov.simActive && !pk.worldMap && !ov.hideUi && !ov.title)	// under the minimap
	{
		ovl::play(ol.markers);
		if (pk.labelsDrawn) { dl::gpu::replay(pk.labels); }	// with the UI the labels are record_ui's list (labelsDrawn: uiOn and this condition)
	}
	if (!ov.hideUi && !ov.title && !ov.creatorShown) { ovl::play(ol.mapHud); }	// the creator screen covers the map and the debug text
	GPU_END(rs.gpu);	// the overlay's debug views end here: the UI layer below is its own section (kenshi-k16.2.101.1)
	GPU_BEGIN(rs.gpu, GpuTimer::Ui);
	overlay_top();	// the UI windows over the debug text
	if (pk.uiDrawn) { dl::gpu::replay(pk.ui); }	// record_ui's
	if (ov.sessionLoad) { rs.loadScreen.draw(ov.sessionProgress, "streaming the world"); }	// over everything but the cursor
	if (!pk.look) { rs.cursors.draw(pk.cursor, pointer::x.load(std::memory_order_relaxed), pointer::y.load(std::memory_order_relaxed)); }
	end_overlay();
	// an sgl overflow dropped commands this frame: say so (once per change of state, k16.19.6.1), and put the flags in the stats slot (render hook 8)
	framestats::fillSgl(sgl_check("default", sgl_error(), rs.sglShown[0]),
		sgl_check("trails", rs.trails.context().id != SG_INVALID_ID ? sgl_context_error(rs.trails.context()) : sgl_error_t{}, rs.sglShown[1]),
		dl::gpu::stats().draws, dl::gpu::stats().vertices, dl::gpu::stats().missing, dl::gpu::stats().textures);
	GPU_END(rs.gpu);
	sg_end_pass();
	framestats::fillDraw();	// the frame's last pass has ended: the per-pass counters are complete (render hook 5); publish below
}

// gpuLog: KCLIENT_GPULOG's line, after the commit (sitting 10).
static void gpu_log(const FramePacket& pk)
{
#if KCLIENT_PROFILE
	// KCLIENT_GPULOG (kenshi-k16.2.205.7): the GPU sections' ms (collected two frames late) and the per-pass submit counters on this frame's
	// stages line, so a run's distribution per pass comes out of the same log as the CPU split. Needs KCLIENT_STAGES set.
#ifdef KCLIENT_DEV_TOOLS	// cpustage is dev-only
	static const bool gpuLog = std::getenv("KCLIENT_GPULOG") != nullptr;
	if (gpuLog)
	{
		char b[160];
		std::string g;
		snprintf(b, sizeof b, " | chars %zu", pk.chars.count);
		g += b;
		snprintf(b, sizeof b, " | hour %.2f", pk.sky.hours);
		g += b;
		for (int s = 0; s < GpuTimer::kSections; s++)
		{
			const drawstats::Pass& p = framestats::drawing().passes[s];
			snprintf(b, sizeof b, " | %s g%.2f d%u i%u t%u", GpuTimer::name(s), framestats::drawing().gpuMs[s], p.draws, p.instances, p.triangles);
			g += b;
		}
		cpustage::extra(g);
	}
#endif
#endif
}

// The early paths (sitting 12, kenshi-k16.2.205.12.60): the frames with no world. Each ends as the world frame does: the readback requests, the commit and
// the frame count. They read the packet's early block and the RenderState only. The viewer's (kView) is dev::viewDraw, in dev/viewmesh.cpp.

// A frame with no world: black (the title's own frame), the UI and the session loading screen when one is up (kenshi-k16.2.112.1).
static void frame_blank(const FramePacket& pk, RenderState& rs)
{
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0, 0, 0, 1 };
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);
	begin_overlay();
	if (pk.uiDrawn) { dl::gpu::replay(pk.ui); }	// record_ui's (the creator's panels wait for their scene, kenshi-k16.2.133.1)
	if (pk.early.sessionLoad) { rs.loadScreen.draw(0.0f, "waiting for the world"); }	// over everything but the cursor
	if (!pk.look) { rs.cursors.draw(pk.cursor, pointer::x.load(std::memory_order_relaxed), pointer::y.load(std::memory_order_relaxed)); }
	end_overlay();
	sg_end_pass();
	readback::serve();	// logic_tail's requests for this frame (the script's shots)
	sg_commit();
	framestats::endFrame();
}

static void draw_loading(const FramePacket& pk, RenderState& rs)
{
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0.03f, 0.03f, 0.04f, 1.0f };
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);
	begin_overlay();
	rs.loadScreen.draw(pk.early.loadFrac, pk.early.loadStep.c_str());
	end_overlay();
	sg_end_pass();
	readback::serve();	// the --loading-shot
	sg_commit();
	framestats::endFrame();	// the frame count (the render side's; logic mirrors it into app.frames)
}

static void draw_title(const FramePacket& pk, RenderState& rs)
{
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0, 0, 0, 1 };
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);
	begin_overlay();
	if (pk.uiDrawn) { dl::gpu::replay(pk.ui); }	// record_ui's
	rs.cursors.draw(pk.cursor, pointer::x.load(std::memory_order_relaxed), pointer::y.load(std::memory_order_relaxed));
	end_overlay();
	sg_end_pass();
	readback::serve();	// logic_tail's requests (the script's shots, F12 / screenshot_delayed on the title, k16.2.146)
	sg_commit();
	framestats::endFrame();	// the frame count (the render side's; logic mirrors it into app.frames)
}

void render_frame(const FramePacket& pk, RenderState& rs)
{
	const view::RenderSize viewSize(pk.width, pk.height);	// every view::width()/height() below is the packet's applied size, never logic's copy (rule 10; kenshi-k16.2.205.12.108)
	rq::drain(pk.serial);	// the resource jobs queued for this packet, before any pass (docs/RENDER_THREAD.md §7.2): creates, frees, one-shot paints
	CPUSTAGE("rq.drain");	// its own stage, ahead of the shadow pass's (kenshi-k16.2.205.12.74); each job's time by its tag is the +<tag> accumulators
	readback::drawing(pk.serial);	// readback::serve serves this packet's requests (and any older)
	switch (pk.path)
	{
	case FramePath::kBlank: case FramePath::kAwait: frame_blank(pk, rs); return;
	case FramePath::kLoading: draw_loading(pk, rs); return;
	case FramePath::kView: dev::viewDraw(pk, rs.gpu); return;	// the mesh viewer (dev/viewmesh.cpp, dev builds only; render hook 9): the packet alone
	case FramePath::kTitle: draw_title(pk, rs); return;
	case FramePath::kWorld: break;
	}
	const auto r0 = std::chrono::steady_clock::now();	// the draw's own CPU span (FrameStats::cpu); logic's is pk.logicMs
	const Camera& cam = pk.views[0];	// mono: view 0 only (a stereo frame loops the views here)
	// the renderers' uploads from the packet (render hook 7a), before any pass: the particle instances (the grass tile buffers are resource jobs, rq::drain);
	// the building instances' upload is in worldSetup, the first thing it does
	rs.particles.upload(pk.particles);
	const bool maskOn = world_setup(pk, rs);
	// The sky (sky.hpp), first in the scene pass worldSetup opened (after the creator's backdrop; the creator and the full world map draw none),
	// from the packet's sky block.
	if (!pk.worldMap && !pk.editor)
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Sky);
		rs.sky.render(pk.sky, cam.f, cam.r, cam.u, cam.fovY, cam.aspect, cam.height);
		GPU_END(rs.gpu);
	}
	// The opaque scene's first renderers, by cost: the buildings first (they cover much of the terrain in towns, and the terrain pixel shader is the most
	// expensive, so the terrain behind them is rejected by the early depth test), then the terrain; the foliage and the grass follow.
	float mvp[16];
	cam.mvp(mvp);
	LightParams light = pk.sky.light;	// + the building lights, for every lit world surface (the game's deferred pass lights them all)
	// makeWet's underwater term (lighting.inc wet_under): off in the creator's scene, which stands far below the world's water plane (never underwater there)
	light.plHead[2] = pk.editor ? -1.0e6f : (float)(Reflection::kPlane - cam.pos[1]);
	if (pk.scene.bldSet) { WorldBuildings::fillLights(pk.buildings, light, pk.scene.lampOcclusion ? &rs.lightVolumes : nullptr); }
	if (pk.editor) { rs.creator.light(light); }
	GPU_BEGIN(rs.gpu, GpuTimer::Buildings);
	if (pk.scene.bldSet && !pk.worldMap) { rs.buildings.render(mvp, light); }
	CPUSTAGE("draw.buildings");
	GPU_END(rs.gpu);
	if (!pk.worldMap)
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Terrain);
		if (!pk.editor) { rs.terrain.render(pk.scene.pos, mvp, light, pk.scene.terrainLoads, pk.scene.terrainView); }
		GPU_END(rs.gpu);
	}
	// The foliage, then the grass (the opaque parts; the foliage's fade band stays in the legacy scene, after the water, as does its shadow)
	GPU_BEGIN(rs.gpu, GpuTimer::Foliage);
	if (!pk.editor && pk.scene.foliageOn && pk.scene.bldSet && !pk.worldMap) { rs.foliage.render(pk.foliage, mvp, light); }
	CPUSTAGE("draw.foliage");
	GPU_END(rs.gpu);
	GPU_BEGIN(rs.gpu, GpuTimer::Grass);
	if (!pk.editor && pk.scene.grassOn && pk.scene.bldSet && !pk.worldMap) { rs.grass.render(pk.grass, mvp, light, pk.scene.animTime); }
	CPUSTAGE("draw.grass");
	GPU_END(rs.gpu);
	// The characters, their items, the squad pillars and the stealth detection arrows (sitting 7, kenshi-k16.2.205.12.51), before the water: its depth
	// copy includes them. The characters draw the packet's frame data (pk.chars, uploaded in worldSetup), the arrows the instances worldSetup wrote from pk.arrows.
	GPU_BEGIN(rs.gpu, GpuTimer::Characters);
	if (!pk.worldMap) { rs.chars.render(mvp, pk.scene.pos, light); }
	CPUSTAGE("draw.chars");
	GPU_END(rs.gpu);
	GPU_BEGIN(rs.gpu, GpuTimer::Items);
	if (!pk.worldMap) { rs.chars.renderItems(mvp, pk.scene.pos, light); }
	CPUSTAGE("draw.items");
	GPU_END(rs.gpu);
	if (!pk.worldMap) { rs.squads.render(mvp, pk.scene.pos, pk.scene.squads); }
	if (!pk.worldMap) { rs.arrows.render(mvp); }
	// The water (water.hpp), then the foliage fade band blended over everything opaque (untimed: a section runs once a frame).
	GPU_BEGIN(rs.gpu, GpuTimer::Water);
	if (!pk.editor && pk.scene.waterOn && !pk.worldMap)
	{
		// the water's opacity depth reads the opaque scene depth (water.hlsl:155), so it is copied first and the scene pass continues
		sg_view waterDepth = {};
		if (rs.water.drawn())
		{
			sg_end_pass();
			rs.hdr.copyDepthForWater();
			sg_begin_pass(rs.hdr.continuePass());
			waterDepth = rs.hdr.opaqueCopyTex();
		}
		rs.water.render(rs.terrain, mvp, pk.sky.light, pk.sky.sunColour, pk.scene.animTime, pk.scene.rain, pk.scene.scripted, waterDepth, cam.nearZ, cam.farZ);
	}
	GPU_END(rs.gpu);
	if (!pk.editor && pk.scene.foliageOn && pk.scene.bldSet && !pk.worldMap) { rs.foliage.renderFadeBand(pk.foliage, mvp, light); }
	// The glass, the cursor id pass, the AO, the sun shafts and the dust (sitting 8, kenshi-k16.2.205.12.53), up to the fog volumes.
	const bool peek = pk.scene.bldSet && !pk.worldMap;
	// While peeked glass draws, the scene depth is kept first (Hdr::copyOpaqueDepth, after the foliage fade band, so the band reaches the particles / fog / AO / haze in every frame as
	// before): the see-through glass writes its nearest surface into the depth and must not hide what is behind it from those passes. The cursor ID pass below still reads the glass depth.
	if (peek && rs.buildings.hasGlass())
	{
		sg_end_pass();
		rs.hdr.copyOpaqueDepth();
		sg_begin_pass(rs.hdr.continuePass());
	}
	if (peek) { rs.buildings.renderGlass(mvp, light); }	// the peeked parts, blended over everything opaque
	sg_end_pass();
	CPUSTAGE("draw.water+glass+misc");
	interiorclip::release();
	if (peek)	// the cursor ID pass (update_peek)
	{
		// The applied size, as the scene depth it loads; the pointer is in window pixels, and the composite scales the applied image onto the window
		// (k16.2.222: while a change is held, the id image is the applied size, so the window pixel maps onto its pixel). The hit is consumed by logic
		// from framestats' cursor slot (FrameStats::cursorHit, taken at the next frame's start): a frame late, as before.
		const int idW = pk.width, idH = pk.height;
		sg_begin_pass(rs.idbuf.pass(rs.hdr.depthAtt(), idW, idH));
		rs.buildings.renderIds(mvp);
		sg_end_pass();
		const double wx = pk.scene.windowW > 0 ? (double)idW / (double)pk.scene.windowW : 1.0, wy = pk.scene.windowH > 0 ? (double)idH / (double)pk.scene.windowH : 1.0;
		framestats::publishCursor(rs.idbuf.read((int)(pk.scene.mouse[0] * wx), (int)(pk.scene.mouse[1] * wy), pk.scene.pos));
	}
	CPUSTAGE("idpass");
	GPU_BEGIN(rs.gpu, GpuTimer::Ao);
	rs.post.endScene(cam, rs.hdr.sceneDepthTex(), rs.hdr.sceneTex(), 1.0f);	// the scene is linear now: no baked exposure to divide out
	GPU_END(rs.gpu);
	// render.sun_shafts (sunshafts.hpp): beams in the air of the live probe volumes, added to the HDR scene; the volumes end after it
	// render.lamp_beams rides the same pass: the lamps' cones are marched in the same loop (the lights are the scene's list, with their visibility codes)
	const float sunScale = dev::lightOff(pk.lightOff, dev::kLtSunShafts) ? 0.0f : pk.scene.sunShafts;
	const float lampScale = dev::lightOff(pk.lightOff, dev::kLtLampBeams) ? 0.0f : pk.scene.lampBeams;
	if ((sunScale > 0.0f || lampScale > 0.0f) && !pk.editor && !pk.worldMap)
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Shafts);
		sg_begin_pass(rs.hdr.fogPass());
		rs.sunShafts.render(cam, light, rs.shadow, sunScale, lampScale, (float)pk.sky.sunDir[1], rs.hdr.sceneDepthTex(), pk.scene.sunShaftsDebug);
		sg_end_pass();
		GPU_END(rs.gpu);
	}
	// render.dust_motes (dust.hpp): motes in the air of the nearest live volume, lit only inside a sunbeam, added to the HDR scene
	if (pk.scene.dustMotes > 0.0f && !pk.editor && !pk.worldMap && !dev::lightOff(pk.lightOff, dev::kLtDust))
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Shafts);
		sg_begin_pass(rs.hdr.fogPass());
		rs.dust.render(cam, pk.sky.light, rs.shadow, pk.scene.dustMotes, lampScale, (float)pk.sky.sunDir[1], pk.scene.animTime, rs.hdr.sceneDepthTex());
		sg_end_pass();
		GPU_END(rs.gpu);
	}
	rs.lightVolumes.end();
	CPUSTAGE("post.endScene+shafts");

	// Fog volumes (fogvolume.hpp): their own pass over the finished scene, because the scene depth is sampled; lit by the packet's sky light.
	framestats::slot().fogDrawn = 0;
	if (rs.fogvol.enabled && !pk.worldMap && !dev::lightOff(pk.lightOff, dev::kLtFogVolumes))
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Fog);
		sg_begin_pass(rs.hdr.fogPass());
		framestats::slot().fogDrawn = rs.fogvol.render(cam, pk.sky.light, (float)pk.sky.sunDir[1], rs.hdr.sceneDepthTex());
		sg_end_pass();
		GPU_END(rs.gpu);
	}

	// Weather particles (particles.hpp): their own pass after the fog volumes, the scene depth sampled; the CLIP_INTERIOR materials (ash, rain,
	// dust...) drop indoors by the interior mask when worldScene drew it.
	if (pk.particles.count > 0)
	{
		GPU_BEGIN(rs.gpu, GpuTimer::Particles);
		sg_begin_pass(rs.hdr.particlePass());
		rs.particles.render(pk.particles, cam, rs.hdr.sceneDepthTex(), maskOn ? rs.hdr.maskTex() : rs.particles.noMaskView());
		sg_end_pass();
		GPU_END(rs.gpu);
	}
	CPUSTAGE("fog+particles.pass");

	// HDR auto exposure (hdr.hpp): luminance chain + adaptation, then the exposed scene into the swapchain; the overlay on top of it is never exposed.
	// Scripted runs jump to the current luminance every frame (deterministic shots); interactive runs adapt at the game's 0.5 / s.
	Hdr::Frame hf;
	hf.bakedExposure = 1.0f;	// unexposed scene (x1): no CPU exposure baked into the world shaders
	hf.key = 0.55f;		// the game's EXPOSURE_KEY (the original's display: a hard clip, no curve to lift the mid tones)
	hf.minLum = pk.sky.minLum;
	hf.maxLum = pk.sky.maxLum;
	hf.dt = (float)std::fmin(pk.dt, 0.25);
	if (pk.post.eyeAdapt) { hf.floorLum = 0.02f; hf.eyeExp = 0.85f; }
	hf.instant = pk.post.scriptedJump || framestats::drawing().frames == 0 || !rs.adaptedOnce;
	hf.fixedExposure = dev::lightOff(pk.lightOff, dev::kLtExposure) ? 1.0f : pk.post.fixedExposure;	// the light debugger's exposure switch: no auto exposure, gain 1
	rs.post.debugOff(pk.lightOff);
	// Heat haze (hdr.hpp; live values: kenshi-inspect findings/heathaze.md). gameTime is game hours (frozen while paused; one wobble = 0.01 h = 36 game s); the
	// shader only uses frac(gameTime x 100), so the total hours are wrapped at 10 to keep float precision. farClip: the main camera's, 50000.
	hf.heatHaze = pk.worldMap ? 0.0f : pk.sky.heatHaze;
	hf.gameTime = (float)std::fmod(pk.sky.day * 24.0 + pk.sky.hours, 10.0);
	hf.nearZ = cam.nearZ;
	hf.farZ = cam.farZ;
	hf.farClip = 50000.0f;
	hf.tanY = cam.tanY;
	hf.aspect = cam.aspect;
	GPU_BEGIN(rs.gpu, GpuTimer::Exposure);
	if (!pk.editor) { rs.hdr.resolve(hf); rs.adaptedOnce = true; }	// the creator leaves the world's adaptation untouched
	GPU_END(rs.gpu);
	GPU_BEGIN(rs.gpu, GpuTimer::Post);	// the composite and the post chain (the composite alone while every post.* effect is off)
	rs.post.render(rs.hdr, hf, pk.width, pk.height);	// optional post effects (post.hpp); the overlay below never gets them
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);
	rs.post.present(rs.hdr, hf);
	GPU_END(rs.gpu);
	CPUSTAGE("exposure+post");
	world_overlay(pk, rs);
	CPUSTAGE("overlay+ui.draw");
	// The frame's CPU ms (review 3.1: split into logic ms and render ms): logic's half from the packet plus the draw's up to the overlay's end, as
	// the old cpu0 span measured on one thread. A blocking load is not a frame. Into the stats slot before publish: logic reads it from its copy.
	const double cpuMs = pk.logicMs + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
	framestats::fillCpu(cpuMs);
	framestats::publish(pk.serial);	// the copy logic takes at its next frame start, stamped (kenshi-k16.2.205.12.24)
	readback::serve();	// logic_tail's requests for this frame: the script's shots and probes, F12, the burst's and the scene test's shots
	CPUSTAGE("script+shots");
	sg_commit();
	framestats::endFrame();
	gpu_log(pk);
	CPUSTAGE("commit");
	CPUSTAGE_END(cpuMs);
}
