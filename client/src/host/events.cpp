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
#include "host/pick.hpp"
#include "host/orders.hpp"
#include "host/hud.hpp"
#include "host/script.hpp"
#include "host/logicframe.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/roadedit.hpp"
#endif
#include "host/uihost.hpp"
#include "host/events.hpp"

namespace host
{

InputQueue gInput;	// sokol_app's events, drained at the top of frame() (render-thread hook 1, inputqueue.hpp)
bool gDraining = false;	// logic thread: event() is running a queued event (queue_event stored its pointer already, newer ones may follow)
bool gFrameRan = false;	// sokol delivers the first loop's events before init(); those still go straight to event(), as before. Written and read on the sapp thread only (frame() sets it, kenshi-k16.2.205.12.23)
void queue_event(const sapp_event* e)
{
	if (e->type == SAPP_EVENTTYPE_CLIPBOARD_PASTED) { sappcmd::publishClipboard(); }	// the published copy, refreshed on sokol's thread while its buffer is valid (render hook 3)
	if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE) { pointer::set(e->mouse_x, e->mouse_y); }	// the draw's cursor, at the newest position (kenshi-k16.2.205.12.97)
	if (gFrameRan) { gInput.push(*e); }
	else { event(e); }
}

// RMB click vs look drag. Locked, X11 reports raw device counts (DPI-dependent), so a quick press forgives a jitter that
// a slow one doesn't: a click while moving the mouse to the next spot must still order (a5 2026-11-03, lost move orders).
// The creator preview's mouse (kenshi-k16.2.433.1): a left press in the preview area and the drag after it turn the assembly (creator_drag: yaw and
// tilt), a wheel step over it zooms towards the point under the cursor (creator_zoom). The panel is a UI widget that consumes its own press and wheel
// and sends only the drag's x, so the host reads the events itself first: observed, never consumed (the UI's press / hover state must keep following).
void creator_event(const sapp_event* e)
{
	if (!app.creatorUi || !app.creatorUi->shown() || !app.creator.active()) { return; }
	const ui::Rect r = app.creatorUi->previewRect();
	switch (e->type)
	{
		case SAPP_EVENTTYPE_MOUSE_DOWN:
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && r.contains(e->mouse_x, e->mouse_y))
			{
				app.creatorDragging = true;
				app.creatorDragX = e->mouse_x;
				app.creatorDragY = e->mouse_y;
			}
			break;
		case SAPP_EVENTTYPE_MOUSE_UP:
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT) { app.creatorDragging = false; }
			break;
		case SAPP_EVENTTYPE_MOUSE_MOVE:
			if (app.creatorDragging)
			{
				const float dx = e->mouse_x - app.creatorDragX, dy = e->mouse_y - app.creatorDragY;
				app.creatorDragX = e->mouse_x;
				app.creatorDragY = e->mouse_y;
				if (dx != 0 || dy != 0) { creator_drag(dx, dy); }
			}
			break;
		case SAPP_EVENTTYPE_MOUSE_SCROLL:
			if (dev::mirrorLocked()) { break; }
			if (r.contains(e->mouse_x, e->mouse_y)) { creator_zoom(e->scroll_y, e->mouse_x, e->mouse_y); }
			break;
		default: break;
	}
}

constexpr double kDragNear = 6, kDragFar = 60, kClickTime = 0.25;	// counts; seconds (MINE)
// An LMB drag on the world shows the selection box only when BOTH sides pass this: fixed pixels, not x ui::scale (the original's
// test, 1407f2c30; kenshi-rwh.134). Below it the release is the ordinary click.
constexpr double kBoxDragPx = 4;

// A debug toggle by its keys:: action name: what the key does (the key handler and the debug panel both call it).
void debug_toggle(const std::string& id)
{
	if (id == "debug.navmesh")
	{
		kNavOverlay.set(!kNavOverlay.get());
		// The overlay draws only the tiles ksim's server streams (kenshi-k16.2.527): with no server there are none, so say so once, not silently
		if (kNavOverlay.get() && !app.sim.active())
		{
			static const char* const kNote = "navmesh: no server in fly mode, use play.sh fly --server";
			fprintf(stderr, "kclient: %s\n", kNote);
			printf("%s\n", kNote);
			ui::message(kNote);
		}
	}
	else if (id == "debug.collision") { dev::collisionToggle(); }
	else if (id == "debug.collision_fill") { dev::collisionFillToggle(); }
#ifdef KCLIENT_DEV_TOOLS
	else if (id == "debug.roads") { if (app.opt.debug) { dev::roadedit::toggle(); } }
#endif
	else if (id == "debug.trails") { app.trails.toggle(); }
	else if (id == "debug.navmesh_pruned") { kNavPruned.set(!kNavPruned.get()); }
	else if (id == "debug.squad_overlay") { app.squads.on = !app.squads.on; }
	else if (id == "debug.shadows") { app.shadowFlip = !app.shadowFlip; }	// applied at the world frame's second half (logic_paths): the draw reads enabled
	else if (id == "debug.walk") { app.walk = !app.walk; }
	else if (id == "debug.follow" && app.sim.active())	// the hover / drive camera; off: back to auto-center on it
	{
		const uint64_t was = app.follow ? app.followId : 0;
		if (was) { leave_follow(); }
		else if (app.spectating) { if (app.inspect) { set_follow_id(app.inspect); } }	// a spectator has no selection: the watch is what Ctrl+C follows
		else { set_follow(true); }
	}
	else if (id == "debug.free_camera")
	{
		if (app.rts || app.follow) { free_camera(); }
		else { set_rts(0); }
	}
	else if (id == "debug.autorun" && app.follow) { app.autorun = !app.autorun; }	// UNSURE: the original's autorun key
	else if (id == "debug.clock_pause") { app.sky.clock.paused = !app.sky.clock.paused; }
#ifdef KCLIENT_DEV_TOOLS
	else if (id == "debug.height_probe") { dev::heightProbeToggle(); }	// the panel's toggle row: the same switch as Ctrl+F10 (kenshi-k16.2.413.1)
#endif
}

#ifdef KCLIENT_DEBUG_VIEW
// With the squad overlay on: the live squad whose pillar is under (x, y) (framebuffer pixels), 0 none. MINE: 12 px from the pillar's axis.
static uint64_t squad_at(float x, float y)
{
	uint64_t best = 0;
	double bestD = 1e30;
	for (const Squads::Dot& d : app.squads.dots())
	{
		if (!d.id) { continue; }
		const double base[3] = { d.x, d.y, d.z };
		const double dist = std::sqrt((base[0] - app.pos[0]) * (base[0] - app.pos[0]) + (base[1] - app.pos[1]) * (base[1] - app.pos[1]) + (base[2] - app.pos[2]) * (base[2] - app.pos[2]));
		const double top[3] = { d.x, d.y + std::max(60.0, dist * 0.03), d.z };	// shaders/squads.glsl's pillar height
		float ax, ay, bx, by;
		if (!world_to_screen(base, ax, ay) || !world_to_screen(top, bx, by)) { continue; }
		const float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
		const float u = len2 > 0 ? std::clamp(((x - ax) * vx + (y - ay) * vy) / len2, 0.0f, 1.0f) : 0.0f;
		const float px = ax + vx * u - x, py = ay + vy * u - y;
		if (px * px + py * py <= 144 && dist < bestD) { bestD = dist; best = d.id; }
	}
	return best;
}
#endif

// The bound actions of a key press (ui::keys, `key.*` in kclient.cfg; docs/UI.md "Keys"). Game actions without a client side yet do nothing.

static void key_actions(const sapp_event& e)
{
	// While the free camera flies, a press that is one of its fly keys (R / F / Space / C may be game keys too) belongs to the camera alone: no floor
	// step, pause or stats window (kenshi-wlql). The fly actions themselves are held, read each frame by update_camera.
	const bool spaceGame = !fly_owns_space() && e.key_code == SAPP_KEYCODE_SPACE;	// a game's Space stays pause
	const bool flyKey = free_flying() && !spaceGame && (ui::keys::pressed(e, "debug.fly_up") || ui::keys::pressed(e, "debug.fly_down") || ui::keys::pressed(e, "debug.fly_fast"));
	const auto pressed = [&](const std::string& action) { return ui::keys::pressed(e, action) && (!flyKey || action.starts_with("debug.fly_")); };
	if (app.scripted())	// tests/flycam: which of the shared game keys the fly keys took (offline there is no sim to pause)
	{
		for (const char* a : { "pause", "floor_up", "floor_down", "toggle_stats" }) { if (ui::keys::pressed(e, a)) { printf("flykey: %s %s\n", a, flyKey ? "swallowed" : "fires"); } }
	}
	// Follow mode (kenshi-cym.40.2.9): every key that sends an order or changes the selection or the camera is locked with the mouse (dev keys and the F8 panel are not).
	// A locked press is logged once, so a run shows it was seen and refused.
	const bool mirror = dev::mirrorLocked();
	const auto open = [&](const std::string& action)
	{
		if (!pressed(action)) { return false; }
		if (mirror) { printf("follow: key %s locked (mirror lock)\n", action.c_str()); return false; }
		return true;
	};
#ifdef KCLIENT_DEV_TOOLS
	if (app.opt.debug && e.type == SAPP_EVENTTYPE_KEY_DOWN && !e.key_repeat && e.key_code == SAPP_KEYCODE_F10 && e.modifiers == SAPP_MODIFIER_CTRL) { dev::heightProbeToggle(); }	// Ctrl+F10: the vertical height probe mode (dev/heightprobe.cpp)
#endif
#ifdef KCLIENT_DEV_TOOLS
	if (app.opt.debug && dev::roadedit::on())	// the road editor's keys act only while its overlay is on (kenshi-ix9r)
	{
		if (pressed("debug.road_next")) { roads_step(1); }
		if (pressed("debug.road_prev")) { roads_step(-1); }
		if (pressed("debug.road_mark_ok")) { dev::roadedit::mark(ui::kRoadOk); }	// kenshi-tqtd: marks kept in memory per (road, marker)
		if (pressed("debug.road_mark_unsure")) { dev::roadedit::mark(ui::kRoadUnsure); }
		if (pressed("debug.road_mark_bad")) { dev::roadedit::mark(ui::kRoadBad); }
	}
#endif
	if (pressed("debug.terrain_view") && app.terrain.hasColour) { app.terrain.viewMode = (app.terrain.viewMode + 1) % (app.terrain.detail.ok ? 4 : 3); }
	for (const char* id : { "debug.squad_overlay", "debug.navmesh", "debug.collision", "debug.roads", "debug.trails", "debug.shadows", "debug.walk", "debug.free_camera", "debug.autorun", "debug.clock_pause" }) { if (pressed(id)) { debug_toggle(id); } }
#ifdef KCLIENT_DEV_TOOLS
	if (pressed("debug.trails_panel")) { dev::toggleTrailsPanel(); }
	if (pressed("debug.lights_panel") && app.lightsPanel) { app.lightsPanel->show(!app.lightsPanel->shown()); }
#endif
	if (open("camera_drive")) { debug_toggle("debug.follow"); }
	if (open("debug.follow")) { debug_toggle("debug.follow"); }
	if (open("cycle_run_speed")) { app.sim.setToggle(5, (uint8_t)((move_speed() + 1) % 4)); }	// the original's Num6: the speed diamond's next mode
	if (app.foliageOn && (pressed("debug.foliage_more") || pressed("debug.foliage_less")))
	{
		static const double steps[] = { 0.5, 1, 2, 4, 6, 8, 12, 16 };
		int at = 0;
		for (int i = 0; i < 8; i++) { if (steps[i] <= app.foliage.foliageRange * 1.001) { at = i; } }
		app.foliage.foliageRange = steps[std::clamp(at + (pressed("debug.foliage_more") ? 1 : -1), 0, 7)];
	}
	// Our floor keys are R / F where the original's are PgUp / PgDn: Change_Level on the change (1407210a0 / 1407210f0), as the HUD's floor arrows
	if (open("floor_up")) { if (floor_step(1)) { post_event(audio::kChangeLevel, "Change_Level"); } }
	if (open("floor_down")) { if (floor_step(-1)) { post_event(audio::kChangeLevel, "Change_Level"); } }
	// the sneak key (X) is the SNEAK toggle: Stealth turning sneak on, General_Click turning it off (140721410)
	if (open("toggle_sneak") && app.sim.active())
	{
		const bool before = app.sim.sneakOn();
		const bool sent = app.sim.sneak();
		printf("sneak: %s\n", sent ? (before ? "off" : "on") : "not sent");
		if (sent) { if (before) { post_event(audio::kGeneralClick, "General_Click"); } else { post_event(audio::kStealth, "Stealth"); } }
	}
	if (pressed("debug.clock_speed")) { app.sky.clock.cycleSpeed(); app.sky.clock.paused = false; }
	if (pressed("debug.clock_back")) { app.sky.clock.nudge(-0.5); }
	if (pressed("debug.clock_forward")) { app.sky.clock.nudge(0.5); }
	if (pressed("debug.clock_back_far")) { app.sky.clock.nudge(-3.0); }
	if (pressed("debug.clock_forward_far")) { app.sky.clock.nudge(3.0); }
	if (pressed("debug.help"))	// the debug panel; the text key list only without the UI
	{
#ifdef KCLIENT_DEV_TOOLS
		if (!dev::toggleDebugPanel()) { app.help = !app.help; }
#else
		app.help = !app.help;
#endif
	}
	// the playtest notes: closed -> open with the caret in the box; open -> the caret; typing -> close (and save)
#ifdef KCLIENT_DEV_TOOLS
	if (app.opt.debug && pressed("playtest.notes")) { dev::notesKey(false); }
#endif
	for (const char* a : { "toggle_options", "toggle_inventory", "toggle_stats", "toggle_squads", "toggle_map", "toggle_faction", "toggle_research", "toggle_crafting", "toggle_help", "prospect", "toggle_build", "quicksave" }) { if (pressed(a)) { shortcut(a); } }
	if (open("quickload")) { shortcut("quickload"); }	// a load drops the world: locked under the mirror
	if (pressed("debug.weather_prev") || pressed("debug.weather_next"))
	{
		app.weatherCycle += pressed("debug.weather_next") ? 1 : -1;
		ensure_weathers();
		apply_weather();
	}
	if (pressed("debug.full_map")) { app.wmap.full = !app.wmap.full; }
	if (pressed("toggle_bar")) { app.hideUi = !app.hideUi; }
	if (pressed("screenshot")) { app.shotPending = true; }
	if (pressed("screenshot_delayed"))
	{
		constexpr double kShotDelay = 3;	// MINE: seconds
		app.shotAt = ui::now() + kShotDelay;
		ui::toast("Screenshot in 3 seconds", ui::style().colour("Main", {}), kShotDelay - 1);	// gone before the shot
	}
	if (pressed("debug.mouse_look")) { app.look = !app.look; app.lookDrag = true; sappcmd::lockMouse(app.look); }
	if (!app.sim.active()) { return; }
	if (mirror)	// pause, the speed keys, stop, the selection keys and the focus key: the stream owns them (a locked press is logged)
	{
		for (const char* a : { "pause", "speed_1", "speed_2", "speed_3", "stop_movement", "focus_char", "select_all", "character_next", "character_prev", "select_0", "select_1", "select_2", "select_3", "select_4", "select_5", "select_6", "select_7", "select_8", "select_9" }) { if (pressed(a)) { printf("follow: key %s locked (mirror lock)\n", a); } }
		return;
	}
	// the sim's game speed (orders 40, 41) and the selection (the HUD bar's characters, in its order)
	if (pressed("pause")) { app.sim.pause(!(app.sim.clock().pauseBits & 1)); }	// order 40 is bit 0 only; the dialogue freeze (bit 1) lifts with an answer
	const float speeds[3] = { ui::Hud::kSpeeds[0], ui::Hud::kSpeeds[1], ui::Hud::kSpeeds[2] };
	for (int i = 0; i < 3; i++) { if (pressed("speed_" + std::to_string(i + 1))) { if (app.sim.setSpeed(speeds[i])) { audiohook_event(speed_event(speeds[i])); } app.sim.pause(false); } }
	if (pressed("stop_movement")) { app.sim.stop(false); }
	// focus_char (Z): auto-center on the selection's first (WASD ends it)
	if (pressed("focus_char") && !app.sim.selected().empty()) { set_rts(app.sim.selected().front()); }
	if (pressed("select_all"))
	{
		app.inspect = 0;
		app.panelProducer = 0;	// the focus returns to the selection (k16.19.8.2)
		for (size_t i = 0; i < app.bar.size(); i++) { app.sim.select(app.bar[i], i > 0); }
		follow_retarget();
	}
	for (int n = 0; n < 10; n++)
	{
		const size_t i = n;	// select_0 = the first (key 1) .. select_9 = the tenth (key 0), as controls.cfg
		if (!pressed("select_" + std::to_string(n)) || i >= app.bar.size()) { continue; }
		// the same number twice = focus (MINE: within 0.4 s)
		const bool twice = app.lastSelect == app.bar[i] && ui::now() - app.lastSelectAt < 0.4;
		app.inspect = 0;
		app.panelProducer = 0;	// the focus returns to the selection (k16.19.8.2)
		app.sim.select(app.bar[i], false);
		follow_retarget();
		if (twice) { set_rts(app.bar[i]); }
		app.lastSelect = app.bar[i];
		app.lastSelectAt = ui::now();
	}
	if ((pressed("character_next") || pressed("character_prev")) && !app.bar.empty())
	{
		const auto& sel = app.sim.selected();
		const auto at = sel.empty() ? app.bar.end() : std::find(app.bar.begin(), app.bar.end(), sel.front());
		const int n = (int)app.bar.size(), cur = at == app.bar.end() ? -1 : (int)(at - app.bar.begin());
		app.panelProducer = 0;	// a portrait step: the panel goes back to the character (k16.19.8.2)
		app.sim.select(app.bar[((pressed("character_next") ? cur + 1 : cur < 0 ? -1 : cur - 1) + n) % n], false);
		follow_retarget();
	}
}

// Selection box: on release, every player character whose position point (the wire position, its feet) projects inside the rectangle AND lies
// within 7500 u of the camera replaces the selection (Shift adds); dead characters are left out (KO'd ones are boxed), occlusion is not tested and
// a box that catches nothing leaves the selection as it was. The focus ends on the last one caught, as the original's order of selection
// (kenshi-rwh.134). Returns how many were caught.
int box_select(std::vector<uint64_t>& in)
{
	constexpr double kReach = 7500.0;	// the original's squared-distance gate, 5.625e7 (kenshi-rwh.134)
	const float x0 = (float)std::min(app.boxX0, app.boxX1), x1 = (float)std::max(app.boxX0, app.boxX1);
	const float y0 = (float)std::min(app.boxY0, app.boxY1), y1 = (float)std::max(app.boxY0, app.boxY1);
	in.clear();
	for (const auto& [id, ch] : app.sim.decoder().chars)
	{
		double w[3];
		float sx, sy;
		if (!app.sim.isPlayer(id) || (ch.cur.flags & simlink::kDead) || !app.sim.simPos(id, w)) { continue; }
		const double dx = w[0] - app.pos[0], dy = w[1] - app.pos[1], dz = w[2] - app.pos[2];
		if (dx * dx + dy * dy + dz * dz > kReach * kReach) { continue; }
		if (world_to_screen(w, sx, sy) && sx >= x0 && sx <= x1 && sy >= y0 && sy <= y1) { in.push_back(id); }
	}
	std::sort(in.begin(), in.end());
	for (size_t i = 0; i < in.size(); i++) { app.sim.select(in[i], app.boxShift || i > 0); }
	follow_retarget();
	if (!in.empty()) { app.panelProducer = 0; app.inspect = in.back(); }
	return (int)in.size();
}

// Ahead of ui::event, so a release over a window still ends the box.
void box_event(const sapp_event* e)
{
	if (!app.boxDown) { return; }
	// a right press mid-drag does not end the box: event() swallows it (the original ignores it; kenshi-rwh.134). A lost focus, the look mode and an
	// orbit (the middle button's grip) still cancel it.
	if (e->type == SAPP_EVENTTYPE_UNFOCUSED || app.look || app.orbit) { app.boxDown = app.boxDrag = false; return; }
	if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE)
	{
		app.boxX1 = e->mouse_x;
		app.boxY1 = e->mouse_y;
		// both sides past 4 px: the box shows, and stays for the rest of the drag (the original's test; kenshi-rwh.134)
		if (!app.boxDrag && std::fabs(app.boxX1 - app.boxX0) > kBoxDragPx && std::fabs(app.boxY1 - app.boxY0) > kBoxDragPx) { app.boxDrag = true; }
	}
	else if (e->type == SAPP_EVENTTYPE_MOUSE_UP && e->mouse_button == SAPP_MOUSEBUTTON_LEFT)
	{
		const bool drag = app.boxDrag;
		app.boxDown = app.boxDrag = false;
		if (!drag) { sim_click(app.boxX0, app.boxY0, false, app.boxShift); return; }
		app.boxX1 = e->mouse_x;
		app.boxY1 = e->mouse_y;
		std::vector<uint64_t> caught;
		const int n = box_select(caught);
		// key=value so a run's check can read it (client/tests/boxselect/check.awk): the caught in the order they were selected, then the whole selection
		printf("box select frame=%llu rect=%.0f,%.0f,%.0f,%.0f shift=%d caught=%d focus=%llu order=", (unsigned long long)app.frames, app.boxX0, app.boxY0, app.boxX1, app.boxY1,
			app.boxShift ? 1 : 0, n, (unsigned long long)app.inspect);
		for (size_t i = 0; i < caught.size(); i++) { printf("%s%llu", i ? "," : "", (unsigned long long)caught[i]); }
		printf(" selected=");
		const auto& sel = app.sim.selected();
		for (size_t i = 0; i < sel.size(); i++) { printf("%s%llu", i ? "," : "", (unsigned long long)sel[i]); }
		printf("\n");
	}
}

void event(const sapp_event* e)
{
	if (app.loading() && !dev::viewing())
	{
		if (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code == SAPP_KEYCODE_ESCAPE) { request_quit(); }
		return;
	}
	if (app.sessionLoad && (e->type == SAPP_EVENTTYPE_KEY_DOWN || e->type == SAPP_EVENTTYPE_CHAR || e->type == SAPP_EVENTTYPE_MOUSE_DOWN || e->type == SAPP_EVENTTYPE_MOUSE_SCROLL)) { return; }	// the releases pass: nothing stays held
	// The game's held keys: a release is tracked before the UI sees it, since a focused edit box consumes every key's release too (onKey returns
	// true) and a Ctrl typed in one would otherwise stay held here for good. A press is tracked only when no window takes it (below), so typing
	// W in an edit box does not pan the camera.
	if (e->type == SAPP_EVENTTYPE_KEY_UP && (int)e->key_code < SAPP_MAX_KEYCODES) { app.keys[e->key_code] = false; }
	if (!dev::mirrorLocked()) { box_event(e); }	// follow mode: no selection box
	creator_event(e);	// the creator preview's drag and wheel (k16.2.433.1), before ui::event: its panel consumes them and sends x only
	// Mid-drag a right press is ignored, as the original's (kenshi-rwh.134): no camera grip, no hold menu, no window, no order on its release; the
	// box keeps going. Before ui::event and the switch, so the press never reaches either.
	if (app.boxDown && e->type == SAPP_EVENTTYPE_MOUSE_DOWN && e->mouse_button == SAPP_MOUSEBUTTON_RIGHT) { return; }
	if (!app.uiOn || app.hideUi) { ui::keys::track(*e); }	// ui::event tracks the held keys itself
	if (app.uiOn && !app.hideUi && ui::event(*e)) { return; }
	switch (e->type)
	{
		case SAPP_EVENTTYPE_KEY_DOWN:
			// Ctrl+F with the Options window shown and the world holding the keyboard (no window took it): the window's Find field, before any world
			// binding (kenshi-k16.2.227.2.1). Closed Options: nothing here; Ctrl+F is bound to nothing (the floor key is a plain F).
			if (e->key_code == SAPP_KEYCODE_F && (e->modifiers & SAPP_MODIFIER_CTRL) && app.uiOn && !app.hideUi && app.options && app.options->shown() && app.options->search())
			{
				if (!e->key_repeat)
				{
					ui::focus(app.options->search());
					app.options->search()->select(0, app.options->search()->text.size());
				}
				break;
			}
			if ((int)e->key_code < SAPP_MAX_KEYCODES) { app.keys[e->key_code] = true; }
			if (e->key_repeat) { break; }
			if (e->key_code == SAPP_KEYCODE_ESCAPE && !app.uiOn) { request_quit(); }	// without the UI there is no menu
			else if (e->key_code == SAPP_KEYCODE_ESCAPE && !title_shown())
			{
				// no window took it (ui::event): Esc leaves direct drive first (k16.2.238.1), else the in-game menu
				if (app.follow && app.sim.active()) { leave_follow(); }
				else { app.hideUi = false; app.gameMenu->toggle(); }
			}
			if (!menu_shown()) { key_actions(*e); }
			else if (ui::keys::pressed(*e, "screenshot")) { app.shotPending = true; }	// the shot key works over the title and the menus too (k16.2.146)
			break;
		case SAPP_EVENTTYPE_MOUSE_DOWN:
			if (menu_shown() || dev::mirrorLocked()) { break; }	// follow mode (kenshi-cym.40.2.5): the player's clicks, look and orbit are locked	// a click no window took, under the title, the creator or a full-screen menu: no look, pick, selection box or order (kenshi-k16.2.472.1)
			if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
			{
				app.look = true;
				sappcmd::lockMouse(true);
				app.clickX = app.mouseX;
				app.clickY = app.mouseY;
				app.clickDrag = 0;
				app.clickAt = ui::now();
				app.clickPend[0] = app.clickPend[1] = 0;
				app.lookDrag = false;
				app.rmbAt = app.uiOn && app.sim.active() && !app.wmap.full ? ui::now() : -1;
				// a locked door's (or a prisoner's) menu opens on the press, without the hold (ctx18_static.md; Shift keeps the hold)
				if (app.rmbAt >= 0 && !(e->modifiers & SAPP_MODIFIER_SHIFT))
				{
					const Pick p = pick_screen(app.clickX, app.clickY);
					const simlink::Interactable* t = p.kind == Pick::Door ? app.sim.thing(p.id) : nullptr;
					if ((t && t->state == simlink::Interactable::kLocked) || (p.kind == Pick::Character && locked_cage(app.sim, p.id))) { app.rmbAt -= kHoldMenu; }	// a prisoner's two menus too (k16.2.105.1)
				}
			}
			if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE) { app.orbit = true; sappcmd::lockMouse(true); }
#ifdef KCLIENT_DEV_TOOLS
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.opt.debug && dev::roadedit::on()) { dev::roadedit::click(); }	// the road editor: a handle is selected, empty ground clears; never an order (kenshi-tqtd)
			else
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.opt.debug && dev::heightProbeMode())
			{
				const Pick p = pick_screen(e->mouse_x, e->mouse_y);	// the vertical height probe (Ctrl+F10): the click sets the probe, nothing else happens
				if (p.kind != Pick::None) { dev::heightProbeSet(dev_context(), p.hit[0], p.hit[2], false); }
			}
			else
#endif
#ifdef KCLIENT_DEBUG_VIEW
			if (const uint64_t pillar = e->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.squads.on && !app.wmap.full && app.sim.active() ? squad_at(e->mouse_x, e->mouse_y) : 0)
			{
				debug_tree_open(0, pillar);	// a squad pillar: its tree, not a ground click
			}
			else
#endif
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && !app.wmap.full && app.sim.active())
			{
				// ground and characters wait for the release (a click, or the start of a selection box); everything else acts on the press as before
				const Pick p = app.look || app.orbit || held("mouse_rotate") || label_at(e->mouse_x, e->mouse_y) ? Pick{} : pick_screen(e->mouse_x, e->mouse_y);
				if (p.kind == Pick::Ground || p.kind == Pick::Character)
				{
					app.boxDown = true;
					app.boxDrag = false;
					app.boxShift = e->modifiers & SAPP_MODIFIER_SHIFT;
					app.boxX0 = app.boxX1 = e->mouse_x;
					app.boxY0 = app.boxY1 = e->mouse_y;
				}
				else { sim_click(e->mouse_x, e->mouse_y, false, e->modifiers & SAPP_MODIFIER_SHIFT); }
			}
			if (e->mouse_button == SAPP_MOUSEBUTTON_LEFT && app.wmap.full)
			{
				double x, z;
				if (app.wmap.fullToWorld(e->mouse_x, e->mouse_y, x, z))
				{
					teleport(x, z);
					printf("teleport: click (%.0f, %.0f) -> world x %.0f z %.0f, camera now x %.1f y %.1f z %.1f\n", e->mouse_x, e->mouse_y, x, z, app.pos[0], app.pos[1], app.pos[2]);
				}
			}
			break;
		case SAPP_EVENTTYPE_MOUSE_UP:
			if (dev::mirrorLocked() && !app.look && !app.orbit) { break; }
			if (e->mouse_button == SAPP_MOUSEBUTTON_MIDDLE && app.orbit) { app.orbit = false; sappcmd::lockMouse(app.look); }
			if (e->mouse_button == SAPP_MOUSEBUTTON_RIGHT && app.look)	// not after the hold menu took over the button
			{
				app.look = false;
				sappcmd::lockMouse(false);
				// a menu_query still waiting (k16.2.121): the release is that right-click menu's, not a default click; it opens on the answer
				if (!app.lookDrag && !app.wmap.full && app.sim.active() && !menuPending.on) { sim_click(app.clickX, app.clickY, true, e->modifiers & SAPP_MODIFIER_SHIFT); }
			}
			break;
		case SAPP_EVENTTYPE_MOUSE_MOVE:
			app.mouseX = e->mouse_x;
			app.mouseY = e->mouse_y;
			if (!gDraining) { pointer::set(e->mouse_x, e->mouse_y); }	// a script's injected move (queued ones were stored by queue_event, newer ones must not be overwritten)
		{
			double dx = e->mouse_dx, dy = e->mouse_dy;
			if (app.look && !app.lookDrag)	// a press not yet a drag: the camera holds still, so the click picks where it went down
			{
				app.clickDrag += std::fabs(dx) + std::fabs(dy);
				app.clickPend[0] += dx;
				app.clickPend[1] += dy;
				app.lookDrag = app.clickDrag >= kDragFar || (app.clickDrag >= kDragNear && ui::now() - app.clickAt >= kClickTime);
				if (!app.lookDrag) { break; }
				dx = app.clickPend[0];
				dy = app.clickPend[1];
			}
			if ((app.look || app.orbit || held("mouse_rotate")) && !dev::mirrorLocked())	// the floating camera clamps the pitch (update_rts)
			{
				if (app.follow && !driving()) { app.followOffset += dx * 0.0025; }	// orbit; while driving, mouse look steers
				else { app.yaw += dx * 0.0025; }
				app.lookDx += dx;
				app.pitch = std::fmax(-1.55, std::fmin(1.55, app.pitch - dy * 0.0025));
			}
		}
			break;
		case SAPP_EVENTTYPE_MOUSE_SCROLL:
			// kenshi-k16.2.237.5 (user 2026-11-14: any zoom is the 'disable cruise control'): a wheel step in direct drive on a player character leaves the drive
			// (the RTS camera on the same character, WASD move the camera again) and is then an ordinary RTS zoom, below. A spectator's follow keeps its zoom.
			if (!dev::viewing() && app.follow && e->scroll_y != 0 && app.sim.isPlayer(app.followId)) { leave_follow(); }
			if (dev::viewing()) { dev::viewScroll(e->scroll_y); }	// the viewer's orbit distance (dev/viewmesh.cpp)
			else if (app.follow) { app.followDist = std::clamp(app.followDist * std::pow(0.9, e->scroll_y), 10.0, 2000.0); }	// the original's zoom clamp
			else if (app.rts)	// kinspect camera_rts.md: x0.75 / x1.25 a notch below 450, +-112.5 above
			{
				for (int n = (int)std::lround(std::fabs(e->scroll_y)); n > 0; n--)
				{
					const bool in = e->scroll_y > 0;
					app.rtsDist = app.rtsDist < 450 ? app.rtsDist * (in ? 0.75 : 1.25) : app.rtsDist + (in ? -112.5 : 112.5);
					app.rtsDist = std::clamp(app.rtsDist, 10.0, 2000.0);
				}
			}
			else { app.speedScale *= std::pow(1.2, e->scroll_y); }
			break;
		case SAPP_EVENTTYPE_UNFOCUSED:
			memset(app.keys, 0, sizeof app.keys);
			app.look = false;
			app.orbit = false;
			break;
		default:
			break;
	}
}
}
