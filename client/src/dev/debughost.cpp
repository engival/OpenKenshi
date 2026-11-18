// The host side of the debug tooling (docs/CLIENT.md "Dev-only code is fenced", kenshi-k16.2.153.4). It holds the animation lab
// (dev/animlab.*): its one character, the `lab` script op, its offscreen pass and its browser window (ui::AnimBrowser), and the `dump` /
// `debug` script ops moved off main.cpp (dev::dumpOp, dev::machineOp, dev::debugOp). main.cpp reaches it only through dev/devhooks.hpp;
// what it needs of the game is passed in (labSetup's resource index, data dir and shadow, dev::Context's references), never App.
// Built only with KCLIENT_DEV_TOOLS.
#include "dev/devhooks.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "net/aitask.hpp"
#include "world/clientnav.hpp"
#include "input/collisionpick.hpp"
#include "render/navoverlay.hpp"
#include "world/worldbuildings.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#include "ksim/debugview/enums.hpp"
#include "ui/dev/debugtree.hpp"
#endif
#include "render/beamfield.hpp"
#include "characters/characters.hpp"
#include "app/config.hpp"
#include "dev/animlab.hpp"
#include "app/logicthread.hpp"
#include "dev/debughost.hpp"
#include "dev/pickboxes.hpp"
#include "audio/audiohook.hpp"
#include "app/eventlog.hpp"
#include "render/foliage.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/ogreskeleton.hpp"
#include "characters/physics.hpp"
#include "characters/ragdoll.hpp"
#include "characters/ragdolldrive.hpp"
#include "render/trails.hpp"
#include "app/framestats.hpp"
#include "render/gputimer.hpp"
#include "render/hdr.hpp"
#include "render/lightvol.hpp"
#include "render/particleworld.hpp"
#include "render/particles.hpp"
#include "net/progressfeed.hpp"
#include "render/readback.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "net/simview.hpp"
#include "render/sky.hpp"
#include "app/sappcmd.hpp"
#include "sokol_app.h"
#include "ui/dev/animbrowser.hpp"
#include "ui/dev/debugpanel.hpp"
#include "ui/dev/trailspanel.hpp"
#include "world/squads.hpp"
#include "world/towns.hpp"
#include "ui/gear.hpp"
#include "ui/gfx.hpp"
#include "ui/itemwindows.hpp"
#include "ui/ui.hpp"
#include "render/weff.hpp"
#include "world/weatherinstances.hpp"
#include "world/weathermap.hpp"
#include "world/worldbuildings.hpp"

namespace dev
{
namespace
{

// The lab is made on first use (its constructor only records nothing yet: the game's data, the render target and the character come with its
// first open or `lab` op) and destroyed at exit, after sokol's cleanup as the App's own members are.
animlab::Lab& lab()
{
	static animlab::Lab instance;
	return instance;
}

std::unique_ptr<ui::AnimBrowser> browser;	// the lab's window (the debug panel's Animations row opens it)

// The rest of a script op's line from args[1]: the clip / race / bone names hold spaces.
std::string rest_of(const std::vector<std::string>& args)
{
	std::string out;
	for (size_t i = 1; i < args.size(); i++) { out += (i > 1 ? " " : "") + args[i]; }
	return out;
}

}	// namespace

void labSetup(const ResourceIndex& res, const std::string& dataDir, const Shadow& shadow) { lab().setup(&res, dataDir, &shadow); }

bool labAvailable() { return lab().available(); }
bool labOpen() { return lab().state().open; }

bool labOp(const std::vector<std::string>& a)
{
	if (a.empty()) { return false; }
	animlab::Lab& l = lab();
	l.touch();
	const std::string rest = rest_of(a);
	if (a[0] == "clip" && a.size() >= 2) { l.select(rest); }
	else if (a[0] == "race" && a.size() >= 2) { l.race(rest); }
	else if (a[0] == "female" && a.size() == 2) { l.female(a[1] != "0"); }
	else if (a[0] == "play" && a.size() == 2) { l.play(a[1] != "0"); }
	else if (a[0] == "step" && a.size() == 2) { l.step(std::atoi(a[1].c_str())); }
	else if (a[0] == "seek" && a.size() == 2) { l.seek(std::atof(a[1].c_str())); }
	else if (a[0] == "speed" && a.size() == 2) { l.speed(std::atof(a[1].c_str())); }
	else if (a[0] == "camera" && a.size() >= 2) { l.camera(rest); }
	else if (a[0] == "plant" && a.size() >= 2) { l.plant(rest); }
	else if (a[0] == "overlay" && a.size() == 3) { l.overlay(a[1], a[2] != "0"); }
	else if (a[0] == "measure" && a.size() == 1) { l.measure(); }
	else if (a[0] == "list" && a.size() == 1) { l.list(); }
	else if (a[0] == "mark" && a.size() == 1) { l.mark(); }
	else if (a[0] == "view" && a.size() == 3) { l.viewSize(std::atoi(a[1].c_str()), std::atoi(a[2].c_str())); }
	else if (a[0] == "csv" && a.size() >= 2) { l.csv(rest); }
	else if (a[0] == "shot" && a.size() >= 2) { l.shot(rest); }
	else if (a[0] == "close" && a.size() == 1) { l.close(); }
	else if (a[0] == "group") { l.group(rest); }
	else if (a[0] == "follow" && a.size() == 2) { l.follow(a[1] != "0"); }
	else { return false; }
	return true;
}

bool labWait()
{
	animlab::Lab& l = lab();
	l.touch();
	return l.pending();
}

void labUpdate(double dt) { lab().update(dt); }

void labFrame(LabFrame& out) { lab().frame(out); }	// logic thread

void labRender(const LabFrame& f) { animlab::Lab::render(f); }	// render thread: the packet's copy only, never lab()

void labShutdown() { lab().shutdown(); }

void animationsCommand()
{
	logicthread::renderIdle();	// overlap (rule 12): the lab's target and character change under no draw
	animlab::Lab& l = lab();
	const bool on = !l.state().open;
	l.open(on);	// the lab inits on the way: a failed init keeps the window shut
	if (browser) { browser->show(l.state().open); }
}

void createAnimBrowser()
{
	ui::AnimBrowserActions aba;
	aba.race = [](const std::string& n) { lab().race(n); };
	aba.female = [](bool on) { lab().female(on); };
	aba.select = [](const std::string& n) { lab().select(n); };
	aba.play = [](bool on) { lab().play(on); };
	aba.step = [](int d) { lab().step(d); };
	aba.seek = [](float t) { lab().seek(t); };
	aba.speed = [](double x) { lab().speed(x); };
	aba.camera = [](const std::string& n) { lab().camera(n); };
	aba.orbit = [](float dx, float dy) { lab().orbit(dx, dy); };
	aba.zoom = [](float w) { lab().zoom(w); };
	aba.plant = [](const std::string& p) { lab().plant(p); };
	aba.overlay = [](const std::string& w, bool on) { lab().overlay(w, on); };
	aba.measure = [] { lab().measure(); };
	aba.mark = [] { lab().mark(); };
	aba.copy = [](const std::string& text) { sappcmd::setClipboard(text); };
	aba.viewSize = [](int w, int h) { lab().viewSize(w, h); };
	aba.close = [] { lab().close(); };
	aba.group = [](const std::string& g) { lab().group(g); };
	aba.follow = [](bool on) { lab().follow(on); };
	browser = std::make_unique<ui::AnimBrowser>(std::move(aba));
}

void animBrowserFrame()
{
	if (!browser || !browser->shown()) { return; }
	const animlab::State& ls = lab().state();
	ui::AnimBrowserState bs;
	bs.view = ls.view;
	bs.races = ls.races;
	bs.race = ls.race;
	bs.female = ls.female;
	for (const animlab::Clip& c : ls.clips) { bs.clips.push_back({ c.name, c.kind, c.length, c.frames, c.groups, c.usedBy, c.clip, c.relocates, c.autoFollow }); }
	bs.clip = ls.clip;
	bs.playing = ls.playing;
	bs.time = ls.time;
	bs.frame = ls.frame;
	bs.frames = ls.frames;
	bs.speed = ls.speed;
	bs.camera = ls.camera;
	bs.plant = ls.plant;
	bs.bones = ls.bones;
	bs.trails = ls.trails;
	bs.rootPath = ls.rootPath;
	bs.boneNames = ls.boneNames;
	bs.readout = ls.readout;
	bs.groups = ls.groups;
	bs.group = ls.group;
	bs.follow = ls.follow;
	bs.travel = ls.travel;
	bs.root = ls.root;
	bs.reply = ls.reply;
	browser->update(bs);
}

void resetAnimBrowser() { browser.reset(); }

// The debug panel (F8) and the trails panel (Shift+F6) host side (kenshi-k16.2.153.12), moved from main.cpp: the panel's toggles, commands, weather
// and time rows, its reply line, and the trails panel's view of Trails. The windows are owned here; main.cpp reaches them through dev/devhooks.hpp.
namespace
{
const Context* host = nullptr;	// the host's Context, set by createDebugPanels (the panels' actions run later, off any frame's argument)
std::unique_ptr<ui::DebugPanel> debugPanel;	// debug.help (F8): the debug toggles, scenes (offline) and the key list
std::unique_ptr<ui::TrailsPanel> trailsPanel;	// debug.trails_panel (Shift+F6): the trails window (--debug; kenshi-k16.3.11)
#ifdef KCLIENT_DEBUG_VIEW
std::string debugReply;	// the debug panel's reply line
uint32_t debugSeq = 0;	// the last debug command's clientSeq
uint32_t seenReplySerial = 0;	// DebugView::replySerial last shown

// The weather chooser's rows: the WEATHER records by name, a name shared by several records gets its sid ("Dust storm [1234-x.mod]"). Label -> sid.
const std::map<std::string, std::string>& debugWeatherRows(const Context& cx)
{
	static std::map<std::string, std::string> rows;
	if (!rows.empty() || !cx.wx.ready()) { return rows; }
	std::map<std::string, int> uses;
	for (const auto& [sid, w] : cx.wx.atlas().byId) { uses[w.name]++; }
	for (const auto& [sid, w] : cx.wx.atlas().byId) { rows[uses[w.name] > 1 ? w.name + " [" + sid + "]" : w.name] = sid; }
	return rows;
}

// The sim's weather (WEATHER 41) of the biome group under the first player character, as its chooser row; "" none known
std::string currentWeather(const Context& cx)
{
	double p[3];
	if (!cx.sim.active() || !cx.sim.focus(p)) { return ""; }
	const WeatherAtlas& a = cx.wx.atlas();
	const auto g = std::find(a.groupNames.begin(), a.groupNames.end(), a.groupAt(p[0], p[2]));
	if (g == a.groupNames.end() || g == a.groupNames.begin()) { return ""; }
	const std::string& groupId = a.groupIds[g - a.groupNames.begin()];
	const simlink::SimLinkDecoder& d = cx.sim.decoder();
	for (const simlink::Weather& w : d.weather)
	{
		if (d.str(w.group) != groupId) { continue; }
		for (const auto& [label, sid] : debugWeatherRows(cx)) { if (sid == d.str(w.weather)) { return label; } }
		return d.str(w.weather);
	}
	return "";
}

// A weather row (DEBUG_SET_WEATHER 213) for the biome group under the first player character, full strength; "auto" releases the pin
void debugWeather(const Context& cx, const std::string& label)
{
	std::string sid;
	if (label != "auto")
	{
		const auto& rows = debugWeatherRows(cx);
		const auto it = rows.find(label);
		if (it == rows.end()) { debugReply = "unknown weather row '" + label + "'"; return; }
		sid = it->second;
	}
	if (!cx.sim.sendDebug(dbg::DebugView::setWeather(++debugSeq, sid, "", 1.0f))) { debugReply = "not sent: not live"; return; }
	debugReply = "#" + std::to_string(debugSeq) + " weather " + label + ": waiting";
}

// A time button (DEBUG_SET_TIME 214), forward to the next such hour: dawn / dusk = GLOBAL CONSTANTS sunrise / sunset, +1h from the sim's clock
void debugTime(const Context& cx, const std::string& preset)
{
	const SkyConstants& k = cx.sky.constants();
	double hour;
	if (preset == "dawn") { hour = k.sunrise; }
	else if (preset == "noon") { hour = 12; }
	else if (preset == "dusk") { hour = k.sunset; }
	else if (preset == "midnight") { hour = 0; }
	else if (preset == "+1h") { hour = cx.sky.clock.hours + 1; }
	else	// the dial's hour, as a number ("14.25")
	{
		char* end = nullptr;
		hour = std::strtod(preset.c_str(), &end);
		if (preset.empty() || *end || !(hour >= 0 && hour <= 24)) { debugReply = "unknown time preset '" + preset + "'"; return; }
	}
	hour = std::fmod(hour, 24.0);
	if (!cx.sim.sendDebug(dbg::DebugView::setTime(++debugSeq, (float)hour, 0))) { debugReply = "not sent: not live"; return; }
	char buf[96];
	snprintf(buf, sizeof buf, "#%u time %s (%.2f h): waiting", debugSeq, preset.c_str(), hour);
	debugReply = buf;
}

// A debug panel command. Spawn bandit patrol: Dust Bandits (the sim's default template) 900 u from the nearest town's centre, on the far side of the
// first player character (MINE: outside the gate of a small town), raiding that town.
void debugCommand(const Context& cx, const std::string& id)
{
	if (id == "animations")	// the animation browser (CLIENT_GOALS "Animation lab"): the command opens and closes its window
	{
		animationsCommand();
		return;
	}
	if (id == "debug.lights") { cx.openLights(); return; }	// window buttons: the window is the answer, no reply text (k16.2.364)
	if (id == "debug.keys") { cx.openOptionsKeys(); return; }
	if (id == "debug.full_map") { cx.toggleFullMap(); return; }
	if (id == "debug.trails_window") { toggleTrailsPanel(); return; }
	if (id == "debug.decisions")	// the DEBUG VIEW tree of the status panel's character, its decisions readable (k16.2.99)
	{
		if (!cx.infoChar) { debugReply = "no character in the status panel"; return; }
		cx.debugTreeOpen(1, cx.infoChar);
		return;
	}
	// the cheat buttons (DEBUG_CHEAT 216): the status panel's character, 0 = the server's first player character
	const int op = id == "debug.heal" ? 0 : id == "debug.knockout" ? 1 : id == "debug.wake" ? 2 : id == "debug.kill" ? 3 : -1;
	if (op >= 0)
	{
		const uint64_t who = cx.infoChar;
		if (!cx.sim.sendDebug(dbg::DebugView::cheat(++debugSeq, who, (uint8_t)op))) { debugReply = "not sent: not live"; return; }
		std::string name = "first player character";
		if (who)
		{
			const auto ch = cx.sim.decoder().chars.find(who);
			if (ch != cx.sim.decoder().chars.end()) { name = cx.sim.decoder().str(ch->second.look.name); }
			if (name.empty()) { name = "#" + std::to_string(who); }
		}
		static const char* const kOps[] = { "heal", "knock out", "wake", "kill" };
		char buf[160];
		snprintf(buf, sizeof buf, "#%u %s %s: waiting", debugSeq, kOps[op], name.c_str());
		debugReply = buf;
		return;
	}
	double p[3];
	if (id != "debug.spawn_bandits" || !cx.townsReady || !cx.sim.focus(p)) { return; }
	const int n = cx.towns.nearest(p[0], p[2]);
	if (n < 0) { return; }
	const Town& t = cx.towns.towns[n];
	double dx = p[0] - t.x, dz = p[2] - t.z;
	const double len = std::sqrt(dx * dx + dz * dz);
	if (len < 1) { dx = 1; dz = 0; }
	else { dx /= len; dz /= len; }
	const float sx = (float)(t.x + dx * 900), sz = (float)(t.z + dz * 900);
	if (!cx.sim.sendDebug(dbg::DebugView::spawnSquad(++debugSeq, "", sx, sz, (float)t.x, (float)t.z))) { debugReply = "not sent: not live"; return; }
	char buf[160];
	snprintf(buf, sizeof buf, "#%u spawn at %.0f, %.0f for %s: waiting", debugSeq, sx, sz, t.name.c_str());
	debugReply = buf;
}

// The answers (DEBUG_SPAWNED, DEBUG_ACK) into the reply line
void replyFrame(const Context& cx)
{
	const dbg::DebugView::Reply& r = cx.debugView.reply;
	if (cx.debugView.replySerial == seenReplySerial) { return; }
	seenReplySerial = cx.debugView.replySerial;
	char buf[256];
	if (r.accepted && r.squad) { snprintf(buf, sizeof buf, "#%u ok: squad %llu", r.seq, (unsigned long long)r.squad); }
	else if (r.accepted) { snprintf(buf, sizeof buf, "#%u ok", r.seq); }
	else { snprintf(buf, sizeof buf, "#%u refused: %s", r.seq, r.reason.c_str()); }
	debugReply = buf;
	printf("debug reply: %s\n", buf);
}
#endif
}

void createDebugPanels(const Context& cx, bool debug)
{
	host = &cx;
	if (!debug) { return; }	// the F8 debug panel is a development feature (--debug)
	ui::DebugActions da;
	da.toggle = [](const std::string& id) { host->debugToggle(id); };
	da.scene = [](const std::string& name) { sceneSetName(*host, name); };	// dev/scenetest.cpp (offline only)
#ifdef KCLIENT_DEBUG_VIEW
	da.command = [](const std::string& id) { debugCommand(*host, id); };
	da.weather = [](const std::string& name) { debugWeather(*host, name); };
	da.time = [](const std::string& preset) { debugTime(*host, preset); };
#endif
	debugPanel = std::make_unique<ui::DebugPanel>(std::move(da));
	// the trails panel (kenshi-k16.3.11): Shift+F6, --debug only like the trails it drives; its actions are the trails' script ops
	ui::TrailsPanelActions tpa;
	tpa.setOn = [](bool on) { if (host->trails.on() != on) { host->trails.toggle(); } };
	tpa.pick = [](uint64_t id)
	{
		std::string reply;
		host->trails.op(id ? std::vector<std::string>{ "id", std::to_string(id) } : std::vector<std::string>{ "all" }, reply);
	};
	tpa.window = [](float s)
	{
		char b[32];
		snprintf(b, sizeof b, "%g", s);
		std::string reply;
		host->trails.op({ "window", b }, reply);
	};
	tpa.clear = [] { std::string reply; host->trails.op({ "clear" }, reply); };
	trailsPanel = std::make_unique<ui::TrailsPanel>(std::move(tpa));
}

void resetDebugPanels() { debugPanel.reset(); trailsPanel.reset(); }

bool toggleDebugPanel()
{
	if (!debugPanel) { return false; }
	debugPanel->show(!debugPanel->shown());
	return true;
}
void toggleTrailsPanel() { if (trailsPanel) { trailsPanel->show(!trailsPanel->shown()); } }

void trailsPanelFrame(const Context& cx)
{
	if (!trailsPanel || !trailsPanel->shown()) { return; }	// the trails panel's view of the trails (kenshi-k16.3.11)
	ui::TrailsPanelState ts;
	ts.on = cx.trails.on();
	ts.pick = cx.trails.picked();
	ts.window = (float)cx.trails.window();
	ts.cap = (float)Trails::cap();	// the button's LAST <cap> S (kenshi-k16.2.106)
	for (const Trails::Row& r : cx.trails.rows()) { ts.chars.push_back({ r.id, r.name, cx.trails.colour(r.id), r.points, r.pops }); }
	ts.status = cx.trails.status();
	trailsPanel->update(ts);
}

void debugPanelFrame(const Context& cx)
{
#ifdef KCLIENT_DEBUG_VIEW
	replyFrame(cx);
#endif
	if (!debugPanel || !debugPanel->shown()) { return; }
	ui::DebugState s;
	const auto add = [&](const char* id, const char* label, bool on, bool keyed = true) { s.toggles.push_back({ id, label, on, keyed ? id : "" }); };
	add("debug.navmesh", "Navmesh", cx.navOverlay.get());
	add("debug.navmesh_pruned", "Navmesh: pruned polygons", cx.navPruned.get(), false);	// a config key, no binding
	add("debug.trails", "Trails", cx.trails.on());
	add("debug.squad_overlay", "Squad overlay", cx.squads.on);
	add("debug.shadows", "Shadows", cx.shadow.enabled != cx.shadowFlip);
	add("debug.walk", "Walk on the ground", cx.walk);
	if (cx.sim.active()) { add("debug.follow", "Follow camera", cx.follow); }
	add("debug.free_camera", "Free camera", !cx.rts && !cx.follow);
	if (cx.follow) { add("debug.autorun", "Autorun", cx.autorun); }
	add("debug.clock_pause", "Clock paused", cx.sky.clock.paused);
	if (!cx.sim.active()) { for (const std::string& name : sceneNames()) { s.scenes.push_back(name); } }	// scenes place their own characters: offline only (dev/scenetest.cpp)
	for (const Squads::Source& src : cx.squads.sources())
	{
		char buf[96];
		snprintf(buf, sizeof buf, "{#%02x%02x%02x}# %s (%zu)\n", (int)(src.r * 255), (int)(src.g * 255), (int)(src.b * 255), src.name.c_str(), src.count);
		s.extra += buf;
	}
#ifdef KCLIENT_DEBUG_VIEW
	using G = ui::DebugGroup;
	s.commands.push_back({ "debug.spawn_bandits", "Spawn bandit patrol", cx.sim.active() && cx.townsReady });
	s.commands.push_back({ "animations", "Animations", labAvailable(), G::kWindow });	// the animation browser window
	s.commands.push_back({ "debug.lights", "Lights", true, G::kWindow });	// the LIGHTS window (Shift+F7)
	s.commands.push_back({ "debug.keys", "Keys", true, G::kWindow });	// Options at the Keys tab: the key list
	s.commands.push_back({ "debug.full_map", "Full map", true, G::kWindow });	// the full-screen world map (Ctrl+M)
	s.commands.push_back({ "debug.trails_window", "Trails window", true, G::kWindow });	// the trails window (Shift+F6)
	// k16.2.99: the status panel's character's AI decisions (the DEBUG_QUERY kind 1 tree) in the DEBUG VIEW window
	s.commands.push_back({ "debug.decisions", "Decisions", cx.sim.active() && cx.infoChar != 0, G::kWindow });
	// the cheats (kenshi-h2v.6.4), in the Character... popup: act on the status panel's character (k16.2.364). A running game with a debug view
	// and a character in the status panel; a view that is off answers its refusal on the reply line.
	const bool cheatOn = cx.sim.active() && cx.infoChar != 0 && (!cx.debugView.infoSeen || cx.debugView.enabled);
	s.commands.push_back({ "debug.heal", "Heal", cheatOn, G::kCharacter });
	s.commands.push_back({ "debug.knockout", "Knock out", cheatOn, G::kCharacter });
	s.commands.push_back({ "debug.wake", "Wake", cheatOn, G::kCharacter });
	s.commands.push_back({ "debug.kill", "Kill", cheatOn, G::kCharacter });
	if (cx.wx.ready())
	{
		for (const auto& [label, sid] : debugWeatherRows(cx)) { s.weathers.push_back(label); }	// sorted by the map
		s.weather = currentWeather(cx);
	}
	s.hour = (float)std::fmod(cx.sky.clock.hours, 24.0);	// the dial's hand and daylight arc
	s.sunrise = (float)cx.sky.constants().sunrise;
	s.sunset = (float)cx.sky.constants().sunset;
	s.weatherEnabled = s.timeEnabled = cx.sim.active();	// like the spawn button: DEBUG_INFO only comes with the overlay; a view that is off answers in the reply
	s.reply = debugReply;
#endif
	debugPanel->update(s);
}

// The `dump` script ops that read only the sim (kenshi-k16.2.153.17), moved from main.cpp's exec_cmd unchanged: stats doors things clock chars wire
// events players beams. Their output goes to stdout. False: not one of these (the host tries its other ops).
bool dumpOp(const Context& cx, const std::vector<std::string>& a)
{
	SimView& sim = cx.sim;
	Characters& chars = cx.chars;
	BeamField& beams = cx.beams;
	auto& doorSlides = cx.doorSlides;
	const int frames = cx.frames;
	if (a.size() == 1 && a[0] == "navdraw") { cx.navTrace = true; return true; }	// dump navdraw: the next overlay draw (debug.navmesh on) prints its stairs
	if (a.size() == 3 && a[0] == "nav")	// dump nav X Z: the client navmesh's polygons at (X, Z), every height (stdout)
	{
		if (cx.nav.mesh()) { dump_navmesh_at(*cx.nav.mesh(), atof(a[1].c_str()), atof(a[2].c_str())); }
		else { printf("nav: no mesh\n"); }
		return true;
	}
	if (a.size() >= 3 && a[0] == "navtile")	// dump navtile TX TZ [FILE]: the wire NAV_TILE held for (TX, TZ), every layer (stdout; bytes to FILE)
	{
		const auto& d = sim.decoder();
		printf("navtile %s %s: meshId %u\n", a[1].c_str(), a[2].c_str(), d.nav.meshId);
		for (const auto& [key, tile] : d.navTiles)
		{
			if (key[0] != atoi(a[1].c_str()) || key[1] != atoi(a[2].c_str())) { continue; }
			uint32_t h = 2166136261u;
			for (const uint8_t b : tile.data) { h = (h ^ b) * 16777619u; }
			printf("  layer %d version %u baked %u size %zu fnv1a %08x\n", key[2], tile.version, tile.baked, tile.data.size(), h);
			if (a.size() == 4)
			{
				const std::string path = a[3] + "." + std::to_string(key[2]);
				if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(tile.data.data(), 1, tile.data.size(), f); fclose(f); }
			}
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "pick")	// dump pick: what the mouse ray hits (kind, point, walkable), each shape it passed and why (k16.2.259), the nearest shape's box and, per container, the distance into its drawn box (stdout)
	{
		double dir[3], hit[3], ph[3];
		cx.mouseRay(cx.mouseX, cx.mouseY, dir);
		double from[3];	// the pick's own ray: the near plane (pick_at)
		for (int k = 0; k < 3; k++) { from[k] = cx.pos[k] + dir[k] * cx.cameraNear(); }
		std::vector<PickSkip> skipped;
		int kind;
		uint64_t pid;
		cx.pickAt(cx.mouseX, cx.mouseY, kind, pid, ph, skipped);
		printf("pick ray: near plane %.2f from %.1f %.1f %.1f (eye %.1f %.1f %.1f)\n", cx.cameraNear(), from[0], from[1], from[2], cx.pos[0], cx.pos[1], cx.pos[2]);
		printf("pick (%.0f, %.0f): kind %d id %llu hit %.1f %.1f %.1f walkable %d\n", cx.mouseX, cx.mouseY, kind, (unsigned long long)pid, ph[0], ph[1], ph[2], (int)cx.nav.walkable(ph));
		for (const PickSkip& s : skipped) { printf("pick skipped \"%s\" at %.1f %.1f %.1f: %s\n", s.name.c_str(), s.at[0], s.at[1], s.at[2], s.why.c_str()); }
		if (cx.rayGround(from, dir, hit)) { printf("pick terrain %.1f %.1f %.1f\n", hit[0], hit[1], hit[2]); }
		if (double ti; const uint64_t item = sim.pickItem(from, dir, &ti)) { printf("pick item sphere %llu at t %.1f\n", (unsigned long long)item, ti); }
		double cur[3] = { from[0], from[1], from[2] }, t;
		float b[6];
		bool solid = false;
		for (int n = 0; n < 8 && cx.collide.ray(cur, dir, 20000, t, b, {}, nullptr, &solid); n++)	// every shape along the ray, as ray_hit walks them
		{
			for (int k = 0; k < 3; k++) { hit[k] = cur[k] + dir[k] * t; cur[k] = hit[k] + dir[k] * 0.5; }
			printf("pick shape at %.1f %.1f %.1f box %.0f %.0f %.0f - %.0f %.0f %.0f solid %d see-through %d nav %d\n", hit[0], hit[1], hit[2], b[0], b[1], b[2], b[3], b[4], b[5], (int)solid,
				(int)cx.seeThrough(hit, solid ? b[1] : 1e30), (int)cx.nav.walkable(hit));
		}
		{	// the world hit's distance along this ray, to compare a container's box entry against
			double wh[3];
			bool wb = false;
			if (cx.rayHit(from, dir, wh, &wb, cx.pos)) { printf("pick world at t %.3f (%.2f %.2f %.2f) building %d\n", std::sqrt(std::pow(wh[0] - from[0], 2) + std::pow(wh[1] - from[1], 2) + std::pow(wh[2] - from[2], 2)), wh[0], wh[1], wh[2], wb ? 1 : 0); }
		}
		for (const auto& [id, th] : sim.things())	// containers: the distance into the piece's box (propRay) against the world hit's
		{
			if (th.kind != simlink::Interactable::kContainer) { continue; }
			double tc;
			if (cx.buildings.propRay(id, from, dir, tc)) { printf("pick container %llu box at t %.3f from-layout %d\n", (unsigned long long)id, tc, cx.buildings.propFromLayout(id) ? 1 : 0); }
		}
		if (cx.buildings.focus() >= 0) { printf("pick focus %s\n", cx.buildings.describe((uint32_t)cx.buildings.focus()).c_str()); }
		return true;
	}
#ifdef KCLIENT_DEBUG_VIEW
	if (a.size() == 1 && a[0] == "debugpanel")	// dump debugpanel: the panel's weather and reply (stdout)
	{
		replyFrame(cx);
		printf("debugpanel: weather '%s' (%zu records) reply '%s'\n", currentWeather(cx).c_str(), cx.wx.atlas().byId.size(), debugReply.c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "debugsquads")	// dump debugsquads: the debug view's state, squads per goal kind, the first 5 (stdout)
	{
		const dbg::DebugView& dv = cx.debugView;
		std::map<std::string, int> goals;
		for (const auto& [id, sq] : dv.squads) { goals[ksim::debugview::goalKindName(sq.goalKind)]++; }
		printf("debugview: info %d (%u frames) enabled %d version %u subscribed %d tick %llu squads %zu (SQUADS %zu)%s%s\n", (int)dv.infoSeen, dv.infoSerial, (int)dv.enabled, dv.version, (int)cx.debugSubscribed,
			(unsigned long long)dv.squadsTick, dv.squads.size(), sim.decoder().squads.size(), dv.error.empty() ? "" : " error: ", dv.error.c_str());
		for (const auto& [g, k] : goals) { printf("  goal %s: %d\n", g.c_str(), k); }
		int shown = 0;
		for (const auto& [id, sq] : dv.squads)
		{
			if (shown++ == 5) { break; }
			const auto& d = sim.decoder();
			printf("  squad %llu %s package %s goal %s fallback %s order %s detail %s members %u/%u dead %u target %s %.0f,%.0f town %s route %d %.0f/%.0f ids %zu\n", (unsigned long long)id,
				d.str(sq.templ).c_str(), d.str(sq.package).c_str(), ksim::debugview::goalKindName(sq.goalKind), ksim::debugview::aiReasonName(sq.fallbackReason),
				ksim::debugview::orderStatusName(sq.orderStatus), ksim::debugview::detailName(sq.detail), sq.effective, sq.members, sq.dead,
				ksim::debugview::targetKindName(sq.targetKind), sq.tx, sq.tz, d.str(sq.townSid).c_str(), (int)sq.routeValid, sq.along, sq.length, sq.memberIds.size());
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "debugtree")	// dump debugtree: the last DEBUG_TREE, indented (stdout)
	{
		const dbg::Tree& t = cx.debugView.tree;
		printf("debugtree kind %u id %llu tick %llu rows %zu\n", t.kind, (unsigned long long)t.id, (unsigned long long)t.tick, t.rows.size());
		for (const dbg::TreeRow& r : t.rows) { printf("  %*s%s: %s\n", r.depth * 2, "", r.key.c_str(), cx.treeStateValue(t.kind, t.id, r.key, r.value).c_str()); }
		return true;
	}
#endif
	if (a.size() == 1 && a[0] == "stats")	// dump stats: the ITEM_STATS answers received (stdout)
	{
		for (const auto& [key, st] : sim.decoder().itemStats)
		{
			printf("item stats %llu v%u #%llu: kind %u value %.0f grade %u level %.2f", (unsigned long long)key[0], (unsigned)key[1], (unsigned long long)key[2], st.kind, st.value, st.grade, st.level);
			if (st.kind == 1) { printf(" blunt %.2f cut %.2f harpoon %.1f parts %zu weight %.1f class %u material %u stealth %.2f", st.blunt, st.cut, st.harpoon, st.coverage.size(), st.weight, st.armourClass, st.material, st.stealthMult); }
			if (st.kind == 2) { printf(" cut %.2f blunt %.2f bleed %.2f ap %.2f weight %.1f type %u", st.cutDamage, st.bluntDamage, st.bleed, st.armourPenetration, st.weight, st.weaponType); }
			printf("\n");
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "doors")	// dump doors: every door the sim sent (id, state, use point, open slide; stdout)
	{
		for (const auto& [id, t] : sim.things())
		{
			if (t.kind != simlink::Interactable::kDoor) { continue; }
			const DoorSlide& d = doorSlides[sim.sidOf(t.record)];
			printf("door %llu state %u at %.0f %.0f %.0f slide axis %d dist %.0f out %.0f\n", (unsigned long long)id, t.state, t.use[0], t.use[1], t.use[2], d.axis, d.dist, d.out);
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "things")	// dump things: every interactable the sim sent (id, kind, record, an item's or container's name, position, owner tail, a container's layout piece and whether one draws it; stdout)
	{
		printf("things: own faction \"%s\"\n", sim.decoder().str(sim.ownFaction()).c_str());
		for (const auto& [id, t] : sim.things())
		{
			printf("thing %llu kind %u record %u \"%s\" at %.0f %.0f %.0f owner \"%s\"%s lock %u model \"%s\" yaw %.2f\n", (unsigned long long)id, t.kind, t.record, t.kind == simlink::Interactable::kItem || t.kind == simlink::Interactable::kContainer ? sim.nameOf(t.record, chars).c_str() : "", t.pos[0], t.pos[1], t.pos[2],
				sim.decoder().str(t.owner).c_str(), sim.foreign(id) ? " FOREIGN" : "", t.lockLevel, sim.decoder().str(t.model).c_str(), t.yaw);
			if (t.kind == simlink::Interactable::kBed) { printf("  bed building %llu furniture %u \"%s\" occupant %llu version %u use %.3f %.3f %.3f\n", (unsigned long long)t.building, t.furniture, sim.decoder().str(t.furniture).c_str(), (unsigned long long)t.occupant, t.version, t.use[0], t.use[1], t.use[2]); }
			if (t.kind == simlink::Interactable::kContainer) { printf("  container building %llu piece %u%s drawn %d piece-instance %d from-layout %d\n", (unsigned long long)t.building, (unsigned)t.piece, t.hasPiece ? "" : " (no tail)", cx.buildings.propInstance(id) >= 0 ? 1 : 0, cx.buildings.layoutPieceInstance(t.building, t.piece), cx.buildings.propFromLayout(id) ? 1 : 0); }
		}
		return true;
	}
	if (a.size() == 2 && a[0] == "prop")	// dump prop ID: the instance a prop (a container) is drawn and picked by and each part's pick box in world space (stdout)
	{
		printf("%s", cx.buildings.propReport(std::strtoull(a[1].c_str(), nullptr, 10)).c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "clock")	// dump clock: CLOCK tick, pause bits (1 idle / admin, 2 a reply menu) and the open menus (stdout)
	{
		const auto& ck = sim.clock();
		printf("clock: tick %llu paused bits %u, %zu open menus\n", (unsigned long long)ck.tick, ck.pauseBits, sim.decoder().menus.size());
		return true;
	}
	if (a.size() == 1 && a[0] == "chars")	// dump chars: every character on the wire (id, name, flags, position, yaw, CHAR_MEDICAL severed part bits, CHAR_STATS goal/strength/encumbrance, the AI tail goal/source/step/target, the stats window tail carry/resist/each skill's bar; stdout)
	{
		for (const auto& [id, ch] : sim.decoder().chars)
		{
			double at[3] = {};
			float hull[3];
			sim.placed(id, at, hull);
			const auto med = sim.decoder().medical.find(id);
			char rest[96] = "";
			if (ch.cur.hasRest) { snprintf(rest, sizeof rest, " rest %.2f %.2f %.2f restYaw %.2f", ch.cur.pos[0], ch.cur.pos[1], ch.cur.pos[2], ch.cur.restYaw); }
			char mounted[48] = "";
			if (ch.cur.flags & simlink::kMounted) { snprintf(mounted, sizeof mounted, " MOUNTED turret %llu", (unsigned long long)ch.cur.turret); }
			printf("char %llu \"%s\" race %s (%s) flags 0x%x%s%s%s at %.0f %.0f %.0f building %llu floor %d yaw %.2f severed 0x%x%s\n", (unsigned long long)id, sim.decoder().str(ch.look.name).c_str(), sim.decoder().str(ch.look.race).c_str(), chars.nameOf(sim.decoder().str(ch.look.race)).c_str(), ch.cur.flags, ch.cur.flags & simlink::kHostile ? " HOSTILE" : "", ch.cur.flags & simlink::kIndoors ? " INDOORS" : "", mounted, at[0], at[1], at[2], (unsigned long long)ch.cur.building, (int)ch.cur.floor, ch.cur.yaw, med == sim.decoder().medical.end() ? 0 : med->second.severed, rest);
			const auto cs = sim.decoder().charStats.find(id);
			if (cs != sim.decoder().charStats.end())
			{
				float strength = -1;
				for (const simlink::CharStats::SkillRow& k : cs->second.skills) { if (k.skill == simlink::CharStats::kStrength) { strength = k.value; } }
				printf("  stats state %u lockLevel %u goal \"%s\" strength %.1f encumbrance %.0f%%\n", (unsigned)cs->second.state, (unsigned)cs->second.lockLevel, sim.decoder().str(cs->second.goal).c_str(), strength, cs->second.encumbrance);
				if (cs->second.hasAi)	// the AI tail (rows 16-20): the enums' names, then the raw values
				{
					const char* goalName = aiTaskName(cs->second.goalEnum), *stepName = aiTaskName(cs->second.stepEnum);
					printf("  ai goal %s/%u source %u step %s/%u %u/%u target %llu at %.1f %.1f\n", goalName ? goalName : "none", (unsigned)cs->second.goalEnum,
						(unsigned)cs->second.source, stepName ? stepName : "none", (unsigned)cs->second.stepEnum, (unsigned)cs->second.stepIndex, (unsigned)cs->second.stepCount,
						(unsigned long long)cs->second.targetId, cs->second.targetX, cs->second.targetZ);
				}
				if (cs->second.hasProgress || cs->second.hasCarry || cs->second.hasResist)	// the stats window tail (rows 21-23): carry kg, resist, each skill's bar
				{
					printf("  window carry %.1f/%.1f kg resist %.0f%% progress:", cs->second.carryKg, cs->second.carryMaxKg, cs->second.damageResist * 100);
					for (const simlink::CharStats::SkillProgress& p : cs->second.progress) { printf(" %u:%.2f", (unsigned)p.skill, p.progress); }
					printf("\n");
				}
			}
			if (ch.cur.poseClip) { printf("  pose \"%s\" at %.1f %.1f %.1f yaw %.2f\n", sim.decoder().str(ch.cur.poseClip).c_str(), ch.cur.pose[0], ch.cur.pose[1], ch.cur.pose[2], ch.cur.pose[3]); }
		}
		return true;
	}
	if (a.size() >= 2 && a[0] == "wire")	// dump wire NAME...: characters whose name holds NAME, their last CHAR_DYNAMIC (tick, position, fwd, action), the newest complete snapshot's tick and the drawn position (stdout)
	{
		std::string want;
		for (size_t k = 1; k < a.size(); k++) { want += (k > 1 ? " " : "") + a[k]; }
		for (const auto& [id, ch] : sim.decoder().chars)
		{
			if (sim.decoder().str(ch.look.name).find(want) == std::string::npos) { continue; }
			double at[3] = {};
			float hull[3];
			sim.placed(id, at, hull);
			printf("wire %llu frame %llu tick %llu complete %llu at %.2f %.2f fwd %.2f action %u flags 0x%x carriedBy %llu drawn %.2f %.2f\n", (unsigned long long)id, (unsigned long long)frames,
				(unsigned long long)ch.cur.tick, (unsigned long long)sim.decoder().lastCompleteTick, ch.cur.pos[0], ch.cur.pos[2], ch.cur.fwd, ch.cur.action, ch.cur.flags,
				(unsigned long long)ch.cur.carriedBy, at[0], at[2]);
			// k16.2.183: the equip state and the appearance record the sim last sent (drawn bit, version, each item's section: 0 back, 1 back2, 2 hip, 9 hand)
			printf("  equip drawn %d appearanceVersion %u (look %u) items", (ch.cur.flags & simlink::kDrawn) ? 1 : 0, (unsigned)ch.cur.appearanceVersion, (unsigned)ch.look.version);
			for (const simlink::Item& it : ch.look.items) { printf(" %u@s%u", (unsigned)it.item, (unsigned)it.section); }
			printf("\n");
		}
		return true;
	}
	if (!a.empty() && a.size() <= 2 && a[0] == "events")	// dump events [SECONDS]: the critical-events log (all held, or the last SECONDS; stdout)
	{
		const size_t n = evlog::write(stdout, a.size() == 2 ? atof(a[1].c_str()) : 0.0);
		printf("events: %zu (run file %s)\n", n, evlog::runFile().c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "players")	// dump players: each player character's last CHAR_DYNAMIC and CHAR_ROUTE (clientSeq, version, flags, waypoints) (stdout)
	{
		for (const auto& [id, ch] : sim.decoder().chars)
		{
			if (!(ch.cur.flags & simlink::kPlayer)) { continue; }
			printf("player %llu \"%s\" frame %llu tick %llu at %.2f %.2f %.2f fwd %.2f action %u route seq %u version %u flags 0x%x points %zu:", (unsigned long long)id,
				sim.decoder().str(ch.look.name).c_str(), (unsigned long long)frames, (unsigned long long)ch.cur.tick, ch.cur.pos[0], ch.cur.pos[1], ch.cur.pos[2], ch.cur.fwd,
				ch.cur.action, ch.route.clientSeq, ch.route.version, ch.route.flags, ch.route.points.size());
			for (const auto& p : ch.route.points) { printf(" %.2f,%.2f,%.2f", p[0], p[1], p[2]); }
			printf("\n");
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "beams")	// dump beams: the sim's Venge beams (id, group, spawn/end ticks, the ground point at the render tick, wind-up/live/tail, scripts) (stdout)
	{
		printf("%s", beams.dump(sim.active() ? sim.tick() : -1.0).c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "beamfeet")	// dump beamfeet: every placed beam's foot at the render tick with its particle system (per technique: material, count, centroid and farthest offset) and the foot-fire verdict (kenshi-h2v.14.9.1)
	{
		// The foot fire (kenshi-rwh.175 2.6): the same burst's Flame1 patch, emitted at the handler point for its 9 s and dead ~2 s later, so
		// it rides the foot (parented) and never outlives the beam's burst. A fire farther than the radius from its beam's foot, or one still
		// burning past the burst, is the failure this op exists to catch. The burst is measured on the system's own clock (`PlacedSystem` time:
		// an event psys plays from the frame it is placed), not the beam's sim age: a scripted run advances the particle sim by a fixed step a
		// frame while the sim ticks by wall time, so the two clocks drift (a scripted beamfoot run read ~0.9 s of psys time per sim second).
		constexpr double kFootRadius = 400.0;	// u: the Flame1 Circle's 140 u radius, a 2 s particle at the 84 u/s drift, and slack
		constexpr double kFireSeconds = 11.5;	// s of psys time: the fire's 9 s emitter + its 2 s particles, one shot (the strip's last is at 12)
		const double tick = sim.active() ? sim.tick() : -1.0;
		const std::vector<BeamField::Foot> fs = beams.feet(tick);
		const std::vector<Particles::PlacedInfo> ps = cx.particles.placedInfo();
		int fires = 0, fails = 0;
		for (const BeamField::Foot& f : fs)
		{
			const double age = (tick - (double)f.spawnTick) / 20.0;
			const Particles::PlacedInfo* sys = nullptr;
			for (const Particles::PlacedInfo& p : ps) { if (p.key == f.key) { sys = &p; break; } }
			printf("beamfoot id %u key %016llx state %d age %.2f foot %.1f %.1f", f.id, (unsigned long long)f.key, f.state, age, f.x, f.z);
			if (!sys) { printf(" (no placed system)\n"); continue; }
			printf(" system %.1f %.1f %.1f time %.2f\n", sys->pos[0], sys->pos[1], sys->pos[2], sys->time);
			for (const Particles::PlacedTech& t : sys->techs)
			{
				printf("  tech \"%s\" n %zu centroid %+.1f %+.1f %+.1f far %.1f\n", t.material.c_str(), t.count, t.centroid[0], t.centroid[1], t.centroid[2], t.far);
				if (t.material != "Flame1" || t.count == 0) { continue; }
				fires++;
				const double d = std::hypot(sys->pos[0] + t.centroid[0] - (double)f.x, sys->pos[2] + t.centroid[2] - (double)f.z);
				if (d > kFootRadius) { printf("  FAIL fire %.1f u from the foot (limit %.0f)\n", d, kFootRadius); fails++; }
				if (sys->time > kFireSeconds) { printf("  FAIL fire at %.2f s of the burst, past the %.1f s it lives (age %.2f s)\n", sys->time, kFireSeconds, age); fails++; }
			}
		}
		printf("beamfeet: %zu placed beams, %d ground fires, %d fails\n", fs.size(), fires, fails);
		return true;
	}
	// kenshi-k16.2.153.19 (pass 2): the buildings / floor / light / window dumps, moved from main.cpp unchanged.
	WorldBuildings& buildings = cx.buildings;
	if (a.size() == 1 && a[0] == "playerbuildings")	// dump playerbuildings: the BUILDING 47 rows of the player's faction (k16.2.36.1)
	{
		for (const simlink::SimLinkDecoder::TaskProgress& t : sim.decoder().tasks) { printf("ptask char %llu kind %u progress %.3f rate %.4f target %llu\n", (unsigned long long)t.ch, t.kind, t.progress, t.rate, (unsigned long long)t.target); }
		for (const auto& [id, b] : sim.decoder().buildings)
		{
			const bool mine = sim.playerBuilding(b);
			printf("pbuilding %llu \"%s\" owner %llu%s built %d progress %.3f (wire %.3f rate %.4f) hp %.0f/%.0f at %.0f %.0f %.0f drawn %d tick %llu now %.0f materials %zu status %d workers %d/%d\n", (unsigned long long)id, sim.sidOf(b.record).c_str(),
				(unsigned long long)b.owner, mine ? " (player)" : "", (b.flags & simlink::SimLinkDecoder::Building::kBuilt) ? 1 : 0, (ProgressFeed{ b.progress, b.rate, b.tick }).at(sim.tick()), b.progress, b.rate,
				b.hp, b.maxHp, b.pos[0], b.pos[1], b.pos[2], buildings.propInstance(id) >= 0 ? 1 : 0, (unsigned long long)b.tick, sim.tick(), b.materials.size(), (int)b.status, (int)b.workers, (int)b.maxWorkers);
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "building")	// dump building: the parts of the floor-control building, else the one nearest the camera (stdout)
	{
		const int b = buildings.focus() >= 0 ? buildings.focus() : buildings.nearestBuilding(cx.pos[0], cx.pos[2]);
		if (b < 0) { fprintf(stderr, "kclient: script: no building near the camera\n"); cx.scriptError(cx.frames, "dump"); }
		printf("%s", buildings.partsReport((uint32_t)b).c_str());
		return true;
	}
	if (a.size() >= 1 && a.size() <= 2 && a[0] == "pickboxes")	// dump pickboxes [R]: every drawn container within R u of the camera (0 = all), its pick box against the drawn mesh (kenshi-k16.2.260.1)
	{
		const double R = a.size() == 2 ? atof(a[1].c_str()) : 600.0;
		int n = 0;
		std::string out;
		for (const auto& [id, th] : sim.things())
		{
			if (th.kind != simlink::Interactable::kContainer) { continue; }
			const double dx = th.pos[0] - cx.pos[0], dz = th.pos[2] - cx.pos[2];
			if (R > 0 && dx * dx + dz * dz > R * R) { continue; }
			const int inst = buildings.propInstance(id);
			if (inst < 0) { continue; }
			char head[256];
			snprintf(head, sizeof head, "container %llu \"%s\" at %.1f %.1f %.1f instance %d\n", (unsigned long long)id, sim.nameOf(th.record, chars).c_str(), th.pos[0], th.pos[1], th.pos[2], inst);
			out += head;
			out += dev::pickBoxReport(buildings, (uint32_t)inst, cx.worldToScreen);
			n++;
		}
		printf("pickboxes: %d drawn containers within %.0f u of the camera\n%s", n, R, out.c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "light_bake")	// dump light_bake: the last building debug.light_probes asked about (stdout)
	{
		printf("%s\n", buildings.lightBakeLine().c_str());
		return true;
	}
	if (a.size() == 2 && a[0] == "light_slice")	// dump light_slice LAYER: one horizontal layer of the probes as text (stdout)
	{
		printf("%s", buildings.lightBakeSlice(atoi(a[1].c_str())).c_str());
		return true;
	}
	if (a.size() >= 1 && a.size() <= 2 && a[0] == "open_windows")	// dump open_windows [MAXLUMA]: the barrier-quad survey of every building type with interiors (render.open_windows, kenshi-jj3.10.1; stdout)
	{
		printf("%s", buildings.openWindowsReport(a.size() == 2 ? (float)atof(a[1].c_str()) : 0.35f).c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "windows")	// dump windows: the open item windows (kenshi-k16.20.13.2): the character's, each box / body / stock with its user, the target, where each is on screen (stdout)
	{
		const auto where = [&](uint64_t id) { const ui::CaptionWindow* w = cx.items ? cx.items->window(id) : nullptr; return w ? w->screen() : ui::Rect{}; };
		const ui::Rect cr = where(cx.gearChar);
		printf("windows: character %llu %s at %.0f,%.0f %.0fx%.0f; target %llu; pending %llu; docked %llu\n", (unsigned long long)cx.gearChar, cx.itemsShown ? "shown" : "hidden",
			cr.x, cr.y, cr.w, cr.h, (unsigned long long)cx.itemTarget, (unsigned long long)cx.pendingId, (unsigned long long)(cx.items ? cx.items->docked() : 0));
		for (const OpenCont& c : cx.conts)
		{
			const ui::Rect r = where(c.id);
			printf("windows: %s %llu user %llu%s%s at %.0f,%.0f %.0fx%.0f%s\n", c.trade ? "stock" : c.body ? "body" : "box", (unsigned long long)c.id, (unsigned long long)c.user,
				c.view ? " view" : "", c.pinned ? " pinned" : "", r.x, r.y, r.w, r.h, cx.itemTarget == c.id ? " TARGET" : "");
		}
		if (const ui::Gear* g = cx.items ? cx.items->character(cx.gearChar) : nullptr)	// each grid's items: cell, footprint, count, and where the grid's top-left cell is on screen
		{
			for (int gi : { (int)ui::kGearMain, (int)ui::kGearBag })
			{
				const ui::ItemGrid* grid = g->grid(gi);
				if (!grid || grid->cols <= 0) { continue; }
				printf("windows: grid %s %dx%d origin %.0f,%.0f cell %.1f px:", gi == ui::kGearMain ? "main" : "bag", grid->cols, grid->rows, grid->screen().x, grid->screen().y, grid->cell * ui::scale());
				for (const ui::GridItem& it : grid->items) { printf(" %s@%d,%d %dx%d x%u", it.name.c_str(), it.x, it.y, it.w, it.h, it.count); }
				printf("\n");
			}
		}
		if (cx.items)
		{
			static const char* const kBadge[] = { "none", "buy", "sell", "transfer", "menu" };
			printf("windows: badge %s mods %u\n", kBadge[(int)cx.items->badge()], cx.items->badgeMods());
			if (const ui::ItemMenu* m = cx.items->menu())
			{
				for (size_t i = 0; i < m->items.size(); i++) { printf("windows: menu row %zu \"%s\" [%s]%s\n", i, m->items[i].text.c_str(), m->bindings[i].c_str(), m->items[i].enabled ? "" : " disabled"); }
			}
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "machine")	// dump machine: the focus panel's producer, its wire fields, its BUILDING row and its panel's lines (k16.19.7.1, k16.19.8.2; stdout)
	{
		const simlink::Interactable* t = cx.panelProducer ? sim.thing(cx.panelProducer) : nullptr;
		if (!t || t->kind != simlink::Interactable::kProducer) { printf("machine: no producer panel; the window %llu is %s\n", (unsigned long long)cx.machine, cx.machine ? "open" : "closed"); }
		else
		{
			printf("machine %llu \"%s\" record \"%s\": state %u, output \"%s\" %u/%u, operators %u/%u, rate %.4f/s\n", (unsigned long long)t->id, sim.nameOf(t->record, chars).c_str(),
				sim.decoder().str(t->record).c_str(), t->state, sim.nameOf(t->output, chars).c_str(),
				t->count, t->cap, t->operators, t->maxOperators, t->rate);
			if (const simlink::SimLinkDecoder::Building* row = sim.focusBuilding())
			{
				printf("  row %llu owner %llu status %u workers %u/%u efficiency %.2f maxEfficiency %.2f hp %.1f/%.1f quality %.2f version %u\n", (unsigned long long)row->id,
					(unsigned long long)row->owner, (unsigned)row->status, (unsigned)row->workers, (unsigned)row->maxWorkers, (double)row->efficiency,
					(double)row->maxEfficiency, (double)row->hp, (double)row->maxHp, (double)row->quality, (unsigned)row->version);
			}
			else { printf("  row: none yet\n"); }
			printf("  buildings map has the focus row: %d (%zu rows)\n", (int)(sim.decoder().buildings.count(cx.panelProducer) != 0), sim.decoder().buildings.size());
			const ui::HudBuildingInfo b = cx.machinePanel();
			printf("  panel \"%s\" faction \"%s\" status \"%s\"\n", b.name.c_str(), b.faction.c_str(), b.status.c_str());
			for (const ui::HudLine& l : b.stats) { printf("  stat %s %s %s\n", l.label.c_str(), l.value.c_str(), l.colour.c_str()); }
			for (const ui::HudBuildingBar& bar : b.bars) { printf("  bar %s %s %d%%\n", bar.label.c_str(), bar.value.c_str(), (int)(bar.fraction * 100)); }
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "trade")	// dump trade: the open TRADE frame's owned-chest prices (container, version vs its INVENTORY's, index:sell; stdout)
	{
		const simlink::SimLinkDecoder::Trade& trade = sim.decoder().trade;
		printf("trade valid %d trader %llu money %lld chests %zu\n", trade.valid, (unsigned long long)trade.trader, (long long)trade.traderMoney, trade.chests.size());
		for (const auto& ch : trade.chests)
		{
			const simlink::Inventory* inv = sim.inventory(ch.container);
			printf("chest %llu version %u inventory %d:", (unsigned long long)ch.container, ch.version, inv ? (int)inv->version : -1);
			for (const auto& x : ch.prices) { printf(" %u:%d%s", x.index, x.sell, inv && x.index < inv->items.size() ? (" " + sim.nameOf(inv->items[x.index].item, chars)).c_str() : ""); }
			printf("\n");
		}
		const simlink::Inventory* stk = sim.inventory(trade.stock);
		printf("buyback %zu\n", trade.buyback.size());
		for (const auto& x : trade.buyback)
		{
			printf("buyback %u %s count %u price %d\n", x.index, stk && x.index < stk->items.size() ? sim.nameOf(stk->items[x.index].item, chars).c_str() : "?", x.count, x.price);
		}
		return true;
	}
	if (a.size() == 1 && a[0] == "floor")	// dump floor: the floor control's building, visible / top floor, the lead's floor (stdout)
	{
		const WorldBuildings::FloorStatus fl = buildings.floorStatus();
		printf("floor: %s shown %d/%d lead %d (%s) level %d\n", fl.valid ? fl.name.c_str() : "-", fl.shown, fl.top, fl.leadFloor,
			fl.leadFloor < 0 ? "no lead" : fl.following ? "following" : "not following", buildings.level());
		return true;
	}
	if (a.size() == 4 && a[0] == "floorables")	// dump floorables X Z R: the floorable buildings near a point (kenshi-k16.2.355; stdout)
	{
		printf("%s", buildings.floorablesReport(atof(a[1].c_str()), atof(a[2].c_str()), atof(a[3].c_str())).c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "cuts")	// dump cuts: every building the floor cut slices, with the floor shown of each (kenshi-k16.2.332.1; stdout)
	{
		printf("%s", buildings.cutsReport().c_str());
		return true;
	}
	if (a.size() == 1 && a[0] == "occluders")	// dump occluders: the buildings between the camera and the seen (else the first selected) character's chest (stdout)
	{
		WorldBuildings& wb = buildings;
		float hull[3];
		const bool had = wb.hasSeen;
		const bool hadIndoors = wb.seenIndoors;
		const uint64_t hadBuilding = wb.seenBuilding;
		if (!had && !sim.selected().empty())
		{
			const uint64_t who = sim.selected().front();
			wb.hasSeen = sim.placed(who, wb.seen, hull);
			const auto ch = sim.decoder().chars.find(who);
			wb.seenIndoors = ch != sim.decoder().chars.end() && (ch->second.cur.flags & simlink::kIndoors);
			wb.seenBuilding = ch != sim.decoder().chars.end() ? ch->second.cur.building : 0;
		}
		printf("occluders from %.1f %.1f %.1f to %.1f %.1f %.1f:\n%s", cx.pos[0], cx.pos[1], cx.pos[2], wb.seen[0], wb.seen[1], wb.seen[2], wb.occluderReport(cx.pos).c_str());
		wb.hasSeen = had;
		wb.seenIndoors = hadIndoors;
		wb.seenBuilding = hadBuilding;
		return true;
	}
	if (a.size() == 1 && a[0] == "peek")	// dump peek: the cursor ID pass's last result: the building drawn under the cursor, that point, its floor (stdout)
	{
		const IdBuffer::Hit& h = framestats::stats().cursorHit;
		const int b = h.building;
		printf("peek at (%.0f, %.0f): %s at %.1f %.1f %.1f floor %d/%d below %d sliced %d on %d\n", cx.mouseX, cx.mouseY, !h.valid ? "(no result)" : b >= 0 ? buildings.describe((uint32_t)b).c_str() : "-",
			h.at[0], h.at[1], h.at[2], b >= 0 ? buildings.floorAt(b, h.at) : -1, buildings.topFloor(b), (int)(b >= 0 && buildings.belowFloors(b, h.at)),
			(int)(b >= 0 && buildings.sliced(b)), (int)(b >= 0 && buildings.peeks.count(b) && buildings.peeks[b].on));
		return true;
	}
	// kenshi-k16.2.153.21 (pass 3): the sky / weather / gpu / effects dumps, moved from main.cpp unchanged.
	if (a.size() == 1 && a[0] == "sky")	// dump sky: this frame's light_params terms, exposure and HDR state (stdout)
	{
		// The terms match the live SharedParams / SkyX names in kenshi-inspect findings/world_particles.md 6, so a frame can be compared
		// term by term with the original's dump. sun_col is already sunColour.rgb x alpha (lighting.inc light_params).
		// Hdr::read is a GPU read: a readback request served after this frame's last pass, and the dump prints from its done (logic, at the next
		// frame start, before logic changes the sky; kenshi-k16.2.205.12.27). cx is main.cpp's long-lived context, not a caller's local.
		Hdr* hdr = &cx.hdr;
		const Context* c = &cx;
		readback::sample([hdr] {
			float lum = 0, adapted = 0;
			hdr->read(&lum, &adapted);
			return std::vector<float>{ lum, adapted };
		}, [c](bool ok, const std::vector<float>& v) {
		const Context& cx = *c;
		if (ok) { cx.hdrSceneLum = v[0]; cx.hdrAdapted = v[1]; }
		Sky& sky = cx.sky;
		const LightParams& L = sky.light();
		const float alpha = L.v[1][3];
		printf("sky: time %.2f day %d sun altitude %.1f deg\n", sky.clock.hours, sky.clock.day, std::asin(std::fmin(1.0, std::fmax(-1.0, sky.sunDir()[1]))) * 180 / 3.14159265358979);
		printf("  sunDirection  %.4f %.4f %.4f   real %.4f %.4f %.4f\n", L.v[0][0], L.v[0][1], L.v[0][2], L.v[9][0], L.v[9][1], L.v[9][2]);
		printf("  sunColour     %.4f %.4f %.4f   intensity %.4f\n", alpha > 0 ? L.v[1][0] / alpha : 0.0f, alpha > 0 ? L.v[1][1] / alpha : 0.0f,
			alpha > 0 ? L.v[1][2] / alpha : 0.0f, alpha);
		printf("  zenithLight   %.4f %.4f %.4f   envColour.w %.3f\n", L.v[2][0], L.v[2][1], L.v[2][2], L.v[2][3]);
		printf("  nadirLight    %.4f %.4f %.4f   ambientParams.w %.3f\n", L.v[3][0], L.v[3][1], L.v[3][2], L.v[3][3]);
		printf("  ambientMult   %.4f %.4f %.4f\n", L.v[7][0], L.v[7][1], L.v[7][2]);
		printf("  horizonClouds %.4f %.4f %.4f   weight %.3f\n", L.v[10][0], L.v[10][1], L.v[10][2], L.v[9][3]);
		printf("  fogColour     %.4f %.4f %.4f   density %.6f   glob %.3f %.3f %.3f %.4f\n", L.v[4][0], L.v[4][1], L.v[4][2], L.v[5][3],
			L.v[6][0], L.v[6][1], L.v[6][2], L.v[6][3]);
		printf("  exposure %.4f (key %.2f) scene luminance %.4f adapted %.4f MIN_LUMINANCE %.3f MAX_LUMINANCE %.3f\n", sky.exposure(), 0.55,
			cx.hdrSceneLum, cx.hdrAdapted, sky.minLum(), sky.maxLum());
		const SkyWeather& w = sky.weather;
		printf("  clouds %.4f  colourMult %.4f %.4f %.4f  group '%s' (%s)\n", w.cloudsDensity, w.skyColourMult[0], w.skyColourMult[1], w.skyColourMult[2],
			cx.wx.ready() ? cx.wx.atlas().groupAt(cx.pos[0], cx.pos[2]).c_str() : "", sky.regionalFog ? (cx.skyRamp.valid ? "regional, sky ramp" : "regional") : "pinned");
		});
		return true;
	}
	if (a.size() == 1 && a[0] == "weather")	// dump weather: the active weather's effects, their anchoring and spawn recipe (stdout)
	{
		static const char* const kAnchor[] = { "world", "camera", "player" };
		printf("weather %s: %zu effects\n", cx.sky.weather.name.c_str(), cx.sky.weather.effects.size());
		if (cx.sky.weather.windValid)
			printf("  wind %.2f u/s yaw %.3f toward %.3f %.3f (live, at the camera)\n", cx.sky.weather.windSpeed, cx.sky.weather.windYaw,
				std::sin(cx.sky.weather.windYaw), std::cos(cx.sky.weather.windYaw));
		else
			printf("  wind %.2f u/s direction (1, 1) (no tail: the fixed gap)\n", cx.sky.weather.windSpeed);
		for (const SkyWeather::Effect& e : cx.sky.weather.effects)
			printf("  %-28s type %2d %-6s amount %.2f cap %d respawn %.0f..%.0f s ttl %.0f..%.0f strength %.2f..%.2f alt %.0f..%.0f slope %.0f..%.0f wander %.0f radius %.0f view %.0f\n",
				e.system.c_str(), e.type, kAnchor[(int)e.anchor()], e.amount, e.cap, e.delay, e.respawnMax, e.ttlMin, e.ttlMax, e.strengthMin, e.strengthMax,
				e.minAltitude, e.maxAltitude, e.minSlope, e.maxSlope, e.wanderSpeed, e.radius, e.viewDistance);
		printf("%s", cx.weatherInst.describe().c_str());	// the world instances (types 2/3/7/8/9/10): live/drawn, position, age, ttl
		return true;
	}
	if (a.size() == 1 && a[0] == "gpu")	// dump gpu: the latest GPU ms per section (GL_TIME_ELAPSED, two frames late) and the live light volumes (stdout)
	{
#if KCLIENT_PROFILE
		printf("gpu ms: total %.3f", framestats::stats().gpuTotal);
		for (int s = 0; s < GpuTimer::kSections; s++) { printf(" %s %.3f", GpuTimer::name(s), framestats::stats().gpuMs[s]); }
		const auto& lv = framestats::stats().lightVols;	// the render thread's counts (it owns the list), as published with the last frame
		printf(" | interior_light %.2f strength %.2f volumes %zu | sun_bounce %.2f sun volumes %zu\n", cx.interiorLight.get(), 1.0 - cx.interiorLight.get(), lv.volumes, cx.sunBounce.get(), lv.sunVolumes);	// the stored value (1 = the effect off) and the strength the scene gets (kenshi-jj3.29.2)
#else
		fprintf(stderr, "kclient: script: dump gpu: built without KCLIENT_PROFILE (cmake -DKCLIENT_PROFILE=ON)\n");	// the timer has no methods at all then (gputimer.hpp)
		cx.scriptError(cx.frames, "dump");
#endif
		return true;
	}
	if (!a.empty() && a[0] == "effects")	// dump effects [SCRIPT...]: visible placed EFFECT instances (script, world pos, owner, floor), then each distinct script's parse line (plus any named ones; stdout)
	{
		const std::vector<WorldBuildings::PlacedEffectRt>& fx = cx.buildings.placedEffects();
		printf("effects: %zu in view, %zu effect defs, %zu building-record lights (not drawn)\n", fx.size(), cx.buildings.effectDefs(), cx.buildings.ownLightDefs());
		std::map<std::string, int> byScript;
		for (const WorldBuildings::PlacedEffectRt& e : fx)
		{
			const std::string& script = cx.buildings.effectScript(e.def);
			byScript[script]++;
			printf("effect %-28s world %.2f %.2f %.2f part %d floor %d dist %.0f seed %u owner %s\n", script.c_str(), e.world[0], e.world[1], e.world[2],
				e.part, e.floor, e.distance, e.seed, cx.buildings.describe(e.instance).c_str());
		}
		for (const auto& [script, n] : byScript) { printf("effect script %-28s %d in view\n", script.c_str(), n); }
		if (cx.particlesOn)
		{
			const std::string st = cx.particleWorld.describe();
			printf("particleworld: %zu systems\n%s", cx.particleWorld.systems().size(), st.c_str());
		}
		if (cx.particlesOn)
		{
			std::vector<std::string> scripts = cx.buildings.effectScripts();
			scripts.insert(scripts.end(), a.begin() + 1, a.end());
			for (const std::string& script : scripts) { printf("parse %s\n", cx.particles.inspectScript(script).c_str()); }
		}
		printf("effects total: %zu in view, %zu scripts distinct, %zu effect defs\n", fx.size(), cx.buildings.effectScripts().size(), cx.buildings.effectDefs());
		return true;
	}
	return false;
}

// The `debug weather|weff|time|nudge` script ops (moved from main.cpp, kenshi-k16.2.153.21; main.cpp calls it only under KCLIENT_DEBUG_VIEW, so the
// ops are absent with the debug view off, as they were). False: not one of these.
bool debugOp(const Context& cx, const std::vector<std::string>& a)
{
#ifdef KCLIENT_DEBUG_VIEW
	if (a.size() == 2 && a[0] == "command") { debugCommand(cx, a[1]); return true; }	// debug command ID: a debug panel button (debug.spawn_bandits)
	if (a.size() == 2 && (a[0] == "squad" || a[0] == "char"))	// debug squad|char ID: DEBUG_QUERY and the tree window (ID "first": the first debug squad, "mine": the status panel's character for char (k16.2.99), else the debug squad holding the first selected)
	{
		uint64_t id = a[1] == "first" && !cx.debugView.squads.empty() ? cx.debugView.squads.begin()->first : strtoull(a[1].c_str(), nullptr, 10);
		if (a[1] == "mine" && a[0] == "char") { id = cx.infoChar; }	// the inspect / status panel's character
		else if (a[1] == "mine" && !cx.sim.selected().empty())	// mine: the debug squad holding the first selected character
		{
			for (const auto& [sid, sq] : cx.debugView.squads) { if (std::find(sq.memberIds.begin(), sq.memberIds.end(), cx.sim.selected()[0]) != sq.memberIds.end()) { id = sid; } }
		}
		cx.debugTreeOpen(a[0] == "squad" ? 0 : 1, id);	// the window too
		printf("debug query %s %llu\n", a[0].c_str(), (unsigned long long)id);
		return true;
	}
#endif
	if (a.size() >= 2 && a[0] == "weather")	// debug weather NAME: the panel's weather row (auto: normal weather; names may hold spaces)
	{
		std::string name = a[1];
		for (size_t i = 2; i < a.size(); i++) { name += " " + a[i]; }
		debugWeather(cx, name);
		return true;
	}
	if (a.size() >= 4 && a[0] == "weff")	// debug weff SCRIPT DX DZ: pin that particle system at the pivot + (DX, DZ), terrain y (a dev A/B aid, k16.2.11.3)
	{
		Weff w;
		w.script = a[1];
		w.off[0] = atof(a[2].c_str());
		w.off[1] = atof(a[3].c_str());
		cx.weff.push_back(w);
		printf("weff: %s pinned at the pivot + %.0f,%.0f\n", w.script.c_str(), w.off[0], w.off[1]);
		return true;
	}
	if (a.size() >= 4 && a[0] == "nudge")	// debug nudge DX DZ NAME...: shift the named characters' drawn position (a forced catch-up, past 400 u a teleport: the events log)
	{
		std::string want;
		for (size_t k = 3; k < a.size(); k++) { want += (k > 3 ? " " : "") + a[k]; }
		int n = 0;
		for (const auto& [id, ch] : cx.sim.decoder().chars)
		{
			if (cx.sim.decoder().str(ch.look.name).find(want) != std::string::npos && cx.sim.nudge(id, atof(a[1].c_str()), atof(a[2].c_str()))) { n++; }
		}
		printf("nudge: %d characters named \"%s\" shifted %s,%s\n", n, want.c_str(), a[1].c_str(), a[2].c_str());
		return true;
	}
	if (a.size() == 2 && a[0] == "time")	// debug time PRESET: dawn, noon, dusk, midnight, +1h
	{
		debugTime(cx, a[1]);
		return true;
	}
	return false;
}

// The `machine id=N` script op (moved from main.cpp, kenshi-k16.2.153.19): N's machine window opens, its focus panel too, as a left click on the producer.
bool machineOp(const Context& cx, const std::vector<std::string>& a)
{
	if (!(a.size() == 1 && a[0].starts_with("id="))) { return false; }
	cx.machine = strtoull(a[0].c_str() + 3, nullptr, 10);
	cx.panelProducer = cx.machine;	// as a left click on the producer: the panel follows it too (k16.19.8.2)
	printf("machine: show %llu\n", (unsigned long long)cx.machine);
	return true;
}

// The physics / ragdoll / foliage / trails / sound script ops (moved from main.cpp, kenshi-k16.2.153.27). op is the script op: "dump", "physics",
// "trails" or "sound". False: not one of these forms (the caller's script error, for a malformed one).
bool physicsOp(const Context& cx, const std::string& op, const std::vector<std::string>& a)
{
	if (op == "dump" && a.size() >= 4 && a[0] == "shapes")	// dump shapes X Z R [thin]: the gathered collision shapes near (X, Z), thin ones only with "thin" (stdout; finds poles and signs for the drive camera)
	{
		cx.collide.wait();
		printf("shapes near %s %s r %s:\n%s", a[1].c_str(), a[2].c_str(), a[3].c_str(), cx.collide.report(atof(a[1].c_str()), atof(a[2].c_str()), atof(a[3].c_str()), a.size() > 4 && a[4] == "thin").c_str());
		return true;
	}
	if (op == "dump" && a.size() == 1 && a[0] == "ragdolls") { printf("%s", cx.ragdolls.report().c_str()); return true; }	// dump ragdolls: the live EVENT 48 ragdolls (stdout)
	if (op == "dump" && a.size() == 1 && a[0] == "physics")	// dump physics: the client physics world (precision, bodies, steps) and each body's state (stdout)
	{
		printf("%s", cx.physics.report().c_str());
		printf("physics colliders: %zu posted\n", cx.statics.posted());
		return true;
	}
	if (op == "dump" && a.size() == 2 && a[0] == "ragdoll")	// dump ragdoll FILE: a data/ragdoll/*.phs read (bodies, joints, limits) and each body's bone in the male skeleton (stdout)
	{
		ragdoll::Def def;
		std::string err;
		if (!ragdoll::load(cx.dataDir + "/ragdoll/" + a[1], def, err)) { printf("dump ragdoll: %s\n", err.c_str()); }
		else
		{
			printf("ragdoll %s: %s\n", a[1].c_str(), ragdoll::summary(def).c_str());
			try
			{
				const kworld::Skeleton skel = kworld::loadSkeleton(cx.dataDir + "/character/meshes/male_skeleton/male_skeleton.skeleton");
				const std::vector<int> bones = ragdoll::boneMap(def, skel);
				for (size_t b = 0; b < def.bodies.size(); b++)
				{
					printf("ragdoll body %zu %-18s bone %d %s\n", b, def.bodies[b].bone.c_str(), bones[b], bones[b] < 0 ? "MISSING" : "");
					for (const ragdoll::Part& p : def.bodies[b].parts)
					{
						printf("  part type %d dims %.3f %.3f %.3f pos %.3f %.3f %.3f rot %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f\n", p.type,
							p.dims[0], p.dims[1], p.dims[2], p.pos[0], p.pos[1], p.pos[2],
							p.rot[0], p.rot[1], p.rot[2], p.rot[3], p.rot[4], p.rot[5], p.rot[6], p.rot[7], p.rot[8]);
					}
				}
			}
			catch (const std::exception& e) { printf("dump ragdoll: skeleton: %s\n", e.what()); }
			for (size_t j = 0; j < def.joints.size(); j++)
			{
				const ragdoll::Joint& jt = def.joints[j];
				printf("ragdoll joint %zu type %u parent %d child %d limits %.0f %.0f %.0f %s\n", j, jt.type, jt.parent, jt.child, jt.limits[0], jt.limits[1], jt.limits[2],
					jt.limitsEnabled ? "on" : "off");
			}
		}
		return true;
	}
	if (op == "dump" && a.size() == 4 && a[0] == "foliage")	// dump foliage X Z R: the foliage within R of (X, Z): record, position, scale, each loaded mesh's world box (stdout)
	{
		const double at[3] = { std::stod(a[1]), 0, std::stod(a[2]) };
		printf("%s", cx.foliage.nearReport(at, std::stod(a[3])).c_str());
		return true;
	}
	if (op == "dump" && a.size() == 1 && a[0] == "trails") { printf("%s\n", cx.trails.status().c_str()); return true; }	// dump trails: the trails' on/off, filter, window, characters and points (stdout)
	if (op == "physics" && a.size() == 7 && a[0] == "box")	// physics box X Y Z HX HY HZ: a dynamic box (half extents), dropped at the point; prints the terrain height there (P1 smoke)
	{
		const double at[3] = { std::stod(a[1]), std::stod(a[2]), std::stod(a[3]) };
		const float half[3] = { (float)std::stod(a[4]), (float)std::stod(a[5]), (float)std::stod(a[6]) };
		const uint32_t id = cx.physics.addBox(at, half);
		printf("physics box %u at %.1f %.1f %.1f half %.1f %.1f %.1f, terrain under it %.2f\n", id, at[0], at[1], at[2], half[0], half[1], half[2],
			cx.hm ? cx.hm->height(at[0], at[2]) : 0.0);
		return true;
	}
	if (op == "physics" && a.size() == 4 && a[0] == "smoke")	// physics smoke X Y Z: the P0 smoke world: a flat static box with its top face at Y, a 2 u sphere 20 u above it (stdout)
	{
		const double at[3] = { std::stod(a[1]), std::stod(a[2]), std::stod(a[3]) };
		const double floor[3] = { at[0], at[1] - 1, at[2] };
		const float half[3] = { 30, 1, 30 };
		const uint32_t ground = cx.physics.addStaticBox(floor, half);
		const double ball[3] = { at[0], at[1] + 20, at[2] };
		const uint32_t sphere = cx.physics.addSphere(ball, 2);
		printf("physics smoke: ground %u top %.1f, sphere %u at %.1f %.1f %.1f\n", ground, at[1], sphere, ball[0], ball[1], ball[2]);
		return true;
	}
	if (op == "physics" && a.size() == 6 && a[0] == "ragdoll")	// physics ragdoll FILE X Y Z YAW: a data/ragdoll/FILE ragdoll (P2 smoke) started at its bind pose there (stdout)
	{
		ragdoll::Def def;
		std::string err;
		if (!ragdoll::load(cx.dataDir + "/ragdoll/" + a[1], def, err)) { printf("physics ragdoll: %s\n", err.c_str()); }
		else
		{
			const double at[3] = { std::stod(a[2]), std::stod(a[3]), std::stod(a[4]) };
			const double yaw = std::stod(a[5]);
			const uint32_t id = cx.physics.addRagdoll(def, at, (float)(yaw * 0.017453292519943295));
			printf("physics ragdoll %u (%s) at %.1f %.1f %.1f yaw %.0f, %zu bodies, terrain under it %.2f\n", id, ragdoll::summary(def).c_str(), at[0], at[1], at[2], yaw,
				cx.physics.ragdollBodies(id).size(), cx.hm ? cx.hm->height(at[0], at[2]) : 0.0);
		}
		return true;
	}
	if (op == "trails" && !a.empty())	// trails all | id=N | name NAME... | window SECONDS (0 = the cap, debug.trails_seconds) | clear | off
	{
		std::string reply;
		if (!cx.trails.op(a, reply)) { cx.scriptError(cx.frames, "trails"); }
		printf("%s\n", reply.c_str());
		return true;
	}
	// sound at R F U [ARMOR [GATE]] | sound char ID [ARMOR [GATE]]: a world sound post (audiohook_object_post; docs/AUDIO_ENGINE.md "World sounds"): the
	// Armor_Worn switch (default Cloth) set on the object, then Footsteps posted on it with the original's 200 u gate (GATE: another gate, 0 none).
	// `at`: a fresh debug object R right, F forward, U up of the camera (world units); `char`: the character's sim id, at its position.
	if (op == "sound" && a.size() >= 2 && (a[0] == "at" || a[0] == "char"))
	{
		static uint32_t debugObjects = 0;
		const size_t fixed = a[0] == "at" ? 4 : 2;
		if (a.size() < fixed) { cx.scriptError(cx.frames, "sound"); }
		uint32_t object;
		double p[3];
		if (a[0] == "at")
		{
			double f[3], r[3], u[3];
			cx.cameraBasis(f, r, u);
			const double o[3] = { std::atof(a[1].c_str()), std::atof(a[2].c_str()), std::atof(a[3].c_str()) };
			for (int k = 0; k < 3; k++) { p[k] = cx.pos[k] + r[k] * o[0] + f[k] * o[1] + u[k] * o[2]; }
			object = 0x7F000000u + ++debugObjects;
		}
		else
		{
			const uint64_t id = std::strtoull(a[1].c_str(), nullptr, 10);
			const auto ch = cx.sim.decoder().chars.find(id);
			if (ch == cx.sim.decoder().chars.end()) { fprintf(stderr, "kclient: script: no character %s\n", a[1].c_str()); cx.scriptError(cx.frames, "sound"); }
			for (int k = 0; k < 3; k++) { p[k] = ch->second.cur.pos[k]; }
			object = (uint32_t)id;
		}
		audiohook_object(object, p);
		audiohook_object_switch(object, "Armor_Worn", a.size() > fixed ? a[fixed] : "Cloth");
		audiohook_object_post(object, "Footsteps", a.size() > fixed + 1 ? (float)std::atof(a[fixed + 1].c_str()) : 200.0f);
		return true;
	}
	return false;
}

}	// namespace dev
