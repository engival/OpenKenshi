// The follow family of the native selector (task ds/follow-bed; kinspect dt24_141): 141 FOLLOW_SQUADLEADER and 171 FOLLOW_SLAVEMASTER are
// one class, Task_FollowLeader, with one resolver - the character in my captor hand, else the platoon leader.
//   - 141: a beak squad's members (their package's only calm goal is 141) are chosen onto the follow by the live selector and end
//     up following the squad's leader;
//   - 171: a member in the Slave list with a captor (slaveState 1 and a follow target that is not the leader) keeps following that
//     captor - the squad's own leader follow must not take it back - and a free member with the same follow goes back to the leader.
// No NPC is ordered: the cases only set the state a cage release or a cuff would have written (slave state, the captor hand).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/crime.hpp"
#include "ksim/detail.hpp"
#include "ksim/factions.hpp"
#include "ksim/goap.hpp"
#include "ksim/navigator.hpp"
#include "ksim/routines.hpp"
#include "ksim/simulation.hpp"
#include "ksim/trader.hpp"
#include "ksim/world.hpp"

#include "world_resolvers.hpp"

#include "knav/navmesh.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
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

// Heft's grid cell (the baked world's region_11_4.knav) and its town centre (the cases run inside its 2000 u cage finder).
constexpr int kHeftCellX = 46;
constexpr int kHeftCellZ = 16;
constexpr double kHeftX = 68772.98;
constexpr double kHeftZ = -71496.61;
// The kidnapper template (Squad role carries both 254 and 112), placed as the town's slavers in the 254 case.
constexpr const char* kKidnappers = "96357-rebirth.mod";
// The slave-carrying guard template (its Slave list carries 172): the UC storehouses' 'Storage Guard' package.
constexpr const char* kSlaverGuards = "56041-rebirth.mod";
// The wandering-trader caravan (the 39 case).
constexpr const char* kCaravan = "1161-gamedata.base";

std::ostream& say()
{
	static std::ostream nowhere(nullptr);
	return std::getenv("KSIM_FOLLOW_GOALS_VERBOSE") != nullptr ? std::cout : nowhere;
}

std::vector<flecs::entity> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each(
		[&](flecs::entity member, const state::Id& id) { found.emplace_back(id.value, member); });
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<flecs::entity> out;
	for (const auto& [id, member] : found)
		out.push_back(member);
	return out;
}

std::uint64_t idOf(flecs::entity e)
{
	return e.get<state::Id>()->value;
}

struct Game
{
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	std::vector<std::string> taskLog;

	explicit Game(const std::string& bake)
	{
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		std::vector<kworld::ZoneCell> zones;
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
				zones.push_back({kHeftCellX + dx, kHeftCellZ + dz});
		mesh = test::openWorldNav(bake, zones);
		SpawnOptions options;
		options.seed = 1;
		options.wildernessPopulation = 0.0;
		options.nestMultiplier = 0.0;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim->enableCombat(combat, settings);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
		sim->setTaskLog([this](const std::string& line) { taskLog.push_back(line); });
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
			if (record->name == "Wanderer")
				sid = record->stringID;
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
			if (member.fighter.playerControlled)
				player = member.id;
	}

	// The player's camera is the squad's spot (the area streams to Full and the world squads build Fighters); the player CHARACTER
	// stands well away, so the placed squad has nobody to fight.
	void bringPlayer(double x, double y, double z)
	{
		if (state::Fighter* me = fighterOf(*sim, player))
		{
			me->x = x + 400.0;
			me->y = y;
			me->z = z + 400.0;
			me->clearRoute();
			me->down = false;
			me->unconscious = false;
		}
	}

	flecs::entity place(const char* templateSid, double x, double z)
	{
		const flecs::entity squad = sim->placeWildernessSquad(templateSid, x, z);
		bringPlayer(x, sim->groundHeight(x, z).value_or(0.0), z);
		sim->enqueue(Command::setPlayerAreas({{x, z}}));
		sim->run(1);
		for (int i = 0; i < 20 * 20; ++i)
		{
			sim->run(1);
			const std::vector<flecs::entity> members = membersOf(*sim, squad);
			if (!members.empty() && std::all_of(members.begin(), members.end(),
				[&](flecs::entity member)
				{
					const state::Id* id = member.get<state::Id>();
					return id != nullptr && sim->combat()->find(id->value) != nullptr;
				}))
				break;
		}
		return squad;
	}

	void tick(int n = 1)
	{
		for (int i = 0; i < n; ++i)
			sim->run(1);
	}
};


// The squad templates, by name (the record ids differ with the load order): "Beak Squad Small" has Squad [21, 141, 143]; "Leviathan Family" has
// Squad [16, 141, 143, 175] and Slave [41, 143, 171].
std::string squadSid(const Game& game, const std::string& name)
{
	for (const kdata::Record* record : game.sim->gameData()->byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		if (record->name == name)
		{
			say() << "  squad " << name << " = " << record->stringID << "\n";
			return record->stringID;
		}
	test::require(false, "no squad template named " + name);
	return {};
}

// The resolver on its own (dt24_141 vf6 14034bac0): the captor hand first, else the platoon leader, else the first live member.
void testResolver()
{
	std::map<std::uint64_t, state::Fighter> world;
	for (const std::uint64_t id : {1, 2, 3, 4})
		world[id] = state::Fighter{};
	const auto lookup = [&](std::uint64_t id) -> const state::Fighter* { return world.count(id) != 0 ? &world[id] : nullptr; };
	const std::vector<std::uint64_t> platoon = {1, 2, 3, 4};
	state::Fighter self;
	self.slaveState = 1;
	self.followTarget = 3; // the captor hand
	test::require(worldres::follow141(self, 2, lookup, 1, platoon) == 3, "171: the captor hand answers first");
	self.slaveState = 0;
	test::require(worldres::follow141(self, 2, lookup, 1, platoon) == 1, "141: a free member follows the platoon leader");
	self.slaveState = 1;
	world[3].dead = true;
	test::require(worldres::follow141(self, 2, lookup, 1, platoon) == 1, "a dead captor falls through to the platoon leader");
	world[1].down = true;
	test::require(worldres::follow141(self, 2, lookup, 1, platoon) == 4, "a down leader falls through to the first live member");
	world[4].unconscious = true;
	test::require(worldres::follow141(self, 2, lookup, 1, platoon) == 0, "nobody to follow: no offer");
	world[4].unconscious = false;
	self.crouched = true;
	test::require(worldres::follow141(self, 2, lookup, 1, platoon) == 0, "IS_CROUCHING_OR_LYING = 0 is a precondition");
	self.crouched = false;
	test::require(worldres::follow141(self, 4, lookup, 4, platoon) == 0 || worldres::follow141(self, 4, lookup, 4, platoon) != 4,
		"a member never follows itself");
}

void testFollowLeader(Game& game)
{
	const flecs::entity squad = game.place(squadSid(game, "Beak Squad Small").c_str(), kHeftX - 900.0, kHeftZ + 900.0);
	const std::vector<flecs::entity> members = membersOf(*game.sim, squad);
	test::require(members.size() >= 2, "the beak squad has a leader and a follower (" + std::to_string(members.size()) + ")");
	const std::uint64_t leader = idOf(members[0]);
	// Let the squad settle, then drop every follower's follow: the squad must end up following its leader again.
	game.tick(20 * 5);
	for (std::size_t i = 0; i < members.size(); ++i)
		say() << "  before: member " << idOf(members[i]) << " followTarget " << fighterOf(*game.sim, idOf(members[i]))->followTarget << " hasRoutine " << members[i].has<state::Routine>() << "\n";
	for (std::size_t i = 1; i < members.size(); ++i)
		CombatArena::endFollow(*fighterOf(*game.sim, idOf(members[i])));
	game.tick(20 * 20);
	std::size_t following = 0;
	for (std::size_t i = 1; i < members.size(); ++i)
	{
		const state::Fighter* f = fighterOf(*game.sim, idOf(members[i]));
		say() << "  member " << idOf(members[i]) << " followTarget " << f->followTarget << " (leader " << leader << ")\n";
		following += f->followTarget == leader ? 1 : 0;
	}
	test::require(following == members.size() - 1, "every follower follows the squad's leader");
}

void testFollowSlaveMaster(Game& game)
{
	const flecs::entity squad = game.place(squadSid(game, "Leviathan Family").c_str(), kHeftX - 900.0, kHeftZ - 900.0);
	const std::vector<flecs::entity> members = membersOf(*game.sim, squad);
	test::require(members.size() >= 3, "the Leviathan family has a leader, a captor and a slave (" + std::to_string(members.size()) + ")");
	const std::uint64_t leader = idOf(members[0]);
	const std::uint64_t captor = idOf(members[1]);
	const flecs::entity slave = members[2];
	const std::uint64_t slaveId = idOf(slave);
	game.tick(20 * 5);
	// The state a cage release leaves (world_tasks.cpp GET_SLAVE): the slave state and the captor it follows. Role list 3 is the
	// package's Slave list (171's row); the data's "Slave AI Goals" are what a member in that list runs.
	{
		state::MemberRole role = *slave.get<state::MemberRole>();
		role.list = 3;
		slave.set<state::MemberRole>(role);
	}
	state::Fighter* f = fighterOf(*game.sim, slaveId);
	CombatArena::endFollow(*f);
	f->slaveState = 1;
	f->followTarget = captor;
	f->followSource = state::Fighter::PlanSource::Plan;
	game.taskLog.clear();
	game.tick(20 * 25);
	f = fighterOf(*game.sim, slaveId);
	say() << "  slave " << slaveId << " followTarget " << f->followTarget << " captor " << captor << " leader " << leader << "\n";
	test::require(f->followTarget == captor, "a slave keeps following its captor, not the squad leader");
	// Free the member (the cuffs come off): the same follow now resolves to the platoon leader.
	f->slaveState = 0;
	game.tick(20 * 25);
	f = fighterOf(*game.sim, slaveId);
	say() << "  freed: followTarget " << f->followTarget << "\n";
	test::require(f->followTarget == leader, "a freed member follows the platoon leader");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_follow_goals KSIM_NAVWORLD");
		testResolver();
		{
			Game game(argv[1]);
			testFollowLeader(game);
		}
		{
			Game game(argv[1]);
			testFollowSlaveMaster(game);
		}
		std::cout << "ksim_follow_goals: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_follow_goals: FAILED: " << e.what() << "\n";
		return 1;
	}
}
