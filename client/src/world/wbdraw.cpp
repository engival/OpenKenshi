#include "world/worldbuildings.hpp"
#include "world/worldbuildings_internal.hpp"

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
constexpr float kGlassBuild = 200000.0f;	// a peeked part: stretch w = -(kGlassBuild + opacity x 1000) (mesh.glsl inst_vs)
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
const cfg::Bool kLeafSway{ cfg::Group{ "Detail" }, "render.leaf_sway", false, "A tree's leaves sway in the wind (off: the original, whose leaves do not move; edits are picked up while the client runs)" };
constexpr float kInteriorFade = 0.8f;	// the dithered fade of interior parts and furniture starts at this fraction of the interior radius
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
