#pragma once

// Seeded random numbers, one stream per subsystem (ARCHITECTURE.md
// "Simulation"). Every stream is derived from the one world seed by its
// stream number, so adding a stream never shifts another.

#include <cstddef>
#include <cstdint>

namespace ksim {

// Stream numbers. 0..2 are Simulation's detail-level systems (Subsystem).
inline constexpr std::size_t kSpawnStream = 3;
// Region engine: one sub-stream per entity, streamSeed(streamSeed(seed,
// kRegionEntityStream), state::Id), and per stand-in group, by group index.
// Each advances only when its own entity or group steps, so thread
// scheduling cannot consume randomness (region.hpp).
inline constexpr std::size_t kRegionEntityStream = 4;
inline constexpr std::size_t kRegionGroupStream = 5;
// Wilderness placement (placement.hpp).
inline constexpr std::size_t kPlacementStream = 6;
// Wilderness spawner (wilderness.hpp), its state is saved.
inline constexpr std::size_t kWildernessStream = 7;
// Combat arena (combat.hpp): one sub-stream per fighter, streamSeed(streamSeed(seed,
// kCombatStream), dice key), advanced only when that fighter draws. Saved in Fighter::rng. The dice key is the
// fighter's stable identity, Fighter::diceSeed (task CC: the squad's spawn key + member slot, or the scenario file's
// per-character spawn value), else its state::Id (a bare arena, an older save), so an unrelated spawn does not re-roll it.
inline constexpr std::size_t kCombatStream = 8;
// Follow formation scatter (combat.cpp): streamSeed(streamSeed(seed, kFormationStream), leader dice key). A pure
// function of the squad, never advanced, so nothing is saved.
inline constexpr std::size_t kFormationStream = 9;
// The scatter of a group move order (simlink.cpp): streamSeed(kClickStream, the member count), the
// original's srand(count): deterministic per count and selection order, identical on every click.
inline constexpr std::size_t kClickStream = 10;
// The portrait serial of a new character (simulation.cpp spawnFighter): streamSeed(streamSeed(seed, kPortraitStream), state::Id), one
// draw. Saved in Fighter::portraitSerial.
inline constexpr std::size_t kPortraitStream = 11;

// A new game's squad (newgame.cpp): streamSeed(streamSeed(seed, kPlayerStartStream), the command's tick), advanced inside the one command.
inline constexpr std::size_t kPlayerStartStream = 13;

// A start-off's squad as STARTS and NEW_GAME both build it (newgame.cpp Simulation::startSquad): streamSeed(streamSeed(seed, kStartSquadStream), FNV-1a of
// the start-off sid). A pure function of the server seed and the sid.
inline constexpr std::size_t kStartSquadStream = 14;

// A merchant's refresh (trader.hpp): the dice of one refresh are RngStream(trader::refreshSeed(seed, squad Id, refreshes)), a pure
// function of the three, so nothing is saved but the count.
inline constexpr std::size_t kTraderStream = 15;

// A promoted resident (residents.cpp): its Fighter's build dice and its initial Routine::lastSleep are pure functions,
// streamSeed(streamSeed(seed, kRoutineStream), dice key), the dice key being the member's stable identity (task CC: its
// squad's SquadInstance content + its slot, never its Id), so a demote and a promote rebuild the same person and an
// unrelated spawn does not change it. Nothing is saved but the results.
inline constexpr std::size_t kRoutineStream = 16;

// A town's price factor for an item (trade.hpp): RngStream(splitMix64(streamSeed(seed, kPriceStream) ^ town Id x K ^ FNV-1a of the item sid)),
// one draw. A pure function, so nothing is saved and a load shows the same prices.
inline constexpr std::size_t kPriceStream = 17;

// A fighter's melee avoidance radii and dead zones (combat.cpp CombatArena::comfort, kinspect s74_static.md 1.2 / 1.6): a pure function,
// streamSeed(streamSeed(streamSeed(seed, kAvoidStream), dice key), Fighter::comfortRolls), four draws; the count (melee task starts, the exe rolls
// at each) is the only saved part.
inline constexpr std::size_t kAvoidStream = 18;

// Dialogue rolls (dialogue_events.hpp): the chance and pick rolls are hashes of (streamSeed(seed, kDialogueStream), speaker, tick, record),
// never a stream, so nothing is saved and the order of evaluation does not matter.
inline constexpr std::size_t kDialogueStream = 19;

// The individual goals (individual_goals.cpp): the flee distance is streamSeed(streamSeed(seed, kGoalStream), character Id) rolled at the tick
// the goal is given, a pure function of (seed, id, tick); nothing is saved but the result. A run-away re-roll is the same with a 1 level
// between the stream and the id.
inline constexpr std::size_t kGoalStream = 20;

// A ground item's rest yaw on the wire (simlink.cpp INTERACTABLE tail): streamSeed(item Id, kRestYawStream), presentation only (not
// sim state, so no world seed); fixed for the item's life.
inline constexpr std::size_t kRestYawStream = 21;

// The weather regions (weather.hpp): streamSeed(streamSeed(seed, kWeatherStream), BIOME_GROUP `index` colour), one per group,
// saved in state::WeatherRegion::rng.
inline constexpr std::size_t kWeatherStream = 12;

// A weather region's wind direction (weather.hpp windYaw): a pure function of the region's saved instance (group sid, weather sid,
// start minute), so a load gives the same direction; nothing is saved. OURS (the exe's pick is not traced).
inline constexpr std::size_t kWeatherWindStream = 22;

// Venge's sky beams (beams.hpp): a pure function of (world seed, group, weather, start minute, slot, k), streamSeed(key, kBeamStream)
// mixed with the slot and k; nothing is saved, so a load re-derives the same beams. OURS (the exe's draws are not traced).
inline constexpr std::size_t kBeamStream = 30;

// A ragdoll start (combat.cpp CombatArena::rollRagdoll, task BG): streamSeed(streamSeed(streamSeed(seed, kRagdollStream), dice key),
// Fighter::restRolls), two draws for the EVENT 48 seed and the body's lying yaw. The count is saved, so a replay and a
// save/load continue the same sequence, and the combat dice (kCombatStream) are untouched.
inline constexpr std::size_t kRagdollStream = 23;

// The town search (RUN_AROUND_TOWN_LOOKING_FOR_PEOPLE 216, task patrol; world_resolvers.hpp runAroundTown216): the building pick
// and the search points are streamSeed(streamSeed(streamSeed(seed, kSearchStream), the member's dice key), the tick) - pure
// functions of the saved state, so nothing is saved but the picks' results.
inline constexpr std::size_t kSearchStream = 24;

// The shopping list (task CH group 3, shopping.hpp buildList): each `shopping` ref's day's chance is streamSeed(streamSeed(streamSeed(seed,
// kShoppingStream), the member's dice key), the game day) - a pure function of the saved state, so nothing is saved but the built list.
inline constexpr std::size_t kShoppingStream = 25;
// A shop transaction (shopping.cpp stepShopping): the resolver's shop pick is keyed by (kShoppingStream, the member's dice key, the day
// with the pick constant mixed in) and the transaction's item pick by (kShoppingPickStream, the member's dice key, the tick) - a pure
// function of the saved state; the tick only moves when the pulse fires, and the pulse timer is saved.
inline constexpr std::size_t kShoppingPickStream = 26;
// The fight-goal re-selection phase (task fight-goals, world_tasks.cpp stepWorldTasks; kinspect patrol_aggro_town.md a8a41b7 section 3):
// streamSeed(streamSeed(streamSeed(seed, kFightGoalStream), the member's dice key), the tick), one draw U(1,40)/60 s - a pure function
// of the state, so a replay is identical and nothing is saved.
inline constexpr std::size_t kFightGoalStream = 27;
// An off-screen token fight (token_fight.hpp, k16.6.46): streamSeed(... (seed, kTokenFightStream), the lower squad Id), the higher), the tick), two draws:
// the winner, and whether the winner's weakest falls. A pure function of the state; nothing is saved.
inline constexpr std::size_t kTokenFightStream = 28;

// A released carried body's landing distance (carry.cpp dropCarried, task body-rest, PHYSICS.md 3.2): one draw from
// streamSeed(streamSeed(streamSeed(seed, kDropStream), dice key), Fighter::restRolls). Nothing is saved but the count the ragdoll
// start already saves.
inline constexpr std::size_t kDropStream = 29;

// xorshift64*; deterministic across platforms.
class RngStream
{
public:
	explicit RngStream(std::uint64_t seed);

	std::uint64_t next();
	double uniform01();
	// Uniform in [low, high]; low when high < low.
	std::int64_t between(std::int64_t low, std::int64_t high);
	std::uint64_t state() const noexcept { return state_; }

private:
	std::uint64_t state_;
};

std::uint64_t splitMix64(std::uint64_t value) noexcept;

// The seed of stream `stream` for world seed `seed`.
std::uint64_t streamSeed(std::uint64_t seed, std::size_t stream) noexcept;

} // namespace ksim
