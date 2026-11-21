#include "world/plants.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#include "kdata/format.hpp"
#include "kdata/gamedata.hpp"

namespace
{

constexpr int kFoliageMaterial = 4;	// MapFeatureMode FOLIAGE (features.hpp FeatureMode::Foliage): the vegetation material

// The material rule, straight from the FCS record (no kworld feature record needed: `collision` is not part of it).
bool materialRule(const kdata::Record& r)
{
	if (r.fileOr("mesh").empty()) { return false; }	// a marker or an unresolved record draws nothing
	if (!r.fileOr("leaves mesh").empty()) { return true; }	// the canopy is drawn with the vegetation material
	if (!r.fileOr("collision").empty()) { return false; }	// a solid: rock, wreck, ruin, scaffolding, prop
	// FOLIAGE_MESH `material type` / MAP_FEATURES `texture mode`: both are MapFeatureMode.
	const std::int32_t mode = r.intOr("material type", r.intOr("texture mode", 0));
	return mode == kFoliageMaterial;
}

std::unordered_map<std::string, plants::Override> g_table;

}

namespace plants
{

bool load(const std::string& clientDataDir)
{
	g_table.clear();
	const std::filesystem::path path = std::filesystem::path(clientDataDir) / "foliage_plants.txt";
	std::ifstream in(path);
	if (!in)
	{
		fprintf(stderr, "kclient: plants: cannot read %s (client/data missing or client.data_dir wrong)\n", path.string().c_str());
		return false;	// fatal for the caller: the material rule alone renders other trees (kenshi-k16.2.467)
	}
	std::string line;
	int number = 0;
	while (std::getline(in, line))
	{
		number++;
		if (const size_t hash = line.find('#'); hash != std::string::npos) { line.resize(hash); }
		std::istringstream words(line);
		std::string id;
		int plant = -1;
		if (!(words >> id)) { continue; }	// blank or a comment
		if (!(words >> plant) || (plant != 0 && plant != 1))
		{
			fprintf(stderr, "kclient: plants: %s:%d: expected `<FCS string id> <0|1> <reason>`\n", path.string().c_str(), number);
			continue;
		}
		std::string reason;
		std::getline(words, reason);
		const size_t first = reason.find_first_not_of(" \t");
		reason = first == std::string::npos ? std::string() : reason.substr(first);
		g_table[id] = Override{ plant == 1, reason };	// a later line for the same id wins
	}
	fprintf(stderr, "kclient: plants: %zu exceptions from %s\n", g_table.size(), path.string().c_str());
	return true;
}

const Override* overrideOf(std::string_view recordID)
{
	const auto it = g_table.find(std::string(recordID));
	return it == g_table.end() ? nullptr : &it->second;
}

int loadedOverrides() { return (int)g_table.size(); }

bool isPlant(const kdata::GameData& gameData, std::string_view recordID)
{
	if (const Override* o = overrideOf(recordID)) { return o->plant; }
	const kdata::Record* r = gameData.find(recordID);
	return r != nullptr && materialRule(*r);
}

}
