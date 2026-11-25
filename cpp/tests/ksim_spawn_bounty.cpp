// A world-spawned character rolls its CHARACTER record's bounty (rwh.193 section 2, Character::vf111), as a new-game member does: chance c
// fires at c/101, the amount is `bounty amount` + int(randInt(0, int(fuzz*2)) * 0.5) on each `bounty factions` entry (United Cities when
// empty); a record with no `bounty chance` holds none. Builds one squad template per seed on a scratch world, over a seed sweep.
#include "ksim/core/rng.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/state.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/world/spawn.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const kdata::GameData& data = *sim.gameData();

		// A squad template whose fixed `leader` is a non-unique CHARACTER with a partial bounty chance, and one whose leader has none.
		const kdata::Record* withBounty = nullptr;
		const kdata::Record* without = nullptr;
		const kdata::Record* bountyLeader = nullptr;
		for (const kdata::Record* squad : data.byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		{
			const kdata::RefCategory* leaders = squad->findRefs("leader");
			if (leaders == nullptr || leaders->entries.size() != 1 || leaders->entries[0].v0 != 1 || squad->findRefs("squad") != nullptr
				|| squad->findRefs("squad2") != nullptr || squad->findRefs("choosefrom list") != nullptr || squad->findRefs("animals") != nullptr
				|| squad->findRefs("animals2") != nullptr)
				continue;
			const kdata::Record* leader = data.find(leaders->entries[0].targetID);
			if (leader == nullptr || leader->boolOr("unique", false))
				continue;
			const int chance = leader->intOr("bounty chance", 0);
			if (chance >= 20 && chance <= 80 && withBounty == nullptr)
			{
				withBounty = squad;
				bountyLeader = leader;
			} else if (chance == 0 && without == nullptr) {
				without = squad;
			}
		}
		test::require(withBounty != nullptr && without != nullptr, "data has a lone-leader template with a partial bounty chance and one with none");

		const int chance = bountyLeader->intOr("bounty chance", 0);
		const int amount = bountyLeader->intOr("bounty amount", 0);
		const int fuzz = bountyLeader->intOr("bounty amount fuzz", 0);
		std::set<std::string> holders;
		if (const kdata::RefCategory* factions = bountyLeader->findRefs("bounty factions"))
		{
			for (const kdata::RefEntry& entry : factions->entries)
				holders.insert(entry.targetID);
		}
		if (holders.empty())
		{
			for (const kdata::Record* faction : data.byType(kworld::itemTypeByName("FACTION")))
			{
				if (faction->name == "United Cities")
					holders.insert(faction->stringID);
			}
		}

		const int kSeeds = 2000;
		int held = 0;
		int noneHeld = 0;
		for (int seed = 1; seed <= kSeeds; ++seed)
		{
			for (const kdata::Record* template_ : {withBounty, without})
			{
				flecs::world ecs;
				RngStream rng(static_cast<std::uint64_t>(seed));
				const double hour = 7.5 * seed; // a squad built at hour H stamps H
				SquadBuilder builder(ecs, data, rng, hour);
				const flecs::entity squad = builder.build(*template_, 0, 0, state::SpawnSource::BiomeHomeless);
				test::require(builder.characters() == 1, "the template spawns its one leader");
				ecs.each([&](const state::CharacterInstance& who, const state::CrimeRecord* crime)
				{
					if (template_ == without)
					{
						noneHeld += crime != nullptr && !crime->bounties.empty();
						return;
					}
					test::require(who.templateSid == bountyLeader->stringID, "the leader is the template's");
					if (crime == nullptr)
						return;
					++held;
					std::set<std::string> got;
					for (const state::Bounty& bounty : crime->bounties)
					{
						got.insert(bounty.factionSid);
						test::require(bounty.stamp == hour, "the bounty is stamped with the builder's game hour");
						test::require(bounty.amount >= amount && bounty.amount <= amount + fuzz, "bounty amount within amount..amount+fuzz");
					}
					test::require(got == holders, "bounty factions are the record's (United Cities when empty)");
				});
				(void)squad;
			}
		}
		test::require(noneHeld == 0, "a record with no `bounty chance` holds no bounty");
		const double expected = static_cast<double>(chance) / 101.0 * kSeeds;
		const double slack = 4.0 * std::sqrt(expected * (1.0 - chance / 101.0));
		std::cout << bountyLeader->stringID << ": chance " << chance << " held " << held << "/" << kSeeds << " expected " << expected << " +-" << slack << "\n";
		test::require(std::fabs(held - expected) <= slack, "the bounty is held at the record's chance c/101 over the seed sweep");
		std::cout << "ksim_spawn_bounty OK\n";
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
