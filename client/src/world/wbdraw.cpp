#include "world/worldbuildings.hpp"
#include "app/cpustage.hpp"
#include "world/worldbuildings_internal.hpp"
#include "world/foliagewind.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/drawstats.hpp"
#include "knobs/lightterms.hpp"
#include "render/ao.hpp"
#include "render/speccube.hpp"
#include "render/weathermap.hpp"
#include "app/config.hpp"
#include "interiormask.glsl.h"
#include "kworld/coords.hpp"
#include "mesh.glsl.h"

using namespace wbi;

namespace
{
constexpr float kFlipFade = 0.25f;	// MINE (user 2026-11-18): seconds a wall that turned away from the cut-away camera takes to fade out
constexpr float kGlassAlpha = 0.25f;	// MINE: a fully peeked or ghosted part's opacity
constexpr double kEffectMargin = 1500.0;	// units of view margin for effect collection: a just-off-screen fire stays alive (ParticleWorld)
constexpr int kUploadsPerFrame = 8;
const cfg::Bool kNormalMaps{ cfg::Group{ "Detail" }, "render.normal_maps", true, "Normal maps on buildings, props and foliage (off: their geometric normal; for comparisons)" };
// The far ring (kenshi-k16.2.65; ours, user 2026-11-09: the original's generic far-town stand-in is "WILDLY inaccurate"): past the load radius a
// building's own exterior parts stay drawn out to this distance (capped by the fog), dithered out over its last kFarFade; no interiors, lights or effects.
const cfg::Float kBuildingFar{ cfg::Group{ "Draw distance" }, "render.building_far", 60000, 0, 100000, "Towns and rocks: how far their exteriors are drawn (world units, 10 = 1 m; capped by the fog; 0 = only the load radius)" };
constexpr float kFarFade = 0.15f;	// MINE: the share of the far distance the exteriors dither out over
const cfg::Bool kSignsIndoors{ cfg::Group{ "Interiors" }, "render.signs_indoors", true, "Display building signs while indoors: on by default (the original): a building's outside signs (shop and bar signs) keep drawing while it is cut to a floor; off hides them until the roof view" };
[[maybe_unused]] const bool kRetiredCutShadows = (cfg::retire("render.cut_shadows", "render.cut_shadows is gone (kenshi-jj3.30): the cut-away floors and roof always cast their sun shadows"), true);
const cfg::Bool kLeafSway{ cfg::Group{ "Detail" }, "render.leaf_sway", true, "A tree bends and its leaves sway with the live wind: strength and direction, still at no wind (on by default, user decision 2026-11-20, kenshi-k16.2.420; off: the original's static trees; edits are picked up while the client runs)" };
const cfg::Float kFoliageWindRef{ cfg::Group{ "Detail" }, "render.foliage_wind_ref", 30.0, 1.0, 1000.0, "The wind speed (u/s) at which foliage sways at full strength; the weather records' wind speed max is 30 for most (60-100 in storms), the windmill blades ramp over 10..50" };
constexpr float kInteriorFade = 0.8f;	// the dithered fade of interior parts and furniture starts at this fraction of the interior radius
// The loading stand-in of a building (kenshi-k16.2.258.4, user 2026-11-19: "If the load delay is just textures and lighting, then we can use the same
// pure-black body trick as the character loader. It shows that SOMETHING is there."; the character's is render.loading_standin, kenshi-k16.2.258.1):
// an instance whose shell meshes are through but whose textures are not all ready draws every part flat black (unlit, no texture); when the last one is
// ready the whole instance switches to its real look over kStandinFade, dithered against the stand-in (a character's colour stage, about 0.2 s).
constexpr double kStandinFade = 0.2;
constexpr float kGlassFndBuild = 210000.0f, kFndOnlyBuild = 220000.0f;	// a ghosted occluder's glass without its solid zone: -(kGlassFndBuild + opacity x 1000); its solid zone alone, opaque: -kFndOnlyBuild (mesh.glsl inst_vs)
constexpr float kGlassBuild = 200000.0f;	// a peeked part: stretch w = -(kGlassBuild + opacity x 1000) (mesh.glsl inst_vs)
#ifdef KCLIENT_DEV_TOOLS
// dev.building_standin_hold (KCLIENT_DEV_TOOLS, kenshi-k16.2.258.4): hold every building in its stand-in (its textures counted as not ready) until this many
// seconds after the first update, so the black silhouettes and the switch can be seen; a script sets it to 0 to release them into the switch at once.
const cfg::Float kStandinHold{ cfg::Group{ "Character loading", "", "Debug" }, "dev.building_standin_hold", 0.0, 0.0, 100000.0, "Dev: keep every building in its loading stand-in (flat black) until this many s after the first update (0 = off; a script sets it back to 0 to release the switch)" };
#endif
constexpr double kMinCasterRadius = 1.0;	// instances out of view cast shadows only if their bounding radius is at least this
constexpr float kTriplanarUnits = 5000.0f;	// world units per texture repeat at tile 1 for the triplanar modes (triplanar.hlsl triplanar_vs: / 5000.0)

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
	vs.camw[3] = (float)time;
	if (swayTop > 0)	// render.leaf_sway: the live foliage wind (foliagewind.hpp)
	{
		const foliagewind::State& f = foliagewind::g;
		vs.sway[0] = f.strength; vs.sway[1] = f.dir[0]; vs.sway[2] = f.dir[1]; vs.sway[3] = (float)f.phase;
		vs.limb[1] = def.leavesPart ? 1.0f : 0.0f;
	}
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

}



// ---- per frame ----

// Rotation function 3 spins at `rotation speed` x the wind's ramp (W - min) / (max - min), clamped to 0..1 (kinspect b50p_static.md Q11, the
// exe's factor; MINE: that the factor scales the drawn spin is untraced). W is the live wind at the camera (the exe reads each building's
// zone); unknown (an offline scene) or a part with no wind fields: full speed, the angle from the clock as before. The angle is integrated,
// so a wind change alters the rate, not the angle.
// A wind-aimed part (function 4, kinspect rotation_function4_wind_aim.md): a new wind starts a turn from where the part faces (at the base
// rate; the shader adds the instance's 0.95..1.05 factor) to the new wind. The first known wind is faced at once (the exe turns from the
// authored pose on load; we skip it).
// The one floor-cut test for a part (see the declaration): the renderer below and the click pick (cutAway) both call it.
bool WorldBuildings::partCutHidden(int floor, bool sign, int shown) { return floor > shown && !(sign && kSignsIndoors.get()); }

void WorldBuildings::advanceMotion()
{
	const double dt = time_ - motionTime_;
	motionTime_ = time_;
	const bool step = dt > 0 && dt < 1;	// a clock jump (a load, a scene) restarts the integration
	{
		// The foliage wind: the same windSpeed / wind[] as the spin ramp and the aimed parts, smoothed (about 1.5 s) and turned to a strength and a unit
		// direction (foliagewind.hpp). An unknown speed (an offline scene) holds strength 1, an unknown direction today's (0.8, 0.6).
		foliagewind::State& f = foliagewind::g;
		const float target = windSpeed < 0 ? 1.0f : std::min(windSpeed / std::max((float)kFoliageWindRef.get(), 1.0f), 1.5f);
		float tx = f.dir[0], tz = f.dir[1];
		if (wind[2] >= 0.5f && std::hypot(wind[0], wind[1]) > 1e-4f) { tx = wind[0]; tz = wind[1]; }
		if (step)
		{
			const float k = 1.0f - std::exp(-(float)dt / 1.5f);
			f.strength += (target - f.strength) * k;
			f.dir[0] += (tx - f.dir[0]) * k;
			f.dir[1] += (tz - f.dir[1]) * k;
			f.phase = std::fmod(f.phase + dt * (0.35 + 0.65 * std::min(f.strength, 1.5f)), 100000.0);	// the rate rises with the strength; integrated
		}
		else { f.strength = target; f.dir[0] = tx; f.dir[1] = tz; }
		const float l = std::hypot(f.dir[0], f.dir[1]);
		if (l > 1e-4f) { f.dir[0] /= l; f.dir[1] /= l; } else { f.dir[0] = 0.8f; f.dir[1] = 0.6f; }
#ifdef KCLIENT_DEV_TOOLS
		// KCLIENT_WINDLOG=1 (dev-only): once a second, the foliage strength and direction next to the raw wind and the blade spin ramp of the first
		// wind-driven spinning part (the same factor advanceMotion applies below), to see that they move together.
		static const bool logOn = getenv("KCLIENT_WINDLOG") != nullptr;
		static double nextLog = 0;
		if (logOn && step && time_ >= nextLog)
		{
			nextLog = time_ + 1.0;
			double blade = -1;
			for (uint32_t pid : turning_)
			{
				const BuildingPartDef& d = data_.parts[pid];
				if (spinning(d) && d.windMax > d.windMin) { blade = windSpeed < 0 ? 1.0 : std::clamp((windSpeed - d.windMin) / (d.windMax - d.windMin), 0.0f, 1.0f); break; }
			}
			fprintf(stderr, "windlog: t=%.1f windSpeed=%.2f foliage strength=%.3f dir=(%.2f,%.2f) phase=%.2f blade ramp=%.3f\n", time_, windSpeed, f.strength, f.dir[0], f.dir[1], f.phase, blade);
		}
#endif
	}
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

// Which resources the streams make first (kenshi-k16.2.258.3): a building in view and near, then in view and far, then out of view, each by distance.
// A lower score runs sooner. The view test is the instance origin against the frustum, with a margin for the building's size.
float WorldBuildings::streamScore(const BuildingInstance& b, const double cam[3], const float mvp[16]) const
{
	if (!streamPriority) { return 0.0f; }	// every score equal: the queues keep their submit order (render.stream_priority 0, the old lane)
	const double p[3] = { b.x - cam[0], b.y - cam[1], b.z - cam[2] };
	const double dist = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	double c[4];
	for (int k = 0; k < 4; k++) { c[k] = mvp[k] * p[0] + mvp[4 + k] * p[1] + mvp[8 + k] * p[2]; }	// the view's rows: x, y, z, w (column-major, no translation)
	const double m = 1.4, slack = 600.0;	// 40 % past the screen's edge and the building's size
	const bool inView = c[3] > -slack && std::fabs(c[0]) <= m * (c[3] + slack) && std::fabs(c[1]) <= m * (c[3] + slack);
	const float cls = !inView ? 2e5f : dist < kNearView ? 0.0f : 1e5f;
	return cls + (float)std::min(dist, 9e4);
}

void WorldBuildings::update(const double cam[3], const float mvp[16], float fogDist, bool blocking, double time, double shadowFar, FrameData& out)
{
	struct SigOut { WorldBuildings& wb; FrameData& out; ~SigOut() { out.shadowSig = wb.shadowSig(); wb.resolveDraws(out); } } sigOut{ *this, out };	// on every return, after the last change
	out.inst.clear();
	out.lightCount = 0;
	frame_++;
	time_ = time;
#ifdef KCLIENT_DEV_TOOLS
	// The pop-in timeline's clock (kenshi-k16.2.258.2): seconds since the first update, the frame, and how this run streams (a scripted run
	// waits for every decode unless debug.stream_scripted is on, so its timeline is one frame long).
	if (popin.on)
	{
		const double s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		if (popin.clock0 < 0) { popin.clock0 = s; }
		popin.now = s - popin.clock0;
		popin.frame = frame_;
		popin.blocking = blocking;
		popin.streaming = !blocking;
	}
	// One curve row a frame, while one of its counts moved: the load's shape in wall time (the same numbers `dump popin` prints).
	auto popinCurve = [&] {
		if (!popin.on) { return; }
		if (!popin.curve.empty())
		{
			const PopIn::Curve& l = popin.curve.back();
			if (l.inRange == popin.inRange && l.drawn == popin.drawn && l.full == popin.full && l.meshesReady == stats_.meshesReady
				&& l.texturesReady == stats_.texturesReady && l.pending == stats_.pending) { return; }
		}
		popin.curve.push_back({ popin.now, frame_, popin.inRange, popin.drawn, popin.full, stats_.meshesReady, stats_.texturesReady, stats_.pending });
	};
#endif
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
	// the loading stand-in's clock and mode (kenshi-k16.2.258.4): a live run's wall seconds, a scripted run's frame time; render.loading_standin read
	// again when a setting changed (silhouette and double are both black here, see the pack loop)
	standNow_ = blocking ? time : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (standT0_ < 0) { standT0_ = standNow_; }
	{
		static uint64_t seen = ~0ull;
		if (const uint64_t g = cfg::generation(); g != seen)
		{
			seen = g;
			for (const cfg::Info& i : cfg::find("render.loading_standin")) { if (i.key == "render.loading_standin") { standMode_ = i.value == "off" ? 2 : i.value == "double" ? 1 : 0; } }
		}
	}
	standHeld_ = false;
#ifdef KCLIENT_DEV_TOOLS
	standHeld_ = kStandinHold.get() > 0.0 && standNow_ - standT0_ < kStandinHold.get();
#endif
	if (frame_ % 120 == 0) { evict(); }
	const Stats prev = stats_;
	stats_ = Stats();
	stats_.uploadedMeshes = prev.uploadedMeshes;
	stats_.uploadedTextures = prev.uploadedTextures;
	needed_.clear();
	if (data_.instances.empty())
	{
#ifdef KCLIENT_DEV_TOOLS
		popinCurve();
#endif
		active_.clear(); fitGround_.clear(); fitRel_.clear(); idDraws_.clear(); return;
	}
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
	// A ghosted occluder (the occluder fade, ghosts_) shows its interior from any distance: its floor, which the faded walls and roof would otherwise leave as
	// the terrain seen through (user 2026-11-19: terrain shows where the floor should be; kenshi-k16.2.411.1), and its interior mask, which clips that terrain.
	const auto interiorWanted = [&](uint32_t ii, const BuildingInstance& b)
	{
		if (interiorNear(b)) { return true; }
		if (!interiors || b.parent >= 0) { return false; }
		const auto g = ghosts_.find(ii);
		return g != ghosts_.end() && g->second > 0;
	};
	hulls_.clear();
	for (const CellRef& c : cells)
	{
		for (const uint32_t ii : *c.list)
		{
			const BuildingInstance& b = data_.instances[ii];
			const bool near = !c.far && interiorWanted(ii, b);
			const float score = streamScore(b, cam, mvp);
			for (uint32_t k = 0; k < b.partCount; k++)
			{
				const uint32_t pid = data_.partRefs[b.firstPart + k];
				if (data_.parts[pid].kind != 0 && !near) { continue; }
				if (data_.parts[pid].kind == 2) { hulls_.push_back({ ii, pid }); }
				if (partStamp_[pid] != (uint32_t)frame_) { partStamp_[pid] = (uint32_t)frame_; partScore_[pid] = score; needed_.push_back(pid); }
				else if (score < partScore_[pid]) { partScore_[pid] = score; }
			}
#ifdef KCLIENT_DEV_TOOLS
			// pop-in: the instance entered a load cell (the load radius / the far ring): its timeline starts here (kenshi-k16.2.258.2)
			if (popin.on && b.parent < 0 && !b.feature && !b.prop && popin.rows.find(ii) == popin.rows.end()) { popinMark(ii, b); }
#endif
		}
	}
	for (;;)
	{
		{ CPUSTAGE_ACC("+wb.needed"); for (const uint32_t pid : needed_) { ensurePart(pid); } }
		if (!blocking && !streamAll) { CPUSTAGE_ACC("+wb.precacheStep"); precacheStep(cam); }	// stream.precache=high: the background world precache, after every real need (kenshi-nbmu)
		{ CPUSTAGE_ACC("+wb.rerank"); rerank(); }
		if (!blocking) { CPUSTAGE_ACC("+wb.drain"); drain(false, streamAll ? -1 : kUploadsPerFrame); break; }
		if (outstanding_ == 0) { break; }
		drain(true, -1);
	}
	if (holdPendingAny_ && frame_ - holdFlushFrame_ >= kHoldBatch)
	{
		holdPendingAny_ = false; holdFlushFrame_ = frame_; holdGen_++;
		for (const int mesh : holdPendingMeshes_) { if (meshes_[mesh].holdPending) { meshes_[mesh].holdPending = false; groundDirtyMesh(mesh); } }	// each held mesh's region, so only the things near them re-run their ground ray (kenshi-uxmu)
		holdPendingMeshes_.clear();
	}	// the precache's held moves (holdMoved)
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
#ifdef KCLIENT_DEV_TOOLS
		popinCurve();
#endif
		return;
	}
	packSig_ = sig;
	packValid_ = true;
	standFading_ = false;
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

	for (Group& g : groups_) { g.onlyShadow.clear(); g.inRange.clear(); g.outRange.clear(); g.glass.clear(); g.flip.clear(); g.standin.clear(); g.fade.clear(); }
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
			const bool furniture = b.parent >= 0, near = !c.far && interiorWanted(ii, b);
			if (!furniture && !c.far) { infoFor(ii); }	// fills the parts' foundY before any part of the building draws (mesh.glsl xray_fnd); cached after the first success
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
			// The loading stand-in's decision for the whole instance (kenshi-k16.2.258.4): 1 nothing (a shell mesh not through yet), 2 every part flat black
			// (a texture still streaming), 3 the switch to the real look, 0 the real look. The per-part ready test (partTexturesReady, kenshi-k16.2.298.1) is
			// the completion test; the draw decision is per instance. Failed and fileless meshes never hold it. An instance that cannot stand in (glass,
			// under construction, render.loading_standin off) waits whole with nothing drawn.
			int ss = 0;
			float cover = 1.0f;
			{
				bool meshWait = false, texWait = false, counted = false;
				for (uint32_t k = 0; k < b.partCount; k++)
				{
					const uint32_t pid = data_.partRefs[b.firstPart + k];
					const PartRt& prt = partRt_[pid];
					const uint8_t kind = data_.parts[pid].kind;
					if (kind == 2 || (kind != 0 && !near) || prt.mesh < 0) { continue; }
					const State ms = meshes_[prt.mesh].state;
					if (ms == Failed) { continue; }
					if (ms != Ready || !prt.resolved) { meshWait = true; continue; }
					counted = true;
					if (!partTexturesReady(pid)) { texWait = true; }
				}
				if (standHeld_ && counted) { texWait = true; }
				const bool standable = build == 0.0f && !glassy && standMode_ != 2;
				auto st = stand_.find(ii);
				if (meshWait) { ss = 1; }
				else if (texWait) { ss = standable ? 2 : 1; }
				if (ss == 2) { Stand& z = stand_[ii]; z.stood = true; z.readyAt = -1; }
				else if (ss == 1) { if (st != stand_.end()) { stand_.erase(st); } }
				else if (st != stand_.end())
				{
					if (st->second.readyAt < 0) { st->second.readyAt = standNow_; }
					cover = (float)((standNow_ - st->second.readyAt) / kStandinFade);
					if (cover >= 1.0f || !standable) { stand_.erase(st); cover = 1.0f; }
					else { ss = 3; standFading_ = true; }
				}
			}
			size_t nBlack = 0, nReal = 0;
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
				const bool cutHidden = partCutHidden(furniture ? furnitureFloor : def.floor, def.sign, shown);
				if (cutHidden && (def.blackCap || shadowFar <= 0)) { continue; }
				if (def.blackCap && def.floor == shown && shown < b.maxFloor) { continue; }	// the cut floor's black cap (BuildingPartDef::blackCap)
				if (!cutHidden && (furniture || def.kind == 1 || night) && !c.far) { for (uint32_t k = 0; k < def.lightCount; k++) { addLight(b, ii, def, def.firstLight + k, cam, planes, maxDist); } }
				for (uint32_t k = 0; k < def.effectCount && !c.far && !cutHidden; k++) { addEffect(b, ii, (int32_t)pid, &def, def.firstEffect + k, cam, planes); }
				if (ss == 1 || rt.mesh < 0 || !rt.resolved || meshes_[rt.mesh].state != Ready) { continue; }	// the instance waits whole, or this part has no mesh (the loading stand-in above; never a grey unlit frame, kenshi-k16.2.298.1)
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
					if (fresh) { groups_.push_back({ pid, (int)s, {}, {}, {}, {}, {}, {}, {}, 0 }); }
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
					if (ss == 2)	// the stand-in: flat black, no shadow, whole (stretch w = glass coding, cover 0: the black not yet dithered away)
					{
						if (!vis) { continue; }
						g.standin.insert(g.standin.end(), inst, inst + kInstanceFloats);
						g.standin.back() = -(kGlassBuild + 0.0f);
						total++;
						nBlack++;
						stats_.submeshDraws++;
						continue;
					}
					if (ss == 3 && def.featureMode != 2)	// the switch: the real look dithers in (its share = cover) over the black dithering out; the shadow is the real one's
					{
						if (casts) { g.onlyShadow.insert(g.onlyShadow.end(), inst, inst + kInstanceFloats); }
						if (!vis || total + 2 > kMaxInstances) { continue; }
						g.fade.insert(g.fade.end(), inst, inst + kInstanceFloats);
						g.fade.back() = -(kGlassBuild + cover * 1000.0f);
						g.standin.insert(g.standin.end(), inst, inst + kInstanceFloats);
						g.standin.back() = -(kGlassBuild + cover * 1000.0f);
						total += 2;
						stats_.submeshDraws++;
						continue;
					}
					if (vis && ss == 0) { nReal++; }
					if ((furniture ? instFloor : def.floor) > peekFloor || (peeked && def.blackCap && def.floor == peekFloor))	// the cut floor's cap too
					{
						if (casts) { g.onlyShadow.insert(g.onlyShadow.end(), inst, inst + kInstanceFloats); }	// the glass keeps its shadow: it does not change with the peek (user 2026-11-05)
						if (!vis) { continue; }
						if (def.blackCap) { continue; }	// not drawn as glass: a near-opaque black layer over the stairwells early in the fade (user 2026-11-04)
						if (cursorOwner) { idDraws_.push_back({ it->second, 2, (uint32_t)(g.glass.size() / kInstanceFloats), owner }); }
						// A ghosted occluder keeps its foundation and ground floor solid (kenshi-k16.2.411.1): the glass pass draws only the nearest glass layer, so a floor
						// behind the roof's layer never showed. The part draws twice: its fragments under rt.foundY opaque in the normal pass (kFndOnlyBuild), the rest as
						// glass without them (kGlassFndBuild); mesh.glsl tells the two apart by v_build.z.
						const bool fndSplit = gh != ghosts_.end() && rt.foundY > -1e29f && total + 2 <= kMaxInstances;
						g.glass.insert(g.glass.end(), inst, inst + kInstanceFloats);
						g.glass.back() = -((fndSplit ? kGlassFndBuild : kGlassBuild) + glassAlpha * 1000.0f);
						if (fndSplit)
						{
							// outRange, never inRange: the shadow pass draws inRange, and onlyShadow above already casts this part's shadow once (kenshi-k16.2.411.2)
							g.outRange.insert(g.outRange.end(), inst, inst + kInstanceFloats);
							g.outRange.back() = -kFndOnlyBuild;
							total++;
						}
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
			stats_.standing += nBlack > 0;
			stats_.fading += ss == 3;
			stats_.mixed += nBlack > 0 && nReal > 0;	// black and real parts of one instance in one frame outside the switch: by construction never
#ifdef KCLIENT_DEV_TOOLS
			if (popin.on)
			{
				popin.standingInstanceFrames += nBlack > 0;
				popin.fadingInstanceFrames += ss == 3;
				popin.mixedInstanceFrames += nBlack > 0 && nReal > 0;
				if (const auto row = popin.rows.find(ii); row != popin.rows.end())
				{
					if (nBlack > 0 && row->second.black < 0) { row->second.black = popin.now; }
					if (ss == 3 && row->second.black >= 0 && row->second.swtch < 0) { row->second.swtch = popin.now; }
					if (nBlack > 0 && nReal > 0) { row->second.mixedFrames++; }
				}
			}
#endif
			if (any && furniture) { stats_.interiorsDrawn++; }
			else if (any)
			{
				stats_.instancesDrawn++;
				stats_.featuresDrawn += b.feature;
				fitGround_.push_back((float)(b.y - b.ground) + lowest);
				fitRel_.push_back((float)(b.y - b.ground));
#ifdef KCLIENT_DEV_TOOLS
				if (popin.on) { popinDrawn(ii); }	// its first drawn frame / its last shell part textured (kenshi-k16.2.258.2)
#endif
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
				if (b.parent >= 0 || !interiorWanted(ii, b)) { continue; }
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
		if (g.onlyShadow.empty() && g.inRange.empty() && g.outRange.empty() && g.glass.empty() && g.flip.empty() && g.standin.empty() && g.fade.empty()) { continue; }
		g.first = (int)(frameData_.size() / kInstanceFloats);
		frameData_.insert(frameData_.end(), g.onlyShadow.begin(), g.onlyShadow.end());
		frameData_.insert(frameData_.end(), g.inRange.begin(), g.inRange.end());
		frameData_.insert(frameData_.end(), g.outRange.begin(), g.outRange.end());
		frameData_.insert(frameData_.end(), g.glass.begin(), g.glass.end());
		frameData_.insert(frameData_.end(), g.flip.begin(), g.flip.end());
		frameData_.insert(frameData_.end(), g.standin.begin(), g.standin.end());
		frameData_.insert(frameData_.end(), g.fade.begin(), g.fade.end());
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
#ifdef KCLIENT_DEV_TOOLS
	popinCurve();
#endif
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
	mix((uint64_t)standMode_ | (standHeld_ ? 4u : 0u) | (standFading_ ? 8u : 0u));
	if (standFading_) { mix(frame_); }	// a switch under way: the cover moves with the clock, so every frame repacks
	return h;
}

void WorldBuildings::setSwayMates(const std::vector<uint32_t>& ids)
{
	for (uint32_t id : ids) { data_.parts[id].swayMates = ids; }
}

float WorldBuildings::swayTop(uint32_t pid) const
{
	const BuildingPartDef& d = data_.parts[pid];
	if (!d.plant) { return 0.0f; }	// rocks, wrecks, ruins and props never bend (plants::isPlant)
	float top = meshes_[partRt_[pid].mesh].bmax[1];
	for (uint32_t id : d.swayMates)
	{
		if (id < partRt_.size() && partRt_[id].mesh < meshes_.size()) { top = std::max(top, meshes_[partRt_[id].mesh].bmax[1]); }
	}
	return top;
}

uint32_t WorldBuildings::addPart(const BuildingPartDef& def)
{
	dataGen_++;
	topBoxGen_++;
	data_.parts.push_back(def);
	partRt_.emplace_back();
	partStamp_.push_back(0);
	partScore_.push_back(0.0f);
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
	{	// the light debugger: the lamps in view (nearest 64) by id, and the switched-off ones leave the list (knobs/lightterms.hpp; no-ops in a release build)
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

int WorldBuildings::meshRef(DrawList& L, int mesh) const
{
	if ((size_t)mesh < L.meshAt.size())
	{
		int& at = L.meshAt[mesh];
		if (at < 0) { at = (int)L.meshes.size(); L.meshes.push_back(meshes_[mesh].gpu); }
		return at;
	}
	L.meshes.push_back(meshes_[mesh].gpu);
	return (int)L.meshes.size() - 1;
}

int WorldBuildings::texRef(DrawList& L, int t) const
{
	if (t < 0 || textures_[t].state != Ready) { return -1; }
	if ((size_t)t < L.texAt.size())
	{
		int& at = L.texAt[t];
		if (at < 0) { at = (int)L.textures.size(); L.textures.push_back(textures_[t].gpu); }
		return at;
	}
	L.textures.push_back(textures_[t].gpu);
	return (int)L.textures.size() - 1;
}

int WorldBuildings::ibufRef(DrawList& L, uint32_t pid) const
{
	const PartRt& rt = partRt_[pid];
	if (!(kOpenWindows.get() && rt.openHas)) { return -1; }
	if (pid < L.ibufAt.size())
	{
		int& at = L.ibufAt[pid];
		if (at < 0) { at = (int)L.ibufs.size(); L.ibufs.push_back(rt.openIbuf); }
		return at;
	}
	L.ibufs.push_back(rt.openIbuf);
	return (int)L.ibufs.size() - 1;
}

// texView on a list slot: the texture's view if its create has run and sokol took it, else the fallback.
sg_view WorldBuildings::viewOf(const DrawList& L, int slot, sg_view fallback) const
{
	if (slot < 0 || !L.textures[slot].ready()) { return fallback; }
	const sg_view v = L.textures[slot].get().view;
	return v.id != SG_INVALID_ID ? v : fallback;
}

static_assert(sizeof(inst_vs_params_t) == sizeof(WorldBuildings::ResolvedDraw::vs) && sizeof(inst_fs_params_t) == sizeof(WorldBuildings::ResolvedDraw::fs)
		&& sizeof(shadow_fs_params_t) == sizeof(WorldBuildings::ResolvedShadow::fs),
	"ResolvedDraw's uniform blocks are the shader's layout");
static_assert(sizeof(inst_vs_params_t) == 60 * sizeof(float) && sizeof(inst_fs_params_t) == 76 * sizeof(float) && sizeof(shadow_fs_params_t) == 12 * sizeof(float),
	"the per-draw uniform caches (WorldBuildings::drawVs_ / drawFs_ / shadowVs_ / shadowFs_) are the blocks' sizes (kenshi-k16.2.205.12.119)");
static_assert(offsetof(inst_vs_params_t, mvp) == 0, "the replay writes the matrix at the block's start");

// Everything draw() reads from the building tables and the logic-side frame state (camera, clock, wind, x-ray, flip eye), by value (see ResolvedDraw).
void WorldBuildings::resolveDraw(DrawList& L, ResolvedDraw& r, uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], float fadeStart, float fadeEnd, int fadeMode,
	const float* limbSkin, const float* limbBlood, int standin, bool flip) const
{
	const PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	// standin 1: the loading stand-in (flat black, unlit: mesh.glsl inst_fs misc2.y = 1; no texture of the part is bound, its fragment shader reads none);
	// 2: the real look dithering in over it (misc2.y = 2). 0: the draw as before.
	auto view = [&](int t) { return standin == 1 ? -1 : texRef(L, t); };
	const int mode = limbSkin || standin == 1 ? (standin == 1 ? 0 : 7) : modeFor(pid);	// 7: a severed limb with the wire's tint (mesh.glsl inst_fs); without one it draws as before
	const MeshEntry& m = meshes_[rt.mesh];
	const Sub& s = m.subs[sub];
	r.mesh = meshRef(L, rt.mesh);
	r.ibuf = ibufRef(L, pid);
	r.firstVertex = s.firstVertex;
	r.firstIndex = s.firstIndex;
	r.indexCount = s.indexCount;
	r.instOffset = instByteOffset;
	r.count = count;
	r.inst = inst;
	r.mode = mode;
	r.cull = cullsBackFaces(def);
	// a severed limb binds the item's own maps (SeveredLimbItem vf108), not the submesh's script material: its texture, the race body normal (DXT5)
	// and its color map; every other part keeps the rule above
	r.tex[0] = mode == 7 && rt.ownTex >= 0 ? view(rt.ownTex) : view(rt.subTex[sub]);
	r.tex[1] = standin == 1 ? -1 : mode == 7 ? texRef(L, rt.normalTex)
		: (rt.subFlat[sub] || !kNormalMaps.get()) && mode != 1 && mode != 2 ? -1 : texRef(L, rt.normalTex);
	r.tex[2] = view(rt.tex2Tex);
	r.tex[3] = rt.subFlat[sub] || !kNormalMaps.get() || rt.nrm2Tex < 0 ? -1 : texRef(L, rt.nrm2Tex);
	r.tex[4] = mode == 7 ? view(rt.maskTex) : -1;	// a severed limb's `color map` (skin tone mask)
	float turn[6];
	const float noMvp[16] = {};
	const float noBase[3] = {};
	const inst_vs_params_t vs = instVsParams(def, offsetFrame, cam_, time_, noBase, noMvp, kLeafSway.get() ? swayTop(pid) : 0.0f, motion_[pid].spin, turnOf(pid, turn));	// base: the replay writes the group's, so a part's template keeps 0
	for (int k = 0; k < 3; k++) { r.base[k] = base[k]; }
	r.fade[0] = fadeStart;
	r.fade[1] = fadeEnd;
	inst_fs_params_t fs = {};
	fs.misc[0] = cutOf(pid);	// features by record (building parts clip on the normal map alpha, mode 1)
	fs.misc[3] = def.featureMode == 4 ? 1.0f : 0.0f;
	fs.misc[1] = (float)mode;
	fs.tri[0] = def.tileX;
	fs.tri[1] = def.tileY;
	fs.tri[2] = def.localCoords ? 1.0f : 0.0f;
	fs.tri[3] = kTriplanarUnits;
	fs.fade[2] = (float)fadeMode;
	std::copy(xray, xray + 4, fs.xray);
	if (def.plant) { fs.xray[3] = 0.0f; }	// foliage never dithers in the occluder cone (kenshi-k16.2.411.4.1; plants::isPlant)
	for (int k = 0; k < 3; k++) { fs.xray_ex0[k] = (float)(xrayEx[k] - cam_[k]); fs.xray_ex1[k] = (float)xrayEx[4 + k]; }
	fs.xray_ex0[3] = (float)xrayEx[3];
	fs.xray_ex1[3] = (float)xrayEx[7];
	fs.xray_fnd[0] = partRt_[pid].foundY;	// the solid zone (foundation + ground floor) never fades (mesh.glsl)
	fs.misc[2] = def.featureMode == 4 ? 1.0f : 0.0f;	// FOLIAGE mode: COLOURING (vertex colour); otherwise lit like any StaticObject
	if (flip)
	{
		fs.flip[0] = kFlipFade / 8.0f;
		std::copy(&flipEye_[0][0], &flipEye_[0][0] + 32, &fs.flip_eye[0][0]);
	}
	fs.misc2[1] = (float)standin;
	fs.misc2[0] = def.interior ? 1.0f : 0.0f;	// the dust coat takes the weather's `dust inside` (objects.hlsl INTERIOR, k16.2.202.1)
	if (limbSkin)	// the wire's tint (SeveredLimbItem custom 2 / custom 3)
	{
		std::copy(limbSkin, limbSkin + 3, fs.skin);
		std::copy(limbBlood, limbBlood + 3, fs.blood);
		fs.blood[3] = 1.0f;	// bloodColour's alpha: the exe's AppearanceHuman stores the RACE `blood colour` as ARGB with alpha 1 (character.hlsl severed_limb_fs)
	}
	memcpy(r.vs, &vs, sizeof vs);
	memcpy(r.fs, &fs, sizeof fs);
}

// The pass's shared uniform blocks (kenshi-k16.2.205.12.119): the light, the AO, the light volumes and this frame's shadow params. Every draw of a
// building-shader pass reads the same ones, and on the GL backend an apply is a glUniform call per block member (~15 in the light block alone), so a
// pass pays them once instead of 597 times. Resets the per-draw block cache: a new pass must upload its own.
void WorldBuildings::applyPassUniforms(const LightParams& light)
{
	drawVsOk_ = drawFsOk_ = false;
	sg_apply_uniforms(UB_light_params, { &light, sizeof light });
	sg_apply_uniforms(UB_ao_params, SG_RANGE(ao_binding().params));
	sg_apply_uniforms(UB_light_volumes, SG_RANGE(lightvol::binding().params));
	sg_apply_uniforms(UB_shadow_params, { &sfd->params, sizeof(ShadowParams) });
}

// The draw call of a resolved draw: bindings the list names, the render-owned shared units, the per-draw uniforms (the pass's are applyPassUniforms').
// `inst` overrides the draw's instance buffer (the foliage names it by buffer slot). Returns the shader mode.
int WorldBuildings::replayDraw(const DrawList& L, const ResolvedDraw& r, sg_buffer inst, const float mvp[16])
{
	const MeshGpu& mg = L.meshes[r.mesh].get();
	sg_bindings b = {};
	b.vertex_buffers[0] = mg.vbuf;
	b.vertex_buffer_offsets[0] = r.firstVertex * kVertexBytes;
	b.vertex_buffers[1] = inst.id != SG_INVALID_ID ? inst : r.inst;
	b.vertex_buffer_offsets[1] = r.instOffset;
	b.index_buffer = r.ibuf >= 0 ? L.ibufs[r.ibuf].get() : mg.ibuf;
	b.index_buffer_offset = r.firstIndex * 4;
	b.views[VIEW_inst_tex] = viewOf(L, r.tex[0], grey_);
	b.views[VIEW_inst_nrm] = viewOf(L, r.tex[1], flat_);
	b.views[VIEW_inst_tex2] = viewOf(L, r.tex[2], grey_);
	b.views[VIEW_inst_nrm2] = viewOf(L, r.tex[3], flat_);
	b.views[VIEW_inst_ground] = groundView_;
	b.views[VIEW_dust_noise] = dust_;
	b.views[VIEW_inst_mask] = viewOf(L, r.tex[4], grey_);
	b.views[VIEW_inst_blood] = blood_;	// materials/bloody.png, bound by every draw like the shared units (sokol drops an unbound slot)
	b.samplers[SMP_inst_smp] = smp_;
	b.samplers[SMP_ground_smp] = groundSmp_;
	b.views[VIEW_shadow_tex] = sfd->view;
	b.samplers[SMP_shadow_smp] = sfd->sampler;
	b.samplers[SMP_shadow_raw] = sfd->rawSampler;
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
	inst_vs_params_t vs;
	memcpy(&vs, r.vs, sizeof vs);
	memcpy(vs.mvp, mvp, sizeof vs.mvp);
	memcpy(vs.base, r.base, sizeof r.base);	// the group's origin: a part's template leaves it 0 (kenshi-k16.2.205.12.119)
	memcpy(vs.fadep, r.fadep, sizeof r.fadep);
	inst_fs_params_t fs;
	memcpy(&fs, r.fs, sizeof fs);
	memcpy(fs.fade, r.fade, sizeof r.fade);
	if (!drawVsOk_ || memcmp(&vs, drawVs_, sizeof vs) != 0) { sg_apply_uniforms(UB_inst_vs_params, SG_RANGE(vs)); memcpy(drawVs_, &vs, sizeof vs); drawVsOk_ = true; }
	if (!drawFsOk_ || memcmp(&fs, drawFs_, sizeof fs) != 0) { sg_apply_uniforms(UB_inst_fs_params, SG_RANGE(fs)); memcpy(drawFs_, &fs, sizeof fs); drawFsOk_ = true; }
	drawstats::draw(0, r.indexCount, r.count);
	return r.mode;
}

int WorldBuildings::draw(uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], const float mvp[16], const LightParams& light, float fadeStart, float fadeEnd, int fadeMode,
	const float* limbSkin, const float* limbBlood, int standin)
{
	scratch_.clear();
	ResolvedDraw r;
	resolveDraw(scratch_, r, pid, sub, inst, instByteOffset, count, base, fadeStart, fadeEnd, fadeMode, limbSkin, limbBlood, standin);
	applyPassUniforms(light);	// this one-draw pass sets its own (renderFlip calls it per group)
	return replayDraw(scratch_, r, inst, mvp);
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
void WorldBuildings::resolveShadow(DrawList& L, ResolvedShadow& r, uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], float fadeStart, float fadeEnd) const
{
	const PartRt& rt = partRt_[pid];
	const BuildingPartDef& def = data_.parts[pid];
	const int mode = modeFor(pid);
	const MeshEntry& m = meshes_[rt.mesh];
	const Sub& s = m.subs[sub];
	const float alphaCut = mode > 3 ? 0.0f : cutOf(pid);
	r.mesh = meshRef(L, rt.mesh);
	r.ibuf = ibufRef(L, pid);
	r.firstVertex = s.firstVertex;
	r.firstIndex = s.firstIndex;
	r.indexCount = s.indexCount;
	r.instOffset = instByteOffset;
	r.count = count;
	r.inst = inst;
	r.animated = spinning(def) || def.aimed;
	r.tex[0] = alphaCut <= 0 ? -1 : def.featureMode == 4 ? texRef(L, rt.normalTex) : texRef(L, rt.subTex[sub]);
	r.tex[1] = mode == 1 ? texRef(L, rt.normalTex) : -1;
	for (int k = 0; k < 3; k++) { r.base[k] = base[k]; }
	r.fade[0] = fadeStart;
	r.fade[1] = fadeEnd;
	float turn[6];
	const float noMvp[16] = {};
	const float noBase[3] = {};
	const inst_vs_params_t vs = instVsParams(def, offsetFrame, cam_, time_, noBase, noMvp, kLeafSway.get() ? swayTop(pid) : 0.0f, motion_[pid].spin, turnOf(pid, turn));
	shadow_fs_params_t fs = {};
	fs.cut[0] = alphaCut;
	fs.cut[1] = mode == 1 ? 1.0f : 0.0f;
	memcpy(r.vs, &vs, sizeof vs);
	memcpy(r.fs, &fs, sizeof fs);
}

void WorldBuildings::replayShadow(const DrawList& L, const ResolvedShadow& r, sg_buffer inst, const float lvp[16], const float bias[4])
{
	const MeshGpu& mg = L.meshes[r.mesh].get();
	sg_bindings b = {};
	b.vertex_buffers[0] = mg.vbuf;
	b.vertex_buffer_offsets[0] = r.firstVertex * kVertexBytes;
	b.vertex_buffers[1] = inst.id != SG_INVALID_ID ? inst : r.inst;
	b.vertex_buffer_offsets[1] = r.instOffset;
	b.index_buffer = r.ibuf >= 0 ? L.ibufs[r.ibuf].get() : mg.ibuf;
	b.index_buffer_offset = r.firstIndex * 4;
	b.views[VIEW_inst_tex] = viewOf(L, r.tex[0], grey_);
	b.views[VIEW_inst_nrm] = viewOf(L, r.tex[1], grey_);
	b.samplers[SMP_inst_smp] = smp_;
	drawstats::bindings(&b);
	inst_vs_params_t vs;
	memcpy(&vs, r.vs, sizeof vs);
	memcpy(vs.mvp, lvp, sizeof vs.mvp);
	memcpy(vs.base, r.base, sizeof r.base);	// the group's origin: a part's template leaves it 0 (kenshi-k16.2.205.12.119)
	memcpy(vs.fadep, r.fadep, sizeof r.fadep);
	shadow_fs_params_t fs;
	memcpy(&fs, r.fs, sizeof fs);
	memcpy(fs.bias, bias, sizeof fs.bias);
	memcpy(fs.fade, r.fade, sizeof r.fade);
	if (!shadowVsOk_ || memcmp(&vs, shadowVs_, sizeof vs) != 0) { sg_apply_uniforms(UB_inst_vs_params, SG_RANGE(vs)); memcpy(shadowVs_, &vs, sizeof vs); shadowVsOk_ = true; }
	if (!shadowFsOk_ || memcmp(&fs, shadowFs_, sizeof fs) != 0) { sg_apply_uniforms(UB_shadow_fs_params, SG_RANGE(fs)); memcpy(shadowFs_, &fs, sizeof fs); shadowFsOk_ = true; }
	drawstats::draw(0, r.indexCount, r.count);
}

void WorldBuildings::drawShadow(uint32_t pid, int sub, sg_buffer inst, int instByteOffset, int count, const float base[3], const float lvp[16], const float bias[4], float fadeStart, float fadeEnd)
{
	scratch_.clear();
	ResolvedShadow r;
	resolveShadow(scratch_, r, pid, sub, inst, instByteOffset, count, base, fadeStart, fadeEnd);
	replayShadow(scratch_, r, inst, lvp, bias);
}

bool WorldBuildings::renderShadow(const DrawList& list, const ShadowCascade& c, int which)
{
	bool any = false;
	for (const ResolvedShadow& r : list.shadow)
	{
		if ((which == 1 && r.animated) || (which == 2 && !r.animated)) { continue; }
		if (!any) { drawstats::pipeline(shadowPip_); any = true; }
		replayShadow(list, r, sg_buffer{}, c.lvp, c.bias);
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

void WorldBuildings::render(const DrawList& list, const float mvp[16], const LightParams& light)
{
	if (list.opaque.empty()) { return; }
	beginDraws(false);
	applyPassUniforms(light);
	bool culled = false;
	for (const ResolvedDraw& r : list.opaque)
	{
		// Interior parts and every feature / FOLIAGE_MESH mesh except the FOLIAGE mode (leaves, `Foliage` material: cull_hardware none) cull back faces
		// like the game's mapfeature.material (no cull_hardware = clockwise); a camera inside a rock sees the terrain, not the rock's far wall.
		if (r.cull != culled)
		{
			culled = !culled;
			drawstats::pipeline(culled ? pipInterior_ : pip_);
		}
		replayDraw(list, r, sg_buffer{}, mvp);
	}
}

// update()'s last step, on every return: the groups' opaque and shadow draws, resolved into the packet (ResolvedDraw). The order is the old render()'s and
// renderShadow()'s: per group (active_), the opaque draw, the loading stand-in, the dithering fade; and the shadow draw. stats_.modeGroups moves here
// from render() (a stats write from the draw side).
void WorldBuildings::resolveDraws(FrameData& out)
{
	DrawList& L = out.draws;
	L.clear();
	for (size_t& n : stats_.modeGroups) { n = 0; }
	if (active_.empty() && maskGroups_.empty() && idDraws_.empty()) { return; }
	L.meshAt.assign(meshes_.size(), -1);
	L.texAt.assign(textures_.size(), -1);
	L.ibufAt.assign(partRt_.size(), -1);
	const float noBase[3] = {};
	// the interior mask hulls (renderMask)
	for (const MaskGroup& g : maskGroups_)
	{
		const PartRt& rt = partRt_[g.part];
		const Sub& s = meshes_[rt.mesh].subs[g.sub];
		L.mask.emplace_back();
		ResolvedMask& r = L.mask.back();
		r.mesh = meshRef(L, rt.mesh);
		r.ibuf = ibufRef(L, g.part);
		r.firstVertex = s.firstVertex;
		r.firstIndex = s.firstIndex;
		r.indexCount = s.indexCount;
		r.instOffset = g.first * (int)kInstanceBytes;
		r.count = (int)(g.inst.size() / kInstanceFloats);
		r.inst = instBuf_;
		float turn[6];
		const float noMvp[16] = {};
		const inst_vs_params_t p = instVsParams(data_.parts[g.part], offsetFrame, cam_, time_, noBase, noMvp, 0, motion_[g.part].spin, turnOf(g.part, turn));
		std::copy(p.offset, p.offset + 4, r.offset);
		std::copy(p.spin, p.spin + 4, r.spin);
	}
	// the peeked glass (renderGlass): the groups' glass instances, after in / out range
	for (const int gi : active_)
	{
		const Group& g = groups_[gi];
		if (g.glass.empty()) { continue; }
		const int before = (int)((g.onlyShadow.size() + g.inRange.size() + g.outRange.size()) / kInstanceFloats);
		float fadeStart, fadeEnd;
		fadeOf(g.part, fadeStart, fadeEnd);
		if (data_.parts[g.part].kind != 0) { fadeStart = fadeEnd = 0.0f; }	// a glass interior part (a ghosted occluder's floor, kenshi-k16.2.411.1) is wanted at any distance: no interior distance fade
		L.glass.emplace_back();
		resolveDraw(L, L.glass.back(), g.part, g.sub, instBuf_, (g.first + before) * (int)kInstanceBytes, (int)(g.glass.size() / kInstanceFloats), noBase, fadeStart, fadeEnd, 0, g.limb ? g.skin : nullptr, g.limb ? g.blood : nullptr, 0);
	}
	// the cut-away flip fade (renderFlip): the groups' flip instances after glass; flipMoving_ and the eye ring are read here, into each draw's uniforms
	if (flipMoving_)
	{
		for (const int gi : active_)
		{
			const Group& g = groups_[gi];
			if (g.flip.empty()) { continue; }
			const int before = (int)((g.onlyShadow.size() + g.inRange.size() + g.outRange.size() + g.glass.size()) / kInstanceFloats);
			float fadeStart, fadeEnd;
			fadeOf(g.part, fadeStart, fadeEnd);
			L.flip.emplace_back();
			resolveDraw(L, L.flip.back(), g.part, g.sub, instBuf_, (g.first + before) * (int)kInstanceBytes, (int)(g.flip.size() / kInstanceFloats), noBase, fadeStart, fadeEnd, 0, nullptr, nullptr, 0, true);
		}
	}
	// the cursor id draws (renderIds): one per run of the same (group, list, owner) with consecutive instances (kenshi-k16.2.205.12.119). The instances of
	// one part are consecutive in its group's list, and the target resolves by the scene depth (EQUAL) with no blend, so grouping the runs by key and drawing
	// them as one instanced range gives the same image in a fraction of the draws. Sorted first: update() appends the entries instance by instance.
	std::sort(idDraws_.begin(), idDraws_.end(), [](const IdDraw& a, const IdDraw& b)
	{
		if (a.group != b.group) { return a.group < b.group; }
		if (a.list != b.list) { return a.list < b.list; }
		if (a.owner != b.owner) { return a.owner < b.owner; }
		return a.index < b.index;
	});
	stats_.idDraws = stats_.idInstances = 0;
	for (size_t i = 0; i < idDraws_.size();)
	{
		const IdDraw& d = idDraws_[i];
		size_t run = 1;
		while (i + run < idDraws_.size())
		{
			const IdDraw& n = idDraws_[i + run];
			if (n.group != d.group || n.list != d.list || n.owner != d.owner || n.index != d.index + run) { break; }
			run++;
		}
		const Group& g = groups_[d.group];
		const BuildingPartDef& def = data_.parts[g.part];
		const PartRt& rt = partRt_[g.part];
		const Sub& sub = meshes_[rt.mesh].subs[g.sub];
		const size_t lists[3] = { g.inRange.size(), g.outRange.size(), g.glass.size() };
		size_t at = g.onlyShadow.size() / kInstanceFloats + d.index;
		for (int l = 0; l < d.list; l++) { at += lists[l] / kInstanceFloats; }
		L.ids.emplace_back();
		ResolvedId& r = L.ids.back();
		r.mesh = meshRef(L, rt.mesh);
		r.ibuf = ibufRef(L, g.part);
		r.firstVertex = sub.firstVertex;
		r.firstIndex = sub.firstIndex;
		r.indexCount = sub.indexCount;
		r.instOffset = (g.first + (int)at) * (int)kInstanceBytes;
		r.count = (int)run;
		r.inst = instBuf_;
		r.cull = cullsBackFaces(def);
		r.owner = (float)d.owner + 1.0f;
		float turn[6];
		const float noMvp[16] = {};
		const inst_vs_params_t vs = instVsParams(def, offsetFrame, cam_, time_, noBase, noMvp, kLeafSway.get() ? swayTop(g.part) : 0.0f, motion_[g.part].spin, turnOf(g.part, turn));
		memcpy(r.vs, &vs, sizeof vs);
		stats_.idDraws++;
		stats_.idInstances += run;
		i += run;
	}
	for (const int gi : active_)
	{
		const Group& g = groups_[gi];
		const BuildingPartDef& def = data_.parts[g.part];
		const int onlyShadow = (int)(g.onlyShadow.size() / kInstanceFloats), n = (int)((g.inRange.size() + g.outRange.size()) / kInstanceFloats);
		float fadeStart, fadeEnd;
		fadeOf(g.part, fadeStart, fadeEnd);
		if (n > 0)
		{
			L.opaque.emplace_back();
			resolveDraw(L, L.opaque.back(), g.part, g.sub, instBuf_, (g.first + onlyShadow) * (int)kInstanceBytes, n, noBase, fadeStart, fadeEnd, 0, g.limb ? g.skin : nullptr, g.limb ? g.blood : nullptr, 0);
			stats_.modeGroups[L.opaque.back().mode]++;
		}
		// the loading stand-in (kenshi-k16.2.258.4) and the real look dithering in against it: after flip in the group's block
		const int nStand = (int)(g.standin.size() / kInstanceFloats), nFade = (int)(g.fade.size() / kInstanceFloats);
		if (nStand > 0 || nFade > 0)
		{
			const int tail = g.first + (int)((g.onlyShadow.size() + g.inRange.size() + g.outRange.size() + g.glass.size() + g.flip.size()) / kInstanceFloats);
			if (nStand > 0) { L.opaque.emplace_back(); resolveDraw(L, L.opaque.back(), g.part, g.sub, instBuf_, tail * (int)kInstanceBytes, nStand, noBase, fadeStart, fadeEnd, 0, nullptr, nullptr, 1); }
			if (nFade > 0) { L.opaque.emplace_back(); resolveDraw(L, L.opaque.back(), g.part, g.sub, instBuf_, (tail + nStand) * (int)kInstanceBytes, nFade, noBase, fadeStart, fadeEnd, 0, nullptr, nullptr, 2); }
		}
		const int ns = (int)((g.onlyShadow.size() + g.inRange.size()) / kInstanceFloats);
		if (ns > 0)
		{
			const float shadowEnd = def.kind != 0 ? (float)interiorRadius : 0.0f;
			L.shadow.emplace_back();
			resolveShadow(L, L.shadow.back(), g.part, g.sub, instBuf_, g.first * (int)kInstanceBytes, ns, noBase, shadowEnd * kInteriorFade, shadowEnd);
		}
	}
}

// The interior mask (interiormask.glsl): back faces into G, then front faces into R of the open mask pass, every fragment its view depth + 1 - the
// game's two InteriorMask render_scene passes. The instance positions were made relative to this view's camera in update(); mvp is this view's.
void WorldBuildings::renderMask(const DrawList& list, const float mvp[16])
{
	if (list.mask.empty()) { return; }
	for (const sg_pipeline pip : { pipMaskBack_, pipMaskFront_ })
	{
		drawstats::pipeline(pip);
		for (const ResolvedMask& r : list.mask)
		{
			const MeshGpu& mg = list.meshes[r.mesh].get();
			imask_vs_params_t vs = {};
			std::copy(mvp, mvp + 16, vs.mvp);
			std::copy(r.offset, r.offset + 4, vs.offset);
			std::copy(r.spin, r.spin + 4, vs.spin);
			sg_apply_uniforms(UB_imask_vs_params, SG_RANGE(vs));
			sg_bindings b = {};
			b.vertex_buffers[0] = mg.vbuf;
			b.vertex_buffer_offsets[0] = r.firstVertex * kVertexBytes;
			b.vertex_buffers[1] = r.inst;
			b.vertex_buffer_offsets[1] = r.instOffset;
			b.index_buffer = r.ibuf >= 0 ? list.ibufs[r.ibuf].get() : mg.ibuf;
			b.index_buffer_offset = r.firstIndex * 4;
			drawstats::bindings(&b);
			drawstats::draw(0, r.indexCount, r.count);
		}
	}
}

bool WorldBuildings::insideInterior(const double p[3], const double* origin, const double* dir, double* exitT, bool carveOnly) const
{
	bool inside = false;
	for (const HullRef& h : hulls_)
	{
		const BuildingInstance& b = data_.instances[h.instance];
		if (carveOnly && !data_.parts[h.part].carve) { continue; }
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

void WorldBuildings::renderGlass(const DrawList& list, const float mvp[16], const LightParams& light)
{
	if (list.glass.empty()) { return; }
	drawstats::pipeline(pipGlassDepth_);
	applyPassUniforms(light);	// both pipelines are the building shader's: one upload serves the pass
	for (const sg_pipeline pip : { pipGlassDepth_, pipGlass_ })
	{
		drawstats::pipeline(pip);
		for (const ResolvedDraw& r : list.glass) { replayDraw(list, r, sg_buffer{}, mvp); }
	}
}

void WorldBuildings::renderFlip(const DrawList& list, const float mvp[16], const LightParams& light)
{
	if (list.flip.empty()) { return; }
	drawstats::pipeline(pipFlip_);
	applyPassUniforms(light);	// the building shader's program, shared with the other passes; the per-draw blocks (the flip eye ring) follow per draw
	for (const ResolvedDraw& r : list.flip) { replayDraw(list, r, sg_buffer{}, mvp); }
}

void WorldBuildings::cursorRay(const double dir[3], bool on)
{
	for (int k = 0; k < 3; k++) { cursorDir_[k] = dir[k]; }
	cursorOn_ = on;
}

void WorldBuildings::renderIds(const DrawList& list, const float mvp[16])
{
	bool culled = false;
	drawstats::pipeline(pipId_);
	idVsOk_ = false;
	idOwner_ = -1;
	for (const ResolvedId& r : list.ids)
	{
		if (r.cull != culled)
		{
			culled = !culled;
			drawstats::pipeline(culled ? pipIdInterior_ : pipId_);
		}
		const MeshGpu& mg = list.meshes[r.mesh].get();
		sg_bindings b = {};
		b.vertex_buffers[0] = mg.vbuf;
		b.vertex_buffer_offsets[0] = r.firstVertex * kVertexBytes;
		b.vertex_buffers[1] = r.inst;
		b.vertex_buffer_offsets[1] = r.instOffset;
		b.index_buffer = r.ibuf >= 0 ? list.ibufs[r.ibuf].get() : mg.ibuf;
		b.index_buffer_offset = r.firstIndex * 4;
		drawstats::bindings(&b);
		inst_vs_params_t vs;
		memcpy(&vs, r.vs, sizeof vs);
		memcpy(vs.mvp, mvp, sizeof vs.mvp);
		if (!idVsOk_ || memcmp(&vs, idVs_, sizeof vs) != 0) { sg_apply_uniforms(UB_inst_vs_params, SG_RANGE(vs)); memcpy(idVs_, &vs, sizeof vs); idVsOk_ = true; }
		const id_fs_params_t fs = { { r.owner, 0, 0, 0 } };
		if (r.owner != idOwner_) { sg_apply_uniforms(UB_id_fs_params, SG_RANGE(fs)); idOwner_ = r.owner; }
		drawstats::draw(0, r.indexCount, r.count);
	}
}
