#include "ksim/core/script.hpp"

#include "ksim/core/rng.hpp"

#include <lua.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// Rules for this file (see the header): Lua errors longjmp. Every function that can
// raise (all the lua_* that allocate or call, luaL_*) lives in a static C-style
// function below that is only ever entered through lua_pcall and holds no object with a
// destructor. The conversion of results back to ScriptValue uses only API that cannot
// raise (type tests, lua_next on an unmodified table, lua_checkstack that reports).

namespace ksim {

namespace {

constexpr int kMaxDepth = 16; // nesting of tables crossing the boundary
constexpr int kMaxNodes = 4096; // values in one tree crossing the boundary
constexpr std::size_t kMaxText = 4096; // bytes of one string crossing back
constexpr std::size_t kMaxLogs = 16; // ksim.log lines kept per call
constexpr std::size_t kMaxLogBytes = 256;

struct Arena
{
	std::size_t used = 0;
	std::size_t peak = 0;
	std::size_t limit = 0;
	bool exceeded = false; // an allocation was refused since the flag was last cleared
};

struct Script
{
	std::string name;
	int ref = LUA_NOREF;
};

} // namespace

struct ScriptHost::Impl
{
	Arena arena;
	lua_State* state = nullptr;
	ScriptLimits limits;
	Log log;
	std::vector<Script> scripts;
	bool budgetTripped = false;
	bool rngActive = false;
	RngStream rng{1};
	std::vector<std::string> pendingLogs;
};

namespace {

using Impl = ScriptHost::Impl;

Impl* implOf(lua_State* L)
{
	return *static_cast<Impl**>(lua_getextraspace(L));
}

// The memory budget. With ptr == nullptr, osize is a type tag, not a size.
void* allocate(void* ud, void* ptr, std::size_t osize, std::size_t nsize)
{
	Arena* arena = static_cast<Arena*>(ud);
	const std::size_t old = ptr != nullptr ? osize : 0;
	if (nsize == 0)
	{
		if (ptr != nullptr)
		{
			arena->used -= old;
			std::free(ptr);
		}
		return nullptr;
	}
	if (nsize > old && arena->used + (nsize - old) > arena->limit)
	{
		arena->exceeded = true;
		return nullptr;
	}
	void* block = std::realloc(ptr, nsize);
	if (block == nullptr)
	{
		arena->exceeded = true;
		return nullptr;
	}
	arena->used = arena->used - old + nsize;
	arena->peak = std::max(arena->peak, arena->used);
	return block;
}

// The count hook: after the budget it raises, and from then on raises at every
// instruction, so a script that wraps its loop in pcall cannot swallow the stop.
void budgetHook(lua_State* L, lua_Debug*)
{
	Impl* impl = implOf(L);
	if (!impl->budgetTripped)
	{
		impl->budgetTripped = true;
		lua_sethook(L, budgetHook, LUA_MASKCOUNT, 1);
	}
	luaL_error(L, "instruction budget exceeded");
}

// ---- functions the scripts get ------------------------------------------------

int scriptCollectGarbage(lua_State* L)
{
	const char* option = luaL_optstring(L, 1, "collect");
	if (std::strcmp(option, "count") != 0)
		return luaL_error(L, "collectgarbage: only \"count\" is available");
	const int kilobytes = lua_gc(L, LUA_GCCOUNT);
	const int rest = lua_gc(L, LUA_GCCOUNTB);
	lua_pushnumber(L, kilobytes + rest / 1024.0);
	return 1;
}

// tostring of values that have no address in them. Tables, functions and userdata print
// their address, which differs between runs: refuse rather than leak it.
int scriptToString(lua_State* L)
{
	luaL_checkany(L, 1);
	const int type = lua_type(L, 1);
	if (type != LUA_TNIL && type != LUA_TBOOLEAN && type != LUA_TNUMBER && type != LUA_TSTRING)
		return luaL_error(L, "tostring: only nil, boolean, number and string (an address is not reproducible)");
	luaL_tolstring(L, 1, nullptr);
	return 1;
}

int scriptRandom(lua_State* L)
{
	Impl* impl = implOf(L);
	if (!impl->rngActive)
		return luaL_error(L, "ksim.random: not available while loading");
	lua_pushnumber(L, impl->rng.uniform01());
	return 1;
}

int scriptRandomInt(lua_State* L)
{
	Impl* impl = implOf(L);
	if (!impl->rngActive)
		return luaL_error(L, "ksim.random_int: not available while loading");
	const lua_Integer low = luaL_checkinteger(L, 1);
	const lua_Integer high = luaL_checkinteger(L, 2);
	lua_pushinteger(L, impl->rng.between(low, high));
	return 1;
}

int scriptLog(lua_State* L)
{
	Impl* impl = implOf(L);
	size_t length = 0;
	const char* text = luaL_checklstring(L, 1, &length);
	if (impl->pendingLogs.size() < kMaxLogs)
		impl->pendingLogs.emplace_back(text, std::min(length, kMaxLogBytes));
	return 0;
}

// ---- state setup --------------------------------------------------------------

void copyField(lua_State* L, const char* name)
{
	// base[name] = _G[name]; stack: base, _G
	lua_getfield(L, -1, name);
	lua_setfield(L, -3, name);
}

// Shallow copy of the table at the top of the stack (raw), leaving the copy on top.
void copyTable(lua_State* L)
{
	luaL_checkstack(L, 4, "copyTable");
	const int source = lua_gettop(L);
	lua_createtable(L, 0, 32);
	lua_pushnil(L);
	while (lua_next(L, source) != 0)
	{
		lua_pushvalue(L, -2);
		lua_insert(L, -2);
		lua_rawset(L, -4);
	}
}

// Opens the permitted libraries into a private "ksim.base" registry table and empties
// the real globals. Entered by lua_pcall with the Impl as light userdata.
int setupC(lua_State* L)
{
	luaL_checkstack(L, 12, "setup");
	lua_createtable(L, 0, 32); // base
	luaL_requiref(L, "_G", luaopen_base, 1); // also sets the globals
	static const char* const kBase[] = {"assert", "error", "ipairs", "next", "pairs", "pcall",
		"rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable", "tonumber", "type",
		"xpcall", "_VERSION"};
	for (const char* name : kBase)
		copyField(L, name);
	// Replacements for the base functions we do not hand out as they are.
	lua_pushcfunction(L, scriptToString);
	lua_setfield(L, -3, "tostring");
	lua_pushcfunction(L, scriptCollectGarbage);
	lua_setfield(L, -3, "collectgarbage");
	// Empty the real globals: nothing is reachable from them.
	lua_pushnil(L);
	while (lua_next(L, -2) != 0)
	{
		lua_pop(L, 1);
		lua_pushvalue(L, -1);
		lua_pushnil(L);
		lua_rawset(L, -4);
	}
	lua_pop(L, 1); // _G; stack: base

	struct Library
	{
		const char* name;
		lua_CFunction open;
		const char* removed[3];
	};
	static const Library kLibraries[] = {
		{LUA_STRLIBNAME, luaopen_string, {"dump", nullptr, nullptr}},
		{LUA_TABLIBNAME, luaopen_table, {nullptr, nullptr, nullptr}},
		{LUA_MATHLIBNAME, luaopen_math, {"random", "randomseed", nullptr}},
		{LUA_UTF8LIBNAME, luaopen_utf8, {nullptr, nullptr, nullptr}},
	};
	for (const Library& library : kLibraries)
	{
		luaL_requiref(L, library.name, library.open, 0);
		for (const char* removed : library.removed)
		{
			if (removed == nullptr)
				continue;
			lua_pushnil(L);
			lua_setfield(L, -2, removed);
		}
		lua_setfield(L, -2, library.name);
	}

	lua_createtable(L, 0, 4); // the ksim API table
	lua_pushcfunction(L, scriptRandom);
	lua_setfield(L, -2, "random");
	lua_pushcfunction(L, scriptRandomInt);
	lua_setfield(L, -2, "random_int");
	lua_pushcfunction(L, scriptLog);
	lua_setfield(L, -2, "log");
	lua_setfield(L, -2, "ksim");

	lua_setfield(L, LUA_REGISTRYINDEX, "ksim.base");
	return 0;
}

// A fresh _ENV: its own copy of every function table, nothing shared mutable.
void pushEnvironment(lua_State* L)
{
	luaL_checkstack(L, 8, "environment");
	lua_getfield(L, LUA_REGISTRYINDEX, "ksim.base");
	const int base = lua_gettop(L);
	lua_createtable(L, 0, 32);
	lua_pushnil(L);
	while (lua_next(L, base) != 0)
	{
		if (lua_type(L, -1) == LUA_TTABLE)
			copyTable(L);
		else
			lua_pushvalue(L, -1);
		// stack: ..., key, value, copy
		lua_pushvalue(L, -3);
		lua_insert(L, -2);
		lua_rawset(L, base + 1);
		lua_pop(L, 1);
	}
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "_G");
	lua_remove(L, base);
}

struct LoadArgs
{
	const char* chunkName;
	const char* source;
	std::size_t length;
	int ref;
};

int loadC(lua_State* L)
{
	LoadArgs* args = static_cast<LoadArgs*>(lua_touserdata(L, 1));
	if (luaL_loadbufferx(L, args->source, args->length, args->chunkName, "t") != LUA_OK)
		return lua_error(L);
	pushEnvironment(L);
	if (lua_setupvalue(L, -2, 1) == nullptr)
		return luaL_error(L, "script has no _ENV");
	lua_call(L, 0, 1);
	if (!lua_istable(L, -1))
		return luaL_error(L, "the script must return a table");
	args->ref = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

// ScriptValue -> Lua. Pure reader of its input; raises only through the Lua API.
void pushValue(lua_State* L, const ScriptValue& value, int depth)
{
	luaL_checkstack(L, 4, "ScriptValue too deep");
	switch (value.kind)
	{
	case ScriptValue::Kind::Nil:
		lua_pushnil(L);
		break;
	case ScriptValue::Kind::Bool:
		lua_pushboolean(L, value.boolean ? 1 : 0);
		break;
	case ScriptValue::Kind::Int:
		lua_pushinteger(L, static_cast<lua_Integer>(value.integer));
		break;
	case ScriptValue::Kind::Number:
		lua_pushnumber(L, value.number);
		break;
	case ScriptValue::Kind::String:
		lua_pushlstring(L, value.text.data(), value.text.size());
		break;
	case ScriptValue::Kind::Array:
		if (depth >= kMaxDepth)
			luaL_error(L, "ScriptValue nested too deep");
		lua_createtable(L, static_cast<int>(value.items.size()), 0);
		for (std::size_t i = 0; i < value.items.size(); ++i)
		{
			pushValue(L, value.items[i], depth + 1);
			lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
		}
		break;
	case ScriptValue::Kind::Map:
		if (depth >= kMaxDepth)
			luaL_error(L, "ScriptValue nested too deep");
		lua_createtable(L, 0, static_cast<int>(value.entries.size()));
		for (std::size_t i = 0; i < value.entries.size(); ++i)
		{
			pushValue(L, value.entries[i].second, depth + 1);
			lua_setfield(L, -2, value.entries[i].first.c_str());
		}
		break;
	}
}

struct CallArgs
{
	int ref;
	const char* function;
	const ScriptValue* argument;
	bool missing;
};

int callC(lua_State* L)
{
	CallArgs* args = static_cast<CallArgs*>(lua_touserdata(L, 1));
	lua_rawgeti(L, LUA_REGISTRYINDEX, args->ref);
	lua_pushstring(L, args->function);
	lua_rawget(L, -2);
	if (!lua_isfunction(L, -1))
	{
		args->missing = true;
		return 0;
	}
	pushValue(L, *args->argument, 0);
	lua_call(L, 1, 1);
	return 1;
}

// Lua -> ScriptValue. Never raises (see the top of the file).
struct Reader
{
	lua_State* L;
	int nodes = 0;
	bool skipFunctions = false; // top level only: drop function entries of a module table
	std::vector<std::string>* skipped = nullptr; // ... and name them here
	const char* why = nullptr;
};

bool readValue(Reader& reader, int index, ScriptValue& out, int depth, bool top)
{
	lua_State* L = reader.L;
	if (++reader.nodes > kMaxNodes)
	{
		reader.why = "result too large";
		return false;
	}
	index = lua_absindex(L, index);
	switch (lua_type(L, index))
	{
	case LUA_TNIL:
		out = ScriptValue{};
		return true;
	case LUA_TBOOLEAN:
		out = ScriptValue::ofBool(lua_toboolean(L, index) != 0);
		return true;
	case LUA_TNUMBER:
		if (lua_isinteger(L, index))
			out = ScriptValue::ofInt(lua_tointeger(L, index));
		else
			out = ScriptValue::ofNumber(lua_tonumber(L, index));
		return true;
	case LUA_TSTRING:
	{
		size_t length = 0;
		const char* text = lua_tolstring(L, index, &length);
		if (length > kMaxText)
		{
			reader.why = "string too long";
			return false;
		}
		out = ScriptValue::ofString(std::string(text, length));
		return true;
	}
	case LUA_TTABLE:
		break;
	default:
		reader.why = "a function, userdata or thread cannot cross";
		return false;
	}
	if (depth >= kMaxDepth || !lua_checkstack(L, 6))
	{
		reader.why = "nested too deep";
		return false;
	}
	std::vector<std::pair<std::int64_t, ScriptValue>> indexed;
	std::vector<ScriptValue::Entry> keyed;
	lua_pushnil(L);
	while (lua_next(L, index) != 0)
	{
		// key at -2, value at -1
		const int keyType = lua_type(L, -2);
		if (top && reader.skipFunctions && lua_type(L, -1) == LUA_TFUNCTION && keyType == LUA_TSTRING)
		{
			if (reader.skipped != nullptr)
			{
				size_t length = 0;
				const char* key = lua_tolstring(L, -2, &length);
				reader.skipped->emplace_back(key, length);
			}
			lua_pop(L, 1);
			continue;
		}
		ScriptValue element;
		if (!readValue(reader, -1, element, depth + 1, false))
		{
			if (top && reader.skipFunctions)
			{
				// A module's helper entries need not be data: leave out what cannot cross.
				reader.why = nullptr;
				lua_pop(L, 1);
				continue;
			}
			lua_pop(L, 2);
			return false;
		}
		if (keyType == LUA_TSTRING)
		{
			size_t length = 0;
			const char* key = lua_tolstring(L, -2, &length);
			keyed.emplace_back(std::string(key, length), std::move(element));
		}
		else if (keyType == LUA_TNUMBER && lua_isinteger(L, -2))
		{
			indexed.emplace_back(lua_tointeger(L, -2), std::move(element));
		}
		else
		{
			lua_pop(L, 2);
			reader.why = "a table key must be a string or an integer";
			return false;
		}
		lua_pop(L, 1);
	}
	if (!indexed.empty() && !keyed.empty())
	{
		reader.why = "a table mixes an array part and named keys";
		return false;
	}
	if (!indexed.empty())
	{
		std::sort(indexed.begin(), indexed.end(),
			[](const auto& left, const auto& right) { return left.first < right.first; });
		out = ScriptValue::array();
		for (std::size_t i = 0; i < indexed.size(); ++i)
		{
			if (indexed[i].first != static_cast<std::int64_t>(i + 1))
			{
				reader.why = "an array has holes";
				return false;
			}
			out.items.push_back(std::move(indexed[i].second));
		}
		return true;
	}
	std::sort(keyed.begin(), keyed.end(),
		[](const auto& left, const auto& right) { return left.first < right.first; });
	out = ScriptValue::map();
	out.entries = std::move(keyed);
	return true;
}

} // namespace

// ---- ScriptValue ----------------------------------------------------------------

ScriptValue ScriptValue::ofBool(bool value)
{
	ScriptValue out;
	out.kind = Kind::Bool;
	out.boolean = value;
	return out;
}

ScriptValue ScriptValue::ofInt(std::int64_t value)
{
	ScriptValue out;
	out.kind = Kind::Int;
	out.integer = value;
	return out;
}

ScriptValue ScriptValue::ofNumber(double value)
{
	ScriptValue out;
	out.kind = Kind::Number;
	out.number = value;
	return out;
}

ScriptValue ScriptValue::ofString(std::string value)
{
	ScriptValue out;
	out.kind = Kind::String;
	out.text = std::move(value);
	return out;
}

ScriptValue ScriptValue::array()
{
	ScriptValue out;
	out.kind = Kind::Array;
	return out;
}

ScriptValue ScriptValue::map()
{
	ScriptValue out;
	out.kind = Kind::Map;
	return out;
}

ScriptValue& ScriptValue::push(ScriptValue value)
{
	items.push_back(std::move(value));
	return *this;
}

ScriptValue& ScriptValue::set(std::string_view key, ScriptValue value)
{
	const auto at = std::lower_bound(entries.begin(), entries.end(), key,
		[](const Entry& entry, std::string_view wanted) { return entry.first < wanted; });
	if (at != entries.end() && at->first == key)
		at->second = std::move(value);
	else
		entries.insert(at, Entry{std::string(key), std::move(value)});
	return *this;
}

const ScriptValue* ScriptValue::find(std::string_view key) const
{
	const auto at = std::lower_bound(entries.begin(), entries.end(), key,
		[](const Entry& entry, std::string_view wanted) { return entry.first < wanted; });
	return at != entries.end() && at->first == key ? &at->second : nullptr;
}

std::string_view scriptOutcomeName(ScriptOutcome outcome) noexcept
{
	switch (outcome)
	{
	case ScriptOutcome::Ok: return "ok";
	case ScriptOutcome::Error: return "error";
	case ScriptOutcome::Budget: return "instruction budget";
	case ScriptOutcome::Memory: return "memory budget";
	case ScriptOutcome::Unusable: return "unusable result";
	case ScriptOutcome::Missing: return "missing function";
	}
	return "?";
}

// ---- ScriptHost -----------------------------------------------------------------

ScriptHost::ScriptHost(ScriptLimits limits, Log log)
	: impl_(std::make_unique<Impl>())
{
	Impl& m = *impl_;
	m.limits = limits;
	m.log = std::move(log);
	m.arena.limit = limits.memoryBytes;
	m.state = lua_newstate(allocate, &m.arena, kScriptSeed);
	if (m.state == nullptr)
		throw std::runtime_error("lua_newstate failed (memory budget too small?)");
	*static_cast<Impl**>(lua_getextraspace(m.state)) = &m;
	lua_pushcfunction(m.state, setupC);
	if (lua_pcall(m.state, 0, 0, 0) != LUA_OK)
	{
		const char* message = lua_tostring(m.state, -1);
		const std::string text = message != nullptr ? message : "?";
		lua_close(m.state);
		m.state = nullptr;
		throw std::runtime_error("script sandbox setup failed: " + text);
	}
}

ScriptHost::~ScriptHost()
{
	if (impl_ != nullptr && impl_->state != nullptr)
		lua_close(impl_->state);
}

int ScriptHost::load(std::string_view name, std::string_view source, ScriptValue* manifest,
	std::string* error, std::vector<std::string>* functions)
{
	Impl& m = *impl_;
	lua_State* L = m.state;
	const int top = lua_gettop(L);
	const std::string chunkName = "=" + std::string(name);
	LoadArgs args{chunkName.c_str(), source.data(), source.size(), LUA_NOREF};
	m.budgetTripped = false;
	m.arena.exceeded = false;
	m.rngActive = false;
	lua_pushcfunction(L, loadC);
	lua_pushlightuserdata(L, &args);
	lua_sethook(L, budgetHook, LUA_MASKCOUNT, static_cast<int>(m.limits.instructions));
	const int status = lua_pcall(L, 1, 0, 0);
	lua_sethook(L, nullptr, 0, 0);
	if (status != LUA_OK || m.budgetTripped)
	{
		if (error != nullptr)
		{
			const char* message = status != LUA_OK ? lua_tostring(L, -1) : nullptr;
			*error = m.budgetTripped ? "instruction budget exceeded while loading"
				: message != nullptr ? message : "(non-string error)";
		}
		lua_settop(L, top);
		if (args.ref != LUA_NOREF)
			luaL_unref(L, LUA_REGISTRYINDEX, args.ref);
		return -1;
	}
	if (manifest != nullptr)
	{
		Reader reader{L};
		reader.skipFunctions = true;
		reader.skipped = functions;
		lua_rawgeti(L, LUA_REGISTRYINDEX, args.ref);
		if (!readValue(reader, -1, *manifest, 0, true))
		{
			if (error != nullptr)
				*error = std::string("the script table cannot be read as data: ") + reader.why;
			lua_settop(L, top);
			luaL_unref(L, LUA_REGISTRYINDEX, args.ref);
			return -1;
		}
		lua_pop(L, 1);
	}
	lua_settop(L, top);
	m.scripts.push_back(Script{std::string(name), args.ref});
	return static_cast<int>(m.scripts.size() - 1);
}

CallResult ScriptHost::call(int script, std::string_view function, const ScriptValue& argument,
	ScriptValue& result, std::uint64_t rngSeed)
{
	Impl& m = *impl_;
	CallResult out;
	if (script < 0 || static_cast<std::size_t>(script) >= m.scripts.size())
	{
		out.outcome = ScriptOutcome::Missing;
		out.message = "no such script";
		return out;
	}
	lua_State* L = m.state;
	const int top = lua_gettop(L);
	const std::string functionName(function);
	CallArgs args{m.scripts[static_cast<std::size_t>(script)].ref, functionName.c_str(), &argument, false};
	m.budgetTripped = false;
	m.arena.exceeded = false;
	m.rng = RngStream(splitMix64(rngSeed ^ 0x5C21D7ULL));
	m.rngActive = true;
	m.pendingLogs.clear();
	lua_pushcfunction(L, callC);
	lua_pushlightuserdata(L, &args);
	lua_sethook(L, budgetHook, LUA_MASKCOUNT, static_cast<int>(m.limits.instructions));
	const int status = lua_pcall(L, 1, 1, 0);
	lua_sethook(L, nullptr, 0, 0);
	m.rngActive = false;
	if (status == LUA_OK && !m.budgetTripped)
	{
		if (args.missing)
		{
			out.outcome = ScriptOutcome::Missing;
			out.message = "the script has no function '" + functionName + "'";
		} else {
			Reader reader{L};
			if (!readValue(reader, -1, result, 0, false))
			{
				out.outcome = ScriptOutcome::Unusable;
				out.message = reader.why != nullptr ? reader.why : "unreadable result";
			}
		}
	}
	else if (m.budgetTripped)
	{
		out.outcome = ScriptOutcome::Budget;
		out.message = "more than " + std::to_string(m.limits.instructions) + " instructions";
	}
	else
	{
		const char* message = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
		out.outcome = status == LUA_ERRMEM ? ScriptOutcome::Memory : ScriptOutcome::Error;
		out.message = message != nullptr ? message : "(error value is not a string)";
		if (status == LUA_ERRMEM)
			out.message = "memory budget of " + std::to_string(m.limits.memoryBytes) + " bytes exceeded";
	}
	lua_settop(L, top);
	if (m.log)
	{
		for (const std::string& line : m.pendingLogs)
			m.log("[" + m.scripts[static_cast<std::size_t>(script)].name + "] " + line);
	}
	return out;
}

const std::string& ScriptHost::name(int script) const
{
	return impl_->scripts.at(static_cast<std::size_t>(script)).name;
}

std::size_t ScriptHost::memoryUsed() const noexcept
{
	return impl_->arena.used;
}

std::size_t ScriptHost::memoryPeak() const noexcept
{
	return impl_->arena.peak;
}

const ScriptLimits& ScriptHost::limits() const noexcept
{
	return impl_->limits;
}

} // namespace ksim
