#include "world/factions_script.hpp"

#include "ksim/core/clock.hpp"
#include "ksim/core/rng.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace ksim {

namespace {

constexpr std::size_t kMaxFields = 16;
constexpr std::size_t kMaxEventsPerCall = 64;

const char* const kIntentNames[state::kIntentCount] = {"consolidate", "defend", "raid", "mourn"};

// An empty Lua table crosses as an empty map: both are an empty list.
const std::vector<ScriptValue>* listOf(const ScriptValue& value)
{
	static const std::vector<ScriptValue> kEmpty;
	if (value.kind == ScriptValue::Kind::Array)
		return &value.items;
	if (value.kind == ScriptValue::Kind::Map && value.entries.empty())
		return &kEmpty;
	return nullptr;
}

bool integerOf(const ScriptValue& value, std::int64_t& out)
{
	if (value.kind == ScriptValue::Kind::Int)
	{
		out = value.integer;
		return true;
	}
	if (value.kind == ScriptValue::Kind::Number && std::isfinite(value.number)
		&& value.number == std::floor(value.number) && std::abs(value.number) < 9.0e15)
	{
		out = static_cast<std::int64_t>(value.number);
		return true;
	}
	return false;
}

bool validName(const std::string& name)
{
	if (name.empty() || name.size() > 32 || !(name[0] == '_' || (name[0] >= 'a' && name[0] <= 'z')))
		return false;
	return std::all_of(name.begin(), name.end(), [](char c)
	{
		return c == '_' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
	});
}

std::string_view wakeName(int wake)
{
	static const char* const kNames[] = {"timer", "squad_lost", "town_lost", "town_gained",
		"raid_outcome", "relation_crossed"};
	return kNames[wake];
}

std::uint64_t idOf(const flecs::world& ecs, flecs::entity_t entity)
{
	if (entity == 0)
		return 0;
	const state::Id* id = flecs::entity(ecs.c_ptr(), entity).get<state::Id>();
	return id != nullptr ? id->value : 0;
}

} // namespace

FactionScript::FactionScript(FactionScriptSetup setup, std::uint64_t seed)
	: setup_(std::move(setup))
	, seed_(seed)
{
	try
	{
		host_ = std::make_unique<ScriptHost>(setup_.limits);
	}
	catch (const std::exception& error)
	{
		say(std::string("faction script '") + setup_.name + "' not loaded: " + error.what());
		return;
	}
	ScriptValue manifest;
	std::vector<std::string> functions;
	std::string error;
	const int handle = host_->load(setup_.name, setup_.source, &manifest, &error, &functions);
	if (handle < 0)
	{
		say("faction script '" + setup_.name + "' not loaded: " + error);
		return;
	}
	if (!parseManifest(manifest, functions, error))
	{
		say("faction script '" + setup_.name + "' not loaded: " + error);
		return;
	}
	handle_ = handle;
}

void FactionScript::say(const std::string& message)
{
	if (setup_.log)
		setup_.log(message);
	else
		std::cerr << "ksim: " << message << "\n";
}

bool FactionScript::parseManifest(const ScriptValue& manifest,
	const std::vector<std::string>& functions, std::string& error)
{
	if (manifest.kind != ScriptValue::Kind::Map)
	{
		error = "the script must return a table";
		return false;
	}
	std::int64_t api = -1;
	const ScriptValue* apiValue = manifest.find("api");
	if (apiValue == nullptr || !integerOf(*apiValue, api) || api != 0)
	{
		error = "the script must declare api = 0 (the API version it was written for)";
		return false;
	}
	if (std::find(functions.begin(), functions.end(), "decide") == functions.end())
	{
		error = "the script has no function 'decide'";
		return false;
	}
	if (const ScriptValue* list = manifest.find("subscriptions"))
	{
		const std::vector<ScriptValue>* items = listOf(*list);
		if (items == nullptr)
		{
			error = "'subscriptions' must be a list";
			return false;
		}
		for (const ScriptValue& item : *items)
		{
			const ScriptValue* on = item.kind == ScriptValue::Kind::Map ? item.find("on") : nullptr;
			if (on == nullptr || on->kind != ScriptValue::Kind::String)
			{
				error = "a subscription needs a string 'on'";
				return false;
			}
			Subscription subscription;
			int wake = -1;
			for (int i = 0; i < 6; ++i)
				wake = on->text == wakeName(i) ? i : wake;
			if (wake < 0)
			{
				error = "unknown subscription '" + on->text + "' (timer, squad_lost, town_lost, "
					"town_gained, raid_outcome, relation_crossed)";
				return false;
			}
			subscription.wake = static_cast<Wake>(wake);
			for (const auto& [key, value] : item.entries)
			{
				std::int64_t number = 0;
				if (key == "on")
					continue;
				if (subscription.wake == Wake::Timer && key == "every_hours" && integerOf(value, number)
					&& number >= 1 && number <= 24 * 365)
					subscription.every = number;
				else if (subscription.wake == Wake::Timer && key == "phase_hours" && integerOf(value, number)
					&& number >= 0)
					subscription.phase = number;
				else if (subscription.wake == Wake::RelationCrossed && key == "threshold"
					&& integerOf(value, number) && (number == -30 || number == 50))
					subscription.threshold = static_cast<std::int32_t>(number);
				else if (subscription.wake == Wake::RelationCrossed && key == "direction"
					&& value.kind == ScriptValue::Kind::String && (value.text == "down" || value.text == "up"))
					subscription.direction = value.text == "down" ? -1 : 1;
				else
				{
					error = "subscription '" + on->text + "': bad or unknown field '" + key + "'";
					return false;
				}
			}
			if (subscription.phase >= subscription.every)
			{
				error = "timer phase_hours must be below every_hours";
				return false;
			}
			subscriptions_.push_back(subscription);
		}
	}
	if (const ScriptValue* list = manifest.find("fields"))
	{
		const std::vector<ScriptValue>* items = listOf(*list);
		if (items == nullptr || items->size() > kMaxFields)
		{
			error = "'fields' must be a list of at most 16";
			return false;
		}
		for (const ScriptValue& item : *items)
		{
			const ScriptValue* name = item.kind == ScriptValue::Kind::Map ? item.find("name") : nullptr;
			const ScriptValue* type = item.kind == ScriptValue::Kind::Map ? item.find("type") : nullptr;
			if (name == nullptr || type == nullptr || name->kind != ScriptValue::Kind::String
				|| type->kind != ScriptValue::Kind::String || !validName(name->text))
			{
				error = "a field needs a lower-case 'name' and a 'type' (number, integer, boolean)";
				return false;
			}
			Field field;
			field.name = name->text;
			const ScriptValue* fallback = item.find("default");
			if (type->text == "number")
			{
				field.type = state::ScriptFieldType::Number;
				if (fallback != nullptr && (!fallback->isNumber() || !std::isfinite(fallback->asNumber())))
				{
					error = "field '" + field.name + "': default must be a number";
					return false;
				}
				field.number = fallback != nullptr ? fallback->asNumber() : 0.0;
			}
			else if (type->text == "integer")
			{
				field.type = state::ScriptFieldType::Integer;
				if (fallback != nullptr && fallback->kind != ScriptValue::Kind::Int)
				{
					error = "field '" + field.name + "': default must be an integer";
					return false;
				}
				field.integer = fallback != nullptr ? fallback->integer : 0;
			}
			else if (type->text == "boolean")
			{
				field.type = state::ScriptFieldType::Boolean;
				if (fallback != nullptr && fallback->kind != ScriptValue::Kind::Bool)
				{
					error = "field '" + field.name + "': default must be a boolean";
					return false;
				}
				field.integer = fallback != nullptr && fallback->boolean ? 1 : 0;
			}
			else
			{
				error = "field '" + field.name + "': unknown type '" + type->text + "'";
				return false;
			}
			if (std::any_of(fields_.begin(), fields_.end(), [&](const Field& other) { return other.name == field.name; }))
			{
				error = "field '" + field.name + "' declared twice";
				return false;
			}
			fields_.push_back(field);
		}
		std::sort(fields_.begin(), fields_.end(),
			[](const Field& left, const Field& right) { return left.name < right.name; });
	}
	return true;
}

// The faction's saved fields match the declaration: kept by name and type, new ones
// at their default, ones no longer declared dropped.
void FactionScript::syncFields(state::FactionAI& ai) const
{
	std::vector<state::ScriptField> wanted;
	wanted.reserve(fields_.size());
	for (const Field& field : fields_)
	{
		const auto at = std::find_if(ai.script.begin(), ai.script.end(),
			[&](const state::ScriptField& saved) { return saved.name == field.name && saved.type == field.type; });
		if (at != ai.script.end())
			wanted.push_back(*at);
		else
			wanted.push_back(state::ScriptField{field.name, field.type, field.integer, field.number});
	}
	if (wanted != ai.script)
		ai.script = std::move(wanted);
}

bool FactionScript::matches(const Subscription& subscription, const state::FactionEvent& event) const
{
	switch (subscription.wake)
	{
	case Wake::Timer:
		return false;
	case Wake::SquadLost:
		return event.kind == state::EventKind::SquadLost;
	case Wake::TownLost:
		return event.kind == state::EventKind::TownLost;
	case Wake::TownGained:
		return event.kind == state::EventKind::TownGained;
	case Wake::RaidOutcome:
		return event.kind == state::EventKind::RaidOutcome;
	case Wake::RelationCrossed:
	{
		if (event.kind != state::EventKind::RelationCrossed)
			return false;
		const auto crossing = static_cast<state::RelationCrossing>(event.code);
		const bool hostile = crossing == state::RelationCrossing::BecameHostile
			|| crossing == state::RelationCrossing::NoLongerHostile;
		const bool down = crossing == state::RelationCrossing::BecameHostile
			|| crossing == state::RelationCrossing::NoLongerAlly;
		return (subscription.threshold == 0 || subscription.threshold == (hostile ? -30 : 50))
			&& (subscription.direction == 0 || subscription.direction == (down ? -1 : 1));
	}
	}
	return false;
}

FactionScript::Result FactionScript::fail(const FactionContext& context, FactionStats& stats,
	ScriptOutcome outcome, const std::string& message)
{
	++stats.scriptFallbacks;
	switch (outcome)
	{
	case ScriptOutcome::Error: ++stats.scriptErrors; break;
	case ScriptOutcome::Budget: ++stats.scriptBudget; break;
	case ScriptOutcome::Memory: ++stats.scriptMemory; break;
	case ScriptOutcome::Missing: ++stats.scriptMissing; break;
	default: ++stats.scriptUnusable; break;
	}
	++failures_;
	if (failures_ <= 5 || failures_ % 1000 == 0)
	{
		say("faction script '" + setup_.name + "' function 'decide' faction '"
			+ context.faction.templateSid + "' tick " + std::to_string(context.tick) + ": "
			+ std::string(scriptOutcomeName(outcome)) + ": " + message
			+ " (failure " + std::to_string(failures_) + "; the native scoring decides)");
	}
	return Result::Fallback;
}

FactionScript::Result FactionScript::decide(FactionContext& context, const FactionInfo* info,
	state::FactionAI& ai, bool canRaid, FactionStats& stats)
{
	if (handle_ < 0)
		return Result::Fallback;
	const std::uint64_t hour = context.tick / kTicksPerGameHour;
	ScriptValue fired = ScriptValue::array();
	const auto mark = [&](Wake wake)
	{
		const std::string name(wakeName(static_cast<int>(wake)));
		for (const ScriptValue& existing : fired.items)
		{
			if (existing.text == name)
				return;
		}
		fired.push(ScriptValue::ofString(name));
	};
	for (const Subscription& subscription : subscriptions_)
	{
		if (subscription.wake == Wake::Timer
			&& hour % static_cast<std::uint64_t>(subscription.every) == static_cast<std::uint64_t>(subscription.phase))
			mark(Wake::Timer);
	}
	ScriptValue events = ScriptValue::array();
	for (const state::FactionEvent& event : context.events)
	{
		if (event.faction.id != context.entity.id() || events.items.size() >= kMaxEventsPerCall)
			continue;
		bool wanted = false;
		for (const Subscription& subscription : subscriptions_)
		{
			if (matches(subscription, event))
			{
				wanted = true;
				mark(subscription.wake);
			}
		}
		if (!wanted)
			continue;
		ScriptValue entry = ScriptValue::map();
		entry.set("on", ScriptValue::ofString(std::string(wakeName(static_cast<int>(
			event.kind == state::EventKind::SquadLost ? Wake::SquadLost
			: event.kind == state::EventKind::TownLost ? Wake::TownLost
			: event.kind == state::EventKind::TownGained ? Wake::TownGained
			: event.kind == state::EventKind::RaidOutcome ? Wake::RaidOutcome : Wake::RelationCrossed)))));
		entry.set("tick", ScriptValue::ofInt(static_cast<std::int64_t>(event.tick)));
		if (event.town.id != 0)
			entry.set("town", ScriptValue::ofInt(static_cast<std::int64_t>(idOf(context.ecs, event.town.id))));
		if (event.kind == state::EventKind::RaidOutcome)
		{
			const auto result = static_cast<state::RaidResult>(event.code);
			entry.set("result", ScriptValue::ofString(result == state::RaidResult::Victory ? "victory"
				: result == state::RaidResult::Defeated ? "defeated" : "loss"));
		}
		if (event.kind == state::EventKind::RelationCrossed)
		{
			const auto crossing = static_cast<state::RelationCrossing>(event.code);
			const bool hostile = crossing == state::RelationCrossing::BecameHostile
				|| crossing == state::RelationCrossing::NoLongerHostile;
			const bool down = crossing == state::RelationCrossing::BecameHostile
				|| crossing == state::RelationCrossing::NoLongerAlly;
			entry.set("other", ScriptValue::ofInt(static_cast<std::int64_t>(idOf(context.ecs, event.other.id))));
			entry.set("threshold", ScriptValue::ofInt(hostile ? -30 : 50));
			entry.set("direction", ScriptValue::ofString(down ? "down" : "up"));
		}
		events.push(std::move(entry));
	}
	if (fired.items.empty())
	{
		++stats.scriptSleeping;
		return Result::Sleeping;
	}
	stats.scriptEvents += events.items.size();
	syncFields(ai);

	ScriptValue view = ScriptValue::map();
	view.set("api", ScriptValue::ofInt(0));
	{
		ScriptValue faction = ScriptValue::map();
		faction.set("sid", ScriptValue::ofString(context.faction.templateSid));
		faction.set("name", ScriptValue::ofString(info != nullptr ? info->name : std::string()));
		faction.set("id", ScriptValue::ofInt(static_cast<std::int64_t>(context.id)));
		view.set("faction", std::move(faction));
	}
	view.set("tick", ScriptValue::ofInt(static_cast<std::int64_t>(context.tick)));
	view.set("hour", ScriptValue::ofInt(static_cast<std::int64_t>(hour)));
	view.set("fired", std::move(fired));
	view.set("events", std::move(events));
	view.set("intent", ScriptValue::ofString(kIntentNames[static_cast<std::size_t>(ai.intent)]));
	view.set("intent_hours", ScriptValue::ofNumber(gameHoursIn(ticksSince(context.tick, ai.since))));
	view.set("alarm", ScriptValue::ofNumber(ai.alarm));
	view.set("grief", ScriptValue::ofNumber(ai.grief));
	view.set("confidence", ScriptValue::ofNumber(ai.confidence));
	view.set("aggression", ScriptValue::ofNumber(ai.aggression));
	view.set("can_raid", ScriptValue::ofBool(canRaid));
	view.set("raids_running", ScriptValue::ofInt(static_cast<std::int64_t>(ai.raids.size())));
	{
		ScriptValue state = ScriptValue::map();
		for (const state::ScriptField& field : ai.script)
		{
			state.set(field.name, field.type == state::ScriptFieldType::Number ? ScriptValue::ofNumber(field.number)
				: field.type == state::ScriptFieldType::Integer ? ScriptValue::ofInt(field.integer)
				: ScriptValue::ofBool(field.integer != 0));
		}
		view.set("state", std::move(state));
	}

	ScriptValue commands;
	const CallResult call = host_->call(handle_, "decide", view, commands,
		splitMix64(seed_ ^ splitMix64(context.id)) ^ splitMix64(context.tick));
	stats.scriptPeakBytes = std::max(stats.scriptPeakBytes, host_->memoryPeak());
	if (!call.ok())
		return fail(context, stats, call.outcome, call.message);
	std::string error;
	if (!apply(commands, context, ai, canRaid, error))
		return fail(context, stats, ScriptOutcome::Unusable, error);
	++stats.scriptApplied;
	return Result::Applied;
}

// Validates every command, then applies them all (or none).
bool FactionScript::apply(const ScriptValue& commands, FactionContext& context,
	state::FactionAI& ai, bool canRaid, std::string& error) const
{
	if (commands.kind == ScriptValue::Kind::Nil)
		return true; // no commands: leave everything as it is
	if (commands.kind != ScriptValue::Kind::Map)
	{
		error = "decide must return a table of commands or nil";
		return false;
	}
	bool setIntent = false;
	state::Intent intent = ai.intent;
	double temperament[3] = {ai.alarm, ai.grief, ai.confidence};
	std::vector<state::ScriptField> fields = ai.script;
	for (const auto& [command, value] : commands.entries)
	{
		if (command == "set_intent")
		{
			int found = -1;
			for (int i = 0; i < state::kIntentCount; ++i)
				found = value.kind == ScriptValue::Kind::String && value.text == kIntentNames[i] ? i : found;
			if (found < 0)
			{
				error = "set_intent: not one of consolidate, defend, raid, mourn";
				return false;
			}
			if (found == static_cast<int>(state::Intent::Raid) && !canRaid)
			{
				error = "set_intent: raid is not on offer to this faction (can_raid is false)";
				return false;
			}
			intent = static_cast<state::Intent>(found);
			setIntent = true;
		}
		else if (command == "set_temperament")
		{
			if (value.kind != ScriptValue::Kind::Map)
			{
				error = "set_temperament: a table of alarm / grief / confidence";
				return false;
			}
			for (const auto& [name, number] : value.entries)
			{
				const int slot = name == "alarm" ? 0 : name == "grief" ? 1 : name == "confidence" ? 2 : -1;
				if (slot < 0 || !number.isNumber() || !std::isfinite(number.asNumber())
					|| number.asNumber() < 0.0 || number.asNumber() > 1.0)
				{
					error = "set_temperament: '" + name + "' must be alarm, grief or confidence, a number in [0, 1]";
					return false;
				}
				temperament[slot] = number.asNumber();
			}
		}
		else if (command == "set_state")
		{
			if (value.kind != ScriptValue::Kind::Map)
			{
				error = "set_state: a table of declared field values";
				return false;
			}
			for (const auto& [name, entry] : value.entries)
			{
				const auto at = std::find_if(fields.begin(), fields.end(),
					[&](const state::ScriptField& field) { return field.name == name; });
				if (at == fields.end())
				{
					error = "set_state: '" + name + "' is not a declared field";
					return false;
				}
				if (at->type == state::ScriptFieldType::Number && entry.isNumber() && std::isfinite(entry.asNumber()))
					at->number = entry.asNumber();
				else if (at->type == state::ScriptFieldType::Integer && entry.kind == ScriptValue::Kind::Int)
					at->integer = entry.integer;
				else if (at->type == state::ScriptFieldType::Boolean && entry.kind == ScriptValue::Kind::Bool)
					at->integer = entry.boolean ? 1 : 0;
				else
				{
					error = "set_state: '" + name + "' has the wrong type";
					return false;
				}
			}
		}
		else
		{
			error = "unknown command '" + command + "' (set_intent, set_temperament, set_state)";
			return false;
		}
	}
	ai.alarm = temperament[0];
	ai.grief = temperament[1];
	ai.confidence = temperament[2];
	ai.script = std::move(fields);
	if (setIntent && intent != ai.intent)
	{
		ai.intent = intent;
		ai.since = context.tick;
	}
	return true;
}

} // namespace ksim
