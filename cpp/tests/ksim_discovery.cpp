// Map discovery (SIMLINK.md DISCOVERED 37 and event 19; kinspect findings/map24_static.md section 5): the radius per town type, the
// visited radius, types 8 and 10, the camera focus term, the once-per-sim-second check, frame 37 decoded by hand (on connect, only on
// change, whole list in TOWN record order), event 19 (not for camps), a new game's empty list, save, load and continue.
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

// DISCOVERED (37): u64 tick, u16 n x {strid town, u8 visited}.
struct Wire37
{
	std::uint64_t tick = 0;
	std::vector<std::pair<std::uint32_t, std::uint8_t>> places;
};

Wire37 decode37(const Frame& frame)
{
	Cursor c{frame.payload};
	Wire37 w;
	w.tick = c.get<std::uint64_t>();
	const std::uint16_t n = c.get<std::uint16_t>();
	for (std::uint16_t i = 0; i < n; ++i)
	{
		const std::uint32_t town = c.get<std::uint32_t>();
		w.places.emplace_back(town, c.get<std::uint8_t>());
	}
	test::require(c.at == frame.payload.size(), "frame 37 has trailing bytes");
	return w;
}

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
	std::vector<Wire37> lists; // every frame 37 since the last clear
	std::vector<std::string> events; // the town of every event 19 since the last clear

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
			if (frame.type == ks::kDiscovered)
				lists.push_back(decode37(frame));
			if (frame.type == ks::kEvent)
			{
				Cursor c{frame.payload};
				c.get<std::uint64_t>();
				if (c.get<std::uint16_t>() == 19)
					events.push_back(session->strings().text(c.get<std::uint32_t>()));
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

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim.combat()->find(id)); }
	void put(double x, double z)
	{
		state::Fighter& f = fighter(squad.players[0]);
		f.x = x;
		f.z = z;
	}

	// The place in the faction's list, if any.
	const Simulation::DiscoveredPlace* find(const std::vector<Simulation::DiscoveredPlace>& list, const std::string& sid) const
	{
		for (const Simulation::DiscoveredPlace& p : list)
		{
			if (p.sid == sid)
				return &p;
		}
		return nullptr;
	}
	bool discovered(const std::string& sid) const { return find(sim.discoveredPlaces(), sid) != nullptr; }
	bool visited(const std::string& sid) const
	{
		const Simulation::DiscoveredPlace* p = find(sim.discoveredPlaces(), sid);
		return p != nullptr && p->visited;
	}
	// Moves to the point, runs the 1.0 s so that exactly one check happens.
	void goAndCheck(double x, double z)
	{
		put(x, z);
		tick(static_cast<int>(Simulation::kDiscoveryIntervalTicks));
	}
};

struct Pick
{
	std::string sid;
	std::int32_t type = 0;
	double x = 0, z = 0;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_discovery HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// One far-from-the-Hub town with a single placement per type.
		std::map<std::int32_t, Pick> picks;
		std::vector<std::string> recordOrder;
		{
			const kworld::Towns towns = kworld::Towns::join(*gd, KENSHI_DATA);
			for (const kworld::TownRecord& town : towns.towns())
			{
				const kdata::Record* record = gd->find(town.stringID);
				if (record == nullptr || record->boolOr("REMOVED", false))
					continue;
				if (town.placementIndices.empty())
					continue;
				const kworld::TownPlacement& at = towns.placements()[town.placementIndices[0]];
				const std::int32_t type = record->intOr("type", 0);
				if (type == 8 || type == 10)
				{
					if (picks.count(type) == 0)
						picks[type] = Pick{town.stringID, type, at.x, at.z};
					continue;
				}
				if (town.placementIndices.size() != 1 || std::hypot(at.x - ks::kHubX, at.z - ks::kHubZ) < 40000.0)
					continue;
				if (picks.count(type) == 0)
					picks[type] = Pick{town.stringID, type, at.x, at.z};
			}
		}
		// Type 0 TOWNs (camps, nests) have no placement in the leveldata (nests are spawned), so they only get the radius check.
		for (const std::int32_t type : {1, 2, 4})
			test::require(picks.count(type) != 0, "no far single-placement town of type " + std::to_string(type));
		std::cout << "picked types:";
		for (const auto& [type, pick] : picks)
			std::cout << " " << type << "=" << pick.sid;
		std::cout << "\n";

		test::require(Simulation::discoveryRadius(0) == 4250.0 && Simulation::discoveryRadius(4) == 4250.0 && Simulation::discoveryRadius(2) == 17000.0
			&& Simulation::discoveryRadius(1) == 8500.0 && Simulation::discoveryRadius(3) == 8500.0 && Simulation::discoveryRadius(9) == 8500.0
			&& Simulation::discoveryRadius(8) == 0.0 && Simulation::discoveryRadius(10) == 0.0, "the radius per type");

		Rig r(order, gd, data, nav);
		test::require(r.sim.playerSquads().faction != 0, "the scenario has a player faction");

		// ---- connect: an empty list is sent; the faction starts with nothing ----
		test::require(r.lists.size() == 1 && r.lists[0].places.empty() && r.lists[0].tick == r.sim.clockTick(), "connect: frame 37 with an empty list");
		test::require(r.sim.discoveredPlaces().empty(), "a new faction has discovered nothing");
		r.lists.clear();

		// ---- once per sim-second: the check runs on ticks that are multiples of 20 ----
		{
			Rig o(order, gd, data, nav);
			const Pick& p = picks.at(1);
			while (o.sim.clockTick() % Simulation::kDiscoveryIntervalTicks != 5)
				o.tick();
			o.lists.clear();
			o.put(p.x + 100.0, p.z);
			o.tick(14);
			test::require(o.lists.empty() && !o.discovered(p.sid), "no check between two sim-seconds (tick " + std::to_string(o.sim.clockTick()) + ")");
			o.tick(1);
			test::require(o.sim.clockTick() % Simulation::kDiscoveryIntervalTicks == 0 && o.discovered(p.sid), "the check runs on the whole second");
			test::require(o.lists.size() == 1, "one frame 37 for the change");
		}

		// ---- each radius: just outside no, just inside yes; visited at 400; types 8 and 10 never ----
		// Types 0 and 4: 4250; 2: 17000; the rest: 8500.
		for (const auto& [type, pick] : picks)
		{
			if (type == 8 || type == 10)
				continue;
			const double radius = Simulation::discoveryRadius(type);
			r.goAndCheck(pick.x + radius + 50.0, pick.z);
			test::require(!r.discovered(pick.sid), "type " + std::to_string(type) + ": not discovered at radius + 50");
			r.goAndCheck(pick.x + radius - 50.0, pick.z);
			test::require(r.discovered(pick.sid) && !r.visited(pick.sid), "type " + std::to_string(type) + ": discovered at radius - 50, not visited");
			r.goAndCheck(pick.x + 410.0, pick.z);
			test::require(!r.visited(pick.sid), "type " + std::to_string(type) + ": not visited at 410");
			r.goAndCheck(pick.x + 390.0, pick.z);
			test::require(r.visited(pick.sid), "type " + std::to_string(type) + ": visited at 390");
			r.goAndCheck(pick.x + radius * 3.0, pick.z);
			test::require(r.visited(pick.sid), "type " + std::to_string(type) + ": both flags stay on");
		}
		for (const std::int32_t type : {8, 10})
		{
			if (picks.count(type) == 0)
			{
				std::cout << "note: no far single-placement town of type " << type << " in the data: its never-rule is not exercised\n";
				continue;
			}
			r.goAndCheck(picks.at(type).x, picks.at(type).z);
			test::require(!r.discovered(picks.at(type).sid), "type " + std::to_string(type) + " is never discovered");
		}
		std::cout << "radii ok\n";

		// ---- the frame: whole list in TOWN record order, visited flags, only on change ----
		{
			Rig f(order, gd, data, nav);
			f.goAndCheck(picks.at(1).x + 1000.0, picks.at(1).z);
			f.lists.clear();
			f.tick(60);
			test::require(f.lists.empty(), "no frame 37 while the list is unchanged");
			f.goAndCheck(picks.at(2).x, picks.at(2).z + 8.0); // a visit to another place
			test::require(!f.lists.empty(), "a change sends frame 37");
			const Wire37& last = f.lists.back();
			const std::vector<Simulation::DiscoveredPlace> after = f.sim.discoveredPlaces();
			test::require(last.places.size() == after.size() && last.tick % Simulation::kDiscoveryIntervalTicks == 0, "the frame is the whole list");
			const std::vector<const kdata::Record*> records = gd->byType(kworld::itemTypeByName("TOWN"));
			std::size_t cursor = 0;
			for (std::size_t i = 0; i < after.size(); ++i)
			{
				test::require(f.session->strings().text(last.places[i].first) == after[i].sid && last.places[i].second == (after[i].visited ? 1 : 0),
					"frame entry " + std::to_string(i));
				test::require(f.session->strings().entry(last.places[i].first).kind == 1, "the town is a kind-1 strid");
				while (cursor < records.size() && records[cursor]->stringID != after[i].sid)
					++cursor;
				test::require(cursor < records.size(), "the list is in TOWN record order: " + after[i].sid);
			}
			test::require(f.visited(picks.at(2).sid) && !f.visited(picks.at(1).sid) && f.discovered(picks.at(1).sid), "visited flags on the wire's source");
			f.lists.clear();
			f.connect();
			test::require(f.lists.size() == 1 && f.lists[0].places.size() == after.size(), "connect sends the whole list");
		}

		// ---- event 19: for places with a type, not for camps (type 0) ----
		{
			Rig e(order, gd, data, nav);
			e.events.clear();
			// (no placed type 0 place exists in the data, so the skip for type 0 in the session is not exercised)
			e.goAndCheck(picks.at(1).x + 100.0, picks.at(1).z);
			test::require(std::count(e.events.begin(), e.events.end(), picks.at(1).sid) == 1, "one event 19 for the type 1 place");
			e.events.clear();
			e.goAndCheck(picks.at(1).x + 50.0, picks.at(1).z);
			test::require(e.events.empty(), "no second event for the same place (nor for its visit)");
		}

		// ---- the camera focus (set_view 51) discovers, too ----
		{
			Rig v(order, gd, data, nav);
			const Pick& p = picks.at(4);
			v.sim.enqueue(Command::setView(p.x + 100.0, p.z));
			v.tick(Simulation::kDiscoveryIntervalTicks + 1);
			test::require(v.discovered(p.sid) && v.visited(p.sid), "the view focus discovers and visits");
			test::require(!v.discovered(picks.at(2).sid), "and only what is near it");
		}

		// ---- a new faction starts empty (what a new game does), then the first check fills it ----
		{
			const std::uint64_t faction = r.sim.playerSquads().faction;
			test::require(!r.sim.discoveredPlaces().empty(), "something is discovered before the reset");
			r.sim.createPlayerFaction(r.sim.playerSquads().factionSid);
			test::require(r.sim.playerSquads().faction == faction && r.sim.discoveredPlaces().empty(), "a recreated player faction has an empty list");
			std::cout << "new faction ok\n";
		}

		// ---- save, load, continue ----
		{
			Rig a(order, gd, data, nav);
			a.goAndCheck(picks.at(1).x + 100.0, picks.at(1).z);
			a.goAndCheck(picks.at(2).x + 100.0, picks.at(2).z);
			const std::vector<Simulation::DiscoveredPlace> before = a.sim.discoveredPlaces();
			test::require(before.size() >= 2, "places before the save");
			std::ostringstream saved;
			a.sim.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, a.sim.gameData());
			loaded.enableCombat(data, Rig::settings());
			loaded.enableNavigation(nav);
			test::require(loaded.stateHash() == a.sim.stateHash(), "the hash changed across save and load");
			test::require(loaded.discoveredPlaces() == before, "the list is in the save");
			// both continue: the same move, the same new place
			for (Simulation* s : {&a.sim, &loaded})
			{
				state::Fighter& f = const_cast<state::Fighter&>(*s->combat()->find(a.squad.players[0]));
				f.x = picks.at(4).x + 100.0;
				f.z = picks.at(4).z;
				s->run(45);
			}
			test::require(loaded.discoveredPlaces().size() > before.size() && loaded.discoveredPlaces() == a.sim.discoveredPlaces(), "the loaded run discovers the same");
			test::require(loaded.stateHash() == a.sim.stateHash(), "save, load and continue differs from the uninterrupted run");
			std::cout << "save ok, hash " << std::hex << a.sim.stateHash() << std::dec << "\n";
		}
		std::cout << "ksim_discovery ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_discovery FAILED: " << e.what() << "\n";
		return 1;
	}
}
