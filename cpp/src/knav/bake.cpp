#include "knav/navmesh.hpp"

#include "kworld/heightmap.hpp"

#include <DetourAlloc.h>
#include <DetourNavMeshBuilder.h>
#include <Recast.h>
#include <RecastAlloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <ostream>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>

namespace knav {

namespace {

constexpr int kMaxVertsPerPoly = 6;
constexpr float kMaxEdgeLength = 64.0f; // world units: long flat polys path badly
constexpr float kMaxSimplificationError = 1.3f; // voxels
constexpr int kMinRegionCells = 8 * 8;
constexpr int kMergeRegionCells = 20 * 20;
constexpr float kDetailErrorCells = 2.0f; // in cellHeight
constexpr float kSolidTopCells = 3.0f; // the top face this close to a solid's top stays walkable
// Stair and gate-ramp collision is a smooth slab (Hub gate A: 42 and 48 degrees), not steps, and
// characters climb it as stairs: its slope limit is not the ground's (Havok 40 degrees).
constexpr float kStairSlopeDegrees = 55.0f;

// Heightmap samples per terrain mesh vertex: 2 = the game's 36-unit height lattice.
constexpr int kTerrainStride = 2;
// A doorway's polygons: the door's footprint grown by this much, so the mat a traveller crosses is
// the door's thickness plus twice the margin deep (about the agent radius plus a voxel each side).
constexpr float kDoorMargin = 8.0f;
// The mat gives back what the house hull's fill took, from this far under the door's floor: a stair that climbs into the doorway keeps its
// slope across the mat's margin (tan 55 degrees, kStairSlopeDegrees), so a bare agent climb cut the run's upper spans away and left a gap between
// the stair and the doorway (Heft Longhouse, kenshi-k16.6.229).
constexpr float kMatStairDrop = kDoorMargin * 1.43f;
// Recast area ids are Area + 1 (0 is "null"); the doors of one tile take the ids after the last
// Area, in key order, so two doors that touch stay two regions. 63 is Recast's largest id.
constexpr int kFirstDoorArea = static_cast<int>(Area::Edge) + 2;
constexpr int kMaxTileDoors = RC_WALKABLE_AREA - kFirstDoorArea;


// Bake profile: nanoseconds per stage summed over threads. A tile "laps" from stage to stage.
enum Stage
{
	StGather, StFlat, StRasTerrain, StRasShapes, StVolumeFill, StFilters, StCompact, StMarkPoly, StWater,
	StDoors, StErode, StDistance, StRegions, StContours, StPolyMesh, StDetail, StDetour, StCount
};
constexpr const char* kStageNames[StCount] = { "gather terrain+shapes", "flat tile shortcut", "rasterize terrain", "rasterize shapes",
	"volume fill", "span filters", "compact", "mark solid footprints", "water areas", "doors", "erode",
	"distance field", "regions", "contours", "poly mesh", "detail mesh", "detour data" };
std::atomic<bool> gProfile{ std::getenv("KNAV_PROFILE") != nullptr };
std::array<std::atomic<std::uint64_t>, StCount> gStageNs{};
std::atomic<std::uint64_t> gProfileTiles{ 0 };
std::atomic<std::uint64_t> gFlatTiles{ 0 };
std::atomic<std::uint64_t> gBandlessTiles{ 0 };
std::atomic<std::uint64_t> gErodedTiles{ 0 };
BakeTuning gTuning;

struct Lap
{
	using Clock = std::chrono::steady_clock;
	bool on = gProfile.load(std::memory_order_relaxed);
	Clock::time_point last = on ? Clock::now() : Clock::time_point{};

	void operator()(Stage stage)
	{
		if (!on)
			return;
		const auto now = Clock::now();
		gStageNs[stage] += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - last).count());
		last = now;
	}
};

// One tile's bake: the Detour data and which polygons are door polygons.
struct BuiltTile
{
	std::vector<std::uint8_t> data;
	std::vector<DoorPoly> doors;
};

template <class T, void (*Free)(T*)>
struct Freer
{
	void operator()(T* p) const { Free(p); }
};

using HeightfieldPtr = std::unique_ptr<rcHeightfield, Freer<rcHeightfield, rcFreeHeightField>>;
using CompactPtr = std::unique_ptr<rcCompactHeightfield, Freer<rcCompactHeightfield, rcFreeCompactHeightfield>>;
using ContoursPtr = std::unique_ptr<rcContourSet, Freer<rcContourSet, rcFreeContourSet>>;
using PolyMeshPtr = std::unique_ptr<rcPolyMesh, Freer<rcPolyMesh, rcFreePolyMesh>>;
using DetailPtr = std::unique_ptr<rcPolyMeshDetail, Freer<rcPolyMeshDetail, rcFreePolyMeshDetail>>;

// Convex hull (monotone chain) of the xz projection; counter-clockwise from
// above. Returns xyz triples (y = 0) in `out`.
// With a margin, the hull of the shape grown by it on every side (Minkowski sum with a square; `round`: with an octagon whose
// sides are `margin` away, so a body radius is cleared along the sides and round a corner, kenshi-k16.7.89).
void footprint(const kworld::WorldShape& shape, std::vector<float>& out, float margin = 0.0f, bool round = false)
{
	std::vector<std::array<float, 2>> points;
	for (std::size_t i = 0; i + 2 < shape.vertices.size(); i += 3)
	{
		const float x = shape.vertices[i];
		const float z = shape.vertices[i + 2];
		if (margin <= 0.0f)
			points.push_back({ x, z });
		else if (round)
			for (int k = 0; k < 8; ++k)
			{
				const float angle = (static_cast<float>(k) + 0.5f) * 0.78539816f;
				const float reach = margin / 0.92387953f; // cos 22.5 degrees
				points.push_back({ x + reach * std::cos(angle), z + reach * std::sin(angle) });
			}
		else
			for (const float dx : { -margin, margin })
				for (const float dz : { -margin, margin })
					points.push_back({ x + dx, z + dz });
	}
	std::sort(points.begin(), points.end());
	points.erase(std::unique(points.begin(), points.end()), points.end());
	out.clear();
	if (points.size() < 3)
		return;
	const auto cross = [](const auto& o, const auto& a, const auto& b)
	{
		return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
	};
	std::vector<std::array<float, 2>> hull(2 * points.size());
	std::size_t n = 0;
	for (std::size_t i = 0; i < points.size(); ++i)
	{
		while (n >= 2 && cross(hull[n - 2], hull[n - 1], points[i]) <= 0)
			--n;
		hull[n++] = points[i];
	}
	for (std::size_t i = points.size() - 1, floor = n + 1; i-- > 0;)
	{
		while (n >= floor && cross(hull[n - 2], hull[n - 1], points[i]) <= 0)
			--n;
		hull[n++] = points[i];
	}
	for (std::size_t i = 0; i + 1 < n; ++i)
	{
		out.push_back(hull[i][0]);
		out.push_back(0.0f);
		out.push_back(hull[i][1]);
	}
}

// Whether (x, z) lies in a footprint() hull (counter-clockwise from above, edges included).
bool insideConvex(const std::vector<float>& hull, float x, float z)
{
	const std::size_t n = hull.size() / 3;
	for (std::size_t i = 0; i < n; ++i)
	{
		const float* a = &hull[i * 3];
		const float* b = &hull[(i + 1) % n * 3];
		if ((b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]) < 0.0f)
			return false;
	}
	return true;
}

// How far outside its region (WorldShape::keepInside) a clipped shape's surface may lie and stay: a floor flush with the
// mask hull's face must not flicker on voxel rounding.
constexpr float kClipTolerance = 2.0f;

// Recast's ledge test without its neighbour-spread test (BakeLedge::Drops): a walkable span is dropped when a neighbour it
// could step to with headroom (or the grid's edge) lies more than a climb below it. Our own code of the same rule.
void filterDrops(int walkableHeight, int walkableClimb, rcHeightfield& hf)
{
	const int kMax = RC_SPAN_MAX_HEIGHT;
	for (int z = 0; z < hf.height; ++z)
	{
		for (int x = 0; x < hf.width; ++x)
		{
			for (rcSpan* s = hf.spans[x + z * hf.width]; s; s = s->next)
			{
				if (s->area == RC_NULL_AREA)
					continue;
				const int bot = static_cast<int>(s->smax);
				const int top = s->next ? static_cast<int>(s->next->smin) : kMax;
				int lowest = kMax;
				for (int dir = 0; dir < 4; ++dir)
				{
					const int nx = x + rcGetDirOffsetX(dir), nz = z + rcGetDirOffsetY(dir);
					if (nx < 0 || nz < 0 || nx >= hf.width || nz >= hf.height)
					{
						lowest = std::min(lowest, -walkableClimb - bot);
						continue;
					}
					const rcSpan* n = hf.spans[nx + nz * hf.width];
					// The space under the column's first span.
					if (std::min(top, n ? static_cast<int>(n->smin) : kMax) - std::max(bot, -walkableClimb) > walkableHeight)
						lowest = std::min(lowest, -walkableClimb - bot);
					for (; n; n = n->next)
					{
						const int nbot = static_cast<int>(n->smax);
						const int ntop = n->next ? static_cast<int>(n->next->smin) : kMax;
						if (std::min(top, ntop) - std::max(bot, nbot) > walkableHeight)
							lowest = std::min(lowest, nbot - bot);
					}
				}
				if (lowest < -walkableClimb)
					s->area = RC_NULL_AREA;
			}
		}
	}
}

// A watershed region may wrap round onto itself: an Area::Edge strip has no distance-field interior, so one region floods
// a stair's band and the floor band above it. Recast only logs that ("overlapping regions") and its contours then drop the
// polygons (bad triangulation). Each region is split into connected pieces holding at most one span per column; the first
// piece keeps the region's id, so a bake without overlaps is unchanged. Should the ids run out the rest stays as Recast left it.
void splitOverlappingRegions(rcCompactHeightfield& chf)
{
	std::vector<int> column(static_cast<std::size_t>(chf.spanCount));
	for (int c = 0; c < chf.width * chf.height; ++c)
		for (unsigned int i = chf.cells[c].index; i < chf.cells[c].index + static_cast<unsigned int>(chf.cells[c].count); ++i)
			column[i] = c;
	std::vector<int> stamp(static_cast<std::size_t>(chf.width * chf.height), -1); // the piece that last took a column
	std::vector<bool> done(static_cast<std::size_t>(chf.spanCount));
	std::vector<bool> seen(RC_BORDER_REG);
	std::vector<int> queue; // breadth first: compact pieces (depth first interleaves them along a slope)
	int next = chf.maxRegions;
	int piece = 0;
	for (int start = 0; start < chf.spanCount; ++start)
	{
		const unsigned short region = chf.spans[start].reg;
		if (region == 0 || (region & RC_BORDER_REG) || done[static_cast<std::size_t>(start)])
			continue;
		const unsigned short id = seen[region] ? static_cast<unsigned short>(next++) : region;
		if (next >= RC_BORDER_REG)
			break;
		seen[region] = true;
		done[static_cast<std::size_t>(start)] = true;
		stamp[static_cast<std::size_t>(column[static_cast<std::size_t>(start)])] = piece;
		chf.spans[start].reg = id;
		queue.assign(1, start);
		for (std::size_t head = 0; head < queue.size(); ++head)
		{
			const int i = queue[head];
			const rcCompactSpan& span = chf.spans[i];
			const int x = column[static_cast<std::size_t>(i)] % chf.width;
			const int z = column[static_cast<std::size_t>(i)] / chf.width;
			for (int dir = 0; dir < 4; ++dir)
			{
				if (rcGetCon(span, dir) == RC_NOT_CONNECTED)
					continue;
				const int cell = x + rcGetDirOffsetX(dir) + (z + rcGetDirOffsetY(dir)) * chf.width;
				const int neighbour = static_cast<int>(chf.cells[cell].index) + rcGetCon(span, dir);
				if (done[static_cast<std::size_t>(neighbour)] || chf.spans[neighbour].reg != region || stamp[static_cast<std::size_t>(cell)] == piece)
					continue;
				done[static_cast<std::size_t>(neighbour)] = true;
				stamp[static_cast<std::size_t>(cell)] = piece;
				chf.spans[neighbour].reg = id;
				queue.push_back(neighbour);
			}
		}
		++piece;
	}
	chf.maxRegions = next;
}

// A use node's reach: the spans this close to a prop's own use node stay walkable through the prop's body clearance.
constexpr float kUseReach = 4.0f;
constexpr int kGapCells = 4; // the least free lane (cells, 4 u) a furniture ring leaves between two blockers: the mesh is the walker's centre space already (walls eroded by the 4 u radius), and a 1-cell lane collapses in the contour simplification


constexpr float kRockMaxSpan = 750.0f; // a shell wider than this (either axis) is cut per triangle height (kenshi-k16.7.59)

// A foliage rock: an open obstacle shell (kworld addFoliage sets `solid`). Its low-poly edges sit within a climb
// of the ground, so rasterised alone they are standable; it cuts the xz cover of its triangles (not their hull) like a closed obstacle.
bool rockShell(const kworld::WorldShape& shape)
{
	return shape.use == kworld::NavUse::Obstacle && !shape.closed && shape.solid;
}

// A closed obstacle hull whose side faces lean enough that its footprint prism would reach well
// past the hull at most heights (a battered tower wall: the inner face of the Watchtower ring wall
// moves 17 units inward over 280, so the prism of the whole hull covers the staircase beside it). Such
// hulls are filled by column like stair solids instead, which is exact for a convex hull.
bool leansOver(const kworld::WorldShape& shape)
{
	if (shape.use != kworld::NavUse::Obstacle || !shape.closed)
		return false;
	constexpr float kLeanTolerance = 4.0f; // the agent radius: a prism error beyond this blocks a walker
	const auto vertex = [&](std::uint32_t index, std::size_t axis) { return shape.vertices[index * 3 + axis]; };
	for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
	{
		const std::uint32_t a = shape.indices[t];
		const std::uint32_t b = shape.indices[t + 1];
		const std::uint32_t c = shape.indices[t + 2];
		const float e1[3] = { vertex(b, 0) - vertex(a, 0), vertex(b, 1) - vertex(a, 1), vertex(b, 2) - vertex(a, 2) };
		const float e2[3] = { vertex(c, 0) - vertex(a, 0), vertex(c, 1) - vertex(a, 1), vertex(c, 2) - vertex(a, 2) };
		const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length < 1e-6f)
			continue;
		const float ny = std::fabs(n[1]) / length;
		if (ny < 0.01f || ny > 0.7f) // upright side (or a near-horizontal top / bottom)
			continue;
		// Horizontal run of the face over its height.
		const float low = std::min({ vertex(a, 1), vertex(b, 1), vertex(c, 1) });
		const float high = std::max({ vertex(a, 1), vertex(b, 1), vertex(c, 1) });
		if ((high - low) * ny / std::sqrt(1.0f - ny * ny) > kLeanTolerance)
			return true;
	}
	return false;
}

// A closed walkable furniture hull (a wall piece, shelf, table, counter top: never stepped onto from the ground) whose footprint fill
// (fillShapes) cuts exactly the cells whose centre lies inside it. Rasterising its side sheets as well takes every floor cell the sheet
// merely touches, up to one 2 u cell on each side of a gap between two hulls: an 11 u passage between a wall (2172) and a shelf (2594) came
// out 6.75 u wide (Heft Noble House, kenshi-k16.7.71). The centre-based fill is then the single rule: the hull keeps its top faces and loses
// its side sheets. A hull under two cells in x or z may have no cell centre inside it, so it keeps its sheets. Not for any other closed
// hull. A building's floor slab (Squin Bar, General Shop) is walked onto, and its sheets are the step from the ground to its top face:
// dropping them cut the doorstep off. A closed obstacle hull keeps its sheets as the keep-out of a solid: without them fighters stood
// inside props (ksim_fight_prop, ksim_route_corners, ksim_storm_door), though dropping them also opens the Slave Shop's listed door.
bool sheetsCoveredByFill(const kworld::WorldShape& shape, float cellSize)
{
	if (!shape.furniture || !shape.closed || shape.solid || shape.foliage || shape.use != kworld::NavUse::Walkable)
		return false;
	return std::min(shape.max[0] - shape.min[0], shape.max[2] - shape.min[2]) >= 2.0f * cellSize;
}

// The steepest face a walkable shape's surface may carry: a stair or ramp slab (a closed solid hull) is climbable to kStairSlopeDegrees.
float shapeSlope(const kworld::WorldShape& shape, float walkSlope)
{
	return shape.solid ? std::max(walkSlope, kStairSlopeDegrees) : walkSlope;
}

// The upward component of triangle t's unit normal (Recast's winding), 0 for a degenerate one.
float triangleUp(const kworld::WorldShape& shape, std::size_t t)
{
	const float* a = &shape.vertices[shape.indices[t * 3] * 3];
	const float* b = &shape.vertices[shape.indices[t * 3 + 1] * 3];
	const float* c = &shape.vertices[shape.indices[t * 3 + 2] * 3];
	const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
	const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	return length < 1e-6f ? 0.0f : n[1] / length;
}

// The ramp faces of an open walkable sheet (a wall-walk building's collision mesh, k16.7.58): faces steeper than the walk limit and
// within the stair limit that form a strip (faces sharing vertices) whose top edge and bottom edge both meet a flat walkable face
// of the same sheet: a run from one walkable level to another. The original walks those (the 14535 wall-walk ramp, 47 degrees);
// measured over the world against its shipped tiles (knav_slope_probe): such strips are walked where their end faces are (87% of
// their area), while every steep face of an open sheet is not (20% on buildings, 6% on foliage rocks, which are skipped). A roof's
// pitch also joins two flat faces; it only joins what the roof's own flat faces already are. Empty: the shape has none.
std::vector<char> rampTriangles(const kworld::WorldShape& shape, float walkSlope)
{
	if (shape.foliage || shape.closed || shape.solid || shape.use != kworld::NavUse::Walkable)
		return {};
	using Key = std::array<long, 3>;
	const float cosWalk = std::cos(walkSlope * 3.14159265f / 180.0f);
	const float cosStair = std::cos(kStairSlopeDegrees * 3.14159265f / 180.0f);
	const std::size_t count = shape.indices.size() / 3;
	const auto corner = [&](std::size_t tri, std::size_t k) { return &shape.vertices[shape.indices[tri * 3 + k] * 3]; };
	const auto keyOf = [](const float* v) { return Key{ std::lround(v[0] * 4.0f), std::lround(v[1] * 4.0f), std::lround(v[2] * 4.0f) }; };
	std::vector<char> band(count, 0);
	std::vector<std::size_t> parent(count);
	std::iota(parent.begin(), parent.end(), std::size_t{ 0 });
	const auto find = [&](std::size_t i)
	{
		while (parent[i] != i)
			i = parent[i] = parent[parent[i]];
		return i;
	};
	std::map<Key, std::size_t> owner;
	std::set<Key> flat;
	bool any = false;
	for (std::size_t t = 0; t < count; ++t)
	{
		const float up = triangleUp(shape, t);
		if (up == 0.0f)
			continue;
		band[t] = up > cosStair && up <= cosWalk;
		any = any || band[t];
		for (std::size_t k = 0; k < 3; ++k)
		{
			if (up > cosWalk)
				flat.insert(keyOf(corner(t, k)));
			else if (band[t])
			{
				const auto [it, fresh] = owner.emplace(keyOf(corner(t, k)), t);
				if (!fresh)
					parent[find(t)] = find(it->second);
			}
		}
	}
	if (!any)
		return {};
	struct Strip
	{
		float low = 1e30f;
		float high = -1e30f;
	};
	std::map<std::size_t, Strip> strips;
	for (std::size_t t = 0; t < count; ++t)
	{
		if (!band[t])
			continue;
		Strip& strip = strips[find(t)];
		for (std::size_t k = 0; k < 3; ++k)
		{
			strip.low = std::min(strip.low, corner(t, k)[1]);
			strip.high = std::max(strip.high, corner(t, k)[1]);
		}
	}
	std::map<std::size_t, std::array<bool, 2>> ends; // per strip: a flat face at the bottom, at the top
	for (std::size_t t = 0; t < count; ++t)
	{
		if (!band[t])
			continue;
		const Strip& strip = strips[find(t)];
		for (std::size_t k = 0; k < 3; ++k)
		{
			const float* v = corner(t, k);
			if (flat.count(keyOf(v)) == 0)
				continue;
			std::array<bool, 2>& end = ends[find(t)];
			end[0] = end[0] || v[1] <= strip.low + 1.0f;
			end[1] = end[1] || v[1] >= strip.high - 1.0f;
		}
	}
	std::vector<char> ramp(count, 0);
	bool anyRamp = false;
	for (std::size_t t = 0; t < count; ++t)
	{
		if (!band[t])
			continue;
		const auto end = ends.find(find(t));
		ramp[t] = end != ends.end() && end->second[0] && end->second[1];
		anyRamp = anyRamp || ramp[t];
	}
	return anyRamp ? ramp : std::vector<char>{};
}

// Where the footprint fill of a closed walkable hull stops: the lowest point of its walkable top
// (faces within the walk slope of level), not its highest vertex. The fill is a prism with one top
// for the whole footprint, so on a hull the building's tilt has leaned (Hub buildings are
// terrain-aligned, up to 5 degrees: a 180-unit floor slab spans 15 units of height) a prism ending
// below the highest vertex would also null the top face where it is lower. Ends at `shape.max[1]`
// for a hull with no such face.
float walkableTopLow(const kworld::WorldShape& shape)
{
	constexpr float kMinUp = 0.766f; // cos 40 degrees: the walk slope
	float low = shape.max[1];
	bool any = false;
	const auto vertex = [&](std::uint32_t index, std::size_t axis) { return shape.vertices[index * 3 + axis]; };
	float centre[3] = { 0, 0, 0 };
	const std::size_t count = shape.vertices.size() / 3;
	for (std::size_t i = 0; i < count; ++i)
		for (std::size_t axis = 0; axis < 3; ++axis)
			centre[axis] += shape.vertices[i * 3 + axis] / static_cast<float>(count);
	for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
	{
		const std::uint32_t a = shape.indices[t];
		const std::uint32_t b = shape.indices[t + 1];
		const std::uint32_t c = shape.indices[t + 2];
		const float e1[3] = { vertex(b, 0) - vertex(a, 0), vertex(b, 1) - vertex(a, 1), vertex(b, 2) - vertex(a, 2) };
		const float e2[3] = { vertex(c, 0) - vertex(a, 0), vertex(c, 1) - vertex(a, 1), vertex(c, 2) - vertex(a, 2) };
		const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length < 1e-6f)
			continue;
		// Cooked hulls do not all wind the same way: orient the normal away from the centroid.
		float outward = 0.0f;
		for (std::size_t axis = 0; axis < 3; ++axis)
			outward += n[axis] * (vertex(a, axis) - centre[axis]);
		if ((outward < 0 ? -n[1] : n[1]) / length < kMinUp)
			continue;
		const float faceLow = std::min({ vertex(a, 1), vertex(b, 1), vertex(c, 1) });
		low = any ? std::min(low, faceLow) : faceLow;
		any = true;
	}
	return low;
}

// What one tile rasterises: terrain grid and the shapes in reach, with the
// vertical range they span.
struct TileInput
{
	float bmin[3];
	float bmax[3];
	std::vector<float> terrainVerts;
	std::vector<int> terrainTris;
	std::vector<const kworld::WorldShape*> shapes;
	float low = 0.0f; // the vertical range of the terrain and shapes
	float high = 0.0f;
	bool covered = false; // the terrain lattice spans the whole box (not a tile at the world's edge)
};

TileInput gatherTile(const NavConfig& c, const kworld::Heightmap& terrain,
	const std::vector<const kworld::WorldShape*>& shapes, int tx, int tz, float border)
{
	TileInput in;
	in.bmin[0] = static_cast<float>(c.originX + tx * c.tileSize) - border;
	in.bmin[2] = static_cast<float>(c.originZ + tz * c.tileSize) - border;
	in.bmax[0] = in.bmin[0] + c.tileSize + 2 * border;
	in.bmax[2] = in.bmin[2] + c.tileSize + 2 * border;
	float low = 1e30f;
	float high = -1e30f;

	// Terrain: the vertices of the game's 36-unit height lattice (every second heightmap sample)
	// covering the box, quads split along the (0,0)-(1,1) diagonal as the game's slope normals are
	// (FOLIAGE_EXE.md). The full 18-unit grid makes a 40-50 degree scarp outside Hub gate B that the
	// lattice smooths below 40.
	const double spacing = kworld::kHeightmapSpacing * kTerrainStride;
	const int lastCol = (terrain.sampleCountX() - 1) / kTerrainStride;
	const int lastRow = (terrain.sampleCountZ() - 1) / kTerrainStride;
	const int col0 = std::clamp(static_cast<int>(std::floor((in.bmin[0] - kworld::kWorldMin) / spacing)), 0, lastCol);
	const int col1 = std::clamp(static_cast<int>(std::ceil((in.bmax[0] - kworld::kWorldMin) / spacing)), 0, lastCol);
	const int row0 = std::clamp(static_cast<int>(std::floor((in.bmin[2] - kworld::kWorldMin) / spacing)), 0, lastRow);
	const int row1 = std::clamp(static_cast<int>(std::ceil((in.bmax[2] - kworld::kWorldMin) / spacing)), 0, lastRow);
	const int cols = col1 - col0 + 1;
	for (int row = row0; row <= row1; ++row)
	{
		for (int col = col0; col <= col1; ++col)
		{
			const auto y = static_cast<float>(terrain.sampleHeight(col * kTerrainStride, row * kTerrainStride));
			low = std::min(low, y);
			high = std::max(high, y);
			in.terrainVerts.push_back(static_cast<float>(kworld::kWorldMin + spacing * col));
			in.terrainVerts.push_back(y);
			in.terrainVerts.push_back(static_cast<float>(kworld::kWorldMin + spacing * row));
		}
	}
	for (int row = 0; row + 1 < row1 - row0 + 1; ++row)
	{
		for (int col = 0; col + 1 < cols; ++col)
		{
			const int v00 = row * cols + col;
			const int v10 = v00 + 1;
			const int v01 = v00 + cols;
			const int v11 = v01 + 1;
			for (const int index : { v00, v01, v11, v00, v11, v10 })
				in.terrainTris.push_back(index);
		}
	}

	for (const kworld::WorldShape* shape : shapes)
	{
		if (shape->max[0] < in.bmin[0] || shape->min[0] > in.bmax[0]
			|| shape->max[2] < in.bmin[2] || shape->min[2] > in.bmax[2])
			continue;
		in.shapes.push_back(shape);
		low = std::min(low, shape->min[1]);
		high = std::max(high, shape->max[1]);
	}
	in.bmin[1] = low - 2 * c.cellHeight;
	in.bmax[1] = high + c.agentHeight;
	in.low = low;
	in.high = high;
	in.covered = kworld::kWorldMin + spacing * col0 <= in.bmin[0] && kworld::kWorldMin + spacing * col1 >= in.bmax[0]
		&& kworld::kWorldMin + spacing * row0 <= in.bmin[2] && kworld::kWorldMin + spacing * row1 >= in.bmax[2];
	return in;
}

// The Detour tile of a finished polygon mesh: door polygons are split out and the area ids shifted
// back (Recast's area 0 is "null"), and each polygon gets its area's flag bit.
std::optional<BuiltTile> finishTile(const NavConfig& c, const rcConfig& cfg, rcPolyMesh& pmesh,
	const rcPolyMeshDetail& dmesh, const std::vector<kworld::DoorKey>& tileDoors, int tx, int tz)
{
	BuiltTile built;
	for (int i = 0; i < pmesh.npolys; ++i)
	{
		const int id = pmesh.areas[i];
		if (id >= kFirstDoorArea)
		{
			built.doors.push_back({ static_cast<std::uint16_t>(i), tileDoors[static_cast<std::size_t>(id - kFirstDoorArea)] });
			pmesh.areas[i] = static_cast<unsigned char>(Area::DoorOpen);
		} else {
			pmesh.areas[i] = static_cast<unsigned char>(id - 1);
		}
		pmesh.flags[i] = areaFlag(static_cast<Area>(pmesh.areas[i]));
	}

	dtNavMeshCreateParams params{};
	params.verts = pmesh.verts;
	params.vertCount = pmesh.nverts;
	params.polys = pmesh.polys;
	params.polyAreas = pmesh.areas;
	params.polyFlags = pmesh.flags;
	params.polyCount = pmesh.npolys;
	params.nvp = pmesh.nvp;
	params.detailMeshes = dmesh.meshes;
	params.detailVerts = dmesh.verts;
	params.detailVertsCount = dmesh.nverts;
	params.detailTris = dmesh.tris;
	params.detailTriCount = dmesh.ntris;
	params.tileX = tx;
	params.tileY = tz;
	std::copy(pmesh.bmin, pmesh.bmin + 3, params.bmin);
	std::copy(pmesh.bmax, pmesh.bmax + 3, params.bmax);
	params.walkableHeight = c.agentHeight;
	params.walkableRadius = c.agentRadius;
	params.walkableClimb = c.agentClimb;
	params.cs = cfg.cs;
	params.ch = cfg.ch;
	params.buildBvTree = true;

	unsigned char* data = nullptr;
	int size = 0;
	if (!dtCreateNavMeshData(&params, &data, &size))
		return std::nullopt;
	built.data.assign(data, data + size);
	dtFree(data);
	return built;
}

// Recast's voxel extent of a flat surface at height y over a box floor `floor`
// (rcRasterizeTriangles: floor and ceil of the height above it, at least one voxel thick).
struct SpanCells
{
	int min = 0;
	int max = 0;
	bool operator==(const SpanCells&) const = default;
};

SpanCells spanCells(float y, float floor, float inverseCellHeight)
{
	const float above = y - floor;
	SpanCells cells;
	cells.min = std::clamp(static_cast<int>(std::floor(above * inverseCellHeight)), 0, RC_SPAN_MAX_HEIGHT);
	cells.max = std::clamp(static_cast<int>(std::ceil(above * inverseCellHeight)), cells.min + 1, RC_SPAN_MAX_HEIGHT);
	return cells;
}

// A tile whose box holds no shape and whose terrain is one flat sheet (the open sea floor, a plateau)
// meshes as one quad over the tile, and the Recast pipeline would only rediscover that: the same
// quad at the same height, tagged with the area its depth gives, with the detail mesh of a four-corner
// polygon. Built by hand here, byte for byte what the pipeline produces (ctest knav_bake_flat).
// Null when the tile is not such a tile.
std::optional<BuiltTile> buildFlatTile(const NavConfig& c, const rcConfig& cfg, const TileInput& in, int tx, int tz)
{
	constexpr float kFlatTolerance = 0.5f; // terrain height range, world units
	constexpr float kRoundingMargin = 1e-3f; // heights interpolated across a triangle are exact to this
	if (!in.shapes.empty() || !in.covered || in.high - in.low > kFlatTolerance)
		return std::nullopt;
	const float inverse = 1.0f / cfg.ch;
	const SpanCells cells = spanCells(in.low, in.bmin[1], inverse);
	if (in.high != in.low)
	{
		// Every height between low and high must land in the same voxels, rounding included.
		for (const float y : { in.low - kRoundingMargin, in.high, in.high + kRoundingMargin })
			if (spanCells(y, in.bmin[1], inverse) != cells)
				return std::nullopt;
	}

	// The water rule of buildTile, for the one surface height there is.
	const float depth = c.waterLevel - (in.bmin[1] + static_cast<float>(cells.max) * cfg.ch);
	const Area area = depth >= c.swimDepth ? Area::Swim : depth >= c.wadeDepth ? Area::Shallow : Area::Land;

	const auto top = static_cast<unsigned short>(cells.max);
	const auto side = static_cast<unsigned short>(cfg.tileSize);
	const std::array<unsigned short, 12> verts = { 0, top, side, side, top, side, side, top, 0, 0, top, 0 };
	// Corner order and portal sides (0x8000 | direction of the tile edge) as Recast emits them.
	const std::array<unsigned short, 12> polys = { 0, 1, 2, 3, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX,
		0x8001, 0x8002, 0x8003, 0x8000, RC_MESH_NULL_IDX, RC_MESH_NULL_IDX };
	const unsigned char areas = static_cast<unsigned char>(static_cast<int>(area) + 1);
	const unsigned short flags = 0;
	const std::array<unsigned int, 4> detailMeshes = { 0, 4, 0, 2 };
	const std::array<unsigned char, 8> detailTris = { 3, 0, 2, 17, 0, 1, 2, 5 };

	// The meshes own their arrays (rcFreePolyMesh frees them), so they are copies.
	const auto copy = [](const auto& array)
	{
		using T = typename std::decay_t<decltype(array)>::value_type;
		T* out = static_cast<T*>(rcAlloc(sizeof(T) * array.size(), RC_ALLOC_PERM));
		std::copy(array.begin(), array.end(), out);
		return out;
	};
	const float pad = cfg.borderSize * cfg.cs;
	PolyMeshPtr pmesh(rcAllocPolyMesh());
	pmesh->verts = copy(verts);
	pmesh->polys = copy(polys);
	pmesh->areas = copy(std::array<unsigned char, 1>{ areas });
	pmesh->flags = copy(std::array<unsigned short, 1>{ flags });
	pmesh->nverts = 4;
	pmesh->npolys = 1;
	pmesh->nvp = cfg.maxVertsPerPoly;
	pmesh->bmin[0] = in.bmin[0] + pad;
	pmesh->bmin[1] = in.bmin[1];
	pmesh->bmin[2] = in.bmin[2] + pad;
	pmesh->bmax[0] = in.bmax[0] - pad;
	pmesh->bmax[1] = in.bmax[1] + static_cast<float>(cfg.walkableHeight) * cfg.ch;
	pmesh->bmax[2] = in.bmax[2] - pad;
	DetailPtr dmesh(rcAllocPolyMeshDetail());
	// The corners again, one voxel above the polygon's: Detour stores none of them (they are the
	// polygon's) but sizes the polygon's bounding-volume node from them.
	std::array<float, 12> detailVerts;
	for (int i = 0; i < 4; ++i)
	{
		detailVerts[i * 3 + 0] = pmesh->bmin[0] + static_cast<float>(verts[i * 3 + 0]) * cfg.cs;
		detailVerts[i * 3 + 1] = pmesh->bmin[1] + static_cast<float>(verts[i * 3 + 1]) * cfg.ch + cfg.ch;
		detailVerts[i * 3 + 2] = pmesh->bmin[2] + static_cast<float>(verts[i * 3 + 2]) * cfg.cs;
	}
	dmesh->meshes = copy(detailMeshes);
	dmesh->verts = copy(detailVerts);
	dmesh->tris = copy(detailTris);
	dmesh->nmeshes = 1;
	dmesh->nverts = 4;
	dmesh->ntris = 2;
	return finishTile(c, cfg, *pmesh, *dmesh, {}, tx, tz);
}

// What becomes of the spans within the agent radius of a wall. Edge is the bake; a tile it takes past kMaxPolysPerTile is
// baked again with Land, then with Eroded (the pre-v9 bake, which fits every tile of the world).
enum class WallBand
{
	Edge, // Area::Edge, its own polygons
	Land, // no band: one surface
	Eroded // cut, as Recast's erosion leaves it
};

// Terrain inside a building's interior volume is not part of the mesh (BUILDING `interior terrain` 0, WorldShape::carveTerrain; kenshi-rwh.90): the
// terrain spans whose surface lies inside one of the volume's convex boxes are unlinked, before the shapes go in, so a sunk building's floor stands alone and joins
// the door instead of hiding under the dune. Terrain height and collision elsewhere are untouched. One pass per distinct volume.
void carveInteriorTerrain(rcHeightfield& hf, const std::vector<const kworld::WorldShape*>& shapes)
{
	std::vector<const std::vector<std::vector<std::array<float, 4>>>*> done;
	for (const kworld::WorldShape* shape : shapes)
	{
		const auto* volumes = shape->carveTerrain.get();
		if (volumes == nullptr || std::find(done.begin(), done.end(), volumes) != done.end())
			continue;
		done.push_back(volumes);
		for (int z = 0; z < hf.height; ++z)
		{
			for (int x = 0; x < hf.width; ++x)
			{
				const float px = hf.bmin[0] + (static_cast<float>(x) + 0.5f) * hf.cs;
				const float pz = hf.bmin[2] + (static_cast<float>(z) + 0.5f) * hf.cs;
				for (rcSpan** link = &hf.spans[x + z * hf.width]; *link != nullptr;)
				{
					const float py = hf.bmin[1] + static_cast<float>((*link)->smax) * hf.ch;
					const bool inside = std::any_of(volumes->begin(), volumes->end(), [&](const std::vector<std::array<float, 4>>& planes)
					{
						return std::all_of(planes.begin(), planes.end(), [&](const std::array<float, 4>& plane)
						{
							return plane[0] * px + plane[1] * py + plane[2] * pz <= plane[3];
						});
					});
					if (inside)
						*link = (*link)->next;
					else
						link = &(*link)->next;
				}
			}
		}
	}
}

// The exact standing height. Recast keeps a span's top in whole voxels (the surface is the voxel's top, and the detail mesh lifts
// its vertices one voxel more), so a floor bakes up to two voxels above the collision and a tilted one is a flat sheet
// (kenshi-k16.7.36: the Storm House bar floor, 0.45..0.71 u above the hull; the original's mesh is 0.02..0.05 below it). A tile with
// shapes in it gets every vertex put back onto the highest walkable triangle under it. The polygon vertices go to 1/kHeightSteps
// voxel (Detour stores them as floats, so nothing reads the voxel back).
class HeightOracle
{
public:
	HeightOracle(const float bmin[3], const float bmax[3], float binSize)
		: x0_(bmin[0]), z0_(bmin[2]), bin_(binSize), nx_(static_cast<int>((bmax[0] - bmin[0]) / binSize) + 1),
		nz_(static_cast<int>((bmax[2] - bmin[2]) / binSize) + 1), bins_(static_cast<std::size_t>(nx_) * static_cast<std::size_t>(nz_))
	{
	}

	// The triangles whose normal is within `minUp` of vertical.
	void add(const float* verts, const int* indices, std::size_t triCount, float minUp)
	{
		for (std::size_t t = 0; t < triCount; ++t)
		{
			const float* a = verts + indices[t * 3] * 3;
			const float* b = verts + indices[t * 3 + 1] * 3;
			const float* c = verts + indices[t * 3 + 2] * 3;
			const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			const float ny = e1[2] * e2[0] - e1[0] * e2[2];
			const float nx = e1[1] * e2[2] - e1[2] * e2[1];
			const float nz = e1[0] * e2[1] - e1[1] * e2[0];
			const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (length < 1e-9f || std::fabs(ny) / length < minUp)
				continue;
			Tri tri;
			std::copy(a, a + 3, tri.p[0]);
			std::copy(b, b + 3, tri.p[1]);
			std::copy(c, c + 3, tri.p[2]);
			const int index = static_cast<int>(tris_.size());
			tris_.push_back(tri);
			const int bx0 = std::max(0, static_cast<int>((std::min({ a[0], b[0], c[0] }) - x0_) / bin_));
			const int bx1 = std::min(nx_ - 1, static_cast<int>((std::max({ a[0], b[0], c[0] }) - x0_) / bin_));
			const int bz0 = std::max(0, static_cast<int>((std::min({ a[2], b[2], c[2] }) - z0_) / bin_));
			const int bz1 = std::min(nz_ - 1, static_cast<int>((std::max({ a[2], b[2], c[2] }) - z0_) / bin_));
			for (int z = bz0; z <= bz1; ++z)
				for (int x = bx0; x <= bx1; ++x)
					bins_[static_cast<std::size_t>(z) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(x)].push_back(index);
		}
	}

	// The highest triangle surface at (x, z) within [y - below, y + above]; false when there is none.
	bool height(float x, float z, float y, float below, float above, float& out) const
	{
		const int bx = static_cast<int>((x - x0_) / bin_);
		const int bz = static_cast<int>((z - z0_) / bin_);
		if (bx < 0 || bz < 0 || bx >= nx_ || bz >= nz_)
			return false;
		constexpr float kEdge = 1e-4f; // a vertex on a shared triangle edge belongs to both
		bool found = false;
		for (const int index : bins_[static_cast<std::size_t>(bz) * static_cast<std::size_t>(nx_) + static_cast<std::size_t>(bx)])
		{
			const Tri& tri = tris_[static_cast<std::size_t>(index)];
			const float* a = tri.p[0];
			const float* b = tri.p[1];
			const float* c = tri.p[2];
			const float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
			if (std::fabs(det) < 1e-12f)
				continue;
			const float l0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det;
			const float l1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det;
			const float l2 = 1.0f - l0 - l1;
			if (l0 < -kEdge || l1 < -kEdge || l2 < -kEdge)
				continue;
			const float top = l0 * a[1] + l1 * b[1] + l2 * c[1];
			if (top < y - below || top > y + above || (found && top <= out))
				continue;
			out = top;
			found = true;
		}
		return found;
	}

private:
	struct Tri
	{
		float p[3][3];
	};
	float x0_;
	float z0_;
	float bin_;
	int nx_;
	int nz_;
	std::vector<Tri> tris_;
	std::vector<std::vector<int>> bins_;
};

constexpr int kHeightSteps = 10; // polygon vertex heights in 1/10 voxel after the exact-height pass

// Puts the tile's polygon and detail vertices on the walkable collision (see HeightOracle). Returns the voxel height the Detour data
// is to be made with.
float refineHeights(const rcConfig& cfg, const HeightOracle& oracle, rcPolyMesh& pmesh, rcPolyMeshDetail& dmesh)
{
	const float below = 2.0f * cfg.ch; // the voxel top and Recast's one more
	const float above = 0.5f * cfg.ch;
	const float fine = cfg.ch / static_cast<float>(kHeightSteps);
	if ((pmesh.bmax[1] - pmesh.bmin[1]) / fine > 60000.0f)
		return cfg.ch; // a tile too tall for 16-bit steps keeps the voxel heights
	for (int i = 0; i < pmesh.nverts; ++i)
	{
		unsigned short* v = &pmesh.verts[i * 3];
		const float wx = pmesh.bmin[0] + static_cast<float>(v[0]) * cfg.cs;
		const float wz = pmesh.bmin[2] + static_cast<float>(v[2]) * cfg.cs;
		float y = pmesh.bmin[1] + static_cast<float>(v[1]) * cfg.ch;
		float exact = 0.0f;
		if (oracle.height(wx, wz, y, below, above, exact))
			y = exact;
		v[1] = static_cast<unsigned short>(std::lround((y - pmesh.bmin[1]) / fine));
	}
	std::vector<char> supported(static_cast<std::size_t>(dmesh.nverts), 0);
	for (int i = 0; i < dmesh.nverts; ++i)
	{
		float* v = &dmesh.verts[i * 3];
		float exact = 0.0f;
		if (oracle.height(v[0], v[2], v[1], below, above, exact))
		{
			v[1] = exact;
			supported[static_cast<std::size_t>(i)] = 1;
		}
	}
	// A polygon's contour overhangs the edge it was simplified from, and Recast fills a detail vertex there from the nearest span of any
	// layer under it: a slab's edge band over a stair well got a vertex at the stair's height, a skirt 22 u down that a walker stands on
	// and falls off (the Hub bar stair, kenshi-k16.8.41). A detail vertex that the oracle found no collision under, and which is further than the agent's climb outside its polygon's own corner
	// heights, on a wall-band polygon whose corners are level to within the climb, takes the nearest corner height.
	const float climb = static_cast<float>(cfg.walkableClimb) * cfg.ch;
	for (int p = 0; p < pmesh.npolys; ++p)
	{
		const unsigned short* poly = &pmesh.polys[p * pmesh.nvp * 2];
		float lo = 1e30f;
		float hi = -1e30f;
		for (int j = 0; j < pmesh.nvp && poly[j] != RC_MESH_NULL_IDX; ++j)
		{
			const float y = pmesh.bmin[1] + static_cast<float>(pmesh.verts[poly[j] * 3 + 1]) * fine;
			lo = std::min(lo, y);
			hi = std::max(hi, y);
		}
		if (hi - lo > climb || pmesh.areas[p] != static_cast<unsigned char>(static_cast<int>(Area::Edge) + 1))
			continue; // a sloped polygon (a stair, a hillside) keeps what its samples say; only a wall band overhangs a drop
		const unsigned int* meta = &dmesh.meshes[p * 4];
		for (unsigned int k = 0; k < meta[1]; ++k)
		{
			if (supported[meta[0] + k])
				continue;
			float& y = dmesh.verts[(meta[0] + k) * 3 + 1];
			y = std::clamp(y, lo - climb, hi + climb);
		}
	}
	return fine;
}

std::optional<BuiltTile> buildTile(const NavConfig& c,
	const kworld::Heightmap& terrain, const std::vector<const kworld::WorldShape*>& shapes,
	int tx, int tz, WallBand band = WallBand::Edge)
{
	rcConfig cfg{};
	cfg.cs = c.cellSize;
	cfg.ch = c.cellHeight;
	cfg.walkableSlopeAngle = c.agentMaxSlope;
	cfg.walkableHeight = static_cast<int>(std::ceil(c.agentHeight / c.cellHeight));
	cfg.walkableClimb = static_cast<int>(std::floor(c.agentClimb / c.cellHeight));
	cfg.walkableRadius = static_cast<int>(std::ceil(c.agentRadius / c.cellSize));
	cfg.maxEdgeLen = static_cast<int>(kMaxEdgeLength / c.cellSize);
	cfg.maxSimplificationError = kMaxSimplificationError;
	cfg.minRegionArea = kMinRegionCells;
	cfg.mergeRegionArea = kMergeRegionCells;
	cfg.maxVertsPerPoly = kMaxVertsPerPoly;
	cfg.tileSize = static_cast<int>(std::lround(c.tileSize / c.cellSize));
	cfg.borderSize = cfg.walkableRadius + 3;
	cfg.width = cfg.tileSize + 2 * cfg.borderSize;
	cfg.height = cfg.width;
	cfg.detailSampleDist = c.cellSize * gTuning.detailSampleCells;
	cfg.detailSampleMaxError = c.cellHeight * kDetailErrorCells;

	Lap lap;
	const TileInput in = gatherTile(c, terrain, shapes, tx, tz, cfg.borderSize * c.cellSize);
	std::copy(in.bmin, in.bmin + 3, cfg.bmin);
	std::copy(in.bmax, in.bmax + 3, cfg.bmax);

	lap(StGather);
	if (gTuning.flatTiles)
	{
		if (auto flat = buildFlatTile(c, cfg, in, tx, tz))
		{
			lap(StFlat);
			++gProfileTiles;
			++gFlatTiles;
			return flat;
		}
		lap(StFlat);
	}
	rcContext ctx(false);
	HeightfieldPtr hf(rcAllocHeightfield());
	if (!rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
		return std::nullopt;

	std::vector<unsigned char> areas;
	bool dropSides = false;
	std::vector<int> topFaces;
	const auto rasterize = [&](const float* verts, int vertCount, const int* tris, int triCount, float slope, const std::vector<char>& ramp = {})
	{
		areas.assign(static_cast<std::size_t>(triCount), RC_NULL_AREA);
		rcMarkWalkableTriangles(&ctx, slope, verts, vertCount, tris, triCount, areas.data());
		for (std::size_t t = 0; t < ramp.size(); ++t)
			if (ramp[t])
				areas[t] = RC_WALKABLE_AREA;
		if (dropSides)
		{
			// Only the walkable faces (the top): the side sheets are the fill's.
			topFaces.clear();
			for (int t = 0; t < triCount; ++t)
				if (areas[static_cast<std::size_t>(t)] != RC_NULL_AREA)
					topFaces.insert(topFaces.end(), { tris[3 * t], tris[3 * t + 1], tris[3 * t + 2] });
			areas.assign(topFaces.size() / 3, RC_WALKABLE_AREA);
			rcRasterizeTriangles(&ctx, verts, vertCount, topFaces.data(), areas.data(), static_cast<int>(topFaces.size() / 3), *hf, cfg.walkableClimb);
			return;
		}
		rcRasterizeTriangles(&ctx, verts, vertCount, tris, areas.data(), triCount, *hf, cfg.walkableClimb);
	};
	rasterize(in.terrainVerts.data(), static_cast<int>(in.terrainVerts.size() / 3),
		in.terrainTris.data(), static_cast<int>(in.terrainTris.size() / 3), cfg.walkableSlopeAngle);
	carveInteriorTerrain(*hf, in.shapes);
	lap(StRasTerrain);
	std::vector<int> tris;
	for (const kworld::WorldShape* shape : in.shapes)
	{
		if (shape->use == kworld::NavUse::Projected || shape->use == kworld::NavUse::Door)
			continue;
		tris.assign(shape->indices.begin(), shape->indices.end());
		const int vertCount = static_cast<int>(shape->vertices.size() / 3);
		const int triCount = static_cast<int>(tris.size() / 3);
		const bool coveredByFill = sheetsCoveredByFill(*shape, cfg.cs);
		if (shape->use == kworld::NavUse::Obstacle)
		{
			// Solid, never standable: every triangle is unwalkable.
			areas.assign(static_cast<std::size_t>(triCount), RC_NULL_AREA);
			rcRasterizeTriangles(&ctx, shape->vertices.data(), vertCount, tris.data(), areas.data(),
				triCount, *hf, cfg.walkableClimb);
		} else {
			dropSides = coveredByFill;
			rasterize(shape->vertices.data(), vertCount, tris.data(), triCount,
				shapeSlope(*shape, cfg.walkableSlopeAngle), rampTriangles(*shape, cfg.walkableSlopeAngle));
			dropSides = false;
		}
	}

	lap(StRasShapes);
	// Volume fill. Recast shapes are thin sheets, so the ground inside a stair slab or a gate body
	// (buildings are sunk into the terrain) stays a walkable span between the sheets: a ledge that
	// the slab's top face has to climb, and a cut-off pocket under a deck. Fill each such convex
	// solid from its lowest to its highest sheet in every column it covers; the terrain inside is
	// absorbed, the top face keeps its own walkability, and the higher of terrain and slab wins.
	for (const kworld::WorldShape* shape : in.shapes)
	{
		const bool leaning = leansOver(*shape);
		if ((!shape->solid && !leaning) || rockShell(*shape))
			continue;
		HeightfieldPtr sheets(rcAllocHeightfield());
		if (!rcCreateHeightfield(&ctx, *sheets, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
			continue;
		tris.assign(shape->indices.begin(), shape->indices.end());
		const int triCount = static_cast<int>(tris.size() / 3);
		areas.assign(static_cast<std::size_t>(triCount), RC_NULL_AREA);
		if (!leaning)
			rcMarkWalkableTriangles(&ctx, std::max(cfg.walkableSlopeAngle, kStairSlopeDegrees), shape->vertices.data(),
				static_cast<int>(shape->vertices.size() / 3), tris.data(), triCount, areas.data());
		rcRasterizeTriangles(&ctx, shape->vertices.data(), static_cast<int>(shape->vertices.size() / 3),
			tris.data(), areas.data(), triCount, *sheets, cfg.walkableClimb);
		for (int i = 0; i < cfg.width * cfg.height; ++i)
		{
			const rcSpan* low = sheets->spans[i];
			if (!low)
				continue;
			const rcSpan* top = low;
			while (top->next)
				top = top->next;
			rcAddSpan(&ctx, *hf, i % cfg.width, i / cfg.width, static_cast<unsigned short>(low->smin),
				static_cast<unsigned short>(top->smax), static_cast<unsigned char>(top->area), cfg.walkableClimb);
		}
	}

	lap(StVolumeFill);
	rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
	if (gTuning.ledge == BakeLedge::Recast)
		rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
	else if (gTuning.ledge == BakeLedge::Drops)
		filterDrops(cfg.walkableHeight, cfg.walkableClimb, *hf);
	rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

	lap(StFilters);
	CompactPtr chf(rcAllocCompactHeightfield());
	if (!rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf))
		return std::nullopt;
	hf.reset();
	lap(StCompact);

	// Closed solids (boxes, hulls) fill their footprint prism. Walkable ones
	// stop just below their top face: the rasterised hull alone leaves the ground inside them
	// walkable, and the top face stays (floors, gate decks).
	// Obstacles fill to the top and above, so roofs are not standable.
	// Projected shapes cut their footprint through all heights, closed or not.
	// A house is a closed walkable hull without its doorway: its fill would bury the deck or
	// floor the door stands on. So the walkable hulls fill first, the door mats (the doors' grown
	// footprints in the height window) are then given back what the fill took from them, and the
	// obstacles and projected shapes fill last (the walls beside a door keep their ground).
	std::vector<float> hull;
	const auto fillShapes = [&](bool walkableHulls)
	{
		for (const kworld::WorldShape* shape : in.shapes)
		{
			const bool projected = shape->use == kworld::NavUse::Projected;
			if ((!shape->closed && !projected && !rockShell(*shape)) || leansOver(*shape))
				continue;
			if ((shape->use == kworld::NavUse::Walkable) != walkableHulls)
				continue;
			const float bottom = projected ? in.bmin[1] - 1.0f : shape->min[1] - c.cellHeight;
			const float top = projected ? in.bmax[1] + 1.0f
				: shape->use == kworld::NavUse::Obstacle ? shape->max[1] + 2 * c.cellHeight
				: walkableTopLow(*shape) - kSolidTopCells * c.cellHeight;
			if (rockShell(*shape))
			{
				// Each triangle cuts its own projection: the gaps between a pile's plates stay walkable (kenshi-k16.19.4).
				// A compact rock cuts it through the whole shell's height (nothing walkable inside it). A shell larger than
				// a rock (a swamp tree canopy, 1100 x 1900 u and 2400 tall at scale 4.25) cuts only the heights its own
				// triangle spans, as a voxelised mesh does: through the whole height it erased the floors of the Ancient
				// Lab under it (5312, zone 19,44) and split them into islands (kenshi-k16.7.59).
				const bool compact = shape->max[0] - shape->min[0] <= kRockMaxSpan && shape->max[2] - shape->min[2] <= kRockMaxSpan;
				for (std::size_t t = 0; t + 2 < shape->indices.size(); t += 3)
				{
					hull.clear();
					float lo = 1e9f, hi = -1e9f;
					for (std::size_t k = 0; k < 3; ++k)
					{
						const float* v = &shape->vertices[shape->indices[t + k] * 3];
						hull.insert(hull.end(), { v[0], v[1], v[2] });
						lo = std::min(lo, v[1]);
						hi = std::max(hi, v[1]);
					}
					if (compact)
						rcMarkConvexPolyArea(&ctx, hull.data(), 3, bottom, top, RC_NULL_AREA, *chf);
					else
						rcMarkConvexPolyArea(&ctx, hull.data(), 3, lo - c.cellHeight, hi + 2 * c.cellHeight, RC_NULL_AREA, *chf);
				}
				continue;
			}
			footprint(*shape, hull);
			if (hull.empty())
				continue;
			rcMarkConvexPolyArea(&ctx, hull.data(), static_cast<int>(hull.size() / 3), bottom, top,
				RC_NULL_AREA, *chf);
		}
	};
	const std::vector<unsigned char> beforeHulls(chf->areas, chf->areas + chf->spanCount);
	fillShapes(true);
	const bool anyDoor = std::any_of(in.shapes.begin(), in.shapes.end(),
		[](const kworld::WorldShape* shape) { return shape->use == kworld::NavUse::Door; });
	if (anyDoor)
	{
		// Mark the mats on the pre-fill areas with a scratch id (1 is never a span's area here: the
		// water pass has not run), then take back exactly those spans.
		const std::vector<unsigned char> afterHulls(chf->areas, chf->areas + chf->spanCount);
		std::copy(beforeHulls.begin(), beforeHulls.end(), chf->areas);
		for (const kworld::WorldShape* shape : in.shapes)
		{
			if (shape->use != kworld::NavUse::Door)
				continue;
			footprint(*shape, hull, kDoorMargin);
			if (!hull.empty())
				rcMarkConvexPolyArea(&ctx, hull.data(), static_cast<int>(hull.size() / 3),
					shape->min[1] - c.agentClimb - kMatStairDrop, shape->min[1] + c.agentHeight, 1, *chf);
		}
		for (int i = 0; i < chf->spanCount; ++i)
			chf->areas[i] = chf->areas[i] == 1 ? beforeHulls[static_cast<std::size_t>(i)] : afterHulls[static_cast<std::size_t>(i)];
	}
	fillShapes(false);
	// Clipped shapes (WorldShape::keepInside): the surface they carry outside their region is cut. A span is theirs when it
	// lies on one of the shape's non-vertical faces (within a climb; winding is not reliable, so up or down): a winding stair's hull covers the whole tower, and the ground
	// floor under it is another part's (knav_tower).
	std::vector<const float*> faces; // per non-vertical face: its three corners
	for (const kworld::WorldShape* shape : in.shapes)
	{
		if (!shape->keepInside)
			continue;
		footprint(*shape, hull);
		if (hull.empty())
			continue;
		faces.clear();
		for (std::size_t t = 0; t + 2 < shape->indices.size(); t += 3)
		{
			const float* a = &shape->vertices[shape->indices[t] * 3];
			const float* b = &shape->vertices[shape->indices[t + 1] * 3];
			const float* d = &shape->vertices[shape->indices[t + 2] * 3];
			const float ny = (b[2] - a[2]) * (d[0] - a[0]) - (b[0] - a[0]) * (d[2] - a[2]);
			if (ny != 0.0f)
				faces.insert(faces.end(), { a, b, d });
		}
		const auto onFace = [&](float px, float py, float pz)
		{
			for (std::size_t f = 0; f < faces.size(); f += 3)
			{
				const float* a = faces[f];
				const float* b = faces[f + 1];
				const float* d = faces[f + 2];
				const float area = (b[0] - a[0]) * (d[2] - a[2]) - (d[0] - a[0]) * (b[2] - a[2]);
				const float u = ((px - a[0]) * (d[2] - a[2]) - (d[0] - a[0]) * (pz - a[2])) / area;
				const float v = ((b[0] - a[0]) * (pz - a[2]) - (px - a[0]) * (b[2] - a[2])) / area;
				if (u < 0.0f || v < 0.0f || u + v > 1.0f)
					continue;
				if (std::fabs(a[1] + u * (b[1] - a[1]) + v * (d[1] - a[1]) - py) <= c.agentClimb)
					return true;
			}
			return false;
		};
		const int x0 = std::max(0, static_cast<int>(std::floor((shape->min[0] - cfg.bmin[0]) / cfg.cs)));
		const int x1 = std::min(chf->width - 1, static_cast<int>(std::floor((shape->max[0] - cfg.bmin[0]) / cfg.cs)));
		const int z0 = std::max(0, static_cast<int>(std::floor((shape->min[2] - cfg.bmin[2]) / cfg.cs)));
		const int z1 = std::min(chf->height - 1, static_cast<int>(std::floor((shape->max[2] - cfg.bmin[2]) / cfg.cs)));
		for (int z = z0; z <= z1; ++z)
		{
			for (int x = x0; x <= x1; ++x)
			{
				const float px = cfg.bmin[0] + (static_cast<float>(x) + 0.5f) * cfg.cs;
				const float pz = cfg.bmin[2] + (static_cast<float>(z) + 0.5f) * cfg.cs;
				if (!insideConvex(hull, px, pz))
					continue;
				const rcCompactCell& cell = chf->cells[x + z * chf->width];
				for (unsigned int i = cell.index; i < cell.index + static_cast<unsigned int>(cell.count); ++i)
				{
					const float py = cfg.bmin[1] + static_cast<float>(chf->spans[i].y) * cfg.ch;
					if (chf->areas[i] == RC_NULL_AREA || py < shape->min[1] - c.agentClimb || py > shape->max[1] + c.agentClimb
						|| !onFace(px, py, pz))
						continue;
					for (const auto& [nx, ny, nz, d] : *shape->keepInside)
					{
						if (nx * px + ny * py + nz * pz > d + kClipTolerance)
						{
							chf->areas[i] = RC_NULL_AREA;
							break;
						}
					}
				}
			}
		}
	}
	lap(StMarkPoly);
	// Water is one plane: the area of a surface is its depth under it. Recast area 0 is "null", so
	// area ids are Area + 1 here and shifted back when the polygons are tagged. Regions never merge
	// across areas, so a shoreline becomes a polygon boundary.
	for (int i = 0; i < chf->spanCount; ++i)
	{
		if (RC_NULL_AREA == chf->areas[i])
			continue;
		const float depth = c.waterLevel - (cfg.bmin[1] + static_cast<float>(chf->spans[i].y) * cfg.ch);
		const Area area = depth >= c.swimDepth ? Area::Swim : depth >= c.wadeDepth ? Area::Shallow : Area::Land;
		chf->areas[i] = static_cast<unsigned char>(static_cast<int>(area) + 1);
	}
	lap(StWater);
	// Doors: the walkable ground under a doorway's grown footprint becomes that door's own area,
	// whatever it was, so its polygons are split off the mesh around it. This only relabels spans
	// (nothing is added or removed), so the passage is exactly as walkable as without the door.
	// Doors are marked in key order, so a span two doors share goes to the larger key.
	std::vector<kworld::DoorKey> tileDoors;
	for (const kworld::WorldShape* shape : in.shapes)
		if (shape->use == kworld::NavUse::Door && shape->door != 0)
			tileDoors.push_back(shape->door);
	std::sort(tileDoors.begin(), tileDoors.end());
	tileDoors.erase(std::unique(tileDoors.begin(), tileDoors.end()), tileDoors.end());
	if (tileDoors.size() > static_cast<std::size_t>(kMaxTileDoors))
		tileDoors.resize(static_cast<std::size_t>(kMaxTileDoors));
	for (std::size_t k = 0; k < tileDoors.size(); ++k)
	{
		for (const kworld::WorldShape* shape : in.shapes)
		{
			if (shape->use != kworld::NavUse::Door || shape->door != tileDoors[k])
				continue;
			footprint(*shape, hull, kDoorMargin);
			if (!hull.empty())
				rcMarkConvexPolyArea(&ctx, hull.data(), static_cast<int>(hull.size() / 3),
					shape->min[1] - c.agentClimb, shape->min[1] + c.agentHeight,
					static_cast<unsigned char>(kFirstDoorArea + static_cast<int>(k)), *chf);
		}
	}
	lap(StDoors);
	// The wall band: what erosion by the agent radius would remove stays, dry land in it becoming Area::Edge (its own
	// polygons; water and doors keep their areas).
	const std::vector<unsigned char> beforeErode(chf->areas, chf->areas + chf->spanCount);
	if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
		return std::nullopt;
	constexpr unsigned char kLandId = static_cast<unsigned char>(Area::Land) + 1;
	for (int i = 0; band != WallBand::Eroded && i < chf->spanCount; ++i)
	{
		const unsigned char was = beforeErode[static_cast<std::size_t>(i)];
		if (chf->areas[i] == RC_NULL_AREA && was != RC_NULL_AREA)
			chf->areas[i] = was == kLandId && band == WallBand::Edge ? static_cast<unsigned char>(static_cast<int>(Area::Edge) + 1) : was;
	}
	// Props (closed Obstacle / Projected solids, cut above) get a hard body clearance: their footprint grown by the agent
	// radius is null, not the walkable Edge band, so a body centre never stands within a body's radius of a barrel or a
	// counter (kenshi-k16.7.89; the original keeps bodies out by physics collision). Walls keep the Edge band; a door's
	// own area wins over a prop's grown footprint, and so do the spans within reach of the prop's own use nodes (a bed's
	// or a counter's user stands there).
	{
		const std::vector<unsigned char> beforeProps(chf->areas, chf->areas + chf->spanCount);
		std::vector<unsigned int> stairs;
		for (const kworld::WorldShape* shape : in.shapes)
		{
			if (!propClearanceShape(*shape))
				continue;
			const bool projected = shape->use == kworld::NavUse::Projected;
			// A cell is marked when its centre is inside the hull, so an unmarked cell can reach half a cell into the ring:
			// grow by that, so the mesh edge keeps the full clearance (the 1 u clearance is under the 2 u cell).
			propClearanceHull(*shape, kFurnitureBodyClearance + 0.5f * c.cellSize, hull);
			if (hull.empty())
				continue;
			const float bottom = projected ? in.bmin[1] - 1.0f : shape->min[1] - c.cellHeight;
			const float top = projected ? in.bmax[1] + 1.0f : shape->max[1] + 2 * c.cellHeight;
			rcMarkConvexPolyArea(&ctx, hull.data(), static_cast<int>(hull.size() / 3), bottom, top, RC_NULL_AREA, *chf);
		}
		for (const kworld::WorldShape* shape : in.shapes)
		{
			if (!shape->useNodes || !propClearanceShape(*shape))
				continue;
			for (const auto& node : *shape->useNodes)
			{
				const int x0 = std::max(0, static_cast<int>(std::floor((node[0] - kUseReach - cfg.bmin[0]) / cfg.cs)));
				const int x1 = std::min(chf->width - 1, static_cast<int>(std::floor((node[0] + kUseReach - cfg.bmin[0]) / cfg.cs)));
				const int z0 = std::max(0, static_cast<int>(std::floor((node[2] - kUseReach - cfg.bmin[2]) / cfg.cs)));
				const int z1 = std::min(chf->height - 1, static_cast<int>(std::floor((node[2] + kUseReach - cfg.bmin[2]) / cfg.cs)));
				for (int z = z0; z <= z1; ++z)
					for (int x = x0; x <= x1; ++x)
					{
						const float dx = cfg.bmin[0] + (static_cast<float>(x) + 0.5f) * cfg.cs - node[0];
						const float dz = cfg.bmin[2] + (static_cast<float>(z) + 0.5f) * cfg.cs - node[2];
						if (dx * dx + dz * dz > kUseReach * kUseReach)
							continue;
						const rcCompactCell& cell = chf->cells[x + z * chf->width];
						for (unsigned int i = cell.index; i < cell.index + static_cast<unsigned int>(cell.count); ++i)
						{
							const float py = cfg.bmin[1] + static_cast<float>(chf->spans[i].y) * cfg.ch;
							if (std::fabs(py - node[1]) <= c.agentClimb + c.agentHeight)
								chf->areas[i] = beforeProps[i];
						}
					}
			}
		}
		// The use-node spare opens the clearance ring, never the furniture's own footprint (a user stands beside it).
		for (const kworld::WorldShape* shape : in.shapes)
		{
			if (!shape->useNodes || !propClearanceShape(*shape))
				continue;
			const bool projected = shape->use == kworld::NavUse::Projected;
			propClearanceHull(*shape, 0.5f * c.cellSize, hull); // the footprint plus the raster margin: a cell is marked by its centre
			if (!hull.empty())
				rcMarkConvexPolyArea(&ctx, hull.data(), static_cast<int>(hull.size() / 3), projected ? in.bmin[1] - 1.0f : shape->min[1] - c.cellHeight,
					projected ? in.bmax[1] + 1.0f : shape->max[1] + 2 * c.cellHeight, RC_NULL_AREA, *chf);
		}
		// The ring never narrows a gap between two blockers (furniture footprints, walls) below the walker's width: along each axis, a
		// run of spans between two blockers that was wide enough before the ring but keeps fewer than kGapCells free after it gets ring
		// spans back (low side first) until it keeps that many. A narrower gap stays as the ring left it (kenshi-k16.7.102).
		{
			const std::vector<unsigned char> ringAreas(chf->areas, chf->areas + chf->spanCount);
			std::copy(beforeProps.begin(), beforeProps.end(), chf->areas);
			for (const kworld::WorldShape* shape : in.shapes)
			{
				if (!propClearanceShape(*shape))
					continue;
				propClearanceHull(*shape, 0.5f * c.cellSize, hull);
				if (!hull.empty())
					rcMarkConvexPolyArea(&ctx, hull.data(), static_cast<int>(hull.size() / 3), shape->use == kworld::NavUse::Projected ? in.bmin[1] - 1.0f : shape->min[1] - c.cellHeight,
						shape->use == kworld::NavUse::Projected ? in.bmax[1] + 1.0f : shape->max[1] + 2 * c.cellHeight, RC_NULL_AREA, *chf);
			}
			std::vector<unsigned char> blocked(static_cast<std::size_t>(chf->spanCount));
			for (int i = 0; i < chf->spanCount; ++i)
				blocked[static_cast<std::size_t>(i)] = chf->areas[i] == RC_NULL_AREA;
			std::copy(ringAreas.begin(), ringAreas.end(), chf->areas);
			const int limit = kGapCells + 2; // a longer run is wide enough as it is
			std::vector<int> run;
			for (int z = 0; z < chf->height; ++z)
				for (int x = 0; x < chf->width; ++x)
				{
					const rcCompactCell& cell = chf->cells[x + z * chf->width];
					for (unsigned int i = cell.index; i < cell.index + static_cast<unsigned int>(cell.count); ++i)
					{
						if (chf->areas[i] != RC_NULL_AREA || blocked[i])
							continue;
						for (int axis = 0; axis < 2; ++axis)
						{
							// Walk to the blockers on both sides along dirs `axis` and `axis + 2`; an open end (a longer run, a ledge) skips the run.
							run.assign(1, static_cast<int>(i));
							bool bounded = true;
							for (int side = 0; side < 2 && bounded; ++side)
							{
								const int dir = axis + 2 * side;
								int at = static_cast<int>(i), ax = x, az = z;
								std::vector<int> half;
								for (int step = 0;; ++step)
								{
									const rcCompactSpan& span = chf->spans[at];
									if (step > limit || rcGetCon(span, dir) == RC_NOT_CONNECTED)
										break;
									ax += rcGetDirOffsetX(dir);
									az += rcGetDirOffsetY(dir);
									at = static_cast<int>(chf->cells[ax + az * chf->width].index) + rcGetCon(span, dir);
									if (blocked[static_cast<std::size_t>(at)])
										break;
									half.push_back(at);
								}
								if (static_cast<int>(half.size()) > limit)
									bounded = false;
								else if (side == 0)
									run.insert(run.begin(), half.rbegin(), half.rend());
								else
									run.insert(run.end(), half.begin(), half.end());
							}
							int free = 0;
							for (const int j : run)
								free += chf->areas[j] != RC_NULL_AREA;
							if (!bounded || free >= kGapCells || static_cast<int>(run.size()) < kGapCells)
								continue;
							for (std::size_t k = 0; k < run.size() && free < kGapCells; ++k)
								if (chf->areas[run[k]] == RC_NULL_AREA)
								{
									chf->areas[run[k]] = beforeProps[static_cast<std::size_t>(run[k])];
									++free;
								}
						}
					}
				}
		}
		// A stair or ramp is never cut by a ring: a span the ring nulled stays walkable where the surface around it (same
		// storey, within the ring's reach) rises or falls by more than the step height, so a furniture ring cannot slice a
		// stair into a gap the walker falls through (k16.7.89, fight_levels at the Hub bar stair).
		{
			const int reach = static_cast<int>(std::ceil((kFurnitureBodyClearance + c.cellSize) / c.cellSize)) + 1;
			const int storey = static_cast<int>(0.5f * c.agentHeight / c.cellHeight);
			for (int z = 0; z < chf->height; ++z)
				for (int x = 0; x < chf->width; ++x)
				{
					const rcCompactCell& cell = chf->cells[x + z * chf->width];
					for (unsigned int i = cell.index; i < cell.index + static_cast<unsigned int>(cell.count); ++i)
					{
						if (chf->areas[i] != RC_NULL_AREA || beforeProps[i] == RC_NULL_AREA)
							continue;
						const int y = chf->spans[i].y;
						int lo = y, hi = y;
						for (int nz = std::max(0, z - reach); nz <= std::min(chf->height - 1, z + reach); ++nz)
							for (int nx = std::max(0, x - reach); nx <= std::min(chf->width - 1, x + reach); ++nx)
							{
								const rcCompactCell& nc = chf->cells[nx + nz * chf->width];
								for (unsigned int j = nc.index; j < nc.index + static_cast<unsigned int>(nc.count); ++j)
								{
									const int dy = chf->spans[j].y - y;
									if (beforeProps[j] != RC_NULL_AREA && std::abs(dy) <= storey)
									{
										lo = std::min(lo, y + dy);
										hi = std::max(hi, y + dy);
									}
								}
							}
						if (hi - lo > cfg.walkableClimb)
							stairs.push_back(i);
					}
				}
			for (const unsigned int i : stairs)
				chf->areas[i] = beforeProps[i];
		}
		for (int i = 0; i < chf->spanCount; ++i)
			if (beforeProps[static_cast<std::size_t>(i)] >= kFirstDoorArea)
				chf->areas[i] = beforeProps[static_cast<std::size_t>(i)];
	}
	lap(StErode);
	bool regionsBuilt = false;
	switch (gTuning.partition)
	{
	case BakePartition::Watershed:
		if (!rcBuildDistanceField(&ctx, *chf))
			return std::nullopt;
		lap(StDistance);
		regionsBuilt = rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea);
		break;
	case BakePartition::Monotone:
		regionsBuilt = rcBuildRegionsMonotone(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea);
		break;
	case BakePartition::Layers:
		regionsBuilt = rcBuildLayerRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea);
		break;
	}
	if (!regionsBuilt)
		return std::nullopt;
	splitOverlappingRegions(*chf);
	lap(StRegions);

	ContoursPtr contours(rcAllocContourSet());
	if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours)
		|| contours->nconts == 0)
		return std::nullopt;
	lap(StContours);
	// A wall-band (Edge) piece whose contour simplifies away (under 3 vertices: a strip one or two cells wide, which the watershed cuts
	// into small pieces beside a Land strip) leaves no polygon, and the mesh wall moves in by the band's width. Such a piece is
	// relabelled Land and the regions and contours are built again: it joins the Land region beside it (kenshi-k16.7.71).
	{
		constexpr unsigned char kEdgeId = static_cast<unsigned char>(Area::Edge) + 1;
		std::vector<char> hasContour(RC_BORDER_REG, 0);
		for (int ci = 0; ci < contours->nconts; ++ci)
			if (contours->conts[ci].nverts >= 3)
				hasContour[contours->conts[ci].reg] = 1;
		bool relabelled = false;
		for (int i = 0; i < chf->spanCount; ++i)
		{
			const unsigned short region = chf->spans[i].reg;
			if (region != 0 && !(region & RC_BORDER_REG) && chf->areas[i] == kEdgeId && !hasContour[region])
			{
				chf->areas[i] = static_cast<unsigned char>(static_cast<int>(Area::Land) + 1);
				relabelled = true;
			}
		}
		if (relabelled)
		{
			if (!rcBuildDistanceField(&ctx, *chf) || !rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea))
				return std::nullopt;
			splitOverlappingRegions(*chf);
			contours.reset(rcAllocContourSet());
			if (!rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *contours) || contours->nconts == 0)
				return std::nullopt;
		}
	}
	PolyMeshPtr pmesh(rcAllocPolyMesh());
	if (!rcBuildPolyMesh(&ctx, *contours, cfg.maxVertsPerPoly, *pmesh) || pmesh->npolys == 0)
		return std::nullopt;
	// A 32-bit dtPolyRef has no bit to spare (kMaxPolysPerTile): a tile over it goes one step down WallBand. Counted
	// (bandlessTiles, erodedTiles); knav_bake prints them.
	if (band != WallBand::Eroded && pmesh->npolys > kMaxPolysPerTile)
	{
		++(band == WallBand::Edge ? gBandlessTiles : gErodedTiles);
		return buildTile(c, terrain, shapes, tx, tz, band == WallBand::Edge ? WallBand::Land : WallBand::Eroded);
	}
	lap(StPolyMesh);
	DetailPtr dmesh(rcAllocPolyMeshDetail());
	if (!rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
		return std::nullopt;

	lap(StDetail);
	if (!in.shapes.empty())
	{
		HeightOracle oracle(in.bmin, in.bmax, 16.0f);
		const float terrainUp = std::cos(cfg.walkableSlopeAngle * 3.14159265f / 180.0f);
		oracle.add(in.terrainVerts.data(), in.terrainTris.data(), in.terrainTris.size() / 3, terrainUp);
		for (const kworld::WorldShape* shape : in.shapes)
		{
			if (shape->use == kworld::NavUse::Projected || shape->use == kworld::NavUse::Door || shape->use == kworld::NavUse::Obstacle)
				continue;
			const float slope = shapeSlope(*shape, cfg.walkableSlopeAngle);
			std::vector<int> indices(shape->indices.begin(), shape->indices.end());
			const std::vector<char> ramp = rampTriangles(*shape, cfg.walkableSlopeAngle);
			if (!ramp.empty())
			{
				// Walkable faces and the ramp strips (steeper than the walk limit), under the stair limit.
				const float cosWalk = std::cos(cfg.walkableSlopeAngle * 3.14159265f / 180.0f);
				std::vector<int> kept;
				for (std::size_t t = 0; t < ramp.size(); ++t)
					if (ramp[t] || triangleUp(*shape, t) > cosWalk)
						kept.insert(kept.end(), { indices[t * 3], indices[t * 3 + 1], indices[t * 3 + 2] });
				oracle.add(shape->vertices.data(), kept.data(), kept.size() / 3, std::cos(kStairSlopeDegrees * 3.14159265f / 180.0f));
				continue;
			}
			oracle.add(shape->vertices.data(), indices.data(), indices.size() / 3, std::cos(slope * 3.14159265f / 180.0f));
		}
		cfg.ch = refineHeights(cfg, oracle, *pmesh, *dmesh);
	}
	auto built = finishTile(c, cfg, *pmesh, *dmesh, tileDoors, tx, tz);
	lap(StDetour);
	++gProfileTiles;
	return built;
}

} // namespace

bool propClearanceShape(const kworld::WorldShape& shape)
{
	return shape.furniture && (shape.use == kworld::NavUse::Obstacle || shape.use == kworld::NavUse::Projected) && shape.closed && !rockShell(shape);
}

void propClearanceHull(const kworld::WorldShape& shape, float radius, std::vector<float>& out)
{
	footprint(shape, out, radius, true);
}

void setBakeTuning(const BakeTuning& tuning)
{
	gTuning = tuning;
	// Recast divides edge lengths by the sample distance: "off" is a distance beyond any polygon.
	if (gTuning.detailSampleCells <= 0.0f)
		gTuning.detailSampleCells = 1000.0f;
}

BakeTuning bakeTuning()
{
	return gTuning;
}

std::uint64_t flatTilesBaked()
{
	return gFlatTiles;
}

std::uint64_t bandlessTiles()
{
	return gBandlessTiles;
}

std::uint64_t erodedTiles()
{
	return gErodedTiles;
}

void setBakeProfile(bool on)
{
	gProfile = on;
}

void printBakeProfile(std::ostream& out)
{
	std::uint64_t total = 0;
	for (const auto& ns : gStageNs)
		total += ns;
	if (0 == total)
		return;
	out << "bake profile (CPU, summed over threads), " << gProfileTiles << " tiles built, " << gFlatTiles << " of them flat shortcuts:\n";
	for (int i = 0; i < StCount; ++i)
	{
		const double seconds = static_cast<double>(gStageNs[i]) * 1e-9;
		out << "  " << std::left << std::setw(24) << kStageNames[i] << std::right << std::fixed << std::setprecision(2)
			<< std::setw(9) << seconds << " s " << std::setw(6) << std::setprecision(1)
			<< 100.0 * static_cast<double>(gStageNs[i]) / static_cast<double>(total) << " %\n";
	}
	out << "  total " << std::setprecision(2) << static_cast<double>(total) * 1e-9 << " s, "
		<< static_cast<double>(total) * 1e-6 / static_cast<double>(std::max<std::uint64_t>(gProfileTiles, 1)) << " ms/tile\n";
}

double tileBorder(const NavConfig& c)
{
	const int radiusCells = static_cast<int>(std::ceil(c.agentRadius / c.cellSize));
	return (radiusCells + 3) * static_cast<double>(c.cellSize);
}

NavConfig configForZones(kworld::ZoneCell min, kworld::ZoneCell max)
{
	NavConfig config;
	config.originX = kworld::zoneMinX(min.x);
	config.originZ = kworld::zoneMinZ(min.z);
	config.tilesX = static_cast<int>(std::lround((max.x - min.x + 1) * kworld::kZoneSize / config.tileSize));
	config.tilesZ = static_cast<int>(std::lround((max.z - min.z + 1) * kworld::kZoneSize / config.tileSize));
	config.window = { 0, 0, config.tilesX - 1, config.tilesZ - 1 };
	return config;
}

std::string regionFileName(int rx, int rz)
{
	return "region_" + std::to_string(rx) + "_" + std::to_string(rz) + ".knav";
}

NavConfig configForWorldZones(kworld::ZoneCell min, kworld::ZoneCell max)
{
	NavConfig config;
	config.originX = kworld::kWorldMin;
	config.originZ = kworld::kWorldMin;
	config.tilesX = kworld::kZoneCellsPerAxis * kTilesPerZone;
	config.tilesZ = config.tilesX;
	config.window = { min.x * kTilesPerZone, min.z * kTilesPerZone, (max.x + 1) * kTilesPerZone - 1,
		(max.z + 1) * kTilesPerZone - 1 };
	return config;
}

// Recast's rcBuildContours (1.6.0) reads heap bytes it never wrote (valgrind: "conditional jump depends on uninitialised
// value", even on one thread), and what it reads decides a contour: one tile of zone 29,16 came out with 26236 or 25988
// bytes depending on what malloc handed it, so a bake was not the same twice (kenshi-k16.7.50). Recast takes its memory
// from rcAlloc: hand it zeroed memory and the unwritten bytes are the same on every run, thread count and host.
namespace
{

void* zeroedAlloc(size_t size, rcAllocHint)
{
	return std::calloc(1, size);
}

void zeroedFree(void* ptr)
{
	std::free(ptr);
}

} // namespace

std::vector<BakedTile> bakeTileSet(const NavConfig& config, const kworld::Heightmap& terrain,
	const std::vector<const kworld::WorldShape*>& shapes, const std::vector<TileCoord>& coords,
	int threads)
{
	static std::once_flag zeroed;
	std::call_once(zeroed, [] { rcAllocSetCustom(zeroedAlloc, zeroedFree); });
	std::vector<std::optional<BuiltTile>> built(coords.size());
	std::atomic<std::size_t> next{ 0 };
	const auto work = [&]
	{
		for (std::size_t i = next++; i < coords.size(); i = next++)
			built[i] = buildTile(config, terrain, shapes, coords[i].x, coords[i].z);
	};
	std::vector<std::thread> pool;
	for (int i = 1; i < std::max(threads, 1); ++i)
		pool.emplace_back(work);
	work();
	for (std::thread& thread : pool)
		thread.join();

	std::vector<BakedTile> tiles;
	for (std::size_t i = 0; i < coords.size(); ++i)
		if (built[i])
			tiles.push_back({ coords[i].x, coords[i].z, std::move(built[i]->data), std::move(built[i]->doors) });
	return tiles;
}

std::vector<BakedTile> bakeTiles(const NavConfig& config, const kworld::Heightmap& terrain,
	const std::vector<kworld::WorldShape>& shapes, int threads)
{
	std::vector<const kworld::WorldShape*> pointers;
	for (const kworld::WorldShape& shape : shapes)
		pointers.push_back(&shape);
	std::vector<TileCoord> coords;
	for (int z = config.window.z0; z <= config.window.z1; ++z)
		for (int x = config.window.x0; x <= config.window.x1; ++x)
			coords.push_back({ x, z });
	return bakeTileSet(config, terrain, pointers, coords, threads);
}

std::vector<TileCoord> tilesTouching(const NavConfig& config, const kworld::WorldShape& shape)
{
	const double border = tileBorder(config);
	const auto range = [&](double low, double high, double origin, int count, int& first, int& last)
	{
		first = std::max(0, static_cast<int>(std::floor((low - border - origin) / config.tileSize)));
		last = std::min(count - 1, static_cast<int>(std::floor((high + border - origin) / config.tileSize)));
	};
	int x0, x1, z0, z1;
	range(shape.min[0], shape.max[0], config.originX, config.tilesX, x0, x1);
	range(shape.min[2], shape.max[2], config.originZ, config.tilesZ, z0, z1);
	x0 = std::max(x0, config.window.x0);
	x1 = std::min(x1, config.window.x1);
	z0 = std::max(z0, config.window.z0);
	z1 = std::min(z1, config.window.z1);
	std::vector<TileCoord> tiles;
	for (int z = z0; z <= z1; ++z)
		for (int x = x0; x <= x1; ++x)
			tiles.push_back({ x, z });
	return tiles;
}

} // namespace knav
