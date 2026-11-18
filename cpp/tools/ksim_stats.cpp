// ksim_stats: loads the world model and prints prefab/instance/relationship
// counts, load time and resident memory.

#include "ksim/world.hpp"

#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

long residentKb()
{
	std::ifstream status("/proc/self/status");
	std::string line;
	while (std::getline(status, line))
	{
		if (line.rfind("VmRSS:", 0) != 0)
			continue;
		std::size_t position = 6;
		while (position < line.size() && line[position] == ' ')
			++position;
		return std::stol(line.substr(position));
	}
	return -1;
}

void row(const char* label, std::size_t value)
{
	std::cout << std::left << std::setw(30) << label << value << "\n";
}

} // namespace

int main()
{
	try
	{
		const ksim::World world = ksim::World::load(KENSHI_DATA);
		const ksim::WorldCounts& counts = world.counts();

		std::cout << "ksim_stats\n";
		row("prefabs total", counts.prefabs);
		row("prefabs FACTION", counts.factions);
		row("prefabs TOWN", counts.towns);
		row("prefabs SQUAD_TEMPLATE", counts.squadTemplates);
		row("prefabs CHARACTER", counts.characters);
		row("prefabs HUMAN_CHARACTER", counts.humanCharacters);
		row("prefabs ANIMAL_CHARACTER", counts.animals);
		row("prefabs BUILDING", counts.buildings);
		row("prefabs RACE", counts.races);
		row("prefabs AI_PACKAGE", counts.aiPackages);

		row("placed buildings", counts.placedBuildings);
		row("placed with BUILDING prefab", counts.placedWithPrefab);
		row("placed missing prefab",
			counts.placedBuildings - counts.placedWithPrefab);

		row("rel faction relations", counts.factionRelations);
		row("rel membership", counts.membership);
		row("rel town owned by faction", counts.townOwnership);
		row("rel town residents", counts.townResidents);
		row("rel town bar squads", counts.townBarSquads);
		row("rel town roaming squads", counts.townRoamingSquads);
		row("rel town default resident", counts.townDefaultResident);
		row("rel faction residents", counts.factionResidents);
		row("rel faction default resident", counts.factionDefaultResident);
		row("rel faction bar squads", counts.factionBarSquads);
		row("rel faction squads", counts.factionSquads);
		row("rel faction squad default", counts.factionSquadDefault);
		row("rel faction roaming squads", counts.factionRoamingSquads);
		row("rel squad members", counts.squadMembers);
		row("rel squad members2", counts.squadMembers2);
		row("rel squad leaders", counts.squadLeaders);
		row("rel squad choosefrom", counts.squadChooseFrom);
		row("rel squad animals", counts.squadAnimals);
		row("rel squad animals2", counts.squadAnimals2);
		row("rel character race options", counts.raceOptions);
		row("rel squad AI packages", counts.aiPackageLinks);
		row("rel AI inherits", counts.aiInherits);
		row("rel AI leads to", counts.aiLeadsTo);
		row("rel town overrides", counts.overridesTown);

		std::cout << std::fixed << std::setprecision(3);
		std::cout << std::left << std::setw(30) << "load seconds"
			<< world.loadSeconds() << "\n";
		std::cout << std::left << std::setw(30) << "rss kb" << residentKb() << "\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_stats: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
