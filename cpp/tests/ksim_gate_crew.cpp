// kenshi-k16.6.463 / .474 / .518 (one crew at Gate IV, post hand-over, 75 / 25 weights): Heft gate crews at full detail (scenarios/gate_heft.kscen): sergeant outside, 4 guards on the gate nodes, the rest on goals 38 / 157 (rwh.207).
#include "ksim/ai/gatecrew.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
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
		// rwh.207 s1: Gate IV (the gate nearest the scenario's centre) has ONE squad. Heft's other gates have their own crews (UC_4 at the
		// next gate), which are not asserted here.
		const GateSite* gateIv = nullptr;
		for (const GateSite& candidate : sim.world().gateSites())
			if (std::hypot(candidate.outX - file.centreX, candidate.outZ - file.centreZ) < 1200.0
				&& (gateIv == nullptr || std::hypot(candidate.outX - file.centreX, candidate.outZ - file.centreZ) < std::hypot(gateIv->outX - file.centreX, gateIv->outZ - file.centreZ)))
				gateIv = &candidate;
		test::require(gateIv != nullptr, "no gate near Heft's Defensive Gate IV");
		std::vector<Crew> onGate;
		for (const Crew& c : crews)
			if (c.crew.gate == gateIv->key)
				onGate.push_back(c);
		test::require(onGate.size() == 1, "Gate IV has " + std::to_string(onGate.size()) + " crews, the original has one (UC_19)");
		const Crew crew = onGate.front();
		const GateSite& site = *gateIv;
		// The template's count: sergeant + `squad` min 6 max 8 guards (rwh.207: 8 members this draw, the 7 guards a random draw).
		test::require(crew.members >= 7 && crew.members <= 9, "the crew is outside the template's sergeant + 6..8 guards");
		sim.run(2400); // two game minutes: the crew has walked to its spots
		sim.run(100); // a post handed over this very second has not started its walk yet
		const auto holdersNow = [&]
		{
			std::set<std::uint64_t> holders;
			for (const auto& member : membersOf(sim, crew.squad))
				if (const state::Routine* r = member.get<state::Routine>(); r != nullptr && r->goal == routines::task::GatePost)
					holders.insert(member.get<state::Id>()->value);
			return holders;
		};
		{
			const auto members = membersOf(sim, crew.squad);
			test::require(members.size() == crew.members, "the crew count changed");
			std::vector<std::pair<double, double>> at;
			int posts = 0;
			int atSpot = 0;
			for (std::size_t index = 0; index < members.size(); ++index)
			{
				const state::Fighter* f = sim.combat()->find(members[index].get<state::Id>()->value);
				test::require(f != nullptr, "a crew member is not a fighter");
				const state::Routine* r = members[index].get<state::Routine>();
				if (index == 0)
				{
					const double off = std::hypot(f->x - site.outX, f->z - site.outZ);
					test::require(off < kGatePostReach, "the sergeant is " + std::to_string(off) + " u from the outside point");
				} else {
					test::require(r != nullptr, "a guard has no routine (setup, member " + std::to_string(index) + " dead " + std::to_string(f->dead) + " ko " + std::to_string(f->unconscious) + " down " + std::to_string(f->down) + " target " + std::to_string(f->target) + " carriedBy " + std::to_string(f->carriedBy) + " player " + std::to_string(f->playerControlled) + ")");
					if (r->goal == routines::task::GatePost)
					{
						// The post's slot rides in the routine's target (postSlots).
						const auto stand = gateStandPoint(site, r->target.slot);
						test::require(stand.has_value(), "a post holder has no slot");
						const double off = std::hypot(f->x - stand->x, f->z - stand->z);
						std::cout << "member " << index << " post " << r->target.slot << " " << off << " u from its spot\n";
						// A guard that took a post over at a rotation may still be walking back from a town node.
						test::require(off < 2.0 || f->walking(), "post holder " + std::to_string(index) + " is " + std::to_string(off) + " u from its spot and not walking");
						if (off < 2.0)
							++atSpot;
						++posts;
					} else {
						// rwh.207 s1/s3: the others run the package's town goals 38 / 157; never the gate's own man-the-gate.
						test::require(r->goal == 38 || r->goal == 157 || r->goal == 0, "an off-duty member runs goal " + std::to_string(r->goal));
					}
				}
				for (const auto& [x, z] : at)
					test::require(std::hypot(f->x - x, f->z - z) >= 1.0, "two crew members stand within 1 u");
				at.emplace_back(f->x, f->z);
			}
			test::require(posts == kGateMemberSlots, "the gate's four posts are not all held");
			test::require(atSpot >= 2, "fewer than two post holders stand at their spots");
		}
		// Hand-over (rwh.207 s3): over 4.8k ticks the holders of the four posts change. UNSURE: the rotation period (ours kGateRotationTicks).
		// Goal shares (rwh.207 s1: package `Static guard- building` 75 / `Static guard- town` 25): a guard at a post is on 38, so a sample
		// counts post holders and goal-38 guards against goal-157 guards. Only 4 posts exist, so with 6..8 guards the 38 share sits under
		// 75 %; it must still be the larger one and within a tolerance of the weights.
		const std::set<std::uint64_t> first = holdersNow();
		bool changed = false;
		int on38 = 0;
		int on157 = 0;
		for (int step = 0; step < 16; ++step)
		{
			sim.run(600);
			if (step < 8 && holdersNow() != first)
				changed = true;
			const auto members = membersOf(sim, crew.squad);
			for (std::size_t index = 1; index < members.size(); ++index)
			{
				const state::Routine* r = members[index].get<state::Routine>();
				test::require(r != nullptr, "a guard has no routine (step " + std::to_string(step) + ", member " + std::to_string(index) + ")");
				if (r->goal == routines::task::GatePost || r->goal == 38)
					++on38;
				else if (r->goal == 157)
					++on157;
			}
		}
		test::require(changed, "the post holders did not change over 4.8k ticks");
		const double share = static_cast<double>(on38) / std::max(1, on38 + on157);
		std::cout << "goal 38 share " << share << " (" << on38 << " / " << on157 << ")\n";
		test::require(share > 0.5 && share < 0.9, "the goal 38 share " + std::to_string(share) + " is far from the package's 75 / 25");
		std::cout << "ksim_gate_crew ok: " << onGate.size() << " crew at Gate IV (" << crews.size() << " near Heft)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
