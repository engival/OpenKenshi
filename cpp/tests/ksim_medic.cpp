// Task CG (2026-11-07): 58 JOB_MEDIC as a package goal, not a special case. A built world squad whose package carries it (the
// slaver caravan's does: kinspect dt24_58.md, dt16 section 6) has a member take the goal on an injured squadmate, within its 40 u
// distance cap (healDistance), and the first aid interact (25 FIRST_AID_ORDER -> Task_FirstAid) treats it. No NPC is ordered; the
// player is not involved.
//   A - an injured squadmate 5 u from the squad: a member without injury of its own takes goal 58 on it and the injury falls.
//   B - the same patient 80 u off, down: no member takes 58 (the 40 u cap); carried back within reach, the goal appears again.
// Needs the world bake (argv[1], like ksim_slaver_caravan; exit 77 without it).
#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/command.hpp"
#include "ksim/factions.hpp"
#include "ksim/routines.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "nav_source.hpp"
#include "sim_access.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

// The wilderness spawn the other Hub tests use (ksim_after_fight, the client's --place-squad point).
constexpr double kSpawnX = -48880.0;
constexpr double kSpawnZ = -823.0;
constexpr std::uint16_t kMedicGoal = routines::task::FirstAid; // 58
constexpr double kNearOffset = 5.0; // inside 58's 40 u cap
constexpr double kFarOffset = 80.0; // beyond it

flecs::entity entityOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::Id& i, const state::Fighter&) { found = i.value == id ? e : found; });
	return found;
}

state::Inventory& packOf(Simulation& sim, std::uint64_t id)
{
	flecs::entity entity = entityOf(sim, id);
	if (entity.get<state::Inventory>() == nullptr)
		entity.set<state::Inventory>(state::Inventory{});
	return *entity.get_mut<state::Inventory>();
}

struct Person
{
	std::uint64_t id = 0;
	flecs::entity entity;
};

std::vector<Person> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<Person> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.push_back({id.value, member});
	});
	std::sort(found.begin(), found.end(), [](const Person& a, const Person& b) { return a.id < b.id; });
	return found;
}

flecs::entity playerFaction(Simulation& sim)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity e, const state::PlayerFaction&, const state::FactionState&) { found = e; });
	return found;
}

const kdata::Record* recordNamed(Simulation& sim, const char* type, const char* name)
{
	for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName(type)))
	{
		if (record->name == name)
			return record;
	}
	return nullptr;
}

// The injury 58's resolver sums (world_resolvers.hpp injuryOf): the part damage the bandage pools have not covered, both categories.
double injuryOf(const state::Fighter& body)
{
	double sum = 0.0;
	for (const state::FighterPart& part : body.parts)
		sum += std::max(0.0, (part.maxHealth - part.wear) - part.health - part.pool);
	return sum;
}

// Puts `amount` of flesh injury on the body: the first organic part loses health, the wear left as it is.
void injure(state::Fighter& body, double amount)
{
	for (state::FighterPart& part : body.parts)
	{
		if (!part.robotic && part.maxHealth - part.wear > 0.0)
		{
			part.health = std::max(0.0, part.maxHealth - part.wear - amount);
			return;
		}
	}
}

// A member on goal 58 against `patient`, and which one.
std::uint64_t medicOn(Simulation&, const std::vector<Person>& members, std::uint64_t patient)
{
	for (const Person& p : members)
	{
		if (p.id == patient)
			continue;
		const state::Routine* r = p.entity.get<state::Routine>();
		if (r != nullptr && r->goal == kMedicGoal && r->chainTarget == patient)
			return p.id;
	}
	return 0;
}

// Turns every other member to face the patient: perception's sight is the cone, and a member facing away does not record it.
void face(Simulation& sim, const std::vector<Person>& members, std::uint64_t patient)
{
	const state::Fighter* body = fighterOf(sim, patient);
	if (body == nullptr)
		return;
	for (const Person& p : members)
	{
		if (p.id == patient)
			continue;
		state::Fighter* f = fighterOf(sim, p.id);
		f->yaw = std::atan2(body->x - f->x, body->z - f->z);
	}
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
		sim->markPristine();
		combat = CombatData::build(*sim->gameData(), KENSHI_DATA);
		CombatSettings s;
		s.substeps = 2;
		sim->enableCombat(combat, s);
		sim->enableNavigation(mesh);
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

	void place(double x, double z) // the player far enough off not to be part of anything
	{
		state::Fighter& f = *fighterOf(*sim, player);
		f.x = x;
		f.z = z;
		f.y = sim->groundHeight(x, z).value_or(f.y);
		f.clearRoute();
		f.building = 0;
	}
};

// Places the slaver caravan (its packages carry 58), hostile to the player so its own combat never mixes in, and waits until its
// members are Fighters.
struct Placed
{
	flecs::entity squad;
	std::vector<Person> members;
};

Placed placeCaravan(Game& game, std::size_t expected)
{
	Simulation& sim = *game.sim;
	const flecs::entity squad = sim.placeWildernessSquad("3387-gamedata.base", kSpawnX, kSpawnZ);
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	test::require(instance != nullptr && instance->faction.id != 0, "the placed squad has a faction");
	sim.enqueue(Command::setFactionRelation(*ecsOf(sim).entity(instance->faction.id).get<state::Id>(),
		*playerFaction(sim).get<state::Id>(), -60));
	Placed placed;
	placed.squad = squad;
	for (int i = 0; i < 20 * 20; ++i)
	{
		sim.run(1);
		placed.members = membersOf(sim, squad);
		const bool built = std::all_of(placed.members.begin(), placed.members.end(), [&](const Person& p) { return sim.combat()->find(p.id) != nullptr; });
		if (built && placed.members.size() >= expected)
			break;
	}
	test::require(placed.members.size() >= expected, "the squad was built with its members");
	for (const Person& p : placed.members)
		test::require(sim.combat()->find(p.id) != nullptr, "the world squad's members are Fighters (it was promoted)");
	return placed;
}

} // namespace

int main(int argc, char** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	try
	{
		const std::string bake = argc > 1 ? argv[1] : "";
		if (bake.empty())
		{
			std::cout << "ksim_medic: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(kSpawnX, kSpawnZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_medic: SKIPPED: no " << region << "\n";
			return 77;
		}

		Game game(bake);
		game.newGame();
		Simulation& sim = *game.sim;
		sim.run(200);
		game.place(simlink::kHubX, simlink::kHubZ); // the player out of the way (the Hub: far from the wilderness spawn)
		sim.run(2);

		const kdata::Record* kit = recordNamed(sim, "ITEM", "Basic First Aid Kit");
		test::require(kit != nullptr, "the Basic First Aid Kit exists in the data");
		const Placed caravan = placeCaravan(game, 2);
		test::require(caravan.members.size() >= 3, "the caravan has a medic, a patient and a bystander");
		// A kit for the medic (two, so the second case's medic has a full one; the first aid spends charges).
		packOf(sim, caravan.members[0].id).add(kit->stringID, 1);
		packOf(sim, caravan.members[2].id).add(kit->stringID, 1);
		const Person patient = caravan.members[1];

		// A - the patient 5 u from the squad, injured: a squadmate takes 58 on it and the injury falls.
		state::Fighter& hurt = *fighterOf(sim, patient.id);
		injure(hurt, 40.0);
		const double injured = injuryOf(hurt);
		test::require(injured > 0.0, "the patient is injured");
		face(sim, caravan.members, patient.id);
		std::uint64_t medic = 0;
		for (int tick = 0; tick < 20 * 60 && medic == 0; ++tick)
		{
			sim.run(1);
			medic = medicOn(sim, caravan.members, patient.id);
			if (tick % 100 == 99 && std::getenv("KSIM_MEDIC_DEBUG") != nullptr)
			{
				for (const Person& p : caravan.members)
				{
					const state::Fighter* f = fighterOf(sim, p.id);
					const state::Routine* r = p.entity.get<state::Routine>();
					std::printf("  t%.1f member %llu at (%.1f, %.1f) y %.1f goal %u injury %.1f\n", tick * 0.05,
						static_cast<unsigned long long>(p.id), f->x, f->z, f->y, r != nullptr ? r->goal : 0u, injuryOf(*f));
				}
			}
		}
		test::require(medic != 0, "a squadmate took JOB_MEDIC 58 on the injured patient");
		std::cout << "medic " << medic << " took 58 on patient " << patient.id << " (injury " << injured << ")\n";
		const double before = injuryOf(*fighterOf(sim, patient.id));
		double best = before;
		for (int tick = 0; tick < 20 * 120; ++tick)
		{
			sim.run(1);
			best = std::min(best, injuryOf(*fighterOf(sim, patient.id)));
		}
		test::require(best < before, "the injury fell while the medic worked");
		std::cout << "the injury fell " << before << " -> " << best << "\n";

		// B - the patient 80 u beyond the squad (its centroid) and down: past the 40 u cap, no member takes 58; moved back within
		// reach, one does.
		double centreX = 0.0;
		double centreZ = 0.0;
		std::size_t others = 0;
		for (const Person& p : caravan.members)
		{
			if (p.id == patient.id)
				continue;
			const state::Fighter* f = fighterOf(sim, p.id);
			centreX += f->x;
			centreZ += f->z;
			++others;
		}
		centreX /= static_cast<double>(others);
		centreZ /= static_cast<double>(others);
		state::Fighter& body = *fighterOf(sim, patient.id);
		injure(body, 0.0); // back to full
		body.unconscious = true;
		body.koTimer = 600.0;
		body.x = centreX + kFarOffset;
		body.z = centreZ;
		body.y = sim.groundHeight(body.x, body.z).value_or(body.y);
		body.clearRoute();
		injure(body, 40.0);
		test::require(injuryOf(body) > 0.0, "the far patient is injured");
		sim.run(1);
		for (int tick = 0; tick < 20 * 15; ++tick)
		{
			sim.run(1);
			test::require(medicOn(sim, caravan.members, patient.id) == 0, "no member walks 80 u for the patient (the 40 u cap)");
		}
		body.x = centreX + kNearOffset;
		body.z = centreZ;
		body.y = sim.groundHeight(body.x, body.z).value_or(body.y);
		// A fresh kit each: the first case spent one, and B asks about distance, not charges.
		for (const Person& p : caravan.members)
			packOf(sim, p.id).add(kit->stringID, 1);
		sim.run(1);
		face(sim, caravan.members, patient.id);
		medic = 0;
		for (int tick = 0; tick < 20 * 30 && medic == 0; ++tick)
		{
			sim.run(1);
			medic = medicOn(sim, caravan.members, patient.id);
		}
		test::require(medic != 0, "moved back within reach, a squadmate takes 58 again");

		std::printf("medic: goal 58 on an injured squadmate (injury %.0f -> %.0f), none at 80 u, again within reach (member %llu)\n",
			injured, best, static_cast<unsigned long long>(medic));
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_medic: %s\n", e.what());
		return 1;
	}
}
