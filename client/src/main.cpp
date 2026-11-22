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
#ifdef KCLIENT_DEV_TOOLS
#include "dev/devpaths.hpp"
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
#include "dev/pacelog.hpp"
#include "app/realfps.hpp"
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
#include "app/userdirs.hpp"	// plat::userDataDir: the screenshot / camera / crash folders (kenshi-379.31.3)
#include "platform/datasetting.hpp"
#include "platform/windowgeom.hpp"
#include "platform/process.hpp"	// plat::unsetEnv
#include "platform/windowgate.hpp"
#include "platform/swapinfo.hpp"
#include "assets/imagewrite.hpp"
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
#include "platform/console.hpp"
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
#include "host/session.hpp"
#include "host/uihost.hpp"
#include "host/panels.hpp"
#include "host/events.hpp"

namespace host
{

#ifdef KCLIENT_DEBUG_VIEW
static std::string tree_state_value(uint8_t kind, uint64_t id, const std::string& key, const std::string& value);
#endif
constexpr double kScriptDt = 1.0 / 60.0;	// fixed timestep while scripting

// TODO: units per metre is unknown (CLIENT.md "Open questions"); the coordinator's guess is 10 units/m.
constexpr double kEyeHeight = 17.0;		// walk mode eye height above ground, world units
constexpr double kWalkSpeed = 15.0;		// world units / s
constexpr double kFlySpeed = 400.0;		// world units / s
constexpr int kScreenshotFrames = 3;		// --screenshot: frame at which the shot is taken
constexpr double kStallMs = 250;	// a frame gap over this is an evlog `stall` (kenshi-k16.2.26: the loading screen froze the window)
constexpr int kLoadsPerFrame = 24;
// A live session with no camera target this long falls back to the Hub default and says so (kenshi-k16.2.112.1, user 2026-11-12).
constexpr double kCameraWaitSeconds = 10.0;

int g_sgErrors = 0;	// sokol-gfx errors and panics logged so far (a message-less [sg][error] means a pool ran out)
int g_exitCode = 0;
std::chrono::steady_clock::time_point g_quitAsked;	// the first quit request: cleanup() logs the time from here to the server's stop (kenshi-379.12)
void request_quit()
{
	if (g_quitAsked == std::chrono::steady_clock::time_point{}) { g_quitAsked = std::chrono::steady_clock::now(); }
	sappcmd::requestQuit();
}

void log_cb(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char* file, void* user)
{
	if (level <= 1 && tag && !strcmp(tag, "sg")) { g_sgErrors++; }
	slog_func(tag, level, item, message, line, file, user);
}

App app;

// _exit with no other cleanup, but the local server child ends first: left alive it holds the stdout/stderr pipes open and whatever waits on kclient hangs (kenshi-k16.2.271).
[[noreturn]] void exit_now(int code)
{
	readback::flushWrites();	// a screenshot still on the writer thread (the load shot's done ends here)
	// The writes' done callbacks and the --bead shot drain touch logic-thread state (dev's shotBeads): only when the logic thread is not
	// running or this is it. exit_now() on any other thread with logic running skips them (the file is flushed above; its copy and comment
	// are lost, as before kenshi-1dpr), never racing the logic thread's own runWrites / shotFrame.
	if (!logicthread::running() || logicthread::onLogicThread())
	{
		readback::runWrites();
		dev::shotDrain();
	}
	app.server.stop(false);	// SIGKILL: no handler race, no unbounded wait (kenshi-k16.2.271.1)
	fflush(stdout);	// _exit skips the C library's exit flush: a line half written (kenshi-k16.2.461; the lines are already out, platform/posix/console.cpp)
	fflush(stderr);
	_exit(code);
}

// One of the game's own events that the host posts (audio/events.hpp): its id and the name beside it for the audio thread's log. The name-based
// audiohook_event() hashes a std::string; this keeps the named constant at the call.
void post_event(uint32_t id, const char* name) { audiohook_ui({ id, 0, 0, name, {}, {} }); }

void forward_vec(double yaw, double pitch, double f[3])
{
	f[0] = std::sin(yaw) * std::cos(pitch);
	f[1] = std::sin(pitch);
	f[2] = -std::cos(yaw) * std::cos(pitch);
}

// the game's main camera: FOVy 45 deg (live frustum, kenshi-inspect presets)
constexpr float kFovY = 45.0f * 3.14159265f / 180.0f;

// Camera forward / right / up unit vectors in world axes (+y up, right = forward x up, so yaw 0 looks toward -z).
void camera_basis(double f[3], double r[3], double u[3])
{
	forward_vec(app.yaw, app.pitch, f);
	r[0] = -f[2]; r[1] = 0; r[2] = f[0];
	const double rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
	r[0] /= rl; r[2] /= rl;
	u[0] = r[1] * f[2] - r[2] * f[1];
	u[1] = r[2] * f[0] - r[0] * f[2];
	u[2] = r[0] * f[1] - r[1] * f[0];
}

// The unit world ray through framebuffer pixel (mx, my) from the camera position.
void mouse_ray(double mx, double my, double dir[3])
{
	double f[3], r[3], u[3];
	camera_basis(f, r, u);
	// Pixels are the window's (the pointer's units); the projection is the applied size's, whose image the composite scales onto the window (k16.2.222).
	const double t = std::tan(kFovY * 0.5), sx = (2 * mx / view::widthf() - 1) * t * windowgate::aspect(), sy = (1 - 2 * my / view::heightf()) * t;
	double l = 0;
	for (int k = 0; k < 3; k++) { dir[k] = f[k] + r[k] * sx + u[k] * sy; l += dir[k] * dir[k]; }
	for (int k = 0; k < 3; k++) { dir[k] /= std::sqrt(l); }
}

// Framebuffer pixel of world point p (the inverse of mouse_ray); false behind the camera.
bool world_to_screen(const double p[3], float& x, float& y)
{
	double f[3], r[3], u[3];
	camera_basis(f, r, u);
	double d[3], z = 0, sx = 0, sy = 0;
	for (int k = 0; k < 3; k++) { d[k] = p[k] - app.pos[k]; z += d[k] * f[k]; }
	if (z <= 0.01) { return false; }
	for (int k = 0; k < 3; k++) { sx += d[k] * r[k]; sy += d[k] * u[k]; }
	const double t = std::tan(kFovY * 0.5), aspect = windowgate::aspect();	// the projection's aspect (the applied size); the pixels are the window's (k16.2.222)
	x = (float)((sx / z / (t * aspect) + 1) * 0.5 * view::widthf());
	y = (float)((1 - sy / z / t) * 0.5 * view::heightf());
	return true;
}

// The player characters' name labels (the original's CharacterNameTag, kenshi-inspect s54_static.md 3): every player-faction character not dead
// (UNSURE: the original's tag on a corpse), at the feet + hull height + 5 (body scale 1: not on the wire), within ~10 950 units of the camera;
// none while F7 hides the interface. Sneaking shows the stealth icon as "hidden" and the light icon at full light until the sim sends detection and
// the light level (not on the wire yet).
const cfg::Bool kShowNames{ cfg::Group{ "HUD and labels" }, "ui.show_names", true, "Show character names: the player characters' names over their heads (the original's \"Show names\")" };
void name_labels_frame()
{
	std::vector<ui::NameLabel> labels;
	if (app.sim.active() && !app.hideUi && !app.creatorUi->shown())	// none over the creator's preview (k16.17.5)
	{
		const std::vector<uint64_t>& sel = app.sim.selected();
		for (const auto& [id, ch] : app.sim.decoder().chars)
		{
			double pos[3];
			float hull[3];
			if (!(ch.cur.flags & simlink::kPlayer) || (ch.cur.flags & simlink::kDead) || app.sim.speech().count(id) || !app.sim.placed(id, pos, hull)) { continue; }	// a speaker's tag hides while it talks (speech_bubble.md 1)
			if (std::hypot(pos[0] - app.pos[0], pos[1] - app.pos[1], pos[2] - app.pos[2]) > 10950) { continue; }
			if (char_cut_hidden(id, pos)) { continue; }	// the character is not drawn (a floor the cut hides): no label over nothing (kenshi-k16.2.264.1)
			pos[1] += hull[1] + 5;
			ui::NameLabel l;
			if (!world_to_screen(pos, l.x, l.y)) { continue; }
			l.id = id;
			l.name = app.sim.decoder().str(ch.look.name);
			l.selected = std::find(sel.begin(), sel.end(), id) != sel.end();
			const simlink::Detect* d = app.sim.detect(id);	// the sneak icon's tint (the original shows it only in sneak mode; Discovered! on the HUD otherwise)
			l.stealth = !(ch.cur.flags & simlink::kStealth) ? 0 : !d ? 1 : d->state == simlink::Detect::kNoticed ? 3 : 2;
			labels.push_back(std::move(l));
		}
	}
	app.nameLabels->update(std::move(labels), kShowNames.get());
}

// Speech bubbles (SIMLINK event 41; kenshi-inspect speech_bubble.md): each speaker's line, its bottom centre on Bip01 Head + 4 up (the idle stance's
// head height, cached per speaker; 3 u up for one lying down; a hull estimate when the body has no head bone), re-projected every frame. Alpha
// saturate((2500 - dist x factor) / 1500) from the camera: factor 0.5 for a large line (flags bit 3: the large font, clamped to the screen), 0.75 for an
// important one (bit 4, also clamped; a large line is important too). F7 does not hide them (the original never reads its flag, M).
void speech_frame()
{
	std::vector<ui::SpeechBubble> bubbles;
	std::erase_if(app.speechHead, [](const auto& kv) { return !app.sim.speech().count(kv.first); });
	for (const auto& [id, sp] : app.sim.speech())
	{
		const auto ch = app.sim.decoder().chars.find(id);
		double at[3];
		float hull[3];
		if (ch == app.sim.decoder().chars.end() || !app.sim.placed(id, at, hull)) { continue; }
		if (!app.speechHead.count(id))
		{
			const int i = app.sim.charIndex(id);
			const float head = i >= 0 ? app.chars.headHeight((size_t)i) : 0;
			app.speechHead[id] = head > 0 ? head : hull[1] * 0.85f;
		}
		at[1] += (ch->second.cur.flags & (simlink::kKO | simlink::kDead | simlink::kProne) ? 3 : app.speechHead[id]) + 4;
		ui::SpeechBubble b;
		b.large = sp.flags & 8;
		b.important = sp.flags & (8 | 16);
		const float factor = b.large ? 0.5f : b.important ? 0.75f : 1.0f;
		b.alpha = std::clamp((2500 - factor * (float)std::hypot(at[0] - app.pos[0], at[1] - app.pos[1], at[2] - app.pos[2])) / 1500, 0.0f, 1.0f);
		if (b.alpha <= 0 || !world_to_screen(at, b.x, b.y)) { continue; }
		b.id = id;
		b.text = app.sim.decoder().str(sp.text);
		bubbles.push_back(std::move(b));
	}
	app.speechBubbles->update(std::move(bubbles));
}

// The conversation screen (ui::Dialogue) over the newest open REPLY_MENU: the speaker's newest line above our char's options ("[...]" for the
// continue entry). A choice sends order 88, Leave order 89; the menu closes on the answer's speech or event 44.
void dialogue_frame()
{
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	for (const auto& [id, sp] : app.sim.speech()) { app.lastLine[id] = sp.text; }
	std::erase_if(app.lastLine, [&](const auto& kv) { return !d.chars.count(kv.first); });
	ui::DialogueState s;
	if (!d.menus.empty())
	{
		const auto& m = d.menus.back();
		const auto portrait = [&](uint64_t id, ui::PortraitRef& out)
		{
			const auto c = d.chars.find(id);
			if (c == d.chars.end()) { return std::string("#" + std::to_string(id)); }
			uint32_t version = 0;
			const int index = app.sim.charIndex(id, &version);
			const uint32_t serial = c->second.look.portraitSerial;
			if (app.portraitTable.get(id, index, version, serial ? serial : (uint32_t)(id * 2654435761u), out.uv)) { out.tex = app.portraitsTex; }
			return d.str(c->second.look.name);
		};
		s.active = true;
		s.conversation = m.speaker * 2654435761u + m.seq;
		s.npcName = portrait(m.speaker, s.npcPortrait);
		s.playerName = portrait(m.ch, s.playerPortrait);
		const auto line = app.lastLine.find(m.speaker);
		if (line != app.lastLine.end()) { s.npcText = d.str(line->second); }
		for (const auto& o : m.options) { s.choices.push_back(o.line ? d.str(o.text) : "[...]"); }
		if (d.playerSquads.hasMoney) { s.money = ui::trade::cats(d.playerSquads.money); }
	}
	app.dialogue->update(s);
}

// The world camera's near clip plane: the eye's height over the ground buys depth precision (frame_world's projection). The click ray starts here, not at
// the eye, as the original's does (its getCameraToViewportRay unprojects z = -1: kenshi-rwh.142), so a surface the camera is against does not pick at the eye.
double camera_near()
{
	const double ground = app.hm ? app.hm->height(app.pos[0], app.pos[2]) : app.pos[1];
	return std::fmax(0.5, std::fmin(300.0, (app.pos[1] - ground) * 0.05));
}

// Where the ray from the camera first meets the terrain: 4-unit steps out to kMaxRange, then bisection. False: no hit.
bool ray_ground(const double origin[3], const double dir[3], double hit[3])
{
	constexpr double kStep = 4, kMaxRange = 20000;
	auto above = [&](double t) { return origin[1] + dir[1] * t - app.hm->height(origin[0] + dir[0] * t, origin[2] + dir[2] * t); };
	if (above(0) < 0) { return false; }
	double a = 0;
	for (double b = kStep; b <= kMaxRange; a = b, b += kStep)
	{
		if (above(b) >= 0) { continue; }
		for (int i = 0; i < 20; i++) { const double m = (a + b) * 0.5; (above(m) >= 0 ? a : b) = m; }
		for (int k = 0; k < 3; k++) { hit[k] = origin[k] + dir[k] * b; }
		return true;
	}
	return false;
}

// Not drawn where it is: inside the occluder fade's cone (mesh.glsl, where most of it is dithered away) or on a floor cut away (WorldBuildings::cutAway;
// bottom: the hit shape's low face, so a shape counts by the floor it starts on). why (kenshi-k16.2.259): the reason, for the F12 pick report; "" drawn.
bool see_through(const double p[3], double bottom, std::string* why)
{
	const float* x = app.buildings.xray;
	if (x[3] > 0)	// not exempted for the click ray (kenshi-k16.2.405.1): the cut-open building's near wall shape is not drawn there, and the click must pass it as before
	{
		const double c[3] = { p[0] - app.pos[0], p[1] - app.pos[1], p[2] - app.pos[2] };
		const double len2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
		const double t = (c[0] * x[0] + c[1] * x[1] + c[2] * x[2]) / len2;
		const double back = (1 - t) * std::sqrt(len2);
		const double d = std::sqrt(std::pow(c[0] - x[0] * t, 2) + std::pow(c[1] - x[1] * t, 2) + std::pow(c[2] - x[2] * t, 2));
		if (t < 1 && back > 8 && d < 0.85 * std::min<double>(x[3], 0.35 * back) && !app.buildings.inSolidZone(p))
		{
			if (why) { *why = "inside the occluder fade's cone"; }
			return true;
		}
	}
	return app.buildings.cutAway(p, bottom, why);
}

// A character's or prop's floor for the cut (Characters::culled, its name tag, WorldBuildings::Prop::cut, item labels): on a staircase it counts
// by the floor the stair starts on, as the click ray does (CLIENT.md "Click through visible stairs": the solid shape's low face), so a climb of a
// building shown below it stays drawn until the feet reach the upper floor's level and the stair is no longer under them; elsewhere the feet decide.
// A character's floor cut (Characters::culled, its debug name tag, the UI's name label): one decision for all three (kenshi-k16.2.264.1, the user's
// 2026-11-15 "went indoors and lost its character model" with its name tag still drawn). The cut's geometric test (WorldBuildings::cutHides: anything at
// or above the slab over the shown level) is right for the geometry, but a character carries its own floor on the wire and the two disagree where a
// storey's walls start below the deck it stands on: at the Outpost s-IV the derived table has floor 1 at local 123.93 (world 1188.03) while the deck the
// character stands on is local 139.35 (part 19, authored floor 0, drawn), so the geometric test read the whole deck as "above floor 1" and hid the body
// of a character on a floor the renderer draws (dump cut id=N prints the two side by side). So a character is hidden exactly when the floor it stands on
// is cut: its wire floor over the shown level (the level the player set, which following tracks to the lead), and a roof deck (roofBuilding, only with the
// wire's indoors bit clear: the wire's floor says nothing there, h2v.24.1 a) keeps the geometric test. Wired to the body, the tag and the label, so they
// cannot disagree.
// (The declaration is above name_labels_frame, which is the first user.)
bool cut_hidden(const double pos[3])
{
	if (!app.buildings.cutHides(pos)) { return false; }
	const double up[3] = { pos[0], pos[1] + 2, pos[2] }, down[3] = { 0, -1, 0 };
	double t;
	float box[6];
	bool solid = false;
	if (app.collide.ray(up, down, 8, t, box, {}, nullptr, &solid) && solid) { return app.buildings.cutHides(pos, box[1]); }
	return true;
}

bool char_cut_hidden(uint64_t id, const double pos[3])
{
	// A hover peek cuts an unoccupied building at its floor (kenshi-k16.2.311: s-IV's floor-1 guards stayed drawn over a peeked, cut deck): the same rule as
	// the occupied cut, with the peek's floor as the level the wire floor is held against.
	int level = -1;
	if (!cut_hidden(pos))
	{
		level = app.buildings.peekCutFloor(pos);
		if (level < 0) { return false; }
	}
	// The camera subject (kenshi-k16.2.236.15, the floor view model's step 4): the model decides, never the wire's floor, which lags the model's view on
	// a descent (the view flips one rise below the top, the wire's floor mid-flight). Hidden only when the model's floor is above the shown level, which
	// needs a manual step (R / F) above the subject; otherwise the shown level is the model's floor and the subject is drawn on every frame.
	if (app.follow && app.followId == id)
	{
		const int f = app.buildings.modelFloor(id);
		const bool hidden = f >= 0 && f > app.buildings.floorStatus().shown;
#ifdef KCLIENT_DEV_TOOLS
		// One line per frame the followed subject is not drawn (the callers ask several times a frame): floor_threshold asserts there are none outside a
		// manual step (kenshi-k16.2.236.15).
		static uint64_t logged = ~0ull;
		if (hidden && app.opt.debug && logged != (uint64_t)app.frames)
		{
			logged = (uint64_t)app.frames;
			printf("floor: subject hidden id %llu frame %llu model floor %d shown %d\n", (unsigned long long)id, (unsigned long long)app.frames, f, app.buildings.floorStatus().shown);
		}
#endif
		return hidden;
	}
	const auto it = app.sim.decoder().chars.find(id);
	if (it == app.sim.decoder().chars.end()) { return true; }
	// A roof deck: roofBuilding is a client test (WorldBuildings.hpp 140) and the wire's floor says nothing over a roof, so the geometry decides there.
	// Only with the wire's indoors bit CLEAR, its own reading of "a roof" (user 2026-11-05: the guard on the roof at floor 0): a single-storey
	// building's box makes roofBuilding match its interior too, which would hide a character the wire says is inside on floor 0 (k16.2.264.1, id 41253
	// in the Longhouse).
	// ... and the feet must stand on a shape: roofBuilding is a box test, and the box of a building low in a dune matches the open ground beside it (k16.2.274:
	// id 1159 on terrain at 68394,1230.8,-71032, roof 27206, no shape within 8 u under it, hidden for 74 frames though its wire floor 0 is the shown 0).
	if (!(it->second.cur.flags & simlink::kIndoors) && app.buildings.roofBuilding(pos) >= 0)
	{
		const double up[3] = { pos[0], pos[1] + 2, pos[2] }, down[3] = { 0, -1, 0 };
		double t;
		if (app.collide.ray(up, down, 8, t)) { return true; }
	}
	if (!(it->second.cur.flags & simlink::kYValid)) { return true; }
	return (int)it->second.cur.floor > (level >= 0 ? level : app.buildings.floorStatus().shown);
}

const cfg::Bool kStreamScripted{ cfg::Group{ "Scripted runs" }, "debug.stream_scripted", false, "Scripted runs: stream the buildings as a live run does (a frame's decodes under the per-frame count, the creates in the render queue's stream lane; kenshi-k16.2.205.12.35) instead of waiting for every mesh and texture each frame. For a walk across a streaming boundary; the scene test leaves it off" };
constexpr double kDoorMatch = 150;

// A held action key (ui::keys); not while an edit box has the keyboard
bool title_shown() { return app.mainMenu && app.mainMenu->shown(); }
// The mode the client is in (kenshi-k16.2.472.3): the title and the creator own the screen, the pointer and the keyboard; only kGame draws the world and takes
// the game's keys and world picks. The world stays loaded under the other two (paused or held), unseen and unheard-to.
Mode app_mode() { return title_shown() ? Mode::kTitle : app.creatorUi && app.creatorUi->shown() ? Mode::kCreator : Mode::kGame; }
bool menu_shown() { return app_mode() != Mode::kGame || (app.gameMenu && app.gameMenu->shown()) || (app.tutorials && app.tutorials->browserShown()) || (app.newGame && app.newGame->shown()) || (app.saves && app.saves->shown()); }	// the game's keys are off
bool held(const char* action) { return !menu_shown() && !dynamic_cast<ui::EditBox*>(ui::focusedWidget()) && ui::keys::held(action); }
bool labels_shown() { return held("highlight"); }

// MINE (user's extension): name labels over characters too while ALT is held, off by default. Drawn only, not click targets. Machines have no
// sim entity yet, so there is no option for them.
// The move marker's mesh (data/meshes/position.mesh), loaded on first use; a missing or broken file is reported once and draws nothing
const ogre::Mesh* move_marker()
{
	static ogre::Mesh mesh;
	static int state = 0;
	if (state == 0)
	{
		std::string err = "not found";
		const std::string path = app.res.locate("position.mesh");
		state = !path.empty() && ogre::load(path, mesh, err) ? 1 : -1;
		if (state < 0) { fprintf(stderr, "kclient: move marker position.mesh: %s\n", err.c_str()); }
	}
	return state > 0 ? &mesh : nullptr;
}

// The original draws nothing under a selected character and no route (kinspect selection_marker.md): our ring and route line are debug aids
const cfg::Bool kSelectionRing{ cfg::Group{ "Markers" }, "markers.selection_ring", false, "Debug: a ring under each selected character (the original draws none)" };
const cfg::Bool kNavOverlay{ cfg::Group{ "Navmesh" }, "debug.navmesh", false, "Debug: the client's navmesh as glowing edges round the camera focus (rims and gaps bright, doors coloured)" };
const cfg::Float kSunShafts{ cfg::Group{ "Indoor lighting (experimental)" }, "render.sun_shafts", 0.0, 0.0, 10.0, "Experimental (kenshi-jj3.7.1): visible sunbeams in the air inside buildings (a ray march against the sun's shadow map through the baked probe volumes of the nearest buildings); the value scales the haze density (0 = off, the default; 1 = the normal look)" };
const cfg::Float kLampBeams{ cfg::Group{ "Indoor lighting (experimental)", "turns render.lamp_occlusion's baked lamp visibility on while above 0" }, "render.lamp_beams", 0.0, 0.0, 1.0, "Experimental, our own effect (kenshi-jj3.28.1; the original has no beams): a faint volumetric cone under each shaded hanging lamp (a spot light pointing down) in the air of the nearest baked buildings, in the lamp's colour, fading with distance and stopped at the lamp's own walls and floors by the baked lamp visibility (so it needs and switches on render.lamp_occlusion's bake). 1 is clearly fainter than render.sun_shafts 1; 0 = off" };
const cfg::Float kDustMotes{ cfg::Group{ "Indoor lighting (experimental)", "works without render.sun_shafts" }, "render.dust_motes", 0.0, 0.0, 10.0, "Experimental (kenshi-jj3.8.1): drifting dust motes in the air of the nearest baked building that show only inside a sunbeam (the sun's shadow lookup of render.sun_shafts, no ambient); the value scales the mote count (0 = off, the default; 1 = the normal amount)" };
#ifdef KCLIENT_DEV_TOOLS
const cfg::Int kSunShaftsDebug{ cfg::Group{ "Light debug", "needs render.sun_shafts above 0", "Debug" }, "render.sun_shafts_debug", 0, 0, 8, "Debug (kenshi-jj3.7.3; needs render.sun_shafts > 0): 1 = the sun visibility the march reads, averaged over each view ray (white = lit air); 2 = the sun visibility at the scene surface (compare with the sunlit floor patch); 3 = the normal beams with the sun forced lit (the density check); 4 = the baked enclosure the indoor density follows, averaged over the ray; 5 = the volume weight (the probe box's edge ramp) along the ray; 6 = the indoor density gate x weight; 7 = the accumulated in-scatter x8; 8 = the density the march used x5 (a scale check)" };
#endif
const cfg::Bool kShadowCache{ cfg::Group{ "Shadows" }, "render.shadow_cache", true, "Cache the sun shadow maps' static casters (terrain, buildings, features, foliage; kenshi-k16.2.205.12.2): each cascade's tile is redrawn only when its snapped centre, the sun, the streamed content or a door changes, and the characters and wind-turned parts are drawn over a restored copy each frame. Off: every cascade redraws every caster every frame" };
const cfg::Int kShadowStaticBudget{ cfg::Group{ "Shadows" }, "render.shadow_static_budget", 1, 0, 4, "With render.shadow_cache: the most cached shadow tiles a frame redraws only because their content changed (buildings streamed in, a door, terrain; kenshi-k16.2.205.12.73). The others keep their old content a few frames (at most 3); a tile the camera or the sun moved redraws at once. 0: every changed tile in the same frame" };
const cfg::Bool kStreamPriority{ cfg::Group{ "Performance" }, "render.stream_priority", true, "The building streams (decodes and the render queue's stream lane) run in view and near first, then in view and far, then out of view by distance, re-ranked as the camera moves (kenshi-k16.2.258.3). Off: submit order, the nearest zone cell first" };
const cfg::Bool kLoadGate{ cfg::Group{ "Single player" }, "session.load_gate", true, "The load screen is the ready-set gate (kenshi-k16.2.258.3): while it is up nothing of the world is shown, so the stream lane's per-frame budgets are lifted (session.load_stream_ms) and the screen lifts once every building in the load ring is created. Off: the lane's play budgets over the screen too" };
const cfg::Float kLoadStreamMs{ cfg::Group{ "Single player" }, "session.load_stream_ms", 40.0, 0.0, 500.0, "The time a frame gives the stream lane's creates while the load screen is up (session.load_gate), ms; 0: no time budget" };
const cfg::Int kVengeBeamsMax{ cfg::Group{ "Performance" }, "render.venge_beams_max", 7, 0, 1000, "The most Venge sky beams drawn (kenshi-k16.2.409.1): the sim runs every beam, the client draws only this many nearest the camera whose zone cell is loaded (the original's weather-handler gate, kenshi-k16.2.409.3). A beam stays in the set until it is 10% farther than the last one, and fades in or out over a second at the boundary. 0 = none; a large number = all the loaded ones" };
#ifdef KCLIENT_DEV_TOOLS
const cfg::Bool kVengeBeamsGate{ cfg::Group{ "Performance" }, "debug.venge_beams_gate", true, "Debug (kenshi-k16.2.409.3): the original's loaded-cell gate on the Venge beam draw set (rwh.211, weather gate 140100ba0: a beam's own zone cell must be loaded). Off = every bursting beam can be drawn, the pre-409.3 look, for the before shot" };
#endif
const cfg::Float kStreamMs{cfg::Group{ "Performance" }, "render.stream_ms", 4.0, 0.0, 50.0, "The time a frame gives the streamed resource creates (buildings, characters: the render queue's stream lane; kenshi-k16.2.205.12.74), ms. A create that would run past it waits for the next frame; one always runs. 0: no time budget (the 8 MB per frame byte cap only)" };
const cfg::Int kStreamSliceMb{ cfg::Group{ "Performance" }, "render.stream_slice_mb", 4, 0, 64, "A streamed texture over twice this size (MiB) is created in slices of this size, one per render queue job, over several frames (kenshi-k16.2.205.12.74); it shows once whole. 0: one create per texture" };
const cfg::Bool kLampOcclusion{ cfg::Group{ "Indoor lighting (experimental)" }, "render.lamp_occlusion", false, "Experimental (kenshi-jj3.9.1): the lamps of a building stop at its walls and floors: a baked per-probe visibility of each lamp (one ray per probe per lamp through the building's triangles) multiplies the lamp's light, so it does not light the floor above or the room next door through the slab or wall. Soft (probe spacing), not the shadow of a chair. Works without render.interior_light" };
const cfg::Float kShadowSoft{ cfg::Group{ "Indoor lighting (experimental)" }, "render.shadow_soft", 0.0, 0.0, 10.0, "Experimental (kenshi-jj3.6.1): sun shadow edges widen with the distance between caster and surface (PCSS, near two cascades); the value is the sun's half angle in degrees (0 = off, the fixed-width edge; the real sun is 0.27, 1.5 to 2 reads soft)" };
const cfg::Float kInteriorLight{ cfg::Group{ "Indoor lighting (experimental)" }, "render.interior_light", 0.25, 0.0, 1.0, "Experimental (kenshi-jj3.2.1): indoors the ambient light follows the openings of the building: the sky light the baked probes of the nearest buildings let reach each spot multiplies the ambient term, so a room is dark where the door, windows and hatches do not reach it. This controls the amount of original Kenshi lighting that comes through: 0 = none, 1 = full original Kenshi lighting (the effect off)" };
const cfg::Bool kEyeAdapt{ cfg::Group{ "Exposure", "needs render.interior_light below 1 to matter indoors" }, "post.eye_adapt", false, "Experimental (kenshi-jj3.5.1): the auto exposure reaches far lower, so a room lit only through its openings (render.interior_light below 1) reads bright indoors and blows out the doorway (off = the original's narrow exposure range)" };
const cfg::Float kSunBounce{ cfg::Group{ "Indoor lighting (experimental)", "needs render.interior_light below 1" }, "render.sun_bounce", 0.0, 0.0, 1.0, "Experimental (kenshi-jj3.4.1): the sun's bounced light inside a building, added to the ambient where the sun reaches the floor and walls (a sun-dependent bake of the nearest buildings that hold the camera or a player character, re-baked in the background as the sun moves; 0 = off, 1 = full; does nothing unless render.interior_light is below 1)" };
const cfg::Bool kLightProbes{ cfg::Group{ "Light debug" }, "debug.light_probes", false, "Debug (experimental light bake, kenshi-jj3.1.1): the baked sky-transfer probes of the nearest building with interiors as points, grey by how much sky reaches them, red = inside a wall (bakes it on a worker thread the first time; nothing in the picture reads it)" };
const cfg::Float kNavStair{ cfg::Group{ "Navmesh" }, "debug.navmesh_stair", 30, 5, 80, "Debug navmesh: a polygon this steep (degrees) or more is a stair or ramp, drawn orange on every nearby level" };
const cfg::Bool kNavPruned{ cfg::Group{ "Navmesh" }, "debug.navmesh_pruned", false, "Debug navmesh: also the polygons the bake pruned (unreachable, flags 0), dim red edges" };
const cfg::Float kNavBand{ cfg::Group{ "Navmesh" }, "debug.navmesh_band", 20, 2, 500, "Debug navmesh: half-height of the level band round the selected character's feet (world units); below it dims, above it fades or hides under a floor cut" };
const cfg::Bool kRouteLine{ cfg::Group{ "Markers" }, "markers.route", false, "Debug: each selected character's route as a line, and the approach point" };
const cfg::Bool kOccluderFade{ cfg::Group{ "Building see-through" }, "camera.occluder_fade", true, "Buildings between the camera and the followed character turn see-through (auto-center and hover camera)" };
const cfg::Bool kHudText{ cfg::Group{ "Developer text" }, "debug.hud_text", false, "Debug: the top-left developer text block (camera, ground, zone, nearest town, counts, time, weather, timings, sim); needs --debug. Off: the F12 shot's .txt still carries every line (hud.* keys)" };
const cfg::Text kShotDir{ cfg::Group{ "Screenshots" }, "screenshot.dir", "", "Where the screenshot key saves (and its .txt scene notes); empty: the user data dir's kclient/screenshots ($XDG_DATA_HOME or ~/.local/share; Windows %LOCALAPPDATA%\\OpenKenshi\\data)" };
const cfg::Text kShotFormat{ cfg::Group{ "Screenshots" }, "screenshot.format", "webp", { "webp", "png" }, "The format of a shot nobody named (F12, scene_test, lockstep shots): webp = lossless WebP at a fast effort (cwebp -z 1, under 0.6 s at 1440p; Linux builds with libwebp; half the PNG size; the other platforms always write png), png. A path with an explicit .png / .webp keeps its extension" };
const cfg::Bool kPeek{ cfg::Group{ "Building see-through" }, "camera.peek", true, "Pointing at a building's inside from outside shows its walls as a ghost (ours; the original has none; playtest)" };
const cfg::Text kSpeedUnit{ cfg::Group{ "HUD and labels" }, "ui.speed_unit", "kph", { "kph", "mph" }, "The unit of speeds in the panels (run speed, wind); the original shows mph" };
// u/s -> the ui.speed_unit (1 u = 10 cm; mph: kinspect run_speed_mph.md, the exe's float)
#ifdef KCLIENT_DEBUG_VIEW
#endif
float speedScale() { return kSpeedUnit.get() == "mph" ? 0.2236936f : 0.36f; }
const cfg::Bool kOccluderGhost{ cfg::Group{ "Building see-through", "needs camera.occluder_fade" }, "camera.occluder_ghost", true, "The see-through is the whole building fading to glass (clicks pass it), else a cone round the character" };
const cfg::Bool kInteriorMask{ cfg::Group{ "Interiors" }, "render.interior_mask", true, "Clip the terrain, grass and weather particles inside buildings by the buildings' interior mask hulls (the game's InteriorMask target), so a sunk building's interior is not filled by the dune; off for comparisons" };
const cfg::Bool kTitleAtStart{ cfg::Group{ "Screens" }, "ui.title_at_start", true, "Start on the title screen (interactive runs)" };
const cfg::Text kServerExe{ cfg::Group{ "Single player" }, "session.server", "", "Single player: the OpenKenshi-server to start (empty: the one built with this client, its build dir's cpp/OpenKenshi-server: target ksim_server)" };
const cfg::Bool kVsync{ cfg::Group{ "Performance" }, "gfx.vsync", true, "Wait for the monitor's refresh before showing a frame. Takes effect after a restart (the driver may override it)" };
const cfg::Int kFpsCap{ cfg::Group{ "Performance" }, "gfx.fps_cap", 0, 0, 1000, "Frame rate limit, 0 none (applies at once; with vsync on, the refresh rate already limits it)" };
const cfg::Int kSessionLoadScreen{ cfg::Group{ "Single player" }, "session.loading_screen", 1, 0, 2, "A loading screen until the world round the squad has streamed in (new game, load, connect): 0 off, 1 on (not in scripted runs), 2 always" };
const cfg::Text kServerArgs{ cfg::Group{ "Single player" }, "session.server_args", "--scenario newgame", "Single player: the server's arguments, split at spaces (e.g. add --navworld DIR, --save-dir DIR)" };
const cfg::Bool kCharacterLabels{ cfg::Group{ "HUD and labels" }, "ui.labels.characters", false, "Holding ALT also shows character names" };


// The control-hints resolution's inputs (controlhints.hpp, DESIGN 2): the host state a resolver reads, at the moment it resolves - the modifiers as
// sokol reports them NOW, so the panel and the F12 sidecar see a held Shift or Alt. Step 0: nothing reads it but the stub resolver and the report.
uint64_t squad_leader(const std::vector<uint64_t>& sel);
void portrait_click(uint64_t id, bool add);
void portrait_focus(uint64_t id);
hints::Context hints_context()
{
	hints::Context c{ app.sim, app.chars };
	c.selection = app.sim.selected();
	c.centred = !app.follow && app.rts && app.centerId;
	c.centerId = app.centerId;
	c.squadLeader = [](const std::vector<uint64_t>& sel) { return squad_leader(sel); };
	c.select = [](uint64_t id, bool add) { portrait_click(id, add); };
	c.focus = [](uint64_t id) { portrait_focus(id); };
	const auto down = [](int k) { return app.keys[k]; };
	c.alt = down(SAPP_KEYCODE_LEFT_ALT) || down(SAPP_KEYCODE_RIGHT_ALT);
	c.ctrl = down(SAPP_KEYCODE_LEFT_CONTROL) || down(SAPP_KEYCODE_RIGHT_CONTROL);
	c.shift = down(SAPP_KEYCODE_LEFT_SHIFT) || down(SAPP_KEYCODE_RIGHT_SHIFT);
	c.paused = app.sim.active() && (app.sim.clock().pauseBits & 1);
	c.floor = app.buildings.level();
	c.following = app.follow;
	c.followId = app.followId;
	return c;
}

// The hint list at a screen point, as text: the F12 sidecar's `hints:` block and the dev `hints at X Y` op (dev/hintsop.cpp) print the same thing
// for the same point. A decision report only: it re-picks and never changes a cursor or sends an order.
std::string hint_report(double mx, double my)
{
	const Pick p = pick_screen(mx, my);
	const hints::Context ctx = hints_context();
	return hints::format_hover(hints::hints_for(p, ctx));
}

// A jobs row's text (SIMLINK JOBS: the client owns the names), in our own words; the types are the exe's TaskData numbers (kinspect
// findings/jobs17_static.md Appendix A). A targeted row adds ": <name>", nothing when the target is unknown (the original's dead-target case).
std::string job_text(const simlink::SimLinkDecoder::JobRow& row)
{
	static const std::unordered_map<uint16_t, const char*> kNames = {
		{ 2, "Build" }, { 4, "Attack" }, { 5, "Attack target" }, { 16, "Fight enemies" }, { 25, "First aid" }, { 26, "Loot" }, { 31, "Stay close" },
		{ 45, "Bodyguard" }, { 57, "Robot repair" }, { 58, "Medic" }, { 60, "Robot repair" }, { 61, "Attack target" }, { 87, "Work machine" },
		{ 95, "Repair" }, { 97, "Train" }, { 106, "Put rescued in bed" }, { 124, "Haul to" }, { 125, "Engineer" }, { 136, "Dispose of corpse" },
		{ 137, "Dispose of corpses" }, { 138, "Find corpses" }, { 146, "Turret gunner" }, { 148, "Rescue" }, { 152, "Run automatic machine" },
		{ 187, "Obedient slave" }, { 221, "Work machine" }, { 223, "Remove home intruder" }, { 234, "Guard turret" }, { 238, "Forage animals" },
		{ 249, "Splints" }, { 250, "Splints" }, { 273, "Remove intruder" }, { 289, "Stay close" } };
	const auto n = kNames.find(row.type);
	std::string text = n != kNames.end() ? n->second : "Job " + std::to_string(row.type);
	if (row.target)
	{
		const auto ch = app.sim.decoder().chars.find(row.target);
		const simlink::Interactable* t = ch == app.sim.decoder().chars.end() ? app.sim.thing(row.target) : nullptr;
		const std::string name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : t ? app.sim.nameOf(t->record, app.chars) : "";
		if (!name.empty()) { text += ": " + name; }
	}
	return text;
}

// CharAct / kAttack / kFollow / sneak_actor / seen_by / locked_cage moved to pick.cpp unchanged (kenshi-k16.2.294.2, pick.hpp): the sim comes in
// as an argument there.

uint64_t hoverCage = 0;	// the cage under the cursor (hover_cursor), 0 = none

// char_act moved to pick.cpp (kenshi-k16.2.294.2); the app state it reads goes in as arguments.
bool ctrl_held() { return app.keys[SAPP_KEYCODE_LEFT_CONTROL] || app.keys[SAPP_KEYCODE_RIGHT_CONTROL]; }

// Peek (camera.peek; ours, user 2026-11-04): the cursor on a building with floors that nobody of ours is in (not sliced, not a ghosted occluder)
// for kPeekDwell cuts it as the inside view would at the floor of the surface under the cursor (the cursor ID pass, idbuffer.hpp: what is drawn,
// so terrain, a buried base or a collision box never counts; below the top: a roof gives the floor under it; under the ground floor, as the base
// and the outside steps: nothing, user 2026-11-05), the cut parts see-through (WorldBuildings::peeks) fading in over kPeekIn. It holds while the
// cursor stays on the building (its glass included), and fades out over kPeekOut after. The occluder glass (fadeGhosts) fades at the same rates.
void update_peek(double dt)
{
	constexpr double kPeekDwell = 0.2, kPeekIn = 0.3, kPeekOut = 0.4;	// MINE: seconds (user 2026-11-04: 1 s fades too slow)
	WorldBuildings& wb = app.buildings;
	wb.fadeGhosts(dt, kPeekIn, kPeekOut);
	int cand = -1, floor = 0;
	const bool overUi = app.uiOn && ui::wantsMouse();
	if (const auto held = wb.peeks.find((uint32_t)app.peekCand); overUi && held != wb.peeks.end() && held->second.on) { cand = app.peekCand; floor = held->second.floor; }	// the floor diamond's arrows act on it
	else if (kPeek.get() && app.sim.active() && !(app.look && app.lookDrag) && !overUi)	// a right press holds it: its release clicks through
	{
		double dir[3];
		mouse_ray(app.mouseX, app.mouseY, dir);
		wb.cursorRay(dir, true);
		const int b = framestats::stats().cursorHit.building;
		const auto held = b >= 0 ? wb.peeks.find(b) : wb.peeks.end();
		if (b < 0 || wb.topFloor(b) == 0 || wb.sliced(b) || wb.ghosted(b) || wb.belowFloors(b, framestats::stats().cursorHit.at)) {}
		else if (held != wb.peeks.end() && held->second.on) { cand = b; floor = held->second.floor; }
		else
		{
			// the player's level decides, clamped to the building's top (user 2026-11-14, kenshi-h2v.24.6: 0 the ground floor, a level at or above the
			// top the roof deck, uncut; the hover never writes the level back)
			floor = std::min(wb.level(), wb.topFloor(b));
			cand = b;
		}
	}
	else { wb.cursorRay(app.pos, false); }
	if (cand != app.peekCand) { app.peekCand = cand; app.peekFor = 0; }
	else { app.peekFor += dt; }
	if (cand >= 0 && app.peekFor >= kPeekDwell && !wb.peeks[cand].on)
	{
		wb.peeks[cand].on = true;
		wb.peeks[cand].floor = floor;
		if (app.scripted()) { printf("peek: %s floor %d\n", wb.describe((uint32_t)cand).c_str(), floor); }
	}
	for (auto it = wb.peeks.begin(); it != wb.peeks.end();)
	{
		WorldBuildings::Peek& pk = it->second;
		if (pk.on && (int)it->first != cand)
		{
			pk.on = false;
			if (app.scripted()) { printf("peek off: %s\n", wb.describe(it->first).c_str()); }
		}
		pk.fade = (float)std::clamp(pk.fade + (pk.on ? dt / kPeekIn : -dt / kPeekOut), 0.0, 1.0);
		it = !pk.on && pk.fade <= 0 ? wb.peeks.erase(it) : std::next(it);
	}
}

// The hover cursor shows what a right click would do there (docs/reference/CURSORS.md, CURSORS_LIVE.md; SIMLINK 13): char_act over a character,
// pick_item over a ground item, door / lock (locked) / spanner (damaged; RMB still opens) over a door, invalidmove over terrain or foliage with no
// client navmesh on its vertical line (ClientNav::walkable), else the plain arrow. A building's shape is the original's hover candidate, so its
// walls never show invalidmove (kinspect findings/cursors.md "Wall hover": a plain foreign building sets no cursor); its other cursors come later.
std::string hoverRule;	// the rule hover_cursor last applied, for the F12 shot's pick report (k16.2.234.1); a text only, never read by the decision
void hover_cursor()
{
	app.hoverHitOk = false;
	if (app.look) { hoverRule = "mouse look: no hover decision (the cursor is hidden)"; return; }
	if (app.boxDown && app.boxDrag) { hoverRule = "box select drag: the hover cursor holds still"; return; }	// while the box shows the hover cursor holds still (the original skips the hover dispatch; kenshi-rwh.134)
	if (app.uiOn && ui::wantsMouse()) { app.hoverThing = 0; app.hoverChar = 0; hoverCage = 0; hoverRule = "pointer over a UI panel -> arrow (no world pick)"; app.cursors.set("arrow"); return; }
	const Pick p = pick_screen(app.mouseX, app.mouseY);
	if (p.kind == Pick::Ground) { app.hoverHitOk = true; std::copy(p.hit, p.hit + 3, app.hoverHit); }
	hoverRule = "no rule matched (nothing hit, or a hit with no cursor of its own) -> arrow";
	app.hoverThing = p.kind == Pick::Item || p.kind == Pick::Door || p.kind == Pick::Container || p.kind == Pick::Producer ? p.id : 0;
	app.hoverChar = p.kind == Pick::Character ? p.id : 0;
	std::string name = "arrow";
	hoverCage = 0;
	if (const simlink::SimLinkDecoder::Cage* cg = p.kind == Pick::Character ? locked_cage(app.sim, p.id) : nullptr)
	{
		// the red padlock with the pick chance (the original's cage hover, kenshi-0qr shot 9); a click orders nothing
		name = "lock";
		hoverRule = "character in a locked cage -> lock (padlock, pick chance; a click orders nothing)";
		hoverCage = cg->cage;
		const auto& sel = app.sim.selected();
		const uint64_t actor = sel.empty() ? 0 : sel.front();
		if (actor && (cageHover.cage != cg->cage || cageHover.actor != actor)) { cageHover = { cg->cage, actor, app.sim.menuQuery(cg->cage), -1 }; }
	}
	else if (p.kind == Pick::Character) { name = char_act(app.sim, app.chars, p.id, ctrl_held()).cursor; hoverRule = "character -> char_act: " + name; }
	else if (p.kind == Pick::Item) { name = "pick_item"; hoverRule = "ground item -> pick_item"; }
	else if (p.kind == Pick::Container)
	{
		// a locked box picks (kind 3 state 2, SIMLINK's container lock; CURSORS_LIVE B6's foreign locked part = 0xf lockpick); else a foreign one is the red
		// 0x18 scavenge (CURSORS_LIVE B4b: foreign storage and a foreign Shop Counter, the red magnifier; kenshi-dx8.1), ours 2 search
		const simlink::Interactable* t = app.sim.thing(p.id);
		name = t && t->state == simlink::Interactable::kLocked ? "lockpick" : app.sim.foreign(p.id) ? "scavenge" : "search";
		hoverRule = std::string("container -> ") + (name == "lockpick" ? "locked: lockpick" : name == "scavenge" ? "foreign (not ours): scavenge" : "ours: search");
	}
	else if (p.kind == Pick::Producer) { name = "mine"; hoverRule = "producer (mine / deposit) -> mine"; }	// kinspect item 59 (CURSORS.md 0x12; a built mine: same class, M)
	else if (p.kind == Pick::Bed) { name = "use"; hoverRule = "bed -> use (no sleep cursor in CURSORS.md: the building fallback)"; }	// UNSURE: CURSORS.md has no sleep cursor; a building's own cursor falls back to use (9)
	else if (const simlink::Interactable* d = p.kind == Pick::Door ? app.sim.thing(p.id) : nullptr)
	{
		name = d->state == simlink::Interactable::kLocked ? "lock" : d->health < d->maxHealth ? "spanner" : "door";
		hoverRule = std::string("door -> ") + (name == "lock" ? "locked: lock" : name == "spanner" ? "damaged: spanner (RMB still opens)" : "intact: door");
	}
	else if (const simlink::SimLinkDecoder::Building* pb = p.kind == Pick::Ground && p.building && p.id ? app.sim.building(p.id) : nullptr)
	{
		// a player's building (CURSORS.md "Building hover", 1407fc6e0): build while it is not built, spanner when damaged (UNSURE there), else the class's
		// own cursor, which the wire does not carry: the fallback use (9)
		name = !(pb->flags & simlink::SimLinkDecoder::Building::kBuilt) ? "build" : pb->hp < pb->maxHp ? "spanner" : "use";
		hoverRule = std::string("player's building -> ") + (name == "build" ? "not built: build" : name == "spanner" ? "damaged: spanner" : "built, class cursor unknown: use");
	}
	else if (p.kind == Pick::Ground && !p.building && app.nav.walkable(p.hit) == ClientNav::Reach::kNo) { name = "invalidmove"; hoverRule = "terrain / foliage with no navmesh under it (walkable = no) -> invalidmove"; }
	else if (p.kind == Pick::Ground && p.building) { hoverRule = "building shape -> arrow (a foreign building sets no cursor: the original's hover candidate)"; }
	else if (p.kind == Pick::Ground) { hoverRule = std::string("ground, walkable ") + (app.nav.walkable(p.hit) == ClientNav::Reach::kYes ? "yes" : "unknown") + " -> arrow (a right click moves there)"; }
	else if (p.kind == Pick::None) { hoverRule = "nothing under the pointer (letterbox bar or sky) -> arrow"; }
	if (app.scripted() && name != app.cursors.current()) { printf("cursor: %s at %.0f %.0f (frame %llu)\n", name.c_str(), app.mouseX, app.mouseY, (unsigned long long)app.frames); }
	app.cursors.set(name);
}

#ifdef KCLIENT_DEV_TOOLS
// debug.pick_mask (dev/pickmask.cpp, kenshi-k16.2.239.5.1): the mask runs pick_at() itself per cell, so it shows what a click hits.
void pick_mask_update()
{
	if (!app.opt.debug || (!dev::pickmask::on() && !app.hoverThing && !app.hoverChar)) { return; }	// development features need --debug
	static std::unordered_map<uint64_t, std::array<float, 6>> sceneryBox;	// the scenery subjects seen: id -> world box
	static const dev::pickmask::Host host{
		[](double mx, double my) -> uint64_t
		{
			double dir[3];
			mouse_ray(mx, my, dir);
			const Pick p = pick_at(dir);
			if (p.kind == Pick::Ground && p.scenery)	// scenery with no sim id (a tree, a rock): the mask's subject is its box's identity (kenshi-l5dx)
			{
				sceneryBox[p.scenery] = std::array<float, 6>{ p.sceneryBox[0], p.sceneryBox[1], p.sceneryBox[2], p.sceneryBox[3], p.sceneryBox[4], p.sceneryBox[5] };
				return p.scenery;
			}
			return p.kind == Pick::Item || p.kind == Pick::Door || p.kind == Pick::Container || p.kind == Pick::Producer || p.kind == Pick::Character ? p.id : 0;
		},
		[](uint64_t id, float box[4]) -> bool
		{
			double lo[3], hi[3];
			if (const auto sb = sceneryBox.find(id); sb != sceneryBox.end()) { for (int k = 0; k < 3; k++) { lo[k] = sb->second[k]; hi[k] = sb->second[3 + k]; } }
			else if (!app.buildings.propBox(id, lo, hi))
			{
				// a character: the box of its posed pick capsules (their ends, each grown by the radius)
				const int index = app.sim.charIndex(id);
				std::vector<Characters::PickCapsule> caps;
				if (index < 0 || app.chars.pickCapsules((size_t)index, caps) <= 0) { return false; }
				bool seen = false;
				for (const Characters::PickCapsule& c : caps)
				{
					for (const double* e : { c.a, c.b })
					{
						for (int k = 0; k < 8; k++)
						{
							const double w[3] = { e[0] + ((k & 1) ? c.r : -c.r), e[1] + ((k & 2) ? c.r : -c.r), e[2] + ((k & 4) ? c.r : -c.r) };
							float x, y;
							if (!world_to_screen(w, x, y)) { return false; }
							if (!seen) { box[0] = box[2] = x; box[1] = box[3] = y; seen = true; }
							box[0] = std::min(box[0], x); box[1] = std::min(box[1], y); box[2] = std::max(box[2], x); box[3] = std::max(box[3], y);
						}
					}
				}
				return seen;
			}
			bool any = false;
			for (int c = 0; c < 8; c++)
			{
				const double w[3] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2] };
				float x, y;
				if (!world_to_screen(w, x, y)) { return false; }	// a corner behind the camera: no box
				if (!any) { box[0] = box[2] = x; box[1] = box[3] = y; any = true; }
				box[0] = std::min(box[0], x); box[1] = std::min(box[1], y); box[2] = std::max(box[2], x); box[3] = std::max(box[3], y);
			}
			return true;
		},
		[]() -> uint64_t
		{
			uint64_t h = 1469598103934665603ull;
			const double v[] = { app.pos[0], app.pos[1], app.pos[2], app.yaw, app.pitch, (double)view::widthf(), (double)view::heightf() };
			for (double d : v) { uint64_t b; std::memcpy(&b, &d, 8); h = (h ^ b) * 1099511628211ull; }
			return h;
		} };
	uint64_t subject = app.hoverThing ? app.hoverThing : app.hoverChar;
	if (!subject && dev::pickmask::on())	// nothing with an id under the cursor: anonymous scenery (a tree, a rock) is a subject too (kenshi-l5dx)
	{
		double dir[3];
		mouse_ray(app.mouseX, app.mouseY, dir);
		const Pick p = pick_at(dir);
		if (p.kind == Pick::Ground && p.scenery) { sceneryBox[p.scenery] = std::array<float, 6>{ p.sceneryBox[0], p.sceneryBox[1], p.sceneryBox[2], p.sceneryBox[3], p.sceneryBox[4], p.sceneryBox[5] }; subject = p.scenery; }
	}
	dev::pickmask::update(host, subject, app.mouseX, app.mouseY);
}
#endif


// Sim orders by mouse (docs/CLIENT.md "Live sim link"): left click selects a player character (Shift adds; anything else clears), a right click (no
// drag) acts on what is under the cursor: a character as char_act says (attack, follow (order 3), or loot / first aid / carry / put down (order
// 60)), pick up a ground item, open / close a door (order 60), else move the selection there (Shift queues).
void order_sent(bool sent)
{
	if (sent) { app.autorun = false; app.driveSent = 0; app.strafeSent = 0; }	// any ordinary order clears drive on the server (SIMLINK 12)
}

// Open a container (the original's LOOT_TARGET on storage): the selection walks to its use point and its window opens once a selected player
// character is within kContainerOpen of it (update_ui). The server's loot action is take-all, so the window is a plain walk plus transfers (order 63).
constexpr double kContainerOpen = 16;	// SIMLINK 13: order 63's reach
constexpr double kProducerOpen = 100;	// a producer's: its functionality's `use range`, to its origin (MINE_SELL_SLICE MQ9; the mines' 100 is not on the wire)

// The open item windows (kenshi-k16.20.13.2): the entry of an owner, the owner's window closed (the mark and the pin go with it).
App::OpenCont* cont_find(uint64_t id)
{
	const auto it = std::find_if(app.conts.begin(), app.conts.end(), [id](const App::OpenCont& c) { return c.id == id; });
	return it == app.conts.end() ? nullptr : &*it;
}

void cont_close(uint64_t id)
{
	std::erase_if(app.conts, [id](const App::OpenCont& c) { return c.id == id; });
	if (app.itemTarget == id) { app.itemTarget = 0; }
	if (app.tradeStock == id) { app.tradeStock = 0; }
}

// Every window besides the character's (a world change, a load).
void cont_close_all()
{
	app.conts.clear();
	app.pending = {};
	app.tradeStock = 0;
	app.itemTarget = 0;
}

// The one place a container window is added (kenshi-k16.2.304.2; the original hides every container window first, rwh.168): the open ones close, then
// `c` is the only one. A trade window going is the client's close of it (as update_ui's), never a reopen.
void cont_add(const App::OpenCont& c)
{
	for (const uint64_t id : [&] { std::vector<uint64_t> v; for (const App::OpenCont& o : app.conts) { if (o.id != c.id) { v.push_back(o.id); } } return v; }())
	{
		if (app.tradeStock == id && !c.trade) { app.sim.tradeClose(); app.tradeClosing = true; }
		if (const App::OpenCont* o = cont_find(id)) { printf("container: %llu closed (replaced by %llu)\n", (unsigned long long)id, (unsigned long long)c.id); (void)o; }
		cont_close(id);
	}
	cont_close(c.id);
	app.conts.push_back(c);
}

// The last open window with a user, a trader's (trade) or not: what the scripted `transfer` ops act on.
App::OpenCont* cont_scripted(bool trade)
{
	for (auto it = app.conts.rbegin(); it != app.conts.rend(); ++it) { if (it->user && !it->view && it->trade == trade) { return &*it; } }
	return nullptr;
}

// The player character that acts for an owner's window: a character window's own, a box's or body's user (0: nobody, a lone box).
uint64_t cont_actor(uint64_t owner)
{
	const App::OpenCont* c = cont_find(owner);
	return c ? c->user : owner;
}
bool open_container(uint64_t id, bool queue)
{
	const simlink::Interactable* t = app.sim.thing(id);
	if (!t || (t->kind != simlink::Interactable::kContainer && t->kind != simlink::Interactable::kProducer)) { return false; }	// a producer's output is a one-slot container (always fed)
	const double at[3] = { t->use[0], t->use[1], t->use[2] };
	if (!app.sim.moveTo(at, queue)) { return false; }
	const auto& s = app.sim.selected();
	const uint64_t lead = s.empty() ? 0 : s.front();
	app.pending = { id, false, 0, lead, app.sim.taskOrder(lead) };
	printf("open: order for %llu pending (walker %llu, task %u)\n", (unsigned long long)id, (unsigned long long)lead, (unsigned)app.pending.task);
	return true;
}

// Work a producer (a deposit, a mine; MINE_SELL_SLICE 3.1): order 60 action 4, the server walks the selection there and they operate it until it is
// full or another order. An order only: no window opens (user 2026-11-02: the original opens nothing on RMB; LMB shows the output).
bool use_producer(uint64_t id, bool queue)
{
	const simlink::Interactable* t = app.sim.thing(id);
	return t && t->kind == simlink::Interactable::kProducer && app.sim.interact(id, 4, queue);
}

// Loot a KO'd or dead character (LOOT_TARGET on a body): order 60 action 5 with flags bit 0, so the server walks there without taking anything and
// sends the body's INVENTORY on arrival; its window opens beside the looter's as a container's does (update_ui), the worn gear draggable (68).
bool open_body(uint64_t id, bool queue)
{
	if (!app.sim.interact(id, 5, queue, 1)) { return false; }
	app.pending = { id, true, app.sim.taskSerial() };
	return true;
}

const std::string kNewGameFaction = "Nameless";	// NEW_GAME's faction name: the original's player faction (MINE until the panel asks for one)

// container_locked moved to pick.cpp (kenshi-k16.2.294.2): it takes the sim (pick.hpp).

// A container order's refusal (SIMLINK AX: event 10 / 13 reasons on loot, put, take and transfer) as a HUD line, or null for one it does not word.
// "It's locked." is the original's line (ORIGINAL_AI.md's GOAP failure bark; the data has no locked-box UI string); the rest are our wording.
// A full pack answers no_room, worded with the quick move's line above.
const char* container_refusal(const std::string& r)
{
	static const std::pair<const char*, const char*> kReasons[] = { { " REJECTED: locked", "It's locked." },
		{ " REJECTED: item_type", "That kind of item doesn't go in there." }, { " REJECTED: out_of_reach", "Too far away." },
		{ " REJECTED: full", "There's no room in there." } };
	for (const auto& [tail, text] : kReasons) { if (r.ends_with(tail)) { return text; } }
	return nullptr;
}

// The view from the camera globals and the window (camera.hpp): the one place renderers' cameras come from.
Camera current_camera(float nearZ, float farZ)
{
	Camera c;
	for (int k = 0; k < 3; k++) { c.pos[k] = app.pos[k]; }
	camera_basis(c.f, c.r, c.u);
	c.fovY = kFovY;
	c.tanY = std::tan(kFovY * 0.5f);
	c.aspect = windowgate::aspect();	// the applied size: while a window change is held the scene renders at the last applied size, scaled to the window (k16.2.222)
	c.nearZ = nearZ;
	c.farZ = farZ;
	c.width = windowgate::width();
	c.height = windowgate::height();
	c.focus = (float)(app.follow ? app.followDist : app.rts ? app.rtsDist : 0);
	return c;
}

void build_mvp(float mvp[16], float nearZ, float farZ) { current_camera(nearZ, farZ).mvp(mvp); }

void update_camera(double dt)
{
	double f[3];
	forward_vec(app.yaw, app.pitch, f);
	double move[3] = {};
	const double rx = -f[2], rz = f[0];	// right, unnormalised in the horizontal plane
	const double rl = std::sqrt(rx * rx + rz * rz);
	auto add = [&](double sf, double sr, double su)
	{
		double fx = f[0], fy = f[1], fz = f[2];
		if (app.walk)	// walking stays in the horizontal plane
		{
			const double l = std::sqrt(fx * fx + fz * fz);
			fx /= l; fy = 0; fz /= l;
		}
		move[0] += sf * fx + sr * rx / rl;
		move[1] += sf * fy + su;
		move[2] += sf * fz + sr * rz / rl;
	};
	if (held("camera_forward")) { add(1, 0, 0); }
	if (held("camera_back")) { add(-1, 0, 0); }
	if (held("camera_right")) { add(0, 1, 0); }
	if (held("camera_left")) { add(0, -1, 0); }
	if (held("camera_zoom_in")) { add(1, 0, 0); }	// the free camera has no focus: zoom moves along the view (MINE)
	if (held("camera_zoom_out")) { add(-1, 0, 0); }
	if (held("debug.fly_up")) { add(0, 0, 1); }
	if (held("debug.fly_down")) { add(0, 0, -1); }
	double speed = (app.walk ? kWalkSpeed : kFlySpeed * app.speedScale) * dt;
	if (held("debug.fly_fast")) { speed *= kFastFactor; }
	// camera_rotate_left / right (Q / E): turn in place (MINE rate; the original orbits its focus)
	constexpr double kRotateRate = 1.5;	// rad/s
	if (!app.follow) { app.yaw += ((held("camera_rotate_right") ? 1 : 0) - (held("camera_rotate_left") ? 1 : 0)) * kRotateRate * dt; }
	for (int i = 0; i < 3; i++) { app.pos[i] += move[i] * speed; }
	const double lim = kHalfWorld - 1;
	app.pos[0] = std::fmax(-lim, std::fmin(lim, app.pos[0]));
	app.pos[2] = std::fmax(-lim, std::fmin(lim, app.pos[2]));
	if (app.walk) { app.pos[1] = app.hm->height(app.pos[0], app.pos[2]) + kEyeHeight; }
}

// The primary selection's move speed, the speed diamond's mode (0 walk, 1 jog, 2 run, 3 group): CHAR_DYNAMIC flag bits 17-18 (sim da83315e)
int move_speed()
{
	const auto& sel = app.sim.selected();
	const auto c = sel.empty() ? app.sim.decoder().chars.end() : app.sim.decoder().chars.find(sel.front());
	return c != app.sim.decoder().chars.end() ? (int)(c->second.cur.flags >> 17) & 3 : 2;
}

double wrap_angle(double a) { return std::remainder(a, 2 * M_PI); }

bool driving() { return app.driveSent != 0 || app.strafeSent != 0; }

// The camera in use, named once for the F12 sidecar and the follow-state log (kenshi-k16.2.291.1): drive while W/S/autorun go out, else follow,
// else rts, else free. (V is the follow camera; it leaves app.rts set, so rts alone does not say what is on screen.)
const char* camera_mode_name() { return driving() ? "drive" : app.follow ? "follow" : app.rts ? "rts" : "free"; }

void send_drive(uint8_t drive, double heading, int strafe = 0)
{
	if (!app.sim.drive(app.followId, drive, (float)wrap_angle(heading), strafe)) { return; }
	app.driveSent = drive;
	app.strafeSent = strafe;
	app.headingSent = heading;
	app.driveAge = 0;
	if (app.scripted())
	{
		const simlink::Dynamic& d = app.sim.decoder().chars.at(app.followId).cur;
		printf("drive %u strafe %d heading %.2f (server: drive %u strafe %d heading %.2f driveSeq %u flags %x pos %.1f %.1f fwd %.2f)\n", drive, strafe, wrap_angle(heading), d.drive, (d.flags >> 23 & 1) - (d.flags >> 22 & 1), d.heading, d.driveSeq, d.flags, d.pos[0], d.pos[2], d.fwd);
	}
}

void set_follow(bool on)
{
	if (!on)
	{
		if (driving()) { send_drive(0, app.headingSent, 0); }
		app.follow = false;
		app.autorun = false;
		app.pitch -= app.followPitchAdj; app.followPitchAdj = 0; app.followRise = 0; app.followRide = 0; app.followEase = 1e9;	// the drive camera's look-down at its raised eye (k16.2.289.4) is not the free camera's pitch
		return;
	}
	// the selection first (the original); with no player squad, the watched character (a click's inspect focus)
	set_follow_id(!app.sim.selected().empty() ? app.sim.selected()[0] : app.inspect);
}

// The floating camera on: from the free camera, the focus is where the view meets the ground; center = a character to auto-center on (0 = none).
void set_rts(uint64_t center)
{
	set_follow(false);
	if (!app.rts)
	{
		double f[3];
		forward_vec(app.yaw, app.pitch, f);
		const double above = app.pos[1] - app.hm->height(app.pos[0], app.pos[2]);
		app.rtsDist = f[1] < -0.1 && above > 0 ? std::clamp(above / -f[1], 10.0, 2000.0) : 400;
		for (int k = 0; k < 3; k++) { app.focus[k] = app.pos[k] + f[k] * app.rtsDist; }
		app.focus[1] = ground_under(app.focus[0], app.focus[2], &app.focusOn);
		app.rts = true;
	}
	app.rtsEase = 1e9; app.rtsRide = 0;	// a new orbit: the direction window's length and ride height start here (kenshi-k16.2.353.4)
	app.centerId = center;
	if (center) { app.buildings.engage(); }
}

void free_camera()
{
	set_follow(false);
	app.rts = false;
	app.centerId = 0;
}

// The follow target while the camera follows one of the player's characters (the RTS auto-centre, F / a portrait double click; not direct drive, which keeps
// its own rules) is the leader of the selection: the first of it in squad order (the squads window's order: the squads in the host's order, each one's
// members in theirs; user 2026-11-15, kenshi-k16.2.293.1). A selection that grows or shrinks through the bar, a box or the keys re-picks it; a plain
// portrait click is the one-member case, so it moves the follow to that character. A click on a character in the world does not call this (lead default).
uint64_t squad_leader(const std::vector<uint64_t>& sel)
{
	uint64_t best = 0;
	size_t bestRank = SIZE_MAX, rank = 0;
	const simlink::PlayerSquads& ps = app.sim.decoder().playerSquads;
	for (const simlink::PlayerSquads::Squad& q : ps.squads)
	{
		for (uint64_t m : q.members)
		{
			if (std::find(sel.begin(), sel.end(), m) != sel.end() && rank < bestRank) { bestRank = rank; best = m; }
			rank++;
		}
	}
	return best ? best : sel.empty() ? 0 : sel.front();	// a member in no squad (no PLAYER_SQUADS yet): the selection's own first
}

// A click on a portrait (Shift: add); the script's `portrait`.
void portrait_click(uint64_t id, bool add)
{
	app.inspect = 0;
	app.panelProducer = 0;
	app.sim.select(id, add);
	follow_retarget();
}

// A portrait double click / cast row double click: a player's selects and centres (the original), anyone else's is the watch: inspect and follow.
void portrait_focus(uint64_t id)
{
	if (app.sim.isPlayer(id)) { app.inspect = 0; app.panelProducer = 0; app.sim.select(id, false); set_rts(id); }
	else { app.inspect = id; set_follow_id(id); }
}

// A portrait gesture, resolved through the control-hints list (kenshi-k16.2.294.4): the HUD's closures and the script's `portrait` run the action the
// panel lists for it, so the label and the behaviour are one code path.
void portrait_gesture(uint64_t id, hints::Gesture g)
{
	const hints::Context ctx = hints_context();
	const hints::Hover h = hints::hints_for_portrait(id, ctx);
	const hints::Action* a = hints::match(h, g, ctx);
	if (!a) { return; }
	a->run();
	if (app.scripted()) { printf("%s (%s)\n", a->log.c_str(), a->kind); }
}

void follow_retarget()
{
	if (app.follow || !app.rts || !app.centerId || app.sim.selected().empty()) { return; }
	const uint64_t lead = squad_leader(app.sim.selected());
	if (lead && lead != app.centerId)
	{
		set_rts(lead);
		if (app.scripted()) { printf("follow target: %llu (first of the selection in squad order)\n", (unsigned long long)lead); }
	}
}

// The screenshot key's folder: screenshot.dir, else the per-user data root's kclient/screenshots (app/userdirs.hpp; on Windows
// %LOCALAPPDATA%\OpenKenshi\data, kenshi-379.31.3).
std::filesystem::path shot_dir()
{
	if (!kShotDir.get().empty()) { return kShotDir.get(); }
	return std::filesystem::path(plat::userDataDir()) / "kclient" / "screenshots";
}

// The HDR luminance levels (Hdr::read, a GPU read) for a done that prints or writes them: a readback request made right before the shot it
// belongs to, so its done sets app.hdrSceneLum / hdrAdapted before the shot's done reads them (kenshi-k16.2.205.12.24; on one thread the
// read happens at the serve point, where the shot's done read them before).
static void request_hdr_read()
{
	readback::sample([] {
		float lum = 0, adapted = 0;
		app.hdr.read(&lum, &adapted);
		return std::vector<float>{ lum, adapted };
	}, [](bool ok, const std::vector<float>& v) {
		if (ok) { app.hdrSceneLum = v[0]; app.hdrAdapted = v[1]; }
	});
}

// The camera sidecar (kenshi-k16.2.134.1): the camera a save was made from, kept beside the client's other per-user files. One file per save and
// server, under cameras/<server>-<data hash>/<save name>.camera: a --simlink join may serve a different world under the same save name. The
// identity is the HELLO's (the server's name and its game data), which stays put across restarts; the client's own server is a OpenKenshi-server too.
std::filesystem::path camera_sidecar_dir()
{
	std::string server = app.sim.decoder().serverName();
	if (server.empty()) { server = "server"; }
	char hash[32];
	snprintf(hash, sizeof hash, "%016llx", (unsigned long long)app.sim.decoder().serverDataHash());
	std::string dir = server + "-" + hash;
	for (char& c : dir)
	{
		const unsigned char u = (unsigned char)c;
		if (!((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '-' || c == '_' || c == '.')) { c = '_'; }
	}
	return shot_dir().parent_path() / "cameras" / dir;
}

std::string camera_sidecar_file(const std::string& save)
{
	std::string n;
	for (char c : save)
	{
		const unsigned char u = (unsigned char)c;
		n += ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || c == '-' || c == '_' || c == '.' || c == ' ') ? c : '_';
	}
	if (n.empty()) { n = "save"; }
	return n + ".camera";
}

CameraState camera_state_now()
{
	CameraState s;
	s.mode = app.follow ? 2 : app.rts ? 1 : 0;
	s.yaw = app.yaw;
	s.pitch = app.pitch;
	for (int k = 0; k < 3; k++) { s.pos[k] = app.pos[k]; s.focus[k] = app.focus[k]; }
	s.rtsDist = app.rtsDist;
	s.centerId = app.centerId;
	s.followId = app.followId;
	s.followDist = app.followDist;
	s.followBase = app.followBase;
	s.followOffset = app.followOffset;
	s.floorLevel = app.buildings.level();
	s.valid = true;
	return s;
}

// One line for the log, degrees as the script's camera ops take them (the sidecar file keeps the same numbers).
std::string camera_state_line(const CameraState& s)
{
	static const char* const kModes[] = { "free", "rts", "follow" };
	const double d = 180.0 / M_PI;
	char buf[640];
	snprintf(buf, sizeof buf, "mode %s yaw %.1f pitch %.1f pos %.1f,%.1f,%.1f focus %.1f,%.1f,%.1f dist %.1f center %llu follow %llu dist %.1f base %.1f offset %.1f",
		kModes[s.mode >= 0 && s.mode <= 2 ? s.mode : 0], s.yaw * d, s.pitch * d, s.pos[0], s.pos[1], s.pos[2], s.focus[0], s.focus[1], s.focus[2],
		s.rtsDist, (unsigned long long)s.centerId, (unsigned long long)s.followId, s.followDist, s.followBase * d, s.followOffset * d);
	return buf;
}

// Written when the client issues a save, from the camera as it stands then (the order itself goes out beside it). The world map's view goes in the
// same file (k16.2.435): not while its default is still waiting for the MAP page (the load computes that default again).
void camera_sidecar_save(const std::string& name)
{
	const CameraState s = camera_state_now();
	const bool hasMap = app.uiOn && app.overview && !app.mapViewPending;
	const ui::MapView mapView = hasMap ? app.overview->mapView() : ui::MapView{};
	const std::filesystem::path path = camera_sidecar_dir() / camera_sidecar_file(name);
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	const std::filesystem::path tmp = path.string() + ".tmp";
	{
		std::ofstream out(tmp);
		out.precision(17);	// the numbers must come back as they went out, not to six digits
		const double d = 180.0 / M_PI;
		out << "# kclient camera sidecar (kenshi-k16.2.134.1): the camera save \"" << name << "\" was made from\n"
			<< "save=" << name << "\n"
			<< "mode=" << (s.mode == 0 ? "free" : s.mode == 2 ? "follow" : "rts") << "\n"
			<< "yaw=" << s.yaw * d << "\npitch=" << s.pitch * d << "\n"
			<< "pos=" << s.pos[0] << " " << s.pos[1] << " " << s.pos[2] << "\n"
			<< "focus=" << s.focus[0] << " " << s.focus[1] << " " << s.focus[2] << "\n"
			<< "rts_dist=" << s.rtsDist << "\ncenter_id=" << (unsigned long long)s.centerId << "\n"
			<< "follow_id=" << (unsigned long long)s.followId << "\nfollow_dist=" << s.followDist << "\n"
			<< "follow_base=" << s.followBase * d << "\nfollow_offset=" << s.followOffset * d << "\n"
			<< "floor_level=" << s.floorLevel << "\n";
		if (hasMap)	// k16.2.435: the world map's view (an older sidecar has no lines: its load starts at the default)
		{
			out << "map_zoom=" << mapView.zoom << "\n";
		}
		if (!out) { fprintf(stderr, "kclient: camera sidecar: cannot write %s\n", tmp.string().c_str()); return; }
	}
	std::filesystem::rename(tmp, path, ec);
	if (ec) { fprintf(stderr, "kclient: camera sidecar: cannot rename %s\n", path.string().c_str()); return; }
	printf("camera sidecar: saved \"%s\": %s (%s)\n", name.c_str(), camera_state_line(s).c_str(), path.string().c_str());
	if (hasMap) { printf("map view: saved \"%s\": zoom %.6f\n", name.c_str(), mapView.zoom); }
	else { printf("map view: not saved with \"%s\": the default overview is not applied yet\n", name.c_str()); }
}

bool camera_sidecar_read(const std::string& name, CameraState& s)
{
	const std::filesystem::path path = camera_sidecar_dir() / camera_sidecar_file(name);
	std::ifstream in(path);
	if (!in) { printf("camera sidecar: none for \"%s\" (%s)\n", name.c_str(), path.string().c_str()); return false; }
	std::map<std::string, std::string> kv;
	std::string line;
	while (std::getline(in, line))
	{
		const size_t eq = line.find('=');
		if (line.empty() || line[0] == '#' || eq == std::string::npos) { continue; }
		kv[line.substr(0, eq)] = line.substr(eq + 1);
	}
	const auto num = [&](const char* key, double def) { const auto it = kv.find(key); return it == kv.end() ? def : atof(it->second.c_str()); };
	const auto id = [&](const char* key) { const auto it = kv.find(key); return it == kv.end() ? (uint64_t)0 : std::strtoull(it->second.c_str(), nullptr, 10); };
	const auto pair3 = [&](const char* key, double out[3]) { const auto it = kv.find(key); return it != kv.end() && sscanf(it->second.c_str(), "%lf %lf %lf", &out[0], &out[1], &out[2]) == 3; };
	const std::string mode = kv.count("mode") ? kv["mode"] : "";
	s.mode = mode == "free" ? 0 : mode == "follow" ? 2 : mode == "rts" ? 1 : -1;
	if (s.mode < 0 || !pair3("pos", s.pos) || !pair3("focus", s.focus))
	{
		printf("camera sidecar: \"%s\" is unreadable (%s); the camera is not moved\n", name.c_str(), path.string().c_str());
		return false;
	}
	const double r = M_PI / 180.0;
	s.yaw = num("yaw", 0) * r;
	s.pitch = num("pitch", -30) * r;
	s.rtsDist = num("rts_dist", 400);
	s.centerId = id("center_id");
	s.followId = id("follow_id");
	s.followDist = num("follow_dist", 60);
	s.followBase = num("follow_base", 0) * r;
	s.followOffset = num("follow_offset", 0) * r;
	s.floorLevel = (int)num("floor_level", -1);
	// k16.2.435: the world map's view, when the sidecar has it (an older one has not: the load starts at the default overview)
	s.hasMapView = false;
	// (map_cx / map_cz of an older sidecar are ignored: the map always opens on the camera, k16.2.435.5)
	if (kv.count("map_zoom"))
	{
		const double z = num("map_zoom", -1);
		if (z >= 0 && std::isfinite(z)) { s.hasMapView = true; s.mapZoom = z; }
	}
	s.valid = true;
	return true;
}

void camera_sidecar_delete(const std::string& name)
{
	const std::filesystem::path path = camera_sidecar_dir() / camera_sidecar_file(name);
	std::error_code ec;
	if (std::filesystem::remove(path, ec) && !ec) { printf("camera sidecar: deleted with save \"%s\" (%s)\n", name.c_str(), path.string().c_str()); }
}

void camera_restore_apply(const std::string& name, const CameraState& s)
{
	set_follow(false);
	app.rts = false;
	app.centerId = 0;
	app.driveSent = 0; app.strafeSent = 0;
	app.yaw = s.yaw;
	app.pitch = s.pitch;
	for (int k = 0; k < 3; k++) { app.pos[k] = s.pos[k]; app.focus[k] = s.focus[k]; }
	app.rtsDist = std::clamp(s.rtsDist, 10.0, 2000.0);
	if (s.mode == 1)
	{
		app.rts = true;
		app.centerId = s.centerId;	// update_rts drops it while the character is not placed
	}
	else if (s.mode == 2 && s.followId && set_follow_id(s.followId))
	{
		app.followDist = std::clamp(s.followDist, 10.0, 2000.0);
		app.followBase = s.followBase;
		app.followOffset = s.followOffset;
		app.yaw = app.followBase + app.followOffset;
		app.pitch = s.pitch;	// set_follow_id's -15 is the default, not this save's
	}
	else if (s.mode == 2)
	{
		// the followed character has not streamed in yet: the free camera at the saved eye, whose interest streams it (a later C follows again)
		printf("camera sidecar: \"%s\": followed character %llu is not drawn; free camera at the saved eye\n", name.c_str(), (unsigned long long)s.followId);
	}
	// the saved floor view is respected (user 2026-11-15): the level comes back and the subject it centres on is adopted without a snap; an older sidecar
	// (no level) is a plain start: the engage the camera restore made above snaps.
	if (s.floorLevel >= 0) { app.buildings.restoreView(s.floorLevel, camera_subject()); }
	// k16.2.435: the save's world map view with the camera; an older sidecar's load keeps the default armed (it applies on the MAP page's first layout)
	if (s.hasMapView && app.uiOn && app.overview)
	{
		ui::MapView v = app.overview->mapView();
		v.zoom = s.mapZoom;
		app.overview->setMapView(v);	// the zoom only; the centre is the camera's at each open
		app.mapViewPending = false;
		printf("map view: restored \"%s\": zoom %.6f\n", name.c_str(), s.mapZoom);
	}
	printf("camera sidecar: restored \"%s\": %s\n", name.c_str(), camera_state_line(camera_state_now()).c_str());
}

// The load's first snapshot after its reset (kenshi-k16.2.134.1): apply the armed sidecar once. A paused sim sends no snapshots until it is
// unpaused, so a load taken while paused applies at once (the clock's pause bit is the last CLOCK the server sent). No sidecar: nothing moves.
void camera_restore_frame()
{
	CamRestore& r = app.camRestore;
	if (!r.armed || (app.sim.decoder().snapSerial == r.snapAt && !app.sim.decoder().clock.paused)) { return; }
	r.armed = false;
	CameraState s;
	if (camera_sidecar_read(r.name, s)) { camera_restore_apply(r.name, s); r.applied = true; }
}

// The world map's default view (k16.2.435: a deliberate deviation from the original, whose map opened fitted and kept nothing): centred on the Hub,
// Stack just inside the top edge. Derived from the two towns' world positions and the MAP page's height; false when a record is missing (a mod).
bool map_view_default(const ui::MapFrame& frame, ui::MapView& v)
{
	const auto named = [](const char* name) -> const Town*
	{
		for (const Town& t : app.towns.towns) { if (t.name == name) { return &t; } }
		return nullptr;
	};
	const Town* hub = named("The Hub");
	const Town* stack = named("Stack");
	const char* missing = !hub ? "The Hub" : !stack ? "Stack" : nullptr;
	if (missing) { fprintf(stderr, "kclient: map view: no town \"%s\" in the records (a mod?); the map starts fitted\n", missing); return false; }
	const double north = hub->z - stack->z;	// Stack north of the Hub: the map is north-up, so a smaller z is higher on the screen
	if (!(north > 0))
	{
		fprintf(stderr, "kclient: map view: \"Stack\" (%.1f, %.1f) is not north of \"The Hub\" (%.1f, %.1f); the map starts fitted\n", stack->x, stack->z, hub->x, hub->z);
		return false;
	}
	const ui::MapState state;	// the host fills only the camera, the characters and the places: the image is the UI's default (uihost.cpp update_ui)
	// Pixels of clearance above Stack's point: the town icon is 48 units tall, halved for its centre by the page's own level sizes (18 here), a little
	// air, and the scroll bar's strip, which the default's zoom turns on after the page's first layout with this view (14 units).
	const double margin = 40.0 * ui::scale();
	const double ppu = (frame.h / 2 - margin) / north;	// pixels a world unit: half the view's height is that distance, less the margin
	v.zoom = ppu * state.extent / state.texels;	// pixels a map texel, the view's own unit (MapArea::ppu is texels x zoom / extent)
	v.cx = hub->x;
	v.cz = hub->z;
	printf("map view: default zoom %.6f, on \"The Hub\" (%.1f, %.1f), \"Stack\" %.1f u north (view %.0f x %.0f)\n", v.zoom, hub->x, hub->z, north, frame.w, frame.h);
	return true;
}

// The pending default (k16.2.435): set on the first frame the MAP page has a rect (a hidden page is never laid out) and the towns are loaded.
void map_view_frame()
{
	if (!app.uiOn || !app.overview || !app.mapViewPending || !app.townsReady) { return; }
	const ui::MapFrame frame = app.overview->mapFrame();
	if (!(frame.w > 0 && frame.h > 0)) { return; }
	ui::MapView v;
	if (!map_view_default(frame, v)) { v = {}; }	// the entire world, as the page started before this change
	app.overview->setMapView(v);
	app.overview->centreMap();	// the default's centre is only a placeholder: the map is on the camera (k16.2.435.5; the open that waited for this frame included)
	app.mapViewPending = false;
}

// Moves the camera to (x, z), keeping its height above the ground.
void teleport(double x, double z)
{
	if (app.rts)
	{
		app.focus[0] = x;
		app.focus[2] = z;
		app.centerId = 0;
		return;
	}
	const double above = app.pos[1] - app.hm->height(app.pos[0], app.pos[2]);
	app.pos[0] = x;
	app.pos[2] = z;
	app.pos[1] = app.hm->height(x, z) + above;
}

std::string lower(std::string v)
{
	for (char& c : v) { c = (char)std::tolower((unsigned char)c); }
	return v;
}

const cfg::Text kDataDir{ cfg::Group{ "Paths" }, "client.data_dir", "", "Our own data files (UI layouts): empty = client/data, found from the executable (client/build/kclient), else (dev build) the source tree it was built from" };

// client.data_dir; else <exe dir>/../data (a build dir directly under client/); else a dev build's source tree client/data (a build in any other dir, kenshi-cym.40.2.13).
// What it picked is logged once; none found keeps the exe-relative path, which session.cpp's fatal line names with the cfg key.
std::string client_data_dir()
{
	static std::string picked;
	if (!kDataDir.get().empty()) { return kDataDir.get(); }
	if (!picked.empty()) { return picked; }
	std::error_code ec;
	std::string how = "beside the executable";
	std::filesystem::path p = kutil::executablePath(ec).parent_path().parent_path() / "data";
#ifdef KCLIENT_DEV_TOOLS
	if (!std::filesystem::is_directory(p / "ui", ec))
	{
		const std::string src = dev::sourceTreeDataDir();
		if (!src.empty()) { p = src; how = "the source tree this build was configured from"; }
	}
#endif
	picked = p.string();
	fprintf(stderr, "kclient: data dir %s (%s)\n", picked.c_str(), how.c_str());
	return picked;
}

// The repo root, found from the executable (client/build/kclient -> the repo root, one more level): where playtests/ and .beads/ are.
std::string repo_root()
{
	std::error_code ec;
	return (kutil::executablePath(ec).parent_path().parent_path().parent_path()).string();
}


void ensure_weathers()
{
	if (app.weathers.empty() && !app.weathersFut.valid()) { app.weathersFut = std::async(std::launch::async, SkyWeather::loadAll, app.opt.data); }
}

// Applies the pending weather request / cycle steps once the list is loaded ("default" never needs it). The cycle ring is auto, default, then the WEATHER records.
void apply_weather()
{
	if (lower(app.wantWeather) == "default") { app.sky.weather = SkyWeather(); app.weatherIdx = 0; app.wantWeather.clear(); }
	if (app.weathers.empty()) { return; }
	const int n = (int)app.weathers.size();
	if (!app.wantWeather.empty())
	{
		app.weatherIdx = 0;
		for (int i = 0; i < n; i++) { if (lower(app.weathers[i].name) == lower(app.wantWeather)) { app.weatherIdx = i; } }
		if (app.weatherIdx == 0) { fprintf(stderr, "kclient: no WEATHER record named '%s', using default\n", app.wantWeather.c_str()); }
		app.wantWeather.clear();
	}
	if (app.weatherCycle)
	{
		const int cur = (((app.weatherAuto ? 0 : app.weatherIdx + 1) + app.weatherCycle) % (n + 1) + n + 1) % (n + 1);
		app.weatherAuto = cur == 0;
		if (cur > 0) { app.weatherIdx = cur - 1; }
		app.weatherCycle = 0;
	}
	if (!app.weatherAuto) { app.sky.weather = app.weathers[app.weatherIdx]; }
}

// The WEATHER list has loaded: the default first, the current weather's index, then the pending request.
void take_weathers()
{
	app.weathers = app.weathersFut.get();
	app.weathers.insert(app.weathers.begin(), SkyWeather());
	app.weatherIdx = 0;
	for (int i = 1; i < (int)app.weathers.size(); i++) { if (app.weathers[i].name == app.sky.weather.name) { app.weatherIdx = i; } }
	apply_weather();
}

void set_weather(const std::string& name)
{
	app.weatherAuto = lower(name) == "auto";
	if (app.weatherAuto) { return; }
	app.wantWeather = name;
	if (lower(name) != "default") { ensure_weathers(); }
	// scripted (scene shots): the list now, so the weather and its particle effects are in place on the scene's first frame, right after the
	// particle reset; a frame later the first scene to need the list drew a different dust haze from the same scene run after another (k16.2.67)
	if (app.scripted() && app.weathersFut.valid()) { take_weathers(); return; }
	apply_weather();
}

sapp_keycode key_by_name(const std::string& n)
{
	static const std::map<std::string, sapp_keycode> names = {
		{ "SPACE", SAPP_KEYCODE_SPACE }, { "SHIFT", SAPP_KEYCODE_LEFT_SHIFT }, { "CTRL", SAPP_KEYCODE_LEFT_CONTROL }, { "ALT", SAPP_KEYCODE_LEFT_ALT },
		{ "TAB", SAPP_KEYCODE_TAB }, { "BACKSPACE", SAPP_KEYCODE_BACKSPACE }, { "F2", SAPP_KEYCODE_F2 }, { "F3", SAPP_KEYCODE_F3 }, { "F7", SAPP_KEYCODE_F7 }, { "ESCAPE", SAPP_KEYCODE_ESCAPE },
		{ "PLUS", SAPP_KEYCODE_EQUAL }, { "MINUS", SAPP_KEYCODE_MINUS },
		{ "LBRACKET", SAPP_KEYCODE_LEFT_BRACKET }, { "RBRACKET", SAPP_KEYCODE_RIGHT_BRACKET }, { "GRAVE", SAPP_KEYCODE_GRAVE_ACCENT },
		{ "HOME", SAPP_KEYCODE_HOME }, { "END", SAPP_KEYCODE_END }, { "DELETE", SAPP_KEYCODE_DELETE }, { "ENTER", SAPP_KEYCODE_ENTER }, { "LEFT", SAPP_KEYCODE_LEFT }, { "RIGHT", SAPP_KEYCODE_RIGHT }, { "PAGEUP", SAPP_KEYCODE_PAGE_UP }, { "PAGEDOWN", SAPP_KEYCODE_PAGE_DOWN }, { "COMMA", SAPP_KEYCODE_COMMA }, { "PERIOD", SAPP_KEYCODE_PERIOD },
	};
	if (n.size() == 1 && n[0] >= '1' && n[0] <= '9') { return (sapp_keycode)(SAPP_KEYCODE_1 + (n[0] - '1')); }
	if (n.size() == 4 && n.compare(0, 3, "KP_") == 0 && std::isdigit((unsigned char)n[3])) { return (sapp_keycode)(SAPP_KEYCODE_KP_0 + (n[3] - '0')); }
	if (n.size() == 1 && n[0] >= 'A' && n[0] <= 'Z') { return (sapp_keycode)(SAPP_KEYCODE_A + (n[0] - 'A')); }
	if (n.size() >= 2 && n[0] == 'F' && std::isdigit((unsigned char)n[1]) && std::atoi(n.c_str() + 1) >= 1 && std::atoi(n.c_str() + 1) <= 12) { return (sapp_keycode)(SAPP_KEYCODE_F1 + std::atoi(n.c_str() + 1) - 1); }
	auto it = names.find(n);
	return it == names.end() ? SAPP_KEYCODE_INVALID : it->second;
}

// Script lines: "<frame> <op> args", '#' comments. See README.md.
bool load_script(const std::string& path)
{
	std::ifstream in(path);
	if (!in) { fprintf(stderr, "kclient: cannot open script %s\n", path.c_str()); return false; }
	std::string line;
	for (int ln = 1; std::getline(in, line); ln++)
	{
		line = line.substr(0, line.find('#'));
		std::istringstream ss(line);
		Cmd c;
		std::string w;
		if (!(ss >> c.frame)) { if (line.find_first_not_of(" \t\r") != std::string::npos) { fprintf(stderr, "%s:%d: expected frame number\n", path.c_str(), ln); return false; } continue; }
		if (!(ss >> c.op)) { fprintf(stderr, "%s:%d: missing command\n", path.c_str(), ln); return false; }
		while (ss >> w) { c.args.push_back(w); }
		app.cmds.push_back(c);
	}
	return true;
}

void script_error(int frame, const char* op)	// the dev ops (dev/debughost.cpp) report a bad command through dev::Context::scriptError
{
	fprintf(stderr, "kclient: bad script command at frame %d: %s\n", frame, op);
	fflush(stdout);
	exit_now(2);	// not exit(): the static App's destructors join worker threads from inside a frame and hang (Grass::~Grass)
}

void script_error(const Cmd& c) { script_error(c.frame, c.op.c_str()); }

// A character on the wire by its display name or "id=N" (the follow script op and --follow): 0 when none is drawn.
uint64_t named_character(const std::string& text)
{
	double at[3];
	float hull[3];
	for (const auto& [id, ch] : app.sim.decoder().chars)
	{
		const bool hit = text.rfind("id=", 0) == 0 ? std::to_string(id) == text.substr(3) : app.sim.decoder().str(ch.look.name) == text;
		if (hit && app.sim.placed(id, at, hull)) { return id; }
	}
	return 0;
}

static void warn_frozen_anim_log(const char* op);

// pick_screen for the dev `dump pick` (dev/debughost.cpp): Pick::kind, id and hit (Pick itself stays here)
static void dev_pick_at(double mx, double my, int& kind, uint64_t& id, double hit[3], std::vector<dev::PickSkip>& skipped)
{
	std::vector<PickSkip> sk;
	const Pick p = pick_screen(mx, my, &sk);
	kind = (int)p.kind;
	id = p.id;
	for (int k = 0; k < 3; k++) { hit[k] = p.hit[k]; }
	for (const PickSkip& s : sk) { skipped.push_back({ s.name, { s.at[0], s.at[1], s.at[2] }, s.why }); }
}

// pick_screen for the shot sidecar (dev/shotsidecars.cpp): the whole Pick, its `building` flag and the skips included.
static Pick dev_pick_screen(double mx, double my, std::vector<dev::PickSkip>& skipped)
{
	std::vector<PickSkip> sk;
	const Pick p = pick_screen(mx, my, &sk);
	for (const PickSkip& s : sk) { skipped.push_back({ s.name, { s.at[0], s.at[1], s.at[2] }, s.why }); }
	return p;
}

// The keys down now as the shot's .txt lists them (SAPP_KEYCODE order), " none" when none is.
static std::string shot_hud_text();	// defined after hud_lines: the developer text block for the shot sidecar (kenshi-k16.2.335.1)

static std::string keys_held()
{
	std::string held;
	for (int k = 0; k < SAPP_MAX_KEYCODES; k++) { if (app.keys[k]) { held += " " + ui::keys::name({ k, 0 }); } }
	return held.empty() ? " none" : held;
}

// The control-hints block for a pick the sidecar already made (the `hints:` lines; the same text `hints at X Y` prints for that point).
static std::string hints_for_pick(const Pick& p) { return hints::format_hover(hints::hints_for(p, hints_context())); }


// The shared context of the dev debug ops (dev/debughost.hpp): the subsystems they read, filled once here, the host side.
static bool world_loading(int* pending = nullptr);	// the scene test's load wait (dev/scenetest.cpp)
static void dev_hud_box(float out[4]);	// main.cpp's hud_box as the scene JSON's four numbers
static void dev_current_camera(float nearZ, float farZ, Camera& out);	// current_camera, by out-parameter
dev::Context& dev_context()
{
	static dev::Context cx{ app.sim, app.chars, app.beams, app.doorSlides, app.frames, app.buildings, app.pos, app.mouseX, app.mouseY,
		app.items, app.gearChar, app.itemsShown, app.itemTarget, app.pending.id, app.conts, app.machine, app.panelProducer, machine_panel, script_error,
		app.sky, app.hdr, app.hdrSceneLum, app.hdrAdapted, app.wx, app.skyRamp, app.weatherInst, kInteriorLight, kSunBounce,
		app.particles, app.particleWorld, app.opt.particles, app.weff,
		app.hm, app.shadow, app.shadowFlip, app.collide, app.progressBars, app.yaw, app.pitch, app.walk, world_to_screen, warn_frozen_anim_log,
		app.nav, app.navTrace, mouse_ray, camera_near, ray_ground,
		[](const double o[3], const double d[3], double h[3], bool* b, const double* e) { return ray_hit(o, d, h, b, e); }, [](const double p[3], double bottom) { return see_through(p, bottom); }, dev_pick_at, hint_report,
		app.physics, app.statics, app.ragdolls, app.foliage, app.trails, app.opt.data, camera_basis,
		app.hideUi, app.sceneName, app.settledChars, app.firstWorldFrame, app.charsReady, app.bldSet, app.bldReady, app.grassOn, app.foliageOn, app.wantWeather,
		app.grass, app.terrain, app.blood, app.gpu, app.opt.exposure, app.particlesDrawn, g_sgErrors, g_exitCode,
		app.rainForce, app.dustForce, app.cloudsForce, app.windForce,
		set_weather, place_characters, place_blood, request_quit, request_hdr_read, dev_hud_box, run_script, world_loading, forward_vec, dev_current_camera,
		app.squads, app.towns, app.townsReady, app.follow, app.rts, app.autorun, kNavOverlay, kNavPruned, debug_toggle,
		app.opt.cmdline, app.opt.debug, app.look, app.weatherMix, app.followId, app.followDist, app.followOffset, app.focus, app.rtsDist, app.focusOn,
		compass, camera_mode_name, keys_held, [] { return app.cursors.current(); }, clicks_report, pick_owner_report, hints_for_pick, dev_pick_screen,
		camera_subject, build_mvp,
#ifdef KCLIENT_DEV_TOOLS
		[] { return app.lightsPanel && app.lightsPanel->shown(); }, [](bool on) { if (app.lightsPanel) { app.lightsPanel->show(on); } },
#else
		[] { return false; }, [](bool) {},
#endif
		[] { return app.options->tabShown("Keys"); }, [](bool on) { if (on) { app.options->showTab("Keys"); } else { app.options->show(false); } }, [] { return app.wmap.full; }, [] { app.wmap.full = !app.wmap.full; }, shot_hud_text
#ifdef KCLIENT_DEBUG_VIEW
		, app.debugView, app.debugSubscribed, app.infoChar, [] { return app.debugTree && app.debugTree->shown(); }, [] { if (app.debugTree) { app.debugTree->show(false); } }, debug_tree_open, tree_state_value
#endif
	};
	return cx;
}


// A scripted run holds the characters' animation still (charDt 0, for repeatable shots) unless the script says `set animate 1`.
// A locomotion log then shows only the clips a state change snapped to: a gait handover (walk -> jog -> run) keeps the linear
// crossfade, which needs time, so the record sticks at the speed the state last changed and reads as a client bug. Say so once when
// such a log starts (kenshi-h2v.3.4.3); the footlog tool's own script sets animate 1 (client/tools/footlog_stop.txt).
static void warn_frozen_anim_log(const char* op)
{
	if (app.scripted() && !app.scriptAnim) { fprintf(stderr, "kclient: %s: the scripted run freezes the characters' animation (set animate 1 to animate them): the clips, weights and phase below are held\n", op); }
}


#ifdef KCLIENT_DEV_TOOLS
// <shot>.route.tsv (kenshi-k16.2.231.1): each selected character's CHAR_ROUTE (or every player character's when none is selected), the last 8
// move_to orders this client sent it, the wire's answer to each, and the character's wire position — what a "stuck at a door" report needs and the
// shot's .txt / trees do not carry. Dev-only, like the trails sidecar; one row a fact, tab-separated, first token the record type.
static std::string tsv_text(const std::string& s)
{
	std::string out = s.empty() ? "-" : s;
	for (char& ch : out) { if (ch == '\t' || ch == '\n' || ch == '\r') { ch = ' '; } }
	return out;
}

static void shot_route(const std::string& shotPath, int frame)	// frame: app.frames when the shot was requested (its done runs a frame later)
{
	if (!app.opt.debug || !app.sim.active()) { return; }
	const std::string base = imagewrite::stripExt(shotPath);
	const std::string path = base + ".route.tsv";
	FILE* f = fopen(path.c_str(), "w");
	if (!f) { fprintf(stderr, "kclient: route: cannot write %s\n", path.c_str()); return; }
	fprintf(f, "# kclient route sidecar (kenshi-k16.2.231.1): the selected characters' CHAR_ROUTE as the wire has it at the shot's frame, the last 8\n");
	fprintf(f, "# orders this client sent each and the answer the wire gave, and the characters' wire position and building. One row a fact.\n");
	fprintf(f, "# order rows: what is the order (attack, follow, stop, hold, job, an interact's name) or a move_to's click origin (click, use point,\n");
	fprintf(f, "# script, map); target is the sim id the order named (only orders that name one); goal is the clicked surface point, or the target's\n");
	fprintf(f, "# wire position at the send (kenshi-4bps.1: not only move_to is logged any more).\n");
	fprintf(f, "# shot\t%s\tframe\t%d\ttick\t%.2f\t%s\n", tsv_text(shotPath).c_str(), frame, app.sim.tick(), tsv_text(app.sim.status()).c_str());
	{
		// The floor view's stair latch (kenshi-k16.2.236.8.4.3): the feet's building-local y against the two switch lines, the origin floor and the level it shows, and
		// the hotspot's consumed state: armed (hot, will fire on the line) or fired (cold until the feet pass rearm_y, about 2 steps past the line).
		const auto& li = app.buildings.latchInfo();
		if (li.on)
		{
			fprintf(f, "floor\tlatch\ty\t%.2f\tup_line\t%.2f\tlow_line\t%.2f\torigin\t%d\tshown\t%d\t%s\trearm_y\t%.2f\tto_rearm\t%.2f\n", li.y, li.upperLine, li.lowerLine, li.origin, li.side,
				li.fired ? "fired" : "armed", li.rearmY, li.fired ? li.rearmY - li.y : 0.0);
		}
		else { fputs("floor\tlatch\toff\n", f); }
	}
	std::vector<uint64_t> ids = app.sim.selected();
	if (ids.empty()) { ids = app.sim.players(); }
	double pos[3];
	for (const uint64_t id : ids)
	{
		fprintf(f, "char\t%llu\t%s\t", (unsigned long long)id, tsv_text(app.sim.charName(id)).c_str());
		if (app.sim.simPos(id, pos)) { fprintf(f, "pos\t%.2f\t%.2f\t%.2f", pos[0], pos[1], pos[2]); }
		else { fputs("pos\tnone", f); }
		const auto ch = app.sim.decoder().chars.find(id);
		if (ch != app.sim.decoder().chars.end() && ch->second.samples > 0)
		{
			const simlink::Dynamic& d = ch->second.cur;
			fprintf(f, "\twireBuilding\t%llu\twireFloor\t%d\tyValid\t%d\tfwd\t%.2f\tlat\t%.2f", (unsigned long long)d.building, (int)d.floor,
				(d.flags & simlink::kYValid) ? 1 : 0, d.fwd, d.lat);
		}
		fputc('\n', f);
		const simlink::Route* rt = app.sim.routeOf(id);
		if (rt && !rt->points.empty())
		{
			fprintf(f, "route\t%llu\tversion\t%u\tclientSeq\t%u\tflags\t%u\ttruncated\t%d\tapproach\t%d\tarrivedTick\t%llu\twaypoints\t%zu\n",
				(unsigned long long)id, rt->version, rt->clientSeq, (unsigned)rt->flags, rt->flags & 1 ? 1 : 0, rt->flags & 2 ? 1 : 0,
				(unsigned long long)rt->arrivedTick, rt->points.size());
			for (size_t i = 0; i < rt->points.size(); i++)
			{
				fprintf(f, "waypoint\t%llu\t%zu\t%.3f\t%.3f\t%.3f\n", (unsigned long long)id, i, rt->points[i][0], rt->points[i][1], rt->points[i][2]);
			}
			const auto& g = rt->points.back();
			fprintf(f, "goal\t%llu\t%.3f\t%.3f\t%.3f\n", (unsigned long long)id, g[0], g[1], g[2]);
		}
		else if (rt) { fprintf(f, "noroute\t%llu\tversion\t%u\tclientSeq\t%u\tflags\t%u\tarrivedTick\t%llu\n", (unsigned long long)id, rt->version, rt->clientSeq,
			(unsigned)rt->flags, (unsigned long long)rt->arrivedTick); }
		else { fprintf(f, "noroute\t%llu\n", (unsigned long long)id); }
		for (const SimView::OrderRow& o : app.sim.orders(id))
		{
			// kenshi-4bps.1: an order that names a target gets its key after what; outcome stays last (it is free text)
			char tgt[32] = "";
			if (o.target) { snprintf(tgt, sizeof tgt, "target\t%llu\t", (unsigned long long)o.target); }
			fprintf(f, "order\t%llu\tseq\t%u\ttick\t%llu\tgoal\t%.2f\t%.2f\t%.2f\tyValid\t%d\tqueue\t%d\twhat\t%s\t%sbuilding\t%s\tfloor\t%d\toutcome\t%s\n",
				(unsigned long long)id, o.seq, (unsigned long long)o.tick, o.goal[0], o.goal[1], o.goal[2], o.yValid ? 1 : 0, o.queue ? 1 : 0,
				tsv_text(o.what).c_str(), tgt, tsv_text(o.buildingName).c_str(), o.floor, tsv_text(o.outcome).c_str());
		}
	}
	fclose(f);
	const std::string note = "route: " + std::filesystem::path(path).filename().string() + "\n";
	if (FILE* side = fopen((base + ".txt").c_str(), "a")) { fputs(note.c_str(), side); fclose(side); }
	printf("%s", note.c_str());
}
#endif

// Runs the script commands due at this frame: input before the update (post=false), screenshots, probes and quit for the frame being built
// (post=true, logic_tail): the reads are readback requests served after the frame's last pass, and a quit is queued behind them
// (readback::after), so each runs once the reads before it are done, in the script's order (kenshi-k16.2.205.12.23).
void run_script(bool post)
{
	for (Cmd& c : app.cmds)
	{
		if (c.done || c.frame > app.frames || (c.op == "screenshot" || c.op == "quit" || c.op == "probe") != post) { continue; }
		if (c.op == "lab" && app.opt.debug && dev::labWait()) { continue; }	// the lab's GPU init / resize is a render-queue job: the op runs once its packet is drawn
		c.done = true;
		if (c.op == "probe" && !c.args.empty() && c.args[0] == "height" && !app.opt.debug) { script_error(c.frame, c.op.c_str()); }	// the height probe needs --debug
		else if (c.op == "probe") { dev::probeOp(dev_context(), c.args); }	// a request: the scene depth once the frame is complete (dev/probe.cpp)
		else if (c.op == "screenshot")
		{
			const std::string shot = c.args[0];
			if (!dev::viewing()) { request_hdr_read(); }	// the luminance the done prints, read on the render side
			readback::screenshot(shot, [shot, frame = app.frames](bool) {	// done follows the read (on the logic thread at the next frame start, readback::runDone), so the sidecars below keep their order
			dev::shotSidecars(dev_context(), shot);	// <shot>.txt (owner / primitive / rule lines included, k16.2.234.1), .events.txt, .trails.tsv (dev/shotsidecars.cpp)
#ifdef KCLIENT_DEV_TOOLS
			shot_route(shot, frame);	// <shot>.route.tsv: the selected characters' routes, last move orders and their answers (--debug)
#endif
			dev::shotTreesBegin(dev_context(), shot);	// the on-screen characters' trees, beside the shot (docs/CLIENT.md "Screenshot debug trees")
			dev::shotBegin(shot);	// a --bead run: the shot also goes to the item's findings folder and as a comment (k16.2.127)
			printf("  frame %d pos %.1f %.1f %.1f (ground %.1f) yaw %.1f pitch %.1f%s\n", frame, app.pos[0], app.pos[1], app.pos[2],
				dev::viewing() ? 0.0f : app.hm->height(app.pos[0], app.pos[2]),
				app.yaw * 180 / 3.14159265358979, app.pitch * 180 / 3.14159265358979, app.walk ? " walk" : "");
			if (app.rts) { printf("  camera pivot %.1f %.1f %.1f dist %.1f yaw %.1f pitch %.1f fovy 45 size %dx%d on %s\n", app.focus[0], app.focus[1], app.focus[2], app.rtsDist, app.yaw * 180 / 3.14159265358979, app.pitch * 180 / 3.14159265358979, view::width(), view::height(), app.focusOn); }	// the same shot in the original (kinspect knavshot)
			if (!dev::viewing())
			{
				printf("  gpu ms: total %.2f fog %.2f (%d fog volumes drawn) mask %.2f particles %.2f (%d drawn: %s)\n", framestats::stats().gpuTotal, framestats::stats().gpuMs[GpuTimer::Fog], framestats::stats().fogDrawn,
					framestats::stats().gpuMs[GpuTimer::Mask], framestats::stats().gpuMs[GpuTimer::Particles], app.particlesDrawn, app.particles.describe().c_str());
				printf("  sky: time %.2f %s\n", app.sky.clock.hours, app.sky.describe().c_str());
				printf("  hdr: scene luminance %.4f adapted %.4f (min %.3f max %.3f) exposure %.4f (baked estimate %.4f)%s\n", app.hdrSceneLum, app.hdrAdapted, app.sky.minLum(), app.sky.maxLum(), app.opt.exposure > 0 ? app.opt.exposure : 0.55 / app.hdrAdapted, app.sky.exposure(), app.opt.exposure > 0 ? " FIXED" : "");
			}
			if (app.sim.active()) { printf("  %s\n", app.sim.status().c_str()); }
			if (!dev::viewing() && app.bldReady)
			{
				const WorldBuildings::Stats bs = app.buildings.stats();
				printf("  buildings: %zu instances, %zu draw groups, %zu submesh draws, %zu with a real texture (%.1f%%); meshes ready %zu failed %zu, textures ready %zu failed %zu; id pass %zu draws for %zu instances; frame %.1f ms\n",
					bs.instancesDrawn, bs.groups, bs.submeshDraws, bs.realTextured, bs.submeshDraws ? 100.0 * bs.realTextured / bs.submeshDraws : 0.0,
					bs.meshesReady, bs.meshesFailed, bs.texturesReady, bs.texturesFailed, bs.idDraws, bs.idInstances, app.frameMs);
				printf("  shader modes (groups): plain %zu ALPHA %zu EMISSIVE %zu DUAL %zu TRIPLANAR %zu TERRAIN %zu DUAL_TRIPLANAR %zu LIMB %zu; features drawn %zu\n", bs.modeGroups[0], bs.modeGroups[1], bs.modeGroups[2],
					bs.modeGroups[3], bs.modeGroups[4], bs.modeGroups[5], bs.modeGroups[6], bs.modeGroups[7], bs.featuresDrawn);
				printf("  lights: %zu in view (nearest %d drawn), outdoor %s\n", app.buildings.lightsInView(), (int)std::min<size_t>(app.buildings.lightsInView(), LightParams::kMaxPoints), app.buildings.night ? "on" : "off");
				printf("  fit: %s\n", app.buildings.fitReport().c_str());
			}
			if (app.foliageOn && app.foliage.ready())
			{
				const Foliage::Stats fs = app.foliage.stats();
				printf("  foliage: %zu cells resident (%zu pending), %zu instances, %zu groups; drawn %zu instances in %zu groups, %zu draws; generated %zu cells, gen avg %.0f max %.0f ms/cell (worker), %zu unresolved instances\n",
					fs.cellsLoaded, fs.cellsPending, fs.instancesLoaded, fs.groupsLoaded, fs.instancesDrawn, fs.groupsDrawn, fs.draws, fs.generated,
					fs.generated ? fs.genMs / fs.generated : 0.0, fs.genMaxMs, fs.unresolved);
				printf("  foliage draws: %zu submesh draws (one per node: %zu); shadow candidates merged/flat L0 %zu/%zu L1 %zu/%zu L2 %zu/%zu\n", fs.draws, fs.drawsFlat, fs.shadowCands[0], fs.shadowFlat[0], fs.shadowCands[1], fs.shadowFlat[1], fs.shadowCands[2], fs.shadowFlat[2]);
				printf("  foliage parts: %zu submesh draws held for a streaming diffuse, %zu drawn on a not-ready diffuse\n", fs.held, fs.greyDraws);
				printf("  foliage: %s; %zu of %zu resident instances are children\n", app.foliage.cellReport(app.pos).c_str(), fs.children, fs.instancesLoaded);
				printf("  foliage CPU this frame: cull+list %.2f ms, cell upload %.2f ms, draw submit %.2f ms; whole frame CPU %.2f ms (smoothed %.2f)\n", fs.cullMs, fs.uploadMs, framestats::stats().foliage.renderMs, framestats::stats().cpu.lastMs, framestats::stats().cpu.ms);
			}
			}, [shot](bool, const std::string&) { dev::shotWritten(shot); });	// a --bead run's copy and comment follow the file (no flushWrites on the logic thread)
		}
		else if (post) { const Cmd q = c; readback::after([q](bool) { exec_cmd(q); }); }	// quit: behind this frame's reads
		else { exec_cmd(c); }
	}
}

// The mesh viewer's resources and its mesh (dev/viewmesh.cpp: --view-mesh). The resources are the viewer's alone.
void init_view()
{
	const std::string root = std::filesystem::path(app.opt.data).parent_path().string();
	if (!app.res.init(root)) { fprintf(stderr, "kclient: no resources.cfg under %s\n", root.c_str()); exit(1); }
	const size_t materials = app.mats.load(app.res);
	printf("resources: %zu directories, %zu files; materials: %zu from %zu scripts\n", app.res.dirCount(), app.res.fileCount(), materials, app.mats.files);
	dev::viewInit(app.res, app.mats);
}


// The character creator's scene after the original's editor (findings/charedit.md, live): the original builds it at Ogre (0, -3000, 0), far below the world;
// we build it the same way, kCreatorDrop below the start's terrain, so no terrain, foliage, grass or water can reach it (creator_frame keeps the camera
// 30 / 9.15 behind and the near / far clip at 5 / 200, so none of the world is in view; the render pass skips those layers while the creator is shown).
// The pivot origin sits kCreatorPivot above the platform mesh (BUILDING_PART character_editor_platform_part, its plank top at the instance origin),
// the character kCreatorRoot below the pivot facing the camera (yaw 180). BODY: the orbit pivot at the origin, pitch 27 deg down, the camera 30 behind it;
// FACE / HAIR: the pivot at Bip01 Head, pitch 0, the camera 9.15 behind (the offsets' x of -3 / -0.91 only shifted the character left of the original's
// window centre: creator_frame centres it in the screen's middle rect instead). FOVy 45, near 5, far 200 while shown (kCreatorNear / kCreatorFar).
constexpr double kCreatorPivot = 12, kCreatorRoot = 11.3, kCreatorBodyDist = 30, kCreatorFaceDist = 9.15;
constexpr double kCreatorDrop = 3000;	// the original's editor scene depth below the world (Ogre (0, -3000, 0))
constexpr double kCreatorBodyPitch = 27 * M_PI / 180.0;
constexpr float kCreatorNear = 5, kCreatorFar = 200;
// The preview's orbit (kenshi-k16.2.433.1, user 2026-11-20 ~14:55 and the lead's 14:44 comment on kenshi-k16.2.433): the character and the platform
// are ONE assembly that the drag turns (the original's charedit_chars_rotation node, its platform under the same root: findings/charedit.md), the
// wheel zooms towards the point of the model under the cursor, and the last 30% of the way out drifts the pivot, the tilt, the yaw and the eye back to
// the page's default framing (the default view is the front one, so the turn goes back too: user 2026-11-20, kenshi-k16.2.433.3). It is the NODE that
// turns, not the camera: CharacterEditorLighting's lightDirection (-3,2,-3) is fixed in the scene and tuned to the default framing (a front-upper key),
// so an assembly turned under it keeps the side facing the camera the lit one (a turntable); a camera orbit would carry the camera behind the light,
// where the only light there is leaves the character black, and the platform would not turn with it.
constexpr double kCreatorZoomsPerNotch = 1.2;	// the wheel: one notch moves this much nearer / farther
constexpr double kCreatorResetFrom = 0.7;	// the reset drift runs over the last share (1 - this) of the zoom range, smoothstepped
constexpr double kCreatorMinDistBody = 8, kCreatorMinDistFace = 6;	// the eye's closest (the near clip is 5)
constexpr double kCreatorTiltMax = 60 * M_PI / 180.0;	// short of flipping over
constexpr double kCreatorDragRate = 0.01;	// radians a pixel of drag
constexpr double kCreatorGlideTau = 0.05;	// s: the wheel glide's time constant (about 5 tau = 0.25 s to settle, k16.2.433.4)
constexpr double kCreatorPickRadius = 5;	// the wheel's skeleton pick (Characters::skeletonRay)

static void creator_copy(double* to, const double* from) { for (int k = 0; k < 3; k++) { to[k] = from[k]; } }
static double creator_smoothstep(double t) { t = std::clamp(t, 0.0, 1.0); return t * t * (3 - 2 * t); }
static double creator_reset(double zoom) { return creator_smoothstep((zoom - kCreatorResetFrom) / (1 - kCreatorResetFrom)); }
// The shorter way round from a yaw to the default front view (any whole turn of the assembly IS the front): the signed angle to the nearest multiple
// of 2 pi, in (-pi, pi]. The drift adds a share of it each step, so the turn goes home the way the user would turn it, never the long way (k16.2.433.3).
static double creator_yaw_home(double yaw)
{
	const double turn = 2 * M_PI;
	double d = std::fmod(yaw + M_PI, turn);	// [0, 2 pi) after the wrap below
	if (d < 0) { d += turn; }
	return -(d - M_PI);
}
static kworld::SkelQuat creator_quat(float ax, float ay, float az)	// the axis-angle the orbit composes (radians)
{
	const float l = std::sqrt(ax * ax + ay * ay + az * az);
	if (l < 1e-9f) { return {}; }
	const float s = std::sin(l * 0.5f) / l;
	return { std::cos(l * 0.5f), ax * s, ay * s, az * s };
}
static kworld::SkelQuat creator_conj(const kworld::SkelQuat& q) { return { q.w, -q.x, -q.y, -q.z }; }
// The assembly's rotation from its home pose: the drag's tilt about +X (its top towards the camera at +tilt) after the yaw about +Y.
static kworld::SkelQuat creator_assembly(float yaw, float tilt) { return kworld::multiply(creator_quat(-tilt, 0, 0), creator_quat(0, yaw, 0)); }
void open_creator(const std::string& what)
{
	const size_t slash = what.find('/');
	double* scene = app.creatorScene;
	scene[0] = app.opt.x;
	scene[2] = app.opt.z;
	scene[1] = app.hm->height(scene[0], scene[2]) - kCreatorDrop + kCreatorPivot;
	const double at[3] = { scene[0], scene[1] - kCreatorRoot, scene[2] };
	std::string err;
	if (!app.creator.open(app.chars, at, (float)-M_PI, what.substr(0, slash), slash != std::string::npos && what.substr(slash + 1) == "female", err))
	{
		fprintf(stderr, "kclient: --creator %s: %s\n", what.c_str(), err.c_str());
		return;
	}
	app.creatorOrbit = App::CreatorOrbit();	// a fresh framing (creator_frame makes it the page's)
	app.creatorDragging = false;
	printf("creator: %s, %zu sliders\n", what.c_str(), app.creator.ranges().size());
	app.creatorFrames = 0;
	printf("creator: weather before: %s rain %.2f wetness %.2f effects %zu\n", app.sky.weather.name.c_str(), app.sky.weather.rain, app.sky.weather.wetness, app.sky.weather.effects.size());
	if (app.creatorUi) { app.creatorUi->show(true); }
}

// A creator slider by its shown name: the editor ranges, plus Skin Tone (0..Creator::skinMax(), tenths of a palette step) and Idle stance (1..N over
// Characters::idleStances). apply() after.
void creator_slider(const std::string& name, float v)
{
	const auto& stances = app.chars.idleStances();
	if (name == "Skin Tone")
	{
		const float top = (float)app.creator.skinMax();
		app.creator.spec().skin = top > 0 ? std::clamp(v / top, 0.0f, 1.0f) : 0.0f;
	}
	else if (name == "Idle stance" && !stances.empty()) { app.creator.spec().stance = stances[std::clamp((int)std::lround(v), 1, (int)stances.size()) - 1]; }
	else { app.creator.setValue(name, v); }
}

// The creator screen's edits: rebuild the preview, a failure goes to the toast
static void creator_apply()
{
	std::string err;
	if (!app.creator.apply(err)) { fprintf(stderr, "kclient: creator: %s\n", err.c_str()); ui::toast(err); }
}

static void creator_race(const std::string& race, bool female)
{
	std::string err;
	if (!app.creator.setRace(race, female, err)) { ui::toast(err); return; }
	creator_apply();
}

// The new game's squad editor (kinspect findings/charsel23_static.md, docs/UI.md "creator"): BEGIN on a start with members opens the creator on them,
// CHANGE CHARACTER steps through them keeping each one's look, CONFIRM asks when some member was never shown, then new_world and NEW_GAME with the Specs.
// The wire Spec of the creator's current character (strids of the server's table; SIMLINK NEW_GAME character creation)
static simlink::OrderWriter::NewGameSpec ng_wire()
{
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	const Characters::Spec& s = app.creator.spec();
	const auto sid = [&](const char* type, const std::string& name) { const auto it = app.creatorSids.find(std::string(type) + ":" + name); return name.empty() || it == app.creatorSids.end() ? 0u : d.find(it->second); };
	simlink::OrderWriter::NewGameSpec w;
	w.race = sid("RACE", s.race);
	w.female = s.female;
	const auto heads = app.raceHeads.find(s.race);
	if (heads != app.raceHeads.end() && s.head >= 0 && s.head < (int)heads->second[s.female].size()) { w.head = d.find(heads->second[s.female][s.head]); }
	w.hair = sid("ATTACHMENT", s.hair);
	w.beard = s.female ? 0 : sid("ATTACHMENT", s.beard);
	w.hairColour = sid("COLOR_DATA", s.hairColour);
	w.skin = std::clamp(s.skin, 0.0f, 1.0f);
	for (const Characters::EditorRange& r : app.creator.ranges()) { w.sliders.push_back({ r.name, app.creator.value(r.name) }); }
	w.name = app.creator.name;
	const auto& stances = app.chars.idleStances();
	const auto at = std::find(stances.begin(), stances.end(), s.stance);
	w.stance = at == stances.end() ? 1 : 1 + (int32_t)(at - stances.begin());
	return w;
}

static void ng_save()
{
	if (app.ngMembers.empty()) { return; }
	App::NewGameMember& m = app.ngMembers[app.ngIndex];
	m.name = m.wire.name = app.creator.name;
	if (!m.editable) { return; }
	m.look = app.creator.spec();
	m.looked = true;
	m.wire = ng_wire();
}

// Shows member i in the creator: its kept look, else (first time) a random one of its race and sex; a non-editable member (an animal) previews
// its own body: its race's default look and its own idle (the wire Spec is the sim's; only its name is the editor's)
void ng_load(int i)
{
	app.ngIndex = i;
	App::NewGameMember& m = app.ngMembers[i];
	m.visited = true;
	app.creator.name = m.name;
	std::string err;
	const auto race = app.raceNames.find(app.sim.decoder().str(m.wire.race));
	const Characters::Spec& want = m.looked ? m.look : Characters::Spec{};
	const std::string raceName = m.looked ? want.race : race != app.raceNames.end() ? race->second : app.creator.spec().race;
	if (!m.editable)	// an animal: no editor choices; the preview is its race's default look, standing on its own idle
	{
		if (!app.creator.defaultLook(raceName, m.wire.female, err) || !app.creator.apply(err)) { ui::toast(err); }
		return;
	}
	if (!app.creator.setRace(raceName, m.looked ? want.female : m.wire.female, err)) { ui::toast(err); return; }
	if (m.looked) { app.creator.spec() = m.look; }
	else
	{
		app.creator.randomize(std::random_device()());
		// the start's own outfit (k16.17.6.1): CLOTHES on shows it, off the underwear
		app.creator.spec().wear = m.wear;
		app.creator.spec().weapons = m.weapons;
	}
	if (!app.creator.apply(err)) { ui::toast(err); }
}

static void ng_confirm()
{
	ng_save();
	for (size_t i = 0; i < app.ngMembers.size(); i++)	// never shown ("Start anyway"): the random look it would have got, so the Spec carries a head
	{
		if (app.ngMembers[i].editable && !app.ngMembers[i].looked) { ng_load((int)i); ng_save(); }
	}
	app.newGameSpecs.clear();
	for (const App::NewGameMember& m : app.ngMembers) { app.newGameSpecs.push_back(m.wire); }
	const std::string id = app.ngStart;
	app.ngMembers.clear();
	app.ngStart.clear();
	app.ngForce.clear();
	app.creatorUi->show(false);
	app.creator.close();
	app.creatorWeatherAfter = true;	// the world's weather is logged again on the first frame back (k16.17.6.1)
	ng_begin(id);
}

static void end_session_load();

// The camera has a target (kenshi-k16.2.112.1): the world may stream and be drawn from here. `reason` names the target for the first_world_frame line.
void set_camera_target(const char* reason)
{
	app.worldTarget = true;
	app.targetReason = reason;
	app.loadPending = false;
}

// The session's first camera placement (kenshi-k16.2.112.1, from the session block below): once the sim has a focus (the squad) or the session is a
// spectator's, the floating camera starts over it and the session loading screen begins. Returns true when it placed the camera this frame.
bool place_session_camera()
{
	if (app.simCamera) { return false; }
	// A local session's pre-game world is nobody's target (kenshi-k16.2.112.1): under the title the server already holds a squad, and a CONTINUE /
	// LOAD would have the world drawn there before the save's own reset arrives. The world chosen (a new game, a scenario started, a load) places it.
	if (app.local && !app.worldChosen) { return false; }
	if (app.camRestore.applied)	// k16.2.134.1: the load's own camera is the session's placement (its sidecar was just restored)
	{
		app.camRestore.applied = false;	// used once: a later reset's placement is the default one again
		app.simCamera = true;
		app.centerOnStart = false;
		begin_session_load();
		if (app.follow) { update_follow(0.0); }
		else if (app.rts) { update_rts(0.0); }
		set_camera_target("load");
		if (app.scripted()) { printf("session: camera placed at %.0f,%.0f (%s)\n", app.pos[0], app.pos[2], app.targetReason); }
		return true;
	}
	double f[3];
	const bool haveFocus = app.sim.focus(f);
	if (!haveFocus && !app.spectating) { return false; }
	// once: the floating camera over the focus character, looking along -z (MINE pitch). A spectator has no focus character:
	// the floating camera starts where --pos (and --alt / --yaw / --pitch) put it, and its interest is what streams the world.
	set_follow(false);
	if (haveFocus)
	{
		app.rts = true;
		std::copy(f, f + 3, app.focus);
		app.yaw = 0; app.pitch = -30 * M_PI / 180.0;	// the original's default (kinspect camera_rts.md)
	}
	else
	{
		// a spectator: the RTS camera orbits the --pos point on the ground (demo.sh passes a fixture's source centre=), at the original's -30
		// degree pitch from 250 u, so the fight there is in frame; looking out from above it showed only the horizon (kenshi-k16.2.111)
		app.yaw = 0; app.pitch = -30 * M_PI / 180.0;
		set_rts(0);
		app.focus[0] = app.pos[0];
		app.focus[2] = app.pos[2];
		app.focus[1] = ground_under(app.focus[0], app.focus[2], &app.focusOn);
		app.rtsDist = 250;
	}
	if (app.opt.camera)
	{
		std::copy(app.opt.cameraPos, app.opt.cameraPos + 3, app.focus);
		app.yaw = app.opt.cameraYaw * M_PI / 180.0;
		app.pitch = app.opt.cameraPitch * M_PI / 180.0;
		app.rtsDist = std::clamp(app.opt.cameraDist, 10.0, 2000.0);
	}
	// An imported save's camera (CAMERA_START, kenshi-cym.40.2.3): the frame wins over --camera-pos. pos is the orbit pivot, rot its world orientation with
	// the original's camera looking +Z, so the view's forward is R*(0,0,1) (our -Z camera: R*rotY(180)); zoom is minus the eye's distance. Bit 1 sets the
	// floor level without engaging a follow: no auto-floor snap until a C / double click (kenshi-rwh.150).
	bool cameraStarted = false;
	{
		const simlink::SimLinkDecoder& dec = app.sim.decoder();
		if (dec.cameraStartSerial != app.cameraStartUsed)
		{
			app.cameraStartUsed = dec.cameraStartSerial;
			const simlink::SimLinkDecoder::CameraStart& cs = dec.cameraStart;
			if (cs.flags & 1)
			{
				const double w = cs.rot[0], x = cs.rot[1], y = cs.rot[2], z = cs.rot[3];
				const double fx = 2 * (x * z + w * y), fy = 2 * (y * z - w * x), fz = 1 - 2 * (x * x + y * y);
				app.rts = true;
				app.centerId = 0;
				std::copy(cs.pos, cs.pos + 3, app.focus);
				app.yaw = std::atan2(fx, -fz);
				app.pitch = std::asin(std::clamp(fy, -1.0, 1.0));
				app.rtsDist = std::clamp((double)std::fabs(cs.zoom), 10.0, 2000.0);
				cameraStarted = true;
			}
			const int floor = cs.flags & 2 ? cs.floor : 0;
			app.buildings.restoreView(floor, 0);
			printf("session: CAMERA_START flags %u pos %.3f %.3f %.3f rot %.5f %.5f %.5f %.5f zoom %.2f floor %d -> yaw %.2f pitch %.2f dist %.1f\n", cs.flags, cs.pos[0], cs.pos[1], cs.pos[2], cs.rot[0], cs.rot[1], cs.rot[2], cs.rot[3], cs.zoom, floor, app.yaw * 180 / M_PI, app.pitch * 180 / M_PI, app.rtsDist);
		}
	}
	app.simCamera = true;
	app.centerOnStart = haveFocus && !app.opt.camera && !cameraStarted;	// an explicit --camera-* framing stays put
	begin_session_load();
	// the camera at the target now: the first world frame is drawn from it, not from the old position one frame later (k16.2.112.1)
	update_rts(0.0);
	set_camera_target(app.loadPending ? "load" : haveFocus ? "squad" : "spectator");
	if (app.scripted()) { printf("session: camera placed at %.0f,%.0f (%s)\n", app.pos[0], app.pos[2], app.targetReason); }
	return true;
}

// A --simlink run's --screenshot with no --script (kenshi-k16.2.124.1): frame 3 is black (nothing is drawn before the camera has a target), so the shot
// is pushed here instead, at the first world frame — three frames on, or after the 10 s Hub fallback. Offline, viewer and scripted runs keep frame 3.
void screenshot_world_frame()
{
	if (!app.screenshotOnWorld) { return; }
	app.screenshotOnWorld = false;
	app.cmds.push_back({ app.frames + kScreenshotFrames, "screenshot", { app.opt.screenshot }, false });
	app.cmds.push_back({ app.frames + kScreenshotFrames, "quit", {}, false });
}

// The frame packet: renderframe.hpp (render hook 6; the draw half's own translation unit since kenshi-k16.2.205.12.27).
FramePacket gPackets[2];

// No camera target yet (kenshi-k16.2.112.1): the sim is pumped and the start placement runs, but nothing is streamed or drawn from the camera — black,
// or the session loading screen (never a world frame: the user's "loading into deadlands should be considered a spoiler").
void frame_await_target(FramePacket& pk, double dt)
{
	app.framePhase = "waiting for the world";
	if (app.sim.active())
	{
		app.sim.hold(app.creator.active());
		app.sim.update(dt, app.chars, [](double x, double z) { return app.hm->height(x, z); });
		app.nav.update(app.sim.decoder(), app.sim.takeNavPatches());
		app.sim.setWalker(app.nav.walker());
		app.sim.takeSplashes();	// nothing draws them yet: a splice on the ground behind the black frame
		if (app.simCamera) { app.sim.view(app.pos[0], app.pos[2]); }
		place_session_camera();
	}
	// The character creator is its own camera target (kenshi-k16.2.133.1): its scene sits far below the world, so nothing of the world shows. Until that
	// scene is drawn the full-screen loading screen stands in (the 10 s Hub fallback used to leave the creator's panels over black meanwhile).
	if (!app.worldTarget && app.creatorUi->shown())
	{
		begin_session_load();
		set_camera_target("creator");
	}
	update_ui(dt);
	pk.path = FramePath::kAwait;	// render_frame draws it (renderframe.cpp frame_blank)
	if (!app.worldTarget)
	{
		app.targetWait += dt;
		if (app.targetWait >= kCameraWaitSeconds)
		{
			// the Hub default: where the camera already is with no explicit position (k16.2.112.1)
			set_camera_target("timeout");
			fprintf(stderr, "kclient: no camera target after %.0f s; Hub default\n", app.targetWait);
		}
	}
}
// BEGIN's wipe: new_world, then NEW_GAME (with app.newGameSpecs) once its reset is in (update_ui)
void ng_begin(const std::string& id)
{
	if (!app.sim.newWorld()) { return; }
	// A playtest run ends with a NEW_GAME (END: its metrics still answers, and its file is rewritten); a recipe's run does not: the new game
	// is the flow it records (k16.2.97), and its file is written when it is left.
	dev::newGameLeave();
	audiohook_event("Create_or_Load_Game");	// at the request, before the world is rebuilt (audio_load_pause.md 1b: load and new game share it)
	app.newGameStart = id;
	app.newGameSent = false;
	app.gameMenu->show(false);
	if (title_shown()) { app.mainMenu->show(false); }
	menu_pause(false);
	app.menuPaused = false;
}

ui::CreatorActions creator_actions()
{
	ui::CreatorActions act;
	act.setName = [](const std::string& name) { app.creator.name = name; };
	act.setGroup = [](int i)
	{
		const auto groups = app.chars.raceGroups();
		if (i >= 0 && i < (int)groups.size() && !groups[i].second.empty()) { creator_race(groups[i].second[0], app.creator.spec().female); }
	};
	act.setRace = [](int i)
	{
		for (const auto& [group, races] : app.chars.raceGroups())
		{
			if (std::find(races.begin(), races.end(), app.creator.spec().race) != races.end() && i >= 0 && i < (int)races.size()) { creator_race(races[i], app.creator.spec().female); return; }
		}
	};
	act.setSex = [](int sex) { creator_race(app.creator.spec().race, sex == 1); };
	act.setCharacter = [](int i) { if (i >= 0 && i < (int)app.ngMembers.size()) { ng_save(); ng_load(i); } };
	act.setHead = [](int i) { app.creator.spec().head = i; creator_apply(); };
	act.setHair = [](int i) { const auto& h = app.creator.choices().hairs; app.creator.spec().hair = i > 0 && i <= (int)h.size() ? h[i - 1] : ""; creator_apply(); };
	act.setBeard = [](int i) { const auto& b = app.creator.choices().beards; app.creator.spec().beard = i > 0 && i <= (int)b.size() ? b[i - 1] : ""; creator_apply(); };
	act.setHairColour = [](int i)
	{
		const auto& c = app.creator.choices().hairColours;
		if (i >= 0 && i < (int)c.size()) { app.creator.spec().hairColour = c[i]; creator_apply(); }
	};
	act.setSlider = [](const std::string& name, float v) { creator_slider(name, v); creator_apply(); };
	act.setClothes = [](bool on) { app.creator.clothes = on; creator_apply(); };
	// ui::CreatorPage and Creator::Part list the tabs in the same order
	act.randomize = [](int page) { app.creator.randomize(std::random_device()(), page); creator_apply(); };
	act.randomizeAll = []() { app.creator.randomize(std::random_device()()); creator_apply(); };
	act.resetDefaults = []() { std::string err; if (!app.creator.reset(err)) { ui::toast(err); return; } creator_apply(); };
	act.importCharacter = []() { ui::toast("import: not yet"); };
	act.exportCharacter = []() { printf("creator: export --character '%s'\n", app.creator.specString().c_str()); ui::toast("exported to the log"); };
	act.done = []()
	{
		if (!app.ngMembers.empty())
		{
			ng_save();
			if (std::all_of(app.ngMembers.begin(), app.ngMembers.end(), [](const App::NewGameMember& m) { return m.visited; })) { ng_confirm(); return; }
			// the original's warning for members never stepped to (charsel23_static.md; our wording)
			app.prompt->open({ "NEW GAME", "Some of your characters have not been looked at yet. Start anyway?", { "Yes", "No" } }, [](int b) { if (b == 0) { ng_confirm(); } });
			return;
		}
		printf("creator: done %s --character '%s'\n", app.creator.name.c_str(), app.creator.specString().c_str());
		app.creatorUi->show(false);
		app.creatorWeatherAfter = true;
	};
	act.cancel = []() { app.creatorUi->show(false); app.creatorWeatherAfter = true; };
	return act;
}

static int creator_page() { return app.creatorUi ? app.creatorUi->page() : ui::kCreatorBody; }
static double creator_pitch(int page) { return page == ui::kCreatorBody ? kCreatorBodyPitch : 0; }
static double creator_dist(int page) { return page == ui::kCreatorBody ? kCreatorBodyDist : kCreatorFaceDist; }
static double creator_min_dist(int page) { return page == ui::kCreatorBody ? kCreatorMinDistBody : kCreatorMinDistFace; }
// The zoom's eye distance at a zoom level: geometric from the page's closest to its default (1 = full out)
static double creator_zoom_dist(int page, double zoom) { const double dmin = creator_min_dist(page), dmax = creator_dist(page); return dmin * std::pow(dmax / dmin, std::clamp(zoom, 0.0, 1.0)); }
// The page's default pivot: the scene origin (BODY) or Bip01 Head as it is drawn (FACE / HAIR: the assembly turned, the pivot goes with it)
static void creator_default_pivot(int page, double out[3])
{
	const double* scene = app.creatorScene;
	out[0] = scene[0];
	out[2] = scene[2];
	if (page == ui::kCreatorBody) { out[1] = scene[1]; return; }
	app.creatorHead = app.chars.headHeight(app.creator.index());
	if (!app.chars.boneWorld(app.creator.index(), "Bip01 Head", out)) { out[1] = scene[1] - kCreatorRoot + app.creatorHead; }
}
// The page's default eye: kCreatorBodyDist / kCreatorFaceDist behind the pivot at its pitch, slid sideways so the character stands at the preview
// rect's centre (the original's fixed x offsets only shifted it left of the window centre, creator_frame's comment)
static void creator_default_eye(int page, const double pivot[3], const ui::Rect& r, double out[3])
{
	const double dist = creator_dist(page), pitch = creator_pitch(page);
	const double ndc = (r.x + r.w * 0.5) / view::widthf() * 2 - 1;
	const double side = -ndc * dist * std::tan(kFovY * 0.5) * view::widthf() / view::heightf();
	out[0] = pivot[0] - side;	// the camera's right is -x
	out[2] = pivot[2] - dist * std::cos(pitch);
	out[1] = pivot[1] + dist * std::sin(pitch);
}
// Turn the whole assembly by the world rotation `next` (from its current one) about the world point P: the scene pivot's move follows so that P
// stays where it is, which is what makes a drag turn about the rotation point the user zoomed onto.
static void creator_about(App::CreatorOrbit& o, const kworld::SkelQuat& next, const double P[3])
{
	const double* c = app.creatorScene;
	const kworld::SkelQuat d = kworld::multiply(next, creator_conj(o.q));
	const kworld::SkelVec3 v = kworld::rotate(d, { (float)(c[0] + o.shift[0] - P[0]), (float)(c[1] + o.shift[1] - P[1]), (float)(c[2] + o.shift[2] - P[2]) });
	o.shift[0] = P[0] - c[0] + v.x;
	o.shift[1] = P[1] - c[1] + v.y;
	o.shift[2] = P[2] - c[2] + v.z;
	o.q = next;
}

// A drag of the preview area (creator_event, host/events.cpp): pixels right and down since the last call. It turns character AND platform about the
// pivot (the assembly, k16.2.433.1): x yaws it about +Y, y tilts it about +X (the top towards the camera), clamped short of flipping over.
void creator_drag(float dx, float dy)
{
	creator_turn((float)(dx * kCreatorDragRate), (float)(dy * kCreatorDragRate));
	// the drag shows at once, no glide behind the pointer (k16.2.457): the turn left the target one step ahead of the shown view, so land it
	App::CreatorOrbit& o = app.creatorOrbit;
	if (o.gliding)
	{
		o.sYaw = o.yaw;
		o.sTilt = o.tilt;
		creator_copy(o.sEye, o.eye);
		creator_copy(o.sShift, o.shift);
		o.sZoom = o.zoom;
		o.gliding = false;
	}
}

// The target becomes the shown view: a drag acts on what is on screen and ends any glide (it must never fight the drag, k16.2.433.4)
static void creator_snap_shown(App::CreatorOrbit& o)
{
	if (!o.gliding) { return; }
	creator_copy(o.eye, o.sEye);
	creator_copy(o.shift, o.sShift);
	o.yaw = o.sYaw;
	o.tilt = o.sTilt;
	o.zoom = o.sZoom;
	o.q = creator_assembly(o.yaw, o.tilt);
	o.gliding = false;
	o.track = false;
}

// the same in radians (the script's `creator turn | tilt`)
void creator_turn(float yaw, float tilt)
{
	if (!app.creator.active()) { return; }
	App::CreatorOrbit& o = app.creatorOrbit;
	creator_snap_shown(o);
	o.yaw += yaw;
	o.tilt = (float)std::clamp((double)o.tilt + tilt, -kCreatorTiltMax, kCreatorTiltMax);
	creator_about(o, creator_assembly(o.yaw, o.tilt), o.pivot);
	o.gliding = true;	// the shown view glides to the turned target like any change (k16.2.457); creator_frame settles it
}

// One wheel gesture in the preview area: `notches` of the wheel at the pointer (mx, my). The eye moves along the pointer's ray to the point of the
// model under it (Characters::skeletonRay: the point stays under the cursor) and that point becomes the pivot, so the next drag turns about it; off
// the model the zoom is towards the pivot the eye is on. Over the last 30% of the way out the pivot, the tilt, the yaw and the eye drift back to the
// page's default framing, never snapping (the lead's 2026-11-20 14:44 comment on kenshi-k16.2.433): the reset position is the default view, and that
// includes the turn, so the yaw eases home too, the shorter way round (user 2026-11-20 ~15:00, kenshi-k16.2.433.3). Zooming in drifts nothing, so a
// drag turns freely again; the drift is one share per notch, so it never fights a drag held through a wheel step.
void creator_zoom(float notches, double mx, double my)
{
	if (!app.creator.active() || notches == 0 || !app.creatorUi) { return; }
	const ui::Rect r = app.creatorUi->previewRect();
	if (r.w <= 0) { return; }
	App::CreatorOrbit& o = app.creatorOrbit;
	const int page = creator_page();
	const double z0 = o.zoom;
	const double z1 = std::clamp(o.zoom * std::pow(kCreatorZoomsPerNotch, -notches), 0.0, 1.0);
	if (z1 == o.zoom) { return; }
	// The pick is on the SHOWN model (what the user points at, mid-glide too); its place in the TARGET assembly is where the step works. The camera's
	// orientation never moves, so the cursor's ray direction is the same in both views.
	double dir[3];
	mouse_ray(mx, my, dir);
	double T[3];
	const bool model = app.chars.skeletonRay(app.creator.index(), app.pos, dir, T, kCreatorPickRadius);
	if (model && o.gliding)
	{
		const double* c = app.creatorScene;
		const kworld::SkelVec3 l = kworld::rotate(creator_conj(creator_assembly(o.sYaw, o.sTilt)), { (float)(T[0] - c[0] - o.sShift[0]), (float)(T[1] - c[1] - o.sShift[1]), (float)(T[2] - c[2] - o.sShift[2]) });
		const kworld::SkelVec3 w = kworld::rotate(o.q, l);
		T[0] = c[0] + o.shift[0] + w.x;
		T[1] = c[1] + o.shift[1] + w.y;
		T[2] = c[2] + o.shift[2] + w.z;
	}
	// the drift out: the share of the distance still to go that this step covers (0 while zooming in)
	const double u0 = creator_reset(o.zoom), u1 = creator_reset(z1), f = u1 > u0 ? (u1 - u0) / (1 - u0) : 0;
	double pivotD[3], eyeD[3];
	creator_default_pivot(page, pivotD);
	creator_default_eye(page, pivotD, r, eyeD);
	if (f > 0)
	{
		for (int k = 0; k < 3; k++) { o.pivot[k] += (pivotD[k] - o.pivot[k]) * f; }
		o.tilt = (float)(o.tilt * (1 - f));
		o.yaw = (float)(o.yaw + creator_yaw_home(o.yaw) * f);	// home the way the user turned it: the shorter arc (k16.2.433.3)
		creator_about(o, creator_assembly(o.yaw, o.tilt), o.pivot);
		// The assembly's offset home too: turns about a point off the scene origin (the wheel's pivot) leave one behind, and the reset position is
		// the home pose, not only the home orientation (k16.2.433.3: without this the full-out frame was the default camera on a shifted character).
		for (int k = 0; k < 3; k++) { o.shift[k] *= (1 - f); }
	}
	const double d = creator_zoom_dist(page, z1);
	if (model) { for (int k = 0; k < 3; k++) { o.eye[k] = T[k] - dir[k] * d; } }
	else
	{
		creator_copy(T, o.pivot);	// off the model: towards the pivot, along the axis the eye is on
		double axis[3] = { T[0] - o.eye[0], T[1] - o.eye[1], T[2] - o.eye[2] };
		const double l = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
		if (l < 1e-6) { return; }
		for (int k = 0; k < 3; k++) { o.eye[k] = T[k] - axis[k] / l * d; }
	}
	creator_copy(o.pivot, T);
	if (f > 0) { for (int k = 0; k < 3; k++) { o.eye[k] += (eyeD[k] - o.eye[k]) * f; } }
	if (z1 >= 1) { creator_copy(o.pivot, pivotD); }	// fully out: the rotation point is the page's default, not whatever the cursor was over (the reset asked for)
	o.zoom = z1;
	o.gliding = true;	// the shown view follows creator_frame's glide; the settled check below is logged when it arrives
	creator_copy(o.tCursor, T);
	o.curX = (float)mx;
	o.curY = (float)my;
	o.track = true;
	printf("creator: zoom %.2f -> %.2f (eye %.2f from %s): cursor %.0f %.0f, target %.2f %.2f %.2f (eye %.2f %.2f %.2f, pivot %.2f %.2f %.2f, tilt %.1f deg, yaw %.1f deg)\n",
		z0, z1, d, model ? "the model" : "the pivot", mx, my, T[0], T[1], T[2],
		o.eye[0], o.eye[1], o.eye[2], o.pivot[0], o.pivot[1], o.pivot[2], o.tilt * 180.0 / M_PI, o.yaw * 180.0 / M_PI);
}

// The pointer onto a bone of the preview (the scripted shots' way to aim the wheel at a hand); false = no such bone drawn.
bool creator_point_at(const char* what)
{
	double p[3];
	if (!app.creator.active() || !app.chars.boneWorld(app.creator.index(), what, p)) { return false; }
	float x = 0, y = 0;
	if (!world_to_screen(p, x, y)) { return false; }
	app.mouseX = x;
	app.mouseY = y;
	pointer::set(x, y);
	printf("creator: pointer on %s at %.0f %.0f (world %.2f %.2f %.2f)\n", what, x, y, p[0], p[1], p[2]);
	return true;
}

// The preview between the creator's columns: the editor's assembly (character and platform, one node: the original's charedit_chars_rotation) turned
// by the orbit, and the camera looking along +z from the eye the orbit holds. The orientation never moves with the zoom, which is what keeps the
// point the wheel tracked exactly under the pointer (moving the eye along its ray cannot move what the ray hits).
void creator_frame(double dt)
{
	const ui::Rect r = app.creatorUi->previewRect();
	if (r.w <= 0) { return; }
	const int page = creator_page();
	App::CreatorOrbit& o = app.creatorOrbit;
	if (o.page != page)	// a tab: its own framing starts (the assembly keeps its yaw)
	{
		double pivot[3], eye[3];
		creator_default_pivot(page, pivot);
		creator_default_eye(page, pivot, r, eye);
		o.page = page;
		o.zoom = 1;
		o.tilt = 0;
		creator_copy(o.pivot, pivot);
		creator_copy(o.eye, eye);
		creator_about(o, creator_assembly(o.yaw, 0), o.pivot);
		creator_copy(o.sEye, o.eye);	// the page's own framing starts shown, not glided
		creator_copy(o.sShift, o.shift);
		o.sYaw = o.yaw;
		o.sTilt = o.tilt;
		o.sZoom = o.zoom;
		o.gliding = false;
	}
	// The glide (k16.2.433.4): the shown view eases to the target the wheel set, exponentially (tau kCreatorGlideTau: settled in about 0.25 s), by dt, so
	// it is frame-rate independent. Eye and shift are straight lines, yaw and tilt angles, the shown rotation the assembly of the two: a pure zoom
	// step moves only the eye, along the cursor's ray, so its point stays under the cursor all the way.
	if (o.gliding)
	{
		const double a = dt > 0 ? 1 - std::exp(-dt / kCreatorGlideTau) : 0;
		double gap = 0;
		for (int k = 0; k < 3; k++)
		{
			o.sEye[k] += (o.eye[k] - o.sEye[k]) * a;
			o.sShift[k] += (o.shift[k] - o.sShift[k]) * a;
			gap = std::max({ gap, std::fabs(o.eye[k] - o.sEye[k]), std::fabs(o.shift[k] - o.sShift[k]) });
		}
		o.sYaw += (float)((o.yaw - o.sYaw) * a);
		o.sTilt += (float)((o.tilt - o.sTilt) * a);
		o.sZoom += (o.zoom - o.sZoom) * a;
		gap = std::max({ gap, (double)std::fabs(o.yaw - o.sYaw) * 50, (double)std::fabs(o.tilt - o.sTilt) * 50 });
		if (gap < 0.002)
		{
			creator_copy(o.sEye, o.eye);
			creator_copy(o.sShift, o.shift);
			o.sYaw = o.yaw;
			o.sTilt = o.tilt;
			o.sZoom = o.zoom;
			o.gliding = false;
		}
	}
	const kworld::SkelQuat shownQ = creator_assembly(o.sYaw, o.sTilt);
	const double* scene = app.creatorScene;
	app.chars.setPreviewOrbit(app.creator.index(), scene, shownQ, o.sShift);
	const kworld::SkelVec3 plat = kworld::rotate(shownQ, { 0.0f, (float)-kCreatorPivot, 0.0f });
	const double platform[3] = { scene[0] + o.sShift[0] + plat.x, scene[1] + o.sShift[1] + plat.y, scene[2] + o.sShift[2] + plat.z };
	const float q4[4] = { shownQ.w, shownQ.x, shownQ.y, shownQ.z };
	app.buildings.setEditor(true, platform, q4);
	creator_copy(app.pos, o.sEye);
	if (o.track && !o.gliding)	// the glide arrived: the step's point against the pointer it was taken at (the zoom is about the cursor)
	{
		float sx = 0, sy = 0;
		const bool on = world_to_screen(o.tCursor, sx, sy);
		printf("creator: glide settled: cursor %.0f %.0f, tracked point now at %.0f %.0f\n", o.curX, o.curY, on ? sx : -1.0f, on ? sy : -1.0f);
		o.track = false;
	}
	app.yaw = M_PI;	// looking along +z
	app.pitch = -creator_pitch(page);
	// The measurement (k16.17.6.1, --debug): the platform's plank top against the preview's foot bones, once the pose has settled. The platform part's
	// mesh has its plank top at the instance origin (character_editor_platform.mesh, the up-facing triangles at y 0); the original's character root is
	// 0.7 above it (findings/charedit.md: character (0,-3011.3,0), platform (0,-3012,0)), which is kCreatorPivot - kCreatorRoot here. Taken at the
	// default framing (the creator just opened: the orbit is at rest and the platform level).
	if (app.opt.debug && app.creatorFrames < 31 && ++app.creatorFrames == 30)
	{
		double node[3] = {}, lf[3] = {}, rf[3] = {};
		app.chars.nodeWorld(app.creator.index(), node);
		const bool l = app.chars.boneWorld(app.creator.index(), "Bip01 L Foot", lf), r = app.chars.boneWorld(app.creator.index(), "Bip01 R Foot", rf);
		printf("creator: platform top %.3f, preview node %.3f; L foot %s%+.3f, R foot %s%+.3f over the planks\n", platform[1], node[1],
			l ? "" : "none ", l ? lf[1] - platform[1] : 0.0, r ? "" : "none ", r ? rf[1] - platform[1] : 0.0);
	}
}


// Loading screen (loadscreen.hpp). A step blocks its frame, so the name shown is the step in progress: it presents before the step runs.
void frame_loading(FramePacket& pk)
{
	if (app.loadShown)
	{
		dev::SgAllowScope allow("load step: before the logic thread starts, its one thread owns sokol (RENDER_THREAD.md §7.1 rule 7)");
		run_load_step();
	}
	app.loadShown = true;
#ifdef KCLIENT_DEV_TOOLS
	// The mid-load resize check (k16.2.221): KCLIENT_LOADING_RESIZE="STEPS,W,H" resizes the whole window once the startup load has run STEPS
	// steps, as a maximize does mid-load. The startup path takes no --script (its steps run before the frame loop), so this drives it; the load
	// screen re-bakes its text at the new height (relayout). Dev builds only.
	if (const char* r = getenv("KCLIENT_LOADING_RESIZE"))
	{
		int step = -1, w = 0, h = 0;
		if (sscanf(r, "%d,%d,%d", &step, &w, &h) != 3 || (int)app.loadIdx >= step)
		{
			if (step > 0) { windowgeom::resizeWindow(w, h); }
			plat::unsetEnv("KCLIENT_LOADING_RESIZE");	// once, not every frame
		}
	}
#endif
	pk.path = FramePath::kLoading;
}

// The --loading-shot (logic_tail on the loading path): a request for this frame's loading screen, then _exit.
void loading_shot()
{
	if (!app.opt.loadShot.empty() && (int)app.loadIdx >= app.opt.loadShotStep) { readback::screenshot(app.opt.loadShot, [](bool) { fflush(stdout); fflush(stderr); exit_now(0); }); }	// no cleanup: the half-started subsystems would hang their destructors
}

const char* compass(double deg)
{
	static const char* names[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	return names[(int)std::floor(deg / 45.0 + 0.5) % 8];
}

HudBox hud_box(bool gpuLine)
{
	const float lh = 9.0f * text_scale();
	if (!kHudText.get()) { return { 4, 4, 0, 0 }; }	// the block is off: no panel, and the help panel starts at the corner
	return { 4, 4, 112.0f * 8 * text_scale(), lh * ((app.foliageOn ? 7 : 6) + (gpuLine ? 1 : 0) + (app.buildings.floorStatus().valid ? 1 : 0) + (app.sim.active() ? 1 : 0)) + (app.opt.debug && !app.squads.dots().empty() ? lh : 0) + 8 };
}

// The dev hooks' host side (dev/debughost.hpp Context): the scene JSON's hud box and the camera out-parameter of `probe depth`.
static void dev_hud_box(float out[4])
{
	const HudBox b = hud_box(framestats::stats().gpuOk);
	out[0] = b.x; out[1] = b.y; out[2] = b.w; out[3] = b.h;
}

static void dev_current_camera(float nearZ, float farZ, Camera& out) { out = current_camera(nearZ, farZ); }

void draw_help()
{
	// the mouse, then every bound action from ui::keys (kclient.cfg `key.*`, the Options' Keys tabs), wrapped
	std::vector<std::string> lines = {
		"mouse: left click selects a player character (Shift adds), right click attacks the character there, else moves (Shift queues);",
		"right button held: mouse look; wheel: fly speed (zoom; in direct drive it leaves the drive); world map: click teleports. Esc quits.",
	};
	std::string line;
	for (const ui::keys::Action& a : ui::keys::actions())
	{
		if (a.group == ui::keys::Group::kDebug && !ui::keys::debug()) { continue; }	// inert without --debug
		const std::string k = ui::keys::label(a.name);
		if (k.empty()) { continue; }
		const std::string item = k + " " + a.title + "   ";
		if (line.size() + item.size() > 150) { lines.push_back(line); line.clear(); }
		line += item;
	}
	if (!line.empty()) { lines.push_back(line); }
	const float lh = 9.0f * text_scale(), y = hud_box(framestats::stats().gpuOk).y + hud_box(framestats::stats().gpuOk).h + 4;
	const int n = (int)lines.size();
	draw_panel(4, y, 152.0f * 8 * text_scale(), lh * (n + 1) + 8);
	draw_text(8, y + 4, 1.0f, 1.0f, 0.6f, "Keys");
	for (int i = 0; i < n; i++) { draw_text(8, y + 4 + lh * (i + 1), 1.0f, 1.0f, 1.0f, lines[i].c_str()); }
	const std::vector<Squads::Source>& src = app.squads.sources();
	if (src.empty() || !ui::keys::debug()) { return; }	// the source list goes with the debug overlay
	const float ly = y + lh * (n + 1) + 12;
	draw_panel(4, ly, 34.0f * 8 * text_scale(), lh * ((float)src.size() + 1) + 8);
	draw_text(8, ly + 4, 1.0f, 1.0f, 0.6f, ("Squads (" + ui::keys::label("debug.squad_overlay") + ")").c_str());
	for (size_t i = 0; i < src.size(); i++)
	{
		char buf[64];
		snprintf(buf, sizeof buf, "# %s (%zu)", src[i].name.c_str(), src[i].count);
		draw_text(8, ly + 4 + lh * (float)(i + 1), src[i].r, src[i].g, src[i].b, buf);
	}
}

// The camera subject (kenshi-k16.2.236.2): the character the camera moves with, whichever mode put it there: the follow / direct-drive camera (V, Ctrl+C;
// app.follow) or the RTS auto-centre (F, a double click on the character or its portrait; app.centerId). 0 = none. Every consumer that asks "is the camera on
// a character" (the auto floor, the occluder fade, the F12 tracked line, the status line) calls this; one that needs the mode (the follow camera's own pivot,
// the watch window's following flag) reads app.follow itself.
uint64_t camera_subject()
{
	return app.follow && app.followId ? app.followId : app.rts && app.centerId ? app.centerId : 0;
}

// The world overlay's camera (render sitting 11, kenshi-k16.2.205.12.57): the packet's view 0 at near 1, far 1000, as build_mvp(mvp, 1, 1000) gave the
// old render-side draws. The overlay lists and record_ui's labels are projected with it in logic, so they hold the frame's pixels.
Camera overlay_camera(const FramePacket& pk)
{
	Camera c = pk.views[0];
	c.nearZ = 1.0f;
	c.farZ = 1000.0f;
	return c;
}

// The shot sidecar's hud lines (kenshi-k16.2.335.1): every line of the developer text block under --debug, shown or hidden (debug.hud_text),
// as "hud.KEY: TEXT" lines; "" without --debug
static std::string shot_hud_text()
{
	if (!app.opt.debug) { return ""; }
	std::string out;
	for (const HudLine& l : hud_lines()) { out += "hud." + l.key + ": " + l.text + "\n"; }
	return out;
}

// The top-left developer text (--debug and the debug.hud_text option, default off: PLAYER_GUIDE 6.10). Hidden, the F12 sidecar still has every line.
void draw_hud()
{
	const std::vector<HudLine> lines = hud_lines();	// every --debug frame, hidden or not
	if (kHudText.get())
	{
		const float lh = 9.0f * text_scale(), x = 8, y = 8;
		const HudBox box = hud_box(framestats::stats().gpuOk);
		draw_panel(box.x, box.y, box.w, box.h);
		float yy = y;
		for (const HudLine& l : lines) { draw_text(x, yy, l.r, l.g, l.b, l.text.c_str()); yy += lh; }
	}
	if (app.help) { draw_help(); }
}

// The scene test fixes the animation time (dev/scenetest.cpp): a shot does not depend on how many frames the scene took to load.
double sim_time() { return dev::sceneSimTime(app.animTime); }

// The world streams on worker threads round the camera: buildings, foliage cells, grass tiles (and its first build), a weather change, the building set,
// and the render queue's stream lane (characters' meshes and textures, a byte budget a frame: RENDER_THREAD.md §7.2, kenshi-k16.2.205.12.34).
static bool world_loading(int* pending)
{
	const WorldBuildings::Stats bs = app.buildings.stats();
	const Foliage::Stats fs = app.foliage.stats();
	const Grass::Stats gs = app.grass.stats();
	const size_t streamed = rq::stats().pendingStream;
	if (pending) { *pending = (int)(bs.pending + fs.cellsPending + gs.tilesPending + streamed); }
	// a character still standing in or inside its two-stage switch (kenshi-k16.2.258.1): the scene test waits the fade out, so its shots are of the
	// real look exactly as before the stand-in
	return bs.pending > 0 || fs.cellsPending > 0 || gs.tilesPending > 0 || streamed > 0 || (app.grassOn && !app.grass.ready() && !app.grass.failed()) || !app.wantWeather.empty() || !app.bldSet
		|| app.chars.standinActive();
}

// The sky's cloud density and colour mult under regional weather (sky.hpp SkyRamp): retarget on a new record in the camera's group or a new group, snap on a
// jump into another group, step in game-speed seconds. Overrides the regional blend's two values; the haze fog stays blended (fog is regional in the exe too).
void sky_ramp(double dt)
{
	const WeatherAtlas& a = app.wx.atlas();
	const int g = a.groupIndexAt(app.pos[0], app.pos[2]);
	const double dx = app.pos[0] - app.skyRampXZ[0], dz = app.pos[2] - app.skyRampXZ[1];
	app.skyRampXZ[0] = app.pos[0]; app.skyRampXZ[1] = app.pos[2];
	if (g >= 0)
	{
		const SkyWeather& w = a.groupWeather[(size_t)g];
		const float target[4] = { w.cloudsDensity, w.skyColourMult[0], w.skyColourMult[1], w.skyColourMult[2] };
		const bool groupChanged = g != app.skyRampGroup, recordChanged = !std::equal(target, target + 4, app.skyRampTarget);
		if (!app.skyRamp.valid || groupChanged || recordChanged)
		{
			// scene tests snap: a still frame has no ramp to show
			const bool snap = !app.skyRamp.valid || (groupChanged && dx * dx + dz * dz > SkyRamp::kJump2) || dev::sceneTesting();
			app.skyRamp.retarget(target[0], target + 1, snap ? 0.0 : SkyRamp::kSeconds);
			app.skyRampGroup = g;
			std::copy(target, target + 4, app.skyRampTarget);
		}
	}
	app.skyRamp.update(app.sky.clock.paused ? 0.0 : dt * app.sky.clock.speed);
	if (!app.skyRamp.valid) { return; }
	app.sky.weather.cloudsDensity = app.skyRamp.density;
	for (int k = 0; k < 3; k++) { app.sky.weather.skyColourMult[k] = app.skyRamp.colour[k]; }
}


// The title's offline line (kenshi-k16.3.12, ui::MainMenu::setOffline): empty when a game is on screen; otherwise why the title's buttons cannot
// start one, and they are greyed. A local start in flight says it is starting; a start or connect that failed says why and points at the launcher.
// A --simlink join that has dropped has no server to start (the title starts none for a join).
// The same, with how the panel draws it: a failure is an error (ui::MainMenu::setError); a start in flight is progress with the server's stage (step of steps,
// from platform/localserver, kenshi-k16.2.431.2); the elapsed seconds change the text once a second.
struct TitleStatus { std::string text; int step = 0, steps = 0; bool error = false; };

static TitleStatus title_status_full()
{
	TitleStatus t;
	if (app.sim.active()) { return t; }
#ifdef KCLIENT_DEV_TOOLS
	const char* const where = "; run client/tools/play.sh game";
#else
	// a player's build has no play.sh: the Windows log file's path when there is one (kenshi-379.31.9.3), else just the log
	const std::string logFile = plat::logPath();
	const std::string where = logFile.empty() ? std::string("; see the game log for details") : "; the game log is " + logFile;
#endif
	if (!app.serverErr.empty()) { t.text = "The game server did not start: " + app.serverErr + where; t.error = true; return t; }
	if (app.local && app.server.running())
	{
		const ServerStage st = app.server.stage();
		t.text = "Starting the game server";
		if (!st.text.empty()) { t.text += ": " + st.text + " (" + std::to_string(st.seconds) + " s)"; t.step = st.step; t.steps = st.steps; }
		else { t.text += "..."; }
		return t;
	}
	if (!app.opt.simlink.empty()) { t.text = std::string("No game server") + where; t.error = true; }
	return t;
}

std::string title_status() { return title_status_full().text; }

// A local start in flight: the only state a held click waits for (kenshi-k16.2.210.6). A failure line or a dropped join never clears by itself.
static bool title_starting() { return !app.sim.active() && app.serverErr.empty() && app.local && app.server.running(); }

static const char* const kMenuButtonNames[4] = { "CONTINUE", "NEW GAME", "LOAD GAME", "IMPORT GAME" };

// A title action that needs the game server (kenshi-k16.2.210.4, user 2026-11-14: "The main menu lets you hit 'new game' before the engine is ready, and it
// just eats the input"): while title_status() says it cannot start yet, the click is not run but held, one at a time (the newest wins), and the title draws
// its button pushed (MainMenu::setPending). false: nothing to hold, run it now. EXIT and OPTIONS never come here: they need no server.
bool hold_menu_action(int button, const std::function<void()>& run)
{
	const std::string status = title_status();
	if (status.empty()) { return false; }
	if (!title_starting())	// a failed start (or a dropped join): nothing is coming to run it; the failure line stays, the click is dropped (kenshi-k16.2.210.6)
	{
		printf("title: not holding %s: %s\n", kMenuButtonNames[button], status.c_str());
		return true;
	}
	app.menuPending = button;
	app.menuPendingRun = run;
	app.mainMenu->setPending(button);
	printf("title: holding %s: %s\n", kMenuButtonNames[button], status.c_str());
	return true;
}

bool title_hold_dev(int button)
{
	return hold_menu_action(button, [button] { printf("title: held %s ran\n", kMenuButtonNames[button]); });
}

// The held click, once: clear it and run it (the status has cleared).
static void run_held_menu_action()
{
	const int button = app.menuPending;
	app.menuPending = -1;
	std::function<void()> run = std::move(app.menuPendingRun);
	app.menuPendingRun = nullptr;
	app.mainMenu->setPending(-1);
	printf("title: running the held %s\n", button >= 0 && button < 4 ? kMenuButtonNames[button] : "?");
	if (run) { run(); }
}

// Under the title nothing of the world streams or draws (docs/CLIENT_GOALS.md "Session front end"): the sim link is still read (STARTS, SAVES for
// its panels) and the UI drawn.
void frame_title(FramePacket& pk, double dt)
{
	const TitleStatus ts = title_status_full();
	const std::string& status = ts.text;
	if (ts.error) { app.mainMenu->setError(status); } else { app.mainMenu->setOffline(status, ts.step, ts.steps); }	// may grey the buttons: on screen it is drawn after this
	// The click held while the server started (kenshi-k16.2.210.4): run it once a game can act. A local server's link is open a frame or two before its
	// HELLO, and no order may go before that (a CONTINUE's load would be refused): the title's line is clear, the held click waits for the greeting.
	if (status.empty() && app.menuPendingRun && (!app.local || app.greeted)) { run_held_menu_action(); }
	else if (app.menuPending >= 0 && !status.empty() && !title_starting())	// the start failed while the click was held (kenshi-k16.2.210.6): drop it, keep the line
	{
		printf("title: dropping the held %s: %s\n", kMenuButtonNames[app.menuPending], status.c_str());
		app.menuPending = -1;
		app.menuPendingRun = nullptr;
		app.mainMenu->setPending(-1);
	}
	if (app.sim.active())
	{
		app.sim.update(dt, app.chars, [](double x, double z) { return app.hm->height(x, z); });
	}
	update_ui(dt);
	pk.path = FramePath::kTitle;
}

// The squad overlay in a live session: the sim's SQUADS once a second while it is on, coloured by the debug view's goal kind when the server
// sends it (KCLIENT_DEBUG_VIEW), else one colour. Subscribes the debug view while the overlay is on.
void live_squads()
{
#ifdef KCLIENT_DEBUG_VIEW
	if (app.greeted && app.squads.on != app.debugSubscribed && app.sim.sendDebug(dbg::DebugView::subscribe(app.squads.on))) { app.debugSubscribed = app.squads.on; }
#endif
	const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	if (!app.squads.on || now - app.squadsLiveAt < 1.0) { return; }
	app.squadsLiveAt = now;
	std::vector<Squads::Live> live;
	live.reserve(app.sim.decoder().squads.size());
	for (const simlink::Squad& s : app.sim.decoder().squads)
	{
		Squads::Live l{ s.id, s.x, s.z, 0.8f, 0.8f, 0.8f, "squad (no debug view)" };
#ifdef KCLIENT_DEBUG_VIEW
		if (const auto d = app.debugView.squads.find(s.id); d != app.debugView.squads.end())
		{
			float rgb[3];
			dbg::goalColour(d->second.goalKind, rgb);
			l = { s.id, s.x, s.z, rgb[0], rgb[1], rgb[2], ksim::debugview::goalKindName(d->second.goalKind) };
		}
#endif
		live.push_back(std::move(l));
	}
	app.squads.setLive(live, *app.hm);
}

// The profiling overlay's Sim block (SIMLINK.md "PROFILE", kenshi-k16.2.148): order 120 about once a second while draw_hud is on screen, so a
// KSIM_PROFILE server's per-system tick timings show beside the client's own GPU numbers. Nothing while the overlay is hidden (F7, the title, a
// full-screen map or the creator), so an idle client costs the server nothing. The client is a loopback session, which the server treats as admin;
// a non-admin or an OFF build refuses (not_allowed / unsupported) and SimView stops asking after that one answer.
void live_profile()
{
#if KCLIENT_PROFILE
	if (!app.opt.debug || app.hideUi || title_shown() || (app.creatorUi && app.creatorUi->shown()) || !app.sim.active()) { return; }
	app.sim.profileQuery();
#endif
}


// The stealth arrows' marks (kenshi-h2v.27.2; the original's geometry, kinspect findings/stealth_detection/rwh63_arrow_live.md section 1): for every
// DETECTED_BY (60) entry, on the line from the sneaker's drawn position to the observer's, at clamp(dist - 7, 0, 15) from the sneaker plus 12 u
// world Y, aimed along the full 3D direction. Colour by the entry (section 2): friendly 0x00ff00, state 1 (noticed) 0xdd0000, state 2 0xffdd00.
std::vector<Arrows::Mark> stealth_arrow_marks()
{
	std::vector<Arrows::Mark> marks;
	float hull[3];
	double sneak[3], seen[3];
	for (const auto& [sneaker, set] : app.sim.decoder().detectedBy)
	{
		if (!app.sim.placed(sneaker, sneak, hull)) { continue; }	// the sneaker's drawn feet
		for (const simlink::DetectedBy::Entry& e : set.by)
		{
			if (!app.sim.placed(e.observer, seen, hull)) { continue; }	// the observer's drawn feet
			const double dx = seen[0] - sneak[0], dy = seen[1] - sneak[1], dz = seen[2] - sneak[2];
			const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			if (dist < 1e-6) { continue; }
			const double k = std::clamp(dist - 7.0, 0.0, 15.0);
			Arrows::Mark m = {};
			m.pos[0] = sneak[0] + dx / dist * k;
			m.pos[1] = sneak[1] + dy / dist * k + 12.0;
			m.pos[2] = sneak[2] + dz / dist * k;
			m.dir[0] = dx / dist;
			m.dir[1] = dy / dist;
			m.dir[2] = dz / dist;
			if (e.friendly) { m.colour[1] = 1.0f; }
			else if (e.state == 1) { m.colour[0] = 0xdd / 255.0f; }
			else { m.colour[0] = 1.0f; m.colour[1] = 0xdd / 255.0f; }
			marks.push_back(m);
		}
	}
	return marks;
}

#ifdef KCLIENT_DEV_TOOLS
// The window-state gate's scripted burst (k16.2.222, dev builds only): KCLIENT_RESIZE_BURST="N,W,H[,MS]" resizes the window N times on the
// first world frame, one event every MS ms (default 16, so a 24-event burst stays inside the gate's 1 s), stepping from the size it started at
// to W x H with a few pixels of wobble either side (the last event is exactly W x H). The gate applies the first change at once, holds the
// rest, and applies the last at the cooldown's end: 2 applies. With KCLIENT_RESIZE_BURST_SHOT=FILE it waits for the gate to hold nothing, takes
// that shot (the window's final size) and quits, so a play.sh launch line can be checked headless; cleanup() then saves the applied geometry.
void resize_burst_frame()
{
	static int state = 0;	// 0: the env is read on the first world frame, 1: sending events, 2: waiting for the last apply, 3: done
	static int n = 0, i = 0, ms = 16, w0 = 0, h0 = 0, w1 = 0, h1 = 0;
	static long applyBase = 0;
	static std::chrono::steady_clock::time_point next, lastEvent;
	if (state == 3) { return; }
	if (state == 0 && world_loading()) { return; }	// start once the world is quiet: a frame stall over 1 s would let the cooldown expire mid-burst
	if (state == 0)
	{
		state = 3;	// reads the env once
		const char* r = getenv("KCLIENT_RESIZE_BURST");
		if (!r || sscanf(r, "%d,%d,%d,%d", &n, &w1, &h1, &ms) < 3 || n < 1 || w1 < 64 || h1 < 64)
		{
			if (r) { fprintf(stderr, "kclient: KCLIENT_RESIZE_BURST=%s: want N,W,H[,MS] with N >= 1 and a size of 64 or more\n", r); }
			return;
		}
		if (ms < 1) { ms = 1; }
		w0 = view::width();
		h0 = view::height();
		applyBase = windowgate::applies();
		next = lastEvent = std::chrono::steady_clock::now();
		printf("resize burst: %d events every %d ms from %dx%d to %dx%d\n", n, ms, w0, h0, w1, h1);
		state = 1;
	}
	const auto now = std::chrono::steady_clock::now();
	if (state == 1)
	{
		if (now < next) { return; }
		next += std::chrono::milliseconds(ms);
		const bool last = i + 1 >= n;
		int w = last ? w1 : w0 + (int)((long long)(w1 - w0) * (i + 1) / n) + ((i % 2) ? 7 : -7);
		int h = last ? h1 : h0 + (int)((long long)(h1 - h0) * (i + 1) / n) + ((i % 2) ? -5 : 5);
		w = std::max(64, w);
		h = std::max(64, h);
		sappcmd::post([w, h] { windowgeom::resizeWindow(w, h); });	// Xlib on the sapp thread, at this frame's window commands (kenshi-k16.2.205.12.25)
		printf("resize burst: event %d/%d %dx%d\n", i + 1, n, w, h);
		i++;
		lastEvent = now;
		if (last) { state = 2; }
		return;
	}
	// state 2: the gate applied the last held change (nothing is held any more), and a short settle after the last event
	if (windowgate::holding() || now - lastEvent < std::chrono::milliseconds(100))
	{
		// KCLIENT_RESIZE_BURST_HOLD=FILE documents the hold itself: a frame drawn at the applied size, scaled onto the (new) window, no black frame.
		static bool holdShot = false;
		if (!holdShot && windowgate::holding()) { holdShot = true; const char* h = getenv("KCLIENT_RESIZE_BURST_HOLD"); if (h && *h) { readback::screenshot(h); } }
		return;
	}
	printf("resize burst: done: %d events, %ld applies, gate %dx%d, window %dx%d\n", n, windowgate::applies() - applyBase, windowgate::width(), windowgate::height(), view::width(), view::height());
	state = 3;
	const char* shot = getenv("KCLIENT_RESIZE_BURST_SHOT");
	if (shot && *shot) { readback::screenshot(shot, [](bool) { request_quit(); }); }
}
#endif

// The RenderState render_frame draws with (renderframe.hpp): references to the render-owned objects App still holds, and the legacy blocks.
RenderState& render_state()
{
	static RenderState rs{ app.gpu, app.hdr, app.particles, app.sky, app.fogvol, app.post, app.buildings, app.terrain, app.foliage, app.grass, app.water, app.chars, app.squads, app.arrows, app.idbuf, app.shadow, app.sunShafts, app.dust, app.lightVolumes, app.refl, app.portraits, app.creatorScene3d, app.loadScreen, app.cursors, app.trails, app.sglShown, false, false };
	return rs;
}


// Rule 4's liveness fallback (kenshi-d2v2, user 2026-11-22): the render thread presents when a packet is ready and waits for it; gfx.vsync and
// gfx.fps_cap are the only pacing. This bound is not a pacing: it is how long frame() may hold the sapp thread with no packet before it gives the
// window a callback's worth of life (the kept frame, or overlap's redraw), so sokol's event pump runs (the sapp thread drains input only between
// callbacks), a title-bar close and a minimize are seen, and a load or a stall keeps the window answering. 100 ms is logic's own bound
// (logicthread.cpp kLogicBound, rule 4's stop-flag check): a quit lands within one such wait, a load screen redraws at 10 Hz at the least.
constexpr double kLivenessBoundMs = 100.0;

// Overlap's frame limiter (rule 11, kenshi-k16.2.205.12.118): gfx.fps_cap paces the render thread, at its frame start before the flip; one cap step at most, so
// rule 4's bound holds. A frame more than a step late restarts the schedule.
static void overlap_limiter()
{
	const int cap = kFpsCap.get();
	if (cap <= 0) { return; }
	static std::chrono::steady_clock::time_point next;
	const auto now = std::chrono::steady_clock::now();
	const auto step = std::chrono::nanoseconds(1000000000 / cap);
	if (now < next) { std::this_thread::sleep_until(next); next += step; }
	else { next = now + step; }
}

// The render thread's frame once the logic thread runs: present when ready. Wait for a packet, draw it and keep the composed frame (lastframe.hpp).
// Only after kLivenessBoundMs with no packet (a load, a stall): lockstep shows the kept frame again with no pass, overlap redraws its drawing packet
// (the draw reads the packet and render-owned state only, so a redraw is legal while logic writes app.). Never a frame repeated to hold a rate.
static void frame_lockstep()
{
#ifdef KCLIENT_DEV_TOOLS
	logicthread::renderStallPoint();	// the script's `renderstall MS`: a window held in its swap
#endif
	if (logicthread::mode() == logicthread::Mode::kOverlap) { overlap_limiter(); }
#ifdef KCLIENT_DEV_TOOLS
	// Dev check of overlap's redraw (kenshi-k16.2.205.12.118): KCLIENT_REDRAW_SHOTS=1 moves the pointer a step every render frame (it is render-live: the
	// draw reads pointer::x/y, not the packet) and, with KCLIENT_LOCKSTEP_STALL_SHOTS=DIR, writes the back buffer of the 1st and the 25th redraw.
	static const bool redrawShots = std::getenv("KCLIENT_REDRAW_SHOTS") != nullptr;
	static int redrawN = 0, frameN = 0;
	if (redrawShots) { frameN++; pointer::set(200.0f + 8.0f * (float)(frameN % 100), 360.0f); }
#endif
	const double bound = kLivenessBoundMs;
	const bool drew = logicthread::renderFrame(bound);
	// The frame's frames without a pass and the logic half beside the bound, into the pace log's once-a-second line (kenshi-k16.2.437.1): the
	// one-frame repeats do not print (logicthread::kMissLogMs), so this is where they are read.
	pacelog::noteLockstep(logicthread::takeMisses(), logicthread::lastLogicMs(), bound);
#ifdef KCLIENT_DEV_TOOLS
	if (redrawShots && drew && logicthread::lastWasRedraw() && (++redrawN == 1 || redrawN == 25)) { lastframe::stallShot(); }
#endif
	const bool unique = drew && !logicthread::lastWasRedraw();	// an overlap redraw of the drawing packet is a repeat
	realfps::note(unique);
	if (drew) { lastframe::keep(); pacelog::note(true, unique); return; }
	lastframe::show();
	pacelog::note(false, false);
#ifdef KCLIENT_DEV_TOOLS
	lastframe::stallShot();
#endif
}

// The swap interval asked for and in effect, once, a few callbacks after the first swap (kenshi-k16.2.418.1): on the sapp thread, which owns the GL
// context and does every swap in all render.thread modes. Printed once the logic thread has started (or at callback 60 when it never does), so the
// mode is the one the run really uses (a scripted overlap runs lockstep).
static void swap_startup_line()
{
	static int n = 0;
	static bool done = false;
	if (done || ++n < 3) { return; }
	const logicthread::Mode m = logicthread::mode();
	if (m != logicthread::Mode::kOff && !logicthread::running() && n < 60) { return; }
	done = true;
	const sapp_desc dd = sapp_query_desc();
	const char* mode = m == logicthread::Mode::kOverlap ? "overlap" : m == logicthread::Mode::kLockstep ? "lockstep" : "off";
	const bool gl = sg_query_backend() == SG_BACKEND_GLCORE;
	fprintf(stderr, "kclient: swap interval asked %d (gfx.vsync %s, disable_vsync %d), in effect %s; render.thread=%s, swaps on the sapp thread (the GL context's; logic has none)\n",
		dd.swap_interval, dd.swap_interval > 0 ? "on" : "off", (int)dd.disable_vsync, gl ? swapinfo::effective().c_str() : "n/a (not the GL backend)", mode);
}

// The frame callback: the window state on the sapp thread (published), the logic, the draw from its packet (render hook 6), then the queued
// window commands once (render hook 3, sappcmd.hpp).
void frame()
{
	gFrameRan = true;	// on the sapp thread, as queue_event reads it (kenshi-k16.2.205.12.23)
	// The sapp side's frame start (RENDER_THREAD.md 7.1 rule 10, kenshi-k16.2.205.12.23): these call sapp_* and X11, so they run here and
	// publish; logic takes the published copy (view::take, windowgate::take). windowgate polls the window's own size, so its order against
	// logic's event drain does not matter.
	view::snapshot();	// the window's size and frame time, once
	windowgeom::restore();	// the first few frames: remembered position, monitor, maximized and fullscreen (the size went into sapp_desc)
	windowgate::frame();	// k16.2.222: the first size change applies at once, later ones within a second are held (the latest applies at the cooldown's end)
#ifdef KCLIENT_DEV_TOOLS
	threadsplit::enter();
	struct Leave { ~Leave() { threadsplit::leave(); } } leave;
#endif
	swap_startup_line();
	pacelog::begin();
	if (logicthread::running()) { frame_lockstep(); return; }
	FramePacket& pk = gPackets[0];	// one thread: slot 0 only
	logic_frame(pk);
	CPUSTAGE_RENDER_BEGIN();
#ifdef KCLIENT_DEV_TOOLS
	if (pk.path == FramePath::kWorld) { threadsplit::split(pk.logicStart, pk.logicMs, false); }
#endif
	render_frame(pk, render_state());
	realfps::note(true);
	pacelog::note(true, true);
	sappcmd::flush();
	// render.thread=lockstep (RENDER_THREAD.md 7.1 rule 7): the logic thread starts once, at the end of the first frame that is not loading, and
	// frame() never runs logic_frame again. Off: never.
	if (logicthread::mode() != logicthread::Mode::kOff && !app.loading()) { start_logic_thread(); }
}

// Our tutorials (texts are ours, never the exe's): "Controls", a page per key group from the current bindings.
static std::vector<ui::Tutorial> tutorials()
{
	ui::Tutorial keys{ "controls", "Controls", {} };
	for (const auto& [group, heading] : { std::pair{ ui::keys::Group::kGame, "Game keys" }, std::pair{ ui::keys::Group::kBuild, "Build mode keys" } })
	{
		std::string text = heading + std::string("\n");
		for (const ui::keys::Action& a : ui::keys::actions()) { if (a.group == group) { text += (a.title.starts_with("Build: ") ? a.title.substr(7) : a.title) + "\t" + ui::keys::label(a.name) + "\n"; } }
		keys.pages.push_back({ text });
	}
	return { keys };
}

// The UI layer (docs/UI.md): the HUD's actions become SimView orders for the selection.
static void prospect_order();

// The debug panel's state, every frame while it shows.
#ifdef KCLIENT_DEBUG_VIEW
static double steady_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Asks the server for one squad's (kind 0) or character's (1) tree and shows the window; its answer fills it (debug_tree_frame)
void debug_tree_open(uint8_t kind, uint64_t id)
{
	if (!app.debugTree || !app.sim.sendDebug(dbg::DebugView::query(kind, id))) { return; }
	app.treeKind = kind;
	app.treeId = id;
	app.treeAskedAt = steady_seconds();
	app.debugTree->show(true);
}

// A task id as the AI_TASK table names it ("86 OPEN_UP_SHOP_DOORS", the table `dump chars` names the AI tail with); the number stays, as the wire gave it.
// Not a number, or a value the table does not name (0xFFFF = no task, past the table): the digits as they are.
static std::string named_task(const std::string& digits)
{
	const bool number = !digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos;
	const unsigned long value = number ? strtoul(digits.c_str(), nullptr, 10) : 0;
	const char* name = number && value <= 0xFFFF ? aiTaskName((uint16_t)value) : nullptr;
	return name == nullptr ? digits : digits + " " + name;
}

// The decisions subtree as the window shows it (SIMLINK "DEBUG VIEW" "Decisions subtree", kenshi-k16.6.43; k16.2.99): the server's rows are `pick`
// (value "tick T Source running R hold Hold -> Result TASK target ID score S tier N") with `candidate TASK` rows under it (value "<Verdict> tier N
// target ID utility U weight W crowd C score S"). Here a pick row carries its tick in its key and a goal row its verdict and numbers, as the wire
// gave them; the task ids are named (named_task, the k16.2.81 add-on), the pick's own task included; the picks are kept newest first, the order the server
// sends. Everything outside the subtree is passed through raw. False: no decisions row.
static bool decisions_rows(const dbg::Tree& t, std::vector<ui::DebugTreeRow>& out)
{
	bool any = false;
	for (size_t i = 0; i < t.rows.size();)
	{
		const dbg::TreeRow& r = t.rows[i];
		if (r.key != "decisions") { out.push_back({ r.depth, r.key, r.value }); i++; continue; }
		any = true;
		out.push_back({ r.depth, "decisions", r.value + " (newest first)" });
		size_t j = i + 1;
		int pick = 0;
		for (; j < t.rows.size() && t.rows[j].depth > r.depth; j++)
		{
			const dbg::TreeRow& pr = t.rows[j];
			if (pr.depth == r.depth + 1 && pr.key == "pick")
			{
				std::string tick, rest = pr.value;
				if (rest.compare(0, 5, "tick ") == 0)
				{
					const size_t sp = rest.find(' ', 5);
					tick = rest.substr(5, sp == std::string::npos ? std::string::npos : sp - 5);
					rest = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
				}
				// "Source running R hold H -> Result TASK target ...": name the picked task after its id
				const size_t arrow = rest.find(" -> ");
				const size_t id = arrow == std::string::npos ? std::string::npos : rest.find(' ', arrow + 4);	// the result word's space
				const size_t end = id == std::string::npos ? std::string::npos : rest.find(' ', id + 1);
				if (end != std::string::npos)
				{
					const std::string digits = rest.substr(id + 1, end - id - 1), named = named_task(digits);
					if (named != digits) { rest = rest.substr(0, id + 1) + named + rest.substr(end); }
				}
				out.push_back({ pr.depth, "pick " + std::to_string(++pick) + (tick.empty() ? std::string() : " (tick " + tick + ")"), rest });
			}
			else if (pr.key.compare(0, 10, "candidate ") == 0) { out.push_back({ pr.depth, "goal " + named_task(pr.key.substr(10)), pr.value }); }
			else { out.push_back({ pr.depth, pr.key, pr.value }); }
		}
		i = j;
	}
	return any;
}

// A character tree's `state` row as shown: the server's row reads its fight state only (dead / unconscious / down / up), so a caged prisoner read "up".
// CHAR_STATS state 5 (kLocked, the cage's lock; lockLevel) and 6 (kUnlocked, the cage open) say more: they replace the value (kenshi-k16.5.8.2).
// CHAR_STATS only flows for the player's characters and the inspect focus; any other character keeps the server's text.
static std::string tree_state_value(uint8_t kind, uint64_t id, const std::string& key, const std::string& value)
{
	using CS = simlink::CharStats;
	if (kind != 1 || key != "state") { return value; }
	const CS* cs = app.sim.charStats(id);
	if (!cs) { return value; }
	if (cs->state == CS::kLocked) { return "locked in a cage (lvl " + std::to_string(cs->lockLevel) + ")"; }
	if (cs->state == CS::kUnlocked) { return value + ", cage unlocked"; }
	return value;
}

void debug_tree_frame()
{
	if (!app.debugTree || !app.debugTree->shown()) { return; }
	if (app.treeAuto && steady_seconds() - app.treeAskedAt > 2.0) { debug_tree_open(app.treeKind, app.treeId); }
	const dbg::Tree& t = app.debugView.tree;
	const bool ours = t.kind == app.treeKind && t.id == app.treeId;
	ui::DebugTreeState st;
	char head[96] = "";
	if (app.treeKind == 0)
	{
		const auto sq = app.debugView.squads.find(app.treeId);
		float rgb[3] = { 0.8f, 0.8f, 0.8f };
		if (sq != app.debugView.squads.end()) { dbg::goalColour(sq->second.goalKind, rgb); }
		snprintf(head, sizeof head, "{#%02x%02x%02x}", (int)(rgb[0] * 255), (int)(rgb[1] * 255), (int)(rgb[2] * 255));
		st.title = head + std::string("Squad ") + std::to_string(app.treeId) + (sq == app.debugView.squads.end() || !sq->second.templ ? "" : " - " + app.sim.decoder().str(sq->second.templ));
	}
	else
	{
		std::string name = "#" + std::to_string(app.treeId);
		if (const auto ch = app.sim.decoder().chars.find(app.treeId); ch != app.sim.decoder().chars.end())
		{
			const std::string n = app.sim.decoder().str(ch->second.look.name);
			if (!n.empty()) { name = n; }
		}
		st.title = "Character " + std::to_string(app.treeId) + " - " + name;
	}
	st.tick = ours ? t.tick : 0;
	st.autoOn = app.treeAuto;
	if (ours && app.treeKind == 1)	// a character tree: its decisions subtree readable, the rest raw
	{
		if (!decisions_rows(t, st.rows) && !t.rows.empty()) { st.rows.push_back({ 1, "decisions", "none yet (this character has not picked a goal)" }); }
		for (ui::DebugTreeRow& r : st.rows) { r.value = tree_state_value(1, t.id, r.key, r.value); }
	}
	else if (ours) { for (const dbg::TreeRow& r : t.rows) { st.rows.push_back({ r.depth, r.key, r.value }); } }
	if (ours && t.rows.empty()) { st.rows.push_back({ 0, "(no rows)", "the server knows no such id" }); }
	app.debugTree->update(st);
}

#endif

// The UI sounds (kenshi-inspect findings/audio_load_pause.md): a load or new game request posts Create_or_Load_Game, a speed button posts its
// Game_Speed_* event, and a flip of the clock's pause bit posts Pause_Game / Resume_Game (update_ui). The audio thread runs the events from
// SFX_General_Bank / MT_Music_Bank and logs them. The third speed is 5x (ui::Hud::kSpeeds, as the original); its event is named x4.
const char* speed_event(float speed)
{
	return speed <= 1.0f ? "Game_Speed_Normal" : speed <= 2.0f ? "Game_Speed_x2" : "Game_Speed_x4";
}

// A window action (its key or the HUD's shortcut buttons). toggle_help is the client's key list until a help screen exists; the stats and research
// windows do not exist yet.
void shortcut(const std::string& action)
{
	if (action == "toggle_help" && !app.uiOn) { app.help = !app.help; }
	if (action == "toggle_help" && app.uiOn)
	{
		if (!app.tutorials->browserShown()) { app.tutorials->setAll(tutorials()); }	// the bindings may have changed
		app.tutorials->showBrowser(!app.tutorials->browserShown());
	}
	// Quick save / load: the save named kQuickSave in the server's save dir (the name is MINE; refusals come back as replies). The camera
	// sidecar goes with the save and comes back with its load (kenshi-k16.2.134.1).
	static const std::string kQuickSave = "quicksave";
	if (action == "quicksave" && app.sim.saveGame(kQuickSave))
	{
		camera_sidecar_save(kQuickSave);
		if (app.uiOn) { ui::message("Quick save."); }
	}
	if (action == "quickload" && app.sim.loadGame(kQuickSave, false))
	{
		app.camLoadName = kQuickSave;
		app.camLoadFrame = app.frames;
		audiohook_event("Create_or_Load_Game");
		app.menuPaused = false;
	}
	if (!app.uiOn) { if (action == "toggle_map") { app.wmap.full = !app.wmap.full; } return; }	// without the UI: the full-screen map
	static const std::pair<const char*, ui::OverviewPage> pages[] = { { "toggle_map", ui::OverviewPage::kMap }, { "toggle_faction", ui::OverviewPage::kFaction },
		{ "toggle_research", ui::OverviewPage::kResearch }, { "toggle_crafting", ui::OverviewPage::kCrafting }, { "toggle_squads", ui::OverviewPage::kSquads } };
	for (const auto& [name, page] : pages) { if (action == name) { app.overview->toggle(page); } }
	if (action == "toggle_options") { app.options->show(!app.options->shown()); }
	if (action == "prospect") { prospect_order(); }
	if (action == "toggle_build") { app.build->show(!app.build->shown()); }
	if (action == "toggle_inventory") { app.itemsShown = !app.itemsShown; }
	// the character stats window (kenshi-k16.2.96): the status panel's character, tracked in update_ui; its state is filled there each frame
	if (action == "toggle_stats" && app.stats) { app.stats->show(!app.stats->shown()); }
}
// A menu stops the game while it is up (MINE: the original pauses under its Escape menu; UNSURE under the title) and only undoes its own pause.
void menu_pause(bool on)
{
	// MINE: a menu's own pause and unpause are silent (the original's title has no running game; whether its escape menu posts Pause_Game is
	// not traced); the player's pause key and the HUD keep their sounds
	if (on && !app.menuPaused && app.sim.active() && (!(app.sim.clock().pauseBits & 1) || app.menuUnpaused)) { app.sim.pause(true); app.menuPaused = true; app.quietPauseTo = 1; }
	else if (!on && app.menuPaused) { app.sim.pause(false); app.menuPaused = false; app.menuUnpaused = true; app.quietPauseTo = 0; return; }
	app.menuUnpaused = false;
}

// The session loading screen (docs/CLIENT_GOALS.md "Session front end": no pop-in): from the camera's first placement on the squad or a reset (a load, a
// new game) until the world round the camera has streamed in. The game is paused under it (MINE: the original's sim does not run while it loads).
bool session_load_expected()
{
	const int mode = kSessionLoadScreen.get();
	return mode == 2 || (mode == 1 && !app.scripted());
}

void begin_session_load()
{
	const int mode = kSessionLoadScreen.get();
	if (mode == 0 || (mode == 1 && app.scripted())) { return; }
	logicthread::renderIdle();	// overlap (rule 12): the whole session load runs gated (logic_paths' frame-start gate from the next frame on)
	if (!app.sessionLoad) { app.loadScreen.init(app.opt.data, app.uiOn ? dl::gpu::view(ui::gfx::atlas()) : sg_view{}); }
	app.sessionLoad = true;
	logicthread::setLoadScreen(true);	// now, not at the next logic_frame's gate: the stall that ends in this frame's packet is the load's (kenshi-k16.2.437.4.4)
	app.sessionQuiet = app.sessionPeak = 0;
	app.sessionTime = 0;
	app.sessionFrameMax = 0;
	app.framePhase = "session load";	// the work this frame does next is the load's (the next frame's gap names it)
	printf("session: loading the world\n");
}

// Done after kQuietFrames frames with nothing streaming and the squad in view; a cap so a stuck worker cannot keep the game hidden, said loudly.
void update_session_load(double dt)
{
	static constexpr int kQuietFrames = 10;
	static constexpr double kMaxSeconds = 120;
	const bool newGame = !app.newGameStart.empty() || app.newGameSent || app.newGameLeader;
	const bool creator = app.creatorUi->shown();	// the creator's scene is what the screen waits for, not a squad (kenshi-k16.2.133.1)
	if (!newGame && !creator && !app.sessionPaused && app.sim.active() && !(app.sim.clock().pauseBits & 1)) { app.sim.pause(true); app.sessionPaused = true; }	// not before: NEW_GAME applies on a later tick
	const bool busy = world_loading(&app.sessionPending) || (!app.simCamera && !creator && app.sim.active()) || newGame;
	app.sessionPeak = std::max(app.sessionPeak, app.sessionPending);
	app.sessionTime += dt;
	app.sessionQuiet = busy ? 0 : app.sessionQuiet + 1;
	if (app.sessionQuiet < kQuietFrames && app.sessionTime < kMaxSeconds) { return; }
	if (app.sessionQuiet < kQuietFrames) { fprintf(stderr, "kclient: session loading screen: still streaming after %.0f s (%d pending); showing the game anyway\n", app.sessionTime, app.sessionPending); }
	end_session_load();
	printf("session: world loaded in %.1f s (peak %d pending)\n", app.sessionTime, app.sessionPeak);
}

// The load screen over, however it went: the pause it took is undone. (A scenario load the server never answers leaves it here too.)
static void end_session_load()
{
	if (!app.sessionLoad) { return; }
	app.sessionLoad = false;
	app.sessionShown = true;
#ifdef KCLIENT_DEV_TOOLS
	app.buildings.popinLift();
#endif
	if (app.sessionPaused) { app.sim.pause(false); app.sessionPaused = false; }
	fprintf(stderr, "kclient: session load: worst frame %.0f ms over the loading screen\n", app.sessionFrameMax);
}

// Single player (docs/CLIENT_GOALS.md "Session front end"): the title starts the server; frame() connects once it listens (poll_local_server).
void start_local_server()
{
	app.serverErr.clear();	// a fresh start: the title's offline line goes with the previous failure (kenshi-k16.3.12)
	std::string exe = kServerExe.get();
	std::vector<std::string> args;
	std::istringstream words(kServerArgs.get());
	for (std::string w; words >> w;) { args.push_back(w); }
	if (exe.empty())
	{
		std::error_code ec;
		exe = (kutil::executablePath(ec).parent_path() / "cpp/OpenKenshi-server").string();	// cpp/build has the pre-move data path (TESTING)
		std::string how = "built with this client";
#ifdef KCLIENT_DEV_TOOLS
		if (!std::filesystem::exists(exe, ec))
		{
			// No server in this build dir (a kclient-only build): the shared master server for the source tree (kenshi-cym.40.2.13).
			const dev::SharedServer sh = dev::sharedServer();
			if (!sh.path.empty())
			{
				exe = sh.path;
				how = "shared master server for " + sh.tree;
				bool hasSim = false;
				for (const std::string& a : args) { hasSim = hasSim || a == "--sim-data"; }
				if (!hasSim) { args.push_back("--sim-data"); args.push_back(sh.simData); }
			}
		}
#endif
		fprintf(stderr, "kclient: server %s (%s)\n", exe.c_str(), how.c_str());
		if (!std::filesystem::exists(exe, ec)) { fprintf(stderr, "kclient: no server binary found: build target ksim_server or set session.server\n"); }
	}
	// The data dir the client resolved, so the server needs no built-in path (kenshi-379.13: the release binary carries none).
	args.push_back("--data");
	args.push_back(app.opt.data);
	serverrows::append_flags(args);
	dev::followServerArgs(args);	// --import-save, --import-report, --paused (side-by-side follow mode, kenshi-cym.40.2.5)
	std::string err;
	if (!app.server.start(exe, args, err))
	{
		fprintf(stderr, "kclient: local server: %s\n", err.c_str());
#ifndef KCLIENT_DEV_TOOLS
		const std::string logFile = plat::logPath();	// Windows: the toast names the log file too (kenshi-379.31.9.3)
		ui::message("The game server did not start: " + err + (logFile.empty() ? std::string() : "; the game log is " + logFile));
#else
		ui::message("The game server did not start: " + err);
#endif
		app.serverErr = err;
		return;
	}
	app.local = true;
}

void poll_local_server()
{
	// A link that failed after it opened (bad HELLO, ERROR frame, decode error, drop): the player sees it once (kenshi-k16.2.380).
	if (app.sim.active() && !app.sim.error().empty())
	{
		static std::string shown;
		if (shown != app.sim.error())
		{
			shown = app.sim.error();
			ui::message("The game link failed: " + shown, ui::style().textColour(ui::Look::kMessage, ui::State::kNormal, false), 30);	// a long toast: a failure is not a passing event
			app.serverErr = "the game link failed: " + shown;
			app.local = false;
		}
	}
	if (!app.local || app.sim.active() || !app.charsReady) { return; }
#ifdef KCLIENT_DEV_TOOLS
	if (app.devServerHold > 0)	// script `serverhold N` (kenshi-k16.2.210.4): the title's early-click test holds the connect the first N frames
	{
		app.devServerHold--;
		return;
	}
#endif
	while (app.scripted() && app.server.address().empty() && app.server.running()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }	// shots stay deterministic
	if (const std::string at = app.server.address(); !at.empty())
	{
		std::string err;
		if (app.sim.open(at, app.opt.data, err)) { sim_opened(); }
		else { fprintf(stderr, "kclient: local server %s: %s\n", at.c_str(), err.c_str()); ui::message("Cannot reach the game server: " + err); app.serverErr = "cannot reach the game server: " + err; app.local = false; }
	}
	else if (!app.server.running())
	{
		// Why, in the server's own words (its last stderr line, e.g. "no game data directory: ..."), else how it exited (kenshi-k16.2.431.2).
		const std::string why = app.server.lastLine().empty() ? app.server.exited() : app.server.lastLine();
		ui::message("The game server stopped: " + why);
		app.serverErr = why.empty() ? "the server stopped" : why;
		app.local = false;
	}
}



// The mark drawn over a portrait (HudMember::mark, SquadMember::mark): the one place that picks it, the original's order (kenshi-rwh.112: Eaten > Prison >
// Slave, first wins). Prison: CHAR_STATS state 5 (locked in a cage). Slave: the character WEARS shackles (a LockedArmour item, not a slave state), which the
// wire carries as ITEM_LOCK 57 of the player character's INVENTORY (an entry for every worn pair, open or locked; empty without). Eaten: the wire has no
// "being eaten" state yet, so never set (kenshi-rwh.112). CHAR_STATS and INVENTORY flow for the player's characters, who are who has a portrait.
static ui::PortraitMark portrait_mark(uint64_t id)
{
	const simlink::CharStats* cs = app.sim.charStats(id);
	if (cs && cs->state == simlink::CharStats::kLocked) { return ui::PortraitMark::kPrison; }
	const simlink::Inventory* inv = app.sim.inventory(id);
	if (inv && !inv->locks.empty()) { return ui::PortraitMark::kSlave; }
	return ui::PortraitMark::kNone;
}

// The rest of a portrait's game-state layers (HudMember / SquadMember back, dim, hurt: ui::PortraitLayers), from the wire, the original's tests
// (kenshi-rwh.112). Back: Combat (CHAR_DYNAMIC kCombat) > Hunger (CHAR_MEDICAL hunger < 1.25 of 3, the original's char+0x4b8). Dim, the face at
// alpha 0.25: CHAR_DYNAMIC KO | dead | carriedBy != 0 | CHAR_MEDICAL putIn (k16.6.158). Hurt, the pulsing background: CHAR_MEDICAL hurt
// (the exe's 1404123a0 minus its camera gates, rwh.117).
ui::PortraitLayers portrait_layers(uint64_t id)
{
	ui::PortraitLayers L;
	L.mark = portrait_mark(id);
	if (const auto c = app.sim.decoder().chars.find(id); c != app.sim.decoder().chars.end())
	{
		const simlink::Dynamic& d = c->second.cur;
		if (d.flags & simlink::kCombat) { L.back = ui::PortraitBack::kCombat; }
		L.dim = (d.flags & (simlink::kKO | simlink::kDead)) != 0 || d.carriedBy != 0;
	}
	if (const simlink::Medical* med = app.sim.medical(id))
	{
		if (L.back == ui::PortraitBack::kNone && med->hunger >= 0 && med->hunger < 1.25f) { L.back = ui::PortraitBack::kHunger; }
		L.dim = L.dim || (med->flags & simlink::Medical::kPutIn) != 0;
		L.hurt = (med->flags & simlink::Medical::kHurt) != 0;
	}
	return L;
}



// The squads window over PLAYER_SQUADS (SIMLINK 31); each action is one order 70-75 (SIMLINK section 9), answered by a fresh record.
ui::SquadsState squads_state()
{
	ui::SquadsState s;
	const simlink::PlayerSquads& ps = app.sim.decoder().playerSquads;
	if (!ps.valid) { return s; }
	s.valid = true;
	s.used = ps.factionCount;
	s.factionMax = ps.maxFaction;
	s.maxSquads = ps.maxSquads;
	for (const simlink::PlayerSquads::Squad& q : ps.squads)
	{
		ui::SquadInfo info;
		info.id = q.id;
		info.name = q.name;
		info.max = ps.maxSquad;
		for (uint64_t id : q.members)
		{
			ui::SquadMember m;
			m.id = id;
			const auto c = app.sim.decoder().chars.find(id);
			if (c != app.sim.decoder().chars.end())
			{
				m.name = app.sim.decoder().str(c->second.look.name);
				m.down = c->second.cur.flags & (simlink::kKO | simlink::kDead);
				const ui::PortraitLayers layers = portrait_layers(id);
				m.mark = layers.mark;
				m.back = layers.back;
				m.dim = layers.dim;
				m.hurt = layers.hurt;
				uint32_t version = 0;
				const int index = app.sim.charIndex(id, &version);
				const uint32_t serial = c->second.look.portraitSerial;
				if (app.portraitTable.get(id, index, version, serial ? serial : (uint32_t)(id * 2654435761u), m.portrait.uv))
				{
					m.portrait.tex = app.portraitsTex;
				}
			}
			info.members.push_back(std::move(m));
		}
		s.squads.push_back(std::move(info));
	}
	return s;
}

ui::SquadsActions squads_actions()
{
	ui::SquadsActions a;
	a.newSquad = [] { app.sim.squadAdd(); };
	a.rename = [](uint64_t squad, const std::string& name) { app.sim.squadRename(squad, name); };
	a.remove = [](uint64_t squad) { app.sim.squadDelete(squad); };
	a.moveMember = [](uint64_t member, uint64_t squad, int index) { app.sim.squadMove(member, squad, index < 0 ? 0xFFFF : (uint16_t)index, false); };
	a.swapMembers = [](uint64_t m, uint64_t other)
	{
		// order 73 swap: m takes other's place (squad + index), other takes m's
		for (const simlink::PlayerSquads::Squad& q : app.sim.decoder().playerSquads.squads)
		{
			const auto at = std::find(q.members.begin(), q.members.end(), other);
			if (at != q.members.end()) { app.sim.squadMove(m, q.id, (uint16_t)(at - q.members.begin()), true); return; }
		}
	};
	a.moveSquad = [](uint64_t squad, int index) { app.sim.squadReorder(squad, (uint16_t)std::max(index, 0)); };
	a.dismiss = [](uint64_t member) { app.sim.dismiss(member); };
	a.close = [] {};
	return a;
}

// The RESEARCH page after kinspect findings/research25_static.md. Which records are listed (RESEARCH_STATE 63 state 1) and what is done (state 0)
// are the sim's; the client's own copy of the listing rules is in client/retired/research_listable.cpp. With no frame yet the page lists nothing
// and says so. Startable (Main) needs the cost in a bench and a bench of its level: the sim has no benches, so every row is red (Bad), as the original
// with none. Not done: repeats clones (`<id>.<k>`), the queue and progress (the sim).
ui::ResearchState research_state()
{
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	const simlink::SimLinkDecoder::ResearchState& rs = d.researchState;
	std::set<std::string> done, listed;
	for (const auto& rec : rs.records)
	{
		if (rec.state == 0) { done.insert(d.str(rec.research)); }
		else if (rec.state == 1) { listed.insert(d.str(rec.research)); }
	}
	constexpr int benchLevel = 0;	// the best built research bench's (none: the sim has no benches)
	constexpr double researchSpeed = 1;	// the advanced option rs, once NEW_GAME carries it
	const auto recOf = [](const std::string& sid) { const auto it = app.researchRecs.find(sid); return it != app.researchRecs.end() ? &it->second : nullptr; };
	const auto startable = [&](const App::ResearchRec& r) { return std::all_of(r.needs.begin(), r.needs.end(), [&](const std::string& q) { return done.count(q) > 0; }); };
	ui::ResearchState s;
	std::set<std::string> cats;
	for (const auto& [sid, r] : app.researchRecs) { cats.insert(r.category); }
	for (const std::string& c : cats) { s.categories.push_back({ c, c }); }
	s.category = cats.count(app.researchCategory) ? app.researchCategory : cats.empty() ? "" : *cats.begin();
	for (const auto& [sid, r] : app.researchRecs)
	{
		if (!listed.count(sid)) { continue; }
		ui::ResearchEntry e;
		e.id = sid;
		e.name = r.name;
		e.category = r.category;
		e.level = r.level;
		// hours = time x 0.4 x 1.2^(level - 1) / speed, divided by the bench rate when one works (none: as at rate 1); rounded up, "<1" under one
		const double hours = r.time * 0.4 * std::pow(1.2, r.level - 1) / researchSpeed;
		const int whole = std::max(1, (int)std::ceil(hours));
		e.time = hours < 1 ? "<1 hr" : whole == 1 ? "1 hr" : std::to_string(whole) + " hrs";
		e.eta = e.time + " (at 1x rate)";
		e.description = r.description;
		e.unlocks = r.unlocks;
		using N = ui::ResearchNeed::State;
		if (r.level > benchLevel) { e.warning = "Needs a level " + std::to_string(r.level) + " research bench"; }
		for (const auto& [item, count] : r.cost) { e.costs.push_back({ item + " x " + std::to_string(count), N::kMissing }); }	// nothing is in a bench
		for (const std::string& q : r.needs)
		{
			const App::ResearchRec* req = recOf(q);
			e.needs.push_back({ req ? req->name : q, done.count(q) > 0 ? N::kMet : N::kMissing });	// kQueued: once the queue is fed
		}
		const bool ok = benchLevel >= r.level && r.cost.empty() && startable(r);
		e.state = ok ? ui::ResearchEntry::State::kAvailable : ui::ResearchEntry::State::kLocked;
		s.entries.push_back(std::move(e));
	}
	std::sort(s.entries.begin(), s.entries.end(), [](const ui::ResearchEntry& a, const ui::ResearchEntry& b) { return std::tie(a.level, a.name) < std::tie(b.level, b.name); });
	s.caption = "RESEARCH - bench level " + std::to_string(benchLevel) + ", rate 0.00x";
	s.benchMessage = rs.valid ? "No research bench." : "No research state from the sim yet.";
	return s;
}

ui::ResearchActions research_actions()
{
	ui::ResearchActions a;
	a.category = [](const std::string& id) { app.researchCategory = id; };
	a.queue = [](const std::string& id) { printf("[research] queue %s (no bench in the sim yet)\n", id.c_str()); ui::message("Research needs a bench."); };
	a.unqueue = [](const std::string&) {};
	a.move = [](const std::string&, int) {};
	a.close = [] {};
	return a;
}

// The build window from the unlocked buildings (UNLOCKS 36) after kinspect findings/build_menu.md: categories = the raw `building category`, groups =
// `building group` (else the record's name), both sorted byte-wise; the arrows step a group's members by `build materials`, highest first, wrapping.
ui::BuildState build_state()
{
	ui::BuildState s;
	std::map<std::string, std::map<std::string, std::vector<const App::BuildRec*>>> tree;	// category -> group -> members (std::map: byte-wise)
	for (uint32_t sid : app.sim.decoder().unlocked)
	{
		const auto b = app.buildRecs.find(app.sim.decoder().str(sid));
		if (b != app.buildRecs.end()) { tree[b->second.category][b->second.group.empty() ? b->second.name : b->second.group].push_back(&b->second); }
	}
	if (!tree.count(app.buildCategory)) { app.buildCategory.clear(); app.buildGroup.clear(); }
	for (const auto& [cat, groups] : tree) { s.categories.push_back({ cat, cat, {} }); }
	s.category = app.buildCategory;
	if (app.buildCategory.empty()) { return s; }
	const auto& groups = tree[app.buildCategory];
	for (const auto& [g, members] : groups) { s.buildings.push_back({ g, g, members.empty() ? std::string() : members.front()->selectSound }); }
	const auto g = groups.find(app.buildGroup);
	if (g == groups.end()) { app.buildGroup.clear(); return s; }
	s.picked = app.buildGroup;
	std::vector<const App::BuildRec*> members = g->second;
	std::stable_sort(members.begin(), members.end(), [](const App::BuildRec* a, const App::BuildRec* b) { return a->materials > b->materials; });
	const int n = (int)members.size();
	app.buildStep = ((app.buildStep % n) + n) % n;
	const App::BuildRec& b = *members[app.buildStep];
	s.variants = n >= 2;
	s.title = b.name;
	s.description = b.description;
	// the stats (build_menu.md 3): Ka (the game's Building speed, 1 until the sim carries it) x Kb 6, read live
	constexpr float kBuildTimeK = 6;
	s.stats.push_back({ "Type:", b.type });
	s.stats.push_back({ "Materials cost:", "" });
	const ui::Color material = ui::res::colour("BuildMaterial");
	int total = 0;
	for (const auto& [item, count] : b.cost) { s.stats.push_back({ item, std::to_string(count), material, true }); total += count; }
	if (b.cost.empty() && b.materials > 0) { s.stats.push_back({ "Building Material", std::to_string(b.materials), material, true }); total = b.materials; }
	if (b.cost.empty() && b.materials <= 0) { s.stats.push_back({ "Nothing", "", material, true }); }
	const int hours = total > 0 && b.speedMult > 0 ? (int)std::lround(total / (b.speedMult * kBuildTimeK)) : 0;
	s.stats.push_back({ "Estimated build time:", std::to_string(hours) + (hours == 1 ? " hr" : " hrs") });
	if (b.powerOut > 0) { s.stats.push_back({ "Power Output:", "+" + std::to_string(b.powerOut), ui::res::colour("BuildPowerOut") }); }
	if (b.powerOut < 0) { s.stats.push_back({ "Power Consumption:", std::to_string(b.powerOut), ui::res::colour("BuildPowerUse") }); }
	if (b.powerCap > 0) { s.stats.push_back({ "Power Capacity:", std::to_string(b.powerCap) }); }
	if (b.operators > 0) { s.stats.push_back({ "Num People:", std::to_string(b.operators) }); }
	if (!b.produces.empty()) { s.stats.push_back({ "Produces:", b.produces }); }
	char v[32];
	if (b.function == 12) { snprintf(v, sizeof v, "%.0f", b.outputRate); s.stats.push_back({ "Damage:", v }); }
	else if (b.function >= 0 && b.function != 7) { snprintf(v, sizeof v, "%.0f%%", b.outputRate * 100); s.stats.push_back({ "Efficiency:", v }); }
	return s;
}

// PROSPECT (findings/prospecting.md): with the window up the button only hides it; else order 79 for the selected player character nearest the
// camera pivot (nothing without a selection). The result (PROSPECT 35) opens the window.
static void prospect_order()
{
	if (app.prospect->shown()) { app.prospect->show(false); return; }
	uint64_t best = 0;
	double bestD = 0;
	for (uint64_t id : app.sim.selected())
	{
		const auto ch = app.sim.decoder().chars.find(id);
		if (ch == app.sim.decoder().chars.end() || !app.sim.isPlayer(id)) { continue; }
		const double dx = ch->second.cur.pos[0] - app.pos[0], dz = ch->second.cur.pos[2] - app.pos[2], d = dx * dx + dz * dz;
		if (!best || d < bestD) { best = id; bestD = d; }
	}
	if (!best) { return; }
	const bool sent = app.sim.prospect(best);
	printf("sim: prospect %llu: %s\n", (unsigned long long)best, sent ? "sent" : "not sent");
	order_sent(sent);
}

// The selected resource's map, N x N as the sim sampled it (row 0 = the lowest z, drawn at the top like the world map), in the exe's colours:
// the altitude grey, a sample v > 0 as R = lerp(grey, 255, (1 - v) v), G = lerp(grey, 255, v^2), B = grey; Iron / Copper as one dot per deposit
// (2 x 2 when N > 40) coloured from its raw richness. Nearest sampling (MINE: the exe's filter is unknown).
// The MAP page's places from DISCOVERED (37), after kinspect findings/map24_static.md: icon and tier by TOWN type (vf111 / vf112), a
// types 8 and 10 never; camps (type 0) only within 3258.4 of a player character. Unvisited: the TOWN's `unexplored name` (Town+0x158; empty:
// the real name) and no tooltip. No NewDiscovered badge: the original sets it only for map items (14075f550), which the wire does not report;
// walking near a place (event 19) only logs "Discovered {1}" (Steam shots 2026-11-05 14:47: no badge on a fresh discovery).
// Relations are not on the wire: every place is Neutral; the visited tooltip (faction, relation, residents) waits for them.
// A place's name before it is visited: the TOWN's `unexplored name`, else its real one.
static std::string place_name(const std::string& sid, const std::string& name)
{
	const auto u = app.townUnexplored.find(sid);
	return u == app.townUnexplored.end() || u->second.empty() ? name : u->second;
}

// The original's "Discovered {1}" log line (event 19, the placeholder name), in the HUD message box (kenshi-k16.2.300.2).
void discovery_log()
{
	if (!app.townsReady) { return; }	// the events wait for the names
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	for (uint32_t t : app.sim.takeDiscovered())
	{
		const std::string sid = d.str(t);
		const auto town = std::find_if(app.towns.towns.begin(), app.towns.towns.end(), [&](const Town& w) { return w.sid == sid; });
		ui::message("Discovered " + place_name(sid, town == app.towns.towns.end() ? sid : town->name));
	}
}

// Events 51 / 52: the original's message-bar texts (oracle MESSAGES.md rows 14070fb60, 1409338b4), exact punctuation (kenshi-k16.2.300.4).
void crime_log()
{
	for (uint16_t k : app.sim.takeCrimeLog()) { ui::message(k == 51 ? "Got caught stealing." : "Alarm raised"); }
}

// Event 13 result 8 on a cage pick (kenshi-k16.2.346; the only case the sim still sends since 7fd41c98, kenshi-888g.1): a lock too hard for the
// picker, a message-bar line. The text is OURS: the original has none (LOCKS_EQUIP_STOCK_STATIC D-lockpick-zero).
void lock_too_hard_log()
{
	for (uint32_t n = app.sim.takeLockTooHard(); n > 0; --n) { ui::message("This lock is too hard to pick"); }
}

void map_places(ui::MapState& ms)
{
	using K = ui::MapPlaceKind;
	using T = ui::MapTier;
	if (!app.townsReady) { return; }
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	if (app.scripted() && d.discoveredSerial != app.seen.discovered)
	{
		app.seen.discovered = d.discoveredSerial;
		printf("discovered list: %zu", d.discovered.size());
		for (const auto& p : d.discovered) { printf(" %s%s", app.chars.nameOf(d.str(p.town)).c_str(), p.visited ? "*" : ""); }
		printf("\n");
	}
	std::unordered_map<std::string, bool> visited;
	for (const auto& p : d.discovered) { visited[d.str(p.town)] = p.visited; }
	int k = 0;
	for (const Town& t : app.towns.towns)
	{
		k++;
		const auto v = visited.find(t.sid);
		const auto ty = app.townTypes.find(t.sid);
		if (v == visited.end() || ty == app.townTypes.end() || ty->second == 8 || ty->second == 10) { continue; }
		const int type = ty->second;
		if (type == 0 && std::none_of(ms.characters.begin(), ms.characters.end(), [&](const auto& c) { return std::hypot(c.first - t.x, c.second - t.z) < 3258.4; })) { continue; }
		ui::MapPlace p;
		p.id = t.sid + "#" + std::to_string(k);
		p.kind = type == 2 ? K::kTown : type == 1 || type == 6 || type == 7 ? K::kOutpost : type == 3 ? K::kVillage : type == 0 ? K::kCamp : type == 4 ? K::kRuin : type == 5 ? K::kWorkcamp : K::kSmallPlace;
		p.tier = type == 0 || type == 9 ? T::kMax : type == 2 || type > 9 ? T::kMin : T::kMid;
		p.x = t.x;
		p.z = t.z;
		p.name = v->second ? t.name : place_name(t.sid, t.name);
		ms.places.push_back(std::move(p));
	}
}

void prospect_paint()
{
	const simlink::SimLinkDecoder::Prospect& p = app.sim.decoder().prospect;
	if (!app.prospectTex) { app.prospectTex = dl::newTex(); }
	// Render side, a frame-lane job of this packet (before its draws, as the calls it replaces): the old map's image and view go, the new one is
	// made and bound under prospectTex. Logic keeps only the pixels (kenshi-k16.2.205.12.39).
	const auto remake = [](std::vector<uint8_t> px, int n)
	{
		const size_t bytes = px.size();
		rq::push("prospect-map", [px = std::move(px), n, tex = app.prospectTex]
		{
			if (app.prospectView.id) { sg_destroy_view(app.prospectView); app.prospectView = {}; }
			if (app.prospectImg.id) { sg_destroy_image(app.prospectImg); app.prospectImg = {}; }
			if (n == 0) { dl::gpu::unbind(tex); return; }
			sg_image_desc id = {};
			id.width = id.height = n;
			id.pixel_format = SG_PIXELFORMAT_RGBA8;
			id.data = { px.data(), px.size() };
			id.label = "prospect-map";
			app.prospectImg = sg_make_image(&id);
			sg_view_desc vd = {};
			vd.texture.image = app.prospectImg;
			app.prospectView = sg_make_view(&vd);
			if (!app.prospectSmp.id)
			{
				sg_sampler_desc sd = {};
				sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
				sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
				app.prospectSmp = sg_make_sampler(&sd);
			}
			dl::gpu::bind(tex, app.prospectView, app.prospectSmp);
		}, rq::Lane::kFrame, bytes);
	};
	const int n = p.n;
	app.prospectShown = n != 0 && p.grey.size() == (size_t)n * n;
	if (!app.prospectShown) { remake({}, 0); return; }
	std::vector<uint8_t> px((size_t)n * n * 4, 255);
	for (size_t i = 0; i < p.grey.size(); i++) { px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = p.grey[i]; }
	const auto put = [&](size_t i, float v)
	{
		v = std::clamp(v, 0.0f, 1.0f);
		const float g = p.grey[i];
		px[i * 4] = (uint8_t)std::lround(g + (255 - g) * (1 - v) * v);
		px[i * 4 + 1] = (uint8_t)std::lround(g + (255 - g) * v * v);
	};
	const int plane = app.prospectRes == "5" ? 0 : app.prospectRes == "6" ? 1 : app.prospectRes == "2" ? 2 : -1;
	if (plane >= 0) { for (size_t i = 0; i < p.planes[plane].size(); i++) { if (p.planes[plane][i]) { put(i, p.planes[plane][i] / 255.0f); } } }
	for (const simlink::SimLinkDecoder::Prospect::Deposit& d : p.deposits)
	{
		if (plane >= 0 || std::to_string(d.res) != app.prospectRes || p.r <= 0) { continue; }
		const int c = (int)std::floor((d.x - (p.x - p.r)) / (2 * p.r) * n), row = (int)std::floor((d.z - (p.z - p.r)) / (2 * p.r) * n);
		const int size = n > 40 ? 2 : 1;
		for (int y = row; y < row + size; y++) { for (int x = c; x < c + size; x++) { if (x >= 0 && x < n && y >= 0 && y < n) { put((size_t)y * n + x, d.richness); } } }
	}
	remake(std::move(px), n);
}

// The PROSPECTING RESULTS lines from PROSPECT 35 (prospecting.md OVERVIEW / RESOURCES; labels ours where the original's are not plain words)
ui::ProspectState prospect_state()
{
	const simlink::SimLinkDecoder::Prospect& p = app.sim.decoder().prospect;
	ui::ProspectState s;
	char buf[64];
	const std::string zone = p.zone ? app.sim.decoder().str(p.zone) : "";
	s.overview[0].push_back({ "Zone", zone.empty() ? "-" : zone, {}, ui::res::colour("ProspectZone") });
	s.overview[0].push_back({ "Altitude", std::to_string((int)p.altitude) + " m" });
	s.overview[0].push_back({ "Prospector Skill", std::to_string((int)p.science) });
	s.overview[0].push_back({ "Area Size", std::to_string((int)(0.2f * p.r)) + " m" });
	snprintf(buf, sizeof buf, "%.1f %s", p.wind * speedScale(), kSpeedUnit.get().c_str());
	s.overview[1].push_back({ "Wind Speed", buf });
	static const char* const kEnv[] = { "", "Green", "Arid", "Swamp" };
	const ui::Color env = ui::res::colour("ProspectEnv");
	if (p.env.empty()) { s.overview[1].push_back({ "Environment", "Dead", {}, env }); }
	for (size_t i = 0; i < p.env.size(); i++)
	{
		const auto& [c, f] = p.env[i];
		s.overview[1].push_back({ i ? "" : "Environment", std::string(c < 4 ? kEnv[c] : "?") + " " + std::to_string((int)(f * 100)) + "%", {}, env });
	}
	static const char* const kRes[] = { "", "Iron", "Stone", "Copper", "", "Water", "Fertility" };
	for (const auto& [id, v] : p.res) { s.resources.push_back({ std::to_string(id), id < 7 ? kRes[id] : "?", (int)(v * 100) }); }
	s.selected = app.prospectRes;
	s.map.tex = app.prospectShown ? app.prospectTex : 0;
	if (p.r > 0)
	{
		const float u = (float)((app.pos[0] - (p.x - p.r)) / (2 * p.r)), v = (float)((app.pos[2] - (p.z - p.r)) / (2 * p.r));
		if (u >= 0 && u <= 1 && v >= 0 && v <= 1) { s.markerU = u; s.markerV = v; }	// the exe's marker is clipped by the map
	}
	s.markerAngle = (float)app.yaw;
	return s;
}


// --at PLACE (k16.2.87, the launcher's fly mode): a town's name from the game data, matched case-insensitively by prefix
// (the shortest name wins, and an exact name is as short as it can be), or an x,z pair read by the caller. The position is
// the town's placed instance from the world layout (Towns::join: data/newland/leveldata's 0-townlist transform). An unknown
// name lists the closest names; the caller exits 2 with the usage line.
static std::size_t edit_distance(const std::string& a, const std::string& b)
{
	std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
	for (std::size_t j = 0; j <= b.size(); j++) { prev[j] = j; }
	for (std::size_t i = 1; i <= a.size(); i++)
	{
		cur[0] = i;
		for (std::size_t j = 1; j <= b.size(); j++)
			cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0u : 1u) });
		std::swap(prev, cur);
	}
	return prev[b.size()];
}

static bool resolve_place(const std::string& dataDir, const std::string& want, double& x, double& z)
{
	kworld::Towns towns;
	try { towns = kworld::Towns::load(dataDir); }
	catch (const std::exception& e) { fprintf(stderr, "kclient: --at: cannot read the towns from %s: %s\n", dataDir.c_str(), e.what()); return false; }
	// "The Hub" is the game's name for the place everyone calls the Hub: a leading "The " is ignored on both sides (--at Hub works)
	const auto bare = [](std::string s) { if (s.rfind("the ", 0) == 0) { s.erase(0, 4); } return s; };
	const std::string q = bare(lower(want));
	struct Match { const kworld::TownRecord* town; const kworld::TownPlacement* place; std::size_t len; };
	std::vector<Match> matches;
	std::vector<const kworld::TownRecord*> placed;
	for (const kworld::TownRecord& t : towns.towns())
	{
		if (t.placementIndices.empty()) { continue; }	// a town record with no placed instance has no position
		placed.push_back(&t);
		if (!q.empty() && bare(lower(t.name)).rfind(q, 0) == 0) { matches.push_back({ &t, &towns.placements()[t.placementIndices.front()], t.name.size() }); }
	}
	if (!matches.empty())
	{
		std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.len != b.len ? a.len < b.len : a.town->name < b.town->name; });
		const Match& m = matches.front();
		x = m.place->x;
		z = m.place->z;
		printf("--at: \"%s\" -> %s at %.0f,%.0f%s\n", want.c_str(), m.town->name.c_str(), x, z,
			matches.size() > 1 ? (" (the shortest of " + std::to_string(matches.size()) + " matches)").c_str() : "");
		return true;
	}
	fprintf(stderr, "kclient: --at: no town starts with \"%s\"", want.c_str());
	if (placed.empty()) { fprintf(stderr, "\n"); return false; }
	std::vector<std::pair<std::size_t, const kworld::TownRecord*>> near;	// the closest names, five of them
	for (const kworld::TownRecord* t : placed) { near.push_back({ edit_distance(q, bare(lower(t->name))), t }); }
	std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first < b.first : a.second->name < b.second->name; });
	fprintf(stderr, "; the closest are:\n");
	for (std::size_t i = 0; i < near.size() && i < 5; i++) { fprintf(stderr, "  %s\n", near[i].second->name.c_str()); }
	return false;
}

// --at PLACE (a name, not an x,z): the eye goes back from the place along the view, so the sight line at the pitch meets the
// place's centre (k16.2.87 follow-up; user 2026-11-10: "fly --at should frame the place, not stand on its centre looking
// outward"). The sight line drops alt over a horizontal reach alt / tan(|pitch|): at yaw 0 (looking -z) the eye sits at
// z + reach, and the yaw turns the offset to the view's back. An x,z --at, or a --pos, is the caller's own eye and stays.
static void frame_place(Options& o)
{
	const double pitch = std::clamp(std::fabs(o.pitch), 1.0, 89.0) * M_PI / 180.0;	// 0 would put the eye at infinity
	const double reach = (o.walk ? kEyeHeight : o.alt) / std::tan(pitch);
	const double yaw = o.yaw * M_PI / 180.0;
	const double cx = o.x, cz = o.z;
	o.x = cx - reach * std::sin(yaw);
	o.z = cz + reach * std::cos(yaw);
	printf("--at: the eye %.0f back along the view (yaw %.0f, pitch %.0f), so the sight line meets the centre at %.0f,%.0f\n",
		reach, o.yaw, o.pitch, cx, cz);
}

// --print-defaults (user 2026-11-14, kenshi-k16.2.243): every declared option, its static compiled-in default and its help line, as
// config-file text on stdout, then exit: no window, no game data, and the user's file is never read (a saved value never shows). The
// defaults are the ones the option declarations compile in; a value detected at runtime (the monitor, the window layout's
// window.<layout>.* entries, screen size, hardware-probed quality) is not an option and never appears here.
void print_defaults()
{
	printf("# kclient options: every option with its static compiled-in default (kclient --print-defaults).\n"
		"# A value left equal to its default is not saved, so it follows a future change of the default.\n");
	printf("\n%s", cfg::defaultsText().c_str());
}

void usage()
{
	fprintf(stderr, "usage: kclient [--data DIR] [--pos x,z] [--at PLACE|x,z] [--alt H] [--yaw DEG] [--pitch DEG] [--walk] [--fog DIST]\n"
		"               [--size WxH] [--time HH.H] [--exposure auto|N] [--adapt] [--day N] [--sky-hours H] [--timescale N] [--simspeed N] [--weather NAME|auto] [--weather-blur KM] [--front-blur KM] [--bradius CELLS] [--fradius CELLS] [--foliage-range F] [--no-features] [--no-interiors] [--no-foliage] [--no-grass] [--no-fog-volumes] [--grass-range F] [--grass-density F] [--grass-map disk|paint] [--no-water] [--no-shadows] [--map GUI_Map.dds] [--squads FILE] [--character SPEC]... [--creator RACE[/female]] [--simlink FILE.bin|HOST:PORT] [--simlink-tick N] [--local-server] [--camera-pos X Y Z] [--camera-yaw DEG] [--camera-pitch DEG] [--camera-dist D] [--screenshot out.png] [--script FILE] [--follow NAME|id=N]\n"
		"               [--console]  (Windows: open a console for the log; otherwise it goes to <user data>\\logs\\openkenshi.log. KCLIENT_CONSOLE=1 does the same)\n"
		"               [--list-mods] [--mod KEY=on|off]... [--save-mods] [--set KEY=VALUE]... [--print-defaults] [--check-options]\n");
	dev::usage();
	fprintf(stderr, "               [--debug]  (adds the development features: the F8 debug panel, the debug keys, the scenes, the debug script ops; the title and the server start as without it)\n"
		"               [--fly]  (the offline fly-around: no title screen, no server; with --debug for the development views)\n"
		"               kclient %s (the build's git hash; a run records it)\n", kclient_git_hash());
}

// The shot's toast, on the log too: a headless run reads the outcome off its log, where it follows the writer thread's "wrote"/"failed to
// write" line (kenshi-k16.2.312).
static void shot_toast(const std::string& text)
{
	ui::toast(text);
	printf("toast: %s\n", text.c_str());
	fflush(stdout);
}

// F12 (or screenshot_delayed): a request for the shot of the frame being built, from logic_tail on the world and title paths (the title takes
// shots too); the sidecars follow the read, the toast the writer thread's result, so "Screenshot saved" means the file is there.
void shot_pending_frame()
{
	if (app.shotAt >= 0 && ui::now() >= app.shotAt) { app.shotAt = -1; app.shotPending = true; }
	if (app.shotPending)	// screenshot: into screenshot.dir (MINE; the game writes into its install), a toast either way
	{
		app.shotPending = false;
		const std::time_t now = std::time(nullptr);
		char name[64];
		std::strftime(name, sizeof name, "kclient-%Y%m%d-%H%M%S", std::localtime(&now));
		const std::filesystem::path dir = shot_dir();
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		const std::string base = (dir / name).string();
		const std::string ext = imagewrite::defaultExt();
		if (ec) { shot_toast("Screenshot failed: could not write " + base + ext); }
		else
		{
		// The sidecars describe the frame in the PNG (kenshi-k16.2.205.12.11): written here, at the request, from this frame's state; the readback's
		// done runs a frame later under the render thread (overlap), when the camera, selection and sim have moved on. A failed read removes them.
		dev::shotSidecars(dev_context(), base + ext);	// F12: .txt, .events.txt, .trails.tsv (dev/shotsidecars.cpp)
#ifdef KCLIENT_DEV_TOOLS
		shot_route(base + ext, app.frames);	// F12: <shot>.route.tsv, the selected characters' routes and last move orders (--debug)
#endif
		dev::shotTreesBegin(dev_context(), base + ext);	// the on-screen characters' trees (this frame's camera), beside the shot (docs/CLIENT.md "Screenshot debug trees")
		printf("shot request: frame %d eye %.1f %.1f %.1f yaw %.1f pitch %.1f (the .txt's camera line is this frame's)\n", app.frames, app.pos[0], app.pos[1], app.pos[2], app.yaw * 180 / 3.14159265358979, app.pitch * 180 / 3.14159265358979);
		readback::screenshot(base + ext, [base, ext](bool ok) {
		if (ok)
		{
			dev::shotBegin(base + ext);		// a --bead run: the shot also goes to the item's findings folder and as a comment (k16.2.127)
		}
		else
		{
			for (const char* ext : { ".txt", ".events.txt", ".trails.tsv", ".route.tsv", ".trees.txt" }) { std::error_code rec; std::filesystem::remove(base + ext, rec); }	// no picture: no sidecars
			shot_toast("Screenshot failed: could not write " + base + ext);	// the pixels were not read: no file to wait for
		}
		}, [base, ext, name = std::string(name)](bool ok, const std::string& why) {	// the writer's result (kenshi-k16.2.312): the toast follows the file
			dev::shotWritten(base + ext);	// a --bead run's copy and comment follow the file (no flushWrites on the logic thread)
			shot_toast(ok ? "Screenshot saved: " + name + ext : "Screenshot failed: could not write " + base + ext + ": " + why);	// the folder is screenshot.dir (the path alone wraps the toast)
		});
		}
	}
}

}
using namespace host;	// sokol_main and the global-scope code below see the host names unqualified, as they saw the anonymous namespace's (kenshi-k16.2.369.3)

sapp_desc sokol_main(int argc, char* argv[])
{
	plat::setupLog(argc, argv);	// Windows: the log file, or the console with --console (platform/console.hpp)
	kutil::crashlog_install((shot_dir().parent_path() / "crashes").string(), "kclient");
	app.trails.setDumpDir((shot_dir().parent_path() / "trails").string());	// debug.trails_dump's segments (kclient/trails beside the events; k16.19.6.1)
	Options& o = app.opt;
	std::vector<std::string> modArgs, setArgs;
	bool listMods = false, saveMods = false, hasPos = false, printDefaults = false, checkOptions = false;
	std::vector<std::string> windowOff;	// the argv texts given that disarm windowgeom (windowgeom::arm logs them verbatim, kenshi-k16.2.235.1)
	for (int i = 0; i < argc; i++) { o.cmdline += (i ? " " : "") + std::string(argv[i]); }
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const bool more = i + 1 < argc;
		if (a == "--data" && more) { o.data = argv[++i]; }
		else if (a == "--pos" && more) { if (sscanf(argv[++i], "%lf,%lf", &o.x, &o.z) != 2) { usage(); exit(2); } hasPos = true; }
		else if (a == "--at" && more) { o.at = argv[++i]; }
		else if (a == "--alt" && more) { o.alt = atof(argv[++i]); }
		else if (a == "--time" && more) { o.time = atof(argv[++i]); }
		else if (a == "--timescale" && more) { o.timescale = atof(argv[++i]); }
		else if (a == "--simspeed" && more) { o.simspeed = atof(argv[++i]); }	// dev: the sim's game speed (SET_SPEED once the session is live)
		else if (a == "--day" && more) { o.day = atoi(argv[++i]); }
		else if (a == "--sky-hours" && more) { o.skyHours = atof(argv[++i]); }
		else if (a == "--weather" && more) { o.weather = argv[++i]; }
		else if (a == "--weather-blur" && more) { o.weatherBlur = (float)atof(argv[++i]); }
		else if (a == "--front-blur" && more) { o.frontBlur = (float)atof(argv[++i]); }
		else if (a == "--fog" && more) { o.fog = (float)atof(argv[++i]); }
		else if (a == "--yaw" && more) { o.yaw = atof(argv[++i]); }
		else if (a == "--pitch" && more) { o.pitch = atof(argv[++i]); }
		else if (a == "--camera-pos" && i + 3 < argc) { o.camera = true; for (double& v : o.cameraPos) { v = atof(argv[++i]); } }
		else if (a == "--camera-yaw" && more) { o.camera = true; o.cameraYaw = atof(argv[++i]); }
		else if (a == "--camera-pitch" && more) { o.camera = true; o.cameraPitch = atof(argv[++i]); }
		else if (a == "--camera-dist" && more) { o.camera = true; o.cameraDist = atof(argv[++i]); }
		else if (a == "--size" && more) { const std::string v = argv[++i]; if (sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2) { usage(); exit(2); } windowOff.push_back("--size " + v); }
		else if (a == "--screenshot" && more) { o.screenshot = argv[++i]; windowOff.push_back("--screenshot " + o.screenshot); }
		else if (a == "--loading-shot" && more) { const std::string v = argv[++i]; o.loadShotStep = atoi(v.c_str()); o.loadShot = v.substr(v.find(',') + 1); windowOff.push_back("--loading-shot " + v); }
		else if (a == "--debug") { o.debug = true; }
		else if (a == "--fly") { o.fly = true; }	// the offline fly-around: no title, no server (kenshi-k16.2.475.1; was --debug without --simlink)
		else if (dev::parseArg(argc, argv, i, windowOff)) {}	// --playtest --play --bead --findings --playtests --scene-* --view-mesh --tex --dist
		else if (a == "--vanilla") { fprintf(stderr, "kclient: --vanilla is gone (k16.2.82): the original's display is the default now; drop the flag\n"); exit(2); }
		else if (a == "--script" && more) { o.script = argv[++i]; windowOff.push_back("--script " + o.script); }
		else if (a == "--follow" && more) { o.follow = argv[++i]; }	// the character to follow once it streams in (spectator)
		else if (a == "--exposure" && more) { const std::string v = argv[++i]; o.exposure = v == "auto" ? 0.0 : atof(v.c_str()); if (v != "auto" && !(o.exposure > 0)) { usage(); exit(2); } }
		else if (a == "--adapt") { o.adaptScripted = true; }
		else if (a == "--bradius" && more) { o.bradius = atoi(argv[++i]); }
		else if (a == "--offset-frame" && more) { o.offsetFrame = atoi(argv[++i]); }
		else if (a == "--hide-markers") { o.hideMarkers = true; }
		else if (a == "--no-features") { o.features = false; }
		else if (a == "--no-interiors") { o.interiors = false; }
		else if (a == "--no-water") { o.water = false; }
		else if (a == "--shadow-stagger" && more) { o.shadowStagger = atoi(argv[++i]); }
		else if (a == "--no-shadows") { o.shadows = false; }
		else if (a == "--water-flat") { o.waterFlat = true; }
		else if (a == "--fradius" && more) { o.fradius = atoi(argv[++i]); }
		else if (a == "--foliage-range" && more) { o.foliageRange = atof(argv[++i]); }
		else if (a == "--no-foliage") { o.foliage = false; }
		else if (a == "--no-grass") { o.grass = false; }
		else if (a == "--no-fog-volumes") { o.fogVolumes = false; }
		else if (a == "--no-particles") { o.particles = false; }
		else if (a == "--grass-range" && more) { o.grassRange = atof(argv[++i]); }
		else if (a == "--grass-map" && more)
		{
			const std::string v = argv[++i];
			if (v != "disk" && v != "paint") { std::fprintf(stderr, "--grass-map: disk or paint, not '%s'\n", v.c_str()); std::exit(2); }
			o.grassPaint = v == "paint";
		}
		else if (a == "--grass-density" && more) { o.grassDensity = (float)atof(argv[++i]); }
		else if (a == "--mat-report" && more) { o.matReport = argv[++i]; }
		else if (a == "--squads" && more) { o.squads = argv[++i]; }
		else if (a == "--character" && more) { o.characters.push_back(argv[++i]); }
		else if (a == "--creator" && more) { o.creator = argv[++i]; }
		else if (a == "--simlink" && more) { o.simlink = argv[++i]; }
		else if (a == "--local-server") { o.localServer = true; }
		else if (a == "--simlink-tick" && more) { o.simlinkTick = atof(argv[++i]); }
		else if (a == "--blood" && more) { o.blood.push_back(argv[++i]); }
		else if (a == "--map" && more) { o.map = argv[++i]; }
		else if (a == "--mod" && more) { modArgs.push_back(argv[++i]); }
		else if (a == "--set" && more) { setArgs.push_back(argv[++i]); }
		else if (a == "--list-mods") { listMods = true; }
		else if (a == "--print-defaults") { printDefaults = true; }
		else if (a == "--check-options") { checkOptions = true; }
		else if (a == "--save-mods") { saveMods = true; }
		else if (a == "--walk") { o.walk = true; }
		else { usage(); exit(2); }
	}
	if (checkOptions)	// every declared option has a section and a group (tests/options_groups); no window, no game data
	{
		const std::vector<std::string> bad = cfg::problems();
		for (const std::string& b : bad) { fprintf(stderr, "check-options: %s\n", b.c_str()); }
		printf("check-options: %zu options, %zu without a section or group\n", cfg::list().size(), bad.size());
		exit(bad.empty() ? 0 : 1);
	}
	if (printDefaults) { print_defaults(); exit(0); }	// before the data resolve and cfg::load(): the static compiled-in defaults only, no window
	// The dev-only launch guard (dev/launchguard.cpp, kenshi-k16.2.392): from here on the run opens a window, so a Claude session without a
	// gamescope above it (tools/headless.sh) is refused instead. After the argument checks' own exits (--check-options, --print-defaults, a
	// usage error) and before datasetting::resolve, whose first-run folder picker is a window too; --list-mods exits later without one.
	if (!listMods) { dev::launchGuard("kclient"); }
#ifdef KENSHI_EMBED_SERVER
	// The combined binary is what the preview ships (kenshi-379.13): with no options it behaves as `play.sh title`, --local-server included,
	// so a scripted or headless run shows the title screen and starts its own server too. --fly keeps the offline fly-around, --simlink
	// keeps joining a given server, and a scripted --debug run starts no server of its own (as before k16.2.475.1; --local-server asks for one).
	if (o.simlink.empty() && !o.fly && !(o.debug && (!o.script.empty() || !o.screenshot.empty() || dev::sceneTesting()))) { o.localServer = true; }
#endif
	// Game data without a flag (kenshi-379.13): --data, $KENSHI_DATA, the saved setting, then the Steam/GOG installs (datadir.cpp).
	{
		const std::string given = o.data;
		std::string why;
		if (!datasetting::resolve(given, o.data, why)) { fprintf(stderr, "kclient: %s", why.c_str()); exit(1); }
	}
	// The run's critical-events file, kclient/events beside the screenshots (kept per line: a crash loses nothing; the events of the
	// argument parsing went to the ring and are written into it here). Its first line is the build (k16.2.127): the sim's is this same
	// checkout when the client starts the server itself, else it arrives with the wire ("?").
	evlog::open((shot_dir().parent_path() / "events").string(),
		"build: client " + std::string(kclient_git_hash()) + " sim " + std::string(o.localServer ? kclient_git_hash() : "?"));
	ui::keys::setDebug(o.debug);	// --debug: the debug keys, the panel and the Options' Debug sections are live
#ifdef KCLIENT_DEV_TOOLS
	dev::followValidate(o.debug);	// --follow-stream / --import-save / --import-report need --debug (exit 2), dev/followstream.cpp
	if (dev::followEnabled()) { o.localServer = true; }	// follow mode starts its own server with the import; the title goes once the world is in
#endif
	if (!o.debug && o.simspeed > 0) { fprintf(stderr, "kclient: --simspeed needs --debug (a measurement flag, kenshi-k16.2.205.7)\n"); exit(2); }
	app.mods.discover(o.data);
	Post::retireOptions();	// the retired post.* keys (k16.2.82): cfg::load() warns for any a cfg file still sets
	app.mods.loadConfig();	// also cfg::load()
	for (const std::string& kv : setArgs)
	{
		if (!cfg::set(kv)) { fprintf(stderr, "kclient: bad --set '%s' (want KEY=VALUE for a known option)\n", kv.c_str()); exit(2); }
	}
	imagewrite::setWebp(kShotFormat.get() == "webp");	// after --set: screenshot.format=png
	for (const std::string& m : modArgs)
	{
		if (!app.mods.set(m)) { fprintf(stderr, "kclient: bad --mod '%s' (want KEY=on|off, keys: nicemap darkui slopeless bughouse)\n", m.c_str()); exit(2); }
	}
	if (saveMods && !app.mods.saveConfig()) { fprintf(stderr, "kclient: cannot write %s\n", app.mods.configPath().c_str()); }
	if (listMods) { app.mods.list(); exit(0); }
	if (!o.at.empty())
	{
		double ax = 0.0, az = 0.0;
		if (sscanf(o.at.c_str(), "%lf,%lf", &ax, &az) == 2) { o.x = ax; o.z = az; }	// an x,z is the eye itself: exact
		else if (!resolve_place(o.data, o.at, o.x, o.z)) { usage(); exit(2); }
		else if (!hasPos) { frame_place(o); }	// a name: the eye goes back so the place is framed, unless --pos pinned it
	}
	o.explicitPos = hasPos || !o.at.empty() || o.camera;	// the camera has a target before any session (k16.2.112.1)
	if (o.explicitPos) { app.targetReason = "option"; }	// first_world_frame says the option placed it
	if (o.map.empty())
	{
		const std::string nice = app.mods.activePath("nicemap") + "/gui/gfx/GUI_Map.dds";
		o.map = !app.mods.activePath("nicemap").empty() && std::filesystem::exists(nice) ? nice : o.data + "/gui/gfx/GUI_Map.dds";
	}
	printf("map image: %s\n", o.map.c_str());
	if (!dev::viewing())	// dev/viewmesh.cpp: --view-mesh has no world under the mesh
	{
		try { app.hm = kworld::Heightmap::open(o.data + "/newland/land/fullmap.tif"); }
		catch (const std::exception& e)
		{
			fprintf(stderr, "kclient: %s\n", e.what());
			exit(1);
		}
		app.chars.ground = [](double x, double z) { return app.hm->terrainHeight(x, z); };	// the drawn triangles: feet out of slopes
	}
	app.pos[0] = o.x;
	app.pos[2] = o.z;
	app.pos[1] = (dev::viewing() ? 0.0f : app.hm->height(o.x, o.z)) + (o.walk ? kEyeHeight : o.alt);
	app.yaw = o.yaw * 3.14159265358979 / 180.0;
	app.pitch = o.pitch * 3.14159265358979 / 180.0;
	app.walk = o.walk;
#ifdef KCLIENT_DEV_TOOLS
	{
		// What the dev tools (dev/devhost.cpp: the playtest store, --play, --bead) need to know of the game; they read the store now, so the
		// script ops and a window see it.
		dev::Env env;
		env.simActive = [] { return app.sim.active(); };
		env.live = [] { return app.sim.live(); };
		env.worldOn = [] { return app.firstWorldFrame; };
		env.paused = [] { return (app.sim.clock().pauseBits & 1) != 0; };
		env.pause = [](bool on) { app.sim.pause(on); };
		env.decoder = []() -> const simlink::SimLinkDecoder& { return app.sim.decoder(); };
		env.scenarioList = [] { return app.sim.scenarioList(); };
		env.scenarioLoad = [](const std::string& name, uint32_t seed) { return app.sim.scenarioLoad(name, seed); };
		env.scenarioEnd = [] { app.sim.scenarioEnd(); };
		env.pumpLink = [] { return app.sim.pumpLink(); };
		env.error = [](const std::string& text)
		{
			ui::message(text);
			end_session_load();
		};
		env.stamp = []
		{
			// "HH:MM.S Name (x, z): ": the game clock (minute and tenths), the first selected character and where it is drawn, else the
			// watched one (a spectator run has no player squad); with neither the camera stands in (a scripted offline Mark still prints).
			const bool simClock = app.sim.active() && app.sim.clock().valid;
			const double hour = simClock ? app.sim.clock().hour : app.sky.clock.hours;
			const int h = (int)std::floor(hour);
			std::string name = "camera";
			double x = app.pos[0], z = app.pos[2];
			const uint64_t stampId = !app.sim.selected().empty() ? app.sim.selected().front() : app.inspect;
			if (stampId)
			{
				const auto ch = app.sim.decoder().chars.find(stampId);
				if (ch != app.sim.decoder().chars.end()) { name = app.sim.decoder().str(ch->second.look.name); }
				double at[3];
				float hull[3];
				if (app.sim.placed(stampId, at, hull)) { x = at[0]; z = at[2]; }
			}
			char buf[160];
			snprintf(buf, sizeof buf, "%02d:%04.1f %s (%.0f, %.0f): ", h, (hour - h) * 60.0, name.c_str(), x, z);
			return std::string(buf);
		};
		env.sessionSim = [] { return app.local && app.sim.live() ? std::string(kclient_git_hash()) : std::string(); };
		env.started = [](bool ok, bool hasCentre, double cx, double cz)
		{
			if (!ok) { return; }
			// a started run is a session to watch, as a load or a new game: the world is chosen, the menus close and their pause is undone
			app.worldChosen = true;	// the scenario's world is in: a player-less one is a spectator's (kenshi-k16.2.95)
			app.centerOnStart = true;	// the camera goes to the scenario's player squad, once its characters stream in
			app.gameMenu->show(false);
			if (title_shown()) { app.mainMenu->show(false); }
			menu_pause(false);
			app.menuPaused = false;
			progress_level_sync(true);	// SCENARIO_STARTED resets the sim's TASK_PROGRESS level to 1
			// the scenario's centre first: the server streams characters near the camera, so a spectator (no squad to centre on) would never see the
			// fight from where the camera was (SIMLINK "SCENARIO", the STARTED tail)
			if (hasCentre)
			{
				teleport(cx, cz);
				begin_session_load();	// the world is drawn from here (the target is set), so the load screen stands over it from this frame, not from the squad's first snapshot (kenshi-k16.2.437.4.4)
				set_camera_target("scenario");	// the scenario's centre is the camera's target (k16.2.112.1)
			}
		};
		env.restLine = [] { return app.creator.active() ? app.chars.restLine(app.creator.index()) : std::string(); };
		env.shotInfo = []
		{
			// the game time as the shot's .txt records it ("09:41 day 2"), where the shot looks (the rts pivot and the character it is on, else the
			// free camera's eye) and the selected character's name (the watched one when nothing is selected, else "none")
			dev::Env::ShotInfo info;
			const GameClock& gc = app.sky.clock;
			const int minutes = (int)(gc.hours * 60.0);
			char buf[160];
			snprintf(buf, sizeof buf, "%02d:%02d day %d", minutes / 60, minutes % 60, gc.day + 1);
			info.time = buf;
			if (app.rts) { snprintf(buf, sizeof buf, "at %.0f, %.0f on %s", app.focus[0], app.focus[2], app.focusOn); }
			else { snprintf(buf, sizeof buf, "at %.0f, %.0f (free camera)", app.pos[0], app.pos[2]); }
			info.place = buf;
			const uint64_t id = !app.sim.selected().empty() ? app.sim.selected().front() : app.inspect;
			const auto ch = id ? app.sim.decoder().chars.find(id) : app.sim.decoder().chars.end();
			info.selected = ch == app.sim.decoder().chars.end() ? "none" : app.sim.decoder().str(ch->second.look.name);
			return info;
		};
		env.treesPending = [](const std::string& base) { return dev::shotTreesPending(base); };	// this shot's trees capture is still running (dev/shotsidecars.cpp; false with no debug view: the copy goes at once)
		env.labScripted = []
		{
			// a headless animation lab run: the script holds a `lab` op, so the world's characters (and their assets) are not loaded
			return std::any_of(app.cmds.begin(), app.cmds.end(), [](const Cmd& c) { return c.op == "lab"; });
		};
		env.repoRoot = repo_root();
		env.clientHash = kclient_git_hash();
		dev::init(env);
		{
			dev::FollowHost fh;	// side-by-side follow mode (dev/followstream.cpp)
			fh.sim = []() -> SimView& { return app.sim; };
			fh.worldOn = [] { return app.firstWorldFrame; };
			fh.beginSession = []
			{
				app.worldChosen = true;	// the imported world is the player's: place_session_camera takes CAMERA_START
				app.gameMenu->show(false);
				if (title_shown()) { app.mainMenu->show(false); }
			};
			fh.camSet = [](double yaw, double pitch, double dist, const double focus[3])
			{
				set_follow(false);
				app.rts = true;
				app.centerId = 0;
				app.yaw = yaw;
				app.pitch = pitch;
				app.rtsDist = dist;
				std::copy(focus, focus + 3, app.focus);
			};
			fh.camGet = [](double& yaw, double& pitch, double& dist, double focus[3]) { yaw = app.yaw; pitch = app.pitch; dist = app.rtsDist; std::copy(app.focus, app.focus + 3, focus); };
			fh.showSquadOf = [](uint64_t id)
			{
				const simlink::PlayerSquads& ps = app.sim.decoder().playerSquads;
				if (!ps.valid) { return false; }
				for (const simlink::PlayerSquads::Squad& q : ps.squads)
				{
					if (std::find(q.members.begin(), q.members.end(), id) != q.members.end()) { app.activeSquad = q.id; return true; }
				}
				return true;	// in no squad: nothing to show
			};
			fh.setFloor = [](int floor) { app.buildings.restoreView(floor, 0); };
			fh.toast = [](const std::string& m) { ui::message(m); };
			fh.clientHash = kclient_git_hash();
			dev::followInit(fh);
		}
		framehooks::labRender = &dev::labRender;	// the render half's dev draws: dev is above the frame layer (frame/renderframe.hpp)
		framehooks::viewDraw = &dev::viewDraw;
	}
#endif
	if (!o.script.empty() && !load_script(o.script)) { exit(2); }
	dev::sceneSetup(o.debug);	// the scenes file and a --scene-test's list (dev/scenetest.cpp; a dev flag without --debug exits here)
	if (!o.screenshot.empty())
	{
		// A --simlink run with no --script has no world at frame 3 (kenshi-k16.2.124.1): the shot waits for the first world frame instead
		// (screenshot_world_frame). Offline, viewer and scripted runs keep the frame-3 shot.
		if (!o.simlink.empty() && o.script.empty()) { app.screenshotOnWorld = true; }
		else
		{
			app.cmds.push_back({ kScreenshotFrames, "screenshot", { o.screenshot }, false });
			app.cmds.push_back({ kScreenshotFrames, "quit", {}, false });
		}
	}

	// The window's remembered geometry (windowgeom.hpp, kenshi-k16.2.109): only for a plain interactive start. A window flag (--size), a script,
	// a screenshot, a scene test or the loading-shot / mesh-view tools leave it, and the config file, alone; window.remember=0 too. The list is
	// what the command line gave, not what the options hold: a --debug run fills the default scene file, which is not a --scene-file
	// (kenshi-k16.2.235.1: play.sh launches came up unarmed on it). arm() logs every cause by its argv text.
	windowgeom::arm(windowOff);
	windowgeom::savedSize(o.width, o.height);
	logicthread::configure();	// render.thread, once (docs/RENDER_THREAD.md 7.1 rule 9)
#ifdef KCLIENT_DEV_TOOLS
	pacelog::init();
#endif

	sapp_desc d = {};
	d.init_cb = init;
	d.frame_cb = frame;
	d.cleanup_cb = cleanup;
	d.event_cb = queue_event;	// queued; frame() drains it into event() (inputqueue.hpp)
	d.width = o.width;
	d.height = o.height;
	d.window_title = "OpenKenshi";
	d.swap_interval = kVsync.get() ? 1 : 0;
	d.disable_vsync = !kVsync.get();
	d.enable_clipboard = true;	// ui EditBox: Ctrl+V
	d.logger.func = slog_func;
	return d;
}
