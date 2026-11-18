// TASK_PROGRESS 39 and event 20 lock_roll (SIMLINK.md section 6): frame 39 decoded by hand on connect and on change only, a lockpick's
// progress / rate / chance and its reset at each roll (with event 20), the end and an interruption giving n = 0, prospecting, two
// characters at once, the client's extrapolation against the sim, save, load and continue mid-lockpick.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/door_plan.hpp"
#include "ksim/goap.hpp"
#include "ksim/navigator.hpp"
#include "ksim/prospect.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

struct Buf
{
	std::vector<std::uint8_t> bytes;
	template <typename T>
	Buf& put(T v)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
		bytes.insert(bytes.end(), p, p + sizeof(T));
		return *this;
	}
};

struct Frame
{
	std::uint16_t type = 0;
	std::vector<std::uint8_t> payload;
};

std::vector<Frame> frames(const std::vector<std::uint8_t>& bytes)
{
	std::vector<Frame> out;
	for (std::size_t at = 0; at < bytes.size();)
	{
		std::uint32_t len = 0;
		std::memcpy(&len, &bytes[at], 4);
		Frame frame;
		std::memcpy(&frame.type, &bytes[at + 4], 2);
		frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 6), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + len));
		out.push_back(std::move(frame));
		at += 4 + len;
	}
	return out;
}

struct Cursor
{
	const std::vector<std::uint8_t>& p;
	std::size_t at = 0;
	template <typename T>
	T get()
	{
		test::require(at + sizeof(T) <= p.size(), "frame overrun");
		T v;
		std::memcpy(&v, &p[at], sizeof(T));
		at += sizeof(T);
		return v;
	}
};

struct Task
{
	std::uint64_t id = 0;
	std::uint8_t kind = 0;
	float progress = 0, rate = 0, value = 0;
	std::uint8_t flags = 0;
	std::uint64_t target = 0;
};

// TASK_PROGRESS (39): u64 tick, u16 n x {u64 char, u8 kind, f32 progress, f32 rate, f32 value}, then u8 flags[n], u64 target[n].
struct Wire39
{
	std::uint64_t tick = 0;
	std::vector<Task> tasks;
};

struct Roll
{
	std::uint64_t tick = 0, who = 0, door = 0;
	std::uint8_t success = 0;
};

Wire39 decode39(const Frame& frame)
{
	Cursor c{frame.payload};
	Wire39 w;
	w.tick = c.get<std::uint64_t>();
	const std::uint16_t n = c.get<std::uint16_t>();
	for (std::uint16_t i = 0; i < n; ++i)
	{
		Task t;
		t.id = c.get<std::uint64_t>();
		t.kind = c.get<std::uint8_t>();
		t.progress = c.get<float>();
		t.rate = c.get<float>();
		t.value = c.get<float>();
		w.tasks.push_back(t);
	}
	for (Task& t : w.tasks)
		t.flags = c.get<std::uint8_t>();
	for (Task& t : w.tasks)
		t.target = c.get<std::uint64_t>();
	test::require(c.at == frame.payload.size(), "frame 39 has trailing bytes");
	return w;
}

// One session on a simulation, collecting frame 39 and event 20.
struct Watch
{
	Simulation& sim;
	ks::ServerContext& context;
	std::unique_ptr<ks::Session> session;
	std::vector<Wire39> lists;
	std::vector<Roll> rolls;

	Watch(Simulation& s, ks::ServerContext& c)
		: sim(s)
		, context(c)
	{
		connect();
	}

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type == ks::kTaskProgress)
				lists.push_back(decode39(frame));
			if (frame.type == ks::kEvent)
			{
				Cursor c{frame.payload};
				const std::uint64_t tick = c.get<std::uint64_t>();
				if (c.get<std::uint16_t>() == 20)
				{
					Roll r;
					r.tick = tick;
					r.who = c.get<std::uint64_t>();
					r.door = c.get<std::uint64_t>();
					r.success = c.get<std::uint8_t>();
					test::require(c.at == frame.payload.size(), "event 20 has trailing bytes");
					rolls.push_back(r);
				}
			}
		}
	}

	void connect()
	{
		session = std::make_unique<ks::Session>(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash);
		hello.put<std::uint16_t>(4).put<std::uint8_t>('t').put<std::uint8_t>('e').put<std::uint8_t>('s').put<std::uint8_t>('t');
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		collect(bytes);
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
		{
			sim.run(1);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			collect(bytes);
		}
	}
	// Order 98 set_progress_level (u32 seq, u16 kind, u8 level); returns whether the session took it.
	bool setLevel(std::uint8_t level)
	{
		Buf order;
		order.put<std::uint32_t>(++orderSeq).put<std::uint16_t>(98).put<std::uint8_t>(level);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		const bool ok = session->onFrame(ks::kOrder, order.bytes.data(), order.bytes.size(), w);
		collect(bytes);
		return ok;
	}
	std::uint32_t orderSeq = 900;
	void clear()
	{
		lists.clear();
		rolls.clear();
	}
};

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	kworld::LoadOrder order;
	ks::ServerContext context;
	Simulation sim;
	ks::Squad squad;
	std::unique_ptr<Watch> watch;
	std::uint32_t seq = 100;
	std::uint64_t doorId = 0;

	static CombatSettings settings()
	{
		CombatSettings s;
		s.substeps = 2;
		return s;
	}

	Rig(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n)
		: gd(g)
		, data(d)
		, nav(n)
		, order(o)
		, sim([&]
		{
			SpawnOptions options;
			options.seed = 7;
			return Simulation::newGame(KENSHI_DATA, o, options);
		}())
	{
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		sim.enableCombat(data, settings());
		sim.enableNavigation(nav);
		squad = ks::spawnSquad(sim, *gd, *data, context);
		watch = std::make_unique<Watch>(sim, context);
		sim.world().ecs().each([&](const state::Id& i, const state::Door&) { if (doorId == 0) doorId = i.value; });
		test::require(doorId != 0, "the world has a door");
	}

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }

	state::Door* findDoor()
	{
		state::Door* found = nullptr;
		sim.world().ecs().each([&](const state::Id& i, const state::Door& d) { if (i.value == doorId) found = const_cast<state::Door*>(&d); });
		return found;
	}

	// A locked door of the given level; the character stands 3 units from it with Lockpicking 1 (chance 0.9 / 2^((level - 1) / 10)).
	state::Door* lockedDoor(std::uint64_t who, double level)
	{
		state::Door* d = findDoor();
		d->ownerSid = "someone-else";
		d->state = 2;
		d->lockLevel = level;
		d->lockElapsed = 0.0;
		d->lockInterval = kPickFirstInterval;
		d->lockAttempts = 0;
		++d->version;
		state::Fighter& f = fighter(who);
		f.skills.lockpicking = 1.0;
		f.x = d->x + 3.0;
		f.y = d->y;
		f.z = d->z;
		watch->tick(2);
		return d;
	}

	void pick(std::uint64_t who) { sim.enqueue(Command::fighterInteract(state::Id{who}, doorId, 3, ++seq, {}, {}, false)); }
	void stop(std::uint64_t who) { sim.enqueue(Command::fighterStop(state::Id{who}, false)); }
};

bool near(float a, double b, double eps) { return std::fabs(a - b) <= eps; }

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_taskprogress HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- connect with n = 0; nothing is resent while nothing runs ----
		{
			Rig r(order, gd, data, nav);
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.empty() && r.watch->lists[0].tick == r.sim.clockTick(),
				"connect: one frame 39 with n = 0");
			r.watch->clear();
			r.watch->tick(100);
			test::require(r.watch->lists.empty(), "no frame 39 while nothing runs");
			std::cout << "connect ok\n";
		}

		// ---- a lockpick: start, chance, rate, no resend, progress reset and event 20 at each roll, then the end ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t who = r.squad.players[0];
			state::Door* door = r.lockedDoor(who, 40.0); // chance 6% (above the 5% floor under which a pick order is refused): rolls fail
			r.watch->clear();
			r.pick(who);
			r.watch->tick(3);
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.size() == 1, "the start sends one frame with one task");
			const Task start = r.watch->lists[0].tasks[0];
			const std::uint64_t startTick = r.watch->lists[0].tick;
			test::require(start.id == who && start.kind == 1, "kind 1 for the character");
			test::require(near(start.value, pickChance(40.0, 1.0), 1e-6), "value is the roll's chance " + std::to_string(start.value));
			test::require(near(start.rate, 1.0 / kPickFirstInterval, 1e-6) && start.progress >= 0.0f && start.progress < 0.05f, "rate 1/5, progress near 0");
			// no resend while unchanged; the extrapolation matches the sim within one tick mid-attempt
			r.watch->clear();
			r.watch->tick(40);
			test::require(r.watch->lists.empty() && r.watch->rolls.empty(), "no frame while the attempt runs");
			const auto now = r.sim.taskProgress();
			test::require(now.size() == 1, "the sim has the task");
			const double extrapolated = start.progress + start.rate * (r.sim.clockTick() - startTick) * kTickSeconds;
			test::require(std::fabs(extrapolated - now[0].progress) <= start.rate * kTickSeconds + 1e-6,
				"extrapolation " + std::to_string(extrapolated) + " vs sim " + std::to_string(now[0].progress));
			// the first roll: failure, event 20 and a frame with progress 0 and the new interval's rate
			int guard = 0;
			while (r.watch->rolls.empty() && guard++ < 400)
				r.watch->tick();
			test::require(r.watch->rolls.size() == 1 && r.watch->rolls[0].success == 0 && r.watch->rolls[0].who == who
				&& r.watch->rolls[0].door == r.doorId, "event 20 for the failed roll");
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tick == r.watch->rolls[0].tick, "the reset frame comes with the event");
			const Task reset = r.watch->lists[0].tasks.at(0);
			test::require(reset.progress == 0.0f && near(reset.rate, 1.0 / door->lockInterval, 1e-6) && reset.kind == 1, "progress 0 and the new rate after a roll");
			test::require(door->lockInterval >= kPickIntervalMin && door->lockInterval < kPickIntervalMax, "the interval is re-rolled in [5, 20)");
			// a second roll resets again (with an identical interval it would still be a change: progress)
			r.watch->clear();
			guard = 0;
			while (r.watch->rolls.empty() && guard++ < 1000)
				r.watch->tick();
			test::require(r.watch->rolls.size() == 1 && r.watch->lists.size() == 1 && r.watch->lists[0].tasks.at(0).progress == 0.0f, "the second roll resets too");
			// a success: chance 0.9; the lock opens, the task ends with n = 0 (the unlocked door is opened next)
			door->lockLevel = 1.0;
			r.watch->clear();
			guard = 0;
			bool success = false;
			while (!success && guard++ < 20000)
			{
				r.watch->tick();
				for (const Roll& roll : r.watch->rolls)
					success = success || roll.success == 1;
			}
			test::require(success, "a roll succeeded at chance 0.9");
			r.watch->tick(3);
			test::require(!r.watch->lists.empty() && r.watch->lists.back().tasks.empty(), "the end sends n = 0");
			const Roll& last = r.watch->rolls.back();
			test::require(last.success == 1 && r.watch->lists.back().tick >= last.tick, "event 20 success = 1");
			const std::size_t frames39 = r.watch->lists.size();
			r.watch->tick(60);
			test::require(r.watch->lists.size() == frames39, "nothing is resent after the end");
			std::cout << "lockpick ok\n";
		}

		// ---- an interruption (a replacing order) removes the task ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t who = r.squad.players[0];
			r.lockedDoor(who, 40.0);
			r.pick(who);
			r.watch->tick(10);
			r.watch->clear();
			r.stop(who);
			r.watch->tick(2);
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.empty(), "an interrupted lockpick gives n = 0");
			std::cout << "interrupt ok\n";
		}

		// ---- prospecting, alone; then two characters at once ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t a = r.squad.players[0];
			const std::uint64_t b = r.squad.players[1];
			r.watch->clear();
			r.sim.enqueue(Command::prospect(state::Id{b}));
			r.watch->tick(2);
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.size() == 1, "a prospect start sends one frame");
			const Task p = r.watch->lists[0].tasks[0];
			test::require(p.flags == 3 && p.target == 0, "a prospect is the original's bar of the player's character: flags 3");
			test::require(p.id == b && p.kind == 4 && p.value == 0.0f && near(p.rate, 1.0, 1e-6) && p.progress < 0.2f, "kind 4, rate 1, value 0");
			const std::uint64_t t0 = r.watch->lists[0].tick;
			r.watch->clear();
			r.watch->tick(8);
			const auto mid = r.sim.taskProgress();
			test::require(r.watch->lists.empty() && mid.size() == 1, "no resend mid-prospect");
			test::require(std::fabs(p.progress + p.rate * (r.sim.clockTick() - t0) * kTickSeconds - mid[0].progress) <= kTickSeconds + 1e-6, "prospect extrapolation");
			r.watch->tick(30);
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.empty(), "the end of a prospect gives n = 0");
			// two at once
			r.lockedDoor(a, 40.0);
			r.watch->clear();
			r.pick(a);
			r.sim.enqueue(Command::prospect(state::Id{b}));
			r.watch->tick(3);
			const Wire39& both = r.watch->lists.back();
			test::require(both.tasks.size() == 2 && both.tasks[0].id < both.tasks[1].id, "two characters, sorted by id");
			const Task* lock = both.tasks[0].id == a ? &both.tasks[0] : &both.tasks[1];
			const Task* pros = both.tasks[0].id == b ? &both.tasks[0] : &both.tasks[1];
			test::require(lock->kind == 1 && pros->kind == 4, "one lockpick, one prospect");
			r.watch->clear();
			r.watch->tick(30); // the prospect ends; the lockpick is still there
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.size() == 1 && r.watch->lists[0].tasks[0].id == a, "the prospect leaves the list");
			std::cout << "prospect and two characters ok\n";
		}

		// ---- the kinds that read a task's state, their flags and targets (the sim's rows, before the session's level) ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t a = r.squad.players[0];
			const std::uint64_t b = r.squad.players[1];
			const auto entityOf = [&](std::uint64_t id)
			{
				flecs::entity found;
				r.sim.world().ecs().each([&](flecs::entity e, const state::Id& i) { if (i.value == id) found = e; });
				return found;
			};
			const auto rowOf = [&](std::uint64_t id, bool everyone) -> const Task
			{
				for (const Simulation::TaskProgressRow& row : r.sim.taskProgress(everyone))
				{
					if (row.id == id)
						return Task{row.id, row.kind, row.progress, row.rate, row.value, row.flags, row.target};
				}
				return Task{};
			};
			// 2 unlock / 3 lock: a door step's phase, the original's bars
			state::DoorStep step;
			step.door = r.doorId;
			step.phase = 0;
			step.remaining = kUnlockSeconds * 0.4;
			entityOf(a).set<state::DoorStep>(step);
			Task t = rowOf(a, false);
			test::require(t.kind == 2 && t.flags == 3 && t.target == r.doorId && near(t.progress, 0.6, 1e-6) && near(t.rate, 1.0 / kUnlockSeconds, 1e-6),
				"kind 2: progress 1 - remaining / 3.33, rate 1 / 3.33, flags 3, target the door");
			step.phase = 6;
			entityOf(a).set<state::DoorStep>(step);
			test::require(rowOf(a, false).kind == 3, "kind 3 for the lock phase");
			entityOf(a).remove<state::DoorStep>();
			test::require(rowOf(a, false).id == 0, "no task, no row");
			// 8 restrain (the original's) and 14 process (not): a Routine's chain seconds
			state::Routine chain;
			chain.goal = 167;
			chain.plan = {goap::kChainTarget};
			chain.step = 0;
			chain.chainTarget = b;
			chain.chainElapsed = kRestrainSeconds * 0.5;
			entityOf(a).set<state::Routine>(chain);
			t = rowOf(a, false);
			test::require(t.kind == 8 && t.flags == 3 && t.target == b && near(t.progress, 0.5, 1e-6) && near(t.rate, 1.0 / kRestrainSeconds, 1e-6),
				"kind 8: chainElapsed / 9.9, flags 3, target the body");
			chain.goal = 182;
			chain.plan.clear();
			chain.chainElapsed = kProcessSeconds * 0.25;
			entityOf(a).set<state::Routine>(chain);
			t = rowOf(a, false);
			test::require(t.kind == 14 && t.flags == 2 && t.target == b && near(t.progress, 0.25, 1e-6), "kind 14: not the original's (flags 2)");
			entityOf(a).remove<state::Routine>();
			// an NPC's rows come only with `everyone`
			r.fighter(b).playerControlled = false;
			chain.chainElapsed = 1.0;
			entityOf(b).set<state::Routine>(chain);
			test::require(rowOf(b, false).id == 0, "the NPC's task is not in the player-only list");
			t = rowOf(b, true);
			test::require(t.kind == 14 && t.flags == 0, "everyone: the NPC's row has flags 0");
			entityOf(b).remove<state::Routine>();
			r.fighter(b).playerControlled = true;
			std::cout << "kinds ok\n";
		}

		// ---- order 98: the level filters the session's rows; 1 is the default and the original's bars only ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t a = r.squad.players[0];
			const std::uint64_t b = r.squad.players[1];
			const auto entityOf = [&](std::uint64_t id)
			{
				flecs::entity found;
				r.sim.world().ecs().each([&](flecs::entity e, const state::Id& i) { if (i.value == id) found = e; });
				return found;
			};
			r.watch->setLevel(4); // bad_value: refused with an ack, the level stays 1
			state::Routine process; // a player character processing: kind 14, flags 2
			process.goal = 182;
			process.chainTarget = b;
			process.chainElapsed = 0.0;
			r.watch->clear();
			r.sim.enqueue(Command::prospect(state::Id{b})); // kind 4, flags 3: the original's
			entityOf(a).set<state::Routine>(process);
			r.watch->tick(3);
			test::require(r.watch->lists.size() == 1 && r.watch->lists[0].tasks.size() == 1 && r.watch->lists[0].tasks[0].kind == 4,
				"level 1 (the default): only the original's bar");
			test::require(r.watch->setLevel(2), "order 98 level 2 taken");
			r.watch->tick(2);
			const Wire39& two = r.watch->lists.back();
			test::require(two.tasks.size() == 2 && two.tasks[0].id == a && two.tasks[0].kind == 14 && two.tasks[0].flags == 2 && two.tasks[0].target == b
				&& two.tasks[1].kind == 4 && two.tasks[1].flags == 3, "level 2 adds the squad's every task, with the tail");
			test::require(r.watch->setLevel(0), "order 98 level 0 taken");
			r.watch->tick(2);
			test::require(r.watch->lists.back().tasks.empty(), "level 0 sends n = 0");
			r.watch->clear();
			r.watch->tick(20);
			test::require(r.watch->lists.empty(), "level 0: nothing is resent");
			std::cout << "levels ok\n";
		}

		// ---- save, load and continue mid-lockpick ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t who = r.squad.players[0];
			r.lockedDoor(who, 40.0);
			r.pick(who);
			r.watch->tick(160); // past the first roll: the lock holds an elapsed time and a re-rolled interval
			const auto before = r.sim.taskProgress();
			test::require(before.size() == 1 && before[0].progress > 0.0f && r.findDoor()->lockAttempts >= 1, "the save point is mid-attempt");
			std::ostringstream saved;
			r.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, r.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == r.sim.stateHash(), "the hash changed across save and load");
			const auto after = loaded.taskProgress();
			test::require(after.size() == 1 && after[0].id == before[0].id && after[0].progress == before[0].progress && after[0].rate == before[0].rate
				&& after[0].value == before[0].value, "the lock timer is in the save: same progress, rate and chance");
			Watch w(loaded, r.context);
			test::require(w.lists.size() == 1 && w.lists[0].tasks.size() == 1 && w.lists[0].tasks[0].kind == 1
				&& near(w.lists[0].tasks[0].progress, before[0].progress, 1e-6), "the connect frame after a load carries the running lockpick");
			w.clear();
			r.watch->clear();
			r.watch->tick(100);
			w.tick(100);
			test::require(loaded.stateHash() == r.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			test::require(w.lists.size() == r.watch->lists.size() && w.rolls.size() == r.watch->rolls.size(), "the same frames after the load");
			std::cout << "save ok\n";
		}
		std::cout << "ksim_taskprogress ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_taskprogress FAILED: " << e.what() << "\n";
		return 1;
	}
}
