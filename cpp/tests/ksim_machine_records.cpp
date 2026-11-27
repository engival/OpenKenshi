// Machine records (kenshi-k16.5.22, kenshi-gzto): at the South Stone Camp every placed machine BUILDING (producer, converter, power, turret,
// furnace) has a record with the inspect panel's fields (kinspect s8l2). Needs no wire.

#include "ksim/core/simulation.hpp"
#include "ksim/economy/machines.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

int main()
{
	using namespace ksim;
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const kdata::GameData gd = kworld::loadMergedGameData(order);
		SpawnOptions options;
		options.seed = 12;
		options.wildernessByDensity = false;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});

		struct Count { int n = 0; machines::Kind kind = machines::Kind::Producer; };
		std::map<std::string, Count> byName;
		std::size_t camp = 0;
		for (const machines::Record& r : sim.machineRecords())
		{
			if (std::hypot(r.x - 64742.0, r.z + 49500.0) > 1100.0)
				continue;
			++camp;
			const kdata::Record* building = gd.find(r.recordSid);
			test::require(building != nullptr, "the record is a BUILDING");
			Count& c = byName[building->name];
			++c.n;
			c.kind = r.kind;
			test::require(r.town == "South Stone Camp", building->name + ": town South Stone Camp, got '" + r.town + "'");
			test::require(r.maxHp > 0.0 && r.hp == r.maxHp, building->name + ": condition hp / max");
			test::require(r.maxEfficiency > 0.0 && r.efficiency >= 0.0 && r.efficiency <= r.maxEfficiency + 1e-9, building->name + ": efficiency fields");
			test::require(r.workers <= r.maxWorkers || r.maxWorkers == 0, building->name + ": workers n/max");
			if (r.kind == machines::Kind::Converter)
				test::require(!r.input.empty() && !r.output.empty(), building->name + ": a converter names its input and output");
			if (r.kind == machines::Kind::Furnace)
				test::require(r.maxWorkers == 0 && r.status == machines::Status::Working, "the furnace: Workers '-', Working");
			if (r.kind == machines::Kind::Producer && r.stat != machines::StatLine::None)
				test::require(r.statValue > 0.0, building->name + ": a stat line value");
			std::cout << building->name << " kind " << int(r.kind) << " owner '" << r.ownerFaction << "' status " << int(r.status) << " workers " << r.workers
				<< "/" << r.maxWorkers << " eff " << r.efficiency << " max " << r.maxEfficiency << " power " << r.powerOutput << " stat " << int(r.stat) << "\n";
		}
		test::require(camp > 0, "the camp has machines");
		// Condition (kenshi-rwh.298 section 2): the absolute current HP, a placement built whole = its `construction` sum. The s8l2 panel
		// values (the Hybrid's 35 ... the Item Furnace's 4) pin it per record, one rule for every building.
		const std::map<std::string, double> panelCondition = {{"Hybrid Stone Mine", 35}, {"Quarry Mine A", 3}, {"Ore Drill II", 30},
			{"Ore Processor", 10}, {"Stone Processor V", 50}, {"Item Furnace", 4}};
		for (const machines::Record& r : sim.machineRecords())
		{
			const kdata::Record* building = gd.find(r.recordSid);
			const auto want = building != nullptr ? panelCondition.find(building->name) : panelCondition.end();
			if (want != panelCondition.end() && std::hypot(r.x - 64742.0, r.z + 49500.0) <= 1100.0)
				test::require(r.hp == want->second && r.maxHp == want->second, building->name + ": Condition " + std::to_string(r.hp) + ", panel " + std::to_string(want->second));
		}
		// Max efficiency per placement (kenshi-rwh.318, live): (int)(richness x rate x 100) at the placement; the owner label is empty
		// while the camp is unvisited (a new game: nobody has been near it), the turret reads no 7500%.
		const struct { const char* name; double x, z; int want; } panelMax[] = {{"Quarry Mine A", 64729.8, -49877.7, 41}, {"Hybrid Stone Mine", 65123.1, -49982.4, 93},
			{"Ore Drill II", 64727.0, -50381.0, 70}, {"Quarry Mine B", 64459.6, -50346.7, 40}};
		int pinned = 0;
		for (const machines::Record& r : sim.machineRecords())
		{
			const kdata::Record* building = gd.find(r.recordSid);
			test::require(r.ownerFaction.empty(), "unvisited camp: no owner label");
			test::require(r.efficiency <= r.maxEfficiency + 1e-9, "efficiency capped at max efficiency");
			if (r.kind == machines::Kind::Turret)
				test::require(r.maxEfficiency <= 1.0 + 1e-9, "a turret's max efficiency is no 7500%");
			for (const auto& want : panelMax)
			{
				if (building != nullptr && building->name == want.name && std::hypot(r.x - want.x, r.z - want.z) < 5.0)
				{
					test::require(static_cast<int>(r.maxEfficiency * 100.0 + 1e-4) == want.want, std::string(want.name) + ": Max efficiency " + std::to_string(r.maxEfficiency * 100.0) + ", panel " + std::to_string(want.want));
					++pinned;
				}
			}
		}
		test::require(pinned >= 3, "the panel's max efficiency placements were found");
		// The types the original shows (s8l2) and the survey found without a row (j8xs): each has at least one record.
		for (const char* name : {"Hybrid Stone Mine", "Quarry Mine A", "Quarry Mine B", "Ore Drill II", "Well II", "Item Furnace", "Stone Processor IV",
				"Stone Processor V", "Ore Processor", "Iron Refinery III", "Campfire", "Small Generator", "Wind Generator", "Mounted Crossbow mkII"})
			test::require(byName.count(name) != 0, std::string("a machine record for ") + name);
		test::require(byName["Mounted Crossbow mkII"].n == 18 && byName["Mounted Crossbow mkII"].kind == machines::Kind::Turret, "18 turrets");
		test::require(byName["Campfire"].n == 6 && byName["Campfire"].kind == machines::Kind::Converter, "6 campfires, converters");
		test::require(byName["Item Furnace"].kind == machines::Kind::Furnace, "the furnace is a Furnace, not a Producer");
		std::cout << "ksim_machines ok (" << camp << " records)\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
