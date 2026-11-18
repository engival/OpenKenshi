// kenshi-k16.6.32: the Hub's bar seats its patrons one to a seat. Setup as tools/ksim_hub_census (New Game at the Hub); at t1 (30 s) and 31 game
// minutes later (1130 ticks) asserts: no two seated characters share a seat (the book holds one member per BarSeat, and no two seated bodies are within
// 2 u), and no RELAX 214 character stands within kTableRing of a table without a seat (the member with no free seat takes another goal).
//
// argv[1]: the world navmesh bake directory (knav_cache).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/routines.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "nav_source.hpp"

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
	sim.run(static_cast<std::uint64_t>(30.0 / kTickSeconds));
	check("t1");
	sim.run(1130);
	check("+31 min");
	patronsSeated();
	std::cout << "ksim_bar_spread: OK\n";
	return 0;
}
catch (const std::exception& e)
{
	std::cerr << "ksim_bar_spread: FAIL: " << e.what() << "\n";
	return 1;
}
