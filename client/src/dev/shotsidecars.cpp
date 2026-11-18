// The shot's debug sidecars (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.13): the text files written beside a screenshot. The
// PNG is the game's (F12, the script's `screenshot`); these are development reports:
//   <shot>.txt          the scene behind the shot (camera, options, cursor pick, owner / primitive / rule, hints, time, weather, keys) so another
//                       session can frame the same view or say what the cursor was on;
//   <shot>.events.txt   the critical-events log's last 10 minutes, and its "events:" line in the .txt;
//   <shot>.trails.tsv   the debug trails' points while they record, and their "trails:" line;
//   <shot>.trees.txt    the DEBUG_TREE of every on-screen character and its squad (KCLIENT_DEBUG_VIEW), and their "trees:" line.
// main.cpp reaches this through dev/devhooks.hpp; what it needs of the game is the shared dev::Context (dev/debughost.hpp), never App.
// Built only with KCLIENT_DEV_TOOLS.
#include "dev/devhooks.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "characters.hpp"
#include "clientnav.hpp"
#include "config.hpp"
#include "dev/debughost.hpp"
#include "dev/lightterms.hpp"
#include "eventlog.hpp"
#include "pick.hpp"
#include "simlink.hpp"
#include "simview.hpp"
#include "sky.hpp"
#include "squads.hpp"
#include "trails.hpp"
#include "windowgate.hpp"
#include "worldbuildings.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#endif

namespace dev
{
namespace
{

constexpr double kDeg = 180 / 3.14159265358979;

// The options a shot was rendered with, away from their defaults (kenshi-k16.2.285): the camera lines reproduce a look only while the options match,
// and the user's config file may have moved on since the shot (the user's Storm House sun leak did not reproduce with default options, and neither the
// sidecar nor the events file said what was set then). window.* keys are left out: window geometry is not a look. One line, "key=value key=value"
// sorted by key; every declared option can be given back as --set KEY=VALUE (cfg::set), so the same list goes on the "recreate:" line.
std::vector<std::pair<std::string, std::string>> shot_changed_options()
{
	std::vector<std::pair<std::string, std::string>> out;
	for (const cfg::Info& i : cfg::list())
	{
		if (i.key.rfind("window.", 0) == 0 || i.value == i.def) { continue; }
		out.emplace_back(i.key, i.value);
	}
	std::sort(out.begin(), out.end());	// list() is in key order already; sorted here so the line does not depend on the registry's order
	return out;
}

// A value safe to paste as one shell word; anything else is single-quoted (a Text option may hold spaces, e.g. session.server_args).
std::string shot_shell_word(const std::string& v)
{
	if (!v.empty() && v.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-+:@/%,=") == std::string::npos) { return v; }
	std::string q = "'";
	for (const char c : v) { if (c == '\'') { q += "'\\''"; } else { q += c; } }
	return q + "'";
}

// The screenshot key's sidecar (user 2026-11-04): the scene behind a shot as text, for another session to frame the same view (kinspect knavshot)
// or a bug report to say what the cursor was on. One "key: value" per line; "recreate" is the kclient args for the same camera.
void shot_meta_file(const Context& cx, const std::string& path)
{
	FILE* f = fopen(path.c_str(), "w");
	if (!f) { fprintf(stderr, "failed to write %s\n", path.c_str()); return; }
	double heading = std::fmod(cx.yaw * kDeg, 360.0);
	if (heading < 0) { heading += 360.0; }
	if (const std::string build = buildLine(); !build.empty()) { fprintf(f, "%s\n", build.c_str()); }	// k16.2.127: the binary's build first, always (with or without --bead)
	fprintf(f, "command: %s\n", cx.cmdline.c_str());
	// kenshi-k16.2.285: the options away from their defaults, so the look can be reproduced on a base whose defaults (or the user's cfg) moved on
	const std::vector<std::pair<std::string, std::string>> changed = shot_changed_options();
	std::string options, setArgs;
	for (const auto& [key, value] : changed)
	{
		options += (options.empty() ? "" : " ") + key + "=" + shot_shell_word(value);
		setArgs += " --set " + shot_shell_word(key + "=" + value);
	}
	fprintf(f, "options: %s\n", options.empty() ? "(defaults)" : options.c_str());
	fprintf(f, "size: %dx%d\nfovy: 45\n", view::width(), view::height());
	fprintf(f, "camera: %s eye %.1f %.1f %.1f heading %s %.1f pitch %.1f\n", cx.cameraMode(), cx.pos[0], cx.pos[1], cx.pos[2], cx.compass(heading), heading, cx.pitch * kDeg);
	if (cx.follow)
	{	// the follow camera's own numbers; no recreate: kclient has --follow NAME|id=N but no args for the distance, pitch or yaw offset
		fprintf(f, "follow: id %llu dist %.1f pitch %.1f yaw-offset %.1f\n", (unsigned long long)cx.followId, cx.followDist, cx.pitch * kDeg, cx.followOffset * kDeg);
	}
	else if (cx.rts)
	{
		fprintf(f, "pivot: %.1f %.1f %.1f dist %.1f on %s\n", cx.focus[0], cx.focus[1], cx.focus[2], cx.rtsDist, cx.focusOn);
		fprintf(f, "recreate: --size %dx%d --camera-pos %.1f %.1f %.1f --camera-yaw %.1f --camera-pitch %.1f --camera-dist %.1f%s\n", view::width(), view::height(), cx.focus[0], cx.focus[1], cx.focus[2], cx.yaw * kDeg, cx.pitch * kDeg, cx.rtsDist, setArgs.c_str());
	}
	if (viewing())
	{	// kenshi-k16.2.299: the mesh viewer has no world: no heightmap, nav, sim, sky or buildings to ask (pick_screen samples the heightmap and aborts)
		fprintf(f, "world: n/a (mesh viewer): pick, pick ray, owner, rule, hints, ground, tracked, selected, time, weather, building and sim do not apply\n");
		fprintf(f, "cursor: at %.0f %.0f\n", cx.mouseX, cx.mouseY);
		fprintf(f, "keys held:%s\n", cx.keysHeld().c_str());
		fclose(f);
		printf("wrote %s\n", path.c_str());
		return;
	}
	static const char* const kKinds[] = { "none", "ground", "character", "item", "door", "container", "producer", "bed" };
	static const char* const kReach[] = { "unknown", "yes", "no" };
	std::vector<PickSkip> skipped;	// the shapes the click ray passed and why (kenshi-k16.2.259)
	const Pick p = cx.pickScreen(cx.mouseX, cx.mouseY, skipped);
	{	// the ray the pick walked: from the near plane, so nothing nearer than it can pick (kenshi-k16.2.259)
		const double near = cx.cameraNear();
		double dir[3], from[3];
		cx.mouseRay(cx.mouseX, cx.mouseY, dir);
		for (int k = 0; k < 3; k++) { from[k] = cx.pos[k] + dir[k] * near; }
		fprintf(f, "pick ray: near plane %.2f from %.1f %.1f %.1f toward %.3f %.3f %.3f (eye %.1f %.1f %.1f)\n", near, from[0], from[1], from[2], dir[0], dir[1], dir[2],
			cx.pos[0], cx.pos[1], cx.pos[2]);
	}
	fputs(cx.clicksReport().c_str(), f);	// kenshi-k16.2.259.3: the last clicks, each at its own frame and camera, before the cursor pick at shot time
	fprintf(f, "cursor: %s at %.0f %.0f%s\n", cx.cursorText().c_str(), cx.mouseX, cx.mouseY, cx.look ? " (mouse look)" : "");
	fprintf(f, "pick: %s id %llu hit %.1f %.1f %.1f walkable %s\n", kKinds[p.kind], (unsigned long long)p.id, p.hit[0], p.hit[1], p.hit[2], p.kind == Pick::None ? "-" : kReach[(int)cx.nav.walkable(p.hit)]);
	fputs(cx.pickOwnerReport(p, cx.mouseX, cx.mouseY, skipped).c_str(), f);
	// kenshi-k16.2.294.2: what the pointer's target can do, as the control-hints resolver lists it (empty until the steps in DESIGN 4 move the kinds
	// in). The same block the dev `hints at X Y` op prints; one line for the target, then one per action.
	fputs(cx.hintsForPick(p).c_str(), f);
	const uint64_t tracked = cx.cameraSubject();
	double feet[3];
	float hull[3];
	if (tracked && cx.sim.placed(tracked, feet, hull)) { fprintf(f, "tracked: %llu at %.1f %.1f %.1f\n", (unsigned long long)tracked, feet[0], feet[1], feet[2]); }
	for (const uint64_t id : cx.sim.selected())
	{
		if (cx.sim.placed(id, feet, hull)) { fprintf(f, "selected: %llu at %.1f %.1f %.1f\n", (unsigned long long)id, feet[0], feet[1], feet[2]); }
	}
	const GameClock& gc = cx.sky.clock;
	const int minutes = (int)(gc.hours * 60.0);
	fprintf(f, "time: %02d:%02d day %d sun altitude %.1f\n", minutes / 60, minutes % 60, gc.day + 1, std::asin(cx.sky.sunDir()[1]) * kDeg);
	fputs(lightLine().c_str(), f);	// the light debugger's switches: "light: all on" or what is off (empty line in a release build)
	fprintf(f, "weather: %s\n", cx.sky.regionalFog ? ("auto: " + cx.weatherMix).c_str() : cx.sky.weather.name.c_str());
	if (const WorldBuildings::FloorStatus fl = cx.buildings.floorStatus(); fl.valid)
	{
		fprintf(f, "building: %s floor %d/%d, lead on floor %d (%s)\n", fl.name.c_str(), fl.shown, fl.top, fl.leadFloor,
			fl.leadFloor < 0 ? "no lead" : fl.following ? "following" : "not following");
	}
	if (cx.sim.active()) { fprintf(f, "sim: %s\n", cx.sim.status().c_str()); }
	fputs(cx.hudText().c_str(), f);	// kenshi-k16.2.335.1: the developer text block's lines under --debug
	fprintf(f, "keys held:%s\n", cx.keysHeld().c_str());
	fclose(f);
	printf("wrote %s\n", path.c_str());
}

// <shot>.events.txt: the critical-events log's last 10 minutes beside a shot (docs/CLIENT.md "Critical-events log"), and an "events:" line in
// the shot's .txt sidecar (created for a scripted shot) and on the console.
void shot_events(const std::string& shotPath)
{
	const std::string base = shotPath.ends_with(".png") ? shotPath.substr(0, shotPath.size() - 4) : shotPath;
	size_t n = 0;
	// k16.2.127: the build is the events file's first line (it is what tells two runs' logs apart)
	if (!evlog::writeFile(base + ".events.txt", 600, n, buildLine())) { fprintf(stderr, "kclient: events: cannot write %s.events.txt\n", base.c_str()); return; }
	const std::string note = "events: " + std::filesystem::path(base + ".events.txt").filename().string() + " (" + std::to_string(n) + " in the last 10 min; run file " + evlog::runFile() + ")\n";
	std::string build = buildLine();
	if (!build.empty()) { build += "\n"; }	// a .txt made here (a scripted shot's) starts with it too
	std::error_code ec;
	const bool fresh = !std::filesystem::exists(base + ".txt", ec);
	if (FILE* side = fopen((base + ".txt").c_str(), "a")) { if (fresh) { fputs(build.c_str(), side); } fputs(note.c_str(), side); fclose(side); }
	printf("%s", note.c_str());
}

// <shot>.trails.tsv: the whole fight's trails beside a shot (kenshi-k16.2.73): every kept point, one row each, and a "trails:" line in the shot's
// .txt sidecar. Written only while the debug trails are recording (--debug, debug.trails on): nothing otherwise.
void shot_trails(const Context& cx, const std::string& shotPath)
{
	if (!cx.debug || !cx.trails.on()) { return; }
	const std::string base = shotPath.ends_with(".png") ? shotPath.substr(0, shotPath.size() - 4) : shotPath;
	const std::string path = base + ".trails.tsv";
	if (!cx.trails.writeFile(path)) { fprintf(stderr, "kclient: trails: cannot write %s\n", path.c_str()); return; }
	const std::string note = "trails: " + std::filesystem::path(path).filename().string() + "\n";
	if (FILE* side = fopen((base + ".txt").c_str(), "a")) { fputs(note.c_str(), side); fclose(side); }
	printf("%s", note.c_str());
}

#ifdef KCLIENT_DEBUG_VIEW
// --- The screenshot's debug trees (docs/CLIENT.md "Screenshot debug trees"): <shot>.trees.txt, the DEBUG_TREE of every on-screen
// character and its squad. The capture never waits: the answers land in the debug sink, and the frame poll below reads them each frame.

// The screenshot's debug trees: one DEBUG_QUERY per on-screen character and per squad holding one, the answers held by (kind, id), and
// <shot>.trees.txt written once all are in or 5 s have passed. Nothing ever waits on the wire.
struct ShotTrees
{
	struct Item { uint8_t kind = 0; uint64_t id = 0; std::string name; };
	std::string base;		// the shot's path without .png (<shot>.trees.txt and <shot>.txt sit beside it)
	double startedAt = 0;		// steady seconds of the shot
	bool active = false;
	bool subscribed = false;	// we sent DEBUG_SUBSCRIBE on (the squad overlay was off): turned off again at the end
	bool squadsIn = false;		// a DEBUG_SQUADS frame after the shot has arrived
	bool squadPass = false;		// the membership pass over it is done
	uint32_t squadsSerial = 0;	// debugView.squadsSerial at the shot: an older frame is no membership source
	std::vector<Item> wanted;	// characters nearest first, then their squads
	std::map<std::pair<uint8_t, uint64_t>, dbg::Tree> got;
	bool asked(uint8_t kind, uint64_t id) const
	{
		for (const Item& i : wanted) { if (i.kind == kind && i.id == id) { return true; } }
		return false;
	}
};

ShotTrees& trees()
{
	static ShotTrees instance;
	return instance;
}

double steady_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// The squads holding a captured character, asked once DEBUG_SQUADS is in (its member lists are the only membership the wire gives).
void trees_squads(const Context& cx)
{
	ShotTrees& st = trees();
	if (!st.active || st.squadPass) { return; }
	st.squadPass = true;
	std::vector<ShotTrees::Item> add;
	for (const auto& [sid, sq] : cx.debugView.squads)
	{
		bool holds = false;
		for (const uint64_t m : sq.memberIds) { if (st.asked(1, m)) { holds = true; break; } }
		if (!holds) { continue; }
		ShotTrees::Item it;
		it.kind = 0;
		it.id = sid;
		it.name = cx.chars.nameOf(cx.sim.decoder().str(sq.templ));
		add.push_back(std::move(it));
	}
	for (const ShotTrees::Item& it : add) { cx.sim.sendDebug(dbg::DebugView::query(it.kind, it.id)); }
	for (ShotTrees::Item& it : add) { st.wanted.push_back(std::move(it)); }
}

// Writes <shot>.trees.txt: each answer in request order (kind, id, name, tick, the rows indented as `dump debugtree` prints them), then
// the ids that never came. Appends "trees: FILE (N chars, M squads)" to the shot's .txt sidecar (creating it for a scripted shot).
// why: "all" (every answer in), "5 s" (the timeout), "quit" (the run ended first), "off" (no debug view: nothing written).
// A character tree's `state` row is shown as the game's state says it (the debug view's row reads only dead / unconscious / down / up, so a caged
// prisoner read "up"): the Context's treeStateValue keeps CHAR_STATS' cage states over it (kenshi-k16.5.8.2).
void trees_finish(const Context& cx, const char* why)
{
	ShotTrees& st = trees();
	if (!st.active) { return; }
	st.active = false;
	if (st.subscribed && !cx.squads.on) { cx.sim.sendDebug(dbg::DebugView::subscribe(false)); }	// turned on for this shot: back off (the overlay has its own)
	st.subscribed = false;
	if (!strcmp(why, "off")) { return; }
	const std::string trees = st.base + ".trees.txt";
	FILE* f = fopen(trees.c_str(), "w");
	if (!f) { fprintf(stderr, "kclient: shot trees: cannot write %s\n", trees.c_str()); return; }
	int squads = 0;
	std::string missing;
	for (const ShotTrees::Item& it : st.wanted)
	{
		const auto g = st.got.find(std::make_pair(it.kind, it.id));
		if (g == st.got.end())
		{
			char buf[72];
			snprintf(buf, sizeof buf, "%skind %u id %llu", missing.empty() ? "" : ", ", it.kind, (unsigned long long)it.id);
			missing += buf;
			continue;
		}
		const dbg::Tree& t = g->second;
		if (it.kind == 0) { squads++; }
		fprintf(f, "debugtree kind %u id %llu name \"%s\" tick %llu rows %zu\n", t.kind, (unsigned long long)t.id, it.name.c_str(), (unsigned long long)t.tick, t.rows.size());
		for (const dbg::TreeRow& r : t.rows) { fprintf(f, "  %*s%s: %s\n", r.depth * 2, "", r.key.c_str(), cx.treeStateValue(t.kind, t.id, r.key, r.value).c_str()); }
	}
	fprintf(f, "missing: %s\n", missing.empty() ? "none" : missing.c_str());
	fclose(f);
	std::error_code ec;
	const size_t chars = (size_t)std::filesystem::file_size(trees, ec);
	const std::string note = "trees: " + std::filesystem::path(trees).filename().string() + " (" + std::to_string(ec ? 0 : chars) + " chars, " + std::to_string(squads) + " squads)\n";
	if (FILE* side = fopen((st.base + ".txt").c_str(), "a")) { fputs(note.c_str(), side); fclose(side); }
	printf("%s", note.c_str());	// the same line on the console (scripted runs read it there)
}
#endif

}	// namespace

// <shot>.txt, <shot>.events.txt and <shot>.trails.tsv, in that order (the .txt first: the other two append their own lines to it).
void shotSidecars(const Context& cx, const std::string& shotPath)
{
	const std::string base = shotPath.ends_with(".png") ? shotPath.substr(0, shotPath.size() - 4) : shotPath;
	shot_meta_file(cx, base + ".txt");	// the F12 sidecar, owner / primitive / rule lines included (k16.2.234.1)
	shot_events(shotPath);
	shot_trails(cx, shotPath);
}

#ifdef KCLIENT_DEBUG_VIEW
// Starts a capture when a shot is taken (F12 or the script's `screenshot`): one DEBUG_QUERY per on-screen character and per squad holding
// one. On-screen: drawn this frame, not cut by a floor, the body's middle inside the frustum; nearest the camera first, at most 64.
void shotTreesBegin(const Context& cx, const std::string& shotPath)
{
	trees_finish(cx, "new shot");	// a shot while one is pending: keep the older one's answers
	ShotTrees& st = trees();
	st = ShotTrees();
	if (!cx.sim.active()) { return; }	// no live link (a recorded stream, the creator): nothing to ask
	std::string base = shotPath;
	if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".png") == 0) { base.resize(base.size() - 4); }
	st.base = base;
	st.startedAt = steady_seconds();
	st.active = true;
	// The squad membership needs DEBUG_SQUADS: subscribe while the overlay is off (the server sends it within a slow tick); off again at the end.
	if (!cx.debugSubscribed)
	{
		if (!cx.sim.sendDebug(dbg::DebugView::subscribe(true))) { st = ShotTrees(); return; }	// a recorded stream, not a live link: nothing to ask
		st.subscribed = true;
	}
	float mvp[16];
	cx.buildMvp(mvp, 1.0f, 1000.0f);
	std::vector<std::pair<double, uint64_t>> on;
	for (const uint64_t id : cx.sim.characterIds())
	{
		double feet[3];
		float hull[3];
		if (!cx.sim.placed(id, feet, hull) || cx.buildings.cutHides(feet)) { continue; }
		const float p[3] = { (float)(feet[0] - cx.pos[0]), (float)(feet[1] + hull[1] * 0.5f - cx.pos[1]), (float)(feet[2] - cx.pos[2]) };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
		if (c[3] <= 0 || std::fabs(c[0]) > c[3] || std::fabs(c[1]) > c[3]) { continue; }
		on.emplace_back(std::hypot(feet[0] - cx.pos[0], feet[1] - cx.pos[1], feet[2] - cx.pos[2]), id);
	}
	std::sort(on.begin(), on.end());
	std::vector<uint64_t> pick;
	for (size_t i = 0; i < on.size() && i < 64; i++) { pick.push_back(on[i].second); }	// MINE cap; the server's queue takes 160 (these and their squads in one batch)
	for (const uint64_t id : pick)
	{
		const auto ch = cx.sim.decoder().chars.find(id);
		ShotTrees::Item it;
		it.kind = 1;
		it.id = id;
		it.name = ch == cx.sim.decoder().chars.end() ? std::string() : cx.chars.nameOf(cx.sim.decoder().str(ch->second.look.name));
		st.wanted.push_back(std::move(it));
	}
	for (const ShotTrees::Item& it : st.wanted) { cx.sim.sendDebug(dbg::DebugView::query(it.kind, it.id)); }
	st.squadsSerial = cx.debugView.squadsSerial;	// the squads come with the next DEBUG_SQUADS (1 Hz), fresh after this shot
	if (st.wanted.empty()) { st.squadPass = true; }	// nobody on screen: nothing to ask, the file is empty
}

// Every frame while a capture is pending: the squads as soon as DEBUG_SQUADS is in, then the file once every answer is in or 5 s are up.
void shotTreesFrame(const Context& cx)
{
	ShotTrees& st = trees();
	if (!st.active) { return; }
	if (cx.debugView.infoSeen && !cx.debugView.enabled)
	{
		fprintf(stderr, "kclient: shot trees: the server's debug view is off (no --debug-view, or not a loopback peer)\n");
		trees_finish(cx, "off");
		return;
	}
	if (!st.squadsIn && cx.debugView.squadsSerial != st.squadsSerial)
	{
		st.squadsIn = true;	// a DEBUG_SQUADS after the shot: its member lists hold the squads (possibly none), ask them too
		trees_squads(cx);
	}
	bool all = st.squadPass;
	for (const ShotTrees::Item& it : st.wanted) { if (!st.got.count(std::make_pair(it.kind, it.id))) { all = false; break; } }
	if (all) { trees_finish(cx, "all"); }
	else if (steady_seconds() - st.startedAt >= 5.0) { trees_finish(cx, "5 s"); }
}

// One DEBUG_TREE decoded (the debug sink): kept when the capture asked for it.
void shotTreesTree(const Context& cx, const dbg::Tree& t)
{
	(void)cx;
	ShotTrees& st = trees();
	if (st.active && st.asked(t.kind, t.id)) { st.got[std::make_pair(t.kind, t.id)] = t; }
}

void shotTreesFinish(const Context& cx, const char* why) { trees_finish(cx, why); }

// This shot's capture is still running (a --bead run waits for its trees before it copies the shot).
bool shotTreesPending(const std::string& base) { return trees().active && trees().base == base; }

// A sim session opens: no capture is pending.
void shotTreesReset() { trees() = ShotTrees(); }
#else
// No debug view (KCLIENT_DEBUG_VIEW off): there is no tree to ask for.
void shotTreesBegin(const Context&, const std::string&) {}
void shotTreesFrame(const Context&) {}
void shotTreesTree(const Context&, const dbg::Tree&) {}
void shotTreesFinish(const Context&, const char*) {}
bool shotTreesPending(const std::string&) { return false; }
void shotTreesReset() {}
#endif

}	// namespace dev
