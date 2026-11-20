// kenshi-k16.6.253: "a character never stands inside a solid prop". The placed collision of the zones around a point (the bake's own AreaGatherer
// shapes: Obstacle and Projected closed solids, i.e. furniture, cages, crates, but not the walkable building shells) and a point-in-solid test for a
// character's (x, y, z), for scenario tests: PropClip clip(gameData); clip.inside(x, y, z) -> a description of the solid it is in, or "".
// A box is tested in its own frame (WorldShape::axes / half about the AABB centre), a convex hull by its triangle planes; a concave closed shape is skipped.
#pragma once

#include "kdata/gamedata.hpp"
#include "knav/navmesh.hpp"
#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"
#include "kworld/heightmap.hpp"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>

namespace prop_clip_detail { inline constexpr double kPi = 3.14159265358979323846; } // M_PI is not in MinGW's strict -std=c++20 <cmath>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace prop_clip {

class PropClip
{
public:
	explicit PropClip(const kdata::GameData& gameData)
		: terrain_(kworld::Heightmap::open(std::string(KENSHI_DATA) + "/newland/land/fullmap.tif")), gatherer_(KENSHI_DATA, terrain_, gameData) {}

	// "" when (x, y, z) is in no solid prop; else the building id, record and the shape's AABB. `margin`: shrinks every solid by that much
	// (a standing character is a point; 0 = the strict collision).
	std::string inside(double x, double y, double z, double margin = 0.0, bool doorsWin = false)
	{
		const kworld::ZoneCell zone = kworld::zoneCellOf(x, z);
		auto found = areas_.find({ zone.x, zone.z });
		if (found == areas_.end())
			found = areas_.emplace(std::make_pair(zone.x, zone.z), gatherer_.gather(zone, zone)).first;
		for (const kworld::AreaBuilding& building : found->second.buildings)
		{
			if (!building.shapes)
				continue;
			for (const kworld::WorldShape& shape : *building.shapes)
			{
				if (!knav::propClearanceShape(shape))
					continue;
				if (x < shape.min[0] || x > shape.max[0] || y < shape.min[1] || y > shape.max[1] || z < shape.min[2] || z > shape.max[2])
					continue;
				if (contains(shape, x, y, z, margin) && !(doorsWin && inDoorway(*building.shapes, x, y, z)))
				{
					char text[256];
					std::snprintf(text, sizeof text, "%s %s box (%.1f %.1f %.1f)..(%.1f %.1f %.1f) use %d axes (%.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f) half (%.1f %.1f %.1f)", building.id.c_str(), building.placed.buildingID.c_str(),
						shape.min[0], shape.min[1], shape.min[2], shape.max[0], shape.max[1], shape.max[2], static_cast<int>(shape.use), shape.axes[0], shape.axes[1], shape.axes[2], shape.axes[3], shape.axes[4], shape.axes[5], shape.axes[6], shape.axes[7], shape.axes[8], shape.half[0], shape.half[1], shape.half[2]);
					return text;
				}
			}
		}
		return {};
	}

	// The closed Obstacle / Projected solids whose AABB is within `reach` of (x, z), one line each (for a failing check's message).
	std::string near(double x, double z, double reach)
	{
		const kworld::ZoneCell zone = kworld::zoneCellOf(x, z);
		auto found = areas_.find({ zone.x, zone.z });
		if (found == areas_.end())
			found = areas_.emplace(std::make_pair(zone.x, zone.z), gatherer_.gather(zone, zone)).first;
		std::string out;
		for (const kworld::AreaBuilding& building : found->second.buildings)
		{
			if (!building.shapes)
				continue;
			for (const kworld::WorldShape& shape : *building.shapes)
			{
				if (!knav::propClearanceShape(shape))
					continue;
				if (x < shape.min[0] - reach || x > shape.max[0] + reach || z < shape.min[2] - reach || z > shape.max[2] + reach)
					continue;
				char text[200];
				std::snprintf(text, sizeof text, "\n  solid %s AABB (%.1f %.1f %.1f)..(%.1f %.1f %.1f) use %d half (%.1f %.1f %.1f)", building.placed.buildingID.c_str(),
					shape.min[0], shape.min[1], shape.min[2], shape.max[0], shape.max[1], shape.max[2], static_cast<int>(shape.use), shape.half[0], shape.half[1], shape.half[2]);
				out += text;
			}
		}
		return out;
	}

	// The nearest furniture footprint (WorldShape::furniture, the set the bake's ring uses) to (x, y, z) in xz, over the 3x3 zones round it: its xz distance
	// (0 = inside the footprint), whether the point is in its ring (footprint + kFurnitureBodyClearance + half a cell, inside its height window; k16.7.89),
	// and a one-line description. "" when no furniture is within 64 u.
	std::string ring(double x, double y, double z, bool& inRing, double& distance)
	{
		inRing = false;
		distance = 1e9;
		std::string best;
		const knav::NavConfig config;
		const double ringRadius = knav::kFurnitureBodyClearance + 0.5 * config.cellSize;
		const kworld::ZoneCell centre = kworld::zoneCellOf(x, z);
		for (int dz = -1; dz <= 1; ++dz)
			for (int dx = -1; dx <= 1; ++dx)
			{
				auto found = areas_.find({ centre.x + dx, centre.z + dz });
				if (found == areas_.end())
					found = areas_.emplace(std::make_pair(centre.x + dx, centre.z + dz), gatherer_.gather({ centre.x + dx, centre.z + dz }, { centre.x + dx, centre.z + dz })).first;
				for (const kworld::AreaBuilding& building : found->second.buildings)
				{
					if (!building.shapes)
						continue;
					for (const kworld::WorldShape& shape : *building.shapes)
					{
						if (!knav::propClearanceShape(shape))
							continue;
						std::vector<float> hull;
						knav::propClearanceHull(shape, 0.0f, hull);
						const std::size_t n = hull.size() / 3;
						if (n < 3)
							continue;
						bool insideHull = true;
						double d2 = 1e18;
						for (std::size_t i = 0; i < n; ++i)
						{
							const double ax = hull[3 * i], az = hull[3 * i + 2];
							const double bx = hull[3 * ((i + 1) % n)], bz = hull[3 * ((i + 1) % n) + 2];
							if ((bx - ax) * (z - az) - (bz - az) * (x - ax) < 0.0)
								insideHull = false;
							const double ex = bx - ax, ez = bz - az;
							const double t = std::fmin(std::fmax(((x - ax) * ex + (z - az) * ez) / (ex * ex + ez * ez + 1e-12), 0.0), 1.0);
							d2 = std::fmin(d2, (x - ax - t * ex) * (x - ax - t * ex) + (z - az - t * ez) * (z - az - t * ez));
						}
						const double d = insideHull ? 0.0 : std::sqrt(d2);
						if (d >= distance || d > 64.0)
							continue;
						distance = d;
						const bool projected = shape.use == kworld::NavUse::Projected;
						inRing = d <= ringRadius && (projected || (y >= shape.min[1] - config.cellHeight && y <= shape.max[1] + 2 * config.cellHeight));
						char text[200];
						std::snprintf(text, sizeof text, "%s %s AABB (%.1f %.1f %.1f)..(%.1f %.1f %.1f)", building.id.c_str(), building.placed.buildingID.c_str(),
							shape.min[0], shape.min[1], shape.min[2], shape.max[0], shape.max[1], shape.max[2]);
						best = text;
					}
				}
			}
		return best;
	}

	// "" when a standing body of horizontal `radius` at (x, y, z) clears every solid; else the first solid it overlaps: the centre and 16 points
	// on the body's rim tested strictly (a margin grown into the solid would also grow it vertically, and a floor under the feet is no clip).
	std::string touches(double x, double y, double z, double radius)
	{
		std::string in = inside(x, y, z);
		for (int i = 0; in.empty() && i < 16 && radius > 0.0; ++i)
		{
			const double a = i * (prop_clip_detail::kPi / 8.0);
			in = inside(x + radius * std::sin(a), y, z + radius * std::cos(a));
		}
		return in;
	}

private:
	// The bake's door mat (knav kDoorMargin, 8 u round a door's footprint): the passage through a wall's solid, which the clearance never nulls.
	static bool inDoorway(const std::vector<kworld::WorldShape>& shapes, double x, double y, double z)
	{
		for (const kworld::WorldShape& door : shapes)
			if (door.use == kworld::NavUse::Door && contains(door, x, y, z, -8.0))
				return true;
		return false;
	}

	static bool contains(const kworld::WorldShape& shape, double x, double y, double z, double margin)
	{
		if (shape.half[0] > 0.0f)
		{
			const double d[3] = { x - 0.5 * (shape.min[0] + shape.max[0]), y - 0.5 * (shape.min[1] + shape.max[1]), z - 0.5 * (shape.min[2] + shape.max[2]) };
			for (int axis = 0; axis < 3; ++axis)
			{
				const double along = d[0] * shape.axes[3 * axis] + d[1] * shape.axes[3 * axis + 1] + d[2] * shape.axes[3 * axis + 2];
				if (std::fabs(along) > shape.half[axis] - margin)
					return false;
			}
			return true;
		}
		const std::vector<float>& v = shape.vertices;
		const std::size_t triangles = shape.indices.size() / 3;
		const auto planeOf = [&](std::size_t t, double n[3], double& d)
		{
			const float* a = &v[3 * shape.indices[3 * t]];
			const float* b = &v[3 * shape.indices[3 * t + 1]];
			const float* c = &v[3 * shape.indices[3 * t + 2]];
			const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
			const double e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			n[0] = e1[1] * e2[2] - e1[2] * e2[1];
			n[1] = e1[2] * e2[0] - e1[0] * e2[2];
			n[2] = e1[0] * e2[1] - e1[1] * e2[0];
			const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			if (len < 1e-9)
				return false;
			for (int i = 0; i < 3; ++i)
				n[i] /= len;
			d = n[0] * a[0] + n[1] * a[1] + n[2] * a[2];
			return true;
		};
		// convex only: every vertex on the inner side of every face
		for (std::size_t t = 0; t < triangles; ++t)
		{
			double n[3], d;
			if (!planeOf(t, n, d))
				continue;
			for (std::size_t i = 0; i + 2 < v.size(); i += 3)
				if (n[0] * v[i] + n[1] * v[i + 1] + n[2] * v[i + 2] > d + 0.01)
					return false;
		}
		for (std::size_t t = 0; t < triangles; ++t)
		{
			double n[3], d;
			if (planeOf(t, n, d) && n[0] * x + n[1] * y + n[2] * z > d - margin)
				return false;
		}
		return triangles > 0;
	}

	kworld::Heightmap terrain_;
	kworld::AreaGatherer gatherer_;
	std::map<std::pair<int, int>, kworld::AreaGeometry> areas_;
};

} // namespace prop_clip
