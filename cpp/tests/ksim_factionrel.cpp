// Faction relations and the faction rename (SIMLINK.md FACTION_RELATIONS 38 and order 85): frame 38 decoded by hand (on connect with and
// without a player faction, record order, the exclusion, not resent while unchanged, resent at 0.5 or more and not below), each rename
// answer, PLAYER_SQUADS factionName following, squad_add using the new name, the order log and its replay, save, load and continue.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "kworld/towns.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
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

struct Row
{
	std::string sid;
	std::string name;
	float value = 0;
};

// FACTION_RELATIONS (38): u64 tick, u16 n x {strid faction, str name, f32 value}.
struct Wire38
{
	std::uint64_t tick = 0;
	std::vector<Row> rows;
	std::vector<std::uint32_t> strids;
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
	std::unique_ptr<ks::Session> session;
	std::vector<Wire38> lists; // every frame 38 since the last clear
	std::size_t squadFrames = 0; // PLAYER_SQUADS frames since the last clear
	std::uint32_t seq = 0;
	bool answered = false;
	std::string reason; // "" after event 9

	static CombatSettings settings()
	{
		CombatSettings s;
		s.substeps = 2;
		return s;
	}

	Rig(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, bool scenario = true)
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
		if (scenario)
			squad = ks::spawnSquad(sim, *gd, *data, context);
		connect();
	}

	Wire38 decode38(const Frame& frame)
	{
		Cursor c{frame.payload};
		Wire38 w;
		w.tick = c.get<std::uint64_t>();
		const std::uint16_t n = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < n; ++i)
		{
			const std::uint32_t id = c.get<std::uint32_t>();
			w.strids.push_back(id);
			Row row;
			row.sid = session->strings().text(id);
			const std::uint16_t len = c.get<std::uint16_t>();
			test::require(c.at + len <= frame.payload.size(), "frame overrun");
			row.name.assign(frame.payload.begin() + static_cast<std::ptrdiff_t>(c.at), frame.payload.begin() + static_cast<std::ptrdiff_t>(c.at + len));
			c.at += len;
			row.value = c.get<float>();
			w.rows.push_back(std::move(row));
		}
		test::require(c.at == frame.payload.size(), "frame 38 has trailing bytes");
		return w;
	}

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type == ks::kFactionRelations)
				lists.push_back(decode38(frame));
			if (frame.type == ks::kPlayerSquads)
				++squadFrames;
			if (frame.type == ks::kEvent && seq != 0)
			{
				Cursor c{frame.payload};
				c.get<std::uint64_t>();
				const std::uint16_t kind = c.get<std::uint16_t>();
				if ((kind == 9 || kind == 10) && c.get<std::uint32_t>() == seq)
				{
					answered = true;
					reason = kind == 10 ? session->strings().text(c.get<std::uint32_t>()) : std::string();
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

	// Sends an order; returns "" for event 9, else the rejection reason. Runs one tick so the order applies.
	std::string sendOrder(std::uint16_t kind, const Buf& body)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		answered = false;
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order closed the session");
		collect(bytes);
		test::require(answered, "order " + std::to_string(kind) + " got no answer");
		const std::string result = reason;
		tick();
		return result;
	}
	std::string rename(const std::string& name)
	{
		Buf body;
		body.put<std::uint16_t>(static_cast<std::uint16_t>(name.size()));
		body.bytes.insert(body.bytes.end(), name.begin(), name.end());
		return sendOrder(85, body);
	}

	flecs::entity entityOf(const std::string& sid)
	{
		flecs::entity found{};
		sim.world().ecs().each([&](flecs::entity e, const state::FactionState& f)
		{
			if (f.templateSid == sid)
				found = e;
		});
		return found;
	}
	flecs::entity player()
	{
		flecs::entity found{};
		sim.world().ecs().each([&](flecs::entity e, const state::PlayerFaction&) { found = e; });
		return found;
	}
	// Sets the player faction's relation toward a faction.
	void setRelation(const std::string& sid, double value)
	{
		entityOf(sid).set<state::Relation>(player(), state::Relation{value}); // THEIR relation toward the player
	}
	// Runs to just before the next check, so that the following tick(1) is the whole second's check.
	void toSecondBoundary()
	{
		while ((sim.clockTick() + 1) % Simulation::kFactionRelationsIntervalTicks != 0)
			tick();
	}
};

const Row* findRow(const Wire38& w, const std::string& sid)
{
	for (const Row& row : w.rows)
	{
		if (row.sid == sid)
			return &row;
	}
	return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_factionrel HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// ---- connect without a player faction: an empty list ----
		{
			Rig n(order, gd, data, nav, false);
			test::require(n.sim.playerSquads().faction == 0, "no player faction without the scenario");
			test::require(n.lists.size() == 1 && n.lists[0].rows.empty() && n.lists[0].tick == n.sim.clockTick(), "connect: frame 38 with an empty list");
			test::require(n.rename("Pilgrims") == "not_allowed", "faction_rename without a player faction is not_allowed");
			std::cout << "no faction ok\n";
		}

		Rig r(order, gd, data, nav);
		test::require(r.sim.playerSquads().faction != 0, "the scenario has a player faction");
		const std::string playerSid = r.player().get<state::FactionState>()->templateSid;

		// ---- connect: the seeded known set (what the template sets explicitly), the rules of the list ----
		test::require(r.lists.size() == 1 && r.lists[0].tick == r.sim.clockTick(), "connect: one frame 38");
		const auto lower = [](std::string text)
		{
			for (char& c : text)
				c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
			return text;
		};
		const auto known = [&](const std::string& sid)
		{
			const auto& list = r.player().get<state::PlayerFaction>()->known;
			return std::find(list.begin(), list.end(), sid) != list.end();
		};
		const auto checkList = [&](const Wire38& w, const std::string& what)
		{
			std::vector<const kdata::Record*> expect;
			for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("FACTION")))
			{
				if (known(record->stringID) && record->stringID != playerSid && record->stringID != "GENERATION_TEMPLATE_FACTION"
					&& !record->boolOr("not real", false) && r.entityOf(record->stringID).is_valid())
					expect.push_back(record);
			}
			std::stable_sort(expect.begin(), expect.end(), [&](const kdata::Record* x, const kdata::Record* y)
			{
				return lower(x->name) != lower(y->name) ? lower(x->name) < lower(y->name) : x->name.size() < y->name.size();
			});
			test::require(w.rows.size() == expect.size(), what + ": the known set minus not real, the template and the player: " + std::to_string(w.rows.size()) + " vs " + std::to_string(expect.size()));
			for (std::size_t i = 0; i < expect.size(); ++i)
			{
				test::require(w.rows[i].sid == expect[i]->stringID && w.rows[i].name == expect[i]->name, what + ": order by name at " + std::to_string(i));
				const double theirs = r.entityOf(w.rows[i].sid).get<state::Relation>(r.player()) != nullptr
					? r.entityOf(w.rows[i].sid).get<state::Relation>(r.player())->value
					: expect[i]->intOr("default relation", 0);
				test::require(w.rows[i].value == static_cast<float>(theirs), what + ": THEIR relation toward the player: " + w.rows[i].sid);
				test::require(r.session->strings().entry(w.strids[i]).kind == 1, "faction is a kind-1 strid");
			}
		};
		checkList(r.lists[0], "connect");
		std::cout << "seeded rows at connect: " << r.lists[0].rows.size() << " (known " << r.player().get<state::PlayerFaction>()->known.size() << ")\n";
		r.lists.clear();

		// ---- a relation change in either direction makes a faction known; not real, the template and the player never show ----
		std::vector<std::string> picks;
		std::string notReal;
		bool haveTemplate = false;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("FACTION")))
		{
			if (!r.entityOf(record->stringID).is_valid() || record->stringID == playerSid || known(record->stringID))
				continue;
			if (record->stringID == "GENERATION_TEMPLATE_FACTION")
				haveTemplate = true;
			else if (record->boolOr("not real", false))
				notReal = notReal.empty() ? record->stringID : notReal;
			else if (picks.size() < 6)
				picks.push_back(record->stringID);
		}
		test::require(picks.size() == 6, "six unknown factions to learn");
		std::vector<std::string> learn = picks;
		if (!notReal.empty())
			learn.push_back(notReal);
		if (haveTemplate)
			learn.emplace_back("GENERATION_TEMPLATE_FACTION");
		const state::Id playerId = *r.player().get<state::Id>();
		for (std::size_t i = 0; i < learn.size(); ++i)
		{
			const state::Id otherId = *r.entityOf(learn[i]).get<state::Id>();
			r.sim.enqueue(i % 2 == 0 ? Command::setFactionRelation(playerId, otherId, -20) : Command::setFactionRelation(otherId, playerId, 30));
		}
		r.toSecondBoundary();
		r.tick(2);
		for (const std::string& sid : learn)
			test::require(known(sid), "known after a relation change (either direction): " + sid);
		r.tick(Simulation::kFactionRelationsIntervalTicks);
		test::require(!r.lists.empty(), "the new known set is sent");
		const Wire38 first = r.lists.back();
		checkList(first, "after learning");
		for (const std::string& sid : picks)
			test::require(findRow(first, sid) != nullptr, "learned row: " + sid);
		test::require(notReal.empty() || findRow(first, notReal) == nullptr, "a `not real` faction is never listed");
		test::require(findRow(first, "GENERATION_TEMPLATE_FACTION") == nullptr && findRow(first, playerSid) == nullptr, "the template and the player are not listed");
		std::cout << "rows after learning: " << first.rows.size() << "\n";
		r.lists.clear();

		// ---- not resent while unchanged ----
		r.tick(Simulation::kFactionRelationsIntervalTicks * 3);
		test::require(r.lists.empty(), "no frame 38 while nothing changed");

		// ---- changes: below 0.5 no, 0.5 or more yes, only on the second's check ----
		const std::string target = first.rows.front().sid;
		const double base = first.rows.front().value;
		const double up = base > 0 ? -1.0 : 1.0; // moves inside -100..100
		r.toSecondBoundary();
		r.setRelation(target, base + up * 0.4);
		r.tick();
		test::require(r.lists.empty(), "a change of 0.4 is not sent");
		r.toSecondBoundary();
		r.tick(); // the check passes
		r.setRelation(target, base + up * 0.5);
		r.tick(Simulation::kFactionRelationsIntervalTicks - 1);
		test::require(r.lists.empty(), "a change of 0.5 waits for the second's check");
		r.tick();
		test::require(r.lists.size() == 1 && findRow(r.lists[0], target)->value == static_cast<float>(base + up * 0.5), "a change of 0.5 resends the whole list");
		test::require(r.lists[0].rows.size() == first.rows.size(), "the whole list");
		r.lists.clear();
		r.toSecondBoundary();
		r.setRelation(target, base + up * 0.5 + up * 0.4); // 0.4 from what the client last heard
		r.tick();
		test::require(r.lists.empty(), "0.4 from the last sent value is not sent (the comparison is to the last send)");
		r.toSecondBoundary();
		r.setRelation(target, base + up * 0.5 + up * 0.5);
		r.tick();
		test::require(r.lists.size() == 1, "0.5 from the last sent value is sent");
		r.lists.clear();
		r.connect();
		test::require(r.lists.size() == 1 && r.lists[0].rows.size() == first.rows.size(), "connect sends the whole list again");
		r.lists.clear();
		std::cout << "relations ok\n";

		// ---- faction_rename answers ----
		const std::string before = r.sim.playerSquads().factionName;
		test::require(r.rename("") == "bad_name", "empty name");
		test::require(r.rename(std::string(33, 'x')) == "bad_name", "33 characters");
		test::require(r.rename("   ") == "bad_name", "only blanks");
		test::require(r.rename("bad\tname") == "bad_name", "a control character");
		test::require(r.rename("\xC3\x28") == "bad_name", "invalid UTF-8");
		test::require(r.sim.playerSquads().factionName == before, "a rejected rename changes nothing");
		r.squadFrames = 0;
		{
			const std::string other = gd->find(picks[0])->name;
			test::require(r.rename(other) == "name_taken", "another faction's display name is name_taken");
			test::require(r.sim.playerSquads().factionName == before, "a name_taken rename changes nothing");
		}
		test::require(r.rename("Pilgrims") == "", "a good name is event 9");
		test::require(r.sim.playerSquads().factionName == "Pilgrims", "the name is set");
		test::require(r.squadFrames == 1, "PLAYER_SQUADS is resent through its change path");
		test::require(r.rename("Pilgrims") == "", "the same rename again is answered");
		test::require(r.squadFrames == 1, "and changes nothing, so nothing is resent");
		test::require(r.rename("T\xC3\xA9te") == "", "UTF-8 is fine");
		test::require(r.rename(gd->find(playerSid)->name) == "", "the player faction's own record name is free to take back");
		test::require(r.rename("Pilgrims") == "", "back to Pilgrims");
		test::require(r.lists.empty(), "a rename does not resend FACTION_RELATIONS");

		// ---- squad_add after a rename uses the new name; existing names stay ----
		{
			const PlayerSquadsView oldView = r.sim.playerSquads();
			test::require(r.sendOrder(70, Buf{}) == "", "squad_add");
			const PlayerSquadsView view = r.sim.playerSquads();
			test::require(view.squads.size() == oldView.squads.size() + 1, "a squad was added");
			test::require(view.squads.back().name.rfind("Pilgrims_", 0) == 0, "the new squad is named after the new faction name: " + view.squads.back().name);
			for (std::size_t i = 0; i < oldView.squads.size(); ++i)
				test::require(view.squads[i].name == oldView.squads[i].name, "existing squad names are kept");
		}
		std::cout << "rename ok\n";

		// ---- the order log carries the rename; a replay reproduces it ----
		{
			Rig a(order, gd, data, nav);
			a.sim.enqueue(Command::setFactionRelation(*a.player().get<state::Id>(), *a.entityOf(picks[0]).get<state::Id>(), -20));
			a.tick(7);
			test::require(a.rename("Wanderers") == "", "rename in rig a");
			a.tick(30);
			Command logged;
			std::size_t count = 0;
			for (const ks::OrderLogEntry& entry : a.context.orderLog)
			{
				if (entry.kind == 85)
				{
					logged = entry.command;
					++count;
					test::require(entry.sim, "the rename is a sim order");
				}
			}
			test::require(count == 1 && logged.type == CommandType::SquadOrder && logged.value == 85 && logged.text == "Wanderers", "the rename is in the order log");
			Rig b(order, gd, data, nav);
			b.sim.enqueue(Command::setFactionRelation(*b.player().get<state::Id>(), *b.entityOf(picks[0]).get<state::Id>(), -20));
			b.tick(7);
			b.sim.enqueue(logged);
			b.tick(31);
			test::require(b.sim.playerSquads().factionName == "Wanderers" && b.sim.clockTick() == a.sim.clockTick(), "the replay renames");
			test::require(b.sim.stateHash() == a.sim.stateHash(), "the replay gives the same hash");

			// ---- save, load, continue ----
			std::ostringstream saved;
			a.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, a.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.playerSquads().factionName == "Wanderers" && loaded.stateHash() == a.sim.stateHash(), "the name is in the save");
			{
				const auto rowsA = a.sim.factionRelations();
				const auto rowsL = loaded.factionRelations();
				test::require(!rowsA.empty() && rowsA.size() == rowsL.size() && rowsA.front().sid == rowsL.front().sid, "the known set is in the save");
			}
			SquadOrder add;
			add.kind = 70;
			for (Simulation* s : {&a.sim, &loaded})
			{
				s->enqueue(Command::squadOrder(add));
				s->run(30);
			}
			test::require(loaded.stateHash() == a.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			test::require(loaded.playerSquads().squads.back().name.rfind("Wanderers_", 0) == 0, "the loaded world names new squads after the saved name");
			std::cout << "save ok, hash " << std::hex << a.sim.stateHash() << std::dec << "\n";
		}
		std::cout << "ksim_factionrel ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_factionrel FAILED: " << e.what() << "\n";
		return 1;
	}
}
