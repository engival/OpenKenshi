#pragma once

// Machine records (kenshi-k16.5.22, kenshi-gzto): every placed BUILDING with a functionality that is a machine, not a container, gets one
// record carrying what the original's bottom-left inspect panel shows (kinspect s8l2, South Stone Camp). A producer, a converter, a
// power building (function 5), a turret (12) and a furnace (14) alike. The record is derived from the game data and the sim's state; it
// is not saved. The wire carries it as INTERACTABLE kind 12 and the BUILDING rows (SIMLINK.md, kenshi-k16.5.26).

#include "kdata/gamedata.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace ksim::machines {

enum class Kind : std::uint8_t
{
	Producer, // `produces`, no `consumes`: a mine, a drill, a well
	Converter, // `produces` and `consumes`: a processor, a refinery, a campfire
	Power, // function 5
	Turret, // function 12
	Furnace, // function 14: a container that melts what is put in it (the original: FurnaceBuilding, Workers "-", Working)
};

enum class Status : std::uint8_t
{
	Normal, // nothing to operate (a turret nobody mans, a container)
	Working,
	NoWorkers,
};

// The panel's stat line: what a producer's `world resource mining` says it digs (prospect ids), or none.
enum class StatLine : std::uint8_t { None, StoneRichness, IronQuality, CopperQuality, WaterQuality };

// The machine kind of a BUILDING from its first live functionality; nullopt for anything that is not a machine.
std::optional<Kind> kindOf(const kdata::GameData& data, const kdata::Record& building);

// What the data says about a machine BUILDING, independent of the placement (World::machineInfo; the wire's kind 12 row reads it).
struct Static
{
	Kind kind = Kind::Producer;
	int maxWorkers = 0; // 0 = automatic (a furnace too); a turret's is its own slot count
	double useRange = 20.0; // the functionality's `use range` (20 when the record has none, as functionalityReach); an order stops within it, x2 once stopped
	std::int32_t powerOutput = 0;
	std::string input; // a converter's first `consumes` item record, "" = none
	std::string output; // the first `produces` item record, "" = none
};
std::optional<Static> staticOf(const kdata::GameData& data, const kdata::Record& building);

// The wire id of a machine with no entity of its own (kind 12; SIMLINK.md): kIdBase + this + the index in World::machineSites(). A
// furnace's entity (a Container, its contents) carries the same id.
inline constexpr std::uint64_t kMachineIdBase = (1ull << 62) + (1ull << 50);

struct Record
{
	std::string recordSid;
	Kind kind = Kind::Producer;
	std::string ownerFaction; // the placement's owner, "" = no label (the original prints it only once the town is visited, kenshi-rwh.318)
	std::string town; // the TOWN record's name of the circle holding the placement, "" = none
	double x = 0.0, y = 0.0, z = 0.0;
	Status status = Status::Normal;
	double hp = 0.0, maxHp = 0.0; // Condition: the placement is whole (no damage model for a zone placement yet)
	// Power: the BUILDING's `power output` is negative for a consumer (needs power), positive for a source. active = a supplied grid;
	// no grid model yet, so a consumer is always inactive with value 0 (the original's "Power Inactive 0").
	bool powerNeeded = false;
	bool powerActive = false;
	double powerValue = 0.0;
	std::int32_t powerOutput = 0;
	StatLine stat = StatLine::None;
	double statValue = 0.0; // richness / quality, 1.0 unless the zone says otherwise
	int workers = 0;
	int maxWorkers = 0; // 0 = automatic: the panel shows "-"
	double efficiency = 0.0; // 0..: workers / max workers x max efficiency (a power term waits for the grid)
	double maxEfficiency = 0.0; // richness x the BUILDING's `output rate`
	std::string input; // a converter's first `consumes` item record, "" = none
	std::string output; // the first `produces` item record, "" = none
	std::uint64_t producerId = 0; // the state::Producer entity's Id when this machine is also a producer, else 0
};

} // namespace ksim::machines
