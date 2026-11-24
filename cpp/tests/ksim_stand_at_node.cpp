// STAND_AT_NODE 51 (and 47 / 37) through the generic path on the town node DB (kenshi-k16.6.12.8, rwh.55 sections 2-5):
//   - the data's `Node Generaal` record (is node, node type 1) classifies as the GeneralNode site kind and the layouts that hold it index real
//     general nodes, each with a facing;
//   - a town character whose package carries 51 reserves a general node of its home building, stands on it and faces its stored direction;
//   - two characters never hold one node (the routine book and the Routines agree);
//   - the reservation is released when the goal ends (the package changes to one without 51).
// No shipped package names 37 / 47 / 51, so the package is a synthetic AI_PACKAGE added to a copy of the merged data.

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/ai/blackboard.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace
{

using namespace ksim;

constexpr const char* kNodeRecord = "1184-gamedata.base"; // `Node Generaal `: is node, node type 1
constexpr std::int32_t kAiTask = 22;
constexpr std::int32_t kAiPackage = 72;
constexpr const char* kNodePackage = "ksim_test_stand_at_node";
constexpr const char* kIdlePackage = "ksim_test_idle";

class StraightNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }
	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}
};

void check(bool condition, const std::string& what)
{
	test::require(condition, what);
	std::printf("ok  %s\n", what.c_str());
}

std::string taskRecord(const kdata::GameData& data, std::int32_t taskEnum)
{
	for (const kdata::Record* record : data.byType(kAiTask))
		if (record->intOr("enum", -1) == taskEnum)
			return record->stringID;
	return {};
}

void rowsOf(kdata::Record& record, const char* category, const std::string& task)
{
	kdata::RefCategory entries;
	entries.category = category;
	kdata::RefEntry entry;
	entry.targetID = task;
	entry.v0 = 0;
	entry.v1 = 24;
	entry.v2 = 0;
	entry.hasV1V2 = true;
	entries.entries.push_back(entry);
	record.references.push_back(std::move(entries));
}

kdata::Record package(const std::string& sid, const std::string& task)
{
	kdata::Record record;
	record.type = kAiPackage;
	record.stringID = sid;
	record.name = sid;
	for (const char* category : {"Squad AI Goals", "Squad2 AI Goals", "Leader AI Goals"})
		rowsOf(record, category, task);
	return record;
}

// ---- 1. the data ----

struct Home
{
	std::uint64_t key = 0;
	std::size_t nodes = 0;
};

Home nodeBuilding(const Simulation& sim, const kdata::GameData& data)
{
	const kdata::Record* node = data.find(kNodeRecord);
	check(node != nullptr && node->boolOr("is node", false) && node->intOr("node type", 1) == routines::kNodeGeneral, "the data's Node Generaal is an `is node`, node type 1 record");
	check(routines::classifyPiece(data, *node) == routines::SiteKind::GeneralNode, "it classifies as the GeneralNode site kind");
	const routines::LayoutLibrary* layouts = sim.interiorLayouts();
	test::require(layouts != nullptr, "the layout library");
	Home best;
	for (const BuildingSite& building : sim.world().buildingSites())
	{
		if (building.ruined || building.interiorLayout.empty())
			continue;
		const routines::Layout* layout = layouts->find(building.buildingSid + "-" + building.interiorLayout);
		if (layout == nullptr)
			continue;
		std::size_t general = 0;
		for (const routines::Piece& piece : routines::classifyLayout(data, *layout))
			general += piece.kind == routines::SiteKind::GeneralNode;
		if (general > best.nodes)
			best = {building.key, general};
	}
	return best;
}

// ---- the sim ----

Simulation newGame(const kworld::LoadOrder& order, const std::shared_ptr<const kdata::GameData>& data)
{
	SpawnOptions options;
	options.seed = 12;
	options.wildernessByDensity = false;
	Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
	sim.setResidentsArrive(false);
	sim.setGameData(data);
	return sim;
}

void enable(Simulation& sim, const std::shared_ptr<const CombatData>& combat)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::make_shared<StraightNavigator>();
	sim.enableAi(std::move(config));
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(combat, settings);
}

void changePackage(Simulation& sim, flecs::entity squad, const std::string& sid)
{
	blackboard::Board* board = sim.blackboardOf(squad);
	test::require(board != nullptr, "the squad has a blackboard");
	blackboard::Package pkg;
	pkg.sid = sid;
	blackboard::changeAI(*board, std::move(pkg));
}

struct Stand
{
	std::uint64_t id = 0;
	routines::Site site;
};

std::vector<Stand> standers(Simulation& sim, const std::vector<flecs::entity>& members)
{
	std::vector<Stand> out;
	for (const flecs::entity member : members)
	{
		const state::Routine* routine = member.get<state::Routine>();
		if (routine != nullptr && routine->goal == routines::task::StandAtNode)
			out.push_back({member.get<state::Id>()->value, {static_cast<routines::SiteKind>(routine->target.kind), routine->target.object, routine->target.slot}});
	}
	(void)sim;
	return out;
}

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		auto data = std::make_shared<kdata::GameData>(kworld::loadMergedGameData(order));
		// The AI_TASK record of an enum, or a synthetic one when the shipped data has none (51 has no package row, so no record is needed).
		kdata::File file;
		file.version = 16;
		const auto taskOf = [&](std::int32_t taskEnum)
		{
			std::string sid = taskRecord(*data, taskEnum);
			if (sid.empty())
			{
				kdata::Record record;
				record.type = kAiTask;
				record.stringID = "ksim_test_task_" + std::to_string(taskEnum);
				record.name = record.stringID;
				record.ints.push_back({"enum", taskEnum});
				record.ints.push_back({"classification", 2});
				sid = record.stringID;
				file.records.push_back(std::move(record));
			}
			return sid;
		};
		const std::string task51 = taskOf(routines::task::StandAtNode);
		const std::string task43 = taskOf(routines::task::StayInHome);
		{
			file.records.push_back(package(kNodePackage, task51));
			file.records.push_back(package(kIdlePackage, task43));
			data->apply(file);
		}
		const std::shared_ptr<const kdata::GameData> shared = data;
		const std::shared_ptr<const CombatData> combat = CombatData::build(*shared, KENSHI_DATA);

		Simulation sim = newGame(order, shared);
		enable(sim, combat);
		const Home home = nodeBuilding(sim, *shared);
		check(home.key != 0 && home.nodes >= 3, "a placed building's layout holds general nodes (" + std::to_string(home.nodes) + ")");

		// A resident squad of three or more takes that building as its home.
		flecs::entity squad;
		ecsOf(sim).each([&](flecs::entity e, const state::Id&, const state::SquadInstance& instance, const state::Position&)
		{
			if (!squad.is_valid() && instance.source == state::SpawnSource::TownResident && instance.homeTown.id != 0
				&& membersOf(sim, e).size() >= 3 && membersOf(sim, e).size() <= 6)
				squad = e;
		});
		test::require(squad.is_valid(), "a resident squad of 3 to 6 members");
		state::SquadInstance instance = *squad.get<state::SquadInstance>();
		instance.homeBuilding = home.key;
		squad.set<state::SquadInstance>(instance);
		const state::Position at = *squad.get<state::Position>();

		sim.enqueue(Command::setPlayerAreas({{at.x, at.z}}));
		sim.run(1);
		for (int i = 0; i < 3000 && !squad.has<state::SquadRoutine>(); ++i)
			sim.run(1);
		test::require(squad.has<state::SquadRoutine>(), "the resident squad is promoted");
		changePackage(sim, squad, kNodePackage);
		std::vector<Stand> stands;
		for (int i = 0; i < 4000 && stands.size() < membersOf(sim, squad).size(); ++i)
		{
			sim.run(1);
			stands = standers(sim, membersOf(sim, squad));
		}
		const std::vector<flecs::entity> members = membersOf(sim, squad);
		for (const flecs::entity member : members)
		{
			const state::Routine* routine = member.get<state::Routine>();
			std::printf("member %llu: goal %d target kind %d object %llu\n", (unsigned long long)member.get<state::Id>()->value, routine != nullptr ? routine->goal : -1,
				routine != nullptr ? routine->target.kind : -1, routine != nullptr ? (unsigned long long)routine->target.object : 0ULL);
		}
		std::printf("stats: adopted %llu blocked %llu noRoute %llu\n", (unsigned long long)sim.townScorerStats().adopted, (unsigned long long)sim.townScorerStats().blocked, (unsigned long long)sim.townScorerStats().noRoute);
		check(stands.size() >= 2 && stands.size() == members.size(), "every member of the squad takes 51 STAND_AT_NODE (" + std::to_string(stands.size()) + " of " + std::to_string(members.size()) + ")");

		// Let the walks end, then: reserved from the DB, stood on, faced.
		// No navmesh in this headless run (the walk is the world selector's, covered where a knav fixture is), so the members are set
		// on their nodes' doorstep; the executor must then hold them there and turn them to the stored direction.
		for (const Stand& stand : stands)
		{
			const routines::SiteEntry* node = sim.siteEntry(stand.site);
			test::require(node != nullptr, "the held node has an entry");
			state::Fighter* f = fighterOf(sim, stand.id);
			f->x = node->x;
			f->z = node->z;
		}
		sim.run(routines::kScoreIntervalTicks * 2);
		stands = standers(sim, members);
		std::set<std::pair<std::uint64_t, std::uint16_t>> nodes;
		for (const Stand& stand : stands)
		{
			check(stand.site.kind == routines::SiteKind::GeneralNode && stand.site.object != 0, "member " + std::to_string(stand.id) + " holds a general node of a building");
			nodes.insert({stand.site.object, stand.site.slot});
			const routines::Claim* claim = sim.routineBook().holder(stand.site);
			check(claim != nullptr && claim->owner == stand.id, "the node's reservation is the member's own");
			const routines::SiteEntry* node = sim.siteEntry(stand.site);
			test::require(node != nullptr && !node->synthetic, "the node is a real layout piece of the DB");
			const state::Fighter* f = sim.combat()->find(stand.id);
			check(std::hypot(f->x - node->x, f->z - node->z) < 4.0, "the member stands on its node");
			const double want = std::atan2(node->faceX, node->faceZ);
			const double turn = std::remainder(f->yaw - want, 2.0 * std::numbers::pi);
			check(std::fabs(turn) < 0.1, "and faces the node's stored direction");
		}
		check(nodes.size() == stands.size(), "two characters never hold one node (" + std::to_string(nodes.size()) + " nodes, " + std::to_string(stands.size()) + " holders)");

		// The goal ends: the package no longer lists 51, so every node is released.
		changePackage(sim, squad, kIdlePackage);
		sim.run(routines::kScoreIntervalTicks * 4);
		std::size_t held = 0;
		for (const Stand& stand : stands)
		{
			const routines::Claim* claim = sim.routineBook().holder(stand.site);
			held += claim != nullptr && claim->owner == stand.id;
		}
		check(standers(sim, members).empty() && held == 0, "when the goal ends every node is released");
		std::cout << "ksim_stand_at_node: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_stand_at_node: FAILED: " << e.what() << "\n";
		return 1;
	}
}
