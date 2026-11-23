#pragma once

// The draw half in its own translation unit (review kenshi-k16.2.205.12.15 3.1, kenshi-k16.2.205.12.27; docs/RENDER_THREAD.md 7): render_frame
// draws one FramePacket with the render-owned objects of a RenderState. renderframe.cpp cannot see main.cpp's App, so whatever it draws comes from
// the packet or the state: a renderer that still reads app. cannot move in without a compile error, and "every renderer moved" can be told from
// "three missed".
//
// Until every renderer had moved, the blocks of render_frame's old body that still read app. stayed in main.cpp behind RenderState::Legacy, one
// entry per block, named by what it draws. Each sitting moved one renderer out of them into renderframe.cpp (its app. inputs into the packet,
// its objects into RenderState); Legacy is gone (sitting 12): the draw half reads no logic state.
//   moved:  the weather particles (upload and pass), the frame's end (CPU stats, publish, readback::serve, commit, the frame count) (sitting 1,
//           kenshi-k16.2.205.12.27); the sky pass and the fog volumes, on the packet's sky block (sitting 2, kenshi-k16.2.205.12.45); the exposure and the post chain up to the swapchain pass
//           (sitting 3, kenshi-k16.2.205.12.47: the luminance range, heat haze and clock in the sky block, the options in the post block); the
//           buildings' and the terrain's opaque draw (sitting 4, kenshi-k16.2.205.12.48: the scene block, the lit LightParams handed on to the legacy scene);
//           the foliage's and the grass's opaque draw (sitting 5, kenshi-k16.2.205.12.49: foliageOn, grassOn and the grass clock in the scene block; the building lights
//           are logic's copy in WorldBuildings::FrameData, packed at buildings.update);
//           the water and the foliage fade band (sitting 6, kenshi-k16.2.205.12.50: waterOn, rain and scripted in the scene block; they draw after
//           characters, items, squads and arrows, which the water's depth copy must include); the characters', items', squads' and detection
//           arrows' draw, between the grass and the water (sitting 7, kenshi-k16.2.205.12.51: the arrows' marks and the squads' draw count in the
//           packet; the characters' frame data, chars.prepare, is logic's since kenshi-k16.2.205.12.26: pk.chars, uploaded in worldSetup); the glass, the cursor id pass, the AO, the
//           sun shafts and the dust (sitting 8, kenshi-k16.2.205.12.53: the mouse and the two switches in the scene block; the id pass's hit goes to
//           framestats' cursor slot, which logic reads a frame late).
//           worldSetup (sitting 9, kenshi-k16.2.205.12.54: the uploads, chars.upload, the shadow pass, portraits, the reflection, the arrows' upload, the light volumes
//           and the interior mask; the scene packet's simActive, interiorLight, sunBounce, interiorMask, ground and the window size; shadowMs goes back in framestats);
//           worldOverlay (sitting 10, kenshi-k16.2.205.12.55: begin_overlay, the UI replay, the load screen, the cursor at scene.mouse, the sgl checks,
//           end_overlay and fillDraw; the packet's overlay block holds the old conditions; the load screen, cursors, trails and sglShown are RenderState's)
//           and KCLIENT_GPULOG's line (the characters' count from pk.chars, the hour from pk.sky); the overlay's debug views (nav overlay, light probes,
//           trails), sim markers (with the labels) and minimap / full map / HUD (sitting 11, kenshi-k16.2.205.12.57: recorded in logic_frame through
//           ovl:: into the packet's overlay lists, projected with the packet's camera; world_overlay plays them, the trails into their own context).
//           the early paths (sitting 12, kenshi-k16.2.205.12.60: blank / await, loading, title; the packet's early block holds the mouse, the load
//           progress and step and the session load flag; the mesh viewer's draw is dev::viewDraw (dev builds only, render hook 9, kenshi-k16.2.205.12.65),
//           from the early block's viewPos and viewText, which dev::viewFrame records in logic). RenderState::Legacy is gone: nothing in render_frame's
//           draw half reads app. now.
#include <chrono>

#include "render/arrows.hpp"
#include "render/roadribbons.hpp"
#include "render/camera.hpp"
#include "characters/characters.hpp"
#include "render/drawlist.hpp"
#include "world/foliage.hpp"
#include "world/grass.hpp"
#include "render/overlaylist.hpp"
#include "render/fogvolume.hpp"
#include "render/particles.hpp"
#include "characters/portraits.hpp"
#include "render/sky.hpp"
#include "world/worldbuildings.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "frame/labframe.hpp"
#endif

#include <cstdint>
#include <string>
#include <vector>

class Cursors;
class LoadScreen;
class Trails;
class CreatorScene;
class Dust;
class GpuTimer;
class Hdr;
class IdBuffer;
class Portraits;
class Post;
class Reflection;
class Shadow;
class Squads;
class SunShafts;
class Terrain;
class Water;
class WorldBuildings;
namespace lightvol { class Volumes; struct Volume; }

// The frame packet (render hook 6, kenshi-k16.2.205.12.13; HOOKS.md 6): logic_frame fills it, render_frame draws from it. path is the frame's kind: one of
// the five early paths or the world. The fields are the values the draw reads from the logic half (the world's locals, the viewer's mvp); hook 7 adds the
// renderers' own (hook 7a: grass, particles). Two slots for the render thread; one thread uses slot 0 only.
enum class FramePath { kBlank, kAwait, kLoading, kView, kTitle, kWorld };
struct FramePacket
{
	FramePath path = FramePath::kBlank;
	uint64_t serial = 0;	// logic's count of the packets it filled (rq::beginPacket): the resource jobs queued while it was filled carry it
	double dt = 0;		// the world's frame step (logic_frame's dt)
	bool editor = false;	// the creator's scene is shown
	// The views the frame draws (review kenshi-k16.2.205.12.15 §6, kenshi-k16.2.205.12.30): an array so a stereo (OpenXR) frame carries one Camera per
	// eye without reshaping the packet. Today viewCount is 1 on kWorld and kView and every renderer reads views[0]; the draw half builds each view's mvp
	// from its Camera (Camera::mvp), so no mono mvp lives in the packet. kWorld: current_camera; kView: dev::viewFrame's mesh camera.
	static constexpr int kMaxViews = 2;
	Camera views[kMaxViews] = {};
	int viewCount = 0;
	// The world frame's logic CPU ms (review 3.1: a CPU span that starts in logic and ends in render means nothing on two threads): from the world
	// path's start to the end of logic_frame, measured by logic. The draw adds its own ms to it for FrameStats::cpu. 0 on the other paths.
	double logicMs = 0;
	std::chrono::steady_clock::time_point logicStart;	// that span's start (logic writes it; the dev split line reads it from the packet)
	// The applied window size the packet was built for (windowgate::width/height, taken at logic's frame start; RENDER_THREAD.md 7.1 rule 10,
	// kenshi-k16.2.205.12.23): the draw sizes the HDR, post and id targets from it, never from the live window. Written on every path.
	int width = 0, height = 0;
	// the renderers' frame data (render hook 7a, kenshi-k16.2.205.12.14): prepare() fills it in logic_frame, upload() and render() read it
	Grass::FrameData grass;		// grass.prepare (when it ran: a slot it did not refill keeps its last list); its buffer creates and frees are resource jobs
	Shadow::FrameData shadow;	// Shadow::snapshot at the end of the frame's first half: the pass, the cache and every receiver read this, never the Shadow (kenshi-k16.2.205.12.113)
	Particles::FrameData particles;	// cleared when the particles are not prepared this frame
	WorldBuildings::FrameData buildings;	// buildings.update: the frame's instances (cleared when it does not run); upload() writes them into the transient buffer
	// chars.prepare (kenshi-k16.2.205.12.26): the palettes, part records and per-pass instance lists, built at the end of logic_frame on the world path
	// (cleared on every other path); chars.upload writes them in worldSetup. Values only: the draw reads no character's live pose.
	Characters::FrameData chars;
	Foliage::FrameData foliage;	// foliage.prepare (hook 7b: the draw and shadow lists, the cell generation; a slot it did not refill keeps its last lists); its buffer creates and frees are resource jobs
	// The cursor (render hook 8, kenshi-k16.2.205.12.28): logic's hover shape (hover_cursor -> Cursors::set) and mouse look, set at the end of
	// logic_frame; the draw puts it at the newest mouse position (pointer::x / y, inputqueue.hpp: stored by queue_event on the sapp thread) so it gains no frame of lag, and a long logic frame does not freeze it.
	std::string cursor;
	bool look = false;	// mouse look: the world paths draw no cursor (the title does)
	// The UI (render hook 8's host half, kenshi-k16.2.205.12.39; DESIGN §6): record_ui fills both lists in logic at the end of logic_frame, their texture
	// uploads go out as one frame-lane job of the packet (dl::gpu::apply), and the draw replays them (dl::gpu::replay). ui: kui's windows (ui::record),
	// replayed where ui::draw ran (frame_blank, draw_title, the world's tail); labels: the world's name and item tags, the selection box and the debug
	// portrait strip, built from the packet's camera and replayed in draw_sim_markers. The flags say a list was recorded (its replay runs; the old
	// draws' conditions, evaluated in logic).
	dl::List ui, labels;
	bool uiDrawn = false, labelsDrawn = false;
	// The sky block (kenshi-k16.2.205.12.45): the sky's light, sun, clock, cloud and wind values (Sky::frame(), at the end of logic_frame, after the
	// frame's last change to the sky: the script's time ops included), read by the sky pass, the fog volumes and the legacy passes that light with the
	// sky. kWorld only.
	SkyFrame sky;
	uint32_t lightOff = 0;	// the light debugger's mask of switched-off terms (knobs/lightterms.hpp), set with the sky block; also in sky.light.plHead[3] for the shaders. 0 in a release build
	// The post block (kenshi-k16.2.205.12.47): the exposure's and the post chain's options, which are logic's (config, command line), set with the sky
	// block. The luminance range, the heat haze and the clock are in `sky`. kWorld only.
	struct Post
	{
		bool eyeAdapt = false;		// post.eye_adapt (kEyeAdapt)
		bool scriptedJump = false;	// a scripted run without --adapt: the adaptation jumps to the current luminance every frame
		float fixedExposure = 0;	// --exposure N (the creator: pinned to 1)
	} post;
	// The scene block (kenshi-k16.2.205.12.48): the values the buildings' and the terrain's draw read from logic's App.
	struct Scene
	{
		bool bldSet = false;		// WorldBuildings::setData done (the building set exists, possibly empty)
		double pos[3] = {};		// app.pos: the terrain's camera-relative origin
		bool foliageOn = false, grassOn = false;	// app.foliageOn / app.grassOn (initialised at the world's load)
		double animTime = 0;		// the grass sway clock: sim_time() (animTime; the scene test's fixed time)
		bool waterOn = false;		// app.opt.water (--no-water off) and its draw inputs, sitting 6 (kenshi-k16.2.205.12.50): the water's and the fade band's draw
		bool scripted = false;		// app.scripted(): the water waits for its tile loads
		float rain = 0;			// the rain the water turns into rainAmount: the scene's rain=, else the weather's blended intensity
		int terrainLoads = 0;		// Terrain::render's tile-load budget for the frame: -1 unbounded (scripted runs), else kLoadsPerFrame
		int terrainView = 0;		// Terrain::viewMode (F2, and the loads' fallbacks): the draws read this copy (kenshi-k16.2.205.12.95)
		int squads = 0;			// the squad pillars drawn (sitting 7, kenshi-k16.2.205.12.51): Squads::drawCount(), 0 while the overlay is off
		// sitting 8 (kenshi-k16.2.205.12.53): the glass, the id pass, the AO, the shafts and the dust
		double mouse[2] = {};		// app.mouseX / mouseY at the end of logic_frame: the id pass reads the pixel under it (its readback is consumed a frame later, as before)
		float sunShafts = 0, dustMotes = 0, lampBeams = 0;	// render.sun_shafts / render.dust_motes / render.lamp_beams (0 = off)
		int sunShaftsDebug = 0;		// render.sun_shafts_debug (dev tools only)
		int windowW = 0, windowH = 0;	// the live window the pointer is in (view::width/height at logic_frame's end), with `mouse`: the id pass scales applied size / window
		// sitting 9 (kenshi-k16.2.205.12.54): worldSetup, the shadow pass, the reflection and the interior mask
		bool simActive = false;		// app.sim.active(): the portraits render (one a frame, all of them in a scripted run)
		float interiorLight = 0, sunBounce = 0;	// the effect strength (render.interior_light 1 - its value, render.sun_bounce its value; 0 = off)
		bool lampOcclusion = false;	// render.lamp_occlusion (kenshi-jj3.9.1): the light volumes stay live for the lamps, the lights' visibility codes are read from them
		bool interiorMask = true;	// render.interior_mask
		bool groundOk = false;		// Sky::hasGround: the colour map under the camera; ground is its albedo (the sun bounce's ground)
		float ground[3] = {};
	} scene;
	// The overlay block (sitting 10, kenshi-k16.2.205.12.55): the world overlay's old conditions, evaluated at the end of logic_frame. kWorld only.
	struct Overlay
	{
		bool debug = false;		// --debug: the nav overlay, light probes, trails, minimap and HUD
		bool hideUi = false;		// toggle_bar (F7)
		bool title = false;		// the title screen is shown over the world
		bool creatorShown = false;	// the creator screen covers the map and the debug text
		bool simActive = false;		// a live sim: the sim markers draw
		bool sessionLoad = false;	// the session load screen is up (drawn over everything but the cursor)
		float sessionProgress = 0;	// 1 - pending / peak, its bar
	} overlay;
	// The overlay's lists (sitting 11, kenshi-k16.2.205.12.57; overlaylist.hpp): record_overlay fills them at the end of logic_frame, after the overlay
	// block, each under the condition world_overlay plays it with (the old draws' conditions); cleared every packet. Their vertices are pixels of a
	// width x height window, the 3D marks projected with views[0] at near 1, far 1000 (main.cpp overlay_camera): no camera travels with them.
	struct OverlayLists
	{
		ovl::List debug;	// the --debug views: the nav overlay, the light probes (the default context)
		ovl::List trails;	// the debug trails (Trails::draw): played into the trails' own context (Trails::submit)
		bool trailsOn = false;	// trails recorded: their context is drawn at its layer
		ovl::List markers;	// the sim markers and, without the UI, the labels; with the UI the `labels` list is replayed right after it
		ovl::List mapHud;	// the minimap / full map and the --debug HUD text
		float width = 0, height = 0;	// view::widthf / heightf when recorded: the trails' viewport and ortho
	} overlayLists;
	// The stealth detection arrows (sitting 7, kenshi-k16.2.205.12.51): one mark per DETECTED_BY entry (main.cpp stealth_arrow_marks, from the sim
	// view's decoder and drawn feet), taken at the end of logic_frame; empty off the world path and on the full world map. worldSetup writes them
	// into the arrows' instance buffer (Arrows::update, outside every pass), renderframe.cpp draws them.
	std::vector<Arrows::Mark> arrows;
	// The road overlay's ribbons (kenshi-qjzb; debug.roads, dev builds): the cached strip mesh by pointer and its look; empty (null mesh) when off. Drawn in the scene pass.
	roadribbons::Frame roads;
	// The portraits to draw this frame (kenshi-k16.2.205.12.56): PortraitTable::send at the fill (kWorld with a live sim; one, all queued in a
	// scripted run), each with its atlas cell, its pose's camera and frame data (Portraits::pose, logic, kenshi-k16.2.205.12.67). worldSetup's
	// Portraits::update uploads and draws them and posts the results that logic takes at its next frame.
	std::vector<PortraitJob> portraitJobs;
#ifdef KCLIENT_DEV_TOOLS
	dev::LabFrame lab;	// the animation lab's draw (dev::labFrame at the fill, every packet; lab.draw is the flag): render reads only this copy
#endif
	bool worldMap = false;	// the full world map is shown (app.wmap.full at logic_frame's end): no sky, no world passes
	// The early paths' block (sitting 12, kenshi-k16.2.205.12.60; render hook 9, kenshi-k16.2.205.12.65): what frame_blank, draw_loading and draw_title
	// read from App, set at the end of logic_frame on every path; and in dev builds the mesh viewer's camera position and its report panel and text,
	// recorded through ovl:: by dev::viewFrame and played by dev::viewDraw (the render table's kView).
	struct Early
	{
		double mouse[2] = {};		// app.mouseX / mouseY: the cursor (the title and the blank frame draw it)
		bool sessionLoad = false;	// app.sessionLoad: the blank frame's session loading screen ("waiting for the world")
		float loadFrac = 0;		// kLoading: loadIdx / loadSteps.size()
		std::string loadStep;		// kLoading: the step's name, or "starting"
#ifdef KCLIENT_DEV_TOOLS
		double viewPos[3] = {};		// kView: the camera position in model space (the viewer's MeshView::render cam)
		ovl::List viewText;		// kView: the report panel and lines
#endif
	} early;
};

// What render_frame draws with besides the packet: the render-owned objects (references into main.cpp's App until their ownership moves here).
// main.cpp builds one and passes it on every draw. The mesh viewer's mesh is not here: it is dev/viewmesh.cpp's (dev builds only), drawn by dev::viewDraw.
struct RenderState
{
	GpuTimer& gpu;		// the GPU section timer (GPU_BEGIN / GPU_END)
	Hdr& hdr;		// the HDR targets: the scene depth and the interior mask the particles sample
	Particles& particles;	// upload() / render() only: prepare() is logic's (render hook 7a)
	Sky& sky;		// render() only (the packet's sky block); update() and frame() are logic's
	FogVolumes& fogvol;	// render() only (load and init at the world's load)
	Post& post;		// the post chain: render() / present() (beginScene / endScene stay in the world passes)
	WorldBuildings& buildings;	// render() (the packet's FrameData is its upload and its light copy); update() is logic's
	Terrain& terrain;	// render() only
	Foliage& foliage;	// render() and renderFadeBand() (the packet's FrameData); prepare() and renderShadow() logic's / the shadow pass's
	Grass& grass;		// render() only (the packet's FrameData)
	Water& water;		// render() / drawn() only (the packet's scene block: waterOn, rain, scripted, animTime); init and shutdown are logic's
	// upload() (worldSetup), renderShadow() / render() / renderItems(): they draw the packet's frame data (pk.chars); update() and prepare() are logic's
	// (kenshi-k16.2.205.12.26). Portraits::update's renderPortrait draws the packet's portrait jobs' frame data (posed in logic, .12.67).
	Characters& chars;
	const Squads& squads;	// render() only, with the packet's count (scene.squads); load / setLive / on are logic's
	Arrows& arrows;		// render() only: the instances worldSetup wrote from the packet's marks (pk.arrows)
	// sitting 8: the glass, id pass, AO, sun shafts and dust draw here. idbuf: pass() / read(); shadow: the shafts and dust read its cascades (the
	// shadow pass is worldSetup's); sunShafts, dust: render() only; lightVolumes: end() only (begin() is worldSetup's)
	IdBuffer& idbuf;
	Shadow& shadow;
	SunShafts& sunShafts;
	Dust& dust;
	lightvol::Volumes& lightVolumes;
	// sitting 9 (kenshi-k16.2.205.12.54): worldSetup. The light volumes are the packet's (pk.buildings.volumes, T19; the dev ops read framestats' counts); refl: setup() / pass() / camera() / mvp() / view(); portraits: update() only, on
	// the packet's jobs (PortraitTable is logic's)
	Reflection& refl;
	Portraits& portraits;
	CreatorScene& creator;	// light() only: the creator's fixed light (the editor scene)
	// sitting 10 (kenshi-k16.2.205.12.55): worldOverlay. loadScreen: draw() (init and shutdown are logic's); cursors: draw(); trails: context() (the
	// sgl error check) and submit() (sitting 11: the packet's trails list into its context; draw() is logic's record); sglShown: the sgl error flags last logged for the default and trails contexts, render-owned
	LoadScreen& loadScreen;
	Cursors& cursors;
	Trails& trails;
	uint32_t (&sglShown)[2];
	bool reflSkyOnly = false;	// the reflection's last decision (kenshi-k16.2.205.11): little water on screen, the pass draws the sky alone; render-owned
	bool adaptedOnce = false;	// the HDR adaptation has run at least once (the creator skips it): render-owned
	uint64_t steppedSerial = ~0ull;	// the serial of the packet whose frame step the time-driven render state (light volume blend, HDR adaptation) last took (kenshi-38zv)
	double stepDt = 0;	// this draw's step for that state: the packet's dt on its first draw, 0 on overlap's redraw of the same packet
};

// The draw of one packet: the resource jobs queued for it, the readbacks it serves, its passes, sg_commit. On one thread frame() calls it after
// logic_frame; with render.thread on, the render thread does (logicthread.hpp).
void render_frame(const FramePacket& pk, RenderState& rs);

#ifdef KCLIENT_DEV_TOOLS
class GpuTimer;
// The two dev draws render_frame calls (dev::labRender, dev::viewDraw): dev sits above the frame layer, so main.cpp installs them once at start
// (before the render thread runs); null until then. Release builds have neither.
namespace framehooks
{
	extern void (*labRender)(const dev::LabFrame&, const Shadow::FrameData&);
	extern void (*viewDraw)(const FramePacket&, GpuTimer&);
}
#endif
