#include "ksim/economy/economy_script.hpp"

#include "ksim/core/rng.hpp"
#include "ksim/core/simdata.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

namespace ksim::economy {

namespace {

std::uint64_t hashOf(std::string_view text) noexcept
{
	std::uint64_t hash = 1469598103934665603ull; // FNV-1a
	for (const char c : text)
	{
		hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
		hash *= 1099511628211ull;
	}
	return hash;
}

// The argument every hook gets: strings, numbers and a list, never an object-keyed map (script.hpp).
ScriptValue lineValue(const state::StockLine& line)
{
	ScriptValue value = ScriptValue::map();
	value.set("item", ScriptValue::ofString(line.itemSid));
	value.set("amount", ScriptValue::ofNumber(line.amount));
	value.set("target", ScriptValue::ofNumber(line.target));
	return value;
}

ScriptValue orderValue(const state::CaravanOrder& order)
{
	ScriptValue value = ScriptValue::map();
	value.set("item", ScriptValue::ofString(order.itemSid));
	value.set("amount", ScriptValue::ofNumber(order.amount));
	value.set("from", ScriptValue::ofInt(static_cast<std::int64_t>(order.from.id)));
	value.set("to", ScriptValue::ofInt(static_cast<std::int64_t>(order.to.id)));
	value.set("phase", ScriptValue::ofInt(static_cast<std::int64_t>(order.phase)));
	value.set("posted_at", ScriptValue::ofNumber(order.postedAt));
	return value;
}

// The draw an event's script gets: a function of the order itself, so it is the same after a load and whatever the call order.
std::uint64_t orderSeed(const state::CaravanOrder& order, std::uint64_t seed)
{
	return splitMix64(seed ^ splitMix64(order.from.id) ^ splitMix64(order.to.id) ^ splitMix64(hashOf(order.itemSid))
		^ splitMix64(static_cast<std::uint64_t>(std::llround(order.amount * 1000.0))));
}

// A finite positive factor from the script, or nothing (the engine rule).
bool factorOf(const ScriptValue& result, double& factor)
{
	if (!result.isNumber())
		return false;
	const double value = result.asNumber();
	if (!std::isfinite(value) || value <= 0.0)
		return false;
	factor = value;
	return true;
}

} // namespace

Hooks::Hooks(HookSetup setup, std::uint64_t seed)
	: setup_(std::move(setup))
	, seed_(seed)
{
	try
	{
		host_ = std::make_unique<ScriptHost>(setup_.limits,
			[this](std::string_view message) { say(std::string(message)); });
	} catch (const std::exception& error) {
		say("economy hooks '" + setup_.name + "' not loaded: " + error.what());
		return;
	}
	ScriptValue manifest;
	std::vector<std::string> functions;
	std::string error;
	handle_ = host_->load(setup_.name, setup_.source, &manifest, &error, &functions);
	if (handle_ < 0)
	{
		say("economy hooks '" + setup_.name + "' not loaded: " + error);
		handle_ = -1;
		return;
	}
	const ScriptValue* declared = manifest.find("api");
	const bool apiOk = declared != nullptr && declared->kind == ScriptValue::Kind::Int && declared->integer == 0;
	if (!apiOk)
	{
		say("economy hooks '" + setup_.name + "' not loaded: the script must declare api = 0");
		handle_ = -1;
		return;
	}
	const auto has = [&functions](const char* name) {
		return std::find(functions.begin(), functions.end(), name) != functions.end();
	};
	price_ = has("price");
	demand_ = has("demand");
	delivery_ = has("on_delivery");
	loss_ = has("on_loss");
	// A module with no hook at all is a mistake worth saying out loud, but it changes nothing: every call takes the engine rule.
	if (!price_ && !demand_ && !delivery_ && !loss_)
		say("economy hooks '" + setup_.name + "': none of price / demand / on_delivery / on_loss is defined");
}

std::shared_ptr<Hooks> Hooks::load(HookSetup setup, std::uint64_t seed)
{
	return std::shared_ptr<Hooks>(new Hooks(std::move(setup), seed));
}

std::shared_ptr<Hooks> Hooks::loadDefault(std::uint64_t seed, std::function<void(std::string_view)> log)
{
	HookSetup setup;
	setup.name = "economy.lua";
	setup.log = std::move(log);
	const std::string path = simDataBehaviourDir() + "/economy.lua";
	std::ifstream in(path, std::ios::binary);
	if (!in)
	{
		const std::string message = "economy hooks: the default script " + path + " cannot be read";
		if (setup.log)
			setup.log(message);
		else
			std::cerr << "ksim: " << message << "\n";
		return nullptr;
	}
	setup.source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	return load(std::move(setup), seed);
}

void Hooks::say(const std::string& message)
{
	if (setup_.log)
		setup_.log(message);
	else
		std::cerr << "ksim: " << message << "\n";
}

bool Hooks::callNumber(const char* function, const ScriptValue& argument, std::uint64_t rngSeed, double& out)
{
	if (handle_ < 0)
		return false;
	ScriptValue result;
	const CallResult call = host_->call(handle_, function, argument, result, rngSeed);
	if (!call.ok())
	{
		say(std::string("economy hook ") + function + " failed (" + std::string(scriptOutcomeName(call.outcome)) + "): "
			+ call.message + "; the engine rule is used for this call");
		return false;
	}
	return factorOf(result, out);
}

void Hooks::callEvent(const char* function, const ScriptValue& argument, std::uint64_t rngSeed)
{
	if (handle_ < 0)
		return;
	ScriptValue result;
	const CallResult call = host_->call(handle_, function, argument, result, rngSeed);
	if (!call.ok())
		say(std::string("economy hook ") + function + " failed (" + std::string(scriptOutcomeName(call.outcome)) + "): "
			+ call.message);
}

bool Hooks::price(std::string_view itemSid, std::uint64_t town, double stock, double target, double& factor)
{
	if (handle_ < 0 || !price_)
		return false;
	ScriptValue argument = ScriptValue::map();
	argument.set("item", ScriptValue::ofString(std::string(itemSid)));
	argument.set("town", ScriptValue::ofInt(static_cast<std::int64_t>(town)));
	argument.set("stock", ScriptValue::ofNumber(stock));
	argument.set("target", ScriptValue::ofNumber(target));
	// The draw is a function of the query, never of the order calls happen in (script.hpp): a cache may skip a call without changing
	// what the script would return.
	return callNumber("price", argument,
		splitMix64(seed_ ^ splitMix64(town) ^ splitMix64(hashOf(itemSid))), factor);
}

bool Hooks::demand(std::uint64_t town, double hour, state::TownStock& stock)
{
	if (handle_ < 0 || !demand_)
		return false;
	ScriptValue argument = ScriptValue::map();
	argument.set("town", ScriptValue::ofInt(static_cast<std::int64_t>(town)));
	argument.set("hour", ScriptValue::ofNumber(hour));
	ScriptValue list = ScriptValue::array();
	for (const state::StockLine& line : stock.lines)
		list.push(lineValue(line));
	argument.set("lines", std::move(list));
	ScriptValue result;
	const CallResult call = host_->call(handle_, "demand", argument, result,
		splitMix64(seed_ ^ splitMix64(town) ^ splitMix64(static_cast<std::uint64_t>(hour))));
	if (!call.ok())
	{
		say(std::string("economy hook demand failed (") + std::string(scriptOutcomeName(call.outcome)) + "): " + call.message
			+ "; the engine targets stand for this town");
		return false;
	}
	// { { item = sid, target = n }, ... } or { [sid] = target }; both cross as maps or arrays.
	std::vector<std::pair<std::string, double>> wanted;
	const auto add = [&wanted](const std::string& item, const ScriptValue& target) {
		if (!item.empty() && target.isNumber() && std::isfinite(target.asNumber()))
			wanted.emplace_back(item, target.asNumber());
	};
	if (result.kind == ScriptValue::Kind::Array)
	{
		for (const ScriptValue& entry : result.items)
		{
			if (entry.kind != ScriptValue::Kind::Map)
				continue;
			const ScriptValue* item = entry.find("item");
			const ScriptValue* target = entry.find("target");
			if (item != nullptr && item->kind == ScriptValue::Kind::String && target != nullptr)
				add(item->text, *target);
		}
	} else if (result.kind == ScriptValue::Kind::Map) {
		for (const auto& [item, target] : result.entries)
			add(item, target);
	} else if (result.kind != ScriptValue::Kind::Nil) {
		say("economy hook demand: the result is not a list of { item, target } or a map; the engine targets stand");
		return false;
	}
	for (const auto& [item, target] : wanted)
		lineFor(stock, item).target = target;
	return true;
}

void Hooks::onDelivery(const state::CaravanOrder& order)
{
	if (handle_ < 0 || !delivery_)
		return;
	callEvent("on_delivery", orderValue(order), orderSeed(order, seed_));
}

void Hooks::onLoss(const state::CaravanOrder& order)
{
	if (handle_ < 0 || !loss_)
		return;
	callEvent("on_loss", orderValue(order), orderSeed(order, seed_));
}

double priceFactor(Hooks* hooks, std::uint64_t town, const state::TownStock& stock, std::string_view itemSid)
{
	const state::StockLine* line = findLine(stock, itemSid);
	if (line == nullptr || line->target <= 0.0)
		return 1.0; // the town does not stock it: no T4 price, and nothing to hand a hook
	double factor = 0.0;
	if (hooks != nullptr && hooks->price(itemSid, town, line->amount, line->target, factor))
		return factor;
	return stockFactor(stock, itemSid);
}

} // namespace ksim::economy
