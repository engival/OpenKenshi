#include "ksim/join_rules.hpp"

namespace ksim::joins {

std::uint16_t bitOfTask(std::uint16_t task) noexcept
{
	switch (task)
	{
	case 16:
		return kAttackEnemies;
	case 21:
		return kAttackNeutrals;
	case 15:
		return kProtectAllies;
	case 34:
		return kTroubleMakers;
	case 143:
		return kProtectSquad;
	case 144:
		return kTerritorial;
	case 41:
		return kMasterEnemies;
	case 101:
		return kDefeatSquad;
	case 248:
		return kDefeatSquadChase;
	default:
		return 0;
	}
}

} // namespace ksim::joins
