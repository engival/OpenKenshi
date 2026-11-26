#include "kworld/foliage.hpp"

#include "kworld/features.hpp"
#include "kworld/gamedata.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/roadoverlay.hpp"
#include "kworld/towns.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kworld {

namespace {

constexpr std::uint32_t kUpperMask = 0x80000000u;
constexpr std::uint32_t kLowerMask = 0x7fffffffu;

// Exe constants (FOLIAGE_EXE.md; read from the exe bytes).
constexpr float kScaleLimitMin = 0.01f; // collector +0x48, set in 1406d3e30
constexpr float kScaleLimitMax = 10.0f; // collector +0x44
constexpr float kClusterMinOffset = 5.0f; // DAT_141685340
constexpr int kGrassGate = 0x95; // 1406d3740: R + G must exceed this for a `limit to grass areas` mesh
constexpr int kGrassRerolls = 20; // 1406d3740:143-163: re-rolls after the first draw (kinspect section 15: 21 draws in all)
constexpr double kHeightStep = 36.0; // height cache cell
constexpr int kStepsPerZone = 128; // kZoneSize / kHeightStep
constexpr int kMaxChildDepth = 8; // guards cyclic `meshes` lists
constexpr double kDefaultTownRange = 2000.0; // TOWN `no-foliage range` default
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr float kPageSize = 2304.0f; // PagedGeometry page; grid origin = zone min corner (live, Hub)
constexpr double kEraserReach = 5000.0; // discs farther than this from a zone are dropped

// The EntData fields the scatter reads (1406ce160), one per FOLIAGE_MESH.
// Fallbacks apply only to records that omit a field; shipped records carry
// them all.
struct MeshParams
{
	std::string id;
	float maxSlope = 1000.0f;
	float minSlope = 0.0f;
	float minAltitude = 0.0f;
	float maxAltitude = 20000.0f;
	float minHeight = 1.0f;
	float maxHeight = 1.0f;
	float verticalMin = 0.0f;
	float verticalMax = 0.0f;
	bool clustered = false;
	bool floating = false;
	bool avoidTowns = false;
	bool slopeAlign = false;
	bool keepUpright = true;
	int clusterNumMin = 1;
	int clusterNumMax = 1;
	float clusterRadiusMin = 0.0f;
	float clusterRadiusMax = 0.0f;
	float childRadius = 0.0f; // `child cluster radius`
	float roadAvoidance = 0.0f; // EntData +0x48
	// The navtile vegetation fold's gate (kinspect findings/navhash.md section 11): a spawned tree-loader instance
	// folds iff its FOLIAGE_MESH has a `navmesh cutter` or a collision file (the exe's EntData+0x40 > 0 ||
	// EntData+0x80 != 0).
	bool navFold = false;
	bool grassLimited = false; // `limit to grass areas`
	float grassSpot = 0.0f; // `grass spot` (EntData+0x44): the radius, x the instance scale, of the disc a placed instance writes into the overlay's G
	std::vector<const MeshParams*> children; // the record's `meshes` list
};

struct Quat
{
	double w = 1.0;
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;
};

struct Disc
{
	double x = 0.0;
	double z = 0.0;
	double radiusSquared = 0.0;
};

// Normalised like Ogre::Vector2::normalise (no change below 1e-8).
void normalise(float& x, float& y)
{
	const float length = std::sqrt(x * x + y * y);
	if (length > 1e-08f)
	{
		x /= length;
		y /= length;
	}
}

// Everything one generateFoliage() call needs; lives on that call's stack.
class ZoneScatter
{
public:
	ZoneScatter(ZoneCell cell, const Heightmap& terrain, const BiomeMap& biomes,
		const kdata::GameData& gameData, const FoliageParams& params)
		: cell_(cell), terrain_(terrain), biomes_(biomes), gameData_(gameData),
		params_(params),
		minX_(static_cast<float>(zoneMinX(cell.x))),
		minZ_(static_cast<float>(zoneMinZ(cell.z))),
		maxX_(minX_ + static_cast<float>(kZoneSize)),
		maxZ_(minZ_ + static_cast<float>(kZoneSize))
	{
		buildHeightGrid();
	}

	void setDiscs(std::vector<Disc> erasers, std::vector<Disc> towns, const RoadOverlay& roads)
	{
		erasers_ = std::move(erasers);
		towns_ = std::move(towns);
		roads_ = &roads;
	}

	// Scatters one FOLIAGE_LAYER; biomeIndex < 0 skips the biome test.
	void runLayer(const kdata::Record& layer, std::int64_t biomeIndex,
		std::vector<FoliageInstance>& out)
	{
		const auto* meshes = layer.findRefs("meshes");
		if (meshes == nullptr)
			return;
		rng_.emplace(foliageLayerSeed(static_cast<int>(minX_), static_cast<int>(minZ_),
			leadingRecordID(layer.stringID)));
		out_ = &out;
		for (const auto& ref : meshes->entries)
		{
			const MeshParams* mesh = meshParams(ref.targetID);
			if (mesh != nullptr)
				scatterMesh(*mesh, static_cast<int>(static_cast<float>(ref.v0)), biomeIndex);
		}
		out_ = nullptr;
	}

private:
	const MeshParams* meshParams(const std::string& id)
	{
		const auto found = meshes_.find(id);
		if (found != meshes_.end())
			return found->second.get();
		const kdata::Record* record = gameData_.find(id);
		if (record == nullptr || record->type != itemTypeByName("FOLIAGE_MESH"))
			return nullptr;

		auto mesh = std::make_unique<MeshParams>();
		MeshParams* raw = mesh.get();
		meshes_.emplace(id, std::move(mesh)); // registered first: a cyclic list resolves
		raw->id = id;
		raw->maxSlope = record->floatOr("max slope", raw->maxSlope);
		raw->minSlope = record->floatOr("min slope", raw->minSlope);
		raw->minAltitude = record->floatOr("min altitude", raw->minAltitude);
		raw->maxAltitude = record->floatOr("max altitude", raw->maxAltitude);
		raw->minHeight = record->floatOr("min height", raw->minHeight);
		raw->maxHeight = record->floatOr("max height", raw->maxHeight);
		raw->verticalMin = record->floatOr("vertical offset min", raw->verticalMin);
		raw->verticalMax = record->floatOr("vertical offset max", raw->verticalMax);
		raw->clustered = record->boolOr("clustered", false);
		raw->floating = record->boolOr("floating", false);
		raw->avoidTowns = record->boolOr("avoid towns", false);
		raw->slopeAlign = record->boolOr("slope align", false);
		raw->keepUpright = record->boolOr("keep upright", true);
		raw->clusterNumMin = record->intOr("cluster num min", 1);
		raw->clusterNumMax = record->intOr("cluster num max", 1);
		raw->clusterRadiusMin = record->floatOr("cluster radius min", 0.0f);
		raw->clusterRadiusMax = record->floatOr("cluster radius max", 0.0f);
		raw->childRadius = record->floatOr("child cluster radius", 0.0f);
		raw->roadAvoidance = record->floatOr("road avoidance", 0.0f);
		raw->navFold = record->floatOr("navmesh cutter", 0.0f) > 0.0f || !record->fileOr("collision").empty();
		raw->grassLimited = record->boolOr("limit to grass areas", false);
		raw->grassSpot = record->floatOr("grass spot", 0.0f);
		if (const auto* children = record->findRefs("meshes"))
			for (const auto& child : children->entries)
				if (const MeshParams* resolved = meshParams(child.targetID))
					raw->children.push_back(resolved);
		return raw;
	}

	// The exe's per-zone height cache: one sample per 36 units. ASSUMPTION
	// (UNVERIFIED): sample (i, j) is the terrain height at the corner
	// (minX + 36 i, minZ + 36 j); the cache origin is the zone's min corner.
	// 129 samples per axis so the bilinear lookup has a far edge.
	void buildHeightGrid()
	{
		constexpr int n = kStepsPerZone + 1;
		grid_.resize(static_cast<std::size_t>(n) * n);
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
				grid_[static_cast<std::size_t>(j) * n + i] = static_cast<float>(
					terrain_.height(minX_ + kHeightStep * i, minZ_ + kHeightStep * j));
	}

	// 1407eae40 (navhash.md section 13): max(|h - h(i-1, j)|, |h - h(i, j+1)|)
	// over THIS zone's 129x129 cache. The cell index is the f32
	// trunc((p - zoneMin) / 36), clamped to [0, 128]; truncation is toward
	// zero, so a point drawn before the zone's min corner reads cell 0. A
	// neighbour off the low-x or high-z edge is missing and contributes 0.
	float slopeAt(float x, float z) const
	{
		constexpr float step = static_cast<float>(kHeightStep);
		const int ix = std::clamp(static_cast<int>((x - minX_) / step), 0, kStepsPerZone);
		const int iz = std::clamp(static_cast<int>((z - minZ_) / step), 0, kStepsPerZone);
		constexpr int n = kStepsPerZone + 1;
		const auto at = [this](int a, int b) { return grid_[static_cast<std::size_t>(b) * n + a]; };
		const float h = at(ix, iz);
		const float west = ix > 0 ? std::fabs(h - at(ix - 1, iz)) : 0.0f;
		const float north = iz < kStepsPerZone ? std::fabs(h - at(ix, iz + 1)) : 0.0f;
		return std::max(west, north);
	}

	// 1407eac60: bilinear over the 36-unit cache. Only called for points
	// inside this zone (the bounds test runs first).
	float heightAt(float x, float z) const
	{
		// f32 as the exe computes it: the index is the f32 trunc((p - zoneMin) / 36) (section 3) and the fraction
		// the same f32 value minus the index. A double ratio rounded once differs in the last bits and moves y
		// across an integer, which the fold's trunc sees (zone 52,37, Canyonland_FatTop01).
		constexpr float step = static_cast<float>(kHeightStep);
		const float u = std::clamp((x - minX_) / step, 0.0f, static_cast<float>(kStepsPerZone));
		const float v = std::clamp((z - minZ_) / step, 0.0f, static_cast<float>(kStepsPerZone));
		const int i = std::min(static_cast<int>(u), kStepsPerZone - 1);
		const int j = std::min(static_cast<int>(v), kStepsPerZone - 1);
		const float fx = u - static_cast<float>(i);
		const float fz = v - static_cast<float>(j);
		constexpr int n = kStepsPerZone + 1;
		const auto at = [this](int a, int b) { return grid_[static_cast<std::size_t>(b) * n + a]; };
		const float rowA = at(i, j) + (at(i + 1, j) - at(i, j)) * fx;
		const float rowB = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fx;
		return rowA + (rowB - rowA) * fz;
	}

	bool insideDisc(const std::vector<Disc>& discs, float x, float z) const
	{
		for (const Disc& disc : discs)
		{
			const double dx = disc.x - x;
			const double dz = disc.z - z;
			if (dx * dx + dz * dz < disc.radiusSquared)
				return true;
		}
		return false;
	}

	// 140927f10 + the test in 1406d2410: the nearest town only, whatever its
	// distance; inside its `no-foliage range` the instance is suppressed.
	// UNVERIFIED: the exe skips this test for meshes with a `building type`
	// when the nearest town has a building attached; not reproduced.
	bool nearTown(float x, float z) const
	{
		const Disc* nearest = nullptr;
		double best = 0.0;
		for (const Disc& town : towns_)
		{
			const double dx = town.x - x;
			const double dz = town.z - z;
			const double d = dx * dx + dz * dz;
			if (nearest == nullptr || d < best)
			{
				nearest = &town;
				best = d;
			}
		}
		return nearest != nullptr && best < nearest->radiusSquared;
	}

	// Terrain normal under a point (Global 1409b1d20 -> 1407ea850, the zone
	// height cache at zone+0xb8): the face normal of ONE fixed triangle of the
	// 36-unit lattice cell holding the point, (i,j), (i,j+1), (i+1,j+1), i.e.
	// the triangle on the (0,0)-(1,1) diagonal with x <= z, whatever side of
	// the diagonal the point is on. Live check (Sho-Battai, 870 slope-align
	// plants): 867 match the game below 0.05 deg, 3 are not (see
	// FOLIAGE_EXE.md). Not reproduced: the exe clamps the point and the
	// i+1/j+1 neighbours to the zone's cache; we read the global lattice.
	void terrainNormal(float x, float z, double& nx, double& ny, double& nz) const
	{
		const double x0 = kWorldMin + std::floor((x - kWorldMin) / kHeightStep) * kHeightStep;
		const double z0 = kWorldMin + std::floor((z - kWorldMin) / kHeightStep) * kHeightStep;
		const double h00 = terrain_.height(x0, z0);
		const double h01 = terrain_.height(x0, z0 + kHeightStep);
		const double h11 = terrain_.height(x0 + kHeightStep, z0 + kHeightStep);
		nx = h01 - h11;
		ny = kHeightStep;
		nz = h00 - h01;
		const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
		nx /= length;
		ny /= length;
		nz /= length;
	}

	// Instance rotation as the exe's rotation callback builds it (1406cc2e0).
	// slope align: shortest arc from +Y to the terrain normal, then the yaw
	// (q = arc * yaw; verified against the game's batch quaternions, 0.00 deg
	// from arc * yaw once the arc is taken from them). Else, not keep upright: a rotation about a random axis by a
	// random angle (libc rand seeded from the position hash, MSVC LCG), which
	// replaces the yaw. Else: yaw only.
	Quat orientation(const MeshParams& mesh, float x, float z, float yawDegrees) const
	{
		const double half = yawDegrees * kDegToRad * 0.5;
		const Quat yaw{ std::cos(half), 0.0, std::sin(half), 0.0 };
		if (mesh.slopeAlign)
		{
			double nx, ny, nz;
			terrainNormal(x, z, nx, ny, nz);
			// getRotationTo(Y -> n): w = 1 + dot, xyz = Y x n; the antiparallel
			// case cannot occur on a heightfield.
			Quat arc{ 1.0 + ny, nz, 0.0, -nx };
			const double length = std::sqrt(arc.w * arc.w + arc.x * arc.x + arc.z * arc.z);
			arc = { arc.w / length, arc.x / length, 0.0, arc.z / length };
			return { arc.w * yaw.w - arc.y * yaw.y,
				arc.w * yaw.x + arc.x * yaw.w + arc.y * yaw.z - arc.z * yaw.y,
				arc.w * yaw.y + arc.y * yaw.w + arc.z * yaw.x - arc.x * yaw.z,
				arc.w * yaw.z + arc.z * yaw.w + arc.x * yaw.y - arc.y * yaw.x };
		}
		if (!mesh.keepUpright)
		{
			std::uint32_t h = static_cast<std::uint32_t>(static_cast<int>(std::floor(z))) * 57u
				+ static_cast<std::uint32_t>(static_cast<int>(std::floor(x)));
			h ^= h << 13;
			h = ((h * h * 0xec4du + 0x131071fu) * h + 0xd208dd0du) & 0x7fffffffu;
			const float t = static_cast<float>(1.0 - static_cast<double>(h) * 9.313225746154785e-10);
			std::uint32_t state = static_cast<std::uint32_t>(
				static_cast<std::int64_t>(t * 1000000.0f));
			const auto msvcRand = [&state] {
				state = state * 214013u + 2531011u;
				return static_cast<double>((state >> 16) & 0x7fffu);
			};
			const double r1 = msvcRand();
			const double r2 = msvcRand();
			const double r3 = msvcRand();
			double length = std::sqrt(r1 * r1 + r2 * r2 + r3 * r3);
			if (length <= 0.0)
				length = 1.0;
			const double a = t * 360.0 * kDegToRad * 0.5;
			const double s = std::sin(a) / length;
			return { std::cos(a), r3 * s, r2 * s, r1 * s };
		}
		return yaw;
	}

	// A coordinate through the loader's u16 page offset (encoder 140a46460):
	// the offset inside its kPageSize page, relative to the zone's min corner,
	// in 65535ths of a page, then decoded back.
	static float quantisePosition(float world, float origin)
	{
		const float d = world - origin;
		const float page = std::floor(d / kPageSize);
		const long u = static_cast<long>(65535.0f * (d - page * kPageSize) / kPageSize);
		return origin + page * kPageSize + u / 65535.0f * kPageSize;
	}

	// 1406d2410. Returns false where the exe returns 0 (slope or altitude
	// failed: the rest of a cluster is abandoned), true otherwise.
	bool place(const MeshParams& mesh, float x, float z, bool isChild, int depth)
	{
		const float slope = slopeAt(x, z);
		if (slope > mesh.maxSlope || slope < mesh.minSlope)
			return false;
		if (x < minX_ || x > maxX_ || z < minZ_ || z > maxZ_)
			return true;

		bool skip = mesh.avoidTowns && nearTown(x, z);

		float y = heightAt(x, z);
		if (y > mesh.maxAltitude || y < mesh.minAltitude)
			return false;
		if (mesh.floating)
			y = std::min(y, params_.waterHeight);

		const float lo = std::max(std::min(mesh.minHeight * 2.0f, kScaleLimitMax), kScaleLimitMin);
		const float hi = std::max(std::min(mesh.maxHeight * 2.0f, kScaleLimitMax), kScaleLimitMin);
		const float scale = rng_->uniform(lo, hi);
		const float offset = mesh.verticalMin != mesh.verticalMax
			? rng_->uniform(mesh.verticalMin, mesh.verticalMax) : mesh.verticalMin;
		y += offset * scale;

		// 1406d82d0: the parent is skipped when any texel on the perimeter or
		// at the centre of the box of half-width `road avoidance` x scale
		// (world units) around its texel has non-zero alpha. Interior texels
		// are never read (navhash.md section 12).
		if (!skip && mesh.roadAvoidance > 0.0f)
			skip = roads_->roadRejected(x, z, mesh.roadAvoidance * scale);

		// Drawn even when the instance is dropped, like the exe.
		const float yaw = rng_->uniform(0.0f, 360.0f);

		if (!skip)
			skip = insideDisc(erasers_, x, z);
		if (!skip)
		{
			// The renderer call (140a46460) stores each plant as u16 offsets
			// inside its page, a yaw byte and a scale byte; the game renders
			// with the decoded values (live-checked at the Hub). Its physics
			// body sits at whole-unit x/z (FOLIAGE_EXE.md, collision pose
			// 2026-11-25); ksim collides at the decoded x/z, under 1 u off.
			// The vertical offset above used the raw scale draw.
			const float decodedX = quantisePosition(x, minX_);
			const float decodedZ = quantisePosition(z, minZ_);
			const float quantisedYaw = static_cast<int>(255.0f * yaw / 360.0f) * (360.0f / 255.0f);
			FoliageInstance instance;
			instance.recordID = mesh.id;
			instance.x = params_.unquantisedPositions ? x : decodedX;
			instance.y = y;
			instance.z = params_.unquantisedPositions ? z : decodedZ;
			const Quat q = orientation(mesh, decodedX, decodedZ, quantisedYaw);
			instance.qw = q.w;
			instance.qx = q.x;
			instance.qy = q.y;
			instance.qz = q.z;
			instance.yawDegrees = quantisedYaw;
			const int scaleByte = static_cast<int>(255.0f * (scale - kScaleLimitMin) / kScaleLimitMax);
			instance.scale = kScaleLimitMin + scaleByte * (kScaleLimitMax / 255.0f);
			instance.isChild = isChild;
			instance.navFold = mesh.navFold;
			instance.grassLimited = mesh.grassLimited;
			out_->push_back(std::move(instance));
			if (mesh.grassSpot > 0.0f)
				paintGrassSpot(x, z, mesh.grassSpot * scale);
		}

		// The child scatter runs even when the parent was dropped.
		if (depth < kMaxChildDepth)
		{
			for (const MeshParams* child : mesh.children)
			{
				const int count = child->clusterNumMin + static_cast<int>(
					rng_->uniformInt(rangeOf(child->clusterNumMin, child->clusterNumMax)));
				const float radius = rng_->uniform(child->clusterRadiusMin, child->clusterRadiusMax);
				for (int k = count; k > 0; --k)
				{
					float dz = rng_->uniform(-1.0f, 1.0f);
					float dx = rng_->uniform(-1.0f, 1.0f);
					normalise(dx, dz);
					const float reach = rng_->uniform(0.0f, radius) + child->childRadius
						+ mesh.childRadius;
					place(*child, dx * reach + x, dz * reach + z, true, depth + 1);
				}
			}
		}
		return true;
	}

	// max - min as the unsigned argument of the exe's integer draw. A
	// negative range (max < min) would wrap to a huge count in the exe;
	// UNVERIFIED: draw as if the range were 0.
	static std::uint32_t rangeOf(int minCount, int maxCount)
	{
		return maxCount > minCount ? static_cast<std::uint32_t>(maxCount - minCount) : 0u;
	}

	// The grass test of 1406d3740 (`limit to grass areas`): the overlay's R (baked grass density, the PNG's R) plus G (the grass
	// spots the scatter has written so far) at the point's texel must exceed 0x95.
	bool grassPasses(float x, float z) const
	{
		const long tx = roads_->texel(x);
		const long tz = roads_->texel(z);
		const auto it = grassSpots_.find(spotKey(tx, tz));
		const int g = it != grassSpots_.end() ? it->second : 0;
		return roads_->grassDensity(tx, tz) + g > kGrassGate;
	}

	static std::uint64_t spotKey(long tx, long tz)
	{
		return (static_cast<std::uint64_t>(tz) << 32) | static_cast<std::uint32_t>(tx);
	}

	// 1406d8550 via 1406d2410:652: a placed instance with `grass spot` writes a disc of radius R = grass spot x scale into the
	// overlay's G, overwriting. Peak v = clamp(2R / texel size, 0.3, 1) falling off linearly with the squared distance to the
	// centre, written (uchar)(v * 96). UNSURE: the clamp's upper bound (1.0: G alone then peaks at 96, below the gate, as the
	// finding says), the exact falloff and whether the box is clipped to the disc.
	void paintGrassSpot(float x, float z, float radius)
	{
		const double r = radius;
		const double peak = std::clamp(2.0 * r / RoadOverlay::kPixelSize, 0.3, 1.0);
		const long x0 = roads_->texel(x - r);
		const long x1 = roads_->texel(x + r);
		const long z0 = roads_->texel(z - r);
		const long z1 = roads_->texel(z + r);
		for (long tz = z0; tz <= z1; ++tz)
			for (long tx = x0; tx <= x1; ++tx)
			{
				const double dx = RoadOverlay::texelCentre(tx) - x;
				const double dz = RoadOverlay::texelCentre(tz) - z;
				const double d2 = dx * dx + dz * dz;
				if (d2 >= r * r)
					continue;
				grassSpots_[spotKey(tx, tz)] = static_cast<std::uint8_t>(peak * (1.0 - d2 / (r * r)) * 96.0);
			}
	}

	// 1406d3740, inner loop: `attempts` iterations for one mesh of the layer.
	void scatterMesh(const MeshParams& mesh, int attempts, std::int64_t biomeIndex)
	{
		int done = 0; // local_res10
		int remaining = 0; // iVar10: members still to place, <= 0 outside a cluster
		float centreX = 0.0f;
		float centreZ = 0.0f;
		float clusterRadius = 0.0f;
		while (done < attempts)
		{
			--remaining;
			float x = 0.0f;
			float z = 0.0f;
			if (!mesh.clustered || remaining < 1)
			{
				if (mesh.clustered)
				{
					remaining = static_cast<int>(rng_->uniformInt(
						rangeOf(mesh.clusterNumMin, mesh.clusterNumMax))) + mesh.clusterNumMin;
					clusterRadius = rng_->uniform(mesh.clusterRadiusMin, mesh.clusterRadiusMax);
				}
				x = rng_->uniform(minX_, maxX_);
				z = rng_->uniform(minZ_, maxZ_);
				if (mesh.grassLimited && !grassPasses(x, z))
				{
					// 1406d3740:143-163: re-roll (both draws) up to kGrassRerolls more times; every draw is consumed.
					int rerolls = 0;
					bool passed = false;
					while (!passed && rerolls < kGrassRerolls)
					{
						++rerolls;
						x = rng_->uniform(minX_, maxX_);
						z = rng_->uniform(minZ_, maxZ_);
						passed = grassPasses(x, z);
					}
					if (!passed)
					{
						// Abandoned: nothing is placed, the attempt is still counted, a cluster being drawn is dropped.
						remaining = 0;
						++done;
						continue;
					}
				}
				centreX = x;
				centreZ = z;
			} else {
				float dz = rng_->uniform(-1.0f, 1.0f);
				float dx = rng_->uniform(-1.0f, 1.0f);
				normalise(dx, dz);
				const float reach = rng_->uniform(kClusterMinOffset, clusterRadius);
				x = dx * reach + centreX;
				z = dz * reach + centreZ;
			}

			if (biomeIndex < 0 || static_cast<std::int64_t>(biomes_.biomeAt(x, z)) == biomeIndex)
			{
				if (!place(mesh, x, z, false, 0))
					remaining = 0;
			} else {
				// Wrong biome: the rest of the cluster is charged to the budget.
				if (remaining > 0)
					done += remaining;
				remaining = 0;
			}
			++done;
		}
	}

	ZoneCell cell_;
	const Heightmap& terrain_;
	const BiomeMap& biomes_;
	const kdata::GameData& gameData_;
	FoliageParams params_;
	float minX_;
	float minZ_;
	float maxX_;
	float maxZ_;
	std::vector<float> grid_;
	std::vector<Disc> erasers_;
	std::vector<Disc> towns_;
	const RoadOverlay* roads_ = nullptr;
	std::unordered_map<std::uint64_t, std::uint8_t> grassSpots_; // the overlay's G, written by this zone's placements (starts at 0)
	std::unordered_map<std::string, std::unique_ptr<MeshParams>> meshes_;
	std::optional<FoliageRng> rng_;
	std::vector<FoliageInstance>* out_ = nullptr;
};

// BIOMES records by `index`; the last definition wins.
std::unordered_map<std::int64_t, const kdata::Record*> biomeRecordsByIndex(const kdata::GameData& gameData)
{
	std::unordered_map<std::int64_t, const kdata::Record*> records;
	for (const kdata::Record* record : gameData.byType(itemTypeByName("BIOMES")))
		records[record->intOr("index", 0)] = record;
	return records;
}

// The zone's biome list (kinspect navhash sections 11 and 14): the zone's tile object carries a list of its biomes, and
// the scatter walks it in that order. The list is not map order: it holds the zone's biomes by how much ground each
// covers in the zone, the widest first, and each biome's FOLIAGE_LAYERs run in that position of the spawn order.
// MEASURED against the live lists: zone 10,11 is [Red Coast (162 px), Cinder Ridge (94 px)] -- the map's first-seen order
// is the reverse -- and the Hub is [Swampfeeder Canyon (252 px), Canyonland FlatTop (4 px)]. Ties keep first-seen order.
// UNSURE: the game builds the list per biome-map cell (a 9216-u cell holding up to 5 biome slots, a per-tile mask); with
// only those two live lists the per-tile pixel count is the rule that fits both, and the cell's own counts do not.
std::vector<std::int64_t> zoneBiomeList(ZoneCell cell, const BiomeMap& biomes,
	const std::unordered_map<std::int64_t, const kdata::Record*>& biomeRecords)
{
	const double unit = biomes.unitsPerPixel();
	const auto span = [unit](double lo) {
		return std::pair<int, int>(
			static_cast<int>(std::floor((lo - kWorldMin) / unit)),
			static_cast<int>(std::ceil((lo + kZoneSize - kWorldMin) / unit)));
	};
	const auto [col0, col1] = span(zoneMinX(cell.x));
	const auto [row0, row1] = span(zoneMinZ(cell.z));
	std::vector<std::int64_t> zoneBiomes;
	std::unordered_set<std::int64_t> seen;
	std::unordered_map<std::int64_t, std::size_t> pixels;
	for (int row = std::max(row0, 0); row < std::min(row1, biomes.height()); ++row)
		for (int col = std::max(col0, 0); col < std::min(col1, biomes.width()); ++col)
		{
			const std::int64_t index = biomes.pixel(col, row);
			if (biomeRecords.count(index) == 0)
				continue;
			++pixels[index];
			if (seen.insert(index).second)
				zoneBiomes.push_back(index);
		}
	std::stable_sort(zoneBiomes.begin(), zoneBiomes.end(),
		[&pixels](std::int64_t a, std::int64_t b) { return pixels[a] > pixels[b]; });
	return zoneBiomes;
}

// A layer the mesh scatter runs: a FOLIAGE_LAYER of the biome without a grass renderer.
bool treeLayer(const kdata::Record* layer)
{
	if (layer == nullptr || layer->type != itemTypeByName("FOLIAGE_LAYER"))
		return false;
	const auto* grass = layer->findRefs("grass");
	return grass == nullptr || grass->entries.empty();
}

} // namespace

FoliageRng::FoliageRng(std::uint32_t seed)
{
	state_[0] = seed;
	for (std::size_t i = 1; i < state_.size(); ++i)
		state_[i] = 0x6c078965u * (state_[i - 1] ^ (state_[i - 1] >> 30))
			+ static_cast<std::uint32_t>(i);
	twist(); // 1406d3740 regenerates right after seeding
}

void FoliageRng::twist()
{
	constexpr std::size_t n = 624;
	constexpr std::size_t m = 397;
	const auto mix = [](std::uint32_t a, std::uint32_t b, std::uint32_t partner) {
		const std::uint32_t y = (a & kUpperMask) | (b & kLowerMask);
		return partner ^ (y >> 1) ^ ((b & 1u) != 0 ? 0x9908b0dfu : 0u);
	};
	for (std::size_t k = 0; k < n - m; ++k)
		state_[k] = mix(state_[k], state_[k + 1], state_[k + m]);
	for (std::size_t k = n - m; k < n - 1; ++k)
		state_[k] = mix(state_[k], state_[k + 1], state_[k + m - n]);
	state_[n - 1] = mix(state_[n - 1], state_[0], state_[m - 1]);
	index_ = 0;
}

std::uint32_t FoliageRng::next()
{
	if (index_ >= state_.size())
		twist();
	std::uint32_t y = state_[index_++];
	y ^= y >> 11;
	y ^= (y & 0xff3a58adu) << 7; // not the standard 0x9d2c5680
	y ^= (y & 0xffffdf8cu) << 15; // not the standard 0xefc60000
	y ^= y >> 18;
	return y;
}

float FoliageRng::uniform(float a, float b)
{
	constexpr double kInv32 = 2.3283064365386963e-10; // 2^-32
	const double y = next();
	return static_cast<float>(y * kInv32 * static_cast<double>(b - a) + static_cast<double>(a));
}

std::uint32_t FoliageRng::uniformInt(std::uint32_t max)
{
	std::uint32_t mask = max >> 1 | max;
	mask |= mask >> 2;
	mask |= mask >> 4;
	mask |= mask >> 8;
	mask |= mask >> 16;
	std::uint32_t value;
	do
		value = next() & mask;
	while (max < value);
	return value;
}

std::uint32_t foliageLayerSeed(int zoneMinX, int zoneMinZ, int layerRecordID)
{
	std::uint32_t n = static_cast<std::uint32_t>(zoneMinZ) * 57u
		+ static_cast<std::uint32_t>(zoneMinX);
	n ^= n << 13;
	const std::uint32_t r = (((n * n * 0xec4du + 0x131071fu) * n + 0xd208dd0du) & 0x7fffffffu);
	// (int)(r * 2^-30 * 16777215 - 16777215), double arithmetic, truncating.
	const double term = static_cast<double>(r) * 9.313225746154785e-10 * 16777215.0 - 16777215.0;
	return static_cast<std::uint32_t>(layerRecordID) - static_cast<std::uint32_t>(
		static_cast<std::int32_t>(term));
}

int leadingRecordID(std::string_view stringID)
{
	std::size_t i = 0;
	while (i < stringID.size() && (stringID[i] == ' ' || stringID[i] == '\t'))
		++i;
	bool negative = false;
	if (i < stringID.size() && (stringID[i] == '-' || stringID[i] == '+'))
		negative = stringID[i++] == '-';
	long long value = 0;
	for (; i < stringID.size() && stringID[i] >= '0' && stringID[i] <= '9'; ++i)
	{
		value = value * 10 + (stringID[i] - '0');
		if (value > INT_MAX)
			return negative ? INT_MIN : INT_MAX;
	}
	return static_cast<int>(negative ? -value : value);
}

std::vector<FoliageInstance> generateFoliage(ZoneCell cell,
	const Heightmap& terrain, const BiomeMap& biomes,
	const kdata::GameData& gameData, const Features& features,
	const Towns& towns, const FoliageParams& params)
{
	std::vector<FoliageInstance> out;
	if (cell.x < 0 || cell.z < 0 || cell.x >= kZoneCellsPerAxis || cell.z >= kZoneCellsPerAxis)
		return out;

	const auto biomeRecords = biomeRecordsByIndex(gameData);
	const std::vector<std::int64_t> zoneBiomes = zoneBiomeList(cell, biomes, biomeRecords);
	if (zoneBiomes.empty())
		return out;

	ZoneScatter scatter(cell, terrain, biomes, gameData, params);

	// Exclusion discs, in world units, trimmed to those that can reach this zone.
	const double minX = zoneMinX(cell.x) - kEraserReach;
	const double maxX = zoneMinX(cell.x) + kZoneSize + kEraserReach;
	const double minZ = zoneMinZ(cell.z) - kEraserReach;
	const double maxZ = zoneMinZ(cell.z) + kZoneSize + kEraserReach;
	std::vector<Disc> erasers;
	for (const RoadFoliagePoint& point : features.roadFoliage())
		if (!point.deleted && point.u > 0.0 && point.x >= minX && point.x <= maxX
			&& point.z >= minZ && point.z <= maxZ)
			erasers.push_back({ point.x, point.z, point.u * point.u });
	std::vector<Disc> townDiscs;
	for (const TownPlacement& placement : towns.placements())
	{
		const kdata::Record* record = gameData.find(placement.targetID);
		if (record == nullptr)
			continue;
		// 140927f10 never picks a type 8 town (the two nest markers).
		if (record->intOr("type", 0) == 8)
			continue;
		double range = record->floatOr("no-foliage range",
			static_cast<float>(kDefaultTownRange));
		if (range == 0.0 && params.zeroTownRangeIsDefault)
			range = kDefaultTownRange;
		townDiscs.push_back({ placement.x, placement.z, range * range });
	}
	scatter.setDiscs(std::move(erasers), std::move(townDiscs), features.roadOverlay());

	// A single-biome zone runs its layers without the biome test (param_5 = -1). Grass layers go to the client-side grass
	// renderer, not the mesh scatter (1406d3e30).
	for (const std::int64_t biomeIndex : zoneBiomes)
	{
		const kdata::Record& biome = *biomeRecords.at(biomeIndex);
		const auto* layers = biome.findRefs("foliage");
		if (layers == nullptr)
			continue;
		for (const auto& ref : layers->entries)
		{
			const kdata::Record* layer = gameData.find(ref.targetID);
			if (!treeLayer(layer))
				continue;
			scatter.runLayer(*layer, zoneBiomes.size() >= 2 ? biomeIndex : -1, out);
		}
	}
	return out;
}

bool zoneHasTreeLayer(ZoneCell cell, const BiomeMap& biomes, const kdata::GameData& gameData)
{
	if (cell.x < 0 || cell.z < 0 || cell.x >= kZoneCellsPerAxis || cell.z >= kZoneCellsPerAxis)
		return false;
	const auto biomeRecords = biomeRecordsByIndex(gameData);
	for (const std::int64_t biomeIndex : zoneBiomeList(cell, biomes, biomeRecords))
	{
		const auto* layers = biomeRecords.at(biomeIndex)->findRefs("foliage");
		if (layers == nullptr)
			continue;
		for (const auto& ref : layers->entries)
			if (treeLayer(gameData.find(ref.targetID)))
				return true;
	}
	return false;
}

} // namespace kworld
