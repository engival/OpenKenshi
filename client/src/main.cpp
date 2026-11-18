#include <algorithm>
#include "lightbake.hpp"
#include "drawstats.hpp"
#include "framestats.hpp"
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

#include "interiorclip.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_debugtext.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#if defined(SOKOL_GLCORE)
#define GL_GLEXT_PROTOTYPES 1	// the frame-buffer calls the dev depth readback needs (animlab / idbuffer do the same)
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include "buildings.hpp"
#include "foliage.hpp"
#include "grass.hpp"
#include "gputimer.hpp"
#include "cpustage.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/threadsplit.hpp"	// the logic / render split on the stages line (kenshi-k16.2.205.12.84)
#endif
#include "dev/packetdigest.hpp"	// KCLIENT_PACKET_DIGEST / KCLIENT_PACKET_POISON (kenshi-k16.2.205.12.93)
#include "dev/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "sidorder.hpp"
#include "fogvolume.hpp"
#include "sunshafts.hpp"
#include "dust.hpp"
#include "blooddecals.hpp"
#include "arrows.hpp"
#include "particles.hpp"
#include "particleworld.hpp"
#include "beamfield.hpp"
#include "physics.hpp"
#include "ragdolldrive.hpp"
#include "ragdoll.hpp"
#include "hdr.hpp"
#include "idbuffer.hpp"
#include "post.hpp"
#include "camera.hpp"
#include "portraits.hpp"
#include "doorpose.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/pickmask.hpp"
#endif
#include "dev/lightterms.hpp"
#include "aitask.hpp"
#include "githash.hpp"
#include "loadscreen.hpp"
#include "localserver.hpp"
#include "dds.hpp"
#include "ogremesh.hpp"
#include "clientnav.hpp"
#include "kworld/towns.hpp"
#include "navoverlay.hpp"
#include "config.hpp"
#include "datadir.hpp"
#include "windowgeom.hpp"
#include "windowgate.hpp"
#include "readback.hpp"
#include "renderqueue.hpp"
#include "inputqueue.hpp"
#include "sappcmd.hpp"
#include "logicthread.hpp"
#include "lastframe.hpp"
#include "renderframe.hpp"
#include "opencont.hpp"
#include "weff.hpp"
#include "progressfeed.hpp"
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
#include "serverrows.hpp"
#include "ui/trade.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/bubbles.hpp"
#include "ui/dialogue.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/coords.hpp"
#include "materials.hpp"
#include "mods.hpp"
#include "resources.hpp"
#include "shadow.hpp"
#include "sky.hpp"
#include "kutil/crashlog.hpp"
#include "eventlog.hpp"
#include "squads.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#include "ui/dev/debugtree.hpp"
#include "ksim/debugview/enums.hpp"
#endif
#include "characters.hpp"
#include "creator.hpp"
#include "creatorscene.hpp"
#include "kworld/gamedata.hpp"
#include "terrain.hpp"
#include "water.hpp"
#include "reflection.hpp"
#include "speccube.hpp"
#include "weathermap.hpp"
#include "weatherinstances.hpp"
#include "audiohook.hpp"
#include "audio/events.hpp"
#include "simview.hpp"
#include "trails.hpp"
#include "collisionpick.hpp"
#include "controlhints.hpp"	// the control-hints types and the resolver stub (kenshi-k16.2.294.2)
#include "pick.hpp"	// Pick and the target predicates the hover / click / menu read (kenshi-k16.2.294.2)
#include "cursors.hpp"
#include "drawlist_gpu.hpp"
#include "overlaylist.hpp"
#include "worldbuildings.hpp"
#include "worldmap.hpp"

namespace
{

static void debug_toggle(const std::string& id);
#ifdef KCLIENT_DEBUG_VIEW
static void debug_tree_open(uint8_t kind, uint64_t id);
static std::string tree_state_value(uint8_t kind, uint64_t id, const std::string& key, const std::string& value);
#endif
static void shot_pending_frame();
constexpr double kScriptDt = 1.0 / 60.0;	// fixed timestep while scripting
void event(const sapp_event* e);
static InputQueue gInput;	// sokol_app's events, drained at the top of frame() (render-thread hook 1, inputqueue.hpp)
static bool gDraining = false;	// logic thread: event() is running a queued event (queue_event stored its pointer already, newer ones may follow)
static bool gFrameRan = false;	// sokol delivers the first loop's events before init(); those still go straight to event(), as before. Written and read on the sapp thread only (frame() sets it, kenshi-k16.2.205.12.23)
static void queue_event(const sapp_event* e)
{
	if (e->type == SAPP_EVENTTYPE_CLIPBOARD_PASTED) { sappcmd::publishClipboard(); }	// the published copy, refreshed on sokol's thread while its buffer is valid (render hook 3)
	if (e->type == SAPP_EVENTTYPE_MOUSE_MOVE) { pointer::set(e->mouse_x, e->mouse_y); }	// the draw's cursor, at the newest position (kenshi-k16.2.205.12.97)
	if (gFrameRan) { gInput.push(*e); }
	else { event(e); }
}
void run_load_step();
void init_ui();
void update_ui(double dt);
void build_mvp(float mvp[16], float nearZ, float farZ);
bool init_characters();
void sim_opened();
void place_characters(const std::vector<std::string>& specs, double x, double z, double yawRad);
bool set_follow_id(uint64_t id);
void leave_follow();
void update_rts(double dt);
void open_creator(const std::string& what);
void creator_slider(const std::string& name, float v);
void place_blood(const std::vector<std::string>& specs, double x, double z, double yawRad);

// TODO: units per metre is unknown (CLIENT.md "Open questions"); the coordinator's guess is 10 units/m.
constexpr double kHalfWorld = -kworld::kWorldMin;
constexpr double kEyeHeight = 17.0;		// walk mode eye height above ground, world units
constexpr double kWalkSpeed = 15.0;		// world units / s
constexpr double kFlySpeed = 400.0;		// world units / s
constexpr double kFastFactor = 10.0;		// shift
constexpr float kFogDistance = 60000.0f;
constexpr int kScreenshotFrames = 3;		// --screenshot: frame at which the shot is taken
constexpr double kStallMs = 250;	// a frame gap over this is an evlog `stall` (kenshi-k16.2.26: the loading screen froze the window)
constexpr int kRecordsFrameMs = 16;	// the characters step's panel records per frame (kenshi-k16.2.26): the budget is the loading screen's own frame
constexpr int kLoadsPerFrame = 24;
// The camera's default position (kenshi-k16.2.112.1): the Hub's centre (the Hub scenarios' `source centre=`), never the world origin, which is the
// Deadlands — the user's "loading into deadlands should be considered a spoiler" (2026-11-12). --pos / --at / --camera-pos still win.
constexpr double kHubDefaultX = -50619.0, kHubDefaultZ = -1086.0;
// A live session with no camera target this long falls back to the Hub default and says so (kenshi-k16.2.112.1, user 2026-11-12).
constexpr double kCameraWaitSeconds = 10.0;

struct Options
{
	std::string data;	// --data; resolved before use when empty (datadir.cpp: $KENSHI_DATA, the saved setting, the default installs)
	std::string cmdline;		// as started (the screenshot sidecar)
	std::string screenshot;
	std::string loadShot;		// --loading-shot N,FILE: debug, interactive-style startup; screenshot the loading screen once N steps are done, then exit
	int loadShotStep = 0;
	std::string script;
	std::string follow;		// --follow NAME|id=N: the character to follow once it streams in (a spectator session)
	// (--view-mesh / --tex / --dist are dev flags: dev/viewmesh.cpp, dev/devhooks.hpp)
	std::string matReport;	// --mat-report: write the odd-submesh-material mesh list here at exit
	int offsetFrame = 0;		// --offset-frame 0|1: axis frame of part offsets and spin axes (1 = Z-up)
	bool hideMarkers = false;	// --hide-markers: do not draw parts flagged "is for position marker" (default: drawn, un-offset)
	bool adaptScripted = false;	// --adapt: scripted runs adapt at the real rate (1/60 s per frame) instead of jumping to the current luminance
	double exposure = 0;		// --exposure auto|N: 0 = HDR auto exposure (GPU luminance chain), N > 0 = fixed multiplier
	int bradius = 3;		// --bradius: building load radius in zone cells (0 = no buildings)
	int fradius = 4;		// --fradius: foliage load radius in zone cells (4 = 18.4 km: landmarks fade past the fog; the last 20 % is the dithered fade)
	double foliageRange = 6.0;	// --foliage-range: multiplier on the game's foliage draw distances (the game ships 1)
	bool grass = true;		// --no-grass: skip the grass layers (GRASS records)
	double grassRange = 2.0;	// --grass-range: multiplier on the game's per-class grass distances (500 / 1000 / 8000 units at 1); the dithered fade covers each one's last 20 %
	bool grassPaint = false;	// --grass-map paint: density from the exe's noise paint instead of the overlay R byte as loaded
	float grassDensity = 1.0f;	// --grass-density: multiplies every layer's blades per area (the game's `grass density` setting, default 1)
	bool foliage = true;		// --no-foliage: skip the procedural foliage (trees, bushes, rocks; kworld::generateFoliage)
	bool fogVolumes = true;	// --no-fog-volumes: skip the static fog volumes (fogfeatures.dat)
	bool particles = true;	// --no-particles: skip the weather particle effects (rain, ash) and the placed world effects; particles.hpp
	bool water = true;		// --no-water: skip the water plane (M4)
	int shadowStagger = -1;		// --shadow-stagger 0|1: redraw the far shadow cascades only every few frames (default: on, off in scripted runs so shots are exact)
	bool shadows = true;		// --no-shadows: start with the sun shadows off (F3 toggles)
	bool waterFlat = false;		// --water-flat: debug, unlit water colour only
	bool features = true;		// --no-features: skip the hand-placed features (rocks, formations; features.dat)
	bool interiors = true;		// --no-interiors: no interior parts or layout furniture (the game's look with every interior unloaded)
	double time = 12.0;		// --time HH.H: starting game hour
	double timescale = 1.0;	// --timescale N: game speed (0 pauses, 1, 2, 5)
	double simspeed = 0;		// --simspeed N (dev): the sim's game speed, sent as SET_SPEED once the session is live (kenshi-k16.2.205.7)
	int day = 1;			// --day N: starting game day (the HUD's "Day"; the moons spin with the total game minutes)
	double skyHours = 0;		// --sky-hours H: starting SkyX+0x7c (played game hours; the star scroll), to match a capture's uTime / 0.5
	std::string weather;		// --weather NAME: pin a WEATHER record ("default" = the fixed default look); empty or "auto" = regional weather blended from the areasmap
	float weatherBlur = 1.0f;	// --weather-blur KM: width (FWHM) of the camera blend of the regional weather
	float frontBlur = 2.0f;		// --front-blur KM: sigma of the blur of the per-pixel weather map (softness of distant storm fronts; 0 = hard cell edges)
	std::string simlink;		// --simlink FILE.bin | HOST:PORT: characters from the sim link (docs/formats/SIMLINK.md)
	bool localServer = false;	// --local-server: the title and its own server in a scripted run too (start_local_server)
	double simlinkTick = -1;	// --simlink-tick N: a replay starts at tick N
	// (--scene-test / --scenes / --scene-file / --scene-hud / --scene-frames are dev flags: dev/scenetest.cpp, dev/devhooks.hpp)
	bool debug = false;		// --debug: the development features (the F8 panel, the debug keys, the scenes, the debug script ops); release builds leave them out
	std::vector<std::string> characters;	// --character SPEC (repeatable): test characters in a row in front of the start camera, add_characters()
	std::string creator;	// --creator RACE[/female]: the character creator's preview in front of the start camera (open_creator)
	std::vector<std::string> blood;	// --blood SPEC (repeatable): ground blood decals placed from the start camera, place_blood()
	std::string squads;	// --squads FILE: sim snapshot (default: cpp/build/default_world_squads.json in the repo); missing = notice
	std::string map;		// GUI_Map.dds override; empty = nicemap mod if found and enabled, else vanilla
	std::string at;			// --at PLACE|x,z: a town name from the game data (case-insensitive prefix) or an x,z pair (k16.2.87)
	double x = kHubDefaultX, z = kHubDefaultZ, alt = 800;	// the Hub, not the origin (k16.2.112.1)
	bool explicitPos = false;	// --pos / --at / --camera-pos: the camera has a target before the session (k16.2.112.1)
	double yaw = 0, pitch = -20;	// degrees; yaw 0 looks towards -z, 90 towards +x
	// --camera-pos/-yaw/-pitch/-dist: the floating camera on first sim contact (a scenario's .kclient_args sidecar); degrees as yaw / pitch
	bool camera = false;
	double cameraPos[3] = {}, cameraYaw = 0, cameraPitch = -30, cameraDist = 150;
	bool walk = false;
	float fog = kFogDistance;
	int width = 1280, height = 720;
};

struct Cmd
{
	int frame;
	std::string op;			// key, mouse, scroll, set, screenshot, quit
	std::vector<std::string> args;
	bool done = false;
};

int g_sgErrors = 0;	// sokol-gfx errors and panics logged so far (a message-less [sg][error] means a pool ran out)
int g_exitCode = 0;
static std::chrono::steady_clock::time_point g_quitAsked;	// the first quit request: cleanup() logs the time from here to the server's stop (kenshi-379.12)
static void request_quit()
{
	if (g_quitAsked == std::chrono::steady_clock::time_point{}) { g_quitAsked = std::chrono::steady_clock::now(); }
	sappcmd::requestQuit();
}

void log_cb(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char* file, void* user)
{
	if (level <= 1 && tag && !strcmp(tag, "sg")) { g_sgErrors++; }
	slog_func(tag, level, item, message, line, file, user);
}


// A versioned inventory order waiting for its INVENTORY (kenshi-k16.20.14.2): one per inventory in flight, the rest queued. The item is remembered by
// its record + ITEM_GRID section + cell (the index moves when an entry leaves) and re-resolved at send time.
struct GearOrder
{
	uint8_t kind = 0;			// 63 machine put (by name), 64 moveItem, 65 arrange, 66 dropEntry, 67 equipEntry, 68 transferEntry
	uint64_t inv = 0;			// the inventory whose version the order carries: the queue's key
	uint64_t owner = 0;			// the order's `id`: the character (a body for its own pack), or the container for a 64 in it
	uint64_t container = 0;		// the order's `container` (68) or the producer (63)
	uint32_t record = 0;		// the item's strid, with section + x, y the re-resolution key
	std::string name;			// the record's name (63 goes by name)
	bool hasQuality = false;	// a graded entry (a weapon / armour): moveAll moves only entries of the same grade and level
	uint8_t grade = 255;
	float level = 0;
	uint8_t section = 255;		// the entry's ITEM_GRID section where it was clicked
	uint8_t x = 0, y = 0;		// its top-left cell
	uint8_t toSection = 255;	// the destination (64 / 68; 255 autoplace)
	uint8_t toX = 0, toY = 0;
	uint16_t count = 1;			// units
	bool take = false;			// 68: the container (or body) is the source
	bool on = false;			// 67: equip
	bool retried = false;		// its one stale retry was used
	uint64_t group = 0;			// moveAll: its orders share one; a refusal drops the rest (0: not one)
	bool byRecord = false;		// 68: the entry is found by its record (and grade, level) anywhere in the section, not by its cell: the follow-up of a chained move
	bool autoCell = false;		// 64: the cell is picked when it is sent (the first free one in toSection that holds the item's footprint; the wire's 64 has no autoplace)
	bool splitOnFull = false;	// a stack dropped at a cell: when the server answers no_room (more than the cell's cap) it is placed unit by unit (auto-split, k16.20.13.3)
	std::shared_ptr<GearOrder> then;	// a box -> trader drag is a take into the user's pack and then this put (kenshi-k16.20.13.2): sent once the take is applied
};

struct GearQueue
{
	std::deque<GearOrder> wait;	// not sent yet, in click order
	bool flight = false;		// one in flight: GearQueue::current
	GearOrder current;
	uint32_t seq = 0;			// its clientSeq (SimView::lastSentSeq at the send)
	uint32_t version = 0;		// the INVENTORY version it carried
	double sentAt = 0;			// steady seconds (gear_now), for the lost-reply timeout
	bool awaitVersion = false;	// it was applied; the INVENTORY that carries the new version is not here yet (the next order must wait)
	double answeredAt = 0;		// when the applied answer came (awaitVersion's watchdog)
};

// The camera a save was made from (kenshi-k16.2.134.1): a sidecar beside the client's other per-user files, written when the client issues a save
// and applied back when that save's load brings its first snapshot. mode: 0 the free (debug) camera, 1 the game's floating (RTS) camera, 2 the
// follow (hover / drive) camera the V key and a spectator's --follow use.
struct CameraState
{
	int mode = 1;
	bool valid = false;
	double yaw = 0, pitch = 0;	// radians (the client's own convention: yaw 0 looks towards -z)
	double pos[3] = {};		// the eye, as it stood
	double focus[3] = {};		// the RTS pivot
	double rtsDist = 400;
	uint64_t centerId = 0;		// the RTS auto-center character, 0 none
	uint64_t followId = 0;		// the followed character
	double followDist = 60, followBase = 0, followOffset = 0;
	int floorLevel = -1;		// the visible floor level 0..3 (WorldBuildings::level); -1 = an older sidecar without it: the start snaps
};

// A load's camera restore in flight: armed when the load's reset is seen (update_ui), applied on the load's first snapshot after it.
struct CamRestore
{
	bool armed = false, applied = false;
	uint32_t snapAt = 0;	// the decoder's snapSerial when armed; the first snapshot past it is the load's
	std::string name;
};

struct App
{
	Options opt;
	bool uiOn = false;	// ui::init succeeded (docs/UI.md); false: the world runs without the UI layer
	std::unique_ptr<ui::Hud> hud;
	ClientNav nav;	// the server's navmesh tiles (SIMLINK 12): invalidmove now, prediction next
	std::unique_ptr<ui::Options> options;	// F10
	std::unique_ptr<ui::ItemWindows> items;	// I: the character's grid inventory, a looted body's, the open boxes and a trader's (kenshi-k16.20.13.2)
	std::unique_ptr<ui::Stats> stats;	// toggle_stats (C / the HUD's STA): the character stats window for the selected character (kenshi-k16.2.96)
	uint64_t statsChar = 0;	// the character the stats window shows while open (the status panel's, unless the window is pinned)
	std::unique_ptr<ui::BuildingWindow> machineWin;	// the window of the producer in machine (a left click on it)
	uint64_t machine = 0;
	// The focus panel's subject (k16.19.8.2): a producer's id while the bottom-left panel shows it, 0 while it shows the character focus / the selection.
	// Kept apart from machine: the machine window's open state survives a character click, which moves the panel back to the character (the lead's 2026-11-12).
	uint64_t panelProducer = 0;
	uint64_t focusLogged = 0;	// the scripted `focus ...` line's last (id, kind); once per change
	bool focusLoggedProducer = false;
	uint64_t rowLoggedId = 0;	// the scripted `producer row ...` line's last row (id, version); once per change
	uint32_t rowLoggedVersion = 0;
	uint64_t gearChar = 0;	// the character the inventory window shows this frame (the dragging character of a machine output drop)
	std::unique_ptr<ui::Overview> overview;	// the original's overview window (MAP / ... / SQUADS tabs); outlives squadsUi
	std::unique_ptr<ui::Squads> squadsUi;	// U (MINE: the original's key for its SQUADS tab is not captured): PLAYER_SQUADS (SIMLINK 31) and orders 70-75
	uint64_t activeSquad = 0;	// the HUD tab: the portrait bar shows its members (client-side, SIMLINK 31)
	std::unique_ptr<ui::Creator> creatorUi;	// the creator screen over app.creator (--creator)
	std::unique_ptr<ui::SavePanel> saves;	// LOAD / SAVE GAME of both menus; outlives them. Empty until the server's save orders (SIMLINK 80-83, planned)
	std::unique_ptr<ui::MainMenu> mainMenu;	// the title screen: at startup (ui.title_at_start) and after EXIT GAME; hides the HUD
	std::unique_ptr<ui::GameMenu> gameMenu;	// Escape in the game
	std::unique_ptr<ui::AdvancedOptions> adv;	// the new game's ADVANCED OPTIONS (kinspect advopts.md / adv22_static.md); values host-side until the sim has the knobs
	std::vector<ui::AdvOption> advRows;
	std::unique_ptr<ui::NewGamePanel> newGame;	// NEW GAME of both menus: the starts; beginning one waits for the server's new_game
	// The title click held while its server starts (kenshi-k16.2.210.4): 0 CONTINUE, 1 NEW GAME, 2 LOAD GAME, 3 IMPORT GAME; -1 none. frame_title runs it
	// once title_status() clears (MainMenu::setPending draws its button pushed until then).
	int menuPending = -1;
	std::function<void()> menuPendingRun;
#ifdef KCLIENT_DEV_TOOLS
	int devServerHold = 0;	// script `serverhold N`: for N frames poll_local_server leaves the title's server unconnected, so a test can click while it starts
#endif
	std::unique_ptr<ui::Prospecting> prospect;	// PROSPECT: order 79 for one character; PROSPECT (35) opens it with the result (prospect_state)
	std::string prospectRes = "5";	// the resource whose map shows (resIds as text: 5 Water, 6 Fertility, 2 Stone, 1 Iron, 3 Copper)
	uint32_t prospectSeen = 0;	// the PROSPECT serial last opened
	std::string prospectPainted;	// "<serial>/<res>" of the map image (repainted when either changes)
	// The map image: render side (prospect_paint's job makes it and binds it under prospectTex, kenshi-k16.2.205.12.39). prospectTex is reserved at
	// the first paint and kept; prospectShown: logic painted a map (none: the window shows no image)
	sg_image prospectImg = {};
	sg_view prospectView = {};
	sg_sampler prospectSmp = {};
	dl::TexId prospectTex = 0;
	bool prospectShown = false;
	// The build window's BUILDING records by stringID (init_characters; kinspect findings/build_menu.md): what its lists and stats read
	struct BuildRec
	{
		std::string name, category, group, description, type;
		int materials = 0, powerOut = 0, powerCap = 0, operators = 0;
		float speedMult = 1;
		std::vector<std::pair<std::string, int>> cost;	// construction: ITEM name, count
		std::string produces;	// the first functionality's first produced ITEM
		int function = -1;	// the first functionality's (-1: none)
		float outputRate = 0;
		int mining = 0;	// the functionality's `world resource mining` (1 iron, 2 stone, 3 copper, 4 oil, 5 water, 6 farming): the focus panel's quality row (k16.19.7.1)
		std::string selectSound;	// the record's `select sound` as the Buildings_Click switch value (ui::BuildEntry::selectSound); empty = none
	};
	struct InteriorSound { int ambience = 0; float integrity = 0; bool cavernous = false; };	// BUILDING `interior ambience` / `interior integrity` / `cavernous` (Enter_Building)
	std::unordered_map<std::string, InteriorSound> interiorSounds;	// by record name (WorldBuildings::recordOf)
	std::unordered_map<std::string, BuildRec> buildRecs;
	std::string buildCategory, buildGroup;	// the build window's picks (empty: none)
	int buildStep = 0;	// the < > arrows' place in the group
	// The RESEARCH page's records by stringID (init_characters; kinspect findings/research25_static.md): which are done is the sim's (UNLOCKS 36);
	// the bench, the queue and progress wait on the sim
	struct ResearchRec
	{
		std::string name, category, description;
		int level = 1, time = 0;	// time: game hours at bench rate 1, before x 0.4 x 1.2^(level - 1) / research speed
		bool blueprintOnly = false, levelUpgrade = false;
		std::vector<std::string> needs;	// RESEARCH stringIDs
		std::vector<std::pair<std::string, int>> cost;	// ITEM name, count
		std::vector<ui::ResearchUnlock> unlocks;
		std::vector<std::string> buildings;	// enable buildings' stringIDs (the listing's "enables something new" test)
	};
	std::unordered_map<std::string, ResearchRec> researchRecs;
	// A door BUILDING's open pose (kinspect findings/door_pick.md: 140298d10 moves the leaf and its collision by the same offset): `door move dist`
	// along `door axis` (0..2: the shape's local x y z, Z-up), after `door comes out` forwards out of the frame
	using DoorSlide = dev::DoorSlide;
	std::unordered_map<std::string, DoorSlide> doorSlides;
	// Each door's pose this frame (update_doors), by kworld::DoorKey: its interactable, openness 0 closed .. 1 open, the offset of its leaf and collision
	struct DoorPoseNow { uint64_t thing = 0; double open = 0, off[3] = {}; bool exact = true, seen = false; };
	std::unordered_map<kworld::DoorKey, DoorPoseNow> doorPose;
	std::shared_ptr<const std::vector<kworld::WorldShape>> doorShapes;	// the gather doorFirst indexes
	std::unordered_map<kworld::DoorKey, size_t> doorFirst;	// a door's first collision shape (its frame)
	std::unordered_map<std::string, int> townTypes;	// TOWN stringID -> `type` (the map icon, tier and discovery radius)
	std::unordered_map<std::string, std::string> townUnexplored;	// TOWN stringID -> `unexplored name` (the name until visited; empty: the real one)
	std::string researchCategory;	// the picked TYPES row (empty: the first)
	std::unique_ptr<ui::Research> research;
	std::unique_ptr<ui::Faction> faction;	// the FACTION page: the name only; relations and renaming wait for the sim
	std::unique_ptr<ui::ProgressBars> progressBars;	// TASK_PROGRESS (39) bars and the event floating texts over the characters
	std::unique_ptr<ui::NameLabels> nameLabels;	// the player characters' names over their heads
	std::unique_ptr<ui::SpeechBubbles> speechBubbles;	// SIMLINK event 41 lines over the speakers
	std::unordered_map<uint64_t, float> speechHead;	// a speaker's Bip01 Head height in its idle stance (Characters::headHeight), while it speaks
	std::unique_ptr<ui::Dialogue> dialogue;	// the conversation screen over a REPLY_MENU (event 43)
	std::unordered_map<uint64_t, uint32_t> lastLine;	// a speaker's newest line (text strid): the dialogue screen's NPC text outlives its bubble
	std::unique_ptr<ui::Prompt> prompt;	// the modal answers (a refused faction rename)
	std::unique_ptr<ui::Crafting> crafting;	// the CRAFTING page: empty until the sim has crafting benches
	std::unique_ptr<ui::Build> build;	// toggle_build (B): the build window; its lists wait on which BUILDING records the original offers (kinspect asked), placement on the sim
#ifdef KCLIENT_DEV_TOOLS
	std::unique_ptr<ui::LightsPanel> lightsPanel;	// debug.lights_panel (Shift+F7): the light debugger, one row per dev::lightTerms term (kenshi-jj3.27.3)
#endif
	std::unique_ptr<ui::Tutorials> tutorials;	// HLP / toggle_help: the TUTORIALS browser. The original's are hard-coded in its exe (no records); ours: "Controls"
	std::vector<ui::StartEntry> starts;	// NEW_GAME_STARTOFF records (init_characters), in the original's order: its hash-bucket walk rotated to Wanderer (k16.17.4.2)
	// New game (SIMLINK 84, 111, 62): BEGIN sends new_world; its reset sends NEW_GAME for newGameStart; the answer's leader is followed once it shows.
	std::string newGameStart;	// the start-off's stringID while new_world is pending
	bool newGameSent = false;	// NEW_GAME sent, its answer pending
	// The new game's character editor (STARTS members, kinspect charsel23_static.md): one entry per member of the start's squad, leader first
	struct NewGameMember { simlink::OrderWriter::NewGameSpec wire; Characters::Spec look; std::string name; std::vector<std::string> wear, weapons; bool editable = false, visited = false, looked = false; };
	std::vector<NewGameMember> ngMembers;	// empty: the creator is not making a squad
	int ngIndex = 0;
	std::string ngStart;	// the start-off's stringID while its editor is up
	std::unordered_set<std::string> ngForce;	// the start's `force race` (RACE names); empty: any
	std::vector<simlink::OrderWriter::NewGameSpec> newGameSpecs;	// sent with NEW_GAME after new_world's reset
	std::unordered_map<std::string, std::string> creatorSids;	// "RACE:" / "ATTACHMENT:" / "COLOR_DATA:" + record name -> stringID (Spec strids)
	std::unordered_map<std::string, std::string> raceNames;	// RACE stringID -> name
	std::unordered_map<std::string, std::array<std::vector<std::string>, 2>> raceHeads;	// RACE name -> heads male / female stringIDs (Spec.head)
	uint64_t newGameLeader = 0;
	// The inspect focus (kinspect findings/inspect_focus.md): a left click on someone not ours shows them in the status and medical panels while
	// orders stay on the selection; a new selection ends it. 0 = the selection's. Never range-checked: unloaded, the panels show empty frames.
	uint64_t inspect = 0;
	uint64_t jobsChar = 0;	// the character whose jobs list the HUD shows (the status panel's)
	uint64_t infoChar = 0;	// the status panel's character this frame; 0 = none (the cheat panel's target, kenshi-h2v.6.4)
	bool menuPaused = false;	// a menu paused the sim; it unpauses when the game comes back
	bool menuUnpaused = false;	// the game menu just sent its unpause (EXIT GAME closes the menu, then pauses again before the clock shows it)
	int quietPauseTo = -1;	// a menu sent this pause state (0 / 1): its clock flip plays no Pause_Game / Resume_Game (update_ui); -1 none
	double creatorScene[3] = {};	// the editor scene's pivot origin (open_creator): the platform stands kCreatorPivot below it, the character kCreatorRoot
	float creatorHead = 17;	// Bip01 Head above the preview's feet (Characters::headHeight; the FACE / HAIR pivot)
	bool creatorWeatherAfter = false;	// the creator just closed: the next world frame logs the world's weather again (k16.17.6.1)
	SkyWeather creatorWeatherSave;	// the world's weather while the creator's clear look stands in for it (a pinned weather is restored on close)
	bool creatorWeatherHeld = false;
	int creatorFrames = 0;	// frames the creator has been shown (its one-shot scene measurement, --debug)
	std::optional<kworld::Heightmap> hm;
	Terrain terrain;
	Shadow shadow;
	bool shadowFlip = false;	// F3 / the debug menu asked to flip shadow.enabled; logic_paths applies it where the frame's second half starts (kenshi-k16.2.205.12.95)
	Water water;
	double animTime = 0;		// seconds, advances with the frame step and stops while the game is paused (water, grass sway, spinning parts)
	WorldMap wmap;
	Squads squads;
	double squadsLiveAt = -1;	// the live overlay's last rebuild (s since start; -1: showing the snapshot file)
	Arrows arrows;	// the stealth detection arrows (DETECTED_BY 60): one arrow.mesh per observer while a player character sneaks (kenshi-h2v.27.2)
#ifdef KCLIENT_DEBUG_VIEW
	dbg::DebugView debugView;	// the sim's DEBUG VIEW (SIMLINK "DEBUG VIEW"): subscribed while the squad overlay is on
	bool debugSubscribed = false;
	std::unique_ptr<ui::DebugTree> debugTree;	// one DEBUG_TREE: a squad pillar clicked with the overlay on
	uint8_t treeKind = 0;
	uint64_t treeId = 0;
	bool treeAuto = false;
	double treeAskedAt = 0;	// steady seconds of the last DEBUG_QUERY (auto: every 2 s)
#endif
	Characters chars;
	CreatorScene creatorScene3d;	// the creator's backdrop and fixed light (creatorscene.hpp)
	Creator creator;	// the character creator (--creator; script `creator ...`)
	SimView sim;
#if KCLIENT_PROFILE
	// The profiling overlay's Sim block (kenshi-k16.2.148): the PROFILE answers' tick means, one per answer, for the sparkline; simProfileSeen is the
	// decoder's profileSerial already taken into the history.
	std::array<float, 40> simTickHistory = {};
	size_t simTickHistoryN = 0, simTickHistoryNext = 0;
	uint32_t simProfileSeen = 0;
#endif
	Trails trails;	// debug trails (kenshi-k16.2.73; the newest 30 s drawn, k16.19.6.1): every drawn character's feet and its sim / drawn centres, debug.trails
	uint32_t sglShown[2] = {};	// the sgl error flags last logged for the default and the trails contexts (k16.19.6.1): log once per change of state
	Cursors cursors;	// the original's hover cursors, loaded with the sim link
	CollisionPick collide;	// click ray vs building / foliage collision, started with the sim link
	PhysicsWorld physics;	// ragdolls (docs/reference/PHYSICS.md): Jolt, started by the first body
	StaticColliders statics{ physics };	// the building / terrain colliders posted round live bodies
	Ragdolls ragdolls{ physics, statics };	// EVENT 48 -> Jolt ragdolls driving the lying bodies (P3; the .phs from the RACE record)
	LocalServer server;	// single player: the server this client started (start_local_server)
	bool local = false;	// the sim link goes to that server, once it listens
	std::string serverErr;	// why the local server did not start or connect (kenshi-k16.3.12): the title's offline line; cleared by a fresh start
	bool greeted = false;	// the server's HELLO is in: orders may go (sim_opened)
	int progressLevelSent = -1;	// the TASK_PROGRESS level (order 98) the session has been told; -1 none yet (progress_level_sync)
	bool session = false;	// a game has been on screen (the title closed into it; --simlink: from the start): CONTINUE resumes it
	// The decoder serials handled so far: one connection's (a new SimView counts from 0 again; end_session clears them)
	struct Seen
	{
		uint32_t discovered = 0, saves = 0, reset = 0, starts = 0, newGame = 0, squads = 0, seenBy = 0, detectedBy = 0;
	} seen;
	// Session loading screen (begin_session_load): up until the world round the squad has streamed in; its own sim pause, undone only by itself
	bool sessionLoad = false, sessionPaused = false;
	int sessionQuiet = 0, sessionPending = 0, sessionPeak = 0;
	double sessionTime = 0;
	bool settledChars = false;	// the scene's characters had settle=SECONDS: its JSON reports the idle fit (foot_idle)
	float scriptBuild = -1;	// script `set playerbuild P`: every unbuilt player building draws at progress P (0..1) instead of the sim's; < 0 the sim's (the sim's sites do not advance in a scenario, k16.2.36.1)
	bool scriptAnim = false;	// script `set animate 1`: characters animate on wall time (scripted runs otherwise freeze them for repeatable shots)
	bool scriptRealDt = false;	// script `set framedt real`: a scripted run steps the world by the frame's real duration, not the fixed 1/60 s (a motion measurement, k16.2.181.1; kScriptDt keeps shots repeatable)
	double particleScriptDt = 0;	// script `set particles N`: scripted runs step the particle sim by N seconds a frame (0 keeps the frozen shot)
	bool simCamera = false;	// the camera was placed at the sim's focus character
	// The first world frame (kenshi-k16.2.112.1): a live session with no explicit position draws and streams nothing until a target places the
	// camera (the squad's placement, a new game or scenario, a load, --pos / --at), then logs `first_world_frame` with the reason.
	bool worldTarget = true;	// the camera has a target: the world may stream and be drawn (true from the start in the offline / viewer modes)
	const char* targetReason = "default";	// which target the first world frame drew from: default, option, squad, spectator, load, newgame, scenario, timeout
	bool firstWorldFrame = false;	// the first_world_frame line is out (once a run)
	double targetWait = 0;		// s a live session has waited for a target (the kCameraWaitSeconds fallback)
	bool loadPending = false;	// the last reset was a load: its squad placement reads as the load's target in the log
	bool screenshotOnWorld = false;	// --screenshot in a --simlink run with no --script: the shot waits for the first world frame (k16.2.124.1)
	// Follow camera and direct drive (docs/CLIENT.md "Camera and control modes", SIMLINK 12): C follows the first selected character from behind,
	// rotating with it; on a player character W / S drive it forward / back along the camera, A / D (or mouse look) steer, Q toggles autorun.
	bool follow = false;
	uint64_t followId = 0;
	bool spectating = false;	// a session with no player faction (spectator): the HUD's squad bar is empty and the watch (the inspect focus) can be followed
	bool worldChosen = false;	// the player asked for a world and got it: a NEW_GAME result, a scenario started (status 0) or a load's reset. A local (title-started) session's
								// pre-game world is nobody's: spectator mode is decided only after this (kenshi-k16.2.95)
	uint32_t cagesSeen = 0;	// the last CAGES frame logged (--debug)
	uint64_t hoverThing = 0;	// the ground item, container or door under the cursor (hover_cursor), 0 = none
	// The open item windows besides the character's (kenshi-k16.20.13.2): opencont.hpp holds the record, the dev `dump windows` op reads it too.
	using OpenCont = ::OpenCont;
	std::vector<OpenCont> conts;
	// The box or body asked for by a click, not open yet: the selection walks there, the window opens once a selected player character is in reach.
	// A box's is superseded by the next order of its walker (lead): `task` is lead's task (SimView::tasked, bumped as an order is sent) once the open's
	// own order went out; a different one later is another order, and the open is dropped (kenshi-k16.2.304.1).
	struct PendingOpen { uint64_t id = 0; bool body = false; uint32_t lootTask = 0; uint64_t lead = 0; uint32_t task = 0; } pending;
	int splitQuiet = 0;	// no_room refusals the auto-split recovers from: their message is not shown
	bool itemsShown = false;	// the character window (I)
	bool charPinned = false;	// its pin
	uint64_t itemTarget = 0;	// the marked window's owner (the target toggle): the quick right click goes there
	// Trade (MINE_SELL_SLICE 3.2): the open TRADE's stock is a container window (no reach rule: the server refuses out-of-range moves); closing the
	// window sends order 86 and waits for event 40 (tradeClosing) so the still-valid TRADE does not reopen it
	uint64_t tradeStock = 0;
	bool tradeClosing = false;
	uint64_t gearPinned = 0;	// the inventory window's owner while its pin is on (outside a loot / container); 0 = it follows the selection
	// The inventory order queues (kenshi-k16.20.14.2), one per inventory id: a rapid sell burst must not send an order before the INVENTORY that
	// answers the last (the version the older clicks carried was refused stale). Items queued or in flight draw pending (dimmed, not clickable).
	std::map<uint64_t, GearQueue> gearQueues;
	std::vector<std::pair<GearOrder, double>> gearFollow;	// the second halves of chained moves (a take applied), waiting for the item to reach the user's pack
	uint64_t gearGroup = 0;		// the last moveAll group's id
	uint64_t clockTick = 0;	// the last sim CLOCK applied to sky.clock (live: the sim owns the time; the sky advances it between frames)
	double followDist = 60, followBase = 0, followOffset = 0;	// camera distance; smoothed camera yaw behind the character; the user's orbit on top
	double followPulled = 1e9, followHold = 0, followBoom = 0, rtsPulled = 1e9, rtsHold = 0, rtsBoom = 0;
	bool rtsLog = false;	// script `rtsboom log`: update_rts prints its boom each frame	// the collision boom (update_follow): the length the world allows it (1e9: free), seconds left before it may ease out
	// The game camera (docs/CLIENT.md "Camera modes"): floating over the ground at rtsDist from focus, on yaw / pitch; centerId != 0 = auto-center on
	// that character. Off = the debug free camera; follow (the hover / drive camera) overrides both.
	bool rts = false;
	uint64_t centerId = 0;
	bool centerOnStart = false;	// a session start: auto-center on the first selected character once there is one (user 2026-11-06)
	double focus[3] = {}, rtsDist = 400;
	const char* focusOn = "terrain";
	CamRestore camRestore;	// the camera sidecar of the save being loaded (k16.2.134.1)
	std::string camLoadName;	// the save name the client last asked to load; its reset arms camRestore
	int camLoadFrame = 0;		// app.frames at that request (a load that never resets expires it)
	bool navTrace = false;	// script `dump navdraw`: the next navmesh overlay draw prints its stair polygons	// what the floating camera's focus rests on (ground_under)
	bool orbit = false;	// the middle button held: the mouse rotates the camera (controls.cfg mouse_rotate = Mouse3)
	bool autorun = false;
	uint8_t driveSent = 0;
	int strafeSent = 0;	// the strafe of the last drive order (mouse look + A / D, kenshi-k16.2.237.4)
	double lookDx = 0;	// mouse-look pixels since the last frame's log line (kenshi-k16.2.237.9)
	double headingSent = 0, driveAge = 0;	// seconds since the last drive order
	bool charsReady = false;	// init_characters() ran (game data loaded): scenes may place their char= specs
	Mods mods;
	std::future<TownSet> townFut;
	TownSet towns;
	bool townsReady = false;
	std::future<BuildingSet> bldFut;
	bool bldReady = false;
	WorldBuildings buildings;
	Foliage foliage;
	bool foliageOn = false;		// initialised (foliage enabled)
	Grass grass;
	bool grassOn = false;		// initialised (needs the foliage context: --no-foliage also turns the grass off)
	bool bldSet = false;		// WorldBuildings::setData done (an empty set when --bradius 0)
	// The props list (containers / producers / ground items) and its dirty tracking (kenshi-k16.2.205.11): the sim sends an interactable only on
	// first sight or a version change (SimLinkDecoder::thingsSerial), so the list is rebuilt on that and on a floor-cut change only; the frames
	// between reuse it and skip WorldBuildings::setProps. propsLive: a player building is under construction (its progress moves every tick).
	std::vector<WorldBuildings::Prop> props;
	uint32_t propsSerial = 0;
	uint32_t propsBuildSerial = 0;
	uint64_t propsCut = 0;
	bool propsValid = false;
	bool propsLive = false;
	GpuTimer gpu;
	Hdr hdr;
	Reflection refl;	// the water's planar reflection (drawn when the water was drawn last frame)
	IdBuffer idbuf;	// the cursor ID pass: the building drawn under the cursor (update_peek)
	// the id pass's last result lives in framestats (FrameStats::cursorHit, render hook 5)
	Post post;
	lightvol::Volumes lightVolumes;	// render.interior_light (kenshi-jj3.2.1): the baked buildings' sky transfer on the GPU
	FogVolumes fogvol;
	SunShafts sunShafts;	// render.sun_shafts (kenshi-jj3.7.1)
	Dust dust;	// render.dust_motes (kenshi-jj3.8.1)
	Particles particles;
	ParticleWorld particleWorld;
	WeatherInstances weatherInst;		// the weather's world-placed effects (types 2/3/7/8/9/10; the type 4/1/5/6 anchored ones live in Particles)
	BeamField beams;			// Venge's sky beams from the sim's BEAMS schedule (the type 9 effects the sim owns, kenshi-h2v.14.2.1)
	uint32_t beamsSeen = 0;			// the last BEAMS frame (decoder beamsSerial) given to beams
	bool beamsOn = true;			// script `set beams on|off` (off: the sim's beams are not placed; an A/B shot for the beam test)
	using Weff = ::Weff;				// weff.hpp holds the record, the dev `debug weff` op appends it too
	std::vector<Weff> weff;				// `debug weff SCRIPT DX DZ`: weather-style systems pinned at the pivot + offset (a dev A/B aid, k16.2.11.3)
	BloodDecals blood;
	Portraits portraits;	// render thread: the atlas and its draw (RenderState::portraits)
	PortraitTable portraitTable;	// logic thread: the cells, the queue, the uv the UI shows (kenshi-k16.2.205.12.56)
	dl::TexId portraitsTex = 0;	// the portrait atlas's UI id, bound at Portraits::init (render hook 8's host half, kenshi-k16.2.205.12.39); 0: none
	bool showPortraits = false;	// script `set portraits 1`: the atlas's first 4 cells drawn top-left at 2x (a check, until the HUD draws them)
	float rainForce = -1, dustForce = -1, cloudsForce = -1;	// the scene's rain= / dust= / clouds= keys
	double windForce = -1;	// the scene's wind= key (degrees), -1: the live wind
	int particlesDrawn = 0;
	sg_environment_defaults swapFormat = {};	// the swapchain's formats; the scene pipelines default to RGBA16F (init)
	float hdrSceneLum = 0, hdrAdapted = 0;	// diagnostics of the last screenshot / scene shot (GL readback, scripted runs only)
	double logicMs = 0;	// render.thread=lockstep: the world frame's logic half, smoothed (start_logic_thread; logic thread only). The draw half is framestats renderMs
	Sky sky;
	std::future<SkyConstants> skyFut;
	std::future<SkyWeather> weatherFut;
	WeatherMap wx;			// regional weather (areasmap -> BIOME_GROUP -> SEASON -> WEATHER): camera blend + per-pixel fog map
	std::future<WeatherAtlas> atlasFut;
	bool weatherAuto = true;	// regional weather drives sky.weather; a manual weather (scene, CLI, , and .) pins it off
	uint32_t weatherSeen = 0;	// the last WEATHER frame (decoder weatherSerial) given to wx
	std::string weatherMix;		// HUD text of the blend, e.g. "Clear 70% / Dust Storm 30%"
	SkyRamp skyRamp;		// regional weather: the sky's eased cloud density and colour mult (sky.hpp)
	int skyRampGroup = -1;		// the camera's weather group the ramp last aimed at
	float skyRampTarget[4] = {};	// its record's clouds density and sky colour mult then
	double skyRampXZ[2] = {};	// the camera's xz last frame (the jump snap)
	bool skyReady = false;
	double frameMs = 0;		// smoothed wall time per frame
	double mouseX = 0, mouseY = 0;
	double orderAt[3] = {}, orderAge = 1e9;	// the last move_to click (the overlay's fading ring), seconds since
	std::string zone;	// the BIOME_GROUP name under the camera, for the zone banner
	bool orderRejected = false;	// event 10 answered it (e.g. no_route): the ring flashes red
	uint32_t orderSeq = 0;	// the clientSeq of that move_to, for the sim's route answer (the marker's walk target, k16.2.259)
	bool orderRouted = false;	// the route with orderSeq has arrived: the marker sits at its end, logged once
	int peekCand = -1;		// update_peek: the building the cursor would peek into, and for how long (seconds)
	double peekFor = 0;
	int floorShown = 0;	// the floor diamond's label: the last floor shown while it had a target
	double rmbAt = -1;	// ui::now() of an RMB press on a target (character, item, door); held kHoldMenu without a drag: the order menu
	double clickX = 0, clickY = 0, clickDrag = 0;	// right button: where it went down, and how far the mouse moved since (raw counts while locked)
	double clickAt = 0, clickPend[2] = {};	// ui::now() of the press; mouse motion held back until the press turns into a look drag
	// Box selection (kenshi-k16.2.230.1): an LMB press on the ground or a character waits for its release; moved past kBoxDrag it draws a rectangle
	// and selects the player's characters inside, held still it is the click it always was (sim_click at the press).
	bool boxDown = false, boxDrag = false, boxShift = false;
	double boxX0 = 0, boxY0 = 0, boxX1 = 0, boxY1 = 0;	// framebuffer pixels: the press, the pointer now
	bool lookDrag = false;	// the press became a look drag (kDragFar, or kDragNear after kClickTime): its release sends no order
	double fps = 0;
	double pos[3] = {};
	double yaw = 0, pitch = 0;
	double speedScale = 1;
	bool walk = false;
	bool look = false;		// mouse look active (right button held or Tab toggled)
	bool keys[SAPP_MAX_KEYCODES] = {};
	std::vector<uint64_t> bar;	// the HUD bar's characters this frame (select_N, character_next)
	uint64_t lastSelect = 0;	// select_N: the last one picked and when (twice = focus)
	double lastSelectAt = -1;
	uint64_t lastRmbChar = 0;	// the last right click on an own character, and when: a second one within kDoubleClick is the direct drive (k16.2.238.1)
	double lastRmbCharAt = -1;
	int frames = 0;	// the frames drawn so far: logic's copy of FrameStats::frames, set at logic_frame's start (the render side counts them)
	std::vector<Cmd> cmds;
	std::string sceneName;
	std::vector<SkyWeather> weathers;	// [0] = the default weather, then every WEATHER record by name; loaded on demand
	std::future<std::vector<SkyWeather>> weathersFut;
	int weatherIdx = 0;
	int weatherCycle = 0;			// cycle steps waiting for the list
	std::string wantWeather;		// scene weather waiting for the list
	bool help = false;
	bool hideUi = false;		// toggle_bar (F7): the interface hidden, the world only
	bool shotPending = false;	// screenshot (F12): saved after this frame's draw
	double shotAt = -1;	// screenshot_delayed: ui::now() it fires at (user 2026-11-05: to show a held key, as ALT's labels)
	ResourceIndex res;
	MaterialLib mats;
	std::future<TerrainDetail::Prepared> detailFut;
	std::future<kdata::GameData> charsFut;	// the characters step's CPU work (kworld::loadMergedGameData) on a worker: the step polls it (kenshi-k16.2.26)
	std::future<uint64_t> hashFut;	// the sim link's data hash, on a worker (kenshi-k16.2.26: 24 ms on the main thread inside the characters step)
	uint64_t dataHash = 0;
	std::string hashErr;
	std::unique_ptr<kdata::GameData> gdPending;	// the game data between the characters step's first frame and the records' last
	std::future<void> gdFreeFut;	// the game data's destruction, on a worker (kenshi-k16.2.26) (the asset build, then the panel records)
	int charsPhase = 0;
	LoadScreen loadScreen;
	std::vector<std::pair<const char*, std::function<bool()>>> loadSteps;	// startup steps (init); interactive runs run one per frame behind the loading screen
	size_t loadIdx = 0;
	bool loadShown = false;	// the loading screen has presented the current step's name
	bool loading() const { return loadIdx < loadSteps.size(); }
	bool scripted() const { return !opt.script.empty() || !opt.screenshot.empty() || dev::sceneTesting(); }
	// Frame-gap measurement (kenshi-k16.2.26): the wall time between frame() calls, so a long load step shows as the frame it blocked; a gap over
	// kStallMs is an evlog `stall` event named with the phase (the load step, the session load, or the world frame). The maxima are the report's table.
	std::chrono::steady_clock::time_point frameStart{};
	double frameGapMax = 0, startupFrameMax = 0, sessionFrameMax = 0;
	const char* framePhase = "startup";	// what the current frame is doing: the load step's name, session load, world frame (the next gap's stall event)
	bool startupFrames = true;	// until the first world frame: the startup maxima's window
} app;

// _exit with no other cleanup, but the local server child ends first: left alive it holds the stdout/stderr pipes open and whatever waits on kclient hangs (kenshi-k16.2.271).
[[noreturn]] static void exit_now(int code)
{
	readback::flushWrites();	// a screenshot still on the writer thread (the load shot's done ends here)
	app.server.stop(false);	// SIGKILL: no handler race, no unbounded wait (kenshi-k16.2.271.1)
	_exit(code);
}

// One of the game's own events that the host posts (audio/events.hpp): its id and the name beside it for the audio thread's log. The name-based
// audiohook_event() hashes a std::string; this keeps the named constant at the call.
void post_event(uint32_t id, const char* name) { audiohook_ui({ id, 0, 0, name, {}, {} }); }

// Whether this run needs the game data and character assets (kenshi-k16.2.26): the characters step loads them, and the first step starts that load on a
// worker so it overlaps the terrain and map steps instead of blocking the window.
static bool wants_characters()
{
	return !app.opt.characters.empty() || !app.opt.simlink.empty() || app.local || !app.opt.creator.empty()
		|| dev::scenesNeedCharacters()	// a scene places char= specs (dev/scenetest.cpp)
		|| dev::labScripted();	// a headless animation lab run (the `lab` script op): no world, no characters in it
}


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
static bool world_to_screen(const double p[3], float& x, float& y)
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

const cfg::Int kFloaters{ cfg::Group{ "HUD and labels" }, "ui.floaters", 1, 0, 2, "Damage floaters over hit characters: 0 off, 1 simple, 2 full (the original's option, default simple)" };
// Over-head progress bars (SIMLINK TASK_PROGRESS 39, order 98): whose tasks show a bar. off none, original the bars the original game shows (the player's
// own tasks only), squad every task of your characters (producing, building, repairing, dismantling, processing, first aid too), everyone those plus the
// NPCs in view. The UI's Options row for it is k16.3.15.
const cfg::Text kProgressBars{ cfg::Group{ "HUD and labels" }, "ui.progress_bars", "original", { "off", "original", "squad", "everyone" },
	"Progress bars over working characters: off none, original the original's own bars (lockpicking, cutting, ...), squad every task of your characters, everyone those plus the NPCs in view" };

// Order 98's level for ui.progress_bars: off 0, original 1, squad 2, everyone 3 (SIMLINK order 98 / TASK_PROGRESS "Who gets rows").
static uint8_t progress_level()
{
	const std::string& v = kProgressBars.get();
	return (uint8_t)(v == "off" ? 0 : v == "squad" ? 2 : v == "everyone" ? 3 : 1);
}

// A TASK_PROGRESS kind's caption (SIMLINK 39 "kind"). 1-8 are the original's own strings (kinspect findings/ui_captures/progbar.md part A, the static
// callers' table); 9-16 are OURS: the original has no bar for a producer, construction, repair, dismantle, processing or first aid (progbar.md:
// "Construction, repair and dismantle do not use this bar"). nullptr: no bar (0, 15 crafting is never sent, anything newer).
static const char* progress_caption(uint8_t kind)
{
	switch (kind)
	{
	case 1: return "Lockpicking";		// the original's, with the chance suffix below
	case 2: return "Unlocking";			// the original's
	case 3: return "Locking";			// the original's
	case 4: return "Prospecting";		// the original's
	case 5: return "Adding materials";	// the original's
	case 6: return "Training";			// the original's
	case 7: return "Cutting";			// the original's, with the hardness suffix below
	case 8: return "Restraining";		// the original's
	case 9: return "Operating";			// OURS (the producer's own progress bar; the original shows none: has progress bar when used is 0 on the mines)
	case 10: return "Producing";		// OURS
	case 11: return "Building";			// OURS
	case 12: return "Repairing";		// OURS
	case 13: return "Dismantling";		// OURS
	case 14: return "Processing";		// OURS
	case 16: return "First aid";		// OURS
	default: return nullptr;
	}
}

// Where a character's progress bar and floating texts hang: the plumb line under the drawn body's own centre, not under its root (kenshi-h2v.16.7.1,
// user 2026-11-13: "the bar drifts side to side as the camera rotates"). The root is where the sim stands the character; a pose (the lockpicking crouch,
// a lean, a kneel) draws the body off it, up to ~1.5 u at the pelvis, and that offset seen from a rotating camera swings the body across the bar. The
// body's centre is the mean x / z of the head, pelvis and both feet, taken from this frame's pose (the hands swing and are left out); the height stays
// the placed (feet) one. A character not drawn, or without those bones, keeps the placed position. UNSURE: the original's own point is kenshi-rwh.115's
// (not answered yet); this is the drawn body's centre, which stays centred at any yaw by construction.
static void body_anchor(uint64_t id, double pos[3])
{
	const int index = app.sim.charIndex(id);
	if (index < 0) { return; }
	double sx = 0, sz = 0;
	int n = 0;
	for (const char* name : { "Bip01 Head", "Bip01 Pelvis", "Bip01 L Foot", "Bip01 R Foot" })
	{
		double b[3];
		if (!app.chars.boneWorld((size_t)index, name, b)) { return; }
		sx += b[0]; sz += b[2]; n++;
	}
	pos[0] = sx / n;
	pos[2] = sz / n;
}

// TASK_PROGRESS bars (SIMLINK 39, captions the client's) and the event floating texts (kinspect floating_text.md: 15 units over the character, rising), every frame
static void progress_frame()
{
	std::vector<ui::WorldProgress> bars;
	std::vector<ui::WorldFloatText> texts;
	const double now = app.sim.tick();
	if (app.sim.active() && now >= 0)
	{
		const simlink::SimLinkDecoder& d = app.sim.decoder();
		const bool barsOn = progress_level() != 0;	// off draws no over-head bars at all (the sim sends no rows then either)
		for (const simlink::SimLinkDecoder::TaskProgress& t : d.tasks)
		{
			const char* caption = progress_caption(t.kind);
			double pos[3];
			float hull[3];
			ui::WorldProgress b;
			if (!barsOn || !caption || !app.sim.placed(t.ch, pos, hull)) { continue; }
				body_anchor(t.ch, pos);
				if (!world_to_screen(pos, b.x, b.y)) { continue; }
			b.id = t.ch;
			b.text = caption;
			char v[32];
			if (t.kind == 1 && t.value > 0) { snprintf(v, sizeof v, " (Chance %.1f%%)", (int)(t.value * 1000) * 0.1); b.text += v; }
			if (t.kind == 7 && t.value > 0) { snprintf(v, sizeof v, ": %g", t.value); b.text += v; }
			b.progress = (ProgressFeed{ t.progress, t.rate, d.tasksTick }).at(now);
			bars.push_back(std::move(b));
		}
		uint64_t n = 0;
		for (const SimView::Popup& p : app.sim.popups())
		{
			// the exe's speed index: 1 for the lock roll, the damage and "Failed!" (4.0/s: age += 4 x dt, life 12 / 4 = 3 s) and 3 for the
			// landed KO's "*Thunk*" (10.0/s, life 1.2 s; s54_static.md 1b, the label keeps its create-time speed until success sets vf+0x30);
			// the label rides its character, `age` units above the + 15 anchor
			const double kSpeed = p.kind == 50 && p.result == 0 ? 10.0 : 4.0;
			ui::WorldFloatText f;
			f.age = (float)((now - p.tick) / SimView::kTickRate * kSpeed);
			if (f.age >= 12) { continue; }
			double feet[3];
			float hull[3];
			const bool placedNow = app.sim.placed(p.ch, feet, hull);
				if (placedNow) { body_anchor(p.ch, feet); }
				const double* base = placedNow ? feet : p.pos;
			if (p.kind == 1)
			{
				// receiveHit's floater: gated by the option, within 4000 of the camera
				const double dx = base[0] - app.pos[0], dz = base[2] - app.pos[2];
				if (kFloaters.get() == 0 || dx * dx + dz * dz > 4000.0 * 4000.0) { continue; }
				// N = (int) the final damage over all four groups (kinspect: receiveHit l.740, after armour, toughness and damageScale, as event 1);
				// Full: cut (+ pierce when > 1) and blunt + the armour stun part
				const float cut = p.damage[2] > 1 ? p.damage[0] + p.damage[2] : p.damage[0], blunt = p.damage[1] + p.damage[3];
				f.font = kFloaters.get() == 1 ? 1 : 0;	// Simple Medium, Full Small; picks Small (the default)
				// "(-M)": what armour stopped, before toughness (M = (int) the sum; each Full group's own (int)), shown only when > 0
				const auto stopped = [](float v) { return (int)v > 0 ? " (-" + std::to_string((int)v) + ")" : std::string(); };
				if (kFloaters.get() == 1) { f.text = std::to_string((int)(p.damage[0] + p.damage[1] + p.damage[2] + p.damage[3])) + stopped(p.absorbed[0] + p.absorbed[1]); }
				else { f.text = app.sim.sidOf(p.part) + ": " + std::to_string((int)cut) + stopped(p.absorbed[0]) + (blunt >= 1 ? " + " + std::to_string((int)blunt) + stopped(p.absorbed[1]) : ""); }	// the part's sid, not yet the game's limb label
				f.colour = app.sim.isPlayer(p.ch) ? ui::Color{ 1.0f, 0.31f, 0.31f, 0.9f } : app.sim.isPlayer(p.other) ? ui::Color{ 0.11f, 0.60f, 0.11f, 0.9f } : ui::Color{ 0.9f, 0.9f, 0.9f, 0.9f };
			}
			else if (p.kind == 20)	// lock_roll (event 20): the original's strings
			{
				f.text = p.result ? "Pick success!" : "Pick failed";
				f.colour = p.result ? ui::Color{ 0.4f, 0.8f, 0.4f, 0.9f } : ui::Color{ 0.7f, 0.4f, 0.4f, 0.7f };
			}
			else if (p.kind == 50)	// stealth_ko (event 50): "*Thunk*" green on a landed KO, "Failed!" red on a failed roll
			{
				// The exe's label (s54_static.md 1b): created as "Failed!" with font index 1 = Kenshi_FloaterFont_Medium, on success recoloured
				// and setText'd to "*Thunk*". Result 1 (the roll failed) and 2 (aborted, the victim was aware) both stay "Failed!": the exe's
				// abort path is unread (STEALTH_KO.md D2) and has no event of its own. Its " (No XP)" suffix is its local XP rule, not on the
				// wire: left out.
				f.text = p.result == 0 ? "*Thunk*" : "Failed!";
				f.font = 1;	// Kenshi_FloaterFont_Medium (the exe's), not the Small default
				f.colour = p.result == 0 ? ui::Color{ 0.4f, 0.8f, 0.4f, 0.9f } : ui::Color{ 0.7f, 0.4f, 0.4f, 0.7f };
			}
			else { continue; }	// no other kind is a popup (SimView::answers pushes 1, 20 and 50)
			const double at[3] = { base[0], base[1] + 15 + f.age, base[2] };
			if (!world_to_screen(at, f.x, f.y)) { continue; }
			f.id = ++n;
			texts.push_back(std::move(f));
		}
	}
	app.progressBars->update(std::move(bars));
	app.progressBars->floatTexts(std::move(texts));
}

// The player characters' name labels (the original's CharacterNameTag, kenshi-inspect s54_static.md 3): every player-faction character not dead
// (UNSURE: the original's tag on a corpse), at the feet + hull height + 5 (body scale 1: not on the wire), within ~10 950 units of the camera;
// none while F7 hides the interface. Sneaking shows the stealth icon as "hidden" and the light icon at full light until the sim sends detection and
// the light level (not on the wire yet).
static bool char_cut_hidden(uint64_t id, const double pos[3]);	// the body's floor cut (its definition reads app; below cut_hidden)
const cfg::Bool kShowNames{ cfg::Group{ "HUD and labels" }, "ui.show_names", true, "Show character names: the player characters' names over their heads (the original's \"Show names\")" };
static void name_labels_frame()
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
static void speech_frame()
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
static void dialogue_frame()
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
static double camera_near()
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
bool see_through(const double p[3], double bottom = 1e30, std::string* why = nullptr)
{
	const float* x = app.buildings.xray;
	if (x[3] > 0)
	{
		const double c[3] = { p[0] - app.pos[0], p[1] - app.pos[1], p[2] - app.pos[2] };
		const double len2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
		const double t = (c[0] * x[0] + c[1] * x[1] + c[2] * x[2]) / len2;
		const double back = (1 - t) * std::sqrt(len2);
		const double d = std::sqrt(std::pow(c[0] - x[0] * t, 2) + std::pow(c[1] - x[1] * t, 2) + std::pow(c[2] - x[2] * t, 2));
		if (t < 1 && back > 8 && d < 0.85 * std::min<double>(x[3], 0.35 * back))
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
static bool cut_hidden(const double pos[3])
{
	if (!app.buildings.cutHides(pos)) { return false; }
	const double up[3] = { pos[0], pos[1] + 2, pos[2] }, down[3] = { 0, -1, 0 };
	double t;
	float box[6];
	bool solid = false;
	if (app.collide.ray(up, down, 8, t, box, {}, nullptr, &solid) && solid) { return app.buildings.cutHides(pos, box[1]); }
	return true;
}

static bool char_cut_hidden(uint64_t id, const double pos[3])
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

// A collision shape the click ray passed over and why (kenshi-k16.2.259): the F12 shot's pick report and `dump pick` name each.
struct PickSkip { std::string name; double at[3]; std::string why; };

// The first collision shape along the ray within far that the view draws (see_through: cut-away floors and roofs pass); building: it is a building's.
// kenshi-k16.2.259 (user 2026-11-15, "pick only what is drawn"): eye (the camera's position, null off the pick) - a shape the ray starts inside is passed
// when it is not drawn at the camera either, MINE: the original reports a shape the ray starts inside at distance 0 (kenshi-rwh.142). skipped: each shape
// passed over and why, for the report.
static bool ray_shapes(const double origin[3], const double dir[3], double far, double hit[3], bool* building = nullptr,
	const double* eye = nullptr, std::vector<PickSkip>* skipped = nullptr, float* hitBox = nullptr)
{
	double from[3] = { origin[0], origin[1], origin[2] }, t;
	float box[6], skippedBox[6] = {};	// a shape passed through is passed whole (its exit face too), told apart by its bounds
	bool solid = false;
	std::string name;
	for (int n = 0; n < 16 && app.collide.ray(from, dir, far, t, box, {}, nullptr, &solid, building, skipped ? &name : nullptr); n++)
	{
		double at[3];
		for (int k = 0; k < 3; k++) { at[k] = from[k] + dir[k] * t; }
		const bool passed = std::equal(box, box + 6, skippedBox);
		if (!passed)
		{
			std::string why;
			const bool inside = eye && eye[0] >= box[0] && eye[0] <= box[3] && eye[1] >= box[1] && eye[1] <= box[4] && eye[2] >= box[2] && eye[2] <= box[5];
			bool hidden = false;
			if (inside && see_through(eye, 1e30, &why))	// MINE (kenshi-k16.2.259): the camera is inside it and it is not drawn there either
			{
				hidden = true;
				why = "the camera is inside it, " + why;
			}
			else { hidden = see_through(at, solid ? box[1] : 1e30, &why); }	// a staircase counts by the floor it starts on (drawn whole); a wall by its height
			if (!hidden)
			{
				std::copy(at, at + 3, hit);
				if (hitBox) { std::copy(box, box + 6, hitBox); }
				return true;
			}
			if (skipped) { skipped->push_back({ name.empty() ? "an unnamed shape" : name, { at[0], at[1], at[2] }, why }); }
		}
		std::copy(box, box + 6, skippedBox);
		constexpr double kStep = 0.5;	// past the face just hit
		for (int k = 0; k < 3; k++) { from[k] = at[k] + dir[k] * kStep; }
		far -= t + kStep;
	}
	return false;
}

// A scripted run waits for the collision gather before every pick so its clicks are deterministic; the player never waits (the pick uses the shapes
// it has, the worker swaps the new cell's in). Off, a script measures the pick as the player meets it (kenshi-k16.2.205.10: the wait was the whole 92-353 ms
// of sim.peek+hover on a camera jump to a fresh zone cell, Hub 22k shapes ~170 ms).
const cfg::Bool kStreamScripted{ cfg::Group{ "Scripted runs" }, "debug.stream_scripted", false, "Scripted runs: stream the buildings as a live run does (a frame's decodes under the per-frame count, the creates in the render queue's stream lane; kenshi-k16.2.205.12.35) instead of waiting for every mesh and texture each frame. For a walk across a streaming boundary; the scene test leaves it off" };
const cfg::Bool kPickWait{ cfg::Group{ "Scripted runs" }, "debug.pick_wait", true, "Scripted runs: wait for the collision gather before each pick (deterministic clicks). Off: pick with the shapes at hand, as the player does" };

// The click ray's world hit: the nearer of the terrain and the collision shapes round the camera (SIMLINK 13: the picker's bottom layer), passing
// through the shapes where they are see_through (a click there is on what shows behind; user 2026-11-03). False: neither. building: the hit is a
// building's shape. kenshi-k16.2.259: origin is the near-plane start of the pick ray, eye the camera's position (ray_shapes), skipped the report's shapes.
bool ray_hit(const double origin[3], const double dir[3], double hit[3], bool* building, const double* eye = nullptr, std::vector<PickSkip>* skipped = nullptr, float* hitBox = nullptr)
{
	*building = false;
	if (app.scripted() && kPickWait.get()) { app.collide.wait(); }
	const bool ground = ray_ground(origin, dir, hit);
	double far = 20000;
	if (ground)
	{
		far = std::sqrt(std::pow(hit[0] - origin[0], 2) + std::pow(hit[1] - origin[1], 2) + std::pow(hit[2] - origin[2], 2));
		// kenshi-k16.2.254.1: the terrain inside a carved interior is the dune a sunk building sits in — the interior mask clips it and it is not drawn
		// (interiorclip.inc) — so it is not the click's ground: the floor is, the same rule the walkers draw by (kyat.2, WorldBuildings::insideInterior).
		// The ray goes on to the drawn floor inside the hull, out to where it leaves the hull's box; an outdoor click ends on the terrain as before.
		double exitT = 0;
		if (app.buildings.insideInterior(hit, app.pos, dir, &exitT) && exitT > far) { far = exitT + 1.0; }
	}
	const bool prevCuts = app.buildings.levelCuts;
	app.buildings.levelCuts = true;	// the click pick: buildings nobody is in are cut at the floor selector's level (WorldBuildings::levelCuts)
	const bool shape = ray_shapes(origin, dir, far, hit, building, eye, skipped, hitBox);
	app.buildings.levelCuts = prevCuts;
	if (shape) { return true; }
	*building = false;
	return ground;
}

// The floating camera's ground at (x, z): the highest surface drawn there, a building floor or roof (cut-away ones pass) or a rock, else the
// terrain (kinspect camera_rts.md 3: a roof or floor under the pivot, ray from +3000, holds its y). on: "floor" or "terrain".
static double ground_under(double x, double z, const char** on)
{
	constexpr double kTop = 3000;
	const double terrain = app.hm->height(x, z);
	const double from[3] = { x, terrain + kTop, z }, down[3] = { 0, -1, 0 };
	double hit[3];
	app.buildings.peekPass = false;	// the camera rests on what stands there, not on what the cursor peeks through
	const bool shape = ray_shapes(from, down, kTop, hit) && hit[1] > terrain;
	app.buildings.peekPass = true;
	*on = shape ? "floor" : "terrain";
	return shape ? hit[1] : terrain;
}

// What the mouse ray hits first: Pick, and its doc, are in pick.hpp (kenshi-k16.2.294.2, step 0 of the control-hints design); pick_at below fills it.
constexpr double kDoorMatch = 150;
constexpr double kProducerMatch = 50;	// MINE: a collision hit within this (XZ) of a producer's placement picks it (deposit rocks, a mine's shell)
constexpr double kPropBoxSlack = 0.5;	// how far past a container's drawn box a click still counts as the container (the sim's collision proxy for the piece sits a thousandth of a unit nearer than its mesh box, k16.2.239.1)

// Ground item name labels (the original's DropItemLabels; kenshi-inspect picker.md): every item within kLabelRange shows one while ALT (the
// original's `highlight` key) is held, otherwise only the hovered item's, which stays while the mouse is on the label itself. The box is the
// hit area, edges included: kLabelHeight of the screen height high, the text plus kLabelPad wide, standing on the item position + 4 y, clamped
// on screen. LMB on a label picks the item up (Shift queues), and the world pick under a label is skipped. With all labels shown, overlaps
// are pushed apart as the original's 140163b70 does (spread_labels). Nearest item last, so it draws on top and is hit first.
struct ItemLabel { uint64_t id; float x, y, w, h; std::string text; double dist; };
constexpr float kLabelHeight = 0.0259f, kLabelPad = 6.0f;	// pad: pixels at 720p (Cup 27, Hemp 38, Wooden Bowl 80 wide)
constexpr double kBedReach = 10, kBedLift = 3;	// a bed's pick sphere over its sleeping spot: MINE (a single bed is about 10 x 20 u)
constexpr double kLabelRange = 707, kLabelLift = 4, kContainerLabelLift = 10, kProducerLabelLift = 20;	// producers: MINE	// containers: MINE (above a crate; the original's container label is not measured)

// Label text: the UI style's tag font (ui::drawTag's default) when the UI is up, else the debug font
const ui::Font* label_font() { return app.uiOn ? ui::fonts::get("Kenshi_StandardFont_Medium") : nullptr; }
float label_text_width(const std::string& text)
{
	const ui::Font* f = label_font();
	return f ? f->width(text) : 8.0f * text_scale() * text.size();
}
float label_height()
{
	const ui::Font* f = label_font();
	return std::max(kLabelHeight * view::heightf(), (f ? f->lineHeight() : 8.0f * text_scale()) + 2.0f);
}

// A held action key (ui::keys); not while an edit box has the keyboard
bool title_shown() { return app.mainMenu && app.mainMenu->shown(); }
std::string title_status();	// the title's offline line (main.cpp, frame_title): empty when a game can start
bool title_hold_dev(int button);	// dev script `titlehold` (main.cpp, below hold_menu_action)
bool menu_shown() { return title_shown() || (app.gameMenu && app.gameMenu->shown()) || (app.tutorials && app.tutorials->browserShown()) || (app.newGame && app.newGame->shown()) || (app.saves && app.saves->shown()); }	// the game's keys are off
static bool held(const char* action) { return !menu_shown() && !dynamic_cast<ui::EditBox*>(ui::focusedWidget()) && ui::keys::held(action); }
bool labels_shown() { return held("highlight"); }

// MINE (user's extension): name labels over characters too while ALT is held, off by default. Drawn only, not click targets. Machines have no
// sim entity yet, so there is no option for them.
// The move marker's mesh (data/meshes/position.mesh), loaded on first use; a missing or broken file is reported once and draws nothing
static const ogre::Mesh* move_marker()
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
const cfg::Float kSunShafts{ cfg::Group{ "Indoor lighting (experimental)" }, "render.sun_shafts", 0.0, 0.0, 10.0, "Experimental (kenshi-jj3.7.1): visible sunbeams in the air inside buildings (a ray march against the sun's shadow map through the baked probe volumes of the nearest buildings); the value scales the haze density (0 = off, 1 = the default look)" };
const cfg::Float kLampBeams{ cfg::Group{ "Indoor lighting (experimental)", "turns render.lamp_occlusion's baked lamp visibility on while above 0" }, "render.lamp_beams", 0.0, 0.0, 1.0, "Experimental, our own effect (kenshi-jj3.28.1; the original has no beams): a faint volumetric cone under each shaded hanging lamp (a spot light pointing down) in the air of the nearest baked buildings, in the lamp's colour, fading with distance and stopped at the lamp's own walls and floors by the baked lamp visibility (so it needs and switches on render.lamp_occlusion's bake). 1 is clearly fainter than render.sun_shafts 1; 0 = off" };
const cfg::Float kDustMotes{ cfg::Group{ "Indoor lighting (experimental)", "works without render.sun_shafts" }, "render.dust_motes", 0.0, 0.0, 10.0, "Experimental (kenshi-jj3.8.1): drifting dust motes in the air of the nearest baked building that show only inside a sunbeam (the sun's shadow lookup of render.sun_shafts, no ambient); the value scales the mote count (0 = off, 1 = the default)" };
#ifdef KCLIENT_DEV_TOOLS
const cfg::Int kSunShaftsDebug{ cfg::Group{ "Light debug", "needs render.sun_shafts above 0", "Debug" }, "render.sun_shafts_debug", 0, 0, 8, "Debug (kenshi-jj3.7.3; needs render.sun_shafts > 0): 1 = the sun visibility the march reads, averaged over each view ray (white = lit air); 2 = the sun visibility at the scene surface (compare with the sunlit floor patch); 3 = the normal beams with the sun forced lit (the density check); 4 = the baked enclosure the indoor density follows, averaged over the ray; 5 = the volume weight (the probe box's edge ramp) along the ray; 6 = the indoor density gate x weight; 7 = the accumulated in-scatter x8; 8 = the density the march used x5 (a scale check)" };
#endif
const cfg::Bool kShadowCache{ cfg::Group{ "Shadows" }, "render.shadow_cache", true, "Cache the sun shadow maps' static casters (terrain, buildings, features, foliage; kenshi-k16.2.205.12.2): each cascade's tile is redrawn only when its snapped centre, the sun, the streamed content or a door changes, and the characters and wind-turned parts are drawn over a restored copy each frame. Off: every cascade redraws every caster every frame" };
const cfg::Int kShadowStaticBudget{ cfg::Group{ "Shadows" }, "render.shadow_static_budget", 1, 0, 4, "With render.shadow_cache: the most cached shadow tiles a frame redraws only because their content changed (buildings streamed in, a door, terrain; kenshi-k16.2.205.12.73). The others keep their old content a few frames (at most 3); a tile the camera or the sun moved redraws at once. 0: every changed tile in the same frame" };
const cfg::Float kStreamMs{ cfg::Group{ "Performance" }, "render.stream_ms", 4.0, 0.0, 50.0, "The time a frame gives the streamed resource creates (buildings, characters: the render queue's stream lane; kenshi-k16.2.205.12.74), ms. A create that would run past it waits for the next frame; one always runs. 0: no time budget (the 8 MB per frame byte cap only)" };
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
const cfg::Text kShotDir{ cfg::Group{ "Screenshots" }, "screenshot.dir", "", "Where the screenshot key saves (and its .txt scene notes); empty: $XDG_DATA_HOME/kclient/screenshots (~/.local/share)" };
const cfg::Bool kPeek{ cfg::Group{ "Building see-through" }, "camera.peek", true, "Pointing at a building's inside from outside shows its walls as a ghost (ours; the original has none; playtest)" };
const cfg::Text kSpeedUnit{ cfg::Group{ "HUD and labels" }, "ui.speed_unit", "kph", { "kph", "mph" }, "The unit of speeds in the panels (run speed, wind); the original shows mph" };
// u/s -> the ui.speed_unit (1 u = 10 cm; mph: kinspect run_speed_mph.md, the exe's float)
#ifdef KCLIENT_DEBUG_VIEW
#endif
static float speedScale() { return kSpeedUnit.get() == "mph" ? 0.2236936f : 0.36f; }
static ui::HudBuildingInfo machine_panel();	// the selected producer's focus panel (k16.19.7.1): the HUD fill and `dump machine` read it
const cfg::Bool kOccluderGhost{ cfg::Group{ "Building see-through", "needs camera.occluder_fade" }, "camera.occluder_ghost", true, "The see-through is the whole building fading to glass (clicks pass it), else a cone round the character" };
const cfg::Bool kInteriorMask{ cfg::Group{ "Interiors" }, "render.interior_mask", true, "Clip the terrain, grass and weather particles inside buildings by the buildings' interior mask hulls (the game's InteriorMask target), so a sunk building's interior is not filled by the dune; off for comparisons" };
const cfg::Bool kTitleAtStart{ cfg::Group{ "Screens" }, "ui.title_at_start", true, "Start on the title screen (interactive runs)" };
const cfg::Text kServerExe{ cfg::Group{ "Single player" }, "session.server", "", "Single player: the OpenKenshi-server to start (empty: the one built with this client, its build dir's cpp/OpenKenshi-server: target ksim_server)" };
const cfg::Bool kVsync{ cfg::Group{ "Performance" }, "gfx.vsync", true, "Wait for the monitor's refresh before showing a frame. Takes effect after a restart (the driver may override it)" };
const cfg::Int kFpsCap{ cfg::Group{ "Performance" }, "gfx.fps_cap", 0, 0, 1000, "Frame rate limit, 0 none (applies at once; with vsync on, the refresh rate already limits it)" };
const cfg::Int kSessionLoadScreen{ cfg::Group{ "Single player" }, "session.loading_screen", 1, 0, 2, "A loading screen until the world round the squad has streamed in (new game, load, connect): 0 off, 1 on (not in scripted runs), 2 always" };
const cfg::Text kServerArgs{ cfg::Group{ "Single player" }, "session.server_args", "--scenario newgame", "Single player: the server's arguments, split at spaces (e.g. add --navworld DIR, --save-dir DIR)" };
const cfg::Bool kCharacterLabels{ cfg::Group{ "HUD and labels" }, "ui.labels.characters", false, "Holding ALT also shows character names" };

// The original's 140163b70 on whole-pixel boxes: sort by top then left, then up to 10 passes over every overlapping pair (edges count). Each
// overlap is measured on both axes as |overlap| * 0.5 + 1. Both boxes move that far apart, away from each other, on the axis that needs less;
// the sixth pass always moves sideways. A pass with no overlap ends it. Not re-clamped on screen, as in the original.
void spread_labels(std::vector<ItemLabel>& labels)
{
	struct Box { int l, t, r, b; };
	std::vector<Box> box;
	for (const ItemLabel& l : labels) { box.push_back({ (int)l.x, (int)l.y, (int)(l.x + l.w), (int)(l.y + l.h) }); }
	std::vector<size_t> order(labels.size());
	for (size_t i = 0; i < order.size(); i++) { order[i] = i; }
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return box[a].t != box[b].t ? box[a].t < box[b].t : box[a].l < box[b].l; });
	for (int pass = 10; pass > 0; pass--)
	{
		bool moved = false;
		for (const size_t i : order)
		{
			for (const size_t j : order)
			{
				Box& a = box[i];
				Box& b = box[j];
				if (i == j || b.l > a.r || a.l > b.r || b.t > a.b || a.t > b.b) { continue; }
				const int dx = (int)(std::abs(std::min(a.r, b.r) - std::max(a.l, b.l)) * 0.5f + 1.0f);
				const int dy = (int)(std::abs(std::min(a.b, b.b) - std::max(a.t, b.t)) * 0.5f + 1.0f);
				if (dy < dx && pass != 5)
				{
					const int d = b.t < a.t ? -dy : dy;
					a.t -= d; a.b -= d; b.t += d; b.b += d;
				} else {
					const int d = b.l < a.l ? -dx : dx;
					a.l -= d; a.r -= d; b.l += d; b.r += d;
				}
				moved = true;
			}
		}
		if (!moved) { break; }
	}
	for (size_t i = 0; i < labels.size(); i++) { labels[i].x = (float)box[i].l; labels[i].y = (float)box[i].t; }
}

std::vector<ItemLabel> item_labels(bool all, const float* cam = nullptr)	// cam: the mvp to project with (record_ui: the packet's); null: build_mvp
{
	std::vector<ItemLabel> out;
	if (!app.sim.active()) { return out; }
	float mvp[16];
	if (cam) { std::copy(cam, cam + 16, mvp); }
	else { build_mvp(mvp, 1.0f, 1000.0f); }
	for (const auto& [id, th] : app.sim.things())
	{
		const bool producer = th.kind == simlink::Interactable::kProducer;	// labelled only while hovered (MINE)
		if ((th.kind != simlink::Interactable::kItem && th.kind != simlink::Interactable::kContainer && !producer) || ((!all || producer) && id != app.hoverThing)) { continue; }
		if (const double at[3] = { th.pos[0], th.pos[1], th.pos[2] }; cut_hidden(at)) { continue; }	// on a floor the cut hides
		const double lift = producer ? kProducerLabelLift : th.kind == simlink::Interactable::kContainer ? kContainerLabelLift : kLabelLift;
		const float p[3] = { (float)(th.pos[0] - app.pos[0]), (float)(th.pos[1] + lift - app.pos[1]), (float)(th.pos[2] - app.pos[2]) };
		if (all && id != app.hoverThing && std::hypot(p[0], p[1], p[2]) > kLabelRange) { continue; }
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { continue; }
		std::string text = app.sim.nameOf(producer ? th.output : th.record, app.chars);	// a producer: its output and fill, "Iron Ore 3/5" (MINE)
		if (producer) { text += " " + std::to_string(th.count) + "/" + std::to_string(th.cap); }
		else if (th.count > 1) { text += " (" + std::to_string(th.count) + ")"; }
		const float w = label_text_width(text) + kLabelPad * view::heightf() / 720.0f, h = label_height();
		const float sx = (c[0] / c[3] * 0.5f + 0.5f) * view::widthf(), sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
		if (sx < 0 || sx > view::widthf() || sy < 0 || sy > view::heightf()) { continue; }	// the item is off screen: no label (the box alone is clamped)
		const float x = std::clamp(sx - w * 0.5f, 0.0f, std::max(0.0f, view::widthf() - w)), y = std::clamp(sy - h, 0.0f, std::max(0.0f, view::heightf() - h));
		out.push_back({ id, x, y, w, h, std::move(text), std::hypot(p[0], p[1], p[2]) });
	}
	if (all) { spread_labels(out); }
	std::sort(out.begin(), out.end(), [](const ItemLabel& a, const ItemLabel& b) { return a.dist > b.dist; });
	return out;
}

// The item whose shown label is under (mx, my); 0: none
uint64_t label_at(double mx, double my)
{
	const std::vector<ItemLabel> labels = item_labels(labels_shown());
	for (auto l = labels.rbegin(); l != labels.rend(); ++l)
	{
		if (mx >= l->x && mx <= l->x + l->w && my >= l->y && my <= l->y + l->h) { return l->id; }
	}
	return 0;
}

// Each door's pose for this frame, shared by the drawn leaf (WorldBuildings::setDoors) and the pick (pick_at): the wire's state sets the target
// (open / broken: 1, closed / locked: 0) and the openness slides to it at `door move speed` per second (the Door_Shut cue's 1 / speed travel; the
// original's own motion curve is not known: UNSURE, linear), a door seen for the first time stands at its target. The offset is doorpose.hpp's.
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
		if (!pose.seen) { pose.open = target; pose.seen = true; }
		else if (pose.open < target) { pose.open = std::min(target, pose.open + dt * d.speed); }
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

// The click's position test (kenshi-k16.2.259), cheap enough for every thing along the ray: pos is not drawn — in a ghosted occluder, on a sliced
// building's hidden floor or, with levelCuts set around a pick, on any building's floor above the level. WorldBuildings::cutAway is the one test the
// renderer's own cut and fade use, so a click on a thing there passes to what shows behind it.
static bool pick_hidden(const double pos[3], std::string* why = nullptr) { return app.buildings.cutAway(pos, 1e30, why); }

// A thing's own pick box the cut hides goes in the report too (kenshi-k16.2.259: the water tank's prop box on floor 1 over a floor-0
// click): the name from the wire's record, at the box's hit point, with the cut's reason.
static void note_thing_skip(std::vector<PickSkip>* skipped, const char* kind, const simlink::Interactable& th, const double at[3], const std::string& why)
{
	if (!skipped) { return; }
	const std::string n = app.sim.nameOf(th.record, app.chars);
	skipped->push_back({ std::string(kind) + " \"" + (n.empty() ? "no record name" : n) + "\"", { at[0], at[1], at[2] }, why });
}

Pick pick_at(const double dir[3], std::vector<PickSkip>* skipped = nullptr)
{
	Pick p;
	double from[3];	// the ray starts at the near clip plane, as the original's does (kenshi-k16.2.259, camera_near)
	for (int k = 0; k < 3; k++) { from[k] = app.pos[k] + dir[k] * camera_near(); }
	const bool prevCuts = app.buildings.levelCuts;
	app.buildings.levelCuts = true;	// every layer below skips what the cut hides, not only the world's shapes
	double best = 1e30;
	float worldBox[6] = {};	// the box of the collision shape the world hit is, when it is one (not the terrain)
	if (ray_hit(from, dir, p.hit, &p.building, app.pos, skipped, worldBox))
	{
		p.kind = Pick::Ground;
		best = std::sqrt(std::pow(p.hit[0] - from[0], 2) + std::pow(p.hit[1] - from[1], 2) + std::pow(p.hit[2] - from[2], 2));
	}
	const double world = best;
	double t;
	// doors: the pose update_doors keeps per door (the drawn leaf's own): the leaf shapes sit where the leaf is drawn, closed in the frame, open
	// (or broken) slid aside, between them mid-travel; a door with no interactable has no pick
	const auto leaf = [&](const kworld::WorldShape& s, double off[3])
	{
		const auto it = app.doorPose.find(s.door);
		if (it == app.doorPose.end() || !it->second.thing) { return false; }
		if (!it->second.exact && it->second.open > 0) { return false; }	// not a box: no leaf normal, left out while open
		for (int k = 0; k < 3; k++) { off[k] = it->second.off[k]; }
		return true;
	};
	if (kworld::DoorKey key; app.collide.ray(from, dir, world, t, nullptr, leaf, &key) && app.doorPose[key].thing)
	{
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		if (!see_through(at))	// a hidden building's doors do not pick
		{
			p.kind = Pick::Door;
			p.id = app.doorPose[key].thing;
			best = t;
			std::copy(at, at + 3, p.hit);
		}
	}
	for (const auto& [id, th] : app.sim.things())	// producers: by their placement's mesh boxes (the original's hover covers the whole mine)
	{
		if (th.kind != simlink::Interactable::kProducer || !app.buildings.propRay(id, from, dir, t) || t > std::min(best, world)) { continue; }
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		std::string why;
		if (pick_hidden(at, &why))	// a producer on a floor the cut hides is not drawn (k16.2.259)
		{
			note_thing_skip(skipped, "producer", th, at, why);
			continue;
		}
		p.kind = Pick::Producer;
		p.id = id;
		best = t;
		std::copy(at, at + 3, p.hit);
	}
	for (const auto& [id, b] : app.sim.decoder().buildings)	// the player's drawn buildings, like a world building's shape: Ground with `building` (k16.2.36.1)
	{
		if (app.buildings.propInstance(id) < 0 || !app.sim.playerBuilding(b)) { continue; }
		if (app.buildings.propRay(id, from, dir, t) && t <= std::min(best, world))
		{
			const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
			if (pick_hidden(at)) { continue; }
			p.kind = Pick::Ground;
			p.building = true;
			p.id = id;
			std::copy(at, at + 3, p.hit);
			best = t;
		}
	}
	// no placement (a deposit is a foliage rock): the rock's own collision, a hit shape whose footprint holds the producer's origin, or else a hit within
	// kProducerMatch of it
	if (float box[6]; p.kind != Pick::Producer && app.collide.ray(from, dir, std::min(best, world) + 1, t, box))
	{
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		double near = kProducerMatch * kProducerMatch;
		if (!pick_hidden(at))
		{
			for (const auto& [id, th] : app.sim.things())
			{
				if (th.kind != simlink::Interactable::kProducer) { continue; }
				const bool own = th.pos[0] >= box[0] && th.pos[0] <= box[3] && th.pos[2] >= box[2] && th.pos[2] <= box[5];
				const double d2 = own ? 0.0 : std::pow(th.pos[0] - at[0], 2) + std::pow(th.pos[2] - at[2], 2);
				if (d2 < near || (own && near > 0))
				{
					near = d2;
					p.kind = Pick::Producer;
					p.id = id;
					best = t;
					std::copy(at, at + 3, p.hit);
				}
			}
		}
	}
	// the item sphere, or a drawn container's mesh box; a thing on a floor the cut hides is not drawn, so the ray goes through it (k16.2.259)
	const auto thingHidden = [&](uint64_t id)
	{
		const simlink::Interactable* th = app.sim.thing(id);
		if (!th) { return true; }
		if (th->kind == simlink::Interactable::kContainer && app.buildings.hasMesh(id)) { return true; }	// a drawn container: its mesh box picks, below
		const double at[3] = { th->pos[0], th->pos[1], th->pos[2] };
		std::string why;
		if (!pick_hidden(at, &why)) { return false; }
		const bool box = th->kind == simlink::Interactable::kContainer;
		const double reach = box ? SimView::kContainerReach : SimView::kItemReach;	// where the ray would have met it, its own hit point
		const double c[3] = { at[0] - from[0], at[1] + (box ? SimView::kContainerLift : SimView::kItemLift) - from[1], at[2] - from[2] };
		const double along = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
		const double d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - along * along;
		double t2 = std::max(0.0, along - std::sqrt(std::max(0.0, reach * reach - d2)));
		const double hit[3] = { from[0] + dir[0] * t2, from[1] + dir[1] * t2, from[2] + dir[2] * t2 };
		note_thing_skip(skipped, box ? "container" : "item", *th, hit, why);
		return true;
	};
	if (const uint64_t id = app.sim.pickItem(from, dir, &t, thingHidden); id && t <= (best < world ? best : world + SimView::kContainerReach))	// a door or producer in front wins; the world only past the pick sphere (an item on a table sits in its collision box)
	{
		const simlink::Interactable* th = app.sim.thing(id);
		p.kind = th && th->kind == simlink::Interactable::kContainer ? Pick::Container : Pick::Item;
		p.id = id;
		best = t;
		for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * t; }
	}
	int backed = 0;	// the world shape's drawn backing, tested on the first container behind it: 0 not yet, 1 none (undrawn proxy), 2 drawn
	for (const auto& [id, th] : app.sim.things())	// drawn containers: by their mesh boxes (a container a layout piece draws: that piece's, any visible part of it)
	{
		if (th.kind != simlink::Interactable::kContainer) { continue; }
		if (!app.buildings.propRay(id, from, dir, t, true))
		{
			// the ray misses the drawn triangles; a hidden container whose box it still crosses is named in the report, as before the refinement
			double tb = 0;
			if (app.buildings.propRay(id, from, dir, tb))
			{
				const double at[3] = { from[0] + dir[0] * tb, from[1] + dir[1] * tb, from[2] + dir[2] * tb };
				std::string why;
				if (pick_hidden(at, &why)) { note_thing_skip(skipped, "container", th, at, why); }
			}
			continue;
		}
		// the click is on the container's own drawn box while the collision proxy the sim bakes for the piece sits just outside it (a counter's
		// face, k16.2.239.1): the box entry is a hair behind the world hit, so the container still picks; any other shape really in front (a wall,
		// a floor edge) puts the hit point outside the box and keeps the click
		double lo[3], hi[3];
		const bool onProp = best == world && app.buildings.propBox(id, lo, hi, kPropBoxSlack) && p.hit[0] >= lo[0] && p.hit[0] <= hi[0] &&
			p.hit[1] >= lo[1] && p.hit[1] <= hi[1] && p.hit[2] >= lo[2] && p.hit[2] <= hi[2];
		// a building's collision shape in front of the container is an undrawn proxy volume unless something is drawn inside it along the ray (Storm House's
		// staircase body: the barrel on the floor behind it lost its whole drawn body, k16.2.239.4.1): the shape holds the click only when a drawn surface backs it
		if (t > best && !onProp && best == world && p.kind == Pick::Ground && p.building && !p.id)
		{
			if (!backed)
			{
				// the span of the ray inside the shape's box, up to the container's hit
				double t0 = 0, t1 = 1e30;
				const double blo[3] = { worldBox[0], worldBox[1], worldBox[2] }, bhi[3] = { worldBox[3], worldBox[4], worldBox[5] };
				for (int a = 0; a < 3; a++)
				{
					const double lo2 = blo[a] - from[a], hi2 = bhi[a] - from[a];
					if (std::abs(dir[a]) < 1e-12) { if (lo2 > 0 || hi2 < 0) { t1 = -1; } continue; }
					t0 = std::max(t0, std::min(lo2 / dir[a], hi2 / dir[a]));
					t1 = std::min(t1, std::max(lo2 / dir[a], hi2 / dir[a]));
				}
				double tb = 0;
				bool drawn = t0 <= t1 && app.buildings.drawnHitIn(from, dir, blo, bhi, std::max(0.0, t0 - 1.0), std::min(t1 + 1.0, t), tb);
				if (drawn)
				{
					const double bat[3] = { from[0] + dir[0] * tb, from[1] + dir[1] * tb, from[2] + dir[2] * tb };
					drawn = !pick_hidden(bat);	// a surface the cut hides is not drawn
				}
				backed = drawn ? 2 : 1;
			}
			if (backed == 1) { best = 1e30; }	// no drawn surface inside the shape: it does not hide the container (nothing nearer held the click: best == world)
		}
		if (t > best && !onProp) { continue; }
		const double at[3] = { from[0] + dir[0] * t, from[1] + dir[1] * t, from[2] + dir[2] * t };
		std::string why;
		if (pick_hidden(at, &why))	// the water tank's box on floor 1 over a floor-0 click (k16.2.259): not drawn, the ray goes through it
		{
			note_thing_skip(skipped, "container", th, at, why);
			continue;
		}
		p.kind = Pick::Container;
		p.id = id;
		best = std::min(best, t);
		std::copy(at, at + 3, p.hit);
	}
	// beds (INTERACTABLE kind 5): the bed's own placed shape, the layout furniture piece holding the sleeping spot (its mesh box, oriented: a click
	// anywhere on the drawn bed picks it, not a sphere over the spot); a sphere only for a bed no drawn piece holds (MINE, k16.2.220)
	double bedAt[3] = {};
	{	// scope of the bed loop
	CPUSTAGE_ACC("pa.beds");	// the bed loop's time (+pa.beds; kenshi-k16.2.205.12.109 found 0.55-0.65 ms at Heft, .111 memoizes bedRay's search)
	for (const auto& [id, th] : app.sim.things())
	{
		if (th.kind != simlink::Interactable::kBed) { continue; }
		const double at[3] = { th.pos[0], th.pos[1], th.pos[2] };
		double entry = 0;
		const int piece = app.buildings.bedRay(at, from, dir, entry);
		bool hit = piece > 0;
		if (piece < 0)	// no drawn piece holds the spot
		{
			const double c[3] = { th.pos[0] - from[0], th.pos[1] + kBedLift - from[1], th.pos[2] - from[2] };
			const double along = c[0] * dir[0] + c[1] * dir[1] + c[2] * dir[2];
			const double perp2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2] - along * along;
			hit = along > 0 && perp2 < kBedReach * kBedReach;
			entry = along - std::sqrt(std::max(0.0, kBedReach * kBedReach - perp2));
		}
		if (!hit || entry > std::min(best, world + 1)) { continue; }
		// a bed on a floor the cut hides is not drawn: the ray goes through it (Storm House floor 0: the floor-1 beds sat over the stairs, k16.2.233.1).
		// Tested only for a bed that would take the pick: cut_hidden casts a collide ray per bed, most of the loop's time at Heft once bedRay's search
		// was memoized (kenshi-k16.2.205.12.111); a filter, so the pick is the same as testing it first.
		if (cut_hidden(at) || pick_hidden(at)) { continue; }
		p.kind = Pick::Bed;
		p.id = id;
		best = std::max(entry, 0.0);
		for (int k = 0; k < 3; k++) { bedAt[k] = at[k]; p.hit[k] = from[k] + dir[k] * best; }
	}
	}
	// a sleeper lies inside the bed's box, so it is behind the box's near face: the character wins over the bed it lies in. A character the floor cut
	// hides is not drawn (Characters::culled): the ray goes through it (k16.2.259: the draw's own test, so a character that shows still picks).
	const auto charHidden = [](uint64_t id)
	{
		double feet[3];
		float hull[3];
		return app.sim.placed(id, feet, hull) && cut_hidden(feet);
	};
	// a lying / asleep character is hit along its drawn skeleton (head - pelvis - both feet), not the sim's segment: the placed, animated body in the bed (k16.2.223)
	const auto drawnBody = [](uint64_t id, double pts[][3])
	{
		const int index = app.sim.charIndex(id);
		double head[3], pelvis[3], lf[3], rf[3];
		if (index < 0 || !app.chars.boneWorld((size_t)index, "Bip01 Head", head) || !app.chars.boneWorld((size_t)index, "Bip01 Pelvis", pelvis) ||
			!app.chars.boneWorld((size_t)index, "Bip01 L Foot", lf) || !app.chars.boneWorld((size_t)index, "Bip01 R Foot", rf)) { return 0; }
		const double* seg[3][2] = { { head, pelvis }, { pelvis, lf }, { pelvis, rf } };
		for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { pts[2 * j][k] = seg[j][0][k]; pts[2 * j + 1][k] = seg[j][1][k]; } }
		return 3;
	};
	if (const uint64_t id = app.sim.pick(from, dir, &t, charHidden, drawnBody); id)
	{
		bool inBed = false;	// the character lies in the picked bed: its sim position is on the sleeping spot
		if (double cp[3]; p.kind == Pick::Bed && app.sim.simPos(id, cp)) { inBed = std::hypot(cp[0] - bedAt[0], cp[2] - bedAt[2]) < kBedReach && std::fabs(cp[1] - bedAt[1]) < 4; }
		if (t <= (inBed ? std::max(best, world) + 2 * kBedReach : std::min(best, world + 3)))
		{
			p.kind = Pick::Character;
			p.id = id;
			for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * t; }
		}
	}
	app.buildings.levelCuts = prevCuts;
	return p;
}

// What is under the screen point: an item label (while shown), else the world pick along the ray. skipped (the F12 report): the shapes the ray passed.
Pick pick_screen(double mx, double my, std::vector<PickSkip>* skipped = nullptr)
{
	if (const int bar = app.post.letterboxBar(); my < bar || my >= view::height() - bar) { return {}; }	// the letterbox bars cover the world: nothing there to pick
	if (const uint64_t id = label_at(mx, my))
	{
		Pick p;
		const simlink::Interactable* th = app.sim.thing(id);
		p.kind = !th ? Pick::Item : th->kind == simlink::Interactable::kContainer ? Pick::Container : th->kind == simlink::Interactable::kProducer ? Pick::Producer : Pick::Item;
		p.id = id;
		return p;
	}
	double dir[3];
	mouse_ray(mx, my, dir);
	return pick_at(dir, skipped);
}

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

// The pick chance the hover tag shows: order 99 on the cage key asked once per (cage, acting character); its MENU_ENTRIES carries the chance.
struct CageHover { uint64_t cage = 0, actor = 0; uint32_t seq = 0; float chance = -1; };
CageHover cageHover;
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
	if (app.look) { hoverRule = "mouse look: no hover decision (the cursor is hidden)"; return; }
	if (app.boxDown && app.boxDrag) { hoverRule = "box select drag: the hover cursor holds still"; return; }	// while the box shows the hover cursor holds still (the original skips the hover dispatch; kenshi-rwh.134)
	if (app.uiOn && ui::wantsMouse()) { app.hoverThing = 0; hoverCage = 0; hoverRule = "pointer over a UI panel -> arrow (no world pick)"; app.cursors.set("arrow"); return; }
	const Pick p = pick_screen(app.mouseX, app.mouseY);
	hoverRule = "no rule matched (nothing hit, or a hit with no cursor of its own) -> arrow";
	app.hoverThing = p.kind == Pick::Item || p.kind == Pick::Door || p.kind == Pick::Container || p.kind == Pick::Producer ? p.id : 0;
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
static void pick_mask_update()
{
	if (!app.opt.debug || (!dev::pickmask::on() && !app.hoverThing)) { return; }	// development features need --debug
	static const dev::pickmask::Host host{
		[](double mx, double my) -> uint64_t
		{
			double dir[3];
			mouse_ray(mx, my, dir);
			const Pick p = pick_at(dir);
			return p.kind == Pick::Item || p.kind == Pick::Door || p.kind == Pick::Container || p.kind == Pick::Producer ? p.id : 0;
		},
		[](uint64_t id, float box[4]) -> bool
		{
			double lo[3], hi[3];
			if (!app.buildings.propBox(id, lo, hi, kPropBoxSlack)) { return false; }
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
	dev::pickmask::update(host, app.hoverThing, app.mouseX, app.mouseY);
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

// Hold RMB on a target: the original's action menu (FEATURES "RMB default action"). A title row names the target; actions the server
// does not do yet are shown disabled. kHoldMenu: the original's, read live by kenshi-inspect.
constexpr double kHoldMenu = 0.2;
// Two right clicks on the same own character within this time: the second is the direct drive (kenshi-k16.2.238.1), as the cast row's double click.
constexpr double kDoubleClick = 0.4;
// Auto-split (kenshi-k16.20.13.3): a held stack dropped on a cell that holds fewer than it carries puts one unit at the cell and autoplaces the rest,
// one unit at a time through the order queue. Off while ui.place_one (the original's one-unit-per-click) is on: that is the UI's.
const cfg::Bool kAutoSplit{ cfg::Group{ "Inventory windows" }, "ui.auto_split", true, "Item windows: a held stack bigger than the cell it is dropped on allows places one unit there and autoplaces the rest (off: the drop is refused)" };
// RMB click vs look drag. Locked, X11 reports raw device counts (DPI-dependent), so a quick press forgives a jitter that
// a slow one doesn't: a click while moving the mouse to the next spot must still order (a5 2026-11-03, lost move orders).
constexpr double kDragNear = 6, kDragFar = 60, kClickTime = 0.25;	// counts; seconds (MINE)
// An LMB drag on the world shows the selection box only when BOTH sides pass this: fixed pixels, not x ui::scale (the original's
// test, 1407f2c30; kenshi-rwh.134). Below it the release is the ordinary click.
constexpr double kBoxDragPx = 4;
const std::string kNewGameFaction = "Nameless";	// NEW_GAME's faction name: the original's player faction (MINE until the panel asks for one)

// entries: the MENU_ENTRIES bitset the server sent for this target (bit 0 Talk To; kenshi-k16.2.121), -1 when there is none (a non-character
// pick, or the query's answer did not come): then the rows are offered as before, Talk included.
std::vector<ui::Menu::Item> menu_items(const Pick& p, int entries = -1)
{
	std::vector<ui::Menu::Item> it;
	const auto order = [](const char* what, uint64_t id, const std::function<bool()>& send)
	{
		const bool sent = send();
		printf("sim menu: %s %llu: %s\n", what, (unsigned long long)id, sent ? "sent" : "not sent");
		order_sent(sent);
	};
	const auto title = [](const std::string& text) { return ui::Menu::Item{ text, nullptr, false, true }; };
	const uint64_t id = p.id;
	if (p.kind == Pick::Character)
	{
		const auto ch = app.sim.decoder().chars.find(id);
		const bool down = ch != app.sim.decoder().chars.end() && (ch->second.cur.flags & (simlink::kKO | simlink::kDead));
		const std::string name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "";
		it.push_back(title(name.empty() ? "#" + std::to_string(id) : name));
		// The original's rows (kinspect ctxmenu.md, ctx18_static.md): ours Trade / Follow / Bodyguard / Pick Up, others Follow / Bodyguard /
		// Attack (ascending order id). A row whose order cannot be done is left out, never greyed; Trade waits for the server. Bodyguard is
		// a job (76 type 45, plus its Stay close row: SIMLINK JOBS) for the selection but the target.
		const ui::Menu::Item bodyguard{ "Bodyguard", [=] { order("bodyguard", id, [=] { return app.sim.jobAdd(45, id); }); } };
		if (app.sim.isPlayer(id))
		{
			it.push_back({ "Follow", [=] { order("follow", id, [=] { return app.sim.follow(id); }); } });
			if (!down) { it.push_back(bodyguard); }
		}
		else
		{
			if (!down)
			{
				// Talk To only when the server's gate passes (MENU_ENTRIES bit 0, the answer to order 99; SIMLINK MENU_ENTRIES)
				if (entries < 0 || (entries & 1)) { it.push_back({ "Talk", [=] { order("talk", id, [=] { return app.sim.interact(id, 6); }); } }); }
				it.push_back(bodyguard);
			}
			it.push_back({ "Attack", [=] { order("attack", id, [=] { return app.sim.attack(id); }); } });
			// Stealth KO stays offered for every living, standing target while a selected character sneaks (user 2026-11-11: the 2_ shot lists
			// ATTACK TARGET, ATTACK ALL, STEALTH KO 100%; we carry no chance, so no %). The row sends the KO whether or not the target has seen
			// the actor (the server answers: an aware victim aborts, event 50 result 2), so the menu also reaches a target the red fist would not.
			if (!down && sneak_actor(app.sim)) { it.push_back({ "Stealth KO", [=] { order("stealth ko", id, [=] { return app.sim.interact(id, 14); }); } }); }
		}
		if (down) { it.push_back({ "Loot", [=] { order("loot", id, [=] { return open_body(id, false); }); } }); }
		// first aid (needs a kit in the medic's inventory; the server answers otherwise): anyone down, and our own when hurt
		if (down || (app.sim.isPlayer(id) && app.sim.health(id, app.chars) < 1.0f)) { it.push_back({ "First aid", [=] { order("first aid", id, [=] { return app.sim.interact(id, 10); }); } }); }
		// carrying (order 60 action 8 lifts a KO'd / dead character, 13 puts the carried one down; the server picks the carrier for 13)
		const bool carried = ch != app.sim.decoder().chars.end() && ch->second.cur.carriedBy != 0;
		if (carried) { it.push_back({ "Put down", [=] { order("put down", id, [=] { return app.sim.interact(id, 13); }); } }); }
		else if (down) { it.push_back({ "Pick up", [=] { order("pick up", id, [=] { return app.sim.interact(id, 8); }); } }); }
		// the carrier's menu puts its load down too (the carried body rides on the carrier, so a click on it picks the carrier)
		for (const auto& [cid, cc] : app.sim.decoder().chars)
		{
			if (cc.cur.carriedBy != id || !app.sim.isPlayer(id)) { continue; }
			it.push_back({ "Put down", [=] { order("put down", cid, [=] { return app.sim.interact(cid, 13); }); } });
		}
	}
	else if (const simlink::Interactable* t = app.sim.thing(id))
	{
		std::string name = app.sim.nameOf(t->record, app.chars);
		if (t->kind == simlink::Interactable::kItem && t->count > 1) { name += " (" + std::to_string(t->count) + ")"; }
		if (t->kind == simlink::Interactable::kDoor && t->state == simlink::Interactable::kBroken) { name += " (broken)"; }
		it.push_back(title(name));
		if (t->kind == simlink::Interactable::kItem) { it.push_back({ "Pick up", [=] { order("pick up", id, [=] { return app.sim.interact(id, 7); }); } }); }
		else if (t->kind == simlink::Interactable::kProducer)
		{
			it.push_back({ "Mine", [=] { order("use", id, [=] { return use_producer(id, false); }); } });	// MINE wording ("Operate" for a powered one later)
			it.push_back({ "Open", [=] { order("open", id, [=] { return open_container(id, false); }); } });
		}
		else if (t->kind == simlink::Interactable::kContainer)
		{
			if (t->state == simlink::Interactable::kLocked)
			{
				// a locked box: the pick (order 60 action 3, SIMLINK's kind 3 lock); "Take all" stays so the sim answers the loot itself
				it.push_back({ "Pick Lock", [=] { order("lockpick", id, [=] { return app.sim.interact(id, 3); }); } });
				it.push_back({ "Take all", [=] { order("take all", id, [=] { return app.sim.interact(id, 5); }); } });
			}
			else
			{
				it.push_back({ "Open", [=] { order("open", id, [=] { return open_container(id, false); }); } });
				it.push_back({ "Take all", [=] { order("take all", id, [=] { return app.sim.interact(id, 5); }); } });
			}
		}
		else
		{
			ui::Menu::Item open{ "Open", [=] { order("open", id, [=] { return app.sim.interact(id, 1); }); } };
			ui::Menu::Item close{ "Close", [=] { order("close", id, [=] { return app.sim.interact(id, 2); }); } };
			// unlock: the server allows the door's owner faction only; bash: any shut door, until it breaks (state 3)
			ui::Menu::Item unlock{ "Unlock", [=] { order("unlock", id, [=] { return app.sim.interact(id, 11); }); } };
			ui::Menu::Item bash{ "Bash", [=] { order("bash", id, [=] { return app.sim.interact(id, 12); }); } };
			ui::Menu::Item pick{ "Pick Lock", [=] { order("lockpick", id, [=] { return app.sim.interact(id, 3); }); } };
			if (t->state == simlink::Interactable::kClosed) { it.push_back(open); }
			if (t->state == simlink::Interactable::kOpen) { it.push_back(close); }
			if (t->state == simlink::Interactable::kLocked) { it.push_back(unlock); it.push_back(pick); }
			if (t->state == simlink::Interactable::kLocked || t->state == simlink::Interactable::kClosed) { it.push_back(bash); }
		}
	}
	return it;
}

// Order 99 menu_query (SIMLINK MENU_ENTRIES, kenshi-k16.2.121): the context menu on a non-player, standing character asks the server which rows
// its right-click gate allows (bit 0 Talk To) and opens when MENU_ENTRIES (61) answers, at most kMenuQueryWait later. A server too old to know
// order 99 (the send fails), a refused query and a silence all open the menu as before, Talk offered, and say why on the log.
constexpr double kMenuQueryWait = 0.5;	// MINE: about 10 ticks; the reply is one order round trip
// The held right-click's menu: the character's answer (bit 0 Talk To) and, on a locked cage's prisoner, the cage's (order 99 on the cage key: bits 16 pick
// lock, 17 use tools, 18 use strength and the pick chance) are both asked; it opens when everything asked is in or at the deadline.
struct PendingMenu
{
	bool on = false;
	Pick p;
	double x = 0, y = 0, deadline = 0;
	uint32_t seq = 0, cageSeq = 0;
	uint64_t cage = 0;
	std::string cageTitle;
	bool charDone = true, cageDone = true;
	int entries = -1, cageEntries = -1;	// -1: not answered (Talk offered; all three cage rows offered)
	float chance = -1;
};
PendingMenu menuPending;	// the right-click whose menu is held for its MENU_ENTRIES (one at a time: a new one supersedes it)

// The prisoner's menus side by side (kenshi-0qr shot 10): the cage's first, at the cursor, the character's beside it on the left (the second menu's
// right edge is at x). No default order: every row is a choice. A cage with no row to offer opens the character's menu alone.
void open_pick_menu(const PendingMenu& pm)
{
	std::vector<ui::Menu::Item> items = menu_items(pm.p, pm.entries);
	std::vector<ui::ContextRow> cageRows;
	std::vector<uint8_t> cageActions;
	if (pm.cage)
	{
		const int e = pm.cageEntries;
		if (e < 0 || (e & (1 << 16))) { cageRows.push_back({ "Pick Lock", pm.chance >= 0 ? (int)(std::floor(pm.chance * 1000.0f) * 0.1f) : -1 }); cageActions.push_back(18); }	// the original's int(chance x 1000) x 0.1
		if (e < 0 || (e & (1 << 17))) { cageRows.push_back({ "Use Tools" }); cageActions.push_back(19); }
		if (e < 0 || (e & (1 << 18))) { cageRows.push_back({ "Use Strength" }); cageActions.push_back(20); }
	}
	if (items.size() < 2 && cageRows.empty()) { return; }	// a title and no rows: no menu (the original opens none without an action)
	app.look = false;
	sappcmd::lockMouse(false);
	app.mouseX = pm.x;
	app.mouseY = pm.y;
	pointer::set((float)pm.x, (float)pm.y);
	// items[0] is the title row (menu_items); the rest become ContextRows, picked by index after the menu closes
	std::vector<ui::ContextRow> rows;
	std::vector<std::function<void()>> acts;
	for (size_t i = 1; i < items.size(); i++) { rows.push_back({ items[i].text }); acts.push_back(items[i].action); }
	if (!cageRows.empty())
	{
		const uint64_t key = pm.cage;
		ui::openContextMenu((float)pm.x, (float)pm.y, pm.cageTitle, cageRows, [key, cageActions](int i)
		{
			if (i < 0 || i >= (int)cageActions.size()) { return; }
			const bool sent = app.sim.interact(key, cageActions[i]);
			printf("sim menu: cage %s %llu: %s\n", cageActions[i] == 18 ? "pick lock" : cageActions[i] == 19 ? "use tools" : "use strength", (unsigned long long)key, sent ? "sent" : "not sent");
			order_sent(sent);
		});
	}
	if (!rows.empty()) { ui::openContextMenu((float)pm.x, (float)pm.y, items[0].text, rows, [acts](int i) { if (i >= 0 && i < (int)acts.size() && acts[i]) { acts[i](); } }, !cageRows.empty()); }	// at the cursor (ctx18_static.md)
	printf("sim menu: open at %.0f %.0f%s%s\n", pm.x, pm.y, pm.entries >= 0 ? (pm.entries & 1 ? " with Talk" : " without Talk") : "", cageRows.empty() ? "" : " with the cage's menu");
}

// The pending menu opens once everything it asked has answered
void menu_open_if_ready()
{
	if (!menuPending.on || !menuPending.charDone || !menuPending.cageDone) { return; }
	const PendingMenu pm = menuPending;
	menuPending = {};
	open_pick_menu(pm);
}

// Per frame, after the sim's update (so a MENU_ENTRIES of this frame is in the decoder): the held right-click's answer, refusal or timeout opens
// its menu. A reply for another seq is a stale one (a second right-click superseded it): logged and dropped.
void menu_query_frame()
{
	if (app.opt.debug && app.sim.decoder().cagesSerial != app.cagesSeen)	// the lead's live check of the wire (kenshi-k16.2.105.1)
	{
		app.cagesSeen = app.sim.decoder().cagesSerial;
		std::string ids;
		for (const auto& [id, c] : app.sim.decoder().cages) { ids += " " + std::to_string(id) + "->" + std::to_string(c.cage) + (c.locked ? " locked lvl " : " open lvl ") + std::to_string(c.level); }
		printf("CAGES decoded: %zu%s\n", app.sim.decoder().cages.size(), ids.c_str());
	}
	for (const SimView::MenuEntries& m : app.sim.takeMenuEntries())
	{
		if (cageHover.seq && m.clientSeq == cageHover.seq)	// the hovered cage's pick chance (the cursor's tag)
		{
			cageHover.seq = 0;
			cageHover.chance = m.pickChance;
			printf("menu_entries: cage %llu entries %x chance %.3f (hover)\n", (unsigned long long)m.target, m.entries, m.pickChance);
			continue;
		}
		const bool forChar = menuPending.on && menuPending.seq && m.clientSeq == menuPending.seq;
		const bool forCage = menuPending.on && menuPending.cageSeq && m.clientSeq == menuPending.cageSeq;
		printf("menu_entries: target %llu entries %x%s%s\n", (unsigned long long)m.target, m.entries, forCage ? " chance " : "", forChar || forCage ? "" : " (stale, dropped)");
		if (forChar) { menuPending.charDone = true; menuPending.entries = (int)m.entries; }
		if (forCage) { menuPending.cageDone = true; menuPending.cageEntries = (int)m.entries; menuPending.chance = m.pickChance; }
		menu_open_if_ready();
	}
	for (const SimView::MenuAnswer& a : app.sim.takeMenuAnswers())
	{
		if (!menuPending.on) { continue; }
		const bool forChar = menuPending.seq && a.seq == menuPending.seq, forCage = menuPending.cageSeq && a.seq == menuPending.cageSeq;
		if (!forChar && !forCage) { continue; }
		printf("menu_entries: none (refused: %s), %s offered\n", a.applied ? "applied, no frame" : a.reason.c_str(), forCage ? "all cage rows" : "Talk");
		(forChar ? menuPending.charDone : menuPending.cageDone) = true;
		menu_open_if_ready();
	}
	if (menuPending.on && ui::now() >= menuPending.deadline)
	{
		printf("menu_entries: none (timeout), Talk offered\n");
		menuPending.charDone = menuPending.cageDone = true;
		menu_open_if_ready();
	}
}

// Per frame: RMB held still on a target long enough opens the menu where it went down; the release then picks (ui held mode).
void rmb_hold_menu()
{
	if (app.rmbAt < 0 || !app.look) { app.rmbAt = -1; return; }
	if (app.clickDrag >= 6) { app.rmbAt = -1; return; }	// a look drag
	if (ui::now() - app.rmbAt < kHoldMenu) { return; }
	app.rmbAt = -1;
	const Pick p = pick_screen(app.clickX, app.clickY);
	if (p.kind != Pick::Character && p.kind != Pick::Item && p.kind != Pick::Door && p.kind != Pick::Container && p.kind != Pick::Producer) { return; }
	PendingMenu pm;
	pm.p = p;
	pm.x = app.clickX;
	pm.y = app.clickY;
	pm.deadline = ui::now() + kMenuQueryWait;
	if (p.kind == Pick::Character && !app.sim.isPlayer(p.id))
	{
		const auto ch = app.sim.decoder().chars.find(p.id);
		const bool down = ch != app.sim.decoder().chars.end() && (ch->second.cur.flags & (simlink::kKO | simlink::kDead));
		if (!down)	// a standing non-player: the server's gate decides Talk To (the menu opens on its answer)
		{
			pm.seq = app.sim.menuQuery(p.id);
			if (!pm.seq) { printf("menu_entries: none (not sent), Talk offered\n"); }
		}
	}
	if (const simlink::SimLinkDecoder::Cage* cg = p.kind == Pick::Character ? locked_cage(app.sim, p.id) : nullptr)	// a prisoner: its cage's menu beside its own (kenshi-k16.2.105.1)
	{
		pm.cage = cg->cage;
		pm.cageTitle = "Prisoner Cage";	// the original's title (kenshi-0qr shot 10); CAGES' record names the building the cage stands in (Longhouse), not the cage
			pm.cageSeq = app.sim.menuQuery(cg->cage);
		if (!pm.cageSeq) { printf("menu_entries: none (not sent), all cage rows offered\n"); }
	}
	pm.charDone = !pm.seq;
	pm.cageDone = !pm.cageSeq;
	if (pm.seq || pm.cageSeq) { pm.on = true; menuPending = pm; return; }
	open_pick_menu(pm);
}

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

const cfg::Bool kForeignWalkTo{ cfg::Group{ "Orders" }, "orders.foreign_walk_to", true, "A right click without ALT on someone else's container (shop counter, cabinet, locked safe) shows the ALT hint and walks there, as the original (user 2026-11-14). Off: the hint only. ALT steals / picks the lock either way" };

#ifdef KCLIENT_DEV_TOOLS
uint64_t g_forcePickThing = 0;	// script `mouse force ID`: the next clicks pick thing ID wherever the pointer is (a test of the order dispatch past the pick's geometry)
#else
constexpr uint64_t g_forcePickThing = 0;
#endif


const char* camera_mode_name();

// The per-click record (kenshi-k16.2.259.3, user 2026-11-17: "the EXACT camera pose, x/y click location, and click projection results"): each click at its own
// frame, with what the pick saw, so a bad click is diagnosable from the F12 shot a second later. The last kClickHistory, newest first, go in the F12 sidecar
// ("clicks:" block) and, one per click, in the run log ("click record:"); `click replay KEY=VALUE...` (the record's first line) repeats the pick.
struct ClickRec
{
	uint64_t n = 0, frame = 0;
	bool right = false, shift = false;
	double mx = 0, my = 0, aspect = 0, near = 0;
	std::string mode;
	int w = 0, h = 0, bar = 0;
	double eye[3] = {}, yawDeg = 0, pitchDeg = 0, dist = 0, focus[3] = {}, from[3] = {}, dir[3] = {}, terrain[3] = {};
	bool hasTerrain = false;
	uint64_t followId = 0;
	Pick pick;
	std::vector<PickSkip> skipped;
	std::string building, ghosts;
	int floor = -1;
	float xray[4] = {};
	uint32_t seq = 0;	// a ground click's move_to clientSeq (0: none sent)
};
constexpr size_t kClickHistory = 6;
static std::deque<ClickRec> g_clicks;
static uint64_t g_clickCount = 0;
static const char* const kPickKinds[] = { "none", "ground", "character", "item", "door", "container", "producer", "bed" };

static std::string click_head(const ClickRec& c)
{
	char b[1400];
	snprintf(b, sizeof b,
		"n=%llu frame=%llu btn=%s shift=%d at=%.1f,%.1f size=%dx%d bar=%d mode=%s eye=%.3f,%.3f,%.3f yaw=%.4f pitch=%.4f dist=%.2f focus=%.3f,%.3f,%.3f follow=%llu fovy=45 aspect=%.5f near=%.2f "
		"from=%.3f,%.3f,%.3f dir=%.6f,%.6f,%.6f pick=%s id=%llu hit=%.3f,%.3f,%.3f building=%d terrain=%.3f,%.3f,%.3f xray=%.2f,%.2f,%.2f,%.2f seq=%u",
		(unsigned long long)c.n, (unsigned long long)c.frame, c.right ? "right" : "left", c.shift ? 1 : 0, c.mx, c.my, c.w, c.h, c.bar, c.mode.c_str(), c.eye[0], c.eye[1], c.eye[2], c.yawDeg, c.pitchDeg,
		c.dist, c.focus[0], c.focus[1], c.focus[2], (unsigned long long)c.followId, c.aspect, c.near, c.from[0], c.from[1], c.from[2], c.dir[0], c.dir[1], c.dir[2], kPickKinds[(int)c.pick.kind],
		(unsigned long long)c.pick.id, c.pick.hit[0], c.pick.hit[1], c.pick.hit[2], c.pick.building ? 1 : 0, c.terrain[0], c.terrain[1], c.terrain[2], c.xray[0], c.xray[1], c.xray[2], c.xray[3], c.seq);
	return b;
}

// The record as text: the replayable first line, then what the pick walked past, the buildings ghosted, and (a move) where the sim sent the order.
static std::string click_text(const ClickRec& c)
{
	char b[512];
	std::string r = "click record: " + click_head(c) + "\n";
	if (!c.building.empty()) { snprintf(b, sizeof b, "  hit building \"%s\" floor %d\n", c.building.c_str(), c.floor); r += b; }
	if (!c.hasTerrain) { r += "  terrain: no ray hit\n"; }
	for (const PickSkip& s : c.skipped)
	{
		snprintf(b, sizeof b, "  skipped \"%s\" at %.1f %.1f %.1f: %s\n", s.name.c_str(), s.at[0], s.at[1], s.at[2], s.why.c_str());
		r += b;
	}
	if (c.skipped.empty()) { r += "  skipped: none\n"; }
	r += "  ghosts: " + c.ghosts + "\n";
	if (c.seq)
	{
		for (const uint64_t id : app.sim.selected())
		{
			const simlink::Route* rt = app.sim.routeOf(id);
			if (rt && rt->clientSeq == c.seq && !rt->points.empty())
			{
				snprintf(b, sizeof b, "  route of %llu for seq %u ends %.1f %.1f %.1f (%zu waypoints, flags %d)\n", (unsigned long long)id, c.seq, rt->points.back()[0], rt->points.back()[1], rt->points.back()[2], rt->points.size(), (int)rt->flags);
				r += b;
			}
		}
	}
	return r;
}

static ClickRec* click_begin(double mx, double my, bool right, bool shift, const Pick& p, std::vector<PickSkip>&& skipped)
{
	constexpr double kDeg = 180 / 3.14159265358979;
	ClickRec c;
	c.n = ++g_clickCount;
	c.frame = (uint64_t)app.frames;
	c.right = right;
	c.shift = shift;
	c.mx = mx;
	c.my = my;
	c.mode = camera_mode_name();
	c.w = view::width();
	c.h = view::height();
	c.bar = app.post.letterboxBar();
	c.aspect = windowgate::aspect();
	c.near = camera_near();
	for (int k = 0; k < 3; k++) { c.eye[k] = app.pos[k]; c.focus[k] = app.focus[k]; }
	c.yawDeg = app.yaw * kDeg;
	c.pitchDeg = app.pitch * kDeg;
	c.dist = app.follow ? app.followDist : app.rtsDist;
	c.followId = app.follow ? app.followId : 0;
	mouse_ray(mx, my, c.dir);
	for (int k = 0; k < 3; k++) { c.from[k] = app.pos[k] + c.dir[k] * c.near; }
	c.hasTerrain = ray_ground(c.from, c.dir, c.terrain);
	c.pick = p;
	c.skipped = std::move(skipped);
	for (int k = 0; k < 4; k++) { c.xray[k] = app.buildings.xray[k]; }
	c.ghosts = app.buildings.ghostState();
	g_clicks.push_front(std::move(c));
	while (g_clicks.size() > kClickHistory) { g_clicks.pop_back(); }
	return &g_clicks.front();
}

// The F12 sidecar's "clicks:" block, newest first.
static std::string clicks_report()
{
	std::string r = "clicks: " + std::to_string(g_clicks.size()) + " recorded (newest first; each first line replays with the script op `click replay <the key=value words>`)\n";
	for (const ClickRec& c : g_clicks) { r += click_text(c); }
	return r;
}

// Script `click replay KEY=VALUE...`: the pick of a recorded click, from its own camera, pointer and viewport; prints the new pick next to the recorded one.
static bool click_replay(const std::vector<std::string>& words)
{
	std::map<std::string, std::string> kv;
	for (const std::string& w : words) { if (const size_t q = w.find('='); q != std::string::npos) { kv[w.substr(0, q)] = w.substr(q + 1); } }
	const auto nums = [&](const char* key, double* out, int n)
	{
		const auto it = kv.find(key);
		if (it == kv.end()) { return false; }
		const char* s = it->second.c_str();
		for (int i = 0; i < n; i++)
		{
			char* end;
			out[i] = strtod(s, &end);
			if (end == s) { return false; }
			s = *end == ',' ? end + 1 : end;
		}
		return true;
	};
	double eye[3], at[2], yaw, pitch, hit[3], xr[4];
	if (!nums("eye", eye, 3) || !nums("at", at, 2) || !nums("yaw", &yaw, 1) || !nums("pitch", &pitch, 1)) { return false; }
	constexpr double kRad = 3.14159265358979 / 180;
	const bool wantHit = nums("hit", hit, 3);
	const bool wantXray = nums("xray", xr, 4);
	double sv[3] = { app.pos[0], app.pos[1], app.pos[2] };
	const double svYaw = app.yaw, svPitch = app.pitch;
	float svX[4];
	for (int k = 0; k < 3; k++) { app.pos[k] = eye[k]; }
	app.yaw = yaw * kRad;
	app.pitch = pitch * kRad;
	for (int k = 0; k < 4; k++) { svX[k] = app.buildings.xray[k]; if (wantXray) { app.buildings.xray[k] = (float)xr[k]; } }
	if (kv.count("size") && kv["size"] != std::to_string(view::width()) + "x" + std::to_string(view::height())) { printf("click replay: the record's size %s is not this run's %dx%d (pass --size)\n", kv["size"].c_str(), view::width(), view::height()); }
	std::vector<PickSkip> sk;
	const Pick p = pick_screen(at[0], at[1], &sk);
	for (int k = 0; k < 3; k++) { app.pos[k] = sv[k]; }
	app.yaw = svYaw;
	app.pitch = svPitch;
	for (int k = 0; k < 4; k++) { app.buildings.xray[k] = svX[k]; }
	const std::string rec = kv.count("pick") ? kv["pick"] : "?";
	const bool same = rec == kPickKinds[(int)p.kind] && (!wantHit || std::hypot(std::hypot(hit[0] - p.hit[0], hit[1] - p.hit[1]), hit[2] - p.hit[2]) < 0.5);
	printf("click replay: at %.1f,%.1f from eye %.1f %.1f %.1f yaw %.2f pitch %.2f: pick %s id %llu hit %.3f %.3f %.3f building %d; recorded %s%s: %s\n", at[0], at[1], eye[0], eye[1], eye[2], yaw, pitch,
		kPickKinds[(int)p.kind], (unsigned long long)p.id, p.hit[0], p.hit[1], p.hit[2], p.building ? 1 : 0, rec.c_str(), wantHit ? (" hit " + kv["hit"]).c_str() : "", same ? "MATCH" : "DIFFERS");
	for (const PickSkip& s : sk) { printf("click replay skipped \"%s\" at %.1f %.1f %.1f: %s\n", s.name.c_str(), s.at[0], s.at[1], s.at[2], s.why.c_str()); }
	return true;
}

void sim_click(double mx, double my, bool right, bool shift)
{
	std::vector<PickSkip> clickSkipped;
	Pick p = pick_screen(mx, my, &clickSkipped);
	if (g_forcePickThing)
	{
		if (const simlink::Interactable* ft = app.sim.thing(g_forcePickThing)) { p = Pick{}; p.kind = ft->kind == simlink::Interactable::kContainer ? Pick::Container : Pick::Item; p.id = g_forcePickThing; }
	}
	ClickRec* const rec = click_begin(mx, my, right, shift, p, std::move(clickSkipped));	// kenshi-k16.2.259.3: the click's own record; printed when this click is done
	struct ClickPrint { ClickRec* r; ~ClickPrint() { fputs(click_text(*r).c_str(), stdout); } } clickPrint{ rec };
	const uint64_t id = p.kind == Pick::Character ? p.id : 0;
	if (!right && p.kind == Pick::Container && label_at(mx, my) == p.id)
	{
		if (container_locked(app.sim, p.id))
		{
			const bool sent = app.sim.interact(p.id, 3, shift);
			printf("sim click (%.0f, %.0f): label: lockpick %llu: %s\n", mx, my, (unsigned long long)p.id, sent ? "sent" : "not sent");
			order_sent(sent);
			return;
		}
		const bool sent = open_container(p.id, shift);
		printf("sim click (%.0f, %.0f): label: open container %llu: %s\n", mx, my, (unsigned long long)p.id, sent ? "sent" : "not sent");
		order_sent(sent);
		return;
	}
	if (!right && p.kind == Pick::Item && label_at(mx, my) == p.id)
	{
		const bool sent = app.sim.interact(p.id, 7, shift);
		printf("sim click (%.0f, %.0f): label: pick up %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
		order_sent(sent);
		return;
	}
	if (!right && p.kind == Pick::Producer)	// LMB: the generic machine window, from anywhere, the selection kept (user 2026-11-02, as the original)
	{
		app.machine = p.id;
		app.panelProducer = p.id;	// the panel follows the last clicked thing (k16.19.8.2): the producer
		printf("sim click (%.0f, %.0f): show machine %llu\n", mx, my, (unsigned long long)p.id);
		return;
	}
	if (!right && p.kind == Pick::Container)	// LMB: its window alone, from anywhere, the selection kept (user 2026-11-02, as the original)
	{
		if (container_locked(app.sim, p.id))
		{
			const bool sent = app.sim.interact(p.id, 3, shift);
			printf("sim click (%.0f, %.0f): lockpick %llu: %s\n", mx, my, (unsigned long long)p.id, sent ? "sent" : "not sent");
			order_sent(sent);
			return;
		}
		if (!cont_find(p.id)) { App::OpenCont oc; oc.id = p.id; oc.view = true; cont_add(oc); }	// already open: the click leaves it be
		printf("sim click (%.0f, %.0f): show container %llu\n", mx, my, (unsigned long long)p.id);
		return;
	}
	if (!right)
	{
		if (id && !app.sim.isPlayer(id))
		{
			app.inspect = id;
			app.panelProducer = 0;	// a character click moves the panel back to the character; the machine window stays (k16.19.8.2)
			printf("sim click (%.0f, %.0f): inspect %llu\n", mx, my, (unsigned long long)id);
			return;
		}
		if (id) { app.inspect = 0; }
		app.panelProducer = 0;
		app.sim.select(id, shift);
		printf("sim click (%.0f, %.0f): character %llu, %zu selected\n", mx, my, (unsigned long long)id, app.sim.selected().size());
		return;
	}
	// A double right click on one of your own characters enters direct drive on it as select + V, instead of a second order (k16.2.238.1,
	// user: "double left click already focused on a single character to follow. double right to direct drive?"). On the character being driven
	// it leaves. The first click keeps its normal order (below); an NPC or the ground never drives, so their two clicks order as they always did.
	if (id && app.sim.isPlayer(id) && !locked_cage(app.sim, id))
	{
		const double now = ui::now();
		const bool twice = app.lastRmbChar == id && now - app.lastRmbCharAt < kDoubleClick;
		app.lastRmbChar = twice ? 0 : id;	// a third click starts a new pair
		app.lastRmbCharAt = twice ? -1 : now;
		if (twice)
		{
			if (app.follow && app.followId == id)
			{
				printf("sim click (%.0f, %.0f): double right click, character %llu: leave the drive\n", mx, my, (unsigned long long)id);
				leave_follow();
			}
			else
			{
				app.inspect = 0;	// as a left click's select: the panel goes with the character
				app.panelProducer = 0;
				app.sim.select(id, false);
				set_follow_id(id);
				printf("sim click (%.0f, %.0f): double right click, character %llu: direct drive on\n", mx, my, (unsigned long long)id);
			}
			return;
		}
	}
	else { app.lastRmbChar = 0; app.lastRmbCharAt = -1; }
	bool sent = false;
	if (id && locked_cage(app.sim, id))	// a caged prisoner has no default order: the menus (opened on the press) are the choices (kenshi-k16.2.105.1)
	{
		printf("sim click (%.0f, %.0f): caged character %llu: no default order\n", mx, my, (unsigned long long)id);
		return;
	}
	if (id)
	{
		const CharAct a = char_act(app.sim, app.chars, id, ctrl_held());
		// Shift: the original's jobs (order 76): attack -> 4 (Fight enemies), follow -> 31 (Stay close), first aid -> 25 (Medic); the rest queue
		const uint16_t job = !shift ? 0 : a.action == kAttack ? 4 : a.action == kFollow ? 31 : a.action == 10 ? 25 : 0;
		if (job) { sent = app.sim.jobAdd(job, job == 31 ? id : 0); }
		else { sent = a.action == kAttack ? app.sim.attack(id) : a.action == kFollow ? app.sim.follow(id) : a.action == 5 ? open_body(id, shift) : a.action > 0 && app.sim.interact(id, (uint8_t)a.action, shift && a.action != 14); }	// stealth KO: queue 1 is bad_value
		printf("sim click (%.0f, %.0f): character %llu %s (%d)%s: %s\n", mx, my, (unsigned long long)id, a.cursor, a.action, job ? " as a job" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Item)
	{
		sent = app.sim.interact(p.id, 7, shift);
		printf("sim click (%.0f, %.0f): pick up %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Bed)	// sleep (order 60 action 9); event 10 occupied / not_allowed come back as refusals
	{
		sent = app.sim.interact(p.id, 9, shift);
		printf("sim click (%.0f, %.0f): sleep in bed %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Producer)
	{
		app.panelProducer = p.id;	// the last clicked thing is the producer here too (k16.19.8.2): a mine order shows its panel
		sent = use_producer(p.id, shift);
		printf("sim click (%.0f, %.0f): use producer %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Container)
	{
		const simlink::Interactable* t = app.sim.thing(p.id);
		const bool locked = container_locked(app.sim, p.id);
		if (t && app.sim.foreign(p.id) && !app.keys[SAPP_KEYCODE_LEFT_ALT] && !app.keys[SAPP_KEYCODE_RIGHT_ALT])
		{
			// a foreign container's RMB without ALT (CURSORS_LIVE B4b storage / shop counter, B6 locked): the original's hint, and (orders.foreign_walk_to) the move
			// order to it at the same instant; no loot, no lockpick (ALT does those; user 2026-11-14, ask kenshi-k16.2.240.2: "Match it")
			ui::message(locked ? "Hold down the ALT key to pick the lock." : "Hold down the ALT key to steal items");	// the HUD message box (kenshi-rwh.161: the original posts both on the RMB press edge; the lock text ends in a full stop, the steal one does not)
			if (kForeignWalkTo.get())
			{
				const double at[3] = { t->use[0], t->use[1], t->use[2] };
				sent = app.sim.moveTo(at, shift);
			}
			printf("sim click (%.0f, %.0f): foreign %s %llu%s, no ALT: hint%s: %s\n", mx, my, locked ? "locked container" : "container", (unsigned long long)p.id, shift ? " (queued)" : "",
				kForeignWalkTo.get() ? " + walk, no steal/lockpick" : " only", sent ? "sent" : "not sent");
		}
		else if (locked)
		{
			sent = app.sim.interact(p.id, 3, shift);
			printf("sim click (%.0f, %.0f): lockpick %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
		}
		else
		{
			sent = open_container(p.id, shift);
			printf("sim click (%.0f, %.0f): open container %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
		}
	}
	else if (const simlink::Interactable* d = p.kind == Pick::Door ? app.sim.thing(p.id) : nullptr)
	{
		// locked: the click does nothing; the hold menu has Pick Lock (order 60 action 3, on the server since kenshi-h2v.15.1), unlock and bash. Broken: nothing to do
		const uint8_t action = d->state == simlink::Interactable::kOpen ? 2 : d->state == simlink::Interactable::kClosed ? 1 : 0;
		sent = action && app.sim.interact(p.id, action, shift);
		printf("sim click (%.0f, %.0f): door %llu %s%s: %s\n", mx, my, (unsigned long long)p.id, action == 2 ? "close" : action == 1 ? "open" : d->state == simlink::Interactable::kBroken ? "broken" : "locked", shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Ground)
	{
		// kenshi-k16.2.231.1: the shot's route sidecar records which building and floor a ground click meant (a floor/deck hit is a building shape)
		SimView::Click click;
		click.what = "click";
		click.building = p.building;
		double dir[3];
		mouse_ray(mx, my, dir);
		const double far = std::hypot(p.hit[0] - app.pos[0], p.hit[1] - app.pos[1], p.hit[2] - app.pos[2]) + 1.0;
		if (const int b = app.buildings.rayBuilding(app.pos, dir, far, p.hit); b >= 0)
		{
			click.buildingName = app.buildings.nameOfInstance(b);
			click.floor = app.buildings.floorAt(b, p.hit);
			rec->building = click.buildingName;
			rec->floor = click.floor;
		}
		sent = app.sim.moveTo(p.hit, shift, true, &click);
		printf("sim click (%.0f, %.0f): move_to %.0f %.0f: %s\n", mx, my, p.hit[0], p.hit[2], sent ? "sent" : "not sent");
		if (sent)
		{
			memcpy(app.orderAt, p.hit, sizeof p.hit);
			app.orderAge = 0;
			app.orderRejected = false;
			rec->seq = app.sim.lastSentSeq();
			app.orderSeq = app.sim.lastSentSeq();	// the marker moves to this order's route end when the sim answers (k16.2.259)
			app.orderRouted = false;
		}
	}
	// The order click (Right_Click, the order dispatcher 1407f9e20): once per issued order, after the whole selection got it, never one per
	// character. A context menu row's action does not come through here, so a move from a row plays no click (the original's menu-row flag).
	if (right && sent) { post_event(audio::kRightClick, "Right_Click"); }
	order_sent(sent);
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

// The sim's yaw (0 = +z, towards +x) of the camera's forward, and back
double heading_of(double camYaw) { return M_PI - camYaw; }

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

// The follow camera's default close view (k16.2.237 part 5): the distance the follow camera starts at (the wheel's 10..2000 zoom moves it after).
constexpr double kFollowDist = 60.0, kFollowPitch = -15 * M_PI / 180.0;	// units, rad
// The follow camera on any drawn character (a spectator's NPC): the selection is not needed, and the drive keys stay the player's (update_follow).
// Every entry starts at the default close camera behind the character, whatever the RTS zoom was, and a leave + re-enter the same (k16.2.237 part 5,
// user: "v - reengaging direct drive, it didn't restore to a 'default close' camera").
bool set_follow_id(uint64_t id)
{
	double pos[3], yaw;
	const simlink::Route* route;
	if (!id || !app.sim.pose(id, pos, yaw, route)) { return false; }
	const double fromRts = app.rts ? app.rtsDist : 0.0;	// the zoom it was at, for the scripted log: it does not carry over
	app.follow = true;
	app.followId = id;
	app.buildings.engage();
	app.followBase = M_PI - yaw;
	app.followOffset = 0;
	app.yaw = app.followBase;
	app.pitch = kFollowPitch;
	app.followDist = kFollowDist;
	app.followPulled = 1e9;
	app.followHold = 0;
	app.driveSent = 0; app.strafeSent = 0;
	if (app.scripted()) { printf("follow %llu: camera mode follow dist %.1f pitch %.1f (from rts dist %.1f)\n", (unsigned long long)id, app.followDist, app.pitch * 180.0 / M_PI, fromRts); }
	return true;
}

void set_follow(bool on)
{
	if (!on)
	{
		if (driving()) { send_drive(0, app.headingSent, 0); }
		app.follow = false;
		app.autorun = false;
		return;
	}
	// the selection first (the original); with no player squad, the watched character (a click's inspect focus)
	set_follow_id(!app.sim.selected().empty() ? app.sim.selected()[0] : app.inspect);
}

// The collision boom (kenshi-k16.2.289.1; user 2026-11-15: a follow camera never hides the character; shared with the RTS camera's look-up,
// kenshi-k16.2.353.1): a sphere kBoomRadius swept from the pivot back along the view to the wanted distance; solid world (terrain, walls, large building
// shapes, foliage) shortens the boom to just before the contact, at once. Thin props (CollisionPick::thin) are not in the way: the occluder cone dithers
// them (the frame's cone). It eases back out at `outRate` u/s once it has held kBoomHold s since the last blockage, so a post passed never makes it
// jitter. `pulled`/`hold` are the caller's state (1e9: free). minLen is the shortest the boom is cut to; sweep false skips the world test (the boom only
// eases out). Every constant is MINE.
struct Boom { double len, allowed; std::string blocker; };
Boom boom_step(const double pivot[3], const double back[3], double wanted, double minLen, double outRate, bool sweep, double dt, double& pulled, double& hold)
{
	constexpr double kBoomRadius = 3.0, kBoomMargin = 0.5, kBoomHold = 0.6, kGroundClear = 2.0, kStep = 3.0;	// u, u, s, u, u
	double allowed = wanted;
	std::string blocker;
	float bbox[6] = {};
	if (sweep)
	{
		if (double t; app.collide.sweep(pivot, back, allowed + kBoomMargin, kBoomRadius, t, bbox, app.scripted() ? &blocker : nullptr)) { allowed = std::max(t - kBoomMargin, minLen); }
		else { blocker.clear(); }
		if (char box[96]; app.scripted() && !blocker.empty()) { snprintf(box, sizeof box, " box %.0f %.0f %.0f - %.0f %.0f %.0f", bbox[0], bbox[1], bbox[2], bbox[3], bbox[4], bbox[5]); blocker += box; }
		for (double d = kStep; d < allowed + kStep; d += kStep)	// the terrain: the first step under the ground, then the last one over it
		{
			const double l = std::min(d, allowed);
			if (pivot[1] + back[1] * l < app.hm->height(pivot[0] + back[0] * l, pivot[2] + back[2] * l) + kGroundClear)
			{
				double lo = d - kStep, hi = l;
				for (int i = 0; i < 6; i++)
				{
					const double m = 0.5 * (lo + hi);
					(pivot[1] + back[1] * m < app.hm->height(pivot[0] + back[0] * m, pivot[2] + back[2] * m) + kGroundClear ? hi : lo) = m;
				}
				allowed = std::max(lo, minLen);
				break;
			}
			if (l >= allowed) { break; }
		}
	}
	if (allowed < std::min(pulled, wanted)) { pulled = allowed; hold = kBoomHold; }	// snap in
	else if ((hold -= dt) <= 0) { pulled = std::min(pulled + outRate * dt, allowed < wanted ? allowed : 1e9); }	// ease out, slowly
	if (pulled >= wanted) { pulled = 1e9; }
	return { std::min(wanted, pulled), allowed, blocker };
}

// Follow camera: behind the character at followDist, its yaw smoothed towards the character's facing, or towards the route ahead so the corners
// don't snap it; while driving the camera's yaw is the heading. Drive orders on change, steering at most at the tick rate.
void update_follow(double dt)
{
	constexpr double kTurnRate = 2.0, kSmooth = 2.5, kLookAhead = 15, kEye = 14, kMinHeadingStep = 0.01;	// MINE: rad/s, 1/s, units, units, rad
	double pos[3], yaw;
	const simlink::Route* route;
	if (!app.sim.pose(app.followId, pos, yaw, route)) { set_follow(false); return; }
	app.driveAge += dt;
	constexpr double kZoomRate = 1.5;	// MINE: e-folds/s of the distance
	const double zoom = (held("camera_zoom_out") ? 1 : 0) - (held("camera_zoom_in") ? 1 : 0);
	// kenshi-k16.2.237.7 (user k16.2.237 part 6: "any zoom is the 'disable cruise control'"): Home / End in the follow of a player character leave it like the wheel,
	// to the RTS camera on the same character, and zoom it this frame. A spectator's follow keeps its zoom.
	if (zoom != 0 && app.sim.isPlayer(app.followId)) { leave_follow(); update_rts(dt); return; }
	app.followDist = std::clamp(app.followDist * std::exp(zoom * kZoomRate * dt), 10.0, 2000.0);	// the scroll wheel's clamp
	if (app.sim.isPlayer(app.followId))
	{
		if (held("camera_back")) { app.autorun = false; }
		const uint8_t drive = held("camera_forward") || app.autorun ? 1 : held("camera_back") ? 2 : 0;
		// A / D steer; stopped, they turn in place (drive 0 with the heading: the sim turns only a character in its drive hold)
		// kenshi-k16.2.237.4: with mouse look held (the right button), A / D strafe (order 20's strafe, facing = the camera heading) and do not turn
		const double ad = (held("camera_right") ? 1 : 0) - (held("camera_left") ? 1 : 0);
		// SIMLINK sign as is (A = -1, D = +1). The sim's lateral is mirrored on screen until kenshi-k16.8.38 lands; the prediction's knav::Walker follows it.
		const int strafe = app.look ? (int)ad : 0;
		const double turn = app.look ? 0.0 : ad;
		app.yaw += turn * kTurnRate * dt;
		// kenshi-k16.2.237.1 (MINE): a backpedal keeps the body's facing and walks along -heading (SIMLINK "Direct drive"), so the heading sent for S is the body's
		// own yaw, not the camera's: A / D with S turn the camera while the body backs away straight (a turning heading walked it sideways across its facing,
		// a forward / lateral mix no clip fits: the S+D of user 2026-11-14)
		const double heading = drive == 2 && !strafe ? yaw : heading_of(app.yaw);
		if (drive != app.driveSent || strafe != app.strafeSent) { send_drive(drive, heading, strafe); }
		else if ((drive || turn || strafe) && app.driveAge >= 1 / SimView::kTickRate && std::abs(wrap_angle(heading - app.headingSent)) > kMinHeadingStep) { send_drive(drive, heading, strafe); }
		if (drive || strafe) { app.sim.aim(app.followId, wrap_angle(heading)); }
		if (drive || turn || strafe)
		{
			app.followBase = app.yaw;
			app.followOffset = 0;
		}
	}
	double rawLook = yaw;
	if (!driving() && !(app.sim.isPlayer(app.followId) && (held("camera_right") || held("camera_left"))))
	{
		double look = yaw;
		if (route && !route->points.empty())
		{
			const auto* p = &route->points.back();
			// kenshi-k16.2.237.7: the wire's route keeps the waypoints already passed (it is the whole path from the order's start, n stays 5 as the body
			// walks it), so a passed waypoint 15 u behind the body counted as the look-ahead and pointed the camera backwards. The look-ahead starts at
			// the end of the route segment nearest the body.
			size_t first = 0;
			double bestD = 1e30;
			for (size_t i = 0; i + 1 < route->points.size(); i++)
			{
				const auto &u = route->points[i], &v = route->points[i + 1];
				const double ex = v[0] - u[0], ez = v[2] - u[2], l2 = ex * ex + ez * ez;
				const double t = l2 > 0 ? std::clamp(((pos[0] - u[0]) * ex + (pos[2] - u[2]) * ez) / l2, 0.0, 1.0) : 0.0;
				const double d = std::hypot(pos[0] - (u[0] + t * ex), pos[2] - (u[2] + t * ez));
				if (d < bestD - 1e-6) { bestD = d; first = i + 1; }
			}
			for (size_t i = first; i < route->points.size(); i++) { const auto& q = route->points[i]; if (std::hypot(q[0] - pos[0], q[2] - pos[2]) >= kLookAhead) { p = &q; break; } }
			if (std::hypot((*p)[0] - pos[0], (*p)[2] - pos[2]) > 1) { look = std::atan2((*p)[0] - pos[0], (*p)[2] - pos[2]); }
		}
		// kenshi-k16.2.237.5 (user 2026-11-14: on a right-click walk down a hill the camera swung to the character's front): the route's look-ahead can lie
		// behind the drawn body (a switchback, a corner the body has not turned into yet, the drawn facing trailing the sim's), and the camera followed it
		// round to the front. The camera stays behind the drawn facing: a route heading more than kMaxRouteTurn off it is not followed.
		constexpr double kMaxRouteTurn = 60 * M_PI / 180;	// MINE
		rawLook = look;
		if (std::abs(wrap_angle(look - yaw)) > kMaxRouteTurn) { look = yaw; }
		// a body that turns sharply (a corner, a switchback) drags the camera round faster the further it is off, so it is never long in front
		const double dev = wrap_angle(M_PI - look - app.followBase);
		app.followBase += dev * std::min(1.0, (kSmooth + 4.0 * std::abs(dev)) * dt);
		app.yaw = app.followBase + app.followOffset;
	}
	{ static const bool followLog = std::getenv("KCLIENT_FOLLOWCAM_LOG") != nullptr; if (followLog && app.scripted()) { printf("followcam: frame %llu cam_heading %.1f drawn_yaw %.1f diff %.1f route_look %.1f route %s drive %u\n", (unsigned long long)app.frames, heading_of(app.yaw) * 180.0 / M_PI, yaw * 180.0 / M_PI, wrap_angle(heading_of(app.yaw) - yaw) * 180.0 / M_PI, rawLook * 180.0 / M_PI, route && !route->points.empty() ? "yes" : "no", app.driveSent); } }	// kenshi-k16.2.237.5: the camera's behind-heading against the drawn facing, per frame
	if (app.scripted() && std::getenv("KDLOOK"))	// kenshi-k16.2.237.9: per frame mouse delta, sent / predicted / server heading, drawn yaw, camera yaw, eye
	{
		const simlink::Dynamic& d = app.sim.decoder().chars.at(app.followId).cur;
		printf("dlook: %llu dx %.1f sent %.4f pred %.4f server %.4f serveryaw %.4f drawn %.4f cam %.4f pos %.2f %.2f eye %.2f %.2f %.2f\n", (unsigned long long)app.frames, app.lookDx, app.headingSent, app.sim.localHeading(), (double)d.heading, (double)d.yaw, yaw, heading_of(app.yaw), pos[0], pos[2], app.pos[0], app.pos[1], app.pos[2]);
		app.lookDx = 0;
	}
	if (app.scripted() && app.frames % 10 == 0 && app.sim.predicting())
	{
		const simlink::Dynamic& d = app.sim.decoder().chars.at(app.followId).cur;
		printf("predict: shown %.1f %.1f  server %.1f %.1f (tick %llu, drive %u)\n", pos[0], pos[2], d.pos[0], d.pos[2], (unsigned long long)d.tick, d.drive);
	}
	constexpr double kBoomMin = 4.0;	// u
	double f[3];
	forward_vec(app.yaw, app.pitch, f);
	const double pivot[3] = { pos[0], pos[1] + kEye, pos[2] };
	const double back[3] = { -f[0], -f[1], -f[2] };
	const Boom b = boom_step(pivot, back, app.followDist, kBoomMin, 30.0, true, dt, app.followPulled, app.followHold);
	for (int k = 0; k < 3; k++) { app.pos[k] = pivot[k] + back[k] * b.len; }
	app.followBoom = b.len;
	if (app.scripted()) { printf("boom: len %.2f wanted %.2f allowed %.2f hold %.2f cam %.1f %.1f %.1f %s\n", b.len, app.followDist, b.allowed, std::max(app.followHold, 0.0), app.pos[0], app.pos[1], app.pos[2], b.blocker.c_str()); }
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
	app.centerId = center;
	if (center) { app.buildings.engage(); }
}

// Leaving direct drive (V, Esc, or a double right click on the driven character; k16.2.238.1): back to the floating camera, auto-centred on the
// character (the camera stays where the drive left it, as V's own toggle does).
void leave_follow()
{
	const uint64_t was = app.followId;
	set_rts(was);	// drops the follow (and a running drive order) itself
	if (app.scripted()) { printf("follow %llu: left, camera mode rts center %llu\n", (unsigned long long)was, (unsigned long long)was); }
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

void follow_retarget();
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

// Floating camera (kinspect findings/camera_rts.md): WASD pans the focus at 500 d / 600 u/s and ends the auto-center, Q/E orbit, , / . tilt,
// Home / End zoom (MINE rate). The focus floats on the ground (MINE smoothing), leashed within kTether of the nearest selected character (else squad
// member); the camera keeps min(0.2 d, 20) + 10 over the ground by lifting the focus. Auto-center: the focus is the character + kCenterLift, no lag.
void update_rts(double dt)
{
	constexpr double kOrbitRate = 54 * M_PI / 180.0, kTiltRate = 12 * M_PI / 180.0, kZoomRate = 1.5, kSmooth = 8, kTether = 2250, kCenterLift = 10.8;	// rad/s, rad/s, e-folds/s (MINE), 1/s (MINE), u, u
	constexpr double kMinPitch = -66.9 * M_PI / 180.0, kMaxPitch = 80.0 * M_PI / 180.0;	// the original stops at +11.5; past it the collision boom shortens the orbit (kenshi-k16.2.353.1)
	const double panX = (held("camera_right") ? 1 : 0) - (held("camera_left") ? 1 : 0), panZ = (held("camera_forward") ? 1 : 0) - (held("camera_back") ? 1 : 0);
	double at[3];
	float hull[3];
	if (panX || panZ || (app.centerId && !app.sim.placed(app.centerId, at, hull))) { app.centerId = 0; }
	app.yaw += ((held("camera_rotate_right") ? 1 : 0) - (held("camera_rotate_left") ? 1 : 0)) * kOrbitRate * dt;
	app.pitch = std::clamp(app.pitch + ((held("camera_tilt+") ? 1 : 0) - (held("camera_tilt-") ? 1 : 0)) * kTiltRate * dt, kMinPitch, kMaxPitch);
	const double zoom = (held("camera_zoom_out") ? 1 : 0) - (held("camera_zoom_in") ? 1 : 0);
	app.rtsDist = std::clamp(app.rtsDist * std::exp(zoom * kZoomRate * dt), 10.0, 2000.0);
	if (app.centerId)
	{
		app.focusOn = "character";
		std::copy(at, at + 3, app.focus);
		app.focus[1] += kCenterLift;
	}
	else
	{
		const double speed = 500 * app.rtsDist / 600 * dt * (held("debug.fly_fast") ? kFastFactor : 1);
		const double sy = std::sin(app.yaw), cy = std::cos(app.yaw);	// the view's horizontal forward is (sy, -cy), its right (cy, sy)
		app.focus[0] += (sy * panZ + cy * panX) * speed;
		app.focus[2] += (-cy * panZ + sy * panX) * speed;
		double best = 1e30, near[3] = {};
		for (uint64_t id : app.sim.selected().empty() ? app.bar : app.sim.selected())
		{
			if (!app.sim.placed(id, at, hull)) { continue; }
			const double d = std::hypot(at[0] - app.focus[0], at[2] - app.focus[2]);
			if (d < best) { best = d; std::copy(at, at + 3, near); }
		}
		if (best > kTether && best < 1e30)
		{
			app.focus[0] = near[0] + (app.focus[0] - near[0]) * kTether / best;
			app.focus[2] = near[2] + (app.focus[2] - near[2]) * kTether / best;
		}
		const double lim = kHalfWorld - 1;
		app.focus[0] = std::clamp(app.focus[0], -lim, lim);
		app.focus[2] = std::clamp(app.focus[2], -lim, lim);
		app.focus[1] += (ground_under(app.focus[0], app.focus[2], &app.focusOn) - app.focus[1]) * std::min(1.0, kSmooth * dt);
	}
	double f[3];
	forward_vec(app.yaw, app.pitch, f);
	// Looking up (user 2026-11-18: look at the sky): the follow camera's collision boom (boom_step) shortens the orbit as the ground (indoors the
	// floor, walls, ceiling) comes between the focus and the eye, and eases back out to rtsDist, the player's zoom, as the view tilts down. At
	// or below the horizon the orbit is the original's, untouched.
	const double back[3] = { -f[0], -f[1], -f[2] };
	const Boom b = boom_step(app.focus, back, app.rtsDist, 0.0, std::max(30.0, 1.5 * app.rtsDist), f[1] > 0, dt, app.rtsPulled, app.rtsHold);
	// The boom's length blends in over pitch 0..11.5 deg (smoothstep), so tilting up from the horizon never jumps the orbit (kenshi-k16.2.353.2);
	// below 11.5 the ground lift below keeps the eye out of the ground, as the original did.
	const double wt = std::clamp(app.pitch / (11.5 * M_PI / 180.0), 0.0, 1.0), w = wt * wt * (3 - 2 * wt);
	const double len = app.rtsDist + (b.len - app.rtsDist) * w;
	app.rtsBoom = len;
	for (int k = 0; k < 3; k++) { app.pos[k] = app.focus[k] + back[k] * len; }
	const double lift = app.hm->height(app.pos[0], app.pos[2]) + std::min(0.2 * len, 20.0) + 10 - app.pos[1];
	if (lift > 0)
	{
		if (w < 1 || b.len >= app.rtsDist) { app.focus[1] += lift; }	// the original's: the focus rides up with the eye; a shortened boom lifts the eye alone, or the focus would climb
		app.pos[1] += lift;
	}
	if (app.scripted() && app.rtsLog) { printf("rtsboom: pitch %.3f len %.2f dist %.1f allowed %.2f eye %.1f %.1f %.1f ground %.1f\n", app.pitch * 180.0 / M_PI, len, app.rtsDist, b.allowed, app.pos[0], app.pos[1], app.pos[2], app.hm->height(app.pos[0], app.pos[2])); }
}


// The screenshot key's folder: screenshot.dir, else the XDG data home's kclient/screenshots.
std::filesystem::path shot_dir()
{
	if (!kShotDir.get().empty()) { return kShotDir.get(); }
	const char* xdg = getenv("XDG_DATA_HOME");
	const char* home = getenv("HOME");
	return std::filesystem::path(xdg && *xdg ? std::string(xdg) : std::string(home ? home : ".") + "/.local/share") / "kclient" / "screenshots";
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

// Written when the client issues a save, from the camera as it stands then (the order itself goes out beside it).
void camera_sidecar_save(const std::string& name)
{
	const CameraState s = camera_state_now();
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
		if (!out) { fprintf(stderr, "kclient: camera sidecar: cannot write %s\n", tmp.c_str()); return; }
	}
	std::filesystem::rename(tmp, path, ec);
	if (ec) { fprintf(stderr, "kclient: camera sidecar: cannot rename %s\n", path.c_str()); return; }
	printf("camera sidecar: saved \"%s\": %s (%s)\n", name.c_str(), camera_state_line(s).c_str(), path.c_str());
}

bool camera_sidecar_read(const std::string& name, CameraState& s)
{
	const std::filesystem::path path = camera_sidecar_dir() / camera_sidecar_file(name);
	std::ifstream in(path);
	if (!in) { printf("camera sidecar: none for \"%s\" (%s)\n", name.c_str(), path.c_str()); return false; }
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
		printf("camera sidecar: \"%s\" is unreadable (%s); the camera is not moved\n", name.c_str(), path.c_str());
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
	s.valid = true;
	return true;
}

void camera_sidecar_delete(const std::string& name)
{
	const std::filesystem::path path = camera_sidecar_dir() / camera_sidecar_file(name);
	std::error_code ec;
	if (std::filesystem::remove(path, ec) && !ec) { printf("camera sidecar: deleted with save \"%s\" (%s)\n", name.c_str(), path.c_str()); }
}

static uint64_t camera_subject();
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

const cfg::Text kDataDir{ cfg::Group{ "Paths" }, "client.data_dir", "", "Our own data files (UI layouts): empty = client/data, found from the executable (client/build/kclient)" };

std::string client_data_dir()
{
	if (!kDataDir.get().empty()) { return kDataDir.get(); }
	std::error_code ec;
	return (std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().parent_path() / "data").string();
}

// The repo root, found from the executable (client/build/kclient -> the repo root, one more level): where playtests/ and .beads/ are.
std::string repo_root()
{
	std::error_code ec;
	return (std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().parent_path().parent_path()).string();
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
		{ "TAB", SAPP_KEYCODE_TAB }, { "F2", SAPP_KEYCODE_F2 }, { "F3", SAPP_KEYCODE_F3 }, { "F7", SAPP_KEYCODE_F7 }, { "ESCAPE", SAPP_KEYCODE_ESCAPE },
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

static std::string pick_owner_report(const Pick& p, double mx, double my, const std::vector<dev::PickSkip>& skipped);	// the F12 sidecar's owner / primitive / rule lines (below)
const char* compass(double deg);	// "N" / "NE" / ... for a heading in degrees (below)

// The shared context of the dev debug ops (dev/debughost.hpp): the subsystems they read, filled once here, the host side.
void run_script(bool post);	// the viewer runs the script's input ops (dev/viewmesh.cpp)
void set_weather(const std::string& name);	// the scene test applies a scene's weather (dev/scenetest.cpp)
static bool world_loading(int* pending = nullptr);	// the scene test's load wait (dev/scenetest.cpp)
static void dev_hud_box(float out[4]);	// main.cpp's hud_box as the scene JSON's four numbers
static void dev_current_camera(float nearZ, float farZ, Camera& out);	// current_camera, by out-parameter
static dev::Context& dev_context()
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
		camera_subject, build_mvp, [] {
#ifdef KCLIENT_DEV_TOOLS
			if (app.lightsPanel) { app.lightsPanel->show(true); }
#endif
		}, [] { app.options->showTab("Keys"); }, [] { app.wmap.full = !app.wmap.full; }, shot_hud_text
#ifdef KCLIENT_DEBUG_VIEW
		, app.debugView, app.debugSubscribed, app.infoChar, debug_tree_open, tree_state_value
#endif
	};
	return cx;
}


// The script commands that only exist for development (--debug): dump / log print the client's internals, debug drives the
// debug panel, lab the animation lab, physics the Jolt smoke worlds, playtest the playtest store's entry point. The rest of
// the language drives the game as a player would.
static bool debug_script_op(const std::string& op) { return op == "debug" || op == "dump" || op == "log" || op == "physics" || op == "lab" || op == "playtest" || op == "trails" || op == "sound" || op == "serverhold" || op == "serverfail" || op == "titlehold" || op == "hints" || op == "light"; }

// A scripted run holds the characters' animation still (charDt 0, for repeatable shots) unless the script says `set animate 1`.
// A locomotion log then shows only the clips a state change snapped to: a gait handover (walk -> jog -> run) keeps the linear
// crossfade, which needs time, so the record sticks at the speed the state last changed and reads as a client bug. Say so once when
// such a log starts (kenshi-h2v.3.4.3); the footlog tool's own script sets animate 1 (client/tools/footlog_stop.txt).
static void warn_frozen_anim_log(const char* op)
{
	if (app.scripted() && !app.scriptAnim) { fprintf(stderr, "kclient: %s: the scripted run freezes the characters' animation (set animate 1 to animate them): the clips, weights and phase below are held\n", op); }
}

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
static void stall_point(int at)
{
	if (gStallAt[at] <= 0) { return; }
	const double ms = gStallAt[at];
	gStallAt[at] = 0;
	std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(ms));
}

void exec_cmd(const Cmd& c)
{
	logicthread::renderIdle();	// overlap (rule 12): an op may poke any renderer (partdepth, wind, the lab); scripted overlap and the console gate here
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
		event(&e);
	}
	else if (c.op == "type" && !a.empty())	// type TEXT...: one CHAR event per character (an edit box's typing)
	{
		std::string text;
		for (const std::string& w : a) { text += (text.empty() ? "" : " ") + w; }
		e.type = SAPP_EVENTTYPE_CHAR;
		for (unsigned char ch : text)
		{
			e.char_code = ch;
			event(&e);
		}
	}
	else if (c.op == "mouse" && a.size() == 2 && (a[0] == "down" || a[0] == "up"))	// mouse down|up left|right|middle
	{
		e.type = a[0] == "down" ? SAPP_EVENTTYPE_MOUSE_DOWN : SAPP_EVENTTYPE_MOUSE_UP;
		e.mouse_button = a[1] == "left" ? SAPP_MOUSEBUTTON_LEFT : a[1] == "middle" ? SAPP_MOUSEBUTTON_MIDDLE : SAPP_MOUSEBUTTON_RIGHT;
		e.mouse_x = (float)app.mouseX;
		e.mouse_y = (float)app.mouseY;
		event(&e);
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
	else if (c.op == "light")	// light <name>|all on|off|solo: the light debugger's switches (dev/lightterms.hpp; docs/CLIENT.md "Light terms")
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
			printf("%s", app.buildings.drawnFloorReport(ch.cur.building, feet[0], feet[2], feet[1] + 1.0, 6.0).c_str());
		}
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
		if (!set_follow_id(id)) { script_error(c); }
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
	else if (c.op == "creator" && !a.empty() && app.creator.active())	// creator list | race NAME... [female] | slider VALUE NAME... | head N | hair NAME...|none | beard NAME...|none | haircol NAME... | skin F | random SEED | wear ARMOUR | weapon WEAPON | clothes on|off | tab body|face|hair | turn DEG
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
			for (const Characters::EditorRange& r : app.creator.ranges()) { printf("creator:   %s / %s %g..%g = %g\n", r.category.c_str(), r.name.c_str(), r.min, r.max, app.creator.value(r.name)); }
			for (size_t i = 0; i < ch.hairs.size(); i++) { printf("creator:   hair %s (weight %g)\n", ch.hairs[i].c_str(), ch.hairWeights[i]); }
			for (size_t i = 0; i < ch.beards.size(); i++) { printf("creator:   beard %s (weight %g)\n", ch.beards[i].c_str(), ch.beardWeights[i]); }
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
		else if (a[0] == "turn" && a.size() == 2) { app.creator.turn((float)(atof(a[1].c_str()) * M_PI / 180.0)); }
		else if (a[0] == "wear") { s.wear.push_back(rest); ok = app.creator.apply(err); }
		else if (a[0] == "weapon") { s.weapons.push_back(rest); ok = app.creator.apply(err); }
		else if (a[0] == "clothes" && a.size() == 2) { app.creator.clothes = a[1] == "on"; ok = app.creator.apply(err); }
		else if (a[0] == "tab" && a.size() == 2)
		{
			const int page = a[1] == "face" ? ui::kCreatorFace : a[1] == "hair" ? ui::kCreatorHair : ui::kCreatorBody;
			if (app.creatorUi && app.creatorUi->tab(page)) { static_cast<ui::Button*>(app.creatorUi->tab(page))->onClicked(); }
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
		else if (a[0] == "simspeed") { app.sim.setSpeed((float)atof(a[1].c_str())); }	// the sim's game speed (order SET_SPEED)
		else if (a[0] == "day") { app.sky.clock.day = atoi(a[1].c_str()) - 1; }
		else if (a[0] == "weather") { set_weather(a[1]); }
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
	else if (c.op == "transfer" && a.size() == 2 && a[0] == "sell" && cont_scripted(true))	// transfer sell N: order 68 for the user's entry N into the open container, as the UI's move (a TRADE stock: a sale)
	{
		const App::OpenCont* oc = cont_scripted(true);
		const simlink::Inventory* inv = app.sim.inventory(oc->user);
		const size_t n = (size_t)atoi(a[1].c_str());
		if (!inv || n >= inv->items.size() || !app.sim.transferEntry(oc->user, oc->id, inv->version, (uint16_t)n, inv->items[n].count, false, 255, 0, 0)) { script_error(c); }
	}
	else if (c.op == "machine" && dev::machineOp(dev_context(), a)) {}	// machine id=N: N's machine window opens (dev/debughost.cpp)
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
	else if (c.op == "interact" && a.size() == 2 && a[0].starts_with("id="))	// interact id=N ACTION: order 60 for the selection on N (6 talk: a moving target, without aiming the pointer; 5 loot opens the body as the UI does)
	{
		const uint64_t id = strtoull(a[0].c_str() + 3, nullptr, 10);
		const int action = atoi(a[1].c_str());
		if (!(action == 5 ? open_body(id, false) : app.sim.interact(id, (uint8_t)action))) { script_error(c); }
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
	else if (c.op == "hints" && a.size() > 1 && a[0] == "portrait")	// hints portrait NAME...: the list a portrait offers now (the same text as `hints at`)
	{
		std::string text;
		for (size_t i = 1; i < a.size(); i++) { text += (text.empty() ? "" : " ") + a[i]; }
		const uint64_t id = named_character(text);
		if (!id) { script_error(c); }
		else { fputs(hints::format_hover(hints::hints_for_portrait(id, hints_context())).c_str(), stdout); }
	}
	else if (c.op == "squadfirst" && !a.empty())	// squadfirst NAME... | id=N: the squads window's drag to first place: the character leads its squad (order SQUAD_MOVE, index 0)
	{
		std::string text;
		for (const std::string& w : a) { text += (text.empty() ? "" : " ") + w; }
		const uint64_t id = named_character(text);
		uint64_t squad = 0;
		for (const simlink::PlayerSquads::Squad& q : app.sim.decoder().playerSquads.squads) { if (std::find(q.members.begin(), q.members.end(), id) != q.members.end()) { squad = q.id; } }
		if (!id || !squad || !app.sim.squadMove(id, squad, 0, false)) { script_error(c); }
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
	else if (c.op == "dump" && a.size() == 1 && a[0] == "title")	// dump title: the title screen's state: its held click, the offline line and the new game panel (kenshi-k16.2.210.4; stdout)
	{
		printf("title: shown %d, pending %d, status \"%s\", new game %d\n", title_shown() ? 1 : 0, app.menuPending, title_status().c_str(),
			app.newGame && app.newGame->shown() ? 1 : 0);
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
	const std::string base = shotPath.ends_with(".png") ? shotPath.substr(0, shotPath.size() - 4) : shotPath;
	const std::string path = base + ".route.tsv";
	FILE* f = fopen(path.c_str(), "w");
	if (!f) { fprintf(stderr, "kclient: route: cannot write %s\n", path.c_str()); return; }
	fprintf(f, "# kclient route sidecar (kenshi-k16.2.231.1): the selected characters' CHAR_ROUTE as the wire has it at the shot's frame, the last 8\n");
	fprintf(f, "# move_to orders this client sent each and the answer the wire gave, and the characters' wire position and building. One row a fact.\n");
	fprintf(f, "# shot\t%s\tframe\t%d\ttick\t%.2f\t%s\n", tsv_text(shotPath).c_str(), frame, app.sim.tick(), tsv_text(app.sim.status()).c_str());
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
		for (const SimView::MoveOrder& o : app.sim.moveOrders(id))
		{
			fprintf(f, "order\t%llu\tseq\t%u\ttick\t%llu\tgoal\t%.2f\t%.2f\t%.2f\tyValid\t%d\tqueue\t%d\twhat\t%s\tbuilding\t%s\tfloor\t%d\toutcome\t%s\n",
				(unsigned long long)id, o.seq, (unsigned long long)o.tick, o.goal[0], o.goal[1], o.goal[2], o.yValid ? 1 : 0, o.queue ? 1 : 0,
				tsv_text(o.what).c_str(), tsv_text(o.buildingName).c_str(), o.floor, tsv_text(o.outcome).c_str());
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
		if (c.op == "probe") { dev::probeOp(dev_context(), c.args); }	// a request: the scene depth once the frame is complete (dev/probe.cpp)
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
				printf("  buildings: %zu instances, %zu draw groups, %zu submesh draws, %zu with a real texture (%.1f%%); meshes ready %zu failed %zu, textures ready %zu failed %zu; frame %.1f ms\n",
					bs.instancesDrawn, bs.groups, bs.submeshDraws, bs.realTextured, bs.submeshDraws ? 100.0 * bs.realTextured / bs.submeshDraws : 0.0,
					bs.meshesReady, bs.meshesFailed, bs.texturesReady, bs.texturesFailed, app.frameMs);
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
				printf("  foliage parts: %zu submesh draws held for a streaming diffuse, %zu drawn on a not-ready diffuse\n", fs.held, fs.greyDraws);
				printf("  foliage: %s; %zu of %zu resident instances are children\n", app.foliage.cellReport(app.pos).c_str(), fs.children, fs.instancesLoaded);
				printf("  foliage CPU this frame: cull+list %.2f ms, cell upload %.2f ms, draw submit %.2f ms; whole frame CPU %.2f ms (smoothed %.2f)\n", fs.cullMs, fs.uploadMs, framestats::stats().foliage.renderMs, framestats::stats().cpu.lastMs, framestats::stats().cpu.ms);
			}
			});
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

void init()
{
	(void)dev_context();	// filled here, on the main thread, before any frame (its first use is on the logic side)
	sg_desc sd = {};
	sd.environment = sglue_environment();
	app.swapFormat = sd.environment.defaults;
	if (!dev::viewing())	// every scene pipeline renders into the HDR target (hdr.hpp); the overlay below keeps the swapchain format
	{
		sd.environment.defaults.color_format = SG_PIXELFORMAT_RGBA16F;
		sd.environment.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;	// the HDR target's depth image (hdr.cpp, reflection.cpp): a pipeline that leaves .depth.pixel_format alone must match it, not the swapchain's (validation: VALIDATE_APIP_DEPTHSTENCILATTACHMENT_FORMAT)
	}
	sd.logger.func = log_cb;
	sd.image_pool_size = sd.view_pool_size = (int)Terrain::kMaxResident + 512 + 2048;	// one image + view per resident chunk
	sd.buffer_pool_size = 8192;
	sd.shader_pool_size = 128;
	sd.pipeline_pool_size = 256;
	sg_setup(&sd);
	dev::sgOwnerInstall();	// dev builds: every sg_* call off this thread aborts (dev/sgowner.cpp)
	{
		// shaders/origin.inc flips an offscreen read where sokol-shdc does not define SOKOL_GLSL (every target but GL); the backend must say the same
#if defined(SOKOL_GLCORE)
		const bool glslOrigin = false;	// GL: bottom left
#else
		const bool glslOrigin = true;
#endif
		if (sg_query_features().origin_top_left != glslOrigin) { fprintf(stderr, "kclient: the texture origin sokol reports (top left: %d) is not the one shaders/origin.inc compiled for (%d): offscreen reads are upside down\n", (int)sg_query_features().origin_top_left, (int)glslOrigin); }
	}
#if defined(SOKOL_GLCORE)
	printf("GL_RENDERER: %s | GL_VENDOR: %s | GL_VERSION: %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VENDOR), (const char*)glGetString(GL_VERSION));
#endif
	GPU_INIT(app.gpu);
	if (dev::viewing())
	{
		sgl_desc_t gd = {};
		gd.logger.func = slog_func;
		sgl_setup(&gd);
		sdtx_desc_t td = {};
		td.fonts[0] = sdtx_font_kc853();
		td.logger.func = slog_func;
		sdtx_setup(&td);
		init_view();
		return;
	}
	sgl_desc_t gd = {};
	gd.logger.func = slog_func;
	gd.color_format = app.swapFormat.color_format;
	gd.depth_format = app.swapFormat.depth_format;
	gd.sample_count = app.swapFormat.sample_count;
	// The default context: the HUD, minimap, markers, navmesh and the windows (the trails have their own, k16.19.6.1). 262144, not sokol's 65536: the
	// navmesh overlay alone may take 56,000 (its cap), and the windows must keep room beside it (client lead, k16.19.6.1 review).
	gd.max_vertices = 1 << 18;
	sgl_setup(&gd);
	// The debug trails' own sgl context: a full history can no longer fill the default context and silently drop the HUD and windows with it (k16.19.6).
	app.trails.initSgl(app.swapFormat.color_format, app.swapFormat.depth_format, app.swapFormat.sample_count);
	sdtx_desc_t td = {};
	td.fonts[0] = sdtx_font_kc853();
	td.context.color_format = app.swapFormat.color_format;
	td.context.depth_format = app.swapFormat.depth_format;
	td.context.sample_count = app.swapFormat.sample_count;
	td.context.char_buf_size = 16384;	// the HUD lines plus the F1 key list (every bound action) overflow the default 4096
	td.logger.func = slog_func;
	sdtx_setup(&td);
	init_ui();
	// The rest of the startup is a list of steps (a step returns false to be retried next frame). Scripted runs (screenshots, scenes) run them all here;
	// interactive runs run one per frame behind the loading screen (frame_loading), so the window presents and stays responsive.
	app.loadSteps = {
		{ "graphics: HDR target, fog volumes, shadows", []()
		{
			app.detailFut = std::async(std::launch::async, [] { return app.terrain.detail.prepare(app.opt.data); });	// the slowest part of startup (CPU only), overlapped with the steps below
			if (wants_characters()) { app.charsFut = std::async(std::launch::async, [] { return kworld::loadMergedGameData(app.opt.data); }); }	// the characters step's CPU work (kenshi-k16.2.26): overlaps these steps, never a frame
			if (!app.opt.simlink.empty()) { app.hashFut = std::async(std::launch::async, [] { return kworld::dataHash(kworld::resolveLoadOrder(app.opt.data)); }); }	// the simlink hello's data hash (kenshi-k16.2.26)
			app.hdr.init(app.swapFormat);
			app.refl.init();
			app.hdr.loadHaze(app.opt.data);
			app.post.init(app.swapFormat);
			interiorclip::init();
			app.lightVolumes.init();
			app.sunShafts.init();
			app.dust.init();
			if (app.opt.fogVolumes && app.fogvol.load(app.opt.data)) { app.fogvol.init(); }
			else { app.fogvol.enabled = false; }
			if (app.opt.particles) { app.particles.init(app.opt.data); }
			app.blood.init(app.opt.data);
			app.portraits.init();
			app.portraitTable.setOrigin(sg_query_features().origin_top_left);	// the uv's row order, read here once (logic asks sokol nothing)
			if (app.portraits.atlas().id != SG_INVALID_ID)	// the UI's id for the atlas, bound here at its creation (the portraits' refs carry it)
			{
				app.portraitsTex = dl::newTex();
				dl::gpu::bind(app.portraitsTex, app.portraits.atlas(), app.portraits.sampler());
			}
			app.terrain.blood = &app.blood;
			app.shadow.init();
			app.shadow.enabled = app.opt.shadows;
			app.shadow.stagger = app.opt.shadowStagger >= 0 ? app.opt.shadowStagger > 0 : !app.scripted();
			app.terrain.shadow = &app.shadow;
			app.buildings.shadow = &app.shadow;
			app.chars.shadow = &app.shadow;	// with the others, not in the lazy characters step: a run without characters still calls chars.render (k16.2.281)
			return true;
		} },
		{ "terrain: biome map", []()
		{
			if (!app.terrain.init(&*app.hm, app.opt.data + "/newland/land/biomemap.png"))
			{
				fprintf(stderr, "kclient: cannot load biomemap.png under %s\n", app.opt.data.c_str());
				exit(1);
			}
			return true;
		} },
		{ "terrain: colour map", []()
		{
			static std::future<bool> fut;	// the cache read (and a first-run build) on a worker: the step polls it (kenshi-k16.2.26)
			if (!fut.valid()) { fut = std::async(std::launch::async, [] { return app.terrain.colour.load(app.opt.data + "/newland/land/overlaymaps"); }); }
			if (!app.scripted() && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
			app.terrain.initColour(fut.get());
			if (app.terrain.hasColour)
			{
				const GroundColour& c = app.terrain.colour;
				printf("colour: %dx%d BC1 uploaded, cache %.1f MiB%s\n", c.uploadedSize, c.uploadedSize, c.cacheBytes / 1048576.0,
					c.buildSeconds > 0 ? (" (built in " + std::to_string(c.buildSeconds) + " s)").c_str() : " (existing)");
			}
			return true;
		} },
		{ "world map image", []()
		{
			if (!app.wmap.init(app.opt.map))
			{
				fprintf(stderr, "kclient: cannot load map image %s (need 8192x8192 DXT1 DDS)\n", app.opt.map.c_str());
				exit(1);
			}
			return true;
		} },
		{ "squads, towns, sky constants", []()
		{
			if (app.opt.squads.empty())
			{
				std::error_code ec;
				app.opt.squads = (std::filesystem::read_symlink("/proc/self/exe", ec).parent_path().parent_path().parent_path() / "cpp/build/default_world_squads.json").string();
			}
			app.squads.load(app.opt.squads, *app.hm);
			app.squads.init();
			app.townFut = std::async(std::launch::async, TownSet::load, app.opt.data);
			app.skyFut = std::async(std::launch::async, SkyConstants::load, app.opt.data);
			return true;
		} },
		{ "sky and weather", []()
		{
			const std::string root = std::filesystem::path(app.opt.data).parent_path().string();
			if (!app.res.init(root)) { fprintf(stderr, "kclient: no resources.cfg under %s\n", root.c_str()); exit(1); }
			app.sky.init(app.res);
			{
				std::string err;
				if (!app.creatorScene3d.load(app.opt.data, err)) { fprintf(stderr, "kclient: creator scene: %s\n", err.c_str()); exit(1); }
			}
			app.wx.init();
			if (!spec_cube_init(app.opt.data)) { fprintf(stderr, "kclient: mp_specularity.dds not usable under %s/materials: no environment specular\n", app.opt.data.c_str()); }
			app.atlasFut = std::async(std::launch::async, WeatherAtlas::load, app.opt.data);
			audiohook_init(app.opt.data);
			return true;
		} },
		{ "water", []()
		{
			if (app.opt.water)
			{
				static std::future<Water::Prepared> fut;	// the PNG decodes on a worker (kenshi-k16.2.26: 2048^2 flowmap, ~40 ms); the uploads are here
				if (!fut.valid()) { fut = std::async(std::launch::async, [] { return Water::prepare(app.opt.data); }); }
				if (!app.scripted() && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
				if (app.water.init(app.opt.data, fut.get())) { app.water.flat = app.opt.waterFlat; }
				else { app.opt.water = false; }
			}
			else { app.opt.water = false; }
			app.sky.clock.setHours(app.opt.time);
			app.sky.clock.setSpeed(app.opt.timescale);
			app.sky.clock.day = app.opt.day - 1;
			app.sky.setSkyHours(app.opt.skyHours);
			app.weatherAuto = app.opt.weather.empty() || lower(app.opt.weather) == "auto";
			if (!app.weatherAuto)
			{
				app.weatherFut = std::async(std::launch::async, [](std::string data, std::string name)
				{
					SkyWeather w;
					w.load(data, name);
					return w;
				}, app.opt.data, app.opt.weather);
			}
			return true;
		} },
		{ "terrain: detail textures", []()
		{
			static TerrainDetail::Prepared prepared;
			static int part = 0;	// one upload part a frame (kenshi-k16.2.26): the window keeps redrawing during the big uploads
			if (part == 0)
			{
				if (!app.scripted() && app.detailFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }	// still reading on the worker
				prepared = app.detailFut.get();
			}
			if (app.terrain.initDetailPart(prepared, part) > 0) { part++; return false; }
			if (app.terrain.detail.ok)
			{
				const TerrainDetail& d = app.terrain.detail;
				printf("terrain detail: %d biomes, %d diffuse + %d normal layers (%d^2), overlay %d^2, loaded in %.2f s\n", d.biomeCount, d.layersDiffuse, d.layersNormal, TerrainDetail::kLayerSize, TerrainDetail::kOverlaySize, d.loadSeconds);
			}
			return true;
		} },
		{ "buildings and materials", []()
		{
			// the stealth arrows' mesh (kenshi-h2v.27.2): the game's own data/meshes/arrow.mesh, loaded once; a failure is one stderr line and no
			// arrows (never a substitute shape)
			{
				std::string err;
				const std::string at = app.res.locate("data/meshes/arrow.mesh");
				const std::string file = at.empty() ? app.res.find("arrow.mesh") : at;
				if (file.empty() || !app.arrows.init(file, err)) { fprintf(stderr, "kclient: stealth arrows: %s\n", file.empty() ? "arrow.mesh not found" : err.c_str()); }
			}
			if (app.opt.bradius > 0 || app.opt.foliage)
			{
				app.mats.load(app.res);
				app.buildings.init(&app.res, &app.mats);
				app.buildings.radius = app.opt.bradius;
				app.buildings.offsetFrame = app.opt.offsetFrame;
				app.buildings.hideMarkers = app.opt.hideMarkers;
				app.buildings.interiors = app.opt.interiors;
				app.buildings.setGround(app.terrain.colour.view, app.terrain.colour.smp, app.terrain.colour.alphaView);
				app.buildings.detail = &app.terrain.detail;
				if (app.opt.bradius > 0) { app.bldFut = std::async(std::launch::async, BuildingSet::load, app.opt.data, app.opt.features); }
				else { app.buildings.setData(BuildingSet()); app.bldSet = true; }
			}
			return true;
		} },
		{ "foliage and grass", []()
		{
			if (app.opt.foliage)
			{
				app.foliage.radius = app.opt.fradius;
				app.foliage.foliageRange = app.opt.foliageRange;
				app.foliage.init(&app.buildings, app.opt.data);
				app.foliageOn = true;
				if (app.opt.grass)
				{
					app.grass.rangeScale = app.opt.grassRange;
					app.grass.density = app.opt.grassDensity;
					app.grass.paintMap = app.opt.grassPaint;
					app.grass.shadow = &app.shadow;
					app.grass.init(&app.res, &app.foliage, app.opt.data);
					app.grassOn = true;
				}
			}
			return true;
		} },
		{ "characters", []()
		{
			if (wants_characters())
			{
				if (!init_characters() && app.charsPhase < 2) { return false; }	// a phase still to run: the step is retried next frame (kenshi-k16.2.26); 2 = done or failed
				if (app.charsReady) { place_characters(app.opt.characters, app.opt.x, app.opt.z, app.opt.yaw * M_PI / 180.0); }
			}
			if (!app.opt.creator.empty() && app.charsReady) { open_creator(app.opt.creator); }
			place_blood(app.opt.blood, app.opt.x, app.opt.z, app.opt.yaw * M_PI / 180.0);
			std::string err;
			app.sim.startTick = app.opt.simlinkTick;
			if (!app.opt.simlink.empty() && app.charsReady)
			{
				if (!app.hashErr.empty()) { err = "data hash: " + app.hashErr; }
				else
				{
					// kenshi-k16.2.268: a scripted run's first world frame blocks on these futures (.get(), 9..12 s under load) before its first pump, and the
					// server ends a session whose HELLO is not in 10 s of the connect. Wait for them here, before the connect, so the timer starts after.
					if (app.scripted())
					{
						if (app.townFut.valid()) { app.townFut.wait(); }
						if (app.bldFut.valid()) { app.bldFut.wait(); }
						if (app.skyFut.valid()) { app.skyFut.wait(); }
					}
					app.sim.open(app.opt.simlink, app.dataHash, err);
				}
				if (!err.empty()) { fprintf(stderr, "kclient: --simlink: %s\n", err.c_str()); }
			}
			if (app.sim.active()) { sim_opened(); }
			return true;
		} },
	};
	if (app.scripted()) { while (app.loading()) { run_load_step(); } }
	else { app.loadScreen.init(app.opt.data, app.uiOn ? dl::gpu::view(ui::gfx::atlas()) : sg_view{}); }
}

// The Buildings_Click switch values, indexed by the BUILDING record's `select sound` int (the original's 1405504a0 reads it; 0 = no click sound and
// the event is skipped). The names are the game's switch table, kenshi-inspect findings/rwh65/switches.tsv (141e17308's values); past the table: ""
// rather than a guess.
const char* buildings_click_sound(int v)
{
	static const char* const kNames[] = { "", "Bed", "Dummy", "Farm", "Generator", "Grain_Silo", "House", "Mine_Ore", "Mine_Stone", "Research_Bench",
		"Store_Armor", "Store_Building_Materials", "Store_Flour", "Store_Ore", "Store_Prisoner", "Store_Rum", "Store_Stones", "Stove", "Well",
		"Wind_Generator", "Chain_Bench", "Fabric_Loom", "Leather_Bench", "Medical_Desk", "Plate_Station", "Shop_Counter", "Smithy", "Stone_Processor",
		"Armorer", "Bar", "Boxes", "Campfire", "Gate", "Gen_Goods", "Hive_Hut", "Lamp", "Long_House", "Outhouse", "Store_Armor",
		"Store_Building_Materials", "Store_Flour", "Store_Ore", "Store_Prisoner", "Tailor", "Tower", "Travel", "Turret", "Wall_Metal",
		"Wall_Station_House", "Wall_Stone", "Wall_Wood", "Wood_Obj_Big", "Wood_Obj_Small" };
	return v >= 0 && v < (int)std::size(kNames) ? kNames[v] : "";
}

// The original's new-game start order (k16.17.4.2; kenshi-inspect findings/newgame_startorder_static.md, kenshi-rwh.72, our own port of its reference
// implementation): there is no order field. The panel walks the game-data container's per-type index, a boost::unordered_map<stringID, GameData*> kept at
// B = 16 buckets while 16 or fewer records load: a sid's bucket is its hash & (B - 1); the first record of a bucket becomes the global list's head and a
// later record of the same bucket goes in front of that bucket's first, so the list reads buckets in reverse order of first appearance, newest first inside
// each bucket. Feed the sids in merge order (GameData::byType's stable merge order: gamedata.base, data/*.mod, then the mods.cfg list; a repeated sid keeps
// its first slot). The panel opens on the hard-coded sid 1980-gamedata.base (Wanderer) and its arrows wrap, so the player's list is this walk rotated to
// that sid (a set without it opens on the walk's first entry).
// The walk itself (the sid hash, the buckets, the rehash past 16 records) is sidorder.hpp, shared with the animation manager's idle list (kenshi-rwh.101).
std::vector<std::string> start_order(const std::vector<std::string>& sidsInLoadOrder)
{
	std::vector<std::string> out;
	for (const std::size_t i : sidorder::walk(sidsInLoadOrder)) { out.push_back(sidsInLoadOrder[i]); }
	return out;
}

// --character SPEC: `;`-separated keys (clip names hold spaces): race=NAME, male / female, clip=NAME (a raw clip), anim=ANIMATION (repeatable, layered), move=SPEED (locomotion: picks and blends the records; 0 = idle), strafe=LATERAL SPEED, combat, stealth,
// action=COMBAT_TECHNIQUE[ override]|ANIMATION|clip (layer 1, full body; a block: upper body, ` override` = the R arm override clone) with action_t=SECONDS since it started and action_speed=F (default: the technique's mult),
// shape=SLIDER:VALUE (repeatable; editor sliders by name, raw values, e.g. shape=Height:115; the rest at the race xml's mid), muscle=M, skinny=0..1, strong=0..1 (normal map blend), wound=PART:HEALTH (repeatable; LOCATIONAL_DAMAGE part, health 0..1: the pain overlay), stance=ANIMATION, legs=R,L (leg values, 100 healthy), posture= / shoulders= / neck= (editor 0..100), t=SECONDS, speed=F, head=N, skin=0..1, wear=ARMOUR|CONTAINER (repeatable; CONTAINER = a backpack),
// weapon=WEAPON|CROSSBOW[/MATERIAL_SPECS_WEAPON] (repeatable: hip, back), drawn (the first weapon in hand), loaded (crossbows hold a bolt), faction=FACTION (armour colours), carry=left|right (carrying a body),
// carried=N (carried by the Nth spec of this list, which has carry=; `carry me` on its shoulder, at= / yaw= unused),
// hair=ATTACHMENT, beard=ATTACHMENT, haircol=COLOR_DATA, settle=SECONDS (steps the animation clock that long before the first frame: a still character's idle fit engages), yaw=DEG (added to facing the camera), at=FORWARD,RIGHT (world units from the start camera; default: a row 30 ahead). Feet on the terrain.
bool init_characters()
{
	try
	{
		// The CPU work (the game data merge, file reads, decode) is the future step 1 started (kenshi-k16.2.26): poll it, never block the frame. A
		// scripted run waits here (it runs every step in one frame for deterministic shots, where polling would only spin).
		if (app.charsPhase == 0)
		{
			if (!app.charsFut.valid()) { app.charsFut = std::async(std::launch::async, [] { return kworld::loadMergedGameData(app.opt.data); }); }
			// Both workers' results before either is consumed (a .get() on a retried phase must not be needed twice).
			const bool charsIn = app.scripted() || app.charsFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			const bool hashIn = !app.hashFut.valid() || app.scripted() || app.hashFut.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
			if (!charsIn || !hashIn) { return false; }	// still on the workers: retried next frame
			app.gdPending = std::make_unique<kdata::GameData>(app.charsFut.get());
			if (app.hashFut.valid())
			{
				try { app.dataHash = app.hashFut.get(); }
				catch (const std::exception& e) { app.hashErr = e.what(); }
			}
			app.chars.shadow = &app.shadow;
			app.chars.culled = char_cut_hidden;	// on a floor the cut hides (user 2026-11-05: the guard on the roof at floor 0; k16.2.264.1: by the wire's own floor, shared with the tags and labels)
			app.chars.init(&app.res, *app.gdPending);
			// the animation lab's own character set: never drawn in the world (it renders offscreen only). Setup is cheap; the lab inits on its first
			// open or `lab` op (every game start used to pay for it), and a failure there is a warning: the client runs on.
			dev::labSetup(app.res, app.opt.data, app.shadow);
			app.charsPhase = 1;
			return false;	// the panel records next frames: they and the asset build used to be one 134 ms step (kenshi-k16.2.26)
		}
		// The panel records (kenshi-k16.2.26): one job per record, run under a per-frame time budget. As one step, or one record type a frame, they
		// still blocked the window for 134-190 ms; the research records' reference lookups alone were ~190 ms.
		static std::vector<std::function<void()>> recs;
		static size_t recsNext = 0;
		if (recs.empty() && recsNext == 0)
		{
			const kdata::GameData* gdp = app.gdPending.get();
			const kdata::GameData& gd = *gdp;
			const auto itemName = [](const kdata::GameData& d, const std::string& id) { const kdata::Record* r = d.find(id); return r ? r->name : id; };
			// The new game panel's starts (docs/reference/STARTS_CAGES_STATIC.md a0: description / difficulty / style are the window's; money the start's)
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			{
				recs.push_back([r]
				{
					std::string info;
					for (const char* k : { "difficulty", "style" }) { if (std::string v = r->stringOr(k); !v.empty()) { info += (char)std::toupper((unsigned char)k[0]) + std::string(k + 1) + "\t" + v + "\n"; } }
					info += "Money\t" + std::to_string(r->intOr("money", 0)) + " c\n\n" + r->stringOr("description");
					app.starts.push_back({ r->stringID, r->name, info });
				});
			}
			recs.push_back([]
			{
				// The original's order (k16.17.4.2): the walk over the sids in merge order (app.starts is filled in byType order), then rotated so the
				// preset start 1980-gamedata.base comes first; a set without it opens on the walk's first entry.
				std::vector<std::string> sids;
				for (const ui::StartEntry& s : app.starts) { sids.push_back(s.id); }
				const std::vector<std::string> walk = start_order(sids);
				std::vector<ui::StartEntry> ordered;
				for (const std::string& sid : walk)
				{
					for (const ui::StartEntry& s : app.starts) { if (s.id == sid) { ordered.push_back(s); break; } }
				}
				const auto preset = std::find_if(ordered.begin(), ordered.end(), [](const ui::StartEntry& s) { return s.id == "1980-gamedata.base"; });
				const bool rotated = preset != ordered.end();
				if (rotated) { std::rotate(ordered.begin(), preset, ordered.end()); }
				app.starts = std::move(ordered);
				if (app.opt.debug)
				{
					std::string names;
					for (size_t i = 0; i < app.starts.size(); i++) { names += (i ? ", " : "") + app.starts[i].title; }
					printf("new game: starts order (%zu, the original's walk rotated to %s): %s\n", app.starts.size(),
						rotated ? "1980-gamedata.base" : "the walk's first (1980-gamedata.base absent)", names.c_str());
					// The stock set (findings section 3): the walk's array and its rotation are known, so a drift shows here.
					static const std::vector<std::string> kStock = {
						"1533659-rebirth.mod", "1533582-rebirth.mod", "47924-rebirth.mod", "1981-gamedata.base", "3062-gamedata.base", "1980-gamedata.base",
						"1994-gamedata.base", "2060-gamedata.base", "2918-gamedata.base", "1533657-rebirth.mod", "1985-gamedata.base", "5425-gamedata.base",
						"1532934-rebirth.mod", "2287-gamedata.base" };
					// The shipped rebirth.mod marks 5425-gamedata.base (The Hungover Ninja) REMOVED, so the stock set is the table's 14 less it; a removal
					// only unlinks one node, so the expected walk is the table filtered to the sids present.
					bool stock = true;
					for (const std::string& sid : sids) { stock = stock && std::find(kStock.begin(), kStock.end(), sid) != kStock.end(); }
					std::vector<std::string> want;
					for (const std::string& sid : kStock) { if (std::find(sids.begin(), sids.end(), sid) != sids.end()) { want.push_back(sid); } }
					if (stock)
					{
						const bool ok = walk == want && app.starts.size() == want.size() && !app.starts.empty() && app.starts[0].id == "1980-gamedata.base"
							&& (app.starts.size() < 2 || app.starts[1].id == "1994-gamedata.base");
						const bool ninja = std::find(sids.begin(), sids.end(), "5425-gamedata.base") != sids.end();
						printf("new game: starts order check: %s (the stock %zu, the findings table%s, rotated: 1980-gamedata.base, 1994-gamedata.base)\n",
							ok ? "ok" : "MISMATCH", sids.size(), ninja ? "" : " less The Hungover Ninja, which rebirth.mod marks REMOVED");
					}
				}
				if (app.newGame) { app.newGame->setStarts(app.starts); }
			});
			// The build window's records (build_menu.md 2-3); which are listed is the sim's (UNLOCKS 36)
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("BUILDING")))
			{
				recs.push_back([gdp, r, itemName]
				{
					const kdata::GameData& gd = *gdp;
					App::BuildRec b;
					b.name = r->name;
					b.category = r->stringOr("building category", "misc");
					b.group = r->stringOr("building group");
					b.description = r->stringOr("Description");
					b.type = r->boolOr("is exterior furniture", false) ? "Exterior" : r->boolOr("is interior furniture", false) ? "Interior" : "Building shell";
					b.selectSound = buildings_click_sound(r->intOr("select sound", 0));
					b.materials = r->intOr("build materials", 0);
					b.speedMult = r->floatOr("build speed mult", 1);
					b.powerOut = r->intOr("power output", 0);
					b.powerCap = r->intOr("power capacity", 0);
					b.operators = r->intOr("max operators", 0);
					if (const kdata::RefCategory* c = r->findRefs("construction")) { for (const kdata::RefEntry& e : c->entries) { b.cost.push_back({ itemName(gd, e.targetID), e.v0 }); } }
					const kdata::RefCategory* fn = r->findRefs("functionality");
					if (const kdata::Record* f = fn && !fn->entries.empty() ? gd.find(fn->entries.front().targetID) : nullptr)
					{
						b.function = f->intOr("function", 0);
						b.outputRate = f->floatOr("output rate", 0);
						b.mining = f->intOr("world resource mining", 0);
						const kdata::RefCategory* p = f->findRefs("produces");
						if (p && !p->entries.empty()) { b.produces = itemName(gd, p->entries.front().targetID); }
					}
					app.buildRecs[r->stringID] = std::move(b);
					app.interiorSounds[r->name] = { r->intOr("interior ambience", 0), r->floatOr("interior integrity", 0), r->intOr("cavernous", 0) != 0 };
					app.sim.setDoorSound(r->stringID, r->intOr("door type", 0), r->floatOr("door move speed", 0.5f));
					app.doorSlides[r->stringID] = { r->intOr("door axis", 0), r->floatOr("door move dist", 30), r->floatOr("door comes out", 0), r->floatOr("door move speed", 0.5f) };
				});
			}
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("TOWN")))
			{
				recs.push_back([r]
				{
					app.townTypes[r->stringID] = r->intOr("type", 0);
					app.townUnexplored[r->stringID] = r->stringOr("unexplored name");
				});
			}
			for (const char* type : { "ATTACHMENT", "COLOR_DATA" })
			{
				const std::string prefix = std::string(type) + ":";
				for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type))) { recs.push_back([r, prefix] { app.creatorSids[prefix + r->name] = r->stringID; }); }
			}
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("RACE")))
			{
				recs.push_back([gdp, r]
				{
					app.creatorSids["RACE:" + r->name] = r->stringID;
					app.raceNames[r->stringID] = r->name;
					for (int g = 0; g < 2; g++)
					{
						const kdata::RefCategory* heads = r->findRefs(g ? "heads female" : "heads male");
						if (!heads) { continue; }
						for (const kdata::RefEntry& e : heads->entries) { if (gdp->find(e.targetID)) { app.raceHeads[r->name][g].push_back(e.targetID); } }	// as Characters counts them
					}
				});
			}
			// The RESEARCH page's records; the unlock labels are ours
			static const std::pair<const char*, const char*> unlockRefs[] = { { "enable buildings", "New building:" }, { "improve buildings", "Improves:" },
				{ "enable weapon type", "New weapon class:" }, { "enable weapon model", "New weapon grade:" }, { "enable armour", "New armour:" },
				{ "enable item", "New item:" }, { "enable crossbow", "New crossbow:" }, { "enable backpack", "New backpack:" }, { "enable robotics", "New robot limb:" } };
			for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("RESEARCH")))
			{
				if (r->name.starts_with("_")) { continue; }	// the start-off sets (_Default Start, ...): never researched (MINE until the static)
				recs.push_back([gdp, r, itemName]
				{
					const kdata::GameData& gd = *gdp;
					App::ResearchRec rr;
					rr.name = r->name;
					rr.category = r->stringOr("category", "misc");
					rr.description = r->stringOr("description");
					rr.level = r->intOr("level", 1);
					rr.time = r->intOr("time", 0);
					rr.blueprintOnly = r->boolOr("blueprint only", false);
					rr.levelUpgrade = r->boolOr("is level upgrade", false);
					if (const kdata::RefCategory* c = r->findRefs("cost")) { for (const kdata::RefEntry& e : c->entries) { rr.cost.push_back({ itemName(gd, e.targetID), e.v0 }); } }
					if (const kdata::RefCategory* c = r->findRefs("enable buildings")) { for (const kdata::RefEntry& e : c->entries) { rr.buildings.push_back(e.targetID); } }
					if (const kdata::RefCategory* c = r->findRefs("requirements")) { for (const kdata::RefEntry& e : c->entries) { rr.needs.push_back(e.targetID); } }
					for (const auto& [ref, label] : unlockRefs)
					{
						if (const kdata::RefCategory* c = r->findRefs(ref)) { for (const kdata::RefEntry& e : c->entries) { rr.unlocks.push_back({ label, itemName(gd, e.targetID) }); } }
					}
					app.researchRecs[r->stringID] = std::move(rr);
				});
			}
		}
		const auto t0 = std::chrono::steady_clock::now();
		while (recsNext < recs.size() && (recsNext == 0 || std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(kRecordsFrameMs))) { recs[recsNext++](); }
		if (recsNext < recs.size()) { return false; }	// the budget ran out: the rest next frame
		recs.clear();
		recsNext = 0;
		if (app.gdPending) { app.gdFreeFut = std::async(std::launch::async, [gd = std::move(app.gdPending)]() mutable { gd.reset(); }); }	// its records free in ~140 ms: a worker, never the frame (kenshi-k16.2.26)
		app.charsPhase = 2;
	}
	catch (const std::exception& e)
	{
		fprintf(stderr, "kclient: characters: game data not loaded (%s)\n", e.what());
		app.charsPhase = 2;	// failed: the step carries on without characters (no retry on a consumed future)
		return false;
	}
	return app.charsReady = true;
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
	printf("creator: %s, %zu sliders\n", what.c_str(), app.creator.ranges().size());
	app.creatorFrames = 0;
	printf("creator: weather before: %s rain %.2f wetness %.2f effects %zu\n", app.sky.weather.name.c_str(), app.sky.weather.rain, app.sky.weather.wetness, app.sky.weather.effects.size());
	if (app.creatorUi) { app.creatorUi->show(true); }
}

// A creator slider by its shown name: the editor ranges, plus Skin Tone (0..100) and Idle stance (1..N over Characters::idleStances). apply() after.
void creator_slider(const std::string& name, float v)
{
	const auto& stances = app.chars.idleStances();
	if (name == "Skin Tone") { app.creator.spec().skin = std::clamp(v / 100.0f, 0.0f, 1.0f); }
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

// Shows member i in the creator: its kept look, else (first time) a random one of its race and sex; a non-editable member (an animal) keeps the
// last preview and only its name (MINE: the original's preview of one is not captured)
static void ng_load(int i)
{
	app.ngIndex = i;
	App::NewGameMember& m = app.ngMembers[i];
	m.visited = true;
	app.creator.name = m.name;
	if (!m.editable) { return; }
	std::string err;
	const auto race = app.raceNames.find(app.sim.decoder().str(m.wire.race));
	const Characters::Spec& want = m.looked ? m.look : Characters::Spec{};
	const std::string raceName = m.looked ? want.race : race != app.raceNames.end() ? race->second : app.creator.spec().race;
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

static void ng_begin(const std::string& id);
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

static void menu_pause(bool on);
static void poll_local_server();
static void begin_session_load();
static void end_session_load();
static void update_session_load(double dt);

// The camera has a target (kenshi-k16.2.112.1): the world may stream and be drawn from here. `reason` names the target for the first_world_frame line.
static void set_camera_target(const char* reason)
{
	app.worldTarget = true;
	app.targetReason = reason;
	app.loadPending = false;
}

// The session's first camera placement (kenshi-k16.2.112.1, from the session block below): once the sim has a focus (the squad) or the session is a
// spectator's, the floating camera starts over it and the session loading screen begins. Returns true when it placed the camera this frame.
static bool place_session_camera()
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
	app.simCamera = true;
	app.centerOnStart = haveFocus && !app.opt.camera;	// an explicit --camera-* framing stays put
	begin_session_load();
	// the camera at the target now: the first world frame is drawn from it, not from the old position one frame later (k16.2.112.1)
	update_rts(0.0);
	set_camera_target(app.loadPending ? "load" : haveFocus ? "squad" : "spectator");
	if (app.scripted()) { printf("session: camera placed at %.0f,%.0f (%s)\n", app.pos[0], app.pos[2], app.targetReason); }
	return true;
}

// A --simlink run's --screenshot with no --script (kenshi-k16.2.124.1): frame 3 is black (nothing is drawn before the camera has a target), so the shot
// is pushed here instead, at the first world frame — three frames on, or after the 10 s Hub fallback. Offline, viewer and scripted runs keep frame 3.
static void screenshot_world_frame()
{
	if (!app.screenshotOnWorld) { return; }
	app.screenshotOnWorld = false;
	app.cmds.push_back({ app.frames + kScreenshotFrames, "screenshot", { app.opt.screenshot }, false });
	app.cmds.push_back({ app.frames + kScreenshotFrames, "quit", {}, false });
}

// The frame packet: renderframe.hpp (render hook 6; the draw half's own translation unit since kenshi-k16.2.205.12.27).
static FramePacket gPackets[2];

// No camera target yet (kenshi-k16.2.112.1): the sim is pumped and the start placement runs, but nothing is streamed or drawn from the camera — black,
// or the session loading screen (never a world frame: the user's "loading into deadlands should be considered a spoiler").
static void frame_await_target(FramePacket& pk, double dt)
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
static void ng_begin(const std::string& id)
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

static ui::CreatorActions creator_actions()
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
	act.resetDefaults = []() { creator_race(app.creator.spec().race, app.creator.spec().female); };
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
	act.rotate = [](float dx) { app.creator.turn(dx * 0.01f); };
	return act;
}

// Per frame: the creator screen's view of app.creator. Idle stance and skin tone first among the Body sliders (stance 1..N over
// Characters::idleStances, skin 0..100 for spec.skin 0..1).
static ui::CreatorState creator_state()
{
	ui::CreatorState st;
	if (!app.creator.active()) { return st; }
	const Characters::Spec& s = app.creator.spec();
	const Characters::RaceChoices& ch = app.creator.choices();
	st.valid = true;
	st.name = app.creator.name;
	if (!app.ngMembers.empty())
	{
		st.character = app.ngIndex;
		st.characters = (int)app.ngMembers.size();
		st.editable = app.ngMembers[app.ngIndex].editable;
	}
	for (auto [group, races] : app.chars.raceGroups())
	{
		if (!app.ngForce.empty()) { std::erase_if(races, [](const std::string& r) { return !app.ngForce.count(r); }); }	// the start's force race
		if (races.empty()) { continue; }
		const auto at = std::find(races.begin(), races.end(), s.race);
		if (at != races.end()) { st.group = (int)st.groups.size(); st.race = (int)(at - races.begin()); }
		st.groups.push_back({ group, races });
	}
	st.sex = s.female ? 1 : 0;
	st.bothSexes = !ch.ranges[0].empty() && !ch.ranges[1].empty();
	st.clothes = app.creator.clothes;
	st.description = ch.description;
	st.stats.push_back({ "Hit Points:", std::to_string((int)(ch.hp + 0.01f)) });
	for (const auto& [name, m] : ch.skills)
	{
		char value[16];
		snprintf(value, sizeof value, "%+ld%%", std::lround((m - 1) * 100));
		st.stats.push_back({ name, value });
	}
	if (ch.noBleeding) { st.stats.push_back({ "No bleeding", "" }); }	// UNSURE: its place in the panel
	st.heads = ch.heads[st.sex];
	st.head = s.head;
	auto withNone = [](const std::vector<std::string>& list, const std::string& cur, int& index)
	{
		std::vector<std::string> out{ "" };
		out.insert(out.end(), list.begin(), list.end());
		const auto at = std::find(list.begin(), list.end(), cur);
		index = at == list.end() ? 0 : 1 + (int)(at - list.begin());
		return out;
	};
	st.hairs = withNone(ch.hairs, s.hair, st.hair);
	if (!s.female) { st.beards = withNone(ch.beards, s.beard, st.beard); }
	st.hairColours = ch.hairColours;
	const auto col = std::find(ch.hairColours.begin(), ch.hairColours.end(), s.hairColour);
	st.hairColour = col == ch.hairColours.end() ? 0 : (int)(col - ch.hairColours.begin());
	const auto& stances = app.chars.idleStances();
	if (!stances.empty())
	{
		const auto at = std::find(stances.begin(), stances.end(), s.stance);
		st.sliders.push_back({ "Body", "Idle stance", 1, (float)stances.size(), at == stances.end() ? 1.0f : 1.0f + (float)(at - stances.begin()), 1 });
	}
	st.sliders.push_back({ "Body", "Skin Tone", 0, 100, s.skin * 100.0f, 0 });
	for (const Characters::EditorRange& r : app.creator.ranges())
	{
		st.sliders.push_back({ r.category, r.name, std::min(r.min, r.max), std::max(r.min, r.max), app.creator.value(r.name), 0 });
	}
	return st;
}

// The preview between the creator's columns: the editor camera (kCreatorBodyDist / kCreatorFaceDist), slid sideways so the character stands at the rect's centre
static void creator_frame()
{
	const ui::Rect r = app.creatorUi->previewRect();
	if (r.w <= 0) { return; }
	const bool body = app.creatorUi->page() == ui::kCreatorBody;
	const double dist = body ? kCreatorBodyDist : kCreatorFaceDist, pitch = body ? kCreatorBodyPitch : 0;
	const double ndc = (r.x + r.w * 0.5) / view::widthf() * 2 - 1;
	const double side = -ndc * dist * std::tan(kFovY * 0.5) * view::widthf() / view::heightf();
	if (!body) { app.creatorHead = app.chars.headHeight(app.creator.index()); }
	const double* scene = app.creatorScene;
	const double pivotY = body ? scene[1] : scene[1] - kCreatorRoot + app.creatorHead;
	app.yaw = M_PI;	// looking along +z
	app.pitch = -pitch;
	app.pos[0] = scene[0] - side;	// the camera's right is -x
	app.pos[2] = scene[2] - dist * std::cos(pitch);
	app.pos[1] = pivotY + dist * std::sin(pitch);
	const double platform[3] = { scene[0], scene[1] - kCreatorPivot, scene[2] };
	app.buildings.setEditor(true, platform);
	// The measurement (k16.17.6.1, --debug): the platform's plank top against the preview's foot bones, once the pose has settled. The platform part's
	// mesh has its plank top at the instance origin (character_editor_platform.mesh, the up-facing triangles at y 0); the original's character root is
	// 0.7 above it (findings/charedit.md: character (0,-3011.3,0), platform (0,-3012,0)), which is kCreatorPivot - kCreatorRoot here.
	if (app.opt.debug && app.creatorFrames < 31 && ++app.creatorFrames == 30)
	{
		double lf[3] = {}, rf[3] = {};
		const bool l = app.chars.boneWorld(app.creator.index(), "Bip01 L Foot", lf), r = app.chars.boneWorld(app.creator.index(), "Bip01 R Foot", rf);
		printf("creator: platform top %.3f, preview node %.3f; L foot %s%+.3f, R foot %s%+.3f over the planks\n", platform[1], scene[1] - kCreatorRoot,
			l ? "" : "none ", l ? lf[1] - platform[1] : 0.0, r ? "" : "none ", r ? rf[1] - platform[1] : 0.0);
	}
}

// specs placed from a camera at x, z looking along yawRad (the start camera, or a scene's)
void place_characters(const std::vector<std::string>& specs, double x, double z, double yr)
{
	const double fx = std::sin(yr), fz = -std::cos(yr);
	const size_t n = specs.size(), base = app.chars.size();
	double settle = 0;
	for (size_t i = 0; i < n; i++)
	{
		Characters::Spec s;
		double fwd = 30, right = ((double)i - (double)(n - 1) * 0.5) * 8.0, yaw = 0;
		std::stringstream ss(specs[i]);
		std::string kv;
		while (std::getline(ss, kv, ';'))
		{
			const size_t eq = kv.find('=');
			const std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
			if (k == "race") { s.race = v; }
			else if (k == "female" || k == "male") { s.female = k == "female"; }
			else if (k == "clip") { s.clip = v; }
			else if (k == "anim") { s.anims.push_back(v); }
			else if (k == "t") { s.time = (float)atof(v.c_str()); }
			else if (k == "speed") { s.speed = (float)atof(v.c_str()); }
			else if (k == "move") { s.loco = true; s.move = (float)atof(v.c_str()); }
			else if (k == "combat") { s.combat = true; }
			else if (k == "strafe") { s.strafe = (float)atof(v.c_str()); }
			else if (k == "wound") { const size_t c = v.rfind(':'); if (c != std::string::npos) { s.wounds.push_back({ v.substr(0, c), (float)atof(v.c_str() + c + 1) }); } }
			else if (k == "shape") { const size_t c = v.rfind(':'); if (c != std::string::npos) { s.shape.push_back({ v.substr(0, c), (float)atof(v.c_str() + c + 1) }); } }
			else if (k == "muscle") { s.muscle = (float)atof(v.c_str()); }
			else if (k == "skinny") { s.skinny = (float)atof(v.c_str()); }
			else if (k == "strong") { s.strong = (float)atof(v.c_str()); }
			else if (k == "action") { s.action = v; }
			else if (k == "action_t") { s.actionTime = (float)atof(v.c_str()); }
			else if (k == "action_speed") { s.actionSpeed = (float)atof(v.c_str()); }
			else if (k == "stealth") { s.stealth = true; }
			else if (k == "stance") { s.stance = v; }
			else if (k == "posture") { s.posture = (float)atof(v.c_str()); }
			else if (k == "shoulders") { s.shoulderSet = (float)atof(v.c_str()); }
			else if (k == "neck") { s.neckPosition = (float)atof(v.c_str()); }
			else if (k == "legs") { sscanf(v.c_str(), "%f,%f", &s.legs[0], &s.legs[1]); }
			else if (k == "head") { s.head = atoi(v.c_str()); }
			else if (k == "skin") { s.skin = (float)atof(v.c_str()); }
			else if (k == "wear") { s.wear.push_back(v); }
			else if (k == "weapon") { s.weapons.push_back(v); }
			else if (k == "drawn") { s.drawn = true; }
			else if (k == "loaded") { s.loaded = true; }
			else if (k == "carry") { s.carry = v == "left" ? 1 : v == "right" ? 2 : 0; }
			else if (k == "carried") { s.carriedBy = (int)base + atoi(v.c_str()); }
			else if (k == "hair") { s.hair = v; }
			else if (k == "beard") { s.beard = v; }
			else if (k == "haircol") { s.hairColour = v; }
			else if (k == "faction") { s.faction = v; }
			else if (k == "severed") { s.severed = atoi(v.c_str()); }	// limb bits: 1 left arm, 2 right arm, 4 left leg, 8 right leg
			else if (k == "yaw") { yaw = atof(v.c_str()); }
			else if (k == "at") { sscanf(v.c_str(), "%lf,%lf", &fwd, &right); }
			else if (k == "settle") { settle = std::max(settle, atof(v.c_str())); }
			else { fprintf(stderr, "kclient: --character: unknown key %s\n", k.c_str()); }
		}
		s.pos[0] = x + fx * fwd - fz * right;
		s.pos[2] = z + fz * fwd + fx * right;
		s.pos[1] = app.hm->height(s.pos[0], s.pos[2]);
		// the mesh faces model +Z: yaw -camera yaw turns it towards the camera
		s.yaw = (float)(-yr + yaw * M_PI / 180.0);
		std::string err;
		if (!app.chars.add(s, err)) { fprintf(stderr, "kclient: --character %s: %s\n", specs[i].c_str(), err.c_str()); }
	}
	// settle=SECONDS (any spec): step every character's animation clock that long in 1/60 s frames, so a still character's idle fit (idleFor >= 0.5 s)
	// has engaged before the shot; scripted runs and scenes otherwise freeze it (the shot stays deterministic: fixed steps)
	for (int i = 0, frames = (int)std::lround(settle * 60); i < frames; i++) { app.chars.update(1.0 / 60.0, 1.0 / 60.0); }
	app.settledChars = settle > 0;
	if (n) { printf("characters: %zu placed\n", app.chars.size()); }
}

// --blood / blood= SPEC: TYPE:FORWARD,RIGHT,SIZE[,DIRDEG[,IMAGE]] from a camera at x, z looking along yawRad. TYPE = splash | pool | smear | line (BloodDecals), SIZE as
// the game's emitters (hit splash 0.05..0.5, smear 0.2, pool drop <= 0.5), DIRDEG = the image's u (width) axis, clockwise from the camera's forward. Human blood colour.
void place_blood(const std::vector<std::string>& specs, double x, double z, double yr)
{
	const double fx = std::sin(yr), fz = -std::cos(yr);
	for (const std::string& spec : specs)
	{
		const size_t colon = spec.find(':');
		const int type = BloodDecals::typeByName(spec.substr(0, colon));
		double fwd = 0, right = 0, size = 0, deg = 0;
		int image = -1;
		if (type < 0 || colon == std::string::npos || sscanf(spec.c_str() + colon + 1, "%lf,%lf,%lf,%lf,%d", &fwd, &right, &size, &deg, &image) < 3)
		{
			fprintf(stderr, "kclient: --blood %s: want TYPE:FORWARD,RIGHT,SIZE[,DIRDEG[,IMAGE]]\n", spec.c_str());
			continue;
		}
		const float colour[4] = { 128 / 255.0f, 0, 0, 1 };	// RACE Human `blood colour` 0x800000
		const double a = yr + deg * M_PI / 180.0;
		app.blood.add((BloodDecals::Type)type, x + fx * fwd - fz * right, z + fz * fwd + fx * right, (float)size, (float)std::atan2(-std::cos(a), std::sin(a)), colour, image);
	}
	if (!specs.empty()) { printf("blood: %zu decals\n", app.blood.size()); }
}

void run_load_step()
{
	static const auto start = std::chrono::steady_clock::now();
	const auto t0 = std::chrono::steady_clock::now();
	if (!app.loadSteps[app.loadIdx].second()) { return; }	// not finished (waiting on a worker): retried next frame
	const auto t1 = std::chrono::steady_clock::now();
	fprintf(stderr, "kclient: load step %zu/%zu %-48s %7.0f ms (%.0f ms since the first step)\n", app.loadIdx + 1, app.loadSteps.size(), app.loadSteps[app.loadIdx].first,
		std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t1 - start).count());
	app.loadIdx++;
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
			unsetenv("KCLIENT_LOADING_RESIZE");	// once, not every frame
		}
	}
#endif
	pk.path = FramePath::kLoading;
}

// The --loading-shot (logic_tail on the loading path): a request for this frame's loading screen, then _exit.
static void loading_shot()
{
	if (!app.opt.loadShot.empty() && (int)app.loadIdx >= app.opt.loadShotStep) { readback::screenshot(app.opt.loadShot, [](bool) { fflush(stdout); fflush(stderr); exit_now(0); }); }	// no cleanup: the half-started subsystems would hang their destructors
}

const char* compass(double deg)
{
	static const char* names[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	return names[(int)std::floor(deg / 45.0 + 0.5) % 8];
}

double bearing(double dx, double dz)	// compass degrees of a world offset, north = -z
{
	const double d = std::atan2(dx, -dz) * 180.0 / 3.14159265358979;
	return d < 0 ? d + 360.0 : d;
}

// One line of the top-left developer text block: its key (the F12 sidecar's "hud.<key>:" line), text and colour (kenshi-k16.2.335.1)
struct HudLine { std::string key, text; float r, g, b; };

struct HudBox { float x, y, w, h; };

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
static uint64_t camera_subject()
{
	return app.follow && app.followId ? app.followId : app.rts && app.centerId ? app.centerId : 0;
}

// The move marker's walk target (user 2026-11-15, kenshi-k16.2.259: the marker and the route end at the target the sim uses, no client-side snap): the
// last waypoint of the selected characters' route that carried the last move_to click's clientSeq — where the sim walks to, an approach point when the
// click was unreachable (route flag 2) — never a point of our own. False: no such route yet (its answer may still come, or never), the click point stands.
static bool order_route_end(double out[3])
{
	if (!app.orderSeq) { return false; }
	for (const uint64_t id : app.sim.selected())
	{
		const simlink::Route* rt = app.sim.routeOf(id);
		if (rt && rt->clientSeq == app.orderSeq && !rt->points.empty())
		{
			for (int k = 0; k < 3; k++) { out[k] = rt->points.back()[k]; }
			return true;
		}
	}
	return false;
}

// Sim markers in the overlay (no depth test): a ring under each selected character, its route as a line along the ground, a fading ring at the last
// move_to click. Lines hug the terrain, except between points standing above it (a floor, a deck: MINE 2 u), which keep the sim's y (raised to the
// terrain where it would dip under); lifted a little. A segment with an end behind the camera is dropped.
// A character's tag point on screen: over the head (MINE: lift x the hull height, 0.85 = 17 u for a human; 3 u for one down), within kLabelRange and
// on screen. mvp: build_mvp(mvp, 1, 1000).
static bool head_screen(uint64_t id, const simlink::Character& ch, const float mvp[16], float& sx, float& sy, double& dist, float lift = 0.85f)
{
	double at[3];
	float hull[3];
	if (!app.sim.placed(id, at, hull)) { return false; }
	const bool down = ch.cur.flags & (simlink::kKO | simlink::kDead | simlink::kProne);
	const float p[3] = { (float)(at[0] - app.pos[0]), (float)(at[1] + (down ? 3.0 : hull[1] * lift) - app.pos[1]), (float)(at[2] - app.pos[2]) };
	dist = std::hypot(p[0], p[1], p[2]);
	if (dist > kLabelRange) { return false; }
	float c[4];
	for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
	if (c[3] < 1.0f) { return false; }
	sx = (c[0] / c[3] * 0.5f + 0.5f) * view::widthf();
	sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
	return sx >= 0 && sx <= view::widthf() && sy >= 0 && sy <= view::heightf();
}

// The UI style chosen at start (ui.style takes effect at ui::init, so a config change needs a restart): for a host-drawn overlay that has the
// game's own art as a second look, as the selection box does (kenshi-rwh.134).
bool original_style()
{
	static const bool original = []
	{
		for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.style") { return i.value == "original"; } }
		return false;
	}();
	return original;
}

// The world overlay's camera (render sitting 11, kenshi-k16.2.205.12.57): the packet's view 0 at near 1, far 1000, as build_mvp(mvp, 1, 1000) gave the
// old render-side draws. The overlay lists and record_ui's labels are projected with it in logic, so they hold the frame's pixels.
static Camera overlay_camera(const FramePacket& pk)
{
	Camera c = pk.views[0];
	c.nearZ = 1.0f;
	c.farZ = 1000.0f;
	return c;
}

// The world's labels over the scene (draw_sim_markers' last part): the name tags over the heads, the item labels, the padlock's pick chance, and with
// the UI the selection box and the debug portrait strip. With the UI they are UI draws: record_ui calls this in logic between gfx::begin and end on
// the packet's labels list (render hook 8's host half, kenshi-k16.2.205.12.39), so the font bakes they cause go with the UI's own; without it,
// draw_sim_markers calls it render side (draw_panel / draw_text). mvp: the packet's camera at 1 .. 1000 (build_mvp's).
static void world_labels(const float mvp[16])
{
	auto tag = [](float x, float y, float w, float h, const std::string& text, ui::TagState st, ui::Color ink = {})
	{
		if (app.uiOn) { ui::drawTag({ x, y, w, h }, text, st, nullptr, ink); return; }
		const float s = text_scale(), g = st == ui::TagState::kNormal ? 0.85f : 1.0f, b = st == ui::TagState::kNormal ? 0.3f : 0.6f;
		draw_panel(x, y, w, h);
		draw_text(x + (w - 8.0f * s * text.size()) * 0.5f, y + (h - 8.0f * s) * 0.5f, st == ui::TagState::kFriendly ? 0.6f : 1.0f, g, b, text.c_str());
	};
	struct Tag { float x, y, w; double dist; std::string name; bool ours; uint64_t id; };
	std::vector<Tag> tags;	// the name tags drawn
	if (kCharacterLabels.get() && labels_shown())
	{
		// over the head (head_screen), overlapping boxes pushed up, nearest placed first
		const float h = label_height();
		for (const auto& [id, ch] : app.sim.decoder().chars)
		{
			std::string name = app.sim.decoder().str(ch.look.name);
			float sx, sy;
			double dist;
			double feet[3];
			float hull[3];
			if (name.empty() || app.sim.speech().count(id) || !head_screen(id, ch, mvp, sx, sy, dist) || (app.sim.placed(id, feet, hull) && char_cut_hidden(id, feet))) { continue; }
			const float w = label_text_width(name) + kLabelPad * view::heightf() / 720.0f;
			tags.push_back({ sx - w * 0.5f, sy - h, w, dist, std::move(name), (ch.cur.flags & simlink::kPlayer) != 0, id });
		}
		std::sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.dist < b.dist; });
		for (size_t i = 0; i < tags.size(); i++)
		{
			for (bool moved = true; moved;)
			{
				moved = false;
				for (size_t k = 0; k < i; k++)
				{
					const Tag& o = tags[k];
					if (tags[i].x < o.x + o.w && o.x < tags[i].x + tags[i].w && tags[i].y < o.y + h && o.y < tags[i].y + h) { tags[i].y = o.y - h - 1; moved = true; }
				}
			}
		}
		for (const Tag& t : tags) { tag(t.x, t.y, t.w, h, t.name, t.ours ? ui::TagState::kFriendly : ui::TagState::kNormal); }
	}
	// item text: ItemStandard (picker.md); the grade inks (ItemGrade0..5 by level, ItemArtifact) wait for the item's spec; someone else's: a red frame
	// (kOwned; the original's ALT view, user 2026-11-05), hovered the stealing pair (red text)
	const ui::Color itemInk = app.uiOn ? ui::style().colour("ItemStandard", {}) : ui::Color{};
	for (const ItemLabel& l : item_labels(labels_shown(), mvp))
	{
		const bool theirs = app.sim.foreign(l.id);
		const ui::TagState state = l.id != app.hoverThing ? (theirs ? ui::TagState::kOwned : ui::TagState::kNormal) : theirs ? ui::TagState::kStealing : ui::TagState::kHover;
		tag(l.x, l.y, l.w, l.h, l.text, state, itemInk);
	}
	if (hoverCage && hoverCage == cageHover.cage && cageHover.chance >= 0 && !app.look && !ui::popupsOpen())	// the padlock's pick chance, above the cursor (kenshi-0qr shot 9)
	{
		char buf[16];
		snprintf(buf, sizeof buf, "%d%%", (int)(std::floor(cageHover.chance * 1000.0f) * 0.1f));
		const float h = label_height(), w = label_text_width(buf) + kLabelPad * view::heightf() / 720.0f;
		tag((float)app.mouseX - w * 0.5f, (float)app.mouseY - h - 26.0f * view::heightf() / 720.0f, w, h, buf, ui::TagState::kNormal, ui::Color{ 0.35f, 1.0f, 0.35f, 1.0f });
	}
#ifdef KCLIENT_DEV_TOOLS
	dev::pickmask::draw();
#endif
	if (app.uiOn && app.boxDown && app.boxDrag)	// the selection box: ours (a 1 px light outline over a faint fill), or under ui.style=original the
	{											// original's Kenshi_SelectionPanel, a thin near-black outline with no fill (kenshi-rwh.134)
		const float x0 = (float)std::min(app.boxX0, app.boxX1), y0 = (float)std::min(app.boxY0, app.boxY1);
		const ui::Rect r{ x0, y0, (float)std::max(app.boxX0, app.boxX1) - x0, (float)std::max(app.boxY0, app.boxY1) - y0 };
		const ui::Skin* sk = original_style() ? ui::res::skin("Kenshi_SelectionPanel") : nullptr;
		if (sk) { sk->draw(r, 1.0f, ui::State::kNormal, false); }	// scale 1: the original's box is fixed pixels at any resolution
		else
		{
			ui::gfx::fill(r, { 1, 1, 1, 0.08f });
			ui::gfx::frame(r, 1.0f, { 0.92f, 0.92f, 0.92f, 0.9f });
		}
	}
	if (app.uiOn && app.showPortraits) { ui::gfx::textured(app.portraitsTex, { 0, 120, 1024, 256 }, 0, 0.0625f, 0.25f, 0, { 1, 1, 1, 1 }); }
}

// The sim markers (render sitting 11, kenshi-k16.2.205.12.57): selection rings, routes, ground items and containers without a mesh, the move pin and,
// without the UI, the labels. Logic, through ovl:: into the packet's markers list (record_overlay binds it); with the UI the labels are record_ui's
// list, which world_overlay replays right after this one (renderframe.cpp), as the old draw did at its end.
static void draw_sim_markers(const FramePacket& pk)
{
	const Camera cam = overlay_camera(pk);
	const double* eye = cam.pos;	// app.pos when the packet was filled
	float mvp[16];
	cam.mvp(mvp);
	auto screen = [&](double x, double y, double z, float& sx, float& sy)
	{
		const float p[3] = { (float)(x - eye[0]), (float)(std::max(y, app.hm->height(x, z)) + 0.5 - eye[1]), (float)(z - eye[2]) };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { return false; }
		sx = (c[0] / c[3] * 0.5f + 0.5f) * view::widthf();
		sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
		return true;
	};
	auto segment = [&](const double a[3], const double b[3])
	{
		// split long segments so the line follows the terrain
		auto raised = [&](const double q[3]) { return q[1] - app.hm->height(q[0], q[2]) > 2; };
		const bool floor = raised(a) || raised(b);	// else y = -inf: the terrain everywhere
		const int n = std::clamp((int)(std::hypot(b[0] - a[0], b[2] - a[2]) / 8.0), 1, 64);
		for (int i = 0; i < n; i++)
		{
			float ax, ay, bx, by;
			const double u = (double)i / n, v = (double)(i + 1) / n;
			const double ya = floor ? a[1] + (b[1] - a[1]) * u : -1e9, yb = floor ? a[1] + (b[1] - a[1]) * v : -1e9;
			if (screen(a[0] + (b[0] - a[0]) * u, ya, a[2] + (b[2] - a[2]) * u, ax, ay) && screen(a[0] + (b[0] - a[0]) * v, yb, a[2] + (b[2] - a[2]) * v, bx, by)) { ovl::v2f(ax, ay); ovl::v2f(bx, by); }
		}
	};
	auto ring = [&](const double c[3], double r)
	{
		constexpr int kSides = 24;
		for (int i = 0; i < kSides; i++)
		{
			const double a = 6.2831853 * i / kSides, b = 6.2831853 * (i + 1) / kSides;
			const double p[3] = { c[0] + r * std::cos(a), c[1], c[2] + r * std::sin(a) }, q[3] = { c[0] + r * std::cos(b), c[1], c[2] + r * std::sin(b) };
			segment(p, q);
		}
	};
	ovl::disable_texture();
	ovl::begin_lines();
	bool approach = false;
	for (const SimView::Marker& m : app.sim.markers())
	{
		if (kSelectionRing.get()) { ovl::c4f(0.3f, 1.0f, 0.4f, 0.9f); ring(m.pos, 4.0); }
		if (!kRouteLine.get()) { continue; }
		ovl::c4f(1.0f, 1.0f, 1.0f, 0.6f);
		double at[3] = { m.pos[0], m.pos[1], m.pos[2] };
		for (const auto& p : m.route->points)
		{
			const double next[3] = { p[0], p[1], p[2] };
			segment(at, next);
			std::copy(next, next + 3, at);
		}
		if (!m.route->points.empty() && (m.route->flags & 2))
		{
			// an approach point (the click was unreachable; the server walks as close as it can): marked where the walk ends, in place of the click
			ovl::c4f(1.0f, 0.7f, 0.2f, 0.9f);
			ring(at, 2.5);
			approach = true;
		}
	}
	// ground items: their record's mesh (WorldBuildings::setProps, BuildingSet::items); a small ring for one without (MINE), and the name label (the original's DropItemLabels) while ALT is held
	// (UNSURE: whether the original shows them only then) or the item is hovered, the hovered one highlighted
	// containers: their BUILDING mesh (WorldBuildings::setProps); a square of kContainerReach for one without, red once a theft event named it
	for (const auto& [id, th] : app.sim.things())
	{
		const bool hover = id == app.hoverThing;
		if (th.kind == simlink::Interactable::kItem && !app.buildings.hasMesh(id))
		{
			ovl::c4f(1.0f, hover ? 1.0f : 0.85f, hover ? 0.6f : 0.3f, 0.9f);
			const double c[3] = { th.pos[0], th.pos[1], th.pos[2] };
			ring(c, 1.5);
		}
		else if (th.kind == simlink::Interactable::kContainer && !app.buildings.hasMesh(id))
		{
			const double r = SimView::kContainerReach * 0.7;
			if (app.sim.foreign(id)) { ovl::c4f(1.0f, 0.3f, 0.2f, 0.9f); }
			else { ovl::c4f(0.6f, hover ? 1.0f : 0.8f, 1.0f, 0.9f); }
			const double corner[4][3] = { { th.pos[0] - r, th.pos[1], th.pos[2] - r }, { th.pos[0] + r, th.pos[1], th.pos[2] - r }, { th.pos[0] + r, th.pos[1], th.pos[2] + r }, { th.pos[0] - r, th.pos[1], th.pos[2] + r } };
			for (int k = 0; k < 4; k++) { segment(corner[k], corner[(k + 1) % 4]); }
		}
	}
	ovl::count_lines();
	ovl::end();
	// the move marker (kinspect selection_marker.md): the game's position.mesh x8, upright on the click point, debug_marker's rgb(0.612, 0.694, 0.529)
	// flat (the live pixel is the constant, kenshi-rwh.5), popping in at full alpha and fading linearly over kPinFade (the live rate while playing:
	// ~1.33/s; real time here, so a paused game fades at play speed). Drawn over everything, as its x-ray pass shows the hidden part. MINE: a refusal red.
	constexpr double kPinFade = 0.75;
	if (const ogre::Mesh* pin = move_marker(); pin && app.orderAge < kPinFade)
	{
		double pinAt[3];	// the click point until the sim's route arrives, then the route's own end (k16.2.259: the marker shows the target the sim walks to)
		std::copy(app.orderAt, app.orderAt + 3, pinAt);
		order_route_end(pinAt);
		auto project = [&](const double w[3], float& sx, float& sy)
		{
			const float q[3] = { (float)(w[0] - eye[0]), (float)(w[1] - eye[1]), (float)(w[2] - eye[2]) };
			float c[4];
			for (int i = 0; i < 4; i++) { c[i] = mvp[i] * q[0] + mvp[4 + i] * q[1] + mvp[8 + i] * q[2] + mvp[12 + i]; }
			if (c[3] < 1.0f) { return false; }
			sx = (0.5f + c[0] / c[3] * 0.5f) * view::widthf();
			sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
			return true;
		};
		const float rgb[3] = { app.orderRejected ? 0.9f : 0.612f, app.orderRejected ? 0.25f : 0.694f, app.orderRejected ? 0.15f : 0.529f };
		ovl::begin_triangles();
		for (const ogre::Submesh& sub : pin->submeshes)
		{
			const std::vector<float>& pos = pin->geometry(sub).pos;
			const std::vector<uint32_t> tris = sub.triangleList();
			for (size_t i = 0; i + 2 < tris.size(); i += 3)
			{
				double w[3][3];
				float sx[3], sy[3];
				bool on = true;
				for (int v = 0; v < 3; v++)
				{
					for (int k = 0; k < 3; k++) { w[v][k] = pinAt[k] + 8.0 * pos[tris[i + v] * 3 + k]; }
					on = on && project(w[v], sx[v], sy[v]);
				}
				const double e1[3] = { w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2] }, e2[3] = { w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2] };
				const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
				const double v[3] = { w[0][0] - eye[0], w[0][1] - eye[1], w[0][2] - eye[2] };
				const double facing = (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]) / (std::hypot(n[0], n[1], n[2]) * std::hypot(v[0], v[1], v[2]) + 1e-12);
				if (!on || facing >= 0) { continue; }	// front faces only: a back face behind one would double the alpha
				ovl::c4f(rgb[0], rgb[1], rgb[2], (float)(1.0 - app.orderAge / kPinFade));
				for (int k = 0; k < 3; k++) { ovl::v2f(sx[k], sy[k]); }
			}
		}
		ovl::count_triangles();
		ovl::end();
	}
	// labels: with the UI the packet's labels list (record_ui built it; world_overlay replays it after this list); without it the debug panel and font, here
	if (!app.uiOn) { world_labels(mvp); }
}

// The F12 shot's pick report (kenshi-k16.2.234.1): who owns the cursor shape, which pick primitive was hit and which rule chose the shape. Three lines,
// "owner:", "primitive:" and "rule:", never an empty name (a thing with no record name says why). kenshi-k16.2.259 adds "skipped shape:" lines: each
// collision shape the click ray passed and why (above the floor cut, the camera inside it, faded). Report only: it re-reads the pick's inputs (the same
// rays and records pick_at uses) and never changes a decision.
static std::string pick_owner_report(const Pick& p, double mx, double my, const std::vector<dev::PickSkip>& skipped)
{
	char b[512];
	std::string owner, prim;
	const auto fmt = [&](const char* f, auto... args) { snprintf(b, sizeof b, f, args...); return std::string(b); };
	double dir[3];
	mouse_ray(mx, my, dir);
	const auto thingName = [&](const simlink::Interactable* t)
	{
		const std::string n = app.sim.nameOf(t->record, app.chars);
		return n.empty() ? fmt("no record name (record strid %u)", t->record) : n;
	};
	const auto instanceOf = [&](uint64_t id) { return app.buildings.propInstance(id); };
	static const char* const kThing[] = { "?", "door", "item", "container", "producer", "bed" };
	if (p.kind == Pick::None)
	{
		owner = "nothing (the pointer is on a letterbox bar, or no ray hit anything)";
		prim = "none";
	}
	else if (p.kind == Pick::Character)
	{
		const auto ch = app.sim.decoder().chars.find(p.id);
		std::string name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "";
		if (name.empty()) { name = "no display name (unnamed character)"; }
		double sp[3] = {}, feet[3] = {};
		float hull[3] = {};
		const bool has = app.sim.simPos(p.id, sp);
		const bool placed = app.sim.placed(p.id, feet, hull);
		owner = fmt("character \"%s\" id %llu %s at %.1f %.1f %.1f flags 0x%x", name.c_str(), (unsigned long long)p.id, app.sim.isPlayer(p.id) ? "(ours)" : "(not ours)",
			has ? sp[0] : 0.0, has ? sp[1] : 0.0, has ? sp[2] : 0.0, ch != app.sim.decoder().chars.end() ? (unsigned)ch->second.cur.flags : 0u);
		prim = placed ? fmt("sim character pick (SimView::pick: the character's hull) feet %.1f %.1f %.1f hull %.1f %.1f %.1f", feet[0], feet[1], feet[2], hull[0], hull[1], hull[2])
			: "sim character pick (SimView::pick); the character has no placed pose";
	}
	else if (const simlink::Interactable* t = p.kind != Pick::Ground ? app.sim.thing(p.id) : nullptr)
	{
		const char* kind = t->kind < 6 ? kThing[t->kind] : "?";
		owner = fmt("%s \"%s\" id %llu at %.1f %.1f %.1f state %d faction %u%s", kind, thingName(t).c_str(), (unsigned long long)p.id, t->pos[0], t->pos[1], t->pos[2], (int)t->state, t->owner,
			p.kind == Pick::Container || p.kind == Pick::Item ? (app.sim.foreign(p.id) ? " (foreign)" : " (ours / nobody's)") : "");
		if (p.kind == Pick::Bed)
		{
			const double at[3] = { t->pos[0], t->pos[1], t->pos[2] };
			const int piece = app.buildings.bedPiece(at);
			owner += fmt(" occupant %llu furniture record %u", (unsigned long long)t->occupant, t->furniture);
			prim = piece >= 0 ? "layout furniture piece, oriented mesh box (WorldBuildings::bedRay): " + app.buildings.describeInstance(piece)
				: fmt("sphere radius %.0f over the sleeping spot lifted %.0f (no drawn layout piece holds the spot)", kBedReach, kBedLift);
		}
		else if (p.kind == Pick::Door)
		{
			std::string leaf = "door leaf collision shape";
			double t2;
			float box[6];
			std::string hn;
			kworld::DoorKey key;
			const auto off = [&](const kworld::WorldShape& s, double o[3])
			{
				const auto it = app.doorPose.find(s.door);
				if (it == app.doorPose.end() || !it->second.thing || (!it->second.exact && it->second.open > 0)) { return false; }
				for (int k = 0; k < 3; k++) { o[k] = it->second.off[k]; }
				return true;
			};
			if (app.collide.ray(app.pos, dir, 20000, t2, box, off, &key, nullptr, nullptr, &hn)) { leaf = fmt("door leaf collision shape %s box %.0f %.0f %.0f - %.0f %.0f %.0f (leaf pose offset applied)", hn.c_str(), box[0], box[1], box[2], box[3], box[4], box[5]); }
			prim = leaf;
		}
		else if (p.kind == Pick::Item)
		{
			double ti;
			app.sim.pickItem(app.pos, dir, &ti);
			prim = fmt("item pick sphere (SimView::pickItem) at the item position, ray t %.1f", ti);
		}
		else
		{
			double tt = 0;
			const int inst = instanceOf(p.id);
			if (inst >= 0 && app.buildings.propRay(p.id, app.pos, dir, tt)) { prim = fmt("prop mesh box, ray t %.1f: ", tt) + app.buildings.describeInstance(inst); }
			else { prim = "no drawn prop mesh: the rock's own collision shape, or a pick label (item label under the pointer)"; }
		}
	}
	else if (p.kind == Pick::Ground && p.building && p.id)
	{
		const simlink::SimLinkDecoder::Building* pb = app.sim.building(p.id);
		const int inst = instanceOf(p.id);
		std::string name = inst >= 0 ? app.buildings.nameOfInstance(inst) : "";
		if (name.empty()) { name = pb ? app.sim.nameOf(pb->record, app.chars) : ""; }
		if (name.empty()) { name = "no record name: player building"; }
		owner = fmt("player building \"%s\" id %llu at %.1f %.1f %.1f%s", name.c_str(), (unsigned long long)p.id, p.hit[0], p.hit[1], p.hit[2], pb && (pb->flags & simlink::SimLinkDecoder::Building::kBuilt) ? " built" : " not built");
		prim = inst >= 0 ? "prop mesh box: " + app.buildings.describeInstance(inst) : "building shape";
	}
	else if (p.kind == Pick::Ground)
	{
		// the nearest drawn world shape at the hit point (the pick's own walk over the ray), else the terrain
		double from[3] = { app.pos[0], app.pos[1], app.pos[2] }, t2;
		float box[6];
		bool solid = false, bld = false, found = false;
		std::string hn;
		for (int n = 0; n < 16 && !found && app.collide.ray(from, dir, 20000, t2, box, {}, nullptr, &solid, &bld, &hn); n++)
		{
			double at[3];
			for (int k = 0; k < 3; k++) { at[k] = from[k] + dir[k] * t2; from[k] = at[k] + dir[k] * 0.5; }
			if (std::hypot(std::hypot(at[0] - p.hit[0], at[1] - p.hit[1]), at[2] - p.hit[2]) < 1.0) { found = true; }
		}
		if (found)
		{
			owner = fmt("world %s \"%s\" at %.1f %.1f %.1f (no sim id: static scenery%s)", bld ? "building shape" : "foliage / rock", hn.empty() ? "no record name: unnamed collision shape" : hn.c_str(), p.hit[0], p.hit[1], p.hit[2], bld ? ", a layout or building placement" : "");
			prim = fmt("collision shape box %.0f %.0f %.0f - %.0f %.0f %.0f%s", box[0], box[1], box[2], box[3], box[4], box[5], solid ? " (solid: stairs / gateway body)" : "");
		}
		else
		{
			owner = fmt("terrain at %.1f %.1f %.1f (no sim id: the heightmap)", p.hit[0], p.hit[1], p.hit[2]);
			prim = fmt("terrain heightmap ray hit, ground y %.1f", app.hm ? app.hm->height(p.hit[0], p.hit[2]) : 0.0);
		}
	}
	else { owner = "unknown pick (no thing with that id)"; prim = "unknown"; }
	// beds the ray also crosses that did not take the pick (hidden by the floor cut, or behind the hit): the stairs case of k16.2.233, a bed's box over the treads
	std::string also;
	for (const auto& [id, th] : app.sim.things())
	{
		if (th.kind != simlink::Interactable::kBed || (p.kind == Pick::Bed && p.id == id)) { continue; }
		const double at[3] = { th.pos[0], th.pos[1], th.pos[2] };
		double entry = 0;
		const int piece = app.buildings.bedRay(at, app.pos, dir, entry);
		if (piece != 1) { continue; }
		also += fmt("%sbed id %llu at %.1f %.1f %.1f, box entered at t %.1f: %s", also.empty() ? "" : "; ", (unsigned long long)id, th.pos[0], th.pos[1], th.pos[2], entry,
			cut_hidden(at) ? "not picked, hidden by the floor cut" : "not picked, behind the hit or nearer shape wins");
	}
	if (!also.empty()) { prim += "\nalso under the ray: " + also; }
	std::string passed;	// kenshi-k16.2.259: what the ray passed, so a click that lands somewhere odd says which shape it went through and why
	for (const dev::PickSkip& s : skipped) { passed += "\nskipped shape: \"" + s.name + "\" at " + fmt("%.1f %.1f %.1f", s.at[0], s.at[1], s.at[2]) + ": " + s.why; }
	return "owner: " + owner + "\nprimitive: " + prim + passed + "\nrule: " + (hoverRule.empty() ? "no hover decision made yet this run" : hoverRule) + " (cursor " + app.cursors.current() + ")\n";
}

#if KCLIENT_PROFILE
// The profiling overlay's Sim block (SIMLINK.md "PROFILE", kenshi-k16.2.148): the KSIM_PROFILE server's answer to order 120 (live_profile), laid out
// like the GPU line above it. Tick mean with the p50 / p99 / max of the server's window and its share of the tick budget, the per-tick counts, a
// sparkline of the recent tick means (one step per answer, the last 40) and one row per docs/PROFILING.md section, sorted by mean. A refusal
// (not_allowed / unsupported) is one line and stops the queries (SimView::profileStatus).
static void sim_profile_lines(std::vector<HudLine>& out)
{
	const simlink::SimLinkDecoder::Profile* p = app.sim.profile();
	if (!p)
	{
		const std::string& st = app.sim.profileStatus();
		if (st.empty()) { return; }	// no answer yet: the query may still be in flight
		char text[256];
		snprintf(text, sizeof text, "sim profile: %s%s", st.c_str(),
			st == "unsupported" ? " (the server was built without KSIM_PROFILE)" : st == "not_allowed" ? " (an admin session is needed)" : "");
		out.push_back({ "simprofile", text, 1.0f, 0.6f, 0.5f });
		return;
	}
	if (app.simProfileSeen != app.sim.decoder().profileSerial)	// one sparkline step per answer
	{
		app.simProfileSeen = app.sim.decoder().profileSerial;
		app.simTickHistory[app.simTickHistoryNext] = p->tickMeanUs;
		app.simTickHistoryNext = (app.simTickHistoryNext + 1) % app.simTickHistory.size();
		app.simTickHistoryN = std::min(app.simTickHistoryN + 1, app.simTickHistory.size());
	}
	char line[256];
	const float budget = p->tickBudgetUs > 0 ? p->tickBudgetUs : 50000.0f;
	snprintf(line, sizeof line, "sim tick %.2f ms mean, p50 %.2f, p99 %.2f, max %.2f  (%.0f%% of %.0f)  window %u", p->tickMeanUs / 1000.0f,
		p->tickP50Us / 1000.0f, p->tickP99Us / 1000.0f, p->tickMaxUs / 1000.0f, 100.0f * p->tickMeanUs / budget, budget / 1000.0f, p->window);
	out.push_back({ "simprofile", line, 0.7f, 1.0f, 1.0f });
	snprintf(line, sizeof line, "sim counts  characters %.1f  squads %.1f  path queries %.1f/tick (%.1f us)", p->characters, p->squads, p->pathQueries, p->pathUs);
	out.push_back({ "simprofile", line, 0.7f, 1.0f, 1.0f });
	// The tick means, oldest first, scaled to their own maximum so a fast server still shows a shape; a step per answer (about 1 s).
	float peak = 1e-3f;
	for (size_t i = 0; i < app.simTickHistoryN; i++) { peak = std::max(peak, app.simTickHistory[(app.simTickHistoryNext + app.simTickHistory.size() - app.simTickHistoryN + i) % app.simTickHistory.size()]); }
	static const char* const kLevels = " .:-=+*#%@";	// 10 steps, sdtx's ASCII only
	std::string spark = "sim spark  ";
	for (size_t i = 0; i < app.simTickHistoryN; i++)
	{
		const float v = app.simTickHistory[(app.simTickHistoryNext + app.simTickHistory.size() - app.simTickHistoryN + i) % app.simTickHistory.size()];
		spark += kLevels[std::clamp((int)(v / peak * 9.99f), 0, 9)];
	}
	snprintf(line, sizeof line, "  (max %.2f ms, %zu answers)", peak / 1000.0f, app.simTickHistoryN);
	spark += line;
	out.push_back({ "simprofile", spark.c_str(), 0.7f, 1.0f, 0.7f });
	out.push_back({ "simprofile", "sim sections (us/tick, sorted by mean)", 0.7f, 1.0f, 0.7f });
	std::vector<const simlink::SimLinkDecoder::Profile::Row*> rows;
	rows.reserve(p->rows.size());
	for (const auto& r : p->rows) { rows.push_back(&r); }
	std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->meanUs > b->meanUs; });
	for (const auto* r : rows)
	{
		snprintf(line, sizeof line, "  %-12s %8.1f us   max %8.1f", r->name.c_str(), r->meanUs, r->maxUs);
		out.push_back({ "simprofile", line, 0.8f, 0.9f, 0.9f });
	}
}
#endif

// The developer text block's lines (PLAYER_GUIDE 6.10), top to bottom; draw_hud draws them and the F12 sidecar writes each as "hud.<key>: text", so the
// two cannot drift (kenshi-k16.2.335.1). Built every --debug frame even while the block is hidden: the sim profile's sparkline steps per answer.
static std::vector<HudLine> hud_lines()
{
	std::vector<HudLine> out;
	const double ground = app.hm->height(app.pos[0], app.pos[2]);
	const kworld::ZoneCell zone = kworld::zoneCellOf(app.pos[0], app.pos[2]);
	double heading = std::fmod(app.yaw * 180.0 / 3.14159265358979, 360.0);
	if (heading < 0) { heading += 360.0; }
	char line[256];
	snprintf(line, sizeof line, "x %.0f  z %.0f  ground %.0f  alt %.0f  zone %d.%d  %.0f fps", app.pos[0], app.pos[2], ground, app.pos[1] - ground, zone.x, zone.z, app.fps);
	out.push_back({ "pos", line, 1.0f, 1.0f, 1.0f });
	// the exact camera, so another session can frame the same shot (kinspect knavshot: eye, pivot, dist, yaw, pitch; fovy 45) and the tracked character
	int at = snprintf(line, sizeof line, "heading %s %.1f  pitch %.1f  eye %.1f %.1f %.1f", compass(heading), heading, app.pitch * 180 / 3.14159265358979, app.pos[0], app.pos[1], app.pos[2]);
	if (app.rts) { at += snprintf(line + at, sizeof line - at, "  pivot %.1f %.1f %.1f dist %.1f on %s", app.focus[0], app.focus[1], app.focus[2], app.rtsDist, app.focusOn); }
	double feet[3];
	float hull[3];
	if (const uint64_t id = camera_subject() ? camera_subject() : app.sim.selected().empty() ? 0 : app.sim.selected().front(); id && app.sim.placed(id, feet, hull))
	{
		snprintf(line + at, sizeof line - at, "  char %.1f %.1f %.1f", feet[0], feet[1], feet[2]);
	}
	out.push_back({ "camera", line, 1.0f, 1.0f, 1.0f });
	const int n = app.towns.nearest(app.pos[0], app.pos[2]);
	if (n >= 0)
	{
		const Town& t = app.towns.towns[n];
		const double dx = t.x - app.pos[0], dz = t.z - app.pos[2], b = bearing(dx, dz);
		snprintf(line, sizeof line, "nearest: %s  approx %.1f km (10 u/m) %s %.0f deg", t.name.c_str(), std::hypot(dx, dz) / 10000.0, compass(b), b);
	}
	else { snprintf(line, sizeof line, "nearest: (towns %s)", app.townsReady ? "unavailable" : "loading"); }
	out.push_back({ "nearest", line, 0.8f, 1.0f, 0.8f });
	const WorldBuildings::Stats bs = app.buildings.stats();
	snprintf(line, sizeof line, "buildings %zu drawn, features %zu, %zu draws, %zu meshes, %zu loading  %.1f ms", bs.instancesDrawn - bs.featuresDrawn, bs.featuresDrawn, bs.groups, bs.meshesReady, bs.pending, app.frameMs);
	out.push_back({ "buildings", line, 0.9f, 0.9f, 0.7f });
	const GameClock& gc = app.sky.clock;
	const int minutes = (int)(gc.hours * 60.0);
	const double* sd = app.sky.sunDir();
	// cpu at the start of this line, not the end of the long foliage line: the PAUSED banner (top centre) covered that end, and the end of this one (kenshi-k16.2.205.1)
	char cpuText[64];
	if (logicthread::running()) { snprintf(cpuText, sizeof cpuText, "cpu %.1f ms (logic %.1f render %.1f)  ", framestats::stats().cpu.ms, app.logicMs, framestats::stats().renderMs); }	// two threads (RENDER_THREAD.md 7.1 rule 11)
	else if (app.foliageOn) { snprintf(cpuText, sizeof cpuText, "cpu %.1f ms (cull %.1f)  ", framestats::stats().cpu.ms, app.foliage.stats().cullMs); }
	else { snprintf(cpuText, sizeof cpuText, "cpu %.1f ms  ", framestats::stats().cpu.ms); }
	snprintf(line, sizeof line, "%stime %02d:%02d  day %d  %s  sun altitude %.0f deg", cpuText, minutes / 60, minutes % 60, gc.day + 1,
		gc.paused ? "paused" : gc.speed == 1 ? "1x" : (std::to_string((int)gc.speed) + "x").c_str(), std::asin(sd[1]) * 180.0 / 3.14159265358979);
	out.push_back({ "time", line, 1.0f, 0.95f, 0.6f });
	if (app.foliageOn)
	{
		const Foliage::Stats fs = app.foliage.stats();
		snprintf(line, sizeof line, "foliage x%.3g %zu drawn of %zu, %zu draws, %zu cells (%zu loading)", app.foliage.foliageRange, fs.instancesDrawn, fs.instancesLoaded, fs.draws, fs.cellsLoaded, fs.cellsPending);
		out.push_back({ "foliage", line, 0.7f, 1.0f, 0.7f });
	}
	if (app.opt.debug && !app.squads.dots().empty())	// the squad overlay is a debug view (--debug)
	{
		snprintf(line, sizeof line, app.squads.on ? "squads %zu shown  %s" : "squads %zu (%s to show)", app.squads.dots().size(), ui::keys::label("debug.squad_overlay").c_str());
		out.push_back({ "squads", line, 1.0f, 0.8f, 0.5f });
	}
	const std::string helpHint = app.opt.debug ? "  " + ui::keys::label("debug.help") + " help" : "";
	snprintf(line, sizeof line, "weather %s  scene %s  shadows %s (%.1f ms)%s", app.sky.regionalFog ? ("auto: " + app.weatherMix).c_str() : app.sky.weather.name.c_str(), app.sceneName.empty() ? "-" : app.sceneName.c_str(), app.shadow.active() ? "on" : "off", framestats::stats().shadowMs, helpHint.c_str());
	out.push_back({ "weather", line, 0.8f, 0.9f, 1.0f });
#if KCLIENT_PROFILE
	if (framestats::stats().gpuOk)
	{
		char gpuLine[512];	// 19 sections (kenshi-k16.2.101.1) no longer fit the 256-byte overlay line
		int g = snprintf(gpuLine, sizeof gpuLine, "gpu %.1f ms:", framestats::stats().gpuTotal);
		for (int s = 0; s < GpuTimer::kSections && g < (int)sizeof gpuLine; s++)
		{
			g += snprintf(gpuLine + g, sizeof gpuLine - g, " %s %.1f", GpuTimer::name(s), framestats::stats().gpuMs[s]);
			if (g >= (int)sizeof gpuLine) { g = (int)sizeof gpuLine - 1; }
		}
		out.push_back({ "gpu", gpuLine, 0.7f, 0.9f, 1.0f });
	}
#endif
	if (const WorldBuildings::FloorStatus fl = app.buildings.floorStatus(); fl.valid)
	{
		const int m = snprintf(line, sizeof line, "building %s  floor %d/%d  R/F", fl.name.c_str(), fl.shown, fl.top);	// the ground floor is 0, as the original counts (user 2026-11-04)
		if (fl.following && fl.leadFloor >= 0) { snprintf(line + m, sizeof line - m, "  lead on floor %d", fl.leadFloor); }
		out.push_back({ "building", line, 1.0f, 0.85f, 0.6f });
	}
	if (app.sim.active()) { out.push_back({ "sim", app.sim.status(), 0.6f, 1.0f, 0.9f }); }
#if KCLIENT_PROFILE
	sim_profile_lines(out);
#endif
	return out;
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
std::string title_status()
{
	if (app.sim.active()) { return {}; }
	if (!app.serverErr.empty()) { return "The game server did not start: " + app.serverErr + "; run client/tools/play.sh game"; }
	if (app.local && app.server.running()) { return "Starting the game server..."; }
	if (!app.opt.simlink.empty()) { return "No game server; run client/tools/play.sh game"; }
	return {};
}

// A local start in flight: the only state a held click waits for (kenshi-k16.2.210.6). A failure line or a dropped join never clears by itself.
static bool title_starting() { return !app.sim.active() && app.serverErr.empty() && app.local && app.server.running(); }

static const char* const kMenuButtonNames[4] = { "CONTINUE", "NEW GAME", "LOAD GAME", "IMPORT GAME" };

// A title action that needs the game server (kenshi-k16.2.210.4, user 2026-11-14: "The main menu lets you hit 'new game' before the engine is ready, and it
// just eats the input"): while title_status() says it cannot start yet, the click is not run but held, one at a time (the newest wins), and the title draws
// its button pushed (MainMenu::setPending). false: nothing to hold, run it now. EXIT and OPTIONS never come here: they need no server.
static bool hold_menu_action(int button, const std::function<void()>& run)
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
	const std::string status = title_status();
	app.mainMenu->setOffline(status);	// may grey the buttons: on screen it is drawn after this
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
static void live_squads()
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
static void live_profile()
{
#if KCLIENT_PROFILE
	if (!app.opt.debug || app.hideUi || title_shown() || (app.creatorUi && app.creatorUi->shown()) || !app.sim.active()) { return; }
	app.sim.profileQuery();
#endif
}


// The stealth arrows' marks (kenshi-h2v.27.2; the original's geometry, kinspect findings/stealth_detection/rwh63_arrow_live.md section 1): for every
// DETECTED_BY (60) entry, on the line from the sneaker's drawn position to the observer's, at clamp(dist - 7, 0, 15) from the sneaker plus 12 u
// world Y, aimed along the full 3D direction. Colour by the entry (section 2): friendly 0x00ff00, state 1 (noticed) 0xdd0000, state 2 0xffdd00.
static std::vector<Arrows::Mark> stealth_arrow_marks()
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
static void resize_burst_frame()
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
	if (const int cap = kFpsCap.get())	// the frame limiter: sleep to the next slot; more than a frame late restarts the schedule
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
	if (!app.firstWorldFrame) { app.firstWorldFrame = true; printf("first_world_frame: x=%.0f z=%.0f reason=%s\n", app.pos[0], app.pos[2], app.targetReason); screenshot_world_frame(); }
	camera_restore_frame();	// a load's camera sidecar, at its first snapshot (k16.2.134.1)
	if (app.follow) { update_follow(dt); }
	else if (app.rts) { update_rts(dt); }
	else { update_camera(dt); }
	app.sim.setDriven(app.follow && app.sim.isPlayer(app.followId) ? app.followId : 0);	// direct drive: the drawn body's travel facing (kenshi-k16.2.237.10)
	CPUSTAGE("camera");
	if (app.sim.active())
	{
		app.sim.hold(app.creator.active());
		app.sim.update(dt, app.chars, [](double x, double z) { return app.hm->height(x, z); });
		CPUSTAGE("sim.update");
		app.nav.update(app.sim.decoder(), app.sim.takeNavPatches());
		app.sim.setWalker(app.nav.walker());
		live_squads();
		live_profile();
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
				uint64_t clickTick = 0, arrived = 0, waypoints = 0;
				uint8_t flags = 0;
				const std::vector<uint64_t> sel = app.sim.selected();
				for (const uint64_t id : sel)
				{
					for (const SimView::MoveOrder& o : app.sim.moveOrders(id)) { if (o.seq == app.orderSeq) { clickTick = o.tick; } }
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
		app.collide.update(app.pos[0], app.pos[2]);
		CPUSTAGE("pk.pre");	// sub-stages of sim.peek+hover (kenshi-k16.2.205.12.109): the splashes .. collide update, the peek, the hover pick
		update_peek(dt);
		CPUSTAGE("pk.peek");
		hover_cursor();
		CPUSTAGE("pk.hover");
#ifdef KCLIENT_DEV_TOOLS
		pick_mask_update();
#endif
		place_session_camera();
		CPUSTAGE("sim.peek+hover");
	}
	update_ui(dt);
	CPUSTAGE("update_ui");
	const double charDt = app.scripted() && !app.scriptAnim ? 0.0 : app.sim.active() ? app.sim.dtSim() : dt;	// on the sim's clock: still while paused (user 2026-11-05), faster at 2x / 3x
	if (app.sim.active()) { app.ragdolls.update(app.sim, app.chars, app.hm ? &*app.hm : nullptr, app.pos); }
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
	dev::gaitFrame(app.sim, app.chars, app.pos, charDt);
	dev::posFrame(app.sim, app.chars, app.pos, app.sim.active() ? app.sim.dtSim() : 0.0);
	CPUSTAGE("sounds+trails");
	// The frame's second half starts here (overlap review .85 section 1.1, part B): the writes to what the draw reads come after this line.
	// Overlap (rule 12): the gate; the render thread is outside render_frame from here until this packet is filled. Off / lockstep: free.
	stall_point(0);
	logicthread::renderIdle();
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
	const bool editor = app.creatorUi->shown();
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
			app.sky.clock.day = std::max(0, (int)c.day - 1);	// CLOCK day is the game's "Day N" (ksim/clock.hpp); the sky clock counts from 0 like --day
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
	pk.buildings.inst.clear();	// every field every frame (§7.1 rule 2): buildings.update below refills it when it runs
	pk.buildings.shadowSig = 0;
	if (app.bldSet && !app.wmap.full)
	{
		// The props list (kenshi-k16.2.205.11): dirty-tracked. The sim sends an interactable on first sight and when its version changes
		// (SimLinkDecoder::thingsSerial), so the list changes on that, on the player buildings' records, on a floor cut (cut_hidden) and on the debug
		// script's build progress only; every other frame reuses the list and skips WorldBuildings::setProps. propsLive: an under-construction player
		// building's progress moves every tick, so the list is rebuilt then.
		const uint32_t thingsSerial = app.sim.thingsSerial();
		const uint32_t buildSerial = app.sim.decoder().buildingsSerial;	// the player buildings' records (BUILDING 47)
		const uint64_t cutSig = app.buildings.cutSig();
		const bool dirty = !app.propsValid || thingsSerial != app.propsSerial || buildSerial != app.propsBuildSerial || cutSig != app.propsCut
			|| app.propsLive || app.scriptBuild >= 0;
		if (dirty && !ablate::on("noprops"))	// KCLIENT_ABL: noprops keeps the list empty (an upper-bound measurement)
		{
			std::vector<WorldBuildings::Prop>& props = app.props;
			props.clear();
			for (const auto& [id, th] : app.sim.things())
			{
				const double at[3] = { th.pos[0], th.pos[1], th.pos[2] };
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
			app.propsLive = live;
			app.buildings.setProps(props);
		}
		update_doors(dt);
		CPUSTAGE("props");
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
		const bool followCone = app.follow && app.buildings.hasSeen;
		if (app.buildings.hasSeen && (followCone || !kOccluderGhost.get()))
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
		app.buildings.windSpeed = app.windForce < 0 && app.sky.weather.windValid ? app.sky.weather.windSpeed : -1.0f;	// a scene's wind=DEG has no speed
		// its mesh and texture creates and evicts are render queue jobs, its instances a packet upload (kenshi-k16.2.205.12.35)
		app.buildings.update(app.pos, mvp, app.opt.fog, app.scripted() && !kStreamScripted.get(), sim_time(), app.shadow.active() ? Shadow::kRadius[Shadow::kCascades - 1] : 0.0, pk.buildings);
		CPUSTAGE("buildings.update");
		if (app.foliageOn)
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
		// The placed flames: ParticleWorld diffs the visible effect instances, fades systems in and out, and hands them to the same simulation.
		static const std::vector<WorldBuildings::PlacedEffectRt> none;
		app.particleWorld.update(pf.dt, app.bldSet ? app.buildings.placedEffects() : none, app.buildings, app.buildings.night);
		// The sim's beams (BEAMS): copy a new frame, then draw the ones at the render tick. Their scripts are kept off the local weather placement
		// below, so each beam is drawn once and at the sim's own position (the wind-up is the lead the sim sent it with, SIMLINK "BEAMS").
		if (app.sim.active() && app.sim.decoder().beamsSerial != app.beamsSeen)
		{
			app.beamsSeen = app.sim.decoder().beamsSerial;
			app.beams.sync(app.sim.decoder());
		}
		const std::vector<std::string> beamScripts = app.beams.scripts();
		std::vector<Particles::PlacedSystem> placed = app.particleWorld.systems();
		for (Particles::PlacedSystem& s : app.weatherInst.systems(beamScripts)) { placed.push_back(std::move(s)); }
		if (app.sim.active() && app.beamsOn) { app.beams.systems(app.sim.tick(), app.hm ? &*app.hm : nullptr, placed); }
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
		app.particles.setPlaced(placed);
		app.particlesDrawn = app.particles.prepare(pf, pk.particles);
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

static void logic_frame(FramePacket& pk)
{
	dev::packetPoison(pk);	// dev builds, KCLIENT_PACKET_POISON: wipe the slot so a field this frame does not write shows (dev/packetdigest.hpp)
	pk.serial = rq::beginPacket();	// first: every resource job queued below (the paths and the tail) belongs to this packet (docs/RENDER_THREAD.md §7.2)
	rq::setStreamTimeBudget(kStreamMs.get());
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
		pk.lightOff = dev::lightOffMask();	// the light debugger (dev/lightterms.hpp): the logic's switches, read by the draw only from the packet
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
		draw_navmesh(*app.nav.mesh(), mvp, cam.pos, app.rts ? app.focus : app.pos, view::widthf(), view::heightf(), bands);
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
	// The corner minimap and the top-left command-line text are development views: --debug only (client/CLAUDE.md). The full map stays: it is
	// the fallback map of a run without the UI layer (toggle_map) and the debug key's view.
	if (app.wmap.full) { app.wmap.drawFull(app.pos[0], app.pos[2], app.yaw, towns, &app.squads, ui::keys::label(app.uiOn ? "debug.full_map" : "toggle_map")); }
	else if (app.opt.debug) { app.wmap.drawMinimap(app.pos[0], app.pos[2], app.yaw, towns, &app.squads); }
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

// The RenderState render_frame draws with (renderframe.hpp): references to the render-owned objects App still holds, and the legacy blocks.
static RenderState& render_state()
{
	static RenderState rs{ app.gpu, app.hdr, app.particles, app.sky, app.fogvol, app.post, app.buildings, app.terrain, app.foliage, app.grass, app.water, app.chars, app.squads, app.arrows, app.idbuf, app.shadow, app.sunShafts, app.dust, app.lightVolumes, {}, app.refl, app.portraits, app.creatorScene3d, app.loadScreen, app.cursors, app.trails, app.sglShown, false, false };
	return rs;
}


// render.thread=lockstep (logicthread.hpp; RENDER_THREAD.md 7.1 rules 7-9, kenshi-k16.2.205.12.25): logic_frame runs on its own thread and the two
// alternate. The logic ms / render ms split of the world frame's CPU (rule 11; no CPU span crosses the threads): logicMs is the packet's
// (pk.logicMs, the world path's start to logic_frame's end), renderMs over render_frame; FrameStats::cpu is their sum up to the overlay's end
// (the hand-off wait not included, kenshi-k16.2.205.12.27).
struct IdleSeen { long polls = 0; double tick0 = 0, tick1 = 0; uint64_t snap0 = 0, snap1 = 0; };	// what the idle polls saw (logic thread only)
static IdleSeen gIdleSeen;
static void start_logic_thread()
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

// Rule 4's "about one refresh" (kenshi-k16.2.205.12.87, review .85 section 1.8): the display's refresh period (RandR, published with the window
// snapshot; 16.7 ms when unknown), or 1000 / gfx.fps_cap under a cap with vsync off (with vsync on: the longer of the two), clamped to [4, 20] ms. Never sapp's measured interval:
// with vsync off that is the callback interval, which the no-pass callbacks shorten themselves, so the bound fell below logic's frame.
static double lockstep_bound_ms()
{
	const int cap = kFpsCap.get();
	const bool vsync = sapp_query_desc().swap_interval > 0;	// the swap interval the window was made with (gfx.vsync applies after a restart)
	const double refresh = view::refreshPeriodLive();
	const double period = refresh > 0 ? refresh * 1000.0 : 1000.0 / 60.0;
	const double ms = cap <= 0 ? period : vsync ? std::max(period, 1000.0 / cap) : 1000.0 / cap;	// vsync and a cap under the refresh: the cap's step
	return std::clamp(ms, 4.0, 20.0);
}

// The render thread's frame once the logic thread runs: wait about one refresh for a packet (lockstep_bound_ms), draw it and keep the composed
// frame (lastframe.hpp); no packet in time: no pass, the kept frame is shown again (in lockstep a redraw would read app. while logic writes it).
static void frame_lockstep()
{
#ifdef KCLIENT_DEV_TOOLS
	logicthread::renderStallPoint();	// the script's `renderstall MS`: a window held in its swap
#endif
	const double bound = lockstep_bound_ms();
	if (logicthread::renderFrame(bound)) { lastframe::keep(); return; }
	lastframe::show();
#ifdef KCLIENT_DEV_TOOLS
	lastframe::stallShot();
#endif
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
	if (logicthread::running()) { frame_lockstep(); return; }
	FramePacket& pk = gPackets[0];	// one thread: slot 0 only
	logic_frame(pk);
	CPUSTAGE_RENDER_BEGIN();
#ifdef KCLIENT_DEV_TOOLS
	if (pk.path == FramePath::kWorld) { threadsplit::split(pk.logicStart, pk.logicMs, false); }
#endif
	render_frame(pk, render_state());
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
static ui::ItemWindowsActions item_actions();
static ui::BuildingWindowActions machine_actions();
static ui::SquadsActions squads_actions();
static ui::ResearchActions research_actions();
static void prospect_order();
// A debug toggle by its keys:: action name: what the key does (the key handler and the debug panel both call it).
static void debug_toggle(const std::string& id)
{
	if (id == "debug.navmesh") { kNavOverlay.set(!kNavOverlay.get()); }
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
}

// The debug panel's state, every frame while it shows.
#ifdef KCLIENT_DEBUG_VIEW
static double steady_seconds() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Asks the server for one squad's (kind 0) or character's (1) tree and shows the window; its answer fills it (debug_tree_frame)
static void debug_tree_open(uint8_t kind, uint64_t id)
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

static void debug_tree_frame()
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

// The UI sounds (kenshi-inspect findings/audio_load_pause.md): a load or new game request posts Create_or_Load_Game, a speed button posts its
// Game_Speed_* event, and a flip of the clock's pause bit posts Pause_Game / Resume_Game (update_ui). The audio thread runs the events from
// SFX_General_Bank / MT_Music_Bank and logs them. The third speed is 5x (ui::Hud::kSpeeds, as the original); its event is named x4.
static const char* speed_event(float speed)
{
	return speed <= 1.0f ? "Game_Speed_Normal" : speed <= 2.0f ? "Game_Speed_x2" : "Game_Speed_x4";
}

// A window action (its key or the HUD's shortcut buttons). toggle_help is the client's key list until a help screen exists; the stats and research
// windows do not exist yet.
static void shortcut(const std::string& action)
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
static void menu_pause(bool on)
{
	// MINE: a menu's own pause and unpause are silent (the original's title has no running game; whether its escape menu posts Pause_Game is
	// not traced); the player's pause key and the HUD keep their sounds
	if (on && !app.menuPaused && app.sim.active() && (!(app.sim.clock().pauseBits & 1) || app.menuUnpaused)) { app.sim.pause(true); app.menuPaused = true; app.quietPauseTo = 1; }
	else if (!on && app.menuPaused) { app.sim.pause(false); app.menuPaused = false; app.menuUnpaused = true; app.quietPauseTo = 0; return; }
	app.menuUnpaused = false;
}

// The session loading screen (docs/CLIENT_GOALS.md "Session front end": no pop-in): from the camera's first placement on the squad or a reset (a load, a
// new game) until the world round the camera has streamed in. The game is paused under it (MINE: the original's sim does not run while it loads).
static void begin_session_load()
{
	const int mode = kSessionLoadScreen.get();
	if (mode == 0 || (mode == 1 && app.scripted())) { return; }
	logicthread::renderIdle();	// overlap (rule 12): the whole session load runs gated (logic_paths' frame-start gate from the next frame on)
	if (!app.sessionLoad) { app.loadScreen.init(app.opt.data, app.uiOn ? dl::gpu::view(ui::gfx::atlas()) : sg_view{}); }
	app.sessionLoad = true;
	app.sessionQuiet = app.sessionPeak = 0;
	app.sessionTime = 0;
	app.sessionFrameMax = 0;
	app.framePhase = "session load";	// the work this frame does next is the load's (the next frame's gap names it)
	printf("session: loading the world\n");
}

// Done after kQuietFrames frames with nothing streaming and the squad in view; a cap so a stuck worker cannot keep the game hidden, said loudly.
static void update_session_load(double dt)
{
	static constexpr int kQuietFrames = 10;
	static constexpr double kMaxSeconds = 120;
	const bool newGame = !app.newGameStart.empty() || app.newGameSent || app.newGameLeader;
	const bool creator = app.creatorUi->shown();	// the creator's scene is what the screen waits for, not a squad (kenshi-k16.2.133.1)
	if (!newGame && !creator && !app.sessionPaused && app.sim.active() && !(app.sim.clock().pauseBits & 1)) { app.sim.pause(true); app.sessionPaused = true; }	// not before: NEW_GAME applies on a later tick
	const bool busy = world_loading(&app.sessionPending) || (!app.simCamera && !creator) || newGame;
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
	if (app.sessionPaused) { app.sim.pause(false); app.sessionPaused = false; }
	fprintf(stderr, "kclient: session load: worst frame %.0f ms over the loading screen\n", app.sessionFrameMax);
}

// Single player (docs/CLIENT_GOALS.md "Session front end"): the title starts the server; frame() connects once it listens (poll_local_server).
static void start_local_server()
{
	app.serverErr.clear();	// a fresh start: the title's offline line goes with the previous failure (kenshi-k16.3.12)
	std::string exe = kServerExe.get();
	if (exe.empty())
	{
		std::error_code ec;
		exe = (std::filesystem::read_symlink("/proc/self/exe", ec).parent_path() / "cpp/OpenKenshi-server").string();	// cpp/build has the pre-move data path (TESTING)
	}
	std::vector<std::string> args;
	std::istringstream words(kServerArgs.get());
	for (std::string w; words >> w;) { args.push_back(w); }
	// The data dir the client resolved, so the server needs no built-in path (kenshi-379.13: the release binary carries none).
	args.push_back("--data");
	args.push_back(app.opt.data);
	serverrows::append_flags(args);
	std::string err;
	if (!app.server.start(exe, args, err))
	{
		fprintf(stderr, "kclient: local server: %s\n", err.c_str());
		ui::message("The game server did not start: " + err);
		app.serverErr = err;
		return;
	}
	app.local = true;
}

static void poll_local_server()
{
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
		ui::message("The game server stopped: " + app.server.exited());
		app.serverErr = app.server.exited().empty() ? "the server stopped" : "the server stopped: " + app.server.exited();
		app.local = false;
	}
}

// The sim link is up: the click layer and the cursors start with it. Once the server's HELLO is in (no order may go before it), update_ui asks for
// the saves (CONTINUE).
void sim_opened()
{
	logicthread::renderIdle();	// overlap (rule 12): the new game's link, collision and the buildings' callbacks change under no draw
	app.collide.start(app.opt.data, &*app.hm);
#ifdef KCLIENT_DEBUG_VIEW
	app.debugView.reset();
	app.debugSubscribed = false;
	dev::shotTreesReset();	// no shot's trees capture is pending at a session's start
	app.sim.insideInterior = [](const double p[3]) { return app.buildings.insideInterior(p); };	// walkers in a carved interior draw at the sim's y (kenshi-kyat.2)
	app.sim.setDebugSink([](uint16_t type, const uint8_t* p, size_t n)
	{
		const uint32_t before = app.debugView.treeSerial;
		app.debugView.frame(type, p, n);
		if (app.debugView.treeSerial != before) { dev::shotTreesTree(dev_context(), app.debugView.tree); }	// one answer: the pending capture's, if it asked
	});
#endif
	app.buildings.lineHits = [](const double from[3], const double to[3], uint32_t building)	// a building shape inside that building's box, along the segment
	{
		double dir[3], p[3] = { from[0], from[1], from[2] }, t;
		const double len = std::sqrt((to[0] - from[0]) * (to[0] - from[0]) + (to[1] - from[1]) * (to[1] - from[1]) + (to[2] - from[2]) * (to[2] - from[2]));
		for (int k = 0; k < 3; k++) { dir[k] = (to[k] - from[k]) / len; }
		bool isBuilding = false;
		float box[6];
		for (double done = 0; done < len && app.collide.ray(p, dir, len - done, t, box, {}, nullptr, nullptr, &isBuilding); done += t + 0.5)
		{
			for (int k = 0; k < 3; k++) { p[k] += dir[k] * t; }
			// the follow camera passes thin props (the boom's sweep does), so one in the line does not ghost its whole building: the cone dithers it
			if (isBuilding && !(app.follow && CollisionPick::thin(box)) && app.buildings.holds((int)building, p, false)) { return true; }
			for (int k = 0; k < 3; k++) { p[k] += dir[k] * 0.5; }
		}
		return false;
	};
	app.cursors.init(app.opt.data, app.uiOn ? std::function<sg_view()>([] { return dl::gpu::view(ui::gfx::atlas()); }) : std::function<sg_view()>(), (int)ui::gfx::kAtlasW, (int)ui::gfx::kAtlasH);	// the callback runs in the render-side job: the atlas TexId resolves there (.38)
	app.greeted = false;
	app.progressLevelSent = -1;	// a new connection: send ui.progress_bars' level again at its HELLO
	// kenshi-k16.2.112.1: a session with no explicit position starts with no camera target — nothing streams or draws until the squad, a new game,
	// a scenario or a load places the camera (the camera sits at the Hub default meanwhile, never the world origin). Once a world has been drawn,
	// a later session (EXIT GAME, a new title game) keeps its camera: that is a place in the game, not a spoiler.
	if (!app.opt.explicitPos && !app.firstWorldFrame)
	{
		app.worldTarget = false;
		app.targetReason = "";
		app.targetWait = 0;
		app.loadPending = false;
	}
}

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
		// the light debugger (kenshi-jj3.27.3): its actions are dev/lightterms.hpp's, the same as the 'light' script op
		ui::LightsPanelActions lpa;
		lpa.set = [](const std::string& name, bool on) { dev::setLight(name, on); };
		lpa.solo = [](const std::string& name) { dev::soloLight(name); };
		lpa.allOn = [] { dev::allLights(); };
		app.lightsPanel = std::make_unique<ui::LightsPanel>(std::move(lpa));
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
	// An interactive --debug run with no --simlink and no --script skips the title too (kenshi-k16.2.87, user 2026-11-10: "If I run
	// kclient --debug, I get stuck at a main menu that can't do anything"): offline, the title's buttons can start nothing, and the
	// debug run is a fly-around. A title that does show starts its own local server behind it (start_local_server).
	const bool offlineDebug = app.opt.debug && !app.scripted() && app.opt.simlink.empty() && !app.opt.localServer;
	if (((kTitleAtStart.get() && !app.scripted() && app.opt.simlink.empty() && !offlineDebug) || app.opt.localServer) && !app.creator.active())
	{
		app.mainMenu->show(true);
		audiohook_event("Boot_Up_Game");	// the menu music, only under the title (a session that starts in the game has none); Create_or_Load_Game ends it
		if (app.opt.simlink.empty()) { start_local_server(); }
	}
	else if (offlineDebug)
	{
		fprintf(stderr, "kclient: no title screen (--debug with no --simlink and no --script, k16.2.87): the free camera starts over %.0f,%.0f\n", app.pos[0], app.pos[2]);
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
		if (!id) { set_follow(false); }
		else if (set_follow_id(id)) { app.inspect = id; app.panelProducer = 0; }
	};
	act.toggle = [](ui::HudToggle t, bool value)	// the ticks with a sim toggle (order 10); the others are disabled in HudState
	{
		if (t == ui::HudToggle::kBlock) { app.sim.setToggle(0, value); }
		// the original's Stealth toggle (140721410): Stealth when it turns sneak on, General_Click when it turns it off; the HUD's SNEAK tick is silent
		// (ui/hud.cpp) so the click is this one, not a generic click as well
		else if (t == ui::HudToggle::kSneak) { if (app.sim.setToggle(1, value)) { if (value) { post_event(audio::kStealth, "Stealth"); } else { post_event(audio::kGeneralClick, "General_Click"); } } }
		else if (t == ui::HudToggle::kRun) { app.sim.setToggle(2, value); }
		else if (t == ui::HudToggle::kHold) { app.sim.stop(value); }	// order 4: hold 1 stands and holds (CHAR_DYNAMIC kHold), 0 a plain stop
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
	act.floor = [](int delta) { app.buildings.stepFloor(delta); };	// as R / F
	act.moveSpeed = [](int mode) { app.sim.setToggle(5, (uint8_t)mode); };	// the speed diamond: order 10 which 5, the mode for the whole selection
	act.prospect = [] { shortcut("prospect"); };
	act.pause = [](bool paused) { app.sim.pause(paused); };
	act.setSpeed = [](float speed) { if (app.sim.setSpeed(speed)) { audiohook_event(speed_event(speed)); } };	// the HUD's speed button; its resume posts Resume_Game through the clock
	act.setActive = [](uint64_t squad) { app.activeSquad = squad; };
	act.shortcut = [](const std::string& action) { shortcut(action); };
	app.hud = std::make_unique<ui::Hud>(std::move(act));
	app.hud->show(false);	// shown once the sim is live
	app.options = std::make_unique<ui::Options>();
	app.items = std::make_unique<ui::ItemWindows>(item_actions());
	app.items->holdTime = (float)kHoldMenu;
	app.stats = std::make_unique<ui::Stats>(ui::StatsActions{});	// toggle_stats (C / STA): update_ui fills it from CHAR_STATS each frame
	app.machineWin = std::make_unique<ui::BuildingWindow>(machine_actions());
	ui::MapActions map;
	map.draw = [](const ui::MapFrame& f) { app.wmap.drawArea(f.x, f.y, f.w, f.h, f.cx, f.cz, f.pxPerUnit); };
	map.moveTo = [](double x, double z, bool queue) { const double at[3] = { x, app.hm->height(x, z), z }; const SimView::Click click{ "map" }; app.sim.moveTo(at, queue, false, &click); };	// the selection; no height (the map sees only the heightmap): the sim snaps by xz
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

// Per frame, before ui::frame: the HUD's view of the sim. Names from APPEARANCE (protocol 4; none: "#id"); health from CHAR_MEDICAL
// (SimView::health: the mean part health / race max), 0 when dead.
// An INVENTORY entry for the UI. Quality (ITEM_QUALITY): grade 0..5 colours the name (ui: ItemGrade0..4, 5 ItemArtifact, UNSURE). A weapon's tooltip
// ends with its model (MATERIAL_SPECS_WEAPON name, e.g. "Catun No.1") and maker lines (Sword::vf80, kinspect item21a_static); armour shows
// "{word} Grade" (Armour::vf79), both in the grade colour.
// Grade from ITEM_QUALITY `level` (docs/reference/GRADES_GATESQUADS_KO_LIVE.md section a): g = round(level x 100). The colour and the name use different bands;
// the name shows only for 0 < g < 100 and only on items without a model name (armour, crossbows, robot limbs; weapons show their model).
struct ItemName { std::string name, model = {}, maker = {}, makerText = {}, grade_word = {}; int grade = -1; };	// grade: the ItemGrade palette index, 5 = artifact; -1 none

static ItemName item_name(const simlink::Inventory::Entry& e)
{
	ItemName out{ app.sim.nameOf(e.item, app.chars) };
	if (!e.hasQuality) { return out; }
	const int g = (int)std::lround(e.level * 100.0f);
	out.grade = g < 16 ? 0 : g < 36 ? 1 : g < 51 ? 2 : g < 66 ? 3 : g <= 80 ? 4 : 5;
	if (e.model) { out.model = app.sim.nameOf(e.model, app.chars); }
	else if (g > 0 && g < 100) { out.grade_word = g < 11 ? "Prototype" : g <= 20 ? "Shoddy" : g < 41 ? "Standard" : g < 61 ? "High" : g < 81 ? "Specialist" : "Masterwork"; }
	if (e.maker)
	{
		out.maker = app.sim.nameOf(e.maker, app.chars);
		if (const Characters::ModInfo* m = app.chars.modInfo(app.sim.decoder().str(e.maker))) { out.makerText = m->description; }
	}
	return out;
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
static ui::PortraitLayers portrait_layers(uint64_t id)
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

// The status panel's rows from CHAR_STATS (kinspect ui_captures/hud_panel16.md 3-5, inspect_focus.md): State / Goal / Encumbrance / the skill
// row, the stats rows by weapon class, the extended rows, the tag row. A stat is "N (+M)" in StatUp, "N (-M)" in StatDown (TOUGHNESS never
// shows one); skills the sim lacks have no row. UNSURE: the KO point's second figure.
static void stats_info(const simlink::CharStats& cs, ui::HudInfo& info, bool playerFaction)
{
	using CS = simlink::CharStats;
	const uint16_t tagFlags = CS::shownTagFlags(cs.tagFlags, playerFaction);
	static const char* const kState[] = { "Normal", "Defensive", "Sleeping", "Unconscious", "Dead", "Locked", "Unlocked" };
	std::string state = cs.state < 7 ? kState[cs.state] : "Normal";
	if (cs.state == CS::kLocked) { state += " (lvl: " + std::to_string(cs.lockLevel) + ")"; }
	const bool bad = cs.state == CS::kUnconscious || cs.state == CS::kDead || cs.state == CS::kLocked;
	// row tips (findings/hud_tooltips.md 2; our wording): State for the states the original explains, Goal fixed
	const std::string stateTip = cs.state == CS::kDead ? "Dead, and not coming back." : cs.state == CS::kDefensive ? "Fighting defensively: +20 defence." : "";
	info.state.push_back({ "State:", state, bad ? "HudBad" : cs.state == CS::kUnlocked ? "HudGood" : "", stateTip });
	const std::string goal = cs.goal ? app.sim.decoder().str(cs.goal) : "";
	info.state.push_back({ "Goal:", goal.empty() ? "Aimless" : goal, "", "What this character is currently trying to do." });
	info.state.push_back({ "Encumbrance:", std::to_string((int)std::lround(cs.encumbrance)) + "%" });
	if (cs.extraKind == 1 || cs.extraKind == 2) { info.state.push_back({ cs.extraKind == 1 ? "Stealth:" : "Athletics:", std::to_string((int)std::lround(cs.extra)) }); }
	const auto row = [&](uint8_t skill, const char* label)
	{
		for (const CS::SkillRow& k : cs.skills)
		{
			if (k.skill != skill) { continue; }
			const int n = (int)std::lround(k.value), m = skill == CS::kToughness ? 0 : (int)std::lround(k.bonus);
			const std::string mod = m > 0 ? " (+" + std::to_string(m) + ")" : m < 0 ? " (" + std::to_string(m) + ")" : "";
			info.stats.push_back({ label, std::to_string(n) + mod, m > 0 ? "StatUp" : m < 0 ? "StatDown" : "" });
		}
	};
	if (cs.weaponClass == 1) { row(CS::kAttack, "ATTACK:"); row(CS::kDefence, "DEFENSE:"); }
	else if (cs.weaponClass == 0) { row(CS::kMartialArts, "MARTIAL ARTS:"); }
	row(CS::kStrength, "STRENGTH:");
	row(CS::kDexterity, "DEXTERITY:");
	row(CS::kToughness, "TOUGHNESS:");
	if (cs.weaponClass != 2) { row(CS::kDodge, "DODGE:"); }
	const auto pct = [](float f) { return std::to_string((int)std::lround(f * 100)) + "%"; };
	const float unit = speedScale();	// any modifier, + or -, is red
	const int mod = (int)std::lround(-cs.runPenalty * unit);
	info.extended.push_back({ "ATHLETICS XP:", pct(cs.athleticsXp) });
	info.extended.push_back({ "STRENGTH XP:", pct(cs.strengthXp) });
	info.extended.push_back({ "TOUGHNESS XP:", std::to_string((int)std::lround(cs.toughnessXp)) + "x" });
	info.extended.push_back({ "KO POINT:", std::to_string((int)std::lround(cs.koPoint)) });
	info.extended.push_back({ "RUN SPEED:", std::to_string((int)std::lround(cs.runSpeed * unit)) + (mod > 0 ? " (+" + std::to_string(mod) + ")" : mod < 0 ? " (" + std::to_string(mod) + ")" : "") + " " + kSpeedUnit.get(), mod ? "StatDown" : "" });
	info.extended.push_back({ "HUNGER RATE:", pct(cs.hungerRate) });
	// The tag row (HudInfo tag / tagValue / tagTip): the original's if/else chain over the selected character, first match wins, in its order
	// (hud16_static.md 5, hud_panel16.md (5)). The deciding fields are CHAR_STATS rows 24-32 (k16.6.215); an older server stops before them
	// (cs.hasTag false) and only the bounty branch (row 14 bounties) can apply, as before. Branch by branch the wire can still be short: b1
	// army commander, b3 diplomat and b5 escaped prisoner arrive 0 until the sim models them (k16.6.211 / .212 / .214) and slaveLook stays 255
	// (k16.6.213), so those branches stay off and the slave tooltip leaves its Situation / Looks-like lines out rather than show 0 / 255.
	//   1 war leader / leader  b1 / b0    2 kidnapped  b2    3 diplomatic status  b3    4 slave state 1/2/3    5 crime in progress (N)
	//   6 escaped prisoner  b5    7 bounty (row 14 + 32)    8 kidnapped (as 2: the same bit, so the exe's last test is unreachable)
	const simlink::SimLinkDecoder& dec = app.sim.decoder();
	const auto factionName = [&](uint32_t sid) { return app.chars.nameOf(dec.str(sid)); };
	const auto amountText = [](int32_t a) { return "c." + std::to_string(a); };
	// The crime labels of the exe's FUN_140856530 switch (kenshi-inspect item25_static.md (a)); the wire sends the CrimeEnum id only. 7
	// (SLAVE_FREEING) shares 9's "Terrorism" label there.
	const auto crimeName = [](uint8_t c) -> const char*
	{
		static const char* const kNames[] = { "", "Enslaving", "Burglary", "Theft", "Murder", "Assault", "Treason", "Terrorism", "Smuggling", "Terrorism",
			"Looting", "Trespassing", "Escaping Prison", "Fencing", "Crop Theft", "Kidnapping", "Uniform Theft" };
		return c < sizeof kNames / sizeof kNames[0] ? kNames[c] : "a crime";
	};
	// The tooltip's per-faction bounty lines (FUN_1405d2da0): faction left, "c.N" right; the local faction's HudNeutral, the others HudFaint.
	// FUN_1405d1400 adds the same lines to the slave and crime tooltips, so those branches call it too.
	const auto bountyLines = [&](std::vector<ui::HudTipLine>& tip)
	{
		for (const CS::Bounty& b : cs.bounties)
		{
			if (b.amount <= 0) { continue; }
			tip.push_back({ factionName(b.faction), amountText(b.amount), "", b.faction != 0 && b.faction == cs.localFaction ? "HudNeutral" : "HudFaint" });
		}
	};
	if (tagFlags & CS::kSquadLeader)
	{
		const bool army = (tagFlags & CS::kArmyCommander) != 0;
		info.tag = army ? "War Leader" : "Leader";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ army ? "This is the Commander in charge of an entire army." : "This is the leader of this group." });
	}
	else if (cs.tagFlags & CS::kCaptive)
	{
		info.tag = "Kidnapped";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ "This character has been kidnapped, law enforcers will try to help." });
	}
	else if (cs.tagFlags & CS::kDiplomat)
	{
		info.tag = "Diplomatic Status";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ "This is a special person with diplomatic importance. Anyone who attacks this guy will be in serious trouble with his faction." });
	}
	else if (cs.slaveState >= 1 && cs.slaveState <= 3)
	{
		static const char* const kTag[] = { "", "[SLAVE]", "[ESCAPED SLAVE]", "[EX-SLAVE]" };
		const std::string owner = cs.captor ? factionName(cs.captor) : "someone";
		info.tag = kTag[cs.slaveState];
		info.tagColour = "HudBad";	// the brackets too (live, the user's kenshi-8x6 shot 23)
		if (cs.slaveState == 1) { info.tagTip.push_back({ "A slave belonging to " + owner + ". Not considered a person with rights, a slave works for his master until death and nothing more." }); }
		else if (cs.slaveState == 2) { info.tagTip.push_back({ "A slave of " + owner + " that has been seen escaping. Needs to lay low for a day and avoid getting spotted." }); }
		else { info.tagTip.push_back({ "Was once a slave of " + owner + " but has now escaped. The heat has died down but he still needs to avoid getting found out." }); }
		// b4 slaveSeen and slaveLook: a slaveLook of 255 is "not modelled" (SIMLINK row 24 / 28), and 0 cannot tell unseen from unknown
		if (cs.slaveLook != 255)
		{
			const bool seen = (cs.tagFlags & CS::kSlaveSeen) != 0;
			info.tagTip.push_back({ "Situation:", seen ? "Seen!" : "Undiscovered", "", seen ? "HudBad" : "HudGood" });
			info.tagTip.push_back({ "Looks like a slave:", std::to_string(cs.slaveLook) + "%" });
		}
		if (cs.slaveState != 1 && cs.slaveHoursLeft >= 0) { info.tagTip.push_back({ "Time remaining:", std::to_string((int)std::lround(cs.slaveHoursLeft)) + " hour(s)" }); }
		if ((cs.tagFlags & CS::kObedient) != 0) { info.tagTip.push_back({ "Your character will have a permanent job assigned called [Obedient slave]." }); }
		bountyLines(info.tagTip);
	}
	else if (cs.crime != 0)
	{
		const char* name = crimeName(cs.crime);
		info.tag = "Committing Crime!";
		info.tagColour = "HudTag";
		info.tagValue = "(" + std::to_string((int)cs.crimeTimer) + ")";
		// witnessed vs caught: a fresh crime's timer tops out at crime::kMinTimer (3 s); the witness refresh lifts it above that
		if (cs.crimeTimer > 3.0) { info.tagTip.push_back({ std::string("This character has been caught committing ") + name + "!" }); }
		else { info.tagTip.push_back({ std::string("This character is currently committing ") + name + ", make sure nobody witnesses it." }); }
		bountyLines(info.tagTip);
	}
	else if (cs.tagFlags & CS::kEscapedPrisoner)
	{
		info.tag = "[ESCAPED PRISONER]";
		info.tagColour = "HudTag";
		info.tagTip.push_back({ "This character broke out of prison, if he gets spotted by any guards he is in trouble. Leave town to clear this status." });
		if (cs.escapeHoursLeft >= 0) { info.tagTip.push_back({ "Expires in:", std::to_string((int)std::lround(cs.escapeHoursLeft)) + " hr(s)" }); }
	}
	else
	{
		// 7 bounty: row 14's per-faction amounts with row 32's claim / crimes / hoursLeft (same order). A claimed reward (claim 1) is not
		// offered again, so it is out of the tag value, but it stays in the tooltip with its claim line.
		const CS::BountyTag none;
		const auto tagOf = [&](size_t i) -> const CS::BountyTag& { return i < cs.bountyTags.size() ? cs.bountyTags[i] : none; };
		const CS::Bounty* local = nullptr;
		const CS::Bounty* other = nullptr;
		size_t localI = 0, otherI = 0;
		for (size_t i = 0; i < cs.bounties.size(); i++)
		{
			const CS::Bounty& b = cs.bounties[i];
			if (b.amount <= 0 || tagOf(i).claim) { continue; }
			if (b.faction != 0 && b.faction == cs.localFaction) { local = &b; localI = i; }
			else if (!other || b.amount > other->amount) { other = &b; otherI = i; }
		}
		const CS::Bounty* top = local ? local : other;	// the local faction's is the one shown (HudNeutral); else the largest other (HudFaint)
		const size_t topI = local ? localI : otherI;
		if (top)
		{
			info.tag = "Bounty:";
			info.tagColour = "HudTag";
			info.tagValue = amountText(top->amount);
			info.tagValueColour = local ? "HudNeutral" : "HudFaint";
			bountyLines(info.tagTip);
			// "-Wanted For:" the crimes of every bounty listed above, named from their CrimeEnum bits (the union: the exe's list is one line)
			uint32_t bits = 0;
			for (size_t i = 0; i < cs.bounties.size(); i++) { if (cs.bounties[i].amount > 0) { bits |= tagOf(i).crimes; } }
			std::string crimes;
			for (int c = 1; c < 32; c++) { if (bits & (1u << c)) { crimes += (crimes.empty() ? "" : ", ") + std::string(crimeName((uint8_t)c)); } }
			if (!crimes.empty()) { info.tagTip.push_back({ "-Wanted For:", crimes, "HudTag" }); }
			// FUN_1405d1110's wanted paragraph, separator and expiry when the character is not caged; the local bounty's expiry is the one
			// shown, or the "not wanted here" line, or the shown bounty's own hoursLeft when the character stands outside any town.
			const bool caged = cs.state == CS::kLocked || cs.state == CS::kUnlocked;
			const auto expiry = [](float left) -> ui::HudTipLine
			{
				return left < 0 ? ui::HudTipLine{ "Notorious:", "The bounty will never expire.", "HudTag" }
					: ui::HudTipLine{ "The bounty will expire in", std::to_string((int)std::lround(left)) + " hr(s)", "HudTag" };
			};
			if (!caged)
			{
				info.tagTip.push_back({ "This character is wanted by the authorities. Turn them in for a reward." });
				info.tagTip.push_back({ "-------------------------" });
				if (local) { info.tagTip.push_back(expiry(tagOf(localI).hoursLeft)); }
				else if (cs.localFaction != 0) { info.tagTip.push_back({ "This character is not wanted by the local faction.", "", "HudTag" }); }
				else { info.tagTip.push_back(expiry(tagOf(topI).hoursLeft)); }
			}
			for (size_t i = 0; i < cs.bounties.size(); i++)	// the turn-in state, one line per collected reward
			{
				if (cs.bounties[i].amount <= 0 || !tagOf(i).claim) { continue; }
				info.tagTip.push_back({ "Claimed:", factionName(cs.bounties[i].faction) + " " + amountText(cs.bounties[i].amount) });
			}
			// The imprisonment row (kenshi-rwh.133) is the tooltip's last, and only while the character is in a CAGE (state 5 / 6, never a bed):
			// the wire's sentenceHours is the time REMAINING, so the 2000 h FOREVER test is on it (an imported length can differ; item rwh133).
			if (caged)
			{
				if (cs.sentenceHours >= 2000) { info.tagTip.push_back({ "Imprisoned, sentence time:", "FOREVER", "", "HudTag" }); }
				else if ((int)std::ceil(cs.sentenceHours) > 0)
				{
					const int hours = (int)std::ceil(cs.sentenceHours);
					info.tagTip.push_back({ "Imprisoned, sentence time remaining:", std::to_string(hours) + (hours == 1 ? " hr" : " hrs"), "", "HudTag" });
				}
				else { info.tagTip.push_back({ "Imprisoned", "Waiting to be released" }); }
			}
		}
	}
}

// The character stats window's state (kenshi-k16.2.96) from CHAR_STATS, in the original's grouping and order (kui_demo demoStats): the four
// attributes beside the skills in four columns of "-Group-" rows (the wire's ids 19-32 fill the rows the guide used to list as missing, k16.2.98;
// PERCEPTION is an attribute row, as the original shows it). A row needs a wire value: the window paints a whole level on every row, so a skill
// the wire leaves out is left out rather than shown as 0. The Next Level bar is row 21's progress (UNSURE: ksim's note that its formula may not
// match the exe's, deferred to playtesting); the per-stat affects/training/description texts are not on the wire. The derived column is ours (the
// original clips it off): wire values only, in the HUD's own wording.
static void stats_window(uint64_t id, ui::StatsState& s)
{
	using CS = simlink::CharStats;
	s = {};
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	const auto ch = id ? d.chars.find(id) : d.chars.end();
	const CS* cs = id ? app.sim.charStats(id) : nullptr;
	if (ch == d.chars.end() || !cs) { return; }	// nothing to show yet: the window's "No stats received yet"
	s.valid = true;
	s.name = d.str(ch->second.look.name);
	// a skill row as the status panel shows it: N, and "N (+M)" in StatUp / "N (-M)" in StatDown when the wire's bonus is not 0 (TOUGHNESS never shows one)
	const auto row = [cs](const char* name, uint8_t skill, ui::StatRow& out)
	{
		for (const CS::SkillRow& k : cs->skills)
		{
			if (k.skill != skill) { continue; }
			out.name = name;
			out.level = k.value;
			if (skill != CS::kToughness && (int)std::lround(k.bonus) != 0) { out.base = k.value - k.bonus; }
			for (const CS::SkillProgress& p : cs->progress) { if (p.skill == skill) { out.xp = p.progress; break; } }	// row 21: the Next Level bar
			return true;
		}
		return false;
	};
	static const struct { uint8_t skill; const char* name; } kAttributes[] = {
		{ CS::kStrength, "STRENGTH" }, { CS::kToughness, "TOUGHNESS" }, { CS::kDexterity, "DEXTERITY" }, { CS::kPerception, "PERCEPTION" } };
	for (const auto& a : kAttributes)
	{
		ui::StatRow r;
		if (row(a.name, a.skill, r)) { s.attributes.push_back(std::move(r)); }
	}
	// The original's order within each group (kui_demo demoStats); ids 19-32 are the wire's 2026-11-11 table (SIMLINK "CHAR_STATS" row 8)
	static const struct { uint8_t skill; const char* name, *group; int column; } kSkills[] = {
		{ 7, "Katanas", "Weapons", 0 }, { 8, "Sabres", "Weapons", 0 }, { 9, "Hackers", "Weapons", 0 },
		{ 11, "Heavy weapons", "Weapons", 0 }, { 10, "Blunt", "Weapons", 0 }, { 13, "Polearms", "Weapons", 0 },
		{ CS::kAttack, "Melee attack", "Combat", 1 }, { CS::kDefence, "Melee defence", "Combat", 1 },
		{ CS::kDodge, "Dodge", "Combat", 1 }, { CS::kMartialArts, "Martial arts", "Combat", 1 },
		{ 21, "Turrets", "Ranged", 1 }, { 20, "Crossbows", "Ranged", 1 }, { 32, "Precision shooting", "Ranged", 1 },
		{ CS::kStealth, "Stealth", "Thievery", 2 }, { 15, "Lockpicking", "Thievery", 2 },	// 15: lockpicking
		{ 22, "Thievery", "Thievery", 2 }, { 23, "Assassination", "Thievery", 2 },
		{ CS::kAthletics, "Athletics", "Athletics", 2 }, { 25, "Swimming", "Athletics", 2 },
		{ 14, "Field medic", "Sciences", 3 }, { 19, "Engineer", "Sciences", 3 }, { 26, "Robotics", "Sciences", 3 }, { 17, "Science", "Sciences", 3 },
		{ 27, "Weapon smith", "Trades", 3 }, { 28, "Armour smith", "Trades", 3 }, { 29, "Crossbow smith", "Trades", 3 },
		{ 18, "Labouring", "Trades", 3 }, { 30, "Farming", "Trades", 3 }, { 31, "Cooking", "Trades", 3 } };
	ui::StatGroup* group = nullptr;
	for (const auto& k : kSkills)
	{
		ui::StatRow r;
		if (!row(k.name, k.skill, r)) { continue; }
		if (!group || group->title != k.group)
		{
			s.skills.push_back({ k.group, k.column, {} });
			group = &s.skills.back();
		}
		group->rows.push_back(std::move(r));
	}
	std::string derived;
	const auto add = [&derived](const std::string& label, const std::string& value) { derived += label + "\t" + value + "\n"; };
	if (const auto m = d.medical.find(id); m != d.medical.end())
	{
		const simlink::Medical& med = m->second;
		const auto whole = [](float v) { return std::to_string((int)std::floor(v + 0.1f)); };	// the HUD's figure (ui/hud.cpp)
		add("Blood", med.bloodMax >= 0 ? whole(med.blood) + " / " + whole(med.bloodMax) : whole(med.blood));
		if (med.hunger >= 0) { add("Hunger", std::to_string((int)(med.hunger * 100)) + " / 300"); }	// the HUD's scale (0..3 x 100)
	}
	if (cs->hasCarry) { add("Carry weight", std::to_string((int)std::lround(cs->carryKg)) + " / " + std::to_string((int)std::lround(cs->carryMaxKg)) + " kg"); }	// row 22: kg, without the 30 kg carry-person weight
	else { add("Carry weight", std::to_string((int)std::lround(cs->encumbrance)) + "%"); }	// an older server: only the percentage
	if (cs->hasResist) { add("Damage resistance", std::to_string((int)std::lround(cs->damageResist * 100)) + "%"); }	// row 23: the fraction as a percent
	const float unit = speedScale();
	const int mod = (int)std::lround(-cs->runPenalty * unit);
	add("Run speed", std::to_string((int)std::lround(cs->runSpeed * unit)) + (mod > 0 ? " (+" + std::to_string(mod) + ")" : mod < 0 ? " (" + std::to_string(mod) + ")" : "")
		+ " " + kSpeedUnit.get());
	add("KO point", std::to_string((int)std::lround(cs->koPoint)));
	add("Hunger rate", std::to_string((int)std::lround(cs->hungerRate * 100)) + "%");
	s.derived = derived;
}

// The grid inventory (ui::Gear) over SIMLINK 9. A GridItem's id is its INVENTORY entry's index + 1, so an action finds the entry (and the version it
// was built from: the current one; the server answers `stale` when it moved since) without a table.
namespace
{
	// Equipment grids by the entry's INVENTORY slot (1 wielded, 2 + AttachSlot worn): size and accepted kind (Characters::ItemInfo::kind), after
	// INVENTORY_GRID_STATIC (a). UNSURE: the wielded weapon goes to WEAPON I (the 10 x 2 `back` section), the 7 x 1 `hip` (WEAPON II) stays empty;
	// the server has one wielded weapon. AttachSlot 1 (back) has no grid and is not shown.
	struct GearSlot { int grid, cols, rows, kind; };
	const GearSlot* gear_slot(int slot)
	{
		static const GearSlot weapon1 = { ui::kGearWeapon1, 10, 2, 1 }, belt = { ui::kGearBelt, 2, 2, 14 }, shirt = { ui::kGearShirt, 4, 2, 8 }, head = { ui::kGearHead, 4, 3, 3 },
			pants = { ui::kGearPants, 4, 5, 6 }, armour = { ui::kGearArmour, 4, 6, 5 }, boots = { ui::kGearBoots, 4, 2, 9 }, pack = { ui::kGearBackpack, 5, 4, 12 };
		switch (slot)
		{
			case 1: return &weapon1;
			case 2 + 3: return &head;
			case 2 + 5: return &armour;
			case 2 + 6: return &pants;
			case 2 + 8: return &shirt;
			case 2 + 9: return &boots;
			case 2 + 12: return &pack;
			case 2 + 14: return &belt;
		}
		return nullptr;
	}

	// The icon: the game's cache <item>.<material>[.<colour>].png from the entry's material, else the record's own `icon` file; none: the grid writes the name.
	std::string gear_icon(const simlink::Inventory::Entry& e, const std::string& sid, const Characters::ItemInfo* info)
	{
		const simlink::SimLinkDecoder& d = app.sim.decoder();
		if (e.material)
		{
			const std::string cached = ui::icons::cached(sid, d.str(e.material), e.colour ? d.str(e.colour) : std::string());
			if (std::filesystem::exists(ui::res::dataFile(cached)) && ui::icons::get(cached)) { return cached; }	// the game caches only items it has shown: a miss is normal
		}
		return info && !info->icon.empty() ? ui::icons::recordPath(info->icon) : std::string();
	}

	// A trade price per unit (TRADE 43): what the trader asks (value) and pays (sell; -1: it won't buy). valid false outside a trade.
	struct TradePrice { bool valid = false; int32_t value = -1, sell = -1; uint32_t buybackCount = 0; int32_t buybackPrice = 0; };

	// e: entry index of the INVENTORY of id at version (the ITEM_STATS key)
	ui::GridItem gear_item(const simlink::Inventory::Entry& e, size_t index, uint64_t id, uint32_t version, TradePrice trade = {})
	{
		const std::string& sid = app.sim.decoder().str(e.item);
		const Characters::ItemInfo* info = app.chars.itemInfo(sid);
		// ITEM_STATS (order 69), asked once per key for weapons and armour: until it answers the rows below come from the record, then the sim's
		// numbers replace them (a crossbow answers kind 0: its record lines stay)
		if (info && (info->kind == 1 || info->armourClass >= 0)) { app.sim.askItemStats(id, version, (uint16_t)index); }
		const simlink::SimLinkDecoder::ItemStats* stats = info ? app.sim.itemStats(id, version, (uint16_t)index) : nullptr;
		const simlink::SimLinkDecoder::ItemStats* weapon = stats && stats->kind == 2 ? stats : nullptr, * armour = stats && stats->kind == 1 ? stats : nullptr;
		const ItemName q = item_name(e);
		ui::GridItem it;
		it.id = (uint32_t)index + 1;
		it.x = e.x;
		it.y = e.y;
		it.name = q.name;
		it.count = e.count;
		it.icon = gear_icon(e, sid, info);
		if (info) { it.w = info->w; it.h = info->h; it.kind = info->kind; it.soundSwitch = info->soundSwitch; }
		// The generic tooltip (kinspect inv_click10_static.md 5, FUN_1407ae130 order): title (n), tags, ingredients and nutrition (food), the weapon
		// lines (items14_static.md c), weight, value (the record price without the town factor: Avg. Price), description; backpacks add their CONTAINER
		// stats (0x1407aa090's labels; the value formats are MINE); armour its class / material, resistances, coverage (kinspect tooltips.md). Not done:
		// weapon and armour prices, blueprint, map, charges and quality.
		it.tip.title = q.name;
		it.tip.count = e.count;
		const std::string gradeInk = q.grade >= 5 ? "ItemArtifact" : "ItemGrade" + std::to_string(std::max(q.grade, 0));
		if (!q.grade_word.empty()) { it.tip.tags.push_back({ "[" + q.grade_word + " Grade]", gradeInk }); }
		if (trade.valid && trade.buybackCount > 0) { it.tip.tags.push_back({ trade.buybackCount > 1 ? "Buyback (" + std::to_string(trade.buybackCount) + ")" : std::string("Buyback"), "TipBad" }); }
		if (info && info->armourClass >= 0 && !info->coverage.empty())
		{
			// tooltips.md (armour; cloth / light): the class left, the material right, both TipMaker; the words follow fcs_enums.def ArmourClass /
			// MaterialType. Cloth (class 0) has no class tag, only the material; armour without part coverage has no line at all
			static const char* const kClass[] = { "", "[Light Armour Class]", "[Medium Armour Class]", "[Heavy Armour Class]" }, * const kMaterial[] = { "Cloth", "Leather", "Chain", "Metal Plate" };
			it.tip.tags.push_back({ kClass[info->armourClass], "TipMaker", std::string("[") + kMaterial[info->material] + "]" });
		}
		if (info)
		{
			const char* tag = info->function == 3 ? "[Food]" : info->function == 0xd ? "[Book]" : info->function == 0xf ? "[Possibly Edible]" : info->function == 0x10 ? "Ammunition" : nullptr;
			if (tag) { it.tip.tags.push_back({ tag, info->function == 3 ? "TipGood" : "" }); }
			if ((info->function == 3 || info->function == 0xf) && !info->ingredients.empty())
			{
				std::string list;
				for (const std::string& n : info->ingredients) { list += (list.empty() ? "" : ", ") + n; }
				it.tip.lines.push_back({ "-Ingredients", list });
			}
			char buf[64];
			if (info->function == 3 || info->function == 0xf)
			{
				snprintf(buf, sizeof buf, "%d nu", (int)info->charges);
				it.tip.lines.push_back({ "-Nutrition", buf });
			}
			if (info->kind == 1 && e.hasQuality)
			{
				// Sword::vf79 (kinspect items14_static.md c): t = level; the maker scales the damage, the model adds attack / defence
				const Characters::ModInfo* maker = e.maker ? app.chars.modInfo(app.sim.decoder().str(e.maker)) : nullptr;
				const Characters::ModInfo* model = e.model ? app.chars.modInfo(app.sim.decoder().str(e.model)) : nullptr;
				// ITEM_STATS: the sim's damage, bleed, penetration and attack / defence replace the derivation once it answers
				// (the rows are the multipliers cutMult / bluntMult, not the absolute cutDamage / bluntDamage)
				const float t = e.level, cut = weapon ? weapon->cutMult : (0.3f + 1.7f * t) * info->cut * (maker ? maker->cut : 1),
					blunt = weapon ? weapon->bluntMult : 2 * t * info->blunt * (maker ? maker->blunt : 1);
				auto line = [&](const char* label, const char* fmt, auto v, const char* colour = "")
				{
					snprintf(buf, sizeof buf, fmt, v);
					it.tip.lines.push_back({ label, buf, false, colour });
				};
				// Sword::vf79: the class from `skill category` (kinspect prices15_static.md); 5 unarmed / 7 turret have none. UNSURE: its place in the list
				static const char* const kClass[] = { "Katana class", "Sabre class", "Blunt weapon class", "Heavy weapons class", "Hacker class", nullptr, "Crossbow class", nullptr, "Polearm class" };
				if (info->skillCategory >= 0 && info->skillCategory < 9 && kClass[info->skillCategory]) { it.tip.lines.push_back({ kClass[info->skillCategory], "" }); }
				if (cut > 0) { line("-Cutting damage", "%.2f", cut); }	// MINE: a zero damage line is hidden
				if (blunt > 0) { line("-Blunt damage", "%.2f", blunt); }
				line("-Blood loss", "%.2fx", weapon ? weapon->bleed : info->bleed);
				if (const int p = (int)((weapon ? weapon->armourPenetration : info->penetration) * 100)) { line("-Armour penetration", "%+d%%", p, p > 0 ? "TipGood" : "TipBad"); }
				if (const int a = weapon ? (int)std::lround(weapon->attackMod) : info->attack + (model ? model->attack : 0)) { line("-Attack bonus", "%+d", a, a > 0 ? "TipGood" : "TipBad"); }
				if (const int d = weapon ? (int)std::lround(weapon->defenceMod) : info->defence + (model ? model->defence : 0)) { line("-Defence bonus", "%+d", d, d > 0 ? "TipGood" : "TipBad"); }
				if (info->indoors) { line(info->indoors > 0 ? "-Indoors bonus" : "-Indoors penalty", "%+d", info->indoors, info->indoors > 0 ? "TipGood" : "TipBad"); }
				const std::pair<const char*, float> vs[] = { { "-Damage vs robots", info->vsRobot }, { "-Damage vs humans", info->vsHuman }, { "-Damage vs animals", info->vsAnimal } };
				for (const auto& [label, m] : vs)
				{
					if (const int p = (int)std::lround((m - 1) * 100)) { line(label, "%+d%%", p, p > 0 ? "TipGood" : "TipBad"); }
				}
				it.tip.lines.push_back({ "----------------", "" });	// tooltips.md: the weapon's dashed rule before the weight (armour has none)
			}
			if (armour)
			{
				// tooltips.md (armour): the resistances before the weight, values in #afa68b; Harpoon in points (the sim's 0..90), none at 0 (Monk Robe).
				// UNSURE (SIMLINK ITEM_STATS): cutEfficiency is the row "Cut resistance efficiency", harpoon the pierce resist
				const std::pair<const char*, float> res[] = { { "-Blunt resistance", armour->blunt }, { "-Cut resistance", armour->cut }, { "-Cut resistance efficiency", armour->cutEfficiency } };
				for (const auto& [label, v] : res)
				{
					snprintf(buf, sizeof buf, "%ld%%", std::lround(v * 100));
					it.tip.lines.push_back({ label, buf, false, "TipMaker" });
				}
				if (const long h = std::lround(armour->harpoon))
				{
					snprintf(buf, sizeof buf, "%ld pts", h);
					it.tip.lines.push_back({ "-Harpoon resistance", buf, false, "TipMaker" });
				}
			}
			snprintf(buf, sizeof buf, e.count > 1 ? "%g kg (x %u)" : "%g kg", info->weight, e.count);
			it.tip.lines.push_back({ "-Weight", buf });
			if (info->kind == 15)
			{
				// a multiplier as a change in percent, green when it helps
				auto mult = [&](const char* label, float m)
				{
					if (std::fabs(m - 1) < 1e-3f) { return; }
					snprintf(buf, sizeof buf, "%+d%%", (int)std::lround((m - 1) * 100));
					it.tip.lines.push_back({ label, buf, false, m > 1 ? "TipGood" : "TipBad" });
				};
				if (std::fabs(info->encumbrance - 1) >= 1e-3f)
				{
					snprintf(buf, sizeof buf, "%d%%", (int)std::lround((1 - info->encumbrance) * 100));	// `encumbrance effect` 0.5: the contents weigh half
					it.tip.lines.push_back({ "-Encumbrance reduction", buf, false, info->encumbrance < 1 ? "TipGood" : "TipBad" });
				}
				if (info->combatSkill != 0)
				{
					snprintf(buf, sizeof buf, "%+d", info->combatSkill);
					it.tip.lines.push_back({ "-Combat skills bonus", buf, false, info->combatSkill > 0 ? "TipGood" : "TipBad" });
				}
				mult("-Athletics", info->athletics);
				mult("-Combat speed", info->combatSpeed);
				mult("-Stealth", info->stealth);
			}
			// Not done: weapon and armour prices (their formula is unknown to us yet), so they show no value at all.
			// Sell value: a quarter of the value, floored (UNSURE: both tooltips.md captures, c.37,979 / c.9,494 and c.2,085 / c.521)
			// While trading (trade_window.md 2): the trader's per-unit prices, both labels muted, then Avg. Price (the record price) and the
			// mark-up (Value over it; TipBad above 100 %)
			if (trade.valid)
			{
				if (trade.buybackCount > 0) { it.tip.lines.push_back({ "-Value", ui::trade::cats(trade.buybackPrice), true }); }	// the first buyback units cost this, the rest `value`
				else if (trade.value >= 0) { it.tip.lines.push_back({ "-Value", ui::trade::cats(trade.value), true }); }
				if (trade.sell >= 0) { it.tip.lines.push_back({ "-Sell value", ui::trade::cats(trade.sell), true }); }
				if (info->plain && info->price > 0 && trade.value >= 0)
				{
					it.tip.lines.push_back({ "-Avg. Price", ui::trade::cats(info->price) });
					const int pct = (int)std::lround(100.0 * trade.value / info->price);
					it.tip.lines.push_back({ "-Price mark-up", std::to_string(pct) + "%", false, pct > 100 ? "TipBad" : pct < 100 ? "TipGood" : "" });
				}
			}
			else if (info->plain && info->price > 0)
			{
				it.tip.lines.push_back({ "-Value", ui::trade::cats(info->price) });
				it.tip.lines.push_back({ "-Sell value", ui::trade::cats(info->price / 4) });
			}
			// Armour, after the value (tooltips.md): [Coverage] and a row per covered part (none: [No Armour Coverage], muted), then [Skills], then a
			// blank line before the description when there is one. Not done: the resistance rows (the numbers are the sim's: they wait on b0's
			// ITEM_STATS reply)
			if (info->armourClass >= 0)
			{
				if (info->coverage.empty()) { it.tip.lines.push_back({ "[No Armour Coverage]", "", true }); }
				else { it.tip.lines.push_back({ "[Coverage]", "" }); }
				for (const auto& [part, pct] : info->coverage) { it.tip.lines.push_back({ "-" + part, std::to_string(pct) + "%" }); }
				// [Skills], in kinspect's fixed order (tooltips.md "The [Skills] block"); the header only when a row follows. Mult rows "1.10x", hidden
				// at exactly 1 (Athletics: unless > 1 or < 0.97); Fist protection (1 - mult) * 100 "%", only below 1; int rows "+6" / "-3", hidden at 0.
				// Stealth, Athletics, Combat speed, Dexterity and Melee attack/defence only from ITEM_STATS (the loader's values; UNSURE: the sim sends
				// stealth / athletics / dexterity raw for now); Dodge, Attack damage and Martial arts from it once it answers; the rest from the record
				bool header = false;
				const auto skill = [&](const char* label, std::string value, bool good)
				{
					if (!header) { it.tip.lines.push_back({ "[Skills]", "" }); header = true; }
					it.tip.lines.push_back({ label, std::move(value), false, good ? "TipGood" : "TipBad" });
				};
				const auto mult = [&](const char* label, float m, bool shown)
				{
					if (!shown) { return; }
					char buf[32];
					snprintf(buf, sizeof buf, "%.2fx", m);
					skill(label, buf, m > 1.0f);
				};
				const auto bonus = [&](const char* label, int v)
				{
					if (v) { skill(label, (v > 0 ? "+" : "") + std::to_string(v), v > 0); }
				};
				if (armour) { mult("-Stealth effect", armour->stealthMult, armour->stealthMult != 1.0f); }
				mult("-Crossbows effect", info->rangedMult, info->rangedMult != 1.0f);
				const float dodge = armour ? armour->dodgeMult : info->dodgeMult, damage = armour ? armour->damageOutputMult : info->damageMult;
				mult("-Dodge skill effect", dodge, dodge != 1.0f);
				if (armour)
				{
					mult("-Athletics effect", armour->athleticsMult, armour->athleticsMult > 1.0f || armour->athleticsMult < 0.97f);
					mult("-Combat speed effect", armour->loadedCombatSpeedMult, armour->loadedCombatSpeedMult != 1.0f);
					mult("-Dexterity effect", armour->dexterityMult, armour->dexterityMult != 1.0f);
				}
				mult("-Attack damage effect", damage, damage != 1.0f);
				mult("-Assassination skill", info->assassinMult, info->assassinMult != 1.0f);
				if (info->fistMult < 1.0f)
				{
					char buf[32];
					snprintf(buf, sizeof buf, "%g%%", (1.0f - info->fistMult) * 100.0f);
					skill("-Fist protection", buf, true);
				}
				if (armour)
				{
					bonus(armour->meleeAttack == armour->meleeDefence ? "-Melee attack/defence bonus" : "-Melee attack bonus", armour->meleeAttack);
					if (armour->meleeDefence != armour->meleeAttack) { bonus("-Melee defence bonus", armour->meleeDefence); }
				}
				bonus("-Martial arts bonus", armour ? (int)std::lround(armour->unarmedBonus) : info->unarmedBonus);
				bonus("-Perception bonus", info->perceptionBonus);
				if (!info->description.empty()) { it.tip.lines.push_back({ "", "" }); }
			}
			// Sword::vf80, as kenshi-inspect's tooltips.md capture shows it: after the description and a blank line, "[Model # <model>]" in the
			// grade colour, "[Manufactured by <maker>]" in #afa68b, the maker's company description. Not done: "Crafted by {1}" (no crafter in SIMLINK).
			it.tip.description = info->description;
			if (!q.model.empty()) { it.tip.footer.push_back({ "[Model # " + q.model + "]", gradeInk }); }
			if (!q.maker.empty()) { it.tip.footer.push_back({ "[Manufactured by " + q.maker + "]", "TipMaker" }); }
			if (!q.makerText.empty()) { it.tip.footer.push_back({ q.makerText, "" }); }
		}
		return it;
	}

	// A grid number is a kGear* one (kGearContainer in a box's window); the window's owner is the GridRef's. "Far": an open box, body or trader's
	// stock (app.conts); the rest are the player's own character windows.
	bool gear_far(uint64_t owner) { return cont_find(owner) != nullptr; }

	// The ITEM_GRID section a grid shows (255 equipment).
	uint8_t gear_section(int grid) { return grid == ui::kGearBag ? 1 : grid == ui::kGearMain || grid == ui::kGearContainer ? 0 : 255; }

	bool gear_equipment(int grid) { return grid != ui::kGearMain && grid != ui::kGearBag && grid != ui::kGearContainer; }

	// The per-inventory order queue (kenshi-k16.20.14.2). Orders 63-68 carry the INVENTORY version the client last saw, so a second order sent before
	// the answering INVENTORY is refused `stale`; the queue keeps one in flight per inventory and re-resolves the rest at send time. A lost answer must
	// not wedge it (a reply comes within a tick; 2 s is noise), and a load / new world clears it.
	constexpr double kGearOrderTimeout = 2.0;

	// Steady seconds, for the queue's watchdog.
	double gear_now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

	// The entry a queued order names, by its record + section + cell: -1 when it is gone or something else is there now.
	int gear_order_index(const GearOrder& o, const simlink::Inventory& inv)
	{
		for (size_t i = 0; i < inv.items.size(); i++)
		{
			const simlink::Inventory::Entry& e = inv.items[i];
			if (o.byRecord)
			{
				if (e.item == o.record && e.section != 255 && (!o.hasQuality || (e.hasQuality && e.grade == o.grade && e.level == o.level))) { return (int)i; }
				continue;
			}
			if (e.item == o.record && e.section == o.section && e.x == o.x && e.y == o.y) { return (int)i; }
		}
		return -1;
	}

	// An entry with an order in flight or queued for it: GridItem.pending (dimmed, no press, nothing drops onto it). The entry is still the clicked one
	// until the answering INVENTORY moves it or the order is dropped.
	bool gear_pending(uint64_t inventory, const simlink::Inventory::Entry& e)
	{
		const auto it = app.gearQueues.find(inventory);
		if (it == app.gearQueues.end()) { return false; }
		const auto match = [&e](const GearOrder& o) { return o.record == e.item && o.section == e.section && o.x == e.x && o.y == e.y; };
		if (it->second.flight && match(it->second.current)) { return true; }
		for (const GearOrder& o : it->second.wait) { if (match(o)) { return true; } }
		return false;
	}

	// The first free top-left cell, scanning row by row, where a w x h item fits in `section` of the inventory; the entry `skip` (the one being moved) does
	// not block. False: no room.
	bool gear_free_cell(const simlink::Inventory& inv, uint8_t section, int w, int h, int skip, uint8_t& ox, uint8_t& oy)
	{
		int cols = 0, rows = 0;
		for (const simlink::Inventory::Grid& g : inv.grids) { if (g.section == section) { cols = g.w; rows = g.h; } }
		std::vector<bool> used((size_t)std::max(0, cols * rows), false);
		for (size_t i = 0; i < inv.items.size(); i++)
		{
			const simlink::Inventory::Entry& e = inv.items[i];
			if ((int)i == skip || e.section != section) { continue; }
			const Characters::ItemInfo* info = app.chars.itemInfo(app.sim.decoder().str(e.item));
			const int ew = info ? info->w : 1, eh = info ? info->h : 1;
			for (int y = e.y; y < e.y + eh && y < rows; y++) { for (int x = e.x; x < e.x + ew && x < cols; x++) { used[(size_t)(y * cols + x)] = true; } }
		}
		for (int y = 0; y + h <= rows; y++)
		{
			for (int x = 0; x + w <= cols; x++)
			{
				bool free = true;
				for (int yy = y; free && yy < y + h; yy++) { for (int xx = x; xx < x + w; xx++) { if (used[(size_t)(yy * cols + xx)]) { free = false; break; } } }
				if (free) { ox = (uint8_t)x; oy = (uint8_t)y; return true; }
			}
		}
		return false;
	}

	bool g_noRoomShown = false;	// gear_order_send said why it could not send (no free cell): the pump adds no "moved" message

	// Sends the order now: its inventory's current version and the entry's current index. False: it cannot go (the entry moved, the inventory is
	// gone); the caller drops it.
	bool gear_order_send(GearQueue& q, const GearOrder& o)
	{
		const simlink::Inventory* inv = app.sim.inventory(o.inv);
		if (o.kind == 63)	// by record, no version: the machine put
		{
			if (!inv || !app.sim.transfer(o.owner, o.container, o.record, o.count, o.take)) { return false; }
		}
		else if (o.kind == 65)	// arrange: no entry
		{
			if (!inv || !app.sim.arrange(o.owner, o.section)) { return false; }
		}
		else
		{
			const int found = inv ? gear_order_index(o, *inv) : -1;
			if (found < 0) { return false; }
			const uint16_t index = (uint16_t)found;
			GearOrder place = o;
			if (o.kind == 64 && o.autoCell)	// the auto-split's rest: the first free cell that holds the item
			{
				const Characters::ItemInfo* info = app.chars.itemInfo(app.sim.decoder().str(inv->items[index].item));
				if (!gear_free_cell(*inv, o.toSection, info ? info->w : 1, info ? info->h : 1, found, place.toX, place.toY))
				{
					ui::message("No room for that item.");
					g_noRoomShown = true;
					return false;
				}
			}
			const bool sent = o.kind == 64 ? app.sim.moveItem(o.owner, inv->version, index, place.toSection, place.toX, place.toY, o.count)
				: o.kind == 66 ? app.sim.dropEntry(o.owner, inv->version, index, o.count)
				: o.kind == 67 ? app.sim.equipEntry(o.owner, inv->version, index, o.on)
				: app.sim.transferEntry(o.owner, o.container, inv->version, index, o.count, o.take, o.toSection, o.toX, o.toY);
			if (!sent) { return false; }
		}
		q.flight = true;
		q.current = o;
		q.seq = app.sim.lastSentSeq();
		q.version = inv ? inv->version : 0;
		q.sentAt = gear_now();
		printf("gear order %u: %s x%u at %u:%u,%u in %llu (seq %u, version %u)\n", o.kind, app.sim.nameOf(o.record, app.chars).c_str(), o.count,
			o.section, o.x, o.y, (unsigned long long)o.inv, q.seq, q.version);
		return true;
	}

	// Drops every queued order of a moveAll group (its first refusal stops the rest, kenshi-k16.20.12).
	void gear_order_drop_group(uint64_t group)
	{
		if (!group) { return; }
		for (auto& [key, q] : app.gearQueues) { std::erase_if(q.wait, [group](const GearOrder& o) { return o.group == group; }); }
	}

	void gear_orders_pump()
	{
		for (auto& [key, q] : app.gearQueues)
		{
			while (!q.flight && !q.awaitVersion && !q.wait.empty())
			{
				const GearOrder o = std::move(q.wait.front());
				q.wait.pop_front();
				if (gear_order_send(q, o)) { break; }
				if (!app.sim.inventory(o.inv))
				{
					q.wait.clear();	// the window closed under the queue: nothing left to move
					break;
				}
				gear_order_drop_group(o.group);
				if (o.kind != 65 && !g_noRoomShown) { ui::message("That item moved; the order is dropped."); }
				g_noRoomShown = false;
			}
		}
		std::erase_if(app.gearQueues, [](const auto& kv) { return !kv.second.flight && !kv.second.awaitVersion && kv.second.wait.empty(); });
	}

	// Queues one order (its inventory's queue) and sends it if that queue is free.
	void gear_order_push(GearOrder o) { app.gearQueues[o.inv].wait.push_back(std::move(o)); gear_orders_pump(); }

	void gear_orders_clear()
	{
		app.gearQueues.clear();
		app.gearFollow.clear();
	}

	// Every frame, after the sim's update: the answers, the INVENTORY versions they brought, then the next orders. A refusal's own text stays the reply
	// loop's (the original's wordings); only a stale is handled here (re-resolved once with the new version, then a message).
	void gear_orders_frame()
	{
		for (const SimView::OrderAnswer& a : app.sim.takeOrderAnswers())
		{
			for (auto& [key, q] : app.gearQueues)
			{
				if (!q.flight || q.seq != a.seq) { continue; }
				q.flight = false;
				if (a.applied)
				{
					// The answer comes with or before the INVENTORY that carries the new version: the next order must wait for that INVENTORY (sending
					// it with the old version is a guaranteed stale; the burst found this).
					q.awaitVersion = true;
					q.answeredAt = gear_now();
					printf("gear order %u applied: %s (inventory %llu)\n", q.current.kind, app.sim.nameOf(q.current.record, app.chars).c_str(), (unsigned long long)key);
					if (q.current.then) { app.gearFollow.push_back({ *q.current.then, gear_now() }); }
					break;
				}
				if (a.reason == "no_room" && q.current.splitOnFull && q.current.count > 1 && kAutoSplit.get() && !ui::ItemGrid::placeOne)
				{
					// Auto-split: one unit at the cell, then the rest unit by unit anywhere (autoplace); a refusal stops the rest (one group).
					const uint64_t group = ++app.gearGroup;
					GearOrder first = q.current;
					first.count = 1;
					first.splitOnFull = false;
					first.group = group;
					GearOrder rest = q.current;
					rest.count = 1;
					rest.splitOnFull = false;
					rest.group = group;
					if (rest.kind == 64) { rest.autoCell = true; }	// the wire's 64 has no autoplace: a free cell is looked up at send time
					else { rest.toSection = 255; }	// 68 autoplaces
					rest.toX = rest.toY = 0;
					for (uint16_t n = 1; n < q.current.count; n++) { q.wait.push_front(rest); }
					q.wait.push_front(std::move(first));
					app.splitQuiet++;
					printf("gear order %u: %u units do not fit the cell; placed one by one\n", q.current.kind, (unsigned)q.current.count);
					break;
				}
				if (a.reason == "stale" && !q.current.retried)
				{
					GearOrder again = q.current;
					again.retried = true;
					q.wait.push_front(std::move(again));
					printf("gear queue: %llu order re-sent once with the new version\n", (unsigned long long)key);
				}
				else
				{
					gear_order_drop_group(q.current.group);
					if (a.reason == "stale") { ui::message("The inventory moved again; the order is dropped."); }
				}
				break;
			}
		}
		const double now = gear_now();
		for (auto& [key, q] : app.gearQueues)
		{
			const simlink::Inventory* inv = app.sim.inventory(key);
			if (!inv) { q.flight = false; q.awaitVersion = false; q.wait.clear(); continue; }	// the inventory is gone (a load, the window closed): its orders with it
			// An applied order's INVENTORY: the commit the answer announced. Its arrival releases the queue; a lost one must not wedge it.
			if (q.awaitVersion)
			{
				if (inv->version != q.version) { q.awaitVersion = false; }
				else if (now - q.answeredAt >= kGearOrderTimeout)
				{
					fprintf(stderr, "kclient: gear queue: no INVENTORY version %u after an applied order in %.0f s; going on\n", q.version, now - q.answeredAt);
					q.awaitVersion = false;
				}
			}
			if (q.flight && now - q.sentAt >= kGearOrderTimeout)	// a lost answer must not wedge the queue
			{
				fprintf(stderr, "kclient: gear queue: no answer for %s in %.0f s; dropped\n", q.current.name.empty() ? "an order" : q.current.name.c_str(), now - q.sentAt);
				q.flight = false;
			}
		}
		// A chained move's second half (a box -> trader drag): sent when the taken item shows in the user's pack, dropped when it never does.
		for (size_t i = 0; i < app.gearFollow.size();)
		{
			const simlink::Inventory* inv = app.sim.inventory(app.gearFollow[i].first.inv);
			if (inv && gear_order_index(app.gearFollow[i].first, *inv) >= 0)
			{
				GearOrder o = std::move(app.gearFollow[i].first);
				app.gearFollow.erase(app.gearFollow.begin() + (long)i);
				gear_order_push(std::move(o));
			}
			else if (!inv || now - app.gearFollow[i].second >= kGearOrderTimeout)
			{
				app.gearFollow.erase(app.gearFollow.begin() + (long)i);
				ui::message("That item moved; the order is dropped.");
			}
			else { i++; }
		}
		gear_orders_pump();
	}
}

// entry i's trade price: the stock's {buy, sell}, the actor's {sell, sell}; none while the inventory is newer than the TRADE that priced it
static TradePrice trade_price(bool stock, uint32_t version, size_t i)
{
	TradePrice p;
	const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
	if (!app.tradeStock || !trade.valid) { return p; }
	if (stock && version == trade.stockVersion)
	{
		for (const auto& x : trade.stockPrices) { if (x.index == i) { p = { true, x.buy, x.sell }; } }
		for (const auto& x : trade.buyback) { if (x.index == i) { p.buybackCount = x.count; p.buybackPrice = x.price; } }
	}
	else if (!stock && version == trade.actorVersion)
	{
		for (const auto& x : trade.actorPrices) { if (x.index == i) { p = { true, x.sell, x.sell }; } }
	}
	return p;
}

static void gear_fill(ui::GearGridState& g, uint64_t id, const simlink::Inventory& inv, uint8_t section, int cols, int rows, bool stock, bool priced)
{
	g.cols = cols;
	g.rows = rows;
	for (const simlink::Inventory::Grid& gr : inv.grids) { if (gr.section == section) { g.cols = gr.w; g.rows = gr.h; } }
	for (size_t i = 0; i < inv.items.size(); i++)
	{
		if (inv.items[i].section != section) { continue; }
		g.items.push_back(gear_item(inv.items[i], i, id, inv.version, priced ? trade_price(stock, inv.version, i) : TradePrice{}));
		g.items.back().pending = gear_pending(id, inv.items[i]);	// an order of ours is in flight or queued for it (k16.20.14.2)
	}
}

// A character window's state: the player's own, or a looted body's (its container fields stay empty; every box is a ContainerState).
static ui::GearState gear_state(uint64_t owner)
{
	ui::GearState s;
	const simlink::Inventory* have = owner ? app.sim.inventory(owner) : nullptr;
	if (!have) { return s; }
	s.valid = true;
	s.owner = owner;
	s.name = app.sim.decoder().str(app.sim.decoder().chars.at(owner).look.name);
	const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
	const bool trading = app.tradeStock && trade.valid && owner == trade.actor;	// its items carry the trader's sell prices
	gear_fill(s.grids[ui::kGearMain], owner, *have, 0, 8, 10, false, trading);
	if (std::any_of(have->grids.begin(), have->grids.end(), [](const simlink::Inventory::Grid& g) { return g.section == 1; })) { gear_fill(s.grids[ui::kGearBag], owner, *have, 1, 0, 0, false, trading); }
	for (int slot : { 1, 2 + 3, 2 + 5, 2 + 6, 2 + 8, 2 + 9, 2 + 12, 2 + 14 })
	{
		const GearSlot* gs = gear_slot(slot);
		ui::GearGridState& g = s.grids[gs->grid];
		g.cols = gs->cols;
		g.rows = gs->rows;
		g.single = true;
		g.accepts = { gs->kind };
	}
	s.grids[ui::kGearWeapon2] = { 7, 1, true, false, { 1 }, {} };
	for (size_t i = 0; i < have->items.size(); i++)
	{
		const simlink::Inventory::Entry& e = have->items[i];
		const GearSlot* gs = e.section == 255 ? gear_slot(e.slot) : nullptr;
		if (!gs) { continue; }
		s.grids[gs->grid].items.push_back(gear_item(e, i, owner, have->version, trading ? trade_price(false, have->version, i) : TradePrice{}));
		s.grids[gs->grid].items.back().pending = gear_pending(owner, e);
		if (gs->grid == ui::kGearBackpack) { s.bagName = s.grids[gs->grid].items.back().name; }
	}
	return s;
}

// A box's, a trader's stock window; invalid (no inventory yet): the window waits.
static bool container_state(const App::OpenCont& c, ui::ContainerState& s)
{
	const simlink::Inventory* inv = app.sim.inventory(c.id);
	if (!inv) { return false; }
	s.owner = c.id;
	s.pinned = c.pinned;
	if (c.trade)
	{
		const simlink::SimLinkDecoder::Trade& trade = app.sim.decoder().trade;
		const auto ch = app.sim.decoder().chars.find(trade.trader);
		s.name = ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "TRADER";
		std::transform(s.name.begin(), s.name.end(), s.name.begin(), [](unsigned char x) { return (char)std::toupper(x); });
		s.trade = { true, trade.traderMoney };
		gear_fill(s.grid, c.id, *inv, 0, 8, 8, true, true);
	}
	else
	{
		if (const simlink::Interactable* box = app.sim.thing(c.id)) { s.name = app.sim.nameOf(box->record, app.chars); }
		s.foreign = app.sim.foreign(c.id);
		gear_fill(s.grid, c.id, *inv, 0, 8, 8, false, false);	// UNSURE: 8 x 8 only before the first ITEM_GRID
	}
	return true;
}

// Gear's actions as orders. Items are addressed by the entry index in the INVENTORY of the grid they come from (version = the latest seen). A move inside
// one inventory is 64; between the character and the open container or looted body 68 (a body's worn piece too: it is unequipped into the looter's
// pack, SIMLINK 13; dropped on a slot it autoplaces, MINE); to / from an equipment slot 67 (the slot is the record's own: the server
// puts an unequipped piece where autoplace says, the drop cell is not used; UNSURE). A drag or drop out carries what the UI lifted (one unit, or the
// whole stack with Shift / Ctrl + left, kenshi-k16.20.12.1); a right-click moves one unit and Shift + right-click the stack. Ctrl + right-click is
// move all (ours). The orders go through the per-inventory queue (k16.20.14.2), which sends the version it re-resolves at send time.
static uint16_t gear_count(const ui::GridItem& item)
{
	// Shift + right-click moves the whole stack (the original; kinspect inv_click10). Ctrl + right-click is moveAll now (ours, k16.20.12.1), so Ctrl
	// alone no longer means the stack.
	const bool all = app.keys[SAPP_KEYCODE_LEFT_SHIFT] || app.keys[SAPP_KEYCODE_RIGHT_SHIFT];
	return all ? (uint16_t)item.count : 1;
}

// A producer's output taken by a player character (order 68 take; the server checks the reach: its `use range`). count: what the click carried.
static void machine_take(uint64_t owner, const ui::GridItem& item, uint16_t count)
{
	const simlink::Inventory* inv = app.sim.inventory(app.machine);
	if (!inv || !owner || !item.id) { return; }
	const size_t n = (size_t)item.id - 1;
	if (n >= inv->items.size()) { return; }
	GearOrder o;
	o.kind = 68;
	o.inv = app.machine;	// the order carries the machine's INVENTORY version (its source)
	o.owner = owner;
	o.container = app.machine;
	o.record = inv->items[n].item;
	o.section = inv->items[n].section;
	o.x = inv->items[n].x;
	o.y = inv->items[n].y;
	o.count = count ? count : 1;
	o.take = true;
	o.toSection = 255;
	gear_order_push(std::move(o));
}

// The generic machine window (ui::BuildingWindow) of a producer: its one output slot, the item's footprint (ITEM_GRID section 0). The bar waits for the
// producer's progress on the wire (asked of b0; TASK_PROGRESS 9 is not sent for mines).
static ui::BuildingWindowState machine_state()
{
	ui::BuildingWindowState s;
	const simlink::Interactable* t = app.machine ? app.sim.thing(app.machine) : nullptr;
	if (!t || t->kind != simlink::Interactable::kProducer) { return s; }
	s.valid = true;
	s.id = app.machine;
	s.name = app.sim.nameOf(t->record, app.chars);
	ui::BuildingSlot out;
	out.name = app.sim.nameOf(t->output, app.chars);
	out.cols = out.rows = 1;
	if (const simlink::Inventory* inv = app.sim.inventory(app.machine))
	{
		for (const simlink::Inventory::Grid& g : inv->grids) { if (g.section == 0) { out.cols = g.w; out.rows = g.h; } }
		for (size_t i = 0; i < inv->items.size(); i++)
		{
			out.items.push_back(gear_item(inv->items[i], i, app.machine, inv->version));
			out.items.back().pending = gear_pending(app.machine, inv->items[i]);
		}
	}
	out.bar = (ProgressFeed{ t->progress, t->rate, t->progressTick }).at(app.sim.tick());	// SIMLINK kind 4 tail, through the one progress feed
	s.output = out;
	s.outputDropIn = true;	// a producer takes back the item it produces while it fits (user 2026-11-12, k16.19.7 / .8.2); the sim decides, not the slot
	return s;
}

// A right-click on the output moves it to the selection's first player character; the close X / Escape closes the window
static ui::BuildingWindowActions machine_actions()
{
	ui::BuildingWindowActions a;
	a.quickMove = [](uint64_t, int, const ui::GridItem& item)
	{
		const auto& sel = app.sim.selected();
		const auto own = std::find_if(sel.begin(), sel.end(), [](uint64_t id) { return app.sim.isPlayer(id); });
		machine_take(own != sel.end() ? *own : 0, item, gear_count(item));	// a right-click: one unit, the stack with Shift
	};
	a.move = [](uint64_t, int from, const ui::GridItem& item, int to, int, int)
	{
		// A drag from the character's own inventory onto the output: order 63 put (take=0) into the producer, carrying what the UI lifted. The sim decides
		// whether it fits and logs a refusal's reason (event 10); the item stays where it was (nothing moves client-side until the wire says so).
		if (to != ui::kBuildingOut || !app.machine || !item.id) { return; }
		const ui::GridRef source = app.items ? app.items->ref(from) : ui::GridRef{};	// the character window the item was dragged from
		if (!source.owner || gear_far(source.owner)) { return; }	// the window's own slots, or an open container's / body's: not the character's
		const uint64_t owner = app.sim.isPlayer(source.owner) ? source.owner : 0;
		const simlink::Inventory* src = owner ? app.sim.inventory(owner) : nullptr;
		const size_t n = (size_t)item.id - 1;
		if (!src || n >= src->items.size()) { return; }
		GearOrder o;
		o.kind = 63;
		o.inv = owner;	// the character's inventory: its moves queue with this one
		o.owner = owner;
		o.container = app.machine;
		o.record = src->items[n].item;
		o.name = app.sim.nameOf(src->items[n].item, app.chars);
		o.section = src->items[n].section;
		o.x = src->items[n].x;
		o.y = src->items[n].y;
		o.count = (uint16_t)std::max<uint32_t>(1, item.count);	// the carried count (k16.20.12.1: the UI lifts one or the stack)
		printf("machine put %s x%u into %llu: queued\n", o.name.c_str(), (unsigned)o.count, (unsigned long long)app.machine);
		gear_order_push(std::move(o));
	};
	a.close = [](uint64_t) { app.machine = 0; };
	return a;
}

// The last clicked producer's focus panel, bottom left (kenshi-k16.19.7.1, k16.19.8.2; the original in kinspect findings/ui_captures/hud_building.md:
// status "Working" / "Output full" / "No workers", then the resource quality, Workers n/max, Efficiency, Max efficiency, and the Condition slat).
// Its values are the producer's BUILDING (47) row, which the sim sends for the session's order 87 focus (SIMLINK "A producer's row"): before the row
// arrives every value reads "-". Nothing is derived client-side (the item's rule): the fields come from the wire or are reported missing.
static ui::HudBuildingInfo machine_panel()
{
	ui::HudBuildingInfo b;
	const simlink::Interactable* t = app.panelProducer ? app.sim.thing(app.panelProducer) : nullptr;
	if (!t || t->kind != simlink::Interactable::kProducer) { return b; }
	b.valid = true;
	b.id = app.panelProducer;
	b.name = app.sim.nameOf(t->record, app.chars);
	if (t->hasOwner && t->owner) { b.faction = app.chars.nameOf(app.sim.decoder().str(t->owner)); }
	// Everything else is the producer's BUILDING row (order 87's focus row, k16.19.8.2): a value the row has not brought yet is "-", never derived.
	const simlink::SimLinkDecoder::Building* rec = app.sim.focusBuilding();
	// HealthText (hud_building.md): section 7's status, the producer row's three being 0 Working, 5 Output full and 6 No workers
	if (rec)
	{
		switch (rec->status)
		{
		case 0: b.status = "Working"; break;
		case 1: b.status = "Broken"; b.statusColour = "HudBad"; break;
		case 2: b.status = "Under construction"; break;
		case 3: b.status = "Needs materials"; break;
		case 4: b.status = "Paused"; break;
		case 5: b.status = "Output full"; b.statusColour = "HudBad"; break;
		case 6: b.status = "No workers"; b.statusColour = "HudBad"; break;
		default: break;
		}
	}
	int mining = 0;
	if (const auto it = app.buildRecs.find(app.sim.decoder().str(t->record)); it != app.buildRecs.end()) { mining = it->second.mining; }
	// the resource quality row's label comes from the functionality's `world resource mining` (SIMLINK BUILDING 47's quality row); 4 (oil) UNSURE
	static const char* const kQuality[] = { "", "Iron Quality", "Stone Richness", "Copper Quality", "Carbon Quality", "Water Available", "Ground Fertility" };
	char v[32];
	if (mining > 0 && mining < (int)(sizeof kQuality / sizeof kQuality[0]))
	{
		if (rec)	// the location richness x 100, its colour computed as the original's RGB(clamp(0.8 - v), clamp(0.8 v), 0.1)
		{
			const float q = std::clamp(rec->quality, 0.0f, 1.0f);
			char ink[16];
			snprintf(ink, sizeof ink, "#%02x%02x%02x", (int)(std::clamp(0.8f - q, 0.0f, 1.0f) * 255.0f + 0.5f), (int)(std::clamp(0.8f * q, 0.0f, 1.0f) * 255.0f + 0.5f), 26);
			snprintf(v, sizeof v, "%d", (int)(q * 100.0f + 0.5f));
			b.stats.push_back({ std::string(kQuality[mining]) + ":", v, ink });
		}
		else { b.stats.push_back({ std::string(kQuality[mining]) + ":", "-" }); }
	}
	// Workers n/max from the row (maxWorkers 0 = fully automatic: "-", as before); before the row: "-"
	if (rec && rec->maxWorkers > 0) { snprintf(v, sizeof v, "%u/%u", (unsigned)rec->workers, (unsigned)rec->maxWorkers); b.stats.push_back({ "Workers:", v, rec->workers == 0 ? "HudBad" : "" }); }
	else { b.stats.push_back({ "Workers:", "-" }); }
	if (rec)
	{
		snprintf(v, sizeof v, "%d%%", (int)(rec->efficiency * 100.0f + 0.5f));
		b.stats.push_back({ "Efficiency:", v, rec->efficiency < 0.9f ? "HudBad" : "" });
		snprintf(v, sizeof v, "%d%%", (int)(rec->maxEfficiency * 100.0f + 0.5f));
		b.stats.push_back({ "Max efficiency:", v });
		// the Condition slat (hud_building.md): fill min(1, hp/maxHp), the number (int) hp
		if (rec->maxHp > 0)
		{
			ui::HudBuildingBar bar;
			bar.label = "Condition";
			bar.value = std::to_string((int)rec->hp);
			bar.fraction = std::clamp(rec->hp / rec->maxHp, 0.0f, 1.0f);
			b.bars.push_back(bar);
		}
	}
	else { b.stats.push_back({ "Efficiency:", "-" }); b.stats.push_back({ "Max efficiency:", "-" }); }
	return b;
}

// The clicked entry as the queue remembers it (its record + ITEM_GRID section + cell): the index it has now is found again at send time.
static bool gear_order_of(const simlink::Inventory& inv, const ui::GridItem& item, GearOrder& o)
{
	const size_t n = (size_t)item.id - 1;
	if (!item.id || n >= inv.items.size()) { return false; }
	const simlink::Inventory::Entry& e = inv.items[n];
	o.record = e.item;
	o.section = e.section;
	o.x = e.x;
	o.y = e.y;
	o.hasQuality = e.hasQuality;
	o.grade = e.grade;
	o.level = e.level;
	return true;
}

// The window a quick right click moves an item to (kenshi-k16.20.13.2): the marked one, else the only other window. With several others and none
// marked (the original picks by its window map, rwh.78 Q3; UNSURE here): an item of a box or body goes to the character's window, a character's own
// to the first box opened.
static uint64_t quick_partner(uint64_t from)
{
	if (app.itemTarget && app.itemTarget != from && app.items && app.items->window(app.itemTarget)) { return app.itemTarget; }
	if (gear_far(from) && app.itemsShown && app.gearChar && app.gearChar != from) { return app.gearChar; }
	for (const App::OpenCont& c : app.conts) { if (c.id != from && app.sim.inventory(c.id)) { return c.id; } }
	return 0;
}

// One item moved between two windows (to.grid < 0: autoplaced there), as orders: 64 within an inventory, 67 to / from a slot, 68 between a character
// and a box / body / stock. A box -> trader drag is a take into the user's pack and then a put by record (the wire has no container-to-container
// order), the put sent once the take shows in the pack (gear_orders_frame).
static bool item_route(ui::GridRef from, const ui::GridItem& item, ui::GridRef to, int x, int y, uint16_t count, uint64_t group, bool placed = false)
{
	if (!from.owner || !to.owner || !item.id) { return false; }
	const simlink::Inventory* src = app.sim.inventory(from.owner);
	if (!src) { return false; }
	GearOrder o;
	if (!gear_order_of(*src, item, o)) { return false; }
	o.count = count;
	o.group = group;
	o.toSection = to.grid < 0 ? 255 : gear_section(to.grid);
	o.toX = (uint8_t)x;
	o.toY = (uint8_t)y;
	o.splitOnFull = placed && count > 1 && to.grid >= 0 && !gear_equipment(to.grid);	// a stack dropped at a cell (auto-split)
	const bool ff = gear_far(from.owner), tf = gear_far(to.owner);
	if (from.owner == to.owner)
	{
		o.owner = from.owner;
		o.inv = from.owner;
		if (ff)
		{
			if (to.grid < 0 || gear_equipment(from.grid) || gear_equipment(to.grid)) { return false; }	// a body's worn gear goes only to the looter
			o.kind = 64;
			gear_order_push(std::move(o));
			return true;
		}
		const bool fromSlot = gear_equipment(from.grid), toSlot = to.grid >= 0 && gear_equipment(to.grid);
		if (fromSlot) { if (toSlot) { return false; } o.kind = 67; o.on = false; gear_order_push(std::move(o)); return true; }
		if (toSlot) { o.kind = 67; o.on = true; gear_order_push(std::move(o)); return true; }
		o.kind = 64;
		gear_order_push(std::move(o));
		return true;
	}
	if (!ff && tf)	// a put: ours into a box, a body or a stock (a sale)
	{
		if (gear_equipment(from.grid)) { return false; }	// ours worn: unequip it first
		o.kind = 68;
		o.owner = from.owner;
		o.inv = from.owner;
		o.container = to.owner;
		o.take = false;
		gear_order_push(std::move(o));
		return true;
	}
	if (ff && !tf)	// a take: into the character the item was dropped on
	{
		o.kind = 68;
		o.owner = to.owner;
		o.inv = from.owner;
		o.container = from.owner;
		o.take = true;
		gear_order_push(std::move(o));
		return true;
	}
	if (ff && tf)
	{
		const uint64_t actor = cont_actor(from.owner) ? cont_actor(from.owner) : cont_actor(to.owner);
		if (!actor || !app.sim.inventory(actor)) { return false; }
		GearOrder put = o;
		put.kind = 68;
		put.byRecord = true;
		put.owner = actor;
		put.inv = actor;
		put.container = to.owner;
		put.take = false;
		put.toSection = 255;
		put.section = 0;
		put.splitOnFull = false;
		o.kind = 68;
		o.owner = actor;
		o.inv = from.owner;
		o.container = from.owner;
		o.take = true;
		o.toSection = 255;
		o.splitOnFull = false;
		o.then = std::make_shared<GearOrder>(std::move(put));
		gear_order_push(std::move(o));
		return true;
	}
	return false;
}

// Every entry of the clicked item's grid with its record (and grade and level, when it has them), whole stacks, to `partner`: one order each in one
// group, so the first refusal stops the rest (gear_orders_frame drops the group). Ctrl + right-click and the menu's "all" rows (kenshi-k16.20.13.3).
static void move_all(ui::GridRef at, const ui::GridItem& item, uint64_t partner)
{
	const simlink::Inventory* inv = partner ? app.sim.inventory(at.owner) : nullptr;
	if (!inv || !item.id) { return; }	// no partner window open: nothing to move them to
	GearOrder pick;
	if (!gear_order_of(*inv, item, pick)) { return; }
	const uint8_t section = gear_section(at.grid);
	const uint64_t group = ++app.gearGroup;
	size_t queued = 0;
	for (size_t i = 0; i < inv->items.size(); i++)
	{
		const simlink::Inventory::Entry& e = inv->items[i];
		if (e.section != section || e.item != pick.record) { continue; }
		if (pick.hasQuality && (!e.hasQuality || e.grade != pick.grade || e.level != pick.level)) { continue; }
		ui::GridItem one = item;
		one.id = (decltype(one.id))(i + 1);
		if (item_route(at, one, { partner, -1 }, 0, 0, (uint16_t)std::max<uint32_t>(1, e.count), group)) { ++queued; }	// the whole stack
	}
	printf("gear moveAll: group %llu, %zu %s stacks into %llu\n", (unsigned long long)group, queued, app.sim.nameOf(pick.record, app.chars).c_str(), (unsigned long long)partner);
}

// How many entries of the clicked item's grid share its record (and grade and level): the menu's "all of this type" row shows from 2.
static size_t same_kind_entries(ui::GridRef at, const ui::GridItem& item)
{
	const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
	GearOrder pick;
	if (!inv || !item.id || !gear_order_of(*inv, item, pick)) { return 0; }
	size_t n = 0;
	for (const simlink::Inventory::Entry& e : inv->items)
	{
		if (e.section != gear_section(at.grid) || e.item != pick.record) { continue; }
		if (pick.hasQuality && (!e.hasQuality || e.grade != pick.grade || e.level != pick.level)) { continue; }
		n++;
	}
	return n;
}

// A window's title for the menu rows: a box's or trader's name, a character's.
static std::string window_name(uint64_t id)
{
	if (const App::OpenCont* c = cont_find(id))
	{
		if (c->trade)
		{
			const auto ch = app.sim.decoder().chars.find(app.sim.decoder().trade.trader);
			return ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "the trader";
		}
		if (!c->body) { if (const simlink::Interactable* box = app.sim.thing(id)) { return app.sim.nameOf(box->record, app.chars); } }
	}
	const auto ch = app.sim.decoder().chars.find(id);
	return ch != app.sim.decoder().chars.end() ? app.sim.decoder().str(ch->second.look.name) : "the container";
}

// The other item windows an item of `from` could go to, the marked one first.
static std::vector<uint64_t> item_destinations(uint64_t from)
{
	std::vector<uint64_t> d;
	if (app.itemsShown && app.gearChar && app.gearChar != from && app.sim.inventory(app.gearChar)) { d.push_back(app.gearChar); }
	for (const App::OpenCont& c : app.conts) { if (c.id != from && app.sim.inventory(c.id)) { d.push_back(c.id); } }
	const auto marked = std::find(d.begin(), d.end(), app.itemTarget);
	if (app.itemTarget && marked != d.end()) { std::rotate(d.begin(), marked, marked + 1); }
	return d;
}

// What a right click on an item of window `from` would do (the badge, and whether the click opens the menu): the marked window, else the only other
// window; ambiguous (several, none marked): the menu; no other window: none (a worn piece unequips, a stored one equips).
// The worn shackles' lock when this is a worn item with a locked one (ITEM_LOCK 57); else nullptr. A locked worn item has no unequip: its right click is
// the menu with the three ways through it.
static const simlink::Inventory::Lock* worn_lock(ui::GridRef at, const ui::GridItem& item)
{
	if (!at.owner || !item.id || gear_far(at.owner) || !gear_equipment(at.grid)) { return nullptr; }
	const simlink::Inventory* inv = app.sim.inventory(at.owner);
	const simlink::Inventory::Lock* l = inv ? inv->lockOf((size_t)item.id - 1) : nullptr;
	return l && l->locked ? l : nullptr;
}

static ui::RightVerb item_verb(ui::GridRef at, const ui::GridItem& item)
{
	if (worn_lock(at, item)) { return ui::RightVerb::kMenu; }
	if (!at.owner || !item.id || (!gear_far(at.owner) && gear_equipment(at.grid))) { return ui::RightVerb::kNone; }
	const std::vector<uint64_t> d = item_destinations(at.owner);
	if (d.empty()) { return ui::RightVerb::kNone; }
	if (d.size() > 1 && d.front() != app.itemTarget) { return ui::RightVerb::kMenu; }
	const App::OpenCont* to = cont_find(d.front());
	const App::OpenCont* from = cont_find(at.owner);
	if (to && to->trade) { return ui::RightVerb::kSell; }
	if (from && from->trade) { return ui::RightVerb::kBuy; }
	return ui::RightVerb::kTransfer;
}

// The menu's actions: row = kind * 100 + destination slot (into g_menuDest, filled by the menu query that opened it).
enum { kRowOne = 1, kRowStack, kRowAll, kRowEquip, kRowUnequip, kRowDrop, kRowDropStack, kRowPickLock, kRowUseTools, kRowUseStrength };
static std::vector<uint64_t> g_menuDest;

// The item windows' actions. Items are addressed by the entry index in the INVENTORY of the grid they come from (version = the latest seen). A move
// inside one inventory is 64; between a character and a box / body / stock 68 (a body's worn piece too: it is unequipped into the looter's pack,
// SIMLINK 13; dropped on a slot it autoplaces, MINE); to / from an equipment slot 67 (the slot is the record's own: the server puts an unequipped
// piece where autoplace says, the drop cell is not used; UNSURE). A drag or drop out carries what the UI lifted (one unit, or the whole stack with
// Shift / Ctrl + left, kenshi-k16.20.12.1); a right-click moves one unit and Shift + right-click the stack, to the marked window else the only
// other one. Ctrl + right-click is move all (ours). The orders go through the per-inventory queue (k16.20.14.2).
static ui::ItemWindowsActions item_actions()
{
	ui::ItemWindowsActions a;
	a.move = [](ui::GridRef from, const ui::GridItem& item, ui::GridRef to, int x, int y)
	{
		const uint16_t count = (uint16_t)std::max<uint32_t>(1, item.count);	// what the UI lifted (k16.20.12.1)
		if (!from.owner)	// the machine window's output (autoplaced into the window it was dropped on)
		{
			if (to.owner) { machine_take(cont_actor(to.owner), item, count); }
			return;
		}
		item_route(from, item, to, x, y, count, 0, true);
	};
	// Right-click on release (kinspect inv_click10; double-click is unbound in the original): the item moves to the partner window (autoplaced);
	// else a worn piece unequips and a stored equippable equips. MINE / not done: the one-window re-pack of other items, and the use actions
	// (blueprint learn, map reveal, money) need sim orders; the failure messages ("Can't wear that item") come from the server replies.
	a.quick = [](ui::GridRef at, const ui::GridItem& item, uint32_t)
	{
		const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
		if (!inv || !item.id) { return; }
		const bool far = gear_far(at.owner);
		const uint16_t count = gear_count(item);	// one unit; Shift + right-click the stack
		GearOrder o;
		if (!far && gear_equipment(at.grid))
		{
			if (!gear_order_of(*inv, item, o)) { return; }
			o.kind = 67;
			o.owner = o.inv = at.owner;
			o.on = false;
			o.count = count;
			gear_order_push(std::move(o));
			return;
		}
		if (const uint64_t partner = quick_partner(at.owner)) { item_route(at, item, { partner, -1 }, 0, 0, count, 0); return; }
		if (!far && item.kind && gear_order_of(*inv, item, o))
		{
			o.kind = 67;
			o.owner = o.inv = at.owner;
			o.on = true;
			o.count = count;
			gear_order_push(std::move(o));
		}
	};
	// Ctrl + right-click (ours, kenshi-k16.20.12.1): every entry of that grid with the clicked item's record (and its grade and level, when it has
	// them), whole stacks, to the window a right-click moves to. The first refusal stops the rest (gear_orders_frame drops the group).
	a.moveAll = [](ui::GridRef at, const ui::GridItem& item) { move_all(at, item, at.owner ? quick_partner(at.owner) : 0); };
	a.dropOutside = [](ui::GridRef at, const ui::GridItem& item)
	{
		const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
		if (!inv || gear_far(at.owner) || gear_equipment(at.grid) || !item.id) { return; }
		GearOrder o;
		if (!gear_order_of(*inv, item, o)) { return; }
		o.kind = 66;
		o.inv = at.owner;
		o.owner = at.owner;
		o.count = (uint16_t)std::max<uint32_t>(1, item.count);
		gear_order_push(std::move(o));
	};
	a.verb = [](ui::GridRef at, const ui::GridItem& item, uint32_t) { return item_verb(at, item); };
	// The item menu (hold right, or an ambiguous right click): per destination window the unit, the stack and all of the type (verb by the two
	// windows: sell to a trader, buy from one, move between ours, pick up from a box or body), then equip / unequip and drop. The binding shows on the
	// rows a click already does: only when the click is not ambiguous.
	a.menu = [](ui::GridRef at, const ui::GridItem& item)
	{
		std::vector<ui::ItemMenuRow> rows;
		if (!at.owner || !item.id) { return rows; }
		if (const simlink::Inventory::Lock* lock = worn_lock(at, item))	// locked shackles (order 60 actions 15 / 16 / 17, SIMLINK "Shackle orders"): the three ways through
		{
			char pick[40];
			snprintf(pick, sizeof pick, "Pick Lock %.1f%%", std::floor(lock->chance * 1000.0f) * 0.1f);	// the original's int(chance x 1000) x 0.1
			rows.push_back({ pick, "", true, kRowPickLock * 100 });
			rows.push_back({ "Use Tools", "", true, kRowUseTools * 100 });
			rows.push_back({ "Use Strength", "", true, kRowUseStrength * 100 });
			return rows;
		}
		const bool far = gear_far(at.owner), worn = !far && gear_equipment(at.grid);
		const App::OpenCont* from = cont_find(at.owner);
		g_menuDest.clear();
		if (!worn) { g_menuDest = item_destinations(at.owner); }
		const bool clicked = item_verb(at, item) != ui::RightVerb::kMenu && item_verb(at, item) != ui::RightVerb::kNone;
		const size_t kinds = same_kind_entries(at, item);
		const simlink::Inventory* inv = app.sim.inventory(at.owner);
		if (!inv || (size_t)item.id > inv->items.size()) { return rows; }
		const std::string kind = app.sim.nameOf(inv->items[(size_t)item.id - 1].item, app.chars);
		for (size_t i = 0; i < g_menuDest.size(); i++)
		{
			const App::OpenCont* to = cont_find(g_menuDest[i]);
			const char* verb = to && to->trade ? "Sell" : from && from->trade ? "Buy" : far ? "Pick up" : "Move";
			const std::string where = g_menuDest.size() > 1 ? " to " + window_name(g_menuDest[i]) : std::string();
			const bool bound = clicked && i == 0;
			rows.push_back({ std::string(verb) + where, bound ? "Right click" : "", true, kRowOne * 100 + (int)i });
			if (item.count > 1) { rows.push_back({ std::string(verb) + " stack" + where, bound ? "Shift+Right click" : "", true, kRowStack * 100 + (int)i }); }
			if (kinds > 1) { rows.push_back({ std::string(verb) + " all " + kind + where, bound ? "Ctrl+Right click" : "", true, kRowAll * 100 + (int)i }); }
		}
		const bool lone = g_menuDest.empty();	// nothing to move it to: the plain right click equips / unequips
		if (worn) { rows.push_back({ "Unequip", "Right click", true, kRowUnequip * 100 }); }
		else if (!far && item.kind) { rows.push_back({ "Equip", lone ? "Right click" : "", true, kRowEquip * 100 }); }
		if (!far && !worn)
		{
			rows.push_back({ "Drop", "Drag outside", true, kRowDrop * 100 });
			if (item.count > 1) { rows.push_back({ "Drop stack", "", true, kRowDropStack * 100 }); }
		}
		return rows;
	};
	a.pick = [](ui::GridRef at, const ui::GridItem& item, int action)
	{
		const int row = action / 100;
		const size_t slot = (size_t)(action % 100);
		const uint64_t dest = slot < g_menuDest.size() ? g_menuDest[slot] : 0;
		const simlink::Inventory* inv = at.owner ? app.sim.inventory(at.owner) : nullptr;
		if (!inv || !item.id) { return; }
		GearOrder o;
		switch (row)
		{
			case kRowOne: if (dest) { item_route(at, item, { dest, -1 }, 0, 0, 1, 0); } break;
			case kRowStack: if (dest) { item_route(at, item, { dest, -1 }, 0, 0, (uint16_t)std::max<uint32_t>(1, item.count), 0); } break;
			case kRowAll: move_all(at, item, dest); break;
			case kRowEquip:
			case kRowUnequip:
				if (!gear_order_of(*inv, item, o)) { return; }
				o.kind = 67;
				o.owner = o.inv = at.owner;
				o.on = row == kRowEquip;
				o.count = 1;
				gear_order_push(std::move(o));
				break;
			case kRowPickLock:
			case kRowUseTools:
			case kRowUseStrength:
			{
				const bool sent = app.sim.shackle(at.owner, at.owner, (uint8_t)(15 + row - kRowPickLock));
				printf("shackles: %s on %llu: %s\n", row == kRowPickLock ? "pick" : row == kRowUseTools ? "cut" : "break", (unsigned long long)at.owner, sent ? "sent" : "not sent");
				break;
			}
			case kRowDrop:
			case kRowDropStack:
				if (!gear_order_of(*inv, item, o)) { return; }
				o.kind = 66;
				o.owner = o.inv = at.owner;
				o.count = row == kRowDrop ? 1 : (uint16_t)std::max<uint32_t>(1, item.count);
				gear_order_push(std::move(o));
				break;
		}
	};
	a.arrange = [](ui::GridRef at)
	{
		if (!at.owner) { return; }
		GearOrder o;
		o.kind = 65;
		o.inv = at.owner;
		o.owner = at.owner;
		o.section = (uint8_t)(at.grid == ui::kGearBag ? 1 : 0);
		gear_order_push(std::move(o));
	};
	a.mark = [](uint64_t owner) { app.itemTarget = owner; };	// the single target; 0: unmarked
	a.close = [](uint64_t owner)
	{
		if (cont_find(owner))	// a body's (the loot ends) or a box's or a stock's window
		{
			printf("window %llu closed\n", (unsigned long long)owner);
			app.gearQueues.erase(owner);
			cont_close(owner);
			return;
		}
		app.itemsShown = false;	// the character's
		if (app.itemTarget == owner) { app.itemTarget = 0; }
	};
	a.pin = [](uint64_t owner, bool on)
	{
		if (App::OpenCont* c = cont_find(owner)) { c->pinned = on; }
		else { app.charPinned = on; }
	};
	return a;
}

// The squads window over PLAYER_SQUADS (SIMLINK 31); each action is one order 70-75 (SIMLINK section 9), answered by a fresh record.
static ui::SquadsState squads_state()
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

static ui::SquadsActions squads_actions()
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
static ui::ResearchState research_state()
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

static ui::ResearchActions research_actions()
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
static void discovery_log()
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

static void map_places(ui::MapState& ms)
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

static void prospect_paint()
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
static ui::ProspectState prospect_state()
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

// The over-head progress bars' level on the wire (order 98): sent after HELLO (a fresh session is at the sim's default 1), again after every
// SCENARIO_STARTED and NEW_GAME (the sim resets it to 1 there), and whenever the setting changes (the per-frame call). `force` for those events.
static void progress_level_sync(bool force = false)
{
	if (!app.sim.active() || !app.greeted) { return; }
	const int level = progress_level();
	if (!force && level == app.progressLevelSent) { return; }
	if (app.sim.progressLevel((uint8_t)level)) { app.progressLevelSent = level; }
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
		if (id && set_follow_id(id))
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
			m.toggles[(int)ui::HudToggle::kHold] = f & simlink::kHold;
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
		st.toggleEnabled[(int)ui::HudToggle::kHold] = true;
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
		const uint64_t focusId = app.panelProducer ? app.panelProducer : app.inspect;
		app.sim.focus(focusId, app.panelProducer != 0);
		if (app.scripted() && (focusId != app.focusLogged || (app.panelProducer != 0) != app.focusLoggedProducer))
		{
			printf("focus %llu kind %s\n", (unsigned long long)focusId, app.panelProducer ? "producer" : "character");
			app.focusLogged = focusId;
			app.focusLoggedProducer = app.panelProducer != 0;
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
		if (app.scripted() && app.sim.decoder().seenBySerial != app.seen.seenBy)	// SEEN_BY (59): each sneaking character's hostile observers that know it, with the frame's tick
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
		if (app.scripted() && ++hudFrames % 300 == 0)
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
			if (const simlink::Inventory* inv = app.sim.inventory(trade.stock)) { for (const auto& e : inv->items) { printf(" %s x%u @%u,%u", app.sim.nameOf(e.item, app.chars).c_str(), e.count, e.x, e.y); } }
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
					if (const simlink::Inventory* inv = app.sim.inventory(pd.id)) { for (const auto& e : inv->items) { printf(" %s x%u", app.sim.nameOf(e.item, app.chars).c_str(), e.count); } }
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
	map_places(ms);
	ms.cameraX = app.pos[0];
	ms.cameraZ = app.pos[2];
	ms.cameraYaw = (float)app.yaw;
	app.overview->updateMap(ms);
	dev::updateUi();	// the playtest windows
	ui::frame(dt);
	if (app.creatorUi->shown()) { logicthread::renderIdle(); creator_frame(); }	// overlap (rule 12): the creator's frames run gated (the switch to it too)
	else { app.buildings.setEditor(false); }
}

void cleanup()
{
	lightbake::cancel();	// first: an in-flight sun/probe bake stops at its next chunk while the logic thread and the server stop (the window no longer pumps events here)
	readback::flushWrites();	// every quit path: the screenshots still being written land first (a script's quit right after its shot)
	if (logicthread::running())	// rule 8: the stop flag, the notify, the join; only then anything below (it all touches what logic used)
	{
		const double ms = logicthread::stop();
		fprintf(stderr, "kclient: quit: logic thread joined in %.0f ms\n", ms);
		lastframe::shutdown();
	}
	{	// every quit path (menu button, window close, scripted quit) ends here: stop the in-process server before anything else, not in the static App's
		// destructor, which can hang on another member first (Grass::~Grass) and never reach ~LocalServer: the server kept ticking (kenshi-379.12)
		const auto t0 = std::chrono::steady_clock::now();
		app.server.stop();
		const auto t1 = std::chrono::steady_clock::now();
		const auto ms = [](auto d) { return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
		fprintf(stderr, "kclient: quit: server stopped in %lld ms (%s)\n", ms(t1 - t0),
			g_quitAsked == std::chrono::steady_clock::time_point{} ? "window close" : ("quit requested " + std::to_string(ms(t1 - g_quitAsked)) + " ms ago").c_str());
	}
	app.buildings.stopThreads();	// the early returns below skip buildings.shutdown(): its workers would block ~App (second hang, worldbuildings workers in cond_destroy)
	dev::cleanup();
	windowgate::flush();	// a change still held at exit is applied, so the saved geometry is the state the client applied (k16.2.222)
	windowgeom::save();	// before cfg::save() below writes it
	audiohook_shutdown();	// before the early returns: the audio thread may be running from the loading screen on
	cfg::save();
	if (!dev::viewing()) { app.hud.reset(); app.options.reset(); app.items.reset(); app.nameLabels.reset(); app.speechBubbles.reset(); app.dialogue.reset(); app.squadsUi.reset(); app.research.reset(); app.crafting.reset(); app.faction.reset(); app.prompt.reset(); app.overview.reset(); app.prospect.reset(); app.build.reset(); app.creatorUi.reset(); app.tutorials.reset();
#ifdef KCLIENT_DEV_TOOLS
		dev::resetDebugPanels(); dev::resetAnimBrowser();
#endif
#ifdef KCLIENT_DEBUG_VIEW
		app.debugTree.reset();
#endif
		 app.gameMenu.reset(); app.mainMenu.reset(); app.newGame.reset(); app.adv.reset(); app.saves.reset(); ui::shutdown(); }
	rq::flushAll();	// the resource jobs the last logic frame queued (docs/RENDER_THREAD.md §7.2), while every renderer is still up
	if (app.loading() && !dev::viewing()) { app.loadScreen.shutdown(); rq::flushAll(); sdtx_shutdown(); sgl_shutdown(); sg_shutdown(); return; }	// closed during the loading screen: most subsystems never started
	if (dev::viewing()) { dev::viewShutdown(); rq::flushAll(); sdtx_shutdown(); sgl_shutdown(); sg_shutdown(); return; }	// the mesh viewer's buffers, while sokol still is
	if (!app.opt.matReport.empty())
	{
		std::ofstream(app.opt.matReport) << app.buildings.materialReport();
	}
	if (app.grassOn) { app.grass.shutdown(); }
	if (app.foliageOn) { app.foliage.shutdown(); }
	if (app.fogvol.enabled) { app.fogvol.shutdown(); }
	if (app.opt.particles) { app.particles.shutdown(); }
	app.blood.shutdown();
	dev::labShutdown();	// the animation lab's render target and character, while sokol still is
	if (app.portraitsTex) { dl::gpu::unbind(app.portraitsTex); app.portraitsTex = 0; }
	app.portraits.shutdown();
	app.portraitTable.clear();
	app.post.shutdown();
	interiorclip::shutdown();
	app.sunShafts.shutdown();
	app.dust.shutdown();
	app.lightVolumes.shutdown();
	app.hdr.shutdown();
	app.refl.shutdown();
	app.idbuf.shutdown();
	GPU_SHUTDOWN(app.gpu);
	app.squads.shutdown();
	app.arrows.shutdown();
	app.chars.shutdown();
	if (app.bldSet) { app.buildings.shutdown(); }
	if (app.opt.water) { app.water.shutdown(); }
	app.wx.shutdown();
	spec_cube_shutdown();
	app.creatorScene3d.shutdown();
	app.sky.shutdown();
	app.terrain.shutdown();
	app.shadow.shutdown();
	app.wmap.shutdown();
	app.cursors.shutdown();
	app.trails.shutdown();	// the dump's open segment (the context is sgl_shutdown's, k16.19.6.1)
	rq::flushAll();	// the shutdowns' queued destroys
	dl::gpu::shutdown();	// the draw list's textures, pipeline and samplers (render hook 8; none until something records)
	sdtx_shutdown();
	sgl_shutdown();
	sg_shutdown();
	if (g_exitCode) { exit(g_exitCode); }
}

// The bound actions of a key press (ui::keys, `key.*` in kclient.cfg; docs/UI.md "Keys"). Game actions without a client side yet do nothing.

static void key_actions(const sapp_event& e)
{
	const auto pressed = [&](const std::string& action) { return ui::keys::pressed(e, action); };
	if (pressed("debug.terrain_view") && app.terrain.hasColour) { app.terrain.viewMode = (app.terrain.viewMode + 1) % (app.terrain.detail.ok ? 4 : 3); }
	for (const char* id : { "debug.squad_overlay", "debug.navmesh", "debug.trails", "debug.shadows", "debug.walk", "debug.free_camera", "debug.autorun", "debug.clock_pause" }) { if (pressed(id)) { debug_toggle(id); } }
#ifdef KCLIENT_DEV_TOOLS
	if (pressed("debug.trails_panel")) { dev::toggleTrailsPanel(); }
	if (pressed("debug.lights_panel") && app.lightsPanel) { app.lightsPanel->show(!app.lightsPanel->shown()); }
#endif
	if (pressed("camera_drive")) { debug_toggle("debug.follow"); }
	if (pressed("debug.follow")) { debug_toggle("debug.follow"); }
	if (pressed("cycle_run_speed")) { app.sim.setToggle(5, (uint8_t)((move_speed() + 1) % 4)); }	// the original's Num6: the speed diamond's next mode
	if (app.foliageOn && (pressed("debug.foliage_more") || pressed("debug.foliage_less")))
	{
		static const double steps[] = { 0.5, 1, 2, 4, 6, 8, 12, 16 };
		int at = 0;
		for (int i = 0; i < 8; i++) { if (steps[i] <= app.foliage.foliageRange * 1.001) { at = i; } }
		app.foliage.foliageRange = steps[std::clamp(at + (pressed("debug.foliage_more") ? 1 : -1), 0, 7)];
	}
	// Our floor keys are R / F where the original's are PgUp / PgDn: Change_Level on the change (1407210a0 / 1407210f0), as the HUD's floor arrows
	if (pressed("floor_up")) { if (app.buildings.stepFloor(1)) { post_event(audio::kChangeLevel, "Change_Level"); } }
	if (pressed("floor_down")) { if (app.buildings.stepFloor(-1)) { post_event(audio::kChangeLevel, "Change_Level"); } }
	// the sneak key (X) is the SNEAK toggle: Stealth turning sneak on, General_Click turning it off (140721410)
	if (pressed("toggle_sneak") && app.sim.active())
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
	for (const char* a : { "toggle_options", "toggle_inventory", "toggle_stats", "toggle_squads", "toggle_map", "toggle_faction", "toggle_research", "toggle_crafting", "toggle_help", "prospect", "toggle_build", "quicksave", "quickload" }) { if (pressed(a)) { shortcut(a); } }
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
	if (pressed("debug.map_zoom_in")) { app.wmap.zoom = std::max(WorldMap::kMinZoom, app.wmap.zoom - 1); }
	if (pressed("debug.map_zoom_out")) { app.wmap.zoom = std::min(WorldMap::kMaxZoom, app.wmap.zoom + 1); }
	if (pressed("debug.mouse_look")) { app.look = !app.look; app.lookDrag = true; sappcmd::lockMouse(app.look); }
	if (!app.sim.active()) { return; }
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
	box_event(e);
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
			if (app.look || app.orbit || held("mouse_rotate"))	// the floating camera clamps the pitch (update_rts)
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
		"               [--list-mods] [--mod KEY=on|off]... [--save-mods] [--set KEY=VALUE]... [--print-defaults] [--check-options]\n");
	dev::usage();
	fprintf(stderr, "               [--debug]  (the development features: the F8 debug panel, the debug keys, the scenes, the debug script ops)\n"
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
static void shot_pending_frame()
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
		if (ec) { shot_toast("Screenshot failed: could not write " + base + ".png"); }
		else
		{
		// The sidecars describe the frame in the PNG (kenshi-k16.2.205.12.11): written here, at the request, from this frame's state; the readback's
		// done runs a frame later under the render thread (overlap), when the camera, selection and sim have moved on. A failed read removes them.
		dev::shotSidecars(dev_context(), base + ".png");	// F12: .txt, .events.txt, .trails.tsv (dev/shotsidecars.cpp)
#ifdef KCLIENT_DEV_TOOLS
		shot_route(base + ".png", app.frames);	// F12: <shot>.route.tsv, the selected characters' routes and last move orders (--debug)
#endif
		dev::shotTreesBegin(dev_context(), base + ".png");	// the on-screen characters' trees (this frame's camera), beside the shot (docs/CLIENT.md "Screenshot debug trees")
		printf("shot request: frame %d eye %.1f %.1f %.1f yaw %.1f pitch %.1f (the .txt's camera line is this frame's)\n", app.frames, app.pos[0], app.pos[1], app.pos[2], app.yaw * 180 / 3.14159265358979, app.pitch * 180 / 3.14159265358979);
		readback::screenshot(base + ".png", [base](bool ok) {
		if (ok)
		{
			dev::shotBegin(base + ".png");		// a --bead run: the shot also goes to the item's findings folder and as a comment (k16.2.127)
		}
		else
		{
			for (const char* ext : { ".txt", ".events.txt", ".trails.tsv", ".route.tsv", ".trees.txt" }) { std::error_code rec; std::filesystem::remove(base + ext, rec); }	// no picture: no sidecars
			shot_toast("Screenshot failed: could not write " + base + ".png");	// the pixels were not read: no file to wait for
		}
		}, [base, name = std::string(name)](bool ok, const std::string& why) {	// the writer's result (kenshi-k16.2.312): the toast follows the file
			shot_toast(ok ? "Screenshot saved: " + name + ".png" : "Screenshot failed: could not write " + base + ".png: " + why);	// the folder is screenshot.dir (the path alone wraps the toast)
		});
		}
	}
}

}

sapp_desc sokol_main(int argc, char* argv[])
{
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
#ifdef KENSHI_EMBED_SERVER
	// The combined binary is what the preview ships (kenshi-379.13): with no options it behaves as `play.sh title`, --local-server included,
	// so a scripted or headless run shows the title screen and starts its own server too. --debug keeps the offline fly-around, --simlink
	// keeps joining a given server.
	if (!o.debug && o.simlink.empty()) { o.localServer = true; }
#endif
	// Game data without a flag (kenshi-379.13): --data, $KENSHI_DATA, the saved setting, then the Steam/GOG installs (datadir.cpp).
	{
		const std::string given = o.data;
		std::string why;
		if (!datadir::resolve(given, o.data, why)) { fprintf(stderr, "kclient: %s", why.c_str()); exit(1); }
	}
	// The run's critical-events file, kclient/events beside the screenshots (kept per line: a crash loses nothing; the events of the
	// argument parsing went to the ring and are written into it here). Its first line is the build (k16.2.127): the sim's is this same
	// checkout when the client starts the server itself, else it arrives with the wire ("?").
	evlog::open((shot_dir().parent_path() / "events").string(),
		"build: client " + std::string(kclient_git_hash()) + " sim " + std::string(o.localServer ? kclient_git_hash() : "?"));
	ui::keys::setDebug(o.debug);	// --debug: the debug keys, the panel and the Options' Debug sections are live
	if (!o.debug && o.simspeed > 0) { fprintf(stderr, "kclient: --simspeed needs --debug (a measurement flag, kenshi-k16.2.205.7)\n"); exit(2); }
	app.mods.discover(o.data);
	Post::retireOptions();	// the retired post.* keys (k16.2.82): cfg::load() warns for any a cfg file still sets
	app.mods.loadConfig();	// also cfg::load()
	for (const std::string& kv : setArgs)
	{
		if (!cfg::set(kv)) { fprintf(stderr, "kclient: bad --set '%s' (want KEY=VALUE for a known option)\n", kv.c_str()); exit(2); }
	}
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
