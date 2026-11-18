#pragma once

// Producers (docs/reference/MINE_SELL_SLICE.md M1-M3; CRAFTING_SLICE 5.1, 7.3, 7.6): a placed BUILDING whose BUILDING_FUNCTIONALITY
// `produces` an item and `consumes` nothing. Wild deposits (Iron Resource, Copper Resource) and built mines (Ore Mine) are the same
// thing; no record, item or resource is named in code. The data says what is made (`produces`), the cap (`produces` v0), how fast
// (`production mult`, `output per day`, the BUILDING's `output rate`), how many may work it (`max operators`) and which stat
// (`stat used`). A producer is a state::Producer + state::Inventory instance, materialised near the player's characters
// (Simulation::stepProducers) and worked by characters holding an InteractTask with action Use.

#include "kdata/gamedata.hpp"
#include "ksim/state.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ksim::producers {

// A producer's Id is derived from its site, so it is the same on every load and after it is dropped and made again: kIdBase + the index
// in World::producerSites() for a zone placement, kIdBase + kFoliageIdBase + zoneIndex * 2^20 + the instance's ordinal among its zone's
// producer instances for a foliage deposit.
inline constexpr std::uint64_t kIdBase = 1ull << 62;
inline constexpr std::uint64_t kFoliageIdBase = 1ull << 40;

// Materialise a site within this many xz units of a player character; drop an unused one beyond kDropRadius. As far as the client's
// interest circle (2000), so a deposit can be hovered and ordered from a distance as in the original (the Hub copper from the Hub's
// front gate, ~1250 u); it was 450 / 600, the session's detail bubble.
inline constexpr double kMaterialiseRadius = 2000.0;
inline constexpr double kDropRadius = 2400.0;
inline constexpr std::uint64_t kIntervalTicks = 20;
// The use point is the walkable point nearest the placement within this (3D).
inline constexpr double kUseSnap = 60.0;

inline constexpr const char* kDefaultClip = "stonemine";

// What the merged data says about one producer BUILDING.
struct Spec
{
	std::string recordSid;
	std::string output; // `produces` target
	std::uint32_t cap = 0; // `produces` v0
	// `max operators`: the BUILDING's value when it is >= 0 (even 0 = fully automatic), else the functionality's (CRAFTING_SLICE 5.1,
	// kinspect b50p Q22). Ore Mine: BUILDING 3 beats the functionality's 6.
	int maxOperators = 1;
	double productionMult = 1.0; // functionality `production mult`
	double outputRate = 1.0; // BUILDING `output rate`
	bool outputPerDay = false; // functionality `output per day`
	std::int32_t statUsed = 0; // functionality `stat used`
	std::int32_t resource = 0; // functionality `world resource mining` (prospect.hpp ids; 0 = not a resource node)
	double useRange = 20.0; // functionality `use range` (default 20)
	bool progressBar = false; // functionality `has progress bar when used` (MQ9: 0 on both mines, so no TASK_PROGRESS for them)
	// The operate clip: the name of the ANIMATION the functionality's `animation` ref points at; no ref = kDefaultClip (the exe's own
	// fallback, kinspect operate_anim.md). CHAR_DYNAMIC `action` while a character operates this producer.
	std::string animation;
};

// The functionality record of a BUILDING when it is a producer (`produces` non-empty, `consumes` empty); null otherwise.
const kdata::Record* producerFunctionality(const kdata::GameData& data, const kdata::Record& building);
// The spec of a producer BUILDING; false for any other record.
bool specOf(const kdata::GameData& data, const kdata::Record& building, Spec& out);
// The functionality record of a BUILDING that `produces` anything, with or without `consumes` (a camp: a farm, a mine, a converter;
// economy.hpp). producerFunctionality() is this plus the no-consumes test; a converter's machine is the economy's (CRAFTING_SLICE 7).
const kdata::Record* productionFunctionality(const kdata::GameData& data, const kdata::Record& building);
// The same spec for a production BUILDING of either class; false when it produces nothing.
bool productionSpecOf(const kdata::GameData& data, const kdata::Record& building, Spec& out);

// Every producer BUILDING of the merged data, by record stringID. Built once.
class Book
{
public:
	explicit Book(const kdata::GameData& data);
	const Spec* find(const std::string& recordSid) const;

private:
	std::unordered_map<std::string, Spec> specs_;
};

// ---- THE RATE AND SKILL, in one place (kinspect item 59, findings/item59_mine_sell.md MQ1 / MQ2, answered) -----------------------
// MQ1: progress units (one unit = one output item) one operator adds per tick, at skill multiplier 1, richness 1, full power.
//   per day:  productionMult * outputRate / maxOperators items per game day (live: copper, L3, richness 0.8 -> 15.2 a day)
//   otherwise (no `output per day`; no producer in the data): the same figure x K per sim-second (K = `production speed`, 2.0; UNSURE)
inline constexpr double kProductionSpeedK = 2.0;
// The Production speed advanced option (0.5-2, default 1).
inline constexpr double kProductionSpeedOption = 1.0;
double baseProgressPerTick(const Spec& spec) noexcept;
// skillMult = lerp(0.5, 1.4, stat / 100) (CRAFTING_SLICE 5.1)
double skillMult(double stat) noexcept;
// MQ2: the governing skill is the functionality's `stat used` (3 = Labouring on every mine). The sim has Labouring and Science; any other
// value falls back to Labouring. It trains every tick the order runs, produced item or not.
double statOf(const state::FighterSkills& skills, std::int32_t statUsed) noexcept;
double& statRef(state::FighterSkills& skills, std::int32_t statUsed) noexcept;
// XP per sim-second per operator: 0.02 * 0.75 * 0.5 * raceMult (raceMult 1.2 / 0.8 unread: 1); Rules::addXp applies ((101 - L) / 101)^2.
inline constexpr double kXpPerSecond = 0.02 * 0.75 * 0.5;
// The power term. HOOK: no power model exists. An operated producer (maxOperators > 0) runs at 1.0; a fully automatic one
// (maxOperators 0, a drill) makes no progress until a power model feeds this (powerHp = hpFrac * received / demand).
double powerFraction(const Spec& spec) noexcept;

// Progress per tick one operator of this skill adds.
double operatorProgress(const Spec& spec, double richness, const state::FighterSkills& skills) noexcept;

} // namespace ksim::producers

namespace ksim {

// Whether a producer entity goes into a save: only while it holds stock, progress or an operator (every other one is made again from the
// data, with the same Id, when a player comes near). True for any entity that is not a producer.
bool producerSaved(const flecs::entity& entity);

} // namespace ksim
