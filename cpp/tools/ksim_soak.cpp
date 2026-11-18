// ksim_soak: runs the squad AI on the default world for N game-days and
// reports the soak metrics (include/ksim/soak.hpp).
//
//   ksim_soak --bake DIR [--regions RX0 RZ0 RX1 RZ1] [--days N] [--seed N]
//             [--graph FILE] [--workers N] [--log N] [--save PATH]
//             [--import-save SAVEDIR] [--csv BASE] [--token-speeds on|off] [--offscreen-medical on|off] [--offscreen-fights on|off] [--campaign-victory-ai on|off] [--npc-raids on|off] [--player-nav on|off]
//
// --csv BASE writes the living-world CSVs, one row per game-hour: BASE.csv (world totals) and BASE.factions.csv (per faction) and BASE.player.csv (each player squad member).
// --import-save SAVEDIR starts from an original Kenshi save (ksim/save_import.hpp, SAVE_IMPORT.md) instead of a new game's world: the squads and
// the world state as the save has them, at the save's clock; the CSV hours count from there. The combat data is loaded for it.
// --token-speeds is the living-world setting ai.token_speeds (COMBAT_OPTIONS.md 7), --offscreen-medical world.offscreen_medical (8); one
// flag per such setting, off by default.
// --player-nav on gives the sim the streaming world mesh (MeshNavigator::openWorld on the bake, following the player's areas) that routes the player's
// own members: without it nothing walks a player character, so an imported player order (SAVE_IMPORT.md "Player orders") waits for ever. Off by default.
//
// DIR is a `knav_bake --world DIR` bake; the regions (default 4 7 6 9, the
// ctest world_sample: the Hub and its neighbours) are loaded and a coarse
// graph is built over them. Squads outside that window run the generic
// fallback, logged. Exit status 1 when any sample had a squad with no goal.
//
// The whole world: --regions 0 0 15 15 --graph FILE, FILE a `knav_graph` file
// of the same bake (the swimmer or the dry-only graph; the graph is read, not
// built). With no player area every squad runs at Squad/Strategic level.

#include "ksim/behaviours.hpp"
#include "ksim/cli.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/dialogue_book.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/save_import.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"
#include "ksim/soak.hpp"

#include "kworld/loadorder.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int usage()
{
	std::cerr << "usage: ksim_soak --bake DIR [--regions RX0 RZ0 RX1 RZ1] [--days N]"
		" [--graph FILE] [--seed N] [--workers N] [--log N] [--save PATH] [--no-factions]"
		" [--raid-frequency X] [--no-doors] [--provoke HOURS] [--economy] [--import-save SAVEDIR] [--csv BASE] [--token-speeds on|off] [--offscreen-medical on|off] [--offscreen-fights on|off] [--campaign-victory-ai on|off] [--npc-raids on|off] [--player-nav on|off] [--profile-json SECONDS (KSIM_PROFILE builds)]\n";
	return 2;
}

// Peak resident set (VmHWM) in MB; 0 where /proc is not there.
long peakMemoryMb()
{
	std::ifstream status("/proc/self/status");
	std::string key;
	while (status >> key)
	{
		if (key == "VmHWM:")
		{
			long kilobytes = 0;
			status >> kilobytes;
			return kilobytes / 1024;
		}
	}
	return 0;
}

} // namespace

int main(int argc, char** argv)
{
	std::string bake;
	std::string savePath;
	std::string graphPath;
	std::string csvBase;
	std::string importPath;
	bool tokenSpeeds = false;
	bool offscreenMedical = false;
	bool playerNav = false;
	bool dialogue = false; // --dialogue on: the dialogue system runs (the server's does; a soak's did not)
	std::string eventLogPath; // --event-log FILE: every spoken line aimed at a player character and the kidnap / torture log lines
	bool offscreenFights = false;
	bool campaignVictoryAi = false;
	bool npcRaids = false;
	std::string table = KSIM_AI_TABLE;
	int regions[4] = {4, 7, 6, 9};
	double days = 10.0;
	std::uint64_t seed = 1;
	bool perTemplate = false;
	bool factions = true;
	bool doors = true;
	bool economy = false;
	std::uint64_t provoke = 0;
	double raidFrequency = 1.0;
	double population = 1.0;
	double nests = 1.0;
	int workers = 1;
	std::size_t logLines = 12;
	double profileJson = 0.0; // seconds between JSON profile lines on stderr, 0 = off
	try
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc;
			if (arg == "--bake" && hasValue)
				bake = argv[++i];
			else if (arg == "--days" && hasValue)
				days = std::stod(argv[++i]);
			else if (arg == "--per-template")
				perTemplate = true;
			else if (arg == "--no-factions")
				factions = false;
			else if (arg == "--provoke" && hasValue)
				provoke = std::stoull(argv[++i]);
			else if (arg == "--no-doors")
				doors = false;
			else if (arg == "--economy")
				economy = true;
			else if (arg == "--raid-frequency" && hasValue)
				raidFrequency = std::stod(argv[++i]);
			else if (arg == "--population" && hasValue)
				population = std::stod(argv[++i]);
			else if (arg == "--nests" && hasValue)
				nests = std::stod(argv[++i]);
			else if (arg == "--seed" && hasValue)
				seed = std::stoull(argv[++i]);
			else if (arg == "--workers" && hasValue)
				workers = std::stoi(argv[++i]);
			else if (arg == "--log" && hasValue)
				logLines = std::stoull(argv[++i]);
			else if (arg == "--graph" && hasValue)
				graphPath = argv[++i];
			else if (arg == "--save" && hasValue)
				savePath = argv[++i];
			else if (arg == "--import-save" && hasValue)
				importPath = argv[++i];
			else if (arg == "--csv" && hasValue)
				csvBase = argv[++i];
			else if (arg == "--token-speeds" && hasValue)
				tokenSpeeds = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--offscreen-medical" && hasValue)
				offscreenMedical = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--dialogue" && hasValue)
				dialogue = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--event-log" && hasValue)
				eventLogPath = argv[++i];
			else if (arg == "--player-nav" && hasValue)
				playerNav = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--npc-raids" && hasValue)
				npcRaids = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--campaign-victory-ai" && hasValue)
				campaignVictoryAi = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--offscreen-fights" && hasValue)
				offscreenFights = ksim::parseOnOff(argv[++i], arg);
			else if (arg == "--profile-json" && hasValue)
				profileJson = std::stod(argv[++i]);
			else if (arg == "--table" && hasValue)
				table = argv[++i];
			else if (arg == "--regions" && i + 4 < argc)
			{
				for (int& region : regions)
					region = std::stoi(argv[++i]);
			} else {
				return usage();
			}
		}
	}
	catch (const std::exception&)
	{
		return usage();
	}
	if (bake.empty())
		return usage();

	try
	{
		using Clock = std::chrono::steady_clock;
		const auto seconds = [](Clock::time_point from)
		{
			return std::chrono::duration<double>(Clock::now() - from).count();
		};
		auto mark = Clock::now();
		ksim::RegionConfig config;
		config.workerThreads = workers;
		ksim::SpawnOptions options;
		options.seed = seed;
		options.wildernessByDensity = !perTemplate;
		options.wildernessPopulation = population;
		options.nestMultiplier = nests;
		options.economy = economy ? ksim::EconomyMode::Real : ksim::EconomyMode::Original;
		ksim::Simulation simulation = ksim::Simulation::newGame(KENSHI_DATA,
			kworld::resolveLoadOrder(KENSHI_DATA), options, nullptr, config);
		std::cout << "world: " << std::fixed << std::setprecision(1) << seconds(mark) << " s\n";
		std::shared_ptr<const ksim::CombatData> combatData;
		if (!importPath.empty() || offscreenMedical || offscreenFights)
		{
			// The imports and both off-screen settings build members from their templates (no player area in the soak otherwise, so nothing is in detail).
			combatData = ksim::CombatData::build(*simulation.gameData(), KENSHI_DATA);
			simulation.enableCombat(combatData, ksim::CombatSettings{});
		}
		if (!importPath.empty())
		{
			mark = Clock::now();
			// The streaming mesh is up before the import (opened on the Hub; the import re-covers the zones round the player's squads), so the import
			// can snap its Full members to the ground.
			if (playerNav)
				simulation.enableNavigation(ksim::MeshNavigator::openWorld(bake, {{ksim::simlink::kHubZoneX, ksim::simlink::kHubZoneZ}}, KENSHI_DATA));
			const ksim::saveimport::Report imported = ksim::saveimport::importSave(simulation, *simulation.gameData(), *combatData, importPath);
			std::cout << "import: " << importPath << ", clock day " << imported.world.day << " " << imported.world.hour << ":" << imported.world.minute << ", "
				<< imported.platoons.size() << " platoons (" << imported.generatedRemoved << " generated squads removed first), " << imported.members << " members (player " << imported.membersPlayer << ", full " << imported.membersFull
				<< ", token " << imported.membersToken << ", unplaced " << imported.unplaced << "), skipped " << imported.membersSkipped << ", " << seconds(mark) << " s\n";
		}

		mark = Clock::now();
		std::shared_ptr<ksim::KnavNavigator> navigator = graphPath.empty()
			? ksim::KnavNavigator::open(KENSHI_DATA, bake, regions[0], regions[1],
				regions[2], regions[3], 4)
			: ksim::KnavNavigator::openWithGraph(KENSHI_DATA, bake, regions[0], regions[1],
				regions[2], regions[3], graphPath);
		std::cout << "navigation: regions " << regions[0] << "," << regions[1] << " .. "
			<< regions[2] << "," << regions[3] << ", " << navigator->clusterCount()
			<< " clusters, " << seconds(mark) << " s\n";

		std::size_t townsInWindow = 0;
		std::size_t squadsInWindow = 0;
		simulation.world().ecs().each([&](const ksim::state::TownState& town)
		{
			townsInWindow += navigator->covers(town.x, town.z) ? 1 : 0;
		});
		simulation.world().ecs().each([&](const ksim::state::SquadInstance&,
			const ksim::state::Position& position)
		{
			squadsInWindow += navigator->covers(position.x, position.z) ? 1 : 0;
		});
		std::cout << "window: " << townsInWindow << " towns, " << squadsInWindow
			<< " positioned squads\n";

		std::size_t logged = 0;
		std::size_t logTotal = 0;
		ksim::AiSystem::Config ai;
		ai.registry = std::make_shared<ksim::BehaviourRegistry>(simulation.world(),
			ksim::BehaviourRegistry::readRules(table));
		ai.navigator = navigator;
		ai.tokenSpeeds = tokenSpeeds;
		ai.victoryAi = campaignVictoryAi;
		ai.factionTuning.npcRaids = npcRaids;
		if (factions)
		{
			ai.factions = ksim::FactionBook::build(simulation.gameData());
			ai.factionTuning.raidFrequency = raidFrequency;
		}
		ai.doorTuning.enabled = doors;
		if (doors)
		{
			const auto data = ksim::DoorData::fromWorld(simulation.world());
			std::size_t gates = 0;
			std::size_t locking = 0;
			for (const ksim::DoorSite& site : data->sites)
			{
				gates += site.gate ? 1 : 0;
				locking += site.gate && site.alwaysLocks ? 1 : 0;
			}
			std::cout << "doors: " << data->sites.size() << " door sites, " << gates << " on gate buildings ("
				<< locking << " always lock), " << data->privateTowns.size() << " non-public TOWN records\n";
		}
		std::ofstream eventLog;
		if (!eventLogPath.empty())
			eventLog.open(eventLogPath);
		ai.log = [&](std::string_view line)
		{
			if (eventLog && (line.find("kidnap") != std::string_view::npos || line.find("peeler") != std::string_view::npos))
				eventLog << "log: " << line << "\n";
			++logTotal;
			if (logged++ < logLines)
				std::cout << "  log: " << line << "\n";
		};
		simulation.enableAi(std::move(ai));
		if (dialogue)
			simulation.enableDialogue(ksim::DialogueBook::build(simulation.gameData()));
		if (eventLog)
		{
			simulation.setTaskLog([&](const std::string& line) { eventLog << "task " << simulation.clockTick() << ": " << line << "\n"; });
			simulation.setSpeechObserver([&](const ksim::Simulation::SpeechEvent& speech)
			{
				const ksim::state::Fighter* target = simulation.combat() != nullptr ? simulation.combat()->find(speech.target) : nullptr;
				if (!speech.end && target != nullptr && target->playerControlled)
					eventLog << "tick " << simulation.clockTick() << " speech " << speech.speaker << " -> " << speech.target << " dialogue " << speech.dialogue << " line " << speech.line << " \"" << speech.text << "\"\n";
			});
		}

		std::ofstream csv;
		std::ofstream factionCsv;
		std::ofstream playerCsv;
		ksim::SoakOptions soakOptions;
		if (!csvBase.empty())
		{
			csv.open(csvBase + ".csv");
			factionCsv.open(csvBase + ".factions.csv");
			playerCsv.open(csvBase + ".player.csv");
			if (!csv || !factionCsv || !playerCsv)
				throw std::runtime_error("cannot write " + csvBase + ".csv");
			soakOptions.csv = &csv;
			soakOptions.factionCsv = &factionCsv;
			soakOptions.playerCsv = &playerCsv;
		}
#if KSIM_PROFILE
		simulation.profiler().setJsonSeconds(profileJson);
#else
		if (profileJson > 0.0)
			throw std::runtime_error("--profile-json needs a build with -DKSIM_PROFILE=ON");
#endif
		simulation.setOffscreenMedical(offscreenMedical);
		simulation.setOffscreenFights(offscreenFights);
		std::cout << "settings: ai.token_speeds " << (tokenSpeeds ? "on" : "off") << ", world.offscreen_medical "
			<< (offscreenMedical ? "on" : "off") << ", world.offscreen_fights " << (offscreenFights ? "on" : "off") << ", world.campaign_victory_ai " << (campaignVictoryAi ? "on" : "off") << ", world.npc_raids " << (npcRaids ? "on" : "off") << "\n";
		soakOptions.days = days;
		soakOptions.provokeEveryHours = provoke;
		const ksim::SoakReport report = ksim::runSoak(simulation, soakOptions);

		std::cout << "soak: " << days << " game-days, " << report.ticks << " ticks, "
			<< report.seconds << " s, state hash " << std::hex << simulation.stateHash()
			<< std::dec << "\n";
		if (offscreenFights)
		{
			const ksim::Simulation::FightStats& fights = simulation.fightStats();
			std::cout << "offscreen fights: " << fights.fights << " fought, " << fights.meetings << " pair-steps within the radius, "
				<< fights.cooling << " cooling, " << fights.empty << " with a side empty, " << fights.tokensMax << " tokens at most, " << fights.steps << " steps\n";
		}
		std::cout << "squads " << report.last.squads << ", goal-less (worst sample) "
			<< report.maxGoalless << ", off route (worst) " << report.maxOffRoute
			<< "\n";
		std::cout << "carried leaders placed at their route's end (no routable carrot for 10 s; should be 0): "
			<< simulation.residentStats().leaderWarps << "\n";
		std::cout << "goals now: idle " << report.last.byGoal[1] << ", go home "
			<< report.last.byGoal[2] << ", travel " << report.last.byGoal[3] << ", patrol "
			<< report.last.byGoal[4] << ", roam " << report.last.byGoal[5] << "\n";
#if KSIM_PROFILE
		{
			const ksim::profile::Totals& total = report.profile;
			const ksim::profile::Summary& window = report.profileWindow;
			std::cout << "profile (KSIM_PROFILE, " << total.ticks << " ticks, " << total.tickNs / 1.0e9 << " s inside Simulation::tick; the Simlink and Save lines are outside it):\n";
			std::cout << "  section        total s   of tick   mean us/tick   window max us\n";
			for (std::size_t i = 0; i < ksim::profile::kSections; ++i)
			{
				char line[120];
				std::snprintf(line, sizeof line, "  %-13s %8.2f %8.1f%% %14.1f %15.1f\n", ksim::profile::kSectionNames[i], total.ns[i] / 1.0e9,
					total.tickNs != 0 ? 100.0 * static_cast<double>(total.ns[i]) / static_cast<double>(total.tickNs) : 0.0,
					total.ticks != 0 ? static_cast<double>(total.ns[i]) / static_cast<double>(total.ticks) / 1000.0 : 0.0, window.maxUs[i]);
				std::cout << line;
			}
			std::cout << "  last " << window.ticks << " ticks: tick us mean " << window.tickMeanUs << ", p50 " << window.tickP50Us << ", p99 " << window.tickP99Us << ", max "
				<< window.tickMaxUs << "; per tick " << window.characters << " characters, " << window.squads << " squads, " << window.pathQueries << " path queries ("
				<< window.pathUs << " us, inside the sections above)\n";
			std::cout << "  path queries over the run " << total.pathQueries << ", " << total.pathNs / 1.0e9 << " s\n";
		}
#endif
		std::cout << "wall time per game-day " << report.seconds / days << " s, peak memory "
			<< peakMemoryMb() << " MB\n";
		const ksim::NavStats& nav = navigator->stats();
		std::cout << "navigator: coarse routes " << nav.routeCalls << " (" << nav.routeFound
			<< " found) " << nav.routeNs / 1.0e9 << " s, local routes " << nav.localCalls << " ("
			<< nav.localFound << " found) " << nav.localNs / 1.0e9 << " s, snaps " << nav.snapCalls
			<< " " << nav.snapNs / 1.0e9 << " s\n";
		std::cout << "route failures: no path " << nav.routeFailures[3] << ", closed " << nav.routeFailures[4] << ", out of nodes "
			<< nav.routeFailures[5] << "; clearance nodes closed " << nav.routeClearanceNodes << "; radius-0 retries "
			<< nav.radiusZeroRetries << "\n";
		std::cout << "coarse routes answered by the connected sets (no graph search): " << nav.routeSetRefusals
			<< " (" << nav.doorRefusedRoutes << " by a gate)\n";
		std::cout << "local goals over a region border " << nav.crossCalls << " (" << nav.crossFound
			<< " found) " << nav.crossNs / 1.0e9 << " s incl. building the window; wide retries " << nav.wideRetries << " ("
			<< nav.wideFound << " found)\n";
		std::cout << "local failures: start off mesh " << nav.localFailures[1] << ", end off mesh "
			<< nav.localFailures[2] << ", no path " << nav.localFailures[3] << ", closed (island) "
			<< nav.localFailures[4] << ", node pool " << nav.localFailures[5] << "; over a border: closed "
			<< nav.crossFailures[4] << ", node pool " << nav.crossFailures[5] << "\n";
		std::cout << "selections " << report.selections << ", arrivals " << report.arrivals
			<< ", squads that moved " << report.squadsThatMoved << "\n";
		std::cout << "stuck (travel goal, no movement for 6 h): now " << report.stuckNow
			<< ", episodes " << report.stuckEpisodes << "\n";
		std::cout << "goal failures by reason:";
		std::uint64_t failures = 0;
		for (int i = 0; i < ksim::state::kAiReasonCount; ++i)
		{
			failures += report.failures[static_cast<std::size_t>(i)];
			if (report.failures[static_cast<std::size_t>(i)] != 0)
				std::cout << " " << ksim::state::aiReasonName(static_cast<ksim::state::AiReason>(i))
					<< "=" << report.failures[static_cast<std::size_t>(i)];
		}
		std::cout << (failures == 0 ? " none" : "") << "\n";
		std::cout << "squads by fallback reason now:";
		for (int i = 0; i < ksim::state::kAiReasonCount; ++i)
		{
			if (report.last.byFallback[static_cast<std::size_t>(i)] != 0)
				std::cout << " " << ksim::state::aiReasonName(static_cast<ksim::state::AiReason>(i))
					<< "=" << report.last.byFallback[static_cast<std::size_t>(i)];
		}
		std::cout << "\nsquads by behaviour now:";
		for (int i = 0; i < ksim::kBehaviourCount; ++i)
			std::cout << " " << ksim::behaviourName(static_cast<ksim::Behaviour>(i)) << "="
				<< report.last.byBehaviour[static_cast<std::size_t>(i)];
		std::cout << "\nfallback log lines " << logTotal << "\n";
		// Town life when no player is near (TOWN_LIFE_SLICE 5.3 LOD, T3f): the leader-list phase of every squad with a package, now and
		// at its hourly peak, plus the window-valid rows the scorer does not run. An unloaded squad locks no door and sleeps in no real
		// bed (there is no catch-up): the phases are what the squads would be doing.
		std::cout << "town life (unloaded): squads by phase now";
		for (int i = 0; i < ksim::routines::kTownPhaseCount; ++i)
			std::cout << " " << ksim::routines::townPhaseName(static_cast<ksim::routines::TownPhase>(i)) << "="
				<< report.last.byPhase[static_cast<std::size_t>(i)];
		std::cout << "; peaks";
		for (int i = 0; i < ksim::routines::kTownPhaseCount; ++i)
			std::cout << " " << ksim::routines::townPhaseName(static_cast<ksim::routines::TownPhase>(i)) << "="
				<< report.maxByPhase[static_cast<std::size_t>(i)];
		std::cout << "; unmodelled goal rows now " << report.last.unmodelled << " (peak " << report.maxUnmodelled << ")\n";
		// The real-economy overlay (TOWN_LIFE_SLICE 7, T4a): the aggregate stock at the final sample. Zero in `original` mode.
		std::cout << "economy: " << report.economyTowns << " towns with stock, " << report.economyLines << " lines ("
			<< report.economyEmptyLines << " at zero), mean fill " << report.economyFill << ", " << report.economyProducers
			<< " producers\n";
		if (const ksim::DoorSystem* gates = simulation.ai()->doorSystem())
		{
			const ksim::DoorStats& ds = gates->stats();
			std::cout << "gates: " << ds.gates << " gate doors in " << ds.gateTowns << " towns; with a crew "
				<< ds.gatesCrewed << ", crew reopens " << ds.crewReopens << ", books applied "
				<< ds.booksApplied << " (" << nav.doorApplyNs / 1.0e9 << " s), doors restricted now "
				<< ds.doorsRestricted << "; started raid-hours " << ds.raidHours << ", at gate towns " << ds.raidHoursAtGates << "\n";
			std::cout << "door effects: routes refused only by a gate " << nav.doorRefusedRoutes
				<< ", polygons skipped by goal picks " << nav.doorSkippedPolys << "\n";
		}
		std::cout << "faction layer calls " << simulation.ai()->stats().factionCalls << "\n";
		if (const ksim::FactionStats* fs = simulation.ai()->factionStats())
		{
			std::uint32_t planned = 0, started = 0, arrived = 0, completed = 0, failed = 0,
				rejected = 0, lost = 0, active = 0, events = 0;
			std::size_t byIntent[ksim::state::kIntentCount] = {};
			simulation.world().ecs().each([&](const ksim::state::FactionAI& f)
			{
				planned += f.planned;
				started += f.started;
				arrived += f.arrived;
				completed += f.completed;
				failed += f.failed;
				rejected += f.rejected;
				lost += f.squadsLost;
				events += f.events;
				active += static_cast<std::uint32_t>(f.raids.size());
				++byIntent[static_cast<int>(f.intent)];
			});
			std::cout << "raids: planned " << planned << ", started " << started << ", arrived "
				<< arrived << ", completed " << completed << ", failed " << failed
				<< ", rejected " << rejected << ", running now " << active << "\n";
			const auto& traders = simulation.traderStats();
			std::cout << "merchants: spawn refreshes " << traders.forced << ", timer refreshes " << traders.refreshes << ", units placed "
				<< traders.placed << ", overflow destroyed " << traders.overflowDestroyed << "\n";
			std::cout << "raid squads: spawned " << fs->squadsSpawned << ", retired "
				<< fs->squadsRetired << "; events consumed " << events << ", squads lost " << lost
				<< "; follow-up campaigns fired " << fs->triggersFired << "\n";
			std::cout << "rolls: " << fs->rolls << " passes, " << fs->rollCandidates
				<< " campaigns drawn, " << fs->rollPassed << " passed; defeated " << fs->defeated
				<< ", territory refusals " << fs->territoryRefused << "\n";
			std::cout << "faction script: " << (fs->scriptLoaded ? "loaded" : "none (native scoring)")
				<< ", decisions " << fs->scriptApplied << ", slept " << fs->scriptSleeping << ", fallbacks "
				<< fs->scriptFallbacks << " (errors " << fs->scriptErrors << ", budget " << fs->scriptBudget
				<< ", memory " << fs->scriptMemory << ", unusable " << fs->scriptUnusable << "), peak "
				<< fs->scriptPeakBytes << " bytes\n";
			std::cout << "faction intents now:";
			for (int i = 0; i < ksim::state::kIntentCount; ++i)
				std::cout << " " << ksim::state::intentName(static_cast<ksim::state::Intent>(i))
					<< "=" << byIntent[i];
			std::cout << "\n";
		}
		if (!savePath.empty())
			simulation.saveGame(savePath);
		return report.maxGoalless == 0 ? 0 : 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_soak: " << error.what() << "\n";
		return 1;
	}
}
