// Retired by kenshi-k16.2.404.1: the client's own part pick for a building, seeded by hash(instance id) with roll(seed, owner/index)
// and a weighted pick per group. The original seeds MSVCR100 rand from the placement's x/z (kworld::selectBuildingPartTree), so
// the drawn mesh and the collision are one pick. These were members of the Resolver in client/src/world/buildings.cpp.

	// `parts` of a BUILDING / BUILDING_PART: group 0 rolls each part by its chance, other groups pick one by chance weight.
	void collect(const kdata::Record& owner, const kdata::Record& building, uint64_t seed, std::vector<uint32_t>& into, int depth)
	{
		const kdata::RefCategory* c = owner.findRefs("parts");
		if (!c || depth > 3) { return; }
		std::map<int, kdata::RefCategory> groups;
		for (const kdata::RefEntry& e : c->entries)
		{
			if (e.v0 == kdata::kReferenceRemoved) { continue; }
			if (e.v0 == 0)	// group 0: each part rolled independently, v1 = absolute chance in percent
			{
				if (roll(seed, owner.stringID + "/" + std::to_string(&e - c->entries.data())) < e.v1) { include(e.targetID, building, seed, into, depth); }
			} else { groups[e.v0].entries.push_back(e); }
		}
		for (auto& [g, cat] : groups)
		{
			const kdata::RefEntry* e = pick(cat, seed, owner.stringID + "/g" + std::to_string(g), [](const kdata::RefEntry& x) { return x.v1; });
			if (e) { include(e->targetID, building, seed, into, depth); }
		}
	}


	// A BUILDING's `interior` / `interior mask` list as parts of the given kind. Every live entry counts: the v0/v1 of these refs are 0/0 or 0/100
	// (INTERIORS.md), and a 0 chance on a mask cannot mean "never".
	void collectAll(const kdata::Record& building, const char* category, int partKind, uint64_t seed, std::vector<uint32_t>& into)
	{
		const kdata::RefCategory* c = building.findRefs(category);
		if (!c) { return; }
		kind = partKind;
		for (const kdata::RefEntry& e : c->entries) { if (e.v0 != kdata::kReferenceRemoved) { include(e.targetID, building, seed, into, 0); } }
		kind = 0;
	}
