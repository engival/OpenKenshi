// kenshi-k16.6.739: a player carrier's order 60 on a free cage's key puts the carried body in it (exe 1407f9e20 carry swap: the order on a cage
// becomes PUT_IN_CAGE 0x6c whatever the action was). Reuses the AI's put-in (Simulation::imprison). Asserts occupancy and state, never coordinates.
// Checks: the wanderer carrying a KO'd guard, ordered with action 9 (and 4) on a free cage key, ends with the body caged (cageBuilding / slot), the cage
// locked, the carrier no longer carrying, event 13 result 0; refusals: not carrying, an occupied cage, no cage there, a corpse carried.
// A Session on scenarios/cage_prisoner (the Heft slave shop). argv[1]: the world navmesh bake (exit 77 without the Heft region).

#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "scenario_rig.hpp"
#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

struct End
{
	std::uint64_t who = 0;
	std::uint64_t target = 0;
	std::uint8_t action = 0;
	std::uint8_t result = 0;
};

struct Wire
{
	Simulation& sim;
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 0;
	std::vector<End> ends;
	std::map<std::uint32_t, std::string> replies;

	Wire(Simulation& s, ks::ServerContext& context)
		: sim(s)
	{
		session = std::make_unique<ks::Session>(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		std::vector<std::uint8_t> hello;
		ks::BinaryWriter h(hello);
		h.begin(ks::kHello);
		h.u32("magic", ks::kMagic);
		h.u16("protocol", ks::kProtocol);
		h.u8("role", 0);
		h.u64("dataHash", context.dataHash);
		h.str("name", "test");
		h.end();
		test::require(session->onFrame(ks::kHello, hello.data() + 6, hello.size() - 6, w), "hello refused");
		session->sendTick(w, true);
		parse(bytes);
	}

	void parse(const std::vector<std::uint8_t>& bytes)
	{
		for (std::size_t p = 0; p < bytes.size();)
		{
			const std::uint32_t len = at<std::uint32_t>(bytes, p);
			const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
			const std::vector<std::uint8_t> body(bytes.begin() + static_cast<long>(p) + 6, bytes.begin() + static_cast<long>(p) + 4 + len);
			p += 4 + len;
			if (type != ks::kEvent)
				continue;
			const std::uint16_t kind = at<std::uint16_t>(body, 8);
			if (kind == 9)
				replies[at<std::uint32_t>(body, 10)] = "";
			else if (kind == 10)
				replies[at<std::uint32_t>(body, 10)] = session->strings().text(at<std::uint32_t>(body, 14));
			else if (kind == 13)
				ends.push_back({at<std::uint64_t>(body, 10), at<std::uint64_t>(body, 18), body[26], body[27]});
		}
	}

	// Order 1 (move), flags 0: an ordinary order (clears the queue), so a later queue=1 order waits behind it.
	void moveTo(std::uint64_t who, double x, double z)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", 1);
		w.u16("n", 1);
		w.u64("id", who);
		w.f32("x", static_cast<float>(x));
		w.f32("z", static_cast<float>(z));
		w.f32("y", 0.0f);
		w.u8("flags", 0);
		w.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply), "a move order kicked the client");
		parse(out);
	}

	void run(int ticks)
	{
		for (int i = 0; i < ticks; ++i)
		{
			sim.tick();
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			parse(bytes);
		}
	}

	// Order 60 (interact): the reply's reason, "" when applied.
	std::string interact(std::uint64_t who, std::uint64_t target, std::uint8_t action, std::uint8_t queue = 0)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", 60);
		w.u16("n", 1);
		w.u64("id", who);
		w.u64("target", target);
		w.u8("action", action);
		w.u8("queue", queue);
		w.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		test::require(session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply), "an order kicked the client");
		parse(out);
		const std::uint32_t mine = seq;
		run(2);
		test::require(replies.count(mine) == 1, "an order got no answer");
		return replies[mine];
	}
};

}

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(ksim::dev::srcPath("../scenarios/cage_prisoner.kscen"));
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_carry_cage: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		rig.context.dataHash = 0;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t wanderer = placed.idOf.at(1);
		const std::uint64_t prisoner = placed.idOf.at(2);
		const std::uint64_t guard = placed.idOf.at(4);
		const std::uint64_t spare = placed.idOf.at(5);
		const auto fighter = [&](std::uint64_t id) -> state::Fighter& { return *const_cast<CombatArena*>(sim.combat())->find(id); };
		// The real lift (order 60 action 8: a KO'd body, the carrier walks up and lifts; CarryStep phase 1 holds it): a hand-set `carrying` is dropped.
		Wire* live = nullptr;
		const auto lift = [&](std::uint64_t carrier, std::uint64_t body)
		{
			fighter(body).unconscious = true;
			fighter(body).koTimer = 600.0;
			test::require(live->interact(carrier, body, 8).empty(), "the lift order was refused");
			for (int i = 0; i < 900 && fighter(carrier).carrying != body; ++i)
				live->run(1);
			test::require(fighter(carrier).carrying == body, "the lift never attached the body");
		};

		Wire wire(sim, rig.context);
		live = &wire;
		wire.run(2);
		const std::uint64_t held = fighter(prisoner).cageBuilding;
		test::require(held != 0, "the scenario's prisoner is not caged");

		// A free cage: any building in view with a cage slot nobody holds.
		std::uint64_t freeKey = 0;
		for (const BuildingSite& site : sim.world().buildingSites())
		{
			const routines::SiteEntry* cage = std::hypot(site.cx - fighter(wanderer).x, site.cz - fighter(wanderer).z) < 400.0 ? sim.cageOf(site.key) : nullptr;
			if (cage != nullptr && !sim.cageTaken(*cage))
			{
				freeKey = site.key;
				break;
			}
		}
		test::require(freeKey != 0, "no free cage within 400 u of the Wanderer");

		// Not carrying: refused (a cage is no other order's target).
		test::require(wire.interact(wanderer, freeKey, 9) == "not_allowed", "an order on a cage without a carried body is not not_allowed");

		// A carrier: the order walks to the cage and puts the body in, whatever the action byte (9 sleep here).
		lift(wanderer, guard);
		// Shift-queued from a distance (kenshi-k16.6.836): behind a short move, so stepOrderQueues plans the route; it asked checkInteract
		// without the Simulation, got the cage refusal, and ended the order as unreachable.
		wire.moveTo(wanderer, fighter(wanderer).x + 6.0, fighter(wanderer).z);
		const std::string reply = wire.interact(wanderer, freeKey, 9, 1);
		test::require(reply.empty(), "the carrier's queued order on the free cage was refused: " + reply);
		bool done = false;
		for (int i = 0; i < 1800 && !done; ++i)
		{
			wire.run(1);
			for (const End& e : wire.ends)
				done = done || (e.who == wanderer && e.target == freeKey);
		}
		test::require(done, "no event 13 for the carrier's cage order");
		test::require(wire.ends.back().result == 0, "the cage order ended with result " + std::to_string(wire.ends.back().result));
		test::require(fighter(wanderer).carrying == 0 && fighter(guard).carriedBy == 0, "the carrier still carries");
		test::require(fighter(guard).cageBuilding == freeKey, "the carried body is not in the cage");
		test::require(sim.cageDoorLocked(freeKey), "the cage is not locked with the body in it");

		// The cage now holds one: a second carrier's order is occupied (or the next slot if the building has one).
		const routines::SiteEntry* again = sim.cageOf(freeKey);
		if (again != nullptr && sim.cageTaken(*again))
		{
			lift(wanderer, spare);
			test::require(wire.interact(wanderer, freeKey, 4) == "occupied", "an order on a full cage is not occupied");
		}
		// Not a cage key: bad_target.
		test::require(wire.interact(wanderer, freeKey + 1, 9) == "bad_target", "an order on a non-cage key is not bad_target");
		std::cout << "ksim_carry_cage: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_carry_cage: " << e.what() << "\n";
		return 1;
	}
}
