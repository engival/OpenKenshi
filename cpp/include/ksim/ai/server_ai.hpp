#pragma once
// The squad AI a server runs, in one place: the behaviour registry, the faction book and the AI's navigator. ksim_server calls it
// and so does the test that drives the server's own run (ksim_spectate_fight), so the two cannot disagree about whether the AI is
// on at all. Until 2026-11-08 the server enabled the AI only when --bake was given, which made the navigator choice decide the AI:
// a server over --navworld / --native-nav / its own Hub bake ran every character goal-less, and the live spectate_fight scenario
// produced no fight (client report on sim 310d5e7a). The AI now runs whenever the server runs; --no-ai is the switch.

#include "ksim/ai/ai.hpp"
#include "ksim/dev/build_paths.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ksim {

class Simulation;

struct ServerAiOptions
{
	std::string registryPath; // KSIM_AI_TABLE: data/ksim_ai_behaviours.tsv
	// --bake DIR: a `knav_bake --world` bake; the AI then streams its own regions from it (KnavNavigator) instead of using the
	// server's navmesh. Empty = the simulation's own navigator (navigationOwner), which may be null.
	std::string bake;
	int regionX0 = 4; // the bake window (--regions)
	int regionZ0 = 7;
	int regionX1 = 6;
	int regionZ1 = 9;
	// The debug view's goal-pick sink (SIMLINK.md "DEBUG VIEW"); empty when the view is off.
	std::function<void(std::uint64_t, const GoalChoice&, const std::vector<GoalChoice>&)> decision;
	bool tokenSpeeds = false; // --token-speeds on: AiSystem::Config::tokenSpeeds (ai.token_speeds, COMBAT_OPTIONS.md 7)
	bool npcRaids = false; // --npc-raids on: FactionTuning::npcRaids (world.npc_raids, FACTIONS.md 4)
	bool victoryAi = false; // --campaign-victory-ai on: AiSystem::Config::victoryAi (world.campaign_victory_ai, COMBAT_OPTIONS.md 10)
};

// Enables the AI the server runs. Returns the navigator it took ("bake", "navmesh", "none"), for the server's log line.
const char* enableServerAi(Simulation& sim, const ServerAiOptions& options);

} // namespace ksim
