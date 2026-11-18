// Retired 2026-11-05: the client's own "which building and floor is this character in" guess (box or roof test, floor from the parts' heights).
// Replaced by the wire's CHAR_DYNAMIC `building` + `floor` (sim 4e3af4e7; WorldBuildings::updateFocus resolves the key via BuildingSet::byKey).
// Kept for reference only; not built.

// The floorable building whose bounds hold p, or whose roof p stands on, the nearest of them, and p's floor in it; -1 = none
constexpr double kRoofReach = 10;	// MINE: local units over the parts' box top still on the roof
int WorldBuildings::containing(const double p[3], int& floor)
{
	int in = -1;
	double inD = kFocusRange * kFocusRange;
	for (const uint32_t bi : data_.floorable)
	{
		const BuildingInstance& b = data_.instances[bi];
		const double d = (b.x - p[0]) * (b.x - p[0]) + (b.z - p[2]) * (b.z - p[2]);
		if (d >= inD) { continue; }
		const BInfo* info = infoFor(bi);
		if (!info || !info->hasBounds) { continue; }
		double l[3];
		toLocal(b, p, l);
		bool inside = true;
		for (int a = 0; a < 3; a++) { inside = inside && l[a] >= info->mn[a] && l[a] <= info->mx[a]; }
		// on its roof (a deck over the interior bounds: the Storm House's upstairs) counts as in it, on the top floor (user 2026-11-04)
		const bool roof = l[1] > info->mx[1] && l[1] <= info->outerMx[1] + kRoofReach && l[0] >= info->outerMn[0] && l[0] <= info->outerMx[0] && l[2] >= info->outerMn[2] && l[2] <= info->outerMx[2];
		if (!inside && !roof) { continue; }
		inD = d;
		in = (int)bi;
		floor = 0;
		for (int f = 1; f < 4; f++) { if (info->floorY[f] <= l[1]) { floor = f; } }
	}
	return in;
}

