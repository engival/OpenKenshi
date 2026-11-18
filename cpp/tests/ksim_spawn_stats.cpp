// The stat block of a spawned fighter with no STATS record (task AR): it comes from the CHARACTER record's `combat stats` and
// `stats randomise`, and a `strength` of 0 there means unset. CHARACTER 510 (Dust bandit) lists `strength` 0, `combat stats` 10
// and `stats randomise` 3; kinspect crowd_melee measured the exe's Dust bandits at strength ~7-13 and 50-92% load. Reading the 0
// literally gave every spawned bandit strength 1 at 150%+ load. A CHARACTER with a STATS record keeps reading that record.

#include "ksim/combat/combat_data.hpp"
#include "kdata/format.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const ksim::CombatData> data = ksim::CombatData::build(gd, KENSHI_DATA);

		// CHARACTER 510: no STATS record, `strength` 0. gamedata.base lists `combat stats` 10 / `stats randomise` 3, but the merged
		// data overlays rebirth.mod's 510 (`combat stats` 15), so the test reads the base and spread from the merged record.
		const kdata::Record* dust = gd.find("510-gamedata.base");
		test::require(dust != nullptr, "no CHARACTER 510 in the merged data");
		test::require(dust->findRefs("stats") == nullptr, "CHARACTER 510 gained a STATS record: this test reads the other branch");
		test::require(dust->intOr("strength", -1) == 0, "CHARACTER 510's strength is no longer 0");
		const double combat = static_cast<double>(dust->intOr("combat stats", 1));
		const double spread = static_cast<double>(dust->intOr("stats randomise", 3));
		test::require(combat > 1.0, "CHARACTER 510's combat stats are gone: the test cannot tell the fallback from strength 1");

		// The jitter of `combat stats` by `stats randomise` stays in [base - spread, base + spread], and every seed draws it.
		for (std::uint64_t seed = 1; seed <= 64; ++seed)
		{
			std::uint64_t rng = seed;
			const ksim::state::Fighter f = data->makeFighter(gd, "510-gamedata.base", "", rng, "1302-gamedata.base");
			test::require(f.skills.strength >= combat - spread && f.skills.strength <= combat + spread,
				"seed " + std::to_string(seed) + ": Dust bandit strength " + std::to_string(f.skills.strength) + " outside "
					+ std::to_string(combat - spread) + "-" + std::to_string(combat + spread));
		}

		// A CHARACTER with a STATS record still takes the record's numbers: a large enough sample must show a strength the
		// `combat stats` branch could not produce (the STATS records carry their own values, not the CHARACTER's).
		const kdata::Record* withStats = nullptr;
		for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("CHARACTER")))
		{
			const kdata::RefCategory* refs = record->findRefs("stats");
			if (refs == nullptr || refs->entries.empty())
				continue;
			const kdata::Record* stats = gd.find(refs->entries.front().targetID);
			if (stats != nullptr && stats->floatOr("strength", 0.0f) > 0.0f)
			{
				withStats = record;
				break;
			}
		}
		test::require(withStats != nullptr, "no CHARACTER with a filled STATS record");
		const kdata::Record* stats = gd.find(withStats->findRefs("stats")->entries.front().targetID);
		std::uint64_t rng = 9;
		const ksim::state::Fighter f = data->makeFighter(gd, withStats->stringID, "", rng);
		test::require(std::fabs(f.skills.strength - stats->floatOr("strength", 0.0f)) < 1e-6,
			withStats->stringID + ": strength " + std::to_string(f.skills.strength) + " is not its STATS record's "
				+ std::to_string(stats->floatOr("strength", 0.0f)));
		std::cout << "ksim_spawn_stats: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_spawn_stats: FAILED: " << e.what() << "\n";
		return 1;
	}
}
