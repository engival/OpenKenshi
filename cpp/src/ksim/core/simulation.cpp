#include "ksim/core/simulation.hpp"
#if KSIM_PROFILE
#include "ksim/nav/navigator.hpp"
#endif

#include "ksim/economy/economy_script.hpp"
#include "ksim/world/squad_alive.hpp"
#include "ksim/world/buildings.hpp"
#include "ksim/ai/gatebind.hpp"
#include "ksim/ai/perception.hpp"
#include "ksim/economy/inventory_grid.hpp"
#include "ksim/nav/los.hpp"

#include "ksim/world/factions.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/nav/placement.hpp"
#include "ksim/economy/trader.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ksim {

namespace {

constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

Subsystem subsystemFor(DetailLevel level) noexcept
{
	switch (level)
	{
	case DetailLevel::Full:
		return Subsystem::FullDetail;
	case DetailLevel::Squad:
		return Subsystem::SquadDetail;
	case DetailLevel::Strategic:
		return Subsystem::Strategic;
	}
	return Subsystem::Strategic;
}

World buildWorld(const kdata::GameData& gameData, std::string_view dataDir,
	const kworld::LoadOrder& order)
{
	std::vector<std::string> names;
	for (const kworld::LoadOrderEntry& entry : order.files)
		names.push_back(entry.name);
	return World::build(gameData, dataDir, std::move(names));
}

// The FactionState instance with this Id; a null entity when there is none.
flecs::entity factionInstance(flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found{};
	ecs.each([&found, id](flecs::entity entity, const state::Id& key,
		const state::FactionState&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

// The squad entity with this state::Id; a null entity when there is none.
flecs::entity squadInstance(flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found{};
	ecs.each([&found, id](flecs::entity entity, const state::Id& key,
		const state::SquadInstance&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

flecs::entity townInstance(flecs::world& ecs, std::uint64_t id)
{
	flecs::entity found{};
	ecs.each([&found, id](flecs::entity entity, const state::Id& key, const state::TownState&)
	{
		if (key.value == id)
			found = entity;
	});
	return found;
}

// Removes a squad and its living members. A dead member is a body, not a member: it stays in the world with its inventory (lootable),
// only leaving the squad that is going.
void removeSquadEntity(flecs::world& ecs, flecs::entity squad)
{
	std::vector<flecs::entity> bodies;
	ecs.query_builder<const state::Fighter>().with<state::MemberOf>(squad).build().each([&](flecs::entity member, const state::Fighter& fighter)
	{
		if (fighter.dead)
			bodies.push_back(member);
	});
	for (flecs::entity body : bodies)
		body.remove<state::MemberOf>(flecs::Wildcard);
	ecs.delete_with<state::MemberOf>(squad.id());
	squad.destruct();
}

} // namespace

Command Command::setFactionRelation(state::Id from, state::Id to, std::int32_t value)
{
	Command command;
	command.type = CommandType::SetFactionRelation;
	command.subject = from.value;
	command.object = to.value;
	command.value = value;
	return command;
}

Command Command::setPlayerAreas(std::vector<state::GroundPoint> centres)
{
	Command command;
	command.type = CommandType::SetPlayerAreas;
	command.points = std::move(centres);
	return command;
}

Command Command::setTownOwner(state::Id town, state::Id faction)
{
	Command command;
	command.type = CommandType::SetTownOwner;
	command.subject = town.value;
	command.object = faction.value;
	return command;
}

Command Command::setDoorState(std::uint64_t doorKey, std::int32_t state)
{
	Command command;
	command.type = CommandType::SetDoorState;
	command.object = doorKey;
	command.value = state;
	return command;
}

Command Command::removeSquad(state::Id squad, state::Id killerFaction)
{
	Command command;
	command.type = CommandType::RemoveSquad;
	command.subject = squad.value;
	command.object = killerFaction.value;
	return command;
}

Command Command::setSquadCentre(std::int32_t group, double x, double z)
{
	Command command;
	command.type = CommandType::SetSquadCentre;
	command.value = group;
	command.x = x;
	command.z = z;
	return command;
}

Command Command::fighterMoveTo(state::Id subject, std::vector<state::GroundPoint> route, std::vector<double> heights,
	std::uint32_t clientSeq, bool queue, bool approach)
{
	Command command;
	command.type = CommandType::FighterMoveTo;
	command.subject = subject.value;
	if (!route.empty())
	{
		command.x = route.back().x;
		command.z = route.back().z;
	}
	command.points = std::move(route);
	command.heights = std::move(heights);
	command.object = clientSeq;
	command.value = (queue ? 1 : 0) | (approach ? 2 : 0); // bit 0 queue, bit 1 approach point
	return command;
}

Command Command::fighterMoveFar(state::Id subject, double goalX, double goalZ, double goalY, std::uint32_t clientSeq, bool queue)
{
	Command command = fighterMoveTo(subject, {{goalX, goalZ}}, {goalY}, clientSeq, queue, false);
	command.value |= 4; // bit 2 far: the one point is the true goal, the route is planned when the order applies
	return command;
}

Command Command::fighterDrive(state::Id subject, std::int32_t mode, double heading, std::uint32_t clientSeq, int strafe)
{
	Command command;
	command.type = CommandType::FighterDrive;
	command.subject = subject.value;
	command.value = mode;
	command.x = heading;
	command.z = strafe;
	command.object = clientSeq;
	return command;
}

Command Command::fighterAttack(state::Id subject, state::Id target)
{
	Command command;
	command.type = CommandType::FighterAttack;
	command.subject = subject.value;
	command.object = target.value;
	return command;
}

Command Command::fighterStop(state::Id subject, bool hold)
{
	Command command;
	command.type = CommandType::FighterStop;
	command.subject = subject.value;
	command.value = hold ? 1 : 0;
	return command;
}

Command Command::fighterBlock(state::Id subject, bool on)
{
	Command command;
	command.type = CommandType::FighterBlock;
	command.subject = subject.value;
	command.value = on ? 1 : 0;
	return command;
}

Command Command::fighterFollow(state::Id subject, state::Id leader)
{
	Command command;
	command.type = CommandType::FighterFollow;
	command.subject = subject.value;
	command.object = leader.value;
	return command;
}

Command Command::fighterPassive(state::Id subject, bool on)
{
	Command command;
	command.type = CommandType::FighterPassive;
	command.subject = subject.value;
	command.value = on ? 1 : 0;
	return command;
}

Command Command::fighterHoldPosition(state::Id subject, bool on)
{
	Command command;
	command.type = CommandType::FighterHoldPosition;
	command.subject = subject.value;
	command.value = on ? 1 : 0;
	return command;
}

Command Command::fighterSneak(state::Id subject, bool on)
{
	Command command;
	command.type = CommandType::FighterSneak;
	command.subject = subject.value;
	command.value = on ? 1 : 0;
	return command;
}

Command Command::fighterInteract(state::Id subject, std::uint64_t target, std::uint8_t action, std::uint32_t clientSeq,
	std::vector<state::GroundPoint> route, std::vector<double> heights, bool approach, bool queue, std::uint8_t flags)
{
	Command command;
	command.type = CommandType::FighterInteract;
	command.subject = subject.value;
	command.object = target;
	command.value = action;
	command.x = static_cast<double>(clientSeq);
	command.z = (approach ? 1.0 : 0.0) + (queue ? 2.0 : 0.0) + 4.0 * flags; // bit 0 approach point, bit 1 queued, the rest order 60's flags
	command.points = std::move(route);
	command.heights = std::move(heights);
	return command;
}

Command Command::fighterRun(state::Id subject, bool run)
{
	Command command;
	command.type = CommandType::FighterRun;
	command.subject = subject.value;
	command.value = run ? 1 : 0;
	return command;
}

Command Command::fighterSpeedMode(state::Id subject, std::uint8_t mode, std::uint64_t group)
{
	Command command;
	command.type = CommandType::FighterSpeedMode;
	command.subject = subject.value;
	command.value = mode;
	command.object = group;
	return command;
}

Command Command::fighterDrop(state::Id subject, std::string item, std::uint32_t count, std::uint32_t clientSeq)
{
	Command command;
	command.type = CommandType::FighterDrop;
	command.subject = subject.value;
	command.object = clientSeq;
	command.value = static_cast<std::int32_t>(count);
	command.text = std::move(item);
	return command;
}

Command Command::fighterEquip(state::Id subject, std::string item, bool equip, std::uint32_t clientSeq)
{
	Command command;
	command.type = CommandType::FighterEquip;
	command.subject = subject.value;
	command.object = clientSeq;
	command.value = equip ? 1 : 0;
	command.text = std::move(item);
	return command;
}

Command Command::containerTransfer(state::Id subject, std::uint64_t container, std::string item, std::uint32_t count, bool take,
	std::uint32_t clientSeq)
{
	Command command;
	command.type = CommandType::ContainerTransfer;
	command.subject = subject.value;
	command.object = container;
	command.value = static_cast<std::int32_t>(count);
	command.x = static_cast<double>(clientSeq);
	command.z = take ? 1.0 : 0.0;
	command.text = std::move(item);
	return command;
}

Command Command::inventoryOrder(const InventoryOrder& order)
{
	Command command;
	command.type = CommandType::InventoryOrder;
	command.subject = order.id;
	command.object = order.container;
	command.value = order.kind;
	command.heights = {static_cast<double>(order.version), static_cast<double>(order.index), static_cast<double>(order.count),
		static_cast<double>(order.section), static_cast<double>(order.x), static_cast<double>(order.y), static_cast<double>(order.flag)};
	command.text = order.trader != 0 || order.source != 0 || order.window != 0
		? std::to_string(order.trader) + ":" + std::to_string(order.source) + ":" + std::to_string(order.window) : std::string();
	return command;
}

Command Command::squadOrder(const SquadOrder& order)
{
	Command command;
	command.type = CommandType::SquadOrder;
	command.subject = order.kind == 73 || order.kind == 75 ? order.character : order.squad;
	command.object = order.kind == 73 ? order.squad : 0;
	command.value = order.kind;
	command.x = order.index;
	command.z = order.swap;
	command.text = order.name;
	return command;
}

Command Command::jobOrder(const JobOrder& order)
{
	Command command;
	command.type = CommandType::JobOrder;
	command.subject = order.character;
	command.object = order.target;
	command.value = order.kind;
	command.x = order.kind == 76 ? order.type : order.kind == 10 ? order.enabled : order.jobId;
	command.z = order.index;
	return command;
}

Command Command::prospect(state::Id subject)
{
	Command command;
	command.type = CommandType::Prospect;
	command.subject = subject.value;
	return command;
}

Command Command::stealthKo(state::Id subject, state::Id victim)
{
	Command command;
	command.type = CommandType::StealthKo;
	command.subject = subject.value;
	command.object = victim.value;
	return command;
}

Command Command::shackleAction(state::Id subject, state::Id wearer, std::uint8_t action)
{
	Command command;
	command.type = CommandType::ShackleAction;
	command.subject = subject.value;
	command.object = wearer.value;
	command.value = action;
	return command;
}

Command Command::buildPlace(const BuildOrder& order)
{
	Command command;
	command.type = CommandType::BuildPlace;
	command.text = order.record;
	command.x = order.x;
	command.z = order.z;
	command.heights = {order.y, order.qw, order.qx, order.qy, order.qz};
	command.value = order.floor;
	return command;
}

Command Command::buildPause(std::uint64_t building, bool paused)
{
	Command command;
	command.type = CommandType::BuildPause;
	command.subject = building;
	command.value = paused ? 1 : 0;
	return command;
}

Command Command::dialogueReply(state::Id character, state::Id speaker, std::uint32_t menuSeq, std::uint32_t option)
{
	Command command;
	command.type = CommandType::DialogueReply;
	command.subject = character.value;
	command.object = speaker.value;
	command.value = static_cast<std::int32_t>(option);
	command.x = static_cast<double>(menuSeq);
	return command;
}

Command Command::endConversation(state::Id character, state::Id speaker, std::uint8_t reason)
{
	Command command;
	command.type = CommandType::EndConversation;
	command.subject = character.value;
	command.object = speaker.value;
	command.value = reason;
	return command;
}

Command Command::changeSquad(state::Id character, std::uint64_t squad)
{
	Command command;
	command.type = CommandType::ChangeSquad;
	command.subject = character.value;
	command.object = squad;
	return command;
}

Command Command::newGame(const std::string& startoff, const std::string& name, std::uint64_t ticket,
	const std::string& specs, const std::string& options)
{
	Command command;
	command.type = CommandType::NewGame;
	command.object = ticket;
	command.text = startoff + '\n' + name;
	// The options section is marked: it may come without Specs, and a name has no newline (bad_name).
	if (!options.empty())
		command.text += "\nopt:" + options;
	if (!specs.empty())
		command.text += '\n' + specs;
	return command;
}

Command Command::setView(double x, double z)
{
	Command command;
	command.type = CommandType::SetView;
	command.x = x;
	command.z = z;
	return command;
}

Simulation::Simulation(World world, std::uint64_t seed, const RegionConfig& regions)
	: world_(std::move(world))
	, seed_(seed)
	, rng_{
		RngStream(streamSeed(seed, 0)),
		RngStream(streamSeed(seed, 1)),
		RngStream(streamSeed(seed, 2)),
	}
	, systems_{
		SystemState{"full_detail", DetailLevel::Full, kFullIntervalTicks},
		SystemState{"squad_detail", DetailLevel::Squad, kSquadIntervalTicks},
		SystemState{"strategic_detail", DetailLevel::Strategic,
			kStrategicIntervalTicks},
	}
{
	flecs::world& ecs = ecsMutable();
	state::registerComponents(ecs);
	registerSquadCaches();
	adoptScheduler();
	regions_ = std::make_unique<RegionEngine>(seed_, regions);
	regions_->rebuild(ecs);
}

void Simulation::adoptScheduler()
{
	if (const state::Scheduler* saved = world_.ecs().get<state::Scheduler>())
	{
		if (saved->subsystems.size() != kSubsystemCount)
			throw SaveError("saved scheduler has " + std::to_string(saved->subsystems.size())
				+ " subsystems, this build " + std::to_string(kSubsystemCount));
		seed_ = saved->seed;
		for (std::size_t i = 0; i < kSubsystemCount; ++i)
		{
			const state::SubsystemState& subsystem = saved->subsystems[i];
			rng_[i] = RngStream(subsystem.rng);
			systems_[i].runs = subsystem.runs;
			systems_[i].checksum = subsystem.checksum;
			systems_[i].lastRunTick = subsystem.lastRunTick;
		}
	}
	mirrorScheduler();
}

std::size_t Simulation::worldRuntimeEntries() const
{
	return worldTaskBlocks_.size() + carryIdleSince_.size() + eatenBites_.size() + eatenQueue_.size() + eatenUntil_.size() + tortureStrip_.size()
		+ lockOrders_.size() + buildOrders_.size() + arrestOrders_.size() + smashOrders_.size() + cropOrders_.size() + standingRun_.size()
		+ limbCooldown_.size() + limbPlay_.size() + alarmDwell_.size() + announced_.size() + fightPairs_.size() + memberScores_.size()
		+ occupiers_.size() + raidLeaders_.size() + producerOffMesh_.size() + talkSeqs_.size() + buyback_.size() + (dialogue_ != nullptr ? dialogue_->worldEntries() : 0);
}

bool Simulation::holdsOrder(std::uint64_t id) const
{
	return lockOrders_.count(id) != 0 || buildOrders_.count(id) != 0 || arrestOrders_.count(id) != 0 || smashOrders_.count(id) != 0
		|| cropOrders_.count(id) != 0 || standingRun_.count(id) != 0;
}

SaveLoadResult Simulation::reload(std::istream& in, bool force)
{
	const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	// A scratch world first: a save that cannot be read leaves this world untouched.
	{
		std::istringstream probe(bytes);
		flecs::world scratch;
		readSave(probe, scratch, world_.loadOrder(), force);
		const state::Scheduler* scheduler = scratch.get<state::Scheduler>();
		if (scheduler != nullptr && scheduler->subsystems.size() != kSubsystemCount)
			throw SaveError("saved scheduler has " + std::to_string(scheduler->subsystems.size())
				+ " subsystems, this build " + std::to_string(kSubsystemCount));
	}
	flecs::world& ecs = ecsMutable();
	ecs.delete_with<state::Id>(); // every instance; the templates stay
	std::istringstream again(bytes);
	SaveLoadResult loaded = readSave(again, ecs, world_.loadOrder(), force);
	pending_ = std::move(loaded.pending);
	adoptScheduler();
	enterable_.clear();
	enterableValid_ = false;
	removed_ = false;
	scenarioHold_.reset(); // the hold belongs to the world that was replaced (task BM): its squad ids are gone with it
	packsDirty_ = true; // a loaded game looks for squads without their pack animals' backpacks once
	dialogueCounts_.clear();
	// Everything derived and never saved (the stamps, order maps and memos of the world that was replaced, WorldRuntime) starts over, as a
	// fresh process that loads the save has it; the reloaded world reuses the old ids and clock (kenshi-k16.6.65).
	static_cast<WorldRuntime&>(*this) = WorldRuntime();
	if (dialogue_ != nullptr)
		dialogue_->resetWorld();
	// The resident pass's derived state (the built world squads, the carried set, the skipped members, the site index) belongs to the world
	// that was replaced: kept, a second NEW_GAME (new_world, then NEW_GAME) saw its squads as already built and promoted none. The
	// world-squad log's next line is a tick of the old clock.
	residentData_.reset();
	homeSeatsKnown_ = false;
	worldSquadLogNext_ = 0;
	const RegionConfig regionConfig = regions_->config();
	regions_ = std::make_unique<RegionEngine>(seed_, regionConfig);
	regions_->rebuild(ecs);
	syncLineOfSight(); // the new region grid's Full set: drop the trees of the old one (the hook follows)
	if (combat_ != nullptr)
		enableCombat(combatData_, combatSettings_);
	if (ai_ != nullptr)
	{
		AiSystem::Config config = ai_->config();
		ai_.reset();
		enableAi(std::move(config));
	}
	if (ecs.get<state::WeatherRegions>() == nullptr)
	{
		weather_.reset();
		beams_.reset();
	} else if (weather_ != nullptr) {
		weather_->attach(ecs, seed_);
		beams_ = std::make_shared<BeamField>(weather_, seed_); // re-derives the beams at the next step
	}
	else if (gameData_ != nullptr)
		enableWeather(*gameData_);
	if (gameData_ != nullptr && ecs.get<state::WildernessState>() != nullptr)
		wild_ = std::make_unique<WildernessSpawner>(*gameData_, world_.dataDir());
	else
		wild_.reset();
	stepUnlocks(); // a save with no completed research gets `_Default Start` before the first snapshot
	furnishedZones_.clear(); // the entities are the save's: the covered zones are checked again, nothing is spawned twice
	syncNavCoverage();
	furnishNavCoverage();
	restoreBoards(); // the old world's Boards are gone with its squads
	ensureBlackboards();
	return loaded;
}

Simulation Simulation::load(std::string_view dataDir, std::uint64_t seed,
	const RegionConfig& regions)
{
	return Simulation(World::load(dataDir), seed, regions);
}

static std::uint64_t settleTicksOf(double seconds)
{
	return seconds > 0.0 ? static_cast<std::uint64_t>(std::llround(seconds / kTickSeconds)) : 0;
}

Simulation Simulation::newGame(std::string_view dataDir, const kworld::LoadOrder& order,
	const SpawnOptions& options, SpawnReport* report, const RegionConfig& regions)
{
	auto gameData = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
	World world = buildWorld(*gameData, dataDir, order);
	flecs::world& ecs = world.ecsMutable();
	// The world starts the settle earlier (TOWN_LIFE_SLICE 5.40), so a NEW_GAME applied on its first tick reaches the first frame at the start
	// time with the clock never having gone backwards.
	SpawnOptions built = options;
	built.startTick -= std::min<std::uint64_t>(settleTicksOf(options.settleSeconds), built.startTick);
	SpawnReport spawned = spawnDefaultWorld(ecs, *gameData,
		kworld::Towns::join(*gameData, dataDir), built, &world.gateSites(), &world.buildingSites());
	std::unique_ptr<WildernessSpawner> spawner;
	if (options.wildernessByDensity)
	{
		// The one-squad-per-template wilderness gives way to the spawner's
		// density instances; faction roaming squads stay persistent.
		std::vector<flecs::entity> persistent;
		ecs.each([&](flecs::entity entity, const state::SquadInstance& squad)
		{
			if (squad.source == state::SpawnSource::BiomeHomeless
				|| squad.source == state::SpawnSource::BiomeNest)
				persistent.push_back(entity);
		});
		for (const flecs::entity entity : persistent)
			removeSquadEntity(ecs, entity);
		spawned.squads -= persistent.size();
		spawner = std::make_unique<WildernessSpawner>(*gameData, dataDir);
	}
	placeWilderness(ecs, *gameData, dataDir, options.seed);
	if (spawner != nullptr)
	{
		const WildernessReport wild = spawner->seed(ecs, options.seed,
			options.wildernessPopulation, options.nestMultiplier);
		spawned.squads += wild.squads;
		spawned.characters += wild.characters;
	}
	if (report != nullptr)
		*report = spawned;
	Simulation simulation(std::move(world), options.seed, regions);
	simulation.gameData_ = gameData;
	simulation.enableWeather(*gameData);
	if (spawner != nullptr)
		simulation.attachWilderness(std::move(gameData), std::move(spawner));
	simulation.seedTraders();
	simulation.seedEconomy(options.economy);
	simulation.arriveUntil_ = simulation.clockTick() + routines::kArriveTicks;
	simulation.setNewGameSettle(options.settleSeconds);
	return simulation;
}

Simulation Simulation::loadGame(std::string_view dataDir, const kworld::LoadOrder& order,
	const std::string& path, bool force, SaveLoadResult* result,
	const RegionConfig& regions)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw SaveError("cannot open " + path);
	auto gameData = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
	World world = buildWorld(*gameData, dataDir, order);
	return restore(std::move(world), in, regions, force, result, std::move(gameData));
}

Simulation Simulation::restore(World world, std::istream& in,
	const RegionConfig& regions, bool force, SaveLoadResult* result,
	std::shared_ptr<const kdata::GameData> gameData)
{
	SaveLoadResult loaded = readSave(in, world.ecsMutable(), world.loadOrder(), force);
	const bool wilderness = gameData != nullptr
		&& world.ecs().get<state::WildernessState>() != nullptr;
	std::unique_ptr<WildernessSpawner> spawner;
	if (wilderness)
		spawner = std::make_unique<WildernessSpawner>(*gameData, world.dataDir());
	Simulation simulation(std::move(world), 1, regions);
	simulation.gameData_ = gameData;
	if (result != nullptr)
		*result = loaded;
	simulation.pending_ = std::move(loaded.pending);
	if (simulation.gameData_ != nullptr && simulation.world_.ecs().get<state::WeatherRegions>() != nullptr)
		simulation.enableWeather(*simulation.gameData_);
	if (spawner != nullptr)
		simulation.attachWilderness(std::move(gameData), std::move(spawner));
	simulation.restoreBoards(); // the saved runtime state first; ensureBlackboards then only fills the squads the save did not hold
	simulation.ensureBlackboards();
	return simulation;
}

void Simulation::enableWeather(const kdata::GameData& gameData)
{
	if (weather_ != nullptr)
		return;
	weather_ = WeatherSystem::build(gameData, world_.dataDir());
	weather_->attach(ecsMutable(), seed_);
	beams_ = std::make_shared<BeamField>(weather_, seed_);
	// Start the fresh regions now, so a snapshot before the first tick (a paused server) already shows a weather.
	weather_->step(ecsMutable(), clockTick());
	beams_->step(ecsMutable(), clockTick());
}

void Simulation::attachWilderness(std::shared_ptr<const kdata::GameData> gameData,
	std::unique_ptr<WildernessSpawner> spawner)
{
	gameData_ = std::move(gameData);
	wild_ = std::move(spawner);
}

void Simulation::instancesChanged()
{
	packsDirty_ = true;
	squadsDirty_ = true;
	regions_->rebuild(ecsMutable());
	syncLineOfSight();
	if (ai_ != nullptr)
		ai_->invalidate();
}

flecs::entity Simulation::placeWildernessSquad(std::string_view templateSid, double x, double z, std::size_t members)
{
	if (wild_ == nullptr)
		throw SaveError("placeWildernessSquad: this world has no wilderness spawner");
	const flecs::entity placed = wild_->placeHomeless(ecsMutable(), templateSid, x, z, members);
	instancesChanged();
	return placed;
}

flecs::entity Simulation::placeTownSquad(std::string_view templateSid, double x, double z)
{
	if (gameData_ == nullptr)
		throw SaveError("placeTownSquad: this world has no game data");
	const kdata::Record* squad = gameData_->find(std::string(templateSid));
	if (squad == nullptr || squad->type != kworld::itemTypeByName("SQUAD_TEMPLATE"))
		throw SaveError("placeTownSquad: not a squad template: " + std::string(templateSid));
	// The template's own faction, as spawnTownSquads resolves it (its `faction` ref).
	flecs::entity_t faction = 0;
	if (const kdata::RefCategory* refs = squad->findRefs("faction"))
		if (!refs->entries.empty())
			world_.ecs().each([&](flecs::entity entity, const state::FactionState& state)
			{
				if (faction == 0 && state.templateSid == refs->entries.front().targetID)
					faction = entity.id();
			});
	RngStream rng(streamSeed(streamSeed(seed_, kSpawnStream), static_cast<std::size_t>(x) ^ (static_cast<std::size_t>(z) << 32)));
	SquadBuilder builder(ecsMutable(), *gameData_, rng);
	const flecs::entity placed = builder.build(*squad, faction, 0, state::SpawnSource::TownBar, true, x, z);
	instancesChanged();
	return placed;
}

Simulation::DebugSpawn Simulation::debugSpawnSquad(std::string_view templateSid, double sx, double sz,
	double tx, double tz)
{
	DebugSpawn result;
	if (wild_ == nullptr)
	{
		result.reason = "this world has no wilderness spawner";
		return result;
	}
	const std::string sid = templateSid.empty() ? std::string("1302-gamedata.base") : std::string(templateSid);
	const kdata::Record* record = gameData_ != nullptr ? gameData_->find(sid) : nullptr;
	if (record == nullptr || record->type != kworld::itemTypeByName("SQUAD_TEMPLATE"))
	{
		result.reason = "unknown squad template '" + sid + "'";
		return result;
	}
	// The squad has to start on the mesh the AI and the regions can see.
	if (navigation_ == nullptr || !navigation_->covers(sx, sz))
	{
		result.reason = "no navmesh at the spawn";
		return result;
	}

	flecs::entity squad;
	try
	{
		squad = placeWildernessSquad(sid, sx, sz);
	}
	catch (const std::exception& error)
	{
		result.reason = error.what();
		return result;
	}
	result.squadId = squad.get<state::Id>()->value;
	const std::uint64_t now = clockTick();

	// A town near the goal: the raid order the faction planner writes (factions.cpp) and the group layer's Raid candidate
	// picks up on its next step. The town list is the AI's own (the ECS TownState entities).
	flecs::entity_t townEntity = 0;
	double nearest = 2000.0;
	ecsMutable().each([&](flecs::entity entity, const state::TownState& town)
	{
		const double far = std::hypot(town.x - tx, town.z - tz);
		if (far <= nearest)
		{
			nearest = far;
			townEntity = entity.id();
		}
	});
	state::GroupAI ai;
	ai.since = now;
	ai.lastStep = now;
	ai.progressTick = now;
	if (townEntity != 0)
	{
		ai.order.status = state::OrderStatus::Issued;
		ai.order.target = state::EntityRef{townEntity};
		ai.order.originX = sx;
		ai.order.originZ = sz;
		// UNSURE: the faction planner's numbers come from the campaign (onSiteHours / kRetreatHours); a debug order has none.
		ai.order.stay = 3 * kTicksPerGameHour;
		ai.order.retreat = 3 * kTicksPerGameHour;
		ai.order.deadline = now + 48 * kTicksPerGameHour;
		ai.order.issued = now;
	} else {
		// No town at the goal: walk there, then the ordinary AI takes over (advance() ends the goal and selects again).
		const Route route = navigation_->routeFor(Traveller{}, sx, sz, tx, tz);
		if (route.found && !route.points.empty())
		{
			ai.goal = state::GoalKind::TravelTo;
			ai.goalX = tx;
			ai.goalZ = tz;
			ai.route = route.points;
			for (std::size_t i = 1; i < ai.route.size(); ++i)
				ai.length += std::hypot(ai.route[i].x - ai.route[i - 1].x, ai.route[i].z - ai.route[i - 1].z);
		}
	}
	squad.set<state::GroupAI>(std::move(ai));
	return result;
}

void Simulation::moveSquad(flecs::entity squad, double x, double z)
{
	if (regions_ != nullptr)
		regions_->place(ecsMutable(), squad.get<state::Id>()->value, x, z);
	else
		squad.set<state::Position>(state::Position{x, z});
	state::SquadInstance& instance = *squad.get_mut<state::SquadInstance>();
	if (instance.hasDen)
	{
		instance.denX = x;
		instance.denZ = z;
	}
}

std::size_t Simulation::removeWorldSquadsNear(double x, double z, double radius, const std::set<flecs::entity_t>& keep)
{
	flecs::world& ecs = ecsMutable();
	std::vector<flecs::entity> doomed;
	ecs.each([&](flecs::entity squad, const state::SquadInstance&, const state::Position& at)
	{
		if (std::hypot(at.x - x, at.z - z) <= radius && keep.count(squad.id()) == 0)
			doomed.push_back(squad);
	});
	for (flecs::entity squad : doomed)
	{
		ecs.delete_with<state::MemberOf>(squad.id());
		squad.destruct();
	}
	if (!doomed.empty() || !keep.empty())
		instancesChanged();
	return doomed.size();
}

void Simulation::holdScenarioArea(double x, double z, double radius, const std::set<flecs::entity_t>& keep)
{
	scenarioHold_ = ScenarioHold{x, z, radius, keep};
}

void Simulation::stepScenarioHold()
{
	if (!scenarioHold_)
		return;
	const ScenarioHold& hold = *scenarioHold_;
	flecs::world& ecs = ecsMutable();
	const auto inside = [&](double x, double z) { return std::hypot(x - hold.x, z - hold.z) <= hold.radius; };
	std::set<flecs::entity_t> doomed;
	// A squad whose token stands in the area (a wilderness den drawn there, a town population loaded there) is the area's ground and goes.
	ecs.each([&](flecs::entity squad, const state::SquadInstance&, const state::Position& at)
	{
		if (hold.keep.count(squad.id()) == 0 && inside(at.x, at.z))
			doomed.insert(squad.id());
	});
	// Its people too: a town squad's token stays at its town while its members stand at a table or a bed - the Hub's bar crowd
	// (Pacifier + 5) materialised at flat1's spot, 356 u from its token - and a homeless camp's are built around its den. A squad
	// with one person in the area goes whole (UNSURE: the alternative, taking only that member out, leaves a squad with a hole).
	for (std::size_t i = 0; combat_ != nullptr && i < combat_->members().size() && i < combatEntities_.size(); ++i)
	{
		const state::Fighter& f = combat_->members()[i].fighter;
		if (f.dead || !inside(f.x, f.z))
			continue;
		const flecs::entity squad = ecs.entity(combatEntities_[i]).target<state::MemberOf>();
		if (!squad.is_valid() || squad.get<state::SquadInstance>() == nullptr || hold.keep.count(squad.id()) != 0)
			continue;
		if (squad.get<state::Id>() == nullptr)
			continue; // no Id: not a squad of the world (a test's bare SquadInstance)
		doomed.insert(squad.id());
	}
	for (const flecs::entity_t id : doomed)
		removeSquadEntity(ecs, ecs.entity(id));
	if (!doomed.empty())
		instancesChanged();
}

bool Simulation::pinWeather(std::string_view groupSid, std::string_view weatherSid)
{
	return weather_ != nullptr && weather_->pin(ecsMutable(), groupSid, weatherSid, clockTick());
}

std::string Simulation::debugSetWeather(std::string_view groupSid, std::string_view weatherSid, double strength)
{
	if (weather_ == nullptr)
		return "group has no weather region";
	const WeatherModel& model = weather_->model();
	if (!weatherSid.empty() && model.weather(weatherSid) == nullptr)
		return "unknown weather '" + std::string(weatherSid) + "'";
	std::vector<std::string> groups;
	if (groupSid == "*")
	{
		for (const WeatherGroupDef& group : model.groups())
			groups.push_back(group.sid);
	} else if (groupSid.empty()) {
		// The session's first player character: the leader of the first player squad (HUD order), the first with a fighter.
		const PlayerSquadsView player = playerSquads();
		const state::Fighter* first = nullptr;
		for (const PlayerSquadsView::Squad& squad : player.squads)
		{
			for (const std::uint64_t member : squad.members)
			{
				first = combat_ != nullptr ? combat_->find(member) : nullptr;
				if (first != nullptr)
					break;
			}
			if (first != nullptr)
				break;
		}
		if (first == nullptr)
			return "no player character";
		const std::string group = weather_->groupAt(first->x, first->z);
		if (group.empty())
			return "group has no weather region";
		groups.push_back(group);
	} else {
		if (model.groupIndex(groupSid) < 0)
			return "unknown biome group '" + std::string(groupSid) + "'";
		groups.push_back(std::string(groupSid));
	}
	// Below 0 (and a non-finite value) means the full strength the frame's field defaults to; above 1 clamps.
	const double value = strength >= 0.0 ? std::min(strength, 1.0) : 1.0;
	bool applied = false;
	for (const std::string& group : groups)
	{
		applied = (weatherSid.empty()
			? weather_->releasePin(ecsMutable(), group)
			: weather_->pin(ecsMutable(), group, weatherSid, clockTick(), value, true, true)) || applied;
	}
	if (!applied)
		return "group has no weather region";
	return {};
}

std::string Simulation::debugCheat(std::uint64_t character, unsigned op)
{
	if (combat_ == nullptr)
		return "no such character";
	if (op > 3)
		return "unknown op " + std::to_string(op);
	if (character == 0)
	{
		// The session's first player character: the leader of the first player squad (HUD order), the first with a fighter.
		for (const PlayerSquadsView::Squad& squad : playerSquads().squads)
		{
			for (const std::uint64_t member : squad.members)
			{
				if (character == 0 && combat_->find(member) != nullptr)
					character = member;
			}
		}
		if (character == 0)
			return "no player character";
	}
	state::Fighter* f = combat_->find(character);
	if (f == nullptr)
		return "no such character";
	if (f->dead)
		return "already dead";
	const CombatRules& rules = combat_->rules();
	if (op == 1 && f->unconscious)
		return "already down";
	if (op == 2 && !f->unconscious)
		return "not down";
	switch (op)
	{
	case 0:
		for (state::FighterPart& part : f->parts)
		{
			part.health = part.maxHealth;
			part.stun = 0.0;
			part.collapsed = false;
			part.pool = 0.0;
		}
		f->blood = rules.maxBlood(*f);
		f->wounds.clear();
		f->bleeding = 0.0;
		f->koTimer = 0.0;
		f->koLatch = false;
		f->unconscious = false;
		f->down = false;
		break;
	case 1:
		f->koTimer = std::max(f->koTimer, rules.knockoutTime(*f));
		break;
	case 2:
		f->koTimer = 0.0;
		f->koLatch = false;
		f->unconscious = false;
		break;
	default:
		f->blood = -rules.maxBlood(*f) - 1.0;
		break;
	}
	mirrorFighter(character);
	return {};
}

std::string Simulation::debugSetTime(double hour, unsigned extraDays)
{
	if (!(hour >= 0.0 && hour < 24.0))
		return "hour out of range";
	if (extraDays > 7)
		return "extraDays out of range";
	const std::uint64_t now = clockTick();
	std::uint64_t day = gameTimeAt(now).day;
	const auto at = [hour](std::uint64_t targetDay) {
		const double minutes = static_cast<double>(targetDay) * 1440.0 + hour * 60.0;
		return static_cast<std::uint64_t>(std::ceil(minutes * kTicksPerGameMinute));
	};
	if (at(day) <= now)
		++day; // the hour has passed today: the next reading is tomorrow's
	const std::uint64_t target = at(day + extraDays);
	if (target <= now)
		return "backwards jump"; // unreachable: the target is always at least one day ahead
	setClockTick(target);
	return {};
}

const state::WorldOptions* Simulation::worldOptions() const
{
	return world_.ecs().get<state::WorldOptions>();
}

namespace {

std::string optionText(double value)
{
	std::ostringstream text;
	text << std::setprecision(5) << value;
	return text.str();
}

} // namespace

std::vector<std::pair<std::string, std::string>> Simulation::gameOptionRows() const
{
	const state::WorldOptions* saved = worldOptions();
	std::vector<std::pair<std::string, std::string>> rows;
	rows.emplace_back("cod", optionText(saved != nullptr ? saved->chanceOfDeath : combatSettings_.chanceOfDeath));
	rows.emplace_back("gdm", optionText(saved != nullptr ? saved->damageScale : combatSettings_.damageScale));
	rows.emplace_back("ht", optionText(saved != nullptr ? saved->hungerTime : combatSettings_.hungerTime));
	rows.emplace_back("medicalClock", optionText(combatSettings_.medicalClock));
	rows.emplace_back("saved", saved != nullptr ? "yes" : "no");
	return rows;
}

std::string Simulation::setGameOption(std::string_view key, double value)
{
	if (!std::isfinite(value))
		return "value is not finite";
	if (key == "medicalClock")
	{
		// The dev override (COMBAT_OPTIONS.md 4): never saved, so every save keeps the default rate.
		if (!(value >= 0.25 && value <= 4.0))
			return "'medicalClock' out of range 0.25..4";
		combatSettings_.medicalClock = value;
		applyGameOptions();
		return {};
	}
	// A world without the singleton takes the server's settings as its starting options, so setting one key does not reset
	// the others to their defaults.
	state::WorldOptions options = worldOptions() != nullptr ? *worldOptions() : state::WorldOptions{};
	if (worldOptions() == nullptr)
	{
		options.chanceOfDeath = combatSettings_.chanceOfDeath;
		options.damageScale = combatSettings_.damageScale;
		options.hungerTime = combatSettings_.hungerTime;
	}
	if (key == "cod")
	{
		if (!(value >= state::WorldOptions::kChanceOfDeathMin && value <= state::WorldOptions::kChanceOfDeathMax))
			return "'cod' out of range 0.5..4";
		options.chanceOfDeath = value;
	} else if (key == "gdm") {
		if (!(value >= state::WorldOptions::kDamageScaleMin && value <= state::WorldOptions::kDamageScaleMax))
			return "'gdm' out of range 0.5..4";
		options.damageScale = value;
	} else if (key == "ht") {
		if (!(value >= state::WorldOptions::kHungerTimeMin && value <= state::WorldOptions::kHungerTimeMax))
			return "'ht' out of range 0.25..8";
		options.hungerTime = value;
	} else {
		return "unknown option '" + std::string(key) + "'";
	}
	ecsMutable().set<state::WorldOptions>(options);
	applyGameOptions();
	return {};
}

void Simulation::setRetargetHold(double seconds)
{
	combatSettings_.retargetHold = seconds;
	if (combat_ != nullptr)
		combat_->setRetargetHold(seconds);
}

void Simulation::applyGameOptions()
{
	if (combat_ == nullptr)
		return;
	const state::WorldOptions* saved = worldOptions();
	combat_->setTuning(saved != nullptr ? saved->chanceOfDeath : combatSettings_.chanceOfDeath,
		saved != nullptr ? saved->damageScale : combatSettings_.damageScale,
		saved != nullptr ? saved->hungerTime : combatSettings_.hungerTime,
		combatSettings_.medicalClock);
}

// The ECS Fighter is written by stepCombat's mirror, mid-tick; the writers after it (stepWorldTasks, the routines, the jobs) change the arena
// Fighter only, and the next tick's mirror carries it over. A save or a hash taken between two ticks would hold the Fighter as it was before
// those writers, and a load would lose what they did (the squad's follow orders, k16.6.320). Until the encode is done the ECS copy is the
// arena's; the old copy is put back after, so taking a save or a hash never changes what the next tick reads (and costs nothing per tick).
Simulation::ArenaMirror::ArenaMirror(const Simulation& simulation) : sim(simulation)
{
	if (sim.combat_ == nullptr)
		return;
	flecs::world& ecs = const_cast<flecs::world&>(sim.world_.ecs());
	const auto& members = sim.combat_->members();
	held.reserve(members.size());
	for (std::size_t i = 0; i < members.size(); ++i)
	{
		// A listed entity can be dead or Fighter-less until dropGoneFighters runs: skip it, the slot stays so held[i] keeps its index.
		const flecs::entity entity = ecs.entity(sim.combatEntities_[i]);
		state::Fighter* copy = entity.is_alive() ? entity.get_mut<state::Fighter>() : nullptr; // no OnSet: nothing observes this
		held.emplace_back();
		if (copy == nullptr)
			continue;
		held.back() = std::move(*copy);
		*copy = members[i].fighter;
		copy->pathSamples = sim.combat_->pathSamplesOf(members[i].id);
	}
}

Simulation::ArenaMirror::~ArenaMirror()
{
	if (sim.combat_ == nullptr)
		return;
	flecs::world& ecs = const_cast<flecs::world&>(sim.world_.ecs());
	for (std::size_t i = 0; i < held.size(); ++i)
	{
		const flecs::entity entity = ecs.entity(sim.combatEntities_[i]);
		if (state::Fighter* copy = entity.is_alive() ? entity.get_mut<state::Fighter>() : nullptr)
			*copy = std::move(held[i]);
	}
}

void Simulation::save(std::ostream& out) const
{
	KSIM_PROFILE_SCOPE(profiler_, Save);
	const ArenaMirror mirror(*this);
	syncBoardStates();
	const struct Drop { const Simulation& sim; ~Drop() { sim.dropBoardStates(); } } drop{*this};
	writeSave(out, world_.ecs(), world_.loadOrder(), pending_);
}

void Simulation::saveGame(const std::string& path) const
{
	KSIM_PROFILE_SCOPE(profiler_, Save);
	const ArenaMirror mirror(*this);
	syncBoardStates();
	const struct Drop { const Simulation& sim; ~Drop() { sim.dropBoardStates(); } } drop{*this};
	writeSaveFile(path, world_.ecs(), world_.loadOrder(), pending_);
}

std::uint64_t Simulation::clockTick() const
{
	return world_.ecs().get<state::Clock>()->tick;
}

void Simulation::setClockTick(std::uint64_t tick)
{
	ecsMutable().set<state::Clock>(state::Clock{tick});
}

// The portrait's engaged hold after the last living target / carried body (OURS, k16.6.159: the AI+0xb0 float decays ~1 per 15 s).
constexpr double kPortraitEngagedSeconds = 15.0;

void Simulation::tick()
{
	KSIM_PROFILE_BEGIN();
	flecs::world& ecs = ecsMutable();
	const std::uint64_t now = ++ecs.get_mut<state::Clock>()->tick;
	applyCommands(now);
	ensureBlackboards(); // a command may have spawned squads
	syncNavCoverage(); // a SetPlayerAreas / NEW_GAME command may have moved the areas
	stepUnlocks();
	stepDiscovery(false);
	stepKnownFactions();
	syncDoorPaint(); // a load, or a door changed by a command, repaints before anyone is routed
	KSIM_PROFILE_MARK(Setup);
	// Intervals are on the absolute Clock tick, so a loaded save keeps its
	// phase.
	for (SystemState& system : systems_)
	{
		if (now % system.intervalTicks == 0)
			runSystem(system, now);
	}
	KSIM_PROFILE_MARK(Systems);
	// The weather regions tick every tick, attended or not (weather.hpp).
	if (weather_ != nullptr)
	{
		weather_->step(ecs, now);
		beams_->step(ecs, now);
	}
	KSIM_PROFILE_MARK(Weather);
	// Respawns come before the AI, so a new squad gets its first step. A wiped spawner squad leaves the quota first.
	if (wild_ != nullptr && now % kStrategicIntervalTicks == 0)
	{
		const bool retired = retireWipedSquads(ecs) != 0;
		if (wild_->step(ecs, now).squads != 0 || retired)
			instancesChanged();
	}
	KSIM_PROFILE_MARK(Wild);
	stepGateBinding();
	if (ai_ != nullptr)
	{
		if (now % kStrategicIntervalTicks == 0 && ai_->strategicStep(ecs, now))
			instancesChanged();
		KSIM_PROFILE_MARK(AiStrategic);
		ensureBlackboards(); // the respawns and the strategic step may have built squads
		if (now % kSquadIntervalTicks == 0)
		{
			attachBarTables();
			attachSlaveDepots();
			KSIM_PROFILE_SUB(BarDepots);
		}
		// Every tick: each does the squads of this tick's slice (inSlice), so the second's work is spread over its 20 ticks.
		prepareWorldCarry();
		KSIM_PROFILE_SUB(CarryPrep);
		ai_->squadStep(ecs, *regions_, now);
		KSIM_PROFILE_SUB(SquadStep);
		logDeliveryFailures();
		KSIM_PROFILE_SUB(DeliveryLog);
		stepWorldSquads();
		KSIM_PROFILE_SUB(WorldSquads);
	}
	KSIM_PROFILE_MARK(AiSquad);
	stepStragglers();
	KSIM_PROFILE_SUB(Stragglers);
	if (now % kSquadIntervalTicks == 0)
		stepTokenFights(); // after the squad step, so a routed loser's goal is picked by the next one; needs no AI (nothing to rout then)
	KSIM_PROFILE_SUB(TokenFights);
	stepStoredMedical();
	KSIM_PROFILE_SUB(StoredMedical);
	stepCorpses();
	KSIM_PROFILE_SUB(Corpses);
	dropGoneFighters();
	KSIM_PROFILE_SUB(DropGone);
	KSIM_PROFILE_MARK(Tokens);
	stepCombat(now);
	KSIM_PROFILE_MARK(Combat);
	stepPlayerSquads();
	stepBuildings();
	stepOrderCombat();
	stepDoorSteps();
	stepSelfAid();
	stepInteractions();
	KSIM_PROFILE_MARK(Interact);
	stepProducers();
	stepItemNodes(now);
	stepCarry();
	KSIM_PROFILE_MARK(Producers);
	stepIntruders();
	KSIM_PROFILE_SUB(Intruders);
	stepCrimeGoals();
	KSIM_PROFILE_SUB(CrimeGoals);
	stepSlaveTrade();
	KSIM_PROFILE_SUB(SlaveTrade);
	stepSlaveState();
	KSIM_PROFILE_SUB(SlaveState);
	stepCorpsePot();
	KSIM_PROFILE_SUB(CorpsePot);
	stepWorldTasks();
	KSIM_PROFILE_SUB(WorldTasks);
	stepTorture();
	KSIM_PROFILE_SUB(Torture);
	KSIM_PROFILE_MARK(Tasks);
	stepPlayerAreas();
	syncNavCoverage(); // before stepResidents builds anyone on the new areas
	KSIM_PROFILE_SUB(PlayerAreas);
	stepResidents();
	KSIM_PROFILE_SUB(Residents);
	stepRoutines();
	KSIM_PROFILE_SUB(Routines);
	stepRearm(); // after the routines: a sleeper woken by an attack and any fighter with a target draws (the plan's 6)
	KSIM_PROFILE_SUB(Rearm);
	stepShopping();
	KSIM_PROFILE_SUB(Shopping);
	stepStealthKo();
	KSIM_PROFILE_SUB(StealthKo);
	KSIM_PROFILE_MARK(Residents);
	stepCrime(now);
	stepDialogue(now);
	KSIM_PROFILE_MARK(Crime);
	stepIndividualGoals();
	stepProspect();
	stepOrderQueues();
	stepFarOrders();
	stepJobs();
	KSIM_PROFILE_MARK(Goals);
	stepTraders();
	stepEconomy();
	stepConstruction();
	stepMeals();
	KSIM_PROFILE_MARK(Economy);
	syncDoorPaint();
	regions_->tick(ecs, now);
	syncLineOfSight(); // the Full set follows the player areas: a region's hull trees come and go with it (task AZ)
	// Last, so a squad the residents built or a den the wilderness drew this tick is gone before the next tick's AI (task BM).
	KSIM_PROFILE_MARK(Nav);
	stepScenarioHold();
	mirrorScheduler();
	KSIM_PROFILE_MARK(Tail);
	KSIM_PROFILE_END(); // before the settle: its ticks are ordinary ticks of their own
	if (settleDue_)
		settleNewGame();
}

#if KSIM_PROFILE
void Simulation::profileTickEnd()
{
	if (!profileFighters_)
	{
		profileFighters_.emplace(ecsMutable().query<const state::Fighter>());
		profileSquads_.emplace(ecsMutable().query<const state::SquadInstance>());
	}
	const NavStats* nav = ai_ != nullptr && ai_->config().navigator != nullptr ? ai_->config().navigator->profileStats() : nullptr;
	profiler_.end(clockTick(), static_cast<std::uint32_t>(profileFighters_->count()), static_cast<std::uint32_t>(profileSquads_->count()),
		nav != nullptr ? nav->routeCalls + nav->localCalls + nav->crossCalls + nav->snapCalls : 0, nav != nullptr ? nav->routeNs + nav->localNs + nav->crossNs + nav->snapNs : 0);
}
#endif

void Simulation::setNewGameSettle(double seconds)
{
	settleTicks_ = settleTicksOf(seconds);
}

// The world as the original has it at the first frame: the NEW_GAME tick has just made the player's squad and areas, so the residents around it are
// still to be built. The walk-in window opens here and the world runs headless forward: the residents promoted in the first ticks walk to their sites
// and start their routines, the mercenaries leave. newGame built the world one settle early, so the clock reaches the start time as the settle ends; it
// never goes backwards (stamps made on the NEW_GAME tick stay in the past). The settle's ticks are ordinary ticks (no new state: a save taken after it
// loads like any other).
void Simulation::settleNewGame()
{
	settleDue_ = false;
	arriveUntil_ = clockTick() + routines::kArriveTicks;
	settling_ = true;
	run(settleTicks_);
	settling_ = false;
}

void Simulation::enableAi(AiSystem::Config config)
{
	if (config.doors == nullptr)
		config.doors = DoorData::fromWorld(world_);
	if (config.data == nullptr)
		config.data = gameData_;
	if (config.areas == nullptr && config.factions != nullptr)
	{
		try
		{
			config.areas = std::make_shared<AreaLookup>(world_.dataDir());
		} catch (const std::exception&) {
			// No areas map: territory is not applied.
		}
	}
	ai_ = std::make_unique<AiSystem>(std::move(config), seed_);
}

void Simulation::enableCombat(std::shared_ptr<const CombatData> data, CombatSettings settings)
{
	combatSettings_ = settings;
	combatData_ = data;
	combat_ = std::make_unique<CombatArena>(std::move(data), settings, seed_);
	applyGameOptions(); // a world with saved options (a load, a NEW_GAME) tunes the new arena
	if (combatObserver_)
		combat_->setObserver(combatObserver_);
	installFightTrace();
	combat_->setNavigation(navigation_);
	maybeEnableLineOfSight(); // phase 2: sight stops at walls once the world's collision is there
	refreshLineOfSightHook();
	combatEntities_.clear();
	flecs::world& ecs = ecsMutable();
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> found;
	ecs.each([&found](flecs::entity entity, const state::Id& id, const state::Fighter&)
	{
		found.push_back({id.value, entity.id()});
	});
	std::sort(found.begin(), found.end());
	for (const auto& [id, entity] : found)
	{
		combat_->add(id, *ecs.entity(entity).get<state::Fighter>());
		touchHomes();
		combatEntities_.push_back(entity);
	}
	rebuildRoutineBook(); // a loaded save: the reservations come from the members' Routine::target
}

void Simulation::installFightTrace()
{
	if (!combat_)
		return;
	if (!decisionTrace_.record)
	{
		combat_->setFightTrace({}, {});
		return;
	}
	combat_->setFightTrace(decisionTrace_.wants, [this](const CombatArena::FightPick& fight)
	{
		trace::Pick pick;
		pick.tick = clockTick();
		pick.member = fight.member;
		pick.source = trace::Source::Combat;
		pick.runningTask = fight.previousGoal;
		pick.runningTarget = fight.previousTarget;
		pick.result = fight.target != 0 ? trace::Result::Chosen : trace::Result::DefaultIdle;
		pick.task = fight.goal;
		pick.target = fight.target;
		pick.score = fight.score;
		for (const CombatArena::FightCandidate& c : fight.candidates)
			pick.candidates.push_back({c.goal, 0, c.id, c.utility, 1.0, c.crowd, c.score, c.id == fight.target ? goalsel::Why::Chosen : goalsel::Why::Outscored});
		decisionTrace_.record(pick);
	});
}

void Simulation::enableNavigation(std::shared_ptr<const MeshNavigator> navigation)
{
	pathRetries_.resetAll(); // invalidate (C57): a new navmesh zeroes every retry counter
	navigation_ = std::move(navigation);
	if (combat_ != nullptr)
		combat_->setNavigation(navigation_);
	syncNavCoverage(); // a streaming mesh follows the saved areas of a loaded game before anyone routes
	furnishNavCoverage();
}

void Simulation::setGameData(std::shared_ptr<const kdata::GameData> gameData)
{
	gameData_ = std::move(gameData);
	maybeEnableLineOfSight();
}

void Simulation::maybeEnableLineOfSight()
{
	if (los_ != nullptr || gameData_ == nullptr || regions_ == nullptr || world_.placements().empty())
		return;
	enableLineOfSight();
}

void Simulation::refreshLineOfSightHook()
{
	if (combat_ == nullptr)
		return;
	if (los_ == nullptr)
	{
		combat_->environmentMutable().lineOfSight = nullptr;
		combat_->environmentMutable().shotBlocker = nullptr;
		return;
	}
	combat_->environmentMutable().lineOfSight =
		[los = los_.get()](std::uint64_t observer, const state::Fighter& a, std::uint64_t target, const state::Fighter& b)
		{
			return los->visible(observer, a, target, b);
		};
	combat_->environmentMutable().shotBlocker = [los = los_.get()](const std::array<double, 3>& muzzle, const state::Fighter& target)
	{
		const LineOfSight::Hit hit = los->firstBlocker(muzzle, target);
		return hit.blocked ? hit.t : -1.0;
	};
}

void Simulation::enableLineOfSight()
{
	if (los_ != nullptr || gameData_ == nullptr || regions_ == nullptr)
		return;
	los_ = std::make_unique<LineOfSight>(world_.dataDir(), *gameData_, world_.placements(),
		regions_->regionsPerAxis(), regions_->config().regionSize);
	syncLineOfSight();
	refreshLineOfSightHook();
}

void Simulation::disableLineOfSight() noexcept
{
	los_.reset();
	if (combat_ != nullptr)
		combat_->environmentMutable().lineOfSight = nullptr; // a null std::function's assignment does not throw
}

void Simulation::syncLineOfSight()
{
	if (los_ == nullptr || regions_ == nullptr)
		return;
	// The doors' open state for sight: a door that is open (0) or broken (3) passes it. Before the regions, so a tree built this tick reads it.
	if (!doorQuery_)
		doorQuery_.emplace(world_.ecs().query<const state::Door>());
	doorQuery_->each([this](const state::Door& door) { los_->setDoorOpen(door.key, door.state == 0 || door.state == 3); });
	const std::vector<bool>& full = regions_->fullRegions();
	for (std::int32_t region = 0; region < static_cast<std::int32_t>(full.size()); ++region)
	{
		if (full[static_cast<std::size_t>(region)])
			los_->ensure(region);
		else
			los_->drop(region);
	}
}

flecs::entity Simulation::spawnFighter(state::Fighter fighter)
{
	if (combat_ == nullptr)
		throw std::logic_error("spawnFighter: combat is not enabled");
	flecs::entity entity = state::createInstance(ecsMutable());
	adoptFighter(entity, std::move(fighter));
	return entity;
}

void Simulation::adoptFighter(flecs::entity entity, state::Fighter fighter)
{
	const std::uint64_t id = entity.get<state::Id>()->value;
	// What it wears and wields is what it owns (looting takes it from here). An entity that already has an Inventory (a pack animal's
	// bag, a resident demoted earlier) keeps it.
	if (entity.get<state::Inventory>() == nullptr)
	{
		state::Inventory inventory;
		if (!fighter.weapon.sid.empty() && fighter.weapon.weaponType != 5)
			inventory.add(fighter.weapon.sid, 1, state::Inventory::kWeapon, fighter.weapon.quality);
		for (const state::FighterArmour& piece : fighter.armour)
		{
			if (!piece.sid.empty())
				inventory.add(piece.sid, 1, static_cast<std::uint8_t>(state::Inventory::kArmourBase + piece.slot), piece.quality);
		}
		if (!fighter.spawnBackpack.empty())
			inventory.add(fighter.spawnBackpack, 1, state::Inventory::kBackpackSlot);
		for (const auto& [sid, count] : fighter.spawnPack)
			inventory.add(sid, count);
		inventory.version = 1;
		if (gameData_ != nullptr)
		{
			// The original's creation order (rwh.194): equipment, the fixed main grid, the worn backpack's contents, and what is left is
			// destroyed (not dropped). The grid never grows.
			const grid::Rules rules(*gameData_);
			for (state::Inventory::Entry& entry : grid::settle(inventory, rules, id))
			{
				if (inventory.wornBackpack() != nullptr)
					grid::autoplace(inventory, rules, std::move(entry), 1);
			}
		}
		entity.set<state::Inventory>(std::move(inventory));
	}
	fighter.spawnPack.clear();
	fighter.spawnBackpack.clear();
	if (fighter.building == 0 && fighter.carriedBy == 0)
	{
		setLocation(fighter, buildingAt(fighter.x, fighter.y, fighter.z), fighter.y);
	}
	// A non-player character that spawns inside a building calls it home, whether the spawner placed it there or the lookup found it
	// (UNSURE: the squad's real home is a squad building reference the Fighter does not carry; a box that covers a whole compound makes
	// every spawn in it a resident). Task CH: PICKUP_INTRUDERS 223 reads it as the exe's ownership block, so a scenario resident placed
	// inside a building needs it too.
	if (fighter.homeBuilding == 0 && fighter.building != 0 && fighter.carriedBy == 0 && !fighter.playerControlled)
	{
		fighter.homeBuilding = fighter.building;
		touchHomes();
	}
	if (fighter.faction == 0 && fighter.playerControlled)
	{
		// MQ14 path 2: a player character belongs to the player faction (the lowest Id, as residents.cpp playerFactionOf) for the neutral test.
		std::uint64_t lowest = 0;
		world_.ecs().each([&](const state::PlayerFaction&, const state::FactionState&, const state::Id& faction)
		{
			if (lowest == 0 || faction.value < lowest)
				lowest = faction.value;
		});
		fighter.faction = lowest;
	}
	fighter.portraitSerial = static_cast<std::uint32_t>(RngStream(streamSeed(streamSeed(seed_, kPortraitStream), static_cast<std::size_t>(id))).uniform01() * 4.0e9);
	combat_->add(id, std::move(fighter));
	touchHomes();
	entity.set<state::Fighter>(*combat_->find(id));
	// The arena is in Id order, and combatEntities_ runs parallel to it: a new character is last, a promoted old one lands in the middle.
	const auto& members = combat_->members();
	const auto at = std::find_if(members.begin(), members.end(), [id](const CombatArena::Member& m) { return m.id == id; });
	combatEntities_.insert(combatEntities_.begin() + (at - members.begin()), entity.id());
}

void Simulation::stepCombat(std::uint64_t now)
{
	if (combat_ == nullptr || combat_->empty())
		return;
	const int steps = std::max(1, combatSettings_.substeps);
	combat_->setTime(static_cast<double>(now - 1) * kTickSeconds);
	if (los_ != nullptr)
		los_->setTick(now); // the sight pair cache ages in ticks
	// Perception's weather: the region under each observer (the shared_ptr keeps the system alive; no pointer to this).
	if (weather_ != nullptr && !combat_->environment().weatherAt)
		combat_->environmentMutable().weatherAt = [weather = weather_](double x, double z, int& type, double& strength)
		{
			weather->at(x, z, type, strength);
		};
	if (beams_ != nullptr && !combat_->environment().beamAt)
		combat_->environmentMutable().beamAt = [beams = beams_](double x, double z, int& type, double& strength)
		{
			strength = beams->burnAt(x, z, type);
		};
	// Perception's walled regions (short sight range inside one; see perception.hpp, findings/bq7_sight_region.md): rebuilt from the gate
	// sites every 10 s of sim time and at the first step after a load. Gates and towns change rarely (a player town's gates are not
	// sites yet), so a load mid-interval agrees with the straight run.
	perception::Environment& environment = combat_->environmentMutable();
	if (environment.walledTick == ~std::uint64_t{0} || ticksSince(now, environment.walledTick) >= 200)
	{
		environment.walledTick = now;
		environment.walled.clear();
		// The towns' circles (goal 34's town test and goal 144's home ring): the exe's current-town geometry, towncircle.hpp.
		environment.towns.clear();
		std::vector<std::pair<double, double>> centres; // placement order, as the gate -> town binding takes them
		world_.ecs().each([&](const state::TownState& town)
		{
			centres.emplace_back(town.x, town.z);
			if (gameData_ != nullptr)
			{
				const towncircle::Circle circle = towncircle::of(*gameData_, town);
				if (circle.radius > 0.0)
					environment.towns.push_back(circle);
			}
		});
		std::vector<perception::Environment::WalledRegion> regions(centres.size());
		for (const GateSite& gate : world_.gateSites())
		{
			const std::size_t town = nearestTownIndex(gate.x, gate.z, centres.size(), [&](std::size_t i) { return centres[i]; });
			if (town == centres.size())
				continue;
			perception::Environment::WalledRegion& region = regions[town];
			region.x = centres[town].first;
			region.z = centres[town].second;
			region.radius = std::max(region.radius, std::hypot(gate.x - region.x, gate.z - region.z));
			region.gates.push_back({gate.x, gate.z, gate.nx, gate.nz});
		}
		for (perception::Environment::WalledRegion& region : regions)
		{
			if (!region.gates.empty())
				environment.walled.push_back(std::move(region));
		}
	}
	// BQ13 / BQ14: the join goals of every package-driven character (every 200 ticks, and when the roster changed).
	if (!joinGoalsDone_ || combat_->members().size() != joinGoalsCount_ || ticksSince(now, joinGoalsTick_) >= 200)
	{
		refreshJoinGoals(now);
		joinGoalsDone_ = true;
	}
	// MQ14 path 2: which faction pairs are not neutral to each other, for the fighters that attack neutrals (combat's enemy test).
	{
		std::vector<std::uint64_t> factions;
		bool aggressors = false;
		for (const CombatArena::Member& member : combat_->members())
		{
			aggressors = aggressors || ((member.fighter.attacksNeutrals || member.joinGoals != 0) && !member.fighter.dead); // join goal 15 reads the bond set too
			if (member.fighter.faction != 0)
				factions.push_back(member.fighter.faction);
		}
		std::sort(factions.begin(), factions.end());
		factions.erase(std::unique(factions.begin(), factions.end()), factions.end());
		if (aggressors && (factions != bondedFactions_ || ticksSince(now, bondedTick_) >= 200))
		{
			std::unordered_map<std::uint64_t, flecs::entity> byId;
			world_.ecs().each([&](flecs::entity entity, const state::FactionState&, const state::Id& id)
			{
				if (std::binary_search(factions.begin(), factions.end(), id.value))
					byId.emplace(id.value, entity);
			});
			std::unordered_set<std::uint64_t> bonded;
			std::unordered_set<std::uint64_t> coexist;
			std::unordered_set<std::uint64_t> hostile;
			for (const auto& [a, from] : byId)
			{
				const kdata::RefCategory* coexistence = nullptr;
				if (const kdata::Record* record = gameData_->find(from.get<state::FactionState>()->templateSid))
					coexistence = record->findRefs("coexistence");
				for (const auto& [b, to] : byId)
				{
					if (a == b)
						continue;
					if (factionRelation(*gameData_, from, to) >= FactionBook::kAlly)
						bonded.insert(CombatArena::bondKey(a, b));
					// Hostile by relation, in one direction (task BV, FUN_14079c950: MY faction's view of the target; the direction
					// lives in factionsHostile): goal 16 ATTACK_ENEMIES takes such a faction's characters on sight.
					// `hostileFactions(mine, other)` reads my view of it.
					if (factionsHostile(*gameData_, from, to))
						hostile.insert(CombatArena::bondKey(a, b));
					// The FACTION `coexistence` list (RelationData byte 3; bq10_goal21.md K2): goal 21 skips such a target faction.
					if (coexistence != nullptr)
					{
						for (const kdata::RefEntry& entry : coexistence->entries)
						{
							if (entry.targetID == to.get<state::FactionState>()->templateSid)
								coexist.insert(CombatArena::bondKey(a, b));
						}
					}
				}
			}
			combat_->setBonded(std::move(bonded), std::move(coexist), std::move(hostile));
			combat_->setSensorContext(sensorContext()); // the enemy test's relation (task BV/BU), same refresh as the sets
			bondedFactions_ = std::move(factions);
			bondedTick_ = now;
		}
	}
	// Perception's time of day: the fraction of the game day at this tick (the sim clock, not wall time), and the tick itself, the
	// stamp of the dialogue memory's sightings (state::Fighter::met).
	combat_->environmentMutable().dayFraction = std::fmod(static_cast<double>(now), kTicksPerGameDay) / kTicksPerGameDay;
	combat_->environmentMutable().tick = now;
	// The depenetration leaves a seated patron, a caged prisoner and an operator alone (the exe skips a character in a bed / cage, char+0x2f8, and one operating a machine, +0xb8;
	// UNSURE whether sitting sets +0x2f8, kinspect BQ16 section 4).
	{
		knowBarSeats();
		knowHomeSeats();
		std::unordered_set<std::uint64_t> exempt;
		const auto& members = combat_->members();
		for (std::size_t i = 0; i < members.size(); ++i)
		{
			const state::Routine* routine = flecs::entity(world_.ecs().c_ptr(), combatEntities_[i]).get<state::Routine>();
			// A town resident's (the town scorer's: its squad holds a SquadRoutine) non-urgent goal walks; a world squad's selector goals keep the mover default.
			bool strolls = false;
			if (routine != nullptr && routines::walksGoal(routine->goal) && !perception::inCombat(members[i].fighter))
			{
				const flecs::entity squad = flecs::entity(world_.ecs().c_ptr(), combatEntities_[i]).target<state::MemberOf>();
				strolls = squad.is_valid() && squad.has<state::SquadRoutine>();
			}
			state::Fighter* mutableFighter = combat_->find(members[i].id);
			mutableFighter->strolling = strolls;
			// The portrait's "engaged" (OURS, UNSURE, k16.6.159): a living target or a body in the arms, held 15 s past the last tick of either.
			const state::Fighter* foe = members[i].fighter.target != 0 ? combat_->find(members[i].fighter.target) : nullptr;
			if ((foe != nullptr && !foe->dead) || members[i].fighter.carrying != 0)
				mutableFighter->engagedUntil = now + static_cast<std::uint64_t>(kPortraitEngagedSeconds / kTickSeconds);
			// A goal-43 floor-sitter stays pushable (as when it stood); only a chair holds its place.
			const bool floorSit = routine != nullptr && routine->goal == routines::task::StayInHome && routine->target.kind != static_cast<std::uint8_t>(routines::SiteKind::Seat);
			// A town resident standing about (at its node, a post, anywhere with nothing to do) is outside the push, as pusher and as neighbour: the
			// original runs the depenetration only on a character that is simulating, and a barman at his node did not move 0.01 u while a player
			// walked through him 2.2 u away (kenshi-rwh.95 section 3, rwh.58). OURS: the exe's gate is not decoded, this is its observable half. Standing =
			// a routine, no target or engagement, no running task, no speed. A bare arena's idle bodies and the player's own still push (the crowd pins).
			const state::Fighter& fighter = members[i].fighter;
			const bool standing = routine != nullptr && !perception::inCombat(fighter) && !fighter.taskRunning() && !perception::isMoving(fighter);
			if (standing || (seatedAt(i) && !floorSit) || operateAction(members[i].id).has_value() || fighter.cageBuilding != 0)
				exempt.insert(members[i].id);
		}
		combat_->setPushExempt(std::move(exempt));
	}
	// Task BP: each shackled slave's captor as a faction id, for combat's enemy test (a captor's squad and its allies leave its captive
	// alone). The cuff writes `captorSid` at any moment, so it is resolved here every tick, not on the bond set's timer; derived, never saved.
	{
		const auto& members = combat_->members();
		const bool any = std::any_of(members.begin(), members.end(), [](const CombatArena::Member& member)
		{
			return member.fighter.slaveState == 1 && !member.fighter.captorSid.empty();
		});
		if (any)
		{
			std::unordered_map<std::string, std::uint64_t> factions;
			world_.ecs().each([&](flecs::entity, const state::FactionState& faction, const state::Id& id)
			{
				factions.emplace(faction.templateSid, id.value);
			});
			for (const CombatArena::Member& member : members)
			{
				const auto at = member.fighter.slaveState == 1 && !member.fighter.captorSid.empty()
					? factions.find(member.fighter.captorSid) : factions.end();
				const std::uint64_t captor = at != factions.end() ? at->second : 0;
				state::Fighter* fighter = combat_->find(member.id);
				if (fighter->captorFaction != captor)
					fighter->captorFaction = captor;
			}
		}
	}
	KSIM_PROFILE_SUB(CombatPre);
#if KSIM_PROFILE
	combat_->setProfiler(&profiler_);
#endif
	for (int i = 0; i < steps; ++i)
		combat_->step(kTickSeconds / steps);
	for (const CombatArena::SeverNote& note : combat_->takeSevers())
		onLimbSevered(note.who, note.part, note.item);
	for (const CombatArena::DeathNote& note : combat_->takeDeaths())
		onNpcDeath(note.who, note.killer);
	KSIM_PROFILE_SUB(CombatStep);
	flecs::world& ecs = ecsMutable();
	const auto& members = combat_->members();
	for (std::size_t i = 0; i < members.size(); ++i)
		ecs.entity(combatEntities_[i]).set<state::Fighter>(members[i].fighter);
	KSIM_PROFILE_SUB(CombatMirror);
}

namespace {

// Adds `sid` to a list that only grows, once.
void appendOnce(std::vector<std::string>& list, const std::string& sid)
{
	if (std::find(list.begin(), list.end(), sid) == list.end())
		list.push_back(sid);
}

const state::WorldEventState* eventSet(const flecs::world& ecs)
{
	return ecs.get<state::WorldEventState>();
}

// The event set as a copy, made when one is absent (a quiet world carries none).
state::WorldEventState eventsOrNew(const flecs::world& ecs)
{
	const state::WorldEventState* held = ecs.get<state::WorldEventState>();
	return held != nullptr ? *held : state::WorldEventState{};
}

// A unique CHARACTER's record sid of a character instance, "" for a non-unique or unknown one: only those are world event actors
// (WORLD_EVENT_STATE's references).
std::string uniqueRecordOf(const kdata::GameData* data, const flecs::entity& entity)
{
	const state::CharacterInstance* character = entity.is_valid() ? entity.get<state::CharacterInstance>() : nullptr;
	if (character == nullptr || data == nullptr)
		return {};
	const kdata::Record* record = data->find(character->templateSid);
	return record != nullptr && record->boolOr("unique", false) ? character->templateSid : std::string();
}

} // namespace

void Simulation::onNpcDeath(std::uint64_t victim, std::uint64_t killer)
{
	const flecs::entity entity = fighterEntity(victim);
	const std::string sid = uniqueRecordOf(gameData_.get(), entity);
	if (sid.empty())
		return;
	state::WorldEventState events = eventsOrNew(world_.ecs());
	appendOnce(events.dead, sid);
	const state::Fighter* attacker = killer != 0 ? combat_->find(killer) : nullptr;
	const std::string player = playerFactionSid();
	bool mine = attacker != nullptr && attacker->playerControlled;
	if (!mine && !player.empty() && killer != 0)
	{
		const state::Allegiance* side = fighterEntity(killer).get<state::Allegiance>();
		mine = side != nullptr && side->factionSid == player;
	}
	if (mine)
		appendOnce(events.playerTouched, sid);
	world_.ecs().set<state::WorldEventState>(std::move(events));
}

void Simulation::noteNpcImprisoned(std::uint64_t victim, const std::string& captorSid)
{
	const std::string player = playerFactionSid();
	if (player.empty() || captorSid != player)
		return;
	const std::string sid = uniqueRecordOf(gameData_.get(), fighterEntity(victim));
	if (sid.empty())
		return;
	state::WorldEventState events = eventsOrNew(world_.ecs());
	appendOnce(events.playerTouched, sid);
	world_.ecs().set<state::WorldEventState>(std::move(events));
}

const state::WorldEventState* Simulation::worldEventState() const
{
	return eventSet(world_.ecs());
}

std::string Simulation::playerFactionSid() const
{
	std::string found;
	world_.each([&](flecs::entity entity, const state::FactionState& faction)
	{
		if (found.empty() && entity.has<state::PlayerFaction>())
			found = faction.templateSid;
	});
	return found;
}

sensor::Context Simulation::sensorContext() const
{
	// Called per member by the routine scorer: kept for the tick, rebuilt when the tick or the faction counts change.
	const std::uint64_t tick = clockTick();
	const std::int64_t factionCount = world_.ecs().count<state::FactionState>();
	const std::int64_t playerCount = world_.ecs().count<state::PlayerFaction>();
	if (sensorCache_.tick == tick && sensorCache_.factions == factionCount && sensorCache_.players == playerCount)
		return sensorCache_.ctx;
	sensor::Context ctx;
	// A fighter's `Fighter::faction` is a faction INSTANCE's state::Id value (residents.cpp), not an ECS entity id: the relation
	// lookup must resolve the ids through the same map the hostile_ refresh builds (updateHostility). Read as entity ids every pair
	// answered 0, so the sensor never set kHostile (task CF: the bandits stopped fighting the relation-hostile group at the den).
	auto factions = std::make_shared<std::unordered_map<std::uint64_t, flecs::entity>>();
	world_.each([&](flecs::entity entity, const state::FactionState&, const state::Id& id)
	{
		factions->emplace(id.value, entity);
	});
	world_.each([&](const state::Id& id, const state::PlayerFaction&)
	{
		if (ctx.playerFaction == 0 || id.value < ctx.playerFaction)
			ctx.playerFaction = id.value;
	});
	if (gameData_ != nullptr)
		ctx.relation = [this, factions](std::uint64_t from, std::uint64_t to)
		{
			const auto mine = factions->find(from);
			const auto theirs = factions->find(to);
			if (mine == factions->end() || theirs == factions->end())
				return 0.0;
			return factionRelation(*gameData_, mine->second, theirs->second);
		};
	sensorCache_ = {tick, factionCount, playerCount, ctx};
	return ctx;
}

worldstate::Facts Simulation::worldStateFacts() const
{
	worldstate::Facts facts;
	const flecs::world& ecs = world_.ecs();
	worldstate::collectStatuses(ecs, facts);
	// The player faction's relations: the FACTION itself, else its own default relation towards that faction.
	const std::string player = playerFactionSid();
	flecs::entity_t playerEntity = 0;
	std::unordered_map<std::string, flecs::entity_t> factions;
	world_.each([&](flecs::entity entity, const state::FactionState& faction)
	{
		factions.emplace(faction.templateSid, entity.id());
		if (entity.has<state::PlayerFaction>())
			playerEntity = entity.id();
	});
	if (gameData_ != nullptr && !player.empty() && playerEntity != 0)
	{
		facts.playerRelation = [this, playerEntity, factions](std::string_view sid) -> double
		{
			const auto found = factions.find(std::string(sid));
			if (found == factions.end())
				return 0.0;
			return playerStanding(*gameData_, flecs::entity(world_.ecs().c_ptr(), playerEntity),
				flecs::entity(world_.ecs().c_ptr(), found->second));
		};
	}
	return facts;
}

void Simulation::run(std::uint64_t ticks)
{
	for (std::uint64_t i = 0; i < ticks; ++i)
		tick();
}

void Simulation::enqueue(Command command)
{
	command.tick = std::max(command.tick, clockTick() + 1);
	pending_.push_back(std::move(command));
}

void Simulation::setWar(state::Id a, state::Id b)
{
	enqueue(Command::setFactionRelation(a, b, -100));
	enqueue(Command::setFactionRelation(b, a, -100));
}

void Simulation::applyNow(const Command& command)
{
	apply(command);
	if (removed_)
	{
		removed_ = false;
		instancesChanged();
	}
}

flecs::entity Simulation::spawnMover(double x, double z, std::int32_t group)
{
	flecs::world& ecs = ecsMutable();
	flecs::entity entity = state::createInstance(ecs);
	entity.set<state::Position>(state::Position{x, z});
	regions_->add(ecs, entity, group);
	return entity;
}

void Simulation::setCombatObserver(std::function<void(const CombatEvent&)> observer)
{
	combatObserver_ = std::move(observer);
	if (combat_ != nullptr)
		combat_->setObserver(combatObserver_);
}

void Simulation::emitRagdoll(std::uint64_t id, std::uint32_t cause)
{
	// The carry step's ragdoll starts (task BG): a put-down or a drop, cause 2. Same shape as the combat step's own events, so
	// the session's observer cannot tell them apart; no attacker, no part, no strength (a gentle release, not a blow).
	if (combat_ == nullptr)
		return;
	state::Fighter* f = combat_->find(id);
	if (f == nullptr)
		return;
	const double x = f->x;
	const double z = f->z;
	const std::uint32_t seed = combat_->rollRagdoll(*f, id);
	if (f->x != x || f->z != z)
		mirrorFighter(id);
	if (!combatObserver_)
		return;
	CombatEvent e;
	e.kind = CombatEvent::Kind::Ragdoll;
	e.time = combat_->time();
	e.who = id;
	e.code = static_cast<std::int32_t>(cause);
	e.seed = seed;
	combatObserver_(e);
}

void Simulation::setHitObserver(std::function<void(const HitEvent&)> observer)
{
	regions_->setHitObserver(std::move(observer));
}

void Simulation::applyCommands(std::uint64_t tick)
{
	// Due commands in enqueue order; later ones stay queued.
	std::vector<Command> later;
	for (Command& command : pending_)
	{
		if (command.tick > tick)
			later.push_back(std::move(command));
		else
			apply(command);
	}
	pending_ = std::move(later);
	if (removed_)
	{
		removed_ = false;
		instancesChanged();
	}
}

void Simulation::retireSquad(flecs::world& ecs, flecs::entity squad, std::uint64_t killerFaction)
{
	if (!squad.is_valid())
		return;
	// A kill is information flowing up to the squad's faction.
	if (const flecs::entity_t faction = squad.get<state::SquadInstance>()->faction.id)
	{
		// A dead ordinary squad costs its faction 10 prosperity and nothing else: no replacement timer (that exists only for the
		// UNIQUE_SQUAD_TEMPLATE `special squads` with a `replacement time`; kinspect item 54 TQ3, TOWN_LIFE_SLICE D9).
		if (state::FactionState* owner = flecs::entity(ecs.c_ptr(), faction).get_mut<state::FactionState>())
		{
			const double cap = gameData_ != nullptr ? factionMaxProsperity(*gameData_, owner->templateSid) : 1000.0;
			owner->prosperityLost = std::min(cap, owner->prosperityLost + trader::kSquadDeathProsperity);
		}
		pushFactionEvent(ecs, state::FactionEvent{state::EventKind::SquadLost,
			state::EntityRef{faction}, {}, clockTick()});
		// A killer faction: the victim's faction lowers its opinion of it by
		// reason 5 for each member, A = 0.025 x the member's `faction importance`.
		const flecs::entity killer = killerFaction != 0 ? factionInstance(ecs, killerFaction)
			: flecs::entity{};
		if (killer.is_valid())
		{
			const flecs::entity victim(ecs.c_ptr(), faction);
			ecs.query_builder<const state::CharacterInstance>()
				.with<state::MemberOf>(squad)
				.build()
				.each([&](const state::CharacterInstance& member)
				{
					const kdata::Record* record = gameData_->find(member.templateSid);
					applyRelationReason(*gameData_, victim, killer, RelationReason::Kill,
						kRelationUnheld * (record != nullptr ? record->floatOr("faction importance", 1.0f) : 1.0));
				});
		}
	}
	// A delivery caravan that dies takes its order with it (T4c: the goods never arrive). The T4e on_loss hook sees the order first.
	if (economyHooks_ != nullptr)
		if (const state::CaravanOrder* order = squad.get<state::CaravanOrder>())
			economyHooks_->onLoss(*order);
	removeSquadEntity(ecs, squad);
}

std::size_t Simulation::importClearSquads()
{
	flecs::world& ecs = ecsMutable();
	std::vector<flecs::entity> squads;
	ecs.each([&](flecs::entity squad, const state::SquadInstance&) { squads.push_back(squad); });
	for (const flecs::entity squad : squads)
		removeSquadEntity(ecs, squad);
	wild_.reset();
	instancesChanged();
	return squads.size();
}

// A spawner squad (state::SquadInstance::slot >= 0) with no member left at all is gone for the quota: retired like a removal (its
// faction pays the squad-death prosperity). That is the faction tick's "no members left" rule (kinspect s71_static.md 5.3); the
// members leave the array only when a corpse is destroyed (stepCorpses: decay or unload; bq15_corpse_cleanup.md a). So an all-dead
// squad stays, non-intact and counting 0.5 in the quota (squadVitals, wilderness topUp), until its last corpse goes. An all-KO'd squad
// stays until a member wakes or dies. Derived each strategic step, so a wiped squad found in a loaded save is retired too.
std::size_t Simulation::retireWipedSquads(flecs::world& ecs)
{
	std::vector<flecs::entity> wiped;
	ecs.each([&](flecs::entity squad, const state::SquadInstance& instance)
	{
		if (instance.slot < 0)
			return;
		if (squadVitals(ecs, squad).members == 0)
			wiped.push_back(squad);
	});
	for (flecs::entity squad : wiped)
		retireSquad(ecs, squad, 0);
	return wiped.size();
}

void Simulation::apply(const Command& command)
{
	flecs::world& ecs = ecsMutable();
	switch (command.type)
	{
	case CommandType::SetFactionRelation:
	{
		if (command.value < -100 || command.value > 100)
			break;
		const flecs::entity from = factionInstance(ecs, command.subject);
		const flecs::entity to = factionInstance(ecs, command.object);
		if (!from.is_valid() || !to.is_valid())
			break;
		const double before = factionRelation(*gameData_, from, to);
		from.set<state::Relation>(to, state::Relation{static_cast<double>(command.value)});
		if (from.id() != to.id())
			noteRelationChange(ecs, from, to, before, static_cast<double>(command.value));
		break;
	}
	case CommandType::SetPlayerAreas:
		regions_->setPlayerAreas(ecs, command.points);
		syncLineOfSight(); // the newly Full regions get their hull trees, the rest are freed (task AZ)
		break;
	case CommandType::SetSquadCentre:
		regions_->setGroupCentre(command.value, command.x, command.z);
		break;
	case CommandType::RemoveSquad:
	{
		retireSquad(ecs, squadInstance(ecs, command.subject), command.object);
		removed_ = true;
		break;
	}
	case CommandType::SetTownOwner:
	{
		const flecs::entity town = townInstance(ecs, command.subject);
		if (!town.is_valid())
			break;
		flecs::entity owner{};
		if (command.object != 0)
		{
			owner = factionInstance(ecs, command.object);
			if (!owner.is_valid())
				break;
		}
		state::TownState* state = town.get_mut<state::TownState>();
		if (state->owner.id == owner.id())
			break;
		if (state->owner.id != 0)
			pushFactionEvent(ecs, state::FactionEvent{state::EventKind::TownLost,
				state->owner, state::EntityRef{town.id()}, clockTick()});
		if (owner.is_valid())
			pushFactionEvent(ecs, state::FactionEvent{state::EventKind::TownGained,
				state::EntityRef{owner.id()}, state::EntityRef{town.id()}, clockTick()});
		state->owner = state::EntityRef{owner.id()};
		break;
	}
	case CommandType::FighterEquip:
	case CommandType::ContainerTransfer:
	{
		// Neither replaces the running order. The order was checked when it was given; checked again, a failure drops it.
		if (combat_ == nullptr || combat_->find(command.subject) == nullptr)
			break;
		if (command.type == CommandType::FighterEquip)
		{
			if (checkEquip(command.subject, command.text, command.value != 0).empty())
				equipItem(command.subject, command.text, command.value != 0);
		} else if (checkTransfer(command.subject, command.object, command.text, static_cast<std::uint32_t>(std::max(command.value, 0)),
			command.z != 0.0).empty()) {
			transferItem(command.subject, command.object, command.text, static_cast<std::uint32_t>(command.value), command.z != 0.0);
		}
		break;
	}
	case CommandType::InventoryOrder:
	{
		if (command.heights.size() != 7)
		{
			++badOrders_;
			break;
		}
		InventoryOrder order;
		order.kind = static_cast<std::uint16_t>(command.value);
		order.id = command.subject;
		order.container = command.object;
		order.version = static_cast<std::uint32_t>(command.heights[0]);
		order.index = static_cast<std::uint16_t>(command.heights[1]);
		order.count = static_cast<std::uint16_t>(command.heights[2]);
		order.section = static_cast<std::uint8_t>(command.heights[3]);
		order.x = static_cast<std::uint8_t>(command.heights[4]);
		order.y = static_cast<std::uint8_t>(command.heights[5]);
		order.flag = static_cast<std::uint8_t>(command.heights[6]);
		order.trader = command.text.empty() ? 0 : std::strtoull(command.text.c_str(), nullptr, 10);
		if (const std::size_t colon = command.text.find(':'); colon != std::string::npos)
		{
			order.source = std::strtoull(command.text.c_str() + colon + 1, nullptr, 10);
			if (const std::size_t second = command.text.find(':', colon + 1); second != std::string::npos)
				order.window = std::strtoull(command.text.c_str() + second + 1, nullptr, 10);
		}
		runInventoryOrder(order, true); // checked when it was given; checked again, a failure drops it
		break;
	}
	case CommandType::SquadOrder:
	{
		SquadOrder order;
		order.kind = static_cast<std::uint16_t>(command.value);
		if (order.kind == 73 || order.kind == 75)
			order.character = command.subject;
		else
			order.squad = command.subject;
		if (order.kind == 73)
			order.squad = command.object;
		order.index = static_cast<std::uint16_t>(command.x);
		order.swap = static_cast<std::uint8_t>(command.z);
		order.name = command.text;
		runSquadOrder(order, true); // checked when it was given; checked again, a failure drops it
		break;
	}
	case CommandType::JobOrder:
	{
		JobOrder order;
		order.kind = static_cast<std::uint16_t>(command.value);
		order.character = command.subject;
		order.target = command.object;
		order.type = order.kind == 76 ? static_cast<std::uint16_t>(command.x) : std::uint16_t{0};
		order.enabled = order.kind == 10 ? static_cast<std::uint8_t>(command.x) : std::uint8_t{0};
		order.jobId = order.kind == 77 || order.kind == 78 ? static_cast<std::uint32_t>(command.x) : 0u;
		order.index = static_cast<std::uint16_t>(command.z);
		runJobOrder(order, true); // checked when it was given; checked again, a failure drops it
		break;
	}
	case CommandType::Prospect:
		runProspect(command.subject, true); // checked when it was given; checked again, a failure drops it
		break;
	case CommandType::StealthKo:
		orderStealthKo(command.subject, command.object); // checked when it was given; checked again, a refusal drops it
		break;
	case CommandType::ShackleAction:
		orderShackleAction(command.subject, command.object, static_cast<std::uint8_t>(command.value)); // checked when it was given; checked again
		break;
	case CommandType::BuildPlace:
		if (command.heights.size() == 5)
		{
			BuildOrder order;
			order.record = command.text;
			order.x = command.x;
			order.z = command.z;
			order.y = command.heights[0];
			order.qw = command.heights[1];
			order.qx = command.heights[2];
			order.qy = command.heights[3];
			order.qz = command.heights[4];
			order.floor = static_cast<std::uint8_t>(command.value);
			runBuildPlace(order, true); // checked when it was given; checked again, a failure drops it
		}
		break;
	case CommandType::BuildPause:
		runBuildPause(command.subject, command.value != 0, true);
		break;
	case CommandType::DialogueReply:
		runDialogueReply(command.subject, command.object, static_cast<std::uint32_t>(command.x), static_cast<std::uint32_t>(command.value), true);
		break;
	case CommandType::EndConversation:
		runEndConversation(command.subject, command.object, static_cast<std::uint8_t>(command.value), true);
		break;
	case CommandType::ChangeSquad:
		runChangeSquad(command.subject, command.object, true); // checked when it was given; checked again, a failure drops it
		break;
	case CommandType::NewGame:
		runNewGame(command);
		break;
	case CommandType::FighterMoveTo:
	case CommandType::FighterAttack:
	case CommandType::FighterStop:
	case CommandType::FighterBlock:
	case CommandType::FighterDrive:
	case CommandType::FighterFollow:
	case CommandType::FighterSneak:
	case CommandType::FighterPassive:
	case CommandType::FighterHoldPosition:
	case CommandType::FighterInteract:
	case CommandType::FighterDrop:
	case CommandType::FighterRun:
	case CommandType::FighterSpeedMode:
	{
		state::Fighter* f = combat_ != nullptr ? combat_->find(command.subject) : nullptr;
		if (f == nullptr || f->dead)
			break;
		// Under Hold (DIALOGUE.md 13.4, user 2026-11-12) a move, attack, follow, drive or interact order of the player character that is not queued
		// ends the open menus addressed to it: walked away (reason 4); the order is then applied. Under Freeze no tick runs while a menu is open.
		const bool moves = command.type == CommandType::FighterMoveTo ? (command.value & 1) == 0
			: command.type == CommandType::FighterInteract ? (static_cast<std::uint64_t>(command.z) & 2) == 0
				&& !(conversation(command.object) != nullptr && conversation(command.object)->target == command.subject)
			: command.type == CommandType::FighterAttack || command.type == CommandType::FighterDrive || command.type == CommandType::FighterFollow;
		if (moves)
			endMenusOf(command.subject, 4);
		const MeshNavigator* nav = combat_->navigation();
		// A queued order (SIMLINK.md section 13) waits behind the character's running order and queue; it replaces nothing.
		// The move keeps its snapped goal, the interact its target: both are routed when they start (stepOrderQueues).
		if (command.type == CommandType::FighterMoveTo && (command.value & 1) != 0)
		{
			if (nav == nullptr || command.points.empty() || command.heights.size() != command.points.size())
				break;
			state::OrderQueue queue;
			if (const state::OrderQueue* held = fighterEntity(command.subject).get<state::OrderQueue>())
				queue = *held;
			queue.orders.push_back({1, static_cast<std::uint32_t>(command.object), 0, 0, command.points.back().x,
				command.heights.back(), command.points.back().z});
			fighterEntity(command.subject).set<state::OrderQueue>(std::move(queue));
			break;
		}
		if (command.type == CommandType::FighterInteract && (static_cast<int>(command.z) & 2) != 0)
		{
			if (nav == nullptr || command.value <= 0 || command.value > kInteractActionMax
				|| findInteractTarget(ecs, combat_.get(), command.object, this).kind == InteractKind::None)
				break;
			state::OrderQueue queue;
			if (const state::OrderQueue* held = fighterEntity(command.subject).get<state::OrderQueue>())
				queue = *held;
			queue.orders.push_back({2, static_cast<std::uint32_t>(command.x), command.object,
				static_cast<std::uint8_t>(command.value), 0.0, 0.0, 0.0, static_cast<std::uint8_t>(static_cast<int>(command.z) >> 2)});
			fighterEntity(command.subject).set<state::OrderQueue>(std::move(queue));
			break;
		}
		// A replacing order wakes a sleeper (a move order included), so an ordered character can always get up; the residents
		// wake the same way, when their goal changes (routine_scorer.cpp). A queued order replaces nothing yet: it waits.
		if (f->asleep)
		{
			f->asleep = false;
			// A player sleeper's bed came from the order (a Routine target the pose tail reads): it goes with the wake.
			if (state::Routine* routine = fighterEntity(command.subject).get_mut<state::Routine>())
				routine->target = {};
			mirrorFighter(command.subject);
		}
		// Any order but sneak and block ends a running interact order and empties the queue (the interact order sets its own below).
		const bool toggle = command.type == CommandType::FighterPassive || command.type == CommandType::FighterHoldPosition;
		if (command.type != CommandType::FighterSneak && command.type != CommandType::FighterBlock
			&& command.type != CommandType::FighterDrop && command.type != CommandType::FighterRun && !toggle)
			cancelOrders(command.subject);
		// Any order but sneak and block replaces the follow task (SQUAD_FOLLOW.md s9: following needs an order).
		if (command.type != CommandType::FighterSneak && command.type != CommandType::FighterBlock
			&& command.type != CommandType::FighterFollow && command.type != CommandType::FighterDrop
			&& command.type != CommandType::FighterRun && !toggle)
			combat_->endFollow(*f);
		if (command.type == CommandType::FighterMoveTo)
		{
			if (nav == nullptr || command.points.empty())
				break; // no navmesh, no route: no move
			// Heights come with the route (the server's own search made them); a mismatch is a caller bug, never a silent drop.
			if (command.heights.size() != command.points.size())
			{
				++badOrders_;
				break;
			}
			// A far goal (bit 2: its one point is the true goal, kenshi-k16.6.481) or a move over 1000 u that the roads serve walks in legs
			// (far_orders.cpp); anything else is the route the server made.
			const bool far = (command.value & 4) != 0;
			const state::RoutePoint goal = {command.points.back().x, command.heights.back(), command.points.back().z};
			const bool legs = (far || std::hypot(goal.x - f->x, goal.z - f->z) > kworld::RoadNetwork::kLongMove)
				&& startMoveOrder(command.subject, *f, goal.x, goal.z, goal.y, static_cast<std::uint32_t>(command.object), far);
			if (far && !legs)
				break; // no first leg: the start is off the mesh, or no exit of its zone has ground
			if (!legs)
			{
				std::vector<state::RoutePoint> route;
				for (std::size_t i = 0; i < command.points.size(); ++i)
					route.push_back({command.points[i].x, command.heights[i], command.points[i].z});
				if (!f->route.empty())
					f->clearWhy = state::Fighter::kClearNewOrder; // the order replaces a live route
				f->route = std::move(route);
				f->routeNext = 0;
				++f->routeVersion;
				f->routeSeq = static_cast<std::uint32_t>(command.object);
				f->routeChase = false; // a move order replaces an attack chase's route; it must not inherit its flag
				f->routeApproach = (command.value & 2) != 0;
				f->routeSource = state::Fighter::PlanSource::Order;
				f->routeTries = 0;
			}
			f->drive = state::DriveMode::Stop;
			f->driveStrafe = 0;
			f->orderTarget = 0;
			f->orderStop = true;
			f->hold = false;
		} else if (command.type == CommandType::FighterAttack) {
			const state::Fighter* target = combat_->find(command.object);
			// Kenshi lets the player attack anyone outside its own party (the player-controlled characters); team is a combat grouping
			// (worldTeam: every faction not hostile to the player shares team 0), not a party, so a neutral Hub NPC is a legal target.
			// An NPC ordered onto the player (fixtures, scripted fights) is not the player's party.
			if (target == nullptr || target->dead || (target->playerControlled && f->playerControlled))
				break;
			// A neutral target (the same team) takes the ordered character's squad as a personal hostile: onTargetedBy runs on the VICTIM
			// (dt23), and the order's attack task holds the target. The ordered character's own map gets nothing.
			// An unsensed victim of a ranged attacker learns of it only from a hit (CombatArena setTarget, rwh.191).
			if (target->team == f->team && !(f->gun().armed() && f->gun().ammo >= 1 && !perception::sensedWithin(*target, command.subject, perception::kRecordFresh)))
				combat_->markHostile(command.object, command.subject);
			f->orderTarget = command.object;
			f->clearRoute(state::Fighter::kClearNewOrder);
			f->drive = state::DriveMode::Stop;
			f->driveStrafe = 0;
			f->hold = false;
		} else if (command.type == CommandType::FighterStop) {
			f->clearRoute(state::Fighter::kClearNewOrder);
			f->drive = state::DriveMode::Stop;
			f->driveStrafe = 0;
			f->orderTarget = 0;
			f->orderStop = true;
			f->hold = command.value != 0;
		} else if (command.type == CommandType::FighterDrive) {
			if (nav == nullptr || command.value < 0 || command.value > 2)
				break;
			f->drive = static_cast<state::DriveMode>(command.value);
			f->driveHeading = command.x;
			f->driveStrafe = static_cast<std::int8_t>(command.z);
			f->driveSeq = static_cast<std::uint32_t>(command.object);
			f->driveBlocked = false;
			f->driveTurn = !f->driving() && f->hold; // a stop in the drive hold turns in place (SIMLINK.md section 12)
			f->clearRoute(state::Fighter::kClearNewOrder);
			if (f->driving())
			{
				f->orderTarget = 0;
				f->orderStop = true;
				f->hold = true; // the AI-off hold; stop leaves it (SIMLINK.md section 12)
			}
		} else if (command.type == CommandType::FighterFollow) {
			const state::Fighter* leader = combat_->find(command.object);
			if (nav == nullptr || leader == nullptr || leader->dead || command.object == command.subject
				|| combat_->followLoops(command.subject, command.object))
				break;
			combat_->endFollow(*f);
			f->clearRoute(state::Fighter::kClearNewOrder);
			f->drive = state::DriveMode::Stop;
			f->driveStrafe = 0;
			f->orderTarget = 0;
			f->orderStop = true;
			f->hold = false;
			f->followTarget = command.object;
			f->followSource = state::Fighter::PlanSource::Order;
		} else if (command.type == CommandType::FighterInteract) {
			if (nav == nullptr || command.value <= 0 || command.value > kInteractActionMax)
				break;
			const InteractTarget target = findInteractTarget(ecs, combat_.get(), command.object, this);
			if (target.kind == InteractKind::None)
				break;
			if (command.heights.size() != command.points.size())
			{
				++badOrders_;
				break;
			}
			std::vector<state::RoutePoint> route;
			for (std::size_t i = 0; i < command.points.size(); ++i)
				route.push_back({command.points[i].x, command.heights[i], command.points[i].z});
			beginInteract(command.subject, command.object, static_cast<std::uint8_t>(command.value),
				static_cast<std::uint32_t>(command.x), std::move(route), (static_cast<int>(command.z) & 1) != 0,
				static_cast<std::uint8_t>(static_cast<int>(command.z) >> 2));
		} else if (command.type == CommandType::FighterDrop) {
			dropItem(command.subject, command.text, static_cast<std::uint32_t>(std::max(command.value, 0)));
		} else if (command.type == CommandType::FighterRun) {
			// The old run toggle is the speed-mode alias (SIMLINK.md order 10 which 2): 1 = Run, 0 = Walk.
			f->moveMode = command.value != 0 ? 2 : 0;
			f->speedGroup = 0;
		} else if (command.type == CommandType::FighterSpeedMode) {
			const std::uint8_t mode = static_cast<std::uint8_t>(std::clamp(command.value, 0, 3));
			f->moveMode = mode;
			f->speedGroup = mode == 3 ? command.object : 0; // another mode leaves the group
		} else if (command.type == CommandType::FighterPassive) {
			f->passive = command.value != 0;
		} else if (command.type == CommandType::FighterHoldPosition) {
			f->holdPosition = command.value != 0;
		} else if (command.type == CommandType::FighterSneak) {
			f->sneak = command.value != 0;
		} else {
			f->blockMode = command.value != 0;
		}
		break;
	}
	case CommandType::SetView:
		ecs.set<state::ViewFocus>(state::ViewFocus{command.x, command.z});
		break;
	case CommandType::SetDoorState:
	{
		if (command.value > 3 || command.value < -1)
			break;
		state::DoorOverrides overrides;
		if (const state::DoorOverrides* current = ecs.get<state::DoorOverrides>())
			overrides = *current;
		std::vector<state::DoorOverride>& entries = overrides.entries;
		const auto at = std::lower_bound(entries.begin(), entries.end(), command.object,
			[](const state::DoorOverride& entry, std::uint64_t key) { return entry.key < key; });
		const bool present = at != entries.end() && at->key == command.object;
		if (command.value < 0)
		{
			if (present)
				entries.erase(at);
		} else if (present) {
			at->state = static_cast<std::uint8_t>(command.value);
		} else {
			entries.insert(at, state::DoorOverride{command.object,
				static_cast<std::uint8_t>(command.value)});
		}
		// No entries = no singleton, so a run that never set a door hashes as before.
		if (entries.empty())
			ecs.remove<state::DoorOverrides>();
		else
			ecs.set<state::DoorOverrides>(std::move(overrides));
		break;
	}
	}
}

void Simulation::runSystem(SystemState& system, std::uint64_t tick)
{
	RngStream& stream = rng_[static_cast<std::size_t>(subsystemFor(system.level))];
	system.checksum = (system.checksum * kFnvPrime) ^ stream.next();
	++system.runs;
	system.lastRunTick = tick;
}

void Simulation::mirrorScheduler()
{
	state::Scheduler scheduler;
	scheduler.seed = seed_;
	for (std::size_t i = 0; i < kSubsystemCount; ++i)
	{
		scheduler.subsystems.push_back(state::SubsystemState{rng_[i].state(),
			systems_[i].runs, systems_[i].checksum, systems_[i].lastRunTick});
	}
	ecsMutable().set<state::Scheduler>(std::move(scheduler));
}

std::uint64_t Simulation::rngState(Subsystem subsystem) const
{
	return rng_[static_cast<std::size_t>(subsystem)].state();
}

std::uint64_t Simulation::stateHash() const
{
	const ArenaMirror mirror(*this);
	return ksim::stateHash(world_.ecs());
}

} // namespace ksim
