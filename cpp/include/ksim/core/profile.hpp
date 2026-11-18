#pragma once

// Per-system tick profiling (KSIM_PROFILE, docs/PROFILING.md). Off by default: with the CMake option OFF this header defines only
// the macros, which expand to nothing, so the tick holds no profile code. With it ON a Profiler (one per Simulation, used on the sim's
// thread only) times each stage of Simulation::tick, plus the server's frame encode and a save, and keeps the last kWindow ticks in a
// ring that never grows. The sim never shows or writes the numbers on its own: it hands them out through summary() / totals() (the
// soak report), json() (one line on stderr every N seconds when KSIM_PROFILE_JSON or setJsonSeconds asks) and, one day, SIMLINK.

#ifndef KSIM_PROFILE
#define KSIM_PROFILE 0
#endif

#if KSIM_PROFILE

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace ksim::profile {

// The tick's stages in the order they run (Simlink and Save are outside the tick: their time lands in the next tick's sample).
enum class Section : std::uint8_t
{
	Setup, // commands, unlocks, discovery, door paint
	Systems, // the scheduled systems (movement, needs, the interval-driven ones)
	Weather,
	Wild, // the wilderness spawner
	AiStrategic, // faction strategy
	AiSquad, // squad goals and the world squads' steps (AI / goal runner)
	Tokens, // off-view stragglers, token fights, stored medical, corpses
	Combat, // the arena step (its path queries are counted apart)
	Interact, // player squads, buildings, doors, self aid, interactions
	Producers, // producers, item nodes, carrying
	Tasks, // intruders, crime goals, slave trade, world tasks, torture, gate crews
	Residents, // player areas, residents, routines, rearm, shopping
	Crime, // crime and dialogue
	Goals, // individual goals, prospecting, order queues, jobs
	Economy, // traders, economy, construction, meals
	Nav, // door paint, region tick, line of sight
	Tail, // scenario hold, scheduler mirror
	Simlink, // frame encode (Session::sendTick), outside the tick
	Save, // Simulation::save / saveGame, outside the tick
	Count
};

inline constexpr std::size_t kSections = static_cast<std::size_t>(Section::Count);

inline constexpr std::array<const char*, kSections> kSectionNames = {"setup", "systems", "weather", "wild", "ai_strategic", "ai_squad", "tokens", "combat",
	"interact", "producers", "tasks", "residents", "crime", "goals", "economy", "nav", "tail", "simlink", "save"};

// Dev probe (docs/PROFILING.md "Heft under live load"): finer stages inside the sections, written only to the per-tick CSV that
// KSIM_PROFILE_TICKS=<path> asks for (nothing is timed without it). Each charges the time since the previous sub mark.
inline constexpr std::array<const char*, 37> kSubNames = {"carry_prep", "bar_depots", "squad_step", "delivery_log", "world_squads", "stragglers",
	"token_fights", "stored_medical", "corpses", "drop_gone", "intruders", "crime_goals", "slave_trade", "slave_state", "corpse_pot", "world_tasks",
	"torture", "gate_crews", "player_areas", "residents", "routines", "rearm", "shopping", "stealth_ko", "wt_prelude", "wt_orders", "wt_in_hand", "wt_fight_goals", "combat_pre", "combat_step", "combat_mirror", "cs_index", "cs_sense", "cs_env", "cs_separate", "cs_update", "cs_tail"};
enum class Sub : std::size_t
{
	CarryPrep, BarDepots, SquadStep, DeliveryLog, WorldSquads, Stragglers, TokenFights, StoredMedical, Corpses, DropGone, Intruders, CrimeGoals, SlaveTrade, SlaveState, CorpsePot, WorldTasks, Torture, GateCrews, PlayerAreas, Residents, Routines, Rearm, Shopping, StealthKo, WtPrelude, WtOrders, WtInHand, WtFightGoals, CombatPre, CombatStep, CombatMirror, CsIndex, CsSense, CsEnv, CsSeparate, CsUpdate, CsTail
};

inline constexpr std::size_t kSubs = kSubNames.size();

struct TickSample
{
	std::uint64_t tick = 0;
	std::uint64_t tickNs = 0; // begin() to end(), the tick's own time (the sections of the tick sum to it)
	std::array<std::uint64_t, kSections> ns{};
	std::uint32_t characters = 0; // Fighter entities
	std::uint32_t squads = 0;
	std::uint32_t pathQueries = 0; // coarse + local + cross-border + snap calls of the navigator
	std::uint64_t pathNs = 0; // their wall time (inside Combat / the AI sections, not a section of its own)
};

// Since the Profiler was made.
struct Totals
{
	std::uint64_t ticks = 0;
	std::uint64_t tickNs = 0;
	std::array<std::uint64_t, kSections> ns{};
	std::uint64_t pathQueries = 0;
	std::uint64_t pathNs = 0;
};

// Over the ring (the last min(ticks, kWindow) ticks).
struct Summary
{
	std::size_t ticks = 0;
	std::array<double, kSections> meanUs{};
	std::array<double, kSections> maxUs{};
	double tickMeanUs = 0.0;
	double tickP50Us = 0.0;
	double tickP99Us = 0.0;
	double tickMaxUs = 0.0;
	double characters = 0.0; // mean per tick
	double squads = 0.0;
	double pathQueries = 0.0;
	double pathUs = 0.0;
};

class Profiler
{
public:
	using Clock = std::chrono::steady_clock;
	static constexpr std::size_t kWindow = 4096;

	Profiler(); // KSIM_PROFILE_JSON=<seconds> turns the JSON line on

	void begin() noexcept;
	// The time since the last mark (or begin) goes to the section.
	void mark(Section section) noexcept;
	// Time measured elsewhere (a Scope), outside the tick: it joins the next tick's sample.
	void add(Section section, std::uint64_t ns) noexcept { pending_[static_cast<std::size_t>(section)] += ns; }
	// Closes the tick: stores the sample. navQueries / navNs are the navigator's running totals (this takes the difference).
	// A finer stage (index into kSubNames); a no-op unless KSIM_PROFILE_TICKS is set.
	void sub(Sub which) noexcept
	{
		if (!ticksFile_)
			return;
		const Clock::time_point now = Clock::now();
		subPending_[static_cast<std::size_t>(which)] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - subLast_).count());
		subLast_ = now;
	}
	void end(std::uint64_t tick, std::uint32_t characters, std::uint32_t squads, std::uint64_t navQueries, std::uint64_t navNs);

	const Totals& totals() const noexcept { return totals_; }
	Summary summary() const;
	std::string json() const;
	// 0 = off. The line goes to stderr from end(), at most once per interval.
	void setJsonSeconds(double seconds) noexcept { jsonSeconds_ = seconds; }

private:
	std::vector<TickSample> ring_; // kWindow entries, written round robin
	std::size_t next_ = 0;
	std::size_t filled_ = 0;
	std::array<std::uint64_t, kSections> pending_{};
	Totals totals_;
	Clock::time_point tickStart_;
	Clock::time_point last_;
	std::uint64_t navQueries_ = 0;
	std::uint64_t navNs_ = 0;
	double jsonSeconds_ = 0.0;
	Clock::time_point jsonLast_;
	std::shared_ptr<std::FILE> ticksFile_; // KSIM_PROFILE_TICKS: one CSV row per tick (shared: a moved Simulation keeps it)
	std::array<std::uint64_t, kSubs> subPending_{};
	Clock::time_point subLast_;
};

// Times a stretch outside the tick into a section.
class Scope
{
public:
	Scope(Profiler& profiler, Section section) noexcept : profiler_(profiler), section_(section), start_(Profiler::Clock::now()) {}
	~Scope() { profiler_.add(section_, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Profiler::Clock::now() - start_).count())); }
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Profiler& profiler_;
	Section section_;
	Profiler::Clock::time_point start_;
};

} // namespace ksim::profile

// In a Simulation member: profiler_ is the member.
#define KSIM_PROFILE_BEGIN() profiler_.begin()
#define KSIM_PROFILE_MARK(section) profiler_.mark(::ksim::profile::Section::section)
#define KSIM_PROFILE_SUB(which) profiler_.sub(::ksim::profile::Sub::which)
#define KSIM_PROFILE_END() profileTickEnd()
// Anywhere with a Profiler& (a Session reaches it through its Simulation).
#define KSIM_PROFILE_SCOPE(profiler, section) const ::ksim::profile::Scope kpScope_(profiler, ::ksim::profile::Section::section)

#else

#define KSIM_PROFILE_BEGIN()
#define KSIM_PROFILE_MARK(section)
#define KSIM_PROFILE_SUB(index)
#define KSIM_PROFILE_END()
#define KSIM_PROFILE_SCOPE(profiler, section)

#endif
