// The Hub's 11 market stalls pick the parts variants the original showed live (rwh.206, per-stall table).
#include "kdata/gamedata.hpp"
#include "kworld/buildingparts.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <iostream>
#include <string>

namespace {

struct Stall
{
	const char* building;
	float x, z;
	const char* mesh; // the part's mesh file name
};

constexpr Stall kStalls[] = {
	{ "5248-TwoStorey.mod", -51119.7f, 2794.0f, "MarketStall_Grouped01_A.mesh" },
	{ "5248-TwoStorey.mod", -51024.6f, 3095.0f, "MarketStall_Grouped01_B.mesh" },
	{ "5248-TwoStorey.mod", -50761.2f, 3094.3f, "MarketStall_Grouped01_C.mesh" },
	{ "5248-TwoStorey.mod", -50745.0f, 3026.4f, "MarketStall_Grouped01_B.mesh" },
	{ "5251-TwoStorey.mod", -51133.7f, 3022.4f, "MarketStall_Grouped02_A.mesh" },
	{ "5251-TwoStorey.mod", -50914.1f, 2780.6f, "MarketStall_Grouped02_C.mesh" },
	{ "5251-TwoStorey.mod", -50814.0f, 3157.7f, "MarketStall_Grouped02.mesh" },
	{ "5253-TwoStorey.mod", -51017.5f, 2851.3f, "MarketStall_Grouped03_C.mesh" },
	{ "5253-TwoStorey.mod", -51052.3f, 3030.0f, "MarketStall_Grouped03_B.mesh" },
	{ "5253-TwoStorey.mod", -50998.1f, 3177.2f, "MarketStall_Grouped03_A.mesh" },
	{ "5253-TwoStorey.mod", -50846.2f, 3041.8f, "MarketStall_Grouped03_A.mesh" },
};

} // namespace

int main()
{
	try
	{
		const kdata::GameData data = kworld::loadMergedGameData(KENSHI_DATA);
		for (const Stall& stall : kStalls)
		{
			const kdata::Record* building = data.find(stall.building);
			test::require(building != nullptr, std::string("no record ") + stall.building);
			const auto parts = kworld::selectBuildingParts(data, *building, stall.x, stall.z, kworld::PartStage::Exterior);
			test::require(parts.size() == 1, std::string(stall.building) + ": expected one part, got " + std::to_string(parts.size()));
			const kdata::Record* part = data.find(parts[0]);
			test::require(part != nullptr, "no part record " + parts[0]);
			const std::string mesh = part->fileOr("phs or mesh");
			test::require(mesh.ends_with(stall.mesh), std::string(stall.building) + " at " + std::to_string(stall.x) + ": got " + mesh
				+ ", want " + stall.mesh);
		}
		std::cout << "kworld_building_parts: 11/11 stalls match\n";
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
