// ksim_checkpoints (DEV ONLY, CMake option KSIM_DEV; kenshi-k16.6.109): writes OUR per-NPC checkpoint log of the spectate_fight squads in the shape of the
// original's rwh.91 log (include/ksim/dev/checkpoint_log.hpp), from either start:
//
//   ksim_checkpoints --bake DIR --kscen [NAME] [--out PREFIX] [--seed N] [--seconds N]
//   ksim_checkpoints --bake DIR --import-save SAVEDIR [--out PREFIX] [--seconds N]
//
// DIR is a `knav_bake --world DIR` bake (the Hub's regions). The world is the server's: combat, navigation on the bake's mesh round the fight, the server AI.
// --kscen runs scenarios/NAME.kscen (default spectate_fight); --import-save runs the original's own save (saveimport::importSave, snapFull on) and finds the two
// fighting squads by their factions near the fight. Writes PREFIX.jsonl (the checkpoints) and PREFIX.sides.csv (per sim second: each squad's members on its own side of the
// bisector between the squad centroids, and the centroid gap). --seconds is the sim seconds to run (default 300 = the original's 60 wall s at speed 5).
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/dev/checkpoint_log.hpp"
#include "ksim/navigator.hpp"
#include "ksim/save_import.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/server_ai.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"

#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

using namespace ksim;

namespace {

// The spectate_fight squads' templates (the kscen's and the original's staging): the Blood Raiders and the Berserkers. (Both squads share one combat team, so the
// squad's SQUAD_TEMPLATE tells them apart.)
constexpr const char* kRaiderTemplate = "16865-gamedata.base";
constexpr const char* kBerserkerTemplate = "42258-rebirth.mod";
// The fight's place: the squads of the staging stand within this distance of the middle between them.
constexpr double kFightCentreX = -50644.0;
constexpr double kFightCentreZ = -990.0;
constexpr double kFightRadius = 400.0;

int usage()
{
	std::cerr << "usage: ksim_checkpoints --bake DIR (--kscen [NAME] | --import-save SAVEDIR) [--out PREFIX] [--seed N] [--seconds N]\n";
	return 2;
}

} // namespace

int main(int argc, char** argv)
{
	std::string bake;
	std::string kscen;
	std::string importPath;
	std::string out = "checkpoints";
	std::uint64_t seed = 11;
	double seconds = 300.0;
	try
	{
		for (int i = 1; i < argc; ++i)
		{
			const std::string arg = argv[i];
			const bool hasValue = i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0;
			if (arg == "--bake" && hasValue)
				bake = argv[++i];
			else if (arg == "--kscen")
				kscen = hasValue ? argv[++i] : "spectate_fight";
			else if (arg == "--import-save" && hasValue)
				importPath = argv[++i];
			else if (arg == "--out" && hasValue)
				out = argv[++i];
			else if (arg == "--seed" && hasValue)
				seed = std::stoull(argv[++i]);
			else if (arg == "--seconds" && hasValue)
				seconds = std::stod(argv[++i]);
			else
				return usage();
		}
	}
	catch (const std::exception&)
	{
		return usage();
	}
	if (bake.empty() || kscen.empty() == importPath.empty())
		return usage();

	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = seed;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		const kworld::ZoneCell zone = kworld::zoneCellOf(kFightCentreX, kFightCentreZ);
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		sim.enableNavigation(MeshNavigator::openWorld(bake, zones, KENSHI_DATA));
		ServerAiOptions ai;
		ai.registryPath = KSIM_AI_TABLE;
		enableServerAi(sim, ai);

		std::vector<std::uint64_t> raiders;
		std::vector<std::uint64_t> berserkers;
		if (!kscen.empty())
		{
			scenario::ScenarioInfo info;
			for (const scenario::ScenarioInfo& listed : scenario::listScenarios(KSIM_SCENARIOS_DIR))
				if (listed.name == kscen)
					info = listed;
			if (info.path.empty())
				throw std::runtime_error("scenario not listed: " + kscen);
			const scenario::File file = scenario::parseFile(info.path);
			simlink::ServerContext context;
			context.gameData = sim.gameData();
			context.squadLimit = 50;
			scenario::check(*sim.gameData(), file);
			sim.newWorld();
			sim.run(1);
			const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *combat, context, file, seed);
			std::cerr << "kscen " << kscen << ": " << placed.others.size() << " members, scenario " << std::hex << scenario::fileHash(info.path) << std::dec << "\n";
		} else {
			saveimport::Options importOptions;
			importOptions.snapFull = true;
			const saveimport::Report report = saveimport::importSave(sim, *sim.gameData(), *combat, importPath, importOptions);
			std::cerr << "import " << importPath << ": " << report.members << " members (full " << report.membersFull << ", token " << report.membersToken << "), unplaced " << report.unplaced << "\n";
			sim.run(1);
		}
		std::map<std::uint64_t, std::string> templateOf; // squad key -> SQUAD_TEMPLATE
		sim.world().ecs().each([&](const state::Id& id, const state::SquadInstance& squad) { templateOf[id.value] = squad.templateSid; });
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (std::hypot(m.fighter.x - kFightCentreX, m.fighter.z - kFightCentreZ) > kFightRadius)
				continue;
			if (m.fighter.playerControlled) // the save's Engival shares the Blood Raiders' template
				continue;
			const auto squad = templateOf.find(m.fighter.squad);
			if (squad != templateOf.end())
			{
				if (squad->second == kRaiderTemplate)
					raiders.push_back(m.id);
				else if (squad->second == kBerserkerTemplate)
					berserkers.push_back(m.id);
			} else if (m.fighter.z < kFightCentreZ) { // a scenario's characters have no SquadInstance (the world's residents have not streamed in yet): the side they start on
				raiders.push_back(m.id);
			} else {
				berserkers.push_back(m.id);
			}
		}
		std::cerr << "squads found: " << raiders.size() << " Blood Raiders, " << berserkers.size() << " Berserkers\n";
		if (raiders.empty() || berserkers.empty())
			throw std::runtime_error("the two squads were not found near the fight");

		dev::CheckpointLog log(sim, combat, !kscen.empty() ? "kscen" : "import");
		log.addSide("Berserkers_16", "Berserker", "Berserkers", berserkers);
		log.addSide("Bloodraiders_109", "Blood Raider", "Bloodraiders", raiders);
		log.begin();
		const int ticks = static_cast<int>(seconds / kTickSeconds + 0.5);
		for (int tick = 0; tick < ticks; ++tick)
		{
			sim.tick();
			log.tick();
		}
		std::ofstream jsonl(out + ".jsonl");
		log.write(jsonl);
		std::ofstream csv(out + ".sides.csv");
		csv << "sim_s,wall_s,berserkers_own,berserkers_n,raiders_own,raiders_n,centroid_gap\n";
		for (const dev::SideShare& s : log.sideShares())
			csv << s.simSeconds << "," << s.simSeconds / 5.0 << "," << s.ownA << "," << s.nA << "," << s.ownB << "," << s.nB << "," << s.gap << "\n";
		std::cerr << "first contact at " << log.firstContact() << " sim s; wrote " << out << ".jsonl and " << out << ".sides.csv\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_checkpoints: " << e.what() << "\n";
		return 1;
	}
}
