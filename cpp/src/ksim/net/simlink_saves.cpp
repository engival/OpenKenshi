// Orders 80-83 (save_game, load_game, delete_save, list_saves) and the SAVES (32) record: SIMLINK.md SAVES.
// Session controls like pause: not sim commands, never in the order log, run between ticks (the server handles a frame
// between two ticks).

#include "ksim/net/simlink.hpp"
#include "ksim/world/interact.hpp"
#include "ksim/net/simlink_saves.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0 // Windows has none; its handles are not inherited without an explicit request
#endif
#ifdef _WIN32
#define KSIM_O_BINARY _O_BINARY
#else
#define KSIM_O_BINARY 0
#endif
#ifdef _WIN32
#define KSIM_FSYNC(fd) ::_commit(fd)
#else
#define KSIM_FSYNC(fd) ::fsync(fd)
#endif

namespace ksim::simlink {

namespace fs = std::filesystem;

namespace {

// One code point from strict UTF-8, advancing `at`; false for anything malformed.
bool nextCodePoint(const std::string& text, std::size_t& at, std::uint32_t& cp)
{
	const auto byte = [&](std::size_t i) { return static_cast<std::uint8_t>(text[i]); };
	const std::uint8_t lead = byte(at);
	std::size_t extra = 0;
	std::uint32_t min = 0;
	if (lead < 0x80)
	{
		cp = lead;
		++at;
		return true;
	} else if ((lead & 0xE0) == 0xC0) {
		extra = 1;
		cp = lead & 0x1F;
		min = 0x80;
	} else if ((lead & 0xF0) == 0xE0) {
		extra = 2;
		cp = lead & 0x0F;
		min = 0x800;
	} else if ((lead & 0xF8) == 0xF0) {
		extra = 3;
		cp = lead & 0x07;
		min = 0x10000;
	} else {
		return false;
	}
	if (text.size() - at <= extra)
		return false;
	for (std::size_t i = 1; i <= extra; ++i)
	{
		if ((byte(at + i) & 0xC0) != 0x80)
			return false;
		cp = (cp << 6) | (byte(at + i) & 0x3F);
	}
	at += extra + 1;
	return cp >= min && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// Whether `path` equals `root` or lies below it, by path component.
bool isInside(const fs::path& path, const fs::path& root)
{
	auto p = path.begin();
	for (auto r = root.begin(); r != root.end(); ++r, ++p)
	{
		if (p == path.end() || *p != *r)
			return false;
	}
	return true;
}

} // namespace

std::string checkSaveName(const std::string& name)
{
	if (name.empty() || name.size() > kSaveNameMaxBytes || name.front() == '.')
		return "bad_value";
	std::size_t chars = 0;
	for (std::size_t at = 0; at < name.size();)
	{
		std::uint32_t cp = 0;
		if (!nextCodePoint(name, at, cp))
			return "bad_value";
		if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == '/' || cp == '\\' || cp == ':')
			return "bad_value";
		++chars;
	}
	return chars > kSaveNameMaxChars ? "bad_value" : "";
}

std::string defaultSaveDir()
{
	const char* xdg = std::getenv("XDG_DATA_HOME");
	if (xdg != nullptr && xdg[0] != '\0')
		return std::string(xdg) + "/ksim/saves";
#ifdef _WIN32
	const char* appdata = std::getenv("APPDATA");
	if (appdata != nullptr && appdata[0] != '\0')
		return std::string(appdata) + "/ksim/saves";
#endif
	const char* home = std::getenv("HOME");
	if (home != nullptr && home[0] != '\0')
		return std::string(home) + "/.local/share/ksim/saves";
	throw std::runtime_error("no save directory: neither XDG_DATA_HOME nor HOME is set (use --save-dir)");
}

std::string prepareSaveDir(const std::string& dir, const std::vector<std::string>& forbidden)
{
	std::error_code error;
	// Judge the target before creating anything: a forbidden tree is never written to, not even with an empty directory.
	const fs::path wanted = fs::weakly_canonical(fs::absolute(dir), error);
	if (error)
		throw std::runtime_error("save directory " + dir + ": " + error.message());
	// The game install is the caller's `forbidden` (server_main passes the resolved data dir's parent): the game never writes saves into it
	// (kenshi-379.13: this used to add one hard-coded developer install, a home path a shipped binary must not carry).
	std::vector<std::string> trees = forbidden;
	for (const std::string& tree : trees)
	{
		const fs::path root = fs::weakly_canonical(fs::absolute(tree), error);
		if (!error && isInside(wanted, root))
			throw std::runtime_error("save directory " + wanted.string() + " is inside the game install " + root.string());
	}
	fs::create_directories(wanted, error);
	if (error || !fs::is_directory(wanted))
		throw std::runtime_error("cannot create save directory " + wanted.string() + (error ? ": " + error.message() : ""));
	return wanted.string();
}

std::string savePath(const std::string& dir, const std::string& name)
{
	return dir + "/" + name;
}

std::vector<SaveEntry> listSaves(const std::string& dir)
{
	std::vector<SaveEntry> entries;
	std::error_code error;
	for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
	{
		const std::string name = it->path().filename().string();
		if (!checkSaveName(name).empty())
			continue;
		if (it->symlink_status(error).type() != fs::file_type::regular)
			continue;
		struct stat info;
		if (::stat(it->path().string().c_str(), &info) != 0)
			continue;
		SaveEntry entry;
		entry.name = name;
		entry.mtime = static_cast<std::int64_t>(info.st_mtime);
		std::ifstream in(it->path(), std::ios::binary);
		entry.info = readSaveInfo(in, static_cast<std::uint64_t>(info.st_size));
		entry.status = entry.info.readable ? 0 : 2;
		entries.push_back(std::move(entry));
	}
	std::sort(entries.begin(), entries.end(), [](const SaveEntry& a, const SaveEntry& b)
	{
		return a.mtime != b.mtime ? a.mtime > b.mtime : a.name < b.name;
	});
	if (entries.size() > 0xFFFF)
		entries.resize(0xFFFF);
	return entries;
}

void writeFileAtomic(const std::string& path, const std::string& bytes)
{
	const fs::path target(path);
	const std::string temp = (target.parent_path() / ("." + target.filename().string() + ".tmp")).string();
	const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | KSIM_O_BINARY, 0644);
	if (fd < 0)
		throw std::runtime_error("cannot write " + temp + ": " + std::strerror(errno));
	std::size_t done = 0;
	bool ok = true;
	while (ok && done < bytes.size())
	{
		const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
		if (n <= 0)
			ok = false;
		else
			done += static_cast<std::size_t>(n);
	}
	ok = ok && KSIM_FSYNC(fd) == 0;
	ok = ::close(fd) == 0 && ok;
	std::error_code renameError;
	if (ok)
		fs::rename(temp, path, renameError); // replaces an existing target on Windows too (::rename does not)
	if (!ok || renameError)
	{
		const std::string why = ok ? renameError.message() : std::strerror(errno);
		::unlink(temp.c_str());
		throw std::runtime_error("cannot write " + path + ": " + why);
	}
}

// ---- the session half ----

void Session::writeSaves(RecordWriter& out)
{
	const std::vector<SaveEntry> entries = listSaves(ctx_.saveDir);
	out.begin(kSaves);
	out.u64("tick", sim_.clockTick());
	out.list("saves", entries.size(), 16);
	for (const SaveEntry& entry : entries)
	{
		out.item();
		out.str("name", entry.name);
		out.u64("mtime", static_cast<std::uint64_t>(entry.mtime)); // i64 on the wire
		out.u32("saveVersion", entry.info.version);
		out.u8("status", entry.status);
		out.u32("day", entry.info.day);
		out.f32("hour", entry.info.hour);
		out.str("factionName", entry.info.factionName);
		out.str("leaderName", entry.info.leaderName);
		out.u16("members", entry.info.members);
		out.u64("money", static_cast<std::uint64_t>(entry.info.money)); // i64 on the wire
	}
	out.listEnd();
	out.end();
}

void Session::notifySaves(bool reload)
{
	for (Session* other : ctx_.sessions)
	{
		if (other == this || !other->ready_)
			continue;
		if (other->admin_)
			other->savesDue_ = true;
		if (reload)
			other->resendDue_ = true;
	}
}

void Session::resetKnown()
{
	known_.clear();
	bloodOut_.clear();
	blood_ = BloodTrail();
	detectSent_.clear();
	charStatsSent_.clear();
	looks_.clear();
	sentBounds_.clear();
	weatherPicksSent_ = ~0ull;
	beamsSent_.clear();
	beamEpochSent_.clear();
	beamsForget_ = true;
	boundsSent_ = false;
	squadsSent_ = PlayerSquadsView();
	squadsKnown_ = false;
	unlocksSent_.clear();
	unlocksKnown_ = false;
	discoveredSent_.clear();
	discoveredKnown_ = false;
	cagesSent_ = {}; // CAGES: the next frame is a full list (a loaded world has other cages)
	taskSent_.clear();
	taskKnown_ = false;
	jobsSent_.clear();
	prospectDue_.reset();
	squadOf_ = nullptr;
	observed_.clear();
	// The world the trades were in is gone: each window closes (the client treats the jump as a reset anyway).
	while (!trades_.empty())
		closeTrade(trades_.begin()->first, 2);
	navSent_.clear();
	navParamsSent_ = false;
	navRevision_ = 0;
	navMeshId_ = 0;
	knownInventory_.clear();
	knownInter_.clear();
	driveCounted_.clear();
	// A NEW_GAME in flight is gone with the world it was queued in.
	pendingNewGame_.clear();
	newGameOutcomes_.clear();
}

void Session::runSaveControl(std::uint16_t kind, std::uint32_t seq, const std::string& name, RecordWriter& out)
{
	if (kind == 83)
	{
		ackApplied(out, seq, sim_.clockTick());
		writeSaves(out);
		return;
	}
	const std::string path = savePath(ctx_.saveDir, name);
	std::error_code error;
	const bool exists = fs::symlink_status(path, error).type() == fs::file_type::regular;
	if (kind == 80)
	{
		std::ostringstream bytes;
		try
		{
			sim_.save(bytes);
			writeFileAtomic(path, bytes.str());
		} catch (const std::exception& e) {
			if (ctx_.log)
				ctx_.log("save_game " + name + ": " + e.what());
			ackRejected(out, seq, "io_error");
			return;
		}
		ackApplied(out, seq, sim_.clockTick());
		writeSaves(out);
		notifySaves(false);
	} else if (kind == 82) {
		if (!exists)
		{
			ackRejected(out, seq, "bad_target");
			return;
		}
		if (!fs::remove(path, error))
		{
			if (ctx_.log)
				ctx_.log("delete_save " + name + ": " + error.message());
			ackRejected(out, seq, "io_error");
			return;
		}
		ackApplied(out, seq, sim_.clockTick());
		writeSaves(out);
		notifySaves(false);
	} else { // 81 load_game
		if (!exists)
		{
			ackRejected(out, seq, "bad_target");
			return;
		}
		std::ifstream in(path, std::ios::binary);
		const char* reason = "bad_value";
		try
		{
			if (!in)
				throw SaveError("cannot open " + path);
			in.seekg(0, std::ios::end);
			const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
			in.seekg(0, std::ios::beg);
			const SaveInfo info = readSaveInfo(in, size);
			if (!info.readable)
			{
				// A file with our magic and an OLDER version number was made by an older build (there are no migrations): old_version,
				// so the save panel can say so. A newer one, or anything with no readable head, stays bad_value.
				if (info.version != 0 && info.version < kSaveVersion)
					reason = "old_version";
				throw SaveError(name + " is not a readable save of this version");
			}
			in.clear();
			in.seekg(0, std::ios::beg);
			sim_.reload(in);
			restoreViewFocus(); // the save's ViewFocus may be another camera's, or none
		} catch (const std::exception& e) {
			if (ctx_.log)
				ctx_.log("load_game " + name + ": " + e.what());
			ackRejected(out, seq, reason);
			return;
		}
		// The world is the save's now: nothing this session sent before is true, the clock jumped.
		ctx_.cameraStart.reset(); // a save of our own format replaces the import
		resetKnown();
		ctx_.scenario.active = false; // the run belonged to the world that was replaced
		ackApplied(out, seq, sim_.clockTick());
		writeSaves(out);
		sendTick(out, true);
		notifySaves(true);
	}
}

void Session::runNewWorld(std::uint32_t seq, RecordWriter& out)
{
	try
	{
		sim_.newWorld();
		restoreViewFocus(); // the pristine mark's ViewFocus, not this client's camera
		// The Hub's furniture is the world's, not a scenario's: the mesh's doors and storage come back with the fresh world.
		if (const MeshNavigator* nav = sim_.navigation())
		{
			spawnMeshDoors(sim_, *nav);
			const kdata::Record* contents = nullptr;
			if (ctx_.gameData != nullptr)
			{
				for (const kdata::Record* record : ctx_.gameData->byType(kworld::itemTypeByName("VENDOR_LIST")))
				{
					if (record->name == "house contents")
						contents = record;
				}
			}
			spawnMeshContainers(sim_, *nav, contents);
			spawnMeshItemNodes(sim_, *nav);
		}
	} catch (const std::exception& e) {
		if (ctx_.log)
			ctx_.log(std::string("new_world: ") + e.what());
		ackRejected(out, seq, "bad_value");
		return;
	}
	// Nothing this server knew of the scenario's characters is true: their looks go, the client's world is cleared by the event.
	ctx_.looks.clear();
	ctx_.cameraStart.reset(); // the world is no longer the import
	resetKnown();
	ctx_.scenario.active = false; // the run belonged to the world that was replaced
	startsDue_ = true;
	ackApplied(out, seq, sim_.clockTick());
	sendTick(out, true);
	notifySaves(true);
}

void Session::restoreViewFocus()
{
	if (!hasView_)
		return;
	sim_.enqueue(Command::setView(viewX_, viewZ_)); // applies on the next tick, like the original order
}

} // namespace ksim::simlink
