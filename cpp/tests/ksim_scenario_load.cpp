// Task CE (2026-11-07): the playtest scenarios as files (`scenarios/<name>.kscen`, docs/reference/PLAYTEST.md 1). Every file the server
// lists (SCENARIO_LIST) parses, checks against the real data, loads by name into the pristine world as SCENARIO_LOAD does
// (Simulation::newWorld, then scenario::apply) and runs; the run's counters (ScenarioRun, the SCENARIO_METRICS values) answer every
// metric its header names, in the header's order, with a finite number. The wire itself (list, load, started, end, metrics) is
// ksim_simlink's; this is the content: the files the peer's client will list must all load.
// Needs the world bake (argv[1], like ksim_aggro_drop; exit 77 without the regions the scenarios stand in).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/save/scenario_run.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr int kRunTicks = 1200; // ~60 game s: the world AI has time to see a nearby patrol and start a fight

// The zones the scenes stand in, each with one zone of margin so a character at the edge has mesh under it.
std::vector<kworld::ZoneCell> zonesOf(const std::vector<scenario::File>& files)
{
	std::set<std::pair<int, int>> cells;
	for (const scenario::File& file : files)
	{
		const kworld::ZoneCell centre = kworld::zoneCellOf(file.centreX, file.centreZ);
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				cells.emplace(centre.x + dx, centre.z + dz);
		}
	}
	std::vector<kworld::ZoneCell> zones;
	for (const auto& [x, z] : cells)
		zones.push_back({x, z});
	return zones;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		std::vector<scenario::ScenarioInfo> infos;
		std::vector<scenario::File> files;
		for (const scenario::ScenarioInfo& info : scenario::listScenarios(KSIM_SCENARIOS_DIR))
		{
			infos.push_back(info);
			files.push_back(scenario::parseFile(info.path));
		}
		test::require(!files.empty(), std::string("no scenario files in ") + KSIM_SCENARIOS_DIR);
		// The six the milestone ships (ROADMAP P): a rename is fine, a disappearance is not.
		for (const char* name : {"bar_lure", "bar_lure_hostile", "manhunters_desert", "beak_open_ground", "slavers_downed", "spectate_fight"})
		{
			bool found = false;
			for (const scenario::ScenarioInfo& info : infos)
				found = found || info.name == name;
			test::require(found, std::string("scenario missing: ") + name);
		}
		// The feature loop's header field bead=<feature id> (kenshi-h2v.7): every playtest scenario names the beads item it proves, and the
		// listing carries it. A file with no bead= parses to "" (older files, a fixture's header).
		{
			const std::map<std::string, std::string> beads = {{"bar_lure", "kenshi-h2v.1"}, {"spectate_fight", "kenshi-h2v.2"}, {"bar_lure_hostile", "kenshi-h2v.3"},
				{"beak_open_ground", "kenshi-h2v.4"}, {"manhunters_desert", "kenshi-h2v.5"}, {"slavers_downed", "kenshi-h2v.6"}, {"cage_prisoner", "kenshi-h2v.11"},
				{"dismember_peeler", "kenshi-h2v.13"}, {"dismember_fight", "kenshi-h2v.13"}, {"journey_walk", "kenshi-k16.18"}, {"journey_mine", "kenshi-k16.19"}, {"journey_sell", "kenshi-k16.20"}};
			for (const scenario::ScenarioInfo& info : infos)
			{
				const auto want = beads.find(info.name);
				test::require(!info.bead.empty() && (want == beads.end() || want->second == info.bead), info.name + ": its bead= header (" + info.bead + ")");
			}
			for (const auto& [name, bead] : beads)
			{
				bool found = false;
				for (const scenario::ScenarioInfo& info : infos)
					found = found || info.name == name;
				test::require(found, "scenario missing: " + name);
			}
			const std::string probe = (std::filesystem::current_path() / "ksim_scenario_load_bead.kscen").string();
			const std::string body = "squad id=1 name=S faction=204-gamedata.base player=1 money=0\nchar id=1 squad=1 name=W template=1533662-rebirth.mod faction=204-gamedata.base x=0 y=0 z=0 yaw=0 leader=1 player=1\n";
			std::ofstream(probe) << "KSCEN 2\nscenario title=T setup=S look_for=L bead=kenshi-x.1 metrics=fight_s\n" << body;
			test::require(scenario::parseFile(probe).bead == "kenshi-x.1", "bead= is parsed into the File");
			std::ofstream(probe) << "KSCEN 2\nscenario title=T setup=S look_for=L metrics=fight_s\n" << body;
			test::require(scenario::parseFile(probe).bead.empty(), "no bead= parses to an empty bead");
			std::filesystem::remove(probe);
		}
		// A header metric outside the vocabulary is refused at parse time, not listed and then answered as never valid.
		{
			const std::string bad = (std::filesystem::current_path() / "ksim_scenario_load_bad.kscen").string();
			std::ofstream(bad) << "KSCEN 2\nscenario title=T setup=S look_for=L metrics=fight_s,not_a_metric\n";
			std::string why;
			try
			{
				scenario::parseFile(bad);
			} catch (const std::exception& e) {
				why = e.what();
			}
			std::filesystem::remove(bad);
			test::require(why.find("not_a_metric") != std::string::npos, "an unknown header metric is not refused at parse time");
		}
		const std::vector<kworld::ZoneCell> zones = zonesOf(files);
		if (bake.empty())
		{
			std::cout << "ksim_scenario_load: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		for (const scenario::File& file : files)
		{
			const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
			const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
			if (test::bakeNav() && !std::filesystem::exists(region))
			{
				std::cout << "ksim_scenario_load: SKIPPED: no " << region << " (the bake lacks the scenario's region)\n";
				return 77;
			}
		}
		std::cout << std::unitbuf;

		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		const std::shared_ptr<ksim::MeshNavigator> mesh = test::openWorldNav(bake, zones);
		SpawnOptions options;
		options.seed = 7;
		std::unique_ptr<Simulation> sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options));
		sim->markPristine(); // the bare world SCENARIO_LOAD places on (ksim_server: before combat and navigation exist)
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));

		simlink::ServerContext context;
		context.gameData = sim->gameData();
		context.squadLimit = 50;

		for (std::size_t i = 0; i < files.size(); ++i)
		{
			const scenario::ScenarioInfo& info = infos[i];
			const scenario::File& file = files[i];
			scenario::check(*sim->gameData(), file);
			sim->newWorld(); // the load's world swap, then the file on it
			sim->run(1); // apply() is between ticks: let the restored world go idle first
			const std::uint64_t seed = static_cast<std::uint64_t>(100 + i);
			const scenario::Placed placed = scenario::apply(*sim, *sim->gameData(), *combat, context, file, seed);
			// A file with no player squad is a spectator one (spectate_fight, trails_mob): its NPC squads fight on their own
			// (ksim_spectate_fight). Every other file must place a player character.
			bool hasPlayerSquad = false;
			for (const scenario::Squad& squad : file.squads)
				hasPlayerSquad = hasPlayerSquad || squad.player;
			test::require(!placed.players.empty() || !hasPlayerSquad, info.name + ": no player character placed");
			if (info.name == "slavers_downed")
			{
				// k16.5.3: no `item` lines, so every char keeps its template's roll (clothing and a weapon in hand), as a world-spawned
				// one does; the file used to strip them to fists and nothing.
				for (const int charId : {1, 2, 3})
				{
					const state::Fighter* f = sim->combat()->find(placed.idOf.at(charId));
					test::require(f != nullptr && !f->armour.empty(), "slavers_downed char " + std::to_string(charId) + " wears no armour");
					test::require(f->weapon.sid != combat->unarmed.sid, "slavers_downed char " + std::to_string(charId) + " holds no weapon");
				}
			}
			const std::uint64_t start = sim->clockTick();
			simlink::ScenarioRun run;
			run.reset(info.name, info.fileHash, static_cast<std::uint32_t>(seed), start, info.metrics);
			for (int tick = 0; tick < kRunTicks; ++tick)
			{
				sim->tick();
				run.sample(*sim, sim->clockTick());
			}
			test::require(sim->clockTick() > start, info.name + ": the scenario did not run");
			std::string line = info.name + " (" + std::to_string(info.metrics.size()) + " metrics, seed " + std::to_string(seed) + "):";
			for (const std::string& metric : info.metrics)
			{
				test::require(simlink::isScenarioMetric(metric), info.name + ": unknown metric " + metric);
				std::uint8_t valid = 0;
				const double value = run.value(metric, sim->clockTick(), valid);
				test::require(std::isfinite(value), info.name + ": " + metric + " is not finite");
				line += " " + metric + "=" + std::to_string(value) + (valid != 0 ? "" : "(-)");
			}
			std::cout << line << "\n";
			if (info.name == "spectate_fight")
			{
				// A player-less file still reports its fight (2026-11-08): the sample used to return with no players, so every metric
				// was invalid and bodies_down 0 while the two patrols fought (the client saw fight_s=?).
				std::uint8_t fightValid = 0;
				const double fight = run.value("fight_s", sim->clockTick(), fightValid);
				test::require(fightValid != 0 && fight > 0.0, "spectate_fight reports no fight_s: a player-less run is not sampled");
			}
			// The head: the file the client's window shows.
			test::require(!info.title.empty() && !info.setup.empty() && !info.lookFor.empty(), info.name + ": the header text");
			test::require(scenario::fileHash(info.path) == info.fileHash, info.name + ": the file hash");
		}
		// A char with explicit `item` lines keeps exactly those lines and gets nothing from its template (k16.5.3): the Boss with a hip
		// weapon and one legs piece wields that weapon and wears that one piece.
		{
			scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/slavers_downed.kscen");
			scenario::Item weapon;
			weapon.sid = "52295-rebirth.mod";
			weapon.section = "hip";
			weapon.material = "914-gamedata.base";
			weapon.company = "912-gamedata.base";
			scenario::Item legs;
			legs.sid = "2308-clothes_v1.mod";
			legs.section = "legs";
			for (scenario::Char& c : file.chars)
			{
				if (c.id == 2)
					c.items = {weapon, legs};
			}
			scenario::check(*sim->gameData(), file);
			sim->newWorld();
			sim->run(1);
			const scenario::Placed placed = scenario::apply(*sim, *sim->gameData(), *combat, context, file, 1);
			const state::Fighter* boss = sim->combat()->find(placed.idOf.at(2));
			test::require(boss != nullptr && boss->weapon.sid == weapon.sid, "explicit items: the Boss does not wield the file's weapon");
			test::require(boss->armour.size() == 1 && boss->armour.front().sid == legs.sid, "explicit items: the Boss wears more or less than the file's one piece");
		}
		std::cout << "scenario load: " << files.size() << " files, " << kRunTicks << " ticks each, every header metric named\n";
		return 0;
	} catch (const std::exception& e) {
		std::cout << "ksim_scenario_load: " << e.what() << "\n";
		return 1;
	}
}
