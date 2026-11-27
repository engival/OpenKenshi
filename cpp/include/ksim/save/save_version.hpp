// The save version and the gates that must move with it (state.hpp cannot include save.hpp: command.hpp needs state.hpp).
#pragma once

#include <cstdint>

namespace ksim {

// 136: CrimeRecord::announced (task ds/k16.6.457): the act's EVENT 51 went out, so a seen-but-uncommitted act is not sent twice across a load; an older save derives it from `committed`.
// The lead assigns the final number at the merge: change it here and kSaveVersion follows.
inline constexpr std::uint32_t kCrimeAnnouncedVersion = 136;

} // namespace ksim
