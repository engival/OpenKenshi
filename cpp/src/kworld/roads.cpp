#include "kworld/roads.hpp"

#include "kworld/error.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/zones.hpp"
#include "io.hpp"
#include "le.hpp"
#include "kdata/format.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <queue>
#include <system_error>
#include <unordered_map>

namespace kworld {

namespace {

using Marker = RoadNetwork::Marker;

bool findInt(const kdata::Record& record, std::string_view key, std::int32_t& out)
{
	for (auto entry = record.ints.rbegin(); entry != record.ints.rend(); ++entry)
		if (entry->key == key)
		{
			out = entry->value;
			return true;
		}
	return false;
}

double findFloat(const kdata::Record& record, std::string_view key)
{
	for (auto entry = record.floats.rbegin(); entry != record.floats.rend(); ++entry)
		if (entry->key == key)
			return detail::readF32(entry->value.data());
	return 0.0;
}

// 14045e760: marker n is the instance keyed by n in lowercase hex; the loop stops at the first missing key, or after the marker whose
// w (rotation X) reaches 1.0. Sorting by decimal or string order would misorder roads of 17 or more markers (roads_static.md).
RoadNetwork::Road readRoad(const kdata::Record& record)
{
	RoadNetwork::Road road;
	findInt(record, "start", road.start);
	findInt(record, "end", road.end);
	road.length = findFloat(record, "length");
	std::map<long, const kdata::Instance*> byIndex;
	for (const kdata::Instance& instance : record.instances)
	{
		const std::string key = instance.idIsString ? instance.id : std::to_string(instance.idInt);
		char* tail = nullptr;
		const long index = std::strtol(key.c_str(), &tail, 16);
		if (tail != key.c_str() && *tail == '\0')
			byIndex[index] = &instance;
	}
	for (long n = 0;; ++n)
	{
		const auto found = byIndex.find(n);
		if (found == byIndex.end())
			break;
		const std::byte* t = found->second->transform.data();
		road.markers.push_back({detail::readF32(t), detail::readF32(t + 4), detail::readF32(t + 8)});
		if (detail::readF32(t + 16) >= 1.0f)
			break;
	}
	return road;
}

double dist(const Marker& a, const Marker& b) { return std::hypot(a.x - b.x, a.z - b.z); }

// An entry or exit: the road, the segment (markers s and s+1) and the point on it.
struct RoadPos
{
	int road = -1;
	int segment = 0;
	double t = 0.0; // 0..1 along the segment
	Marker point;
	double score = std::numeric_limits<double>::max();
};

// 14045a8b0: the segment within kScanRadius (xz) minimising dist - dot(dir, closest - pos): segments ahead of travel are favoured.
RoadPos nearest(const std::vector<RoadNetwork::Road>& roads, double px, double pz, double dirX, double dirZ)
{
	RoadPos best;
	for (std::size_t r = 0; r < roads.size(); ++r)
	{
		const auto& m = roads[r].markers;
		for (std::size_t s = 0; s + 1 < m.size(); ++s)
		{
			const double sx = m[s + 1].x - m[s].x;
			const double sz = m[s + 1].z - m[s].z;
			const double len2 = sx * sx + sz * sz;
			const double t = len2 > 0.0 ? std::clamp(((px - m[s].x) * sx + (pz - m[s].z) * sz) / len2, 0.0, 1.0) : 0.0;
			const double cx = m[s].x + sx * t;
			const double cz = m[s].z + sz * t;
			const double d = std::hypot(cx - px, cz - pz);
			if (d > RoadNetwork::kScanRadius)
				continue;
			const double score = d - ((cx - px) * dirX + (cz - pz) * dirZ);
			if (score < best.score)
			{
				best.road = static_cast<int>(r);
				best.segment = static_cast<int>(s);
				best.t = t;
				best.point = {cx, m[s].y + (m[s + 1].y - m[s].y) * t, cz};
				best.score = score;
			}
		}
	}
	return best;
}

// Arc length of a road from its first marker to a position on it.
double arcTo(const RoadNetwork::Road& road, const RoadPos& pos)
{
	double len = 0.0;
	for (int s = 0; s < pos.segment; ++s)
		len += std::hypot(road.markers[s + 1].x - road.markers[s].x, road.markers[s + 1].z - road.markers[s].z);
	const auto& m = road.markers;
	return len + std::hypot(m[pos.segment + 1].x - m[pos.segment].x, m[pos.segment + 1].z - m[pos.segment].z) * pos.t;
}

double polyline(const RoadNetwork::Road& road)
{
	double len = 0.0;
	for (std::size_t s = 0; s + 1 < road.markers.size(); ++s)
		len += dist(road.markers[s], road.markers[s + 1]);
	return len;
}

// Appends the markers of `road` from marker a to marker b inclusive (either direction).
void appendMarkers(std::vector<Marker>& out, const RoadNetwork::Road& road, int a, int b)
{
	if (a <= b)
		for (int i = a; i <= b; ++i)
			out.push_back(road.markers[i]);
	else
		for (int i = a; i >= b; --i)
			out.push_back(road.markers[i]);
}

} // namespace

RoadNetwork RoadNetwork::fromRoads(std::vector<Road> roads)
{
	RoadNetwork net;
	for (Road& road : roads)
		if (road.markers.size() >= 2 && road.length > 0.0)
			net.roads_.push_back(std::move(road));
	return net;
}

RoadNetwork RoadNetwork::load(std::string_view dataDir)
{
	namespace fs = std::filesystem;
	const fs::path root = fs::absolute(fs::path(dataDir)) / "newland";
	std::error_code error;
	if (!fs::is_directory(root / "leveldata", error))
		throw Error("roads: no leveldata directory at " + (root / "leveldata").string());
	struct Source
	{
		int rank;
		std::string layer;
		fs::path path;
	};
	std::vector<Source> sources;
	if (fs::is_regular_file(root / "leveldata.level", error))
		sources.push_back({zoneLayerRank("."), ".", root / "leveldata.level"});
	for (fs::directory_iterator it(root / "leveldata", error), end; it != end; it.increment(error))
	{
		if (error)
			throw Error("roads: cannot walk leveldata: " + error.message());
		const fs::path candidate = it->path() / "leveldata.level";
		if (it->is_directory(error) && fs::is_regular_file(candidate, error))
		{
			const std::string layer = it->path().filename().string();
			sources.push_back({zoneLayerRank(layer), layer, candidate});
		}
	}
	std::sort(sources.begin(), sources.end(), [](const Source& a, const Source& b) { return a.rank != b.rank ? a.rank < b.rank : a.layer < b.layer; });
	const std::int32_t roadType = itemTypeByName("ROAD");
	std::map<std::string, Road> byId;
	for (const Source& source : sources)
	{
		auto bytes = detail::readFile(source.path.string());
		const kdata::File file = kdata::parse(bytes, source.path.filename().string());
		for (const kdata::Record& record : file.records)
			if (record.type == roadType)
				byId[record.stringID] = readRoad(record);
	}
	std::vector<Road> roads;
	for (auto& [id, road] : byId)
		roads.push_back(std::move(road));
	return fromRoads(std::move(roads));
}

std::vector<Marker> RoadNetwork::route(double fromX, double fromZ, double toX, double toZ, double factor) const
{
	const double straight = std::hypot(toX - fromX, toZ - fromZ);
	if (roads_.empty() || straight <= 0.0)
		return {};
	const double bias = std::clamp(factor, 0.2, 0.9);
	const double dirX = (toX - fromX) / straight * bias;
	const double dirZ = (toZ - fromZ) / straight * bias;
	const RoadPos in = nearest(roads_, fromX, fromZ, dirX, dirZ);
	const RoadPos out = nearest(roads_, toX, toZ, -dirX, -dirZ);
	if (in.road < 0 || out.road < 0)
		return {};

	// Junction nodes, then the entry (E) and exit (X) nodes. 140470580: A* over junctions, cost = road length, straight-line heuristic;
	// a junction sits at the end marker of its first incident road.
	std::unordered_map<std::int32_t, int> junction;
	std::vector<Marker> pos;
	const auto node = [&](std::int32_t id, const Marker& at)
	{
		const auto [slot, fresh] = junction.emplace(id, static_cast<int>(pos.size()));
		if (fresh)
			pos.push_back(at);
		return slot->second;
	};
	struct Edge
	{
		int to;
		double cost;
		int road;  // the road walked; -2: no marker between (entry and exit on one segment)
		int a, b;  // marker range walked, from the near node's side
	};
	std::vector<std::vector<Edge>> adj;
	std::vector<int> roadStart(roads_.size()), roadEnd(roads_.size());
	for (std::size_t r = 0; r < roads_.size(); ++r)
	{
		roadStart[r] = node(roads_[r].start, roads_[r].markers.front());
		roadEnd[r] = node(roads_[r].end, roads_[r].markers.back());
	}
	const int nodeE = static_cast<int>(pos.size());
	pos.push_back(in.point);
	const int nodeX = static_cast<int>(pos.size());
	pos.push_back(out.point);
	adj.resize(pos.size());
	const auto link = [&](int a, int b, double cost, int road, int ma, int mb)
	{
		adj[a].push_back({b, cost, road, ma, mb});
		adj[b].push_back({a, cost, road, mb, ma});
	};
	for (std::size_t r = 0; r < roads_.size(); ++r)
		link(roadStart[r], roadEnd[r], roads_[r].length, static_cast<int>(r), 0, static_cast<int>(roads_[r].markers.size()) - 1);
	// A split road: the stored length is shared out by the arc fraction (14045b690, length = fraction x parent length).
	const auto split = [&](const RoadPos& p, int self)
	{
		const Road& road = roads_[p.road];
		const double total = polyline(road);
		const double frac = total > 0.0 ? arcTo(road, p) / total : 0.0;
		const int last = static_cast<int>(road.markers.size()) - 1;
		adj[self].push_back({roadStart[p.road], road.length * frac, p.road, p.segment, 0});
		adj[roadStart[p.road]].push_back({self, road.length * frac, p.road, 0, p.segment});
		adj[self].push_back({roadEnd[p.road], road.length * (1.0 - frac), p.road, p.segment + 1, last});
		adj[roadEnd[p.road]].push_back({self, road.length * (1.0 - frac), p.road, last, p.segment + 1});
		return frac;
	};
	const double fracIn = split(in, nodeE);
	const double fracOut = split(out, nodeX);
	if (in.road == out.road)
	{
		const Road& road = roads_[in.road];
		// Along the one road from the entry to the exit, no junction between: the markers strictly between the two points.
		const bool forward = fracOut >= fracIn;
		Edge e{nodeX, road.length * std::fabs(fracOut - fracIn), in.road, forward ? in.segment + 1 : in.segment, forward ? out.segment : out.segment + 1};
		if ((forward && out.segment < in.segment + 1) || (!forward && in.segment < out.segment + 1))
			e.road = -2; // no marker between
		adj[nodeE].push_back(e);
	}

	struct Open
	{
		double f;
		int node;
		bool operator<(const Open& o) const { return f > o.f; }
	};
	std::vector<double> g(pos.size(), std::numeric_limits<double>::max());
	std::vector<std::pair<int, int>> from(pos.size(), {-1, -1}); // previous node, edge index in its list
	std::priority_queue<Open> open;
	g[nodeE] = 0.0;
	open.push({dist(pos[nodeE], pos[nodeX]), nodeE});
	while (!open.empty())
	{
		const Open cur = open.top();
		open.pop();
		if (cur.node == nodeX)
			break;
		if (cur.f - dist(pos[cur.node], pos[nodeX]) > g[cur.node] + 1.0e-9)
			continue;
		for (std::size_t i = 0; i < adj[cur.node].size(); ++i)
		{
			const Edge& e = adj[cur.node][i];
			const double ng = g[cur.node] + e.cost;
			if (ng < g[e.to])
			{
				g[e.to] = ng;
				from[e.to] = {cur.node, static_cast<int>(i)};
				open.push({ng + dist(pos[e.to], pos[nodeX]), e.to});
			}
		}
	}
	if (g[nodeX] == std::numeric_limits<double>::max())
		return {};

	// The use test (14065f940): legs on foot plus the discounted road against the straight line.
	const double legs = std::hypot(in.point.x - fromX, in.point.z - fromZ) + std::hypot(out.point.x - toX, out.point.z - toZ);
	const double through = legs + g[nodeX] * factor;
	if (through * through > straight * straight)
		return {};

	std::vector<std::pair<int, int>> chain; // (node, edge index) walked, reversed
	for (int n = nodeX; from[n].first >= 0; n = from[n].first)
		chain.push_back(from[n]);
	std::reverse(chain.begin(), chain.end());
	std::vector<Marker> markers{in.point};
	for (const auto& [n, i] : chain)
	{
		const Edge& e = adj[n][i];
		if (e.road >= 0)
			appendMarkers(markers, roads_[e.road], e.a, e.b);
	}
	markers.push_back(out.point);
	// Drop the repeats at joints (a point twice in a row).
	markers.erase(std::unique(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.x == b.x && a.z == b.z; }), markers.end());
	return markers;
}

} // namespace kworld
