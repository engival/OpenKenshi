#pragma once

// JSON snapshot of the squad instances, for viewers and diffing (hand-written
// JSON, no library). One object:
//
//   {"clock": {"tick", "day", "hour", "minute"},
//    "squads": [{"id", "sid", "name", "source", "faction", "town", "x", "z",
//                "members": [{"id", "sid"}, ...]}, ...]}
//
// Squads in Id order, members in Id order. faction is the FACTION sid, town
// the root TOWN sid, x/z the ground position; each null when absent. name
// comes from the template (nameOf), "" when unknown.

#include <flecs.h>

#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>

namespace ksim {

void writeSquadSnapshotJson(std::ostream& out, const flecs::world& ecs,
	const std::function<std::string(std::string_view sid)>& nameOf);

} // namespace ksim
