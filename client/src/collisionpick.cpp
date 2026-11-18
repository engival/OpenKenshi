#include "collisionpick.hpp"

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
