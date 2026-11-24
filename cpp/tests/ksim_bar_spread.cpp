// kenshi-k16.6.32 / k16.6.512: the Hub's bar seats its patrons one to a seat. Setup as tools/ksim_hub_census (New Game at the Hub); at t1 (30 s) and 31 game
// minutes later (1130 ticks) asserts: no two seated characters share a seat (the book holds one member per BarSeat, and no two seated bodies are within
// 2 u), and no RELAX 214 character stands within kTableRing of a table without a seat (the member with no free seat takes another goal).
//
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

int main(int argc, char** argv)
try
{
	if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
	{
		std::cout << "ksim_bar_spread: SKIPPED: needs the world navmesh bake (pass its directory)\n";
		return 77;
	}
	constexpr double kCx = -50986.0;
	constexpr double kCz = 3514.0;
	constexpr double kRadius = 1200.0;
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 1;
	options.settleSeconds = 0.0; // a fresh load, not a New Game settle (TOWN_LIFE_SLICE 5.40): rwh.73 measured the day-1 load
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.markPristine();
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), settings);
	std::vector<kworld::ZoneCell> hubZones;
	for (int dz = -1; dz <= 1; ++dz)
	{
		for (int dx = -1; dx <= 1; ++dx)
			hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	}
	sim.enableNavigation(test::openWorldNav(argv[1], hubZones));
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, argv[1], 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim.gameData());
	sim.enableAi(std::move(ai));
	std::string sid;
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
	{
		if (record->name == "Wanderer")
			sid = record->stringID;
	}
	sim.enqueue(Command::newGame(sid, "Tester", 1));
	sim.run(3);
	const auto near = [&](const state::Fighter& f)
	{
		return !f.playerControlled && std::hypot(f.x - kCx, f.z - kCz) <= kRadius;
	};
	const auto count = [&]
	{
		std::size_t n = 0;
		for (const CombatArena::Member& m : sim.combat()->members())
			n += near(m.fighter) ? 1 : 0;
		return n;
	};
	for (int i = 0; i < 400 && count() == 0; ++i)
		sim.run(1);
	const auto check = [&](const char* when)
	{
		std::vector<std::pair<double, double>> seated;
		std::size_t relaxing = 0;
		std::size_t standers = 0;
		std::map<routines::Site, int> holders;
		const flecs::world& ecs = sim.world().ecs();
		ecs.each([&](const state::Id& id, const state::Routine& routine)
		{
			const state::Fighter* f = sim.combat()->find(id.value);
			if (f == nullptr || !near(*f) || routine.goal != routines::task::RelaxInTownPackage)
				return;
			++relaxing;
			const routines::Site held = sim.routineBook().held(id.value);
			if (held.kind == routines::SiteKind::BarSeat)
				++holders[held];
			if (sim.seated(id.value))
				seated.emplace_back(f->x, f->z);
			else if (!f->walking() && held.kind != routines::SiteKind::BarSeat)
			{
				++standers;
				std::cout << "  stander " << f->name << " id " << id.value << " at " << f->x << " " << f->z << " plan " << routine.plan.size() << " step " << routine.step << " target kind " << int(routine.target.kind) << "\n";
			}
		});
		std::size_t stacked = 0;
		for (std::size_t i = 0; i < seated.size(); ++i)
		{
			for (std::size_t j = i + 1; j < seated.size(); ++j)
				stacked += std::hypot(seated[i].first - seated[j].first, seated[i].second - seated[j].second) < 2.0 ? 1 : 0;
		}
		std::size_t shared = 0;
		for (const auto& [site, n] : holders)
			shared += n > 1 ? 1 : 0;
		std::cout << when << ": RELAX " << relaxing << ", seated " << seated.size() << ", stacked pairs " << stacked << ", shared seats " << shared
			<< ", seatless standing " << standers << "\n";
		test::require(relaxing >= 5, std::string(when) + ": the bar's patrons relax");
		test::require(stacked == 0 && shared == 0, std::string(when) + ": no two characters on one seat");
		test::require(standers == 0, std::string(when) + ": no RELAX character stands without a seat");
	};
	// kenshi-h2v.31.1: every hang-out squad of the bar is seated at 31 game minutes (vanilla seats all four patron squads, rwh.73): the real tables
	// hold the first three and the bar's loose stools the fourth (TOWN_LIFE_SLICE 5.39).
	const auto patronsSeated = [&]()
	{
		std::size_t patrons = 0;
		std::size_t thugs = 0;
		std::size_t unseated = 0;
		const flecs::world& ecs = sim.world().ecs();
		ecs.each([&](flecs::entity entity, const state::Id& id, const state::Routine& routine)
		{
			const state::Fighter* f = sim.combat()->find(id.value);
			if (f == nullptr || !near(*f))
				return;
			const flecs::entity squad = entity.target<state::MemberOf>();
			const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
			const kdata::Record* record = instance != nullptr ? sim.gameData()->find(instance->templateSid) : nullptr;
			if (record == nullptr || (record->name != "Bar thugs" && record->name != "Mercenary guild" && record->name != "Hound Pacifier"
				&& record->name.find("Recruits list outlaw") == std::string::npos))
				return;
			++patrons;
			thugs += record->name == "Bar thugs" ? 1 : 0;
			if (routine.goal != routines::task::RelaxInTownPackage || !sim.seated(id.value))
			{
				++unseated;
				std::cout << "  unseated: " << record->name << " " << f->name << " goal " << routine.goal << " at " << f->x << " " << f->z << "\n";
			}
		});
		std::cout << "patrons " << patrons << " (Bar thugs " << thugs << "), unseated " << unseated << "\n";
		test::require(patrons >= 5 && thugs >= 1, "the bar's four patron squads are in the Hub");
		test::require(unseated == 0, "a patron of the bar is not seated on its RELAX goal after 31 game minutes");
	};
	// rwh.73 (findings/rwh73_hub_bar_thugs_live.md): on a fresh load the Bar Thug is not at the bar; he walks in (~495 u) and sits ~20.6 game min
	// after the load. So no Bar thugs member is seated at t0, and he sits only after the walk (not before 10 game min) and within the 31 game min run.
	std::uint64_t elapsed = 0;
	std::map<std::uint64_t, std::uint64_t> thugSeated; // Bar thugs member id -> the tick (after the load) it first sat
	const auto watch = [&](std::uint64_t ticks)
	{
		const flecs::world& ecs = sim.world().ecs();
		for (std::uint64_t t = 0; t < ticks; ++t, ++elapsed)
		{
			ecs.each([&](flecs::entity entity, const state::Id& id, const state::Routine&)
			{
				const state::Fighter* f = sim.combat()->find(id.value);
				if (f == nullptr || !near(*f) || thugSeated.count(id.value) != 0 || !sim.seated(id.value))
					return;
				const flecs::entity squad = entity.target<state::MemberOf>();
				const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
				const kdata::Record* record = instance != nullptr ? sim.gameData()->find(instance->templateSid) : nullptr;
				if (record != nullptr && record->name == "Bar thugs")
					thugSeated[id.value] = elapsed;
			});
			sim.run(1);
		}
	};
	watch(static_cast<std::uint64_t>(30.0 / kTickSeconds));
	check("t1");
	watch(1130);
	check("+31 min");
	test::require(!thugSeated.empty(), "a Bar Thug sits at the bar within the run");
	for (const auto& [id, tick] : thugSeated)
	{
		std::cout << "Bar Thug " << id << " first seated " << static_cast<double>(tick) / kTicksPerGameMinute << " game min after the load\n";
		test::require(tick >= static_cast<std::uint64_t>(10.0 * kTicksPerGameMinute), "a Bar Thug is not pre-seated: he walks in and sits after >= 10 game min (rwh.73: 20.6)");
	}
	patronsSeated();
	std::cout << "ksim_bar_spread: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_bar_spread: FAIL: " << e.what() << "\n";
	return 1;
}
