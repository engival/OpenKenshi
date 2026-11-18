// Ground-blood events (SIMLINK events 2, 16, 17; docs/formats/SIMLINK.md, kenshi-inspect findings/bleed_pool.md).
// Presentation-derived state: the trackers live in the simlink layer, are derived each tick from the
// mirror, are not saved and are not hashed. Nothing here reads a clock or std::rand.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ksim {

// What the tracker needs of one character, every tick it is in view.
struct BloodInput
{
	std::uint64_t id = 0;
	double x = 0, y = 0, z = 0;
	double rate = 0; // blood lost per second (med+0x78)
	bool dead = false;
	bool unconscious = false;
	bool down = false; // ragdoll (a leg or torso part collapsed)
	bool carried = false;
};

struct BloodEvent
{
	std::uint16_t kind = 0; // 2 drop (value = rate), 16 pool (value = g), 17 smear (value = alpha)
	std::uint64_t id = 0;
	double pos[3] = {};
	std::uint8_t dir = 0; // smear: direction of travel, encoded as event 1's dir
	double value = 0;
};

class BloodTrail
{
public:
	static constexpr std::uint64_t kMedicalTicks = 20; // the exe's medical update: ~1 s
	static constexpr double kFrameDt = 1.0 / 54.0; // the exe's per-call dt is one frame at ~54 fps (frame-rate dependent there)
	static constexpr double kPoolCap = 2.0;
	// Pool growth cadence: the exe grows a pool one step per medical update, one update per 5 frames with the camera near the
	// character and one per 50 far away (BLOOD_DECALS.md "Pool growth cadence", MEDICAL_HUNGER_LIVE.md (b2)). We match what the
	// player sees, the near cadence: 54 / 5 = 10.8 steps per second (the far one, 1 per second, was our old kMedicalTicks cadence).
	static constexpr double kPoolStepsPerSecond = 54.0 / 5.0;
	// Event 16 is sent only after g grew by this much since the last one sent (growth itself is not limited): ~3 per second at the
	// start of a 0.35 bleed, fewer as the pool slows (SIMLINK.md event 16).
	static constexpr double kPoolSendDelta = 0.005;

	// Call once per tick for every character in view; events of a medical tick are appended to out.
	void observe(const BloodInput& c, std::uint64_t tick, std::vector<BloodEvent>& out);
	void forget(std::uint64_t id) { state_.erase(id); }

private:
	struct State
	{
		double lastX = 0, lastZ = 0; // position at the previous tick
		bool poolOn = false;
		double poolX = 0, poolY = 0, poolZ = 0, g = 0;
		double sentG = 0; // g of the last event 16 sent
		double smearX = 0, smearZ = 0; // where the last smear was laid
		double dropX = 0, dropZ = 0; // where the last drop fell
		double counter = 0; // drop counter
	};
	std::unordered_map<std::uint64_t, State> state_;
};

} // namespace ksim
