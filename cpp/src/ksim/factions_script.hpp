#pragma once

// The faction-intent script of the Faction layer (private to the layer; the API a
// script sees is docs/reference/LUA_API.md). Loads the script once, reads its
// manifest (subscriptions, declared fields), and per faction step wakes it only if a
// subscription fired: it builds the read-only view, calls `decide`, validates the
// commands it returns and applies them. Anything wrong (error, budget, memory, an
// unusable or refused result) is a Fallback: the caller runs the native scoring from
// the unchanged state, so a broken script can never leave a faction without a decision.

#include "ksim/factions.hpp"
#include "ksim/script.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ksim {

class FactionScript
{
public:
	enum class Result
	{
		Applied, // woken, its commands applied
		Sleeping, // no subscription fired: nothing to decide this step
		Fallback, // not usable this step: run the native scoring
	};

	FactionScript(FactionScriptSetup setup, std::uint64_t seed);

	// The script loaded and its manifest is valid.
	bool loaded() const noexcept { return handle_ >= 0; }

	Result decide(FactionContext& context, const FactionInfo* info, state::FactionAI& ai,
		bool canRaid, FactionStats& stats);

private:
	enum class Wake : std::uint8_t
	{
		Timer,
		SquadLost,
		TownLost,
		TownGained,
		RaidOutcome,
		RelationCrossed,
	};

	struct Subscription
	{
		Wake wake = Wake::Timer;
		std::int64_t every = 1; // Timer: game-hours
		std::int64_t phase = 0;
		std::int32_t threshold = 0; // RelationCrossed: -30 or 50; 0 = either
		std::int32_t direction = 0; // RelationCrossed: -1 down, +1 up, 0 = either
	};

	struct Field
	{
		std::string name;
		state::ScriptFieldType type = state::ScriptFieldType::Number;
		std::int64_t integer = 0;
		double number = 0.0;
	};

	bool parseManifest(const ScriptValue& manifest, const std::vector<std::string>& functions,
		std::string& error);
	void syncFields(state::FactionAI& ai) const;
	bool matches(const Subscription& subscription, const state::FactionEvent& event) const;
	bool apply(const ScriptValue& commands, FactionContext& context, state::FactionAI& ai,
		bool canRaid, std::string& error) const;
	Result fail(const FactionContext& context, FactionStats& stats, ScriptOutcome outcome,
		const std::string& message);
	void say(const std::string& message);

	FactionScriptSetup setup_;
	std::uint64_t seed_;
	std::unique_ptr<ScriptHost> host_;
	int handle_ = -1;
	std::vector<Subscription> subscriptions_;
	std::vector<Field> fields_; // declared, ascending by name
	std::uint64_t failures_ = 0;
};

} // namespace ksim
