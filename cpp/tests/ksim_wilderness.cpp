// The wilderness spawner against the exe's rules (kinspect s71_static.md, ccf1940): the Hub's zones are BIOME_GROUP
// 42262-rebirth "Border Zone", and a homeless squad is drawn by weight = w - liveCopies x totalW / (zones x pop x amount).
// Prints the Border Zone census of newGame (seed 1) against the proportions the list asks for; KSIM_WILDERNESS_VERBOSE=1
// prints every slot.
#include "ksim/nav/placement.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/wilderness.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr std::uint32_t kBorderZoneColour = 0xFF0000;

void printSlot(const WildernessSlotCensus& slot, const kdata::GameData& data)
{
	const double density = static_cast<double>(slot.totalWeight) / (slot.zones * slot.amount);
	std::cout << slot.group << " colour 0x" << std::hex << slot.colour << std::dec << ": " << slot.zones << " zones, amount "
		<< slot.amount << ", quota " << slot.quota << ", total weight " << slot.totalWeight << ", density " << std::fixed
		<< std::setprecision(2) << density << "\n";
	for (const WildernessTemplateCount& entry : slot.templates)
	{
		const kdata::Record* record = data.find(entry.squad);
		const double expected = slot.quota * static_cast<double>(entry.weight) / static_cast<double>(slot.totalWeight);
		std::cout << "  " << std::setw(18) << entry.squad << " " << std::setw(32) << std::left
			<< (record != nullptr ? record->name : "?") << std::right << " w " << std::setw(3) << entry.weight << " live "
			<< std::setw(3) << entry.live << " proportional " << std::setw(6) << expected << "\n";
	}
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const kdata::GameData& data = *sim.gameData();

		// The Hub block (zones x20-22, z30-32) is one BIOME_GROUP, "Border Zone".
		const AreaLookup areas(KENSHI_DATA);
		for (int dz = 0; dz < 3; ++dz)
		{
			for (int dx = 0; dx < 3; ++dx)
			{
				const double x = kworld::kWorldMin + (simlink::kHubZoneX + dx + 0.5) * kworld::kZoneSize;
				const double z = kworld::kWorldMin + (30 + dz + 0.5) * kworld::kZoneSize;
				test::require(areas.colourAt(x, z) == kBorderZoneColour,
					"Hub zone " + std::to_string(simlink::kHubZoneX + dx) + "," + std::to_string(30 + dz) + " is not areasmap colour 0xFF0000");
			}
		}
		const kdata::Record* border = nullptr;
		for (const kdata::Record* group : data.byType(kworld::itemTypeByName("BIOME_GROUP")))
		{
			if (static_cast<std::uint32_t>(group->intOr("index", 0)) == kBorderZoneColour)
			{
				test::require(border == nullptr, "two BIOME_GROUPs share index 0xFF0000");
				border = group;
			}
		}
		test::require(border != nullptr && border->stringID == "42262-rebirth.mod" && border->name == "Border Zone",
			"index 0xFF0000 is not BIOME_GROUP 42262-rebirth.mod \"Border Zone\"");

		// The weight rule: a spawn of a template happens only while w - n x density > 0, so after the initial fill no
		// template holds more than ceil(w / density) homeless copies. With density > w that is one copy.
		const std::vector<WildernessSlotCensus> census = sim.wilderness()->census(sim.world().ecs());
		const bool verbose = std::getenv("KSIM_WILDERNESS_VERBOSE") != nullptr;
		bool sawBorderZone = false;
		for (const WildernessSlotCensus& slot : census)
		{
			const double density = static_cast<double>(slot.totalWeight) / (slot.zones * slot.amount);
			std::int64_t total = 0;
			for (const WildernessTemplateCount& entry : slot.templates)
			{
				total += entry.live;
				const double cap = std::ceil(static_cast<double>(entry.weight) / density);
				test::require(static_cast<double>(entry.live) <= cap,
					slot.group + ": " + entry.squad + " has " + std::to_string(entry.live) + " live copies, the weight rule allows "
					+ std::to_string(static_cast<long>(cap)));
			}
			test::require(total <= slot.quota, slot.group + " is over its quota");
			if (slot.group == border->stringID)
			{
				sawBorderZone = true;
				printSlot(slot, data);
			} else if (verbose) {
				printSlot(slot, data);
			}
		}
		test::require(sawBorderZone, "no Border Zone slot");
		std::cout << "ksim_wilderness: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_wilderness: FAILED: " << error.what() << "\n";
		return 1;
	}
}
