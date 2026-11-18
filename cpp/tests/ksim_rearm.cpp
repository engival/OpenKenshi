// 42 DON'T BE UNARMED (task dt25; kinspect dt25_bodyguard_unarmed_putdown.md section 2). The finder-less goal is met while the
// member has a weapon (hasWeapon: the hands, the back or the pack); when it is disarmed, the rearm loop runs -
// 8 FIND_WEAPON (the nearest loose weapon on the ground within 100 u, else an armed down or dead body) then 6 EQUIP_WEAPON -
// and the member ends up holding it. An animal never searches: its natural weapon is its weapon (dt25 section 2.2; the
// coordinator's pin), so a Beak Thing beside a loose sword neither takes a goal nor touches it.
// A bare world (Simulation::load): a hand-made Berserker squad (package "Patrol map: Aggressive" carries 42 on both lists), the
// weapon is a spawned GroundItem within the pick-up reach, so no navigation is needed.

#include "ksim/combat/combat_data.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/world/world.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"
#include "sim_access.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

// Open ground outside the Hub's buildings (the animal-feed fixture's spot).
constexpr double kX = -50978.8;
constexpr double kZ = 2932.48;
// "Berserker squad": its package "Patrol map: Aggressive" carries 42 DON'T BE UNARMED on the Squad and Leader lists.
constexpr const char* kSquadTemplate = "42258-rebirth.mod";

std::string recordSid(const kdata::GameData& gd, std::string_view type, std::string_view name)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName(type)))
		if (record->name == name)
			return record->stringID;
	throw std::runtime_error("no " + std::string(type) + " " + std::string(name));
}

std::string namelessSid(const kdata::GameData& gd)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("FACTION")))
		if (record->name == "Nameless")
			return record->stringID;
	throw std::runtime_error("no FACTION Nameless");
}

struct Rig
{
	std::shared_ptr<const kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	Simulation sim;
	std::uint64_t rng = 13;
	std::string body;
	std::string beak; // the ANIMAL_CHARACTER the animal case spawns
	std::string weapon; // the loose WEAPON the case drops
	flecs::entity squad;
	std::uint64_t leader = 0;
	std::uint64_t member = 0;

	Rig()
		: gd(std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(KENSHI_DATA)))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::load(KENSHI_DATA, 91))
	{
		sim.setGameData(gd);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		body = recordSid(*gd, "CHARACTER", "Escaped Servant");
		beak = recordSid(*gd, "ANIMAL_CHARACTER", "Beak Thing");
		weapon = firstWeapon(*gd);
		// A player faction and a squad at Full detail with a position: the resident pass keeps such a world squad built.
		sim.createPlayerFaction(namelessSid(*gd));
		squad = state::createInstance(ecsOf(sim));
		state::SquadInstance instance;
		instance.templateSid = kSquadTemplate;
		squad.set<state::SquadInstance>(std::move(instance));
		squad.set<state::Position>(state::Position{kX, kZ});
		squad.set<Detail>(Detail{DetailLevel::Full});
		leader = spawn(body, kX, kZ);
		member = spawn(body, kX + 6.0, kZ);
		join(leader);
		join(member);
	}

	static std::string firstWeapon(const kdata::GameData& gd)
	{
		for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("WEAPON")))
			if (!record->name.empty() && record->intOr("category", 0) >= 0)
				return record->stringID;
		throw std::runtime_error("the data has no WEAPON record");
	}

	std::uint64_t spawn(const std::string& sid, double x, double z)
	{
		state::Fighter f = data->makeFighter(*gd, sid, "", rng);
		f.x = x;
		f.z = z;
		f.y = 1647.0;
		f.team = 0;
		f.hunger = 3.0;
		return sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	}

	flecs::entity entityOf(std::uint64_t id)
	{
		flecs::entity found;
		ecsOf(sim).each([&](flecs::entity e, const state::Id& key)
		{
			if (key.value == id)
				found = e;
		});
		return found;
	}
	state::Fighter& fighterOf(std::uint64_t id) { return *const_cast<CombatArena*>(sim.combat())->find(id); }
	bool exists(std::uint64_t id)
	{
		bool found = false;
		ecsOf(sim).each([&](const state::Id& key) { found = found || key.value == id; });
		return found;
	}

	void join(std::uint64_t id)
	{
		flecs::entity entity = entityOf(id);
		entity.add<state::MemberOf>(squad);
		entity.set<state::MemberRole>(state::MemberRole{});
		fighterOf(id).squad = squad.get<state::Id>()->value;
	}

	std::uint64_t drop(double x, double z)
	{
		state::GroundItem item;
		item.itemSid = weapon;
		item.count = 1;
		item.quality.level = 1.0; // a generated weapon has a quality; equipping an instance without one is refused
		item.quality.grade = 2;
		item.x = item.useX = x;
		item.z = item.useZ = z;
		item.y = item.useY = 1647.0;
		return sim.spawnGroundItem(std::move(item)).get<state::Id>()->value;
	}

	void tick(int n)
	{
		for (int i = 0; i < n; ++i)
			sim.tick();
	}
};

void rearm()
{
	Rig rig;
	const std::string weapon = rig.weapon;
	// A hand-made member straight from its record may carry its own weapon: the case starts it clean.
	state::Fighter& memberFighter = rig.fighterOf(rig.member);
	memberFighter.weapon = rig.data->unarmed;
	flecs::entity me = rig.entityOf(rig.member);
	state::Inventory empty = *me.get<state::Inventory>();
	empty.items.clear();
	empty.version = 1;
	me.set<state::Inventory>(std::move(empty));
	test::require(!rig.fighterOf(rig.member).carrying, "the member carries nothing");
	const std::uint64_t loose = rig.drop(kX + 10.0, kZ); // inside the 21 u pick-up reach
	for (int i = 0; i < 30 && rig.fighterOf(rig.member).weapon.sid != weapon; ++i)
		rig.tick(10); // the selector's 0.4 s pass, the pick-up, then 6 EQUIP_WEAPON
	test::require(rig.fighterOf(rig.member).weapon.sid == weapon, "the disarmed member did not pick the loose weapon up and equip it");
	test::require(!rig.exists(loose), "the picked weapon is still lying on the ground");
	std::cout << "  rearm: the loose weapon was taken and equipped\n";
}

void animalNeverSearches()
{
	Rig rig;
	const std::uint64_t animal = rig.spawn(rig.beak, kX + 6.0, kZ + 4.0);
	rig.join(animal);
	test::require(rig.fighterOf(animal).animal, "the fixture's animal is not an animal");
	const std::uint64_t loose = rig.drop(kX + 10.0, kZ + 4.0);
	rig.tick(600); // 10 s: plenty of selector passes
	const state::Routine* routine = rig.entityOf(animal).get<state::Routine>();
	test::require(routine == nullptr, "the animal started a routine (goal " + std::to_string(routine != nullptr ? routine->goal : 0) + ")");
	test::require(rig.exists(loose), "the animal took a weapon off the ground");
	std::cout << "  animal: no FIND_WEAPON, the loose weapon stays\n";
}

} // namespace

int main()
{
	try
	{
		rearm();
		animalNeverSearches();
		std::cout << "ksim_rearm: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_rearm: FAILED: " << e.what() << "\n";
		return 1;
	}
}
