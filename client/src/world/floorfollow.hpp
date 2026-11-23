#pragma once
#include <cstdint>

// The floor view's one model (kenshi-k16.2.236.9, fable review section 3; migration step 2, kenshi-k16.2.236.11). The client used to
// show a level written by five edge-triggered rules (WorldBuildings::updateFocus: a snap on engage, an auto threshold, a stair
// hotspot, R / F and a load); each fired once on an event and a missed or misread event was never corrected. This is its
// replacement, pure: no GL, no mesh loading, no globals. Each frame the caller hands a sample (the followed character's wire state,
// its sim position and the flights of the building that position is measured in) and the model returns the level to show. The
// events arrive as calls (R / F, engage, load) or as transitions of the samples (a subject change, the auto pair moving, the follow
// ending).
//
// Shown level while following = the subject's resolved floor, every frame (L3). Not following: the player's level alone (F2: it changes
// only when the player steps it). No rule fires once and is then trusted: a wrong frame of input is corrected by the next one.
// There is no manual level while following (user 2026-11-22, kenshi-k16.2.236.13 L4/L5): a floor step ends the follow, so the caller
// (host floor_step) drops the camera lock first and step() sets the player's level; the next sample is not following.
//
// Events:
//   R / F / the diamond                                                     player's level = the stepped level (the caller has ended follow)
//   subject changes                                                         reset the stair latch
//   follow ends or the subject is lost                                      player's level = the last shown level; no write after
//   load                                                                    player's level = saved
//
// The auto floor (the subject's building and floor), in a fixed order, from the subject's sim position only:
//   1. on a flight of the sample's building: the stair latch (the building is that one, whatever the wire says)
//   2. on a roof deck (the wire's indoors bit clear and the client's roof test names a building): that building's top floor
//   3. indoors on the wire: (building, wire floor)
//   4. outdoors: (none, 0)
//
// The stair latch (review section 3): "on a flight" means the feet are inside the flight's footprint and within about 1.5 rises of
// the flight's own slope (the height interpolated from the foot tread to the top tread along the run). That one test replaces the old
// footprint test, zone rect and height band: a character under the stairwell or on the deck beside it is not on the flight. When the
// subject boards a flight, origin = the floor it was settled on (first seen mid-flight: the wire floor). With top = the top tread's
// height and rise the measured riser: feet above top - 1 rise are on the upper floor, feet below top - 2 rises on the lower floor,
// and between the two the floor that is not origin (the one it is heading to). So a climb flips at 2 rises below the top, a descent
// at 1 rise below the top, and turning round mid-flight flips back. A flip holds until the feet are half a rise past the line it
// crossed, which stops a stand on a line from flickering. When the subject leaves the flight, the floor it left on becomes settled.
//
// What the model does not take, on purpose: the client's roof box test and its wire-key fallback stay in WorldBuildings (the sample
// says which building, if any, the roof rule names), and the flight measurement stays in WorldBuildings::stairTopStep. The wire's
// floor is used only where the review says: the origin of a character first seen mid-flight, and indoors resolution.
namespace ff
{
	// One staircase in its building's local mesh units (the frame WorldBuildings::toLocal gives its measurement): the whole flight's
	// footprint, the foot and top treads' heights, one tread's run and one riser's height, the floors it joins, and the two treads'
	// centres in x/z so "on the flight" can ask the slope (a footprint alone cannot say which end is the foot).
	struct Flight
	{
		float mn[2] = {}, mx[2] = {};		// the flight's footprint, local x/z
		float footY = 0, topY = 0;			// the foot and top treads' heights, local y
		float tread = 0, rise = 0;			// one tread's run and one riser's height
		float foot[2] = {}, top[2] = {};	// the foot and top treads' centres, local x/z
		int lower = 0, upper = 1;			// the floors the flight joins
	};

	// A frame's sample. building is the building pos and flights are measured in: the caller's pick, the model's latched flight
	// building while it is on a flight (latchBuilding()), else the wire's building, else the roof's. -1 = none. pos is the
	// subject's sim position in that building's local mesh units, never its drawn (interpolated) one: x, z are the wire's, y is the standing
	// ray's ground under them since kenshi-k16.2.344 (SimView::simPos stores it), so the rule compares the drawn feet height.
	struct Sample
	{
		bool following = false;				// the camera follows a character this frame
		uint64_t subject = 0;				// the camera subject
		bool known = false;					// the wire knows the subject (it is in the decoder)
		bool hasPos = false;				// its sim position is known this frame
		int building = -1;					// the building pos and flights are measured in, -1 none
		int wireFloor = 0;					// the wire's floor for the subject
		bool indoors = false;				// the wire's indoors bit (in an interior and under a ceiling)
		int roof = -1;						// the client's roof deck test: the building whose deck the subject is on, -1 none
		int roofFloor = 0;					// that building's top floor
		double pos[3] = {};					// the subject's sim position, building-local mesh units
		const Flight* flights = nullptr;	// building's flights (from WorldBuildings::stairTopStep), null = none measured
		int flightCount = 0;
	};

	enum class Reason : uint8_t
	{
		kPlayer,	// not following: the player's level
		kFlight,	// following, on a staircase: the stair latch
		kRoof,		// following, on a roof deck
		kIndoors,	// following, indoors on the wire
		kOutdoors,	// following, outdoors
	};
	const char* reasonName(Reason r);	// "player", "flight", "roof", "indoors", "outdoors"

	struct View
	{
		int level = 0;			// the level to show
		Reason reason = Reason::kPlayer;
		int building = -1;		// the building the level belongs to, -1 none (outdoors, or not following)
	};

	class FloorFollow
	{
	public:
		// One frame: the shown level and why. Subject changes, the auto pair moving and the follow ending are read from the samples.
		View update(const Sample& s);
		// R / F / the diamond: the player's level = the shown one stepped, clamped to [0, top] (the caller's FloorStatus::top, 3 when
		// nothing is sliced or peeked). While following, the caller ends the follow with it: the next update is the free level.
		void step(int dir, int top);
		// A save load or a scene reset: the saved level becomes the player's; a restored subject is followed again from the next sample.
		void load(int level, uint64_t subject);

		int shown() const { return shown_; }			// the last level update returned
		int playerLevel() const { return level_; }		// the player's own level (what the view keeps when the follow ends)
		int autoFloor() const { return following_ && pairValid_ ? pairFloor_ : -1; }	// the subject's resolved floor (-1: not following)
		uint64_t subject() const { return subject_; }	// the subject the latch belongs to (0: none)
		int latchBuilding() const { return latch_.building; }	// the building whose flight the subject is on, -1 none (the next sample's frame)

		// The latch as of the last update, for the trace and the F12 sidecar's floor line (kenshi-k16.2.236.8.4.3).
		struct LatchInfo
		{
			bool on = false, fired = false;	// on a flight; fired: the latch flipped away from its origin floor and is consumed
			int origin = 0, side = 0;
			double y = 0, upperLine = 0, lowerLine = 0, rearmY = 0;	// the feet's local y, the two switch lines, the y that re-arms (fired only)
		};
		const LatchInfo& latchInfo() const { return info_; }

	private:
		struct Resolved
		{
			int building = -1, floor = 0;
			Reason reason = Reason::kOutdoors;
		};
		Resolved resolve(const Sample& s);
		static int onFlight(const Sample& s);		// the flight the feet are on, -1 none
		static double rearmY(const Flight& f, int originSide);	// the feet's y past which a fired hotspot is hot again
		static int sideOnFlight(const Flight& f, int origin, int prev, double y);	// the latch's level, with the half-rise hold
		void resetLatch();

		int level_ = 3;			// the player's level (visible_'s start)
		int shown_ = 3;			// the last level returned
		bool following_ = false;			// the last sample followed
		uint64_t subject_ = 0;				// the last followed subject
		bool pairValid_ = false;			// pair{,_Building,_Floor} holds the last frame's auto pair
		int pairBuilding_ = -1, pairFloor_ = 0;
		bool settledValid_ = false;			// settled_ is the floor the subject was settled on (the next flight's origin)
		int settled_ = 0;
		LatchInfo info_;
		struct Latch
		{
			bool on = false;
			int building = -1, flight = -1;
			int origin = 0, side = 0;	// the floor it boarded from, and the level the latch shows now
		} latch_;
	};
}
