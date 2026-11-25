// kenshi-k16.6.820 / kenshi-xu27: a player Lock order (order 60 action 21) on Heft's Defensive Gate IV door, sent through a SIMLINK Session
// on the scenarios/gate_heft_occupant.kscen world, must end with an event 13 (a result, not necessarily done) within 60 s. Dev repro: prints the
// Wanderer's walk state once a second (KSIM_GATE_LOCK_TRACE=1) so a hang shows which layer holds it.
#include "ksim/core/simulation.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/world/interact.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdlib>
#include <iostream>

using namespace ksim;
namespace ks = ksim::simlink;

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/gate_heft_occupant.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_interact_gate_lock: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world);
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		sim.run(40);
		const bool trace = std::getenv("KSIM_GATE_LOCK_TRACE") != nullptr;
		// Defensive Gate IV's door (the client's walk.log: id 1099511627876 at about 67475,-71792): the door entity nearest to that point.
		std::uint64_t door = 0;
		double best = 1e18;
		ecsOf(sim).each([&](const state::Id& id, const state::Door& d)
		{
			const double dist = std::hypot(d.x - 67475.0, d.z + 71792.0);
			if (dist < best) { best = dist; door = id.value; }
		});
		test::require(door != 0 && best < 300.0, "no door near the gate");
		if (trace)
			std::cout << "door " << door << " " << best << " u from the gate point\n";
		std::uint64_t who = 0;
		ecsOf(sim).each([&](const state::Id& id, const state::Fighter& f) { if (f.playerControlled && who == 0) who = id.value; });
		test::require(who != 0, "no player character");
		ks::Session session(sim, rig.context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session.begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", rig.context.dataHash);
		h.str("name", "test");
		h.end();
		test::require(session.onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session.sendTick(w, true);
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter o(frame);
		o.begin(ks::kOrder);
		o.u32("clientSeq", 1);
		o.u16("kind", 60);
		o.u16("n", 1);
		o.u64("id", who);
		o.u64("target", door);
		o.u8("action", 21);
		o.u8("queue", 0);
		o.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		test::require(session.onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply), "the order kicked the client");
		std::cout << "order reply " << out.size() << " bytes: "; for (std::uint8_t c : out) std::cout << (c >= 32 && c < 127 ? char(c) : '.'); std::cout << "\n";
		bool ended = false;
		sim.setInteractObserver([&](const InteractEvent& e)
		{
			if (e.kind == InteractEvent::Kind::Done && e.who == who)
				ended = true;
			if (trace)
				std::cout << "event13 who " << e.who << " result " << int(e.result) << "\n";
		});
		int tick = 0;
		for (; tick < 1200 && !ended; ++tick)
		{
			sim.tick();
			if (trace && tick % 20 == 0)
			{
				const state::Fighter* f = sim.combat()->find(who);
				flecs::entity ent;
				ecsOf(sim).each([&](flecs::entity e, const state::Id& id) { if (id.value == who) ent = e; });
				const state::InteractTask* it = ent.get<state::InteractTask>();
				const state::OrderQueue* oq = ent.get<state::OrderQueue>();
				std::cout << "t " << tick / 20 << " pos " << f->x << "," << f->z << " walking " << f->walking() << " route " << f->route.size()
					<< " next " << f->routeNext << " blocked " << sim.combat()->routesBlocked() << " task " << (it != nullptr ? int(it->action) : -1) << " queue " << (oq != nullptr ? oq->orders.size() : 0) << " doorstep " << (sim.doorStep(who) != nullptr ? int(sim.doorStep(who)->phase) : -1) << "\n";
			}
		}
		test::require(ended, "the Lock order on the gate door never ended in 60 s (routes blocked " + std::to_string(sim.combat()->routesBlocked()) + ")");
		std::cout << "ksim_interact_gate_lock: ok after " << tick << " ticks\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_interact_gate_lock: FAILED: " << e.what() << "\n";
		return 1;
	}
}
