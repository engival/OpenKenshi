// Jobs V1 (ARCHITECTURE "V1: the original's per-character jobs", SIMLINK.md JOBS 34, orders 76-78, toggle 10 which 4, jobs17_static.md):
// the exe's addTask rules and drag rule as pure functions, every answer of the three orders and the toggle through an in-process
// Session, frame 34 decoded by hand (client/src/simlink.* has no type 34 decoder yet, so ksim_simlink's loopback has nothing to
// compare with; this test reads the wire layout of SIMLINK.md itself), the executors acting (Medic, Attacking enemies, Staying close,
// Bodyguard), the priority fall-through and a player's order outranking the jobs, and save, load and continue giving the same hash.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/jobs.hpp"
#include "ksim/navigator.hpp"
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

// ---- wire helpers ----

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
	Buf& str(const std::string& s)
	{
		put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
		bytes.insert(bytes.end(), s.begin(), s.end());
		return *this;
	}
	Buf& ids(const std::vector<std::uint64_t>& list)
	{
		put<std::uint16_t>(static_cast<std::uint16_t>(list.size()));
		for (const std::uint64_t id : list)
			put(id);
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

// JOBS (type 34) decoded from the layout in SIMLINK.md.
struct Wire34
{
	struct Row
	{
		std::uint32_t jobId = 0;
		std::uint16_t type = 0;
		std::uint64_t target = 0;
		std::uint8_t active = 0;
	};
	struct Character
	{
		std::uint64_t id = 0;
		std::uint8_t enabled = 0;
		std::vector<Row> rows;
	};
	std::uint64_t tick = 0;
	std::vector<Character> characters;
};

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	kworld::LoadOrder order;
	ks::ServerContext context;
	Simulation sim;
	ks::Squad squad; // players[0], players[1]; enemies
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 0;
	std::vector<Wire34> jobFrames; // every frame 34 since the last clear

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
		connect();
	}

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type != ks::kJobs)
				continue;
			Cursor c{frame.payload};
			Wire34 wire;
			wire.tick = c.get<std::uint64_t>();
			const std::uint16_t n = c.get<std::uint16_t>();
			for (std::uint16_t i = 0; i < n; ++i)
			{
				Wire34::Character ch;
				ch.id = c.get<std::uint64_t>();
				ch.enabled = c.get<std::uint8_t>();
				const std::uint16_t rows = c.get<std::uint16_t>();
				for (std::uint16_t k = 0; k < rows; ++k)
				{
					Wire34::Row row;
					row.jobId = c.get<std::uint32_t>();
					row.type = c.get<std::uint16_t>();
					row.target = c.get<std::uint64_t>();
					row.active = c.get<std::uint8_t>();
					ch.rows.push_back(row);
				}
				wire.characters.push_back(std::move(ch));
			}
			test::require(c.at == frame.payload.size(), "frame 34 has trailing bytes");
			jobFrames.push_back(std::move(wire));
		}
	}

	void connect()
	{
		session = std::make_unique<ks::Session>(sim, context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		collect(bytes);
	}

	// Sends the order; returns "" when it was applied, else the refusal reason. The sim runs one tick and the session sends it.
	std::string order_(std::uint16_t kind, const Buf& body)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order closed the session");
		std::string reason;
		bool answered = false;
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type != ks::kEvent)
				continue;
			Cursor c{frame.payload};
			c.get<std::uint64_t>();
			const std::uint16_t kindOf = c.get<std::uint16_t>();
			if (kindOf == 9 || kindOf == 10)
			{
				test::require(c.get<std::uint32_t>() == seq, "the answer is for another order");
				answered = true;
				if (kindOf == 10)
					reason = session->strings().text(c.get<std::uint32_t>());
			}
		}
		test::require(answered, "an order got no answer");
		tick();
		return reason;
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

	std::string add(const std::vector<std::uint64_t>& ids, std::uint16_t type, std::uint64_t target = 0)
	{
		return order_(76, Buf{}.ids(ids).put(type).put(target));
	}
	std::string remove(std::uint64_t id, std::uint32_t jobId) { return order_(77, Buf{}.put(id).put(jobId)); }
	std::string move(std::uint64_t id, std::uint32_t jobId, std::uint16_t index) { return order_(78, Buf{}.put(id).put(jobId).put(index)); }
	std::string toggleJobs(const std::vector<std::uint64_t>& ids, std::uint8_t value) { return order_(10, Buf{}.ids(ids).put<std::uint8_t>(4).put(value)); }

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }

	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { if (i.value == id) found = e; });
		return found;
	}

	void giveKit(std::uint64_t id)
	{
		std::string kit;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ITEM")))
		{
			if (r->name == "Basic First Aid Kit")
				kit = r->stringID;
		}
		test::require(!kit.empty(), "no Basic First Aid Kit in the data");
		flecs::entity e = entityOf(id);
		state::Inventory inv = e.get<state::Inventory>() != nullptr ? *e.get<state::Inventory>() : state::Inventory{};
		inv.add(kit, 1);
		e.set<state::Inventory>(inv);
	}

	void wound(std::uint64_t id)
	{
		for (state::FighterPart& part : fighter(id).parts)
			part.health = part.maxHealth * 0.5;
	}

	double healthOf(std::uint64_t id)
	{
		double sum = 0.0;
		for (const state::FighterPart& part : fighter(id).parts)
			sum += part.health + part.pool;
		return sum;
	}

	// Puts a fighter on the mesh `distance` units from `near`, in front of it (along its yaw) when `ahead`, facing away from it.
	void place(std::uint64_t id, std::uint64_t near, double distance, double yaw)
	{
		const state::Fighter& anchor = *sim.combat()->find(near);
		const auto spot = nav->walker().snap({anchor.x + std::sin(yaw) * distance, anchor.y, anchor.z + std::cos(yaw) * distance}, 100.0);
		test::require(spot.has_value(), "no mesh at the spot");
		state::Fighter& f = fighter(id);
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		f.yaw = yaw;
	}

	JobsView jobs(std::uint64_t id) const { return sim.jobsOf(id); }
	std::vector<std::uint16_t> types(std::uint64_t id) const
	{
		std::vector<std::uint16_t> out;
		for (const state::JobRow& row : sim.jobsOf(id).rows)
			out.push_back(row.type);
		return out;
	}
};

using Ids = std::vector<std::uint64_t>;
using Types = std::vector<std::uint16_t>;

void requireReason(const std::string& got, const std::string& want, const std::string& what)
{
	test::require(got == want, what + ": answered '" + got + "', wanted '" + want + "'");
}

// ---- the pure rules ----

std::vector<std::uint32_t> ids(const state::Jobs& jobs)
{
	std::vector<std::uint32_t> out;
	for (const state::JobRow& row : jobs.rows)
		out.push_back(row.jobId);
	return out;
}

std::vector<std::uint16_t> typesOf(const state::Jobs& jobs)
{
	std::vector<std::uint16_t> out;
	for (const state::JobRow& row : jobs.rows)
		out.push_back(row.type);
	return out;
}

void rules()
{
	// kind 0 or unknown types never become rows
	test::require(jobType(153) == nullptr && jobType(1) == nullptr && jobType(0) == nullptr, "a kind 0 type has an Appendix A entry");
	test::require(jobType(58) != nullptr && jobType(58)->executor && jobType(87) != nullptr && !jobType(87)->executor && jobType(250)->executor, "the table's executor flags");

	state::Jobs jobs;
	// the replacement type is applied: First aid becomes Medic, Attacking target becomes Attacking enemies
	test::require(addJob(jobs, 25, 0) == 1 && typesOf(jobs) == Types{58}, "First aid is stored as Medic");
	test::require(addJob(jobs, 5, 77) == 1 && typesOf(jobs) == (Types{58, 16}), "Attacking target is stored as Attacking enemies, its target dropped");
	test::require(jobs.rows[1].target == 0, "a type without a target flag stores target 0");
	// duplicates: by type when the target flag is 0 (any target), by type and target when it is 1
	test::require(addJob(jobs, 58, 0) == 0 && addJob(jobs, 25, 0) == 0 && addJob(jobs, 4, 12) == 0, "a duplicate (also through a replacement) is dropped");
	test::require(typesOf(jobs) == (Types{58, 16}), "dropped duplicates changed the list");
	// a Sleep order has a kind 0 replacement: no row
	test::require(addJob(jobs, 98, 0) == 0 && addJob(jobs, 261, 0) == 0, "Sleeping / Hiding became a row");
	// companions follow directly: Bodyguard adds Staying close
	test::require(addJob(jobs, 45, 100) == 2 && typesOf(jobs) == (Types{58, 16, 45, 31}), "Bodyguard adds its companion directly after");
	test::require(jobs.rows[2].target == 100 && jobs.rows[3].target == 100, "the companion has the main row's target");
	test::require(addJob(jobs, 45, 100) == 0, "the same Bodyguard again is a duplicate");
	test::require(addJob(jobs, 45, 200) == 2 && typesOf(jobs) == (Types{58, 16, 45, 31, 45, 31}), "a Bodyguard of another target is a new pair");
	test::require(addJob(jobs, 31, 100) == 0 && addJob(jobs, 31, 200) == 0, "Staying close of a target already held is a duplicate");
	test::require(addJob(jobs, 31, 300) == 1 && jobs.rows.back().type == 31, "Staying close of a new target is a row");
	// a duplicate main row still gets a missing companion, at the end
	state::Jobs lone;
	lone.rows.push_back({lone.nextJobId++, 45, 5});
	test::require(addJob(lone, 45, 5) == 1 && typesOf(lone) == (Types{45, 31}), "a duplicate Bodyguard still adds its missing companion");
	// jobIds are never reused
	std::vector<std::uint32_t> seen = ids(jobs);
	for (std::size_t i = 1; i < seen.size(); ++i)
		test::require(seen[i] > seen[i - 1], "jobIds not ascending");
	const std::uint32_t last = jobs.rows.back().jobId;
	test::require(removeJob(jobs, last), "remove failed");
	addJob(jobs, 31, 301);
	test::require(jobs.rows.back().jobId > last, "a removed jobId was reused");
	// adding sets enabled on, a dropped duplicate does not
	jobs.enabled = false;
	test::require(addJob(jobs, 58, 0) == 0 && !jobs.enabled, "a duplicate turned JOBS on");
	test::require(addJob(jobs, 16, 0) == 0 && !jobs.enabled, "a duplicate turned JOBS on");
	test::require(addJob(jobs, 31, 999) == 1 && jobs.enabled, "adding a row did not turn JOBS on");

	// the drag rule: the row is taken out, then inserted at index of the shortened list
	state::Jobs abcd;
	for (std::uint32_t i = 1; i <= 4; ++i)
		abcd.rows.push_back({i, static_cast<std::uint16_t>(i), 0});
	test::require(moveJob(abcd, 1, 2) && ids(abcd) == (std::vector<std::uint32_t>{2, 3, 1, 4}), "A onto C in ABCD is BCAD");
	test::require(moveJob(abcd, 2, 0) && ids(abcd) == (std::vector<std::uint32_t>{2, 3, 1, 4}), "B onto itself (index 0) is unchanged");
	test::require(moveJob(abcd, 4, 0) && ids(abcd) == (std::vector<std::uint32_t>{4, 2, 3, 1}), "D onto the first row");
	test::require(moveJob(abcd, 4, state::Jobs{}.rows.size() + 0xFFFF) && ids(abcd) == (std::vector<std::uint32_t>{2, 3, 1, 4}), "0xFFFF goes last");
	test::require(moveJob(abcd, 2, 99) && ids(abcd) == (std::vector<std::uint32_t>{3, 1, 4, 2}), "an index past the end is last");
	test::require(!moveJob(abcd, 77, 0) && ids(abcd) == (std::vector<std::uint32_t>{3, 1, 4, 2}), "a stale jobId moved something");
	test::require(removeJob(abcd, 1) && ids(abcd) == (std::vector<std::uint32_t>{3, 4, 2}) && !removeJob(abcd, 1), "remove by jobId, twice");

	// with JOBS off only the six (and the second Staying close) still run
	for (std::uint16_t t = 0; t < 400; ++t)
	{
		const bool six = t == 16 || t == 31 || t == 45 || t == 57 || t == 58 || t == 250 || t == 289;
		test::require(jobRunsWhenOff(t) == six, "jobRunsWhenOff(" + std::to_string(t) + ")");
	}
	std::cout << "rules ok\n";
}

Wire34::Character wireOf(const Rig& r, std::uint64_t id)
{
	for (auto frame = r.jobFrames.rbegin(); frame != r.jobFrames.rend(); ++frame)
	{
		for (const Wire34::Character& c : frame->characters)
		{
			if (c.id == id)
				return c;
		}
	}
	throw test::Failure("no frame 34 holds character " + std::to_string(id));
}

// What the last frame 34 says of the character is what the sim has.
void requireWire(const Rig& r, std::uint64_t id, const std::string& where)
{
	const Wire34::Character c = wireOf(r, id);
	const JobsView v = r.jobs(id);
	test::require((c.enabled != 0) == v.enabled && c.rows.size() == v.rows.size(), where + ": header or row count differs");
	for (std::size_t i = 0; i < v.rows.size(); ++i)
	{
		test::require(c.rows[i].jobId == v.rows[i].jobId && c.rows[i].type == v.rows[i].type && c.rows[i].target == v.rows[i].target,
			where + ": row " + std::to_string(i) + " differs");
		test::require((c.rows[i].active != 0) == (v.enabled || jobRunsWhenOff(v.rows[i].type)), where + ": active flag of row " + std::to_string(i));
	}
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_jobs HUB.knav");
		rules();
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- the orders, their answers and frame 34 ----
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t p1 = r.squad.players[0];
			const std::uint64_t p2 = r.squad.players[1];
			const std::uint64_t enemy = r.squad.enemies[0];

			// the connect snapshot: every player character, empty lists included
			test::require(r.jobFrames.size() == 1 && r.jobFrames[0].characters.size() == 2, "the connect snapshot is one frame of both player characters");
			for (const Wire34::Character& c : r.jobFrames[0].characters)
				test::require(c.enabled == 1 && c.rows.empty() && (c.id == p1 || c.id == p2), "a snapshot entry is not an enabled empty list");
			r.jobFrames.clear();
			r.tick(3);
			test::require(r.jobFrames.empty(), "an unchanged tick sent a frame 34");

			// 76 refusals: nothing changes
			requireReason(r.add({p1}, 0), "bad_value", "type 0");
			requireReason(r.add({p1}, 999), "bad_value", "a type with no TaskData");
			requireReason(r.add({p1}, 153), "unsupported", "a kind 0 type (Going to bed)");
			requireReason(r.add({p1}, 98), "unsupported", "Sleeping (replaced by a kind 0 type)");
			requireReason(r.add({p1}, 87, p2), "unsupported", "Operating machine: no executor yet");
			requireReason(r.add({p1}, 152, p2), "unsupported", "Operating automatic machine: no executor yet");
			requireReason(r.add({p1}, 124, 0), "unsupported", "Hauling to: no executor yet");
			requireReason(r.add({p1}, 45, 0), "bad_target", "Bodyguard with no target");
			requireReason(r.add({p1}, 45, p1), "bad_target", "Bodyguard of itself");
			requireReason(r.add({p1}, 45, 987654321), "bad_target", "Bodyguard of an unknown id");
			requireReason(r.add({p1}, 31, 0), "bad_target", "Staying close with no target");
			requireReason(r.add({}, 58), "no_characters", "no ids");
			requireReason(r.add({987654321}, 58), "unknown_id", "an unknown id");
			requireReason(r.add({enemy}, 58), "not_player_controlled", "an enemy");
			requireReason(r.add({p1, enemy}, 58), "not_player_controlled", "one bad id refuses the whole order");
			test::require(r.types(p1).empty() && r.types(p2).empty() && r.jobFrames.empty(), "a refused order changed a list or sent a frame");

			// 76 fans out; the replacement and the dedup; a frame 34 with only the changed characters
			requireReason(r.add({p1, p2}, 25), "", "First aid for both");
			test::require(r.types(p1) == Types{58} && r.types(p2) == Types{58}, "First aid is stored as Medic for both");
			test::require(!r.jobFrames.empty() && r.jobFrames.back().characters.size() == 2, "the fan-out sent one frame of both characters");
			requireWire(r, p1, "after the fan-out");
			requireWire(r, p2, "after the fan-out");
			r.jobFrames.clear();
			requireReason(r.add({p1}, 58), "", "a duplicate is applied, no change");
			test::require(r.jobFrames.empty() && r.types(p1) == Types{58}, "a duplicate changed the list or sent a frame");
			requireReason(r.add({p1}, 45, p2), "", "Bodyguard of p2");
			test::require(r.types(p1) == (Types{58, 45, 31}) && r.types(p2) == Types{58}, "Bodyguard and its Staying close follow directly");
			test::require(r.jobFrames.size() == 1 && r.jobFrames[0].characters.size() == 1 && r.jobFrames[0].characters[0].id == p1,
				"only the changed character is sent, with its whole list");
			requireWire(r, p1, "after Bodyguard");
			const JobsView v1 = r.jobs(p1);
			test::require(v1.rows[1].target == p2 && v1.rows[2].target == p2, "the Bodyguard rows name p2");

			// 78 move: the exe's rule
			const std::uint32_t medic = v1.rows[0].jobId;
			const std::uint32_t guard = v1.rows[1].jobId;
			const std::uint32_t close = v1.rows[2].jobId;
			requireReason(r.move(p1, medic, 2), "", "move Medic onto Staying close");
			test::require(r.types(p1) == (Types{45, 31, 58}), "A onto C in ABC is BCA");
			requireReason(r.move(p1, medic, 0), "", "move Medic first");
			test::require(r.types(p1) == (Types{58, 45, 31}), "back to the front");
			requireReason(r.move(p1, medic, 0xFFFF), "", "move Medic last (0xFFFF)");
			test::require(r.types(p1) == (Types{45, 31, 58}), "0xFFFF is last");
			requireReason(r.move(p1, guard, 77), "", "move to an index past the end");
			test::require(r.types(p1) == (Types{31, 58, 45}), "past the end is last");
			requireWire(r, p1, "after the moves");
			requireReason(r.move(p1, 123456, 0), "bad_target", "move a stale jobId");
			requireReason(r.move(p2, close, 0), "bad_target", "move another character's jobId");
			requireReason(r.move(enemy, close, 0), "not_player_controlled", "move on an enemy");

			// 77 remove
			requireReason(r.remove(p1, close), "", "remove Staying close");
			test::require(r.types(p1) == (Types{58, 45}), "the row is gone");
			requireReason(r.remove(p1, close), "bad_target", "remove it again (a stale client)");
			requireReason(r.remove(987654321, 1), "unknown_id", "remove on an unknown id");
			requireWire(r, p1, "after the remove");

			// toggle 10 which 4: JOBS off keeps the rows and greys the ones the AI ignores; adding turns it back on
			requireReason(r.toggleJobs({p1, p2}, 0), "", "JOBS off");
			test::require(!r.jobs(p1).enabled && !r.jobs(p2).enabled && r.types(p1) == (Types{58, 45}), "JOBS off lost rows or did not apply");
			requireWire(r, p1, "JOBS off");
			requireWire(r, p2, "JOBS off");
			requireReason(r.toggleJobs({p1}, 2), "bad_value", "toggle value 2");
			requireReason(r.toggleJobs({enemy}, 0), "not_player_controlled", "toggle on an enemy");
			requireReason(r.add({p1}, 16), "", "Attacking enemies while JOBS is off");
			test::require(r.jobs(p1).enabled && !r.jobs(p2).enabled, "adding a job did not turn JOBS back on for that character only");
			requireReason(r.toggleJobs({p2}, 1), "", "JOBS on");
			test::require(r.jobs(p2).enabled && r.jobs(p2).rows.size() == 1, "JOBS on");
			requireWire(r, p2, "JOBS on");
			std::cout << "orders and frame 34 ok\n";
		}

		// ---- executors ----
		// Medic: without the job a character with a kit leaves a wounded ally alone; with it, it walks over and patches it.
		{
			Rig control(order, gd, data, nav);
			const std::uint64_t m = control.squad.players[0];
			const std::uint64_t patient = control.squad.players[1];
			control.giveKit(m);
			control.wound(patient);
			control.tick(300);
			test::require(control.fighter(m).kitSid.empty(), "a character without the job treated a wounded ally by itself");

			Rig r(order, gd, data, nav);
			test::require(r.squad.players[0] == m && r.squad.players[1] == patient, "the rigs differ");
			r.giveKit(m);
			// no kit, no plan: the row falls through and the character stays idle
			r.wound(patient);
			const double before = r.healthOf(patient);
			requireReason(r.add({patient}, 58), "", "Medic for the patient (no kit)");
			r.tick(100);
			test::require(r.fighter(patient).kitSid.empty() && r.entityOf(patient).get<state::InteractTask>() == nullptr, "a medic without a kit started an interact");
			requireReason(r.add({m}, 58), "", "Medic");
			r.tick(100);
			test::require(!r.fighter(m).kitSid.empty(), "the medic did not open a kit");
			test::require(r.healthOf(patient) > before, "the medic did not patch the wounded ally");
			std::cout << "medic ok: " << before << " -> " << r.healthOf(patient) << "\n";
		}

		// Splint Rigging (249 / 250, Task_FirstAidRig): the same patient walk and kit as the Medic; 249 is stored as 250 (the splint's own
		// effect on a limb is undecoded, so the first aid interact is all it does).
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t m = r.squad.players[0];
			const std::uint64_t patient = r.squad.players[1];
			r.giveKit(m);
			r.wound(patient);
			const double before = r.healthOf(patient);
			requireReason(r.add({m}, 249), "", "Splinting");
			test::require(r.types(m) == Types{250}, "Splinting is stored as Splint Rigging");
			r.tick(100);
			test::require(!r.fighter(m).kitSid.empty() && r.healthOf(patient) > before, "the splint job did not patch the wounded ally");
			std::cout << "splint ok\n";
		}

		// Attacking enemies: a player character never picks a fight (SQUAD_FOLLOW.md s9); with the job it chases the nearest hostile it is
		// aware of. The enemy stands 120 units ahead of it, facing away, so it has not noticed anyone.
		{
			const auto setup = [&](Rig& r)
			{
				const std::uint64_t p1 = r.squad.players[0];
				const std::uint64_t enemy = r.squad.enemies[0];
				r.place(enemy, p1, 120.0, r.fighter(p1).yaw);
				for (const std::uint64_t other : {r.squad.enemies[1], r.squad.enemies[2]})
					r.place(other, p1, 600.0, r.fighter(p1).yaw);
			};
			Rig control(order, gd, data, nav);
			setup(control);
			control.tick(300);
			test::require(control.fighter(control.squad.players[0]).orderTarget == 0 && control.fighter(control.squad.players[0]).target == 0,
				"control: a player character fought without the job");

			Rig r(order, gd, data, nav);
			setup(r);
			const std::uint64_t p1 = r.squad.players[0];
			requireReason(r.add({p1}, 4), "", "Attacking (stored as Attacking enemies)");
			test::require(r.types(p1) == Types{16}, "Attacking is stored as 16");
			r.tick(300);
			const state::Fighter& f = r.fighter(p1);
			const state::Fighter& e = r.fighter(r.squad.enemies[0]);
			std::cout << "attack: target " << f.target << " orderTarget " << f.orderTarget << " enemy blood " << e.blood << " dead " << e.dead << "\n";
			test::require(f.target != 0 || f.orderTarget != 0 || e.blood < 150.0 || e.dead, "the job did not engage the hostile");
			std::cout << "attack ok\n";
		}

		// Staying close, priority, a player's order, remove.
		{
			Rig r(order, gd, data, nav);
			const std::uint64_t p1 = r.squad.players[0];
			const std::uint64_t p2 = r.squad.players[1];
			r.giveKit(p1);

			requireReason(r.add({p1}, 31, p2), "", "Staying close to p2");
			r.tick(25);
			test::require(r.fighter(p1).followTarget == p2, "Staying close did not follow");

			// Medic below Staying close: wounding p2 does not take p1 off it.
			requireReason(r.add({p1}, 58), "", "Medic");
			requireReason(r.add({p1}, 16), "", "Attacking enemies");
			test::require(r.types(p1) == (Types{31, 58, 16}), "rows");
			r.wound(p2);
			r.tick(100);
			test::require(r.fighter(p1).followTarget == p2 && r.fighter(p1).kitSid.empty(), "a lower row took over from the running Staying close");

			// Medic first: it outranks the running follow, treats p2, and the follow comes back after.
			const std::uint32_t medic = r.jobs(p1).rows[1].jobId;
			requireReason(r.move(p1, medic, 0), "", "Medic first");
			r.tick(60);
			test::require(!r.fighter(p1).kitSid.empty(), "the higher row did not take over from the running Staying close");

			// Priority fall-through: nothing to treat, no hostile aware -> Staying close (row 3) runs. Heal p2 fully.
			for (state::FighterPart& part : r.fighter(p2).parts)
			{
				part.health = part.maxHealth;
				part.pool = 0.0;
			}
			r.fighter(p2).wounds.clear();
			r.tick(400);
			test::require(r.entityOf(p1).get<state::InteractTask>() == nullptr && r.fighter(p1).followTarget == p2,
				"the unplannable Medic and Attacking rows did not fall through to Staying close");

			// A player's order outranks the jobs while it runs: a move order ends the follow, the job comes back when the walk ends.
			const state::Fighter& f1 = r.fighter(p1);
			const auto far = r.nav->walker().snap({f1.x + 90.0, f1.y, f1.z}, 100.0);
			test::require(far.has_value(), "no mesh for the move");
			requireReason(r.order_(1, Buf{}.ids({p1}).put(static_cast<float>(far->x)).put(static_cast<float>(far->z)).put(static_cast<float>(far->y)).put<std::uint8_t>(0)), "", "move_to");
			r.tick(12); // a walk is at 70 u/s after 0.35 s (200 u/s^2): 25 ticks could finish a snapped move of under 75 units
			test::require(r.fighter(p1).followTarget == 0 && r.fighter(p1).walking(), "the move order did not replace the follow");
			r.tick(600);
			test::require(!r.fighter(p1).walking() && r.fighter(p1).followTarget == p2, "the job did not resume after the player's order ended");

			// Remove: the running goal is dropped at the next replan.
			std::uint32_t follow = 0;
			for (const state::JobRow& row : r.jobs(p1).rows)
			{
				if (row.type == 31)
					follow = row.jobId;
			}
			test::require(follow != 0, "no Staying close row");
			requireReason(r.remove(p1, follow), "", "remove Staying close");
			r.tick(25);
			test::require(r.fighter(p1).followTarget == 0, "the removed row's goal kept running");
			std::cout << "staying close, priority, player order, remove ok\n";
		}

		// Bodyguard: follows its target and fights the target's attackers, which a plain Staying close does not.
		{
			const auto setup = [&](Rig& r)
			{
				const std::uint64_t p1 = r.squad.players[0];
				const std::uint64_t p2 = r.squad.players[1];
				// place() faces outward; face the protectee so the attacker really acquires it. With the
				// 25 u leader ring the bodyguard starts inside the stopped-slot dead zone and never moves
				// into the outward-facing enemy's view (the old fixture's premise silently disappeared).
				const double angle = r.fighter(p2).yaw + 1.5;
				r.place(r.squad.enemies[0], p2, 25.0, angle);
				r.fighter(r.squad.enemies[0]).yaw = angle + 3.141592653589793;
				for (const std::uint64_t other : {r.squad.enemies[1], r.squad.enemies[2]})
					r.place(other, p1, 600.0, r.fighter(p1).yaw);
			};
			Rig plain(order, gd, data, nav);
			setup(plain);
			requireReason(plain.add({plain.squad.players[0]}, 31, plain.squad.players[1]), "", "Staying close only");
			plain.tick(200);
			const std::uint64_t p1 = plain.squad.players[0];
			test::require(plain.fighter(p1).orderTarget == 0, "control: a plain follower fought the attacker");

			Rig r(order, gd, data, nav);
			setup(r);
			requireReason(r.add({p1}, 45, r.squad.players[1]), "", "Bodyguard of p2");
			bool defended = false;
			for (int i = 0; i < 40 && !defended; ++i)
			{
				r.tick(5);
				defended = r.fighter(p1).orderTarget == r.squad.enemies[0] || r.fighter(p1).target == r.squad.enemies[0];
			}
			std::cout << "bodyguard: enemy target " << r.fighter(r.squad.enemies[0]).target << " p2 " << r.squad.players[1] << " p1 follow " << r.fighter(p1).followTarget << " p1 target " << r.fighter(p1).target << " dist " << std::hypot(r.fighter(p1).x - r.fighter(r.squad.enemies[0]).x, r.fighter(p1).z - r.fighter(r.squad.enemies[0]).z) << " rows " << r.types(p1).size() << " enemy dead " << r.fighter(r.squad.enemies[0]).dead << "\n";
			test::require(defended, "the bodyguard did not engage the attacker of its target");
			std::cout << "bodyguard ok\n";
		}

		// ---- save, load, continue ----
		{
			const auto script = [&](Rig& r)
			{
				const std::uint64_t p1 = r.squad.players[0];
				const std::uint64_t p2 = r.squad.players[1];
				r.add({p1, p2}, 25);
				r.add({p1}, 45, p2);
				r.add({p2}, 4);
				r.tick(3);
				r.move(p1, r.jobs(p1).rows[0].jobId, 0xFFFF);
				r.remove(p2, r.jobs(p2).rows[0].jobId);
				r.toggleJobs({p2}, 0);
				r.tick(60);
			};
			Rig straight(order, gd, data, nav);
			script(straight);
			Rig first(order, gd, data, nav);
			script(first);
			const std::uint64_t p1 = first.squad.players[0];
			const std::uint64_t p2 = first.squad.players[1];
			test::require(first.types(p1) == (Types{45, 31, 58}) && first.types(p2) == Types{16} && !first.jobs(p2).enabled, "the script's lists");
			std::ostringstream saved;
			first.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, first.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == first.sim.stateHash(), "the hash changed across save and load");
			for (const std::uint64_t id : {p1, p2})
				test::require(loaded.jobsOf(id) == first.sim.jobsOf(id), "a jobs list changed across save and load (order included)");
			loaded.run(200);
			straight.sim.run(200);
			test::require(loaded.stateHash() == straight.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			// the counter was saved: a row added after the load never reuses a jobId
			std::uint32_t top = 0;
			for (const state::JobRow& row : loaded.jobsOf(p2).rows)
				top = std::max(top, row.jobId);
			JobOrder add;
			add.kind = 76;
			add.character = p2;
			add.type = 58;
			loaded.enqueue(Command::jobOrder(add));
			loaded.run(1);
			test::require(loaded.jobsOf(p2).rows.back().jobId > top, "the jobId counter was not saved");
			std::cout << "save ok, hash " << std::hex << straight.sim.stateHash() << std::dec << "\n";
		}
		std::cout << "ksim_jobs ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
