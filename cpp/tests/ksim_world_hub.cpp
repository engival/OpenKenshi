// The server's new game at the Hub (seed 1, wilderness by density, streaming navmesh, squad AI as the server runs it with --bake), watched for game
// hours (TOWN_LIFE_SLICE 5.14 "Hub debug"). The Dust Bandit squad nearest the Hub (the den at (-48880, -823), 15 members) is a built world squad that
// patrols: it leaves the den (a player who arrives late finds the den empty), keeps within the patrol's reach, is never demoted while no player is near,
// and its token follows its leader.
// Needs the world bake (argv[1], like ksim_nav_regions; exit 77 without it). Probe use: argv[2] = game hours (default 4), argv[3] = "noai" for the server
// without --bake (nobody moves), argv[4] argv[5] = a point the player walks to; set KSIM_WORLD_HUB_VERBOSE=1 to print a line per squad every 15 game minutes.
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/detail.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "nav_source.hpp"
#include "sim_access.hpp"
#include "ksim/dev/build_paths.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

std::string factionName(Simulation& sim, const state::SquadInstance& instance)
{
	if (instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id))
		return "";
	const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
	const kdata::Record* record = faction != nullptr ? sim.gameData()->find(faction->templateSid) : nullptr;
	return record != nullptr ? record->name : "";
}

const char* goalName(state::GoalKind goal)
{
	static const char* names[] = {"None", "Idle", "GoHome", "TravelTo", "Patrol", "Roam", "Raid", "ManGate"};
	return names[static_cast<int>(goal)];
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_world_hub: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const std::string world = argv[1];
		// 4 hours: the squad's first patrol point (north, on the hill) is past its leash by the walkable way round on our recipe bake and on
		// the shipped tiles alike, so it starts on the next point an hour later (v8's bake had a shortcut the original has not).
		const double hours = argc > 2 ? std::stod(argv[2]) : 4.0;
		std::ostream nowhere(nullptr);
		std::ostream& out = std::getenv("KSIM_WORLD_HUB_VERBOSE") != nullptr ? std::cout : nowhere;
		const bool withAi = !(argc > 3 && std::string(argv[3]) == "noai");
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		// The 15-member patrol the test watches, placed on purpose (BAR_FIGHT_LOOP "Why Dust Bandits patrol").
		const std::uint64_t placed = sim.placeWildernessSquad("1302-gamedata.base", -48880.0, -823.0, 15).get<state::Id>()->value;
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
		sim.enableNavigation(test::openWorldNav(world, hubZones));
		if (withAi)
		{
			AiSystem::Config ai;
			ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
			ai.factions = FactionBook::build(sim.gameData());
			sim.enableAi(std::move(ai));
		}
		std::vector<std::string> logLines; // the server's --log-world-squads 15 sink
		// Demotes: the log line is written inside the step that demotes, so the squad's Detail is the one the sim judged it by. A world squad is
		// demoted when it is no longer eligible, and eligibility needs Detail Full (residents.cpp stepResidents), the region engine's own bound.
		std::vector<std::string> badDemotes; // demoted while still at Full detail
		std::size_t placedDemotes = 0;
		bool placedLeftFull = false; // the placed squad was seen outside Full detail
		sim.setWorldSquadLog(15, [&](const std::string& line)
		{
			logLines.push_back(line);
			const std::string prefix = "world squad demote ";
			if (line.rfind(prefix, 0) != 0)
				return;
			const std::uint64_t id = std::stoull(line.substr(prefix.size()));
			ecsOf(sim).each([&](flecs::entity squad, const state::Id& squadId, const state::SquadInstance&, const Detail& detail)
			{
				if (squadId.value != id)
					return;
				placedDemotes += id == placed ? 1 : 0;
				// A squad at Full detail is also demoted at the edge of the mesh: it starts an off-mesh token leg (residents.cpp `leaving`, k16.7.15).
				const state::GroupAI* groupAi = squad.get<state::GroupAI>();
				const bool leaving = groupAi != nullptr && UtilityGroupLayer::tokenLeg(*groupAi);
				if (detail.level == DetailLevel::Full && !leaving)
					badDemotes.push_back(line);
			});
		});
		std::string sid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim.enqueue(Command::newGame(sid, "Tester", 1));
		// The den is Full only by the loaded-zone rule (rwh.202): the camera looks at it, its 3x3 zones are loaded (the Wanderer's +-340 u box is not).
		sim.enqueue(Command::setView(-48880.0, -823.0));
		sim.run(3);
		out << "start tick " << sim.clockTick() << ", ai " << withAi << "\n";

		const std::uint64_t ticks = static_cast<std::uint64_t>(hours * static_cast<double>(kTicksPerGameHour));
		const std::uint64_t every = static_cast<std::uint64_t>(kTicksPerGameMinute * 15.0);
		std::uint64_t player = 0;
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			if (member.fighter.playerControlled)
				player = member.id;
		}
		if (argc > 4)
		{
			const double wx = std::stod(argv[4]);
			const double wz = std::stod(argv[5]);
			const state::Fighter& f = *sim.combat()->find(player);
			Route route = sim.navigation()->routeFrom({f.x, f.y, f.z}, wx, wz);
			out << "walk route found " << route.found << " points " << route.points.size() << " approach " << route.approach << "\n";
			if (route.found)
				sim.enqueue(Command::fighterMoveTo(state::Id{player}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
		}
		std::uint64_t watched = 0; // the Dust Bandit squad nearest the player's start, chosen at 30 game minutes
		double denDistance = 0.0; // the furthest its token has been from its den
		double leaderGap = 0.0; // the furthest its token has been from its leader
		std::size_t samples = 0;
		// The patrol pace window (400 ticks of a walking leader, 20 s at kTickSeconds).
		struct PaceWindow
		{
			bool active = false;
			std::uint64_t start = 0;
			std::array<double, 2> prev{};
			std::vector<double> speeds;
		};
		PaceWindow pace;
		std::uint64_t paceLeader = 0;
		bool paceDone = false;
		for (std::uint64_t t = 0; t <= ticks; ++t)
		{
			if (t % every == 0)
			{
				const Simulation::ResidentStats& stats = sim.residentStats();
				out << "t=" << t / kTicksPerGameHour << "h" << (t % kTicksPerGameHour) * 60 / kTicksPerGameHour << "m worldPromoted " << stats.worldPromotedSquads
					<< " worldDemoted " << stats.worldDemotedSquads << " player (" << sim.combat()->find(player)->x << ", " << sim.combat()->find(player)->z << ")\n";
				if (t == 2 * every || t == 2 * kTicksPerGameHour / 15 * 15)
				{
					ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
					{
						flecs::entity playerFaction;
						ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { playerFaction = e; });
						const double toPlayer = std::hypot(at.x - sim.combat()->find(player)->x, at.z - sim.combat()->find(player)->z);
						if (toPlayer < 12000.0 && instance.faction.id != 0 && ecsOf(sim).is_alive(instance.faction.id) && playerFaction.is_valid())
						{
							const flecs::entity own = ecsOf(sim).entity(instance.faction.id);
							if (factionRelation(*sim.gameData(), own, playerFaction) <= -30.0 || factionRelation(*sim.gameData(), playerFaction, own) <= -30.0)
							{
								const kdata::Record* rec = sim.gameData()->find(instance.templateSid);
								const state::GroupAI* g = squad.get<state::GroupAI>();
								out << "  TOKEN-HOSTILE " << id.value << " " << (rec != nullptr ? rec->name : instance.templateSid) << " / " << factionName(sim, instance) << " at (" << static_cast<long>(at.x)
									<< ", " << static_cast<long>(at.z) << ") " << static_cast<long>(toPlayer) << " u, goal " << (g != nullptr ? goalName(g->goal) : "none") << "\n";
							}
						}
						if (toPlayer < 9000.0 && instance.faction.id != 0 && ecsOf(sim).is_alive(instance.faction.id) && playerFaction.is_valid())
						{
							const flecs::entity own = ecsOf(sim).entity(instance.faction.id);
							const kdata::Record* rec = sim.gameData()->find(instance.templateSid);
							out << "  NEAR " << id.value << " " << (rec != nullptr ? rec->name : instance.templateSid) << " / " << factionName(sim, instance) << " " << static_cast<long>(toPlayer)
								<< " u, relation to the player " << factionRelation(*sim.gameData(), own, playerFaction) << ", from the player " << factionRelation(*sim.gameData(), playerFaction, own) << "\n";
						}
						std::size_t hostile = 0;
						std::size_t total = 0;
						bool any = false;
						ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& mid)
						{
							const state::Fighter* f = sim.combat()->find(mid.value);
							++total;
							if (f != nullptr && !f->dead && !member.has<state::Routine>())
							{
								any = true;
								hostile += f->team != 0 ? 1 : 0;
							}
						});
						if (!any || hostile == 0)
							return;
						const state::GroupAI* group = squad.get<state::GroupAI>();
						const kdata::Record* rec = sim.gameData()->find(instance.templateSid);
						out << "  HOSTILE " << id.value << " " << (rec != nullptr ? rec->name : instance.templateSid) << " / " << factionName(sim, instance) << " members " << hostile << "/" << total
							<< " at (" << static_cast<long>(at.x) << ", " << static_cast<long>(at.z) << ") " << static_cast<long>(std::hypot(at.x - sim.combat()->find(player)->x, at.z - sim.combat()->find(player)->z)) << " u from the player, goal "
							<< (group != nullptr ? goalName(group->goal) : "none") << "\n";
					});
				}
				ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
				{
					if (factionName(sim, instance).find("Dust Bandit") == std::string::npos)
						return;
					const state::GroupAI* group = squad.get<state::GroupAI>();
					std::size_t built = 0;
					double lx = 0.0;
					double lz = 0.0;
					std::uint64_t leaderId = 0;
					bool walking = false;
					std::uint64_t target = 0;
					ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity, const state::Id& member)
					{
						const state::Fighter* f = sim.combat()->find(member.value);
						if (f != nullptr && !f->dead)
						{
							if (built++ == 0 || member.value < leaderId)
							{
								leaderId = member.value;
								lx = f->x;
								lz = f->z;
								walking = f->walking();
								target = f->target;
							}
						}
					});
					const double den = std::hypot(at.x - instance.denX, at.z - instance.denZ);
					out << "  squad " << id.value << " " << instance.templateSid << " token (" << static_cast<long>(at.x) << ", " << static_cast<long>(at.z)
						<< ") den+" << static_cast<long>(den) << " built " << built << " leader (" << static_cast<long>(lx) << ", " << static_cast<long>(lz) << ")" << (walking ? " walking" : " still") << (target != 0 ? " fighting" : "")
						<< " player " << static_cast<long>(std::hypot(lx - sim.combat()->find(player)->x, lz - sim.combat()->find(player)->z)) << " away";
					if (group != nullptr)
						out << " goal " << goalName(group->goal) << " goalXZ (" << static_cast<long>(group->goalX) << ", " << static_cast<long>(group->goalZ) << ") along "
							<< static_cast<long>(group->along) << "/" << static_cast<long>(group->length) << " idle " << (sim.clockTick() - group->progressTick) / kTicksPerGameHour << "h";
					out << "\n";
				});
				if (t == 2 * every)
				{
					double best = 1.0e18;
					ecsOf(sim).each([&](const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
					{
						const double d = std::hypot(at.x - sim.combat()->find(player)->x, at.z - sim.combat()->find(player)->z);
						if (factionName(sim, instance) == "Dust Bandits" && d < best && instance.hasDen)
						{
							best = d;
							watched = id.value;
						}
					});
				}
				if (watched != 0 && t >= 2 * every)
				{
					ecsOf(sim).each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
					{
						if (id.value != watched)
							return;
						std::uint64_t leader = 0;
						ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& mid)
						{
							const state::Fighter* f = sim.combat()->find(mid.value);
							if (f != nullptr && !f->dead && !f->unconscious && !f->playerControlled && !member.has<state::Routine>() && (leader == 0 || mid.value < leader))
								leader = mid.value;
						});
						if (leader == 0)
							return;
						denDistance = std::max(denDistance, std::hypot(at.x - instance.denX, at.z - instance.denZ));
						leaderGap = std::max(leaderGap, std::hypot(at.x - sim.combat()->find(leader)->x, at.z - sim.combat()->find(leader)->z));
						++samples;
					});
				}
			}
			ecsOf(sim).each([&](const state::Id& id, const state::SquadInstance&, const Detail& detail)
			{
				placedLeftFull = placedLeftFull || (id.value == placed && detail.level != DetailLevel::Full);
			});
			// A walking patrol leader is sampled every tick for 20 s (400 ticks): it never stands still for more than 2 ticks while
			// the patrol is under way, and its mean speed is the group's pace (UtilityGroupLayer::kWalkSpeed), not the gait's top speed.
			if (!paceDone)
			{
				flecs::entity squadEntity;
				ecsOf(sim).each([&](flecs::entity squad, const state::Id& sid, const state::SquadInstance&) { squadEntity = sid.value == watched ? squad : squadEntity; });
				const state::GroupAI* group = squadEntity.is_valid() ? squadEntity.get<state::GroupAI>() : nullptr;
				std::uint64_t leader = 0;
				if (squadEntity.is_valid())
					ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squadEntity).build().each([&](flecs::entity member, const state::Id& mid)
					{
						const state::Fighter* f = sim.combat()->find(mid.value);
						if (f != nullptr && !f->dead && !f->unconscious && !f->playerControlled && !member.has<state::Routine>() && (leader == 0 || mid.value < leader))
							leader = mid.value;
					});
				const state::Fighter* lf = leader != 0 ? sim.combat()->find(leader) : nullptr;
				const bool patrolling = group != nullptr && (group->goal == state::GoalKind::Patrol || group->goal == state::GoalKind::Roam)
					&& group->along < group->length - 60.0;
				if (!pace.active)
				{
					if (patrolling && lf != nullptr && lf->walking())
					{
						pace.active = true;
						pace.start = t;
						pace.prev = {lf->x, lf->z};
						pace.speeds.clear();
						paceLeader = leader;
					}
				} else if (!patrolling || lf == nullptr || t - pace.start > 420) {
					pace.active = false; // a real stop (the leg ended, the goal changed): wait for the next walk
				} else {
					pace.speeds.push_back(std::hypot(lf->x - pace.prev[0], lf->z - pace.prev[1]) / kTickSeconds);
					pace.prev = {lf->x, lf->z};
					if (pace.speeds.size() >= 400)
					{
						double sum = 0.0;
						std::size_t longest = 0;
						std::size_t run = 0;
						for (const double speed : pace.speeds)
						{
							sum += speed;
							run = speed <= 0.01 ? run + 1 : 0;
							longest = std::max(longest, run);
						}
						const double mean = sum / static_cast<double>(pace.speeds.size());
						out << "patrol pace: leader " << paceLeader << ", " << pace.speeds.size() << " ticks from t " << pace.start << ", mean " << mean
							<< " u/s, longest stop " << longest << " ticks\n";
						test::require(longest <= 2, "a walking patrol leader never stands still for more than 2 ticks");
						test::require(mean >= 0.85 * UtilityGroupLayer::kWalkSpeed && mean <= 1.15 * UtilityGroupLayer::kWalkSpeed, "a walking patrol leader keeps the group pace");
						paceDone = true;
					}
				}
			}
			sim.run(1);
		}
		test::require(watched != 0, "the new game has a Dust Bandit squad with a den");
		test::require(paceDone, "the patrol walked for 20 s without a goal or leg change (the pace window)");
		out << "watched squad " << watched << ": " << samples << " samples, furthest from its den " << denDistance << " u, token to leader at most " << leaderGap << " u, world squads demoted "
			<< sim.residentStats().worldDemotedSquads << "\n";
		std::cout << "ksim_world_hub: squad " << watched << " " << (withAi ? "patrols " : "stays ") << static_cast<long>(denDistance) << " u from its den\n";
		std::size_t promotes = 0;
		std::size_t built = 0;
		std::size_t banditLines = 0;
		for (const std::string& line : logLines)
		{
			out << "log: " << line << "\n";
			promotes += line.rfind("world squad promote ", 0) == 0;
			built += line.rfind("world squad built ", 0) == 0;
			// Task BV (2026-11-07): hostility is directional (FUN_14079c950, kinspect dt11); the Dust Bandits' own relation to a new player is
			// -10, so the patrol is logged "team 0 hostile 0" (residents.cpp worldTeam), not "hostile 1".
			banditLines += line.rfind("world squad built " + std::to_string(watched) + " ", 0) == 0 && line.find("hostile 0") != std::string::npos && line.find("members 15/15") != std::string::npos;
		}
		test::require(promotes >= 1 && built >= 1 && banditLines >= 1, "--log-world-squads logs the promotes and a periodic line per built squad (the neutral 15-member bandits among them)");
		out << "uncorrectable (off the mesh, none in reach) " << sim.combat()->uncorrectable() << ", corrected " << sim.combat()->positionCorrections() << "\n";
		test::require(samples >= 8, "the watched squad stays built");
		// Was: "no world squad is demoted" at all. That encoded the old world's accident: the Hub's 3 x 3 zones held one hostile squad. Since the
		// wilderness weight rule (ARCHITECTURE "Wilderness spawns") the draw puts other squads around, and one that patrols out of Full detail is
		// rightly demoted. Now: (a) the placed Hub squad is not demoted while it stays at Full detail, and (b) every demote is of a squad that is
		// no longer at Full detail (the bound is the region engine's Detail, not a guessed distance), which catches the old bug class too.
		test::require(placedDemotes == 0 || placedLeftFull, "the placed Hub squad is never demoted while it stays at Full detail");
		test::require(badDemotes.empty(), "every world squad demote is of a squad outside Full detail: " + (badDemotes.empty() ? std::string() : badDemotes.front()));
		if (withAi)
		{
			test::require(denDistance > 500.0, "the patrol takes the squad away from its den (a player who comes later finds it empty)");
			test::require(denDistance < 2500.0, "the patrol stays within its reach of the den");
			test::require(leaderGap < 150.0, "the squad's token is where its leader is");
		} else {
			test::require(denDistance < 50.0, "without the squad AI a world squad stays at its den");
		}
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_world_hub: FAILED: " << e.what() << "\n";
		return 1;
	}
}
