#include "ksim/simlink_scenario.hpp"

#include "ksim/navigator.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"

#include <iostream>
#include <stdexcept>

namespace ksim::simlink {

namespace {

const kdata::Record& byName(const kdata::GameData& gd, const char* type, std::string_view name)
{
	const kdata::Record* found = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			found = r;
	}
	if (found == nullptr)
		throw std::runtime_error(std::string("scenario: no ") + type + " named " + std::string(name));
	return *found;
}

const kdata::Record* findByName(const kdata::GameData& gd, const char* type, std::string_view name)
{
	const kdata::Record* found = nullptr;
	for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(type)))
	{
		if (r->name == name)
			found = r;
	}
	return found;
}

// Engival at the start of the duel_d1 log (skills of its join record): fists, Rag Loincloth. Built on
// the race and anatomy of `body`. UNSURE: heal factor and the robot legs of the log are not copied
// (a plain Greenlander body).
state::Fighter makeEngival(const kdata::GameData& gd, const CombatData& data, const kdata::Record& body,
	std::uint64_t& rng)
{
	state::Fighter player = data.makeFighter(gd, body.stringID, "", rng);
	player.skills = {};
	player.skills.strength = 86.109581;
	player.skills.dexterity = 24.5625725;
	player.skills.toughness = 55.527668;
	player.skills.athletics = 90.5490952;
	player.skills.dodge = 37.3559723;
	player.skills.blunt = 1.46064448;
	player.skills.unarmed = 24.0728703;
	player.skills.attack = 5.05585909;
	player.skills.defence = 4.34450102;
	player.weapon = data.makeFists(gd);
	player.armour.clear();
	state::ItemQuality loincloth; // clothing without part coverage: level 1, no grade
	loincloth.level = 1.0;
	player.armour.push_back(data.buildArmour(byName(gd, "ARMOUR", "Rag Loincloth"), loincloth));
	player.name = "Engival";
	player.team = 0;
	player.playerControlled = true;
	return player;
}

// Places a fighter on the navmesh's nearest walkable ground (a placement, not a movement: no
// position_corrected event).
void placeOnMesh(const MeshNavigator& nav, const kworld::Heightmap& terrain, state::Fighter& fighter, double reach)
{
	const auto spot = nav.walker().snap({fighter.x, terrain.terrainHeight(fighter.x, fighter.z), fighter.z}, reach);
	if (!spot)
		throw std::runtime_error("scenario: a character's ground is not on the navmesh (" + std::to_string(fighter.x) + ", "
			+ std::to_string(fighter.z) + ", reach " + std::to_string(reach) + ")");
	fighter.x = spot->x;
	fighter.y = spot->y;
	fighter.z = spot->z;
}

// A stack of `count` of item record `sid` on the ground at (x, z): its use point is the nearest walkable ground, its height
// that point's (UNSURE: the item rests on the surface it is picked up from).
void dropItem(Simulation& sim, const MeshNavigator& nav, const kworld::Heightmap& terrain, const std::string& sid,
	std::uint32_t count, double x, double z)
{
	const auto spot = nav.walker().snap({x, terrain.terrainHeight(x, z), z}, 100.0);
	if (!spot)
		throw std::runtime_error("scenario: a ground item's spot is not on the navmesh (" + std::to_string(x) + ", " + std::to_string(z) + ")");
	state::GroundItem item;
	item.itemSid = sid;
	item.count = count;
	// A weapon or armour piece lies there with a generated quality (grade 2, "standard", by the fixed roll; the first fit manufacturer).
	const kdata::Record* record = sim.gameData()->find(sid);
	if (record != nullptr && (record->type == kworld::itemTypeByName("WEAPON") || record->type == kworld::itemTypeByName("ARMOUR")))
		item.quality = sim.combat()->data().rollQuality(*sim.gameData(), *record, 0.75, 0.0);
	item.x = spot->x;
	item.y = spot->y;
	item.z = spot->z;
	item.useX = spot->x;
	item.useY = spot->y;
	item.useZ = spot->z;
	sim.spawnGroundItem(std::move(item));
}

// The scenarios' stand-in for the Hub's storage: two crates beside the player, filled from the "house contents" VENDOR_LIST
// (UNSURE, interact.hpp spawnContainerAt). The original builds a town's interior furniture at runtime and no static data places
// it, so the Hub mesh has no placed container to snap to.
void placeCrates(Simulation& sim, const MeshNavigator& nav, const kdata::GameData& gd, const state::Fighter& anchor)
{
	const kdata::Record* contents = findByName(gd, "VENDOR_LIST", "house contents");
	spawnContainerAt(sim, nav, contents, byName(gd, "BUILDING", "Metal Box").stringID, anchor.x + 22.0, anchor.y, anchor.z - 16.0, 1);
	spawnContainerAt(sim, nav, contents, byName(gd, "BUILDING", "Booze Barrel").stringID, anchor.x - 18.0, anchor.y, anchor.z - 22.0, 2);
}

// The heaviest plate the data holds, one piece per outer slot, chosen by fields (no names, so a different mod set picks its own):
// ARMOUR records of material type 3 (plate) and class 3 (heavy) with part coverage, per AttachSlot 3 hat, 5 body, 6 legs, 9 boots
// (SIMLINK.md INVENTORY); the most coverage (the sum of the `part coverage` v0 percentages) wins, a tie goes to the lowest record
// id. In the stock data (gamedata.base) that is a Samurai Helmet or Tin Can (tie), Samurai Armour, Samurai Legplates and Samurai
// Boots; a mod's heavy plate with more coverage replaces them. The picks are printed at startup.
std::vector<state::FighterArmour> fullPlate(const kdata::GameData& gd, const CombatData& data)
{
	std::vector<state::FighterArmour> plate;
	for (const std::int32_t slot : {3, 5, 6, 9})
	{
		const kdata::Record* best = nullptr;
		std::int64_t bestCover = 0;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("ARMOUR")))
		{
			const kdata::RefCategory* coverage = r->findRefs("part coverage");
			if (coverage == nullptr || r->intOr("slot", -1) != slot || r->intOr("material type", -1) != 3 || r->intOr("class", -1) != 3)
				continue;
			std::int64_t cover = 0;
			for (const kdata::RefEntry& entry : coverage->entries)
				cover += entry.opaqueEntry ? 0 : entry.v0;
			if (best == nullptr || cover > bestCover || (cover == bestCover && r->id < best->id))
			{
				best = r;
				bestCover = cover;
			}
		}
		if (best == nullptr)
			throw std::runtime_error("scenario: no plate armour (material 3, class 3) for slot " + std::to_string(slot));
		plate.push_back(data.buildArmour(*best, data.rollQuality(gd, *best, 0.5, 0.5))); // the stock roll at its middle, as the katana
		std::cerr << "ksim_server: duel_armour: slot " << slot << " " << best->name << " (" << best->stringID << ")\n";
	}
	return plate;
}

} // namespace

Duel spawnDuel(Simulation& sim, const kdata::GameData& gd, const CombatData& data, ServerContext& context, bool katana, bool playerSquads, bool enemyArmour)
{
	const kdata::Record& servantRecord = byName(gd, "CHARACTER", "Escaped Servant");
	std::uint64_t rng = 12345;

	// The servant: as the combat tests spawn it.
	state::Fighter servant = data.makeFighter(gd, servantRecord.stringID, "", rng);
	servant.team = 1;
	servant.x = kHubX + 24.0;
	servant.z = kHubZ + 32.0;
	servant.yaw = 3.14159265358979323846; // facing the player
	if (enemyArmour)
		servant.armour = fullPlate(gd, data); // replaces the Escaped Servant's rags

	state::Fighter player = makeEngival(gd, data, servantRecord, rng);
	player.x = kHubX;
	player.z = kHubZ;
	player.yaw = 0.0;
	if (katana)
	{
		// A cutting weapon so the servant's wounds bleed (fists bleed at 0.05: a fist KO grows no pool). Quality by the
		// stock roll at its middle (rollQuality 0.5, 0.5): a reproducible weapon, not one the duel log carried.
		const kdata::Record& record = byName(gd, "WEAPON", "Katana");
		player.weapon = data.buildWeapon(gd, record, data.rollQuality(gd, record, 0.5, 0.5));
	}

	// On a navmesh the characters are placed on its nearest walkable ground (a placement, not a
	// movement: no position_corrected event).
	const MeshNavigator* nav = sim.navigation();
	if (nav == nullptr)
		throw std::runtime_error("scenario: the duel takes orders, so it needs a navmesh (Simulation::enableNavigation)");
	{
		const kworld::Heightmap terrain = kworld::Heightmap::open(sim.world().dataDir() + "/newland/land/fullmap.tif");
		for (state::Fighter* fighter : {&player, &servant})
		{
			const auto spot = nav->walker().snap({fighter->x, terrain.terrainHeight(fighter->x, fighter->z), fighter->z}, 100.0);
			if (!spot)
				throw std::runtime_error("scenario: the duel's ground is not on the navmesh");
			fighter->x = spot->x;
			fighter->y = spot->y;
			fighter->z = spot->z;
		}
	}

	Duel duel;
	flecs::entity playerEntity = sim.spawnFighter(std::move(player));
	flecs::entity enemyEntity = sim.spawnFighter(std::move(servant));
	duel.player = playerEntity.get<state::Id>()->value;
	duel.enemy = enemyEntity.get<state::Id>()->value;
	// Things to interact with (SIMLINK.md section 13), after the characters so their ids stay: a dropped Katana and some
	// Bread by the player, and the Hub's doors.
	{
		const kworld::Heightmap terrain = kworld::Heightmap::open(sim.world().dataDir() + "/newland/land/fullmap.tif");
		const state::Fighter& placed = *sim.combat()->find(duel.player);
		dropItem(sim, *nav, terrain, byName(gd, "WEAPON", "Katana").stringID, 1, placed.x + 14.0, placed.z + 10.0);
		dropItem(sim, *nav, terrain, byName(gd, "ITEM", "Bread").stringID, 3, placed.x - 12.0, placed.z + 18.0);
	}
	spawnMeshDoors(sim, *nav);
	placeCrates(sim, *nav, gd, *sim.combat()->find(duel.player));
	spawnMeshContainers(sim, *nav, findByName(gd, "VENDOR_LIST", "house contents"));

	// Appearance seeds (the sim carries none yet): factions by record name, males, head 0.
	CharLook playerLook;
	playerLook.factionSid = byName(gd, "FACTION", "Nameless").stringID;
	context.looks[duel.player] = playerLook;
	CharLook servantLook;
	if (const kdata::RefCategory* refs = servantRecord.findRefs("faction"); refs != nullptr && !refs->entries.empty())
		servantLook.factionSid = refs->entries.front().targetID;
	context.looks[duel.enemy] = servantLook;
	// The faction of each character is also its allegiance (door ownership, interact.hpp).
	playerEntity.set<state::Allegiance>(state::Allegiance{playerLook.factionSid});
	enemyEntity.set<state::Allegiance>(state::Allegiance{servantLook.factionSid});
	// The player faction and its one squad (default name Nameless_1: the faction's counter starts at 1, UNSURE).
	if (playerSquads)
	{
		sim.createPlayerFaction(playerLook.factionSid);
		sim.addPlayerMember(sim.addPlayerSquad(), duel.player);
	}
	return duel;
}

Squad spawnSquad(Simulation& sim, const kdata::GameData& gd, const CombatData& data, ServerContext& context, bool playerSquads)
{
	const kdata::Record& body = byName(gd, "CHARACTER", "Escaped Servant");
	const MeshNavigator* nav = sim.navigation();
	if (nav == nullptr)
		throw std::runtime_error("scenario: the squad takes orders, so it needs a navmesh (Simulation::enableNavigation)");
	const kworld::Heightmap terrain = kworld::Heightmap::open(sim.world().dataDir() + "/newland/land/fullmap.tif");
	std::uint64_t rng = 12345;

	// The player squad: Engival, and Izumi from the game data when the load order has her (rebirth.mod does),
	// else a second Engival-like body.
	std::vector<state::Fighter> players;
	players.push_back(makeEngival(gd, data, body, rng));
	if (const kdata::Record* izumi = findByName(gd, "CHARACTER", "Izumi"))
	{
		state::Fighter fighter = data.makeFighter(gd, izumi->stringID, "", rng);
		fighter.playerControlled = true;
		players.push_back(std::move(fighter));
	} else {
		players.push_back(makeEngival(gd, data, body, rng));
	}
	for (std::size_t i = 0; i < players.size(); ++i)
	{
		players[i].team = 0;
		players[i].x = kHubX + 30.0 * static_cast<double>(i);
		players[i].z = kHubZ;
		placeOnMesh(*nav, terrain, players[i], 100.0);
	}

	// The hostile squad: bandits (team 1) 700 units east, standing with their backs to the Hub (facing +x): they notice
	// the squad only when it walks into their hearing (a walker's noise is 375 x 0.5 behind them) or round into their cone
	// (perception.hpp; a bandit facing the Hub would see a squad 700 away on a clear day within a scan and charge at once).
	// Walk towards them to see members join.
	// Real bandits: "Hungry bandit" of the "Starving bandit mob" squad template. The record lists no race; makeFighter
	// takes its faction's (Starving Bandits: none listed, so Greenlander), and rolls the Iron Club / Rag Shirt gear.
	const kdata::Record* banditRecord = &byName(gd, "CHARACTER", "Hungry bandit");
	const std::string banditSquad = byName(gd, "SQUAD_TEMPLATE", "Starving bandit mob").stringID;
	// The Hub navmesh ends short of 700 units east since the 3D snap (fee92f1): take the farthest distance in
	// 700..100 whose ground is on the mesh, so the bandits start as far out as the mesh allows.
	double hostileDistance = 700.0;
	while (hostileDistance > 100.0
		&& !nav->walker().snap({kHubX + hostileDistance, terrain.terrainHeight(kHubX + hostileDistance, kHubZ), kHubZ}, 100.0))
		hostileDistance -= 25.0;
	if (hostileDistance < 600.0)
		std::cerr << "scenario squad: the navmesh ends " << hostileDistance << " units east of the Hub; the bandits start there\n";
	std::vector<state::Fighter> hostiles;
	for (int i = 0; i < 3; ++i)
	{
		state::Fighter fighter = data.makeFighter(gd, banditRecord->stringID, "", rng, banditSquad);
		fighter.team = 1;
		fighter.x = kHubX + hostileDistance;
		fighter.z = kHubZ + 20.0 * static_cast<double>(i);
		fighter.yaw = 3.14159265358979323846 / 2.0;
		placeOnMesh(*nav, terrain, fighter, 100.0);
		hostiles.push_back(std::move(fighter));
	}

	Squad squad;
	CharLook playerLook;
	playerLook.factionSid = byName(gd, "FACTION", "Nameless").stringID;
	for (state::Fighter& fighter : players)
	{
		flecs::entity entity = sim.spawnFighter(std::move(fighter));
		const std::uint64_t id = entity.get<state::Id>()->value;
		squad.players.push_back(id);
		context.looks[id] = playerLook;
		entity.set<state::Allegiance>(state::Allegiance{playerLook.factionSid});
	}
	CharLook hostileLook;
	if (const kdata::RefCategory* refs = byName(gd, "SQUAD_TEMPLATE", "Starving bandit mob").findRefs("faction");
		refs != nullptr && !refs->entries.empty())
		hostileLook.factionSid = refs->entries.front().targetID;
	for (state::Fighter& fighter : hostiles)
	{
		flecs::entity entity = sim.spawnFighter(std::move(fighter));
		const std::uint64_t id = entity.get<state::Id>()->value;
		squad.enemies.push_back(id);
		context.looks[id] = hostileLook;
		entity.set<state::Allegiance>(state::Allegiance{hostileLook.factionSid});
	}
	// The player faction with two squads (Nameless_1, Nameless_2: one character each, so moves between squads can be tried).
	if (playerSquads)
	{
		sim.createPlayerFaction(playerLook.factionSid);
		for (const std::uint64_t id : squad.players)
			sim.addPlayerMember(sim.addPlayerSquad(), id);
	}
	const state::Fighter& first = *sim.combat()->find(squad.players.front());
	dropItem(sim, *nav, terrain, byName(gd, "WEAPON", "Katana").stringID, 1, first.x + 14.0, first.z + 10.0);
	dropItem(sim, *nav, terrain, byName(gd, "ITEM", "Bread").stringID, 3, first.x - 12.0, first.z + 18.0);
	spawnMeshDoors(sim, *nav);
	placeCrates(sim, *nav, gd, first);
	spawnMeshContainers(sim, *nav, findByName(gd, "VENDOR_LIST", "house contents"));
	return squad;
}

} // namespace ksim::simlink
