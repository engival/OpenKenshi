#include "net/simlink.hpp"
#include "net/simlink_io.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace simlink
{
namespace
{
	constexpr size_t kMaxFrame = 16u << 20;

	using simlink::io::Reader;
	using simlink::io::Writer;
}

// The names of CHAR_STATS row 34's tables (docs/formats/SIMLINK.md); nullptr for 0 / a value past the table.
const char* CharStats::engageBasisName(uint8_t basis)
{
	static const char* const names[] = { "none", "player attack order", "attacked by target", "ally attacked", "faction enemy", "personal hostile entry",
		"troublemaker", "package goal", "selector commit", "counter-flip" };
	return basis < sizeof names / sizeof *names ? names[basis] : nullptr;
}

const char* CharStats::engageSenseName(uint8_t sense)
{
	static const char* const names[] = { "n/a", "sight", "hearing", "remembered", "shot from unsensed" };
	return sense < sizeof names / sizeof *names ? names[sense] : nullptr;
}

bool SimLinkDecoder::feed(const uint8_t* data, size_t n)
{
	if (!error_.empty()) { return false; }
	buf_.insert(buf_.end(), data, data + n);
	size_t at = 0;
	while (buf_.size() - at >= 6)
	{
		uint32_t len;
		uint16_t type;
		memcpy(&len, &buf_[at], 4);
		if (len < 2 || len > kMaxFrame) { error_ = "bad frame length " + std::to_string(len); return false; }
		if (buf_.size() - at - 4 < len) { break; }
		memcpy(&type, &buf_[at + 4], 2);
		if (!frame(type, &buf_[at + 6], len - 2)) { return false; }
		frames++;
		at += 4 + len;
	}
	buf_.erase(buf_.begin(), buf_.begin() + (ptrdiff_t)at);
	return true;
}

uint32_t SimLinkDecoder::find(const std::string& s) const
{
	for (uint32_t i = 1; i < strings_.size(); i++) { if (strings_[i] == s) { return i; } }
	return 0;
}

const std::string& SimLinkDecoder::str(uint32_t id) const
{
	return id < strings_.size() ? strings_[id] : strings_[0];
}

void SimLinkDecoder::skipped(const char* what, uint16_t v)
{
	if (skipped_.insert(std::string(what) + " " + std::to_string(v)).second) { fprintf(stderr, "simlink: skipping unknown %s %u\n", what, v); }
}

bool SimLinkDecoder::frame(uint16_t type, const uint8_t* p, size_t n)
{
	Reader r{ p, n };
	if (!hello_ && type != kHello && type != kError) { error_ = "frame " + std::to_string(type) + " before HELLO"; return false; }
	switch (type)
	{
	case kHello:
	{
		const uint32_t magic = r.u32();
		const uint16_t proto = r.u16();
		const uint8_t role = r.u8();
		dataHash_ = r.u64();
		serverName_ = r.str();
		if (magic != kMagic) { error_ = "bad HELLO (not a Kenshi game server)"; return false; }
		if (proto != kProtocol) { error_ = "This game server speaks protocol " + std::to_string(proto) + "; this client speaks " + std::to_string(kProtocol) + ". Use matching builds."; return false; }
		if (role != 1) { error_ = "bad HELLO (the peer is not a server)"; return false; }
		hello_ = true;
		break;
	}
	case kError:
		error_ = "server: " + r.str();
		return false;
	case kStrtab:
	{
		const uint32_t first = r.u32(), count = r.u32();
		if (first != strings_.size()) { error_ = "string table gap at " + std::to_string(first); return false; }
		for (uint32_t i = 0; i < count && !r.bad; i++) { r.u8(); strings_.push_back(r.str()); }
		break;
	}
	case kSnapBegin:
		openTick_ = r.u64();
		r.u8();
		inSnap_ = true;
		break;
	case kSnapEnd:
		lastCompleteTick = r.u64();
		r.u8();
		inSnap_ = false;
		if (!r.bad) { snapSerial++; }
		break;
	case kAppearance:
	{
		r.u64();
		Character& c = chars[r.u64()];
		Appearance& a = c.look;
		a.squad = r.u64();
		a.faction = r.u32();
		a.race = r.u32();
		a.sex = r.u8();
		a.head = r.u16();
		a.hair = r.u32();
		a.beard = r.u32();
		a.hairColour = r.u32();
		a.skin = r.f32();
		a.body.resize(r.u8());
		for (auto& b : a.body) { b.first = r.u32(); b.second = r.f32(); }
		a.items.resize(r.u8());
		for (Item& it : a.items) { it.item = r.u32(); it.spec = r.u32(); it.colour = r.u32(); it.section = r.u8(); }
		a.crossbowAmmo = r.u16();
		a.hiddenLimbs = r.u8();
		a.version = r.u32();
		a.name = r.u32();
		if constexpr (kProtocol >= 5) { a.portraitSerial = r.u32(); }
		c.hasAppearance = true;
		break;
	}
	case kDynamic:
	{
		Dynamic d;
		d.tick = r.u64();
		Character& c = chars[r.u64()];
		for (float& v : d.pos) { v = r.f32(); }
		d.yaw = r.f32();
		d.fwd = r.f32();
		d.lat = r.f32();
		d.flags = r.u32();
		d.carriedBy = r.u64();
		d.legs[0] = r.f32();
		d.legs[1] = r.f32();
		d.action = r.u32();
		d.actionStart = r.u64();
		d.actionSpeed = r.f32();
		d.drive = r.u8();
		d.heading = r.f32();
		d.driveSeq = r.u32();
		d.building = r.u64();
		d.floor = r.i16();
		d.appearanceVersion = r.u32();
		if (r.at < r.n) { d.actionFlags = r.u8(); }	// an older server: no tail
		if ((d.flags & (kSeated | kAsleep)) && r.at < r.n) { for (float& v : d.pose) { v = r.f32(); } d.poseClip = r.u32(); }
		else if ((d.flags & kProne) && r.at < r.n) { d.restYaw = r.f32(); d.hasRest = true; }	// rest tail (SIMLINK CHAR_DYNAMIC, sim BG): x, y, z are the rest point
		if ((d.flags & kAim) && r.at + 4 <= r.n) { d.aimYaw = r.f32(); d.hasAim = true; }	// aim tail (SIMLINK CHAR_DYNAMIC aimYaw)
		if ((d.flags & kMounted) && r.at + 8 <= r.n) { d.turret = r.u64(); }	// mounted tail (SIMLINK CHAR_DYNAMIC turret): the record's last bytes
		if ((d.flags & kInMachine) && r.at + 12 <= r.n) { d.mSpace = r.u8(); d.mBuilding = r.u64(); d.mPiece = r.u16(); d.mSlot = r.u8(); d.hasMachine = true; }	// machine tail (SIMLINK CHAR_DYNAMIC space, building, piece, slot): the very end
		if (c.samples > 1) { c.past.push_back(c.prev); if (c.past.size() > Character::kPastSamples) { c.past.pop_front(); } }
		if (c.samples > 0) { c.prev = c.cur; }
		c.cur = d;
		c.samples++;
		break;
	}
	case kRoute:
	{
		const uint64_t tick = r.u64();
		Route& rt = chars[r.u64()].route;
		rt.arrivedTick = tick;	// kenshi-k16.2.231.1: the shot route sidecar's "arrived" (SIMLINK 6, the record's leading tick)
		rt.clientSeq = r.u32();
		rt.version = r.u32();
		rt.flags = r.u8();
		rt.points.resize(r.u16());
		for (auto& p : rt.points) { for (float& v : p) { v = r.f32(); } }
		break;
	}
	case kPing:
		break;	// client -> server keepalive; harmless if echoed
	case kMedical:
	{
		Medical m;
		m.tick = r.u64();
		const uint64_t id = r.u64();
		m.blood = r.f32();
		m.parts.resize(r.u8());
		for (Medical::Part& p : m.parts) { p.part = r.u32(); p.health = r.f32(); p.stun = r.f32(); }
		if (r.at < r.n) { m.severed = r.u16(); }	// an older server: no tail
		if (r.at < r.n)	// the panel tail; n repeats the part count (a self-check)
		{
			m.bloodMax = r.f32();
			m.bleed = r.f32();
			m.hunger = r.f32();
			m.flags = r.u8();
			if (r.u8() != m.parts.size()) { r.bad = true; fprintf(stderr, "simlink: CHAR_MEDICAL %llu: tail row count differs from the part count, record dropped\n", (unsigned long long)id); }
			for (Medical::Part& p : m.parts) { p.max = r.f32(); p.bandage = r.f32(); p.splint = r.f32(); p.limb = r.u8(); }
		}
		if (!r.bad) { medical[id] = std::move(m); }
		break;
	}
	case kCharStats:
	{
		CharStats c;
		c.tick = r.u64();
		const uint64_t id = r.u64();
		c.state = r.u8();
		c.lockLevel = r.u8();
		c.goal = r.u32();
		c.encumbrance = r.f32();
		c.extraKind = r.u8();
		c.extraSkill = r.u8();
		c.extra = r.f32();
		c.weaponClass = r.u8();
		c.skills.resize(r.u8());
		for (CharStats::SkillRow& s : c.skills) { s.skill = r.u8(); s.value = r.f32(); s.bonus = r.f32(); }
		c.athleticsXp = r.f32();
		c.strengthXp = r.f32();
		c.toughnessXp = r.f32();
		c.koPoint = r.f32();
		c.runSpeed = r.f32();
		c.runPenalty = r.f32();
		c.hungerRate = r.f32();
		c.light = r.f32();
		c.bounties.resize(r.u8());
		for (CharStats::Bounty& b : c.bounties) { b.faction = r.u32(); b.amount = (int32_t)r.u32(); }
		c.sentenceHours = r.f32();
		if (r.at < r.n)	// the AI tail (rows 16-20; an older server stops at row 15)
		{
			c.hasAi = true;
			c.goalEnum = r.u16();
			c.source = r.u8();
			c.stepEnum = r.u16();
			c.stepIndex = r.u8();
			c.stepCount = r.u8();
			c.targetId = r.u64();
			c.targetX = r.f32();
			c.targetZ = r.f32();
		}
		// the stats window tail (rows 21-23, k16.6.54; additive after the AI tail, length-guarded row by row: an older server stops before it)
		if (!r.bad && r.at < r.n)	// the Next Level bars: the fractional part of each row-8 value, in row 8's order
		{
			c.hasProgress = true;
			c.progress.resize(r.u8());
			for (CharStats::SkillProgress& p : c.progress) { p.skill = r.u8(); p.progress = r.f32(); }
		}
		if (!r.bad && r.at < r.n)	// the pack's kg (without the 30 kg carry-person weight) and the capacity
		{
			c.hasCarry = true;
			c.carryKg = r.f32();
			c.carryMaxKg = r.f32();
		}
		if (!r.bad && r.at < r.n)	// the damage resistance, a fraction
		{
			c.hasResist = true;
			c.damageResist = r.f32();
		}
		// the HUD tag row tail (rows 24-32, k16.6.215; additive after row 23: an older server stops before it)
		if (!r.bad && r.at < r.n)
		{
			c.hasTag = true;
			c.tagFlags = r.u16();
			c.slaveState = r.u8();
			c.captor = r.u32();
			c.slaveHoursLeft = r.f32();
			c.slaveLook = r.u8();
			c.crime = r.u8();
			c.crimeTimer = r.f32();
			c.escapeHoursLeft = r.f32();
			c.localFaction = r.u32();
			c.bountyTags.resize(r.u8());
			for (CharStats::BountyTag& t : c.bountyTags) { t.crimes = r.u32(); t.claim = r.u8(); t.hoursLeft = r.f32(); }
		}
		if (!r.bad && r.n - r.at >= 18)	// the engage trigger (row 34, k16.5.37): the last 18 bytes today; row 33's bytes will land after them, so read by the frame length
		{
			c.engageBasis = r.u8();
			c.engageSense = r.u8();
			c.engageCause = r.u64();
			c.engageTick = r.u64();
		}
		if (!r.bad) { charStats[id] = std::move(c); }
		break;
	}
	case kDetect:
	{
		Detect d;
		d.tick = r.u64();
		const uint64_t id = r.u64();
		d.meter = r.f32();
		d.state = r.u8();
		d.by = r.u64();
		if (r.bad) { break; }
		if (d.state == Detect::kNone) { detect.erase(id); }
		else { detect[id] = d; }
		break;
	}
	case kSeenBy:
	{
		SeenBy s;
		s.tick = r.u64();
		const uint64_t id = r.u64();
		s.by.resize(r.u16());
		for (uint64_t& o : s.by) { o = r.u64(); }
		if (r.bad) { break; }
		if (s.by.empty()) { seenBy.erase(id); }
		else { seenBy[id] = std::move(s); }
		seenBySerial++;
		break;
	}
	case kDetectedBy:
	{
		DetectedBy d;
		d.tick = r.u64();
		const uint64_t id = r.u64();
		const uint16_t n = r.u16();
		d.by.resize(n);
		for (DetectedBy::Entry& e : d.by) { e.observer = r.u64(); e.state = r.u8(); e.friendly = r.u8(); }
		if (r.bad) { break; }
		if (d.by.empty()) { detectedBy.erase(id); }
		else { detectedBy[id] = std::move(d); }
		detectedBySerial++;
		break;
	}
	case kSquads:
	{
		r.u64();
		squads.resize(r.u32());
		for (Squad& s : squads) { s.id = r.u64(); s.faction = r.u32(); s.x = r.f32(); s.z = r.f32(); s.members = r.u16(); }
		break;
	}
	case kJobs:
	{
		r.u64();
		std::vector<std::pair<uint64_t, Jobs>> v(r.u16());
		for (auto& [id, j] : v)
		{
			id = r.u64();
			j.enabled = r.u8() != 0;
			j.rows.resize(r.u16());
			for (JobRow& row : j.rows) { row.jobId = r.u32(); row.type = r.u16(); row.target = r.u64(); row.active = r.u8() != 0; }
			if (r.bad) { break; }
		}
		if (!r.bad)
		{
			for (auto& [id, j] : v) { jobs[id] = std::move(j); }
			jobsSerial++;
		}
		break;
	}
	case kProspect:
	{
		Prospect p;
		r.u64();
		p.id = r.u64();
		p.x = r.f32(); p.z = r.f32(); p.altitude = r.f32(); p.science = r.f32(); p.r = r.f32();
		p.zone = r.u32();
		p.wind = r.f32();
		p.env.resize(r.u8());
		for (auto& [c, f] : p.env) { c = r.u8(); f = r.f32(); }
		p.res.resize(r.bad ? 0 : r.u8());
		for (auto& [id, v] : p.res) { id = r.u8(); v = r.f32(); }
		p.n = r.bad ? 0 : r.u16();
		const size_t nn = (size_t)p.n * p.n;
		for (std::vector<uint8_t>* plane : { &p.grey, &p.planes[0], &p.planes[1], &p.planes[2] })
		{
			plane->resize(r.bad ? 0 : nn);
			for (uint8_t& b : *plane) { b = r.u8(); }
		}
		p.deposits.resize(r.bad ? 0 : r.u16());
		for (Prospect::Deposit& d : p.deposits) { d.res = r.u8(); d.x = r.f32(); d.z = r.f32(); d.richness = r.f32(); }
		if (!r.bad)
		{
			prospect = std::move(p);
			prospectSerial++;
		}
		break;
	}
	case kDiscovered:
	{
		r.u64();
		std::vector<Place> places(r.u16());
		for (Place& p : places) { p.town = r.u32(); p.visited = r.u8() != 0; }
		if (!r.bad)
		{
			discovered = std::move(places);
			discoveredSerial++;
		}
		break;
	}
	case kFactionRelations:
	{
		r.u64();
		std::vector<Relation> rows(r.u16());
		for (Relation& x : rows) { x.faction = r.u32(); x.name = r.str(); x.value = r.f32(); }
		if (!r.bad)
		{
			relations = std::move(rows);
			relationsSerial++;
		}
		break;
	}
	case kTownState:
	{
		r.u64();
		std::vector<TownState> v(r.u16());
		for (TownState& t : v)
		{
			t.town = r.u32();
			t.owner = r.u32();
			t.flags = r.u8();
			t.live = r.u16();
			t.residentsMaxCur = (int32_t)r.u32(); t.residentsMaxDes = (int32_t)r.u32(); t.residentsActual = (int32_t)r.u32();
			t.roamingMaxCur = (int32_t)r.u32(); t.roamingMaxDes = (int32_t)r.u32(); t.roamingPop = (int32_t)r.u32(); t.roamingSquads = (int32_t)r.u32();
			t.residentFactions.resize(r.u8());
			for (uint32_t& f : t.residentFactions) { f = r.u32(); }
			t.hostile.resize(r.u8());
			for (uint32_t& f : t.hostile) { f = r.u32(); }
			if (r.bad) { break; }
		}
		if (!r.bad)
		{
			townStates = std::move(v);
			townStateSerial++;
		}
		break;
	}
	case kBuilding:
	{
		const uint64_t tick = r.u64();
		std::vector<Building> v(r.u16());
		for (Building& b : v)
		{
			b.tick = tick;
			b.id = r.u64();
			b.record = r.u32();
			b.owner = r.u64();
			for (float& f : b.pos) { f = r.f32(); }
			for (float& f : b.rot) { f = r.f32(); }
			b.floor = r.u8();
			b.status = r.u8();
			b.flags = r.u32();
			b.progress = r.f32();
			b.rate = r.f32();
			b.hp = r.f32();
			b.maxHp = r.f32();
			b.materials.resize(r.bad ? 0 : r.u8());
			for (Building::Material& m : b.materials) { m.item = r.u32(); m.have = r.u16(); m.need = r.u16(); }
			b.powerValue = r.f32();
			b.powerSupplied = r.f32();
			b.powerDemand = r.f32();
			b.stored = r.f32();
			b.capacity = r.f32();
			b.workers = r.u8();
			b.maxWorkers = r.u8();
			b.efficiency = r.f32();
			b.maxEfficiency = r.f32();
			b.quality = r.f32();
			b.town = r.u64();
			b.version = r.u32();
		}
		if (!r.bad)
		{
			for (Building& b : v) { buildings[b.id] = std::move(b); }
			buildingsSerial++;
		}
		break;
	}
	case kTaskProgress:
	{
		const uint64_t tick = r.u64();
		std::vector<TaskProgress> v(r.u16());
		for (TaskProgress& t : v)
		{
			t.ch = r.u64();
			t.kind = r.u8();
			t.progress = r.f32();
			t.rate = r.f32();
			t.value = r.f32();
		}
		// Tail (SIMLINK TASK_PROGRESS "Tail, kinds and levels", h2v.16.1): u8 flags[n], then u64 target[n], in row order; an older server sends none,
		// so a frame without the bytes keeps flags / target 0.
		if (!r.bad && r.n - r.at >= v.size() * 9)
		{
			for (TaskProgress& t : v) { t.flags = r.u8(); }
			for (TaskProgress& t : v) { t.target = r.u64(); }
		}
		if (!r.bad)
		{
			tasks = std::move(v);
			tasksTick = tick;
		}
		break;
	}
	case kItemStats:
	{
		r.u64();
		const uint64_t id = r.u64();
		const uint32_t version = r.u32();
		const uint16_t index = r.u16();
		ItemStats st;
		st.kind = r.u8();
		if (st.kind == 1)
		{
			st.blunt = r.f32();
			st.cut = r.f32();
			st.cutEfficiency = r.f32();
			st.harpoon = r.f32();
			st.coverage.resize(r.bad ? 0 : r.u8());
			for (ItemStats::Cover& c : st.coverage) { c.part = r.u32(); c.fraction = r.f32(); }
			st.weight = r.f32();
			st.combatSpeedMult = r.f32();
			st.dodgeMult = r.f32();
			st.damageOutputMult = r.f32();
			st.attackBonus = r.f32();
			st.defenceBonus = r.f32();
			st.unarmedBonus = r.f32();
			st.armourClass = r.u8();
			st.material = r.u8();
			st.stealthMult = r.f32();
			st.athleticsMult = r.f32();
			st.loadedCombatSpeedMult = r.f32();
			st.dexterityMult = r.f32();
			st.meleeAttack = r.i32();
			st.meleeDefence = r.i32();
		}
		else if (st.kind == 2)
		{
			st.cutDamage = r.f32();
			st.bluntDamage = r.f32();
			st.cutMult = r.f32();
			st.bluntMult = r.f32();
			st.bleed = r.f32();
			st.armourPenetration = r.f32();
			st.attackMod = r.f32();
			st.defenceMod = r.f32();
			st.weight = r.f32();
			st.length = r.f32();
			st.weaponType = r.u8();
		}
		st.value = r.f32();
		st.sellValue = r.f32();
		st.grade = r.u8();
		st.level = r.f32();
		if (!r.bad)
		{
			itemStats[{ id, version, index }] = std::move(st);
			itemStatsSerial++;
		}
		break;
	}
	case kTrade:
	{
		Trade t;
		t.tick = r.u64();
		t.trader = r.u64();
		t.actor = r.u64();
		t.stock = r.u64();
		t.tmpl = r.u32();
		t.traderMoney = r.i64();
		t.stockVersion = r.u32();
		t.stockPrices.resize(r.u16());
		for (Trade::StockPrice& x : t.stockPrices) { x.index = r.u16(); x.buy = r.i32(); x.sell = r.i32(); }
		t.actorVersion = r.u32();
		t.actorPrices.resize(r.bad ? 0 : r.u16());
		for (Trade::ActorPrice& x : t.actorPrices) { x.index = r.u16(); x.sell = r.i32(); }
		if (!r.bad && r.at < r.n)	// an older server: no tail
		{
			t.chests.resize(r.u16());
			for (Trade::ChestPrices& c : t.chests)
			{
				c.container = r.u64();
				c.version = r.u32();
				c.prices.resize(r.bad ? 0 : r.u16());
				for (Trade::ActorPrice& x : c.prices) { x.index = r.u16(); x.sell = r.i32(); }
			}
			if (!r.bad && r.at < r.n)	// an older server: b = 0
			{
				t.buyback.resize(r.u16());
				for (Trade::Buyback& x : t.buyback) { x.index = r.u16(); x.count = r.u32(); x.price = r.i32(); }
			}
		}
		if (!r.bad)
		{
			t.valid = true;
			trade = std::move(t);
			tradeSerial++;
		}
		break;
	}
	case kBuildingGrid:
	{
		r.u64();
		std::vector<PlayerTown> v(r.u16());
		for (PlayerTown& t : v)
		{
			t.id = r.u64();
			t.name = r.str();
			t.supply = r.f32();
			t.demand = r.f32();
			t.stored = r.f32();
			t.capacity = r.f32();
			t.generators = r.u16();
			t.consumers = r.u16();
			t.batteries = r.u16();
		}
		if (!r.bad)
		{
			playerTowns = std::move(v);
			playerTownsSerial++;
		}
		break;
	}
	case kUnlocks:
	{
		r.u64();
		std::vector<uint32_t> res(r.u16());
		for (uint32_t& s : res) { s = r.u32(); }
		std::vector<uint32_t> bld(r.bad ? 0 : r.u16());
		for (uint32_t& s : bld) { s = r.u32(); }
		if (!r.bad)
		{
			research = std::move(res);
			unlocked = std::move(bld);
			unlocksSerial++;
		}
		break;
	}
	case kResearchState:
	{
		ResearchState v;
		v.tick = r.u64();
		v.techLevel = r.u8();
		v.records.resize(r.bad ? 0 : r.u16());
		for (ResearchRecord& rec : v.records) { rec.research = r.u32(); rec.state = r.u8(); rec.reason = r.u8(); }
		const unsigned kinds = r.bad ? 0 : r.u8();
		for (unsigned k = 0; k < kinds && !r.bad; k++)
		{
			const uint8_t kind = r.u8();
			std::vector<uint32_t> refs(r.bad ? 0 : r.u16());
			for (uint32_t& ref : refs) { ref = r.u32(); }
			v.unlocks[kind] = std::move(refs);
		}
		if (!r.bad)	// trailing bytes (later steps append) are ignored
		{
			v.valid = true;
			researchState = std::move(v);
			researchStateSerial++;
		}
		break;
	}
	case kStarts:	// no leading tick (SIMLINK 6 STARTS)
	{
		std::vector<Start> v(r.u16());
		for (Start& s : v)
		{
			s.startoff = r.u32();
			s.ready = r.u8() != 0;
			s.forceRace.resize(r.bad ? 0 : r.u8());
			for (uint32_t& f : s.forceRace) { f = r.u32(); }
			s.members.resize(r.bad ? 0 : r.u8());
			for (Member& m : s.members) { m.race = r.u32(); m.female = r.u8() != 0; m.name = r.str(); m.flags = r.u8(); m.character = r.u32(); m.armour.resize(r.bad ? 0 : r.u8()); for (uint32_t& a : m.armour) { a = r.u32(); } m.weapon = r.u32(); }
		}
		if (!r.bad)
		{
			starts = std::move(v);
			startsSerial++;
		}
		break;
	}
	case kMenuEntries:	// order 99's answer: tick, clientSeq, id, target, entries (a bitset of the right-click rows that apply)
		r.u64();
	{
		MenuEntries m;
		m.clientSeq = r.u32();
		m.id = r.u64();
		m.target = r.u64();
		m.entries = r.u32();
		if (!r.bad && (m.entries & (1u << 16))) { m.pickChance = r.f32(); }	// a cage's pick lock: the actor's chance per roll
		if (!r.bad) { menuEntries.push_back(m); }
		break;
	}
	case kCages:	// tick, then per caged character: id, cage key, record strid, flags (bit 0 locked), level u16, hardness, maxHardness
	{
		r.u64();
		std::unordered_map<uint64_t, Cage> v;
		for (uint16_t i = 0, n = r.u16(); i < n && !r.bad; i++)
		{
			const uint64_t id = r.u64();
			Cage c;
			c.cage = r.u64();
			c.record = r.u32();
			c.locked = r.u8() & 1;
			c.level = r.u16();
			c.hardness = r.f32();
			c.maxHardness = r.f32();
			if (!r.bad) { v[id] = c; }
		}
		if (!r.bad) { cages = std::move(v); cagesSerial++; }
		break;
	}
	case kCameraStart:	// flags u8, pos f64 x3, rot f32 x4 (w, x, y, z), zoom f32, floor u8 (no tick)
	{
		CameraStart c;
		c.flags = r.u8();
		for (double& v : c.pos) { v = r.get<double>(); }
		for (float& v : c.rot) { v = r.f32(); }
		c.zoom = r.f32();
		c.floor = r.u8();
		if (!r.bad) { cameraStart = c; cameraStartSerial++; }
		break;
	}
	case kNewGameResult:
	{
		newGame.result = r.u8();
		newGame.members.clear();
		newGame.faction = 0;
		newGame.reason = 0;
		newGame.hasPos = false;
		if (newGame.result == 0)
		{
			newGame.faction = r.u64();
			newGame.members.resize(r.u16());
			for (uint64_t& id : newGame.members) { id = r.u64(); }
			newGame.hasPos = !r.bad && r.n - r.at >= 12;	// appended later (SIMLINK 5c5c563): read only when present
			if (newGame.hasPos) { for (float& v : newGame.pos) { v = r.f32(); } }
		}
		else { newGame.reason = r.u32(); }
		if (!r.bad) { newGameSerial++; }
		break;
	}
	case kScenarioList:
	{
		r.u32();	// clientSeq
		std::vector<SimLinkDecoder::ScenarioInfo> v(r.bad ? 0 : r.u16());
		for (SimLinkDecoder::ScenarioInfo& s : v)
		{
			s.name = r.str();
			s.title = r.str();
			s.setup = r.str();
			s.lookFor = r.str();
			s.fileHash = r.u64();
			s.metrics.resize(r.bad ? 0 : r.u8());
			for (std::string& m : s.metrics) { m = r.str(); }
			if (r.bad) { break; }
		}
		// The 2026-11-10 tail (SIMLINK "The bead tail"): u16 n, n x str bead in list order. An older server sends none: all "".
		if (!r.bad && r.at + 2 <= r.n)
		{
			const size_t nb = std::min<size_t>(r.u16(), v.size());
			for (size_t i = 0; i < nb; i++) { v[i].bead = r.str(); }
		}
		if (!r.bad)
		{
			scenarioList = std::move(v);
			scenarioListSerial++;
		}
		break;
	}
	case kScenarioStarted:	// the server's field order (seed before simHash), as it wrote the frame and its test reads it (SIMLINK 2026-11-07, the doc line corrected)
	{
		auto& s = scenarioStarted;
		s.clientSeq = r.u32();
		s.status = r.u8();
		s.error = r.str();
		s.name = r.str();
		s.fileHash = r.u64();
		s.seed = r.u32();
		s.simHash = r.str();
		s.startGameTime = r.get<double>();
		s.centreX = s.centreZ = std::numeric_limits<float>::quiet_NaN();
		s.bead.clear();
		if (r.at < r.n) { s.centreX = r.f32(); s.centreZ = r.f32(); }	// optional tail (sim 55f13304): the file's centre=, NaN = none
		if (!r.bad && r.at < r.n) { s.bead = r.str(); }	// the 2026-11-10 tail: the file's bead=, "" on a refusal (SIMLINK "The bead tail")
		if (!r.bad) { scenarioStartedSerial++; }
		break;
	}
	case kScenarioMetrics:
	{
		auto& m = scenarioMetrics;
		m.clientSeq = r.u32();
		m.name = r.str();
		m.elapsedGameS = r.get<double>();
		m.metrics.resize(r.bad ? 0 : r.u8());
		for (SimLinkDecoder::ScenarioMetric& metric : m.metrics)
		{
			metric.name = r.str();
			metric.value = r.get<double>();
			metric.valid = r.u8() != 0;
		}
		if (!r.bad) { scenarioMetricsSerial++; }
		break;
	}
	case kBeams:
	{
		beamsTick = r.u64();
		const uint16_t ng = r.u16();
		// The sim forgets a beam at its endTick and only bit 0 (a weather change, a pin, a load, the connect snapshot) clears a group, so a Venge
		// weather, which lasts game days, would otherwise keep every ended beam and its linear id search would grow with them for that long. Drop the
		// beams whose particles have finished (endTick + the tail handles the psys outliving the damage window, rwh.48 4.4), before the list is applied.
		for (BeamGroup& g : beamGroups) { std::erase_if(g.beams, [&](const Beam& b) { return b.endTick + kBeamParticleTailTicks < beamsTick; }); }
		for (uint16_t gi = 0; gi < ng && !r.bad; gi++)
		{
			const uint32_t group = r.u32();
			const uint8_t flags = r.u8();
			const uint16_t n = r.u16();
			BeamGroup* g = nullptr;
			for (BeamGroup& e : beamGroups) { if (e.group == group) { g = &e; break; } }
			if (!g) { beamGroups.push_back(BeamGroup()); g = &beamGroups.back(); g->group = group; }
			g->flags = flags;
			if (flags & 1) { g->beams.clear(); }	// a weather change, a pin, a load or the connect snapshot: the sim's set afresh
			for (uint16_t bi = 0; bi < n && !r.bad; bi++)
			{
				Beam b;
				b.id = r.u32();
				b.effect = r.u32();
				b.spawnTick = r.u64();
				b.endTick = r.u64();
				b.visibleTicks = r.u16();
				b.x = r.f32();
				b.z = r.f32();
				b.speed = r.f32();
				b.radius = r.f32();
				b.strength = r.f32();
				b.particles = r.u32();
				b.sound = r.u32();
				b.segments.resize(r.u16());
				for (BeamSegment& s : b.segments) { s.ticks = r.u16(); s.yaw = r.f32(); }
				if (r.bad) { break; }
				Beam* cur = nullptr;
				for (Beam& e : g->beams) { if (e.id == b.id) { cur = &e; break; } }
				if (cur) { *cur = std::move(b); } else { g->beams.push_back(std::move(b)); }
			}
		}
		if (!r.bad)
		{
			if (getenv("KBEAMLOG"))
			{
				size_t total = 0;
				for (const BeamGroup& g : beamGroups) { total += g.beams.size(); }
				fprintf(stderr, "beams frame: tick %llu, %zu groups, %zu beams, %zu bytes\n", (unsigned long long)beamsTick, beamGroups.size(), total, n);
			}
			beamsSerial++;
		}
		break;
	}
	case kSaves:
	{
		r.u64();
		std::vector<SaveInfo> v(r.u16());
		for (SaveInfo& s : v)
		{
			s.name = r.str();
			s.mtime = r.i64();
			s.version = r.u32();
			s.status = r.u8();
			s.day = r.u32();
			s.hour = r.f32();
			s.factionName = r.str();
			s.leaderName = r.str();
			s.members = r.u16();
			s.money = r.i64();
			if (r.bad) { break; }
		}
		if (!r.bad)
		{
			saves = std::move(v);
			savesSerial++;
		}
		break;
	}
	case kPlayerSquads:
	{
		PlayerSquads s;
		s.tick = r.u64();
		s.faction = r.u64();
		s.factionName = r.u32();
		s.factionCount = r.u16();
		s.maxFaction = r.u16();
		s.maxSquad = r.u16();
		s.maxSquads = r.u16();
		s.squads.resize(r.u16());
		for (PlayerSquads::Squad& q : s.squads)
		{
			q.id = r.u64();
			q.name = r.str();
			q.members.resize(r.u16());
			for (uint64_t& m : q.members) { m = r.u64(); }
			if (r.bad) { break; }
		}
		if (!r.bad && r.at < r.n) { s.money = r.i64(); s.hasMoney = true; }
		if (!r.bad)
		{
			s.valid = true;
			playerSquads = std::move(s);
			playerSquadsSerial++;
		}
		break;
	}
	case kClock:
		clock.tick = r.u64();
		clock.day = r.u32();
		clock.hour = r.f32();
		clock.speed = r.f32();
		clock.pauseBits = r.u8();
		clock.paused = clock.pauseBits != 0;
		clock.valid = true;
		break;
	case kWeather:
	{
		const uint64_t wtick = r.u64();
		weather.resize(r.u16());
		for (Weather& w : weather) { w.group = r.u32(); w.weather = r.u32(); w.previous = r.u32(); w.fogBlend = r.f32(); w.skyBlend = r.f32(); w.strength = r.f32(); }
		windValid = r.at + 8 <= r.n;	// the optional tail: no view focus or no region leaves it out
		if (windValid) { windSpeed = r.f32(); windYaw = r.f32(); }
		if (getenv("KWEATHERLOG")) { fprintf(stderr, "weather frame: tick %llu, %zu regions, %zu bytes, %zu trailing bytes\n", (unsigned long long)wtick, weather.size(), n, r.n - r.at); }
		weatherSerial++;
		break;
	}
	case kBounds:
	{
		r.u64();
		bounds.resize(r.u16());
		for (Box& b : bounds) { b.minX = r.f32(); b.minZ = r.f32(); b.maxX = r.f32(); b.maxZ = r.f32(); }
		break;
	}
	case kInteractable:
	{
		r.u64();
		Interactable t;
		t.id = r.u64();
		t.kind = r.u8();
		t.record = r.u32();
		for (float& v : t.pos) { v = r.f32(); }
		for (float& v : t.use) { v = r.f32(); }
		t.state = r.u8();
		t.health = r.f32();
		t.maxHealth = r.f32();
		t.count = r.u16();
		t.version = r.u32();
		if (t.kind == Interactable::kProducer)
		{
			t.output = r.u32(); t.cap = r.u16(); t.operators = r.u8(); t.maxOperators = r.u8();
			if (r.at < r.n) { t.progress = r.f32(); t.rate = r.f32(); t.progressTick = r.u64(); }	// an older server: no tail
		}
		if (r.at < r.n) { t.owner = r.u32(); t.lockLevel = r.u16(); t.model = r.u32(); t.yaw = r.f32(); t.hasOwner = true; }	// an older server: no tail
		if (t.kind == Interactable::kBed) { t.building = r.u64(); t.furniture = r.u32(); t.occupant = r.u64(); }
		else if (t.kind == Interactable::kContainer && r.n - r.at >= 10) { t.building = r.u64(); t.piece = r.u16(); t.hasPiece = true; }	// an older server: no tail, the client matches a piece by position
		if (t.kind == Interactable::kMachine && r.n - r.at >= 10)
		{
			t.machineKind = r.u8(); t.mstate = r.u8(); t.input = r.u32(); t.machineOutput = r.u32();
			if (t.machineKind == 3 && r.n - r.at >= 8) { t.turret = r.u64(); }
			t.hasMachine = true;
		}
		// limb tail (kind 2 only, an ITEM with `item function` 0x11): 9 f32, skin rgb, blood rgb, scale xyz (SIMLINK "Tail, kind 2 severed limb only").
		// The record's item function is not known here, so the tail is read when the bytes are there, as the doc says; a server without it sends
		// nothing after the ownership tail and the item draws as before.
		if (t.kind == Interactable::kItem && r.n - r.at >= 36)
		{
			for (float& v : t.limbSkin) { v = r.f32(); }
			for (float& v : t.limbBlood) { v = r.f32(); }
			for (float& v : t.limbScale) { v = r.f32(); }
			t.hasLimb = true;
		}
		if (!r.bad) { things[t.id] = t; thingsSerial++; }
		break;
	}
	case kInventory:
	{
		r.u64();
		const uint64_t id = r.u64();
		Inventory inv;
		inv.version = r.u32();
		inv.items.resize(r.u16());
		for (Inventory::Entry& e : inv.items) { e.item = r.u32(); e.count = r.u32(); e.slot = r.u8(); }
		if (!r.bad) { inventories[id] = std::move(inv); }
		break;
	}
	case kItemQuality:	// applies to the INVENTORY of the same id and version (it follows it); a stale one is dropped
	{
		r.u64();
		const uint64_t id = r.u64();
		const uint32_t version = r.u32();
		const auto it = inventories.find(id);
		const uint16_t n = r.u16();
		for (uint16_t i = 0; i < n && !r.bad; i++)
		{
			const uint16_t index = r.u16();
			const uint32_t maker = r.u32(), model = r.u32();
			const float level = r.f32();
			const uint8_t grade = r.u8();
			if (r.bad || it == inventories.end() || it->second.version != version || index >= it->second.items.size()) { continue; }
			Inventory::Entry& e = it->second.items[index];
			e.hasQuality = true;
			e.maker = maker;
			e.model = model;
			e.level = level;
			e.grade = grade;
		}
		break;
	}
	case kItemGrid:	// like ITEM_QUALITY: applies to the INVENTORY of the same id and version, a stale one is dropped
	{
		r.u64();
		const uint64_t id = r.u64();
		const uint32_t version = r.u32();
		const auto it = inventories.find(id);
		const bool live = it != inventories.end() && it->second.version == version;
		std::vector<Inventory::Grid> grids(r.u8());
		for (Inventory::Grid& g : grids) { g.section = r.u8(); g.w = r.u8(); g.h = r.u8(); }
		if (live && !r.bad) { it->second.grids = std::move(grids); }
		const uint16_t n = r.u16();
		for (uint16_t i = 0; i < n && !r.bad; i++)
		{
			const uint16_t index = r.u16();
			const uint8_t section = r.u8(), x = r.u8(), y = r.u8();
			const uint32_t material = r.u32(), colour = r.u32();
			if (r.bad || !live || index >= it->second.items.size()) { continue; }
			Inventory::Entry& e = it->second.items[index];
			e.section = section;
			e.x = x;
			e.y = y;
			e.material = material;
			e.colour = colour;
		}
		// Steal tail (SIMLINK ITEM_GRID, k16.6.392): one u8 per entry of the INVENTORY (the list above is that order), absent from an older server: all 255
		if (live && !r.bad)
		{
			std::vector<Inventory::Entry>& items = it->second.items;
			const bool tail = r.n - r.at >= items.size();
			for (Inventory::Entry& e : items) { e.steal = tail ? r.u8() : 255; }
			if (getenv("KSTEALLOG"))
			{
				fprintf(stderr, "steal tail: inv %llu v%u %s:", (unsigned long long)id, version, tail ? "tail" : "no tail");
				for (const Inventory::Entry& e : items) { fprintf(stderr, " %u", (unsigned)e.steal); }
				fprintf(stderr, "\n");
			}
		}
		break;
	}
	case kItemLock:	// like ITEM_GRID: applies to the INVENTORY of the same id and version, a stale one is dropped
	{
		r.u64();
		const uint64_t id = r.u64();
		const uint32_t version = r.u32();
		const auto it = inventories.find(id);
		std::vector<Inventory::Lock> locks(r.u16());
		for (Inventory::Lock& l : locks)
		{
			l.index = r.u16();
			const uint8_t flags = r.u8();
			l.locked = flags & 1;
			l.canCut = flags & 2;
			l.canBreak = flags & 4;
			l.level = r.u16();
			l.chance = r.f32();
			l.hardness = r.f32();
			l.maxHardness = r.f32();
		}
		if (!r.bad && it != inventories.end() && it->second.version == version) { it->second.locks = std::move(locks); }
		break;
	}
	case kNavParams:
	{
		NavParams p;
		p.meshId = r.u32();
		p.detourVersion = r.u32();
		for (float& v : p.origin) { v = r.f32(); }
		p.tileWidth = r.f32();
		p.tileHeight = r.f32();
		p.maxTiles = r.u32();
		p.maxPolys = r.u32();
		p.valid = true;
		if (!nav.valid || p.meshId != nav.meshId) { navTiles.clear(); navPatches.clear(); }
		nav = p;
		navGeneration++;
		break;
	}
	case kNavTile:
	{
		const uint32_t mesh = r.u32();
		const std::array<int32_t, 3> key = { (int32_t)r.u32(), (int32_t)r.u32(), (int32_t)r.u32() };
		const uint32_t version = r.u32(), size = r.u32();
		if (r.bad || n - r.at < size) { r.bad = true; break; }
		const uint8_t* data = p + r.at;
		r.at += size;
		if (!nav.valid || mesh != nav.meshId) { break; }	// another bake's tile
		const auto it = navTiles.find(key);
		if (it != navTiles.end() && version <= it->second.version) { break; }	// older or the same
		if (size == 0) { if (it != navTiles.end()) { navTiles.erase(it); } }
		else { navTiles[key] = { version, version, std::vector<uint8_t>(data, data + size) }; }
		navGeneration++;
		break;
	}
	case kNavPolyFlags:
	{
		const uint32_t mesh = r.u32();
		NavPatch patch;
		patch.key = { (int32_t)r.u32(), (int32_t)r.u32(), (int32_t)r.u32() };
		patch.version = r.u32();
		patch.polys.resize(r.u16());
		for (NavPoly& q : patch.polys) { q.poly = r.u16(); q.flags = r.u16(); q.area = r.u8(); }
		const auto it = navTiles.find(patch.key);
		// it applies to the tile at the previous version; a mismatch means a NAV_TILE is on its way
		if (r.bad || !nav.valid || mesh != nav.meshId || it == navTiles.end() || patch.version != it->second.version + 1) { break; }
		it->second.version = patch.version;
		navPatches.push_back(std::move(patch));
		break;
	}
	case kEvent:
	{
		Event e;
		e.tick = r.u64();
		e.kind = r.u16();
		switch (e.kind)
		{
		case 1:
			e.a = r.u64();
			e.b = r.u64();
			e.str = r.u32();
			for (float& v : e.pos) { v = r.f32(); }
			e.dir = r.u8();
			for (float& v : e.damage) { v = r.f32(); }
			e.result = r.u8();
			if (r.at < r.n) { for (float& v : e.absorbed) { v = r.f32(); } }	// cutAbsorbed, bluntAbsorbed (5a27b7d; recorded streams end at result)
			break;
		case 2:
			for (float& v : e.pos) { v = r.f32(); }
			e.amount = r.f32();
			break;
		case 16:	// bleed pool: a = id, amount = g
			e.a = r.u64();
			for (float& v : e.pos) { v = r.f32(); }
			e.amount = r.f32();
			break;
		case 17:	// blood smear: amount = alpha
			for (float& v : e.pos) { v = r.f32(); }
			e.dir = r.u8();
			e.amount = r.f32();
			break;
		case 3: case 4: case 5: case 8: case 11:
			e.a = r.u64();
			break;
		case 6: case 7:
			e.a = r.u64();
			e.str = r.u32();
			for (float& v : e.pos) { v = r.f32(); }
			break;
		case 9:
			e.a = r.u32();
			e.b = r.u64();
			break;
		case 10:
			e.a = r.u32();
			e.str = r.u32();
			break;
		case 12:
			e.a = r.u64();
			e.amount = r.f32();
			break;
		case 13:
			e.a = r.u64();
			e.b = r.u64();
			e.dir = r.u8();
			e.result = r.u8();
			break;
		case 14:
			e.a = r.u64();
			e.b = r.u64();
			e.str = r.u32();
			break;
		case 47:	// limb severed: a = the character, str = the part (its CHAR_MEDICAL row's strid)
			e.a = r.u64();
			e.str = r.u32();
			break;
		case 18:	// dismissed: a = the character, str = its new faction's name, b = its new squad
			e.a = r.u64();
			e.str = r.u32();
			e.b = r.u64();
			break;
		case 19:	// discovered: str = the TOWN
			e.str = r.u32();
			break;
		case 20:	// lock_roll: a = the picker, b = the door, result = success
			e.a = r.u64();
			e.b = r.u64();
			e.result = r.u8();
			break;
		case 40:	// trade_closed: a = the trader, result = reason (0 client, 1 order, 2 trader gone)
			e.a = r.u64();
			e.result = r.u8();
			break;
		case 41:	// speech: a = the speaker, b = the target, str = the resolved text, ref = the DIALOGUE and DIALOGUE_LINE, amount = seconds shown, dir = flags
			e.a = r.u64();
			e.b = r.u64();
			for (uint32_t& v : e.ref) { v = r.u32(); }
			e.str = r.u32();
			e.amount = r.f32();
			e.dir = r.u8();
			if (e.dir & 4) { std::erase_if(menus, [&](const ReplyMenu& x) { return x.ch == e.a; }); }	// the answer spoken: its menu is closed (no event 44 for a reply; a char answers one menu at a time)
			break;
		case 42:	// speech_end: a = the speaker, its bubble cut short
			e.a = r.u64();
			break;
		case 43:	// reply_menu: a = the speaker, b = the answering char, dir = flags, result = options; the menu itself goes to menus
		{
			ReplyMenu m;
			e.a = m.speaker = r.u64();
			e.b = m.ch = r.u64();
			m.seq = r.u32();
			e.dir = m.flags = r.u8();
			e.result = r.u8();
			m.options.resize(e.result);
			for (ReplyMenu::Option& o : m.options) { o.line = r.u32(); o.text = r.u32(); }
			std::erase_if(menus, [&](const ReplyMenu& x) { return x.speaker == m.speaker && x.ch == m.ch; });
			if (!r.bad) { menus.push_back(std::move(m)); }
			break;
		}
		case 44:	// conversation_end: a = the speaker, b = the char, result = reason (0 finished, 2 interrupted, 3 gone, 4 end_talk / walked away, 5 no client)
			e.a = r.u64();
			e.b = r.u64();
			e.result = r.u8();
			std::erase_if(menus, [&](const ReplyMenu& x) { return x.speaker == e.a && x.ch == e.b; });
			break;
		case 48:	// ragdoll: a = the character, result = cause (0 KO, 1 death, 2 drop, 3 launch), pos = impulse dir (unit, world; 0 = collapse), amount = strength (u/s), str = the part, ref[0] = seed
			e.a = r.u64();
			e.result = r.u8();
			for (float& v : e.pos) { v = r.f32(); }
			e.amount = r.f32();
			e.str = r.u32();
			e.ref[0] = r.u32();
			break;
		case 49:	// shot: a = the shooter, b = the target, pos = the muzzle, to = the target point, result = 0 hit / 1 blocked (reserved) / 2 missed, str = the CROSSBOW, c = the turret (0: in hand)
			e.a = r.u64();
			e.b = r.u64();
			for (float& v : e.pos) { v = r.f32(); }
			for (float& v : e.to) { v = r.f32(); }
			e.result = r.u8();
			e.str = r.u32();
			e.c = r.u64();
			break;
		case 50:	// stealth_ko: a = the assassin, b = the victim, result = 0 landed / 1 roll failed / 2 aborted (the victim was aware), amount = the roll's chance (0..100; 0 on an abort)
			e.a = r.u64();
			e.b = r.u64();
			e.result = r.u8();
			e.amount = r.f32();
			break;
		case 51:	// caught_stealing: a = the thief, b = the witness (0: the failed steal roll), result = cause (0 roll failed, 1 take witnessed, 2 lockpick / break witnessed), c = the container / door (0 none)
			e.a = r.u64();
			e.b = r.u64();
			e.result = r.u8();
			e.c = r.u64();
			break;
		case 52:	// alarm_raised: a = the raiser, b = the criminal, result = the CrimeEnum id, str = the raiser's FACTION, pos = the alarm's point
			e.a = r.u64();
			e.b = r.u64();
			e.result = r.u8();
			e.str = r.u32();
			for (float& v : e.pos) { v = r.f32(); }
			break;
		case 15:	// noticed: a = observer, b = target
			e.a = r.u64();
			e.b = r.u64();
			break;
		default:
			skipped("event kind", e.kind);
			return true;
		}
		if (e.kind == 9 && resetSeq && e.a == resetSeq)
		{
			chars.clear(); things.clear(); thingsSerial++; medical.clear(); inventories.clear(); detect.clear(); seenBy.clear(); detectedBy.clear(); jobs.clear(); research.clear(); unlocked.clear(); researchState = {}; discovered.clear(); relations.clear(); townStates.clear(); buildings.clear(); playerTowns.clear(); tasks.clear(); prospect = {}; squads.clear(); playerSquads = {}; weather.clear(); beamGroups.clear(); beamsTick = 0; bounds.clear(); trade = {}; itemStats.clear(); menus.clear(); cages.clear();
			resetSeq = 0;
			resetSerial++;
		}
		if (e.kind == 40 && trade.valid)
		{
			inventories.erase(trade.stock);
			trade = {};
			tradeSerial++;
		}
		if (e.kind == 11) { chars.erase(e.a); if (things.erase(e.a)) { thingsSerial++; } inventories.erase(e.a); medical.erase(e.a); charStats.erase(e.a); detect.erase(e.a); seenBy.erase(e.a); detectedBy.erase(e.a); jobs.erase(e.a); cages.erase(e.a); if (buildings.erase(e.a)) { buildingsSerial++; } }
		events.push_back(e);
		break;
	}
	case kGameOptions:	// u64 tick, u8 flags (bit 0 feed_animals, bit 1 share_food), f32 cod, f32 gdm, f32 ht; read by the frame length, a longer tail is skipped
	{
		GameOptions g;
		g.tick = r.u64();
		const uint8_t f = r.u8();
		g.feedAnimals = (f & 1) != 0;
		g.shareFood = (f & 2) != 0;
		g.cod = r.f32();
		g.gdm = r.f32();
		g.ht = r.f32();
		if (!r.bad) { g.received = true; gameOptions = g; }
		break;
	}
	case kProfile:
	{
		// PROFILE (SIMLINK.md "PROFILE", kenshi-k16.2.148): the answer to order 120 from a KSIM_PROFILE server. Fields in the doc's order; a
		// newer server may append (the trailing-byte rule).
		Profile p;
		p.tick = r.u64();
		p.window = r.u32();
		p.tickMeanUs = r.f32();
		p.tickP50Us = r.f32();
		p.tickP99Us = r.f32();
		p.tickMaxUs = r.f32();
		p.tickBudgetUs = r.f32();
		p.characters = r.f32();
		p.squads = r.f32();
		p.pathQueries = r.f32();
		p.pathUs = r.f32();
		const uint16_t n = r.u16();
		for (uint16_t i = 0; i < n && !r.bad; i++)
		{
			Profile::Row row;
			row.name = r.str();
			row.meanUs = r.f32();
			row.maxUs = r.f32();
			row.totalNs = r.u64();
			if (!r.bad) { p.rows.push_back(std::move(row)); }
		}
		if (!r.bad) { p.valid = true; profile = std::move(p); profileSerial++; }
		break;
	}
	default:
		if (type >= 200 && type <= 219)	// DEBUG VIEW: not part of the game protocol
		{
			if (debugFrames) { debugFrames(type, p, n); }
			return true;
		}
		skipped("frame type", type);	// newer server: the frame length makes skipping safe; the HELLO protocol check is the gate (SIMLINK 11)
		return true;
	}
	if (r.bad) { error_ = "frame " + std::to_string(type) + " too short (" + std::to_string(n) + " bytes)"; return false; }
	if (r.at != n) { skipped("trailing bytes in frame type", type); }	// a newer server's appended tail (b0's rule: tails only ever append)
	return true;
}

void OrderWriter::hello(std::vector<uint8_t>& out, uint64_t dataHash, const std::string& name)
{
	Writer w(out, kHello);
	w.put(kMagic);
	w.put(kProtocol);
	w.put((uint8_t)0);
	w.put(dataHash);
	w.str(name);
}

uint32_t OrderWriter::moveTo(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, float x, float z, bool queue, float y, bool yValid)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)1);
	w.ids(ids);
	w.put(x);
	w.put(z);
	w.put(std::isfinite(y) ? y : 0.0f);
	w.put((uint8_t)((queue ? 1 : 0) | (yValid && std::isfinite(y) ? 2 : 0)));
	return seq_;
}

uint32_t OrderWriter::attack(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint64_t target)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)2);
	w.ids(ids);
	w.put(target);
	return seq_;
}

uint32_t OrderWriter::follow(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint64_t target)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)3);
	w.ids(ids);
	w.put(target);
	return seq_;
}

uint32_t OrderWriter::interact(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint64_t target, uint8_t action, bool queue, uint8_t flags)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)60);
	w.ids(ids);
	w.put(target);
	w.put(action);
	w.put((uint8_t)queue);
	if (flags) { w.put(flags); }
	return seq_;
}

uint32_t OrderWriter::drop(std::vector<uint8_t>& out, uint64_t id, const std::string& item, uint16_t count)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)61);
	w.put(id);
	w.str(item);
	w.put(count);
	return seq_;
}

uint32_t OrderWriter::equip(std::vector<uint8_t>& out, uint64_t id, const std::string& item, bool on)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)62);
	w.put(id);
	w.str(item);
	w.put((uint8_t)on);
	return seq_;
}

uint32_t OrderWriter::transfer(std::vector<uint8_t>& out, uint64_t id, uint64_t container, const std::string& item, uint16_t count, bool take)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)63);
	w.put(id);
	w.put(container);
	w.str(item);
	w.put(count);
	w.put((uint8_t)take);
	return seq_;
}

uint32_t OrderWriter::moveItem(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index, uint8_t section, uint8_t x, uint8_t y, uint16_t count)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)64);
	w.put(id);
	w.put(version);
	w.put(index);
	w.put(section);
	w.put(x);
	w.put(y);
	w.put(count);
	return seq_;
}

uint32_t OrderWriter::arrange(std::vector<uint8_t>& out, uint64_t id, uint8_t section)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)65);
	w.put(id);
	w.put(section);
	return seq_;
}

uint32_t OrderWriter::dropEntry(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index, uint16_t count)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)66);
	w.put(id);
	w.put(version);
	w.put(index);
	w.put(count);
	return seq_;
}

uint32_t OrderWriter::equipEntry(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index, bool on)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)67);
	w.put(id);
	w.put(version);
	w.put(index);
	w.put((uint8_t)on);
	return seq_;
}

uint32_t OrderWriter::saveGame(std::vector<uint8_t>& out, const std::string& name)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)80);
	w.str(name);
	return seq_;
}

uint32_t OrderWriter::loadGame(std::vector<uint8_t>& out, const std::string& name, bool resetPositions)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)81);
	w.str(name);
	w.put((uint8_t)resetPositions);
	return seq_;
}

uint32_t OrderWriter::deleteSave(std::vector<uint8_t>& out, const std::string& name)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)82);
	w.str(name);
	return seq_;
}

uint32_t OrderWriter::listSaves(std::vector<uint8_t>& out)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)83);
	return seq_;
}

uint32_t OrderWriter::profileQuery(std::vector<uint8_t>& out)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)120);
	return seq_;
}

uint32_t OrderWriter::newWorld(std::vector<uint8_t>& out)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)84);
	return seq_;
}

uint32_t OrderWriter::scenarioList(std::vector<uint8_t>& out)
{
	Writer w(out, kScenarioListReq);
	w.put(++seq_);
	return seq_;
}

uint32_t OrderWriter::scenarioLoad(std::vector<uint8_t>& out, const std::string& name, uint32_t seed)
{
	Writer w(out, kScenarioLoad);
	w.put(++seq_);
	w.str(name);
	w.put(seed);
	return seq_;
}

uint32_t OrderWriter::scenarioEnd(std::vector<uint8_t>& out)
{
	Writer w(out, kScenarioEnd);
	w.put(++seq_);
	return seq_;
}

uint32_t OrderWriter::newGame(std::vector<uint8_t>& out, const std::string& name, uint32_t startoff, const std::vector<NewGameSpec>& specs)
{
	Writer w(out, kNewGame);
	w.put(++seq_);
	w.str(name);
	w.put(startoff);
	w.put((uint8_t)specs.size());
	for (const NewGameSpec& s : specs)
	{
		w.put(s.race);
		w.put((uint8_t)s.female);
		w.put(s.head);
		w.put(s.hair);
		w.put(s.beard);
		w.put(s.hairColour);
		w.put(s.skin);
		w.put((uint8_t)s.sliders.size());
		for (const auto& [n, v] : s.sliders) { w.str(n); w.put(v); }
		w.str(s.name);
		w.put(s.stance);
	}
	return seq_;
}

uint32_t OrderWriter::squadAdd(std::vector<uint8_t>& out)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)70);
	return seq_;
}

uint32_t OrderWriter::squadDelete(std::vector<uint8_t>& out, uint64_t squad)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)71);
	w.put(squad);
	return seq_;
}

uint32_t OrderWriter::squadRename(std::vector<uint8_t>& out, uint64_t squad, const std::string& name)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)72);
	w.put(squad);
	w.str(name);
	return seq_;
}

uint32_t OrderWriter::factionRename(std::vector<uint8_t>& out, const std::string& name)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)85);
	w.str(name);
	return seq_;
}

uint32_t OrderWriter::squadMove(std::vector<uint8_t>& out, uint64_t id, uint64_t squad, uint16_t index, bool swap)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)73);
	w.put(id);
	w.put(squad);
	w.put(index);
	w.put((uint8_t)swap);
	return seq_;
}

uint32_t OrderWriter::squadReorder(std::vector<uint8_t>& out, uint64_t squad, uint16_t index)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)74);
	w.put(squad);
	w.put(index);
	return seq_;
}

uint32_t OrderWriter::dismiss(std::vector<uint8_t>& out, uint64_t id)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)75);
	w.put(id);
	return seq_;
}

uint32_t OrderWriter::transferEntry(std::vector<uint8_t>& out, uint64_t id, uint64_t container, uint32_t version, uint16_t index, uint16_t count, bool take, uint8_t section, uint8_t x, uint8_t y)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)68);
	w.put(id);
	w.put(container);
	w.put(version);
	w.put(index);
	w.put(count);
	w.put((uint8_t)take);
	w.put(section);
	w.put(x);
	w.put(y);
	return seq_;
}

uint32_t OrderWriter::toggle(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint8_t which, uint8_t value)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)10);
	w.ids(ids);
	w.put(which);
	w.put(value);
	return seq_;
}

uint32_t OrderWriter::stop(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, bool hold)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)4);
	w.ids(ids);
	w.put((uint8_t)hold);
	return seq_;
}

uint32_t OrderWriter::jobAdd(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint16_t type, uint64_t target)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)76);
	w.ids(ids);
	w.put(type);
	w.put(target);
	return seq_;
}

uint32_t OrderWriter::prospect(std::vector<uint8_t>& out, uint64_t id)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)79);
	w.put(id);
	return seq_;
}

uint32_t OrderWriter::jobRemove(std::vector<uint8_t>& out, uint64_t id, uint32_t jobId)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)77);
	w.put(id);
	w.put(jobId);
	return seq_;
}

uint32_t OrderWriter::jobMove(std::vector<uint8_t>& out, uint64_t id, uint32_t jobId, uint16_t index)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)78);
	w.put(id);
	w.put(jobId);
	w.put(index);
	return seq_;
}

uint32_t OrderWriter::itemStats(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)69);
	w.put(id);
	w.put(version);
	w.put(index);
	return seq_;
}

uint32_t OrderWriter::reply(std::vector<uint8_t>& out, uint64_t id, uint64_t speaker, uint32_t menuSeq, uint8_t option)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)88);
	w.put(id);
	w.put(speaker);
	w.put(menuSeq);
	w.put(option);
	return seq_;
}

uint32_t OrderWriter::endTalk(std::vector<uint8_t>& out, uint64_t id, uint64_t speaker)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)89);
	w.put(id);
	w.put(speaker);
	return seq_;
}

uint32_t OrderWriter::tradeClose(std::vector<uint8_t>& out)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)86);
	return seq_;
}

uint32_t OrderWriter::pause(std::vector<uint8_t>& out, bool paused)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)40);
	w.put((uint8_t)paused);
	return seq_;
}

uint32_t OrderWriter::setSpeed(std::vector<uint8_t>& out, float speed)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)41);
	w.put(speed);
	return seq_;
}

uint32_t OrderWriter::drive(std::vector<uint8_t>& out, uint64_t id, uint8_t drive, float heading, int8_t strafe)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)20);
	w.put(id);
	w.put(drive);
	w.put(heading);
	if (strafe) { w.put(strafe); }	// the 13-byte body is strafe 0 (SIMLINK "Direct drive", kenshi-k16.2.237.4)
	return seq_;
}

uint32_t OrderWriter::focus(std::vector<uint8_t>& out, uint64_t id)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)87);
	w.put(id);
	return seq_;
}

uint32_t OrderWriter::setProgressLevel(std::vector<uint8_t>& out, uint8_t level)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)98);
	w.put(level);
	return seq_;
}

uint32_t OrderWriter::menuQuery(std::vector<uint8_t>& out, uint64_t id, uint64_t target)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)99);
	w.put(id);
	w.put(target);
	return seq_;
}

uint32_t OrderWriter::setInterest(std::vector<uint8_t>& out, float x, float z, float radius)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)50);
	w.put(x);
	w.put(z);
	w.put(radius);
	return seq_;
}

uint32_t OrderWriter::setGameOption(std::vector<uint8_t>& out, uint8_t option, uint8_t value)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)121);
	w.put((uint8_t)1);
	w.put(option);
	w.put(value);
	return seq_;
}

uint32_t OrderWriter::setCameraFollow(std::vector<uint8_t>& out, uint64_t id)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)52);
	w.put(id);
	return seq_;
}

uint32_t OrderWriter::setView(std::vector<uint8_t>& out, float x, float z)
{
	Writer w(out, kOrder);
	w.put(++seq_);
	w.put((uint16_t)51);
	w.put(x);
	w.put(z);
	return seq_;
}
}
