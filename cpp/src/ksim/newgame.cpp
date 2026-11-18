// New game: the NEW_GAME_STARTOFF catalog, the readiness of a start, and the NewGame command (docs/MULTIPLAYER.md 4b / 4c,
// docs/reference/STARTS_CAGES_STATIC.md a). The session half (frames 111, 62, STARTS 33, order 84) is in simlink.cpp / simlink_saves.cpp.

#include "ksim/newgame.hpp"

#include "ksim/navigator.hpp"
#include "ksim/rng.hpp"
#include "ksim/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ksim {

namespace {

// The exe's fallback when a start has no `town` ref that resolves to a town (STARTS_CAGES_STATIC.md a1: DAT_1416c4d08 / 0c).
constexpr double kFallbackX = -43500.0;
constexpr double kFallbackZ = -6625.0;
// How far from a start position walkable ground may be (the click reach of MeshNavigator: a spot the player could walk to).
constexpr double kStartReach = MeshNavigator::kClickReach;

template <typename Func>
void forEachRef(const kdata::Record& record, std::string_view category, Func&& func)
{
	const kdata::RefCategory* refs = record.findRefs(category);
	if (refs == nullptr)
		return;
	for (const kdata::RefEntry& entry : refs->entries)
	{
		if (!entry.opaqueEntry)
			func(entry);
	}
}

bool isCharacterType(std::int32_t type)
{
	return type == kworld::itemTypeByName("CHARACTER") || type == kworld::itemTypeByName("HUMAN_CHARACTER");
}

const kdata::Record* namelessFaction(const kdata::GameData& gameData)
{
	const kdata::Record* found = nullptr;
	for (const kdata::Record* record : gameData.byType(kworld::itemTypeByName("FACTION")))
	{
		if (record->name == "Nameless")
			found = record;
	}
	return found;
}

std::optional<knav::Position> snapTo(const MeshNavigator& nav, const kworld::Heightmap& terrain, double x, double z, double reach)
{
	return nav.walker().snap({x, terrain.terrainHeight(x, z), z}, reach);
}

// A streaming mesh (MeshNavigator::openWorld) holds the zones around every place of the start besides what it holds, so the places can be
// tested and the squad placed. The areas take over on the next sync once the squad stands there (Simulation::syncNavCoverage).
void coverPlaces(const MeshNavigator& nav, const StartOff& start)
{
	if (!nav.streaming())
		return;
	std::vector<kworld::ZoneCell> wanted = nav.coveredZones();
	for (const StartOff::Place& place : start.places)
	{
		const kworld::ZoneCell at = kworld::zoneCellOf(place.x, place.z);
		for (int dz = -1; dz <= 1; ++dz)
		{
			for (int dx = -1; dx <= 1; ++dx)
				wanted.push_back({at.x + dx, at.z + dz});
		}
	}
	nav.setCoverage(wanted);
}

// One start position the squad can be placed at: the ready places in order, one of them drawn with `rng` (the first without one).
std::optional<knav::Position> pickPlace(const StartOff& start, const MeshNavigator& nav, const kworld::Heightmap& terrain, RngStream* rng)
{
	std::vector<knav::Position> ready;
	for (const StartOff::Place& place : start.places)
	{
		if (const auto spot = snapTo(nav, terrain, place.x, place.z, kStartReach))
			ready.push_back(*spot);
	}
	if (ready.empty())
		return std::nullopt;
	if (rng == nullptr || ready.size() == 1)
		return ready.front();
	return ready[static_cast<std::size_t>(rng->between(0, static_cast<std::int64_t>(ready.size()) - 1))];
}

// A creator Spec on a built fighter: the name, and the look CHAR_APPEARANCE sends (SIMLINK.md NEW_GAME). The race went into makeFighter.
void applySpec(state::Fighter& fighter, const CharacterSpec& spec, const CreatorCatalog& creator)
{
	fighter.name = spec.name;
	state::FighterLook& look = fighter.look;
	look.custom = true;
	look.female = spec.female;
	const std::vector<std::string>& heads = creator.find(spec.race)->heads[spec.female];
	const auto head = std::find(heads.begin(), heads.end(), spec.head);
	look.headIndex = head == heads.end() ? 0 : static_cast<std::uint16_t>(head - heads.begin());
	look.hair = spec.hair;
	look.beard = spec.beard;
	look.hairColour = spec.hairColour;
	look.skin = spec.skin;
	look.sliders.clear();
	for (const auto& [name, value] : spec.sliders)
		look.sliders.push_back({name, value});
	look.stance = spec.stance;
}

} // namespace

StartCatalog StartCatalog::build(const kdata::GameData& gameData, std::string_view dataDir)
{
	StartCatalog catalog;
	const std::vector<const kdata::Record*> records = gameData.byType(kworld::itemTypeByName("NEW_GAME_STARTOFF"));
	if (records.empty())
		return catalog;
	const kworld::Towns towns = kworld::Towns::join(gameData, dataDir);
	const auto townPlace = [&](const std::string& sid) -> std::optional<StartOff::Place>
	{
		for (const kworld::TownRecord& town : towns.towns())
		{
			if (town.stringID == sid && !town.placementIndices.empty())
			{
				const kworld::TownPlacement& at = towns.placements()[town.placementIndices.front()];
				return StartOff::Place{sid, at.x, at.z};
			}
		}
		return std::nullopt;
	};
	for (const kdata::Record* record : records)
	{
		if (record->boolOr("REMOVED", false))
			continue;
		StartOff start;
		start.sid = record->stringID;
		start.name = record->name;
		start.money = record->intOr("money", 0);
		start.forcePos = record->boolOr("force start pos", false);
		if (start.forcePos)
		{
			start.places.push_back({"", static_cast<double>(record->intOr("start pos X", 0)), static_cast<double>(record->intOr("start pos Z", 0))});
		} else {
			forEachRef(*record, "town", [&](const kdata::RefEntry& entry)
			{
				if (const auto place = townPlace(entry.targetID))
					start.places.push_back(*place);
			});
			if (start.places.empty())
				start.places.push_back({"", kFallbackX, kFallbackZ});
		}
		forEachRef(*record, "squad", [&](const kdata::RefEntry& entry)
		{
			start.squads.push_back({entry.targetID, entry.v0});
		});
		forEachRef(*record, "faction relations", [&](const kdata::RefEntry& entry)
		{
			start.relations.emplace_back(entry.targetID, entry.v0);
		});
		forEachRef(*record, "research", [&](const kdata::RefEntry& entry)
		{
			start.research.push_back(entry.targetID);
		});
		forEachRef(*record, "force race", [&](const kdata::RefEntry& entry)
		{
			start.forceRace.push_back(entry.targetID);
		});
		catalog.starts_.push_back(std::move(start));
	}
	return catalog;
}

const StartOff* StartCatalog::find(std::string_view sid) const noexcept
{
	for (const StartOff& start : starts_)
	{
		if (start.sid == sid)
			return &start;
	}
	return nullptr;
}

std::vector<StartMember> startMembers(const kdata::GameData& gameData, const StartOff& start, RngStream* rng)
{
	std::vector<StartMember> members;
	const std::int32_t templateType = kworld::itemTypeByName("SQUAD_TEMPLATE");
	const auto add = [&](const std::string& character, const std::string& squad)
	{
		const kdata::Record* record = gameData.find(character);
		if (record != nullptr && isCharacterType(record->type))
			members.push_back({character, squad});
	};
	for (const StartOff::SquadRef& ref : start.squads)
	{
		const kdata::Record* record = gameData.find(ref.sid);
		if (record == nullptr)
			continue;
		if (record->type != templateType)
		{
			// A bare CHARACTER (Holy Sword's Vagabond): v0 of them, 0 counts as 1.
			for (std::int32_t i = 0; i < std::max(1, ref.count); ++i)
				add(ref.sid, "");
			continue;
		}
		forEachRef(*record, "leader", [&](const kdata::RefEntry& entry)
		{
			for (std::int32_t i = 0; i < entry.v0; ++i)
				add(entry.targetID, ref.sid);
		});
		for (const std::string_view list : {"squad", "squad2"})
		{
			forEachRef(*record, list, [&](const kdata::RefEntry& entry)
			{
				const std::int64_t count = entry.v1 == 0 || rng == nullptr ? entry.v0 : rng->between(entry.v0, std::max(entry.v0, entry.v1));
				for (std::int64_t i = 0; i < count; ++i)
					add(entry.targetID, ref.sid);
			});
		}
		// `choosefrom list`: the default-world spawner's draw (spawn.cpp spawnChooseFrom): weighted picks by v0, with replacement.
		std::vector<std::pair<std::string, std::int64_t>> choices;
		std::int64_t total = 0;
		forEachRef(*record, "choosefrom list", [&](const kdata::RefEntry& entry)
		{
			const kdata::Record* target = gameData.find(entry.targetID);
			if (target == nullptr || !isCharacterType(target->type) || entry.v0 <= 0)
				return;
			choices.emplace_back(entry.targetID, entry.v0);
			total += entry.v0;
		});
		if (!choices.empty())
		{
			const std::int32_t low = record->intOr("num random chars", 0);
			const std::int32_t high = record->intOr("num random chars max", 0);
			const std::int64_t draws = rng != nullptr ? rng->between(low, std::max(low, high)) : low;
			for (std::int64_t i = 0; i < draws; ++i)
			{
				std::int64_t pick = rng != nullptr ? rng->between(0, total - 1) : 0;
				for (const auto& [sid, weight] : choices)
				{
					if (pick < weight)
					{
						add(sid, ref.sid);
						break;
					}
					pick -= weight;
				}
			}
		}
	}
	return members;
}

std::vector<std::string> startResearch(const kdata::GameData& gameData, const std::vector<std::string>& wanted)
{
	const std::int32_t researchType = kworld::itemTypeByName("RESEARCH");
	const auto live = [&](const std::string& sid)
	{
		const kdata::Record* record = gameData.find(sid);
		return record != nullptr && record->type == researchType && !record->boolOr("REMOVED", false);
	};
	std::vector<std::string> done;
	for (const std::string& sid : wanted)
	{
		if (live(sid) && std::find(done.begin(), done.end(), sid) == done.end())
			done.push_back(sid);
	}
	if (done.empty() && live(std::string(kDefaultStartResearch)))
		done.emplace_back(kDefaultStartResearch);
	return done;
}

std::string checkFactionName(const std::string& name)
{
	if (name.empty() || !validSquadName(name))
		return "bad_name";
	bool visible = false;
	for (const char c : name)
	{
		const auto byte = static_cast<unsigned char>(c);
		if (byte < 0x20 || byte == 0x7F)
			return "bad_name";
		visible = visible || (byte != ' ' && byte != 0xC2 && byte != 0xA0);
	}
	return visible ? "" : "bad_name";
}

// ---- the Simulation half ----

const StartCatalog* Simulation::startCatalog() const
{
	if (gameData_ == nullptr)
		return nullptr;
	if (starts_ == nullptr)
		starts_ = std::make_shared<const StartCatalog>(StartCatalog::build(*gameData_, world_.dataDir()));
	return starts_.get();
}

std::vector<Simulation::StartReady> Simulation::startStatuses() const
{
	std::vector<StartReady> statuses;
	const StartCatalog* catalog = startCatalog();
	if (catalog == nullptr)
		return statuses;
	std::optional<kworld::Heightmap> terrain;
	for (const StartOff& start : catalog->all())
	{
		StartReady status;
		status.sid = start.sid;
		const std::vector<std::string> allowed = creatorCatalog()->allowedRaces(start.forceRace);
		if (allowed.size() != creatorCatalog()->races().size())
			status.races = allowed;
		for (const StartMember& member : startSquad(start))
			status.members.push_back({member.race, member.female, member.name, static_cast<std::uint8_t>(member.editable ? 1 : 0),
				member.character, member.armour, member.weapon});
		if (navigation_ != nullptr)
		{
			if (!terrain)
				terrain = kworld::Heightmap::open(world_.dataDir() + "/newland/land/fullmap.tif");
			// A streaming mesh does not hold the start's zone yet: ready when a place has a bake to load (UNSURE that it is on the mesh).
			if (navigation_->streaming())
			{
				for (const StartOff::Place& place : start.places)
					status.ready = status.ready || navigation_->canCover(kworld::zoneCellOf(place.x, place.z));
			} else {
				status.ready = pickPlace(start, *navigation_, *terrain, nullptr).has_value();
			}
		}
		statuses.push_back(std::move(status));
	}
	return statuses;
}

const CreatorCatalog* Simulation::creatorCatalog() const
{
	if (gameData_ == nullptr)
		return nullptr;
	if (creator_ == nullptr)
		creator_ = std::make_shared<const CreatorCatalog>(CreatorCatalog::build(*gameData_, world_.dataDir()));
	return creator_.get();
}

std::vector<StartMember> Simulation::startSquad(const StartOff& start) const
{
	std::uint64_t hash = 14695981039346656037ull;
	for (const char c : start.sid)
		hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
	RngStream rng(streamSeed(streamSeed(seed_, kStartSquadStream), static_cast<std::size_t>(hash)));
	std::vector<StartMember> members = startMembers(*gameData_, start, &rng);
	const CreatorCatalog* creator = creatorCatalog();
	for (StartMember& member : members)
	{
		member.rng = rng.next() | 1;
		const kdata::Record* record = gameData_->find(member.character);
		std::uint64_t state = member.rng;
		if (combatData_ == nullptr)
		{
			member.name = record != nullptr ? record->name : std::string();
			continue;
		}
		try
		{
			const state::Fighter fighter = combatData_->makeFighter(*gameData_, member.character, "", state, member.squadTemplate);
			member.race = fighter.raceSid;
			member.name = fighter.name;
			member.female = fighter.look.female; // CombatData::rollFemale, the same roll every spawn path makes
			const CreatorRace* race = fighter.animal ? nullptr : creator->find(fighter.raceSid);
			member.editable = race != nullptr;
			for (const state::FighterArmour& piece : fighter.armour)
				member.armour.push_back(piece.sid);
			member.weapon = fighter.weapon.sid;
			if (race != nullptr && !race->sex(member.female != 0))
				member.female = member.female ? 0 : 1;
		} catch (const std::exception&) {
			member.race.clear();
			member.name = record != nullptr ? record->name : std::string();
		}
	}
	return members;
}

std::string Simulation::checkNewGame(const std::string& startoff, const std::string& name, const std::vector<CharacterSpec>& specs) const
{
	const StartCatalog* catalog = startCatalog();
	if (catalog == nullptr || combat_ == nullptr || navigation_ == nullptr || namelessFaction(*gameData_) == nullptr)
		return "unsupported";
	const StartOff* start = catalog->find(startoff);
	if (start == nullptr)
		return "unknown_startoff";
	bool hasFaction = false;
	world_.ecs().each([&](const state::PlayerFaction&)
	{
		hasFaction = true;
	});
	if (hasFaction)
		return "has_faction";
	if (const std::string reason = checkFactionName(name); !reason.empty())
		return reason;
	std::vector<StartMember> squad;
	if (!specs.empty())
	{
		squad = startSquad(*start);
		if (specs.size() != squad.size())
			return "squad_size";
	}
	const kworld::Heightmap terrain = kworld::Heightmap::open(world_.dataDir() + "/newland/land/fullmap.tif");
	coverPlaces(*navigation_, *start);
	if (!pickPlace(*start, *navigation_, terrain, nullptr))
		return "not_ready";
	// A Spec list (character creation) must be something the client's creator can produce for this start-off.
	if (specs.empty())
		return std::string();
	std::vector<SquadSlot> slots;
	for (const StartMember& member : squad)
		slots.push_back({member.race, member.female, member.editable});
	return creatorCatalog()->check(start->forceRace, specs, slots);
}

void Simulation::addNewGameObserver(const void* owner, std::function<void(const NewGameOutcome&)> observer)
{
	removeNewGameObserver(owner);
	newGameObservers_.emplace_back(owner, std::move(observer));
}

void Simulation::removeNewGameObserver(const void* owner)
{
	newGameObservers_.erase(std::remove_if(newGameObservers_.begin(), newGameObservers_.end(),
		[owner](const auto& entry) { return entry.first == owner; }), newGameObservers_.end());
}

void Simulation::markPristine()
{
	std::ostringstream bytes;
	save(bytes);
	pristine_ = std::make_shared<const std::string>(bytes.str());
}

void Simulation::newWorld()
{
	if (pristine_ == nullptr)
		throw std::logic_error("newWorld: no markPristine");
	std::istringstream in(*pristine_);
	reload(in);
	scenarioLockedCages_.clear();
}

void Simulation::stepUnlocks()
{
	if (gameData_ == nullptr || ecsMutable().count<state::PlayerFaction>() == 0)
		return;
	std::vector<std::pair<flecs::entity, state::PlayerFaction>> empty;
	ecsMutable().each([&](flecs::entity entity, const state::PlayerFaction& faction)
	{
		if (faction.research.empty())
			empty.emplace_back(entity, faction);
	});
	if (empty.empty())
		return;
	const std::vector<std::string> start = startResearch(*gameData_, {});
	for (auto& [entity, faction] : empty)
	{
		faction.research = start;
		entity.set<state::PlayerFaction>(std::move(faction));
	}
}

std::vector<std::string> Simulation::completedResearch() const
{
	std::vector<std::string> research;
	std::uint64_t lowest = 0;
	world_.each([&](const state::Id& id, const state::PlayerFaction& faction)
	{
		if (lowest == 0 || id.value < lowest)
		{
			lowest = id.value;
			research = faction.research;
		}
	});
	return research;
}

const research::Snapshot& Simulation::researchState() const
{
	static const research::Snapshot none;
	if (gameData_ == nullptr)
		return none;
	const std::vector<std::string> done = completedResearch();
	if (researchSnapshot_ == nullptr || researchData_ != gameData_.get() || researchFor_ != done)
	{
		if (researchBook_ == nullptr || researchData_ != gameData_.get())
			researchBook_ = std::make_shared<const research::Book>(*gameData_);
		researchSnapshot_ = std::make_shared<const research::Snapshot>(researchBook_->evaluate(done));
		researchFor_ = done;
		researchData_ = gameData_.get();
	}
	return *researchSnapshot_;
}

std::vector<std::string> Simulation::unlockedBuildings(const std::vector<std::string>& research) const
{
	std::vector<std::string> buildings;
	if (gameData_ == nullptr || research.empty())
		return buildings;
	std::set<std::string> enabled;
	for (const std::string& sid : research)
	{
		if (const kdata::Record* record = gameData_->find(sid))
			forEachRef(*record, "enable buildings", [&](const kdata::RefEntry& entry) { enabled.insert(entry.targetID); });
	}
	for (const kdata::Record* record : gameData_->byType(kworld::itemTypeByName("BUILDING")))
	{
		if (!record->boolOr("REMOVED", false) && enabled.count(record->stringID) != 0)
			buildings.push_back(record->stringID);
	}
	return buildings;
}

// A NEW_GAME's options section ("cod=1.5;gdm=1;ht=4", SIMLINK NEW_GAME): the keys are the exe's new-game options
// (ADVANCED_OPTIONS_STATIC.md), each range-checked. False on a malformed pair or an unknown key; `options` is untouched then.
bool parseNewGameOptions(std::string_view text, state::WorldOptions& options)
{
	state::WorldOptions parsed = options;
	for (std::size_t at = 0; at < text.size();)
	{
		const std::size_t end = text.find(';', at);
		const std::string_view pair = text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
		at = end == std::string_view::npos ? text.size() : end + 1;
		const std::size_t equals = pair.find('=');
		if (equals == std::string_view::npos)
			return false;
		const std::string key(pair.substr(0, equals));
		const std::string digits(pair.substr(equals + 1));
		char* stop = nullptr;
		const double value = std::strtod(digits.c_str(), &stop);
		if (stop == digits.c_str() || *stop != '\0' || !std::isfinite(value))
			return false;
		if (key == "cod" && value >= state::WorldOptions::kChanceOfDeathMin && value <= state::WorldOptions::kChanceOfDeathMax)
			parsed.chanceOfDeath = value;
		else if (key == "gdm" && value >= state::WorldOptions::kDamageScaleMin && value <= state::WorldOptions::kDamageScaleMax)
			parsed.damageScale = value;
		else if (key == "ht" && value >= state::WorldOptions::kHungerTimeMin && value <= state::WorldOptions::kHungerTimeMax)
			parsed.hungerTime = value;
		else
			return false;
	}
	options = parsed;
	return true;
}

void Simulation::runNewGame(const Command& command)
{
	const std::size_t cut = command.text.find('\n');
	const std::string startoff = command.text.substr(0, cut);
	// startoff '\n' name, then an optional '\nopt:' options section (SIMLINK NEW_GAME's advanced options) and, for a
	// character-creation start, '\n' and the encoded Specs (a name has no newline: bad_name).
	const std::size_t cut2 = cut == std::string::npos ? cut : command.text.find('\n', cut + 1);
	const std::string name = cut == std::string::npos ? std::string() : command.text.substr(cut + 1, cut2 == std::string::npos ? cut2 : cut2 - cut - 1);
	std::string rest = cut2 == std::string::npos ? std::string() : command.text.substr(cut2 + 1);
	// A world without a NEW_GAME options section keeps the server's settings as its effective options.
	state::WorldOptions options;
	options.chanceOfDeath = combatSettings_.chanceOfDeath;
	options.damageScale = combatSettings_.damageScale;
	options.hungerTime = combatSettings_.hungerTime;
	bool decoded = true;
	if (rest.rfind("opt:", 0) == 0)
	{
		const std::size_t end = rest.find('\n');
		decoded = parseNewGameOptions(std::string_view(rest).substr(4, end == std::string::npos ? end : end - 4), options);
		rest = end == std::string::npos ? std::string() : rest.substr(end + 1);
	}
	std::vector<CharacterSpec> specs;
	decoded = decoded && (rest.empty() || decodeSpecs(rest, specs));
	NewGameOutcome outcome;
	outcome.ticket = command.object;
	outcome.tick = clockTick();
	const auto finish = [&]()
	{
		for (const auto& entry : newGameObservers_)
			entry.second(outcome);
		settleDue_ = outcome.reason.empty() && settleTicks_ != 0 && !settling_; // the world settles at the end of this tick (settleNewGame)
	};
	outcome.reason = decoded ? checkNewGame(startoff, name, specs) : std::string("unsupported");
	if (!outcome.reason.empty())
	{
		finish();
		return;
	}
	const StartOff& start = *startCatalog()->find(startoff);
	const kdata::Record& nameless = *namelessFaction(*gameData_);
	const kworld::Heightmap terrain = kworld::Heightmap::open(world_.dataDir() + "/newland/land/fullmap.tif");
	RngStream rng(streamSeed(streamSeed(seed_, kPlayerStartStream), clockTick()));
	const knav::Position base = *pickPlace(start, *navigation_, terrain, &rng);
	const std::vector<StartMember> wanted = startSquad(start);
	if (wanted.empty() || (!specs.empty() && specs.size() != wanted.size()))
	{
		outcome.reason = "unsupported";
		finish();
		return;
	}

	// Every character is built before anything changes: a record the combat data cannot build refuses the whole start.
	std::vector<state::Fighter> fighters;
	try
	{
		for (std::size_t i = 0; i < wanted.size(); ++i)
		{
			const StartMember& member = wanted[i];
			// The member's own draw (the one STARTS showed). A Spec replaces an editable member's race (with the race's stats), name and look;
			// a non-editable one keeps its race and takes only the Spec's name. The rest is the template's.
			std::uint64_t fighterRng = member.rng;
			const bool editable = member.editable && !specs.empty();
			state::Fighter fighter = combatData_->makeFighter(*gameData_, member.character, editable ? std::string_view(specs[i].race) : std::string_view(),
				fighterRng, member.squadTemplate);
			fighter.look.female = member.female;
			if (editable)
				applySpec(fighter, specs[i], *creatorCatalog());
			else if (!specs.empty())
				fighter.name = specs[i].name;
			fighter.team = 0;
			fighter.playerControlled = true;
			if (combatSettings_.newGameStealth >= 0.0)
				fighter.skills.stealth = combatSettings_.newGameStealth;
			// The exe scatters each by +-50 (a1.3); the spot must be walkable, else the start position itself.
			const double jx = static_cast<double>(rng.between(-50, 50));
			const double jz = static_cast<double>(rng.between(-50, 50));
			const std::optional<knav::Position> spot = snapTo(*navigation_, terrain, base.x + jx, base.z + jz, 100.0);
			const knav::Position at = spot ? *spot : base;
			fighter.x = at.x;
			fighter.y = at.y;
			fighter.z = at.z;
			fighters.push_back(std::move(fighter));
		}
	} catch (const std::exception&) {
		outcome.reason = "unsupported";
		finish();
		return;
	}

	flecs::world& ecs = ecsMutable();
	// The world's advanced options (ADVANCED_OPTIONS_STATIC.md "Ours"): saved from here on, and live in the combat arena.
	ecs.set<state::WorldOptions>(options);
	applyGameOptions();
	outcome.faction = createPlayerFaction(nameless.stringID);
	outcome.factionSid = nameless.stringID;
	flecs::entity player{};
	ecs.each([&](flecs::entity entity, const state::Id& id, const state::PlayerFaction&)
	{
		if (id.value == outcome.faction)
			player = entity;
	});
	{
		state::PlayerFaction faction = *player.get<state::PlayerFaction>();
		faction.name = name;
		faction.money = newGameMoney_ >= 0 ? newGameMoney_ : start.money;
		faction.startoff = start.sid;
		faction.research = startResearch(*gameData_, start.research);
		player.set<state::PlayerFaction>(std::move(faction));
	}

	// Relations: the template's own (spawnFactions already seeded them in a world with faction instances; a bare world gets them
	// here) and the start-off's `faction relations` in both directions (a2.2: only v0, no clamp in the exe; ours keeps -100..100).
	const auto factionOf = [&](const std::string& sid) -> flecs::entity
	{
		flecs::entity found{};
		ecs.each([&](flecs::entity entity, const state::FactionState& faction)
		{
			if (faction.templateSid == sid)
				found = entity;
		});
		return found;
	};
	forEachRef(nameless, "relations", [&](const kdata::RefEntry& entry)
	{
		const flecs::entity to = factionOf(entry.targetID);
		if (to.is_valid() && to.id() != player.id() && player.get<state::Relation>(to) == nullptr)
			player.set<state::Relation>(to, state::Relation{static_cast<double>(entry.v0)});
	});
	for (const auto& [sid, value] : start.relations)
	{
		const flecs::entity other = factionOf(sid);
		if (!other.is_valid() || other.id() == player.id())
			continue;
		const double clamped = static_cast<double>(std::clamp(value, -100, 100));
		other.set<state::Relation>(player, state::Relation{clamped});
		player.set<state::Relation>(other, state::Relation{clamped});
	}

	seedKnownFactions(player); // the factions the template and the start-off set a relation for (UNSURE, kinspect 49)

	// The squad: one player squad, leader first.
	const std::uint64_t squad = addPlayerSquad();
	for (state::Fighter& fighter : fighters)
	{
		flecs::entity entity = spawnFighter(std::move(fighter));
		entity.set<state::Allegiance>(state::Allegiance{nameless.stringID});
		const std::uint64_t id = entity.get<state::Id>()->value;
		// --give-item (testing only): the extra items every player character's pack holds (after the start's own gear).
		if (!newGameItems_.empty())
		{
			state::Inventory pack = entity.get<state::Inventory>() != nullptr ? *entity.get<state::Inventory>() : state::Inventory{};
			for (const auto& [sid, count] : newGameItems_)
				pack.add(sid, count);
			++pack.version;
			entity.set<state::Inventory>(std::move(pack));
		}
		addPlayerMember(squad, id);
		if (outcome.members.empty())
		{
			const state::Fighter* leader = combat_->find(id);
			outcome.x = leader->x;
			outcome.y = leader->y;
			outcome.z = leader->z;
		}
		outcome.members.push_back(id);
	}
	stepDiscovery(true); // the start position's first check
	finish();
}

} // namespace ksim
