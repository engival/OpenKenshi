// Offline road normalization analysis (kenshi-k16.6.716). Reads the game data dir, runs kworld::normalizeRoads over the raw roads
// and writes the change list as TSV (to stdout, or to OUT) with the input data stamp in the header and the counts per kind.
// Nothing is applied. Usage: kroads_normalize [DATA_DIR] [OUT.tsv]

#include "kworld/roads.hpp"
#include "kworld/roads_normalize.hpp"

#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

#ifndef KENSHI_DATA
#define KENSHI_DATA ""
#endif

namespace {

std::string at(const kworld::RoadRef& r)
{
	if (r.road.empty())
		return "";
	return r.road + "#" + (r.marker >= 0 ? std::to_string(r.marker) : std::to_string(r.segment) + "+" + std::to_string(r.t).substr(0, 5));
}

std::string num(double v)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.1f", v);
	return buf;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const std::string dataDir = argc > 1 && argv[1][0] ? argv[1] : KENSHI_DATA;
		std::FILE* out = argc > 2 ? std::fopen(argv[2], "w") : stdout;
		if (!out)
		{
			std::cerr << "cannot write " << argv[2] << "\n";
			return 1;
		}
		const kworld::RoadInputStamp stamp = kworld::roadInputStamp(dataDir);
		const kworld::RoadNetwork net = kworld::RoadNetwork::load(dataDir);
		const kworld::RoadNormalizeResult result = kworld::normalizeRoads(net);
		std::fprintf(out, "# kroads_normalize v1 zone=%.0f roads=%zu\n# data_stamp %016llx\n", kworld::RoadNormalizeResult::kZone, net.roads().size(), static_cast<unsigned long long>(stamp.hash));
		for (const auto& layer : stamp.layers)
			std::fprintf(out, "# layer %s size=%llu fnv=%016llx\n", layer.layer.c_str(), static_cast<unsigned long long>(layer.size), static_cast<unsigned long long>(layer.hash));
		for (std::size_t k = 0; k < result.perKind.size(); ++k)
			std::fprintf(out, "# count %s %zu\n", kworld::roadChangeKindName(static_cast<kworld::RoadChange::Kind>(k)), result.perKind[k]);
		const char* names[] = {"0-1", "1-5", "5-10", "10-25", "25-50", "50-100", "100-200"};
		for (std::size_t i = 0; i < 7; ++i)
			std::fprintf(out, "# hist %s cross_pairs=%zu same_road_pairs=%zu marker_to_segment=%zu\n", names[i], result.pairsCross[i], result.pairsSame[i], result.markerSegment[i]);
		std::fprintf(out, "kind\ta\tb\ta_end\tb_end\tx\tz\tx2\tz2\tdistance\tnodes\treversed\tnote\n");
		for (const kworld::RoadChange& c : result.changes)
			std::fprintf(out, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\n", kworld::roadChangeKindName(c.kind), at(c.a).c_str(), at(c.b).c_str(), at(c.aEnd).c_str(), at(c.bEnd).c_str(), num(c.x).c_str(), num(c.z).c_str(),
				c.kind == kworld::RoadChange::Kind::Overlap ? num(c.x2).c_str() : "", c.kind == kworld::RoadChange::Kind::Overlap ? num(c.z2).c_str() : "", num(c.distance).c_str(), c.nodes, c.reversed ? 1 : 0, c.note.c_str());
		if (out != stdout)
			std::fclose(out);
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "kroads_normalize: " << e.what() << "\n";
		return 1;
	}
}
