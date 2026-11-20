// Retired by kenshi-k16.2.412.3: host/pick.cpp ray_hit's interior skip (kenshi-k16.2.254.1 / kyat.2). The ground layer now hits the walkable drawn parts and the
// terrain under the carve rule (WorldBuildings::walkableRay, insideInterior carveOnly), so the ray no longer needs to run on past a masked terrain hit to the
// shapes inside the hull.
//
//	if (ground)
//	{
//		far = distance to the terrain hit;
//		double exitT = 0;
//		if (app.buildings.insideInterior(hit, app.pos, dir, &exitT) && exitT > far) { far = exitT + 1.0; }
//	}
