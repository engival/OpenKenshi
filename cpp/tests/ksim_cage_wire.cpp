// The cage wire (kenshi-k16.5.7, client-approved 2026-11-13 on kenshi-k16.5.6; SIMLINK.md CAGES, MENU_ENTRIES bits 16-18, order 60 actions 18 / 19 / 20):
//   - CAGES (type 64) on connect: the caged prisoner of scenarios/cage_prisoner with its cage key, BUILDING record, locked, lock level and hardness; it is
//     decoded against the committed frame dump fixtures/cage_prisoner_cages.hex (CAGES, then the order 99 reply for the Wanderer on the cage);
//   - order 99 on the cage key: bits 16 (pick lock) always while locked, 17 (use tools) with a carried TOOL, 18 (use strength) when strength beats 3 x
//     hardness, and the trailing pickChance (the Wanderer's own; the caged prisoner's own is 10 skill points lower); a character id keeps bit 0 only;
//   - action 18 picks the lock open (event 13 result 0 naming the cage key, CAGES resent unlocked, the bar is kind 1 with target = the cage key),
//     19 without a tool and 20 with too little strength end result 1, 19 with a tool cuts it open (bar kind 7), 20 with strength breaks it;
//   - the refusals: action 18 on a character id is bad_target, on a building that is no cage key too, queue 1 is bad_value, an open cage is not_allowed.
// A Session on scenarios/cage_prisoner (the Heft slave shop's cage). argv[1]: the world navmesh bake (exit 77 without the Heft region). With
// KSIM_WRITE_FIXTURE=1 the fixture is rewritten instead of compared.

#include "ksim/door_plan.hpp"
#include "ksim/interact.hpp"
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "scenario_rig.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIO
#error KSIM_SCENARIO must be defined
#endif
#ifndef KSIM_FIXTURE
#error KSIM_FIXTURE must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;

constexpr std::uint64_t kHeftCage = 3893074017808827376ull; // the slave shop's placement key (ksim_cage_scenario)

template <typename T>
T at(const std::vector<std::uint8_t>& b, std::size_t pos)
{
	T v;
	std::memcpy(&v, b.data() + pos, sizeof(T));
	return v;
}

struct CageRow
{
	std::uint64_t id = 0;
	std::uint64_t cage = 0;
	std::uint32_t record = 0;
	std::uint8_t flags = 0;
	std::uint16_t level = 0;
	float hardness = 0.0f;
	float maxHardness = 0.0f;
};

struct Menu
{
	bool seen = false;
	std::uint32_t entries = 0;
	bool hasChance = false;
	float chance = 0.0f;
	std::size_t length = 0; // the body's length
};

struct Bar
{
	std::uint8_t kind = 0;
	std::uint64_t target = 0;
};

struct End
{
	std::uint64_t who = 0;
	std::uint64_t target = 0;
	std::uint8_t action = 0;
	std::uint8_t result = 0;
};

// One Session on the live scenario world, its frames decoded as they arrive (the layouts are SIMLINK.md's, read here without the sim's own writers).
struct Wire
{
	Simulation& sim;
	std::unique_ptr<ks::Session> session;
	std::uint32_t seq = 0;
	bool cagesSeen = false;
	std::vector<CageRow> cages;
	int cageFrames = 0;
	Menu menu;
	std::vector<Bar> bars; // every TASK_PROGRESS row ever received
	std::vector<End> ends;
	std::vector<std::uint8_t> rawCages; // the first CAGES frame whole (length, type, body)
	std::vector<std::uint8_t> rawMenu; // the last MENU_ENTRIES frame whole
	std::map<std::uint32_t, std::string> replies;
	std::size_t mark = 0;

	Wire(Simulation& s, ks::ServerContext& context)
		: sim(s)
	{
		session = std::make_unique<ks::Session>(sim, context);
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
			const std::vector<std::uint8_t> frame(bytes.begin() + static_cast<long>(p), bytes.begin() + static_cast<long>(p) + 4 + len);
			const std::vector<std::uint8_t> body(frame.begin() + 6, frame.end());
			p += 4 + len;
			if (type == ks::kCages)
			{
				// tick u64, u16 n, n x {u64 id, u64 cage, strid record, u8 flags, u16 level, f32 hardness, f32 maxHardness} (31 bytes each)
				const std::uint16_t n = at<std::uint16_t>(body, 8);
				test::require(body.size() == 10u + 31u * n, "CAGES length");
				cages.clear();
				for (std::size_t i = 0; i < n; ++i)
				{
					const std::size_t o = 10 + 31 * i;
					cages.push_back({at<std::uint64_t>(body, o), at<std::uint64_t>(body, o + 8), at<std::uint32_t>(body, o + 16), body[o + 20],
						at<std::uint16_t>(body, o + 21), at<float>(body, o + 23), at<float>(body, o + 27)});
				}
				if (!cagesSeen)
					rawCages = frame;
				cagesSeen = true;
				++cageFrames;
			} else if (type == ks::kMenuEntries) {
				// tick u64, u32 clientSeq, u64 id, u64 target, u32 entries, [f32 pickChance when bit 16 is set]
				menu.seen = true;
				menu.entries = at<std::uint32_t>(body, 28);
				menu.hasChance = body.size() >= 36;
				menu.chance = menu.hasChance ? at<float>(body, 32) : 0.0f;
				menu.length = body.size();
				rawMenu = frame;
			} else if (type == ks::kTaskProgress) {
				const std::uint16_t n = at<std::uint16_t>(body, 8);
				test::require(body.size() == 10u + 21u * n + n + 8u * n, "TASK_PROGRESS length (rows, then flags, then targets)");
				for (std::size_t i = 0; i < n; ++i)
					bars.push_back({body[10 + 21 * i + 8], at<std::uint64_t>(body, 10 + 21u * n + n + 8 * i)});
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
			sim.tick();
			std::vector<std::uint8_t> bytes;
			ks::BinaryWriter w(bytes);
			session->sendTick(w, false);
			parse(bytes);
		}
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
	std::string interact(std::uint64_t who, std::uint64_t target, std::uint8_t action, std::uint8_t queue = 0)
	{
		mark = ends.size();
		return order(60, [&](ks::BinaryWriter& w)
		{
			w.u16("n", 1);
			w.u64("id", who);
			w.u64("target", target);
			w.u8("action", action);
			w.u8("queue", queue);
		});
	}
	std::string menuQuery(std::uint64_t who, std::uint64_t target)
	{
		menu = {};
		return order(99, [&](ks::BinaryWriter& w)
		{
			w.u64("id", who);
			w.u64("target", target);
		});
	}
	// Runs until an event 13 for `action` arrives, at most `maxTicks`.
	const End* runToEnd(std::uint8_t action, int maxTicks)
	{
		for (int i = -1; i < maxTicks; ++i)
		{
			if (i >= 0)
				run(1);
			for (std::size_t k = mark; k < ends.size(); ++k)
				if (ends[k].action == action)
					return &ends[k];
		}
		return nullptr;
	}
};

std::string hexOf(const std::vector<std::uint8_t>& frame)
{
	static const char* digits = "0123456789abcdef";
	std::string text;
	for (const std::uint8_t byte : frame)
	{
		text += digits[byte >> 4];
		text += digits[byte & 15];
	}
	return text;
}

// The fixture: `name hex` lines, `#` comments.
std::map<std::string, std::string> readFixture(const std::string& path)
{
	std::map<std::string, std::string> lines;
	std::ifstream in(path);
	for (std::string line; std::getline(in, line);)
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::istringstream parts(line);
		std::string name;
		std::string hex;
		parts >> name >> hex;
		lines[name] = hex;
	}
	return lines;
}

}

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_cage_wire: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		rig.context.dataHash = 0;

		std::string tool;
		for (const kdata::Record* record : rig.gameData->byType(kworld::itemTypeByName("ITEM")))
			if (tool.empty() && record->intOr("item function", 0) == 9 && rig.combatData->item(record->stringID) != nullptr)
				tool = record->stringID;
		test::require(!tool.empty(), "no TOOL item");

		struct Cast
		{
			std::uint64_t wanderer = 0;
			std::uint64_t prisoner = 0;
		};
		const auto load = [&]() -> Cast
		{
			sim.newWorld();
			sim.run(1);
			const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
			return {placed.idOf.at(1), placed.idOf.at(2)};
		};
		const auto fighter = [&](std::uint64_t id) -> state::Fighter& { return *const_cast<CombatArena*>(sim.combat())->find(id); };
		const auto give = [&](std::uint64_t id, const std::string& sid)
		{
			flecs::entity found;
			sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == id) found = e; });
			state::Inventory inventory = *found.get<state::Inventory>();
			state::Inventory::Entry entry;
			entry.itemSid = sid;
			entry.count = 1;
			entry.slot = state::Inventory::kCarried;
			inventory.items.push_back(std::move(entry));
			++inventory.version;
			found.set<state::Inventory>(std::move(inventory));
		};

		// ---- run A: the wire, the menu, the refusals, 19 and 20 refused at run time, then 18 picks it open ----
		{
			const Cast cast = load();
			state::Fighter& me = fighter(cast.wanderer);
			me.skills.lockpicking = 40.0; // below the lock (level 46), so the penalty shows: the chance is capped at 0.9 above it
			me.skills.strength = 10.0;
			Wire wire(sim, rig.context);
			wire.run(2);
			Simulation::CageView view;
			test::require(sim.cageView(kHeftCage, view) && view.locked, "the Heft cage reads locked in the sim");

			// CAGES on connect: the prisoner, its cage, the BUILDING record, locked, level, hardness.
			test::require(wire.cagesSeen && wire.cages.size() == 1, "CAGES on connect carries the caged prisoner");
			const CageRow row = wire.cages.at(0);
			test::require(row.id == cast.prisoner && row.cage == kHeftCage, "CAGES: the prisoner and its cage key");
			test::require(wire.session->strings().text(row.record) == view.record && !view.record.empty(), "CAGES: the cage's BUILDING record");
			test::require((row.flags & 1) == 1, "CAGES: locked (flags bit 0)");
			test::require(row.level == static_cast<std::uint16_t>(view.level) && row.level > 0, "CAGES: the lock level is cageLockLevel");
			test::require(row.hardness == static_cast<float>(view.maxHardness) && row.maxHardness == static_cast<float>(view.maxHardness) && row.maxHardness > 0.0f,
				"CAGES: hardness = maxHardness = the BUILDING hardness, untouched");
			test::require(wire.cageFrames == 1, "CAGES is not resent every tick");
			std::cout << "CAGES: " << row.id << " in " << row.cage << " locked level " << row.level << " hardness " << row.hardness << "\n";

			// Order 99 on the cage key: pick (16) and not tools (17) or strength (18) for strength 10, the Wanderer's own pick chance.
			test::require(wire.menuQuery(cast.wanderer, kHeftCage).empty(), "order 99 on the cage key is answered");
			test::require(wire.menu.seen && wire.menu.entries == (1u << 16), "MENU_ENTRIES on the cage: bit 16 only (no tool, strength 10): " + std::to_string(wire.menu.entries));
			test::require(wire.menu.hasChance && wire.menu.length == 36, "MENU_ENTRIES carries the trailing pickChance when bit 16 is set");
			test::require(std::fabs(wire.menu.chance - static_cast<float>(pickChance(view.level, 40.0))) < 1e-6f, "the chance is pickChance(level, lockpicking)");
			const std::vector<std::uint8_t> menuFrame = wire.rawMenu;
			// The caged prisoner picks from inside, 10 skill points lower.
			fighter(cast.prisoner).skills.lockpicking = 40.0;
			test::require(wire.menuQuery(cast.prisoner, kHeftCage).empty(), "the caged prisoner's own query is answered");
			test::require(std::fabs(wire.menu.chance - static_cast<float>(pickChance(view.level, 40.0 - kCagePickSkillPenalty))) < 1e-6f
					&& wire.menu.chance < static_cast<float>(pickChance(view.level, 40.0)), "the caged picker's chance is lower by the penalty");
			// A character id keeps the character gates: no cage bit.
			test::require(wire.menuQuery(cast.wanderer, cast.prisoner).empty() && (wire.menu.entries & 0xffff0000u) == 0 && !wire.menu.hasChance,
				"order 99 on a character answers no cage bit");
			// A tool and strength add 17 and 18.
			give(cast.wanderer, tool);
			me.skills.strength = 100.0;
			wire.run(1);
			test::require(wire.menuQuery(cast.wanderer, kHeftCage).empty() && wire.menu.entries == ((1u << 16) | (1u << 17) | (1u << 18)),
				"MENU_ENTRIES with a tool and strength 100: bits 16, 17, 18: " + std::to_string(wire.menu.entries));
			// A building that is no cage: bad_target.
			test::require(wire.menuQuery(cast.wanderer, kHeftCage + 1) == "bad_target", "order 99 on a building with nobody caged in it is bad_target");

			// The fixture: the first CAGES frame and the Wanderer's menu reply (no tool, strength 10), committed.
			{
				const std::string cagesHex = hexOf(wire.rawCages);
				const std::string menuHex = hexOf(menuFrame);
				if (std::getenv("KSIM_WRITE_FIXTURE") != nullptr)
				{
					std::ofstream out(KSIM_FIXTURE);
					out << "# scenarios/cage_prisoner after load + 2 ticks, seed 3 (ksim_cage_wire; rewrite: KSIM_WRITE_FIXTURE=1 ksim_cage_wire <navworld>).\n"
						<< "# Whole frames: u32 length, u16 type, body (SIMLINK.md section 2). cages = CAGES (64), menu = MENU_ENTRIES (61) for the Wanderer on the cage\n"
						<< "# (order 99: no tool, strength 10 -> bit 16 + pickChance). The strid is this session's string table index.\n"
						<< "cages " << cagesHex << "\nmenu " << menuHex << "\n";
				} else {
					const std::map<std::string, std::string> pinned = readFixture(KSIM_FIXTURE);
					test::require(pinned.count("cages") == 1 && pinned.count("menu") == 1, "the fixture has a cages and a menu line");
					// The pinned frames decode to the documented layout: the decoder of a client reading them gets these values.
					const auto bytesOf = [](const std::string& hex)
					{
						std::vector<std::uint8_t> out;
						for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
							out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
						return out;
					};
					const std::vector<std::uint8_t> pinnedCages = bytesOf(pinned.at("cages"));
					const std::vector<std::uint8_t> pinnedMenu = bytesOf(pinned.at("menu"));
					test::require(at<std::uint32_t>(pinnedCages, 0) + 4 == pinnedCages.size() && at<std::uint16_t>(pinnedCages, 4) == ks::kCages
							&& at<std::uint16_t>(pinnedCages, 14) == 1 && pinnedCages.size() == 6 + 10 + 31, "the pinned CAGES frame: type 64, one row of 31 bytes");
					// The character ids come from the shared entity counter, which the zone coverage history moves (NAVMESH_CACHE.md "Determinism"):
					// the bake and the runtime mesh number the cast differently. The pinned ids are the bake's; the frames are compared with
					// both characters' ids blanked, after checking they sit where the cast does (the Wanderer is placed one before the prisoner).
					const std::uint64_t pinnedPrisoner = at<std::uint64_t>(pinnedCages, 16);
					test::require(at<std::uint64_t>(pinnedCages, 24) == kHeftCage && (pinnedCages[36] & 1) == 1 && cast.prisoner - cast.wanderer == 1,
						"the pinned CAGES row: the prisoner, the Heft cage, locked");
					const auto blanked = [](std::vector<std::uint8_t> bytes, std::uint64_t id)
					{
						for (std::size_t i = 0; i + 8 <= bytes.size(); ++i)
						{
							if (at<std::uint64_t>(bytes, i) == id) std::fill(bytes.begin() + i, bytes.begin() + i + 8, std::uint8_t{0});
						}
						return bytes;
					};
					const auto blankedBoth = [&](const std::vector<std::uint8_t>& bytes, std::uint64_t prisoner) { return blanked(blanked(bytes, prisoner), prisoner - 1); };
					test::require(at<std::uint32_t>(pinnedMenu, 0) + 4 == pinnedMenu.size() && at<std::uint16_t>(pinnedMenu, 4) == ks::kMenuEntries && pinnedMenu.size() == 6 + 36
							&& at<std::uint32_t>(pinnedMenu, 6 + 28) == (1u << 16), "the pinned MENU_ENTRIES frame: bit 16 and the trailing f32");
					test::require(blankedBoth(bytesOf(cagesHex), cast.prisoner) == blankedBoth(pinnedCages, pinnedPrisoner),
						"the live CAGES frame differs from fixtures/cage_prisoner_cages.hex (a wire change, or rewrite it with KSIM_WRITE_FIXTURE=1)");
					test::require(blankedBoth(bytesOf(menuHex), cast.prisoner) == blankedBoth(pinnedMenu, pinnedPrisoner),
						"the live MENU_ENTRIES frame differs from fixtures/cage_prisoner_cages.hex");
				}
			}

			// Refusals.
			test::require(wire.interact(cast.wanderer, cast.prisoner, kPickCageAction) == "bad_target", "action 18 on a character id is bad_target");
			test::require(wire.interact(cast.wanderer, kHeftCage + 1, kPickCageAction) == "bad_target", "action 18 on a building nobody is caged in is bad_target");
			test::require(wire.interact(cast.wanderer, kHeftCage, kPickCageAction, 1) == "bad_value", "action 18 queued is bad_value");
			test::require(wire.interact(cast.wanderer, kHeftCage, 21) == "bad_value", "action 21 is bad_value");

			// 19 without a tool, 20 with too little strength: the Wanderer walks over and the run-time end is result 1 on the cage key.
			me.skills.strength = 10.0;
			fighter(cast.wanderer).skills.strength = 10.0;
			{
				// the tool goes back out of the carried grid (a fresh inventory)
				flecs::entity found;
				sim.world().ecs().each([&](flecs::entity e, const state::Id& i, const state::Inventory&) { if (i.value == cast.wanderer) found = e; });
				state::Inventory inventory = *found.get<state::Inventory>();
				std::erase_if(inventory.items, [&](const state::Inventory::Entry& entry) { return entry.itemSid == tool; });
				++inventory.version;
				found.set<state::Inventory>(std::move(inventory));
			}
			test::require(wire.interact(cast.wanderer, kHeftCage, kCutCageAction).empty(), "action 19 is accepted");
			const End* cut = wire.runToEnd(kCutCageAction, 1200);
			test::require(cut != nullptr && cut->who == cast.wanderer && cut->target == kHeftCage && cut->result == 1,
				"action 19 with no tool ends result 1 naming the cage key" + (cut == nullptr ? std::string(" (no end; ends " + std::to_string(wire.ends.size()) + ")")
					: " (who " + std::to_string(cut->who) + " target " + std::to_string(cut->target) + " result " + std::to_string(cut->result) + ")"));
			test::require(wire.interact(cast.wanderer, kHeftCage, kBreakCageAction).empty(), "action 20 is accepted");
			const End* forced = wire.runToEnd(kBreakCageAction, 1200);
			test::require(forced != nullptr && forced->target == kHeftCage && forced->result == 1, "action 20 with strength 10 ends result 1 naming the cage key");
			test::require(sim.cageDoorLocked(kHeftCage) && wire.cageFrames == 1, "a refused cut / break leaves the cage locked and sends no CAGES");

			// 18: the pick, with a bar of kind 1 whose target is the cage key, ends result 0; CAGES is resent unlocked.
			me.skills.lockpicking = 100.0;
			wire.bars.clear();
			test::require(wire.interact(cast.wanderer, kHeftCage, kPickCageAction).empty(), "action 18 is accepted");
			const End* picked = wire.runToEnd(kPickCageAction, 6000);
			test::require(picked != nullptr && picked->who == cast.wanderer && picked->target == kHeftCage && picked->result == 0, "action 18 picks the cage open: event 13 result 0");
			wire.run(2);
			test::require(!sim.cageDoorLocked(kHeftCage), "the cage is open in the sim");
			test::require(std::any_of(wire.bars.begin(), wire.bars.end(), [&](const Bar& bar) { return bar.kind == 1 && bar.target == kHeftCage; }),
				"the pick's TASK_PROGRESS row is kind 1 with the cage key as its target");
			test::require(wire.cageFrames >= 2 && wire.cages.size() == 1 && wire.cages[0].cage == kHeftCage && (wire.cages[0].flags & 1) == 0,
				"CAGES is resent with the cage unlocked");
			test::require(wire.menuQuery(cast.wanderer, kHeftCage).empty() && wire.menu.entries == 0, "order 99 on an open cage offers nothing");
			test::require(wire.interact(cast.wanderer, kHeftCage, kPickCageAction) == "not_allowed", "action 18 on an open cage is not_allowed");
			std::cout << "pick: opened, " << wire.cageFrames << " CAGES frames\n";
		}

		// ---- run B: a cut with a tool drains the hardness (bar kind 7) and opens it ----
		{
			const Cast cast = load();
			state::Fighter& me = fighter(cast.wanderer);
			me.skills.strength = 10.0;
			give(cast.wanderer, tool);
			Wire wire(sim, rig.context);
			wire.run(2);
			wire.bars.clear();
			test::require(wire.interact(cast.wanderer, kHeftCage, kCutCageAction).empty(), "action 19 with a tool is accepted");
			const End* cut = wire.runToEnd(kCutCageAction, 12000);
			test::require(cut != nullptr && cut->target == kHeftCage && cut->result == 0, "action 19 with a tool cuts the lock open: event 13 result 0");
			wire.run(2);
			test::require(!sim.cageDoorLocked(kHeftCage), "the cut cage is open");
			test::require(std::any_of(wire.bars.begin(), wire.bars.end(), [&](const Bar& bar) { return bar.kind == 7 && bar.target == kHeftCage; }),
				"the cut's TASK_PROGRESS row is kind 7 with the cage key as its target");
			test::require(wire.cages.size() == 1 && (wire.cages[0].flags & 1) == 0, "CAGES is resent unlocked after the cut");
			std::cout << "cut: opened\n";
		}

		// ---- run C: a break with strength is instant ----
		{
			const Cast cast = load();
			fighter(cast.wanderer).skills.strength = 100.0;
			Wire wire(sim, rig.context);
			wire.run(2);
			test::require(wire.interact(cast.wanderer, kHeftCage, kBreakCageAction).empty(), "action 20 with strength 100 is accepted");
			const End* forced = wire.runToEnd(kBreakCageAction, 1200);
			test::require(forced != nullptr && forced->target == kHeftCage && forced->result == 0, "action 20 breaks the lock: event 13 result 0");
			wire.run(2);
			test::require(!sim.cageDoorLocked(kHeftCage) && wire.cages.size() == 1 && (wire.cages[0].flags & 1) == 0, "the broken cage is open and CAGES says so");
			std::cout << "break: opened\n";
		}

		// ---- run D: the prisoner works its own cage from inside (action 18 by the caged character) ----
		{
			const Cast cast = load();
			fighter(cast.prisoner).skills.lockpicking = 100.0;
			Wire wire(sim, rig.context);
			wire.run(2);
			test::require(wire.interact(cast.prisoner, kHeftCage, kPickCageAction).empty(), "a caged actor may pick its own cage");
			const End* picked = wire.runToEnd(kPickCageAction, 6000);
			test::require(picked != nullptr && picked->who == cast.prisoner && picked->target == kHeftCage && picked->result == 0, "the caged prisoner picks its own cage open");
			std::cout << "own cage: opened\n";
		}

		std::cout << "ksim_cage_wire ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
