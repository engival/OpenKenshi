// The fight's decision trace (kenshi-k16.6.59): on the spectate_fight scenario (the player-less fight) a fighter's combat goal / target pick
// goes into the same trace as the goal selectors' (trace::Source::Combat), once per CHANGE: a target held for N ticks adds one row, not N.
// Checks: picks with a target and one Chosen candidate for it; no pick inside a stable (goal, target) window; the state hash is the same
// with the sink on and off.
// Needs the world bake (argv[1], like ksim_scenario_load; exit 77 without its regions).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/debugview/decisions.hpp"
#include "ksim/ai/decision_trace.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/ai/server_ai.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "caravan_unhurt.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr int kRunTicks = 20 * 150; // 150 game s: the fight starts within 60 and runs on
constexpr const char* kName = "spectate_fight";

struct Run
{
	std::uint64_t hash = 0;
	std::vector<trace::Pick> picks; // the Combat picks, in order
	// per scenario character: the stable (goal, target) windows [from, to] sampled after each tick, and the pick ticks
	std::map<std::uint64_t, std::vector<std::uint64_t>> pickTicks;
	std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> windows; // member, from tick, to tick
	std::size_t changes = 0; // (goal, target) changes seen
};

constexpr std::uint64_t kReportLag = 64; // ticks between a (goal, target) change and its row: a retarget made outside pickTarget is reported by the next pick, and with the 1.0 s hold (kenshi-k16.6.134) that pick comes later (spectate_fight: between 55 and 62 ticks; was 12 with a pick every tick: kenshi-h2v.31, kenshi-k16.6.124)

Run run(const scenario::File& file, const std::shared_ptr<MeshNavigator>& mesh, bool traceOn)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	SpawnOptions options;
	options.seed = 11;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
	sim.markPristine();
	const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	sim.enableNavigation(mesh);
	ServerAiOptions ai;
	ai.registryPath = KSIM_AI_TABLE;
	test::require(std::string(enableServerAi(sim, ai)) == "navmesh", "the server AI took the simulation's navigator");
	simlink::ServerContext context;
	context.gameData = sim.gameData();
	context.squadLimit = 50;
	scenario::check(*sim.gameData(), file);
	sim.newWorld();
	sim.run(1);
	const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *combat, context, file, 11);
	test::require(placed.others.size() >= 8, "the two squads are built");
	// This fixture pins the combat trace, not the aid: spawned fighters carry a first aid kit (k16.6.287) and a medic pick lands inside a stable window.
	for (const std::uint64_t id : placed.others)
		stripMedkits(sim, id, id + 1);
	Run out;
	if (traceOn) // the debug view's ring behind it, as the server's --debug-view has it
	{
		sim.setDecisionTrace(debugview::traceSink({}, [&out](const trace::Pick& pick)
		{
			if (pick.source != trace::Source::Combat)
				return;
			out.picks.push_back(pick);
			out.pickTicks[pick.member].push_back(pick.tick);
		}));
	}
	std::map<std::uint64_t, std::tuple<std::uint16_t, std::uint64_t, std::uint64_t>> state; // member -> goal, target, since
	for (int tick = 0; tick < kRunTicks; ++tick)
	{
		sim.tick();
		const std::uint64_t now = sim.clockTick();
		for (std::uint64_t id : placed.others)
		{
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr)
				continue;
			auto it = state.find(id);
			if (it == state.end())
				it = state.emplace(id, std::make_tuple(f->combatGoal, f->target, now)).first;
			auto& [goal, target, since] = it->second;
			if (goal != f->combatGoal || target != f->target)
			{
				if (target != 0)
					out.windows.emplace_back(id, since, now - 1);
				++out.changes;
				goal = f->combatGoal;
				target = f->target;
				since = now;
			}
		}
	}
	for (const auto& [id, s] : state)
	{
		if (std::get<1>(s) != 0)
			out.windows.emplace_back(id, std::get<2>(s), sim.clockTick());
	}
	out.hash = sim.stateHash();
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const scenario::ScenarioInfo info = [&]
		{
			for (const scenario::ScenarioInfo& listed : scenario::listScenarios(KSIM_SCENARIOS_DIR))
				if (listed.name == kName)
					return listed;
			throw std::runtime_error(std::string("scenario not listed: ") + kName);
		}();
		const scenario::File file = scenario::parseFile(info.path);
		if (bake.empty())
		{
			std::cout << "ksim_fight_trace: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_fight_trace: SKIPPED: no " << region << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(bake, zones);
		const Run on = run(file, mesh, true);
		const Run off = run(file, mesh, false);
		test::require(on.hash == off.hash, "the state hash differs with the trace sink on and off");

		// A combat pick has a target and, when a scan ran, exactly one Chosen candidate: the target.
		std::size_t withTarget = 0;
		std::size_t scanned = 0;
		for (const trace::Pick& pick : on.picks)
		{
			test::require(pick.result == trace::Result::Chosen || pick.result == trace::Result::DefaultIdle, "a combat pick has a result");
			if (pick.target == 0)
			{
				test::require(pick.task == 0 && pick.result == trace::Result::DefaultIdle, "a let-go pick names no goal");
				continue;
			}
			++withTarget;
			if (pick.candidates.empty())
				continue; // an attack order: no scan
			++scanned;
			std::size_t chosen = 0;
			for (const trace::Candidate& c : pick.candidates)
			{
				chosen += c.verdict == goalsel::Why::Chosen ? 1 : 0;
				test::require((c.verdict == goalsel::Why::Chosen) == (c.target == pick.target), "the Chosen candidate is the target");
			}
			test::require(chosen == 1, "a combat pick has one Chosen candidate");
		}
		test::require(withTarget > 0 && scanned > 0, "the fight recorded no combat pick with a target (" + std::to_string(on.picks.size()) + " picks)");

		// One row per change, not per tick: no pick lands inside a stable (goal, target) window past its first ticks, and the rate is far below one a tick.
		test::require(on.picks.size() * 20 < static_cast<std::size_t>(kRunTicks) * on.pickTicks.size(), "combat picks come more often than one per 20 ticks per fighter");
		std::size_t longest = 0;
		std::size_t inside = 0;
		for (const auto& [member, from, to] : on.windows)
		{
			longest = std::max<std::size_t>(longest, to - from + 1);
			if (to - from < 40)
				continue;
			const auto found = on.pickTicks.find(member);
			if (found == on.pickTicks.end())
				continue;
			for (std::uint64_t tick : found->second)
				inside += tick > from + kReportLag && tick <= to ? 1 : 0; // a retarget made outside pickTarget is reported by its next pick
		}
		test::require(longest >= 100, "no fighter held a target for 100 ticks (" + std::to_string(longest) + "): the fixture shows no hold");
		test::require(inside == 0, std::to_string(inside) + " combat picks inside a stable (goal, target) window");
		std::printf("fight_trace: %zu combat picks (%zu with a target, %zu from a scan) in %d ticks, %zu (goal, target) changes, longest hold %zu ticks, hash %016llx on = off\n",
			on.picks.size(), withTarget, scanned, kRunTicks, on.changes, longest, static_cast<unsigned long long>(on.hash));
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_fight_trace: %s\n", e.what());
		return 1;
	}
}
