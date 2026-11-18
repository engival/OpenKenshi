// Retired 2026-11-16 (kenshi-k16.2.236.14, the floor view migration step 3 of the fable review kenshi-k16.2.236.9): the five edge-triggered rules
// that wrote the floor view (WorldBuildings::visible_) from WorldBuildings::updateFocus: W1 the snap on engage, W2 the auto threshold (with its
// stair up-hold / down-hold), W3 the stair hotspot (the top-kZoneSteps-tread zone, armed / held, the height band), and the follow safety net's
// remnants (stairWatch_, the "crossed but kept" log). Each fired once on an event and a missed or misread event was never corrected (the review's
// section 2). They are replaced by the one per-frame model ff::FloorFollow (client/src/floorfollow.hpp), which updateFocus feeds a sample each frame.
// This is the code as it stood at client 8e987bca (the last hash with it live). The fields it used, all removed from worldbuildings.hpp:
//   WorldBuildings: startSubject, leadKnown, snapId_, autoIn_, autoFloor_, autoTracked_, leadIn_, leadAt_, stairWatch_, kZoneSteps
//   StairHot: mn / mx (the hotspot zone rect), yZone, armed, held
//   helpers: stairPair, stairDistance, stairInside, stairOn, stairMid
// Kept for reference only; not built. No option and no fallback path brings it back (user rule: refactors move old code to retired/).

// ---- worldbuildings.hpp, the fields ----
	uint64_t startSubject = 0;	// a save load's restored subject (restoreView): adopted as the subject without a snap, its saved level kept
	bool leadKnown = false;	// the lead's character record is decoded (a snap waits for it: before that its floor is not known)
	// An explicit engage (double click, portrait double click, F, V, Z: main.cpp set_rts / set_follow_id): the next updateFocus snaps the level to the lead's
	// floor even when the subject is the one already centred (kenshi-k16.2.236.5). The one place the routes call.
	void engage() { snapId_ = 0; startSubject = 0; }
	// A save load's own view (camera sidecar): the saved level, adopted without a snap for the subject the load centres on (0: none, no snap to skip).
	void restoreView(int level, uint64_t subject) { visible_ = std::clamp(level, 0, 3); startSubject = subject; snapId_ = 0; }
	void resetFloors() { visible_ = 3; autoTracked_ = false; snapId_ = 0; }
	};
	// A staircase's top-step hotspot (kenshi-k16.2.236.3), built from the staircase's own mesh (`is stairs`, keepHull) in the building's local mesh units
	// (before the instance scale, like BInfo). Measured on the Storm House's InteriorStairs mesh: tread 4.1 u (the rise 3.9 u), the top tread one stair
	// deep across the full width. The hotspot is a zone of the top kZoneSteps treads (kenshi-k16.2.236.8.1): the feet entering it flip the view, the
	// down flip only once they are a rise below the top tread (kenshi-k16.2.236.8.4.1: a stop on the top tread, or a click there from the deck, is no descent).
	static constexpr size_t kZoneSteps = 3;	// the hotspot zone: the staircase's top 3 treads; the user's up spot (203819) is 2.1 treads below the top (kenshi-k16.2.236.8.1)
	struct StairHot
	{
		int lower = 0, upper = 1;		// the floors the staircase joins: the view flips between them when the feet enter the zone
		float mn[2] = {}, mx[2] = {};	// the hotspot, building-local x/z (mesh units)
		float fmn[2] = {}, fmx[2] = {};	// the whole flight's footprint: the subject on it stands on this staircase (the wire's floor turns over at its middle)
		float y = 0;					// the top tread's height, building-local
		float rise = 0;					// one tread's rise (mesh units): the DOWN flip needs the feet a rise below the top tread (kenshi-k16.2.236.8.4.1); the floor model's stair latch switches at top - 1 and top - 2 rises (kenshi-k16.2.236.11)
		float yZone = 0;				// the zone's lowest tread height: the zone is the band yZone..y (a subject on the floor under the stairwell is below it)
		float yLow = 0;					// the foot tread's height: below it the subject is on the floor under the stairwell, not on the flight
		float tread = 0;				// one tread's run (mesh units): the hotspot arms 2 of these away from it
		float footC[2] = {}, topC[2] = {};	// the foot and top treads' centres, local x/z: the flight's slope for the floor model's "on the flight" test
		bool armed = false;				// the subject was 2 treads clear of the zone: its feet entering it fire once (a subject that starts inside must leave first)
		bool held = false;				// the down flip was withheld (the feet within a rise of the top tread) and logged once: a stop at the stair top says why it did not fire
	};
	struct Job
	const BInfo* infoFor(uint32_t building);
	int floorableOf(uint64_t key) const;
	// The building's staircases' top-step hotspots (StairHot), built once from their meshes; null until every staircase mesh is loaded (retry next frame)
	// or when the building has no `is stairs` part. `stairPair`: the hotspot joining those two floors (the threshold rule leaves such a climb to it), and
	// the building it was found on.
	std::vector<StairHot>* stairHots(uint32_t building);
	StairHot* stairPair(uint32_t building, int lower, int upper, uint32_t& found);
	static bool stairTopStep(const MeshEntry& m, const float offset[3], StairHot& hot);	// measure one staircase's top step from its mesh
	// A click or a subject position against one hotspot: local (building mesh units) xz distance to the hotspot rect (0 inside) and whether it is inside.
	static float stairDistance(const StairHot& h, const double local[3]);
	static bool stairInside(const StairHot& h, const double local[3]);
	static bool stairOn(const StairHot& h, const double local[3]);	// the subject is on the flight: its footprint, at or above the foot tread
	static float stairMid(const StairHot& h) { return 0.5f * (h.yLow + h.y); }	// the flight's height middle: the wire's floor turns over there
	std::unordered_map<uint32_t, std::vector<StairHot>> stairs_;	// by building instance, once its staircases' meshes are loaded (the arm state lives here)
	int stairWatch_ = -1;	// the stair building the subject was last seen standing on a flight of: for the top step's frames the wire drops its building key (or names the deck's own record)
	std::unordered_map<uint32_t, float> construction_;	// by building instance: progress < 1
	int viewLogged_ = 3;	// the level the last "floor: view A -> B" line ended on (the session starts on 3, like visible_)
	const char* viewWhy_ = "player";	// which writer changed the level since (R / F, restore and reset leave it 'player')
	int visible_ = 3, autoFloor_ = 0;	// the visible floor (the player's level; written by their input and the lead's auto floor while following); the lead's floor at the last threshold
	int autoIn_ = -1;			// the floorable building the lead stood in at that threshold (-1: outdoors)
	uint64_t snapId_ = 0;			// the subject the level was last snapped to
	int leadIn_ = -2, leadAt_ = 0;		// the lead's (building, floor) at the last frame, for the "no floor change" log
	bool autoTracked_ = false;		// a following frame set the pair: the next change of (autoIn_, autoFloor_) is a threshold that writes visible_ (updateFocus)

// ---- worldbuildings.cpp: stairTopStep's hotspot zone ----
	if (hot.tread < 0.5f) { return false; }
	// the hotspot zone: the top kZoneSteps treads' footprint (the flight is monotone along its axis, so their union is one rect), from the mesh
	const size_t k = std::min<size_t>(kZoneSteps, steps.size());
	hot.mn[0] = hot.mn[1] = 1e30f; hot.mx[0] = hot.mx[1] = -1e30f;
	for (size_t i = steps.size() - k; i < steps.size(); i++)
	{
		hot.mn[0] = std::min(hot.mn[0], steps[i]->mnx); hot.mx[0] = std::max(hot.mx[0], steps[i]->mxx);
		hot.mn[1] = std::min(hot.mn[1], steps[i]->mnz); hot.mx[1] = std::max(hot.mx[1], steps[i]->mxz);
	}
	hot.mn[0] += off[0]; hot.mx[0] += off[0]; hot.mn[1] += off[2]; hot.mx[1] += off[2];
	hot.yZone = steps[steps.size() - k]->y + off[1];

// ---- worldbuildings.cpp: the hotspot helpers ----
}

WorldBuildings::StairHot* WorldBuildings::stairPair(uint32_t bi, int lower, int upper, uint32_t& found)
{
	std::vector<StairHot>* hots = stairHots(bi);
	if (!hots) { return nullptr; }
	for (StairHot& h : *hots)
	{
		if (h.lower == lower && h.upper == upper) { found = bi; return &h; }
	}
	return nullptr;
}

float WorldBuildings::stairDistance(const StairHot& h, const double local[3])
{
	const double dx = std::max({ h.mn[0] - local[0], 0.0, local[0] - h.mx[0] });
	const double dz = std::max({ h.mn[1] - local[2], 0.0, local[2] - h.mx[1] });
	return (float)std::sqrt(dx * dx + dz * dz);
}

bool WorldBuildings::stairInside(const StairHot& h, const double local[3])
{
	return local[0] >= h.mn[0] && local[0] <= h.mx[0] && local[2] >= h.mn[1] && local[2] <= h.mx[1];
}

bool WorldBuildings::stairOn(const StairHot& h, const double local[3])
{
	// a tolerance under the foot tread: the subject's sim position sits a tread or so above the riser it stands on
	return local[0] >= h.fmn[0] && local[0] <= h.fmx[0] && local[2] >= h.fmn[1] && local[2] <= h.fmx[1] && local[1] >= h.yLow - 2.0;

// ---- worldbuildings.cpp: WorldBuildings::updateFocus, the rules (W1 snap on engage, W2 auto threshold, W3 stair hotspot) ----
	}
	int in = -1, autoFloor = -1;
	if (hasLead)
	{
		in = lead.instance >= 0 ? lead.instance : floorableOf(lead.building);
		if (following) { autoFloor = lead.instance >= 0 ? data_.instances[lead.instance].maxFloor : in >= 0 ? lead.floor : 0; }	// the roof deck is the top floor
	}
	const int leadFloor = leadId == 0 ? -1 : lead.instance >= 0 ? data_.instances[lead.instance].maxFloor : in >= 0 ? lead.floor : 0;	// the lead's own floor; 0 outdoors
	// The auto floor (camera_rts.md 6: 140801630, the original writes the visible floor only while following a character). Ours keys the write on the
	// (building, floor) pair the lead stands on, so a threshold writes once even where the floor number does not change: in through a front door from
	// outdoors (-1, 0) -> (building, 0) is the threshold the user asked for (2026-11-14: entering "sets and keeps" the ground floor). The old write
	// compared the floor number alone, against a tracker that only reset while nobody was inside anything: with another player character inside a
	// building the tracker could already hold 0, so the followed lead's own door wrote nothing and the level stayed at the player's (3: the roof over
	// the interior, the blind view). Leaving is a threshold too and sets 0: the lead's computed floor outdoors (outside, char.vf60() in camera_rts.md 6
	// is no building's floor, the ground floor 0 our outside value already is; the finding has no separate outdoor number). A steady pair writes nothing,
	// so a level the player steps to with R / F while following holds until the lead crosses the next threshold (the original's r3), and a lead outdoors
	// (the pair (-1, 0)) writes nothing at all, as the old code's write did not reach there either (it compared only when the lead was inside; the blind
	// doorway came from the stale tracker, not from an outdoor write). Not following, the level is the player's alone (kenshi-h2v.24.6.2) and the pair is
	// forgotten: starting to follow adopts the lead's floor without writing, since a follow start is not a threshold the character crossed.
	// The log's other half (kenshi-k16.2.236.2): a lead threshold crossing that did not change the level says why (debug builds, once per crossing).
	const int leadAt = leadId == 0 ? 0 : leadFloor < 0 ? 0 : leadFloor;
	const bool crossed = hasLead || leadId ? (leadIn_ != -2 && (in != leadIn_ || leadAt != leadAt_)) : false;
	if (leadId && leadKnown) { leadIn_ = in; leadAt_ = leadAt; } else { leadIn_ = -2; }
	if (following && subjectId && subjectId != snapId_ && leadKnown)
	{
		// Snap on engage: the camera just went onto this character (from none, or from another): the level is its floor this frame, by whichever route
		// (V, F, a double click, a portrait double click). Leaving a subject changes nothing. Then the threshold rule below runs as before.
		const int at = autoFloor >= 0 ? autoFloor : 0;
		if (subjectId != startSubject)
		{
#ifdef KCLIENT_DEV_TOOLS
			if (logFloor) { printf("floor: snap on engage lead %llu floor %d level %d -> %d\n", (unsigned long long)leadId, at, visible_, at); }
#endif
			visible_ = at;
			viewWhy_ = "snap on engage";
			stairWatch_ = -1;	// another subject's stair building says nothing about this one
		}
		startSubject = 0;
		snapId_ = subjectId;
		autoIn_ = in;
		autoFloor_ = at;
		autoTracked_ = true;
	}
	else if (!following || !subjectId) { snapId_ = 0; stairWatch_ = -1; }
#ifdef KCLIENT_DEV_TOOLS
	if (logFloor && crossed && !(following && autoTracked_ && (in != autoIn_ || (autoFloor >= 0 ? autoFloor : 0) != autoFloor_)))
	{
		printf("floor: lead %llu crossed a threshold (building %llu floor %d), level stays %d: %s\n", (unsigned long long)leadId, (unsigned long long)lead.building,
			leadAt, visible_, !following ? "no camera subject" : "level already there (clamped to the building)");
	}
#endif
	if (following)
	{
		const int at = autoFloor >= 0 ? autoFloor : 0;
		const bool moved = autoTracked_ && (in != autoIn_ || at != autoFloor_);
		bool stairHold = false;
		if (moved)
		{
			// kenshi-k16.2.236.3: a climb up a staircase is decided at its top step, not half-way. The wire's char floor turns over in the middle of the
			// flight (ksim BuildingSite::floorAt "halfway up the stairs"), so this crossing names the staircase and leaves the write to the hotspot below
			// (a jittering foot at that point never flips the view); the way down still writes here, unless the subject is still above the flight's middle
			// (the wire's own floor dithered back down at the top step's edge, or a sample was dropped: run 3 of the 2026-11-15 diag lost its fire to
			// exactly that). Not a stair pair (or its mesh is not loaded yet, no hotspot): the crossing writes as before. The staircase is sought on
			// the lead's building this frame and then on the one whose flight the subject last stood on (stairWatch_): the top step's frames are the ones
			// the wire drops its building key for (the deck's roof path), or names the deck's own record for.
			const int lower = std::min(autoFloor_, at), upper = std::max(autoFloor_, at);
			uint32_t sbi = 0;
			StairHot* stair = in >= 0 ? stairPair((uint32_t)in, lower, upper, sbi) : nullptr;
			if (!stair && stairWatch_ >= 0) { stair = stairPair((uint32_t)stairWatch_, lower, upper, sbi); }
			if (stair)
			{
				double sl[3] = { 0, 0, 0 };
				const bool slOk = hasSubjectPos && sbi < data_.instances.size();
				if (slOk) { toLocal(data_.instances[sbi], subjectPos, sl); }
				// up: the top step decides. Down: the wire's floor turns over at the flight's middle; above it the subject is still coming up (or standing
				// at the top step's edge), so the view stays on the upper floor and the pair is retried next frame (autoFloor_ not advanced).
				stairHold = at == autoFloor_ + 1 ? (!slOk || (uint32_t)in == sbi || stairOn(*stair, sl)) : (!slOk || sl[1] > stairMid(*stair) + 1.0f);
			}
			if (stairHold)
			{
#ifdef KCLIENT_DEV_TOOLS
				if (logFloor)
				{
					const std::string name = sbi < data_.instances.size() ? data_.names[data_.instances[sbi].nameId] : recordOf(lead.building);
					if (at > autoFloor_)
					{
						printf("floor: lead %llu climbs the %s stairs floor %d -> %d: the top step sets the level (level %d stays)\n", (unsigned long long)leadId,
							name.empty() ? "-" : name.c_str(), autoFloor_, at, visible_);
					}
					else if (crossed)	// held until the subject is really below the flight's middle: say it once per crossing
					{
						printf("floor: lead %llu's wire floor dipped on the %s stairs floor %d -> %d: the subject is above the flight's middle (level %d stays)\n",
							(unsigned long long)leadId, name.empty() ? "-" : name.c_str(), autoFloor_, at, visible_);
					}
				}
#endif
				if (at > autoFloor_) { autoIn_ = in; autoFloor_ = at; }	// an up-hold is the climb's floor; a down-hold leaves the pair for a later write
			}
			else
			{
#ifdef KCLIENT_DEV_TOOLS
				if (logFloor)
				{
					const std::string name = in >= 0 ? data_.names[data_.instances[in].nameId] : recordOf(lead.building);
					printf("floor: auto threshold lead %llu building %llu %s floor %d level %d -> %d\n", (unsigned long long)leadId,
						(unsigned long long)lead.building, name.empty() ? "-" : name.c_str(), at, visible_, at);
				}
#endif
				visible_ = at;
				viewWhy_ = "auto threshold";
			}
		}
		// a down-hold leaves the pair where it was: the subject may still be inside the flight, and the retried crossing writes when it is really down
		if (!(stairHold && at < autoFloor_)) { autoIn_ = in; autoFloor_ = at; }
		autoTracked_ = true;
	}
	else { autoTracked_ = false; }
	// The stair hotspot (kenshi-k16.2.236.3, redone k16.2.236.8.1, user 2026-11-15): a zone of a staircase's top kZoneSteps treads, measured from its
	// mesh. With the camera following a character, the subject's feet (sim position) ENTERING the zone fire it: the view on the floor above flips
	// down to the floor below, the view on the floor below flips up. Where a click lands never matters. After firing, the zone is dead until the
	// subject is 2 treads clear of it, so neither a stand on its border nor the walk on across it flips the view back.
	// The down flip needs a real descent (kenshi-k16.2.236.8.4.1, user 2026-11-15 22:23): the feet at least one rise below the top tread. A stop on
	// the top tread, or a click on the deck at the stair top whose walk ends in the zone, is not a way down and never fires it; walking down fires it a
	// tread later (the second tread), which the threshold rule agrees with.
	if (following && subjectId && hasSubjectPos)
	{
		// The lead's building this frame and the one its flight was last seen on (stairWatch_): the wire names neither for some of the top step's frames.
		uint32_t cand[2] = { 0, 0 };
		int nc = 0;
		for (int bi : { in, stairWatch_ })
		{
			if (bi < 0 || nc >= 2) { continue; }
			const uint32_t b2 = (uint32_t)bi;
			if ((nc > 0 && cand[0] == b2) || (nc > 1 && cand[1] == b2)) { continue; }
			cand[nc++] = b2;
		}
		for (int i = 0; i < nc; i++)
		{
			std::vector<StairHot>* hots = stairHots(cand[i]);
			if (!hots || hots->empty()) { continue; }
			double local[3];
			toLocal(data_.instances[cand[i]], subjectPos, local);
			for (StairHot& h : *hots)
			{
				if (stairOn(h, local)) { stairWatch_ = (int)cand[i]; }
				const bool inBand = local[1] >= h.yZone - 2.0 && local[1] <= h.y + 6.0;	// not the floor under the stairwell, nor a floor above the deck
				const float away = inBand ? stairDistance(h, local) : 1e30f;
				if (!h.armed && away >= 2.0f * h.tread)
				{
					h.armed = true;
					h.held = false;
#ifdef KCLIENT_DEV_TOOLS
					if (logFloor)
					{
						const std::string name = data_.names[data_.instances[cand[i]].nameId];
						printf("floor: stair hotspot arm building %u %s floors %d -> %d: subject %.1f u away (2 treads %.1f u)\n", cand[i],
							name.empty() ? "-" : name.c_str(), h.lower, h.upper, away, 2.0f * h.tread);
					}
#endif
				}
				const int shown = std::min(visible_, (int)data_.instances[cand[i]].maxFloor);
				const bool inZone = inBand && stairInside(h, local);
				const bool down = shown == h.upper, up = shown == h.lower;
				const bool descended = down && local[1] <= (double)h.y - (double)h.rise;	// k16.2.236.8.4.1: a rise below the top tread
				if (h.armed && inZone && (up || descended))
				{
					const int to = down ? h.lower : h.upper;
					h.armed = false;
					h.held = false;
#ifdef KCLIENT_DEV_TOOLS
					if (logFloor)
					{
						const std::string name = data_.names[data_.instances[cand[i]].nameId];
						printf("floor: stair hotspot fire lead %llu building %u %s floor %d level %d -> %d at local %.1f %.1f %.1f (%.1f u below the top tread)\n",
							(unsigned long long)leadId, cand[i], name.empty() ? "-" : name.c_str(), h.upper, visible_, to, local[0], local[1], local[2], h.y - (float)local[1]);
					}
#endif
					visible_ = to;
					viewWhy_ = "stair hotspot";
				}
				else if (h.armed && inZone && down && !h.held)
				{
					// the feet in the zone but not a rise below the top tread: the deck click (user 2026-11-15), said once per arming
					h.held = true;
#ifdef KCLIENT_DEV_TOOLS
					if (logFloor)
					{
						const std::string name = data_.names[data_.instances[cand[i]].nameId];
						printf("floor: stair hotspot hold lead %llu building %u %s floor %d at local %.1f %.1f %.1f: top tread y %.1f, feet %+.1f u below it (a rise is %.1f u); level %d stays\n",
							(unsigned long long)leadId, cand[i], name.empty() ? "-" : name.c_str(), h.upper, local[0], local[1], local[2], h.y, h.y - (float)local[1], h.rise, visible_);
					}
#endif
				}
				if (!inZone) { h.held = false; }
			}

// ---- worldbuildings.cpp: stepFloor (W4) as it was ----
bool WorldBuildings::stepFloor(int dir)
{
	if (!floor_.valid)	// nothing sliced or peeked: the global level alone (always live, the original's selector)
	{
		const int to = std::clamp(visible_ + dir, 0, 3);
		const bool moved = to != visible_;
		visible_ = to;
		return moved;
	}
	// From the floor the player sees: the level clamped to the building's (or the sliced ones') top, e.g. 3 on a one-floor house steps down to 0.
	// The level follows (the original's pi+0x1e4 is written by the player's input only; a hover never writes it).
	const int base = std::min(visible_, floor_.top);
	const int to = std::clamp(base + dir, 0, floor_.top);
	visible_ = to;
	if (floor_.peek) { peeks[(uint32_t)focus_].floor = to; }	// at the top the roof deck shows uncut (as the inside view)
	floor_.shown = to;
	return to != base;
}
