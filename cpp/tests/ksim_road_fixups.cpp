// The road fixup table (kenshi-k16.6.712; docs/formats/ROAD_FIXUPS.md): parse / apply / write on hand-made roads (always), the version, op and
// guard refusals, and (needs the install) the Hub spike of 424-Newwworld marker 3 moved onto the stretch between markers 2 and 4: the far-order
// road route past the Hub no longer enters the town and back; the same route with no table shows the old spike.

#include "kworld/error.hpp"
#include "kworld/roadfixups.hpp"
#include "kworld/roads.hpp"

#include "test_util.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <unistd.h>
#include <iostream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace {

using namespace kworld;

RoadNetwork::Road road(const char* sid, std::vector<RoadNetwork::Marker> markers)
{
	RoadNetwork::Road r;
	r.sid = sid;
	r.start = 1;
	r.end = 2;
	r.width = 10.0;
	r.markers = std::move(markers);
	for (std::size_t i = 0; i + 1 < r.markers.size(); ++i)
		r.length += std::hypot(r.markers[i + 1].x - r.markers[i].x, r.markers[i + 1].z - r.markers[i].z);
	return r;
}

const char* kHead = "# kenshi-road-fixups 1\nroad\top\tindex\tx\ty\tz\told_x\told_z\tstatus\tnote\n";

bool refuses(const std::string& text)
{
	try { parseRoadFixups(text); }
	catch (const Error&) { return true; }
	return false;
}

void synthetic()
{
	test::require(parseRoadFixups(kHead).rows.empty(), "a header-only table has no rows");
	test::require(refuses("# kenshi-road-fixups 2\nroad\top\n"), "a newer version is refused");
	test::require(refuses("road\top\n"), "a table with no version line is refused");
	// A bad row is skipped and reported; the good rows still apply.
	const RoadFixupTable partial = parseRoadFixups(std::string(kHead) + "a\tspin\t1\t\t\t\t\t\tok\t\n" + "a\tmove\t1\t1\t2\t3\t\t\tok\t\n" +
		"a\tmark\t\t\t\t\t\t\tok\tgood\n");
	test::require(partial.rows.size() == 1 && partial.errors.size() == 2 && partial.errors[0].find("line 3") != std::string::npos
		&& partial.errors[0].find("unknown op") != std::string::npos, "a bad row is skipped and reported with its line; the good row stays");

	const RoadFixupTable table = parseRoadFixups(std::string(kHead) +
		"a\tmove\t1\t50.00\t0.00\t60.00\t100.00\t0.00\tok\tmoved\n"
		"a\tmark\t\t\t\t\t\t\tunsure\twhole road\n"
		"a\tdelete\t2\t\t\t\t200.00\t0.00\tbad\t\n"
		"a\tinsert\t0\t25.00\t0.00\t10.00\t\t\tok\t\n"
		"a\tmove\t0\t5\t0\t5\t7\t7\tok\tguard\n"
		"a\tmove\t3\t5\t0\t5\t300\t0\tok\tend moved\n"
		"b\tmove\t1\t1\t1\t1\t1\t1\tok\tno such road\n"
		"a\tdelete\t3\t\t\t\t300\t0\tok\tend marker\n");
	std::vector<RoadNetwork::Road> roads{ road("a", { {0, 0, 0}, {100, 0, 0}, {200, 0, 0}, {300, 0, 0} }) };
	const double before = roads[0].length;
	const std::vector<RoadFixupResult> res = applyRoadFixups(roads, table.rows);
	test::require(res[0].applied && res[1].applied && res[2].applied && res[3].applied, "the good rows apply");
	test::require(!res[4].applied && res[4].why.find("guard") != std::string::npos, "a guard mismatch skips the row: " + res[4].why);
	test::require(res[5].applied, "an end marker may move");
	test::require(!res[6].applied && !res[7].applied, "an unknown road and a delete of an end marker are skipped");
	// Markers: 0 (inserted 25,10 after it), 1 moved to (50,60), 2 deleted, 3 moved to (5,5).
	const auto& m = roads[0].markers;
	test::require(m.size() == 4 && m[1].x == 25.0 && m[2].x == 50.0 && m[2].z == 60.0 && m[3].x == 5.0, "the patched marker list");
	test::require(roads[0].patched && roads[0].original.size() == 4 && roads[0].original[1].x == 100.0, "the original markers are kept");
	test::require(std::fabs(roads[0].length - before) > 1.0 && roads[0].length > 0.0, "the length is recomputed");
	test::require(res[0].lengthBefore == before && res[0].lengthAfter == roads[0].length, "the row result carries the road length before -> after");
	test::require(m[2].width == 10.0, "a moved marker keeps the road width");

	// The writer is deterministic: sorted, %.2f, no tabs in notes; a round trip is stable.
	RoadFixupTable messy = table;
	messy.rows[0].note = "a\tb\nc";
	std::reverse(messy.rows.begin(), messy.rows.end());
	const std::string out = writeRoadFixups(messy);
	test::require(out.find("a b c") != std::string::npos && out.find("50.00") != std::string::npos, "the writer cleans notes and writes %.2f");
	test::require(writeRoadFixups(parseRoadFixups(out)) == out, "write -> parse -> write is stable");
	test::require(out.find("a\tmark\t\t") < out.find("a\tinsert\t0"), "rows sort by index, a road-level mark first");
}

// The far-order route on the real data: from the 424 road's first marker to its far end, past the Hub.
double maxZ(const std::vector<RoadNetwork::Marker>& route)
{
	double top = -1.0e9;
	for (const auto& m : route)
		top = std::max(top, m.z);
	return top;
}

void vanilla()
{
	if (!std::filesystem::is_directory(std::string(KENSHI_DATA) + "/newland/leveldata/Newwworld"))
	{
		std::cerr << "ksim_road_fixups: part 2 skipped: no Newwworld leveldata in " << KENSHI_DATA << "\n";
		return;
	}
	const std::string file = "ksim_road_fixups_" + std::to_string(::getpid()) + ".tsv"; // unique per run: parallel ctest cannot collide
	struct Cleanup
	{
		const std::string& path;
		~Cleanup() { std::error_code ignore; std::filesystem::remove(path, ignore); }
	} cleanup{file};
	const auto write = [&](const std::string& text) { std::ofstream(file, std::ios::binary) << text; };
	const auto route = [](const RoadNetwork& net) { return net.route(-50354.0, 1132.0, -57023.0, -878.0, RoadNetwork::kStuckFactor); };

	const RoadNetwork plain = RoadNetwork::load(KENSHI_DATA);
	const std::vector<RoadNetwork::Marker> old = route(plain);
	test::require(!old.empty() && maxZ(old) > 2700.0, "with no table the far route enters the Hub spike (max z " + std::to_string(maxZ(old)) + ")");

	// Marker 3 (-51032.7, 2782.9) onto the stretch between markers 2 (-50932.8, 873.5) and 4 (-51602.3, 1023.8).
	write(std::string(kHead) + "424-Newwworld\tmove\t3\t-51267.55\t1160.00\t948.65\t-51032.70\t2782.94\tok\tfixture: onto the 2..4 stretch\n"
		"424-Newwworld\tmark\t\t\t\t\t\t\tunsure\tfixture\n");
	const RoadNetwork fixed = RoadNetwork::load(KENSHI_DATA, file);
	const std::vector<RoadNetwork::Marker> now = route(fixed);
	test::require(!now.empty() && maxZ(now) < 1300.0, "with the table the far route stays on the stretch (max z " + std::to_string(maxZ(now)) + ")");
	const auto find = [](const RoadNetwork& net) -> const RoadNetwork::Road*
	{
		for (const auto& r : net.roads())
			if (r.sid == "424-Newwworld")
				return &r;
		return nullptr;
	};
	test::require(find(plain) && !find(plain)->patched && find(plain)->original.empty(), "the unpatched road has no original list");
	test::require(find(fixed) && find(fixed)->patched && find(fixed)->original.size() == 20 && find(fixed)->original[3].z > 2700.0, "the patched road keeps the original markers");
	test::require(find(fixed)->length < find(plain)->length - 2000.0, "the patched road's length drops by the spike");
	test::require(plain.roads().size() == fixed.roads().size(), "no road added or dropped");
	test::require(roadFixupsHash(file) != 0 && foldRoadFixups(5, file) != 5 && foldRoadFixups(5, "") == 5, "the table's hash folds into the data hash");

	// A guard mismatch skips the row (the old spike stays); a newer version or an unknown op refuses the file.
	write(std::string(kHead) + "424-Newwworld\tmove\t3\t-51267.55\t1160.00\t948.65\t-40000.00\t0.00\tok\twrong guard\n");
	const RoadNetwork skipped = RoadNetwork::load(KENSHI_DATA, file);
	test::require(!find(skipped)->patched && maxZ(route(skipped)) > 2700.0, "a guard mismatch leaves the road as the game has it");
	// A bad row applies the good rows (and reports the bad); a bad header or newer version loads the roads unpatched, never throwing.
	write(std::string(kHead) + "424-Newwworld\twarp\t3\t\t\t\t\t\tok\t\n424-Newwworld\tmove\t3\t-51267.55\t1160.00\t948.65\t-51032.70\t2782.94\tok\tgood\n");
	test::require(find(RoadNetwork::load(KENSHI_DATA, file))->patched, "a table with one bad row still applies the good row");
	for (const std::string& bad : { std::string("# kenshi-road-fixups 9\n"), std::string("424-Newwworld\tmove\n"), std::string("") })
	{
		write(bad);
		const RoadNetwork unpatched = RoadNetwork::load(KENSHI_DATA, file);
		test::require(!find(unpatched)->patched && unpatched.roads().size() == plain.roads().size(), "a bad header or version loads unpatched");
		test::require(roadFixupsHash(file) == 0, "an unusable table adds nothing to the data hash");
	}
	const RoadNetwork missing = RoadNetwork::load(KENSHI_DATA, file + ".absent");
	test::require(!find(missing)->patched, "a missing table loads unpatched");
}

} // namespace

int main()
{
	synthetic();
	vanilla();
	return 0;
}
