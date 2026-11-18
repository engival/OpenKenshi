#include "debug/debugview.hpp"

#include "ksim/debugview/enums.hpp"
#include "simlink_io.hpp"

namespace dbg
{

namespace dv = ksim::debugview;

void DebugView::frame(uint16_t type, const uint8_t* p, size_t n)
{
	simlink::io::Reader r{ p, n };
	if (type == dv::kDebugInfo)
	{
		version = r.u8();
		enabled = r.u8() == dv::kEnabled && version == dv::kDebugVersion;
		infoSeen = !r.bad;
		infoSerial++;
		if (!enabled) { squads.clear(); }
	}
	else if (type == dv::kDebugSquads)
	{
		const uint64_t tick = r.u64();
		std::unordered_map<uint64_t, Squad> next;
		for (uint32_t i = 0, count = r.u32(); i < count && !r.bad; i++)
		{
			const uint64_t id = r.u64();
			Squad& s = next[id];
			s.templ = r.u32();
			s.package = r.u32();
			s.goalKind = r.u8();
			s.fallbackReason = r.u8();
			s.orderStatus = r.u8();
			s.detail = r.u8();
			s.members = r.u8();
			s.effective = r.u8();
			s.dead = r.u8();
			s.intact = r.u8();
			s.targetKind = r.u8();
			s.tx = r.f32();
			s.tz = r.f32();
			s.targetId = r.u64();
			s.townSid = r.u32();
			s.routeValid = r.u8() != 0;
			s.along = r.f32();
			s.length = r.f32();
			s.memberIds.resize(r.u8());
			for (uint64_t& m : s.memberIds) { m = r.u64(); }
		}
		if (r.bad) { if (error.empty()) { error = "DEBUG_SQUADS too short (" + std::to_string(n) + " bytes)"; } return; }
		squads = std::move(next);
		squadsTick = tick;
		squadsSerial++;
	}
	else if (type == dv::kDebugTree)
	{
		Tree t;
		t.tick = r.u64();
		t.kind = r.u8();
		t.id = r.u64();
		t.rows.resize(r.u16());
		for (TreeRow& row : t.rows)
		{
			row.depth = r.u16();
			row.key = r.str();
			row.value = r.str();
			if (r.bad) { break; }
		}
		if (r.bad) { if (error.empty()) { error = "DEBUG_TREE too short (" + std::to_string(n) + " bytes)"; } return; }
		tree = std::move(t);
		treeSerial++;
	}
	else if (type == dv::kDebugSpawned)
	{
		Reply a;
		a.frame = dv::kDebugSpawnSquad;
		a.seq = r.u32();
		a.squad = r.u64();
		a.reason = r.str();
		a.accepted = a.squad != 0;
		if (r.bad) { if (error.empty()) { error = "DEBUG_SPAWNED too short (" + std::to_string(n) + " bytes)"; } return; }
		reply = std::move(a);
		replySerial++;
	}
	else if (type == dv::kDebugAck)
	{
		Reply a;
		a.seq = r.u32();
		a.frame = r.u16();
		a.accepted = r.u8() != 0;
		a.reason = r.str();
		if (r.bad) { if (error.empty()) { error = "DEBUG_ACK too short (" + std::to_string(n) + " bytes)"; } return; }
		reply = std::move(a);
		replySerial++;
	}
}

std::vector<uint8_t> DebugView::subscribe(bool on)
{
	std::vector<uint8_t> out;
	{
		simlink::io::Writer w(out, dv::kDebugSubscribe);	// its destructor writes the frame length
		w.put((uint8_t)on);
	}
	return out;
}

std::vector<uint8_t> DebugView::query(uint8_t kind, uint64_t id)
{
	std::vector<uint8_t> out;
	{
		simlink::io::Writer w(out, dv::kDebugQuery);
		w.put(kind);
		w.put(id);
	}
	return out;
}

std::vector<uint8_t> DebugView::spawnSquad(uint32_t seq, const std::string& templ, float sx, float sz, float tx, float tz)
{
	std::vector<uint8_t> out;
	{
		simlink::io::Writer w(out, dv::kDebugSpawnSquad);
		w.put(seq);
		w.str(templ);
		for (float v : { sx, sz, tx, tz }) { w.put(v); }
	}
	return out;
}

std::vector<uint8_t> DebugView::setWeather(uint32_t seq, const std::string& sid, const std::string& group, float strength)
{
	std::vector<uint8_t> out;
	{
		simlink::io::Writer w(out, dv::kDebugSetWeather);
		w.put(seq);
		w.str(sid);
		w.str(group);
		w.put(strength);
	}
	return out;
}

std::vector<uint8_t> DebugView::setTime(uint32_t seq, float hour, uint8_t extraDays)
{
	std::vector<uint8_t> out;
	{
		simlink::io::Writer w(out, dv::kDebugSetTime);
		w.put(seq);
		w.put(hour);
		w.put(extraDays);
	}
	return out;
}

std::vector<uint8_t> DebugView::cheat(uint32_t seq, uint64_t character, uint8_t op)
{
	std::vector<uint8_t> out;
	{
		simlink::io::Writer w(out, dv::kDebugCheat);
		w.put(seq);
		w.put(character);
		w.put(op);
	}
	return out;
}

void DebugView::reset()
{
	infoSeen = enabled = false;
	version = 0;
	squadsTick = 0;
	squads.clear();
	tree = Tree();
	error.clear();
	squadsSerial++;	// the serials only grow: a watcher sees the change
	treeSerial++;
}

void goalColour(uint8_t goalKind, float rgb[3])
{
	static const float kColours[][3] = {	// MINE: by GoalKind value; past the table grey
		{ 0.55f, 0.55f, 0.55f },	// none
		{ 0.35f, 0.55f, 1.00f },	// idle
		{ 0.35f, 0.90f, 0.95f },	// go home
		{ 0.30f, 0.90f, 0.30f },	// travel to
		{ 1.00f, 0.85f, 0.20f },	// patrol
		{ 0.85f, 0.45f, 1.00f },	// roam
		{ 1.00f, 0.25f, 0.25f },	// raid
		{ 1.00f, 0.55f, 0.15f },	// man gate
	};
	const float* c = goalKind < sizeof kColours / sizeof kColours[0] ? kColours[goalKind] : kColours[0];
	for (int k = 0; k < 3; k++) { rgb[k] = c[k]; }
}

} // namespace dbg
