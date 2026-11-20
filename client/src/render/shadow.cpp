#include "render/shadow.hpp"
#include "render/drawstats.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/config.hpp"
#include "shadowclear.glsl.h"
#include "shadowrestore.glsl.h"

namespace
{

constexpr double kTowardSun = 60000.0;		// casters this far towards the sun still reach the cascade (mountains at low sun)
// The game's caster bias (RTW, the default shadow mode: ShadowSharedParams biasParams set in the RTW constructor 140866e40:267-270; kenshi-inspect
// findings/terrain_shadow_casting.md, rwh.23): depth += min(0.002, 6 x |depth gradient|) in the RTW shadow camera's normalised depth, whose range is the
// light-space extent of the camera frustum out to the Shadow Range (M in the findings). The cap is converted to world units with that extent, then to
// each cascade's own depth range.
constexpr double kBiasSlope = 6.0, kBiasCapFraction = 0.002;
constexpr double kShadowRange = 5000.0;	// settings.cfg:47 'Shadow Range' (= the outer cascade's radius)
constexpr double kNoShadowBelow = -0.2;	// the sun colour is zero below sun height -0.2 (SKY2 Q2): no shadow pass; above it the game's shadow factor is full strength (LIGHTING.md section 2: no fade with the sun height)
constexpr float kPcfRadius = 1.5f;
// MINE: a sun moving every frame re-rasterised every thin caster (trunks, shrubs) a sub-texel differently each frame, and their shadows crawled;
// stepped, they change once every few seconds at 1x.
constexpr double kSunStep = 0.25 * 3.14159265358979 / 180.0;
// render.sun_smooth (kenshi-h2v.32.1; user 2026-11-12: the step reads as a jerk when the sun sets behind a mountain, as in the original): the
// map's light direction holds until the shading's has moved kSunStep, then the next map is drawn (render.shadow_crossfade: into the atlas's other
// set) and the shading blends the old map into the new one over kFadeSeconds, so nothing jumps and a thin caster's shadow morphs between steps
// instead of changing every frame (user 2026-11-14, kenshi-k16.2.225: smooth read as very jittery, each 0.25 degree step as just as jarring,
// since the shadow blobs changed shape at every tick). It also keeps a CLOCK that arrives a little early or late (live: 1 Hz, the sim owns the
// time) from moving the shadows. A turn beyond kSunSnapJump is a clock set, a load or a rewind, not the sun, and snaps as everything else on
// such a jump does. With render.shadow_crossfade off, sun_smooth is the old per-frame chase of a map redrawn every frame (kept for comparison).
constexpr double kFadeSeconds = 0.75;
constexpr double kSunSmoothTau = 0.35, kSunSnapJump = 3.0 * 3.14159265358979 / 180.0;
constexpr double kSunSmoothEps = 1e-5;	// radians: closer than this the filter has converged, so the direction holds and paused frames stay identical

const cfg::Bool kSunSmooth{ cfg::Group{ "Indoor lighting (experimental)" }, "render.sun_smooth", true, "Move the sun shadow map smoothly instead of in the original's 0.25 degree steps: a 0.75 s crossfade between stepped maps (render.shadow_crossfade on), else the per-frame chase of a map drawn every frame (crossfade off). Off: the shadows stand still for a second or two and then jump, as the original's do" };
const cfg::Bool kCrossfade{ cfg::Group{ "Shadows" }, "render.shadow_crossfade", true, "The sun shadow map's light direction steps and the shading crossfades the old map into the new one over 0.75 s (kenshi-k16.2.225.1, option 1): thin casters' shadows (cage bars, limbs) morph between steps instead of re-rolling every frame or re-shaping at each step. Off: render.sun_smooth is the old per-frame chase. Needs render.sun_smooth" };

void norm(double v[3])
{
	const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	for (int k = 0; k < 3; k++) { v[k] /= l; }
}

double dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The atlas is a square grid of Shadow::kTilesPerSide x Shadow::kTilesPerSide tiles (equal in uv, so the shader's texel-size scalar holds for
// both axes); cascade c of set s sits in tile s * 4 + c, laid out left to right, top to bottom. The second set is the previous step's map.
constexpr int kTileCol(int set, int cascade) { return (set * Shadow::kCascades + cascade) % Shadow::kTilesPerSide; }
constexpr int kTileRow(int set, int cascade) { return (set * Shadow::kCascades + cascade) / Shadow::kTilesPerSide; }
constexpr double kTileUV = (double)Shadow::kTile / Shadow::kAtlas;	// uv span of one tile

}

void Shadow::init()
{
	sg_image_desc id = {};
	id.usage.depth_stencil_attachment = true;
	id.width = id.height = kAtlas;
	id.pixel_format = SG_PIXELFORMAT_DEPTH;
	id.sample_count = 1;
	id.label = "shadow-atlas";
	image_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = image_;
	view_ = sg_make_view(&vd);
	vd = {};
	vd.depth_stencil_attachment.image = image_;
	att_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.mipmap_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.compare = SG_COMPAREFUNC_LESS_EQUAL;
	smp_ = sg_make_sampler(&sd);
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.compare = SG_COMPAREFUNC_NEVER;
	rawSmp_ = sg_make_sampler(&sd);
	clearShd_ = sg_make_shader(shadowclear_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = clearShd_;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pd.depth.write_enabled = true;
	pd.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
	pd.color_count = 0;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.sample_count = 1;
	pd.label = "shadow-clear";
	clearPip_ = sg_make_pipeline(&pd);
	forceCache_ = std::getenv("KCLIENT_SHADOWCACHE_FORCE") != nullptr;	// dev A/B: the cache's draws every frame, to compare with the keyed redraws pixel for pixel
}

// The cache's atlas and the restore pipeline, made on first use (a run with render.shadow_cache off never pays the second atlas).
void Shadow::makeCache()
{
	if (cacheImg_.id != SG_INVALID_ID) { return; }
	sg_image_desc id = {};
	id.usage.depth_stencil_attachment = true;
	id.width = id.height = kAtlas;
	id.pixel_format = SG_PIXELFORMAT_DEPTH;
	id.sample_count = 1;
	id.label = "shadow-cache-atlas";
	cacheImg_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = cacheImg_;
	cacheView_ = sg_make_view(&vd);
	vd = {};
	vd.depth_stencil_attachment.image = cacheImg_;
	cacheAtt_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	cacheSmp_ = sg_make_sampler(&sd);
	restoreShd_ = sg_make_shader(shadowrestore_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = restoreShd_;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pd.depth.write_enabled = true;
	pd.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
	pd.color_count = 0;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.sample_count = 1;
	pd.label = "shadow-restore";
	restorePip_ = sg_make_pipeline(&pd);
}

void Shadow::shutdown()
{
	if (cacheImg_.id != SG_INVALID_ID)
	{
		sg_destroy_pipeline(restorePip_);
		sg_destroy_shader(restoreShd_);
		sg_destroy_sampler(cacheSmp_);
		sg_destroy_view(cacheAtt_);
		sg_destroy_view(cacheView_);
		sg_destroy_image(cacheImg_);
		cacheImg_ = {};
	}
	sg_destroy_pipeline(clearPip_);
	sg_destroy_shader(clearShd_);
	sg_destroy_sampler(smp_);
	sg_destroy_sampler(rawSmp_);
	sg_destroy_view(att_);
	sg_destroy_view(view_);
	sg_destroy_image(image_);
}

void Shadow::update(const double cam[3], const Camera& view, const float sunDir[3], double sunRealY, double realDt, bool paused)
{
	// light basis: lz towards the sun, lx horizontal, ly = lz x lx
	double d[3] = { sunDir[0], sunDir[1], sunDir[2] };
	norm(d);
	// render.sun_smooth on: the map follows the shading's sun. With render.shadow_crossfade (the default) it steps: the map holds its direction
	// until the sun has moved kSunStep, then draws that direction into the other atlas set and the shading blends the old set into it over a fade
	// (a step never starts while a fade is running: the fade finishes first, so the map trails the sun by at most one fade). Off (the chase), it
	// follows the shading's sun every frame with this time constant, so a clock step is spread over about a second and a CLOCK's early or late
	// arrival does not move the shadows; the old path, kept for comparison. No frame time (a --scene-test shot, whose clock is held): no fade can
	// run, so a sun the scene moved follows at once, as the stepped path did (a scene jump of 0.25-3 degrees must not keep the last scene's map).
	const bool smooth = kSunSmooth.get(), cross = smooth && kCrossfade.get();
	const double err = std::acos(std::clamp(dot(d, sun_), -1.0, 1.0));
	if (paused && err <= kSunSnapJump) {}	// paused (the sky clock is frozen): the map holds where it is, no catching up for a split second after the pause and no fade progress; a jump (load, time set) still snaps below
	else if (cross)
	{
		if (err > kSunSnapJump)	// not the sun: a time set, a load, a rewind. Snap (no crossfade), every tile redraws now rather than after a stagger's few frames
		{
			memcpy(sun_, d, sizeof sun_);
			fade_ = 1.0f;
			for (bool& v : valid_) { v = false; }
		}
		else if (realDt <= 0)	// a held clock (a --scene-test shot): no fade can run, so a moved sun follows at once, exactly as the stepped path did
		{
			if (err > kSunStep) { memcpy(sun_, d, sizeof sun_); fade_ = 1.0f; for (bool& v : valid_) { v = false; } }
		}
		else if (fade_ >= 1.0f)
		{
			if (err > kSunStep) { beginStep(cam, d); }
		}
		else { fade_ = (float)std::min(1.0, fade_ + realDt / kFadeSeconds); }
	}
	else if (smooth && realDt > 0)
	{
		if (err > kSunSnapJump)	// not the sun: a time set, a load, a rewind. Every tile redraws now rather than after a stagger's few frames
		{
			memcpy(sun_, d, sizeof sun_);
			for (bool& v : valid_) { v = false; }
		}
		else if (err > kSunSmoothEps)
		{
			const double t = 1.0 - std::exp(-realDt / kSunSmoothTau);
			for (int k = 0; k < 3; k++) { sun_[k] += (d[k] - sun_[k]) * t; }
			norm(sun_);
		}
	}
	else if (dot(d, sun_) < std::cos(kSunStep))
	{
		memcpy(sun_, d, sizeof sun_);
		for (bool& v : valid_) { v = false; }
	}
#ifdef KCLIENT_DEV_TOOLS
	// the drawn direction against the shading's, one line a frame (a check of render.sun_smooth; KSUNLOG in sky.cpp prints the sun itself);
	// fade is render.shadow_crossfade's 0..1 old -> new (1: no previous map) and set which atlas set the map is drawn into
	if (getenv("KSUNSHADOWLOG")) { fprintf(stderr, "SHADOWLOG %.9f %.9f %.9f err %.9f fade %.3f set %d\n", sun_[0], sun_[1], sun_[2], err, fade_, set_); }
#endif
	// the game's shadow light: the sun direction with y clamped to the horizon, no minimum elevation (FUN_14066cfe0; kenshi-inspect
	// findings/shadow_sun_direction.md, rwh.20), so the map is lit from the same direction the shading uses (kenshi-k16.2.57: a 3 degree floor here
	// blacked out sun-facing slopes at dawn). Straight below (no azimuth left) the sun colour is zero anyway; any horizontal axis does.
	double lz[3] = { sun_[0], std::max(sun_[1], 0.0), sun_[2] };
	if (std::fabs(lz[0]) + std::fabs(lz[1]) + std::fabs(lz[2]) < 1e-9) { lz[2] = 1.0; }
	norm(lz);
	double lx[3] = { lz[2], 0.0, -lz[0] };	// up x lz
	if (std::fabs(lx[0]) + std::fabs(lx[2]) < 1e-4) { lx[0] = 1.0; lx[2] = 0.0; }
	norm(lx);
	const double ly[3] = { lz[1] * lx[2] - lz[2] * lx[1], lz[2] * lx[0] - lz[0] * lx[2], lz[0] * lx[1] - lz[1] * lx[0] };
	const double camLx = dot(lx, cam), camLy = dot(ly, cam), camLz = dot(lz, cam);
	// the game's shadow camera depth extent: the camera point and the frustum's corners at the Shadow Range, along the light
	double zMin = 0, zMax = 0;
	for (int k = 0; k < 4; k++)
	{
		const double sx = (k & 1 ? 1.0 : -1.0) * view.tanY * view.aspect, sy = (k & 2 ? 1.0 : -1.0) * view.tanY;
		double p[3];
		for (int j = 0; j < 3; j++) { p[j] = (view.f[j] + view.r[j] * sx + view.u[j] * sy) * kShadowRange; }
		zMin = std::min(zMin, dot(p, lz));
		zMax = std::max(zMax, dot(p, lz));
	}
	double capWorld = kBiasCapFraction * (zMax - zMin);
	// The cache's tiles are drawn with one bias cap and one depth range until their key changes, so the cap (which follows the view's depth extent along the sun, and
	// turns with the camera) is held until it is 10 % off, and the depth range's centre snaps to a texel (below): a turning camera does not redraw every tile.
	cacheLive_ = cache && enabled && sunRealY >= kNoShadowBelow;
	if (cacheLive_)
	{
		if (capUsed_ <= 0 || std::fabs(capWorld / capUsed_ - 1.0) > 0.1) { capUsed_ = capWorld; }
		capWorld = capUsed_;
	}

	params_ = {};
	frame_++;
	for (int c = 0; c < kCascades; c++)
	{
		ShadowCascade& sc = cascade_[c];
		const double half = Shadow::kRadius[c] * 1.01;
		const double texel = 2.0 * half / kTile;
		params_.split[c] = (float)Shadow::kRadius[c];
		params_.texel[c] = (float)texel;
		const double moved = std::fabs(cam[0] - lastCam_[c][0]) + std::fabs(cam[1] - lastCam_[c][1]) + std::fabs(cam[2] - lastCam_[c][2]);
		render_[c] = !valid_[c] || !stagger || kPeriod[c] == 1 || moved > kMaxMove[c] || (frame_ + kPhase[c]) % kPeriod[c] == 0;
		if (!render_[c])
		{
			// the lookup takes positions relative to the current camera, the tile was drawn relative to the camera of the last redraw
			memcpy(params_.sm[c], lastSm_[c], sizeof lastSm_[c]);
			for (int r = 0; r < 3; r++)
			{
				for (int k = 0; k < 3; k++) { params_.sm[c][12 + r] += (float)((cam[k] - lastCam_[c][k]) * lastSm_[c][k * 4 + r]); }
			}
		}
		else
		{
			valid_[c] = true;
			memcpy(lastCam_[c], cam, sizeof lastCam_[c]);
			const int col = kTileCol(set_, c), row = kTileRow(set_, c);
			const double cx = std::round(camLx / texel) * texel, cy = std::round(camLy / texel) * texel;
			const double zc = cacheLive_ ? std::round(camLz / texel) * texel : camLz;	// the cache: whole texels, so the depth range holds still with the camera
			const double zTop = zc + half + kTowardSun, zBot = zc - half - 50.0, range = zTop - zBot;
			geom_[c][0] = cx; geom_[c][1] = cy; geom_[c][2] = zTop;
			geom_[c][3] = lz[0]; geom_[c][4] = lz[1]; geom_[c][5] = lz[2];
			geom_[c][6] = capWorld; geom_[c][7] = set_; geom_[c][8] = 0;
			sc.radius = (float)Shadow::kRadius[c];
			sc.texel = (float)texel;
			sc.tile[0] = col;
			sc.tile[1] = row;
			sc.index = c;
			sc.bias[0] = (float)kBiasSlope;
			sc.bias[1] = (float)(capWorld / range);

			// rows (xyz, w) of: x in -1..1, y in -1..1, depth 0..1 (0 = towards the sun), as functions of the camera-relative position
			double rows[3][4];
			for (int k = 0; k < 3; k++)
			{
				rows[0][k] = lx[k] / half;
				rows[1][k] = ly[k] / half;
				rows[2][k] = -lz[k] / range;
			}
			rows[0][3] = (camLx - cx) / half;
			rows[1][3] = (camLy - cy) / half;
			rows[2][3] = (zTop - camLz) / range;

			memset(sc.lvp, 0, sizeof sc.lvp);
			for (int k = 0; k < 4; k++)
			{
				sc.lvp[k * 4 + 0] = (float)rows[0][k];
				sc.lvp[k * 4 + 1] = (float)rows[1][k];
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
				sc.lvp[k * 4 + 2] = (float)rows[2][k];
#else
				sc.lvp[k * 4 + 2] = (float)(rows[2][k] * 2.0 - (k == 3 ? 1.0 : 0.0));
#endif
			}
			sc.lvp[15] = 1.0f;

			// lookup matrix: atlas uv in x, y; depth in z (a tile spans kTileUV of the atlas in both axes)
			for (int k = 0; k < 4; k++)
			{
				params_.sm[c][k * 4 + 0] = (float)((rows[0][k] * 0.5 + (k == 3 ? 0.5 : 0.0)) * kTileUV + (k == 3 ? col * kTileUV : 0.0));
				params_.sm[c][k * 4 + 1] = (float)((rows[1][k] * 0.5 + (k == 3 ? 0.5 : 0.0)) * kTileUV + (k == 3 ? row * kTileUV : 0.0));
				params_.sm[c][k * 4 + 2] = (float)rows[2][k];
			}
			params_.sm[c][15] = 1.0f;
			memcpy(lastSm_[c], params_.sm[c], sizeof lastSm_[c]);

			// side planes (inside positive), normalised: |lx| = |ly| = 1 so scale the offsets by half
			for (int k = 0; k < 3; k++)
			{
				sc.planes[0][k] = (float)lx[k];
				sc.planes[1][k] = (float)-lx[k];
				sc.planes[2][k] = (float)ly[k];
				sc.planes[3][k] = (float)-ly[k];
			}
			sc.planes[0][3] = (float)(rows[0][3] * half + half);
			sc.planes[1][3] = (float)(-rows[0][3] * half + half);
			sc.planes[2][3] = (float)(rows[1][3] * half + half);
			sc.planes[3][3] = (float)(-rows[1][3] * half + half);
		}
		// the previous step's set (render.shadow_crossfade, read while sinfo.w < 1): drawn relative to its own camera, so its lookup takes
		// positions relative to the current one the same way a stagger-redrawn tile does
		params_.tile[c][0] = (float)(kTileCol(set_, c) * kTileUV);
		params_.tile[c][1] = (float)(kTileRow(set_, c) * kTileUV);
		params_.tile[c][2] = (float)((kTileCol(set_, c) + 1) * kTileUV);
		params_.tile[c][3] = (float)((kTileRow(set_, c) + 1) * kTileUV);
		if (fade_ < 1.0f)
		{
			memcpy(params_.smPrev[c], lastSmPrev_[c], sizeof lastSmPrev_[c]);
			for (int r = 0; r < 3; r++)
			{
				for (int k = 0; k < 3; k++) { params_.smPrev[c][12 + r] += (float)((cam[k] - lastCamPrev_[c][k]) * lastSmPrev_[c][k * 4 + r]); }
			}
			const int prevSet = set_ ^ 1;
			params_.tilePrev[c][0] = (float)(kTileCol(prevSet, c) * kTileUV);
			params_.tilePrev[c][1] = (float)(kTileRow(prevSet, c) * kTileUV);
			params_.tilePrev[c][2] = (float)((kTileCol(prevSet, c) + 1) * kTileUV);
			params_.tilePrev[c][3] = (float)((kTileRow(prevSet, c) + 1) * kTileUV);
		}
	}
	params_.sinfo[0] = enabled && sunRealY >= kNoShadowBelow ? 1.0f : 0.0f;
	params_.sinfo[1] = 1.0f / kAtlas;
	params_.sinfo[2] = kPcfRadius;
	params_.sinfo[3] = cross ? fade_ : 1.0f;	// the crossfade's 0..1 old -> new; 1 (no previous map) with the crossfade off or after a snap
	if (cross && fade_ < 1.0f)
	{
		double travel = 0;
		for (int k = 0; k < 3; k++) { travel += (cam[k] - stepCam_[k]) * (cam[k] - stepCam_[k]); }
		params_.xinfo[0] = (float)std::sqrt(travel);	// the previous map's ball is centred on the step's camera: the shader drops it out there
	}
	for (int k = 0; k < 3; k++) { params_.ldir[k] = (float)lz[k]; }
	params_.ldir[3] = soft > 0.0f ? std::tan(soft * 3.14159265f / 180.0f) : 0.0f;
}

void Shadow::beginStep(const double cam[3], const double d[3])
{
	// A sun step (render.shadow_crossfade): the drawn set becomes the previous map, with the matrices and camera of its tiles' last redraws (the
	// lookup of the fade), and the next direction is drawn into the other set. Every cascade invalidates, so this frame's tiles all hold the new
	// direction and none mixes two (the stagger cannot leave a neighbouring tile on the old one).
	for (int c = 0; c < kCascades; c++)
	{
		memcpy(lastSmPrev_[c], lastSm_[c], sizeof lastSmPrev_[c]);
		memcpy(lastCamPrev_[c], lastCam_[c], sizeof lastCamPrev_[c]);
	}
	set_ ^= 1;
	fade_ = 0.0f;
	memcpy(stepCam_, cam, sizeof stepCam_);
	memcpy(sun_, d, sizeof sun_);
	for (bool& v : valid_) { v = false; }
}

void Shadow::beginPass()
{
	sg_pass p = {};
	p.action.depth.load_action = SG_LOADACTION_LOAD;
	p.action.depth.clear_value = 1.0f;
	p.attachments.depth_stencil = att_;
	p.label = "shadow";
	sg_begin_pass(&p);
}

void Shadow::snapshot(FrameData& o) const
{
	o.params = params_;
	for (int i = 0; i < kCascades; i++) { o.cascade[i] = cascade_[i]; o.render[i] = render_[i]; }
	memcpy(o.geom, geom_, sizeof o.geom);
	o.enabled = enabled;
	o.cacheOn = cacheLive_;
	o.set = set_;
	o.staticBudget = staticBudget;
	o.view = view_;
	o.sampler = smp_;
	o.rawSampler = rawSmp_;
}

void Shadow::selectCascade(const FrameData& fd, int i)
{
	const ShadowCascade& c = fd.cascade[i];
	sg_apply_viewport(c.tile[0] * kTile, c.tile[1] * kTile, kTile, kTile, false);
	sg_apply_scissor_rect(c.tile[0] * kTile, c.tile[1] * kTile, kTile, kTile, false);
	drawstats::pipeline(clearPip_);
	drawstats::draw(0, 3, 1);
}

void Shadow::selectTile(const FrameData& fd, int i)
{
	const ShadowCascade& c = fd.cascade[i];
	sg_apply_viewport(c.tile[0] * kTile, c.tile[1] * kTile, kTile, kTile, false);
	sg_apply_scissor_rect(c.tile[0] * kTile, c.tile[1] * kTile, kTile, kTile, false);
}

void Shadow::resolve(const FrameData& fd, uint64_t sig)
{
	const int set_ = fd.set, staticBudget = fd.staticBudget;
	const auto& geom_ = fd.geom;
	for (bool& s : staticDraw_) { s = false; }
	staticAny_ = false;
	if (!fd.cacheOn)
	{
		for (auto& set : slot_) { for (CacheSlot& s : set) { s.valid = false; } }	// whatever the owners did while it was off went unseen
		memset(liveClean_, 0, sizeof liveClean_);
		return;
	}
	makeCache();
	resolves_++;
	const auto draw = [&](int c)
	{
		CacheSlot& s = slot_[set_][c];
		s.valid = true;
		s.sig = sig;
		s.waitingSince = 0;
		memcpy(s.key, geom_[c], sizeof s.key);
		staticDraw_[c] = true;
		staticAny_ = true;
		liveClean_[set_][c] = false;	// the cache's tile changed: the live one has to be restored from it
	};
	int drawn = 0, waiting[kCascades], nWaiting = 0;
	for (int c = 0; c < kCascades; c++)
	{
		if (!fd.render[c]) { continue; }
		CacheSlot& s = slot_[set_][c];
		const bool sameKey = s.valid && memcmp(s.key, geom_[c], sizeof s.key) == 0;
		if (!forceCache_ && sameKey && s.sig == sig) { s.waitingSince = 0; continue; }
		if (forceCache_ || !sameKey || staticBudget <= 0) { draw(c); drawn++; continue; }
		if (s.waitingSince == 0) { s.waitingSince = resolves_; }
		waiting[nWaiting++] = c;	// content-only: under the budget below
	}
	// the oldest waiting first, the nearer cascade on a tie (cascade order already is the tie order: a stable sort)
	std::stable_sort(waiting, waiting + nWaiting, [&](int a, int b) { return slot_[set_][a].waitingSince < slot_[set_][b].waitingSince; });
	for (int k = 0; k < nWaiting; k++)
	{
		const int c = waiting[k];
		if (drawn < staticBudget || resolves_ - slot_[set_][c].waitingSince + 1 >= (uint64_t)kCascades) { draw(c); drawn++; }
	}
}

void Shadow::beginStaticPass()
{
	sg_pass p = {};
	p.action.depth.load_action = SG_LOADACTION_LOAD;
	p.action.depth.clear_value = 1.0f;
	p.attachments.depth_stencil = cacheAtt_;
	p.label = "shadow-static";
	sg_begin_pass(&p);
}

void Shadow::selectStatic(const FrameData& fd, int i) { selectCascade(fd, i); }	// the same tile rectangle in the cache's atlas, cleared the same way

void Shadow::endStaticPass() { sg_end_pass(); }

void Shadow::restoreCascade(const FrameData& fd, int i)
{
	if (liveClean_[fd.set][i]) { return; }
	liveClean_[fd.set][i] = true;
	const ShadowCascade& c = fd.cascade[i];
	sg_apply_viewport(c.tile[0] * kTile, c.tile[1] * kTile, kTile, kTile, false);
	sg_apply_scissor_rect(c.tile[0] * kTile, c.tile[1] * kTile, kTile, kTile, false);
	drawstats::pipeline(restorePip_);
	sg_bindings b = {};
	b.views[VIEW_cache_tex] = cacheView_;
	b.samplers[SMP_cache_smp] = cacheSmp_;
	drawstats::bindings(&b);
	drawstats::draw(0, 3, 1);
}

void Shadow::invalidateAtlas()
{
	for (auto& set : slot_) { for (CacheSlot& s : set) { s.valid = false; } }
	memset(liveClean_, 0, sizeof liveClean_);
}

void Shadow::invalidate()
{
	for (bool& v : valid_) { v = false; }
	fade_ = 1.0f;	// whatever the other set holds is from before the gap: no crossfade into it
}

void Shadow::endPass() { sg_end_pass(); }
