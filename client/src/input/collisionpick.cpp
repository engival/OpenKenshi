#include "input/collisionpick.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>

#include "kworld/collision_area.hpp"
#include "kworld/coords.hpp"

namespace
{
	// Slab test against an AABB given relative to the ray origin; true when the ray enters it before maxT.
	bool hitsBox(const double lo[3], const double hi[3], const double dir[3], double maxT)
	{
		double t0 = 0, t1 = maxT;
		for (int k = 0; k < 3; k++)
		{
			if (std::abs(dir[k]) < 1e-12)
			{
				if (lo[k] > 0 || hi[k] < 0) { return false; }
				continue;
			}
			double a = lo[k] / dir[k], b = hi[k] / dir[k];
			if (a > b) { std::swap(a, b); }
			t0 = std::max(t0, a);
			t1 = std::min(t1, b);
			if (t0 > t1) { return false; }
		}
		return true;
	}
}

CollisionPick::~CollisionPick()
{
	{
		std::lock_guard<std::mutex> lk(m_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable()) { thread_.join(); }
}

void CollisionPick::start(const std::string& dataDir, const kworld::Heightmap* terrain)
{
	if (thread_.joinable()) { return; }	// world data: a second session (end_session, then a new server) keeps the running gatherer
	thread_ = std::thread(&CollisionPick::worker, this, dataDir, terrain);
}

void CollisionPick::update(double x, double z)
{
	const kworld::ZoneCell c = kworld::zoneCellOf(x, z);
	std::lock_guard<std::mutex> lk(m_);
	if (c.x == want_[0] && c.z == want_[1]) { return; }
	want_[0] = c.x;
	want_[1] = c.z;
	cv_.notify_all();
}

void CollisionPick::worker(std::string dataDir, const kworld::Heightmap* terrain)
{
	std::unique_ptr<kworld::AreaGatherer> gatherer;
	try { gatherer = std::make_unique<kworld::AreaGatherer>(dataDir, *terrain); }
	catch (const std::exception& e)
	{
		fprintf(stderr, "collision pick: %s\n", e.what());
		std::lock_guard<std::mutex> lk(m_);
		failed_ = true;
		return;
	}
	for (;;)
	{
		int cell[2];
		{
			std::unique_lock<std::mutex> lk(m_);
			cv_.wait(lk, [&] { return stop_ || (want_[0] >= 0 && (want_[0] != have_[0] || want_[1] != have_[1])); });
			if (stop_) { return; }
			cell[0] = have_[0] = want_[0];
			cell[1] = have_[1] = want_[1];
		}
		// the camera's cell and its neighbours (the gatherer adds one more ring for overhangs)
		const auto start = std::chrono::steady_clock::now();
		const kworld::ZoneCell lo{ std::max(cell[0] - 1, 0), std::max(cell[1] - 1, 0) };
		const kworld::ZoneCell hi{ std::min(cell[0] + 1, kworld::kZoneCellsPerAxis - 1), std::min(cell[1] + 1, kworld::kZoneCellsPerAxis - 1) };
		auto shapes = std::make_shared<std::vector<kworld::WorldShape>>();
		auto names = std::make_shared<std::vector<std::string>>();
		size_t buildings = 0;
		try
		{
			const kworld::AreaGeometry area = gatherer->gather(lo, hi);
			*shapes = area.flatShapes();
			buildings = shapes->size() - area.foliage->size();	// flatShapes: buildings, then foliage
			// The names in flatShapes order (buildings sorted by id, a PathMode::Ignore placement left out): the placement, its record and the
			// shape's index, so a hit says which building's collision (dump feetfloor, kenshi-k16.17.7.24.4)
			std::vector<const kworld::AreaBuilding*> sorted;
			for (const kworld::AreaBuilding& b : area.buildings) { sorted.push_back(&b); }
			std::sort(sorted.begin(), sorted.end(), [](const kworld::AreaBuilding* a, const kworld::AreaBuilding* b) { return a->id < b->id; });
			names->reserve(shapes->size());
			for (const kworld::AreaBuilding* b : sorted)
			{
				if (b->nav.pathMode == kworld::PathMode::Ignore) { continue; }
				for (size_t k = 0; k < b->shapes->size(); k++) { names->push_back(b->id + " " + b->placed.buildingID + " shape " + std::to_string(k)); }
			}
			for (size_t k = 0; k < area.foliage->size(); k++) { names->push_back("foliage shape " + std::to_string(k)); }
			if (names->size() != shapes->size()) { names->clear(); }	// a shape count mismatch: no names rather than the wrong ones
		}
		catch (const std::exception& e) { fprintf(stderr, "collision pick: zone %d,%d: %s\n", cell[0], cell[1], e.what()); }
		printf("collision pick: zone %d,%d: %zu shapes, %.0f ms\n", cell[0], cell[1], shapes->size(),
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		std::lock_guard<std::mutex> lk(m_);
		shapes_ = std::move(shapes);
		names_ = std::move(names);
		buildingShapes_ = buildings;
		done_[0] = cell[0];
		done_[1] = cell[1];
	}
}

bool CollisionPick::ray(const double origin[3], const double dir[3], double maxT, double& t, float bounds[6], const DoorOffset& door, kworld::DoorKey* key, bool* solid, bool* building, std::string* hitName) const
{
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes;
	std::shared_ptr<const std::vector<std::string>> names;
	size_t buildings;
	{
		std::lock_guard<std::mutex> lk(m_);
		shapes = shapes_;
		names = names_;
		buildings = buildingShapes_;
	}
	if (!shapes) { return false; }
	double best = maxT;
	const kworld::WorldShape* hit = nullptr;
	for (const kworld::WorldShape& s : *shapes)
	{
		if ((s.use == kworld::NavUse::Door) != (bool)door) { continue; }
		double o[3] = {};	// the shape's offset, applied to the ray instead
		if (door && !door(s, o)) { continue; }
		for (int k = 0; k < 3; k++) { o[k] = origin[k] - o[k]; }
		const double lo[3] = { s.min[0] - o[0], s.min[1] - o[1], s.min[2] - o[2] };
		const double hi[3] = { s.max[0] - o[0], s.max[1] - o[1], s.max[2] - o[2] };
		if (!hitsBox(lo, hi, dir, best)) { continue; }
		// Moller-Trumbore, both faces, in doubles relative to the origin (world coordinates are ~1e5)
		for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
		{
			double v[3][3];
			for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { v[j][k] = s.vertices[s.indices[i + j] * 3 + k] - o[k]; } }
			const double e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
			const double e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
			const double p[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0] };
			const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
			if (std::abs(det) < 1e-12) { continue; }
			const double inv = 1 / det;
			const double s0[3] = { -v[0][0], -v[0][1], -v[0][2] };
			const double u = (s0[0] * p[0] + s0[1] * p[1] + s0[2] * p[2]) * inv;
			if (u < 0 || u > 1) { continue; }
			const double q[3] = { s0[1] * e1[2] - s0[2] * e1[1], s0[2] * e1[0] - s0[0] * e1[2], s0[0] * e1[1] - s0[1] * e1[0] };
			const double w = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * inv;
			if (w < 0 || u + w > 1) { continue; }
			const double d = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
			if (d > 0 && d < best) { best = d; hit = &s; }
		}
	}
	if (best >= maxT) { return false; }
	t = best;
	if (bounds) { for (int k = 0; k < 3; k++) { bounds[k] = hit->min[k]; bounds[3 + k] = hit->max[k]; } }
	if (key) { *key = hit->door; }
	if (solid) { *solid = hit->solid; }
	if (building) { *building = (size_t)(hit - shapes->data()) < buildings; }
	if (hitName)
	{
		const size_t index = (size_t)(hit - shapes->data());
		*hitName = names && index < names->size() ? (*names)[index] : "shape " + std::to_string(index);
	}
	return true;
}

int CollisionPick::groundY(double x, double z, double wireY, double up, double down, double& y, const void** gen) const
{
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes;
	size_t buildings;
	int done[2];
	{
		std::lock_guard<std::mutex> lk(m_);
		shapes = shapes_;
		buildings = buildingShapes_;
		done[0] = done_[0];
		done[1] = done_[1];
	}
	if (gen) { *gen = shapes.get(); }
	if (!shapes) { return -1; }
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	if (std::abs(cell.x - done[0]) > 1 || std::abs(cell.z - done[1]) > 1) { return -1; }
	const double top = wireY + up, bottom = wireY - down;
	double best = 1e30;
	bool found = false;
	const size_t n = std::min(buildings, shapes->size());
	for (size_t si = 0; si < n; si++)
	{
		const kworld::WorldShape& s = (*shapes)[si];
		if (s.use == kworld::NavUse::Door) { continue; }
		if (x < s.min[0] || x > s.max[0] || z < s.min[2] || z > s.max[2] || s.max[1] < bottom || s.min[1] > top) { continue; }
		const float box[6] = { (float)s.min[0], (float)s.min[1], (float)s.min[2], (float)s.max[0], (float)s.max[1], (float)s.max[2] };
		if (thin(box)) { continue; }
		for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
		{
			double v[3][3];
			for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { v[j][k] = s.vertices[s.indices[i + j] * 3 + k]; } }
			// 2D point-in-triangle on (x, z), then the plane's y there
			const double d = (v[1][2] - v[2][2]) * (v[0][0] - v[2][0]) + (v[2][0] - v[1][0]) * (v[0][2] - v[2][2]);
			if (std::abs(d) < 1e-9) { continue; }
			const double a = ((v[1][2] - v[2][2]) * (x - v[2][0]) + (v[2][0] - v[1][0]) * (z - v[2][2])) / d;
			const double b = ((v[2][2] - v[0][2]) * (x - v[2][0]) + (v[0][0] - v[2][0]) * (z - v[2][2])) / d;
			const double c = 1 - a - b;
			if (a < 0 || b < 0 || c < 0) { continue; }
			const double hy = a * v[0][1] + b * v[1][1] + c * v[2][1];
			if (hy > top || hy < bottom) { continue; }
			if (!found || std::abs(hy - wireY) < std::abs(best - wireY)) { best = hy; found = true; }
		}
	}
	if (!found) { return 0; }
	y = best;
	return 1;
}

// One wall face (|normal y| < 0.7) of a shape near a grid cell, with everything depenetrate() derives from it that does not depend on the body's position.
struct NudgeFace { double v0x, v0z, hx, hz, tx, tz, t0, t1, ymin, ymax; };
struct NudgeItem { size_t si; std::vector<NudgeFace> faces; };
struct CollisionPick::NudgeCell
{
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes;	// keeps the gather the indices point into alive
	std::vector<size_t> doors;
	std::vector<NudgeItem> items;
};

namespace
{
	constexpr double kNudgeCellSize = 8.0, kNudgeMaxR = 1.0;	// radii up to kNudgeMaxR are served from a cell's cache
	constexpr double kNudgeHalfDiag = 0.7072 * kNudgeCellSize;	// a body in the cell is at most this far from its centre
}

bool CollisionPick::depenetrate(double x, double z, double y, double radius, double maxPush, double out[2], std::string* why) const
{
	out[0] = out[1] = 0;
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes;
	size_t buildings;
	int done[2];
	{
		std::lock_guard<std::mutex> lk(m_);
		shapes = shapes_;
		buildings = buildingShapes_;
		done[0] = done_[0];
		done[1] = done_[1];
	}
	if (!shapes) { return false; }
	const kworld::ZoneCell cell = kworld::zoneCellOf(x, z);
	if (std::abs(cell.x - done[0]) > 1 || std::abs(cell.z - done[1]) > 1) { return false; }
	const double lo = y + 0.35, hi = y + 1.5;
	const size_t n = std::min(buildings, shapes->size());
	// The cell's candidates (kenshi-k16.2.350): the geometry is static, so the shapes and faces that can matter to a body in this cell are gathered once
	const int gx = (int)std::floor(x / kNudgeCellSize), gz = (int)std::floor(z / kNudgeCellSize);
	const int64_t key = ((int64_t)gx << 32) ^ (int64_t)(uint32_t)gz;
	std::shared_ptr<NudgeCell> nc;
	if (radius <= kNudgeMaxR)
	{
		std::lock_guard<std::mutex> lk(nudgeM_);
		if (nudgeGen_ != shapes.get()) { nudgeCells_.clear(); nudgeGen_ = shapes.get(); }
		auto it = nudgeCells_.find(key);
		if (it != nudgeCells_.end()) { nc = it->second; }
		else
		{
			nc = std::make_shared<NudgeCell>();
			nc->shapes = shapes;
			const double c0x = gx * kNudgeCellSize, c0z = gz * kNudgeCellSize, c1x = c0x + kNudgeCellSize, c1z = c0z + kNudgeCellSize;
			const double ccx = 0.5 * (c0x + c1x), ccz = 0.5 * (c0z + c1z);
			for (size_t si = 0; si < n; si++)
			{
				const kworld::WorldShape& s = (*shapes)[si];
				if (c1x < s.min[0] - kNudgeMaxR || c0x > s.max[0] + kNudgeMaxR || c1z < s.min[2] - kNudgeMaxR || c0z > s.max[2] + kNudgeMaxR) { continue; }
				if (s.use == kworld::NavUse::Door) { nc->doors.push_back(si); continue; }
				if (s.solid) { continue; }
				const double cx = 0.5 * (s.min[0] + s.max[0]), cz = 0.5 * (s.min[2] + s.max[2]);
				NudgeItem item; item.si = si;				for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
				{
					double v[3][3];
					for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { v[j][k] = s.vertices[s.indices[i + j] * 3 + k]; } }
					const double e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] }, e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
					const double nrm[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
					const double nl = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
					if (nl < 1e-9 || std::fabs(nrm[1] / nl) >= 0.7) { continue; }
					NudgeFace f;
					double hx = nrm[0], hz = nrm[2];
					const double hl = std::hypot(hx, hz);
					hx /= hl; hz /= hl;
					if (s.closed && hx * (cx - v[0][0]) + hz * (cz - v[0][2]) > 0) { hx = -hx; hz = -hz; }
					f.v0x = v[0][0]; f.v0z = v[0][2]; f.hx = hx; f.hz = hz; f.tx = -hz; f.tz = hx;
					f.t0 = 1e30; f.t1 = -1e30;
					for (int j = 0; j < 3; j++) { const double t = f.tx * (v[j][0] - v[0][0]) + f.tz * (v[j][2] - v[0][2]); f.t0 = std::min(f.t0, t); f.t1 = std::max(f.t1, t); }
					f.ymin = std::min({ v[0][1], v[1][1], v[2][1] }); f.ymax = std::max({ v[0][1], v[1][1], v[2][1] });
					// Keep the face when it can matter to a body in the cell: its segment passes within a radius (nearest-point push), or, for a closed shape,
					// its plane is within 1.5 radii of the cell (the inside test's largest signed distance; a face further behind never is that maximum)
					const double tp = std::clamp(f.tx * (ccx - f.v0x) + f.tz * (ccz - f.v0z), f.t0, f.t1);
					const double dseg = std::hypot(ccx - (f.v0x + f.tx * tp), ccz - (f.v0z + f.tz * tp));
					const double dpl = f.hx * (ccx - f.v0x) + f.hz * (ccz - f.v0z);
					const bool near = dseg < kNudgeMaxR + kNudgeHalfDiag + 1e-6, inside = s.closed && dpl + kNudgeHalfDiag + 1e-6 >= -1.5 * kNudgeMaxR;
					if (near || inside) { item.faces.push_back(f); }
				}
				if (!item.faces.empty()) { nc->items.push_back(std::move(item)); }
			}
			nudgeCells_.emplace(key, nc);
		}
	}
	double best[2] = { 0, 0 }, bestLen = 0;
	size_t bestShape = 0;
	bool bestInside = false;
	if (nc)
	{
		// In a doorway (inside a door shape's footprint, grown by a radius) the body is where the passage is: no push, so a walker through a door never sways
		for (const size_t si : nc->doors)
		{
			const kworld::WorldShape& s = (*shapes)[si];
			if (x >= s.min[0] - radius && x <= s.max[0] + radius && z >= s.min[2] - radius && z <= s.max[2] + radius && s.max[1] >= y - 0.5 && s.min[1] <= y + 2.5) { return false; }
		}
		for (const NudgeItem& item : nc->items)
		{
			const size_t si = item.si;
			const kworld::WorldShape& s = (*shapes)[si];
			if (x < s.min[0] - radius || x > s.max[0] + radius || z < s.min[2] - radius || z > s.max[2] + radius || s.max[1] < lo || s.min[1] > hi) { continue; }
			// Thin shapes too (kenshi-k16.2.345 rework): a barrel, a stool or a table is a small box the camera's thin() calls a bar. Floors and ramps
			// (|normal y| >= 0.7) are the ground ray's; every other face stands in the body's way.
			double inDepth = -1e30, inDir[2] = { 0, 0 };	// a closed (convex) shape: the largest signed distance over its wall faces; <= 0 means the body's centre is inside
			bool faces = false;
			for (const NudgeFace& f : item.faces)
			{
				if (f.ymax < lo || f.ymin > hi) { continue; }
				const double hx = f.hx, hz = f.hz;
				const double d = hx * (x - f.v0x) + hz * (z - f.v0z);
				faces = true;
				if (s.closed && d > inDepth) { inDepth = d; inDir[0] = hx; inDir[1] = hz; }
				// the face's horizontal footprint is a segment along the tangent; the nearest point of it to the body, and the push away from that point
				const double tp = std::clamp(f.tx * (x - f.v0x) + f.tz * (z - f.v0z), f.t0, f.t1);
				const double qx = f.v0x + f.tx * tp, qz = f.v0z + f.tz * tp;
				const double dist = std::hypot(x - qx, z - qz);
				if (dist >= radius) { continue; }
				const double push = radius - dist;
				if (push <= bestLen) { continue; }
				bestLen = push; bestShape = si; bestInside = false;
				if (dist > 1e-4) { best[0] = (x - qx) / dist; best[1] = (z - qz) / dist; }
				else if (s.closed) { best[0] = hx; best[1] = hz; }
				else { best[0] = d >= 0 ? hx : -hx; best[1] = d >= 0 ? hz : -hz; }
			}
			// the centre inside a closed solid: out through its nearest wall face (the segment test above only saw the surface)
			// Only a shallow overlap counts (the centre within 1.5 radii of a wall face): a room-sized closed solid (the building's volume hull, 183 u across at
			// the Bar) holds every body inside it, and its nearest face is 9-40 u away (the first rework's false pushes, all at the cap)
			if (faces && s.closed && inDepth <= 0 && inDepth >= -1.5 * radius && radius - inDepth > bestLen) { bestLen = radius - inDepth; best[0] = inDir[0]; best[1] = inDir[1]; bestShape = si; bestInside = true; }
		}
	}
	else { return false; }	// radius above the cache's: not asked for
	if (bestLen <= 0) { return false; }
	if (why)
	{
		const kworld::WorldShape& w = (*shapes)[bestShape];
		char b[256];
		std::shared_ptr<const std::vector<std::string>> names;
		{ std::lock_guard<std::mutex> lk(m_); names = names_; }
		snprintf(b, sizeof b, "%s %s box %.0f %.0f %.0f - %.0f %.0f %.0f push %.2f dir %.2f,%.2f", w.closed ? (bestInside ? "closed-inside" : "closed") : "surface", names && bestShape < names->size() ? (*names)[bestShape].c_str() : "?",
			w.min[0], w.min[1], w.min[2], w.max[0], w.max[1], w.max[2], bestLen, best[0], best[1]);
		*why = b;
	}
	const double m = std::min(bestLen, maxPush);
	out[0] = best[0] * m;
	out[1] = best[1] * m;
	return true;
}

bool CollisionPick::thin(const float b[6])
{
	float d[3] = { b[3] - b[0], b[4] - b[1], b[5] - b[2] };
	std::sort(d, d + 3);
	return d[1] < 10 || (d[0] < 3 && d[2] < 60);	// MINE: the split (docs/CLIENT.md "Follow camera")
}

namespace
{
	double dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

	// The first t >= 0 where the ray (origin 0, dir) meets the sphere at c, radius r; the ray starting inside is not a hit (a sweep leaving a touch).
	bool raySphere(const double c[3], double r, const double dir[3], double& t)
	{
		const double b = dot3(c, dir), cc = dot3(c, c) - r * r;
		if (cc < 0) { return false; }
		const double disc = b * b - cc;
		if (disc < 0 || b < 0) { return false; }
		t = b - std::sqrt(disc);
		return true;
	}

	// ... and the infinite cylinder round the segment a -> b (clamped to it by the caller's axis parameter).
	bool rayCylinder(const double a[3], const double e[3], double r, const double dir[3], double& t)
	{
		const double ee = dot3(e, e);
		if (ee < 1e-12) { return false; }
		const double ed = dot3(e, dir), ea = dot3(e, a);
		// distance from the line to the point on the ray, squared: |m - dir*s - e*u|, m = -a
		const double m[3] = { -a[0], -a[1], -a[2] };
		const double dd[3] = { dir[0] - e[0] * ed / ee, dir[1] - e[1] * ed / ee, dir[2] - e[2] * ed / ee };
		const double mm[3] = { m[0] + e[0] * ea / ee, m[1] + e[1] * ea / ee, m[2] + e[2] * ea / ee };	// -a with its axis part removed
		const double A = dot3(dd, dd), B = dot3(mm, dd), C = dot3(mm, mm) - r * r;
		if (A < 1e-12 || C < 0) { return false; }
		const double disc = B * B - A * C;
		if (disc < 0) { return false; }
		t = (-B - std::sqrt(disc)) / A;
		return t >= 0;
	}
}

bool CollisionPick::sweep(const double origin[3], const double dir[3], double maxT, double r, double& t, float bounds[6], std::string* hitName) const
{
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes;
	std::shared_ptr<const std::vector<std::string>> names;
	size_t buildings;
	{
		std::lock_guard<std::mutex> lk(m_);
		shapes = shapes_;
		names = names_;
		buildings = buildingShapes_;
	}
	if (!shapes) { return false; }
	double best = maxT;
	size_t hit = (size_t)-1;
	for (size_t si = 0; si < shapes->size(); si++)
	{
		const kworld::WorldShape& s = (*shapes)[si];
		if (s.use == kworld::NavUse::Door) { continue; }
		if (const float box[6] = { s.min[0], s.min[1], s.min[2], s.max[0], s.max[1], s.max[2] }; si < buildings && thin(box)) { continue; }	// min and max are separate arrays
		const double lo[3] = { s.min[0] - origin[0] - r, s.min[1] - origin[1] - r, s.min[2] - origin[2] - r };
		const double hi[3] = { s.max[0] - origin[0] + r, s.max[1] - origin[1] + r, s.max[2] - origin[2] + r };
		if (!hitsBox(lo, hi, dir, best)) { continue; }
		for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
		{
			double v[3][3];
			for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { v[j][k] = s.vertices[s.indices[i + j] * 3 + k] - origin[k]; } }
			const double e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
			const double e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
			double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
			const double nl = std::sqrt(dot3(n, n));
			if (nl < 1e-9) { continue; }
			for (double& c : n) { c /= nl; }
			// the face, offset r towards the origin's side, moving into it
			const double d0 = -dot3(n, v[0]), dn = dot3(n, dir);	// the origin's signed distance to the plane, and the approach rate
			if (std::abs(dn) > 1e-9 && d0 * dn < 0)
			{
				const double side = d0 > 0 ? r : -r, ft = (side - d0) / dn;
				if (ft >= 0 && ft < best)
				{
					// the contact point on the plane: the centre at ft, less the offset
					double q[3];
					for (int k = 0; k < 3; k++) { q[k] = dir[k] * ft - n[k] * side - v[0][k]; }
					const double d11 = dot3(e1, e1), d12 = dot3(e1, e2), d22 = dot3(e2, e2), q1 = dot3(q, e1), q2 = dot3(q, e2);
					const double den = d11 * d22 - d12 * d12;
					if (std::abs(den) > 1e-12)
					{
						const double u = (d22 * q1 - d12 * q2) / den, w = (d11 * q2 - d12 * q1) / den;
						if (u >= 0 && w >= 0 && u + w <= 1) { best = ft; hit = si; }
					}
				}
			}
			// the three edges (cylinders; the contact inside the segment) and the vertices (spheres)
			for (int j = 0; j < 3; j++)
			{
				const double* a = v[j];
				const double* b = v[(j + 1) % 3];
				const double e[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
				double ct;
				if (rayCylinder(a, e, r, dir, ct) && ct < best)
				{
					const double u = (dot3(e, dir) * ct - dot3(e, a)) / dot3(e, e);	// the axis parameter of the contact
					if (u >= 0 && u <= 1) { best = ct; hit = si; }
				}
				if (raySphere(a, r, dir, ct) && ct < best) { best = ct; hit = si; }
			}
		}
	}
	if (best >= maxT) { return false; }
	t = best;
	if (bounds && hit < shapes->size()) { for (int k = 0; k < 3; k++) { bounds[k] = (*shapes)[hit].min[k]; bounds[3 + k] = (*shapes)[hit].max[k]; } }
	if (hitName) { *hitName = names && hit < names->size() ? (*names)[hit] : "shape " + std::to_string(hit); }
	return true;
}

std::string CollisionPick::report(double x, double z, double radius, bool thinOnly) const
{
	std::shared_ptr<const std::vector<kworld::WorldShape>> shapes;
	std::shared_ptr<const std::vector<std::string>> names;
	size_t buildings;
	{
		std::lock_guard<std::mutex> lk(m_);
		shapes = shapes_;
		names = names_;
		buildings = buildingShapes_;
	}
	std::string out;
	if (!shapes) { return out; }
	for (size_t si = 0; si < shapes->size(); si++)
	{
		const kworld::WorldShape& s = (*shapes)[si];
		const float box[6] = { s.min[0], s.min[1], s.min[2], s.max[0], s.max[1], s.max[2] };
		if (s.use == kworld::NavUse::Door || box[0] > x + radius || box[3] < x - radius || box[2] > z + radius || box[5] < z - radius) { continue; }
		const bool building = si < buildings, isThin = building && thin(box);
		if (thinOnly && !isThin) { continue; }
		char line[256];
		snprintf(line, sizeof line, "%s %s box %.0f %.0f %.0f - %.0f %.0f %.0f size %.0f %.0f %.0f (%s)\n", isThin ? "thin" : building ? "solid" : "foliage",
			si < names->size() ? (*names)[si].c_str() : "?", box[0], box[1], box[2], box[3], box[4], box[5], box[3] - box[0], box[4] - box[1], box[5] - box[2], s.use == kworld::NavUse::Walkable ? "walkable" : "other");
		out += line;
	}
	return out;
}

size_t CollisionPick::shapes() const
{
	std::lock_guard<std::mutex> lk(m_);
	return shapes_ ? shapes_->size() : 0;
}

std::shared_ptr<const std::vector<kworld::WorldShape>> CollisionPick::gathered() const
{
	std::lock_guard<std::mutex> lk(m_);
	return shapes_;
}

void CollisionPick::wait() const
{
	for (;;)
	{
		{
			std::lock_guard<std::mutex> lk(m_);
			if (failed_ || !thread_.joinable() || (done_[0] == want_[0] && done_[1] == want_[1])) { return; }
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

bool CollisionPick::failed() const
{
	std::lock_guard<std::mutex> lk(m_);
	return failed_;
}
