// DEV ONLY: KSIM_ARENA_STRIDES=<fight>/<idle> (e.g. 1/2) overrides CombatSettings::fightStride / idleStride of every CombatRules built in the
// process, so a scenario golden can run at the production arena strides (kenshi-k16.6.658). Unset = the settings as given.
#pragma once

#include "ksim/combat/combat_data.hpp"

namespace ksim::dev {

void applyArenaStrides(CombatSettings& settings);
// KSIM_PATH_WALK=1: walkOrder walks the corridor (PATH_WALK.md 3b/3c, kenshi-e9s9.5). Default off; read once.
bool pathWalk();

} // namespace ksim::dev
