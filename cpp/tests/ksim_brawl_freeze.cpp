// The frozen bar brawl (user playtest 2026-11-06, task AR): a Hub new game as `ksim_server --scenario newgame` runs it (seed 1, the world
// bake's streaming navmesh and squad AI), three debug-spawned Dust Bandit squads into the Storm House, then the fight. After a while ~20
// hostiles stood frozen, weapons out, some mid-swing, while 1-2 moved. This test runs the same scenario in process and reports, per
// fighter, which layer leaves it standing: off the mesh, a swing the arena no longer advances, a Wait with a live target, or an overloaded
// crawl. In 10 game hours the brawl does not start on its own: the members are built inside the bar (building set), the raid order walks
// them out before anyone notices, outdoors the building gate blinds them to the bar staff, and when they return the join rules refuse both
// ways (the bar's faction is not hostile to the bandits'). Needs the world bake (argv[1], like ksim_bar_fight; exit 77 without).
#include "ksim/ai/ai.hpp"
#include "ksim/ai/behaviours.hpp"
#include "ksim/combat/combat.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/core/command.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/world/join_rules.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"
#include "knav/navmesh.hpp"
#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"
#include "test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
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

// The Storm House's Bar blackmarket placement 3384 (the bar of BAR_FIGHT_LOOP; TOWN_LIFE_SLICE 5.17).
constexpr double kBarX = -50592.0;
constexpr double kBarZ = -1211.0;
constexpr std::size_t kSquads = 3;
constexpr const char* kTemplate = "1302-gamedata.base";

std::ostream& say()
{
	static std::ostream nowhere(nullptr);
	return std::getenv("KSIM_BRAWL_VERBOSE") != nullptr ? std::cout : nowhere;
}

std::string stateName(state::CombatState state)
{
	switch (state)
	{
	case state::CombatState::Attack: return "Attack";
	case state::CombatState::Defend: return "Defend";
	case state::CombatState::Between: return "Between";
	case state::CombatState::Hub: return "Hub";
	case state::CombatState::Circle: return "Circle";
	case state::CombatState::Wait: return "Wait";
	case state::CombatState::Stumble: return "Stumble";
	case state::CombatState::Done: return "Done";
	case state::CombatState::Approach: return "Approach";
	}
	return "?";
}

// `DebugSpawn::squadId` is the state::Id value, not the flecs entity id.
flecs::entity squadEntityOf(Simulation& sim, std::uint64_t value)
{
	flecs::entity found;
	ecsOf(sim).each([&](flecs::entity entity, const state::Id& id, const state::SquadInstance&)
	{
		if (id.value == value)
			found = entity;
	});
	return found;
}

// The one fighter's last change, sampled every kSampleTicks.
struct Track
{
	double x = 0.0;
	double z = 0.0;
	double clip = 0.0;
	state::CombatState state = state::CombatState::Hub;
	std::uint64_t lastMoved = 0;
	std::uint64_t lastClip = 0;
	std::uint64_t lastState = 0;
	std::uint64_t lastCorrection = 0;
	std::size_t corrections = 0;
	std::size_t uncorrectable = 0;
	bool everTarget = false;
	bool seen = false;
	double firstX = 0.0; // where the fighter first appeared in the arena (a lazy squad is built where its token has got to)
	double firstZ = 0.0;
	std::uint64_t firstBuilding = 0;
	std::array<std::size_t, 12> states{}; // samples per combat state (0..11)
};

constexpr std::uint64_t kSampleTicks = 10; // 0.5 s
constexpr std::uint64_t kFrozenTicks = 1200; // 60 s without moving
constexpr std::uint64_t kCrawlTicks = 120; // 6 s

struct Frozen
{
	std::uint64_t id = 0;
	const state::Fighter* fighter = nullptr;
	const Track* track = nullptr;
	std::string layer;
	double encumbrance = 0.0;
	bool onMesh = true;
};

} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc < 2 || !test::worldRegionReady(std::string(argv[1]), simlink::kHubZoneX, simlink::kHubZoneZ))
		{
			std::cout << "ksim_brawl_freeze: SKIPPED: needs the world navmesh bake (pass its directory)\n";
			return 77;
		}
		std::cout << std::unitbuf;
		const std::string bake(argv[1]);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");
		std::vector<kworld::ZoneCell> hubZones;
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				hubZones.push_back({simlink::kHubZoneX + dx, simlink::kHubZoneZ + dz});
		}

		// ---- the server's --scenario newgame setup (ksim_server.cpp): seed 1, the world bake, the squad AI ----
		SpawnOptions options;
		options.seed = 1;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, options, nullptr, RegionConfig{});
		CombatSettings combatSettings;
		combatSettings.substeps = 2;
		sim.enableCombat(CombatData::build(*sim.gameData(), KENSHI_DATA), combatSettings);
		sim.enableNavigation(test::openWorldNav(bake, hubZones));
		AiSystem::Config ai;
		ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(KSIM_AI_TABLE));
		ai.navigator = KnavNavigator::open(KENSHI_DATA, bake, 4, 7, 6, 9, 4);
		ai.factions = FactionBook::build(sim.gameData());
		sim.enableAi(std::move(ai));
		std::string startSid;
		for (const kdata::Record* record : sim.gameData()->byType(kworld::itemTypeByName("NEW_GAME_STARTOFF")))
		{
			if (record->name == "Wanderer")
				startSid = record->stringID;
		}
		sim.enqueue(Command::newGame(startSid, "Tester", 1));
		sim.run(3);
		// The user's camera stood over the bar. Full detail is the player's bubbles union the camera's region (residents.cpp
		// stepPlayerAreas), so without the view the Hub's residents stay in their lazy regions and nobody is in the arena to fight.
		sim.enqueue(Command::setView(kBarX, kBarZ));
		sim.run(60);
		std::cout << "new game day " << gameTimeAt(sim.clockTick()).day << ", " << sim.combat()->members().size() << " fighters\n";

		ecsOf(sim).each([&](const state::TownState& town)
		{
			if (std::hypot(town.x - kBarX, town.z - kBarZ) < 3000.0)
				std::cout << "  town " << town.rootSid << " at (" << town.x << ", " << town.z << ")\n";
		});
		// Which y makes the bar point a building (the spawn resolves `building` with buildingAt on its ground y).
		say() << "  buildingAt at the bar point by y:";
		for (double y = 960.0; y <= 1300.0; y += 20.0)
		{
			const std::uint64_t building = sim.buildingAt(kBarX, y, kBarZ);
			if (building != 0)
				say() << " " << y << "=" << building;
		}
		say() << "\n";
		// The Storm House's own heights against the terrain and the mesh at the spawn point.
		const double groundY = sim.groundHeight(kBarX, kBarZ).value_or(0.0);
		const std::optional<knav::Position> spawnSnap = sim.navigation() != nullptr
			? sim.navigation()->walker().snap({kBarX, groundY, kBarZ}, 100.0) : std::nullopt;
		say() << "  bar terrain y " << groundY << ", snap y " << (spawnSnap ? spawnSnap->y : 0.0);
		if (const BuildingSite* site = sim.world().buildingSite(4661440962770670491ull))
		{
			say() << ", Storm House originY " << site->originY << " minY " << site->minY << " maxY " << site->maxY
				<< " hull " << site->hull << " floors [";
			for (const double surface : site->floors)
				say() << " " << surface;
			say() << " ] hullY " << site->hullMinY << ".." << site->hullMaxY;
		}
		say() << "\n";

		// ---- the spawn: three Dust Bandit squads into the Storm House, the goal the town by the bar ----
		// KSIM_BRAWL_SPAWN moves the spawn point (for trying the brawl in the open); the bar itself stays the measuring point.
		double spawnX = kBarX;
		double spawnZ = kBarZ;
		if (const char* at = std::getenv("KSIM_BRAWL_SPAWN"); at != nullptr)
			std::sscanf(at, "%lf,%lf", &spawnX, &spawnZ);
		std::vector<std::uint64_t> spawned;
		for (std::size_t i = 0; i < kSquads; ++i)
		{
			const Simulation::DebugSpawn placed = sim.debugSpawnSquad(kTemplate, spawnX, spawnZ, spawnX, spawnZ);
			test::require(placed.squadId != 0, "debugSpawnSquad " + std::to_string(i + 1) + ": " + placed.reason);
			spawned.push_back(placed.squadId);
			std::cout << "spawned squad " << placed.squadId << " at (" << spawnX << ", " << spawnZ << ")\n";
		}
		std::size_t bandits = 0;
		std::vector<std::pair<std::uint64_t, std::string>> banditIds; // Id, CHARACTER template
		for (const std::uint64_t squad : spawned)
		{
			const flecs::entity squadEntity = squadEntityOf(sim, squad);
			std::size_t members = 0;
			ecsOf(sim).each([&](flecs::entity member, const state::CharacterInstance& instance)
			{
				if (member.target<state::MemberOf>() != squadEntity.id())
					return;
				++members;
				if (const state::Id* id = member.get<state::Id>())
					banditIds.emplace_back(id->value, instance.templateSid);
			});
			bandits += members;
			std::cout << "  squad " << squad << " alive " << squadEntity.is_alive() << " instance "
				<< squadEntity.has<state::SquadInstance>() << " position " << squadEntity.has<state::Position>()
				<< " members " << members << "\n";
		}
		std::size_t characters = 0;
		ecsOf(sim).each([&](const state::CharacterInstance&) { ++characters; });
		std::cout << "spawned " << bandits << " bandits in " << kSquads << " squads (" << characters << " characters in the world)\n";
		std::map<std::string, std::size_t> byTemplate;
		for (const auto& [id, templateSid] : banditIds)
			++byTemplate[templateSid];
		for (const auto& [templateSid, count] : byTemplate)
			std::cout << "  " << count << " x " << templateSid << "\n";
		for (const std::uint64_t squad : spawned)
		{
			const flecs::entity entity = squadEntityOf(sim, squad);
			const state::GroupAI* ai = entity.get<state::GroupAI>();
			std::cout << "  squad " << squad << " group goal " << (ai != nullptr ? static_cast<int>(ai->goal) : -1)
				<< " order " << (ai != nullptr ? static_cast<int>(ai->order.status) : -1)
				<< " target " << (ai != nullptr ? ai->order.target.id : 0)
				<< " route " << (ai != nullptr ? ai->route.size() : 0) << "\n";
		}
		test::require(bandits > 0, "the spawns produced no members");
		std::set<std::uint64_t> banditSet;
		for (const auto& [id, templateSid] : banditIds)
			banditSet.insert(id);

		// ---- observe ----
		std::unordered_map<std::uint64_t, Track> tracks;
		sim.setCombatObserver([&](const CombatEvent& event)
		{
			if (event.kind != CombatEvent::Kind::PositionCorrected)
				return;
			Track& track = tracks[event.who];
			++track.corrections;
			track.uncorrectable += event.a < 0.0 ? 1 : 0;
			track.lastCorrection = sim.clockTick();
		});
		const std::uint64_t observeStart = sim.clockTick();
		const std::uint64_t observeTicks = 10 * kTicksPerGameHour; // 10 game hours of brawl
		std::uint64_t now = observeStart;
		for (std::uint64_t step = 0; step < observeTicks; step += kSampleTicks)
		{
			sim.run(kSampleTicks);
			now = sim.clockTick();
			for (const CombatArena::Member& member : sim.combat()->members())
			{
				const state::Fighter& f = member.fighter;
				Track& track = tracks[member.id];
				if (track.lastMoved == 0)
				{
					track.lastMoved = track.lastClip = track.lastState = now;
					track.state = f.state;
				}
				if (!track.seen)
				{
					track.seen = true;
					track.firstX = f.x;
					track.firstZ = f.z;
					track.firstBuilding = f.building;
				}
				if (std::hypot(f.x - track.x, f.z - track.z) > 0.01)
					track.lastMoved = now;
				if (f.clipTime != track.clip)
					track.lastClip = now;
				if (f.state != track.state)
				{
					track.lastState = now;
					track.state = f.state;
				}
				track.x = f.x;
				track.z = f.z;
				track.clip = f.clipTime;
				track.everTarget = track.everTarget || f.target != 0;
				if (static_cast<std::size_t>(f.state) < track.states.size())
					++track.states[static_cast<std::size_t>(f.state)];
			}
			if (step == 0)
			{
				for (const CombatArena::Member& member : sim.combat()->members())
				{
					const state::Fighter& f = member.fighter;
					if (f.dead || std::hypot(f.x - kBarX, f.z - kBarZ) > 150.0)
						continue;
					say() << "  first sample near the bar " << member.id << " " << f.name << " bandit "
						<< (banditSet.count(member.id) != 0) << " at (" << f.x << ", " << f.y << ", " << f.z << ") building "
						<< f.building << " buildingAt " << sim.buildingAt(f.x, f.y, f.z) << " team " << f.team << "\n";
				}
			}
			if (step % (300 * kSampleTicks) == 0)
			{
				std::size_t down = 0;
				std::size_t moving = 0;
				std::size_t nearBar = 0;
				std::size_t targeting = 0;
				for (const CombatArena::Member& member : sim.combat()->members())
				{
					const state::Fighter& f = member.fighter;
					down += f.dead || f.unconscious || f.down;
					moving += now - tracks[member.id].lastMoved < 100;
					nearBar += !f.dead && std::hypot(f.x - kBarX, f.z - kBarZ) < 300.0;
					targeting += !f.dead && f.target != 0;
				}
				say() << "t+" << (now - observeStart) / kTicksPerGameMinute << " min: " << sim.combat()->members().size() << " fighters, "
					<< down << " down, " << moving << " moved in the last 5 s, " << targeting << " have a target, " << nearBar
					<< " within 300 of the bar, corrections " << sim.combat()->positionCorrections()
					<< " (" << sim.combat()->uncorrectable() << " uncorrectable)\n";
				std::size_t townAware = 0; // town fighters that see a bandit
				std::size_t banditAware = 0; // bandits that see a town fighter
				double nearestPair = 1e9;
				std::uint64_t nearestBanditId = 0;
				std::uint64_t nearestOtherId = 0;
				for (const CombatArena::Member& a : sim.combat()->members())
				{
					const bool aBandit = banditSet.count(a.id) != 0;
					for (const CombatArena::Member& b : sim.combat()->members())
					{
						if (&a == &b || a.fighter.dead || b.fighter.dead || aBandit == (banditSet.count(b.id) != 0))
							continue;
						const double far = std::hypot(a.fighter.x - b.fighter.x, a.fighter.z - b.fighter.z);
						if (far < nearestPair)
						{
							nearestPair = far;
							nearestBanditId = aBandit ? a.id : b.id;
							nearestOtherId = aBandit ? b.id : a.id;
						}
						if (perception::aware(a.fighter, b.id))
							(aBandit ? banditAware : townAware) += 1;
					}
				}
				const state::Fighter* nearBandit = sim.combat()->find(nearestBanditId);
				const state::Fighter* nearTown = sim.combat()->find(nearestOtherId);
				const CombatArena::Member* banditMember = nullptr;
				const CombatArena::Member* townMember = nullptr;
				for (const CombatArena::Member& member : sim.combat()->members())
				{
					if (member.id == nearestBanditId)
						banditMember = &member;
					if (member.id == nearestOtherId)
						townMember = &member;
				}
				say() << "    perception: " << townAware << " town-aware-of-a-bandit pairs, " << banditAware
					<< " bandit-aware-of-a-town pairs, nearest pair " << nearestPair << " (" << nearestBanditId << " vs " << nearestOtherId
					<< ", buildings " << (nearBandit != nullptr ? nearBandit->building : 0) << " vs "
					<< (nearTown != nullptr ? nearTown->building : 0) << ")\n";
				if (banditMember != nullptr && townMember != nullptr)
				{
					say() << "    join: bandit driven " << banditMember->joinDriven << " goals " << static_cast<int>(banditMember->joinGoals)
						<< " attacksNeutrals " << banditMember->fighter.attacksNeutrals << ", town driven " << townMember->joinDriven
						<< " goals " << static_cast<int>(townMember->joinGoals) << " attacksNeutrals " << townMember->fighter.attacksNeutrals
						<< ", hostileFactions " << sim.combat()->hostileFactions(banditMember->fighter.faction, townMember->fighter.faction)
						<< ", aware bandit-of-town " << perception::aware(banditMember->fighter, townMember->id)
						<< " town-of-bandit " << perception::aware(townMember->fighter, banditMember->id) << "\n";
				}
				for (const std::uint64_t squad : spawned)
				{
					const state::GroupAI* ai = squadEntityOf(sim, squad).get<state::GroupAI>();
					if (ai != nullptr)
						say() << "    squad " << squad << " goal " << static_cast<int>(ai->goal) << " order "
							<< static_cast<int>(ai->order.status) << " failure " << static_cast<int>(ai->order.failure)
							<< " route " << ai->route.size() << "\n";
				}
			}
		}

		// ---- who is still standing and has not moved for a minute ----
		std::vector<Frozen> frozen;
		std::map<std::string, std::size_t> layers;
		std::size_t standing = 0;
		std::size_t frozenEverTarget = 0; // of the frozen: fighters that ever held a target (were in a fight)
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			const state::Fighter& f = member.fighter;
			if (f.dead || f.unconscious || f.down)
				continue;
			++standing;
			const Track& track = tracks[member.id];
			if (now - track.lastMoved < kFrozenTicks)
				continue;
			Frozen one;
			one.id = member.id;
			one.fighter = &f;
			one.track = &track;
			one.onMesh = sim.navigation() != nullptr && sim.navigation()->walker().onMesh({f.x, f.y, f.z});
			one.encumbrance = sim.charStats(member.id).encumbrance;
			const bool moving = std::hypot(f.velX, f.velZ) > 1.0;
			if (track.uncorrectable > 0)
				one.layer = "off-mesh, uncorrectable";
			else if (!one.onMesh)
				one.layer = "off-mesh";
			else if (f.state == state::CombatState::Attack && now - track.lastClip >= kCrawlTicks)
				one.layer = "swing frozen";
			else if (f.state == state::CombatState::Wait)
				one.layer = f.target != 0 ? "Wait on a target" : "Wait, no target";
			else if (moving)
				one.layer = "crawl";
			else
				one.layer = "idle (" + stateName(f.state) + ")";
			++layers[one.layer];
			frozenEverTarget += track.everTarget ? 1 : 0;
			frozen.push_back(one);
		}
		std::cout << "after " << (now - observeStart) / kTicksPerGameMinute << " game minutes: " << standing << " standing, "
			<< frozen.size() << " frozen for a minute or more (" << frozenEverTarget << " of them ever held a target), corrections "
			<< sim.combat()->positionCorrections() << " (" << sim.combat()->uncorrectable() << " uncorrectable)\n";
		for (const auto& [layer, count] : layers)
			std::cout << "  " << count << " x " << layer << "\n";
		// The building ledger: the bar's own people against the spawned bandits (perception only sees one building at a time).
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			const state::Fighter& f = member.fighter;
			if (f.dead || std::hypot(f.x - kBarX, f.z - kBarZ) > 100.0)
				continue;
			say() << "  near the bar " << member.id << " " << f.name << " bandit " << (banditSet.count(member.id) != 0)
				<< " y " << f.y << " building " << f.building << " buildingAt " << sim.buildingAt(f.x, f.y, f.z)
				<< " team " << f.team << "\n";
		}
		for (const Frozen& one : frozen)
		{
			const state::Fighter& f = *one.fighter;
			std::string engaged;
			for (const std::uint64_t id : f.engaged)
				engaged += (engaged.empty() ? "" : ",") + std::to_string(id);
			std::cout << "  frozen " << one.id << " team " << f.team << " " << one.layer << ": state " << stateName(f.state)
				<< " technique " << f.technique << " clip " << f.clipTime << " target " << f.target << " engaged [" << engaged << "]"
				<< " vel " << std::hypot(f.velX, f.velZ) << " enc " << one.encumbrance << "% at (" << f.x << ", " << f.z << ")"
				<< " corrections " << one.track->corrections << "\n";
		}
		// The spawned bandits as fighters: the stat block the spawner gave them (kinspect crowd_melee: CHARACTER 510 has
		// combat stats 10, stats randomise 3, so strength ~7-13) and their load.
		std::size_t promoted = 0;
		std::map<std::string, std::pair<double, double>> strengthRange; // template -> (min, max) strength
		for (const auto& [id, templateSid] : banditIds)
		{
			const state::Fighter* f = sim.combat()->find(id);
			if (f == nullptr)
			{
				say() << "  bandit " << id << " " << templateSid << ": no fighter (not promoted)\n";
				continue;
			}
			++promoted;
			auto& range = strengthRange.try_emplace(templateSid, std::make_pair(f->skills.strength, f->skills.strength)).first->second;
			range.first = std::min(range.first, f->skills.strength);
			range.second = std::max(range.second, f->skills.strength);
			say() << "  bandit " << id << " " << templateSid << " strength " << f->skills.strength << " enc "
				<< sim.charStats(id).encumbrance << "% team " << f->team << " state " << stateName(f->state)
				<< " target " << f->target << " stood " << (now - tracks[id].lastMoved) / kTicksPerGameMinute << " min"
				<< " building " << f->building << " goal " << static_cast<int>(f->goal.kind) << " route " << f->routeNext << "/"
				<< f->route.size() << " walking " << f->walking() << " at (" << f->x << ", " << f->z << ")\n";
		}
		for (const auto& [templateSid, range] : strengthRange)
			std::cout << "  " << templateSid << ": strength " << range.first << " to " << range.second << "\n";
		// Where a lazy squad's fighters are actually built: the squad token has usually left the spawn point by then, so the
		// spawn point's building is not the one they get.
		for (const auto& [id, templateSid] : banditIds)
		{
			const Track& track = tracks[id];
			if (!track.seen)
				continue;
			say() << "  bandit " << id << " first seen at (" << track.firstX << ", " << track.firstZ << ") building "
				<< track.firstBuilding << ", " << std::hypot(track.firstX - kBarX, track.firstZ - kBarZ) << " u from the bar\n";
		}
		// Combat states over the whole run: the spawned bandits against everyone else (Circle / Wait / Approach per sample).
		std::array<std::size_t, 12> banditSamples{};
		std::array<std::size_t, 12> otherSamples{};
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			const Track& track = tracks[member.id];
			std::array<std::size_t, 12>& into = banditSet.count(member.id) != 0 ? banditSamples : otherSamples;
			for (std::size_t s = 0; s < 12; ++s)
				into[s] += track.states[s];
		}
		for (std::size_t s = 0; s < 12; ++s)
		{
			if (banditSamples[s] == 0 && otherSamples[s] == 0)
				continue;
			std::cout << "  state " << stateName(static_cast<state::CombatState>(s)) << ": bandits " << banditSamples[s]
				<< " samples, others " << otherSamples[s] << "\n";
		}
		// Who fought at all: every fighter with a sample outside Hub/Done, most first.
		std::vector<std::pair<std::size_t, const CombatArena::Member*>> fighters;
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			const Track& track = tracks[member.id];
			const std::size_t hub = static_cast<std::size_t>(state::CombatState::Hub);
			const std::size_t done = static_cast<std::size_t>(state::CombatState::Done);
			std::size_t fought = 0;
			for (std::size_t s = 0; s < track.states.size(); ++s)
				fought += s != hub && s != done ? track.states[s] : 0;
			if (fought > 0)
				fighters.emplace_back(fought, &member);
		}
		std::sort(fighters.begin(), fighters.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
		std::cout << "  " << fighters.size() << " fighters saw combat (Attack/Defend/Circle/Wait/Stumble/Approach samples):\n";
		for (std::size_t i = 0; i < std::min<std::size_t>(fighters.size(), 15); ++i)
		{
			const state::Fighter& f = fighters[i].second->fighter;
			std::cout << "    " << fighters[i].second->id << " " << f.name << " team " << f.team << " faction " << f.faction
				<< " " << fighters[i].first << " samples, at (" << f.x << ", " << f.z << ")\n";
		}
		std::cout << promoted << " of " << banditIds.size() << " spawned bandits became fighters\n";
		for (const std::uint64_t squad : spawned)
		{
			const flecs::entity entity = squadEntityOf(sim, squad);
			const state::GroupAI* ai = entity.get<state::GroupAI>();
			const state::Position* at = entity.get<state::Position>();
			std::cout << "  squad " << squad << " group goal " << (ai != nullptr ? static_cast<int>(ai->goal) : -1)
				<< " order " << (ai != nullptr ? static_cast<int>(ai->order.status) : -1)
				<< " failure " << (ai != nullptr ? static_cast<int>(ai->order.failure) : -1)
				<< " target " << (ai != nullptr ? ai->order.target.id : 0)
				<< " route " << (ai != nullptr ? ai->route.size() : 0)
				<< " fallback " << (ai != nullptr ? static_cast<int>(ai->fallback) : -1)
				<< " at (" << (at != nullptr ? at->x : 0.0) << ", " << (at != nullptr ? at->z : 0.0) << ")\n";
		}
		// Whether the brawl happened at all (task AR: in this reproduction it does not, see the perception lines: the spawned
		// bandits carry building 0, and perception only sees a target in the same building). A diagnostic, not a gate.
		std::size_t fighting = 0;
		for (const CombatArena::Member& member : sim.combat()->members())
		{
			const state::Fighter& f = member.fighter;
			fighting += !f.dead && f.target != 0;
		}
		std::cout << "ksim_brawl_freeze: " << fighting << " fighters have a target at the end, " << frozen.size() << " of "
			<< standing << " standing frozen for a minute or more\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_brawl_freeze: FAILED: " << e.what() << "\n";
		return 1;
	}
}
