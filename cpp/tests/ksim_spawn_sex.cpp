// A spawned character rolls its sex (task ds/spawn-sex): CombatData::rollFemale, `female chance` percent from the character's own dice
// state (FUN_1402e2b50; STARTS_CAGES_STATIC.md), called by makeFighter so every spawn path (squad, town, wilderness, new game) shares it.
// A chance of 100 spawns female, 0 male, and a mixed one rolls the same on a re-run.

#include "dt27_rig.hpp"

#include "ksim/combat/combat_data.hpp"

namespace {

using namespace ksim;
using namespace dt27;

std::uint8_t spawnSexOf(Rig& rig, const std::string& character, std::uint64_t state)
{
	return rig.data->makeFighter(*rig.gd, character, "", state).look.female;
}

void rolls()
{
	Rig rig({}, 0);
	std::string always, never, mixed;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("CHARACTER")))
	{
		const std::int64_t chance = record->intOr("female chance", 0);
		try
		{
			std::uint64_t state = 7;
			const state::Fighter probe = rig.data->makeFighter(*rig.gd, record->stringID, "", state);
			const RaceCombat* race = rig.data->race(probe.raceSid);
			if (race == nullptr || !race->hasFemale)
				continue;
			if (chance == 100 && always.empty())
				always = record->stringID;
			if (chance == 0 && never.empty())
				never = record->stringID;
			if (chance > 10 && chance < 90 && mixed.empty())
				mixed = record->stringID;
		} catch (const std::exception&) {
		}
	}
	test::require(!always.empty() && !never.empty() && !mixed.empty(), "the data lacks a CHARACTER with female chance 100, 0 and a mixed one");
	int females = 0;
	for (std::uint64_t state = 1; state <= 200; ++state)
	{
		test::require(spawnSexOf(rig, always, state) == 1, "female chance 100 spawned male");
		test::require(spawnSexOf(rig, never, state) == 0, "female chance 0 spawned female");
		const std::uint8_t sex = spawnSexOf(rig, mixed, state);
		test::require(sex == spawnSexOf(rig, mixed, state), "a re-run rolled a different sex");
		females += sex;
	}
	test::require(females > 0 && females < 200, "a mixed chance rolled one sex only");
	std::cout << "  " << always << " always female, " << never << " never, " << mixed << " " << females << "/200\n";
}

} // namespace

int main()
{
	try
	{
		rolls();
		std::cout << "ksim_spawn_sex: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_spawn_sex: FAILED: " << e.what() << "\n";
		return 1;
	}
}
