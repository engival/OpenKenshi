// Task BD (2026-11-07), corrected by BK (2026-11-08): what happens when a fight is over. A DOWN (conscious) enemy or a
// KO'd one is no longer a target: the crew drops it within 0.2 s and lands nothing on the body (kinspect 84a2ab0,
// findings/downed_target_combat.md), then finishes the rest of its squad and goes back to its own business (a world
// squad to its package goal, a town's people to their routines).
// A player-only fixture on the server's Hub game, like ksim_aggro_drop: the player provokes the Dust Bandit patrol,
// walks into the Bar and stays out of it while the crew fights. One bandit a crew member is fighting is put down (a
// leg at its KO point: down and conscious, the live collapse law) and watched; then the rest of the patrol is KO'd and
// the crew's targets and footing are checked after the fight. Needs the world bake (argv[1]; exit 77 without it).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
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

// The Bar blackmarket (ksim_bar_fight): footprint centre and its door pivot.
constexpr double kBarX = -50592.0;
constexpr double kBarZ = -1211.0;
constexpr double kDoorX = -50619.0;
constexpr double kDoorZ = -1156.0;

std::string factionName(Simulation& sim, const state::SquadInstance& instance)
{
	if (instance.faction.id == 0 || !ecsOf(sim).is_alive(instance.faction.id))
		return "";
	const state::FactionState* faction = ecsOf(sim).entity(instance.faction.id).get<state::FactionState>();
	const kdata::Record* record = faction != nullptr ? sim.gameData()->find(faction->templateSid) : nullptr;
	return record != nullptr ? record->name : "";
}

struct Person
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

std::vector<Person> membersOf(Simulation& sim, std::uint64_t squadId)
{
	std::vector<Person> found;
	flecs::entity squad;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& id, const state::SquadInstance&)
	{
		if (id.value == squadId)
			squad = e;
	});
	if (!squad.is_valid())
		return found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.push_back({id.value, member});
	});
	std::sort(found.begin(), found.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
	return found;
}

// The sum of the part healths: a hit lowers it, bleeding does not (it takes blood, not flesh).
double flesh(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& p : f.parts)
		sum += p.health;
	return sum;
}

struct Game
{
	std::string world;
	kworld::LoadOrder order;
	std::shared_ptr<const CombatData> combat;
	std::shared_ptr<MeshNavigator> mesh;
	std::unique_ptr<Simulation> sim;
	std::uint64_t player = 0;

	explicit Game(const std::string& bake)
		: world(bake)
		, order(kworld::resolveLoadOrder(KENSHI_DATA, ""))
	{
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}
		mesh = test::openWorldNav(world, hubZones);
	}

	void newGame()
	{
		SpawnOptions options;
		options.seed = 1;
		sim = std::make_unique<Simulation>(Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{}));
		sim->placeWildernessSquad("1302-gamedata.base", -48880.0, -823.0, 15);
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings s;
		s.substeps = 2;
		sim->enableCombat(combat, s);
		sim->enableNavigation(mesh);
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim->world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim->gameData());
		sim->enableAi(std::move(ai));
		std::string sid;
		for (const kdata::Record* record : sim->gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				sid = record->stringID;
		}
		sim->enqueue(Command::newGame(sid, "Tester", 1));
		sim->run(3);
		for (const CombatArena::Member& member : sim->combat()->members())
		{
			if (member.fighter.playerControlled)
				player = member.id;
		}
	}

	state::Fighter& me() { return *fighterOf(*sim, player); }

	// A tester who survives the patrol (a fresh character is KO'd in seconds).
	void makeTester()
	{
		state::FighterSkills& s = me().skills;
		s.toughness = 90.0;
		s.defence = 90.0;
		s.dodge = 90.0;
		s.athletics = 90.0;
	}

	bool walkTo(double x, double z)
	{
		const state::Fighter& f = me();
		Route route = sim->navigation()->routeFrom({f.x, f.y, f.z}, x, z);
		if (!route.found)
			return false;
		sim->enqueue(Command::fighterMoveTo(state::Id{player}, std::move(route.points), std::move(route.heights), 1, false, route.approach));
		return true;
	}

	void teleport(double x, double z)
	{
		state::Fighter& f = me();
		f.x = x;
		f.z = z;
		f.y = sim->groundHeight(x, z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_after_fight: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		Game game(argv[1]);
		game.newGame();
		Simulation& sim = *game.sim;
		test::require(game.player != 0, "the new game has a player character");
		sim.run(200);
		game.makeTester();
		sim.enqueue(Command::fighterSpeedMode(state::Id{game.player}, 2, 0)); // Run, as the client's default toggle sends
		sim.run(2);

		std::uint64_t banditSquad = 0;
		{
			double best = 1.0e18;
			ecsOf(sim).each([&](const state::Id& id, const state::SquadInstance& instance, const state::Position& at)
			{
				const double d = std::hypot(at.x - game.me().x, at.z - game.me().z);
				if (factionName(sim, instance) == "Dust Bandits" && instance.hasDen && d < best)
				{
					best = d;
					banditSquad = id.value;
				}
			});
		}
		test::require(banditSquad != 0, "a Dust Bandit squad with a den exists");
		std::vector<std::uint64_t> crew;
		for (const CombatArena::Member& m : sim.combat()->members())
		{
			if (m.fighter.team == 0 && !m.fighter.playerControlled && !m.fighter.dead && m.fighter.homeBuilding != 0
				&& std::hypot(m.fighter.x - kBarX, m.fighter.z - kBarZ) < 120.0)
				crew.push_back(m.id);
		}
		test::require(crew.size() >= 3, "the Bar's crew (barman and guards) stands at the Bar");
		const std::vector<Person> bandits = membersOf(sim, banditSquad);
		test::require(bandits.size() >= 10, "the patrol has its members");

		// The provocation outside the door, as ksim_aggro_drop: the patrol rows up in front of the Bar and sees the player.
		game.teleport(kDoorX, kDoorZ + 70.0);
		int k = 0;
		for (const Person& p : bandits)
		{
			state::Fighter* f = fighterOf(sim, p.id);
			if (f == nullptr)
				continue;
			f->x = kDoorX - 12.0 + 6.0 * (k % 5);
			f->z = kDoorZ + 220.0 + 6.0 * (k / 5);
			f->y = sim.groundHeight(f->x, f->z).value_or(f->y);
			f->clearRoute();
			f->building = 0;
			++k;
		}
		// The provocation (task BV, 2026-11-07): the Dust Bandits' own relation to a new player is -10 (neutral, FUN_14079c950 /
		// kinspect dt11 + dt21), so standing in their sight no longer starts a fight. The player attacks the nearest bandit (the
		// original's personal type-4 flip, as ksim_aggro_drop) and the patrol answers.
		{
			std::uint64_t mark = 0;
			double nearest = 1.0e18;
			for (const Person& p : bandits)
			{
				const state::Fighter* f = sim.combat()->find(p.id);
				if (f == nullptr || f->dead)
					continue;
				const double d = std::hypot(f->x - game.me().x, f->z - game.me().z);
				if (d < nearest) { nearest = d; mark = p.id; }
			}
			test::require(mark != 0, "the patrol has a bandit to provoke");
			sim.enqueue(Command::fighterAttack(state::Id{game.player}, state::Id{mark}));
		}
		std::size_t provoked = 0;
		for (int i = 0; i < 60 && provoked < 10; ++i)
		{
			sim.run(5);
			provoked = 0;
			for (const Person& p : bandits)
			{
				const state::Fighter* f = sim.combat()->find(p.id);
				provoked += f != nullptr && !f->dead && f->target == game.player ? 1 : 0;
			}
		}
		test::require(provoked >= 10, "the patrol targets the player outside the Bar (>= 10 of 15)");

		// The player walks in through the door and stands out of it; the crew joins by its own rules.
		if (!game.walkTo(kDoorX, kDoorZ - 45.0))
			game.teleport(kDoorX, kDoorZ - 45.0);
		std::size_t crewTargeting = 0;
		for (int i = 0; i < 20 * 120 && crewTargeting == 0; ++i)
		{
			sim.run(1);
			crewTargeting = 0;
			for (const std::uint64_t cid : crew)
			{
				const state::Fighter* c = sim.combat()->find(cid);
				if (c == nullptr || c->dead || c->target == 0)
					continue;
				for (const Person& p : bandits)
					crewTargeting += p.id == c->target ? 1 : 0;
			}
		}
		test::require(crewTargeting > 0, "the Bar's crew joins the fight (a crew member targets a bandit)");

		// The bandit a crew member is fighting: it is put down (a leg at its KO point: down, still conscious).
		std::uint64_t victim = 0;
		for (const Person& p : bandits)
		{
			const state::Fighter* f = sim.combat()->find(p.id);
			if (f == nullptr || f->dead || f->unconscious)
				continue;
			for (const std::uint64_t cid : crew)
			{
				const state::Fighter* c = sim.combat()->find(cid);
				if (c != nullptr && !c->dead && c->target == p.id)
				{
					victim = p.id;
					break;
				}
			}
			if (victim != 0)
				break;
		}
		test::require(victim != 0, "a crew member targets a bandit to put down");
		state::Fighter& v = *fighterOf(sim, victim);
		const double koPoint = sim.combat()->rules().koPoint(v);
		state::FighterPart* leg = nullptr;
		for (state::FighterPart& p : v.parts)
		{
			if (p.collapses && (p.collapseBits & (1 | 16 | 32)) && !p.vital && !p.robotic)
			{
				leg = &p;
				break;
			}
		}
		test::require(leg != nullptr, "the bandit has a collapsing leg (a non-vital down part)");
		leg->health = koPoint - 1.0;
		sim.run(2);
		// The collapse also fires the forced KO writer (task BL, kinspect down_ko_trigger.md / fb0248d): the bandit is down AND
		// unconscious for 3.0 medical timer units, then wakes with down still set - the conscious crawler this block measures.
		test::require(v.down && v.unconscious, "a leg at the KO point puts the bandit down and KO's him for the collapse's 3.0 units");
		// 3.0 medical units at the medical clock: 30 ticks at the watched-duel 2.0, ~63 at the synced ~0.96 (task BT); the body
		// wakes and down stays.
		const double medicalClock = sim.combat()->rules().settings().medicalClock;
		for (int i = 0, limit = static_cast<int>(std::ceil(3.0 / (Simulation::kTickSeconds * medicalClock))) + 2; i < limit && v.unconscious; ++i)
			sim.run(1);
		test::require(v.down && !v.unconscious, "the collapse's KO ends with the bandit down, still conscious");
		std::cout << "victim " << victim << " down at t=" << sim.clockTick() / 20.0 << " (ko point " << koPoint << ")\n";

		// Down invalidates the target (task BK; kinspect 84a2ab0, findings/downed_target_combat.md): a DOWN body is dropped in
		// 1-10 frames and an in-flight swing is aborted with no impact, so the crew leaves it where it crawled. It does not keep
		// hitting it until the KO, as the sim did before (task BD's audit said the opposite; retired). A swing aimed at another
		// body can still splash the prone one in its arc (the crowd hit list): that is not the crew hitting it, so the flesh
		// taken while no crew member targets it is reported apart and not counted against the rule.
		std::size_t heldWhileDown = 0; // ticks a crew member still had it as its target
		double dealtWhileDown = 0.0; // flesh taken while the crew still had it as a target
		double splashWhileDown = 0.0; // flesh taken with no crew targeting it: another swing's arc (the crowd hit list)
		double flesh0 = flesh(v);
		std::uint64_t downTick = sim.clockTick();
		std::uint64_t dropTick = 0;
		for (int i = 0; i < 20 * 10; ++i)
		{
			sim.run(1);
			std::size_t held = 0;
			for (const std::uint64_t cid : crew)
			{
				const state::Fighter* c = sim.combat()->find(cid);
				held += (c != nullptr && !c->dead && c->target == victim) ? 1 : 0;
			}
			heldWhileDown += held;
			if (held == 0 && dropTick == 0)
				dropTick = sim.clockTick();
			const double now = flesh(v);
			if (now < flesh0 - 0.5)
				(held != 0 ? dealtWhileDown : splashWhileDown) += flesh0 - now;
			flesh0 = now;
		}
		const double dropSeconds = dropTick != 0 ? static_cast<double>(dropTick - downTick) / 20.0 : -1.0;
		std::cout << "victim " << victim << ": target dropped after "
			<< (dropTick != 0 ? std::to_string(dropSeconds) + " s" : std::string("never")) << " (" << heldWhileDown
			<< " crew-ticks held), flesh dealt on the down body " << dealtWhileDown << ", splashed by swings at others "
			<< splashWhileDown << "\n";

		// After the fight: the rest of the patrol is KO'd outright, and the crew drops its targets and moves again. The KO timers
		// are re-set every tick: a lightly hurt body's real timer (BA's damage sum) can run out inside the window and stand it
		// back up, which would turn this into a second fight.
		for (const Person& p : bandits)
		{
			state::Fighter* f = fighterOf(sim, p.id);
			if (f != nullptr && !f->dead)
			{
				f->unconscious = true;
				f->koTimer = 600.0;
			}
		}
		std::vector<std::pair<double, double>> crewAt;
		for (const std::uint64_t cid : crew)
		{
			const state::Fighter* c = sim.combat()->find(cid);
			crewAt.push_back(c != nullptr ? std::pair<double, double>{c->x, c->z} : std::pair<double, double>{0.0, 0.0});
		}
		std::uint64_t windowStart = sim.clockTick();
		std::uint64_t clearTick = 0;
		std::size_t crewTargetingAfter = 0;
		double crewMoved = 0.0;
		bool crewWalking = false;
		// A KO'd body keeps losing flesh to its wounds' bleeding, whichever bandit it is (the bleed rate follows the wounds, so it varies with the
		// squad world creation hands the test): the rule is about the crew's blows, so the Hit events on it are counted, and the bleed reported apart.
		double dealtAfterKO = 0.0;
		double bledAfterKO = 0.0;
		const_cast<CombatArena*>(sim.combat())->setObserver([&](const CombatEvent& e)
		{
			if (e.kind == CombatEvent::Kind::Hit && e.who == victim)
				dealtAfterKO += 1.0;
		});
		flesh0 = flesh(v);
		for (int i = 0; i < 20 * 40; ++i)
		{
			sim.run(1);
			for (const Person& p : bandits)
			{
				state::Fighter* b = fighterOf(sim, p.id);
				if (b != nullptr && !b->dead && (!b->unconscious || b->koTimer < 60.0))
				{
					b->unconscious = true;
					b->koTimer = 600.0;
				}
			}
			const double fleshNow = flesh(v);
			if (fleshNow < flesh0 - 0.5)
				bledAfterKO += flesh0 - fleshNow; // every flesh loss, hit or not; the rule below counts the hits
			flesh0 = fleshNow;
			crewTargetingAfter = 0;
			double walked = 0.0;
			for (std::size_t j = 0; j < crew.size(); ++j)
			{
				const state::Fighter* c = sim.combat()->find(crew[j]);
				if (c == nullptr || c->dead)
					continue;
				const bool onBandit = std::any_of(bandits.begin(), bandits.end(), [&](const Person& p) { return p.id == c->target; });
				crewTargetingAfter += onBandit ? 1 : 0;
				walked = std::max(walked, std::hypot(c->x - crewAt[j].first, c->z - crewAt[j].second));
				crewWalking = crewWalking || c->walking();
			}
			crewMoved = std::max(crewMoved, walked);
			if (clearTick == 0 && crewTargetingAfter == 0)
				clearTick = sim.clockTick();
		}
		const double clearSeconds = clearTick != 0 ? static_cast<double>(clearTick - windowStart) / 20.0 : -1.0;
		std::cout << "after the KO: hits on the KO'd body " << dealtAfterKO << " (bled " << bledAfterKO << ")\n";
		std::cout << "after the fight: crew targets on the bandits " << crewTargetingAfter << ", all dropped after "
			<< (clearTick != 0 ? std::to_string(clearSeconds) + " s" : std::string("never"))
			<< ", moved " << crewMoved << " u, walking " << (crewWalking ? "yes" : "no") << ", player target " << game.me().target << "\n";

		std::vector<std::string> failures;
		const auto check = [&](bool ok, const std::string& what) { if (!ok) failures.push_back(what); };
		check(dropTick != 0 && dropSeconds <= 0.2 && dealtWhileDown < 0.5,
			"a DOWN enemy is dropped within 0.2 s and not hit (" + (dropTick != 0 ? std::to_string(dropSeconds) + " s" : std::string("never"))
				+ ", " + std::to_string(dealtWhileDown) + " flesh while down)");
		check(dealtAfterKO < 0.5, "a KO'd enemy is left alone (no hits)");
		check(crewTargetingAfter == 0, "after the fight no crew member holds a bandit target");
		check(crewTargetingAfter == 0 && (crewMoved > 5.0 || crewWalking),
			"the crew goes back to its own business (moves again or walks)");
		if (failures.empty())
		{
			std::cout << "ksim_after_fight: ok\n";
			return 0;
		}
		for (const std::string& f : failures)
			std::cerr << "error: " << f << "\n";
		return 1;
	}
	catch (const std::exception& error)
	{
		std::cerr << "error: " << error.what() << "\n";
		return 1;
	}
}
