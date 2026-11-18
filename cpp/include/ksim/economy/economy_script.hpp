#pragma once

// The economy's Lua hooks (TOWN_LIFE_SLICE 7.8, implemented in 7.13; ARCHITECTURE.md "Behaviour layer: Lua"). A mod replaces the
// engine's rules one call at a time: economy.price / economy.demand / economy.on_delivery / economy.on_loss. Every hook defaults to
// the engine rule -- a script without the function, or one that errors, gets it for that call (logged, never a silent session-wide
// fallback). The script is a process setting, never saved; `original` economy mode calls no hook at all.
//
// One Hooks object owns one lua_State (see script.hpp): not thread safe, not shared between threads. Simulations that trade or step
// the economy on one thread share one; a test and its world may hold the same one.

#include "ksim/economy/economy.hpp"
#include "ksim/core/script.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ksim::economy {

struct HookSetup
{
	std::string name = "economy.lua"; // for messages
	std::string source; // the script text (a file's contents)
	ScriptLimits limits;
	std::function<void(std::string_view)> log; // load errors and per-call fallbacks; stderr by default
};

class Hooks
{
public:
	// Loads the module `{ api = 0, price = fn, demand = fn, on_delivery = fn, on_loss = fn }`. A script that fails to load logs and
	// yields a Hooks whose calls all return false (the engine rules), never an exception: a broken mod cannot stop the game.
	static std::shared_ptr<Hooks> load(HookSetup setup, std::uint64_t seed);
	// The shipped default (data/behaviour/default/economy.lua): the engine's rules written in Lua, the starting point for a mod. A file
	// that cannot be read is logged and leaves no hooks.
	static std::shared_ptr<Hooks> loadDefault(std::uint64_t seed, std::function<void(std::string_view)> log = {});

	bool hasPrice() const noexcept { return price_; }
	bool hasDemand() const noexcept { return demand_; }
	bool hasDelivery() const noexcept { return delivery_; }
	bool hasLoss() const noexcept { return loss_; }

	// economy.price(item, town, stock, target) -> the factor multiplying both sides of a quote. False when the script has no such
	// function or it could not answer (logged): the caller uses the engine's stockFactor for this call.
	bool price(std::string_view itemSid, std::uint64_t town, double stock, double target, double& factor);

	// economy.demand(town) -> { {item = sid, target = n}, ... } (or { [sid] = target }): the town's target levels, applied to the
	// stock (a new item gets a line at 0). False when the script has no such function or it could not answer; the lines are left
	// as they are.
	bool demand(std::uint64_t town, double hour, state::TownStock& stock);

	// economy.on_delivery(order), economy.on_loss(order): a delivery arrived / the carrier died with it. Errors are logged and
	// swallowed (nothing to fall back to). Each fires once per order.
	void onDelivery(const state::CaravanOrder& order);
	void onLoss(const state::CaravanOrder& order);

	const std::string& name() const noexcept { return setup_.name; }

private:
	Hooks(HookSetup setup, std::uint64_t seed);
	void say(const std::string& message);
	// Calls `function(argument)` and takes a number back; false (logged) on an error, a budget, or a non-number.
	bool callNumber(const char* function, const ScriptValue& argument, std::uint64_t rngSeed, double& out);
	// Calls `function(argument)`; the result is discarded (an event). An error is logged and swallowed.
	void callEvent(const char* function, const ScriptValue& argument, std::uint64_t rngSeed);

	std::unique_ptr<ScriptHost> host_;
	int handle_ = -1; // -1 = not loaded
	bool price_ = false;
	bool demand_ = false;
	bool delivery_ = false;
	bool loss_ = false;
	HookSetup setup_;
	std::uint64_t seed_ = 0;
};

// The factor a quote takes: the script's economy.price when it answers, else the engine's stockFactor. `hooks` may be null.
double priceFactor(Hooks* hooks, std::uint64_t town, const state::TownStock& stock, std::string_view itemSid);

} // namespace ksim::economy
