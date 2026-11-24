// Offline road normalization analysis (kenshi-k16.6.716; user 2026-11-22 on k16.6.715: consolidate repeated points, merge overlapping
// lines, split a road where another road's end lies on it). Part 1: a synthetic network (a road end touching another road mid-segment,
// a repeated point, an overlap run, a crossing). Part 2 (needs the install): 1601-Newwworld #15-#18 = 1886-Newww Fixes #3-#0 is one
// overlap run with its fork node at (49108.2, -20364.6); 837-Newwworld #5 is a T on 532-Newwworld-INGAME's segment #6-#7 and
// 837 #5..#7 ride the shared 532 run; 837 #1/#2 collapse; the data stamp is stable; the whole-world counts are printed.

#include "kworld/roads.hpp"
#include "kworld/roads_normalize.hpp"

#include "test_util.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using kworld::RoadChange;
using kworld::RoadNetwork;
using Kind = RoadChange::Kind;

RoadNetwork::Road road(const std::string& id, int a, int b, std::vector<RoadNetwork::Marker> markers)
{
	RoadNetwork::Road r;
	r.id = id;
	r.start = a;
	r.end = b;
	r.width = 100.0;
	r.markers = std::move(markers);
	for (std::size_t i = 0; i + 1 < r.markers.size(); ++i)
		r.length += std::hypot(r.markers[i + 1].x - r.markers[i].x, r.markers[i + 1].z - r.markers[i].z);
	return r;
}

std::vector<const RoadChange*> ofKind(const kworld::RoadNormalizeResult& result, Kind kind)
{
	std::vector<const RoadChange*> out;
	for (const RoadChange& c : result.changes)
		if (c.kind == kind)
			out.push_back(&c);
	return out;
}

bool near(double a, double b, double tol = 0.1) { return std::fabs(a - b) < tol; }

void synthetic()
{
	// A runs x 0..2000 along z 0. B ends 10 u above A's middle (a T). C starts at A's end (repeat, 1 u off) and shares B's way back
	// (an overlap run of 3 nodes with D, reversed). E crosses A mid-span far from every marker.
	const RoadNetwork net = RoadNetwork::fromRoads({
		road("A", 1, 2, {{0, 0, 0}, {1000, 0, 0}, {2000, 0, 0}}),
		road("B", 3, 4, {{500, 0, 10}, {500, 0, 600}}),
		road("C", 2, 5, {{2001, 0, 0}, {3000, 0, 0}, {3000, 0, 1000}, {3500, 0, 1000}}),
		road("D", 5, 6, {{3500, 0, 1000}, {3000, 0, 1000}, {3000, 0, 0}, {3600, 0, -400}}),
		road("E", 7, 8, {{1500, 0, -500}, {1500, 0, 700}}),
	});
	const auto result = kworld::normalizeRoads(net);
	const auto touches = ofKind(result, Kind::TTouch);
	test::require(touches.size() == 1, "one T-touch (B's end on A's first segment), got " + std::to_string(touches.size()));
	test::require(touches[0]->a.road == "B" && touches[0]->a.marker == 0 && touches[0]->b.road == "A" && touches[0]->b.segment == 0, "the T is B #0 on A's segment 0");
	test::require(near(touches[0]->b.t, 0.5, 1e-6) && near(touches[0]->x, 500.0) && near(touches[0]->distance, 10.0), "at t = 0.5, 10 u off");
	const auto repeats = ofKind(result, Kind::Repeat);
	bool join = false;
	for (const RoadChange* c : repeats)
		join = join || (c->a.road == "A" && c->a.marker == 2 && c->b.road == "C" && c->b.marker == 0 && near(c->distance, 1.0));
	test::require(join, "A #2 and C #0 (1 u apart) are one node");
	const auto overlaps = ofKind(result, Kind::Overlap);
	test::require(overlaps.size() == 1 && overlaps[0]->a.road == "C" && overlaps[0]->b.road == "D" && overlaps[0]->nodes == 3 && overlaps[0]->reversed, "one reversed overlap run of 3 nodes between C and D");
	test::require(overlaps[0]->a.marker == 1 && overlaps[0]->aEnd.marker == 3 && overlaps[0]->b.marker == 2 && overlaps[0]->bEnd.marker == 0, "the run is C #1..#3 = D #2..#0");
	const auto crossings = ofKind(result, Kind::Crossing);
	test::require(crossings.size() == 1 && crossings[0]->a.road == "A" && crossings[0]->b.road == "E" && near(crossings[0]->x, 1500.0) && crossings[0]->note.find("rwh.305") != std::string::npos, "E crosses A mid-span: listed, pending rwh.305/306");
	const auto again = kworld::normalizeRoads(net);
	test::require(again.changes.size() == result.changes.size(), "deterministic");
}

const RoadChange* overlapOf(const std::vector<const RoadChange*>& list, const std::string& a, const std::string& b)
{
	for (const RoadChange* c : list)
		if ((c->a.road == a && c->b.road == b) || (c->a.road == b && c->b.road == a))
			return c;
	return nullptr;
}

void install()
{
	if (!std::filesystem::is_directory(std::string(KENSHI_DATA) + "/newland/leveldata/Newwworld"))
	{
		std::cerr << "kworld_roads_normalize: part 2 skipped: no Newwworld leveldata in " << KENSHI_DATA << "\n";
		return;
	}
	const RoadNetwork net = RoadNetwork::load(KENSHI_DATA);
	const auto result = kworld::normalizeRoads(net);
	const auto overlaps = ofKind(result, Kind::Overlap);
	const auto touches = ofKind(result, Kind::TTouch);
	std::cout << "kworld_roads_normalize: whole world: roads " << net.roads().size();
	for (std::size_t k = 0; k < result.perKind.size(); ++k)
		std::cout << ", " << kworld::roadChangeKindName(static_cast<Kind>(k)) << " " << result.perKind[k];
	std::cout << "\n";

	// 1601-Newwworld #15-#18 = 1886-Newww Fixes #3-#0, fork node at the south end.
	const RoadChange* run = overlapOf(overlaps, "1601-Newwworld", "1886-Newww Fixes");
	test::require(run != nullptr, "the 1601 / 1886 Fixes overlap is found");
	test::require(run->a.road == "1601-Newwworld" && run->reversed && run->nodes == 4, "one run of 4 nodes, reversed");
	test::require(run->a.marker == 15 && run->aEnd.marker == 18 && run->b.marker == 3 && run->bEnd.marker == 0, "1601 #15..#18 = 1886F #3..#0");
	test::require(near(run->x, 49108.2, 0.1) && near(run->z, -20364.6, 0.1), "the fork node is at (49108.2, -20364.6)");
	test::require(run->note == "start:AB end:-", "both roads go on at the fork; at 2044 both end");

	// 837-Newwworld #5 lies on 532-Newwworld-INGAME's segment #6-#7: a T; 837 #5..#7 ride the shared 532 run.
	const RoadChange* tee = nullptr;
	for (const RoadChange* c : touches)
		if (c->a.road == "837-Newwworld" && c->a.marker == 5)
			tee = c;
	test::require(tee != nullptr && tee->b.road == "532-Newwworld-INGAME" && tee->b.segment == 6, "837 #5 is a T on 532 segment #6-#7");
	test::require(tee->distance < 1.0 && near(tee->b.t, 0.92, 0.01), "0.2 u off, t = 0.92");
	const RoadChange* ride = overlapOf(overlaps, "532-Newwworld-INGAME", "837-Newwworld");
	test::require(ride != nullptr && ride->b.road == "837-Newwworld" && ride->reversed, "837 and 532 share a run");
	test::require(ride->bEnd.marker == 5 && ride->b.marker >= 7, "the run covers 837 #5..#7 (the T, then 532's #6 and #5)");
	bool collapse = false;
	for (const RoadChange& c : result.changes)
		collapse = collapse || (c.kind == Kind::Repeat && c.a.road == "837-Newwworld" && c.a.marker == 1 && c.b.road == "837-Newwworld" && c.b.marker == 2);
	test::require(collapse, "837 #1 / #2 (18 u apart) collapse to one node");

	test::require(result.perKind[0] > 1000 && result.perKind[1] >= 30 && result.perKind[2] >= 10, "plausible whole-world counts");
	const kworld::RoadInputStamp stamp = kworld::roadInputStamp(KENSHI_DATA);
	test::require(!stamp.layers.empty() && stamp.hash == kworld::roadInputStamp(KENSHI_DATA).hash, "the data stamp is stable");
	std::cout << "kworld_roads_normalize: data stamp " << std::hex << stamp.hash << std::dec << " over " << stamp.layers.size() << " layers\n";
}

} // namespace

int main()
{
	try
	{
		synthetic();
		install();
		std::cout << "kworld_roads_normalize: repeats, overlaps and T-touches found\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "kworld_roads_normalize: " << e.what() << "\n";
		return 1;
	}
}
