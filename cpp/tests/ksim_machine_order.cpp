// Player orders on kind 12 machines (kenshi-k16.6.788, kenshi-gzto): order 60 action 4 (Use) on a placed converter gives the member goal 87
// OPERATE_MACHINERY (CHAR_STATS), on a placed turret mans it (Fighter::mount, the slot the routine gunner takes), and keeps the refusals:
// another faction's machine "not_allowed", a manned turret "full", a converter with every slot taken "no_slot", an automatic machine
// (OPERATE_AUTOMATIC_MACHINERY 152: no player order) "bad_target". A replacing order lets the turret go.
// The order is held from a distance first (no goal 87 until the member is within the record's `use range`), then at the machine.
// The walk itself (routeToInteract) is the interact tests' business; the test has no navmesh.

#include "ksim/combat/combat.hpp"
#include "ksim/core/simulation.hpp"
#include "ksim/economy/machines.hpp"
#include "ksim/world/buildings.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/world/world.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <cmath>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace ksim;

constexpr double kCampX = 64742.0; // the South Stone Camp (ksim_machine_records)
constexpr double kCampZ = -49500.0;
constexpr std::uint8_t kUse = static_cast<std::uint8_t>(InteractAction::Use);

} // namespace

int main()
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		SpawnOptions options;
		options.seed = 12;
		options.wildernessByDensity = false;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		const std::shared_ptr<const kdata::GameData> gd = sim.gameData();
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);

		const MachineSite* turret = nullptr;
		const MachineSite* converter = nullptr;
		const MachineSite* automaticConverter = nullptr;
		for (const MachineSite& s : sim.world().machineSites())
		{
			if (s.producerId != 0 || std::hypot(s.x - kCampX, s.z - kCampZ) > 1100.0 || s.ownerFaction.empty())
				continue;
			const machines::Static& info = sim.world().machineInfo(s);
			if (info.kind == machines::Kind::Turret && turret == nullptr && sim.turretPostOf(s.key) != nullptr)
				turret = &s;
			else if (info.kind == machines::Kind::Converter && info.maxWorkers > 0 && converter == nullptr)
				converter = &s;
			else if (info.kind == machines::Kind::Converter && info.maxWorkers == 0 && automaticConverter == nullptr)
				automaticConverter = &s;
		}
		test::require(turret != nullptr && converter != nullptr, "the camp has an owned turret and an operated converter");

		const kdata::Record* servant = nullptr;
		for (const kdata::Record* r : gd->byType(kworld::itemTypeByName("CHARACTER")))
			if (r->name == "Escaped Servant")
				servant = r;
		test::require(servant != nullptr, "Escaped Servant record");
		std::uint64_t rng = 3;
		const auto spawn = [&](const MachineSite& at, const std::string& faction)
		{
			state::Fighter f = data->makeFighter(*gd, servant->stringID, "", rng);
			f.team = 0;
			f.playerControlled = true;
			f.x = at.x;
			f.y = at.y;
			f.z = at.z;
			flecs::entity e = sim.spawnFighter(std::move(f));
			e.set<state::Allegiance>(state::Allegiance{faction});
			return e.get<state::Id>()->value;
		};
		const auto order60 = [&](std::uint64_t who, const MachineSite& site)
		{
			const InteractTarget target = findInteractTarget(sim.world().ecs(), sim.combat(), site.id, &sim);
			test::require(target.kind == InteractKind::Machine, "findInteractTarget names the machine " + site.recordSid);
			return checkInteract(sim.world().ecs(), *sim.combat(), who, target, kUse, 0, gd.get(), &sim);
		};
		flecs::world& ecs = const_cast<flecs::world&>(sim.world().ecs());
		const auto entityOf = [&](std::uint64_t who)
		{
			flecs::entity found;
			ecs.each([&](flecs::entity e, const state::Id& id, const state::Fighter&) { if (id.value == who) found = e; });
			return found;
		};
		const auto hold = [&](std::uint64_t who, const MachineSite& site)
		{
			entityOf(who).set<state::InteractTask>(state::InteractTask{site.id, kUse, 1, 0});
		};

		// ---- a converter: accepted, then goal 87 once the member stands at it ----
		const std::uint64_t worker = spawn(*converter, converter->ownerFaction);
		test::require(!converter->ownerFaction.empty(), "the converter has an owner, so the faction check is real");
		const double reach = sim.world().machineInfo(*converter).useRange;
		test::require(order60(worker, *converter).empty(), "a converter of the member's faction takes the Use order");
		// Held from beyond twice the use range: the order stands but the member is not at work.
		const_cast<CombatArena*>(sim.combat())->find(worker)->x = converter->x + 3.0 * reach;
		hold(worker, *converter);
		for (int i = 0; i < 10; ++i)
			sim.tick();
		test::require(sim.charStats(worker).goalEnum != 87, "no goal 87 while the member is outside the use range");
		const_cast<CombatArena*>(sim.combat())->find(worker)->x = converter->x + 0.5 * reach;
		hold(worker, *converter); // a stalled walk without a navmesh may have ended the order
		for (int i = 0; i < 40; ++i)
			sim.tick();
		const Simulation::CharStats stats = sim.charStats(worker);
		test::require(stats.goalEnum == 87, "the operator's goal is OPERATE_MACHINERY 87, got " + std::to_string(stats.goalEnum));
		test::require(stats.targetId == converter->id, "goal 87 names the converter");
		// The row counts the operator (k16.6.839): workers 1, Working, version up; after release No workers again.
		const auto rowWith = [&](const MachineSite& site)
		{
			const auto held = sim.machineOperators();
			const auto it = held.find(site.id);
			return sim.machineView(site, it != held.end() ? static_cast<int>(it->second) : 0);
		};
		const std::optional<Simulation::BuildingView> busy = rowWith(*converter);
		test::require(busy && busy->workers == 1 && busy->status == buildings::kWorking, "a held converter reads 1 worker, Working");
		entityOf(worker).remove<state::InteractTask>();
		const std::optional<Simulation::BuildingView> idle = rowWith(*converter);
		test::require(idle && idle->workers == 0 && idle->status == buildings::kNoWorkers && idle->version < busy->version, "a released converter reads No workers, an older version");
		hold(worker, *converter);
		for (int i = 0; i < 4; ++i)
			sim.tick();
		std::cout << "converter: goal 87 at " << converter->recordSid << "\n";

		// ---- a rejection: another faction's machine ----
		const std::uint64_t stranger = spawn(*converter, "no-such-faction");
		test::require(order60(stranger, *converter) == "not_allowed", "another faction's converter is not_allowed");

		// ---- the slots: a second worker on a one-slot converter is no_slot ----
		if (sim.world().machineInfo(*converter).maxWorkers == 1)
			test::require(order60(spawn(*converter, converter->ownerFaction), *converter) == "no_slot", "a full converter answers no_slot");

		// ---- automatic machinery (152): no player order ----
		if (automaticConverter != nullptr)
			test::require(order60(worker, *automaticConverter) == "bad_target", "an automatic machine takes no Use order");

		// ---- a turret: accepted, mounted, full for the next, let go by a replacing order ----
		const std::uint64_t gunner = spawn(*turret, turret->ownerFaction);
		test::require(order60(gunner, *turret).empty(), "a turret of the member's faction takes the Use order");
		hold(gunner, *turret);
		for (int i = 0; i < 40; ++i)
			sim.tick();
		const state::Fighter* body = sim.combat()->find(gunner);
		test::require(body != nullptr && body->mount.mounted() && body->mount.building == turret->key, "the member mans the turret");
		test::require(order60(spawn(*turret, turret->ownerFaction), *turret) == "full", "a manned turret answers full");
		entityOf(gunner).remove<state::InteractTask>();
		for (int i = 0; i < 4; ++i)
			sim.tick();
		test::require(!sim.combat()->find(gunner)->mount.mounted(), "a replacing order lets the turret go");
		const std::optional<Simulation::BuildingView> row = sim.machineView(*turret, 1);
		test::require(row && row->workers == 1, "the turret's wire row shows the gunner");
		std::cout << "turret: manned " << turret->recordSid << ", full, released\n";
		std::cout << "ksim_machine_order: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
}
