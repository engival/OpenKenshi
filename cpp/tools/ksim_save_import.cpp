// ksim_save_import SAVEDIR [--no-snap] [--radius R] [--seed N] [--ticks N] [--platoons]: imports an original Kenshi save's platoons and characters
// (ksim/save/save_import.hpp, docs/reference/SAVE_IMPORT.md "Stage 2") into a new game's world and prints what happened: the platoons by outcome, the
// members, the gaps. --ticks N then runs N sim ticks (the imported world must survive them); --platoons lists every platoon with its outcome.
// The save is only read (quick.save and the platoon files it names); nothing is written.
#include "ksim/combat/combat_data.hpp"
#include "ksim/save/save_import.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/loadorder.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main(int argc, char** argv)
{
	std::string dir;
	saveimport::Options options;
	std::uint64_t seed = 1;
	int ticks = 0;
	bool list = false;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--no-snap")
			options.snapFull = false;
		else if (arg == "--platoon-box" && i + 1 < argc)
			options.platoonBox = std::atof(argv[++i]);
		else if (arg == "--seed" && i + 1 < argc)
			seed = std::strtoull(argv[++i], nullptr, 10);
		else if (arg == "--ticks" && i + 1 < argc)
			ticks = std::atoi(argv[++i]);
		else if (arg == "--platoons")
			list = true;
		else if (dir.empty())
			dir = arg;
		else
		{
			std::cerr << "ksim_save_import: unexpected argument " << arg << "\n";
			return 2;
		}
	}
	if (dir.empty())
	{
		std::cerr << "usage: ksim_save_import SAVEDIR [--no-snap] [--radius R] [--seed N] [--ticks N] [--platoons]\n";
		return 2;
	}
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = seed;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		const auto gameData = sim.gameData();
		const auto combatData = CombatData::build(*gameData, KENSHI_DATA);
		sim.enableCombat(combatData, CombatSettings{});
		const saveimport::Report report = saveimport::importSave(sim, *gameData, *combatData, dir, options);

		std::cout << "platoons: " << report.platoons.size() << " records (" << report.generatedRemoved << " generated squads removed first)\n";
		for (const auto& [outcome, count] : report.byOutcome)
			std::cout << "  " << outcome << ": " << count << "\n";
		std::cout << "stale platoon files (no record names them): " << report.staleFiles.size() << "\n";
		std::cout << "members: " << report.members << " (player " << report.membersPlayer << ", full " << report.membersFull << ", token " << report.membersToken
			<< "), skipped " << report.membersSkipped << ", dead bodies " << report.dead << ", slaves " << report.slaves << " (captor resolved " << report.captors
			<< "), unplaced " << report.unplaced << "\n";
		std::cout << "handle map: " << report.squadOfPool.size() << " squads, " << report.memberOfHandle.size() << " members; unresolved handles " << report.unresolvedHandles << "\n";
		std::cout << "player orders: " << report.jobsImported << " persistent rows, " << report.movesImported << " one-shot moves queued; skipped";
		if (report.ordersSkipped.empty())
			std::cout << " none";
		for (const auto& [task, count] : report.ordersSkipped)
			std::cout << " task " << task << " x" << count;
		std::cout << "\n";
		if (report.hasCamera)
			std::cout << "camera: (" << report.cameraPos[0] << ", " << report.cameraPos[1] << ", " << report.cameraPos[2] << ") zoom " << report.cameraZoom << ", floor " << (report.hasCameraFloor ? std::to_string(report.cameraFloor) : std::string("absent")) << "; selected "
				<< report.selected << ", selection " << report.selection.size() << "\n";
		const saveimport::WorldReport& w = report.world;
		std::cout << "world: clock day " << w.day << " " << w.hour << ":" << w.minute << "; relations " << w.relations << " (" << w.relationsNonZero << " non-zero) over "
			<< w.factions << " factions, prosperity " << w.prosperity << "; towns " << w.towns << ": owners " << w.owners << " (" << w.ownersChanged << " changed), no placement "
			<< w.townsWithoutPlacement << ", replacementTown read " << w.replacements.size() << "; research " << w.research << " (dropped " << w.researchDropped << "); money "
			<< w.money << (w.playerFaction ? "" : " (no player faction: not applied)") << "; bounties " << w.bounties << " on " << w.bountyCharacters << " characters (lost "
			<< w.bountiesLost << "); weather " << w.weatherSet.size() << "/" << w.weatherSlots << " slots set\n";
		std::cout << "world 3b: extra towns " << w.extraTowns << " (placed " << w.extraTownsPlaced << ", owned " << w.extraTownsOwned << "); town flags " << w.townsInfo
			<< " (public " << w.townsPublic << ", discovered " << w.townsDiscovered << ", explored " << w.townsExplored << "); replacements applied " << w.replacementsApplied
			<< ", refused " << w.replacementsRefused << "; ranked factions " << w.ranked << ", trust rows " << w.trustRows << "; uniques " << w.uniques << " (resolved "
			<< w.uniquesResolved << "); sentences " << w.sentences << "\n";
		for (const auto& [slot, group] : w.weatherSet)
			std::cout << "  weather slot " << slot << " -> " << group << "\n";
		for (const auto& [why, count] : w.weatherLeft)
			std::cout << "  weather left: " << why << ": " << count << "\n";
		std::cout << "appearance mod sids skipped: " << report.appearanceSids.size() << " distinct, " << report.appearanceOccurrences << " occurrences\n";
		std::cout << "gaps:\n";
		for (const auto& [what, count] : report.gaps)
			std::cout << "  " << what << ": " << count << "\n";
		std::cout << "STATS with no FighterSkills field: " << report.statsUnmapped.size() << " names\n";
		for (const auto& [name, count] : report.statsUnmapped)
			std::cout << "  " << name << ": " << count << "\n";
		if (list)
		{
			for (const saveimport::PlatoonResult& result : report.platoons)
				std::cout << "platoon " << result.name << " | " << result.file << " | " << result.outcome << " | " << result.reason << " | " << result.members << "\n";
			for (const std::string& name : report.staleFiles)
				std::cout << "stale " << name << "\n";
		}
		if (ticks > 0)
		{
			sim.run(static_cast<std::uint64_t>(ticks));
			std::cout << "ran " << ticks << " ticks\n";
		}
	} catch (const std::exception& e) {
		std::cerr << "ksim_save_import: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
