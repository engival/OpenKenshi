// Task raid-goals (2026-11-08): the two raid goals of the package ladder, driven by LIVE world squads and no NPC order -
//  - 276 LIBERATE_ALL_THE_PRISONERS (kinspect dt24_276, resolver 1408cf140): a squad whose applied package carries 276 sends a member to
//    an occupied cage of its town and frees a captive that is neither its own nor covered by its squad; a captive the liberators' own
//    squad covers stays in its cage.
//  - 270 ASSAULT_FORTIFICATIONS_PREFER_GATES (dt24_270, resolver 1405a3f60): a squad on the passing-town-assault package (59293, which
//    carries 270 in its Squad list; the ladder's own signal tests are not modelled, so the test applies it with `changeAI`) walks to the
//    gate of the town it stands in and bashes it down when the gate is locked (the measured gate bash, DOORS.md section 6).
// The fixture's squad is SQUAD_TEMPLATE 1302 "Dust Bandit squad" (faction 200, packages 96451 and 59293) on the server's new game and
// the navmesh bake. The player is created by its own New Game command and never ordered; the player AREA is moved over the chosen town so
// its squads build at Full detail (`Command::setPlayerAreas`). The bake opened is the Hub's 3x3 (where the start point is picked) plus
// the fixture sites' zones. Needs the bake (argv[1], like the other live fixtures; exit 77 without it).

#include "ksim/ai.hpp"
#include "ksim/blackboard.hpp"
#include "ksim/combat_data.hpp"
#include "ksim/dialogue_events.hpp"
#include "ksim/command.hpp"
#include "ksim/navigator.hpp"
#include "ksim/newgame.hpp"
#include "ksim/routines.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"
#include "ksim/towncircle.hpp"
#include "ksim/world.hpp"

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
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

std::vector<std::pair<std::uint64_t, flecs::entity>> membersOf(Simulation& sim, flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	ecsOf(sim).query_builder<const state::Id>().with<state::MemberOf>(squad).build().each(
		[&](flecs::entity member, const state::Id& id) { found.push_back({id.value, member}); });
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	return found;
}

// "Escaped Servant": the character the combat fixtures build a Fighter from.
std::string servantSid(const kdata::GameData& gd)
{
	for (const kdata::Record* record : gd.byType(kworld::itemTypeByName("CHARACTER")))
		if (record->name == "Escaped Servant")
			return record->stringID;
	return {};
}

// A building whose interior layout holds a cage piece (the site index's own source: indexBuilding -> addLayoutFurniture).
bool hasCageLayout(const kdata::GameData& gd, const routines::LayoutLibrary& layouts, const BuildingSite& site)
{
	std::string layoutName = site.interiorLayout;
	if (layoutName.empty() && !site.squadTemplate.empty())
		if (const kdata::Record* squad = gd.find(site.squadTemplate))
			layoutName = squad->stringOr("layout interior", "");
	const routines::Layout* layout = layoutName.empty() ? nullptr : layouts.find(site.buildingSid + "-" + layoutName);
	for (std::size_t i = 0; layout != nullptr && i < layout->pieces.size(); ++i)
		if (const kdata::Record* piece = gd.find(layout->pieces[i].target))
			if (routines::classifyPiece(gd, *piece) == routines::SiteKind::Cage)
				return true;
	return false;
}

struct Game
{
	std::shared_ptr<kdata::GameData> gd;
	std::shared_ptr<const CombatData> data;
	std::shared_ptr<MeshNavigator> nav;
	std::vector<towncircle::Circle> circles;
	const BuildingSite* cage = nullptr; // the 276 fixture: a cage building in a fixture town
	const GateSite* gate = nullptr; // the 270 fixture: a gate in a fixture town
	Simulation sim;

	explicit Game(const std::string& bakeDir, bool doors = true)
		: gd(std::make_shared<kdata::GameData>(kworld::loadMergedGameData(kworld::vanillaLoadOrder(KENSHI_DATA))))
		, data(CombatData::build(*gd, KENSHI_DATA))
		, sim(Simulation::newGame(KENSHI_DATA, kworld::vanillaLoadOrder(KENSHI_DATA)))
	{
		sim.markPristine(); // the New Game command needs the fresh world mark (SIMLINK order 84)
		// The world data is in before the nav: the fixture sites are chosen from it, then the bake is opened for their zones (and the
		// Hub's 3x3, where the start point is picked).
		circles = live_rig::townCircles(sim, *gd);
		const routines::LayoutLibrary layouts = routines::LayoutLibrary::load(sim.world().dataDir());
		for (const BuildingSite& site : sim.world().buildingSites())
			if (cage == nullptr && live_rig::fixtureTownAt(circles, site.cx, site.cz) && hasCageLayout(*gd, layouts, site))
			{
				cage = &site;
				break;
			}
		for (const GateSite& site : sim.world().gateSites())
			// No gate crew (`squadTemplate` empty): a crew reopens a shut gate and removes its lock, so a locked gate of a crewed town
			// would fall open before the bash ends.
			if (gate == nullptr && site.squadTemplate.empty() && live_rig::fixtureTownAt(circles, site.x, site.z))
				gate = &site;
		test::require(cage != nullptr, "no building with a cage layout stands in a town");
		test::require(gate != nullptr, "no gate stands inside a town circle");
		nav = test::openWorldNav(bakeDir, live_rig::hubZonesWith({{cage->cx, cage->cz}, {gate->x, gate->z}}));
		sim.enableCombat(data, CombatSettings{});
		sim.enableNavigation(nav);
		// The AI system owns the doors (270's gate list) and the fight pass of the goal selector: without it the world has neither.
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = nav;
		ai.data = gd;
		ai.doorTuning.enabled = doors;
		sim.enableAi(std::move(ai));
		// The player (the new-game start): the detail anchor and the "a player exists" test the promotions need. Never ordered.
		live_rig::startWanderer(sim, *gd);
	}

	// The Dust Bandit squad at (x, z), built at Full detail: the player character is put there (the area it carries is the input;
	// it is never ordered), and the squad spawns beside it.
	flecs::entity placeSquad(double x, double z)
	{
		live_rig::standPlayerAt(sim, x, z);
		flecs::entity squad = sim.placeTownSquad("1302-gamedata.base", x, z);
		test::require(squad.is_valid(), "the Dust Bandit squad does not spawn");
		sim.run(300); // the resident pass promotes it (and takes the player area's Full detail)
		std::size_t built = 0;
		for (const auto& [id, entity] : membersOf(sim, squad))
			built += fighterOf(sim, id) != nullptr ? 1 : 0;
		test::require(built > 1, "the placed squad did not build at Full detail");
		return squad;
	}
};

enum class FinderGate { None, Tag, Relation }; // which extra finder gate (rwh.100) the 276 run pins after its control

// 276: a caged captive of another faction is freed; a captive the liberators' own squad covers is not.
void testLiberatePrisoners(Game& game, FinderGate gate)
{
	Simulation& sim = game.sim;
	const std::string servant = servantSid(*game.gd);
	test::require(!servant.empty(), "no Escaped Servant");
	const double cageX = game.cage->cx;
	const double cageZ = game.cage->cz;
	const std::uint64_t cageKey = game.cage->key;
	// The town's owner: the faction of the caged stranger (not the liberators').
	std::uint64_t town = 0;
	ecsOf(sim).each([&](flecs::entity, const state::TownState& state)
	{
		if (town == 0 && std::hypot(state.x - cageX, state.z - cageZ) <= 2500.0 && state.owner.id != 0)
			town = flecs::entity(ecsOf(sim).c_ptr(), state.owner.id).get<state::Id>()->value;
	});
	test::require(town != 0, "the cage's town has no owner faction");
	flecs::entity squad = game.placeSquad(cageX, cageZ);
	// 96451 is the OccupyTown class (signal 30 = 0x1e): it never starts by itself, only after PassingByTownAssault (dt41 F s3), so the
	// squad is put on it as `change AI` would.
	blackboard::Board* board = sim.blackboardOf(squad);
	test::require(board != nullptr, "the placed squad has no blackboard");
	blackboard::Package occupy;
	test::require(blackboard::readPackage(*game.gd, "96451-rebirth.mod", 2, occupy), "no AI package 96451");
	blackboard::changeAI(*board, std::move(occupy));
	// Two captives: a stranger (freed) and one the liberators' own squad covers (left in its cage). Both in the building's first cage
	// slot at its centre.
	std::uint64_t rng = 17;
	auto spawnCaged = [&](std::uint64_t faction, bool serving) -> std::uint64_t
	{
		state::Fighter body = game.data->makeFighter(*game.gd, servant, "", rng);
		body.x = cageX;
		body.z = cageZ;
		body.faction = faction;
		body.squad = 0x51D50000u + faction;
		body.cageBuilding = cageKey;
		body.cageSlot = 0;
		flecs::entity made = sim.spawnFighter(std::move(body));
		if (serving) // still serving a sentence: the law addon's 211 (free the served) leaves it, so only the liberators' 276 frees it
		{
			state::CrimeRecord record;
			record.sentenceStart = 0.0;
			record.sentenceHours = 100000.0;
			made.set<state::CrimeRecord>(std::move(record));
		}
		return made.get<state::Id>()->value;
	};
	const std::uint64_t stranger = spawnCaged(town, true);
	const std::uint64_t squadKey = squad.get<state::Id>()->value;
	const std::uint64_t own = spawnCaged(town, true);
	fighterOf(sim, own)->squad = squadKey; // another faction, but covered by the liberators' squad
	// An unconscious captive: 110's precondition TARGET_IS_CONSCIOUS refuses it (rwh.100), so 276 leaves it in its cage.
	const std::uint64_t sleeper = spawnCaged(town, true);
	fighterOf(sim, sleeper)->unconscious = true;
	sim.setScenarioLocked(cageKey); // a locked cage: the chain has no lock test and 110 clears the lock itself (rwh.100)
	// The crew stands on the captives (the executors' reach tests do the rest).
	for (const auto& [id, entity] : membersOf(sim, squad))
	{
		state::Fighter* f = fighterOf(sim, id);
		if (f == nullptr)
			continue;
		f->x = cageX;
		f->z = cageZ;
		f->y = fighterOf(sim, stranger)->y;
	}
	// The goal: some member takes 276 and the stranger leaves its cage, while the covered captive stays.
	bool taken = false;
	for (int tick = 0; tick < 1200 && fighterOf(sim, stranger)->cageBuilding != 0; ++tick)
	{
		sim.run(1);
		for (const auto& [id, entity] : membersOf(sim, squad))
			if (const state::Routine* routine = entity.get<state::Routine>(); routine != nullptr && routine->goal == routines::task::LiberatePrisoners)
				taken = true;
	}
	test::require(taken, "276: no member of the squad took LIBERATE_ALL_THE_PRISONERS");
	test::require(fighterOf(sim, stranger)->cageBuilding == 0, "276: the cage's foreign captive was not freed");
	test::require(fighterOf(sim, own)->cageBuilding == cageKey, "276: the covered captive was freed");
	test::require(fighterOf(sim, sleeper)->cageBuilding == cageKey, "276: an unconscious captive was freed (110 is refused)");
	test::require(sim.dialogueEventCount(0x2e) >= 1, "276: the free step did not go through 110 (no event 0x2e)");
	test::require(!sim.cageDoorLocked(cageKey), "276: the locked cage was left locked (110 clears the lock)");

	// ---- kenshi-k16.6.130 (rwh.100): the finder's other accept gates. Each runs on a fresh world after the control above (the crew freed the stranger, so it
	// answers a free prisoner), with the cage cleared of the captives above (one occupant per cage: the first found). ----
	if (gate == FinderGate::None)
		return;
	for (const std::uint64_t id : {own, sleeper})
	{
		state::Fighter* gone = fighterOf(sim, id);
		gone->cageBuilding = 0;
		gone->x = cageX + 400.0;
	}
	const std::vector<std::uint64_t> crew = [&]
	{
		std::vector<std::uint64_t> ids;
		for (const auto& [id, entity] : membersOf(sim, squad))
			ids.push_back(id);
		return ids;
	}();
	const std::uint64_t held = spawnCaged(town, true);
	if (gate == FinderGate::Tag)
	{
		// A prisoner the liberators' squad carries a hate tag on (LT_DEFEATED_MY_SQUAD_ONCE, 1 << 7; mask 0x2b0) is skipped.
		sim.enableDialogue(DialogueBook::build(game.gd)); // the long-term tag memory lives in the dialogue system (the rig has none until now)
		for (const std::uint64_t id : crew)
			sim.dialogue()->setMemory(id, held, 1u << 7);
	} else {
		// A cage whose owner's relation to the liberators' faction is >= 50 is skipped.
		const std::uint64_t liberatorFaction = fighterOf(sim, crew.front())->faction;
		flecs::entity_t ownerEntity = 0;
		flecs::entity_t liberatorEntity = 0;
		ecsOf(sim).each([&](flecs::entity entity, const state::FactionState&)
		{
			if (const state::Id* id = entity.get<state::Id>(); id != nullptr && id->value == town)
				ownerEntity = entity.id();
			if (const state::Id* id = entity.get<state::Id>(); id != nullptr && id->value == liberatorFaction)
				liberatorEntity = entity.id();
		});
		test::require(ownerEntity != 0 && liberatorEntity != 0 && ownerEntity != liberatorEntity, "276: the cage owner's and the liberators' factions were not found");
		sim.importRelation({ownerEntity}, {liberatorEntity}, 60.0);
	}
	for (const auto& [mate, entity] : membersOf(sim, squad))
		if (state::Fighter* f = fighterOf(sim, mate))
		{
			f->x = cageX;
			f->z = cageZ;
			f->y = fighterOf(sim, held)->y;
		}
	for (int tick = 0; tick < 800; ++tick)
		sim.run(1);
	test::require(fighterOf(sim, held)->cageBuilding == cageKey,
		gate == FinderGate::Tag ? "276: a prisoner my squad holds a 0x2b0 tag on was freed" : "276: a cage of an owner allied (relation 60) to the liberators was opened");
}

// 270: on the passing-town-assault package the squad breaks the locked gate of the town it stands in. With no door system (--no-doors;
// kenshi-k16.6.66) there is no lock to read, but every gate is still a fortification: the member takes the goal.
void testAssaultGate(Game& game, bool doors = true)
{
	Simulation& sim = game.sim;
	const std::uint64_t gateKey = game.gate->key;
	const double gateX = game.gate->x;
	const double gateZ = game.gate->z;
	flecs::entity squad = game.placeSquad(gateX, gateZ);
	// The ladder applies the highest priority (96451 "hostile occupy town", which has no 270); the assault half is 59293 at priority 1.
	// Its subclasses' should-start tests are not modelled, so the test applies it as `change AI` would (UNSURE).
	blackboard::Board* board = game.sim.blackboardOf(squad);
	test::require(board != nullptr, "the placed squad has no blackboard");
	blackboard::Package assault;
	test::require(blackboard::readPackage(*game.gd, "59293-rebirth.mod", 1, assault), "no AI package 59293");
	blackboard::changeAI(*board, std::move(assault));
	// The gate is locked (a command: the gates stand open by default), and the crew stands on it. The fixture's gate has no guard:
	// its gate crew (a promoted town gate squad) would reopen a shut gate and drop the lock - remove it first.
	{
		std::vector<std::uint64_t> crewSquads;
		ecsOf(sim).each([&](flecs::entity, const state::Id& id, const state::GateCrew& crew)
		{
			if (crew.gate == gateKey)
				crewSquads.push_back(id.value);
		});
		for (const std::uint64_t crewSquad : crewSquads)
			sim.enqueue(Command::removeSquad(state::Id{crewSquad}));
		sim.run(2);
	}
	sim.enqueue(Command::setDoorState(gateKey, 2));
	sim.run(2);
	for (const auto& [id, entity] : membersOf(sim, squad))
	{
		state::Fighter* f = fighterOf(sim, id);
		if (f == nullptr)
			continue;
		f->x = gateX;
		f->z = gateZ;
	}
	// The gate's saved override state (-1 none; 3 = broken, the bash's end).
	const auto gateState = [&]() -> int
	{
		const state::DoorOverrides* overrides = ecsOf(sim).get<state::DoorOverrides>();
		if (overrides == nullptr)
			return -1;
		const auto at = std::lower_bound(overrides->entries.begin(), overrides->entries.end(), gateKey,
			[](const state::DoorOverride& entry, std::uint64_t key) { return entry.key < key; });
		return at != overrides->entries.end() && at->key == gateKey ? static_cast<int>(at->state) : -1;
	};
	// The goal: a member takes 270, and the gate ends broken (the door override state 3).
	bool taken = false;
	for (int tick = 0; tick < 4000 && gateState() != 3 && (doors || !taken); ++tick)
	{
		sim.run(1);
		for (const auto& [id, entity] : membersOf(sim, squad))
			if (const state::Routine* routine = entity.get<state::Routine>(); routine != nullptr && routine->goal == routines::task::AssaultFortifications)
				taken = true;
	}
	test::require(taken, doors ? "270: no member took ASSAULT_FORTIFICATIONS_PREFER_GATES" : "270 without a door system: no member took the goal");
	if (!doors)
		return;
	test::require(gateState() == 3, "270: the locked gate was not bashed down");
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !std::filesystem::exists(argv[1]))
		{
			std::cout << "ksim_raid_goals: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		Game game(argv[1]);
		testLiberatePrisoners(game, FinderGate::None);
		{
			Game tagged(argv[1]);
			testLiberatePrisoners(tagged, FinderGate::Tag);
		}
		{
			Game allied(argv[1]);
			testLiberatePrisoners(allied, FinderGate::Relation);
		}
		testAssaultGate(game);
		Game bare(argv[1], false);
		test::require(bare.sim.ai()->doorSystem() == nullptr || !bare.sim.ai()->doorSystem()->tuning().enabled, "the no-doors game has live doors");
		testAssaultGate(bare, false);
	} catch (const std::exception& e) {
		std::cerr << "FAIL: " << e.what() << "\n";
		return 1;
	}
	std::cout << "ksim_raid_goals OK\n";
	return 0;
}
