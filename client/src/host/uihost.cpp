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
#include "dev/roadedit.hpp"
#endif
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
#include "ui/minimap.hpp"
#include "ui/statuspopup.hpp"
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
#include "host/uihost.hpp"
#include "host/panels.hpp"
#include "dev/framelog.hpp"

// Control hints, the key rows (kenshi-eq2z): each HUD widget lists the keys that do its thing, named from the bindings as they are now (an unbound
// action has no row). The kinds are the guide-coverage keys: each phrase must appear in docs/PLAYER_GUIDE.md section 5.
HINT_KIND("key_focus", "focus the camera on the selection");
HINT_KIND("key_next", "select the next character");
HINT_KIND("key_all", "select all characters");
HINT_KIND("key_slot", "select this character");
HINT_KIND("key_pause", "pause the game");
HINT_KIND("key_speed", "set the game speed");
HINT_KIND("key_floor", "step the floor");
HINT_KIND("key_drive", "direct drive on the selection");
HINT_KIND("key_build", "open the build panel");

static void key_row(std::vector<ui::HintLine>& out, const char* action, const std::string& label)
{
	const std::string key = ui::keys::label(action);
	if (!key.empty()) { out.push_back({ key, label, true, "" }); }
}

static void fill_key_hints(ui::HudKeyHints& k, size_t members)
{
	k = ui::HudKeyHints();
	key_row(k.portrait, "focus_char", "Focus the camera on the selection");
	key_row(k.portrait, "character_next", "Select the next character");
	key_row(k.portrait, "select_all", "Select all characters");
	k.slot.resize(std::min<size_t>(members, 10));
	for (size_t i = 0; i < k.slot.size(); i++) { key_row(k.slot[i], ("select_" + std::to_string(i)).c_str(), "Select this character (twice: focus)"); }
	key_row(k.speed[0], "pause", "Pause the game");
	for (int i = 1; i < 4; i++) { key_row(k.speed[i], ("speed_" + std::to_string(i)).c_str(), "Set the game speed to " + std::to_string(i)); }
	key_row(k.floor, "floor_up", "Step the floor up");
	key_row(k.floor, "floor_down", "Step the floor down");
	key_row(k.moveSpeed, "camera_drive", "Direct drive on the selection (again: leave it)");
	key_row(k.build, "toggle_build", "Open the build panel");
}


const cfg::Bool kMinimap{ cfg::Group{ "HUD and labels" }, "ui.minimap", true, "Minimap: a small map window in a corner of the screen, centred on the camera; drag it to move it, the wheel over it zooms (the place and the zoom are remembered). A deliberate addition: the original has none" };

namespace host
{

// The title screen, the Escape menu and their save panel (docs/UI.md "Menus"). Saving and loading wait for the server's orders (SIMLINK 80-83, planned);
// NEW GAME / IMPORT GAME have no server side yet.

static void init_menus()
{
	const auto notYet = [](const char* what) { fprintf(stderr, "kclient: %s: the server has no support yet\n", what); ui::message(std::string(what) + " is not available yet."); };
	// The server's save dir (SIMLINK 80-83, SAVES 32): the list is refreshed on opening and by every SAVES (update_ui). A load drops back to the game.
	const auto live = [](const char* what) { if (app.sim.active()) { return true; } ui::message(std::string(what) + " needs a running server."); return false; };
	ui::SaveActions sva;
	const auto loadNow = [=](const std::string& name, bool reset)
	{
		if (!live("Loading") || !app.sim.loadGame(name, reset)) { return; }
		app.camLoadName = name;	// its reset brings the camera sidecar home (k16.2.134.1)
		app.camLoadFrame = app.frames;
		audiohook_event("Create_or_Load_Game");	// at the request, before the save is read (audio_load_pause.md 1b)
		app.gameMenu->show(false);
		if (title_shown()) { app.mainMenu->show(false); }
		menu_pause(false);
		app.menuPaused = false;	// the save brings its own clock
	};
	// The title's final load choice is held while its server starts, as CONTINUE / NEW GAME / IMPORT are (kenshi-k16.2.210.4). In the game
	// title_status() is empty, so a load runs at once.
	const auto load = [=](const std::string& name, bool reset)
	{
		if (hold_menu_action(2, [=] { loadNow(name, reset); })) { return; }
		loadNow(name, reset);
	};
	sva.load = load;
	sva.save = [=](const std::string& name) { if (live("Saving") && app.sim.saveGame(name)) { camera_sidecar_save(name); } };
	sva.remove = [=](const std::string& name) { if (live("Deleting a save") && app.sim.deleteSave(name)) { camera_sidecar_delete(name); } };
	sva.opening = [](ui::SaveMode) { app.sim.listSaves(); };
	app.saves = std::make_unique<ui::SavePanel>(std::move(sva));
	ui::MainMenuActions mma;
	const auto continueGame = [=]	// the original's CONTINUE: the newest save
	{
		if (app.session)	// the world is already loaded (--simlink): resuming it is this client's load, so it sounds like one
		{
			audiohook_event("Create_or_Load_Game");
			app.mainMenu->show(false);
			menu_pause(false);
			return;
		}
		const auto& s = app.sim.decoder().saves;
		const auto newest = std::max_element(s.begin(), s.end(), [](const auto& a, const auto& b) { return a.mtime < b.mtime; });
		if (newest != s.end()) { load(newest->name, false); }
	};
	mma.continueGame = [=] { if (hold_menu_action(0, continueGame)) { return; } continueGame(); };
	// The original's 9 rows (ids = its save keys, ranges and defaults per adv22_static.md); the tips are ours. Easy prospecting has no reader in the
	// exe, so no tip. The values stay here (no staging: CANCEL keeps them) until NEW_GAME carries them.
	using Adv = ui::AdvOption;
	app.advRows = {
		{ "ht", "Hunger time", Adv::Kind::kSlider, 0.25, 8, 0.01, 1, "How long characters go before they get hungry. Higher: they eat less often." },
		{ "cod", "Chance of death", Adv::Kind::kSlider, 0.5, 4, 0.01, 1, "How easily anyone, yours or not, dies of their wounds: bleeding out and wounds worsening. Higher: deadlier." },
		{ "gdm", "Global damage multiplier", Adv::Kind::kSlider, 0.5, 4, 0.01, 1, "Scales all combat damage. Fights end sooner either way; it changes their pace more than their difficulty." },
		{ "ps", "Production speed", Adv::Kind::kSlider, 0.5, 2, 0.01, 1, "How fast mines, farms and crafting benches turn out goods. Above 1 is easier." },
		{ "rs", "Research speed", Adv::Kind::kSlider, 0.5, 2, 0.01, 1, "How fast research projects finish. Above 1 is easier." },
		{ "bs", "Building speed", Adv::Kind::kSlider, 0.5, 2, 0.01, 1, "How fast construction goes. Above 1 is easier." },
		{ "nnm", "Number of nests multiplier", Adv::Kind::kSlider, 0.5, 4, 0.01, 1, "How many animal nests and bandit camps the wilds hold. Higher: more dangerous." },
		{ "bl", "Bandits loot the player", Adv::Kind::kTick, 0, 1, 1, 1, "Bandits who win a fight strip your fallen of their goods. Harsh, by design." },
		{ "ep", "Easy prospecting", Adv::Kind::kTick, 0, 1, 1, 0, "" } };
	ui::AdvancedActions aa;
	aa.change = [](const std::string& id, double value)
	{
		for (ui::AdvOption& o : app.advRows) { if (o.id == id) { o.value = std::clamp(value, o.min, o.max); } }
		app.adv->update(app.advRows);
	};
	app.adv = std::make_unique<ui::AdvancedOptions>(std::move(aa));
	app.adv->update(app.advRows);
	ui::NewGameActions nga;
	nga.advanced = [] { app.adv->shown() ? app.adv->show(false) : app.adv->openBelow(*app.newGame->window()); };
	// BEGIN: only a ready start-off (STARTS) wipes the world; new_world first, NEW_GAME once its reset is in (update_ui).
	nga.begin = [=](const std::string& id)
	{
		if (!live("Starting a new game")) { return; }
		const simlink::SimLinkDecoder& d = app.sim.decoder();
		const auto s = std::find_if(d.starts.begin(), d.starts.end(), [&](const auto& s) { return d.str(s.startoff) == id; });
		if (s == d.starts.end() || !s->ready) { ui::message("That start is not available on this server yet."); return; }
		app.newGameSpecs.clear();
		if (std::none_of(s->members.begin(), s->members.end(), [](const auto& m) { return m.flags & 1; })) { ng_begin(id); return; }
		// the squad editor first (no CANCEL: the original has none; Escape is the game menu)
		app.ngMembers.clear();
		for (size_t mi = 0; mi < s->members.size(); mi++)
		{
			const simlink::SimLinkDecoder::Member& m = s->members[mi];
			App::NewGameMember nm;
			nm.wire.race = m.race;
			nm.wire.female = m.female;
			nm.wire.name = nm.name = m.name;
			nm.editable = (m.flags & 1) != 0;
			// the outfit the sim spawns this member in (k16.2.107): the STARTS wire's drawn armour and weapon record strids, never a client guess; a
			// weapon strid 0 is none. Each strid is a record stringID: show the record's name, the stringID itself when the game data lacks it.
			for (uint32_t a : m.armour) { nm.wear.push_back(app.chars.nameOf(d.str(a))); }
			if (m.weapon != 0) { nm.weapons.push_back(app.chars.nameOf(d.str(m.weapon))); }
			app.ngMembers.push_back(std::move(nm));
		}
		app.ngForce.clear();
		for (uint32_t r : s->forceRace) { if (const auto n = app.raceNames.find(d.str(r)); n != app.raceNames.end()) { app.ngForce.insert(n->second); } }
		app.ngStart = id;
		app.newGame->close();
		app.gameMenu->show(false);
		if (title_shown()) { app.mainMenu->show(false); }
		const auto first = std::find_if(app.ngMembers.begin(), app.ngMembers.end(), [](const auto& m) { return m.editable; });
		const auto race = app.raceNames.find(d.str(first->wire.race));
		open_creator((race != app.raceNames.end() ? race->second : std::string("Greenlander")) + (first->wire.female ? "/female" : ""));
		app.creatorUi->setCancel(nullptr);
		ng_load(0);
	};
	app.newGame = std::make_unique<ui::NewGamePanel>(std::move(nga));
	app.newGame->setStarts(app.starts);
	const auto openNewGame = [] { app.newGame->open([] { app.adv->show(false); }); };	// the title keeps its pause	// over the title: hiding it would bring the world and the HUD back
	mma.newGame = [=] { if (hold_menu_action(1, openNewGame)) { return; } openNewGame(); };
	const auto importGame = [=] { notYet("Import game"); };
	mma.import = [=] { if (hold_menu_action(3, importGame)) { return; } importGame(); };
	mma.options = [] { app.options->show(true); };
#ifdef KCLIENT_DEV_TOOLS
	if (dev::scenariosButton(app.opt.debug)) { mma.scenarios = [] { dev::openScenarios(); }; }	// the SCENARIOS button: --debug or --playtest (k16.2.110)
#endif
	mma.exit = [] { request_quit(); };
	app.mainMenu = std::make_unique<ui::MainMenu>(std::move(mma), app.saves.get());
	app.mainMenu->setVersion(std::string("preview ") + kclient_git_hash());	// the title footer: "OpenKenshi preview <hash> - not affiliated ..." (kenshi-379.11.2)
	app.mainMenu->setCredits("OPENKENSHI\n\nA free, open-source engine for Kenshi. Not affiliated with or endorsed by Lo-Fi Games; \"Kenshi\" is a trademark of Lo-Fi Games Ltd.\n"
		"The game's data and art are Lo-Fi Games'; you need your own copy.\n\n"
		"LIBRARIES\nsokol (zlib), stb (MIT / public domain), Jolt Physics (MIT), flecs (MIT), Lua (MIT), SMAA (MIT), Recast & Detour (zlib), libvorbis / libogg (BSD-3-Clause), aoTuV codebooks (BSD-3-Clause)\n\n"
		"REFERENCES\nwwiser, ww2ogg, vgmstream, KenshiLib / RE_Kenshi, OGRE, ParticleUniverse, Kenshi Wiki (format and data notes)");
	DdsImage dds;
	std::string err;
	if (dds_load(app.opt.data + "/gui/gfx/title_screen1.dds", dds, err))
	{
		sg_view_desc vd = {};
		vd.texture.image = dds_make_image(dds, "title-art");
		ui::PortraitRef art;
		art.tex = dl::newTex();	// the title art's UI id, bound at its creation (init_ui is a load step)
		dl::gpu::bind(art.tex, sg_make_view(&vd), {});
		app.mainMenu->setBackground(art);
	}
	else { fprintf(stderr, "kclient: title art: %s\n", err.c_str()); }
	ui::GameMenuActions gma;
	gma.newGame = [] { menu_pause(true); app.newGame->open([] { app.adv->show(false); menu_pause(false); }); };	// the game menu has closed (and unpaused) first
	gma.options = [] { app.options->show(true); };
#ifdef KCLIENT_DEV_TOOLS
	if (dev::scenariosButton(app.opt.debug)) { gma.scenarios = [] { dev::openScenarios(); }; }
#endif
	gma.exitGame = [] { request_quit(); };	// EXIT GAME quits the program, as the original and the title's EXIT do (kenshi-k16.2.212); a --simlink join too
	gma.shown = [](bool on) { menu_pause(on); };
	app.gameMenu = std::make_unique<ui::GameMenu>(std::move(gma), app.saves.get());
	ui::TutorialActions ta;
	ta.activate = [](const std::string& id) { app.tutorials->showBrowser(false); app.tutorials->hint(id); app.tutorials->open(id); };
	app.tutorials = std::make_unique<ui::Tutorials>(std::move(ta));
#ifdef KCLIENT_DEV_TOOLS
	dev::createDebugPanels(dev_context(), app.opt.debug);	// the F8 debug panel and the trails panel (--debug only; dev/debughost.cpp)
	if (app.opt.debug)
	{
		// the light debugger (kenshi-jj3.27.3): its actions are knobs/lightterms.hpp's, the same as the 'light' script op
		ui::LightsPanelActions lpa;
		lpa.set = [](const std::string& name, bool on) { dev::setLight(name, on); };
		lpa.solo = [](const std::string& name) { dev::soloLight(name); };
		lpa.allOn = [] { dev::allLights(); };
		app.lightsPanel = std::make_unique<ui::LightsPanel>(std::move(lpa));
		ui::RoadsPanelActions rpa;	// the road editor's panel (kenshi-tqtd): its actions are dev/roadedit's
		rpa.select = [](int i) { roads_select_suspect(i); };
		rpa.mark = [](int m) { dev::roadedit::mark(m); };
		rpa.copy = [] { const std::string c = dev::roadedit::coords(); if (!c.empty()) { sappcmd::setClipboard(c); } };
		rpa.close = [] { dev::roadedit::setOn(false); };	// a user choice: saved, so it stays closed (kenshi-gopl)
		app.roadsPanel = std::make_unique<ui::RoadsPanel>(std::move(rpa));
	}
	// The animation lab's window (ui/dev/animbrowser.hpp): it shows the host's state and reports its clicks; dev/debughost.cpp renders, plays
	// and measures, and binds the actions.
	dev::createAnimBrowser();
#endif
	dev::createWindows();
#ifdef KCLIENT_DEBUG_VIEW
	ui::DebugTreeActions dta;
	dta.refresh = [] { debug_tree_open(app.treeKind, app.treeId); };
	dta.setAuto = [](bool on) { app.treeAuto = on; };
	dta.close = [] { app.treeAuto = false; };
	dta.copy = [](const std::string& text) { sappcmd::setClipboard(text); };
	app.debugTree = std::make_unique<ui::DebugTree>(std::move(dta));
#endif
	// --simlink is an instant join (user 2026-11-07): straight into the game, no title, no menu music; the title is for a client that starts its own server
	// An interactive --fly run with no --simlink and no --script skips the title (kenshi-k16.2.87; the flag was --debug until
	// kenshi-k16.2.475.1, user 2026-11-21: --debug only adds the development tools): offline, the title's buttons can start nothing, and the
	// run is a fly-around. A title that does show starts its own local server behind it (start_local_server).
	const bool offlineDebug = app.opt.fly && !app.scripted() && app.opt.simlink.empty() && !app.opt.localServer;
	if (((kTitleAtStart.get() && !app.scripted() && app.opt.simlink.empty() && !offlineDebug) || app.opt.localServer) && !app.creator.active())
	{
		app.mainMenu->show(true);
		audiohook_event("Boot_Up_Game");	// the menu music, only under the title (a session that starts in the game has none); Create_or_Load_Game ends it
		if (app.opt.simlink.empty()) { start_local_server(); }
	}
	else if (offlineDebug)
	{
		fprintf(stderr, "kclient: no title screen (--fly with no --simlink and no --script, k16.2.87): the free camera starts over %.0f,%.0f\n", app.pos[0], app.pos[2]);
	}
	app.session = !app.opt.simlink.empty();
}

void init_ui()
{
	std::vector<std::string> themes;	// UI theme folders over data/gui, per file, last wins
	if (const std::string p = app.mods.activePath("darkui"); !p.empty()) { themes.push_back(p); }
	app.uiOn = ui::init(app.opt.data, themes);
	dl::gpu::apply(ui::gfx::uploads().takeUploads());	// the atlas (and init's font pages) now: the cursors and loading screens take its view (kenshi-k16.2.205.12.38)
	ui::setClipboardFns([] { return sappcmd::clipboard(); }, [](const std::string& s) { sappcmd::setClipboard(s); });	// render hook 3's ui/ half (kenshi-k16.3.29)
	// The host's side of the UI sounds (docs/UI.md "Sounds"; kenshi-inspect findings/audio_sfx_triggers.md section 2): the widgets post the original's
	// event names, the host hashes them (audio/events.hpp) and the audio thread sets the switch on the GUI object (110) before the post, as the
	// original's 14070d230 and the other callers do.
	ui::setSoundSink([](const ui::SoundPost& p)
	{
		audiohook_ui({ audio::wwiseHashName(p.event), p.group.empty() ? 0u : audio::wwiseHashName(p.group), p.value.empty() ? 0u : audio::wwiseHashName(p.value), p.event, p.group, p.value });
	});
	ui::setLayoutDir(client_data_dir());	// before any window built from a layout file (<dir>/ui/*.layout: Options, the debug panel)
	if (!app.uiOn) { fprintf(stderr, "kclient: the UI layer did not start; running without it\n"); return; }
	ui::HudActions act;
	act.select = [](uint64_t id, bool add) { portrait_gesture(id, add ? hints::Gesture::kShiftClick : hints::Gesture::kClick); };
	// a portrait / cast-row double click: a player's selects and centers (the original), anyone else's is the watch (a spectator's cast list)
	act.focus = [](uint64_t id) { portrait_gesture(id, hints::Gesture::kDoubleClick); };
#ifdef KCLIENT_DEV_TOOLS
	act.notes = [] { dev::notesKey(true); };	// the NOTES button (a playtest run)
#endif
	act.watch = [](uint64_t id) { app.inspect = id; app.panelProducer = 0; };	// a cast row: the watch (the inspect focus and its CHAR_STATS goal)
	act.follow = [](uint64_t id)	// a cast row's double click or the FOLLOW / STOP button: 0 stops the camera and keeps the watch
	{
		cast_follow(id);
	};
	act.toggle = [](ui::HudToggle t, bool value)	// the ticks with a sim toggle (order 10); the others are disabled in HudState
	{
		if (t == ui::HudToggle::kBlock) { app.sim.setToggle(0, value); }
		// the original's Stealth toggle (140721410): Stealth when it turns sneak on, General_Click when it turns it off; the HUD's SNEAK tick is silent
		// (ui/hud.cpp) so the click is this one, not a generic click as well
		else if (t == ui::HudToggle::kSneak) { if (app.sim.setToggle(1, value)) { if (value) { post_event(audio::kStealth, "Stealth"); } else { post_event(audio::kGeneralClick, "General_Click"); } } }
		else if (t == ui::HudToggle::kRun) { app.sim.setToggle(2, value); }
		// PASSIVE / HOLD: order 10 which 6 / 7 (k16.6.313), the opposite of the shown character's state for the whole selection (as the exe does);
		// order 4 hold (CHAR_DYNAMIC kHold, bit 13) stays on the script's `stop on` only
		else if (t == ui::HudToggle::kPassive || t == ui::HudToggle::kHold)
		{
			const bool hold = t == ui::HudToggle::kHold;
			const auto c = app.sim.decoder().chars.find(app.jobsChar);
			const uint32_t bit = hold ? simlink::kHoldPosition : simlink::kPassive;
			app.sim.setToggle(hold ? 7 : 6, app.jobsChar && c != app.sim.decoder().chars.end() ? !(c->second.cur.flags & bit) : value);
		}
		else if (t == ui::HudToggle::kJobs)	// the original sends the opposite of the shown character's for the whole selection (SIMLINK toggle 4)
		{
			const auto j = app.sim.decoder().jobs.find(app.jobsChar);
			app.sim.setToggle(4, app.jobsChar ? !(j == app.sim.decoder().jobs.end() || j->second.enabled) : value);
		}
	};
	act.removeJob = [](uint64_t job) { app.sim.jobRemove(app.jobsChar, (uint32_t)job); };
	act.moveJob = [](uint64_t job, int index) { app.sim.jobMove(app.jobsChar, (uint32_t)job, (uint16_t)index); };
	act.medic = [] { app.sim.jobAdd(58, 0); };	// the MEDIC button: the Medic job for the selection
	act.stop = [] { app.sim.stop(false); };
	act.floor = [](int delta) { floor_step(delta); };	// as R / F
	act.moveSpeed = [](int mode) { app.sim.setToggle(5, (uint8_t)mode); };	// the speed diamond: order 10 which 5, the mode for the whole selection
	act.prospect = [] { shortcut("prospect"); };
	act.pause = [](bool paused) { app.sim.pause(paused); };
	act.setSpeed = [](float speed) { if (app.sim.setSpeed(speed)) { audiohook_event(speed_event(speed)); } };	// the HUD's speed button; its resume posts Resume_Game through the clock
	act.setActive = [](uint64_t squad) { app.activeSquad = squad; };
	act.shortcut = [](const std::string& action) { shortcut(action); };
	// Follow mode (kenshi-cym.40.2.10): the stream owns pause, speed, selection and orders, so every HUD action that sends an order, changes the
	// selection or the speed, or moves the floor is refused under the mirror lock, logged once per press (as the keys, events.cpp key_actions).
	// The windows' own buttons (inventory, map, options, F8) are not wrapped.
	const auto locked = [](const char* name) { if (!dev::mirrorLocked()) { return false; } printf("follow: hud %s locked (mirror lock)\n", name); return true; };
	const auto guard = [&](auto& fn, const char* name)
	{
		if (!fn) { return; }
		fn = [f = std::move(fn), name, locked](auto&&... a) { if (!locked(name)) { f(std::forward<decltype(a)>(a)...); } };
	};
	guard(act.select, "select"); guard(act.focus, "focus"); guard(act.toggle, "toggle"); guard(act.stop, "stop");
	guard(act.prospect, "prospect"); guard(act.medic, "medic"); guard(act.rescue, "rescue"); guard(act.pause, "pause");
	guard(act.setSpeed, "speed"); guard(act.setActive, "squad"); guard(act.floor, "floor"); guard(act.moveSpeed, "move speed");
	guard(act.removeJob, "job remove"); guard(act.moveJob, "job move");
	guard(act.buildingButton, "building button"); guard(act.buildingTick, "building tick");
	app.hud = std::make_unique<ui::Hud>(std::move(act));
	app.hud->show(false);	// shown once the sim is live
	app.options = std::make_unique<ui::Options>();
	app.items = std::make_unique<ui::ItemWindows>(item_actions());
	app.items->holdTime = (float)kHoldMenu;
	app.stats = std::make_unique<ui::Stats>(ui::StatsActions{});	// toggle_stats (C / STA): update_ui fills it from CHAR_STATS each frame
	app.machineWin = std::make_unique<ui::BuildingWindow>(machine_actions());
	ui::MapActions map;
	map.draw = [](const ui::MapFrame& f) { app.wmap.drawArea(f.x, f.y, f.w, f.h, f.cx, f.cz, f.pxPerUnit); };
	map.moveTo = [](double x, double z, bool queue) { if (dev::mirrorLocked()) { printf("follow: hud map move locked (mirror lock)\n"); return; } const double at[3] = { x, app.hm->height(x, z), z }; const SimView::Click click{ "map" }; app.sim.moveTo(at, queue, false, &click); };	// the selection; no height (the map sees only the heightmap): the sim snaps by xz
	{
		// The minimap window (kenshi-k16.2.436; the original has none): its map image is the overview's, its zoom level and place are kept as raw settings
		// (not options rows) and restored here; the actions write them at once (a wheel step, a drag's end).
		ui::MinimapActions mma;
		mma.draw = map.draw;
		mma.level = [](int level) { cfg::setRaw("minimap.level", std::to_string(level)); app.minimapState.level = level; };
		mma.moved = [](float x, float y)
		{
			cfg::setRaw("minimap.x", std::to_string((int)std::lround(x)));
			cfg::setRaw("minimap.y", std::to_string((int)std::lround(y)));
			app.minimapState.x = x;
			app.minimapState.y = y;
		};
		app.minimap = std::make_unique<ui::Minimap>(std::move(mma));
		app.minimap->show(false);
		app.statusPopup = std::make_unique<ui::StatusPopup>(ui::StatusPopupActions{});	// kenshi-k16.2.445.3: server / link conditions, top right (update_ui feeds it)
		const auto saved = [](const char* key, float def) { const std::string v = cfg::getRaw(key); return v.empty() ? def : (float)atof(v.c_str()); };
		app.minimapState.level = std::clamp((int)saved("minimap.level", ui::Minimap::kDefaultLevel), 0, ui::Minimap::kLevels - 1);
		app.minimapState.x = saved("minimap.x", -1);
		app.minimapState.y = saved("minimap.y", -1);
	}
	app.overview = std::make_unique<ui::Overview>(ui::OverviewActions{}, std::move(map));
	app.squadsUi = std::make_unique<ui::Squads>(*app.overview, squads_actions());
	app.research = std::make_unique<ui::Research>(*app.overview, research_actions());
	app.crafting = std::make_unique<ui::Crafting>(*app.overview, ui::CraftingActions{});
	ui::FactionActions fa;
	fa.rename = [](const std::string& name) { app.sim.factionRename(name); };
	app.faction = std::make_unique<ui::Faction>(*app.overview, fa);
	app.prompt = std::make_unique<ui::Prompt>();
	app.progressBars = std::make_unique<ui::ProgressBars>();
	app.nameLabels = std::make_unique<ui::NameLabels>();
	app.speechBubbles = std::make_unique<ui::SpeechBubbles>();
	ui::DialogueActions dact;
	dact.choose = [](int i) { const auto& menus = app.sim.decoder().menus; if (!menus.empty()) { app.sim.reply(menus.back(), (uint8_t)i); } };
	dact.leave = [] { const auto& menus = app.sim.decoder().menus; if (!menus.empty()) { app.sim.endTalk(menus.back()); } };
	app.dialogue = std::make_unique<ui::Dialogue>(dact);
	ui::ProspectActions pa;
	pa.select = [](const std::string& id) { app.prospectRes = id; };
	app.prospect = std::make_unique<ui::Prospecting>(std::move(pa));
	ui::BuildActions ba;
	ba.close = [] { app.build->show(false); };
	ba.category = [](const std::string& id) { if (id != app.buildCategory) { app.buildCategory = id; app.buildGroup.clear(); } };
	ba.pick = [](const std::string& id) { app.buildGroup = id; app.buildStep = 0; };
	ba.step = [](int d) { app.buildStep += d; };	// wrapped by build_state
	app.build = std::make_unique<ui::Build>(std::move(ba));
	app.creatorUi = std::make_unique<ui::Creator>(creator_actions());
	if (app.creator.active()) { app.creatorUi->show(true); }
	init_menus();
	ui::keys::report();
}

// The over-head progress bars' level on the wire (order 98): sent after HELLO (a fresh session is at the sim's default 1), again after every
// SCENARIO_STARTED and NEW_GAME (the sim resets it to 1 there), and whenever the setting changes (the per-frame call). `force` for those events.
void progress_level_sync(bool force)
{
	if (!app.sim.active() || !app.greeted) { return; }
	const int level = progress_level();
	if (!force && level == app.progressLevelSent) { return; }
	if (app.sim.progressLevel((uint8_t)level)) { app.progressLevelSent = level; }
}

// The status popup (kenshi-k16.2.445.3): real server / link conditions only. The lockstep frame repeats never feed it (k16.2.437.4).
// 1. In play, not paused, speed > 0, and no new sim tick in the snapshots for over 1 s: "Server not responding" (warn, busy, seconds since the
//    last tick; error past 10 s). A load / zone-in the server reports is a later wire item: nothing is guessed here.
// 2. The link failed (socket closed or errored, SimView::error): "Connection to the server lost" (error). There is no reconnect yet.
static void status_popup_update()
{
	using Clock = std::chrono::steady_clock;
	static uint64_t lastTick = 0;
	static Clock::time_point since = Clock::now();
	static bool stalled = false, lost = false;
	ui::StatusPopupState st;
	const bool play = app.sim.active() && !title_shown();
	const auto& clk = app.sim.decoder().clock;
	const uint64_t tick = app.sim.decoder().lastCompleteTick;
	const auto now = Clock::now();
	const bool running = play && app.sim.error().empty() && tick > 0 && clk.valid && !clk.paused && clk.speed > 0;
	if (!running || tick != lastTick) { lastTick = tick; since = now; }	// a pause, a stop or a new tick restarts the wait
	const float wait = std::chrono::duration<float>(now - since).count();
	const bool nowLost = play && !app.sim.error().empty();
	if (nowLost)
	{
		st.lines.push_back({ "link", "Connection to the server lost", ui::StatusLine::Level::kError, false, -1 });
	}
	else if (running && wait > 1.0f)
	{
		st.lines.push_back({ "stall", "Server not responding", wait > 10.0f ? ui::StatusLine::Level::kError : ui::StatusLine::Level::kWarn, true, wait });
	}
	if (nowLost != lost) { fprintf(stderr, "kclient: status: %s\n", nowLost ? "connection to the server lost" : "connection restored"); lost = nowLost; }
	const bool nowStalled = !st.lines.empty() && st.lines[0].key == "stall";
	if (nowStalled != stalled) { fprintf(stderr, "kclient: status: server %s\n", nowStalled ? "not responding" : "responding again"); stalled = nowStalled; }
	if (app.minimap->shown()) { st.avoid = app.minimap->window()->screen(); }
	app.statusPopup->update(st);
}

void update_ui(double dt)
{
	cfg::poll();
	// The inventory order queue (kenshi-k16.20.14.2): the answers of the last frame, then the next order of each inventory. The windows closed is
	// the queue's end (the item stays pending otherwise, and a closed window's clicks must not keep going to the server).
	if (!app.items || (!app.itemsShown && app.conts.empty())) { gear_orders_clear(); }
	gear_orders_frame();
	progress_level_sync();	// ui.progress_bars changed: tell the sim (order 98), which resends the rows on the next tick
	// The pause / resume sounds (audio_load_pause.md 2): on the clock's pause bit (order 40) as we see it, edge-triggered; the reply-menu freeze
	// (bit 1) is not a pause, and the first clock of a session is not a flip.
	static int pauseSeen = -1;
	if (!app.sim.active() || !app.sim.decoder().clock.valid) { pauseSeen = -1; }
	else
	{
		const int paused = app.sim.clock().pauseBits & 1;
		if (pauseSeen >= 0 && paused != pauseSeen)
		{
			if (app.quietPauseTo != paused) { audiohook_event(paused ? "Pause_Game" : "Resume_Game"); }
			app.quietPauseTo = -1;
		}
		pauseSeen = paused;
	}
	if (!app.uiOn) { return; }
	app.options->update();
	const uint32_t prospectSerial = app.sim.decoder().prospectSerial;
	if (prospectSerial != app.prospectSeen)
	{
		app.prospectSeen = prospectSerial;
		app.prospectRes = "5";	// Water selected at open
		app.prospect->show(true);	// re-centred on every show
		const auto& p = app.sim.decoder().prospect;
		printf("prospect: %llu at %.0f %.0f: N %u, %zu deposits\n", (unsigned long long)p.id, p.x, p.z, p.n, p.deposits.size());
	}
	if (app.prospect->shown() && app.sim.decoder().prospect.id)
	{
		const std::string painted = std::to_string(prospectSerial) + "/" + app.prospectRes;
		if (painted != app.prospectPainted) { app.prospectPainted = painted; prospect_paint(); }
		app.prospect->update(prospect_state());
	}
	if (app.build->shown()) { app.build->update(build_state()); }
	app.hud->show(app.sim.active() && !title_shown() && !app.creatorUi->shown());	// the creator screen hides the game HUD (k16.17.5)
	if (app.sim.active() && !title_shown()) { app.session = true; }
	if (app.sim.active() && !app.greeted && app.sim.decoder().helloSeen())
	{
		app.greeted = true;
		app.sim.listSaves();
		progress_level_sync(true);	// a fresh session starts at the sim's default level: send ui.progress_bars' one
	}
	static int canContinue = -1;
	const bool saved = app.sim.active() && !app.sim.decoder().saves.empty();
	if (const int can = app.session && app.sim.active() ? 1 : saved ? 2 : app.local && !app.sim.active() ? 3 : 0; can != canContinue)
	{
		canContinue = can;
		app.mainMenu->setContinue(can == 1 || can == 2, can == 3 ? "The game server is starting." : can == 0 ? "No game is running and nothing is saved." : "");
	}
	app.creatorUi->update(creator_state());
	if (app.sim.decoder().startsSerial != app.seen.starts)	// STARTS: the panel lists the start-offs this server can place now (MINE: the original has every start)
	{
		app.seen.starts = app.sim.decoder().startsSerial;
		const simlink::SimLinkDecoder& d = app.sim.decoder();
		std::vector<ui::StartEntry> v;
		for (const ui::StartEntry& e : app.starts)
		{
			if (std::any_of(d.starts.begin(), d.starts.end(), [&](const auto& s) { return s.ready && d.str(s.startoff) == e.id; })) { v.push_back(e); }
		}
		app.newGame->setStarts(v);
		if (app.scripted())	// the creator's CLOTHES source (k16.2.107): each member's drawn outfit, the WIRE's strids resolved to record names
		{
			printf("starts: %zu", d.starts.size());
			for (const simlink::SimLinkDecoder::Start& s : d.starts)
			{
				printf(" \"%s\"%s:", d.str(s.startoff).c_str(), s.ready ? "" : " (not ready)");
				for (const simlink::SimLinkDecoder::Member& m : s.members)
				{
					printf(" [%s armour:", d.str(m.character).c_str());
					for (uint32_t a : m.armour) { printf(" %s", app.chars.nameOf(d.str(a)).c_str()); }
					printf(" weapon:%s]", m.weapon != 0 ? app.chars.nameOf(d.str(m.weapon)).c_str() : "-");
				}
			}
			printf("\n");
		}
	}
	if (app.sim.decoder().savesSerial != app.seen.saves)	// SAVES: the panel's list (date local; details from the save's header block)
	{
		app.seen.saves = app.sim.decoder().savesSerial;
		std::vector<ui::SaveEntry> v;
		for (const simlink::SaveInfo& s : app.sim.decoder().saves)
		{
			const std::time_t t = (std::time_t)s.mtime;
			char date[32], hour[16];
			std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", std::localtime(&t));
			snprintf(hour, sizeof hour, "%02d:%02d", (int)s.hour, (int)((s.hour - (int)s.hour) * 60));
			std::string info = s.status == 2 ? "Unreadable or from a newer version\n\n" : "";
			info += "Day\t" + std::to_string(s.day) + ", " + hour + "\nFaction\t" + s.factionName + "\nLeader\t" + s.leaderName + "\nMembers\t" + std::to_string(s.members);
			v.push_back({ s.name, t, date, info });
		}
		app.saves->setSaves(std::move(v));
	}
	if (app.sim.decoder().resetSerial != app.seen.reset)	// a load replaced the world: nothing of the old one stays addressed
	{
		app.seen.reset = app.sim.decoder().resetSerial;
		begin_session_load();
		app.loadPending = true;	// a load's squad placement is the load's target in the first_world_frame log (k16.2.112.1)
		// k16.2.134.1: every reset starts a fresh camera restore (the last one's `applied` must not outlive its session placement); a load this
		// client asked for arms its sidecar, and the first snapshot after the reset applies it (camera_restore_frame)
		app.camRestore = CamRestore();
		app.mapViewPending = true;	// k16.2.435: this world starts at the default map view unless its sidecar brings one (camera_restore_frame)
		if (!app.camLoadName.empty() && app.frames - app.camLoadFrame < 1800)
		{
			app.camRestore.armed = true;
			app.camRestore.snapAt = app.sim.decoder().snapSerial;
			app.camRestore.name = app.camLoadName;
		}
		app.camLoadName.clear();
		gear_orders_clear();	// the inventories it queued against are gone (k16.20.14.2)
		set_follow(false);
		app.centerId = 0;
		app.rtsDist = 150;	// the original's distance after a load (kinspect camera_rts.md)
		app.machine = 0;
		app.panelProducer = app.focusLogged = app.rowLoggedId = 0;	// the new world's focus panel starts on the character (k16.19.8.2)
		cont_close_all();
		app.lastSelect = 0;
		if (app.newGameStart.empty())
		{
			app.worldChosen = true;	// a load (save or scenario): the world the player asked for replaced the old one (kenshi-k16.2.95)
			ui::message("Game loaded.");
		}
		else
		{
			// the fresh world's STARTS came in the same write: the start-off's strid is current
			const simlink::SimLinkDecoder& d = app.sim.decoder();
			const auto s = std::find_if(d.starts.begin(), d.starts.end(), [&](const auto& s) { return d.str(s.startoff) == app.newGameStart; });
			if (s != d.starts.end() && app.sim.newGame(kNewGameFaction, s->startoff, app.newGameSpecs)) { app.newGameSent = true; }
			else { ui::message("New game: the start is gone from the server's list."); }
			app.newGameStart.clear();
		}
	}
	if (app.sim.decoder().newGameSerial != app.seen.newGame)	// NEW_GAME_RESULT: our faction, or why not
	{
		app.seen.newGame = app.sim.decoder().newGameSerial;
		const auto& r = app.sim.decoder().newGame;
		if (app.newGameSent && r.result == 0) { app.worldChosen = true; }	// BEGIN's world is in: the wire may now say who plays (kenshi-k16.2.95)
		if (r.result == 0) { progress_level_sync(true); }	// NEW_GAME resets the sim's TASK_PROGRESS level to 1
		if (app.newGameSent && r.result == 0 && !r.members.empty()) { app.newGameLeader = r.members[0]; }
		if (app.newGameSent && r.result == 0 && r.hasPos)
		{
			teleport(r.pos[0], r.pos[2]);	// the members stream once the camera's interest covers them
			set_camera_target("newgame");	// the new game's start position is the camera's target (k16.2.112.1)
		}
		if (r.result != 0) { ui::message("New game refused: " + app.sim.decoder().str(r.reason)); }
		printf("sim new game: result %u%s%s, faction %llu, %zu members\n", r.result, r.result ? " " : "", r.result ? app.sim.decoder().str(r.reason).c_str() : "", (unsigned long long)r.faction, r.members.size());
		app.newGameSent = false;
	}
	dev::poll();
	if (app.newGameLeader && app.sim.decoder().chars.count(app.newGameLeader))	// the members came with the next snapshot: look at the leader
	{
		app.sim.select(app.newGameLeader, false);
		set_rts(app.newGameLeader);
		app.newGameLeader = 0;
	}
	// Spectator (no player faction): the watch (the inspect focus) can be followed, and --follow NAME|id=N starts the follow camera on it once it streams in.
	// Spectating also comes from the wire: the player's own characters are always sent, wherever the interest is (SIMLINK 5), so a complete
	// snapshot with none on it is a session nobody controls. (PLAYER_SQUADS is sent only with a player faction, so it cannot say this alone.)
	// The wire's answer is followed both ways. A joined session's is always the wire's. A title-started (local) server streams its pre-game world
	// with no player before the player chooses one, which is not a spectator session: there the wire is asked only once a world is chosen (a NEW_GAME
	// result, a scenario started or a load), which still lets a local session play a player-less spectate_* scenario (kenshi-k16.2.95: CAST, the
	// watch and the empty squad bar leaked into a plain single-player title run, and stayed for the game it started).
	if (app.sim.active() && (!app.local || app.worldChosen) && app.sim.decoder().lastCompleteTick != 0)
	{
		bool anyPlayer = false;
		for (const auto& [id, ch] : app.sim.decoder().chars) { if (ch.cur.flags & simlink::kPlayer) { anyPlayer = true; break; } }
		app.spectating = !anyPlayer || (app.sim.decoder().playerSquads.valid && app.sim.decoder().playerSquads.squads.empty());
	}
	if (app.sim.active() && !app.opt.follow.empty())
	{
		const uint64_t id = named_character(app.opt.follow);
		if (id && set_follow_id(id, true))	// --follow: a dev/test-only route (a non-squad target needs --debug / a script)
		{
			app.inspect = id;	// the watch: its CHAR_STATS (order 87) carries the goal for the status panel; spectating stays the wire's call (a session
								// with a player character is not a spectator's, even while a non-player one is followed)
			printf("follow: %llu \"%s\"\n", (unsigned long long)id, app.opt.follow.c_str());
			app.opt.follow.clear();
		}
	}
	// Zone banner (BiomeTextPanel) when the camera's BIOME_GROUP changes in game; the save keeps the area by the CAMERA (`string area`). UNSURE: the camera or
	// the squad, and whether loading a game shows it. Groups named NONE show nothing. None over the main menu: the zone is kept, so it shows when the game starts.
	if (app.sim.active() && !app.creatorUi->shown() && !title_shown())
	{
		const std::string& z = app.wx.atlas().groupAt(app.pos[0], app.pos[2]);
		if (z != app.zone && !z.empty() && z != "NONE")
		{
			ui::banner(z);
			if (app.scripted()) { printf("banner: %s\n", z.c_str()); }
		}
		if (!z.empty()) { app.zone = z; }
	}
	if (app.sim.active())
	{
		ui::HudState st;
		const auto& c = app.sim.clock();
		st.clockValid = c.valid;
		st.day = (uint32_t)c.day;
		st.hour = (float)c.hour;
		st.speed = (float)c.speed;
		st.driving = app.follow && app.sim.isPlayer(app.followId);	// direct drive: both diamonds draw active (k16.3.32; replaces k16.2.238.1's host-drawn DRIVING tag)
		st.paused = c.pauseBits & 1;	// the player's pause only: the dialogue freeze (bit 1) shows no PAUSED banner (the original's look for it is untraced; deferred)
		if (app.sim.decoder().playerSquads.hasMoney) { st.money = app.sim.decoder().playerSquads.money; }
		const auto& sel = app.sim.selected();
		std::vector<uint64_t> ids;
		for (const auto& [id, ch] : app.sim.decoder().chars) { if (ch.cur.flags & simlink::kPlayer) { ids.push_back(id); } }
		std::sort(ids.begin(), ids.end());
		// PLAYER_SQUADS: a tab per squad with members (one stays shown), the bar shows the active one's in squad order; without it every player character
		std::vector<uint64_t>& bar = app.bar;
		bar = ids;
		const simlink::PlayerSquads& ps = app.sim.decoder().playerSquads;
		if (ps.valid)
		{
			const simlink::PlayerSquads::Squad* active = nullptr;
			for (const simlink::PlayerSquads::Squad& q : ps.squads)
			{
				if (q.members.empty()) { continue; }
				st.squadTabs.push_back({ q.id, q.name });
				if (q.id == app.activeSquad || !active) { active = &q; }
			}
			app.activeSquad = active ? active->id : 0;
			st.activeSquad = app.activeSquad;
			bar.clear();
			if (active) { for (uint64_t id : active->members) { if (app.sim.decoder().chars.count(id)) { bar.push_back(id); } } }
		}
		// a character's name, health, medical rows and HealthText: a squad portrait's, or the inspect focus's
		const auto member = [&](uint64_t id)
		{
			const uint32_t f = app.sim.decoder().chars.at(id).cur.flags;
			ui::HudMember m;
			m.id = id;
			m.name = app.sim.decoder().str(app.sim.decoder().chars.at(id).look.name);
			m.down = f & (simlink::kKO | simlink::kDead);
			const ui::PortraitLayers layers = portrait_layers(id);
			m.mark = layers.mark;
			m.back = layers.back;
			m.dim = layers.dim;
			m.hurt = layers.hurt;
			m.health = f & simlink::kDead ? 0.0f : app.sim.health(id, app.chars);
			if (const simlink::Medical* med = app.sim.medical(id))
			{
				const std::string race = app.chars.nameOf(app.sim.decoder().str(app.sim.decoder().chars.at(id).look.race));
				for (const simlink::Medical::Part& p : med->parts)
				{
					if (m.parts.size() == 9) { break; }
					const std::string part = app.sim.decoder().str(p.part);
					const float max = p.max >= 0 ? p.max : app.chars.partMaxHealth(race, part);
					const uint8_t state = p.limb & simlink::Medical::kLimbState;
					m.parts.push_back({ part, p.health, p.stun, max > 0 ? max : 100.0f, p.bandage, p.splint, (p.limb & simlink::Medical::kLimbRobot) != 0, state == simlink::Medical::kLimbStump || state == simlink::Medical::kLimbCrushed });
				}
				m.robot = !m.parts.empty() && std::all_of(m.parts.begin(), m.parts.end(), [](const ui::HudPart& hp) { return hp.robot; });
				// the original's row order (capture 20261103004059_1); parts outside it (animals) keep the wire order after these
				static const char* const kOrder[] = { "Head", "Stomach", "Chest", "Left Arm", "Right Arm", "Left Leg", "Right Leg" };
				const auto rank = [](const ui::HudPart& hp) { return (int)(std::find_if(std::begin(kOrder), std::end(kOrder), [&](const char* n) { return hp.name == n; }) - std::begin(kOrder)); };
				std::stable_sort(m.parts.begin(), m.parts.end(), [&](const ui::HudPart& a, const ui::HudPart& b) { return rank(a) < rank(b); });
				m.blood = med->blood;
				m.bloodMax = med->bloodMax;
				m.bleed = med->bleed;
				m.hunger = med->hunger;
				m.fed = med->flags & simlink::Medical::kDigesting;
				m.resting = med->flags & simlink::Medical::kInBed;
			}
			// HealthText, first match wins (kinspect findings/ui_captures/hud_panel16.md 2); only the states the wire carries: no KO
			// seconds, cripples or disguise yet (Discovered! / Disguise Blown! are disguise rows, not stealth: sneak detection shows in the
			// Stealth row). The tip is the winning state's sentence, Wounded and Slightly Injured keeping the default
			// (findings/hud_tooltips.md 1); our wording
			const char* const kFine = "Healthy enough, for now.";
			if (f & simlink::kDead) { m.status = "Dead"; m.statusColour = "HudBad"; m.statusTip = "This one is gone."; }
			else if (f & simlink::kKO) { m.status = "Unconscious"; m.statusColour = "HudBad"; m.statusTip = "Knocked out; back up once they come round."; }
			else if (m.hunger >= 0 && m.hunger < 1) { m.status = "Starving"; m.statusColour = "HudBad"; m.statusTip = "Starving: find food soon, or they collapse."; }
			else if (m.bleed > 0) { m.status = "Bleeding"; m.statusColour = "HudBad"; m.statusTip = "Losing blood; they pass out if it runs dry."; }
			else if (m.health < 0.7f) { m.status = "Wounded"; m.statusTip = kFine; }
			else if (m.hunger >= 0 && m.hunger < 2 && !m.fed) { m.status = "Hungry"; m.statusTip = "Hungry and losing strength: time to eat."; }
			else if (m.hunger >= 0 && m.hunger < 2) { m.status = "Malnourished"; m.statusTip = "Underfed, but eating now; on the mend."; }
			else if (m.health < 0.9f) { m.status = "Slightly Injured"; m.statusTip = kFine; }
			else { m.status = "Normal"; m.statusTip = kFine; }
			return m;
		};
		for (uint64_t id : bar)
		{
			const uint32_t f = app.sim.decoder().chars.at(id).cur.flags;
			ui::HudMember m = member(id);
			m.selected = std::find(sel.begin(), sel.end(), id) != sel.end();
			m.toggles[(int)ui::HudToggle::kSneak] = f & simlink::kStealth;
			m.toggles[(int)ui::HudToggle::kBlock] = f & simlink::kBlockMode;
			m.toggles[(int)ui::HudToggle::kRun] = f & simlink::kRun;
			m.toggles[(int)ui::HudToggle::kHold] = f & simlink::kHoldPosition;
			m.toggles[(int)ui::HudToggle::kPassive] = f & simlink::kPassive;
			const auto jobs = app.sim.decoder().jobs.find(id);
			m.toggles[(int)ui::HudToggle::kJobs] = jobs == app.sim.decoder().jobs.end() || jobs->second.enabled;
			uint32_t version = 0;
			const int index = app.sim.charIndex(id, &version);
			// the sim's portrait_serial (protocol 5); MINE: a stand-in from the id without it
			const uint32_t serial = app.sim.decoder().chars.at(id).look.portraitSerial;
			if (app.portraitTable.get(id, index, version, serial ? serial : (uint32_t)(id * 2654435761u), m.portraitUv))
			{
				m.portrait = app.portraitsTex;
			}
			st.squad.push_back(std::move(m));
		}
		// the status panel: the selection's first player character (the medical panel's), else the bar's first
		uint64_t infoId = 0;
		for (uint64_t id : sel) { if (std::find(bar.begin(), bar.end(), id) != bar.end()) { infoId = id; break; } }
		if (!infoId && !bar.empty()) { infoId = bar.front(); }
		st.jobsValid = infoId != 0;	// the status panel's character's (SIMLINK JOBS); no entry: none (the list shows "[No jobs assigned]")
		st.toggleEnabled[(int)ui::HudToggle::kJobs] = true;
		st.toggleEnabled[(int)ui::HudToggle::kHold] = true;	// order 10 which 6 / 7 (ksim 44025812, master 90b7d0eb; kenshi-k16.2.295.1)
		st.toggleEnabled[(int)ui::HudToggle::kPassive] = true;
		st.medicEnabled = true;
		app.jobsChar = infoId;
		if (const auto j = app.sim.decoder().jobs.find(infoId); j != app.sim.decoder().jobs.end())
		{
			for (const simlink::SimLinkDecoder::JobRow& row : j->second.rows) { st.jobs.push_back({ row.jobId, job_text(row), !row.active }); }
		}
		const WorldBuildings::FloorStatus fs = app.buildings.floorStatus();
		st.floorValid = true;	// always shown (user 2026-11-04: it went away on the way to click it); no target: both arrows grey
		const int level = app.buildings.level();	// no building to act on: the global level 0..3 (always steppable, the original's selector)
		app.floorShown = fs.valid ? fs.shown : level;
		st.floor = app.floorShown;
		st.floorName = fs.name;
		st.floorUp = fs.valid ? fs.shown < fs.top : level < 3;
		st.floorDown = fs.valid ? fs.shown > 0 : level > 0;
		st.moveSpeed = move_speed();
		// Order 87: one focus per session, the last clicked thing (k16.19.8.2): a producer's id while the panel shows it, else the character focus, else 0
		// for the selection. The scripted lines prove the panel's source headless: `focus <id> kind producer|character` once per change, `producer row ...`
		// when the row (or its version) changes.
		const simlink::Interactable* panelThing = app.panelProducer ? app.sim.thing(app.panelProducer) : nullptr;
		const bool producerFocus = panelThing && panelThing->kind == simlink::Interactable::kProducer;	// a storage's panel has no BUILDING row: order 87 would be bad_target
		const uint64_t focusId = producerFocus ? app.panelProducer : app.inspect;
		app.sim.focus(focusId, producerFocus);
		if (app.scripted() && (focusId != app.focusLogged || producerFocus != app.focusLoggedProducer))
		{
			printf("focus %llu kind %s\n", (unsigned long long)focusId, producerFocus ? "producer" : "character");
			app.focusLogged = focusId;
			app.focusLoggedProducer = producerFocus;
		}
		if (app.scripted())
		{
			const simlink::SimLinkDecoder::Building* row = app.sim.focusBuilding();
			if (row && (row->id != app.rowLoggedId || row->version != app.rowLoggedVersion))
			{
				printf("producer row %llu: status %u workers %u/%u efficiency %.2f maxEfficiency %.2f hp %.1f/%.1f quality %.2f\n", (unsigned long long)row->id,
					(unsigned)row->status, (unsigned)row->workers, (unsigned)row->maxWorkers, (double)row->efficiency, (double)row->maxEfficiency, (double)row->hp,
					(double)row->maxHp, (double)row->quality);
				app.rowLoggedId = row->id;
				app.rowLoggedVersion = row->version;
			}
			else if (!row) { app.rowLoggedId = 0; app.rowLoggedVersion = 0; }	// the next row for anyone prints again
		}
		if (app.panelProducer && !app.sim.thing(app.panelProducer)) { app.panelProducer = 0; }	// the producer left interest: the panel goes back to the character
		// Spectator (no player faction, docs/UI.md "Spectator mode"): the watch is the inspect focus, so the name plate and the Goal row below
		// are the followed character's (CHAR_STATS via order 87); the CAST rows are every character on the wire, down dimmed.
		st.spectating = app.spectating;
#ifdef KCLIENT_DEV_TOOLS
		dev::hudState(st.playtestRun, st.notesOpen);
#endif
		if (app.spectating)
		{
			st.watch.id = app.inspect;
			st.watch.following = app.inspect && app.follow && app.followId == app.inspect;
			const auto watched = app.sim.decoder().chars.find(app.inspect);
			if (app.inspect && watched != app.sim.decoder().chars.end())
			{
				st.watch.name = app.sim.decoder().str(watched->second.look.name);
				st.watch.faction = app.chars.nameOf(app.sim.decoder().str(watched->second.look.faction));
				if (const simlink::CharStats* cs = app.sim.charStats(app.inspect))
				{
					st.watch.goal = cs->goal ? app.sim.decoder().str(cs->goal) : "";
					if (st.watch.goal.empty()) { st.watch.goal = "Aimless"; }	// an empty goal strid is Aimless, as the status panel reads it
					if (cs->hasAi)	// the AI tail: the primitive task the goal runs, its plan step and its target (rows 16-20); empty: the panel's "-"
					{
						std::string task;
						if (const char* name = aiTaskName(cs->goalEnum)) { task = name; }
						else if (cs->goalEnum == simlink::CharStats::kNoTaskValue) { task = "none"; }
						else { task = "task #" + std::to_string(cs->goalEnum); }
						if (const char* step = aiTaskName(cs->stepEnum)) { task += "  step " + std::string(step) + " " + std::to_string(cs->stepIndex) + "/" + std::to_string(cs->stepCount); }
						if (cs->targetId)
						{
							const auto target = app.sim.decoder().chars.find(cs->targetId);
							const std::string targetName = target != app.sim.decoder().chars.end() ? app.sim.decoder().str(target->second.look.name) : "";
							task += "  target " + (targetName.empty() ? "#" + std::to_string(cs->targetId) : targetName);
						}
						else if (std::isfinite(cs->targetX) && std::isfinite(cs->targetZ))
						{
							char point[64];
							snprintf(point, sizeof point, "  at %.0f %.0f", cs->targetX, cs->targetZ);
							task += point;
						}
						st.watch.task = task;
					}
				}
			}
			for (const auto& [id, ch] : app.sim.decoder().chars)
			{
				st.cast.push_back({ id, app.sim.decoder().str(ch.look.name), app.chars.nameOf(app.sim.decoder().str(ch.look.faction)), (ch.cur.flags & (simlink::kKO | simlink::kDead)) != 0 });
			}
		}
		if (app.inspect)	// the jobs above stay the selection's; unloaded: both panels as empty frames
		{
			const bool loaded = app.sim.decoder().chars.count(app.inspect);
			st.focusSet = true;
			if (loaded) { st.focus = member(app.inspect); }
			st.info.valid = st.info.blank = !loaded;
			infoId = loaded ? app.inspect : 0;
		}
		app.infoChar = infoId;	// the cheat panel's character: the status panel's, 0 = none (the server's first player character)
		if (infoId)
		{
			const simlink::Character& ch = app.sim.decoder().chars.at(infoId);
			st.info.valid = true;
			st.info.name = app.sim.decoder().str(ch.look.name);
			st.info.faction = app.chars.nameOf(app.sim.decoder().str(ch.look.faction));
			if (const simlink::CharStats* cs = app.sim.charStats(infoId)) { stats_info(*cs, st.info, app.sim.isPlayer(infoId)); }
		}
		// the stats window's character: the status panel's, unless the window is pinned (it then stays on the one it was opened on)
		if (infoId && (!app.stats->shown() || !app.stats->pinned())) { app.statsChar = infoId; }
		if (app.stats->shown())
		{
			ui::StatsState ss;
			stats_window(app.statsChar, ss);
			app.stats->update(ss);
		}
		if (app.panelProducer) { st.building = machine_panel(); }	// the last clicked producer: its lines replace the character's (k16.19.7.1, k16.19.8.2)
		fill_key_hints(st.keys, st.squad.size());
		app.hud->update(st);
		{	// the hints panel (ui.control_hints): the hovered portrait's list; the world's targets feed it once they migrate (empty for now)
			ui::HintBlock block;
			if (ui::Widget* over = ui::wantsMouse() ? ui::hoveredWidget() : nullptr)
			{
				for (size_t i = 0; i < st.squad.size() && block.lines.empty(); i++)
				{
					const ui::Widget* frame = app.hud->portrait(i);
					for (const ui::Widget* w = over; w && frame; w = w->parent())
					{
						if (w != frame) { continue; }
						const hints::Context ctx = hints_context();
						const hints::Hover h = hints::hints_for_portrait(st.squad[i].id, ctx);
						block.title = h.target;
						for (const hints::Action& act : h.actions) { block.lines.push_back({ hints::gesture_name(act.gesture), act.label, act.enabled, act.why }); }
						break;
					}
				}
			}
			ui::setHints(block);
		}
		app.squadsUi->update(squads_state());
#ifdef KCLIENT_DEV_TOOLS
		dev::trailsPanelFrame(dev_context());
		dev::animBrowserFrame();	// the animation lab's window: its view of the lab (dev/debughost.cpp)
#endif
		if (app.research->shown()) { app.research->update(research_state()); }
		if (app.crafting->shown()) { app.crafting->update(ui::CraftingState{}); }	// no benches: the sim has no crafting yet
		if (app.faction->shown())
		{
			// the name from PLAYER_SQUADS, the rows from FACTION_RELATIONS (38) in the sim's order; INFORMATION (contracts, campaigns) is not on the wire
			const simlink::SimLinkDecoder& d = app.sim.decoder();
			ui::FactionState fs;
			fs.name = d.str(d.playerSquads.factionName);
			for (const auto& r : d.relations) { fs.relations.push_back({ d.str(r.faction), r.name, r.value }); }
			app.faction->update(fs);
		}
		if (app.scripted() && app.sim.decoder().seenBySerial != app.seen.seenBy)	// SEEN_BY (59): each sneaking character's non-allied observers that know it, with the frame's tick
		{
			app.seen.seenBy = app.sim.decoder().seenBySerial;
			const auto& sb = app.sim.decoder().seenBy;
			printf("seen_by: %zu sneaking", sb.size());
			for (const auto& [sneaker, set] : sb)
			{
				printf(" \"%s\" (%llu) tick %llu knows", app.sim.decoder().str(app.sim.decoder().chars.count(sneaker) ? app.sim.decoder().chars.at(sneaker).look.name : 0).c_str(),
					(unsigned long long)sneaker, (unsigned long long)set.tick);
				for (uint64_t o : set.by) { printf(" %llu", (unsigned long long)o); }
			}
			printf("\n");
		}
		if (app.scripted() && app.sim.decoder().detectedBySerial != app.seen.detectedBy)	// DETECTED_BY (60): each sneaking character's detection records (observer:state[friendly])
		{
			app.seen.detectedBy = app.sim.decoder().detectedBySerial;
			const auto& db = app.sim.decoder().detectedBy;
			printf("detected_by: %zu sneaking", db.size());
			for (const auto& [sneaker, set] : db)
			{
				printf(" \"%s\" (%llu) tick %llu:", app.sim.decoder().str(app.sim.decoder().chars.count(sneaker) ? app.sim.decoder().chars.at(sneaker).look.name : 0).c_str(),
					(unsigned long long)sneaker, (unsigned long long)set.tick);
				for (const simlink::DetectedBy::Entry& e : set.by) { printf(" %llu:%u%s", (unsigned long long)e.observer, e.state, e.friendly ? "f" : ""); }
			}
			printf("\n");
		}
		if (app.scripted() && app.sim.decoder().playerSquadsSerial != app.seen.squads)
		{
			app.seen.squads = app.sim.decoder().playerSquadsSerial;
			const simlink::PlayerSquads& ps = app.sim.decoder().playerSquads;
			printf("squads: %s %u/%u", app.sim.decoder().str(ps.factionName).c_str(), ps.factionCount, ps.maxFaction);
			for (const auto& q : ps.squads)
			{
				printf(" | %s:", q.name.c_str());
				for (uint64_t id : q.members) { printf(" %s", app.sim.decoder().chars.count(id) ? app.sim.decoder().str(app.sim.decoder().chars.at(id).look.name).c_str() : std::to_string(id).c_str()); }
			}
			printf("\n");
		}
		static int hudFrames = 0;
		static const bool hudLog = framelog::on("hud");
		if (hudLog && app.scripted() && ++hudFrames % 300 == 0)
		{
			for (const ui::HudMember& m : st.squad)
			{
				printf("hud: %s health %.2f%s", m.name.c_str(), m.health, m.down ? " down" : "");
				if (const simlink::Detect* d = app.sim.detect(m.id)) { printf(" detect %s %.2f by %llu", d->state == simlink::Detect::kNoticed ? "noticed" : "noticing", d->meter, (unsigned long long)d->by); }
				if (const simlink::Medical* med = app.sim.medical(m.id))
				{
					const std::string race = app.chars.nameOf(app.sim.decoder().str(app.sim.decoder().chars.at(m.id).look.race));
					for (const simlink::Medical::Part& p : med->parts)
					{
						printf(" %s %.0f/%.0f", app.sim.decoder().str(p.part).c_str(), p.health, p.max >= 0 ? p.max : app.chars.partMaxHealth(race, app.sim.decoder().str(p.part)));
						if (p.stun > 0) { printf(" stun %.0f", p.stun); }
						if (p.bandage > 0 || p.splint > 0 || p.limb) { printf(" (bandage %.1f splint %.1f limb %u)", p.bandage, p.splint, p.limb); }
					}
					printf(" blood %.1f/%.1f bleed %.3f hunger %.2f flags %u%s%s", med->blood, med->bloodMax, med->bleed, med->hunger, med->flags,
						med->flags & simlink::Medical::kHurt ? " hurt" : "", med->flags & simlink::Medical::kPutIn ? " putIn" : "");
				}
				printf("\n");
			}
		}
		// The item windows (kenshi-k16.20.13.2). A box or body asked for by a click (app.pending) opens once a selected player character is within
		// kContainerOpen of its use point (a body's feet; its INVENTORY received); a window then lives until it is closed, its thing goes, or a new
		// move order of its user ends it (never the distance: the original keeps the windows at any range, rwh.78; a pinned one survives the order).
		const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
		if (!trade.valid)
		{
			if (app.tradeStock) { cont_close(app.tradeStock); }
			app.tradeStock = 0;
			app.tradeClosing = false;
		}
		else if (app.tradeStock && !cont_find(app.tradeStock))
		{
			printf("trade: %llu closed by the client\n", (unsigned long long)trade.trader);
			app.sim.tradeClose();
			app.tradeStock = 0;
			app.tradeClosing = true;
		}
		else if (!app.tradeStock && !app.tradeClosing && app.sim.isPlayer(trade.actor))
		{
			App::OpenCont c;
			c.id = trade.stock;
			c.user = trade.actor;
			c.trade = true;
			cont_add(c);
			app.tradeStock = trade.stock;
			app.itemsShown = true;
			printf("trade: %llu opened by %llu, stock %llu:", (unsigned long long)trade.trader, (unsigned long long)trade.actor, (unsigned long long)trade.stock);
			if (const simlink::Inventory* inv = app.sim.inventory(trade.stock)) { for (const auto& e : inv->items) { printf(" %s x%u @%u,%u", app.sim.nameOf(e.item, app.chars).c_str(), e.count, e.x, e.y); if (e.steal != 255) { printf(" steal %u%%", (unsigned)e.steal); } } }
			printf("\n");
		}
		const auto is_up = [&](uint64_t id) { const auto ch = app.sim.decoder().chars.find(id); return ch != app.sim.decoder().chars.end() && !(ch->second.cur.flags & (simlink::kKO | simlink::kDead)); };
		// the windows that end: the thing gone (a body that got up), the looter down, or a new order of the user
		for (const uint64_t id : [&] { std::vector<uint64_t> v; for (const App::OpenCont& c : app.conts) { v.push_back(c.id); } return v; }())
		{
			const App::OpenCont* c = cont_find(id);
			if (!c || c->trade) { continue; }
			const char* why = nullptr;
			if (c->body) { const auto ch = app.sim.decoder().chars.find(id); if (ch == app.sim.decoder().chars.end() || !(ch->second.cur.flags & (simlink::kKO | simlink::kDead))) { why = "gone"; } }
			else if (!app.sim.thing(id)) { why = "gone"; }
			if (!why && c->user && !c->pinned)
			{
				if (app.sim.taskOrder(c->user) != c->task) { why = "ordered"; }
				else if (c->body && !is_up(c->user)) { why = "looter down"; }
			}
			if (why)
			{
				if (c->user) { printf("container: %llu closed (%s)\n", (unsigned long long)id, why); }
				cont_close(id);
			}
		}
		if (app.pending.id && !app.pending.body && app.pending.lead)	// a box's open ends with the next order of its walker: it opens nothing later
		{
			App::PendingOpen& po = app.pending;
			const uint32_t cur = app.sim.taskOrder(po.lead);
			if (cur != po.task)
			{
				printf("open: order for %llu dropped (walker %llu ordered on, task %u -> %u)\n", (unsigned long long)po.id, (unsigned long long)po.lead, (unsigned)po.task, (unsigned)cur);
				po = {};
			}
		}
		if (app.pending.id)
		{
			const App::PendingOpen pd = app.pending;
			const simlink::Interactable* box = pd.body ? nullptr : app.sim.thing(pd.id);
			const bool producer = box && box->kind == simlink::Interactable::kProducer;
			double spot[3] = {};
			bool there = box != nullptr;
			if (box) { std::copy(producer ? box->pos : box->use, (producer ? box->pos : box->use) + 3, spot); }
			else if (pd.body)
			{
				const auto ch = app.sim.decoder().chars.find(pd.id);
				double yaw;
				const simlink::Route* route;
				there = ch != app.sim.decoder().chars.end() && (ch->second.cur.flags & (simlink::kKO | simlink::kDead)) && app.sim.pose(pd.id, spot, yaw, route);
			}
			if (!there) { app.pending = {}; }
			else if (!pd.body || app.sim.inventory(pd.id))	// a body opens only for a character given the loot order: anyone else would end the loot at once
			{
				uint64_t user = 0;
				for (uint64_t id : sel)
				{
					double at[3], yaw;
					const simlink::Route* route;
					if (!app.sim.isPlayer(id) || !is_up(id) || !app.sim.pose(id, at, yaw, route)) { continue; }
					if (std::hypot(at[0] - spot[0], at[2] - spot[2]) > (producer ? kProducerOpen : kContainerOpen)) { continue; }
					if (pd.body && app.sim.taskOrder(id) != pd.lootTask) { continue; }
					user = id;
					break;
				}
				if (user)
				{
					App::OpenCont c;
					if (const App::OpenCont* had = cont_find(pd.id)) { c = *had; }
					c.id = pd.id;
					c.user = user;
					c.view = false;
					c.body = pd.body;
					c.task = pd.body ? pd.lootTask : app.sim.taskOrder(user);
					cont_add(c);
					app.pending = {};
					app.itemsShown = true;
					const std::string name = box ? app.sim.nameOf(box->record, app.chars) : app.sim.decoder().str(app.sim.decoder().chars.at(pd.id).look.name);
					printf("%s: %llu %s opened by %llu (task %u):", box ? "container" : "body", (unsigned long long)pd.id, name.c_str(), (unsigned long long)user, (unsigned)c.task);
					if (const simlink::Inventory* inv = app.sim.inventory(pd.id)) { for (const auto& e : inv->items) { printf(" %s x%u", app.sim.nameOf(e.item, app.chars).c_str(), e.count); if (e.steal != 255) { printf(" steal %u%%", (unsigned)e.steal); } } }
					printf("\n");
				}
			}
		}
		// the character window shows the first open window's user, else the first selected player character, else the lowest player id
		const auto own = std::find_if(sel.begin(), sel.end(), [&](uint64_t id) { return app.sim.isPlayer(id); });
		uint64_t cu = 0;
		for (const App::OpenCont& c : app.conts) { if (c.user) { cu = c.user; break; } }
		// pinned, it keeps its owner as the selection changes (kinspect loot.md, Kenshi_PinButtonSkin). A loot / container open still retargets it:
		// the original's open clears every inventory window first (loot.md, s55_static Q3)
		const uint64_t pick = cu ? cu : own != sel.end() ? *own : ids.empty() ? 0 : ids.front();
		app.gearPinned = app.charPinned && !cu ? (app.gearPinned ? app.gearPinned : pick) : 0;
		if (app.machine && !app.sim.thing(app.machine)) { app.machine = 0; }
		app.machineWin->update(machine_state());
		app.gearChar = app.itemsShown ? app.gearPinned ? app.gearPinned : pick : 0;	// the machine drop's source character (k16.19.8.2)
		ui::ItemWindowsState iws;
		if (app.itemsShown) { const ui::GearState g = gear_state(app.gearChar); if (g.valid) { iws.characters.push_back(g); } }
		for (const App::OpenCont& c : app.conts)
		{
			if (c.body)
			{
				ui::GearState body = gear_state(c.id);
				body.nameForeign = app.sim.foreign(c.id);
				if (body.valid) { iws.characters.push_back(body); }
			}
			else if (ui::ContainerState cs; container_state(c, cs)) { iws.containers.push_back(cs); }
		}
		if (app.itemTarget && !std::any_of(iws.characters.begin(), iws.characters.end(), [](const ui::GearState& g) { return g.owner == app.itemTarget; })
			&& !std::any_of(iws.containers.begin(), iws.containers.end(), [](const ui::ContainerState& c) { return c.owner == app.itemTarget; })) { app.itemTarget = 0; }
		iws.target = app.itemTarget;
		app.items->update(iws);
		menu_query_frame();
		rmb_hold_menu();
		// A refusal the original words goes to the HUD message box (FUN_14070f7e0's texts, kinspect inv_click10_static.md 2.6); the rest stay toasts
		const auto ends = [](const std::string& s, const char* tail) { const size_t n = strlen(tail); return s.size() >= n && s.compare(s.size() - n, n, tail) == 0; };
		for (const std::string& r : app.sim.takeReplies())
		{
			printf("sim reply: %s\n", r.c_str());
			if (ends(r, " REJECTED: no_room") && app.splitQuiet > 0) { app.splitQuiet--; continue; }	// the auto-split places it unit by unit
			if (ends(r, " REJECTED: no_room") || ends(r, " REJECTED: occupied")) { ui::message("No room for that item."); }
			if (r.starts_with("item stats REJECTED")) { continue; }	// a stale version: the next INVENTORY's version is asked anew
			if (ends(r, " REJECTED: stale")) { continue; }	// the gear queue's: re-sent once, then its own message (gear_orders_frame, k16.20.14.2)
			// trade refusals (MINE_SELL_SLICE 3.2: the original's texts)
			static const std::pair<const char*, const char*> kTradeRefusal[] = { { " REJECTED: out_of_range", "Out of trading range." },
				{ " REJECTED: cant_afford", "You can't afford that." }, { " REJECTED: shop_cant_afford", "The shopkeeper can't afford that." },
				{ " REJECTED: not_for_sale", "That's not for sale." }, { " REJECTED: illegal_only", "Smugglers only buy illegal goods." } };
			for (const auto& [tail, text] : kTradeRefusal) { if (ends(r, tail)) { ui::message(text); } }
			// a refused rename: the original's modal (faction29_static.md; our wording); the FACTION page puts the old name back itself
			if (r.starts_with("loot ") && r.find(" REJECTED: ") != std::string::npos)	// the server refused the loot: no window (a5 2026-11-04: it opened anyway)
			{
				if (app.pending.body) { app.pending = {}; }
				if (const char* text = container_refusal(r)) { ui::message(text); continue; }
				const std::string why = r.substr(r.find(" REJECTED: ") + 11);
				ui::message(why == "dead" ? "The dead can't loot." : "Can't loot: " + why);	// our wording
				continue;
			}
			if ((r.starts_with("put ") || r.starts_with("take ") || r.starts_with("transfer ")) && r.find(" REJECTED: ") != std::string::npos)
			{
				if (const char* text = container_refusal(r)) { ui::message(text); continue; }
				if (ends(r, " REJECTED: not_allowed")) { ui::message("It won't go in there."); continue; }	// an actor-state refusal (KO'd, not ours): ours, generic
			}
			if (r == "rename faction REJECTED: name_taken") { app.prompt->open({ "FACTION NAME", "Another faction already has that name." }, nullptr); continue; }
			if (r == "rename faction REJECTED: bad_name") { app.prompt->open({ "FACTION NAME", "That name can't be used." }, nullptr); continue; }
			const bool saveOrder = r.starts_with("save game ") || r.starts_with("load game ") || r.starts_with("delete save ");
			if (r.starts_with("load game ") && r.find(" REJECTED: ") != std::string::npos) { app.camLoadName.clear(); }	// no reset will come (k16.2.134.1)
			if (saveOrder && r.ends_with(" REJECTED: old_version")) { ui::message("This save is from an older version and can't be loaded."); }	// our wording
			else if (saveOrder && r.find(" REJECTED: ") != std::string::npos) { ui::message("Refused: " + r.substr(r.find(" REJECTED: ") + 11)); }
			else
			{
				// An order's result is not a player message (k16.2.300.5.1): it stays in the "sim reply:" line above, the debug line (sim.status()) and the
				// events log; only a refusal with no mapped message toasts, and only with --debug so a developer sees it.
				evlog::add("reply", app.sim.tick(), 0, "%s", r.c_str());
				if (app.opt.debug && r.find(" REJECTED") != std::string::npos) { ui::toast(r); }
			}
		}
	}
	progress_frame();
	name_labels_frame();
	speech_frame();
	dialogue_frame();
	dev::debugPanelFrame(dev_context());
#ifdef KCLIENT_DEBUG_VIEW
	debug_tree_frame();
#endif
#ifdef KCLIENT_DEV_TOOLS
	if (app.roadsPanel)	// the road editor's panel follows debug.roads and shows the editor's state (kenshi-tqtd)
	{
		const bool want = app.opt.debug && dev::roadedit::on();
		if (app.roadsPanel->shown() != want) { app.roadsPanel->show(want); }
		if (want) { app.roadsPanel->update(dev::roadedit::panelState(app.opt.data)); }
	}
	if (app.lightsPanel && app.lightsPanel->shown())	// the light debugger's view of the terms, offline too (kenshi-jj3.27.3)
	{
		ui::LightsPanelState ls;
		for (const dev::LightTerm& t : dev::lightTerms()) { ls.terms.push_back({ t.name, t.group, t.on }); }
		app.lightsPanel->update(ls);
	}
#endif
	ui::MapState ms;	// the camera and the player's characters; places when the sim sends discovery (DISCOVERED 37)
	for (const auto& [id, ch] : app.sim.decoder().chars) { if ((ch.cur.flags & simlink::kPlayer) && !(ch.cur.flags & simlink::kDead)) { ms.characters.push_back({ ch.cur.pos[0], ch.cur.pos[2] }); } }
	discovery_log();
	crime_log();
	lock_too_hard_log();
	map_places(ms);
	ms.cameraX = app.pos[0];
	ms.cameraZ = app.pos[2];
	ms.cameraYaw = (float)app.yaw;
	app.overview->updateMap(ms);
	{
		// k16.2.435.5: the world map always opens centred on the camera (the zoom is kept; the centre never is): on the frame the MAP page turns up shown
		static bool mapWasShown = false;
		const bool mapShown = app.overview->shown(ui::OverviewPage::kMap);
		if (mapShown && !mapWasShown) { app.overview->centreMap(); }
		mapWasShown = mapShown;
	}
	app.minimap->show(kMinimap.get() && app.sim.active() && !title_shown() && !app.creatorUi->shown() && !app.wmap.full);	// hidden with the full map, the title, loading (no sim), the creator
	if (app.minimap->shown())
	{
		app.minimapState.map = std::move(ms);	// after the overview's copy; the minimap draws no names and has no extent / texels
		app.minimap->update(app.minimapState);
	}
	status_popup_update();
	dev::updateUi();	// the playtest windows
	ui::frame(dt);
	map_view_frame();	// k16.2.435: the map's default view, once the MAP page has a rect (before this frame's draws are recorded)
	if (app.creatorUi->shown()) { logicthread::renderIdle(); creator_frame(dt); }	// overlap (rule 12): the creator's frames run gated (the switch to it too)
	else { app.buildings.setEditor(false); app.chars.clearPreviewOrbit(); }	// the preview keeps the orbit only while it shows (it may be closed alone)
}

}
