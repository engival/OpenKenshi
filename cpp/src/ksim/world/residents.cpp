#include "ksim/core/simulation.hpp"

#include "world/resident_data.hpp"

#include "ksim/ai/ai.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/combat/corpses.hpp"
#include "ksim/combat/combat_data.hpp"
#include "ksim/world/factions.hpp"
#include "ksim/world/join_rules.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/ai/routines.hpp"
#include "ksim/economy/trader.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <numbers>
#include <set>
#include <string_view>
#include <tuple>
#include <vector>

namespace ksim {

// Residents as Fighters (TOWN_LIFE_SLICE 5.3, T3b1). A town squad that is at Full detail and in routine mode has its members built
// into Fighters (CharacterInstance entity + Fighter + Routine, the squad gets SquadRoutine); when the squad leaves Full detail or
// routine mode they collapse back to the token (the Fighter and the Routine go, the reservations are released). Nothing here chooses
// what a resident does: that is the scorer (T3b2).

namespace {

// The squad is a town resident group this step: at Full detail, a town source with a home, resting at home (Idle) on a home-bound
// behaviour whose package the scorer models (routines::routineStatus "modelled"). T3b2 widens the behaviours as they land.
bool routineModeByAi(flecs::entity squad, const state::SquadInstance& instance, const AiSystem& ai, const routines::PackageTable& packages)
{
	const Detail* detail = squad.get<Detail>();
	const state::GroupAI* group = squad.get<state::GroupAI>();
	if (detail == nullptr || detail->level != DetailLevel::Full || group == nullptr || squad.get<state::Position>() == nullptr)
		return false;
	if (instance.homeTown.id == 0 || group->goal != state::GoalKind::Idle || group->fallback != state::AiReason::None)
		return false;
	if (instance.source != state::SpawnSource::TownResident && instance.source != state::SpawnSource::TownBar
		&& instance.source != state::SpawnSource::TownDefaultResident)
		return false;
	const Mapping& mapping = ai.registry().squad(instance.templateSid);
	if (mapping.behaviour != Behaviour::GoHome && mapping.behaviour != Behaviour::StayHome && mapping.behaviour != Behaviour::ShopKeeper
		&& mapping.behaviour != Behaviour::ResidentLife && mapping.behaviour != Behaviour::BarHangout
		&& mapping.behaviour != Behaviour::GuardShift && mapping.behaviour != Behaviour::CageWarden)
		return false;
	const routines::CompiledPackage* package = packages.find(mapping.package);
	return package != nullptr && std::strcmp(routines::routineStatus(*package), "modelled") == 0;
}

// A merchant squad (T1's TraderStock) at home in a town at Full detail: its people are built whether or not the AI is on and whatever its package
// maps to, so that the player can walk up and talk (docs/reference/MINE_SELL_SLICE.md M4; the barman of the Hub). A wandering trader mid-trip is
// not at home; a squad the AI sends elsewhere (a goal other than Idle) is not either.
bool merchantAtHome(flecs::entity squad, const state::SquadInstance& instance)
{
	const Detail* detail = squad.get<Detail>();
	if (detail == nullptr || detail->level != DetailLevel::Full || squad.get<state::Position>() == nullptr || squad.get<state::TraderStock>() == nullptr)
		return false;
	if (instance.homeTown.id == 0 || squad.has<state::TraderTrip>())
		return false;
	if (instance.source != state::SpawnSource::TownResident && instance.source != state::SpawnSource::TownBar
		&& instance.source != state::SpawnSource::TownDefaultResident)
		return false;
	const state::GroupAI* group = squad.get<state::GroupAI>();
	return group == nullptr || group->goal == state::GoalKind::Idle;
}

// `ai` may be null (a server without --bake): then only the merchants are promoted.
bool inRoutineMode(flecs::entity squad, const state::SquadInstance& instance, const AiSystem* ai, const routines::PackageTable& packages)
{
	return (ai != nullptr && routineModeByAi(squad, instance, *ai, packages)) || merchantAtHome(squad, instance);
}

routines::Site fromState(const state::RoutineSite& site)
{
	return {static_cast<routines::SiteKind>(site.kind), site.object, site.slot};
}

// The members of a squad, in Id order.
std::vector<std::pair<std::uint64_t, flecs::entity>> membersOf(flecs::entity squad)
{
	std::vector<std::pair<std::uint64_t, flecs::entity>> found;
	squad.world().query_builder<const state::Id>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Id& id)
	{
		found.emplace_back(id.value, member);
	});
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	return found;
}

// FNV-1a: a stable hash of a stringID, the same on every platform (as weather.cpp's).
std::uint64_t fnv1a(std::string_view text) noexcept
{
	std::uint64_t hash = 1469598103934665603ull;
	for (const char c : text)
	{
		hash ^= static_cast<unsigned char>(c);
		hash *= 1099511628211ull;
	}
	return hash;
}

// Task CC: a member's stable dice identity, the key its combat stream, comfort radii, ragdoll and follow formation hang on. The
// member's Id is not usable: every entity created before it moves its Id, which used to re-roll every fighter and re-pin every fight
// fixture. This key is the squad's own content (its SquadInstance: template, source, spawner slot and nest, den and home point, home
// building: no Ids) plus the member's slot in the squad, so a spawn anywhere else in the world leaves it alone. OURS: two squads of
// one template with identical content (the same building, the same den point) share their dice, which is what the data says of them.
std::uint64_t memberDiceKey(const state::SquadInstance& squad, std::size_t slot) noexcept
{
	std::uint64_t key = fnv1a(squad.templateSid);
	key = splitMix64(key ^ static_cast<std::uint64_t>(squad.source));
	key = splitMix64(key ^ static_cast<std::uint64_t>(static_cast<std::uint32_t>(squad.slot)));
	key = splitMix64(key ^ squad.nest);
	key = splitMix64(key ^ std::bit_cast<std::uint64_t>(squad.hasDen ? squad.denX : 0.0));
	key = splitMix64(key ^ std::bit_cast<std::uint64_t>(squad.hasDen ? squad.denZ : 0.0));
	key = splitMix64(key ^ squad.homeBuilding);
	key = splitMix64(key ^ std::bit_cast<std::uint64_t>(squad.homeX));
	key = splitMix64(key ^ std::bit_cast<std::uint64_t>(squad.homeZ));
	key = splitMix64(key ^ (static_cast<std::uint64_t>(slot) + 1ull) * 0x9E3779B97F4A7C15ull);
	return key != 0 ? key : 1;
}

// The key a fighter's other dice hang on: its stable identity when the spawn path set one, else its Id (a scenario member spawned
// without a key, an older save).
std::uint64_t diceKeyOf(const state::Fighter& fighter, std::uint64_t id) noexcept
{
	return fighter.diceSeed != 0 ? fighter.diceSeed : id;
}

constexpr std::int32_t kWildTeam = 1;
// A fight holds a demotion only while the two are this close (the region's keepRadius). A target kept in memory across the map (a fled
// player) would otherwise hold a squad built for ever, with no region simulating it.
constexpr double kFightRange = 900.0;

// Whether a member of the squad is in a fight: it has a living target, or a player character targets it, within kFightRange.
bool squadFighting(const CombatArena& combat, const std::map<std::uint64_t, std::uint64_t>& targetedByPlayer,
	const std::vector<std::pair<std::uint64_t, flecs::entity>>& members)
{
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat.find(id);
		if (f == nullptr || f->dead || f->playerControlled)
			continue;
		const auto near = [&](const state::Fighter& other) { return std::hypot(f->x - other.x, f->z - other.z) <= kFightRange; };
		const auto hunter = targetedByPlayer.find(id);
		if (hunter != targetedByPlayer.end() && near(*combat.find(hunter->second)))
			return true;
		const state::Fighter* target = f->target != 0 ? combat.find(f->target) : nullptr;
		if (target != nullptr && !target->dead && !target->unconscious && near(*target))
			return true;
	}
	return false;
}

// Whether a built, alive, non-player member of the squad stands where the navmesh is not (a streaming mesh dropped the zone under it, or the walk
// ended past the coverage): it cannot move or be corrected (PositionCorrected -1 every tick), so no fight it is in can be fought.
bool squadOffMesh(const CombatArena& combat, const MeshNavigator* navigation, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members)
{
	if (navigation == nullptr)
		return false;
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat.find(id);
		if (f != nullptr && !f->dead && !f->playerControlled && !navigation->walker().onMesh({f->x, f->y, f->z}))
			return true;
	}
	return false;
}

// Who is aimed at by a player character: target -> that character.
std::map<std::uint64_t, std::uint64_t> targetedByPlayers(const CombatArena& combat)
{
	std::map<std::uint64_t, std::uint64_t> found;
	for (const CombatArena::Member& member : combat.members())
	{
		if (member.fighter.playerControlled && !member.fighter.dead && member.fighter.target != 0)
			found.emplace(member.fighter.target, member.id);
	}
	return found;
}

// The leader of a built world squad: its first member, in Id order, that is an alive, conscious, non-player Fighter.
std::uint64_t leaderOf(const CombatArena& combat, const std::vector<std::pair<std::uint64_t, flecs::entity>>& members)
{
	for (const auto& [id, member] : members)
	{
		const state::Fighter* f = combat.find(id);
		if (f != nullptr && !f->dead && !f->unconscious && !f->playerControlled && !member.has<state::Routine>())
			return id;
	}
	return 0;
}

// How far past the group's route progress the leader is sent (the carrot); its route is re-issued every carry step, so the leader never
// runs out of walk.
constexpr double kCarryLookahead = 40.0;
constexpr double kCarryReachY = 60.0; // vertical reach of the carrot's surface lookup
constexpr double kScenarioTownReach = 2500.0; // scenarioSquad: a new squad within this of a town is its resident (UNSURE, ours)
constexpr std::uint32_t kCarryNoRouteTicks = 200; // 10 s without a route to the carrot: the leader is placed at it

flecs::entity playerFactionOf(flecs::world& ecs)
{
	flecs::entity found;
	std::uint64_t lowest = ~std::uint64_t(0);
	ecs.each([&](flecs::entity entity, const state::PlayerFaction&, const state::FactionState&, const state::Id& id)
	{
		if (id.value < lowest)
		{
			lowest = id.value;
			found = entity;
		}
	});
	return found;
}

} // namespace

// The one place a world fighter's hostility is decided (TOWN_LIFE_SLICE 5.14). Combat's enemy() is "different team", so for now it is a
// team: 0 (the player's, the neutrals') unless the squad's own faction is hostile to the player faction by factionsHostile - the
// original's directional test (FUN_14079c950, kinspect dt11: MY faction's view of the target, -30 or below, or a personal enemy tag),
// so a Dust Bandit (-10 toward a new player) is neutral while a Bloodraider (-100) is not. Otherwise
// 2 + the lowest Id among the factions hostile to the player that this one tolerates both ways (two hostile factions that do not hate
// each other share a team and do not fight; the grouping is not transitive: UNSURE). A squad with no faction (kWildTeam, 1) is hostile
// to everyone, on a team of its own. Never reads a faction or record name. `hostileEither`, retired 2026-11-07 (task BV), read both
// directions; its text is cpp/src/ksim/retired/hostility_either_view.txt.
static std::int32_t worldTeam(flecs::world& ecs, const kdata::GameData& data, state::EntityRef faction)
{
	const flecs::entity player = playerFactionOf(ecs);
	if (!player.is_valid())
		return 0;
	if (faction.id == 0 || !ecs.is_alive(faction.id))
		return kWildTeam;
	const flecs::entity own = ecs.entity(faction.id);
	if (!factionsHostile(data, own, player))
		return 0;
	std::uint64_t lowest = own.get<state::Id>() != nullptr ? own.get<state::Id>()->value : faction.id;
	ecs.each([&](flecs::entity other, const state::FactionState&, const state::Id& id)
	{
		if (other.id() == own.id() || id.value >= lowest)
			return;
		if (factionsHostile(data, other, player) && factionRelation(data, own, other) > FactionBook::kHostile
			&& factionRelation(data, other, own) > FactionBook::kHostile)
			lowest = id.value;
	});
	return 2 + static_cast<std::int32_t>(lowest % 1000000);
}

flecs::entity Simulation::scenarioSquad(std::string_view templateSid, std::string_view factionSid, double x, double z, double areaX, double areaZ,
	double radius, bool hostile, const std::set<flecs::entity_t>& taken, bool* claimed)
{
	if (claimed != nullptr)
		*claimed = false;
	flecs::world& ecs = ecsMutable();
	flecs::entity best;
	double bestD = 0.0;
	ecs.each([&](flecs::entity squad, const state::SquadInstance& instance, const state::Position& at)
	{
		if (instance.templateSid != templateSid || instance.source == state::SpawnSource::Dismissed || taken.count(squad.id()) != 0
			|| std::hypot(at.x - areaX, at.z - areaZ) > radius)
			return;
		const double d = std::hypot(at.x - x, at.z - z);
		if (!best.is_valid() || d < bestD)
		{
			best = squad;
			bestD = d;
		}
	});
	if (best.is_valid())
	{
		ecs.delete_with<state::MemberOf>(best.id()); // the default world's people of this squad; the file's replace them
		// A town squad's token stays at its town (it is Idle there and its people run their routines from wherever the file put them); the others
		// follow their people, as a built world squad's token does.
		if (best.get<state::SquadInstance>()->homeTown.id == 0)
		{
			if (regions_ != nullptr)
				regions_->place(ecs, best.get<state::Id>()->value, x, z);
			else
				best.set<state::Position>(state::Position{x, z});
		}
		if (claimed != nullptr)
			*claimed = true;
		return best;
	}
	state::EntityRef faction;
	ecs.each([&](flecs::entity entity, const state::FactionState& state)
	{
		if (state.templateSid == factionSid)
			faction.id = entity.id();
	});
	// A world built from the templates alone (the server's --no-world) has no faction instances for the file's factions: the squad
	// would be factionless, and worldTeam reads a factionless squad as hostile to everyone (kWildTeam, residents.cpp). bar_lure's
	// Dust Bandit patrol then charged the player on sight (2026-11-07; the client's run counted 11 targeting the player at load).
	// The file names the faction, so its instance is made here, as createPlayerFaction makes the player's.
	if (faction.id == 0 && !factionSid.empty() && gameData_ != nullptr && gameData_->find(std::string(factionSid)) != nullptr)
	{
		const flecs::entity made = state::createInstance(ecs);
		made.set<state::FactionState>(state::FactionState{std::string(factionSid)});
		faction.id = made.id();
	}
	flecs::entity squad = state::createInstance(ecs);
	state::SquadInstance instance;
	instance.templateSid = std::string(templateSid);
	instance.faction = faction;
	// UNSURE (the file does not say where a squad lives): a squad not hostile to the player that stands within kScenarioTownReach of a town is that
	// town's resident (its token at the town); anything else is a homeless wanderer with its den where its people stand.
	flecs::entity town;
	double townD = kScenarioTownReach;
	if (!hostile)
	{
		ecs.each([&](flecs::entity entity, const state::TownState& state)
		{
			const double d = std::hypot(state.x - x, state.z - z);
			if (d <= townD)
			{
				townD = d;
				town = entity;
			}
		});
	}
	state::Position at{x, z};
	if (town.is_valid())
	{
		instance.homeTown = state::EntityRef{town.id()};
		instance.source = state::SpawnSource::TownResident;
		at = state::Position{town.get<state::TownState>()->x, town.get<state::TownState>()->z};
	} else {
		instance.source = state::SpawnSource::BiomeHomeless;
		instance.hasDen = true;
		instance.denX = x;
		instance.denZ = z;
	}
	squad.set<state::SquadInstance>(std::move(instance));
	squad.set<state::Position>(at);
	return squad;
}

flecs::entity Simulation::scenarioMember(flecs::entity squad, const std::string& characterSid, bool leader, state::Fighter fighter)
{
	flecs::world& ecs = ecsMutable();
	flecs::entity member = state::createInstance(ecs);
	member.set<state::CharacterInstance>(state::CharacterInstance{characterSid});
	member.set<state::MemberRole>(state::MemberRole{leader ? state::kRoleLeader : state::kRoleSquad});
	member.add<state::MemberOf>(squad);
	adoptFighter(member, std::move(fighter));
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	residentData_->adopted.insert(member.get<state::Id>()->value);
	residentData_->fileSquads.insert(squad.get<state::Id>()->value);
	return member;
}

std::int32_t Simulation::teamOfFaction(const std::string& factionSid)
{
	flecs::world& ecs = ecsMutable();
	state::EntityRef faction;
	ecs.each([&](flecs::entity entity, const state::FactionState& state)
	{
		if (state.templateSid == factionSid)
			faction.id = entity.id();
	});
	return worldTeam(ecs, *gameData_, faction);
}

void Simulation::stepPlayerAreas()
{
	if (combat_ == nullptr || clockTick() % routines::kResidentIntervalTicks != 0)
		return;
	// Only a game started by NEW_GAME (a player faction with a start-off) has a living world around it. The scenario factions (the duel, the
	// squad scenario and the tests built on them stand on the Hub barman's spot) keep the areas the commands give them, as before.
	if (world_.ecs().count<state::PlayerFaction>() == 0)
		return;
	bool started = false;
	world_.ecs().each([&](const state::PlayerFaction& faction) { started = started || !faction.startoff.empty(); });
	if (!started)
		return;
	std::vector<state::GroundPoint> centres;
	std::set<std::int32_t> regions;
	for (const CombatArena::Member& member : combat_->members())
	{
		if (!member.fighter.playerControlled || member.fighter.dead)
			continue;
		if (regions.insert(regions_->regionOf(member.fighter.x, member.fighter.z)).second)
			centres.push_back({member.fighter.x, member.fighter.z});
	}
	// Full detail is the union of the bubbles and the camera's view (SIMLINK.md 5): the set_view focus is an area too.
	if (const state::ViewFocus* view = world_.ecs().get<state::ViewFocus>(); view != nullptr && regions.insert(regions_->regionOf(view->x, view->z)).second)
		centres.push_back({view->x, view->z});
	if (centres.empty())
		return;
	std::set<std::int32_t> current;
	if (const state::PlayerAreas* areas = world_.ecs().get<state::PlayerAreas>())
	{
		for (const state::GroundPoint& point : areas->centres)
			current.insert(regions_->regionOf(point.x, point.z));
	}
	if (current != regions)
		regions_->setPlayerAreas(ecsMutable(), centres);
}

// The world squads after a load: a squad whose alive member Fighters (not the player's) carry no Routine. Once. Also needed before the
// first squad step after a load (prepareWorldCarry), or that step would move a built squad's token by the route.
void Simulation::knowWorldSquads()
{
	if (combat_ == nullptr || gameData_ == nullptr)
		return;
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);
	if (residentData_->worldKnown)
		return;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	residentData_->worldKnown = true;
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const state::Fighter& f = members[i].fighter;
		flecs::entity entity = ecs.entity(combatEntities_[i]);
		if (f.dead || f.playerControlled || entity.has<state::Routine>() || residentData_->adopted.count(members[i].id) != 0)
			continue;
		const flecs::entity squad = entity.target<state::MemberOf>();
		const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
		if (instance != nullptr && instance->source != state::SpawnSource::Dismissed && squad.get<state::Id>() != nullptr)
			residentData_->world.insert(squad.get<state::Id>()->value);
	}
}

void Simulation::stepResidents()
{
	if (combat_ == nullptr || gameData_ == nullptr || combatData_ == nullptr || clockTick() % routines::kResidentIntervalTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (residentData_ == nullptr)
		residentData_ = std::make_shared<ResidentData>(*gameData_);

	// A resident that died stops running a routine and gives up its reservation (the original keeps the bed; ours frees it:
	// kinspect item 58, more permissive). Its body stays in the arena for looting.
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		if (!members[i].fighter.dead)
			continue;
		flecs::entity entity = ecs.entity(combatEntities_[i]);
		if (entity.has<state::Routine>())
		{
			routineBook_.release(members[i].id);
			entity.remove<state::Routine>();
		}
	}

	knowWorldSquads();

	// Who is in a fight: an arena member with a living target, or one a player character targets.
	const std::map<std::uint64_t, std::uint64_t> targetedByPlayer = targetedByPlayers(*combat_);
	const auto fighting = [&](flecs::entity squad) { return squadFighting(*combat_, targetedByPlayer, membersOf(squad)); };

	const bool hasPlayer = playerFactionOf(ecs).is_valid();
	std::vector<std::pair<std::uint64_t, flecs::entity>> promote;
	std::vector<std::pair<std::uint64_t, flecs::entity>> demote;
	std::vector<std::pair<std::uint64_t, flecs::entity>> worldPromote;
	std::vector<std::pair<std::uint64_t, flecs::entity>> worldDemote;
	std::set<std::uint64_t> seen;
	ecs.each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance& instance)
	{
		// A wandering trader carries SquadRoutine only for its bar table (5.17); it is never a promoted resident squad.
		const bool promoted = squad.has<state::SquadRoutine>() && !squad.has<state::TraderTrip>();
		const bool mode = inRoutineMode(squad, instance, ai_.get(), residentData_->packages);
		const bool world = residentData_->world.count(id.value) != 0;
		seen.insert(id.value);
		if (mode && !promoted)
			promote.emplace_back(id.value, squad);
		else if (!mode && promoted)
			demote.emplace_back(id.value, squad);
		// A world squad that became a resident group hands its people over (they are rebuilt with a Routine a pass later).
		if (mode && world)
			worldDemote.emplace_back(id.value, squad);
		if (mode || promoted)
			return;
		// A squad at Full detail with a position, and a player to be hostile to or friendly with. A wandering trader is promoted too (task
		// BP): its people are built while it is at Full detail, and the group step still walks its circuit by the leader (ARCHITECTURE
		// "Threading", the built-squad bullet; ai.cpp's `context.carried`); its `TraderTrip` is untouched, so a fold-back resumes the trip.
		const Detail* detail = squad.get<Detail>();
		const bool eligible = (hasPlayer || residentData_->fileSquads.count(id.value) != 0) && instance.source != state::SpawnSource::Dismissed && detail != nullptr && detail->level == DetailLevel::Full && squad.get<state::Position>() != nullptr;
		// An off-mesh leg (k16.7.15) is a token's: it is demoted at the edge and not promoted until the streaming brings it back onto the mesh
		// (the leg is then dropped by the group step, which sees a built squad).
		const state::GroupAI* groupAi = squad.get<state::GroupAI>();
		const state::Position* at = squad.get<state::Position>();
		const bool leaving = groupAi != nullptr && UtilityGroupLayer::tokenLeg(*groupAi) && at != nullptr
			&& !(navigation_ != nullptr && navigation_->covers(at->x, at->z) && groupAi->along > UtilityGroupLayer::kTokenLegFree);
		if (eligible && !world && !leaving)
			worldPromote.emplace_back(id.value, squad);
		else if (world && leaving)
			worldDemote.emplace_back(id.value, squad);
		else if (!eligible && world && (!fighting(squad) || squadOffMesh(*combat_, navigation_.get(), membersOf(squad))))
			worldDemote.emplace_back(id.value, squad); // a fight holds a squad only while it stands on the mesh: off it the fight is stuck for ever
	});
	// A squad that no longer exists leaves the set (its fighters went with dropGoneFighters).
	for (auto it = residentData_->world.begin(); it != residentData_->world.end();)
		it = seen.count(*it) != 0 ? std::next(it) : residentData_->world.erase(it);
	const auto byId = [](const auto& a, const auto& b) { return a.first < b.first; };
	std::sort(demote.begin(), demote.end(), byId);
	std::sort(promote.begin(), promote.end(), byId);
	std::sort(worldDemote.begin(), worldDemote.end(), byId);
	std::sort(worldPromote.begin(), worldPromote.end(), byId);
	for (const auto& [id, squad] : demote)
		demoteResidents(squad);
	for (const auto& [id, squad] : worldDemote)
		demoteWorldSquad(squad);
	// One cap for both kinds of promotion, residents first.
	std::size_t budget = static_cast<std::size_t>(routines::kResidentSquadsPerPass);
	for (std::size_t i = 0; i < promote.size() && budget != 0; ++i, --budget)
		promoteResidents(promote[i].second);
	for (std::size_t i = 0; i < worldPromote.size() && budget != 0; ++i, --budget)
		promoteWorldSquad(worldPromote[i].second);
	// T3d: a claim whose owner is gone is given back: a table of a squad that was wiped, a trader squad that is no longer staying.
	std::set<std::uint64_t> live;
	ecs.each([&](const state::Id& id, const state::Routine&) { live.insert(id.value); });
	std::vector<flecs::entity> leftTown;
	ecs.each([&](flecs::entity squad, const state::Id& id, const state::SquadRoutine&)
	{
		const state::TraderTrip* trip = squad.get<state::TraderTrip>();
		if (trip != nullptr && !trip->staying)
			leftTown.push_back(squad);
		else
			live.insert(id.value);
	});
	for (const flecs::entity squad : leftTown)
		releaseBarTable(squad);
	routineBook_.releaseDead([&](std::uint64_t owner) { return live.count(owner) != 0; });
	logWorldSquads();
}

std::optional<state::Fighter> Simulation::templateFighter(flecs::entity member, const state::SquadInstance& instance, std::size_t index, std::uint64_t& base)
{
	// Task CC: the build dice and the combat stream hang on the member's stable identity (squad content + slot), not its Id, so a spawn
	// elsewhere in the world leaves the person (stats, weapon, armour) and its fight the same.
	const state::CharacterInstance* character = member.get<state::CharacterInstance>();
	if (combatData_ == nullptr || gameData_ == nullptr || character == nullptr)
		return std::nullopt;
	const std::string templateSid = character->templateSid; // a copy: setting the slot below moves the member's components
	// The member's own slot (state::MemberSlot), not its place in the list: a squadmate that left must not move it. One without a slot takes the
	// index it has now, once.
	std::size_t slot = index;
	if (const state::MemberSlot* own = member.get<state::MemberSlot>())
		slot = own->slot;
	else
		member.set<state::MemberSlot>(state::MemberSlot{static_cast<std::uint32_t>(index)});
	const std::uint64_t dice = memberDiceKey(instance, slot);
	base = streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(dice));
	try
	{
		std::uint64_t build = base;
		state::Fighter fighter = combatData_->makeFighter(*gameData_, templateSid, "", build, instance.templateSid);
		fighter.diceSeed = dice;
		return fighter;
	} catch (const std::exception&) {
		return std::nullopt;
	}
}

bool Simulation::buildMember(flecs::entity member, const state::SquadInstance& instance, state::Position at, std::size_t k, std::size_t n,
	std::int32_t team, std::uint64_t& base, bool world, bool body)
{
	const std::uint64_t id = member.get<state::Id>()->value;
	std::set<std::uint64_t>& skipped = residentData_->skipped;
	const double angle = 2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(n);
	const double ring = n > 1 ? routines::kResidentRing : 0.0;
	const double x = at.x + ring * std::cos(angle);
	const double z = at.z + ring * std::sin(angle);
	std::optional<state::Fighter> built = templateFighter(member, instance, k, base);
	if (!built)
	{
		skipped.insert(id);
		++residentStats_.skippedMembers;
		return false;
	}
	state::Fighter fighter = std::move(*built);
	double y = groundHeight(x, z).value_or(0.0);
	if (navigation_ != nullptr && !body)
	{
		const std::optional<knav::Position> placed = placeOnGround(x, y, z);
		if (!placed)
		{
			skipped.insert(id);
			++residentStats_.skippedMembers;
			return false;
		}
		fighter.x = placed->x;
		fighter.y = placed->y;
		fighter.z = placed->z;
	} else {
		fighter.x = x;
		fighter.y = y;
		fighter.z = z;
	}
	fighter.team = team;
	fighter.playerControlled = false;
	if (instance.faction.id != 0 && member.world().is_alive(instance.faction.id))
		fighter.faction = member.world().entity(instance.faction.id).get<state::Id>()->value;
	if (const flecs::entity squad = member.target<state::MemberOf>(); squad.is_valid() && squad.get<state::Id>() != nullptr)
		fighter.squad = squad.get<state::Id>()->value;
	if (const kdata::Record* record = gameData_->find(instance.templateSid))
		fighter.uniqueSquad = record->type == kworld::itemTypeByName("UNIQUE_SQUAD_TEMPLATE");
	if (world)
	{
		// The live role is Leader for the first member (the squad's leader), else the list the member was spawned from.
		const state::MemberRole* role = member.get<state::MemberRole>();
		fighter.attacksNeutrals = attackGoalWindow(member.target<state::MemberOf>(), k == 0 ? state::kRoleLeader : (role != nullptr ? role->list : state::kRoleSquad),
			fighter.aggroStart, fighter.aggroEnd);
	}
	// What the member kept while its squad was a token (demoteWorldSquad): applied to the fresh Fighter, which stands at the ring with no route,
	// target or task. The record is spent: the Fighter is the member's state again, and the next demotion writes a new one (or none).
	if (const state::MemberDelta* kept = member.get<state::MemberDelta>())
	{
		kept->applyTo(fighter);
		member.remove<state::MemberDelta>();
	}
	adoptFighter(member, std::move(fighter));
	return true;
}

void Simulation::setResidentsArrive(bool walkIn)
{
	arriveUntil_ = walkIn ? clockTick() + routines::kArriveTicks : 0;
}

void Simulation::promoteResidents(flecs::entity squad)
{
	flecs::world& ecs = ecsMutable();
	// A copy: the squad.set<SquadRoutine> below moves the squad to another table, which would leave a reference dangling (the members were
	// then built from another squad's template: a different race and stats at each promotion).
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const state::Position at = *squad.get<state::Position>();
	const std::uint64_t now = clockTick();
	state::Allegiance allegiance;
	if (instance.faction.id != 0 && ecs.is_alive(instance.faction.id))
	{
		if (const state::FactionState* faction = ecs.entity(instance.faction.id).get<state::FactionState>())
			allegiance.factionSid = faction->templateSid;
	}

	// Before the members: the placement pass (T3c) may lock the home's doors and records the flag on the squad.
	squad.set<state::SquadRoutine>(state::SquadRoutine{});
	// A fresh world's start: no placement, the scorer walks the members in from the token (arriveUntil_).
	const bool place = now >= arriveUntil_;
	const auto members = membersOf(squad);
	for (std::size_t k = 0; k < members.size(); ++k)
	{
		const auto& [id, member] = members[k];
		if (member.has<state::Fighter>() && residentData_->adopted.erase(id) != 0)
		{
			// A scenario file's character (scenarioMember): already built, at its saved spot. It gets the Routine a built resident gets, with no
			// placement pass; its team and allegiance are the file's. A caged slave (T3e2) gets none: stepSlaveTrade keeps it in its cage.
			const state::Fighter* built = member.get<state::Fighter>();
			if (!built->dead && !built->playerControlled && built->cageBuilding == 0)
			{
				state::Routine routine;
				routine.since = now;
				routine.nextScore = now;
				routine.lastSleep = trader::gameHours(now) - 24.0
					* RngStream(streamSeed(streamSeed(streamSeed(seed_, kRoutineStream), static_cast<std::size_t>(diceKeyOf(*built, id))), 0)).uniform01();
				member.set<state::Routine>(std::move(routine));
				++residentStats_.promotedMembers;
			}
			continue;
		}
		if (member.has<state::Fighter>() || residentData_->skipped.count(id) != 0 || member.get<state::CharacterInstance>() == nullptr)
			continue;
		// UNSURE: team 0, the player's, so that a resident never attacks the player on sight (combat's enemy test is "different team");
		// the price is that the player cannot attack one with an order either. Hostility by relation is worldTeam (world squads).
		std::uint64_t base = 0;
		if (!buildMember(member, instance, at, k, members.size(), 0, base))
			continue;
		if (!allegiance.factionSid.empty())
			member.set<state::Allegiance>(allegiance);
		state::Routine routine;
		routine.since = now;
		routine.nextScore = now;
		// OURS: the initial tiredness, now minus U(0, 24) h from the keyed hash (TOWN_LIFE_SLICE 5.3).
		routine.lastSleep = trader::gameHours(now) - 24.0 * RngStream(streamSeed(base, 0)).uniform01();
		member.set<state::Routine>(std::move(routine));
		// Seeded placement (5.3): one scorer pass in placement mode puts the member at its winner's site instead of on the ring.
		if (townScorer_ && place)
			scoreMember(member, squad, true);
		++residentStats_.promotedMembers;
	}
	++residentStats_.promotedSquads;
}

bool Simulation::attackGoalWindow(flecs::entity squad, std::uint8_t role, std::int32_t& start, std::int32_t& end)
{
	if (ai_ == nullptr || residentData_ == nullptr)
		return false;
	// One active package per squad (its default one); each character reads ONE role list of it (bq10_goal21.md K4); the lists already
	// carry the fallbacks (Squad <- Leader, Squad2 <- Squad). The window is stored on the Fighter and checked on every evaluation.
	// The registry's pick still, for every squad: the board's differs for 75 roaming / raid / caravan templates (task ds/board-pick moves it).
	const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
	const routines::CompiledPackage* package = instance != nullptr ? residentData_->packages.find(ai_->registry().squad(instance->templateSid).package) : nullptr;
	if (package == nullptr)
		return false;
	for (const routines::GoalRow& row : package->list(static_cast<routines::Role>(std::min<std::uint8_t>(role, 3))))
	{
		if (row.task == routines::task::AttackEnemiesAndNeutrals)
		{
			start = row.start;
			end = row.end;
			return true;
		}
	}
	return false;
}

// BQ13 / BQ14 (join_rules.hpp): every package-driven character picks fights through the attack goals of its own role list. Derived from the squad's one
// active package (as attackGoalWindow), refreshed with the bonded set; a character with no package (a duel, a test, the player's) keeps the sight rule.
void Simulation::refreshJoinGoals(std::uint64_t now)
{
	if (combat_ == nullptr || ai_ == nullptr || residentData_ == nullptr)
		return;
	const double hour = std::fmod(static_cast<double>(now), kTicksPerGameDay) / kTicksPerGameDay * 24.0;
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		const flecs::entity entity = ecs.entity(combatEntities_[i]);
		bool driven = false;
		std::uint16_t goals = 0;
		const flecs::entity squad = members[i].fighter.playerControlled ? flecs::entity() : entity.target<state::MemberOf>();
		const state::SquadInstance* instance = squad.is_valid() ? squad.get<state::SquadInstance>() : nullptr;
		if (instance != nullptr)
		{
			// A scenario file's characters were built without their squad key (the arena gave them a private one): they are squadmates now.
			if (const state::Id* key = squad.get<state::Id>(); key != nullptr && members[i].fighter.squad != key->value)
				combat_->find(members[i].id)->squad = key->value;
			if (members[i].fighter.faction == 0 && instance->faction.id != 0 && ecs.is_alive(instance->faction.id))
				combat_->find(members[i].id)->faction = ecs.entity(instance->faction.id).get<state::Id>()->value;
			// The registry's pick (see attackGoalWindow); the board's differs for 75 templates (task ds/board-pick).
			if (const routines::CompiledPackage* package = residentData_->packages.find(ai_->registry().squad(instance->templateSid).package))
			{
				driven = true;
				for (const routines::GoalRow& row : package->list(liveRole(entity)))
				{
					if (routines::windowHolds(row, hour))
						goals |= joins::bitOfTask(row.task);
				}
			}
		}
		combat_->setJoinGoals(members[i].id, driven, goals);
	}
	joinGoalsTick_ = now;
	joinGoalsCount_ = members.size();
}

void Simulation::promoteWorldSquad(flecs::entity squad)
{
	flecs::world& ecs = ecsMutable();
	if (!squad.is_alive())
		return; // a straggler token that merged into a parent promoted earlier in this pass
	const state::SquadInstance instance = *squad.get<state::SquadInstance>(); // a copy, as in promoteResidents
	const state::Position at = *squad.get<state::Position>();
	state::Allegiance allegiance;
	if (instance.faction.id != 0 && ecs.is_alive(instance.faction.id))
	{
		if (const state::FactionState* faction = ecs.entity(instance.faction.id).get<state::FactionState>())
			allegiance.factionSid = faction->templateSid;
	}
	const std::int32_t team = worldTeam(ecs, *gameData_, instance.faction);

	mergeStragglersNear(squad);
	const auto members = membersOf(squad);
	bool any = false;
	for (std::size_t k = 0; k < members.size(); ++k)
	{
		const auto& [id, member] = members[k];
		if (member.has<state::Fighter>())
		{
			residentData_->adopted.erase(id); // a scenario file's character stays as it was built (scenarioMember)
			any = any || !member.get<state::Fighter>()->dead;
			continue;
		}
		if (residentData_->skipped.count(id) != 0 || member.get<state::CharacterInstance>() == nullptr)
			continue;
		std::uint64_t base = 0;
		if (!buildMember(member, instance, at, k, members.size(), team, base, true))
			continue;
		if (!allegiance.factionSid.empty())
			member.set<state::Allegiance>(allegiance);
		++residentStats_.worldPromotedMembers;
		residentStats_.worldHostileMembers += team != 0 ? 1 : 0;
		any = true;
	}
	if (any)
	{
		residentData_->world.insert(squad.get<state::Id>()->value);
		++residentStats_.worldPromotedSquads;
		logWorldSquadEvent("promote", squad);
	}
}

// The parent promotes: its stragglers within kStragglerMerge of it that are not built themselves rejoin as members.
void Simulation::mergeStragglersNear(flecs::entity parent)
{
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::StragglerOf>() == 0)
		return;
	const state::Position at = *parent.get<state::Position>();
	std::vector<flecs::entity> near;
	ecs.each([&](flecs::entity token, const state::Id& id, const state::StragglerOf& link, const state::Position& where)
	{
		if (link.parent.id == parent.id() && std::hypot(where.x - at.x, where.z - at.z) <= routines::kStragglerMerge
			&& (residentData_ == nullptr || residentData_->world.count(id.value) == 0))
			near.push_back(token);
	});
	for (const flecs::entity token : near)
		mergeStraggler(token, parent);
}

void Simulation::leaveCorpse(flecs::entity squad, flecs::entity member, std::size_t k, state::MemberDelta delta, std::uint8_t cause, const state::Position* where)
{
	flecs::world& ecs = ecsMutable();
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const state::Position at = where != nullptr ? *where : *squad.get<state::Position>();
	delta.mask |= state::MemberDelta::kBody;
	delta.dead = true;
	delta.unconscious = true;
	member.set<state::MemberDelta>(std::move(delta));
	std::uint64_t base = 0;
	if (!buildMember(member, instance, at, k, 1, worldTeam(ecs, *gameData_, instance.faction), base, true, true))
		return; // no body could be built (its template is gone): the dead record stays, and promotion tries again
	if (instance.faction.id != 0 && ecs.is_alive(instance.faction.id))
	{
		if (const state::FactionState* faction = ecs.entity(instance.faction.id).get<state::FactionState>(); faction != nullptr && !faction->templateSid.empty())
			member.set<state::Allegiance>(state::Allegiance{faction->templateSid});
	}
	state::CorpseDecay decay;
	decay.stamp = std::max(gameHoursIn(clockTick()), 1.0e-6); // the death is the first evaluation: it is aged from now
	decay.deathTick = clockTick();
	decay.cause = cause;
	member.set<state::CorpseDecay>(decay);
}

void Simulation::demoteWorldSquad(flecs::entity squad)
{
	logWorldSquadEvent("demote", squad);
	// The token takes the leader's position (the Fighters were the squad; the strategic mover goes on from there).
	bool hasLeader = false;
	double leaderX = 0.0;
	double leaderZ = 0.0;
	if (combat_ != nullptr && regions_ != nullptr)
	{
		const auto members = membersOf(squad);
		if (const state::Fighter* leader = combat_->find(leaderOf(*combat_, members)))
		{
			hasLeader = true;
			leaderX = leader->x;
			leaderZ = leader->z;
			regions_->place(ecsMutable(), squad.get<state::Id>()->value, leader->x, leader->z);
		}
	}
	// A member left apart from the leader (kStragglerRadius) becomes a token of its own, so its medical step and its walk back go on without it being
	// built (k16.6.53). Not a player's squad, not a carrier or the carried (they stay with the squad).
	const bool splits = hasLeader && !squad.has<state::PlayerSquad>() && !squad.has<state::StragglerOf>();
	const state::SquadInstance instance = *squad.get<state::SquadInstance>();
	const auto members = membersOf(squad);
	for (std::size_t k = 0; k < members.size(); ++k)
	{
		const auto& [id, member] = members[k];
		const state::Fighter* fighter = member.get<state::Fighter>();
		// The dead stay as bodies on the map (corpses are k16.6.52); the player's own never go.
		if (fighter == nullptr || fighter->dead || fighter->playerControlled)
			continue;
		// A member on a world task (task BO: capture 167 / loot 177) ends it with the squad; a resident's Routine does not happen here
		// (the squad has no SquadRoutine when this is a world squad), but one is left alone all the same.
		if (member.has<state::Routine>())
		{
			if (squad.has<state::SquadRoutine>())
				continue;
			member.remove<state::Routine>();
		}
		// What the member differs in from its template build is kept (state::MemberDelta); an untouched member stores nothing.
		std::uint64_t base = 0;
		const std::optional<state::Fighter> built = templateFighter(member, instance, k, base);
		// remove<Routine> and templateFighter's MemberSlot set move the member's components to another table: the pointer from before is a moved-from Fighter
		// (empty strings, old numbers), whose delta gave a promoted member a weapon type with no name (k16.5.13).
		fighter = member.get<state::Fighter>();
		const double x = fighter->x;
		const double z = fighter->z;
		// A member that is down splits only when the off-screen medical step runs: without it nothing wakes it, and its token would lie there for ever.
		const bool apart = splits && (!fighter->unconscious || offscreenMedical_) && fighter->carriedBy == 0 && fighter->carrying == 0 && !member.has<state::CarryStep>() && std::hypot(x - leaderX, z - leaderZ) > routines::kStragglerRadius;
		if (std::optional<state::MemberDelta> delta = built ? state::MemberDelta::between(*fighter, *built) : std::nullopt)
			member.set<state::MemberDelta>(std::move(*delta));
		else
			member.remove<state::MemberDelta>();
		removeFighter(id);
		if (apart && member.get<state::MemberSlot>() != nullptr)
			splitStraggler(squad, member, instance, x, z);
		++residentStats_.worldDemotedMembers;
	}
	residentData_->world.erase(squad.get<state::Id>()->value);
	++residentStats_.worldDemotedSquads;
}

// A straggler's token: a squad of the one member, its SquadInstance the parent's (the member's dice key on the parent's content, not on a squad of its
// own), at the spot where the member stood. The region engine and the AI pick it up with the next instance rebuild (removed_).
void Simulation::splitStraggler(flecs::entity parent, flecs::entity member, const state::SquadInstance& instance, double x, double z)
{
	flecs::world& ecs = ecsMutable();
	flecs::entity token = state::createInstance(ecs);
	token.set<state::SquadInstance>(instance);
	token.set<state::Position>(state::Position{x, z});
	token.set<state::StragglerOf>(state::StragglerOf{state::EntityRef{parent.id()}});
	member.remove<state::MemberOf>(flecs::Wildcard);
	member.add<state::MemberOf>(token);
	removed_ = true;
	++residentStats_.stragglerSplits;
}

// The token's member goes back to the parent squad and the token is gone.
void Simulation::mergeStraggler(flecs::entity token, flecs::entity parent)
{
	for (const auto& [id, member] : membersOf(token))
	{
		member.remove<state::MemberOf>(flecs::Wildcard);
		member.add<state::MemberOf>(parent);
	}
	token.destruct();
	removed_ = true;
	++residentStats_.stragglerMerges;
}

// A straggler token walks to its parent while its member is conscious (a stored one: no body record saying it is down or dead; a built one: its Fighter),
// and merges when within kStragglerMerge of the parent. A token whose parent is gone stays a lone squad. OURS / UNSURE (deferred: playtesting): a
// straight line at the token speed (no route, no door); a token that is built (its member promoted) does not walk, it merges only when the parent
// promotes near it.
void Simulation::stepStragglers()
{
	if (regions_ == nullptr || clockTick() % kStragglerTicks != 0)
		return;
	flecs::world& ecs = ecsMutable();
	if (ecs.count<state::StragglerOf>() == 0)
		return;
	std::map<std::uint64_t, std::pair<flecs::entity, flecs::entity>> tokens; // by Id: a fixed order
	ecs.each([&](flecs::entity token, const state::Id& id, const state::StragglerOf& link, const state::Position&)
	{
		tokens.emplace(id.value, std::pair{token, link.parent.id != 0 && ecs.is_alive(link.parent.id) ? ecs.entity(link.parent.id) : flecs::entity()});
	});
	const double step = UtilityGroupLayer::kTokenTravelSpeed * static_cast<double>(kStragglerTicks) * kTickSeconds;
	for (const auto& [id, pair] : tokens)
	{
		const auto& [token, parent] = pair;
		if (!parent.is_valid() || parent.get<state::Position>() == nullptr || parent.get<state::SquadInstance>() == nullptr)
		{
			token.remove<state::StragglerOf>();
			continue;
		}
		const state::Position at = *token.get<state::Position>();
		const state::Position to = *parent.get<state::Position>();
		const double away = std::hypot(to.x - at.x, to.z - at.z);
		if (residentData_ != nullptr && residentData_->world.count(id) != 0)
			continue; // built: the member stands where it is
		if (away <= routines::kStragglerMerge)
		{
			mergeStraggler(token, parent);
			continue;
		}
		bool conscious = false;
		for (const auto& [memberId, member] : membersOf(token))
		{
			const state::MemberDelta* record = member.get<state::MemberDelta>();
			conscious = conscious || (member.get<state::CharacterInstance>() != nullptr && !member.has<state::CorpseDecay>()
				&& (record == nullptr || (record->mask & state::MemberDelta::kBody) == 0 || (!record->dead && !record->unconscious)));
		}
		if (conscious)
		{
			const double along = std::min(step, away) / away;
			regions_->place(ecs, id, at.x + (to.x - at.x) * along, at.z + (to.z - at.z) * along);
		}
	}
}

void Simulation::forSquadsInSlice(std::uint64_t now, const std::function<void(flecs::entity, const state::Id&, const state::SquadInstance&)>& visit, bool all)
{
	flecs::world& ecs = ecsMutable();
	if (ai_ == nullptr)
	{
		std::vector<std::pair<std::uint64_t, flecs::entity>> due;
		ecs.each([&](flecs::entity squad, const state::Id& id, const state::SquadInstance&)
		{
			if (all || inSlice(now, id.value))
				due.emplace_back(id.value, squad);
		});
		std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		for (const auto& [id, squad] : due)
			visit(squad, *squad.get<state::Id>(), *squad.get<state::SquadInstance>());
		return;
	}
	for (const auto& [squadId, raw] : all ? ai_->squadIndex(ecs, now) : ai_->squadSlice(ecs, now))
	{
		if (!ecs.is_alive(raw))
			continue;
		const flecs::entity squad = ecs.entity(raw);
		const state::Id* id = squad.get<state::Id>();
		const state::SquadInstance* instance = squad.get<state::SquadInstance>();
		if (id != nullptr && instance != nullptr)
			visit(squad, *id, *instance);
	}
}

void Simulation::prepareWorldCarry()
{
	ai_->carried().clear();
	knowWorldSquads();
	if (combat_ == nullptr || residentData_ == nullptr)
		return;
	residentData_->carried.clear();
	if (residentData_->world.empty())
		return;
	flecs::world& ecs = ecsMutable();
	const std::map<std::uint64_t, std::uint64_t> targetedByPlayer = targetedByPlayers(*combat_);
	std::vector<std::tuple<std::uint64_t, double, double>> moves;
	const std::uint64_t now = clockTick();
	// Only the squads whose second-slice is this tick (id % kSquadIntervalTicks, k16.6.232): each squad is still prepared, stepped and
	// carried once a second, but not all on one tick.
	forSquadsInSlice(now, [&](flecs::entity squad, const state::Id& id, const state::SquadInstance&)
	{
		if (residentData_->world.count(id.value) == 0 || squad.has<state::GateCrew>() || squad.get<state::GroupAI>() == nullptr)
			return;
		const state::Position* position = squad.get<state::Position>();
		const auto members = membersOf(squad);
		const std::uint64_t leaderId = leaderOf(*combat_, members);
		const state::Fighter* leader = combat_->find(leaderId);
		if (position == nullptr || leader == nullptr)
			return;
		const bool holding = squadFighting(*combat_, targetedByPlayer, members);
		double slaveGap = 0.0;
		if (const auto board = boards_.find(id.value); board != boards_.end())
		{
			for (const std::uint64_t slaveId : board->second.owned)
			{
				const state::Fighter* slave = combat_->find(slaveId);
				if (slave != nullptr && !slave->dead && !slave->down && !slave->unconscious && slave->carriedBy == 0 && slave->followTarget != 0)
					slaveGap = std::max(slaveGap, std::hypot(slave->x - leader->x, slave->z - leader->z));
			}
		}
		ai_->carried()[id.value] = AiSystem::Carry{leader->x, leader->z, holding, slaveGap};
		residentData_->carried.push_back({id.value, squad, leaderId, holding});
		if (position->x != leader->x || position->z != leader->z)
			moves.emplace_back(id.value, leader->x, leader->z);
	});
	// The token's Position is written outside the query.
	for (const auto& [id, x, z] : moves)
		regions_->place(ecs, id, x, z);
}

void Simulation::stepWorldSquads()
{
	if (combat_ == nullptr || residentData_ == nullptr)
		return;
	for (const ResidentData::Carried& carried : residentData_->carried)
	{
		if (carried.holding || !carried.entity.is_alive())
			continue;
		const state::GroupAI* group = carried.entity.get<state::GroupAI>();
		state::Fighter* leader = combat_->find(carried.leader);
		if (group == nullptr || leader == nullptr)
			continue;
		// The others follow the leader (the follow task, SQUAD_FOLLOW.md s4; it ends by itself when the leader is out of sight for long, so
		// it is given again). A member that is fighting, dead, or knocked out is left alone. Without a navmesh the follow command is refused.
		if (navigation_ != nullptr)
		{
			for (const auto& [id, member] : membersOf(carried.entity))
			{
				const state::Fighter* f = combat_->find(id);
				// The fallback "stay at home" (43) a member took while the squad stood in a town is not a task: when the squad walks on it gives
				// way to the follow (k16.6.21: a caravan leaving for its depot left every member standing in the Hub).
				if (group->goal == state::GoalKind::TravelTo && !group->route.empty() && f != nullptr && member.has<state::Routine>()
					&& f->carrying == 0 && member.get<state::Routine>()->goal == routines::task::StayInHome) // a carrier keeps its escort (k16.6.3)
				{
					routineBook_.release(id);
					member.remove<state::Routine>();
				}
				// A carrier whose Routine holds no goal (the chain's captor took the lift and nothing was scored since: kenshi-k16.7.36, a
				// caravan whose carrier was not the first chainer stood where the squad left it) is not busy: it follows like the others.
				const state::Routine* routine = member.get<state::Routine>();
				const bool busy = routine != nullptr && !(f != nullptr && f->carrying != 0 && routine->goal == 0);
				if (id == carried.leader || f == nullptr || f->dead || f->unconscious || f->playerControlled || busy
					|| f->target != 0 || f->followTarget == carried.leader || f->goal.kind != state::IndividualKind::None
					|| (member.has<state::CarryStep>() && member.get<state::CarryStep>()->phase != 1) // a lift or put-down runs: a follow would cancel it
					|| (f->slaveState == 1 && f->followTarget != 0)) // a slave follows its captor (141 / 171, world_tasks.cpp)
					continue;
				apply(Command::fighterFollow(state::Id{id}, state::Id{carried.leader}));
				if (state::Fighter* follower = combat_->find(id))
					follower->followSource = state::Fighter::PlanSource::Plan; // the squad plan, not an order (task fight-goals)
			}
		}
		// The leader walks to the carrot on the group's route (the group step decides where; this carries it out).
		double toX = 0.0;
		double toZ = 0.0;
		if (group->goal == state::GoalKind::Idle || !AiSystem::carrot(*group, kCarryLookahead, toX, toZ))
			continue;
		if (leader->target != 0 || leader->followTarget != 0 || leader->goal.kind != state::IndividualKind::None)
			continue;
		// The route is re-issued every step, also while the leader still walks the old one. Keeping the old route while its end was
		// near the new carrot let the leader finish it and stand with nothing to walk until the next step: the 1 Hz stop-start patrol (0.4 s
		// of motion, 0.6 s still). At the group's pace the leader cannot reach the next carrot before the next step, so it walks on.
		std::vector<state::RoutePoint> points;
		bool approach = false;
		if (navigation_ != nullptr)
		{
			// The carrot is a point on the group's route, which need not be walkable (a straight order through a wall, a point over a cliff):
			// its height is the mesh surface nearest the leader's level there (the terrain can lie 150 below a deck, and the 3D snap
			// would pick a disconnected island under it), and when it cannot be reached or the nearest mesh point is where the leader
			// already stands, the carrot further along the route is tried (the way round the obstacle), up to the route's end.
			Route route;
			double carrotY = leader->y;
			bool away = false; // some carrot on the route is more than 4 units off (else the leader is at the route's end)
			for (const double lookahead : {kCarryLookahead, 2.0 * kCarryLookahead, 4.0 * kCarryLookahead, 8.0 * kCarryLookahead, 16.0 * kCarryLookahead})
			{
				if (!AiSystem::carrot(*group, lookahead, toX, toZ) || std::hypot(leader->x - toX, leader->z - toZ) <= 4.0)
					continue;
				away = true;
				carrotY = navigation_->walker().surfaceY(toX, leader->y, toZ, kCarryReachY).value_or(leader->y);
				route = navigation_->routeFrom({leader->x, leader->y, leader->z}, toX, toZ, carrotY, true, body(*leader));
				if (!route.found)
					route = navigation_->routeFrom({leader->x, leader->y, leader->z}, toX, toZ, std::nullopt, true,
						body(*leader)); // any height counts
				if (route.found && route.approach && !route.points.empty())
				{
					// An approach route ends at the mesh point nearest the carrot the walker can reach: where the leader stands, or a point
					// no closer to the carrot than the leader already is (a gap or a wall between them). Taking it gains nothing: the leader
					// would walk into the same edge, block and be re-routed every second. Try the carrot further along the group's route.
					const double endToCarrot = std::hypot(toX - route.points.back().x, toZ - route.points.back().z);
					const double hereToCarrot = std::hypot(toX - leader->x, toZ - leader->z);
					if (std::hypot(route.points.back().x - leader->x, route.points.back().z - leader->z) <= 4.0 || endToCarrot >= hereToCarrot)
						route = Route{};
				}
				if (route.found && !route.points.empty())
					break;
			}
			if (!away)
				continue;
			std::uint32_t& waited = residentData_->carriedNoRoute[carried.leader];
			if (!route.found || route.points.empty())
			{
				// No carrot on the whole route can be reached: it must not wait for ever (no terminal state). After kCarryNoRouteTicks the
				// leader is put on the mesh at the route's far end; counted (ResidentStats::leaderWarps) and logged.
				if (++waited < kCarryNoRouteTicks)
					continue;
				waited = 0;
				AiSystem::carrot(*group, group->length, toX, toZ);
				if (const std::optional<knav::Position> spot = navigation_->walker().snap({toX, carrotY, toZ}, kCarryReachY * 2.0))
				{
					std::cerr << "leader " << carried.leader << " could not route along its group route for " << kCarryNoRouteTicks / 20 << " s from "
						<< leader->x << "," << leader->z << ": placed at its end " << spot->x << "," << spot->z << "\n";
					leader->clearRoute();
					leader->forwardSpeed = 0.0;
					leader->x = spot->x;
					leader->y = spot->y;
					leader->z = spot->z;
					++residentStats_.leaderWarps;
					mirrorFighter(carried.leader);
				}
				continue;
			}
			waited = 0;
			for (std::size_t p = 0; p < route.points.size(); ++p)
				points.push_back({route.points[p].x, route.heights[p], route.points[p].z});
			approach = route.approach;
		} else {
			points = {{leader->x, leader->y, leader->z}, {toX, leader->y, toZ}};
		}
		leader->clearRoute();
		leader->route = std::move(points);
		leader->routeNext = 0;
		++leader->routeVersion;
		leader->routeSeq = 0;
		leader->routeChase = false;
		leader->routeApproach = approach;
		leader->routeSource = state::Fighter::PlanSource::Plan;
		leader->routeTries = 0;
		mirrorFighter(carried.leader);
	}
}

void Simulation::demoteResidents(flecs::entity squad)
{
	for (const auto& [id, member] : membersOf(squad))
	{
		if (!member.has<state::Routine>())
			continue;
		routineBook_.release(id);
		member.remove<state::Routine>();
		const state::Fighter* fighter = combat_ != nullptr ? combat_->find(id) : nullptr;
		if (fighter != nullptr && !fighter->dead && !fighter->playerControlled)
		{
			removeFighter(id);
			++residentStats_.demotedMembers;
		}
	}
	routineBook_.release(squad.get<state::Id>()->value); // the squad's table
	squad.remove<state::SquadRoutine>();
	++residentStats_.demotedSquads;
}

void Simulation::dropGoneFighters()
{
	if (combat_ == nullptr || combat_->empty())
		return;
	flecs::world& ecs = ecsMutable();
	for (std::size_t i = combatEntities_.size(); i-- > 0;)
	{
		if (ecs.is_alive(combatEntities_[i]))
			continue;
		const std::uint64_t id = combat_->members()[i].id;
		routineBook_.release(id);
		// Whoever was lifting it or being lifted by it is let go; the carry step of the survivor ends on its own.
		for (CombatArena::Member& other : combat_->membersMutable())
		{
			if (other.fighter.carriedBy == id)
			{
				other.fighter.carriedBy = 0;
				other.fighter.captive = false;
			}
			if (other.fighter.carrying == id)
				other.fighter.carrying = 0;
		}
		combat_->remove(id);
		combatEntities_.erase(combatEntities_.begin() + static_cast<std::ptrdiff_t>(i));
	}
}

std::size_t Simulation::rebuildRoutineBook()
{
	routineBook_.clear();
	flecs::world& ecs = ecsMutable();
	std::vector<std::tuple<std::uint64_t, routines::Site, std::uint64_t>> claims;
	ecs.each([&](const state::Id& id, const state::Routine& routine)
	{
		if (routine.target.kind != 0)
			claims.emplace_back(id.value, fromState(routine.target), routine.since);
	});
	ecs.each([&](const state::Id& id, const state::SquadRoutine& squad)
	{
		if (squad.barTable != 0)
			claims.emplace_back(id.value, routines::Site{routines::SiteKind::Table, squad.barTable, 0}, 0);
	});
	std::sort(claims.begin(), claims.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });
	std::size_t lost = 0;
	for (const auto& [owner, site, since] : claims)
		lost += routineBook_.adopt(site, owner, since) ? 0 : 1;
	return lost;
}

} // namespace ksim
