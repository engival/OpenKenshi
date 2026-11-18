// Gate crews (docs/reference/GATES_CARRY_LIVE.md a, GRADES_GATESQUADS_KO_LIVE.md b, DOORS.md section 8):
//
//  1. spawning (the real data): a town's `residents` entry whose SQUAD_TEMPLATE `building` list names a free gate of the town
//     (the gate's town is the nearest town centre, no cutoff) homes one squad per bound gate; the squad spawns on the outside
//     point (centre + 15 n) of the gate's first door and carries a GateCrew whose post is that point (+-0.5 u); the member slots
//     are outside + 33 n at -34, -19, +19, +34; a gate never gets a template that does not list
//     it, and never two squads; the counts per gate-guard package are printed;
//  2. the squad layer (a straight-line navigator): every crew walks to its post and holds it for ever (goal ManGate, never
//     goal-less); a shut gate is reopened by its crew after UNLOCK (3.33 s, only the door's faction) + OPEN (0.8 / `door move
//     speed`); a gate with no crew, or a crew of another faction at a locked gate, stays shut; nothing closes a gate (the owner
//     loses a town and defends: the book does not change);
//  3. (argv[1] = the Stack navmesh cache; the Hub's gates are ruins, no doors) the full-detail leader and guards on Stack's gate: it walks to the outside point, a
//     locked gate is unlocked then opened by it within seconds, a gate shut again is reopened, it never closes it, a leader of
//     another faction cannot unlock, and a leader in a fight leaves the gate as it is;
//  4. the binding (GATE_CREWS_LIVE.md section 5): a crew's home gate is its gate at spawn; a stale own gate falls back to the home
//     building's gate; a removed gate falls back to the town's nearest free gateway; a duplicate is never made (the second crew
//     takes the next free gateway or stands down); a town with no gate left keeps its squads as ordinary residents; save, load
//     and continue give the same hash.

#include "ksim/ai.hpp"
#include "ksim/behaviours.hpp"
#include "ksim/clock.hpp"
#include "ksim/combat.hpp"
#include "ksim/doors.hpp"
#include "ksim/factions.hpp"
#include "ksim/gatebind.hpp"
#include "ksim/gatecrew.hpp"
#include "ksim/interact.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simlink_scenario.hpp"
#include "ksim/simulation.hpp"

#include "knav/walk.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_AI_TABLE
#error KSIM_AI_TABLE must be defined
#endif

namespace {

using namespace ksim;

// Straight lines everywhere; keeps the last door book it was given.
class FakeNavigator final : public Navigator
{
public:
	bool covers(double, double) const override { return true; }

	Route route(double fromX, double fromZ, double toX, double toZ) const override
	{
		Route route;
		route.found = true;
		route.points = {{fromX, fromZ}, {toX, toZ}};
		return route;
	}

	void applyDoors(const knav::DoorBook& book) const override { book_ = book; }
	const knav::DoorBook& book() const { return book_; }

private:
	mutable knav::DoorBook book_;
};

flecs::entity entityOf(const Simulation& simulation, flecs::entity_t id)
{
	return flecs::entity(simulation.world().ecs(), id);
}

struct Crew
{
	flecs::entity_t entity = 0;
	std::uint64_t id = 0;
	state::GateCrew crew;
	state::SquadInstance squad;
	state::Position position;
};

std::vector<Crew> crewsOf(const Simulation& simulation)
{
	std::vector<Crew> crews;
	simulation.world().ecs().each([&](flecs::entity entity, const state::Id& id, const state::GateCrew& crew,
		const state::SquadInstance& squad, const state::Position& position)
	{
		crews.push_back(Crew{entity.id(), id.value, crew, squad, position});
	});
	std::sort(crews.begin(), crews.end(), [](const Crew& a, const Crew& b) { return a.id < b.id; });
	return crews;
}

const GateSite* siteOf(const Simulation& simulation, std::uint64_t key)
{
	for (const GateSite& site : simulation.world().gateSites())
		if (site.key == key)
			return &site;
	return nullptr;
}

bool lists(const kdata::Record& squad, const std::string& type)
{
	const kdata::RefCategory* refs = squad.findRefs("building");
	return refs != nullptr && std::any_of(refs->entries.begin(), refs->entries.end(),
		[&](const kdata::RefEntry& entry) { return !entry.opaqueEntry && entry.targetID == type; });
}

Simulation newGame(const kworld::LoadOrder& order, int workers, SpawnReport* report = nullptr)
{
	SpawnOptions options;
	options.seed = 5;
	options.wildernessByDensity = false;
	RegionConfig config;
	config.workerThreads = workers;
	return Simulation::newGame(KENSHI_DATA, order, options, report, config);
}

void enable(Simulation& simulation, std::shared_ptr<const Navigator> navigator)
{
	AiSystem::Config config;
	config.registry = std::make_shared<BehaviourRegistry>(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
	config.navigator = std::move(navigator);
	config.factions = FactionBook::build(simulation.gameData());
	config.factionTuning.raidFrequency = 0.0;
	simulation.enableAi(std::move(config));
}

constexpr std::uint64_t kSecond = 20; // ticks

void runSeconds(Simulation& simulation, double seconds)
{
	simulation.run(static_cast<std::uint64_t>(seconds / kTickSeconds));
}

bool shut(const knav::DoorBook& book, std::uint64_t key)
{
	return book.get(key).state != knav::DoorState::Open;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);

		// ---- 1. spawning ----
		SpawnReport report;
		Simulation simulation = newGame(order, 1, &report);
		const std::shared_ptr<const kdata::GameData> gd = simulation.gameData();
		// Ruins have no doors (7d6ac14, as the original; user-checked on the Hub 2026-11-04): 112 gate sites, was 125+.
		test::require(simulation.world().gateSites().size() >= 105, "too few gate sites: " + std::to_string(simulation.world().gateSites().size()));
		for (const GateSite& site : simulation.world().gateSites())
		{
			const double length = std::hypot(site.nx, site.nz);
			test::require(std::fabs(length - 1.0) < 1e-9, "the gate normal is not a unit vector");
			// 15 either side of the centre: the outside point is the leader's stand point and the squad's spawn point.
			test::require(std::fabs(std::hypot(site.outX - site.x, site.outZ - site.z) - 15.0) < 1e-6
				&& std::fabs(std::hypot(site.inX - site.x, site.inZ - site.z) - 15.0) < 1e-6,
				"a gate's inside and outside points are not 15 from its door");
			test::require((site.outX - site.x) * site.nx + (site.outZ - site.z) * site.nz > 14.9, "the outside point is not outside");
			// The member slots (GATE_CREWS_LIVE.md section 2): outside + 33 n, across -34, -19, +19, +34, all on the plane.
			for (int slot = 1; slot <= kGateMemberSlots; ++slot)
			{
				const GateStand stand = *gateStandPoint(site, slot);
				const double along = (stand.x - site.outX) * site.nx + (stand.z - site.outZ) * site.nz;
				const double across = -(stand.x - site.outX) * site.nz + (stand.z - site.outZ) * site.nx;
				test::require(std::fabs(along - 33.0) < 1e-6 && std::fabs(across - kGateSlotAcross[static_cast<std::size_t>(slot - 1)]) < 1e-6,
					"a member slot is off its place");
			}
			test::require(!gateStandPoint(site, kGateMemberSlots + 1).has_value(), "a fifth member has a slot");
		}
		const std::vector<Crew> crews = crewsOf(simulation);
		test::require(crews.size() >= 20, "too few gate crews: " + std::to_string(crews.size()));
		std::set<std::uint64_t> gates;
		std::map<std::string, std::size_t> byTemplate;
		std::map<std::string, std::map<std::string, std::size_t>> byTown;
		std::size_t outward = 0;
		std::size_t beyondOldReach = 0;
		for (const Crew& crew : crews)
		{
			if (gates.count(crew.crew.gate) != 0)
				for (const Crew& other : crews)
					if (other.crew.gate == crew.crew.gate)
						std::cerr << "  crew " << other.id << " " << other.squad.templateSid << " " << gd->find(other.squad.templateSid)->name << " town " << other.squad.homeTown.id << " faction " << other.squad.faction.id << "\n";
			test::require(gates.insert(crew.crew.gate).second, "a gate has two crews: " + crew.squad.templateSid + " id " + std::to_string(crew.id) + " ent " + std::to_string(crew.entity) + " at " + std::to_string(crew.position.x) + " " + std::to_string(crew.position.z));
			const GateSite* site = siteOf(simulation, crew.crew.gate);
			test::require(site != nullptr, "a crew's gate has no site");
			test::require(std::hypot(crew.position.x - site->outX, crew.position.z - site->outZ) < 0.5,
				"a crew did not spawn on the outside point");
			test::require(std::hypot(crew.crew.postX - site->outX, crew.crew.postZ - site->outZ) < 0.5, "a crew's post is not the outside point");
			test::require(crew.squad.source == state::SpawnSource::TownResident && crew.squad.homeTown.id != 0, "a crew is not a town resident");
			const kdata::Record* squad = gd->find(crew.squad.templateSid);
			test::require(squad != nullptr && lists(*squad, site->buildingSid),
				"a gate got a template that does not list its building type: " + crew.squad.templateSid);
			++byTemplate[squad->name];
			const state::TownState* town = entityOf(simulation, crew.squad.homeTown.id).get<state::TownState>();
			++byTown[gd->find(town->activeSid)->name + " (" + gd->find(site->buildingSid)->name + ")"][squad->name];
			const double facing = (site->x - town->x) * site->nx + (site->z - town->z) * site->nz;
			outward += facing > 0.0 ? 1 : 0;
			// The old binding stopped at 1,500 u; the crews whose gate is further than that from its town exist only because it is gone.
			const double townAway = std::hypot(site->x - town->x, site->z - town->z);
			if (townAway > 1500.0)
			{
				++beyondOldReach;
				std::cout << "far gate: " << gd->find(town->activeSid)->name << " (" << gd->find(site->buildingSid)->name << ") " << townAway
					<< " u from its town, centre " << site->x << " " << site->z << "\n";
			}
			// KGATES_LIST=1: each crew's gate in world coordinates, for live checks (kinspect item 31).
			if (std::getenv("KGATES_LIST") != nullptr)
				std::cout << "gate " << gd->find(town->activeSid)->name << " | " << gd->find(site->buildingSid)->name << " | centre "
					<< site->x << " " << site->z << " | n " << site->nx << " " << site->nz << " | town " << town->x << " " << town->z
					<< " | " << (facing > 0.0 ? "away" : "INTO TOWN") << " | " << squad->name << "\n";
		}
		// Which side is outside is the model's authored orientation (UNSURE, world.cpp gateNormal): the +X column of the placement
		// faces away from the town centre for most gates (34 of 43 here; the others are far gates whose nearest town is a guess,
		// and a few small towns where the column points in: OPEN). A sanity bound well above a coin toss, not a measurement.
		test::require(outward * 10 >= crews.size() * 6, "the outside point faces the town for " + std::to_string(crews.size() - outward) + " of "
			+ std::to_string(crews.size()) + " crews");
		// A gate-guard template's resident entries: every one is either a crew or counted as not housed.
		std::size_t gatePackageSquads = 0;
		{
			const BehaviourRegistry registry(simulation.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
			simulation.world().ecs().each([&](const state::SquadInstance& squad)
			{
				gatePackageSquads += registry.squad(squad.templateSid).behaviour == Behaviour::ManTheGate ? 1 : 0;
			});
		}
		std::cout << "spawn: " << simulation.world().gateSites().size() << " gates, " << crews.size() << " crews on distinct gates, "
			<< outward << " facing away from their town (the gate->town binding has no cutoff: gates " << simulation.world().gateSites().size()
			<< ", every gate bound to its nearest town; " << crews.size() << " crews, one per gate, " << beyondOldReach
			<< " of them further than the old 1,500 u cutoff); " << report.gateSquads << " spawned by the gate rule, " << report.gateSquadsUnhoused
			<< " resident entries of a gate template found no free gate in their town; " << gatePackageSquads << " squads run a Gate guard package\n";
		for (const auto& [name, count] : byTemplate)
			std::cout << "  " << count << " x " << name << "\n";
		for (const auto& [town, templates] : byTown)
			for (const auto& [name, count] : templates)
				std::cout << "  town " << town << ": " << count << " x " << name << "\n";

		// ---- 2. the squad layer ----
		auto fake = std::make_shared<FakeNavigator>();
		enable(simulation, fake);
		runSeconds(simulation, 120.0);
		const DoorSystem& doors = *simulation.ai()->doorSystem();
		test::require(doors.stats().gatesCrewed == crews.size(), "gatesCrewed " + std::to_string(doors.stats().gatesCrewed));
		for (const Crew& crew : crews)
		{
			const flecs::entity squad = entityOf(simulation, crew.entity);
			const state::Position* at = squad.get<state::Position>();
			test::require(std::hypot(at->x - crew.crew.postX, at->z - crew.crew.postZ) < 1e-6, "a crew is not at its post after 120 s");
			test::require(squad.get<state::GroupAI>()->goal == state::GoalKind::ManGate, "a crew does not man its gate");
		}
		const knav::DoorBook calm = fake->book();
		const AiAudit audit = simulation.ai()->audit(simulation.world().ecs());
		test::require(audit.goalless == 0, "a squad has no goal");
		test::require(audit.byGoal[static_cast<std::size_t>(state::GoalKind::ManGate)] == crews.size(), "the ManGate count");
		test::require(audit.offRoute == 0, "a squad is off its route");

		// A crewed gate whose crew belongs to the faction that owns the door (UNLOCK is planned only by the door's faction).
		const DoorData data = *DoorData::fromWorld(simulation.world());
		const auto siteData = [&](std::uint64_t key) -> const DoorSite&
		{
			for (const DoorSite& site : data.sites)
				if (site.key == key)
					return site;
			throw std::runtime_error("no door site");
		};
		// UNLOCK is planned only by the door's faction (isObjectMine): the gate placement's `owner faction ID`. How many crews are
		// of it, and two crews made so for the reopen checks.
		std::map<std::string, flecs::entity_t> factionBySid;
		simulation.world().ecs().each([&](flecs::entity entity, const state::FactionState& faction)
		{
			factionBySid[faction.templateSid] = entity.id();
		});
		const auto factionOf = [&](const Crew& crew)
		{
			return entityOf(simulation, crew.entity).get<state::SquadInstance>()->faction.id;
		};
		std::size_t ofOwner = 0;
		std::vector<const Crew*> picks;
		for (const Crew& crew : crews)
		{
			const std::string& ownerSid = siteData(crew.crew.gate).ownerFaction;
			if (doors.townOf(crew.crew.gate) == 0 || factionBySid.count(ownerSid) == 0)
				continue;
			ofOwner += factionBySid.at(ownerSid) == factionOf(crew) ? 1 : 0;
			if (picks.size() < 2)
				picks.push_back(&crew);
		}
		test::require(picks.size() == 2, "no two crews with a town and a gate owner");
		const Crew* owned = picks[0];
		const Crew* second = picks[1];
		for (const Crew* crew : picks)
		{
			state::SquadInstance squad = *entityOf(simulation, crew->entity).get<state::SquadInstance>();
			squad.faction = state::EntityRef{factionBySid.at(siteData(crew->crew.gate).ownerFaction)};
			entityOf(simulation, crew->entity).set<state::SquadInstance>(squad);
		}
		std::cout << "crews of their gate's owner (the only ones that can unlock it): " << ofOwner << " of " << crews.size() << "\n";
		const double openSeconds = 0.8 / siteData(owned->crew.gate).moveSpeed;
		const double openSeconds2 = 0.8 / siteData(second->crew.gate).moveSpeed;
		// A shut (closed) gate: reopened after OPEN alone.
		simulation.enqueue(Command::setDoorState(owned->crew.gate, 1));
		runSeconds(simulation, 1.5);
		test::require(fake->book().get(owned->crew.gate).state == knav::DoorState::Closed, "the gate was not shut");
		runSeconds(simulation, std::max(openSeconds - 2.5, 0.0));
		test::require(shut(fake->book(), owned->crew.gate), "the crew reopened the gate before OPEN could finish");
		runSeconds(simulation, 4.0);
		test::require(!shut(fake->book(), owned->crew.gate) && fake->book() == calm,
			"the crew did not reopen a closed gate within OPEN + 4 s (" + std::to_string(openSeconds) + " s)");
		// A locked gate: UNLOCK first (3.33 s) then OPEN.
		simulation.enqueue(Command::setDoorState(second->crew.gate, 2));
		runSeconds(simulation, 3.0);
		test::require(fake->book().get(second->crew.gate).state == knav::DoorState::Locked, "the gate was not locked");
		runSeconds(simulation, 3.33 + openSeconds2 + 0.0);
		test::require(!shut(fake->book(), second->crew.gate), "the crew did not unlock and open a locked gate in UNLOCK + OPEN + 3 s");
		// The crew of another faction cannot unlock: it waits.
		const flecs::entity foreign = entityOf(simulation, second->entity);
		state::SquadInstance changed = *foreign.get<state::SquadInstance>();
		flecs::entity_t other = 0;
		simulation.world().ecs().each([&](flecs::entity faction, const state::FactionState&)
		{
			if (other == 0 && faction.id() != changed.faction.id)
				other = faction.id();
		});
		changed.faction = state::EntityRef{other};
		foreign.set<state::SquadInstance>(changed);
		simulation.enqueue(Command::setDoorState(second->crew.gate, 2));
		runSeconds(simulation, 60.0);
		test::require(fake->book().get(second->crew.gate).state == knav::DoorState::Locked, "a crew of another faction unlocked the door");
		simulation.enqueue(Command::setDoorState(second->crew.gate, 1));
		runSeconds(simulation, openSeconds2 + 3.0 + 2.0);
		test::require(!shut(fake->book(), second->crew.gate), "the other faction's crew could not open an unlocked gate");
		simulation.enqueue(Command::setDoorState(second->crew.gate, -1));
		// A gate without a crew stays as it was left.
		std::uint64_t uncrewed = 0;
		for (const DoorSite& site : data.sites)
			if (site.gate && doors.townOf(site.key) != 0 && gates.count(site.key) == 0 && uncrewed == 0)
				uncrewed = site.key;
		test::require(uncrewed != 0, "every gate has a crew");
		simulation.enqueue(Command::setDoorState(uncrewed, 1));
		runSeconds(simulation, 120.0);
		test::require(fake->book().get(uncrewed).state == knav::DoorState::Closed, "a gate with no crew was reopened by somebody");
		simulation.enqueue(Command::setDoorState(uncrewed, -1));
		runSeconds(simulation, 2.0);
		test::require(fake->book() == calm, "the book did not return to calm");
		std::cout << "reopen: closed gate " << openSeconds << " s, locked gate +3.33 s; foreign crew and no crew leave it shut; "
			<< doors.stats().crewReopens << " reopens\n";

		// Nothing closes a gate: the owner of a crewed gate loses a town (it defends), a hostile raid may come; the book is calm.
		{
			const flecs::entity_t owner = entityOf(simulation, doors.townOf(owned->crew.gate)).get<state::TownState>()->owner.id;
			std::uint64_t spare = 0;
			simulation.world().ecs().each([&](flecs::entity, const state::Id& id, const state::TownState& town)
			{
				if (town.owner.id == owner && spare == 0 && id.value != entityOf(simulation, doors.townOf(owned->crew.gate)).get<state::Id>()->value)
					spare = id.value;
			});
			flecs::entity_t hostile = 0;
			simulation.world().ecs().each([&](flecs::entity faction, const state::FactionState&)
			{
				if (hostile == 0 && doors.classOf(faction.id(), owner) == DoorClass::Hostile)
					hostile = faction.id();
			});
			test::require(spare != 0 && hostile != 0, "no spare town or hostile faction");
			simulation.enqueue(Command::setTownOwner(state::Id{spare}, state::Id{entityOf(simulation, hostile).get<state::Id>()->value}));
			simulation.run(kTicksPerGameHour * 2);
			test::require(entityOf(simulation, owner).get<state::FactionAI>()->intent == state::Intent::Defend, "the owner does not defend");
			simulation.run(Simulation::kSquadIntervalTicks + 1);
			test::require(fake->book() == calm, "a gate was closed by an alert");
			std::cout << "no alert closing: the owner defends, " << doors.stats().gates << " gates still as the data has them\n";
		}
		// The crews hold their posts through days.
		simulation.run(3 * static_cast<std::uint64_t>(kTicksPerGameDay));
		for (const Crew& crew : crews)
		{
			const flecs::entity squad = entityOf(simulation, crew.entity);
			test::require(squad.get<state::GroupAI>()->goal == state::GoalKind::ManGate, "a crew left its goal");
			const state::Position* at = squad.get<state::Position>();
			test::require(std::hypot(at->x - crew.crew.postX, at->z - crew.crew.postZ) < 1e-6, "a crew left its post");
		}

		// ---- 2b. the binding ----
		{
			for (const Crew& crew : crews)
				test::require(crew.crew.home == crew.crew.gate && crew.crew.home != 0, "a crew's home gate is not its gate at spawn");
			// The gate -> town rule again (nearest centre, first in Id order on a tie), to know each town's gateways.
			std::vector<std::pair<std::uint64_t, flecs::entity_t>> townIds;
			std::vector<std::pair<double, double>> centres;
			{
				std::vector<std::tuple<std::uint64_t, flecs::entity_t, double, double>> towns;
				simulation.world().ecs().each([&](flecs::entity entity, const state::Id& id, const state::TownState& town)
				{
					towns.emplace_back(id.value, entity.id(), town.x, town.z);
				});
				std::sort(towns.begin(), towns.end());
				for (const auto& [id, entity, x, z] : towns)
				{
					townIds.emplace_back(id, entity);
					centres.emplace_back(x, z);
				}
			}
			std::map<flecs::entity_t, std::vector<std::uint64_t>> gatewaysOf;
			for (const GateSite& site : simulation.world().gateSites())
			{
				const std::size_t town = nearestTownIndex(site.x, site.z, centres.size(), [&](std::size_t i) { return centres[i]; });
				if (town != centres.size())
					gatewaysOf[townIds[town].second].push_back(site.key);
			}
			const auto squadOf = [&](const Crew& crew) { return entityOf(simulation, crew.entity); };
			const auto gateOf = [&](const Crew& crew) -> const state::GateCrew* { return squadOf(crew).get<state::GateCrew>(); };
			const auto allDistinct = [&](const char* when)
			{
				std::set<std::uint64_t> held;
				for (const Crew& crew : crews)
					if (const state::GateCrew* bound = gateOf(crew))
						test::require(held.insert(bound->gate).second, std::string("two crews on one gate ") + when);
			};
			const auto townOfCrew = [&](const Crew& crew) { return squadOf(crew).get<state::SquadInstance>()->homeTown.id; };
			const auto crewedKeys = [&]
			{
				std::set<std::uint64_t> held;
				for (const Crew& crew : crews)
					if (const state::GateCrew* bound = gateOf(crew))
						held.insert(bound->gate);
				return held;
			};
			// Towns that have a crew, and what is free in each.
			std::map<flecs::entity_t, std::vector<const Crew*>> crewsOfTown;
			for (const Crew& crew : crews)
				crewsOfTown[townOfCrew(crew)].push_back(&crew);
			const std::set<std::uint64_t> held0 = crewedKeys();
			const auto freeOf = [&](flecs::entity_t town, const std::set<std::uint64_t>& held)
			{
				std::vector<std::uint64_t> freeKeys;
				for (const std::uint64_t key : gatewaysOf[town])
					if (held.count(key) == 0)
						freeKeys.push_back(key);
				return freeKeys;
			};

			// (a) a stale own gate (task+0x10 pointing at nothing) falls back to the home building's gate.
			const Crew& homeCrew = crews[0];
			{
				state::GateCrew stale = *gateOf(homeCrew);
				stale.gate = 0xdeadbeefULL;
				squadOf(homeCrew).set<state::GateCrew>(stale);
				runSeconds(simulation, 1.5);
				test::require(gateOf(homeCrew)->gate == homeCrew.crew.home && gateOf(homeCrew)->home == homeCrew.crew.home, "a stale own gate did not fall back to the home gate");
				const GateSite& site = *siteOf(simulation, homeCrew.crew.home);
				test::require(std::hypot(gateOf(homeCrew)->postX - site.outX, gateOf(homeCrew)->postZ - site.outZ) < 1e-9, "the rebound post is not the outside point");
				allDistinct("after the home fallback");
			}
			// (b) the gate is removed (own and home both gone): the town's nearest free gateway, nobody doubled. The crew is the one with the
			// nearest free gateway of all, so the walk to its new post fits the 120 s below (a town's gateways bind with no cutoff: one can be
			// 9 k u off).
			const Crew* townCrew = nullptr;
			double townCrewFar = 1e300;
			for (const Crew& crew : crews)
			{
				if (&crew == &homeCrew)
					continue;
				const state::Position at = *squadOf(crew).get<state::Position>();
				for (const std::uint64_t key : freeOf(townOfCrew(crew), held0))
				{
					const GateSite& site = *siteOf(simulation, key);
					const double far = std::hypot(site.outX - at.x, site.outZ - at.z);
					if (far < townCrewFar)
					{
						townCrewFar = far;
						townCrew = &crew;
					}
				}
			}
			test::require(townCrew != nullptr, "no crew has a free gateway in its town");
			{
				const std::vector<std::uint64_t> freeKeys = freeOf(townOfCrew(*townCrew), held0);
				const state::Position at = *squadOf(*townCrew).get<state::Position>();
				std::uint64_t expect = 0;
				double best = 1e300;
				for (const std::uint64_t key : freeKeys)
				{
					const GateSite& site = *siteOf(simulation, key);
					const double far = std::hypot(site.outX - at.x, site.outZ - at.z);
					if (far < best)
					{
						best = far;
						expect = key;
					}
				}
				simulation.removeGate(townCrew->crew.gate);
				runSeconds(simulation, 1.5);
				const state::GateCrew* bound = gateOf(*townCrew);
				test::require(bound != nullptr && bound->gate == expect && bound->home == townCrew->crew.home,
					"a removed gate did not fall back to the town's nearest free gateway");
				test::require(expect != townCrew->crew.gate && std::find(gatewaysOf[townOfCrew(*townCrew)].begin(), gatewaysOf[townOfCrew(*townCrew)].end(), expect) != gatewaysOf[townOfCrew(*townCrew)].end(),
					"the fallback gate is not a gateway of the town");
				allDistinct("after the town fallback");
				std::cout << "binding: stale own gate -> home gate; removed gate -> the town's nearest free gateway (" << freeKeys.size() << " free)\n";
			}
			// (c) the fallback would double a gate: the home gate is held by a crew of the same town, and no gate of the town is
			// free but one; the second crew takes it, and the third stands down.
			{
				const Crew* first = nullptr;
				for (const auto& [town, list] : crewsOfTown)
					if (first == nullptr && list.size() >= 2 && town != townOfCrew(*townCrew) && town != townOfCrew(homeCrew))
						first = list[0];
				if (first != nullptr)
				{
					const flecs::entity_t town = townOfCrew(*first);
					const std::vector<const Crew*>& list = crewsOfTown[town];
					const std::set<std::uint64_t> held = crewedKeys();
					const std::vector<std::uint64_t> freeKeys = freeOf(town, held);
					// Both of the first two crews lose their gate; each has the other's gate as its home.
					const Crew& a = *list[0];
					const Crew& b = *list[1];
					for (const Crew* crew : {&a, &b})
					{
						state::GateCrew bound = *gateOf(*crew);
						simulation.removeGate(bound.gate);
					}
					state::GateCrew boundA = *gateOf(a);
					state::GateCrew boundB = *gateOf(b);
					boundA.home = b.crew.gate;
					boundB.home = a.crew.gate; // both removed: no home either
					squadOf(a).set<state::GateCrew>(boundA);
					squadOf(b).set<state::GateCrew>(boundB);
					runSeconds(simulation, 1.5);
					allDistinct("after two crews lost their gates");
					const state::GateCrew* ga = gateOf(a);
					const state::GateCrew* gb = gateOf(b);
					std::size_t bound = (ga != nullptr ? 1 : 0) + (gb != nullptr ? 1 : 0);
					test::require(bound == std::min<std::size_t>(2, freeKeys.size()), "the crews that lost their gates did not fill the free gateways first-come: "
						+ std::to_string(bound) + " bound, " + std::to_string(freeKeys.size()) + " free");
					for (const state::GateCrew* g : {ga, gb})
						if (g != nullptr)
							test::require(std::find(freeKeys.begin(), freeKeys.end(), g->gate) != freeKeys.end(), "a crew took a gate that was not free");
					std::cout << "binding: two crews lost their gates in a town with " << freeKeys.size() << " free gateway(s): " << bound << " rebound, "
						<< 2 - bound << " stood down; no gate doubled\n";
				}
			}
			// (d) a duplicate in the data (two crews on one gate) is undone: the higher Id rebinds.
			{
				const Crew* v = nullptr;
				const Crew* w = nullptr;
				for (const Crew& crew : crews)
				{
					if (!gateOf(crew) || townOfCrew(crew) == townOfCrew(*townCrew) || &crew == &homeCrew)
						continue;
					if (v == nullptr)
						v = &crew;
					else if (w == nullptr && gateOf(crew)->gate != gateOf(*v)->gate && crew.crew.home == gateOf(crew)->gate)
						w = &crew;
				}
				test::require(v != nullptr && w != nullptr, "no two bound crews for the duplicate check");
				if (v->id > w->id)
					std::swap(v, w);
				const std::uint64_t shared = gateOf(*v)->gate;
				const std::uint64_t wHome = gateOf(*w)->home;
				state::GateCrew doubled = *gateOf(*w);
				doubled.gate = shared;
				squadOf(*w).set<state::GateCrew>(doubled);
				runSeconds(simulation, 1.5);
				allDistinct("after a duplicate");
				// The lower Id keeps the shared gate; the other goes to its home gate, which nobody holds.
				test::require(gateOf(*v)->gate == shared && gateOf(*w)->gate == wHome, "a duplicated gate was not undone by the higher Id taking its home gate");
			}
			// (e) every gate of a town is gone: its crews stand down and are ordinary town residents with a goal.
			{
				const flecs::entity_t victim = [&]
				{
					for (const auto& [town, list] : crewsOfTown)
						if (town != townOfCrew(*townCrew) && town != townOfCrew(homeCrew) && !gatewaysOf[town].empty() && gateOf(*list[0]) != nullptr)
							return town;
					return flecs::entity_t(0);
				}();
				test::require(victim != 0, "no town to lose all its gates");
				for (const std::uint64_t key : gatewaysOf[victim])
					simulation.removeGate(key);
				runSeconds(simulation, 1.5);
				std::size_t stood = 0;
				for (const Crew* crew : crewsOfTown[victim])
					stood += gateOf(*crew) == nullptr ? 1 : 0;
				// A crew rebinds to a gateway of another town? No: only its own town's. So all of them stand down.
				test::require(stood == crewsOfTown[victim].size(), "a crew kept a gate in a town with no gate left");
				runSeconds(simulation, 60.0);
				for (const Crew* crew : crewsOfTown[victim])
				{
					const state::GroupAI* ai = squadOf(*crew).get<state::GroupAI>();
					test::require(ai->goal != state::GoalKind::ManGate && ai->goal != state::GoalKind::None, "a stood-down crew is stuck without a goal");
				}
				allDistinct("after a town lost its gates");
				std::cout << "binding: a town with all " << gatewaysOf[victim].size() << " gate(s) gone: " << stood << " crew(s) stand down as ordinary residents\n";
			}
			// The rebound crews walk to their new posts and hold them; nobody is goal-less.
			runSeconds(simulation, 120.0);
			for (const Crew& crew : crews)
			{
				const state::GateCrew* bound = gateOf(crew);
				if (bound == nullptr)
					continue;
				const state::Position* at = squadOf(crew).get<state::Position>();
				test::require(squadOf(crew).get<state::GroupAI>()->goal == state::GoalKind::ManGate, "a bound crew does not man its gate");
				test::require(std::hypot(at->x - bound->postX, at->z - bound->postZ) < 1e-6, "a rebound crew is not at its post");
			}
			test::require(simulation.ai()->audit(simulation.world().ecs()).goalless == 0, "a squad has no goal after the rebinding");
		}

		// ---- 2c. save, load and continue (the binding and the gates that are gone are saved) ----
		{
			const auto build = [&]
			{
				Simulation sim = newGame(order, 1);
				std::vector<std::uint64_t> keys;
				sim.world().ecs().each([&](const state::GateCrew& crew) { keys.push_back(crew.gate); });
				std::sort(keys.begin(), keys.end());
				// Some gates gone; one crew's own gate stale.
				for (std::size_t i = 0; i < keys.size(); i += 4)
					sim.removeGate(keys[i]);
				sim.run(2 * kGateCrewIntervalTicks + 3);
				return sim;
			};
			Simulation straight = build();
			Simulation first = build();
			std::ostringstream saved;
			first.save(saved);
			std::istringstream in(saved.str());
			Simulation loaded = Simulation::restore(World::load(KENSHI_DATA), in, {}, false, nullptr, first.gameData());
			test::require(loaded.stateHash() == first.stateHash(), "the hash changed across save and load");
			std::size_t crewsLeft = 0;
			std::size_t rebound = 0;
			loaded.world().ecs().each([&](const state::GateCrew& crew) { ++crewsLeft; rebound += crew.home != crew.gate ? 1 : 0; });
			test::require(loaded.world().ecs().get<state::GatesGone>() != nullptr, "the gates that are gone were not saved");
			std::size_t gone = loaded.world().ecs().get<state::GatesGone>()->keys.size();
			loaded.run(3 * kGateCrewIntervalTicks);
			straight.run(3 * kGateCrewIntervalTicks);
			test::require(loaded.stateHash() == straight.stateHash(), "state hash after save, load and continue differs");
			std::ostringstream a;
			std::ostringstream b;
			loaded.save(a);
			straight.save(b);
			test::require(a.str() == b.str(), "the saves differ after continuing");
			std::cout << "save: " << gone << " gates gone, " << crewsLeft << " crews left, " << rebound << " rebound; hash identical after load and continue\n";
		}

		// ---- 3. the full-detail leader and guards on Stack's gate ----
		if (argc == 2)
		{
			const auto gameData = simulation.gameData();
			const std::shared_ptr<const CombatData> data = CombatData::build(*gameData, KENSHI_DATA);
			const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);
			Simulation stack = Simulation::load(KENSHI_DATA, 91);
			stack.setGameData(gameData);
			CombatSettings settings;
			settings.substeps = 2;
			stack.enableCombat(data, settings);
			stack.enableNavigation(nav);
			spawnMeshDoors(stack, *nav);
			stack.setClockTick(tickAt(kNewGameDay, kNewGameHour, 0));
			flecs::world& ecs = const_cast<flecs::world&>(stack.world().ecs());
			// The body of the leader, the guards and the foe: the duel's servant (the combat tests' Escaped Servant).
			const kdata::Record* servant = nullptr;
			for (const kdata::Record* r : gameData->byType(kworld::itemTypeByName("CHARACTER")))
				if (r->name == "Escaped Servant")
					servant = r;
			test::require(servant != nullptr, "no Escaped Servant in the data");
			std::uint64_t rng = 12345;
			const state::Fighter servantBody = data->makeFighter(*gameData, servant->stringID, "", rng);
			const auto fighter = [&](std::uint64_t id) -> state::Fighter& { return *const_cast<CombatArena*>(stack.combat())->find(id); };
			// The Stack gate that has a door entity and an outside point on the mesh.
			const GateSite* gate = nullptr;
			flecs::entity doorEntity;
			knav::Position post{};
			for (const GateSite& site : stack.world().gateSites())
			{
				flecs::entity candidate;
				ecs.each([&](flecs::entity e, const state::Door& door) { if (door.key == site.key) candidate = e; });
				if (!candidate.is_valid())
					continue;
				const auto spot = nav->walker().snap({site.outX, candidate.get<state::Door>()->y, site.outZ}, 40.0);
				if (!spot || std::hypot(spot->x - site.outX, spot->z - site.outZ) > 4.0)
					continue;
				gate = &site;
				doorEntity = candidate;
				post = *spot;
				break;
			}
			test::require(gate != nullptr, "no gate of Stack with a door and an outside point on the mesh");
			const auto door = [&]() -> state::Door& { return *doorEntity.get_mut<state::Door>(); };
			test::require(!door().ownerSid.empty(), "the gate of Stack has no owner faction");
			std::cout << "stack gate: " << gameData->find(gate->buildingSid)->name << ", door state " << int(door().state) << ", owner "
				<< gameData->find(door().ownerSid)->name << ", speed " << door().moveSpeed << "\n";

			state::Fighter body = servantBody;
			const auto spawn = nav->walker().snap({gate->outX + 20.0 * gate->nx, door().y, gate->outZ + 20.0 * gate->nz}, 40.0);
			test::require(spawn.has_value(), "the spawn point is off the mesh");
			body.team = 0;
			body.unconscious = false;
			body.koTimer = 0.0;
			body.target = 0;
			body.x = spawn->x;
			body.y = spawn->y;
			body.z = spawn->z;
			flecs::entity leaderEntity = stack.spawnFighter(std::move(body));
			leaderEntity.set<state::Allegiance>(state::Allegiance{door().ownerSid});
			leaderEntity.set<state::GateDuty>(state::GateDuty{gate->key, post.x, post.y, post.z, 0});
			const std::uint64_t leader = leaderEntity.get<state::Id>()->value;
			const auto tick = [&](int n) { for (int i = 0; i < n; ++i) stack.tick(); };
			const auto toPost = [&] { return std::hypot(fighter(leader).x - post.x, fighter(leader).z - post.z); };
			const auto setDoor = [&](std::uint8_t state)
			{
				door().state = state;
				++door().version;
				tick(1);
			};
			// 1. The gate is open: the leader walks from the spawn point to the outside point and stands there.
			setDoor(0);
			int ticks = 0;
			for (; ticks < 20 * 60 && toPost() > kGatePostReach; ++ticks)
				stack.tick();
			test::require(toPost() <= kGatePostReach, "the leader never reached the outside point: " + std::to_string(toPost()) + " away");
			tick(20 * 10);
			test::require(toPost() <= kGatePostReach && door().state == 0 && fighter(leader).building == 0, "the leader left its post or touched an open gate");
			std::cout << "leader: walked " << std::hypot(spawn->x - post.x, spawn->z - post.z) << " units to the outside point in "
				<< ticks * kTickSeconds << " game-s and stands there; the open gate stays open\n";

			// 1b. The guards (full detail): spawned at the post, each walks to its slot (outside + 33 n, across -34, -19, +19, +34)
			// and stands there; a displaced guard walks back; a gate shut or locked is none of its business.
			{
				std::vector<std::uint64_t> guards;
				std::vector<GateStand> slots;
				for (int slot = 1; slot <= kGateMemberSlots; ++slot)
				{
					const GateStand stand = *gateStandPoint(*gate, slot);
					const auto spot = nav->walker().snap({stand.x, door().y, stand.z}, 40.0);
					if (!spot || std::hypot(spot->x - stand.x, spot->z - stand.z) > 4.0)
						continue; // this slot is off the mesh
					state::Fighter guardBody = fighter(leader);
					guardBody.clearRoute();
					flecs::entity guard = stack.spawnFighter(std::move(guardBody));
					guard.set<state::Allegiance>(state::Allegiance{door().ownerSid});
					guard.set<state::GateDuty>(state::GateDuty{gate->key, spot->x, spot->y, spot->z, kGateMemberPhase});
					guards.push_back(guard.get<state::Id>()->value);
					slots.push_back(GateStand{spot->x, spot->z});
				}
				test::require(!guards.empty(), "no member slot of the gate of Stack is on the mesh");
				const auto worst = [&]
				{
					double far = 0.0;
					for (std::size_t g = 0; g < guards.size(); ++g)
						far = std::max(far, std::hypot(fighter(guards[g]).x - slots[g].x, fighter(guards[g]).z - slots[g].z));
					return far;
				};
				int n = 0;
				for (; n < 20 * 90 && worst() > kGatePostReach; ++n)
					stack.tick();
				test::require(worst() <= kGatePostReach, "a guard never reached its slot: " + std::to_string(worst()) + " away");
				fighter(guards[0]).x += 30.0;
				for (int i = 0; i < 20 * 90 && worst() > kGatePostReach; ++i)
					stack.tick();
				test::require(worst() <= kGatePostReach, "a displaced guard did not walk back to its slot");
				test::require(toPost() <= kGatePostReach, "the leader left its post for the guards");
				std::cout << "guards: " << guards.size() << " of " << kGateMemberSlots << " slots on the mesh, all reached in " << n * kTickSeconds
					<< " game-s; a displaced guard walked back\n";
				for (const std::uint64_t guard : guards)
					fighter(guard).dead = true; // out of the way of the rest
			}

			// 2. The gate is locked (its `always locks` re-lock): UNLOCK (3.3 s) then OPEN, within seconds; nothing closes it again.
			const auto reopen = [&](std::uint8_t from, double limit)
			{
				setDoor(from);
				int n = 0;
				bool unlocked = false;
				for (; n < static_cast<int>(limit / kTickSeconds) && door().state != 0; ++n)
				{
					stack.tick();
					unlocked = unlocked || door().state == 1;
				}
				test::require(door().state == 0, "the leader did not open the gate in " + std::to_string(limit) + " s (state " + std::to_string(int(door().state)) + ")");
				test::require(from != 2 || unlocked, "a locked gate went to open without passing closed (the UNLOCK step)");
				return n * kTickSeconds;
			};
			const double lockedSeconds = reopen(2, 40.0);
			test::require(lockedSeconds >= kUnlockSeconds, "UNLOCK took less than " + std::to_string(kUnlockSeconds) + " s");
			tick(20 * 30);
			test::require(door().state == 0, "the crew (or anyone) closed the gate again");
			const double closedSeconds = reopen(1, 40.0);
			tick(20 * 10);
			test::require(toPost() <= kGatePostReach + 1.0 && door().state == 0, "the leader did not return to its post after the reopen");
			std::cout << "leader: locked gate unlocked and opened in " << lockedSeconds << " game-s, a closed one opened in " << closedSeconds
				<< " game-s, back at the post; it stays open\n";

			// 3. A leader of another faction cannot unlock, and waits; unlocked (closed) it opens the gate like anyone.
			const std::string owner = door().ownerSid;
			leaderEntity.set<state::Allegiance>(state::Allegiance{""});
			setDoor(2);
			tick(20 * 40);
			test::require(door().state == 2, "a leader of another faction unlocked the gate");
			setDoor(1);
			tick(20 * 20);
			test::require(door().state == 0, "the other faction's leader did not open a closed gate");
			leaderEntity.set<state::Allegiance>(state::Allegiance{owner});

			// 4. In a fight the gate stays as it is (the leader has a target: goal 144 owns it). A hostile servant of the other team
			// stands beside the post; every second the leader has a target, a shut gate stays shut.
			state::Fighter foe = servantBody;
			foe.team = 1;
			foe.unconscious = false;
			foe.koTimer = 0.0;
			foe.target = 0;
			foe.x = fighter(leader).x + 8.0;
			foe.z = fighter(leader).z;
			foe.y = fighter(leader).y;
			stack.spawnFighter(std::move(foe));
			// The fight starts first, then the gate shuts: a leader without a target yet opens it (step 2), which is not this check.
			for (int i = 0; i < 20 * 10 && fighter(leader).target == 0; ++i)
				stack.tick();
			test::require(fighter(leader).target != 0, "the leader never took the foe as its target");
			setDoor(1);
			int fighting = 0;
			for (int i = 0; i < 20 * 12; ++i)
			{
				stack.tick();
				if (stack.clockTick() % kGateCrewIntervalTicks == 0 && fighter(leader).target != 0)
				{
					++fighting;
					test::require(door().state == 1, "a fighting leader worked the gate (state " + std::to_string(int(door().state)) + ")");
				}
			}
			test::require(fighting >= 3, "the leader never fought (" + std::to_string(fighting) + " samples with a target)");
			std::cout << "leader: " << fighting << " seconds with a target, the shut gate stayed shut\n";
		}
		std::cout << "ksim_gates ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_gates: " << error.what() << "\n";
		return 1;
	}
}
