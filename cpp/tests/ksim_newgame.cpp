// Single-player new game over SIMLINK (docs/formats/SIMLINK.md order 84 new_world, STARTS 33, NEW_GAME 111 and its reply 62;
// docs/MULTIPLAYER.md section 4): through in-process Sessions (no socket), the new frames decoded by hand from the documented layouts.
//   STARTS on connect (every start-off, the Hub ones ready), new_world (no player faction, no scenario squads, event 9 then the full
//   resend then STARTS, a non-admin session refused, the same world every time), NEW_GAME n = 0 for every ready start-off (reply 62 with
//   the members, then PLAYER_SQUADS, the members take a move order, money / relations / allegiance), every reject reason, every kick,
//   the logged command replays to the same hash, and save -> load -> continue after a new game.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "net/simlink.hpp" // the client's decoder (client/src), compiled in unchanged

#include "ksim/core/clock.hpp"
#include "ksim/save/creator.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_saves.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;
namespace fs = std::filesystem;

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

// STARTS (33): u16 n, n x {strid startoff, u8 ready, u8 k, k x strid race, u8 m, m x {strid race, u8 female, str name, u8 flags, strid character, u8 a, a x strid armour, strid weapon}}.
struct Wire33
{
	struct Member
	{
		std::uint32_t race = 0;
		std::uint8_t female = 0;
		std::string name;
		std::uint8_t flags = 0;
		std::uint32_t character = 0;
		std::vector<std::uint32_t> armour;
		std::uint32_t weapon = 0;
	};
	struct Entry
	{
		std::uint32_t id = 0;
		std::uint8_t ready = 0;
		std::vector<std::uint32_t> forceRace;
		std::vector<Member> members;
	};
	std::vector<Entry> entries;
	static Wire33 decode(const std::vector<std::uint8_t>& payload)
	{
		Cursor c{payload};
		Wire33 w;
		const std::uint16_t n = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < n; ++i)
		{
			Entry e;
			e.id = c.get<std::uint32_t>();
			e.ready = c.get<std::uint8_t>();
			const std::uint8_t k = c.get<std::uint8_t>();
			for (std::uint8_t j = 0; j < k; ++j)
				e.forceRace.push_back(c.get<std::uint32_t>());
			const std::uint8_t m = c.get<std::uint8_t>();
			for (std::uint8_t j = 0; j < m; ++j)
			{
				Member member;
				member.race = c.get<std::uint32_t>();
				member.female = c.get<std::uint8_t>();
				member.name = c.str();
				member.flags = c.get<std::uint8_t>();
				member.character = c.get<std::uint32_t>();
				const std::uint8_t a = c.get<std::uint8_t>();
				for (std::uint8_t q = 0; q < a; ++q)
					member.armour.push_back(c.get<std::uint32_t>());
				member.weapon = c.get<std::uint32_t>();
				e.members.push_back(std::move(member));
			}
			w.entries.push_back(std::move(e));
		}
		test::require(c.at == payload.size(), "frame 33 has trailing bytes");
		return w;
	}
};

// NEW_GAME_RESULT (62): u8 result; 0 = ok: u64 factionId, u16 n, n x u64 member; 1 = rejected: strid reason.
struct Wire62
{
	std::uint8_t result = 0;
	std::uint64_t faction = 0;
	std::vector<std::uint64_t> members;
	float pos[3] = {};
	std::uint32_t reason = 0;
	static Wire62 decode(const std::vector<std::uint8_t>& payload)
	{
		Cursor c{payload};
		Wire62 w;
		w.result = c.get<std::uint8_t>();
		if (w.result == 0)
		{
			w.faction = c.get<std::uint64_t>();
			const std::uint16_t n = c.get<std::uint16_t>();
			for (std::uint16_t i = 0; i < n; ++i)
				w.members.push_back(c.get<std::uint64_t>());
			for (float& v : w.pos)
				v = c.get<float>();
		} else {
			w.reason = c.get<std::uint32_t>();
		}
		test::require(c.at == payload.size(), "frame 62 has trailing bytes");
		return w;
	}
};

// PLAYER_SQUADS (31): u64 tick, u64 faction, strid factionName, u16 count, u16 maxFaction, u16 maxSquad, u16 maxSquads, u16 n x
// {u64 id, str name, u16 m, m x u64}.
struct Wire31
{
	std::uint64_t faction = 0;
	std::vector<std::vector<std::uint64_t>> squads;
	std::vector<std::string> names;
	static Wire31 decode(const std::vector<std::uint8_t>& payload)
	{
		Cursor c{payload};
		Wire31 w;
		c.get<std::uint64_t>();
		w.faction = c.get<std::uint64_t>();
		c.get<std::uint32_t>();
		c.get<std::uint16_t>();
		c.get<std::uint16_t>();
		c.get<std::uint16_t>();
		c.get<std::uint16_t>();
		const std::uint16_t n = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < n; ++i)
		{
			c.get<std::uint64_t>();
			w.names.push_back(c.str());
			std::vector<std::uint64_t> members;
			const std::uint16_t m = c.get<std::uint16_t>();
			for (std::uint16_t k = 0; k < m; ++k)
				members.push_back(c.get<std::uint64_t>());
			w.squads.push_back(std::move(members));
		}
		c.get<std::int64_t>(); // the trailing money (MINE_SELL_SLICE 3.1)
		test::require(c.at == payload.size(), "frame 31 has trailing bytes");
		return w;
	}
};

// UNLOCKS (36): u64 tick, u16 nResearch, strid x n, u16 nBuild, strid x n.
struct Wire36
{
	std::uint64_t tick = 0;
	std::vector<std::uint32_t> research;
	std::vector<std::uint32_t> buildings;
	static Wire36 decode(const std::vector<std::uint8_t>& payload)
	{
		Cursor c{payload};
		Wire36 w;
		w.tick = c.get<std::uint64_t>();
		const std::uint16_t nResearch = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < nResearch; ++i)
			w.research.push_back(c.get<std::uint32_t>());
		const std::uint16_t nBuild = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < nBuild; ++i)
			w.buildings.push_back(c.get<std::uint32_t>());
		test::require(c.at == payload.size(), "frame 36 has trailing bytes");
		return w;
	}
};

// What one call put on the wire.
struct Output
{
	std::vector<Frame> all;
	std::vector<Wire36> unlocks;
	std::vector<Wire33> starts;
	std::vector<Wire62> results;
	std::vector<Wire31> squads;
	std::string reason; // an order's rejection ("" = applied)
	bool answered = false;
	int count(std::uint16_t type) const
	{
		int n = 0;
		for (const Frame& f : all)
			n += f.type == type;
		return n;
	}
	// Index of the first frame of this type, -1 if none.
	int index(std::uint16_t type) const
	{
		for (std::size_t i = 0; i < all.size(); ++i)
		{
			if (all[i].type == type)
				return static_cast<int>(i);
		}
		return -1;
	}
};

struct Client
{
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 0;
	Output first; // what the handshake wrote
	mutable ::simlink::SimLinkDecoder dec; // the client's decoder, fed every write when `decode` is set (character creation checks)
	bool decode = false;

	Client(Simulation& sim, ks::ServerContext& context, bool admin, std::uint64_t dataHash, bool decodeFrames = false)
		: session(std::make_unique<ks::Session>(sim, context)), decode(decodeFrames)
	{
		session->setAdmin(admin);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		first = digest(bytes, 0);
	}

	Output digest(const std::vector<std::uint8_t>& bytes, std::uint32_t orderSeq) const
	{
		Output out;
		out.all = frames(bytes);
		if (decode)
		{
			for (std::size_t at = 0; at < bytes.size();)
			{
				std::uint32_t len = 0;
				std::memcpy(&len, &bytes[at], 4);
				std::uint16_t type = 0;
				std::memcpy(&type, &bytes[at + 4], 2);
				// NEW_GAME_RESULT is decoded by hand (Wire62): it ends with the leader's x y z, which the client's decoder reads only once 0f updates it.
				if (type != ks::kNewGameResult)
					{
						const std::vector<std::uint8_t> view = test::clientFrame(&bytes[at], 4u + len);
						test::require(dec.feed(view.data(), view.size()), "the client decoder: " + dec.error());
					}
				at += 4 + len;
			}
			dec.events.clear();
			dec.navPatches.clear();
		}
		for (const Frame& frame : out.all)
		{
			if (frame.type == ks::kStarts)
				out.starts.push_back(Wire33::decode(frame.payload));
			if (frame.type == ks::kNewGameResult)
				out.results.push_back(Wire62::decode(frame.payload));
			if (frame.type == ks::kPlayerSquads)
				out.squads.push_back(Wire31::decode(frame.payload));
			if (frame.type == ks::kUnlocks)
				out.unlocks.push_back(Wire36::decode(frame.payload));
			if (frame.type != ks::kEvent || orderSeq == 0)
				continue;
			Cursor c{frame.payload};
			c.get<std::uint64_t>();
			const std::uint16_t kind = c.get<std::uint16_t>();
			if ((kind == 9 || kind == 10) && c.get<std::uint32_t>() == orderSeq)
			{
				out.answered = true;
				if (kind == 10)
					out.reason = session->strings().text(c.get<std::uint32_t>());
			}
		}
		return out;
	}

	Output order(std::uint16_t kind, const Buf& body)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order closed the session");
		Output out = digest(bytes, seq);
		test::require(out.answered, "order " + std::to_string(kind) + " got no answer");
		return out;
	}

	// A frame of any type; the session must accept it (onFrame true).
	Output frame(std::uint16_t type, const Buf& body)
	{
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(type, body.bytes.data(), body.bytes.size(), w), "frame " + std::to_string(type) + " closed the session");
		return digest(bytes, 0);
	}

	// A frame the session must refuse (a protocol error: onFrame false).
	bool refused(std::uint16_t type, const Buf& body)
	{
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		return !session->onFrame(type, body.bytes.data(), body.bytes.size(), w);
	}

	Output sendTick(bool initial = false)
	{
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->sendTick(w, initial);
		return digest(bytes, 0);
	}

	Output tick(Simulation& sim)
	{
		sim.run(1);
		return sendTick();
	}

	// NEW_GAME with n empty-ish Specs (the shape is checked, the content is not read yet).
	static Buf newGameBody(const std::string& name, std::uint32_t startoff, std::uint8_t n)
	{
		Buf b;
		b.put<std::uint32_t>(1).str(name).put<std::uint32_t>(startoff).put<std::uint8_t>(n);
		for (std::uint8_t i = 0; i < n; ++i)
		{
			b.put<std::uint32_t>(0).put<std::uint8_t>(0).put<std::uint32_t>(0).put<std::uint32_t>(0).put<std::uint32_t>(0).put<std::uint32_t>(0);
			b.put<float>(0.5f).put<std::uint8_t>(1).str("height").put<float>(0.25f).str("Spec").put<std::int32_t>(0);
		}
		return b;
	}

	std::string text(std::uint32_t id) const { return session->strings().text(id); }

	// The strid the server gave a record sid (kind 1); the creator's sids are in the table from the STARTS write on.
	std::uint32_t strid(const std::string& sid) const
	{
		if (sid.empty())
			return 0;
		if (sid == kBadSid)
			return 99999;
		for (std::uint32_t id = 1; id <= session->strings().size(); ++id)
		{
			if (session->strings().entry(id).kind == 1 && session->strings().text(id) == sid)
				return id;
		}
		throw test::Failure("the string table has no sid " + sid);
	}

	// NEW_GAME with real Specs (frame 111).
	Buf specBody(const std::string& name, std::uint32_t startoff, const std::vector<CharacterSpec>& specs) const
	{
		Buf b;
		b.put<std::uint32_t>(1).str(name).put<std::uint32_t>(startoff).put<std::uint8_t>(static_cast<std::uint8_t>(specs.size()));
		for (const CharacterSpec& s : specs)
		{
			b.put<std::uint32_t>(strid(s.race)).put<std::uint8_t>(s.female).put<std::uint32_t>(strid(s.head)).put<std::uint32_t>(strid(s.hair))
				.put<std::uint32_t>(strid(s.beard)).put<std::uint32_t>(strid(s.hairColour)).put<float>(s.skin).put<std::uint8_t>(static_cast<std::uint8_t>(s.sliders.size()));
			for (const auto& [sliderName, value] : s.sliders)
				b.str(sliderName).put<float>(value);
			b.str(s.name).put<std::int32_t>(s.stance);
		}
		return b;
	}

	std::vector<std::string> texts(const std::vector<std::uint32_t>& ids) const
	{
		std::vector<std::string> out;
		for (const std::uint32_t id : ids)
			out.push_back(text(id));
		return out;
	}
};

void requireReason(const Output& out, const std::string& want, const std::string& what)
{
	test::require(out.reason == want, what + ": answered '" + out.reason + "', wanted '" + want + "'");
}

// ---- the world ----

struct Rig
{
	kworld::LoadOrder order;
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::unique_ptr<Simulation> sim;
	ks::ServerContext context;
	std::shared_ptr<const StartCatalog> catalog;
	std::map<std::string, std::string> startSid; // start-off name -> sid
	std::string namelessSid;
};

std::uint32_t startId(const Client& client, const Rig& rig, const std::string& name)
{
	for (const Wire33::Entry& e : client.first.starts.at(0).entries)
	{
		if (client.text(e.id) == rig.startSid.at(name))
			return e.id;
	}
	throw test::Failure("no STARTS entry for " + name);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_newgame HUB.knav");
		Rig rig;
		rig.order = kworld::vanillaLoadOrder(KENSHI_DATA);
		rig.gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(rig.order));
		rig.data = CombatData::build(*rig.gd, KENSHI_DATA);
		rig.nav = MeshNavigator::load(argv[1]);
		for (const kdata::Record* r : rig.gd->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			rig.startSid[r->name] = r->stringID;
		for (const kdata::Record* r : rig.gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (r->name == "Nameless")
				rig.namelessSid = r->stringID;
		}
		rig.catalog = std::make_shared<const StartCatalog>(StartCatalog::build(*rig.gd, KENSHI_DATA));
		test::require(rig.startSid.size() == 13 && !rig.namelessSid.empty(), "the vanilla data holds 13 start-offs and Nameless (Hungover Ninja is removed)");

		const std::string base = std::string(KSIM_BINARY_DIR) + "/ksim_newgame_test";
		fs::remove_all(base);
		const std::string saveDir = ks::prepareSaveDir(base + "/saves");

		SpawnOptions spawn;
		spawn.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
		spawn.seed = 7;
		rig.sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, rig.order, spawn));
		Simulation& sim = *rig.sim;
		ks::ServerContext& context = rig.context;
		context.gameData = rig.gd;
		context.dataHash = ks::dataHash(rig.order);
		context.saveDir = saveDir;
		sim.markPristine(); // the server marks the fresh world before any scenario, as ksim_server does
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(rig.data, settings);
		sim.enableNavigation(rig.nav);
		const ks::Squad scenario = ks::spawnSquad(sim, *rig.gd, *rig.data, context);
		sim.run(5);
		const std::uint64_t freshTick = sim.clockTick() - 5;

		Client admin(sim, context, true, context.dataHash);
		Client other(sim, context, false, context.dataHash);

		// ---- STARTS on connect: every start-off, in record order; the Hub ones are ready ----
		{
			test::require(admin.first.starts.size() == 1 && other.first.starts.size() == 1, "one STARTS frame in the handshake, also to a non-admin session");
			const Wire33& w = admin.first.starts[0];
			const std::vector<const kdata::Record*> records = rig.gd->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF"));
			test::require(w.entries.size() == 13, "STARTS lists all 13 start-offs");
			std::size_t at = 0;
			for (const kdata::Record* r : records)
			{
				if (r->boolOr("REMOVED", false))
					continue;
				test::require(admin.text(w.entries.at(at).id) == r->stringID, "STARTS is in record order: " + r->name);
				++at;
			}
			const std::set<std::string> ready = {"Wanderer", "Nobodies", "The Holy Sword", "Son of a Captain"};
			for (const Wire33::Entry& e : w.entries)
			{
				std::string name;
				for (const auto& [n, sid] : rig.startSid)
				{
					if (sid == admin.text(e.id))
						name = n;
				}
				test::require(e.ready == (ready.count(name) != 0 ? 1 : 0), "ready flag of " + name + ": " + std::to_string(e.ready));
			}
			test::require(sim.playerSquads().faction != 0, "the scenario has a player faction before new_world");
		}

		// ---- NEW_GAME with the scenario's faction in place: has_faction ----
		{
			const Output out = admin.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(admin, rig, "Wanderer"), 0));
			test::require(out.results.size() == 1 && out.results[0].result == 1 && admin.text(out.results[0].reason) == "has_faction",
				"NEW_GAME while a player faction exists: has_faction");
		}

		// ---- new_world: admin only ----
		{
			requireReason(other.order(84, Buf{}), "not_allowed", "non-admin new_world");
			test::require(sim.playerSquads().faction != 0, "a refused new_world changed nothing");
			Client malformed(sim, context, true, context.dataHash);
			test::require(malformed.refused(ks::kOrder, Buf{}.put<std::uint32_t>(1).put<std::uint16_t>(84).put<std::uint8_t>(0)),
				"new_world with a body is malformed");
		}

		std::uint64_t freshHash = 0;
		{
			admin.sendTick();
			const Output out = admin.order(84, Buf{});
			requireReason(out, "", "new_world");
			test::require(out.index(ks::kEvent) >= 0, "the answer is event 9");
			// Event 9 first, then the full resend, then STARTS last.
			test::require(out.all.front().type == ks::kEvent, "event 9 comes first");
			test::require(out.index(ks::kSnapBegin) > 0, "the snapshot follows the event");
			test::require(out.count(ks::kSnapBegin) == 1 && out.count(ks::kSnapEnd) == 1, "one full snapshot");
			test::require(out.starts.size() == 1 && out.all.back().type == ks::kStarts, "STARTS is the last frame of the answer");
			test::require(out.count(ks::kPlayerSquads) == 0, "no PLAYER_SQUADS: no player faction");
			test::require(out.count(ks::kWeather) == 1 && out.count(ks::kClock) == 1 && out.count(ks::kBounds) == 1,
				"weather, clock and bounds are sent again (no tiles: nothing of the player's is near)");
			test::require(sim.playerSquads().faction == 0, "no player faction");
			test::require(sim.combat()->members().empty(), "no characters: the scenario squads are gone");
			test::require(sim.clockTick() == freshTick, "the clock is the fresh world's");
			const Wire33& w = out.starts[0];
			int ready = 0;
			for (const Wire33::Entry& e : w.entries)
				ready += e.ready;
			test::require(w.entries.size() == 13 && ready == 4, "STARTS after new_world: 13 start-offs, the 4 Hub ones ready");
			freshHash = sim.stateHash();
			// The same fresh world every time.
			requireReason(admin.order(84, Buf{}), "", "new_world again");
			test::require(sim.stateHash() == freshHash, "new_world twice gives the same world (same hash)");
			// The other sessions get the resend at their next tick.
			const Output o = other.sendTick();
			test::require(o.count(ks::kSnapBegin) == 1 && o.starts.size() == 1, "another session gets the full resend and STARTS after new_world");
		}

		// ---- rejects, each reason (nothing changes) ----
		const std::uint32_t wanderer = startId(admin, rig, "Wanderer");
		const auto expectReject = [&](const Buf& body, const std::string& reason, const std::string& what)
		{
			const std::uint64_t hash = sim.stateHash();
			const Output out = admin.frame(ks::kNewGame, body);
			test::require(out.results.size() == 1 && out.results[0].result == 1, what + ": a rejection");
			test::require(admin.text(out.results[0].reason) == reason, what + ": reason '" + admin.text(out.results[0].reason) + "', wanted '" + reason + "'");
			test::require(sim.stateHash() == hash && sim.pendingCommands() == 0, what + ": nothing changed, nothing queued");
		};
		expectReject(Client::newGameBody("Pilgrims", 0, 0), "unknown_startoff", "startoff strid 0");
		expectReject(Client::newGameBody("Pilgrims", 99999, 0), "unknown_startoff", "startoff strid past the table");
		{
			// A string the session knows that is not a start-off (a weather or a biome).
			std::uint32_t other = 1;
			while (other <= admin.session->strings().size() && (admin.session->strings().entry(other).kind != 1 || admin.text(other).empty()
				|| std::any_of(rig.startSid.begin(), rig.startSid.end(), [&](const auto& e) { return e.second == admin.text(other); })))
				++other;
			test::require(other <= admin.session->strings().size(), "the session knows a string that is not a start-off");
			expectReject(Client::newGameBody("Pilgrims", other, 0), "unknown_startoff", "a strid that is not a start-off");
		}
		expectReject(Client::newGameBody("", wanderer, 0), "bad_name", "empty name");
		expectReject(Client::newGameBody("   ", wanderer, 0), "bad_name", "blank name");
		expectReject(Client::newGameBody(std::string(33, 'a'), wanderer, 0), "bad_name", "33 characters");
		expectReject(Client::newGameBody("a\nb", wanderer, 0), "bad_name", "a control character");
		expectReject(Client::newGameBody("a\xC3\x28", wanderer, 0), "bad_name", "invalid UTF-8");
		expectReject(Client::newGameBody("Pilgrims", wanderer, 2), "squad_size", "two Specs for a one-member start-off");
		expectReject(Client::newGameBody("Pilgrims", wanderer, 1), "unknown_race", "a shape-only Spec (race strid 0) is no race");
		expectReject(Client::newGameBody("Pilgrims", startId(admin, rig, "Rock Bottom"), 0), "not_ready", "Rock Bottom (its zone has no navmesh)");
		expectReject(Client::newGameBody("Pilgrims", startId(admin, rig, "The Slaves"), 0), "not_ready", "The Slaves (Rebirth has no navmesh)");
		expectReject(Client::newGameBody(std::string(32, 'a'), wanderer, 3), "squad_size", "a 32-character name is fine, the count is not");
		// Kicks: frame-shape faults close the session.
		// "trailing garbage": since task BT a trailing block is the optional options section (u8 count, then that many
		// {str key, f32 value} pairs), so a lone count byte is an incomplete one, not a stray byte.
		for (const char* what : {"n past 16", "a string past 256", "slider count past 64", "trailing garbage", "truncated", "second pending"})
		{
			Client c(sim, context, true, context.dataHash);
			Buf body = Client::newGameBody("Pilgrims", wanderer, 0);
			const std::string w = what;
			if (w == "n past 16")
				body = Client::newGameBody("Pilgrims", wanderer, 17);
			else if (w == "a string past 256")
				body = Client::newGameBody(std::string(257, 'a'), wanderer, 0);
			else if (w == "slider count past 64")
			{
				body = Buf{};
				body.put<std::uint32_t>(1).str("Pilgrims").put<std::uint32_t>(wanderer).put<std::uint8_t>(1);
				body.put<std::uint32_t>(0).put<std::uint8_t>(0).put<std::uint32_t>(0).put<std::uint32_t>(0).put<std::uint32_t>(0).put<std::uint32_t>(0);
				body.put<float>(0.5f).put<std::uint8_t>(65);
			} else if (w == "trailing garbage") {
				body.put<std::uint8_t>(65); // an options count with no entries behind it
			} else if (w == "truncated") {
				body.bytes.pop_back();
			} else {
				body = Client::newGameBody("Pilgrims", startId(c, rig, "Wanderer"), 0); // c's own string table
				test::require(!c.refused(ks::kNewGame, body) && c.refused(ks::kNewGame, body), "a second NEW_GAME while one is pending is a protocol error");
				sim.run(1);
				c.sendTick(); // takes the reply, so the next sections start clean
				// That NEW_GAME made a faction: back to the fresh world.
				requireReason(admin.order(84, Buf{}), "", "new_world after the pending test");
				continue;
			}
			test::require(c.refused(ks::kNewGame, body), std::string("NEW_GAME ") + what + " is a protocol error");
		}
		test::require(sim.playerSquads().faction == 0, "no reject or kick made a faction");

		// ---- the optional trailing options block (ADVANCED_OPTIONS_STATIC.md "Ours", task BT) ----
		{
			// An unknown key and an out-of-range value are protocol errors, like a trailing byte.
			{
				Client c(sim, context, true, context.dataHash);
				Buf body = Client::newGameBody("Pilgrims", startId(c, rig, "Wanderer"), 0);
				body.put<std::uint8_t>(1).str("bs").put<float>(1.0f);
				test::require(c.refused(ks::kNewGame, body), "NEW_GAME option 'bs' is a protocol error");
			}
			{
				Client c(sim, context, true, context.dataHash);
				Buf body = Client::newGameBody("Pilgrims", startId(c, rig, "Wanderer"), 0);
				body.put<std::uint8_t>(1).str("cod").put<float>(4.5f);
				test::require(c.refused(ks::kNewGame, body), "NEW_GAME option cod 4.5 is a protocol error");
			}
			test::require(sim.worldOptions() == nullptr, "a refused option made the singleton");
			// cod + ht ride the start: the world carries them, the arena takes them, and the start's own defaults stay for gdm.
			Client c(sim, context, true, context.dataHash);
			Buf body = Client::newGameBody("Pilgrims", startId(c, rig, "Wanderer"), 0);
			body.put<std::uint8_t>(2).str("cod").put<float>(1.5f).str("ht").put<float>(2.0f);
			c.frame(ks::kNewGame, body);
			const Output started = c.tick(sim);
			test::require(started.results.size() == 1 && started.results[0].result == 0, "the options NEW_GAME started");
			const state::WorldOptions* options = sim.worldOptions();
			test::require(options != nullptr && options->chanceOfDeath == 1.5f && options->hungerTime == 2.0f
				&& options->damageScale == 1.0f, "NEW_GAME's options were saved, and only those sent");
			const CombatSettings& live = sim.combat()->rules().settings();
			test::require(live.chanceOfDeath == 1.5f && live.hungerTime == 2.0f && live.damageScale == 1.0f,
				"the arena took NEW_GAME's options");
			// The next world starts from the options it carries, not the last world's: new_world clears the singleton.
			requireReason(admin.order(84, Buf{}), "", "new_world after the options start");
			test::require(sim.worldOptions() == nullptr, "new_world kept the previous world's options");
			test::require(sim.combat()->rules().settings().chanceOfDeath == CombatSettings{}.chanceOfDeath,
				"new_world kept the previous world's cod in the arena");
			std::cout << "NEW_GAME options: cod 1.5, ht 2 saved and applied; new_world cleared them\n";
		}

		// ---- every ready start-off at n = 0: reply 62, then PLAYER_SQUADS, members controllable ----
		struct Expect
		{
			std::string name;
			std::size_t members;
			std::int64_t money;
		};
		for (const Expect& want : {Expect{"Wanderer", 1, 1000}, Expect{"Nobodies", 5, 0}, Expect{"The Holy Sword", 1, 100}, Expect{"Son of a Captain", 1, 0}})
		{
			requireReason(admin.order(84, Buf{}), "", "new_world before " + want.name);
			Client c(sim, context, true, context.dataHash);
			const Output queued = c.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(c, rig, want.name), 0));
			test::require(queued.results.empty() && sim.pendingCommands() == 1, want.name + ": accepted, queued for the next tick, no reply yet");
			const std::uint64_t tickBefore = sim.clockTick();
			const Output out = c.tick(sim);
			test::require(sim.clockTick() == tickBefore + 1 && sim.pendingCommands() == 0, want.name + ": applied at tick + 1");
			test::require(out.results.size() == 1 && out.results[0].result == 0, want.name + ": reply 62 ok");
			const Wire62& reply = out.results[0];
			test::require(reply.members.size() == want.members, want.name + ": " + std::to_string(reply.members.size()) + " members");
			{
				const state::Fighter* leader = sim.combat()->find(reply.members.front());
				test::require(leader != nullptr && reply.pos[0] == static_cast<float>(leader->x) && reply.pos[1] == static_cast<float>(leader->y)
					&& reply.pos[2] == static_cast<float>(leader->z), want.name + ": reply 62 carries the leader's position");
			}
			const PlayerSquadsView view = sim.playerSquads();
			test::require(view.faction == reply.faction && view.factionName == "Pilgrims" && view.factionSid == rig.namelessSid, want.name + ": the faction");
			test::require(view.squads.size() == 1 && view.squads[0].members == reply.members && view.squads[0].name == "Pilgrims_1",
				want.name + ": one player squad with the members, leader first");
			test::require(out.squads.size() == 1 && out.squads[0].faction == reply.faction && out.squads[0].squads.size() == 1
				&& out.squads[0].squads[0] == reply.members && out.squads[0].names[0] == "Pilgrims_1", want.name + ": PLAYER_SQUADS in the same write");
			test::require(out.index(ks::kNewGameResult) < out.index(ks::kPlayerSquads), want.name + ": the reply comes before PLAYER_SQUADS");
			// UNLOCKS: the research the start-off lists (else `_Default Start`), and the buildings it enables, sent when the set changed.
			{
				const std::vector<std::string> wantResearch = startResearch(*rig.gd, rig.catalog->find(rig.startSid.at(want.name))->research);
				test::require(out.unlocks.size() == 1, want.name + ": one UNLOCKS frame when the new game gave the faction its research");
				test::require(c.texts(out.unlocks[0].research) == wantResearch && sim.completedResearch() == wantResearch, want.name + ": the completed research");
				test::require(c.texts(out.unlocks[0].buildings) == sim.unlockedBuildings(wantResearch), want.name + ": the unlocked buildings");
				test::require(out.unlocks[0].tick == sim.clockTick() && out.index(ks::kPlayerSquads) < out.index(ks::kUnlocks), want.name + ": UNLOCKS follows PLAYER_SQUADS");
				test::require(c.sendTick().unlocks.empty(), want.name + ": nothing changed, no second UNLOCKS");
			}
			// DISCOVERED: the list starts empty and the new game's first check, at the start position, fills it (frame 37 in the same write).
			{
				const std::vector<Simulation::DiscoveredPlace> places = sim.discoveredPlaces();
				test::require(!places.empty() && out.count(ks::kDiscovered) == 1, want.name + ": the start position discovers what is around, sent as DISCOVERED");
				test::require(std::any_of(places.begin(), places.end(), [](const Simulation::DiscoveredPlace& p) { return p.visited; }), want.name + ": the Hub is visited");
			}
			test::require(out.count(ks::kAppearance) >= static_cast<int>(want.members) && out.count(ks::kDynamic) >= static_cast<int>(want.members),
				want.name + ": the members are in the stream");
			// State: money, start-off, relations, allegiance, control.
			bool found = false;
			sim.world().ecs().each([&](const state::Id& id, const state::PlayerFaction& faction)
			{
				found = true;
				test::require(id.value == reply.faction && faction.money == want.money && faction.startoff == rig.startSid.at(want.name),
					want.name + ": money " + std::to_string(faction.money) + " and the start-off recorded");
			});
			test::require(found, want.name + ": a PlayerFaction component");
			for (const std::uint64_t id : reply.members)
			{
				const state::Fighter* f = sim.combat()->find(id);
				test::require(f != nullptr && f->playerControlled && f->team == 0 && !f->dead, want.name + ": a controllable member");
				bool allegiance = false;
				sim.world().ecs().each([&](const state::Id& sid, const state::Allegiance& a)
				{
					if (sid.value == id)
						allegiance = a.factionSid == rig.namelessSid;
				});
				test::require(allegiance, want.name + ": Allegiance is the player faction's template");
				test::require(std::hypot(f->x - ks::kHubX, f->z - ks::kHubZ) < 400.0, want.name + ": placed at the Hub");
			}
			if (want.name == "The Holy Sword")
			{
				// faction relations: The Holy Nation -100 and Imperial Lords -100 (a2.2), both directions.
				const auto relation = [&](const std::string& sid, bool toPlayer) -> double
				{
					flecs::entity player{}, other{};
					sim.world().ecs().each([&](flecs::entity e, const state::FactionState& s)
					{
						if (s.templateSid == rig.namelessSid)
							player = e;
						if (s.templateSid == sid)
							other = e;
					});
					const state::Relation* r = toPlayer ? other.get<state::Relation>(player) : player.get<state::Relation>(other);
					test::require(r != nullptr, "a relation pair exists with " + sid);
					return r->value;
				};
				for (const char* sid : {"1083-gamedata.base", "1087-gamedata.base"})
					test::require(relation(sid, true) == -100.0 && relation(sid, false) == -100.0, std::string("Holy Sword: relation with ") + sid + " is -100 both ways");
			}
			// A second NEW_GAME: has_faction.
			const Output again = c.frame(ks::kNewGame, Client::newGameBody("Again", startId(c, rig, want.name), 0));
			test::require(again.results.size() == 1 && again.results[0].result == 1 && c.text(again.results[0].reason) == "has_faction", want.name + ": a second NEW_GAME is has_faction");
			// Controllable: a move order for the leader is applied, and the leader walks.
			{
				const state::Fighter before = *sim.combat()->find(reply.members.front());
				Buf move;
				move.put<std::uint16_t>(1).put<std::uint64_t>(reply.members.front()).put<float>(static_cast<float>(before.x + 20.0))
					.put<float>(static_cast<float>(before.z)).put<float>(0.0f).put<std::uint8_t>(0);
				const Output m = c.order(1, move);
				requireReason(m, "", want.name + ": move_to");
				for (int i = 0; i < 60; ++i)
					sim.run(1);
				const state::Fighter& after = *sim.combat()->find(reply.members.front());
				test::require(std::hypot(after.x - before.x, after.z - before.z) > 5.0, want.name + ": the leader walked");
			}
		}

		// ---- the logged command replays to the same world ----
		{
			requireReason(admin.order(84, Buf{}), "", "new_world for the replay");
			Client c(sim, context, true, context.dataHash);
			c.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(c, rig, "Nobodies"), 0));
			Command logged;
			for (const ks::OrderLogEntry& entry : context.orderLog)
			{
				if (entry.kind == ks::kNewGame)
					logged = entry.command;
			}
			test::require(logged.type == CommandType::NewGame && logged.tick == sim.clockTick() + 1, "the NewGame command is in the order log, stamped for the next tick");
			sim.run(40);
			std::cout << "FACTION_RELATIONS rows in a new game (Nobodies): " << sim.factionRelations().size() << "\n";
			const std::uint64_t first = sim.stateHash();
			requireReason(admin.order(84, Buf{}), "", "new_world again");
			sim.enqueue(logged); // the log entry alone, no session
			sim.run(40);
			test::require(sim.stateHash() == first, "replaying the logged command gives the same hash");
			// And pending in a save: the command survives save and load.
			requireReason(admin.order(84, Buf{}), "", "new_world for the pending test");
			sim.enqueue(logged);
			std::ostringstream bytes;
			sim.save(bytes);
			sim.run(1);
			const std::uint64_t applied = sim.stateHash();
			std::istringstream in(bytes.str());
			sim.reload(in);
			sim.run(1);
			test::require(sim.stateHash() == applied, "a NewGame command pending in a save applies after the load");
		}

		// ---- save -> load -> continue after a new game ----
		{
			requireReason(admin.order(84, Buf{}), "", "new_world for the save test");
			Client c(sim, context, true, context.dataHash);
			c.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(c, rig, "Nobodies"), 0));
			sim.run(30);
			c.sendTick();
			const std::uint64_t savedTick = sim.clockTick();
			const std::uint64_t savedHash = sim.stateHash();
			requireReason(c.order(80, Buf{}.str("ng")), "", "save ng");
			sim.run(120);
			const std::uint64_t laterTick = sim.clockTick();
			const std::uint64_t laterHash = sim.stateHash();
			test::require(laterHash != savedHash, "the world moved on");
			const Output out = c.order(81, Buf{}.str("ng").put<std::uint8_t>(0));
			requireReason(out, "", "load ng");
			test::require(sim.clockTick() == savedTick && sim.stateHash() == savedHash, "the load restored the saved tick and hash");
			test::require(out.count(ks::kPlayerSquads) == 1 && out.squads[0].squads.size() == 1 && out.squads[0].squads[0].size() == 5, "PLAYER_SQUADS (5 members) in the resend");
			sim.run(laterTick - sim.clockTick());
			test::require(sim.stateHash() == laterHash, "save, load and continue after a new game gives the same hash");
			// The metadata block carries the faction, its money and the members.
			const PlayerSquadsView view = sim.playerSquads();
			const SaveInfo info = describeState(sim.world().ecs());
			test::require(info.factionName == "Pilgrims" && info.members == 5 && info.money == 0 && view.factionCount == 5, "the save metadata: faction, members");
		}


		// ---- character creation: NEW_GAME with Specs (n = the start-off's member count) ----
		{
			const CreatorCatalog creator = CreatorCatalog::build(*rig.gd, KENSHI_DATA);
			test::require(creator.races().size() == 7 && creator.stanceCount() == 8, "the creator offers 7 races and 8 idle stances (Skeleton, Shek, Greenlander, Scorchlander, three Hive)");
			const auto race = [&](const std::string& name) -> const CreatorRace&
			{
				for (const CreatorRace& r : creator.races())
				{
					if (r.name == name)
						return r;
				}
				throw test::Failure("no creator race " + name);
			};
			// A Spec the creator could make: the race's last head, a hair and beard when it has some, its last hair colour, three sliders at
			// their min / max / mid, a skin and a stance.
			const auto makeSpec = [&](const std::string& raceName, bool female, int k) -> CharacterSpec
			{
				const CreatorRace& r = race(raceName);
				CharacterSpec s;
				s.race = r.sid;
				s.female = female ? 1 : 0;
				s.head = r.heads[s.female].empty() ? std::string() : r.heads[s.female].back();
				s.hair = r.hairs.empty() ? std::string() : r.hairs[static_cast<std::size_t>(2 + k) % r.hairs.size()];
				s.beard = female || r.beards.empty() ? std::string() : r.beards[static_cast<std::size_t>(1 + k) % r.beards.size()];
				s.hairColour = r.hairColours.empty() ? std::string() : r.hairColours.back();
				s.skin = 0.25f + 0.1f * static_cast<float>(k);
				const std::vector<EditorSlider>& sliders = r.sliders[s.female];
				test::require(sliders.size() > 3, raceName + " has sliders");
				s.sliders.emplace_back(sliders[0].name, std::min(sliders[0].min, sliders[0].max));
				s.sliders.emplace_back(sliders[1].name, std::max(sliders[1].min, sliders[1].max));
				s.sliders.emplace_back(sliders[2].name, (sliders[2].min + sliders[2].max) * 0.5f);
				s.name = "Hero " + std::to_string(k);
				s.stance = 1 + k % creator.stanceCount();
				return s;
			};
			const auto specsFor = [&](std::size_t count)
			{
				const std::vector<std::pair<std::string, bool>> picks = {{"Greenlander", false}, {"Shek", true}, {"Skeleton", false}, {"Hive Prince", true}, {"Scorchlander", false},
					{"Greenlander", true}};
				std::vector<CharacterSpec> specs;
				for (std::size_t k = 0; k < count; ++k)
					specs.push_back(makeSpec(picks[k].first, picks[k].second, static_cast<int>(k)));
				return specs;
			};

			requireReason(admin.order(84, Buf{}), "", "new_world for character creation");
			Client c(sim, context, true, context.dataHash, true);

			// ---- STARTS carries the force race list and the seeded squad ----
			{
				const Wire33& w = c.first.starts.at(0);
				const std::map<std::string, std::size_t> required = {{"Wanderer", 1}, {"Nobodies", 5}, {"The Holy Sword", 1}, {"Son of a Captain", 1}};
				for (const Wire33::Entry& e : w.entries)
				{
					const StartOff* start = rig.catalog->find(c.text(e.id));
					test::require(start != nullptr, "a STARTS entry is a start-off");
					const std::vector<StartMember> squad = sim.startSquad(*start);
					test::require(e.members.size() == squad.size() && !e.members.empty(), start->name + ": m is the squad size");
					for (std::size_t i = 0; i < squad.size(); ++i)
						test::require(c.text(e.members[i].race) == squad[i].race && e.members[i].female == squad[i].female && e.members[i].name == squad[i].name
							&& (e.members[i].flags & 1) == (squad[i].editable ? 1 : 0), start->name + ": member " + std::to_string(i) + " is the sim's expansion");
					if (required.count(start->name) != 0)
						test::require(e.members.size() == required.at(start->name), start->name + ": m equals the Spec n it requires");
					std::vector<std::string> force;
					for (const std::uint32_t id : e.forceRace)
						force.push_back(c.text(id));
					const std::vector<std::string> expect = start->forceRace.empty() ? std::vector<std::string>() : creator.allowedRaces(start->forceRace);
					test::require(force == expect, start->name + ": the force race list");
					// the expansion is a pure function of the seed and the sid
					const std::vector<StartMember> again = sim.startSquad(*start);
					for (std::size_t i = 0; i < squad.size(); ++i)
						test::require(again[i].character == squad[i].character && again[i].race == squad[i].race && again[i].rng == squad[i].rng, start->name + ": the same draw twice");
					if (start->name == "The Slaves")
						test::require(e.forceRace.size() == 6, "The Slaves force six races");
					if (start->name == "The Hive Exile")
						test::require(e.forceRace.size() == 3, "The Hive Exile forces three races");
					if (start->name == "Wanderer")
						test::require(e.forceRace.empty(), "Wanderer forces nothing");
				}
				// NEW_GAME n = 0 builds exactly the squad STARTS showed
				Client z(sim, context, true, context.dataHash);
				z.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(z, rig, "Nobodies"), 0));
				const Output made = z.tick(sim);
				test::require(made.results.size() == 1 && made.results[0].result == 0, "n = 0 Nobodies for the draw check");
				const Wire33::Entry* shown = nullptr;
				for (const Wire33::Entry& e : w.entries)
					shown = c.text(e.id) == rig.startSid.at("Nobodies") ? &e : shown;
				test::require(shown != nullptr && shown->members.size() == made.results[0].members.size(), "the same member count");
				for (std::size_t i = 0; i < shown->members.size(); ++i)
				{
					const state::Fighter* f = sim.combat()->find(made.results[0].members[i]);
					test::require(f != nullptr && f->raceSid == c.text(shown->members[i].race) && f->name == shown->members[i].name && f->look.female == shown->members[i].female,
						"the built member " + std::to_string(i) + " is what STARTS showed (race, name, sex)");
					// the outfit: character, worn armour (equip order) and weapon (kenshi-k16.5.4)
					const Wire33::Member& shownMember = shown->members[i];
					std::vector<std::string> worn;
					for (const std::uint32_t id : shownMember.armour)
						worn.push_back(c.text(id));
					std::vector<std::string> built;
					for (const state::FighterArmour& piece : f->armour)
						built.push_back(piece.sid);
					test::require(worn == built && (shownMember.weapon == 0 ? std::string() : c.text(shownMember.weapon)) == f->weapon.sid
						&& c.text(shownMember.character) == sim.startSquad(*rig.catalog->find(rig.startSid.at("Nobodies")))[i].character,
						"the built member " + std::to_string(i) + " wears and carries what STARTS listed");
				}
				requireReason(admin.order(84, Buf{}), "", "new_world after the draw check");
				c.sendTick(); // takes the resend of the reset, so a NEW_GAME queued next is not dropped with it
			}
			const std::uint32_t nobodies = startId(c, rig, "Nobodies");

			// The string table carries the creator's sids from the STARTS write on.
			for (const std::string& sid : creator.sids())
				test::require(c.strid(sid) != 0, "the creator's sid " + sid + " is in the string table");

			// ---- rejections: one reason each, nothing changes ----
			const std::uint32_t wanderer = startId(c, rig, "Wanderer");
			const auto reject = [&](const std::vector<CharacterSpec>& specs, const std::string& reason, const std::string& what, std::uint32_t startoff = 0)
			{
				const std::uint64_t hash = sim.stateHash();
				const Output out = c.frame(ks::kNewGame, c.specBody("Pilgrims", startoff == 0 ? wanderer : startoff, specs));
				test::require(out.results.size() == 1 && out.results[0].result == 1, what + ": a rejection");
				test::require(c.text(out.results[0].reason) == reason, what + ": reason '" + c.text(out.results[0].reason) + "', wanted '" + reason + "'");
				test::require(sim.stateHash() == hash && sim.pendingCommands() == 0, what + ": nothing changed, nothing queued");
			};
			const CharacterSpec good = makeSpec("Greenlander", false, 0);
			{
				const CreatorRace& green = race("Greenlander");
				const CreatorRace& shek = race("Shek");
				CharacterSpec s = good;
				// race
				s.race = "";
				reject({s}, "unknown_race", "race strid 0");
				s.race = std::string(kBadSid);
				reject({s}, "unknown_race", "race strid past the table");
				s.race = rig.startSid.at("Wanderer");
				reject({s}, "unknown_race", "a sid that is no RACE");
				// sex
				s = good;
				s.female = 2;
				reject({s}, "sex_not_available", "female byte 2");
				// head
				s = good;
				s.head = shek.heads[0].front();
				reject({s}, "bad_head", "another race's head");
				s.head = "";
				reject({s}, "bad_head", "no head for a race that has heads");
				s.head = green.heads[1].back();
				test::require(std::find(green.heads[0].begin(), green.heads[0].end(), s.head) == green.heads[0].end(), "a Greenlander female head is not on the male list");
				reject({s}, "bad_head", "a female head on a male");
				// hair
				s = good;
				s.hair = green.beards.front();
				reject({s}, "bad_hair", "a beard as hair");
				s.hair = std::string(kBadSid);
				reject({s}, "bad_hair", "hair strid past the table");
				s = makeSpec("Shek", true, 1);
				s.hair = green.hairs.front();
				reject({s}, "bad_hair", "hair for a race with none");
				// beard
				s = good;
				s.beard = green.hairs.front();
				reject({s}, "bad_beard", "hair as a beard");
				s = makeSpec("Greenlander", true, 0);
				s.beard = green.beards.front();
				reject({s}, "bad_beard", "a beard on a female (the creator has no beard picker for one)");
				// hair colour
				s = good;
				s.hairColour = shek.hairColours.front();
				test::require(std::find(green.hairColours.begin(), green.hairColours.end(), s.hairColour) == green.hairColours.end(), "a Shek colour is not a Greenlander one");
				reject({s}, "bad_hair_colour", "another race's colour");
				s.hairColour = "";
				reject({s}, "bad_hair_colour", "no colour for a race that has colours");
				s = makeSpec("Skeleton", false, 0);
				s.hairColour = green.hairColours.front();
				reject({s}, "bad_hair_colour", "a colour for a race with none");
				// sliders
				s = good;
				s.sliders.emplace_back("No such slider", 1.0f);
				reject({s}, "bad_slider", "an unknown slider");
				s = good;
				s.sliders.push_back(s.sliders.front());
				reject({s}, "bad_slider", "a slider twice");
				{
					// A female-only slider on a male.
					std::string only;
					for (const EditorSlider& f : green.sliders[1])
					{
						const bool male = std::any_of(green.sliders[0].begin(), green.sliders[0].end(), [&](const EditorSlider& m) { return m.name == f.name; });
						if (!male)
							only = f.name;
					}
					test::require(!only.empty(), "the Greenlander female editor has a slider the male one lacks");
					s = good;
					s.sliders.emplace_back(only, 1.0f);
					reject({s}, "bad_slider", "a slider of the other sex");
				}
				s = good;
				s.sliders[0].second = std::max(green.sliders[0][0].min, green.sliders[0][0].max) + 1.0f;
				reject({s}, "slider_range", "a slider above its max (rejected, not clamped)");
				s.sliders[0].second = std::min(green.sliders[0][0].min, green.sliders[0][0].max) - 1.0f;
				reject({s}, "slider_range", "a slider below its min");
				s = good;
				s.skin = 1.5f;
				reject({s}, "slider_range", "skin above 1");
				s.skin = -0.1f;
				reject({s}, "slider_range", "skin below 0");
				s = good;
				s.stance = 0;
				reject({s}, "slider_range", "stance 0");
				s.stance = creator.stanceCount() + 1;
				reject({s}, "slider_range", "stance past the list");
				// name
				s = good;
				s.name = "";
				reject({s}, "empty_name", "empty character name");
				s.name = "  ";
				reject({s}, "empty_name", "blank character name");
				s.name = "a\nb";
				reject({s}, "bad_character_name", "a control character in the character name");
				s.name = std::string(33, 'a');
				reject({s}, "bad_character_name", "33 characters");
				// the first fault in Spec order decides, and a later Spec's fault is found
				{
					std::vector<CharacterSpec> two = specsFor(5);
					two[3].name = "";
					reject(two, "empty_name", "a fault in the fourth Spec", nobodies);
					two = specsFor(5);
					two[0].name = "";
					two[4].race = "";
					reject(two, "empty_name", "the first Spec's fault comes first", nobodies);
				}
				// the count: squad_size is unchanged and comes before the Specs are read
				reject({good, good}, "squad_size", "two Specs for a one-member start-off");
				{
					std::vector<CharacterSpec> bad = specsFor(4);
					bad[0].race = "";
					reject(bad, "squad_size", "four Specs for five members (squad_size, before the Specs)", nobodies);
				}
				// a frame-shape fault still kicks: a NaN slider value
				{
					Client k(sim, context, true, context.dataHash);
					Buf body = k.specBody("Pilgrims", startId(k, rig, "Wanderer"), {good});
					const float nan = std::nanf("");
					std::memcpy(&body.bytes[body.bytes.size() - 4 - 2 - good.name.size() - 4], &nan, 4); // the last slider's f32 (before name and stance)
					test::require(k.refused(ks::kNewGame, body), "a NaN slider value is a protocol error");
				}
			}
			test::require(sim.playerSquads().faction == 0, "no rejection made a faction");

			// The catalog on its own: reasons the wire cannot reach with this data.
			{
				const CharacterSpec s = good;
				test::require(creator.check({}, {s}).empty(), "the good Spec passes");
				// force race: only the listed races, an empty result falls back to all.
				const std::string shekSid = race("Shek").sid;
				test::require(creator.check({shekSid}, {s}) == "race_forced", "force race [Shek] excludes a Greenlander");
				test::require(creator.check({shekSid}, {makeSpec("Shek", false, 0)}).empty(), "force race [Shek] allows a Shek");
				test::require(creator.check({"no-such-race"}, {s}).empty(), "a force race list with no valid race allows every creator race (the exe's fallback)");
				test::require(creator.allowedRaces({race("Greenlander").sid, race("Scorchlander").sid}).size() == 2, "Holy Nation Citizen allows two races");
				// a non-editable member takes an echo with an empty look (renaming allowed); anything else is spec_mismatch
				{
					const std::vector<SquadSlot> slots = {{race("Greenlander").sid, 1, false}};
					CharacterSpec echo;
					echo.race = race("Greenlander").sid;
					echo.female = 1;
					echo.name = "Rex";
					test::require(creator.check({}, {echo}, slots).empty(), "a non-editable echo (race, female, empty look, a new name) is accepted");
					CharacterSpec wrong = echo;
					wrong.female = 0;
					test::require(creator.check({}, {wrong}, slots) == "spec_mismatch", "a different sex for a non-editable member is spec_mismatch");
					wrong = echo;
					wrong.race = race("Shek").sid;
					test::require(creator.check({}, {wrong}, slots) == "spec_mismatch", "a different race is spec_mismatch");
					wrong = echo;
					wrong.hair = race("Greenlander").hairs.front();
					test::require(creator.check({}, {wrong}, slots) == "spec_mismatch", "a look on a non-editable member is spec_mismatch");
					wrong = echo;
					wrong.stance = 3;
					test::require(creator.check({}, {wrong}, slots) == "spec_mismatch", "a stance on a non-editable member is spec_mismatch");
					wrong = echo;
					wrong.name = "";
					test::require(creator.check({}, {wrong}, slots) == "empty_name", "a non-editable member still needs a name");
				}
				// a RACE record the creator does not list
				std::string unplayable;
				for (const kdata::Record* r : rig.gd->byType(kworld::itemTypeByName("RACE")))
				{
					if (creator.find(r->stringID) == nullptr)
						unplayable = r->stringID;
				}
				test::require(!unplayable.empty(), "the data has a RACE the creator does not list");
				CharacterSpec u = s;
				u.race = unplayable;
				test::require(creator.check({}, {u}) == "race_not_playable", "a real RACE that is not in the creator is race_not_playable");
				// a one-sex race
				CreatorRace one = race("Greenlander");
				one.sliders[1].clear();
				const CreatorCatalog single = CreatorCatalog::from({one}, {one.sid}, 8);
				test::require(single.check({}, {good}).empty(), "the one-sex race takes its male");
				test::require(single.check({}, {makeSpec("Greenlander", true, 0)}) == "sex_not_available", "female on a race with no female editor is sex_not_available");
				// the forced list in the wire path: The Hive Exile / The Slaves are not ready, so their Specs are never read
				test::require(rig.catalog->find(rig.startSid.at("The Hive Exile"))->forceRace.size() == 3, "The Hive Exile forces three races");
				test::require(sim.checkNewGame(rig.startSid.at("The Hive Exile"), "Pilgrims", {good}) == "squad_size" || sim.checkNewGame(rig.startSid.at("The Hive Exile"), "Pilgrims", {good}) == "not_ready",
					"a start that is not ready answers before its Specs are read");
				// the encoding the logged command carries
				std::vector<CharacterSpec> back;
				const std::vector<CharacterSpec> five = specsFor(5);
				test::require(decodeSpecs(encodeSpecs(five), back) && back.size() == 5, "the Spec encoding round-trips");
				for (std::size_t k = 0; k < 5; ++k)
					test::require(back[k].race == five[k].race && back[k].name == five[k].name && back[k].sliders == five[k].sliders && back[k].skin == five[k].skin
						&& back[k].stance == five[k].stance && back[k].hair == five[k].hair && back[k].beard == five[k].beard, "Spec " + std::to_string(k) + " survives the encoding");
				test::require(!decodeSpecs(encodeSpecs(five).substr(0, 20), back), "a cut encoding is refused");
			}

			// ---- a valid Spec per member (Nobodies: 5): race, name, look, stance in the sim and in CHAR_APPEARANCE ----
			const std::vector<CharacterSpec> specs = specsFor(5);
			const Output queued = c.frame(ks::kNewGame, c.specBody("Pilgrims", nobodies, specs));
			test::require(queued.results.empty() && sim.pendingCommands() == 1, "five Specs for Nobodies: accepted, queued");
			const Output out = c.tick(sim);
			test::require(out.results.size() == 1 && out.results[0].result == 0 && out.results[0].members.size() == 5, "reply 62 ok with five members (" + (out.results.empty() ? std::string("no reply") : c.text(out.results[0].reason)) + ")");
			const std::vector<std::uint64_t>& members = out.results[0].members;
			const auto checkLook = [&](const std::string& when)
			{
				for (std::size_t k = 0; k < 5; ++k)
				{
					const CharacterSpec& s = specs[k];
					const state::Fighter* f = sim.combat()->find(members[k]);
					test::require(f != nullptr, when + ": member " + std::to_string(k));
					test::require(f->raceSid == s.race && f->name == s.name && f->look.custom && f->look.female == s.female && f->look.hair == s.hair
						&& f->look.beard == s.beard && f->look.hairColour == s.hairColour && f->look.stance == s.stance
						&& f->look.sliders.size() == s.sliders.size(), when + ": the sim's member " + std::to_string(k) + " has the Spec's race, name and look");
					const auto found = c.dec.chars.find(members[k]);
					test::require(found != c.dec.chars.end() && found->second.hasAppearance, when + ": CHAR_APPEARANCE of member " + std::to_string(k));
					const ::simlink::Appearance& a = found->second.look;
					const CreatorRace& r = *creator.find(s.race);
					const std::vector<std::string>& heads = r.heads[s.female];
					const std::size_t head = static_cast<std::size_t>(std::find(heads.begin(), heads.end(), s.head) - heads.begin());
					test::require(c.dec.str(a.race) == s.race && a.sex == s.female && a.head == head && c.dec.str(a.hair) == s.hair && c.dec.str(a.beard) == s.beard
						&& c.dec.str(a.hairColour) == s.hairColour && a.skin == s.skin && c.dec.str(a.name) == s.name, when + ": the wire look of member " + std::to_string(k));
					test::require(a.body.size() == s.sliders.size() + 1, when + ": the body list holds the sliders and the stance");
					for (std::size_t i = 0; i < s.sliders.size(); ++i)
						test::require(c.dec.str(a.body[i].first) == s.sliders[i].first && a.body[i].second == s.sliders[i].second, when + ": slider " + s.sliders[i].first);
					test::require(c.dec.str(a.body.back().first) == "Idle stance" && a.body.back().second == static_cast<float>(s.stance), when + ": the idle stance rides in the body list");
				}
			};
			checkLook("new game");
			// The race's stats came with the race: a Shek has the Shek's strength, not the template's.
			{
				const state::Fighter* shek = sim.combat()->find(members[1]);
				const state::Fighter* green = sim.combat()->find(members[0]);
				test::require(shek->raceStrength == rig.data->race(race("Shek").sid)->strength && green->raceStrength == rig.data->race(race("Greenlander").sid)->strength,
					"each member has its race's stats");
				test::require(shek->raceStrength != green->raceStrength, "Shek and Greenlander differ in strength");
			}
			// An n = 0 start of the same start-off keeps the template's own look (no creator look).
			sim.run(20);
			const std::uint64_t madeHash = sim.stateHash();

			// ---- replay: the logged command alone gives the same world ----
			{
				Command logged;
				for (const ks::OrderLogEntry& entry : context.orderLog)
				{
					if (entry.kind == ks::kNewGame)
						logged = entry.command;
				}
				test::require(logged.type == CommandType::NewGame && logged.text.find('\n') != logged.text.rfind('\n'), "the logged command carries the Specs");
				requireReason(admin.order(84, Buf{}), "", "new_world for the creation replay");
				sim.enqueue(logged);
				sim.run(1 + 20);
				test::require(sim.stateHash() == madeHash, "replaying the logged character-creation command gives the same hash");
				// Different Specs make a different world: the look is part of the state hash.
				requireReason(admin.order(84, Buf{}), "", "new_world for the plain start");
				Client plain(sim, context, true, context.dataHash);
				plain.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(plain, rig, "Nobodies"), 0));
				sim.run(1 + 20);
				test::require(sim.stateHash() != madeHash, "a start with Specs differs from the plain start");
				// n = 0 members have no creator look
				bool anyCustom = false;
				for (const auto& member : sim.combat()->members())
					anyCustom = anyCustom || member.fighter.look.custom;
				test::require(!anyCustom, "n = 0 leaves the characters without a creator look");
				// back to the made world for the save test
				requireReason(admin.order(84, Buf{}), "", "new_world again");
				sim.enqueue(logged);
				sim.run(1 + 20);
				test::require(sim.stateHash() == madeHash, "the made world again");
			}

			// ---- save -> load -> continue: the hash and the look are equal ----
			{
				c.sendTick();
				const std::uint64_t savedTick = sim.clockTick();
				const std::uint64_t savedHash = sim.stateHash();
				requireReason(c.order(80, Buf{}.str("cc")), "", "save cc");
				sim.run(100);
				const std::uint64_t laterTick = sim.clockTick();
				const std::uint64_t laterHash = sim.stateHash();
				test::require(laterHash != savedHash, "the made world moved on");
				const Output loaded = c.order(81, Buf{}.str("cc").put<std::uint8_t>(0));
				requireReason(loaded, "", "load cc");
				test::require(sim.clockTick() == savedTick && sim.stateHash() == savedHash, "the load restored the made world's hash");
				checkLook("after the load");
				sim.run(laterTick - sim.clockTick());
				test::require(sim.stateHash() == laterHash, "save, load and continue after character creation gives the same hash");
			}
		}

		// ---- UNLOCKS: the data (the start sets), the frame decoded by hand, the empty-set rule, save and load ----
		{
			const std::int32_t researchType = kworld::itemTypeByName("RESEARCH");
			const std::int32_t buildingType = kworld::itemTypeByName("BUILDING");
			const auto nameOf = [&](const std::string& sid) { return rig.gd->find(sid)->name; };
			const auto namesOf = [&](const std::vector<std::string>& sids)
			{
				std::set<std::string> names;
				for (const std::string& sid : sids)
					names.insert(nameOf(sid));
				return names;
			};

			// `_Default Start` is the base game's record 5359, found by its stringID.
			const kdata::Record* def = rig.gd->find(std::string(kDefaultStartResearch));
			test::require(def != nullptr && def->type == researchType && def->name == "_Default Start", "5359-gamedata.base is the RESEARCH _Default Start");
			const std::vector<std::string> defaultSet = startResearch(*rig.gd, {});
			test::require(defaultSet == std::vector<std::string>{std::string(kDefaultStartResearch)}, "no research listed: _Default Start alone");
			test::require(startResearch(*rig.gd, {"no-such-record", rig.namelessSid}) == defaultSet, "unknown and wrong-type refs are dropped, leaving _Default Start");
			const std::set<std::string> defaultBuildings = {"Small Shack", "Small Research Bench", "Stone Mine", "Manual Stone Processor", "Torch Post",
				"Camp Bed", "Campfire", "Well", "Vegetable Farm L", "Vegetable Farm M", "Vegetable Farm S"};
			{
				const std::vector<std::string> sids = sim.unlockedBuildings(defaultSet);
				const std::set<std::string> names = namesOf(sids);
				test::require(names == defaultBuildings, "_Default Start enables exactly the eleven buildings of build_menu.md section 1 (got " + std::to_string(names.size()) + ")");
				test::require(sids.size() == defaultBuildings.size(), "no duplicate in the unlocked list");
				// BUILDING record order, REMOVED skipped.
				std::size_t at = 0;
				for (const kdata::Record* r : rig.gd->byType(buildingType))
				{
					if (at < sids.size() && r->stringID == sids[at])
						++at;
				}
				test::require(at == sids.size(), "the buildings come in BUILDING record order");
			}

			// Freedom Seekers lists its own research.
			const StartOff* seekers = rig.catalog->find(rig.startSid.at("The Freedom Seekers"));
			test::require(seekers != nullptr && !seekers->research.empty(), "Freedom Seekers lists research");
			const std::vector<std::string> seekerSet = startResearch(*rig.gd, seekers->research);
			test::require(namesOf(seekerSet) == std::set<std::string>{"_start- freedom seekers", "_Low Start"} && seekerSet.size() == 2,
				"Freedom Seekers completes _start- freedom seekers + _Low Start");
			{
				const std::vector<std::string> sids = sim.unlockedBuildings(seekerSet);
				const std::set<std::string> union_ = namesOf(sids);
				std::set<std::string> expect;
				for (const std::string& sid : seekerSet)
				{
					const kdata::RefCategory* refs = rig.gd->find(sid)->findRefs("enable buildings");
					for (const kdata::RefEntry& entry : refs != nullptr ? refs->entries : std::vector<kdata::RefEntry>{})
					{
						const kdata::Record* b = rig.gd->find(entry.targetID);
						if (b != nullptr && b->type == buildingType && !b->boolOr("REMOVED", false))
							expect.insert(b->name);
					}
				}
				test::require(union_ == expect && !union_.empty(), "the unlocked set is the union of the research's `enable buildings`");
			}
			// A start-off with research on the ready list: the frame in the loop above matched its own list; make sure one of the four has some.
			bool ownResearch = false;
			for (const char* name : {"Wanderer", "Nobodies", "The Holy Sword", "Son of a Captain"})
				ownResearch = ownResearch || !rig.catalog->find(rig.startSid.at(name))->research.empty();
			std::cout << "ksim_newgame: a ready start-off lists research: " << (ownResearch ? "yes" : "no") << "\n";

			// The frame decoded by hand: raw bytes of an initial snapshot after a new game.
			requireReason(admin.order(84, Buf{}), "", "new_world for UNLOCKS");
			{
				Client empty(sim, context, true, context.dataHash);
				test::require(empty.first.unlocks.size() == 1 && empty.first.unlocks[0].research.empty() && empty.first.unlocks[0].buildings.empty(),
					"a connect without a player faction: UNLOCKS with empty lists");
				for (const Frame& f : empty.first.all)
				{
					if (f.type == ks::kUnlocks)
						test::require(f.payload.size() == 8 + 2 + 2, "the empty frame is u64 tick, u16 0, u16 0");
				}
			}
			Client c(sim, context, true, context.dataHash);
			c.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(c, rig, "Nobodies"), 0));
			sim.run(1);
			const std::vector<std::string> nobodiesSet = startResearch(*rig.gd, rig.catalog->find(rig.startSid.at("Nobodies"))->research);
			std::vector<std::uint8_t> bytes;
			{
				ks::BinaryWriter w(bytes);
				c.session->sendTick(w, true);
			}
			const std::vector<std::string> nobodiesBuildings = sim.unlockedBuildings(nobodiesSet);
			const Frame* raw = nullptr;
			const std::vector<Frame> all = frames(bytes);
			for (const Frame& f : all)
			{
				if (f.type == ks::kUnlocks)
					raw = &f;
			}
			test::require(raw != nullptr, "an initial snapshot carries UNLOCKS");
			test::require(raw->payload.size() == 8 + 2 + 4 * nobodiesSet.size() + 2 + 4 * nobodiesBuildings.size(), "UNLOCKS size: 8 + 2 + 4n + 2 + 4m");
			{
				Cursor r{raw->payload};
				test::require(r.get<std::uint64_t>() == sim.clockTick(), "UNLOCKS tick");
				test::require(r.get<std::uint16_t>() == nobodiesSet.size(), "nResearch");
				for (const std::string& sid : nobodiesSet)
					test::require(c.text(r.get<std::uint32_t>()) == sid, "research strid " + sid);
				test::require(r.get<std::uint16_t>() == nobodiesBuildings.size(), "nBuild");
				for (const std::string& sid : nobodiesBuildings)
					test::require(c.text(r.get<std::uint32_t>()) == sid, "building strid " + sid);
				test::require(r.at == raw->payload.size(), "UNLOCKS has no trailing byte");
			}

			// An old-style player faction with an empty set gets `_Default Start` at the next tick (and on a load).
			{
				flecs::entity entity{};
				state::PlayerFaction faction;
				sim.world().ecs().each([&](flecs::entity e, const state::PlayerFaction& f)
				{
					entity = e;
					faction = f;
				});
				// The Freedom Seekers' set first, heard by the client; then an empty one.
				faction.research = startResearch(*rig.gd, rig.catalog->find(rig.startSid.at("The Freedom Seekers"))->research);
				entity.set<state::PlayerFaction>(faction);
				test::require(c.texts(c.tick(sim).unlocks.at(0).research) == faction.research, "a changed set is sent at the next tick");
				faction.research.clear();
				entity.set<state::PlayerFaction>(faction);
				test::require(sim.completedResearch().empty(), "the set is emptied");
				const Output out = c.tick(sim);
				test::require(sim.completedResearch() == defaultSet, "an empty set completes _Default Start at the next tick");
				test::require(out.unlocks.size() == 1 && c.texts(out.unlocks[0].research) == defaultSet
					&& namesOf(c.texts(out.unlocks[0].buildings)) == defaultBuildings, "the change is sent as UNLOCKS");
				// And through a save: an emptied set in the bytes is filled at the load, before the resend.
				faction.research.clear();
				entity.set<state::PlayerFaction>(faction);
				std::ostringstream saved;
				sim.save(saved);
				std::istringstream in(saved.str());
				sim.reload(in);
				test::require(sim.completedResearch() == defaultSet, "a save with no completed research gets _Default Start at the load");
			}

			// Save, load, continue: the research is in the save and the hash.
			sim.run(10);
			const std::uint64_t savedTick = sim.clockTick();
			const std::uint64_t savedHash = sim.stateHash();
			const std::vector<std::string> savedSet = sim.completedResearch();
			Client d(sim, context, true, context.dataHash);
			requireReason(d.order(80, Buf{}.str("unl")), "", "save unl");
			sim.run(30);
			const Output out = d.order(81, Buf{}.str("unl").put<std::uint8_t>(0));
			requireReason(out, "", "load unl");
			test::require(sim.clockTick() == savedTick && sim.stateHash() == savedHash && sim.completedResearch() == savedSet,
				"load: the same tick, hash and completed research");
			test::require(out.unlocks.size() == 1 && d.texts(out.unlocks[0].research) == savedSet, "UNLOCKS in the load's resend");
			// A different set hashes differently.
			flecs::entity entity{};
			state::PlayerFaction faction;
			sim.world().ecs().each([&](flecs::entity e, const state::PlayerFaction& f)
			{
				entity = e;
				faction = f;
			});
			faction.research = seekerSet;
			entity.set<state::PlayerFaction>(faction);
			test::require(sim.stateHash() != savedHash, "the research is part of the state hash");
			const Output changed = d.tick(sim);
			test::require(changed.unlocks.size() == 1 && d.texts(changed.unlocks[0].research) == seekerSet, "a changed set is sent at the next tick");
		}

		// ---- a server without a mark refuses new_world ----
		{
			Simulation bare = Simulation::load(KENSHI_DATA, 3);
			ks::ServerContext bareContext;
			bareContext.dataHash = context.dataHash;
			Client c(bare, bareContext, true, context.dataHash);
			requireReason(c.order(84, Buf{}), "unsupported", "new_world without a pristine mark");
			test::require(c.first.starts.empty(), "no game data, no STARTS");
		}

		// ---- --give-item (ksim_server, task BR2): the configured items land in every new game's player pack ----
		{
			const kdata::Record* bread = nullptr;
			for (const kdata::Record* r : rig.gd->byType(kworld::itemTypeByName("ITEM")))
			{
				if (r->name == "Bread")
					bread = r;
			}
			test::require(bread != nullptr, "Bread exists in the data");
			requireReason(admin.order(84, Buf{}), "", "new_world before the --give-item run");
			sim.setNewGameItems({{bread->stringID, 3}});
			Client c(sim, context, true, context.dataHash);
			const Output queued = c.frame(ks::kNewGame, Client::newGameBody("Pilgrims", startId(c, rig, "Wanderer"), 0));
			const Output out = c.tick(sim);
			test::require(queued.results.empty() && out.results.size() == 1 && out.results[0].result == 0, "--give-item: the new game starts");
			const std::uint64_t member = out.results[0].members.front();
			std::size_t breads = 0;
			sim.world().ecs().each([&](const state::Id& id, const state::Inventory& pack)
			{
				if (id.value != member)
					return;
				for (const state::Inventory::Entry& entry : pack.items)
					breads += entry.itemSid == bread->stringID ? entry.count : 0;
			});
			test::require(breads == 3, "--give-item: the player character carries the three Bread");
			sim.setNewGameItems({}); // the rest of the suite runs without it
		}

		std::cout << "ksim_newgame: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_newgame FAILED: " << e.what() << "\n";
		return 1;
	}
}
