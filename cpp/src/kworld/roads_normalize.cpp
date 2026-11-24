#include "kworld/roads_normalize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <numeric>
#include <unordered_map>

namespace kworld {

namespace {

using Kind = RoadChange::Kind;
constexpr double kZone = RoadNormalizeResult::kZone;

struct Pt
{
	double x, z;
};

// One stop along a road: a marker, or a point where another road's marker touches a segment.
struct Entry
{
	int node;
	int marker; // -1: a touch point
	int segment;
	double t;
	double x, z;
};

struct Grid
{
	double cell;
	std::unordered_map<std::int64_t, std::vector<int>> cells;
	static std::int64_t key(std::int64_t cx, std::int64_t cz) { return cx * 1000003 + cz; }
	std::int64_t at(double v) const { return static_cast<std::int64_t>(std::floor(v / cell)); }
	void add(double x0, double z0, double x1, double z1, int id)
	{
		for (std::int64_t cx = at(std::min(x0, x1)); cx <= at(std::max(x0, x1)); ++cx)
			for (std::int64_t cz = at(std::min(z0, z1)); cz <= at(std::max(z0, z1)); ++cz)
				cells[key(cx, cz)].push_back(id);
	}
	// Ids whose box touches the cells within r of (x, z); may repeat none (callers dedupe by sort).
	std::vector<int> near(double x, double z, double r) const
	{
		std::vector<int> out;
		for (std::int64_t cx = at(x - r); cx <= at(x + r); ++cx)
			for (std::int64_t cz = at(z - r); cz <= at(z + r); ++cz)
			{
				const auto found = cells.find(key(cx, cz));
				if (found != cells.end())
					out.insert(out.end(), found->second.begin(), found->second.end());
			}
		std::sort(out.begin(), out.end());
		out.erase(std::unique(out.begin(), out.end()), out.end());
		return out;
	}
};

// Closest point of segment p..q to c; t in 0..1.
double toSegment(Pt c, Pt p, Pt q, double& t)
{
	const double sx = q.x - p.x, sz = q.z - p.z;
	const double len2 = sx * sx + sz * sz;
	t = len2 > 0.0 ? std::clamp(((c.x - p.x) * sx + (c.z - p.z) * sz) / len2, 0.0, 1.0) : 0.0;
	return std::hypot(p.x + sx * t - c.x, p.z + sz * t - c.z);
}

// The histogram bin of a distance under 200 u.
std::size_t bin(double d)
{
	std::size_t i = 0;
	while (i + 1 < RoadNormalizeResult::kBinEdge.size() && d >= RoadNormalizeResult::kBinEdge[i])
		++i;
	return i;
}

struct Dsu
{
	std::vector<int> parent;
	explicit Dsu(int n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
	int find(int a)
	{
		while (parent[a] != a)
			a = parent[a] = parent[parent[a]];
		return a;
	}
	void join(int a, int b)
	{
		a = find(a);
		b = find(b);
		if (a != b)
			parent[std::max(a, b)] = std::min(a, b); // the lowest marker id leads
	}
};

} // namespace

const char* roadChangeKindName(RoadChange::Kind kind)
{
	switch (kind)
	{
	case Kind::Repeat: return "repeat";
	case Kind::Overlap: return "overlap";
	case Kind::TTouch: return "t_touch";
	case Kind::Crossing: return "crossing";
	case Kind::NearMiss: return "near_miss";
	}
	return "?";
}

RoadNormalizeResult normalizeRoads(const RoadNetwork& net)
{
	RoadNormalizeResult result;
	const auto& roads = net.roads();
	const int nRoads = static_cast<int>(roads.size());

	// Global marker ids, road by road.
	std::vector<int> first(nRoads + 1, 0);
	for (int r = 0; r < nRoads; ++r)
		first[r + 1] = first[r] + static_cast<int>(roads[r].markers.size());
	const int nMarkers = first[nRoads];
	std::vector<int> roadOf(nMarkers), indexOf(nMarkers);
	std::vector<Pt> pos(nMarkers);
	for (int r = 0; r < nRoads; ++r)
		for (int i = 0; i < static_cast<int>(roads[r].markers.size()); ++i)
		{
			const int g = first[r] + i;
			roadOf[g] = r;
			indexOf[g] = i;
			pos[g] = {roads[r].markers[i].x, roads[r].markers[i].z};
		}
	const auto ref = [&](int g) { return RoadRef{roads[roadOf[g]].id, indexOf[g], -1, 0.0}; };
	const auto segRef = [&](int r, int s, double t) { return RoadRef{roads[r].id, -1, s, t}; };

	// (a) repeated points: zones overlapping (distance < 2 kZone) -> one node. Histogram counts every pair under 200 u.
	Dsu dsu(nMarkers);
	std::vector<RoadChange> repeats;
	{
		Grid grid{200.0, {}};
		for (int g = 0; g < nMarkers; ++g)
			grid.add(pos[g].x, pos[g].z, pos[g].x, pos[g].z, g);
		for (int g = 0; g < nMarkers; ++g)
			for (const int h : grid.near(pos[g].x, pos[g].z, 200.0))
			{
				if (h <= g)
					continue;
				const double d = std::hypot(pos[g].x - pos[h].x, pos[g].z - pos[h].z);
				if (d >= 200.0)
					continue;
				++(roadOf[g] == roadOf[h] ? result.pairsSame : result.pairsCross)[bin(d)];
				if (d < 2.0 * kZone)
				{
					dsu.join(g, h);
					RoadChange c;
					c.kind = Kind::Repeat;
					c.a = ref(g);
					c.b = ref(h);
					c.x = pos[h].x;
					c.z = pos[h].z;
					c.distance = d;
					repeats.push_back(std::move(c));
				}
			}
	}
	std::vector<int> node(nMarkers);
	for (int g = 0; g < nMarkers; ++g)
		node[g] = dsu.find(g);

	// (c) T-touches: a marker within kZone of another road's segment interior. Also the near misses (to the road width) and the histogram.
	std::vector<std::vector<std::vector<Entry>>> touches(nRoads); // [road][segment] -> touch points
	std::vector<RoadChange> tTouches, nearMisses;
	{
		Grid grid{500.0, {}};
		std::vector<std::pair<int, int>> segs; // (road, segment)
		for (int r = 0; r < nRoads; ++r)
		{
			touches[r].resize(roads[r].markers.size());
			for (int s = 0; s + 1 < static_cast<int>(roads[r].markers.size()); ++s)
			{
				const int g = first[r] + s;
				grid.add(pos[g].x, pos[g].z, pos[g + 1].x, pos[g + 1].z, static_cast<int>(segs.size()));
				segs.push_back({r, s});
			}
		}
		for (int g = 0; g < nMarkers; ++g)
		{
			const int r = roadOf[g];
			for (const int si : grid.near(pos[g].x, pos[g].z, std::max(roads[r].width, 200.0)))
			{
				const auto [r2, s] = segs[si];
				if (r2 == r)
					continue;
				const int g0 = first[r2] + s;
				double t = 0.0;
				const double d = toSegment(pos[g], pos[g0], pos[g0 + 1], t);
				if (d >= 200.0 || t <= 0.0 || t >= 1.0)
					continue;
				if (node[g] == node[g0] || node[g] == node[g0 + 1])
					continue; // already the same node as one end
				++result.markerSegment[bin(d)];
				// A marker whose zone reaches an end of the segment is a repeat (above), not a touch.
				const double dEnd = std::min(std::hypot(pos[g].x - pos[g0].x, pos[g].z - pos[g0].z), std::hypot(pos[g].x - pos[g0 + 1].x, pos[g].z - pos[g0 + 1].z));
				if (dEnd < 2.0 * kZone)
					continue;
				const double px = pos[g0].x + (pos[g0 + 1].x - pos[g0].x) * t, pz = pos[g0].z + (pos[g0 + 1].z - pos[g0].z) * t;
				if (d < kZone)
				{
					bool dup = false;
					for (const Entry& e : touches[r2][s])
						dup = dup || e.node == node[g];
					if (dup)
						continue; // another marker of the same node already splits this segment
					touches[r2][s].push_back({node[g], -1, s, t, pos[g].x, pos[g].z});
					RoadChange c;
					c.kind = Kind::TTouch;
					c.a = ref(g);
					c.b = segRef(r2, s, t);
					c.x = pos[g].x;
					c.z = pos[g].z;
					c.distance = d;
					tTouches.push_back(std::move(c));
				} else {
					const double w = std::min(roads[r].width, roads[r2].width);
					if (w > 0.0 && d < w)
					{
						RoadChange c;
						c.kind = Kind::NearMiss;
						c.a = ref(g);
						c.b = segRef(r2, s, t);
						c.x = pos[g].x;
						c.z = pos[g].z;
						c.distance = d;
						c.note = "to point " + std::to_string(px) + "," + std::to_string(pz);
						nearMisses.push_back(std::move(c));
					}
				}
			}
		}
	}

	// Each road as a node sequence: its markers with the touch points between them, consecutive repeats of a node dropped.
	std::vector<std::vector<Entry>> seq(nRoads);
	for (int r = 0; r < nRoads; ++r)
	{
		const auto push = [&](const Entry& e)
		{
			if (seq[r].empty() || seq[r].back().node != e.node)
				seq[r].push_back(e);
		};
		for (int i = 0; i < static_cast<int>(roads[r].markers.size()); ++i)
		{
			push({node[first[r] + i], i, -1, 0.0, pos[first[r] + i].x, pos[first[r] + i].z});
			auto& on = touches[r][i];
			std::sort(on.begin(), on.end(), [](const Entry& a, const Entry& b) { return a.t < b.t; });
			for (const Entry& e : on)
				push(e);
		}
	}

	// (b) overlaps: maximal runs of >= 2 consecutive shared nodes, same or reversed order, between two roads (road A < road B).
	std::vector<RoadChange> overlaps;
	{
		std::unordered_map<int, std::vector<std::pair<int, int>>> where; // node -> (road, position)
		for (int r = 0; r < nRoads; ++r)
			for (int p = 0; p < static_cast<int>(seq[r].size()); ++p)
				where[seq[r][p].node].push_back({r, p});
		const auto entryRef = [&](int r, const Entry& e) { return e.marker >= 0 ? RoadRef{roads[r].id, e.marker, -1, 0.0} : segRef(r, e.segment, e.t); };
		for (int ra = 0; ra < nRoads; ++ra)
			for (int p = 0; p + 1 < static_cast<int>(seq[ra].size()); ++p)
				for (const auto& [rb, q] : where[seq[ra][p].node])
				{
					if (rb <= ra)
						continue;
					const auto& sa = seq[ra];
					const auto& sb = seq[rb];
					for (const int dir : {1, -1})
					{
						const auto matches = [&](int pa, int qb) { return pa >= 0 && pa < static_cast<int>(sa.size()) && qb >= 0 && qb < static_cast<int>(sb.size()) && sa[pa].node == sb[qb].node; };
						if (!matches(p + 1, q + dir) || matches(p - 1, q - dir))
							continue; // no run here, or not its start
						int n = 1;
						while (matches(p + n, q + dir * n))
							++n;
						const int pe = p + n - 1, qe = q + dir * (n - 1);
						RoadChange c;
						c.kind = Kind::Overlap;
						c.a = entryRef(ra, sa[p]);
						c.aEnd = entryRef(ra, sa[pe]);
						c.b = entryRef(rb, sb[q]);
						c.bEnd = entryRef(rb, sb[qe]);
						c.x = sa[p].x;
						c.z = sa[p].z;
						c.x2 = sa[pe].x;
						c.z2 = sa[pe].z;
						c.nodes = n;
						c.reversed = dir < 0;
						const bool startA = p > 0, startB = q - dir >= 0 && q - dir < static_cast<int>(sb.size());
						const bool endA = pe + 1 < static_cast<int>(sa.size()), endB = qe + dir >= 0 && qe + dir < static_cast<int>(sb.size());
						c.note = std::string("start:") + (startA ? "A" : "") + (startB ? "B" : "") + (startA || startB ? "" : "-") + " end:" + (endA ? "A" : "") + (endB ? "B" : "") + (endA || endB ? "" : "-");
						overlaps.push_back(std::move(c));
					}
				}
	}

	// (d) crossings: two segments of different roads crossing mid-span, no shared node, every end more than kZone from the crossing.
	std::vector<RoadChange> crossings;
	{
		Grid grid{1000.0, {}};
		std::vector<std::pair<int, int>> segs;
		for (int r = 0; r < nRoads; ++r)
			for (int s = 0; s + 1 < static_cast<int>(roads[r].markers.size()); ++s)
			{
				const int g = first[r] + s;
				grid.add(pos[g].x, pos[g].z, pos[g + 1].x, pos[g + 1].z, static_cast<int>(segs.size()));
				segs.push_back({r, s});
			}
		for (int i = 0; i < static_cast<int>(segs.size()); ++i)
		{
			const auto [ra, sa] = segs[i];
			const int a0 = first[ra] + sa;
			std::vector<int> candidates;
			{
				const double x0 = std::min(pos[a0].x, pos[a0 + 1].x), x1 = std::max(pos[a0].x, pos[a0 + 1].x);
				const double z0 = std::min(pos[a0].z, pos[a0 + 1].z), z1 = std::max(pos[a0].z, pos[a0 + 1].z);
				for (std::int64_t cx = grid.at(x0); cx <= grid.at(x1); ++cx)
					for (std::int64_t cz = grid.at(z0); cz <= grid.at(z1); ++cz)
					{
						const auto found = grid.cells.find(Grid::key(cx, cz));
						if (found != grid.cells.end())
							candidates.insert(candidates.end(), found->second.begin(), found->second.end());
					}
				std::sort(candidates.begin(), candidates.end());
				candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			}
			for (const int j : candidates)
			{
				const auto [rb, sb] = segs[j];
				if (rb <= ra)
					continue;
				const int b0 = first[rb] + sb;
				if (node[a0] == node[b0] || node[a0] == node[b0 + 1] || node[a0 + 1] == node[b0] || node[a0 + 1] == node[b0 + 1])
					continue;
				const double ax = pos[a0 + 1].x - pos[a0].x, az = pos[a0 + 1].z - pos[a0].z;
				const double bx = pos[b0 + 1].x - pos[b0].x, bz = pos[b0 + 1].z - pos[b0].z;
				const double den = ax * bz - az * bx;
				if (den == 0.0)
					continue;
				const double ex = pos[b0].x - pos[a0].x, ez = pos[b0].z - pos[a0].z;
				const double ta = (ex * bz - ez * bx) / den, tb = (ex * az - ez * ax) / den;
				if (ta <= 0.0 || ta >= 1.0 || tb <= 0.0 || tb >= 1.0)
					continue;
				const double cx = pos[a0].x + ax * ta, cz = pos[a0].z + az * ta;
				bool close = false;
				for (const int g : {a0, a0 + 1, b0, b0 + 1})
					close = close || std::hypot(pos[g].x - cx, pos[g].z - cz) < kZone;
				if (close)
					continue;
				RoadChange c;
				c.kind = Kind::Crossing;
				c.a = segRef(ra, sa, ta);
				c.b = segRef(rb, sb, tb);
				c.x = cx;
				c.z = cz;
				c.note = "pending kenshi-rwh.305/306";
				crossings.push_back(std::move(c));
			}
		}
	}

	for (auto* list : {&repeats, &overlaps, &tTouches, &crossings, &nearMisses})
		for (RoadChange& c : *list)
		{
			++result.perKind[static_cast<std::size_t>(c.kind)];
			result.changes.push_back(std::move(c));
		}
	return result;
}

} // namespace kworld
