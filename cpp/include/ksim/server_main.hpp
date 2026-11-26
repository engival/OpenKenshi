// ksim_server as a library: the command line (ServerConfig, parseServerArgs) and the server itself (runServer). tools/ksim_server.cpp is
// the process around it (signals, crash recorder, exit code); the options are documented in src/ksim/server_main.cpp.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace ksim {

struct ServerConfig
{
	std::string listen;
	std::string data; // --data DIR: the game data dir; empty = the build's KENSHI_DATA (dev and test builds only, kenshi-379.13)
	std::string simData; // --sim-data DIR: behaviour/ + ksim_ai_behaviours.tsv; empty = simdata/ beside the executable, else a dev build's cpp/data
	std::string scenario = "duel";
	std::string scenarioFile; // --scenario-file
	std::optional<bool> turretManning; // --turret-manning on|off: Simulation::setTurretManning, over a scenario header's
	bool offscreenMedical = false; // --offscreen-medical on|off: Simulation::setOffscreenMedical (COMBAT_OPTIONS.md 8)
	bool offscreenFights = false; // --offscreen-fights on|off: Simulation::setOffscreenFights (COMBAT_OPTIONS.md 9)
	int idleStride = 2; // --idle-stride N: CombatSettings::idleStride (arena strides: 1 = every member every step, kenshi-k16.6.636)
	std::optional<double> retargetHold; // --combat-retarget-hold S: CombatSettings::retargetHold, over a scenario header's
	std::string scenarios; // --scenarios: the playtest scenario directory; the constructor sets the repository default (dev builds; empty otherwise)
	double stealth = -1.0; // --stealth: CombatSettings::newGameStealth
	std::string start = "1980-gamedata.base"; // --start: the NEW_GAME_STARTOFF of --scenario newgame (the Wanderer)
	std::string mods;
	std::uint64_t seed = 1;
	bool noWorld = false;
	std::string bake;
	std::string navmesh; // knav_bake --out cache for the duel's navmesh; empty = bake the Hub zone at startup
	std::string navworld; // knav_bake --world DIR: the navmesh follows the players' full-detail areas; empty = --navmesh / the Hub bake
	bool nativeNav = false; // --native-nav: accepted, the default (a no-op); the same streaming over the shipped navtiles (MeshNavigator::openNative)
	bool navResident = false; // --nav-resident=world|off: load the whole world's navmesh before tick 0 (default off until the world cache is baked)
	std::string navZones; // --nav-zones SET (dev only; ksim/dev/nav_zone_set.hpp): the resident navmesh holds this zone set, not the whole world
	bool navCache = true; // --no-nav-cache: no native patch cache on disk (docs/formats/NAVMESH_CACHE.md "Patch cache")
	bool followHeadingFix = false; // squad.follow_heading_fix
	bool fixExtraAttackSlots = true; // --fix-extra-attack-slots on|off: CombatSettings::fixExtraAttackSlots (COMBAT_OPTIONS.md 1)
	bool noRunawaySkills = true; // --no-runaway-skills on|off: CombatSettings::noRunawaySkills (COMBAT_OPTIONS.md 2)
	bool runWithoutClients = false; // keep ticking with no ready session
	bool noAi = false; // --no-ai: the squad AI off (the old AI-less server, for a screenshot or a fixture)
	bool aiRoutes = false; // also send CHAR_ROUTE for characters that are not player-controlled
	std::vector<std::uint64_t> traceMembers; // --trace-member ID (repeatable): one log line per goal pick of that member
	bool tokenSpeeds = false; // --token-speeds on|off: AiSystem::Config::tokenSpeeds (ai.token_speeds, COMBAT_OPTIONS.md 7)
	bool npcRaids = false; // --npc-raids on|off: FactionTuning::npcRaids (world.npc_raids, FACTIONS.md 4)
	bool campaignVictoryAi = false; // --campaign-victory-ai on|off: AiSystem::Config::victoryAi (world.campaign_victory_ai, COMBAT_OPTIONS.md 10)
	bool dialogueFreeze = false; // --dialogue-pause hold|freeze: ServerContext::dialogueFreeze (DIALOGUE.md 13.2; default hold = real time)
	bool debugView = false; // --debug-view: the debug frames 200-219 to a loopback peer (docs/formats/SIMLINK.md "DEBUG VIEW")
	std::uint64_t logWorldSquads = 0; // game minutes between the built-world-squad dumps; 0 = off
	struct PlaceSquad
	{
		std::string templateSid;
		double x = 0.0;
		double z = 0.0;
	};
	std::vector<PlaceSquad> placeSquads; // --place-squad, in order
	std::vector<std::uint64_t> ownContainers; // --own-container
	std::int64_t money = -1; // --money
	struct GiveItem
	{
		std::string itemSid;
		std::int32_t count = 1;
	};
	std::vector<GiveItem> giveItems; // --give-item
	int regions[4] = {4, 7, 6, 9};
	std::size_t squadLimit = 0;
	std::string forceCharacter, forceDialogue; // --force-dialogue
	bool dialogue = true; // --dialogue 0|1: the dialogue engine (DIALOGUE.md slice 1: barks as SIMLINK events 41 / 42)
	int dismemberment = 1; // --dismemberment N: the game's Dismemberment setting (0 off, 1 Rare: hits only, 2 also the passive sink); CombatSettings::limbLossLevel
	std::string record;
	std::string orderLog;
	std::string saveDir;
	std::string importSave; // --import-save SAVEDIR: the world is saveimport::importSave of that original save folder (a failure ends the server)
	std::string importReport; // --import-report PATH: the import's id map as JSON, format 1 (docs/formats/IMPORT_REPORT.md)
	bool paused = false; // --paused: start with the admin pause set; the first client speed or pause order releases it
	std::string crashDir; // --crash-dir: a crash report's directory; empty = ~/.local/share/ksim/crashes
	std::string dumpSnapshot;
	std::string dumpBin;
	std::string dumpScript;
	std::uint64_t dumpTicks = 6;

	// What the embedding process supplies; none of it is a command-line option.
	std::ostream* log = nullptr; // "ksim_server: ..." lines and the usage text; null = std::cerr
	std::ostream* out = nullptr; // the `listening 127.0.0.1:PORT` line; null = std::cout
	std::function<void(std::uint64_t)> onTick; // --dump-snapshot: after each tick, with the clock tick (the crash recorder's tick)
	std::function<void()> beforeRun; // serve mode: just before the listening socket opens (the process installs its signal handlers here)

	ServerConfig();
};

// Fills `config` from the command line. Returns -1 when the server should run, else the process exit code (0 for --help, 2 for a
// usage error); the usage text and the error lines go to config.log.
int parseServerArgs(int argc, char** argv, ServerConfig& config);

// Builds the world and serves (--listen) or dumps a snapshot (--dump-snapshot) until `stop` is set; returns the exit code (0, or 1
// after an error line on config.log, or 2 for a usage error). Never calls exit(), installs no handler and reads no global.
int runServer(const ServerConfig& config, std::atomic<bool>& stop);

} // namespace ksim
