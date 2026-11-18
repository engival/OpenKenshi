// The grid inventory (docs/formats/SIMLINK.md sections 6 and 7, docs/reference/INVENTORY_GRID_STATIC.md): autoplace order, stack caps
// (a bag's bonus), the merge key, the no-room refusal, move / merge / occupied / stale, arrange (a messy grid, and the case where no
// sort fits so the grid stays), container grids from the data, entry-addressed orders on two instances of one record, a
// transfer by entry, save + load (an old save without positions is placed), and the ITEM_GRID frame through a Session.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/economy/inventory_grid.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;
using Entry = state::Inventory::Entry;

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

// What the client holds after INVENTORY + ITEM_GRID (parsed from the frames).
struct Wire
{
	struct E
	{
		std::string item;
		std::uint32_t count = 0;
		std::uint8_t slot = 0;
		std::uint8_t section = 255;
		std::uint8_t x = 0;
		std::uint8_t y = 0;
		std::string material;
		std::string colour;
	};
	struct G
	{
		std::uint8_t section, w, h;
	};
	std::uint32_t version = 0;
	std::vector<E> items;
	std::vector<G> grids;
	std::uint32_t gridVersion = 0;
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
	std::map<std::uint64_t, Wire> wire;
	std::map<std::uint32_t, std::string> replies; // clientSeq -> "" applied, else the reason

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
				Wire& w = wire[at<std::uint64_t>(body, 8)];
				w = Wire{};
				w.version = at<std::uint32_t>(body, 16);
				const std::uint16_t n = at<std::uint16_t>(body, 20);
				test::require(body.size() == 22u + 9u * n, "INVENTORY length");
				for (std::size_t i = 0; i < n; ++i)
				{
					Wire::E e;
					e.item = session->strings().text(at<std::uint32_t>(body, 22 + 9 * i));
					e.count = at<std::uint32_t>(body, 26 + 9 * i);
					e.slot = body[30 + 9 * i];
					w.items.push_back(e);
				}
			} else if (type == ks::kItemGrid) {
				// tick u64, id u64, version u32, u8 ns, ns x {u8 section, u8 w, u8 h}, u16 n, n x {u16 index, u8 section, u8 x, u8 y, strid material, strid colour}
				Wire& w = wire[at<std::uint64_t>(body, 8)];
				test::require(w.version == at<std::uint32_t>(body, 16), "ITEM_GRID does not follow an INVENTORY of its version");
				w.gridVersion = w.version;
				const std::size_t ns = body[20];
				for (std::size_t i = 0; i < ns; ++i)
					w.grids.push_back({body[21 + 3 * i], body[22 + 3 * i], body[23 + 3 * i]});
				const std::size_t head = 21 + 3 * ns;
				const std::uint16_t n = at<std::uint16_t>(body, head);
				test::require(n == w.items.size() && body.size() == head + 2 + 13u * n, "ITEM_GRID length");
				for (std::size_t i = 0; i < n; ++i)
				{
					const std::size_t o = head + 2 + 13 * i;
					Wire::E& e = w.items.at(at<std::uint16_t>(body, o));
					e.section = body[o + 2];
					e.x = body[o + 3];
					e.y = body[o + 4];
					const std::uint32_t material = at<std::uint32_t>(body, o + 5);
					const std::uint32_t colour = at<std::uint32_t>(body, o + 9);
					e.material = material == 0 ? std::string() : session->strings().text(material);
					e.colour = colour == 0 ? std::string() : session->strings().text(colour);
				}
			} else if (type == ks::kEvent) {
				const std::uint16_t kind = at<std::uint16_t>(body, 8);
				if (kind == 9)
					replies[at<std::uint32_t>(body, 10)] = "";
				else if (kind == 10)
					replies[at<std::uint32_t>(body, 10)] = session->strings().text(at<std::uint32_t>(body, 14));
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

	// An order frame through the session; the answer ("" = applied, else the reason) after a tick.
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
	// `count` < 0 leaves the optional trailing field out (the old payload).
	std::string move(std::uint64_t id, std::uint32_t version, std::uint16_t index, std::uint8_t section, std::uint8_t x, std::uint8_t y, int count = -1)
	{
		return order(64, [&](ks::BinaryWriter& w)
		{
			w.u64("id", id);
			w.u32("version", version);
			w.u16("index", index);
			w.u8("section", section);
			w.u8("x", x);
			w.u8("y", y);
			if (count >= 0)
				w.u16("count", static_cast<std::uint16_t>(count));
		});
	}
	std::string arrange(std::uint64_t id, std::uint8_t section)
	{
		return order(65, [&](ks::BinaryWriter& w) { w.u64("id", id); w.u8("section", section); });
	}
	std::string dropEntry(std::uint64_t id, std::uint32_t version, std::uint16_t index, std::uint16_t count)
	{
		return order(66, [&](ks::BinaryWriter& w) { w.u64("id", id); w.u32("version", version); w.u16("index", index); w.u16("count", count); });
	}
	std::string equipEntry(std::uint64_t id, std::uint32_t version, std::uint16_t index, bool on)
	{
		return order(67, [&](ks::BinaryWriter& w) { w.u64("id", id); w.u32("version", version); w.u16("index", index); w.u8("equip", on ? 1 : 0); });
	}
	// Order 63: `count` units of the record `item` between the character and the container (by record, no index).
	std::string transferByRecord(std::uint64_t id, std::uint64_t box, const std::string& item, std::uint16_t count, bool take)
	{
		return order(63, [&](ks::BinaryWriter& w)
		{
			w.u64("id", id);
			w.u64("container", box);
			w.str("item", item);
			w.u16("count", count);
			w.u8("take", take ? 1 : 0);
		});
	}
	std::string transferEntry(std::uint64_t id, std::uint64_t box, std::uint32_t version, std::uint16_t index, std::uint16_t count, bool take, std::uint8_t section,
		std::uint8_t x, std::uint8_t y)
	{
		return order(68, [&](ks::BinaryWriter& w)
		{
			w.u64("id", id);
			w.u64("container", box);
			w.u32("version", version);
			w.u16("index", index);
			w.u16("count", count);
			w.u8("take", take ? 1 : 0);
			w.u8("section", section);
			w.u8("x", x);
			w.u8("y", y);
		});
	}

	flecs::entity entityWith(std::uint64_t id)
	{
		flecs::entity found;
		sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == id) found = e; });
		return found;
	}
	state::Inventory stored(std::uint64_t id) { return *entityWith(id).get<state::Inventory>(); }
	// Replaces the carried entries of `id` (equipped ones stay) and bumps the version, so the wire resends.
	void setPack(std::uint64_t id, std::vector<Entry> carried, std::uint8_t gridW = 8, std::uint8_t gridH = 10)
	{
		state::Inventory inv = stored(id);
		inv.items.erase(std::remove_if(inv.items.begin(), inv.items.end(), [](const Entry& e) { return e.slot == state::Inventory::kCarried; }), inv.items.end());
		for (Entry& e : carried)
			inv.items.push_back(std::move(e));
		inv.gridW = gridW;
		inv.gridH = gridH;
		++inv.version;
		entityWith(id).set<state::Inventory>(std::move(inv));
		run(2);
	}
	state::Fighter& fighter(std::uint64_t id) { return *const_cast<CombatArena*>(sim->combat())->find(id); }
};

const kdata::Record* named(const kdata::GameData& gd, const char* type, const char* name)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			return r;
	}
	throw std::runtime_error(std::string("no ") + type + " " + name);
}

// The first record of the type with this footprint that never stacks (a weapon, armour, or an ITEM with stackable 1); `skip` takes later ones.
std::string withFootprint(const kdata::GameData& gd, const grid::Rules& rules, const char* type, int w, int h, int skip = 0)
{
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		const grid::Size fp = rules.footprint(r->stringID);
		if (fp.w == w && fp.h == h && rules.stackCap(r->stringID, nullptr) == 1 && skip-- == 0)
			return r->stringID;
	}
	throw std::runtime_error(std::string("no ") + type + " of that footprint");
}

Entry carriedEntry(const std::string& sid, std::uint32_t count, int x, int y, const state::ItemQuality& quality = {})
{
	Entry e;
	e.itemSid = sid;
	e.count = count;
	e.quality = quality;
	e.section = 0;
	e.x = static_cast<std::uint8_t>(x);
	e.y = static_cast<std::uint8_t>(y);
	return e;
}

const Entry* find(const state::Inventory& inv, const std::string& sid, std::size_t nth = 0)
{
	for (const Entry& e : inv.items)
	{
		if (e.itemSid == sid && nth-- == 0)
			return &e;
	}
	return nullptr;
}

std::size_t indexOf(const state::Inventory& inv, const std::string& sid, std::size_t nth = 0)
{
	for (std::size_t i = 0; i < inv.items.size(); ++i)
	{
		if (inv.items[i].itemSid == sid && nth-- == 0)
			return i;
	}
	throw std::runtime_error("entry not found: " + sid);
}

void expect(const std::string& got, const std::string& want, const char* what)
{
	if (got != want)
		throw test::Failure(std::string(what) + ": answered '" + got + "', wanted '" + want + "'");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_inventory_grid HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const grid::Rules rules(*gd);

		const std::string katana = named(*gd, "WEAPON", "Katana")->stringID;
		const std::string bread = named(*gd, "ITEM", "Bread")->stringID;
		const std::string plating = named(*gd, "ITEM", "Armour Plating")->stringID; // stackable 4 (3 x 3 in the merged data, 4 x 3 in the base)
		const std::string leather = named(*gd, "ITEM", "Leather")->stringID; // stackable 4, 2 x 3
		const int PW = rules.footprint(plating).w;
		const int PH = rules.footprint(plating).h;
		test::require((PW == 3 || PW == 4) && PH == 3, "Armour Plating is not 3 or 4 wide and 3 high");
		const std::string tradersBag = named(*gd, "CONTAINER", "Traders Backpack Medium")->stringID; // 8 x 8, bonus min 2 mult 3
		test::require(rules.footprint(katana).w == 7 && rules.footprint(katana).h == 1 && PW > 0
			&& rules.stackCap(plating, nullptr) == 4 && rules.stackCap(bread, nullptr) == 1 && rules.stackCap(leather, nullptr) == 4,
			"footprints or stack caps are not read from the records: katana " + std::to_string(rules.footprint(katana).w) + "x" + std::to_string(rules.footprint(katana).h)
				+ " plating " + std::to_string(rules.footprint(plating).w) + "x" + std::to_string(rules.footprint(plating).h) + " cap " + std::to_string(rules.stackCap(plating, nullptr))
				+ " bread cap " + std::to_string(rules.stackCap(bread, nullptr)) + " leather cap " + std::to_string(rules.stackCap(leather, nullptr)));
		const state::ItemQuality qa = data->rollQuality(*gd, *gd->find(katana), 0.75, 0.0);
		const state::ItemQuality qb = data->rollQuality(*gd, *gd->find(katana), 0.05, 0.9);
		test::require(!(qa == qb), "two distinct Katana qualities");

		// ---- autoplace: first free cell, row by row from the top left; existing stacks fill first ----
		{
			state::Inventory inv;
			const std::string oneByOne = withFootprint(*gd, rules, "ARMOUR", 1, 1);
			Entry k;
			k.itemSid = katana;
			k.count = 1;
			test::require(grid::autoplace(inv, rules, k) == 0 && find(inv, katana)->x == 0 && find(inv, katana)->y == 0, "the first Katana is not at (0,0)");
			test::require(grid::autoplace(inv, rules, k) == 0 && find(inv, katana, 1)->x == 0 && find(inv, katana, 1)->y == 1, "the second Katana is not at (0,1)");
			Entry small;
			small.itemSid = oneByOne;
			small.count = 1;
			test::require(grid::autoplace(inv, rules, small) == 0 && find(inv, oneByOne)->x == 7 && find(inv, oneByOne)->y == 0,
				"a 1 x 1 does not take the free cell at the end of row 0 (x is the inner loop)");
			// A 4 x 3 goes below the two Katana; the cell scan finds (0,2) even though (7,1) is free.
			Entry p;
			p.itemSid = plating;
			p.count = 3;
			test::require(grid::autoplace(inv, rules, p) == 0 && find(inv, plating)->x == 0 && find(inv, plating)->y == 2 && find(inv, plating)->count == 3, "the plating is not at (0,2)");
			// Stacks: cap 4. One more fills the stack (no new cell); six more make a full stack and a stack of 2 next to it.
			p.count = 1;
			test::require(grid::autoplace(inv, rules, p) == 0 && inv.items.size() == 4 && find(inv, plating)->count == 4, "a unit did not top up the existing stack");
			p.count = 6;
			test::require(grid::autoplace(inv, rules, p) == 0, "six plating did not fit");
			test::require(find(inv, plating, 1)->count == 4 && find(inv, plating, 1)->x == PW && find(inv, plating, 1)->y == 2 && find(inv, plating, 2)->count == 2
				&& find(inv, plating, 2)->x == 0 && find(inv, plating, 2)->y == 2 + PH, "the new stacks are not 4 and 2 in scan order");
		}

		// ---- stack caps and a bag's bonus ----
		{
			Entry bag;
			bag.itemSid = tradersBag;
			bag.count = 1;
			test::require(rules.stackCap(bread, &bag) == 6 && rules.stackCap(plating, &bag) == 12 && rules.stackCap(katana, &bag) == 1,
				"the bag's stackable bonus (min 2, mult 3) is wrong: bread " + std::to_string(rules.stackCap(bread, &bag)));
			test::require(rules.bagSize(tradersBag).w == 8 && rules.bagSize(tradersBag).h == 8, "the bag's storage size");
			// INVENTORY_GRID_LIVE (b): with no room in main nothing falls back to the worn bag; named as the target it takes 14 as 12 + 2.
			state::Inventory inv;
			inv.gridW = inv.gridH = 1;
			inv.items.push_back({tradersBag, 1, state::Inventory::kBackpackSlot});
			Entry p;
			p.itemSid = plating;
			p.count = 14;
			test::require(grid::autoplace(inv, rules, p) == 14 && inv.wornBackpack()->contents.empty(), "main overflowed into the worn bag");
			test::require(grid::autoplace(inv, rules, p, 1) == 0, "14 plating did not fit the bag");
			const Entry* worn = inv.wornBackpack();
			test::require(worn != nullptr && worn->contents.size() == 2 && worn->contents[0].count == 12 && worn->contents[1].count == 2 && inv.carried(plating) == 14,
				"the bag stacks are not 12 and 2");
			// INVENTORY_GRID_LIVE (c): leather (stackable 4) is 4 in main, 12 in the bag; a whitelisted section takes base 1 (max(1, 2) x 3 = 6).
			test::require(rules.stackCap(leather, nullptr) == 4 && rules.stackCap(leather, worn) == 12 && rules.stackCap(leather, nullptr, true) == 1
				&& rules.stackCap(leather, worn, true) == 6 && rules.stackCap(katana, worn, true) == 1, "leather caps (4 main, 12 bag, whitelist base 1)");
			// A non-empty container never goes into a bag.
			Entry full;
			full.itemSid = tradersBag;
			full.count = 1;
			full.contents.push_back(carriedEntry(bread, 1, 0, 0));
			test::require(grid::autoplace(inv, rules, full) == 1, "a filled bag went into a bag");
		}

		// ---- the merge key: record + manufacturer + material, colour is not part of it ----
		{
			state::Inventory inv;
			Entry a;
			a.itemSid = plating;
			a.count = 1;
			a.material = "mat-a";
			test::require(grid::autoplace(inv, rules, a) == 0, "place a");
			Entry other = a;
			other.material = "mat-b";
			test::require(grid::autoplace(inv, rules, other) == 0 && inv.items.size() == 2, "stacks of two materials merged");
			Entry coloured = a;
			coloured.colour = "some-colour";
			test::require(grid::autoplace(inv, rules, coloured) == 0 && inv.items.size() == 2 && inv.items[0].count == 2, "a different colour kept a stack from merging");
			Entry made = a;
			made.quality.maker = "m1";
			made.quality.model = "";
			test::require(!a.sameKind(made), "a different manufacturer is the same kind");
		}

		// ---- the no-room refusal, and container grids from the data ----
		{
			state::Inventory inv;
			inv.gridW = 7;
			inv.gridH = 1;
			Entry k;
			k.itemSid = katana;
			k.count = 1;
			test::require(grid::autoplace(inv, rules, k) == 0 && grid::autoplace(inv, rules, k) == 1 && inv.items.size() == 1, "a second Katana fit a 7 x 1 grid");
			Entry big;
			big.itemSid = plating;
			big.count = 1;
			test::require(grid::autoplace(inv, rules, big) == 1, "a 4 x 3 fit a 7 x 1 grid");
		}

		// ---- INVENTORY_GRID_LIVE: spawn overflow, swap / merge on move, split, arrange ties (pure rules) ----
		{
			const std::string g11a = withFootprint(*gd, rules, "ARMOUR", 1, 1, 0);
			const std::string g11b = withFootprint(*gd, rules, "ARMOUR", 1, 1, 1);
			const std::string g11c = withFootprint(*gd, rules, "ARMOUR", 1, 1, 2);
			const std::string vest = withFootprint(*gd, rules, "ARMOUR", 4, 2);
			const auto bagged = [&](state::Inventory& inv) { inv.items.push_back({tradersBag, 1, state::Inventory::kBackpackSlot}); };
			const auto wornList = [&](state::Inventory& inv) -> std::vector<Entry>& { return inv.wornBackpack()->contents; };
			// settle: what main cannot hold is returned for the ground, never put in the worn backpack
			{
				state::Inventory inv;
				inv.gridW = inv.gridH = 1;
				bagged(inv);
				inv.items.push_back(carriedEntry(katana, 1, 0, 0, qa));
				inv.items.push_back(carriedEntry(katana, 1, 0, 0, qb));
				inv.items.push_back(carriedEntry(g11a, 1, 0, 0));
				const std::vector<Entry> lost = grid::settle(inv, rules, 77);
				test::require(lost.size() == 2 && wornList(inv).empty() && inv.items.size() == 2, "settle put overflow in the backpack or kept a Katana in a 1 x 1");
			}
			// merge on move: 4 leather from main dropped onto 10 in the bag leave 12 there and 2 at the source
			{
				state::Inventory inv;
				bagged(inv);
				wornList(inv).push_back(carriedEntry(leather, 10, 0, 0));
				inv.items.push_back(carriedEntry(leather, 4, 0, 0));
				expect(grid::moveEntry(inv, rules, 1, 1, 0, 0), "", "merge into the bag");
				test::require(wornList(inv)[0].count == 12 && inv.items.size() == 2 && inv.items[1].count == 2 && inv.items[1].x == 0 && inv.items[1].y == 0,
					"4 onto 10 did not leave 12 and 2 at the source");
				// the bag stack is full now: a further drop onto it is occupied and changes nothing
				expect(grid::moveEntry(inv, rules, 1, 1, 0, 0), "occupied", "merge onto a full stack");
				test::require(inv.items[1].count == 2 && wornList(inv)[0].count == 12, "a refused merge changed counts");
			}
			// swap inside main; the other entry lands in the moved entry's old cell
			{
				state::Inventory inv;
				inv.items.push_back(carriedEntry(vest, 1, 0, 0, qa));
				inv.items.push_back(carriedEntry(g11a, 1, 2, 5));
				expect(grid::moveEntry(inv, rules, 1, 0, 1, 1), "", "swap g11 with the vest");
				test::require(inv.items[1].x == 1 && inv.items[1].y == 1 && inv.items[0].x == 2 && inv.items[0].y == 5 && inv.items.size() == 2, "the swap did not exchange the cells");
				// a different kind of the same footprint swaps as well (not a merge)
				inv.items.push_back(carriedEntry(g11b, 1, 7, 9));
				expect(grid::moveEntry(inv, rules, 2, 0, 1, 1), "", "swap two 1 x 1");
				test::require(inv.items[2].x == 1 && inv.items[2].y == 1 && inv.items[1].x == 7 && inv.items[1].y == 9, "the 1 x 1 swap");
			}
			// swap refused: the other does not fit the old cell
			{
				state::Inventory inv;
				inv.items.push_back(carriedEntry(katana, 1, 0, 0, qa));
				inv.items.push_back(carriedEntry(g11a, 1, 7, 5));
				expect(grid::moveEntry(inv, rules, 1, 0, 0, 0), "occupied", "swap where the Katana does not fit the old cell");
				test::require(inv.items[0].x == 0 && inv.items[1].x == 7 && inv.items[1].y == 5, "a refused swap moved something");
				// ...and one that does not fit because the moved entry's new rect covers the old cell
				inv.items.clear();
				inv.items.push_back(carriedEntry(vest, 1, 0, 0, qa));
				inv.items.push_back(carriedEntry(g11a, 1, 3, 0));
				expect(grid::moveEntry(inv, rules, 1, 0, 1, 0), "", "swap of overlapping footprints needs the old cell free of the new rect");
				test::require(inv.items[0].x == 3 && inv.items[1].x == 1, "swap into a cell next to the new rect");
			}
			// 2+ overlaps: occupied
			{
				state::Inventory inv;
				inv.items.push_back(carriedEntry(g11a, 1, 0, 0));
				inv.items.push_back(carriedEntry(g11b, 1, 1, 0));
				inv.items.push_back(carriedEntry(vest, 1, 0, 4, qa));
				expect(grid::moveEntry(inv, rules, 2, 0, 0, 0), "occupied", "move onto two entries");
				test::require(inv.items[2].y == 4 && inv.items[0].x == 0 && inv.items[1].x == 1, "a refused move changed positions");
			}
			// swap across the sections: the vest goes to the bag cell of the 1 x 1
			{
				state::Inventory inv;
				bagged(inv);
				wornList(inv).push_back(carriedEntry(g11a, 1, 2, 2));
				inv.items.push_back(carriedEntry(vest, 1, 0, 0, qa));
				expect(grid::moveEntry(inv, rules, 1, 1, 2, 2), "", "swap the vest into the bag");
				test::require(inv.items.size() == 2 && inv.items[1].itemSid == g11a && inv.items[1].x == 0 && inv.items[1].y == 0 && wornList(inv).size() == 1
					&& wornList(inv)[0].itemSid == vest && wornList(inv)[0].x == 2 && wornList(inv)[0].y == 2, "the cross-section swap");
			}
			// split: optional count (0 / absent / >= entry = whole stack)
			{
				state::Inventory inv;
				inv.items.push_back(carriedEntry(leather, 4, 0, 0));
				expect(grid::moveEntry(inv, rules, 0, 0, 4, 0, 1), "", "split 1 to a free rect");
				test::require(inv.items.size() == 2 && inv.items[0].count == 3 && inv.items[1].count == 1 && inv.items[1].x == 4, "the split did not leave 3 and 1");
				expect(grid::moveEntry(inv, rules, 0, 0, 4, 0, 2), "", "split 2 onto the 1 (merge)");
				test::require(inv.items[0].count == 1 && inv.items[1].count == 3, "the split merge");
				expect(grid::moveEntry(inv, rules, 1, 0, 5, 0, 1), "occupied", "split onto its own source");
				expect(grid::moveEntry(inv, rules, 1, 0, 2, 3, 1), "", "split another 1");
				inv.items.push_back(carriedEntry(g11a, 1, 4, 6));
				expect(grid::moveEntry(inv, rules, 1, 0, 4, 6, 1), "occupied", "split onto another kind");
				expect(grid::moveEntry(inv, rules, 1, 0, 7, 9, 1), "no_room", "split partly outside the grid");
				test::require(inv.items[1].count == 2 && inv.items[0].count == 1 && inv.items.size() == 4, "a refused split changed counts");
			}
			// arrange: ties keep the old column-major (x, then y) order; the original arranges main only, the backpack stays as it is
			{
				state::Inventory inv;
				bagged(inv);
				wornList(inv).push_back(carriedEntry(g11a, 1, 6, 6));
				inv.items.push_back(carriedEntry(g11a, 1, 5, 5));
				inv.items.push_back(carriedEntry(g11b, 1, 0, 3));
				inv.items.push_back(carriedEntry(g11c, 1, 0, 1));
				bool changed = false;
				expect(grid::arrange(inv, rules, 0, &changed), "", "arrange main");
				test::require(changed, "arrange of a loose main changed nothing");
				const auto at = [&](const std::string& sid) -> const Entry& { return *find(inv, sid); };
				test::require(at(g11c).x == 0 && at(g11c).y == 0 && at(g11b).x == 1 && at(g11b).y == 0 && at(g11a).x == 2 && at(g11a).y == 0, "arrange did not keep the x-then-y order for ties");
				test::require(wornList(inv)[0].x == 6 && wornList(inv)[0].y == 6, "arranging main moved the backpack's contents");
				expect(grid::arrange(inv, rules, 1, &changed), "", "arrange the backpack (our extension, MINE)");
				test::require(wornList(inv)[0].x == 0 && wornList(inv)[0].y == 0, "arrange of the backpack did not pack it");
			}
		}

		Rig rig(gd, data, nav, order);
		const std::uint64_t self = rig.duel.player;
		{
			// The duel's enemy would attack (equipping is refused mid-swing): it lies knocked out.
			state::Fighter& body = rig.fighter(rig.duel.enemy);
			body.unconscious = true;
			body.koTimer = 1e6;
			body.hold = true;
		}
		auto packState = [&] { return rig.sim->settledInventory(self); };
		auto version = [&] { return rig.stored(self).version; };

		// container grid sizes from the data
		std::vector<std::uint64_t> boxes;
		rig.sim->world().ecs().each([&](const state::Id& i, const state::Container&) { boxes.push_back(i.value); });
		test::require(!boxes.empty(), "the duel scenario placed no container");
		for (const std::uint64_t id : boxes)
		{
			const state::Inventory inv = rig.stored(id);
			std::string record;
			rig.sim->world().ecs().each([&](const state::Id& i, const state::Container& c) { if (i.value == id) record = c.recordSid; });
			const grid::Size want = rules.buildingSize(record);
			test::require(want.w > 0 && want.h > 0 && inv.gridW == want.w && inv.gridH == want.h, "a container's grid is not its BUILDING storage size");
			std::printf("container %s: grid %d x %d\n", record.c_str(), inv.gridW, inv.gridH);
		}
		// A container of another building record gets that record's size.
		{
			const kdata::Record* ore = gd->find("2939-gamedata.base");
			test::require(ore != nullptr && rules.buildingSize(ore->stringID).w == 6 && rules.buildingSize(ore->stringID).h == 4, "Storage: Ore is 6 x 4");
			state::Container box;
			box.recordSid = ore->stringID;
			const std::uint64_t id = rig.sim->spawnContainer(box, state::Inventory{}).get<state::Id>()->value;
			test::require(rig.stored(id).gridW == 6 && rig.stored(id).gridH == 4, "a spawned Storage: Ore container is not 6 x 4");
		}

		// ---- move / merge / occupied / stale ----
		{
			const std::string vest = withFootprint(*gd, rules, "ARMOUR", 4, 2);
			rig.setPack(self, {carriedEntry(katana, 1, 0, 0, qa), carriedEntry(plating, 3, 0, 1), carriedEntry(plating, 2, 4, 1), carriedEntry(vest, 1, 0, 6, qa)});
			Wire& w = rig.wire[self];
			test::require(w.gridVersion == w.version && w.grids.size() >= 1 && w.grids[0].section == 0 && w.grids[0].w == 8 && w.grids[0].h == 10, "ITEM_GRID: the main grid is not 8 x 10");
			const std::uint16_t first = static_cast<std::uint16_t>(indexOf(packState(), plating, 0));
			const std::uint16_t second = static_cast<std::uint16_t>(indexOf(packState(), plating, 1));
			const std::uint32_t v = version();
			// stale: an older version is refused, nothing changes
			expect(rig.move(self, v - 1, second, 0, 4, 6), "stale", "move at an old version");
			expect(rig.move(self, v, 99, 0, 0, 0), "bad_value", "move of an entry that is not there");
			// out of the grid / the section does not exist
			expect(rig.move(self, v, second, 0, 6, 1), "no_room", "move partly outside the grid");
			expect(rig.move(self, v, second, 1, 0, 0), "no_room", "move into a backpack nobody wears");
			// onto the Katana: occupied; onto two entries: occupied
			expect(rig.move(self, v, second, 0, 1, 0), "occupied", "move onto the Katana");
			expect(rig.move(self, v, first, 0, 3, 0), "occupied", "move onto the Katana and the other stack");
			test::require(version() == v, "a refused move changed the inventory version");
			// merge: 3 + 2 with a cap of 4 -> 4 and 1; the one left stays where it was
			expect(rig.move(self, v, second, 0, 0, 1), "", "merge onto the other stack");
			{
				const state::Inventory inv = packState();
				test::require(find(inv, plating, 0)->count == 4 && find(inv, plating, 1)->count == 1 && find(inv, plating, 1)->x == 4 && find(inv, plating, 1)->y == 1,
					"the merge did not leave 4 and the rest in place");
				test::require(version() == v + 1, "a move did not bump the version once");
			}
			// onto a full stack of the same kind: occupied
			expect(rig.move(self, v + 1, indexOf(packState(), plating, 1), 0, 0, 1), "occupied", "move onto a full stack of the same kind");
			// a plain move
			expect(rig.move(self, v + 1, indexOf(packState(), plating, 1), 0, 4, 3), "", "move to a free rect");
			{
				const Entry* moved = find(packState(), plating, 1);
				test::require(moved->x == 4 && moved->y == 3 && moved->section == 0, "the move did not land at (4,3)");
			}
			// the new state reached the client with its grid
			const Wire& after = rig.wire[self];
			test::require(after.version == version() && after.gridVersion == after.version, "the client's INVENTORY / ITEM_GRID versions");
			std::size_t seen = 0;
			for (const Wire::E& e : after.items)
				seen += e.item == plating && e.slot == 0 && e.section == 0 && e.x == 4 && e.y == 3;
			test::require(seen == 1, "the client's ITEM_GRID does not show the moved stack at (4,3)");
			// an equipped entry is not moved by order 64
			std::size_t equipped = 0;
			for (std::size_t i = 0; i < packState().items.size(); ++i)
			{
				if (packState().items[i].slot != state::Inventory::kCarried)
					equipped = i;
			}
			expect(rig.move(self, version(), static_cast<std::uint16_t>(equipped), 0, 0, 0), "not_allowed", "move of an equipped entry");
		}

		// ---- order 64 count on the wire: absent (old payload), 0, partial, full, over the entry ----
		{
			rig.setPack(self, {carriedEntry(leather, 4, 0, 0)});
			const auto leatherAt = [&](std::size_t nth) { return find(packState(), leather, nth); };
			expect(rig.move(self, version(), static_cast<std::uint16_t>(indexOf(packState(), leather)), 0, 4, 0), "", "move without count");
			test::require(packState().carried(leather) == 4 && leatherAt(0)->x == 4 && leatherAt(0)->count == 4, "the old payload did not move the whole stack");
			expect(rig.move(self, version(), static_cast<std::uint16_t>(indexOf(packState(), leather)), 0, 0, 0, 0), "", "move with count 0");
			test::require(packState().items.size() >= 1 && leatherAt(0)->count == 4 && leatherAt(0)->x == 0, "count 0 did not move the whole stack");
			expect(rig.move(self, version(), static_cast<std::uint16_t>(indexOf(packState(), leather)), 0, 4, 0, 1), "", "move with a partial count");
			test::require(leatherAt(1) != nullptr && leatherAt(0)->count == 3 && leatherAt(1)->count == 1 && leatherAt(1)->x == 4, "the partial count did not split 3 + 1");
			expect(rig.move(self, version(), static_cast<std::uint16_t>(indexOf(packState(), leather, 1)), 0, 6, 0, 1), "", "move a single unit with count 1 (full)");
			test::require(leatherAt(1)->x == 6 && leatherAt(1)->count == 1 && packState().carried(leather) == 4, "count equal to the entry did not move it whole");
			expect(rig.move(self, version(), static_cast<std::uint16_t>(indexOf(packState(), leather, 1)), 0, 4, 0, 9), "", "move with count over the entry");
			test::require(leatherAt(1)->x == 4 && leatherAt(1)->count == 1, "count over the entry did not move it whole");
			// a refused split changes nothing (and not the version)
			const std::uint32_t v = version();
			expect(rig.move(self, v, static_cast<std::uint16_t>(indexOf(packState(), leather, 0)), 0, 4, 0, 2), "", "split merging into the single unit");
			test::require(leatherAt(1)->count == 3 && leatherAt(0)->count == 1, "split merge counts");
			expect(rig.move(self, version(), static_cast<std::uint16_t>(indexOf(packState(), leather, 0)), 0, 7, 9, 1), "no_room", "split partly outside");
		}

		// ---- arrange: a messy grid is packed (first-fit decreasing); a grid no sort packs stays as it was ----
		{
			const std::string vest = withFootprint(*gd, rules, "ARMOUR", 4, 2);
			const std::string coat = withFootprint(*gd, rules, "ARMOUR", 4, 6);
			rig.setPack(self, {carriedEntry(vest, 1, 4, 6), carriedEntry(katana, 1, 0, 9, qa), carriedEntry(coat, 1, 4, 0), carriedEntry(bread, 1, 0, 3), carriedEntry(plating, 4, 0, 5)});
			const std::uint32_t v = version();
			expect(rig.arrange(self, 0), "", "arrange");
			const state::Inventory inv = packState();
			test::require(inv.version == v + 1, "arrange did not bump the version");
			// largest first: the 4 x 6 (max side 6) before the Katana (7)? max(w,h): Katana 7, coat 6, vest 4, plating 4, bread 3.
			test::require(find(inv, katana)->x == 0 && find(inv, katana)->y == 0, "the Katana (largest side) is not first at (0,0)");
			std::vector<char> cells(80, 0);
			std::size_t carried = 0;
			for (const Entry& e : inv.items)
			{
				if (e.slot != state::Inventory::kCarried)
					continue;
				++carried;
				const grid::Size fp = rules.footprint(e.itemSid);
				test::require(e.section == 0 && e.x + fp.w <= 8 && e.y + fp.h <= 10, "an arranged entry is outside the grid");
				for (int cy = e.y; cy < e.y + fp.h; ++cy)
				{
					for (int cx = e.x; cx < e.x + fp.w; ++cx)
					{
						test::require(cells[static_cast<std::size_t>(cy * 8 + cx)] == 0, "arranged entries overlap");
						cells[static_cast<std::size_t>(cy * 8 + cx)] = 1;
					}
				}
			}
			test::require(carried == 5 && inv.carried(plating) == 4, "arrange lost or merged entries");

			// The revert case: seven items packed by hand in 8 x 7 that none of the three sorts fits again.
			const std::string g33 = withFootprint(*gd, rules, "ARMOUR", 3, 3);
			const std::string g44 = withFootprint(*gd, rules, "ARMOUR", 4, 4);
			const std::string g32 = withFootprint(*gd, rules, "ITEM", 3, 2);
			const std::string g31 = withFootprint(*gd, rules, "ARMOUR", 3, 1);
			const std::string g42 = withFootprint(*gd, rules, "ARMOUR", 4, 2);
			const std::string g11 = withFootprint(*gd, rules, "ARMOUR", 1, 1);
			rig.setPack(self, {carriedEntry(g33, 1, 0, 2), carriedEntry(g44, 1, 4, 0), carriedEntry(g32, 1, 4, 4), carriedEntry(katana, 1, 0, 6, qa), carriedEntry(g31, 1, 0, 5),
				carriedEntry(g42, 1, 0, 0), carriedEntry(g11, 1, 3, 3)}, 8, 7);
			const state::Inventory before = packState();
			expect(rig.arrange(self, 0), "", "arrange of a grid no sort packs");
			const state::Inventory kept = packState();
			test::require(kept.version == before.version, "a reverted arrange changed the version");
			for (std::size_t i = 0; i < before.items.size(); ++i)
				test::require(kept.items[i].x == before.items[i].x && kept.items[i].y == before.items[i].y && kept.items[i].itemSid == before.items[i].itemSid,
					"a reverted arrange moved " + before.items[i].itemSid);
			expect(rig.arrange(self, 7), "bad_value", "arrange of a section that does not exist");
			expect(rig.arrange(self, 1), "bad_target", "arrange of a backpack nobody wears");
		}

		// ---- entry-addressed orders: two instances of one record ----
		{
			rig.setPack(self, {carriedEntry(katana, 1, 0, 0, qa), carriedEntry(katana, 1, 0, 1, qb)});
			const std::uint32_t v = version();
			const std::uint16_t second = static_cast<std::uint16_t>(indexOf(packState(), katana, 1));
			expect(rig.equipEntry(self, v, second, true), "", "equip_entry of the second Katana");
			test::require(rig.fighter(self).weapon.quality == qb, "equip_entry did not wield the second instance");
			test::require(find(packState(), katana, 0)->quality == qa && find(packState(), katana, 0)->slot == state::Inventory::kCarried, "the other instance left the pack");
			// swap: equip the first; the wielded one (qb) goes back to the pack
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(indexOf(packState(), katana, 0)), true), "", "equip_entry swap");
			{
				const state::Inventory inv = packState();
				test::require(rig.fighter(self).weapon.quality == qa, "the swap did not wield the first instance");
				std::size_t packed = 0;
				for (const Entry& e : inv.items)
					packed += e.itemSid == katana && e.slot == state::Inventory::kCarried && e.quality == qb;
				test::require(packed == 1, "the swapped-out instance lost its quality");
			}
			// The index of the Katana that is carried (true) or in hand (false); the pack order depends on the swaps.
			const auto katanaIdx = [&](bool carried)
			{
				const state::Inventory inv = packState();
				for (std::size_t i = 0; i < inv.items.size(); ++i)
				{
					if (inv.items[i].itemSid == katana && (inv.items[i].slot == state::Inventory::kCarried) == carried)
						return i;
				}
				throw std::runtime_error("no such Katana entry");
			};
			// drop_entry: exactly the qb instance lies on the ground
			const std::size_t pile = katanaIdx(true);
			expect(rig.dropEntry(self, version() - 1, static_cast<std::uint16_t>(pile), 1), "stale", "drop_entry at an old version");
			expect(rig.dropEntry(self, version(), static_cast<std::uint16_t>(pile), 2), "not_allowed", "drop_entry of more than the entry holds");
			expect(rig.dropEntry(self, version(), static_cast<std::uint16_t>(katanaIdx(false)), 1), "not_allowed", "drop_entry of the wielded one");
			std::size_t before = 0;
			rig.sim->world().ecs().each([&](const state::GroundItem& g) { before += g.itemSid == katana && g.quality == qb; });
			expect(rig.dropEntry(self, version(), static_cast<std::uint16_t>(pile), 1), "", "drop_entry");
			std::size_t after = 0;
			rig.sim->world().ecs().each([&](const state::GroundItem& g) { after += g.itemSid == katana && g.quality == qb; });
			test::require(after == before + 1 && packState().carried(katana) == 0, "drop_entry did not put the qb instance on the ground");
			// unequip by entry: back in the pack with a cell
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(katanaIdx(false)), false), "", "unequip by entry");
			test::require(rig.fighter(self).weapon.weaponType == 5 && packState().carried(katana) == 1, "unequip by entry");
			// unequip with no room: refused, nothing changes
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(katanaIdx(true)), true), "", "re-equip");
			rig.setPack(self, {carriedEntry(plating, 4, 0, 0), carriedEntry(plating, 4, 4, 0), carriedEntry(plating, 4, 0, 3), carriedEntry(plating, 4, 4, 3), carriedEntry(plating, 4, 0, 6),
				carriedEntry(plating, 4, 4, 6)}, 8, 9);
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(katanaIdx(false)), false), "no_room", "unequip into a full pack");
			test::require(rig.fighter(self).weapon.quality == qa, "a refused unequip changed the weapon");
		}

		// ---- transfer by entry, into a container's own grid ----
		{
			// A fresh, empty Storage: Ore (6 x 4) where the player stands (the scenario's own crates are filled).
			const std::uint64_t box = [&]
			{
				state::Container crate;
				crate.recordSid = "2939-gamedata.base";
				const state::Fighter& at = rig.fighter(self);
				crate.x = crate.useX = at.x;
				crate.y = crate.useY = at.y;
				crate.z = crate.useZ = at.z;
				return rig.sim->spawnContainer(crate, state::Inventory{}).get<state::Id>()->value;
			}();
			const state::Container* storage = nullptr;
			rig.sim->world().ecs().each([&](const state::Id& i, const state::Container& c) { if (i.value == box) storage = &c; });
			state::Fighter& f = rig.fighter(self);
			f.x = storage->useX + 2.0;
			f.y = storage->useY;
			f.z = storage->useZ;
			rig.run(2);
			rig.setPack(self, {carriedEntry(plating, 4, 0, 0), carriedEntry(plating, 3, 4, 0), carriedEntry(leather, 2, 0, 3)});
			const state::Inventory empty = rig.stored(box);
			const std::uint32_t pv = version();
			const std::uint32_t bv = empty.version;
			const std::uint16_t plateIx = static_cast<std::uint16_t>(indexOf(packState(), plating, 0));
			expect(rig.transferEntry(self, box, pv - 1, plateIx, 4, false, 255, 0, 0), "stale", "transfer_entry at an old version");
			expect(rig.transferEntry(self, box, pv, plateIx, 5, false, 255, 0, 0), "not_allowed", "transfer_entry of more than the stack");
			expect(rig.transferEntry(self, box, pv, plateIx, 0, false, 255, 0, 0), "bad_value", "transfer_entry of nothing");
			expect(rig.transferEntry(self, 999999, pv, 0, 4, false, 255, 0, 0), "bad_target", "transfer_entry to nothing");
			// into a chosen cell of the container's grid; a rect outside it is no_room
			expect(rig.transferEntry(self, box, pv, plateIx, 4, false, 0, static_cast<std::uint8_t>(empty.gridW - 1), 0), "no_room", "transfer_entry past the container's edge");
			expect(rig.transferEntry(self, box, pv, plateIx, 4, false, 0, 2, 0), "", "transfer_entry into a chosen cell");
			{
				const state::Inventory inv = rig.sim->settledInventory(box);
				const Entry* put = find(inv, plating);
				test::require(put != nullptr && put->count == 4 && put->x == 2 && put->y == 0 && put->section == 0 && inv.version == bv + 1, "the container did not get the stack at (2,0)");
				test::require(packState().carried(plating) == 3 && version() == pv + 1, "the pack did not lose the stack");
			}
			// onto a stack of the same kind with room: merge (3 into 4 + cap 4 has no room: no_room); a half stack of leather merges
			expect(rig.transferEntry(self, box, version(), static_cast<std::uint16_t>(indexOf(packState(), plating)), 3, false, 0, 2, 0), "no_room", "transfer_entry onto a full stack");
			// take back: autoplace into the pack
			expect(rig.transferEntry(self, box, rig.stored(box).version, 0, 4, true, 255, 0, 0), "", "take by entry");
			test::require(packState().carried(plating) == 7 && rig.sim->settledInventory(box).items.empty(), "the take did not move the stack back");
			expect(rig.transferEntry(self, box, 1, 0, 1, true, 255, 0, 0), "stale", "take at an old container version");
			// a refusal at the far end: a full container takes nothing
			rig.fighter(self).x = storage->useX + 200.0;
			rig.run(2);
			expect(rig.transferEntry(self, box, version(), 0, 1, false, 255, 0, 0), "out_of_reach", "transfer_entry from too far");
			f.x = storage->useX + 2.0;
			rig.run(2);
			// a rearrange inside the container (id = the container)
			expect(rig.transferEntry(self, box, version(), static_cast<std::uint16_t>(indexOf(packState(), leather)), 2, false, 0, 0, 0), "", "leather in");
			expect(rig.transferEntry(self, box, version(), static_cast<std::uint16_t>(indexOf(packState(), plating, 0)), 4, false, 0, static_cast<std::uint8_t>(rig.stored(box).gridW - PW), 0), "", "plating in");
			const std::uint32_t bv2 = rig.stored(box).version;
			expect(rig.move(box, bv2, 0, 0, 0, 1), "", "move inside a container");
			expect(rig.move(box, bv2, 0, 0, 0, 0), "stale", "move inside a container, old version");
			expect(rig.arrange(box, 0), "", "arrange a container");
		}

		// ---- a worn backpack: its grid, the bag's stack bonus, contents that move with it ----
		{
			rig.setPack(self, {carriedEntry(tradersBag, 1, 0, 0)});
			Entry content = carriedEntry(bread, 7, 0, 0);
			{
				state::Inventory inv = rig.stored(self);
				for (Entry& e : inv.items)
				{
					if (e.itemSid == tradersBag)
						e.contents.push_back(content); // 7 Bread, a cap of 6 in this bag: settle makes 6 + 1
				}
				++inv.version;
				rig.entityWith(self).set<state::Inventory>(std::move(inv));
				rig.run(2);
			}
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(indexOf(packState(), tradersBag)), true), "", "wear the backpack");
			const Wire& w = rig.wire[self];
			test::require(w.grids.size() == 2 && w.grids[1].section == 1 && w.grids[1].w == 8 && w.grids[1].h == 8, "ITEM_GRID: no 8 x 8 grid for the worn backpack");
			std::vector<std::uint32_t> counts;
			for (const Wire::E& e : w.items)
			{
				if (e.item == bread)
				{
					test::require(e.section == 1 && e.slot == 0, "the bag's Bread is not in section 1");
					counts.push_back(e.count);
				}
			}
			test::require(counts.size() == 2 && counts[0] == 6 && counts[1] == 1, "the bag did not cap the Bread at 6");
			// the bag's contents are addressable: move the single Bread in the bag
			const std::uint16_t single = static_cast<std::uint16_t>(w.items.size() - 1);
			expect(rig.move(self, w.version, single, 1, 5, 6), "", "move inside the backpack");
			expect(rig.arrange(self, 1), "", "arrange the backpack (our extension, MINE)");
			// take it off: it lands in main with its contents
			std::uint16_t bagIndex = 0;
			for (std::size_t i = 0; i < rig.wire[self].items.size(); ++i)
			{
				if (rig.wire[self].items[i].item == tradersBag)
					bagIndex = static_cast<std::uint16_t>(i);
			}
			expect(rig.equipEntry(self, rig.wire[self].version, bagIndex, false), "", "take the backpack off");
			const state::Inventory inv = packState();
			const Entry* off = find(inv, tradersBag);
			test::require(off != nullptr && off->slot == state::Inventory::kCarried && off->contents.size() == 2 && inv.wornBackpack() == nullptr, "the backpack's contents did not stay with it");
			test::require(rig.wire[self].grids.size() == 1, "ITEM_GRID still shows a backpack grid after it was taken off");
		}

		// ---- order 68 quick move (section 255) into a character: a free matching worn slot, then main, then the worn backpack ----
		{
			// An armour record whose slot nobody of the player's armour holds, so it can go on.
			const kdata::Record* helm = nullptr;
			for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ARMOUR")))
			{
				const std::int32_t slot = r->intOr("slot", 7);
				const auto& worn = rig.fighter(self).armour;
				if (helm == nullptr && std::none_of(worn.begin(), worn.end(), [&](const state::FighterArmour& a) { return a.slot == slot; }) && rules.footprint(r->stringID).w > 0)
					helm = r;
			}
			test::require(helm != nullptr, "no armour record for a free slot");
			const std::string helmSid = helm->stringID;
			const std::int32_t helmSlot = helm->intOr("slot", 7);
			const state::ItemQuality helmQuality = data->rollQuality(*gd, *helm, 0.5, 0.0);
			const grid::Size fp = rules.footprint(helmSid);
			const std::uint64_t box = [&]
			{
				state::Container crate;
				crate.recordSid = "2939-gamedata.base";
				const state::Fighter& at = rig.fighter(self);
				crate.x = crate.useX = at.x;
				crate.y = crate.useY = at.y;
				crate.z = crate.useZ = at.z;
				state::Inventory stuff;
				for (int i = 0; i < 3; ++i)
				{
					Entry e = carriedEntry(helmSid, 1, 0, 0, helmQuality);
					e.section = Entry::kUnplaced;
					stuff.items.push_back(e);
				}
				return rig.sim->spawnContainer(crate, stuff).get<state::Id>()->value;
			}();
			const auto wornCount = [&]
			{
				const auto& worn = rig.fighter(self).armour;
				return std::count_if(worn.begin(), worn.end(), [&](const state::FighterArmour& a) { return a.slot == helmSlot; });
			};
			const auto equippedHelm = [&]
			{
				std::size_t n = 0;
				for (const Entry& e : packState().items)
					n += e.itemSid == helmSid && e.slot == state::Inventory::kArmourBase + helmSlot;
				return n;
			};
			const auto take = [&] { return rig.transferEntry(self, box, rig.stored(box).version, 0, 1, true, 255, 0, 0); };
			rig.setPack(self, {});
			test::require(wornCount() == 0 && equippedHelm() == 0, "the slot is not free");
			// 1. the free slot: it is worn (the fighter's armour list too), not parked in main.
			expect(take(), "", "quick move into the free slot");
			test::require(wornCount() == 1 && equippedHelm() == 1 && packState().carried(helmSid) == 0, "the quick move did not wear the piece");
			// 2. the slot is taken: the next one goes to main, the worn one stays.
			expect(take(), "", "quick move with the slot taken");
			test::require(wornCount() == 1 && equippedHelm() == 1 && packState().carried(helmSid) == 1, "the quick move with the slot taken did not go to main");
			// 3. main full (exactly the piece's footprint, filled), a backpack worn: it goes to the backpack.
			rig.setPack(box, {carriedEntry(helmSid, 1, 0, 0, helmQuality)}); // the crate's grid may have dropped one of the three
			rig.setPack(self, {carriedEntry(tradersBag, 1, 0, 0)});
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(indexOf(packState(), tradersBag)), true), "", "wear the backpack");
			rig.setPack(self, {carriedEntry(helmSid, 1, 0, 0, helmQuality)}, static_cast<std::uint8_t>(fp.w), static_cast<std::uint8_t>(fp.h));
			expect(take(), "", "quick move with main full");
			{
				const state::Inventory inv = packState();
				test::require(std::count_if(inv.items.begin(), inv.items.end(), [&](const Entry& e) { return e.itemSid == helmSid && e.slot == state::Inventory::kCarried; }) == 1 && inv.wornBackpack() != nullptr && inv.wornBackpack()->contents.size() == 1
					&& inv.wornBackpack()->contents[0].itemSid == helmSid && wornCount() == 1, "the quick move with main full did not go to the backpack");
			}
			// A container destination stays main-only: the backpack is not tried, and the piece is not worn.
			rig.setPack(self, {});
			const std::uint64_t other = [&]
			{
				state::Container crate;
				crate.recordSid = [&]() -> std::string { for (const kdata::Record& r : gd->records()) if (r.name == "Small Crate" && r.boolOr("has inventory", false)) return r.stringID; throw std::runtime_error("no Small Crate"); }();
				const state::Fighter& at = rig.fighter(self);
				crate.x = crate.useX = at.x;
				crate.y = crate.useY = at.y;
				crate.z = crate.useZ = at.z;
				return rig.sim->spawnContainer(crate, state::Inventory{}).get<state::Id>()->value;
			}();
			state::Inventory stash = rig.stored(self);
			Entry carriedHelm = carriedEntry(helmSid, 1, 0, 0, helmQuality);
			stash.items.push_back(carriedHelm);
			++stash.version;
			rig.entityWith(self).set<state::Inventory>(stash);
			rig.run(2);
			expect(rig.transferEntry(self, other, version(), static_cast<std::uint16_t>(indexOf(packState(), helmSid, 1)), 1, false, 255, 0, 0), "", "quick move into a container");
			test::require(rig.sim->settledInventory(other).carried(helmSid) == 1, "a container destination did not take the piece in main");
			std::cout << "order 68 quick move: free slot wears, taken slot -> main, full main -> backpack, container main-only\n";

			// ---- order 63 (by record) takes the same quick move: free slot wears, taken slot -> main, full main -> backpack ----
			const auto take63 = [&] { return rig.transferByRecord(self, box, helmSid, 1, true); };
			// the 68 block left a helm and a backpack worn: take both off and empty the pack, so that only the quick-move path is under test
			rig.setPack(self, {});
			if (equippedHelm() != 0)
				expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(indexOf(packState(), helmSid)), false), "", "63: take the helm off");
			if (packState().wornBackpack() != nullptr)
				expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(indexOf(packState(), tradersBag)), false), "", "63: take the backpack off");
			rig.setPack(self, {});
			rig.setPack(box, {carriedEntry(helmSid, 1, 0, 0, helmQuality), carriedEntry(helmSid, 1, 2, 0, helmQuality)});
			test::require(wornCount() == 0 && equippedHelm() == 0, "63: the slot is not free");
			expect(take63(), "", "63: quick move into the free slot");
			test::require(wornCount() == 1 && equippedHelm() == 1 && packState().carried(helmSid) == 0, "63 did not wear the piece");
			expect(take63(), "", "63: quick move with the slot taken");
			test::require(wornCount() == 1 && equippedHelm() == 1 && packState().carried(helmSid) == 1, "63 with the slot taken did not go to main");
			rig.setPack(box, {carriedEntry(helmSid, 1, 0, 0, helmQuality)});
			rig.setPack(self, {carriedEntry(tradersBag, 1, 0, 0)});
			expect(rig.equipEntry(self, version(), static_cast<std::uint16_t>(indexOf(packState(), tradersBag)), true), "", "63: wear the backpack");
			rig.setPack(self, {carriedEntry(helmSid, 1, 0, 0, helmQuality)}, static_cast<std::uint8_t>(fp.w), static_cast<std::uint8_t>(fp.h));
			expect(take63(), "", "63: quick move with main full");
			{
				const state::Inventory inv = packState();
				test::require(inv.wornBackpack() != nullptr && inv.wornBackpack()->contents.size() == 1 && inv.wornBackpack()->contents[0].itemSid == helmSid && wornCount() == 1,
					"63 with main full did not go to the backpack");
			}
			// giving (take = 0) into a container stays main-only: not worn, the piece is carried in the crate
			rig.setPack(other, {});
			expect(rig.transferByRecord(self, other, helmSid, 1, false), "", "63: give into a container");
			test::require(rig.sim->settledInventory(other).carried(helmSid) == 1, "63 into a container did not land in main");
			std::cout << "order 63 quick move: free slot wears, taken slot -> main, full main -> backpack, container main-only\n";
		}

		// ---- save + load: equal hash; an old save without positions is placed ----
		{
			const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_inventory_grid.save";
			rig.sim->saveGame(path);
			const Simulation reloaded = Simulation::loadGame(KENSHI_DATA, order, path);
			test::require(reloaded.stateHash() == rig.sim->stateHash(), "save + load changed the state hash");
			std::remove(path.c_str());
			// positions missing: every carried entry unplaced, as a save from before the grid; the settled view places them all, deterministically
			const state::Inventory settledBefore = packState();
			state::Inventory old = rig.stored(self);
			for (Entry& e : old.items)
			{
				if (e.slot == state::Inventory::kCarried)
				{
					e.section = Entry::kUnplaced;
					e.x = e.y = 0;
				}
			}
			rig.entityWith(self).set<state::Inventory>(old);
			const state::Inventory placed = packState();
			test::require(placed.items.size() == settledBefore.items.size(), "placing an old inventory changed the entry count");
			std::vector<char> cells(static_cast<std::size_t>(placed.gridW * placed.gridH), 0);
			for (const Entry& e : placed.items)
			{
				if (e.slot != state::Inventory::kCarried)
					continue;
				const grid::Size fp = rules.footprint(e.itemSid);
				test::require(e.section == 0 && e.x + fp.w <= placed.gridW && e.y + fp.h <= placed.gridH, "an unplaced entry was not placed");
				for (int cy = e.y; cy < e.y + fp.h; ++cy)
				{
					for (int cx = e.x; cx < e.x + fp.w; ++cx)
					{
						test::require(cells[static_cast<std::size_t>(cy * placed.gridW + cx)] == 0, "placed entries overlap");
						cells[static_cast<std::size_t>(cy * placed.gridW + cx)] = 1;
					}
				}
			}
		}

		std::cout << "ksim_inventory_grid ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
