// ksim_spectate_stagger (kenshi-k16.6.124, rwh.102 section 5): spectate_fight member by member. Each member senses and takes its fight goal from its own
// sensor record (no NPC squad shares a sighting), so the first cross-side targets are spread, not simultaneous; the bodies spawn sheathed (weapon_type 5)
// and draw only once they have a target (plan task 6 before 59 / 4); a member moves at its run speed once it has committed to a target.
// Needs the world bake (argv[1], exit 77 without its regions).
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/combat.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/factions.hpp"
#include "ksim/scenario_file.hpp"
#include "ksim/server_ai.hpp"
#include "ksim/simlink.hpp"
#include "ksim/perception.hpp"
#include "ksim/simulation.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

namespace {

using namespace ksim;

constexpr int kMaxTicks = 20 * 20;
constexpr const char* kName = "spectate_fight";

struct Track
{
	std::uint64_t id = 0;
	int side = 0;
	int firstTarget = -1; // tick of the first target (an enemy of the other squad)
	int drawn = -1; // tick the weapon was first in hand (weapon_type != 5)
	bool drawnAtStart = false;
	double peakBefore = 0.0; // peak step speed (u/s) before the first target
	double peakAfter = 0.0; // peak step speed once targeting
	double runSpeed = 0.0; // the member's run speed (u/s)
	bool sensed = true; // at the end it holds no target, or one it has its own sensor record of
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		const scenario::ScenarioInfo info = [&]
		{
			for (const scenario::ScenarioInfo& listed : scenario::listScenarios(KSIM_SCENARIOS_DIR))
				if (listed.name == kName)
					return listed;
			throw std::runtime_error(std::string("scenario not listed: ") + kName);
		}();
		const scenario::File file = scenario::parseFile(info.path);
		test::require(file.squads.size() == 2, "the file places two squads");
		// The original lays its members out with a generated set of yaws (rwh.91 section 2: Raiders -88 .. 52, Berserkers -18 .. 89 deg), not one facing
		// for a whole squad; a squad that all faces the enemy gives every member the same first sighting.
		for (const scenario::Squad& squad : file.squads)
		{
			std::set<double> yaws;
			for (const scenario::Char& c : file.chars)
				if (c.squad == squad.id)
					yaws.insert(c.yaw);
			test::require(yaws.size() >= 3, "squad " + squad.name + " has the generated facings, not one (" + std::to_string(yaws.size()) + " distinct yaws)");
		}
		for (const scenario::Squad& squad : file.squads)
			test::require(!squad.player, "no squad of the file is the player's");
		if (bake.empty())
		{
			std::cout << "ksim_spectate_stagger: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(file.centreX, file.centreZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_spectate_stagger: SKIPPED: no " << region << "\n";
			return 77;
		}
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({zone.x + dx, zone.z + dz});
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		const std::shared_ptr<MeshNavigator> mesh = test::openWorldNav(bake, zones);
		// One run of the file's fight; `aside` moves the last Berserker 300 u east of its squad, out of sight of the enemy (rwh.102: a member with no sensor
		// record of its own keeps its package goal, follow 141, even while its squadmates fight).
		struct Result
		{
			std::vector<Track> tracks;
			std::vector<std::string> log;
			std::uint64_t straggler = 0;
			double gapStart = 0.0; // the moved member's distance to the centroid of its squadmates at the start / after 3 s
			double gapLater = 0.0;
			double walkSpeed = 0.0;
		};
		const auto play = [&](bool aside)
		{
			SpawnOptions options;
			options.seed = 11;
			Simulation sim = Simulation::newGame(KENSHI_DATA, order, options);
			sim.markPristine();
			const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
			CombatSettings settings;
			settings.substeps = 2;
			sim.enableCombat(combat, settings);
			sim.enableNavigation(mesh);
			// The server's own AI enablement (server_ai.hpp), with no --bake: it takes the server's navmesh.
			ServerAiOptions ai;
			ai.registryPath = KSIM_AI_TABLE;
			test::require(std::string(enableServerAi(sim, ai)) == "navmesh", "the server AI took the simulation's navigator");

			simlink::ServerContext context;
			context.gameData = sim.gameData();
			context.squadLimit = 50;
			scenario::check(*sim.gameData(), file);
			sim.newWorld();
			sim.run(1);
			const scenario::Placed placed = scenario::apply(sim, *sim.gameData(), *combat, context, file, 11);
			test::require(placed.players.empty(), "the scenario places no player character");
			test::require(placed.orders == 0, "the scenario issues no order");
			test::require(placed.others.size() >= 8, "the two squads are built (" + std::to_string(placed.others.size()) + ")");

			Result result;
			result.walkSpeed = UtilityGroupLayer::kWalkSpeed;
			sim.setTaskLog([&](const std::string& line) { result.log.push_back(line); });
			const auto fighterOf = [&](std::uint64_t id) { return const_cast<state::Fighter*>(sim.combat()->find(id)); };
			std::map<std::uint64_t, int> sideOf; // the squad each body started in (by its start z)
			for (const std::uint64_t id : placed.others)
				sideOf[id] = fighterOf(id)->z < -1000.0 ? 0 : 1;
			const auto gapOf = [&](std::uint64_t id)
			{
				double cx = 0.0;
				double cz = 0.0;
				int n = 0;
				for (const std::uint64_t other : placed.others)
				{
					if (other == id || sideOf[other] != sideOf[id])
						continue;
					cx += fighterOf(other)->x;
					cz += fighterOf(other)->z;
					++n;
				}
				return std::hypot(fighterOf(id)->x - cx / n, fighterOf(id)->z - cz / n);
			};
			if (aside)
			{
				result.straggler = placed.others.back();
				fighterOf(result.straggler)->x += 300.0;
				result.gapStart = gapOf(result.straggler);
			}
			std::vector<double> lastX;
			std::vector<double> lastZ;
			for (const std::uint64_t id : placed.others)
			{
				Track track;
				track.id = id;
				track.side = sideOf[id];
				track.drawnAtStart = fighterOf(id)->weapon.weaponType != 5;
				result.tracks.push_back(track);
				lastX.push_back(fighterOf(id)->x);
				lastZ.push_back(fighterOf(id)->z);
			}
			for (int tick = 0; tick < kMaxTicks; ++tick)
			{
				sim.tick();
				if (aside && tick == 60)
					result.gapLater = gapOf(result.straggler);
				for (std::size_t i = 0; i < result.tracks.size(); ++i)
				{
					Track& track = result.tracks[i];
					const state::Fighter* f = fighterOf(track.id);
					const double speed = std::hypot(f->x - lastX[i], f->z - lastZ[i]) / 0.05;
					lastX[i] = f->x;
					lastZ[i] = f->z;
					if (track.firstTarget < 0 && f->target != 0)
						track.firstTarget = tick;
					if (track.drawn < 0 && f->weapon.weaponType != 5)
						track.drawn = tick;
					double& peak = track.firstTarget < 0 ? track.peakBefore : track.peakAfter;
					peak = std::max(peak, speed);
					if (track.firstTarget >= 0 && track.runSpeed == 0.0)
						track.runSpeed = sim.combat()->runSpeed(*f);
				}
			}
			for (Track& track : result.tracks)
			{
				const state::Fighter* f = fighterOf(track.id);
				track.sensed = f->target == 0 || perception::aware(*f, f->target);
				if (track.runSpeed == 0.0)
					track.runSpeed = sim.combat()->runSpeed(*f);
			}
			return result;
		};

		const Result plain = play(false);
		int earliest = kMaxTicks;
		int latest = -1;
		std::size_t sheathedAtStart = 0;
		for (const Track& track : plain.tracks)
		{
			std::printf("member %llu side %d: weapon drawn at start %d, first target %.2f s, drawn %.2f s, peak step speed before the target %.1f / after %.1f u/s (run speed %.1f)\n",
				static_cast<unsigned long long>(track.id), track.side, track.drawnAtStart ? 1 : 0, track.firstTarget * 0.05, track.drawn * 0.05,
				track.peakBefore, track.peakAfter, track.runSpeed);
			test::require(track.firstTarget >= 0, "a member took no target in " + std::to_string(kMaxTicks / 20) + " s");
			// Each member's own sensor record carries its target (perception::aware): a squad shares no sighting, so a member with no record of its own has
			// no fight goal 16 (rwh.102 section 1). Asked at the end of the window, while the fight holds the record fresh.
			test::require(track.sensed, "a member holds a target with no sensor record of its own");
			earliest = std::min(earliest, track.firstTarget);
			latest = std::max(latest, track.firstTarget);
			// The draw is a plan step (task 6) taken after the goal: a sheathed body never has the weapon in hand before its own first target.
			if (!track.drawnAtStart)
			{
				++sheathedAtStart;
				test::require(track.drawn >= track.firstTarget, "a sheathed member drew its weapon before it had a target");
			}
			// Gait: no committed target, no run (the task layer walks at the race walk speed, 15); a committed member reaches its run speed.
			test::require(track.peakBefore <= plain.walkSpeed * 1.05, "a member with no target moved faster than the walk speed");
			test::require(track.peakAfter >= 0.6 * track.runSpeed, "a member with a target never got near its run speed");
		}
		// Spawned sheathed (rwh.91 section 2): every body has weapon_type 5 at the start, the Berserkers' oversized Fragment Axe too (the weapon stays equipped in
		// the back slot, Inventory::kSheathed, not in the pack).
		test::require(sheathedAtStart == plain.tracks.size(), "the bodies did not all spawn sheathed (" + std::to_string(sheathedAtStart) + ")");
		// Per-member acquisition: the first targets come over a spread (the sensor scans are staggered by dice key), never all in one tick.
		test::require(latest - earliest >= 2, "the first targets came within 0.1 s of each other (" + std::to_string((latest - earliest) * 0.05) + " s)");
		std::printf("spectate_stagger: first targets %.2f .. %.2f s, %zu of %zu bodies sheathed at the start\n", earliest * 0.05, latest * 0.05, sheathedAtStart, plain.tracks.size());

		// A member out of its enemy's sight holds no record, so it has no goal 16: the world selector gives it its package's follow 141 (the file's squad runs
		// the package like any world squad), and the task walks it at the race walk speed - not the run - toward its leader.
		const Result aside = play(true);
		const Track* far = nullptr;
		for (const Track& track : aside.tracks)
			if (track.id == aside.straggler)
				far = &track;
		test::require(far != nullptr, "the moved member is tracked");
		const std::string follows = "goal 141 follow member " + std::to_string(aside.straggler);
		test::require(std::any_of(aside.log.begin(), aside.log.end(), [&](const std::string& line) { return line.rfind(follows, 0) == 0; }),
			"the member out of sight never took its package's follow (141)");
		test::require(far->peakBefore > 1.0, "the member out of sight stood still on its package task (peak " + std::to_string(far->peakBefore) + " u/s)");
		test::require(aside.gapLater < 0.8 * aside.gapStart, "the member out of sight did not close on its squad (" + std::to_string(aside.gapStart) + " -> " + std::to_string(aside.gapLater) + " u)");
		std::printf("spectate_stagger: the member moved 300 u aside followed on 141 (gap %.0f -> %.0f u in 3 s, peak %.1f u/s, walk speed %.1f) until its first target at %.2f s\n",
			aside.gapStart, aside.gapLater, far->peakBefore, aside.walkSpeed, far->firstTarget * 0.05);
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ksim_spectate_stagger: " << e.what() << "\n";
		return 1;
	}
}
