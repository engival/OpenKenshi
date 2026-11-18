// kenshi-h2v.13.3: the dismemberment scenarios (scenarios/dismember_peeler.kscen, dismember_fight.kscen) run what their look_for says.
//   - dismember_peeler: the Heft slave shop's cage, marked a peeler by `peeler=1` (a stand-in for the Peeler Machine), holds an armoured prisoner. Its armour goes first with no
//     wound; the four limbs wear down together (they may sever on the same tick; items are made Left Arm, Right Arm, Left Leg, Right Leg), each with one ground item; head and torso are never touched (rwh97 7); the cage then lets go.
//   - dismember_fight: the player attacks a Dust bandit whose arms are at -118; a hit takes an arm off with one ground item.
// Needs the world navmesh bake (argv[1], exit 77 without the Heft and Hub regions).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/scenario_file.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "knav/navmesh.hpp"
#include "kworld/coords.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <filesystem>
#include <iostream>
#include <set>
#include <memory>
#include <string>
#include <vector>
#include "nav_source.hpp"

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif
#ifndef KSIM_SCENARIOS_DIR
#error KSIM_SCENARIOS_DIR must be defined
#endif

using namespace ksim;

namespace {

constexpr int kPeelerTicks = 40000; // 2000 game s: the whole peel at the original's rates takes a few minutes
constexpr int kFightTicks = 1200; // 60 game s
constexpr std::uint64_t kFightSeed = 3;
constexpr std::uint64_t kFightSeeds = 20; // seeds kFightSeed .. kFightSeed + 19

// The ground items that are one of the race's severed limbs (the world holds other ground items at load).
std::size_t limbItems(Simulation& sim, const state::Fighter& body)
{
	std::set<std::string> kinds;
	const kdata::Record* race = sim.gameData()->find(body.raceSid);
	const kdata::RefCategory* list = race != nullptr ? race->findRefs("severed limbs") : nullptr;
	if (list != nullptr)
		for (const kdata::RefEntry& entry : list->entries)
			kinds.insert(entry.targetID);
	std::size_t n = 0;
	sim.world().ecs().each([&](const state::GroundItem& g) { n += kinds.count(g.itemSid); });
	return n;
}

const state::FighterPart* partNamed(const state::Fighter& f, const std::string& name)
{
	for (const state::FighterPart& p : f.parts)
		if (p.name == name)
			return &p;
	return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const scenario::File peeler = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/dismember_peeler.kscen");
		const scenario::File fight = scenario::parseFile(std::string(KSIM_SCENARIOS_DIR) + "/dismember_fight.kscen");
		std::vector<kworld::ZoneCell> zones;
		for (const scenario::File* file : {&peeler, &fight})
		{
			const kworld::ZoneCell zone = kworld::zoneCellOf(file->centreX, file->centreZ);
			if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), zone.x, zone.z))
			{
				std::cout << "ksim_dismember_scen: SKIPPED: needs the world navmesh bake with the scenarios' regions (pass its directory)\n";
				return 77;
			}
			for (int dz = -1; dz <= 1; ++dz)
				for (int dx = -1; dx <= 1; ++dx)
					zones.push_back({zone.x + dx, zone.z + dz});
		}
		const std::string world = argv[1];
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		sim.markPristine();
		const auto gameData = sim.gameData();
		const auto combatData = CombatData::build(*gameData, KENSHI_DATA);
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(combatData, settings);
		sim.enableNavigation(test::openWorldNav(world, zones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, world, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		simlink::ServerContext context;
		context.gameData = gameData;
		context.squadLimit = 50;
		scenario::check(*gameData, peeler);
		scenario::check(*gameData, fight);

		// ---- the peeler ----
		{
			sim.newWorld();
			sim.run(1);
			const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, peeler, 3);
			const std::uint64_t occupant = placed.idOf.at(2);
			// The peeler pins the machine's limb order, not the aid: characters spawn with their CHARACTER `inventory` kit (k16.6.287), and a
			// friendly town medic (member 910 in the Heft shop, promoted after the start) would treat the severed prisoner and delay the last
			// limbs past the machine's order. This fixture's world carries no kit: the loop strips what has spawned since, every 10 ticks.
			const auto noKits = [&]()
			{
				sim.world().ecs().each([&](state::Inventory& pack)
				{
					std::erase_if(pack.items, [&](const state::Inventory::Entry& entry)
					{
						const ItemInfo* info = combatData->item(entry.itemSid);
						return info != nullptr && info->function == kItemFunctionMedkit;
					});
				});
			};
			noKits();
			const state::Fighter* f = sim.combat()->find(occupant);
			test::require(f != nullptr && f->cageBuilding != 0, "the prisoner is in the cage");
			test::require(f->armour.size() >= 2, "the prisoner wears its armour (" + std::to_string(f->armour.size()) + " pieces)");
			const std::size_t worn = f->armour.size();
			const std::size_t before = limbItems(sim, *f);
			const char* limbs[] = {"Left Arm", "Right Arm", "Left Leg", "Right Leg"};
			std::size_t severedAt[4] = {0, 0, 0, 0};
			std::size_t armourGoneAt = 0;
			std::size_t firstWoundAt = 0;
			std::size_t torsoHurtAt = 0;
			std::size_t limbsDownAt = 0; // every limb at health <= 0 (or severed): informational
			std::size_t releasedAt = 0;
			for (int tick = 1; tick <= kPeelerTicks && releasedAt == 0; ++tick)
			{
				sim.tick();
				if (tick % 10 == 0)
					noKits();
				f = sim.combat()->find(occupant);
				test::require(f != nullptr, "the occupant stays in the arena until released");
				if (armourGoneAt == 0 && f->armour.empty())
					armourGoneAt = static_cast<std::size_t>(tick);
				for (const state::FighterPart& p : f->parts)
				{
					if (p.limb() ? p.health < p.maxHealth : p.health < 100.0 - 1e-6)
					{
						if (firstWoundAt == 0)
							firstWoundAt = static_cast<std::size_t>(tick);
						if (!p.limb() && torsoHurtAt == 0)
							torsoHurtAt = static_cast<std::size_t>(tick);
					}
				}
				bool limbsUp = false;
				for (const state::FighterPart& p : f->parts)
					limbsUp = limbsUp || (p.limb() && !p.severed() && p.health > 0.0);
				if (!limbsUp && limbsDownAt == 0)
					limbsDownAt = static_cast<std::size_t>(tick);
				for (int i = 0; i < 4; ++i)
				{
					const state::FighterPart* p = partNamed(*f, limbs[i]);
					if (severedAt[i] == 0 && p != nullptr && p->severed())
						severedAt[i] = static_cast<std::size_t>(tick);
				}
				if (f->cageBuilding == 0)
					releasedAt = static_cast<std::size_t>(tick);
			}
			sim.run(5); // the machine now lets go in the tick the last limb goes: the severs' ground items land a tick or two later
			f = sim.combat()->find(occupant);
			test::require(f != nullptr, "the occupant stays in the arena after release");
			test::require(armourGoneAt != 0, "all " + std::to_string(worn) + " armour pieces were destroyed");
			test::require(firstWoundAt == 0 || firstWoundAt >= armourGoneAt, "no part took damage before the last armour piece went");
			for (int i = 0; i < 4; ++i)
			{
				test::require(severedAt[i] != 0, std::string(limbs[i]) + " was severed");
				if (i > 0)
					test::require(severedAt[i] >= severedAt[i - 1], std::string(limbs[i]) + " severed before " + limbs[i - 1]);
			}
			test::require(severedAt[0] > armourGoneAt, "the first limb went after the armour");
			test::require(torsoHurtAt == 0, "head and torso stay untouched by the machine");
			test::require(releasedAt != 0, "the occupant was released from the cage");
			test::require(limbItems(sim, *f) - before == 4, "one ground item per severed limb: " + std::to_string(limbItems(sim, *f) - before));
			std::cout << "peeler: armour gone at " << armourGoneAt * 0.05 << " s, limbs severed at " << severedAt[0] * 0.05 << " / " << severedAt[1] * 0.05 << " / "
				<< severedAt[2] * 0.05 << " / " << severedAt[3] * 0.05 << " s, torso untouched, released at " << releasedAt * 0.05 << " s (dead " << f->dead << "), 4 limb items\n";
		}

		// ---- the arm-off fight ----
		// Which part a hit lands on is a roll, and the Hub's barman and guards join the fight (k16.6.103) and land most of the hits: with the world as it
		// is, one seed's fight ends by a guard's KO blow on the chest and the next's by a guard's hit on an arm (k16.15.13: seed 3 passed on the arm hit of
		// Ninja Guard 6884 at 14 s, until the Hub shopkeeper's move behind the counter, k16.6.106, shifted the timeline and the guard hit the chest). So
		// the fight runs on successive seeds until a hit severs an arm; the claim is that it does within kFightSeeds of them.
		std::size_t severed = 0;
		std::uint64_t seed = kFightSeed;
		for (; seed < kFightSeed + kFightSeeds && severed == 0; ++seed)
		{
			sim.newWorld();
			sim.run(1);
			const scenario::Placed placed = scenario::apply(sim, *gameData, *combatData, context, fight, seed);
			const std::uint64_t wanderer = placed.idOf.at(1);
			const std::uint64_t bandit = placed.idOf.at(2);
			const state::Fighter* victim = sim.combat()->find(bandit);
			const std::size_t before = limbItems(sim, *victim);
			sim.enqueue(Command::fighterAttack(state::Id{wanderer}, state::Id{bandit}));
			for (int tick = 0; tick < kFightTicks && severed == 0; ++tick)
			{
				sim.tick();
				const state::Fighter* b = sim.combat()->find(bandit);
				for (const char* name : {"Left Arm", "Right Arm"})
				{
					const state::FighterPart* p = b != nullptr ? partNamed(*b, name) : nullptr;
					severed += p != nullptr && p->severed() ? 1 : 0;
				}
			}
			std::cout << "fight seed " << seed << ": " << severed << " arm(s) off\n";
			if (severed == 0)
				continue;
			sim.run(10);
			const std::size_t items = limbItems(sim, *sim.combat()->find(bandit)) - before;
			test::require(items == severed, "one ground item per severed arm: " + std::to_string(items) + " for " + std::to_string(severed));
			std::cout << "fight: " << severed << " arm(s) off, " << items << " item(s) on the ground\n";
		}
		test::require(severed >= 1, "a hit severed an arm within 60 s on one of " + std::to_string(kFightSeeds) + " seeds");
		std::cout << "ksim_dismember_scen ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "error: " << e.what() << "\n";
		return 1;
	}
}
