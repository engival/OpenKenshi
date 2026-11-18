// ksimlink_dump: decodes a recorded SIMLINK server byte stream (docs/formats/SIMLINK.md) and prints a summary; --selftest decodes a synthetic stream.
//   ksimlink_dump FILE.bin
//   ksimlink_dump --selftest
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "net/simlink.hpp"

using namespace simlink;

static int dump(const simlink::SimLinkDecoder& d)
{
	printf("frames %zu  server \"%s\"  data hash %016llx  last tick %llu\n", d.frames, d.serverName().c_str(), (unsigned long long)d.serverDataHash(), (unsigned long long)d.lastCompleteTick);
	if (d.clock.valid) { printf("clock: tick %llu day %u hour %.3f speed %.2f%s\n", (unsigned long long)d.clock.tick, d.clock.day, d.clock.hour, d.clock.speed, d.clock.paused ? " paused" : ""); }
	printf("squads %zu  weather %zu  bounds %zu  events %zu  characters %zu\n", d.squads.size(), d.weather.size(), d.bounds.size(), d.events.size(), d.chars.size());
	for (const auto& [id, c] : d.chars)
	{
		const Dynamic& s = c.cur;
		printf("  char %llu: %s %s head %u items %zu | tick %llu pos %.1f %.1f %.1f yaw %.2f fwd %.1f lat %.1f flags %x legs %.0f/%.0f action \"%s\" @%llu x%.2f samples %d\n",
			(unsigned long long)id, c.hasAppearance ? d.str(c.look.race).c_str() : "(no appearance)", c.look.sex ? "female" : "male", c.look.head, c.look.items.size(),
			(unsigned long long)s.tick, s.pos[0], s.pos[1], s.pos[2], s.yaw, s.fwd, s.lat, s.flags, s.legs[0], s.legs[1], d.str(s.action).c_str(), (unsigned long long)s.actionStart,
			s.actionSpeed, c.samples);
		for (const Item& it : c.look.items) { printf("    item %s spec %s section %u\n", d.str(it.item).c_str(), d.str(it.spec).c_str(), it.section); }
	}
	for (const auto& [id, m] : d.medical)
	{
		printf("  medical %llu tick %llu blood %.1f:", (unsigned long long)id, (unsigned long long)m.tick, m.blood);
		for (const Medical::Part& p : m.parts) { printf(" %s %.1f/%.1f;", d.str(p.part).c_str(), p.health, p.stun); }
		printf("\n");
	}
	for (const auto& [id, inv] : d.inventories)
	{
		printf("  inventory %llu v%u:", (unsigned long long)id, inv.version);
		for (const Inventory::Entry& e : inv.items) { printf(" %s x%u slot %u;", d.str(e.item).c_str(), e.count, e.slot); }
		printf("\n");
	}
	for (const auto& [id, s] : d.seenBy)
	{
		printf("  seen_by %llu tick %llu n %zu:", (unsigned long long)id, (unsigned long long)s.tick, s.by.size());
		for (uint64_t o : s.by) { printf(" %llu", (unsigned long long)o); }
		printf("\n");
	}
	for (const Event& e : d.events) { printf("  event tick %llu kind %u a %llu b %llu str \"%s\"\n", (unsigned long long)e.tick, e.kind, (unsigned long long)e.a, (unsigned long long)e.b, d.str(e.str).c_str()); }
	if (d.profile.valid)
	{
		const SimLinkDecoder::Profile& p = d.profile;
		printf("  profile tick %llu window %u: tick mean %.1f p50 %.1f p99 %.1f max %.1f budget %.1f us; characters %.1f squads %.1f path queries %.1f (%.1f us)\n",
			(unsigned long long)p.tick, p.window, p.tickMeanUs, p.tickP50Us, p.tickP99Us, p.tickMaxUs, p.tickBudgetUs, p.characters, p.squads, p.pathQueries, p.pathUs);
		for (const SimLinkDecoder::Profile::Row& r : p.rows) { printf("    section %s mean %.1f us max %.1f us total %llu ns\n", r.name.c_str(), r.meanUs, r.maxUs, (unsigned long long)r.totalNs); }
	}
	return 0;
}

// Synthetic server stream: the same layout the doc gives, written field by field here so a mistake in the decoder's order shows as a mismatch.
struct W
{
	std::vector<uint8_t>& out;
	size_t start;
	W(std::vector<uint8_t>& o, uint16_t type) : out(o), start(o.size()) { put<uint32_t>(0); put(type); }
	~W() { const uint32_t len = (uint32_t)(out.size() - start - 4); memcpy(out.data() + start, &len, 4); }
	template <class T> void put(T v) { const uint8_t* b = (const uint8_t*)&v; out.insert(out.end(), b, b + sizeof v); }
	void str(const std::string& s) { put((uint16_t)s.size()); out.insert(out.end(), s.begin(), s.end()); }
};

static int selftest()
{
	std::vector<uint8_t> s;
	{ W w(s, kHello); w.put(kMagic); w.put(kProtocol); w.put((uint8_t)1); w.put((uint64_t)0x1234); w.str("selftest"); }
	{ W w(s, kStrtab); w.put((uint32_t)1); w.put((uint32_t)3); w.put((uint8_t)1); w.str("Greenlander"); w.put((uint8_t)1); w.str("Katana"); w.put((uint8_t)3); w.str("walk lower"); }
	{ W w(s, kClock); w.put((uint64_t)100); w.put((uint32_t)3); w.put(10.5f); w.put(1.0f); w.put((uint8_t)0); }
	for (uint64_t t = 100; t < 102; t++)
	{
		{ W w(s, kSnapBegin); w.put(t); w.put((uint8_t)1); }
		if (t == 100)
		{
			W w(s, kAppearance);
			w.put(t); w.put((uint64_t)7); w.put((uint64_t)70); w.put((uint32_t)0); w.put((uint32_t)1); w.put((uint8_t)0); w.put((uint16_t)2);
			w.put((uint32_t)0); w.put((uint32_t)0); w.put((uint32_t)0); w.put(0.5f);
			w.put((uint8_t)0);
			w.put((uint8_t)1); w.put((uint32_t)2); w.put((uint32_t)0); w.put((uint32_t)0); w.put((uint8_t)kHip);
			w.put((uint16_t)0); w.put((uint8_t)0); w.put((uint32_t)1); w.put((uint32_t)0); w.put((uint32_t)0);	// crossbowAmmo, hiddenLimbs, version, name, portraitSerial
		}
		{
			W w(s, kDynamic);
			w.put(t); w.put((uint64_t)7); w.put(-50950.0f); w.put(10.0f); w.put(3480.0f + (float)(t - 100)); w.put(1.5f); w.put(14.0f); w.put(0.0f);
			w.put((uint32_t)(kPlayer | kYValid)); w.put((uint64_t)0); w.put(100.0f); w.put(100.0f); w.put((uint32_t)3); w.put((uint64_t)90); w.put(0.98f);
			w.put((uint8_t)0); w.put(0.0f); w.put((uint32_t)0); w.put((uint64_t)0); w.put((int16_t)0); w.put((uint32_t)1);
		}
		{ W w(s, kSnapEnd); w.put(t); w.put((uint8_t)1); }
	}
	{ W w(s, 99); w.put((uint32_t)5); }	// unknown to this client: skipped
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)77); w.put(1.0f); }	// unknown event kind: skipped
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)9); w.put((uint32_t)1); w.put((uint64_t)101); }
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)1); w.put((uint64_t)7); w.put((uint64_t)8); w.put((uint32_t)0); w.put(1.0f); w.put(2.0f); w.put(3.0f);
		w.put((uint8_t)64); for (int i = 0; i < 4; i++) { w.put(1.0f); } w.put((uint8_t)0); }
	// interactables: a door and an item; the item leaves (event 11, a pick-up), event 13 answers
	auto thing = [&](uint64_t id, uint8_t kind, uint8_t state)
	{
		W w(s, kInteractable); w.put((uint64_t)101); w.put(id); w.put(kind); w.put((uint32_t)2);
		for (int i = 0; i < 6; i++) { w.put((float)i); }
		w.put(state); w.put(100.0f); w.put(100.0f); w.put((uint16_t)3); w.put((uint32_t)4);
	};
	{ W w(s, kInventory); w.put((uint64_t)101); w.put((uint64_t)7); w.put((uint32_t)3); w.put((uint16_t)2);
		w.put((uint32_t)2); w.put((uint32_t)1); w.put((uint8_t)1); w.put((uint32_t)1); w.put((uint32_t)3); w.put((uint8_t)0); }
	{ W w(s, kItemQuality); w.put((uint64_t)101); w.put((uint64_t)7); w.put((uint32_t)3); w.put((uint16_t)2);
		w.put((uint16_t)0); w.put((uint32_t)3); w.put((uint32_t)2); w.put(0.5f); w.put((uint8_t)2);
		w.put((uint16_t)9); w.put((uint32_t)3); w.put((uint32_t)2); w.put(0.5f); w.put((uint8_t)2); }	// index 9: out of range, dropped
	{ W w(s, kMedical); w.put((uint64_t)101); w.put((uint64_t)7); w.put(3.5f); w.put((uint8_t)2);
		w.put((uint32_t)2); w.put(40.0f); w.put(1.0f); w.put((uint32_t)3); w.put(-5.0f); w.put(0.0f); }
	{ W w(s, kDetect); w.put((uint64_t)101); w.put((uint64_t)7); w.put(0.5f); w.put((uint8_t)2); w.put((uint64_t)8); }
	{ W w(s, kDetect); w.put((uint64_t)101); w.put((uint64_t)9); w.put(0.25f); w.put((uint8_t)2); w.put((uint64_t)8); }
	{ W w(s, kDetect); w.put((uint64_t)101); w.put((uint64_t)9); w.put(0.0f); w.put((uint8_t)0); w.put((uint64_t)0); }	// cleared
	// SEEN_BY (59): a set, a change of it, and n = 0 (the sneaking character's set emptied / the sneak ended)
	{ W w(s, kSeenBy); w.put((uint64_t)101); w.put((uint64_t)7); w.put((uint16_t)2); w.put((uint64_t)8); w.put((uint64_t)9); }
	{ W w(s, kSeenBy); w.put((uint64_t)102); w.put((uint64_t)7); w.put((uint16_t)1); w.put((uint64_t)9); }
	{ W w(s, kSeenBy); w.put((uint64_t)101); w.put((uint64_t)9); w.put((uint16_t)1); w.put((uint64_t)8); }
	{ W w(s, kSeenBy); w.put((uint64_t)103); w.put((uint64_t)9); w.put((uint16_t)0); }
	{ W w(s, kSeenBy); w.put((uint64_t)101); w.put((uint64_t)8); w.put((uint16_t)1); w.put((uint64_t)9); }	// forgotten by the event 11 at the end
	// CHAR_STATS: 7 carries the AI tail (rows 16-20), 9 is from an older server (it stops at row 15)
	auto stats = [&](uint64_t id, bool tail)
	{
		W w(s, kCharStats);
		w.put((uint64_t)101); w.put(id); w.put((uint8_t)0); w.put((uint8_t)0); w.put((uint32_t)0); w.put(0.0f);
		w.put((uint8_t)0); w.put(0.0f); w.put((uint8_t)0); w.put((uint8_t)1); w.put((uint8_t)CharStats::kStrength); w.put(10.0f); w.put(0.0f);
		for (int i = 0; i < 8; i++) { w.put(0.0f); }	// athleticsXp .. light
		w.put((uint8_t)0); w.put(0.0f);	// bounties, sentenceHours
		if (!tail) { return; }
		w.put((uint16_t)1); w.put((uint8_t)5); w.put((uint16_t)0xFFFF); w.put((uint8_t)0); w.put((uint8_t)0);	// goalEnum, source, stepEnum, stepIndex, stepCount
		w.put((uint64_t)8); w.put(-50600.0f); w.put(-900.0f);	// targetId, targetX, targetZ
	};
	stats(7, true);
	stats(9, false);
	thing(30, 1, 1);
	thing(31, 2, 0);
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)11); w.put((uint64_t)31); }
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)13); w.put((uint64_t)7); w.put((uint64_t)30); w.put((uint8_t)1); w.put((uint8_t)0); }
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)14); w.put((uint64_t)7); w.put((uint64_t)32); w.put((uint32_t)2); }
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)15); w.put((uint64_t)8); w.put((uint64_t)7); }
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)49); w.put((uint64_t)8); w.put((uint64_t)7); for (int i = 0; i < 6; i++) { w.put((float)i); }
		w.put((uint8_t)2); w.put((uint32_t)2); w.put((uint64_t)0x1234); }	// shot: missed, weapon strid 2, from a turret
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)50); w.put((uint64_t)7); w.put((uint64_t)8); w.put((uint8_t)1); w.put(42.5f); }	// stealth_ko: a failed roll at 42.5%
	{ W w(s, kEvent); w.put((uint64_t)101); w.put((uint16_t)11); w.put((uint64_t)8); }	// character 8 leaves the view: its SEEN_BY entry goes (event 11)
	// navmesh tile stream: params, tiles (a repeat and another bake's tile ignored), a door patch (a stale one dropped), a removal
	auto navTile = [&](uint32_t mesh, int32_t tx, uint32_t version, uint32_t size)
	{
		W w(s, kNavTile); w.put(mesh); w.put(tx); w.put((int32_t)2); w.put((int32_t)0); w.put(version); w.put(size);
		for (uint32_t i = 0; i < size; i++) { w.put((uint8_t)(i + version)); }
	};
	auto navPatch = [&](int32_t tx, uint32_t version)
	{
		W w(s, kNavPolyFlags); w.put((uint32_t)5); w.put(tx); w.put((int32_t)2); w.put((int32_t)0); w.put(version); w.put((uint16_t)1);
		w.put((uint16_t)3); w.put((uint16_t)0x10); w.put((uint8_t)4);
	};
	{ W w(s, kNavParams); w.put((uint32_t)5); w.put((uint32_t)7); w.put(1.0f); w.put(2.0f); w.put(3.0f); w.put(64.0f); w.put(64.0f); w.put((uint32_t)4096); w.put((uint32_t)2048); }
	navTile(5, 1, 1, 3);
	navTile(5, 1, 1, 5);	// same version: ignored
	navTile(6, 9, 1, 3);	// another bake: dropped
	navTile(5, 4, 1, 2);
	navTile(5, 4, 2, 0);	// removed
	navPatch(1, 2);
	navPatch(1, 2);	// stale: dropped
	// PROFILE (70): the answer to order 120 from a KSIM_PROFILE server (SIMLINK.md "PROFILE", kenshi-k16.2.148); a trailing byte a newer server appends
	{ W w(s, kProfile); w.put((uint64_t)102); w.put((uint32_t)4096);
		w.put(2500.0f); w.put(2000.0f); w.put(9000.0f); w.put(15000.0f); w.put(50000.0f);
		w.put(120.0f); w.put(12.0f); w.put(3.5f); w.put(0.8f);
		w.put((uint16_t)2);
		w.str("setup"); w.put(5.5f); w.put(140.5f); w.put((uint64_t)1234567);
		w.str("combat"); w.put(1500.0f); w.put(8000.0f); w.put((uint64_t)987654321);
		w.put((uint8_t)0); }

	// feed in awkward chunk sizes so frames split across calls
	SimLinkDecoder d;
	for (size_t at = 0, k = 1; at < s.size(); at += k, k = k % 7 + 1)
	{
		if (!d.feed(s.data() + at, std::min(k, s.size() - at))) { fprintf(stderr, "selftest: decode error: %s\n", d.error().c_str()); return 1; }
	}
	dump(d);
	int fails = 0;
	auto check = [&](bool ok, const char* what) { if (!ok) { fprintf(stderr, "selftest FAIL: %s\n", what); fails++; } };
	const auto it = d.chars.find(7);
	check(it != d.chars.end(), "character 7 present");
	if (it != d.chars.end())
	{
		const Character& c = it->second;
		check(d.str(c.look.race) == "Greenlander", "race string");
		check(c.look.head == 2 && c.look.items.size() == 1 && c.look.items[0].section == kHip && d.str(c.look.items[0].item) == "Katana", "appearance fields");
		check(c.samples == 2 && c.prev.tick == 100 && c.cur.tick == 101, "two dynamic samples");
		check(std::fabs(c.cur.pos[2] - 3481.0f) < 1e-3f && c.cur.flags == (kPlayer | kYValid) && d.str(c.cur.action) == "walk lower", "dynamic fields");
	}
	check(d.lastCompleteTick == 101 && d.clock.valid && d.clock.day == 3, "snapshot / clock");
	check(d.events.size() == 9 && d.events[0].kind == 9 && d.events[1].kind == 1 && d.events[1].b == 8 && d.events[1].dir == 64, "events");
	check(d.events.size() == 9 && d.events[4].kind == 14 && d.events[4].a == 7 && d.events[4].b == 32 && d.str(d.events[4].str) == "Katana", "theft event");
	check(d.events.size() == 9 && d.events[3].kind == 13 && d.events[3].a == 7 && d.events[3].b == 30 && d.events[3].dir == 1 && d.events[3].result == 0, "interact done event");
	const auto door = d.things.find(30);
	check(d.things.size() == 1 && door != d.things.end() && door->second.kind == 1 && door->second.state == 1 && door->second.use[2] == 5.0f && door->second.count == 3
		&& door->second.version == 4, "interactables");
	const auto inv = d.inventories.find(7);
	check(inv != d.inventories.end() && inv->second.version == 3 && inv->second.items.size() == 2 && inv->second.items[0].slot == 1
		&& inv->second.items[1].count == 3 && d.str(inv->second.items[0].item) == "Katana", "inventory");
	check(inv != d.inventories.end() && inv->second.items[0].hasQuality && inv->second.items[0].grade == 2 && inv->second.items[0].level == 0.5f
		&& d.str(inv->second.items[0].model) == "Katana" && !inv->second.items[1].hasQuality, "item quality");
	const auto med = d.medical.find(7);
	check(med != d.medical.end() && med->second.tick == 101 && med->second.blood == 3.5f && med->second.parts.size() == 2 && med->second.parts[1].health == -5.0f
		&& d.str(med->second.parts[0].part) == "Katana", "medical");
	check(d.events.size() == 9 && d.events[5].kind == 15 && d.events[5].a == 8 && d.events[5].b == 7, "noticed event");
	check(d.events.size() == 9 && d.events[6].kind == 49 && d.events[6].a == 8 && d.events[6].b == 7 && d.events[6].pos[2] == 2.0f && d.events[6].to[0] == 3.0f
		&& d.events[6].to[2] == 5.0f && d.events[6].result == 2 && d.events[6].str == 2 && d.events[6].c == 0x1234, "shot event");
	check(d.events.size() == 9 && d.events[7].kind == 50 && d.events[7].a == 7 && d.events[7].b == 8 && d.events[7].result == 1
		&& std::fabs(d.events[7].amount - 42.5f) < 1e-6f, "stealth_ko event");
	// SEEN_BY: 7's set changed to {9}; 9's set emptied (n = 0); 8's entry went with event 11 (character 8 leaves the view)
	const auto sb7 = d.seenBy.find(7), sb9 = d.seenBy.find(9);
	check(d.seenBy.size() == 1 && sb7 != d.seenBy.end() && sb7->second.tick == 102 && sb7->second.by.size() == 1 && sb7->second.by[0] == 9, "seen_by set and change");
	check(sb9 == d.seenBy.end() && d.seenBy.count(8) == 0 && d.events.size() == 9 && d.events[8].kind == 11 && d.events[8].a == 8, "seen_by n = 0 and event 11");
	const auto det = d.detect.find(7);
	check(d.detect.size() == 1 && det != d.detect.end() && det->second.meter == 0.5f && det->second.state == Detect::kNoticing && det->second.by == 8, "detect");
	const auto stats7 = d.charStats.find(7), stats9 = d.charStats.find(9);
	check(stats7 != d.charStats.end() && stats7->second.hasAi && stats7->second.goalEnum == 1 && stats7->second.source == 5
		&& stats7->second.stepEnum == CharStats::kNoTaskValue && stats7->second.stepCount == 0 && stats7->second.targetId == 8
		&& stats7->second.targetX == -50600.0f && stats7->second.targetZ == -900.0f && stats7->second.skills.size() == 1
		&& stats7->second.skills[0].value == 10.0f, "char stats AI tail");
	check(stats9 != d.charStats.end() && !stats9->second.hasAi && stats9->second.goalEnum == CharStats::kNoTaskValue, "char stats without the tail");
	const auto nt = d.navTiles.find({ 1, 2, 0 });
	check(d.nav.valid && d.nav.meshId == 5 && d.nav.detourVersion == 7 && d.nav.tileWidth == 64.0f && d.nav.maxPolys == 2048, "nav params");
	check(d.navTiles.size() == 1 && nt != d.navTiles.end() && nt->second.data.size() == 3 && nt->second.data[2] == 3 && nt->second.version == 2, "nav tiles");
	check(d.navPatches.size() == 1 && d.navPatches[0].polys.size() == 1 && d.navPatches[0].polys[0].poly == 3 && d.navPatches[0].polys[0].area == 4, "nav patch");
	check(d.profile.valid && d.profileSerial == 1 && d.profile.tick == 102 && d.profile.window == 4096 && d.profile.tickMeanUs == 2500.0f
		&& d.profile.tickP50Us == 2000.0f && d.profile.tickP99Us == 9000.0f && d.profile.tickMaxUs == 15000.0f && d.profile.tickBudgetUs == 50000.0f
		&& d.profile.characters == 120.0f && d.profile.squads == 12.0f && d.profile.pathQueries == 3.5f && d.profile.pathUs == 0.8f, "profile summary");
	check(d.profile.rows.size() == 2 && d.profile.rows[0].name == "setup" && d.profile.rows[0].meanUs == 5.5f && d.profile.rows[0].maxUs == 140.5f
		&& d.profile.rows[0].totalNs == 1234567 && d.profile.rows[1].name == "combat" && d.profile.rows[1].meanUs == 1500.0f
		&& d.profile.rows[1].maxUs == 8000.0f && d.profile.rows[1].totalNs == 987654321, "profile rows");
	std::vector<uint8_t> o;
	OrderWriter ow;
	OrderWriter::hello(o, 0x1234, "kclient");
	check(ow.moveTo(o, { 7 }, 1.0f, 2.0f, false) == 1 && ow.setView(o, 3.0f, 4.0f) == 2, "order sequence numbers");
	check(ow.drop(o, 7, "Bread", 2) == 3, "drop sequence number");
	check(ow.equip(o, 7, "Katana", true) == 4 && ow.transfer(o, 7, 32, "Bread", 2, true) == 5, "equip / transfer sequence numbers");
	// HELLO 4+2+4+2+1+8+2+7 = 30 bytes, move_to 4+2+4+2+2+8+4+4+4+1 = 35, set_view 4+2+4+2+4+4 = 20, drop 4+2+4+2+8+2+5+2 = 29,
	// equip 4+2+4+2+8+2+6+1 = 29, transfer 4+2+4+2+8+8+2+5+2+1 = 38
	check(o.size() == 30 + 35 + 20 + 29 + 29 + 38, "order frame sizes");
	check(ow.profileQuery(o) == 6 && o.size() == 30 + 35 + 20 + 29 + 29 + 38 + 12, "profile query order");
	printf("selftest %s\n", fails ? "FAILED" : "ok");
	return fails ? 1 : 0;
}

int main(int argc, char** argv)
{
	if (argc == 2 && !strcmp(argv[1], "--selftest")) { return selftest(); }
	if (argc != 2) { fprintf(stderr, "usage: ksimlink_dump FILE.bin | --selftest\n"); return 2; }
	FILE* f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	SimLinkDecoder d;
	uint8_t buf[65536];
	size_t n;
	bool ok = true;
	while (ok && (n = fread(buf, 1, sizeof buf, f)) > 0) { ok = d.feed(buf, n); }
	fclose(f);
	dump(d);
	if (!ok) { fprintf(stderr, "decode error: %s\n", d.error().c_str()); return 1; }
	return 0;
}
