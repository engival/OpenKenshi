// kenshi-k16.6.592 (rwh.247): a CHARACTER with `named` = 1 gets a name from the name list (namesM + namesMF, or namesF + namesMF by sex) when
// it is built (makeFighter: the world spawn paths and the new-game start squad all use it); ANIMAL_CHARACTER and `named` = 0 keep the record
// name. Checked: the start leader "start- Homeless" (named = 1 in rebirth.mod) gets a listed name, Bonedog keeps "Bonedog", a named = 0 record
// keeps its own, the draw is deterministic for a state, varies with it, and a save and a load keep the name (Fighter::name is saved as is).

#include "dt27_rig.hpp"

#include "ksim/save/save.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>

namespace {

using namespace ksim;
using namespace dt27;

std::vector<std::string> lines(const std::string& file)
{
	std::vector<std::string> out;
	std::ifstream in(std::string(KENSHI_DATA) + "/" + file);
	for (std::string l; std::getline(in, l);)
	{
		while (!l.empty() && (l.back() == '\r' || l.back() == '\n'))
			l.pop_back();
		if (!l.empty())
			out.push_back(l);
	}
	return out;
}

} // namespace

int main()
{
	try
	{
		Rig rig({}, 0);
		const kdata::GameData& gd = *rig.gd;
		std::vector<std::string> male = lines("namesM.txt");
		std::vector<std::string> female = lines("namesF.txt");
		for (const std::string& n : lines("namesMF.txt"))
		{
			male.push_back(n);
			female.push_back(n);
		}
		test::require(!male.empty() && !female.empty(), "the name lists are empty");
		const auto listed = [&](const state::Fighter& f)
		{
			const std::vector<std::string>& l = f.look.female ? female : male;
			return std::find(l.begin(), l.end(), f.name) != l.end();
		};

		const kdata::Record* leader = nullptr;
		const kdata::Record* dog = nullptr;
		const kdata::Record* plain = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "start- Homeless")
				leader = r;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ANIMAL_CHARACTER")))
			if (r->name == "Bonedog")
				dog = r;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (plain == nullptr && !r->boolOr("named", false) && r->name != "start- Homeless")
				plain = r;
		test::require(leader != nullptr && dog != nullptr && plain != nullptr, "records missing");
		test::require(leader->boolOr("named", false), "start- Homeless is not named");

		std::set<std::string> seen;
		for (std::uint64_t s = 1; s <= 40; ++s)
		{
			std::uint64_t a = s * 7919;
			std::uint64_t b = a;
			const state::Fighter f = rig.data->makeFighter(gd, leader->stringID, "", a);
			const state::Fighter g = rig.data->makeFighter(gd, leader->stringID, "", b);
			test::require(f.name != "start- Homeless" && listed(f), "a named character's name is not from the list: " + f.name);
			test::require(f.name == g.name, "the same state drew two names");
			seen.insert(f.name);
			std::uint64_t c = a = s * 7919;
			test::require(rig.data->makeFighter(gd, dog->stringID, "", c).name == "Bonedog", "Bonedog was renamed");
			c = s * 7919;
			test::require(rig.data->makeFighter(gd, plain->stringID, "", c).name == plain->name, "a named = 0 character was renamed");
		}
		test::require(seen.size() > 10, "the draw does not vary with the state");

		const std::uint64_t id = rig.spawn(leader->stringID, 0.0, 0.0);
		const std::string name = rig.fighterOf(id).name;
		test::require(name != "start- Homeless", "the spawn kept the record name");
		const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_char_names.ksav";
		rig.sim.saveGame(path);
		Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
		std::remove(path.c_str());
		std::string kept;
		loaded.world().ecs().each([&](const state::Id& key, const state::Fighter& f)
		{
			if (key.value == id)
				kept = f.name;
		});
		test::require(kept == name, "a load changed the name: '" + kept + "' != '" + name + "'");
		std::cout << "ksim_char_names: ok (" << seen.size() << " distinct names in 40 draws)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
