// DEV ONLY (include/ksim/dev/arena_strides.hpp).
#include "ksim/dev/arena_strides.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ksim::dev {

void applyArenaStrides(CombatSettings& settings)
{
	const char* text = std::getenv("KSIM_ARENA_STRIDES");
	int fight = 0;
	int idle = 0;
	if (text && std::sscanf(text, "%d/%d", &fight, &idle) == 2)
	{
		settings.fightStride = std::max(1, fight);
		settings.idleStride = std::max(1, idle);
	}
}

bool pathWalk()
{
	static const bool on = []{ const char* t = std::getenv("KSIM_PATH_WALK"); return t == nullptr || (t[0] != '\0' && t[0] != '0'); }();
	return on;
}

} // namespace ksim::dev
