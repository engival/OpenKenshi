#include "ksim/server_ai.hpp"

#include "ksim/behaviours.hpp"
#include "ksim/factions.hpp"
#include "ksim/navigator.hpp"
#include "ksim/simulation.hpp"

#include <memory>
#include <utility>

namespace ksim {

const char* enableServerAi(Simulation& sim, const ServerAiOptions& options)
{
	AiSystem::Config ai;
	ai.registry = std::make_shared<BehaviourRegistry>(sim.world(), BehaviourRegistry::readRules(options.registryPath));
	ai.factions = FactionBook::build(sim.gameData());
	ai.decision = options.decision;
	ai.tokenSpeeds = options.tokenSpeeds;
	ai.victoryAi = options.victoryAi;
	ai.factionTuning.npcRaids = options.npcRaids;
	const char* navigator = "none";
	if (!options.bake.empty())
	{
		// The world bake's own streaming navigator: the AI's routes then come from the coarse graph over every region, whatever
		// the server's navmesh covers (the Hub bake, --navworld, --native-nav choose the characters' mesh, not the AI's).
		ai.navigator = KnavNavigator::open(sim.world().dataDir(), options.bake,
			options.regionX0, options.regionZ0, options.regionX1, options.regionZ1, 4);
		navigator = "bake";
	} else if (const std::shared_ptr<const Navigator> mesh = sim.navigationOwner()) {
		ai.navigator = std::move(mesh);
		navigator = "navmesh";
	}
	// No navigator at all is allowed (AiSystem::Config): the goals still pick and a member with a goal and no route stands; a
	// character never reaches a terminal state.
	sim.enableAi(std::move(ai));
	return navigator;
}

} // namespace ksim
