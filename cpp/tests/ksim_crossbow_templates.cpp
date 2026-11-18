// kenshi-k16.6.24: a vanilla template's crossbow. The templates list crossbows under the `crossbows` reference category (not `weapons`);
// makeFighter must arm Fighter::ranged from either: `v1` of a `crossbows` entry is the percent chance per spawn (100 always, 0 never).
// Prints the counts for the report.

#include "ksim/combat.hpp"

#include "kworld/gamedata.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const std::int32_t crossbowType = kworld::itemTypeByName("CROSSBOW");
		// A template's crossbow chance in percent: its crossbow entries roll in turn, each `v1` percent (a `weapons` entry always arms); -1 = none.
		const auto chanceOf = [&](const kdata::Record& character, const char* category) -> double
		{
			double miss = 1.0;
			bool any = false;
			const kdata::RefCategory* refs = character.findRefs(category);
			if (refs != nullptr)
				for (const kdata::RefEntry& entry : refs->entries)
				{
					const kdata::Record* item = entry.opaqueEntry || entry.v0 < 1 ? nullptr : gd.find(entry.targetID);
					if (item != nullptr && item->type == crossbowType)
					{
						any = true;
						miss *= 1.0 - (std::string_view(category) == "weapons" ? 100 : std::clamp(entry.v1, 0, 100)) / 100.0;
					}
				}
			return any ? 100.0 * (1.0 - miss) : -1.0;
		};
		int inWeapons = 0;
		int inCrossbows = 0;
		int gain = 0;
		int certain = 0;
		int bad = 0;
		std::uint64_t rng = 3;
		for (const kdata::Record* character : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			const double w = chanceOf(*character, "weapons");
			const double c = chanceOf(*character, "crossbows");
			inWeapons += w >= 0;
			inCrossbows += c >= 0;
			const double chance = std::max(w, c);
			if (chance < 0)
				continue;
			int armed = 0;
			constexpr int kSpawns = 400;
			for (int i = 0; i < kSpawns; ++i)
				armed += data->makeFighter(gd, character->stringID, "", rng).ranged.ammo > 0;
			gain += chance > 0.0;
			certain += chance >= 100.0;
			// 100 always arms, 0 never; a percent in between lands within 12 points over 400 spawns (std dev <= 2.5)
			const bool ok = chance >= 100.0 ? armed == kSpawns : chance == 0.0 ? armed == 0 : std::abs(armed * 100.0 / kSpawns - chance) < 12.0;
			if (!ok)
			{
				++bad;
				std::printf("%s: chance %.1f%%, armed %d / %d\n", character->name.c_str(), chance, armed, kSpawns);
			}
		}
		std::printf("crossbow templates: under `weapons` %d, under `crossbows` %d; with a chance above 0: %d (100%%: %d)\n", inWeapons, inCrossbows, gain, certain);
		test::require(inCrossbows > 0 && certain > 0, "no vanilla template lists a crossbow under `crossbows`");
		test::require(bad == 0, "a template's crossbow chance is not followed");
		std::printf("ok  crossbow templates armed by their chance\n");
	} catch (const std::exception& e)
	{
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
	return 0;
}
