#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include "characters/characters.hpp"
#include "app/cpustage.hpp"
#include "knobs/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "render/drawstats.hpp"
#include "assets/sidorder.hpp"
#include "app/eventlog.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>

#include "render/ao.hpp"
#include "render/lightvol.hpp"
#include "app/config.hpp"
#include "assets/dds.hpp"
#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "assets/ogremesh.hpp"
#include "render/speccube.hpp"
#include "stb_image.h"
#include "render/weathermap.hpp"

#include "character.glsl.h"
#include "characters/characters_internal.hpp"

namespace
{

const cfg::Float kIdleDraw{ cfg::Group{ "Locomotion" }, "anim.idle_draw", 0.0, 0.0, 1.0, "The standing idle pick (kenshi-k16.2.162.1, rwh101): 1 = the original's weighted random draw, held for the record's `idle time` (stealth idle 10-40 s); 0 = the first maximum in its idle list's order (deterministic, what its combat picker does). Every ANIMATION record of this data has idle time 10..40, so the draw makes a sneaking armed character flip between `stealth idle` and `stand 1 sword`: 0 keeps the crouch the original shows" };
// anim.pose_threads (kenshi-k16.2.205.12.102, overlap review .85 B2 / T22): the threads Characters::update's pose pass runs on, the calling (logic)
// thread included; 1 = the serial loop. The output is the same at any count (each pose writes its own character only).
const cfg::Int kPoseThreads{ cfg::Group{ "Performance" }, "anim.pose_threads", 4, 1, 16, "Characters: threads the per-frame pose sampling runs on, the logic thread included (1 = serial; the poses are the same at any count)" };
// The contact table's resolution (MINE): phase samples per clip cycle, and how close to the cycle's lowest sole a point is on the ground.
constexpr double kOnTerrain = 0.1;	// the heightmap within this of the wire y: the character stands on terrain (kenshi-k16.2.339; stage 4 replaces it with the wire surface-kind byte)
constexpr int kContactSamples = 96;
constexpr float kContactHeight = 0.4f;
constexpr double kActMaxOff = 1.0;
// foot lock (foot IK slice 1): a per-frame knee step past this reads as a pop (the swing-foot sanity, log feet; MINE)
constexpr float kKneePopDeg = 15.0f;
// the applied knee may move only so fast: past this the foot slides a little instead (720 deg/s = 12 deg/frame at 60 fps, the pop threshold
// with a margin for frames that run a little long)
constexpr float kKneeRate = 720.0f;

}

// A window's weight (14051cd10, used for speed bands and leg health): rises lo..mid, falls mid..hi; open below when lo < 1, above when hi < 0.5.
// Constants verified (pe.py): lower clamp DAT_141681b38 = 0.0, open-below -40 (1416e459c), open-above 0.5 (14167e8d8), 1.0 (14167c308).
static float windowHat(float x, float lo, float hi, float mid)
{
	if (x < -40 && lo < -40) { return 1.0f; }
	float w = 0;
	if (x < lo || x > mid)
	{
		if (mid <= x && (x <= hi || hi < 0.5f)) { w = 1.0f - (x - mid) / (hi - mid); }
	}
	else { w = lo < 1.0f ? 1.0f : (x - lo) / (mid - lo); }
	return std::clamp(w, 0.0f, 1.0f);
}

// Locomotion (docs/reference/LOCOMOTION.md): moving, every moving record weighs in by its speed band at x = spec.move (hat, sharpened by a sigmoid, the
// upper list's weapon filter), normalised per layer, the synching clips on one shared phase that advances x x the weighted play speed of the lower list
// (or, with anim.stride_warp, by the clips' measured stance ground: task L4).
// At rest the idle chooser: the stored stance if it fits, else the highest `idle chance` (the first of equals). Weights move linearly toward their targets
// at the layer's rate (faster fading in at a fast clip). instant: weights jump (the first frame).
// Combat (vf28, docs/reference/COMBAT_ANIM.md): the lower list weighs in at any speed (the shuffle holds still at 0), the chosen idle is the guard stance
// (weight = its band hat, so it stays alone on layer 1), and `strafe lower` takes frac = |lateral| / (|fwd| + |lateral|) of the lower layer.
// An action (technique, stumble) takes layer 1 alone: the rest of layer 1 fades out, the lower layer is capped at 1 - its weight; it plays from 0 at its
// speed and is cut at progress 0.98 (fading out), the clips it displaced restarting from time 0.
// Carrying: the admitted records are the carry variants (locoAdd), plus the arm overlay.
// NOT YET: the record flag that skips the leg window (+0x9f), the idle re-pick timer (the chooser is deterministic here), hesitate holds, `slave anim`
// (the carrier's clip names `carry me` for the carried; whether it syncs the two is not traced).
void Characters::locomote(Char& c, float dt, bool instant)
{
	const Spec& s = c.spec;
	// every locomotion layer carries its record; one without (a pose clip left on a character that walks again) crashed the client
	// (2026-11-06, characters.cpp:1661). Loud, once per character, and this character's locomotion stops instead of the client.
	for (const Layer& l : c.layers)
	{
		if (l.info) { continue; }
		if (!c.infoless)
		{
			std::string anims;
			for (const std::string& a : s.anims) { anims += " " + a; }
			fprintf(stderr, "kclient: BUG: moving character%s with a record-less layer (should not happen, see layer-null): clip %s, %zu layers, move %.2f clip '%s' anims [%s ] action '%s' carriedBy %d carry %d posed %d\n",
				idTag(s).c_str(), l.clip ? l.clip->name.c_str() : "?", c.layers.size(), s.move, s.clip.c_str(), anims.c_str(), s.action.c_str(), s.carriedBy, s.carry, (int)s.posed);
			c.infoless = true;
		}
		return;
	}
	// Speed == 0 is the idle chooser (140520500, LOCOMOTION.md 1); the sim's stop gives exactly 0 but the drawn body's measured velocity (the catch-up, the trimmed
	// mean) leaves a residue of hundredths of a unit a second, and any non-zero speed weighs the walk clips and the weights are normalised to 1, so a standing guard
	// held `walk lower` / `walk upper` at frame 0 (a frozen stride, arms hanging; kenshi-h2v.11.5.1). MINE: below 0.5 u/s (a tenth of the slowest walk) the body is at rest.
	constexpr float kRestSpeed = 0.5f;
	const float x = std::fabs(s.move) < kRestSpeed ? 0.0f : s.move;
	auto stateFits =[&](const AnimInfo& a) { return (a.combat == 2 || (a.combat == 1) == s.combat) && (a.stealth == 2 || (a.stealth == 1) == s.stealth); };
	// the weapon filter (14051d660, layer 1/2 records): the held category's bit, or unarmed, which damps records that want a weapon
	auto weaponFactor = [&](const AnimInfo& a)
	{
		if (a.code != 1 && a.code != 2) { return 1.0f; }
		if (c.weaponCategory == 5) { return a.handL == 1 || a.handR == 1 ? 0.1f : 1.0f; }
		return a.weaponBits & (1u << c.weaponCategory) ? 1.0f : 0.0f;
	};
	// the leg window (14051d660): the worse leg's value against that leg's window. The window peaks at the record's `max` and closes at its `ideal`
	// (cd10(v, min, ideal, max)): the only reading that fits the live samples (kinspect legs_anim.md: healthy 100 = `walk lower` 1 (60..100, ideal 90),
	// left leg 49.7 = `limp L lower` 1 (-100..75, ideal 50))
	auto legFactor = [&](const AnimInfo& a)
	{
		const int i = s.legs[0] <= s.legs[1] ? 0 : 1;
		return windowHat(s.legs[i], a.leg[i][0], a.leg[i][2], a.leg[i][1]);
	};
	Layer* action = c.action >= 0 ? &c.layers[(size_t)c.action] : nullptr;
	const bool acting = action && !c.actionDone;
	// strafe split (14051de70): frac of the lower layer to `strafe lower`, played at play speed x lateral speed
	const float sum = std::fabs(x) + std::fabs(s.strafe);
	// kenshi-k16.2.237.8: out of combat a body backing away (the direct drive's S) plays the forward walk LOWER clips reversed, but its upper body takes the
	// idle pick (the carry / weapon pose, no arm swing): the reversed upper walk clips swung the arms wrong (user 2026-11-18)
	const bool backUpper = !s.combat && x < 0;
	const float frac = (s.combat || s.strafeDrive) && sum > 1 ? std::fabs(s.strafe) / sum : 0.0f;
	float total[5] = {};
	for (Layer& l : c.layers)
	{
		const AnimInfo& a = *l.info;
		if (&a == &actionInfo_) { continue; }
		if (l.overlay)
		{
			// vf30: the pain overlay only below speed 50 (141683170; MINE: not during an action), the carry arm at any speed
			l.target = s.carry || (x <= 50 && !acting) ? 1.0f : 0.0f;
			l.rate = a.play;
			continue;
		}
		l.target = 0;
		l.rate = a.idle ? a.play : a.play * x;	// K = 1 for humans
		if (a.move == 0 && !a.idle)
		{
			// strafe lower (the only one admitted with no band): timed by the lateral speed over its planted stride (plan 3, coordinator
			// 2026-11-07), never by the forward-driven shared phase; the record's play sign keeps the clip's direction. A bad planted stride
			// is caught by the warp's same 12x sanity guard, logged once per character and clip.
			const float base = a.play * s.strafe;	// the original's rate: the sanity reference
			l.rate = l.contacts.stride > 0 ? (a.play < 0 ? -s.strafe : s.strafe) / l.contacts.stride : base;	// the direction stays the record's
			if (base != 0 && std::fabs(l.rate) > 12.0f * std::fabs(base))
			{
				l.rate = std::copysign(12.0f * std::fabs(base), l.rate);
				if (std::find(c.warpClamped.begin(), c.warpClamped.end(), a.name) == c.warpClamped.end())
				{
					c.warpClamped.push_back(a.name);
					fprintf(stderr, "kclient: stride warp: %s clip \"%s\" (lateral) clamped to 12x (stride %.2f u/cycle, lateral %.1f u/s)\n",
						label(c.spec).c_str(), a.name.c_str(), l.contacts.stride, s.strafe);
					evlog::add("warp", -1, c.spec.id, "%s clip \"%s\" (lateral) clamped to 12x (stride %.2f u/cycle, lateral %.1f u/s)", label(c.spec).c_str(), a.name.c_str(),
						l.contacts.stride, s.strafe);
				}
			}
			l.target = frac > 0 && (stateFits(a) || s.strafeDrive) ? frac : 0.0f;
			continue;
		}
		if ((x == 0 && !s.combat) || a.idle || !stateFits(a)) { continue; }
		if (backUpper && a.code == 1) { continue; }
		// kenshi-k16.2.237.1 (MINE): the game data has no backward clip outside combat (`walk lower combat shuffle long BK` is the only backward set and its state
		// filter is combat), so out of combat a body backing away (the direct drive's S) takes the forward walk / jog / run bands by its speed's magnitude; the
		// layer's rate keeps the speed's sign, so the clip plays in reverse, upper body (arms) included, as the combat sets' negative play speeds do.
		const float hat = windowHat(s.combat ? x : std::fabs(x), a.min, a.max, a.move);
		if (hat <= 0) { continue; }
		l.target = 1.0f / (1.0f + std::exp(-(2.0f * hat - 1.0f) * 10.0f)) * legFactor(a) * (a.upper ? weaponFactor(a) : 1.0f);
		total[a.code] += l.target;
	}
	if (x == 0 || s.combat || backUpper)
	{
		// The idle pick as the original does it (kenshi-rwh.101, rwh101_idle_pick_static.md; the gates H, the order H for the stock set):
		// - the gates are pass/fail: category (the idle list), `is combat mode` / `stealth mode` (2 or equal; nothing ranks an exact match over an `either`), `crouched`
		//   (equal to the crouch flag, which the client never sets: 0), the arm/hand fit; the weights are the weapon category's bit, the leg window and, for the normal
		//   picker only, `idle chance`; the combat picker also has the speed window's logistic (a record's band at the drawn speed).
		// - normal (14051ec80), anim.idle_draw 1: a WEIGHTED RANDOM draw over the fitting records in the idle list's order (cumulative weights, rand x total); the drawn record is held
		//   until it stops fitting or its dwell, rand x (`idle time max` - `idle time min`) + `idle time min` s, runs out (14051d3b0), so a record with no dwell is drawn again
		//   about every frame and one with a dwell (stealth idle 10-40 s) holds the pose. The generator is ours (a per-character xorshift seeded from its slot, so
		//   headless shots are stable); the exe's is rand().
		//   With anim.idle_draw 0 (the default, MINE: a deliberate deviation, rwh101 section 5) the normal pick is the combat picker's first maximum instead: every ANIMATION record
		//   of this data has `idle time` 10..40 (not only `stealth idle`, which the findings assume), so the draw would hold `stand 1 sword` half the time a sneaking armed
		//   character is drawn, and the original shows the crouch (k16.2.160.1).
		// - combat (14051d660 + 140523450): the FIRST maximum (strict <) over the same candidates in the idle list's order, held rand x 60 + 5 s.
		// - the order is the walk of the ANIMATION index in sid hash order (sidorder.hpp, AnimInfo::order), not load order.
		// MINE, not from the exe: the character's stored stance (Spec::stance) is tried first when it fits (the exe does the same through animclass+0x2e0, LOCOMOTION.md 1);
		// none fitting leaves no idle (the exe falls back to the first record of the list); the combat picker's hand-carry byte match is not modelled.
		const bool combatPick = s.combat;
		auto fits = [&](const Layer& l) { const AnimInfo& a = *l.info; return a.idle && !l.overlay && stateFits(a) && !a.crouched && handsFit(c, a); };
		auto idleWeight = [&](const Layer& l)
		{
			if (!fits(l)) { return 0.0f; }
			const AnimInfo& a = *l.info;
			float w = weaponFactor(a) * legFactor(a);
			if (combatPick)
			{
				const float hat = windowHat(x, a.min, a.max, a.move);
				w *= 1.0f / (1.0f + std::exp(-(2.0f * hat - 1.0f) * 10.0f));
			}
			else { w *= a.idleChance; }
			return w;
		};
		const int state = (s.combat ? 1 : 0) | (s.stealth ? 2 : 0);
		const bool held = c.idle >= 0 && c.idleCombat == combatPick && c.idleState == state && idleWeight(c.layers[(size_t)c.idle]) > 0;
		c.idleTimer -= dt;
		if (!held || c.idleTimer < 0)
		{
			if (c.idleRng == 0)
			{
				const std::uint32_t slot = (std::uint32_t)(&c - chars_.data());	// a temporary Char (the portrait copy) is out of the vector: any slot will do
				c.idleRng = 2463534242u ^ (slot * 2654435761u + 0x9e3779b9u);
				if (c.idleRng == 0) { c.idleRng = 1; }
			}
			auto rnd = [&]() { c.idleRng ^= c.idleRng << 13; c.idleRng ^= c.idleRng >> 17; c.idleRng ^= c.idleRng << 5; return (float)(c.idleRng >> 8) * (1.0f / 16777216.0f); };
			c.idle = -1;
			const auto st = anims_.find(folded(s.stance));
			if (!combatPick)
			{
				for (size_t i = 0; i < c.layers.size() && st != anims_.end(); i++)
				{
					if (c.layers[i].info == &st->second && fits(c.layers[i])) { c.idle = (int)i; }
				}
			}
			if (c.idle < 0)
			{
				std::vector<size_t> cand;	// the fitting records, in the idle list's order
				for (size_t i = 0; i < c.layers.size(); i++) { if (idleWeight(c.layers[i]) > 0) { cand.push_back(i); } }
				std::sort(cand.begin(), cand.end(), [&](size_t p, size_t q) { return c.layers[p].info->order < c.layers[q].info->order; });
				if (combatPick)
				{
					float best = 0;
					for (const size_t i : cand) { if (const float w = idleWeight(c.layers[i]); best < w) { best = w; c.idle = (int)i; } }	// strict <: the first maximum wins
				}
				else if (kIdleDraw.get() < 0.5)
				{
					float best = 0;
					for (const size_t i : cand) { if (const float w = idleWeight(c.layers[i]); best < w) { best = w; c.idle = (int)i; } }
				}
				else
				{
					float total = 0;
					for (const size_t i : cand) { total += idleWeight(c.layers[i]); }
					const float r = rnd() * total;
					float run = 0;
					for (const size_t i : cand)
					{
						run += idleWeight(c.layers[i]);
						if (r < run) { c.idle = (int)i; break; }
					}
					if (c.idle < 0 && !cand.empty()) { c.idle = (int)cand.back(); }
				}
			}
			c.idleCombat = combatPick;
			c.idleState = state;
			const AnimInfo* picked = c.idle >= 0 ? c.layers[(size_t)c.idle].info : nullptr;
			c.idleTimer = combatPick ? rnd() * 60.0f + 5.0f : picked ? rnd() * (picked->idleTimeMax - picked->idleTimeMin) + picked->idleTimeMin : 0.0f;
		}
		if (c.idle >= 0)
		{
			// in combat the guard stance weighs its band (-99..0..99), so it fades as speed rises (MINE: no sigmoid)
			Layer& l = c.layers[(size_t)c.idle];
			l.target = s.combat ? windowHat(x, l.info->min, l.info->max, l.info->move) : 1.0f;
			total[l.info->code] += l.target;
		}
		else if (!acting && !c.noIdleLogged)
		{
			// kenshi-k16.2.448: a body at rest with no idle clip fitting draws the bind pose (T-pose). Loud, once per character (dev only).
			c.noIdleLogged = true;
			std::string anims;
			for (const std::string& a : s.anims) { anims += " " + a; }
			fprintf(stderr, "kclient: no idle clip: %s stance '%s' combat %d stealth %d handL %d handR %d weaponCategory %d legs %.0f/%.0f action '%s' clip '%s' carry %d carriedBy %d posed %d layers %zu anims [%s ]\n",
				label(s).c_str(), s.stance.c_str(), (int)s.combat, (int)s.stealth, (int)c.handL, (int)c.handR, (int)c.weaponCategory, (double)s.legs[0], (double)s.legs[1],
				s.action.c_str(), s.clip.c_str(), (int)s.carry, (int)s.carriedBy, (int)s.posed, c.layers.size(), anims.c_str());
			evlog::add("noidle", -1, s.id, "%s no idle clip (stance '%s' handL %d handR %d weapon %d)", label(s).c_str(), s.stance.c_str(), (int)c.handL, (int)c.handR, (int)c.weaponCategory);
		}
	}
	float phaseRate = 0, strideSum = 0;
	for (Layer& l : c.layers)
	{
		if (l.target <= 0 || l.info == &actionInfo_ || l.overlay) { continue; }
		if (l.info->code != 0 || l.info->move != 0 || l.info->idle) { l.target /= total[l.info->code]; }
		if (l.info->code == 0 && l.info->move != 0) { l.target *= 1.0f - frac; }
		if (acting && l.info->code == 1) { l.target = 0; }
		if (!l.info->idle && !l.info->upper && !(l.info->code == 0 && l.info->move == 0))	// the shared phase: never the strafe lower (move 0: timed by lateral speed)
		{
			phaseRate += l.target * l.info->play;
			if (l.info->code == 0 && l.contacts.stride > 0) { strideSum += l.target * l.contacts.stride * (l.info->play < 0 ? -1.0f : 1.0f); }
		}
	}
	// kenshi-k16.2.237.8: KCLIENT_DRIVELOG=<id> logs the chosen lower / upper clips and rates of that character each frame (the drive's S / strafe check)
	if (static const char* dl = getenv("KCLIENT_DRIVELOG"); dl && (s.strafeDrive || x < 0) && (uint64_t)strtoull(dl, nullptr, 10) == s.id)
	{
		std::string o;
		for (const Layer& l : c.layers)
		{
			if (l.target > 0.01f && l.info && !l.overlay) { char b[160]; snprintf(b, sizeof b, " [%s w%.2f r%.3f]", l.info->name.c_str(), (double)l.target, (double)l.rate); o += b; }
		}
		fprintf(stderr, "drivelog: combat %d move %.1f lat %.1f frac %.2f clips:%s\n", (int)s.combat, (double)s.move, (double)s.strafe, (double)frac, o.c_str());
	}
	// stride warp (task L4): the phase rate from the clips' planted stride (deriveContacts: the ground per cycle with the feet held), so a
	// planted foot stays put. 434d373a's 1.4x band was too tight for a run (the clip's own cadence lags the sim's speed), but fdbea25c's
	// 12x let the legs whirl at several times cadence through a stop: MINE, the forward-clip bound is 2.5x, and a warp past 2x is logged once
	// per clip (the hand-off's k in the stop steer must stay under 1.4 to read as plain footing).
	float advance = x * phaseRate;
	if (strideWarp_ && strideSum != 0 && advance != 0)
	{
		const float warped = x / strideSum;
		if ((advance > 0) == (warped > 0))
		{
			const float base = advance, lo = 0.7f * advance, hi = 2.5f * advance;
			advance = std::fabs(warped) < std::fabs(lo) ? lo : warped;
			const float warp = advance / base;
			if (std::fabs(warp) > 2.0f)
			{
				const Layer* heaviest = nullptr;	// the clip that asked: the heaviest synching lower one
				for (const Layer& l : c.layers)
				{
					if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || !l.synchs || l.target <= 0 || l.contacts.stride <= 0) { continue; }
					if (!heaviest || l.target * l.contacts.stride > heaviest->target * heaviest->contacts.stride) { heaviest = &l; }
				}
				// The clips are the same for every character, so log once per clip, not once per character (13 characters x 2-3 clips was 34 lines).
				static std::vector<std::string> warpLogged;
				if (heaviest && std::find(warpLogged.begin(), warpLogged.end(), heaviest->info->name) == warpLogged.end())
				{
					warpLogged.push_back(heaviest->info->name);
					fprintf(stderr, "kclient: stride warp: clip \"%s\" warped %.1fx%s (stride %.2f u/cycle, speed %.1f u/s)\n",
						heaviest->info->name.c_str(), warp, std::fabs(advance) > std::fabs(hi) ? ", clamped to 2.5x" : "", heaviest->contacts.stride, x);
					evlog::add("warp", -1, 0, "clip \"%s\" warped %.1fx%s (stride %.2f u/cycle, speed %.1f u/s)", heaviest->info->name.c_str(), warp,
						std::fabs(advance) > std::fabs(hi) ? ", clamped to 2.5x" : "", heaviest->contacts.stride, x);
				}
			}
			if (std::fabs(advance) > std::fabs(hi)) { advance = hi; }
			c.warped = true;
		}
	}
	// stop contact (task L3, ds/stop-fix): the sim brakes at the race's `pathfind acceleration` (the drawn speed steps down at that rate),
	// so a phase rate k x the natural one covers k x the natural travel (v^2 / 2a) and can be steered to put the rest inside a sole contact
	// interval. The aim is latched once per stop (434d373a) from the blended lower clips: a rest phase counts while one sole point of the blend
	// is more than half down there (the foot lock's own rule), and the aim is the middle of the nearest such stretch the steer's 0.7..1.4 band
	// reaches. fdbea25c re-aimed every frame in a 0.7..12 band and, as nat shrinks with x^2, k climbed to the top near v = 0 and the legs
	// whirled at several times cadence through a stop (user 2026-11-07).
	// ds/stop-fix: the decel is the sim's own speed's, not the drawn one's - a catch-up step raises the drawn speed and drops it again, and the end of
	// such a step read as a big decel and fired the steer mid-run (simview's vSim)
	// kenshi-qaud.1: the sim's speed is a step function of the samples (one value per 0.05 s tick, between steps it holds), so the decel is a step's
	// size over the time since the previous step, not over this one frame: at 60 Hz a 2.3 u/s wobble in a 70 u/s walk read as 138 u/s^2 (3x its
	// real 46) and armed the steer mid-walk. Frames between steps carry no decel.
	float decel = 0.0f;
	if (dt > 0)
	{
		c.moveSimAge += dt;
		if (std::fabs(s.moveSim - c.moveSimPrev) > 1e-3f)
		{
			decel = (c.moveSimPrev - s.moveSim) / c.moveSimAge;
			c.moveSimAge = 0;
		}
	}
	c.moveSimPrev = s.moveSim;
	// off when the body stops, when it speeds up, or when the speed has not fallen for a few sim ticks: a ramp down to a *lower* limit
	// (a corner, a slower order) ends there, and the steer must not keep holding the phase to its windows in a normal cycle
	constexpr float kStopQuiet = 0.12f;	// seconds without a decel step (the sim's ramp steps once per tick, 0.05 s)
	// kenshi-qaud.1: and only when the sim is stopping: its route's last waypoint is within the braking distance (v^2 / 2a, with a 50% margin and a
	// 30 u slack for the standing point). A route with waypoints still ahead is a corner or a leg's slower limit (the sim dips to ~22 u/s at a corner
	// of a 70 u/s walk and appends the next leg); the steer took each as a stop. No route on the wire (-1): the plain decel rule as before.
	const float vStop = std::max(x, s.moveSim);
	const bool routeStopping = s.routeEnd < 0 || s.routeEnd <= 1.5f * vStop * vStop / (2.0f * c.accel) + 30.0f;
	if (c.stopActive && (x <= 0.0f || decel < -0.25f * c.accel)) { c.stopActive = false; }
	else if (!c.stopActive && stopContact_ && routeStopping && dt > 0 && x > 2.0f && decel > 0.5f * c.accel)	// MINE: from a walk or faster
	{
		c.stopActive = true;
		c.stopQuiet = 0;
		c.stopClip = nullptr;
		c.stopTarget = -1;
		c.stopKMin = c.stopKMax = 0;
		c.stopSpeed0 = x;
		c.stopN = 0;
#ifdef KCLIENT_DEV_TOOLS
		if (std::getenv("KSTEPLOG")) { fprintf(stderr, "STOPARM%s: drawn %.2f moveSim %.2f (prev %.2f) wireMove %.2f dt %.4f\n", idTag(c.spec).c_str(), x, s.moveSim, c.moveSimPrev, s.wireMove, dt); }
#endif
	}
	if (c.stopActive)
	{
		// ds/stop-fix: the decel is the sim's, so the route end's dead stop no longer steps it; while the sim is already stopped and the drawn body is
		// still closing its gap (the catch-up at up to kCatchMin), the steer must stay on to that rest, not time out mid-catch-up
		const bool simStopped = std::fabs(s.wireMove) < 0.5f;
		c.stopQuiet = decel > 0.25f * c.accel || (simStopped && x > 0.5f) ? 0.0f : c.stopQuiet + dt;
		if (c.stopQuiet > kStopQuiet) { c.stopActive = false; }
	}
	if (!c.stopActive && c.stopN > 0)
	{
		// log feet: this stop's steer - the k range must stay in the 0.7..1.4 band (ds/stop-fix: fdbea25c's re-aiming ran it to 12x and the legs whirled)
		fprintf(stderr, "kclient: stop steer%s: speed %.2f -> %.2f u/s, k %.2f..%.2f, %d frames\n", idTag(c.spec).c_str(), c.stopSpeed0, x, c.stopKMin, c.stopKMax, c.stopN);
		c.stopN = 0;
	}
	if (c.stopActive && dt > 0 && x > 0 && advance > 0 && strideSum > 0)
	{
		// cycles the phase would cover unsteered before v = 0: the rate is proportional to the speed, which falls to 0, so half the rate x the time
		float nat = 0.5f * advance * (x / c.accel);
		// ds/run-stop-2: the drawn speed does not follow that ramp here - the sim's route end dead-stops its last tick (walkOrder sets the speed to
		// what is left of the route) and the drawn speed then coasts to rest through the interpolation, which made every stop land short of its aim
		// (measured -0.04..-0.18 cycles). Once the sim's own speed is 0 the drawn body's remaining travel is its distance to the sim's final
		// position, so the phase's own rate x that gap is what it will cover; that is the travel to aim by
		if (s.wireMove > -0.5f && s.wireMove < 0.5f && s.wireGap > 0) { nat = std::min(nat, advance * (s.wireGap / x)); }
		float total = 0;	// the lower moving clips' weight (the foot lock's denominator; the crossfade-would-be weight, as the lock reads it)
		for (const Layer& l : c.layers)
		{
			if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || l.vweight <= 0 || !l.clip || l.clip->length <= 0) { continue; }
			total += l.vweight;
		}
		// the down weight of the blend at a rest phase: each lower moving clip's windows at that phase (the shared phase + its offset); with
		// flat set, the foot's *worse* sole point (both down: a flat plant), else its better one (the foot lock's rule, either point held)
		auto depthAt = [&](float p, bool flat)
		{
			p -= std::floor(p);
			float depth = 0;	// the best foot
			for (const Layer& l : c.layers)
			{
				if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || l.vweight <= 0 || !l.clip || l.clip->length <= 0) { continue; }
				float q = p + l.synchOffset; q -= std::floor(q);
				for (int f = 0; f < 2; f++)
				{
					float pt[2] = {};
					for (const SoleContact& sc : l.contacts.window[f])
					{
						if (sc.end <= 1.0f ? q >= sc.start && q < sc.end : q >= sc.start || q < sc.end - 1.0f) { pt[sc.point] += l.vweight; }
					}
					depth = std::max(depth, flat ? std::min(pt[0], pt[1]) : std::max(pt[0], pt[1]));
				}
			}
			return depth;
		};
		if (total > 0)
		{
			// the aim is LATCHED once per stop (again when the heaviest lower clip changes), 434d373a's rule: fdbea25c re-aimed it every frame and,
			// as nat shrinks with x^2 and the wireGap clamp, k = aim / nat ran up to the band's top near v = 0 - the legs whirled at several times
			// cadence through a stop. The aim is the middle of a stretch the 0.7..1.4 band reaches, on a 1/128 grid over the next cycle: a good
			// stretch is one where the phase is down by more than half the blend. A flat plant (both sole points down) is tried first: a toe-off
			// edge is down past half by the lock's rule, but the foot is rolling off there and the pin cannot keep it (ds/run-stop-2); the
			// any-point stretches are the fallback
			const Layer* dom = nullptr;
			for (const Layer& l : c.layers)
			{
				if (l.overlay || !l.info || l.info->code != 0 || l.info->idle || l.vweight <= 0 || !l.clip || l.clip->length <= 0) { continue; }
				if (!dom || l.vweight > dom->vweight) { dom = &l; }
			}
			if (dom && dom->info != c.stopClip)
			{
				c.stopClip = dom->info;
				c.stopTarget = -1;
				c.stopLen = 0;
				const float step = 1.0f / 128.0f, tolNeed = 0.5f * nat;	// a stretch narrower than the estimate's own error is a last resort
				auto scan = [&](bool flat, bool requireMargin, float& aimOut, float& lenOut)
				{
					float depth[129];
					for (int i = 0; i <= 128; i++) { depth[i] = depthAt(c.phase + (float)i * step, flat); }
					float bestTol = -1;
					bool in = false;
					int startI = 0;
					for (int i = 0; i <= 128; i++)
					{
						const bool good = i < 128 && depth[i] > 0.5f * total;
						if (good && !in) { in = true; startI = i; }
						if (in && (!good || i == 128))
						{
							const float start = (float)startI * step, end = (float)i * step;
							const float lo = std::max(start, 0.7f * nat), hi = std::min(end, 1.4f * nat);
							if (lo <= hi)
							{
								const float aim = 0.5f * (lo + hi), tol = std::min(aim - start, end - aim);	// the middle of the reachable part
								if (tol > bestTol && (!requireMargin || tol >= tolNeed)) { bestTol = tol; aimOut = aim; lenOut = end - start; }
							}
							in = false;
						}
					}
				};
				float aim = -1, len = 0;
				scan(true, true, aim, len);
				if (aim < 0) { scan(false, true, aim, len); }
				if (aim < 0) { scan(true, false, aim, len); }
				if (aim < 0) { scan(false, false, aim, len); }
				if (aim >= 0) { c.stopTarget = c.phase + aim; c.stopLen = len; }
			}
			if (c.stopTarget >= 0.0f)
			{
				// a rate k x the natural one, held from here, lands the phase k x nat ahead (k times the falling speed integrates to k x v^2 / 2a), so
				// k puts the interval's start at v = 0; k stays in 0.7..1.4 (434d373a's band: past it the footing stops reading as the clip's own)
				float d = c.stopTarget - c.phase;
				d -= std::round(d);	// signed distance to the start, (-0.5, 0.5]
				if (d + c.stopLen > 0.0f)	// not already past it
				{
					const float kLo = d > 0.0f ? std::max(0.7f, d / nat) : 0.7f, kHi = std::min(1.4f, (d + c.stopLen) / nat);
					const float adv0 = advance;
					if (kLo <= kHi) { advance = std::clamp(advance, kLo * advance, kHi * advance); }
					else if (kLo > 1.4f) { advance = 1.4f * advance; }	// out of reach ahead: as fast as the bound allows
					else { advance = 0.7f * advance; }	// too near: brake the phase as far as the bound allows
					const float k = adv0 != 0.0f ? advance / adv0 : 1.0f;
					c.stopKMin = c.stopN == 0 ? k : std::min(c.stopKMin, k);
					c.stopKMax = c.stopN == 0 ? k : std::max(c.stopKMax, k);
					c.stopN++;
					c.steered = true;
				}
			}
		}
	}
	c.phase += dt * advance;
	c.phase -= std::floor(c.phase);
	if (action)
	{
		action->target = c.actionDone ? 0.0f : 1.0f;
		action->rate = c.actionDone ? 0.0f : c.actionSpeed;
	}
	const float kLayerScale[5] = { 2.0f, 1.25f, 1.0f, 0.5f, 0.5f };	// x animation blend rate (1406216b0, 140636dc0)
	// Inertialization (anim.inertialize; Bollo 2016): a clip's target jumping (the clip enters or leaves the blend) is a transition. The weights
	// snap to the targets, so the new clip plays at full weight from this frame, and the length the linear ramp would have taken becomes the
	// decay's (pose). A target that drifts - a speed band sweeping as the body accelerates - keeps the ramp: there is no clip change to
	// smooth, and re-capturing an offset every frame would only lag the pose. MINE: the jump threshold, well over the band sigmoid's ~0.3/frame
	// at the sim's acceleration, so a stop or a start reads as a transition and a steady walk does not.
	constexpr float kInertJump = 0.5f;
	// A band swap is a weight change within one layer, not a new motion: a moving clip hands its layer to its band neighbour as the speed
	// crosses an edge (stepback -> stepback short), the two being the same gait at a neighbouring speed on the same phase. It keeps the
	// linear crossfade, and a decay already running keeps going: re-capturing would add the pose both clips share to the offset (the action's
	// end, where a locomotion band swapped inside its fade, swung to 44 deg/frame that way; off 16). Detected within the jumping layers only: a
	// moving clip's jump out of the blend paired with a moving clip's jump into it (any layer: the lower and upper lists hand over at slightly
	// different speeds). The idle chooser, the action and the overlays never pair - their clip leaving or entering is the motion's change (a
	// stop, a start, an action), and it captures.
	// the target a layer is actually moving toward: an action displacing the lower body pins those layers under 1 - the action's weight (the
	// cap below), so their band target is not what they ramp to. MINE: without that, a pinned lower layer read a >0.5 jump on every frame of an
	// attack and the client captured once a frame - the last capture's decay (0.11-0.25 s) was still swinging through the action's out.
	auto targetOf = [&](const Layer& l) { return acting && !c.actionBlock && l.info->code == 0 ? std::min(l.target, 1.0f - action->weight) : l.target; };
	auto bandSwap = [&](const Layer& a)
	{
		if (!inertOn_ || a.info->idle || a.overlay || &a == action) { return false; }
		const float da = targetOf(a) - a.weight;
		if (std::fabs(da) <= kInertJump) { return false; }
		for (const Layer& b : c.layers)
		{
			if (&b == &a || b.info->idle || b.overlay || &b == action) { continue; }
			// half the jump: the partner of a redistribution lags its own weight, so it can sit just under the capture threshold (walk lower
			// hands to jog lower at 0.50/0.50)
			const float db = targetOf(b) - b.weight;
			if (da > 0 ? db < -0.5f * kInertJump : db > 0.5f * kInertJump) { return true; }
		}
		return false;
	};
	// MINE (ds/inertialize-4/5): the action layer's whole blend keeps the linear crossfade, in and out; inertialization captures only a stop and
	// a start (locomotion <-> idle). A capture snapped the clips it let in to full weight on that frame and their own motion showed at once: the
	// out often comes as the body backs away, so the stepback's 27 deg/frame landed on the fade's first frames where the ramp gives 16, and the
	// in played the technique's own wind-up at full weight (10-frame peak 31 vs off 12.7; the smoother transition frame did not buy that back).
	// With the layer's weight off its target - either direction - there is no capture and the weights ramp as with anim.inertialize 0. The whole
	// of each fade, not just the jump frame: a band re-choice inside it otherwise captured and snapped every weight.
	auto actionBlend = [&] { return action && std::fabs(action->weight - action->target) > 1e-4f; };
	float fade = 0;	// this frame's transition length, s (the longest snapped layer's ramp); 0 = no transition
	if (inertOn_ && !instant && !actionBlend())
	{
		for (const Layer& l : c.layers)
		{
			const float d = targetOf(l) - l.weight;
			if (std::fabs(d) <= kInertJump || bandSwap(l)) { continue; }
			const float r = blendRate_ * kLayerScale[l.info->code];
			fade = std::max(fade, std::fabs(d) / (d > 0 ? r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) : r));
		}
	}
	for (Layer& l : c.layers)
	{
		const float r = blendRate_ * kLayerScale[l.info->code];
		if (instant) { l.vfade = 0; }
		else if (fade > 0) { l.vfade = std::fabs(l.target - l.weight) / (l.target > l.weight ? r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) : r); }
		if (instant || fade > 0) { l.weight = l.target; }
		else if (l.weight < l.target) { l.weight = std::min(l.target, l.weight + r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) * dt); }
		else { l.weight = std::max(l.target, l.weight - r * dt); }
		// the lower cap: 1 - the action's weight. MINE: not under a block, whose clip has no leg tracks (the legs would sag toward the bind pose)
		if (acting && !c.actionBlock && l.info->code == 0) { l.weight = std::min(l.weight, 1.0f - action->weight); }
		// the crossfade-would-be weight (vweight): the linear ramp above, walked on through a snap for the length that ramp would have taken. The
		// foot lock's contact windows read it: a snapped layer sits at its target this frame, which would end the stance the fade still holds down.
		if (l.vfade > 0)
		{
			l.vfade -= dt;
			l.vweight = l.vweight < l.target ? std::min(l.target, l.vweight + r * std::clamp(std::fabs(l.rate), 0.5f, 10.0f) * dt) : std::max(l.target, l.vweight - r * dt);
		}
		else { l.vweight = l.weight; }
		const float len = l.clip->length;
		// a layer in its virtual fade keeps its clip running: the crossfade would, and the contact windows the lock reads come from its phase
		if (len <= 0 || (l.weight <= 0 && l.vweight <= 0)) { continue; }
		if (l.synchs)
		{
			const float p = c.phase + l.synchOffset;
			l.time = (p - std::floor(p)) * len;
			continue;
		}
		l.time += dt * l.rate;
		l.time = l.loop ? l.time - std::floor(l.time / len) * len : std::clamp(l.time, 0.0f, len);
	}
	c.inert.pending = fade;	// the transition pose() captures the last pose from and decays onto these snapped weights
	if (acting && !action->loop && action->time >= 0.98f * action->clip->length)
	{
		// cut at progress 0.98: it fades out; what it displaced restarts from 0
		c.actionDone = true;
		for (Layer& l : c.layers) { if (l.weight <= 0 && !l.synchs) { l.time = 0; } }
	}
	if (action && c.actionDone && action->weight <= 0)
	{
		c.layers.erase(c.layers.begin() + c.action);
		c.action = -1;
		c.actionDone = false;
	}
}

WorkPool& Characters::posePool(size_t items)
{
	// anim.pose_threads: the threads the pose pass runs on, the logic thread included (1 = serial); read on a cfg change. Under kPoseMinItems the
	// pass runs serial (a wake costs more than a few poses).
	const int want = items < kPoseMinItems ? 1 : std::max(1, kPoseThreads.get());
	if (!posePool_ || posePool_->workers() != want - 1) { posePool_ = std::make_unique<WorkPool>(want - 1, "kpose"); }
	return *posePool_;
}

// anim.action_root (fix 2): the sim's own technique displacement IS the clip's root replayed tick by tick (clip.at(t1) - clip.at(t0) in x/z, rotated
// by the aim yaw fixed at the technique's start, no root yaw, at the technique's playback rate; COMBAT_ANIM.md 5), while the client zeroed the action's
// root and drew the body at the wire, so a clip's stance foot slid under it at body speed. Draw the node along the same replay: anchor + R(yaw0) x
// (root(clip time) - root(0)), the anchor taken so the first drawn frame is on the wire. Where the wire differs (navmesh clamp, separation push,
// stumble recoil) the path eases onto it over kActEase, never away from it. The phase is the wire's own action_t x its rate, so the path cannot drift
// from the sim's integrate(). yaw0 is the sim's aim yaw (the CHAR_DYNAMIC aim tail): combat.cpp sets it from the bearing to the target at the
// technique's start and lets the body turn onto it, so it is not the body yaw; an older server without the tail: the wire's entity yaw at the
// action's first sample.
bool Characters::actionPath(Char& c, float dt)
{
	if (!actionRoot_ || !c.actPath || c.action < 0 || (size_t)c.action >= c.layers.size()) { return false; }
	const Layer& l = c.layers[(size_t)c.action];
	if (!l.clip || l.clip->length <= 0) { return false; }
	const kworld::Skeleton& sk = c.body->skel;
	const int b = sk.bone("Bip01");
	if (b < 0) { return false; }
	const float len = l.clip->length;
	const float t = (c.spec.actionTime > 0 ? c.spec.actionTime : l.time / std::max(c.actionSpeed, 1e-3f)) * c.actionSpeed;
	const float u = std::clamp(t, 0.0f, len);	// the sim clamps clipTime at the clip's length and ends the technique there (combat.cpp)
	const kworld::SkelVec3 r = kworld::sampleLocal(sk, l.clip, b, u).pos;
	const kworld::SkelVec3 d = { r.x - c.actRoot0[0], 0, r.z - c.actRoot0[1] };
	if (!c.actAnchored)
	{
		if (!std::isfinite(c.spec.yaw) || !std::isfinite(c.spec.pos[0]) || !std::isfinite(c.spec.pos[2]))
		{
			// no finite wire yaw / position this character (a sim state, not this path): draw it at the wire, no path
			static int bad = 0;
			if (bad++ < 4) { fprintf(stderr, "kclient: action path%s: %s: non-finite wire yaw %.2f pos %.1f %.1f, path off\n", idTag(c.spec).c_str(), c.spec.action.c_str(), c.spec.yaw, c.spec.pos[0], c.spec.pos[2]); }
			c.actPath = false;
			return false;
		}
		c.actYaw0 = c.spec.hasAim ? c.spec.aimYaw : c.spec.yaw;	// the aim is fixed at the technique's start
		c.actAnchored = true;
		c.actCorr[0] = c.actCorr[1] = 0;
		const kworld::SkelQuat q0 = { std::cos(c.actYaw0 * 0.5f), 0, std::sin(c.actYaw0 * 0.5f), 0 };
		const kworld::SkelVec3 o0 = kworld::rotate(q0, d);
		c.actAnchor[0] = c.spec.pos[0] - o0.x;	// the first path frame lands on the wire
		c.actAnchor[1] = c.spec.pos[2] - o0.z;
	}
	const kworld::SkelQuat qy = { std::cos(c.actYaw0 * 0.5f), 0, std::sin(c.actYaw0 * 0.5f), 0 };
	const kworld::SkelVec3 o = kworld::rotate(qy, d);
	const double path[2] = { c.actAnchor[0] + o.x, c.actAnchor[1] + o.z };
	c.actPathPt[0] = path[0];
	c.actPathPt[1] = path[1];
	const float e = std::min(1.0f, dt / kActEase);
	c.actCorr[0] += (c.spec.pos[0] - path[0] - c.actCorr[0]) * e;
	c.actCorr[1] += (c.spec.pos[2] - path[1] - c.actCorr[1]) * e;
	// never fight the wire: the drawn body may lead it by at most a step (the clip's replay between samples, or a push easing in); a clamp
	// (a dash into a crowd) is the wire's, and drawing through it would take the whole pose past it
	double drawn[2] = { path[0] + c.actCorr[0], path[1] + c.actCorr[1] };
	const double ox = drawn[0] - c.spec.pos[0], oz = drawn[1] - c.spec.pos[2], off = std::hypot(ox, oz);
	if (off > kActMaxOff) { drawn[0] = c.spec.pos[0] + ox / off * kActMaxOff; drawn[1] = c.spec.pos[2] + oz / off * kActMaxOff; }
	c.pos[0] = drawn[0];
	c.pos[2] = drawn[1];
	c.actG[0] = c.pos[0] - c.spec.pos[0];	// the hand-off's offset from the wire, should the action end right after this frame
	c.actG[1] = c.pos[2] - c.spec.pos[2];
	c.actWas = true;
	return true;
}

// 1405cda20 (every frame while carried) and 1405b5980: the carried character's node follows the carrier's upper arm, `Bip01 L UpperArm` when carrying
// left, else R (1405ced90). Position = carrier position + carrier orientation x the bone's model position + (carrier orientation x the bone's model
// orientation) x an offset in the bone's frame; the offset runs A -> B -> C as the carried's Height goes 80 -> 100 -> 120 (lift = (Height - 80) x 0.025
// x 2: 14052e6c0 +0x18c x 2). Orientation = that bone orientation x Q1 (90 degrees about x) x Q2 (14 degrees about z). The carrier is posed first (a
// lower index). The game skips this while the carried is a ragdoll (vtable +0x248), which is not modelled.
void Characters::carryPlace(Char& c, const Char& carrier, const std::vector<kworld::SkelTransform>& model, const kworld::SkelQuat& orient, const double pos[3])
{
	const kworld::Skeleton& sk = carrier.body->skel;
	const int b = sk.bone(carrier.spec.carry == 1 ? "Bip01 L UpperArm" : "Bip01 R UpperArm");
	if (b < 0 || model.empty()) { return; }
	const kworld::SkelVec3 kA = { -1.4f, 0.0f, 1.0f }, kB = { -1.9f, -1.0f, 2.6f }, kC = { -3.0f, -0.4f, 5.0f };	// 1421301c0 / b0 / a0
	const float s = c.carryLift;
	const kworld::SkelVec3& p = s < 1.0f ? kA : kB;
	const kworld::SkelVec3& q = s < 1.0f ? kB : kC;
	const float t = s < 1.0f ? s : s - 1.0f;
	const kworld::SkelVec3 offset = { p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t };
	const kworld::SkelTransform& bone = model[(size_t)b];
	const kworld::SkelVec3 bp = kworld::rotate(orient, bone.pos);
	const kworld::SkelQuat br = kworld::multiply(orient, bone.rot);
	const kworld::SkelVec3 o = kworld::rotate(br, offset);
	c.pos[0] = pos[0] + bp.x + o.x;
	c.pos[1] = pos[1] + bp.y + o.y;
	c.pos[2] = pos[2] + bp.z + o.z;
	const kworld::SkelQuat q1 = { 0.70710677f, 0.70710677f, 0, 0 }, q2 = { 0.99254614f, 0, 0, 0.12186930f };	// 1416f0b38, 1416f0b30 / b34
	c.orient = kworld::multiply(kworld::multiply(br, q1), q2);
}

// The skinning palette of this frame: node orientation x model(t) x inverse(bind) per bone, as row-major 3x4 rows.
// Layers apply like Ogre's AnimationTrack::applyToNode on a reset bone: each adds its keyframe offset scaled by its weight (translate x w, rotate by
// nlerp(identity, q, w) post-multiplied, scale lerped). MINE: weights are normalised per bone over the layers that move it (Ogre's average mode scales every
// state by 1 / total weight); with the traced deletes, lower and upper layers share no animated bone, so either reading gives the same pose.
void Characters::pose(Char& c, float dt)
{
	const kworld::Skeleton& sk = c.body->skel;
	// Pose cache (kenshi-k16.2.205.11): a frame that cannot move the pose (dt 0: the sim paused, or a scripted shot) and whose inputs are the same
	// as the last pose's computes the same palette and model; keep them (the same bone x layer sampling and blending would reproduce them bit for
	// bit). footOffset's node y step is the one input-side effect to replay (footLift is unchanged at dt 0). Any deeper state (a ragdoll's bones, the
	// get-up blend) keeps the cache off.
	const bool cacheable = dt == 0 && c.rag.empty() && c.riseDur == 0;
	std::uint64_t sig = 1469598103934665603ull;
	if (cacheable)
	{
		auto mix = [&sig](std::uint64_t v) { sig ^= v + 0x9e3779b97f4a7c15ull + (sig << 6) + (sig >> 2); };
		auto fb = [](float x) { std::uint32_t u; std::memcpy(&u, &x, sizeof u); return (std::uint64_t)u; };
		auto db = [](double x) { std::uint64_t u; std::memcpy(&u, &x, sizeof u); return u; };
		mix((std::uint64_t)(std::uintptr_t)c.body);
		mix((std::uint64_t)(std::uintptr_t)c.mesh);
		mix(fb(c.orient.w)); mix(fb(c.orient.x)); mix(fb(c.orient.y)); mix(fb(c.orient.z));
		mix(db(c.pos[0])); mix(db(c.pos[1])); mix(db(c.pos[2]));
		mix((std::uint64_t)(c.spec.loco ? 1u : 0u) | (std::uint64_t)(c.spec.rootInPlace ? 2u : 0u) | (std::uint64_t)(footLock_ ? 4u : 0u));
		mix(c.layers.size()); mix(c.postures.size()); mix(c.boneScale.size()); mix(c.mounts.size()); mix(c.size.size()); mix(c.posSize.size());
		for (const Layer& l : c.layers) { mix((std::uint64_t)(std::uintptr_t)l.clip); mix((std::uint64_t)(std::uintptr_t)l.info); mix(fb(l.time)); mix(fb(l.weight)); }
		for (const Layer& l : c.postures) { mix((std::uint64_t)(std::uintptr_t)l.clip); mix(fb(l.time)); }
		for (const auto& [bone, sc] : c.boneScale) { mix((std::uint64_t)bone); mix(fb(sc.x)); mix(fb(sc.y)); mix(fb(sc.z)); }
		for (const Char::Mount& mt : c.mounts) { mix((std::uint64_t)(uint32_t)mt.slot); mix((std::uint64_t)(uint32_t)mt.bone); for (int k = 0; k < 16; k++) { mix(fb(mt.local[k])); } }
		if (c.poseValid && sig == c.poseSig)
		{
			if (c.spec.loco) { c.pos[1] += c.footLift; }	// Characters::footOffset's node step (below), the pose's only write back to the node
			return;
		}
	}
	std::vector<kworld::SkelTransform> local(sk.bones.size()), model(sk.bones.size());
	CPUSTAGE_DECL(stageAcc);
	CPUSTAGE_OPT(stageAcc, "pose.sample");
	const bool noSample = ablate::on("nosample");	// KCLIENT_ABL: bind pose, no clip sampling per bone (an upper-bound measurement)
	// The layers that can move a bone this frame: a weight-0 layer adds nothing (translate x 0, an identity rotation, a 0-lerped scale) and counts 0
	// in the normalisation, so skipping it is the same pose; a character keeps one layer per ANIMATION record it can play and holds a few at weight.
	std::vector<const Layer*> active;
	active.reserve(c.layers.size());
	for (const Layer& l : c.layers) { if (l.weight > 0) { active.push_back(&l); } }
	bool anyTracks = false;
	for (const Layer* l : active) { if (l->clip && !l->clip->tracks.empty()) { anyTracks = true; } }
	for (const Layer& l : c.postures) { if (l.clip && !l.clip->tracks.empty()) { anyTracks = true; } }
	if (!anyTracks && !c.noClipLogged && !noSample && !c.spec.anims.empty())
	{
		// kenshi-k16.2.448: no layer carries weight and no posture clip: every bone is the bind pose (a T-pose). Loud, once per character (dev only).
		c.noClipLogged = true;
		std::string layers;
		for (const Layer& l : c.layers) { layers += " " + (l.info ? l.info->name : std::string("?")) + (l.clip ? "" : "(no clip)") + (l.overlay ? "(overlay)" : ""); }
		fprintf(stderr, "kclient: BIND POSE: %s drawn with no weighted clip: move %.2f stance '%s' combat %d stealth %d hands %d/%d weaponCategory %d idle %d action %d action '%s' clip '%s' carry %d carriedBy %d layers [%s ]\n",
			label(c.spec).c_str(), (double)c.spec.move, c.spec.stance.c_str(), (int)c.spec.combat, (int)c.spec.stealth, (int)c.handL, (int)c.handR, (int)c.weaponCategory, c.idle, c.action,
			c.spec.action.c_str(), c.spec.clip.c_str(), (int)c.spec.carry, (int)c.spec.carriedBy, layers.c_str());
		evlog::add("bindpose", -1, c.spec.id, "%s drawn in the bind pose: no weighted clip (stance '%s' hands %d/%d weapon %d)", label(c.spec).c_str(), c.spec.stance.c_str(), (int)c.handL, (int)c.handR, (int)c.weaponCategory);
	}
	for (size_t b = 0; b < sk.bones.size(); b++)
	{
		const kworld::SkelTransform& bind = sk.bones[b].bind;
		if (noSample)
		{
			local[b] = bind;
			if (sk.bones[b].parent < 0) { c.rootXZ[0] = c.rootXZ[1] = 0; }
			continue;
		}
		// an override bone (keep 2; OldNodeAnimationTrack::applyToNode 180031260 with the override flag): before adding its rotation it pulls the
		// bone's accumulated orientation back toward the initial one by its weight. MINE: done as scaling the other layers by 1 - that weight.
		float total = 0, over = 0;
		for (const Layer* l : active)
		{
			if (l->keep[b] == 2) { over = std::max(over, l->weight); }
			else if (l->keep[b]) { total += l->weight; }
		}
		const float norm = (total > 1.0f ? 1.0f / total : 1.0f) * (1.0f - over);
		kworld::SkelTransform t = bind;
		auto apply = [&](const Layer& l, float w)
		{
			kworld::SkelTransform s = kworld::sampleLocal(sk, l.clip, (int)b, l.time);
			if (l.info == &actionInfo_ && sk.bones[b].parent < 0)
			{
				// an action plays in place: its root travel (gains ground, dodges) is in the sim's position already (COMBAT_ANIM.md section 5)
				const kworld::SkelTransform s0 = kworld::sampleLocal(sk, l.clip, (int)b, 0.0f);
				s.pos.x = s0.pos.x;
				s.pos.z = s0.pos.z;
			}
			// the keyframe offset back out of bind (+) offset
			t.pos.x += (s.pos.x - bind.pos.x) * w;
			t.pos.y += (s.pos.y - bind.pos.y) * w;
			t.pos.z += (s.pos.z - bind.pos.z) * w;
			const kworld::SkelQuat inv = { bind.rot.w, -bind.rot.x, -bind.rot.y, -bind.rot.z };
			kworld::SkelQuat q = kworld::multiply(inv, s.rot);
			if (q.w < 0) { q = { -q.w, -q.x, -q.y, -q.z }; }
			kworld::SkelQuat r = { 1 - w + q.w * w, q.x * w, q.y * w, q.z * w };
			const float len = std::sqrt(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
			r = { r.w / len, r.x / len, r.y / len, r.z / len };
			t.rot = kworld::multiply(t.rot, r);
			t.scale.x *= 1 + (s.scale.x / bind.scale.x - 1) * w;
			t.scale.y *= 1 + (s.scale.y / bind.scale.y - 1) * w;
			t.scale.z *= 1 + (s.scale.z / bind.scale.z - 1) * w;
		};
		for (const Layer* l : active) { if (l->keep[b] == 1) { apply(*l, l->weight * norm); } }
		for (const Layer* l : active) { if (l->keep[b] == 2) { apply(*l, l->weight); } }
		for (const Layer& l : c.postures) { if (l.keep[b]) { apply(l, 1.0f); } }
		local[b] = t;
		// The root bone (Bip01) carries the entity's position, not just the skeleton's: the lab's root lock reports what the pose would add, then
		// zeroes it, so the caller's node carries it instead - the game's own rule for a relocates record and for an action (COMBAT_ANIM.md 5).
		if (sk.bones[b].parent < 0)
		{
			c.rootXZ[0] = t.pos.x - bind.pos.x;
			c.rootXZ[1] = t.pos.z - bind.pos.z;
			if (c.spec.rootInPlace) { local[b].pos.x = bind.pos.x; local[b].pos.z = bind.pos.z; }
		}
	}
	// anim.inertialize: the transition's decay onto the blended local pose, before the hierarchy and lockFeet, so the lock still fixes the feet
	CPUSTAGE_OPT(stageAcc, "pose.inertialize");
	if (dt > 0) { inertialize(c, local, dt); }
	CPUSTAGE_OPT(stageAcc, "pose.hierarchy");
	// OldBone::updateFromParentImpl (BODY_SHAPE.md): a bone's derived scale is its own size x its animation scale, not the parent's; its offset is
	// scaled by the parent's derived scale y x its own positional size
	std::vector<int> driven(c.rag.empty() ? 0 : sk.bones.size(), -1);	// bone -> its ragdoll entry
	for (size_t k = 0; k < c.rag.size(); k++) { if (c.rag[k].bone >= 0 && (size_t)c.rag[k].bone < driven.size()) { driven[(size_t)c.rag[k].bone] = (int)k; } }
	std::vector<int> rise(c.riseDur > 0 ? sk.bones.size() : 0, -1);	// bone -> the get-up blend's ragdoll pose
	for (size_t k = 0; k < c.riseFrom.size() && !rise.empty(); k++) { if (c.riseFrom[k].bone >= 0 && (size_t)c.riseFrom[k].bone < rise.size()) { rise[(size_t)c.riseFrom[k].bone] = (int)k; } }
	const kworld::SkelQuat unorient = { c.orient.w, -c.orient.x, -c.orient.y, -c.orient.z };
	for (int b : sk.parentFirst)
	{
		const int p = sk.bones[(size_t)b].parent;
		const kworld::SkelTransform& l = local[(size_t)b];
		kworld::SkelTransform& t = model[(size_t)b];
		if (!driven.empty() && driven[(size_t)b] >= 0)	// the body's world transform, back into the node's model space
		{
			const RagBone& r = c.rag[(size_t)driven[(size_t)b]];
			t.scale = { l.scale.x * c.size[(size_t)b].x, l.scale.y * c.size[(size_t)b].y, l.scale.z * c.size[(size_t)b].z };
			t.pos = kworld::rotate(unorient, { (float)(r.p[0] - c.pos[0]), (float)(r.p[1] - c.pos[1]), (float)(r.p[2] - c.pos[2]) });
			t.rot = kworld::multiply(unorient, r.q);
		}
		else
		{
			const kworld::SkelVec3& size = c.size[(size_t)b];
			t.scale = { l.scale.x * size.x, l.scale.y * size.y, l.scale.z * size.z };
			for (const auto& [bone, f] : c.boneScale) { if (bone == b) { t.scale = { t.scale.x * f.x, t.scale.y * f.y, t.scale.z * f.z }; } }
			if (p < 0)
			{
				t.pos = l.pos;
				t.rot = l.rot;
			}
			else
			{
				const kworld::SkelTransform& pt = model[(size_t)p];
				const kworld::SkelVec3& ps = c.posSize[(size_t)b];
				const kworld::SkelVec3 o = kworld::rotate(pt.rot, { l.pos.x * ps.x * pt.scale.y, l.pos.y * ps.y * pt.scale.y, l.pos.z * ps.z * pt.scale.y });
				t.pos = { pt.pos.x + o.x, pt.pos.y + o.y, pt.pos.z + o.z };
				t.rot = kworld::multiply(pt.rot, l.rot);
			}
		}
		// the get-up blend (riseFrom, Characters::rise): the ragdoll's last world pose cross-fades into the clip's over riseDur
		if (!rise.empty() && rise[(size_t)b] >= 0)
		{
			const RagBone& f = c.riseFrom[(size_t)rise[(size_t)b]];
			const float w = std::clamp(c.riseT / c.riseDur, 0.0f, 1.0f);
			const kworld::SkelVec3 fo = kworld::rotate(unorient, { (float)(f.p[0] - c.pos[0]), (float)(f.p[1] - c.pos[1]), (float)(f.p[2] - c.pos[2]) });
			t.pos.x = fo.x + (t.pos.x - fo.x) * w;
			t.pos.y = fo.y + (t.pos.y - fo.y) * w;
			t.pos.z = fo.z + (t.pos.z - fo.z) * w;
			kworld::SkelQuat fq = kworld::multiply(unorient, f.q);
			if (fq.w * t.rot.w + fq.x * t.rot.x + fq.y * t.rot.y + fq.z * t.rot.z < 0) { fq = { -fq.w, -fq.x, -fq.y, -fq.z }; }	// the shorter way round
			const kworld::SkelQuat q = { fq.w + (t.rot.w - fq.w) * w, fq.x + (t.rot.x - fq.x) * w, fq.y + (t.rot.y - fq.y) * w, fq.z + (t.rot.z - fq.z) * w };
			const float len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (len > 1e-6f) { t.rot = { q.w / len, q.x / len, q.y / len, q.z / len }; }
		}
	}
	CPUSTAGE_OPT(stageAcc, "pose.feet");
	if (c.spec.loco) { footOffset(c, model, dt); }	// the body's height from this frame's pose, before the feet are solved against it
	if (footLock_ && c.spec.loco) { lockFeet(c, local, model, dt); }
	for (int f = 0; f < 2; f++)
	{
		const int b = sk.bone(f ? "Bip01 R Foot" : "Bip01 L Foot");
		const kworld::SkelVec3 o = b >= 0 ? kworld::rotate(c.orient, model[(size_t)b].pos) : kworld::SkelVec3{};
		c.feet[f][0] = o.x; c.feet[f][1] = o.y; c.feet[f][2] = o.z;
	}
	CPUSTAGE_OPT(stageAcc, "pose.skin");
	float node[16];
	kworld::toMatrix({ {}, c.orient, { 1, 1, 1 } }, node);
	std::vector<std::array<float, 16>> skin(sk.bones.size());
	auto store = [&](size_t slot, const float m[16]) { for (int r = 0; r < 3; r++) { for (int k = 0; k < 4; k++) { c.palette[slot * 12 + r * 4 + k] = m[k * 4 + r]; } } };
	for (size_t b = 0; b < sk.bones.size(); b++)
	{
		float* m = skin[b].data();
		kworld::toMatrix(model[b], m);
		mul(m, node, m);
		mul(m, m, c.body->invBind[b].data());
		store(b, m);
	}
	for (const Char::Mount& mt : c.mounts)
	{
		float m[16];
		mul(m, skin[(size_t)mt.bone].data(), mt.local);
		store((size_t)mt.slot, m);
	}
	c.model = std::move(model);
	// the cache above: this pose's inputs, so an unchanged paused / scripted frame can keep it
	c.poseValid = cacheable;
	c.poseSig = cacheable ? sig : 0;
}

// Inertialization (Bollo, GDC 2016, "Inertialization: High-Performance Animation Transitions in Gears of War"; CLIENT_GOALS "Locomotion fidelity" 5).
// locomote snapped the layer weights to the new clip's targets and marked the transition with its length (the crossfade's would-be time). The pose
// shown last frame, with its velocity, is the source; each bone's offset from this frame's target pose (translation in the bone's parent frame,
// rotation as an axis-angle vector in its own) decays by 1 - quintic(t / T), the linear term carrying the velocity, so the body keeps its motion and
// the new clip plays at full weight from the transition. Per bone the fade clamps to |offset| / |velocity| when the velocity points away from the
// pose (the paper's rule), so no bone overshoots. Runs on the local pose before the hierarchy and lockFeet: the foot lock then fixes the feet on top.
void Characters::inertialize(Char& c, std::vector<kworld::SkelTransform>& local, float dt)
{
	Char::Inertial& in = c.inert;
	const size_t n = local.size();
	if (!inertOn_) { in.len = in.pending = 0; }	// 0: the pose is the linear crossfade's, and the tracking below only measures it
	bool capture = false;
	if (inertOn_ && in.pending > 0)
	{
		// the pose shown last frame needs a velocity: its interval from the frame before is that. A first pose has no source (the target stands),
		// and neither has the second: it has the one before it missing (both must be the same bone count as this pose: a body can change).
		capture = in.from.size() == n && in.prev.size() == n && in.dt > 0;
		if (capture)
		{
			in.len = in.pending;
			in.t = 0;
			in.pos.resize(n); in.posV.resize(n); in.rot.resize(n); in.rotV.resize(n); in.dur.resize(n); in.durR.resize(n);
			// MINE (ds/inertialize-3): cap the captured velocity. A stop's source pose is mid-stride, and the decay carries the stride's speed through
			// the fade: the run's legs turn 27 deg/frame into the 100 u/s stop, where the linear crossfade peaks at 18.1 deg/frame. A channel moving
			// away from its target also has its fade cut to |offset| / |velocity|, and the quintic then swings at 2.31x that velocity mid-fade (the
			// steepest slope, 1.875 |offset| / T = 1.875 |velocity|, plus the carried 0.44 v0): at the 70 u/s stop a 101 deg offset turn cut to
			// 0.159 s swung 24.6. So the carry is held to 7 deg/frame (420 deg/s): 2.31 x 7 = 16.2, under the crossfade's 18.1 with room for the
			// target clip's own ~1 deg/frame of motion on top. Translation likewise at 60 u/s (1 u/frame, a stride's limb speed; stops measured 20-56).
			constexpr float kInertV0Rot = 420.0f / 57.29578f, kInertV0Pos = 60.0f;	// rad/s, u/s
			for (size_t b = 0; b < n; b++)
			{
				const kworld::SkelTransform& src = in.from[b];
				in.pos[b] = vsub(src.pos, local[b].pos);
				in.posV[b] = vmul(vsub(src.pos, in.prev[b].pos), 1.0f / in.dt);
				in.rot[b] = qaxis(local[b].rot, src.rot);
				in.rotV[b] = vmul(qaxis(in.prev[b].rot, src.rot), 1.0f / in.dt);
				float vp = vlen(in.posV[b]);
				if (vp > kInertV0Pos) { in.posV[b] = vmul(in.posV[b], kInertV0Pos / vp); vp = kInertV0Pos; }
				float vr = vlen(in.rotV[b]);
				if (vr > kInertV0Rot) { in.rotV[b] = vmul(in.rotV[b], kInertV0Rot / vr); vr = kInertV0Rot; }
				// the velocity-away clamp, per channel: a channel whose motion leads the offset further from the target first gets the
				// shorter fade |offset| / |velocity| (so it does not overshoot); the other keeps the crossfade's time. Separate, because a
				// limb's translation moving fast must not cut a large rotation's fade to a few frames (a snap).
				float d = in.len;
				if (vp > 1e-6f && vdot(in.pos[b], in.posV[b]) > 0.0f) { d = std::min(d, vlen(in.pos[b]) / vp); }
				float dr = in.len;
				if (vr > 1e-6f && vdot(in.rot[b], in.rotV[b]) > 0.0f) { dr = std::min(dr, vlen(in.rot[b]) / vr); }
				in.dur[b] = std::max(d, 1e-4f);
				in.durR[b] = std::max(dr, 1e-4f);
			}
		}
		else { in.len = 0; }
		in.pending = 0;
	}
	if (in.len > 0 && in.dur.size() != n) { in.len = 0; }	// a capture from a pose of another bone count: its offsets index past this one
	if (in.len > 0)
	{
		// the capture frame advances one step: at t = dt the decay's slope is still 0, so the pose moves by the source's own velocity
		// (v0, captured), which keeps the frame smooth; showing t = 0 would repeat the previous pose for a frame (a hitch)
		in.t = std::min(in.len, in.t + dt);
		for (size_t b = 0; b < n; b++)
		{
			const float up = std::clamp(in.t / in.dur[b], 0.0f, 1.0f);
			const float ur = std::clamp(in.t / in.durR[b], 0.0f, 1.0f);
			const float decayP = 1.0f - up * up * up * (up * (up * 6.0f - 15.0f) + 10.0f);	// 1 - the quintic 6u^5 - 15u^4 + 10u^3
			const float decayR = 1.0f - ur * ur * ur * (ur * (ur * 6.0f - 15.0f) + 10.0f);
			const kworld::SkelVec3 pv = vadd(in.pos[b], vmul(in.posV[b], in.t));
			const kworld::SkelVec3 rv = vmul(vadd(in.rot[b], vmul(in.rotV[b], in.t)), decayR);
			local[b].pos = vadd(local[b].pos, vmul(pv, decayP));
			local[b].rot = kworld::multiply(local[b].rot, qexp(rv));
		}
		if (in.t >= in.len) { in.len = 0; }
	}
	// the pose shown this frame (the offsets included) is the next transition's source; with the one before it gives that transition's velocity.
	// It also gives the frame's largest local joint rotation step (log feet: the peak angular velocity across a transition, measured the same on
	// and off, so the option's effect is comparable on one build).
	c.poseStep = 0;
	if (in.from.size() == n)
	{
		for (size_t b = 0; b < n; b++) { c.poseStep = std::max(c.poseStep, vlen(qaxis(in.from[b].rot, local[b].rot))); }
		c.poseStep *= 57.29578f;
	}
	in.prev = in.from;
	in.from = local;
	in.dt = dt;
}

bool Characters::lockStopped(const Char& c)
{
	// anim.action_root (fix 2): an action whose clip root draws the body (actPath: a technique, not a block) keeps its own contact windows locked
	if ((c.action >= 0 && !c.actPath) || c.spec.carriedBy >= 0 || !c.rag.empty() || c.lieLayer >= 0 || c.riseDur > 0 || c.gettingUp || c.infoless) { return true; }
	for (const Layer& l : c.layers) { if (l.info && l.info->relocates && !l.overlay && l.weight > 0) { return true; } }	// root motion: the feet are not the ground's
	return false;
}

// The body's one height offset (kenshi-k16.17.7.5, review step 5; it replaces the slope compensation applied before the pose from the last pose's
// feet, and the slice-3 pelvis drop): each foot that is down in this frame's pose votes the ground under its sole over the ground under the node
// (the clip is authored for flat ground at the node), the body takes the lowest vote, so the lower foot stands on the ground and the higher one
// is the IK's to reach (a knee bend, never a leg past its reach). A foot is down when its lower sole point is within kPlanted of the lowest sole
// of the two. Same frame: read from the pose being drawn, not the one before it. Independent of anim.foot_lock (it is the original's slope
// compensation, user 2026-11-06); without the ground callback, or off the terrain (the heightmap not within kOnTerrain of the wire y: a floor, step or deck), no offset.
void Characters::footOffset(Char& c, const std::vector<kworld::SkelTransform>& model, float dt)
{
	constexpr float kPlanted = 0.3f;	// a sole this close to the lowest one's height (u) is down with it (MINE: a walk's swing foot is past it within a few frames)
	constexpr float kRate = 24.0f;	// the smoothing of the offset's steps when the voting foot changes, 1/s (MINE: ~0.04 s)
	float target = 0;
	if (ground && dt > 0 && std::fabs(ground(c.spec.pos[0], c.spec.pos[2]) - c.spec.pos[1]) < kOnTerrain)
	{
		double gx[2] = {}, gz[2] = {};
		float y[2] = { 1e9f, 1e9f };
		for (int f = 0; f < 2; f++)
		{
			for (int pt = 0; pt < 2; pt++)
			{
				const int b = c.body->soleBone[f][pt];
				if (b < 0 || (size_t)b >= model.size()) { continue; }
				kworld::SkelTransform at;
				at.pos = c.body->soleAt[f][pt];
				const kworld::SkelVec3 P = kworld::combine(model[(size_t)b], at).pos;
				if (P.y >= y[f]) { continue; }
				const kworld::SkelVec3 o = kworld::rotate(c.orient, P);
				y[f] = P.y; gx[f] = c.pos[0] + o.x; gz[f] = c.pos[2] + o.z;
			}
		}
		const float low = std::min(y[0], y[1]);
		if (low < 1e8f)
		{
			const double here = ground(c.pos[0], c.pos[2]);
			double best = 1e9;
			for (int f = 0; f < 2; f++) { if (y[f] <= low + kPlanted) { best = std::min(best, ground(gx[f], gz[f]) - here); } }
			target = (float)std::clamp(best, -3.0, 3.0);
		}
	}
	c.footLift += (target - c.footLift) * std::min(1.0f, dt * kRate);
	c.pos[1] += c.footLift;
}

// Foot lock (foot IK slices 1 and 2; CLIENT_GOALS "Locomotion fidelity" > "Foot IK"): a post-process on the blended pose, after the lower layers mix,
// before the palette. While the lower layers' weighted contact windows (deriveContacts) hold a sole point down, the point stays at the world
// position where it planted; two-bone IK (thigh, calf) with the knee in the clip's own bend plane, the foot's orientation kept, so the whole
// foot translates. Release at the window's end or past ~98% of the leg's reach: the foot blends back to the animated pose over 0.15 s.
// kenshi-k16.17.7.5 (review steps 4-6): plain locomotion only. No plant in combat mode, at speed ~0, or once the body's yaw has moved more than
// 20 deg from the yaw at the plant (the foot is released as at a window's end and does not re-plant until the next window); at once on an action
// (technique, block), carry, prone, ragdoll or get-up, and while no lower layer plays. The body's height is Characters::footOffset's, one offset
// from the pose of this frame; the lock only moves feet. The pelvis drop, the stop / idle-fade pins, the lifted step, the placed free foot and
// the sole pitch fit are retired (client/retired/foot_lock_slice3.cpp). No solver library, no hand-authored data.
void Characters::lockFeet(Char& c, std::vector<kworld::SkelTransform>& local, std::vector<kworld::SkelTransform>& model, float dt)
{
	constexpr float kRelease = 0.15f;	// the blend back to the animated foot, seconds (MINE)
	constexpr float kReach = 0.98f;	// release past this share of the leg's length (CLIENT_GOALS)
	constexpr float kSlopeIn = 0.1f;	// the slope rotation's ramp-in at the stance's start, seconds (MINE)
	constexpr float kMaxSlope = 0.7f;	// the steepest foot tilt from the ground (40 deg, MINE): past it the ground is a cliff, not a stance
	constexpr float kYawRelease = 0.3491f;	// the body's yaw away from the plant's yaw that lets the foot go, rad (20 deg, review step 4)
	const kworld::Skeleton& sk = c.body->skel;
	auto clear = [](Char::FootLock& fl) { fl.active = false; fl.releasing = false; fl.shiftPrev = {}; fl.slopeW = 0; };
	const bool stopped = lockStopped(c);
	// plain locomotion: a walk or faster, not in combat mode (the knees shuffled there: the contact windows are the walk's, not a fighter's)
	const bool plain = !stopped && !c.spec.combat && (std::fabs(c.spec.move) >= 0.5f || std::fabs(c.spec.strafe) >= 0.5f);
	int thigh[2], calf[2], foot[2], toe[2];
	for (int f = 0; f < 2; f++)
	{
		thigh[f] = c.body->legBone[f][0];
		calf[f] = c.body->legBone[f][1];
		foot[f] = c.body->legBone[f][2];
		toe[f] = c.body->soleBone[f][1];	// the ball's bone (Toe0)
		if (thigh[f] < 0 || calf[f] < 0 || foot[f] < 0 || sk.bones[(size_t)thigh[f]].parent < 0 || sk.bones[(size_t)calf[f]].parent != thigh[f] || sk.bones[(size_t)foot[f]].parent != calf[f])
		{
			static std::atomic<bool> once{ false };	// the pose pass runs on several threads (anim.pose_threads)
			if (!once.exchange(true)) { fprintf(stderr, "kclient: foot lock: %s: no thigh / calf / foot chain, off\n", label(c.spec).c_str()); }
			for (Char::FootLock& fl : c.feetLock) { clear(fl); fl.event = 0; fl.animKnee = -1; fl.dPrev = -1; }
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
	const float yaw = 2.0f * std::atan2(c.orient.y, c.orient.w);	// the node's pure yaw rotation
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
	// foot IK slice 2: the drawn ground under a world x/z and its slope normal, from the ground callback (the terrain's triangles). Off the
	// terrain (the heightmap not within kOnTerrain of the wire y) the body stands on a floor, step or deck: a flat plane at the body's own y, the height the gravity settle
	// fits the soles to. Without the option or the callback the slice-1 lock stands (the plant stays where the animation put it).
	const bool onTerrain = footTerrain_ && ground && std::fabs(ground(c.spec.pos[0], c.spec.pos[2]) - c.spec.pos[1]) < kOnTerrain;
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
	// The slice-2 slope rotation a stance foot gets about its ankle this frame (identity when none)
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
	for (int f = 0; f < 2; f++)
	{
		Char::FootLock& fl = c.feetLock[f];
		fl.event = 0;
		fl.animKnee = -1;
		if (stopped) { clear(fl); fl.blocked = false; fl.dPrev = -1; continue; }	// at once, no blend: the action / carry / ragdoll pose owns the feet
		const int w = down[f][1] > down[f][0] ? 1 : 0;
		const bool hold = plain && total > 0 && down[f][w] > 0.5f * total;
		// the settled idle: kenshi-k16.17.7.2 (review cause B): only on terrain whose two feet stand at different heights (> 0.3 u to start, > 0.15 u to
		// stay, the stance's weight fading in by slopeW); on a floor or flat ground the animation's feet are right and the idle fit does nothing. It
		// is the ground fit (both soles on the drawn ground), not a plant: the foot's x/z follow the idle's own sway
		const bool slopeDelta = onTerrain && std::fabs(ground(c.pos[0] + c.feet[0][0], c.pos[2] + c.feet[0][2]) - ground(c.pos[0] + c.feet[1][0], c.pos[2] + c.feet[1][2])) > (fl.slopeW > 0.01f ? 0.15 : 0.3);
		const bool idleStance = footTerrain_ && onTerrain && slopeDelta && total <= 0 && c.soleFit && c.idleFor >= 0.5f;
		const int h = thigh[f], k = calf[f], a = foot[f];
		auto soleY = [&](int pt) { const int b = c.body->soleBone[f][pt]; if (b < 0) { return 1e9f; } kworld::SkelTransform at; at.pos = c.body->soleAt[f][pt]; return kworld::combine(model[(size_t)b], at).pos.y; };
		int pi;
		if (!idleStance) { fl.idlePi = -1; }
		if (idleStance && fl.idlePi >= 0) { pi = fl.idlePi; }	// per stance, not per frame (kenshi-k16.17.7.4)
		else if (idleStance) { pi = soleY(1) < soleY(0) ? 1 : 0; fl.idlePi = pi; }	// the idle's point is the lower of the two (the settle's rule), chosen before the slope rotation moves them
		else { pi = fl.active ? fl.point : w; }	// the planted point stays the one the lock took (heel and ball are far apart)
		const int sb = c.body->soleBone[f][pi];
		if (sb < 0) { clear(fl); fl.dPrev = -1; continue; }
		// slice 2: in stance the foot is pitched and rolled to the drawn ground's slope about the ankle (the clip's own pitch and roll kept),
		// ramped in at the stance's start and blended back with the release below; the toe follows
		const bool stance = footTerrain_ && ground && (hold || idleStance);
		fl.slopeW = std::clamp(fl.slopeW + (stance ? dt / kSlopeIn : -dt / kRelease), 0.0f, 1.0f);
		bool sloped = false;
		const kworld::SkelQuat sq = slopeQuat(a, fl.slopeW, &sloped);
		if (sloped)
		{
			model[(size_t)a].rot = kworld::multiply(sq, model[(size_t)a].rot);
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
		const kworld::SkelVec3 wp = kworld::rotate(c.orient, P);	// in world units from the body
		// the body has turned away from the plant: let go as at a window's end (blocked: no re-plant until that window is over)
		if (fl.active && !idleStance)
		{
			float dy = yaw - fl.yaw0;
			dy -= 6.2831853f * std::round(dy / 6.2831853f);
			if (std::fabs(dy) > kYawRelease) { fl.active = false; fl.blocked = true; fl.releasing = true; fl.releaseLen = std::max(vlen(fl.shiftPrev), 1e-4f); }
		}
		// the foot translation wanted this frame (== the sole's correction)
		kworld::SkelVec3 shift = {};
		if (!hold && !idleStance) { fl.blocked = false; }	// a new window may plant again
		if (idleStance)
		{
			// slice 2: the idle's foot set on the drawn ground under its sole; both feet fit with a bend, never past the reach (the body's height
			// is footOffset's). The foot's x/z follow the idle's own sway; at keeps them for a window that opens next.
			fl.event = fl.active ? 2 : 1;
			fl.active = true;
			fl.point = pi;
			fl.releasing = false;
			fl.blocked = false;
			fl.yaw0 = yaw;
			const double gy = groundAt(c.pos[0] + wp.x, c.pos[2] + wp.z, nullptr);
			fl.at[0] = c.pos[0] + wp.x; fl.at[1] = gy; fl.at[2] = c.pos[2] + wp.z;
			shift.y = (float)(gy - (c.pos[1] + wp.y));
		}
		else if (hold && !fl.active && !fl.blocked)
		{
			// the window's start: the plant is at the foot's current position (with whatever correction the last frame applied), so a window
			// that opens in the middle of a release does not jump; slice 2: set on the drawn ground under it
			fl.active = true;
			fl.point = w;
			fl.yaw0 = yaw;
			const kworld::SkelVec3 wq = kworld::rotate(c.orient, vadd(P, fl.shiftPrev));
			fl.at[0] = c.pos[0] + wq.x; fl.at[2] = c.pos[2] + wq.z;
			fl.at[1] = footTerrain_ && ground ? groundAt(fl.at[0], fl.at[2], nullptr) : c.pos[1] + wq.y;
			fl.releasing = false;
			fl.event = 1;
			shift = vsub(modelAt(fl.at), P);
		}
		else if (hold && fl.active) { shift = vsub(modelAt(fl.at), P); fl.event = 2; }
		else if (fl.active)
		{
			// the window ended (or the body slowed to rest, or a fight began): blend back from where the foot is
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
			const float keep = sp > 1e-4f ? std::max(sp - fl.releaseLen * dt / kRelease, 0.0f) / sp : 0.0f;
			shift = vmul(fl.shiftPrev, keep);
			fl.event = fl.blocked ? 4 : 3;	// past the reach or the yaw: the blocked release, else the window-end blend
			if (vlen(shift) <= 1e-4f) { fl.releasing = false; }
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
		if (d > dmax && hold && fl.active)
		{
			// the lock needs the leg past its reach: release from the last reachable foot pose (clamping to the extended leg would snap the knee),
			// and stay released until the window ends
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

// The foot lock's knee sanity (log feet): each knee's per-frame angle step and the steps past kKneePopDeg, reported every 600 frames, split by
// this frame's lock event (engage / held / release blend / blocked past the reach / none) and, against the same frame of the blended animation
// before the lock, the steps only the lock added. Measured with anim.foot_lock off too; a character whose knees do not move stays silent.
void Characters::kneeReport(Char& c)
{
	for (int f = 0; f < 2; f++)
	{
		Char::FootLock& fl = c.feetLock[f];
		const int th = c.body->legBone[f][0], ca = c.body->legBone[f][1], fo = c.body->legBone[f][2];
		if (lockStopped(c) || th < 0 || ca < 0 || fo < 0) { fl.knee = -1; fl.animKneePrev = -1; continue; }
		if (!footLock_) { fl.animKnee = -1; fl.animKneePrev = -1; }
		const kworld::SkelVec3 d1 = vsub(c.model[(size_t)ca].pos, c.model[(size_t)th].pos);
		const kworld::SkelVec3 d2 = vsub(c.model[(size_t)fo].pos, c.model[(size_t)ca].pos);
		const float l = vlen(d1) * vlen(d2);
		const float ang = l > 1e-6f ? std::acos(std::clamp(vdot(d1, d2) / l, -1.0f, 1.0f)) * 57.29578f : 0.0f;
		if (fl.knee >= 0)
		{
			const float step = std::fabs(ang - fl.knee);
			fl.kneeStep = std::max(fl.kneeStep, step);
			if (step > kKneePopDeg)
			{
				fl.pops++;
				const int cls = footLock_ ? fl.event : 0;
				fl.popsBy[cls]++;
				// did the blended animation pop here too, or is the lock the only reason?
				if (fl.animKnee >= 0 && fl.animKneePrev >= 0 && std::fabs(fl.animKnee - fl.animKneePrev) <= kKneePopDeg)
				{
					fl.addedPops++;
					fl.addedBy[cls]++;
				}
			}
		}
		fl.knee = ang;
		fl.animKneePrev = fl.animKnee;
	}
	Char::FootLock& l0 = c.feetLock[0];
	if (++l0.frames < 600) { return; }
	if (c.feetLock[0].kneeStep > 0 || c.feetLock[1].kneeStep > 0)
	{
		fprintf(stderr, "kclient: foot lock: %s: knee step max L %.1f R %.1f deg/frame, pops > %.0f deg L %d [engage %d held %d release %d blocked %d none %d] added %d/%d/%d/%d/%d, "
			"R %d [engage %d held %d release %d blocked %d none %d] added %d/%d/%d/%d/%d, held L %d R %d of %d frames\n",
			label(c.spec).c_str(), c.feetLock[0].kneeStep, c.feetLock[1].kneeStep, kKneePopDeg,
			c.feetLock[0].pops, c.feetLock[0].popsBy[1], c.feetLock[0].popsBy[2], c.feetLock[0].popsBy[3], c.feetLock[0].popsBy[4], c.feetLock[0].popsBy[0],
			c.feetLock[0].addedBy[1], c.feetLock[0].addedBy[2], c.feetLock[0].addedBy[3], c.feetLock[0].addedBy[4], c.feetLock[0].addedBy[0],
			c.feetLock[1].pops, c.feetLock[1].popsBy[1], c.feetLock[1].popsBy[2], c.feetLock[1].popsBy[3], c.feetLock[1].popsBy[4], c.feetLock[1].popsBy[0],
			c.feetLock[1].addedBy[1], c.feetLock[1].addedBy[2], c.feetLock[1].addedBy[3], c.feetLock[1].addedBy[4], c.feetLock[1].addedBy[0],
			c.feetLock[0].held, c.feetLock[1].held, l0.frames);
	}
	for (Char::FootLock& fl : c.feetLock)
	{
		fl.kneeStep = 0; fl.pops = 0; fl.held = 0; fl.addedPops = 0;
		for (int& n : fl.popsBy) { n = 0; }
		for (int& n : fl.addedBy) { n = 0; }
	}
	l0.frames = 0;
}

// The heel and ball points of each sole (log feet), from the bind-pose mesh: the vertices weighted >= 0.5 to the foot or toe bone; the sole =
// those within 15% of the ankle's height above the lowest; heel = the rearmost 15% of the sole along the foot (ankle -> toe joint), ball =
// the sole within 10% of the foot's length of the toe joint; both at the lowest height. MINE: the fractions.
void Characters::fitSolePoints(Body& b)
{
	const kworld::Skeleton& sk = b.skel;
	for (int f = 0; f < 2; f++)
	{
		const int foot = sk.bone(f ? "Bip01 R Foot" : "Bip01 L Foot"), toe = sk.bone(f ? "Bip01 R Toe0" : "Bip01 L Toe0");
		b.legBone[f][0] = sk.bone(f ? "Bip01 R Thigh" : "Bip01 L Thigh");
		b.legBone[f][1] = sk.bone(f ? "Bip01 R Calf" : "Bip01 L Calf");
		b.legBone[f][2] = foot;
		if (foot < 0 || toe < 0) { continue; }
		const kworld::SkelTransform fm = kworld::sampleModel(sk, nullptr, foot, 0), tm = kworld::sampleModel(sk, nullptr, toe, 0);
		double dir[2] = { tm.pos.x - fm.pos.x, tm.pos.z - fm.pos.z };
		const double len = std::hypot(dir[0], dir[1]);
		if (len <= 0) { continue; }
		dir[0] /= len;
		dir[1] /= len;
		std::vector<std::array<double, 3>> pts;	// x, z, y
		auto scan = [&](const ogre::Geometry& g)
		{
			if (g.boneIndex.empty()) { return; }
			for (uint32_t v = 0; v < g.count; v++)
			{
				for (int k = 0; k < 4; k++)
				{
					const int bi = g.boneIndex[v * 4 + k];
					if ((bi == foot || bi == toe) && g.boneWeight[v * 4 + k] >= 0.5f) { pts.push_back({ g.pos[v * 3], g.pos[v * 3 + 2], g.pos[v * 3 + 1] }); break; }
				}
			}
		};
		if (b.src.hasShared) { scan(b.src.shared); }
		for (const ogre::Submesh& sm : b.src.submeshes) { if (!sm.shared) { scan(sm.geometry); } }
		if (pts.empty()) { continue; }
		double minY = 1e30, lo = 1e30, hi = -1e30;
		for (const auto& p : pts) { minY = std::min(minY, p[2]); }
		const double band = minY + 0.15 * (fm.pos.y - minY);
		auto along = [&](const std::array<double, 3>& p) { return (p[0] - fm.pos.x) * dir[0] + (p[1] - fm.pos.z) * dir[1]; };
		for (const auto& p : pts) { if (p[2] <= band) { lo = std::min(lo, along(p)); hi = std::max(hi, along(p)); } }
		const double footLen = hi - lo;
		double sum[2][2] = {};
		int n[2] = {};
		for (const auto& p : pts)
		{
			if (p[2] > band) { continue; }
			const double a = along(p);
			const int which = a <= lo + 0.15 * footLen ? 0 : std::fabs(a - len) <= 0.1 * footLen ? 1 : -1;
			if (which < 0) { continue; }
			sum[which][0] += p[0];
			sum[which][1] += p[1];
			n[which]++;
		}
		for (int w = 0; w < 2; w++)
		{
			if (!n[w]) { continue; }
			kworld::SkelTransform at;
			at.pos = { (float)(sum[w][0] / n[w]), (float)minY, (float)(sum[w][1] / n[w]) };
			b.soleAt[f][w] = kworld::combine(kworld::inverse(w ? tm : fm), at).pos;
			b.soleBone[f][w] = w ? toe : foot;
		}
	}
}

// The sole contacts and the stride of each lower moving clip, derived at load (task L4; MINE: the sample count and contact height).
// The clip alone is posed over one cycle with this body's bone sizes (as pose() bakes them, minus the severed-limb scales), its heel and
// ball soles (Body::soleBone / soleAt) sampled: a point is in contact while within kContactHeight of its foot's own lowest sole over the
// cycle (not both feet's: a crouch walk keeps one foot up all cycle) and moving backward against the foot's heel -> ball axis. The
// stride is the ground the body covers in one cycle with those points planted (the planted figure, coordinator 2026-11-07); the older
// window sum (the windows' travel over their length, task L4b) is kept for the forward clips where the two agree within 5%. Nothing is
// hand-authored.
void Characters::deriveContacts(Char& c)
{
	for (Layer& l : c.layers)
	{
		if (!l.info || l.overlay || l.info->code != 0 || l.info->idle || !l.clip || l.clip->length <= 0) { continue; }
		contactsOf(c, l);
	}
}

// One clip's contact windows and stride over this body (deriveContacts; and the action clip at its start, anim.action_root).
void Characters::contactsOf(Char& c, Layer& l) const
{
	const kworld::Skeleton& sk = c.body->skel;
	const Body& body = *c.body;
	for (int f = 0; f < 2; f++) { for (int w = 0; w < 2; w++) { if (body.soleBone[f][w] < 0) { return; } } }
	if (!l.clip || l.clip->length <= 0) { return; }
	// The figures depend on the body (skeleton, sole points), the clip, its record and this character's bone sizes only: the same inputs give the
	// same result from the cache (a fresh layer's contacts are empty, so the cached ones are exactly what the derivation would append).
	const bool fresh = l.contacts.window[0].empty() && l.contacts.window[1].empty();
	std::string key;
	if (fresh)
	{
		const void* const ptrs[3] = { &body, l.clip, l.info };
		key.append((const char*)ptrs, sizeof ptrs);
		key.append((const char*)c.size.data(), c.size.size() * sizeof c.size[0]);
		key.append((const char*)c.posSize.data(), c.posSize.size() * sizeof c.posSize[0]);
		if (const auto it = contactCache_.find(key); it != contactCache_.end()) { l.contacts = it->second; return; }
	}
	CPUSTAGE_ACC("char.contacts.derive");	// a cache miss
	struct Store
	{
		std::unordered_map<std::string, Contacts>& cache;
		std::string& key;
		const Layer& l;
		bool on;
		~Store()
		{
			if (!on) { return; }
			if (cache.size() >= 16384) { cache.clear(); }	// bounded: ~40 clips a body and slider set
			cache.emplace(std::move(key), l.contacts);
		}
	} store{ contactCache_, key, l, fresh };
	std::vector<kworld::SkelTransform> model(sk.bones.size());
	// Only the sole bones are read, so only they and their ancestors are posed (in parentFirst order: the same sums, the same figures). The whole
	// skeleton 96 times per moving clip was 2.2 ms an add, 80% of char.add in a crowd burst (kenshi-k16.2.205.12.76).
	std::vector<char> need(sk.bones.size(), 0);
	for (int f = 0; f < 2; f++) { for (int w = 0; w < 2; w++) { for (int b = body.soleBone[f][w]; b >= 0 && !need[(size_t)b]; b = sk.bones[(size_t)b].parent) { need[(size_t)b] = 1; } } }
	std::vector<int> chain;
	for (const int b : sk.parentFirst) { if (need[(size_t)b]) { chain.push_back(b); } }
	// the clip at time t, the bones scaled by this body's sizes as pose() bakes them (the clip alone at weight 1)
	auto poseAt = [&](const kworld::SkeletonAnimation& clip, float t)
	{
		for (const int b : chain)
		{
			kworld::SkelTransform& m = model[(size_t)b];
			const kworld::SkelTransform local = kworld::sampleLocal(sk, &clip, b, t);
			const kworld::SkelVec3& size = c.size[(size_t)b];
			m.scale = { local.scale.x * size.x, local.scale.y * size.y, local.scale.z * size.z };
			const int p = sk.bones[(size_t)b].parent;
			if (p < 0) { m.pos = local.pos; m.rot = local.rot; continue; }
			const kworld::SkelVec3& ps = c.posSize[(size_t)b];
			const float sy = model[(size_t)p].scale.y;
			const kworld::SkelVec3 o = kworld::rotate(model[(size_t)p].rot, { local.pos.x * ps.x * sy, local.pos.y * ps.y * sy, local.pos.z * ps.z * sy });
			m.pos = { model[(size_t)p].pos.x + o.x, model[(size_t)p].pos.y + o.y, model[(size_t)p].pos.z + o.z };
			m.rot = kworld::multiply(model[(size_t)p].rot, local.rot);
		}
	};
	{
		const kworld::SkeletonAnimation& clip = *l.clip;
		double pos[kContactSamples][2][2][3];
		bool planted[kContactSamples][2][2];
		float lowest[2] = { 1e30f, 1e30f };
		for (int k = 0; k < kContactSamples; k++)
		{
			poseAt(clip, clip.length * (float)k / kContactSamples);
			for (int f = 0; f < 2; f++)
			{
				for (int w = 0; w < 2; w++)
				{
					kworld::SkelTransform at;
					at.pos = body.soleAt[f][w];
					const kworld::SkelVec3 q = kworld::combine(model[(size_t)body.soleBone[f][w]], at).pos;
					pos[k][f][w][0] = q.x;
					pos[k][f][w][1] = q.y;
					pos[k][f][w][2] = q.z;
					lowest[f] = std::min(lowest[f], q.y);
				}
			}
		}
		// moving backward: the point's horizontal speed at sample k against the foot's heel -> ball axis
		auto backward = [&](int k, int f, int w)
		{
			const double* p0 = pos[(k + kContactSamples - 1) % kContactSamples][f][w];
			const double* p1 = pos[(k + 1) % kContactSamples][f][w];
			const double fx = pos[k][f][1][0] - pos[k][f][0][0], fz = pos[k][f][1][2] - pos[k][f][0][2];
			const double len = std::hypot(fx, fz);
			return len > 1e-6 && ((p1[0] - p0[0]) * fx + (p1[2] - p0[2]) * fz) / len < 0;
		};
		float travel = 0, span = 0;
		for (int f = 0; f < 2; f++)
		{
			for (int w = 0; w < 2; w++)
			{
				for (int k = 0; k < kContactSamples; k++) { planted[k][f][w] = pos[k][f][w][1] - lowest[f] <= kContactHeight && backward(k, f, w); }
				// windows: the runs of contact samples, walked from a sample off the ground so one over the cycle boundary stays whole
				int start = 0;
				while (start < kContactSamples && planted[start][f][w]) { start++; }
				for (int i = 0; i < kContactSamples; )
				{
					if (!planted[(start + i) % kContactSamples][f][w]) { i++; continue; }
					int n = 0;
					while (n < kContactSamples && planted[(start + i + n) % kContactSamples][f][w]) { n++; }
					const int a = (start + i) % kContactSamples, b = (start + i + n - 1) % kContactSamples;
					const double fx = pos[a][f][1][0] - pos[a][f][0][0], fz = pos[a][f][1][2] - pos[a][f][0][2];
					const double len = std::hypot(fx, fz);
					const float win = (float)n / kContactSamples;
					const float gone = len > 1e-6 ? (float)(-((pos[b][f][w][0] - pos[a][f][w][0]) * fx + (pos[b][f][w][2] - pos[a][f][w][2]) * fz) / len) : 0.0f;
					if (gone > 0)
					{
						SoleContact sc;
						sc.start = (float)a / kContactSamples;
						sc.end = sc.start + win;
						sc.point = w;
						l.contacts.window[f].push_back(sc);
						// the stride counts the feet that reach the clip's ground only: a foot held above it all cycle (the crouch
						// walk's left, 0.56 u up) keeps its windows, but its slow hover would drag the mean (task L4b)
						if (lowest[f] - std::min(lowest[0], lowest[1]) <= kContactHeight) { travel += gone; span += win; }
					}
					i += n;
				}
			}
		}
		l.contacts.windows = span > 0 ? travel / span : 0.0f;
		// The planted figure (the animation lab's measure, coordinator 2026-11-07): with the down points held still the body must move
		// opposite to them; the mean of the planted points' velocities, carried over the whole cycle, is the ground per cycle (a coherent
		// clip pins both feet to one motion). Feet that never reach the clip's ground are left out, as in the window sum (task L4b).
		const float ground = std::min(lowest[0], lowest[1]);
		const bool reaches[2] = { lowest[0] - ground <= kContactHeight, lowest[1] - ground <= kContactHeight };
		const double dt = (double)clip.length / kContactSamples;
		double body[2] = {}, held = 0;	// model-space displacement while the feet are down (x = the character's right, z = forward)
		for (int k = 0; k < kContactSamples; k++)
		{
			double vx = 0, vz = 0; int n = 0;
			for (int f = 0; f < 2; f++)
			{
				for (int w = 0; w < 2; w++)
				{
					if (!planted[k][f][w] || !reaches[f]) { continue; }
					const double* p0 = pos[(k + kContactSamples - 1) % kContactSamples][f][w];
					const double* p1 = pos[(k + 1) % kContactSamples][f][w];
					vx += (p1[0] - p0[0]) / (2 * dt);
					vz += (p1[2] - p0[2]) / (2 * dt);
					n++;
				}
			}
			if (n) { body[0] -= vx / n * dt; body[1] -= vz / n * dt; held += dt; }
		}
		const float plantedStride = held > 0 ? (float)(std::hypot(body[0], body[1]) / held * clip.length) : 0.0f;
		// Which figure the stride warp uses: the planted one for the strafe (timed by lateral speed) and the stepback records (backward),
		// and for a forward clip only where the window sum is off by more than 5%; that switch is reported (once per clip name at load: the
		// numbers are this body's, `dump contacts` prints both). A clip with no planted figure keeps the window sum.
		const bool lateral = l.info->move == 0;	// strafe lower: the only lower moving record with no speed band
		const bool backwardClip = !lateral && l.info->play < 0;	// stepback / stepback short
		const bool differs = plantedStride > 0 && l.contacts.windows > 0 && std::fabs(plantedStride - l.contacts.windows) > 0.05f * l.contacts.windows;
		l.contacts.stride = plantedStride <= 0 ? l.contacts.windows : lateral || backwardClip || differs ? plantedStride : l.contacts.windows;
		if (differs && !l.info->name.empty())	// a technique clip has none: its stride is never read (no stride warp on an action)
		{
			static std::vector<std::string> reported;
			if (std::find(reported.begin(), reported.end(), l.info->name) == reported.end())
			{
				reported.push_back(l.info->name);
				fprintf(stderr, "kclient: contacts: %s%s: planted stride %.2f u/cycle vs windows %.2f (%+.0f%%), the planted figure is used\n",
					l.info->name.c_str(), lateral ? " (lateral)" : backwardClip ? " (backward)" : "", plantedStride, l.contacts.windows,
					100.0 * (plantedStride - l.contacts.windows) / l.contacts.windows);
			}
		}
	}
}

bool Characters::solePoints(size_t i, double out[2][2][3]) const
{
	if (i >= chars_.size() || !chars_[i].body) { return false; }
	const Char& c = chars_[i];
	for (int f = 0; f < 2; f++)
	{
		for (int w = 0; w < 2; w++)
		{
			const int b = c.body->soleBone[f][w];
			if (b < 0 || (size_t)b >= c.model.size()) { return false; }
			kworld::SkelTransform at;
			at.pos = c.body->soleAt[f][w];
			const kworld::SkelVec3 o = kworld::rotate(c.orient, kworld::combine(c.model[(size_t)b], at).pos);
			out[f][w][0] = c.pos[0] + o.x;
			out[f][w][1] = c.pos[1] + o.y;
			out[f][w][2] = c.pos[2] + o.z;
		}
	}
	return true;
}

