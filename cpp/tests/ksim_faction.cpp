// The faction layer, first slice (include/ksim/world/factions.hpp):
//
//  1. data: the FactionBook loads the campaigns and factions of the real data
//     (counts, a known campaign's fields, default relations);
//  2. events flow up: a town lost, a town gained and a kill change the
//     temperament variables and the intent (Defend), and decay back;
//  3. raids run: campaigns are planned, squads spawn at the faction's town,
//     travel, stay, return and are retired; a route that fails when the
//     squads try it fails the raid with a cooldown and a successor is
//     planned; no squad is ever goal-less, none is stuck, no campaign squad
//     leaks (straight-line navigator over the whole world, no bake needed);
//  4. 1 worker == 4 workers, and save + load + run == the uninterrupted run
//     with raids in flight.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/core/soak.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace {

using ksim::state::GoalKind;
using ksim::state::OrderStatus;

// Seen raids, for the cadence, one-at-a-time and range checks.
struct RaidSeen
{
	std::string campaign;
	std::uint64_t planned = 0;
	double distance = 0.0;
	bool aligned = false; // planned on the faction's 6-hour roll clock
};

// Straight lines everywhere; `broken` makes every route fail (a world whose
// roads are cut). Only the serial AI steps call it.
class FakeNavigator final : public ksim::Navigator
{
public:
	explicit FakeNavigator(const bool* broken)
		: broken_(broken)
	{
	}

	bool covers(double, double) const override { return true; }

	ksim::Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		ksim::Route route;
		if (*broken_)
		{
			route.failure = ksim::state::AiReason::NoRoute;
			return route;
		}
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}

private:
	const bool* broken_;
};

ksim::Simulation newGame(const kworld::LoadOrder& order, int workers, std::uint64_t seed = 5)
{
	ksim::SpawnOptions options;
	options.seed = seed;
	options.wildernessByDensity = false;
	ksim::RegionConfig config;
	config.workerThreads = workers;
	return ksim::Simulation::newGame(KENSHI_DATA, order, options, nullptr, config);
}

void enable(ksim::Simulation& simulation, const bool* broken, double frequency,
	double sizeMult = 1.0)
{
	ksim::AiSystem::Config config;
	config.registry = std::make_shared<ksim::BehaviourRegistry>(simulation.world(),
		ksim::BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<FakeNavigator>(broken);
	config.factions = ksim::FactionBook::build(simulation.gameData());
	config.factionTuning.raidFrequency = frequency;
	config.factionTuning.raidSizeMult = sizeMult;
	config.factionTuning.npcRaids = true; // these cases drive raids on NPC towns (world.npc_raids on; the default is player towns only, kenshi-51x.5)
	simulation.enableAi(std::move(config));
}

struct Totals
{
	std::uint32_t planned = 0;
	std::uint32_t started = 0;
	std::uint32_t arrived = 0;
	std::uint32_t completed = 0;
	std::uint32_t failed = 0;
	std::uint32_t rejected = 0;
	std::size_t running = 0;
	std::size_t cooldowns = 0;
	std::size_t raiders = 0; // campaign squads alive
	std::size_t referenced = 0; // ... named by a raid
};

Totals totals(const ksim::Simulation& simulation)
{
	Totals total;
	const flecs::world& ecs = simulation.world().ecs();
	ecs.each([&](const ksim::state::FactionAI& f)
	{
		total.planned += f.planned;
		total.started += f.started;
		total.arrived += f.arrived;
		total.completed += f.completed;
		total.failed += f.failed;
		total.rejected += f.rejected;
		total.running += f.raids.size();
		total.cooldowns += f.targetCooldowns.size() + f.campaignCooldowns.size();
		for (const ksim::state::Raid& raid : f.raids)
			total.referenced += raid.squads.size();
	});
	ecs.each([&](const ksim::state::SquadInstance& squad)
	{
		total.raiders += squad.source == ksim::state::SpawnSource::Campaign ? 1 : 0;
	});
	return total;
}

// Every squad has a goal; an order never sits Issued past the squad step after
// the hour it was given.
void checkSquads(const ksim::Simulation& simulation)
{
	const ksim::AiAudit audit = simulation.ai()->audit(simulation.world().ecs());
	test::require(audit.goalless == 0, std::to_string(audit.goalless) + " squads without a goal");
	const std::uint64_t now = simulation.clockTick();
	simulation.world().ecs().each([&](const ksim::state::SquadInstance& squad,
		const ksim::state::GroupAI& ai)
	{
		test::require(ai.order.status == OrderStatus::None
			|| squad.source == ksim::state::SpawnSource::Campaign,
			"a squad that is not a campaign raider holds an order");
		if (ai.order.status == OrderStatus::Issued)
			test::require(now - ai.order.issued <= ksim::kTicksPerGameHour,
				"an order sat Issued for more than an hour");
		if (ai.goal == GoalKind::Raid)
			test::require(ai.order.status != OrderStatus::None && ai.order.status != OrderStatus::Done
				&& ai.order.status != OrderStatus::Failed, "a Raid goal with a finished order");
	});
}

bool broken(std::uint64_t tick, bool enabled)
{
	// For the first minutes of every second 6-hour roll pass (the game starts at
	// hour 33 and the first faction step is hour 34, so the roll hours are
	// 4 mod 6 and these 4 mod 12), just after the faction step has planned and
	// before the squads have all started.
	const std::uint64_t phase = tick % ksim::kTicksPerGameHour;
	return enabled && (tick / ksim::kTicksPerGameHour) % 12 == 4 && phase >= 1 && phase < 120;
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// 1. Data.
		{
			ksim::Simulation simulation = newGame(order, 1);
			const auto book = ksim::FactionBook::build(simulation.gameData());
			test::require(book->campaigns().size() >= 170, "too few campaigns: "
				+ std::to_string(book->campaigns().size()));
			std::size_t eligible = 0;
			std::size_t withForce = 0;
			for (const ksim::CampaignInfo& campaign : book->campaigns())
			{
				eligible += campaign.eligible ? 1 : 0;
				withForce += !campaign.squads.empty() ? 1 : 0;
			}
			test::require(eligible > 80 && eligible < book->campaigns().size(),
				"world-state eligibility did not split the campaigns");
			test::require(book->runnable() >= 20, "too few runnable campaigns: "
				+ std::to_string(book->runnable()));
			const ksim::CampaignInfo* fog = nullptr;
			for (const ksim::CampaignInfo& campaign : book->campaigns())
				fog = campaign.name == "Fog standard raid" ? &campaign : fog;
			test::require(fog != nullptr && fog->territorial && fog->eligible && fog->numForces == 40
				&& fog->rangeFar == 1600.0 && fog->rangeNear == 1200.0 && !fog->squads.empty(),
				"Fog standard raid is not what the data says");
			std::size_t withCampaigns = 0;
			std::size_t chanceIgnoring = 0;
			std::int32_t cannibalDefault = 0;
			simulation.world().ecs().each([&](const ksim::state::FactionState& faction)
			{
				const ksim::FactionInfo* info = book->faction(faction.templateSid);
				test::require(info != nullptr, "a FactionState has no FactionInfo");
				withCampaigns += info->campaigns.empty() ? 0 : 1;
				cannibalDefault = info->name == "Cannibals" ? info->defaultRelation : cannibalDefault;
			});
			test::require(withCampaigns >= 20, "too few factions list campaigns");
			std::size_t withBiomes = 0;
			std::size_t withNoGo = 0;
			for (const ksim::CampaignInfo& campaign : book->campaigns())
				chanceIgnoring += campaign.ignoresChanceMults ? 1 : 0;
			simulation.world().ecs().each([&](const ksim::state::FactionState& faction)
			{
				const ksim::FactionInfo* info = book->faction(faction.templateSid);
				withBiomes += info->biomes.empty() ? 0 : 1;
				withNoGo += info->noGo.empty() ? 0 : 1;
			});
			test::require(withBiomes >= 10 && withNoGo >= 10, "the factions' biomes / no-go zones did not load");
			test::require(cannibalDefault == -100, "the Cannibals' default relation is not -100");
			std::cout << "data: " << book->campaigns().size() << " campaigns, " << eligible
				<< " eligible at the default, " << withForce << " with a force list, "
				<< book->runnable() << " runnable, " << withCampaigns
				<< " factions with campaigns, " << withBiomes << " with biomes, " << withNoGo
				<< " with no-go zones, " << chanceIgnoring << " ignoring chance mults\n";
		}

		// 1b. The relation reasons (FACTIONS.md 3.1): floors, overshoot by one delta, clamp.
		{
			using ksim::RelationReason;
			const auto after = [](double relation, RelationReason reason, double amount, double dt = 1.0)
			{
				return ksim::relationAfter(relation, reason, amount, dt);
			};
			test::require(ksim::FactionBook::kHostile == -30.0 && ksim::FactionBook::kAlly == 50.0,
				"the thresholds are not -30 / 50");
			// A kill: -20 x A, floor -75.
			test::require(after(0, RelationReason::Kill, 1.0) == -20.0, "a kill is -20 x A");
			test::require(after(0, RelationReason::Kill, 0.025) == -0.5, "an unheld kill is -0.5");
			test::require(after(-55, RelationReason::Kill, 1.0) == -75.0, "a kill reaches its floor");
			test::require(after(-70, RelationReason::Kill, 1.0) == -90.0,
				"a kill overshoots its floor by one delta");
			test::require(after(-90, RelationReason::Kill, 1.0) == -90.0, "below the floor nothing moves");
			test::require(after(-75, RelationReason::Kill, 1.0) == -75.0, "at the floor nothing moves");
			// A hit: heavy -10 x A, light -2.5 x A, both floor -50; the aggression tick -0.05 x A, floor -20.
			test::require(after(0, RelationReason::HeavyHit, 0.025) == -0.25, "a heavy hit is -0.25");
			test::require(after(-45, RelationReason::HeavyHit, 1.0) == -55.0, "a heavy hit overshoots -50");
			test::require(after(-50, RelationReason::Hit, 1.0) == -50.0, "a hit stops at -50");
			test::require(after(-19.99, RelationReason::Aggression, 1.0) == -20.04, "aggression overshoots -20");
			test::require(after(-20.04, RelationReason::Aggression, 1.0) == -20.04, "aggression stops under -20");
			// Reason 4 has the default floor (-100) and clamps there; 12 is floor -60.
			test::require(after(-50, RelationReason::Unprovoked, 1.0) == -70.0, "reason 4 goes on past -50");
			test::require(after(-90, RelationReason::Unprovoked, 1.0) == -100.0, "reason 4 clamps at -100");
			test::require(after(-100, RelationReason::Unprovoked, 1.0) == -100.0, "-100 stays");
			test::require(after(0, RelationReason::Capture, 1.0) == -10.0 && after(-55, RelationReason::Capture, 1.0) == -65.0,
				"reason 12 is -10 x A, floor -60");
			// The witness betrayal (ours, 13; rwh138 case-4 block): one flat -20 per firing, clamped at -100 like vf4.
			test::require(after(70, RelationReason::Betrayal, 1.0) == 50.0 && after(50, RelationReason::Betrayal, 1.0) == 30.0,
				"betrayal is -20 per firing (70 -> 50 -> 30)");
			test::require(after(-90, RelationReason::Betrayal, 1.0) == -100.0 && after(-100, RelationReason::Betrayal, 1.0) == -100.0,
				"betrayal clamps at -100");
			// Treatment: +0.15 x A x dt, ceiling +5, clamp 100.
			test::require(std::abs(after(0, RelationReason::Treatment, 2.0, 0.5) - 0.15) < 1e-12,
				"treatment scales with the frame delta");
			test::require(std::abs(after(4.95, RelationReason::Treatment, 1.0, 1.0) - 5.1) < 1e-12,
				"treatment overshoots +5 by one delta");
			test::require(after(5.1, RelationReason::Treatment, 1.0, 1.0) == 5.1, "treatment stops above +5");
			test::require(after(4.0, RelationReason::Treatment, 1000.0, 1.0) == 100.0, "the relation clamps at +100");
			// Codes with no effect.
			for (const int code : {6, 7, 9, 10, 11, 0})
				test::require(after(10.0, static_cast<RelationReason>(code), 5.0) == 10.0,
					"a code with no relation effect changed the relation");
			// The campaign timings.
			test::require(ksim::onSiteHours(0) == 4.0 && ksim::onSiteHours(2) == 12.0
				&& ksim::onSiteHours(3) == 1.0 && ksim::onSiteHours(1) == 4.0, "on-site hours by key");
			std::cout << "relation reasons: floors, one-delta overshoot, clamp and no-effect codes ok\n";
		}

		// 1c. A kill by a faction lowers the victim's opinion of the killer (reason 5, per member).
		{
			bool cut = false;
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, &cut, 0.0);
			simulation.run(ksim::kTicksPerGameHour + 1);
			const flecs::world& ecs = simulation.world().ecs();
			flecs::entity victimFaction;
			flecs::entity killerFaction;
			std::vector<std::pair<std::uint64_t, flecs::entity>> squads; // two squads of the victim
			ecs.each([&](flecs::entity entity, const ksim::state::Id& id, const ksim::state::SquadInstance& s)
			{
				if (s.faction.id == 0 || squads.size() >= 2)
					return;
				if (!victimFaction.is_valid())
					victimFaction = flecs::entity(ecs, s.faction.id);
				if (s.faction.id == victimFaction.id())
					squads.emplace_back(id.value, entity);
			});
			ecs.each([&](flecs::entity entity, const ksim::state::FactionState&)
			{
				if (!killerFaction.is_valid() && victimFaction.is_valid() && entity.id() != victimFaction.id())
					killerFaction = entity;
			});
			test::require(squads.size() == 2 && killerFaction.is_valid(), "no squads or killer faction to use");
			const auto opinion = [&]()
			{
				return ksim::factionRelation(*simulation.gameData(), victimFaction, killerFaction);
			};
			const double before = opinion();
			// A kill with no killer changes nothing between the two.
			simulation.enqueue(ksim::Command::removeSquad(ksim::state::Id{squads[0].first}));
			simulation.run(1);
			test::require(opinion() == before, "a kill with no killer changed a relation");
			// The second squad, killed by the killer.
			double expected = before;
			std::size_t members = 0;
			ecs.query_builder<const ksim::state::CharacterInstance>().with<ksim::state::MemberOf>(squads[1].second)
				.build().each([&](const ksim::state::CharacterInstance& character)
				{
					const kdata::Record* record = simulation.gameData()->find(character.templateSid);
					expected = ksim::relationAfter(expected, ksim::RelationReason::Kill,
						ksim::kRelationUnheld * (record != nullptr ? record->floatOr("faction importance", 1.0f) : 1.0));
					++members;
				});
			test::require(members > 0, "the squad has no members");
			simulation.enqueue(ksim::Command::removeSquad(ksim::state::Id{squads[1].first},
				ksim::state::Id{killerFaction.get<ksim::state::Id>()->value}));
			simulation.run(1);
			test::require(std::abs(opinion() - expected) < 1e-9 && opinion() < before,
				"the victim's opinion of the killer is not the sum of its members' kills: "
				+ std::to_string(opinion()) + " vs " + std::to_string(expected));
			std::cout << "kill: a " << members << "-member squad, relation " << before << " -> " << opinion() << "\n";
		}

		// 2. Events flow up.
		{
			bool cut = false;
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, &cut, 0.0);
			simulation.run(ksim::kTicksPerGameHour + 1);
			std::uint64_t townId = 0;
			std::uint64_t ownerId = 0;
			std::uint64_t otherId = 0;
			std::uint64_t victimId = 0;
			flecs::entity_t ownerEntity = 0;
			simulation.world().ecs().each([&](flecs::entity entity, const ksim::state::FactionState&)
			{
				test::require(entity.get<ksim::state::FactionAI>() != nullptr,
					"a faction has no FactionAI after a strategic step");
			});
			simulation.world().ecs().each([&](const ksim::state::Id& id,
				const ksim::state::TownState& town)
			{
				if (townId == 0 && town.owner.id != 0)
				{
					townId = id.value;
					ownerEntity = town.owner.id;
					ownerId = flecs::entity(simulation.world().ecs(), town.owner.id)
						.get<ksim::state::Id>()->value;
				}
			});
			simulation.world().ecs().each([&](const ksim::state::Id& id,
				const ksim::state::FactionState&)
			{
				if (id.value != ownerId && otherId == 0)
					otherId = id.value;
			});
			simulation.world().ecs().each([&](const ksim::state::Id& id,
				const ksim::state::SquadInstance& squad)
			{
				if (victimId == 0 && squad.faction.id == ownerEntity)
				{
					victimId = id.value;
				}
			});
			test::require(townId != 0 && otherId != 0 && victimId != 0, "no town, faction or squad to use");
			const auto owner = [&]()
			{
				return *flecs::entity(simulation.world().ecs(), ownerEntity)
					.get<ksim::state::FactionAI>();
			};
			const auto other = [&]()
			{
				const ksim::state::FactionAI* found = nullptr;
				simulation.world().ecs().each([&](const ksim::state::Id& id,
					const ksim::state::FactionAI& ai)
				{
					found = id.value == otherId ? &ai : found;
				});
				return *found;
			};
			test::require(owner().alarm == 0.0 && owner().grief == 0.0, "a calm faction is alarmed");
			simulation.enqueue(ksim::Command::setTownOwner(ksim::state::Id{townId},
				ksim::state::Id{otherId}));
			simulation.enqueue(ksim::Command::removeSquad(ksim::state::Id{victimId}));
			simulation.run(ksim::kTicksPerGameHour);
			test::require(owner().alarm > 0.3 && owner().grief > 0.2, "losses did not alarm the owner");
			test::require(owner().intent == ksim::state::Intent::Defend,
				"a faction that lost a town does not defend");
			test::require(owner().squadsLost == 1 && owner().events == 2, "the owner's events were not counted");
			test::require(other().confidence > 0.2 && other().events == 1,
				"the new owner did not gain confidence");
			simulation.run(ksim::kTicksPerGameHour * 24 * 4);
			test::require(owner().alarm < 0.1 && owner().grief < 0.1, "the temperament did not decay");
			test::require(owner().intent != ksim::state::Intent::Defend
				&& owner().intent != ksim::state::Intent::Mourn,
				"the faction did not settle back from defend");
			std::cout << "events: alarm/grief rose and decayed, intent defend -> settled\n";
		}

		// 3. Raids, including route failures when the squads try to start.
		{
			bool cut = false;
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, &cut, 40.0);
			std::set<std::uint32_t> seenRunning;
			std::size_t mostRunning = 0;
			std::size_t staying = 0;
			std::size_t returning = 0;
			std::size_t retreating = 0;
			constexpr int kDays = 8;
			const auto book = ksim::FactionBook::build(simulation.gameData());
			std::set<std::string> followUps; // campaigns a chain can start (no roll, no range)
			for (const ksim::CampaignInfo& campaign : book->campaigns())
			{
				for (const auto* list : {&campaign.onVictory, &campaign.onLoss})
					for (const ksim::CampaignInfo::Follow& follow : *list)
						followUps.insert(follow.campaign);
			}
			std::map<std::pair<flecs::entity_t, std::uint64_t>, RaidSeen> seen;
			std::size_t mostPerFaction = 0;
			std::size_t beyondMetres = 0; // rolled raids further than `range far` read as units
			std::size_t rolled = 0;
			for (std::uint64_t i = 0; i < kDays * 24 * ksim::kTicksPerGameHour; ++i)
			{
				cut = broken(simulation.clockTick() + 1, true);
				simulation.run(1);
				if (i % ksim::kTicksPerGameHour == 0)
				{
					const flecs::world& ecs = simulation.world().ecs();
					ecs.each([&](flecs::entity faction, const ksim::state::FactionAI& f)
					{
						mostPerFaction = std::max(mostPerFaction, f.raids.size());
						for (const ksim::state::Raid& raid : f.raids)
						{
							const auto key = std::make_pair(faction.id(), raid.planned);
							if (seen.count(key) != 0)
								continue;
							const auto* from = flecs::entity(ecs, raid.origin.id).get<ksim::state::TownState>();
							const auto* to = flecs::entity(ecs, raid.target.id).get<ksim::state::TownState>();
							RaidSeen entry;
							entry.campaign = raid.campaign;
							entry.planned = raid.planned;
							entry.distance = std::hypot(from->x - to->x, from->z - to->z);
							constexpr std::uint64_t kRoll = 6 * ksim::kTicksPerGameHour;
							entry.aligned = raid.planned % kRoll == f.nextRoll % kRoll;
							seen.emplace(key, entry);
						}
					});
				}
				if (i % 100 == 0 && i > 100)
				{
					checkSquads(simulation);
					simulation.world().ecs().each([&](const ksim::state::GroupAI& ai)
					{
						staying += ai.order.status == OrderStatus::Staying ? 1 : 0;
						returning += ai.order.status == OrderStatus::Returning ? 1 : 0;
						retreating += ai.order.status == OrderStatus::Retreating ? 1 : 0;
					});
				}
				if (i % ksim::kTicksPerGameHour == 0)
					mostRunning = std::max(mostRunning, totals(simulation).running);
			}
			cut = false;
			// One campaign at a time per faction; rolled campaigns start on the 6-hour roll
			// clock and within `range far` as metres x 10 (a chain's follow-up is exempt).
			test::require(mostPerFaction == 1, "a faction ran " + std::to_string(mostPerFaction)
				+ " campaigns at once (expected exactly 1 at most)");
			for (const auto& [key, entry] : seen)
			{
				if (followUps.count(entry.campaign) != 0)
					continue;
				const ksim::CampaignInfo& campaign = *book->campaign(entry.campaign);
				++rolled;
				test::require(entry.aligned, "a rolled campaign started off the faction's 6-hour roll clock");
				if (campaign.rangeFar > 0.0)
				{
					test::require(entry.distance <= campaign.rangeFar * ksim::kUnitsPerMetre + 1e-6,
						"a rolled raid is beyond range far x 10: " + entry.campaign);
					beyondMetres += entry.distance > campaign.rangeFar ? 1 : 0;
				}
			}
			test::require(rolled >= 3 && beyondMetres > 0,
				"no rolled raid was further than range far read as units (the metres bug): rolled "
				+ std::to_string(rolled) + ", beyond " + std::to_string(beyondMetres));
			const ksim::FactionStats& rollStats = *simulation.ai()->factionStats();
			test::require(rollStats.rolls > 0 && rollStats.rollPassed > 0, "no roll pass drew a campaign");
			const Totals t = totals(simulation);
			test::require(t.planned >= 5, "too few raids planned: " + std::to_string(t.planned));
			test::require(t.arrived > 0 && t.completed > 0, "no raid arrived and came home");
			test::require(t.failed > 0 && t.cooldowns > 0,
				"a cut road did not fail a raid with a cooldown (planned " + std::to_string(t.planned)
				+ ", failed " + std::to_string(t.failed) + ", rejected " + std::to_string(t.rejected)
				+ ", cooldowns " + std::to_string(t.cooldowns) + ", rolls " + std::to_string(rollStats.rolls)
				+ ")");
			test::require(t.planned > t.failed, "failed raids were not followed by others");
			test::require(t.referenced == t.raiders, "campaign squads leaked or are unnamed: "
				+ std::to_string(t.raiders) + " alive, " + std::to_string(t.referenced) + " named");
			test::require(staying > 0 && returning > 0 && retreating > 0,
				"no raider was seen staying, retreating and returning");
			const ksim::FactionStats& stats = *simulation.ai()->factionStats();
			test::require(stats.squadsRetired > 0, "no finished raider was retired");
			std::cout << "raids over " << kDays << " days: planned " << t.planned << ", started "
				<< t.started << ", arrived " << t.arrived << ", completed " << t.completed
				<< ", failed " << t.failed << ", rejected " << t.rejected << ", running "
				<< t.running << " (most " << mostRunning << "), cooldowns " << t.cooldowns
				<< ", follow-ups " << stats.triggersFired << ", squads spawned "
				<< stats.squadsSpawned << " retired " << stats.squadsRetired << "; roll passes "
				<< stats.rolls << " (" << stats.rollCandidates << " campaigns drawn, "
				<< stats.rollPassed << " passed), " << rolled << " rolled raids on the 6 h clock, "
				<< beyondMetres << " beyond 'range far' as metres, territory refused "
				<< stats.territoryRefused << "\n";
		}

		// 3b. A force whose alive fraction is under 0.2 is defeated: the rest is retired, the
		// campaign ends as a loss (cooldowns, loss triggers), the faction goes on.
		{
			bool cut = false;
			ksim::Simulation simulation = newGame(order, 1);
			enable(simulation, &cut, 40.0, 6.0); // raidSizeMult 6: int(num forces x 6) men, up to 12 squads
			flecs::entity_t raider = 0;
			ksim::state::Raid chosen;
			for (int hour = 0; hour < 24 * 10 && raider == 0; ++hour)
			{
				simulation.run(ksim::kTicksPerGameHour);
				simulation.world().ecs().each([&](flecs::entity faction, const ksim::state::FactionAI& f)
				{
					for (const ksim::state::Raid& raid : f.raids)
					{
						if (raider == 0 && raid.squads.size() >= 6 && raid.squadsLost == 0)
						{
							raider = faction.id();
							chosen = raid;
						}
					}
				});
			}
			test::require(raider != 0, "no raid with six squads to defeat");
			const std::uint32_t failedBefore = flecs::entity(simulation.world().ecs(), raider)
				.get<ksim::state::FactionAI>()->failed;
			// Kill all but one squad: 1 / n alive, under 0.2 for n >= 6.
			for (std::size_t i = 1; i < chosen.squads.size(); ++i)
				simulation.enqueue(ksim::Command::removeSquad(ksim::state::Id{chosen.squads[i]}));
			simulation.run(ksim::kTicksPerGameHour);
			const ksim::state::FactionAI& after = *flecs::entity(simulation.world().ecs(), raider)
				.get<ksim::state::FactionAI>();
			test::require(simulation.ai()->factionStats()->defeated >= 1, "the force was not declared defeated");
			test::require(std::none_of(after.raids.begin(), after.raids.end(), [&](const ksim::state::Raid& raid)
				{ return raid.campaign == chosen.campaign && raid.planned == chosen.planned; }),
				"the defeated campaign is still running");
			test::require(after.failed == failedBefore + 1, "the defeat did not count as a loss");
			bool survivor = false;
			simulation.world().ecs().each([&](const ksim::state::Id& id, const ksim::state::SquadInstance&)
			{
				survivor = survivor || id.value == chosen.squads[0];
			});
			test::require(!survivor, "the last squad of a defeated force was not retired");
			std::cout << "defeat: " << chosen.squads.size() << "-squad force down to 1 -> defeated, retired, loss counted\n";
		}

		// 4. Workers and continuation.
		{
			constexpr std::uint64_t kHours = 24 * 3;
			bool cutOne = false;
			bool cutFour = false;
			ksim::Simulation one = newGame(order, 1);
			ksim::Simulation four = newGame(order, 4);
			enable(one, &cutOne, 40.0);
			enable(four, &cutFour, 40.0);
			one.run(kHours * ksim::kTicksPerGameHour);
			four.run(kHours * ksim::kTicksPerGameHour);
			test::require(one.stateHash() == four.stateHash(), "1 worker and 4 workers differ");
			const Totals t = totals(one);
			test::require(t.planned > 0, "no raid planned in the determinism run");

			// Save with raids in flight: find such a moment first.
			bool cut = false;
			ksim::Simulation reference = newGame(order, 1);
			enable(reference, &cut, 40.0);
			std::size_t inFlight = 0;
			for (int hour = 0; hour < 24 * 8 && inFlight == 0; ++hour)
			{
				reference.run(ksim::kTicksPerGameHour);
				reference.world().ecs().each([&](const ksim::state::GroupAI& ai)
				{
					inFlight += ai.order.status == OrderStatus::Staying
						|| ai.order.status == OrderStatus::Outbound ? 1 : 0;
				});
			}
			test::require(inFlight > 0, "no raid in flight to save");
			// A kill with a killer, so the saved state holds a changed (fractional) relation.
			{
				const flecs::world& ecs = reference.world().ecs();
				std::uint64_t squadId = 0;
				flecs::entity victim;
				ecs.each([&](const ksim::state::Id& id, const ksim::state::SquadInstance& squad)
				{
					if (squadId == 0 && squad.source != ksim::state::SpawnSource::Campaign
						&& squad.faction.id != 0)
					{
						squadId = id.value;
						victim = flecs::entity(ecs, squad.faction.id);
					}
				});
				flecs::entity killer;
				ecs.each([&](flecs::entity entity, const ksim::state::FactionState&)
				{
					if (!killer.is_valid() && entity.id() != victim.id()
						&& ksim::factionRelation(*reference.gameData(), victim, entity) > 0.0)
						killer = entity;
				});
				test::require(squadId != 0 && killer.is_valid(), "no squad and friendly killer for the save");
				const double before = ksim::factionRelation(*reference.gameData(), victim, killer);
				reference.enqueue(ksim::Command::removeSquad(ksim::state::Id{squadId},
					ksim::state::Id{killer.get<ksim::state::Id>()->value}));
				reference.run(1);
				test::require(ksim::factionRelation(*reference.gameData(), victim, killer) < before,
					"the kill did not lower the relation before the save");
			}
			reference.run(36);
			const std::uint64_t split = reference.stateHash();
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_faction_test.ksav";
			reference.saveGame(path);
			const std::uint64_t after = 30 * ksim::kTicksPerGameHour + 11;
			reference.run(after);
			for (const int workers : {4, 1})
			{
				ksim::RegionConfig config;
				config.workerThreads = workers;
				bool cutLoaded = false;
				ksim::Simulation loaded = ksim::Simulation::loadGame(KENSHI_DATA, order, path,
					false, nullptr, config);
				test::require(loaded.stateHash() == split, "the loaded game hashes differently");
				enable(loaded, &cutLoaded, 40.0);
				loaded.run(after);
				test::require(loaded.stateHash() == reference.stateHash(),
					"load + run differs from the uninterrupted run (" + std::to_string(workers)
					+ " workers)");
			}
			std::remove(path.c_str());
			std::cout << "determinism: 1 = 4 workers over " << kHours << " hours (" << t.planned
				<< " raids); continuation with " << inFlight << " raiders in flight identical\n";
		}
		std::cout << "ksim_faction: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_faction: " << error.what() << "\n";
		return 1;
	}
}
