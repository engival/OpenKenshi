// The due-stamps of kenshi-k16.6.637 (save v130), seen through a Simulation:
// - the arena -> ECS mirror copies only the members that stepped or were written, and no ECS reader sees a stale Fighter: every tick the
//   ECS copy of every member equals the arena's, with the fight tier at 1 and everyone else at stride 8, and a write from outside (a
//   mutable find) is read back from the ECS after the next tick;
// - the previous version's save (v130 on this build, no `offPath`) loads it as 0 and the world plays on;
// - a save and load of the current version gives the saved state hash.

#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/save/save.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "sim_access.hpp"
#include "test_util.hpp"

#include <cstdio>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

const state::Fighter* ecsFighter(Simulation& sim, std::uint64_t id)
{
	const state::Fighter* found = nullptr;
	sim.world().ecs().each([&](const state::Id& i, const state::Fighter& f) { if (i.value == id) found = &f; });
	return found;
}

bool same(const state::Fighter& a, const state::Fighter& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z && a.yaw == b.yaw && a.hunger == b.hunger && a.blood == b.blood && a.target == b.target
		&& a.state == b.state && a.senseAt == b.senseAt && a.medStep == b.medStep && a.hungerStep == b.hungerStep && a.sight.size() == b.sight.size()
		&& a.hostileSquads == b.hostileSquads && a.engaged == b.engaged && a.unconscious == b.unconscious && a.dead == b.dead
		&& a.parts.size() == b.parts.size() && a.parts[0].health == b.parts[0].health && a.forwardSpeed == b.forwardSpeed && a.velX == b.velX;
}

void testMirror(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, const kdata::Record* servant)
{
	Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
	CombatSettings settings;
	settings.substeps = 2;
	settings.idleStride = 8;
	sim.enableCombat(data, settings);
	std::uint64_t rng = 99;
	std::vector<std::uint64_t> ids;
	for (int i = 0; i < 24; ++i)
	{
		state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
		f.x = 40.0 * (i % 6);
		f.z = 40.0 * (i / 6);
		f.faction = i < 12 ? 10 : 20; // two sides close enough to see each other
		f.team = static_cast<int>(f.faction);
		ids.push_back(sim.spawnFighter(std::move(f)).get<state::Id>()->value);
	}
	int checked = 0;
	for (int tick = 0; tick < 240; ++tick)
	{
		if (tick == 100)
			fighterOf(sim, ids[3])->target = ids[15]; // an order from outside: a fight starts
		sim.run(1);
		for (const std::uint64_t id : ids)
		{
			const state::Fighter* mirrored = ecsFighter(sim, id);
			test::require(mirrored != nullptr, "the ECS holds the Fighter");
			test::require(same(*mirrored, *std::as_const(*sim.combat()).find(id)), "tick " + std::to_string(tick) + ": the ECS copy of member " + std::to_string(id) + " is stale");
			++checked;
		}
	}
	// A write from outside is read back.
	state::Fighter* w = fighterOf(sim, ids[7]);
	w->hunger = 1.25;
	w->x += 3.0;
	sim.run(1);
	const state::Fighter* back = ecsFighter(sim, ids[7]);
	test::require(back->hunger > 1.24 && back->hunger <= 1.25 && back->x == std::as_const(*sim.combat()).find(ids[7])->x, "an outside write is read back from the ECS");
	std::printf("ok  mirror: %d member-ticks equal arena and ECS at stride 8; an outside write is read back\n", checked);
}

void testOldSave(const kdata::GameData& gd, const std::shared_ptr<const CombatData>& data, const kdata::Record* servant)
{
	Simulation sim = Simulation::load(KENSHI_DATA, 5, RegionConfig{});
	CombatSettings settings;
	settings.substeps = 2;
	sim.enableCombat(data, settings);
	std::uint64_t rng = 7;
	state::Fighter f = data->makeFighter(gd, servant->stringID, "", rng);
	f.hunger = 2.0;
	f.offPath = 2.5; // v132's field: a previous-version file does not carry it
	const std::uint64_t id = sim.spawnFighter(std::move(f)).get<state::Id>()->value;
	test::require(ecsFighter(sim, id)->offPath == 2.5, "the ECS holds the time off route");

	const std::string path = std::string(KSIM_BINARY_DIR) + "/ksim_save_stamps_prev.ksav";
	writeSaveFile(path, sim.world().ecs(), sim.world().loadOrder(), {}, kSaveVersion - 1);
	Simulation old = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), path, false, nullptr, RegionConfig{});
	std::remove(path.c_str());
	old.enableCombat(data, settings);
	const state::Fighter* read = ecsFighter(old, id);
	test::require(read != nullptr && read->offPath == 0.0, "a previous-version file loads the time off route as 0");
	old.run(200);
	const state::Fighter* now = std::as_const(*old.combat()).find(id);
	test::require(now != nullptr && now->senseAt > state::Fighter::kNoStamp * 0.5, "the sensor stamp is placed");
	test::require(now->medStep != state::Fighter::kNoStep && now->hungerStep != state::Fighter::kNoStep && now->hunger < 2.0, "the stamps are placed and the member got hungry");
	test::require(same(*ecsFighter(old, id), *now), "the loaded member is mirrored");
	std::printf("ok  a previous-version save loads (offPath 0) and plays on\n");

	// The current version round-trips.
	const std::string again = std::string(KSIM_BINARY_DIR) + "/ksim_save_stamps_v132.ksav";
	const std::uint64_t held = old.stateHash();
	old.saveGame(again);
	Simulation loaded = Simulation::loadGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA), again, false, nullptr, RegionConfig{});
	std::remove(again.c_str());
	test::require(loaded.stateHash() == held, "a v132 save loads to its hash");
	std::printf("ok  a v132 save round-trips (hash %llx)\n", static_cast<unsigned long long>(held));
}

} // namespace

int main()
{
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(KENSHI_DATA);
		const std::shared_ptr<const CombatData> data = CombatData::build(gd, KENSHI_DATA);
		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "no Escaped Servant");
		testMirror(gd, data, servant);
		testOldSave(gd, data, servant);
		std::cout << "ksim_save_stamps: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_save_stamps: FAILED: " << e.what() << "\n";
		return 1;
	}
}
