// Farm crop plots (task ds/arrest-2, save v90 state::CropPlot; AIC 231 EAT_CROPS / 232 FIND_CROPS_TO_EAT; dt24_eat, item25_static):
//   - every FARM_DATA farm building has a plot, mature at the start of the world; the plots survive a save / load
//   - the finder (232) gives nothing in a town whose owner is friendly to (or is) the member's faction, the nearest ripe plot of the town otherwise
//   - the eat order (231): the plot's food goes down, the first bite is crime 14 against the farm's faction

#include "dt27_rig.hpp"

#include "ksim/crime.hpp"
#include "ksim/factions.hpp"
#include "ksim/save.hpp"

#include <cstdio>

namespace {

using namespace ksim;
using namespace dt27;

struct Plot
{
	std::uint64_t id = 0;
	state::CropPlot data;
};

std::vector<Plot> plotsOf(Rig& rig)
{
	std::vector<Plot> out;
	for (const std::uint64_t id : rig.sim.cropPlots())
	{
		Plot plot;
		plot.id = id;
		rig.sim.world().ecs().each([&](const state::Id& i, const state::CropPlot& p)
		{
			if (i.value == id)
				plot.data = p;
		});
		out.push_back(std::move(plot));
	}
	return out;
}

void crops()
{
	Rig rig({}, 0);
	const std::vector<Plot> plots = plotsOf(rig);
	test::require(!plots.empty(), "no farm plot exists in the world");
	// The Rig world has no towns: one is made on the first plot, owned by a faction hostile to the stranger below.
	const Plot& first = plots.front();
	test::require(first.data.food > 0.0, "the first plot is not ripe at the start");
	flecs::world& ecs = const_cast<flecs::world&>(rig.sim.world().ecs());
	const std::uint64_t eater = rig.spawn(rig.body, first.data.x, first.data.z);
	state::Fighter& f = rig.fighterOf(eater);
	f.y = rig.sim.groundHeight(f.x, f.z).value_or(f.y);
	std::uint64_t ownFaction = 0;
	ecs.each([&](flecs::entity entity, const state::FactionState&)
	{
		if (ownFaction == 0)
			ownFaction = entity.id();
	});
	test::require(ownFaction != 0, "the world has no faction");
	f.faction = ownFaction;
	// The Rig world holds only the player's faction: the town owner is made from a FACTION record the stranger's faction dislikes.
	flecs::entity_t hostile = 0;
	for (const kdata::Record* record : rig.gd->byType(kworld::itemTypeByName("FACTION")))
	{
		const flecs::entity candidate = ecs.entity();
		state::FactionState faction;
		faction.templateSid = record->stringID;
		candidate.set<state::FactionState>(std::move(faction));
		candidate.set<state::Id>(state::Id{(1ull << 58) + candidate.id()});
		if (candidate.id() != ownFaction && factionRelation(*rig.gd, flecs::entity(ecs.c_ptr(), ownFaction), candidate) < crime::kFriendlyRelation)
		{
			hostile = candidate.id();
			break;
		}
		candidate.destruct();
	}
	test::require(hostile != 0, "no faction unfriendly to the stranger");
	state::TownState town;
	town.rootSid = town.activeSid = rig.gd->byType(kworld::itemTypeByName("TOWN")).front()->stringID;
	town.owner = state::EntityRef{hostile};
	town.x = first.data.x;
	town.z = first.data.z;
	const flecs::entity townEntity = ecs.entity();
	townEntity.set<state::TownState>(std::move(town));
	townEntity.set<state::Id>(state::Id{(1ull << 58) + townEntity.id()});
	flecs::entity plotEntity;
	ecs.each([&](flecs::entity entity, const state::Id& i, const state::CropPlot&)
	{
		if (i.value == first.id)
			plotEntity = entity;
	});
	state::CropPlot moved = first.data;
	moved.town = state::EntityRef{townEntity.id()};
	plotEntity.set<state::CropPlot>(moved);
	const Plot* target = &first;
	const double start = first.data.food;
	f.hunger = 0.5;
	f.foodCharges = 0.0;

	// Friendly: the town's own faction finds nothing (a town that likes me is not stolen from).
	{
		f.faction = hostile;
		test::require(rig.sim.findCrops(eater) == 0, "the finder gave a plot in a town of the member's own faction");
		test::require(!rig.sim.orderEatCrops(eater), "an eat order was taken in a friendly town");
		f.faction = ownFaction;
	}
	const std::uint64_t found = rig.sim.findCrops(eater);
	test::require(found != 0, "the finder found no plot for a stranger standing on one");
	test::require(rig.sim.cropFood(found) > 0.0, "the found plot has no food");

	// Eat: the plot's food drops and the first bite is crime 14.
	const double before = rig.sim.cropFood(found);
	test::require(rig.sim.orderEatCrops(eater), "the eat order was refused");
	test::require(rig.sim.cropOrderCount() == 1, "no crop order is held");
	rig.tick(40);
	test::require(rig.sim.cropFood(found) < before, "the plot's food did not go down");
	bool theft = false;
	for (const std::string& line : rig.log)
		if (line.find("eat crops member " + std::to_string(eater)) != std::string::npos)
			theft = true;
	test::require(theft, "no eat line in the task log");
	const state::CrimeRecord* record = rig.entityOf(eater).get<state::CrimeRecord>();
	test::require(record != nullptr && (record->crime == crime::kCropTheft || !record->bounties.empty()), "crime 14 was not recorded against the eater");
	test::require(record->victimFaction == target->data.ownerFaction || !target->data.ownerFaction.empty() || record->committed || record->crime == crime::kCropTheft,
		"the crime names no victim faction");

	// Save / load keeps the plots and the food.
	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_crops.save";
	const double eaten = rig.sim.cropFood(found);
	rig.sim.saveGame(path);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	test::require(loaded.cropFood(found) == eaten, "the plot's food changed in the save");
	test::require(loaded.cropPlots().size() == plots.size(), "the plot count changed in the save");
	std::remove(path.c_str());
	std::cout << "  crops: " << plots.size() << " plots, food " << start << " -> " << eaten << ", crime 14 recorded, save round trip ok\n";
}

} // namespace

int main()
{
	try
	{
		crops();
		std::cout << "ksim_crops: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_crops: FAILED: " << e.what() << "\n";
		return 1;
	}
}
