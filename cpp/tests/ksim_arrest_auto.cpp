// Arrest with no order (task ds/arrest-2; CRIME_STATIC "What guards do (arrest chain)", DIALOGUE.md EV_BOUNTY_SPOTTED):
//   - a player with a bounty held by the guards' legal faction walks past UC police at Heft: the wanted reaction, the law dialogue, DA 29, 240, the cage and
//     the sentence all fire on their own, and the cage owner (Slave Traders) answers to the same legal system as the guards (no order but the player's placement)
//   - a bounty of another legal system (Holy Nation) starts nothing

#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/law/crime.hpp"
#include "ksim/core/detail.hpp"
#include "ksim/dialogue/dialogue_events.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/ai/goap.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/trader.hpp"
#include "ksim/world/world.hpp"

#include "knav/navmesh.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "nav_source.hpp"
#include "test_util.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// Heft's grid cell (the baked world's region_11_4.knav) and its town centre (the cases run inside its 2000 u cage finder).
constexpr int kHeftCellX = 46;
constexpr int kHeftCellZ = 16;
constexpr double kHeftX = 68772.98;
constexpr double kHeftZ = -71496.61;
// The kidnapper template (Squad role carries both 254 and 112), placed as the town's slavers in the 254 case.
// The slave-carrying guard template (its Slave list carries 172): the UC storehouses' 'Storage Guard' package.
// The wandering-trader caravan (the 39 case).

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

// The FactionState of a faction record (the cage owner, the captor).
struct FactionLookup
{
	std::uint64_t entity = 0; // the flecs entity id
	std::uint64_t id = 0; // its state::Id value (the id a Cage's faction and a captor's resolve hold)
	std::string sid;
};

FactionLookup factionBySid(Simulation& sim, const std::string& sid)
{
	FactionLookup out;
	ecsOf(sim).each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (faction.templateSid != sid)
			return;
		out.entity = entity.id();
		out.sid = faction.templateSid;
		const state::Id* id = entity.get<state::Id>();
		out.id = id != nullptr ? id->value : static_cast<std::uint64_t>(entity.id());
	});
	return out;
}

struct Game
{
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;
	std::vector<std::string> taskLog;

	// `idBump`: ids skipped before the new game, so the run does not depend on how the world's entity ids happen to be numbered (k16.7.25: a +5 shift flipped the
	// capture race this case once leaned on).
	explicit Game(const std::string& bake, int idBump = 0)
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
		sim->enableDialogue(DialogueBook::build(sim->gameData()));
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
		const_cast<flecs::world&>(sim->world().ecs()).get_mut<state::Ids>()->next += static_cast<std::uint64_t>(idBump);
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

// The cage-bearing home the two Heft cases use: the resident Cage Warden squads' home building (a slave shop whose placement is the
// Slave Traders'). Its building sites are materialised by the selector itself, so the test only needs where it stands.
struct CageHome
{
	std::uint64_t building = 0;
	double x = 0.0;
	double z = 0.0;
	FactionLookup owner;
	decltype(state::SquadInstance::homeTown) town;
};

bool findCageHome(Simulation& sim, CageHome& out)
{
	bool found = false;
	ecsOf(sim).each([&](flecs::entity, const state::SquadInstance& instance)
	{
		if (found || instance.homeBuilding == 0 || instance.templateSid.empty())
			return;
		const Mapping& mapping = sim.ai()->registry().squad(instance.templateSid);
		if (mapping.packageName != "Cage Warden")
			return;
		const BuildingSite* site = sim.world().buildingSite(instance.homeBuilding);
		if (site == nullptr || std::hypot(site->cx - kHeftX, site->cz - kHeftZ) > 2000.0)
			return; // the Heft slave shops are the loaded ones; the world's other shops have no mesh here
		out.building = instance.homeBuilding;
		out.town = instance.homeTown;
		out.x = site->cx;
		out.z = site->cz;
		if (!site->ownerFaction.empty())
			out.owner = factionBySid(sim, site->ownerFaction);
		if (out.owner.id == 0 && instance.faction.id != 0)
		{
			const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
			if (faction != nullptr)
				out.owner = factionBySid(sim, faction->templateSid);
		}
		found = out.owner.id != 0;
	});
	return found;
}



std::string factionByName(Simulation& sim, const std::string& name)
{
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("FACTION")))
		if (record->name == name)
			return record->stringID;
	return {};
}

// The player's bounty sighted by law guards of a town (no order is given to anyone but the fixture's player placement): the wanted
// reaction fires EV_BOUNTY_SPOTTED, the law package's dialogue says its line, DA_ARREST_TARGET (29) knocks the criminal down and orders
// 240, the guard lifts it, carries it to a cage its legal faction may use and the sentence starts.
void autoArrest(Game& game)
{
	Simulation& sim = *game.sim;
	CageHome home;
	test::require(findCageHome(sim, home), "no cage home");
	const std::string legal = crime::legalFaction(sim.gameData().get(), home.owner.sid);
	test::require(legal == "defaultEmpireFactionSID", "the Heft cages answer to the United Cities (" + legal + ")");
	const std::string holy = factionByName(sim, "The Holy Nation");
	test::require(!holy.empty() && crime::legalFaction(sim.gameData().get(), holy) != legal, "the Holy Nation has its own legal system");

	// The UC police station's tower squad: law-class, United Cities, resident of the cage home's town.
	const flecs::entity squad = game.place("1120-gamedata.base", home.x + 60.0, home.z + 60.0);
	squad.get_mut<state::SquadInstance>()->homeTown = home.town;
	const std::vector<flecs::entity> members = membersOf(sim, squad);
	test::require(members.size() >= 2, "the guard squad was built");
	std::set<std::uint64_t> mine;
	for (const flecs::entity member : members)
	{
		const state::Fighter* f = sim.combat()->find(idOf(member));
		test::require(f != nullptr && f->npcClass == crime::kClassLaw, "the squad's members are law class");
		mine.insert(idOf(member));
		// The scene's spawn gives the tower squad the nearest building as its home: the cage home. A real tower squad lives in its police station, and a
		// guard at home in the shop is the original's PICKUP_INTRUDERS 223 resident for the body downed inside it (world_resolvers.hpp pickupIntruders223,
		// carry.cpp new take-outs: bump 3, a guard with no order carried the player out and the 240 order ended on the carried body, rwh.104 2.4).
		fighterOf(sim, idOf(member))->homeBuilding = 0;
	}
	const std::uint64_t leader = idOf(members[0]);
	test::require(sim.atHomeTown(leader), "the guards stand in their home town");

	const std::uint64_t player = game.player;
	state::Fighter* me = fighterOf(sim, player);
	test::require(me != nullptr, "the player's fighter");
	// The Slave Traders' own slavers (the cage owner's faction) would cuff the downed player before a guard lifts it (CAPTURE_NEW_SLAVES: the sim's arrest-vs-
	// capture priority is kenshi-k16.6.125, a race that entity-id order decided). This case pins the arrest chain alone, so they stay out of it: asleep.
	// So do the cage home's own residents (any faction): PICKUP_INTRUDERS 223 (worldres::pickupIntruders223, carry.cpp new take-outs) is the original's
	// finder for an incapacitated non-ally inside the resident's home building, and the downed player lies inside this one, so an awake resident
	// (bump 3: fighter 953, faction 18, no Routine) legitimately carries the body out before the guard lifts it (rwh.104: no cross-claim, first lift wins).
	int slavers = 0;
	for (const CombatArena::Member& m : sim.combat()->members())
		if (!m.fighter.playerControlled && mine.count(m.id) == 0 && (m.fighter.faction == home.owner.id || m.fighter.homeBuilding == home.building))
		{
			fighterOf(sim, m.id)->asleep = true;
			++slavers;
		}
	std::cout << "ksim_arrest_auto: " << slavers << " Slave Traders and cage-home residents put to sleep\n";
	const auto beside = [&]
	{
		const state::Fighter* lead = sim.combat()->find(leader);
		me->x = lead->x + 25.0;
		me->y = lead->y;
		me->z = lead->z;
		me->clearRoute();
	};
	// Any law guard may spot the bounty and take the order (the EV_BOUNTY_SPOTTED pick is generic), not only this squad: which guard faces the player is
	// a matter of idle yaws, and those depend on the arena strides.
	const auto isLaw = [&](std::uint64_t id)
	{
		const state::Fighter* f = sim.combat()->find(id);
		return f != nullptr && f->npcClass == crime::kClassLaw;
	};
	const auto spotted = [&](std::size_t from)
	{
		int picked = 0;
		const auto& log = sim.dialogue()->log();
		for (std::size_t i = from; i < log.size(); ++i)
			if (isLaw(log[i].event.speaker) && log[i].event.event == 44 && log[i].result == DialoguePick::Result::Picked)
				++picked;
		return picked;
	};

	// A bounty held by another legal system is nothing to these guards: no spotting, no arrest.
	sim.addBounty(player, holy, 5000);
	std::size_t logFrom = sim.dialogue()->log().size();
	beside();
	game.tick(600);
	test::require(spotted(logFrom) == 0, "a Holy Nation bounty made the United Cities guards spot the player");
	test::require(sim.arrestOrderCount() == 0 && !me->unconscious && me->cageBuilding == 0, "a Holy Nation bounty got the player arrested");
	test::require(sim.clearBounty(player, holy), "the foreign bounty is cleared");

	// The cage owner's bounty: the chain runs by itself.
	sim.addBounty(player, legal, 2000);
	logFrom = sim.dialogue()->log().size();
	beside();
	bool ordered = false;
	bool knockedOut = false;
	for (int second = 0; second < 150 && me->cageBuilding == 0; ++second)
	{
		game.tick(20);
		ordered = ordered || sim.arrestOrderCount() != 0;
		// The melee's own KO is the brief collapse timer (~3 s). A real KO from that damage lasts tens of seconds (medical model), so the scene stretches it.
		// And the guard who holds the arrest order lifts only a body within its reach (the order has no walk of its own: whoever downed the victim stands
		// beside it; a victim another guard downed 57 u away is never fetched, and what the original does then is kenshi-rwh.124 / kenshi-k16.6.187). So the
		// body lies down beside the order holder.
		if (me->unconscious && !knockedOut)
		{
			me->koTimer = std::max(me->koTimer, 90.0);
			for (const CombatArena::Member& m : sim.combat()->members())
				if (sim.arrestOrdered(m.id))
				{
					const state::Fighter* holder = sim.combat()->find(m.id);
					me->x = holder->x + 2.0;
					me->y = holder->y;
					me->z = holder->z;
					me->clearRoute();
					break;
				}
		}
		knockedOut = knockedOut || me->unconscious;
	}
	test::require(spotted(logFrom) >= 1, "EV_BOUNTY_SPOTTED did not pick a law dialogue");
	test::require(ordered, "DA_ARREST_TARGET did not order an arrest (the guard's home town gate)");
	test::require(knockedOut, "the guard did not knock the player down");
	test::require(me->cageBuilding != 0, "the player was not put in a cage");
	const state::CrimeRecord* record = sim.crimeRecord(player);
	test::require(record != nullptr && record->sentenceHours > 0.0, "the prisoner's sentence did not start");
	test::require(crime::bountyOf(*record, legal) >= 2000, "the bounty went missing");
	test::require(sim.arrestOrderCount() == 0, "the arrest order is still open after the put-in");
	std::cout << "ksim_arrest_auto: caged in cage " << me->cageBuilding << ", sentence " << record->sentenceHours << " h\n";
}

}

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_arrest_auto KSIM_NAVWORLD");
		for (int bump = 0; bump <= 5; ++bump)
		{
			Game game(argv[1], bump);
			autoArrest(game);
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_arrest_auto: FAILED: " << e.what() << "\n";
		return 1;
	}
}
