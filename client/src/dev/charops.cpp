// The chars / anim script ops, moved off main.cpp's exec_cmd (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.23, pass 4 of 6):
// dump contacts | feet | bones | anim | idle | motion | bars, log feet | hits, and the per-frame foot log behind `log feet`. They read the
// characters' drawn poses beside the wire and the terrain; everything they need of the game comes through dev::Context. Built only with
// KCLIENT_DEV_TOOLS.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "characters.hpp"
#include "collisionpick.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#include "kworld/heightmap.hpp"
#include "shadow.hpp"
#include "simview.hpp"
#include "ui/progressbars.hpp"

namespace dev
{
namespace
{

// log feet's state: which characters, and the seconds of character time since it started.
bool footLog = false;
int footLogMode = 1;	// 0 = the one id in footLogId, 1 = players, 2 = combat, 3 = all within the interest radius (MINE cap 32, nearest the camera)
uint64_t footLogId = 0;
double footLogT = 0;

// The characters a per-frame log covers (log feet): the one id, the players, the sim's combat ones, or all drawn within the interest radius.
std::vector<uint64_t> log_ids(const Context& cx)
{
	std::vector<uint64_t> ids;
	if (footLogMode == 3)	// all: nearest the camera first, within the interest radius
	{
		std::vector<std::pair<double, uint64_t>> near;
		for (const uint64_t id : cx.sim.characterIds())
		{
			double feet[3];
			float hull[3];
			if (!cx.sim.placed(id, feet, hull)) { continue; }
			if (std::hypot(feet[0] - cx.pos[0], feet[2] - cx.pos[2]) > SimView::kInterestRadius) { continue; }
			near.emplace_back(std::hypot(feet[0] - cx.pos[0], feet[1] - cx.pos[1], feet[2] - cx.pos[2]), id);
		}
		std::sort(near.begin(), near.end());
		for (size_t i = 0; i < near.size() && i < 32; i++) { ids.push_back(near[i].second); }
	}
	else
	{
		for (const auto& [id, ch] : cx.sim.decoder().chars)
		{
			const bool want = footLogMode == 0 ? id == footLogId : footLogMode == 2 ? (ch.cur.flags & simlink::kCombat) != 0 : (ch.cur.flags & simlink::kPlayer) != 0;
			if (want) { ids.push_back(id); }
		}
		std::sort(ids.begin(), ids.end());
	}
	return ids;
}

}	// namespace

// log feet (docs/CLIENT_GOALS.md "Locomotion fidelity", step 1): per frame and logged character, one line:
// footlog T DT id ID tick K wire FWD LAT wv WV legs MOVE STRAFE phase P flags 0xX act "NAME" ca A cd D stop S lie L rise R warp W steer T drop D lock L R pose P | L heel X Y Z g G ball X Y Z g G | R ... | clips
// lock = the foot lock per foot: 1 held at its plant, 2 released past the leg's reach (its window still down), 0 none (anim.foot_lock off, no window, or the blend back)
// pose = this pose's largest local joint rotation step, deg/frame (the blend's peak angular velocity, on or off anim.inertialize)
// tick = the wire sample's tick (the sim's, to line a line up with the sim's own per-tick logs); wire = that sample's fwd / lat (the sim's own
// velocity fields), wv = its position displacement (what the drawn body follows; legs above wv is the catch-up); act = the wire's action strid
// ("" none: a clip in `clips` while act is empty is the client holding it), ca / cd = the client's action layer index (-1 none) and its done flag;
// legs = what the locomotion got (Spec.move / strafe, i.e. SimView's sl.vFwd / vLat: the drawn velocity);
// flags = the wire bits (1 combat, 8 KO, 16 dead, 32 prone); stop = the stop-contact latch; lie = the KO lying fade's layer (-1 none);
// rise = the get-up clip playing (sl.rising); warp / steer = the stride warp and the stop steer touched this frame's phase rate;
// drop = the pelvis drop's body y this frame (foot IK slice 3, anim.foot_pelvis);
// actroot (anim.action_root, on a travelling action only): the clip root's replay point (path), the wire position, the drawn body and the
// eased correction (drawn = path + corr, clamped to lead the wire by at most a step), world x/z; a push shows as the wire moving beyond path.
// `log feet all`: the drawn characters within the interest radius, nearest the camera first, MINE cap 32. `combat`: the wire's kCombat.
// client/tools/footlog.py (script: client/tools/footlog_stop.txt) turns it into contact, slide and float figures.
void footFrame(const Context& cx, double dt)
{
	if (!footLog) { return; }
	footLogT += dt;
	const std::vector<uint64_t> ids = log_ids(cx);
	for (const uint64_t id : ids)
	{
		const auto ch = cx.sim.decoder().chars.find(id);
		if (ch == cx.sim.decoder().chars.end()) { continue; }
		const int index = cx.sim.charIndex(id);
		double p[2][2][3];
		if (index < 0 || !cx.chars.solePoints((size_t)index, p)) { continue; }
		const Characters::Spec& s = cx.chars.live((size_t)index);
		const Characters::LocoState ls = cx.chars.locoState((size_t)index);
		// The ground under the feet: the first collision shape below them (a floor, a stair), else the terrain. A ray because the character's y is
		// the sim's and the heightmap under a raised floor is the terrain below it (the Hub's floors sit ~25 u up); MINE, for this log only.
		double under = NAN;
		{
			double feet[3];
			float hull[3];
			double t = 0;
			if (cx.sim.placed(id, feet, hull))
			{
				const double from[3] = { feet[0], feet[1] + 1.0, feet[2] }, dir[3] = { 0, -1, 0 };
				if (cx.collide.ray(from, dir, 6.0, t)) { under = from[1] - t; }
			}
		}
		const double span = ((double)ch->second.cur.tick - (double)ch->second.prev.tick) / SimView::kTickRate;
		const double wv = span > 0 ? std::hypot(ch->second.cur.pos[0] - ch->second.prev.pos[0], ch->second.cur.pos[2] - ch->second.prev.pos[2]) / span : 0.0;
		printf("footlog %.4f %.4f id %llu tick %llu wire %.3f %.3f wv %.3f legs %.3f %.3f phase %.4f flags 0x%x act \"%s\" ca %d cd %d stop %d lie %d rise %d warp %d steer %d drop %.3f lock %d %d pose %.2f",
			footLogT, dt, (unsigned long long)id, (unsigned long long)ch->second.cur.tick, ch->second.cur.fwd, ch->second.cur.lat, wv, s.move, s.strafe,
			cx.chars.phase((size_t)index), ch->second.cur.flags, cx.sim.decoder().str(ch->second.cur.action).c_str(), ls.action, (int)ls.done, (int)ls.stop, ls.lie,
			(int)ls.rising, (int)ls.warped, (int)ls.steered, ls.drop, ls.lock[0], ls.lock[1], cx.chars.poseStep((size_t)index));
		for (int f = 0; f < 2; f++)
		{
			printf(" | %s", f ? "R" : "L");
			for (int w = 0; w < 2; w++)
			{
				const double g = std::isnan(under) ? (cx.chars.ground ? cx.chars.ground(p[f][w][0], p[f][w][2]) : cx.hm->terrainHeight(p[f][w][0], p[f][w][2])) : under;
				printf(" %s %.3f %.3f %.3f g %.3f", w ? "ball" : "heel", p[f][w][0], p[f][w][1], p[f][w][2], g);
			}
		}
		// kenshi-k16.17.7.24.1: the foot IK's inputs per frame: the two options in effect, the sim's y, the terrain under it and whether the IK took it
		// for the ground (within 2 u), the collision floor under the feet (nan: none), the drawn body y, the standing / floor offsets and the pelvis lift
		printf(" | ik lock %d terr %d on %d sim_y %.3f terrain_y %.3f floor_y %.3f drawn_y %.3f soleLift %.3f rootShift %.3f lift %.3f", (int)ls.footLock, (int)ls.footTerrain, (int)ls.onTerrain,
			ls.simY, ls.terrainY, under, ls.drawnY, ls.soleLift, ls.rootShift, ls.drop);
		printf(" | %s", cx.chars.animSummary((size_t)index).c_str());
		// anim.action_root (the pushed-swinger check, kenshi-k16.2.78.2): on a travelling action, the clip root's replay point beside the wire
		// and the drawn body, so a squadmate's push shows as wire movement beyond the clip root and the corr easing the draw onto it
		Characters::ActionRoot ar;
		if (cx.chars.actionRoot((size_t)index, ar))
		{
			printf(" | actroot %s t %.3f/%.3f path %.3f %.3f wire %.3f %.3f drawn %.3f %.3f corr %.3f %.3f", ar.clip.c_str(), ar.clipTime, ar.length,
				ar.path[0], ar.path[1], ar.wire[0], ar.wire[1], ar.drawn[0], ar.drawn[1], ar.corr[0], ar.corr[1]);
		}
		printf("\n");
	}
}

// The `dump` / `log` ops of the characters (op is "dump" or "log", a the args after it). False: not one of them.
bool charsOp(const Context& cx, const std::string& op, const std::vector<std::string>& a)
{
	if (op == "dump" && a.size() == 1 && a[0] == "contacts")	// dump contacts: each player character's lower moving clips, their sole contact windows and stride (stdout)
	{
		for (const auto& [id, ch] : cx.sim.decoder().chars)
		{
			const int index = (ch.cur.flags & simlink::kPlayer) ? cx.sim.charIndex(id) : -1;
			if (index < 0) { continue; }
			printf("contacts %llu flags 0x%x:\n%s", (unsigned long long)id, ch.cur.flags, cx.chars.contactReport((size_t)index).c_str());
		}
		return true;
	}
	if (op == "dump" && (a.size() == 1 || (a.size() == 2 && a[1] == "all")) && a[0] == "feet")	// dump feet [all]: each player character's (all: every drawn character's) lowest sole vertex per foot (skinned this frame) minus the terrain under it (stdout)
	{
		const bool all = a.size() == 2;
		for (const auto& [id, ch] : cx.sim.decoder().chars)
		{
			const int index = all || (ch.cur.flags & simlink::kPlayer) ? cx.sim.charIndex(id) : -1;
			double s[2][3];
			if (index < 0 || !cx.chars.soles((size_t)index, s)) { continue; }
			// against the drawn ground (terrainHeight: the mesh's triangles), not the bilinear height()
			printf("feet %llu flags 0x%x wire y %.2f (%+.2f over the drawn ground):", (unsigned long long)id, ch.cur.flags, ch.cur.pos[1], ch.cur.pos[1] - cx.hm->terrainHeight(ch.cur.pos[0], ch.cur.pos[2]));
			for (int f = 0; f < 2; f++) { printf(" %s %.2f %.2f %.2f terrain %.2f gap %+.2f", f ? "R" : "L", s[f][0], s[f][1], s[f][2], cx.hm->terrainHeight(s[f][0], s[f][2]), s[f][1] - cx.hm->terrainHeight(s[f][0], s[f][2])); }
			printf(" %s\n", cx.chars.fitReport((size_t)index).c_str());
			// where the ground shadow of the sole and of points 5 / 10 / 20 u above it should land (the shadow map's light), on screen
			const ShadowParams& sp = cx.shadow.params();
			printf("feet shadow texel %.3f %.3f ldir %.3f %.3f %.3f\n", sp.texel[0], sp.texel[1], sp.ldir[0], sp.ldir[1], sp.ldir[2]);
			for (int f = 0; f < 2; f++)
			{
				float sx, sy;
				printf("feet screen %s sole", f ? "R" : "L");
				if (cx.worldToScreen(s[f], sx, sy)) { printf(" %.0f,%.0f", sx, sy); }
				for (const double up : { 0.0, 5.0, 10.0, 20.0 })
				{
					const double h = s[f][1] + up - cx.hm->height(s[f][0], s[f][2]);
					const double g[3] = { s[f][0] - sp.ldir[0] * h / sp.ldir[1], 0, s[f][2] - sp.ldir[2] * h / sp.ldir[1] };
					const double gp[3] = { g[0], cx.hm->height(g[0], g[2]), g[2] };
					if (cx.worldToScreen(gp, sx, sy)) { printf(" | +%.0f shadow %.0f,%.0f", up, sx, sy); }
				}
				printf("\n");
			}
		}
		return true;
	}
	if (op == "log" && a.size() == 2 && a[0] == "feet")	// log feet ID|players|combat|all|off: per frame, a character's heel / ball sole points, ground, speeds, state and clips (footFrame, stdout)
	{
		footLog = a[1] != "off";
		footLogMode = a[1] == "players" ? 1 : a[1] == "combat" ? 2 : a[1] == "all" ? 3 : a[1] == "off" ? 1 : 0;
		footLogId = footLogMode == 0 ? strtoull(a[1].c_str(), nullptr, 10) : 0;
		footLogT = 0;
		if (footLog) { cx.warnFrozenAnimLog("log feet"); }
		return true;
	}
	if (op == "log" && a.size() == 2 && a[0] == "hits")	// log hits on|off: every EVENT 1 as it arrives (tick, victim, attacker, result, damage, point; stdout)
	{
		cx.sim.logHits = a[1] != "off";
		return true;
	}
	if (op == "dump" && a.size() == 2 && a[0] == "bones")	// dump bones ID: a wire character's drawn body node and key bones in the world, and their height over the pose tail / node (stdout)
	{
		const uint64_t id = strtoull(a[1].c_str(), nullptr, 10);
		const auto it = cx.sim.decoder().chars.find(id);
		const int index = it == cx.sim.decoder().chars.end() ? -1 : cx.sim.charIndex(id);
		if (index < 0) { cx.scriptError(cx.frames, "dump"); }
		else
		{
			const simlink::Dynamic& d = it->second.cur;
			double node[3];
			cx.chars.nodeWorld((size_t)index, node);
			printf("bones %llu flags 0x%x wire y %.3f pose y %.3f yaw %.3f clip \"%s\" node %.3f %.3f %.3f root shift %+.3f terrain %+.3f\n", (unsigned long long)id, d.flags, d.pos[1], d.pose[1], d.pose[3],
				cx.sim.decoder().str(d.poseClip).c_str(), node[0], node[1], node[2], cx.chars.rootShift((size_t)index),
				cx.hm ? node[1] - cx.hm->terrainHeight(node[0], node[2]) : 0.0);
			for (const char* name : { "Bip01", "Bip01 Pelvis", "Bip01 L Foot", "Bip01 R Foot", "Bip01 Head" })
			{
				double p[3];
				if (!cx.chars.boneWorld((size_t)index, name, p)) { continue; }
				printf("  %-13s %.3f %.3f %.3f   node %+.3f   pose %+.3f   terrain %+.3f\n", name, p[0], p[1], p[2], p[1] - node[1], p[1] - d.pose[1],
					cx.hm ? p[1] - cx.hm->terrainHeight(p[0], p[2]) : 0.0);
			}
			printf("  clips: %s\n", cx.chars.clipReport((size_t)index).c_str());
		}
		return true;
	}
	if (op == "dump" && a.size() == 1 && a[0] == "bars")	// dump bars: per progress row, where its bar is anchored (placed(), the sim's feet) against the drawn body (node, pelvis, head) and the bar panel's centre, all in framebuffer pixels (stdout; kenshi-h2v.16.7.1)
	{
		for (const simlink::SimLinkDecoder::TaskProgress& t : cx.sim.decoder().tasks)
		{
			double pos[3];
			float hull[3];
			const int index = cx.sim.charIndex(t.ch);
			if (!cx.sim.placed(t.ch, pos, hull) || index < 0) { continue; }
			const auto px = [&cx](const double p[3]) { static char s[40]; float x = 0, y = 0; if (cx.worldToScreen(p, x, y)) { snprintf(s, sizeof s, "%.1f,%.1f", x, y); } else { snprintf(s, sizeof s, "behind"); } return std::string(s); };
			double node[3], pel[3] = {}, head[3] = {};
			cx.chars.nodeWorld((size_t)index, node);
			const bool hasPel = cx.chars.boneWorld((size_t)index, "Bip01 Pelvis", pel), hasHead = cx.chars.boneWorld((size_t)index, "Bip01 Head", head);
			const ui::Rect r = cx.progressBars->placed(t.ch);
			// the drawn body's screen box over these bones (the head, the spine, the pelvis, both feet and hands): its centre x is where the body reads as centred
			float lo = 1e9f, hi = -1e9f;
			for (const char* name : { "Bip01 Head", "Bip01 Spine1", "Bip01 Pelvis", "Bip01 L Foot", "Bip01 R Foot", "Bip01 L Hand", "Bip01 R Hand" })
			{
				double bp[3];
				float bx = 0, by = 0;
				if (cx.chars.boneWorld((size_t)index, name, bp) && cx.worldToScreen(bp, bx, by)) { lo = std::min(lo, bx); hi = std::max(hi, bx); }
			}
			printf("bars %llu kind %d yaw %.1f placed %.1f %.1f %.1f px %s | node %.1f %.1f %.1f px %s | pelvis %s | head %s | body box x %.1f..%.1f centre %.1f | panel centre %.1f,%.1f (x %.1f w %.1f y %.1f h %.1f)\n", (unsigned long long)t.ch, (int)t.kind, cx.yaw * 180 / 3.14159265358979,
				pos[0], pos[1], pos[2], px(pos).c_str(), node[0], node[1], node[2], px(node).c_str(), hasPel ? px(pel).c_str() : "-", hasHead ? px(head).c_str() : "-", lo, hi, (lo + hi) * 0.5f, r.x + r.w * 0.5f, r.y + r.h * 0.5f, r.x, r.w, r.y, r.h);
		}
		return true;
	}
	if (op == "dump" && !a.empty() && a[0] == "anim" && (a.size() == 1 || (a.size() == 2 && (a[1] == "all" || a[1].starts_with("id=")))))	// dump anim [all|id=N]: each player character's flags and the clips playing (all: every character on the wire; id=N: that one; stdout)
	{
		const bool all = a.size() == 2 && a[1] == "all";
		const uint64_t only = a.size() == 2 && a[1].starts_with("id=") ? std::stoull(a[1].substr(3)) : 0;
		for (const auto& [id, ch] : cx.sim.decoder().chars)
		{
			if ((only && id != only) || (!all && !only && !(ch.cur.flags & simlink::kPlayer))) { continue; }
			const int index = cx.sim.charIndex(id);
			if (index >= 0) { printf("anim %llu flags 0x%x: %s\n", (unsigned long long)id, ch.cur.flags, cx.chars.animSummary(index).c_str()); }
		}
		return true;
	}
	if (op == "dump" && a.size() == 1 && a[0] == "idle")	// dump idle: every drawn character's weapon category, combat / stealth and held idle record (stdout)
	{
		printf("%s", cx.chars.idleLines().c_str());
		return true;
	}
	if (op == "dump" && (a.size() == 1 || (a.size() == 2 && a[1] == "all")) && a[0] == "motion")	// dump motion [all]: each player character's (all: every one's) wire fwd / lat, wire XZ speed (last two samples), combat flag, drawn position, wall time (stdout)
	{
		const bool all = a.size() == 2;
		const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		for (const auto& [id, ch] : cx.sim.decoder().chars)
		{
			if (!all && !(ch.cur.flags & simlink::kPlayer)) { continue; }
			double at[3] = {};
			float hull[3];
			cx.sim.placed(id, at, hull);
			const double span = ((double)ch.cur.tick - (double)ch.prev.tick) / SimView::kTickRate;
			const double wire = span > 0 ? std::hypot(ch.cur.pos[0] - ch.prev.pos[0], ch.cur.pos[2] - ch.prev.pos[2]) / span : 0.0;
			printf("motion %llu t %.4f fwd %.2f lat %.2f wire %.2f combat %d dead %d at %.2f %.2f %.2f chars %zu\n", (unsigned long long)id, now, ch.cur.fwd, ch.cur.lat, wire,
				(ch.cur.flags & simlink::kCombat) != 0, (ch.cur.flags & (simlink::kDead | simlink::kKO)) != 0, at[0], at[1], at[2], cx.chars.size());
		}
		return true;
	}
	return false;
}

}	// namespace dev
