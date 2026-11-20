// Retired by kenshi-k16.2.382.2 (user 2026-11-18: collision meshes are "y axis barricades", the ground is the drawn art): the standing ray's building
// source. The stand search now reads the drawn floors / stairs / ramps (WorldBuildings::groundDrawn); this was CollisionPick::groundY (collisionpick.cpp),
// 1 hit / 0 none in the window / -1 not gathered / 3 no shape over x,z (kenshi-k16.2.344, .382.1).

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
	if (found) { y = best; return 1; }
	// kenshi-k16.2.382.1: 0 = a shape covers x,z but none in the window (a floor above or below), 3 = no shape covers x,z at all (open ground: the
	// caller's heightmap is the surface whatever the window)
	for (size_t si = 0; si < n; si++)
	{
		const kworld::WorldShape& s = (*shapes)[si];
		if (s.use == kworld::NavUse::Door) { continue; }
		if (x < s.min[0] || x > s.max[0] || z < s.min[2] || z > s.max[2]) { continue; }
		const float box[6] = { (float)s.min[0], (float)s.min[1], (float)s.min[2], (float)s.max[0], (float)s.max[1], (float)s.max[2] };
		if (thin(box)) { continue; }
		for (size_t i = 0; i + 2 < s.indices.size(); i += 3)
		{
			double v[3][3];
			for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { v[j][k] = s.vertices[s.indices[i + j] * 3 + k]; } }
			const double d = (v[1][2] - v[2][2]) * (v[0][0] - v[2][0]) + (v[2][0] - v[1][0]) * (v[0][2] - v[2][2]);
			if (std::abs(d) < 1e-9) { continue; }
			const double a = ((v[1][2] - v[2][2]) * (x - v[2][0]) + (v[2][0] - v[1][0]) * (z - v[2][2])) / d;
			const double b = ((v[2][2] - v[0][2]) * (x - v[2][0]) + (v[0][0] - v[2][0]) * (z - v[2][2])) / d;
			if (a >= 0 && b >= 0 && 1 - a - b >= 0) { return 0; }
		}
	}
	return 3;
}
