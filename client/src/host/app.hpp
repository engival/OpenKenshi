#pragma once

// The host: the App state main.cpp and the host/ slices share (kenshi-k16.2.369.3; the slice plan on kenshi-k16.2.369.2).

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sokol_app.h"
#include "sokol_gfx.h"

#include "frame/loadscreen.hpp"
#include "platform/localserver.hpp"
#include "app/opencont.hpp"
#include "assets/materials.hpp"
#include "assets/mods.hpp"
#include "assets/resources.hpp"
#include "characters/characters.hpp"
#include "characters/creator.hpp"
#include "characters/creatorscene.hpp"
#include "characters/physics.hpp"
#include "characters/portraits.hpp"
#include "bridge/ragdolldrive.hpp"
#include "debug/debugview.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#include "input/collisionpick.hpp"
#include "input/cursors.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/collision.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/ogreskeleton.hpp"	// SkelQuat (the creator's preview orbit)
#include "net/simlink.hpp"
#include "bridge/simview.hpp"
#include "render/arrows.hpp"
#include "frame/beamfield.hpp"
#include "render/blooddecals.hpp"
#include "render/detail.hpp"
#include "render/drawlist.hpp"
#include "render/dust.hpp"
#include "render/fogvolume.hpp"
#include "world/foliage.hpp"
#include "render/gputimer.hpp"
#include "world/grass.hpp"
#include "render/hdr.hpp"
#include "render/idbuffer.hpp"
#include "render/lightvol.hpp"
#include "render/particles.hpp"
#include "world/particleworld.hpp"
#include "render/post.hpp"
#include "render/reflection.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "render/sunshafts.hpp"
#include "render/terrain.hpp"
#include "frame/trails.hpp"
#include "render/water.hpp"
#include "render/weff.hpp"
#include "ui/advanced.hpp"
#include "ui/bubbles.hpp"
#include "ui/build.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/crafting.hpp"
#include "ui/creator.hpp"
#include "ui/dev/debugtree.hpp"
#include "ui/dev/lightspanel.hpp"
#include "ui/dialogue.hpp"
#include "ui/faction.hpp"
#include "ui/hud.hpp"
#include "ui/itemwindows.hpp"
#include "ui/menus.hpp"
#include "ui/namelabels.hpp"
#include "ui/newgame.hpp"
#include "ui/options.hpp"
#include "ui/minimap.hpp"
#include "ui/statuspopup.hpp"
#include "ui/overview.hpp"
#include "ui/progressbars.hpp"
#include "ui/prompt.hpp"
#include "ui/prospecting.hpp"
#include "ui/research.hpp"
#include "ui/savepanel.hpp"
#include "ui/squads.hpp"
#include "ui/stats.hpp"
#include "ui/tutorials.hpp"
#include "world/buildings.hpp"
#include "world/clientnav.hpp"
#include "world/doorpose.hpp"
#include "world/squads.hpp"
#include "world/towns.hpp"
#include "world/weatherinstances.hpp"
#include "render/weathermap.hpp"
#include "world/worldbuildings.hpp"
#include "world/worldmap.hpp"

namespace host
{

constexpr float kFogDistance = 60000.0f;
// The camera's default position (kenshi-k16.2.112.1): the Hub's centre (the Hub scenarios' `source centre=`), never the world origin, which is the
// Deadlands — the user's "loading into deadlands should be considered a spoiler" (2026-11-12). --pos / --at / --camera-pos still win.
constexpr double kHubDefaultX = -50619.0, kHubDefaultZ = -1086.0;

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
	// The world map's view the save was made at (k16.2.435, a deliberate deviation): zoom in pixels a map texel (0 = fitted), the world point
	// at the view's centre. An older sidecar has none: its load starts at the default overview.
	bool hasMapView = false;
	double mapZoom = 0, mapCx = 0, mapCz = 0;
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
	ui::MinimapState minimapState;	// what the host passes the minimap each frame (level / place from the saved settings, then its actions)
	std::unique_ptr<ui::Minimap> minimap;	// the corner minimap window (ui.minimap; place and zoom saved as minimap.x / .y / .level)
	std::unique_ptr<ui::StatusPopup> statusPopup;	// the top-right status lines (server not responding, link lost; kenshi-k16.2.445.3)
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
	struct DoorPoseNow { uint64_t thing = 0; double open = 0, off[3] = {}; bool exact = true, seen = false, drawn = false; };	// drawn: the leaf was in the gathered world a frame earlier (its first state then eases from the authored pose; kenshi-k16.2.258.10.1)
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
	float creatorHead = 17;	// Bip01 Head above the preview's feet (Characters::headHeight; the FACE / HAIR pivot until the body is built)
	// The creator preview's orbit (kenshi-k16.2.433.1, user 2026-11-20; main.cpp creator_frame / creator_drag / creator_zoom): the character and the
	// platform are one assembly, drawn as p' = center + q x (p - center) + shift (Characters::setPreviewOrbit, WorldBuildings::setEditor), so a drag
	// turns both about the pivot: yaw about +Y from its x, tilt about +X from its y, clamped short of flipping. The wheel moves the eye along the
	// cursor's ray to the point of the model under it (Characters::skeletonRay, so that point stays put) and makes it the pivot; over the last 30% of
	// the way out the pivot, the tilt and the eye drift back to the page's default framing of the whole character (the yaw stays).
	struct CreatorOrbit
	{
		kworld::SkelQuat q;		// the assembly's rotation from its home pose (identity: as the scene builds it)
		double shift[3] = {};		// the scene pivot's world displacement (the assembly's home is the scene itself)
		double pivot[3] = {};		// the rotation point (world): a drag turns the assembly about it
		double eye[3] = {};		// the camera's eye
		float yaw = 0, tilt = 0;	// radians: the drag's yaw about +Y and tilt about +X (positive: the top towards the camera)
		double zoom = 1;		// 1 = the page's default framing (full out), 0 = the closest
		int page = -1;			// the creator page the framing was made for; a page change starts it over
		// The SHOWN view (k16.2.433.4): a wheel step sets the fields above as the TARGET; these glide towards it each frame (exponential, dt-based),
		// and the camera and the assembly draw from these. A drag snaps the target to them first (it acts on the shown view and ends the glide).
		double sEye[3] = {}, sShift[3] = {};
		float sYaw = 0, sTilt = 0;
		double sZoom = 1;
		bool gliding = false;
		double tCursor[3] = {};	// the step's tracked point (target world) and the pointer it was taken at, for the settled check line
		float curX = 0, curY = 0;
		bool track = false;
	};
	CreatorOrbit creatorOrbit;
	bool creatorDragging = false;	// a left button held from the preview area (creator_event)
	float creatorDragX = 0, creatorDragY = 0;	// the drag's last pointer position
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
	bool sessionShown = false;	// a session's world has been shown once (end_session_load): until then the client's start is covered by the load screen's rule (kenshi-k16.2.437.4.4)
	bool offlineGateDone = false;	// the offline flight's load screen was begun (or not wanted): once per run (kenshi-k16.2.258.3)
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
	bool hoverHitOk = false;	// the cursor is over ground (hover_cursor's pick) and hoverHit is its point (the debug route preview)
	double hoverHit[3] = {};
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
	double followDist = 100, followBase = 0, followOffset = 0;	// camera distance (kFollowDist, kenshi-k16.2.353.4); smoothed camera yaw behind the character; the user's orbit on top
	double followEase = 1e9, followBoom = 0, followRise = 0, followPitchAdj = 0, followRide = 0, rtsEase = 1e9, rtsBoom = 0, rtsRide = 0;
	unsigned followSnaps = 0, rtsSnaps = 0;	// the safety snap-ins (the direction window let the view's own allowed length through; kenshi-k16.2.353.4)
	bool rtsLog = false;	// script `rtsboom log`: update_rts prints its boom each frame (the eased length above: the direction window's value, kenshi-k16.2.353.4)
	double followPivotY = 0;	// kenshi-k16.2.395.1: the follow camera's boom pivot y (the smoothed drawn body y + kEye), lightly smoothed itself; followPivotOn = one follow frame has set it (an engage snaps it)
	bool followPivotOn = false;
	// kenshi-k16.2.474.1: the view's horizontal anchor slack. A wire correction of the followed / centred body (a shove, a catch-up) steps its drawn x, z; the part of a
	// frame's step past what the body can walk is kept here (resid) and decays, so the view glides over it. slackId = the subject it belongs to (a change snaps).
	uint64_t slackId = 0;
	bool slackOn = false;
	double slackLast[2] = {}, slackResid[2] = {}, slackMotion = 0;	// slackMotion: last frame's step that counted as the body's own motion (u)
	// The game camera (docs/CLIENT.md "Camera modes"): floating over the ground at rtsDist from focus, on yaw / pitch; centerId != 0 = auto-center on
	// that character. Off = the debug free camera; follow (the hover / drive camera) overrides both.
	bool rts = false;
	uint64_t centerId = 0;
	bool centerOnStart = false;	// a session start: auto-center on the first selected character once there is one (user 2026-11-06)
	double focus[3] = {}, rtsDist = 400;
	const char* focusOn = "terrain";
	CamRestore camRestore;	// the camera sidecar of the save being loaded (k16.2.134.1)
	// The world map's default view (k16.2.435): a new game, or a load whose sidecar has no map view, starts at the overview once the MAP page has a
	// rect (its height is the default's zoom) and the towns are in; the sidecar's own view lands with the camera and clears this.
	bool mapViewPending = true;
	uint32_t cameraStartUsed = 0;	// the decoder's cameraStartSerial the session start has applied (CAMERA_START, kenshi-cym.40.2.3)
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
	double propsThingY = 0;	// the sum of the items' and containers' drawn y at the last props build (k16.2.412.4)
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
	double windSpeedForce = -1;	// `set windspeed`: the wind speed (u/s) a scene or script forces, -1: the live one
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
};

extern App app;

}
