// Task raid-phases (kenshi-h2v.21.1; kinspect rwh.53 "the food raid" sections 1, 2.1, 4 and rwh.52): the phase packages of a raid campaign
// on the whole force.
//  1. data: the FactionBook carries pt1 / pt2 / retreat for the leader and the others (an empty `others` list takes the leader's), for
//     both rows of the table - the 6-hourly rolled Sand ninja raid and the trigger-only Stone Rat Assault - and the Shek tribute's
//     non-violent pt2.
//  2. a Sand ninja raid on a town, two built squads: pt1 (leader / others) while the order is outbound, pt2 "Assault Town and loot food"
//     on both once it is staying on site, the retreat package after the on-site time, and nothing once the order is over (the contract
//     ends with the campaign instance).
//  3. a stale target: an order on a town that is not in the world fails at once (NoTarget) and sends nobody anywhere.
// The squads are SQUAD_TEMPLATE "Sand ninja cell" built at Full detail on the server's new game and the navmesh bake (as ksim_raid_goals);
// the orders are written as the faction layer writes them (the group layer's own walk is not under test). Needs the bake (argv[1]; exit 77).

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/blackboard.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/newgame.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "live_world_rig.hpp"
#include "sim_access.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <memory>
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


const kdata::Record* recordNamed(const kdata::GameData& gd, const char* type, const std::string& name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName(type)))
		if (record->name == name)
			return record;
	return nullptr;
}

// The first record of the type whose name holds every needle.
const kdata::Record* recordLike(const kdata::GameData& gd, const char* type, std::initializer_list<const char*> needles)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName(type)))
	{
		bool all = true;
		for (const char* needle : needles)
			all = all && record->name.find(needle) != std::string::npos;
		if (all)
			return record;
	}
	return nullptr;
}

std::string nameOf(const kdata::GameData& gd, const std::string& sid)
{
	const kdata::Record* record = gd.find(sid);
	return record != nullptr ? record->name : "(none)";
}

// 1. The data.
void testData(const kdata::GameData& gd, const FactionBook& book)
{
	const auto campaignOf = [&](std::initializer_list<const char*> needles) -> const CampaignInfo&
	{
		const kdata::Record* record = recordLike(gd, "FACTION_CAMPAIGN", needles);
		test::require(record != nullptr, std::string("no FACTION_CAMPAIGN named like ") + *needles.begin());
		const CampaignInfo* info = book.campaign(record->stringID);
		test::require(info != nullptr, "the book has no campaign " + record->name);
		return *info;
	};
	const auto phase = [&](const CampaignInfo& info, std::size_t index, bool leader)
	{
		return nameOf(gd, leader ? info.phases[index].leader : info.phases[index].others);
	};
	const CampaignInfo& ninja = campaignOf({"Sand ninja standard raid"});
	test::require(ninja.territorial && ninja.chancePerDay > 0.0 && ninja.hostile, "the Sand ninja raid is not a rolled, hostile raid");
	test::require(phase(ninja, kPhaseAssemble, true) == "Military Assemble leader", "pt1 leader is " + phase(ninja, kPhaseAssemble, true));
	test::require(phase(ninja, kPhaseAssemble, false) == "Military Assemble forces", "pt1 others is " + phase(ninja, kPhaseAssemble, false));
	test::require(phase(ninja, kPhaseAssault, true) == "Assault Town and loot food", "pt2 leader is " + phase(ninja, kPhaseAssault, true));
	test::require(phase(ninja, kPhaseAssault, false) == "Assault Town and loot food", "pt2 others (the leader's list) is " + phase(ninja, kPhaseAssault, false));
	test::require(phase(ninja, kPhaseRetreat, true) == "Retreat and refresh", "retreat leader is " + phase(ninja, kPhaseRetreat, true));
	test::require(!ninja.phases[kPhaseRetreat].others.empty(), "no retreat package for the others");
	// The trigger-only row: never rolled (no territorial triggers, no chance), yet it carries the same loot package.
	const CampaignInfo& assault = campaignOf({"Stonerat", "Assault"});
	test::require(!assault.territorial && assault.chancePerDay == 0.0, "Stone Rat Assault is not trigger-only");
	test::require(phase(assault, kPhaseAssault, true) == "Assault Town and loot food", "Stone Rat Assault pt2 is " + phase(assault, kPhaseAssault, true));
	// The Shek tribute: non-hostile, the non-violent package.
	const CampaignInfo& shek = campaignOf({"Shek Visit Tribute"});
	test::require(!shek.hostile, "the Shek tribute is hostile");
	test::require(phase(shek, kPhaseAssault, true) == "Loot food then run away (non violent)", "Shek pt2 is " + phase(shek, kPhaseAssault, true));
	// The victory AI: data the exe never loads (rwh.53 4.1); the campaigns that name an occupy package for it (FACTIONS.md 2.1b).
	std::vector<std::string> victors;
	for (const CampaignInfo& info : book.campaigns())
		if (nameOf(gd, info.victory.leader) == "campaign victory occupy town")
			victors.push_back(info.name);
	std::sort(victors.begin(), victors.end());
	test::require(!victors.empty(), "no campaign names 'campaign victory occupy town' as its victory AI");
	std::cout << "  data: " << victors.size() << " campaigns name 'campaign victory occupy town'\n";
	if (std::getenv("KSIM_VICTORY_LIST"))
		for (const std::string& name : victors)
			std::cout << "    " << name << "\n";
	std::cout << "  data: Sand ninja pt1 '" << phase(ninja, kPhaseAssemble, true) << "' / '" << phase(ninja, kPhaseAssemble, false) << "', pt2 '"
		<< phase(ninja, kPhaseAssault, true) << "', retreat '" << phase(ninja, kPhaseRetreat, true) << "'; trigger-only and Shek rows read\n";
}

struct Game
{
	std::shared_ptr<kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<const FactionBook> book;
	std::shared_ptr<MeshNavigator> nav;
	Simulation sim;
	flecs::entity town; // the raid's target: a town away from the Hub
	double townX = 0.0;
	double townZ = 0.0;

	explicit Game(const std::string& bakeDir)
		: gd(std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA))))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, book(FactionBook::build(gd))
		, sim(Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA)))
	{
		sim.markPristine();
		ecsOf(sim).each([&](flecs::entity entity, const state::TownState& state)
		{
			if (!town.is_valid() && state.owner.id != 0 && std::hypot(state.x - kHubX, state.z - kHubZ) > 6000.0)
			{
				town = entity;
				townX = state.x;
				townZ = state.z;
			}
		});
		test::require(town.is_valid(), "no owned town away from the Hub");
		nav = test::openWorldNav(bakeDir, live_rig::hubZonesWith({{townX, townZ}}));
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.data = gd;
		ai.factions = book;
		ai.factionTuning.raidFrequency = 0.0; // no rolled raid disturbs the fixture
		sim.enableAi(std::move(ai));
		live_rig::startWanderer(sim, *gd);
	}

	// Two Sand ninja cells at the town, built at Full detail: the player character stands there (never ordered).
	std::vector<flecs::entity> placeForce(const std::string& templateSid)
	{
		live_rig::standPlayerAt(sim, townX, townZ);
		std::vector<flecs::entity> force;
		for (int i = 0; i < 2; ++i)
		{
			force.push_back(sim.placeTownSquad(templateSid, townX + 40.0 * i, townZ));
			test::require(force.back().is_valid(), "the Sand ninja cell does not spawn");
		}
		sim.run(300); // the resident pass promotes them
		return force;
	}

	std::string appliedOf(flecs::entity squad)
	{
		const blackboard::Board* board = sim.blackboardOf(squad);
		test::require(board != nullptr, "a squad has no blackboard");
		return board->current;
	}

	// The faction's order on every squad of the force, as commission() writes it.
	void order(const std::vector<flecs::entity>& force, const std::string& campaign, flecs::entity target, state::OrderStatus status)
	{
		for (const flecs::entity squad : force)
		{
			state::GroupAI group = squad.get<state::GroupAI>() != nullptr ? *squad.get<state::GroupAI>() : state::GroupAI{};
			group.order.status = status;
			group.order.target = state::EntityRef{target.id()};
			group.order.campaign = campaign;
			group.order.deadline = ~std::uint64_t(0);
			group.order.stay = group.order.retreat = 100000;
			squad.set<state::GroupAI>(std::move(group));
		}
	}
};

// Every member of the squad is on it (the board is the squad's: the members read its goal rows).
std::size_t memberCount(Game& game, flecs::entity squad)
{
	std::size_t count = 0;
	ecsOf(game.sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](const state::Id&) { ++count; });
	return count;
}

void testPhases(Game& game)
{
	const kdata::Record* ninjaCell = recordNamed(*game.gd, "SQUAD_TEMPLATE", "Sand ninja cell");
	test::require(ninjaCell != nullptr, "no SQUAD_TEMPLATE Sand ninja cell");
	const kdata::Record* campaignRecord = recordNamed(*game.gd, "FACTION_CAMPAIGN", "Sand ninja standard raid");
	const CampaignInfo& campaign = *game.book->campaign(campaignRecord->stringID);
	const std::vector<flecs::entity> force = game.placeForce(ninjaCell->stringID);
	flecs::entity lead = force[0];
	flecs::entity rest = force[1];
	if (lead.get<state::Id>()->value > rest.get<state::Id>()->value)
		std::swap(lead, rest);
	for (const flecs::entity squad : force)
		test::require(memberCount(game, squad) > 1, "a squad of the force has no built members");

	const auto expect = [&](const char* when, std::size_t phase)
	{
		game.sim.run(60);
		test::require(game.appliedOf(lead) == campaign.phases[phase].leader,
			std::string(when) + ": the leader squad carries '" + nameOf(*game.gd, game.appliedOf(lead)) + "', not '"
				+ nameOf(*game.gd, campaign.phases[phase].leader) + "'");
		test::require(game.appliedOf(rest) == campaign.phases[phase].others,
			std::string(when) + ": the other squad carries '" + nameOf(*game.gd, game.appliedOf(rest)) + "', not '"
				+ nameOf(*game.gd, campaign.phases[phase].others) + "'");
	};
	game.order(force, campaignRecord->stringID, game.town, state::OrderStatus::Outbound);
	expect("pt1 assemble", kPhaseAssemble);
	game.order(force, campaignRecord->stringID, game.town, state::OrderStatus::Staying);
	expect("pt2 assault", kPhaseAssault);
	test::require(nameOf(*game.gd, game.appliedOf(rest)) == "Assault Town and loot food", "the force does not carry the loot package");
	game.order(force, campaignRecord->stringID, game.town, state::OrderStatus::Retreating);
	expect("retreat", kPhaseRetreat);
	// Over: the instance is gone, the contract finishes, the template's own ladder is back.
	game.order(force, campaignRecord->stringID, game.town, state::OrderStatus::Done);
	game.sim.run(60);
	for (const flecs::entity squad : force)
		test::require(game.appliedOf(squad) != campaign.phases[kPhaseRetreat].leader && game.appliedOf(squad) != campaign.phases[kPhaseRetreat].others,
			"the retreat package outlived the order");
	std::cout << "  phases: pt1 -> pt2 'Assault Town and loot food' on both squads -> retreat -> released\n";

	// The victory AI behind its switch: a campaign that names an occupy package for its victory runs it on the Retreating status (the force
	// holds the town) with the switch on, and the retreat package with it off (the exe's behaviour).
	const CampaignInfo* victor = nullptr;
	for (const CampaignInfo& info : game.book->campaigns())
		if (victor == nullptr && nameOf(*game.gd, info.victory.leader) == "campaign victory occupy town" && !info.phases[kPhaseRetreat].leader.empty())
			victor = &info;
	test::require(victor != nullptr, "no victory campaign with a retreat package");
	AiSystem* ai = const_cast<AiSystem*>(game.sim.ai());
	test::require(!ai->config().victoryAi, "the campaign victory AI is on by default: the original never loads it (world.campaign_victory_ai, kenshi-h2v.22.5)");
	for (const bool on : {true, false})
	{
		ai->setVictoryAi(on);
		game.order(force, victor->sid, game.town, state::OrderStatus::Retreating);
		game.sim.run(60);
		const std::string& want = on ? victor->victory.leader : victor->phases[kPhaseRetreat].leader;
		test::require(game.appliedOf(lead) == want, std::string("victory AI ") + (on ? "on" : "off") + ": the leader carries '"
			+ nameOf(*game.gd, game.appliedOf(lead)) + "', not '" + nameOf(*game.gd, want) + "'");
		game.order(force, victor->sid, game.town, state::OrderStatus::Done);
		game.sim.run(60);
	}
	ai->setVictoryAi(false); // the default
	std::cout << "  victory AI: '" << victor->name << "' occupies on Retreating with the switch on, retreats with it off\n";

	// 3. A stale target: an order on something that is not a town is failed, and the squad walks nowhere.
	game.order(force, campaignRecord->stringID, force[0], state::OrderStatus::Issued);
	game.sim.run(80);
	for (const flecs::entity squad : force)
	{
		const state::GroupAI* group = squad.get<state::GroupAI>();
		test::require(group != nullptr && group->order.status == state::OrderStatus::Failed && group->order.failure == state::AiReason::NoTarget,
			"a squad ordered on a stale target town was not failed with NoTarget");
		test::require(group->goal != state::GoalKind::Raid, "a squad with a stale target took the raid goal");
	}
	std::cout << "  stale target: the order failed with NoTarget, nobody was sent\n";
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_raid_phases: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		Game game(argv[1]);
		testData(*game.gd, *game.book);
		testPhases(game);
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_raid_phases OK\n";
	return 0;
}
