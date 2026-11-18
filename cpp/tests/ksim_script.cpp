// The sandboxed Lua host (include/ksim/script.hpp): what a script can and cannot
// reach, the instruction and memory budgets, error reporting, the data boundary and
// reproducible iteration order. No game data needed.

#include "ksim/script.hpp"

#include "test_util.hpp"

#include <iostream>
#include <string>

namespace {

using ksim::CallResult;
using ksim::ScriptHost;
using ksim::ScriptOutcome;
using ksim::ScriptValue;

int load(ScriptHost& host, const std::string& name, const std::string& source,
	ScriptValue* manifest = nullptr, std::string* error = nullptr)
{
	std::string local;
	const int handle = host.load(name, source, manifest, error != nullptr ? error : &local);
	return handle;
}

// A script with one function `f` returning what `body` computes.
std::string scriptOf(const std::string& body)
{
	return "return { f = function(arg) " + body + " end }";
}

CallResult run(ScriptHost& host, const std::string& body, ScriptValue& result,
	const ScriptValue& arg = {})
{
	std::string error;
	const int handle = load(host, "t", scriptOf(body), nullptr, &error);
	test::require(handle >= 0, "load failed: " + error);
	return host.call(handle, "f", arg, result, 1);
}

void expectOk(ScriptHost& host, const std::string& body, const std::string& expectText)
{
	ScriptValue result;
	const CallResult out = run(host, body, result);
	test::require(out.ok(), "call failed (" + body + "): " + out.message);
	test::require(result.kind == ScriptValue::Kind::String && result.text == expectText,
		"'" + body + "' gave '" + result.text + "', expected '" + expectText + "'");
}

} // namespace

int main()
{
	try
	{
		// 1. The environment: what exists, what does not.
		{
			ScriptHost host;
			// Present.
			for (const char* name : {"assert", "error", "ipairs", "next", "pairs", "pcall", "rawget",
				"select", "setmetatable", "tonumber", "tostring", "type", "xpcall", "collectgarbage"})
				expectOk(host, std::string("return type(") + name + ")", "function");
			expectOk(host, "return type(string.format) .. type(table.sort) .. type(math.floor) .. type(utf8.char)",
				"functionfunctionfunctionfunction");
			expectOk(host, "return string.format('%.1f', 2.5) .. ('x'):rep(3) .. #utf8.char(228)", "2.5xxx2");
			// Absent.
			for (const char* name : {"load", "loadstring", "loadfile", "dofile", "require", "print",
				"getmetatable", "io", "os", "debug", "package", "coroutine", "string.dump",
				"math.random", "math.randomseed", "warn"})
			{
				// Evaluate `a.b` chains without erroring on the missing leaf.
				expectOk(host, std::string("local v = _ENV; for part in ('") + name
					+ "'):gmatch('[^.]+') do v = v and v[part] end return type(v)", "nil");
			}
			// collectgarbage: "count" only.
			ScriptValue result;
			CallResult out = run(host, "return collectgarbage('count')", result);
			test::require(out.ok() && result.isNumber() && result.asNumber() > 0.0, "collectgarbage('count')");
			out = run(host, "return collectgarbage('collect')", result);
			test::require(out.outcome == ScriptOutcome::Error, "collectgarbage('collect') worked");
			// tostring refuses addresses.
			out = run(host, "return tostring({})", result);
			test::require(out.outcome == ScriptOutcome::Error, "tostring of a table worked");
			// Text mode only: a precompiled chunk is refused.
			std::string error;
			const int binary = host.load("bin", std::string("\x1bLua", 4) + "junk", nullptr, &error);
			test::require(binary < 0 && error.find("binary") != std::string::npos,
				"a binary chunk was accepted: " + error);
			// A syntax error is a load failure with the script name and line.
			const int broken = host.load("broken.lua", "return {\n f = function( end }", nullptr, &error);
			test::require(broken < 0 && error.find("broken.lua:2") != std::string::npos,
				"syntax error not located: " + error);
			std::cout << "environment: whitelist, text-only loading, located syntax errors ok\n";
		}

		// 2. Each script has its own _ENV and its own copies of the libraries.
		{
			ScriptHost host;
			const int a = load(host, "a", "string.shout = function() return 'a' end; leak = 1; "
				"return { f = function() return string.shout() .. tostring(leak) end, "
				"g = function() return tostring(leak) end }");
			const int b = load(host, "b", "return { f = function() return tostring(leak) .. type(string.shout) end }");
			test::require(a >= 0 && b >= 0, "load");
			ScriptValue result;
			test::require(host.call(a, "f", {}, result, 1).ok() && result.text == "a1", "a's own env");
			test::require(host.call(b, "f", {}, result, 1).ok() && result.text == "nilnil",
				"b sees a's globals or library edits: " + result.text);
			// A hostile edit of the string table inside a script does not reach other strings.
			expectOk(host, "string.rep = nil; return ('x'):rep(2)", "xx");
			std::cout << "isolation: per-script _ENV and library copies ok\n";
		}

		// 3. Budgets.
		{
			ksim::ScriptLimits limits;
			limits.instructions = 20000;
			limits.memoryBytes = std::size_t{1} << 20;
			ScriptHost host(limits);
			ScriptValue result;
			CallResult out = run(host, "while true do end", result);
			test::require(out.outcome == ScriptOutcome::Budget, "an endless loop was not stopped: "
				+ std::string(ksim::scriptOutcomeName(out.outcome)));
			// pcall cannot swallow the stop.
			out = run(host, "repeat pcall(function() while true do end end) until false", result);
			test::require(out.outcome == ScriptOutcome::Budget, "pcall swallowed the budget");
			out = run(host, "local t = {} for i = 1, 1e7 do t[i] = i end", result);
			test::require(out.outcome == ScriptOutcome::Budget || out.outcome == ScriptOutcome::Memory,
				"an unbounded table was not stopped");
			out = run(host, "local t = {} for i = 1, 100000 do t[i] = {i} end return 1", result);
			test::require(out.outcome == ScriptOutcome::Memory || out.outcome == ScriptOutcome::Budget,
				"memory growth was not stopped");
			out = run(host, "return #string.rep('x', 100000000)", result);
			test::require(out.outcome == ScriptOutcome::Memory, "a huge string was allowed: "
				+ std::string(ksim::scriptOutcomeName(out.outcome)) + " " + out.message);
			test::require(host.memoryPeak() <= limits.memoryBytes, "the allocator went past its budget");
			// And the host is fine afterwards.
			expectOk(host, "return 'alive'", "alive");
			out = run(host, "error('boom')", result);
			test::require(out.outcome == ScriptOutcome::Error && out.message.find("boom") != std::string::npos
				&& out.message.find("t:1") != std::string::npos, "error message: " + out.message);
			out = run(host, "error({})", result);
			test::require(out.outcome == ScriptOutcome::Error, "a non-string error");
			std::cout << "budgets: loop, pcall loop, table growth, huge string stopped; host alive; peak "
				<< host.memoryPeak() << " of " << limits.memoryBytes << "\n";
		}

		// 4. The data boundary.
		{
			ScriptHost host;
			ScriptValue arg = ScriptValue::map();
			arg.set("n", ScriptValue::ofInt(3)).set("x", ScriptValue::ofNumber(0.5))
				.set("s", ScriptValue::ofString("hi")).set("b", ScriptValue::ofBool(true));
			ScriptValue list = ScriptValue::array();
			list.push(ScriptValue::ofString("a")).push(ScriptValue::ofString("b"));
			arg.set("list", list);
			ScriptValue result;
			CallResult out = run(host, "return { sum = arg.n + arg.x, type_n = math.type(arg.n), "
				"type_x = math.type(arg.x), s = arg.s .. #arg.list, b = arg.b, list = arg.list }", result, arg);
			test::require(out.ok(), out.message);
			test::require(result.find("sum")->asNumber() == 3.5 && result.find("type_n")->text == "integer"
				&& result.find("type_x")->text == "float" && result.find("s")->text == "hi2"
				&& result.find("b")->boolean && result.find("list")->items.size() == 2, "round trip");
			// Unusable results.
			for (const char* body : {"return function() end", "return { f = ipairs }", "return {1, 2, nil, 4}",
				"return { 1, a = 2 }", "local t = {} t.t = t return t", "return { [1.5] = 1 }",
				"return string.rep('x', 5000)", "local t = {} for i = 1, 5000 do t[i] = i end return t"})
			{
				out = run(host, body, result);
				test::require(out.outcome == ScriptOutcome::Unusable, std::string("'") + body
					+ "' was usable: " + std::string(ksim::scriptOutcomeName(out.outcome)));
			}
			// Missing function.
			std::string error;
			const int handle = load(host, "m", "return { a = 1 }", nullptr, &error);
			out = host.call(handle, "nothing", {}, result, 1);
			test::require(out.outcome == ScriptOutcome::Missing, "a missing function");
			// The manifest carries the data, not the functions.
			ScriptValue manifest;
			load(host, "man", "return { api = 0, subs = { { on = 'timer', every_hours = 1 } }, f = function() end }",
				&manifest);
			test::require(manifest.find("api") != nullptr && manifest.find("subs") != nullptr
				&& manifest.find("f") == nullptr && manifest.find("subs")->items.size() == 1, "manifest");
			std::cout << "boundary: types, sorted maps, unusable results, manifest ok\n";
		}

		// 5. Reproducible order and the seeded RNG.
		{
			const std::string body = "local t = {} for i = 1, 40 do t['k' .. i * 7 % 41] = i end "
				"local out = {} for k, v in pairs(t) do out[#out + 1] = k end return table.concat(out, ',')";
			std::string first;
			for (int i = 0; i < 3; ++i)
			{
				ScriptHost host;
				ScriptValue result;
				test::require(run(host, body, result).ok(), "pairs order script");
				if (i == 0)
					first = result.text;
				test::require(result.text == first, "pairs order differs between states");
			}
			ScriptHost host;
			ScriptValue a;
			ScriptValue b;
			ScriptValue c;
			const std::string rng = "return { ksim.random(), ksim.random_int(1, 100), ksim.random() }";
			const int handle = load(host, "rng", scriptOf(rng));
			test::require(host.call(handle, "f", {}, a, 42).ok() && host.call(handle, "f", {}, b, 42).ok()
				&& host.call(handle, "f", {}, c, 43).ok(), "rng calls");
			test::require(a.items[0].asNumber() == b.items[0].asNumber() && a.items[1].integer == b.items[1].integer
				&& a.items[2].asNumber() == b.items[2].asNumber(), "same seed, different draws");
			test::require(a.items[0].asNumber() != c.items[0].asNumber(), "different seed, same draw");
			std::cout << "determinism: pairs order identical across states (" << first.substr(0, 24)
				<< "...), ksim.random a function of the call seed\n";
		}

		std::cout << "ksim_script: ok\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_script: " << error.what() << "\n";
		return 1;
	}
}
