// kenshi-k16.6.820 / kenshi-xu27: a player Lock order (order 60 action 21) on Heft's Defensive Gate IV door, sent through a SIMLINK Session
// on the scenarios/gate_heft_occupant.kscen world, must end with an event 13 (a result, not necessarily done) within 60 s. Dev repro: prints the
// Wanderer's walk state once a second (KSIM_GATE_LOCK_TRACE=1) so a hang shows which layer holds it.
// The test replays the field orders (set_progress_level, set_interest, set_view, camera follow) before the Lock; KSIM_GATE_LOCK_DOOR picks the door id, KSIM_GATE_LOCK_NEAREST the nearest door.
#include "ksim/core/simulation.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/world/interact.hpp"

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdlib>
#include <functional>
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
		sim.setRouteTrace(trace);
		sim.setTaskLog([](const std::string& l) { std::cout << "task: " << l << "\n"; });
		// Defensive Gate IV's door (the client's walk.log: id 1099511627876 at about 67475,-71792): the door entity nearest to that point.
		std::uint64_t door = 0;
		double best = 1e18;
		ecsOf(sim).each([&](const state::Id& id, const state::Door& d)
		{
			const double dist = std::hypot(d.x - 67475.0, d.z + 71792.0);
			if (dist < best) { best = dist; door = id.value; }
		});
		test::require(door != 0 && best < 300.0, "no door near the gate");
		// The field door (xu27): the Heft building door 1099511627876, when this world has it (the nearest door above is Gate IV's).
		const char* want = std::getenv("KSIM_GATE_LOCK_DOOR");
		const std::uint64_t fieldDoor = want != nullptr ? std::strtoull(want, nullptr, 10) : 1099511627876ull;
		bool fieldFound = false;
		ecsOf(sim).each([&](const state::Id& id, const state::Door& d)
		{
			if (id.value == fieldDoor)
			{
				fieldFound = true;
				std::cout << "field door " << id.value << " at " << d.x << "," << d.z << " state " << int(d.state) << "\n";
			}
		});
		if (trace)
			ecsOf(sim).each([&](const state::Id& id, const state::Door& d)
			{
				if (std::hypot(d.x - 67475.0, d.z + 71792.0) < 1500.0)
					std::cout << "door " << id.value << " at " << d.x << "," << d.z << " state " << int(d.state) << "\n";
			});
		const bool useField = std::getenv("KSIM_GATE_LOCK_NEAREST") == nullptr && fieldFound;
		if (useField)
			door = fieldDoor;
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
		// The client's orders before the Lock (the field orders.log): set_progress_level 1 (98), set_interest (50), set_view (51), camera follow 0 then the Wanderer (52).
		std::uint32_t seq = 1;
		const auto control = [&](std::uint16_t kind, const std::function<void(ks::BinaryWriter&)>& body)
		{
			std::vector<std::uint8_t> fr;
			ks::BinaryWriter fw(fr);
			fw.begin(ks::kOrder);
			fw.u32("clientSeq", seq++);
			fw.u16("kind", kind);
			body(fw);
			fw.end();
			std::vector<std::uint8_t> ro;
			ks::BinaryWriter rw(ro);
			test::require(session.onFrame(ks::kOrder, fr.data() + 6, fr.size() - 6, rw), "a control order kicked the client");
		};
		control(98, [](ks::BinaryWriter& f) { f.u8("level", 1); });
		control(50, [](ks::BinaryWriter& f) { f.f32("x", 67111.53125f); f.f32("z", -71758.578125f); f.f32("radius", 4000.0f); });
		control(51, [](ks::BinaryWriter& f) { f.f32("x", 67111.53125f); f.f32("z", -71758.578125f); });
		control(52, [](ks::BinaryWriter& f) { f.u64("id", 0); });
		for (int i = 0; i < 31; ++i) // the field's 31 ticks between the follow orders
			sim.tick();
		control(52, [who](ks::BinaryWriter& f) { f.u64("id", who); });
		for (int i = 0; i < 152; ++i) // and the 152 ticks before the interact
			sim.tick();
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter o(frame);
		o.begin(ks::kOrder);
		o.u32("clientSeq", seq++);
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
		// The hardened stall stop (kenshi-k16.6.824): the same walk again, but the body is put back 3 u about its start every tick, so it never
		// stands still by the 2 u rule and never gains ground on the route. It must end with event 13 kUnreachable after 15 s (300 ticks), not hang.
		{
			state::Fighter* body = fighterOf(sim, who);
			ended = false;
			std::uint8_t result = 255;
			sim.setInteractObserver([&](const InteractEvent& e)
			{
				if (e.kind == InteractEvent::Kind::Done && e.who == who)
				{
					ended = true;
					result = e.result;
				}
			});
			// Lock again on the far door the first walk reached, from the start: the door is locked now, so unlock it first with the rules' own state.
			ecsOf(sim).each([&](const state::Id& id, state::Door& d) { if (id.value == door) d.state = 1; });
			body->x = 67113.33;
			body->z = -72102.62;
			std::vector<std::uint8_t> again;
			ks::BinaryWriter ag(again);
			ag.begin(ks::kOrder);
			ag.u32("clientSeq", seq++);
			ag.u16("kind", 60);
			ag.u16("n", 1);
			ag.u64("id", who);
			ag.u64("target", door);
			ag.u8("action", 21);
			ag.u8("queue", 0);
			ag.end();
			std::vector<std::uint8_t> reply2;
			ks::BinaryWriter rw2(reply2);
			test::require(session.onFrame(ks::kOrder, again.data() + 6, again.size() - 6, rw2), "the second order kicked the client");
			int osc = 0;
			for (; osc < 700 && !ended; ++osc)
			{
				state::Fighter* w = fighterOf(sim, who);
				if (w->walking())
				{
					w->x = 67113.33 + ((osc / 10) % 2 != 0 ? 3.0 : 0.0);
					w->z = -72102.62;
					w->forwardSpeed = 0.0;
				}
				sim.tick();
			}
			test::require(ended && result == InteractEvent::kUnreachable, "an oscillating walker (3 u about its start) did not end with event 13 unreachable (ended " + std::to_string(ended)
				+ " result " + std::to_string(int(result)) + " after " + std::to_string(osc) + " ticks)");
			test::require(osc >= 300, "the oscillating walk ended before the 15 s stall limit (" + std::to_string(osc) + " ticks): the stall stop was not what ended it");
			std::cout << "oscillating walker -> event 13 unreachable after " << osc << " ticks\n";
		}
		// A walker that really travels is not stalled (kenshi-k16.6.832): the same walk again, the body pushed 0.3 u per tick AWAY from its next
		// route point (the distance left grows, so only travel from the stall baseline shows progress). 400 ticks, past the 300 limit, must not
		// end it by the stall stop.
		{
			bool done = false;
			std::uint8_t res = 255;
			sim.setInteractObserver([&](const InteractEvent& e)
			{
				if (e.kind == InteractEvent::Kind::Done && e.who == who)
				{
					done = true;
					res = e.result;
				}
			});
			ecsOf(sim).each([&](const state::Id& id, state::Door& d) { if (id.value == door) d.state = 1; });
			fighterOf(sim, who)->x = 67113.33;
			fighterOf(sim, who)->z = -72102.62;
			std::vector<std::uint8_t> third;
			ks::BinaryWriter tw(third);
			tw.begin(ks::kOrder);
			tw.u32("clientSeq", seq++);
			tw.u16("kind", 60);
			tw.u16("n", 1);
			tw.u64("id", who);
			tw.u64("target", door);
			tw.u8("action", 21);
			tw.u8("queue", 0);
			tw.end();
			std::vector<std::uint8_t> reply3;
			ks::BinaryWriter rw3(reply3);
			test::require(session.onFrame(ks::kOrder, third.data() + 6, third.size() - 6, rw3), "the third order kicked the client");
			double px = 0.0;
			double pz = 0.0;
			bool have = false;
			int drift = 0;
			for (; drift < 400 && !done; ++drift)
			{
				state::Fighter* w = fighterOf(sim, who);
				if (w->walking())
				{
					if (!have) { px = w->x; pz = w->z; have = true; }
					const state::RoutePoint& next = w->route[w->routeNext];
					const double dx = px - next.x;
					const double dz = pz - next.z;
					const double len = std::max(1.0, std::hypot(dx, dz));
					px += 0.3 * dx / len;
					pz += 0.3 * dz / len;
					w->x = px;
					w->z = pz;
					w->forwardSpeed = 0.0;
				}
				sim.tick();
			}
			test::require(!(done && res == InteractEvent::kUnreachable), "a walker travelling away from its next point was ended by the stall stop after " + std::to_string(drift) + " ticks");
			std::cout << "travelling walker not stalled (" << drift << " ticks, ended " << done << ")\n";
		}
		std::cout << "ksim_interact_gate_lock: ok after " << tick << " ticks\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_interact_gate_lock: FAILED: " << e.what() << "\n";
		return 1;
	}
}
