#pragma once

// Level of detail (ARCHITECTURE.md "Simulation" table). Scheduling
// bookkeeping shared by the tick drivers; neither template nor saved state.

#include <cstdint>

namespace ksim {

// The detail level is a component value, never a tag: entities keep their
// archetype when they are promoted/demoted.
enum class DetailLevel : std::int32_t
{
	Full = 0,
	Squad = 1,
	Strategic = 2,
};

inline constexpr int kDetailLevelCount = 3;

struct Detail
{
	DetailLevel level = DetailLevel::Strategic;
};

} // namespace ksim
