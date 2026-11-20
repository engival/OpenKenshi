// The plant predicate check (kenshi-k16.2.420.6.1): compile client/src/world/plants.cpp against libkdata, load the four
// shipped game data files (the same merge order the client uses), load client/data/foliage_plants.txt, and assert the
// predicate's verdict on the records the task names: the user's swaying rock and the other rocks and props are static,
// the palms, trees, bushes and grass clumps bend, and every id in the data file exists in the game data.
//
// usage: plants_main <game data dir> <client data dir>
#include "world/plants.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <sstream>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"

namespace
{
int failures = 0;
void check(bool ok, const std::string& what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) { failures++; }
}

kdata::GameData loadMerged(const std::string& dataDir)
{
	std::vector<kdata::File> files;
	kdata::GameData gd;
	for (const char* name : { "gamedata.base", "Newwworld.mod", "Dialogue.mod", "rebirth.mod" })
	{
		const std::filesystem::path path = std::filesystem::path(dataDir) / name;
		std::ifstream in(path, std::ios::binary);
		if (!in) { fprintf(stderr, "plants_main: cannot open %s\n", path.string().c_str()); std::exit(2); }
		const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		files.push_back(kdata::parse(std::as_bytes(std::span<const char>(bytes.data(), bytes.size())), name));
	}
	for (const kdata::File& f : files) { gd.apply(f); }
	return gd;
}
}

int main(int argc, char** argv)
{
	if (argc != 3) { fprintf(stderr, "usage: %s <game data dir> <client data dir>\n", argv[0]); return 2; }
	const kdata::GameData gd = loadMerged(argv[1]);
	plants::load(argv[2]);
	const auto plant = [&](const char* id) { return plants::isPlant(gd, id); };

	printf("game data: %zu records; %d plant exceptions loaded\n", gd.size(), plants::loadedOverrides());

	// The user's rock (a FOLIAGE_MESH record: TERRAIN material and a collision mesh) and the other rocks, wrecks and ruins.
	check(!plant("3430-Newwworld.mod"), "Foliage_1K_RockStack01 (the user's rock) is static");
	check(!plant("2700-Newwworld.mod"), "FOLIAGE_Boulder01 is static");
	check(!plant("1533075-Newwworld.mod"), "FOLIAGE_ContourStones01 is static");
	check(!plant("14546-Newwworld.mod"), "Crumble-Building01 is static");
	check(!plant("3416-Newwworld.mod"), "Ashland_Foliage_Shell01 is static");
	check(!plant("64596-Newwworld.mod"), "Ashland_StructureWreck01 is static");
	// props the material rule would bend: the data file's zeros
	check(!plant("14554-Newwworld.mod"), "LandCables01 is static (data)");
	check(!plant("14559-Newwworld.mod"), "Building_Ruins_Newer01 is static (data)");
	check(!plant("2257-vurt.mod"), "2DGreenPlants is static (data)");
	check(!plant("1466-vurt.mod"), "DeadTree 01 is static (data: the lead's call)");
	check(!plant("2980-Newwworld.mod"), "DeadPine is static (data: the lead's call)");
	// plants: the material rule
	check(plant("2256-D-vurt.mod"), "PalmYucca bends (material type 4)");
	check(plant("2807-Newwworld.mod"), "Thin Tree [Wide] bends (leaves mesh)");
	check(plant("47184-Newwworld.mod"), "Yucka01 bends (material type 4)");
	check(plant("1470-vurt.mod"), "Bush 01 [Brown] bends (material type 4)");
	// plants the rule misses: the data file's ones
	check(plant("3208-Newwworld.mod"), "FanPalm01 bends (data)");
	check(plant("3118-Newwworld.mod"), "ShootCluster01 bends (data)");
	check(plant("5811-Newwworld.mod"), "BigGrassClump bends (data)");
	check(plant("55235-Newwworld.mod"), "Pale_BigGrass01 bends (data)");
	check(plant("5544-Newwworld.mod"), "Cactus_Type01 bends (data)");
	check(plant("999999999-Newwworld.mod") == false, "an id that does not exist is static");

	// The data file's reasons reach `dump foliage` (plants::overrideOf), and every id it names exists in the game data.
	int withReason = 0;
	std::ifstream in(std::filesystem::path(argv[2]) / "foliage_plants.txt");
	std::string line;
	while (std::getline(in, line))
	{
		if (const size_t hash = line.find('#'); hash != std::string::npos) { line.resize(hash); }
		std::string id;
		int value = -1;
		if (!(std::istringstream(line) >> id >> value)) { continue; }
		const plants::Override* o = plants::overrideOf(id);
		check(o != nullptr && o->plant == (value == 1), "data file id " + id + " loads");
		if (o != nullptr && !o->reason.empty()) { withReason++; }
		if (gd.find(id) == nullptr) { check(false, "data file id " + id + " exists in the game data"); }
	}
	check(withReason > 0, "the data file's reasons are kept for dump foliage (" + std::to_string(withReason) + ")");

	printf("%s: %d failure(s)\n", failures == 0 ? "plants check passed" : "plants check FAILED", failures);
	return failures == 0 ? 0 : 1;
}
