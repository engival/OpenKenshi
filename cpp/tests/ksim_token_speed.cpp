// Off-screen token speeds (kenshi-k16.6.45, COMBAT_OPTIONS.md 7, setting ai.token_speeds): a token walks Hub -> Squin on a straight
// leg (the group layer's own route walk, no Fighters). With the setting off it moves at the flat kWalkSpeed 15 u/s (today's time);
// with it on at 55 u/s, 25 u/s while inside a town's circle (towncircle, the exe's Town radius), capped at 70.
// The expected times are summed from the same circles along the line, so the test checks the rule and the constants, not the layer's
// arithmetic twice. Straight-line navigator: no navmesh, no bake. The world's own hashes with the setting off are the other tests'.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/spawn.hpp"
#include "ksim/world/towncircle.hpp"

#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

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

struct Leg
{
	double seconds = 0.0; // game-seconds the token took
	double length = 0.0;
	std::uint64_t arrivedTick = 0;
};

const state::TownState* townNamed(const Simulation& sim, const std::string& name, flecs::entity* entity = nullptr)
{
	const state::TownState* found = nullptr;
	sim.world().ecs().each([&](flecs::entity e, const state::TownState& town)
	{
		const kdata::Record* record = sim.gameData()->find(town.rootSid);
		if (found == nullptr && record != nullptr && record->name == name)
		{
			found = &town;
			if (entity != nullptr)
				*entity = e;
		}
	});
	return found;
}

// The expected time: the line from `a` to `b` in 1 u steps, each at the token speed of the point it starts on (25 inside any town's
// circle, else 55, capped at 70) or at the flat 15.
double expectedSeconds(const std::vector<towncircle::Circle>& circles, double ax, double az, double bx, double bz, bool tokenSpeeds)
{
	const double length = std::hypot(bx - ax, bz - az);
	if (!tokenSpeeds)
		return length / UtilityGroupLayer::kWalkSpeed;
	double seconds = 0.0;
	double inTown = 0.0;
	for (double d = 0.0; d < length; d += 1.0)
	{
		const double x = ax + (bx - ax) * d / length;
		const double z = az + (bz - az) * d / length;
		const bool inside = towncircle::at(circles, x, z) >= 0;
		inTown += inside ? 1.0 : 0.0;
		seconds += 1.0 / std::min(inside ? UtilityGroupLayer::kTokenTownSpeed : UtilityGroupLayer::kTokenTravelSpeed, UtilityGroupLayer::kTokenSpeedCap);
	}
	std::cout << "  expected: " << length << " u, " << inTown << " u inside a town's circle\n";
	return seconds;
}

Leg run(bool tokenSpeeds)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA);
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	RegionConfig regions;
	regions.workerThreads = 1;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, regions);
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	config.tokenSpeeds = tokenSpeeds;
	sim.enableAi(std::move(config));

	const state::TownState* hub = townNamed(sim, "The Hub");
	const state::TownState* squin = townNamed(sim, "Squin");
	test::require(hub != nullptr && squin != nullptr, "the data has The Hub and Squin");
	const double length = std::hypot(squin->x - hub->x, squin->z - hub->z);
	std::cout << (tokenSpeeds ? "on" : "off") << ": Hub (" << hub->x << ", " << hub->z << ") -> Squin (" << squin->x << ", " << squin->z
		<< "), " << length << " u\n";
	test::require(length > 16500.0 && length < 19000.0, "Hub to Squin is about 17.7 km");

	std::vector<towncircle::Circle> circles;
	sim.world().ecs().each([&](const state::TownState& town)
	{
		const towncircle::Circle circle = towncircle::of(*sim.gameData(), town);
		if (circle.radius > 0.0)
			circles.push_back(circle);
	});

	// A token that is not a trader: the first world squad whose behaviour is not the trade circuit.
	sim.run(40); // the first squad step gives every squad its GroupAI
	flecs::entity token;
	sim.world().ecs().each([&](flecs::entity squad, const state::SquadInstance& instance, const state::GroupAI&)
	{
		if (!token.is_valid() && sim.ai()->registry().squad(instance.templateSid).behaviour != Behaviour::TradeCircuit)
			token = squad;
	});
	test::require(token.is_valid(), "a squad to send");
	token.set<state::Position>(state::Position{hub->x, hub->z});
	const std::uint64_t start = sim.clockTick();
	{
		state::GroupAI& ai = *token.get_mut<state::GroupAI>();
		ai.goal = state::GoalKind::Roam;
		ai.fallback = state::AiReason::None;
		ai.route = {{hub->x, hub->z}, {squin->x, squin->z}};
		ai.length = length;
		ai.along = 0.0;
		ai.goalX = squin->x;
		ai.goalZ = squin->z;
		ai.since = start;
		ai.lastStep = start;
		ai.progressTick = start;
		ai.until = 0;
	}

	Leg leg;
	leg.length = length;
	const std::uint64_t limit = static_cast<std::uint64_t>(length / UtilityGroupLayer::kWalkSpeed / kTickSeconds) + 4000;
	for (std::uint64_t t = 0; t < limit; t += 20)
	{
		sim.run(20);
		const state::GroupAI* ai = token.get<state::GroupAI>();
		const state::Position* at = token.get<state::Position>();
		// Arrived: the leg's goal is over (the layer picks another) or the position is at the end of the line.
		if (ai->goal != state::GoalKind::Roam || ai->route.size() != 2 || std::hypot(at->x - squin->x, at->z - squin->z) < 1.0)
		{
			leg.arrivedTick = sim.clockTick();
			break;
		}
	}
	test::require(leg.arrivedTick != 0, "the token reached Squin within the limit");
	leg.seconds = static_cast<double>(leg.arrivedTick - start) * kTickSeconds;
	const double expected = expectedSeconds(circles, hub->x, hub->z, squin->x, squin->z, tokenSpeeds);
	std::cout << "  took " << leg.seconds << " s (" << leg.seconds / 60.0 << " game-min), expected " << expected << " s\n";
	// One group step is 1 s, plus the 20-tick poll: a few seconds of slack on a leg of minutes.
	test::require(std::fabs(leg.seconds - expected) <= 0.02 * expected + 4.0, "the leg's time matches the speeds' sum");
	return leg;
}

} // namespace

int main()
{
	try
	{
		const Leg off = run(false);
		const Leg on = run(true);
		// 17.7 km at 55 u/s is 5.4 game-minutes; the in-town stretches at 25 u/s add to it, and the cap of 70 is never reached.
		test::require(on.seconds >= on.length / 55.0 - 4.0, "no faster than 55 u/s over the whole leg");
		test::require(on.seconds <= on.length / 25.0, "no slower than 25 u/s over the whole leg");
		test::require(off.seconds >= off.length / UtilityGroupLayer::kWalkSpeed - 4.0, "the setting off is the flat 15 u/s");
		test::require(off.seconds > 2.0 * on.seconds, "the token speeds more than halve the leg");
		std::cout << "ksim_token_speed ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_token_speed: FAILED: " << e.what() << "\n";
		return 1;
	}
}
