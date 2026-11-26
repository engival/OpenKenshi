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
#include "dev/roadedit.hpp"
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
#include "ui/diamond.hpp"
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
#include "host/orders.hpp"
#include "host/pick.hpp"
#include "host/logicframe.hpp"
#include "host/script.hpp"
#include "host/events.hpp"

namespace host
{


// The script commands that only exist for development (--debug): dump / log print the client's internals, debug drives the
// debug panel, lab the animation lab, physics the Jolt smoke worlds, playtest the playtest store's entry point. The rest of
// the language drives the game as a player would.
// A ground pick at world x z (args from i; "building ID" after them names a player's building): the control-hints cases' pointer-less target
static Pick ground_pick(const std::vector<std::string>& a, size_t i)
{
	Pick p;
	p.kind = Pick::Ground;
	p.hit[0] = strtod(a[i].c_str(), nullptr);
	p.hit[2] = strtod(a[i + 1].c_str(), nullptr);
	p.hit[1] = app.hm ? app.hm->height(p.hit[0], p.hit[2]) : 0;
	if (a.size() >= i + 4 && a[i + 2] == "building") { p.building = true; p.id = strtoull(a[i + 3].c_str(), nullptr, 10); }
	return p;
}

static bool debug_script_op(const std::string& op) { return op == "debug" || op == "dump" || op == "log" || op == "physics" || op == "lab" || op == "playtest" || op == "trails" || op == "sound" || op == "serverhold" || op == "serverfail" || op == "titlehold" || op == "hints" || op == "roads" || op == "groundclick" || op == "charclick" || op == "thingclick" || op == "menuclick" || op == "light"; }

static void script_needs_debug(const Cmd& c)
{
	fprintf(stderr, "kclient: script command at frame %d needs --debug: %s", c.frame, c.op.c_str());
	for (const std::string& a : c.args) { fprintf(stderr, " %s", a.c_str()); }
	fprintf(stderr, "\n");
	fflush(stdout);
	exit_now(2);	// as script_error
}

// The `stall MS gate|b` op's pending sleeps (logic thread only): [0] just before the world frame's gate, [1] just after it; taken once.
static double gStallAt[2] = { 0, 0 };
void stall_point(int at)
{
	if (gStallAt[at] <= 0) { return; }
	const double ms = gStallAt[at];
	gStallAt[at] = 0;
	std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

void exec_cmd(const Cmd& c)
{
	// debug.overlap_scripted (review .101 C1): key, type and mouse down/up go through gInput like live input, drained at the next frame's start while the
	// previous packet is drawn, so TSan sees the handlers beside a draw. Every other op keeps the gate.
	const bool queued = logicthread::mode() == logicthread::Mode::kOverlap && logicthread::running()
		&& (c.op == "key" || c.op == "type" || (c.op == "mouse" && c.args.size() == 2 && (c.args[0] == "down" || c.args[0] == "up")));
	const auto deliver = [&](const sapp_event& ev) { if (queued) { gInput.push(ev); } else { event(&ev); } };
	if (c.op != "stall" && c.op != "renderstall" && !queued) { logicthread::renderIdle(); }	// the two stall ops only arm a sleep (kenshi-k16.2.205.12.118: a gate here would stop the redraw they test)
	// overlap (rule 12): an op may poke any renderer (partdepth, wind, the lab); scripted overlap and the console gate here
	if (!app.opt.debug && debug_script_op(c.op)) { script_needs_debug(c); }
	sapp_event e = {};
	e.frame_count = (uint64_t)app.frames;
	// modifiers from the scripted keys held, as sokol fills them for real input (not app.keys: a focused edit box takes a Ctrl's press)
	static uint32_t held = 0;
	e.modifiers = held;
	const auto& a = c.args;
	if (c.op == "key" && a.size() == 2)
	{
		const uint32_t bit = ui::keys::modBit(key_by_name(a[0]));
		held = a[1] == "down" ? held | bit : held & ~bit;
	}
	if (c.op == "key" && a.size() == 2)
	{
		e.key_code = key_by_name(a[0]);
		e.type = a[1] == "down" ? SAPP_EVENTTYPE_KEY_DOWN : SAPP_EVENTTYPE_KEY_UP;
		if (e.key_code == SAPP_KEYCODE_INVALID || (a[1] != "down" && a[1] != "up")) { script_error(c); }
		deliver(e);
	}
	else if (c.op == "type" && !a.empty())	// type TEXT...: one CHAR event per character (an edit box's typing)
	{
		std::string text;
		for (const std::string& w : a) { text += (text.empty() ? "" : " ") + w; }
		e.type = SAPP_EVENTTYPE_CHAR;
		for (unsigned char ch : text)
		{
			e.char_code = ch;
			deliver(e);
		}
	}
	else if (c.op == "mouse" && a.size() == 2 && (a[0] == "down" || a[0] == "up"))	// mouse down|up left|right|middle
	{
		e.type = a[0] == "down" ? SAPP_EVENTTYPE_MOUSE_DOWN : SAPP_EVENTTYPE_MOUSE_UP;
		e.mouse_button = a[1] == "left" ? SAPP_MOUSEBUTTON_LEFT : a[1] == "middle" ? SAPP_MOUSEBUTTON_MIDDLE : SAPP_MOUSEBUTTON_RIGHT;
		e.mouse_x = (float)app.mouseX;
		e.mouse_y = (float)app.mouseY;
		deliver(e);
	}
	else if (c.op == "mouse" && a.size() >= 2 && a[0] == "label")	// mouse label TEXT...: to the centre of the shown item / container label with that text
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (i > 1 ? " " : "") + a[i]; }
		const std::vector<ItemLabel> labels = item_labels(labels_shown());
		const auto l = std::find_if(labels.begin(), labels.end(), [&](const ItemLabel& l) { return l.text == text; });
		if (l == labels.end()) { fprintf(stderr, "kclient: script: no label \"%s\" shown\n", text.c_str()); script_error(c); }
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = l->x + l->w * 0.5f;
		e.mouse_y = l->y + l->h * 0.5f;
		event(&e);
	}
	else if (c.op == "mouse" && a.size() == 2 && a[0] == "itemmenu")	// mouse itemmenu N: onto row N of the open item menu (0 is its title row; k16.20.13.3)
	{
		const ui::ItemMenu* m = app.items ? app.items->menu() : nullptr;
		const size_t n = (size_t)atoi(a[1].c_str());
		if (!m || n >= m->items.size()) { fprintf(stderr, "kclient: script: no item menu row\n"); script_error(c); }
		else
		{
			const ui::Rect r = m->row(n);
			e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
			e.mouse_x = r.x + r.w * 0.5f;
			e.mouse_y = r.y + r.h * 0.5f;
			event(&e);
		}
	}
	else if (c.op == "mouse" && a.size() >= 3 && a[0] == "gear")	// mouse gear [OWNER] grid NAME CX CY | arrange NAME | button bag|limbs | mark | pin: onto a cell centre / button of an item window (OWNER: its id, default the character window; NAME: main bag container weapon1 weapon2 belt shirt head pants armour boots backpack)
	{
		static const char* const names[] = { "backpack", "weapon1", "weapon2", "belt", "shirt", "head", "pants", "armour", "boots", "main", "bag", "armr", "arml", "legr", "legl", "container" };
		const auto grid = [&](const std::string& n) { for (int g = 0; g < ui::kGearGrids; g++) { if (n == names[g]) { return g; } } return -1; };
		std::vector<std::string> b(a.begin() + 1, a.end());
		uint64_t owner = app.gearChar;
		if (!b.empty() && std::all_of(b[0].begin(), b[0].end(), [](unsigned char ch) { return std::isdigit(ch); })) { owner = strtoull(b[0].c_str(), nullptr, 10); b.erase(b.begin()); }
		ui::Gear* gear = app.items ? app.items->character(owner) : nullptr;
		ui::ContainerWindow* box = app.items ? app.items->container(owner) : nullptr;
		const ui::ItemGrid* g = b.size() == 4 && grid(b[1]) >= 0 ? (gear ? gear->grid(grid(b[1])) : box && grid(b[1]) == ui::kGearContainer ? &box->grid() : nullptr) : nullptr;
		const ui::Widget* w = nullptr;
		ui::Rect r = {};
		if (!b.empty() && b[0] == "grid" && g)
		{
			r = { g->screen().x + ((float)atof(b[2].c_str()) + 0.5f) * g->cell * ui::scale(), g->screen().y + ((float)atof(b[3].c_str()) + 0.5f) * g->cell * ui::scale(), 0, 0 };
		}
		else if (b.size() == 2 && b[0] == "arrange" && grid(b[1]) >= 0) { w = gear ? gear->arrangeButton(grid(b[1])) : box ? box->arrangeButton() : nullptr; }
		else if (b.size() == 2 && b[0] == "button" && (b[1] == "bag" || b[1] == "limbs") && gear) { w = b[1] == "bag" ? gear->bagButton() : gear->limbsButton(); }
		else if (b.size() == 1 && b[0] == "mark" && app.items && app.items->window(owner)) { w = &app.items->window(owner)->targetButton(); }
		else if (b.size() == 1 && b[0] == "pin" && app.items && app.items->window(owner)) { w = &app.items->window(owner)->pinButton(); }
		if (w) { r = w->screen(); r.x += r.w * 0.5f; r.y += r.h * 0.5f; }
		else if (b.empty() || b[0] != "grid" || !g) { fprintf(stderr, "kclient: script: no gear target\n"); script_error(c); }
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = r.x;
		e.mouse_y = r.y;
		event(&e);
	}
	else if (c.op == "mouse" && a.size() == 4 && a[0] == "machine" && a[1] == "grid" && app.machineWin && app.machineWin->grid(ui::kBuildingOut))	// mouse machine grid CX CY: onto a cell of the open machine window's output slot (a drag's target, k16.19.7.1)
	{
		const ui::ItemGrid* g = app.machineWin->grid(ui::kBuildingOut);
		const ui::Rect r = { g->screen().x + ((float)atof(a[2].c_str()) + 0.5f) * g->cell * ui::scale(), g->screen().y + ((float)atof(a[3].c_str()) + 0.5f) * g->cell * ui::scale(), 0, 0 };
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = r.x;
		e.mouse_y = r.y;
		event(&e);
	}
	else if (c.op == "mouse" && a.size() >= 2 && a[0] == "squads")	// mouse squads add | dismiss | member NAME | strip N | row N | delete N | name N (N: the squad's index in PLAYER_SQUADS order; strip: its right end)
	{
		const auto& sq = app.sim.decoder().playerSquads.squads;
		const auto squad = [&]() -> uint64_t { const size_t n = a.size() == 3 ? (size_t)atoi(a[2].c_str()) : sq.size(); return n < sq.size() ? sq[n].id : 0; };
		const ui::Widget* w = nullptr;
		if (a[1] == "add") { w = app.squadsUi->addButton(); }
		else if (a[1] == "dismiss") { w = app.squadsUi->dismissZone(); }
		else if (a[1] == "strip" && squad()) { w = app.squadsUi->strip(squad()); }
		else if (a[1] == "delete" && squad()) { w = app.squadsUi->deleteButton(squad()); }
		else if (a[1] == "row" && squad()) { w = app.squadsUi->block(squad()); }
		else if (a[1] == "name" && squad()) { w = app.squadsUi->nameBox(squad()); }
		else if (a[1] == "member" && a.size() == 3)
		{
			for (const auto& q : sq)
			{
				for (uint64_t id : q.members)
				{
					const auto ch = app.sim.decoder().chars.find(id);
					if (ch != app.sim.decoder().chars.end() && app.sim.decoder().str(ch->second.look.name) == a[2]) { w = app.squadsUi->member(id); }
				}
			}
		}
		if (!w) { fprintf(stderr, "kclient: script: no squads target\n"); script_error(c); }
		ui::Rect r = w->screen();
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = a[1] == "strip" ? r.x + r.w - 10 : r.x + r.w * 0.5f;	// a strip: its empty right end (a drop there appends)
		e.mouse_y = r.y + r.h * 0.5f;
		event(&e);
	}
	else if (c.op == "mouse" && a.size() >= 2 && a[0] == "hud")	// mouse hud pause | speed N (1..3) | portrait N | floor | movespeed | build | minimap: onto the centre of that HUD control (then `mouse down|up left` clicks it; kenshi-cym.40.2.10)
	{
		const ui::Widget* w = nullptr;
		const int n = a.size() == 3 ? atoi(a[2].c_str()) : 0;
		if (a[1] == "pause" && app.hud) { w = app.hud->speedButton(0); }
		else if (a[1] == "speed" && app.hud) { w = app.hud->speedButton(n); }
		else if (a[1] == "portrait" && app.hud) { w = app.hud->portrait((size_t)n); }
		else if (a[1] == "shortcut" && app.hud) { w = app.hud->shortcutButton(n); }	// 0 INV .. 5 (Hud::kShortcuts order)
		else if (a[1] == "floor" && app.hud) { w = app.hud->floorDiamond(); }
		else if (a[1] == "movespeed" && app.hud) { w = app.hud->moveSpeedDiamond(); }
		else if (a[1] == "build" && app.hud) { w = app.hud->buildButton(); }
		else if (a[1] == "minimap" && app.minimap) { w = app.minimap->window(); }
		if (!w) { fprintf(stderr, "kclient: script: no hud target\n"); script_error(c); }
		const ui::Rect r = w->screen();
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = r.x + r.w * 0.5f;
		e.mouse_y = r.y + r.h * 0.5f;
		event(&e);
	}
	else if (c.op == "camera" && a.size() == 7 && a[0] == "view")	// camera view X Y Z YAW PITCH DIST: the free camera DIST (world units) back from the point X,Y,Z along its look direction (yaw 0 looks toward -z; pitch in degrees, negative looks down), so the point lands mid-screen (kenshi-wsbz)
	{
		const double yaw = atof(a[4].c_str()) * 3.14159265358979 / 180.0, pitch = atof(a[5].c_str()) * 3.14159265358979 / 180.0, d = atof(a[6].c_str());
		free_camera();
		app.yaw = yaw;
		app.pitch = pitch;
		app.pos[0] = atof(a[1].c_str()) - std::sin(yaw) * std::cos(pitch) * d;
		app.pos[1] = atof(a[2].c_str()) - std::sin(pitch) * d;
		app.pos[2] = atof(a[3].c_str()) + std::cos(yaw) * std::cos(pitch) * d;
	}
	else if (c.op == "camera" && a.size() >= 2 && a[0] == "thing")	// camera thing NAME... | id=N: 45 u south of and 35 u above the named interactable (or that id), looking down at it (it lands mid-screen)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (i > 1 ? " " : "") + a[i]; }
		const auto& things = app.sim.things();
		const uint64_t wantId = text.starts_with("id=") ? std::strtoull(text.c_str() + 3, nullptr, 10) : 0;
		const auto t = std::find_if(things.begin(), things.end(), [&](const auto& kv) { return wantId ? kv.first == wantId : app.sim.nameOf(kv.second.record, app.chars) == text; });
		if (t == things.end())
		{
			fprintf(stderr, "kclient: script: no interactable \"%s\"; in view:\n", text.c_str());
			for (const auto& [id, th] : things) { fprintf(stderr, "  kind %d state %d \"%s\" at %.0f %.0f\n", th.kind, th.state, app.sim.nameOf(th.record, app.chars).c_str(), th.pos[0], th.pos[2]); }
			script_error(c);
		}
		free_camera();
		app.pos[0] = t->second.pos[0];
		app.pos[1] = t->second.pos[1] + 35;
		app.pos[2] = t->second.pos[2] + 45;
		app.yaw = 0;
		app.pitch = std::atan2(-35.0, 45.0);	// radians
	}
	else if (c.op == "light")	// light <name>|all on|off|solo: the light debugger's switches (knobs/lightterms.hpp; docs/CLIENT.md "Light terms")
	{
		std::string out;
		if (dev::lightOp(a, out)) { printf("%s\n", out.c_str()); } else { script_error(c); }
	}
	else if (c.op == "dump" && !a.empty() && a[0] == "light") { fputs(dev::lightDump().c_str(), stdout); }	// dump light: every term and lamp, on / off
	else if ((c.op == "dump" || c.op == "physics" || c.op == "trails" || c.op == "sound") && dev::physicsOp(dev_context(), c.op, a)) {}	// the physics / ragdoll / foliage / trails / sound ops (dev/debughost.cpp): dump shapes ragdolls physics ragdoll foliage trails, physics box smoke ragdoll, trails ..., sound at char
	else if (c.op == "dump" && dev::dumpOp(dev_context(), a)) {}	// the dump ops (dev/debughost.cpp): stats doors things clock chars wire events players beams, then building playerbuildings light_bake light_slice floor occluders peek windows machine trade, sky weather gpu effects
#ifdef KCLIENT_DEBUG_VIEW
	else if (c.op == "debug" && dev::debugOp(dev_context(), a)) {}	// the debug ops (dev/debughost.cpp): weather NAME, weff SCRIPT DX DZ, time PRESET
#endif
	else if (c.op == "reply" && a.size() == 1)	// reply N: order 88, option N of the newest open reply menu (event 43)
	{
		const auto& menus = app.sim.decoder().menus;
		if (menus.empty()) { printf("reply: no open menu\n"); }
		else { app.sim.reply(menus.back(), (uint8_t)std::stoi(a[0])); }
	}
	else if (c.op == "end_talk" && a.empty())	// end_talk: order 89 on the newest open reply menu
	{
		const auto& menus = app.sim.decoder().menus;
		if (menus.empty()) { printf("end_talk: no open menu\n"); }
		else { app.sim.endTalk(menus.back()); }
	}
	else if ((c.op == "dump" || c.op == "log") && dev::charsOp(dev_context(), c.op, a)) {}	// the chars / anim ops (dev/charops.cpp): dump contacts feet bones anim motion idle bars, log feet hits
	else if (c.op == "dump" && !a.empty() && a[0] == "feetfloor" && (a.size() == 1 || (a.size() == 2 && (a[1] == "all" || a[1].starts_with("id=")))))
	{
		// dump feetfloor [all|id=N]: the three heights at a standing character's xz, side by side (kenshi-k16.17.7.24.4): what the client's floor ray
		// hits (the collision shape's name and y), the sim's y and our drawn soles, and the top of the drawn render mesh under the xz (the part and mesh
		// it is). No building the point stands in (open terrain): the drawn floor report says so. Text only (stdout).
		const uint64_t onlyId = a.size() == 2 && a[1].starts_with("id=") ? strtoull(a[1].c_str() + 3, nullptr, 10) : 0;
		const bool all = a.size() == 2 && a[1] == "all";
		for (const auto& [id, ch] : app.sim.decoder().chars)
		{
			if (onlyId ? id != onlyId : !all && !(ch.cur.flags & simlink::kPlayer)) { continue; }
			const int index = app.sim.charIndex(id);
			double feet[3];
			float hull[3];
			if (index < 0 || !app.sim.placed(id, feet, hull)) { continue; }
			double s[2][3];
			const bool fit = app.chars.soles((size_t)index, s);
			printf("feetfloor id %llu flags 0x%x wire building %llu floor %d sim y %.3f drawn body y %.3f terrain %.3f soles %s", (unsigned long long)id, ch.cur.flags,
				(unsigned long long)ch.cur.building, (int)ch.cur.floor, feet[1], (double)app.chars.live((size_t)index).pos[1],
				app.hm->terrainHeight(feet[0], feet[2]), fit ? "" : "(none)");
			if (fit) { printf(" L %.3f R %.3f (lowest sole vertex per foot)", s[0][1], s[1][1]); }
			printf("\n");
			// The same ray the foot log takes, with the hit's identity: from the sim's feet +1 down 6 u
			const double from[3] = { feet[0], feet[1] + 1.0, feet[2] }, dir[3] = { 0, -1, 0 };
			double t = 0;
			float bounds[6] = {};
			bool solid = false, building = false;
			std::string shape;
			if (app.collide.ray(from, dir, 6.0, t, bounds, {}, nullptr, &solid, &building, &shape))
			{
				printf("  collision ray: y %.3f at %.2f %.2f \"%s\" solid %d building %d bounds x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", from[1] - t, feet[0],
					feet[2], shape.c_str(), (int)solid, (int)building, bounds[0], bounds[3], bounds[1], bounds[4], bounds[2], bounds[5]);
			}
			else { printf("  collision ray: no hit within 6 u\n"); }
			{
				// the drawn render triangles under the feet, the first hit down from the wire y + 1.5 (kenshi-k16.2.344): what the body would stand on by faces
				const double o2[3] = { feet[0], feet[1] + 1.5, feet[2] }, lo[3] = { feet[0] - 1, feet[1] - 8, feet[2] - 1 }, hi[3] = { feet[0] + 1, feet[1] + 3, feet[2] + 1 };
				double td = 0;
				if (app.buildings.drawnHitIn(o2, dir, lo, hi, 0, 8.0, td)) { printf("  drawn ray: y %.3f (wire %.3f)\n", o2[1] - td, feet[1]); } else { printf("  drawn ray: no drawn triangle within 8 u\n"); }
			}
			printf("%s", app.buildings.drawnFloorReport(ch.cur.building, feet[0], feet[2], feet[1] + 1.0, 6.0).c_str());
		}
	}
	else if (c.op == "dump" && a.size() == 1 && a[0].starts_with("walkparts="))
	{
		printf("%s", app.buildings.walkableReport(a[0].substr(10)).c_str());	// dump walkparts=NAME: the walkable-part counts of the placed buildings named so (kenshi-k16.2.414.2)
	}
	else if (c.op == "dump" && a.size() == 2 && a[0] == "namelabel" && a[1].starts_with("id="))
	{
		// dump namelabel id=N: whether last frame drew the character's name tag, and the carrier the sim gives it (kenshi-pgvp: a carried body draws none)
		const uint64_t id = strtoull(a[1].c_str() + 3, nullptr, 10);
		const ui::Rect r = app.nameLabels->placed(id);
		printf("namelabel id %llu carrier %llu shown %d text \"%s\"\n", (unsigned long long)id, (unsigned long long)app.sim.carrier(id), r.w > 0 ? 1 : 0, app.nameLabels->text(id).c_str());
	}
	else if (c.op == "dump" && a.size() == 2 && a[0] == "cut" && a[1].starts_with("id="))
	{
		// dump cut id=N: one character's two floor-cut inputs side by side (kenshi-k16.2.264.1): the drawn feet the name tag tests (SimView::placed,
		// the pos log's `drawn`) and the Characters' own pos the body's cull tests (Characters::culled), each through cut_hidden's two steps (cutHides at
		// the feet, then the solid shape's low face its ray finds) and the drawn terrain under both. A frozen shot of a character whose tag draws and
		// body does not reads the disagreement here with its y. Text only (stdout).
		const uint64_t id = strtoull(a[1].c_str() + 3, nullptr, 10);
		const auto ch = app.sim.decoder().chars.find(id);
		const int index = app.sim.charIndex(id);
		double feet[3], body[3];
		float hull[3];
		if (ch == app.sim.decoder().chars.end() || index < 0 || !app.sim.placed(id, feet, hull)) { fprintf(stderr, "kclient: script: dump cut: no drawn character %llu\n", (unsigned long long)id); script_error(c); }
		else
		{
			app.chars.nodeWorld((size_t)index, body);
			printf("cut id %llu wire %.3f %.3f %.3f building %llu floor %d indoors %d yValid %d\n", (unsigned long long)id, ch->second.cur.pos[0], ch->second.cur.pos[1],
				ch->second.cur.pos[2], (unsigned long long)ch->second.cur.building, (int)ch->second.cur.floor, (int)((ch->second.cur.flags & simlink::kIndoors) != 0),
				(int)((ch->second.cur.flags & simlink::kYValid) != 0));
			int hidden[2] = {};
			const char* const name[2] = { "label feet (placed)", "body (Characters pos)" };
			for (int k = 0; k < 2; k++)
			{
				const double* p = k == 0 ? feet : body;
				const bool inBuilding = app.buildings.cutHides(p);
				const double up[3] = { p[0], p[1] + 2, p[2] }, down[3] = { 0, -1, 0 };
				double t = 0;
				float box[6] = {};
				bool solid = false;
				const bool hit = app.collide.ray(up, down, 8, t, box, {}, nullptr, &solid);
				const bool after = solid ? app.buildings.cutHides(p, box[1]) : true;	// cut_hidden: a solid shape counts by its low face
				hidden[k] = char_cut_hidden(id, p);
				printf("  %s %.3f %.3f %.3f terrain %.3f: cutHides %d;", name[k], p[0], p[1], p[2], app.hm->terrainHeight(p[0], p[2]), (int)inBuilding);
				if (hit) { printf(" ray top %.3f solid %d;", up[1] - t, (int)solid); } else { printf(" ray no hit within 8 u;"); }
				printf(" geometric hides %d; roof %d; %s %d\n", (int)(inBuilding && after), app.buildings.roofBuilding(p), "hidden", (int)hidden[k]);
			}
			const WorldBuildings::FloorStatus fl = app.buildings.floorStatus();
			printf("  %s (y %+.3f body to label); floor %d/%d lead %d %s; wire floor %d over the shown %d hides %d\n", hidden[0] == hidden[1] ? "agree" : "DISAGREE",
				body[1] - feet[1], fl.shown, fl.top, fl.leadFloor, fl.following ? "following" : "not following", (int)ch->second.cur.floor, fl.shown,
				(int)((int)ch->second.cur.floor > fl.shown));
		}
	}
	else if (c.op == "log" && dev::logOp(dev_context(), a)) {}	// log gait ID|players|combat|all|off, log pos ID|players|all|off: the per-frame logs (dev/scriptlog.cpp); the rest of `log` is dev::charsOp
	else if (c.op == "follow" && a.size() == 1 && (a[0] == "on" || a[0] == "off"))	// follow on|off: the follow camera on the first selected character (fails with none)
	{
		set_follow(a[0] == "on");
		if (app.follow != (a[0] == "on")) { script_error(c); }
	}
	else if (c.op == "follow" && a.size() >= 2 && a[0] == "char")	// follow char NAME... | id=N: the follow camera on that character (a spectator has no selection), its CHAR_STATS (order 87)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (i > 1 ? " " : "") + a[i]; }
		const uint64_t id = named_character(text);
		if (!id)
		{
			fprintf(stderr, "kclient: script: no character \"%s\"; in view:\n", text.c_str());
			for (const auto& [cid, ch] : app.sim.decoder().chars) { fprintf(stderr, "  %llu \"%s\" flags 0x%x carried by %llu\n", (unsigned long long)cid, app.sim.decoder().str(ch.look.name).c_str(), ch.cur.flags, (unsigned long long)ch.cur.carriedBy); }
			script_error(c);
		}
		app.inspect = id;	// the watch is the inspect focus: its CHAR_STATS carries the goal
		if (!set_follow_id(id, true)) { script_error(c); }	// dev/test-only route (a non-squad target is allowed under --debug / scripted)
	}
	else if (c.op == "hold" && a.size() == 1 && (a[0] == "on" || a[0] == "off"))	// hold on|off: order 4 for the selection, as the HUD HOLD toggle (off = a plain stop)
	{
		if (!app.sim.stop(a[0] == "on")) { script_error(c); }
	}
	else if (c.op == "camera" && a.size() >= 2 && a[0] == "beam")	// camera beam [id=N] [windup|live|same]: the named beam, else the next to strike (a live one if none is in its lead), any if no token is given, `same` the beam the last call framed; framed 1200 u south of and 600 u above its ground point, looking down at it, the pick and its lead logged (h2v.14.2.1)
	{
		uint32_t id = 0;
		int phase = 0;
		double dist = 1200, pitchDeg = 0;	// pitch=0: the op's own framing below (atan2(-600, 1200))
		bool havePitch = false;
		for (size_t i = 1; i < a.size(); i++)
		{
			if (a[i].starts_with("id=")) { id = (uint32_t)std::strtoul(a[i].c_str() + 3, nullptr, 10); }
			else if (a[i].starts_with("dist=")) { dist = std::fmax(10.0, atof(a[i].c_str() + 5)); }
			else if (a[i].starts_with("pitch=")) { pitchDeg = atof(a[i].c_str() + 6); havePitch = true; }
			else if (a[i] == "windup") { phase = 1; }
			else if (a[i] == "live") { phase = 2; }
			else if (a[i] == "same") { phase = 3; }
		}
		float x = 0, z = 0;
		BeamField::Aim pick;
		if (!app.sim.active() || !app.beams.aim(app.sim.tick(), id, phase, app.pos, x, z, &pick))
		{
			fprintf(stderr, "kclient: script: no beam %s (BEAMS frames: the sim sends them, not an offline run)\n", phase == 1 ? "in its wind-up" : phase == 2 ? "striking" : phase == 3 ? "from a this-run pick" : id ? "with that id" : "to frame");
			script_error(c);
		}
		else
		{
			const double lead = ((double)pick.spawnTick - app.sim.tick()) / 20.0;
			fprintf(stderr, "kclient: script: camera beam id %u %s %+.2f s (render tick %.2f, spawn %llu end %llu) point %.0f %.0f\n",
				pick.id, lead > 0 ? "wind-up" : "strike", lead, app.sim.tick(), (unsigned long long)pick.spawnTick, (unsigned long long)pick.endTick, x, z);
			// The op's framing: 1200 u south of the ground point (yaw 0 looks toward -z) and 600 u up, looking down at it; `dist=D` keeps
			// the line and scales it, `pitch=DEG` sets the height from the distance (the brief's pitch -35, 1000-3000 u out, kenshi-h2v.14.2.1).
			const double radius = havePitch ? dist * std::tan(std::fabs(pitchDeg) * 3.14159265358979 / 180.0) : dist * 0.5;
			free_camera();
			app.pos[0] = x;
			app.pos[1] = (app.hm ? app.hm->height(x, z) : 0.0) + radius;
			app.pos[2] = z + dist;
			app.yaw = 0;
			app.pitch = havePitch ? -std::fabs(pitchDeg) * 3.14159265358979 / 180.0 : std::atan2(-radius, dist);
		}
	}
	else if (c.op == "camera" && a.size() >= 2 && a[0] == "char")	// camera char NAME... | id=N: as camera thing, on the first character with that name (or that id)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (i > 1 ? " " : "") + a[i]; }
		double at[3];
		float hull[3];
		const auto& chars = app.sim.decoder().chars;
		const auto named = [&](const auto& kv) { return text.rfind("id=", 0) == 0 ? std::to_string(kv.first) == text.substr(3) : app.sim.decoder().str(kv.second.look.name) == text; };
		const auto t = std::find_if(chars.begin(), chars.end(), [&](const auto& kv) { return named(kv) && app.sim.placed(kv.first, at, hull); });
		if (t == chars.end())
		{
			fprintf(stderr, "kclient: script: no character \"%s\"; in view:\n", text.c_str());
			for (const auto& [id, ch] : chars) { fprintf(stderr, "  %llu \"%s\" flags 0x%x carried by %llu\n", (unsigned long long)id, app.sim.decoder().str(ch.look.name).c_str(), ch.cur.flags, (unsigned long long)ch.cur.carriedBy); }
			script_error(c);
		}
		free_camera();
		app.pos[0] = at[0];
		app.pos[1] = at[1] + 35;
		app.pos[2] = at[2] + 45;
		app.yaw = 0;
		app.pitch = std::atan2(-35.0, 45.0);	// radians
	}
	else if (c.op == "creator" && !a.empty() && app.creator.active())	// creator list | race NAME... [female] | slider VALUE NAME... | head N | hair NAME...|none | beard NAME...|none | haircol NAME... | skin F | random SEED | wear ARMOUR | weapon WEAPON | clothes on|off | tab body|face|hair | reset | turn DEG | tilt DEG | zoom N [X Y] | point BONE
	{
		std::string rest, err;
		for (size_t i = 1; i < a.size(); i++) { rest += (i > 1 ? " " : "") + a[i]; }
		Characters::Spec& s = app.creator.spec();
		bool ok = true;
		if (a[0] == "list")
		{
			const Characters::RaceChoices& ch = app.creator.choices();
			printf("creator: races:");
			for (const auto& [group, races] : app.chars.raceGroups())
			{
				printf(" %s:", group.c_str());
				for (const std::string& r : races) { printf(" [%s]", r.c_str()); }
			}
			printf("\ncreator: %s %s: %d heads, %zu hairs, %zu beards, %zu hair colours\n", s.race.c_str(), s.female ? "female" : "male", ch.heads[s.female ? 1 : 0], ch.hairs.size(), ch.beards.size(), ch.hairColours.size());
			// the Skin Tone row the panel shows (its own, not a race xml Range), then the Ranges: its range is capped by the palette (k16.2.432.3)
			printf("creator:   Body / Skin Tone 0..%d = %g (skin %.3f)\n", app.creator.skinMax(), app.creator.spec().skin * (float)app.creator.skinMax(), app.creator.spec().skin);
			for (const Characters::EditorRange& r : app.creator.ranges()) { printf("creator:   %s / %s %g..%g = %g\n", r.category.c_str(), r.name.c_str(), r.min, r.max, app.creator.value(r.name)); }
			for (size_t i = 0; i < ch.hairs.size(); i++) { printf("creator:   hair %s (weight %g)\n", ch.hairs[i].c_str(), ch.hairWeights[i]); }
			for (size_t i = 0; i < ch.beards.size(); i++) { printf("creator:   beard %s (weight %g)\n", ch.beards[i].c_str(), ch.beardWeights[i]); }
			printf("creator: spec %s\n", app.creator.specString().c_str());
			printf("creator:   Hit Points %g\n", ch.hp);
			for (const auto& [name, m] : ch.skills) { printf("creator:   %s %+ld%%\n", name.c_str(), std::lround((m - 1) * 100)); }
			if (ch.noBleeding) { printf("creator:   No bleeding\n"); }
			for (size_t i = 0; i < ch.hairColours.size(); i++) { printf("creator:   haircol %s (weight %g)\n", ch.hairColours[i].c_str(), ch.hairColourWeights[i]); }
		}
		else if (a[0] == "race")
		{
			const bool female = a.back() == "female";
			std::string race;
			for (size_t i = 1; i + (female ? 1 : 0) < a.size(); i++) { race += (i > 1 ? " " : "") + a[i]; }
			ok = app.creator.setRace(race, female, err) && app.creator.apply(err);
		}
		else if (a[0] == "slider" && a.size() >= 3)
		{
			std::string name;
			for (size_t i = 2; i < a.size(); i++) { name += (i > 2 ? " " : "") + a[i]; }
			creator_slider(name, (float)atof(a[1].c_str()));
			ok = app.creator.apply(err);
		}
		else if (a[0] == "head" && a.size() == 2) { s.head = atoi(a[1].c_str()); ok = app.creator.apply(err); }
		else if (a[0] == "hair") { s.hair = rest == "none" ? "" : rest; ok = app.creator.apply(err); }
		else if (a[0] == "beard") { s.beard = rest == "none" ? "" : rest; ok = app.creator.apply(err); }
		else if (a[0] == "haircol") { s.hairColour = rest; ok = app.creator.apply(err); }
		else if (a[0] == "skin" && a.size() == 2) { s.skin = (float)atof(a[1].c_str()); ok = app.creator.apply(err); }
		else if (a[0] == "random" && a.size() == 2) { app.creator.randomize((uint32_t)atoi(a[1].c_str())); ok = app.creator.apply(err); }
		else if (a[0] == "turn" && a.size() == 2) { creator_turn((float)(atof(a[1].c_str()) * M_PI / 180.0), 0); }	// the orbit's own path (k16.2.433.1)
		else if (a[0] == "tilt" && a.size() == 2) { creator_turn(0, (float)(atof(a[1].c_str()) * M_PI / 180.0)); }
		else if (a[0] == "zoom" && (a.size() == 2 || a.size() >= 4) && app.creatorUi)	// zoom N [X Y]: N wheel notches at the pointer (X Y, else where `point` left it)
		{
			const double x = a.size() >= 4 ? atof(a[2].c_str()) : app.mouseX, y = a.size() >= 4 ? atof(a[3].c_str()) : app.mouseY;
			creator_zoom((float)atof(a[1].c_str()), x, y);
		}
		else if (a[0] == "point" && a.size() >= 2) { ok = creator_point_at(rest.c_str()); }	// point BONE...: the pointer onto a bone (a shot aims the wheel)

		else if (a[0] == "wear") { s.wear.push_back(rest); ok = app.creator.apply(err); }
		else if (a[0] == "weapon") { s.weapons.push_back(rest); ok = app.creator.apply(err); }
		else if (a[0] == "clothes" && a.size() == 2) { app.creator.clothes = a[1] == "on"; ok = app.creator.apply(err); }
		else if (a[0] == "tab" && a.size() == 2)
		{
			const int page = a[1] == "face" ? ui::kCreatorFace : a[1] == "hair" ? ui::kCreatorHair : ui::kCreatorBody;
			if (app.creatorUi && app.creatorUi->tab(page)) { static_cast<ui::Button*>(app.creatorUi->tab(page))->onClicked(); }
		}
		else if (a[0] == "reset")	// reset: the RESET button's own click (the race's default appearance), for a headless check of it
		{
			if (!app.creatorUi || !app.creatorUi->resetButton()) { err = "no RESET button"; ok = false; }
			else { static_cast<ui::Button*>(app.creatorUi->resetButton())->onClicked(); }
		}
		else { script_error(c); }
		if (!ok) { fprintf(stderr, "kclient: script: creator %s: %s\n", a[0].c_str(), err.c_str()); script_error(c); }
	}
#ifdef KCLIENT_DEV_TOOLS
	else if (c.op == "mouse" && a.size() == 2 && a[0] == "force") { g_forcePickThing = strtoull(a[1].c_str(), nullptr, 10); }	// mouse force ID|0: the next clicks pick that thing (dispatch tests)
#endif
	else if (c.op == "mouse" && a.size() == 3 && a[0] == "pos")	// mouse pos X Y: absolute framebuffer pixels
	{
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = (float)atof(a[1].c_str());
		e.mouse_y = (float)atof(a[2].c_str());
		event(&e);
	}
	else if (c.op == "mouse" && a.size() >= 1 && a.size() <= 2 && a[0].starts_with("char="))	// mouse char=N [UP]: onto N's body centre this frame, UP u higher (an NPC walks; k16.2.121's menu test; the click itself stays a real one)
	{
		const uint64_t id = std::strtoull(a[0].c_str() + 5, nullptr, 10);
		double w[3];
		if (!app.sim.simPos(id, w)) { fprintf(stderr, "kclient: script: character %llu is not drawn\n", (unsigned long long)id); script_error(c); }
		w[1] += 10 + (a.size() == 2 ? atof(a[1].c_str()) : 0);	// the click box's middle (feet to +20), UP higher
		float mvp[16];
		build_mvp(mvp, 1.0f, 1000.0f);
		const float q[3] = { (float)(w[0] - app.pos[0]), (float)(w[1] - app.pos[1]), (float)(w[2] - app.pos[2]) };
		float cc[4];
		for (int i = 0; i < 4; i++) { cc[i] = mvp[i] * q[0] + mvp[4 + i] * q[1] + mvp[8 + i] * q[2] + mvp[12 + i]; }
		if (cc[3] < 1.0f) { fprintf(stderr, "kclient: script: character %llu is behind the camera\n", (unsigned long long)id); script_error(c); }
		float sx = (0.5f + cc[0] / cc[3] * 0.5f) * view::widthf(), sy = (0.5f - cc[1] / cc[3] * 0.5f) * view::heightf();
		// two NPCs can overlap on screen (the Hub's barman and a mercenary): spiral out from the centre until the pick at the pixel is this id
		const auto picks = [&](float x, float y) { const Pick q = pick_screen(x, y); return q.kind == Pick::Character && q.id == id; };
		if (!picks(sx, sy))
		{
			bool found = false;
			for (int r = 6; r <= 72 && !found; r += 6)
				for (int k = 0; k < 16 && !found; k++)
				{
					const float x = sx + (float)r * std::cos(k * 3.14159265f / 8), y = sy + (float)r * std::sin(k * 3.14159265f / 8);
					if (x >= 0 && y >= 0 && x < view::widthf() && y < view::heightf() && picks(x, y)) { sx = x; sy = y; found = true; }
				}
			if (!found) { fprintf(stderr, "kclient: script: character %llu is not pickable on screen\n", (unsigned long long)id); script_error(c); }
		}
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_x = sx;
		e.mouse_y = sy;
		event(&e);
		printf("mouse char %llu: %.0f %.0f\n", (unsigned long long)id, e.mouse_x, e.mouse_y);
	}
	else if (c.op == "mouse" && a.size() == 2)
	{
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		e.mouse_dx = (float)atof(a[0].c_str());
		e.mouse_dy = (float)atof(a[1].c_str());
		event(&e);
	}
	else if (c.op == "click" && a.size() >= 2 && a[0] == "replay")	// click replay KEY=VALUE...: the pick of a recorded click (the "click record:" line of the run log / F12 sidecar), from its own camera and pointer (k16.2.259.3)
	{
		if (!click_replay(std::vector<std::string>(a.begin() + 1, a.end()))) { fprintf(stderr, "kclient: script: click replay needs eye= at= yaw= pitch=\n"); script_error(c); }
	}
	else if (c.op == "scroll" && a.size() == 1)
	{
		e.type = SAPP_EVENTTYPE_MOUSE_SCROLL;
		e.scroll_y = (float)atof(a[0].c_str());
		e.mouse_x = (float)app.mouseX;	// at the cursor, as real wheel events (the UI routes by position)
		e.mouse_y = (float)app.mouseY;
		event(&e);
	}
	else if (c.op == "set" && a.size() == 1)	// set NAME: list the options of a section ("Graphics"), a group ("Shadows") or a key prefix ("render.") with their current values (kenshi-k16.2.244.1)
	{
		const std::vector<cfg::Info> found = cfg::find(a[0]);
		if (found.empty()) { script_error(c); }
		std::string cur;
		for (const cfg::Info& i : found)
		{
			const std::string h = i.section + " / " + i.group;
			if (h != cur) { printf("set %s: %s\n", a[0].c_str(), h.c_str()); cur = h; }
			printf("  %s = %s%s%s%s%s\n", i.key.c_str(), i.value.c_str(), i.value == i.def ? "" : " (default " , i.value == i.def ? "" : i.def.c_str(), i.value == i.def ? "" : ")", i.needs.empty() ? "" : ("   [" + i.needs + "]").c_str());
		}
		fflush(stdout);
	}
	else if (c.op == "set" && a.size() == 2)
	{
		const double ground = dev::viewing() ? 0.0 : app.hm->height(app.pos[0], app.pos[2]);
		if (a[0] == "dist")
		{
			if (app.follow) { app.followDist = std::clamp(atof(a[1].c_str()), 10.0, 2000.0); }	// the follow camera's set distance (kenshi-k16.2.289.2: this used to set only viewDist, which the follow camera never reads)
			else { dev::setViewDist(atof(a[1].c_str())); }	// the mesh viewer's orbit (dev/viewmesh.cpp)
		}
		else if (a[0] == "pos")
		{
			double x, z;
			if (sscanf(a[1].c_str(), "%lf,%lf", &x, &z) != 2) { script_error(c); }
			free_camera();
			teleport(x, z);
		}
		else if (a[0] == "alt") { free_camera(); app.pos[1] = ground + atof(a[1].c_str()); }
		else if (a[0] == "rtsdist") { app.rtsDist = std::clamp(atof(a[1].c_str()), 10.0, 2000.0); }	// the floating camera's zoom (the wheel's), for the boom tests over hills (kenshi-k16.2.353.4)
		else if (a[0] == "yaw") { app.yaw = atof(a[1].c_str()) * 3.14159265358979 / 180.0; }
		else if (a[0] == "pitch") { app.pitch = atof(a[1].c_str()) * 3.14159265358979 / 180.0; }
		else if (a[0] == "rtsboom") { app.rtsLog = atoi(a[1].c_str()) != 0; }	// update_rts prints pitch, boom length and eye height each frame
		else if (a[0] == "portraits") { app.showPortraits = atoi(a[1].c_str()) != 0; }
		else if (a[0] == "exposure") { app.opt.exposure = a[1] == "auto" ? 0.0 : atof(a[1].c_str()); }
		else if (a[0] == "time") { app.sky.clock.setHours(atof(a[1].c_str())); }
		else if (a[0] == "timescale") { app.sky.clock.setSpeed(atof(a[1].c_str())); }
		else if (a[0] == "playerbuild") { app.scriptBuild = (float)atof(a[1].c_str()); }
		else if (a[0] == "animate") { app.scriptAnim = atoi(a[1].c_str()) != 0; }
		else if (a[0] == "framedt")	// real | fixed: a motion run steps the world by the frame's real duration (the sim runs in real time), a shot by the fixed 1/60 s
		{
			if (!app.opt.debug) { script_needs_debug(c); }
			app.scriptRealDt = a[1] == "real";
		}
		else if (a[0] == "particles") { app.particleScriptDt = std::fmax(0.0, atof(a[1].c_str())); }
		else if (a[0] == "beams") { app.beamsOn = a[1] != "off"; }
		else if (a[0] == "partdepth") { app.particles.setDepthTest(a[1] != "off"); }	// dev A/B: the particle FS's scene depth test (kenshi-h2v.14.2.1)
#ifdef KCLIENT_DEV_TOOLS
		else if (a[0] == "stall") { app.sim.devStall(atoi(a[1].c_str()) != 0); printf("stall %s: render tick %.2f newest snapshot %llu\n", a[1].c_str(), app.sim.tick(), (unsigned long long)app.sim.decoder().lastCompleteTick); }	// hold the snapshots (kenshi-k16.2.479.1)
#endif
		else if (a[0] == "simspeed") { app.sim.setSpeed((float)atof(a[1].c_str())); }	// the sim's game speed (order SET_SPEED)
		else if (a[0] == "day") { app.sky.clock.day = atoi(a[1].c_str()) - 1; }
		else if (a[0] == "weather") { set_weather(a[1]); }
		else if (a[0] == "windspeed") { if (!app.opt.debug) { script_needs_debug(c); } app.windSpeedForce = a[1] == "live" ? -1.0 : atof(a[1].c_str()); }	// set windspeed N|live: the foliage and blade wind speed (u/s)
		else if (a[0] == "wind")	// set wind DEG|live: wind-aimed parts face (turn to) this wind, as a scene's wind=DEG; live = the sim's again
		{
			if (!app.opt.debug) { script_needs_debug(c); }
			app.windForce = a[1] == "live" ? -1.0 : atof(a[1].c_str());
			if (app.windForce >= 360) { script_error(c); }
		}
		else if (a[0] == "scene")
		{
			if (!app.opt.debug) { script_needs_debug(c); }	// the scenes are a development feature
			if (!dev::sceneSetName(dev_context(), a[1])) { script_error(c); }	// dev/scenetest.cpp
		}
		else if (a[0].find('.') != std::string::npos && cfg::set(a[0] + "=" + a[1])) { }	// any config key: set ui.progress_bars squad (as --ui.progress_bars=squad; not saved)
		else { script_error(c); }
	}
	else if (c.op == "transfer" && a.size() == 2 && (a[0] == "take" || a[0] == "put") && cont_scripted(false))	// transfer take|put N: order 63 (by record) for entry N of the source's INVENTORY, the open container / body and its user; the UI itself moves by entry (68)
	{
		const bool take = a[0] == "take";
		const App::OpenCont* oc = cont_scripted(false);
		const simlink::Inventory* src = app.sim.inventory(take ? oc->id : oc->user);
		const size_t n = (size_t)atoi(a[1].c_str());
		if (!src || n >= src->items.size()) { script_error(c); }
		app.sim.transfer(oc->user, oc->id, src->items[n].item, src->items[n].count, take);
	}
	else if (c.op == "transfer" && a.size() == 2 && a[0] == "put" && app.machine)	// transfer put N with a machine window open: order 63 take=0 into the producer, by entry N of the selection's first player character (the drag-in k16.19.7.1 asks about)
	{
		const simlink::Interactable* t = app.sim.thing(app.machine);
		const auto& sel = app.sim.selected();
		const auto own = std::find_if(sel.begin(), sel.end(), [](uint64_t id) { return app.sim.isPlayer(id); });
		const simlink::Inventory* src = own != sel.end() ? app.sim.inventory(*own) : nullptr;
		const size_t n = (size_t)atoi(a[1].c_str());
		if (!t || t->kind != simlink::Interactable::kProducer || !src || n >= src->items.size()) { script_error(c); }
		const bool sent = app.sim.transfer(*own, app.machine, src->items[n].item, src->items[n].count, false);
		printf("transfer put %zu into machine %llu: %s\n", n, (unsigned long long)app.machine, sent ? "sent" : "not sent");
	}
	else if (c.op == "drop" && a.size() >= 1)	// drop N [count]: order 61 for entry N of the selection's first player character's INVENTORY, dropped at its feet (kenshi-k16.2.412.4: a ground item to shoot)
	{
		const auto& sel = app.sim.selected();
		const auto own = std::find_if(sel.begin(), sel.end(), [](uint64_t id) { return app.sim.isPlayer(id); });
		const simlink::Inventory* src = own != sel.end() ? app.sim.inventory(*own) : nullptr;
		const size_t n = (size_t)atoi(a[0].c_str());
		if (!src || n >= src->items.size()) { printf("drop: %s, %zu entries\n", src ? "inventory known" : "no inventory (own selected: %d)", src ? src->items.size() : (size_t)0); script_error(c); }
		const int count = a.size() > 1 ? atoi(a[1].c_str()) : 1;
		const bool sent = app.sim.drop(*own, src->items[n].item, (uint16_t)std::min<int>(count, src->items[n].count));
		printf("drop %zu: %s\n", n, sent ? "sent" : "not sent");
	}
	else if (c.op == "transfer" && a.size() == 2 && a[0] == "sell" && cont_scripted(true))	// transfer sell N: order 68 for the user's entry N into the open container, as the UI's move (a TRADE stock: a sale)
	{
		const App::OpenCont* oc = cont_scripted(true);
		const simlink::Inventory* inv = app.sim.inventory(oc->user);
		const size_t n = (size_t)atoi(a[1].c_str());
		if (!inv || n >= inv->items.size() || !app.sim.transferEntry(oc->user, oc->id, inv->version, (uint16_t)n, inv->items[n].count, false, 255, 0, 0)) { script_error(c); }
	}
	else if (c.op == "machine" && dev::machineOp(dev_context(), a)) {}	// machine id=N: N's machine window opens (dev/debughost.cpp)
#ifdef KCLIENT_DEV_TOOLS
	else if (c.op == "roads" && a.size() == 3 && (a[0] == "select" || a[0] == "point"))	// roads select ROAD MARKER: select it; roads point ROAD MARKER: the pointer onto its handle (then mouse down|up left clicks it)
	{
		const int r = atoi(a[1].c_str()), k = atoi(a[2].c_str());
		if (a[0] == "select") { if (!dev::roadedit::selectMarker(r, k, app.opt.data)) { script_error(c); } else { printf("%s\n", roads_dump().c_str()); fflush(stdout); } }
		else if (!roads_pointer_to(r, k)) { script_error(c); }
	}
	else if (c.op == "roads" && a.size() == 2 && a[0] == "row")	// roads row N: the panel's Nth shown row, as its click
	{
		const int row = atoi(a[1].c_str());
		const int s = app.roadsPanel && row >= 0 ? app.roadsPanel->rowSuspect((size_t)row) : -1;
		if (s < 0) { script_error(c); } else { roads_select_suspect(s); }
	}
	else if (c.op == "roads" && a.size() == 2 && a[0] == "mark")	// roads mark ok|unsure|bad|clear: the selection's mark
	{
		const int m = a[1] == "ok" ? ui::kRoadOk : a[1] == "unsure" ? ui::kRoadUnsure : a[1] == "bad" ? ui::kRoadBad : a[1] == "clear" ? ui::kRoadUnmarked : -1;
		if (!dev::roadedit::mark(m)) { script_error(c); }
	}
#endif
	else if (c.op == "roads" && a.size() == 1)	// roads on|off|next|prev|dump: the road editor (dev/roadedit.cpp, kenshi-ix9r); dump prints the selected suspect
	{
		if (a[0] == "on" || a[0] == "off") { cfg::set(std::string("debug.roads=") + a[0]); }
		else if (a[0] == "next") { roads_step(1); }
		else if (a[0] == "prev") { roads_step(-1); }
		else if (a[0] == "list") { printf("%s", roads_list().c_str()); fflush(stdout); }
		else if (a[0] == "dump") { printf("%s\n", roads_dump().c_str()); fflush(stdout); }
		else { script_error(c); }
	}
	else if (c.op == "hints" && dev::hintsOp(dev_context(), a)) {}	// hints at X Y | hints kinds: the control-hints list at a screen point, or the kind registry (dev/hintsop.cpp)
	else if (c.op == "inspect" && a.size() == 1 && a[0].starts_with("id="))	// inspect id=N: N becomes the inspect focus (order 87, its CHAR_STATS), as a click on an NPC
	{
		app.inspect = std::stoull(a[0].substr(3));
	}
	else if (c.op == "attack" && a.size() == 1 && a[0].starts_with("id="))	// attack id=N: the attack order for the selection on N (a Ctrl+click without aiming)
	{
		if (!app.sim.attack(strtoull(a[0].c_str() + 3, nullptr, 10))) { script_error(c); }
	}
	else if (c.op == "move" && a.size() == 2)	// move X Z: move_to for the selection, no height: the sim's walkable surface nearest in xz (a walk too far to aim with the pointer)
	{
		const double at[3] = { atof(a[0].c_str()), app.hm->height(atof(a[0].c_str()), atof(a[1].c_str())), atof(a[1].c_str()) };
		const SimView::Click click{ "script" };
		if (!app.sim.moveTo(at, false, false, &click)) { script_error(c); }
	}
	else if (c.op == "move" && a.size() == 3)	// move X Y Z: move_to for the selection, to the surface at height Y (a floor, a deck; sent as yValid)
	{
		const double at[3] = { atof(a[0].c_str()), atof(a[1].c_str()), atof(a[2].c_str()) };
		const SimView::Click click{ "script" };
		if (!app.sim.moveTo(at, false, true, &click)) { script_error(c); }
	}
	else if (c.op == "interact" && (a.size() == 2 || a.size() == 3) && a[0].starts_with("id="))	// interact id=N ACTION [FLAGS]: order 60 for the selection on N (6 talk: a moving target, without aiming the pointer; 5 loot opens the body as the UI does; FLAGS as simview interact: 2 on a close (2) = lock the door)
	{
		const uint64_t id = strtoull(a[0].c_str() + 3, nullptr, 10);
		const int action = atoi(a[1].c_str());
		const uint8_t flags = a.size() == 3 ? (uint8_t)atoi(a[2].c_str()) : 0;
		if (!(action == 5 ? open_body(id, false) : app.sim.interact(id, (uint8_t)action, false, flags))) { script_error(c); }
	}
	else if (c.op == "gameoption" && a.size() == 2 && (a[0] == "feed_animals" || a[0] == "share_food") && (a[1] == "on" || a[1] == "off"))	// gameoption feed_animals|share_food on|off: order 121 (the sim's answer is the GAME_OPTIONS frame; kenshi-ehn5)
	{
		if (!app.sim.setGameOption(a[0] == "share_food" ? 1 : 0, a[1] == "on")) { script_error(c); }
	}
	else if (c.op == "options" && a.size() == 2 && a[0] == "tab" && app.options) { app.options->showTab(a[1]); }	// options tab NAME: open the options window on that tab (Game; kenshi-s3je)
	else if (c.op == "options" && a.size() == 2 && a[0] == "click" && (a[1] == "feed_animals" || a[1] == "share_food"))	// options click feed_animals|share_food: the World group's box click, as the UI does
	{
		ui::Button* b = app.options ? app.options->worldBox(a[1] == "share_food" ? ui::kShareFood : ui::kFeedAnimals) : nullptr;
		if (!b || !b->enabled || !b->onClicked) { script_error(c); } else { b->onClicked(); }
	}
	else if (c.op == "portrait" && !a.empty())	// portrait [add] NAME... | id=N: a click (add: Shift) on that character's bar portrait, the HUD's own path (a follow moves to the selection's first in squad order)
	{
		const bool add = a[0] == "add", dbl = a[0] == "dbl";
		std::string text;
		for (size_t i = add || dbl ? 1 : 0; i < a.size(); i++) { text += (text.empty() ? "" : " ") + a[i]; }
		const uint64_t id = named_character(text);
		if (!id || !app.sim.isPlayer(id)) { script_error(c); }
		portrait_gesture(id, dbl ? hints::Gesture::kDoubleClick : add ? hints::Gesture::kShiftClick : hints::Gesture::kClick);
	}
	else if (c.op == "cast" && a.size() == 2 && (a[0] == "focus" || a[0] == "follow") && a[1].starts_with("id="))	// cast focus|follow id=N: the cast row's double click (portrait_focus) / FOLLOW button (cast_follow) on any character
	{
		const uint64_t id = std::strtoull(a[1].c_str() + 3, nullptr, 10);
		if (!app.sim.decoder().chars.count(id)) { script_error(c); }
		if (a[0] == "focus") { portrait_focus(id); } else { cast_follow(id); }
	}
	else if (c.op == "hints" && a.size() == 1 && a[0] == "shown")	// hints shown: the control-hints panel as the pointer's widget shows it now (ui.control_hints on), one `  input  label` row each
	{
		const ui::HintBlock hb = ui::shownHints();
		printf("hints shown: %s: %zu rows\n", hb.title.empty() ? "(no title)" : hb.title.c_str(), hb.lines.size());
		for (const ui::HintLine& l : hb.lines) { printf("  %s  \"%s\"%s\n", l.input.c_str(), l.label.c_str(), l.enabled ? "" : "  disabled"); }
	}
	else if (c.op == "hints" && a.size() > 1 && a[0] == "portrait")	// hints portrait NAME...: the list a portrait offers now (the same text as `hints at`)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (text.empty() ? "" : " ") + a[i]; }
		const uint64_t id = named_character(text);
		if (!id) { script_error(c); }
		else { fputs(hints::format_hover(hints::hints_for_portrait(id, hints_context())).c_str(), stdout); }
	}
	else if (c.op == "hints" && a.size() > 1 && a[0] == "char")	// hints char NAME... | id=N: the list a character in the world offers now (the same text as `hints at` over it)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (text.empty() ? "" : " ") + a[i]; }
		const uint64_t id = named_character(text);
		if (!id) { script_error(c); }
		else { Pick p; p.kind = Pick::Character; p.id = id; fputs(hints::format_hover(hints::hints_for(p, hints_context())).c_str(), stdout); }
	}
	else if (c.op == "hints" && a.size() == 2 && a[0] == "thing")	// hints thing ID [label]: the list an item, bed, producer or door offers now
	{
		const simlink::Interactable* t = app.sim.thing(strtoull(a[1].c_str(), nullptr, 10));
		if (!t) { script_error(c); }
		else
		{
			Pick p; p.id = t->id;
			p.kind = t->kind == simlink::Interactable::kDoor ? Pick::Door : t->kind == simlink::Interactable::kBed ? Pick::Bed : t->machineLike() ? Pick::Producer : t->kind == simlink::Interactable::kContainer ? Pick::Container : Pick::Item;
			fputs(hints::format_hover(hints::hints_for(p, hints_context())).c_str(), stdout);
		}
	}
	else if (c.op == "hints" && a.size() == 3 && a[0] == "thing" && a[2] == "label")	// hints thing ID label: the same, with the pointer on its label
	{
		const simlink::Interactable* t = app.sim.thing(strtoull(a[1].c_str(), nullptr, 10));
		if (!t) { script_error(c); }
		else { Pick p; p.id = t->id; p.kind = t->kind == simlink::Interactable::kContainer ? Pick::Container : Pick::Item; hints::Context cx = hints_context(); cx.labelId = t->id; fputs(hints::format_hover(hints::hints_for(p, cx)).c_str(), stdout); }
	}
	else if (c.op == "hints" && a.size() >= 3 && a[0] == "ground")	// hints ground X Z [building ID]: the list the ground at world x z (a player's building when its id follows) offers now
	{
		Pick p = ground_pick(a, 1);
		fputs(hints::format_hover(hints::hints_for(p, hints_context())).c_str(), stdout);
	}
#ifdef KCLIENT_DEV_TOOLS
	else if (c.op == "groundclick" && a.size() >= 2)	// groundclick [right] [shift] X Z [building ID]: sim_click on the ground at world x z (the control-hints cases; `click hint:` + `sim click` lines)
	{
		bool right = false, shift = false;
		size_t i = 0;
		for (; i < a.size() && (a[i] == "right" || a[i] == "shift"); i++) { (a[i] == "right" ? right : shift) = true; }
		if (a.size() < i + 2) { script_error(c); }
		else { g_forcePickGround = ground_pick(a, i); sim_click(app.mouseX, app.mouseY, right, shift); g_forcePickGround = Pick{}; }
	}
	else if (c.op == "thingclick" && a.size() >= 1)	// thingclick [right] [shift] [label] ID: sim_click on that thing wherever the pointer is (`click hint:` + `sim click` lines)
	{
		bool right = false, shift = false;
		size_t i = 0;
		bool label = false;
		for (; i + 1 < a.size() && (a[i] == "right" || a[i] == "shift" || a[i] == "label"); i++) { (a[i] == "right" ? right : a[i] == "shift" ? shift : label) = true; }
		const uint64_t id = strtoull(a[i].c_str(), nullptr, 10);
		if (!id || !app.sim.thing(id)) { script_error(c); }
		else { g_forcePickThing = id; g_forcePickLabel = label; sim_click(app.mouseX, app.mouseY, right, shift); g_forcePickThing = 0; g_forcePickLabel = false; }
	}
	else if (c.op == "charclick" && !a.empty())	// charclick [right] [shift] [ctrl] NAME... | id=N: sim_click on that character wherever the pointer is (the control-hints cases: the click and its `click hint:` line)
	{
		bool right = false, shift = false;
		size_t i = 0;
		for (; i < a.size() && (a[i] == "right" || a[i] == "shift"); i++) { (a[i] == "right" ? right : shift) = true; }
		std::string text;
		for (; i < a.size(); i++) { text += (text.empty() ? "" : " ") + a[i]; }
		const uint64_t id = named_character(text);
		if (!id) { script_error(c); }
		else { g_forcePickChar = id; sim_click(app.mouseX, app.mouseY, right, shift); g_forcePickChar = 0; }
	}
	else if (c.op == "menuclick" && a.size() >= 2)	// menuclick KIND ID | id=N | NAME...: the hold menu's row of that kind on a character or thing (`click hint:` + `sim menu:` lines; the control-hints cases)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (text.empty() ? "" : " ") + a[i]; }
		Pick p;
		const uint64_t num = strtoull(text.c_str(), nullptr, 10);
		const simlink::Interactable* th = num ? app.sim.thing(num) : nullptr;
		if (th) { p.id = num; p.kind = th->kind == simlink::Interactable::kContainer ? Pick::Container : th->machineLike() ? Pick::Producer : th->kind == simlink::Interactable::kDoor ? Pick::Door : th->kind == simlink::Interactable::kBed ? Pick::Bed : Pick::Item; }
		else if (const uint64_t id = named_character(text)) { p.id = id; p.kind = Pick::Character; }
		const hints::Context ctx = hints_context();
		const hints::Action* row = nullptr;
		hints::Hover h;
		if (p.id) { h = hints::hints_for(p, ctx); }
		for (const hints::Action& x : h.actions) { if (x.menu && a[0] == x.kind) { row = &x; break; } }
		if (!row) { printf("menuclick: no %s row\n", a[0].c_str()); script_error(c); }
		else { printf("click hint: %s\n", row->kind); run_menu_action(*row); }
	}
#endif
	else if (c.op == "squadfirst" && !a.empty())	// squadfirst NAME... | id=N: the squads window's drag to first place: the character leads its squad (order SQUAD_MOVE, index 0)
	{
		std::string text;
		for (const std::string& w : a) { text += (text.empty() ? "" : " ") + w; }
		const uint64_t id = named_character(text);
		uint64_t squad = 0;
		for (const simlink::PlayerSquads::Squad& q : app.sim.decoder().playerSquads.squads) { if (std::find(q.members.begin(), q.members.end(), id) != q.members.end()) { squad = q.id; } }
		if (!id || !squad || !app.sim.squadMove(id, squad, 0, false)) { script_error(c); }
	}
	else if (c.op == "follow" && a.size() == 1 && a[0] == "lock")	// follow lock: the F8 panel's mirror-lock row (a follow-stream run only; kenshi-cym.40.2.10)
	{
		dev::followLockToggle();
	}
	else if (c.op == "follow" && a.size() == 1 && a[0] == "state")	// follow state: the camera mode, the follow target (centre / drive) and the selection (stdout)
	{
		std::string sel;
		for (uint64_t id : app.sim.selected()) { sel += (sel.empty() ? "" : ",") + std::to_string(id); }
		printf("follow state: mode %s center %llu drive %llu subject %llu rtsDist %.1f yaw %.1f focus %.1f %.1f %.1f selected %s\n", camera_mode_name(),
			(unsigned long long)app.centerId, (unsigned long long)(app.follow ? app.followId : 0), (unsigned long long)camera_subject(), app.rtsDist, app.yaw * 180.0 / M_PI, app.focus[0], app.focus[1], app.focus[2], sel.c_str());
	}
	else if (c.op == "select" && a.size() >= 1 && a[0].starts_with("id="))	// select id=N [add]: the selection becomes that player character (add: joins it), as a click on its bar button would
	{
		const uint64_t id = std::strtoull(a[0].c_str() + 3, nullptr, 10);
		if (!app.sim.decoder().chars.count(id)) { fprintf(stderr, "kclient: script: no character %llu\n", (unsigned long long)id); script_error(c); }
		app.sim.select(id, a.size() > 1 && a[1] == "add");
		printf("select %llu: selection %zu\n", (unsigned long long)id, app.sim.selected().size());
	}
	else if (c.op == "open" && a.size() >= 1 && a[0].starts_with("id="))	// open id=N [view]: a box's window as the UI opens it: the selection walks there and the window opens on arrival (view: a left click, the window alone)
	{
		const uint64_t id = strtoull(a[0].c_str() + 3, nullptr, 10);
		if (a.size() == 2 && a[1] == "view")
		{
			if (!app.sim.thing(id)) { script_error(c); }
			if (!cont_find(id)) { App::OpenCont oc; oc.id = id; oc.view = true; cont_add(oc); }
		}
		else if (!open_container(id, false)) { script_error(c); }
	}
	else if (c.op == "interact" && a.size() == 2 && a[0] == "speaker")	// interact speaker ACTION: on the NPC whose bubble to a player character is newest (none: a log line)
	{
		uint64_t id = 0;
		double at = -1;
		for (const auto& [who, sp] : app.sim.speech()) { if (!app.sim.isPlayer(who) && app.sim.isPlayer(sp.target) && sp.tick > at) { id = who; at = sp.tick; } }
		if (!id) { printf("interact speaker: nobody speaking to us\n"); }
		else if (!app.sim.interact(id, (uint8_t)atoi(a[1].c_str()))) { script_error(c); }
	}
#ifdef KCLIENT_DEV_TOOLS
	else if (c.op == "serverhold" && a.size() == 1)	// serverhold N: the title's server stays "starting" for N frames (the early-click test, kenshi-k16.2.210.4; dev builds only)
	{
		app.devServerHold = std::max(0, atoi(a[0].c_str()));
		printf("title: server held for %d frames\n", app.devServerHold);
	}
	else if (c.op == "serverfail")	// serverfail: the local server's start fails now, as a refused launch does (kenshi-k16.2.210.6; dev builds only)
	{
		app.devServerHold = 0;
		app.serverErr = "dev: forced failure";
		app.local = false;
		printf("title: server start failed (dev)\n");
	}
	else if (c.op == "stall" && a.size() == 2 && (a[1] == "gate" || a[1] == "b"))	// stall MS gate|b: this frame's logic sleeps MS just before the gate (overlap: the draw may run meanwhile) or just after it (part B); the quit checks of kenshi-k16.2.205.12.96
	{
		gStallAt[a[1] == "b" ? 1 : 0] = atof(a[0].c_str());
		printf("stall: logic sleeps %s ms %s the gate\n", a[0].c_str(), a[1] == "b" ? "after" : "before");
	}
	else if (c.op == "stall" && a.size() == 1)	// stall MS: this logic frame sleeps MS (a decode stall, REPORT §4's 1.2 s; the lockstep window check, kenshi-k16.2.205.12.25; dev builds only)
	{
		printf("stall: logic sleeps %s ms\n", a[0].c_str());
		fflush(stdout);
		std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(atof(a[0].c_str())));
	}
	else if (c.op == "renderstall" && a.size() == 1)	// renderstall MS: the render thread sleeps MS at its next frame start, as a window held in its swap (lockstep only: logic's idle link poll; dev builds only)
	{
		printf("renderstall: %s ms (render tick %.2f, newest snapshot %llu)\n", a[0].c_str(), app.sim.tick(), (unsigned long long)app.sim.decoder().lastCompleteTick);
		logicthread::stallRender(atof(a[0].c_str()));
	}
	else if (c.op == "titlehold" && a.size() == 1)	// titlehold N: the title's host holds button N (0 CONTINUE, 2 LOAD GAME) as its click would; running it prints (no saves needed)
	{
		const int b = atoi(a[0].c_str());
		if (b == 0 || b == 2) { title_hold_dev(b); }
		else { script_error(c); }
	}
	else if (c.op == "resize" && a.size() == 2)	// resize W H: the window to WxH, as a window-manager resize would; a mid-load resize checks the loading screen's re-layout (k16.2.221; dev builds only)
	{
		const int w = atoi(a[0].c_str()), h = atoi(a[1].c_str());
		if (w < 1 || h < 1) { script_error(c); }
		else
		{
			sappcmd::post([w, h] { if (!windowgeom::resizeWindow(w, h)) { fprintf(stderr, "kclient: resize %dx%d: no X window\n", w, h); } });	// Xlib on the sapp thread (kenshi-k16.2.205.12.25)
			printf("resize: asked for %dx%d\n", w, h);
		}
	}
	else if (c.op == "dump" && a.size() == 1 && a[0] == "map")	// dump map: the MAP page's view (k16.2.435): zoom (pixels a map texel, 0 = fitted), the world
	{	//                                                   point at its centre, the view's rect and whether the page shows (stdout)
		if (!app.uiOn || !app.overview) { printf("map view: no UI\n"); }
		else
		{
			const ui::MapView v = app.overview->mapView();
			const ui::MapFrame f = app.overview->mapFrame();
			printf("map view: zoom %.6f cx %.3f cz %.3f (view %.0f x %.0f, %s)\n", v.zoom, v.cx, v.cz, f.w, f.h, app.overview->shown(ui::OverviewPage::kMap) ? "shown" : "hidden");
		}
	}
	else if (c.op == "dump" && a.size() == 1 && a[0] == "inventory") { printf("inventory: shown %d\n", app.itemsShown ? 1 : 0); }	// dump inventory: whether the inventory window is open (kenshi-cym.40.2.10)
	else if (c.op == "dump" && a.size() == 1 && a[0] == "title")	// dump title: the title screen's state: its held click, the offline line and the new game panel (kenshi-k16.2.210.4; stdout)
	{
		printf("title: shown %d, pending %d, status \"%s\", new game %d\n", title_shown() ? 1 : 0, app.menuPending, title_status().c_str(),
			app.newGame && app.newGame->shown() ? 1 : 0);
	}
	else if (c.op == "dump" && a.size() == 1 && a[0] == "creator")	// dump creator: the creator's member, its name (the host's and the box's) and which game panels are open (kenshi-k16.2.472.1; stdout)
	{
		const ui::EditBox* box = app.creatorUi ? dynamic_cast<const ui::EditBox*>(app.creatorUi->nameBox()) : nullptr;
		printf("creator: shown %d, member %d/%zu, name \"%s\", box \"%s\", box focused %d, panels: stats %d, inventory %d, options %d, build %d, help %d\n", app.creatorUi && app.creatorUi->shown() ? 1 : 0,
			app.ngIndex + 1, app.ngMembers.size(), app.creator.name.c_str(), box ? box->text.c_str() : "", box && ui::focusedWidget() == box ? 1 : 0, app.stats && app.stats->shown() ? 1 : 0,
			app.itemsShown ? 1 : 0, app.options && app.options->shown() ? 1 : 0, app.build && app.build->shown() ? 1 : 0, app.tutorials && app.tutorials->browserShown() ? 1 : 0);
	}
	else if (c.op == "restmetric")	// the creator preview's rest pose (knees, pelvis) for the IK on / off check (dev/devhost.cpp, kenshi-k16.17.7.4)
	{
		if (!dev::restMetric()) { script_error(c); }
	}
	else if (c.op == "playtest" && !a.empty())	// the playtest store's entry point (dev/devhost.cpp)
	{
		if (!dev::scriptOp(a)) { script_error(c); }
	}
	else if (c.op == "lab" && !a.empty())	// lab clip NAME... | race NAME... | female 0|1 | play 0|1 | step -1|+1 | seek T | speed X | camera NAME |
	{	//                                    plant POINT... | overlay trails|root|bones 0|1 | measure | mark | view W H | csv FILE | shot FILE | close |
		//                                    group [NAME...] | follow 0|1 | list (the animation lab, dev/debughost.cpp)
		if (!dev::labOp(a)) { script_error(c); }
	}
#endif
	else if (c.op == "quit")
	{
		dev::shotTreesFinish(dev_context(), "quit");	// the run ends here: keep the answers that came in, the rest listed missing (dev/shotsidecars.cpp)
		request_quit();
	}
	else if (c.op != "screenshot" || a.size() != 1) { script_error(c); }
}

}
