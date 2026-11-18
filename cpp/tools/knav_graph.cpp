// Coarse graph builder: knav_graph --world DIR --out FILE [--regions RX0 RZ0 RX1 RZ1] [--threads N]
//                       [--route X0 Z0 X1 Z1]
// Reads the region files knav_bake --world wrote (DIR/region_RX_RZ.knav), builds the clusters of each
// region against a mesh holding the region and a one-cluster margin of its neighbours, and writes
// one graph file. With the original globalPathing.path available it prints a cross-check of which
// 4 x 4 zone blocks it joins and which ours does. --route times a coarse query between two world points
// against the finished graph, loading just the region meshes under the two ends.

#include "knav/coarse.hpp"
#include "knav/navmesh.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/pathing.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point from)
{
	return std::chrono::duration<double>(Clock::now() - from).count();
}

} // namespace

int main(int argc, char** argv)
{
	std::string dir;
	std::string out = "world.kgraph";
	int threads = 4;
	constexpr int kRegions = kworld::kZoneCellsPerAxis / knav::kRegionZones;
	int regions[4] = { 0, 0, kRegions - 1, kRegions - 1 };
	double route[4] = {};
	bool routed = false;
	knav::TravelFilter travel;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--world" && i + 1 < argc)
			dir = argv[++i];
		else if (arg == "--out" && i + 1 < argc)
			out = argv[++i];
		else if (arg == "--threads" && i + 1 < argc)
			threads = std::atoi(argv[++i]);
		else if (arg == "--dry")
			travel = knav::TravelFilter::dryOnly();
		else if (arg == "--route" && i + 4 < argc)
		{
			for (double& v : route)
				v = std::atof(argv[++i]);
			routed = true;
		}
		else if (arg == "--regions" && i + 4 < argc)
		{
			for (int& region : regions)
				region = std::atoi(argv[++i]);
		} else {
			dir.clear();
			break;
		}
	}
	if (dir.empty())
	{
		std::cerr << "usage: knav_graph --world DIR --out FILE [--regions RX0 RZ0 RX1 RZ1] [--threads N] [--dry]\n";
		return 2;
	}

	try
	{
		const auto start = Clock::now();
		constexpr int kRegionTiles = knav::kRegionZones * knav::kTilesPerZone;
		knav::CoarseGraph graph;
		for (int rz = regions[1]; rz <= regions[3]; ++rz)
		{
			for (int rx = regions[0]; rx <= regions[2]; ++rx)
			{
				const auto step = Clock::now();
				const knav::TileRect own{ rx * kRegionTiles, rz * kRegionTiles, (rx + 1) * kRegionTiles - 1,
					(rz + 1) * kRegionTiles - 1 };
				if (!std::filesystem::exists(dir + "/" + knav::regionFileName(rx, rz)))
					continue;
				const knav::NavMesh mesh = knav::NavMesh::loadRegion(dir, rx, rz, knav::kClusterTiles);
				graph.merge(knav::CoarseGraph::build(mesh, knav::CoarseGraph::clustersOf(own), threads, travel));
				std::cout << "region " << rx << "," << rz << ": " << secondsSince(step) << " s\n" << std::flush;
			}
		}
		graph.save(out);
		const knav::CoarseStats stats = graph.stats();
		std::cout << "graph: " << secondsSince(start) << " s on " << threads << " thread(s), " << stats.clusters
			<< " clusters, " << stats.nodes << " nodes, " << stats.portals << " portals, " << stats.pairs
			<< " intra edges, " << stats.points << " path points, " << std::filesystem::file_size(out)
			<< " bytes (" << out << ")\n";

		// Which neighbouring 4 x 4 zone blocks hold a portal between them (the blocks are our regions).
		constexpr int kClustersPerRegion = kRegionTiles / knav::kClusterTiles;
		std::set<std::pair<int, int>> ours;
		for (const auto& [id, cluster] : graph.clusters())
		{
			for (const knav::Portal& portal : cluster.portals)
			{
				const int dx = portal.side == knav::Side::East ? 1 : portal.side == knav::Side::West ? -1 : 0;
				const int dz = portal.side == knav::Side::North ? 1 : portal.side == knav::Side::South ? -1 : 0;
				const int from = id.x / kClustersPerRegion + kRegions * (id.z / kClustersPerRegion);
				const int to = (id.x + dx) / kClustersPerRegion + kRegions * ((id.z + dz) / kClustersPerRegion);
				if (from != to && id.x + dx >= 0 && id.z + dz >= 0)
					ours.insert({ std::min(from, to), std::max(from, to) });
			}
		}
		if (routed)
		{
			const auto terrain = kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif");
			const auto regionAt = [](double x, double z)
			{
				const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
				return std::pair{ cell.x / knav::kRegionZones, cell.z / knav::kRegionZones };
			};
			const auto fromRegion = regionAt(route[0], route[1]);
			const auto toRegion = regionAt(route[2], route[3]);
			const knav::NavMesh fromMesh = knav::NavMesh::loadRegion(dir, fromRegion.first, fromRegion.second, 0);
			const knav::NavMesh toMesh = knav::NavMesh::loadRegion(dir, toRegion.first, toRegion.second, 0);
			const knav::Point from = { static_cast<float>(route[0]), static_cast<float>(terrain.height(route[0], route[1])), static_cast<float>(route[1]) };
			const knav::Point to = { static_cast<float>(route[2]), static_cast<float>(terrain.height(route[2], route[3])), static_cast<float>(route[3]) };
			constexpr int kRuns = 20;
			const auto begun = Clock::now();
			knav::CoarsePath path;
			for (int i = 0; i < kRuns; ++i)
				path = graph.findPath(fromMesh, toMesh, from, to, { 40.0f, 60.0f, 40.0f });
			std::cout << "route: found " << path.found << " (failure " << static_cast<int>(path.failure) << "), " << path.legs.size()
				<< " legs, length " << path.length << ", straight "
				<< std::hypot(route[2] - route[0], route[3] - route[1]) << ", " << secondsSince(begun) * 1000.0 / kRuns
				<< " ms per query (average of " << kRuns << ")\n";
		}
		const std::string pathing = std::string(KENSHI_DATA) + "/globalPathing.path";
		if (std::filesystem::exists(pathing))
		{
			std::set<std::pair<int, int>> theirs;
			for (const kworld::PathNode& node : kworld::GlobalPathing::load(pathing).nodes)
			{
				const int from = static_cast<int>(node.cellX) + kRegions * static_cast<int>(node.cellZ);
				const int to = static_cast<int>(node.neighbourX) + kRegions * static_cast<int>(node.neighbourZ);
				theirs.insert({ std::min(from, to), std::max(from, to) });
			}
			const auto inRange = [&](int block)
			{
				return block % kRegions >= regions[0] && block % kRegions <= regions[2]
					&& block / kRegions >= regions[1] && block / kRegions <= regions[3];
			};
			int both = 0;
			int onlyOurs = 0;
			int onlyTheirs = 0;
			for (const auto& pair : ours)
				(theirs.contains(pair) ? both : onlyOurs) += 1;
			for (const auto& pair : theirs)
				if (!ours.contains(pair) && inRange(pair.first) && inRange(pair.second))
					++onlyTheirs;
			std::cout << "block pairs joined: both " << both << ", only ours " << onlyOurs << ", only globalPathing "
				<< onlyTheirs << "\n";
		}
	} catch (const std::exception& e) {
		std::cerr << "knav_graph: " << e.what() << "\n";
		return 1;
	}
	return 0;
}
