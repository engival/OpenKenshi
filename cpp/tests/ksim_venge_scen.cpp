// Venge's sky beams playtest scenarios (kenshi-h2v.14.3; docs/reference/VENGE_BEAMS_STATIC.md): scenarios/venge_beams.kscen (midday, pinned),
// venge_beams_approach.kscen (outside the group's border) and venge_beams_night.kscen (23:30, no pin) each load, and
//  - by day the Venge group has its 40 handlers (the weather pin), at night none;
//  - day: the Wanderer in the open is burned, the Sheltered one in the ruin is indoors and takes none, the Dustcoat one takes less than the Wanderer
//    (the beam callback is held at a full burn everywhere so the comparison does not wait for a beam to wander over three characters);
//  - approach: the start is outside the Venge group and a beam is within the range the BEAMS frame is sent to, before the character enters;
//  - night: nobody burns over 60 s with the world's own weather.
// Needs the world navmesh bake (argv[1], exit 77 without the Venge region).
#include "ksim/world/beams.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/weather.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "nav_source.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

using namespace ksim;

namespace {

constexpr std::uint32_t kVengeColour = 6566400;
constexpr const char* kVengeGroup = "55678-rebirth.mod";
constexpr const char* kVengeBeams = "57717-rebirth.mod"; // the weather with the SkyBeam effect, window 5 to 23

struct Loaded
{
	std::unique_ptr<Simulation> sim;
	scenario::Placed placed;
	std::shared_ptr<const kdata::GameData> gd;

	state::Fighter& fighter(int fileId)
	{
		return *const_cast<CombatArena*>(sim->combat())->find(placed.idOf.at(fileId));
	}
	// The Venge group's field; null when the world has no beam field or the group has no beams now.
	const BeamGroup* vengeBeams() const
	{
		const std::vector<WeatherGroupDef>& groups = sim->weather()->model().groups();
		for (std::size_t g = 0; g < groups.size(); ++g)
			if (groups[g].sid == kVengeGroup)
				return sim->beams() != nullptr && sim->beams()->groups().size() > g && !sim->beams()->groups()[g].slots.empty() ? &sim->beams()->groups()[g] : nullptr;
		test::require(false, "no Venge group in the weather model");
		return nullptr;
	}
};

double wearOf(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& p : f.parts)
		sum += p.robotic ? 0.0 : p.wear;
	return sum;
}

Loaded load(const std::string& name, const std::string& world, const std::vector<kworld::ZoneCell>& zones, const kworld::LoadOrder& order)
{
	const scenario::File file = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/" + name + ".kscen");
	test::require(file.bead == "kenshi-h2v.14", name + ": bead is not kenshi-h2v.14");
	// The day files pin the beams (a noon pick of the world's own weather may or may not be them); the night file must not pin.
	const bool pinned = name != "venge_beams_night";
	test::require(pinned ? file.weatherGroup == kVengeGroup && file.weather == kVengeBeams : file.weatherGroup.empty(), name + ": the weather line is wrong");
	SpawnOptions spawn;
	spawn.seed = 7;
	Loaded out;
	out.sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, spawn));
	out.sim->markPristine();
	out.gd = out.sim->gameData();
	const std::shared_ptr<const CombatData> data = CombatData::build(*out.gd, KENSHI_DATA);
	CombatSettings settings;
	settings.substeps = 2;
	out.sim->enableCombat(data, settings);
	out.sim->enableNavigation(test::openWorldNav(world, zones));
	out.sim->enableWeather(*out.gd);
	out.sim->newWorld();
	out.sim->run(1);
	simlink::ServerContext context;
	context.squadLimit = 50;
	out.placed = scenario::apply(*out.sim, *out.gd, *data, context, file, 5);
	out.sim->run(3);
	return out;
}

// The wear each listed character takes in `seconds` of game time.
std::vector<double> wearOver(Loaded& l, const std::vector<int>& chars, int seconds)
{
	std::vector<double> before;
	for (const int id : chars)
		before.push_back(wearOf(l.fighter(id)));
	l.sim->run(static_cast<std::uint64_t>(seconds) * 20);
	std::vector<double> taken;
	for (std::size_t i = 0; i < chars.size(); ++i)
		taken.push_back(wearOf(l.fighter(chars[i])) - before[i]);
	return taken;
}

} // namespace

int main(int, char** argv)
{
	try
	{
		const scenario::File probe = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/venge_beams.kscen");
		const kworld::ZoneCell zone = kworld::zoneCellOf(probe.centreX, probe.centreZ);
		if (!test::worldRegionReady(std::string(argv[1]), zone.x, zone.z))
		{
			std::cout << "ksim_venge_scen: SKIPPED: needs the world navmesh bake with Venge's region (pass its directory)\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -2; dz <= 2; ++dz)
			for (int dx = -4; dx <= 2; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");

		// Day: the pin gives the group its 40 handlers; the burn, the shelter and the Dustcoat.
		{
			Loaded day = load("venge_beams", world, zones, order);
			const BeamGroup* beams = day.vengeBeams();
			test::require(beams != nullptr && beams->slots.size() == 40, "day: the Venge group has no 40 beam handlers (the weather pin)");
			test::require(!day.fighter(1).indoors && day.fighter(2).indoors && !day.fighter(3).indoors, "day: indoors is not (open, ruin, open)");
			test::require(!day.fighter(3).armour.empty() && day.fighter(3).armour.front().weatherProtection > 0.5, "day: the Dustcoat is not worn");
			const_cast<CombatArena*>(day.sim->combat())->environmentMutable().beamAt = [](double, double, int& type, double& strength)
			{
				type = 3;
				strength = 0.75;
			};
			const std::vector<double> taken = wearOver(day, {1, 2, 3}, 4);
			std::cout << "day: open " << taken[0] << ", sheltered " << taken[1] << ", dustcoat " << taken[2] << " wear in 4 s\n";
			test::require(taken[0] > 1.0, "day: the Wanderer in the open does not burn");
			test::require(taken[1] < 1.0e-9, "day: the Sheltered one in the ruin burns");
			test::require(taken[2] > 0.0 && taken[2] < 0.5 * taken[0], "day: the Dustcoat one does not burn clearly less than the Wanderer");
		}

		// Approach: outside the group, a beam within the frame's range before the character enters.
		{
			Loaded approach = load("venge_beams_approach", world, zones, order);
			const state::Fighter& f = approach.fighter(1);
			test::require(approach.sim->weather()->areas()->colourAt(f.x, f.z) != kVengeColour, "approach: the start is inside Venge");
			const BeamGroup* beams = approach.vengeBeams();
			test::require(beams != nullptr && beams->slots.size() == 40, "approach: the Venge group has no 40 beam handlers (the weather pin)");
			double nearest = 1.0e18;
			std::size_t inRange = 0;
			for (const BeamSlot& slot : beams->slots)
			{
				const double d = std::hypot(slot.px - f.x, slot.pz - f.z);
				nearest = std::min(nearest, d);
				inRange += d <= kBeamRange;
			}
			std::cout << "approach: " << inRange << "/40 beams within " << kBeamRange << " u of the start, the nearest " << nearest << " u\n";
			test::require(inRange > 0, "approach: no beam is within the range the BEAMS frame is sent to");
			test::require(wearOf(approach.fighter(1)) == 0.0, "approach: the Wanderer burned at the start");
		}

		// Night: 23:30 and no pin, the window is shut: no beams, nobody burns.
		{
			Loaded night = load("venge_beams_night", world, zones, order);
			test::require(night.vengeBeams() == nullptr, "night: the Venge group has beams at 23:30");
			const std::vector<double> taken = wearOver(night, {1, 2, 3}, 60);
			test::require(taken[0] == 0.0 && taken[1] == 0.0 && taken[2] == 0.0, "night: somebody burned");
			test::require(night.vengeBeams() == nullptr, "night: beams appeared within a minute");
		}
		std::cout << "ksim_venge_scen ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
