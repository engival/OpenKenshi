// The player's shackle orders (kenshi-h2v.15.1; SIMLINK order 60 actions 15 / 16 / 17, ITEM_LOCK 57, TASK_PROGRESS kinds 1 and 7, event 13):
//   - ITEM_LOCK follows the worn shackles' INVENTORY: locked, can-cut (a carried tool), can-break (strength), level, pick chance, hardness;
//   - 15 picks (201), 16 cuts (185), 17 breaks (186): the bars, the lock state after, the INVENTORY resend, event 13 on every end;
//   - the refusals: bad_value (queue 1, a flag, an action past 20 (18-20 are the cage orders, ksim_cage_wire)), bad_target (no such character), not_allowed (no locked shackles, a KO'd actor),
//     and the run-time ends: no tool and too little strength (event 13 result 1), a replaced order (5), a KO'd actor (4);
//   - the same orders on another character's shackles.
// A Session on the duel scenario (the enemy lies knocked out). argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/nav/door_plan.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
using Entry = state::Inventory::Entry;

constexpr std::uint8_t kPick = 15;
constexpr std::uint8_t kCut = 16;
constexpr std::uint8_t kBreak = 17;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

struct Lock
{
	bool seen = false;
	std::uint16_t n = 0;
	std::uint16_t index = 0;
	std::uint8_t flags = 0;
	std::uint16_t level = 0;
	float chance = 0.0f;
	float hardness = 0.0f;
	float maxHardness = 0.0f;
	std::uint32_t version = 0;
};

struct Bar
{
	std::uint8_t kind = 0;
	float progress = 0.0f;
	float rate = 0.0f;
	float value = 0.0f;
	std::uint8_t flags = 0;
	std::uint64_t target = 0;
};

struct End
{
	std::uint64_t who = 0;
	std::uint64_t target = 0;
	std::uint8_t action = 0;
	std::uint8_t result = 0;
};

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	ks::ServerContext context;
	std::unique_ptr<Simulation> sim;
	std::unique_ptr<ks::Session> session;
	ks::Duel duel;
	std::uint32_t seq = 0;
	std::map<std::uint64_t, Lock> locks;
	std::map<std::uint64_t, std::vector<std::uint8_t>> slots;
	std::map<std::uint64_t, std::vector<Bar>> bars; // the last TASK_PROGRESS: by character
	std::vector<Bar> wireBars; // every TASK_PROGRESS row ever received
	std::vector<End> ends;
	std::map<std::uint32_t, std::string> replies;
	int inventoryFrames = 0;
	std::size_t mark = 0; // ends before the last order 60 was sent: its own end may arrive within the order's two ticks

	Rig(const std::shared_ptr<const kdata::GameData>& g, const std::shared_ptr<const CombatData>& d, const std::shared_ptr<MeshNavigator>& nav,
		const kworld::LoadOrder& order)
		: gd(g)
		, data(d)
	{
		sim = std::make_unique<Simulation>(Simulation::load(KENSHI_DATA, 91));
		sim->setGameData(g);
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(data, settings);
		sim->enableNavigation(nav);
		duel = ks::spawnDuel(*sim, *gd, *data, context);
		session = std::make_unique<ks::Session>(*sim, context);
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
			if (type == ks::kInventory)
			{
				// tick u64, id u64, version u32, u16 n, n x {strid item, u32 count, u8 slot}
				const std::uint64_t id = at<std::uint64_t>(body, 8);
				const std::uint16_t n = at<std::uint16_t>(body, 20);
				slots[id].clear();
				for (std::size_t i = 0; i < n; ++i)
					slots[id].push_back(body[30 + 9 * i]);
				++inventoryFrames;
			} else if (type == ks::kItemLock) {
				// tick u64, id u64, version u32, u16 n, n x {u16 index, u8 flags, u16 level, f32 chance, f32 hardness, f32 maxHardness}
				Lock lock;
				lock.seen = true;
				lock.version = at<std::uint32_t>(body, 16);
				lock.n = at<std::uint16_t>(body, 20);
				test::require(body.size() == 22u + 17u * lock.n, "ITEM_LOCK length");
				if (lock.n == 1)
				{
					lock.index = at<std::uint16_t>(body, 22);
					lock.flags = body[24];
					lock.level = at<std::uint16_t>(body, 25);
					lock.chance = at<float>(body, 27);
					lock.hardness = at<float>(body, 31);
					lock.maxHardness = at<float>(body, 35);
				}
				locks[at<std::uint64_t>(body, 8)] = lock;
			} else if (type == ks::kTaskProgress) {
				const std::uint16_t n = at<std::uint16_t>(body, 8);
				test::require(body.size() == 10u + 21u * n + n + 8u * n, "TASK_PROGRESS length (rows, then flags, then targets)");
				bars.clear();
				for (std::size_t i = 0; i < n; ++i)
				{
					const std::size_t o = 10 + 21 * i;
					const std::size_t tail = 10 + 21u * n;
					const Bar bar{body[o + 8], at<float>(body, o + 9), at<float>(body, o + 13), at<float>(body, o + 17), body[tail + i], at<std::uint64_t>(body, tail + n + 8 * i)};
					bars[at<std::uint64_t>(body, o)].push_back(bar);
					wireBars.push_back(bar);
				}
			} else if (type == ks::kEvent) {
				const std::uint16_t kind = at<std::uint16_t>(body, 8);
				if (kind == 9)
					replies[at<std::uint32_t>(body, 10)] = "";
				else if (kind == 10)
					replies[at<std::uint32_t>(body, 10)] = session->strings().text(at<std::uint32_t>(body, 14));
				else if (kind == 13)
					ends.push_back({at<std::uint64_t>(body, 10), at<std::uint64_t>(body, 18), body[26], body[27]});
			}
		}
	}

	void run(int ticks)
	{
		for (int i = 0; i < ticks; ++i)
		{
			sim->tick();
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			parse(bytes);
		}
	}

	// Runs until an event 13 for `action` arrives, at most `maxTicks`; the sim's bars seen on the way go to `seen`.
	bool runToEnd(std::uint8_t action, int maxTicks, std::vector<Bar>* seen = nullptr, std::uint64_t who = 0)
	{
		const std::size_t before = mark;
		for (int i = -1; i < maxTicks; ++i)
		{
			if (i >= 0)
				run(1);
			// The wire sends the list on connect and on change only (the client extrapolates), so the rise is read from the sim's rows.
			if (seen != nullptr)
				for (const Simulation::TaskProgressRow& row : sim->taskProgress())
					if (row.id == who)
						seen->push_back({row.kind, row.progress, row.rate, row.value, row.flags, row.target});
			for (std::size_t k = before; k < ends.size(); ++k)
				if (ends[k].action == action)
					return true;
		}
		return false;
	}

	std::string order(std::uint16_t kind, const std::function<void(ks::BinaryWriter&)>& body)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", kind);
		body(w);
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
	// Order 60 on `target`; a flag byte is sent only when given.
	std::string interact(std::uint64_t who, std::uint64_t target, std::uint8_t action, std::uint8_t queue = 0, int flags = -1)
	{
		mark = ends.size();
		return order(60, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", who);
			w.u64("target", target);
			w.u8("action", action);
			w.u8("queue", queue);
			if (flags >= 0)
				w.u8("flags", static_cast<std::uint8_t>(flags));
		});
	}

	flecs::entity entityWith(std::uint64_t id)
	{
		flecs::entity found;
		sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == id) found = e; });
		return found;
	}
	state::Fighter& fighter(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
	void put(std::uint64_t id, const std::string& sid, std::uint8_t slot)
	{
		flecs::entity e = entityWith(id);
		state::Inventory inv = *e.get<state::Inventory>();
		Entry entry;
		entry.itemSid = sid;
		entry.count = 1;
		entry.slot = slot;
		inv.items.push_back(std::move(entry));
		++inv.version;
		e.set<state::Inventory>(std::move(inv));
	}
	void shackle(std::uint64_t id) { put(id, gd->find("SHACKLES")->stringID, state::Inventory::kArmourBase + 9); }
	void unshackle(std::uint64_t id)
	{
		sim->removeShackles(id);
		run(2);
	}
	std::size_t shackleIndex(std::uint64_t id)
	{
		const std::vector<std::uint8_t>& s = slots[id];
		return static_cast<std::size_t>(std::find(s.begin(), s.end(), state::Inventory::kArmourBase + 9) - s.begin());
	}
};

}

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_shackle_orders HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		std::string tool;
		for (const kdata::Record* record : gd->byType(kworld::itemTypeByName("ITEM")))
			if (tool.empty() && record->intOr("item function", 0) == 9 && data->item(record->stringID) != nullptr)
				tool = record->stringID;
		test::require(!tool.empty(), "no TOOL item");

		Rig rig(gd, data, nav, order);
		const std::uint64_t self = rig.duel.player;
		const std::uint64_t other = rig.duel.enemy;
		{
			state::Fighter& body = rig.fighter(other);
			body.unconscious = true;
			body.koTimer = 1e6;
			body.hold = true;
		}
		state::Fighter& me = rig.fighter(self);
		me.skills.lockpicking = 100.0;
		me.skills.strength = 10.0;
		rig.run(2);

		// ---- ITEM_LOCK: an empty list with no shackles, then the lock state of worn ones ----
		test::require(rig.locks[self].seen && rig.locks[self].n == 0, "ITEM_LOCK is not sent empty for a player character with no shackles");
		rig.shackle(self);
		rig.run(2);
		{
			const Lock& l = rig.locks[self];
			test::require(l.n == 1 && l.index == rig.shackleIndex(self) && (l.flags & 1) == 1, "ITEM_LOCK: the worn shackles are not locked on the wire");
			test::require((l.flags & 2) == 0 && (l.flags & 4) == 0, "ITEM_LOCK: can-cut / can-break set with no tool and strength 10");
			test::require(l.level == 20 && l.maxHardness == 20.0f && l.hardness == 20.0f, "ITEM_LOCK: level / hardness are not the shackle lock's");
			test::require(std::fabs(l.chance - static_cast<float>(pickChance(20.0, 100.0))) < 1e-6f, "ITEM_LOCK: the pick chance is not pickChance(level, lockpicking)");
		}
		rig.put(self, tool, state::Inventory::kCarried);
		me.skills.strength = 100.0;
		rig.run(2);
		test::require((rig.locks[self].flags & 6) == 6, "ITEM_LOCK: can-cut and can-break not set with a tool and strength 100");
		me.skills.strength = 10.0;
		std::printf("ok  ITEM_LOCK: empty with no shackles; locked, level 20, hardness 20 (live rwh62 section 7), the pick chance, can-cut / can-break follow the tool and strength\n");

		// ---- the order-time refusals ----
		test::require(rig.interact(self, self, kPick, 1) == "bad_value", "queue 1 was not bad_value");
		test::require(rig.interact(self, self, kPick, 0, 1) == "bad_value", "a flag was not bad_value");
		test::require(rig.interact(self, self, 21) == "bad_value", "action 21 was not bad_value");
		test::require(rig.interact(self, 987654321ull, kPick) == "bad_target", "no such character was not bad_target");
		test::require(rig.interact(self, other, kPick) == "not_allowed", "a wearer with no shackles was not not_allowed");
		std::printf("ok  refusals: queue 1 / a flag / action 21 bad_value, no such character bad_target, no shackles not_allowed\n");

		// ---- 17: too little strength ends with event 13 result 1, the shackles stay locked; then enough strength breaks them ----
		test::require(rig.interact(self, self, kBreak).empty(), "the break order was refused");
		test::require(rig.runToEnd(kBreak, 40), "no event 13 for the weak break");
		test::require(rig.ends.back().result == 1 && rig.ends.back().who == self && rig.ends.back().target == self, "the weak break did not end refused (1)");
		test::require(rig.sim->shacklesLocked(self), "the weak break opened the shackles");
		me.skills.strength = 100.0;
		const int framesBefore = rig.inventoryFrames;
		test::require(rig.interact(self, self, kBreak).empty(), "the strong break order was refused");
		test::require(rig.runToEnd(kBreak, 40) && rig.ends.back().result == 0, "the strong break did not end done (0)");
		test::require(rig.sim->shacklesWorn(self) && !rig.sim->shacklesLocked(self), "the break did not leave the shackles worn and open");
		rig.run(2);
		test::require((rig.locks[self].flags & 1) == 0 && rig.inventoryFrames > framesBefore, "the open lock did not reach the wire with a new INVENTORY");
		test::require(rig.interact(self, self, kPick) == "not_allowed", "picking open shackles was not not_allowed");
		std::printf("ok  17: strength 10 ends refused, strength 100 breaks them (worn, open, resent); open shackles refuse every action\n");
		rig.unshackle(self);
		test::require(!rig.sim->shacklesWorn(self) && rig.locks[self].n == 0, "removing the shackles left an ITEM_LOCK entry");

		// ---- 15: the pick: kind 1 bar, chance on it, event 13 done, the shackles stay on, open ----
		rig.shackle(self);
		rig.run(2);
		test::require(rig.interact(self, self, kPick).empty(), "the pick order was refused");
		std::vector<Bar> seen;
		test::require(rig.runToEnd(kPick, 1200, &seen, self) && rig.ends.back().result == 0, "the pick never ended done");
		float top = 0.0f;
		for (const Bar& b : seen)
		{
			test::require(b.target == self && (b.flags & Simulation::kProgressOriginal) != 0, "the pick row has no wearer target or original flag");
			test::require(b.kind == 1 && std::fabs(b.value - static_cast<float>(pickChance(20.0, 100.0))) < 1e-6f && b.rate > 0.0f, "the pick bar is not kind 1 with the chance");
			top = std::max(top, b.progress);
		}
		test::require(!seen.empty() && top > 0.0f, "the sim had no pick bar");
		test::require(std::any_of(rig.wireBars.begin(), rig.wireBars.end(), [](const Bar& b) { return b.kind == 1; }), "no kind 1 row reached the wire");
		test::require(std::any_of(rig.wireBars.begin(), rig.wireBars.end(), [&](const Bar& b) { return b.kind == 1 && b.target == self && (b.flags & 1) != 0; }), "the kind 1 wire row has no wearer target or flag");
		test::require(rig.sim->shacklesWorn(self) && !rig.sim->shacklesLocked(self), "the pick did not leave the shackles worn and open");
		rig.run(2);
		test::require(rig.bars[self].empty(), "a bar outlived the pick");
		std::printf("ok  15: a kind 1 bar with the chance (peak %.2f), event 13 done, the shackles worn and open\n", static_cast<double>(top));
		rig.unshackle(self);

		// ---- 16: the cut: no tool refuses; with a tool a kind 7 bar rises, the shackles are removed, event 13 done ----
		rig.shackle(self);
		{
			flecs::entity e = rig.entityWith(self);
			state::Inventory inv = *e.get<state::Inventory>();
			inv.items.erase(std::remove_if(inv.items.begin(), inv.items.end(), [&](const Entry& x) { return x.itemSid == tool; }), inv.items.end());
			++inv.version;
			e.set<state::Inventory>(std::move(inv));
		}
		rig.run(2);
		test::require(rig.interact(self, self, kCut).empty(), "the cut order was refused");
		test::require(rig.runToEnd(kCut, 40) && rig.ends.back().result == 1 && rig.sim->shacklesLocked(self), "a cut with no tool did not end refused (1)");
		rig.put(self, tool, state::Inventory::kCarried);
		test::require(rig.interact(self, self, kCut).empty(), "the cut order with a tool was refused");
		seen.clear();
		test::require(rig.runToEnd(kCut, 3000, &seen, self) && rig.ends.back().result == 0, "the cut never ended done");
		float first = 2.0f;
		float last = -1.0f;
		for (const Bar& b : seen)
		{
			test::require(b.target == self && (b.flags & Simulation::kProgressOriginal) != 0 && b.value >= 0.0f && b.value <= 20.0f && std::fabs(b.value - 20.0f * (1.0f - b.progress)) < 0.01f,
				"the cut row: target the wearer, flag bit 0, value = the hardness left");
			test::require(b.kind == 7 && b.rate > 0.0f && b.progress >= 0.0f && b.progress <= 1.0f, "the cut bar is not kind 7");
			first = std::min(first, b.progress);
			last = std::max(last, b.progress);
		}
		test::require(seen.size() >= 2 && last > first && last > 0.5f, "the cut bar did not rise");
		test::require(std::any_of(rig.wireBars.begin(), rig.wireBars.end(), [](const Bar& b) { return b.kind == 7; }), "no kind 7 row reached the wire");
		test::require(!rig.sim->shacklesWorn(self), "the cut left the shackles on");
		rig.run(2);
		test::require(rig.locks[self].n == 0 && rig.bars[self].empty(), "the cut left an ITEM_LOCK entry or a bar");
		std::printf("ok  16: no tool ends refused; with one a kind 7 bar rises %.2f -> %.2f, the shackles are gone, event 13 done\n", static_cast<double>(first), static_cast<double>(last));

		// ---- another character's shackles: a KO'd wearer next to the actor ----
		{
			state::Fighter& w = rig.fighter(other);
			me.x = w.x + 1.5;
			me.z = w.z;
			me.y = w.y;
		}
		rig.shackle(other);
		rig.run(2);
		const std::size_t freed = rig.ends.size();
		test::require(rig.interact(self, other, kBreak).empty(), "breaking another's shackles was refused");
		test::require(rig.runToEnd(kBreak, 40) && rig.ends.back().result == 0 && rig.ends.back().target == other, "the break on another's shackles did not end done on the wearer");
		test::require(rig.ends.size() > freed && !rig.sim->shacklesLocked(other), "the wearer's shackles are still locked");
		rig.unshackle(other);
		std::printf("ok  17 on another character's shackles: event 13 names the wearer\n");

		// ---- a replacing order ends the running one with result 5; a KO'd actor ends it with 4 ----
		me.skills.strength = 100.0;
		rig.shackle(self);
		rig.run(2);
		test::require(rig.interact(self, self, kCut).empty(), "the cut order was refused");
		rig.run(5);
		test::require(rig.interact(self, self, kPick).empty(), "the replacing order was refused");
		bool cancelled = false;
		for (const End& e : rig.ends)
			cancelled = cancelled || (e.action == kCut && e.result == 5);
		test::require(cancelled, "the replaced cut did not end cancelled (5)");
		rig.run(5);
		me.unconscious = true;
		me.koTimer = 1e6;
		test::require(rig.runToEnd(kPick, 10) && rig.ends.back().result == 4, "a KO'd actor did not end the pick with 4");
		test::require(rig.interact(self, self, kPick) == "not_allowed", "a KO'd actor's order was not not_allowed");
		std::printf("ok  a replacing order ends the running one with 5, a KO'd actor with 4, and a KO'd actor's order is not_allowed\n");

		std::printf("ksim_shackle_orders: ok\n");
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_shackle_orders: FAILED: " << e.what() << "\n";
		return 1;
	}
}
