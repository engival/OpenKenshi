// Retired by kenshi-k16.2.339 (standing height stage 1, report kenshi-k16.2.338): the client's own choice of the drawn y. The client now draws the
// sim's wire y; these three rules each swapped the standing surface (terrain within 2..4 u, interior mask hull, wire indoors flag).
// Was in SimView::sync (simview.cpp):
//
//	const double terrain = ground(p[0], p[2]);
//	const double simY = p[1];
//	bool inside = false;
//	if (ib->flags & kYValid)
//	{
//		inside = (ib->flags & kIndoors) || (insideInterior && insideInterior(p));
//		p[1] = inside ? simY : terrain + (simY - terrain) * std::clamp((simY - terrain - 2.0) / 2.0, 0.0, 1.0);
//	}
//	else { p[1] = terrain; }
//
// and SimView::insideInterior (std::function, set at main.cpp: app.sim.insideInterior = [](const double p[3]) { return app.buildings.insideInterior(p); };).
// WorldBuildings::insideInterior stays for the terrain clip and the ground pick.
