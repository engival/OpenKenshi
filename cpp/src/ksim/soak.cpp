#include "ksim/soak.hpp"

#include "ksim/clock.hpp"
#include "ksim/economy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ksim {

namespace {

struct Watch
{
	double x = 0.0;
	double z = 0.0;
	std::uint64_t movedTick = 0; // last sample at which the squad had moved
	bool stuck = false;
	bool everMoved = false;
};

// One hourly sample of the living world, appended to the CSVs. Pure reads of the world. Respawns are new homeless / nest squads (the
// wilderness spawner's refills); other_new_squads are every other squad that appeared (campaign raiders, merchants, roamers).
void writeCsvRow(const SoakOptions& options, const flecs::world& ecs, std::uint64_t hour, std::set<std::uint64_t>& knownSquads,
	std::set<std::uint64_t>& countedDead, std::uint64_t& respawnsTotal, std::uint64_t& otherNewTotal, std::uint64_t& goneTotal,
	std::uint64_t& deathsDetailTotal, std::uint64_t& deathsFightTotal, std::uint64_t& deathsBledTotal)
{
	struct Tally
	{
		std::size_t squads = 0;
		std::size_t members = 0;
	};
	std::map<flecs::entity_t, Tally> byFaction; // 0 = factionless
	std::map<flecs::entity_t, flecs::entity_t> factionOf; // squad entity -> faction entity
	std::set<std::uint64_t> seen;
	std::size_t squads = 0;
	ecs.each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance)
	{
		++squads;
		seen.insert(id.value);
		++byFaction[instance.faction.id].squads;
		factionOf[squad.id()] = instance.faction.id;
		if (knownSquads.count(id.value) == 0)
		{
			const bool wild = instance.source == state::SpawnSource::BiomeHomeless || instance.source == state::SpawnSource::BiomeNest;
			(wild ? respawnsTotal : otherNewTotal) += 1;
		}
	});
	for (const std::uint64_t id : knownSquads)
		goneTotal += seen.count(id) == 0 ? 1 : 0;
	knownSquads = std::move(seen);

	std::size_t members = 0;
	std::size_t corpses = 0;
	ecs.query_builder<>().with<state::MemberOf>(flecs::Wildcard).build().each([&](flecs::entity member)
	{
		const auto squad = factionOf.find(member.target<state::MemberOf>().id());
		const state::Fighter* fighter = member.get<state::Fighter>();
		if (fighter != nullptr && fighter->dead)
		{
			++corpses;
			// A body counts once, by its cause (CorpseDecay::cause: 2 off-screen fight, 1 bled out off-screen, else a death in detail).
			if (countedDead.insert(member.id()).second)
			{
				const state::CorpseDecay* decay = member.get<state::CorpseDecay>();
				const std::uint8_t cause = decay != nullptr ? decay->cause : state::CorpseDecay::kCauseOther;
				++(cause == state::CorpseDecay::kCauseTokenFight ? deathsFightTotal : cause == state::CorpseDecay::kCauseBledOut ? deathsBledTotal : deathsDetailTotal);
			}
			return;
		}
		++members;
		if (squad != factionOf.end())
			++byFaction[squad->second].members;
	});

	std::uint64_t started = 0;
	std::uint64_t arrived = 0;
	ecs.each([&](const state::FactionAI& faction)
	{
		started += faction.started;
		arrived += faction.arrived;
	});

	if (options.csv != nullptr)
		*options.csv << hour << ',' << hour / 24 << ',' << squads << ',' << members << ',' << goneTotal << ',' << respawnsTotal << ','
			<< otherNewTotal << ',' << deathsDetailTotal << ',' << deathsFightTotal << ',' << deathsBledTotal << ',' << corpses << ',' << started << ',' << arrived << '\n';
	if (options.playerCsv != nullptr)
	{
		std::unordered_map<std::uint64_t, const state::Fighter*> fighters;
		ecs.each([&](const state::Id& id, const state::Fighter& fighter) { fighters[id.value] = &fighter; });
		const auto flag = [](bool value) { return value ? "True" : "False"; };
		ecs.each([&](const state::PlayerSquad& squad)
		{
			for (const std::uint64_t member : squad.members)
			{
				const auto found = fighters.find(member);
				if (found == fighters.end())
					continue;
				const state::Fighter& f = *found->second;
				*options.playerCsv << hour << ',' << f.name << ',' << std::round(f.blood * 100.0) / 100.0 << ',' << flag(f.down) << ',' << flag(f.unconscious) << ','
					<< flag(f.dead) << ',' << std::round(f.x * 10.0) / 10.0 << ',' << std::round(f.y * 10.0) / 10.0 << ',' << std::round(f.z * 10.0) / 10.0 << '\n';
			}
		});
	}
	if (options.factionCsv != nullptr)
	{
		for (const auto& [faction, tally] : byFaction)
		{
			const state::FactionState* state = faction != 0 ? flecs::entity(ecs.c_ptr(), faction).get<state::FactionState>() : nullptr;
			*options.factionCsv << hour << ',' << (state != nullptr ? state->templateSid : std::string("none")) << ',' << tally.squads
				<< ',' << tally.members << '\n';
		}
	}
}

} // namespace

SoakReport runSoak(Simulation& simulation, const SoakOptions& options)
{
	if (simulation.ai() == nullptr)
		throw std::invalid_argument("runSoak needs Simulation::enableAi");
	SoakReport report;
	const auto begin = std::chrono::steady_clock::now();
	const std::uint64_t hours = static_cast<std::uint64_t>(
		std::llround(options.days * 24.0));
	const flecs::world& ecs = simulation.world().ecs();
	const AiStats startStats = simulation.ai()->stats();
	std::unordered_map<std::uint64_t, Watch> watches;
#if KSIM_PROFILE
	const profile::Totals profileStart = simulation.profiler().totals();
#endif

	// The provocations: owned towns and factions in Id order, and the hand-backs due.
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> ownedTowns;
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> factions;
	struct Handback
	{
		std::uint64_t hour = 0;
		std::uint64_t town = 0;
		std::uint64_t owner = 0;
	};
	std::vector<Handback> handbacks;
	const DoorSystem* gates = simulation.ai()->doorSystem();
	if (options.provokeEveryHours != 0 && gates != nullptr)
	{
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::TownState& town)
		{
			if (town.owner.id != 0)
				ownedTowns.emplace_back(id.value, entity.id());
		});
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::FactionState&)
		{
			factions.emplace_back(id.value, entity.id());
		});
		std::sort(ownedTowns.begin(), ownedTowns.end());
		std::sort(factions.begin(), factions.end());
	}
	std::uint64_t provoked = 0;

	// The CSV state: squads seen (by Id) at the last sample, bodies counted as a death already, and the running totals.
	std::set<std::uint64_t> knownSquads;
	std::set<std::uint64_t> countedDead;
	std::uint64_t respawnsTotal = 0;
	std::uint64_t otherNewTotal = 0;
	std::uint64_t goneTotal = 0;
	std::uint64_t deathsDetailTotal = 0;
	std::uint64_t deathsFightTotal = 0;
	std::uint64_t deathsBledTotal = 0;
	const bool wantCsv = options.csv != nullptr || options.factionCsv != nullptr || options.playerCsv != nullptr;
	if (options.csv != nullptr)
		*options.csv << kSoakCsvHeader << '\n';
	if (options.factionCsv != nullptr)
		*options.factionCsv << kSoakFactionCsvHeader << '\n';
	if (options.playerCsv != nullptr)
		*options.playerCsv << kSoakPlayerCsvHeader << '\n';
	if (wantCsv)
		ecs.each([&](const state::Id& id, const state::SquadInstance&) { knownSquads.insert(id.value); });

	for (std::uint64_t hour = 0; hour < hours; ++hour)
	{
		for (const Handback& back : handbacks)
			if (back.hour == hour)
				simulation.enqueue(Command::setTownOwner(state::Id{back.town}, state::Id{back.owner}));
		if (options.provokeEveryHours != 0 && !ownedTowns.empty() && hour > 0 && hour % options.provokeEveryHours == 0)
		{
			// Rotate through the towns with a stride, and take the first faction hostile to the owner.
			const auto& [townId, townEntity] = ownedTowns[(provoked++ * 7) % ownedTowns.size()];
			const flecs::entity town(ecs.c_ptr(), townEntity);
			const flecs::entity_t owner = town.get<state::TownState>()->owner.id;
			if (owner != 0)
			{
				for (const auto& [factionId, factionEntity] : factions)
				{
					if (gates->classOf(factionEntity, owner) != DoorClass::Hostile)
						continue;
					simulation.enqueue(Command::setTownOwner(state::Id{townId}, state::Id{factionId}));
					handbacks.push_back({hour + 12, townId,
						flecs::entity(ecs.c_ptr(), owner).get<state::Id>()->value});
					break;
				}
			}
		}
		simulation.run(kTicksPerGameHour);
		report.ticks += kTicksPerGameHour;
		++report.samples;
		const std::uint64_t now = simulation.clockTick();
		const AiAudit audit = simulation.ai()->audit(ecs);
		report.maxGoalless = std::max(report.maxGoalless, audit.goalless);
		report.maxOffRoute = std::max(report.maxOffRoute, audit.offRoute);
		// Town life when no player is near (T3f): the hourly peak per unloaded phase.
		for (std::size_t phase = 0; phase < report.maxByPhase.size(); ++phase)
			report.maxByPhase[phase] = std::max(report.maxByPhase[phase], audit.byPhase[phase]);
		report.maxUnmodelled = std::max(report.maxUnmodelled, audit.unmodelled);
		// The real-economy overlay (T4a): the towns' stock at this sample (the counters are that sample, not a peak: stock evolves).
		report.economyTowns = 0;
		report.economyLines = 0;
		report.economyEmptyLines = 0;
		report.economyProducers = 0;
		double fillSum = 0.0;
		ecs.each([&](flecs::entity, const state::TownStock& stock)
		{
			++report.economyTowns;
			for (const state::StockLine& line : stock.lines)
			{
				++report.economyLines;
				report.economyEmptyLines += line.amount <= 0.0 ? 1 : 0;
				fillSum += economy::fill(line);
			}
		});
		report.economyFill = report.economyLines != 0 ? fillSum / static_cast<double>(report.economyLines) : 0.0;
		ecs.each([&](flecs::entity, const state::EconomyProducer&) { ++report.economyProducers; });

		if (wantCsv)
			writeCsvRow(options, ecs, hour + 1, knownSquads, countedDead, respawnsTotal, otherNewTotal, goneTotal, deathsDetailTotal, deathsFightTotal, deathsBledTotal);

		report.stuckNow = 0;
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::SquadInstance&)
		{
			const state::Position* position = entity.get<state::Position>();
			if (position == nullptr)
				return;
			const state::GroupAI* ai = entity.get<state::GroupAI>();
			const auto [slot, fresh] = watches.try_emplace(id.value,
				Watch{position->x, position->z, now});
			Watch& watch = slot->second;
			if (!fresh && std::hypot(position->x - watch.x, position->z - watch.z) > 1.0)
			{
				watch.x = position->x;
				watch.z = position->z;
				watch.movedTick = now;
				watch.stuck = false;
				watch.everMoved = true;
			}
			// A raid staying at its target stands still on purpose.
			const bool travelling = ai != nullptr && ai->goal != state::GoalKind::None
				&& ai->goal != state::GoalKind::Idle
				&& !(ai->goal == state::GoalKind::ManGate && ai->route.empty()) // a crew holding its post
				&& !(ai->goal == state::GoalKind::Raid
					&& (ai->order.status == state::OrderStatus::Staying
						|| ai->order.status == state::OrderStatus::Retreating));
			if (!travelling)
			{
				// Standing still on an Idle goal is not stuck.
				watch.movedTick = now;
				watch.stuck = false;
			} else if (ticksSince(now, watch.movedTick) >= options.stuckHours * kTicksPerGameHour) {
				++report.stuckNow;
				if (!watch.stuck)
					++report.stuckEpisodes;
				watch.stuck = true;
			}
		});
	}

	report.last = simulation.ai()->audit(ecs);
	const AiStats& stats = simulation.ai()->stats();
	report.selections = stats.selections - startStats.selections;
	report.arrivals = stats.arrivals - startStats.arrivals;
	for (std::size_t i = 0; i < report.failures.size(); ++i)
		report.failures[i] = stats.failures[i] - startStats.failures[i];
	for (const auto& [id, watch] : watches)
		report.squadsThatMoved += watch.everMoved ? 1 : 0;
#if KSIM_PROFILE
	{
		const profile::Totals& now = simulation.profiler().totals();
		report.profile.ticks = now.ticks - profileStart.ticks;
		report.profile.tickNs = now.tickNs - profileStart.tickNs;
		for (std::size_t i = 0; i < profile::kSections; ++i)
			report.profile.ns[i] = now.ns[i] - profileStart.ns[i];
		report.profile.pathQueries = now.pathQueries - profileStart.pathQueries;
		report.profile.pathNs = now.pathNs - profileStart.pathNs;
		report.profileWindow = simulation.profiler().summary();
	}
#endif
	report.seconds = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - begin).count();
	return report;
}

} // namespace ksim
