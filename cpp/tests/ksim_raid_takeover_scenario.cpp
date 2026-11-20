// kenshi-h2v.22.2: scenarios/raid_takeover.kscen itself (Old Village the player's, one player character, a Crab Raiders force 400 u out carrying the
// raid order). Asserted: the force assaults and switches to `hostile occupy town` (FACTIONS.md 2.1b); the player character is downed; the town's owner never changes; and when the test kills every occupier the occupation ends
// within 4 s of play (the leader's death ends it at once; 5 s allowed for the 1 Hz pass). Run twice: Old Village, then Heft (walled; the file moved there as ksim_food_raid_scenario does, kenshi-h2v.22.6): both occupy; the Heft stay is watched 60 s and the throw-out is reported, not asserted (FACTIONS.md 2.1b, TQ2). Needs the world navmesh bake (argv[1], exit 77 without the Old Village region).
#include "ksim/ai/blackboard.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

using namespace ksim;

namespace {

constexpr int kStayTicks = 60 * 20; // how long the occupation is watched for the throw-out
constexpr int kRunTicks = 4 * 60 * 20; // 4 game minutes
constexpr const char* kAssaultSid = "59293-rebirth.mod"; // passing town assault then occupy
constexpr const char* kOccupySid = "96451-rebirth.mod"; // hostile occupy town

} // namespace

// One run of the scenario in `townName` (the file already moved there); false when the bake lacks the town's region.
static bool run(const scenario::File& file, const std::string& world, const char* townName)
{
	if (!scenario_rig::hasRegion(file, world))
		return false;
	std::cout << "-- " << townName << "\n";
	scenario_rig::Rig rig(file, world);
	Simulation& sim = rig.sim;

	sim.newWorld();
	sim.run(1);

	const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
	sim.run(2); // the squads' blackboards are made at fixed points of a tick
	flecs::world& ecs = ecsOf(sim);
	const std::uint64_t human = placed.players.front();
	const auto fighter = [&](std::uint64_t id) { return const_cast<state::Fighter*>(sim.combat()->find(id)); };
	const flecs::entity town(ecs.c_ptr(), sim.importTown(townName, state::Position{file.centreX, file.centreZ}).id);
	test::require(town.is_valid(), std::string(townName) + " is not in the world");
	const flecs::entity_t owner = town.get<state::TownState>()->owner.id;
	test::require(owner != 0, "the scenario did not hand Old Village to the player");
	flecs::entity squad;
	ecs.each([&](flecs::entity member, const state::Id& id)
	{
		if (id.value == placed.others.front())
			squad = member.target<state::MemberOf>();
	});
	// The package the squad applies now ("" once the squad is gone: its board is dropped with it).
	const auto applied = [&]() -> std::string
	{
		const blackboard::Board* board = squad.is_alive() ? sim.blackboardOf(squad) : nullptr;
		return board != nullptr ? board->current : std::string();
	};
	test::require(!applied().empty() || sim.blackboardOf(squad) != nullptr, "the raider squad has no blackboard");

	// 1. The march, the assault, the switch to the occupy package; the Wanderer is held down once the stay began (nothing here wakes her).
	bool assaulted = false;
	bool occupied = false;
	bool downed = false;
	bool brought = false;
	bool threw = false; // 273 / 274 seen: printed only, by design: nobody is thrown out in the original either (FACTIONS.md 2.1b, kenshi-rwh.69)
	int occupiedAt = -1;
	for (int tick = 0; tick < kRunTicks && !(occupied && downed && tick >= occupiedAt * 20 + kStayTicks); ++tick)
	{
		sim.run(1);
		// Before the test's own hold below: only the crabs can have downed her here.
		downed = downed || (fighter(human) != nullptr && (fighter(human)->unconscious || fighter(human)->dead));
		if (state::Fighter* f = fighter(human); f != nullptr && !f->dead && occupied)
			f->unconscious = true; // held down once the stay began: nothing in this test wakes her
		assaulted = assaulted || applied() == kAssaultSid;
		if (applied() == kAssaultSid && !brought)
		{
			// Two raiders are put in reach of the Wanderer as the assault begins. The assault ends once two members are engaged at a 1 Hz sample, and
			// the members' own goal 216 sends each to a random building of the town's circle on a dice keyed (seed, member, tick), so whether two are
			// near her at a sample is a matter of the dice and of the tick of the second the sample falls on (the slices, k16.6.320). This test is
			// about the package's transition, not the dice.
			brought = true;
			for (std::size_t i = 1; i <= 2 && i < placed.others.size(); ++i)
			{
				state::Fighter* raider = fighter(placed.others[i]);
				const state::Fighter* wanderer = fighter(human);
				test::require(raider != nullptr && wanderer != nullptr, "no raider or Wanderer to bring into reach");
				raider->x = wanderer->x + 4.0 * static_cast<double>(i);
				raider->z = wanderer->z;
				raider->y = wanderer->y;
				raider->target = human; // engaged now, not when its perception happens to turn to her
			}
		}
		if (applied() == kOccupySid && !occupied)
		{
			occupied = true;
			occupiedAt = tick / 20;
		}
		ecs.each([&](const state::Id& id, const state::Routine& routine)
		{
			if (occupied && std::find(placed.others.begin(), placed.others.end(), id.value) != placed.others.end()
				&& (routine.goal == routines::task::PickupIntrudersTown || routine.goal == routines::task::TakeIntruderOutsideTown))
				threw = true;
		});
		if (tick % 400 == 0)
		{
			const state::Fighter* lead = fighter(placed.others.front());
			std::cout << "  " << tick / 20 << " s: board " << applied() << ", lead crab " << (lead != nullptr ? std::to_string(lead->x) + ", " + std::to_string(lead->z) : std::string("gone"))
				<< " team " << (lead != nullptr ? lead->team : -9) << " target " << (lead != nullptr ? lead->target : 0) << ", Wanderer " << (fighter(human) == nullptr ? "gone" : fighter(human)->dead ? "dead" : fighter(human)->unconscious ? "down" : "up") << "\n";
		}
	}
	// Where everyone is at the end of the watched stay: the throw-out needs an occupier that senses the body (FACTIONS.md 2.1b).
	const state::Fighter* body = fighter(human);
	std::cout << "the Wanderer lies " << (sim.combat()->environment().inWalls(body->x, body->z) ? "inside" : "outside") << " the walls, carried by " << body->carriedBy << "; occupiers' distance to her:";
	for (const std::uint64_t id : placed.others)
		if (const state::Fighter* f = fighter(id))
			std::cout << " " << static_cast<int>(std::hypot(f->x - body->x, f->z - body->z));
	std::cout << "\n";
	std::cout << "assault " << assaulted << ", occupy package at " << occupiedAt << " s, throw-out goal seen " << threw << "\n";
	test::require(assaulted, "59293 passing town assault never started in the player's town");
	test::require(occupied, "the force never switched to 96451 hostile occupy town (applied: " + applied() + ")");
	test::require(downed, "the Wanderer was never downed");
	test::require(town.get<state::TownState>()->owner.id == owner, "the town's owner changed (rwh.53 4.3: it never does)");

	// 2. Every occupier dies (test-side): the occupation ends within 4 s of play, the town stays the player's.
	test::require(applied() == kOccupySid, "the occupation ended while the occupiers still lived");
	for (const std::uint64_t id : placed.others)
		if (state::Fighter* f = fighter(id))
			f->dead = true;
	int ended = -1;
	for (int tick = 1; tick <= 20 * 5 && ended < 0; ++tick)
	{
		sim.run(1);
		if (applied() != kOccupySid)
			ended = tick;
	}
	std::cout << "the occupation ended " << ended / 20.0 << " s after the last occupier died\n";
	test::require(ended > 0, "the occupation did not end within 5 s of the last occupier dying");
	test::require(town.get<state::TownState>()->owner.id == owner, "the town's owner changed when the occupation ended");
	return true;
}

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/raid_takeover.kscen"));
		if (argc < 2 || !run(file, argv[1], "Old Village"))
		{
			std::cout << "ksim_raid_takeover_scenario: SKIPPED: needs the world navmesh bake with the Old Village region (pass its directory)\n";
			return 77;
		}
		if (!run(scenario_rig::inHeft(file), argv[1], "Heft"))
			std::cout << "ksim_raid_takeover_scenario: the Heft variant is skipped: no bake region\n";
		std::cout << "ksim_raid_takeover_scenario ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_raid_takeover_scenario: FAILED: " << e.what() << "\n";
		return 1;
	}
}
