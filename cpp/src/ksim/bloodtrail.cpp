#include "ksim/bloodtrail.hpp"

#include <algorithm>
#include <cmath>

#include "ksim/clock.hpp"

namespace ksim {

namespace {

std::uint8_t dirCode(double dx, double dz)
{
	const double turns = std::atan2(dz, dx) / (2.0 * 3.14159265358979323846);
	return static_cast<std::uint8_t>(static_cast<long>(std::lround(turns * 256.0)) & 0xff);
}

// Deterministic roll in [0, 1): splitmix64 of (id, tick).
double roll(std::uint64_t id, std::uint64_t tick)
{
	std::uint64_t z = id * 0x9e3779b97f4a7c15ull + tick * 0xbf58476d1ce4e5b9ull + 0x94d049bb133111ebull;
	z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
	z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
	z ^= z >> 31;
	return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
}

} // namespace

void BloodTrail::observe(const BloodInput& c, std::uint64_t tick, std::vector<BloodEvent>& out)
{
	auto it = state_.find(c.id);
	const bool fresh = it == state_.end();
	State& s = fresh ? (it = state_.emplace(c.id, State{}).first)->second : it->second;
	if (fresh)
	{
		s.lastX = s.smearX = s.dropX = c.x;
		s.lastZ = s.smearZ = s.dropZ = c.z;
	}
	const double stepSq = (c.x - s.lastX) * (c.x - s.lastX) + (c.z - s.lastZ) * (c.z - s.lastZ);
	s.lastX = c.x;
	s.lastZ = c.z;
	// The gate: rate > 0; a dead character's update skips bleeding, so its rate and pool freeze.
	if (c.rate <= 0.0 || c.dead)
		return;
	const bool lying = c.unconscious || c.down; // FUN_14028ddc0
	if (stepSq < 1e-6 && lying)
	{
		// Branch 1: the growing pool, every tick at the NEAR-camera cadence (kPoolStepsPerSecond), not the medical tick.
		const double fromPool = (c.x - s.poolX) * (c.x - s.poolX) + (c.z - s.poolZ) * (c.z - s.poolZ);
		if (!s.poolOn || fromPool > 10.0)
		{
			s.poolOn = true;
			s.poolX = c.x;
			s.poolY = c.y;
			s.poolZ = c.z;
			s.g = s.sentG = 0.0;
		}
		// One exe step is 0.2 x rate x one frame / max(1, 10 g); a tick is kPoolStepsPerSecond x kTickSeconds of them.
		s.g = std::min(BloodTrail::kPoolCap, s.g + kPoolStepsPerSecond * kTickSeconds * 0.2 * c.rate * kFrameDt / std::max(1.0, 10.0 * s.g));
		// Only the sending is rate limited (event volume): a change of kPoolSendDelta, or reaching the cap.
		if (s.g - s.sentG >= BloodTrail::kPoolSendDelta || (s.g == BloodTrail::kPoolCap && s.sentG < s.g))
		{
			s.sentG = s.g;
			BloodEvent e;
			e.kind = 16;
			e.id = c.id;
			e.pos[0] = s.poolX;
			e.pos[1] = s.poolY;
			e.pos[2] = s.poolZ;
			e.value = s.g;
			out.push_back(e);
		}
		return;
	}
	// The smear and drop branches stay on the medical tick (~1 s). The drop accumulator is per medical update in the exe too,
	// so its cadence is also camera dependent there; it keeps the 1 s tick here (only the pool was asked to follow the near cadence).
	if ((tick + c.id) % kMedicalTicks != 0)
		return;
	if (lying && !c.carried && (c.x - s.smearX) * (c.x - s.smearX) + (c.z - s.smearZ) * (c.z - s.smearZ) > 16.0)
	{
		// Branch 2: a smear along the way.
		BloodEvent e;
		e.kind = 17;
		e.id = c.id;
		e.pos[0] = c.x;
		e.pos[1] = c.y;
		e.pos[2] = c.z;
		e.dir = dirCode(c.x - s.smearX, c.z - s.smearZ);
		e.value = std::clamp(c.rate, 0.0, 1.0);
		out.push_back(e);
		s.smearX = c.x;
		s.smearZ = c.z;
		return;
	}
	if (c.rate > 0.01 && stepSq >= 1e-6)
	{
		// Branch 3: drops, only while the character is going somewhere (standing still sends nothing).
		s.counter += roll(c.id, tick) * c.rate * (kMedicalTicks * 0.05) * 10.0;
		if (s.counter > 1.0 && (c.x - s.dropX) * (c.x - s.dropX) + (c.z - s.dropZ) * (c.z - s.dropZ) > 1.0)
		{
			BloodEvent e;
			e.kind = 2;
			e.id = c.id;
			e.pos[0] = c.x;
			e.pos[1] = c.y;
			e.pos[2] = c.z;
			e.value = c.rate;
			out.push_back(e);
			s.counter = 0.0;
			s.dropX = c.x;
			s.dropZ = c.z;
		}
	}
}

} // namespace ksim
