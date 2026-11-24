// kenshi-k16.6.640: a squad outside every Full region steps every 5 s with the elapsed time. A far raid walks the same route at the second's
// cadence (AiSystem::setFarSquadPeriod(20)) and at the 5 s gate; its arrival tick must be within one 5 s step (100 ticks) of the 1 Hz run,
// and the gated run must step the squad about a fifth as often.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/nav/navigator.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "sim_access.hpp"
#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

struct Run
{
	std::uint64_t arrival = 0; // ticks after the order until the raid is on site
	std::uint64_t steps = 0;
	double distance = 0.0;
};

Run run(const kworld::LoadOrder& order, const std::shared_ptr<const CombatData>& combat, std::uint64_t period)
{
	SpawnOptions options;
	options.seed = 5;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.setResidentsArrive(false);
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
	const_cast<AiSystem*>(sim.ai())->setFarSquadPeriod(period);
	const flecs::entity squad = sim.placeWildernessSquad("1302-gamedata.base", -48880.0, -823.0, 15);
	const std::uint64_t id = squad.get<state::Id>()->value;
	sim.run(60);
	flecs::entity town;
	double distance = 0.0;
	ecsOf(sim).each([&](flecs::entity e, const state::TownState& at)
	{
		const double d = std::hypot(at.x + 48880.0, at.z + 823.0);
		if (d > 6000.0 && d < 20000.0 && (!town.is_valid() || e.id() < town.id()))
		{
			town = e;
			distance = d;
		}
	});
	test::require(town.is_valid(), "a town 6-20 km from the squad");
	const std::uint64_t issued = sim.clockTick();
	const state::GroupAI* current = squad.get<state::GroupAI>();
	state::GroupAI ai = current != nullptr ? *current : state::GroupAI{};
	ai.goal = state::GoalKind::None;
	ai.order.status = state::OrderStatus::Issued;
	ai.order.target = state::EntityRef{town.id()};
	ai.order.originX = -48880.0;
	ai.order.originZ = -823.0;
	ai.order.stay = 100;
	ai.order.deadline = issued + 40 * kTicksPerGameHour;
	ai.order.issued = issued;
	squad.set<state::GroupAI>(ai);
	Run out;
	out.distance = distance;
	const std::uint64_t before = sim.ai()->stats().squadSteps;
	for (std::uint64_t t = 0; t < 30 * kTicksPerGameHour; ++t)
	{
		sim.run(1);
		const state::GroupAI* now = squad.get<state::GroupAI>();
		const state::OrderStatus status = now != nullptr ? now->order.status : state::OrderStatus::None;
		if (status == state::OrderStatus::Staying || status == state::OrderStatus::Retreating || status == state::OrderStatus::Returning)
		{
			out.arrival = sim.clockTick() - issued;
			break;
		}
	}
	(void)id;
	out.steps = sim.ai()->stats().squadSteps - before;
	return out;
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const auto combat = CombatData::build(gd, KENSHI_DATA);
		const Run fast = run(order, combat, 20);
		const Run gated = run(order, combat, 100);
		std::cout << "far raid " << fast.distance << " u: arrival " << fast.arrival << " ticks at 1 Hz (" << fast.steps << " steps), "
			<< gated.arrival << " with the 5 s gate (" << gated.steps << " steps)\n";
		test::require(fast.arrival > 0 && gated.arrival > 0, "the raid arrived in both runs");
		const std::uint64_t gap = fast.arrival > gated.arrival ? fast.arrival - gated.arrival : gated.arrival - fast.arrival;
		test::require(gap <= 100, "the gated arrival is within one 5 s step of the 1 Hz run: " + std::to_string(gap) + " ticks apart");
		test::require(gated.steps * 2 < fast.steps, "the gate steps the squad less often");
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_far_squad_step OK\n";
	return 0;
}
