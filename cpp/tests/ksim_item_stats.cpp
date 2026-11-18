// ITEM_STATS (docs/formats/SIMLINK.md "ITEM_STATS"): order 69 item_stats and the type 29 reply, through a Session on the Hub cache.
// The frame is decoded by hand with strict trailing-byte checks for a weapon, an armour piece (with the (g) trailing fields) and a
// kind-0 item (Bread); the numbers are compared with what CombatData builds for the same quality (what the sim fights with). Both
// refusals (bad_target, bad_value) are covered, and the reply goes to the asking session only, never into the input log.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

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

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	test::require(pos + sizeof(T) <= b.size(), "frame read past the end");
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

// A strict reader: every field is taken in order, and done() requires the body to be consumed exactly.
struct Cursor
{
	const std::vector<std::uint8_t>& b;
	std::size_t p = 0;
	template <typename T>
	T get()
	{
		const T v = at<T>(b, p);
		p += sizeof(T);
		return v;
	}
	void done(const char* what) const { test::require(p == b.size(), std::string(what) + ": trailing bytes (" + std::to_string(b.size() - p) + ")"); }
};

struct Reply
{
	std::string refusal; // "" = applied (event 9)
	bool answered = false;
	std::vector<std::vector<std::uint8_t>> stats; // type 29 bodies in this answer
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
	int statsFramesOutsideOrders = 0;

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
	}

	// Splits a byte stream into frames; collects the replies to `mine` and any type 29 body.
	void parse(const std::vector<std::uint8_t>& bytes, std::uint32_t mine, Reply& reply)
	{
		for (std::size_t p = 0; p < bytes.size();)
		{
			const std::uint32_t len = at<std::uint32_t>(bytes, p);
			const std::uint16_t type = at<std::uint16_t>(bytes, p + 4);
			test::require(p + 4 + len <= bytes.size(), "frame length past the end");
			const std::vector<std::uint8_t> body(bytes.begin() + static_cast<long>(p) + 6, bytes.begin() + static_cast<long>(p) + 4 + len);
			p += 4 + len;
			if (type == ks::kItemStats)
			{
				reply.stats.push_back(body);
			} else if (type == ks::kEvent) {
				const std::uint16_t kind = at<std::uint16_t>(body, 8);
				if (kind == 9 && at<std::uint32_t>(body, 10) == mine)
					reply.answered = true;
				else if (kind == 10 && at<std::uint32_t>(body, 10) == mine)
				{
					reply.answered = true;
					reply.refusal = session->strings().text(at<std::uint32_t>(body, 14));
				}
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
			Reply none;
			parse(bytes, 0, none);
			statsFramesOutsideOrders += static_cast<int>(none.stats.size());
		}
	}

	// Order 69 through the session; `extra` bytes are appended to the body (the strict trailing check).
	Reply itemStats(std::uint64_t id, std::uint32_t version, std::uint16_t index, int extra = 0)
	{
		std::vector<std::uint8_t> frame;
		ks::BinaryWriter w(frame);
		w.begin(ks::kOrder);
		w.u32("clientSeq", ++seq);
		w.u16("kind", 69);
		w.u64("id", id);
		w.u32("version", version);
		w.u16("index", index);
		for (int i = 0; i < extra; ++i)
			w.u8("pad", 0);
		w.end();
		std::vector<std::uint8_t> out;
		ks::BinaryWriter reply(out);
		const bool kept = session->onFrame(ks::kOrder, frame.data() + 6, frame.size() - 6, reply);
		Reply result;
		if (extra != 0)
		{
			test::require(!kept, "an order 69 with trailing bytes was accepted");
			return result;
		}
		test::require(kept, "an order 69 kicked the client");
		parse(out, seq, result);
		return result;
	}

	flecs::entity entityWith(std::uint64_t id)
	{
		flecs::entity found;
		sim->world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == id) found = e; });
		return found;
	}
	state::Inventory stored(std::uint64_t id) { return *entityWith(id).get<state::Inventory>(); }
	void setPack(std::uint64_t id, std::vector<Entry> carried)
	{
		state::Inventory inv = stored(id);
		inv.items.erase(std::remove_if(inv.items.begin(), inv.items.end(), [](const Entry& e) { return e.slot == state::Inventory::kCarried; }), inv.items.end());
		for (Entry& e : carried)
			inv.items.push_back(std::move(e));
		++inv.version;
		entityWith(id).set<state::Inventory>(std::move(inv));
		run(2);
	}
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

double number(const kdata::Record& record, const char* key, double fallback)
{
	if (const kdata::FieldFloat* field = record.findFloat(key))
		return kdata::decodeFloat(*field);
	if (const kdata::FieldInt* field = record.findInt(key))
		return field->value;
	return fallback;
}

Entry carried(const std::string& sid, const state::ItemQuality& quality, int x, int y)
{
	Entry e;
	e.itemSid = sid;
	e.count = 1;
	e.quality = quality;
	e.section = 0;
	e.x = static_cast<std::uint8_t>(x);
	e.y = static_cast<std::uint8_t>(y);
	return e;
}

std::uint16_t indexOf(const state::Inventory& inv, const std::string& sid)
{
	for (std::size_t i = 0; i < inv.items.size(); ++i)
	{
		if (inv.items[i].itemSid == sid)
			return static_cast<std::uint16_t>(i);
	}
	throw std::runtime_error("entry not found: " + sid);
}

void near(float got, double want, const char* what)
{
	if (std::fabs(static_cast<double>(got) - want) > 1e-5 + 1e-6 * std::fabs(want))
		throw test::Failure(std::string(what) + ": " + std::to_string(got) + ", wanted " + std::to_string(want));
}

// The header every kind shares: tick, id, version, index (the echo of the order), kind.
void head(Cursor& c, std::uint64_t id, std::uint32_t version, std::uint16_t index, std::uint8_t kind)
{
	c.get<std::uint64_t>(); // tick
	test::require(c.get<std::uint64_t>() == id, "ITEM_STATS id echo");
	test::require(c.get<std::uint32_t>() == version, "ITEM_STATS version echo");
	test::require(c.get<std::uint16_t>() == index, "ITEM_STATS index echo");
	test::require(c.get<std::uint8_t>() == kind, "ITEM_STATS kind");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_item_stats HUB.knav");
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		const kdata::Record& katana = *named(*gd, "WEAPON", "Katana");
		const kdata::Record& bread = *named(*gd, "ITEM", "Bread");
		// The armour piece with the most non-neutral (g) rows among those with part coverage.
		const kdata::Record* plate = nullptr;
		int bestScore = -1;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("ARMOUR")))
		{
			const kdata::RefCategory* coverage = r->findRefs("part coverage");
			if (coverage == nullptr || coverage->entries.empty())
				continue;
			const int score = (number(*r, "stealth mult", 1.0) != 1.0) + (number(*r, "athletics mult", 1.0) != 1.0)
				+ (number(*r, "combat speed mult", 1.0) != 1.0) + (number(*r, "dexterity mult", 1.0) != 1.0)
				+ (number(*r, "combat attk bonus", 0.0) != 0.0) + (number(*r, "combat def bonus", 0.0) != 0.0);
			if (score > bestScore)
			{
				bestScore = score;
				plate = r;
			}
		}
		test::require(plate != nullptr, "no armour with part coverage");
		const state::ItemQuality qWeapon = data->rollQuality(*gd, katana, 0.75, 0.0);
		const state::ItemQuality qArmour = data->rollQuality(*gd, *plate, 0.5, 0.5);
		test::require(qWeapon.set() && qArmour.set(), "the rolled qualities are unset");

		Rig rig(gd, data, nav, order);
		const std::uint64_t self = rig.duel.player;
		{
			// The duel's enemy would attack: it lies knocked out (and is not the session's, so its inventory is not visible).
			state::Fighter& body = *const_cast<CombatArena*>(rig.sim->combat())->find(rig.duel.enemy);
			body.unconscious = true;
			body.koTimer = 1e6;
			body.hold = true;
		}
		rig.setPack(self, {carried(katana.stringID, qWeapon, 0, 0), carried(plate->stringID, qArmour, 0, 1), carried(bread.stringID, {}, 7, 9)});
		const state::Inventory pack = rig.sim->settledInventory(self);
		const std::uint32_t version = pack.version;
		const std::size_t logged = rig.context.orderLog.size();
		const int outside = rig.statsFramesOutsideOrders;

		// ---- weapon ----
		{
			const std::uint16_t index = indexOf(pack, katana.stringID);
			const Reply reply = rig.itemStats(self, version, index);
			test::require(reply.answered && reply.refusal.empty() && reply.stats.size() == 1, "the weapon order was not answered with one ITEM_STATS and event 9");
			const state::FighterWeapon want = data->buildWeapon(*gd, katana, qWeapon);
			Cursor c{reply.stats[0]};
			head(c, self, version, index, 2);
			near(c.get<float>(), want.cut1, "cutDamage");
			near(c.get<float>(), want.blunt1, "bluntDamage");
			near(c.get<float>(), want.cutMult, "cutMult");
			near(c.get<float>(), want.bluntMult, "bluntMult");
			near(c.get<float>(), want.bleedMult, "bleed");
			near(c.get<float>(), want.armourPenetration, "armourPenetration");
			near(c.get<float>(), want.attackMod, "attackMod");
			near(c.get<float>(), want.defenceMod, "defenceMod");
			near(c.get<float>(), want.weight, "weight");
			near(c.get<float>(), want.length, "length");
			test::require(c.get<std::uint8_t>() == want.weaponType, "weaponType");
			// the Book's item+0x1f0 for this maker and level (s72 section 2a), not the record's `value`: ksim_gear_price pins the numbers
			near(c.get<float>(), trade::Book(*gd, 0).gearPrice(katana, qWeapon.maker, static_cast<int>(std::lround(qWeapon.level * 100.0))), "value");
			near(c.get<float>(), -1.0, "sellValue");
			test::require(c.get<std::uint8_t>() == qWeapon.grade, "grade");
			near(c.get<float>(), qWeapon.level, "level");
			c.done("weapon ITEM_STATS");
			test::require(want.cut1 > 0.0 && want.weight > 0.0, "the Katana's numbers are zero: the test checks nothing");
		}

		// ---- armour, with the (g) trailing fields ----
		{
			const std::uint16_t index = indexOf(pack, plate->stringID);
			const Reply reply = rig.itemStats(self, version, index);
			test::require(reply.answered && reply.refusal.empty() && reply.stats.size() == 1, "the armour order was not answered with one ITEM_STATS and event 9");
			const state::FighterArmour want = data->buildArmour(*plate, qArmour);
			Cursor c{reply.stats[0]};
			head(c, self, version, index, 1);
			near(c.get<float>(), want.blunt, "blunt");
			near(c.get<float>(), want.cut, "cut");
			near(c.get<float>(), want.cutIntoStun, "cutEfficiency");
			near(c.get<float>(), want.pierce, "harpoon");
			const std::uint8_t n = c.get<std::uint8_t>();
			test::require(n == want.coverage.size() && n > 0, "nCoverage");
			for (std::size_t i = 0; i < n; ++i)
			{
				test::require(rig.session->strings().text(c.get<std::uint32_t>()) == want.coverage[i].part, "coverage part name");
				near(c.get<float>(), want.coverage[i].coverage, "coverage fraction");
			}
			near(c.get<float>(), number(*plate, "weight kg", 0.0), "weight");
			near(c.get<float>(), want.combatSpeedMult, "combatSpeedMult");
			near(c.get<float>(), want.dodgeMult, "dodgeMult");
			near(c.get<float>(), want.damageOutputMult, "damageOutputMult");
			near(c.get<float>(), want.attackBonus, "attackBonus");
			near(c.get<float>(), want.defenceBonus, "defenceBonus");
			near(c.get<float>(), want.unarmedBonus, "unarmedBonus");
			test::require(c.get<std::uint8_t>() == std::clamp(plate->intOr("class", 0), 0, 3), "armourClass");
			test::require(c.get<std::uint8_t>() == std::clamp(plate->intOr("material type", 0), 0, 3), "material");
			// (g): stealth (clamped < 0 -> 1), athletics, combat speed, dexterity, then the two i32 melee bonuses.
			const double stealth = number(*plate, "stealth mult", 1.0);
			near(c.get<float>(), stealth < 0.0 ? 1.0 : stealth, "stealthMult");
			near(c.get<float>(), number(*plate, "athletics mult", 1.0), "athleticsMult");
			near(c.get<float>(), want.combatSpeedMult, "combatSpeedMult (g)");
			near(c.get<float>(), number(*plate, "dexterity mult", 1.0), "dexterityMult");
			test::require(c.get<std::int32_t>() == static_cast<std::int32_t>(number(*plate, "combat attk bonus", 0.0)), "meleeAttack");
			test::require(c.get<std::int32_t>() == static_cast<std::int32_t>(number(*plate, "combat def bonus", 0.0)), "meleeDefence");
			near(c.get<float>(), trade::Book(*gd, 0).gearPrice(*plate, std::string(), 0), "value"); // item+0x1f0 of spawned armour (s72 section 2c)
			near(c.get<float>(), -1.0, "sellValue");
			test::require(c.get<std::uint8_t>() == qArmour.grade, "grade");
			near(c.get<float>(), qArmour.level, "level");
			c.done("armour ITEM_STATS");
			std::printf("armour %s: %d non-neutral (g) rows\n", plate->name.c_str(), bestScore);
		}

		// ---- kind 0: answered, not refused; the common block only ----
		{
			const std::uint16_t index = indexOf(pack, bread.stringID);
			const Reply reply = rig.itemStats(self, version, index);
			test::require(reply.answered && reply.refusal.empty() && reply.stats.size() == 1, "the kind-0 order was not answered with one ITEM_STATS and event 9");
			Cursor c{reply.stats[0]};
			head(c, self, version, index, 0);
			near(c.get<float>(), bread.intOr("value", 0), "value");
			near(c.get<float>(), -1.0, "sellValue");
			test::require(c.get<std::uint8_t>() == state::ItemQuality::kNoGrade, "a plain item's grade is not 255");
			near(c.get<float>(), -1.0, "level");
			c.done("kind-0 ITEM_STATS");
		}

		// ---- refusals ----
		{
			const std::uint16_t index = indexOf(pack, katana.stringID);
			const auto refused = [&](const Reply& r, const char* reason, const char* what)
			{
				test::require(r.answered && r.refusal == reason && r.stats.empty(), std::string(what) + ": answered '" + r.refusal + "', wanted '" + reason + "'");
			};
			refused(rig.itemStats(0xdeadbeefull, version, 0), "bad_target", "no such id");
			refused(rig.itemStats(rig.duel.enemy, rig.stored(rig.duel.enemy).version, 0), "bad_target", "a body the session did not open");
			refused(rig.itemStats(self, version - 1, index), "bad_value", "a stale version");
			refused(rig.itemStats(self, version + 1, index), "bad_value", "a future version");
			refused(rig.itemStats(self, version, static_cast<std::uint16_t>(pack.items.size())), "bad_value", "an index past the end");
			refused(rig.itemStats(self, version, 0xffff), "bad_value", "the largest index");
			// A container in view is addressable.
			std::uint64_t box = 0;
			rig.sim->world().ecs().each([&](const state::Id& i, const state::Container&) { box = i.value; });
			test::require(box != 0, "the duel scenario placed no container");
			const Reply boxed = rig.itemStats(box, rig.stored(box).version, 0xfff0);
			test::require(boxed.answered && (boxed.refusal == "bad_value" || boxed.refusal == "bad_target"), "a container order was not answered");
		}

		// ---- never broadcast, never logged ----
		rig.run(3);
		test::require(rig.statsFramesOutsideOrders == outside, "an ITEM_STATS frame was pushed outside an order's answer");
		test::require(rig.context.orderLog.size() == logged, "order 69 was written to the input log");

		// Trailing bytes are a malformed frame, not an answer (last: the session is refused afterwards).
		test::require(rig.itemStats(self, version, indexOf(pack, katana.stringID), 1).stats.empty(), "a malformed order 69 got a reply");

		std::cout << "ksim_item_stats ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
