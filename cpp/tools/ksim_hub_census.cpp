// ksim_hub_census: the server's new game at the Hub (squad AI and streaming navmesh as the server runs it), then a TSV of every character within R of a
// centre at t = 0 (the first ticks after NEW_GAME) and after SECONDS of game time at 1x, for the comparison with a vanilla census (kenshi-k16.6.23).
//
//   ksim_hub_census NAVWORLD [--seconds S] [--x X] [--z Z] [--radius R] [--seed N]
//
// Columns: name faction squad x y z yaw state goal plan step target(site kind:object:slot) mode building floor clip. `goal` is the AI_TASK enum of the
// member's routine (vanilla numbering); `mode` is seated / walking / standing / asleep; `clip` the pose tail's clip for a seated or asleep member.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"

#include "kdata/gamedata.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

flecs::world& ecsOf(Simulation& sim)
{
	return const_cast<flecs::world&>(sim.world().ecs());
}

// One line per building a dumped member stands in: the placement's record, label, ruined flag, and its interior layout's pieces by `functionality` function id
// (6 bed, 9 counter, 16 table, 17 chair, 24 throne; `node` = an `is node` piece, `other` = anything else).
void dumpBuilding(Simulation& sim, std::uint64_t key)
{
	const BuildingSite* site = sim.world().buildingSite(key);
	if (site == nullptr)
	{
		std::printf("building\t%llu\t(no site)\n", static_cast<unsigned long long>(key));
		return;
	}
	const kdata::Record* record = sim.gameData()->find(site->buildingSid);
	std::map<std::string, int> counts;
	const routines::LayoutLibrary* library = sim.interiorLayouts();
	const routines::Layout* layout = library != nullptr ? library->find(site->buildingSid + "-" + site->interiorLayout) : nullptr;
	if (layout == nullptr && library != nullptr && !library->forBuilding(site->buildingSid).empty())
		layout = library->forBuilding(site->buildingSid).front();
	for (const routines::LayoutPiece& piece : layout != nullptr ? layout->pieces : std::vector<routines::LayoutPiece>{})
	{
		const kdata::Record* furniture = sim.gameData()->find(piece.target);
		std::string kind = "other";
		if (furniture != nullptr && furniture->boolOr("is node", false))
			kind = "node";
		else if (const kdata::RefCategory* functionality = furniture != nullptr ? furniture->findRefs("functionality") : nullptr; functionality != nullptr)
		{
			for (const kdata::RefEntry& entry : functionality->entries)
			{
				if (const kdata::Record* fn = sim.gameData()->find(entry.targetID); fn != nullptr)
					kind = "fn" + std::to_string(fn->intOr("function", 0));
			}
		}
		++counts[kind];
	}
	std::string pieces;
	for (const auto& [kind, n] : counts)
		pieces += (pieces.empty() ? "" : " ") + kind + "=" + std::to_string(n);
	std::printf("building\t%llu\t%s\t%s\tlabel=%s\ttemplate=%s\tlayout=%s\truined=%d\t%s\n", static_cast<unsigned long long>(key), site->buildingSid.c_str(),
		record != nullptr ? record->name.c_str() : "?", site->label.c_str(), site->squadTemplate.c_str(), layout != nullptr ? layout->sid.c_str() : "(none)", site->ruined ? 1 : 0,
		pieces.c_str());
}

void dump(Simulation& sim, const char* label, double cx, double cz, double radius)
{
	std::map<std::uint64_t, flecs::entity> entities;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id) { entities[id.value] = e; });
	std::set<std::uint64_t> buildings;
	std::fprintf(stderr, "## %s tick %llu\n", label, static_cast<unsigned long long>(sim.clockTick()));
	std::printf("# %s\nname\tfaction\tsquad\tx\ty\tz\tyaw\tstate\tgoal\tplan\tstep\ttarget\tmode\tbuilding\tfloor\tclip\n", label);
	for (const CombatArena::Member& m : sim.combat()->members())
	{
		const state::Fighter& f = m.fighter;
		if (f.playerControlled || std::hypot(f.x - cx, f.z - cz) > radius)
			continue;
		const flecs::entity e = entities.count(m.id) != 0 ? entities[m.id] : flecs::entity();
		std::string faction;
		std::string squad = "none";
		const state::Routine* routine = nullptr;
		if (e.is_valid())
		{
			routine = e.get<state::Routine>();
			if (const flecs::entity squadEntity = e.target<state::MemberOf>(); squadEntity.is_valid())
			{
				const state::SquadInstance* instance = squadEntity.get<state::SquadInstance>();
				const state::Id* squadId = squadEntity.get<state::Id>();
				const kdata::Record* record = instance != nullptr ? sim.gameData()->find(instance->templateSid) : nullptr;
				if (instance != nullptr && instance->faction.id != 0 && ecsOf(sim).is_alive(instance->faction.id))
				{
					const state::FactionState* fs = ecsOf(sim).entity(instance->faction.id).get<state::FactionState>();
					const kdata::Record* factionRecord = fs != nullptr ? sim.gameData()->find(fs->templateSid) : nullptr;
					faction = factionRecord != nullptr ? factionRecord->name : "?";
				}
				squad = (record != nullptr ? record->name : std::string("?")) + "#" + std::to_string(squadId != nullptr ? squadId->value : 0);
			}
		}
		double faceYaw = 0.0;
		const bool seated = sim.seated(m.id, &faceYaw);
		const std::optional<Simulation::Pose> pose = sim.pose(m.id);
		const char* mode = f.asleep ? "asleep" : seated ? "seated" : f.walking() ? "walking" : "standing";
		std::string plan;
		if (routine != nullptr)
		{
			for (const std::uint16_t task : routine->plan)
				plan += (plan.empty() ? "" : ",") + std::to_string(task);
		}
		if (f.building != 0)
			buildings.insert(f.building);
		std::printf("%s\t%s\t%s\t%.1f\t%.1f\t%.1f\t%.1f\t%d\t%u\t%s\t%u\t%u:%llu:%u\t%s\t%llu\t%d\t%s\n", f.name.c_str(), faction.c_str(), squad.c_str(), f.x, f.y,
			f.z, f.yaw * 180.0 / 3.14159265358979, static_cast<int>(f.state), routine != nullptr ? routine->goal : 0u, plan.c_str(),
			routine != nullptr ? routine->step : 0u, routine != nullptr ? routine->target.kind : 0u,
			static_cast<unsigned long long>(routine != nullptr ? routine->target.object : 0), routine != nullptr ? routine->target.slot : 0u, mode,
			static_cast<unsigned long long>(f.building), static_cast<int>(f.floor), pose ? pose->clip.c_str() : "");
	}
	for (const std::uint64_t key : buildings)
		dumpBuilding(sim, key);
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2)
		{
			std::cerr << "usage: ksim_hub_census NAVWORLD [--seconds S] [--x X] [--z Z] [--radius R] [--seed N]\n";
			return 2;
		}
		const std::string world = argv[1];
		double seconds = 30.0; // 1x: 30 s is 16.5 game minutes (kGameDaySeconds)
		double cx = -50986.0;
		double cz = 3514.0;
		double radius = 1200.0;
		std::uint64_t seed = 1;
		for (int i = 2; i + 1 < argc; i += 2)
		{
			const std::string key = argv[i];
			const double value = std::stod(argv[i + 1]);
			if (key == "--seconds") seconds = value;
			else if (key == "--x") cx = value;
			else if (key == "--z") cz = value;
			else if (key == "--radius") radius = value;
			else if (key == "--seed") seed = static_cast<std::uint64_t>(value);
		}
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = seed;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		sim.markPristine();
		const std::shared_ptr<const CombatData> combat = CombatData::build(*sim.gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combat, settings);
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		sim.enableNavigation(MeshNavigator::openWorld(world, hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		std::string sid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim.enqueue(Command::newGame(sid, "Tester", 1));
		sim.run(3);
		// t0 is the first tick the Hub's residents are Fighters (the promotion pass, a few ticks after NEW_GAME).
		const auto residents = [&]
		{
			std::size_t n = 0;
			for (const CombatArena::Member& m : sim.combat()->members())
				n += !m.fighter.playerControlled && std::hypot(m.fighter.x - cx, m.fighter.z - cz) <= radius ? 1 : 0;
			return n;
		};
		for (int i = 0; i < 400 && residents() == 0; ++i)
			sim.run(1);
		dump(sim, "t0", cx, cz, radius);
		sim.run(static_cast<std::uint64_t>(seconds / kTickSeconds));
		dump(sim, "t1", cx, cz, radius);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_hub_census: " << e.what() << "\n";
		return 1;
	}
}
