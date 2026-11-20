// ksim_server: runs the simulation in real time and serves one SIMLINK client over TCP
// (docs/formats/SIMLINK.md).
//
//   ksim_server --listen 127.0.0.1:PORT [--scenario duel|duel_katana|duel_armour|squad|newgame [--start SID] [--stealth N]] [--seed N] [--wire-version] [--no-world] [--follow-heading-fix]
//               [--run-without-clients] [--bake DIR [--regions RX0 RZ0 RX1 RZ1]] [--navmesh FILE | --navworld DIR] [--ai-routes] [--squad-limit N]
//               [--log-world-squads N] [--place-squad TEMPLATE X Z]... [--force-dialogue CHARACTER DIALOGUE] [--own-container ID]... [--money N] [--give-item ITEM [COUNT]]... [--record FILE] [--order-log FILE] [--mods FILE] [--save-dir DIR]
//               [--scenario-file PATH]
//   ksim_server --dump-snapshot FILE [--dump-bin FILE] [--dump-ticks N] [--dump-script FILE]
//               [--seed N] [--no-world] [--squad-limit N]
//
// --save-dir DIR: where orders 80-83 (save / load / delete / list) keep the saves; default
// $XDG_DATA_HOME/ksim/saves, else ~/.local/share/ksim/saves; created if missing; refused inside the game install.
// --mods FILE: our mod list (kworld/loadorder.hpp grammar); without it the load order is vanilla.
// The install's own data/mods.cfg is never read.
// --no-world starts from the templates only (no default world, so no squads): quick to load.
// The squad AI (docs/reference: squad AI) runs on every server and takes its navigator from the navmesh below (server_ai.hpp);
// --bake DIR gives it a `knav_bake --world` bake's own streaming navigator instead, and --no-ai turns it off.
// The navmesh (no navmesh, no nav: moves are routed on it) is, by default, the runtime mesh: the streaming coverage over the
// shipped navtiles, converted to Detour at load and patched where the world changed (MeshNavigator::openNative, NAVMESH_CACHE.md
// "Runtime mesh"), with the patch cache on. No world bake is needed. --native-nav is that default, accepted as a no-op.
// --navmesh FILE reads a `knav_bake --out` cache of the Hub zone instead (~0.1 s); with neither it and --navworld the server no longer
// bakes the Hub zone at startup.
// --navworld DIR (a `knav_bake --world DIR` bake, 256 region files) replaces the default: the navmesh holds the zones around every player's
// full-detail area, loaded and dropped as the players move (docs/formats/NAVMESH_CACHE.md "Streaming coverage"); it starts with the Hub's
// 3 x 3 zones. The three navmesh options exclude each other.
// --scenario newgame: the world starts as a NEW GAME of --start (default 1980-gamedata.base, the Wanderer at the Hub) with the
// faction name Nameless, as the menu's NEW GAME with no character creation would: the start's own characters and stats, its money.
// --stealth N (testing only): every NEW_GAME character's Stealth skill is N (the sneak run: ninjarun past 30 u/s, kinspect
// sprint_anim.md); a client's NEW_GAME gets it too.
// Order 84 new_world returns to the world without it. Script name PLAYER = 0 (the members exist from the first tick).
// --scenario squad: two player characters (Engival, Izumi) and three bandits 700 units east, to watch
// follow (order 3) and the fight join; script names P1 P2 / E1 E2 E3 (PLAYER = P1, ENEMY = E1).
// --scenario duel_katana: the duel with the player holding a Katana (fists don't bleed; this one grows pools).
// --scenario duel_armour: the duel with the servant in full-coverage plate (hat, body, legs, boots), for the absorbed tail of event 1.
// --follow-heading-fix [on|off]: option squad.follow_heading_fix (default off = the original's follow heading; bare = on).
// --fix-extra-attack-slots on|off (default on): combat.fix_extra_attack_slots; off = the original's slot-loop bug (docs/COMBAT_OPTIONS.md 1).
// --no-runaway-skills on|off (default on): xp.no_runaway_skills; off = the original's addXP past 100 (docs/COMBAT_OPTIONS.md 2).
// The startup log names the bug-fix options 1, 2 and 5 ("ksim_server: options ..."), and 6, 7-10 on their own lines.
// The sim is paused (CLOCK says so) while no session has completed HELLO: it starts paused, resumes at the first ready session, pauses again
// when the last one leaves, keeps its speed, and an admin pause order wins. --run-without-clients keeps it ticking with no client (a server
// config will replace the flag). --dump-snapshot is unaffected.
// --ai-routes: also send CHAR_ROUTE for AI characters (debug overlay).
// --debug-view: answer the debug view's frames 200-219 to a loopback peer (docs/formats/SIMLINK.md "DEBUG VIEW"); only a
// KSIM_DEBUG_VIEW build sends the data (Release: the frames are accepted and dropped).
// The default mesh reads the native patch cache (docs/formats/NAVMESH_CACHE.md "Patch cache") from $XDG_CACHE_HOME/kenshi-sim/
// navmesh (KENSHI_NAV_CACHE overrides) under the startup data hash; --no-nav-cache turns it off (nothing read or written).
// --token-speeds on|off (default off): off-screen tokens move at 55 u/s travelling and 25 inside a town, capped at 70, instead of the flat
// 15 (setting ai.token_speeds, docs/COMBAT_OPTIONS.md 7). The startup log names the value.
// --campaign-victory-ai on|off (default off): a won raid whose campaign names a "victory AI" occupies the town instead of going home (setting
// world.campaign_victory_ai, docs/COMBAT_OPTIONS.md 10). The startup log names the value.
// --trace-member ID (repeatable): one stderr line per goal pick of that member (`ksim_server: trace tick ... member ID ...`, the
// candidates and their verdicts; docs/formats/SIMLINK.md "DEBUG VIEW"). With --debug-view every member's last 8 picks also go to its
// DEBUG_QUERY tree.
// --log-world-squads N: every N game minutes log (stderr, `ksim_server: world squad built ...`) one line per BUILT world squad (id, template, faction, team,
// hostile, members alive/total, leader x/z, token, goal), and one line at each promote / demote (docs/RUNBOOK.md).
// --place-squad TEMPLATE X Z (repeatable; testing only): a homeless wilderness squad of SQUAD record TEMPLATE (e.g. 1302-gamedata.base, the
// Dust Bandits) with its den at (X, Z), placed as a drawn one (Simulation::placeWildernessSquad, template size). Placed before the pristine
// snapshot, so every NEW_GAME (order 84) has it again. Needs the default world (not --no-world).
// --force-dialogue CHARACTER DIALOGUE (testing only): a speaker of CHARACTER record CHARACTER whose event roots the DIALOGUE record takes it,
// the chance rolls of its tree skipped (DialogueSystem::force), e.g. 1303-gamedata.base 4853-D-dialogue-02.mod, the starving mugging by squad 1307's leader.
// --own-container ID (repeatable; testing only): the storage container with Id ID (`dump things` kind 3) belongs to the player faction (the
// FACTION record "Nameless", which NEW_GAME gives the player), e.g. to see TRADE 53's chest prices. Set before the pristine snapshot, so every
// NEW_GAME has it again.
// --money N (testing only): every NEW_GAME (the menu's or --scenario newgame) gives the player faction N cats instead of the start's money,
// e.g. to hire a 3000-cat recruit from the Hub start.
// --give-item ITEM [COUNT] (repeatable; testing only): every NEW_GAME gives each player character COUNT (1) of the ITEM record ITEM in its
// main inventory, e.g. Bread (1980-gamedata.base's Wanderer carries no food) for the loot repros.
// --scenario-file PATH (test fixture, docs/reference/SCENARIO_IMPORT.md): instead of --scenario, the characters, clock and weather of a KSCEN file
// (tools/ksave_scenario.py makes one from an original save) in the default world; the world squads inside its radius are removed. The file's
// player squads become the client's control group (as NEW_GAME's would). The pristine snapshot is the bare world (taken BEFORE the file is
// applied), so order 84 new_world gives the bare world and SCENARIO_LOAD (order 103) can place another file. The AI starts fresh.
// --scenarios DIR (task CE, docs/reference/PLAYTEST.md): the directory SCENARIO_LIST (102) lists and SCENARIO_LOAD places from; the default is
// the repository's `scenarios/` (the CMake define), "" = the scenario frames answer an empty list / a refusal. Script names PLAYER / ENEMY = the first player / the first other character, P1.. / E1.. in file order.
// --dump-snapshot runs the scenario for N ticks (default 6) without a socket and writes every
// record the server would send as key/line text; --dump-bin writes the same stream as frames.
// --dump-script FILE: orders to feed, one per line: `TICK KIND ARGS` with the names PLAYER and
// ENEMY for the duel's ids (move PLAYER x z [y] | attack PLAYER ENEMY | stop PLAYER hold |
// drive PLAYER 0|1|2 heading | block PLAYER 0|1 | sneak PLAYER 0|1 | follow PLAYER LEADER | view x z | speed v | pause 0|1 | interest x z r); TICK counts from the scenario start (1 = the first tick); it is the tick the order
// takes effect at.
//
// Process-wide state noticed while moving main() here (a list, not an audit; nothing was changed for it):
//   - g_stop and the SIGINT / SIGTERM handlers stay in tools/ksim_server.cpp; runServer takes the stop flag and never installs a handler
//     (config.beforeRun is where the process does).
//   - kutil::crashlog (static state, signal handlers) is installed by the process; runServer reaches it only through config.onTick.
//   - std::getenv("HOME") for ~/.local/share/ksim/move_diverged.log (runServer) and, in main(), the crash directory default.
//   - the move_diverged.log file itself: appended by every server in the process.
//   - std::cout / std::cerr: now behind config.out / config.log.
//   - kNewGameFaction below: a namespace-scope const string, built at static init (read-only).
//   - the library internals runServer calls (Simulation, MeshNavigator's native cache under $XDG_CACHE_HOME, enableServerAi, the
//     debugview sinks, SimLinkServer's socket) were not looked at; two servers in one process are untested.

#include "ksim/ai/behaviours.hpp"
#include "ksim/core/cli.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simdata.hpp"
#if KSIM_DEBUG_VIEW
#include "ksim/debugview/decisions.hpp"
#endif
#include "ksim/dialogue/dialogue_book.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_saves.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/save/scenario_run.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/server_main.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
// A release build (KSIM_BAKE_DATA_PATH=OFF, kenshi-379.13): the game data dir arrives at runtime, --data from the client.
#define KENSHI_DATA ""
#endif
#ifndef KSIM_SCENARIOS_DIR
#define KSIM_SCENARIOS_DIR "" // a KSIM_DEV=OFF build has no default scenario directory (kenshi-379.13.3)
#endif

namespace {

using namespace ksim;

const std::string kNewGameFaction = "Nameless"; // --scenario newgame: the faction name (the client's NEW_GAME default)

struct ScriptLine
{
	std::uint64_t tick = 0;
	std::vector<std::uint8_t> frame; // the client's order frame, ready for Session::onFrame
};

// Builds the client's order frame for one script line.
std::vector<std::uint8_t> orderFrame(std::uint32_t seq, const std::string& line,
	const std::map<std::string, std::uint64_t>& names, std::uint64_t& tick)
{
	std::istringstream in(line);
	std::string kind;
	in >> tick >> kind;
	const auto id = [&](const std::string& name) -> std::uint64_t
	{
		const auto found = names.find(name);
		return found != names.end() ? found->second : std::stoull(name);
	};
	std::vector<std::uint8_t> bytes;
	simlink::BinaryWriter w(bytes);
	w.begin(simlink::kOrder);
	w.u32("clientSeq", seq);
	std::string a;
	std::string b;
	float x = 0;
	float z = 0;
	float r = 0;
	int v = 0;
	if (kind == "move")
	{
		in >> a >> x >> z;
		float y = 0;
		const bool yValid = static_cast<bool>(in >> y); // an optional click height
		w.u16("kind", 1);
		w.u16("n", 1);
		w.u64("id", id(a));
		w.f32("x", x);
		w.f32("z", z);
		w.f32("y", yValid ? y : 0.0f);
		w.u8("flags", yValid ? simlink::kMoveYValid : 0);
	} else if (kind == "attack") {
		in >> a >> b;
		w.u16("kind", 2);
		w.u16("n", 1);
		w.u64("id", id(a));
		w.u64("target", id(b));
	} else if (kind == "stop") {
		in >> a >> v;
		w.u16("kind", 4);
		w.u16("n", 1);
		w.u64("id", id(a));
		w.u8("hold", static_cast<std::uint8_t>(v));
	} else if (kind == "drive") {
		in >> a >> v >> x;
		w.u16("kind", 20);
		w.u64("id", id(a));
		w.u8("drive", static_cast<std::uint8_t>(v));
		w.f32("heading", x);
	} else if (kind == "block") {
		in >> a >> v;
		w.u16("kind", 10);
		w.u16("n", 1);
		w.u64("id", id(a));
		w.u8("which", 0);
		w.u8("value", static_cast<std::uint8_t>(v));
	} else if (kind == "sneak") {
		in >> a >> v;
		w.u16("kind", 10);
		w.u16("n", 1);
		w.u64("id", id(a));
		w.u8("which", 1);
		w.u8("value", static_cast<std::uint8_t>(v));
	} else if (kind == "follow") {
		in >> a >> b;
		w.u16("kind", 3);
		w.u16("n", 1);
		w.u64("id", id(a));
		w.u64("target", id(b));
	} else if (kind == "view") {
		in >> x >> z;
		w.u16("kind", 51);
		w.f32("x", x);
		w.f32("z", z);
	} else if (kind == "speed") {
		in >> x;
		w.u16("kind", 41);
		w.f32("speed", x);
	} else if (kind == "pause") {
		in >> v;
		w.u16("kind", 40);
		w.u8("paused", static_cast<std::uint8_t>(v));
	} else if (kind == "interest") {
		in >> x >> z >> r;
		w.u16("kind", 50);
		w.f32("x", x);
		w.f32("z", z);
		w.f32("radius", r);
	} else {
		throw std::runtime_error("script: unknown order '" + kind + "' in: " + line);
	}
	w.end();
	return bytes;
}

// The usage text; always the usage-error exit code.
int usage(std::ostream& err)
{
	err << "OpenKenshi server\nusage: OpenKenshi-server --listen HOST:PORT [--data DIR] [--sim-data DIR] [--scenario duel|duel_katana|duel_armour|squad|newgame [--start SID] [--stealth N]] [--seed N] [--no-world] [--follow-heading-fix [on|off]] [--fix-extra-attack-slots on|off] [--no-runaway-skills on|off]"
		" [--run-without-clients] [--bake DIR [--regions RX0 RZ0 RX1 RZ1]] [--navmesh FILE | --navworld DIR] [--no-nav-cache] [--no-ai] [--ai-routes] [--debug-view] [--trace-member ID]... [--token-speeds on|off] [--squad-limit N]"
		" [--dismemberment 0|1|2] [--dialogue 0|1] [--log-world-squads N] [--place-squad TEMPLATE X Z]... [--force-dialogue CHARACTER DIALOGUE] [--own-container ID]... [--money N] [--give-item ITEM [COUNT]]... [--record FILE] [--order-log FILE] [--mods FILE] [--save-dir DIR] [--crash-dir DIR]"
		" [--scenario-file PATH] [--scenarios DIR] [--combat-retarget-hold SECONDS] [--turret-manning on|off] [--offscreen-medical on|off] [--offscreen-fights on|off] [--campaign-victory-ai on|off] [--npc-raids on|off] [--dialogue-pause hold|freeze]\n"
		"       OpenKenshi-server --dump-snapshot FILE [--dump-bin FILE] [--dump-ticks N] [--dump-script FILE]"
		" [--seed N] [--no-world] [--squad-limit N]\n";
	return 2;
}

} // namespace

namespace ksim {

ServerConfig::ServerConfig() : scenarios(KSIM_SCENARIOS_DIR) {}

int parseServerArgs(int argc, char** argv, ServerConfig& options)
{
	std::ostream& err = options.log != nullptr ? *options.log : std::cerr;
	try
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--version")
			{
				// stdout, not the log: a script reads it.
				(options.log != nullptr ? *options.log : std::cout) << "OpenKenshi server " << simlink::buildHash() << "\n";
				return 0;
			}
			if (arg == "--wire-version")
			{
				// The SIMLINK kProtocol alone, for a deploy script that pairs a server binary with a client (SIMLINK.md "Protocol version").
				(options.log != nullptr ? *options.log : std::cout) << simlink::kProtocol << "\n";
				return 0;
			}
			if (arg == "--help" || arg == "-h")
			{
				usage(err);
				return 0;
			}
			if (arg == "--data" && hasValue)
				options.data = argv[++i];
			else if (arg == "--sim-data" && hasValue)
				options.simData = argv[++i];
			else if (arg == "--listen" && hasValue)
				options.listen = argv[++i];
			else if (arg == "--scenario" && hasValue)
				options.scenario = argv[++i];
			else if (arg == "--start" && hasValue)
				options.start = argv[++i];
			else if (arg == "--stealth" && hasValue)
				options.stealth = std::stod(argv[++i]);
			else if (arg == "--scenario-file" && hasValue)
				options.scenarioFile = argv[++i];
			else if (arg == "--scenarios" && hasValue)
				options.scenarios = argv[++i];
			else if (arg == "--combat-retarget-hold" && hasValue)
			{
				options.retargetHold = std::stod(argv[++i]);
				if (!(*options.retargetHold >= 0.0 && *options.retargetHold <= 10.0))
					throw std::runtime_error("--combat-retarget-hold takes 0..10 seconds");
			}
			else if (arg == "--turret-manning" && hasValue)
				options.turretManning = parseOnOff(argv[++i], arg);
			else if (arg == "--offscreen-medical" && hasValue)
				options.offscreenMedical = parseOnOff(argv[++i], arg);
			else if (arg == "--offscreen-fights" && hasValue)
				options.offscreenFights = parseOnOff(argv[++i], arg);
			else if (arg == "--mods" && hasValue)
				options.mods = argv[++i];
			else if (arg == "--seed" && hasValue)
				options.seed = std::stoull(argv[++i]);
			else if (arg == "--follow-heading-fix")
			{
				// Bare = on (the old form); an on|off value may follow (the client's options table, kenshi-h2v.22.7).
				options.followHeadingFix = true;
				if (hasValue && (std::string_view(argv[i + 1]) == "on" || std::string_view(argv[i + 1]) == "off"))
					options.followHeadingFix = std::string_view(argv[++i]) == "on";
			}
			else if (arg == "--fix-extra-attack-slots" && hasValue)
				options.fixExtraAttackSlots = parseOnOff(argv[++i], arg);
			else if (arg == "--no-runaway-skills" && hasValue)
				options.noRunawaySkills = parseOnOff(argv[++i], arg);
			else if (arg == "--run-without-clients")
				options.runWithoutClients = true;
			else if (arg == "--no-world")
				options.noWorld = true;
			else if (arg == "--no-ai")
				options.noAi = true;
			else if (arg == "--bake" && hasValue)
				options.bake = argv[++i];
			else if (arg == "--navmesh" && hasValue)
				options.navmesh = argv[++i];
			else if (arg == "--navworld" && hasValue)
				options.navworld = argv[++i];
			else if (arg == "--native-nav")
				options.nativeNav = true;
			else if (arg == "--ai-routes")
				options.aiRoutes = true;
			else if (arg == "--debug-view")
				options.debugView = true;
			else if (arg == "--token-speeds" && hasValue)
				options.tokenSpeeds = std::string(argv[++i]) == "on";
			else if (arg == "--npc-raids" && hasValue)
				options.npcRaids = parseOnOff(argv[++i], arg);
			else if (arg == "--campaign-victory-ai" && hasValue)
				options.campaignVictoryAi = std::string(argv[++i]) == "on";
			else if (arg == "--dialogue-pause" && hasValue)
			{
				const std::string value = argv[++i];
				if (value != "hold" && value != "freeze")
					throw std::runtime_error("--dialogue-pause takes hold or freeze");
				options.dialogueFreeze = value == "freeze";
			}
			else if (arg == "--trace-member" && hasValue)
				options.traceMembers.push_back(std::stoull(argv[++i]));
			else if (arg == "--no-nav-cache")
				options.navCache = false;
			else if (arg == "--log-world-squads" && hasValue)
			{
				options.logWorldSquads = std::stoull(argv[++i]);
				if (options.logWorldSquads == 0)
					return usage(err);
			}
			else if (arg == "--place-squad" && i + 3 < argc)
			{
				ServerConfig::PlaceSquad place;
				place.templateSid = argv[++i];
				place.x = std::stod(argv[++i]);
				place.z = std::stod(argv[++i]);
				options.placeSquads.push_back(std::move(place));
			}
			else if (arg == "--own-container" && hasValue)
				options.ownContainers.push_back(std::stoull(argv[++i]));
			else if (arg == "--money" && hasValue)
				options.money = std::stoll(argv[++i]);
			else if (arg == "--give-item" && hasValue)
			{
				ServerConfig::GiveItem item;
				item.itemSid = argv[++i];
				// [count]: an optional number right after the SID (a following flag is not a count).
				if (i + 1 < argc && argv[i + 1][0] != '-')
					item.count = static_cast<std::int32_t>(std::stol(argv[++i]));
				options.giveItems.push_back(std::move(item));
			}
			else if (arg == "--regions" && i + 4 < argc)
			{
				for (int& region : options.regions)
					region = std::stoi(argv[++i]);
			} else if (arg == "--squad-limit" && hasValue)
				options.squadLimit = std::stoull(argv[++i]);
			else if (arg == "--force-dialogue" && i + 2 < argc)
			{
				options.forceCharacter = argv[++i];
				options.forceDialogue = argv[++i];
			}
			else if (arg == "--dialogue" && hasValue)
				options.dialogue = std::stoi(argv[++i]) != 0;
			else if (arg == "--dismemberment" && hasValue)
			{
				options.dismemberment = std::stoi(argv[++i]);
				if (options.dismemberment < 0 || options.dismemberment > 2)
					throw std::runtime_error("--dismemberment takes 0, 1 or 2");
			} else if (arg == "--record" && hasValue)
				options.record = argv[++i];
			else if (arg == "--order-log" && hasValue)
				options.orderLog = argv[++i];
			else if (arg == "--save-dir" && hasValue)
				options.saveDir = argv[++i];
			else if (arg == "--crash-dir" && hasValue)
				options.crashDir = argv[++i];
			else if (arg == "--dump-snapshot" && hasValue)
				options.dumpSnapshot = argv[++i];
			else if (arg == "--dump-bin" && hasValue)
				options.dumpBin = argv[++i];
			else if (arg == "--dump-ticks" && hasValue)
				options.dumpTicks = std::stoull(argv[++i]);
			else if (arg == "--dump-script" && hasValue)
				options.dumpScript = argv[++i];
			else
				return usage(err);
		}
	}
	catch (const std::exception&)
	{
		return usage(err);
	}
	if (options.listen.empty() == options.dumpSnapshot.empty())
		return usage(err); // exactly one of --listen and --dump-snapshot
	if (!options.scenarioFile.empty() && options.noWorld)
	{
		err << "ksim_server: --scenario-file needs the default world (not --no-world)\n";
		return 2;
	}
	if (options.scenarioFile.empty() && options.scenario != "duel" && options.scenario != "duel_katana" && options.scenario != "duel_armour" && options.scenario != "squad"
		&& options.scenario != "newgame")
	{
		err << "ksim_server: unknown scenario '" << options.scenario << "' (known: duel, duel_katana, duel_armour, squad, newgame)\n";
		return 2;
	}
	return -1;
}

int runServer(const ServerConfig& options, std::atomic<bool>& stop)
{
	std::ostream& err = options.log != nullptr ? *options.log : std::cerr;
	std::ostream& out = options.out != nullptr ? *options.out : std::cout;
	try
	{
		using Wall = std::chrono::steady_clock;
		const auto start = Wall::now();
		const auto seconds = [&]() { return std::chrono::duration<double>(Wall::now() - start).count(); };
		// --data, else the build's KENSHI_DATA (dev and test builds); a release build has neither and says so (kenshi-379.13).
		const std::string dataDir = !options.data.empty() ? options.data : std::string(KENSHI_DATA);
		if (dataDir.empty())
			throw std::runtime_error("no game data directory: the client passes --data DIR, or start ksim_server with it");

		// The sim data (behaviour/, the AI table): --sim-data, else simdata/ beside the executable, else a dev build's source tree.
		if (!options.simData.empty())
			setSimDataDir(options.simData);
		if (const std::string problem = simDataError(); !problem.empty())
			throw std::runtime_error(problem);
		err << "ksim_server: sim data " << simDataDir() << "\n";

		const kworld::LoadOrder order = kworld::resolveLoadOrder(dataDir, options.mods);
		for (const std::string& missing : order.missing)
			throw std::runtime_error("--mods lists a missing file: " + missing);
		simlink::ServerContext context;
		if (options.dumpSnapshot.empty())
		{
			// Fail before the slow world build when the directory is unusable. The install is the data dir's parent.
			std::filesystem::path data(dataDir);
			if (!data.has_filename())
				data = data.parent_path();
			context.saveDir = simlink::prepareSaveDir(options.saveDir.empty() ? simlink::defaultSaveDir() : options.saveDir,
				{data.parent_path().string()});
			err << "ksim_server: save directory " << context.saveDir << "\n";
		}
		context.scenariosDir = options.scenarios; // task CE: SCENARIO_LIST and SCENARIO_LOAD read it
		if (!options.scenarios.empty())
			err << "ksim_server: scenarios " << options.scenarios << "\n";
		context.dataHash = simlink::dataHash(order);
		context.squadLimit = options.squadLimit;
		context.dialogueFreeze = options.dialogueFreeze;
		err << "ksim_server: dialogue pause " << (options.dialogueFreeze ? "freeze (--dialogue-pause freeze: the tick stops while a menu is open)" : "hold (default: real time, the two parties held)") << "\n";
		err << "ksim_server: data hash " << std::hex << context.dataHash << std::dec << " ("
			<< order.files.size() << " files, " << seconds() << " s)\n";

		ksim::SpawnOptions spawnOptions;
		spawnOptions.seed = options.seed;
		Simulation sim = options.noWorld ? Simulation::load(dataDir, options.seed)
			: Simulation::newGame(dataDir, order, spawnOptions);
		if (options.noWorld)
			sim.setClockTick(tickAt(kNewGameDay, kNewGameHour, kNewGameMinute)); // a new game starts at 10:00; midnight is night
		context.gameData = sim.gameData();
		if (context.gameData == nullptr)
			context.gameData = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		sim.setGameData(context.gameData);
		sim.enableWeather(*context.gameData); // a no-op for a new game (already on); --no-world gets the regions here
		for (const ServerConfig::PlaceSquad& place : options.placeSquads)
		{
			if (options.noWorld)
				throw std::runtime_error("--place-squad needs the default world (not --no-world)");
			const flecs::entity squad = sim.placeWildernessSquad(place.templateSid, place.x, place.z);
			err << "ksim_server: placed squad " << squad.get<state::Id>()->value << " '" << place.templateSid << "' den at ("
				<< place.x << ", " << place.z << ")\n";
		}
		// --own-container: the world's containers take the player faction before the pristine snapshot (every NEW_GAME keeps
		// them); an id the world does not have is retried after the scenario spawns (the duel's boxes: gone after a NEW_GAME).
		const kdata::Record* nameless = nullptr;
		for (const kdata::Record* record : context.gameData->byType(kworld::itemTypeByName("FACTION")))
		{
			if (record->name == "Nameless")
				nameless = record;
		}
		std::vector<std::uint64_t> unowned; // --own-container ids not found yet
		const auto ownContainers = [&](const std::vector<std::uint64_t>& wanted, const char* when)
		{
			unowned.clear();
			for (const std::uint64_t want : wanted)
			{
				bool found = false;
				sim.world().ecs().each([&](const state::Id& id, state::Container& box)
				{
					if (id.value != want)
						return;
					box.ownerSid = nameless->stringID;
					++box.version;
					found = true;
				});
				if (found)
					err << "ksim_server: container " << want << " belongs to the player faction (" << nameless->stringID << ", " << when << ")\n";
				else
					unowned.push_back(want);
			}
		};
		if (!options.ownContainers.empty())
		{
			if (nameless == nullptr)
				throw std::runtime_error("--own-container: no FACTION record \"Nameless\" in the data");
			ownContainers(options.ownContainers, "world");
		}
		if (options.dumpSnapshot.empty())
			sim.markPristine(); // order 84 new_world returns to this world: the data's instances, no player faction, no scenario
		// Task CE: the pristine world is the bare one even for --scenario-file, so SCENARIO_LOAD (103) can place another file on it.
		err << "ksim_server: world ready (" << seconds() << " s)\n";

		const std::shared_ptr<const CombatData> data = CombatData::build(*context.gameData, dataDir);
		CombatSettings settings;
		settings.substeps = 2;
		settings.squadFollowHeadingFix = options.followHeadingFix;
		settings.fixExtraAttackSlots = options.fixExtraAttackSlots;
		settings.noRunawaySkills = options.noRunawaySkills;
		err << "ksim_server: options combat.fix_extra_attack_slots " << (settings.fixExtraAttackSlots ? "on" : "off (the original)")
			<< ", xp.no_runaway_skills " << (settings.noRunawaySkills ? "on" : "off (the original)")
			<< ", squad.follow_heading_fix " << (settings.squadFollowHeadingFix ? "on" : "off (the original)") << "\n";
		settings.limbLossLevel = options.dismemberment;
		settings.retargetHold = options.retargetHold.value_or(settings.retargetHold);
		context.retargetHold = options.retargetHold;
		context.turretManning = options.turretManning;
		sim.setTurretManning(options.turretManning.value_or(false));
		sim.setOffscreenMedical(options.offscreenMedical);
		sim.setOffscreenFights(options.offscreenFights);
		err << "ksim_server: combat.retarget_hold_s " << settings.retargetHold << (options.retargetHold ? " (--combat-retarget-hold)" : " (default: the exe)") << "\n";
		settings.newGameStealth = options.stealth;
		sim.enableCombat(data, settings);
		sim.setNewGameMoney(options.money);
		if (!options.giveItems.empty())
		{
			const std::int32_t itemType = static_cast<std::int32_t>(kworld::itemTypeByName("ITEM"));
			std::vector<std::pair<std::string, std::int32_t>> items;
			for (const ServerConfig::GiveItem& give : options.giveItems)
			{
				const kdata::Record* record = context.gameData->find(give.itemSid);
				if (record == nullptr || record->type != itemType)
					throw std::runtime_error("--give-item: no ITEM record " + give.itemSid);
				if (give.count < 1)
					throw std::runtime_error("--give-item: the count must be at least 1");
				items.emplace_back(give.itemSid, give.count);
				err << "ksim_server: every new game's player characters get " << give.count << " x '" << record->name << "' ("
					<< give.itemSid << ")\n";
			}
			sim.setNewGameItems(std::move(items));
		}
		if (options.dialogue)
		{
			sim.enableDialogue(DialogueBook::build(context.gameData));
			sim.setReplyMenus(true); // the server ends a menu nobody answers (the disconnect grace), so one may open
			if (!options.forceDialogue.empty())
			{
				const std::uint32_t forced = sim.dialogue()->book().dialogueIndex(options.forceDialogue);
				if (forced == kNoIndex)
					throw std::runtime_error("--force-dialogue: no DIALOGUE record " + options.forceDialogue);
				sim.dialogue()->force(options.forceCharacter, forced, [&err](const std::string& line) { err << "ksim_server: " << line << '\n'; });
			}
			err << "ksim_server: dialogue book ready (" << seconds() << " s)\n";
		}
		// The duel takes orders, so it needs a navmesh: no navmesh, no nav (SIMLINK.md section 12).
		// The default is the runtime mesh over the shipped navtiles; --navmesh reads a knav_bake cache, --navworld a world bake.
		if ((!options.navworld.empty() || options.nativeNav) && !options.navmesh.empty())
			throw std::runtime_error("--navworld/--native-nav and --navmesh exclude each other");
		if (!options.navworld.empty() && options.nativeNav)
			throw std::runtime_error("--navworld and --native-nav exclude each other");
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		const bool native = options.navworld.empty() && options.navmesh.empty();
		sim.enableNavigation(native ? MeshNavigator::openNative(dataDir, hubZones,
			[&err](const std::string& line) { err << "ksim_server: " << line << "\n"; }, context.gameData.get(),
			NativeCacheOptions{ {}, options.navCache, context.dataHash })
			: !options.navworld.empty() ? MeshNavigator::openWorld(options.navworld, hubZones, dataDir)
			: MeshNavigator::load(options.navmesh, dataDir));
		err << "ksim_server: navmesh " << (native ? "streaming the shipped navtiles"
			: !options.navworld.empty() ? "streaming from " + options.navworld : "loaded") << " ("
			<< sim.navigation()->mesh().tiles().size() << " tiles, " << sim.navigation()->mesh().polyCount()
			<< " polys, " << seconds() << " s since start)\n";
		if (native)
		{
			// The original's stale-tile check: what shipped as is, what our bake replaced, and what could not be verified
			// (docs/formats/NAVMESH_CACHE.md "The stale-tile check in the loader").
			const MeshNavigator::NativeStaleStats& stale = sim.navigation()->nativeStaleStats();
			err << "ksim_server: native stale check: zones " << stale.zonesShipped << " shipped, " << stale.zonesRebaked
				<< " rebaked (stale hash), " << stale.zonesNotComputable << " unverifiable; interiors "
				<< stale.interiorsShipped << " shipped, " << stale.interiorsRebaked << " rebaked (stale hash), "
				<< stale.interiorsNotComputable << " unverifiable; check " << stale.checkMs << " ms, bake " << stale.bakeMs << " ms\n";
		}
		context.sendAiRoutes = options.aiRoutes;
		context.debugView = options.debugView; // the view itself answers only a loopback peer (Session::setDebugPeer)
		if (options.logWorldSquads != 0)
			sim.setWorldSquadLog(options.logWorldSquads, [&err](const std::string& line) { err << "ksim_server: " << line << "\n"; });
		// The capture chain always logs (task BR2): the client cannot see a strip, a restrain or a carry start, so a live repro
		// needs the server's own line. Off only in --dump-snapshot (a fixture has no reader).
		if (options.dumpSnapshot.empty())
			sim.setTaskLog([&err](const std::string& line) { err << "ksim_server: " << line << "\n"; });
		if (!options.traceMembers.empty() || options.debugView)
		{
			const std::set<std::uint64_t> traced(options.traceMembers.begin(), options.traceMembers.end());
			const auto log = [&err, traced](const trace::Pick& pick)
			{
				if (traced.count(pick.member) != 0)
					err << "ksim_server: " << trace::formatPick(pick) << "\n";
			};
#if KSIM_DEBUG_VIEW
			if (options.debugView)
				sim.setDecisionTrace(debugview::traceSink({}, log)); // every member's ring, the log for the traced ones
			else
#endif
				sim.setDecisionTrace({[traced](std::uint64_t member) { return traced.count(member) != 0; }, log});
		}
		// move_diverged lines (the click vs the route's end, simlink.cpp) also go to ~/.local/share/ksim/move_diverged.log: demo.sh
		// deletes the server log on a clean exit, and a player who saw a wrong walk needs the line afterwards (user 2026-11-08).
		std::string divergeLog;
		if (const char* home = std::getenv("HOME"))
			divergeLog = std::string(home) + "/.local/share/ksim/move_diverged.log";
		context.log = [&err, divergeLog](const std::string& line)
		{
			err << "ksim_server: " << line << "\n";
			if (!divergeLog.empty() && line.rfind("move_diverged ", 0) == 0)
			{
				std::error_code ec;
				std::filesystem::create_directories(std::filesystem::path(divergeLog).parent_path(), ec);
				std::ofstream(divergeLog, std::ios::app) << std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) << " " << line << "\n";
			}
		};
		std::map<std::string, std::uint64_t> names; // the script's names for the scenario's characters
		if (!options.scenarioFile.empty())
		{
			const scenario::File file = scenario::parseFile(options.scenarioFile);
			const scenario::Placed placed = scenario::apply(sim, *context.gameData, *data, context, file, options.seed);
			err << "ksim_server: scenario file " << options.scenarioFile << " (save " << file.save << "): " << file.chars.size() << " characters ("
				<< placed.players.size() << " player-controlled), " << placed.removedSquads << " world squads removed, clock day " << file.day << " "
				<< file.hour << ":" << file.minute << (file.weatherGroup.empty() ? "" : ", weather " + file.weather) << "\n";
			for (const std::string& note : placed.notes)
				err << "ksim_server: scenario: " << note << "\n";
			for (std::size_t i = 0; i < placed.players.size(); ++i)
				names["P" + std::to_string(i + 1)] = placed.players[i];
			for (std::size_t i = 0; i < placed.others.size(); ++i)
				names["E" + std::to_string(i + 1)] = placed.others[i];
			names["PLAYER"] = placed.players.empty() ? 0 : placed.players.front();
			names["ENEMY"] = placed.others.empty() ? 0 : placed.others.front();
		} else if (options.scenario == "squad") {
			const simlink::Squad squad = simlink::spawnSquad(sim, *context.gameData, *data, context);
			err << "ksim_server: squad: players";
			for (std::size_t i = 0; i < squad.players.size(); ++i)
			{
				names["P" + std::to_string(i + 1)] = squad.players[i];
				err << " " << squad.players[i];
			}
			err << ", enemies";
			for (std::size_t i = 0; i < squad.enemies.size(); ++i)
			{
				names["E" + std::to_string(i + 1)] = squad.enemies[i];
				err << " " << squad.enemies[i];
			}
			err << "\n";
			names["PLAYER"] = squad.players.front();
			names["ENEMY"] = squad.enemies.front();
		} else if (options.scenario == "newgame") {
			// The first tick applies it, like a client's NEW_GAME (a logged command: replay and save see it).
			if (const std::string reason = sim.checkNewGame(options.start, kNewGameFaction, {}); !reason.empty())
				throw std::runtime_error("--scenario newgame: start " + options.start + " refused: " + reason);
			Command command = Command::newGame(options.start, kNewGameFaction, 0);
			command.tick = sim.clockTick() + 1;
			sim.enqueue(command);
			err << "ksim_server: newgame: start " << options.start << " at tick " << command.tick << "\n";
		} else {
			const simlink::Duel duel = simlink::spawnDuel(sim, *context.gameData, *data, context,
				options.scenario == "duel_katana", true, options.scenario == "duel_armour");
			err << "ksim_server: duel: player id " << duel.player << ", enemy id " << duel.enemy << "\n";
			names["PLAYER"] = duel.player;
			names["ENEMY"] = duel.enemy;
		}
		if (!unowned.empty())
		{
			ownContainers(std::vector<std::uint64_t>(unowned), "scenario");
			if (!unowned.empty())
			{
				std::string ids;
				sim.world().ecs().each([&](const state::Id& id, const state::Container&)
				{
					if (id.value + 64 > unowned.front() && id.value < unowned.front() + 64)
						ids += " " + std::to_string(id.value);
				});
				throw std::runtime_error("--own-container: no container with id " + std::to_string(unowned.front())
					+ " (containers within 64 ids:" + (ids.empty() ? std::string(" none") : ids) + ")");
			}
		}

		// The squad AI runs on every server (server_ai.hpp): --bake only chooses its navigator (the world bake's own streaming
		// ones), and without it the AI takes the server's navmesh. Before 2026-11-08 only --bake switched the AI on, so a server
		// over --navworld / --native-nav / the Hub bake ran every character goal-less and no scenario fight ever started.
		if (!options.noAi)
		{
			ServerAiOptions ai;
			ai.registryPath = simDataAiTable();
			ai.tokenSpeeds = options.tokenSpeeds;
			ai.victoryAi = options.campaignVictoryAi;
			ai.npcRaids = options.npcRaids;
			ai.bake = options.bake;
			ai.regionX0 = options.regions[0];
			ai.regionZ0 = options.regions[1];
			ai.regionX1 = options.regions[2];
			ai.regionZ1 = options.regions[3];
#if KSIM_DEBUG_VIEW
			if (options.debugView)
				ai.decision = debugview::decisionSink(); // the debug view's goal-pick sink
#endif
			const char* navigator = enableServerAi(sim, ai);
			err << "ksim_server: squad AI on, navigator " << navigator << " (" << seconds() << " s); ai.token_speeds "
				<< (options.tokenSpeeds ? "on (--token-speeds)" : "off (default: flat 15 u/s)") << ", world.campaign_victory_ai "
				<< (options.campaignVictoryAi ? "on (--campaign-victory-ai)" : "off (default: the original's raids go home)") << ", world.npc_raids "
				<< (options.npcRaids ? "on (--npc-raids)" : "off (default: raids target player towns only)") << "\n";
		}

		if (!options.dumpSnapshot.empty())
		{
			std::vector<ScriptLine> script;
			if (!options.dumpScript.empty())
			{
				std::ifstream in(options.dumpScript);
				if (!in)
					throw std::runtime_error("cannot read " + options.dumpScript);
				std::string line;
				std::uint32_t seq = 0;
				while (std::getline(in, line))
				{
					if (line.empty() || line[0] == '#')
						continue;
					ScriptLine entry;
					entry.frame = orderFrame(++seq, line, names, entry.tick);
					entry.tick += sim.clockTick(); // script ticks count from the scenario's start
					script.push_back(std::move(entry));
				}
			}
			std::ofstream textFile(options.dumpSnapshot);
			if (!textFile)
				throw std::runtime_error("cannot write " + options.dumpSnapshot);
			std::vector<std::uint8_t> bytes;
			simlink::Session session(sim, context);
			simlink::TextWriter text(textFile, &session.strings());
			simlink::BinaryWriter binary(bytes);
			simlink::TeeWriter tee(text, binary);
			textFile << "# ksim_server --dump-snapshot, scenario " << options.scenario << ", seed " << options.seed
				<< (options.noWorld ? ", no world" : "") << "; player id " << names["PLAYER"] << ", enemy id "
				<< names["ENEMY"] << "; one record per line, `NAME key=value ...`, lists `key=N[k=v,k=v; ...]`\n";
			session.begin(tee);
			std::vector<std::uint8_t> hello;
			simlink::BinaryWriter clientHello(hello);
			clientHello.begin(simlink::kHello);
			clientHello.u32("magic", simlink::kMagic);
			clientHello.u16("protocol", simlink::kProtocol);
			clientHello.u8("role", 0);
			clientHello.u64("dataHash", context.dataHash);
			clientHello.str("name", "dump");
			clientHello.end();
			if (!session.onFrame(simlink::kHello, hello.data() + 6, hello.size() - 6, tee))
				throw std::runtime_error("dump: the session refused its own hello");
			session.sendTick(tee, true);
			for (std::uint64_t i = 0; i < options.dumpTicks; ++i)
			{
				for (const ScriptLine& entry : script)
				{
					if (entry.tick == sim.clockTick() + 1
						&& !session.onFrame(simlink::kOrder, entry.frame.data() + 6, entry.frame.size() - 6, tee))
						throw std::runtime_error("dump: the session refused a script order");
				}
				sim.tick();
				if (options.onTick)
					options.onTick(sim.clockTick());
				session.sendTick(tee, false);
			}
			if (!options.dumpBin.empty())
			{
				std::ofstream binFile(options.dumpBin, std::ios::binary);
				binFile.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
				if (!binFile)
					throw std::runtime_error("cannot write " + options.dumpBin);
			}
			err << "ksim_server: dumped " << options.dumpTicks << " ticks, " << bytes.size() << " stream bytes\n";
			return 0;
		}

		simlink::ServerOptions serverOptions;
		const std::size_t colon = options.listen.rfind(':');
		if (colon == std::string::npos)
			return usage(err);
		serverOptions.host = options.listen.substr(0, colon);
		serverOptions.port = std::stoi(options.listen.substr(colon + 1));
		serverOptions.recordPath = options.record;
		serverOptions.runWithoutClients = options.runWithoutClients;
		serverOptions.onListening = [&out](int port)
		{
			out << "listening 127.0.0.1:" << port << std::endl;
		};
		if (options.beforeRun)
			options.beforeRun();
		simlink::SimLinkServer server(sim, context, serverOptions);
		server.listen();
		server.run(stop);
		if (!options.orderLog.empty())
		{
			std::ofstream log(options.orderLog);
			for (const simlink::OrderLogEntry& entry : context.orderLog)
			{
				log << "tick=" << entry.tick << " seq=" << entry.seq << (entry.sim ? " sim " : " nosim ")
					<< entry.text << "\n";
			}
		}
		err << "ksim_server: stopped at tick " << sim.clockTick() << ", state hash " << std::hex
			<< sim.stateHash() << std::dec << "; position corrections " << sim.combat()->positionCorrections()
			<< " (uncorrectable " << sim.combat()->uncorrectable() << "), routes blocked "
			<< sim.combat()->routesBlocked() << "\n";
	}
	catch (const std::exception& error)
	{
		err << "ksim_server: " << error.what() << "\n";
		return 1;
	}
	return 0;
}

} // namespace ksim
