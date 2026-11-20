// Player squads (docs/reference/FACTION_SQUADS_LIVE.md, SIMLINK.md PLAYER_SQUADS 31, orders 70-75, event 18): every order and
// every refusal through an in-process Session (no socket), frame 31 decoded by hand (client/src/simlink.* has no type 31 decoder
// yet; this test reads the wire layout of SIMLINK.md itself), CHAR_APPEARANCE.squadId and SQUADS (30), the dismissed character's
// faction, squad and cleared orders, an AI goal 60 s later, and save, load and continue giving the same hash.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

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
	std::string str()
	{
		const std::uint16_t n = get<std::uint16_t>();
		test::require(at + n <= p.size(), "frame overrun");
		std::string s(p.begin() + static_cast<std::ptrdiff_t>(at), p.begin() + static_cast<std::ptrdiff_t>(at + n));
		at += n;
		return s;
	}
};

// PLAYER_SQUADS (type 31) decoded from the layout in SIMLINK.md.
struct Wire31
{
	bool seen = false;
	std::uint64_t faction = 0;
	std::string factionName;
	std::uint16_t count = 0;
	std::uint16_t maxFaction = 0;
	std::uint16_t maxSquad = 0;
	std::uint16_t maxSquads = 0;
	struct Squad
	{
		std::uint64_t id = 0;
		std::string name;
		std::vector<std::uint64_t> members;
	};
	std::vector<Squad> squads;
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
	std::uint64_t rng = 99;
	Wire31 last; // the latest frame 31
	std::vector<std::vector<std::uint8_t>> appearances; // CHAR_APPEARANCE payloads since the last clear
	std::vector<std::vector<std::uint8_t>> squadFrames; // SQUADS payloads
	std::vector<std::vector<std::uint8_t>> events; // EVENT payloads of the latest sendTick

	static CombatSettings settings()
	{
		CombatSettings s;
		s.substeps = 2;
		return s;
	}

	Rig(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, bool ai)
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
		if (ai)
		{
			AiSystem::Config config;
			config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			sim.enableAi(std::move(config));
		}
		connect();
	}

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		events.clear();
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type == ks::kAppearance)
				appearances.push_back(frame.payload);
			else if (frame.type == ks::kSquads)
				squadFrames.push_back(frame.payload);
			else if (frame.type == ks::kEvent)
				events.push_back(frame.payload);
			else if (frame.type == ks::kPlayerSquads)
			{
				Cursor c{frame.payload};
				last = Wire31{};
				last.seen = true;
				c.get<std::uint64_t>();
				last.faction = c.get<std::uint64_t>();
				last.factionName = session->strings().text(c.get<std::uint32_t>());
				last.count = c.get<std::uint16_t>();
				last.maxFaction = c.get<std::uint16_t>();
				last.maxSquad = c.get<std::uint16_t>();
				last.maxSquads = c.get<std::uint16_t>();
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					Wire31::Squad s;
					s.id = c.get<std::uint64_t>();
					s.name = c.str();
					const std::uint16_t m = c.get<std::uint16_t>();
					for (std::uint16_t k = 0; k < m; ++k)
						s.members.push_back(c.get<std::uint64_t>());
					last.squads.push_back(std::move(s));
				}
				c.get<std::int64_t>(); // the trailing money (MINE_SELL_SLICE 3.1)
				test::require(c.at == frame.payload.size(), "frame 31 has trailing bytes");
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
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		last = Wire31{};
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

	// Order helpers.
	std::string add() { return order_(70, Buf{}); }
	std::string remove(std::uint64_t id) { return order_(71, Buf{}.put(id)); }
	std::string rename(std::uint64_t id, const std::string& name) { return order_(72, Buf{}.put(id).str(name)); }
	std::string move(std::uint64_t who, std::uint64_t to, std::uint16_t index, std::uint8_t swap)
	{
		return order_(73, Buf{}.put(who).put(to).put(index).put(swap));
	}
	std::string reorder(std::uint64_t id, std::uint16_t index) { return order_(74, Buf{}.put(id).put(index)); }
	std::string dismiss(std::uint64_t who) { return order_(75, Buf{}.put(who)); }

	std::uint64_t spawnPlayer()
	{
		const kdata::Record* body = nullptr;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
		{
			if (r->name == "Escaped Servant")
				body = r;
		}
		state::Fighter f = data->makeFighter(*gd, body->stringID, "", rng);
		f.team = 0;
		f.playerControlled = true;
		const state::Fighter& anchor = *sim.combat()->find(squad.players[0]);
		const auto spot = nav->walker().snap({anchor.x + 3.0, anchor.y, anchor.z + 3.0}, 100.0);
		test::require(spot.has_value(), "no mesh next to the squad");
		f.x = spot->x;
		f.y = spot->y;
		f.z = spot->z;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	// Truth from the sim, to compare with the wire.
	PlayerSquadsView view() const { return sim.playerSquads(); }
	const Wire31::Squad& wireSquad(std::uint64_t id) const
	{
		for (const Wire31::Squad& s : last.squads)
		{
			if (s.id == id)
				return s;
		}
		throw test::Failure("squad " + std::to_string(id) + " is not in the last frame 31");
	}
};

// The squad ids, in list order, from the sim.
std::vector<std::uint64_t> listOrder(const Rig& r)
{
	std::vector<std::uint64_t> ids;
	for (const PlayerSquadsView::Squad& s : r.view().squads)
		ids.push_back(s.id);
	return ids;
}

const PlayerSquadsView::Squad& squadOf(const PlayerSquadsView& v, std::uint64_t id)
{
	for (const PlayerSquadsView::Squad& s : v.squads)
	{
		if (s.id == id)
			return s;
	}
	throw test::Failure("no such squad in the view");
}

using Ids = std::vector<std::uint64_t>;

// Every squad of the sim is what the last frame 31 says, and none is over max.
void requireWireMatches(const Rig& r, const std::string& where)
{
	const PlayerSquadsView v = r.view();
	test::require(r.last.seen, where + ": no frame 31");
	test::require(r.last.faction == v.faction && r.last.factionName == v.factionName && r.last.count == v.factionCount, where + ": header differs");
	test::require(r.last.maxFaction == v.limits.faction && r.last.maxSquad == v.limits.squad && r.last.maxSquads == v.limits.squads, where + ": limits differ");
	test::require(r.last.squads.size() == v.squads.size(), where + ": squad count differs");
	for (std::size_t i = 0; i < v.squads.size(); ++i)
	{
		test::require(r.last.squads[i].id == v.squads[i].id && r.last.squads[i].name == v.squads[i].name
			&& r.last.squads[i].members == v.squads[i].members, where + ": squad " + std::to_string(i) + " differs");
		test::require(v.squads[i].members.size() <= v.limits.squad, where + ": a squad is over max");
	}
}

void requireReason(const std::string& got, const std::string& want, const std::string& what)
{
	test::require(got == want, what + ": answered '" + got + "', wanted '" + want + "'");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_squads HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		{
			Rig r(order, gd, data, nav, false);
			const std::uint64_t p1 = r.squad.players[0];
			const std::uint64_t p2 = r.squad.players[1];
			const std::uint64_t enemy = r.squad.enemies[0];

			// ---- on connect: frame 31 ----
			requireWireMatches(r, "connect");
			test::require(r.last.factionName == "Nameless" && r.last.count == 2 && r.last.squads.size() == 2, "the squad scenario starts with two squads");
			test::require(r.last.maxFaction == 30 && r.last.maxSquad == 20 && r.last.maxSquads == 10, "the limits are not the vanilla merged 30 / 20 / 10");
			const std::uint64_t s1 = r.last.squads[0].id;
			const std::uint64_t s2 = r.last.squads[1].id;
			test::require(r.last.squads[0].name == "Nameless_1" && r.last.squads[1].name == "Nameless_2", "scenario squad names");
			test::require(r.last.squads[0].members == Ids{p1} && r.last.squads[1].members == Ids{p2}, "scenario members");

			// ---- the cached playerSquads view follows every writer of the player's money ----
			{
				const std::int64_t base = r.view().money;
				test::require(r.sim.adjustPlayerMoney(250), "adjustPlayerMoney found no player faction");
				test::require(r.view().money == base + 250, "playerSquads().money is stale after adjustPlayerMoney");
				test::require(r.sim.adjustPlayerMoney(-100) && r.view().money == base + 150, "playerSquads().money is stale after a second adjustPlayerMoney");
				test::require(r.sim.importPlayerFaction(777, {}), "importPlayerFaction found no player faction");
				test::require(r.view().money == 777, "playerSquads().money is stale after importPlayerFaction");
				test::require(r.sim.adjustPlayerMoney(23) && r.view().money == 800, "playerSquads().money is stale after adjust following an import");
			}

			// ---- 70 squad_add, up to max, then not_allowed; the counter is never reused ----
			requireReason(r.add(), "", "add");
			requireWireMatches(r, "after add");
			const std::uint64_t s3 = r.last.squads[2].id;
			test::require(r.last.squads[2].name == "Nameless_3" && r.last.squads[2].members.empty(), "the first added squad is Nameless_3 and empty");
			for (int i = 4; i <= 10; ++i)
				requireReason(r.add(), "", "add " + std::to_string(i));
			test::require(r.last.squads.size() == 10 && r.last.squads[9].name == "Nameless_10", "ten squads");
			requireReason(r.add(), "not_allowed", "add at max squads");
			test::require(r.view().squads.size() == 10, "a refused add changed the list");
			const std::uint64_t s10 = r.last.squads[9].id;
			requireReason(r.remove(s10), "", "delete an empty squad");
			requireReason(r.add(), "", "add after a delete");
			test::require(r.last.squads[9].name == "Nameless_11", "the name counter was reused after a delete");
			requireReason(r.remove(r.last.squads[9].id), "", "delete the new squad");
			for (std::size_t i = 8; i > 2; --i)
				requireReason(r.remove(r.view().squads[i].id), "", "delete an empty squad");
			test::require(listOrder(r) == (Ids{s1, s2, s3}), "squads 4..9 are deleted, 1..3 stay");
			requireWireMatches(r, "after deletes");

			// ---- 71 refusals ----
			requireReason(r.remove(s1), "not_allowed", "delete a non-empty squad");
			requireReason(r.remove(987654321), "bad_target", "delete an unknown squad");
			test::require(listOrder(r) == (Ids{s1, s2, s3}), "a refused delete changed the list");

			// ---- 72 rename ----
			requireReason(r.rename(s3, std::string(33, 'a')), "bad_value", "rename to 33 characters");
			test::require(r.view().squads[2].name == "Nameless_3", "a refused rename changed the name");
			requireReason(r.rename(s3, std::string(32, 'b')), "", "rename to 32 characters");
			test::require(squadOf(r.view(), s3).name == std::string(32, 'b'), "32-character name");
			std::string accents;
			for (int i = 0; i < 32; ++i)
				accents += "\xC3\xA9"; // 32 characters, 64 bytes
			requireReason(r.rename(s3, accents), "", "rename to 32 two-byte characters");
			requireReason(r.rename(s3, accents + "\xC3\xA9"), "bad_value", "rename to 33 two-byte characters");
			requireReason(r.rename(s3, "ab\xFF"), "bad_value", "rename to bad UTF-8");
			requireReason(r.rename(s3, "\xC3"), "bad_value", "rename to a truncated sequence");
			requireReason(r.rename(s3, ""), "", "rename to empty");
			test::require(squadOf(r.view(), s3).name.empty(), "the empty name was kept");
			requireWireMatches(r, "after renames");
			requireReason(r.rename(987654321, "x"), "bad_target", "rename an unknown squad");

			// ---- 73 squad_move ----
			// insert at 0 makes the character leader
			requireReason(r.move(p2, s1, 0, 0), "", "insert at 0");
			test::require(squadOf(r.view(), s1).members == (Ids{p2, p1}), "insert at 0: p2 is the leader of s1");
			test::require(squadOf(r.view(), s2).members.empty(), "the old squad keeps existing, empty");
			// append, to another squad
			requireReason(r.move(p1, s2, 0xFFFF, 0), "", "append");
			test::require(squadOf(r.view(), s1).members == Ids{p2} && squadOf(r.view(), s2).members == Ids{p1}, "append moved p1 to s2");
			requireWireMatches(r, "after moves");
			// fill s1 to max: 19 more players
			Ids extra;
			for (int i = 0; i < 19; ++i)
			{
				extra.push_back(r.spawnPlayer());
				r.sim.addPlayerMember(s1, extra.back());
			}
			r.tick();
			test::require(squadOf(r.view(), s1).members.size() == 20, "s1 is full");
			// a full squad refuses an insert from another squad and an append, and never goes over max
			requireReason(r.move(p1, s1, 3, 0), "not_allowed", "insert into a full squad");
			requireReason(r.move(p1, s1, 0xFFFF, 0), "not_allowed", "append to a full squad");
			test::require(squadOf(r.view(), s1).members.size() == 20 && squadOf(r.view(), s2).members == Ids{p1}, "a refused move changed the squads");
			// ... but takes a move inside the squad, and a swap across squads
			requireReason(r.move(extra[2], s1, 0, 0), "", "move inside a full squad");
			test::require(squadOf(r.view(), s1).members[0] == extra[2] && squadOf(r.view(), s1).members.size() == 20, "moved to the leader slot, size unchanged");
			const Ids before = squadOf(r.view(), s1).members;
			requireReason(r.move(p1, s1, 5, 1), "", "swap across squads while full");
			{
				const PlayerSquadsView v = r.view();
				test::require(squadOf(v, s1).members.size() == 20 && squadOf(v, s2).members == Ids{before[5]}, "the swapped member went to s2");
				test::require(squadOf(v, s1).members[5] == p1, "p1 took the slot");
			}
			// swap inside one squad
			requireReason(r.move(p1, s1, 0, 1), "", "swap inside a squad");
			{
				const Ids now = squadOf(r.view(), s1).members;
				test::require(now[0] == p1 && now[5] == before[0], "positions 0 and 5 exchanged");
			}
			requireReason(r.move(p1, s1, 0, 1), "", "swap with itself");
			// refusals
			requireReason(r.move(p1, s1, 20, 1), "bad_value", "swap with no member at the index");
			requireReason(r.move(p1, s1, 0xFFFF, 1), "bad_value", "swap at the append index");
			requireReason(r.move(p1, s1, 0, 2), "bad_value", "swap flag 2");
			requireReason(r.move(p1, 987654321, 0, 0), "bad_target", "move to an unknown squad");
			requireReason(r.move(987654321, s1, 0, 0), "unknown_id", "move an unknown character");
			requireReason(r.move(enemy, s1, 0, 0), "not_player_controlled", "move an enemy");
			requireReason(r.move(p1, s3, 99, 0), "", "insert past the end appends");
			test::require(squadOf(r.view(), s3).members == Ids{p1}, "insert past the end appended");
			requireWireMatches(r, "after the move refusals");
			for (const PlayerSquadsView::Squad& s : r.view().squads)
				test::require(s.members.size() <= 20, "a squad went over max");

			// ---- the squad id rides in CHAR_APPEARANCE and the member counts in SQUADS ----
			r.appearances.clear();
			requireReason(r.move(p1, s2, 0, 0), "", "move p1 back to s2");
			r.tick();
			{
				std::uint64_t seen = ~0ull;
				for (const std::vector<std::uint8_t>& payload : r.appearances)
				{
					Cursor c{payload};
					c.get<std::uint64_t>();
					if (c.get<std::uint64_t>() == p1)
						seen = c.get<std::uint64_t>();
				}
				test::require(seen == s2, "CHAR_APPEARANCE.squadId did not follow the move");
			}
			r.tick(40); // SQUADS comes at 1-2 Hz
			{
				test::require(!r.squadFrames.empty(), "no SQUADS frame");
				bool found = false;
				for (const std::vector<std::uint8_t>& payload : r.squadFrames)
				{
					Cursor c{payload};
					c.get<std::uint64_t>();
					const std::uint32_t n = c.get<std::uint32_t>();
					for (std::uint32_t i = 0; i < n; ++i)
					{
						const std::uint64_t id = c.get<std::uint64_t>();
						c.get<std::uint32_t>();
						c.get<float>();
						c.get<float>();
						const std::uint16_t members = c.get<std::uint16_t>();
						if (id == s1 && members == squadOf(r.view(), s1).members.size())
							found = true;
					}
				}
				test::require(found, "SQUADS (30) has no row for s1 with its member count");
			}

			// ---- 74 reorder ----
			requireReason(r.reorder(s3, 0), "", "reorder to 0");
			test::require(listOrder(r) == (Ids{s3, s1, s2}), "reorder to the front");
			requireReason(r.reorder(s3, 500), "", "reorder past the end");
			test::require(listOrder(r) == (Ids{s1, s2, s3}), "reorder clamps to the last index");
			requireReason(r.reorder(987654321, 0), "bad_target", "reorder an unknown squad");
			requireWireMatches(r, "after reorders");

			// ---- 75 dismiss ----
			const std::uint64_t e0 = extra[0];
			r.sim.enqueue(Command::fighterFollow(state::Id{e0}, state::Id{p2}));
			r.tick();
			test::require(r.sim.combat()->find(e0)->followTarget != 0, "e0 should be following");
			const std::uint16_t countBefore = r.view().factionCount;
			r.events.clear();
			requireReason(r.dismiss(e0), "", "dismiss");
			{
				const state::Fighter& f = *r.sim.combat()->find(e0);
				test::require(!f.playerControlled && f.followTarget == 0 && !f.hold && f.orderTarget == 0 && f.route.empty(), "the dismissed character keeps orders or control");
				test::require(r.view().factionCount == countBefore - 1, "the faction count did not drop");
				for (const PlayerSquadsView::Squad& s : r.view().squads)
				{
					for (const std::uint64_t m : s.members)
						test::require(m != e0, "the dismissed character is still in a squad");
				}
				const kdata::Record* faction = gd->find("1233-gamedata.base");
				test::require(faction != nullptr, "no faction 1233-gamedata.base");
				std::cout << "dismiss faction 1233-gamedata.base is named '" << faction->name << "' in this load order\n";
				flecs::entity entity;
				r.sim.world().ecs().each([&](flecs::entity e, const state::Id& id, const state::Fighter&) { if (id.value == e0) entity = e; });
				const state::Allegiance* allegiance = entity.get<state::Allegiance>();
				test::require(allegiance != nullptr && allegiance->factionSid == "1233-gamedata.base", "the dismissed character's allegiance");
				const flecs::entity squad = entity.target<state::MemberOf>();
				test::require(squad.is_valid() && squad.get<state::SquadInstance>() != nullptr, "the dismissed character has no new squad");
				const state::Position* position = squad.get<state::Position>();
				// The squad is made at the character as the command runs; the same tick's depenetration then moves the character (a follower stands within R of its
				// leader), by at most half an overlap.
				test::require(position != nullptr && std::hypot(position->x - f.x, position->z - f.z) < 1.0, "the new squad is not at the character's position");
				const flecs::entity factionState(r.sim.world().ecs().c_ptr(), squad.get<state::SquadInstance>()->faction.id);
				test::require(factionState.is_valid() && factionState.get<state::FactionState>()->templateSid == "1233-gamedata.base", "the new squad's faction");
				// event 18
				bool toast = false;
				for (const std::vector<std::uint8_t>& payload : r.events)
				{
					Cursor c{payload};
					c.get<std::uint64_t>();
					if (c.get<std::uint16_t>() != 18)
						continue;
					test::require(c.get<std::uint64_t>() == e0, "event 18 names another character");
					test::require(r.session->strings().text(c.get<std::uint32_t>()) == faction->name, "event 18 faction name");
					test::require(c.get<std::uint64_t>() == squad.get<state::Id>()->value, "event 18 squad id");
					toast = true;
				}
				test::require(toast, "no event 18");
			}
			requireWireMatches(r, "after the dismissal");
			// the appearance carries the new squad
			requireReason(r.dismiss(e0), "not_player_controlled", "dismiss a dismissed character");
			requireReason(r.dismiss(987654321), "unknown_id", "dismiss an unknown character");
			requireReason(r.dismiss(enemy), "not_player_controlled", "dismiss an enemy");

			// ---- the last squad, the last character ----
			// Put every member into s1 as far as it goes, dismiss down to one character, then the last one is refused.
			for (const std::uint64_t id : extra)
			{
				if (id != e0 && id != extra[1])
					requireReason(r.dismiss(id), "", "dismiss");
			}
			// remaining player characters: p1, p2, extra[1]
			requireReason(r.dismiss(extra[1]), "", "dismiss");
			requireReason(r.dismiss(p1), "", "dismiss");
			test::require(r.view().factionCount == 1, "one character should be left");
			requireReason(r.dismiss(p2), "not_allowed", "dismiss the last character");
			test::require(r.view().factionCount == 1 && r.sim.combat()->find(p2)->playerControlled, "the last character was dismissed");
			// delete the empty squads down to one: the squad that holds p2 stays
			{
				const Ids all = listOrder(r);
				for (const std::uint64_t id : all)
				{
					if (squadOf(r.view(), id).members.empty())
						requireReason(r.remove(id), "", "delete an empty squad");
				}
				test::require(r.view().squads.size() == 1 && r.view().squads[0].members == Ids{p2}, "one squad with p2 should stay");
			}
			// the last squad is never deleted, even empty: the character leaves the arena (a despawn), the squad is pruned
			r.sim.removeFighter(p2);
			r.tick();
			test::require(r.view().squads.size() == 1 && r.view().squads[0].members.empty() && r.view().factionCount == 0, "a removed character stays in its squad");
			requireReason(r.remove(r.view().squads[0].id), "not_allowed", "delete the faction's last squad");
			requireWireMatches(r, "end");
			std::cout << "orders ok\n";
		}

		// ---- an AI goal 60 s after a dismissal (the squad AI, no navigator: it can only idle, still with a goal) ----
		{
			Rig r(order, gd, data, nav, true);
			const std::uint64_t p1 = r.squad.players[0];
			requireReason(r.dismiss(p1), "", "dismiss with the squad AI on");
			flecs::entity entity;
			r.sim.world().ecs().each([&](flecs::entity e, const state::Id& id, const state::Fighter&) { if (id.value == p1) entity = e; });
			const flecs::entity squad = entity.target<state::MemberOf>();
			test::require(squad.is_valid(), "no new squad");
			r.tick(60 * 20);
			const state::GroupAI* ai = squad.get<state::GroupAI>();
			test::require(ai != nullptr && ai->goal != state::GoalKind::None, "the dismissed character's squad has no AI goal after 60 s");
			std::cout << "AI goal after 60 s: " << static_cast<int>(ai->goal) << "\n";
		}

		// ---- save, load, continue ----
		{
			const auto script = [&](Rig& r)
			{
				r.add();
				r.rename(r.view().squads.back().id, "Saved");
				const std::uint64_t extra = r.spawnPlayer();
				r.sim.addPlayerMember(r.view().squads[0].id, extra);
				r.tick();
				r.move(extra, r.view().squads[1].id, 0, 0);
				r.dismiss(r.squad.players[0]);
				r.tick(40);
			};
			Rig straight(order, gd, data, nav, false);
			script(straight);
			Rig first(order, gd, data, nav, false);
			script(first);
			std::ostringstream saved;
			first.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, first.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == first.sim.stateHash(), "the hash changed across save and load");
			test::require(loaded.playerSquads() == first.sim.playerSquads(), "the squads changed across save and load");
			loaded.run(200);
			straight.sim.run(200);
			test::require(loaded.stateHash() == straight.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			// a squad added after the load takes the next counter value
			SquadOrder add;
			add.kind = 70;
			loaded.enqueue(Command::squadOrder(add));
			loaded.run(1);
			const std::string name = loaded.playerSquads().squads.back().name;
			test::require(name == "Nameless_4", "the name counter was not saved (got " + name + ")");
			std::cout << "save ok, hash " << std::hex << straight.sim.stateHash() << std::dec << "\n";
		}
		std::cout << "ksim_squads ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
