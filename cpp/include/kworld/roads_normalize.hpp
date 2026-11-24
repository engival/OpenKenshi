#pragma once

// Offline analysis of the raw road network (kenshi-k16.6.716, user 2026-11-22 on k16.6.715): which markers repeat, which runs
// overlap, which road ends touch another road's segment. Nothing is applied: the result becomes bundled fixup rows (a later item),
// the loader never normalizes at launch. Every marker is a zone ("about the area you want to be in"), radius kZone.
//
//   repeat   two markers whose zones overlap (distance < 2 kZone), same or different road: one shared node
//   overlap  a run of >= 2 consecutive shared nodes in two roads (same or reversed order): one shared edge run; its ends are the fork nodes
//   t_touch  a marker within kZone of another road's segment (interior): the segment splits there, at a node shared with the marker
//   crossing two segments cross mid-span with no shared node: listed only (pending kenshi-rwh.305/306)
//   near_miss a marker 50 u .. road width from another road: listed with its distance, never merged
// Positions are xz. Rows come in a fixed order (kind, then road order, then marker order), the roads in string-id order.

#include "kworld/roads.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace kworld {

struct RoadRef
{
	std::string road;  // string id
	int marker = -1;   // marker index; -1 = a point on a segment
	int segment = -1;  // that point: lies on segment `segment` (markers segment, segment+1) ...
	double t = 0.0;    // ... at this fraction
};

struct RoadChange
{
	enum class Kind { Repeat, Overlap, TTouch, Crossing, NearMiss };
	Kind kind = Kind::Repeat;
	// repeat: a = the kept marker (lowest road, marker order), b = the one merged into it. t_touch: a = the touching marker, b = the point
	// on the touched segment. crossing: a, b = the two crossing points. near_miss: a = the marker, b = the nearest point on the other road.
	// overlap: a..aEnd = the run in road A, b..bEnd = the same nodes in road B (bEnd before b when `reversed`).
	RoadRef a, b, aEnd, bEnd;
	double x = 0.0, z = 0.0;   // the position (overlap: start node)
	double x2 = 0.0, z2 = 0.0; // overlap: end node
	double distance = 0.0;     // repeat / t_touch / near_miss: the distance that qualified it
	int nodes = 0;             // overlap: shared nodes in the run
	bool reversed = false;     // overlap: road B runs the other way
	std::string note;          // overlap: which roads continue past each end (fork); crossing: the pending beads
};

struct RoadNormalizeResult
{
	static constexpr double kZone = 25.0; // the user's 20-30 u circle zone
	static constexpr std::array<double, 7> kBinEdge{1.0, 5.0, 10.0, 25.0, 50.0, 100.0, 200.0}; // histogram bins: [0,1) ... [100,200)

	std::vector<RoadChange> changes;
	std::array<std::size_t, 5> perKind{}; // indexed by RoadChange::Kind
	// Distances below 200 u: marker to marker, in different roads / in one road; marker to another road's segment interior.
	std::array<std::size_t, 7> pairsCross{}, pairsSame{}, markerSegment{};
};

RoadNormalizeResult normalizeRoads(const RoadNetwork& net);

const char* roadChangeKindName(RoadChange::Kind kind);

} // namespace kworld
