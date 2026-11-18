#pragma once

// A gate crew's leader at full detail (GATES_CARRY_LIVE.md a, DOORS.md section 8; the squad-level version is state::GateCrew and
// the AI goal ManGate, spawn.hpp / ai.hpp, with the crew's reopen of doors.hpp).
//
// state::GateDuty on a fighter makes it the man at one gate (a member, phase kGateMemberPhase, only stands at its slot): Simulation::stepGateCrews (once a second) walks it to the gate's
// outside point (door centre + 15 n, GateSite) and keeps it there. It never closes the gate and looks at no alert, enemy, time,
// relation or crime. A gate that is shut (state 1 or 2) fails the task's DOOR_IS_OPEN precondition, so the leader re-plans:
// UNLOCK (kUnlockSeconds, only if its state::Allegiance is the door's faction) then OPEN, then walks back to the post. Re-locking
// is the door's `always locks`, not the crew. A leader in a fight (Fighter::target) or down does nothing: the gate stays as it is.

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
// beyond are off duty: they get no GateDuty and stay as the squad layer leaves them (idle with the squad at the post).
// Derived from the gate geometry and the member index, never saved.
inline constexpr int kGateMemberSlots = 4;
inline constexpr double kGateSlotOut = 33.0;
inline constexpr std::array<double, kGateMemberSlots> kGateSlotAcross = {-34.0, -19.0, 19.0, 34.0};
// GateDuty::phase of a member: it only stands at its slot (no door work), and walks back to it like the leader.
inline constexpr std::uint8_t kGateMemberPhase = 3;

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

// A leader within this xz distance of its post (units) is standing at it.
inline constexpr double kGatePostReach = 3.0;

} // namespace ksim
