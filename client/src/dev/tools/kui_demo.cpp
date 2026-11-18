// kui_demo: every UI widget on one screen, drawn with the game's skins and fonts (docs/UI.md). Interactive, or --screenshot for a headless check:
//   client/tools/headless.sh -W 1920 -H 1080 -- client/build/kui_demo --screenshot /tmp/kui.png
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <unistd.h>
#include <fstream>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <string>
#include <vector>

#include "kutil/executable.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_time.h"
#include "stb_image_write.h"
#include "assets/dds.hpp"
#include "render/drawlist_gpu.hpp"
#include "app/config.hpp"
#include "net/simlink.hpp"
#include "ui/bubbles.hpp"
#include "ui/diamond.hpp"
#include "ui/progressbars.hpp"
#include "ui/namelabels.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/creator.hpp"
#include "ui/stats.hpp"
#include "ui/prompt.hpp"
#include "ui/dev/debugpanel.hpp"
#include "ui/crafting.hpp"
#include "ui/faction.hpp"
#include "ui/pagekit.hpp"
#include "ui/research.hpp"
#include "ui/squads.hpp"
#include "ui/tutorials.hpp"
#include "ui/prospecting.hpp"
#include "ui/build.hpp"
#include "ui/trade.hpp"
#include "ui/dialogue.hpp"
#include "ui/gear.hpp"
#include "ui/itemwindows.hpp"
#include "ui/icons.hpp"
#include "ui/hud.hpp"
#include "ui/keys.hpp"
#include "ui/menus.hpp"
#include "ui/newgame.hpp"
#include "ui/advanced.hpp"
#include "ui/dev/debugtree.hpp"
#include "ui/dev/trailspanel.hpp"
#include "ui/dev/lightspanel.hpp"
#include "ui/dev/animbrowser.hpp"
#include "ui/dev/notes.hpp"
#include "ui/dev/scenarios.hpp"
#include "ui/layout.hpp"
#include "ui/options.hpp"
#include "ui/overview.hpp"
#include "ui/savepanel.hpp"
#include "ui/statgrid.hpp"
#include "ui/ui.hpp"

#if defined(SOKOL_GLCORE)
#include <GL/gl.h>
#endif

namespace
{

// One option of each type for the options window (only ui.scale is declared otherwise in this demo).
const cfg::Int kVolume{ cfg::Group{ "kui_demo" }, "demo.volume", 70, 0, 100, "A volume-style percentage" };
const cfg::Float kGamma{ cfg::Group{ "kui_demo" }, "demo.gamma", 1.0, 0.5, 2.0, "A float option with a range" };
const cfg::Bool kSubtitles{ cfg::Group{ "kui_demo" }, "demo.subtitles", false, "A tick box option" };
const cfg::Text kName{ cfg::Group{ "kui_demo" }, "demo.name", "Wanderer", "A text option (an edit box)" };
// A copy of the host's ui.progress_bars (main.cpp kProgressBars, h2v.16.2.1; kui_demo doesn't link main.cpp) for the Interface row check (k16.3.15).
// Keep the key, values and default in step with main.cpp.
const cfg::Text kProgressBars{ cfg::Group{ "HUD and labels" }, "ui.progress_bars", "original", { "off", "original", "squad", "everyone" },
	"Progress bars over working characters: off none, original the original's own bars (lockpicking, cutting, ...), squad every task of your characters, everyone those plus the NPCs in view" };
// A copy of the host's window.remember (windowgeom.cpp kRemember, k16.2.109.1; kui_demo doesn't link it) for the Graphics row check (k16.2.109.2).
// Keep the key, default and help in step with windowgeom.cpp.
const cfg::Bool kWindowRemember{ cfg::Group{ "Windows" }, "window.remember", true, "Remember the game window's position, size and fullscreen state per monitor layout and restore them at the next start (X11; best effort under Wayland)" };
// Stand-ins for the host's sun shadow options (worldbuildings.cpp, main.cpp, shadow.cpp; kui_demo links none) for the options find check
// (k16.2.227.1): a key, a key and help, help only. Our own help text, the keys and types as the host's.
const cfg::Bool kDemoToggle{ cfg::Group{ "Indoor lighting (experimental)" }, "render.demo_shadow_toggle", true, "Demo stand-in: a tick other rows need" };
const cfg::Float kShadowSoft{ cfg::Group{ "Indoor lighting (experimental)", "needs render.demo_shadow_toggle" }, "render.shadow_soft", 0.0, 0.0, 10.0, "Demo stand-in: soft shadow edges, the sun's half angle" };
const cfg::Bool kSunSmooth{ cfg::Group{ "Indoor lighting (experimental)" }, "render.sun_smooth", true, "Demo stand-in: the sun's shadow map follows the sun smoothly" };
const cfg::Float kSunBounce{ cfg::Group{ "Indoor lighting (experimental)" }, "render.sun_bounce", 0.0, 0.0, 1.0, "Demo stand-in: light bounced off the lit ground" };

struct Demo
{
	std::string data = KENSHI_DATA;	// CMake KENSHI_DATA
	std::vector<std::string> themes;
	std::string shot;
	int w = 1600, h = 900;
	int frame = 0;
	uint64_t last = 0;
	ui::TextLog* log = nullptr;
	ui::Widget* hoverMe = nullptr;
	ui::TickBox* jobs = nullptr;
	ui::List* choices = nullptr;
	bool failed = false;
	ui::HudState hs;
	std::unique_ptr<ui::Hud> hud;
	std::unique_ptr<ui::Options> opts;
	std::unique_ptr<ui::Creator> cr;
	ui::CreatorState cs;
	std::string crLog;	// the creator actions asked
	float rotated = 0;
	bool showCreator = false;	// --show creator: the screenshot ends on the character creator
	std::unique_ptr<ui::Gear> gear;
	ui::GearState gs;
	std::string gearLog;	// the grid inventory actions asked
	uint32_t gearCount = 0;	// the last gear move's carried count (GridItem::count of the dropped copy)
	std::string sounds;	// ui::sound posts, "Event;" or "Event(Group=Value);" (k16.3.17)
	ui::GearState gsKept;	// the gear state while the container kinds are checked
	std::unique_ptr<ui::ContainerWindow> cont;	// the gear's container, docked as ui::ItemWindows docks one (kenshi-k16.20.13.1.3)
	ui::ContainerState ccs, ccsKept;	// its state; its grid's items live in gs.grids[kGearContainer] (gearMove's store), copied by demoContainer()
	std::unique_ptr<ui::Gear> lootGear;	// a looted body's, docked right of gear (--show gearbody)
	ui::GearState lgs;
	std::unique_ptr<ui::ItemWindows> items;	// several item windows at once (--show items, kenshi-k16.20.13.1.1)
	ui::ItemWindowsState iws;
	std::string itemsLog;
	ui::RightVerb itemsVerb = ui::RightVerb::kNone;	// the host's answer to ItemWindowsActions::verb (kenshi-k16.20.13.1.2)
	bool showItems = false;	// --show items: a character, a box and a trader at once (frames 119-126), the screenshot at 127
	bool showDialogue = false;	// --show dialogue: the screenshot ends on a conversation, as in the in-game capture (kenshi-inspect darkui_dialogue_0943)
	bool showZone = false;	// --show zone: the screenshot ends on the zone banner, as in the capture (kenshi-inspect darkui_zone_toast_0958a)
	bool showGear = false;	// --show gear: the screenshot ends on the grid inventory with the backpack and robotics open
	int showTip = 0;	// --show food | weapon | blueprint | armour: as gear, ending on that item's tooltip (1, 2, 3, 4)
	std::unique_ptr<ui::Stats> stats;
	ui::StatsState ss;
	int statsClosed = 0;
	std::unique_ptr<ui::Overview> overview;
	std::string overviewLog;	// OverviewActions / MapActions calls
	ui::MapState map;	// the overview map's stand-in markers
	int mapDraws = 0;
	bool showOverview = false;	// --show overview: the screenshot ends on its MAP page
	bool mapSweep = false;	// --map-sweep: print the map's zoom sweep, a row a notch (k16.2.201.1)
	int mapNotches = -1;	// --map-notches N, with --show overview: the map fitted, then N wheel notches in about the hub (shots at a zoom)
	std::unique_ptr<ui::Squads> squads;
	ui::SquadsState sq;
	std::string sqLog;	// the squad actions asked
	int sqNext = 3;	// the host's running squad number ("Nameless_3"), never reused
	bool showSquads = false;	// --show squads: the screenshot ends on the squads window
	std::unique_ptr<ui::Research> research;
	ui::ResearchState rs;
	std::string rsLog;	// the research actions asked
	std::unique_ptr<ui::Crafting> crafting;
	ui::CraftingState cs2;
	std::string csLog;	// the crafting actions asked
	bool showCrafting = false;	// --show crafting: the crafting checks run (frames 119-124), the screenshot at 125
	std::unique_ptr<ui::Faction> faction;
	ui::FactionState fs;
	std::string fsLog;	// the faction actions asked
	float fsY = 0;	// a relation row's top before the wheel
	std::unique_ptr<ui::BuildingWindow> building;
	ui::BuildingWindowState bws;
	std::string bwLog;	// the building window's actions
	float bwW = 0;	// the refinery window's width
	bool showGearBody = false;	// --show gearbody: a looted body beside the looter (frames 119-126), the screenshot at 127
	bool showStatsMock = false;	// --show statsmock: the stats table mockup (frames 64-66, after the grid drags), over the gallery; at 130 again and in the HUD's place, the screenshot at 135
	ui::StatGrid* statsMock[3] = {};
	ui::StatGrid* statsText[3] = {};	// the same three with text labels
	int showTag = 0;	// --show tag-NAME: the status tag row's case (kTagCases), checked at 120, its tip hovered, the screenshot at 127
	bool showHudBuilding = false;	// --show hudbuilding: the HUD's building panel checks run (frames 119-124), the screenshot at 125
	std::string hbLog;	// HudActions::buildingButton / buildingTick calls
	bool showBuilding = false;	// --show building: the building window checks run (frames 119-124), the screenshot at 125
	int buildingShot = 2;	// the case on the screenshot: the bench; --show mine: the drill (kenshi-k16.19.8.1)
	bool showFaction = false;	// --show faction: the faction checks run (frames 119-124), the screenshot at 125
	bool showResearch = false;	// --show research: the research checks run (frames 119-123), the screenshot at 125
	std::unique_ptr<ui::SavePanel> saves;
	std::unique_ptr<ui::MainMenu> mainMenu;
	std::unique_ptr<ui::GameMenu> gameMenu;
	std::unique_ptr<ui::NewGamePanel> newGame;
	std::vector<ui::StartEntry> starts;	// the new game panel's
	std::unique_ptr<ui::Tutorials> tutorials;
	std::unique_ptr<ui::Prospecting> prospect;
	std::unique_ptr<ui::DebugPanel> debug;
	std::unique_ptr<ui::DebugTree> tree;
	ui::DebugTreeState ts;
	std::string treeLog, treeCopy;
	std::unique_ptr<ui::AnimBrowser> anim;
	ui::AnimBrowserState as;
	std::string animLog, animCopy;
	int animW = 0, animH = 0;	// the last viewSize
	std::unique_ptr<ui::ScenariosWindow> scen;
	ui::ScenariosState scs;
	std::unique_ptr<ui::NotesWindow> notes;
	ui::NotesState ns;
	std::string playLog;
	bool showSpectator = false;	// --show spectator: the spectator HUD and CAST window's checks (frames 119-121), the screenshot at 125
	bool showDriving = false;	// --show driving: the HUD's diamonds left in the direct drive state (HudState::driving) for the screenshot
	std::string specLog;	// HudActions::watch / follow calls
	bool showRaise = false;	// --show raise: the raise-on-press test windows left up, the lower one raised by its button (frame 72), the screenshot at 73
	bool showScenarios = false, showNotes = false;	// --show scenarios / notes: the playtest windows' checks (frames 119-121), the screenshot at 125
	bool showAnim = false;	// --show anim: the animation browser checks (frames 119-121), the screenshot at 125
	bool showTree = false;	// --show debugtree: the DEBUG VIEW tree checks (frames 119-120), the screenshot at 125
	std::unique_ptr<ui::TrailsPanel> trails;
	ui::TrailsPanelState trs;
	std::string trailsLog;
	bool showTrails = false;	// --show trails: the TRAILS panel checks (frames 119-120), the screenshot at 125
	std::unique_ptr<ui::LightsPanel> lights;
	ui::LightsPanelState ls;
	std::string lightsLog;
	bool showLights = false;	// --show lights: the LIGHTS window checks (frames 119-120), the screenshot at 125
	int showToast = 0;	// --show toast (1, over a busy light background) / toast-text (2, over lines of debug text): fresh toasts at 100, the screenshot at 125
	int showHints = 0;	// --show hints (1, over a light background) / hints-dark (2): the control hints panel on by F6 (frames 119-120), the screenshot at 125
	ui::DebugState dbg;
	std::string debugLog;
	bool showDebug = false;	// --show debug: the debug panel checks run (frames 119-122), the screenshot at 125
	std::unique_ptr<ui::AdvancedOptions> advanced;
	std::vector<ui::AdvOption> adv;	// the advanced options' stand-in rows (the host keeps the real values)
	std::unique_ptr<ui::Build> build;
	ui::BuildState bs;
	ui::ProspectState ps;
	bool showBuild = false;	// --show build: the build window checks run (frames 119-123), the screenshot at 125
	bool showAdvanced = false;	// --show advanced: the advanced options checks run (frames 118-122), the screenshot at 125
	bool showProspect = false;	// --show prospect: the prospecting checks run (frames 119-122), the screenshot at 125
	bool showContext = false;	// --show context: a hold-RMB menu open at frame 71, the row under the pointer hot, for the screenshot at 73
	bool showJobs = false;	// --show jobs: the jobs checks run (frames 119-124), the screenshot at 125
	bool showTutorial = false;	// --show tutorial: the tutorial checks run (frames 111-119), the screenshot at 125
	std::string menuLog;	// the menus' and save panel's actions
	std::vector<ui::Widget*> underTitle;	// the screens hidden while the title shows
	int showMenu = 0;	// --show mainmenu (1) / gamemenu (2) / load (3) / save (4) / newgame (5) / mainmenu-pending (6) / gamemenu-exit (7): the menu checks run (frames 87-104), the screenshot at 110
	bool showMessage = false;	// --show message: the screenshot ends on the HUD message box
	bool showTrade = false;	// --show trade: the screenshot ends on the trade windows (a 40-row trader, scrolled), a trader item's tooltip shown
	std::unique_ptr<ui::Prompt> prompt;
	std::string promptLog;	// the answers given
	std::string shortcutLog;	// HudActions::shortcut calls
	std::string extendLog;	// HudActions::extend calls
	double extendAt = -1;	// when the status panel's button was clicked; the end of the slide is checked once it has had time
	bool showPrompt = false;	// --show prompt: the screenshot ends on the fence prompt
	std::string layoutDir;	// client/data
	std::string optsTab = "Interface", optsRow = "ui.progress_bars";	// --options-tab TITLE --options-row KEY: the tab and row the options shot hovers
	std::string optsSearch;	// --options-search TEXT: the options shot on that find's match
	int optsHit = 1;	// --options-hit N: its Nth match (1-based)
	bool showOptions = false;	// --show options: the screenshot ends on the options window
	bool showStats = false;	// --show stats: the screenshot ends on the stats window, a skill's tooltip shown
	std::unique_ptr<ui::Dialogue> talk;
	std::unique_ptr<ui::SpeechBubbles> bubbles;
	std::unique_ptr<ui::ProgressBars> progress;
	float hurtA = -1;	// the Hurt background's alpha at frame 2 (the pulse check)
	std::unique_ptr<ui::NameLabels> names;
	bool namesOn = true;
	ui::DialogueState ds;
	int node = 0;
} demo;

// Squads as the capture has them (darkui_squads_0926: Expedition and Town, two each).
void demoSquads()
{
	demo.sq = { true, { { 1, "Expedition", { { 100, "Beep" }, { 101, "Ruka" } } }, { 2, "Town", { { 102, "Greenlander" }, { 103, "Burn" } } } }, 4, 30, 10 };
	demo.sq.squads[0].members[0].back = ui::PortraitBack::kCombat;
	demo.sq.squads[0].members[1].back = ui::PortraitBack::kHunger;
	demo.sq.squads[0].members[1].hurt = true;
	demo.sq.squads[1].members[0].mark = ui::PortraitMark::kSlave;	// wears shackles, carried: dimmed
	demo.sq.squads[1].members[0].dim = true;
	demo.sq.squads[1].members[1].mark = ui::PortraitMark::kPrison;	// caged
	demo.sqNext = 3;
}

// A creator state as the host fills it from Characters::Spec and the race records; the actions change it as the host would.
void demoCreator()
{
	ui::CreatorState& c = demo.cs;
	c.valid = true;
	c.name = "Wanderer";
	c.groups = { { "Human", { "Greenlander", "Scorchlander" } }, { "Shek", { "Shek" } }, { "Hive", { "Prince", "Soldier Drone", "Worker Drone" } },
		{ "Skeleton", { "Skeleton" } } };
	c.characters = 6;
	c.description = "Greenlanders are the descendants of the old empire's farming caste: adaptable, unremarkable, and found wherever there is water.";
	c.stats = { { "Hit Points", "100" }, { "Farming", "+20%" }, { "Science", "+10%" }, { "Labouring", "-20%" }, { "No bleeding", "" } };
	c.heads = 7;
	c.hairs = { "", "Short", "Long", "Mohawk" };
	c.beards = { "", "Stubble", "Full" };
	c.hairColours = { "Black", "Brown", "Grey" };
	c.sliders = { { "Body", "Idle stance", 0, 3, 0, 1 }, { "Body", "Skin Tone", 0, 100, 50 } };
	for (const char* n : { "Jaw width", "Chin length", "Nose size", "Eye spacing", "Brow height", "Cheekbones" }) { c.sliders.push_back({ "Face", n, -1, 1, 0 }); }
	for (const char* n : { "Muscle", "Skinny", "Strong" }) { c.sliders.push_back({ "Body", n, 0, 1, 0.5f }); }
	c.sliders.push_back({ "Proportions", "Arm length", 0.8f, 1.2f, 1 });
}

// A three-node conversation standing in for the dialogue data: what the host would do with DIALOGUE / DIALOGUE_LINE records.
// The grid inventory: real icons from the game's cache and record files; items the cache has no picture for show their names.
// Kinds as the host would pass them: ARMOUR slot numbers, 1 a weapon, 100 a limb, 0 anything else.
// A stats state as a host would fill it: the original's grouping and order (kenshi-inspect findings/ui_captures/stats_window.md); the texts are
// stand-ins of our own (the host takes them from the records where they exist).
void demoStats()
{
	ui::StatsState& s = demo.ss;
	s = {};
	s.valid = true;
	s.name = "ENGIVAL";
	s.attributes = { { "STRENGTH", 86, 0.4f }, { "TOUGHNESS", 52, 0.8f }, { "DEXTERITY", 20, 0.15f, 18 }, { "PERCEPTION", 1, 0.05f } };
	s.skills = {
		{ "Weapons", 0, { { "Katanas", 1, 0, -1, { "Damage with katanas" }, { "Fighting with a katana" },
			"Long single-edged blades. Quick and good at cutting; poor against heavy armour. This text is a stand-in, long enough to show the wrapping." },
			{ "Sabres", 4 }, { "Hackers", 1 }, { "Heavy weapons", 1 }, { "Blunt", 1 }, { "Polearms", 1 } } },
		{ "Combat", 1, { { "Melee attack", 5 }, { "Melee defence", 42 }, { "Dodge", 35, 0.6f, 38 }, { "Martial arts", 20 } } },
		{ "Ranged", 1, { { "Turrets", 1 }, { "Crossbows", 1 }, { "Precision shooting", 1 } } },
		{ "Thievery", 2, { { "Stealth", 94 }, { "Lockpicking", 42, 0.85f, -1, { "Chance to pick a lock", "Time a lock takes" }, { "Picking locks", "Training equipment" },
			"Opening locks without the key. A stand-in description." }, { "Thievery", 1 }, { "Assassination", 44 } } },
		{ "Athletics", 2, { { "Athletics", 88 }, { "Swimming", 16 } } },
		{ "Sciences", 3, { { "Field medic", 22 }, { "Engineer", 43 }, { "Robotics", 4 }, { "Science", 6 } } },
		{ "Trades", 3, { { "Weapon smith", 1 }, { "Armour smith", 1 }, { "Crossbow smith", 1 }, { "Labouring", 86 }, { "Farming", 1 }, { "Cooking", 1 } } } };
	s.derived = "Hunger\tFull\nCarry weight\t19 / 101\nRun speed\t21.5\nBlood\t100 / 100\nDamage resistance\t{StatUp}12%\n\n"
		"Our own section (the original clips it off); the numbers are the host's, and this line shows that long text wraps.";
}

// The HUD's panel for a selected building, after kinspect hud_building.md: 0 a mine (computed quality ink, output full), 1 an upgradable
// processor, 2 a generator (yellow bars, no workers), 3 one under construction (material slats, paused). Our own stand-in names.
ui::HudBuildingInfo hudBuildingCase(int c)
{
	ui::HudBuildingInfo b;
	b.valid = true;
	b.id = 60 + c;
	b.name = c == 0 ? "Ore Drill" : c == 1 ? "Stone Processor II" : c == 2 ? "Fuel Generator" : "Small Shack";
	b.faction = "Wanderers";
	if (c != 3) { b.state = { { "Town:", "Dusty Hollow" } }; }	// none in the wilds
	const ui::HudBuildingButton dismantle = { "dismantle", "Dismantle", "BadBright", "Take this building down? Anything inside is lost." };
	if (c == 0)
	{
		b.status = "Output full";
		b.statusColour = "HudBad";
		b.stats = { { "Iron Quality:", "80", "#00a31a" }, { "Workers:", "0/2", "HudBad" }, { "Efficiency:", "0%", "HudBad" }, { "Max efficiency:", "100%" } };
		b.ticks = { { "battery", "Battery", false }, { "power", "Power", true } };
		b.bars = { { "Condition", "", "25", 1 }, { "Power", "Inactive", "0", 0 } };
	}
	else if (c == 1)
	{
		b.status = "No workers";
		b.statusColour = "HudBad";
		b.buttons = { { "upgrade", "[UPGRADE]", "GoodBright" }, { "downgrade", "[DOWNGRADE]" } };
		b.stats = { { "Workers:", "1/2" }, { "Efficiency:", "45%", "HudBad" }, { "Max efficiency:", "60%" } };
		b.ticks = { { "battery", "Battery", true }, { "power", "Power", true } };
		b.bars = { { "Condition", "", "15", 0.6f }, { "Power", "", "12", 0.5f } };
	}
	else if (c == 2)
	{
		b.status = "Out of fuel";
		b.statusColour = "HudBad";
		b.stats = { { "Workers:", "-" }, { "Efficiency:", "0%", "HudBad" }, { "Max efficiency:", "100%" } };
		b.ticks = { { "power", "Power", true } };	// a generator: no battery tick
		b.bars = { { "Condition", "", "40", 1 }, { "Power Output", "", "0", 0, "BarLayerYellow" }, { "Fuel Use", "", "0", 0, "BarLayerYellow" } };
	}
	else
	{
		b.status = "Under construction";
		b.buttons = { { "pause", "PAUSE CONSTRUCTION" }, { "", "[PAUSED]", "GreyedBright", "", true } };
		b.bars = { { "Condition", "", "12", 0.3f }, { "Building Materials", "", "3/10", 0.3f }, { "Iron Plates", "", "0/4", 0 } };
	}
	b.buttons.push_back(dismantle);
	return b;
}

// The status panel's tag row, one case per branch of the original's chain (kinspect hud_panel16.md (5), hud16_static.md 5), in --show tag-NAME.
// The tags are the original's labels; the hover texts are our own stand-in wording, the factions our own.
const char* const kTagCases[] = { "", "warleader", "leader", "kidnapped", "diplomatic", "slave", "escapedslave", "exslave", "crime", "escapedprisoner",
	"bounty", "bounty-other", "playerleader" };
const char* const kTagDrawn[] = { "", "War Leader", "Leader", "Kidnapped", "Diplomatic Status", "[SLAVE]", "[ESCAPED SLAVE]", "[EX-SLAVE]",
	"Committing Crime! (3)", "[ESCAPED PRISONER]", "Bounty: c.2,500", "Bounty: c.800", "" };

void hudTagCase(int c, ui::HudInfo& in)
{
	using L = ui::HudTipLine;
	const L hintGap = {};
	in.tagValue = in.tagValueColour = {};
	in.tagColour = "HudTag";
	const std::vector<L> wanted = { hintGap, { "Bounty:", "c.2,500", "HudTag", "HudNeutral" } };
	auto slave = [&](const char* tag, const char* who, bool seen, int looks, int hours, const char* hint, bool bounty)
	{
		in.tag = tag;
		in.tagColour = "HudBad";
		in.tagTip = { { who }, hintGap };
		if (looks >= 0)
		{
			in.tagTip.push_back({ "Situation:", seen ? "Seen!" : "Undiscovered", "", seen ? "HudBad" : "HudGood" });
			in.tagTip.push_back({ "Looks like a slave:", std::to_string(looks) + "%" });
			in.tagTip.push_back({ "Time remaining:", std::to_string(hours) + " hours" });
			in.tagTip.push_back(hintGap);
		}
		in.tagTip.push_back({ hint, "", "HudTag" });
		if (bounty) { in.tagTip.insert(in.tagTip.end(), wanted.begin(), wanted.end()); }
	};
	switch (c)
	{
	case 1: in.tag = "War Leader"; in.tagTip = { { "Commands a whole army on the march." } }; break;
	case 2: in.tag = "Leader"; in.tagTip = { { "Leads this group; the others follow their lead." } }; break;
	case 3: in.tag = "Kidnapped"; in.tagTip = { { "Carried off against their will. The law may step in to free them." } }; break;
	case 4: in.tag = "Diplomatic Status"; in.tagTip = { { "An envoy. Harming them sets their whole faction against you." } }; break;
	case 5: slave("[SLAVE]", "Property of the Sand Guild: owned, worked and never paid.", false, -1, 0, "They keep a fixed job while enslaved.", false); break;
	case 6: slave("[ESCAPED SLAVE]", "Ran from the Sand Guild and was spotted doing it. Lie low for a day.", true, 64, 18, "Keep the slave look down.", true); break;
	case 7: slave("[EX-SLAVE]", "Once the Sand Guild's. The hunt has cooled, but being recognised still costs.", false, 12, 0, "Keep the slave look down.", false); break;
	case 8:
		in.tag = "Committing Crime!";
		in.tagValue = "(3)";
		in.tagTip = { { "Theft in progress. Nobody must see it." } };
		break;
	case 9:
		in.tag = "[ESCAPED PRISONER]";
		in.tagTip = { { "Broke out of a cell. Guards who spot them will act; leave town to shake it." }, hintGap, { "Expires in:", "20 hours" } };
		break;
	case 10:
		in.tag = "Bounty:";
		in.tagValue = "c.2,500";
		in.tagValueColour = "HudNeutral";
		in.tagTip = { { "Sand Guild", "c.2,500", "", "HudNeutral" }, { "Iron Court", "c.800", "", "HudFaint" }, hintGap, { "Wanted for:", "Theft, Assault", "HudTag" },
			hintGap, { "Hand them over for the reward." }, { "Expires in:", "40 hours" } };
		break;
	case 11:
		in.tag = "Bounty:";
		in.tagValue = "c.800";
		in.tagValueColour = "HudFaint";
		in.tagTip = { { "Iron Court", "c.800", "", "HudFaint" }, hintGap, { "Wanted for:", "Assault", "HudTag" }, hintGap, { "Never expires.", "", "HudTag" },
			{ "Not wanted here.", "", "HudTag" } };
		break;
	default: in.tag.clear(); in.tagTip.clear(); break;	// 12 playerleader: the host's filter left no tag
	}
}

// Stand-in building windows of our own: 0 a refinery (one input), 1 a drill (output only), 2 a crafting bench, 3 a furnace.
ui::BuildingWindowState buildingCase(int c)
{
	auto item = [](uint32_t id, const char* name, int w, int h, int kind, uint32_t count)
	{
		ui::GridItem it;
		it.id = id;
		it.name = name;
		it.w = w;
		it.h = h;
		it.kind = kind;
		it.count = count;
		return it;
	};
	ui::BuildingWindowState s;
	s.valid = true;
	s.id = 50 + c;
	ui::BuildingSlot ore = { "Lump Ore", "", "", 3, 2, { 1 }, { item(7, "Lump Ore", 1, 1, 1, 4) }, 0.4f };
	ui::BuildingSlot plates = { "Metal Sheets", "", "", 4, 3, { 2 }, { item(8, "Metal Sheets", 2, 1, 2, 6) }, 0.75f };
	if (c == 0)
	{
		s.name = "Ore Smelter";
		s.inputs = { ore };
		s.output = plates;
	}
	else if (c == 1)
	{
		s.name = "Ore Drill";
		s.output = ore;
		s.output->bar = 0.35f;
	}
	else if (c == 2)
	{
		s.name = "Tinker Bench";
		s.inputs = { { "Wire", "OK", "HudGood", 2, 2, { 3 }, { item(9, "Wire", 1, 1, 3, 3) }, 0.5f }, { "Glass", "Empty!", "HudNeutral", 2, 2, { 4 }, {}, 0 } };
		s.output = ui::BuildingSlot{ "Gadget", "", "", 2, 2, { 5 }, {}, 0.2f };
		s.crafting = ui::BuildingCrafting{ "Gadget" };
	}
	else
	{
		s.name = "Melting Pot";
		s.inputs = { { "", "", "", 12, 7, {}, { item(10, "Scrap", 2, 2, 6, 1) }, 0 } };
		s.furnace = ui::BuildingFurnace{ "1.5", true, false };
	}
	return s;
}

void demoGear()
{
	enum { kItem = 0, kWeapon = 1, kHat = 3, kArmour = 5, kLegs = 6, kShirt = 8, kBoots = 9, kPack = 12, kBelt = 14, kLimb = 100 };
	auto item = [](uint32_t id, const char* name, int w, int h, std::string icon, int kind = kItem, uint32_t count = 1)
	{
		ui::GridItem it;
		it.id = id;
		it.name = name;
		it.w = w;
		it.h = h;
		it.icon = std::move(icon);
		it.kind = kind;
		it.count = count;
		return it;
	};
	auto at = [](ui::GridItem it, int x, int y) { it.x = x; it.y = y; return it; };
	auto charged = [](ui::GridItem it, float left) { it.bar = left; return it; };	// charges / durability: the white strip
	auto slot = [](int cols, int rows, int kind, std::vector<ui::GridItem> items = {})
	{
		ui::GearGridState g;
		g.cols = cols;
		g.rows = rows;
		g.single = true;
		g.accepts = { kind };
		g.items = std::move(items);
		return g;
	};
	ui::GearState& s = demo.gs;
	s = {};
	demo.ccs = {};
	s.valid = true;
	s.owner = 101;
	s.name = "ENGIVAL";
	s.bagName = "SMALL BACKPACK";
	using namespace ui;
	s.grids[kGearBackpack] = slot(5, 4, kPack, { item(1, "Small Backpack", 3, 3, icons::cached("576-gamedata.base", "686-gamedata.base"), kPack) });
	s.grids[kGearWeapon1] = slot(10, 2, kWeapon, { item(2, "Katana", 7, 1, icons::cached("476-gamedata.base", "1069-gamedata.base"), kWeapon) });
	s.grids[kGearWeapon2] = slot(7, 1, kWeapon);
	s.grids[kGearBelt] = slot(2, 2, kBelt);
	s.grids[kGearShirt] = slot(4, 2, kShirt, { item(3, "Leather Turtleneck", 4, 2, icons::recordPath(".\\data\\items\\icons\\LeatherShirt.png"), kShirt) });
	s.grids[kGearHead] = slot(4, 3, kHat, { item(4, "Straw Hat", 3, 3, icons::cached("575-gamedata.base", "2146-gamedata.base"), kHat) });
	s.grids[kGearPants] = slot(4, 5, kLegs);
	s.grids[kGearArmour] = slot(4, 6, kArmour, { item(5, "Dustcoat", 4, 6, icons::cached("3061-gamedata.base", "3060-gamedata.base"), kArmour) });
	s.grids[kGearBoots] = slot(4, 2, kBoots);
	s.grids[kGearMain] = { 8, 10, false, false, {}, { at(charged(item(6, "Basic First Aid Kit", 2, 1, icons::recordPath(".\\data\\items\\icons\\basic_medkit.png"), kItem, 3), 0.6f), 0, 0),
		at(item(7, "Bread", 3, 2, "", kItem, 4), 2, 0), at(item(8, "Steel bars", 6, 1, "", kItem, 2), 0, 2), at(item(9, "Hemp", 4, 4, ""), 0, 3) } };
	// Tooltips as the host fills them (line sets after kenshi-inspect inv_click10_static.md 5; the texts are stand-ins of our own).
	ui::GridItem& katana = s.grids[kGearWeapon1].items[0];
	katana.tip = { "Katana", 1, { { "[Katana]", "" }, { "[Standard]", "ItemGrade2" } },
		{ { "-Cutting damage", "x1.05" }, { "-Blunt damage", "x0.12" }, { "-Blood loss", "x1.2" }, { "-Armour penetration", "-20%", false, "TipBad" },
			{ "-Defence bonus", "+2", false, "TipGood" }, { "-Indoors bonus", "+0%" }, { "-Damage vs robots", "x0.8" }, { "----------------", "" }, { "-Weight", "5 kg" },
			{ "-Value", "c.1,040" }, { "-Sell value", "c.520" } }, "A long single-edged blade. A stand-in description of our own." };
	// Armour (kenshi-inspect tooltips.md): class left / material right, no rule, [Coverage] rows, a blank line, the description. No resistance rows yet.
	s.grids[kGearArmour].items[0].tip = { "Dustcoat", 1, { { "[Standard Grade]", "ItemGrade1" }, { "[Light Armour Class]", "TipMaker", "[Leather]" } },
		{ { "-Blunt resistance", "8%", false, "TipMaker" }, { "-Cut resistance", "12%", false, "TipMaker" }, { "-Cut resistance efficiency", "50%", false, "TipMaker" },
			{ "-Harpoon resistance", "6 pts", false, "TipMaker" }, { "-Weight", "6 kg" }, { "-Value", "c.640" }, { "-Sell value", "c.160" }, { "[Coverage]", "" }, { "-Chest", "80%" }, { "-Stomach", "80%" }, { "", "" } },
		"A long coat against the dust. A stand-in description of our own." };
	ui::GridItem& bread = s.grids[kGearMain].items[1];
	bread.tip = { "Bread", bread.count, { { "[Food]", "TipGood" } },
		{ { "Nutrition", "20 nu" }, { "Ingredients", "Wheatstraw" }, { "-Weight", "1 kg (x 4) 4 kg" }, { "-Value", "c.48" }, { "-Sell value", "c.24" } },
		"Flat and filling. A stand-in description of our own." };
	ui::GridItem& kit = s.grids[kGearMain].items[0];
	kit.tip = { "Basic First Aid Kit", kit.count, {}, { { "-Weight", "2 kg" }, { "-Value", "c.120" }, { "-Sell value", "c.120" }, { "Avg. Price", "c.130" },
		{ "-Charges", "600/1,000", true }, { "-Quality", "60/100", true } }, "Bandages and a little disinfectant. A stand-in description of our own." };
	s.grids[kGearBag] = { 8, 8, false, false, {}, { at(item(10, "Wakizashi", 6, 1, icons::cached("1020-gamedata.base", "1067-gamedata.base"), kWeapon), 0, 0),
		at(item(11, "Ninja Rags", 4, 6, icons::cached("2151-gamedata.base", "2152-gamedata.base"), kArmour), 0, 1), at(item(12, "Straw Hat", 3, 3, icons::cached("575-gamedata.base", "2146-gamedata.base"), kHat), 4, 1),
		at(item(13, "Blueprint", 2, 2, ""), 4, 5) } };
	s.grids[kGearBag].items[3].tip = { "Blueprint: Katanas", 1, { { "Research Blueprint", "TipTitle" } },
		{ { "Tech Level", "2" }, { "Requirements", "Research bench" }, { "New Weapon Class", "Katanas" }, { "New Items", "Katana, Wakizashi" } }, "",
		"Right-click to learn tech" };
	s.grids[kGearArmRight] = slot(2, 6, kLimb);
	s.grids[kGearArmLeft] = slot(2, 6, kLimb);
	s.grids[kGearArmRight].blocked = s.grids[kGearArmLeft].blocked = true;	// still attached
	// Kenshi_LimbReplacementWindow: arms 2 x 6, legs 2 x 7 (the limb records' footprint). Stealth legs, as in the in-game capture
	// (kenshi-inspect findings/ui_captures/darkui_robotics_tooltip_0948.jpg); the left and right records render their icons alike.
	auto leg = [&](uint32_t id, const char* side, const char* sid)
	{
		ui::GridItem it = item(id, (std::string("Stealth Leg (") + side + ")").c_str(), 2, 7, icons::cached(sid, "95580-Newwworld.mod"), kLimb);
		it.tooltip = "{TipTitle}" + it.name + "\n{ItemGrade4}[Masterwork]\n-Max HP:\t170\n-----STATS-----\n-Stealth\t{TipGood}+25%\n-Athletics\t{TipGood}+4%\n"
			"-Swimming\t{TipBad}-25%\n----------------\n-Weight\t15 kg\n-Value\tc.18,440\n-Sell value\tc.9,220\n"
			"A stand-in description of our own, as long as the capture's, so the paragraph wraps over several lines the way the game's does at this width.";
		return it;
	};
	s.grids[kGearLegRight] = slot(2, 7, kLimb, { leg(13, "right", "95742-Newwworld.mod") });
	s.grids[kGearLegLeft] = slot(2, 7, kLimb, { leg(14, "left", "95579-Newwworld.mod") });
	// The capture's other tooltip (darkui_inventory_tooltip_0930.jpg): a stolen straw hat.
	s.grids[kGearBag].items[2].tooltip = "{TipTitle}Straw Hat\n{TipBad}Stolen\n-Weight\t1 kg\n[Weather Protections]\n{TipGood}-Acid\t40%\n-Value\tc.10\n"
		"-Sell value\tc.2\n{TipDim}[No Armour Coverage]\n[Skills]\n-Perception bonus\t{TipGood}+6\n\n"
		"A wide-brimmed hat of woven straw. A stand-in description of our own, long enough to wrap over a few lines like the capture's.";
	s.info = { { "Total Weight:", "19/101", "" }, { "", "-Weightless", "" }, { "Disguise:", "None", "", "Wearing nobody's colours." },
		{ "Slave of:", "Slave Hunters", "Bad", "Who holds your papers." }, { "Looks like slave:", "20%", "Bad", "How much your clothes give you away.\n-Clothing\t20%" } };
}

// The gear's container window (demo.cont), as ui::ItemWindows keeps one docked: its state from demo.ccs, its grid from demo.gs (gearMove moves
// items there); shown while the gear is and it has a grid. Every frame, before the gear's update (Gear::place lays out the partner).
void demoContainer()
{
	demo.ccs.owner = demo.gs.owner;
	demo.ccs.grid = demo.gs.grids[ui::kGearContainer];
	demo.cont->update(demo.ccs);
	demo.cont->window().visible = demo.gear->shown() && demo.gs.valid && demo.cont->open();
}

// Trading (kenshi-inspect findings/ui_captures/trade_window.md): the gear with a trader as its container, the trader's cats over the grid; the
// tooltips carry the capture's price lines (per unit, Price mark-up red above 100 %, green below), a stolen item of ours its fence lines.
void demoTrade()
{
	demoGear();
	ui::GearState& s = demo.gs;
	demo.ccs.name = "Trade Goods";
	demo.ccs.trade = { true, 25000 };
	ui::GridItem parts, plating, loot;
	parts.id = 20;
	parts.name = "Electrical Components";
	parts.w = parts.h = 2;
	parts.count = 43;
	parts.soundSwitch = "Metal";	// a stand-in Inventory_Items value
	parts.tip = { parts.name, 43, {}, { { "-Weight", "43 x 1 kg (43 kg)" }, { "-Value", "c.190", true }, { "-Sell value", "c.190", true }, { "Avg. Price", "c.216" },
		{ "Price mark-up", "88%", false, "TipGood" } }, "A stand-in description of our own." };
	plating = parts;
	plating.id = 21;
	plating.name = "Armour Plating";
	plating.x = 2;
	plating.count = 13;
	plating.tip = { plating.name, 13, {}, { { "-Value", "c.454", true }, { "-Sell value", "c.504", true }, { "Price mark-up", "90%", false, "TipGood" } } };
	ui::GridItem hide = parts;	// down the 40 rows: the grid scrolls
	hide.id = 23;
	hide.name = "Goat Hide";
	hide.y = 38;
	hide.count = 1;
	hide.tip = { hide.name, 1, {}, { { "-Value", "c.120", true }, { "-Sell value", "c.120", true } } };
	s.grids[ui::kGearContainer] = { 10, 40, false, false, {}, { parts, plating, hide } };
	loot.id = 22;
	loot.name = "Iron Plate";
	loot.w = loot.h = 2;
	loot.x = 4;
	loot.y = 3;
	loot.tip = { loot.name, 1, { { "Buyback", "" }, { "FENCING CHANCE: 10%", "TipBad" }, { "Stolen from: Trade Goods", "TipBad" } },
		{ { "-Weight", "3 kg" }, { "-Value", "c.190", true }, { "-Sell value", "c.95", true }, { "Price mark-up", "104%", false, "TipBad" } } };
	s.grids[ui::kGearMain].items.push_back(loot);
}

// As the server answers a move: the item leaves its grid and lands at (x, y) in the other (a slot holds it at 0, 0).
void gearMove(int from, const ui::GridItem& it, int to, int x, int y)
{
	auto& src = demo.gs.grids[from].items;
	auto i = std::find_if(src.begin(), src.end(), [&](const ui::GridItem& o) { return o.id == it.id; });
	if (i == src.end()) { return; }
	ui::GridItem moved = *i;
	src.erase(i);
	moved.x = demo.gs.grids[to].single ? 0 : x;
	moved.y = demo.gs.grids[to].single ? 0 : y;
	demo.gs.grids[to].items.push_back(moved);
}

void talkNode(int n)
{
	demo.node = n;
	ui::DialogueState& d = demo.ds;
	d.active = n >= 0;
	d.npcName = "Bartender";
	d.playerName = "Greenlander";
	d.money = "c. 1,250";
	switch (n)
	{
		case 0:
			d.npcText = "You look like you've walked a long way. The well's dry, the guards are drunk and the Holy Nation is two days out. What'll it be?";
			d.choices = { "What is this place?", "I'm looking for work. Anything that pays, and I mean anything, even the jobs nobody else will take.", "Goodbye." };
			break;
		case 1:
			d.npcText = "The Hub. Used to be something. Now it's a bar, a shop and a lot of sand.";
			d.choices = { "Tell me more.", "Back." };
			break;
		case 2:
			d.npcText = "Work? Try the bounty board. Or the bandits, they're always hiring the desperate.";
			d.choices = {};
			break;
	}
}

void save_png(const std::string& path)
{
#if defined(SOKOL_GLCORE)
	const int w = sapp_width(), h = sapp_height();
	std::vector<unsigned char> px((size_t)w * h * 4);
	glFinish();
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadBuffer(GL_BACK);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	for (size_t i = 3; i < px.size(); i += 4) { px[i] = 255; }
	stbi_flip_vertically_on_write(1);
	if (stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4)) { printf("wrote %s (%dx%d)\n", path.c_str(), w, h); }
	else { fprintf(stderr, "failed to write %s\n", path.c_str()); }
#else
	fprintf(stderr, "--screenshot needs the glcore backend (%s)\n", path.c_str());
#endif
}

ui::Box& row(ui::Widget& parent, float spacing = 8)
{
	ui::Box& b = parent.add<ui::Box>(false, spacing);
	return b;
}

void build()
{
	using A = ui::Widget::Align;
	ui::Widget& root = ui::root();

	// A generic window: title, buttons, toggles, tick boxes, dialogue-style choices and a log.
	ui::Panel& win = root.add<ui::Panel>(ui::Look::kWindow, 26);
	win.rect = { 40, 40, 470, 0 };
	ui::Box& col = win.add<ui::Box>(true, 10);
	col.halign = A::kStretch;
	ui::Label& title = col.add<ui::Label>("Hub - Bar", "Kenshi_PaintedTextFont_Large");
	title.shadow = true;
	ui::Label& speaker = col.add<ui::Label>("Bartender:", "Kenshi_StandardFont_Medium");
	speaker.colour = ui::res::colour("Special");
	ui::Label& body = col.add<ui::Label>("You look like you've walked a long way. The well's dry, the guards are drunk and the Holy Nation is two days out. "
		"What'll it be?", "Kenshi_StandardFont_Medium");
	body.wrap = true;
	body.rect.w = 418;
	ui::List& choices = col.add<ui::List>();
	choices.rect.w = 418;
	choices.numbered = true;
	demo.choices = &choices;
	choices.items = { "What is this place?", "I'm looking for work. Anything that pays, and I mean anything, even the jobs nobody else will take.", "Goodbye." };
	choices.onPick = [](int i) { if (demo.log) { demo.log->push("You picked choice " + std::to_string(i + 1), ui::res::colour("Good")); } };
	ui::Box& buttons = row(col);
	ui::Button& recruit = buttons.add<ui::Button>("Recruit");
	recruit.tooltip = "Hire this character into your squad. Costs 2000 cats.";
	recruit.onClicked = [] { ui::toast("Not enough money", ui::res::colour("BadBright")); };
	buttons.add<ui::Button>("Trade").onClicked = [] { ui::toast("Trade window: not yet"); };
	buttons.add<ui::Button>("Inventory").onClicked = [] { demo.gear->show(!demo.gear->shown()); };
	buttons.add<ui::Button>("Options").onClicked = [] { demo.opts->show(!demo.opts->shown()); };
	ui::Button& steal = buttons.add<ui::Button>("Steal");
	steal.enabled = false;
	steal.tooltip = "Disabled";
	ui::Box& toggles = row(col, 4);
	for (const char* t : { "BLOCK", "HOLD", "PASSIVE", "SNEAK" })
	{
		ui::Button& b = toggles.add<ui::Button>(t, ui::Look::kToggle, "Kenshi_StandardFont_Small");
		b.checkable = true;
		b.checked = !strcmp(t, "HOLD");
		b.onToggle = [t](bool on) { if (demo.log) { demo.log->push(std::string(t) + (on ? " on" : " off"), ui::res::colour("Main")); } };
	}
	ui::Box& ticks = row(col, 18);
	ticks.add<ui::TickBox>("Run").checked = true;
	demo.jobs = &ticks.add<ui::TickBox>("Jobs");
	ui::TickBox& off = ticks.add<ui::TickBox>("Disabled");
	off.enabled = false;
	ui::TextLog& log = col.add<ui::TextLog>("Kenshi_StandardFont_Small");
	log.rect = { 0, 0, 418, 150 };
	log.halign = A::kStretch;
	demo.log = &log;
	const char* lines[] = { "Beep has joined the squad.", "Hungry Bandits are attacking!", "Greenlander was knocked out.", "You found 140 cats.",
		"Day 12: the dust storm passes.", "Ruka bought Rice bag x4.", "Squad 1 arrived at The Hub.", "A long line of text that has to wrap onto a second row inside the log panel to show wrapping works." };
	for (int k = 0; k < 3; k++)
	{
		for (const char* l : lines) { log.push(l, k == 1 ? ui::res::colour("BadBright") : ui::res::colour("Main")); }
	}

	// The HUD (time / speed, squad bar, selection toggles) on made-up sim state; its actions change that state as the sim would.
	const char* names[] = { "Beep", "Ruka", "Greenlander", "Burn", "Hamut" };
	for (int i = 0; i < 5; i++)
	{
		ui::HudMember m;
		m.id = 100 + i;
		m.name = names[i];
		m.selected = i < 2;
		m.health = 1.0f - 0.2f * i;
		m.down = i == 4;
		m.mark = i == 2 ? ui::PortraitMark::kSlave : i == 3 ? ui::PortraitMark::kPrison : ui::PortraitMark::kNone;	// shackled, caged
		m.back = i == 0 ? ui::PortraitBack::kCombat : i == 1 ? ui::PortraitBack::kHunger : ui::PortraitBack::kNone;
		m.hurt = i == 1;
		m.dim = i == 2;	// shackled and carried: the face at 0.25, the shackle full
		m.toggles[(int)ui::HudToggle::kBlock] = i == 0;
		// A Greenlander's anatomy order; the first member is hurt and stunned, the second has a limb below zero.
		const char* parts[] = { "Right Leg", "Left Arm", "Left Leg", "Head", "Stomach", "Right Arm", "Chest" };
		for (int k = 0; k < 7; k++) { m.parts.push_back({ parts[k], 100 - (float)(k * 13 * (i + 1) % 90), i == 0 && k == 4 ? 35.0f : 0.0f, 100 }); }
		if (i == 1) { m.parts[1].health = -40; }
		if (i == 0)	// the original's layers: a robotic left leg over 100, a bandaged arm, a splinted leg, a bleed, food digesting
		{
			m.parts[2] = { "Left Leg", 123, 0, 140, 0, 0, true };
			m.parts[1].bandage = 8;
			m.parts[0] = { "Right Leg", 60, 0, 100, 0, 25 };
			m.bloodMax = 120;
			m.bleed = 0.12f;
			m.hunger = 2.28f;
			m.fed = true;
		}
		m.blood = 100 - 10.0f * i;
		m.status = "Normal";
		m.statusTip = "In one piece, more or less.";	// stand-in wording
		demo.hs.squad.push_back(m);
	}
	// The status panel: the capture's lines (kenshi-inspect trade_cant_afford_rclick.png).
	demo.hs.info = { true, "[ESCAPED SLAVE]", "HudBad", "Beep", "Nameless", "",
		{ { "State:", "Defensive" }, { "Goal:", "Ditching resources", "", "What they are up to." }, { "Encumbrance:", "0%" }, { "Athletics:", "88" } },
		{ { "ATHLETICS XP:", "150%" }, { "STRENGTH XP:", "0%" }, { "TOUGHNESS XP:", "3x" }, { "KO POINT:", "-13" }, { "RUN SPEED:", "39 kph" },
			{ "HUNGER RATE:", "100%" } },	// hud16_expanded.png
		{ { "MARTIAL ARTS:", "24 (+4)", "HudGood" }, { "STRENGTH:", "86" }, { "DEXTERITY:", "20" }, { "TOUGHNESS:", "52" }, { "DODGE:", "59 (+24)", "HudGood" } } };
	hudTagCase(6, demo.hs.info);	// the capture's [ESCAPED SLAVE]
	demo.hs.clockValid = true;
	demo.hs.floorValid = true;	// a stand-in building of floors 0..2
	demo.hs.floorName = "Storm House";
	demo.hs.floorDown = false;	// on the ground
	demo.hs.jobsValid = true;
	demo.hs.jobs = { { 1, "Medic" }, { 2, "Robotics" }, { 3, "Operating machine: Iron Refinery" }, { 4, "Hauling to: Storage chest" } };
	demo.hs.day = 12;
	demo.hs.hour = 14.1f;
	ui::HudActions act;
	act.select = [](uint64_t id, bool add) { for (ui::HudMember& m : demo.hs.squad) { m.selected = m.id == id || (add && m.selected); } };
	act.focus = [](uint64_t id) { ui::toast("Follow #" + std::to_string(id)); };
	act.toggle = [](ui::HudToggle t, bool v) { for (ui::HudMember& m : demo.hs.squad) { if (m.selected) { m.toggles[(int)t] = v; } } };
	act.stop = [] { ui::toast("Stop"); };
	act.prospect = [] { demo.menuLog += "prospect;"; };
	act.pause = [](bool p) { demo.hs.paused = p; };
	act.setSpeed = [](float v) { demo.hs.speed = v; };
	act.removeJob = [](uint64_t id)
	{
		std::erase_if(demo.hs.jobs, [id](const ui::Job& j) { return j.id == id; });
		demo.menuLog += "remove job " + std::to_string(id) + ";";
	};
	act.moveJob = [](uint64_t id, int index)
	{
		auto it = std::find_if(demo.hs.jobs.begin(), demo.hs.jobs.end(), [id](const ui::Job& j) { return j.id == id; });
		if (it == demo.hs.jobs.end()) { return; }
		const ui::Job j = *it;
		demo.hs.jobs.erase(it);
		demo.hs.jobs.insert(demo.hs.jobs.begin() + std::clamp(index, 0, (int)demo.hs.jobs.size()), j);
		demo.menuLog += "move job " + std::to_string(id) + " " + std::to_string(index) + ";";
	};
	act.setActive = [](uint64_t id) { demo.hs.activeSquad = id; demo.sqLog += "active " + std::to_string(id) + ";"; };
	act.floor = [](int delta)
	{
		demo.hs.floor = std::clamp(demo.hs.floor + delta, 0, 2);
		demo.hs.floorUp = demo.hs.floor < 2;
		demo.hs.floorDown = demo.hs.floor > 0;
	};
	act.moveSpeed = [](int mode) { demo.hs.moveSpeed = mode; };
	act.shortcut = [](const std::string& action) { demo.shortcutLog += action + ";"; };
	act.extend = [](bool on) { demo.extendLog += on ? "up;" : "down;"; };
	act.buildingButton = [](uint64_t id, const std::string& b) { demo.hbLog += "button " + std::to_string(id) + " " + b + ";"; };
	act.buildingTick = [](uint64_t id, const std::string& t, bool on)
	{
		demo.hbLog += "tick " + std::to_string(id) + " " + t + " " + (on ? "1;" : "0;");
		for (ui::HudBuildingTick& k : demo.hs.building.ticks) { if (k.id == t) { k.checked = on; } }	// the host's answer
	};
	act.watch = [](uint64_t id)
	{
		demo.specLog += "watch " + std::to_string(id) + ";";
		for (const ui::HudCastRow& c : demo.hs.cast) { if (c.id == id) { demo.hs.watch.id = id; demo.hs.watch.name = c.name; demo.hs.watch.faction = c.faction; } }
	};
	act.follow = [](uint64_t id) { demo.specLog += "follow " + std::to_string(id) + ";"; demo.hs.watch.following = id != 0; };
	act.notes = [] { demo.specLog += "notes;"; demo.hs.notesOpen = !demo.hs.notesOpen; };
	demo.hud = std::make_unique<ui::Hud>(std::move(act));
	demo.hud->update(demo.hs);
	demo.opts = std::make_unique<ui::Options>();
	ui::DialogueActions da;
	da.choose = [](int i)
	{
		static const int next[3][3] = { { 1, 2, -1 }, { 1, 0, 0 }, { -1, -1, -1 } };
		talkNode(next[demo.node][i]);
	};
	da.leave = [] { talkNode(-1); };
	demo.talk = std::make_unique<ui::Dialogue>(std::move(da));
	demo.bubbles = std::make_unique<ui::SpeechBubbles>();
	demo.progress = std::make_unique<ui::ProgressBars>();
	demo.names = std::make_unique<ui::NameLabels>();
	ui::CreatorActions ca;
	auto logged = [](const char* what) { return [what](int i) { demo.crLog += std::string(what) + " " + std::to_string(i) + ";"; }; };
	ca.setName = [](const std::string& n) { demo.cs.name = n; demo.crLog += "name " + n + ";"; };
	ca.setGroup = [](int i)
	{
		demo.cs.group = i;
		demo.cs.race = 0;
		demo.cs.bothSexes = i != 2;	// hive: one sex
		if (!demo.cs.bothSexes) { demo.cs.sex = 0; }
		demo.crLog += "group " + std::to_string(i) + ";";
	};
	ca.setRace = [](int i) { demo.cs.race = i; demo.crLog += "race " + std::to_string(i) + ";"; };
	ca.setSex = logged("sex");
	ca.setCharacter = [](int i) { demo.cs.character = i; demo.crLog += "character " + std::to_string(i) + ";"; };
	ca.setHead = [](int i) { demo.cs.head = i; demo.crLog += "head " + std::to_string(i) + ";"; };
	ca.setHair = [](int i) { demo.cs.hair = i; demo.crLog += "hair " + std::to_string(i) + ";"; };
	ca.setBeard = logged("beard");
	ca.setHairColour = logged("colour");
	ca.setSlider = [](const std::string& n, float v) { for (ui::CreatorSlider& sl : demo.cs.sliders) { if (sl.name == n) { sl.value = v; } } };
	ca.setClothes = [](bool on) { demo.cs.clothes = on; demo.crLog += on ? "clothes on;" : "clothes off;"; };
	ca.randomize = logged("random");
	ca.randomizeAll = [] { demo.crLog += "randomall;"; };
	ca.resetDefaults = [] { demo.crLog += "reset;"; };
	ca.importCharacter = [] { demo.crLog += "import;"; };
	ca.exportCharacter = [] { demo.crLog += "export;"; };
	ca.done = [] { demo.crLog += "done;"; };
	ca.cancel = [] { demo.crLog += "cancel;"; };
	ca.rotate = [](float dx) { demo.rotated += dx; };
	demo.cr = std::make_unique<ui::Creator>(std::move(ca));
	demo.prompt = std::make_unique<ui::Prompt>();
	// The actions change the squads state as the server would.
	demoSquads();
	demo.hs.activeSquad = 1;
	ui::SquadsActions sa;
	sa.newSquad = []
	{
		demo.sq.squads.push_back({ demo.sq.squads.back().id + 1, "Nameless_" + std::to_string(demo.sqNext++), {} });
		demo.sqLog += "new;";
	};
	sa.rename = [](uint64_t id, const std::string& n)
	{
		for (ui::SquadInfo& q : demo.sq.squads) { if (q.id == id) { q.name = n; } }
		demo.sqLog += "rename " + std::to_string(id) + " " + n + ";";
	};
	sa.remove = [](uint64_t id)
	{
		std::erase_if(demo.sq.squads, [id](const ui::SquadInfo& q) { return q.id == id; });
		demo.sqLog += "remove " + std::to_string(id) + ";";
	};
	sa.moveMember = [](uint64_t member, uint64_t to, int index)
	{
		ui::SquadMember moved;
		for (ui::SquadInfo& q : demo.sq.squads)
		{
			auto it = std::find_if(q.members.begin(), q.members.end(), [member](const ui::SquadMember& m) { return m.id == member; });
			if (it != q.members.end()) { moved = *it; q.members.erase(it); }
		}
		for (ui::SquadInfo& q : demo.sq.squads)
		{
			if (q.id == to) { q.members.insert(index < 0 ? q.members.end() : q.members.begin() + std::min<size_t>(index, q.members.size()), moved); }
		}
		demo.sqLog += "move " + std::to_string(member) + " " + std::to_string(to) + " " + std::to_string(index) + ";";
	};
	sa.swapMembers = [](uint64_t a, uint64_t b)
	{
		ui::SquadMember *pa = nullptr, *pb = nullptr;
		for (ui::SquadInfo& q : demo.sq.squads)
		{
			for (ui::SquadMember& m : q.members) { if (m.id == a) { pa = &m; } else if (m.id == b) { pb = &m; } }
		}
		if (pa && pb) { std::swap(*pa, *pb); }
		demo.sqLog += "swap " + std::to_string(a) + " " + std::to_string(b) + ";";
	};
	sa.moveSquad = [](uint64_t id, int index)
	{
		auto& v = demo.sq.squads;
		auto it = std::find_if(v.begin(), v.end(), [id](const ui::SquadInfo& q) { return q.id == id; });
		if (it == v.end()) { return; }
		const ui::SquadInfo q = *it;
		v.erase(it);
		v.insert(v.begin() + std::clamp<size_t>(index, 0, v.size()), q);
		demo.sqLog += "movesquad " + std::to_string(id) + " " + std::to_string(index) + ";";
	};
	sa.dismiss = [](uint64_t member)
	{
		for (ui::SquadInfo& q : demo.sq.squads) { std::erase_if(q.members, [member](const ui::SquadMember& m) { return m.id == member; }); }
		demo.sq.used--;
		demo.sqLog += "dismiss " + std::to_string(member) + ";";
	};
	ui::OverviewActions oa;
	oa.close = [] { demo.overviewLog += "close;"; };
	oa.page = [](ui::OverviewPage p) { demo.overviewLog += "page " + std::to_string((int)p) + ";"; };
	ui::MapActions ma;
	ma.draw = [](const ui::MapFrame& f)	// a stand-in map with sokol-gl over the world square of the view: a sand gradient, a grid, a marker
	{
		demo.mapDraws++;
		const float side = (float)(294912 * f.pxPerUnit);
		const float x = (float)(f.x + f.w / 2 - (147456 + f.cx) * f.pxPerUnit), y = (float)(f.y + f.h / 2 - (147456 + f.cz) * f.pxPerUnit), w = side, h = side;
		sgl_disable_texture();
		sgl_begin_quads();
		sgl_v2f_c3f(x, y, 0.55f, 0.47f, 0.33f); sgl_v2f_c3f(x + w, y, 0.62f, 0.52f, 0.36f);
		sgl_v2f_c3f(x + w, y + h, 0.40f, 0.33f, 0.24f); sgl_v2f_c3f(x, y + h, 0.36f, 0.30f, 0.22f);
		sgl_end();
		sgl_begin_lines();
		sgl_c4f(0, 0, 0, 0.25f);
		for (int i = 1; i < 8; i++)
		{
			sgl_v2f(x + w * i / 8, y); sgl_v2f(x + w * i / 8, y + h);
			sgl_v2f(x, y + h * i / 8); sgl_v2f(x + w, y + h * i / 8);
		}
		sgl_end();
		sgl_begin_triangles();
		sgl_c3f(0.8f, 0.2f, 0.15f);
		sgl_v2f(x + w * 0.4f, y + h * 0.45f); sgl_v2f(x + w * 0.42f, y + h * 0.5f); sgl_v2f(x + w * 0.38f, y + h * 0.5f);
		sgl_end();
	};
	ma.moveTo = [](double x, double z, bool queue)
	{
		char b[96];
		snprintf(b, sizeof b, "move %.0f %.0f %d;", x, z, queue ? 1 : 0);
		demo.overviewLog += b;
	};
	demo.overview = std::make_unique<ui::Overview>(std::move(oa), std::move(ma));
	{
		using K = ui::MapPlaceKind;
		using T = ui::MapTier;
		using R = ui::MapRelation;
		ui::MapState& m = demo.map;	// stand-in places of our own around the middle of the world
		m.places = {
			{ "hub", K::kTown, T::kMin, -5000, 3000, "Crossroads", R::kNeutral, "Crossroads\n\nFaction\tTraders\nRelations\t0\nResidents\t40" },
			{ "home", K::kOutpost, T::kMin, 8000, -6000, "Home", R::kPlayer, "" },
			{ "stack", K::kVillage, T::kMid, 2000, 9000, "Stack", R::kAlly, "" },
			{ "camp", K::kCamp, T::kMid, -9000, -4000, "Dust camp", R::kEnemy, "" },
			{ "ruin", K::kRuin, T::kMax, 12000, 6000, "Old ruin", R::kNeutral, "", true } };
		m.characters = { { 7000, -5000 }, { 7600, -5400 } };
		m.cameraX = 6000;
		m.cameraZ = -4000;
		m.cameraYaw = 0.6f;
		demo.overview->updateMap(m);
	}
	demo.squads = std::make_unique<ui::Squads>(*demo.overview, std::move(sa));
	// Research: stand-in entries of our own; the actions change the state as the server would.
	{
		using S = ui::ResearchEntry::State;
		using N = ui::ResearchNeed::State;
		ui::ResearchState& r = demo.rs;
		r.categories = { { "core", "Core" }, { "defence", "Defence" }, { "farming", "Farming" } };
		r.category = "defence";
		r.entries = {
			{ "lamp", "Watchtower Lamp", "defence", 3, "<1 hr", "<1 hr (at 1x rate)", { { "Book x1", N::kMissing } }, { { "Watchtowers", N::kMet }, { "Iron Gate", N::kQueued } },
				{ { "New Building:", "Watchtower Lamp [light]" } }, "A lamp for the top of a watchtower, so the night watch sees what comes.", S::kLocked, 0, "A bigger desk is needed (level 3)" },
			{ "spikes", "Spike Wall", "defence", 2, "2 hrs", "2 hrs (at 1x rate)", { { "Paid", N::kMet } }, {}, { { "New Building:", "Spike Wall" } }, "Sharpened stakes in a row.", S::kAvailable, 0.25f },
			{ "gate", "Iron Gate", "defence", 4, "3 hrs", "3 hrs (at 1x rate)", { { "Book x2", N::kMet } }, {}, { { "New Building:", "Iron Gate" } }, "A gate that holds.", S::kAvailable },
			{ "wall", "Stone Wall", "defence", 1, "1 hr", "", {}, {}, {}, "", S::kDone },
			{ "beds", "Better Beds", "core", 1, "1 hr", "1 hr (at 1x rate)", { { "Book x1", N::kMet } }, {}, { { "New Building:", "Bed" } }, "Sleep heals faster.", S::kInProgress, 0.4f },
			{ "wheat", "Wheat Farm", "farming", 1, "1 hr", "1 hr (at 1x rate)", { { "Book x1", N::kMet } }, {}, { { "New Building:", "Wheat Farm" } }, "Bread starts here.", S::kAvailable } };
		r.queue = { "beds", "gate" };
		r.benchMessage = "One research bench, staffed.";
		ui::ResearchActions ra;
		ra.category = [](const std::string& id) { demo.rsLog += "category " + id + ";"; demo.rs.category = id; };
		ra.queue = [](const std::string& id) { demo.rsLog += "queue " + id + ";"; demo.rs.queue.push_back(id); };
		ra.unqueue = [](const std::string& id) { demo.rsLog += "unqueue " + id + ";"; std::erase(demo.rs.queue, id); };
		ra.move = [](const std::string& id, int to)
		{
			demo.rsLog += "move " + id + " " + std::to_string(to) + ";";
			std::erase(demo.rs.queue, id);
			demo.rs.queue.insert(demo.rs.queue.begin() + std::min<size_t>(to, demo.rs.queue.size()), id);
		};
		ra.close = [] { demo.rsLog += "close;"; };
		demo.research = std::make_unique<ui::Research>(*demo.overview, std::move(ra));
	}
	// Crafting: stand-in benches of our own; the actions change the state as the server would.
	{
		ui::CraftingState& c = demo.cs2;
		using L = std::vector<ui::CraftLine>;
		ui::CraftStation bench = { "bench", "Armour Bench", "", true, {
			{ "vest", "Plated Vest", L{ { "Cut", "40%", "" }, { "Weight", "6 kg", "" }, { "Made from", "Iron Plates x2, Fabric x1", "" } }, L{ { "Build time", "2 hrs", "" }, { "Plates", "2", "" } } },
			{ "helm", "Iron Helm", L{ { "Blunt", "30%", "" }, { "Made from", "Iron Plates x3", "" } }, L{ { "Build time", "3 hrs", "" } } } },
			{ { "vest", 0.3f, L{ { "Status", "Working", "" }, { "Done in", "1 hr", "" }, { "Expected", "Prime", "Bad" } } }, { "vest", 0, L{ { "Status", "Waiting", "" } } } }, false };
		ui::CraftStation loom = { "loom", "Loom", "No power", false, {}, {}, false };
		c.stations = { bench, loom };
		c.station = "bench";
		c.empty = "Nothing this bench can make yet: research or buy some blueprints.";	// stand-in wording
		auto at = [](const std::string& id) -> ui::CraftStation& { for (ui::CraftStation& s : demo.cs2.stations) { if (s.id == id) { return s; } } return demo.cs2.stations.front(); };
		ui::CraftingActions ca;
		ca.station = [](const std::string& id) { demo.csLog += "station " + id + ";"; demo.cs2.station = id; };
		ca.queue = [at](const std::string& st, const std::string& r) { demo.csLog += "queue " + st + " " + r + ";"; at(st).queue.push_back({ r, 0, {} }); };
		ca.unqueue = [at](const std::string& st, int i) { demo.csLog += "unqueue " + st + " " + std::to_string(i) + ";"; auto& q = at(st).queue; q.erase(q.begin() + i); };
		ca.move = [at](const std::string& st, int from, int to)
		{
			demo.csLog += "move " + st + " " + std::to_string(from) + " " + std::to_string(to) + ";";
			auto& q = at(st).queue;
			const ui::CraftQueued e = q[from];
			q.erase(q.begin() + from);
			q.insert(q.begin() + std::min<size_t>(to, q.size()), e);
		};
		ca.repeat = [at](const std::string& st, bool on) { demo.csLog += "repeat " + st + " " + (on ? "1" : "0") + ";"; at(st).repeat = on; };
		ca.close = [] { demo.csLog += "close;"; };
		demo.crafting = std::make_unique<ui::Crafting>(*demo.overview, std::move(ca));
	}
	// Faction: stand-in factions of our own, from -100 to 100; a rename changes the state as the server would.
	{
		ui::FactionState& f = demo.fs;
		f.name = "Drifters";
		const std::pair<const char*, float> rel[] = { { "Ash Walkers", 0 }, { "Bone Pickers", -100 }, { "Canyon Guild", 0 }, { "Cinder Monks", -11.4f }, { "Dune Rangers", 35 },
			{ "Dust Choir", 0 }, { "Ember Clans", 0 }, { "Glass Hunters", -60 }, { "Grey Wardens", 0 }, { "Hollow Kings", 0 }, { "Iron Pact", 100 }, { "Jade Riders", 0 },
			{ "Kiln Folk", 4.75f }, { "Lantern Order", 0 }, { "Mire Wolves", -100 }, { "Night Caravan", 0 }, { "Ochre League", 0 }, { "Pale Hand", -35 }, { "Quarry Union", 0 },
			{ "Red Tide", 0 }, { "Salt Barons", 0 }, { "Silt Runners", 70 }, { "Stone Choir", 0 }, { "Thorn Kin", 0 }, { "Umber Watch", 0 }, { "Vale Traders", 20 }, { "Wind Callers", 0 } };
		for (const auto& [name, v] : rel) { f.relations.push_back({ name, name, v }); }
		f.info = "{ResearchDim}---Upcoming Events---";
		ui::FactionActions fa;
		fa.rename = [](const std::string& n) { demo.fsLog += "rename " + n + ";"; demo.fs.name = n; };
		fa.close = [] { demo.fsLog += "close;"; };
		demo.faction = std::make_unique<ui::Faction>(*demo.overview, std::move(fa));
	}
	// Building windows: the actions log what the host would order; active() flips the furnace as the server would.
	{
		ui::BuildingWindowActions ba;
		auto logged = [](const char* what) { return [what](uint64_t id, int g, const ui::GridItem& it) { demo.bwLog += std::string(what) + " " + std::to_string(id) + " " + std::to_string(g) + " " + std::to_string(it.id) + ";"; }; };
		ba.move = [](uint64_t id, int from, const ui::GridItem& it, int to, int x, int y)
		{
			demo.bwLog += "move " + std::to_string(id) + " " + std::to_string(from) + " " + std::to_string(it.id) + " " + std::to_string(to) + " " + std::to_string(x) + " " + std::to_string(y) + ";";
		};
		ba.quickMove = logged("quick");
		ba.dropOutside = logged("drop");
		ba.queue = [](uint64_t id) { demo.bwLog += "queue " + std::to_string(id) + ";"; };
		ba.active = [](uint64_t id, bool on) { demo.bwLog += "active " + std::to_string(id) + " " + (on ? "1" : "0") + ";"; if (demo.bws.furnace) { demo.bws.furnace->active = on; } };
		ba.close = [](uint64_t id) { demo.bwLog += "close " + std::to_string(id) + ";"; demo.bws.valid = false; };
		demo.building = std::make_unique<ui::BuildingWindow>(std::move(ba));
	}
	demo.stats = std::make_unique<ui::Stats>(ui::StatsActions{ [] { demo.statsClosed++; } });

	// The title screen, the game menu and the save panel over six made-up saves (fixed times: the screenshot stays the same).
	ui::SaveActions sva;
	sva.load = [](const std::string& n, bool reset) { demo.menuLog += "load " + n + " " + std::to_string(reset) + ";"; };
	sva.save = [](const std::string& n) { demo.menuLog += "save " + n + ";"; };
	sva.remove = [](const std::string& n)
	{
		demo.menuLog += "remove " + n + ";";
		std::vector<ui::SaveEntry> left;
		for (const ui::SaveEntry& e : demo.saves->saves()) { if (e.name != n) { left.push_back(e); } }
		demo.saves->setSaves(std::move(left));
	};
	sva.close = [] { demo.menuLog += "close;"; };
	sva.opening = [](ui::SaveMode m) { demo.menuLog += m == ui::SaveMode::kLoad ? "opening load;" : "opening save;"; };
	demo.saves = std::make_unique<ui::SavePanel>(std::move(sva));
	{
		const std::time_t base = 1793000000;	// 2026-10-26
		const struct { const char* name; int age; int day; const char* squad; } fake[] = {
			{ "quicksave", 60, 41, "Beep, Ruka, Burn" }, { "autosave", 1800, 41, "Beep, Ruka, Burn" }, { "Hub run", 86400, 30, "Beep, Ruka" },
			{ "Before the Swamp", 3 * 86400, 22, "Beep, Ruka" }, { "Holy Nation start", 9 * 86400, 3, "Wanderer" }, { "Zz old", 30 * 86400, 1, "Wanderer" } };
		std::vector<ui::SaveEntry> v;
		for (const auto& f : fake)
		{
			const std::time_t t = base - f.age;
			char date[32];
			std::strftime(date, sizeof date, "%Y-%m-%d %H:%M", std::gmtime(&t));
			v.push_back({ f.name, t, date, std::string("Day\t") + std::to_string(f.day) + "\nSquad\t" + f.squad + "\nMoney\t1,240 c" });
		}
		demo.saves->setSaves(std::move(v));
	}
	ui::MainMenuActions mma;
	mma.continueGame = [] { demo.menuLog += "continue;"; };
	mma.newGame = [] { demo.menuLog += "new;"; };
	mma.import = [] { demo.menuLog += "import;"; };
	mma.options = [] { demo.menuLog += "options;"; };
	mma.exit = [] { demo.menuLog += "exit;"; };
	mma.scenarios = [] { demo.playLog += "menu scenarios;"; };
	demo.mainMenu = std::make_unique<ui::MainMenu>(std::move(mma), demo.saves.get());
	demo.mainMenu->setVersion("0.1");
	demo.mainMenu->setCredits("OPENKENSHI\n\nA free, open-source engine for Kenshi. Not affiliated with or endorsed by Lo-Fi Games; \"Kenshi\" is a trademark of Lo-Fi Games Ltd.\n"
		"The game's data and art are Lo-Fi Games'; you need your own copy.\n\n"
		"LIBRARIES\nsokol (zlib), stb (MIT / public domain), Jolt Physics (MIT), flecs (MIT), Lua (MIT), SMAA (MIT), Recast & Detour (zlib), libvorbis / libogg (BSD-3-Clause), aoTuV codebooks (BSD-3-Clause)\n\n"
		"REFERENCES\nwwiser, ww2ogg, vgmstream, KenshiLib / RE_Kenshi, OGRE, ParticleUniverse, Kenshi Wiki (format and data notes)");	// the host's text (main.cpp), so the demo checks it fits
	{
		DdsImage dds;
		std::string err;
		if (dds_load(demo.data + "/gui/gfx/title_screen1.dds", dds, err))
		{
			sg_view_desc vd = {};
			vd.texture.image = dds_make_image(dds, "title-art");
			ui::PortraitRef art;
			art.tex = dl::newTex();
			dl::gpu::bind(art.tex, sg_make_view(&vd), {});	// the host binds its own texture under a UI id (kenshi-k16.2.205.12.38)
			demo.mainMenu->setBackground(art);
		}
		else { fprintf(stderr, "kui_demo: title art: %s\n", err.c_str()); }
	}
	ui::GameMenuActions gma;
	gma.newGame = [] { demo.menuLog += "new;"; };
	gma.options = [] { demo.menuLog += "options;"; };
	gma.exitGame = [] { demo.menuLog += "exit;"; };
	gma.shown = [](bool on) { demo.menuLog += on ? "shown 1;" : "shown 0;"; };
	gma.scenarios = [] { demo.playLog += "pause scenarios;"; };
	demo.gameMenu = std::make_unique<ui::GameMenu>(std::move(gma), demo.saves.get());
	ui::NewGameActions nga;
	nga.begin = [](const std::string& id) { demo.menuLog += "begin " + id + ";"; };
	nga.advanced = []
	{
		demo.menuLog += "advanced;";
		if (!demo.showAdvanced) { return; }	// the menu runs: the panel alone
		if (demo.advanced->shown()) { demo.advanced->show(false); }
		else { demo.advanced->openBelow(*demo.newGame->window()); }
	};
	nga.cancel = [] { demo.menuLog += "cancel;"; };
	demo.newGame = std::make_unique<ui::NewGamePanel>(std::move(nga));
	{
		std::string slaves = "{Special}Slaves\n\nYou were caught and sold. Escape, or work the mines until you die.\n\nSquad\tsome slaves\nMoney\t0 c\nStart\tRebirth";
		for (int i = 0; i < 12; i++) { slaves += "\nPrisoner " + std::to_string(i + 1) + "\tshackled"; }	// taller than the box: it scrolls
		demo.starts = {
			{ "wanderer", "Wanderer", "A lone wanderer with nothing but a katana and a few cats to his name.\n\nSquad\t1 character\nMoney\t1,000 c\nStart\tThe Hub" },
			{ "nobodies", "The Nobodies", "A ragtag group of nobodies with a fistful of cats.\n\nSquad\t5 characters\nMoney\t2,000 c\nStart\tSquin" },
			{ "slaves", "Slaves", slaves } };
		demo.newGame->setStarts(demo.starts);
	}
	ui::TutorialActions ta;	// stand-in tutorials of our own: the original's texts live in its exe and never come into the repo
	ta.done = [](const std::string& id) { demo.menuLog += "done " + id + ";"; };
	ta.dismiss = [](const std::string& id) { demo.menuLog += "dismiss " + id + ";"; };
	ta.activate = [](const std::string& id)
	{
		demo.menuLog += "activate " + id + ";";
		demo.tutorials->hint(id);
		demo.tutorials->open(id);
	};
	ta.page = [](const std::string& id, int n) { demo.menuLog += "page " + id + " " + std::to_string(n) + ";"; };
	demo.tutorials = std::make_unique<ui::Tutorials>(std::move(ta));
	ui::ProspectActions pa;	// stand-in data: the host computes the real values
	pa.select = [](const std::string& id)
	{
		demo.menuLog += "select " + id + ";";
		demo.ps.selected = id;
		demo.prospect->update(demo.ps);
	};
	pa.close = [] { demo.menuLog += "close;"; };
	demo.prospect = std::make_unique<ui::Prospecting>(std::move(pa));
	{
		ui::DebugActions da;
		da.toggle = [](const std::string& id)
		{
			demo.debugLog += "toggle " + id + ";";
			for (ui::DebugToggle& t : demo.dbg.toggles) { if (t.id == id) { t.on = !t.on; } }
		};
		da.scene = [](const std::string& name) { demo.debugLog += "scene " + name + ";"; };
		da.close = [] { demo.debugLog += "close;"; };
		da.command = [](const std::string& id) { demo.debugLog += "command " + id + ";"; };
		da.weather = [](const std::string& name) { demo.debugLog += "weather " + name + ";"; };
		da.time = [](const std::string& preset) { demo.debugLog += "time " + preset + ";"; };
		demo.debug = std::make_unique<ui::DebugPanel>(std::move(da));
		ui::DebugTreeActions ta;
		ta.refresh = [] { demo.treeLog += "refresh;"; };
		ta.setAuto = [](bool on) { demo.treeLog += on ? "auto on;" : "auto off;"; demo.ts.autoOn = on; };
		ta.close = [] { demo.treeLog += "close;"; };
		ta.copy = [](const std::string& t) { demo.treeLog += "copy;"; demo.treeCopy = t; };
		demo.tree = std::make_unique<ui::DebugTree>(std::move(ta));
		{
			// The trails panel: each action echoes into the state, as the host's next frame would.
			ui::TrailsPanelActions ra;
			ra.setOn = [](bool on) { demo.trailsLog += on ? "on;" : "off;"; demo.trs.on = on; };
			ra.pick = [](uint64_t id) { demo.trailsLog += "pick " + std::to_string(id) + ";"; demo.trs.pick = id; };
			ra.window = [](float s) { demo.trailsLog += "window " + ui::trailsSeconds(s) + ";"; demo.trs.window = s; };
			ra.clear = [] { demo.trailsLog += "clear;"; for (ui::TrailsChar& c : demo.trs.chars) { c.points = c.pops = 0; } };
			ra.close = [] { demo.trailsLog += "close;"; };
			demo.trails = std::make_unique<ui::TrailsPanel>(std::move(ra));
		}
		{
			// The light debugger: each action echoes into the state, as the host's next frame would (solo: that term alone; a lamp keeps the lamps switch).
			ui::LightsPanelActions la;
			la.set = [](const std::string& n, bool on)
			{
				demo.lightsLog += "set " + n + (on ? " on;" : " off;");
				for (ui::LightRow& t : demo.ls.terms) { if (t.name == n) { t.on = on; } }
			};
			la.solo = [](const std::string& n)
			{
				demo.lightsLog += "solo " + n + ";";
				for (ui::LightRow& t : demo.ls.terms) { t.on = t.name == n || (n.rfind("lamp:", 0) == 0 && t.name == "lamps"); }
			};
			la.allOn = [] { demo.lightsLog += "all on;"; for (ui::LightRow& t : demo.ls.terms) { t.on = true; } };
			la.close = [] { demo.lightsLog += "close;"; };
			demo.lights = std::make_unique<ui::LightsPanel>(std::move(la));
		}
		{
			// The animation browser: the actions play the host (each echoes into the state, as the host's next frame would).
			ui::AnimBrowserActions aa;
			auto log = [](const std::string& t) { demo.animLog += t + ";"; };
			aa.race = [log](const std::string& r) { log("race " + r); demo.as.race = r; };
			aa.female = [log](bool on) { log(on ? "female on" : "female off"); demo.as.female = on; };
			aa.group = [log](const std::string& g) { log("group " + g); demo.as.group = g; };
			aa.follow = [log](bool on)
			{
				log(on ? "follow on" : "follow off");
				for (const ui::AnimClip& c : demo.as.clips) { if (c.name == demo.as.clip && c.autoFollow) { demo.as.reply = "refused: the game locks this clip's root"; return; } }
				demo.as.follow = on;
			};
			aa.select = [log](const std::string& c) { log("select " + c); demo.as.clip = c; };
			aa.play = [log](bool on) { log(on ? "play" : "pause"); demo.as.playing = on; };
			aa.step = [log](int d) { log("step " + std::to_string(d)); };
			aa.seek = [log](float t) { char b[32]; snprintf(b, sizeof b, "seek %.2f", t); log(b); };
			aa.speed = [log](double v) { char b[32]; snprintf(b, sizeof b, "speed %.2f", v); log(b); demo.as.speed = v; };
			aa.camera = [log](const std::string& c) { log("camera " + c); demo.as.camera = c; };
			aa.orbit = [log](float dx, float dy) { log(dx > 0 && dy > 0 ? "orbit +" : "orbit ?"); };
			aa.zoom = [log](float d) { log(d > 0 ? "zoom in" : "zoom out"); };
			aa.plant = [log](const std::string& p) { log("plant " + p); demo.as.plant = p; };
			aa.overlay = [log](const std::string& id, bool on) { log("overlay " + id + (on ? " on" : " off")); if (id == "trails") { demo.as.trails = on; } };
			aa.measure = [log] { log("measure"); };
			aa.mark = [log] { log("mark"); demo.as.reply = "marked " + demo.as.clip + " f" + std::to_string(demo.as.frame); };
			aa.copy = [log](const std::string& t) { log("copy"); demo.animCopy = t; };
			aa.viewSize = [](int w, int h) { demo.animW = w; demo.animH = h; };
			aa.close = [log] { log("close"); };
			demo.anim = std::make_unique<ui::AnimBrowser>(std::move(aa));
			// The playtest windows: the actions play the host. Stand-in scenarios, runs and notes (our own wording).
			ui::ScenariosActions sa;
			sa.select = [](const std::string& n) { demo.playLog += "select " + n + ";"; demo.scs.selected = n; };
			sa.play = [](const std::string& n) { demo.playLog += "play " + n + ";"; demo.scs.status = "loading " + n + "..."; };
			sa.close = [] { demo.playLog += "close;"; demo.scs.open = false; };
			demo.scen = std::make_unique<ui::ScenariosWindow>(std::move(sa));
			auto run = [](const char* date, const char* verdict, const char* note, bool mismatch, std::vector<ui::PlaytestMetric> m)
			{
				ui::PlaytestRun r;
				r.date = date;
				r.file = std::string("playtests/bar_lure/") + date + ".md";
				r.simHash = mismatch ? "7c1e09a" : "4b2d11f";
				r.clientHash = mismatch ? "e903b6c" : "a81f2c4";
				r.verdict = verdict;
				r.firstNote = note;
				r.seed = 4242;
				r.metrics = std::move(m);
				r.mismatch = mismatch;
				return r;
			};
			ui::PlaytestScenario bar{ "bar_lure", "Lure into the bar", "Outside the town, a raider patrol close by: provoke it, run into the bar and up, rejoin",
				"Do the raiders follow you up the stairs? Does your crew join the fight within a few seconds of the provoke?", "2026-11-07 15:30", "off", {} };
			bar.versions.push_back({ "v2", "2026-11-07", "The crew now joins on a provoke.", "v1/20261106-2010 #2",
				{ run("2026-11-07 15:30", "off", "the crew joined, but one raider slid sideways at the door", true,
					{ { "targeted", 3, true }, { "crew joined (s)", 2.4, true }, { "bodies down", 2, true }, { "fight (s)", 41, true }, { "player downed (s)", 0, false } }),
				  run("2026-11-07 14:05", "good", "fine this time", false, { { "targeted", 3, true }, { "crew joined (s)", 1.9, true }, { "bodies down", 3, true }, { "fight (s)", 36, true } }) } });
			bar.versions.push_back({ "v1", "2026-11-06", "", "", { run("2026-11-06 20:10", "broken", "nobody followed me in", false, { { "targeted", 1, true } }) } });
			demo.scs.scenarios = { bar,
				{ "beak_open_ground", "Beaks in the open", "Open ground, a pack of long-legged birds", "Does the pack zigzag or come straight?", "", "", {} },
				{ "slavers_downed", "Slavers and a downed member", "A slaver squad, one of yours down", "Do they pick up the downed one?", "", "",
					{ { "v1", "2026-11-07", "First cut.", "", {} } } } };
			demo.scs.selected = "bar_lure";
			demo.scs.status = "3 scenarios";
			ui::NotesActions na;
			na.add = [](const std::string& t) { demo.playLog += "add " + t + ";"; demo.ns.notes.push_back({ (int)demo.ns.notes.size() + 1, t, "" }); };
			na.verdict = [](const std::string& v) { demo.playLog += "verdict " + v + ";"; demo.ns.verdict = v; };
			na.mark = [] { demo.playLog += "mark;"; return std::string("00:42.3 Wanderer (120, -40):"); };
			na.typing = [](bool on) { demo.playLog += on ? "typing 1;" : "typing 0;"; };
			na.close = [] { demo.playLog += "close;"; demo.ns.open = false; };
			demo.notes = std::make_unique<ui::NotesWindow>(std::move(na));
			demo.ns.scenario = "bar_lure";
			demo.ns.version = "v2";
			demo.ns.verdict = "off";
			demo.ns.notes = { { 1, "the raider slid sideways at the door", "FEATURES locomotion row: the strafe slide (task 812)" },
				{ 2, "when I ran up the stairs two of them stopped at the bottom and turned around, then came back up a few seconds later, which looked like they forgot me", "", "not on kenshi-h2v.2.1: bd not found on PATH" } };
			demo.ns.warning = "bd unreachable: the notes stay in the run file";	// stand-in (kenshi-k16.2.130)
			// Stand-ins (our own names): 600 clips, a third "run", every fourth raw.
			const char* const verbs[] = { "walk", "run", "stride", "turn", "idle", "hop" };
			for (int i = 0; i < 600; i++)
			{
				char n[48];
				snprintf(n, sizeof n, "%s_%s_%03d", verbs[i % 3 == 1 ? 1 : (i / 3) % 6 == 1 ? 0 : (i / 3) % 6], i % 2 ? "left" : "fwd", i);
				ui::AnimClip c;
				c.name = n;
				c.kind = i % 4 == 3 ? "raw" : "record";
				c.length = 1.2f;
				c.frames = 37;
				demo.as.clips.push_back(c);
			}
			for (size_t i = 0; i < demo.as.clips.size(); i++)	// stand-in groups: by the verb, every seventh also Combat; Animals has none
			{
				ui::AnimClip& c = demo.as.clips[i];
				const std::string v = c.name.substr(0, c.name.find('_'));
				c.groups = { v == "idle" ? "Idle" : v == "turn" || v == "hop" ? "Combat" : "Locomotion" };
				if (i % 7 == 0 && c.groups[0] != "Combat") { c.groups.push_back("Combat"); }
				c.usedBy = i % 2 ? "Stand-in record " + std::to_string(i) + ", Tall folk" : "";
				c.clip = i % 4 == 3 ? c.name : c.name + "_src";
				c.relocates = i % 10 == 5;
				c.autoFollow = c.relocates;
			}
			demo.as.groups = { "Locomotion", "Combat", "Idle", "Animals" };
			demo.as.races = { "Tall folk", "Stone folk", "Insect folk" };
			demo.as.race = "Tall folk";
			demo.as.bones = { "pelvis", "spine", "neck", "head", "l_thigh", "l_calf", "l_foot", "l_toe", "r_thigh", "r_calf", "r_foot", "r_toe" };
			demo.as.clip = demo.as.clips[1].name;
			demo.as.frames = 37;
			demo.as.frame = 4;
			demo.as.time = 0.13f;
			demo.as.plant = "lheel";
			demo.as.readout = "stride 182 u, 1.52 u/s implied\nfwd 0.96, lat 0.04\nresidual slip 1.8 u\nL contact 0.00 - 0.52 s\nR contact 0.60 - 1.12 s";
		}
		{
			// Stand-in fighters (our own names): enough rows to scroll; two with pops.
			const char* names[] = { "Wren", "Bandit A", "Bandit B", "Ostler", "Tamsin", "Bandit C", "Hob", "Marl", "Pike", "Ivo", "Sedge", "Bandit D", "Rook", "Fenn" };
			const char* hues[] = { "#3ec46d", "#4fb8c9", "#5a7fe0", "#9a5ae0" };
			uint64_t id = 101;
			for (const char* n : names)
			{
				const uint32_t i = (uint32_t)demo.trs.chars.size();
				demo.trs.chars.push_back({ id++, n, i < 4 ? hues[i] : "", 300 + 37 * i, i == 1 ? 3u : i == 5 ? 1u : 0u });
			}
			demo.trs.on = true;
			demo.trs.window = 30;
			demo.trs.status = "trails: all, 14 chars, 4 tracks, window 30 s";
		}
		{
			// The host's terms as merged (dev/lightterms.cpp, kenshi-jj3.27.1), cut_band in its own "marker" group, then three lamps nearest first.
			const std::pair<const char*, const char*> terms[] = { { "sun_direct", "sun" }, { "sun_shadows", "sun" }, { "sky_ambient", "ambient" }, { "ao", "ambient" },
				{ "post_indirect", "ambient" }, { "sun_bounce", "ambient" }, { "interior_light", "ambient" }, { "env_specular", "ambient" }, { "lamps", "lamps" },
				{ "emissive", "emissive" }, { "fog_atmosphere", "fog" }, { "fog_weather", "fog" }, { "sky", "sky" }, { "cut_band", "marker" }, { "sun_shafts", "air" },
				{ "dust_motes", "air" }, { "fog_volumes", "air" }, { "bloom", "post" }, { "exposure", "post" }, { "post_grade", "post" }, { "lamp_beams", "air" },
				{ "lamp:4012", "lamps" }, { "lamp:4013", "lamps" }, { "lamp:3977", "lamps" } };
			for (const auto& [n, g] : terms) { demo.ls.terms.push_back({ n, g, std::string(n) != "sun_shadows" && std::string(n) != "fog_weather" }); }
		}
		demo.ts.title = "{#cc4422}Squad 1234 - Hungry Bandits";	// a stand-in squad (our own)
		demo.ts.tick = 5000;
		demo.ts.rows = { { 0, "squad", "1234 Hungry Bandits" }, { 1, "goal", "raid" }, { 1, "members", "2" }, { 2, "member", "Bandit A" }, { 3, "hp", "80" },
			{ 2, "member", "Bandit B" }, { 3, "hp", "55" }, { 1, "pos", "1200, 40, -900" } };
		demo.dbg.toggles = { { "navmesh", "Navmesh overlay", false, "debug.navmesh" }, { "pruned", "Pruned navmesh", false, "" },
			{ "squads", "Squad overlay", true, "debug.squad_overlay" }, { "shadows", "Shadows", true, "debug.shadows" } };
		demo.dbg.scenes = { "hub_bar", "squin_gate" };
		demo.dbg.extra = "{DebugHeading}SQUADS\n{#e06060}# Dust bandits (6)\n{#60b0e0}# Traders (3)";
		using G = ui::DebugGroup;
		demo.dbg.commands = { { "bandits", "Spawn bandit patrol", true }, { "traders", "Spawn trader caravan", false },	// our own stand-ins
			{ "lights", "Lights", true, G::kWindow }, { "anims", "Animations", true, G::kWindow }, { "fullmap", "Full map", true, G::kWindow },
			{ "heal", "Heal", true, G::kCharacter }, { "ko", "Knock out", true, G::kCharacter }, { "wake", "Wake", false, G::kCharacter }, { "kill", "Kill", true, G::kCharacter } };
		demo.dbg.weathers = { "Clear sky", "Dust storm", "Acid rain", "Gas cloud", "Light rain", "Heavy rain", "Fog", "Ash fall", "Sand storm",
			"Heat haze", "Overcast", "Spore drift", "Red fog", "Burning wind", "Drizzle", "Thunder", "Calm", "Cold front", "Hot wind", "Mist" };
		for (int i = 0; i < 33; i++) { demo.dbg.weathers.push_back("Zone weather " + std::to_string(i + 1)); }	// 53, as live
		demo.dbg.weather = "Zone weather 30";	// near the end, set before the panel first shows (as the host)
		demo.dbg.reply = "spawned squad 41760";
	}
	ui::AdvancedActions aa;	// our own rows and tips; ids: the save's keys (findings/adv22_static.md), as the host's
	aa.change = [](const std::string& id, double v)
	{
		char b[64];
		snprintf(b, sizeof b, "change %s %s;", id.c_str(), ui::AdvancedOptions::number(v).c_str());
		demo.menuLog += b;
		for (ui::AdvOption& o : demo.adv) { if (o.id == id) { o.value = v; } }
		demo.advanced->update(demo.adv);
	};
	demo.advanced = std::make_unique<ui::AdvancedOptions>(std::move(aa));
	{
		using K = ui::AdvOption::Kind;
		demo.adv = {
			{ "ht", "Hunger time", K::kSlider, 0.25, 8, 0.01, 1, "How long characters go before they get hungry. Higher: they eat less often." },
			{ "cod", "Chance of death", K::kSlider, 0.5, 4, 0.01, 1, "How easily anyone, yours or not, dies of their wounds: bleeding out and wounds worsening. Higher: deadlier." },
			{ "gdm", "Global damage multiplier", K::kSlider, 0.5, 4, 0.01, 1, "Scales all combat damage. Fights end sooner either way; it changes their pace more than their difficulty." },
			{ "ps", "Production speed", K::kSlider, 0.5, 2, 0.01, 1, "How fast mines, farms and crafting benches turn out goods. Above 1 is easier." },
			{ "rs", "Research speed", K::kSlider, 0.5, 2, 0.01, 1, "How fast research projects finish. Above 1 is easier." },
			{ "bs", "Building speed", K::kSlider, 0.5, 2, 0.01, 1, "How fast construction goes. Above 1 is easier." },
			{ "nnm", "Number of nests multiplier", K::kSlider, 0.5, 4, 0.01, 1, "How many animal nests and bandit camps the wilds hold. Higher: more dangerous." },
			{ "bl", "Bandits loot the player", K::kTick, 0, 1, 1, 1, "Bandits who win a fight strip your fallen of their goods. Harsh, by design." },
			{ "ep", "Easy prospecting", K::kTick, 0, 1, 1, 0, "" } };
		demo.advanced->update(demo.adv);
	}
	ui::BuildActions ba;	// stand-in buildings of our own; the host fills the real ones from the game's records
	ba.category = [](const std::string& id)
	{
		demo.menuLog += "category " + id + ";";
		demo.bs.category = id;
		if (id == "power") { demo.bs.buildings = { { "wind", "Wind Generator" }, { "battery", "Battery" } }; }
		else { demo.bs.buildings = { { "bed", "Camp Bed" }, { "box", "Storage Box" }, { "lamp", "Streetlamp" } }; }
		demo.build->update(demo.bs);
	};
	ba.pick = [](const std::string& id) { demo.menuLog += "pick " + id + ";"; };
	ba.step = [](int d) { demo.menuLog += "step " + std::to_string(d) + ";"; };
	ba.confirm = [] { demo.menuLog += "confirm;"; };
	ba.undo = [] { demo.menuLog += "undo;"; };
	ba.build = [] { demo.menuLog += "build;"; };
	ba.close = [] { demo.menuLog += "close;"; };
	demo.build = std::make_unique<ui::Build>(std::move(ba));
	demo.bs.categories = { { "furniture", "Furniture" }, { "power", "Power" }, { "walls", "Walls" } };
	demo.bs.category = "furniture";
	demo.bs.buildings = { { "bed", "Camp Bed" }, { "box", "Storage Box" }, { "lamp", "Streetlamp" } };
	demo.bs.picked = "box";
	demo.bs.title = "Storage Box";
	demo.bs.image = { ui::gfx::atlas(), { 0.30f, 0.05f, 0.36f, 0.12f } };
	{
		const ui::Color mat = ui::style().colour("BuildMaterial", {});
		demo.bs.stats = { { "Type:", "Interior" }, { "Materials cost:", "" }, { "Building Material", "4", mat, true }, { "Iron Plates", "2", mat, true },
			{ "Estimated build time:", "1 hrs" }, { "Power Consumption:", "-5", ui::style().colour("BuildPowerUse", {}) } };
	}
	demo.bs.variants = true;
	demo.bs.description = "A stand-in box for the demo: the host fills this from the building's record.";
	demo.bs.commands = "{BuildLabel}Place:\t{BuildKey}Left-Click\n{BuildLabel}Rotate:\t{BuildKey}, and .\n{BuildLabel}Undo:\t{BuildKey}Backspace\n{BuildLabel}Cancel all:\t{BuildKey}Esc";
	const ui::Color zone = ui::style().colour("ProspectZone", {});
	demo.ps.overview[0] = { { "Zone:", "Stand-in Flats", zone, zone }, { "Altitude:", "420 m" }, { "Prospector Skill:", "7" }, { "Area Size:", "380 m" } };
	demo.ps.overview[1] = { { "Wind Speed:", "9.5 mph" }, { "Environment:", "Arid 80%", { 0, 0, 0, 0 }, ui::style().colour("ProspectEnv", {}) } };
	demo.ps.resources = { { "iron", "Iron", 88 }, { "water", "Water", 62 }, { "copper", "Copper", 15 }, { "stone", "Stone", 100 } };
	demo.ps.selected = "stone";
	demo.ps.map = { ui::gfx::atlas(), { 0.30f, 0.05f, 0.55f, 0.32f } };	// any region will do as the stand-in map
	demo.ps.markerU = 0.5f;
	demo.ps.markerV = 0.4f;
	demo.ps.markerAngle = 0.6f;
	{
		std::vector<ui::Tutorial> all = {
			{ "camera", "Looking around", { { "Pan the view with the movement keys and turn it with the middle mouse button; the wheel zooms.\n\nTry both now.", true },
				{ "Double-click a portrait to follow that character. Press the follow key again to let go." } } },
			{ "squad", "Your squad", { { "The portraits along the bottom are your squad. Click one to select it, drag a box to select several." },
				{ "{Special}Orders\nRight-click the ground to walk there, a character to talk or fight, an item to pick it up." },
				{ "Hold Shift to queue orders one after another." } } },
			{ "hunger", "Food", { { "Characters get hungry. Keep some food in a backpack and they will eat on their own." } } } };
		for (int i = 1; i <= 14; i++) { all.push_back({ "extra" + std::to_string(i), "Placeholder topic " + std::to_string(i), { { "A stand-in page." } } }); }
		demo.tutorials->setAll(std::move(all));
	}

	ui::GearActions ga;
	ga.move = [](uint64_t, int from, const ui::GridItem& it, int to, int x, int y)
	{
		if (demo.ccs.trade.active && from == ui::kGearContainer && to != ui::kGearContainer && it.id == 21)	// the plating costs more than we have
		{
			demo.gearLog += "refused 21;";
			ui::message("You can't afford that.");
			return;
		}
		demo.gearLog += "move " + std::to_string(it.id) + " " + std::to_string(from) + ">" + std::to_string(to) + " " + std::to_string(x) + "," + std::to_string(y) + ";";
		demo.gearCount = it.count;
		if (from < ui::kGearOther && to < ui::kGearOther) { gearMove(from, it, to, x, y); }
	};
	ga.activate = [](uint64_t, int g, const ui::GridItem& it) { demo.gearLog += "use " + std::to_string(g) + " " + std::to_string(it.id) + ";"; };
	ga.context = [](uint64_t, int g, const ui::GridItem& it, float, float) { demo.gearLog += "menu " + std::to_string(g) + " " + std::to_string(it.id) + ";"; };
	ga.moveAll = [](uint64_t, int g, const ui::GridItem& it) { demo.gearLog += "all " + std::to_string(g) + " " + std::to_string(it.id) + ";"; };
	ga.dropOutside = [](uint64_t, int g, const ui::GridItem& it) { demo.gearLog += "drop " + std::to_string(g) + " " + std::to_string(it.id) + ";"; };
	ga.arrange = [](uint64_t, int g) { demo.gearLog += "arrange " + std::to_string(g) + ";"; };
	const auto gearMoveAct = ga.move;
	const auto gearContext = ga.context;
	const auto gearActivate = ga.activate;
	const auto gearDropOutside = ga.dropOutside;
	demo.gear = std::make_unique<ui::Gear>(std::move(ga));
	{
		// The gear's container, owned and wired here as ui::ItemWindows does (kenshi-k16.20.13.1.3): grid tag kGearContainer, the same log.
		ui::Widget& screen = ui::root().add<ui::Widget>();
		screen.halign = screen.valign = ui::Widget::Align::kStretch;
		demo.cont = std::make_unique<ui::ContainerWindow>(screen, [] { demo.gearLog += "close;"; });
		ui::ContainerWindow& cw = *demo.cont;
		cw.onArrange = [] { demo.gearLog += "arrange " + std::to_string(ui::kGearContainer) + ";"; };
		cw.onResearch = [] { demo.gearLog += "research;"; };
		ui::ItemGrid& grid = cw.grid();
		grid.tag = ui::kGearContainer;
		grid.onDrop = [&cw, gearMoveAct](ui::ItemGrid& from, const ui::GridItem& it, int x, int y)
		{
			const bool in = from.tag != cw.grid().tag;	// as ItemWindows' (audio_sfx_triggers.md 5)
			ui::sound(!in ? "Item_Lift" : cw.state().trade.active ? "Sell_Item" : "Item_Drop", "Inventory_Items", it.soundSwitch);
			gearMoveAct(demo.gs.owner, from.tag, it, ui::kGearContainer, x, y);
		};
		grid.onItemClick = [&grid, gearContext](int i, ui::Btn b)
		{
			grid.selected = i;
			if (b == ui::Btn::kRight) { gearContext(demo.gs.owner, ui::kGearContainer, grid.items[i], 0, 0); }
		};
		grid.onItemActivate = [&grid, gearActivate](int i) { gearActivate(demo.gs.owner, ui::kGearContainer, grid.items[i]); };
		grid.onDropOutside = [gearDropOutside](const ui::GridItem& it, float, float) { gearDropOutside(demo.gs.owner, ui::kGearContainer, it); };
		demo.gear->partner(demo.cont.get());
	}
	ui::GearActions la;
	la.move = [](uint64_t o, int from, const ui::GridItem& it, int to, int x, int y) { demo.gearLog += "move " + std::to_string(o) + " " + std::to_string(it.id) + " " + std::to_string(from) + ">" + std::to_string(to) + " " + std::to_string(x) + "," + std::to_string(y) + ";"; };
	la.context = [](uint64_t o, int g, const ui::GridItem& it, float, float) { demo.gearLog += "menu " + std::to_string(o) + " " + std::to_string(g) + " " + std::to_string(it.id) + ";"; };
	la.arrange = [](uint64_t o, int g) { demo.gearLog += "arrange " + std::to_string(o) + " " + std::to_string(g) + ";"; };
	la.close = [] { demo.gearLog += "loot close;"; };
	demo.lootGear = std::make_unique<ui::Gear>(std::move(la), ui::kGearOther);
	ui::ItemWindowsActions ia;
	const auto ref = [](ui::GridRef r) { return std::to_string(r.owner) + ":" + std::to_string(r.grid); };
	ia.move = [ref](ui::GridRef from, const ui::GridItem& it, ui::GridRef to, int x, int y)
	{
		demo.itemsLog += "move " + std::to_string(it.id) + " " + ref(from) + ">" + ref(to) + " " + std::to_string(x) + "," + std::to_string(y) + ";";
	};
	ia.quick = [ref](ui::GridRef at, const ui::GridItem& it, uint32_t mods) { demo.itemsLog += "quick " + ref(at) + " " + std::to_string(it.id) + " " + std::to_string(mods) + ";"; };
	ia.moveAll = [ref](ui::GridRef at, const ui::GridItem& it) { demo.itemsLog += "all " + ref(at) + " " + std::to_string(it.id) + ";"; };
	ia.arrange = [ref](ui::GridRef at) { demo.itemsLog += "arrange " + ref(at) + ";"; };
	ia.mark = [](uint64_t o) { demo.itemsLog += "mark " + std::to_string(o) + ";"; };
	ia.close = [](uint64_t o) { demo.itemsLog += "close " + std::to_string(o) + ";"; };
	ia.pin = [](uint64_t o, bool on) { demo.itemsLog += "pin " + std::to_string(o) + " " + std::to_string(on) + ";"; };
	ia.verb = [](ui::GridRef, const ui::GridItem&, uint32_t) { return demo.itemsVerb; };
	ia.menu = [](ui::GridRef, const ui::GridItem&)
	{
		return std::vector<ui::ItemMenuRow>{ { "Sell", "Right click", true, 1 }, { "Sell all", "Ctrl+Right click", true, 2 }, { "Equip", "", false, 3 } };
	};
	ia.pick = [ref](ui::GridRef at, const ui::GridItem& it, int a) { demo.itemsLog += "pick " + ref(at) + " " + std::to_string(it.id) + " " + std::to_string(a) + ";"; };
	demo.items = std::make_unique<ui::ItemWindows>(std::move(ia));

	// Looks strip: a few roles at odd sizes (stretching in the original style).
	ui::Panel& fp = root.add<ui::Panel>(ui::Look::kPanel, 30);
	fp.rect = { 560, 60, 420, 220 };
	ui::Label& fl = fp.add<ui::Label>("Floating panel (loading screen tips) stretched to 420 x 220.", "Kenshi_FloaterFont_Small");
	fl.wrap = true;
	fl.rect = { 0, 0, 360, 0 };
	ui::Panel& tb = root.add<ui::Panel>(ui::Look::kInset, 20);
	tb.rect = { 560, 300, 160, 140 };	// narrow: the progress bars (at 0.62 of the width) clear it at 5:4 (k16.3.41)
	ui::Label& tl = tb.add<ui::Label>("Inset text box", "Kenshi_StandardFont_Medium");
	tl.colour = ui::style().textColour(ui::Look::kInset, ui::State::kNormal, false);
	ui::Panel& fd = root.add<ui::Panel>(ui::Look::kField, 0);
	fd.rect = { 560, 460, 300, 38 };
	ui::Panel& lb = root.add<ui::Panel>(ui::Look::kBarFill);
	lb.rect = { 1080, 460, 300, 26 };
	ui::Panel& lbs = root.add<ui::Panel>(ui::Look::kBarStun);
	lbs.rect = { 1080, 492, 250, 26 };
	ui::Panel& lbh = root.add<ui::Panel>(ui::Look::kBarHurt);
	lbh.rect = { 1080, 524, 200, 26 };
	ui::Image& walk = root.add<ui::Image>("Kenshi_CharacterSpeedSet", "Walk");
	walk.rect.x = 1080;
	walk.rect.y = 600;
	ui::Image& run = root.add<ui::Image>("Kenshi_CharacterSpeedSet", "Run");
	run.rect.x = 1140;
	run.rect.y = 600;

	ui::toast("Toast line: messages fade after a few seconds");
	ui::toast("Hungry Bandits are attacking!", ui::res::colour("BadBright"), 6);
}

// The system clipboard for the edit boxes, as kclient sets it (the UI calls no sapp clipboard function: render hook 3, kenshi-k16.3.29).
void sappClipboard()
{
	ui::setClipboardFns([] { const char* c = sapp_get_clipboard_string(); return std::string(c ? c : ""); },
		[](const std::string& s) { sapp_set_clipboard_string(s.c_str()); });
}

void init()
{
	sg_desc sd = {};
	sd.environment = sglue_environment();
	sd.logger.func = slog_func;
	sg_setup(&sd);
	sgl_desc_t gd = {};
	gd.logger.func = slog_func;
	gd.max_vertices = 1 << 17;
	sgl_setup(&gd);
	stm_setup();
	cfg::load();
	if (!ui::init(demo.data, demo.themes)) { sapp_quit(); return; }
	ui::setViewport(sapp_width(), sapp_height());	// the UI calls no sapp size function (render hook 2, kenshi-k16.3.29)
	sappClipboard();
	{
		std::error_code ec;	// client/data, found as kclient does: the executable (client/build/kui_demo) two levels up
		const std::filesystem::path exe = kutil::executablePath(ec);
		demo.layoutDir = (exe.parent_path().parent_path() / "data").string();
		ui::setLayoutDir(demo.layoutDir);
	}
	build();
	ui::setSoundSink([](const ui::SoundPost& p) { demo.sounds += p.event + (p.group.empty() ? "" : "(" + p.group + "=" + p.value + ")") + ";"; });
	demo.last = stm_now();
}

// The options tab of the demo.* keys, the Debug section (its place depends on which other tabs exist).
ui::Widget* demoTab()
{
	for (int i = 0; i < 16 && demo.opts->tab(i); i++) { if (static_cast<const ui::Button*>(demo.opts->tab(i))->text == "Debug") { return demo.opts->tab(i); } }
	return nullptr;
}

// Synthetic input for the screenshot: hover the Recruit button (tooltip), then open a menu.
void script()
{
	auto send = [](sapp_event_type t, float x, float y, float scroll = 0, uint32_t mods = 0)
	{
		sapp_event e = {};
		e.modifiers = mods;
		e.type = t;
		e.mouse_x = x;
		e.mouse_y = y;
		e.scroll_y = scroll;
		e.mouse_button = SAPP_MOUSEBUTTON_LEFT;
		return ui::event(e);
	};
	auto click = [&](float x, float y, uint32_t mods = 0)
	{
		send(SAPP_EVENTTYPE_MOUSE_MOVE, x, y);
		const bool down = send(SAPP_EVENTTYPE_MOUSE_DOWN, x, y, 0, mods);
		const bool up = send(SAPP_EVENTTYPE_MOUSE_UP, x, y, 0, mods);
		return down && up;
	};
	// The middle of row i of a list of n single-line rows.
	struct Pt { float x, y; };
	auto clickOn = [&](const ui::Widget* w, uint32_t mods = 0) { const ui::Rect& r = w->screen(); return click(r.x + r.w / 2, r.y + r.h / 2, mods); };
	// The HUD's portrait bar paged until portrait i shows (a no-op while every portrait fits; k16.3.41).
	auto pageTo = [&](size_t i)
	{
		ui::Widget* spanW = demo.hud->squadSpan();
		for (int guard = 0; guard < 40 && !demo.hud->portrait(i)->shown(); guard++)
		{
			clickOn(demo.hud->squadArrow(demo.hud->squadFirst() > i ? -1 : 1));
			spanW->place(spanW->screen(), ui::scale());
		}
	};
	auto selectedIds = [] { std::string s; for (const ui::HudMember& m : demo.hs.squad) { if (m.selected) { s += std::to_string(m.id) + " "; } } return s; };
	auto check = [](bool ok, const char* what) { printf("kui_demo script: %s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) { demo.failed = true; } };
	if (demo.frame == 1)	// a portrait image on Greenlander (a stand-in texture, the straw hat's icon: the host's are Portraits::get cells)
	{
		demo.hs.squad[2].portrait = ui::icons::get(ui::icons::cached("575-gamedata.base", "2146-gamedata.base")).tex;
	}
	if (demo.frame == 2)
	{
		auto markIs = [](const ui::Widget* w, ui::PortraitMark m)
		{
			const ui::Image* im = static_cast<const ui::Image*>(w);
			const ui::Rect a = ui::portraitMarkArt(m);
			return im && (m == ui::PortraitMark::kNone ? !im->visible : im->visible && a.w > 0 && im->texels.x == a.x && im->texels.y == a.y);
		};
		check(markIs(demo.hud->portraitMark(2), ui::PortraitMark::kSlave) && markIs(demo.hud->portraitMark(3), ui::PortraitMark::kPrison)
			&& markIs(demo.hud->portraitMark(0), ui::PortraitMark::kNone) && ui::portraitMarkArt(ui::PortraitMark::kEaten).w > 0
			&& !demo.hud->portraitMark(2)->solid, "hud: portrait marks (shackle, bars) over the face from the game's art; none when unmarked");
		const ui::PortraitFace *f0 = demo.hud->portraitFace(0), *f1 = demo.hud->portraitFace(1), *f2 = demo.hud->portraitFace(2), *f3 = demo.hud->portraitFace(3);
		check(f0->back->visible && f0->back->texels.x == ui::portraitBackArt(ui::PortraitBack::kCombat).x && f1->back->visible
			&& f1->back->texels.x == ui::portraitBackArt(ui::PortraitBack::kHunger).x && !f2->back->visible && f0->back->texels.w > 0,
			"hud: the back overlay (Combat, Hunger) behind the face");
		check(f2->face->tint.a == 0.25f && f2->mark->tint.a == 1 && f3->face->tint.a == 1 && f0->hurtAlpha() == 0,
			"hud: a dimmed face at alpha 0.25, its mark at full");
		demo.hurtA = f1->hurtAlpha();
	}
	if (demo.frame == 6)
	{
		const float a = demo.hud->portraitFace(1)->hurtAlpha();
		check(a >= 0 && a <= 1 && demo.hurtA >= 0 && demo.hurtA <= 1 && a != demo.hurtA, ("hud: the Hurt background pulses (" + std::to_string(demo.hurtA) + " -> " + std::to_string(a) + ")").c_str());
	}
	if (demo.frame == 2)
	{
		const ui::Rect& r = demo.jobs->screen();
		check(click(r.x + 10, r.y + r.h / 2) && demo.jobs->checked, "click on a tick box is consumed and ticks it");
		const ui::Rect& l = demo.choices->screen();
		check(click(l.x + 20, l.y + 5) && demo.choices->selected == 0, "click on a list row picks it");
		check(!click(sapp_widthf() - 40, sapp_heightf() / 2), "click on empty screen falls through to the world");
		const ui::Rect& g = demo.log->screen();
		check(send(SAPP_EVENTTYPE_MOUSE_SCROLL, g.x + 20, g.y + 20, 1), "wheel over the log is consumed");
		check(!send(SAPP_EVENTTYPE_MOUSE_SCROLL, sapp_widthf() - 40, sapp_heightf() / 2, 1), "wheel over the world is not");
	}
	if (demo.frame == 3)
	{
		{
			// The status panel bottom left, the medical panel beside it, both clear of the squad tabs. It starts collapsed: the tag, plate and
			// state rows over the stats; the button slides them up to show the extended rows.
			const ui::Hud& h = *demo.hud;
			const ui::Rect in = h.infoPanel()->screen(), plate = h.infoPlate()->screen(), med = h.medicalPanel()->screen();
			const float u = ui::scale();
			check(h.infoPanel()->shown() && h.infoTag()->shown() && plate.w > 0 && std::abs(in.x) <= 1 && std::abs(in.bottom() - sapp_heightf()) <= 1
				&& std::abs(h.infoStats()->screen().h - 163 * u) <= 1 && std::abs(plate.w - 405 * u) <= 1 && plate.bottom() - 12 * u + 4 * u <= h.medicalStatus()->screen().y
				&& std::abs(med.bottom() - sapp_heightf()) <= 1 && med.right() <= h.squadTab(0)->screen().x,
				"status panel: in the screen's corner, the tag and the 405-wide plate above the stats (its box 4+ over the status box); the medical panel beside it on the foot, left of the squad tabs");
			check(h.infoTagText() == "[ESCAPED SLAVE]" && h.infoTagInk(false) == ui::res::colour("HudBad") && !h.infoTagTip().empty(),
				"status panel: the tag [ESCAPED SLAVE] in HudBad, brackets too, with its hover text");
			const auto* st = static_cast<const ui::Label*>(h.medicalStatus()->children().front().get());
			check(h.medicalStatus()->shown() && st->text == "Normal", "medical panel: the status box");
			const ui::Rect box = h.medicalStatus()->screen(), b0 = h.medicalBar(0)->screen(), b1 = h.medicalBar(1)->screen();
			check(std::abs(box.bottom() + 7 * u - med.y) <= 1 && std::abs(box.x - med.x) <= 1 && std::abs(box.w - med.w) <= 1 && std::abs(box.h - 37 * u) <= 1,
				"medical panel: the status box its own frame, 37 high, 7 over the panel and as wide (no name row)");
			check(std::abs(b0.h - 18 * u) <= 1 && std::abs(b1.y - b0.y - 24 * u) <= 1 && b0.y - med.y < 16 * u, "medical panel: bars 18 high at the original's 24 pitch, the first at the top");
			const ui::Rect ext = h.infoExtended()->screen(), stats = h.infoStats()->screen();
			check(!h.extended() && h.slide() == 0 && std::abs(ext.bottom() - (stats.bottom() - 4 * u)) <= 1 && std::abs(plate.bottom() - (ext.y + 6 * u)) <= 1
				&& ext.y + 112 * u < stats.y + 2 && ext.y + 112 * u > stats.y - 4 * u, "status panel: starts collapsed, the state rows showing above the stats");
			check(clickOn(h.infoButton()) && h.extended() && demo.extendLog == "up;", "status panel: the button extends it (the host's sound hook)");
			demo.extendAt = ui::now();
		}
		{
			auto text = [](const ui::Widget* w) { return static_cast<const ui::Label*>(w)->text; };
			const ui::Hud& h = *demo.hud;
			auto name = [&h](int i) { return static_cast<const ui::Label*>(h.medicalBar(i)->children()[0].get())->text; };
			check(h.medicalPanel()->shown() && text(h.medicalValue(5)) == "13" && h.medicalValue(8)->shown() && name(5) == "Stomach",
				"medical panel: the first selected member's parts after Blood, effective health (health - stun)");
			check(name(0) == "Blood" && text(h.medicalValue(0)) == "100" && std::fabs(h.medicalFill(0) - 100.0f / 120) < 0.01f
				&& h.medicalRight(0)->text == "<<<" && name(8) == "Hunger" && text(h.medicalValue(8)) == "228" && h.medicalRight(8)->text == ">"
				&& std::fabs(h.medicalFill(8) - 0.28f) < 0.01f && text(h.medicalValue(3)) == "123" && h.medicalName() == "Beep",
				"medical panel: Blood first (its max, the bleed's arrows), Hunger last (x 100, the green band, digesting), a robot leg over 100");
			const ui::Rect& r = demo.hud->medicalBar(0)->screen();
			check(click(r.x + r.w / 2, r.y + r.h / 2), "medical panel: a bar takes the mouse (tooltip), not the world");
		}
		{
			// The bottom-centre column spans the space between the corner clusters at any aspect (k16.3.41): the medical panel left of it, the
			// diamonds and shortcuts right; the tabs and the portrait bar inside it. Portraits that don't fit page behind < > arrows.
			const ui::Hud& h = *demo.hud;
			const ui::Rect span = h.squadSpan()->screen(), bar = h.squadBar()->parent()->screen(), tabs = h.squadTab(0)->parent()->screen();
			const float u = ui::scale();
			const float right = std::min(h.shortcutsPanel()->screen().x, std::min(h.floorDiamond()->screen().x, h.moveSpeedDiamond()->screen().x));
			check(span.x >= h.medicalPanel()->screen().right() + 8 * u - 1 && span.right() <= right - 8 * u + 1 && bar.x >= span.x - 1 && bar.right() <= span.right() + 1
				&& tabs.x >= span.x - 1 && tabs.right() <= span.right() + 1 && std::abs(bar.x + bar.w / 2 - (span.x + span.w / 2)) <= 1,
				"squad bar: centred in the space between the medical panel and the diamonds / shortcuts, the tabs and portraits inside it");
			const ui::Widget* prev = h.squadArrow(-1);
			const ui::Widget* next = h.squadArrow(1);
			size_t shownN = 0;
			for (size_t i = 0; h.portrait(i); i++) { shownN += h.portrait(i)->shown(); }
			const size_t n = demo.hs.squad.size();
			if (shownN == n) { check(!prev->shown() && !next->shown() && h.squadFirst() == 0, "squad bar: every portrait fits, no page arrows"); }
			else
			{
				check(prev->shown() && next->shown() && !prev->enabled && next->enabled && shownN >= 1 && h.portrait(0)->shown() && !h.portrait(n - 1)->shown(),
					("squad bar: " + std::to_string(shownN) + " of " + std::to_string(n) + " portraits shown, the arrows at its ends, < off at the start").c_str());
				ui::Widget* spanW = h.squadSpan();
				auto relayout = [&] { spanW->place(spanW->screen(), u); };
				const ui::Rect p0 = h.portrait(0)->screen();
				send(SAPP_EVENTTYPE_MOUSE_MOVE, p0.x + p0.w / 2, p0.y + p0.h / 2);
				send(SAPP_EVENTTYPE_MOUSE_SCROLL, p0.x + p0.w / 2, p0.y + p0.h / 2, -1);
				relayout();
				check(h.squadFirst() == 1 && !h.portrait(0)->shown() && h.portrait(1)->shown() && prev->enabled, "squad bar: the wheel over it pages one portrait on");
				check(clickOn(prev) && (relayout(), h.squadFirst() == 0 && h.portrait(0)->shown()), "squad bar: < pages back");
				check(clickOn(next) && (relayout(), h.squadFirst() == 1), "squad bar: > pages on");
			}
		}
		pageTo(2);
		check(clickOn(demo.hud->portrait(2)) && selectedIds() == "102 ", "portrait click selects only that character (through its image)");
		check(demo.hud->portraitInitial(2)->text.empty() && demo.hud->portraitInitial(1)->text == "R" && static_cast<ui::Button*>(demo.hud->portrait(1))->text.empty(),
			"a portrait image replaces the initial; none keeps it");
		pageTo(3);
		check(clickOn(demo.hud->portrait(3), SAPP_MODIFIER_SHIFT) && selectedIds() == "102 103 ", "Shift + portrait click adds to the selection");
	}
	if (demo.frame == 4)	// the toggles follow the selection a frame later, as with the sim
	{
		demo.hs.squad[2].status = "Discovered!";
		demo.hs.squad[2].statusColour = "TipBad";
		const int sneak = (int)ui::HudToggle::kSneak;
		check(clickOn(demo.hud->toggleButton(ui::HudToggle::kSneak)) && demo.hs.squad[2].toggles[sneak] && demo.hs.squad[3].toggles[sneak] && !demo.hs.squad[0].toggles[sneak],
			"SNEAK toggles the selection");
		{
			const ui::Rect o = demo.hud->ordersPanel()->screen(), b = demo.hud->toggleButton(ui::HudToggle::kBlock)->screen(), k = demo.hud->toggleButton(ui::HudToggle::kSneak)->screen();
			const float u = ui::scale();
			check(std::abs(o.right() - sapp_widthf()) <= 1 && std::abs(o.bottom() - sapp_heightf()) <= 1 && std::abs(o.w - 483 * u) <= 1 && std::abs(o.h - 193 * u) <= 1
				&& std::abs(b.x - o.x - 8 * u) <= 1 && std::abs(b.y - o.y - 6 * u) <= 1 && std::abs(k.y - b.y - 156 * u) <= 1,
				"orders panel: the bottom-right corner, 483 x 193; the ticks BLOCK .. SNEAK down its left");
			const ui::Rect j = demo.hud->jobList()->screen(), r = demo.hud->rescueButton()->screen(), p = demo.hud->prospectButton()->screen();
			check(std::abs(j.x - o.x - 128 * u) <= 1 && std::abs(j.y - o.y - 7 * u) <= 1 && r.y >= j.bottom() && std::abs(p.right() - (o.right() - 8 * u)) <= 1
				&& std::abs(p.y - r.y) <= 1, "orders panel: the jobs list inside it, RESCUE .. PROSPECT in a row under it");
			// The rest of the cluster at Dark UI's spots (1920 x 1080 rects, kept from the bottom-right corner at any width).
			auto at = [u](const ui::Widget* w, float x, float y, float rw, float rh)
			{
				const ui::Rect s = w->screen();
				const float W = sapp_widthf(), H = sapp_heightf();
				return std::abs(s.right() - (W - (1920 - x - rw) * u)) <= 1 && std::abs(s.bottom() - (H - (1080 - y - rh) * u)) <= 1 && std::abs(s.w - rw * u) <= 1
					&& std::abs(s.h - rh * u) <= 1;
			};
			check(at(demo.hud->timePanel(), 1437, 824, 202, 58) && at(demo.hud->moneyBox(), 1645, 824, 191, 58) && at(demo.hud->buildButton()->parent(), 1842, 824, 78, 58)
				&& at(demo.hud->floorDiamond(), 1374, 743, 101, 101) && at(demo.hud->moveSpeedDiamond(), 1334, 812, 101, 101) && at(demo.hud->shortcutsPanel(), 1354, 914, 77, 166)
				&& demo.hud->timePanel()->screen().x == demo.hud->ordersPanel()->screen().x,
				"bottom right: speed buttons (on the orders panel's left), Money / Day box, build button, the diamonds and the shortcuts where Dark UI draws them, 6 apart");
			demo.shortcutLog.clear();
			check(clickOn(demo.hud->buildButton()) && demo.shortcutLog == "toggle_build;", ("hud: the build button sends toggle_build (" + demo.shortcutLog + ")").c_str());
			demo.shortcutLog.clear();
			check(demo.hud->dayText() == "Day: 12" && demo.hud->clockText().size() == 5 && demo.hud->clockText()[2] == ':', "money box: Day: N and hh:mm under the money");
			demo.menuLog.clear();
			check(!demo.hud->toggleButton(ui::HudToggle::kJobs)->enabled && demo.hud->toggleButton(ui::HudToggle::kJobs)->shown() && !demo.hud->medicButton()->enabled
				&& !clickOn(demo.hud->toggleButton(ui::HudToggle::kTaunt)) && !demo.hs.squad[2].toggles[(int)ui::HudToggle::kTaunt] && clickOn(demo.hud->prospectButton())
				&& demo.menuLog == "prospect;",
				"orders panel: ticks with no order disabled, not hidden; MEDIC off; PROSPECT is the host's");
			demo.menuLog.clear();
		}
		check(clickOn(demo.hud->speedButton(2)) && demo.hs.speed == 2, "speed button sets x2");
		check(clickOn(demo.hud->speedButton(3)) && demo.hs.speed == 5, "the third speed button sets x5 (the original's)");
		check(clickOn(demo.hud->speedButton(2)) && demo.hs.speed == 2, "back to x2");
		check(clickOn(demo.hud->speedButton(0)) && demo.hs.paused, "pause button pauses");
		demo.hud->update(demo.hs);
		check(demo.hud->pausedPanel()->visible, "PAUSED shows while paused");
	}
	if (demo.extendAt >= 0 && ui::now() - demo.extendAt > ui::Hud::kSlideTime + 0.05)
	{
		const ui::Hud& h = *demo.hud;
		const float u = ui::scale();
		const ui::Rect ext = h.infoExtended()->screen(), stats = h.infoStats()->screen();
		check(h.slide() == 1 && std::abs(ext.bottom() - (stats.bottom() - 163 * u)) <= 1 && std::abs(h.infoPlate()->screen().bottom() - (ext.y + 6 * u)) <= 1,
			"status panel: after half a second the extended rows stand on the stats panel");
		demo.extendAt = -2;
	}
	if (demo.frame == 5)
	{
		check(demo.hud->slide() > 0, ("status panel: sliding up (" + std::to_string(demo.hud->slide()) + ")").c_str());
		{
			const auto* st = static_cast<const ui::Label*>(demo.hud->medicalStatus()->children().front().get());
			check(st->text == "Discovered!" && st->colour == ui::res::colour("TipBad"), "medical panel: Discovered! in the status box, red");
		}
		{
			// The diamonds: the floor selector, the move speed down and left of it; each half its own button, the hit area the diamond (a corner
			// of its rect falls through).
			const ui::Hud& h = *demo.hud;
			ui::Diamond& fl = *h.floorDiamond();
			ui::Diamond& mv = *h.moveSpeedDiamond();
			const ui::Rect f = fl.screen(), m = mv.screen();
			const float u = ui::scale();
			check(fl.shown() && mv.shown() && std::abs(f.w - 101 * u) <= 1 && std::abs(m.x - (f.x - 40 * u)) <= 1 && std::abs(m.y - (f.y + 69 * u)) <= 1
				&& fl.text == "Floor 0", "diamonds: the floor (floor 0 reads Floor 0, 101 x 101), the move speed 40 left and 69 down");
			auto at = [](const ui::Rect& r, float fx, float fy) { return std::pair{ r.x + r.w * fx, r.y + r.h * fy }; };
			const auto [ux, uy] = at(f, 0.5f, 0.18f);
			const auto [dx, dy] = at(f, 0.5f, 0.82f);
			check(fl.secondTip.starts_with("Storm House\n") && fl.firstTip.starts_with("Storm House\n"), "floor diamond: the building it acts on in the tooltips");
			check(!fl.secondEnabled && fl.secondTip.ends_with("\nDown a floor") && click(dx, dy) && demo.hs.floor == 0, "floor diamond: down off on the ground, its tip still Down a floor");
			check(click(ux, uy) && demo.hs.floor == 1, "floor diamond: up steps up");
			demo.hud->update(demo.hs);
			check(fl.text == "Floor 1" && click(ux, uy) && demo.hs.floor == 2, "floor diamond: the host's floor shown, up again");
			demo.hud->update(demo.hs);
			check(fl.secondEnabled && !fl.firstEnabled && click(ux, uy) && demo.hs.floor == 2 && click(dx, dy) && demo.hs.floor == 1,
				"floor diamond: up disabled at the top, down steps down");
			const auto [lx, ly] = at(m, 0.18f, 0.5f);
			const auto [rx, ry] = at(m, 0.82f, 0.5f);
			check(h.speedAtlas() == (ui::res::themed("gfx/Kenshi_UI.png") && ui::res::images("Kenshi_CharacterSpeedSet") != nullptr),
				h.speedAtlas() ? "speed diamond: the theme's atlas icons (walk / jog / run / group)" : "speed diamond: our drawn figures (no themed atlas)");
			check(demo.hs.moveSpeed == 2 && click(rx, ry) && demo.hs.moveSpeed == 3, "speed diamond: right picks group from run (the default)");
			demo.hud->update(demo.hs);
			check(click(rx, ry) && demo.hs.moveSpeed == 0, "speed diamond: right wraps group to walk");
			demo.hud->update(demo.hs);
			check(click(lx, ly) && demo.hs.moveSpeed == 3, "speed diamond: left wraps walk to group");
			demo.hs.moveSpeed = 2;
			demo.hud->update(demo.hs);
			check(click(lx, ly) && demo.hs.moveSpeed == 1, "speed diamond: left steps run to jog");
			demo.hs.moveSpeed = 2;
			check(mv.hit(m.x + 3 * u, m.y + 3 * u) == nullptr && fl.hit(f.x + 3 * u, f.bottom() - 3 * u) == nullptr && mv.hit(m.x + m.w / 2, m.y + m.h / 2) == &mv,
				"diamonds: the rect's corners are not the diamond");
			const bool keepUp = demo.hs.floorUp, keepDown = demo.hs.floorDown;
			const std::string keepName = demo.hs.floorName;
			demo.hs.floorUp = demo.hs.floorDown = false;
			demo.hs.floorName.clear();
			demo.hud->update(demo.hs);
			check(fl.shown() && fl.text == "Floor" && !fl.firstEnabled && !fl.secondEnabled && click(ux, uy) && demo.hs.floor == 1,
				"floor diamond: no target keeps it up, \"Floor\" with no number, both arrows off");
			demo.hs.floorUp = keepUp;
			demo.hs.floorDown = keepDown;
			demo.hs.floorName = keepName;
			demo.hs.floorValid = false;
			demo.hud->update(demo.hs);
			check(!fl.shown() && mv.shown(), "floor diamond: hidden while the host sets floorValid false");
			demo.hs.floorValid = true;
			demo.hud->update(demo.hs);
			// Direct drive (k16.3.32): both diamonds draw active while the host says so; their arrows still step.
			check(!fl.active && !mv.active, "diamonds: not active while not driving");
			demo.hs.driving = true;
			demo.hud->update(demo.hs);
			check(fl.active && mv.active && click(ux, uy) && demo.hs.floor == 2 && click(dx, dy) && demo.hs.floor == 1,
				"diamonds: driving draws both active, the floor arrows still step");
			demo.hs.driving = demo.showDriving;	// --show driving: left on for the screenshot
			demo.hud->update(demo.hs);
		}
		{
			// The inspect focus: an NPC in the medical panel, the selection untouched; unloaded, both panels empty frames.
			ui::HudState& hs = demo.hs;
			const ui::HudInfo info = hs.info;
			hs.focus = hs.squad[0];
			hs.focus.id = 900;
			hs.focus.name = "Wanderer";
			hs.focus.selected = false;
			hs.focusSet = true;
			demo.hud->update(hs);
			const ui::Hud& h = *demo.hud;
			check(h.medicalPanel()->shown() && h.medicalName().starts_with("Wanderer") && h.medicalValue(0)->shown() && selectedIds() == "102 103 ",
				"inspect focus: the medical panel draws it, the selection unchanged");
			hs.focus.parts.clear();
			hs.info.blank = true;
			demo.hud->update(hs);
			check(h.medicalPanel()->shown() && h.medicalName().empty() && !h.medicalValue(0)->shown() && !h.medicalStatus()->shown(),
				"inspect focus: unloaded, the medical panel an empty frame");
			check(h.infoPanel()->shown() && h.infoStats()->shown() && h.infoName().empty() && h.infoRows() == 0 && !h.infoTag()->shown(),
				"inspect focus: blank, the status panel empty frames");
			hs.focusSet = false;
			hs.info = info;
			demo.hud->update(hs);
			check(!h.medicalName().empty() && !h.medicalName().starts_with("Wanderer"), "inspect focus: cleared, the selection's member again");
		}
		pageTo(2);
		demo.hoverMe = demo.hud->portrait(2);
		const ui::Rect& r = demo.hoverMe->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 2, r.y + r.h / 2);
	}
	if (demo.frame == 6)
	{
		std::vector<ui::Menu::Item> items = { { "Hungry Bandit", nullptr, true, true }, { "Attack", [] { ui::toast("Attack"); } }, { "Talk", [] { ui::toast("Talk"); } }, { "Loot", nullptr, false }, { "Steal", [] { ui::toast("Steal"); } } };
		items.back().value = "45%";
		auto& m = static_cast<ui::ContextMenu&>(ui::openMenu(std::make_unique<ui::ContextMenu>(std::move(items)), sapp_widthf() * 0.75f, sapp_heightf() * 0.55f));
		const float s = ui::scale();
		check(std::abs(m.screen().h - (34.5f + 4 * 37.5f + 1.5f) * s) < 1, "context menu: a 34.5 title, four 37.5 rows, 1.5 under");
		const ui::Rect t = m.row(0), r1 = m.row(1), r4 = m.row(4);
		check(m.itemAt(t.y + t.h / 2) == 0 && m.itemAt(r1.y + r1.h / 2) == 1 && m.itemAt(r4.y + r4.h / 2) == 4 && m.itemAt(m.screen().bottom() + 1) == -1, "context menu: rows hit");
		check(std::abs(r1.w - m.screen().w + 51 * s) < 1 && std::abs(r1.x - m.screen().x - 4.5f * s) < 1 && std::abs(r1.h - 34.5f * s) < 1 && r4.w == r1.w, "context menu: a value anywhere narrows every button (189 x 34.5 at 4.5 at 16:9)");
		check(std::abs(m.screen().w - 0.125f * sapp_widthf()) <= 1, "context menu: an eighth of the screen wide");
		bool original = false;	// the style in effect (ui.style takes effect at start, before the options check changes it)
		for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.style") { original = i.value == "original"; } }
		const ui::Color normal = ui::style().textColour(ui::Look::kButton, ui::State::kNormal, false), hot = ui::style().textColour(ui::Look::kButton, ui::State::kHighlighted, false);
		check(normal != hot && (!original || hot.r > hot.b + 0.2f), "context menu: the hovered caption changes colour (original: gold, the face unchanged by Kenshi_Button1Skin)");
		ui::closePopups();
		auto& wide = ui::openContextMenu(100, 100, "A Very Long Name For A Storage Box Of Many Things", { { "Attack Unprovoked And Then Some More" } }, nullptr);
		check(std::abs(wide.screen().w - 0.125f * sapp_widthf()) <= 1, "context menu: long labels do not widen it");
		ui::closePopups();
	}
	auto key = [&](sapp_keycode k)
	{
		sapp_event e = {};
		e.type = SAPP_EVENTTYPE_KEY_DOWN;
		e.key_code = k;
		return ui::event(e);
	};
	if (demo.frame == 3)	// the control hints panel is off by default: a host block shows nothing (kenshi-k16.2.294.3)
	{
		ui::setHints({ "Beep", { { "Click", "Select" } } });
		check(!ui::hintsShown() && ui::shownHints().lines.empty(), "hints: ui.control_hints off by default, a fed block stays hidden");
		ui::setHints({});
	}
	if (demo.frame == 119 && demo.showHints)
	{
		// our own stand-in wording; the host's resolver feeds these each frame
		const ui::HintBlock sample = { "Beep", { { "Click", "Select Beep" }, { "Shift+Click", "Add to the selection" },
			{ "Double click", "Select and follow with the camera" }, { "Right click", "Order the selection to follow" },
			{ "Ctrl+Click", "Knock out", false, "not hostile" } } };
		ui::setHints(sample);
		check(!ui::hintsShown() && key(SAPP_KEYCODE_F6) && ui::hintsShown(), "hints: F6 (toggle_hints) turns the panel on");
		const ui::HintBlock b = ui::shownHints();
		check(b.title == "Beep" && b.lines.size() == 5 && !b.lines[4].enabled && b.lines[4].why == "not hostile", "hints: the host's block shows, the greyed line with its reason");
		ui::Button& btn = ui::root().add<ui::Button>("Recruit");
		btn.rect = { 40, 300, 160, 40 };
		btn.halign = ui::Widget::Align::kEnd;
		btn.solid = true;
		btn.clickHint = "Hire them";
		btn.doubleClickHint = "Hire and open the squad";
		ui::frame(0);
		const ui::Rect& r = btn.screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 2, r.y + r.h / 2);
		const ui::HintBlock w = ui::shownHints();
		check(ui::hoveredWidget() == &btn && w.title == "Recruit" && w.lines.size() == 2 && w.lines[0].input == "Click" && w.lines[1].label == "Hire and open the squad",
			"hints: a hovered button's clickHint / doubleClickHint replace the host's block");
		ui::root().remove(&btn);
		sapp_event e = {};
		e.type = SAPP_EVENTTYPE_MOUSE_LEAVE;
		ui::event(e);
		check(!ui::hoveredWidget() && ui::shownHints().title == "Beep", "hints: off the widget, the host's block again");
	}
	if (demo.frame == 100 && demo.showToast)
	{
		ui::clearMessages();
		for (int i = 0; i < 5; i++) { ui::toast("Toast line: messages fade after a few seconds", ui::res::colour("Main"), 30); }	// stand-in wording; one bubble, (x5)
		ui::toast("Hungry Bandits are attacking!", ui::res::colour("BadBright"), 30);
		ui::toast("A longer development message that does not fit on one line wraps inside its own bubble, which grows downwards to hold it all", ui::res::colour("Main"), 30);
	}
	if (demo.frame == 101 && demo.showToast)
	{
		// The toast line's rules (kenshi-k16.2.300.5.2) on a line of its own, the shown one kept for the shot. Time is the UI's (ui::frame's dt):
		// the game paused, it still runs.
		ui::Toasts t;
		const ui::Color c = ui::res::colour("Main");
		demo.hs.paused = true;
		demo.hud->update(demo.hs);
		for (int i = 0; i < 5; i++) { t.push("Same text", c, 4); }
		check(t.shown() == 1 && t.count("Same text") == 5, "toast: the same text pushed 5 times is one toast, counted 5");
		ui::frame(3.9);
		check(!t.empty(), "toast: up until its seconds");
		ui::frame(0.1 + ui::Toasts::kFade);
		check(t.empty(), "toast: gone after its seconds and the fade, the game paused (UI time)");
		for (int i = 0; i < 5; i++) { t.push("Toast " + std::to_string(i), c, 4); }
		check(t.shown() == 3 && t.count("Toast 1") == 0 && t.count("Toast 2") == 1 && t.count("Toast 4") == 1, "toast: 5 different texts, at most 3 shown, the oldest gone first");
		t.clear();
		t.push("A", c, 4);
		t.push("B", c, 4);
		ui::frame(3);
		t.push("A", c, 4);
		ui::frame(2);
		check(t.shown() == 1 && t.count("A") == 2 && t.count("B") == 0, "toast: a repeat restarts its time");
		demo.hs.paused = false;
		demo.hud->update(demo.hs);
	}
	if (demo.frame == 102 && demo.showToast)
	{
		const ui::Toasts* t = ui::toasts();
		const std::vector<ui::Rect>& b = t->bubbles();
		check(b.size() == 3 && t->count("Toast line: messages fade after a few seconds") == 5, "toast: one bubble per message, a repeated one counted");
		bool centred = true, stacked = true, sized = true;
		const float mid = sapp_widthf() / 2, lh = t->font->lineHeight();
		for (size_t i = 0; i < b.size(); i++)
		{
			centred = centred && std::abs(b[i].x + b[i].w / 2 - mid) <= 1;
			sized = sized && b[i].h > lh && b[i].w > b[i].h;
			if (i) { stacked = stacked && b[i].y > b[i - 1].bottom(); }
		}
		check(centred && stacked && sized, "toast: the bubbles are centred, stacked top down with a gap, padded around their text");
		check(b.size() == 3 && b[2].h > b[1].h * 1.5f && b[0].h == b[1].h, "toast: a long message wraps inside a taller bubble");
		check(t->bubbleAlpha == 0.8f, "toast: the bubble is 0.8 black");
	}
	if (demo.frame == 124 && demo.showToast) { ui::root().visible = false; }	// the shot: the toasts straight on the background, no window over them
	if (demo.frame == 120 && demo.showHints)
	{
		check(key(SAPP_KEYCODE_F6) && !ui::hintsShown() && key(SAPP_KEYCODE_F6) && ui::hintsShown(), "hints: F6 again hides it, a third press shows it");
		// Docked above the bottom-right cluster (kenshi-k16.2.294.10.1): its foot one panel gap over the speed / money row, its right edge
		// on the BuildPanel's; more lines grow it upward. Run at two window sizes (docs/UI.md "Control hints").
		const ui::Hud& h = *demo.hud;
		const ui::Rect r = ui::hintsRect(), row = h.timePanel()->screen(), bp = h.buildButton()->parent()->screen();
		const float gap = h.panelGap();
		check(h.shown() && gap > 0 && std::abs(h.moneyBox()->screen().x - row.right() - gap) <= 1,
			("hints: the HUD's panel gap from its layout (" + std::to_string(gap) + " px)").c_str());
		check(std::abs(row.y - gap - r.bottom()) <= 1 && r.right() == bp.right() && r.x > row.x,
			("hints: docked, foot " + std::to_string(row.y - r.bottom()) + " px over the speed row (gap " + std::to_string(gap) + "), right edge on the HUD's ("
				+ std::to_string(r.right()) + " / " + std::to_string(bp.right()) + ")").c_str());
		const ui::HintBlock five = ui::shownHints();
		ui::HintBlock two = five;
		two.lines.resize(2);
		ui::setHints(two);
		const ui::Rect r2 = ui::hintsRect();
		ui::setHints(five);
		check(r2.bottom() == r.bottom() && r2.y > r.y && r2.right() == r.right(), "hints: fewer lines, a shorter panel on the same foot (it grows upward)");
		demo.hud->show(false);
		const ui::Rect u = ui::hintsRect();
		demo.hud->show(true);
		check(u.right() == ui::root().screen().right() && std::abs(u.y + u.h / 2 - ui::root().screen().h / 2) <= 1, "hints: the HUD hidden, the panel against the right edge, centred");
	}
	if (demo.frame == 7)
	{
		{
			// held mode (hold RMB): the release over a row picks it; over the title it only closes; no greyed rows
			std::vector<ui::Menu::Item> items = { { "Gate", nullptr, false, true }, { "Open", [] { demo.menuLog += "ctx open;"; } }, { "Unlock", [] { demo.menuLog += "ctx unlock;"; }, false } };
			auto open = [&] { return &static_cast<ui::ContextMenu&>(ui::openMenu(std::make_unique<ui::ContextMenu>(items), 100, 100, true)); };
			demo.menuLog.clear();
			ui::Rect r = open()->row(0);
			send(SAPP_EVENTTYPE_MOUSE_UP, r.x + r.w / 2, r.y + r.h / 2);
			check(demo.menuLog.empty() && !ui::popupsOpen(), "context menu: the title only closes it");
			r = open()->row(2);
			send(SAPP_EVENTTYPE_MOUSE_UP, r.x + r.w / 2, r.y + r.h / 2);
			check(demo.menuLog == "ctx unlock;", ("context menu: no greyed rows, a row passed disabled picks (" + demo.menuLog + ")").c_str());
			demo.menuLog.clear();
			const ui::ContextMenu* plain = open();
			r = plain->row(1);
			check(std::abs(r.w - plain->screen().w + 9 * ui::scale()) < 1, "context menu: no values, the buttons are the width less 9 (231 at 16:9)");
			send(SAPP_EVENTTYPE_MOUSE_UP, r.x + r.w / 2, r.y + r.h / 2);
			check(demo.menuLog == "ctx open;", ("context menu: the release picks Open (" + demo.menuLog + ")").c_str());
			demo.menuLog.clear();
			ui::Rect m = open()->screen();	// any button: the left release picks from the menu a right hold opened
			send(SAPP_EVENTTYPE_MOUSE_UP, m.right() + 50, m.bottom() + 50);
			check(!ui::popupsOpen() && demo.menuLog.empty(), "context menu: a release elsewhere closes it");
			open();
			check(!key(SAPP_KEYCODE_ESCAPE) && ui::popupsOpen(), "context menu: Escape passes it by");
			ui::closePopups();
		}
		{
			// a building's menu at the cursor and its occupant's with its right edge there; the release picks from either
			std::vector<ui::Menu::Item> bed = { { "Bed", nullptr, false, true }, { "Sleep", [] { demo.menuLog += "bed;"; } } };
			std::vector<ui::Menu::Item> who = { { "Hurt Drifter", nullptr, false, true }, { "Pick Up", [] { demo.menuLog += "who;"; } } };
			const float cx = 600, cy = 300;
			ui::ContextMenu& a = static_cast<ui::ContextMenu&>(ui::openMenu(std::make_unique<ui::ContextMenu>(bed), cx, cy, true));
			ui::ContextMenu& b = static_cast<ui::ContextMenu&>(ui::openMenu(std::make_unique<ui::ContextMenu>(who), cx, cy, true, true, true));
			check(std::abs(a.screen().x - cx) < 1 && std::abs(b.screen().right() - cx) < 1 && a.screen().y == b.screen().y, "context menu: two at once, the occupant's right edge at the cursor");
			const ui::Rect r = b.row(1);
			send(SAPP_EVENTTYPE_MOUSE_UP, r.x + r.w / 2, r.y + r.h / 2);
			check(demo.menuLog == "who;" && !ui::popupsOpen(), ("context menu: the release over the occupant's picks it and closes both (" + demo.menuLog + ")").c_str());
			demo.menuLog.clear();
		}
		{
			// the host's form: title, rows, onPick(index); the occupant's menu may run off the left edge
			int picked = -1;
			ui::ContextMenu& a = ui::openContextMenu(40, 300, "Secure Wooden Chest", { { "Pick Lock", 29 }, { "Use Tools" }, { "Use Strength" } }, [&](int i) { picked = i; });
			check(ui::contextMenuShown() && a.items.size() == 4 && a.items[1].value == "29%" && a.items[2].value.empty(), "context menu: openContextMenu, a title and three rows, Pick Lock 29%");
			check(std::abs(a.row(3).w - a.screen().w + 51 * ui::scale()) < 1, "context menu: one value narrows every row");
			ui::ContextMenu& b = ui::openContextMenu(40, 300, "Sleeper", { { "Pick Up" } }, [&](int i) { picked = 10 + i; }, true);
			check(ui::popupsOpen() && b.screen().x < 0 && std::abs(b.screen().right() - 40) < 1 && std::abs(a.screen().x - 40) < 1, "context menu: the second's right edge at the cursor, past the left edge");
			const ui::Rect v = a.row(1);
			send(SAPP_EVENTTYPE_MOUSE_UP, a.screen().right() - 4, v.y + v.h / 2);
			check(picked == -1 && !ui::popupsOpen(), "context menu: a release over the Value box closes both, no order");
			ui::ContextMenu& c = ui::openContextMenu(40, 300, "Secure Wooden Chest", { { "Pick Lock", 29 }, { "Use Tools" }, { "Use Strength" } }, [&](int i) { picked = i; });
			const ui::Rect r = c.row(3);
			send(SAPP_EVENTTYPE_MOUSE_UP, r.x + r.w / 2, r.y + r.h / 2);
			check(picked == 2 && !ui::contextMenuShown(), ("context menu: the release picks Use Strength, index 2 (" + std::to_string(picked) + ")").c_str());
		}
		{
			using C = ui::Color;
			const C red = ui::ContextMenu::valueColour("0%"), mid = ui::ContextMenu::valueColour("50%"), top = ui::ContextMenu::valueColour("120%");
			check(red == C{ 1, 0, 0, 1 } && mid == C{ 0.5f, 0.5f, 0, 1 } && top == C{ 0, 1, 0, 1 } && ui::ContextMenu::valueColour("?") == ui::res::colour("Accent"), "context menu: values red -> yellow -> green");
		}
		ui::closePopups();
		cfg::set("demo.gamma=1.5");	// a command-line pin: shown, enabled, the change ends it (k16.2.303.1)
		demo.opts->show(true);
	}
	if (demo.frame == 8)
	{
		check(demoTab() && clickOn(demoTab()), "options: the Debug tab (the demo.* rows)");
		demo.opts->reveal("demo.volume");	// the kui_demo group, the tab's last: its rows scrolled into view (kenshi-k16.2.244.2)
	}
	if (demo.frame == 9)
	{
		const ui::Widget* vol = demo.opts->control("demo.volume");
		const ui::Widget* sub = demo.opts->control("demo.subtitles");
		const ui::Widget* gamma = demo.opts->control("demo.gamma");
		check(vol && sub && gamma, "options: a row per demo option");
		if (vol && sub && gamma)
		{
			const ui::Rect& r = vol->screen();
			const bool clicked = click(r.x + r.w * 0.3f, r.y + r.h / 2);
			check(clicked && kVolume.get() > 0 && kVolume.get() < 50, ("options: a click on the slider track sets the value (" + std::to_string(kVolume.get()) + ")").c_str());
			check(clickOn(sub) && kSubtitles.get(), "options: the tick box sets a Bool");
			auto gammaPinned = [] { for (const cfg::Info& i : cfg::list()) { if (i.key == "demo.gamma") { return i.pinned; } } return false; };
			check(gamma->enabled && kGamma.get() == 1.5 && gammaPinned() && gamma->tip().find("command line") != std::string::npos,
				"options: a pinned (--set) option is enabled, its tooltip says so");
			const ui::Rect& g = gamma->screen();
			const bool moved = click(g.x + g.w * 0.3f, g.y + g.h / 2);	// before the message reads the value (argument order)
			check(moved && kGamma.get() != 1.5 && !gammaPinned(),
				("options: changing a pinned option ends its pin (" + std::to_string(kGamma.get()) + ")").c_str());
			cfg::set("demo.gamma=1.5");
			{	// setUser to the value already in effect still ends the pin and counts as a change (k16.2.303.2)
				const uint64_t gen = cfg::generation();
				kGamma.set(1.5);
				check(!gammaPinned() && cfg::generation() > gen, "options: setUser to the value in effect ends the pin and bumps the generation");
			}
			cfg::set("demo.gamma=1.5");	// pinned again for the reset check and the --show options shot
		}
	}
	if (demo.frame == 10)
	{
		check(key(SAPP_KEYCODE_ESCAPE) && !demo.opts->shown(), "options: Escape closes the window");
		demo.opts->show(true);
		demo.opts->update();
		ui::frame(0);
		const ui::Widget* vol = demo.opts->control("demo.volume");
		ui::Button* rv = demo.opts->reset("demo.volume");
		check(rv && rv->shown() && demo.opts->reset("demo.subtitles")->shown() && demo.opts->reset("demo.gamma")->shown() && !demo.opts->reset("demo.name")->shown()
			&& rv->tip() == "Reset to the default: 70", "options: a reset button on the rows off their default (a pinned one too)");
		if (vol && rv)
		{
			const ui::Rect& r = vol->screen();
			const float h = r.h, lw = h * 29 / 51, tw = h * 27 / 51, trackW = r.w - lw - h * 31 / 51 - tw;
			auto px = [&](double v) { return r.x + lw + trackW * (float)(v / 100) + tw / 2; };
			const float y = r.y + r.h / 2, u = ui::scale();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, px(40), y);
			send(SAPP_EVENTTYPE_MOUSE_DOWN, px(40), y);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, px(66), y);
			const int before = kVolume.get();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, px(73), y);
			const int onNotch = kVolume.get();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, px(73) + 10 * u, y);
			const int stillOn = kVolume.get();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, px(73) + 20 * u, y);
			const int pulled = kVolume.get();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, px(68), y);
			const int back = kVolume.get();
			send(SAPP_EVENTTYPE_MOUSE_UP, px(68), y);
			check(before == 66 && onNotch == 70 && stillOn == 70 && pulled > 73 && back == 70,
				("options: a drag across the default's notch holds there, pulled further it lets go, back across it holds again (" + std::to_string(before) + " " + std::to_string(onNotch)
					+ " " + std::to_string(stillOn) + " " + std::to_string(pulled) + " " + std::to_string(back) + ")").c_str());
			click(px(30), y);
			demo.opts->update();
			ui::frame(0);
			check(kVolume.get() == 30 && clickOn(rv) && kVolume.get() == 70 && (demo.opts->update(), ui::frame(0), !rv->shown()), "options: a click on reset sets the default, and the button goes");
			click(px(30), y);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, vol->screen().x + 20, vol->screen().y + 20);	// the help line
		}
	}
	auto type = [&](const char* s)
	{
		bool all = true;
		for (const char* c = s; *c; c++)
		{
			sapp_event e = {};
			e.type = SAPP_EVENTTYPE_CHAR;
			e.char_code = (unsigned char)*c;
			all = ui::event(e) && all;
		}
		return all;
	};
	if (demo.frame == 11)
	{
		const ui::Widget* name = demo.opts->control("demo.name");
		check(name && clickOn(name), "edit box: a click focuses it");
		if (name)
		{
			key(SAPP_KEYCODE_END);
			const bool typed = type("Xq");
			key(SAPP_KEYCODE_BACKSPACE);
			check(typed && key(SAPP_KEYCODE_W), "edit box: typing and world keys are consumed while focused");
			check(key(SAPP_KEYCODE_ENTER) && kName.get() == "WandererX", ("edit box: Enter commits (" + kName.get() + ")").c_str());
			clickOn(name);
			type("Y");
			check(key(SAPP_KEYCODE_ESCAPE) && kName.get() == "WandererX" && demo.opts->shown(), "edit box: Escape restores and keeps the window open");
			clickOn(name);
			type("Z");
			clickOn(demoTab());	// focus moves to the window: the edit commits
			check(kName.get() == "WandererXZ", ("edit box: losing focus commits (" + kName.get() + ")").c_str());
		}
		// Selection, the clipboard and the keypad (kenshi-k16.3.18), on a box of its own.
		ui::EditBox& eb = ui::root().add<ui::EditBox>("alpha beta");
		eb.rect = { 40, 1000, 400, 0 };
		ui::frame(0);
		std::string clip;
		ui::setClipboardFns([&] { return clip; }, [&](const std::string& t) { clip = t; });
		int entered = 0;
		eb.onEnter = [&] { entered++; };
		auto ks = [&](sapp_keycode k, uint32_t mods = 0)
		{
			sapp_event e = {};
			e.type = SAPP_EVENTTYPE_KEY_DOWN;
			e.key_code = k;
			e.modifiers = mods;
			return ui::event(e);
		};
		constexpr uint32_t kS = SAPP_MODIFIER_SHIFT, kC = SAPP_MODIFIER_CTRL;
		const ui::Rect er = eb.screen();
		click(er.x + er.w - 5, er.y + er.h / 2);
		check(ui::focused(&eb) && eb.caret() == 10 && eb.selected().empty(), "text box: a click past the end puts the caret there, nothing selected");
		ks(SAPP_KEYCODE_A, kC);
		check(eb.selected() == "alpha beta", "text box: Ctrl+A selects all");
		type("x");
		check(eb.text == "x" && eb.selected().empty(), ("text box: typing replaces the selection (" + eb.text + ")").c_str());
		type("yz abc");
		ks(SAPP_KEYCODE_LEFT, kS);
		ks(SAPP_KEYCODE_LEFT, kS);
		const std::string two = eb.selected();
		ks(SAPP_KEYCODE_BACKSPACE);
		check(two == "bc" && eb.text == "xyz a", ("text box: Shift+Left selects, Backspace deletes the selection (" + two + " / " + eb.text + ")").c_str());
		ks(SAPP_KEYCODE_HOME, kS);
		const std::string home = eb.selected();
		ks(SAPP_KEYCODE_LEFT);
		const size_t near = eb.caret();
		ks(SAPP_KEYCODE_END, kS);
		check(home == "xyz a" && near == 0 && eb.selected() == "xyz a", "text box: Shift+Home / End select to the ends, a plain arrow collapses to the near end");
		ks(SAPP_KEYCODE_RIGHT);
		ks(SAPP_KEYCODE_LEFT, kC);
		const size_t word = eb.caret();
		ks(SAPP_KEYCODE_RIGHT, kS);
		ks(SAPP_KEYCODE_DELETE);
		check(word == 4 && eb.text == "xyz ", ("text box: Ctrl+Left jumps a word, Delete deletes the selection (" + eb.text + ")").c_str());
		ks(SAPP_KEYCODE_A, kC);
		ks(SAPP_KEYCODE_C, kC);
		ks(SAPP_KEYCODE_END);
		ks(SAPP_KEYCODE_V, kC);
		check(clip == "xyz " && eb.text == "xyz xyz ", ("text box: Ctrl+C copies, Ctrl+V pastes at the caret (" + eb.text + ")").c_str());
		ks(SAPP_KEYCODE_HOME);
		ks(SAPP_KEYCODE_RIGHT, kS | kC);
		ks(SAPP_KEYCODE_X, kC);
		clip += "\nsecond line";
		ks(SAPP_KEYCODE_END);
		ks(SAPP_KEYCODE_V, kC);
		check(eb.text == "xyz xyz " && clip.rfind("xyz ", 0) == 0, ("text box: Ctrl+X cuts; a paste stops at a line break (" + eb.text + ")").c_str());
		send(SAPP_EVENTTYPE_MOUSE_MOVE, er.x + 4, er.y + er.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, er.x + 4, er.y + er.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, er.x + er.w - 5, er.y + er.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_UP, er.x + er.w - 5, er.y + er.h / 2);
		check(eb.selected() == "xyz xyz ", ("text box: a drag selects (" + eb.selected() + ")").c_str());
		ks(SAPP_KEYCODE_END);
		ks(SAPP_KEYCODE_KP_4);
		const size_t kpLeft = eb.caret();
		ks(SAPP_KEYCODE_KP_7);
		check(kpLeft == 7 && eb.caret() == 0 && eb.text == "xyz xyz ", "text box: the keypad with NumLock off navigates (KP_4 left, KP_7 Home), types nothing");
		ks(SAPP_KEYCODE_KP_4);
		type("4");
		ks(SAPP_KEYCODE_KP_DECIMAL);
		type(".");
		check(eb.text == "4.xyz xyz " && eb.caret() == 2, ("text box: NumLock on, the keypad's digit follows and is typed instead (" + eb.text + ")").c_str());
		ks(SAPP_KEYCODE_KP_DECIMAL);
		ui::frame(0.02);
		type("q");
		check(eb.text == "4.qyz xyz ", ("text box: a char in a later frame keeps the keypad's move (KP_DECIMAL deleted) (" + eb.text + ")").c_str());
		check(ks(SAPP_KEYCODE_KP_ENTER) && entered == 1 && !ui::focused(&eb), "text box: keypad Enter still submits like Enter");
		sappClipboard();
		ui::root().remove(&eb);
	}
	// Key bindings: the Keys tab, capture, conflicts, held keys, the HUD's shortcut buttons.
	auto keyEv = [&](sapp_event_type t, sapp_keycode k, uint32_t mods = 0)
	{
		sapp_event e = {};
		e.type = t;
		e.key_code = k;
		e.modifiers = mods;
		return ui::event(e);
	};
	auto keyEvent = [](sapp_keycode k, uint32_t mods)
	{
		sapp_event e = {};
		e.type = SAPP_EVENTTYPE_KEY_DOWN;
		e.key_code = k;
		e.modifiers = mods;
		return e;
	};
	auto rclickOn = [&](const ui::Widget* w)
	{
		const ui::Rect& r = w->screen();
		sapp_event e = {};
		e.mouse_x = r.x + r.w / 2;
		e.mouse_y = r.y + r.h / 2;
		e.mouse_button = SAPP_MOUSEBUTTON_RIGHT;
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		ui::event(e);
		e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
		const bool down = ui::event(e);
		e.type = SAPP_EVENTTYPE_MOUSE_UP;
		return ui::event(e) && down;
	};
	auto keyText = [](const char* action) { std::string s; for (const cfg::Info& i : cfg::list()) { if (i.key == std::string("key.") + action) { s = i.value; } } return s; };
	if (demo.frame == 12)
	{
		{
			ui::Options& o = *demo.opts;
			ui::CaptionWindow& ow = *o.window();
			const float u = ui::scale();
			bool inside = o.tab(0) != nullptr;
			for (size_t i = 0; o.tab(i); i++) { inside = inside && o.tab(i)->screen().x >= ow.screen().x && o.tab(i)->screen().right() <= ow.screen().right(); }
			check(ow.shown() && ow.resizable && inside, "options: a resizable window, every tab inside it (a side list)");
			for (size_t i = 0; o.tab(i); i++) { if (static_cast<const ui::Button*>(o.tab(i))->text == "Interface") { clickOn(o.tab(i)); } }
			ui::frame(0);
			const ui::Widget* sc = o.control("ui.scale");
			const ui::Widget* val = o.value("ui.scale");
			const ui::Widget* tick = o.control("ui.windows_movable");
			check(sc && val && tick && std::fabs(val->screen().right() - tick->screen().right()) < 2 && val->screen().x >= sc->screen().right(),
				"options: values and ticks share the right edge, the slider before them");
			if (!sc || !val || !tick) { return; }
			const float w0 = sc->screen().w;
			ow.sizeW = 100;
			ui::frame(0);
			check(std::fabs(sc->screen().w - (std::min(520 * u, w0 + 100 * u))) < 2 && std::fabs(val->screen().right() - tick->screen().right()) < 2,
				("options: wider, the slider takes the room (" + std::to_string(w0 / u) + " -> " + std::to_string(sc->screen().w / u) + ")").c_str());
			ow.sizeW = -2000;
			ui::frame(0);
			check(std::fabs(ow.screen().w - 700 * u) < 2 && sc->screen().w >= 120 * u - 1, "options: at its minimum width the slider keeps its least");
			ow.sizeW = 0;
			o.reveal("ui.progress_bars");	// the Interface tab is longer than the list since its groups (kenshi-k16.2.244.2)
			ui::frame(0);
			// The progress bars row (k16.3.15): a menu of the four values, the help line names them all, a pick sets the key.
			ui::Button* pb = static_cast<ui::Button*>(o.control("ui.progress_bars"));
			check(pb && pb->shown() && pb->text == "original", "options: Interface has the Progress bars row, at its default (original)");
			if (!pb) { return; }
			const ui::Rect& pr = pb->screen();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, pr.x + pr.w / 2, pr.y + pr.h / 2);
			o.update();
			std::string pbHelp;
			for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.progress_bars") { pbHelp = i.help; } }
			check(!pbHelp.empty() && o.helpText() == pbHelp, ("options: hovering the Progress bars row shows its help (" + o.helpText() + ")").c_str());
			clickOn(pb);
			ui::frame(0);
			const float rowH = ui::fonts::get("Kenshi_StandardFont_Medium")->lineHeight() + 6 * u;
			click(pr.x + 30, pr.bottom() + 10 * u + rowH * 2.5f);	// the third entry
			o.update();
			ui::frame(0);
			check(kProgressBars.get() == "squad" && pb->text == "squad", ("options: picking squad from the Progress bars menu sets it (" + kProgressBars.get() + ")").c_str());
			cfg::setUser("ui.progress_bars", "original");
			o.update();
			// window.remember (k16.2.109.2): a row in its section's tab, Interface (group Windows; kenshi-k16.2.244.2), no Window tab; the tick sets the key.
			{
				ui::Widget *homeTab = nullptr, *ifaceTab = nullptr;
				bool windowTab = false;
				for (size_t i = 0; o.tab(i); i++)
				{
					const std::string& t = static_cast<const ui::Button*>(o.tab(i))->text;
					if (t == "Interface") { homeTab = ifaceTab = o.tab(i); }
					if (t == "Window") { windowTab = true; }
				}
				std::string got = "no Interface tab";
				if (homeTab && ifaceTab)
				{
					clickOn(homeTab);
					o.update();
					o.reveal("window.remember");
					ui::frame(0);
					ui::Widget* wr = o.control("window.remember");
					got = wr && wr->shown() ? "row" : "no row";
					bool named = false;
					if (wr)
					{
						for (const ui::Widget* row = wr->parent(); row && !named; row = row->parent())
						{
							for (const auto& c : row->children())
							{
								const ui::Label* l = dynamic_cast<const ui::Label*>(c.get());
								if (l && l->text == "Remember") { named = true; }
							}
						}
					}
					got += named ? " named" : " unnamed";
					if (wr)
					{
						got += kWindowRemember.get() ? " on" : " off";
						clickOn(wr);
						o.update();
						got += kWindowRemember.get() ? " on" : " off";
						clickOn(wr);
						o.update();
						got += kWindowRemember.get() ? " on" : " off";
					}
					clickOn(ifaceTab);
					o.update();
					ui::frame(0);
				}
				check(!windowTab && got == "row named on off on", ("options: window.remember is an Interface row (no Window tab), its tick sets the key (" + got + ")").c_str());
			}
			// Groups (kenshi-k16.2.244.2): Graphics has a header per group, the stand-ins under Indoor lighting (experimental); the stand-in's needs
			// note (our own, "needs render.demo_shadow_toggle") shows under its row by the label, "needs Demo shadow toggle" (k16.2.244.7), dimmed while render.demo_shadow_toggle is off.
			{
				ui::Widget *gfx = nullptr, *iface = nullptr;
				for (size_t i = 0; o.tab(i); i++)
				{
					const std::string& t = static_cast<const ui::Button*>(o.tab(i))->text;
					if (t == "Graphics") { gfx = o.tab(i); }
					if (t == "Interface") { iface = o.tab(i); }
				}
				std::string got = "no Graphics tab";
				if (gfx && iface && clickOn(gfx))
				{
					o.update();
					o.reveal("render.shadow_soft");
					ui::frame(0);
					const std::vector<std::string> g = o.groups();
					const bool indoor = std::find(g.begin(), g.end(), "Indoor lighting (experimental)") != g.end();
					const ui::Label* note = o.needsLabel("render.shadow_soft");
					const ui::Widget* row = o.control("render.shadow_soft");
					got = std::string(indoor ? "group" : "no group") + (note && row && note->shown() && note->screen().y >= row->screen().bottom() - 1 ? " note" : " no note");
					if (note)
					{
						got += " " + note->text;
						const float on = note->colour.a;
						cfg::setUser("render.demo_shadow_toggle", "off");
						o.update();
						const float off = note->colour.a;
						cfg::setUser("render.demo_shadow_toggle", "on");
						o.update();
						got += off < on && note->colour.a == on ? " dims" : " no dim";
					}
					clickOn(iface);
					o.update();
					ui::frame(0);
				}
				check(got == "group note needs Demo shadow toggle dims", ("options: Graphics groups under headers, a needs note under its row that dims while unmet (" + got + ")").c_str());
			}
			// The Find bar (k16.2.227.1): Ctrl+F focuses the field; typing 'shadow' goes to the first match and shows 'n of m'; Enter steps through
			// every match across the tabs (the tab switched, the row in view and highlighted) and wraps; Shift+Enter and PREVIOUS go back; no tab
			// or row is ever hidden; a highlighted row's control works; Escape clears it, the next one closes the window.
			{
				const size_t before = o.currentTab();
				std::vector<size_t> rows;	// each tab's row count, before any find
				for (size_t i = 0; o.tab(i); i++)
				{
					clickOn(o.tab(i));
					o.update();
					rows.push_back(o.rowCount());
				}
				clickOn(o.tab(before));
				o.update();
				ui::frame(0);
				const auto has = [](std::string s, const std::string& q)
				{
					for (char& c : s) { c = (char)tolower((unsigned char)c); }
					return s.find(q) != std::string::npos;
				};
				const auto shiftKey = [](sapp_keycode k, uint32_t m)
				{
					sapp_event e = {};
					e.type = SAPP_EVENTTYPE_KEY_DOWN;
					e.key_code = k;
					e.modifiers = m;
					const bool took = ui::event(e);
					e.type = SAPP_EVENTTYPE_KEY_UP;
					e.modifiers = 0;
					ui::event(e);
					return took;
				};
				const auto settle = [&] { o.update(); ui::frame(0); ui::frame(0); };
				// A tab is whole: its row count as before, and every tab button still there.
				const auto whole = [&]
				{
					size_t tabs = 0;
					for (size_t i = 0; o.tab(i); i++) { tabs += o.tab(i)->shown() ? 1 : 0; }
					return tabs == rows.size() && o.currentTab() < rows.size() && o.rowCount() == rows[o.currentTab()];
				};
				ui::focus(o.window());
				const bool ctrlF = shiftKey(SAPP_KEYCODE_F, SAPP_MODIFIER_CTRL) && ui::focused(o.search());
				const bool typed = type("shadow");
				settle();
				size_t expected = 0;
				for (const cfg::Info& i : cfg::list()) { if (i.key.compare(0, 6, "debug.") && i.key.compare(0, 10, "key.debug.") && (has(i.key, "shadow") || has(i.help, "shadow"))) { expected++; } }
				const size_t n = o.matches();
				const std::string count = o.countText();
				check(ctrlF && typed && o.query() == "shadow" && n >= expected && n >= 3 && o.hit() >= 0 && count == std::to_string(o.hit() + 1) + " of " + std::to_string(n)
					&& !o.highlighted().empty() && o.rowInView(o.highlighted()) && whole(),
					("options find: Ctrl+F, then typing 'shadow' goes to a match and shows the count (" + count + ", " + o.highlighted() + ")").c_str());
				// Enter n times: each the next match, its tab shown, in view and highlighted, nothing hidden; back at the start after n.
				const int start = o.hit();
				std::string got, tabsSeen;
				bool stepped = true;
				for (size_t k = 0; k < n; k++)
				{
					const int was = o.hit();
					const bool took = key(SAPP_KEYCODE_ENTER);
					settle();
					const std::string h = o.highlighted();
					bool match = false;
					for (const cfg::Info& i : cfg::list()) { if (i.key == h) { match = true; } }
					const bool tabOn = static_cast<const ui::Button*>(o.tab(o.currentTab()))->checked;
					stepped = stepped && took && ui::focused(o.search()) && o.hit() == (was + 1) % (int)n && match && o.rowInView(h) && tabOn && whole()
						&& o.countText() == std::to_string(o.hit() + 1) + " of " + std::to_string(n);
					got += h + " ";
					const std::string t = std::to_string(o.currentTab()) + ",";
					if (tabsSeen.find("," + t) == std::string::npos) { tabsSeen += "," + t; }
				}
				const size_t tabCount = std::count(tabsSeen.begin(), tabsSeen.end(), ',') / 2;
				check(stepped && o.hit() == start && tabCount >= 2 && got.find("render.demo_shadow_toggle ") != std::string::npos,
					("options find: Enter steps through every match across " + std::to_string(tabCount) + " tabs, each shown, highlighted, nothing hidden, and wraps (" + got + ")").c_str());
				// Back: Shift+Enter and PREVIOUS, wrapping from the first to the last.
				for (size_t k = 0; k < n && o.hit() != 0; k++) { key(SAPP_KEYCODE_ENTER); }
				settle();
				const bool sback = shiftKey(SAPP_KEYCODE_ENTER, SAPP_MODIFIER_SHIFT);
				settle();
				const int afterShift = o.hit();
				const bool prev = clickOn(o.findButton(false));
				settle();
				check(sback && afterShift == (int)n - 1 && prev && o.hit() == (int)n - 2 && o.rowInView(o.highlighted()) && whole(),
					("options find: Shift+Enter goes back, wrapping to the last; PREVIOUS too (" + std::to_string(afterShift) + " " + std::to_string(o.hit()) + ")").c_str());
				const bool nextB = clickOn(o.findButton(true));
				settle();
				check(nextB && o.hit() == (int)n - 1, "options find: NEXT goes to the next match");
				// A highlighted row's control still works, and the find stays.
				for (size_t k = 0; k < n && o.highlighted() != "render.demo_shadow_toggle"; k++) { o.step(true); }
				settle();
				ui::Widget* cs = o.control("render.demo_shadow_toggle");
				const bool was = kDemoToggle.get();
				const int at = o.hit();
				const bool clicked = o.highlighted() == "render.demo_shadow_toggle" && cs && clickOn(cs);
				settle();
				check(clicked && kDemoToggle.get() != was && o.query() == "shadow" && o.hit() == at && o.highlighted() == "render.demo_shadow_toggle",
					"options find: the highlighted row's tick sets its option, the find stays on it");
				cfg::setUser("render.demo_shadow_toggle", was ? "on" : "off");
				// Ctrl+F (the tick has the keyboard's window) selects the field's text: a new word replaces it; no match reads '0 matches'.
				ui::focus(o.window());
				const bool ctrlF2 = shiftKey(SAPP_KEYCODE_F, SAPP_MODIFIER_CTRL);
				const bool selected = ui::focused(o.search()) && o.search()->selected() == "shadow";
				type("zzqx");
				settle();
				check(ctrlF2 && selected && o.query() == "zzqx" && o.matches() == 0 && o.hit() < 0 && o.countText() == "0 matches" && o.highlighted().empty() && whole(),
					("options find: Ctrl+F selects the field's text; no match reads '0 matches', nothing highlighted (" + o.countText() + ")").c_str());
				ui::focus(o.window());
				shiftKey(SAPP_KEYCODE_F, SAPP_MODIFIER_CTRL);
				type("gamma");
				settle();
				const ui::Widget* gamma = o.control("demo.gamma");
				check(o.highlighted() == "demo.gamma" && gamma && gamma->enabled, "options find: a pinned match is shown highlighted, enabled");
				// Ranked (k16.2.227.5): "sun" goes first to a Sun* setting; the colours whose help says "Sunken" (Inset) come after every option whose
				// key has "sun" (its label's words), and Next follows that order across tabs.
				ui::focus(o.window());
				shiftKey(SAPP_KEYCODE_F, SAPP_MODIFIER_CTRL);
				type("sun");
				settle();
				const auto helpOf = [](const std::string& k)
				{
					for (const cfg::Info& i : cfg::list()) { if (i.key == k) { return i.help; } }
					return std::string();
				};
				const size_t sn = o.matches();
				int lastLabel = -1, firstSunken = (int)sn;
				std::string order;
				for (size_t k = 0; k < sn; k++)
				{
					const std::string& mk = o.matchKey(k);
					order += mk + " ";
					if (has(mk, "sun")) { lastLabel = (int)k; }
					else if (has(helpOf(mk), "sunken") && firstSunken == (int)sn) { firstSunken = (int)k; }
				}
				check(sn >= 3 && o.hit() == 0 && o.countText() == "1 of " + std::to_string(sn) && has(o.highlighted(), "sun") && lastLabel >= 1
					&& firstSunken < (int)sn && lastLabel < firstSunken && o.rowInView(o.highlighted()) && whole(),
					("options find: 'sun' ranks the Sun* settings first, the 'Sunken' colours after them (" + order + ")").c_str());
				bool ranked = true;
				std::string rankTabs;
				for (size_t k = 1; k <= sn; k++)
				{
					key(SAPP_KEYCODE_ENTER);
					settle();
					ranked = ranked && o.hit() == (int)(k % sn) && o.highlighted() == o.matchKey(k % sn) && o.rowInView(o.highlighted())
						&& o.countText() == std::to_string(k % sn + 1) + " of " + std::to_string(sn);
					const std::string t = std::to_string(o.currentTab()) + ",";
					if (rankTabs.find("," + t) == std::string::npos) { rankTabs += "," + t; }
				}
				check(ranked && std::count(rankTabs.begin(), rankTabs.end(), ',') / 2 >= 2,
					"options find: Enter steps 'sun' in score order, across tabs, the count following");
				const size_t tabNow = o.currentTab();
				const bool esc = key(SAPP_KEYCODE_ESCAPE);
				settle();
				check(esc && o.query().empty() && o.search()->text.empty() && o.countText().empty() && o.highlighted().empty() && o.shown() && o.currentTab() == tabNow
					&& whole() && !ui::focused(o.search()), "options find: Escape clears the text and the highlight, the tab stays");
				ui::focus(o.search());
				check(key(SAPP_KEYCODE_ESCAPE) && !o.shown(), "options find: Escape in the empty field closes the window");
				o.show(true);
				clickOn(o.tab(before));
				o.update();
				ui::frame(0);
			}
			// UI sounds (k16.3.17): a test button and slider, the options' tabs, the stats window, through the recording sink.
			ui::Button& tb = ui::root().add<ui::Button>("SOUND TEST");
			tb.rect = { 20, 1000, 200, 40 };
			int clicks = 0;
			tb.onClicked = [&clicks] { clicks++; };
			tb.hoverSound = true;	// opt-in (kenshi-k16.3.20)
			ui::Slider& sl = ui::root().add<ui::Slider>(0, 100, 1);
			sl.rect = { 260, 1000, 300, 40 };
			ui::frame(0);
			const ui::Rect tr = tb.screen();
			demo.sounds.clear();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, tr.x + tr.w / 2, tr.y + tr.h / 2);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, tr.x + tr.w / 2 + 2, tr.y + tr.h / 2);
			check(ui::hovered(&tb) && demo.sounds == "Hover;", ("sounds: the pointer onto a button posts Hover once (" + demo.sounds + ")").c_str());
			demo.sounds.clear();
			clickOn(&tb);
			check(clicks == 1 && demo.sounds == "General_Click;", ("sounds: a click posts General_Click (" + demo.sounds + ")").c_str());
			tb.sound = "Building_Confirm";
			demo.sounds.clear();
			clickOn(&tb);
			check(demo.sounds == "Building_Confirm;", ("sounds: a button's own sound replaces General_Click (" + demo.sounds + ")").c_str());
			tb.sound = ui::kSilent;
			demo.sounds.clear();
			clickOn(&tb);
			check(clicks == 3 && demo.sounds.empty(), ("sounds: a silent button posts nothing (" + demo.sounds + ")").c_str());
			tb.sound.clear();
			cfg::setUser("ui.sounds", "false");
			demo.sounds.clear();
			clickOn(&tb);
			cfg::setUser("ui.sounds", "true");
			check(clicks == 4 && demo.sounds.empty(), ("sounds: ui.sounds off posts nothing (" + demo.sounds + ")").c_str());
			demo.sounds.clear();
			const ui::Rect sr = sl.screen();
			click(sr.x + sr.w / 2, sr.y + sr.h / 2);
			std::string slid = demo.sounds + "|";
			demo.sounds.clear();
			sl.setValue(10);
			slid += demo.sounds;
			check(slid == "Slider;|" && sl.value == 10, ("sounds: a click on a slider's track posts Slider, setValue from code nothing (" + slid + ")").c_str());
			// Hover once per entry, opt-in by hoverSound, ui.hover_sounds (kenshi-h2v.29.9.1, k16.3.20).
			ui::Widget& part = tb.add<ui::Widget>();
			part.rect = { 4, 4, 40, 32 };
			part.solid = true;
			ui::frame(0);
			const ui::Rect partR = part.screen();
			auto enter = [&](float x, float y)
			{
				send(SAPP_EVENTTYPE_MOUSE_MOVE, 2, 2);
				demo.sounds.clear();
				send(SAPP_EVENTTYPE_MOUSE_MOVE, x, y);
			};
			enter(tr.x + tr.w - 10, tr.y + tr.h / 2);
			std::string entry = demo.sounds + "|";
			demo.sounds.clear();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, partR.x + partR.w / 2, partR.y + partR.h / 2);
			const bool onPart = ui::hovered(&part);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, tr.x + tr.w - 12, tr.y + tr.h / 2);
			entry += demo.sounds + "|";
			enter(partR.x + partR.w / 2, partR.y + partR.h / 2);
			entry += demo.sounds;
			check(onPart && entry == "Hover;||Hover;", ("sounds: Hover once per entry, not again over the button's own child; leave and re-enter: once more (" + entry + ")").c_str());
			ui::Widget& loud = ui::root().add<ui::Widget>();
			loud.rect = { 600, 1000, 220, 40 };
			loud.hoverSound = true;
			ui::Button& lb = loud.add<ui::Button>("LOUD");
			lb.rect = { 0, 0, 200, 40 };
			lb.onClicked = [] {};
			ui::Button& plainB = ui::root().add<ui::Button>("PLAIN");
			plainB.rect = { 860, 1000, 200, 40 };
			plainB.onClicked = [] {};
			ui::frame(0);
			const ui::Rect lr = lb.screen(), plr = plainB.screen();
			std::string modes;
			for (const char* m : { "original", "all", "none" })
			{
				cfg::set(std::string("ui.hover_sounds=") + m);
				enter(lr.x + lr.w / 2, lr.y + lr.h / 2);
				modes += demo.sounds + "/";
				enter(plr.x + plr.w / 2, plr.y + plr.h / 2);
				modes += demo.sounds + "|";
			}
			const bool unknown = cfg::set("ui.hover_sounds=few");
			std::string kept;
			for (const cfg::Info& info : cfg::list()) { if (info.key == "ui.hover_sounds") { kept = info.value; } }
			check(!unknown && kept == "none", ("sounds: ui.hover_sounds refuses an unknown value (few) and keeps its own (" + kept + ")").c_str());
			cfg::set("ui.hover_sounds=original");
			check(modes == "Hover;/|Hover;/Hover;|/|", ("sounds: original: only under a hoverSound widget; all: every button; none: no button (" + modes + ")").c_str());
			auto flagged = [](const ui::Widget* w)
			{
				for (; w; w = w->parent()) { if (w->hoverSound) { return true; } }
				return false;
			};
			std::string who;
			for (int k = 0; k < 7; k++) { who += flagged(demo.mainMenu->button(k)) ? "T" : "-"; }
			who += flagged(demo.mainMenu->scenariosButton()) ? "T" : "-";
			who += demo.hud && flagged(demo.hud->portrait(2)) ? "T" : "-";
			check(who == "TTTTTTT--", ("sounds: the 7 title buttons hover-click, SCENARIOS and the HUD don't (" + who + ")").c_str());
			send(SAPP_EVENTTYPE_MOUSE_MOVE, 2, 2);
			ui::root().remove(&loud);
			ui::root().remove(&plainB);
			ui::root().remove(&tb);
			ui::root().remove(&sl);
			ui::frame(0);
			ui::Widget *colours = nullptr, *iface = nullptr;
			for (size_t i = 0; o.tab(i); i++)
			{
				const std::string& t = static_cast<const ui::Button*>(o.tab(i))->text;
				if (t == "Debug") { colours = o.tab(i); }	// another tab than Interface (the Colours tab before kenshi-k16.2.244.2)
				if (t == "Interface") { iface = o.tab(i); }
			}
			std::string tabs;
			if (colours && iface)
			{
				demo.sounds.clear();
				clickOn(colours);
				tabs = demo.sounds;
				demo.sounds.clear();
				clickOn(colours);
				tabs += "|" + demo.sounds;
				clickOn(iface);
				ui::frame(0);
			}
			check(tabs == "Tab;|", ("sounds: another tab posts Tab and no General_Click or Hover (side tabs are quiet), the shown one nothing (" + tabs + ")").c_str());
			ui::Stats& st = *demo.stats;
			const bool was = st.shown();
			st.show(false);
			demo.sounds.clear();
			st.show(true);
			st.show(true);
			const std::string open = demo.sounds;
			demo.sounds.clear();
			st.show(false);
			check(open == "Stats_Open;" && demo.sounds == "Stats_Close;", ("sounds: the stats window posts Stats_Open / Stats_Close on a change (" + open + demo.sounds + ")").c_str());
			st.show(was);
		}
		ui::Widget* tab = nullptr;
		for (int i = 0; i < 16 && demo.opts->tab(i); i++) { if (static_cast<const ui::Button*>(demo.opts->tab(i))->text == "Keys") { tab = demo.opts->tab(i); } }
		check(tab && clickOn(tab), "keys: the Options' Keys tab");
		demo.opts->reveal("key.camera_rotate_left");
		std::string list;
		for (const ui::keys::Conflict& c : ui::keys::conflicts()) { list += ui::keys::name(c.chord) + " " + c.a + "/" + c.b + " "; }
		check(list.empty(), ("keys: the defaults have no conflicts (build keys may share game ones) " + list).c_str());
		ui::keys::Chord c;
		check(ui::keys::parse("ctrl+shift+lbracket", c) && ui::keys::name(c) == "Ctrl+Shift+LBracket" && !ui::keys::parse("Bogus", c)
			&& ui::keys::parse("Num7", c) && ui::keys::name(c) == "Num7", "keys: names parse and print back");
		check(ui::keys::label("toggle_stats") == "C" && ui::keys::label("change_squad").empty(), "keys: the original's defaults (C stats, change_squad unbound)");
	}
	if (demo.frame == 13)
	{
		ui::Widget* f0 = demo.opts->keyField("camera_rotate_left", 0);
		ui::Widget* f1 = demo.opts->keyField("camera_rotate_left", 1);
		check(f0 && f1 && f0->shown(), "keys: two key boxes per action");
		if (f0 && f1)
		{
			const ui::KeyField* k0 = static_cast<const ui::KeyField*>(f0);
			check(clickOn(f0) && k0->capturing(), "keys: a click starts capturing");
			check(keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_LEFT_SHIFT, SAPP_MODIFIER_SHIFT) && keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_F, SAPP_MODIFIER_SHIFT)
				&& !k0->capturing() && keyText("camera_rotate_left") == "Shift+F", ("keys: a key with Shift binds it (" + keyText("camera_rotate_left") + ")").c_str());
			keyEv(SAPP_EVENTTYPE_KEY_UP, SAPP_KEYCODE_F, SAPP_MODIFIER_SHIFT);
			keyEv(SAPP_EVENTTYPE_KEY_UP, SAPP_KEYCODE_LEFT_SHIFT);
			check(ui::keys::pressed(keyEvent(SAPP_KEYCODE_F, SAPP_MODIFIER_SHIFT), "camera_rotate_left") && !ui::keys::pressed(keyEvent(SAPP_KEYCODE_F, 0), "camera_rotate_left")
				&& ui::keys::pressed(keyEvent(SAPP_KEYCODE_F, 0), "floor_down") && !ui::keys::pressed(keyEvent(SAPP_KEYCODE_Q, 0), "camera_rotate_left")
				&& ui::keys::pressed(keyEvent(SAPP_KEYCODE_F12, SAPP_MODIFIER_ALT), "screenshot_delayed") && !ui::keys::pressed(keyEvent(SAPP_KEYCODE_F12, SAPP_MODIFIER_ALT), "screenshot")
				&& ui::keys::pressed(keyEvent(SAPP_KEYCODE_F12, 0), "screenshot"),
				"keys: pressed() needs the exact modifiers");
			clickOn(f0);
			check(keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_ESCAPE) && !k0->capturing() && keyText("camera_rotate_left") == "Shift+F" && demo.opts->shown(),
				"keys: Escape cancels the capture, the window stays");
			clickOn(f1);
			keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_LEFT_CONTROL, SAPP_MODIFIER_CTRL);
			keyEv(SAPP_EVENTTYPE_KEY_UP, SAPP_KEYCODE_LEFT_CONTROL);
			check(keyText("camera_rotate_left") == "Shift+F, LCtrl", ("keys: a modifier alone binds on release (" + keyText("camera_rotate_left") + ")").c_str());
			check(rclickOn(f0) && keyText("camera_rotate_left") == "LCtrl", ("keys: a right click clears the slot, the next moves up (" + keyText("camera_rotate_left") + ")").c_str());
			clickOn(f0);
			keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_E);
			const std::vector<std::string> with = ui::keys::conflictsWith("camera_rotate_left", { SAPP_KEYCODE_E, 0 });
			check(with.size() == 1 && with[0] == "camera_rotate_right" && ui::keys::conflicts().size() == 1 && f0->tip() == "Also: Camera rotate right",
				("keys: a key two actions share is a conflict, kept and shown (" + f0->tip() + ")").c_str());
			cfg::setUser("key.debug.walk", "G");
			check(ui::keys::conflictsWith("debug.walk", { SAPP_KEYCODE_G, 0 }) == std::vector<std::string>{ "toggle_faction" }, "keys: a debug key on a game key is a conflict");
			check(ui::keys::conflictsWith("pause", { SAPP_KEYCODE_SPACE, 0 }).empty(), "keys: a build key may share a game key");
			keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_W);
			keyEv(SAPP_EVENTTYPE_KEY_DOWN, SAPP_KEYCODE_LEFT_SHIFT, SAPP_MODIFIER_SHIFT);
			const bool heldW = ui::keys::held("camera_forward") && ui::keys::held("debug.fly_fast");
			keyEv(SAPP_EVENTTYPE_KEY_UP, SAPP_KEYCODE_W, SAPP_MODIFIER_SHIFT);
			keyEv(SAPP_EVENTTYPE_KEY_UP, SAPP_KEYCODE_LEFT_SHIFT);
			check(heldW && !ui::keys::held("camera_forward"), "keys: held() follows key down / up, extra modifiers allowed");
			for (const char* k : { "camera_rotate_left", "debug.walk" }) { for (const cfg::Info& i : cfg::list()) { if (i.key == std::string("key.") + k) { cfg::setUser(i.key, i.def); } } }
			check(keyText("camera_rotate_left") == "Q" && ui::keys::conflicts().empty(), "keys: back to the defaults");
		}
		const ui::Widget* sta = demo.hud->shortcutButton(1);
		check(sta && clickOn(sta) && demo.shortcutLog == "toggle_stats;", ("hud: a shortcut button gives its action (" + demo.shortcutLog + ")").c_str());
	}
	if (demo.frame == 14)
	{
		{
			const ui::Widget* p = demo.hud->shortcutPanel();
			// hud.cpp anchors it bottom-right (halign/valign kEnd), rect.x units in from the window's right edge: the same rule at any aspect.
			const ui::Widget* b = demo.hud->shortcutButton(1);
			const float s = ui::scale();
			check(p && b && b->tip() == "Character stats (C)" && std::fabs(p->screen().right() - (sapp_widthf() - p->rect.x * s)) < 2
				&& std::fabs(p->screen().bottom() - sapp_heightf()) < 2, ("hud: the shortcut panel's place and key tooltips (" + (b ? b->tip() : "") + ")").c_str());
		}
		demo.opts->show(false);
		demo.ds.conversation = 1;
		talkNode(0);
	}
	if (demo.frame == 15)
	{
		check(demo.talk->shown() && key(SAPP_KEYCODE_2) && demo.node == 2, "dialogue: key 2 picks the second reply");
	}
	if (demo.frame == 16)
	{
		click(sapp_widthf() - 40, 200);	// a world click clears the focus; the dialogue takes the keyboard back next update
		const ui::Widget* l = demo.talk->choiceList();
		check(static_cast<const ui::Label*>(demo.talk->npcLabel())->text.rfind("Work?", 0) == 0, "dialogue: the NPC line follows the state");
		check(click(l->screen().x + 30, l->screen().y + 8) && !demo.ds.active, "dialogue: no replies shows (Leave), which ends it");
	}
	if (demo.frame == 17) { check(!demo.talk->shown(), "dialogue: hidden when inactive"); demo.ds.conversation = 2; talkNode(0); }
	if (demo.frame == 18) { check(key(SAPP_KEYCODE_ESCAPE) && !demo.ds.active, "dialogue: Escape leaves"); }
	if (demo.frame == 19)
	{
		demo.ds.conversation = 3;
		talkNode(0);
		click(sapp_widthf() - 40, 200);
	}
	if (demo.frame == 20)
	{
		const ui::Rect a = demo.bubbles->placed(1), b = demo.bubbles->placed(2);
		check(a.w > 0 && b.w > 0 && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom(), "bubbles: two speakers side by side overlap (no stacking, as in the original)");
		check(!click(a.x + a.w / 2, a.y + a.h / 2) && !ui::wantsMouse(), "bubbles: a click on a bubble reaches the world");
		const ui::Rect m = demo.bubbles->placed(3);
		const float u = ui::scale();
		check(demo.bubbles->lines(1) == 1 && demo.bubbles->lines(2) >= 2 && b.h > a.h && b.w <= std::round(450 * u) && a.w < b.w,
			"bubbles: a short line fits its width, a long one wraps (taller, at most 450 wide)");
		check(std::abs(m.bottom() - sapp_heightf() * 0.80f) <= 1 && std::abs(m.x + m.w / 2 - sapp_widthf() * 0.62f) <= 1, "bubbles: the anchor is the bubble's bottom centre");
		const ui::Rect big = demo.bubbles->placed(4), edge = demo.bubbles->placed(5);
		check(demo.bubbles->alpha(3) == 0.5f && demo.bubbles->alpha(1) == 1 && demo.bubbles->placed(6).w == 0, "bubbles: the host's distance alpha, none at 0");
		check(big.right() <= sapp_widthf() && edge.right() > sapp_widthf() && big.h > a.h, "bubbles: a large important line in the large font, kept on screen; an ordinary one may go off it");
		const ui::Rect p = demo.progress->placed(1), q = demo.progress->placed(2), f = demo.progress->filled(1), full = demo.progress->filled(2);
		check(std::abs(p.w - std::round(215 * ui::scale())) < 1 && std::abs(p.h - std::round(33 * ui::scale())) < 1 && std::abs(p.y - sapp_heightf() * 0.30f) < 1 && std::abs(p.x + p.w / 2 - sapp_widthf() * 0.62f) < 1,
			"progress: the panel 215 x 33 units, its top centre at the host's point");
		check(std::abs(q.y - sapp_heightf() * 0.31f) < 1, "progress: bars overlap as in the game, no stacking");
		const ui::Rect ft = demo.progress->floatPlaced(1);
		check(std::abs(ft.x - std::round(sapp_widthf() * 0.62f)) < 1 && std::abs(ft.y - std::round(sapp_heightf() * 0.26f)) < 1 && std::abs(demo.progress->floatAlpha(1) - 0.9f) < 0.01f
			&& std::abs(demo.progress->floatAlpha(2) - 0.7f * 0.5f) < 0.01f && demo.progress->floatPlaced(3).w == 0, "progress: floating texts top left at the point, faded from age 8, gone at 12");
		const ui::Font* small = ui::fonts::get("Kenshi_FloaterFont_Small");
		const ui::Font* large = ui::fonts::get("Kenshi_FloaterFont_Large");
		check(small && large && std::abs(ft.h - small->lineHeight()) < 1 && std::abs(demo.progress->floatPlaced(2).h - large->lineHeight()) < 1 && large->lineHeight() > small->lineHeight(),
			"progress: a floating text's font index picks Kenshi_FloaterFont_Small / Large");
		ui::NameLabels& nl = *demo.names;
		const ui::Rect n1 = nl.placed(1), n3 = nl.placed(3);
		check(std::abs(n1.h - std::round(32 * ui::scale())) < 1 && std::abs(n1.x + n1.w / 2 - sapp_widthf() * 0.56f) < 1 && std::abs(n1.y + n1.h / 2 - sapp_heightf() * 0.18f) < 1,
			"name labels: a 32-unit panel centred on the host's point");
		check(nl.text(1) == "Beep" && nl.colour(1) == ui::res::colour("NameTagUnselected") && nl.text(2) == "[Ruka]" && nl.colour(2) == ui::res::colour("NameTagSelected"),
			"name labels: unselected plain in grey, selected [Name] in white");
		check(n3.w > nl.textBox(3).w + (38 + 32) * ui::scale() && n1.w < nl.textBox(1).w + 38 * ui::scale(), "name labels: sneaking adds the stealth and light icons");
		demo.namesOn = false;
		check(f.w > 0 && full.w > 0 && std::abs(f.w - 0.3f * full.w) < 1.5f && std::abs((f.x - p.x) - (full.x - q.x)) < 1, "progress: the fill is the progress of the track");
		check(!click(p.x + p.w / 2, p.y + p.h / 2) && !ui::wantsMouse(), "progress: a click on a bar reaches the world");
		{
			const ui::Rect bar = ui::progressBarPlace(p);
			const float u = ui::scale();
			check(std::abs(f.x - bar.x - std::round(11 * u)) <= 1 && std::abs(f.y - bar.y - std::round(3 * u)) <= 1 && std::abs(f.h - (bar.h - 7 * u)) <= 1.5f
				&& std::abs(full.right() - (ui::progressBarPlace(q).right() - 10 * u)) <= 1.5f, "progress: the fill keeps the template's edge offsets (11 / 3 / 10 / 4 units), as MyGUI's Stretch");
		}
		const ui::Color none = { 0, 0, 0, 0 };
		check(ui::style().colour("DropLabelSteal", none).a > 0 && ui::style().colour("ItemStandard", none).a > 0 && ui::style().colour("ItemArtifact", none).a > 0, "tags: the stealing and item-grade colours are in the palette");
	}
	if (demo.frame == 21) { check(key(SAPP_KEYCODE_1) && demo.node == 1, "dialogue: keys work again after a world click"); talkNode(0); }
	if (demo.frame == 22)
	{
		check(demo.names->textBox(1).w == 0 && demo.names->placed(1).w == 0 && demo.names->placed(3).w > 0, "name labels: names off leaves only the sneaking one's icons");
		demo.namesOn = true;
	}
	if (demo.frame == 22)	// end on the HUD: Beep selected (stunned stomach), the tooltip of a bar
	{
		key(SAPP_KEYCODE_ESCAPE);
		for (ui::HudMember& m : demo.hs.squad) { m.selected = m.id == 100; }
	}
	if (demo.frame == 23)	// the style choice: a menu of the allowed values under the button
	{
		demo.opts->show(true);
		for (int i = 0; demo.opts->tab(i); i++) { if (static_cast<const ui::Button*>(demo.opts->tab(i))->text == "Interface") { clickOn(demo.opts->tab(i)); } }
		demo.opts->update();
		demo.opts->reveal("ui.style");	// the tab's rows overflow the list: the style row scrolled into view (k16.3.28; mid-tab since kenshi-k16.2.244.2)
	}
	// Run with --set ui.style=...: the choice is pinned; the button stays enabled, the menu part is skipped (picking would switch the run's style).
	bool stylePinned = false;
	for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.style") { stylePinned = i.pinned; } }
	// The HUD money box (the original's TimeMoneyPanel, top row): hidden below 0, "c.N" at the spot, updated in place; left on for the screenshot.
	// The stats table mockup (--show statsmock; user idea 2026-11-03): the status panel's stats as icons, one panel per weapon class, from the
	// rows the host already builds (the stats plus the extended rows). Not in the HUD.
	if (demo.showStatsMock && demo.frame == 64)
	{
		using ui::StatIcon;
		const ui::StatCell str{ StatIcon::kArm, "86", "", "", 0.0f, "Strength: 86 (XP 0%)", "Str" }, dex{ StatIcon::kBolt, "20", "", "", -1, "Dexterity: 20", "Dex" },
			tou{ StatIcon::kBricks, "52", "xp x3", "", -1, "Toughness: 52 (XP x3)", "Tgh" }, dodge{ StatIcon::kDodge, "59", "+24", "HudGood", -1, "Dodge: 59 (+24)", "Dodge" };
		const std::vector<ui::StatCell> second = { { StatIcon::kRun, "39", "kph", "", 1.0f, "Run speed: 39 kph (Athletics XP 150%)", "Run" },
			{ StatIcon::kKnockout, "-13", "KO", "", -1, "KO point: -13", "KO" }, { StatIcon::kHunger, "100%", "rate", "", -1, "Hunger rate: 100%", "Hunger" } };
		const struct { const char* title; std::vector<ui::StatCell> first; } kCases[] = {
			{ "Unarmed", { { StatIcon::kFist, "24", "+4", "HudGood", -1, "Martial Arts: 24 (+4)", "MA" }, str, dex, tou, dodge } },
			{ "Katana", { { StatIcon::kSword, "31", "+2", "HudGood", -1, "Attack: 31 (+2)", "Atk" }, { StatIcon::kShield, "48", "+20", "HudGood", -1, "Defense: 48 (+20, defensive stance)", "Def" },
				str, dex, tou, dodge } },
			{ "Crossbow", { { StatIcon::kCrossbow, "45", "", "", -1, "Crossbows: 45", "Xbow" }, { StatIcon::kCrosshair, "40", "", "", -1, "Precision: 40", "Prec" },
				{ StatIcon::kEye, "33", "-5", "HudBad", -1, "Perception: 33 (-5)", "Perc" }, str, dex, tou } } };
		ui::Panel& back = ui::root().add<ui::Panel>(ui::Look::kWindow, 0);
		back.halign = back.valign = ui::Widget::Align::kCenter;
		back.rect = { 0, -40, 3 * 243 + 4 * 16, 222 + 176 };
		for (int i = 0; i < 3; i++)
		{
			ui::Label& t = back.add<ui::Label>(kCases[i].title, "Kenshi_StandardFont_Medium");
			t.rect = { 16 + 259.0f * i + 8, 14, 0, 0 };
			for (int v = 0; v < 2; v++)
			{
				ui::Panel& p = back.add<ui::Panel>(ui::Look::kPanel, 0);
				p.rect = { 16 + 259.0f * i, 44 + 176.0f * v, 243, 160 };
				ui::StatGrid& g = p.add<ui::StatGrid>();
				g.rect = { 0, 8, 243, 144 };
				g.rows = { kCases[i].first, second };
				g.labels = v == 1;
				(v ? demo.statsText : demo.statsMock)[i] = &g;
			}
		}
	}
	if (demo.showStatsMock && demo.frame == 86) { demo.statsMock[0]->parent()->parent()->visible = false; }	// out of the later drags
	// Once the script is done, to look at in a window: the three classes again, and the unarmed one in the HUD's place, the status panel (its
	// expand button and named rows) hidden.
	if (demo.showStatsMock && demo.frame == 130)
	{
		demo.statsMock[0]->parent()->parent()->visible = true;
		demo.hs.info.valid = false;
		ui::Panel& p = ui::root().add<ui::Panel>(ui::Look::kPanel, 0);
		p.valign = ui::Widget::Align::kEnd;
		p.rect = { 16, 16, 243, 268 };
		ui::Label& name = p.add<ui::Label>("Beep", "Kenshi_StandardFont_Medium");
		name.rect = { 10, 10, 0, 0 };
		ui::Label& faction = p.add<ui::Label>("Nameless", "Kenshi_StandardFont_Medium");
		faction.halign = ui::Widget::Align::kEnd;
		faction.rect = { 10, 10, 0, 0 };
		const char* rows[][2] = { { "State:", "Defensive" }, { "Goal:", "Ditching resources" }, { "Encumbrance:", "0%" }, { "Athletics:", "88" } };
		for (int i = 0; i < 4; i++)
		{
			ui::Label& l = p.add<ui::Label>(rows[i][0], "Kenshi_StandardFont_MediumSmall");
			l.rect = { 10, 40 + 20.0f * i, 0, 0 };
			ui::Label& v = p.add<ui::Label>(rows[i][1], "Kenshi_StandardFont_MediumSmall");
			v.halign = ui::Widget::Align::kEnd;
			v.rect = { 10, 40 + 20.0f * i, 0, 0 };
		}
		ui::StatGrid& g = p.add<ui::StatGrid>();
		g.rect = { 0, 122, 243, 144 };
		g.rows = demo.statsMock[0]->rows;
	}
	if (demo.showStatsMock && demo.frame == 66)
	{
		const ui::StatGrid& g = *demo.statsMock[0];
		const ui::Rect r = g.screen();
		const float u = ui::scale();
		const ui::StatCell* first = g.cellAt(r.x + 5 * u, r.y + 5 * u);
		const ui::StatCell* last = g.cellAt(r.right() - 5 * u, r.bottom() - 5 * u);
		check(std::abs(r.h - 144 * u) <= 1 && first && first->tip == "Martial Arts: 24 (+4)" && last && last->icon == ui::StatIcon::kHunger
			&& demo.statsMock[1]->rows[0].size() == 6 && demo.statsMock[2]->rows[0][2].icon == ui::StatIcon::kEye,
			"stats mockup: two rows of icon cells per class (unarmed 5, melee 6, ranged 6), a tooltip per cell");
		check(demo.statsText[0]->labels && !demo.statsMock[0]->labels && demo.statsText[0]->rows[0][1].label == "Str" && demo.statsText[2]->rows[0][1].label == "Prec",
			"stats mockup: the text variant labels each cell (Str, Prec) in the icon's place");
		demo.hoverMe = demo.statsMock[0];
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 10, r.y + 36 * u);	// the screenshot's tooltip: Martial Arts
	}
	if (demo.frame == 30)
	{
		check(demo.hud->moneyBox()->visible && !demo.hud->moneyShown(), "hud money: the row hidden while the host has no money (-1), the box kept for the day");
		demo.hs.money = 227249;
	}
	if (demo.frame == 31)
	{
		const ui::Rect r = demo.hud->moneyBox()->screen();
		const float u = ui::scale();
		check(demo.hud->moneyShown() && demo.hud->moneyText() == "c.227,249" && std::fabs(r.right() - (sapp_widthf() - 84 * u)) < 2 && std::fabs(r.w - 191 * u) < 2,
			("hud money: \"c.227,249\" at the TimeMoneyPanel's spot (" + std::to_string(r.x / u) + ", " + std::to_string(r.y / u) + ")").c_str());
		demo.hs.money = 1500;
	}
	if (demo.frame == 32)
	{
		check(demo.hud->moneyText() == "c.1,500", "hud money: updates in place");
		demo.hs.money = -1;
	}
	if (demo.frame == 33)
	{
		check(!demo.hud->moneyShown(), "hud money: hidden again at -1");
		demo.hs.money = 227249;
	}
	if (demo.frame == 24 && stylePinned) { check(demo.opts->control("ui.style")->enabled, "options: a pinned style choice stays enabled (k16.2.303.1)"); demo.opts->show(false); }
	if (demo.frame == 24 && !stylePinned) { check(clickOn(demo.opts->control("ui.style")), "options: the style button opens its menu"); }
	if (demo.frame == 25 && !stylePinned)
	{
		const ui::Rect& r = demo.opts->control("ui.style")->screen();
		const ui::Font* f = ui::fonts::get("Kenshi_StandardFont_Medium");
		const float rowH = f->lineHeight() + 6 * ui::scale();
		click(r.x + 30, r.bottom() + 10 * ui::scale() + rowH * 1.5f);
		std::string v;
		for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.style") { v = i.value; } }
		check(v == "original", ("options: picking from the style menu sets it (" + v + ")").c_str());
		demo.opts->show(false);
	}
	if (demo.frame == 41)
	{
		demoCreator();
		demo.cr->show(true);
	}
	using C = ui::Creator;
	auto value = [](int i) { return static_cast<const ui::Label*>(demo.cr->pickerValue(i))->text; };
	if (demo.frame == 42)
	{
		const ui::Rect p = demo.cr->previewRect();
		check(p.w > 0 && p.h > 0 && p.x >= demo.cr->doneButton()->screen().right() && p.right() <= demo.cr->randomAllButton()->screen().x,
			"creator: the preview is the screen between the columns");
		check(value(C::kGroup) == "Human" && value(C::kRace) == "Greenlander" && value(C::kSex) == "MALE" && value(C::kCharacter) == "1/6"
			&& static_cast<const ui::EditBox*>(demo.cr->nameBox())->text == "Wanderer", "creator: the pickers and name show the state");
		check(static_cast<const ui::Label*>(demo.cr->statValue(1))->text == "+20%" && static_cast<const ui::Label*>(demo.cr->description())->text.rfind("Greenlanders", 0) == 0,
			"creator: the race description and stats");
		check(clickOn(demo.cr->pickerNext(C::kRace)) && demo.crLog == "race 1;", "creator: > picks the next subrace");
		check(clickOn(demo.cr->pickerPrev(C::kGroup)) && clickOn(demo.cr->pickerPrev(C::kGroup)) && demo.crLog == "race 1;group 3;group 2;", "creator: < wraps around the races");
		check(clickOn(demo.cr->pickerNext(C::kCharacter)) && demo.cs.character == 1, "creator: change character");
		check(clickOn(demo.cr->clothesButton()) && !demo.cs.clothes && clickOn(demo.cr->importButton()) && clickOn(demo.cr->exportButton()),
			"creator: clothes, import, export");
		check(demo.cr->slider("Skin Tone")->shown() && !demo.cr->pickerNext(C::kHead)->shown() && clickOn(demo.cr->tab(ui::kCreatorFace)) && demo.cr->page() == ui::kCreatorFace,
			"creator: BODY first; the FACE tab shows its page");
		demo.crLog.clear();
	}
	if (demo.frame == 43)
	{
		check(value(C::kGroup) == "Hive" && value(C::kRace) == "Prince" && !demo.cr->pickerNext(C::kSex)->enabled && value(C::kCharacter) == "2/6",
			"creator: the pickers follow the host (a one-sex race disables gender)");
		check(clickOn(demo.cr->pickerNext(C::kHead)) && demo.cs.head == 1 && demo.cr->slider("Jaw width")->shown(), "creator: FACE: head picker and face sliders");
		const ui::Rect r = demo.cr->slider("Jaw width")->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w * 0.5f, r.y + r.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, r.x + r.w * 0.5f, r.y + r.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.right() + 50, r.y + r.h / 2);
		const float during = demo.cs.sliders[2].value;
		send(SAPP_EVENTTYPE_MOUSE_UP, r.right() + 50, r.y + r.h / 2);
		check(during == 1 && static_cast<const ui::EditBox*>(demo.cr->sliderBox("Jaw width"))->text == "1.00",
			("creator: dragging a slider sets it live, with its value box (" + std::to_string(during) + ")").c_str());
		const ui::Rect p = demo.cr->previewRect();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, p.x + 100, p.y + p.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, p.x + 100, p.y + p.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, p.x + 160, p.y + p.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, p.x + 220, p.y + p.h / 2);
		const bool up = send(SAPP_EVENTTYPE_MOUSE_UP, p.x + 220, p.y + p.h / 2);
		check(up && demo.rotated == 120, ("creator: a drag in the preview rotates (" + std::to_string(demo.rotated) + ")").c_str());
		demo.crLog.clear();
		check(clickOn(demo.cr->randomButton()) && demo.crLog == "random 1;", "creator: RANDOMISE sends the shown tab");
		demo.cs.sliders.push_back({ "Face", "Ear size", 0, 1, 0.25f });
		check(clickOn(demo.cr->tab(ui::kCreatorHair)) && value(C::kHair) == "NONE", "creator: the HAIR tab");	// its page lays out next frame
	}
	if (demo.frame == 44)
	{
		check(clickOn(demo.cr->pickerNext(C::kHair)) && demo.cs.hair == 1, "creator: HAIR: the hair picker");
		const auto* jaw = static_cast<const ui::Slider*>(demo.cr->slider("Jaw width"));
		const auto* ear = static_cast<const ui::Slider*>(demo.cr->slider("Ear size"));
		check(jaw && jaw->value == 1 && ear && ear->value == 0.25, "creator: a new slider in the state gets a row, values kept");
		check(clickOn(demo.cr->tab(ui::kCreatorBody)) && demo.cr->slider("Muscle")->shown() && demo.cr->slider("Arm length")->shown(), "creator: BODY has other categories too");
	}
	if (demo.frame == 45)
	{
		check(value(C::kHair) == "Short", "creator: the picker follows the host's answer");
		ui::Widget* stance = demo.cr->sliderBox("Idle stance");
		check(clickOn(stance) && key(SAPP_KEYCODE_END) && key(SAPP_KEYCODE_BACKSPACE) && type("2.6") && key(SAPP_KEYCODE_ENTER) && demo.cs.sliders[0].value == 3,
			"creator: a typed value is stepped and sent");
		demo.crLog.clear();
		check(clickOn(demo.cr->randomAllButton()) && clickOn(demo.cr->resetButton()) && clickOn(demo.cr->doneButton()) && key(SAPP_KEYCODE_ESCAPE),
			"creator: the buttons and Escape are consumed");
		check(demo.crLog == "randomall;reset;done;cancel;", ("creator: randomise all, reset, confirm, cancel (" + demo.crLog + ")").c_str());
		demo.crLog.clear();
		check(clickOn(demo.cr->nameBox()) && key(SAPP_KEYCODE_BACKSPACE) && key(SAPP_KEYCODE_ENTER) && demo.crLog == "name Wandere;", "creator: the name commits on Enter");
		ui::CreatorState one = demo.cs;
		one.characters = 1;
		one.character = 0;
		demo.cr->update(one);
		check(!demo.cr->pickerNext(C::kCharacter)->shown() && demo.cr->pickerNext(C::kGroup)->shown(), "creator: one character, no CHANGE CHARACTER");
		ui::CreatorState animal = demo.cs;
		animal.editable = false;
		demo.cr->update(animal);
		check(!demo.cr->pickerNext(C::kGroup)->enabled && !demo.cr->pickerNext(C::kHair)->enabled && !demo.cr->randomButton()->enabled && !demo.cr->clothesButton()->enabled
			&& !demo.cr->nameBox()->enabled && !demo.cr->tab(ui::kCreatorFace)->enabled && !demo.cr->slider("Muscle")->enabled
			&& demo.cr->pickerNext(C::kCharacter)->enabled && demo.cr->doneButton()->enabled, "creator: an animal: only the stepper and CONFIRM");
		demo.crLog.clear();
		check(clickOn(demo.cr->pickerNext(C::kCharacter)) && demo.crLog == "character 2;", "creator: an animal: stepping on");
		demo.cr->update(demo.cs);
		check(demo.cr->pickerNext(C::kGroup)->enabled && demo.cr->slider("Muscle")->enabled, "creator: editable again");
		demo.cr->setCancel(nullptr);	// the new game's
		ui::focus(demo.cr->doneButton()->parent());
		check(!demo.cr->cancelButton()->visible && !key(SAPP_KEYCODE_ESCAPE), "creator: setCancel(null): no CANCEL, Escape goes on to the host");
		demo.crLog.clear();
		demo.cr->setCancel([] { demo.crLog += "cancel;"; });
		check(demo.cr->cancelButton()->visible && key(SAPP_KEYCODE_ESCAPE) && demo.crLog == "cancel;", "creator: setCancel back: CANCEL and Escape");
	}
	if (demo.frame == 46)
	{
		demo.cr->show(demo.showCreator);
		check(demo.showCreator || demo.cr->previewRect().w == 0, "creator: hidden, no preview rect");
		demoGear();
		demo.gear->show(true);
	}
	// The grid inventory: windows, drags between grids, refusals, the world drop, double / right click, ARRANGE.
	auto cellPt = [](const ui::ItemGrid* g, int cx, int cy)
	{
		const float c = g->cell * ui::scale();
		return Pt{ g->screen().x + (cx + 0.5f) * c, g->screen().y - std::round(g->scroll * ui::scale()) + (cy + 0.5f) * c };
	};
	auto drag = [&](Pt a, Pt b, uint32_t mods = 0)
	{
		send(SAPP_EVENTTYPE_MOUSE_MOVE, a.x, a.y, 0, mods);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, a.x, a.y, 0, mods);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, (a.x + b.x) / 2, (a.y + b.y) / 2, 0, mods);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, b.x, b.y, 0, mods);
		return send(SAPP_EVENTTYPE_MOUSE_UP, b.x, b.y, 0, mods);
	};
	auto rclick = [](Pt p, uint32_t mods = 0)
	{
		sapp_event e = {};
		e.modifiers = mods;
		e.mouse_x = p.x;
		e.mouse_y = p.y;
		e.mouse_button = SAPP_MOUSEBUTTON_RIGHT;
		e.type = SAPP_EVENTTYPE_MOUSE_MOVE;
		ui::event(e);
		e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
		ui::event(e);
		e.type = SAPP_EVENTTYPE_MOUSE_UP;
		return ui::event(e);
	};
	// The squads window and the HUD's squad tabs.
	ui::Squads& sw = *demo.squads;
	auto boxText = [](const ui::Widget* w) { return static_cast<const ui::EditBox*>(w)->text; };
	auto mid = [](const ui::Widget* w) { const ui::Rect& r = w->screen(); return Pt{ r.x + r.w / 2, r.y + r.h / 2 }; };
	auto stripEnd = [](const ui::Widget* w) { const ui::Rect& r = w->screen(); return Pt{ r.right() - 20, r.y + r.h / 2 }; };	// empty strip: the row
	auto labelText = [](const ui::Widget* w) { return static_cast<const ui::Label*>(w)->text; };
	auto tabText = [](int i) { const ui::Widget* t = demo.hud->squadTab(i); return t ? static_cast<const ui::Button*>(t)->text : std::string("(none)"); };
	auto told = [&](const std::string& title, const std::string& text)
	{
		return sw.prompt().shown() && static_cast<const ui::CaptionWindow*>(sw.prompt().window())->caption().text == title
			&& static_cast<const ui::RichText*>(sw.prompt().text())->text == text;
	};
	auto members = [](uint64_t squad)
	{
		std::string ids;
		for (const ui::SquadInfo& q : demo.sq.squads) { if (q.id == squad) { for (const ui::SquadMember& m : q.members) { ids += std::to_string(m.id) + " "; } } }
		return ids;
	};
	if (demo.frame == 30) { sw.show(true); }
	if (demo.frame == 31)
	{
		const ui::Color ink = ui::style().textColour(ui::Look::kInset, ui::State::kNormal, false);
		check(sw.shown() && boxText(sw.nameBox(1)) == "Expedition" && boxText(sw.nameBox(2)) == "Town" && sw.member(102) && labelText(sw.count(1)) == "2/20"
			&& labelText(sw.used()) == "4/30" && static_cast<const ui::Label*>(sw.count(1))->colour == ink, "squads: rows, names, members and counts from the state");
		check(sw.memberMark(102)->visible && static_cast<const ui::Image*>(sw.memberMark(103))->texels.x == ui::portraitMarkArt(ui::PortraitMark::kPrison).x
			&& !sw.memberMark(100)->visible, "squads: portrait marks over the face as on the HUD");
		check(sw.memberFace(100)->back->visible && sw.memberFace(101)->hurtAlpha() >= 0 && sw.memberFace(102)->face->tint.a == 0.25f && sw.memberFace(103)->face->tint.a == 1,
			"squads: the back overlay, Hurt background and dim as on the HUD");
		check(demo.hud->squadTab(1) && static_cast<const ui::Button*>(demo.hud->squadTab(0))->checked && tabText(0) == "EXPEDITION",
			"hud: squad tabs in capitals, the active one checked");
		check(clickOn(demo.hud->squadTab(1)) && demo.hs.activeSquad == 2 && demo.sqLog == "active 2;", "hud: a squad tab sets the active squad");
		check(clickOn(sw.member(100)) && clickOn(sw.dismissZone()) && demo.sqLog == "active 2;" && !sw.prompt().shown(), "squads: a click on a portrait or on DISMISS does nothing");
		check(clickOn(sw.deleteButton(1)) && told("", "The squad needs to be empty before it can be removed.") && key(SAPP_KEYCODE_ESCAPE) && !sw.prompt().shown()
			&& demo.sqLog == "active 2;", "squads: DELETE on a squad with members says so and changes nothing");
		check(clickOn(sw.addButton()) && demo.sqLog.ends_with("new;") && demo.sq.squads.back().name == "Nameless_3", "squads: ADD SQUAD (the host names it)");
		clickOn(sw.nameBox(1));
		key(SAPP_KEYCODE_END);
		type("X");
		check(demo.sqLog.ends_with("rename 1 ExpeditionX;"), "squads: a keystroke renames at once");
		check(drag(mid(sw.member(100)), stripEnd(sw.strip(2))) && demo.sqLog.ends_with("move 100 2 -1;"), "squads: a portrait dropped on another strip's empty space goes last");
	}
	if (demo.frame == 32)
	{
		auto inside = [](const ui::Widget* w, const ui::Widget* in) { while (w && w != in) { w = w->parent(); } return w != nullptr; };
		check(inside(sw.member(100), sw.block(2)) && labelText(sw.count(2)) == "3/20" && sw.block(3) && tabText(0) == "EXPEDITIONX" && tabText(1) == "TOWN"
			&& !demo.hud->squadTab(2), "squads: the next state rebuilds the rows; the HUD has the live name and no tab for the empty squad");
		check(drag(mid(sw.member(101)), mid(sw.member(102))) && demo.sqLog.ends_with("move 101 2 0;"), "squads: a portrait dropped on a portrait takes its place (0 = the leader)");
		check(clickOn(sw.deleteButton(3)) && demo.sqLog.ends_with("remove 3;"), "squads: DELETE an empty squad at once");
	}
	if (demo.frame == 33)
	{
		check(members(2) == "101 102 103 100 " && labelText(sw.count(1)) == "0/20" && sw.strip(1)->children().empty()
			&& tabText(0) == "TOWN" && !demo.hud->squadTab(1), "squads: 101 leads Town; Expedition is an empty strip, 0/20, and has no HUD tab (one tab stays)");
		check(drag(stripEnd(sw.strip(2)), stripEnd(sw.strip(1))) && demo.sqLog.ends_with("movesquad 2 0;"), "squads: a row dropped on another reorders the squads");
	}
	if (demo.frame == 34)
	{
		check(sw.block(2)->screen().y < sw.block(1)->screen().y && demo.sq.squads[0].id == 2, "squads: Town is now first");
		check(drag(mid(sw.member(103)), mid(sw.member(102)), SAPP_MODIFIER_SHIFT) && demo.sqLog.ends_with("swap 103 102;") && members(2) == "101 103 102 100 ",
			"squads: Shift + a drop on a portrait swaps the two");
		demo.sq.squads[1].max = 0;	// Expedition full (0/0), as the GlobalConstants could make it
		demo.sq.squads[0].max = 4;	// Town at its max
	}
	if (demo.frame == 35)
	{
		const ui::Color full = ui::res::colour("SquadFull");
		check(static_cast<const ui::Label*>(sw.count(2))->colour == full && labelText(sw.count(2)) == "4/4", "squads: a count at the max in SquadFull");
		const std::string before = demo.sqLog;
		check(drag(mid(sw.member(102)), stripEnd(sw.strip(1))) && demo.sqLog == before, "squads: a full squad refuses a drop silently");
		check(drag(mid(sw.member(102)), stripEnd(sw.strip(1)), SAPP_MODIFIER_SHIFT) && demo.sqLog == before, "squads: Shift on a full squad's empty space is refused too (the sim never passes the max)");
		demo.sq.maxSquads = 2;	// for ADD SQUAD next
	}
	if (demo.frame == 36)
	{
		check(drag(mid(sw.member(103)), mid(sw.dismissZone())) && told("DISMISS CHARACTER", "Dismiss character 'Burn'?") && key(SAPP_KEYCODE_ENTER)
			&& demo.sqLog.ends_with("dismiss 103;"), "squads: a portrait dropped on DISMISS asks, Yes dismisses");
		check(clickOn(sw.addButton()) && told("", "Maximum number of squads reached.") && !demo.sqLog.ends_with("new;") && key(SAPP_KEYCODE_ESCAPE),
			"squads: ADD SQUAD at max squads says so");
		clickOn(sw.nameBox(2));
		key(SAPP_KEYCODE_END);
		for (int i = 0; i < 4; i++) { key(SAPP_KEYCODE_BACKSPACE); }
		check(demo.sqLog.ends_with("rename 2 ;") && demo.sq.squads[0].name.empty(), "squads: an empty name is kept");
	}
	if (demo.frame == 37)
	{
		check(tabText(0).empty() && demo.hud->squadTab(0)->shown(), "hud: an empty squad name is a blank tab");
		demo.sq = { true, { { 9, "Solo", { { 104, "Izumi" } } } }, 1, 30, 10 };
	}
	if (demo.frame == 38)
	{
		check(drag(mid(sw.member(104)), mid(sw.dismissZone())) && told("", "Cannot dismiss the only remaining character in the faction.") && key(SAPP_KEYCODE_ESCAPE)
			&& !demo.sqLog.ends_with("dismiss 104;"), "squads: never the faction's last character");
		demo.sq.squads[0].members.clear();
	}
	if (demo.frame == 39)
	{
		check(clickOn(sw.deleteButton(9)) && told("", "You must have at least one squad.") && key(SAPP_KEYCODE_ESCAPE) && demo.sq.squads.size() == 1,
			"squads: never the only squad");
		demo.sq = { true, {}, 6, 30, 10 };
		for (uint64_t i = 1; i <= 6; i++) { demo.sq.squads.push_back({ i, "Squad number " + std::to_string(i), { { 200 + i, "M" } } }); }
	}
	if (demo.frame == 40)
	{
		const float w = demo.hud->squadTab(0)->preferred().w;
		const float header = std::min(762.0f, demo.hud->squadSpan()->screen().w / ui::scale());
		check(demo.hud->squadTab(5) && w < 200 && std::abs(w * 6 + 4 * 5 - header) < 6,
			("hud: six squad tabs shrink to share the header, 762 or the span if narrower (" + std::to_string(w) + ")").c_str());
		ui::focus(sw.window());
		check(key(SAPP_KEYCODE_ESCAPE) && !sw.shown(), "squads: Escape closes it");
		demoSquads();
		if (demo.showSquads) { demo.sq.squads.push_back({ 3, "Nameless_3", {} }); }	// for the screenshot: the capture's state, an empty squad
		sw.show(demo.showSquads);
	}
	// The overview window (after the gear frames, whose drag runs 57 to 63): pages by key action, tabs, the map's hook and buttons, Escape.
	ui::Overview& ov = *demo.overview;
	using OP = ui::OverviewPage;
	if (demo.frame == 69)
	{
		demo.overviewLog.clear();
		ov.toggle(OP::kMap);
		check(ov.shown(OP::kMap) && !sw.shown() && demo.overviewLog == "page 0;", ("overview: toggle opens it on its page (" + demo.overviewLog + ")").c_str());
		ov.toggle(OP::kSquads);
		check(ov.shown(OP::kSquads) && sw.shown() && demo.overviewLog == "page 0;page 4;", ("overview: another key goes to its page (" + demo.overviewLog + ")").c_str());
		ov.toggle(OP::kSquads);
		check(!ov.shown() && demo.overviewLog == "page 0;page 4;close;", "overview: the same key again closes it");
		ov.open(OP::kMap);
	}
	if (demo.frame == 70)	// Dark UI's map (the user's shot 20261105141740_1.jpg): the window's top, - + O bottom right, scroll bars, labels, marks
	{
		const float u = ui::scale();
		const ui::Rect w = ov.window()->screen(), m = ov.mapArea()->screen();
		const ui::Rect o = ov.mapButton(0)->screen(), minus = ov.mapButton(1)->screen(), plus = ov.mapButton(2)->screen();
		check(std::abs(w.y - 58 * u) <= 1 && std::abs(w.x + w.w / 2 - ui::root().screen().w / 2) <= 1, "map: the window centred across, its top 58 down");
		check(minus.right() < plus.x && plus.right() < o.x && std::abs(o.right() - m.right()) <= 1 && std::abs(o.w - 25 * u) <= 1 && std::abs(plus.x - minus.x - 41 * u) <= 1
			&& o.y > m.bottom(), "map: - + O at the bottom right, O under the map's right edge");
		check(!ov.mapBar(0)->visible && !ov.mapBar(1)->visible, "map: no scroll bars while the image fits");
		check(std::string(ui::Overview::mapLabelFont()) == "Kenshi_StandardFont_Medium" && ui::Overview::mapCameraUnits() == 20, "map: labels in the normal text size, a 20-unit camera arrow");
		for (int i = 0; i < 9; i++) { clickOn(ov.mapButton(2)); }
		ui::frame(0);
		const ui::ScrollBar* vb = ov.mapBar(0);
		const ui::ScrollBar* hb = ov.mapBar(1);
		const ui::Rect zm = ov.mapArea()->screen();
		check(vb->visible && hb->visible && std::abs(vb->screen().x - zm.right()) <= 1 && std::abs(hb->screen().y - zm.bottom()) <= 1 && zm.right() <= m.right(),
			"map: zoomed past the fit, the bars show along the right and the bottom, outside the view");
		const ui::Rect badge = ov.mapBadge(4), ruin = ov.mapMarker(4);
		check(ov.mapLevel() == 2 && badge.w > 0 && badge.x == ruin.x && badge.y == ruin.y && badge.w == ruin.w && badge.h == ruin.h && ov.mapBadge(0).w == 0
			&& ui::res::colour("MapNew", {}) == ui::Color{ 0, 1, 0, 1 }, "map: a new place's frame is green, over its icon");
		const double cz0 = ov.mapFrame().cz, cx0 = ov.mapFrame().cx;
		const ui::Rect th = vb->thumb();
		drag({ th.x + th.w / 2, th.y + th.h / 2 }, { th.x + th.w / 2, th.y + th.h / 2 + 60 * u });
		check(ov.mapFrame().cz > cz0, "map: dragging the vertical thumb down pans south");
		const ui::Rect right = hb->arrow(true);
		check(click(right.x + right.w / 2, right.y + right.h / 2) && ov.mapFrame().cx > cx0, "map: the horizontal bar's right arrow steps east");
		for (int i = 0; i < 12; i++) { clickOn(ov.mapButton(1)); }
		clickOn(ov.mapButton(0));
		ui::frame(0);
	}
	if (demo.frame == 70)
	{
		const float u = ui::scale();
		const ui::Rect w = ov.window()->screen(), m = ov.mapArea()->screen();
		check(std::abs(w.w - 866 * u) <= 2 && std::abs(w.h - 794 * u) <= 2 && m.w > 780 * u && m.h > 600 * u && m.y > w.y + 70 * u && m.bottom() < w.bottom(),
			("overview: 866 x 794, the map filling its page above the buttons (" + std::to_string(w.w / u) + " x " + std::to_string(w.h / u) + ")").c_str());
		check(demo.mapDraws > 0, "overview: the host's map hook draws during ui::draw");
		auto world = [&](float px, float py, double& x, double& z)
		{
			const ui::MapFrame f = ov.mapFrame();
			x = f.cx + (px - f.x - f.w / 2) / f.pxPerUnit;
			z = f.cz + (py - f.y - f.h / 2) / f.pxPerUnit;
		};
		const double fit = std::min(m.w, m.h) / 8192.0;
		std::string stepped0 = "false";
		for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.map_icons_stepped") { stepped0 = i.value; } }
		cfg::set("ui.map_icons_stepped=true");	// the level checks and the first sweep: the original's steps; the smooth sweep follows (k16.2.206.1)
		ui::MapFrame f = ov.mapFrame();
		check(std::abs(ov.mapZoom() - fit) < 1e-6 && f.cx == 0 && f.cz == 0 && ov.mapLevel() == 0, "map: opens fitted, the image centred (the original's ScrollView; no scrolling past the edges)");
		check(ov.mapMarker(0).w > 0 && ov.mapMarker(1).w > 0 && ov.mapMarker(2).w == 0 && ov.mapMarker(4).w == 0 && !ov.mapLabel(0),
			"map: level 0: Min places only, no labels");
		check(std::abs(ov.mapMarker(0).w - 105 * 0.5f * u) <= 1, ("map: level 0 icons at half their frame (" + std::to_string(ov.mapMarker(0).w / u) + ")").c_str());
		demo.overviewLog.clear();
		check(clickOn(ov.mapButton(2)) && std::abs(ov.mapZoom() - 2 * fit) < 1e-6 && clickOn(ov.mapButton(1)) && std::abs(ov.mapZoom() - fit) < 1e-6
			&& clickOn(ov.mapButton(1)) && std::abs(ov.mapZoom() - fit) < 1e-6, "map: + / - step a fit-zoom, never under the fit");
		const float px = m.x + m.w * 0.3f, py = m.y + m.h * 0.6f;
		double x0, z0, x1, z1;
		world(px, py, x0, z0);
		send(SAPP_EVENTTYPE_MOUSE_SCROLL, px, py, 1);
		world(px, py, x1, z1);
		check(std::abs(ov.mapZoom() - fit - 0.075) < 1e-6 && std::abs(x1 - x0) < 1 && std::abs(z1 - z0) < 1, "map: the wheel zooms 0.075 about the pointer");
		f = ov.mapFrame();
		drag({ m.x + m.w / 2, m.y + m.h / 2 }, { m.x + m.w / 2 - 40, m.y + m.h / 2 - 30 });
		const ui::MapFrame g = ov.mapFrame();
		check(std::abs((g.cx - f.cx) * f.pxPerUnit - 40) < 0.5 && std::abs((g.cz - f.cz) * f.pxPerUnit - 30) < 0.5 && demo.overviewLog.empty(), "map: a drag pans 1:1, no order");
		check(clickOn(ov.mapArea()) && demo.overviewLog.empty() && !ov.mapMoveShown(), "map: a left click does nothing");
		double mx, mz;
		world(m.x + 100, m.y + 120, mx, mz);
		rclick({ m.x + 100, m.y + 120 });
		char want[96];
		snprintf(want, sizeof want, "move %.0f %.0f 0;", mx, mz);
		check(demo.overviewLog == want && ov.mapMoveShown(), ("map: a right click orders a move there, the marker up (" + demo.overviewLog + ")").c_str());
		demo.overviewLog.clear();
		rclick({ m.x + 100, m.y + 120 }, SAPP_MODIFIER_SHIFT);
		check(demo.overviewLog.ends_with(" 1;"), ("map: Shift queues it (" + demo.overviewLog + ")").c_str());
		check(clickOn(ov.mapButton(0)) && ov.mapFrame().cx == 6000 && std::abs(ov.mapZoom() - fit - 0.075) < 1e-6, "map: O recentres on the camera, the zoom kept");
		for (int i = 0; i < 5; i++) { clickOn(ov.mapButton(2)); }
		check(ov.mapLevel() == 1 && ov.mapMarker(2).w > 0 && ov.mapMarker(4).w == 0 && ov.mapLabel(0) && ov.mapLabel(2) && !ov.mapLabel(3),
			("map: level 1: Mid places and labels, none on camps (" + std::to_string(ov.mapZoom() / u) + ")").c_str());
		for (int i = 0; i < 3; i++) { clickOn(ov.mapButton(2)); }
		check(ov.mapLevel() == 2 && ov.mapMarker(4).w > 0 && std::abs(ov.mapMarker(0).w - 105 * u) <= 1, "map: level 2: Max places, towns at their frame");
		for (int i = 0; i < 30; i++) { clickOn(ov.mapButton(2)); }
		check(std::abs(ov.mapZoom() - 1.5 * u) < 1e-6, "map: the zoom stops at 1.5 x the UI scale");
		for (int i = 0; i < 25; i++) { clickOn(ov.mapButton(1)); }
		for (int i = 0; i < 6; i++) { clickOn(ov.mapButton(2)); }
		const ui::Rect hub = ov.mapMarker(0);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, hub.x + hub.w / 2, hub.y + hub.h / 2);
		check(ov.mapArea()->tip() == demo.map.places[0].tip, "map: a visited town's tip under the pointer");
		const ui::Rect home = ov.mapMarker(1);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, home.x + home.w / 2, home.y + home.h / 2);
		check(ov.mapArea()->tip().empty(), "map: no tip without one");
		// The wheel's notch is 0.075 at any UI scale, the range the fit to 1.5 x the scale: 13 notches at 720 lines, 19 at 1080 (k16.3.36).
		const int notches = (int)std::ceil((1.5 * u - fit) / 0.075 - 1e-9);
		{
			// The wheel swept out to the fit and in to the max about the hub, a notch a frame (k16.2.201.1): the icon keeps one size per level
			// (map24_static §2: constant on screen, steps only at the levels), its centre on the hub's pixel within 1 px, the level never back.
			ui::Rect h = ov.mapMarker(0);
			for (int i = 0; i < 40; i++) { send(SAPP_EVENTTYPE_MOUSE_SCROLL, h.x + h.w / 2, h.y + h.h / 2, -1); }
			float sizes[3][2] = {};
			int level = 0, rows = 0, bad = 0, back = 0;
			double worst = 0;
			for (int i = 0; i < 40 && ov.mapZoom() < 1.5 * u - 1e-9; i++)
			{
				h = ov.mapMarker(0);
				send(SAPP_EVENTTYPE_MOUSE_SCROLL, h.x + h.w / 2 + 3, h.y + h.h / 2 - 2, 1);	// off the centre: the pointer's anchor moves the view
				ui::frame(0);
				const ui::MapFrame f = ov.mapFrame();
				h = ov.mapMarker(0);
				const double sx = f.x + f.w / 2 + (demo.map.places[0].x - f.cx) * f.pxPerUnit, sy = f.y + f.h / 2 + (demo.map.places[0].z - f.cz) * f.pxPerUnit;
				const double off = std::max(std::abs(h.x + h.w / 2 - sx), std::abs(h.y + h.h / 2 - sy));
				const int l = ov.mapLevel();
				back += l < level;
				level = l;
				if (sizes[l][0] == 0) { sizes[l][0] = h.w; sizes[l][1] = h.h; }
				bad += h.w != sizes[l][0] || h.h != sizes[l][1];
				worst = std::max(worst, off);
				rows++;
				if (demo.mapSweep) { printf("map sweep: zoom %.4f level %d hub icon %.0f,%.0f %.0fx%.0f pixel %.2f,%.2f off %.2f\n", ov.mapZoom(), l, h.x, h.y, h.w, h.h, sx, sy, off); }
			}
			check(rows >= notches && bad == 0 && back == 0 && worst <= 1 && sizes[0][0] < sizes[1][0] && sizes[1][0] < sizes[2][0],
				("map: a wheel sweep keeps the hub's icon one size a level, on its pixel (" + std::to_string(rows) + " notches, worst " + std::to_string(worst) + " px)").c_str());
			const double z0 = ov.mapZoom();
			send(SAPP_EVENTTYPE_MOUSE_SCROLL, h.x + h.w / 2, h.y + h.h / 2, -0.4f);
			check(std::abs(ov.mapZoom() - (z0 - 0.03)) < 1e-6, "map: a fraction of a notch zooms by its fraction (the original's delta x 0.000625)");
		}
		{
			// Smooth (ui.map_icons_stepped off, k16.2.206.1): the same sweep; the hub's icon never shrinks, grows at most 6% of its frame + 1 px
			// a notch (the steps jump 25%), from half its frame at the fit to its frame at the max, on its pixel within 1 px.
			cfg::set("ui.map_icons_stepped=false");
			ui::Rect h = ov.mapMarker(0);
			for (int i = 0; i < 40; i++) { send(SAPP_EVENTTYPE_MOUSE_SCROLL, h.x + h.w / 2, h.y + h.h / 2, -1); }
			ui::frame(0);
			h = ov.mapMarker(0);
			// The bound is not scaled: a notch is 0.075 / u of the scale-free zoom the size follows, so its growth in pixels is the same at
			// every UI scale (6 px at 720 lines failed 105 x 0.667 x 6% + 1, k16.3.36).
			const float first = h.w, bound = 105 * 0.06f + 1;
			float last = h.w, jump = 0;
			int rows = 0, shrink = 0;
			double worst = 0;
			for (int i = 0; i < 40 && ov.mapZoom() < 1.5 * u - 1e-9; i++)
			{
				h = ov.mapMarker(0);
				send(SAPP_EVENTTYPE_MOUSE_SCROLL, h.x + h.w / 2 + 3, h.y + h.h / 2 - 2, 1);
				ui::frame(0);
				const ui::MapFrame f = ov.mapFrame();
				h = ov.mapMarker(0);
				const double sx = f.x + f.w / 2 + (demo.map.places[0].x - f.cx) * f.pxPerUnit, sy = f.y + f.h / 2 + (demo.map.places[0].z - f.cz) * f.pxPerUnit;
				const double off = std::max(std::abs(h.x + h.w / 2 - sx), std::abs(h.y + h.h / 2 - sy));
				shrink += h.w < last;
				jump = std::max(jump, h.w - last);
				last = h.w;
				worst = std::max(worst, off);
				rows++;
				if (demo.mapSweep) { printf("map sweep smooth: zoom %.4f level %d hub icon %.0f,%.0f %.0fx%.0f pixel %.2f,%.2f off %.2f\n", ov.mapZoom(), ov.mapLevel(), h.x, h.y, h.w, h.h, sx, sy, off); }
			}
			check(rows >= notches && shrink == 0 && jump <= bound && worst <= 1 && std::abs(first - 105 * 0.5f * u) <= 1 && std::abs(last - 105 * u) <= 1,
				("map: smooth: a wheel sweep grows the hub's icon steadily, half its frame to all of it, on its pixel (" + std::to_string(rows) + " notches, biggest step "
					+ std::to_string(jump) + " px, worst " + std::to_string(worst) + " px)").c_str());
			cfg::set("ui.map_icons_stepped=" + stepped0);
		}
		demo.overviewLog.clear();
		check(clickOn(ov.tab(OP::kResearch)) && ov.page() == OP::kResearch && static_cast<const ui::CaptionWindow*>(ov.window())->caption().text == "RESEARCH",
			"overview: a tab click shows its page, the caption follows");
		demo.mapDraws = -1000;	// no map drawn while another page shows
		ui::focus(ov.window());
		demo.overviewLog.clear();
		check(key(SAPP_KEYCODE_ESCAPE) && !ov.shown() && demo.overviewLog == "close;", "overview: Escape closes it");
		ov.open(OP::kResearch);
	}
	if (demo.frame == 71)
	{
		check(demo.mapDraws == -1000, "overview: the map is not drawn on another page");
		ov.show(false);
		if (demo.showOverview) { ov.open(OP::kMap); }
		if (demo.showOverview && demo.mapNotches >= 0)
		{
			ui::frame(0);
			for (int i = 0; i < 40; i++) { clickOn(ov.mapButton(1)); }
			for (int i = 0; i < demo.mapNotches; i++)
			{
				const ui::Rect h = ov.mapMarker(0);
				send(SAPP_EVENTTYPE_MOUSE_SCROLL, h.x + h.w / 2, h.y + h.h / 2, 1);
				ui::frame(0);
			}
			printf("map shot: zoom %.4f level %d hub icon %.0fx%.0f\n", ov.mapZoom(), ov.mapLevel(), ov.mapMarker(0).w, ov.mapMarker(0).h);
		}
		if (demo.showSquads) { sw.show(true); }
	}
	// Movable windows: a test window dragged by its header (ui::frame(0) lays it out at once), then removed.
	if (demo.frame == 72)
	{
		const float u = ui::scale();
		std::string closed;
		ui::CaptionWindow& w = ui::root().add<ui::CaptionWindow>("MOVE ME", [&closed] { closed += "close;"; });
		w.halign = w.valign = ui::Widget::Align::kCenter;
		w.body().rect = { 0, 0, 300, 200 };
		ui::frame(0);
		auto bar = [&] { const ui::Rect& r = w.screen(); return Pt{ r.x + r.w / 2, r.y + 19 * u }; };
		auto near = [](float a, float b) { return std::fabs(a - b) <= 1.5f; };
		const ui::Rect start = w.screen();
		const ui::Rect screen = ui::root().screen();
		drag(bar(), { bar().x + 100 * u, bar().y + 50 * u });
		ui::frame(0);
		check(near(w.screen().x, start.x + 100 * u) && near(w.screen().y, start.y + 50 * u) && near(w.moveX, 100) && near(w.moveY, 50), "windows: a header drag moves the window by as much");
		const ui::Rect at = w.screen();
		const ui::Rect& x = w.closeButton().screen();
		drag({ x.x + x.w / 2, x.y + x.h / 2 }, { x.x - 200 * u, x.y + 100 * u });
		ui::frame(0);
		check(near(w.screen().x, at.x) && near(w.screen().y, at.y) && closed.empty(), "windows: a drag started on the close button neither moves nor closes");
		drag(bar(), { bar().x - 5000 * u, bar().y - 5000 * u });
		ui::frame(0);
		check(w.screen().x == screen.x && w.screen().y == screen.y, "windows: dragged past the top-left corner, it stops at the screen's edges");
		drag(bar(), { bar().x + 6 * u, bar().y + 9 * u });
		ui::frame(0);
		check(w.screen().x == screen.x && w.screen().y == screen.y, "windows: dropped within 10 units of an edge, it snaps to it");
		drag(bar(), { bar().x + 30 * u, bar().y + 40 * u });
		ui::frame(0);
		check(near(w.screen().x, screen.x + 30 * u) && near(w.screen().y, screen.y + 40 * u), "windows: past the snap distance it stays where dropped");
		const ui::Rect kept = w.screen();
		w.visible = false;
		ui::frame(0);
		w.visible = true;
		ui::frame(0);
		check(w.screen().x == kept.x && w.screen().y == kept.y, "windows: closed and reopened, it keeps its spot");
		drag(bar(), { bar().x + 5000 * u, bar().y + 5000 * u });
		ui::frame(0);
		check(w.screen().right() == screen.right() && w.screen().bottom() == screen.bottom(), "windows: dragged past the bottom-right corner, it stops at the screen's edges");
		ui::root().place({ 0, 0, std::round(screen.w * 0.6f), std::round(screen.h * 0.6f) }, u);	// a smaller screen
		check(w.screen().right() <= std::round(screen.w * 0.6f) && w.screen().bottom() <= std::round(screen.h * 0.6f) && w.screen().x >= 0 && w.screen().y >= 0,
			"windows: a moved window stays on a smaller screen");
		ui::frame(0);
		w.movable = false;	// laid out where its screen puts it
		ui::frame(0);
		const ui::Rect fixed = w.screen();
		drag(bar(), { bar().x - 300 * u, bar().y - 200 * u });
		ui::frame(0);
		w.movable = true;
		const ui::Widget* pw = demo.prompt->window();
		const ui::Widget* aw = demo.advanced->window();
		check(w.screen().x == fixed.x && w.screen().y == fixed.y && pw && aw && !static_cast<const ui::CaptionWindow*>(pw)->movable && !static_cast<const ui::CaptionWindow*>(aw)->movable,
			"windows: a fixed window doesn't move (the prompt and the advanced options are fixed)");
		check(clickOn(&w.closeButton()) && closed == "close;", "windows: the close button still closes");
		ui::root().remove(&w);
		ui::frame(0);

		// Raise on press (kenshi-k16.3.30.1): two overlapping windows; a left press anywhere on the lower one raises it and still reaches the
		// control under it (a button fires once, an edit box takes the keyboard, the scroll bar pages), its plain body too.
		{
			using A = ui::Widget::Align;
			ui::CaptionWindow& lo = ui::root().add<ui::CaptionWindow>("LOWER", [] {});
			lo.rect = { 100, 100, 428, ui::CaptionWindow::kHeaderH + 300 };	// the body stretches across: its rect.x / rect.w are margins
			lo.body().rect.h = 300;
			static int fired;	// static: the button outlives this block under --show raise
			fired = 0;
			ui::Button& lb = lo.body().add<ui::Button>("Press");
			lb.rect = { 10, 10, 120, 32 };
			lb.onClicked = [] { fired++; };
			ui::EditBox& le = lo.body().add<ui::EditBox>("text");
			le.rect = { 10, 60, 200, 30 };
			ui::ScrollView& ls = lo.body().add<ui::ScrollView>();
			ls.rect = { 10, 110, 200, 150 };
			for (int i = 0; i < 20; i++)
			{
				ui::Label& l = ls.body().add<ui::Label>("row " + std::to_string(i));
				l.fit = false;
				l.rect = { 0, 0, 150, 20 };
				l.halign = A::kStart;
			}
			ui::CaptionWindow& hi = ui::root().add<ui::CaptionWindow>("UPPER", [] {});
			hi.rect = { 360, 60, 428, ui::CaptionWindow::kHeaderH + 300 };
			hi.body().rect.h = 300;
			ui::frame(0);
			auto onTop = [](const ui::Widget& x) { return ui::root().children().back().get() == &x; };
			const ui::Rect lr = lo.screen(), hr = hi.screen();
			check(onTop(hi) && hr.x < lr.right() && hr.y < lr.bottom() && ui::root().hit(hr.x + 5, hr.y + 50 * u) != nullptr
				&& !onTop(lo), "raise on press: two overlapping windows, UPPER over LOWER");
			check(clickOn(&lb) && onTop(lo) && fired == 1, "raise on press: a click on the lower window's button raises it and fires the button once");
			hi.raiseGroup();
			ui::frame(0);
			check(clickOn(&le) && onTop(lo) && ui::focusedWidget() == &le, "raise on press: a click on its edit box raises it and the box takes the keyboard");
			hi.raiseGroup();
			ui::frame(0);
			const ui::Rect sr = ls.screen();
			const float off0 = ls.offset;
			check(ls.thumb().h > 0 && click(sr.right() - 3, sr.bottom() - 5) && onTop(lo) && ls.offset > off0,
				"raise on press: a press on its scroll bar raises it and pages the scroll area");
			hi.raiseGroup();
			ui::frame(0);
			check(click(lr.x + 300 * u, lr.bottom() - 10 * u) && onTop(lo) && fired == 1, "raise on press: a press on its plain body raises it");
			hi.raiseGroup();
			ui::frame(0);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, lr.x + 300 * u, lr.bottom() - 10 * u);
			{
				sapp_event e = {};
				e.type = SAPP_EVENTTYPE_MOUSE_DOWN;
				e.mouse_x = lr.x + 300 * u;
				e.mouse_y = lr.bottom() - 10 * u;
				e.mouse_button = SAPP_MOUSEBUTTON_RIGHT;
				ui::event(e);
				e.type = SAPP_EVENTTYPE_MOUSE_UP;
				ui::event(e);
			}
			check(onTop(hi), "raise on press: a right press does not raise (left only)");
			if (demo.showRaise) { clickOn(&lb); }	// left up for the capture: LOWER raised by a click on its button
			else
			{
				ui::root().remove(&hi);
				ui::root().remove(&lo);
			}
			ui::frame(0);
		}

		// Flex, laid out into a given rect (nothing else read).
		ui::Flex& f = ui::root().add<ui::Flex>(false, 10, 10);
		ui::Widget& fixed100 = f.add<ui::Widget>(ui::FlexItem{ 100 });
		ui::Widget& pref = f.add<ui::Widget>();
		pref.rect = { 0, 0, 50, 30 };
		ui::Widget& one = f.add<ui::Widget>(ui::FlexItem{ .fill = 1, .min = 40 });
		ui::Widget& two = f.add<ui::Widget>(ui::FlexItem{ .fill = 2 });
		two.valign = ui::Widget::Align::kStretch;
		pref.valign = ui::Widget::Align::kCenter;
		auto lay = [&](float wd) { f.place({ 0, 0, wd * u, 100 * u }, u); };
		auto wOf = [&](const ui::Widget& c) { return c.screen().w / u; };
		lay(600);
		check(near(wOf(fixed100), 100) && near(wOf(pref), 50) && near(wOf(one), 400.0f / 3) && near(wOf(two), 800.0f / 3) && near(one.screen().x / u, 180),
			"flex: fixed, preferred, then fill 1 : 2 of what is left, with the gap and padding");
		check(near(two.screen().h / u, 80) && near(pref.screen().h / u, 30) && near(pref.screen().y / u, 35), "flex: across, stretch fills inside the padding, center centres");
		f.set(two, { .fill = 2, .max = 200 });
		lay(600);
		check(near(wOf(two), 200) && near(wOf(one), 200), "flex: a fill child at its max gives the rest to the others");
		lay(200);
		check(near(wOf(one), 40) && near(wOf(two), 0) && near(f.preferred().w, 240), "flex: squeezed, a fill child keeps its min; preferred is the least it needs");
		f.set(one, { 60 });
		f.set(two, { 60 });
		f.justify = ui::Widget::Align::kEnd;
		lay(600);
		check(near(two.screen().right() / u, 590) && near(fixed100.screen().x / u, 600 - 10 - 60 - 60 - 50 - 100 - 30), "flex: no fill child, justify end packs them to the far side");
		f.remove(&pref);
		lay(600);
		check(near(wOf(one), 60) && near(wOf(fixed100), 100), "flex: a child removed, the others keep their items");
		ui::root().remove(&f);

		// Tabs, both sides, laid out into given rects.
		const char* const names[] = { "Audio", "Interface", "Colours", "Mods", "Keys", "Debug keys", "Camera", "Debug", "Markers", "Post", "Render", "Screenshot", "Session" };
		std::string picked;
		ui::Tabs& top = ui::root().add<ui::Tabs>(ui::Tabs::Side::kTop);
		for (const char* n : names) { top.add(n); }
		top.onSelect = [&picked](size_t i) { picked += std::to_string(i) + ";"; };
		auto layTop = [&](float wd) { top.place({ 100 * u, 100 * u, wd * u, 32 * u }, u); };
		layTop(3000);
		bool all = true;
		for (size_t i = 0; i < top.count(); i++) { all = all && top.tab(i)->visible; }
		check(all && !top.arrow(false)->visible && top.tab(0)->checked && near(top.tab(1)->screen().x / u, 100 + top.tab(0)->preferred().w + top.gap), "tabs: a top strip that fits shows every tab, no arrows");
		layTop(400);
		size_t last = 0;
		for (size_t i = 0; i < top.count(); i++) { if (top.tab(i)->visible) { last = i; } }
		check(top.arrow(true)->visible && top.tab(0)->visible && !top.tab(12)->visible && !top.arrow(false)->enabled && last > 0 && top.tab(last)->screen().right() <= top.arrow(false)->screen().x,
			"tabs: too wide, arrows at the end and the tabs that fit before them");
		check(clickOn(top.arrow(true)) && (layTop(400), top.first() == 1 && top.arrow(false)->enabled), "tabs: the next arrow steps the first tab shown");
		top.select(12);
		layTop(400);
		check(top.tab(12)->visible && top.tab(12)->checked && !top.tab(0)->checked && !top.arrow(true)->enabled && picked == "12;" && top.tab(12)->screen().right() <= top.arrow(false)->screen().x,
			"tabs: selecting a hidden tab brings it into view, clear of the arrows");
		send(SAPP_EVENTTYPE_MOUSE_MOVE, top.tab(12)->screen().x + 2, top.tab(12)->screen().y + 2);
		const size_t before = top.first();
		send(SAPP_EVENTTYPE_MOUSE_SCROLL, top.tab(12)->screen().x + 2, top.tab(12)->screen().y + 2, 1);
		layTop(400);
		check(top.first() + 1 == before, "tabs: the wheel steps the strip");
		picked.clear();
		check(clickOn(top.tab(top.first())) && picked == std::to_string(top.first()) + ";" && clickOn(top.tab(top.first())) && picked == std::to_string(top.first()) + ";",
			"tabs: a click selects; the selected tab again does nothing");
		ui::root().remove(&top);
		ui::Tabs& side = ui::root().add<ui::Tabs>(ui::Tabs::Side::kLeft);
		side.heading("GAME");
		for (int i = 0; i < 5; i++) { side.add(names[i]); }
		side.heading("CLIENT");
		for (int i = 5; i < 13; i++) { side.add(names[i]); }
		picked.clear();
		side.onSelect = [&picked](size_t i) { picked += std::to_string(i) + ";"; };
		side.place({ 100 * u, 100 * u, 200 * u, 300 * u }, u);
		const ui::Rect t0 = side.tab(0)->screen(), t5 = side.tab(5)->screen();
		check(near(t0.w / u, 200 - 14) && near(t0.h / u, side.tabH) && t0.y > side.screen().y && t5.y > side.tab(4)->screen().bottom() + (side.tabH - 2) * u,
			("tabs: a side list, rows the width less the scroll bar, under their headings (" + std::to_string(t0.w / u) + " x " + std::to_string(t0.h / u) + ")").c_str());
		check(clickOn(side.tab(2)) && picked == "2;" && side.tab(2)->checked && !side.tab(0)->checked, "tabs: a side row click selects it");
		auto* sv = dynamic_cast<ui::ScrollView*>(side.children().front().get());
		check(sv && sv->thumb().h > 0, "tabs: a side list longer than its rect scrolls");
		ui::root().remove(&side);

		// A resizable window: the grip by press / drag / release alone (as a ray pointer), the body's Flex following.
		ui::CaptionWindow& rw = ui::root().add<ui::CaptionWindow>("RESIZE ME", [] {});
		rw.rect = { 200, 150, 400, ui::CaptionWindow::kHeaderH + 300 };
		rw.body().rect.h = 300;
		rw.resizable = true;
		rw.minSize = { 250, 200 };
		ui::Flex& rf = rw.body().add<ui::Flex>(false, 8);
		rf.halign = rf.valign = ui::Widget::Align::kStretch;
		ui::Widget& rfill = rf.add<ui::Widget>(ui::FlexItem{ .fill = 1 });
		rfill.valign = ui::Widget::Align::kStretch;
		rf.add<ui::Widget>(ui::FlexItem{ 100 });
		ui::frame(0);
		auto gripAt = [&] { const ui::Rect& g = rw.grip().screen(); return Pt{ g.x + g.w / 2, g.y + g.h / 2 }; };
		const ui::Rect r0 = rw.screen();
		const float fill0 = rfill.screen().w;
		check(rw.grip().shown() && near(rw.grip().screen().right(), r0.right()) && near(rw.grip().screen().bottom(), r0.bottom()), "resize: a grip at the bottom-right corner");
		drag(gripAt(), { gripAt().x + 120 * u, gripAt().y + 60 * u });
		ui::frame(0);
		check(near(rw.screen().w, r0.w + 120 * u) && near(rw.screen().h, r0.h + 60 * u) && near(rw.screen().x, r0.x) && near(rw.screen().y, r0.y) && near(rfill.screen().w, fill0 + 120 * u)
			&& near(rw.body().screen().bottom(), rw.screen().bottom()), "resize: a grip drag grows it from the top-left corner; the body and its fill child follow");
		drag(gripAt(), { gripAt().x - 2000 * u, gripAt().y - 2000 * u });
		ui::frame(0);
		check(near(rw.screen().w, 250 * u) && near(rw.screen().h, 200 * u), "resize: it stops at its minimum size");
		drag(gripAt(), { gripAt().x + 5000 * u, gripAt().y + 5000 * u });
		ui::frame(0);
		check(near(rw.screen().right(), screen.right()) && near(rw.screen().bottom(), screen.bottom()), "resize: it stops at the UI's edges");
		drag(gripAt(), { gripAt().x - 300 * u, gripAt().y - 200 * u });
		drag(gripAt(), { gripAt().x + 294 * u, gripAt().y + 195 * u });
		ui::frame(0);
		check(near(rw.screen().right(), screen.right()) && near(rw.screen().bottom(), screen.bottom()), "resize: within 10 units of an edge, it snaps to it");
		drag(gripAt(), { gripAt().x - 300 * u, gripAt().y - 200 * u });
		ui::frame(0);
		const ui::Rect sized = rw.screen();
		rw.visible = false;
		ui::frame(0);
		rw.visible = true;
		ui::frame(0);
		check(near(rw.screen().w, sized.w) && near(rw.screen().h, sized.h), "resize: closed and reopened, it keeps its size");
		const ui::Rect bar0 = rw.screen();
		drag({ bar0.x + bar0.w / 2, bar0.y + 19 * u }, { bar0.x + bar0.w / 2 - 50 * u, bar0.y + 19 * u + 30 * u });
		ui::frame(0);
		check(near(rw.screen().x, bar0.x - 50 * u) && near(rw.screen().w, sized.w), "resize: a resized window moves by its header, its size kept");
		rw.resizable = false;
		ui::frame(0);
		check(!rw.grip().shown() && near(rw.screen().w, 400 * u), "resize: not resizable, no grip and its own size");
		ui::root().remove(&rw);
		ui::frame(0);

		// Layout files, from a scratch dir.
		check(demo.opts->layout().ok(), ("layout: options.layout loads (" + demo.opts->layout().error() + ")").c_str());
		namespace fs = std::filesystem;
		const fs::path dir = fs::temp_directory_path() / ("kui_layout_" + std::to_string(getpid()));
		fs::create_directories(dir / "ui");
		auto write = [&](const char* text) { std::ofstream(dir / "ui" / "t.layout") << text; };
		ui::setLayoutDir(dir.string());
		write("# a test\nrow gap=10 pad=10\n\tbutton \"OK\" id=ok size=100\n\tslot id=s size=fill\n\tlabel \"x\" id=lbl size=50 align=center colour=#ff0000\n");
		ui::Widget& host = ui::root().add<ui::Widget>();
		int built = 0;
		auto lf = std::make_unique<ui::Layout>(host, "t", [&built](ui::Layout&) { built++; });
		host.place({ 0, 0, 600 * u, 100 * u }, u);
		ui::Widget& mine = lf->at<ui::Widget>("s").add<ui::Widget>();
		ui::Button& ok = lf->at<ui::Button>("ok");
		check(lf->ok() && built == 1 && ok.text == "OK" && near(ok.screen().w / u, 100) && near(lf->at<ui::Widget>("s").screen().w / u, 600 - 20 - 20 - 100 - 50)
			&& near(lf->at<ui::Label>("lbl").colour.r, 1), ("layout: a file builds a row by its sizes, ids found (" + lf->error() + ")").c_str());
		write("row gap=10 pad=10\n\tbutton \"OK\" id=ok size=200\n\tslot id=s size=fill\n");
		lf->reload();
		host.place({ 0, 0, 600 * u, 100 * u }, u);
		check(lf->ok() && built == 2 && near(lf->at<ui::Button>("ok").screen().w / u, 200) && mine.parent() == &lf->at<ui::Widget>("s"), "layout: a reload rebuilds it; a slot's children move over");
		write("row gap=10\n\tbutton \"OK\" id=ok size=200\n\trwo id=s\n");
		lf->reload();
		check(!lf->ok() && lf->error().find("t.layout:3: unknown kind 'rwo'") != std::string::npos && built == 2 && host.children().size() == 2 && lf->find("ok") && mine.parent() == lf->find("s"),
			("layout: a bad reload says file:line, keeps the last good tree and shows the red box (" + lf->error() + ")").c_str());
		check(lf->at<ui::Tabs>("nope").count() == 0, "layout: an id the file lacks gives a spare, not a crash");
		lf.reset();
		write("row\n\tbutton id=ok size=wide\n");
		lf = std::make_unique<ui::Layout>(host, "t");
		check(!lf->ok() && lf->error().find(":2: size=wide") != std::string::npos && host.children().size() == 1, ("layout: a bad first file: only the red box (" + lf->error() + ")").c_str());
		lf.reset();
		lf = std::make_unique<ui::Layout>(host, "missing");
		check(!lf->ok() && lf->error().find("can't open") != std::string::npos, "layout: a missing file is a loud error");
		lf.reset();
		ui::root().remove(&host);
		fs::remove(dir / "ui" / "t.layout");
		fs::remove(dir / "ui");
		fs::remove(dir);
		ui::setLayoutDir(demo.layoutDir);
		ui::frame(0);
	}
	ui::Gear* gear = demo.gear.get();
	if (demo.frame == 47)
	{
		const ui::ItemGrid* main = gear->grid(ui::kGearMain);
		check(gear->shown() && main->shown() && std::fabs(main->screen().w - 8 * 25 * ui::scale()) < 2, "gear: the main inventory is 8 x 25 units wide");
		check(!gear->window(ui::kGearBag)->shown() && !gear->window(ui::kGearLegLeft)->shown(), "gear: backpack and robotics start closed");
		check(static_cast<const ui::Label*>(gear->infoValue(3))->text == "Slave Hunters", "gear: the stats panel lines");
		check(gear->grid(ui::kGearLegLeft)->mirror && !gear->grid(ui::kGearLegRight)->mirror, "gear: the left limbs draw mirrored");
		check(clickOn(gear->bagButton()) && gear->bagShown() && clickOn(gear->limbsButton()) && gear->limbsShown(), "gear: OPEN BAG and LIMBS open their windows");
		ui::GridItem bread;
		bread.w = 3;
		bread.h = 2;
		using F = ui::ItemGrid::Fit;
		check(main->fits(bread, 4, 0, nullptr) == F::kSwap && main->fits(bread, 6, 0, nullptr) == F::kNo && main->fits(bread, 5, 8, nullptr) == F::kFree,
			"gear: fits() onto one item / past the edge / free cells");
	}
	if (demo.frame == 48)	// the inventory's windows move as one (user 2026-11-05); restored at once for the checks below
	{
		const float u = ui::scale();
		auto* bag = static_cast<ui::CaptionWindow*>(gear->window(ui::kGearBag));
		auto* body = static_cast<ui::CaptionWindow*>(gear->window(ui::kGearMain));
		const ui::Rect b0 = bag->screen(), c0 = body->screen();
		const Pt grab = { b0.x + b0.w / 2, b0.y + 19 * u };
		drag(grab, { grab.x + 40 * u, grab.y + 30 * u });
		ui::frame(0);
		const ui::Rect b1 = bag->screen(), c1 = body->screen();
		check(b1.x - b0.x == c1.x - c0.x && b1.y - b0.y == c1.y - c0.y && std::fabs(b1.x - b0.x - 40 * u) <= 1.5f && std::fabs(b1.y - b0.y - 30 * u) <= 1.5f,
			"gear: a drag on the backpack's header moves the character window with it");
		const Pt moved = { b1.x + b1.w / 2, b1.y + 19 * u };
		drag(moved, { moved.x - 9000 * u, moved.y });
		ui::frame(0);
		check(bag->screen().x == 0 && body->screen().x - bag->screen().x == c0.x - b0.x, "gear: the group stops at the screen's edge as one box");
		body->moveX = body->moveY = 0;
		ui::frame(0);
		check(bag->screen().x == b0.x && body->screen().x == c0.x, "gear: back in place");
		// The stats rows' tips (kinspect info_panel/README.md: Disguise, Slave of, Looks like slave) show in the HUD's fixed box.
		auto hoverRow = [&](size_t i)
		{
			const ui::Rect& r = gear->infoValue(i)->parent()->screen();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + 4 * u, r.y + r.h / 2);
			demo.hud->update(demo.hs);
			return demo.hud->hintText();
		};
		check(hoverRow(2) == "Wearing nobody's colours." && demo.hud->hintBox()->visible, "gear: the Disguise row's tip shows in the HUD's fixed box");
		check(hoverRow(0).empty() && !demo.hud->hintBox()->visible, "gear: Total Weight has no tip");
		cfg::setUser("ui.hud_tips", "float");
		gear->update(demo.gs);
		check(hoverRow(3).empty() && gear->infoValue(3)->parent()->tip() == "Who holds your papers.", "gear: ui.hud_tips=float leaves the row's tip to float");
		cfg::setUser("ui.hud_tips", "box");
		ui::GearState free = demo.gs;
		for (size_t i : { 3u, 4u }) { free.info[i].value = "-"; free.info[i].dim = true; }
		gear->update(free);
		const ui::Color off = ui::style().textColour(ui::Look::kInset, ui::State::kDisabled, false);
		check(static_cast<const ui::Label*>(gear->infoValue(3))->colour == off && static_cast<const ui::Label*>(gear->infoValue(2))->colour != off, "gear: a dim row is greyed, the others not");
		gear->update(demo.gs);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 2, 2);
	}
	if (demo.frame == 48)
	{
		const ui::Rect c = gear->window(ui::kGearMain)->screen(), b = gear->window(ui::kGearBag)->screen(), l = gear->window(ui::kGearLegLeft)->screen();
		check(b.right() <= c.x && l.x >= c.right(), "gear: the backpack opens on the left, robotics on the right");
		demo.gearLog.clear();
		check(drag(cellPt(gear->grid(ui::kGearBag), 0, 0), cellPt(gear->grid(ui::kGearMain), 0, 8)) && demo.gearLog == "move 10 10>9 0,8;", "gear: drag from the backpack into the inventory");
		demo.gearLog.clear();
		drag(cellPt(gear->grid(ui::kGearArmour), 1, 1), cellPt(gear->grid(ui::kGearWeapon1), 1, 0));
		const std::string kind = ui::messageBox()->text();
		ui::message("");
		drag(cellPt(gear->grid(ui::kGearMain), 0, 0), cellPt(gear->grid(ui::kGearArmLeft), 0, 2));
		const std::string limb = ui::messageBox()->text();
		ui::message("");
		drag(cellPt(gear->grid(ui::kGearMain), 0, 0), cellPt(gear->grid(ui::kGearMain), 7, 0));
		const std::string edge = ui::messageBox()->text();
		check(demo.gearLog.empty(), ("gear: refused drops (wrong kind, blocked limb, past the edge) ask nothing (" + demo.gearLog + ")").c_str());
		check(kind == "Can't wear that item." && limb == "Can't wear that item." && edge == "No room for that item.",
			("gear: a refused drop says why in the message box (" + kind + " / " + limb + " / " + edge + ")").c_str());
		ui::message("");
		check(!ui::ItemGrid::dragged(), "gear: no drag left over");
	}
	if (demo.frame == 49)
	{
		const auto& items = demo.gs.grids[ui::kGearMain].items;
		check(!items.empty() && items.back().id == 10 && items.back().x == 0 && items.back().y == 8 && gear->grid(ui::kGearMain)->items.size() == 5, "gear: the next state shows the moved item");
		demo.gearLog.clear();
		drag(cellPt(gear->grid(ui::kGearMain), 2, 0), { ui::root().screen().w - 4, ui::root().screen().h * 0.4f });	// the world (no solid widget at the right edge)
		check(demo.gearLog == "drop 9 7;", ("gear: dragged over the world drops it (" + demo.gearLog + ")").c_str());
		demo.gearLog.clear();
		const Pt kit = cellPt(gear->grid(ui::kGearMain), 0, 0);
		check(rclick(kit) && demo.gearLog == "menu 9 6;", ("gear: right-click asks for the menu (" + demo.gearLog + ")").c_str());
		// Click-to-lift (the original's way). These clicks share a frame, so a right-click between steps keeps the UI from pairing two
		// left clicks into a double click.
		demo.gearLog.clear();
		click(kit.x, kit.y);
		check(ui::ItemGrid::dragged() && ui::ItemGrid::dragged()->id == 6 && demo.gearLog.empty() && gear->grid(ui::kGearMain)->selected == 0,
			"gear: a click selects and lifts the item");
		click(kit.x, kit.y);
		check(demo.gearLog == "use 9 6;" && !ui::ItemGrid::dragged(), ("gear: a second click on it within the double-click time uses it (" + demo.gearLog + ")").c_str());
		rclick(kit);
		demo.gearLog.clear();
		click(kit.x, kit.y);
		const ui::Rect bagR = gear->grid(ui::kGearBag)->screen();
		check(rclick({ bagR.x + 5, bagR.y + 5 }) && !ui::ItemGrid::dragged() && demo.gearLog.empty(), "gear: a right-click puts a lifted item back (nothing else)");
		rclick(kit);
		demo.gearLog.clear();
		click(kit.x, kit.y);
		check(key(SAPP_KEYCODE_ESCAPE) && !ui::ItemGrid::dragged() && demo.gearLog.empty() && gear->shown(), "gear: Escape puts a lifted item back (the window stays)");
		rclick(kit);
		demo.gearLog.clear();
		click(kit.x, kit.y);
		const Pt edge = cellPt(gear->grid(ui::kGearMain), 7, 0);
		ui::message("");
		check(click(edge.x, edge.y) && !ui::ItemGrid::dragged() && demo.gearLog.empty() && ui::messageBox()->text() == "No room for that item.",
			"gear: a lifted item clicked down where it does not fit is refused, with the message");
		ui::message("");
		rclick(kit);
		demo.gearLog.clear();
		click(kit.x, kit.y);
		const Pt bag0 = cellPt(gear->grid(ui::kGearBag), 0, 0);
		const ui::GearState kept = demo.gs;
		click(bag0.x, bag0.y);
		check(demo.gearLog == "move 6 9>10 0,0;" && !ui::ItemGrid::dragged(), ("gear: a lifted item is placed by the next click (" + demo.gearLog + ")").c_str());
		demo.gs = kept;	// the later checks want the kit where it was
		demo.gearLog.clear();
		demo.gearLog.clear();
		check(clickOn(gear->arrangeButton(ui::kGearBag)) && demo.gearLog == "arrange 10;", "gear: ARRANGE");
		// A pending item (kenshi-k16.20.14.1: its order is in flight): dimmed, no press, lift, menu, use or drag, nothing drops onto it; the tip stays.
		ui::ItemGrid* mg = gear->grid(ui::kGearMain);
		const int pk = mg->itemAt(kit.x, kit.y), other = mg->itemAt(cellPt(mg, 2, 0).x, cellPt(mg, 2, 0).y);
		ui::GridItem probe = pk >= 0 ? mg->items[pk] : ui::GridItem{};	// the kit's footprint and kind: a swap onto it, unless it is pending
		probe.id = 999;
		const bool swapOk = pk >= 0 && mg->fits(probe, probe.x, probe.y, nullptr) == ui::ItemGrid::Fit::kSwap;
		if (pk >= 0) { mg->items[pk].pending = true; }
		check(pk >= 0 && mg->itemAlpha(pk) == ui::ItemGrid::kPendingAlpha && (other < 0 || mg->itemAlpha(other) == 1), "gear: a pending item draws dimmed");
		rclick(kit);
		demo.gearLog.clear();
		click(kit.x, kit.y);
		check(!ui::ItemGrid::dragged() && demo.gearLog.empty() && !mg->tip().empty(), ("gear: a click on a pending item lifts nothing, its tip stays (" + demo.gearLog + ")").c_str());
		click(kit.x, kit.y);
		rclick(kit);
		check(demo.gearLog.empty(), ("gear: a pending item: no use on a double click, no menu on a right-click (" + demo.gearLog + ")").c_str());
		check(drag(kit, cellPt(gear->grid(ui::kGearBag), 0, 0)) && !ui::ItemGrid::dragged() && demo.gearLog.empty(), ("gear: a pending item does not drag (" + demo.gearLog + ")").c_str());
		check(swapOk && mg->fits(probe, probe.x, probe.y, nullptr) == ui::ItemGrid::Fit::kNo, "gear: nothing swaps or stacks onto a pending item");
		if (pk >= 0) { mg->items[pk].pending = false; }
		demo.gearLog.clear();
		// The carried count (kenshi-k16.20.12.1): a plain lift or drag takes one unit, Shift / Ctrl + left the stack; Ctrl + right is moveAll.
		const ui::GearState was = demo.gs;
		const Pt bread = cellPt(mg, 2, 0), free = cellPt(mg, 5, 0);
		const int bi = mg->itemAt(bread.x, bread.y);
		if (bi >= 0) { mg->items[bi].count = 5; }
		rclick(kit);
		demo.gearLog.clear();
		click(bread.x, bread.y);
		const ui::GridItem* held = ui::ItemGrid::dragged();
		check(bi >= 0 && held && held->count == 1 && mg->shownCount(bi) == 4 && mg->itemAlpha(bi) == 1 && mg->itemAt(bread.x, bread.y) == bi,
			"gear: a plain lift carries one of a 5-stack, the rest (4) stays drawn in its place");
		key(SAPP_KEYCODE_ESCAPE);
		rclick(kit);
		click(bread.x, bread.y, SAPP_MODIFIER_SHIFT);
		check(ui::ItemGrid::dragged() && ui::ItemGrid::dragged()->count == 5 && mg->itemAlpha(bi) < 1, "gear: Shift + left lifts the whole stack");
		key(SAPP_KEYCODE_ESCAPE);
		rclick(kit);
		click(bread.x, bread.y, SAPP_MODIFIER_CTRL);
		check(ui::ItemGrid::dragged() && ui::ItemGrid::dragged()->count == 5, "gear: Ctrl + left lifts the whole stack");
		key(SAPP_KEYCODE_ESCAPE);
		rclick(kit);
		demo.gearLog.clear();
		demo.gearCount = 0;
		drag(bread, free);
		check(demo.gearLog == "move 7 9>9 5,0;" && demo.gearCount == 1, ("gear: a plain drag of a stack moves one unit (" + demo.gearLog + " x" + std::to_string(demo.gearCount) + ")").c_str());
		demo.gearLog.clear();
		demo.gearCount = 0;
		drag(bread, free, SAPP_MODIFIER_SHIFT);
		check(demo.gearLog == "move 7 9>9 5,0;" && demo.gearCount == 5, ("gear: a Shift drag moves the stack (" + demo.gearLog + " x" + std::to_string(demo.gearCount) + ")").c_str());
		demo.gearLog.clear();
		rclick(bread, SAPP_MODIFIER_CTRL);
		rclick(bread, SAPP_MODIFIER_CTRL | SAPP_MODIFIER_SHIFT);
		check(demo.gearLog == "all 9 7;all 9 7;", ("gear: Ctrl + right-click (with Shift too) is moveAll, no menu (" + demo.gearLog + ")").c_str());
		demo.gearLog.clear();
		check(rclick(bread) && demo.gearLog == "menu 9 7;", ("gear: a plain right-click still asks for the menu (" + demo.gearLog + ")").c_str());
		demo.gearLog.clear();
		if (bi >= 0) { mg->items[bi].count = 4; }
		demo.gs = was;
		// Ids renumbered under a held press (kenshi-k16.20.14.3): the host's ids are list indexes + 1, so when an earlier entry leaves between
		// press and release the old index (and the old id) name the next item. The grid finds the pressed item again by its place.
		const std::vector<ui::GridItem> orig = mg->items;
		auto numbered = [&](bool dropKit, bool dropBread)
		{
			std::vector<ui::GridItem> v;
			for (const ui::GridItem& it : orig)
			{
				if ((dropKit && it.id == 6) || (dropBread && it.id == 7)) { continue; }
				v.push_back(it);
				v.back().id = (uint32_t)v.size();
			}
			return v;
		};
		auto rbtn = [](sapp_event_type t, Pt p)
		{
			sapp_event e = {};
			e.type = t;
			e.mouse_x = p.x;
			e.mouse_y = p.y;
			e.mouse_button = SAPP_MOUSEBUTTON_RIGHT;
			return ui::event(e);
		};
		mg->items = numbered(false, false);	// kit 1, bread 2, steel 3, hemp 4
		demo.gearLog.clear();
		rbtn(SAPP_EVENTTYPE_MOUSE_MOVE, bread);
		rbtn(SAPP_EVENTTYPE_MOUSE_DOWN, bread);
		mg->items = numbered(true, false);	// the kit sold: bread 1, steel 2
		rbtn(SAPP_EVENTTYPE_MOUSE_UP, bread);
		check(demo.gearLog == "menu 9 1;", ("gear: a right-click whose list lost an earlier entry mid-press names the item pressed (" + demo.gearLog + ")").c_str());
		mg->items = numbered(false, false);
		demo.gearLog.clear();
		rbtn(SAPP_EVENTTYPE_MOUSE_DOWN, bread);
		mg->items = numbered(false, true);	// the bread itself gone: steel takes its index
		rbtn(SAPP_EVENTTYPE_MOUSE_UP, bread);
		check(demo.gearLog.empty(), ("gear: a click on an item that left mid-press is eaten (" + demo.gearLog + ")").c_str());
		mg->items = numbered(false, false);
		rclick(kit);
		demo.gearLog.clear();
		click(bread.x, bread.y);
		mg->items = numbered(true, false);
		click(free.x, free.y);
		check(demo.gearLog == "move 1 9>9 5,0;" && !ui::ItemGrid::dragged(), ("gear: a lifted item placed after its list renumbered carries its new id (" + demo.gearLog + ")").c_str());
		mg->items = numbered(false, false);
		rclick(kit);
		demo.gearLog.clear();
		click(bread.x, bread.y);
		mg->items = numbered(false, true);
		click(free.x, free.y);
		check(demo.gearLog.empty() && !ui::ItemGrid::dragged(), ("gear: a lifted item that left meanwhile places nothing (" + demo.gearLog + ")").c_str());
		mg->items = numbered(false, false);
		rclick(kit);
		demo.gearLog.clear();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, bread.x, bread.y);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, bread.x, bread.y);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, (bread.x + free.x) / 2, bread.y);
		mg->items = numbered(true, false);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, free.x, free.y);
		send(SAPP_EVENTTYPE_MOUSE_UP, free.x, free.y);
		check(demo.gearLog == "move 1 9>9 5,0;", ("gear: a held drag whose list renumbered drops with the item's new id (" + demo.gearLog + ")").c_str());
		mg->items = orig;
		demo.gearLog.clear();
	}
	if (demo.frame == 50)
	{
		std::vector<ui::GridItem> pack = demo.gs.grids[ui::kGearMain].items;
		const bool all = ui::arrange(8, 10, pack);
		bool apart = true;
		for (size_t i = 0; i < pack.size(); i++)
		{
			for (size_t j = i + 1; j < pack.size(); j++)
			{
				const ui::GridItem &a = pack[i], &b = pack[j];
				apart &= !(a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h);
			}
		}
		check(all && apart && pack[3].x == 0 && pack[3].y == 0, "gear: arrange() packs biggest first without overlaps");
		check(clickOn(gear->limbsButton()) && !gear->limbsShown(), "gear: LIMBS closes robotics again");
		demo.gsKept = demo.gs;	// the trade checks' container comes back at 52
		demo.ccsKept = demo.ccs;
		demo.ccs.name = "Research Bench";
		demo.ccs.kind = ui::ContainerKind::kResearch;
		demo.gs.grids[ui::kGearContainer] = { 6, 4, false, false, {}, {} };
	}
	if (demo.frame == 51)
	{
		const ui::CaptionWindow* cw = static_cast<const ui::CaptionWindow*>(gear->window(ui::kGearContainer));
		check(cw->visible && demo.cont->researchButton()->visible && cw->closeButton().visible && !demo.cont->traderMoney()->parent()->visible, "gear: a research container: SHOW RESEARCH, closable");
		const ui::Rect rb = demo.cont->researchButton()->screen(), gr = demo.cont->grid().screen();
		check(rb.bottom() <= gr.y && rb.w >= gr.w, "gear: SHOW RESEARCH over the grid, the window's width");
		// ui.gear_top: the open windows share a top at 54 units and end above the HUD money box (748), whatever their height
		const float u = ui::scale();
		bool tops = true;
		for (int g : { ui::kGearMain, ui::kGearBag, ui::kGearLegLeft, ui::kGearContainer })
		{
			const ui::Widget* w = gear->window(g);
			if (w->shown()) { tops = tops && std::fabs(w->screen().y - 54 * u) < 2 && w->screen().bottom() < 748 * u; }
		}
		check(tops, "gear: the open windows share a top at 54 units, above the HUD money box");
		demo.gearLog.clear();
		gear->window(ui::kGearContainer)->parent()->raise();	// over the stats window the stats checks opened
		clickOn(demo.cont->researchButton());
		clickOn(gear->arrangeButton(ui::kGearContainer));
		demo.stats->window()->raise();	// back under it for the stats checks below
		check(demo.gearLog == "research;arrange 15;", ("gear: SHOW RESEARCH and the container's ARRANGE are the host's (" + demo.gearLog + ")").c_str());
		check(!demo.cont->typeText()->parent()->visible && !demo.cont->capacityText()->visible, "gear: no type or capacity text: both lines hidden");
		demo.ccs.kind = ui::ContainerKind::kFixed;
		demo.ccs.type = "Ore storage";
		demo.ccs.capacity = "Holds 40 (12 free)";
	}
	if (demo.frame == 52)
	{
		const ui::CaptionWindow* cw = static_cast<const ui::CaptionWindow*>(gear->window(ui::kGearContainer));
		check(cw->visible && !demo.cont->researchButton()->visible && !cw->closeButton().visible && gear->arrangeButton(ui::kGearContainer)->visible, "gear: a fixed container: no close button, ARRANGE stays");
		const ui::Rect ty = demo.cont->typeText()->parent()->screen(), cap = demo.cont->capacityText()->screen(), gr = demo.cont->grid().screen(), ar = gear->arrangeButton(ui::kGearContainer)->screen();
		check(demo.cont->typeText()->parent()->visible && ty.bottom() <= gr.y, "gear: the type text over the grid");
		check(demo.cont->capacityText()->visible && cap.y >= gr.bottom() && cap.bottom() <= ar.y, "gear: the capacity text between the grid and ARRANGE");
		demo.gs = demo.gsKept;
		demo.ccs = demo.ccsKept;
		if (demo.showGear) { gear->showLimbs(true); }
		else { gear->show(false); }
	}
	if (demo.frame == 52 && demo.showDialogue)
	{
		ui::DialogueState& d = demo.ds;
		d = {};
		d.active = true;
		d.conversation = 9;
		d.npcName = "Barman";
		d.playerName = "HubPlayer";
		d.npcText = "Looking to buy?";
		d.choices = { "Let's trade.", "No, sorry." };
		d.money = "c. 1,000";
		d.npcPortrait.tex = ui::icons::get(ui::icons::cached("575-gamedata.base", "2146-gamedata.base")).tex;	// a stand-in texture; the player keeps the initial
	}
	if (demo.frame == 52 && demo.showZone) { ui::banner("The Great Desert"); }
	// The stats window: rows from the state, a modifier's colour, values in place, close and Escape.
	if (demo.frame == 50)
	{
		demoStats();
		demo.stats->show(true);
	}
	if (demo.frame == 51)
	{
		ui::Stats& st = *demo.stats;
		check(st.levelText("STRENGTH") == "86" && st.levelText("Precision shooting") == "1" && !st.row("Bows"), "stats: attribute and skill rows from the state");
		check(st.row("Katanas")->screen().x < st.row("Melee attack")->screen().x && st.row("Field medic")->screen().x > st.row("Stealth")->screen().x
			&& st.row("Turrets")->screen().y > st.row("Martial arts")->screen().y, "stats: groups go to their columns, stacked in order");
		check(st.described().empty(), "stats: no description until a row is hovered or clicked");
		const ui::Rect& lp = st.row("Lockpicking")->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, lp.x + lp.w / 2, lp.y + lp.h / 2);
		st.update(demo.ss);
		const std::string about = static_cast<const ui::RichText*>(st.about())->text;
		check(st.described() == "Lockpicking" && about.find("Stats Affected:\n-Chance to pick a lock") != std::string::npos
			&& about.find("Ways To Train:\n-Picking locks") != std::string::npos
			&& static_cast<const ui::RichText*>(st.text())->text.rfind("{StatLabel}Description:\nOpening", 0) == 0, "stats: hovering a row describes it");
		check(clickOn(st.row("Dodge")) && (send(SAPP_EVENTTYPE_MOUSE_MOVE, 1, 1), st.update(demo.ss), st.described() == "Dodge")
			&& static_cast<const ui::RichText*>(st.about())->text.find("35 (-3)") != std::string::npos, "stats: a clicked row stays described, with its modifier");
		const ui::Widget* sab = st.row("Sabres");
		demo.ss.skills[0].rows[1].level = 5;
		st.update(demo.ss);
		check(st.row("Sabres") == sab && st.levelText("Sabres") == "5" && st.described() == "Dodge", "stats: a level change updates the row in place");
		check(st.derived()->preferred().h > 6 * 24, "stats: derived statistics wrap");
		check(clickOn(&static_cast<ui::CaptionWindow*>(st.window())->closeButton()) && !st.shown() && demo.statsClosed == 1, "stats: the close button hides it and tells the host");
		st.show(true);
		ui::Button& pin = static_cast<ui::CaptionWindow*>(st.window())->pinButton();
		check(clickOn(&pin) && st.pinned() && key(SAPP_KEYCODE_ESCAPE) && st.shown() && demo.statsClosed == 1, "stats: pinned, Escape leaves it open");
		check(clickOn(&pin) && !st.pinned() && key(SAPP_KEYCODE_ESCAPE) && !st.shown() && demo.statsClosed == 2, "stats: unpinned, Escape closes it");
		st.show(demo.showStats);
	}
	// --show options: the Interface tab, the pointer on the Progress bars row so the shot (frame 73) has its help line (k16.3.15).
	// Frame 71: the overview map (frames 69-70) covers the options window until frame 71 closes it, and would take the click (k16.3.39).
	if (demo.showOptions && demo.frame == 71)
	{
		auto lower = [](std::string t) { for (char& c : t) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); } return t; };
		int picked = -1;
		for (int i = 0; demo.opts->tab(i); i++) { if (lower(static_cast<const ui::Button*>(demo.opts->tab(i))->text) == lower(demo.optsTab)) { picked = i; clickOn(demo.opts->tab(i)); } }
		check(picked >= 0 && demo.opts->currentTab() == static_cast<size_t>(picked), ("options shot: --options-tab " + demo.optsTab + " is the shown tab").c_str());
		if (!demo.optsSearch.empty())	// --options-search TEXT [--options-hit N]: typed into the find field, the shot on its Nth match (k16.2.227.1)
		{
			ui::focus(demo.opts->search());
			type(demo.optsSearch.c_str());
			for (size_t k = 0; k < demo.opts->matches() && demo.opts->hit() != demo.optsHit - 1; k++) { demo.opts->step(true); }
		}
	}
	if (demo.showOptions && demo.frame == 72 && demo.opts->control(demo.optsRow))	// after frame 72's window drag, which moves the pointer
	{
		const ui::Rect& r = demo.opts->control(demo.optsRow)->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 2, r.y + r.h / 2);
	}
	// The prompt: text and buttons from the spec, Enter / Escape / a click answer once, the backdrop takes the mouse. Each open lays out on the
	// next frame.
	auto fence = [](bool replaced = false)
	{
		ui::PromptSpec p{ "Fence", "You have a 35% chance of getting caught selling this stolen item.\nFence it anyway?", { "Yes", "No" } };
		demo.prompt->open(p, [replaced](int i) { demo.promptLog += (replaced ? "r" : "") + std::to_string(i) + ";"; });
	};
	ui::Prompt& pr = *demo.prompt;
	if (demo.frame == 26)
	{
		fence(true);
		fence();
		check(pr.shown() && static_cast<const ui::RichText*>(pr.text())->text.find("Fence it anyway?") != std::string::npos && pr.button(1) && !pr.button(2),
			"prompt: text and buttons from the spec");
	}
	if (demo.frame == 27)
	{
		check(click(10, 10) && pr.shown() && demo.promptLog.empty(), "prompt: the backdrop takes the mouse, the prompt stays");
		check(key(SAPP_KEYCODE_ENTER) && !pr.shown() && demo.promptLog == "0;", "prompt: Enter picks the default; a replaced prompt never answers");
		fence();
	}
	if (demo.frame == 28)
	{
		check(key(SAPP_KEYCODE_ESCAPE) && !pr.shown() && demo.promptLog == "0;1;", "prompt: Escape picks the last button");
		fence();
	}
	if (demo.frame == 29)
	{
		check(clickOn(pr.button(1)) && !pr.shown() && demo.promptLog == "0;1;1;", "prompt: a button answers");
		check(!click(10, 10), "prompt: closed, the mouse reaches the world again");
	}
	if (demo.frame == 65 && demo.showPrompt) { fence(); }	// after the trade frames, which open and answer their own
	if (demo.frame == 52)
	{
		auto tipOf = [](int grid, const char* name)	// the drags above have moved items between grids: by name
		{
			for (const ui::GridItem& it : demo.gs.grids[grid].items)
			{
				if (it.name == name) { return ui::itemTipText(it.tip); }
			}
			return std::string();
		};
		const std::string bread = tipOf(ui::kGearMain, "Bread"), kit = tipOf(ui::kGearMain, "Basic First Aid Kit");
		check(bread.rfind("{TipTitle}Bread (4)\n{TipGood}[Food]\nNutrition\t20 nu\n", 0) == 0 && bread.find("\nFlat and filling.") != std::string::npos
			&& kit.find("\n{TipDim}-Charges\t600/1,000\n") != std::string::npos
			&& tipOf(ui::kGearBag, "Blueprint").ends_with("\n{TipGood}Right-click to learn tech"), "gear: item tooltips from ItemTip (title (n), tags, rows, muted, hint)");
		const std::string coat = tipOf(ui::kGearArmour, "Dustcoat");
		check(coat.find("\n{TipMaker}[Light Armour Class]\t[Leather]\n") != std::string::npos
			&& coat.find("\n-Sell value\tc.160\n[Coverage]\n-Chest\t80%\n-Stomach\t80%\n\nA long coat") != std::string::npos
			&& coat.find("----") == std::string::npos && tipOf(ui::kGearWeapon1, "Katana").find("\n----------------\n-Weight\t5 kg\n") != std::string::npos,
			"gear: armour tooltip (class / material tag, Coverage rows, blank line, no rule); the weapon's rule before Weight");
		const ui::RichLayout coatRows = ui::layoutRich(coat, *ui::fonts::get("Kenshi_StandardFont_Large"), 300, 20, 8, ui::res::colour("TipText"));
		check(std::ranges::any_of(coatRows.rows, [](const ui::RichRow& r)
			{ return r.text == "[Light Armour Class]" && r.value == "[Leather]" && r.ink == ui::res::colour("TipMaker") && r.valueInk == r.ink; }),
			"gear: a tag's right text is the line's right-aligned value, in the tag's colour");
		// ITEM_STATS resistances (tooltips.md armour 4): before the weight, values in #afa68b, Harpoon in points
		check(coat.find("[Leather]\n-Blunt resistance\t{TipMaker}8%\n") != std::string::npos && coat.find("\n-Harpoon resistance\t{TipMaker}6 pts\n-Weight\t6 kg\n") != std::string::npos
			&& std::ranges::any_of(coatRows.rows, [](const ui::RichRow& r) { return r.text == "-Cut resistance efficiency" && r.value == "50%" && r.valueInk == ui::res::colour("TipMaker"); }),
			"gear: armour resistance rows before the weight, values in TipMaker");
		// Cloth armour (tooltips.md "Cloth and light armour"): the material alone on the right; without coverage, [No Armour Coverage] last
		const std::string robe = ui::itemTipText({ "Plain Robe", 1, { { "", "TipMaker", "[Cloth]" } },
			{ { "-Weight", "2 kg" }, { "[Coverage]", "" }, { "-Chest", "100%" }, { "[Skills]", "" }, { "-Dodge skill effect", "0.90x", false, "TipBad" },
				{ "-Fist protection", "20%", false, "TipGood" }, { "-Martial arts bonus", "+6", false, "TipGood" }, { "", "" } },
			"A stand-in description of our own." });
		const std::string shirt = ui::itemTipText({ "Plain Shirt", 1, {}, { { "-Weight", "1 kg" }, { "[No Armour Coverage]", "", true } } });
		const ui::RichLayout robeRows = ui::layoutRich(robe, *ui::fonts::get("Kenshi_StandardFont_Large"), 300, 20, 8, ui::res::colour("TipText"));
		check(robe.find("\n{TipMaker}\t[Cloth]\n") != std::string::npos && robeRows.rows.size() > 1 && robeRows.rows[1].text.empty() && robeRows.rows[1].value == "[Cloth]"
			&& shirt.ends_with("\n{TipDim}[No Armour Coverage]"), "gear: cloth armour's material alone on the right; [No Armour Coverage] the last line");
		// [Skills] rows (tooltips.md "The [Skills] block"): the label in the line's ink, the value in its own good / bad ink
		check(std::ranges::any_of(robeRows.rows, [](const ui::RichRow& r)
			{ return r.text == "-Dodge skill effect" && r.value == "0.90x" && r.valueInk == ui::res::colour("TipBad") && r.ink != r.valueInk; })
			&& std::ranges::any_of(robeRows.rows, [](const ui::RichRow& r) { return r.text == "-Fist protection" && r.value == "20%" && r.valueInk == ui::res::colour("TipGood"); }),
			"gear: [Skills] mult and percent rows right-aligned in their good / bad colour");
		const ui::ItemGrid* main = demo.gear->grid(ui::kGearMain);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, cellPt(main, 3, 1).x, cellPt(main, 3, 1).y);
		check(main->tip() == bread, "gear: a grid answers with its item's ItemTip");
	}
	// The screenshot's tooltip: hovered at 53, and for the gear and trade modes again at 72, after the overview and window-drag tests (69-72)
	// that move the pointer; their screenshot is at 88, 16 frames (0.53 s) later, past the 0.5 s tooltip delay.
	if ((demo.frame == 53 && !demo.showDialogue) || (demo.frame == 72 && (demo.showGear || demo.showTrade)))
	{
		demo.hoverMe = demo.showStats ? demo.stats->row("Katanas") : demo.showGear ? demo.gear->grid(ui::kGearLegLeft) : demo.hud->medicalBar(4);	// gear: the leg's tooltip, as in the capture
		const ui::Rect& r = demo.hoverMe->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 2, r.y + r.h / 2);
		if (demo.showTrade && demo.frame == 72)	// the trader's hide, where the edge drag left it in view
		{
			const ui::ItemGrid* cont = &demo.cont->grid();
			for (const ui::GridItem& it : cont->items)
			{
				if (it.id == 23) { send(SAPP_EVENTTYPE_MOUSE_MOVE, cellPt(cont, it.x + 1, it.y + 1).x, cellPt(cont, it.x + 1, it.y + 1).y); }
			}
		}
		const int tipGrid[] = { 0, ui::kGearMain, ui::kGearWeapon1, ui::kGearBag, ui::kGearArmour }, tipCell[][2] = { {}, { 3, 1 }, { 3, 0 }, { 5, 5 }, { 1, 1 } };
		if (demo.showTip)
		{
			demo.hoverMe = demo.gear->grid(tipGrid[demo.showTip]);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, cellPt(static_cast<ui::ItemGrid*>(demo.hoverMe), tipCell[demo.showTip][0], tipCell[demo.showTip][1]).x,
				cellPt(static_cast<ui::ItemGrid*>(demo.hoverMe), tipCell[demo.showTip][0], tipCell[demo.showTip][1]).y);
		}
	}
	// The HUD's fixed tip box (ui.hud_tips=box): the medical bar hovered at 53 shows its tip there (no floating tooltip), beside the medical panel
	// and clear of the squad tabs and the RUN / STOP row; ui.hud_tips=float hides it; with nothing hovered (55) it is hidden.
	if (demo.frame == 54 && !demo.showDialogue && !demo.showGear && !demo.showStats)
	{
		demo.hud->update(demo.hs);	// the hover moved at 53, after that frame's update
		const ui::Rect b = demo.hud->hintBox()->screen(), m = demo.hud->medicalPanel()->screen(), run = demo.hud->toggleButton(ui::HudToggle::kRun)->screen();
		const ui::Widget* tab = demo.hud->squadTab(0);
		const float u = ui::scale();
		check(!demo.hud->hintText().empty() && demo.hud->hintText() == demo.hud->medicalBar(4)->tip() && demo.hud->hintBox()->visible,
			"hud tips: a hovered medical bar's tip shows in the fixed box");
		check(b.x >= m.right() && std::abs(b.bottom() - (ui::root().screen().bottom() - 199 * u)) <= 1 && b.w <= 398 * u + 1 && b.bottom() <= run.y
			&& (std::abs(b.w - 398 * u) <= 1 || b.right() <= std::min(demo.hud->moveSpeedDiamond()->screen().x, demo.hud->shortcutsPanel()->screen().x) - 8 * u + 1)
			&& (!tab || b.bottom() <= tab->screen().y),
			"hud tips: the box 398 wide (narrower only to end left of the diamonds), 199 off the bottom, right of the medical panel, above RUN and the squad tabs");
		cfg::setUser("ui.hud_tips", "float");
		demo.hud->update(demo.hs);
		check(!demo.hud->hintBox()->visible && demo.hud->hintText().empty(), "hud tips: ui.hud_tips=float hides the box (the tips float again)");
		cfg::setUser("ui.hud_tips", "box");
		demo.hud->update(demo.hs);
		const ui::Rect st = demo.hud->medicalStatus()->screen(), ext = demo.hud->infoExtended()->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, st.x + st.w / 2, st.y + st.h / 2);
		demo.hud->update(demo.hs);
		const std::string statusTip = demo.hud->hintText();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, ext.x + 20 * u, ext.y + (11 + 21 * 1.5f) * u);	// the Goal row
		demo.hud->update(demo.hs);
		check(statusTip == "In one piece, more or less." && demo.hud->hintText() == "What they are up to.", "hud tips: the status box's health sentence and a state row's tip in the box");
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 2, 2);	// off every widget
	}
	if (demo.frame == 55 && !demo.showDialogue && !demo.showGear && !demo.showStats)
	{
		demo.hud->update(demo.hs);
		check(!demo.hud->hintBox()->visible && demo.hud->hintText().empty(), "hud tips: nothing hovered, the box is hidden");
	}
	// Trading: the trader's window (caption in capitals, the trader's cats), the money format, the fence prompt, a buy by drag.
	if (demo.frame == 54)
	{
		demoTrade();
		demo.gear->show(true);
		demo.gear->showBag(true);
	}
	if (demo.frame == 55)
	{
		ui::Gear& g = *demo.gear;
		const auto* title = &static_cast<ui::CaptionWindow*>(g.window(ui::kGearContainer))->caption();
		check(g.window(ui::kGearContainer)->shown() && title->text == "TRADE GOODS" && title->colour == ui::res::colour("Main") && demo.cont->traderMoney()->shown()
			&& static_cast<const ui::Label*>(demo.cont->traderMoney())->text == "Money: c.25,000", "trade: the trader's window, caption in capitals, the trader's cats");
		check(ui::trade::cats(0) == "c.0" && ui::trade::cats(999) == "c.999" && ui::trade::cats(1000) == "c.1,000" && ui::trade::cats(227249) == "c.227,249"
			&& ui::trade::cats(-10) == "-c.10", "trade: the money format");
		const std::string tip = ui::itemTipText(demo.gs.grids[ui::kGearMain].items.back().tip);
		check(tip.find("\n{TipBad}FENCING CHANCE: 10%") != std::string::npos && tip.find("Price mark-up\t{TipBad}104%") != std::string::npos, "trade: the stolen item's fence and mark-up lines");
		int answer = -1;
		ui::trade::askFence(*demo.prompt, 10, [&answer](bool yes) { answer = yes; });
		check(demo.prompt->shown() && static_cast<const ui::Label&>(static_cast<ui::CaptionWindow*>(demo.prompt->window())->caption()).text == "FENCING ITEM"
			&& static_cast<const ui::RichText*>(demo.prompt->text())->text.rfind("You have a 90% chance", 0) == 0, "trade: the fence prompt (100 - the fencing chance)");
		check(key(SAPP_KEYCODE_ESCAPE) && answer == 0, "trade: Escape on the fence prompt is No");
		demo.gearLog.clear();
		const ui::ItemGrid* cont = &demo.cont->grid();
		const ui::ItemGrid* main = g.grid(ui::kGearMain);
		demo.sounds.clear();
		check(drag(cellPt(cont, 0, 0), cellPt(main, 4, 7)) && demo.gearLog.rfind("move 20 15>9 ", 0) == 0, ("trade: a drag from the trader is an ordinary move (" + demo.gearLog + ")").c_str());
		check(demo.sounds == "Item_Lift(Inventory_Items=Metal);", ("sounds: a buy is Item_Lift with the item's Inventory_Items (" + demo.sounds + ")").c_str());
		demo.gearLog.clear();
		check(drag(cellPt(cont, 2, 0), cellPt(main, 0, 7)) && demo.gearLog == "refused 21;" && ui::messageBox()->text() == "You can't afford that.",
			("trade: a refused buy says so in the HUD message box (" + demo.gearLog + ")").c_str());
		ui::GridItem sold = demo.gs.grids[ui::kGearMain].items.back();	// the stolen plate, back where it was after the check
		sold.soundSwitch = "Iron";
		demo.sounds.clear();
		demo.cont->grid().onDrop(*g.grid(ui::kGearMain), sold, 0, 30);
		check(demo.sounds == "Sell_Item(Inventory_Items=Iron);", ("sounds: a drop into the trader's window is Sell_Item (" + demo.sounds + ")").c_str());
		gearMove(ui::kGearContainer, sold, ui::kGearMain, sold.x, sold.y);
	}
	// The HUD message box: bottom centre, clear of the HUD's bottom stack; messages queue in order, a repeat is dropped; it fades out and is gone.
	if (demo.frame == 56)
	{
		const ui::HudMessage* m = ui::messageBox();
		const ui::Rect r = m->screen();
		const float u = ui::scale();
		check(m->alpha() == 1 && std::abs(r.x + r.w / 2 - sapp_widthf() / 2) <= 1 && std::abs(r.bottom() - (sapp_heightf() - 240 * u)) <= 1
			&& std::abs(r.w - 690 * u) <= 1 && r.bottom() <= demo.hud->toggleButton(ui::HudToggle::kRun)->screen().y,
			"message: the box sits bottom centre (690 x 70, 240 up), above the HUD's toggles");
		ui::message("");
		ui::message("Out of trading range.");
		ui::message("Too far.");
		ui::message("Out of trading range.");
		ui::message("Too far.");
		check(m->text() == "Out of trading range." && m->queued() == 1, "message: the next waits behind the one up, repeats dropped");
		ui::message("");
		ui::message("A short one", ui::res::colour("BadBright"), 0);
	}
	if (demo.frame == 64) { check(ui::messageBox()->alpha() > 0 && ui::messageBox()->alpha() < 1, "message: fades out at the end of its time"); }
	if (demo.frame == 67) { check(ui::messageBox()->text().empty() && ui::messageBox()->alpha() == 0, "message: gone after its time"); }
	if (demo.frame == 68 && demo.showMessage) { ui::message("You can't afford that."); }
	if (demo.frame == 56)	// the 40-row trader scrolls: 19 rows shown, the wheel, the arrows, the track, the thumb
	{
		ui::ItemGrid* cont = &demo.cont->grid();
		const float u = ui::scale();
		const float winH = demo.gear->window(ui::kGearContainer)->preferred().h, charH = demo.gear->window(ui::kGearMain)->preferred().h;
		check(cont->scrolls() && cont->preferred().h == 19 * cont->cell && winH <= charH,
			("trade: a 40-row trader shows 19 rows, its window no taller than the character's (" + std::to_string(winH) + ", " + std::to_string(charH) + ")").c_str());
		const ui::Rect v = cont->view();
		check(send(SAPP_EVENTTYPE_MOUSE_SCROLL, v.x + 20, v.y + 20, -2) && cont->scroll == 2 * cont->cell, "item grid: the wheel scrolls a row a notch");
		const float bx = v.right() + (cont->screen().right() - v.right()) / 2;
		click(bx, cont->screen().bottom() - 4 * u);
		check(cont->scroll == 3 * cont->cell, "item grid: the down arrow steps a row");
		click(bx, cont->screen().bottom() - 20 * u);
		check(cont->scroll == 21 * cont->cell, "item grid: a click on the track below the thumb pages (to the end here)");
		click(bx, cont->screen().y + 4 * u);
		const float top = cont->scroll;
		drag({ bx, cont->thumb().y + 4 * u }, { bx, cont->thumb().y - 600 * u });
		check(top == 20 * cont->cell && cont->scroll == 0, ("item grid: the up arrow, then the thumb dragged to the top (" + std::to_string(cont->scroll) + ")").c_str());
		drag({ bx, cont->thumb().bottom() - 4 * u }, { bx, cont->screen().bottom() + 600 * u });
		check(cont->scroll == 21 * cont->cell, "item grid: the thumb dragged to the end");
	}
	if (demo.frame == 57)	// laid out at the end: the bottom rows show, the top ones are out of reach; an item held at the top edge scrolls the grid
	{
		ui::ItemGrid* cont = &demo.cont->grid();
		const Pt hide = cellPt(cont, 0, 38), first = cellPt(cont, 0, 0);
		const int at = cont->itemAt(hide.x, hide.y);
		check(at >= 0 && cont->items[at].id == 23 && cont->itemAt(first.x, first.y) < 0 && !cont->view().contains(first.x, first.y), "item grid: scrolled to the end, row 38 shows and row 0 is out of view");
		const ui::Rect v = cont->view();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, hide.x, hide.y);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, hide.x, hide.y);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, hide.x, hide.y - 40);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, v.x + 3 * cont->cell * ui::scale(), v.y + 4 * ui::scale());
	}
	if (demo.frame == 63)
	{
		ui::ItemGrid* cont = &demo.cont->grid();
		const float held = cont->scroll;
		demo.gearLog.clear();
		const ui::Rect v = cont->view();
		send(SAPP_EVENTTYPE_MOUSE_UP, v.x + 3 * cont->cell * ui::scale(), v.y + 4 * ui::scale());
		int y = 99;
		sscanf(demo.gearLog.c_str(), "move 23 15>15 %*d,%d", &y);
		check(held > 0 && held < 21 * cont->cell && y < 38, ("item grid: a dragged item at the top edge scrolls it up (" + std::to_string(held) + ", " + demo.gearLog + ")").c_str());
	}
	if (demo.frame == 64)
	{
		demo.opts->show(demo.showOptions);	// --show options: opened after the item grid's drag checks (57-63), which it would cover (k16.3.28)
		if (!demo.showTrade)
		{
			demo.gear->show(false);
			demoGear();
		}
		if (demo.showGear)	// back to what --show gear ends on: the backpack and robotics open
		{
			demo.gear->show(true);
			demo.gear->showBag(true);
			demo.gear->showLimbs(true);
		}
		else
		{
			const ui::ItemGrid* cont = &demo.cont->grid();
			demo.hoverMe = &demo.cont->grid();
			for (const ui::GridItem& it : cont->items)	// the hide, where the edge drag left it in view
			{
				if (it.id == 23) { send(SAPP_EVENTTYPE_MOUSE_MOVE, cellPt(cont, it.x + 1, it.y + 1).x, cellPt(cont, it.x + 1, it.y + 1).y); }
			}
		}
	}

	// The title screen, the save panel and the game menu (frames 87-105: runs that go past 73, the menu modes and interactive ones). A widget
	// shown in one frame is laid out by the next, so a click on it waits a frame.
	ui::SavePanel& sp = *demo.saves;
	ui::MainMenu& mm = *demo.mainMenu;
	ui::GameMenu& gm = *demo.gameMenu;
	auto ids = [&] { std::string r; for (const ui::Table::Row& row : sp.list()->rows()) { r += row.id + ";"; } return r; };
	auto rowPt = [&](int i) { const float u = ui::scale(); const ui::Rect& r = sp.list()->screen(); return Pt{ r.x + 40 * u, r.y + (30 + 28 * i + 14) * u }; };
	if (demo.frame == 87 && !demo.showGear && !demo.showTrade)	// gear / trade shoot at the end of 87, on their tooltip
	{
		ui::closePopups();
		for (const auto& c : ui::root().children())	// the host hides the game's screens under the title
		{
			if (c->visible) { demo.underTitle.push_back(c.get()); }
			c->visible = false;
		}
		ui::banner("Border Zone");
		ui::toast("Hungry Bandits are attacking!");
		ui::message("You can't afford that.");
		mm.show(true);
		check(!ui::messagesShown(), "main menu: showing clears the toasts, message box and zone banner");
		mm.setContinue(false, "No save to continue");
		mm.setOffline("The game server did not start: OpenKenshi-server exited with status 1 (could not open the world data under the data directory); "
			"run client/tools/play.sh game");	// the host's failure line (stand-in wording), long on purpose
	}
	if (demo.frame == 88)
	{
		{
			// No server (kenshi-k16.3.12): the hint wrapped under EXIT, on screen, and the four game buttons' tip; they stay clickable and the click
			// reaches the host, which holds it with setPending (kenshi-k16.2.210.2).
			const ui::Label& off = *mm.offlineLine();
			const ui::Rect l = off.screen(), exitR = mm.button(6)->screen();
			const float u = ui::scale();
			check(off.shown() && !mm.button(0)->enabled && mm.button(1)->enabled && mm.button(2)->enabled && mm.button(3)->enabled
				&& mm.button(4)->enabled && mm.button(5)->enabled && mm.button(6)->enabled && mm.button(1)->tooltip == off.text && mm.button(3)->tooltip == off.text
				&& mm.button(0)->tooltip == "No save to continue",
				"main menu offline: NEW / LOAD / IMPORT live with the hint as their tip; CONTINUE as setContinue left it");
			check(l.y >= exitR.bottom() && l.h >= 2 * off.font->lineHeight() - 0.5f && l.bottom() <= sapp_heightf() && std::abs(l.x - exitR.x) <= 1,
				("main menu offline: a long hint wraps under EXIT, from the column's edge, on screen (" + std::to_string(l.y / u) + " +" + std::to_string(l.h / u) + ")").c_str());
			demo.menuLog.clear();
			clickOn(mm.button(1));
			clickOn(mm.button(3));
			mm.setContinue(true);
			clickOn(mm.button(0));
			check(demo.menuLog == "new;import;continue;", ("main menu offline: CONTINUE / NEW GAME / IMPORT reach the host (" + demo.menuLog + ")").c_str());
			check(mm.button(0)->tooltip == off.text, "main menu offline: a live CONTINUE takes the hint as its tip");
			mm.setContinue(false, "No save to continue");
			mm.setPending(1);
			const bool one = mm.pending() == 1 && mm.button(1)->held && !mm.button(0)->held && !mm.button(2)->held && !mm.button(3)->held;
			mm.setPending(3);
			const bool moved = mm.pending() == 3 && mm.button(3)->held && !mm.button(1)->held;
			mm.setPending(9);
			const bool bad = mm.pending() == -1 && !mm.button(3)->held;
			mm.setPending(1);
			mm.setPending(-1);
			check(one && moved && bad && mm.pending() == -1 && !mm.button(1)->held, "main menu: setPending holds one button pushed, a new one replaces it, -1 (or out of range) none");
			mm.setOffline("Starting the game server...");
			mm.setOffline("Starting the game server...");
			check(off.text == "Starting the game server..." && mm.button(3)->tooltip == off.text && mm.button(3)->enabled, "main menu offline: the status line follows the host");
			mm.setOffline("");
			check(!off.visible && mm.button(1)->enabled && mm.button(2)->enabled && mm.button(3)->enabled && mm.button(1)->tooltip.empty()
				&& !mm.button(0)->enabled && mm.button(0)->tooltip == "No save to continue", "main menu online: the tips cleared, CONTINUE as setContinue left it");
		}
		{
			// The footer (kenshi-379.11.2): name, the host's version, the not-affiliated line; on screen, one line, clear of the column.
			const ui::Label& v = *mm.version();
			const ui::Rect r = v.screen();
			bool clear = true;
			for (int k = 0; k < 7; k++)
			{
				const ui::Rect b = mm.button(k)->screen();
				clear = clear && (r.right() <= b.x || b.right() <= r.x || r.bottom() <= b.y || b.bottom() <= r.y);
			}
			check(v.shown() && v.text == "OpenKenshi 0.1 - not affiliated with or endorsed by Lo-Fi Games. Requires a purchased copy of Kenshi (Steam or GOG)."
				&& r.x >= 0 && r.right() <= sapp_widthf() && r.bottom() <= sapp_heightf() && r.h <= v.font->lineHeight() + 0.5f && clear,
				"main menu: the footer bottom right, on screen, one line, clear of the buttons");
			ui::MainMenu bare;
			check(bare.version()->text.rfind("OpenKenshi dev - not affiliated with or endorsed by Lo-Fi Games.", 0) == 0, "main menu: no version from the host reads dev");
		}
		demo.menuLog.clear();
		check(!clickOn(mm.button(0)) || demo.menuLog.empty(), "main menu: CONTINUE greyed without a save");
		mm.setContinue(true);
		check(clickOn(mm.button(5)) && mm.creditsShown() && clickOn(mm.button(5)) && !mm.creditsShown(), "main menu: CREDITS toggles the credits box");
		check(clickOn(mm.button(0)) && clickOn(mm.button(1)) && clickOn(mm.button(3)) && clickOn(mm.button(4)) && clickOn(mm.button(6))
			&& demo.menuLog == "continue;new;import;options;exit;", ("main menu: the buttons call their actions (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		check(clickOn(mm.button(2)) && sp.shown() && sp.mode() == ui::SaveMode::kLoad && demo.menuLog == "opening load;",
			("main menu: LOAD GAME opens the load panel (" + demo.menuLog + ")").c_str());
		check(ids() == "quicksave;autosave;Hub run;Before the Swamp;Holy Nation start;Zz old;" && sp.list()->selectedId() == "quicksave"
			&& sp.info()->text.rfind("Day\t41", 0) == 0 && sp.actionButton()->enabled && sp.resetTick()->visible && !sp.nameBox()->visible,
			("load panel: newest first, the newest selected, its details shown (" + ids() + ")").c_str());
	}
	if (demo.frame == 89)
	{
		const float u = ui::scale();
		const ui::Rect w = sp.window()->screen(), l = sp.list()->screen();
		check(std::abs(w.w - 680 * u) <= 2 && std::abs(w.h - 325 * u) <= 2 && std::abs(l.h - 271 * u) <= 2,
			("load panel: 680 x 325, the list 271 tall (" + std::to_string(w.w / u) + " x " + std::to_string(w.h / u) + ")").c_str());
		const float hy = l.y + 15 * u;	// the header row
		const bool sorted = click(l.x + 40 * u, hy);
		check(sorted && ids().rfind("autosave;Before the Swamp;", 0) == 0 && sp.list()->selectedId() == "quicksave",
			("table: a header click sorts by it, the selection stays (" + ids() + " " + sp.list()->selectedId() + ")").c_str());
		check(click(l.x + 40 * u, hy) && ids().rfind("Zz old;quicksave;", 0) == 0 && sp.list()->sortDescending(), "table: again reverses it");
		check(click(l.right() - 40 * u, hy) && ids().rfind("Zz old;Holy Nation start;", 0) == 0 && !sp.list()->sortDescending(), "table: another column sorts ascending");
		sp.list()->sortBy(1, true);
		ui::focus(sp.list());
		check(key(SAPP_KEYCODE_DOWN) && sp.list()->selectedId() == "autosave" && sp.info()->text.find("Beep, Ruka, Burn") != std::string::npos,
			"table: Down moves the selection, the details follow");
		check(click(rowPt(2).x, rowPt(2).y) && sp.list()->selectedId() == "Hub run", "table: a click selects a row");
		demo.menuLog.clear();
		check(clickOn(sp.resetTick()) && sp.resetTick()->checked && clickOn(sp.actionButton()) && !sp.shown() && demo.menuLog == "load Hub run 1;",
			("load panel: LOAD loads the selected save, with the reset tick (" + demo.menuLog + ")").c_str());
		sp.open(ui::SaveMode::kLoad);
		demo.menuLog.clear();
		check(key(SAPP_KEYCODE_DELETE) && sp.prompt().shown(), "load panel: Delete asks first");
	}
	if (demo.frame == 90)
	{
		check(clickOn(sp.prompt().button(1)) && !sp.prompt().shown() && demo.menuLog.empty() && sp.saves().size() == 6, "load panel: No keeps the save");
		ui::focus(sp.list());
		key(SAPP_KEYCODE_DELETE);
	}
	if (demo.frame == 91)
	{
		check(clickOn(sp.prompt().button(0)) && demo.menuLog == "remove quicksave;" && sp.saves().size() == 5 && sp.list()->selectedId() == "autosave",
			("load panel: Yes removes it, its neighbour selected (" + demo.menuLog + ")").c_str());
		ui::focus(sp.list());
		const std::string want = "remove quicksave;load autosave " + std::to_string(sp.resetTick()->checked) + ";";
		check(key(SAPP_KEYCODE_ENTER) && !sp.shown() && demo.menuLog == want, ("load panel: Enter in the list loads (" + demo.menuLog + ")").c_str());
		sp.open(ui::SaveMode::kLoad);
		demo.menuLog.clear();
		ui::focus(sp.list());
		check(key(SAPP_KEYCODE_ESCAPE) && !sp.shown() && demo.menuLog == "close;", "load panel: Escape (from the list) closes it");
		mm.show(false);
		for (ui::Widget* w : demo.underTitle) { w->visible = true; }
		demo.underTitle.clear();
		demo.menuLog.clear();
		gm.show(true);
		check(gm.shown() && demo.menuLog == "shown 1;", "game menu: shows (the host's pause hook)");
	}
	if (demo.frame == 92)
	{
		check(clickOn(gm.button(1)) && sp.shown() && sp.mode() == ui::SaveMode::kSave && sp.nameBox()->text == "autosave" && ui::focused(sp.nameBox()),
			("save panel: SAVE GAME opens it, the name box holds the selected save (" + sp.nameBox()->text + ")").c_str());
	}
	if (demo.frame == 93)
	{
		const float u = ui::scale();
		const ui::Rect g = gm.window()->screen(), l = sp.list()->screen();
		const float gh = gm.scenariosButton()->visible ? 394 : 344;	// --debug: the SCENARIOS row under RESUME
		check(std::abs(g.w - 200 * u) <= 2 && std::abs(g.h - gh * u) <= 2 && std::abs(l.h - 225 * u) <= 2, "game menu 200 x 344 (+50 for SCENARIOS); the save list stops above the name box");
		key(SAPP_KEYCODE_END);
		for (int i = 0; i < 8; i++) { key(SAPP_KEYCODE_BACKSPACE); }
		check(type("Fresh start") && sp.actionButton()->enabled, "save panel: typing a name");
		demo.menuLog.clear();
		check(key(SAPP_KEYCODE_ENTER) && !sp.shown() && demo.menuLog == "save Fresh start;" && gm.shown() && ui::focused(gm.window()),
			("save panel: Enter saves a new name, the keys go back to the menu (" + demo.menuLog + ")").c_str());
		sp.open(ui::SaveMode::kSave, "  ");
		check(!sp.actionButton()->enabled, "save panel: no SAVE without a name");
	}
	if (demo.frame == 94)
	{
		check(click(rowPt(1).x, rowPt(1).y) && sp.nameBox()->text == "Hub run", "save panel: a picked row fills the name");
		check(clickOn(sp.actionButton()) && sp.prompt().shown() && sp.shown(), "save panel: saving over a save asks first");
		check(key(SAPP_KEYCODE_ENTER) && !sp.shown() && demo.menuLog == "save Fresh start;opening save;save Hub run;", ("save panel: Yes (Enter) saves over it (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		check(clickOn(gm.button(4)) && gm.prompt().shown(), "game menu: EXIT GAME asks first");
		check(static_cast<const ui::RichText*>(gm.prompt().text())->text == "Exit the game? Unsaved progress will be lost.",
			"game menu: the EXIT GAME confirm says it quits the game (kenshi-k16.2.212)");
	}
	if (demo.frame == 95)
	{
		check(clickOn(gm.prompt().button(1)) && gm.shown() && demo.menuLog.empty(), "game menu: No stays");
		clickOn(gm.button(4));
	}
	if (demo.frame == 96)
	{
		check(clickOn(gm.prompt().button(0)) && !gm.shown() && demo.menuLog == "shown 0;exit;", ("game menu: Yes closes it and exits (" + demo.menuLog + ")").c_str());
		gm.show(true);
	}
	if (demo.frame == 97)
	{
		demo.menuLog.clear();
		check(clickOn(gm.button(3)) && demo.menuLog == "options;", "game menu: OPTIONS");
		ui::focus(gm.window());
		check(key(SAPP_KEYCODE_ESCAPE) && !gm.shown() && demo.menuLog == "options;shown 0;", ("game menu: Escape resumes (" + demo.menuLog + ")").c_str());
		gm.show(true);
	}
	if (demo.frame == 98) { check(clickOn(gm.button(5)) && !gm.shown(), "game menu: RESUME"); }
	ui::NewGamePanel& ng = *demo.newGame;
	ui::Tutorials& tu = *demo.tutorials;
	if (demo.frame == 99)	// the new game panel, over a screen that had the keys (it gets them back)
	{
		gm.show(true);
		demo.menuLog.clear();
		ng.open([] { demo.menuLog += "hidden;"; });
	}
	if (demo.frame == 100)
	{
		check(ng.shown() && ng.current() == 0 && ng.title()->text == "Wanderer" && ui::focused(ng.window()) && ng.advancedButton()->enabled,
			"new game: opens on the first start, with the keys");
		bool order = ng.list()->count() == demo.starts.size();
		for (size_t i = 0; order && i < demo.starts.size(); i++) { order = ng.list()->tab(i)->text == demo.starts[i].title; }
		check(order && ng.list()->selected() == 0, "new game: the list has a row per start, in the host's order, the first chosen");
		check(clickOn(ng.list()->tab(2)) && ng.currentId() == "slaves" && ng.title()->text == "Slaves" && ng.info()->text.rfind("{Special}Slaves", 0) == 0,
			"new game: a click on a row picks that start, the title and description follow");
		check(key(SAPP_KEYCODE_DOWN) && ng.currentId() == "slaves" && key(SAPP_KEYCODE_UP) && ng.currentId() == "nobodies" && ng.list()->selected() == 1,
			"new game: Up steps back, Down stops at the end");
		check(key(SAPP_KEYCODE_UP) && key(SAPP_KEYCODE_UP) && ng.current() == 0 && key(SAPP_KEYCODE_DOWN) && ng.currentId() == "nobodies",
			"new game: Up stops at the top, Down steps on");
		check(clickOn(ng.advancedButton()) && demo.menuLog == "advanced;" && ng.shown(), "new game: ADVANCED OPTIONS calls the host, the panel stays");
		demo.menuLog.clear();
		check(key(SAPP_KEYCODE_ENTER) && !ng.shown() && demo.menuLog == "hidden;begin nobodies;" && ui::focused(gm.window()),
			("new game: Enter begins the start shown, the keys go back (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		ng.open({}, 2);
	}
	if (demo.frame == 101)
	{
		check(ng.currentId() == "slaves" && ng.list()->selected() == 2 && ng.info()->text.rfind("{Special}Slaves", 0) == 0,
			"new game: open with an index chooses that start");
		check(clickOn(ng.beginButton()) && !ng.shown() && demo.menuLog == "begin slaves;", ("new game: BEGIN (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		ng.open();
	}
	if (demo.frame == 102)
	{
		check(key(SAPP_KEYCODE_ESCAPE) && !ng.shown() && demo.menuLog == "cancel;", "new game: Escape cancels");
		demo.menuLog.clear();
		ng.open();
	}
	if (demo.frame == 103)
	{
		check(clickOn(ng.cancelButton()) && !ng.shown() && demo.menuLog == "cancel;", "new game: CANCEL");
		std::vector<ui::StartEntry> v = ng.starts();
		ng.pick(1);
		std::swap(v[0], v[2]);
		ng.setStarts(v);
		check(ng.currentId() == "nobodies" && ng.current() == 1 && ng.list()->selected() == 1 && ng.list()->tab(0)->text == "Slaves",
			"new game: setStarts keeps the choice by id, the rows follow the new order");
		v.resize(1);
		ng.setStarts(v);
		check(ng.currentId() == "slaves" && ng.list()->count() == 1 && ng.beginButton()->enabled, "new game: one start, one row");
		ng.setStarts({});
		check(ng.current() < 0 && ng.list()->count() == 0 && !ng.beginButton()->enabled && ng.title()->text.empty(), "new game: no start greys BEGIN");
		ng.open();
	}
	if (demo.frame == 104)
	{
		check(key(SAPP_KEYCODE_ENTER) && ng.shown() && demo.menuLog == "cancel;", "new game: Enter without a start does nothing");
		ng.close();
		gm.show(false);
		ng.setStarts(demo.starts);
	}
	if (demo.frame == 111)	// the tutorials (tutorial runs only: frames past the menu screenshot)
	{
		demo.menuLog.clear();
		tu.hint("camera");
		tu.hint("squad");
		tu.hint("camera");
		tu.hint("nope");
		tu.open("camera");
	}
	if (demo.frame == 112)
	{
		check(tu.hints().size() == 2 && tu.hintButton(0)->visible && tu.hintButton(0)->text == "Looking around" && tu.hintButton(1)->visible
			&& tu.hintButton(1)->rect.y > tu.hintButton(0)->rect.y, "tutorials: a hint row per tutorial, once, the newest at the bottom, unknown ids ignored");
		check(tu.shown() && ui::focused(tu.window()) && tu.pageLabel()->text == "1 / 2" && tu.nextButton()->text == "SKIP" && !tu.prevButton()->enabled
			&& tu.dismissButton()->visible, "tutorials: the window opens on page 1, SKIP on a waiting page, PREVIOUS greyed");
		check(clickOn(tu.nextButton()) && tu.page() == 1 && tu.nextButton()->text == "FINISH" && !tu.dismissButton()->visible && tu.prevButton()->enabled
			&& demo.menuLog == "page camera 1;", ("tutorials: SKIP turns the page; the last reads FINISH, DISMISS hidden (" + demo.menuLog + ")").c_str());
		check(key(SAPP_KEYCODE_LEFT) && tu.page() == 0 && key(SAPP_KEYCODE_RIGHT) && tu.page() == 1, "tutorials: Left / Right turn the pages");
		demo.menuLog.clear();
		check(clickOn(tu.nextButton()) && !tu.shown() && tu.hints().size() == 1 && tu.hints()[0] == "squad" && demo.menuLog == "done camera;",
			("tutorials: FINISH completes it, its hint goes (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
	}
	if (demo.frame == 113)
	{
		check(clickOn(tu.hintButton(0)) && tu.shown() && tu.openId() == "squad" && tu.text()->text.rfind("The portraits", 0) == 0, "tutorials: a hint row opens its tutorial");
		key(SAPP_KEYCODE_RIGHT);
		check(key(SAPP_KEYCODE_ESCAPE) && !tu.shown() && tu.hints().size() == 1 && demo.menuLog == "page squad 1;", "tutorials: Escape hides it, the hint stays");
		check(clickOn(tu.hintButton(0)) && tu.shown() && tu.page() == 1, "tutorials: the hint reopens it on the page it was left on");
		demo.menuLog.clear();
		tu.open("squad", 9);
	}
	if (demo.frame == 114)
	{
		check(tu.page() == 2 && tu.pageLabel()->text == "3 / 3", "tutorials: open clamps the page");
		tu.open("squad", 1);
	}
	if (demo.frame == 115)
	{
		check(clickOn(tu.dismissButton()) && !tu.shown() && tu.hints().empty() && demo.menuLog == "dismiss squad;", ("tutorials: DISMISS drops it (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		tu.hint("hunger");
	}
	if (demo.frame == 116)
	{
		check(clickOn(tu.hintClose(0)) && tu.hints().empty() && !tu.hintButton(0)->visible && demo.menuLog == "done hunger;",
			("tutorials: a hint's X completes it (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		tu.showBrowser(true);
	}
	if (demo.frame == 117)
	{
		check(tu.browserShown() && tu.list()->items.size() == 17 && tu.list()->selected == 0 && tu.browseText()->text.rfind("Pan the view", 0) == 0
			&& !tu.browsePrev()->enabled && tu.browseNext()->enabled, "tutorials: TUTORIALS lists them all, the first selected");
		check(clickOn(tu.browseNext()) && tu.browseText()->text.rfind("Double-click", 0) == 0 && !tu.browseNext()->enabled, "tutorials: >> pages, greyed on the last");
		tu.list()->selected = 1;
		tu.list()->onPick(1);
		check(tu.browseText()->text.rfind("The portraits", 0) == 0 && key(SAPP_KEYCODE_RIGHT) && tu.browseText()->text.rfind("{Special}Orders", 0) == 0,
			"tutorials: picking a tutorial shows its first page; Right pages");
		check(clickOn(tu.activateButton()) && demo.menuLog == "activate squad;" && tu.hints().size() == 1 && tu.shown() && tu.openId() == "squad",
			("tutorials: ACTIVATE asks the host (here: hint and open) (" + demo.menuLog + ")").c_str());
	}
	if (demo.frame == 118)
	{
		ui::focus(tu.browser());
		check(key(SAPP_KEYCODE_ESCAPE) && !tu.browserShown() && tu.shown(), "tutorials: Escape closes TUTORIALS, the tutorial stays");
		tu.setAll(std::vector<ui::Tutorial>(tu.all().begin() + 1, tu.all().end()));
		check(tu.shown() && tu.list()->items.size() == 16, "tutorials: setAll keeps the open one");
		tu.setAll(std::vector<ui::Tutorial>(tu.all().begin() + 1, tu.all().end()));
		check(!tu.shown() && tu.hints().empty(), "tutorials: setAll without it closes it and drops its hint");
	}
	if (demo.frame == 71 && demo.showContext)
	{
		std::vector<ui::Menu::Item> items = { { "Hungry Bandit", nullptr, false, true }, { "Attack", [] {} }, { "Talk", nullptr, false }, { "Loot", [] {} }, { "Steal", [] {} }, { "First aid", [] {} } };
		items[4].value = "45%";
		auto& m = static_cast<ui::ContextMenu&>(ui::openMenu(std::make_unique<ui::ContextMenu>(std::move(items)), sapp_widthf() * 0.6f, sapp_heightf() * 0.4f));
		const ui::Rect r = m.row(3);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 2, r.y + r.h / 2);
	}
	ui::Build& bu = *demo.build;
	if (demo.frame == 119 && demo.showBuild)
	{
		demo.menuLog.clear();
		bu.update(demo.bs);
		bu.show(true);
	}
	if (demo.frame == 120 && demo.showBuild)
	{
		const ui::Rect m = bu.mainPanel()->screen();
		check(bu.mainPanel()->shown() && !bu.buttonPanel()->shown() && m.x == 0 && std::abs(m.bottom() - sapp_heightf()) < 1 && std::abs(m.w - 1068 * ui::scale()) < 1,
			"build: the main panel bottom left, 1068 wide; no placing panels");
		check(bu.categoryList()->selected == 0 && bu.buildingList()->selected == 1 && bu.title()->text == "Storage Box", "build: the lists show the category and the pick");
		const ui::Rect s0 = bu.statLabel(0)->screen(), s2 = bu.statLabel(2)->screen();
		check(bu.statLabel(0)->text == "Type:" && bu.statValue(2)->colour == ui::style().colour("BuildMaterial", {}) && bu.statLabel(1)->colour == ui::style().textColour(ui::Look::kInset, ui::State::kNormal, false)
			&& std::abs(s2.x - s0.x - ui::Build::kIndent * ui::scale()) < 1 && std::abs(s2.y - s0.y - 2 * ui::Build::kLineH * ui::scale()) < 1 && !bu.statLabel(6),
			"build: stats lines with their colours, materials indented");
		check(bu.prevButton()->shown() && bu.nextButton()->shown() && !bu.undoButton()->enabled, "build: arrows with variants; UNDO off with nothing placed");
		const ui::Rect mb = ui::messageBox()->screen();
		check(std::abs(mb.x - 6 * ui::scale()) < 1 && std::abs(mb.bottom() - (sapp_heightf() - 322.5f * ui::scale())) < 1, "build: the message box over the main panel while open");
		check(ui::focusedWidget() == bu.mainPanel(), "build: the main panel has the keyboard");
		const ui::Rect c = bu.categoryList()->screen();
		const float rowH = c.h / 3;
		check(click(c.x + 20, c.y + rowH * 1.5f) && demo.menuLog == "category power;", "build: a category is the host's order");
	}
	if (demo.frame == 121 && demo.showBuild)
	{
		check(bu.buildingList()->items.size() == 2 && bu.buildingList()->items[0] == "Wind Generator" && bu.buildingList()->selected == -1, "build: the new category's buildings");
		demo.menuLog.clear();
		const ui::Rect l = bu.buildingList()->screen();
		click(l.x + 20, l.y + l.h / 4);
		clickOn(bu.prevButton());
		clickOn(bu.nextButton());
		check(demo.menuLog == "pick wind;step -1;step 1;", ("build: pick and the arrows (" + demo.menuLog + ")").c_str());
		demo.bs.placing = true;
		demo.bs.chooser = false;
		demo.bs.variants = false;
		demo.bs.canUndo = true;
		bu.update(demo.bs);
		ui::message("");
		ui::message("Too close to another building.");	// the host's, on a failed placement click
	}
	if (demo.frame == 122 && demo.showBuild)
	{
		check(!bu.mainPanel()->shown() && bu.buttonPanel()->shown() && bu.commandsPanel()->shown() && ui::messageBox()->alpha() > 0 && bu.undoButton()->enabled
			&& !bu.prevButton()->shown(), "build: placing shows the buttons, commands and the message; no arrows without variants");
		demo.menuLog.clear();
		clickOn(bu.confirmButton());
		clickOn(bu.undoButton());
		clickOn(bu.buildButton());
		check(demo.menuLog == "confirm;undo;build;", "build: CONFIRM, UNDO, the hammer");
		demo.bs.chooser = true;
		bu.update(demo.bs);
	}
	if (demo.frame == 123 && demo.showBuild)
	{
		demo.menuLog.clear();
		key(SAPP_KEYCODE_ESCAPE);
		check(demo.menuLog == "close;", "build: Escape is the host's close");
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 5, 5);
	}
	ui::Research& re = *demo.research;
	auto midRect = [](const ui::Rect& r) { return Pt{ r.x + r.w / 2, r.y + r.h / 2 }; };
	if (demo.frame == 119 && demo.showResearch)
	{
		demo.rsLog.clear();
		demo.rs.caption = "RESEARCH - LEVEL 3 (RATE 1.00X)";	// only here: the overview checks expect the tab's own caption
		re.show(true);
	}
	if (demo.frame == 120 && demo.showResearch)
	{
		using V = std::vector<std::string>;
		check(re.shown() && demo.overview->caption() == "RESEARCH - LEVEL 3 (RATE 1.00X)", "research: the page shows, the host's caption");
		check(re.typeList()->items.size() == 3 && re.typeList()->selected == 1, "research: TYPES from the state, Defence picked");
		check(re.listed() == V{ "lamp", "spikes" }, "research: BLUEPRINTS lists the type's entries, not done, not queued");
		check(re.queueRow(1).w > 0 && re.queueRow(2).w == 0 && re.benchMessage()->text == "One research bench, staffed.", "research: the queue's two rows and the bench message");
		const ui::Rect page = demo.overview->pageWidget(ui::OverviewPage::kResearch).screen(), bl = re.blueprints()->parent()->parent()->screen();
		check(std::abs(bl.x - (page.x + 0.186937f * page.w) - 0.00571429f * 0.394144f * page.w) < 2 && std::abs(bl.w - 0.925714f * 0.394144f * page.w) < 2, "research: BLUEPRINTS where the layout puts it");
		const ui::ButtonRows& br = *static_cast<const ui::ButtonRows*>(re.blueprints());
		const ui::Rect lamp = re.blueprintRow(0);
		check(std::abs(lamp.h - 30 * ui::scale()) < 1.5f && br.progressBar(0).w == 0 && std::abs(br.progressBar(1).w - 0.25f * (lamp.w - 2 * (br.progressBar(1).x - lamp.x))) < 1.5f,
			"research: blueprint buttons 20 of 720 high; a listed one's kept progress is its bar");
		click(lamp.x + lamp.w / 2, lamp.y + lamp.h / 2);
		check(demo.rsLog.empty() && re.described() == "lamp", "research: a click on a blueprint that cannot start only describes it");
		const ui::Rect sp = re.blueprintRow(1);
		click(sp.x + sp.w / 2, sp.y + sp.h / 2);
		check(demo.rsLog == "queue spikes;", ("research: a click on one that can start queues it (" + demo.rsLog + ")").c_str());
		send(SAPP_EVENTTYPE_MOUSE_MOVE, lamp.x + lamp.w / 2, lamp.y + lamp.h / 2);
	}
	if (demo.frame == 121 && demo.showResearch)
	{
		using V = std::vector<std::string>;
		check(re.listed() == V{ "lamp" } && re.queueRow(2).w > 0, "research: the queued blueprint moved to the queue");
		const std::string& d = re.details()->text;
		check(re.described() == "lamp" && d.starts_with("{ResearchName}Watchtower Lamp\tlvl 3\nETA:\t<1 hr (at 1x rate)\n{ResearchMissing}A bigger desk is needed (level 3)\n")
			&& d.find("Cost:\t{ResearchMissing}Book x1") != std::string::npos && d.find("Requirements:\t{ResearchDim}Watchtowers, {ResearchGood}Iron Gate\n") != std::string::npos
			&& d.find("New Building:\t{ResearchUnlock}Watchtower Lamp [light]") != std::string::npos && re.description()->text.starts_with("A lamp"),
			"research: the hovered blueprint's description (the warning, missing red, requirements on one line by state, unlocks)");
		check(re.blueprints()->tip() == "Missing: Book x1", ("research: the blocked one's tooltip says why (" + re.blueprints()->tip() + ")").c_str());
		const ui::ButtonRows& qr = *static_cast<const ui::ButtonRows*>(re.queueList());
		check(qr.progressBar(0).w > 0 && qr.progressBar(2).w > 0 && qr.progressBar(1).w == 0, "research: each queue row its own progress");
		demo.rsLog.clear();
		click(midRect(qr.removeButton(2)).x, midRect(qr.removeButton(2)).y);
		check(demo.rsLog == "unqueue spikes;", ("research: a queue row's X takes it out (" + demo.rsLog + ")").c_str());
		demo.rsLog.clear();
		drag(midRect(re.queueRow(0)), midRect(re.queueRow(1)));
		check(demo.rsLog == "move beds 1;", ("research: a queue row dragged onto another moves there (" + demo.rsLog + ")").c_str());
		demo.rsLog.clear();
		const ui::Rect t = re.typeList()->screen();
		click(t.x + 20, t.y + t.h * 5 / 6);
		check(demo.rsLog == "category farming;", ("research: a TYPES row is the host's (" + demo.rsLog + ")").c_str());
	}
	if (demo.frame == 122 && demo.showResearch)
	{
		using V = std::vector<std::string>;
		check(re.listed() == V{ "wheat" } && re.typeList()->selected == 2 && re.described() == "gate", "research: the new type's blueprints; nothing hovered or clicked shown: the queue's first");
		check(re.details()->text.find("Requirements:\t-\n") != std::string::npos, "research: no requirements show a dash");
		demo.rsLog.clear();
		rclick(midRect(re.queueRow(1)));
		check(demo.rsLog == "unqueue beds;", ("research: a right-click on a queue row takes it out too (" + demo.rsLog + ")").c_str());
		demo.rsLog.clear();
		clickOn(demo.overview->tab(ui::OverviewPage::kMap));
	}
	if (demo.frame == 123 && demo.showResearch)
	{
		check(demo.rsLog == "close;" && demo.overview->caption() == "MAP", ("research: leaving the page is the host's close; the map's own caption (" + demo.rsLog + ")").c_str());
		demo.rs.category = "defence";
		demo.rs.queue = { "beds", "gate" };
		re.show(true);
	}
	if (demo.frame == 124 && demo.showResearch)
	{
		check(demo.overview->caption() == "RESEARCH - LEVEL 3 (RATE 1.00X)", "research: back on the page, the host's caption again");
		const ui::Rect lamp = re.blueprintRow(0);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, lamp.x + lamp.w / 2, lamp.y + lamp.h / 2);	// the screenshot: its description and tooltip
	}
	ui::Crafting& cr = *demo.crafting;
	if (demo.frame == 119 && demo.showCrafting)
	{
		demo.csLog.clear();
		cr.show(true);
	}
	if (demo.frame == 120 && demo.showCrafting)
	{
		using V = std::vector<std::string>;
		check(cr.shown() && demo.overview->caption() == "CRAFTING", "crafting: the page shows, captioned CRAFTING");
		check(cr.stationList()->items.size() == 2 && cr.stationList()->selected == 0 && cr.stationList()->itemColours[1] == ui::style().colour("ResearchBlocked", {})
			&& cr.stationList()->itemTips[1] == "No power", "crafting: STATIONS, the bench picked, the unpowered one red with its status");
		check(cr.listed() == V{ "vest", "helm" } && cr.queueRow(1).w > 0 && cr.queueRow(2).w == 0 && !cr.repeatButton()->checked && !cr.emptyText()->visible, "crafting: ITEMS, the queue (a recipe twice), REPEAT off");
		const auto* ir = static_cast<const ui::ButtonRows*>(cr.items());
		check(ir->rows[0].right.empty() && ir->rows[1].ink.a == 0 && ir->rows[1].tip.empty(), "crafting: ITEMS rows are the name only, in the Main colour");
		const ui::Rect st = cr.stationList()->screen(), qu = cr.queueList()->parent()->parent()->screen(), rp = cr.repeatButton()->screen();
		check(std::abs(st.w - cr.items()->parent()->parent()->screen().w / 0.93617f) < 0.15f * st.w && rp.y >= qu.bottom() - 1, "crafting: three equal columns, REPEAT QUEUE under the queue");
		const ui::Rect page = demo.overview->pageWidget(ui::OverviewPage::kCrafting).screen(), it = cr.items()->parent()->parent()->screen();
		check(std::abs(it.x - (page.x + 0.328829f * page.w)) < 2 && std::abs(it.w - 0.93617f * 0.317568f * page.w) < 2, "crafting: ITEMS where the layout puts it");
		click(midRect(cr.itemRow(1)).x, midRect(cr.itemRow(1)).y);
		check(demo.csLog == "queue bench helm;" && cr.described() == "helm", ("crafting: a click on a recipe queues one, no material check (" + demo.csLog + ")").c_str());
		send(SAPP_EVENTTYPE_MOUSE_MOVE, midRect(cr.itemRow(1)).x, midRect(cr.itemRow(1)).y);
	}
	if (demo.frame == 121 && demo.showCrafting)
	{
		check(cr.queueRow(2).w > 0 && cr.described() == "helm" && cr.describedRow() == -1 && cr.details()->text == "Blunt\t30%\nMade from\tIron Plates x3\n" && cr.description()->text == "Build time\t3 hrs\n",
			"crafting: the hovered recipe's two panels, the host's lines");
		demo.csLog.clear();
		const ui::Rect x2 = static_cast<const ui::ButtonRows*>(cr.queueList())->removeButton(2);
		click(midRect(x2).x, midRect(x2).y);
		check(demo.csLog == "unqueue bench 2;", ("crafting: a queue row's X takes it out by index (" + demo.csLog + ")").c_str());
		demo.csLog.clear();
		drag(midRect(cr.queueRow(0)), midRect(cr.queueRow(1)));
		check(demo.csLog == "move bench 0 1;", ("crafting: a queue row dragged onto another (" + demo.csLog + ")").c_str());
		demo.csLog.clear();
		clickOn(cr.repeatButton());
		check(demo.csLog == "repeat bench 1;" && !cr.repeatButton()->checked, ("crafting: REPEAT QUEUE is the host's (" + demo.csLog + ")").c_str());
		send(SAPP_EVENTTYPE_MOUSE_MOVE, midRect(cr.queueRow(1)).x, midRect(cr.queueRow(1)).y);
	}
	if (demo.frame == 122 && demo.showCrafting)
	{
		check(cr.repeatButton()->checked, "crafting: the next state ticks REPEAT");
		check(cr.described() == "vest" && cr.describedRow() == 1 && cr.details()->text.starts_with("Cut\t40%\n") && cr.description()->text == "Status\tWorking\nDone in\t1 hr\nExpected\t{Bad}Prime\n",
			"crafting: a hovered queue row: the recipe's stats left, the row's own lines right");
		demo.csLog.clear();
		const ui::Rect t = cr.stationList()->screen();
		click(t.x + 20, t.y + t.h * 3 / 4);
		check(demo.csLog == "station loom;", ("crafting: a STATIONS row is the host's (" + demo.csLog + ")").c_str());
	}
	if (demo.frame == 123 && demo.showCrafting)
	{
		check(cr.listed().empty() && cr.emptyText()->visible && cr.emptyText()->text == demo.cs2.empty && cr.queueRow(0).w == 0 && !cr.repeatButton()->checked && cr.described().empty(),
			"crafting: a bench with nothing unlocked: the host's empty text, an empty queue, nothing described");
		demo.csLog.clear();
		clickOn(demo.overview->tab(ui::OverviewPage::kMap));
	}
	if (demo.frame == 124 && demo.showCrafting)
	{
		check(demo.csLog == "close;", ("crafting: leaving the page is the host's close (" + demo.csLog + ")").c_str());
		demo.cs2.station = "bench";
		cr.show(true);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, midRect(cr.itemRow(0)).x, midRect(cr.itemRow(0)).y);	// the screenshot: a description
	}
	ui::Faction& fa = *demo.faction;
	if (demo.frame == 119 && demo.showFaction)
	{
		demo.fsLog.clear();
		fa.show(true);
	}
	if (demo.frame == 120 && demo.showFaction)
	{
		check(fa.shown() && demo.overview->caption() == "FACTION", "faction: the page shows, the tab's caption");
		const ui::Rect page = demo.overview->pageWidget(ui::OverviewPage::kFaction).screen(), list = fa.relations()->parent()->parent()->screen();
		check(std::abs(list.x - page.x) < 2 && std::abs(list.w - 0.5f * page.w) < 2 && std::abs(list.y - (page.y + 0.0120846f * page.h)) < 2, "faction: RELATIONS where the layout puts it");
		check(fa.relationRow(26).w > 0 && fa.relationRow(27).w == 0, "faction: a row per relation");
		const ui::Rect r = fa.relationRow(1), full = fa.relationBar(1), small = fa.relationBar(3), up = fa.relationBar(10), mid = fa.relationBar(4);
		const float centre = r.x + r.w / 2;
		check(fa.relationBar(0).w == 0 && std::abs(full.right() - centre) < 3 && full.w > r.w * 0.45f && std::abs(small.right() - full.right()) < 1 && std::abs(small.w - full.w * 0.11f) < 2,
			"faction: a bar below zero grows left from the middle, |value| / 100 of the half");
		check(std::abs(up.x - full.right()) < 1 && std::abs(up.w - full.w) < 1 && std::abs(mid.x - up.x) < 1 && std::abs(mid.w - full.w * 0.35f) < 2, "faction: a bar above zero grows right");
		const ui::EditBox* name = fa.nameBox();
		const ui::Rect nb = name->screen();
		check(name->text == "Drifters" && std::abs(nb.x - (page.x + 0.601351f * page.w)) < 2, "faction: FACTION NAME, the host's name where the layout puts it");
		clickOn(name);
		key(SAPP_KEYCODE_END);
		type("Xy");
		check(demo.fsLog.empty(), ("faction: no rename while typing (" + demo.fsLog + ")").c_str());
		clickOn(fa.info());	// the focus leaves the box: committed
		check(demo.fsLog == "rename DriftersXy;", ("faction: one rename when the focus leaves it (" + demo.fsLog + ")").c_str());
		clickOn(name);
		type("!");
		key(SAPP_KEYCODE_ESCAPE);
		key(SAPP_KEYCODE_ENTER);
		check(demo.fsLog == "rename DriftersXy;", ("faction: Escape restores, Enter on the unchanged name sends nothing (" + demo.fsLog + ")").c_str());
		clickOn(fa.info());
		send(SAPP_EVENTTYPE_MOUSE_MOVE, midRect(fa.relationRow(3)).x, midRect(fa.relationRow(3)).y);
	}
	if (demo.frame == 121 && demo.showFaction)
	{
		check(fa.relationText(3) == "-11" && fa.relationText(12) == "4" && fa.relationText(1) == "-100" && fa.relationColour(1) == ui::style().colour("RelationBad", {})
			&& fa.relationColour(3) == ui::style().colour("RelationNeutral", {}) && fa.relationColour(10) == ui::style().colour("RelationGood", {}), "faction: the value cut toward zero, red below -30, green above 60");
		check(fa.relations()->tip() == "-11.4", ("faction: a row's tooltip is the exact value (" + fa.relations()->tip() + ")").c_str());
		check(fa.nameBox()->text == "DriftersXy" && !ui::focused(fa.nameBox()), "faction: the name stays the host's after the edit");
		demo.fs.name = "Drifters";
		demo.fs.caption = "FACTION - TEST";
		demo.fsLog.clear();
		const ui::Rect l = fa.relations()->parent()->parent()->screen();
		demo.fsY = fa.relationRow(0).y;
		send(SAPP_EVENTTYPE_MOUSE_SCROLL, l.x + l.w / 2, l.y + l.h / 2, -3);
	}
	if (demo.frame == 122 && demo.showFaction)
	{
		check(fa.relationRow(0).y < demo.fsY, "faction: the relations scroll");
		check(fa.nameBox()->text == "Drifters" && demo.overview->caption() == "FACTION - TEST", "faction: the host's name and caption follow");
		demo.fs.caption.clear();
	}
	if (demo.frame == 123 && demo.showFaction)
	{
		check(demo.overview->caption() == "FACTION", "faction: an empty caption is the tab's");
		clickOn(demo.overview->tab(ui::OverviewPage::kMap));
	}
	if (demo.frame == 124 && demo.showFaction)
	{
		check(demo.fsLog == "close;", ("faction: leaving the page is the host's close (" + demo.fsLog + ")").c_str());
		fa.show(true);
		send(SAPP_EVENTTYPE_MOUSE_SCROLL, fa.relationRow(0).x + 20, fa.relationRow(0).y + 5, 30);	// the screenshot: back at the top
		send(SAPP_EVENTTYPE_MOUSE_MOVE, midRect(fa.relationRow(1)).x, midRect(fa.relationRow(1)).y);	// and a tooltip
	}
	// The building window: placement, the columns per kind, the slots' drags and refusals, QUEUE..., the furnace's toggle, close.
	ui::BuildingWindow& bw = *demo.building;
	if (demo.frame == 119 && demo.showBuilding) { demo.bws = buildingCase(0); }
	if (demo.frame == 120 && demo.showBuilding)
	{
		const ui::Rect w = bw.window()->screen();
		check(bw.shown() && bw.window()->caption().text == "ORE SMELTER" && w.x < 1 && std::abs(w.bottom() - sapp_heightf() * 470 / 720) < 2, "building: left edge at 0, bottom at 470 of 720 lines, the name in capitals");
		check(bw.column(ui::kBuildingIn1)->visible && !bw.column(ui::kBuildingIn2)->visible && bw.column(ui::kBuildingOut)->visible && bw.title(false)->text == "INPUT"
			&& bw.nameText(ui::kBuildingIn1)->text == "Lump Ore" && !bw.statusText(ui::kBuildingIn1)->visible, "building: a refinery: one INPUT and the OUTPUT, no status line");
		const ui::Rect b = bw.bar(ui::kBuildingOut), f = bw.barFill(ui::kBuildingOut), g = bw.grid(ui::kBuildingIn1)->screen();
		const float barH = ui::style().natural(ui::Look::kSlotBar).h;
		check(b.w >= std::round(168 * ui::scale()) - 1 && std::abs(b.h - std::round(barH * ui::scale())) < 2 && (barH == 6 || barH == 24) && f.w > 0 && f.w < b.w && std::abs(g.w - 3 * 25 * ui::scale()) < 2,
			("building: the bar at least 168 wide, the style's height (flat 6, original 24), filled to the host's progress; the slot is the footprint in cells ("
				+ std::to_string(b.w / ui::scale()) + " x " + std::to_string(b.h / ui::scale()) + ")").c_str());
		const ui::ItemGrid* in = bw.grid(ui::kBuildingIn1);
		const ui::ItemGrid* out = bw.grid(ui::kBuildingOut);
		demo.bwLog.clear();
		bw.window()->raise();
		drag(cellPt(in, 0, 0), cellPt(in, 2, 1));
		drag(cellPt(in, 2, 1), cellPt(out, 3, 2));
		drag(cellPt(out, 0, 0), cellPt(in, 1, 0));
		check(demo.bwLog == "move 50 100 7 100 2 1;", ("building: moves within an input; the output takes no drop; an input only its ingredient (" + demo.bwLog + ")").c_str());
		demo.bwLog.clear();
		rclick(cellPt(out, 0, 0));
		check(demo.bwLog == "quick 50 102 8;", ("building: a right-click quick-moves (" + demo.bwLog + ")").c_str());
		demo.bwW = w.w;
		demo.bws = buildingCase(1);
	}
	if (demo.frame == 121 && demo.showBuilding)
	{
		const ui::Rect w = bw.window()->screen();
		check(!bw.column(ui::kBuildingIn1)->visible && !bw.title(false)->parent()->visible && bw.column(ui::kBuildingOut)->visible && w.w < demo.bwW
			&& std::abs(w.bottom() - sapp_heightf() * 470 / 720) < 2, "building: a drill: the OUTPUT alone, narrower, the same bottom edge");
		check(!bw.craftingPanel()->visible && !bw.furnacePanel()->visible, "building: no crafting or furnace panel on production buildings");
		{
			const ui::Rect g = bw.grid(ui::kBuildingOut)->screen(), col = bw.column(ui::kBuildingOut)->screen(), b = bw.bar(ui::kBuildingOut), f = bw.barFill(ui::kBuildingOut);
			check(bw.window()->caption().text == "ORE DRILL" && bw.title(true)->visible && bw.title(true)->text == "OUTPUT", "building: a mine reads its name in capitals, then OUTPUT");
			check(std::abs(g.w - 3 * 25 * ui::scale()) < 2 && std::abs(g.h - 2 * 25 * ui::scale()) < 2 && std::abs((g.x + g.w / 2) - (col.x + col.w / 2)) <= 1,
				"building: the mine's grid is 3 x 2 cells, centred in its column");
			check(b.y > g.bottom() && std::abs(f.w / b.w - demo.bws.output->bar) < 0.05f, ("building: the bar under the grid, filled to the host's progress ("
				+ std::to_string(f.w / b.w) + ")").c_str());	// the original's fill is inside its frame
		}
		const ui::ItemGrid* out = bw.grid(ui::kBuildingOut);	// the drill's output: Lump Ore (kind 1) at 0,0 in 3 x 2
		demo.bwLog.clear();
		bw.window()->raise();
		drag(cellPt(out, 0, 0), cellPt(out, 2, 1));
		check(demo.bwLog.empty(), ("building: outputDropIn off, the output refuses even its own item (" + demo.bwLog + ")").c_str());
		demo.bws.outputDropIn = true;
		bw.update(demo.bws);
		ui::frame(0);
		drag(cellPt(out, 0, 0), cellPt(out, 2, 1));
		check(demo.bwLog == "move 51 102 7 102 2 1;", ("building: outputDropIn on, the output takes a drop, a plain move (" + demo.bwLog + ")").c_str());
		{	// kenshi-k16.2.122.1: a 3 x 2 item in its 3 x 2 output, grabbed at cell (1,1), let go a cell to the right: it lands at 0,0
			ui::GridItem rock;
			rock.id = 11;
			rock.name = "Ore Block";
			rock.w = 3;
			rock.h = 2;
			rock.kind = 1;
			rock.count = 1;
			demo.bws.output->items = { rock };
			bw.update(demo.bws);
			ui::frame(0);
			demo.bwLog.clear();
			ui::message("");
			drag(cellPt(out, 1, 1), cellPt(out, 2, 1));
			check(demo.bwLog == "move 51 102 11 102 0 0;", ("building: a drop off the footprint's origin lands where the item fits (" + demo.bwLog + ")").c_str());
			ui::GridItem plate = rock;	// a 3 x 2 from an input: refused first by its kind, then for room
			plate.id = 12;
			plate.kind = 2;
			demo.bws.inputs = { { "Metal Sheets", "", "", 3, 2, { 2 }, { plate }, 0 } };
			bw.update(demo.bws);
			ui::frame(0);
			const ui::ItemGrid* in = bw.grid(ui::kBuildingIn1);
			demo.bwLog.clear();
			drag(cellPt(in, 1, 1), cellPt(out, 1, 1));
			const std::string kindMsg = ui::messageBox()->text();
			ui::message("");
			plate.kind = 1;
			demo.bws.inputs[0].accepts = { 1 };
			demo.bws.inputs[0].items = { plate };
			ui::GridItem chip = rock;	// the output holds a 2 x 2 and a 1 x 1: the 3 x 2 overlaps both
			chip.id = 13;
			chip.w = chip.h = 1;
			rock.w = 2;
			demo.bws.output->items = { rock, chip };
			demo.bws.output->items[1].x = 2;
			bw.update(demo.bws);
			ui::frame(0);
			drag(cellPt(in, 1, 1), cellPt(out, 1, 1));
			const std::string roomMsg = ui::messageBox()->text();
			check(demo.bwLog.empty() && kindMsg == "Can't put that there." && roomMsg == "No room for that item.",
				("building: a refused drop says why: the kind (" + kindMsg + "), no room (" + roomMsg + "); no move (" + demo.bwLog + ")").c_str());
			ui::message("");
			demo.bws = buildingCase(1);
			demo.bws.outputDropIn = true;
		}
		demo.bws.outputDropIn = false;
		demo.bws = buildingCase(2);
	}
	if (demo.frame == 122 && demo.showBuilding)
	{
		check(bw.craftingPanel()->visible && bw.craftingName()->text == "Gadget" && bw.title(false)->text == "INPUTS" && bw.column(ui::kBuildingIn2)->visible, "building: a bench: the item in progress, two inputs");
		check(bw.statusText(ui::kBuildingIn1)->text == "OK" && bw.statusText(ui::kBuildingIn1)->colour == ui::style().colour("HudGood", {}) && bw.statusText(ui::kBuildingIn2)->text == "Empty!",
			"building: a bench's status lines, the host's text and colour");
		demo.bwLog.clear();
		bw.window()->raise();
		clickOn(bw.queueButton());
		check(demo.bwLog == "queue 52;", ("building: QUEUE... is the host's (" + demo.bwLog + ")").c_str());
		demo.bws = buildingCase(3);
	}
	if (demo.frame == 123 && demo.showBuilding)
	{
		check(bw.furnacePanel()->visible && bw.metalsText()->text == "1.5" && bw.metalsText()->colour == ui::style().colour("HudGood", {}) && bw.activeButton()->text == "OFF"
			&& !bw.activeButton()->tooltip.empty() && bw.grid(ui::kBuildingIn1)->accepts.empty() && !bw.column(ui::kBuildingOut)->visible, "building: a furnace: one open slot, Total Metals, Active OFF with its warning");
		demo.bwLog.clear();
		bw.window()->raise();
		clickOn(bw.activeButton());
		check(demo.bwLog == "active 53 1;", ("building: Active is the host's (" + demo.bwLog + ")").c_str());
	}
	if (demo.frame == 124 && demo.showBuilding)
	{
		check(bw.activeButton()->text == "ON", "building: the next state shows ON");
		demo.bwLog.clear();
		clickOn(&bw.window()->closeButton());
		check(demo.bwLog == "close 53;", ("building: the close X is the host's (" + demo.bwLog + ")").c_str());
		demo.bws = buildingCase(demo.buildingShot);	// the screenshot: the bench, or the drill (--show mine)
	}
	// A looted body (SIMLINK 13 loot open): a second Gear docked right of the looter's, its grids numbered from kGearOther, its title red when foreign.
	ui::Gear& lg = *demo.lootGear;
	if (demo.frame == 119 && demo.showGearBody)
	{
		demo.gear->show(false);
		demoGear();
		demo.gs.grids[ui::kGearContainer] = {};
		demo.lgs = demo.gs;
		demo.lgs.owner = 77;
		demo.lgs.name = "Dust Bandit";
		demo.lgs.nameForeign = true;
		demo.lgs.info.clear();
		demo.gs.grids[ui::kGearWeapon1].items.clear();
		demo.gear->dock(ui::GearDock::kLeft);
		demo.gear->show(true);
		lg.dock(ui::GearDock::kRight);
		lg.show(true);
	}
	if (demo.frame == 121 && demo.showGearBody)
	{
		const ui::Rect mine = demo.gear->window(ui::kGearMain)->screen(), body = lg.window(ui::kGearMain)->screen();
		check(lg.shown() && mine.right() < body.x && std::fabs((mine.right() + body.x) / 2 - sapp_widthf() / 2) < 2, "gear body: the looter's window left, the body's right, meeting at the centre");
		check(lg.window(ui::kGearMain) != demo.gear->window(ui::kGearMain) && lg.grid(ui::kGearMain)->tag == ui::kGearOther + ui::kGearMain, "gear body: its grids are numbered from kGearOther");
		demo.gearLog.clear();
		check(drag(cellPt(lg.grid(ui::kGearWeapon1), 0, 0), cellPt(demo.gear->grid(ui::kGearMain), 0, 7)) && demo.gearLog.rfind("move 2 201>9 ", 0) == 0,
			("gear body: a worn piece drags off the body into the looter's pack, named as the body's (" + demo.gearLog + ")").c_str());
		demo.gearLog.clear();
		rclick(cellPt(lg.grid(ui::kGearMain), 0, 0));
		check(demo.gearLog == "menu 77 209 6;", ("gear body: a right-click hands back the body and its grid (" + demo.gearLog + ")").c_str());
		lg.showBag(true);
		demo.gear->showBag(true);
	}
	if (demo.frame == 123 && demo.showGearBody)
	{
		check(lg.window(ui::kGearBag)->screen().x > lg.window(ui::kGearMain)->screen().right() && demo.gear->window(ui::kGearBag)->screen().right() < demo.gear->window(ui::kGearMain)->screen().x,
			"gear body: each backpack opens outwards");
		demo.gearLog.clear();
		clickOn(&static_cast<ui::CaptionWindow*>(lg.window(ui::kGearMain))->closeButton());
		check(demo.gearLog == "loot close;" && lg.shown(), "gear body: the close X is the host's");
	}
	// The pin (kinspect loot.md: Kenshi_PinButtonSkin left of the X): both character windows have it; pinned, Escape spares the window; hiding unpins
	if (demo.frame == 124 && demo.showGearBody)
	{
		check(demo.gear->pinButton()->visible && lg.pinButton()->visible && !lg.pinned(), "gear body: both character windows show the pin, off");
		demo.gearLog.clear();
		check(clickOn(lg.pinButton()) && lg.pinned() && !demo.gear->pinned(), "gear body: a click pins the body's window alone");
		key(SAPP_KEYCODE_ESCAPE);
		check(lg.shown() && demo.gearLog.find("loot close") == std::string::npos, ("gear body: Escape spares the pinned window (" + demo.gearLog + ")").c_str());
		lg.show(false);
		lg.show(true);
		check(!lg.pinned(), "gear body: hiding the window unpins it");
	}
	if (demo.frame == 125 && demo.showGearBody)
	{
		demo.gearLog.clear();
		key(SAPP_KEYCODE_ESCAPE);
		check(demo.gearLog.find("loot close") != std::string::npos, ("gear body: unpinned, Escape is the host's close (" + demo.gearLog + ")").c_str());
	}
	if (demo.frame == 126 && demo.showGearBody) { demo.gear->showBag(false); lg.showBag(false); }
	// Several item windows (kenshi-k16.20.13.1.1): a character, a box and a trader at once; GridRef actions; the target mark.
	ui::ItemWindows& iw = *demo.items;
	const auto boxState = [](uint64_t owner)
	{
		ui::GridItem rope, hemp;
		rope.id = 30;
		rope.name = "Rope";
		rope.w = 2;
		hemp.id = 31;
		hemp.name = "Hemp";
		hemp.w = hemp.h = 2;
		hemp.x = 3;
		return ui::ContainerState{ .owner = owner, .name = "Storage Box", .capacity = "Capacity: 30", .grid = { 6, 5, false, false, {}, { rope, hemp } } };
	};
	static ui::Rect itemsTrader;	// the trader's window after its move (frame 122)
	if (demo.frame == 119 && demo.showItems)
	{
		demo.gear->show(false);
		demoTrade();
		ui::GearState c = demo.gs;
		const ui::ContainerState trader{ .owner = 502, .name = demo.ccs.name, .trade = demo.ccs.trade, .grid = c.grids[ui::kGearContainer] };
		c.grids[ui::kGearContainer] = {};
		demo.iws = { { c }, { boxState(501), trader }, 0 };
	}
	if (demo.frame == 121 && demo.showItems)
	{
		ui::CaptionWindow *cw = iw.window(101), *bw = iw.window(501), *tw = iw.window(502);
		check(cw && bw && tw && cw->visible && bw->visible && tw->visible && cw != bw && bw != tw, "items: a character, a box and a trader open as three windows");
		if (cw && bw && tw)
		{
			const float s = ui::scale();
			const ui::Rect c = cw->screen(), b = bw->screen(), t = tw->screen();
			check(iw.docked() == 501 && b.x > c.right() && std::fabs(b.y - c.y) < 1, "items: the first container docks right of the character window, tops level (Gear's dock)");
			check(std::fabs(t.x - b.x - ui::ItemWindows::kCascade * s) < 1.5f && std::fabs(t.y - b.y - ui::ItemWindows::kCascade * s) < 1.5f,
				"items: the second container cascades 30 units right and down from the docked one");
			check(iw.ref(iw.container(501)->grid().tag) == ui::GridRef{ 501, ui::kGearContainer } && iw.ref(iw.character(101)->grid(ui::kGearMain)->tag) == ui::GridRef{ 101, ui::kGearMain }
				&& iw.ref(5).owner == 0, "items: each grid's tag maps back to its window's owner and grid");
			check(cw->targetButton().visible && bw->targetButton().visible && tw->targetButton().visible && tw->pinButton().visible
				&& !cw->targetButton().checked && !bw->targetButton().checked && !tw->targetButton().checked, "items: every window shows the target toggle (and the pin), none marked");
			tw->moveX = 300;	// the user moves the trader's window off the box
		}
	}
	if (demo.frame == 122 && demo.showItems)
	{
		ui::CaptionWindow *bw = iw.window(501), *tw = iw.window(502);
		ui::ItemGrid *box = &iw.container(501)->grid(), *stock = &iw.container(502)->grid(), *main = iw.character(101)->grid(ui::kGearMain);
		itemsTrader = tw->screen();
		check(std::fabs(itemsTrader.x - bw->screen().x - (ui::ItemWindows::kCascade + 300) * ui::scale()) < 2, "items: a cascaded window moves on its own");
		demo.itemsLog.clear();
		check(drag(cellPt(box, 0, 0), cellPt(stock, 5, 5)) && demo.itemsLog == "move 30 501:15>502:15 5,5;", ("items: a drag box -> trader names both windows' GridRefs (" + demo.itemsLog + ")").c_str());
		demo.itemsLog.clear();
		check(drag(cellPt(main, 2, 0), cellPt(box, 0, 2)) && demo.itemsLog == "move 7 101:9>501:15 0,2;", ("items: a drag character -> box (" + demo.itemsLog + ")").c_str());
		demo.itemsLog.clear();
		rclick(cellPt(box, 3, 0));
		rclick(cellPt(box, 3, 0), SAPP_MODIFIER_CTRL);
		check(demo.itemsLog == "quick 501:15 31 0;all 501:15 31;", ("items: a right click is quick, Ctrl + right click moveAll (" + demo.itemsLog + ")").c_str());
		demo.itemsLog.clear();
		check(clickOn(&bw->targetButton()) && demo.itemsLog == "mark 501;", ("items: the target toggle marks its window (" + demo.itemsLog + ")").c_str());
		demo.iws.target = 501;	// the host's answer
	}
	if (demo.frame == 123 && demo.showItems)
	{
		ui::CaptionWindow *cw = iw.window(101), *bw = iw.window(501), *tw = iw.window(502);
		check(bw->targetButton().checked && !cw->targetButton().checked && !tw->targetButton().checked, "items: the state's target is drawn on its window alone");
		demo.itemsLog.clear();
		clickOn(&bw->targetButton());
		clickOn(&tw->targetButton());
		check(demo.itemsLog == "mark 0;mark 502;", ("items: the toggle on the marked window unmarks, on another marks that one (" + demo.itemsLog + ")").c_str());
		demo.itemsLog.clear();
		clickOn(&tw->pinButton());
		clickOn(&tw->closeButton());
		check(demo.itemsLog == "pin 502 1;close 502;", ("items: the pin and the close X are the host's, by owner (" + demo.itemsLog + ")").c_str());
		// The right-click verb, its badge, the item menu (quick with kMenu, or a held right press) and place-one (kenshi-k16.20.13.1.2).
		ui::ItemGrid* box = &iw.container(501)->grid();
		const Pt hemp = cellPt(box, 3, 0), rope = cellPt(box, 0, 0), spare = cellPt(box, 0, 3);
		auto hover = [&](Pt p, uint32_t mods) { send(SAPP_EVENTTYPE_MOUSE_MOVE, p.x, p.y, 0, mods); iw.update(demo.iws); };
		demo.itemsVerb = ui::RightVerb::kSell;
		hover(hemp, 0);
		const bool sell = iw.badge() == ui::RightVerb::kSell && iw.badgeMods() == 0;
		hover(hemp, SAPP_MODIFIER_CTRL);
		check(sell && iw.badge() == ui::RightVerb::kSell && (iw.badgeMods() & SAPP_MODIFIER_CTRL), "items: over an item the badge is the host's verb, with the modifiers held (Ctrl: all)");
		hover(spare, 0);
		check(iw.badge() == ui::RightVerb::kNone, "items: over no item, no badge");
		demo.itemsLog.clear();
		rclick(hemp);
		check(demo.itemsLog == "quick 501:15 31 0;" && !iw.menu(), ("items: a quick right click with a verb is quick(), no menu (" + demo.itemsLog + ")").c_str());
		demo.itemsVerb = ui::RightVerb::kMenu;
		hover(hemp, 0);
		const bool none = iw.badge() == ui::RightVerb::kMenu;	// drawn as nothing
		demo.itemsLog.clear();
		rclick(hemp);
		ui::ItemMenu* m = iw.menu();
		check(none && demo.itemsLog.empty() && m && m->items.size() == 4 && m->items[0].text == "Hemp" && !m->pickable(0) && m->bindings[2] == "Ctrl+Right click"
			&& !m->pickable(3), ("items: verb kMenu: a quick right click opens the item menu, titled, its bindings, a disabled row (" + demo.itemsLog + ")").c_str());
		ui::closePopups();
		demo.itemsVerb = ui::RightVerb::kSell;
		auto rightEv = [&](sapp_event_type t, Pt p)
		{
			sapp_event e = {};
			e.type = t;
			e.mouse_x = p.x;
			e.mouse_y = p.y;
			e.mouse_button = SAPP_MOUSEBUTTON_RIGHT;
			return ui::event(e);
		};
		auto rowPt = [](const ui::ItemMenu* m, size_t i) { const ui::Rect r = m->row(i); return Pt{ r.x + r.w / 2, r.y + r.h / 2 }; };
		demo.itemsLog.clear();
		rightEv(SAPP_EVENTTYPE_MOUSE_MOVE, hemp);
		rightEv(SAPP_EVENTTYPE_MOUSE_DOWN, hemp);
		ui::frame(0.1);
		const bool early = !iw.menu();
		ui::frame(0.15);
		ui::frame(0);
		m = iw.menu();
		check(early && m && m->items.size() == 4 && demo.itemsLog.empty(), ("items: a right press held 0.2 s still on an item opens the menu, nothing quick (" + demo.itemsLog + ")").c_str());
		if (m)
		{
			rightEv(SAPP_EVENTTYPE_MOUSE_MOVE, rowPt(m, 3));
			rightEv(SAPP_EVENTTYPE_MOUSE_UP, rowPt(m, 3));
			check(iw.menu() == m && demo.itemsLog.empty(), ("items: released on the disabled row, the menu stays open (" + demo.itemsLog + ")").c_str());
			click(rowPt(m, 2).x, rowPt(m, 2).y);
			check(!iw.menu() && demo.itemsLog == "pick 501:15 31 2;", ("items: then a click on a row picks it (" + demo.itemsLog + ")").c_str());
		}
		demo.itemsLog.clear();
		rightEv(SAPP_EVENTTYPE_MOUSE_MOVE, hemp);
		rightEv(SAPP_EVENTTYPE_MOUSE_DOWN, hemp);
		ui::frame(0.25);
		ui::frame(0);
		m = iw.menu();
		if (m)
		{
			rightEv(SAPP_EVENTTYPE_MOUSE_MOVE, rowPt(m, 1));
			rightEv(SAPP_EVENTTYPE_MOUSE_UP, rowPt(m, 1));
		}
		check(m && !iw.menu() && demo.itemsLog == "pick 501:15 31 1;", ("items: a held press released on a row picks it, nothing quick (" + demo.itemsLog + ")").c_str());
		demo.itemsVerb = ui::RightVerb::kNone;
		demo.itemsLog.clear();
		rightEv(SAPP_EVENTTYPE_MOUSE_MOVE, rope);
		rightEv(SAPP_EVENTTYPE_MOUSE_DOWN, rope);
		rightEv(SAPP_EVENTTYPE_MOUSE_MOVE, spare);
		rightEv(SAPP_EVENTTYPE_MOUSE_UP, spare);
		check(demo.itemsLog.empty() && !iw.menu(), ("items: a right press moved off its item drags nothing and clicks nothing (" + demo.itemsLog + ")").c_str());
		// place-one (ui.place_one, the original's setting): a lifted 3-stack is put down one unit per click, the rest stays lifted.
		cfg::set("ui.place_one=true");	// a pin, not saved
		iw.update(demo.iws);
		const int ri = box->itemAt(rope.x, rope.y);
		if (ri >= 0) { box->items[ri].count = 3; }
		demo.itemsLog.clear();
		click(rope.x, rope.y, SAPP_MODIFIER_SHIFT);
		click(spare.x, spare.y);
		const ui::GridItem* still = ui::ItemGrid::dragged();
		check(ri >= 0 && demo.itemsLog == "move 30 501:15>501:15 0,3;" && still && still->count == 2,
			("items: place-one puts one of a lifted 3-stack down, 2 stay lifted (" + demo.itemsLog + ")").c_str());
		key(SAPP_KEYCODE_ESCAPE);
		cfg::set("ui.place_one=false");
		iw.update(demo.iws);
		if (ri >= 0) { box->items[ri].count = 1; }
		check(!ui::ItemGrid::placeOne && !ui::ItemGrid::dragged(), "items: place-one off by default");
	}
	if (demo.frame == 124 && demo.showItems)
	{
		check(iw.window(501)->targetButton().checked && !iw.window(502)->targetButton().checked, "items: the marks follow the state, not the clicks");
		{
			// A container with no character window (a left click on a box): its window alone, centred at ui.gear_top (was Gear's characterHidden,
			// kenshi-k16.20.13.1.3).
			ui::ItemWindows lone({});
			lone.update({ {}, { boxState(601) }, 0 });
			ui::frame(0);
			const ui::CaptionWindow* w = lone.window(601);
			check(w && w->visible && !lone.character(601) && w->rect.x == 0 && (ui::gearTop() < 0 || std::fabs(w->screen().y - ui::gearTop() * ui::scale()) < 2),
				"items: a container alone is centred at ui.gear_top");
		}
		ui::frame(0);
		ui::GearState body = demo.iws.characters[0];
		body.owner = 77;
		body.name = "Dust Bandit";
		body.nameForeign = true;
		body.info.clear();
		demo.iws.characters.push_back(body);
		demo.iws.containers.erase(demo.iws.containers.begin());	// the docked box closes
		demo.iws.target = 0;
	}
	if (demo.frame == 125 && demo.showItems)
	{
		const ui::Rect mine = iw.window(101)->screen(), body = iw.window(77)->screen(), t = iw.window(502)->screen();
		check(!iw.window(501) && iw.docked() == 0 && std::fabs(t.x - itemsTrader.x) < 1 && std::fabs(t.y - itemsTrader.y) < 1, "items: the docked one closing leaves the others where they are");
		check(mine.right() < body.x && std::fabs((mine.right() + body.x) / 2 - sapp_widthf() / 2) < 2, "items: two character windows dock left and right, meeting at the centre");
		check(iw.ref(iw.character(77)->grid(ui::kGearMain)->tag) == ui::GridRef{ 77, ui::kGearMain }, "items: the body's grids name the body");
		demo.iws.containers.push_back(boxState(503));
	}
	if (demo.frame == 126 && demo.showItems)
	{
		check(iw.docked() == 503 && iw.window(503)->screen().right() < iw.window(101)->screen().x, "items: the next container to open docks (beside the looter's, outwards)");
		demo.iws.characters.pop_back();	// the screenshot: the character, the box docked, the trader moved off it, the box marked
		demo.iws.target = 503;
	}
	// The stack (kenshi-k16.20.13.1.4): four windows (two characters, two containers); a new one opens on top, a left press anywhere on one
	// raises it, Escape closes the topmost unpinned one and then the next down, a pinned one is skipped; none (or only pinned ones): the host's.
	static ui::ItemWindowsState itemsFour;
	const auto itemsWins = [&iw](uint64_t o)
	{
		std::vector<const ui::Widget*> v;
		if (const ui::Gear* g = iw.character(o)) { for (int k : { ui::kGearMain, ui::kGearBag, ui::kGearArmLeft }) { v.push_back(g->window(k)); } }
		else { v.push_back(iw.window(o)); }
		return v;
	};
	const auto itemsFree = [&](uint64_t self, const std::vector<Pt>& cands)	// the first point no other item window covers; x -1: none
	{
		for (Pt p : cands)
		{
			bool covered = false;
			for (uint64_t o : iw.stack()) { if (o != self) { for (const ui::Widget* w : itemsWins(o)) { covered = covered || (w && w->visible && w->screen().contains(p.x, p.y)); } } }
			if (!covered) { return p; }
		}
		return Pt{ -1, -1 };
	};
	const auto itemsStack = [&iw]
	{
		std::string t;
		for (uint64_t o : iw.stack()) { t += std::to_string(o) + " "; }
		return t + "top " + std::to_string(iw.top()) + (ui::focusedWidget() == iw.window(iw.top()) ? " focused" : " unfocused");
	};
	const auto itemsSettle = [&] { iw.update(demo.iws); ui::frame(0); };
	const auto itemsClose = [&](uint64_t o)	// the host's answer to close(o)
	{
		std::erase_if(demo.iws.characters, [o](const ui::GearState& g) { return g.owner == o; });
		std::erase_if(demo.iws.containers, [o](const ui::ContainerState& c) { return c.owner == o; });
		itemsSettle();
	};
	if (demo.frame == 127 && demo.showItems)
	{
		ui::GearState body = demo.iws.characters[0];
		body.owner = 77;
		body.name = "Dust Bandit";
		body.nameForeign = true;
		body.info.clear();
		demo.iws.characters.push_back(body);
		demo.iws.target = 0;
		itemsFour = demo.iws;
	}
	if (demo.frame == 128 && demo.showItems)
	{
		const float s = ui::scale();
		check(iw.stack().size() == 4 && iw.top() == 77 && ui::focusedWidget() == iw.window(77),
			("items stack: four windows, the character window opened last on top with the keyboard (" + itemsStack() + ")").c_str());
		demo.itemsLog.clear();
		const ui::Rect tr = iw.window(502)->screen();
		const Pt empty = itemsFree(502, { { tr.x + 6 * s, tr.bottom() - 6 * s }, { tr.right() - 6 * s, tr.bottom() - 6 * s }, { tr.right() - 6 * s, tr.y + 44 * s } });
		check(empty.x >= 0 && click(empty.x, empty.y) && (itemsSettle(), iw.top() == 502), "items stack: a left click on a window's empty area raises it");
		ui::ItemGrid* box = &iw.container(503)->grid();
		std::vector<Pt> cells;
		for (int y = 2; y < 5; y++) { for (int x = 0; x < 6; x++) { cells.push_back(cellPt(box, x, y)); } }
		const Pt cell = itemsFree(503, cells);
		check(cell.x >= 0 && click(cell.x, cell.y) && (itemsSettle(), iw.top() == 503), "items stack: a left click on an empty grid cell raises its window");
		const ui::Rect cr = iw.window(101)->screen();
		std::vector<Pt> caption;
		for (float x = cr.x + 10 * s; x < cr.right() - 130 * s; x += 20 * s) { caption.push_back({ x, cr.y + 19 * s }); }
		const Pt cap = itemsFree(101, caption);
		check(cap.x >= 0 && click(cap.x, cap.y) && (itemsSettle(), iw.stack() == std::vector<uint64_t>{ 77, 502, 503, 101 }) && ui::focusedWidget() == iw.window(101)
			&& demo.itemsLog.empty(), ("items stack: a click on a caption raises it; the stack is the press order (" + demo.itemsLog + ")").c_str());
		iw.window(502)->pinButton().checked = false;	// frame 123 clicked its pin and the host never answered
		for (ui::ContainerState& c : demo.iws.containers) { c.pinned = c.owner == 503; }
		itemsSettle();
		bool taken = key(SAPP_KEYCODE_ESCAPE);
		const bool first = demo.itemsLog == "close 101;";
		itemsClose(101);
		const bool down = ui::focusedWidget() == iw.window(503);	// the keyboard goes to the next one down (pinned: Escape still reaches the stack)
		const std::string after = itemsStack();
		taken = key(SAPP_KEYCODE_ESCAPE) && taken;
		itemsClose(502);
		taken = key(SAPP_KEYCODE_ESCAPE) && taken;
		itemsClose(77);
		const bool passed = !key(SAPP_KEYCODE_ESCAPE);
		check(taken && first && down && demo.itemsLog == "close 101;close 502;close 77;" && passed,
			("items stack: Escape closes the topmost, then the next down, skipping the pinned box; only it left, Escape passes to the host (" + demo.itemsLog + "; " + after + ")").c_str());
		demo.iws.containers[0].pinned = false;
		itemsSettle();
		demo.itemsLog.clear();
		taken = key(SAPP_KEYCODE_ESCAPE);
		itemsClose(503);
		check(taken && demo.itemsLog == "close 503;" && !key(SAPP_KEYCODE_ESCAPE) && !ui::focusedWidget() && iw.stack().empty(),
			("items stack: unpinned it closes; with none open Escape is the host's (" + demo.itemsLog + ")").c_str());
		demo.iws = itemsFour;	// all four again
	}
	if (demo.frame == 129 && demo.showItems)
	{
		check(iw.stack() == std::vector<uint64_t>{ 502, 503, 101, 77 } && iw.top() == 77, "items stack: windows opened together stack in the state's order, containers first, each on top");
		// The screenshot: the docked container clicked up over the one cascaded from it.
		const uint64_t lower = iw.docked(), upper = lower == 502 ? 503 : 502;
		const ui::Rect lr = iw.window(lower)->screen(), ur = iw.window(upper)->screen();
		const float s = ui::scale();
		std::vector<Pt> spots;
		for (float y = lr.y + 44 * s; y < lr.bottom() - 6 * s; y += 20 * s) { for (float x = lr.x + 6 * s; x < lr.right() - 6 * s; x += 20 * s) { if (!iw.container(lower)->grid().screen().contains(x, y)) { spots.push_back({ x, y }); } } }	// no item lifted for the shot
		const Pt p = itemsFree(lower, spots);
		check(p.x >= 0 && lr.right() > ur.x && lr.bottom() > ur.y && click(p.x, p.y) && (itemsSettle(), iw.top() == lower),
			"items stack: the docked container, under the cascaded one, comes over it when clicked");
	}
	// The HUD's building panel: the status and medical panels with the building's contents, its buttons and ticks, back to the character.
	ui::Hud& hud = *demo.hud;
	const auto childText = [](const ui::Widget* w, size_t i) { return static_cast<const ui::Label*>(w->children()[i].get())->text; };
	if (demo.frame == 119 && demo.showHudBuilding) { demo.hs.building = hudBuildingCase(0); }
	if (demo.frame == 120 && demo.showHudBuilding)
	{
		check(hud.buildingShown() && hud.infoPanel()->visible && childText(hud.infoPlate(), 0) == "Ore Drill" && childText(hud.infoPlate(), 1) == "Wanderers" && !hud.infoButton()->visible
			&& static_cast<const ui::Label*>(hud.infoPlate()->children()[0].get())->colour == ui::res::colour("HudBuildingName", ui::style().textColour(ui::Look::kNamePlate, ui::State::kNormal, false)),
			"hud building: the plate holds the name and owner in the name ink, no extend button");
		check(hud.medicalPanel()->visible && hud.medicalStatus()->visible && childText(hud.medicalStatus(), 0) == "Output full", "hud building: the status box holds its state");
		check(hud.medicalFill(0) == 1 && hud.medicalRight(1)->visible && hud.medicalRight(1)->text == "Inactive" && hud.medicalFill(1) == 0 && hud.medicalBar(8)->parent()->visible
			&& static_cast<const ui::Label*>(hud.medicalValue(0))->text == "25", "hud building: Condition and Power bars, the other 7 of the 9 slats empty");
		ui::Color q;
		check(ui::res::colour("#00a31a") == (ui::Color::parse("#00a31a", q), q) && ui::res::colour("#00a31a") != ui::res::colour("HudGood"), "hud building: a colour may be the host's #rrggbb (the mine quality's ink)");
		const ui::Rect ext = hud.infoExtended()->screen(), st = hud.infoStats()->screen(), b0 = hud.buildingButton(0)->screen(), t1 = hud.buildingTick(1)->screen();
		check(hud.buildingButton(0)->text == "Dismantle" && !hud.buildingButton(1) && b0.y > ext.y && b0.bottom() < ext.bottom() && t1.y > st.y && t1.bottom() < st.bottom(),
			"hud building: Dismantle under the state rows, the ticks under the stats inside their panel");
		demo.hbLog.clear();
		clickOn(hud.buildingButton(0));
		check(demo.hbLog.empty() && hud.buildingPrompt() && hud.buildingPrompt()->shown(), "hud building: Dismantle asks first");
		key(SAPP_KEYCODE_ESCAPE);
		check(demo.hbLog.empty() && !hud.buildingPrompt()->shown(), "hud building: No (Escape) sends nothing");
		clickOn(hud.buildingButton(0));
		key(SAPP_KEYCODE_ENTER);
		clickOn(hud.buildingTick(0));
		check(demo.hbLog == "button 60 dismantle;tick 60 battery 1;", ("hud building: Yes (Enter) sends it; the ticks are the host's (" + demo.hbLog + ")").c_str());
	}
	if (demo.frame == 121 && demo.showHudBuilding)
	{
		check(hud.buildingTick(0)->checked && hud.buildingTick(1)->checked, "hud building: the next state ticks Battery");
		demo.hs.building = hudBuildingCase(1);
	}
	if (demo.frame == 122 && demo.showHudBuilding)
	{
		check(hud.buildingButton(0)->text == "[UPGRADE]" && hud.buildingButton(1)->text == "[DOWNGRADE]" && hud.buildingButton(2)->text == "Dismantle" && childText(hud.medicalStatus(), 0) == "No workers"
			&& hud.medicalRight(1)->text.empty() && hud.medicalFill(0) == 0.6f, "hud building: another building rebuilds the buttons, bars and status");
		demo.hs.building.valid = false;
	}
	if (demo.frame == 123 && demo.showHudBuilding)
	{
		check(!hud.buildingShown() && hud.infoButton()->visible && !hud.buildingButton(0)->parent()->visible && childText(hud.infoPlate(), 0) == demo.hs.info.name
			&& (!hud.medicalPanel()->visible || hud.medicalRight(0)->shadow), "hud building: deselected, the character's panels come back (the limb ink; Blood's right text is its bleed arrows)");
		demo.hs.building = hudBuildingCase(2);
	}
	if (demo.frame == 124 && demo.showHudBuilding)
	{
		check(hud.buildingTick(0)->text == "Power" && !hud.buildingTick(1) && childText(hud.medicalStatus(), 0) == "Out of fuel" && childText(hud.medicalBar(1), 0) == "Power Output"
			&& childText(hud.medicalBar(2), 0) == "Fuel Use", "hud building: a generator: the Power tick only, output and fuel bars");
		demo.hs.building = hudBuildingCase(3);
	}
	if (demo.frame == 125 && demo.showHudBuilding)
	{
		check(hud.buildingButton(0)->text == "PAUSE CONSTRUCTION" && hud.buildingButton(1)->text == "Dismantle" && !hud.buildingTick(0) && childText(hud.medicalStatus(), 0) == "Under construction"
			&& static_cast<const ui::Label*>(hud.medicalValue(1))->text == "3/10" && static_cast<const ui::Label*>(hud.medicalValue(2))->text == "0/4" && hud.medicalBar(3)->parent()->visible,
			"hud building: under construction: PAUSE CONSTRUCTION, the [PAUSED] tag, a have/need slat per material, no ticks");
		demo.hs.building = hudBuildingCase(0);	// the screenshot: the mine
	}
	// The status tag row: one branch of the original's chain, its text and inks, its hover text in the tip box.
	if (demo.frame == 119 && demo.showTag)
	{
		demo.hs.building.valid = false;
		hudTagCase(demo.showTag, demo.hs.info);
	}
	if (demo.frame == 120 && demo.showTag && kTagCases[demo.showTag] == std::string("playerleader"))
	{
		// k16.2.276.1: a player faction's squad leader (b0, army b1 too) carries no Leader / War Leader tag, an NPC's does; other bits stay.
		using CS = simlink::CharStats;
		const uint16_t lead = CS::kSquadLeader, war = CS::kSquadLeader | CS::kArmyCommander, kid = CS::kSquadLeader | CS::kCaptive;
		check(!(CS::shownTagFlags(lead, true) & CS::kSquadLeader), "hud tag playerleader: a player faction's squad leader shows no Leader tag");
		check(!(CS::shownTagFlags(war, true) & (CS::kSquadLeader | CS::kArmyCommander)), "hud tag playerleader: a player faction's army commander shows no War Leader tag");
		check((CS::shownTagFlags(lead, false) & CS::kSquadLeader) && (CS::shownTagFlags(war, false) & CS::kArmyCommander), "hud tag playerleader: an NPC leader keeps Leader / War Leader");
		check(CS::shownTagFlags(kid, true) == CS::kCaptive, "hud tag playerleader: the Kidnapped bit is left alone");
		check(!hud.infoTag()->shown() || hud.infoTagText().empty(), "hud tag playerleader: the row is empty for the player's member");
	}
	if (demo.frame == 120 && demo.showTag && kTagCases[demo.showTag] != std::string("playerleader"))
	{
		const ui::HudInfo& in = demo.hs.info;
		const ui::Color ink = ui::style().textColour(ui::Look::kField, ui::State::kNormal, false), textInk = ui::res::colour(in.tagColour, ink);
		const std::string name = std::string("hud tag ") + kTagCases[demo.showTag] + ": ";
		check(hud.infoTag()->shown() && hud.infoTagText() == kTagDrawn[demo.showTag], (name + "the row reads " + kTagDrawn[demo.showTag] + " (" + hud.infoTagText() + ")").c_str());
		check(hud.infoTagInk(false) == textInk && hud.infoTagInk(true) == (in.tagValueColour.empty() ? textInk : ui::res::colour(in.tagValueColour, ink)),
			(name + "the tag in " + in.tagColour + ", its value in " + (in.tagValueColour.empty() ? in.tagColour : in.tagValueColour)).c_str());
		const std::string& tip = hud.infoTagTip();
		check(!tip.empty() && tip.find(in.tagTip.front().label) != std::string::npos && (in.tagTip.size() < 2 || tip.find('\n') != std::string::npos),
			(name + "the hover text is the host's lines").c_str());
		const ui::Rect r = hud.infoTag()->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, r.x + r.w / 2, r.y + r.h / 2);
	}
	if (demo.frame == 126 && demo.showTag && kTagCases[demo.showTag] != std::string("playerleader"))
	{
		const std::string name = std::string("hud tag ") + kTagCases[demo.showTag] + ": ";
		const ui::Widget* box = hud.hintBox();
		check(!ui::fixedTips() || (hud.hintText() == hud.infoTagTip() && box->shown() && box->screen().bottom() > hud.infoTag()->screen().y
			&& box->children().front()->preferred().h * ui::scale() <= box->screen().h - 19 * ui::scale()),
			(name + "hovered, its text in the tip box, which grows to hold it").c_str());
	}
	if (demo.showTag == 6 && demo.frame == 120)
	{
		const std::string& tip = hud.infoTagTip();
		check(tip.find("Situation:\t{HudBad}Seen!") != std::string::npos && tip.find("Looks like a slave:\t64%") != std::string::npos
			&& tip.find("Time remaining:\t18 hours") != std::string::npos, "hud tag escapedslave: Situation Seen! in HudBad, Looks like a slave, Time remaining");
	}
	if (demo.showTag == 10 && demo.frame == 120)
	{
		const std::string& tip = hud.infoTagTip();
		check(tip.find("Sand Guild\t{HudNeutral}c.2,500") != std::string::npos && tip.find("Iron Court\t{HudFaint}c.800") != std::string::npos,
			"hud tag bounty: a line per faction, the local one in HudNeutral, others in HudFaint");
	}
	ui::Prospecting& pro = *demo.prospect;
	ui::AdvancedOptions& adv = *demo.advanced;
	if (demo.frame == 118 && demo.showAdvanced)
	{
		adv.show(false);	// the new game checks' ADVANCED click opened it
		ng.open([] { demo.advanced->show(false); demo.menuLog += "hidden;"; });	// the host's: the window goes with the panel
	}
	if (demo.frame == 119 && demo.showAdvanced)
	{
		demo.menuLog.clear();
		check(clickOn(ng.advancedButton()) && adv.shown() && ng.shown() && demo.menuLog == "advanced;", "advanced: ADVANCED OPTIONS opens the window");
		check(adv.slider(0) && adv.valueBox(0)->text == "1" && !adv.tick(0) && adv.tick(7)->checked && !adv.tick(8)->checked && !adv.slider(8), "advanced: slider and tick rows from the host");
		check(ui::focused(ng.window()), "advanced: the panel keeps the keys");
	}
	if (demo.frame == 120 && demo.showAdvanced)
	{
		const ui::Rect p = ng.window()->screen(), w = adv.window()->screen();
		check(std::abs(w.x - p.x) < 1 && std::abs(w.y - (p.bottom() - 2.5f * ui::scale())) < 1 && std::abs(p.x - sapp_widthf() / 2) < 1
			&& std::abs(p.y - 54 * ui::scale()) < 1, "advanced: below the panel, same left edge; the panel's left edge at the centre");
		demo.menuLog.clear();
		const ui::Rect s0 = adv.slider(0)->screen();
		drag({ s0.x + s0.w / 2, s0.y + s0.h / 2 }, { s0.right() + 60, s0.y + s0.h / 2 });
		check(adv.slider(0)->value == 8 && adv.valueBox(0)->text == "8" && demo.menuLog.size() >= 15 && demo.menuLog.ends_with("change ht 8;"), ("advanced: a drag to the end, max (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		const ui::Rect s1 = adv.slider(1)->screen();
		click(s1.x + 3 * ui::scale(), s1.y + s1.h / 2);
		check(demo.menuLog == "change cod 0.99;" && adv.valueBox(1)->text == "0.99", ("advanced: the left arrow steps 0.01 (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		clickOn(adv.valueBox(2));
		key(SAPP_KEYCODE_END);
		key(SAPP_KEYCODE_BACKSPACE);
		type("9");
		key(SAPP_KEYCODE_ENTER);
		check(demo.menuLog == "change gdm 4;" && adv.valueBox(2)->text == "4" && adv.slider(2)->value == 4, ("advanced: a typed value is clamped (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		clickOn(adv.valueBox(3));
		key(SAPP_KEYCODE_END);
		key(SAPP_KEYCODE_BACKSPACE);
		type("1.253");
		key(SAPP_KEYCODE_ENTER);
		check(demo.menuLog == "change ps 1.253;" && adv.valueBox(3)->text == "1.253", ("advanced: a typed value is not stepped (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		check(clickOn(adv.tick(7)) && demo.menuLog == "change bl 0;" && !adv.tick(7)->checked, ("advanced: a tick (" + demo.menuLog + ")").c_str());
		demo.menuLog.clear();
		const ui::Rect n = adv.name(1)->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, n.x + 10, n.y + n.h / 2);
	}
	if (demo.frame == 121 && demo.showAdvanced)
	{
		check(adv.tipText()->text == demo.adv[1].tip, "advanced: the hovered name's tip");
		const ui::Rect n = adv.name(8)->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, n.x + 10, n.y + n.h / 2);
	}
	if (demo.frame == 122 && demo.showAdvanced)
	{
		check(adv.tipText()->text == demo.adv[1].tip, "advanced: a row without a tip keeps the last");
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 5, 5);
		check(clickOn(ng.advancedButton()) && !adv.shown() && clickOn(ng.advancedButton()) && adv.shown(), "advanced: the button toggles it");
		demo.menuLog.clear();
		check(clickOn(ng.cancelButton()) && !ng.shown() && !adv.shown() && demo.menuLog == "hidden;cancel;", ("advanced: CANCEL closes both (" + demo.menuLog + ")").c_str());
		ng.open([] { demo.advanced->show(false); });
		check(!adv.shown() && adv.valueBox(0)->text == "8", "advanced: NEW GAME again: the window closed, the values kept");
		clickOn(ng.advancedButton());	// what the screenshot shows
		const ui::Rect n = adv.name(6)->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, n.x + 10, n.y + n.h / 2);
	}
	// The debug panel (user 2026-11-05: the key list (was Ctrl+F1, now F8) as a window, the debug toggles as tick boxes, the scenes as buttons).
	// The playtest windows (client coordinator 2026-11-07; docs/reference/PLAYTEST.md).
	if (demo.showScenarios && demo.frame == 119) { demo.scs.open = true; }
	if (demo.showScenarios && demo.frame >= 119) { demo.scen->update(demo.scs); }
	if (demo.showScenarios && demo.frame == 121)
	{
		ui::ScenariosWindow& sw = *demo.scen;
		auto sync = [&] { sw.update(demo.scs); ui::frame(0); };
		check(sw.layout().ok() && sw.shown() && sw.window()->resizable && sw.window()->caption().text == "SCENARIOS", ("scenarios: from scenarios.layout, open by the host's state (" + sw.layout().error() + ")").c_str());
		check(sw.list()->rows().size() == 3 && sw.list()->selectedId() == "bar_lure" && sw.list()->rows()[0].cells[2] == "2026-11-07 15:30" && sw.list()->rows()[1].cells[2] == "never",
			"scenarios: a table row per scenario (name, setup, last run, verdict), the host's selected marked");
		check(sw.title()->text == "Lure into the bar" && sw.lookFor()->text == demo.scs.scenarios[0].lookFor && sw.caveat()->shown() && sw.caveat()->text.find("evidence, not proof") != std::string::npos,
			"scenarios: the title, LOOK FOR and the evidence-not-proof line");
		check(sw.versionBlocks() == 2 && sw.versionHead(0)->text.rfind("v2", 0) == 0 && sw.versionHead(1)->text.rfind("v1", 0) == 0, "scenarios: a block per version, in the host's order");
		const std::vector<std::string> names = { "targeted", "crew joined (s)", "bodies down", "fight (s)", "player downed (s)" };
		check(sw.metricNames() == names, "scenarios: a metric column per name, first seen first");
		{
			const auto* a = sw.runCells(0, 0);
			const auto* b = sw.runCells(0, 1);
			const auto* c = sw.runCells(1, 0);
			check(a && b && c && !sw.runCells(1, 1) && a->size() == 4 + names.size() + 2, "scenarios: a row per run, its cells");
			check(a && (*a)[1]->text == "! 7c1e09a" && (*a)[1]->colour == ui::res::colour("Bad") && b && (*b)[1]->text == "4b2d11f",
				"scenarios: a run built from other hashes flagged ! in Bad");
			check(a && (*a)[4 + 4]->text == "-" && (*a)[4 + 1]->text == "2.4" && c && (*c)[4 + 1]->text == "-", "scenarios: metric values, - when not reported");
			check(a && (*a)[4 + 5]->text == "off" && (*a)[4 + 6]->text == demo.scs.scenarios[0].versions[0].runs[0].firstNote, "scenarios: the verdict and the first note");
		}
		demo.playLog.clear();
		sw.list()->select("beak_open_ground");	// as a click does
		sync();
		check(demo.playLog == "select beak_open_ground;" && sw.versionBlocks() == 0 && sw.title()->text == "Beaks in the open" && sw.playButton()->enabled,
			("scenarios: a pick asks the host; no versions yet (" + demo.playLog + ")").c_str());
		demo.playLog.clear();
		check(clickOn(sw.playButton()) && demo.playLog == "play beak_open_ground;" && (sync(), sw.status()->text == "loading beak_open_ground..."), "scenarios: PLAY asks for the scenario; the host's status line");
		demo.scs.selected = "slavers_downed";
		sync();
		check(sw.versionBlocks() == 1 && sw.list()->selectedId() == "slavers_downed", "scenarios: the host's pick follows; a version without runs");
		demo.scs.selected.clear();
		sync();
		check(!sw.playButton()->enabled && sw.versionBlocks() == 0, "scenarios: nothing picked: PLAY greyed");
		{
			// k16.3.14: a recipe playtest (no fixture, steps from the title) and a bead id. Our own stand-in wording.
			check(!sw.recipe()->shown() && !sw.bead()->shown(), "scenarios: no RECIPE block or bead line without them");
			ui::PlaytestScenario rec{ "title_start", "Start from the title", "", "Does the start put you under your own control on day 1?", "", "", {} };
			rec.recipe = "From the checkout root run the title mode, pick NEW GAME, choose the first start and BEGIN; no fixture, the world as a new game makes it.";
			rec.bead = "kenshi-h2v.1";
			demo.scs.scenarios.push_back(rec);
			demo.scs.selected = "title_start";
			sync();
			const ui::Table::Row* row = nullptr;
			for (const ui::Table::Row& r : sw.list()->rows()) { if (r.id == "title_start") { row = &r; } }
			check(row && row->cells[1] == "Recipe" && row->tip == "Start from the title: " + rec.recipe, "scenarios: a recipe scenario's Setup reads Recipe, its tip the steps");
			ui::frame(0);
			const ui::Rect rt = sw.recipe()->screen(), lf = sw.lookFor()->screen(), ti = sw.title()->screen(), bd = sw.bead()->screen();
			check(sw.recipe()->shown() && sw.recipe()->text == rec.recipe && sw.lookFor()->text == rec.lookFor && rt.y > ti.bottom() && lf.y > rt.bottom() && rt.h > 0 && sw.recipe()->wrap,
				("scenarios: the RECIPE block (wrapping) between the title and LOOK FOR (title " + std::to_string(ti.y) + ", recipe " + std::to_string(rt.y) + "+"
				+ std::to_string(rt.h) + ", look for " + std::to_string(lf.y) + ")").c_str());
			check(sw.bead()->shown() && sw.bead()->text == "Bead: kenshi-h2v.1" && bd.y >= ti.bottom() && bd.bottom() <= rt.y, "scenarios: the bead line under the title");
			demo.scs.selected = "bar_lure";
			sync();
			check(!sw.recipe()->shown() && !sw.bead()->shown(), "scenarios: picking a fixture scenario hides them again");
			demo.scs.scenarios.pop_back();
			sync();
		}
		demo.scs.selected = "bar_lure";
		sync();
		demo.playLog.clear();
		ui::focus(sw.window());
		check(key(SAPP_KEYCODE_ESCAPE) && !sw.shown() && demo.playLog == "close;", "scenarios: Escape closes it at once and takes the key (no game menu)");
		demo.scs.open = true;
		sync();
		check(sw.shown(), "scenarios: the host's open shows it again");
		// The menus' SCENARIOS buttons: whenever the host sets the action (it does with --debug or --playtest; kenshi-k16.2.110.1).
		demo.playLog.clear();
		demo.mainMenu->show(true);
		ui::frame(0);
		check(demo.mainMenu->scenariosButton()->shown() && clickOn(demo.mainMenu->scenariosButton()) && demo.playLog == "menu scenarios;", "scenarios: the title's SCENARIOS button (debug)");
		demo.mainMenu->show(false);
		ui::keys::setDebug(false);
		demo.mainMenu->show(true);
		const bool mainShown = demo.mainMenu->scenariosButton()->visible;
		demo.mainMenu->show(false);
		demo.gameMenu->show(true);
		const bool pauseShown = demo.gameMenu->scenariosButton()->visible && demo.gameMenu->window()->rect.h == 394;
		demo.gameMenu->show(false);
		ui::keys::setDebug(true);
		demo.playLog.clear();
		demo.gameMenu->show(true);
		ui::frame(0);
		check(mainShown && pauseShown && demo.gameMenu->scenariosButton()->shown() && clickOn(demo.gameMenu->scenariosButton()) && demo.playLog == "pause scenarios;",
			"scenarios: the pause menu's SCENARIOS button; both show without --debug when the host sets the action");
		demo.gameMenu->show(false);
		{
			ui::MainMenu bareMain;	// no scenarios action, under debug: no button
			ui::GameMenu bareGame;
			bareMain.show(true);
			bareGame.show(true);
			check(ui::keys::debug() && !bareMain.scenariosButton()->visible && !bareGame.scenariosButton()->visible && bareGame.window()->rect.h == 344,
				"scenarios: menus without the action hide SCENARIOS even under --debug");
			bareGame.show(false);
			bareMain.show(false);
		}
		demo.scen->update(demo.scs);
		demo.scen->window()->raise();
	}
	if (demo.showSpectator && demo.frame == 119)	// stand-ins (our own names): 40 characters of four factions, every seventh down
	{
		const char* const first[] = { "Ash", "Brin", "Corra", "Dask", "Elm", "Fenn", "Gall", "Hesk", "Ivo", "Jun" };
		const char* const factions[] = { "Dune Traders", "Rust Hounds", "Saltmarsh Folk", "Grey Watch" };
		demo.hs.cast.clear();
		for (int i = 0; i < 40; i++) { demo.hs.cast.push_back({ (uint64_t)(1000 + i * 7), std::string(first[i % 10]) + " " + std::to_string(i / 10 + 1), factions[i % 4], i % 7 == 3 }); }
		demo.hs.cast[5].name.clear();	// no name: "#1035"
		demo.hs.spectating = true;
		demo.hs.watch = { 1014, "Corra 1", "Saltmarsh Folk", "Guard the gate until dusk", "", false };
		demo.hs.paused = true;	// PAUSED moves under the Watching panel
	}
	if (demo.showSpectator && demo.frame == 121)
	{
		ui::Hud& h = *demo.hud;
		auto sync = [&] { h.update(demo.hs); ui::frame(0); };
		ui::CastWindow* cw = h.castWindow();
		check(!h.squadBar()->shown() && !h.toggleRow()->shown() && !h.ordersPanel()->shown() && !h.moneyBox()->shown() && !h.buildPanel()->shown()
			&& !h.moveSpeedDiamond()->shown() && !h.shortcutButton(0)->shown() && !h.shortcutButton(4)->shown(), "spectator: no squad bar, ticks, orders, money, build, move speed, INV or SQD");
		check(h.timePanel()->shown() && h.shortcutButton(2)->shown() && h.infoPanel()->shown() && h.floorDiamond()->shown(), "spectator: the clock, speed, MAP, floor and status panel stay");
		check(h.watchPanel()->shown() && h.watchText() == "Watching: Corra 1   Saltmarsh Folk" && h.watchGoalText() == "Goal: Guard the gate until dusk   Task: -",
			("spectator: the Watching panel: who, the goal, an empty task as - (" + h.watchText() + " | " + h.watchGoalText() + ")").c_str());
		check(h.pausedPanel()->shown() && h.pausedPanel()->screen().y >= h.watchPanel()->screen().bottom(), "spectator: PAUSED sits under the Watching panel");
		check(cw && cw->layout().ok() && cw->shown() && cw->window()->caption().text == "CAST" && cw->table()->rows().size() == 40 && cw->countText() == "40 characters",
			("spectator: CAST opens on its own, from cast.layout, a row per character (" + (cw ? cw->layout().error() + cw->countText() : std::string("none")) + ")").c_str());
		check(cw && cw->table()->rows()[0].cells[0] == "#1035" && cw->table()->rows()[1].cells[0] == "Ash 1" && cw->table()->selectedId() == "1014",
			"spectator: sorted by name (no name: #id), the watched one marked");
		check(cw && cw->table()->rows()[0].colour.a == 0 && [&] { for (const auto& r : cw->table()->rows()) { if (r.id == "1021") { return r.colour.a > 0; } } return false; }(),
			"spectator: a downed character dimmed");
		if (cw)
		{
			cw->filter()->text = "rust";
			cw->filter()->onChange("rust");
			check(cw->table()->rows().size() == 10 && cw->countText() == "10 of 40 characters", ("spectator: the filter keeps faction matches (" + cw->countText() + ")").c_str());
			cw->filter()->text = "ash";
			cw->filter()->onChange("ash");
			check(cw->table()->rows().size() == 4 && cw->countText() == "4 of 40 characters", ("spectator: and name matches, any case (" + cw->countText() + ")").c_str());
			cw->filter()->text.clear();
			cw->filter()->onChange("");
			ui::frame(0);
			ui::Table& t = *cw->table();
			const float u = ui::scale();
			const ui::Rect& r = t.screen();
			const float y = r.y + (t.headerH + t.rowH * 1.5f) * u;	// row 1: Ash 1 (1000)
			demo.specLog.clear();
			check(click(r.x + r.w / 2, y) && demo.specLog == "watch 1000;" && (sync(), h.watchText() == "Watching: Ash 1   Dune Traders" && t.selectedId() == "1000"),
				("spectator: a click inspects that character (" + demo.specLog + ")").c_str());
			demo.specLog.clear();
			check(click(r.x + r.w / 2, y) && demo.specLog == "follow 1000;" && (sync(), h.watchText().find("(following)") != std::string::npos && h.followButton()->text == "STOP"),
				("spectator: a double click follows it; STOP shows (" + demo.specLog + ")").c_str());
			demo.specLog.clear();
			check(clickOn(h.followButton()) && demo.specLog == "follow 0;" && (sync(), h.followButton()->text == "FOLLOW"), ("spectator: STOP stops following (" + demo.specLog + ")").c_str());
			demo.specLog.clear();
			check(clickOn(h.followButton()) && demo.specLog == "follow 1000;", ("spectator: FOLLOW follows the watched one (" + demo.specLog + ")").c_str());
			demo.hs.watch.following = false;
			sync();
			ui::focus(cw->table());
			check(key(SAPP_KEYCODE_ESCAPE) && !cw->shown(), "spectator: Escape closes CAST and takes the key (no game menu)");
			sync();
			check(!cw->shown() && clickOn(h.castButton()) && (sync(), cw->shown()), "spectator: closed stays closed; CAST opens it again");
		}
		check(ui::keys::label("debug.follow") == "Ctrl+C", "spectator: Ctrl+C follows (debug)");
		check(cw && cw->window()->screen().x < ui::root().screen().w / 4, "spectator: CAST sits at the left edge (the docked notes take the right)");
		check(!h.notesButton()->shown(), "notes button: hidden without a playtest run");
		demo.hs.playtestRun = true;
		demo.hs.notesOpen = true;
		sync();
		demo.specLog.clear();
		check(h.notesButton()->shown() && h.notesButton()->checked && h.notesButton()->tooltip == "Playtest notes (Ctrl+N)" && clickOn(h.notesButton()) && demo.specLog == "notes;",
			("notes button: shown during a run, checked while open, names Ctrl+N; a click asks the host (" + demo.specLog + ")").c_str());
		{
			const ui::Rect& n = h.notesButton()->parent()->screen();
			const ui::Rect& sc = h.shortcutsPanel()->screen();
			check(n.right() <= sc.x && std::fabs(n.bottom() - ui::root().screen().bottom()) < 2, "notes button: left of the shortcuts, on the screen's foot");
		}
		demo.hs.watch = {};
		sync();
		check(h.watchText() == "Watching: nobody" && !h.followButton()->enabled, "spectator: nobody watched: FOLLOW greyed");
		demo.hs.spectating = false;
		sync();
		check(!h.watchPanel()->shown() && !cw->shown() && h.squadBar()->shown() && h.ordersPanel()->shown() && h.moneyBox()->shown() && h.shortcutButton(0)->shown()
			&& h.moveSpeedDiamond()->shown(), "spectator: off again: the normal HUD back, CAST gone");
		demo.hs.spectating = true;
		demo.hs.watch = { 1014, "Corra 1", "Saltmarsh Folk", "Guard the gate until dusk", "", false };
		sync();
		check(cw->shown(), "spectator: on again: CAST opens on its own");
	}
	if (demo.showNotes && demo.frame == 119) { demo.ns.open = true; }
	if (demo.showNotes && demo.frame >= 119) { demo.notes->update(demo.ns); }
	if (demo.showNotes && demo.frame == 121)
	{
		ui::NotesWindow& nw = *demo.notes;
		auto sync = [&] { nw.update(demo.ns); ui::frame(0); };
		check(nw.layout().ok() && nw.shown() && nw.window()->caption().text == "NOTES  bar_lure v2" && !ui::focused(nw.entry()) && demo.playLog.empty(),
			("notes: from notes.layout; the host's open leaves the keyboard to the game: not typing (" + demo.playLog + nw.layout().error() + ")").c_str());
		{
			const ui::Rect& w = nw.window()->screen();
			const float u = ui::scale();
			check(std::fabs(ui::root().screen().right() - w.right() - 8 * u) < 2 && std::fabs(w.w - 420 * u) < 2 && std::fabs(w.h - 340 * u) < 2,
				"notes: docked small at the right edge (420 x 340)");
			check(!key(SAPP_KEYCODE_W), "notes: open, a game key isn't taken");
			ui::focus(nullptr);
			check(click(w.x + w.w / 3, w.y + 10 * u) && (sync(), ui::focused(nw.entry())) && demo.playLog == "typing 1;",
				("notes: a click on its header lands in the box: typing (" + demo.playLog + ")").c_str());
		}
		check(nw.noteRows() == 2 && nw.noteText(0)->text == "1. the raider slid sideways at the door" && nw.noteTriage(0) && nw.noteTriage(0)->colour.a < nw.noteText(0)->colour.a
			&& nw.noteTriage(0)->screen().x > nw.noteText(0)->screen().x && !nw.noteTriage(1), "notes: numbered notes, the triage under, dimmed and indented; none yet: no line");
		check(nw.noteText(1)->screen().h > 1.5f * nw.noteText(1)->font->lineHeight(), "notes: a long note wraps (never clipped)");
		check(nw.button("off")->checked && !nw.button("good")->checked, "notes: the host's verdict marked");
		demo.playLog.clear();
		check(clickOn(nw.button("good")) && demo.playLog.find("verdict good;") != std::string::npos && (sync(), nw.button("good")->checked && !nw.button("off")->checked),
			("notes: a verdict asks the host (" + demo.playLog + ")").c_str());
		ui::focus(nw.entry());
		sync();
		demo.playLog.clear();
		nw.entry()->text = "it froze for a moment";
		check(key(SAPP_KEYCODE_ENTER) && demo.playLog == "add it froze for a moment;" && nw.entry()->text.empty() && ui::focused(nw.entry()) && (sync(), nw.noteRows() == 3)
			&& nw.noteText(2)->text == "3. it froze for a moment", ("notes: Enter adds the note, clears the box, keeps the focus (" + demo.playLog + ")").c_str());
		demo.playLog.clear();
		nw.entry()->text = "   ";
		key(SAPP_KEYCODE_ENTER);
		check(demo.playLog.empty() && (ui::focus(nw.entry()), true), "notes: a blank note isn't added");
		nw.entry()->text = "stuck on the stairs";
		demo.playLog.clear();
		check(clickOn(nw.button("mark")) && demo.playLog.find("mark;") != std::string::npos && nw.entry()->text == "00:42.3 Wanderer (120, -40): stuck on the stairs" && ui::focused(nw.entry()),
			("notes: MARK puts the stamp before the text, the box focused (" + nw.entry()->text + ")").c_str());
		sync();
		demo.playLog.clear();
		check(key(SAPP_KEYCODE_ESCAPE) && nw.shown() && !ui::focusedWidget() && nw.entry()->text == "00:42.3 Wanderer (120, -40): stuck on the stairs" && (sync(), demo.playLog == "typing 0;")
			&& nw.shown() && !ui::focusedWidget(), ("notes: Escape in the box keeps the draft, the window stays, the keyboard back to the game (" + demo.playLog + ")").c_str());
		demo.playLog.clear();
		check(!key(SAPP_KEYCODE_ESCAPE) && nw.shown() && demo.playLog.empty(), "notes: a second Escape is the game's (the game menu), the window kept");
		check(clickOn(&nw.window()->closeButton()) && !nw.shown() && demo.playLog == "close;", ("notes: the X closes it (" + demo.playLog + ")").c_str());
		demo.playLog.clear();
		nw.focusEntry();
		demo.ns.open = true;
		sync();
		check(nw.shown() && ui::focused(nw.entry()) && demo.playLog == "typing 1;", ("notes: focusEntry (Ctrl+N) opens it with the box focused (" + demo.playLog + ")").c_str());
		{
			sapp_event e = {};
			e.type = SAPP_EVENTTYPE_KEY_DOWN;
			e.key_code = SAPP_KEYCODE_N;
			e.modifiers = SAPP_MODIFIER_CTRL;
			const std::string before = nw.entry()->text;
			check(!ui::event(e) && ui::keys::pressed(e, "playtest.notes") && ui::focused(nw.entry()) && nw.entry()->text == before,
				"notes: Ctrl+N in the box isn't taken: it goes on to the host (its rule closes the window while typing)");
		}
		ui::focus(nw.window());
		demo.playLog.clear();
		check(key(SAPP_KEYCODE_ESCAPE) && !nw.shown() && demo.playLog.find("close;") != std::string::npos, "notes: Escape on the window itself closes it and takes the key");
		demo.ns.open = true;
		sync();
		nw.focusEntry();
		sync();
		demo.playLog.clear();
		demo.ns.open = false;
		sync();
		check(!nw.shown() && demo.playLog == "typing 0;", "notes: the host's open closes it; typing off");
		demo.ns.open = true;
		sync();
		// Not posted to the --bead item (kenshi-k16.2.130): why, under the note in TipBad; the run's warning above the box.
		ui::Label* post = nw.notePost(1);
		check(!nw.notePost(0) && post && post->text == "not on kenshi-h2v.2.1: bd not found on PATH" && post->colour.a < nw.noteText(1)->colour.a
			&& post->screen().x > nw.noteText(1)->screen().x && post->screen().y >= nw.noteText(1)->screen().bottom() - 1,
			"notes: a note not posted carries why, under it, indented and dimmer; a posted one has no line");
		ui::Label* warn = nw.warning();
		const ui::Rect& ww = nw.window()->screen();
		check(warn && warn->shown() && warn->text == "bd unreachable: the notes stay in the run file" && warn->screen().h > 0
			&& warn->screen().bottom() <= nw.entry()->screen().y + 1 && warn->screen().y >= nw.list()->screen().bottom() - 1 && ww.contains(warn->screen().x + 1, warn->screen().y + 1),
			"notes: the run's warning is a line between the notes and the box");
		const float listH = nw.list()->screen().h;
		const std::string keep = demo.ns.warning;
		demo.ns.warning.clear();
		sync();
		check(!warn->shown() && nw.list()->screen().h > listH, "notes: no warning: the line hides and the notes take the room back");
		demo.ns.warning = keep;
		sync();
	}
	// The animation browser (client coordinator 2026-11-07; CLIENT_GOALS "Animation lab").
	if (demo.showAnim && demo.frame == 119)
	{
		demo.as.view.tex = ui::icons::get(ui::icons::cached("575-gamedata.base", "2146-gamedata.base")).tex;	// a stand-in texture (the straw hat's icon)
		demo.anim->show(true);
	}
	if (demo.showAnim && demo.frame >= 119) { demo.anim->update(demo.as); }
	if (demo.showAnim && demo.frame == 121)
	{
		ui::AnimBrowser& ab = *demo.anim;
		auto sync = [&] { ab.update(demo.as); ui::frame(0); };
		check(ab.layout().ok() && ab.shown() && ab.window()->resizable && ab.window()->caption().text == "ANIMATIONS",
			("anim: from animbrowser.layout, shown, resizable (" + ab.layout().error() + ")").c_str());
		const ui::Rect vr = ab.view()->screen();
		check(demo.animW == (int)std::lround(vr.w) && demo.animH == (int)std::lround(vr.h) && vr.w > 300,
			("anim: the render box's size sent to the host (" + std::to_string(demo.animW) + " x " + std::to_string(demo.animH) + ")").c_str());
		check(ab.filtered() == 600 && ab.clipRow(399) && !ab.clipRow(400) && ab.moreLine() && static_cast<ui::Label*>(ab.moreLine())->text == "200 more: narrow the filter"
			&& ab.count()->text == "600 of 600", "anim: at most 400 clip rows, then a narrow-the-filter line; N of M");
		check(ab.clipRow(1)->checked && !ab.clipRow(0)->checked, "anim: the current clip's row marked");
		ab.filter()->text = "RUN";
		ab.filter()->onChange("RUN");
		sync();
		check(ab.filtered() == 200 && !ab.moreLine() && ab.count()->text == "200 of 600" && ab.clipRow(0)->text == "run_left_001",
			("anim: the filter keeps the names that hold it, any case (" + ab.count()->text + ")").c_str());
		demo.animLog.clear();
		check(clickOn(ab.clipRow(2)) && demo.animLog == "select run_left_007;", ("anim: a row click selects its clip (" + demo.animLog + ")").c_str());
		sync();
		check(ab.clipRow(2)->checked && !ab.clipRow(0)->checked, "anim: the host's clip marked");
		demo.as.clip = "run_fwd_598";	// the last match
		sync();
		demo.animLog.clear();
		clickOn(ab.button("next"));
		sync();
		clickOn(ab.button("prev"));
		sync();
		clickOn(ab.button("prev"));
		check(demo.animLog == "select run_left_001;select run_fwd_598;select run_left_595;", ("anim: NEXT / PREV walk the filtered list, wrapping (" + demo.animLog + ")").c_str());
		sync();
		{
			const ui::Rect rr = ab.clipRow(198)->screen(), lv = ab.clipList()->screen();
			check(rr.y >= lv.y - 0.5f && rr.bottom() <= lv.bottom() + 0.5f, "anim: the new current clip scrolled into view");
		}
		auto pickIn = [&](ui::Dropdown& d, int i)
		{
			if (!clickOn(&d) || !ui::popupsOpen() || !d.menu()) { return false; }
			const ui::Rect c = d.menu()->content();
			for (float y = c.y; y < c.bottom(); y += 1) { if (d.menu()->itemAt(y) == i) { return click(c.x + c.w / 2, y + 4); } }
			return false;
		};
		demo.animLog.clear();
		check(ab.raceBox()->selected == 0 && pickIn(*ab.raceBox(), 2) && !ui::popupsOpen() && demo.animLog == "race Insect folk;", ("anim: the race dropdown asks for a race (" + demo.animLog + ")").c_str());
		sync();
		check(ab.raceBox()->selected == 2, "anim: the host's race shown");
		demo.animLog.clear();
		check(clickOn(ab.femaleBox()) && demo.animLog == "female on;" && (sync(), ab.femaleBox()->checked), "anim: Female asks, the host's state ticks it");
		check(ab.button("lheel")->checked && !ab.button("none")->checked && ab.boneBox()->selected == -1, "anim: the plant point marked, the bone dropdown empty");
		demo.animLog.clear();
		check(pickIn(*ab.boneBox(), 6) && demo.animLog == "plant l_foot;", ("anim: the bone dropdown plants a bone (" + demo.animLog + ")").c_str());
		sync();
		check(ab.boneBox()->selected == 6 && !ab.button("lheel")->checked && !ab.button("none")->checked, "anim: a planted bone shows in the dropdown, no point marked");
		demo.animLog.clear();
		clickOn(ab.button("rball"));
		sync();
		clickOn(ab.button("none"));
		sync();
		check(demo.animLog == "plant rball;plant ;" && ab.button("none")->checked && !ab.button("rball")->checked, ("anim: the point buttons and NONE plant (" + demo.animLog + ")").c_str());
		demo.animLog.clear();
		clickOn(ab.button("top"));
		sync();
		check(demo.animLog == "camera top;" && ab.button("top")->checked && !ab.button("side")->checked, "anim: a camera button asks, the host's camera marked");
		demo.animLog.clear();
		check(ab.overlayBox("trails")->checked && clickOn(ab.overlayBox("trails")) && demo.animLog == "overlay trails off;" && (sync(), !ab.overlayBox("trails")->checked),
			"anim: an overlay tick asks the host, its state ticks it");
		demo.animLog.clear();
		clickOn(ab.button("play"));
		sync();
		check(demo.animLog == "play;" && ab.button("play")->text == "PAUSE", "anim: PLAY asks to play, then reads PAUSE");
		demo.animLog.clear();
		clickOn(ab.button("stepback"));
		clickOn(ab.button("stepfwd"));
		clickOn(ab.button("measure"));
		check(demo.animLog == "step -1;step 1;measure;", ("anim: the step buttons and MEASURE (" + demo.animLog + ")").c_str());
		demo.animLog.clear();
		ui::focus(ab.window());
		key(SAPP_KEYCODE_SPACE);
		key(SAPP_KEYCODE_COMMA);
		key(SAPP_KEYCODE_PERIOD);
		key(SAPP_KEYCODE_M);
		check(demo.animLog == "pause;step -1;step 1;mark;", ("anim: keys Space, comma, period, M (" + demo.animLog + ")").c_str());
		sync();
		check(ab.reply()->visible && ab.reply()->text == "marked run_left_595 f4", "anim: the host's reply line");
		demo.animLog.clear();
		ui::focus(ab.filter());
		key(SAPP_KEYCODE_SPACE);
		check(demo.animLog.empty(), "anim: Space in the filter doesn't play");
		ui::focus(ab.window());
		demo.animLog.clear();
		ab.speedSlider()->setValue(0.5);
		sync();
		check(demo.animLog == "speed 0.50;" && ab.speedText()->text == "0.50x" && ab.speedSlider()->notches.size() == 1, ("anim: the speed slider (" + demo.animLog + ")").c_str());
		demo.animLog.clear();
		ab.scrub()->setValue(18);
		check(ab.scrub()->max == 36 && demo.animLog == "seek 0.60;", ("anim: the scrub seeks by frame (" + demo.animLog + ")").c_str());
		demo.animLog.clear();
		{
			const ui::Rect v = ab.view()->screen();
			const float cx = v.x + v.w / 2, cy = v.y + v.h / 2;
			send(SAPP_EVENTTYPE_MOUSE_MOVE, cx, cy);
			send(SAPP_EVENTTYPE_MOUSE_DOWN, cx, cy);
			send(SAPP_EVENTTYPE_MOUSE_MOVE, cx + 30, cy + 12);
			send(SAPP_EVENTTYPE_MOUSE_UP, cx + 30, cy + 12);
			send(SAPP_EVENTTYPE_MOUSE_SCROLL, cx, cy, 1);
		}
		check(demo.animLog == "orbit +;zoom in;", ("anim: a drag on the render orbits, the wheel zooms (" + demo.animLog + ")").c_str());
		demo.animLog.clear();
		check(clickOn(ab.button("copy")) && demo.animCopy == demo.as.readout && ab.readout()->screen().h > 3 * ab.readout()->font->lineHeight(),
			"anim: the readout's lines shown, COPY gives them");
		{
			ui::CaptionWindow& w = *ab.window();
			const int before = demo.animW;
			w.sizeW = 120;
			ui::frame(0);
			ab.update(demo.as);
			check(demo.animW == before + (int)std::lround(120 * ui::scale()), ("anim: a resize sends the render box's new size (" + std::to_string(demo.animW) + ")").c_str());
			w.sizeW = 0;
			ui::frame(0);
			ab.update(demo.as);
		}
		// Groups and the root lock (client coordinator 2026-11-07).
		ab.filter()->text.clear();
		ab.filter()->onChange("");
		sync();
		check(ab.groupBox()->items.size() == 5 && ab.groupBox()->items[0] == "All" && ab.groupBox()->selected == 0 && ab.filtered() == 600, "anim: the group dropdown, All first and picked");
		demo.animLog.clear();
		size_t idle = 0;
		for (const ui::AnimClip& c : demo.as.clips) { if (std::find(c.groups.begin(), c.groups.end(), "Idle") != c.groups.end()) { idle++; } }
		check(pickIn(*ab.groupBox(), 3) && demo.animLog == "group Idle;" && (sync(), ab.filtered() == idle) && ab.groupBox()->selected == 3
			&& ab.count()->text == std::to_string(idle) + " of 600", ("anim: a group picked keeps its clips (" + demo.animLog + ab.count()->text + ")").c_str());
		{
			const ui::Label* g = ab.clipRow(0)->children().empty() ? nullptr : dynamic_cast<const ui::Label*>(ab.clipRow(0)->children().front().get());
			check(g && g->text == "Idle" && g->screen().right() <= ab.clipRow(0)->screen().right() && g->screen().x > ab.clipRow(0)->screen().x + ab.clipRow(0)->screen().w / 2,
				"anim: a row's groups, faint at its right");
		}
		ab.filter()->text = "_0";	// the first hundred
		ab.filter()->onChange("_0");
		sync();
		check(ab.filtered() > 0 && ab.filtered() < idle, "anim: the group and the name filter both apply");
		pickIn(*ab.groupBox(), 4);
		sync();
		check(ab.filtered() == 0 && ab.emptyLine() && ab.emptyLine()->text == "no clips in this group for Insect folk", "anim: an empty group says so for the race");
		pickIn(*ab.groupBox(), 0);
		ab.filter()->text = "zzz";
		ab.filter()->onChange("zzz");
		sync();
		check(demo.as.group.empty() && ab.filtered() == 0 && ab.emptyLine() && ab.emptyLine()->text == "no clips match the filter", "anim: All, and a name that matches nothing, says so");
		ab.filter()->text.clear();
		ab.filter()->onChange("");
		demo.as.group = "Bogus";
		sync();
		check(ab.groupBox()->selected == -1 && ab.filtered() == 0, "anim: an unknown group shows none picked and lists nothing");
		demo.as.group.clear();
		demo.as.clip = demo.as.clips[3].name;	// usedBy, not locked
		demo.as.root = "travels 50.6 u per loop (unlocked)";
		sync();
		check(ab.filtered() == 600 && ab.followBox()->enabled && !ab.followBox()->checked && ab.usedBy()->visible && ab.usedBy()->text == demo.as.clips[3].usedBy
			&& ab.rootText()->visible && ab.rootText()->text == demo.as.root, "anim: the clip's used-by line and the root line shown; Follow root free");
		demo.animLog.clear();
		check(clickOn(ab.followBox()) && demo.animLog == "follow on;" && (sync(), ab.followBox()->checked), "anim: Follow root asks, the host's state ticks it");
		demo.as.clip = demo.as.clips[5].name;	// relocates: the game locks it
		demo.as.root.clear();
		sync();
		demo.animLog.clear();
		clickOn(ab.followBox());
		check(!ab.followBox()->enabled && demo.animLog.empty(), "anim: an auto-locked clip greys Follow root, a click sends nothing");
		demo.as.clip = demo.as.clips[4].name;	// no usedBy
		sync();
		check(!ab.usedBy()->visible && !ab.rootText()->visible, "anim: no used-by or root line when empty");
		demo.as.clip = demo.as.clips[3].name;
		demo.as.root = "travels 50.6 u per loop (root-locked)";
		demo.animLog.clear();
		check(key(SAPP_KEYCODE_ESCAPE) && !ab.shown() && demo.animLog == "close;", "anim: Escape closes it");
		ab.filter()->text.clear();
		ab.filter()->onChange("");
		demo.as.trails = true;
		demo.as.playing = true;
		ab.show(true);
	}
	if (demo.frame == 119 && demo.showTree)
	{
		demo.tree->show(true);
		demo.tree->update(demo.ts);
	}
	if (demo.frame == 120 && demo.showTree)
	{
		ui::DebugTree& t = *demo.tree;
		check(t.layout().ok() && t.shown() && t.window()->resizable && t.swatch()->shown() && t.window()->caption().text == "Squad 1234 - Hungry Bandits",
			("debug tree: from debugtree.layout, resizable, the title's swatch and name (" + t.layout().error() + ")").c_str());
		check(t.visibleRows() == 4 && t.toggle(0) && !t.toggle(1) && t.toggle(2) && t.path(2) == "squad/members", "debug tree: depth 0 open, deeper closed; toggles on the nodes with children");
		check(clickOn(t.toggle(2)) && (t.update(demo.ts), t.visibleRows() == 6) && t.path(3) == "squad/members/member" && t.path(4) == "squad/members/member#1",
			"debug tree: a toggle opens a node; a repeated key gets #n in its path");
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 5, 5);
		ui::frame(0);
		clickOn(t.toggle(0), SAPP_MODIFIER_SHIFT);
		t.update(demo.ts);
		ui::frame(0);
		const size_t closedAll = t.visibleRows();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 5, 5);
		clickOn(t.toggle(0), SAPP_MODIFIER_SHIFT);
		t.update(demo.ts);
		ui::frame(0);
		check(closedAll == 1 && t.visibleRows() == 8, ("debug tree: Shift+click closes / opens a whole branch (" + std::to_string(closedAll) + ", " + std::to_string(t.visibleRows()) + ")").c_str());
		demo.ts.rows.insert(demo.ts.rows.begin() + 7, { { 2, "member", "Bandit C" }, { 3, "hp", "12" } });
		demo.ts.tick = 5001;
		t.update(demo.ts);
		check(t.visibleRows() == 9 && t.path(7) == "squad/members/member#2", "debug tree: a refresh with a new row keeps the open state (the new node closed)");
		t.filter()->text = "55";
		t.filter()->onChange("55");
		t.update(demo.ts);
		check(t.visibleRows() == 4 && t.path(3) == "squad/members/member#1/hp", "debug tree: the filter keeps the matches and their ancestors, open");
		t.filter()->text.clear();
		t.filter()->onChange("");
		t.update(demo.ts);
		ui::frame(0);
		check(clickOn(t.copyButton()) && demo.treeCopy.rfind("Squad 1234 - Hungry Bandits (tick 5001)\nsquad: 1234 Hungry Bandits\n  goal: raid\n  members: 2\n    member: Bandit A\n", 0) == 0,
			("debug tree: COPY gives the shown rows as indented text (" + demo.treeCopy.substr(0, 60) + ")").c_str());
		demo.treeLog.clear();
		check(clickOn(t.refreshButton()) && clickOn(t.autoBox()) && demo.treeLog == "refresh;auto on;", ("debug tree: refresh and auto ask the host (" + demo.treeLog + ")").c_str());
		t.update(demo.ts);
		check(t.autoBox()->checked, "debug tree: auto follows the host's state");
		demo.treeLog.clear();
		check(key(SAPP_KEYCODE_ESCAPE) && !t.shown() && demo.treeLog == "close;", "debug tree: Escape closes it");
		t.show(true);
	}
	if (demo.frame == 119 && demo.showTrails)
	{
		demo.trails->show(true);
		demo.trails->update(demo.trs);
	}
	if (demo.frame == 120 && demo.showTrails)
	{
		ui::TrailsPanel& t = *demo.trails;
		auto sync = [&] { t.update(demo.trs); ui::frame(0); };
		ui::frame(0);	// shown at 119: its first layout
		check(t.layout().ok() && t.shown() && t.window()->caption().text == "TRAILS" && t.onBox()->checked && t.statusLabel()->text == demo.trs.status,
			("trails: from trailspanel.layout, On ticked and the status line from the state (" + t.layout().error() + ")").c_str());
		check(t.rows() == 15 && t.popsLabel(0) == nullptr && t.swatch(0) == nullptr && t.swatch(1) && t.swatch(4) && !t.swatch(5),
			"trails: an All row then one per character; a swatch where the host gave a colour");
		check(t.windowBox()->text == "30", ("trails: the window box shows the state's seconds (" + t.windowBox()->text + ")").c_str());
		check(t.popsLabel(2)->colour == ui::res::colour("BadBright") && t.popsLabel(2)->text == "3" && !(t.popsLabel(1)->colour == ui::res::colour("BadBright")),
			"trails: pops > 0 in the warning ink, 0 not");
		{
			const ui::Rect lv = t.list()->screen();
			check(t.list()->thumb().h > 0, "trails: 15 rows overflow the list (a scroll bar)");
			send(SAPP_EVENTTYPE_MOUSE_SCROLL, lv.x + lv.w / 2, lv.y + lv.h / 2, -3);
			ui::frame(0);
			check(t.list()->offset > 0, ("trails: the wheel scrolls the list (" + std::to_string(t.list()->offset) + ")").c_str());
			t.list()->offset = 0;
			ui::frame(0);
		}
		demo.trailsLog.clear();
		check(clickOn(t.row(3)) && demo.trailsLog == "pick 103;", ("trails: a row click picks its character (" + demo.trailsLog + ")").c_str());
		sync();
		demo.trailsLog.clear();
		check(clickOn(t.row(3)) && demo.trailsLog == "pick 0;", ("trails: a second click on the pick goes back to all (" + demo.trailsLog + ")").c_str());
		sync();
		clickOn(t.row(5));
		sync();
		demo.trailsLog.clear();
		check(clickOn(t.row(0)) && demo.trailsLog == "pick 0;", ("trails: the All row picks all (" + demo.trailsLog + ")").c_str());
		sync();
		{
			// k16.3.13: while trails record the counts change every frame; the rows stay, so a press held over frames still picks on release.
			ui::Widget* r2 = t.row(2);
			const ui::Rect rr = r2->screen();
			demo.trailsLog.clear();
			send(SAPP_EVENTTYPE_MOUSE_MOVE, rr.x + rr.w / 2, rr.y + rr.h / 2);
			send(SAPP_EVENTTYPE_MOUSE_DOWN, rr.x + rr.w / 2, rr.y + rr.h / 2);
			for (int f = 0; f < 4; f++)
			{
				for (ui::TrailsChar& c : demo.trs.chars) { c.points += 3; }
				demo.trs.chars[0].pops++;
				sync();
			}
			send(SAPP_EVENTTYPE_MOUSE_UP, rr.x + rr.w / 2, rr.y + rr.h / 2);
			check(t.row(2) == r2 && demo.trailsLog == "pick 102;" && t.popsLabel(1)->text == "4" && t.popsLabel(1)->colour == ui::res::colour("BadBright"),
				("trails: counts change in place (the rows stay), a press held over four frames still picks (" + demo.trailsLog + ")").c_str());
			sync();
			demo.trailsLog.clear();
			clickOn(t.row(0));
			sync();
			demo.trs.chars[0].pops = 0;
			sync();
			check(!(t.popsLabel(1)->colour == ui::res::colour("BadBright")) && t.popsLabel(1)->text == "0", "trails: pops back to 0, the warning ink goes");
			// CLEAR takes a click across its painted width (the client worker saw its left part miss).
			const ui::Rect cr = t.clearButton()->screen();
			const float u = ui::scale();
			int hits = 0;
			const int n = 9;
			for (int i = 0; i < n; i++)
			{
				const float x = cr.x + 1 + (cr.w - 2) * i / (n - 1);
				demo.trailsLog.clear();
				send(SAPP_EVENTTYPE_MOUSE_MOVE, x, cr.y + cr.h / 2);
				send(SAPP_EVENTTYPE_MOUSE_DOWN, x, cr.y + cr.h / 2);
				send(SAPP_EVENTTYPE_MOUSE_UP, x, cr.y + cr.h / 2);
				if (demo.trailsLog == "clear;") { hits++; }
			}
			const ui::Rect wr = t.window()->screen();
			check(hits == n, ("trails: CLEAR clicks across its width (" + std::to_string(hits) + "/" + std::to_string(n) + "; at " + std::to_string((cr.x - wr.x) / u) + ".."
				+ std::to_string((cr.right() - wr.x) / u) + " of the window, units)").c_str());
			for (ui::TrailsChar& c : demo.trs.chars) { c.points = 300 + 37 * (uint32_t)(c.id - 101); }
			demo.trs.chars[1].pops = 3;
			demo.trs.chars[5].pops = 1;
			sync();
		}
		demo.trailsLog.clear();
		check(clickOn(t.onBox()) && demo.trailsLog == "off;" && (sync(), !t.onBox()->checked), "trails: On asks the host, its state unticks it");
		clickOn(t.onBox());
		sync();
		demo.trailsLog.clear();
		clickOn(t.windowBox());
		key(SAPP_KEYCODE_END);
		key(SAPP_KEYCODE_BACKSPACE);
		key(SAPP_KEYCODE_BACKSPACE);
		type("12.5");
		key(SAPP_KEYCODE_ENTER);
		sync();
		check(demo.trailsLog == "window 12.5;" && t.windowBox()->text == "12.5", ("trails: typed seconds set the window (" + demo.trailsLog + ")").c_str());
		demo.trailsLog.clear();
		key(SAPP_KEYCODE_END);
		type("x");
		key(SAPP_KEYCODE_ENTER);
		sync();
		check(demo.trailsLog.empty() && t.windowBox()->text == "12.5", ("trails: bad input puts the shown value back (" + t.windowBox()->text + ")").c_str());
		check(t.capButton()->text == "LAST 30 S" && clickOn(t.capButton()) && demo.trailsLog == "window 0;" && (sync(), t.windowBox()->text == "0"),
			("trails: LAST 30 S sets window 0, the cap (" + t.capButton()->text + ", " + demo.trailsLog + ")").c_str());
		demo.trs.cap = 12.5f;
		sync();
		check(t.capButton()->text == "LAST 12.5 S", ("trails: the cap button follows the host's cap (" + t.capButton()->text + ")").c_str());
		demo.trs.cap = 30;
		sync();
		demo.trailsLog.clear();
		check(clickOn(t.clearButton()) && demo.trailsLog == "clear;" && (sync(), t.popsLabel(2)->text == "0"), "trails: CLEAR asks the host, its zeroed counts show");
		check(ui::trailsSeconds(0) == "0" && ui::trailsSeconds(2) == "2" && ui::trailsSeconds(0.25f) == "0.25" && ui::trailsSeconds(-1) == "0", "trails: the seconds text");
		demo.trailsLog.clear();
		ui::focus(t.window());
		check(key(SAPP_KEYCODE_ESCAPE) && !t.shown() && demo.trailsLog == "close;", ("trails: Escape closes it (" + demo.trailsLog + ")").c_str());
		for (ui::TrailsChar& c : demo.trs.chars) { c.points = 300 + 37 * (uint32_t)(c.id - 101); }	// the shot: counts back, one pick, a window
		demo.trs.chars[1].pops = 3;
		demo.trs.chars[5].pops = 1;
		demo.trs.pick = 102;
		demo.trs.window = 30;
		t.show(true);
		sync();
	}
	if (demo.frame == 119 && demo.showLights)
	{
		demo.lights->show(true);
		demo.lights->update(demo.ls);
	}
	if (demo.frame == 120 && demo.showLights)
	{
		ui::LightsPanel& l = *demo.lights;
		auto sync = [&] { l.update(demo.ls); ui::frame(0); };
		ui::frame(0);	// shown at 119: its first layout
		check(l.layout().ok() && l.shown() && l.window()->caption().text == "LIGHTS" && l.rows() == 23,
			("lights: from lightspanel.layout, one row per term and lamp (" + std::to_string(l.rows()) + "; " + l.layout().error() + ")").c_str());
		check(l.headers() == 9 && l.header(0)->text == "SUN" && l.header(2)->text == "LAMPS" && l.header(7)->text == "POST" && l.header(8)->text == "MARKER",
			"lights: the host's groups in order (sun .. post), an unknown one (marker) last");
		check(ui::lightGroups({ { "x", "zzz", true }, { "y", "post", true }, { "z", "sun", true }, { "w", "zzz", true } }) == std::vector<std::string>{ "sun", "post", "zzz" },
			"lights: lightGroups keeps the known order, then the others as first seen");
		check(l.tick("sun_direct")->checked && !l.tick("sun_shadows")->checked && !l.tick("fog_weather")->checked && l.countLabel()->text == "2 off"
			&& l.countLabel()->colour == ui::res::colour("BadBright"), ("lights: the ticks and the off count from the state (" + l.countLabel()->text + ")").c_str());
		{
			const ui::Rect& wr = l.window()->screen();
			const ui::Rect& lr = l.list()->screen();
			check(l.solo("lamp:3977") && l.solo("lamp:3977")->screen().y + l.solo("lamp:3977")->screen().h <= lr.y + lr.h && wr.y + wr.h <= ui::root().screen().h,
				"lights: every row fits the list unscrolled (lamps in view) and the window fits the screen");
		}
		ui::TickBox* sd = l.tick("sun_direct");
		demo.lightsLog.clear();
		check(clickOn(sd) && demo.lightsLog == "set sun_direct off;", ("lights: a tick calls set with the row's name (" + demo.lightsLog + ")").c_str());
		sync();
		check(l.tick("sun_direct") == sd && !sd->checked && l.countLabel()->text == "3 off", "lights: the host's state unticks it in place (the rows stay)");
		demo.lightsLog.clear();
		check(clickOn(l.tick("sun_shadows")) && demo.lightsLog == "set sun_shadows on;", ("lights: an off tick asks for on (" + demo.lightsLog + ")").c_str());
		sync();
		demo.lightsLog.clear();
		check(clickOn(l.solo("lamp:4013")) && demo.lightsLog == "solo lamp:4013;", ("lights: SOLO calls solo with the row's name (" + demo.lightsLog + ")").c_str());
		sync();
		check(l.tick("lamp:4013")->checked && l.tick("lamps")->checked && !l.tick("lamp:4012")->checked && l.countLabel()->text == "21 off",
			("lights: the solo shows as the host's ticks (" + l.countLabel()->text + ")").c_str());
		demo.lightsLog.clear();
		check(clickOn(l.solo("bloom")) && demo.lightsLog == "solo bloom;", ("lights: SOLO on another group's row (" + demo.lightsLog + ")").c_str());
		sync();
		demo.lightsLog.clear();
		check(clickOn(l.allButton()) && demo.lightsLog == "all on;" && (sync(), l.countLabel()->text == "all on" && l.tick("sun_direct")->checked),
			("lights: ALL ON asks the host, every tick back (" + demo.lightsLog + ")").c_str());
		demo.ls.terms.insert(demo.ls.terms.begin() + 20, { "lamp:4100", "lamps", true });	// a lamp comes into view, nearest
		demo.ls.terms.pop_back();	// and one goes out of it
		sync();
		check(l.rows() == 23 && l.tick("lamp:4100") && !l.tick("lamp:3977"), "lights: the lamp rows follow the host's view");
		demo.lightsLog.clear();
		check(clickOn(l.solo("lamp:4100")) && demo.lightsLog == "solo lamp:4100;", ("lights: a new lamp row's SOLO (" + demo.lightsLog + ")").c_str());
		demo.lightsLog.clear();
		ui::focus(l.window());
		check(key(SAPP_KEYCODE_ESCAPE) && !l.shown() && demo.lightsLog == "close;", ("lights: Escape closes it (" + demo.lightsLog + ")").c_str());
		for (ui::LightRow& t : demo.ls.terms) { t.on = t.name != "sun_shadows" && t.name != "cut_band"; }	// the shot: two switched off
		l.show(true);
		sync();
	}
	if (demo.frame == 119 && demo.showDebug)
	{
		demo.debug->update(demo.dbg);
		demo.debug->show(true);
	}
	if (demo.frame == 120 && demo.showDebug)
	{
		ui::DebugPanel& dp = *demo.debug;
		check(dp.layout().ok() && dp.window()->screen().w > 0, ("debug: built from debug.layout (" + dp.layout().error() + ")").c_str());
		{
			const ui::Rect wr = dp.weatherRow(50)->screen(), lv = dp.weatherList()->screen();
			check(dp.weatherRow(50)->checked && wr.y >= lv.y - 0.5f && wr.bottom() <= lv.bottom() + 0.5f,
				("debug: the current weather (row 50 of 54) in view when the panel opens (" + std::to_string(wr.y) + " in " + std::to_string(lv.y) + ".." + std::to_string(lv.bottom()) + ")").c_str());
			demo.dbg.weather = "Dust storm";
			dp.update(demo.dbg);
			ui::frame(0);
		}
		check(dp.shown() && dp.toggle(0) && dp.toggle(3) && !dp.toggle(4) && !dp.toggle(0)->checked && dp.toggle(2)->checked, "debug: a tick box per toggle, ticked by the state");
		check(clickOn(dp.toggle(0)) && demo.debugLog == "toggle navmesh;", ("debug: a click asks for the toggle (" + demo.debugLog + ")").c_str());
		dp.update(demo.dbg);
		check(dp.toggle(0)->checked, "debug: the host's next state ticks it");
		demo.debugLog.clear();
		check(dp.scenesBox()->visible && clickOn(dp.scene(1)) && demo.debugLog == "scene squin_gate;", "debug: a scene button asks for its scene");
		{
			// The key list left the panel (k16.2.352: the Options Keys tab has every binding); the host's extra text sits under the toggles.
			const ui::Widget* ex = dp.extra();
			const ui::Rect er = ex->screen(), last = dp.toggle(3)->screen(), sb = dp.scenesBox()->screen();
			check(static_cast<const ui::RichText*>(ex)->text == demo.dbg.extra && ex->shown() && er.h > 40 && er.y >= last.bottom() && er.bottom() <= sb.y + 0.5f
				&& er.right() <= dp.command(0)->screen().x, ("debug: the host's extra text (two sources) under the toggles, above the scenes (" + std::to_string(er.y) + "+" + std::to_string(er.h) + ")").c_str());
			check(dp.window()->screen().w < 620 * ui::scale(), "debug: no key list, the panel narrows to the toggles and commands");
		}
		{
			// The middle column: commands, the reply, time, weather.
			demo.debugLog.clear();
			check(dp.command(0) && dp.command(1) && dp.command(4) && !dp.command(5) && dp.command(0)->text == "Spawn bandit patrol" && dp.command(0)->enabled && !dp.command(1)->enabled,
				"debug: a button per command, in order, the host's enabled");
			check(clickOn(dp.command(0)) && demo.debugLog == "command bandits;", ("debug: a command button sends its id (" + demo.debugLog + ")").c_str());
			demo.debugLog.clear();
			clickOn(dp.command(1));
			check(demo.debugLog.empty(), "debug: a disabled command sends nothing");
			check(dp.reply()->visible && dp.reply()->text == "spawned squad 41760" && dp.reply()->screen().y > dp.command(1)->screen().bottom(),
				"debug: the host's reply under the commands");
			{
				// k16.2.364: WINDOWS apart from ACTIONS; the character actions in a popup toolbar.
				ui::Button* win = dp.command(2);
				check(win->text == "Lights" && dp.windowsBox()->shown() && win->screen().bottom() <= dp.command(0)->screen().y && dp.characterButton()->shown()
					&& dp.characterButton()->screen().y >= dp.command(1)->screen().bottom() && dp.characterButton()->screen().bottom() <= dp.reply()->screen().y + 0.5f,
					"debug: WINDOWS (Lights, Animations, Full map) above ACTIONS; Character... after the actions, above the reply");
				demo.debugLog.clear();
				check(clickOn(win) && demo.debugLog == "command lights;", ("debug: a window button sends its id (" + demo.debugLog + ")").c_str());
				demo.debugLog.clear();
				check(!dp.characterPopup() && clickOn(dp.characterButton()) && dp.characterPopup() && dp.characterCommand(3) && !dp.characterCommand(4),
					"debug: Character... opens a toolbar of the four character commands");
				const ui::Rect bar = dp.characterPopup()->screen(), cb = dp.characterButton()->screen();
				check(bar.x >= cb.x - 1 && bar.y >= cb.bottom() && dp.characterCommand(0)->text == "Heal" && dp.characterCommand(3)->text == "Kill" && dp.characterCommand(0)->enabled
					&& !dp.characterCommand(2)->enabled && dp.characterCommand(3)->screen().right() <= bar.right() && dp.characterCommand(1)->screen().x > dp.characterCommand(0)->screen().right(),
					"debug: a row of buttons below the button, the disabled one (Wake) greyed");
				check(clickOn(dp.characterCommand(0)) && clickOn(dp.characterCommand(1)) && demo.debugLog == "command heal;command ko;" && dp.characterPopup(),
					("debug: a toolbar button sends its id and the toolbar stays open (" + demo.debugLog + ")").c_str());
				demo.debugLog.clear();
				clickOn(dp.characterCommand(2));
				check(demo.debugLog.empty() && dp.characterPopup(), "debug: a disabled toolbar button sends nothing");
				const ui::Rect tb = dp.timeButton(0)->screen();
				click(tb.x + 4, tb.y + 4);
				check(!dp.characterPopup(), "debug: a click outside closes the toolbar");
				clickOn(dp.characterButton());
				key(SAPP_KEYCODE_ESCAPE);
				check(!dp.characterPopup(), "debug: Escape closes the toolbar");
				demo.debugLog.clear();
			}
			check(dp.timeButton(0) && dp.timeButton(4) && !dp.timeButton(0)->enabled && !dp.timeButton(4)->enabled, "debug: the time buttons greyed until the host enables them");
			clickOn(dp.timeButton(1));
			ui::ClockDial* dial = dp.timeDial();
			const auto dialAt = [&](float h, float k) { const ui::Rect& r = dial->screen(); return Pt{ r.x + r.w / 2 - k * r.w / 2 * std::sin(h / 24 * 6.2831853f), r.y + r.h / 2 + k * r.h / 2 * std::cos(h / 24 * 6.2831853f) }; };
			if (dial) { click(dialAt(18, 0.8f).x, dialAt(18, 0.8f).y); }
			check(demo.debugLog.empty(), "debug: a greyed time button or dial sends nothing");
			check(dp.weatherRow(53) && !dp.weatherRow(54) && dp.weatherRow(0)->text == "Auto (normal weather)" && dp.weatherRow(2)->checked && !dp.weatherRow(0)->checked
				&& !dp.weatherRow(2)->enabled, "debug: the weather rows (Auto, then the names), the current one marked, greyed until enabled");
			clickOn(dp.weatherRow(3));
			check(demo.debugLog.empty(), "debug: a greyed weather row sends nothing");
			demo.dbg.timeEnabled = true;
			demo.dbg.weatherEnabled = true;
			demo.dbg.reply = "refused: no town within 2000";
			demo.dbg.commands[1].enabled = true;
			dp.update(demo.dbg);
			ui::frame(0);
			std::string presets;
			for (size_t i = 0; i < 5; i++) { if (clickOn(dp.timeButton(i))) { presets += "+"; } }
			check(presets.size() == 5 && demo.debugLog == "time dawn;time noon;time dusk;time midnight;time +1h;", ("debug: each time button sends its preset (" + demo.debugLog + ")").c_str());
			demo.debugLog.clear();
			{
				// The 24 h dial (k16.3.37): midnight at the bottom, clockwise, so 3 o'clock is 18:00; a drag sends once, on release.
				demo.dbg.hour = 9.5f;
				demo.dbg.sunrise = 5.5f;
				demo.dbg.sunset = 19.25f;
				dp.update(demo.dbg);
				ui::frame(0);
				const ui::Rect dr = dial->screen(), nb = dp.timeButton(1)->screen();
				check(dial->shown() && dial->enabled && dr.w > 60 && std::abs(dr.w - dr.h) < 1 && dr.x >= nb.right() && dr.y <= nb.y + 1 && dial->hour == 9.5f
					&& dial->sunrise == 5.5f && dial->sunset == 19.25f, "debug: the time dial beside the buttons, round, the host's hour and daylight");
				const Pt three = dialAt(18, 0.85f);
				send(SAPP_EVENTTYPE_MOUSE_MOVE, three.x, three.y);
				ui::frame(0);
				const std::string tip3 = dial->tip();
				const bool clicked3 = click(three.x, three.y);
				check(clicked3 && demo.debugLog == "time 18.00;" && tip3 == "Set the time: 18:00",
					("debug: a click at the dial's 3 o'clock sets 18:00, its tooltip says so (" + demo.debugLog + " / " + tip3 + ")").c_str());
				demo.debugLog.clear();
				const Pt p1 = dialAt(2.1f, 0.9f), p2 = dialAt(8, 0.5f), p3 = dialAt(14.3f, 1.3f);	// the last past the rim: still steers
				send(SAPP_EVENTTYPE_MOUSE_MOVE, p1.x, p1.y);
				send(SAPP_EVENTTYPE_MOUSE_DOWN, p1.x, p1.y);
				send(SAPP_EVENTTYPE_MOUSE_MOVE, p2.x, p2.y);
				send(SAPP_EVENTTYPE_MOUSE_MOVE, p3.x, p3.y);
				const bool quiet = demo.debugLog.empty();
				send(SAPP_EVENTTYPE_MOUSE_UP, p3.x, p3.y);
				check(quiet && demo.debugLog == "time 14.25;", ("debug: a drag round the dial sends one hour, snapped to 15 min, on release (" + demo.debugLog + ")").c_str());
				demo.debugLog.clear();
				const Pt mid = dialAt(0, 0.05f);
				click(mid.x, mid.y);
				check(demo.debugLog.empty() && dial->hourAt(mid.x, mid.y) < 0 && dial->hourAt(dialAt(0, 0.9f).x, dialAt(0, 0.9f).y) == 0
					&& dial->hourAt(dialAt(23.9f, 0.9f).x, dialAt(23.9f, 0.9f).y) == 0, "debug: the dial's centre picks nothing; near midnight snaps to 0");
				send(SAPP_EVENTTYPE_MOUSE_MOVE, 4, 4);
			}
			check(dp.command(1)->enabled && dp.reply()->text == "refused: no town within 2000", "debug: the host's enabled and reply updated");
			ui::ScrollView& wl0 = *dp.weatherList();
			auto wheelUp = [&]
			{
				for (int i = 0; i < 60; i++) { send(SAPP_EVENTTYPE_MOUSE_SCROLL, wl0.screen().x + 20, wl0.screen().y + 20, 1); }
				ui::frame(0);
			};
			const bool row3 = clickOn(dp.weatherRow(3));
			wheelUp();	// Auto is above the fold (the list shows Dust storm, the current one)
			const bool row0 = clickOn(dp.weatherRow(0));
			check(row3 && row0 && demo.debugLog == "weather Acid rain;weather auto;", ("debug: a weather row sends its name, Auto auto (" + demo.debugLog + ")").c_str());
			demo.dbg.weather = "auto";
			demo.dbg.reply.clear();
			dp.update(demo.dbg);
			check(dp.weatherRow(0)->checked && !dp.weatherRow(2)->checked && !dp.reply()->visible, "debug: auto marks the Auto row; no reply, no line");
			check(dp.command(0)->textColour == ui::res::colour("Main") && dp.timeButton(0)->textColour == ui::res::colour("Main")
				&& dp.weatherRow(1)->textColour == ui::res::colour("Main"), "debug: enabled buttons and rows in Main, apart from the greyed");
			ui::ScrollView& wl = *dp.weatherList();
			demo.dbg.weather = "Mist";	// the last row, below the fold
			dp.update(demo.dbg);
			ui::frame(0);
			const ui::Rect wr = dp.weatherRow(20)->screen(), lv = wl.screen();
			check(wl.offset > 0 && wr.y >= lv.y - 0.5f && wr.bottom() <= lv.bottom() + 0.5f, ("debug: the current weather scrolled into view (" + std::to_string(wl.offset) + ")").c_str());
			wheelUp();
			demo.dbg.reply = "spawned squad 41761";
			dp.update(demo.dbg);
			ui::frame(0);
			check(wl.offset == 0, "debug: another change (the reply) leaves the list where the user scrolled it");
			// The live case (client coordinator 2026-11-06): a layout before the window has its size must not leave the row short.
			ui::CaptionWindow& dw = static_cast<ui::CaptionWindow&>(*dp.window());
			const float mx = dw.moveX, my = dw.moveY;
			dw.sizeH = 400;	// taller first (as a resize would)
			demo.dbg.weather = "Zone weather 30";
			dp.update(demo.dbg);
			ui::frame(0);
			dw.sizeH = 0;	// back to its height
			dw.moveX = mx;
			dw.moveY = my;
			ui::frame(0);
			const ui::Rect r50 = dp.weatherRow(50)->screen(), lv50 = wl.screen();
			check(r50.y >= lv50.y - 0.5f && r50.bottom() <= lv50.bottom() + 0.5f, "debug: the current row still in view after the list shrinks (reveal holds until the user scrolls)");
			demo.dbg.weather = "Mist";
			dp.update(demo.dbg);
			demo.dbg.commands[1].enabled = false;
			dp.update(demo.dbg);
			const_cast<ui::Layout&>(dp.layout()).reload();
			ui::frame(0);
			check(dp.layout().ok() && dp.timeButton(0)->enabled && !dp.command(1)->enabled && dp.weatherRow(20)->checked, "debug: a layout reload keeps the enabled and marked states");
			demo.debugLog.clear();
			demo.dbg.weather = "Dust storm";
			demo.dbg.reply = "spawned squad 41760";
			dp.update(demo.dbg);
		}
		demo.dbg.scenes.clear();
		dp.update(demo.dbg);
		check(!dp.scenesBox()->visible, "debug: no scenes, no list (a sim connected)");
		{
			const std::string extra = demo.dbg.extra;
			demo.dbg.extra.clear();
			dp.update(demo.dbg);
			const bool hidden = !dp.extra()->shown();
			demo.dbg.extra = extra;
			dp.update(demo.dbg);
			check(hidden && dp.extra()->shown(), "debug: no extra text, no gap; back when the host sends it");
		}
		demo.debugLog.clear();
		check(key(SAPP_KEYCODE_ESCAPE) && !dp.shown() && demo.debugLog == "close;", "debug: Escape closes it");
		dp.show(true);
	}
	if (demo.frame == 123 && demo.showDebug)	// the screenshot at 125 shows the Character... toolbar open
	{
		clickOn(demo.debug->characterButton());
		check(demo.debug->characterPopup(), "debug: the toolbar open for the screenshot");
	}
	if (demo.frame == 119 && demo.showProspect)
	{
		demo.menuLog.clear();
		pro.update(demo.ps);
		pro.show(true);
	}
	if (demo.frame == 120 && demo.showProspect)
	{
		check(pro.shown() && pro.tick(1)->text == "Water" && pro.value(1)->text == "62" && pro.tick(3)->checked && !pro.tick(2)->checked, "prospecting: resource rows, values, ticks");
		const ui::Color ink = ui::style().textColour(ui::Look::kInset, ui::State::kNormal, false);
		check(pro.overviewLabel(0, 1)->text == "Altitude:" && pro.overviewValue(0, 1)->text == "420 m" && pro.overviewLabel(0, 0)->colour == ui::style().colour("ProspectZone", {})
			&& pro.overviewValue(1, 0)->colour == ink && pro.overviewLabel(1, 1)->colour == ink && !pro.overviewLabel(1, 2), "prospecting: overview columns and colours");
		check(pro.value(0)->colour == ui::style().colour("ProspectHigh", {}) && pro.value(1)->colour == ui::style().colour("ProspectMid", {})
			&& pro.value(2)->colour == ui::style().colour("ProspectLow", {}), "prospecting: value colours by band (< 50, 50..75, > 75)");
		const ui::Rect m = pro.map()->screen(), k = pro.marker();
		check(k.w > 0 && std::abs(k.x + k.w / 2 - (m.x + m.w / 2)) < 1.5f && std::abs(k.y + k.h / 2 - (m.y + m.h * 0.4f)) < 1.5f, "prospecting: the camera marker at its uv");
		check(clickOn(pro.tick(2)) && demo.menuLog == "select copper;" && pro.tick(2)->checked && !pro.tick(3)->checked, "prospecting: one resource selected, picking another is the host's order");
		demo.menuLog.clear();
		check(clickOn(pro.tick(2)) && demo.menuLog.empty(), "prospecting: the selected one again does nothing");
	}
	if (demo.frame == 121 && demo.showProspect)
	{
		demo.menuLog.clear();
		key(SAPP_KEYCODE_ESCAPE);
		check(!pro.shown() && demo.menuLog == "close;", "prospecting: Escape closes");
		pro.window()->rect.x = 200;	// as if dragged
		pro.show(true);
		check(pro.window()->rect.x == 0 && pro.window()->rect.y == 0, "prospecting: re-centred on show");
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 5, 5);
	}
	if (demo.frame == 119 && demo.showTutorial)	// what the screenshot shows
	{
		ui::Tutorial cam{ "camera", "Looking around", { { "Pan the view with the movement keys and turn it with the middle mouse button; the wheel zooms.\n\nTry both now.", true },
			{ "Double-click a portrait to follow that character." } } };
		tu.setAll({ cam, { "squad", "Your squad", { { "The portraits along the bottom are your squad." } } }, { "hunger", "Food", { { "Characters get hungry." } } } });
		tu.hint("camera");
		tu.hint("hunger");
		tu.open("camera", 0);
		tu.showBrowser(true);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, 5, 5);	// off the buttons: their plain look
		key(SAPP_KEYCODE_RIGHT);	// as kclient's F1 + Right
		check(tu.activateButton()->enabled && tu.list()->selected == 2, "tutorials: TUTORIALS keeps its selection by id across setAll, ACTIVATE live");
	}
	ui::JobList& jl = *demo.hud->jobList();
	auto jobIds = [] { std::string r; for (const ui::Job& j : demo.hs.jobs) { r += std::to_string(j.id); } return r; };
	if (demo.frame == 119 && demo.showJobs)	// the jobs list (jobs runs only)
	{
		demo.menuLog.clear();
		check(jl.shown() && jl.jobs().size() == 4 && static_cast<const ui::Label*>(jl.row(0)->children()[0].get())->text == "1: Medic" && !jl.emptyText()->visible,
			"jobs: the list shows the jobs, numbered");
		const ui::Rect a = jl.row(0)->screen(), b = jl.row(2)->screen();
		const float x = a.x + 40 * ui::scale();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, x, a.y + a.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, x, a.y + a.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, x, b.y + b.h * 0.75f);
		check(jl.dropIndex() == 2, ("jobs: dragging a row shows where it lands (" + std::to_string(jl.dropIndex()) + ")").c_str());
		send(SAPP_EVENTTYPE_MOUSE_UP, x, b.y + b.h * 0.75f);
		check(jl.dropIndex() < 0 && demo.menuLog == "move job 1 2;" && jobIds() == "2314", ("jobs: dropping it moves the job (" + demo.menuLog + jobIds() + ")").c_str());
		demo.menuLog.clear();
	}
	if (demo.frame == 120 && demo.showJobs)
	{
		check(static_cast<const ui::Label*>(jl.row(2)->children()[0].get())->text == "3: Medic", "jobs: the numbers follow the order");
		check(clickOn(jl.row(1)) && demo.menuLog.empty() && jobIds() == "2314", "jobs: a click without a drag moves nothing");
		check(clickOn(jl.removeButton(0)) && demo.menuLog == "remove job 2;", ("jobs: X removes it (" + demo.menuLog + ")").c_str());
		const ui::Rect a = jl.row(0)->screen();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, a.x + 40, a.y + a.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, a.x + 40, a.y + a.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, a.x + 40, a.y + a.h / 2 - 2 * ui::scale());
		send(SAPP_EVENTTYPE_MOUSE_UP, a.x + 40, a.y + a.h / 2 - 2 * ui::scale());
		check(demo.menuLog == "remove job 2;", "jobs: a short wobble is not a drag");
	}
	if (demo.frame == 121 && demo.showJobs)
	{
		demo.menuLog.clear();
		const ui::Rect a = jl.row(0)->screen(), last = jl.row(2)->screen();
		const float x = a.x + 40 * ui::scale(), below = last.bottom() + 30 * ui::scale();
		send(SAPP_EVENTTYPE_MOUSE_MOVE, x, a.y + a.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_DOWN, x, a.y + a.h / 2);
		send(SAPP_EVENTTYPE_MOUSE_MOVE, x, below);
		send(SAPP_EVENTTYPE_MOUSE_UP, x, below);
		check(demo.menuLog == "move job 3 2;" && jobIds() == "143", ("jobs: a drop on empty space puts it last (" + demo.menuLog + jobIds() + ")").c_str());
	}
	if (demo.frame == 122 && demo.showJobs) { demo.hs.jobs.clear(); }
	if (demo.frame == 123 && demo.showJobs)
	{
		check(jl.shown() && jl.emptyText()->visible && !jl.row(0), "jobs: no jobs shows the hint text");
		demo.hs.jobsValid = false;
	}
	if (demo.frame == 124 && demo.showJobs)
	{
		check(!jl.shown(), "jobs: hidden without a valid state");
		demo.hs.jobsValid = true;
		demo.hs.jobs = { { 1, "Medic" }, { 2, "Robotics" }, { 3, "Operating machine: Iron Refinery" }, { 4, "Hauling to: Storage chest" } };
		demo.hs.jobs[2].dim = demo.hs.jobs[3].dim = true;	// as JOBS off: the AI skips them
		for (int i = 5; i <= 9; i++) { demo.hs.jobs.push_back({ (uint64_t)i, "Job " + std::to_string(i) }); }
	}
	if (demo.frame == 105 && demo.showMenu)	// what the screenshot shows
	{
		if (demo.showMenu == 1)
		{
			mm.show(true);
			mm.showCredits(true);
		}
		if (demo.showMenu == 6)	// the server starting, NEW GAME clicked and held by the host (kenshi-k16.2.210.2)
		{
			mm.show(true);
			mm.setOffline("Starting the game server...");
			mm.setPending(1);
		}
		if (demo.showMenu == 2) { gm.show(true); }
		if (demo.showMenu == 7)	// the EXIT GAME confirm over the game menu (kenshi-k16.2.212.2)
		{
			gm.show(true);
			gm.button(4)->onClicked();
		}
		if (demo.showMenu == 3)
		{
			sp.open(ui::SaveMode::kLoad);
			sp.resetTick()->checked = false;
		}
		if (demo.showMenu == 4)
		{
			gm.show(true);
			sp.open(ui::SaveMode::kSave);
		}
		if (demo.showMenu == 5)
		{
			mm.show(true);
			ng.open();
		}
	}
}

void frame()
{
	const bool shooting = !demo.shot.empty();
	const double dt = shooting ? 1.0 / 30 : stm_sec(stm_laptime(&demo.last));
	ui::setViewport(sapp_width(), sapp_height());
	if (shooting || demo.showStatsMock) { script(); }	// statsmock plays the script in a window too: the mockup is built there
	cfg::poll();
	if (!demo.hs.paused) { demo.hs.hour = std::fmod(demo.hs.hour + (float)dt * demo.hs.speed / 60, 24.0f); }	// a game minute per second
	demo.hs.squadTabs.clear();
	for (const ui::SquadInfo& q : demo.sq.squads) { if (!q.members.empty()) { demo.hs.squadTabs.push_back({ q.id, q.name }); } }	// as the original: squads with members
	demo.hud->update(demo.hs);
	demo.opts->update();
	demo.cr->update(demo.cs);
	demo.stats->update(demo.ss);
	demo.squads->update(demo.sq);
	demo.research->update(demo.rs);
	demo.crafting->update(demo.cs2);
	demo.faction->update(demo.fs);
	demo.building->update(demo.bws);
	demoContainer();
	demo.gear->update(demo.gs);
	demo.lootGear->update(demo.lgs);
	demo.items->update(demo.iws);
	demo.talk->update(demo.ds);
	// Speakers' heads as the host would project them (pixels): two close together, one apart.
	const float W = sapp_widthf(), H = sapp_heightf();
	// Two close together (they overlap, as in the original), one faded by distance, a large important one and an ordinary one at the right edge
	// (only the important one is kept on screen), one out of range.
	demo.bubbles->update({ { 1, "Hey! You there!", W * 0.80f, H * 0.42f, 1, false, false },
		{ 2, "Give us all your food. All of it, the water too, and those boots, or we take the lot off your corpse and leave you to the birds.", W * 0.82f, H * 0.43f, 1, false, false },
		{ 3, "Mmm.", W * 0.62f, H * 0.80f, 0.5f, false, false }, { 4, "Guards! Guards!", W * 0.99f, H * 0.55f, 1, true, true },
		{ 5, "Over there.", W * 0.995f, H * 0.62f, 1, false, false }, { 6, "Too far to read.", W * 0.3f, H * 0.5f, 0, false, false } });
	demo.progress->update({ { 1, "Picking lock", 0.3f, W * 0.62f, H * 0.30f }, { 2, "Filling up", 1, W * 0.64f, H * 0.31f } });	// stand-in wording
	demo.progress->floatTexts({ { 1, "Got it open!", { 0.4f, 0.8f, 0.4f, 0.9f }, W * 0.62f, H * 0.26f, 2 }, { 2, "No luck", { 0.7f, 0.4f, 0.4f, 0.7f }, W * 0.70f, H * 0.26f, 10, 2 },
		{ 3, "Gone", { 1, 1, 1, 1 }, W * 0.70f, H * 0.22f, 12 } });
	demo.names->update({ { 1, "Beep", false, W * 0.56f, H * 0.18f }, { 2, "Ruka", true, W * 0.66f, H * 0.18f }, { 3, "Burn", false, W * 0.76f, H * 0.18f, 2, 0.3f } }, demo.namesOn);	// stand-in wording; ages 2, 10 (half faded, the Large font), 12 (gone)
	demo.hud->show(!demo.ds.active);	// the conversation panels cover the squad bar, as in the original
	ui::frame(dt);
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = demo.showHints == 1 ? sg_color{ 0.88f, 0.86f, 0.80f, 1.0f } : demo.showHints == 2 ? sg_color{ 0.05f, 0.05f, 0.06f, 1.0f }
		: demo.showToast == 1 ? sg_color{ 0.92f, 0.90f, 0.84f, 1.0f } : sg_color{ 0.55f, 0.47f, 0.36f, 1.0f };	// sand, to judge the panels' edges; --show hints: light / dark behind the see-through panel
	pass.swapchain = sglue_swapchain();
	sg_begin_pass(&pass);
	sgl_defaults();
	// World tags as the host draws them (item labels, name tags): normal, hovered, friendly. The host's own list, then the UI's (ui::record), as
	// kclient records them (record_ui; render hook 8's host half); each list's uploads apply before its replay.
	static dl::List tags, uiList;
	ui::gfx::begin(tags, sapp_width(), sapp_height());
	if (demo.showToast == 1)
	{
		// A busy light scene behind the toast line: pale blocks of sand, sky and white (the toast must read over the brightest ground).
		const ui::Color pale[] = { { 0.98f, 0.97f, 0.92f, 1 }, { 0.80f, 0.86f, 0.92f, 1 }, { 0.95f, 0.88f, 0.72f, 1 }, { 1, 1, 1, 1 }, { 0.85f, 0.80f, 0.70f, 1 } };
		const float s = std::round(H / 18);
		for (int y = 0; y * s < H / 2; y++)
		{
			for (int x = 0; x * s < W; x++) { ui::gfx::fill({ x * s, y * s, s, s }, pale[(x * 7 + y * 3 + x * y) % 5]); }
		}
	} else if (demo.showToast == 2) {
		// Lines of white debug text across the top, as kclient's debug overlay draws them under the toasts.
		const ui::Font* f = ui::fonts::get("Kenshi_StandardFont_Medium");
		for (int i = 0; f && i * f->lineHeight() < H / 2; i++)
		{
			f->draw(8, i * f->lineHeight(), "fps 60.0  frame 16.6 ms  chars 214  squads 31  pos 12043.5 -88.2 4410.7  zone 17,22  cam 1.00 yaw 33.4 pitch -41.0  sim tick 88213  path q 4  "
				"ai goals 412  nav 3 ms  draw calls 1840  tris 2.1M", i % 3 ? ui::Color{ 1, 1, 1, 1 } : ui::Color{ 1, 0.95f, 0.5f, 1 });
		}
	}
	const float th = std::round(H * 0.0259f);
	ui::drawTag({ std::round(W * 0.55f), std::round(H * 0.62f), std::round(W * 0.08f), th }, "Iron Plate", ui::TagState::kNormal);
	ui::drawTag({ std::round(W * 0.55f), std::round(H * 0.62f + th * 1.5f), std::round(W * 0.08f), th }, "Ring Blade", ui::TagState::kHover);
	ui::drawTag({ std::round(W * 0.55f), std::round(H * 0.62f + th * 3), std::round(W * 0.08f), th }, "Beep", ui::TagState::kFriendly);
	// Item labels by grade (ItemStandard, ItemGrade0..4, ItemArtifact; the host picks one by level), and one hovered where taking it is theft.
	const char* const grades[] = { "ItemStandard", "ItemGrade0", "ItemGrade1", "ItemGrade2", "ItemGrade3", "ItemGrade4", "ItemArtifact" };
	for (int i = 0; i < 7; i++) { ui::drawTag({ std::round(W * 0.64f), std::round(H * 0.62f + th * 1.5f * i), std::round(W * 0.06f), th }, grades[i], ui::TagState::kNormal, nullptr, ui::style().colour(grades[i], {})); }
	ui::drawTag({ std::round(W * 0.64f), std::round(H * 0.62f + th * 10.5f), std::round(W * 0.06f), th }, "Not yours", ui::TagState::kStealing);
	ui::drawTag({ std::round(W * 0.64f), std::round(H * 0.62f + th * 12), std::round(W * 0.06f), th }, "Theirs", ui::TagState::kOwned, nullptr, ui::style().colour("ItemStandard", {}));
	ui::gfx::end();
	ui::record(uiList);
	dl::gpu::apply(tags.takeUploads());
	dl::gpu::replay(tags);
	dl::gpu::apply(uiList.takeUploads());
	dl::gpu::replay(uiList);
	sgl_draw();
	sg_end_pass();
	sg_commit();
	demo.frame++;
	if (shooting && demo.frame == (demo.showTutorial || demo.showToast || demo.showJobs || demo.showProspect || demo.showDebug || demo.showTree || demo.showTrails || demo.showLights || demo.showHints || demo.showAnim || demo.showScenarios || demo.showNotes || demo.showSpectator || demo.showBuild || demo.showAdvanced || demo.showResearch || demo.showCrafting || demo.showFaction || demo.showBuilding ? 125 : demo.showItems ? 130 : demo.showHudBuilding || demo.showGearBody || demo.showTag ? 127 : demo.showMenu ? 110 : demo.showTrade || demo.showGear ? 88 : demo.showStatsMock ? 135 : 73))	// the tooltip (0.5 s at 30 fps) is up; the gear and trade ones are hovered at 72
	{
		save_png(demo.shot);
		sapp_quit();
	}
}

void event(const sapp_event* e)
{
	if (ui::event(*e)) { return; }
	if (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code == SAPP_KEYCODE_Q) { sapp_quit(); }
	if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN && e->mouse_button == SAPP_MOUSEBUTTON_RIGHT)
	{
		std::vector<ui::Menu::Item> items = { { "Attack", [] { ui::toast("Attack"); } }, { "Talk", [] { ui::toast("Talk"); } }, { "Loot", nullptr, false } };
		ui::openMenu(std::make_unique<ui::ContextMenu>(std::move(items)), e->mouse_x, e->mouse_y, true);
	}
	if (e->type == SAPP_EVENTTYPE_MOUSE_DOWN && e->mouse_button == SAPP_MOUSEBUTTON_LEFT) { ui::toast("World click (not consumed by the UI)"); }
}

void cleanup()
{
	demo.hud.reset();
	demo.opts.reset();
	demo.tree.reset();
	demo.anim.reset();
	demo.scen.reset();
	demo.notes.reset();
	demo.squads.reset();
	demo.overview.reset();
	demo.prompt.reset();
	demo.cr.reset();
	demo.items.reset();
	demo.gear.reset();
	demo.talk.reset();
	demo.bubbles.reset();
	demo.progress.reset();
	if (demo.extendAt != -2)
	{
		printf("kui_demo script: FAIL status panel: the slide's end was never checked\n");
		demo.failed = true;
	}
	ui::shutdown();
	dl::gpu::shutdown();	// after ui::shutdown (its destroys), before sgl_shutdown
	sgl_shutdown();
	sg_shutdown();
	if (demo.failed) { exit(1); }
}

}

sapp_desc sokol_main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const bool more = i + 1 < argc;
		if (a == "--data" && more) { demo.data = argv[++i]; }
		else if (a == "--screenshot" && more) { demo.shot = argv[++i]; }
		else if (a == "--size" && more && sscanf(argv[i + 1], "%dx%d", &demo.w, &demo.h) == 2) { i++; }
		else if (a == "--set" && more) { cfg::set(argv[++i]); }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "creator") { demo.showCreator = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "gear") { demo.showGear = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "dialogue") { demo.showDialogue = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "zone") { demo.showZone = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "stats") { demo.showStats = true; i++; }
		else if (a == "--options-tab" && more) { demo.optsTab = argv[++i]; }
		else if (a == "--options-row" && more) { demo.optsRow = argv[++i]; }
		else if (a == "--options-search" && more) { demo.optsSearch = argv[++i]; }
		else if (a == "--options-hit" && more) { demo.optsHit = std::max(1, atoi(argv[++i])); }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "options") { demo.showOptions = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "prompt") { demo.showPrompt = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "debug") { demo.showDebug = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "debugtree") { demo.showTree = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "trails") { demo.showTrails = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "lights") { demo.showLights = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "toast") { demo.showToast = 1; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "toast-text") { demo.showToast = 2; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "hints") { demo.showHints = 1; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "hints-dark") { demo.showHints = 2; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "anim") { demo.showAnim = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "scenarios") { demo.showScenarios = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "notes") { demo.showNotes = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "raise") { demo.showRaise = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "driving") { demo.showDriving = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "spectator") { demo.showSpectator = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "squads") { demo.showSquads = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "overview") { demo.showOverview = true; i++; }
		else if (a == "--map-sweep") { demo.mapSweep = true; }
		else if (a == "--map-notches" && more) { demo.mapNotches = atoi(argv[++i]); }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "mainmenu") { demo.showMenu = 1; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "gamemenu") { demo.showMenu = 2; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "load") { demo.showMenu = 3; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "save") { demo.showMenu = 4; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "newgame") { demo.showMenu = 5; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "mainmenu-pending") { demo.showMenu = 6; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "gamemenu-exit") { demo.showMenu = 7; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "tutorial") { demo.showTutorial = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "jobs") { demo.showJobs = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "context") { demo.showContext = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "prospect") { demo.showProspect = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "advanced") { demo.showAdvanced = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "build") { demo.showBuild = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "research") { demo.showResearch = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "crafting") { demo.showCrafting = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "faction") { demo.showFaction = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "building") { demo.showBuilding = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "mine") { demo.showBuilding = true; demo.buildingShot = 1; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "hudbuilding") { demo.showHudBuilding = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]).starts_with("tag-"))
		{
			const std::string w = std::string(argv[++i]).substr(4);
			for (int c = 1; c < (int)std::size(kTagCases); c++) { if (w == kTagCases[c]) { demo.showTag = c; } }
			if (!demo.showTag) { fprintf(stderr, "kui_demo: no tag case %s\n", w.c_str()); exit(2); }
		}
		else if (a == "--show" && more && std::string(argv[i + 1]) == "statsmock") { demo.showStatsMock = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "gearbody") { demo.showGearBody = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "items") { demo.showItems = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "message") { demo.showMessage = true; i++; }
		else if (a == "--show" && more && std::string(argv[i + 1]) == "trade") { demo.showTrade = true; i++; }
		else if (a == "--show" && more && (std::string(argv[i + 1]) == "food" || std::string(argv[i + 1]) == "weapon" || std::string(argv[i + 1]) == "blueprint" || std::string(argv[i + 1]) == "armour"))
		{
			const std::string w = argv[++i];
			demo.showTip = w == "food" ? 1 : w == "weapon" ? 2 : w == "armour" ? 4 : 3;
			demo.showGear = true;
		}
		else if (a == "--theme" && more) { demo.themes.push_back(argv[++i]); }	// a mod folder with gui/ files, e.g. mods/Dark UI
		else { fprintf(stderr, "usage: kui_demo [--data DIR] [--size WxH] [--set key=value] [--theme MODDIR]... [--show creator|gear|dialogue|zone|stats|options|prompt|toast|toast-text|squads|overview|mainmenu|gamemenu|gamemenu-exit|load|save|message|trade|food|weapon|blueprint|tag-CASE] [--screenshot out.png]\n"); exit(2); }
	}
	ui::keys::setDebug(true);	// the demo shows and checks the development windows (the debug panel, its keys, the Debug keys tab)
	sapp_desc d = {};
	d.init_cb = init;
	d.frame_cb = frame;
	d.event_cb = event;
	d.cleanup_cb = cleanup;
	d.width = demo.w;
	d.height = demo.h;
	d.window_title = "kui_demo";
	d.logger.func = slog_func;
	return d;
}
