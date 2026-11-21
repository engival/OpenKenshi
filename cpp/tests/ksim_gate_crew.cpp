// kenshi-k16.6.463 / .474: Heft gate crews at full detail (scenarios/gate_heft.kscen): sergeant outside, 4 guards on the gate nodes, the rest on goals 38 / 157 (rwh.207).
#include "ksim/ai/gatecrew.hpp"
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace ksim;

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/gate_heft.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_gate_crew: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world);
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		flecs::world& ecs = ecsOf(sim);
		struct Crew { flecs::entity squad; state::GateCrew crew; std::size_t members; };
		std::vector<Crew> crews;
		ecs.each([&](flecs::entity squad, const state::GateCrew& crew, const state::Position& pos)
		{
			if (std::hypot(pos.x - file.centreX, pos.z - file.centreZ) < 1200.0)
				crews.push_back({squad, crew, membersOf(sim, squad).size()});
		});
		test::require(!crews.empty(), "no gate crew near Heft's Defensive Gate IV");
		std::set<std::uint64_t> gates;
		for (const Crew& c : crews)
			test::require(gates.insert(c.crew.gate).second, "two crews on one gate");
		sim.run(2400); // two game minutes: the crew has walked to its spots
		for (const Crew& c : crews)
		{
			const GateSite* site = nullptr;
			for (const GateSite& candidate : sim.world().gateSites())
				if (candidate.key == c.crew.gate)
					site = &candidate;
			test::require(site != nullptr, "the crew's gate is unknown");
			const auto members = membersOf(sim, c.squad);
			test::require(members.size() == c.members && members.size() >= 7 && members.size() <= 9, "the crew count changed or is outside the template's sergeant + 6..8 guards");
			std::vector<std::pair<double, double>> at;
			int offDuty = 0;
			int onGoal = 0;
			for (std::size_t index = 0; index < members.size(); ++index)
			{
				const state::Fighter* f = sim.combat()->find(members[index].get<state::Id>()->value);
				test::require(f != nullptr, "a crew member is not a fighter");
				if (const auto stand = gateStandPoint(*site, static_cast<int>(index)))
				{
					const double off = std::hypot(f->x - stand->x, f->z - stand->z);
					std::cout << "gate " << site->key << " member " << index << " " << off << " u from its spot\n";
					test::require(off < (index == 0 ? kGatePostReach : 2.0), "crew member " + std::to_string(index) + " is " + std::to_string(off) + " u from its spot");
				}
				else
				{
					// rwh.207 s1/s3: the members beyond the sergeant and the four posts run the package's town goals: 38 (a building's
					// node) or 157 (a town node), by its 75 / 25 weights; never the gate's own man-the-gate.
					const state::Routine* r = members[index].get<state::Routine>();
					test::require(r != nullptr, "off-duty member " + std::to_string(index) + " has no routine");
					test::require(r->goal != 51, "an off-duty member holds a post");
					std::cout << "  off-duty member " << index << " goal " << r->goal << " at " << f->x << "," << f->z << " "
						<< std::hypot(f->x - site->outX, f->z - site->outZ) << " u from outside\n";
					++offDuty;
					if (r->goal == 38 || r->goal == 157)
						++onGoal;
				}
				for (const auto& [x, z] : at)
					test::require(std::hypot(f->x - x, f->z - z) >= 1.0, "two crew members stand within 1 u");
				at.emplace_back(f->x, f->z);
			}
			std::cout << "  " << members.size() << " members, " << offDuty << " off duty, " << onGoal << " on goal 38/157\n";
			test::require(offDuty >= 2 && onGoal == offDuty, "off-duty members are not all on goal 38 / 157");
		}
		// The off-duty members rotate between 38 and 157 (rwh.207 s3: the goal changes every few game minutes), always one of the two.
		// UNSURE (rwh.207 leaves open): the rotation period, the 75 / 25 split (ours is the scorer's own: about 40% on 38 over 12 min),
		// and the day-138 save (not re-run). Over a window each crew's off-duty members must have shown both goals.
		for (const Crew& c : crews)
		{
			std::set<int> shown;
			for (int step = 0; step < 4; ++step)
			{
				sim.run(1200);
				const auto members = membersOf(sim, c.squad);
				for (std::size_t index = kGateMemberSlots + 1; index < members.size(); ++index)
				{
					const state::Routine* r = members[index].get<state::Routine>();
					test::require(r != nullptr && (r->goal == 38 || r->goal == 157 || r->goal == 0), "an off-duty member runs a goal other than 38 / 157");
					if (r->goal != 0)
						shown.insert(r->goal);
				}
			}
			test::require(shown.size() == 2, "the off-duty members did not rotate between goals 38 and 157");
		}
		std::cout << "ksim_gate_crew ok: " << crews.size() << " crews\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
