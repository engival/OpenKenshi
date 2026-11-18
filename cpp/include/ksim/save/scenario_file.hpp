#pragma once

// Scenario files (docs/reference/SCENARIO_IMPORT.md): a TEST FIXTURE that places characters, a clock and a weather in the default
// world, written by tools/ksave_scenario.py from an original save (or by hand). Not save compatibility (ARCHITECTURE "Non-goals").
// Loaded by `ksim_server --scenario-file` and by `ksim_fight`.
//
// The text format "KSCEN 2" (1 is read too; it lacks `order`): one record per line, `TAG key=value key=value ...`, values percent-encoded (space, '=', ',', ':' and
// every non-ASCII byte as %XX); a list value joins already-encoded items with ',' and an item's name and value with ':'.
// Lines starting with '#' and blank lines are ignored.

#include "ksim/combat/combat_data.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/core/simulation.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ksim::scenario {

struct Item
{
	std::string sid; // ITEM / WEAPON / ARMOUR ... record
	std::string section; // the original's slot: back / hip / armour / legs / boots / main / backpack_content ...
	std::uint32_t count = 1;
	int level = 0; // the original's quality tier (item level x 100)
	double quality = 100.0;
	std::string material; // MATERIAL_SPECS record (a weapon's model)
	std::string company; // WEAPON_MANUFACTURER record
	std::string colour;
};

struct Limb
{
	std::string sid; // LOCATIONAL_DAMAGE record
	double flesh = 100.0; // part health
	double hit = 0.0; // UNSURE: the part's hit weight (race anatomy v0); read, not applied
	double hitMult = 1.0;
	double wear = 0.0;
	double bandage = 0.0;
	double stun = 0.0;
	double rig = 0.0; // UNSURE; read, not applied
};

struct Char
{
	int id = 0;
	int squad = 0;
	std::string name;
	std::string templateSid; // CHARACTER record
	std::string race; // RACE record, "" = the template's
	std::string faction; // FACTION record
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
	double yaw = 0.0; // radians, rotation about +y as the original stores it
	bool leader = false;
	bool player = false;
	bool passive = false;
	bool hold = false; // HOLD POSITION
	bool defensive = false;
	bool taunt = false;
	bool chase = false;
	bool ranged = false;
	bool stealth = false;
	bool noncombat = false;
	int speed = 2;
	double age = 1.0;
	double blood = -1.0; // absent: the race's maximum for its strength (scenario_file.cpp apply)
	double hunger = 3.0; // 3.0 full (UI x 100)
	double fed = 0.0;
	double bleeding = 0.0;
	double ko = 0.0;
	bool unconscious = false;
	bool coma = false;
	bool dead = false;
	bool incapacitated = false;
	std::map<std::string, double> stats; // STATS record names
	std::vector<Limb> limbs;
	std::vector<Item> items;
	// `cage=near` (the nearest free cage within Simulation::kScenarioCageReach of x, z) or `cage=<building key>` (a free cage of that building, its
	// placement key as SIMLINK's building ids): the character is put in it at load through Simulation::imprison (kenshi-k16.6.36). "" = not caged.
	std::string cage;
	// `peeler=1` with `cage=`: the cage's building is marked a skin peeler (Simulation::setPeeler), a STAND-IN for the original's Peeler Machine, which no
	// loaded zone places (kenshi-h2v.13.3): the occupant is then torn limb by limb like a real peeler's.
	bool peeler = false;
	int bounty = 0; // `bounty=<n>` with `cage=`: the cage owner's legal faction holds it, so the put-in starts a sentence (int(n x 0.02 x prison time) hours)
	// `shackled=1`: the char wears locked prisoner shackles. `task=` starts a player task at load (Simulation::scenarioTask): pick_cage / cut_cage
	// (with `cage=`), pick_shackles / cut_shackles (`target=` a char id), operate / build (`target=` a `building` id). Player chars only.
	bool shackled = false;
	std::string task;
	int target = 0;
	bool female = false;
	std::map<std::string, std::string> look; // appearance fields, read and kept; only `female` is applied
};

struct Squad
{
	int id = 0;
	std::string name;
	std::string faction;
	std::string templateSid; // SQUAD_TEMPLATE
	std::string package; // AI_PACKAGE (recorded; the squad runs the world selector with the template's packages: Simulation::scenarioMember, residents.cpp fileSquads)
	bool player = false;
	int money = 0;
	// Hostile to the player's characters (the converter: the faction's relation toward the player's faction is negative; UNSURE rule, a Starving Bandit is
	// -10, above the sim's -30 hostile line). A hostile squad's characters fight the player's even where Simulation::teamOfFaction says friendly.
	bool hostile = false;
	// `war=1` (KSCEN 2): the squad's faction and the player's faction are at war, relation -100 both ways at load (Command::setFactionRelation), so the
	// squad's people pick the player's as targets by the normal perception rule and the faction's campaigns see an enemy. `hostile` alone sets only the team.
	bool war = false;
	// `sheathed=1` (kenshi-k16.6.124): the squad's bodies spawn with the weapon on the back (carried, weapon_type 5 until the draw), as the original's do
	// (rwh.91 section 2, rwh.102 section 4): the draw is plan task 6 (stepRearm), taken when a fight goal commits. Not for a player's character.
	bool sheathed = false;
};

// An initial attack order (`order char=<id> attack=<id>`): at the first tick the character gets the attack order on the other (Command::fighterAttack,
// the original's goal 5 FOCUSED_MELEE_ATTACK). KSCEN 2.
struct Order
{
	int who = 0;
	int attack = 0;
};

// `building` (KSCEN 2): a BUILDING placed on the ground at load (Simulation::placeScenarioBuilding). `id` names it for a char's `target=`.
struct Building
{
	int id = 0;
	std::string sid;
	double x = 0.0;
	double z = 0.0;
	double yaw = 0.0;
	bool built = true;
	std::string faction; // a producer's owner faction sid, "" = wild
};

// `town` (KSCEN 2): the world's TOWN of that record name (the one nearest the file's centre) changes hands to the player faction (`owner=player`), so a faction's campaign can pick it.
struct Town
{
	std::string name; // the TOWN record's name
	bool player = true;
};

// `container` (KSCEN 2): a storage box on the ground at load (Simulation::spawnContainer), unlocked, holding `stock` (item sid, count). `owner` is a
// FACTION sid, "player" for the file's player faction, "" = nobody's.
struct Container
{
	std::string sid; // BUILDING record
	double x = 0.0;
	double z = 0.0;
	std::string owner;
	std::vector<std::pair<std::string, std::uint32_t>> stock;
};

// `raid` (KSCEN 2): the file's non-player squads `squads` are a faction's campaign force already under way against the town `town`: each gets the
// raid order the faction layer writes at a commission (Issued, the squad's own spot as the origin) and the faction records the raid, so the group
// layer marches them and the faction layer follows them as it does a rolled one. `stay` / `retreat` are game minutes.
struct Raid
{
	std::string faction; // FACTION sid of the raiders
	std::string campaign; // FACTION_CAMPAIGN record, "" = none (the squads run their template's packages)
	std::string town; // TOWN record name of the target
	std::vector<int> squads; // file squad ids
	double stay = 10.0;
	double retreat = 5.0;
};

struct File
{
	std::string path;
	int version = 1; // KSCEN 1 or 2
	std::string save;
	std::string gameVersion;
	double radius = 0.0;
	double centreX = 0.0;
	double centreZ = 0.0;
	std::string leader;
	std::uint64_t day = 1;
	int hour = 10;
	int minute = 0;
	std::string weatherGroup; // BIOME_GROUP, "" = no weather in the file
	std::string weather; // WEATHER
	std::optional<std::string> cameraLine; // the camera record, kept verbatim (the kclient args sidecar carries it to the client)
	std::vector<Squad> squads;
	std::vector<Char> chars;
	std::vector<Order> orders;
	std::vector<Building> buildings;
	std::vector<Town> towns;
	std::vector<Container> containers;
	std::vector<Raid> raids;
	// The playtest header (record `scenario`, task CE, docs/reference/PLAYTEST.md 1): the line the client's Scenarios window shows and the
	// metrics the run reports. Absent in a fixture file. A playtest scenario is `scenarios/<name>.kscen`; the name is the file's stem.
	std::string title;
	std::string setup;
	std::string lookFor;
	std::string bead; // header field bead=<feature id>: the beads feature item this playtest proves ("" = none)
	std::vector<std::string> metrics;
	std::optional<double> retargetHold; // header field retarget_hold=S: combat.retarget_hold_s pinned for this scenario (COMBAT_OPTIONS.md 6)
	std::optional<bool> turretManning; // header field turret_manning=0|1: Simulation::setTurretManning for this scenario (RANGED.md)
	bool hasHeader = false;
};

// Applies one saved character onto `f`, a Fighter built from its template: name, sex, stats, the equipment on the body, limbs, blood and modes (the
// mapping of SCENARIO_IMPORT.md; the save importer uses it for every member). What the character carries in its pack goes to `carried`; modes the sim
// has no counterpart for are noted in `unmapped`. Position, team and squad are the caller's. Throws on a limb the race does not have.
void applyChar(const kdata::GameData& gd, const CombatData& data, const Char& c, state::Fighter& f, std::vector<Item>& carried, std::set<std::string>& unmapped);
// Adds the carried items (applyChar's `carried`) to `pack` as unplaced entries; the caller settles the grid.
void addCarried(state::Inventory& pack, const kdata::GameData& gd, const std::vector<Item>& carried);

// The STATS names applyChar does not map to a FighterSkills field (the import reports them as a gap).
std::vector<std::string> unmappedStats(const std::map<std::string, double>& stats);

// Parses a KSCEN file. Throws std::runtime_error with `path:LINE: reason`.
File parseFile(const std::string& path);

// FNV-1a 64 of a file's bytes (the SIMLINK scenario wire's fileHash; the hello dataHash constants).
std::uint64_t fileHash(const std::string& path);

struct ScenarioInfo
{
	std::string name; // the file's stem, `scenarios/<name>.kscen`
	std::string path;
	std::string title;
	std::string setup;
	std::string lookFor;
	std::string bead;
	std::vector<std::string> metrics;
	std::uint64_t fileHash = 0;
};

// Every `*.kscen` in `dir` that carries the header record, sorted by name (SIMLINK SCENARIO_LIST). A file that does not parse is an
// error (std::runtime_error with the parser's message): a scenario the client lists must load.
std::vector<ScenarioInfo> listScenarios(const std::string& dir);

struct Placed
{
	std::vector<std::uint64_t> players; // state::Id, leader first
	std::vector<std::uint64_t> others;
	std::map<int, std::uint64_t> idOf; // file char id -> state::Id
	std::map<int, std::uint64_t> buildingOf; // file building id -> the building's or producer's state::Id
	std::size_t removedSquads = 0;
	std::size_t claimedSquads = 0; // file squads that took over a default-world squad of the same template (the rest are new squads)
	std::size_t orders = 0; // attack orders queued for the first tick
	std::vector<std::uint64_t> attackers; // state::Id of the ordered characters
	std::vector<std::string> notes; // what was read but not applied (modes without a sim counterpart, appearance, ...)
};

// Fails (std::runtime_error naming every record) when a sid the file mentions is not in `gameData`. Never substitutes.
void check(const kdata::GameData& gameData, const File& file);

// Builds the scenario in a default world: the clock and weather, the player faction with the file's player squads, every non-player squad as a
// world squad (Simulation::scenarioSquad: a default-world squad of the same template inside the radius is claimed, else a new one; a faction the
// file names but the world has no instance of gets one; the other default-world squads inside the radius are removed) so its characters run the normal world AI once the AI is enabled, every character as a
// Fighter on the navmesh at its saved spot (the sim needs combat and navigation enabled), looks for the client, the file's attack orders queued
// for the first tick. Between ticks. `seed` draws the fighters' own streams.
Placed apply(Simulation& sim, const kdata::GameData& gameData, const CombatData& data, simlink::ServerContext& context,
	const File& file, std::uint64_t seed);

} // namespace ksim::scenario
