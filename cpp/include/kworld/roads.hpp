#pragma once

// The original's road network (roads_static.md; ROADS_EXE.md C7-C26): type-53 ROAD records of each mod's
// data/newland/leveldata/<mod>/leveldata.level. A road joins two junction ids by a polyline of markers (absolute world xyz);
// junctions have no stored position (the end marker of an incident road). Static, loaded once.
//
// route() is the exe's long-range shortcut (14045a8b0 nearest segment, 14045da60 split + A* over junctions, 140470580): the
// entry point is the nearest road segment to the mover biased toward the goal, the exit the same from the goal, the A* cost is
// road length with a straight-line heuristic, and the road is taken only when (legs + roadLength x factor)^2 <= straight^2.

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kworld {

class RoadNetwork
{
public:
	struct Marker
	{
		double x = 0.0;
		double y = 0.0;
		double z = 0.0;
		double width = 0.0; // the road's ROAD width (xz); 0 = unknown (a restored order, a hand-made road without one)
	};
	struct Road
	{
		std::string id; // the ROAD record's string id (e.g. "1601-Newwworld"); empty for a hand-made road
		std::int32_t start = 0; // junction ids
		std::int32_t end = 0;
		double length = 0.0;
		double width = 0.0; // the ROAD record's float width: the follower hands over the next marker within it (rwh.268)
		std::vector<Marker> markers; // in hex-index order; after a fixup table, the patched list
		std::string sid;             // the ROAD record's string id (empty for a hand-made road)
		bool hidden = false;         // the ROAD's `hidden` flag (not drawn on the map; still walked)
		std::vector<Marker> original; // the markers as the game data loads them, kept when a fixup row patched the road (else empty)
		bool patched = false;        // a move / insert / delete row of the fixup table changed it (kworld/roadfixups.hpp)
	};

	// The exe's scan radius (xz) for an entry or exit segment.
	static constexpr double kScanRadius = 20000.0;
	// Moves over this are tried on the roads first (xz, straight line).
	static constexpr double kLongMove = 1000.0;
	// The "road preference" factor of every vanilla faction is 1 - 0.4; the stuck fallback's call uses 0.2.
	static constexpr double kDefaultFactor = 0.6;
	static constexpr double kStuckFactor = 0.2;
	// The use-test factor of a faction: max(0, 1 - its "road preference") (14065f940 +0x100; ROADS_EXE.md C27/C28). A preference of 1 or more gives factor 0:
	// the road length then costs nothing in the use test, so any road reached passes it.
	static double factorFor(double roadPreference) { return std::max(0.0, 1.0 - roadPreference); }

	RoadNetwork() = default;
	// Throws kworld::Error when dataDir has no newland/leveldata. A later layer's road replaces an earlier one with the same string id.
	// `fixupsPath` (empty = none): the road fixup table applied after the game data, never editing its files (kworld/roadfixups.hpp,
	// docs/formats/ROAD_FIXUPS.md); an unreadable file, an unknown op or a newer version throws kworld::Error. Each applied row is logged to stderr once.
	static RoadNetwork load(std::string_view dataDir, std::string_view fixupsPath = {});
	// A network from roads built by hand (tests); roads with under 2 markers or no length are dropped as the loader does.
	static RoadNetwork fromRoads(std::vector<Road> roads);

	const std::vector<Road>& roads() const noexcept { return roads_; }

	// The marker waypoints, in walking order, of the road shortcut from (fromX, fromZ) to (toX, toZ): the entry point on the nearest
	// segment, the markers along the A* path, the exit point. Empty when no road is near, the roads do not connect the two ends, or the
	// use test fails. `factor` is 1 - the faction's road preference (clamped 0.2..0.9 for the entry bias, as the exe).
	std::vector<Marker> route(double fromX, double fromZ, double toX, double toZ, double factor = kDefaultFactor) const;

private:
	std::vector<Road> roads_;
};

// What the road loader read: per layer (mod folder, "." for the base file) the leveldata.level size and FNV-1a 64 of its bytes, in load
// order, and one FNV-1a 64 over (layer, 0, size, hash) of them all. Offline fixups (kroads_normalize) record it and warn at load on a mismatch.
struct RoadInputStamp
{
	struct Layer
	{
		std::string layer;
		std::uint64_t size = 0;
		std::uint64_t hash = 0;
	};
	std::vector<Layer> layers;
	std::uint64_t hash = 0;
};
// Throws kworld::Error when dataDir has no newland/leveldata.
RoadInputStamp roadInputStamp(std::string_view dataDir);

} // namespace kworld
