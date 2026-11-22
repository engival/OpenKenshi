#include "world/worldbuildings.hpp"
#include "world/worldbuildings_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "kworld/coords.hpp"

using namespace wbi;

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
#ifdef KCLIENT_DEV_TOOLS
		if (logFloor && view_.latchInfo().on)	// kenshi-k16.2.236.8.4.3: the latch's feet against its lines, when the feet moved 0.5 u or the latch changed state
		{
			const auto& li = view_.latchInfo();
			static double lastY = -1e9; static int lastSide = -1; static bool lastFired = false;
			if (std::fabs(li.y - lastY) >= 0.5 || li.side != lastSide || li.fired != lastFired)
			{
				lastY = li.y; lastSide = li.side; lastFired = li.fired;
				printf("floor: latch y %.2f up %.2f low %.2f origin %d side %d %s rearm %.2f\n", li.y, li.upperLine, li.lowerLine, li.origin, li.side, li.fired ? "fired" : "armed", li.rearmY);
			}
		}
#endif
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
	const BInfo* info = building >= 0 ? infoFor((uint32_t)building) : nullptr;
	if (!info || !info->hasBounds) { return false; }
	double l[3];
	toLocal(data_.instances[building], p, l);
	return l[1] < info->mn[1] - kFoundSlab;
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
	xrayEx[3] = 0;
	if (hasSeen)
	{
		// the building ours are in (sliced), else the one the camera is inside of (its parts' box), else the one seen is indoors in
		int ex = cuts_.empty() ? -1 : (int)cuts_.front();
		if (ex < 0)
		{
			for (const uint32_t i : topLevel_)
			{
				const BuildingInstance& b = data_.instances[i];
				if (std::abs(b.x - cam[0]) > kFocusRange || std::abs(b.z - cam[2]) > kFocusRange) { continue; }
				const BInfo* info = infoFor(i);
				double l[3];
				toLocal(b, cam, l);
				bool in = info && info->hasBounds;
				for (int a = 0; in && a < 3; a++) { in = l[a] >= info->mn[a] && l[a] <= info->mx[a]; }
				if (in) { ex = (int)i; break; }
			}
		}
		const BInfo* info = ex >= 0 ? infoFor((uint32_t)ex) : nullptr;
		if (info && info->outerMn[0] <= info->outerMx[0])
		{
			const BuildingInstance& b = data_.instances[ex];
			constexpr double kPad = 6, kBelow = 40;	// MINE: local units round the parts' box; the foundation reaches under the lowest part
			const double c[3] = { (info->outerMn[0] + info->outerMx[0]) / 2, (info->outerMn[1] + info->outerMx[1]) / 2 - kBelow / 2, (info->outerMn[2] + info->outerMx[2]) / 2 };
			const double cs[3] = { c[0] * b.scale, c[1] * b.scale, c[2] * b.scale };
			double w[3];
			const float q[4] = { b.qw, b.qx, b.qy, b.qz };
			rotate(q, cs, w);
			xrayEx[0] = b.x + w[0];
			xrayEx[1] = b.y + w[1];
			xrayEx[2] = b.z + w[2];
			xrayEx[4] = ((info->outerMx[0] - info->outerMn[0]) / 2 + kPad) * b.scale;
			xrayEx[5] = ((info->outerMx[1] - info->outerMn[1]) / 2 + kBelow / 2 + kPad) * b.scale;
			xrayEx[6] = ((info->outerMx[2] - info->outerMn[2]) / 2 + kPad) * b.scale;
			xrayEx[7] = 2 * std::atan2((double)b.qy, (double)b.qw);	// yaw about the up axis
			xrayEx[3] = 1;
		}
	}
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

// kenshi-k16.2.412.3: p is in the solid zone (foundation + ground floor) of a ghosted occluder: drawn opaque under the fade (mesh.glsl xray_fnd), cone or not
bool WorldBuildings::inSolidZone(const double p[3])
{
	for (const uint32_t bi : occluders_)
	{
		const BInfo* info = infoFor(bi);
		if (!info) { continue; }
		double l[3];
		toLocal(data_.instances[bi], p, l);
		bool in = l[1] <= info->foundY;
		for (int a = 0; a < 3 && in; a++) { in = l[a] >= info->outerMn[a] && l[a] <= info->outerMx[a]; }
		if (in) { return true; }
	}
	return false;
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
		return !(levelCuts && l[1] <= info->foundY);	// the click pick: the foundation and ground floor stay drawn solid under the ghost (kenshi-k16.2.411.1), so they are hit (412.3)
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
		if (double lo[3], hi[3]; partWorldBox(b, k, lo, hi))	// the part's mesh box as the client draws it, a world AABB (kenshi-uuh8: against the sim's stair hull, u59y)
		{
			snprintf(line, sizeof line, "    world box %.1f %.1f %.1f - %.1f %.1f %.1f%s\n", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], d.stairs ? " (stairs part)" : "");
			out += line;
		}
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
