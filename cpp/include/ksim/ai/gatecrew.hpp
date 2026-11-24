#pragma once

// A gate crew (GATES_CARRY_LIVE.md a, DOORS.md section 8): the squad-level state::GateCrew (the post and its binding, stepGateBinding)
// and MAN_THE_GATE 180, which the squad layer takes from the squad's package row (ai.cpp candidates(): goalsel over the 180 row, the
// goap row's utility) and holds at the gate's outside point (door centre + 15 n, GateSite). The old full-detail GateDuty system
// (stepGateCrews) is gone: nothing in production set it. The component is kept in the save list, unused (removing it is a save bump).

#include "ksim/world/world.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ksim {

// The crew looks at its gate this often (ticks): once a game-second. The measured "~600 frame" reopen is plan latency; this adds
// at most one second of it. UNSURE.
inline constexpr std::uint64_t kGateCrewIntervalTicks = 20;
// The members (task 51 STAND_AT_NODE, GATE_CREWS_LIVE.md section 2): outside + 33 u at v = -34, -19, +19, +34 along the door axis,
// u = n (the outside direction), v = (-nz, nx). Filled in that order. UNSURE: the order, and the sign of v (the exe's door axis
// orientation was not read; the slots are symmetric, so only which member takes which side is affected). A fifth member and
// beyond are off duty (rwh.207): a Routine, so the town scorer gives them the package's 38 / 157 (stepGateCrewPosts).
// Derived from the gate geometry and the member index, never saved.
inline constexpr int kGateMemberSlots = 4;
inline constexpr double kGateSlotOut = 33.0;
inline constexpr std::array<double, kGateMemberSlots> kGateSlotAcross = {-34.0, -19.0, 19.0, 34.0};

struct GateStand
{
	double x = 0.0;
	double z = 0.0;
};

// The stand point of the crew's member `index`: 0 is the leader (the outside point), 1..4 the guards at the slots in order.
// Nothing for index 5 and up (off duty).
inline std::optional<GateStand> gateStandPoint(const GateSite& gate, int index)
{
	if (index == 0)
		return GateStand{gate.outX, gate.outZ};
	if (index < 0 || index > kGateMemberSlots)
		return std::nullopt;
	const double across = kGateSlotAcross[static_cast<std::size_t>(index - 1)];
	return GateStand{gate.outX + kGateSlotOut * gate.nx - across * gate.nz, gate.outZ + kGateSlotOut * gate.nz + across * gate.nx};
}

// The posts change hands this often (ticks, 2.5 game minutes). UNSURE: rwh.207 s3 gives "every few game minutes" (goal min-duration 0.1 + rand
// x 0.5 h for 38), not a period; ours is fixed.
inline constexpr std::uint64_t kGateRotationTicks = 3000;

// A leader within this xz distance of its post (units) is standing at it.
inline constexpr double kGatePostReach = 3.0;

} // namespace ksim
