// The trade tests' rig: a Simulation with a Session over the Hub navmesh cache, orders sent as wire frames, the client's view collected
// (INVENTORY, ITEM_GRID, TRADE, PLAYER_SQUADS, events). Shared by ksim_trade and ksim_journey.
#pragma once

#include "ksim/core/clock.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/economy/producers.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/ai/ai.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_saves.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trade.hpp"
#include "ksim/economy/trader.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;
namespace fs = std::filesystem;

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
	bool done() const { return at == p.size(); }
};

// ---- what the client holds ----
struct Inv
{
	std::uint32_t version = 0;
	std::vector<std::tuple<std::string, std::uint32_t, std::uint8_t>> items; // item, count, slot
	std::uint32_t gridVersion = 0;
	int gridItems = 0;
};
struct Trade
{
	std::uint64_t tick = 0, trader = 0, actor = 0, stock = 0;
	std::size_t packet = 0; // the tick packet it arrived in (collect call number)
	std::string templateSid;
	std::int64_t traderMoney = 0;
	std::uint32_t stockVersion = 0, actorVersion = 0;
	struct Price
	{
		int index, buy, sell;
	};
	std::vector<Price> stockPrices;
	std::vector<std::pair<int, int>> actorPrices;
	struct Chest
	{
		std::uint64_t container = 0;
		std::uint32_t version = 0;
		std::vector<std::pair<int, int>> prices;
	};
	std::vector<Chest> chests;
	struct Buyback
	{
		int index;
		std::uint32_t count;
		int price;
	};
	std::vector<Buyback> buyback; // the stock entries holding units the actor's faction sold in this window
};
struct MenuReply
{
	std::uint32_t clientSeq = 0;
	std::uint64_t id = 0, target = 0;
	std::uint32_t entries = 0;
};

struct Ev
{
	std::uint16_t kind = 0;
	std::uint64_t a = 0, b = 0;
	std::uint8_t action = 0, result = 0;
	std::uint32_t seq = 0; // event 43: menuSeq
	std::vector<std::string> options; // event 43: the reply texts
	std::size_t packet = 0; // the tick packet it arrived in
};

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	kworld::LoadOrder order;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 100;
	std::map<std::uint64_t, Inv> inv;
	std::vector<Trade> trades;
	std::vector<Ev> events;
	std::vector<std::int64_t> money; // every PLAYER_SQUADS
	std::vector<std::uint64_t> members;
	std::vector<std::uint16_t> frameTypes; // every frame, in order
	std::vector<std::uint64_t> charStatsIds; // the character of every CHAR_STATS frame, in order
	std::string saveDir;
	SpawnOptions spawn;

	static CombatSettings settings()
	{
		CombatSettings s;
		s.substeps = 2;
		return s;
	}

	Rig(const kworld::LoadOrder& o, const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d,
		const std::shared_ptr<MeshNavigator>& n, const std::string& dir)
		: gd(g)
		, data(d)
		, nav(n)
		, order(o)
		, saveDir(dir)
	{
		spawn.seed = 7;
		spawn.settleSeconds = 0.0; // pins the t0 state: no new-game settle (TOWN_LIFE_SLICE 5.40)
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		context.saveDir = dir;
		freshSim();
	}

	void freshSim()
	{
		session.reset();
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, spawn));
		sim->setResidentsArrive(false); // the trade tests talk to the placed barman; arrival (5.34) is ksim_residents_arrive's
		sim->markPristine();
		sim->enableCombat(data, settings());
		sim->enableNavigation(nav);
	}

	void connect(bool admin)
	{
		session.reset(); // first: a Session's destructor clears the sim's observers, which the next one sets
		session = std::make_unique<ks::Session>(*sim, context);
		session->setAdmin(admin);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(context.dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
		collect(bytes);
	}

	std::string text(std::uint32_t id) { return id == 0 ? std::string() : session->strings().text(id); }
	std::uint32_t strid(const std::string& sid)
	{
		for (std::uint32_t id = 1; id <= session->strings().size(); ++id)
		{
			if (session->strings().entry(id).kind == 1 && session->strings().text(id) == sid)
				return id;
		}
		throw test::Failure("the string table has no sid " + sid);
	}

	std::size_t packets = 0;
	std::vector<MenuReply> menuReplies;
	void collect(const std::vector<std::uint8_t>& bytes)
	{
		++packets;
		for (const Frame& frame : frames(bytes))
		{
			frameTypes.push_back(frame.type);
			Cursor c{frame.payload};
			switch (frame.type)
			{
			case ks::kInventory:
			{
				c.get<std::uint64_t>();
				const std::uint64_t id = c.get<std::uint64_t>();
				Inv& v = inv[id];
				v.version = c.get<std::uint32_t>();
				v.items.clear();
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					const std::string item = text(c.get<std::uint32_t>());
					const std::uint32_t count = c.get<std::uint32_t>();
					v.items.emplace_back(item, count, c.get<std::uint8_t>());
				}
				break;
			}
			case ks::kItemGrid:
			{
				c.get<std::uint64_t>();
				const std::uint64_t id = c.get<std::uint64_t>();
				Inv& v = inv[id];
				v.gridVersion = c.get<std::uint32_t>();
				const std::uint8_t ns = c.get<std::uint8_t>();
				for (std::uint8_t i = 0; i < ns; ++i)
				{
					c.get<std::uint8_t>();
					c.get<std::uint8_t>();
					c.get<std::uint8_t>();
				}
				v.gridItems = c.get<std::uint16_t>();
				break;
			}
			case ks::kMenuEntries:
			{
				c.get<std::uint64_t>();
				MenuReply m;
				m.clientSeq = c.get<std::uint32_t>();
				m.id = c.get<std::uint64_t>();
				m.target = c.get<std::uint64_t>();
				m.entries = c.get<std::uint32_t>();
				test::require(c.done(), "MENU_ENTRIES has trailing bytes");
				menuReplies.push_back(m);
				break;
			}
			case ks::kCharStats:
			{
				c.get<std::uint64_t>();
				charStatsIds.push_back(c.get<std::uint64_t>());
				break;
			}
			case ks::kTrade:
			{
				Trade t;
				t.tick = c.get<std::uint64_t>();
				t.trader = c.get<std::uint64_t>();
				t.actor = c.get<std::uint64_t>();
				t.stock = c.get<std::uint64_t>();
				t.templateSid = text(c.get<std::uint32_t>());
				t.traderMoney = c.get<std::int64_t>();
				t.stockVersion = c.get<std::uint32_t>();
				const std::uint16_t n = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					Trade::Price p;
					p.index = c.get<std::uint16_t>();
					p.buy = c.get<std::int32_t>();
					p.sell = c.get<std::int32_t>();
					t.stockPrices.push_back(p);
				}
				t.actorVersion = c.get<std::uint32_t>();
				const std::uint16_t m = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < m; ++i)
				{
					const int index = c.get<std::uint16_t>();
					t.actorPrices.emplace_back(index, c.get<std::int32_t>());
				}
				const std::uint16_t k = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < k; ++i)
				{
					Trade::Chest chest;
					chest.container = c.get<std::uint64_t>();
					chest.version = c.get<std::uint32_t>();
					const std::uint16_t cm = c.get<std::uint16_t>();
					for (std::uint16_t j = 0; j < cm; ++j)
					{
						const int index = c.get<std::uint16_t>();
						chest.prices.emplace_back(index, c.get<std::int32_t>());
					}
					t.chests.push_back(std::move(chest));
				}
				const std::uint16_t b = c.get<std::uint16_t>();
				for (std::uint16_t i = 0; i < b; ++i)
				{
					Trade::Buyback back;
					back.index = c.get<std::uint16_t>();
					back.count = c.get<std::uint32_t>();
					back.price = c.get<std::int32_t>();
					t.buyback.push_back(back);
				}
				test::require(c.done(), "TRADE has trailing bytes");
				t.packet = packets;
				trades.push_back(std::move(t));
				break;
			}
			case ks::kPlayerSquads:
			{
				c.get<std::uint64_t>();
				c.get<std::uint64_t>();
				c.get<std::uint32_t>();
				for (int i = 0; i < 4; ++i)
					c.get<std::uint16_t>();
				const std::uint16_t n = c.get<std::uint16_t>();
				members.clear();
				for (std::uint16_t i = 0; i < n; ++i)
				{
					c.get<std::uint64_t>();
					const std::uint16_t len = c.get<std::uint16_t>();
					for (std::uint16_t k = 0; k < len; ++k)
						c.get<std::uint8_t>();
					const std::uint16_t m = c.get<std::uint16_t>();
					for (std::uint16_t k = 0; k < m; ++k)
						members.push_back(c.get<std::uint64_t>());
				}
				money.push_back(c.get<std::int64_t>());
				test::require(c.done(), "PLAYER_SQUADS has trailing bytes after money");
				break;
			}
			case ks::kEvent:
			{
				c.get<std::uint64_t>();
				Ev e;
				e.kind = c.get<std::uint16_t>();
				if (e.kind == 13)
				{
					e.a = c.get<std::uint64_t>();
					e.b = c.get<std::uint64_t>();
					e.action = c.get<std::uint8_t>();
					e.result = c.get<std::uint8_t>();
				} else if (e.kind == 11) {
					e.a = c.get<std::uint64_t>();
				} else if (e.kind == 43) { // REPLY_MENU: speaker, char, menuSeq, flags, n x {line, text}
					e.a = c.get<std::uint64_t>();
					e.b = c.get<std::uint64_t>();
					e.seq = c.get<std::uint32_t>();
					e.result = c.get<std::uint8_t>();
					const std::uint8_t n = c.get<std::uint8_t>();
					for (std::uint8_t k = 0; k < n; ++k)
					{
						c.get<std::uint32_t>();
						e.options.push_back(text(c.get<std::uint32_t>()));
					}
				} else if (e.kind == 44) { // CONVERSATION_END: speaker, char, reason
					e.a = c.get<std::uint64_t>();
					e.b = c.get<std::uint64_t>();
					e.result = c.get<std::uint8_t>();
				} else if (e.kind == 40) {
					e.a = c.get<std::uint64_t>();
					e.result = c.get<std::uint8_t>();
					test::require(c.done(), "event 40 has trailing bytes");
				} else {
					break;
				}
				e.packet = packets;
				events.push_back(e);
				break;
			}
			default:
				break;
			}
		}
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
		{
			sim->run(1);
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			collect(bytes);
		}
	}

	// Sends an order; "" when applied (event 9), else the refusal reason of event 10.
	std::string send(const Buf& body, std::uint16_t kind)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order closed the session");
		collect(bytes);
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
				return kindOf == 10 ? text(c.get<std::uint32_t>()) : std::string();
			}
		}
		throw test::Failure("an order got no answer");
	}
	std::string interact(std::uint64_t who, std::uint64_t target, std::uint8_t action, std::uint8_t queue = 0)
	{
		Buf b;
		b.put<std::uint16_t>(1).put<std::uint64_t>(who).put<std::uint64_t>(target).put<std::uint8_t>(action).put<std::uint8_t>(queue);
		return send(b, 60);
	}
	std::string talk(std::uint64_t who, std::uint64_t target) { return interact(who, target, 6); }
	// Order 99 menu_query: "" when answered (the entries are then the last of menuReplies), else the refusal.
	std::string menuQuery(std::uint64_t who, std::uint64_t target)
	{
		Buf b;
		b.put<std::uint64_t>(who).put<std::uint64_t>(target);
		return send(b, 99);
	}
	// Order 68: the character, the container, the container's INVENTORY version and index, count, take, autoplace.
	// `source` != 0 appends the optional trailing source container (a sale from it: version / index are then its own).
	std::string transfer68(std::uint64_t who, std::uint64_t box, std::uint32_t version, std::uint16_t index, std::uint16_t count, bool take,
		std::uint64_t source = 0)
	{
		Buf b;
		b.put<std::uint64_t>(who).put<std::uint64_t>(box).put<std::uint32_t>(version).put<std::uint16_t>(index).put<std::uint16_t>(count)
			.put<std::uint8_t>(take ? 1 : 0).put<std::uint8_t>(255).put<std::uint8_t>(0).put<std::uint8_t>(0);
		if (source != 0)
			b.put<std::uint64_t>(source);
		return send(b, 68);
	}
	// A storage container of `owner` holding `count` of `item` at (x, z); its id.
	std::uint64_t spawnChest(const std::string& owner, double x, double z, const std::string& item, std::uint32_t count)
	{
		state::Container box;
		box.recordSid = "chest";
		box.x = box.useX = x;
		box.z = box.useZ = z;
		box.ownerSid = owner;
		state::Inventory stock;
		stock.add(item, count);
		return sim->spawnContainer(box, stock).get<state::Id>()->value;
	}
	std::string moveTo(std::uint64_t who, double x, double z, bool queue = false, std::optional<double> y = std::nullopt)
	{
		Buf b;
		b.put<std::uint16_t>(1).put<std::uint64_t>(who).put<float>(static_cast<float>(x)).put<float>(static_cast<float>(z))
			.put<float>(static_cast<float>(y.value_or(0.0))).put<std::uint8_t>((queue ? 1 : 0) | (y ? ksim::simlink::kMoveYValid : 0));
		return send(b, 1);
	}
	std::string tradeClose() { return send(Buf{}, 86); }
	std::string transfer63(std::uint64_t who, std::uint64_t box, const std::string& item, std::uint16_t count, bool take)
	{
		Buf b;
		b.put<std::uint64_t>(who).put<std::uint64_t>(box).str(item).put<std::uint16_t>(count).put<std::uint8_t>(take ? 1 : 0);
		return send(b, 63);
	}
	std::string move64(std::uint64_t id, std::uint32_t version, std::uint16_t index)
	{
		Buf b;
		b.put<std::uint64_t>(id).put<std::uint32_t>(version).put<std::uint16_t>(index).put<std::uint8_t>(0).put<std::uint8_t>(0).put<std::uint8_t>(0);
		return send(b, 64);
	}
	std::string arrange65(std::uint64_t id)
	{
		Buf b;
		b.put<std::uint64_t>(id).put<std::uint8_t>(0);
		return send(b, 65);
	}
	std::string save(const std::string& name)
	{
		Buf b;
		b.str(name);
		return send(b, 80);
	}
	std::string load(const std::string& name)
	{
		Buf b;
		b.str(name).put<std::uint8_t>(0);
		return send(b, 81);
	}

	// order 84 on an admin session, then a fresh admin session sends NEW_GAME (n = 0) for the start-off called `name`; two ticks apply it.
	void newGame(const std::string& startName)
	{
		connect(true);
		test::require(send(Buf{}, 84).empty(), "new_world");
		std::string startSid;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == startName)
				startSid = record->stringID;
		}
		test::require(!startSid.empty(), "the data has the start-off " + startName);
		connect(true);
		members.clear();
		money.clear();
		Buf b;
		b.put<std::uint32_t>(1).str("Pilgrims").put<std::uint32_t>(strid(startSid)).put<std::uint8_t>(0);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kNewGame, b.bytes.data(), b.bytes.size(), w), "NEW_GAME closed the session");
		tick(2);
		test::require(!members.empty() && !money.empty(), "the new game put a player squad on the wire");
	}

	state::Fighter& fighter(std::uint64_t id) { return const_cast<state::Fighter&>(*sim->combat()->find(id)); }
	flecs::world& ecs() { return const_cast<flecs::world&>(sim->world().ecs()); }
	flecs::entity entityWithId(std::uint64_t id)
	{
		flecs::entity found;
		ecs().each([&](flecs::entity e, const state::Id& i) { if (i.value == id) found = e; });
		return found;
	}
	std::uint32_t carried(std::uint64_t who, const std::string& item)
	{
		const state::Inventory* v = entityWithId(who).get<state::Inventory>();
		return v != nullptr ? v->carried(item) : 0;
	}
	void give(std::uint64_t who, const std::string& item, std::uint32_t count)
	{
		flecs::entity e = entityWithId(who);
		state::Inventory v = *e.get<state::Inventory>();
		state::Inventory::Entry entry;
		entry.itemSid = item;
		entry.count = count;
		test::require(grid::autoplace(v, grid::Rules(*gd), entry) == 0, "give: no room for " + item);
		++v.version;
		e.set<state::Inventory>(std::move(v));
	}
	flecs::entity playerFaction()
	{
		flecs::entity found;
		ecs().each([&](flecs::entity e, const state::PlayerFaction&) { found = e; });
		return found;
	}
	std::int64_t wallet()
	{
		return playerFaction().get<state::PlayerFaction>()->money;
	}
	void setWallet(std::int64_t value)
	{
		flecs::entity e = playerFaction();
		state::PlayerFaction f = *e.get<state::PlayerFaction>();
		f.money = value;
		e.set<state::PlayerFaction>(std::move(f));
	}
	void teleport(std::uint64_t who, double x, double z)
	{
		state::Fighter& f = fighter(who);
		f.x = x;
		f.z = z;
		f.clearRoute();
	}
	// The wire index of the first entry with this item in an INVENTORY the client holds; -1 none.
	static int indexOf(const Inv& v, const std::string& item)
	{
		for (std::size_t i = 0; i < v.items.size(); ++i)
		{
			if (std::get<0>(v.items[i]) == item && std::get<2>(v.items[i]) == 0)
				return static_cast<int>(i);
		}
		return -1;
	}
	static std::uint32_t countIn(const Inv& v, const std::string& item)
	{
		std::uint32_t n = 0;
		for (const auto& e : v.items)
			n += std::get<0>(e) == item && std::get<2>(e) == 0 ? std::get<1>(e) : 0;
		return n;
	}
	const Trade* lastTrade() const { return trades.empty() ? nullptr : &trades.back(); }
	std::size_t countEvents(std::uint16_t kind, std::uint64_t a = 0) const
	{
		return static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [&](const Ev& e) { return e.kind == kind && (a == 0 || e.a == a); }));
	}
};

} // namespace
