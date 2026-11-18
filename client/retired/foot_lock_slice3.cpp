// Retired 2026-11-13 (kenshi-k16.17.7.5): the foot lock's slice 3 (anim.foot_pelvis: the pelvis drop), 3b (anim.stop_pin: the stop / idle-fade pins),
// 3c (anim.foot_pitch: the sole pitch fit), anim.stop_step (the lifted release step) and the fly placement (kenshi-k16.2.64), with the slopeLift
// slope compensation (Characters::update) and the pelvisDrop field. The review (kenshi-inspect findings/foot_ik_review/review_20261112.md, user
// 2026-11-12: "IK never looks right") found them the recurring bug source: five interacting pieces of per-frame state for what the original does
// with a plain blend. The live lockFeet is plain locomotion only (release in combat, at speed ~0, past 20 deg of body yaw from the plant), with one
// same-frame pelvis offset (min of the soles' ground deltas) in the foot-solve pass. A cfg file still setting the four keys below is ignored.
// This is the whole lockFeet as it stood at client a8c3e54c (the last hash with this code live); the Char fields it used (pelvisDrop, slopeLift,
// FootLock stopPin / pinSlow / stepT / stepLen / stepFrom / flyFrom / flyH / idlePi, stopRest) are in characters.hpp at that hash.
// Kept for reference only; not built.

const cfg::Bool kFootPelvis{ "anim.foot_pelvis", false, "Locomotion: lower the body when a pinned leg is at its reach (0 = no drop; a plant past the reach releases)" };
const cfg::Bool kStopStep{ "anim.stop_step", true, "Locomotion: a stop pin's foot that is far from the idle's stance steps there (lifted, 0.25-0.45 s, one foot at a time; 0 = it glides at 4 u/s)" };
const cfg::Bool kStopPin{ "anim.stop_pin", true, "Locomotion: hold a stop's planted foot through the idle fade, then release it at 4 u/s (0 = the lock lets go as the idle snaps in)" };
const cfg::Bool kFootPitch{ "anim.foot_pitch", false, "Locomotion: fit a held foot's unpinned sole point onto the drawn ground on a slope (0 = the slope rotation alone)" };

// Foot lock (foot IK slice 1; CLIENT_GOALS "Locomotion fidelity" > "Foot IK"): a post-process on the blended pose, after the lower layers mix,
// before the palette. While the lower layers' weighted contact windows (deriveContacts) hold a sole point down, the point stays at the world
// position where it planted; two-bone IK (thigh, calf) with the knee in the clip's own bend plane, the foot's orientation kept, so the whole
// foot translates. Release at the window's end or past ~98% of the leg's reach: the foot blends back to the animated pose over 0.15 s. At once
// on an action (technique, block), carry, prone, ragdoll or get-up, and while no lower layer plays. No solver library, no hand-authored data.
// Slice 3 (anim.foot_pelvis): a reach past the leg's length lowers the body by the shortfall (capped, smoothed) instead of releasing, and the
// contact the stop steer lands stays pinned through the locomotion -> idle fade, then blends into the idle fit.
void Characters::lockFeet(Char& c, std::vector<kworld::SkelTransform>& local, std::vector<kworld::SkelTransform>& model, float dt)
{
	constexpr float kRelease = 0.15f;	// the blend back to the animated foot, seconds (MINE)
	constexpr float kReach = 0.98f;	// release past this share of the leg's length (CLIENT_GOALS)
	constexpr float kSlopeIn = 0.1f;	// the slope rotation's ramp-in at the stance's start, seconds (MINE)
	constexpr float kMaxSlope = 0.7f;	// the steepest foot tilt from the ground (40 deg, MINE): past it the ground is a cliff, not a stance
	constexpr float kDropShare = 0.2f;	// slice 3: the deepest pelvis drop, a fifth of the leg's length: at ~8.5 u of thigh and calf (dump bones: the
										// pelvis ~8 u over the feet) that is 1.7 u, past the deepest ask measured (1.48 u, a walk stop's turn; 0.54 u
										// at the slope walk's blocked phases); a deeper drop bends the knee under ~100 deg and reads as a crouch
	constexpr float kDropSmooth = 20.0f;	// slice 3: the pelvis drop's smoothing rate, 1/s (MINE): ~0.1 s to cover a 1 u shortfall
	constexpr float kPinComfort = 0.2f;	// slice 3b: the pinned foot may sit up to a fifth of the leg (~1.7 u at 8.5 u of thigh and calf) from the
										// idle clip's own foot and the idle fit's IK bends to it (knee ~32 deg there, 0.96 of the reach; the pelvis
										// drop covers its y, the same 0.2 share); past that the pin would read as a planted-away stance, so it releases
	constexpr float kPinSpeed = 4.0f;	// slice 3b: the release past that distance is speed-capped, u/s: about a shuffle step's (~2 u in 0.5 s),
										// so the foot never skates faster than a small repositioning step (the old 0.15 s blend, 2 u -> ~13 u/s)
	// anim.stop_step (MINE): a pin released past kPinComfort steps to the idle's foot instead of gliding: the shift runs off on a smoothstep over
	// its horizontal length / kStepSpeed (clamped to a quick step's 0.25..0.45 s) while the foot lifts on a sine arc, kStepLift at a step of
	// kStepFull or longer (shorter steps lift in proportion). The other foot keeps its pin until this one is down: one foot at a time.
	constexpr float kStepSpeed = 15.0f, kStepMin = 0.25f, kStepMax = 0.45f, kStepLift = 1.2f, kStepFull = 3.0f;
	constexpr float kFlyMin = 0.3f, kFlyMax = 0.7f, kFlyLand = 0.75f, kFlyHold = 0.6f;	// kenshi-k16.2.64: a free foot this high (u) at the rest is placed,
										// over at most kFlyMax s, its x/z run in the first kFlyLand of it at least kFlyHold up
	constexpr float kPitchMax = 0.7f;	// slice 3c: the sole pitch fit's cap, rad (the same 40 deg as the terrain tilt: past it the clip's line is no stance)
	constexpr float kPenetrate = 0.02f;	// slice 3c: a sole point this far (u) below the drawn ground is fitted; less is noise, and a point above it stays (the clip's own roll)
	constexpr float kTerrainNear = 1.0f;	// slice 3c: the fit needs the drawn terrain under the foot's plant (u): nearer than this it is the ground, further the foot is on a floor (the body's height)
	const kworld::Skeleton& sk = c.body->skel;
	auto clear = [](Char::FootLock& fl) { fl.active = false; fl.releasing = false; fl.shiftPrev = {}; fl.slopeW = 0; fl.stopPin = 0; fl.pinSlow = false; fl.stepLen = 0; fl.animOk = false; };
	const bool stopped = lockStopped(c);
	int thigh[2], calf[2], foot[2], toe[2];
	for (int f = 0; f < 2; f++)
	{
		thigh[f] = c.body->legBone[f][0];
		calf[f] = c.body->legBone[f][1];
		foot[f] = c.body->legBone[f][2];
		toe[f] = c.body->soleBone[f][1];	// the ball's bone (Toe0)
		if (thigh[f] < 0 || calf[f] < 0 || foot[f] < 0 || sk.bones[(size_t)thigh[f]].parent < 0 || sk.bones[(size_t)calf[f]].parent != thigh[f] || sk.bones[(size_t)foot[f]].parent != calf[f])
		{
			static bool once = false;
			if (!once) { once = true; fprintf(stderr, "kclient: foot lock: %s: no thigh / calf / foot chain, off\n", c.spec.race.c_str()); }
			for (Char::FootLock& fl : c.feetLock) { clear(fl); fl.event = 0; fl.animKnee = -1; fl.dPrev = -1; }
			c.pelvisDrop = 0;
			return;
		}
		if (toe[f] >= 0 && sk.bones[(size_t)toe[f]].parent != foot[f]) { toe[f] = -1; }
	}
	// the weighted contact windows of the lower moving layers, each at its own phase, at their crossfade-would-be weights (vweight): through an
	// inertial transition the snapped weight would end the stance the fade still holds down
	float down[2][2] = {}, total = 0;	// per foot (L, R) and point (heel, ball): the weight saying it is down; the lower layers' weight
	if (!stopped)
	{
		for (const Layer& l : c.layers)
		{
			if (!l.info || l.overlay || (l.info->code != 0 && l.info != &actionInfo_) || l.info->idle || !l.clip || l.clip->length <= 0 || l.vweight <= 0) { continue; }
			const float phase = l.time / l.clip->length;
			total += l.vweight;
			for (int f = 0; f < 2; f++)
			{
				for (int w = 0; w < 2; w++)
				{
					for (const SoleContact& sc : l.contacts.window[f])
					{
						if (sc.point != w) { continue; }
						if (sc.end <= 1.0f ? phase >= sc.start && phase < sc.end : phase >= sc.start || phase < sc.end - 1.0f) { down[f][w] += l.vweight; break; }
					}
				}
			}
		}
	}
	const kworld::SkelQuat unorient = { c.orient.w, -c.orient.x, -c.orient.y, -c.orient.z };
	auto modelAt = [&](const double world[3])
	{
		return kworld::rotate(unorient, { (float)(world[0] - c.pos[0]), (float)(world[1] - c.pos[1]), (float)(world[2] - c.pos[2]) });
	};
	// a model transform placed from its parent the pose() way (only rotations are solved, so the scales stand)
	auto place = [&](int b)
	{
		const int p = sk.bones[(size_t)b].parent;
		const kworld::SkelVec3& ps = c.posSize[(size_t)b];
		const kworld::SkelTransform& pt = model[(size_t)p];
		const kworld::SkelTransform& l = local[(size_t)b];
		const kworld::SkelVec3 o = kworld::rotate(pt.rot, { l.pos.x * ps.x * pt.scale.y, l.pos.y * ps.y * pt.scale.y, l.pos.z * ps.z * pt.scale.y });
		model[(size_t)b].pos = { pt.pos.x + o.x, pt.pos.y + o.y, pt.pos.z + o.z };
		model[(size_t)b].rot = kworld::multiply(pt.rot, l.rot);
	};
	// foot IK slice 2: the drawn ground under a world x/z and its slope normal, from the ground callback (the terrain's triangles). Over 2 u
	// above it the body stands on a floor (SimView's rule, simview.cpp sync): a flat plane at the body's own y, the height the gravity settle
	// fits the soles to. Without the option or the callback the slice-1 lock stands (the plant stays where the animation put it).
	const bool onTerrain = footTerrain_ && ground && std::fabs(ground(c.spec.pos[0], c.spec.pos[2]) - c.spec.pos[1]) < 2.0;
	auto groundAt = [&](double x, double z, kworld::SkelVec3* n) -> double
	{
		if (n) { *n = { 0, 1, 0 }; }
		if (!onTerrain) { return c.spec.pos[1]; }
		const double y = ground(x, z);
		if (n)
		{
			constexpr double e = 2.0;	// MINE: the normal's sample step, u
			const double dx = (ground(x + e, z) - ground(x - e, z)) / (2 * e), dz = (ground(x, z + e) - ground(x, z - e)) / (2 * e);
			const float len = (float)std::sqrt(dx * dx + dz * dz + 1.0);
			*n = { (float)(-dx / len), 1.0f / len, (float)(-dz / len) };
		}
		return y;
	};
	// The slice-2 slope rotation a stance foot gets about its ankle this frame (identity when none): the drop's reach figure uses it too,
	// so it measures the same foot the IK solves (the rotation moves the sole point by up to the foot's length, well past the reach's margin)
	auto slopeQuat = [&](int a, float w, bool* any) -> kworld::SkelQuat
	{
		*any = false;
		if (w <= 0.001f || !footTerrain_ || !ground) { return { 1, 0, 0, 0 }; }
		const kworld::SkelVec3 aw = kworld::rotate(c.orient, model[(size_t)a].pos);
		kworld::SkelVec3 nrm;
		groundAt(c.pos[0] + aw.x, c.pos[2] + aw.z, &nrm);
		const kworld::SkelVec3 nm = kworld::rotate(unorient, nrm);
		const float tilt = std::acos(std::clamp(nm.y, -1.0f, 1.0f));
		const float sw = std::min(w, tilt > kMaxSlope ? kMaxSlope / std::max(tilt, 1e-6f) : 1.0f);	// a cliff is not a stance
		const kworld::SkelQuat dq = qfromTo({ 0, 1, 0 }, nm);
		const kworld::SkelQuat q = { 1 - sw + dq.w * sw, dq.x * sw, dq.y * sw, dq.z * sw };	// nlerp(identity, dq, sw)
		const float ql = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
		if (ql <= 1e-6f) { return { 1, 0, 0, 0 }; }
		*any = true;
		return { q.w / ql, q.x / ql, q.y / ql, q.z / ql };
	};
	// Slice 3c: the held foot's sole on the ground. The slope rotation above tilts the foot to the terrain but keeps the clip's own push-off
	// pitch, so the sole point the lock is not pinning can dig under the drawn ground (up to 0.85 u at the 26 deg slope's blocked phases).
	// The pin puts the point it holds on the ground, so the other point's height there is the sole's own line rise over the ground's line under
	// the two points; when that digs, rotate the foot about the ankle by the angle between the two lines, capped, so the sole lies on the
	// ground and not only the pinned point. A point above the ground is the clip's own roll and stays; roll across the foot has no second pair
	// of points, so it keeps the terrain normal's fit above. Only where the drawn terrain is what the foot stands on: the plant's own height
	// against the terrain under it (on a floor the body's y is the ground, the terrain far below, and slice 1's lock stands). w is the slope
	// rotation's own ramp.
	auto pitchFit = [&](int f, int pi, const kworld::SkelQuat& q, float w) -> kworld::SkelQuat
	{
		if (!footPitch_ || !footTerrain_ || !ground || w <= 0.001f) { return { 1, 0, 0, 0 }; }
		const int sb0 = c.body->soleBone[f][0], sb1 = c.body->soleBone[f][1];
		if (sb0 < 0 || sb1 < 0) { return { 1, 0, 0, 0 }; }
		const kworld::SkelVec3 A = model[(size_t)foot[f]].pos;
		kworld::SkelVec3 P[2];
		for (int pt = 0; pt < 2; pt++)
		{
			kworld::SkelTransform at;
			at.pos = c.body->soleAt[f][pt];
			P[pt] = vadd(A, kworld::rotate(q, vsub(kworld::combine(model[(size_t)(pt ? sb1 : sb0)], at).pos, A)));
		}
		// the ground under where the pin will put the foot, not under the animation: the plant is the shift away (the last one is the
		// reference), and a foot the pin holds 2-3 u from its animation would otherwise be fitted to the ground it left
		const kworld::SkelVec3 sh = c.feetLock[f].shiftPrev;
		kworld::SkelVec3 ws[2];
		for (int pt = 0; pt < 2; pt++) { ws[pt] = kworld::rotate(c.orient, vadd(P[pt], sh)); }
		const double g0 = ground(c.pos[0] + ws[0].x, c.pos[2] + ws[0].z), g1 = ground(c.pos[0] + ws[1].x, c.pos[2] + ws[1].z);
		// the point the pin holds as it stands this frame: its plant; the fit needs the drawn terrain to be what the foot stands on
		if (std::fabs(ground(c.pos[0] + ws[pi].x, c.pos[2] + ws[pi].z) - (c.pos[1] + ws[pi].y)) > kTerrainNear) { return { 1, 0, 0, 0 }; }
		const float dx = P[1].x - P[0].x, dz = P[1].z - P[0].z, len = std::hypot(dx, dz);
		if (len < 1e-3f) { return { 1, 0, 0, 0 }; }
		const float rise = P[1].y - P[0].y - (float)(g1 - g0);	// the ball's height over the ground's line with the heel on it
		// the rotation is about up x the sole line, so a positive angle lowers the ball: the sign is the sole's line minus the ground's
		const float d = std::clamp((std::atan2(P[1].y - P[0].y, len) - (float)std::atan2(g1 - g0, (double)len)) * w, -kPitchMax, kPitchMax);
		if ((pi ? -rise : rise) > -kPenetrate) { return { 1, 0, 0, 0 }; }	// the point the pin is not holding stays on or above the ground
		if (std::fabs(d) < 1e-4f) { return { 1, 0, 0, 0 }; }
		const float half = 0.5f * d, s = std::sin(half);
		return { std::cos(half), (dz / len) * s, 0.0f, (-dx / len) * s };
	};
	// Slice 3: the pelvis drop. A held leg whose plant is past ~98% of its length asks for the body drop that puts the ankle back at
	// the reach (solved in y: the body's height is the only free axis); the body comes down by the deepest ask, smoothed, capped at a
	// share of the leg. Only feet the lock is actually holding this frame count, so the run's flight (no foot held) keeps its height.
	// A foot the cap cannot reach still releases below. The ask is measured from the node as update() built it (no drop), so the drop
	// settling does not feed back into its own target.
	const bool idleChar = std::fabs(c.spec.move) < 0.5f && std::fabs(c.spec.strafe) < 0.5f && c.spec.action.empty();
	const bool fadeDone = c.idle < 0 || (size_t)c.idle >= c.layers.size() || c.layers[(size_t)c.idle].vweight >= 0.999f;	// the idle is all in (the crossfade's end)
	bool overCap[2] = {}, dropEval[2] = {};
	if ((footPelvis_ || stopPin_) && !stopped)
	{
		float target = 0;
		for (int f = 0; f < 2; f++)
		{
			const Char::FootLock& fl = c.feetLock[f];
			const int w = down[f][1] > down[f][0] ? 1 : 0;
			// ds/run-stop-2: the pin's release keeps needing the drop: the foot is still at its plant while the shift comes back, so the body must
			// not rise out from under that leg (the rise over-reached it past the reach and the clamp dragged the foot ~2 u at 25-45 u/s)
			if (fl.stopPin == 3 || (fl.stopPin == 0 && (!fl.active || !(down[f][w] > 0.5f * total)))) { continue; }	// 3: in the air, nothing to reach
			dropEval[f] = true;
			const int h = thigh[f], k = calf[f], a = foot[f], sb = c.body->soleBone[f][fl.point];
			if (h < 0 || k < 0 || a < 0 || sb < 0) { continue; }
			kworld::SkelTransform at;
			at.pos = c.body->soleAt[f][fl.point];
			const kworld::SkelVec3 H = model[(size_t)h].pos, A = model[(size_t)a].pos;
			const float l1 = vlen(vsub(model[(size_t)k].pos, H)), l2 = vlen(vsub(A, model[(size_t)k].pos));
			if (l1 <= 0 || l2 <= 0) { continue; }
			// the sole point as the main loop will see it: after this frame's slope rotation (its own weight update, the same expression)
			const float sw = std::clamp(fl.slopeW + ((footTerrain_ && ground && (down[f][w] > 0.5f * total || fl.stopPin == 1)) ? dt / kSlopeIn : -dt / kRelease), 0.0f, 1.0f);
			bool sloped = false;
			const kworld::SkelQuat sq = slopeQuat(a, sw, &sloped);
			const kworld::SkelQuat rot = kworld::multiply(sloped ? pitchFit(f, fl.point, sq, sw) : kworld::SkelQuat{ 1, 0, 0, 0 }, sq);
			const kworld::SkelVec3 P0 = kworld::combine(model[(size_t)sb], at).pos;
			const kworld::SkelVec3 P = sloped ? vadd(A, kworld::rotate(rot, vsub(P0, A))) : P0;
			const float dmax = kReach * (l1 + l2);
			const kworld::SkelVec3 v = vsub(vadd(A, vsub(modelAt(fl.at), P)), H);
			const float vh2 = v.x * v.x + v.z * v.z;
			if (vh2 + v.y * v.y <= dmax * dmax) { continue; }	// within the reach
			if (vh2 >= dmax * dmax) { overCap[f] = true; continue; }	// the plant is too far sideways for any drop
			const float need = -std::sqrt(dmax * dmax - vh2) - v.y;	// the drop that puts the ankle's distance at the reach
			const float cap = footPelvis_ ? kDropShare * (l1 + l2) : 0.0f;	// the pin alone (no drop): any plant past the reach is over the cap
			if (need > cap) { overCap[f] = true; }
			target = std::max(target, std::min(need, cap));
		}
		c.pelvisDrop += (target - c.pelvisDrop) * std::min(1.0f, dt * kDropSmooth);
		c.pos[1] -= c.pelvisDrop;
	}
	else { c.pelvisDrop = 0; }
	for (int f = 0; f < 2; f++)
	{
		Char::FootLock& fl = c.feetLock[f];
		fl.event = 0;
		fl.animKnee = -1;
		if (stopped) { clear(fl); fl.blocked = false; fl.dPrev = -1; continue; }	// at once, no blend: the action / carry / ragdoll pose owns the feet
		const int w = down[f][1] > down[f][0] ? 1 : 0;
		const bool hold = total > 0 && down[f][w] > 0.5f * total;
		// slice 3: the steered stop's contact is latched at rest and held through the locomotion -> idle fade (past the moving layers'
		// weight, which is what the windows above say); once the idle is all in (or it walks off) it releases into the idle fit (state 2),
		// which waits for that blend so the foot does not jump to the fit's target
		if (stopPin_ && c.stopRest && hold)
		{
			// ds/run-stop-2: a foot the lock had to release past its reach has no plant the pin can keep, and the pelvis drop has already given what it
			// can - the pin takes the animated sole where it is now (the plant branch below reads shiftPrev, so clear it), on the ground, so the rest's
			// own foot is held through the fade instead of released
			if (fl.blocked) { fl.shiftPrev = {}; }
			fl.stopPin = 1;
			fl.blocked = false;
		}
		// both feet down: the settled idle (slice 3: not while a stop pin / its release owns this foot)
		// kenshi-k16.17.7.2 (review cause B): only on terrain whose two feet stand at different heights (> 0.3 u to start, > 0.15 u to stay, the stance's
		// weight fading in by slopeW); on a floor or flat ground the animation's feet are right and the idle fit does nothing
		const bool slopeDelta = onTerrain && std::fabs(ground(c.pos[0] + c.feet[0][0], c.pos[2] + c.feet[0][2]) - ground(c.pos[0] + c.feet[1][0], c.pos[2] + c.feet[1][2])) > (fl.slopeW > 0.01f ? 0.15 : 0.3);
		const bool idleStance = footTerrain_ && onTerrain && slopeDelta && total <= 0 && c.soleFit && c.idleFor >= 0.5f && !(stopPin_ && fl.stopPin != 0);
		const int h = thigh[f], k = calf[f], a = foot[f];
		auto soleY = [&](int pt) { const int b = c.body->soleBone[f][pt]; if (b < 0) { return 1e9f; } kworld::SkelTransform at; at.pos = c.body->soleAt[f][pt]; return kworld::combine(model[(size_t)b], at).pos.y; };
		// kenshi-k16.2.64: a free foot at the rest (the placed step's trigger below) is placed by its lower point, the one that lands first
		const bool flyCand = stopStep_ && stopPin_ && c.stopRest && idleChar && !hold && fl.stopPin == 0 && !fl.active && !fl.releasing;
		int pi;
		if (!idleStance) { fl.idlePi = -1; }
		if (idleStance && fl.idlePi >= 0) { pi = fl.idlePi; }	// per stance, not per frame (kenshi-k16.17.7.4)
		else if (idleStance || flyCand) { pi = soleY(1) < soleY(0) ? 1 : 0; if (idleStance) { fl.idlePi = pi; } }	// the idle's point is the lower of the two (the settle's rule), chosen before the slope rotation moves them
		else { pi = fl.active || fl.stopPin == 3 ? fl.point : w; }	// the planted point stays the one the lock took (heel and ball are far apart)
		const int sb = c.body->soleBone[f][pi];
		if (sb < 0) { clear(fl); fl.dPrev = -1; continue; }
		// slice 2: in stance the foot is pitched and rolled to the drawn ground's slope about the ankle (the clip's own pitch and roll kept),
		// ramped in at the stance's start and blended back with the release below; the toe follows
		const bool stance = footTerrain_ && ground && (hold || fl.stopPin == 1 || idleStance);	// before the pin's fade-end decision below
		fl.slopeW = std::clamp(fl.slopeW + (stance ? dt / kSlopeIn : -dt / kRelease), 0.0f, 1.0f);
		bool sloped = false;
		const kworld::SkelQuat sq = slopeQuat(a, fl.slopeW, &sloped);
		const kworld::SkelQuat rot = kworld::multiply(sloped ? pitchFit(f, pi, sq, fl.slopeW) : kworld::SkelQuat{ 1, 0, 0, 0 }, sq);
		if (sloped)
		{
			model[(size_t)a].rot = kworld::multiply(rot, model[(size_t)a].rot);
			local[(size_t)a].rot = kworld::multiply(qconj(model[(size_t)k].rot), model[(size_t)a].rot);
			if (toe[f] >= 0) { place(toe[f]); }
		}
		const kworld::SkelVec3 H = model[(size_t)h].pos, K = model[(size_t)k].pos, A = model[(size_t)a].pos;
		const kworld::SkelVec3 kh = vsub(K, H), ka = vsub(A, K);	// the animated thigh and calf directions, the IK's start
		const float l1 = vlen(kh), l2 = vlen(ka);
		if (l1 <= 0 || l2 <= 0) { clear(fl); fl.knee = -1; fl.dPrev = -1; continue; }
		fl.animKnee = std::acos(std::clamp(vdot(kh, ka) / (l1 * l2), -1.0f, 1.0f)) * 57.29578f;	// before the lock: what the blend alone did
		kworld::SkelTransform at;
		at.pos = c.body->soleAt[f][pi];
		const kworld::SkelVec3 P = kworld::combine(model[(size_t)sb], at).pos;	// the animated sole point, model space
		const kworld::SkelVec3 wp = kworld::rotate(c.orient, P);	// in world units from the body (the release's own-motion measure)
		const float animPrev[2] = { fl.animAt[0], fl.animAt[1] };
		const bool animPrevOk = fl.animOk;
		fl.animAt[0] = (float)c.pos[0] + wp.x; fl.animAt[1] = (float)c.pos[2] + wp.z; fl.animOk = true;
		// slice 3b: at the fade's end. Within kPinComfort of the idle clip's own foot the pin stays for good and the idle fit's IK bends the
		// leg to it; past that (or a walk-off) the foot releases into the animated idle, speed-capped at kPinSpeed when it is the fit's
		// repositioning (the walk-off keeps the 0.15 s blend)
		if (fl.stopPin == 1)
		{
			bool release = !idleChar;
			fl.pinSlow = false;
			// ds/run-stop-2: the pelvis drop cannot keep this plant (its ask is past the cap, or the plant is sideways past the reach): release
			// now at the speed cap instead of holding a plant the leg must be dragged to - waiting for the fade's end left the reach clamp
			// pulling the pinned sole along the leg (9 u in a frame once the idle's pose had moved away from it)
			// kenshi-k16.2.64: not while the other foot is being placed (in the air): the leg's clamp holds it at the reach until that one is down
			if (!release && stopPin_ && overCap[f] && c.feetLock[1 - f].stopPin != 3) { release = true; fl.pinSlow = true; }
			if (!release && fadeDone && !(stopStep_ && c.feetLock[1 - f].stepLen > 0))	// one step at a time: wait for the other foot to land
			{
				release = std::hypot(c.pos[0] + wp.x - fl.at[0], c.pos[2] + wp.z - fl.at[2]) > kPinComfort * (l1 + l2);
				fl.pinSlow = release;
			}
			if (release)
			{
				fl.stopPin = 2;
				fl.active = false;
				fl.releasing = true;
				fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f);
				fl.blocked = false;
				if (stopStep_ && fl.pinSlow)
				{
					fl.stepFrom = fl.shiftPrev;
					fl.stepT = 0;
					fl.stepLen = std::clamp(std::hypot(fl.shiftPrev.x, fl.shiftPrev.z) / kStepSpeed, kStepMin, kStepMax);
				}
			}
		}
		// kenshi-k16.2.64 (anim.stop_step, MINE): the rest's other foot, in flight (the steer lands one sole contact, not both), would be brought
		// down by the idle's inertial fade while it still moves - the toe touched 0.3 u under the ground and dragged ~2 u at ~20 u/s. It is
		// placed instead (state 3): from where it hangs at the rest its x/z run to the animated foot on a smoothstep over the step's first
		// kFlyLand, coming down to kFlyHold, and the rest is the set down with no ground speed. The x/z run ends with that fade: the idle pose
		// it lands on moves until then (landing inside it slid at 16 u/s; a quick 0.25 s placement then pinned landed past the idle stance's
		// reach and stepped again). The other foot's pin waits for it, even past its reach (one foot at a time: its step lifted with this
		// one still up, and a 4 u/s glide instead ran ~6 u)
		if (flyCand)
		{
			const double gy = groundAt(c.pos[0] + wp.x, c.pos[2] + wp.z, nullptr);
			const float up = (float)(c.pos[1] + wp.y - gy);
			if (up > kFlyMin)
			{
				fl.stopPin = 3;
				fl.point = pi;
				fl.flyFrom[0] = c.pos[0] + wp.x; fl.flyFrom[1] = c.pos[2] + wp.z;
				fl.flyH = up;
				fl.stepT = 0;
				fl.stepLen = kStepMin;
			}
		}
		if (fl.stopPin == 3 && (hold || !idleChar))
		{
			// a walk-off (or a new window) takes the foot back: blend from where it is, the window-end rule
			fl.stopPin = 0;
			fl.stepLen = 0;
			fl.releasing = vlen(fl.shiftPrev) > 1e-4f;
			fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f);
		}
		const bool holdNow = hold || fl.stopPin == 1;
		// the foot translation wanted this frame (== the sole's correction)
		kworld::SkelVec3 shift = {};
		if (!holdNow && !idleStance) { fl.blocked = false; }	// a new window may plant again
		if (idleStance)
		{
			// slice 2: the idle's foot set on the drawn ground under its sole; the body height is the settle's (from the lower foot), so both
			// feet fit with a bend, never past the reach. The foot's x/z follow the idle's own sway; at keeps them for a window that opens next.
			fl.event = fl.active ? 2 : 1;
			fl.active = true;
			fl.point = pi;
			fl.releasing = false;
			fl.blocked = false;
			const double gy = groundAt(c.pos[0] + wp.x, c.pos[2] + wp.z, nullptr);
			fl.at[0] = c.pos[0] + wp.x; fl.at[1] = gy; fl.at[2] = c.pos[2] + wp.z;
			shift.y = (float)(gy - (c.pos[1] + wp.y));
		}
		else if (fl.stopPin == 3)
		{
			const Char::Inertial& in = c.inert;
			fl.stepT += dt;
			if (in.len > 0) { fl.stepLen = std::min(std::max(fl.stepLen, (fl.stepT + in.len - in.t) / kFlyLand), kFlyMax); }
			const float u = std::min(fl.stepT / fl.stepLen, 1.0f), uh = std::min(u / kFlyLand, 1.0f);
			const float ud = std::max(u - kFlyLand, 0.0f) / (1.0f - kFlyLand);	// the set down's own progress
			const float e = uh * uh * (3.0f - 2.0f * uh), ed = ud * ud * (3.0f - 2.0f * ud);
			const double ax = c.pos[0] + wp.x, az = c.pos[2] + wp.z;
			const float animH = (float)(c.pos[1] + wp.y - groundAt(ax, az, nullptr));
			const float hLow = std::min(fl.flyH, kFlyHold);	// the run comes down to this, the set down from it
			double to[3] = { fl.flyFrom[0] + (ax - fl.flyFrom[0]) * e, 0, fl.flyFrom[1] + (az - fl.flyFrom[1]) * e };
			to[1] = groundAt(to[0], to[2], nullptr) + fl.flyH + (hLow - fl.flyH) * e + (animH - hLow) * ed;
			shift = vsub(modelAt(to), P);
			fl.event = 3;
			if (u >= 1.0f) { shift = {}; fl.stepLen = 0; fl.stopPin = 0; }
		}
		else if (holdNow && !fl.active && !fl.blocked)
		{
			// the window's start: the plant is at the foot's current position (with whatever correction the last frame applied), so a window
			// that opens in the middle of a release does not jump; slice 2: set on the drawn ground under it
			fl.active = true;
			fl.point = w;
			const kworld::SkelVec3 wp = kworld::rotate(c.orient, vadd(P, fl.shiftPrev));
			fl.at[0] = c.pos[0] + wp.x; fl.at[2] = c.pos[2] + wp.z;
			fl.at[1] = footTerrain_ && ground ? groundAt(fl.at[0], fl.at[2], nullptr) : c.pos[1] + wp.y;
			fl.releasing = false;
			if (fl.stopPin == 2) { fl.stopPin = 0; fl.pinSlow = false; fl.stepLen = 0; }	// walking off during the idle-fit release: the new window owns the foot
			fl.event = 1;
			shift = vsub(modelAt(fl.at), P);
		}
		else if (holdNow && fl.active) { shift = vsub(modelAt(fl.at), P); fl.event = 2; }
		else if (fl.active)
		{
			// the window ended: blend back from where the foot is
			fl.active = false;
			fl.releasing = true;
			fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f);
			shift = fl.shiftPrev;
			fl.event = 3;
		}
		else if (fl.releasing)
		{
			// back to the animation at the release's own rate (its length over kRelease seconds); the applied shift is the state, so a frame the
			// knee rate limit holds back only makes the return longer, never a jump
			const float sp = vlen(fl.shiftPrev);
			float keep = sp > 1e-4f ? std::max(sp - fl.releaseLen * dt / kRelease, 0.0f) / sp : 0.0f;	// the walk-off's blend
			bool grown = false;
			if (fl.stepLen > 0)
			{
				fl.stepT += dt;
				const float u = std::min(fl.stepT / fl.stepLen, 1.0f), d0 = std::hypot(fl.stepFrom.x, fl.stepFrom.z);
				shift = vmul(fl.stepFrom, 1.0f - u * u * (3.0f - 2.0f * u));
				shift.y += kStepLift * std::min(d0 / kStepFull, 1.0f) * std::sin(3.14159265f * u);
				grown = true;
				if (u >= 1.0f) { shift = {}; fl.stepLen = 0; }
			}
			else if (fl.pinSlow)
			{
				// slice 3b, ds/run-stop: the cap is the foot's *world* speed, and the idle clip's own sole point moves in the same frames (the
				// blend's last steps carry the whole mismatch, up to a frame's 0.1+ u). The shift gives up what is left of the budget; while
				// the animation alone is over it, the rest is taken back along the animation's own (world) direction - which sits at an angle
				// to the shift, so a shave along the shift alone leaves most of it - and the foot is held a frame or two longer
				const float ax = c.pos[0] + wp.x - animPrev[0], az = c.pos[2] + wp.z - animPrev[1];
				const float av = animPrevOk ? std::hypot(ax, az) : 0.0f;
				const float budget = kPinSpeed * dt;
				if (av > budget && sp > 1e-4f)
				{
					const kworld::SkelVec3 dir = kworld::rotate(qconj(c.orient), kworld::SkelVec3{ ax / av, 0.0f, az / av });
					shift = vsub(fl.shiftPrev, vmul(dir, av - budget));
					grown = true;
				}
				else if (sp > 1e-4f) { keep = std::max(sp - (budget - av), 0.0f) / sp; }
			}
			if (!grown) { shift = vmul(fl.shiftPrev, keep); }
			fl.event = fl.blocked ? 4 : 3;	// past the reach: the blocked release, else the window-end blend
			if (vlen(shift) <= 1e-4f) { fl.releasing = false; if (fl.stopPin == 2) { fl.stopPin = 0; fl.pinSlow = false; } }
		}
		const float sl = vlen(shift);
		if (sl <= 1e-4f)
		{
			fl.shiftPrev = {};
			fl.dPrev = vlen(vsub(A, H));
			continue;
		}
		// the ankle target: the foot moves rigidly with the sole point
		kworld::SkelVec3 A2 = vadd(A, shift);
		kworld::SkelVec3 v = vsub(A2, H);
		float d = vlen(v);
		if (d < 1e-4f) { fl.dPrev = d; continue; }
		const float dmax = kReach * (l1 + l2), dmin = std::fabs(l1 - l2) * 1.02f + 1e-3f;
		if (d > dmax && holdNow && fl.active && (!(footPelvis_ || stopPin_) || !dropEval[f] || overCap[f]) && !(fl.stopPin == 1 && c.feetLock[1 - f].stopPin == 3))
		{
			// the lock needs the leg past its reach: release from the last reachable foot pose (clamping to the extended leg would snap the knee),
			// and stay released until the window ends (slice 3: the pelvis drop covers it instead, unless it is past the drop's cap)
			fl.active = false;
			fl.blocked = true;
			fl.event = 4;
			fl.releasing = true;
			fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f);
			shift = fl.shiftPrev;
			A2 = vadd(A, shift);
			v = vsub(A2, H);
			d = vlen(v);
		}
		if (d > dmax || d < dmin)
		{
			const float d2 = std::clamp(d, dmin, dmax);
			A2 = vadd(H, vmul(v, d2 / d));
			shift = vsub(A2, A);
			d = d2;
			v = vsub(A2, H);
		}
		// The knee angle follows from the ankle distance alone (law of cosines), so the shown distance may only move so fast from the last
		// frame's: the desired direction stands, the distance is held inside that step, and the foot slides along the leg instead of the knee
		// snapping. (A release still continues from the applied shift, so holding it back only makes the return longer, never a jump.)
		if (dt > 0 && fl.dPrev > 1e-3f)
		{
			auto kneeDeg = [&](float dd) { return std::acos(std::clamp((dd * dd - l1 * l1 - l2 * l2) / (2 * l1 * l2), -1.0f, 1.0f)) * 57.29578f; };
			auto distAt = [&](float th) { th = std::clamp(th, 0.0f, 180.0f); return std::sqrt(std::max(0.0f, l1 * l1 + l2 * l2 + 2 * l1 * l2 * std::cos(th * 0.01745329f))); };
			const float lim = kKneeRate * dt, thPrev = kneeDeg(fl.dPrev);
			if (std::fabs(kneeDeg(d) - thPrev) > lim)
			{
				// the allowed distances are the angle window's, and at a nearly straight (or folded) knee the two bounds come back swapped: the
				// wider angle clamps at 180 (or 0) and sits on the larger distance, so ordering them is what keeps the limit there - the old
				// dhi > dlo test skipped the clamp entirely at a straight leg and the knee popped on the release (ds/run-stop-2)
				const float dlo = std::max(dmin, distAt(thPrev + lim)), dhi = std::min(dmax, distAt(thPrev - lim));
				const float lo = std::min(dlo, dhi), hi = std::max(dlo, dhi);
				if (hi > lo)
				{
					d = std::clamp(d, lo, hi);
					A2 = vadd(H, vmul(v, d / vlen(v)));
					shift = vsub(A2, A);
					v = vsub(A2, H);
				}
			}
		}
		fl.dPrev = d;
		if (vlen(shift) <= 1e-4f) { fl.shiftPrev = {}; fl.dPrev = vlen(vsub(A, H)); continue; }	// the rate limit took the whole correction: the animated pose stands
		const kworld::SkelVec3 u = vmul(v, 1.0f / d);
		// kenshi-k16.17.7.4: the bend plane is the animated knee's own (the clip's pose), so a small shift moves the knee along its own path and a zero
		// shift (handled above) leaves the pose as it was; the toe's direction only stands in when the leg is nearly straight and the knee has no side
		kworld::SkelVec3 side = vsub(kh, vmul(u, vdot(kh, u)));
		float sw = vlen(side);
		if (sw < 0.05f * l1 && toe[f] >= 0) { const kworld::SkelVec3 fw = vsub(model[(size_t)toe[f]].pos, A); side = vsub(fw, vmul(u, vdot(fw, u))); sw = vlen(side); }
		if (sw < 1e-3f) { side = vcross(u, std::fabs(u.y) < 0.9f ? kworld::SkelVec3{ 0, 1, 0 } : kworld::SkelVec3{ 1, 0, 0 }); sw = vlen(side); }
		if (sw > 1e-6f)
		{
			// the knee stays in the clip's own bend plane (side)
			side = vmul(side, 1.0f / sw);
			const float cosH = std::clamp((l1 * l1 + d * d - l2 * l2) / (2 * l1 * d), -1.0f, 1.0f);
			const float sinH = std::sqrt(std::max(0.0f, 1.0f - cosH * cosH));
			const kworld::SkelVec3 K2 = vadd(vadd(H, vmul(u, l1 * cosH)), vmul(side, l1 * sinH));
			const kworld::SkelQuat q1 = qfromTo(vmul(kh, 1.0f / l1), vmul(vsub(K2, H), 1.0f / l1));
			const kworld::SkelQuat q2 = qfromTo(vmul(ka, 1.0f / l2), vmul(vsub(A2, K2), 1.0f / l2));
			const kworld::SkelQuat Rp = sk.bones[(size_t)h].parent >= 0 ? model[(size_t)sk.bones[(size_t)h].parent].rot : kworld::SkelQuat{};
			const kworld::SkelQuat Rt = model[(size_t)h].rot, Rc = model[(size_t)a].rot;
			local[(size_t)h].rot = kworld::multiply(kworld::multiply(kworld::multiply(qconj(Rp), q1), Rp), local[(size_t)h].rot);
			model[(size_t)h].rot = kworld::multiply(Rp, local[(size_t)h].rot);
			local[(size_t)k].rot = kworld::multiply(kworld::multiply(kworld::multiply(qconj(model[(size_t)h].rot), q2), Rt), local[(size_t)k].rot);
			place(k);
			local[(size_t)a].rot = kworld::multiply(qconj(model[(size_t)k].rot), Rc);	// the foot keeps its orientation
			place(a);
			if (toe[f] >= 0) { place(toe[f]); }
			fl.shiftPrev = vsub(A2, A);	// what the next frame starts from (the plant and the release both)
		}
	}
	for (Char::FootLock& fl : c.feetLock) { if (fl.active) { fl.held++; } }
}
