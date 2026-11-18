// Retired 2026-11-12 (kenshi-k16.2.139): the client's own copy of the RESEARCH listing rules (listable records, tech level), replaced by
// RESEARCH_STATE 63 (the sim's state 1 = listable, techLevel). Not compiled. It lived in main.cpp research_state(); `done` / `unlocked` came from
// UNLOCKS 36, `app.researchRecs` from the data.

// The RESEARCH page after kinspect findings/research25_static.md. Listed (else hidden; there are no locked rows): not blueprint only, not done, level
// <= the tech level (1, or 1 + the level of the highest done `is level upgrade`), each requirement done or itself startable, no requirement
// blueprint only, and something it enables not yet unlocked (only buildings are on the wire, so only an all-buildings record can fail that).
// Startable (Main) needs the cost in a bench and a bench of its level: the sim has no benches, so every row is red (Bad), as the original with none.
// Not done: repeats clones (`<id>.<k>`), the queue and progress (the sim).
static ui::ResearchState research_state()
{
	const simlink::SimLinkDecoder& d = app.sim.decoder();
	std::set<std::string> done, unlocked;
	for (uint32_t id : d.research) { done.insert(d.str(id)); }
	for (uint32_t id : d.unlocked) { unlocked.insert(d.str(id)); }
	int techLevel = 1;
	for (const std::string& sid : done)
	{
		const auto it = app.researchRecs.find(sid);
		if (it != app.researchRecs.end() && it->second.levelUpgrade) { techLevel = std::max(techLevel, it->second.level + 1); }
	}
	constexpr int benchLevel = 0;	// the best built research bench's (none: the sim has no benches)
	constexpr double researchSpeed = 1;	// the advanced option rs, once NEW_GAME carries it
	const auto recOf = [](const std::string& sid) { const auto it = app.researchRecs.find(sid); return it != app.researchRecs.end() ? &it->second : nullptr; };
	const auto startable = [&](const App::ResearchRec& r) { return std::all_of(r.needs.begin(), r.needs.end(), [&](const std::string& q) { return done.count(q) > 0; }); };
	ui::ResearchState s;
	std::set<std::string> cats;
	for (const auto& [sid, r] : app.researchRecs) { cats.insert(r.category); }
	for (const std::string& c : cats) { s.categories.push_back({ c, c }); }
	s.category = cats.count(app.researchCategory) ? app.researchCategory : cats.empty() ? "" : *cats.begin();
	for (const auto& [sid, r] : app.researchRecs)
	{
		if (r.blueprintOnly || done.count(sid) || r.level > techLevel) { continue; }
		bool listable = true;
		for (const std::string& q : r.needs)
		{
			const App::ResearchRec* req = recOf(q);
			if (!done.count(q) && (!req || req->blueprintOnly || !startable(*req))) { listable = false; }
		}
		const bool onlyBuildings = !r.buildings.empty() && r.unlocks.size() == r.buildings.size();
		if (onlyBuildings && std::all_of(r.buildings.begin(), r.buildings.end(), [&](const std::string& b) { return unlocked.count(b) > 0; })) { listable = false; }
		if (!listable) { continue; }
