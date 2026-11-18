// Self-aid and who treats a down NPC (task k16.6.4; docs/reference/MEDICAL_HUNGER_LIVE.md (f), kinspect dt45_down_treatment.md):
//   A - a down, conscious NPC with a first-aid kit heals its own legs (goal 269 HEAL_MY_LEGS, Task_FirstAid on itself) and stands.
//   B - the same NPC with no kit stays down and conscious.
//   C - an ordinary squadmate carrying a kit, 3 u from a kitless down one, does nothing for it.
//   D - a medic-role character (the slaver caravan's package carries 58) treats a kitless down squadmate 6 u away, and does not at 49 u.
//   E - a kit handed to an already-down NPC starts its self-aid within one tick.
// Needs the world bake (argv[1], like ksim_medic; exit 77 without it).
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

std::vector<Person> personsOf(Simulation& sim, flecs::entity squad)
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

// Places a wilderness squad of `squadTemplate`, hostile to the player so its own combat never mixes in, and waits until its members
// are Fighters.
struct Placed
{
	flecs::entity squad;
	std::vector<Person> members;
};

Placed placeSquad(Game& game, const char* squadTemplate, std::size_t expected)
{
	Simulation& sim = *game.sim;
	const flecs::entity squad = sim.placeWildernessSquad(squadTemplate, kSpawnX, kSpawnZ);
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	test::require(instance != nullptr && instance->faction.id != 0, "the placed squad has a faction");
	sim.enqueue(Command::setFactionRelation(*ecsOf(sim).entity(instance->faction.id).get<state::Id>(),
		*playerFaction(sim).get<state::Id>(), -60));
	Placed placed;
	placed.squad = squad;
	for (int i = 0; i < 20 * 20; ++i)
	{
		sim.run(1);
		placed.members = personsOf(sim, squad);
		const bool built = std::all_of(placed.members.begin(), placed.members.end(), [&](const Person& p) { return sim.combat()->find(p.id) != nullptr; });
		if (built && placed.members.size() >= expected)
			break;
	}
	test::require(placed.members.size() >= expected, "the squad was built with its members");
	for (const Person& p : placed.members)
		test::require(sim.combat()->find(p.id) != nullptr, "the world squad's members are Fighters (it was promoted)");
	return placed;
}


// Whether any AI package a squad reads (its template's `AI packages`, then its faction's `AI fallback`: createBlackboard) holds goal 58
// in any role list: the squad has a medic role. The test picks its "ordinary" squad by this, not by watching who happens to go.
bool squadHasMedicRole(Simulation& sim, const routines::PackageTable& table, flecs::entity squad)
{
	const state::SquadInstance* instance = squad.get<state::SquadInstance>();
	std::vector<std::string> sids;
	if (const kdata::Record* record = sim.gameData()->find(instance->templateSid))
	{
		if (const kdata::RefCategory* links = record->findRefs("AI packages"))
		{
			for (const kdata::RefEntry& entry : links->entries)
				sids.push_back(entry.targetID);
		}
	}
	if (instance->faction.id != 0)
	{
		const state::FactionState* faction = ecsOf(sim).entity(instance->faction.id).get<state::FactionState>();
		const kdata::Record* record = faction != nullptr ? sim.gameData()->find(faction->templateSid) : nullptr;
		const kdata::RefCategory* fallback = record != nullptr ? record->findRefs("AI fallback") : nullptr;
		if (fallback != nullptr && !fallback->entries.empty())
			sids.push_back(fallback->entries.front().targetID);
	}
	for (const std::string& sid : sids)
	{
		const routines::CompiledPackage* package = table.find(sid);
		for (std::size_t role = 0; package != nullptr && role < routines::kRoleCount; ++role)
		{
			for (const routines::GoalRow& row : package->lists[role])
			{
				if (row.task == routines::task::FirstAid)
					return true;
			}
		}
	}
	return false;
}

// Puts the first collapsing organic leg 5 u below the KO point: the character goes down (checkCollapse) and, after the forced KO of
// the collapse, is awake.
void cripple(Simulation& sim, std::uint64_t id)
{
	state::Fighter& f = *fighterOf(sim, id);
	const double point = sim.combat()->rules().koPoint(f);
	for (state::FighterPart& part : f.parts)
	{
		if (!part.robotic && part.collapses && (part.collapseBits & (1 | 16 | 32)) != 0)
		{
			part.health = point - 5.0;
			return;
		}
	}
	throw std::runtime_error("no leg to cripple");
}

void teleport(Simulation& sim, std::uint64_t id, double x, double z)
{
	state::Fighter& f = *fighterOf(sim, id);
	f.x = x;
	f.z = z;
	f.y = sim.groundHeight(x, z).value_or(f.y);
	f.clearRoute();
}

// Runs until `id` is down and conscious (the forced KO of the collapse over); returns the ticks it took.
int untilAwakeDown(Simulation& sim, std::uint64_t id, int limit)
{
	for (int tick = 0; tick < limit; ++tick)
	{
		sim.run(1);
		const state::Fighter& f = *fighterOf(sim, id);
		if (f.down && !f.unconscious)
			return tick + 1;
	}
	return -1;
}

double poolOf(const state::Fighter& f)
{
	double sum = 0.0;
	for (const state::FighterPart& part : f.parts)
		sum += part.pool;
	return sum;
}

// Whether `who` treats `patient` now: a first aid interact on it.
bool treating(Simulation& sim, std::uint64_t who, std::uint64_t patient)
{
	const state::InteractTask* task = entityOf(sim, who).get<state::InteractTask>();
	return task != nullptr && task->action == static_cast<std::uint8_t>(InteractAction::FirstAid) && task->target == patient;
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
			std::cout << "ksim_self_aid: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		const kworld::ZoneCell zone = kworld::zoneCellOf(kSpawnX, kSpawnZ);
		const std::string region = bake + "/" + knav::regionFileName(zone.x / knav::kRegionZones, zone.z / knav::kRegionZones);
		if (test::bakeNav() && !std::filesystem::exists(region))
		{
			std::cout << "ksim_self_aid: SKIPPED: no " << region << "\n";
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

		// ---- an ordinary squad: the first template with 3+ members whose packages (and faction fallback) hold no goal 58 ----
		const routines::PackageTable table(*sim.gameData());
		std::vector<Person> band;
		std::string bandTemplate;
		int offset = 0;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("SQUAD_TEMPLATE")))
		{
			if (++offset > 60)
				break;
			const flecs::entity squad = sim.placeWildernessSquad(record->stringID, kSpawnX + 300.0 * (offset % 8), kSpawnZ - 300.0 * (offset / 8));
			if (!squad.is_valid())
				continue;
			std::vector<Person> members;
			for (int i = 0; i < 400; ++i)
			{
				sim.run(1);
				members = personsOf(sim, squad);
				if (!members.empty() && std::all_of(members.begin(), members.end(), [&](const Person& q) { return sim.combat()->find(q.id) != nullptr; }))
					break;
			}
			const bool built = members.size() >= 3
				&& std::all_of(members.begin(), members.end(), [&](const Person& q) { return sim.combat()->find(q.id) != nullptr; });
			if (built && !squadHasMedicRole(sim, table, squad))
			{
				band = members;
				bandTemplate = record->stringID;
				break;
			}
		}
		test::require(!band.empty(), "an ordinary squad (no goal 58 in its packages) of 3+ members exists in the data");
		std::printf("ordinary squad: %s (%zu members)\n", bandTemplate.c_str(), band.size());
		const std::uint64_t victim = band[1].id;
		const std::uint64_t mate = band[2].id;

		// B - no kit: the victim goes down, wakes from the collapse's KO and stays down and conscious.
		cripple(sim, victim);
		const int woke = untilAwakeDown(sim, victim, 20 * 30);
		test::require(woke > 0, "B: the crippled bandit is down and conscious after the collapse");
		std::printf("B: down and awake %d ticks after the collapse\n", woke);
		for (int tick = 0; tick < 20 * 60; ++tick)
		{
			sim.run(1);
			test::require(fighterOf(sim, victim)->down && !fighterOf(sim, victim)->unconscious, "B: with no kit it stays down and conscious");
			test::require(!treating(sim, victim, victim), "B: no self-aid without a kit");
		}

		// C - a squadmate with a kit, 3 u away: it does nothing for the victim.
		packOf(sim, mate).add(kit->stringID, 1);
		teleport(sim, mate, fighterOf(sim, victim)->x + 3.0, fighterOf(sim, victim)->z);
		face(sim, band, victim);
		for (int tick = 0; tick < 20 * 60; ++tick)
		{
			sim.run(1);
			test::require(!treating(sim, mate, victim), "C: an ordinary squadmate with a kit never treats the down one");
			test::require(fighterOf(sim, victim)->down, "C: the victim is still down");
		}
		std::printf("C: the squadmate with a kit did nothing for 60 s\n");

		// E - a kit handed to the down victim: self-aid starts within one tick.
		const double poolBefore = poolOf(*fighterOf(sim, victim));
		packOf(sim, victim).add(kit->stringID, 1);
		int started = -1;
		for (int tick = 0; tick < 3 && started < 0; ++tick)
		{
			sim.run(1);
			if (treating(sim, victim, victim) || poolOf(*fighterOf(sim, victim)) > poolBefore)
				started = tick + 1;
		}
		test::require(started > 0 && started <= 1, "E: a kit handed to a down NPC starts self-aid within one tick");
		std::printf("E: self-aid started %d tick(s) after the kit\n", started);

		// A - ... and it stands.
		int stood = -1;
		for (int tick = 0; tick < 20 * 90 && stood < 0; ++tick)
		{
			sim.run(1);
			if (!fighterOf(sim, victim)->down)
				stood = tick + 1;
		}
		test::require(stood > 0, "A: the NPC with a kit heals its legs and stands");
		std::printf("A: stood %.1f s after the kit; charges left %.1f\n", stood * 0.05, fighterOf(sim, victim)->kitCharges);

		// D - a medic: the caravan's package carries 58. A fresh world, so no squad left over from the search above fights it (a squad in a
		// fight runs no goals). A kitless victim 6 u from the medics is treated, one 49 u away is not.
		Game game2(bake);
		game2.newGame();
		Simulation& sim2 = *game2.sim;
		sim2.run(200);
		game2.place(simlink::kHubX, simlink::kHubZ);
		sim2.run(2);
		const Placed caravan = placeSquad(game2, "3387-gamedata.base", 3);
		test::require(caravan.members.size() >= 3, "the caravan has a medic, a patient and a bystander");
		const std::uint64_t patient = caravan.members[1].id;
		for (const Person& p : caravan.members)
		{
			if (p.id != patient)
				packOf(sim2, p.id).add(kit->stringID, 1); // every squadmate is a possible medic, the patient has no kit: the distance is what is asked
		}
		const auto otherTreats = [&]() -> bool
		{
			for (const Person& p : caravan.members)
			{
				if (p.id != patient && treating(sim2, p.id, patient))
					return true;
			}
			return false;
		};
		// The squad (every member but the patient) `distance` u from the patient, on a line, turned toward it.
		const auto gather = [&](double distance)
		{
			const state::Fighter& body = *fighterOf(sim2, patient);
			for (const Person& p : caravan.members)
			{
				if (p.id != patient)
					teleport(sim2, p.id, body.x + distance, body.z);
			}
			face(sim2, caravan.members, patient);
		};
		// The squad snaps back toward its leader, so the gap is re-made before every tick: at each AI evaluation the medics are exactly
		// `distance` away.
		cripple(sim2, patient);
		bool awake = false;
		for (int tick = 0; tick < 20 * 30 && !awake; ++tick)
		{
			gather(49.0);
			sim2.run(1);
			awake = fighterOf(sim2, patient)->down && !fighterOf(sim2, patient)->unconscious;
			test::require(!otherTreats(), "D: a medic 49 u away does not treat");
		}
		test::require(awake, "D: the caravan's patient is down and conscious");
		for (int tick = 0; tick < 20 * 30; ++tick)
		{
			gather(49.0);
			sim2.run(1);
			test::require(!otherTreats(), "D: a medic 49 u away does not treat");
		}
		bool treated = false;
		for (int tick = 0; tick < 20 * 30 && !treated; ++tick)
		{
			gather(6.0);
			sim2.run(1);
			treated = otherTreats();
		}
		test::require(treated, "D: a medic 6 u away treats the down squadmate");
		std::printf("D: none at 49 u, a medic at 6 u treats\n");
		return 0;
	} catch (const std::exception& e) {
		std::printf("ksim_self_aid: %s\n", e.what());
		return 1;
	}
}
