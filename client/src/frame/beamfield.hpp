#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "render/camera.hpp"
#include "render/particles.hpp"
#include "net/simlink.hpp"

namespace kworld { class Heightmap; }

// Venge's sky beams drawn from the sim's schedule (SIMLINK "BEAMS", docs/reference/VENGE_BEAMS_STATIC.md, kenshi-rwh.48): the sim owns the
// burning wandering weather effects (EFFECT type 9), plans each a few seconds ahead and sends its exact path. This class keeps those beams and
// hands the drawn ones to Particles at the path point of the render tick, y snapped to the terrain under it (the client's snap, SIMLINK
// "BEAMS"), as the EFFECT's particle system (SkyBeam.pu: a 200 x 16 000 u additive strip, the base flame, the ground blast and the mist).
// A beam plays as an event (no pre-warm: the script's own colour curve is the strike and a pre-warm would eat its one-shot emitter). The
// advance notice is the wind-up: the beam is registered before it strikes so it appears exactly at its spawnTick, and nothing is drawn
// during the lead (the original has no pre-strike visual). The client drops a beam at endTick; the particle system it started is kept a
// little longer at that last point, because in the original the psys outlives the handler's damage window (rwh.48 4.4).
// A beam is SEEN only over its burst [spawnTick, spawnTick + visibleTicks) (BEAMS visibleTicks, kenshi-h2v.14.12; 0 = its whole life): at the
// burst's end the system stops emitting and its particles die out (the tail runs from there); the decoder keeps the beam to endTick for damage.
class BeamField
{
public:
	// Sim ticks the particle system can still live after a beam's end. The decoder uses the same value to drop an ended beam (simlink::kBeamParticleTailTicks).
	static constexpr uint64_t kParticleTailTicks = simlink::kBeamParticleTailTicks;

	void clear();
	// Copies the decoder's beams when its BEAMS serial moved (a BEAMS frame is at most a few ticks old). Returns true when it changed.
	bool sync(const simlink::SimLinkDecoder& d);
	// The placed systems of the beams drawn at `tick` (the render tick), appended to `out`. Nothing before a beam's spawnTick. The sim
	// runs every beam; only the `maxBeams` nearest the camera `cam` whose own zone cell is loaded (cellLoaded, the original's gate) are drawn
	// (render.venge_beams_max, kenshi-k16.2.409.1; 0 = none). `players` are the player-faction characters' positions (the gate's per-character
	// cells); `gate` false bypasses the cell test (a KCLIENT_DEV_TOOLS A/B, the pre-409.3 look). A beam in the set stays until it is
	// kHysteresis x farther than the X-th; one that joins or leaves fades over kFadeTicks (PlacedSystem::fade) instead of popping. Cost is
	// the particle count `dump beams` / the HUD report.
	void systems(double tick, const kworld::Heightmap* hm, const Camera& cam, const std::vector<std::array<double, 3>>& players, int maxBeams, bool gate, std::vector<Particles::PlacedSystem>& out);
	static constexpr double kHysteresis = 1.1;
	static constexpr double kFadeTicks = 20.0;	// one second
	// The last systems() call: beams on the wire, in their burst, in the drawn set, fading out of it, of the drawn set in the view (its column
	// against the camera frustum); the gate's focus cell and the number of loaded cells (a log line per second reads it).
	struct Stats { size_t wire = 0, burst = 0, drawn = 0, inview = 0, fading = 0, dup = 0, stale = 0, burstNear = 0, drawnNear = 0, cells = 0, burstView = 0, burstOpenView = 0, newView = 0; double drawnFar = 0, drawnNearest = -1; int focusX = -1, focusZ = -1; };
	// keys of the held beams (for counting their particle tails)
	std::vector<uint64_t> keys() const { std::vector<uint64_t> k; for (const Beam& e : beams_) { k.push_back(e.key); } return k; }
	// KBEAMSLOG=2: one line per frame for each beam in a join or leave fade
	void fadeLog(double tick, long frame) const;
	const Stats& stats() const { return stats_; }
	size_t newViewTotal() const { return viewSeen_.size(); }	// distinct beams that have ever been drawn in the view (KBEAMSLOG rate)
	// The particle scripts the sim is drawing: the weather's own local placement of them must not run as well (the sim's schedule is the truth).
	std::vector<std::string> scripts() const;
	std::string dump(double tick) const;	// a line per beam (id, group, spawn/end, the ground point at `tick`, scripts) (stdout)
	size_t live(double tick) const;		// spawnTick <= tick < endTick
	// One row per beam held: its particle-system key (what systems() places, so a placed system pairs with its beam), the state at
	// `tick` and the ground point systems() places it at. `dump beamfeet` reads it for the foot-fire check (kenshi-h2v.14.9.1).
	struct Foot
	{
		uint64_t key = 0, spawnTick = 0, endTick = 0;
		uint32_t id = 0;
		int state = 0;		// 1 burst, 2 tail (the particles after the burst's end; the wind-up is not placed, so it is not a foot)
		std::string particles;
		float x = 0, z = 0;
	};
	std::vector<Foot> feet(double tick) const;
	struct Aim
	{
		uint32_t id = 0;
		uint64_t spawnTick = 0, endTick = 0;
	};
	// The camera op (`camera beam [id=N] [windup|live|same]`): the named beam else the next to strike else the live one nearest the camera,
	// restricted to the beams still in their lead (`phase` 1) or already striking (`phase` 2), the beam the last call picked (`phase` 3:
	// the shot pair watches one beam from its wind-up into its strike), any (`phase` 0); its ground point at `tick` (the spawn point while
	// it is in the lead) and which beam it was. False when there is no such beam.
	bool aim(double tick, uint32_t id, int phase, const double cam[3], float& x, float& z, Aim* out = nullptr);

private:
	struct Beam
	{
		uint64_t key = 0;
		std::string group, particles;
		simlink::Beam b;
	};
	struct State { float fade = 0; double last = 0; bool in = false; };
	// The original's loaded zone-cell set (kenshi-rwh.211; the weather gate 140100ba0): the 3x3 cells round the focus cell -- the camera's cell,
	// re-centred only past 0.9 cell (4147.2 u) from the focus cell's centre per axis, the original's hysteresis -- plus every cell touched by a
	// +-340 u box round each player-faction character (the "Fast zone hopping" radius is 0 in the default config, so just the touched cells).
	// x * 64 + z for each loaded cell, rebuilt by cells() on every systems() call.
	void cells(const double cam[3], const std::vector<std::array<double, 3>>& players);
	// The draw gate: true when the zone cell under (x, z) is in the loaded set. The one predicate the beam gate is (140100ba0, rwh.211); the
	// cell's biome group == the handler's owner group half of that gate is already the sim's population rule (a beam starts in a random zone
	// cell of its own group, docs/reference/VENGE_BEAMS_STATIC.md), so nothing more is tested here.
	bool cellLoaded(double x, double z) const;
	std::unordered_map<uint64_t, State> state_;	// per beam in its burst: the drawn-set membership and its fade
	Stats stats_;
	std::vector<Beam> beams_;
	int focusX_ = -1, focusZ_ = -1;	// the focus cell of the loaded set (per axis; -1 = not set yet)
	std::vector<int> loaded_;		// the loaded zone cells of this frame (x * 64 + z)
	uint32_t serial_ = 0;
	std::unordered_set<uint64_t> viewSeen_;
	uint32_t lastAim_ = 0;	// the beam the last aim() picked (`camera beam same`)
};
