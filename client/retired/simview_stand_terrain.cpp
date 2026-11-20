// Retired by kenshi-k16.2.414.2 (one ground solver, both directions; CLIENT.md Decisions): SimView's standTerrain(), the stand ray's second half. Its rules
// moved into SimView::groundAt / SimView::solveGround (bridge/simview.cpp), which add the walkable-surface set (WorldBuildings::groundDrawn reads only parts
// the navmesh bake walks) and the interior-mask rule for the terrain. The window and the nearness rule (kenshi-k16.2.382 / .382.1 / .412.1) are unchanged.
// Was in bridge/simview.cpp (kenshi-k16.2.412.1's version; now SimView::solveGround, which keeps the rule and changes only its inputs: the ray sees walkable parts only, and `buried`
// is the interior-mask carve test, WorldBuildings::insideInterior(..., carveOnly) through SimView::setTerrainFn):
//
//	static int standTerrain(int res, double& ry, double th, double wy, double up, double down, const std::function<bool(double, double, double)>& buried, double x, double z)
//	{
//		if (res == 3) { ry = th; return 1; }
//		if (res == 0)
//		{
//			if (!(buried && buried(x, th, z)) && std::fabs(th - wy) < std::fabs(ry - wy)) { ry = th; }
//			return 1;
//		}
//		if (res != -1 && th <= wy + up && th >= wy - down && (res < 1 || std::fabs(th - wy) < std::fabs(ry - wy))) { ry = th; return 1; }
//		return res;
//	}
//
// And host/pick.cpp ray_hit's interior skip (the click's own sunk-dune rule, kenshi-k16.2.254.1 / kyat.2): retired by the click step (kenshi-k16.2.412.3).
