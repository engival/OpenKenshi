// Research state over SIMLINK (docs/formats/SIMLINK.md RESEARCH_STATE, frame 63; CRAFTING_SLICE 9.8): through an in-process Session (no socket),
// the frame decoded by hand from the layout in SIMLINK.md. On a new game: tech level 1, Dried Meat the only item, the Simple Clothing Types row
// listable, the silently finished record with reason 4; after one completion: the record Completed, its enables unlocked, the frame sent once.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/navigator.hpp"
#include "ksim/economy/research.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
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

// RESEARCH_STATE (type 63), decoded from the layout in SIMLINK.md.
struct Wire63
{
	struct Record
	{
		std::string sid;
		std::uint8_t state = 0;
		std::uint8_t reason = 0;
	};
	std::uint64_t tick = 0;
	std::uint8_t techLevel = 0;
	std::vector<Record> records;
	std::map<std::uint8_t, std::vector<std::string>> kinds;
};

struct Client
{
	std::unique_ptr<ks::Session> session;

	Client(Simulation& sim, ks::ServerContext& context)
		: session(std::make_unique<ks::Session>(sim, context))
	{
		session->setAdmin(true);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		first = decode(bytes);
	}

	std::vector<Wire63> first;

	std::vector<Wire63> decode(const std::vector<std::uint8_t>& bytes) const
	{
		std::vector<Wire63> out;
		for (const Frame& frame : frames(bytes))
		{
			if (frame.type != ks::kResearchState)
				continue;
			Cursor c{frame.payload};
			Wire63 w;
			w.tick = c.get<std::uint64_t>();
			w.techLevel = c.get<std::uint8_t>();
			const std::uint16_t n = c.get<std::uint16_t>();
			for (std::uint16_t i = 0; i < n; ++i)
			{
				Wire63::Record r;
				r.sid = session->strings().text(c.get<std::uint32_t>());
				r.state = c.get<std::uint8_t>();
				r.reason = c.get<std::uint8_t>();
				w.records.push_back(std::move(r));
			}
			const std::uint8_t kinds = c.get<std::uint8_t>();
			for (std::uint8_t k = 0; k < kinds; ++k)
			{
				const std::uint8_t id = c.get<std::uint8_t>();
				const std::uint16_t count = c.get<std::uint16_t>();
				w.kinds[id];
				for (std::uint16_t i = 0; i < count; ++i)
					w.kinds[id].push_back(session->strings().text(c.get<std::uint32_t>()));
			}
			test::require(c.at == frame.payload.size(), "RESEARCH_STATE has trailing bytes");
			out.push_back(std::move(w));
		}
		return out;
	}

	std::vector<Wire63> tick(Simulation& sim)
	{
		sim.run(1);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->sendTick(w, false);
		return decode(bytes);
	}
};

const Wire63::Record* find(const Wire63& w, const std::string& sid)
{
	for (const Wire63::Record& r : w.records)
		if (r.sid == sid)
			return &r;
	return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_research_wire HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		ks::ServerContext context;
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		ks::spawnSquad(sim, *gd, *data, context);
		sim.run(5);

		const auto nameOf = [&](const std::string& sid) { const kdata::Record* r = gd->find(sid); return r != nullptr ? r->name : std::string("?"); };
		Client client(sim, context);
		test::require(client.first.size() == 1, "one RESEARCH_STATE with the connect snapshot");
		const Wire63& start = client.first[0];
		const research::Snapshot& truth = sim.researchState();
		test::require(start.techLevel == 1 && truth.techLevel == 1, "a new game is tech level 1");
		test::require(start.records.size() == truth.rows.size() && !start.records.empty(), "a record per RESEARCH row");
		for (std::size_t i = 0; i < truth.rows.size(); ++i)
			test::require(start.records[i].sid == truth.rows[i].sid && start.records[i].state == static_cast<std::uint8_t>(truth.rows[i].state), "record " + std::to_string(i) + " in row order with the row's state");
		const std::vector<std::string>& items = start.kinds.at(1);
		test::require(items.size() == 1 && nameOf(items[0]) == "Dried Meat", "kind 1 (item): only Dried Meat");
		test::require(start.kinds.size() == research::kKindCount, "eight kinds");
		const std::vector<std::string> own = sim.completedResearch();
		int silent = 0;
		const Wire63::Record* clothing = nullptr;
		for (const Wire63::Record& r : start.records)
		{
			if (r.state == 0 && std::find(own.begin(), own.end(), r.sid) == own.end())
			{
				test::require(r.reason == 4, "a silently finished record carries reason 4");
				++silent;
			}
			if (nameOf(r.sid) == "Simple Clothing Types")
				clothing = &r;
		}
		test::require(silent >= 1, "the silent record is there (reason 4)");
		test::require(clothing != nullptr && clothing->state == 1 && clothing->reason == 0, "Simple Clothing Types is listable on a new game");
		std::size_t completed = 0;
		for (const Wire63::Record& r : start.records)
			completed += r.state == 0;
		test::require(completed == own.size() + static_cast<std::size_t>(silent), "completed rows = the faction's list + the silent ones");
		test::require(client.tick(sim).empty(), "no RESEARCH_STATE while nothing changed");

		// One completion: the Simple Clothing Types record.
		bool done = false;
		const_cast<flecs::world&>(sim.world().ecs()).each([&](state::PlayerFaction& faction)
		{
			if (!done)
				faction.research.push_back(clothing->sid);
			done = true;
		});
		test::require(done, "a player faction to complete research on");
		const std::vector<Wire63> after = client.tick(sim);
		test::require(after.size() == 1, "one RESEARCH_STATE after the completion");
		const Wire63::Record* now = find(after[0], clothing->sid);
		test::require(now != nullptr && now->state == 0 && now->reason == 0, "the completed record reads state 0, reason 0");
		std::size_t unlockedBefore = 0;
		std::size_t unlockedAfter = 0;
		for (const auto& [id, list] : start.kinds)
			unlockedBefore += list.size();
		for (const auto& [id, list] : after[0].kinds)
			unlockedAfter += list.size();
		test::require(unlockedAfter > unlockedBefore, "the completion unlocked something (" + std::to_string(unlockedBefore) + " -> " + std::to_string(unlockedAfter) + ")");
		test::require(client.tick(sim).empty(), "sent once, not every tick");
		std::cout << "research wire: " << start.records.size() << " records, level " << int(start.techLevel) << ", " << silent << " silent; clothing unlocked " << (unlockedAfter - unlockedBefore) << " refs\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_research_wire: FAILED: " << e.what() << "\n";
		return 1;
	}
}
