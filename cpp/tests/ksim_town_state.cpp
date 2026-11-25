// TOWN_STATE 66 (SIMLINK.md): visited towns only, the owner following SetTownOwner, the resident count following a death, the 5 s rate limit,
// the placeholder bits, decoded by hand. argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

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

// TOWN_STATE (66): u64 tick, u16 n x entry (SIMLINK.md).
struct Entry
{
	std::string town, owner;
	std::uint8_t flags = 0;
	std::uint16_t live = 0;
	std::int32_t residentsActual = 0, roamingPop = 0, roamingSquads = 0;
	std::vector<std::string> residentFactions, hostile;
};
struct Wire66
{
	std::uint64_t tick = 0;
	std::size_t size = 0;
	std::vector<Entry> towns;
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
	std::vector<Wire66> lists; // every frame 66 since the last clear

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

	Wire66 decode66(const Frame& frame)
	{
		Cursor c{frame.payload};
		Wire66 w;
		w.size = frame.payload.size() + 6;
		w.tick = c.get<std::uint64_t>();
		const auto text = [&](std::uint32_t id) { return id == 0 ? std::string() : session->strings().text(id); };
		const std::uint16_t n = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < n; ++i)
		{
			Entry e;
			e.town = text(c.get<std::uint32_t>());
			e.owner = text(c.get<std::uint32_t>());
			e.flags = c.get<std::uint8_t>();
			e.live = c.get<std::uint16_t>();
			c.get<std::int32_t>(); // residentsMaxCur
			c.get<std::int32_t>();
			e.residentsActual = c.get<std::int32_t>();
			c.get<std::int32_t>(); // roamingMaxCur
			c.get<std::int32_t>();
			e.roamingPop = c.get<std::int32_t>();
			e.roamingSquads = c.get<std::int32_t>();
			for (std::vector<std::string>* list : {&e.residentFactions, &e.hostile})
			{
				const std::uint8_t k = c.get<std::uint8_t>();
				for (std::uint8_t j = 0; j < k; ++j)
					list->push_back(text(c.get<std::uint32_t>()));
			}
			w.towns.push_back(std::move(e));
		}
		test::require(c.at == frame.payload.size(), "frame 66 has trailing bytes");
		return w;
	}

	void collect(const std::vector<std::uint8_t>& bytes)
	{
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type == ks::kTownState)
				lists.push_back(decode66(frame));
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
	flecs::entity entity;
	double x = 0, z = 0;
	std::int32_t residents = 0;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_town_state HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		Rig r(order, gd, data, nav);
		const flecs::world& ecs = r.sim.world().ecs();

		// Two far-apart towns of a discoverable type, one placement each, with resident squads.
		std::map<std::string, std::int32_t> residents;
		ecs.each([&](const state::SquadInstance& s)
		{
			if (s.homeTown.id != 0 && (s.source == state::SpawnSource::TownResident || s.source == state::SpawnSource::TownDefaultResident))
				if (const state::TownState* t = flecs::entity(ecs.c_ptr(), s.homeTown.id).get<state::TownState>())
					++residents[t->rootSid];
		});
		std::vector<Pick> picks;
		{
			const kworld::Towns towns = kworld::Towns::join(*gd, KENSHI_DATA);
			for (const kworld::TownRecord& town : towns.towns())
			{
				const kdata::Record* record = gd->find(town.stringID);
				if (record == nullptr || record->boolOr("REMOVED", false) || town.placementIndices.size() != 1 || residents.count(town.stringID) == 0
					|| Simulation::discoveryRadius(record->intOr("type", 0)) == 0.0)
					continue;
				const kworld::TownPlacement& at = towns.placements()[town.placementIndices[0]];
				if (std::hypot(at.x - ks::kHubX, at.z - ks::kHubZ) < 40000.0)
					continue;
				bool apart = true;
				for (const Pick& p : picks)
					apart = apart && std::hypot(at.x - p.x, at.z - p.z) > 30000.0;
				if (apart && picks.size() < 2)
				{
					Pick pick;
					pick.sid = town.stringID;
					pick.x = at.x;
					pick.z = at.z;
					picks.push_back(pick);
				}
			}
		}
		test::require(picks.size() == 2, "two far apart towns with resident squads");
		ecs.each([&](flecs::entity e, const state::TownState& t)
		{
			for (Pick& p : picks)
				if (t.rootSid == p.sid)
					p.entity = e;
		});
		std::cout << "towns: " << picks[0].sid << ", " << picks[1].sid << "\n";
		const auto sidOfOwner = [&](const Pick& p)
		{
			const state::TownState* t = p.entity.get<state::TownState>();
			return t->owner.id == 0 ? std::string() : flecs::entity(ecs.c_ptr(), t->owner.id).get<state::FactionState>()->templateSid;
		};
		const auto findEntry = [](const Wire66& w, const std::string& sid) -> const Entry*
		{
			for (const Entry& e : w.towns)
				if (e.town == sid)
					return &e;
			return nullptr;
		};
		// The live members of the town's resident squads, counted from the arena / ECS.
		const auto aliveResidents = [&](const Pick& p)
		{
			std::int32_t n = 0;
			ecs.each([&](flecs::entity squad, const state::SquadInstance& s)
			{
				if (s.homeTown.id == p.entity.id() && (s.source == state::SpawnSource::TownResident || s.source == state::SpawnSource::TownDefaultResident))
					r.sim.world().eachMember(squad, [&](flecs::entity member, const state::Id& id)
					{
						const state::Fighter* f = r.sim.combat()->find(id.value);
						if (f == nullptr)
							f = member.get<state::Fighter>();
						const state::MemberDelta* d = f == nullptr ? member.get<state::MemberDelta>() : nullptr;
						n += (f != nullptr ? f->dead : d != nullptr && (d->mask & state::MemberDelta::kBody) != 0 && d->dead) ? 0 : 1;
					});
			});
			return n;
		};

		// ---- a discovered, unvisited town gets no entry (the start place, the Hub, is visited by the first check and may give a frame) ----
		r.goAndCheck(picks[0].x + 3000.0, picks[0].z);
		r.tick(100);
		std::size_t base = 0; // entries already on the list: the towns visited at the start
		if (!r.lists.empty())
		{
			base = r.lists.back().towns.size();
			for (const Entry& e : r.lists.back().towns)
			{
				bool visited = false;
				for (const Simulation::DiscoveredPlace& place : r.sim.discoveredPlaces())
					visited = visited || (place.sid == e.town && place.visited);
				test::require(visited && e.town != picks[0].sid && e.town != picks[1].sid, "only visited towns have an entry: " + e.town);
			}
		}
		r.lists.clear();

		// ---- first visit: one frame, one more entry ----
		r.goAndCheck(picks[0].x + 10.0, picks[0].z);
		for (int i = 0; i < 200 && r.lists.empty(); ++i)
			r.tick();
		test::require(r.lists.size() == 1 && r.lists[0].towns.size() == base + 1, "one frame with one more entry after the first visit");
		{
			const Entry* found = findEntry(r.lists[0], picks[0].sid);
			test::require(found != nullptr, "the visited town has an entry");
			const Entry& e = *found;
			test::require(e.town == picks[0].sid && e.owner == sidOfOwner(picks[0]), "the town and its live owner");
			test::require((e.flags & 3) == 1, "flags: known, not dead");
			test::require((e.live & 1) == 0 && (e.live & 4) == 0 && e.live == 0x7a, "live bits 0 and 2 clear, the rest set");
			test::require(e.residentsActual == aliveResidents(picks[0]) && e.residentsActual > 0, "the resident count");
			std::cout << "entry: residents " << e.residentsActual << ", roaming " << e.roamingPop << "/" << e.roamingSquads << ", factions "
				<< e.residentFactions.size() << ", hostile " << e.hostile.size() << ", frame " << r.lists[0].size << " bytes, "
				<< r.lists[0].towns.size() << " towns\n";
		}
		const std::uint64_t firstTick = r.lists[0].tick;
		r.lists.clear();

		// ---- the rate limit: a second visit inside 5 s waits; the frame has both entries (record order, no others) ----
		r.goAndCheck(picks[1].x + 10.0, picks[1].z);
		r.tick(Simulation::kTownStateMinGapTicks - 60); // 20 + this < gap after the first frame
		test::require(r.lists.empty() && r.sim.clockTick() - firstTick < Simulation::kTownStateMinGapTicks, "no second frame within 5 sim-seconds");
		for (int i = 0; i < 200 && r.lists.empty(); ++i)
			r.tick();
		test::require(r.lists.size() == 1 && r.lists[0].tick - firstTick >= Simulation::kTownStateMinGapTicks, "the second frame comes at 5 s or later");
		test::require(r.lists[0].towns.size() == base + 2 && findEntry(r.lists[0], picks[0].sid) != nullptr && findEntry(r.lists[0], picks[1].sid) != nullptr,
			"one entry per visited town");
		{
			const std::vector<const kdata::Record*> records = gd->byType(kworld::itemTypeByName("TOWN"));
			std::size_t cursor = 0;
			for (const Entry& e : r.lists[0].towns)
			{
				while (cursor < records.size() && records[cursor]->stringID != e.town)
					++cursor;
				test::require(cursor < records.size(), "TOWN record order: " + e.town);
			}
		}
		std::uint64_t lastTick = r.lists[0].tick;
		r.lists.clear();
		r.tick(200);
		test::require(r.lists.empty(), "no frame while nothing changes");

		// ---- the owner follows SetTownOwner ----
		std::uint64_t other = 0;
		std::string otherSid;
		ecs.each([&](flecs::entity e, const state::Id& id, const state::FactionState& f)
		{
			if (other == 0 && e.id() != picks[0].entity.get<state::TownState>()->owner.id && !e.has<state::PlayerFaction>())
			{
				other = id.value;
				otherSid = f.templateSid;
			}
		});
		test::require(other != 0, "another faction to give the town to");
		r.sim.enqueue(Command::setTownOwner(*picks[0].entity.get<state::Id>(), state::Id{other}));
		for (int i = 0; i < 200 && r.lists.empty(); ++i)
			r.tick();
		test::require(r.lists.size() == 1, "an owner change sends a frame");
		test::require(findEntry(r.lists[0], picks[0].sid)->owner == otherSid, "the owner strid follows SetTownOwner");
		test::require(r.lists[0].tick - lastTick >= Simulation::kTownStateMinGapTicks, "the owner frame respects the gap");
		lastTick = r.lists[0].tick;
		r.lists.clear();

		// ---- a resident's death lowers the count ----
		const std::int32_t before = aliveResidents(picks[0]);
		bool killed = false;
		ecs.each([&](flecs::entity squad, const state::SquadInstance& s)
		{
			if (killed || s.homeTown.id != picks[0].entity.id()
				|| (s.source != state::SpawnSource::TownResident && s.source != state::SpawnSource::TownDefaultResident))
				return;
			r.sim.world().eachMember(squad, [&](flecs::entity member, const state::Id& id)
			{
				if (killed)
					return;
				if (state::Fighter* f = const_cast<state::Fighter*>(r.sim.combat()->find(id.value)))
					f->dead = killed = true;
				else if (state::Fighter* g = member.get_mut<state::Fighter>())
					g->dead = killed = true;
				else
				{
					state::MemberDelta delta; // a token member: the persistent subset says it died
					delta.mask = state::MemberDelta::kBody;
					delta.dead = true;
					member.set<state::MemberDelta>(std::move(delta));
					killed = true;
				}
			});
		});
		test::require(killed, "found a resident to kill");
		for (int i = 0; i < 200 && r.lists.empty(); ++i)
			r.tick();
		test::require(r.lists.size() == 1, "a death sends a frame");
		test::require(findEntry(r.lists[0], picks[0].sid)->residentsActual == before - 1 && before - 1 == aliveResidents(picks[0]), "the resident count follows the death");
		test::require(r.lists[0].tick - lastTick >= Simulation::kTownStateMinGapTicks, "the death frame respects the gap");
		r.lists.clear();

		// ---- connect sends every visited town at once ----
		r.connect();
		test::require(r.lists.size() == 1 && r.lists[0].towns.size() == base + 2, "connect: every visited town");
		std::cout << "ksim_town_state ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_town_state FAILED: " << e.what() << "\n";
		return 1;
	}
}
