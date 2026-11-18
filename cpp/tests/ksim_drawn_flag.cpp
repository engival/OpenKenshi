// CHAR_DYNAMIC kFlagDrawn is the weapon in the hand (kenshi-k16.6.132, rwh.105): scenarios/cage_prisoner, the Wanderer fights the Slaver boss and his
// guards; the flag stays set through the fight and clears once a guard's next goal is a patrol (the plan's 7 in front of PATROL 22 / PATROL_TOWN 36),
// the sheathed weapon staying equipped (Inventory::kSheathed, on the wire in section 0). argv[1]: the world navmesh bake (exit 77 without Heft).
#include "ksim/simlink.hpp"
#include "ksim/simulation.hpp"
#include "simlink.hpp" // client/src/simlink.hpp

#include "scenario_rig.hpp"
#include "sim_access.hpp"
#include "test_util.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef KSIM_SCENARIO
#error KSIM_SCENARIO must be defined
#endif

using namespace ksim;

static flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i)
	{
		if (i.value == id)
			found = e;
	});
	return found;
}

namespace ks = ksim::simlink;

// The last CHAR_APPEARANCE of each character as the wire carries it: its version and the items by section (section 0 = the sheathed weapon).
struct WireLook
{
	std::uint32_t version = 0;
	std::vector<std::pair<std::uint32_t, std::uint8_t>> items; // {item sid, section}
};

static void decodeAppearance(const std::uint8_t* p, std::size_t n, std::map<std::uint64_t, WireLook>& looks)
{
	// tick, id, squadId, faction, race, sex, headIndex, hair, beard, hairColour, skinTone, then the body list (u8 n, 8 bytes each) and the items
	// list (u8 n, 13 bytes each), crossbowAmmo u16, hiddenLimbs u8, appearanceVersion, name, portraitSerial.
	const auto at = [&](std::size_t o, void* out, std::size_t size) { test::require(o + size <= n, "CHAR_APPEARANCE too short"); std::memcpy(out, p + o, size); };
	std::uint64_t id = 0;
	at(8, &id, 8);
	std::uint8_t bodyCount = 0;
	at(51, &bodyCount, 1);
	std::size_t o = 52 + 8u * bodyCount;
	std::uint8_t itemCount = 0;
	at(o, &itemCount, 1);
	o += 1;
	WireLook look;
	for (std::uint8_t i = 0; i < itemCount; ++i, o += 13)
	{
		std::uint32_t item = 0;
		at(o, &item, 4);
		look.items.push_back({item, p[o + 12]});
	}
	o += 2 + 1;
	at(o, &look.version, 4);
	looks[id] = std::move(look);
}

int main(int argc, char** argv)
{
	try
	{
		const scenario::File file = scenario::parseFile(KSIM_SCENARIO);
		const std::string world = argc > 1 ? argv[1] : "";
		if (world.empty() || !scenario_rig::hasRegion(file, world))
		{
			std::cout << "ksim_drawn_flag: SKIPPED: needs the world navmesh bake with the Heft region (pass its directory)\n";
			return 77;
		}
		scenario_rig::Rig rig(file, world, scenario_rig::Window{4, 7, 6, 9, 4});
		Simulation& sim = rig.sim;
		sim.newWorld();
		sim.run(1);
		const scenario::Placed placed = scenario::apply(sim, *rig.gameData, *rig.combatData, rig.context, file, 3);
		const std::uint64_t wanderer = placed.idOf.at(1);
		const std::vector<std::uint64_t> slavers = {placed.idOf.at(3), placed.idOf.at(4), placed.idOf.at(5), placed.idOf.at(6)};
		const auto drawn = [&](std::uint64_t id) { return sim.combat()->find(id)->weapon.weaponType != 5; };
		const auto goalOf = [&](std::uint64_t id) { const state::Routine* r = entityOf(sim, id).get<state::Routine>(); return r ? int(r->goal) : -1; };
		for (const std::uint64_t id : slavers)
			test::require(drawn(id) && sim.sheathedWeaponOf(id).empty(), "every slaver starts with its weapon in the hand");
		ks::Session session(sim, rig.context);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter hello(bytes);
		session.begin(hello);
		std::vector<std::uint8_t> helloBytes;
		::simlink::OrderWriter::hello(helloBytes, rig.context.dataHash, "test");
		test::require(session.onFrame(ks::kHello, helloBytes.data() + 6, helloBytes.size() - 6, hello), "hello refused");
		session.sendTick(hello, true);
		std::map<std::uint64_t, WireLook> wire;
		const auto decode = [&]()
		{
			for (std::size_t at = 0; at + 6 <= bytes.size();)
			{
				std::uint32_t len = 0;
				std::uint16_t type = 0;
				std::memcpy(&len, &bytes[at], 4);
				std::memcpy(&type, &bytes[at + 4], 2);
				if (type == ks::kAppearance)
					decodeAppearance(&bytes[at + 6], len - 2, wire);
				at += 4 + len;
			}
		};
		decode();
		const auto encode = [&]()
		{
			bytes.clear();
			ks::BinaryWriter out(bytes);
			session.sendTick(out, false);
			decode();
		};
		std::map<std::uint64_t, std::string> carried;
		for (const std::uint64_t id : slavers)
			carried[id] = fighterOf(sim, id)->weapon.sid;
		sim.enqueue(Command::fighterAttack(state::Id{wanderer}, state::Id{slavers[0]}));
		std::uint64_t sheathedId = 0;
		bool fought = false;
		int checked = 0;
		bool refused = false;
		const auto run = [&](int ticks)
		{
			for (int tick = 0; tick < ticks; ++tick)
			{
				sim.tick();
				encode();
				for (const std::uint64_t id : slavers)
				{
					// The encoded CHAR_APPEARANCE, not the sim state: a weapon in the hand is section 9, a sheathed one section 0 (k16.6.181).
					const auto look = wire.find(id);
					if (look != wire.end())
					{
						++checked;
						int inHand = 0, onBack = 0;
						for (const auto& item : look->second.items)
						{
							inHand += item.second == 9;
							onBack += item.second == 0;
						}
						const std::string why = " (slaver " + std::to_string(id) + ", tick " + std::to_string(sim.clockTick()) + ")";
						test::require(inHand == (drawn(id) ? 1 : 0), "the wire lists the weapon in section 9 exactly while it is drawn" + why);
						test::require(onBack == (sim.sheathedWeaponOf(id).empty() ? 0 : 1), "the wire lists the sheathed weapon in section 0" + why);
					}
					const bool hasTarget = fighterOf(sim, id)->target != 0;
					fought = fought || hasTarget;
					if (hasTarget && !refused && fighterOf(sim, id)->state != state::CombatState::Attack && fighterOf(sim, id)->state != state::CombatState::Defend
						&& fighterOf(sim, id)->state != state::CombatState::Stumble)
					{
						// 7 UNEQUIP_WEAPON in the middle of a fight (a goal change between two swings) must leave the weapon in the hand (k16.6.181).
						sim.sheatheWeapon(id);
						test::require(drawn(id) && sim.sheathedWeaponOf(id).empty(), "the plan's 7 refuses a fighter with a target");
						refused = true;
					}
					if (hasTarget)
						test::require(drawn(id), "the flag stays set while a slaver has a target (" + std::to_string(id) + ")");
					if (!drawn(id) && sheathedId == 0)
					{
						sheathedId = id;
						test::require(goalOf(id) > 0, "a slaver sheathes only as its next goal is taken (the plan's 7), goal " + std::to_string(goalOf(id)));
						test::require(!hasTarget, "no sheathe with a target");
					}
				}
			}
		};
		run(20 * 20);
		sim.removeFighter(wanderer);
		run(20 * 60);
		test::require(fought, "the slavers fought");
		test::require(refused, "a slaver was asked to sheathe between two swings of the fight");
		test::require(checked > 1000, "the wire was checked on every tick for every slaver");
		test::require(sheathedId != 0, "a slaver sheathed once its goal changed (rwh.105)");
		for (const std::uint64_t id : slavers)
		{
			if (id == sheathedId)
			{
				test::require(sim.sheathedWeaponOf(id) == carried[id], "the sheathed weapon stays equipped on the back, the same record");
				continue;
			}
			test::require(drawn(id) && sim.sheathedWeaponOf(id).empty(), "a slaver with no peaceful goal keeps its weapon drawn (" + std::to_string(id) + ")");
		}
		sim.drawWeapon(sheathedId);
		test::require(drawn(sheathedId) && sim.sheathedWeaponOf(sheathedId).empty(), "drawing puts it back in the hand: flag set, nothing sheathed");
		std::cout << "ksim_drawn_flag ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
