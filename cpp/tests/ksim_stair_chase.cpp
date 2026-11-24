// kenshi-k16.6.104 (playtest kenshi-h2v.3.3 point 3): "Ran upstairs, 2 of them remained locked on to me. They're SLOWLY moving up the stairs." In
// bar_lure_hostile the player runs into the Rebel Base's Bar (a Storm House) and up its stairs to the upper floor while the Blood Raider patrol chases.
// Every raider that is on the player is followed per tick; its speed (xz, u/s) is read on three segments: flat ground outside (at its plateau), the stair
// segment (inside the building, climbing: its y between the floors and past the door) and the upper floor's last approach. The stairs are asserted: the mean
// speed of a chaser on them is within 15% of its run speed. Verdict at the base: the sim runs a chaser up the stairs at its run speed (70 u/s, the same as
// flat); the slow part is the last approach on the upper floor, where they close on the player in the small room (printed, not asserted). argv[1]: the
// world bake (the Bar's floors need the shipped navtiles: KSIM_NATIVE_NAV=1 or the native mesh below; exit 77 without them).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/save/scenario_run.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "knav/native.hpp"
#include "nav_source.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

struct Built
{
	std::unique_ptr<Simulation> sim;
	std::shared_ptr<const CombatData> combat;
};

// The server's default world (ksim_server without --no-world): the new game's towns and residents, combat, navigation, the AI.
Built openWorld(const std::string& bake)
{
	const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
	std::vector<kworld::ZoneCell> zones;
	for (int dz = -1; dz <= 1; ++dz)
		for (int dx = -1; dx <= 1; ++dx)
			zones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
	SpawnOptions options;
	options.seed = 7;
	std::unique_ptr<Simulation> sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
	sim->markPristine();
	std::shared_ptr<const CombatData> combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	sim->enableCombat(combat, settings);
	sim->enableNavigation(MeshNavigator::openNative(KENSHI_DATA, zones)); // the shipped navtiles: the world bake has no building floors
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
	ai.factions = FactionBook::build(sim->gameData());
	sim->enableAi(std::move(ai));
	return {std::move(sim), std::move(combat)};
}

// Loads the scenario on the pristine world (SCENARIO_LOAD: newWorld, then the file).
scenario::Placed load(Built& world, const scenario::File& file)
{
	Simulation& sim = *world.sim;
	simlink::ServerContext context;
	context.gameData = sim.gameData();
	context.squadLimit = 50;
	sim.newWorld();
	sim.run(1);
	scenario::check(*sim.gameData(), file);
	return scenario::apply(sim, *sim.gameData(), *world.combat, context, file, 100);
}

const scenario::ScenarioInfo& infoOf(const std::vector<scenario::ScenarioInfo>& infos, const char* name)
{
	const auto it = std::find_if(infos.begin(), infos.end(), [&](const scenario::ScenarioInfo& info) { return info.name == name; });
	test::require(it != infos.end(), std::string("no scenario ") + name);
	return *it;
}


} // namespace


int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const std::vector<scenario::ScenarioInfo> infos = scenario::listScenarios(KSIM_SCENARIOS_DIR);
		const scenario::File hostileFile = scenario::parseFile(infoOf(infos, "bar_lure_hostile").path);
		const kworld::ZoneCell zone = kworld::zoneCellOf(hostileFile.centreX, hostileFile.centreZ);
		if (bake.empty() || !test::worldRegionReady(bake, zone.x, zone.z))
		{
			std::cout << "ksim_stair_chase: SKIPPED: needs the world navmesh bake\n";
			return 77;
		}
		if (!std::filesystem::is_directory(knav::native::navtilesDir(KENSHI_DATA)))
		{
			std::cout << "ksim_stair_chase: SKIPPED: no shipped navtiles in " << KENSHI_DATA << "\n";
			return 77;
		}
		Built world = openWorld(bake);
		Simulation& sim = *world.sim;
		scenario::Placed placed = load(world, hostileFile);
		const state::Fighter& me = *sim.combat()->find(placed.players.front());
		const MeshNavigator& nav = *sim.navigation();
		const std::uint64_t playerId = placed.players.front();
		std::set<std::uint64_t> raiders;
		for (const CombatArena::Member& member : sim.combat()->members())
			if (member.id > playerId)
				raiders.insert(member.id);
		std::printf("player at %.1f %.1f %.1f, %zu raiders\n", me.x, me.y, me.z, raiders.size());
		const double radius = world.combat->pathRadius(me.raceSid);
		const Route up = nav.routeFrom({me.x, me.y, me.z}, -50592.0, -1211.0, 1145.0, false, radius);
		test::require(up.found && !up.approach, "a route to the Bar's upper floor");
		sim.enqueue(Command::fighterMoveTo(state::Id{playerId}, up.points, up.heights, 1));
		// Per raider and segment: ticks, summed xz speed. flat = outside, y above the street's floor level and no building; stairs = in the building with y
		// between the ground floor (1079) and the upper floor (1145), past the door side of the stairwell; room = the upper floor, still chasing.
		struct Seg
		{
			int ticks = 0;
			double sum = 0.0;
			double mean() const { return ticks > 0 ? sum / ticks : 0.0; }
		};
		struct Chaser
		{
			Seg flat, stairs, room;
			double plateau = 0.0; // the best speed outside
			double stairMin = 1.0e9;
			std::array<double, 2> last{0.0, 0.0};
			bool seen = false;
			int lastStart = -1000; // the last tick it began a new chase walk (the route came up) from a standstill
			bool hadRoute = false;
			int lastStumble = -1000; // the last tick it staggered on the stairs
			bool restarted = false; // it reached the stairs less than kRestartTicks after starting a new walk from a standstill: its speed there is the ramp out of it
		};
		// The run speed of the raiders when the chase starts: a leg wound taken in the brawl on the way lowers it, and the stair speed is measured while they still run at it.
		const double run = sim.combat()->runSpeed(*sim.combat()->find(*raiders.begin()));
		// Seconds-worth of ticks: a chaser at its footwork ramp (k16.8.24) takes about 5 s to get from a standstill to its run, so a stair run that begins
		// within that of a stop measures the ramp, not the stairs (the jam at the stairwell foot, kenshi-k16.7.110 trace).
		constexpr int kRestartTicks = 20 * 5;
		constexpr int kRecoverTicks = 20 * 2; // the ramp out of a stagger
		std::map<std::uint64_t, Chaser> chasers;
		for (int tick = 0; tick < 20 * 14; ++tick)
		{
			sim.run(1);
			for (const std::uint64_t id : raiders)
			{
				const state::Fighter& f = *sim.combat()->find(id);
				Chaser& c = chasers[id];
				const double v = c.seen ? std::hypot(f.x - c.last[0], f.z - c.last[1]) * 20.0 : 0.0;
				c.last = {f.x, f.z};
				c.seen = true;
				if (f.target != playerId || f.dead || f.unconscious)
					continue;
				// A new walk is a route that comes up on a body standing nearly still. A combat stumble or a recoil stop on a live route is not one: the
				// test asks whether the stairs can be climbed, and a raider that is hit, recovers and climbs proves that (kenshi-e9s9.18).
				if (f.routeChase && !c.hadRoute && v < 0.15 * run)
					c.lastStart = tick;
				c.hadRoute = f.routeChase;
				if (f.building == 0)
				{
					++c.flat.ticks;
					c.flat.sum += v;
					c.plateau = std::max(c.plateau, v);
				} else if (f.y > 1086.0 && f.y < 1139.0 && f.z < -1255.0) {
					if (f.state == state::CombatState::Stumble || tick - c.lastStumble < kRecoverTicks)
					{
						c.lastStumble = f.state == state::CombatState::Stumble ? tick : c.lastStumble;
						continue; // a hit's stagger on the stairs holds the body still by design (and the second after it is the ramp out of it): the speed is read on the ticks it runs
					}
					if (c.stairs.ticks == 0 && tick - c.lastStart < kRestartTicks)
						c.restarted = true;
					++c.stairs.ticks;
					c.stairs.sum += v;
					c.stairMin = std::min(c.stairMin, v);
				} else if (f.y >= 1139.0 && f.routeNext < f.route.size()) {
					++c.room.ticks;
					c.room.sum += v;
				}
			}
		}
		std::size_t measured = 0;
		for (const auto& [id, c] : chasers)
		{
			if (c.stairs.ticks < 5 || c.restarted)
			{
				std::printf("raider %llu not measured: %d stair ticks, restarted %d\n", static_cast<unsigned long long>(id), c.stairs.ticks, c.restarted ? 1 : 0);
				continue; // under a quarter second on the stairs: it fell (stumbled, was KO'd) as it came in, which no speed can be read from (k16.8.24: the chase arrives sooner)
			}
			++measured;
			std::printf("raider %llu: flat plateau %.1f (%d ticks), stairs mean %.1f min %.1f (%d ticks), upper-floor approach mean %.1f (%d ticks); run speed %.1f\n",
				static_cast<unsigned long long>(id), c.plateau, c.flat.ticks, c.stairs.mean(), c.stairMin, c.stairs.ticks, c.room.mean(), c.room.ticks, run);
			test::require(std::fabs(c.stairs.mean() - run) <= 0.15 * run, "raider " + std::to_string(id) + " runs the stairs at " + std::to_string(c.stairs.mean())
				+ " u/s against its run speed " + std::to_string(run));
		}
		test::require(measured >= 3, "at least three raiders climbed the stairs behind the player (got " + std::to_string(measured) + ")");
		std::cout << "ksim_stair_chase: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_stair_chase: %s\n", e.what());
		return 1;
	}
}
