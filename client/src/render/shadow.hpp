#pragma once
#include "render/camera.hpp"
#include "sokol_gfx.h"

#include <cstdint>
#include <cstring>

// Sun shadows: 4 cascaded shadow maps in one depth atlas (3 x 3 tiles, 8 used), docs/reference/LIGHTING.md "Shadows". Every cascade is an orthographic
// view along the sun covering a ball around the camera (so it does not change when the camera turns), its centre snapped to whole texels in
// absolute world space (no shimmer when the camera moves). Casters (terrain, buildings, features, foliage) are drawn into the tiles by their owners;
// receivers sample the atlas through shaders/shadow.inc with `ShadowParams` (the `shadow_params` uniform block).
// The atlas holds two full sets of the 4 cascades (set 0 and set 1), so a sun step can draw the next set while the shading still reads the
// previous one and crossfades between them (render.shadow_crossfade, kenshi-k16.2.225.1): option 1 of kenshi-k16.2.225, a thin caster's shadow
// morphs between steps instead of re-rolling every frame (the chase) or jumping at each step.

// Matches the `shadow_params` uniform block of shaders/shadow.inc; users static_assert against their generated struct.
struct ShadowParams
{
	float sm[4][16];	// camera-relative world -> atlas uv + depth, column-major (the set being drawn)
	float smPrev[4][16];	// the same for the previous step's set, read while sinfo.w < 1
	float split[4];		// outer camera distance of each cascade
	float texel[4];		// world size of one shadow texel
	float sinfo[4];		// x = strength 0..1 (0 = off), y = 1 / atlas size, z = PCF radius in texels, w = crossfade 0..1 (1 = no previous map)
	float ldir[4];		// direction towards the sun used by the shadow map; w = tan(sun half angle) of render.shadow_soft (0 = off)
	float tile[4][4];	// atlas uv rect (x0, y0, x1, y1) of each cascade's tile in the drawn set: the PCSS blocker search stays inside it
	float tilePrev[4][4];	// the same for the previous step's set (read while sinfo.w < 1)
	float xinfo[4];		// x = camera travel since the step that started the fade: the previous map is only used within its ball
};

struct ShadowCascade
{
	float lvp[16];		// camera-relative world -> clip space of the tile (column-major, same convention as the camera mvp)
	float planes[4][4];	// the four side planes of the tile's box, normalised, inside = positive (Gribb-Hartmann order: x > -1, x < 1, y > -1, y < 1)
	float radius = 0;	// ball around the camera
	float texel = 0;	// world units per shadow texel
	int tile[2] = {};	// tile column / row in the atlas
	int index = 0;		// cascade number 0..3 (the tile's position in the atlas is a layout detail: use this for per-cascade data)
	float bias[4] = {};	// the casters' depth bias (shadow.inc caster_bias): slope, cap in this tile's normalised depth, fixed
};

class Shadow
{
public:
	static constexpr int kCascades = 4;
	static constexpr int kTile = 2048;
	static constexpr int kTilesPerSide = 3;		// 3 x 3 tiles: 8 hold two sets of the 4 cascades (tile (2,2) unused), so a step can crossfade
	static constexpr int kAtlas = kTile * kTilesPerSide;
	static constexpr int kSets = 2;
	static constexpr double kRadius[kCascades] = { 120.0, 400.0, 1400.0, 5000.0 };	// ball radius = outer camera distance of each cascade, in world units (10 per metre); the last is the game's Shadow Range 5000 (settings.cfg:47)

	// The frame's shadow data, written by logic (snapshot(), at the end of the frame's first half) into the render packet; the draw and every receiver read
	// only this (kenshi-k16.2.205.12.113, overlap review T17). The atlas, the cache slots and liveClean_ stay render-owned (resolve / restoreCascade / markDirty).
	struct FrameData
	{
		ShadowParams params = {};
		ShadowCascade cascade[kCascades];
		double geom[kCascades][9] = {};	// the cache keys of the cascades that render
		bool render[kCascades] = {};	// update() decided to redraw cascade i
		bool enabled = true, cacheOn = false;
		int set = 0;			// the atlas set being drawn
		int staticBudget = 1;
		sg_view view = {};		// the atlas, fixed after init
		sg_sampler sampler = {}, rawSampler = {};
		bool active() const { return enabled && params.sinfo[0] > 0; }
		bool renders(int i) const { return render[i]; }
		const ShadowCascade& casc(int i) const { return cascade[i]; }
	};
	void snapshot(FrameData& out) const;

	void init();
	void shutdown();

	bool enabled = true;	// F3
	float soft = 0;		// render.shadow_soft: sun half angle in degrees for PCSS on cascades 0 and 1 (0 = off, the fixed-width edge)

	// cam: the absolute position the cascades centre on. view: the camera, whose frustum sizes the casters' bias cap. sunDir: the lighting's sunDirection (unit, towards
	// the sun, clamped to the horizon); sunRealY: the unclamped sun height (sin of elevation), which switches the shadows off below -0.2 (where the sun colour is zero); no fade above that, as in the game.
	// realDt: this frame's seconds (0: nothing moves), which render.sun_smooth crossfades a step over, and which the old chase follows the sun by
	// (render.sun_smooth off holds the direction as the original steps it). paused: the clock is frozen, so no step starts and a running crossfade
	// holds where it is (kenshi-k16.2.226.1's hold).
	void update(const double cam[3], const Camera& view, const float sunDir[3], double sunRealY, double realDt, bool paused = false);
	bool active() const { return enabled && params_.sinfo[0] > 0; }

	const ShadowParams& params() const { return params_; }
	const ShadowCascade& cascade(int i) const { return cascade_[i]; }
	sg_view view() const { return view_; }
	sg_sampler sampler() const { return smp_; }
	sg_sampler rawSampler() const { return rawSmp_; }	// nearest, no comparison: the atlas as plain depth (PCSS blocker search)

	// One depth-only pass over the whole atlas: beginPass(), then per cascade with renders(i) selectCascade(i) (which clears the tile) and the casters' draws,
	// then endPass(). The tiles keep their contents between passes, so a cascade that is not redrawn is looked up with the matrices of its last redraw.
	void beginPass();
	bool renders(int i) const { return render_[i]; }	// this frame's update() decided to redraw cascade i (logic side)
	void selectCascade(const FrameData& fd, int i);
	// viewport and scissor of cascade i with no clear (kenshi-k16.2.101.1): the characters redraw their cascade's tile after the world's casters
	void selectTile(const FrameData& fd, int i);
	void endPass();
	void invalidate();	// logic side of "the atlas is not trustworthy (shadows were off)": redraw every cascade next frame, no crossfade into the old set
	void invalidateAtlas();	// render side: forget the cache slots and the clean flags

	// The static-caster cache (render.shadow_cache, kenshi-k16.2.205.12.2). The static casters (terrain, buildings, features, foliage) of each (atlas set, cascade) are drawn into a
	// second atlas of the same layout (the cache) only when the tile's key changes: the cascade's snapped centre and depth range in absolute space, the light basis, the bias cap
	// and `contentSig` (the owners' generations: streaming, doors, construction). The live atlas the receivers sample is the cache's tile restored by a depth-writing fullscreen draw,
	// then the animated casters and the characters on top, so the result is the plain redraw's depth exactly (no receiver shader reads two maps).
	// Order per frame: update(), resolve(sig), then beginStaticPass / selectStatic(i) + the static draws for each staticDraw(i) / endStaticPass, then beginPass() and per renders(i)
	// cascade restoreCascade(i) (a no-op while the live tile is clean), selectTile(i), the dynamic draws with markDirty(i) when any drew, endPass().
	bool cache = true;	// render.shadow_cache
	bool cacheOn() const { return cacheLive_; }	// this frame's decision (cache && the sun is up)
	// render.shadow_static_budget (kenshi-k16.2.205.12.73): the most static tiles a frame redraws for a content change alone (`contentSig` moved, the
	// tile's key held: new buildings streamed in, a door, terrain). A tile whose key changed (or that holds nothing) redraws at once and counts
	// against the budget; a content-only tile waits (it keeps its old content, drawn with the same matrices, so it is right except for the new
	// casters) and the oldest waiting goes first, nearest cascade on a tie. One waiting since kCascades - 1 frames redraws regardless, so a new
	// building's shadow is at most kCascades - 1 frames late. 0: no limit (every changed tile in the frame of the change, the old behaviour).
	int staticBudget = 1;
	void resolve(const FrameData& fd, uint64_t contentSig);
	bool staticDraw(int i) const { return staticDraw_[i]; }
	void beginStaticPass();
	void selectStatic(const FrameData& fd, int i);	// viewport and scissor of cascade i's cache tile, cleared
	void endStaticPass();
	void restoreCascade(const FrameData& fd, int i);	// live tile of cascade i := the cache's, unless it already is; leaves the viewport on the tile
	void markDirty(const FrameData& fd, int i) { liveClean_[fd.set][i] = false; }	// something was drawn over the restored tile
	bool anyStaticDraw() const { return staticAny_; }

	// Cascades 1-3 are redrawn every kPeriod[i] frames (staggered) unless the camera has moved enough to matter; off (every cascade every frame) by default.
	bool stagger = false;

private:
	void beginStep(const double cam[3], const double d[3]);	// render.shadow_crossfade: keep the drawn set as the previous map and start the new direction in the other set

	struct CacheSlot
	{
		bool valid = false;
		double key[9] = {};	// snapped centre x, y, depth top, the light direction, the bias cap
		uint64_t sig = 0;
		uint64_t waitingSince = 0;	// the resolve count at which a content-only change was first deferred, 0 none
	};
	void makeCache();
	CacheSlot slot_[kSets][kCascades];
	double geom_[kCascades][9] = {};	// this frame's key of the cascades that render
	bool liveClean_[kSets][kCascades] = {};	// the live tile equals the cache's tile (nothing drawn over it since the restore)
	bool staticDraw_[kCascades] = {};
	bool staticAny_ = false, cacheLive_ = false, forceCache_ = false;
	uint64_t resolves_ = 0;	// resolve() calls while the cache is live (the waiting tiles' clock)
	double capUsed_ = 0;	// the bias cap in world units the cache draws with (held until the true one is 10 % off)
	sg_image cacheImg_ = {};
	sg_view cacheView_ = {}, cacheAtt_ = {};
	sg_sampler cacheSmp_ = {};
	sg_shader restoreShd_ = {};
	sg_pipeline restorePip_ = {};
	ShadowParams params_ = {};
	ShadowCascade cascade_[kCascades];
	sg_image image_ = {};
	sg_view view_ = {}, att_ = {};
	sg_sampler smp_ = {}, rawSmp_ = {};
	sg_shader clearShd_ = {};
	sg_pipeline clearPip_ = {};
	bool render_[kCascades] = {};
	bool valid_[kCascades] = {};		// the tile of the drawn set holds a finished render
	float lastSm_[kCascades][16] = {};	// lookup matrices of the last redraw
	double lastCam_[kCascades][3] = {};	// camera position of the last redraw
	// The set being drawn (render.shadow_crossfade): a step flips it, snapshots the other set's matrices for the fade and invalidates the tiles.
	int set_ = 0;
	float fade_ = 1.0f;			// 0..1 old -> new; 1 = no previous map (sinfo.w)
	float lastSmPrev_[kCascades][16] = {};	// the previous set's lookup matrices at the step
	double lastCamPrev_[kCascades][3] = {};	// camera position when the previous set was last redrawn
	double stepCam_[3] = {};		// the camera at the step that started the fade (xinfo: how far the view has left the previous map's ball)
	uint64_t frame_ = 0;
	double sun_[3] = {};	// the shadow light direction (render.sun_smooth on: the last committed step; off: held until it has moved kSunStep, all tiles redraw then)
	static constexpr int kPeriod[kCascades] = { 1, 2, 2, 4 };	// redraw interval with stagger on
	static constexpr int kPhase[kCascades] = { 0, 0, 1, 2 };	// frame offset: c1 redraws on even frames, c2 on odd ones, c3 on every 4th even one
	static constexpr double kMaxMove[kCascades] = { 2.0, 8.0, 28.0, 100.0 };	// camera travel since the last redraw that forces one (world units)
};
