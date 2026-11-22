#include <algorithm>
#include "kutil/executable.hpp"
#include "render/lightbake.hpp"
#include "render/drawstats.hpp"
#include "render/framestats.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <future>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <unistd.h>
#include <string>
#include <vector>

#include "render/interiorclip.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_debugtext.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#if defined(SOKOL_GLCORE)
#include "platform/gl.hpp"	// the frame-buffer calls the dev depth readback needs (animlab / idbuffer do the same)
#endif

#include "world/buildings.hpp"
#include "world/foliage.hpp"
#include "world/grass.hpp"
#include "render/gputimer.hpp"
#include "app/cpustage.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "app/threadsplit.hpp"	// the logic / render split on the stages line (kenshi-k16.2.205.12.84)
#endif
#include "dev/packetdigest.hpp"	// KCLIENT_PACKET_DIGEST / KCLIENT_PACKET_POISON (kenshi-k16.2.205.12.93)
#include "knobs/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "assets/sidorder.hpp"
#include "render/fogvolume.hpp"
#include "render/sunshafts.hpp"
#include "render/dust.hpp"
#include "render/blooddecals.hpp"
#include "render/arrows.hpp"
#include "render/particles.hpp"
#include "world/particleworld.hpp"
#include "frame/beamfield.hpp"
#include "characters/physics.hpp"
#include "bridge/ragdolldrive.hpp"
#include "characters/ragdoll.hpp"
#include "render/hdr.hpp"
#include "render/idbuffer.hpp"
#include "render/post.hpp"
#include "render/camera.hpp"
#include "characters/portraits.hpp"
#include "world/doorpose.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/pickmask.hpp"
#include "dev/routepreview.hpp"
#endif
#include "knobs/lightterms.hpp"
#include "net/aitask.hpp"
#include "app/githash.hpp"
#include "frame/loadscreen.hpp"
#include "platform/localserver.hpp"
#include "assets/dds.hpp"
#include "assets/ogremesh.hpp"
#include "world/clientnav.hpp"
#include "kworld/towns.hpp"
#include "render/navoverlay.hpp"
#include "app/config.hpp"
#include "platform/windowgeom.hpp"
#include "platform/process.hpp"	// plat::unsetEnv
#include "platform/windowgate.hpp"
#include "render/readback.hpp"
#include "render/renderqueue.hpp"
#include "input/inputqueue.hpp"
#include "app/sappcmd.hpp"
#include "app/logicthread.hpp"
#include "frame/lastframe.hpp"
#include "frame/renderframe.hpp"
#include "app/opencont.hpp"
#include "render/weff.hpp"
#include "net/progressfeed.hpp"
#include "ui/ui.hpp"
#include "ui/gfx.hpp"
#include "ui/hud.hpp"
#include "ui/options.hpp"
#include "ui/gear.hpp"
#include "ui/itemwindows.hpp"
#include "ui/stats.hpp"
#include "ui/keys.hpp"
#include "ui/overview.hpp"
#include "ui/squads.hpp"
#include "ui/research.hpp"
#include "ui/crafting.hpp"
#include "ui/faction.hpp"
#include "ui/progressbars.hpp"
#include "ui/namelabels.hpp"
#include "ui/prompt.hpp"
#include "ui/icons.hpp"
#include "ui/creator.hpp"
#include "ui/menus.hpp"
#include "ui/savepanel.hpp"
#include "ui/newgame.hpp"
#include "ui/advanced.hpp"
#include "ui/tutorials.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "ui/dev/lightspanel.hpp"
#endif
#include "ui/layout.hpp"
#include "ui/prospecting.hpp"
#include "ui/build.hpp"
#include "ui/style.hpp"
#include "app/serverrows.hpp"
#include "ui/trade.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/bubbles.hpp"
#include "ui/dialogue.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/coords.hpp"
#include "assets/materials.hpp"
#include "assets/mods.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "kutil/crashlog.hpp"
#include "app/eventlog.hpp"
#include "world/squads.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#include "ui/dev/debugtree.hpp"
#include "ksim/debugview/enums.hpp"
#endif
#include "characters/characters.hpp"
#include "characters/creator.hpp"
#include "characters/creatorscene.hpp"
#include "kworld/gamedata.hpp"
#include "render/terrain.hpp"
#include "render/water.hpp"
#include "render/reflection.hpp"
#include "render/speccube.hpp"
#include "render/weathermap.hpp"
#include "world/weatherinstances.hpp"
#include "audio/audiohook.hpp"
#include "audio/events.hpp"
#include "bridge/simview.hpp"
#include "frame/trails.hpp"
#include "input/collisionpick.hpp"
#include "input/controlhints.hpp"	// the control-hints types and the resolver stub (kenshi-k16.2.294.2)
#include "input/pick.hpp"	// Pick and the target predicates the hover / click / menu read (kenshi-k16.2.294.2)
#include "input/cursors.hpp"
#include "render/drawlist_gpu.hpp"
#include "render/overlaylist.hpp"
#include "world/worldbuildings.hpp"
#include "world/worldmap.hpp"

#include "host/app.hpp"
#include "host/uihost.hpp"
#include "host/pick.hpp"
#include "host/orders.hpp"
#include "host/hud.hpp"
#include "host/script.hpp"
#include "host/logicframe.hpp"
#include "host/events.hpp"

namespace host
{

#ifdef KCLIENT_DEV_TOOLS
// debug.route_preview (dev/routepreview.cpp): the one selected character as a click would plan for it. Its race is on the wire (the appearance's sid), its
// routing body comes from the client's RACE records exactly as ksim's CombatData::body derives it (radius, water cost scale; no water scale for an unknown race).
static bool route_subject(dev::routepreview::Subject& s)
{
	const std::vector<uint64_t> sel = app.sim.selected();
	if (sel.size() != 1) { return false; }
	double yaw = 0;
	const simlink::Route* rt = nullptr;
	if (!app.sim.pose(sel[0], s.from, yaw, rt)) { return false; }
	const auto ch = app.sim.decoder().chars.find(sel[0]);
	if (ch == app.sim.decoder().chars.end()) { return false; }
	s.id = sel[0];
	float water = 1.0f;
	s.water.reset();
	if (app.chars.pathBody(app.chars.nameOf(app.sim.decoder().str(ch->second.look.race)), s.radius, water)) { s.water = water; }
	return true;
}
void route_preview_frame()
{
	if (!app.opt.debug) { return; }
	dev::routepreview::Subject s;
	const bool one = dev::routepreview::on() && route_subject(s);
	dev::routepreview::update(app.nav, one ? &s : nullptr, app.hoverHitOk ? app.hoverHit : nullptr, app.sim.tick());
}
void route_preview_order(const double goal[3], uint32_t seq)
{
	dev::routepreview::Subject s;
	if (app.opt.debug && dev::routepreview::on() && route_subject(s)) { dev::routepreview::onOrder(app.nav, s, goal, seq, app.sim.tick()); }
}
void route_preview_arrived(uint32_t seq)
{
	if (!app.opt.debug || !dev::routepreview::on()) { return; }
	for (const uint64_t id : app.sim.selected())
	{
		double now[3], yaw = 0;
		const simlink::Route* rt = nullptr;
		if (app.sim.pose(id, now, yaw, rt) && rt && rt->clientSeq == seq && !rt->points.empty()) { dev::routepreview::onServerRoute(app.nav, seq, rt->points, rt->flags, now, app.sim.tick()); return; }
	}
}
#else
void route_preview_frame() {}
void route_preview_order(const double[3], uint32_t) {}
void route_preview_arrived(uint32_t) {}
#endif

// Each door's pose for this frame, shared by the drawn leaf (WorldBuildings::setDoors) and the pick (pick_at): the wire's state sets the target
// (open / broken: 1, closed / locked: 0) and the openness slides to it at `door move speed` per second (the Door_Shut cue's 1 / speed travel; the
// original's own motion curve is not known: UNSURE, linear). A door seen for the first time eases to its target from the authored pose when it was
// already drawn at that pose (the town approach: the gate no longer teleports open), and stands at it when it is first drawn with its state known
// (kenshi-k16.2.258.10.1). The offset is doorpose.hpp's.
void update_doors(double dt)
{
	const auto shapes = app.collide.gathered();
	if (!shapes) { return; }
	if (shapes != app.doorShapes)
	{
		app.doorShapes = shapes;
		app.doorFirst.clear();
		for (size_t i = 0; i < shapes->size(); i++) { if ((*shapes)[i].use == kworld::NavUse::Door && (*shapes)[i].door) { app.doorFirst.emplace((*shapes)[i].door, i); } }
	}
	std::unordered_map<uint64_t, std::array<double, 3>> draw;
	for (const auto& [key, idx] : app.doorFirst)
	{
		const kworld::WorldShape& s = (*shapes)[idx];
		App::DoorPoseNow& pose = app.doorPose[key];
		if (!pose.thing || !app.sim.thing(pose.thing))	// the door interactable nearest the leaf's closed-pose centre (kDoorMatch)
		{
			pose.thing = 0;
			const double c[3] = { (s.min[0] + s.max[0]) / 2.0, (s.min[1] + s.max[1]) / 2.0, (s.min[2] + s.max[2]) / 2.0 };
			double near = kDoorMatch * kDoorMatch;
			for (const auto& [id, th] : app.sim.things())
			{
				const double d2 = std::pow(th.pos[0] - c[0], 2) + std::pow(th.pos[1] - c[1], 2) + std::pow(th.pos[2] - c[2], 2);
				if (th.kind == simlink::Interactable::kDoor && d2 < near) { near = d2; pose.thing = id; }
			}
		}
		// The leaf has now been drawn at least a frame (this pass draws every door of the gathered world each frame, the ones outside the view
		// included: UNSURE, a leaf behind the camera counts as drawn too). A door drawn before its state was known eases into that state from its
		// authored pose, like a later change (kenshi-k16.2.258.10.1); one first drawn with its state already known stands at it.
		const bool wasDrawn = pose.drawn;
		pose.drawn = true;
		const simlink::Interactable* th = pose.thing ? app.sim.thing(pose.thing) : nullptr;
#ifdef KCLIENT_DEV_TOOLS
		// kenshi-k16.2.241.2 (dev only, --debug runs): the run line for a leaf's match — an unmatched leaf draws at its authored pose
		if (!th && app.opt.debug)
		{
			static std::unordered_map<uint64_t, bool> loggedNone;
			if (!loggedNone[key]) { loggedNone[key] = true; printf("doorpose %llu: no kDoor within %.0f (drawn at its authored pose)\n", (unsigned long long)key, kDoorMatch); }
		}
#endif
		if (!th) { continue; }
		const auto slide = app.doorSlides.find(app.sim.sidOf(th->record));
		const DoorSlide d = slide == app.doorSlides.end() ? DoorSlide{} : slide->second;
		const double target = th->state == simlink::Interactable::kOpen || th->state == simlink::Interactable::kBroken ? 1.0 : 0.0;
		if (!pose.seen) { pose.open = wasDrawn ? 0.0 : target; pose.seen = true; }	// 0: the authored (closed) pose it was drawn at
		if (pose.open < target) { pose.open = std::min(target, pose.open + dt * d.speed); }
		else if (pose.open > target) { pose.open = std::max(target, pose.open - dt * d.speed); }
		pose.exact = doorOffset(s, d, pose.open, pose.off);
#ifdef KCLIENT_DEV_TOOLS
		// kenshi-k16.2.241.2 (dev only, --debug runs): which interactable each drawn leaf follows and its pose, on a change
		if (app.opt.debug)
		{
			struct LeafLog { uint64_t thing = 0; int state = -1; double open = -1; bool exact = false; };
			static std::unordered_map<uint64_t, LeafLog> logged;
			const LeafLog was = logged[key];
			if (was.thing != pose.thing || was.state != (int)th->state || was.exact != pose.exact || std::fabs(was.open - pose.open) > 0.05)
			{
				logged[key] = LeafLog{ pose.thing, (int)th->state, pose.open, pose.exact };
				printf("doorpose %llu thing %llu state %d open %.2f off %.1f %.1f %.1f%s\n", (unsigned long long)key, (unsigned long long)pose.thing, (int)th->state, pose.open,
					pose.off[0], pose.off[1], pose.off[2], pose.exact ? "" : " (no box)");
			}
		}
#endif
		if (pose.open > 0) { draw[key] = { pose.off[0], pose.off[1], pose.off[2] }; }
	}
	app.buildings.setDoors(draw);
}

// A scripted run's wait for a worker's result (the foliage's and the grass's builds): polls every 10 ms, and gives up when the logic thread is told to
// stop, so a quit while a build never finishes still joins (overlap review .85 section 3 C5, kenshi-k16.2.205.12.95). Off: stopping() is never set.
template <class Ready> static void wait_ready(Ready ready)
{
	while (!ready() && !logicthread::stopping()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
}

static void logic_paths(FramePacket& pk)
{
	// Overlap (rule 12): only the plain world frame overlaps the draw (its first half). The session load, the title, a session that waits for its
	// camera, the creator, the open animation lab and the mesh viewer run gated from their frame's start, as lockstep does (review .85 section 1.6): nobody needs
	// their frame rate, and they write renderer state (the load's sets, setEditor, the lab's target) all through. Last frame's state decides; the
	// frames that switch into them gate at the switch (begin_session_load, sim_opened, the creator's update_ui line, animationsCommand).
	if (dev::viewing() || app.sessionLoad || title_shown() || !app.worldTarget || (app.creatorUi && app.creatorUi->shown()) || dev::labOpen()) { logicthread::renderIdle(); }
	view::take();	// the window's size and frame time as frame() published them; every logic read below goes through view:: (render hook 2, kenshi-k16.2.205.12.7)
	ui::setViewport(view::width(), view::height());	// the UI's size, before the drained handlers use it (render hook 2's ui/ half, kenshi-k16.3.29)
	gDraining = true;	// first: the events sokol delivered before this frame, in order (inputqueue.hpp)
	gInput.drain([](const sapp_event& e) { event(&e); });
	gDraining = false;
	windowgate::take();	// the gate's state as frame() published it (it polled the window there, kenshi-k16.2.205.12.23): read after the drain, as before
	pk.width = windowgate::width();	// the applied size the packet is built for: the draw sizes its targets from it, never from the live window (RENDER_THREAD.md 7.1 rule 10)
	pk.height = windowgate::height();
	if (windowgate::appliedThisFrame()) { app.loadScreen.relayout(); }	// the load's text re-bakes at the applied height (a no-op when no load screen is up)
	if (const int cap = (logicthread::running() && logicthread::mode() == logicthread::Mode::kOverlap) ? 0 : kFpsCap.get())	// the frame limiter (overlap: the render thread's, frame_lockstep, rule 11): sleep to the next slot; more than a frame late restarts the schedule
	{
		static std::chrono::steady_clock::time_point next;
		const auto now = std::chrono::steady_clock::now();
		const auto step = std::chrono::nanoseconds(1000000000 / cap);
		if (now < next) { std::this_thread::sleep_until(next); next += step; }
		else { next = now + step; }
	}
	// The frame gap (kenshi-k16.2.26): measured after the limiter's sleep, so only real work counts. A gap over kStallMs is an evlog `stall` with the
	// phase that blocked (the load step's own name, the session load, or the world frame); the maxima feed the startup / session-load reports.
	{
		const auto frameNow = std::chrono::steady_clock::now();
		if (app.frameStart.time_since_epoch().count() != 0)
		{
			const double gapMs = std::chrono::duration<double, std::milli>(frameNow - app.frameStart).count();
			app.frameGapMax = std::max(app.frameGapMax, gapMs);
			if (app.startupFrames) { app.startupFrameMax = std::max(app.startupFrameMax, gapMs); }
			else if (app.sessionLoad) { app.sessionFrameMax = std::max(app.sessionFrameMax, gapMs); }
			if (gapMs > kStallMs)
			{
				const double tick = app.sim.tick();
				evlog::add("stall", tick, 0, "frame %.0f ms in %s", gapMs, app.framePhase);
				fprintf(stderr, "kclient: stall: frame %.0f ms in %s\n", gapMs, app.framePhase);
			}
		}
		app.frameStart = frameNow;
		app.framePhase = dev::viewing() ? "viewer" : app.loading() ? app.loadSteps[app.loadIdx].first : app.sessionLoad ? "session load" : "world frame";
	}
	if (dev::viewFrame(dev_context(), pk)) { return; }	// the mesh viewer's own frame (dev/viewmesh.cpp)
	if (app.loading()) { frame_loading(pk); return; }
	if (app.startupFrames)	// the first world frame: the last load step's gap was measured at this frame's entry, so it is in the maximum
	{
		app.startupFrames = false;
		fprintf(stderr, "kclient: startup: worst frame %.0f ms over the loading screen\n", app.startupFrameMax);
	}
	if (!app.sessionLoad) { app.loadScreen.shutdown(); }	// no-op once freed
	CPUSTAGE_BEGIN();
	const auto cpu0 = std::chrono::steady_clock::now();
	if (app.sim.active()) { app.sim.pumpLink(); }	// the HELLO answer goes out before anything below can block (the futures' .get() in a scripted run; kenshi-k16.2.268)
	if (!app.townsReady && app.townFut.valid() && (app.scripted() || app.townFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
	{
		app.towns = app.townFut.get();
		app.townsReady = true;
		printf("towns: %zu usable, loaded in %.0f ms; %s\n", app.towns.towns.size(), app.towns.loadMs, app.towns.report.c_str());
	}
	if (!app.bldReady && app.bldFut.valid() && (app.scripted() || app.bldFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
	{
		BuildingSet set = app.bldFut.get();
		app.bldReady = true;
		printf("buildings: loaded in %.0f ms; %s\n", set.loadMs, set.report.c_str());
		std::vector<std::pair<size_t, int>> dense;
		for (const auto& [cell, list] : set.cells) { dense.push_back({ list.size(), cell }); }
		std::sort(dense.rbegin(), dense.rend());
		for (size_t i = 0; i < 8 && i < dense.size(); i++)
		{
			const double cx = kworld::zoneMinX(dense[i].second % 64) + 2304, cz = kworld::zoneMinZ(dense[i].second / 64) + 2304;
			const int n = app.towns.nearest(cx, cz);
			printf("  dense cell %d.%d centre %.0f,%.0f: %zu instances, nearest town %s\n", dense[i].second % 64, dense[i].second / 64, cx, cz, dense[i].first, n >= 0 ? app.towns.towns[n].name.c_str() : "?");
		}
		logicthread::renderIdle();	// overlap (rule 12): every buildings draw reads data_
		app.buildings.setData(std::move(set));
		app.bldSet = true;
	}
	if (!app.skyReady && app.skyFut.valid() && (app.scripted() || app.skyFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
	{
		const SkyConstants k = app.skyFut.get();
		app.skyReady = true;
		printf("sky: %s\n", k.source.c_str());
		app.sky.setConstants(k);
	}
	if (app.weatherFut.valid() && (app.scripted() || app.weatherFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) { app.sky.weather = app.weatherFut.get(); }
	if (app.weathersFut.valid() && (app.scripted() || app.weathersFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) { take_weathers(); }
	kutil::crashlog_tick(app.frames);
	run_script(false);
	dev::sceneTestPre(dev_context());	// a --scene-test run: apply the next scene (dev/scenetest.cpp)
	CPUSTAGE("futures+script");
	// A scripted shot steps a fixed 1/60 s a frame (repeatable); `set framedt real` steps the frame's real duration instead, so a scripted run
	// measures motion as a live client sees it (the sim runs in real time, k16.2.181.1).
	const double dt = app.scripted() && !app.scriptRealDt ? kScriptDt : view::frameDuration();
	// Ragdolls on the sim's clock (PHYSICS.md 4): still while paused, a fixed 1/60 s step each, more of them with the speed (capped at
	// kRagdollMaxSteps). The pull kicks ahead of every substep, so it follows the body one step at a time at any game speed.
	const double dtPhys = app.sim.active() ? app.sim.dtSim() : dt;
	app.physics.stepFixed(dtPhys, kRagdollMaxSteps, [&](double h) { app.ragdolls.substep(app.sim, app.hm ? &*app.hm : nullptr, h); });
	// The colliders run once the world has a body, or a ragdoll waits for its floor (the first ragdoll on a building floor).
	if ((app.physics.started() || !app.ragdolls.waitingAt().empty()) && app.hm) { app.statics.update(app.collide.gathered(), *app.hm, app.physics.dynamicBodies(), app.ragdolls.waitingAt(), 80); }
	poll_local_server();
	CPUSTAGE("physics+poll");
	if (title_shown()) { frame_title(pk, dt); return; }
	// A live session with no camera target yet streams and draws nothing (k16.2.112.1): black or the session loading screen, never the world.
	if (!app.worldTarget) { frame_await_target(pk, dt); return; }
	// The creator is its own mode (kenshi-k16.2.472.3): its scene is the editor platform and the preview character alone. The world is kept loaded and its
	// link read, but nothing of it is picked, streamed (foliage, grass, props, particles) or drawn (the id pass, the world's characters) while it shows.
	const bool editor = app_mode() == Mode::kCreator;
	if (!app.firstWorldFrame) { app.firstWorldFrame = true; printf("first_world_frame: x=%.0f z=%.0f reason=%s\n", app.pos[0], app.pos[2], app.targetReason); screenshot_world_frame(); }
	// An offline flight (play.sh fly: no sim, no session) gets the same ready-set gate as a session: the load screen stands over the first world frames until
	// the load ring is created (session.loading_screen: 1 live runs only, 2 scripted too; kenshi-k16.2.258.3)
	if (!app.offlineGateDone && !app.sim.active() && !dev::viewing() && app.hm && app.buildings.hasData()) { app.offlineGateDone = true; begin_session_load(); }
	camera_restore_frame();	// a load's camera sidecar, at its first snapshot (k16.2.134.1)
	if (app.follow) { update_follow(dt); }
	else if (app.rts) { update_rts(dt); }
	else { update_camera(dt); }
	if (static const bool camTrace = std::getenv("KCLIENT_CAMTRACE") != nullptr; camTrace && app.scripted())	// kenshi-k16.2.474.1: per frame, the view's step check input (tools/camera_settle_check.sh)
	{
		const uint64_t sub = app.follow ? app.followId : app.centerId;
		double dp[3] = {}, sp[3] = {}; float hull[3];
		const bool have = sub && app.sim.placed(sub, dp, hull);
		if (have) { app.sim.simPos(sub, sp); }
		printf("camtrace: frame %llu mode %s id %llu eye %.3f %.3f %.3f drawn %.3f %.3f sim %.3f %.3f slack %.3f\n", (unsigned long long)app.frames, app.follow ? "follow" : app.rts ? "rts" : "free", (unsigned long long)sub, app.pos[0], app.pos[1], app.pos[2], dp[0], dp[2], sp[0], sp[2], std::hypot(app.slackResid[0], app.slackResid[1]));
	}
	app.sim.setDriven(app.follow && app.sim.isPlayer(app.followId) ? app.followId : 0);	// direct drive: the drawn body's travel facing (kenshi-k16.2.237.10)
	CPUSTAGE("camera");
	if (app.sim.active())
	{
		app.sim.hold(app.creator.active());
		app.sim.update(dt, app.chars, [](double x, double z) { return app.hm->terrainHeight(x, z); });	// the drawn terrain triangles, not the bilinear height() (kenshi-k16.2.382.1: they differ by metres on a slope; the stand ray and yValid-clear fallback stand on the drawn ground)
		CPUSTAGE("sim.update");
		app.nav.update(app.sim.decoder(), app.sim.takeNavPatches());
		app.sim.setWalker(app.nav.walker());
		if (!app.creatorUi->shown()) { live_squads(); live_profile(); }
		CPUSTAGE("sim.nav+squads");
		for (const SimView::Splash& s : app.sim.takeSplashes())
		{
			static const char* const names[] = { "splash", "drip", "pool", "smear" };
			bool fresh = true;
			if (s.kind == SimView::kPool) { fresh = app.blood.pool(s.id, s.x, s.z, s.size, s.colour); }
			else { app.blood.add(s.kind == SimView::kHit ? BloodDecals::kSplash : s.kind == SimView::kDrop ? BloodDecals::kPool : BloodDecals::kSmear, s.x, s.z, s.size, s.dirRad, s.colour); }
			if (app.scripted()) { printf("blood: %s%s %.0f %.0f size %.4f\n", names[s.kind], fresh ? "" : " (grown)", s.x, s.z, s.size); }
		}
		app.orderAge += dt;
		if (app.orderSeq && !app.orderRouted)	// the marker's target once the sim answers: the route's end, and how long the click point stood (k16.2.259)
		{
			double end[3];
			if (order_route_end(end))
			{
				app.orderRouted = true;
				route_preview_arrived(app.orderSeq);
				uint64_t clickTick = 0, arrived = 0, waypoints = 0;
				uint8_t flags = 0;
				const std::vector<uint64_t> sel = app.sim.selected();
				for (const uint64_t id : sel)
				{
					for (const SimView::OrderRow& o : app.sim.orders(id)) { if (o.move && o.seq == app.orderSeq) { clickTick = o.tick; } }
					if (const simlink::Route* rt = app.sim.routeOf(id); rt && rt->clientSeq == app.orderSeq) { arrived = rt->arrivedTick; flags = rt->flags; waypoints = rt->points.size(); }
				}
				printf("move marker: seq %u click %.1f %.1f %.1f -> route end %.1f %.1f %.1f (%zu waypoints%s), arrived %lld ticks after the order (tick %llu)\n",
					app.orderSeq, app.orderAt[0], app.orderAt[1], app.orderAt[2], end[0], end[1], end[2], (size_t)waypoints,
					flags & 2 ? ", approach point" : "", clickTick && arrived > clickTick ? (long long)(arrived - clickTick) : -1LL, (unsigned long long)arrived);
			}
		}
		if (app.sim.takeRejected()) { app.orderAge = 0; app.orderRejected = true; }
		if (app.simCamera) { app.sim.view(app.pos[0], app.pos[2]); }	// only once the camera has gone to the sim's characters
		if (app.centerOnStart && app.rts && !app.sim.selected().empty())
		{
			// A new game / scenario start's centring is an engage: it focuses the first selected character and the floor view snaps to its floor like a
			// double click (user 2026-11-15; kenshi-k16.2.236.5). One that already engaged (a script's or player's click first) is never demoted by it.
			if (!camera_subject()) { app.centerId = app.sim.selected()[0]; app.buildings.engage(); }
			app.centerOnStart = false;
		}
		if (!editor)	// game mode only: the creator owns the pointer and picks nothing of the world (kenshi-k16.2.472.3)
		{
			app.collide.update(app.pos[0], app.pos[2]);
			CPUSTAGE("pk.pre");	// sub-stages of sim.peek+hover (kenshi-k16.2.205.12.109): the splashes .. collide update, the peek, the hover pick
			update_peek(dt);
			CPUSTAGE("pk.peek");
			hover_cursor();
			CPUSTAGE("pk.hover");
#ifdef KCLIENT_DEV_TOOLS
			pick_mask_update();
#endif
			route_preview_frame();
		}
		else { app.hoverThing = 0; app.hoverHitOk = false; app.cursors.set("arrow"); }
		place_session_camera();
		CPUSTAGE("sim.peek+hover");
	}
	update_ui(dt);
	CPUSTAGE("update_ui");
	const double charDt = app.scripted() && !app.scriptAnim ? 0.0 : app.sim.active() ? app.sim.dtAnim() : dt;	// on the sim time drawn (kenshi-k16.2.479.1; was dtSim: wall dt x speed, which ran the legs through a stall): still while paused (user 2026-11-05), faster at 2x / 3x
	if (app.sim.active() && !editor) { app.ragdolls.update(app.sim, app.chars, app.hm ? &*app.hm : nullptr, app.pos); }
	app.chars.update(charDt, dt);	// dt: the loading stand-in's clocks run on the frame's own time, even where the poses are held (kenshi-k16.2.258.1)
	CPUSTAGE("chars.update");
	// the animation notifies' sounds (kenshi-h2v.29.6): each fired notify posts on its character's world object (the sim id), with the switches the game sets
	// (AnimationSFXEvent::vf1 1400649c0): Footsteps Armor_Worn + 200 u gate, the record's own override, Attack the held weapon's Weapons value; 1000 u otherwise
	for (const Characters::SoundNote& n : app.chars.takeSounds())
	{
		const uint64_t id = app.sim.idOfChar((int)n.index);
		if (!id) { continue; }
		const uint32_t object = (uint32_t)id;
		audiohook_object(object, app.chars.live(n.index).pos);
		if (!n.group.empty()) { audiohook_object_switch(object, n.group, n.value); }
		else if (n.event == "Footsteps") { audiohook_object_switch(object, "Armor_Worn", app.chars.armourSwitch(n.index)); }
		else if (n.event == "Attack") { if (const std::string w = app.chars.weaponSwitch(n.index); !w.empty()) { audiohook_object_switch(object, "Weapons", w); } }
		audiohook_object_post(object, n.event, n.gate);
		if (app.opt.debug && audiohook_trace()) { fprintf(stderr, "notify: char %llu %s%s%s%s%s%s at %.2f s\n", (unsigned long long)id, n.event.c_str(), n.group.empty() ? "" : " ", n.group.c_str(), n.value.empty() ? "" : ("=" + n.value).c_str(), n.action.empty() ? "" : " from ", n.action.c_str(), app.animTime); }
	}
	// the combat sounds (kenshi-h2v.29.7): the posts receiveHit and the ranged launcher make, from SIMLINK events 1 / 5 / 49 (SimView::cueSounds)
	for (const SimView::SoundCue& c : app.sim.takeSoundCues())
	{
		const uint32_t object = (uint32_t)c.object;
		audiohook_object(object, c.pos);
		for (const auto& [group, value] : c.switches) { audiohook_object_switch(object, group, value); }
		audiohook_object_post(object, c.event, c.gate);
		if (c.temporary) { audiohook_object_remove(object); }
		if (app.opt.debug)
		{
			std::string sw;
			for (const auto& [group, value] : c.switches) { sw += " " + group + "=" + value; }
			fprintf(stderr, "combat sound: tick %llu object %u %s%s at %.2f s\n", (unsigned long long)c.tick, object, c.event.c_str(), sw.c_str(), app.animTime);
		}
	}
	// the debug trails, after the poses (the sole points). dt: the sim's own frame (kenshi-k16.2.73): charDt is 0 in a scripted run (frozen poses
	// for deterministic shots) while the drawn positions still follow the wire each frame, and the trails must record those frames too.
	if (app.opt.debug && app.trails.on()) { app.trails.record(app.sim, app.chars, app.sim.active() ? app.sim.dtSim() : dt); }
	dev::labUpdate(dt);	// the animation lab's own clock (a dev tool: real time, not the sim's dtSim)
	dev::footFrame(dev_context(), charDt);
	dev::heightProbeFrame(dev_context());	// the vertical height probe's rows (dev/heightprobe.cpp)
	dev::gaitFrame(app.sim, app.chars, app.pos, charDt);
	dev::posFrame(app.sim, app.chars, app.pos, app.sim.active() ? app.sim.dtSim() : 0.0);
	CPUSTAGE("sounds+trails");
	// The frame's second half starts here (overlap review .85 section 1.1, part B): the writes to what the draw reads come after this line.
	// Overlap, full (kenshi-k16.2.205.12.118): no gate. Everything below writes only logic's own state or what the packet carries; the render
	// thread may be drawing (or redrawing) the previous packet meanwhile (RENDER_THREAD.md 7.1 rule 12). `stall MS gate` / `stall MS b` both sleep here.
	stall_point(0);
	stall_point(1);
	if (app.shadowFlip) { app.shadow.enabled = !app.shadow.enabled; app.shadowFlip = false; }	// F3 (debug_toggle)
	app.animTime += app.sky.clock.paused ? 0.0 : dt;	// MINE: paused freezes the world's motion (user 2026-11-09); not scaled by the game speed
	if (app.atlasFut.valid() && (app.scripted() || app.atlasFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)) { app.wx.setAtlas(app.atlasFut.get(), app.opt.frontBlur); }
	if (app.wx.ready() && app.sim.active() && app.sim.decoder().weatherSerial != app.weatherSeen)
	{
		app.weatherSeen = app.sim.decoder().weatherSerial;
		app.wx.setLive(app.sim.decoder().weather, app.sim.decoder());
	}
	app.sky.regionalFog = app.weatherAuto && app.wx.ready();
	app.sky.ambientBlurKm = app.sky.regionalFog ? app.opt.weatherBlur : 0.0f;
	if (app.sky.regionalFog)
	{
		app.sky.weather = app.wx.blend(app.pos, app.opt.weatherBlur, app.weatherMix);
		sky_ramp(dt);
	} else {
		app.skyRamp.valid = false;	// a pinned weather is shown as is; back on auto the sky snaps to the region's record
	}
	if (app.cloudsForce >= 0 && app.sky.weather.cloudsDensity != app.cloudsForce) { app.sky.weather.cloudsDensity = app.cloudsForce; app.sky.weather.finalize(); }
	// The creator's scene is clear and dry while it is shown (k16.17.6.1): the original's editor builds its own scene far below the world; ours showed the
	// region's weather over it (the user's rain and glistening skin). Client-side display only: the world's weather is not touched, and it is logged again
	// on the first world frame after the creator closes (CONFIRM / cancel), next to the value that was held under the creator.
	if (!editor && app.creatorWeatherHeld)
	{
		if (!app.sky.regionalFog) { app.sky.weather = app.creatorWeatherSave; }	// a pinned weather is not recomputed per frame: put it back (the regional blend above already has)
		app.creatorWeatherHeld = false;
	}
	if (app.creatorWeatherAfter && !editor)
	{
		printf("creator: world weather after: %s rain %.2f wetness %.2f effects %zu (held under the creator: %s rain %.2f wetness %.2f effects %zu)\n",
			app.sky.weather.name.c_str(), app.sky.weather.rain, app.sky.weather.wetness, app.sky.weather.effects.size(),
			app.creatorWeatherSave.name.c_str(), app.creatorWeatherSave.rain, app.creatorWeatherSave.wetness, app.creatorWeatherSave.effects.size());
		app.creatorWeatherAfter = false;
	}
	if (editor)
	{
		// Track the world's own weather each shown frame (the regional blend keeps easing while the creator is up, e.g. as the sim's live WEATHER arrives),
		// so the close log compares against what the world had on the frame before it closed.
		app.creatorWeatherSave = app.sky.weather;
		app.creatorWeatherHeld = true;
		app.sky.weather = SkyWeather();	// the fixed default look: no rain, ash, dust or wetness, no weather effects (no lightning either: type 7 is a world instance)
	}
	// --simspeed (kenshi-k16.2.205.7): ask the sim for the requested game speed until its clock reads it back (an order sent during the join's
	// first frames can be dropped); once every ~30 frames, so a speed the sim refuses does not spam the wire. A measurement flag, dev builds only.
	if (app.opt.simspeed > 0 && app.sim.active() && app.sim.decoder().clock.valid)
	{
		static int retry = 0;
		const double want = app.opt.simspeed, have = app.sim.decoder().clock.speed;
		if (std::abs(have - want) > 1e-3 && (retry++ % 30) == 0) { app.sim.setSpeed((float)want); }
	}
	if (app.sim.active() && app.sim.decoder().clock.valid)
	{
		const auto& c = app.sim.decoder().clock;
		// pause and speed every frame: a pause arrives without a new tick (a paused sim does not tick), so a tick-gated copy never saw it
		app.sky.clock.speed = c.speed;
		app.sky.clock.paused = c.paused;
		if (c.tick != app.clockTick)
		{
			app.clockTick = c.tick;
			app.sky.clock.setHours(c.hour);
			app.sky.clock.day = std::max(0, (int)c.day - 1);	// CLOCK day is the game's "Day N" (ksim/core/clock.hpp); the sky clock counts from 0 like --day
		}
	}
	app.sky.skyAmbient = Post::skyAmbient();
	app.sky.moonlight = Post::moonlight();
	// logic thread: the only writer of hasGround / groundAlbedo (sky.hpp); the render thread reads the packet's copy (pk.scene.groundOk / ground)
	app.sky.hasGround = app.terrain.hasColour && app.terrain.colour.albedoAt(app.pos[0], app.pos[2], app.sky.groundAlbedo);
	app.sky.update(dev::sceneTesting() ? 0.0 : dt, app.opt.fog, app.pos);	// scene shots: clock and cloud drift held, else they move by the load's frame count
	{
		double lf[3], lr[3], lu[3];
		camera_basis(lf, lr, lu);
		audiohook_update(app.pos, lf, lr, app.hm->height(app.pos[0], app.pos[2]), app.sky.weather, app.sky.clock.hours);
	}
	app.frameMs = app.frameMs == 0 ? view::frameDuration() * 1000 : app.frameMs * 0.9 + view::frameDuration() * 100;
	app.fps = app.fps == 0 ? 1.0 / view::frameDuration() : app.fps * 0.95 + 0.05 / view::frameDuration();
	const double ground = app.hm->height(app.pos[0], app.pos[2]);
	const float nearZ = (float)camera_near();	// the one near plane: the click ray starts there too (kenshi-k16.2.259)
	const Camera cam = current_camera(editor ? kCreatorNear : nearZ, editor ? kCreatorFar : 700000.0f);
	app.shadow.soft = (float)kShadowSoft.get();
	app.shadow.cache = kShadowCache.get();
	app.shadow.staticBudget = kShadowStaticBudget.get();
	// the creator has no shadows: strength 0 (sunY below the fade). render.sun_smooth moves the map's light direction by the sky's own dt (held in a --scene-test)
	app.shadow.update(app.pos, cam, app.sky.light().v[0], editor ? -1.0 : app.sky.sunDir()[1], dev::sceneTesting() ? 0.0 : dt, app.sky.clock.paused);
	CPUSTAGE("sky+weather+audio+shadow.update");
	float mvp[16];
	cam.mvp(mvp);
	pk.buildings.draws.clear();
	pk.buildings.inst.clear();	// every field every frame (§7.1 rule 2): buildings.update below refills it when it runs
	pk.buildings.shadowSig = 0;
	pk.buildings.volumes.clear();
	if (app.bldSet && !app.wmap.full)
	{
		// The props list (kenshi-k16.2.205.11): dirty-tracked. The sim sends an interactable on first sight and when its version changes
		// (SimLinkDecoder::thingsSerial), so the list changes on that, on the player buildings' records, on a floor cut (cut_hidden) and on the debug
		// script's build progress only; every other frame reuses the list and skips WorldBuildings::setProps. propsLive: an under-construction player
		// building's progress moves every tick, so the list is rebuilt then.
		const uint32_t thingsSerial = app.sim.thingsSerial();
		const uint32_t buildSerial = app.sim.decoder().buildingsSerial;	// the player buildings' records (BUILDING 47)
		const uint64_t cutSig = app.buildings.cutSig();
		double thingYSig = 0;	// the drawn y of the items and containers (SimView::thingY): a building mesh that loads moves it without a new thing version (k16.2.412.4)
		for (const auto& [id, th] : app.sim.things()) { if (th.kind == simlink::Interactable::kItem || th.kind == simlink::Interactable::kContainer) { thingYSig += app.sim.thingY(id, th); } }
		const bool dirty = !app.propsValid || thingYSig != app.propsThingY || thingsSerial != app.propsSerial || buildSerial != app.propsBuildSerial || cutSig != app.propsCut
			|| app.propsLive || app.scriptBuild >= 0;
		if (dirty && !ablate::on("noprops"))	// KCLIENT_ABL: noprops keeps the list empty (an upper-bound measurement)
		{
			std::vector<WorldBuildings::Prop>& props = app.props;
			props.clear();
			for (const auto& [id, th] : app.sim.things())
			{
				// an item or container is drawn at the ring's y (SimView::thingY: lifted out of the terrain it is buried in, k16.2.412.4); a producer keeps the wire's
				const double at[3] = { th.pos[0], th.kind == simlink::Interactable::kItem || th.kind == simlink::Interactable::kContainer ? app.sim.thingY(id, th) : th.pos[1], th.pos[2] };
				if (th.kind == simlink::Interactable::kContainer)
				{
					WorldBuildings::Prop p{ id, app.sim.sidOf(th.record), { at[0], at[1], at[2] }, false, false, 0, cut_hidden(at) };
					p.pieceBuilding = th.building;	// the wire's tail: the layout piece that draws and picks it (SIMLINK kind 3)
					p.pieceIndex = th.piece;
					p.hasPiece = th.hasPiece;
					props.push_back(std::move(p));
				}
				if (th.kind == simlink::Interactable::kProducer) { props.push_back({ id, app.sim.sidOf(th.record), { at[0], at[1], at[2] }, true }); }
				if (th.kind == simlink::Interactable::kItem)
				{
					WorldBuildings::Prop p{ id, app.sim.sidOf(th.record), { at[0], at[1], at[2] }, false, true, th.yaw, cut_hidden(at) };
					// a severed limb with the wire's tint (SIMLINK "Tail, kind 2 severed limb only"): only an ITEM whose `item function` is 0x11 draws
					// through the limb material, and only when the server sent the tint; with no tail it draws as before
					const Characters::ItemInfo* info = app.chars.itemInfo(p.sid);
					if (th.hasLimb && info && info->function == 0x11)
					{
						p.limb = true;
						std::copy(th.limbSkin, th.limbSkin + 3, p.limbSkin);
						std::copy(th.limbBlood, th.limbBlood + 3, p.limbBlood);
						std::copy(th.limbScale, th.limbScale + 3, p.limbScale);
					}
					props.push_back(p);
				}
			}
			// The player's buildings (SIMLINK BUILDING 47; kenshi-k16.2.36.1): each drawn as a placement of its record, under construction by the sim's progress
			// extrapolated to now (a container or producer is a prop of its own above). The ones that are not storage or a mine have no other drawing.
			bool live = false;
			for (const auto& [id, b] : app.sim.decoder().buildings)
			{
				if (!app.sim.playerBuilding(b)) { continue; }
				const simlink::Interactable* th = app.sim.thing(id);
				if (th && (th->kind == simlink::Interactable::kContainer || th->kind == simlink::Interactable::kProducer)) { continue; }
				WorldBuildings::Prop p;
				p.id = id;
				p.sid = app.sim.sidOf(b.record);
				p.building = true;
				for (int k = 0; k < 3; k++) { p.pos[k] = b.pos[k]; }
				for (int k = 0; k < 4; k++) { p.rot[k] = b.rot[k]; }
				p.progress = (b.flags & simlink::SimLinkDecoder::Building::kBuilt) ? 1.0f : app.scriptBuild >= 0 ? std::min(app.scriptBuild, 1.0f) : std::min(0.999f, (ProgressFeed{ b.progress, b.rate, b.tick }).at(app.sim.tick()));
				if (p.progress < 1.0f) { live = true; }	// its progress is extrapolated from the tick: never cache it
				props.push_back(std::move(p));
			}
			app.propsValid = true;
			app.propsSerial = thingsSerial;
			app.propsBuildSerial = buildSerial;
			app.propsCut = cutSig;
			app.propsThingY = thingYSig;
			app.propsLive = live;
			app.buildings.setProps(props);
		}
		// the editor platform is one of the props (WorldBuildings::setProps adds it while setEditor is on): the list is rebuilt for it, the doors and the floor model wait for the game mode (kenshi-k16.2.472.3)
		if (!editor) { update_doors(dt); }
		CPUSTAGE("props");
		if (!editor)
		{
		// roof / floor slicing (kinspect camera_rts.md 6): every player character indoors, selected or not, slices the building it stands in; the auto
		// floor follows the followed character only, as the original's does; the first selected one only names the sliced building
		// (the wire's indoors bit + building + floor: in a building's interior AND under a ceiling, so a roof deck or a porch slices nothing; never a
		// client guess. Without a wire character the cursor peek's own bounds tests still decide what counts as inside)
		app.buildings.occupants.clear();
		float hullNone[3];
		for (const auto& [id, ch] : app.sim.decoder().chars)
		{
			if (!app.sim.isPlayer(id)) { continue; }
			if (ch.cur.flags & simlink::kIndoors) { app.buildings.occupants.push_back({ ch.cur.building, ch.cur.floor }); }
			else if (double rp[3]; app.sim.placed(id, rp, hullNone))	// on a roof deck: not indoors for the wire, but the building's top floor (roofBuilding)
			{
				if (const int roof = app.buildings.roofBuilding(rp, ch.cur.building); roof >= 0) { app.buildings.occupants.push_back({ 0, 0, roof }); }
			}
		}
		const std::vector<uint64_t>& sel = app.sim.selected();
		// "Following" for the auto floor is the original's follow (camera_rts.md 5): the hover / drive camera (V) AND the RTS camera centred on a character (F, a
		// portrait double click: centerId). order_sounds is played centred, so the threshold rule never ran there while bar_lure_hostile used V (kenshi-k16.2.236.1).
		const uint64_t tracked = camera_subject();
		const uint64_t lead = app.follow ? app.followId : tracked ? tracked : sel.empty() ? 0 : sel[0];
		app.buildings.following = tracked != 0;	// the auto floor follows the followed / centred character only; the first selected still names the sliced building (kenshi-h2v.24.6.2)
		app.buildings.leadId = lead;		// the lead's own floor is logged apart from the auto floor (the state file's "lead on floor N" said -1 for "not following")
		// The floor model's sample (WorldBuildings::updateFocus, kenshi-k16.2.236.14): the camera subject's wire state and its SIM position, never the drawn
		// one (its display slop lags a climb by metres, kenshi-k16.2.236.3); the roof deck test measures from the sim position too (the review's two
		// positions feeding one decision, kenshi-k16.2.236.9 section 2).
		app.buildings.subjectId = tracked;
		app.buildings.hasSubjectPos = tracked && app.sim.simPos(tracked, app.buildings.subjectPos);
		{
			// The wire's floor / indoors / building at the render tick (stateAt), not `cur`: the position is interpolated at the render clock, which sits
			// behind the newest snapshot and further behind after a stall, and reading the newest bits let a floor the feet had not reached settle the
			// model's stair latch origin (the 0/1/0/1 flap under load, kenshi-k16.2.236.17).
			simlink::Dynamic st;
			const bool known = tracked && app.sim.stateAt(tracked, st);
			app.buildings.subjectKnown = known;
			app.buildings.subjectIndoors = known && (st.flags & simlink::kIndoors);
			app.buildings.subjectBuilding = known ? st.building : 0;
			app.buildings.subjectFloor = known ? (int)st.floor : 0;
			app.buildings.subjectRoof = known && !app.buildings.subjectIndoors && app.buildings.hasSubjectPos
				? app.buildings.roofBuilding(app.buildings.subjectPos, st.building) : -1;
		}
		app.buildings.logFloor = app.opt.debug;	// the floor view's line (updateFocus, KCLIENT_DEV_TOOLS builds)
		const auto leadCh = app.sim.decoder().chars.find(lead);
		const bool leadKnown = lead && leadCh != app.sim.decoder().chars.end();
		app.buildings.hasLead = leadKnown && (leadCh->second.cur.flags & simlink::kIndoors);
		if (app.buildings.hasLead) { app.buildings.lead = { leadCh->second.cur.building, leadCh->second.cur.floor }; }
		else if (double rp[3]; leadKnown && app.sim.simPos(lead, rp))	// the lead on a roof deck (its sim position): the building's top floor
		{
			if (const int roof = app.buildings.roofBuilding(rp, leadCh->second.cur.building); roof >= 0) { app.buildings.hasLead = true; app.buildings.lead = { 0, 0, roof }; }
		}
#ifdef KCLIENT_DEV_TOOLS
		if (app.buildings.logFloor && leadKnown)	// kenshi-k16.2.266: the floor inputs, one line per change (the wire's indoors / building / floor, the client's roof test), with the sim tick
		{
			static unsigned long long pw = ~0ull, pb = 0; static int pf = -9, pr = -9;
			const auto& cu = leadCh->second.cur;
			double rp[3]; int roof = -1;
			if (!(cu.flags & simlink::kIndoors) && app.sim.simPos(lead, rp)) { roof = app.buildings.roofBuilding(rp, cu.building); }
			const unsigned long long w = (cu.flags & simlink::kIndoors) ? 1 : 0;
			if (w != pw || (unsigned long long)cu.building != pb || cu.floor != pf || roof != pr)
			{
				printf("floortrace tick %.3f wire indoors %llu building %llu floor %d | client roof %d\n", app.sim.tick(), w, (unsigned long long)cu.building, (int)cu.floor, roof);
				pw = w; pb = cu.building; pf = cu.floor; pr = roof;
			}
		}
#endif
		{	// Enter_Building / Leave_Building (audio_sfx_triggers.md 3): the lead character's interior state; Indoors = the record's `interior ambience` (1 Metal, 2 Stone, 3 Wicker: UNSURE order)
			const bool inside = lead && leadCh != app.sim.decoder().chars.end() && (leadCh->second.cur.flags & simlink::kIndoors);
			App::InteriorSound is;
			if (inside) { if (const auto r = app.interiorSounds.find(app.buildings.recordOf(leadCh->second.cur.building)); r != app.interiorSounds.end()) { is = r->second; } }
			static const char* const kIndoors[] = { "", "Metal", "Stone", "Wicker" };
			audiohook_indoors(inside, is.cavernous, kIndoors[std::clamp(is.ambience, 0, 3)], is.integrity);
		}		// the occluder fade: buildings between the camera and the character go to glass, or a see-through cone (ours; the original has none, kinspect
		// camera_rts.md 7): the followed or auto-centred one, else in the RTS camera the first selected (user 2026-11-05: not only when centred)
		const uint64_t seen = tracked ? tracked : !app.rts ? 0 : sel.empty() ? 0 : sel[0];
		double at[3];
		float hull[3];
		std::fill(app.buildings.xray, app.buildings.xray + 4, 0.0f);
		app.buildings.hasSeen = seen && kOccluderFade.get() && app.sim.placed(seen, at, hull);
		app.buildings.autoGhost = kOccluderGhost.get();
		std::copy(at, at + 3, app.buildings.seen);
		const auto seenCh = app.sim.decoder().chars.find(seen);
		app.buildings.seenIndoors = seenCh != app.sim.decoder().chars.end() && (seenCh->second.cur.flags & simlink::kIndoors);
		app.buildings.seenBuilding = seenCh != app.sim.decoder().chars.end() ? seenCh->second.cur.building : 0;
		// The follow camera (kenshi-k16.2.289.1): the boom keeps solid walls off the line, so the cone is only what is left: thin props (a sign, its frame,
		// a rail), faded to a dither whether or not they carry a collision shape. The cone is narrower there than the RTS camera's (MINE, units).
		// The RTS camera gets the cone too (kenshi-k16.2.353.4 point 8: anything solid inside the boom's minimum fades, foliage and rocks as well as
		// buildings), with or without the whole-building ghost (the ghost only knows buildings; the cone reaches a rock or a tree).
		const bool followCone = app.follow && app.buildings.hasSeen;
		if (app.buildings.hasSeen)
		{
			const float kRadius = followCone ? 16 : 40;	// MINE: the cone's widest, units
			for (int k = 0; k < 3; k++) { app.buildings.xray[k] = (float)(at[k] + (k == 1 ? 10.8 : 0) - app.pos[k]); }
			app.buildings.xray[3] = kRadius;
		}
		// outdoor lights: hour outside [sunrise, sunset) of GLOBAL CONSTANTS (kenshi-inspect world_particles.md §8a; rebirth: 05:00-23:00 is day)
		app.buildings.night = app.sky.clock.hours >= app.sky.constants().sunset || app.sky.clock.hours < app.sky.constants().sunrise;
		app.buildings.lightMinutes = (app.sky.clock.day * 24.0 + app.sky.clock.hours) * 60.0;
		// the interior mask (the particle clip) needs its hulls collected and loaded with the buildings; the particle systems of the last frame say whether it is live
		app.buildings.maskWanted = kInteriorMask.get();
		app.buildings.lampOcclusion = (kLampOcclusion.get() || kLampBeams.get() > 0.0) && !app.wmap.full && app.bldSet;	// render.lamp_occlusion (kenshi-jj3.9.1): the lamps register for the baked visibility (logic side)
		const double windYaw = app.windForce >= 0 ? app.windForce * 3.14159265358979 / 180.0 : app.sky.weather.windYaw;	// toward = (sin, cos): wind-aimed parts
		app.buildings.wind[0] = (float)std::sin(windYaw);
		app.buildings.wind[1] = (float)std::cos(windYaw);
		app.buildings.wind[2] = app.windForce >= 0 || app.sky.weather.windValid ? 1.0f : 0.0f;
		app.buildings.windSpeed = app.windSpeedForce >= 0 ? (float)app.windSpeedForce : app.windForce < 0 && app.sky.weather.windValid ? app.sky.weather.windSpeed : -1.0f;	// a scene's wind=DEG has no speed
		}	// !editor
		// its mesh and texture creates and evicts are render queue jobs, its instances a packet upload (kenshi-k16.2.205.12.35)
		app.buildings.update(app.pos, mvp, app.opt.fog, app.scripted() && !kStreamScripted.get(), sim_time(), app.shadow.active() ? Shadow::kRadius[Shadow::kCascades - 1] : 0.0, pk.buildings);
		CPUSTAGE("buildings.update");
		// render.interior_light / shafts / lamps / sun bounce (overlap review T19): the baked buildings' volumes, collected here (it starts the bakes and reads the building tables), carried in
		// the packet; the draw side only uploads them (renderframe worldSetup). The conditions are the ones the draw used to evaluate on the packet's copies.
		if (!editor)
		{
			const float strength = (float)(1.0 - kInteriorLight.get());
			const bool shafts = kSunShafts.get() > 0.0 || kDustMotes.get() > 0.0 || kLampBeams.get() > 0.0 || app.buildings.lampOcclusion;
			const bool sunBounce = kSunBounce.get() > 0.0 && strength > 0.0f;
			const LightParams& sl = app.sky.light();
			if (sunBounce && sl.v[0][1] > 0.0f)
			{
				float ground[3] = { 0.3f, 0.3f, 0.3f };
				if (app.sky.hasGround) { for (int k = 0; k < 3; k++) { ground[k] = app.sky.groundAlbedo[k]; } }
				if (strength > 0.0f || shafts) { app.buildings.lightVolumes(cam.pos, 500.0, lightvol::kMax, pk.buildings.volumes, true, sl.v[0], ground); }
			}
			else if (strength > 0.0f || shafts) { app.buildings.lightVolumes(cam.pos, 500.0, lightvol::kMax, pk.buildings.volumes); }
		}
		CPUSTAGE("buildings.lightVolumes");
		if (app.foliageOn && !editor)	// the creator draws no foliage or grass (kenshi-k16.2.472.3)
		{
			if (app.scripted()) { wait_ready([] { return app.foliage.ready() || app.foliage.failed(); }); }
			{
				// its cell buffers and the building meshes it drains (WorldBuildings::ensurePart, drain) are resource jobs (kenshi-k16.2.205.12.18, .35)
				const unsigned shadowLevels = app.shadow.active() && !app.wmap.full && !editor ? (1u << Foliage::kLevels) - 1 : 0u;
				app.foliage.prepare(app.pos, mvp, app.opt.fog, app.scripted() && !kStreamScripted.get(), shadowLevels, pk.foliage);
			}
			if (app.grassOn)
			{
				if (app.scripted()) { wait_ready([] { return app.foliage.ready() || app.foliage.failed(); }); wait_ready([] { return app.grass.ready() || app.grass.failed(); }); }
				app.grass.prepare(app.pos, mvp, app.scripted(), pk.grass);
			}
		}
	}

	CPUSTAGE("foliage+grass.update");
	// Weather + placed world particles: simulate and write the instance buffer before any pass (a scripted run freezes the simulation after
	// its pre-warm, so shots are deterministic). After buildings.update, so the placed flames are this frame's effect instances.
	app.particlesDrawn = 0;
	pk.particles.clear();
	if (app.opt.particles && !app.wmap.full && !editor)	// the creator's scene: clear and dry, no weather particles (k16.17.6.1)
	{
		Particles::Frame pf;
		for (int k = 0; k < 3; k++) { pf.cam[k] = app.pos[k]; }
		// the anchored weather effects (kenshi-inspect world_effects §2/§3): the camera forward and the pivot the type 4 handler rides (rwh.81 §3,
		// one handler at the pivot, y = terrain + 100 there). The pivot: the selected player's feet, else the followed character's, else the floating
		// camera's focus, else (a free camera has none) the point kCoverAhead along the view - MINE.
		double fwd[3];
		forward_vec(app.yaw, app.pitch, fwd);
		for (int k = 0; k < 3; k++) { pf.view[k] = fwd[k]; }
		double pivot[3];
		for (int k = 0; k < 3; k++) { pivot[k] = app.pos[k] + fwd[k] * Particles::kCoverAhead; }
		if (app.hm) { const char* on = nullptr; pivot[1] = ground_under(pivot[0], pivot[2], &on); }
		else { pivot[1] = ground; }
		{
			double feet[3];
			float hull[3];
			bool havePivot = false;
			for (const uint64_t id : app.sim.selected())
			{
				if (app.sim.isPlayer(id) && app.sim.placed(id, feet, hull)) { for (int k = 0; k < 3; k++) { pivot[k] = feet[k]; } havePivot = true; break; }
			}
			if (!havePivot && app.follow && app.followId && app.sim.placed(app.followId, feet, hull)) { for (int k = 0; k < 3; k++) { pivot[k] = feet[k]; } havePivot = true; }
			if (!havePivot && app.rts)
			{
				pivot[0] = app.focus[0];
				pivot[2] = app.focus[2];
				pivot[1] = ground_under(pivot[0], pivot[2], &app.focusOn);
			}
		}
		for (int k = 0; k < 3; k++) { pf.anchor[k] = pivot[k]; }
		pf.groundY = (float)ground;
		pf.sunY = (float)app.sky.sunDir()[1];
		pf.exposure = app.sky.light().v[4][3];
		if (app.sky.weather.windValid)	// the sim's live wind at the camera (character yaw: toward = (sin, cos))
		{
			pf.wind[0] = (float)(std::sin(app.sky.weather.windYaw) * app.sky.weather.windSpeed);
			pf.wind[1] = (float)(std::cos(app.sky.weather.windYaw) * app.sky.weather.windSpeed);
		}
		else
		{
			const float wl = std::hypot(app.sky.weather.wind[0], app.sky.weather.wind[1]);
			for (int k = 0; k < 2; k++) { pf.wind[k] = wl > 0 ? app.sky.weather.wind[k] / wl * app.sky.weather.windSpeed : 0.0f; }
		}
		pf.warmSteps = app.scripted() ? 1 << 30 : 100;	// live: a burst of new systems warms over frames (particles.cpp); scripted runs and scene shots warm at once
		pf.dt = app.scripted() ? (float)app.particleScriptDt : app.sky.clock.paused ? 0.0f : (float)std::fmin(dt, 0.1);	// paused: weather and flames hold still
		app.particles.setEffects(app.sky.weather.effects, app.rainForce, app.dustForce);
		// The weather's world instances (types 2/3/7/8/9/10): the exe's group tick, spawned on the owner group's terrain cells (type 10: on a band
		// around the pivot). The owner is the group showing this weather; a pinned weather (no group) uses the camera's group. Lightning (7) lands
		// near the nearest player character, so the player faction's characters go along (the wire's player flag; none without a sim, as in the exe
		// with no player).
		int owner = -1;
		if (!app.wx.atlas().group.empty())
		{
			const WeatherAtlas& a = app.wx.atlas();
			for (size_t i = 0; i < a.groupWeather.size(); i++) { if (a.groupWeather[i].name == app.sky.weather.name) { owner = (int)i; break; } }
			if (owner < 0) { owner = a.groupIndexAt(app.pos[0], app.pos[2]); }
		}
		std::vector<std::array<double, 3>> players;
		for (const uint64_t id : app.sim.characterIds())
		{
			double p[3];
			float hull[3];
			if (app.sim.isPlayer(id) && app.sim.placed(id, p, hull)) { players.push_back({ p[0], p[1], p[2] }); }
		}
		app.weatherInst.update(app.sky.clock.paused || dev::sceneTesting() ? 0.0 : dt * app.sky.clock.speed, app.sky.weather, app.wx.atlas(), app.hm ? &*app.hm : nullptr, owner,
			pf.wind[0], pf.wind[1], app.pos, pivot, players);
		CPUSTAGE("weatherInst.update");
		// The placed flames: ParticleWorld diffs the visible effect instances, fades systems in and out, and hands them to the same simulation.
		static const std::vector<WorldBuildings::PlacedEffectRt> none;
		app.particleWorld.update(pf.dt, app.bldSet ? app.buildings.placedEffects() : none, app.buildings, app.buildings.night);
		CPUSTAGE("particleWorld.update");
		// The sim's beams (BEAMS): copy a new frame, then draw the ones at the render tick. Their scripts are kept off the local weather placement
		// below, so each beam is drawn once and at the sim's own position (the wind-up is the lead the sim sent it with, SIMLINK "BEAMS").
		if (app.sim.active() && app.sim.decoder().beamsSerial != app.beamsSeen)
		{
			app.beamsSeen = app.sim.decoder().beamsSerial;
			app.beams.sync(app.sim.decoder());
		}
		CPUSTAGE("beams.sync");
		const std::vector<std::string> beamScripts = app.beams.scripts();
		std::vector<Particles::PlacedSystem> placed = app.particleWorld.systems();
		for (Particles::PlacedSystem& s : app.weatherInst.systems(beamScripts)) { placed.push_back(std::move(s)); }
		if (app.sim.active() && app.beamsOn)
		{
			// The original's loaded-cell gate on the draw set (kenshi-k16.2.409.3, rwh.211); `debug.venge_beams_gate 0` is the dev A/B (the old look)
#ifdef KCLIENT_DEV_TOOLS
			const bool gate = kVengeBeamsGate.get();
#else
			constexpr bool gate = true;
#endif
			app.beams.systems(app.sim.tick(), app.hm ? &*app.hm : nullptr, cam, players, kVengeBeamsMax.get(), gate, placed);
		}
		// `debug weff SCRIPT DX DZ`: a dev A/B aid that pins one weather-style system at the pivot + offset on the terrain (a twister for a shot)
		for (size_t i = 0; i < app.weff.size(); i++)
		{
			Particles::PlacedSystem p;
			p.key = 0xffeef00000000000ull + i;
			p.script = app.weff[i].script;
			const double x = pivot[0] + app.weff[i].off[0], z = pivot[2] + app.weff[i].off[1];
			p.pos[0] = x;
			p.pos[1] = app.hm ? app.hm->height(x, z) : 0.0;
			p.pos[2] = z;
			p.seed = (uint32_t)(0x5eedu + i);
			placed.push_back(std::move(p));
		}
		CPUSTAGE("beams.systems+placed");
		app.particles.setPlaced(placed);
		CPUSTAGE("particles.setPlaced");
		if (app.sim.active() && app.beamsOn)
		{
			if (const char* lv = getenv("KBEAMSLOG")) { if (atoi(lv) >= 2) { app.beams.fadeLog(app.sim.tick(), (long)app.frames); } }
			if (getenv("KBEAMSLOG"))		// a line per second: beams on the wire / in their burst / drawn / fading out
			{
				static double lastLog = -1e9;
				const double t = app.sim.tick();
				if (t - lastLog >= 20.0)
				{
					lastLog = t;
					const BeamField::Stats& st = app.beams.stats();
					size_t tp = 0, tnear = 0;
					const size_t tail = app.particles.tailSystems(app.beams.keys(), &tp, cam.pos, 3000.0, &tnear);
					// inview: the drawn beams whose column is in the camera frustum (the ask's "visible at once", rwh.175 0-2); focus/cells: the
					// gate's focus cell and its loaded-cell count (rwh.211, kenshi-k16.2.409.3)
					fprintf(stderr, "beamlog tick %.0f wire %zu burst %zu drawn %zu inview %zu fading %zu tail %zu tailparts %zu dup %zu stale %zu max %d burst3k %zu drawn3k %zu tail3k %zu drawnfar %.0f focus %d,%d cells %zu burstview %zu burstopenview %zu nearest %.0f newview %zu\n", t, st.wire, st.burst, st.drawn, st.inview, st.fading, tail, tp, st.dup, st.stale, kVengeBeamsMax.get(), st.burstNear, st.drawnNear, tnear, st.drawnFar, st.focusX, st.focusZ, st.cells, st.burstView, st.burstOpenView, st.drawnNearest, app.beams.newViewTotal());
				}
			}
		}
		app.particlesDrawn = app.particles.prepare(pf, pk.particles);
		CPUSTAGE("particles.prepare");
	}
	app.blood.age(app.scripted() ? 0.0 : dt, app.sky.clock.paused ? 0.0 : app.sky.clock.speed);
	app.blood.update(app.pos);
	CPUSTAGE("particles+blood");
	// the hand-off to the draw (render hook 6): the locals the draw half reads
	pk.path = FramePath::kWorld;
	pk.dt = dt;
	pk.editor = editor;
	pk.views[0] = cam;
	pk.viewCount = 1;
	pk.logicStart = cpu0;	// logic_frame's end measures pk.logicMs from it
}

// The logic of the frame's end (kenshi-k16.2.205.12.23; review kenshi-k16.2.205.12.15 section 2): what used to run between the draw's last
// pass and sg_commit runs here, after the packet is filled. Whatever needs the drawn frame (pixels, depth, the draw's counters and stats) is a
// readback request, served after the last pass (readback::serve) with its done in request order; on one thread that is the same point in
// the frame as before. Per path, as the draws called them: the script's post commands everywhere, F12 and the shot captures on the title and
// the world, the session load's progress, the burst and the scene test on the world, the --loading-shot on the loading screen.
static void logic_tail(FramePacket& pk)
{
	if (pk.path == FramePath::kLoading) { loading_shot(); return; }
	if (pk.path == FramePath::kWorld && app.sessionLoad) { update_session_load(pk.dt); }	// before the draw's loading screen reads its progress, as before
	run_script(true);
	if (pk.path != FramePath::kTitle && pk.path != FramePath::kWorld) { return; }
	shot_pending_frame();
	// the shot captures follow this frame's shot dones (dev::shotTreesBegin, dev::shotBegin), as they did when they ran after the reads
	readback::after([](bool) { dev::shotTreesFrame(dev_context()); });	// the capture's squad pass, completion and file (never waits: the answers come in on the sim thread; dev/shotsidecars.cpp)
	readback::after([](bool) { dev::shotFrame(); });	// the shot's bead copy once its trees are done (or 6 s are up)
	if (pk.path != FramePath::kWorld) { return; }
#ifdef KCLIENT_DEV_TOOLS
	resize_burst_frame();	// the window-state gate's scripted burst (k16.2.222)
#endif
	dev::sceneTestPost(dev_context());	// a --scene-test run: the frame counts, the floor step, the shot (dev/scenetest.cpp)
}

// The early paths' packet block (render sitting 12, kenshi-k16.2.205.12.60; renderframe.hpp FramePacket::Early): what frame_blank, draw_loading and
// draw_title read from App, taken at the end of logic_frame. The viewer's fields are dev::viewFrame's (dev/viewmesh.cpp, render hook 9).
static void record_early(FramePacket& pk)
{
	FramePacket::Early& e = pk.early;
	e.mouse[0] = app.mouseX;
	e.mouse[1] = app.mouseY;
	e.sessionLoad = app.sessionLoad;
	if (pk.path == FramePath::kLoading)
	{
		e.loadFrac = (float)app.loadIdx / (float)app.loadSteps.size();
		e.loadStep = app.loading() ? app.loadSteps[app.loadIdx].first : "starting";
	}
}

// The packet's UI lists (render hook 8's host half, kenshi-k16.2.205.12.39; DESIGN §6), in logic after the tail, where the draws that replay them used to
// read the UI: each list is recorded when the draw that replays it would have drawn it, the labels first (their old batch came first). Their texture
// uploads (font pages baked while recording or measuring, icons, the atlas) are one frame-lane job of the packet, so they apply once before its draws.
static void record_ui(FramePacket& pk)
{
	pk.uiDrawn = pk.labelsDrawn = false;
	pk.labels.begin(pk.width, pk.height);	// no stale draws when nothing records
	pk.ui.begin(pk.width, pk.height);
	if (app.uiOn)
	{
		// draw_sim_markers' condition (the world's overlay)
		if (pk.path == FramePath::kWorld && pk.viewCount > 0 && app.sim.active() && !app.wmap.full && !app.hideUi && !title_shown())
		{
			float mvp[16];
			overlay_camera(pk).mvp(mvp);
			ui::gfx::begin(pk.labels, view::width(), view::height());
			world_labels(mvp);
			ui::gfx::end();
			pk.labelsDrawn = true;
		}
		// where ui::draw ran: frame_blank (the creator's panels wait for their scene), draw_title, the world's tail
		switch (pk.path)
		{
		case FramePath::kBlank: case FramePath::kAwait: pk.uiDrawn = !(app.sessionLoad && app.creatorUi->shown()); break;
		case FramePath::kTitle: pk.uiDrawn = true; break;
		case FramePath::kWorld: pk.uiDrawn = !app.hideUi; break;
		default: break;
		}
		if (pk.uiDrawn) { ui::record(pk.ui); }
	}
	std::vector<dl::Upload> ups = pk.labels.takeUploads();
	for (dl::Upload& u : pk.ui.takeUploads()) { ups.push_back(std::move(u)); }
	size_t bytes = 0;
	for (const dl::Upload& u : ups) { bytes += u.rgba.size(); }
	// every packet, uploads or not: the destroys of the last one take effect here (drawlist_gpu.hpp)
	rq::push("ui-uploads", [ups = std::move(ups)]() mutable { dl::gpu::apply(std::move(ups)); }, rq::Lane::kFrame, bytes);
}

static void record_overlay(FramePacket& pk);

// The characters' frame data (kenshi-k16.2.205.12.26; review kenshi-k16.2.205.12.15 3.3): the palettes, the part records and the instance lists of
// every pass, built from the live poses here in the logic half into the packet (renderframe.cpp's worldSetup uploads them; the draw reads no pose).
// The cascades are shadow.update's (logic_paths): resolve() in the draw changes which tiles it restores, not renders(). Every field, every path.
static void snapshot_shadow(FramePacket& pk)
{
	// the atlas is not trustworthy while the pass does not run (the draw's else branch forgets its cache): logic's half of that is here, so the draw never writes Shadow state
	if (!(app.shadow.active() && !pk.worldMap && !pk.editor)) { app.shadow.invalidate(); }
	app.shadow.snapshot(pk.shadow);
}

static void prepare_chars(FramePacket& pk)
{
	if (pk.path != FramePath::kWorld) { pk.chars.clear(); return; }
	CPUSTAGE_ACC("chars.prepare");
	const ShadowCascade* casc[Characters::kMaxCascades] = {};
	if (app.shadow.active() && !pk.worldMap && !pk.editor)
	{
		for (int c = 0; c < Shadow::kCascades; c++) { if (app.shadow.renders(c)) { casc[c] = &app.shadow.cascade(c); } }
	}
	app.chars.prepare(pk.scene.pos, casc, pk.chars);
}

void logic_frame(FramePacket& pk)
{
	dev::packetPoison(pk);	// dev builds, KCLIENT_PACKET_POISON: wipe the slot so a field this frame does not write shows (dev/packetdigest.hpp)
	pk.serial = rq::beginPacket();	// first: every resource job queued below (the paths and the tail) belongs to this packet (docs/RENDER_THREAD.md §7.2)
	{	// the ready-set gate (kenshi-k16.2.258.3): under the session load screen nothing of the world is on show, so the stream lane's play budgets are lifted
		const bool gate = app.sessionLoad && kLoadGate.get();
		// From the client's start until the first session's world is shown nothing of the world is on show either (the first frames create the
		// pipelines behind the title or the load screen): the lockstep log counts that window as the load's (kenshi-k16.2.437.4.4)
		logicthread::setLoadScreen(gate || (!app.sessionShown && session_load_expected()));
		rq::setStreamTimeBudget(gate ? kLoadStreamMs.get() : kStreamMs.get());
		rq::setStreamBudget(gate ? (size_t)256 << 20 : (size_t)8 << 20);
		app.buildings.streamAll = gate;
		app.buildings.streamPriority = kStreamPriority.get();
	}
	dds_set_slice((size_t)kStreamSliceMb.get() << 20);
	dev::SgLogicScope sgLogic;	// --sg-owner-logic: the logic half owns no sokol (an sg_* call here aborts, as on the logic thread to come); the tail too
	readback::beginPacket(pk.serial);	// the readback requests made from here on (the dones below included) belong to this packet
	app.portraitTable.apply(app.portraits.take());	// what the render thread drew of the jobs sent before (the mailbox; kenshi-k16.2.205.12.56)
	framestats::take();	// the last drawn frame's stats, once: every logic read of framestats::stats() this frame sees this copy (render hook 5, kenshi-k16.2.205.12.24)
	app.frames = framestats::stats().frames;	// the frames drawn so far: logic's copy (the render side counts them, kenshi-k16.2.205.12.27)
	readback::runDone();	// the dones of the requests the last drawn frame served, here on the logic thread before anything else (render hook 4, kenshi-k16.2.205.12.24)
	readback::runWrites();	// the screenshots the writer thread finished since the last frame: their toasts follow the file, not the read (kenshi-k16.2.312)
	logic_paths(pk);
	logic_tail(pk);
	record_ui(pk);
	pk.cursor = app.cursors.current();	// after every hover decision of the frame (render hook 8)
	pk.look = app.look;
	pk.worldMap = app.wmap.full;
	if (pk.path == FramePath::kWorld)
	{
		pk.sky = app.sky.frame();	// the sky block (renderframe.hpp): after the frame's last change to the sky
		pk.lightOff = dev::lightOffMask();	// the light debugger (knobs/lightterms.hpp): the logic's switches, read by the draw only from the packet
		pk.sky.light.plHead[3] = (float)pk.lightOff;
		pk.post.eyeAdapt = kEyeAdapt.get();
		pk.post.scriptedJump = app.scripted() && !app.opt.adaptScripted;
		pk.post.fixedExposure = pk.editor ? 1.0f : (float)app.opt.exposure;	// the creator has no exposure: the display gain is pinned to 1 (creatorscene.hpp)
		pk.scene.bldSet = app.bldSet;
		std::copy(app.pos, app.pos + 3, pk.scene.pos);
		pk.scene.foliageOn = app.foliageOn;
		pk.scene.grassOn = app.grassOn;
		pk.scene.animTime = sim_time();
		pk.scene.terrainLoads = app.scripted() ? -1 : kLoadsPerFrame;
		pk.scene.terrainView = app.terrain.viewMode;
		pk.scene.squads = app.squads.drawCount();	// sitting 7 (kenshi-k16.2.205.12.51): the draw reads neither app.squads.on nor its dots
		pk.scene.waterOn = app.opt.water;
		pk.scene.mouse[0] = app.mouseX;	// sitting 8: the id pass's pixel (logic's copy of the pointer; the readback is consumed a frame later)
		pk.scene.mouse[1] = app.mouseY;
		pk.scene.windowW = view::width();	// the window the pointer is in, from the same frame (the id pass scales by applied size / window)
		pk.scene.windowH = view::height();
		// sitting 9 (kenshi-k16.2.205.12.54): worldSetup's inputs
		pk.scene.simActive = app.sim.active();	// the portraits render only with a live sim
		if (pk.scene.simActive) { app.portraitTable.send(pk.portraitJobs, app.scripted() ? Portraits::kCells : 1); }	// after every get() of the frame
		else { pk.portraitJobs.clear(); }
#ifdef KCLIENT_DEV_TOOLS
		dev::labFrame(pk.lab);	// the animation lab's draw values (logic thread; worldSetup's labRender reads only this copy, kenshi-k16.2.205.12.56)
#endif
		pk.scene.interiorLight = (float)(1.0 - kInteriorLight.get());	// the option is the amount of original lighting kept (1 = the effect off); the packet carries the effect strength
		pk.scene.sunBounce = (float)kSunBounce.get();
		pk.scene.lampOcclusion = app.buildings.lampOcclusion;
		pk.scene.interiorMask = kInteriorMask.get();
		pk.scene.groundOk = app.sky.hasGround;	// the colour map under the camera (set at the frame's camera update): the sun bounce's ground
		std::copy(app.sky.groundAlbedo, app.sky.groundAlbedo + 3, pk.scene.ground);
		pk.scene.sunShafts = (float)kSunShafts.get();
		pk.scene.dustMotes = (float)kDustMotes.get();
		pk.scene.lampBeams = (float)kLampBeams.get();
#ifdef KCLIENT_DEV_TOOLS
		pk.scene.sunShaftsDebug = kSunShaftsDebug.get();
#endif
		pk.scene.scripted = app.scripted();
		// sitting 10 (kenshi-k16.2.205.12.55): the overlay's flags (the old conditions, evaluated here), the load screen's progress
		pk.overlay.debug = app.opt.debug;
		pk.overlay.hideUi = app.hideUi;
		pk.overlay.title = title_shown();
		pk.overlay.creatorShown = app.creatorUi && app.creatorUi->shown();
		pk.overlay.simActive = app.sim.active();
		pk.overlay.sessionLoad = app.sessionLoad;
		pk.overlay.sessionProgress = app.sessionPeak > 0 ? 1.0f - (float)app.sessionPending / (float)app.sessionPeak : 0.0f;
		// the rain the particles use: the scene's forced rain=, else the current weather's blended intensity. Water turns it into the game's rainAmount.
		pk.scene.rain = app.rainForce >= 0.0f ? app.rainForce : app.sky.weather.rain;
	}
	// the detection arrows' marks (sitting 7): the sim view's DETECTED_BY entries at their drawn feet; none off the world path or on the full map
	if (pk.path == FramePath::kWorld && !app.wmap.full) { pk.arrows = stealth_arrow_marks(); } else { pk.arrows.clear(); }
	record_early(pk);	// sitting 12: the early paths' block
	record_overlay(pk);	// sitting 11: the overlay's debug views, sim markers and map / HUD, after the overlay block's flags
	// the portrait jobs' poses and frame data (kenshi-k16.2.205.12.67): in the logic half beside prepare_chars, after every change of the frame
	if (pk.path == FramePath::kWorld && pk.scene.simActive) { Portraits::pose(app.chars, pk.portraitJobs); }
	snapshot_shadow(pk);	// the shadow data every draw reads, once shadow.update and the frame's path are final (kenshi-k16.2.205.12.113)
	prepare_chars(pk);	// last: the poses after every change of the frame (the tail's script ops included), as when it ran in the draw
	CPUSTAGE("logic.tail+packet");	// the tail, the UI and overlay recording, the poses: their own stage, not folded into the draw's first (kenshi-k16.2.205.12.74)
	CPUSTAGE_HALF_END();	// the logic half's marks go to the shared hand-off; the draw's thread merges them at its line (kenshi-k16.2.205.12.94)
	pk.logicMs = pk.path == FramePath::kWorld ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pk.logicStart).count() : 0.0;
	dev::packetDigest(pk);	// dev builds, KCLIENT_PACKET_DIGEST: one hash per packet block to stderr (dev/packetdigest.hpp)
}

// The world overlay's logic-owned draws (render sitting 11, kenshi-k16.2.205.12.57; renderframe.hpp FramePacket::OverlayLists, overlaylist.hpp): the
// debug views (nav overlay, light probes, trails), the sim markers (with the labels when there is no UI) and the minimap / full map / HUD were the
// last world blocks the draw half ran from App (RenderState::Legacy's debugViews, simMarkers and mapHud). They run here now, in logic at the end of
// logic_frame under the old conditions, through ovl:: into the packet's overlay lists; renderframe.cpp's world_overlay plays the lists at the same
// place in the order. The 3D marks are projected here with overlay_camera (the packet's view 0): the lists hold pixels.
// debugViews: the --debug overlays under the markers (recorded with the packet's debug flag, off the full map and the title). The trails go to their own
// list: renderframe.cpp plays it into the trails' sgl context (Trails::submit).
static void record_debug_views(const FramePacket& pk, ovl::List& out, ovl::List& trails, bool& trailsOn)
{
	const Camera cam = overlay_camera(pk);
	float mvp[16];
	cam.mvp(mvp);
	ovl::Record rec(out);
	// the overlays' range centre (debug.overlay_range, kenshi-uuh8): the first selected character, else the camera focus
	double centre[3];
	{
		const double* f = app.rts ? app.focus : app.pos;
		std::copy(f, f + 3, centre);
		double at[3];
		float hull[3];
		if (app.opt.debug && !app.sim.selected().empty() && app.sim.placed(app.sim.selected().front(), at, hull)) { std::copy(at, at + 3, centre); }
	}
	if (app.opt.debug && kNavOverlay.get() && app.nav.mesh())	// a debug overlay: --debug gates the config key too
	{
		NavBands bands;	// the level: the first selected character's feet, else the focus ground
		bands.band = kNavBand.get();
		bands.ref = app.rts ? app.focus[1] : app.hm->height(app.pos[0], app.pos[2]);
		double at[3];
		float hull[3];
		bands.stair = (float)kNavStair.get();
		bands.pruned = kNavPruned.get();
		bands.trace = std::exchange(app.navTrace, false);
		const double* f = app.rts ? app.focus : app.pos;
		std::copy(f, f + 3, bands.at);
		bands.at[1] = bands.ref;
		if (!app.sim.selected().empty() && app.sim.placed(app.sim.selected().front(), at, hull)) { bands.ref = at[1]; std::copy(at, at + 3, bands.at); }
		bands.cut = [](const double p[3]) { return app.buildings.cutAway(p); };
		const Pick hp = pick_screen(app.mouseX, app.mouseY);
		bands.hover = hp.kind == Pick::Ground && app.nav.walkable(hp.hit) == ClientNav::Reach::kYes;	// the cursor's own test: no highlight where it says invalidmove
		std::copy(hp.hit, hp.hit + 3, bands.hoverAt);
		draw_navmesh(*app.nav.mesh(), mvp, cam.pos, centre, view::widthf(), view::heightf(), bands);
	}
	if (app.opt.debug)	// debug.collision, the flip partner of the navmesh overlay (dev/colloverlay.cpp: off by default, and off in an OFF build)
	{
		dev::collisionOverlay(app.collide, app.hm ? &*app.hm : nullptr, centre, mvp, cam.pos, view::widthf(), view::heightf());
	}
	if (kLightProbes.get())	// the baked light probes of the nearest building (kenshi-jj3.1.1); off: nothing runs
	{
		static std::vector<WorldBuildings::ProbePoint> probes;
		if (app.buildings.lightProbes(app.pos, 700.0, probes))
		{
			const float width = view::widthf(), height = view::heightf();
			ovl::disable_texture();
			ovl::begin_triangles();
			for (const WorldBuildings::ProbePoint& p : probes)
			{
				if (ovl::num_vertices() + 6 > 56000) { break; }	// never fill the default context (k16.19.6.1)
				const float v[3] = { (float)(p.p[0] - cam.pos[0]), (float)(p.p[1] - cam.pos[1]), (float)(p.p[2] - cam.pos[2]) };
				float c[4];
				for (int i = 0; i < 4; i++) { c[i] = mvp[i] * v[0] + mvp[4 + i] * v[1] + mvp[8 + i] * v[2] + mvp[12 + i]; }
				if (c[3] < 1.0f) { continue; }
				const float sx = (c[0] / c[3] * 0.5f + 0.5f) * width, sy = (0.5f - c[1] / c[3] * 0.5f) * height, h = 2.0f;
				if (p.usable) { const float g = std::clamp(std::sqrt(std::max(p.level, 0.0f)), 0.0f, 1.0f); ovl::c4f(g, g, g, 1.0f); }
				else { ovl::c4f(1.0f, 0.1f, 0.1f, 1.0f); }
				ovl::v2f(sx - h, sy - h); ovl::v2f(sx + h, sy - h); ovl::v2f(sx + h, sy + h);
				ovl::v2f(sx - h, sy - h); ovl::v2f(sx + h, sy + h); ovl::v2f(sx - h, sy + h);
			}
			ovl::count_triangles();
			ovl::end();
		}
	}
	dev::heightProbeDraw(mvp, cam.pos, view::widthf(), view::heightf());	// the vertical height probe's line and ticks (--debug, dev/heightprobe.cpp)
	trailsOn = app.trails.on() && app.sim.active();	// the debug trails over the scene (kenshi-k16.2.73)
	if (trailsOn)
	{
		ovl::Record trec(trails);
		app.trails.draw(mvp, cam.pos, view::widthf(), view::heightf());
	}
}

// The corner minimap / full map and the top-left HUD (recorded with the UI shown and no creator screen over it)
static void record_map_hud()
{
	const TownSet* towns = app.townsReady ? &app.towns : nullptr;
	// The top-left command-line text is a development view: --debug only (client/CLAUDE.md). The full map stays: it is the fallback map of a run
	// without the UI layer (toggle_map) and the debug key's view. The corner minimap is the UI's window now (ui::Minimap, uihost.cpp, ui.minimap).
	if (app.wmap.full) { app.wmap.drawFull(app.pos[0], app.pos[2], app.yaw, towns, &app.squads, ui::keys::label(app.uiOn ? "debug.full_map" : "toggle_map")); }
	if (app.opt.debug) { draw_hud(); }
}

// The packet's overlay lists, in logic after record_ui and the overlay block (logic_frame): each recorded under the condition world_overlay
// (renderframe.cpp) plays it with, in the old order (the debug views, the sim markers, the map / HUD).
static void record_overlay(FramePacket& pk)
{
	FramePacket::OverlayLists& ol = pk.overlayLists;
	ol.debug.clear();
	ol.trails.clear();
	ol.markers.clear();
	ol.mapHud.clear();
	ol.trailsOn = false;
	ol.width = view::widthf();
	ol.height = view::heightf();
	if (pk.path != FramePath::kWorld || pk.viewCount < 1) { return; }
	const FramePacket::Overlay& ov = pk.overlay;
	if (ov.debug && !pk.worldMap && !ov.title) { record_debug_views(pk, ol.debug, ol.trails, ol.trailsOn); }
	if (ov.simActive && !pk.worldMap && !ov.hideUi && !ov.title) { ovl::Record rec(ol.markers); draw_sim_markers(pk); }	// under the minimap
	if (!ov.hideUi && !ov.title && !ov.creatorShown) { ovl::Record rec(ol.mapHud); record_map_hud(); }	// the creator screen covers the map and the debug text
}

// render.thread=lockstep (logicthread.hpp; RENDER_THREAD.md 7.1 rules 7-9, kenshi-k16.2.205.12.25): logic_frame runs on its own thread and the two
// alternate. The logic ms / render ms split of the world frame's CPU (rule 11; no CPU span crosses the threads): logicMs is the packet's
// (pk.logicMs, the world path's start to logic_frame's end), renderMs over render_frame; FrameStats::cpu is their sum up to the overlay's end
// (the hand-off wait not included, kenshi-k16.2.205.12.27).
struct IdleSeen { long polls = 0; double tick0 = 0, tick1 = 0; uint64_t snap0 = 0, snap1 = 0; };	// what the idle polls saw (logic thread only)
static IdleSeen gIdleSeen;
void start_logic_thread()
{
	logicthread::Hooks h;
	h.logic = [](int slot)
	{
		FramePacket& pk = gPackets[slot];
		logic_frame(pk);
		if (pk.path == FramePath::kWorld)
		{
			const double ms = pk.logicMs;
			if (ms < 250) { app.logicMs = app.logicMs == 0 ? ms : app.logicMs * 0.9 + ms * 0.1; }
			logicthread::noteLogicMs(ms);	// kenshi-k16.2.437.1: the pace log's "logic ms vs bound" (atomic; the render thread reads it)
		}
		const logicthread::IdleStats idle = logicthread::takeIdle();
		if (idle.waits && logicthread::mode() == logicthread::Mode::kOverlap)	// rule 6's skip: the packets taken back, the frames run meanwhile
		{
			fprintf(stderr, "kclient: overlap: logic waited %.0f ms for its packet's take (the 100 ms bound ran out %ld time%s, %ld packet%s skipped); render tick %.2f\n",
				idle.waitedMs, idle.expiries, idle.expiries == 1 ? "" : "s", idle.skips, idle.skips == 1 ? "" : "s", app.sim.tick());
		}
		else if (idle.waits)
		{
			fprintf(stderr, "kclient: lockstep: logic waited %.0f ms for its last packet's draw (the 100 ms bound ran out %ld time%s, %ld link poll%s", idle.waitedMs, idle.expiries,
				idle.expiries == 1 ? "" : "s", idle.polls, idle.polls == 1 ? "" : "s");
			if (gIdleSeen.polls) { fprintf(stderr, "; over the polls: render tick %.2f -> %.2f, newest snapshot %llu -> %llu", gIdleSeen.tick0, gIdleSeen.tick1,
				(unsigned long long)gIdleSeen.snap0, (unsigned long long)gIdleSeen.snap1); }
			fprintf(stderr, ")\n");
		}
		gIdleSeen = {};
	};
	h.render = [](int slot)
	{
		const FramePacket& pk = gPackets[slot];
		CPUSTAGE_RENDER_BEGIN();
#ifdef KCLIENT_DEV_TOOLS
		if (pk.path == FramePath::kWorld) { threadsplit::split(pk.logicStart, pk.logicMs, true); }
#endif
		const auto r0 = std::chrono::steady_clock::now();
		render_frame(pk, render_state());
		if (pk.path == FramePath::kWorld)
		{
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
			framestats::fillRenderMs(ms);
		}
		sappcmd::flush();
	};
	// rule 9: while logic waits on a draw that does not come, the link is pumped (socket bytes into the decoder, HELLO / ping) and the world is not
	// stepped (SimView::pumpLink; SimView::update does the stepping). Runs only while the render thread has not taken the packet.
	h.idle = []
	{
		if (!app.sim.active()) { return; }
		app.sim.pumpLink();
		const uint64_t snap = app.sim.decoder().lastCompleteTick;
		if (gIdleSeen.polls++ == 0) { gIdleSeen.tick0 = app.sim.tick(); gIdleSeen.snap0 = snap; }
		gIdleSeen.tick1 = app.sim.tick();
		gIdleSeen.snap1 = snap;
	};
	logicthread::start(std::move(h), app.scripted());
}

}
