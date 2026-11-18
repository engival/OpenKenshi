// Save / load over SIMLINK (docs/formats/SIMLINK.md SAVES, orders 80-83, frame 32; docs/formats/RUNTIME_SAVE.md v36 metadata
// block): through in-process Sessions (no socket) with a save directory under the build dir, never $HOME. Frame 32 is decoded
// by hand from the layout in SIMLINK.md (client/src/simlink.* has no decoder for it).
//   save -> list (fields, newest first), overwrite, bad names (each rule), non-admin refusal, delete (and a missing one),
//   load -> full resend with nothing stale in the session's caches + the state hash equal to the saved one + the same
//   continuation as the uninterrupted run, corrupt / truncated / old files (status 2, bad_value, world untouched), SAVES unasked
//   to the other admin session after save, load and delete, the order log never sees 80-83, and the save directory rules.
// argv[1]: the Hub navmesh cache (`knav_bake`, ctest fixture knav_cache).

#include "ksim/core/clock.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/save/save.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/net/simlink_saves.hpp"
#include "ksim/net/simlink_scenario.hpp"
#include "ksim/core/simulation.hpp"

#include "kworld/gamedata.hpp"
#include "kworld/loadorder.hpp"

#include "test_util.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif
#ifndef KSIM_BINARY_DIR
#error KSIM_BINARY_DIR must be defined
#endif

namespace {

using namespace ksim;
namespace ks = ksim::simlink;
namespace fs = std::filesystem;

// ---- wire helpers ----

struct Buf
{
	std::vector<std::uint8_t> bytes;
	template <typename T>
	Buf& put(T v)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
		bytes.insert(bytes.end(), p, p + sizeof(T));
		return *this;
	}
	Buf& str(const std::string& s)
	{
		put<std::uint16_t>(static_cast<std::uint16_t>(s.size()));
		bytes.insert(bytes.end(), s.begin(), s.end());
		return *this;
	}
};

struct Frame
{
	std::uint16_t type = 0;
	std::vector<std::uint8_t> payload;
};

std::vector<Frame> frames(const std::vector<std::uint8_t>& bytes)
{
	std::vector<Frame> out;
	for (std::size_t at = 0; at < bytes.size();)
	{
		std::uint32_t len = 0;
		std::memcpy(&len, &bytes[at], 4);
		Frame frame;
		std::memcpy(&frame.type, &bytes[at + 4], 2);
		frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 6), bytes.begin() + static_cast<std::ptrdiff_t>(at + 4 + len));
		out.push_back(std::move(frame));
		at += 4 + len;
	}
	return out;
}

struct Cursor
{
	const std::vector<std::uint8_t>& p;
	std::size_t at = 0;
	template <typename T>
	T get()
	{
		test::require(at + sizeof(T) <= p.size(), "frame overrun");
		T v;
		std::memcpy(&v, &p[at], sizeof(T));
		at += sizeof(T);
		return v;
	}
	std::string str()
	{
		const std::uint16_t n = get<std::uint16_t>();
		test::require(at + n <= p.size(), "frame overrun");
		std::string s(p.begin() + static_cast<std::ptrdiff_t>(at), p.begin() + static_cast<std::ptrdiff_t>(at + n));
		at += n;
		return s;
	}
};

// SAVES (type 32), decoded from the layout in SIMLINK.md: u64 tick, u16 n, n x {str name, i64 mtime, u32 saveVersion, u8 status,
// u32 day, f32 hour, str factionName, str leaderName, u16 members, i64 money}.
struct Wire32
{
	struct Entry
	{
		std::string name;
		std::int64_t mtime = 0;
		std::uint32_t version = 0;
		std::uint8_t status = 0;
		std::uint32_t day = 0;
		float hour = 0;
		std::string faction;
		std::string leader;
		std::uint16_t members = 0;
		std::int64_t money = 0;
	};
	std::uint64_t tick = 0;
	std::vector<Entry> entries;

	static Wire32 decode(const std::vector<std::uint8_t>& payload)
	{
		Cursor c{payload};
		Wire32 w;
		w.tick = c.get<std::uint64_t>();
		const std::uint16_t n = c.get<std::uint16_t>();
		for (std::uint16_t i = 0; i < n; ++i)
		{
			Entry e;
			e.name = c.str();
			e.mtime = c.get<std::int64_t>();
			e.version = c.get<std::uint32_t>();
			e.status = c.get<std::uint8_t>();
			e.day = c.get<std::uint32_t>();
			e.hour = c.get<float>();
			e.faction = c.str();
			e.leader = c.str();
			e.members = c.get<std::uint16_t>();
			e.money = c.get<std::int64_t>();
			w.entries.push_back(std::move(e));
		}
		test::require(c.at == payload.size(), "frame 32 has trailing bytes");
		return w;
	}
	const Entry* find(const std::string& name) const
	{
		for (const Entry& e : entries)
		{
			if (e.name == name)
				return &e;
		}
		return nullptr;
	}
};

// What one call put on the wire, sorted into what the tests look at.
struct Output
{
	std::vector<Frame> all;
	std::vector<Wire32> saves;
	std::string reason; // the rejection reason ("" = applied)
	bool answered = false;
	int count(std::uint16_t type) const
	{
		int n = 0;
		for (const Frame& f : all)
			n += f.type == type;
		return n;
	}
};

struct Client
{
	std::unique_ptr<ks::Session> session;

	Client(Simulation& sim, ks::ServerContext& context, bool admin, std::uint64_t dataHash)
		: session(std::make_unique<ks::Session>(sim, context))
	{
		session->setAdmin(admin);
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->begin(w);
		Buf hello;
		hello.put<std::uint32_t>(ks::kMagic).put<std::uint16_t>(ks::kProtocol).put<std::uint8_t>(0).put<std::uint64_t>(dataHash).str("test");
		test::require(session->onFrame(ks::kHello, hello.bytes.data(), hello.bytes.size(), w), "hello refused");
		session->sendTick(w, true);
	}

	Output digest(const std::vector<std::uint8_t>& bytes, std::uint32_t seq) const
	{
		Output out;
		out.all = frames(bytes);
		for (const Frame& frame : out.all)
		{
			if (frame.type == ks::kSaves)
				out.saves.push_back(Wire32::decode(frame.payload));
			if (frame.type != ks::kEvent || seq == 0)
				continue;
			Cursor c{frame.payload};
			c.get<std::uint64_t>();
			const std::uint16_t kind = c.get<std::uint16_t>();
			if ((kind == 9 || kind == 10) && c.get<std::uint32_t>() == seq)
			{
				out.answered = true;
				if (kind == 10)
					out.reason = session->strings().text(c.get<std::uint32_t>());
			}
		}
		return out;
	}

	std::uint32_t seq = 0;

	// Sends one order; the answer and everything the server wrote with it.
	Output order(std::uint16_t kind, const Buf& body)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		test::require(session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w), "order closed the session");
		Output out = digest(bytes, seq);
		test::require(out.answered, "order " + std::to_string(kind) + " got no answer");
		return out;
	}

	// A malformed order frame: the session must refuse (onFrame false).
	bool refused(std::uint16_t kind, const Buf& body)
	{
		Buf payload;
		payload.put<std::uint32_t>(++seq).put<std::uint16_t>(kind);
		payload.bytes.insert(payload.bytes.end(), body.bytes.begin(), body.bytes.end());
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		return !session->onFrame(ks::kOrder, payload.bytes.data(), payload.bytes.size(), w);
	}

	Output tick(Simulation& sim)
	{
		sim.run(1);
		return sendTick(false);
	}

	Output sendTick(bool initial)
	{
		std::vector<std::uint8_t> bytes;
		ks::BinaryWriter w(bytes);
		session->sendTick(w, initial);
		return digest(bytes, 0);
	}

	Output save(const std::string& name) { return order(80, Buf{}.str(name)); }
	Output load(const std::string& name) { return order(81, Buf{}.str(name).put<std::uint8_t>(0)); }
	Output remove(const std::string& name) { return order(82, Buf{}.str(name)); }
	Output list() { return order(83, Buf{}); }
};

// ---- helpers ----

std::string readAll(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void writeAll(const std::string& path, const std::string& bytes)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << bytes;
}

std::size_t dirCount(const std::string& dir)
{
	std::size_t n = 0;
	for (const auto& entry : fs::directory_iterator(dir))
	{
		(void)entry;
		++n;
	}
	return n;
}

void requireReason(const Output& out, const std::string& want, const std::string& what)
{
	test::require(out.reason == want, what + ": answered '" + out.reason + "', wanted '" + want + "'");
}

// The metadata block the sim should have written, from the live world.
void requireMatchesSim(const Wire32::Entry& e, Simulation& sim, std::uint64_t tick, const std::string& what)
{
	const GameTime time = gameTimeAt(tick);
	const PlayerSquadsView view = sim.playerSquads();
	test::require(e.status == 0, what + ": status " + std::to_string(e.status));
	test::require(e.version == kSaveVersion, what + ": saveVersion");
	test::require(e.day == time.day, what + ": day " + std::to_string(e.day) + " vs " + std::to_string(time.day));
	const float hour = static_cast<float>(std::fmod(static_cast<double>(tick) / kTicksPerGameMinute, 1440.0) / 60.0);
	test::require(e.hour == hour, what + ": hour");
	test::require(e.faction == view.factionName && e.faction == "Nameless", what + ": faction name '" + e.faction + "'");
	const std::uint64_t leader = view.squads.front().members.front();
	test::require(e.leader == sim.combat()->find(leader)->name && !e.leader.empty(), what + ": leader name '" + e.leader + "'");
	test::require(e.members == view.factionCount && e.members == 2, what + ": members " + std::to_string(e.members));
	test::require(e.money == 0, what + ": money (not modelled: 0)");
}

std::string utf8(std::uint32_t cp)
{
	std::string s;
	if (cp < 0x80)
		s += static_cast<char>(cp);
	else if (cp < 0x800)
	{
		s += static_cast<char>(0xC0 | (cp >> 6));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		s += static_cast<char>(0xE0 | (cp >> 12));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	} else {
		s += static_cast<char>(0xF0 | (cp >> 18));
		s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		s += static_cast<char>(0x80 | (cp & 0x3F));
	}
	return s;
}

std::string repeat(const std::string& s, int n)
{
	std::string out;
	for (int i = 0; i < n; ++i)
		out += s;
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		test::require(argc == 2, "usage: ksim_saves HUB.knav");
		const kworld::LoadOrder order = kworld::vanillaLoadOrder(KENSHI_DATA);
		const auto gd = std::make_shared<const kdata::GameData>(kworld::loadMergedGameData(order));
		const std::shared_ptr<const CombatData> data = CombatData::build(*gd, KENSHI_DATA);
		const std::shared_ptr<MeshNavigator> nav = MeshNavigator::load(argv[1]);

		// The save directory lives under the build dir.
		const std::string base = std::string(KSIM_BINARY_DIR) + "/ksim_saves_test";
		fs::remove_all(std::string(KSIM_BINARY_DIR) + "/ksim_saves_test");
		const std::string dir = ks::prepareSaveDir(base + "/saves");
		test::require(dir == fs::weakly_canonical(base + "/saves").string() && fs::is_directory(dir), "prepareSaveDir made the directory");

		SpawnOptions spawn;
		spawn.seed = 7;
		Simulation sim = Simulation::newGame(KENSHI_DATA, order, spawn);
		ks::ServerContext context;
		context.gameData = gd;
		context.dataHash = ks::dataHash(order);
		context.saveDir = dir;
		CombatSettings settings;
		settings.substeps = 2;
		sim.enableCombat(data, settings);
		sim.enableNavigation(nav);
		const ks::Squad squad = ks::spawnSquad(sim, *gd, *data, context);
		sim.run(5);

		Client admin(sim, context, true, context.dataHash);
		Client other(sim, context, false, context.dataHash); // not an admin session
		Client watcher(sim, context, true, context.dataHash); // a second admin session: gets SAVES unasked
		admin.sendTick(false);

		// ---- list on an empty directory ----
		{
			const Output out = admin.list();
			requireReason(out, "", "list");
			test::require(out.saves.size() == 1 && out.saves[0].entries.empty() && out.saves[0].tick == sim.clockTick(), "an empty directory lists nothing");
		}

		// ---- save -> list: the fields, the file, SAVES unasked ----
		const std::uint64_t tickAlpha = sim.clockTick();
		{
			const Output out = admin.save("alpha");
			requireReason(out, "", "save alpha");
			test::require(out.saves.size() == 1 && out.saves[0].entries.size() == 1, "SAVES follows a save to the saving admin session");
			requireMatchesSim(*out.saves[0].find("alpha"), sim, tickAlpha, "alpha");
			test::require(fs::is_regular_file(dir + "/alpha") && dirCount(dir) == 1, "the file is exactly the name; no temp file is left");
			std::ostringstream direct;
			sim.save(direct);
			test::require(readAll(dir + "/alpha") == direct.str(), "the file is the runtime save");
			// Another admin session hears of it at its next tick; a non-admin one does not.
			const Output w = watcher.tick(sim);
			test::require(w.saves.size() == 1 && w.saves[0].find("alpha") != nullptr, "SAVES unasked to another admin session after a save");
			test::require(other.sendTick(false).saves.empty(), "no SAVES to a non-admin session");
			test::require(watcher.sendTick(false).saves.empty(), "SAVES is sent once, not every tick");
		}

		// ---- a second save later; newest first ----
		sim.run(2500); // past the next game hour: the metadata differs
		const std::uint64_t tickBeta = sim.clockTick();
		fs::last_write_time(dir + "/alpha", fs::file_time_type::clock::now() - std::chrono::hours(2));
		{
			const Output out = admin.save("beta");
			requireReason(out, "", "save beta");
			const Wire32& s = out.saves.at(0);
			test::require(s.entries.size() == 2 && s.entries[0].name == "beta" && s.entries[1].name == "alpha", "newest first");
			test::require(s.entries[0].mtime > s.entries[1].mtime, "mtimes order the list");
			requireMatchesSim(s.entries[0], sim, tickBeta, "beta");
			test::require(s.entries[0].hour != s.entries[1].hour || s.entries[0].day != s.entries[1].day, "the two saves differ in time");
			const Output listed = admin.list();
			test::require(listed.saves.size() == 1 && listed.saves[0].entries.size() == 2 && listed.saves[0].entries[0].name == "beta", "order 83 lists the same");
		}

		// ---- overwrite an existing name ----
		sim.run(100);
		{
			const std::uint64_t tickNow = sim.clockTick();
			const std::string before = readAll(dir + "/alpha");
			const Output out = admin.save("alpha");
			requireReason(out, "", "overwrite alpha");
			test::require(out.saves.at(0).entries.size() == 2 && out.saves[0].entries[0].name == "alpha", "the overwritten save is the newest again");
			requireMatchesSim(*out.saves[0].find("alpha"), sim, tickNow, "alpha overwritten");
			test::require(readAll(dir + "/alpha") != before && dirCount(dir) == 2, "overwritten in place, no temp file left");
			(void)tickAlpha;
		}

		// ---- bad names, each rule ----
		{
			const std::size_t files = dirCount(dir);
			const std::vector<std::pair<std::string, std::string>> bad = {
				{"", "empty"},
				{repeat("a", 65), "65 characters"},
				{repeat(utf8(0xE9), 65), "65 two-byte characters"},
				{repeat(utf8(0x10348), 64), "64 four-byte characters (256 bytes, past NAME_MAX)"},
				{"a\xC3\x28", "invalid UTF-8 (bad continuation)"},
				{"\xC0\x80", "overlong NUL"},
				{"\xED\xA0\x80", "a surrogate"},
				{"\xF4\x90\x80\x80", "past U+10FFFF"},
				{"\xFF", "a stray byte"},
				{"\xE2\x82", "truncated sequence"},
				{std::string("a\0b", 3), "NUL"},
				{"a\tb", "tab"},
				{"a\nb", "newline"},
				{"a\x7F" "b", "DEL"},
				{"a" + utf8(0x85) + "b", "a C1 control"},
				{"a/b", "slash"},
				{"/etc/passwd", "absolute path"},
				{"..", "dot dot"},
				{"../x", "traversal"},
				{"a\\b", "backslash"},
				{"a:b", "colon"},
				{".hidden", "leading dot"},
				{".", "dot"},
			};
			for (const auto& [name, what] : bad)
			{
				requireReason(admin.save(name), "bad_value", "save with " + what);
				requireReason(admin.remove(name), "bad_value", "delete with " + what);
				requireReason(admin.load(name), "bad_value", "load with " + what);
			}
			test::require(dirCount(dir) == files, "a bad name wrote or removed nothing");
			// The edges that are fine.
			const std::vector<std::string> good = {repeat("a", 64), repeat(utf8(0xE9), 64), "Sauvegarde " + utf8(0x30C6) + utf8(0x1F600), "a b", "a.b", "x" + std::string(1, '.'), "-dash", "a;b"};
			for (const std::string& name : good)
			{
				requireReason(admin.save(name), "", "save '" + name + "'");
				test::require(fs::is_regular_file(dir + "/" + name), "the file is named exactly '" + name + "'");
				requireReason(admin.remove(name), "", "delete '" + name + "'");
			}
			test::require(dirCount(dir) == files, "the good edge names were cleaned up");
		}

		// ---- not an admin session ----
		{
			const std::size_t files = dirCount(dir);
			const std::string beforeAlpha = readAll(dir + "/alpha");
			for (const std::uint16_t kind : {80, 81, 82, 83})
			{
				Buf body;
				if (kind != 83)
					body.str(kind == 80 ? "evil" : "alpha");
				if (kind == 81)
					body.put<std::uint8_t>(0);
				const Output out = other.order(kind, body);
				requireReason(out, "not_allowed", "non-admin order " + std::to_string(kind));
				test::require(out.saves.empty(), "a refused order sends no SAVES");
			}
			// not_allowed comes before the name check: a bad name from a non-admin leaks nothing.
			requireReason(other.save(".."), "not_allowed", "non-admin with a bad name");
			test::require(dirCount(dir) == files && readAll(dir + "/alpha") == beforeAlpha && fs::exists(dir + "/alpha"), "a non-admin session changed nothing");
		}

		// ---- malformed frames are protocol errors like any other order ----
		test::require(admin.refused(83, Buf{}.put<std::uint8_t>(1)), "list_saves with a body is malformed");
		test::require(admin.refused(80, Buf{}.put<std::uint16_t>(5)), "save_game with a short name is malformed");
		test::require(admin.refused(81, Buf{}.str("alpha")), "load_game without resetPositions is malformed");
		// (that closed `admin`'s session in the protocol sense; a fresh one carries on)
		Client boss(sim, context, true, context.dataHash);

		// ---- delete, and a missing one ----
		{
			const Output out = boss.remove("beta");
			requireReason(out, "", "delete beta");
			test::require(out.saves.size() == 1 && out.saves[0].entries.size() == 1 && out.saves[0].entries[0].name == "alpha", "SAVES follows a delete");
			test::require(!fs::exists(dir + "/beta"), "beta is gone");
			const Output w = watcher.tick(sim);
			test::require(w.saves.size() == 1 && w.saves[0].entries.size() == 1, "SAVES unasked to the other admin session after a delete");
			const Output again = boss.remove("beta");
			requireReason(again, "bad_target", "delete a missing save");
			test::require(again.saves.empty(), "a refused delete sends no SAVES");
			test::require(watcher.tick(sim).saves.empty(), "...and tells nobody");
			requireReason(boss.load("beta"), "bad_target", "load a missing save");
		}

		// ---- load: full resend, same hash, same continuation ----
		{
			boss.sendTick(false);
			const std::uint64_t savedTick = sim.clockTick();
			const std::uint64_t savedHash = sim.stateHash();
			requireReason(boss.save("snap"), "", "save snap");
			watcher.sendTick(false); // takes the unasked SAVES for the save above

			// The uninterrupted run, for the continuation check below.
			for (int i = 0; i < 120; ++i)
				boss.tick(sim);
			const std::uint64_t laterTick = sim.clockTick();
			const std::uint64_t laterHash = sim.stateHash();
			test::require(laterHash != savedHash && laterTick == savedTick + 120, "the world moved on");
			// The session now believes it has sent everything: a plain tick sends no appearances.
			test::require(boss.tick(sim).count(ks::kAppearance) == 0, "the session's caches are warm before the load");

			const Output out = boss.load("snap");
			requireReason(out, "", "load snap");
			test::require(sim.clockTick() == savedTick, "the tick jumped back to the save's tick");
			test::require(sim.stateHash() == savedHash, "the state hash equals the saved one");
			test::require(out.saves.size() == 1 && out.saves[0].entries.size() == 2, "SAVES follows a load");
			// The full resend, as on connect: snapshot, appearances of everyone, squads, weather, clock, bounds.
			test::require(out.count(ks::kSnapBegin) == 1 && out.count(ks::kSnapEnd) == 1, "a snapshot follows a load");
			test::require(out.count(ks::kAppearance) >= static_cast<int>(squad.players.size()),
				"every player character's appearance is sent again (the Known caches were cleared)");
			test::require(out.count(ks::kPlayerSquads) == 1, "PLAYER_SQUADS is sent again");
			test::require(out.count(ks::kWeather) == 1 && out.count(ks::kClock) == 1 && out.count(ks::kBounds) == 1, "WEATHER, CLOCK and DETAIL_BOUNDS are sent again");
			test::require(out.count(ks::kInventory) >= static_cast<int>(squad.players.size()), "the players' inventories are sent again");
			test::require(out.count(ks::kNavParams) == 1 && out.count(ks::kNavTile) >= 1, "the navmesh tiles are sent again");
			std::uint64_t snapTick = 0;
			for (const Frame& f : out.all)
			{
				if (f.type == ks::kSnapBegin)
				{
					Cursor c{f.payload};
					snapTick = c.get<std::uint64_t>();
				}
			}
			test::require(snapTick == savedTick, "the snapshot is at the save's tick");

			// The other admin session and the non-admin one get the full resend at their next tick; only the admin one SAVES.
			const Output w = watcher.tick(sim);
			test::require(w.count(ks::kSnapBegin) == 1 && w.count(ks::kAppearance) >= 2 && w.count(ks::kNavParams) == 1, "other sessions get the full resend after a load");
			test::require(w.saves.size() == 1, "SAVES unasked to the other admin session after a load");
			const Output o = other.sendTick(false);
			test::require(o.count(ks::kAppearance) >= 2 && o.saves.empty(), "a non-admin session gets the resend, not SAVES");

			// The continuation: the loaded world, run on, is the uninterrupted one.
			const std::uint64_t now = sim.clockTick();
			test::require(now == savedTick + 1, "one session tick since the load");
			sim.run(laterTick - now);
			test::require(sim.stateHash() == laterHash, "load and continue gives the straight run's hash");
		}

		// ---- corrupt, truncated and old files ----
		{
			const std::string good = readAll(dir + "/snap");
			std::uint32_t metaSize = 0;
			std::memcpy(&metaSize, good.data() + 8, 4);
			const std::size_t bodyAt = 12 + metaSize;
			writeAll(dir + "/junk", "this is not a save, just text that is long enough to be read");
			writeAll(dir + "/empty", "");
			writeAll(dir + "/short", good.substr(0, 10));
			writeAll(dir + "/truncated", good.substr(0, good.size() - 1));
			writeAll(dir + "/longer", good + "x");
			std::string future = good;
			future[4] = static_cast<char>(kSaveVersion + 1);
			writeAll(dir + "/future", future);
			std::string old = good; // two versions back: the previous one (v64) is still read (ksim_speed_modes)
			old[4] = static_cast<char>(kSaveVersion - 2);
			writeAll(dir + "/old", old);
			std::string v45 = good; // the layout of the build before the merchant refresh (save v45): intact, but older
			v45[4] = 45;
			writeAll(dir + "/v45", v45);
			std::string broken = good; // an intact head, a body that cannot be read: lists ok, loads bad_value
			for (std::size_t i = bodyAt; i < broken.size(); ++i)
				broken[i] = static_cast<char>(0xFF);
			writeAll(dir + "/brokenbody", broken);
			// Things the listing skips.
			writeAll(dir + "/.hidden", good);
			fs::create_directory(dir + "/subdir");
			writeAll(dir + "/colon:name", good);
			fs::create_symlink(dir + "/snap", dir + "/link");

			const Wire32 listed = boss.list().saves.at(0);
			for (const std::string name : {"junk", "empty", "short", "truncated", "longer", "future", "old", "v45"})
			{
				const Wire32::Entry* e = listed.find(name);
				test::require(e != nullptr, name + " is listed");
				test::require(e->status == 2 && e->day == 0 && e->hour == 0 && e->faction.empty() && e->leader.empty() && e->members == 0 && e->money == 0,
					name + ": status 2, fields empty");
			}
			test::require(listed.find("future")->version == kSaveVersion + 1 && listed.find("old")->version == kSaveVersion - 2 && listed.find("junk")->version == 0,
				"the version read from the head is reported");
			test::require(listed.find("brokenbody") != nullptr && listed.find("brokenbody")->status == 0, "a broken body behind an intact head lists as ok (the head is all a listing reads)");
			test::require(listed.find(".hidden") == nullptr && listed.find("subdir") == nullptr && listed.find("colon:name") == nullptr && listed.find("link") == nullptr,
				"dot files, directories, invalid names and symlinks are not listed");
			test::require(listed.find("snap") != nullptr && listed.find("snap")->status == 0, "the good save is still ok");

			const std::uint64_t tick = sim.clockTick();
			const std::uint64_t hash = sim.stateHash();
			for (const std::string name : {"junk", "empty", "short", "truncated", "longer", "future", "old", "v45", "brokenbody"})
			{
				const Output out = boss.load(name);
				// An intact head of an older version is old_version (SIMLINK 81); everything else unreadable stays bad_value.
				requireReason(out, name == "old" || name == "v45" ? "old_version" : "bad_value", "load " + name);
				test::require(out.saves.empty() && out.count(ks::kSnapBegin) == 0, "a failed load sends nothing more");
			}
			requireReason(boss.load("link"), "bad_target", "a symlink is not a save");
			test::require(sim.clockTick() == tick && sim.stateHash() == hash, "a failed load leaves the world untouched");
			// The session keeps working after a failed load.
			test::require(boss.tick(sim).count(ks::kSnapBegin) == 1, "the session carries on");
		}

		// ---- 80-83 are not sim commands ----
		for (const ks::OrderLogEntry& entry : context.orderLog)
			test::require(entry.kind < 80 || entry.kind > 83, "the order log holds a save/load control (kind " + std::to_string(entry.kind) + ")");
		test::require(sim.pendingCommands() == 0, "no control became a command");

		// ---- the save directory rules ----
		{
			// The game install is the caller's `forbidden` (the data dir's parent), as server_main passes it (kenshi-379.13).
			const std::string install = fs::path(KENSHI_DATA).parent_path().string();
			bool refused = false;
			try
			{
				ks::prepareSaveDir(install + "/ksim_saves_never", {install});
			} catch (const std::runtime_error&) {
				refused = true;
			}
			test::require(refused, "a directory inside the game install is refused");
			test::require(!fs::exists(install + "/ksim_saves_never"), "...and not created");
			refused = false;
			try
			{
				ks::prepareSaveDir(install + "/data/../ksim_saves_never", {install});
			} catch (const std::runtime_error&) {
				refused = true;
			}
			test::require(refused, "a dotdot path into the game install is refused");
			refused = false;
			try
			{
				ks::prepareSaveDir(base + "/forbidden/inner", {base + "/forbidden"});
			} catch (const std::runtime_error&) {
				refused = true;
			}
			test::require(refused && !fs::exists(base + "/forbidden"), "a caller's forbidden tree is refused and not created");
			test::require(ks::prepareSaveDir(base + "/forbidden_not/inner", {base + "/forbidden"}) == fs::weakly_canonical(base + "/forbidden_not/inner").string(),
				"a sibling with the same name prefix is not inside");
			test::require(ks::prepareSaveDir(base + "/saves/../saves2/x") == fs::weakly_canonical(base + "/saves2/x").string(), "dotdot is resolved");

			// The default: $XDG_DATA_HOME/ksim/saves, else ~/.local/share/ksim/saves.
			::setenv("XDG_DATA_HOME", (base + "/xdg").c_str(), 1);
			test::require(ks::defaultSaveDir() == base + "/xdg/ksim/saves", "XDG_DATA_HOME default");
			::unsetenv("XDG_DATA_HOME");
			::setenv("HOME", (base + "/hm").c_str(), 1);
			test::require(ks::defaultSaveDir() == base + "/hm/.local/share/ksim/saves", "HOME default");
		}

		// ---- no save directory: the orders are unsupported ----
		{
			ks::ServerContext bare;
			bare.gameData = gd;
			bare.dataHash = context.dataHash;
			Client lone(sim, bare, true, bare.dataHash);
			requireReason(lone.save("x"), "unsupported", "no save directory");
		}

		fs::remove_all(std::string(KSIM_BINARY_DIR) + "/ksim_saves_test");
		std::cout << "ksim_saves: ok\n";
		return 0;
	} catch (const std::exception& e) {
		std::cerr << "ksim_saves: FAILED: " << e.what() << "\n";
		return 1;
	}
}
