// Retired by kenshi-1rnr: pick.cpp's per-case character volumes (the lying body's head-pelvis-feet segments, k16.2.223, and the bed occupant rule, k16.2.224.3),
// replaced by Characters::pickCapsules / SimView::pickBody (a capsule per bone of the drawn skeleton). Kept for reference; not built.
	const auto drawnBody = [](uint64_t id, double pts[][3])
	{
		const int index = app.sim.charIndex(id);
		double head[3], pelvis[3], lf[3], rf[3];
		if (index < 0 || !app.chars.boneWorld((size_t)index, "Bip01 Head", head) || !app.chars.boneWorld((size_t)index, "Bip01 Pelvis", pelvis) ||
			!app.chars.boneWorld((size_t)index, "Bip01 L Foot", lf) || !app.chars.boneWorld((size_t)index, "Bip01 R Foot", rf)) { return 0; }
		const double* seg[3][2] = { { head, pelvis }, { pelvis, lf }, { pelvis, rf } };
		for (int j = 0; j < 3; j++) { for (int k = 0; k < 3; k++) { pts[2 * j][k] = seg[j][0][k]; pts[2 * j + 1][k] = seg[j][1][k]; } }
		return 3;
	};
	double t;
	// kenshi-k16.2.224.3, kept until the character capsules (kenshi-1rnr) replace it: a character whose sim position is on the picked bed's sleeping spot (the bed's
	// occupant) wins over the bed, though the bed's drawn mesh is nearer; every other character competes by distance.
	uint64_t sleeper = 0;
	if (p.kind == Pick::Bed) { if (const simlink::Interactable* bed = app.sim.thing(p.id)) { sleeper = bed->occupant; } }
	if (sleeper)
	{
		double ts;
		const auto onlySleeper = [&](uint64_t id) { return charHidden(id) || id != sleeper; };
		if (const uint64_t id = app.sim.pick(from, dir, &ts, onlySleeper, drawnBody))
		{
			p.kind = Pick::Character;
			p.id = id;
			p.building = false;
			for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * ts; }
			app.buildings.levelCuts = prevCuts;
			return p;
		}
	}
	if (const uint64_t id = app.sim.pick(from, dir, &t, charHidden, drawnBody); id && t <= best)
	{
		p.kind = Pick::Character;
		p.id = id;
		p.building = false;
		for (int k = 0; k < 3; k++) { p.hit[k] = from[k] + dir[k] * t; }
	}
	app.buildings.levelCuts = prevCuts;
