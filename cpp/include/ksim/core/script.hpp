#pragma once

// The sandboxed Lua host (ARCHITECTURE.md "Behaviour layer: Lua"; the API scripts
// see: docs/reference/LUA_API.md). Lua 5.5.0 is vendored (third_party/lua-5.5.0).
//
//  - One ScriptHost = one lua_State. A host is NOT thread safe and shares nothing
//    with any other host: one per region/worker thread, none shared.
//  - States are made with lua_newstate(alloc, ud, kScriptSeed): our allocator
//    enforces the memory budget, the seed is fixed so `pairs` over string keys is
//    reproducible. Table and userdata keys hash by address, so the API never hands
//    scripts object-keyed maps: only arrays and string-keyed maps (ScriptValue
//    keeps those sorted).
//  - Every call runs under an instruction budget (a count hook that raises).
//  - Scripts see only: base functions (no load, loadfile, dofile, require, getmetatable,
//    print; collectgarbage("count") only), string (no dump), table, math (no random,
//    randomseed), utf8, and the `ksim` table (random, random_int, log). No io, os,
//    debug, package or coroutine: those libraries are not even compiled in.
//  - Scripts load in text mode only, each with its own _ENV.
//  - Data crosses as ScriptValue trees (plain C++), never as Lua references, so a Lua
//    error can only unwind Lua frames: every raise-capable stretch of Lua API is in a
//    lua_pcall'd C function that owns no C++ objects.
//  - Errors are caught and returned as a CallResult; the caller logs and falls back.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ksim {

// The fixed string-hash seed of every script state.
inline constexpr unsigned kScriptSeed = 0x4B534D31u; // "KSM1"

struct ScriptLimits
{
	// Total bytes the state may hold (libraries, scripts and their garbage).
	std::size_t memoryBytes = std::size_t{4} << 20;
	// VM instructions one load or call may run.
	std::uint32_t instructions = 50000;
};

// A value that crosses the Lua boundary. Maps are sorted by key (so conversion
// order, and with it `pairs` order in Lua, never depends on how a map was built).
struct ScriptValue
{
	enum class Kind : std::uint8_t { Nil, Bool, Int, Number, String, Array, Map };
	using Entry = std::pair<std::string, ScriptValue>;

	Kind kind = Kind::Nil;
	bool boolean = false;
	std::int64_t integer = 0;
	double number = 0.0;
	std::string text;
	std::vector<ScriptValue> items; // Array
	std::vector<Entry> entries; // Map, ascending by key

	static ScriptValue ofBool(bool value);
	static ScriptValue ofInt(std::int64_t value);
	static ScriptValue ofNumber(double value);
	static ScriptValue ofString(std::string value);
	static ScriptValue array();
	static ScriptValue map();

	bool isNumber() const noexcept { return kind == Kind::Int || kind == Kind::Number; }
	// Int or Number as a double.
	double asNumber() const noexcept { return kind == Kind::Int ? static_cast<double>(integer) : number; }

	ScriptValue& push(ScriptValue value);
	// Sets (or replaces) a map entry; returns *this for chaining.
	ScriptValue& set(std::string_view key, ScriptValue value);
	const ScriptValue* find(std::string_view key) const;
};

enum class ScriptOutcome : std::uint8_t
{
	Ok,
	Error, // a Lua error (message in CallResult)
	Budget, // the instruction budget ran out
	Memory, // the memory budget ran out
	Unusable, // the call worked but the result could not cross (function, cycle, mixed table, too big)
	Missing, // the script has no such function
};

std::string_view scriptOutcomeName(ScriptOutcome outcome) noexcept;

struct CallResult
{
	ScriptOutcome outcome = ScriptOutcome::Ok;
	std::string message;

	bool ok() const noexcept { return outcome == ScriptOutcome::Ok; }
};

class ScriptHost
{
public:
	using Log = std::function<void(std::string_view)>;
	struct Impl; // opaque

	// Throws std::runtime_error if the state cannot be created.
	explicit ScriptHost(ScriptLimits limits = {}, Log log = {});
	~ScriptHost();
	ScriptHost(const ScriptHost&) = delete;
	ScriptHost& operator=(const ScriptHost&) = delete;

	// Loads `source` (Lua text; binary chunks are refused) under `name` with a fresh
	// _ENV and runs it. The chunk must return a table (the script's module). Returns a
	// handle >= 0, or -1 with `error` set. `manifest`, if given, receives the module's
	// non-function entries (subscriptions, declared fields, ...) as data, and `functions`
	// the names of its function entries.
	int load(std::string_view name, std::string_view source, ScriptValue* manifest,
		std::string* error, std::vector<std::string>* functions = nullptr);

	// Calls module[function](argument) and converts the one result. `rngSeed` seeds the
	// stream ksim.random() draws from during this call, so the draw is a function of the
	// caller's own state and not of the order calls happen in.
	CallResult call(int script, std::string_view function, const ScriptValue& argument,
		ScriptValue& result, std::uint64_t rngSeed);

	const std::string& name(int script) const;
	std::size_t memoryUsed() const noexcept;
	std::size_t memoryPeak() const noexcept;
	const ScriptLimits& limits() const noexcept;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace ksim
