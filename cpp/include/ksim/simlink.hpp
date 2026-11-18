#pragma once

// The server half of SIMLINK (docs/formats/SIMLINK.md, protocol 1): the live sim <-> client
// protocol. Everything here sits outside the simulation: it reads the sim between ticks and
// writes to it only through Simulation::enqueue (the command queue is the only writer).
//
//   RecordWriter     one interface, two encoders: BinaryWriter (the wire) and TextWriter (the
//                    key/line debug dump), so the dump can never drift from the wire.
//   Session          one client's server-side state: handshake, string table, what the client
//                    knows, orders in, snapshots and events out. No sockets.
//   SimLinkServer    the TCP loop (POSIX sockets) that drives a Simulation in real time.
//
// Strings are interned while a tick's records are collected, and the STRTAB_APPEND frame is
// written before any record of that tick, so a string id is always defined before first use.

#include "ksim/bloodtrail.hpp"
#include "ksim/command.hpp"
#include "ksim/debugview/view.hpp"
#include "ksim/scenario_run.hpp"
#include "ksim/simulation.hpp"

#include "kdata/gamedata.hpp"
#include "knav/navmesh.hpp"
#include "kworld/loadorder.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iosfwd>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace ksim::simlink {

inline constexpr std::uint32_t kMagic = 0x4D49534B; // 'KSIM'
inline constexpr std::uint16_t kProtocol = 5;
// INTERACTABLE `kind` (SIMLINK.md section 13).
inline constexpr std::uint8_t kInteractableDoor = 1;
inline constexpr std::uint8_t kInteractableItem = 2;
inline constexpr std::uint8_t kInteractableContainer = 3;
inline constexpr std::uint8_t kInteractableProducer = 4; // a deposit, mine or drill (state::Producer); carries the kind-4 tail
inline constexpr std::uint8_t kInteractableBed = 5; // a real layout bed (Simulation::bedViews); carries the kind-5 tail (client-approved 2026-11-06)
// move_to `flags` (SIMLINK.md section 7): bit 0 queue, bit 1 the y field is the clicked surface height.
inline constexpr std::uint8_t kMoveQueue = 1;
inline constexpr std::uint8_t kMoveYValid = 2;
// A valid click height must be within this of 0 (the world is 0..~3000 high; wide on purpose).
inline constexpr double kMaxMoveY = 100000.0;

enum FrameType : std::uint16_t
{
	kHello = 1,
	kStrtab = 2,
	kError = 3,
	kPing = 4, // client keepalive, empty payload (SIMLINK.md section 11)
	kSnapBegin = 10,
	kSnapEnd = 11,
	kAppearance = 20,
	kDynamic = 21,
	kMedical = 22,
	kRoute = 23, // CHAR_ROUTE (SIMLINK.md section 6)
	kInteractable = 24, // INTERACTABLE (SIMLINK.md section 13)
	kInventory = 25, // INVENTORY (SIMLINK.md section 6)
	kDetect = 26, // CHAR_DETECT: a player character's detection meter (SIMLINK.md section 6)
	kItemQuality = 27, // ITEM_QUALITY: the generated quality of an INVENTORY's weapon / armour entries (SIMLINK.md section 6)
	kItemGrid = 28, // ITEM_GRID: the grids and each INVENTORY entry's cell, material and colour (SIMLINK.md section 6)
	kItemLock = 57, // ITEM_LOCK: the lock state of the worn shackles, after ITEM_GRID (SIMLINK.md section 6)
	kItemStats = 29, // ITEM_STATS: one entry's weapon / armour numbers, the answer to order 69 (SIMLINK.md ITEM_STATS); asker only
	kMenuEntries = 61, // MENU_ENTRIES: which right-click entries apply to a target, the answer to order 99 (SIMLINK.md MENU_ENTRIES); asker only
	kProfile = 70, // PROFILE: the per-system tick timings, the answer to order 120 (SIMLINK.md PROFILE); asker only, KSIM_PROFILE builds
	kCharStats = 49, // CHAR_STATS: the character panels of the session's player characters and its focus (SIMLINK.md CHAR_STATS)
	kSquads = 30,
	kPlayerSquads = 31, // PLAYER_SQUADS: the player faction's squads, to the owning session (SIMLINK.md section 6)
	kSaves = 32, // SAVES: the save directory's listing, to admin sessions (SIMLINK.md section 6)
	kStarts = 33, // STARTS: every NEW_GAME_STARTOFF and whether it is ready (SIMLINK.md section 6)
	kJobs = 34, // JOBS: the player characters' jobs lists (SIMLINK.md section 6)
	kProspect = 35, // PROSPECT: a finished prospecting, to every session (SIMLINK.md section 6)
	kUnlocks = 36,
	kDiscovered = 37, // DISCOVERED: the places the player faction has discovered and visited (SIMLINK.md section 6) // UNLOCKS: the player faction's completed research and the buildings it enables (SIMLINK.md section 6)
	kTaskProgress = 39, // TASK_PROGRESS: the player characters' timed tasks with a bar (SIMLINK.md section 6)
	kFactionRelations = 38, // FACTION_RELATIONS: the player faction's relation to the other factions (SIMLINK.md section 6)
	kClock = 40,
	kWeather = 41,
	kTrade = 53, // TRADE: an open trade's prices and the trader's wallet, to the trading session (SIMLINK.md section 6; 43 is NAV_PARAMS)
	kBuilding = 47, // BUILDING: the player faction's buildings, first sight and on change (SIMLINK.md section 6)
	kBuildingGrid = 48, // BUILDING_GRID: the player-town list (Phase 6, not sent yet)
	kBounds = 42,
	kNavParams = 43, // navmesh tile stream (SIMLINK.md section 12)
	kNavTile = 44,
	kNavPolyFlags = 45,
	kEvent = 50,
	kScenarioList = 54, // SCENARIO_LIST: the playtest scenarios the server carries (SIMLINK.md SCENARIO; task CE)
	kScenarioStarted = 55, // SCENARIO_STARTED: the answer to SCENARIO_LOAD 103
	kSeenBy = 59, // SEEN_BY: the hostile observers that know a sneaking player character, the stealth KO's abort test (SIMLINK.md section 6)
	kDetectedBy = 60, // DETECTED_BY: a sneaking player character's detection records (observer, red/yellow, friendly) for the stealth arrows (SIMLINK.md)
	kBeams = 58, // BEAMS: Venge's sky beams, planned ahead, to the sessions in range (SIMLINK.md section 6; 57 is ITEM_LOCK)
	kScenarioMetrics = 56, // SCENARIO_METRICS: the answer to SCENARIO_END 104
	kCages = 64, // CAGES: the caged characters in the session's view: cage key, record, lock state, level, hardness (SIMLINK.md CAGES; kenshi-k16.5.7)
	kResearchState = 63, // RESEARCH_STATE: the player faction's tech level, every RESEARCH record's state and the unlocked refs per kind (SIMLINK.md RESEARCH_STATE)
	kNewGameResult = 62, // NEW_GAME's answer: result, then the faction and its members, or the reason (SIMLINK.md NEW_GAME, MULTIPLAYER.md 4a)
	kScenarioListReq = 102, // SCENARIO_LIST_REQ (client -> server): u32 clientSeq (SIMLINK.md SCENARIO; task CE)
	kScenarioLoad = 103, // SCENARIO_LOAD: u32 clientSeq, str name, u32 seed (0 = the server picks); any session
	kScenarioEnd = 104, // SCENARIO_END: u32 clientSeq; answered with SCENARIO_METRICS; any session
	kOrder = 100,
	kNewGame = 111, // NEW_GAME (client -> server): start a player faction at a start-off (MULTIPLAYER.md 4a). 110 AUTH, 112 JOIN, 113 LEAVE: not built
};

enum Flag : std::uint32_t
{
	kFlagCombat = 1u << 0,
	kFlagStealth = 1u << 1,
	kFlagDrawn = 1u << 2,
	kFlagKO = 1u << 3,
	kFlagDead = 1u << 4,
	kFlagProne = 1u << 5,
	kFlagPlayer = 1u << 6,
	kFlagCarryL = 1u << 7,
	kFlagCarryR = 1u << 8,
	kFlagYValid = 1u << 9,
	kFlagLowDetail = 1u << 10,
	kFlagBlockMode = 1u << 11,
	kFlagRun = 1u << 12, // the run gait is on (toggle 2); with speed modes: set when the mode is not Walk
	kFlagHold = 1u << 13, // standing in hold (order 4 hold = 1, or the AI-off hold); cleared by any replacing order
	kFlagHostile = 1u << 14, // would fight this session's player characters (the sim's enemy() test); never on the player's own
	kFlagSeated = 1u << 15, // a resident on RELAX standing within 1 u of a layout seat of its squad's bar table (TOWN_LIFE_SLICE 5.17); never for the ring fallback
	kFlagAsleep = 1u << 16, // a sleeper lying in its bed (Fighter::asleep: the routine set it on arrival at a bed)
	kFlagSpeedModeShift = 17, // bits 17-18: the speed mode (order 10 `which` 5), (flags >> 17) & 3
	// Under a ceiling (Fighter::indoors, the exe's char+0xd5): in a building's interior and with a surface above this floor, so a
	// roof / top deck is clear. Approved by the client 2026-11-07 (additive, no protocol bump); its roof cut-away may use it.
	kFlagIndoors = 1u << 19,
	// The aim tail is present: a trailing f32 aimYaw after every other conditional tail. Set while a technique with a clip plays
	// (Fighter::aimYaw, the look hand the clip's root motion is turned by). Approved by the client 2026-11-07 (additive).
	kFlagAimTail = 1u << 20,
	// The mounted tail is present: a trailing u64 turret (the kworld::doorKey of the turret placement) after the aim tail. Set while
	// the fighter mans a turret (FighterMount). Approved by the client 2026-11-10 (additive).
	kFlagMounted = 1u << 21,
	// The applied strafe of a direct drive (Fighter::driveStrafe, order 20's trailing i8): 22 left (towards heading - 90 degrees), 23 right.
	// Approved by the client 2026-11-14 (kenshi-k16.8.15; additive, no protocol bump).
	kFlagStrafeLeft = 1u << 22,
	kFlagStrafeRight = 1u << 23,
	// The per-character PASSIVE (24) and HOLD POSITION (25) toggles (Fighter::passive / holdPosition), set by order 10 which 6 / 7.
	// Not bit 13 (kFlagHold, the order-4 stand-and-stay state). Approved by the client 2026-11-16 (k16.6.312; additive, no protocol bump).
	kFlagPassive = 1u << 24,
	kFlagHoldPosition = 1u << 25,
};

// Limits on what a client may send (SIMLINK.md section 11).
inline constexpr std::size_t kMaxClientFrame = 64u << 10; // bytes after the length field
// NEW_GAME (MULTIPLAYER.md 4b): a string field is at most this long, a Spec list has at most kMaxSquadSpec entries and a Spec at most
// kMaxSliders sliders; more is a protocol error.
inline constexpr std::size_t kMaxClientString = 256;
inline constexpr std::uint8_t kMaxSquadSpec = 16;
inline constexpr std::uint8_t kMaxSliders = 64;
inline constexpr std::uint16_t kMaxOrderIds = 256;
inline constexpr float kMaxInterestRadius = 100000.0f;
inline constexpr float kMaxSpeed = 64.0f;
inline constexpr std::size_t kDefaultOrderRate = 100; // orders per wall second
// Drive orders are coalesced per character per tick before they count against the rate; this is
// the hard cap on raw drive frames per window, as a multiple of the rate, against flooding.
inline constexpr std::size_t kDriveRawFactor = 20;
// CHAR_ROUTE carries at most this many waypoints; a longer route sets the truncated bit.
inline constexpr std::size_t kMaxRoutePoints = 256;
enum RouteFlag : std::uint8_t
{
	kRouteTruncated = 1u << 0,
	kRouteApproach = 1u << 1, // the destination is an approach point, not the clicked spot
};

// SIMLINK.md section 2; lives in kworld so the client (which links kworld, not ksim) shares it.
using kworld::dataHash;

// ---- record writers ----

class StringTable
{
public:
	struct Entry
	{
		std::uint8_t kind = 4;
		std::string text;
	};
	// Dense ids from 1; the same (kind, text) always returns the same id.
	std::uint32_t intern(std::uint8_t kind, const std::string& text);
	const std::string& text(std::uint32_t id) const;
	std::size_t size() const noexcept { return entries_.size(); }
	const Entry& entry(std::uint32_t id) const { return entries_[id - 1]; }
	std::uint32_t sent() const noexcept { return sent_; }
	void markSent() noexcept { sent_ = static_cast<std::uint32_t>(entries_.size()); }

private:
	std::vector<Entry> entries_;
	std::map<std::pair<std::uint8_t, std::string>, std::uint32_t> index_;
	std::uint32_t sent_ = 0;
};

// Field names exist for the text dump; the binary writer ignores them. A record is
// begin(type) ... end(); a list is list(name, n, countBits) { item() fields } listEnd().
class RecordWriter
{
public:
	virtual ~RecordWriter() = default;
	virtual void begin(std::uint16_t type) = 0;
	virtual void end() = 0;
	virtual void u8(const char* key, std::uint8_t v) = 0;
	virtual void u16(const char* key, std::uint16_t v) = 0;
	virtual void u32(const char* key, std::uint32_t v) = 0;
	virtual void u64(const char* key, std::uint64_t v) = 0;
	virtual void i16(const char* key, std::int16_t v) = 0;
	virtual void f32(const char* key, float v) = 0;
	virtual void f64(const char* key, double v) = 0;
	virtual void str(const char* key, const std::string& v) = 0;
	virtual void strid(const char* key, std::uint32_t id) = 0;
	virtual void bytes(const char* key, const std::uint8_t* data, std::size_t n) = 0;
	virtual void list(const char* key, std::size_t n, int countBits) = 0;
	virtual void item() = 0;
	virtual void listEnd() = 0;
};

// Frames: u32 len (bytes after it), u16 type, payload; little-endian.
class BinaryWriter : public RecordWriter
{
public:
	explicit BinaryWriter(std::vector<std::uint8_t>& out) : out_(out) {}
	void begin(std::uint16_t type) override;
	void end() override;
	void u8(const char*, std::uint8_t v) override { put(v); }
	void u16(const char*, std::uint16_t v) override { put(v); }
	void u32(const char*, std::uint32_t v) override { put(v); }
	void u64(const char*, std::uint64_t v) override { put(v); }
	void i16(const char*, std::int16_t v) override { put(v); }
	void f32(const char*, float v) override { put(v); }
	void f64(const char*, double v) override { put(v); }
	void str(const char*, const std::string& v) override;
	void strid(const char*, std::uint32_t id) override { put(id); }
	void bytes(const char*, const std::uint8_t* data, std::size_t n) override { out_.insert(out_.end(), data, data + n); }
	void list(const char*, std::size_t n, int countBits) override;
	void item() override {}
	void listEnd() override {}

private:
	template <typename T>
	void put(T v)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(&v);
		out_.insert(out_.end(), bytes, bytes + sizeof(T));
	}
	std::vector<std::uint8_t>& out_;
	std::size_t start_ = 0;
};

// One record per line: `NAME key=value ...`; a list is `key=N[k=v,k=v; k=v,k=v]`; a strid
// prints as `id("text")` when a table is set.
class TextWriter : public RecordWriter
{
public:
	explicit TextWriter(std::ostream& out, const StringTable* table = nullptr) : out_(out), table_(table) {}
	void begin(std::uint16_t type) override;
	void end() override;
	void u8(const char* k, std::uint8_t v) override { field(k, std::to_string(v)); }
	void u16(const char* k, std::uint16_t v) override { field(k, std::to_string(v)); }
	void u32(const char* k, std::uint32_t v) override { field(k, std::to_string(v)); }
	void u64(const char* k, std::uint64_t v) override { field(k, std::to_string(v)); }
	void i16(const char* k, std::int16_t v) override { field(k, std::to_string(v)); }
	void f32(const char* k, float v) override;
	void f64(const char* k, double v) override;
	void str(const char* k, const std::string& v) override;
	void strid(const char* k, std::uint32_t id) override;
	void bytes(const char* k, const std::uint8_t*, std::size_t n) override { field(k, "bytes[" + std::to_string(n) + "]"); }
	void list(const char* k, std::size_t n, int countBits) override;
	void item() override;
	void listEnd() override;

private:
	void field(const char* key, const std::string& value);
	std::ostream& out_;
	const StringTable* table_;
	bool inList_ = false;
	bool firstInEntry_ = true;
	bool firstItem_ = true;
};

// Forwards every call to two writers (the dump tool writes text and binary in one pass).
class TeeWriter : public RecordWriter
{
public:
	TeeWriter(RecordWriter& a, RecordWriter& b) : a_(a), b_(b) {}
	void begin(std::uint16_t t) override { a_.begin(t); b_.begin(t); }
	void end() override { a_.end(); b_.end(); }
	void u8(const char* k, std::uint8_t v) override { a_.u8(k, v); b_.u8(k, v); }
	void u16(const char* k, std::uint16_t v) override { a_.u16(k, v); b_.u16(k, v); }
	void u32(const char* k, std::uint32_t v) override { a_.u32(k, v); b_.u32(k, v); }
	void u64(const char* k, std::uint64_t v) override { a_.u64(k, v); b_.u64(k, v); }
	void i16(const char* k, std::int16_t v) override { a_.i16(k, v); b_.i16(k, v); }
	void f32(const char* k, float v) override { a_.f32(k, v); b_.f32(k, v); }
	void f64(const char* k, double v) override { a_.f64(k, v); b_.f64(k, v); }
	void str(const char* k, const std::string& v) override { a_.str(k, v); b_.str(k, v); }
	void strid(const char* k, std::uint32_t v) override { a_.strid(k, v); b_.strid(k, v); }
	void bytes(const char* k, const std::uint8_t* d, std::size_t n) override { a_.bytes(k, d, n); b_.bytes(k, d, n); }
	void list(const char* k, std::size_t n, int bits) override { a_.list(k, n, bits); b_.list(k, n, bits); }
	void item() override { a_.item(); b_.item(); }
	void listEnd() override { a_.listEnd(); b_.listEnd(); }

private:
	RecordWriter& a_;
	RecordWriter& b_;
};

// Little-endian reader over one frame payload; an overrun sets bad and returns zeros.
struct Reader
{
	const std::uint8_t* p;
	std::size_t n;
	std::size_t at = 0;
	bool bad = false;

	template <typename T>
	T get()
	{
		T v{};
		if (bad || n - at < sizeof(T))
		{
			bad = true;
			return T{};
		}
		std::memcpy(&v, p + at, sizeof(T));
		at += sizeof(T);
		return v;
	}
	std::uint8_t u8() { return get<std::uint8_t>(); }
	std::uint16_t u16() { return get<std::uint16_t>(); }
	std::uint32_t u32() { return get<std::uint32_t>(); }
	std::uint64_t u64() { return get<std::uint64_t>(); }
	// A NaN or Inf anywhere in a client frame is a protocol error, not a bad value.
	float f32()
	{
		const float v = get<float>();
		if (!std::isfinite(v))
			bad = true;
		return v;
	}
	std::string str()
	{
		const std::uint16_t len = u16();
		if (bad || n - at < len)
		{
			bad = true;
			return {};
		}
		std::string s(reinterpret_cast<const char*>(p + at), len);
		at += len;
		return s;
	}
};

// ---- the session ----

// What the scenario knows about a character that the sim does not carry yet (SIMLINK.md
// section 9: appearance fields wait for the sim to carry appearance). PROVISIONAL.
struct CharLook
{
	std::string factionSid; // FACTION record sid; empty = none
	std::uint8_t sex = 0;
	std::uint16_t head = 0;
};

// One accepted order, in log order. sim = it became the Command (stamped for `tick`); the
// others (pause, speed, interest) are logged for the record but are not sim commands.
struct OrderLogEntry
{
	std::uint64_t tick = 0;
	std::uint32_t seq = 0;
	std::uint16_t kind = 0;
	bool sim = false;
	Command command;
	std::string text; // one readable line
	bool immediate = false; // applied between ticks while paused (SIMLINK.md section 7): a replay applies it first on its tick
};

class Session;

// State shared by the sessions of one server run.
struct ServerContext
{
	std::shared_ptr<const kdata::GameData> gameData;
	std::uint64_t dataHash = 0;
	std::string name = "ksim_server";
	std::map<std::uint64_t, CharLook> looks; // by fighter Id
	std::size_t squadLimit = 0; // 0 = every squad
	std::vector<OrderLogEntry> orderLog;
	bool paused = false; // the admin's pause order
	bool idle = false; // SimLinkServer: no ready session is connected (the default pause policy); never set by an order
	// DIALOGUE.md 13.2 (V1: Freeze): the speakers whose reply menu a present session shows; while any is open the sim does not tick. Wall
	// clock, never saved: a reply or end_talk order takes its entry out (the order then applies on the first tick), a gone session clears
	// them all (the menus run on under Hold until the grace ends them, SimLinkServer), a resend after a connect or a load puts them back.
	std::vector<std::uint64_t> menuHolds;
	// The InteractionPolicy of every dialogue menu (DIALOGUE.md 13.2): false = Hold, the V1 default since 2026-11-12 (real time, only the two
	// parties held: menuHolds stays empty, REPLY_MENU's freezes bit is 0); true = Freeze (`--dialogue-pause freeze`): the rows above.
	bool dialogueFreeze = false;
	bool pausedNow() const noexcept { return paused || idle || !menuHolds.empty(); }
	// CLOCK `paused`: bit 0 the idle or admin pause, bit 1 the dialogue pause.
	std::uint8_t pauseBits() const noexcept { return static_cast<std::uint8_t>((paused || idle ? 1 : 0) | (menuHolds.empty() ? 0 : 2)); }
	float speed = 1.0f;
	// Debug overlay: also send CHAR_ROUTE for characters that are not player-controlled.
	bool sendAiRoutes = false;
	// The debug view (docs/formats/SIMLINK.md "DEBUG VIEW"): the server was started with --debug-view. Only a loopback peer
	// (Session::setDebugPeer) gets the data frames; without this flag every debug frame answers enabled = 0.
	bool debugView = false;
	// DIALOGUE.md section 13 `window.close_on_move`: a non-queued order that moves the actor closes its trade window (event 40 reason 1).
	// Off = the original's (MQ11: nothing in the world closes the window; the per-transfer range is the guard).
	bool tradeCloseOnMove = false;
	// Called for notable server-side events (position corrections); null = silent.
	std::function<void(const std::string&)> log;
	// Where orders 80-83 keep the saves (simlink_saves.hpp prepareSaveDir made it); empty = no save directory, the orders
	// answer `unsupported`.
	std::string saveDir;
	// The playtest scenarios (task CE): the directory of `scenarios/<name>.kscen` files (ksim_server --scenarios made it); empty = the
	// scenario orders answer `unsupported`. Order 99 loads one into this simulation.
	std::string scenariosDir;
	// ksim_server --combat-retarget-hold S: overrides a scenario header's retarget_hold (COMBAT_OPTIONS.md 6); unset = the header, else 0.
	std::optional<double> retargetHold;
	// ksim_server --turret-manning on|off: overrides a scenario header's turret_manning; unset = the header, else off.
	std::optional<bool> turretManning;
	// The loaded scenario's run: its seed, start tick and the counters SCENARIO_METRICS reads. Sampled by SimLinkServer after every tick
	// while active; a run never changes the world.
	ScenarioRun scenario;
	// The live sessions, registered by Session itself: a save, load or delete notifies the other admin sessions through it.
	std::vector<Session*> sessions;
	// The next NewGame command's ticket: it names the command to the session that asked for it.
	std::uint64_t nextTicket = 1;
};

class Session
{
public:
	Session(Simulation& sim, ServerContext& context);
	~Session();
	Session(const Session&) = delete;
	Session& operator=(const Session&) = delete;

	// The server's HELLO. First frame.
	void begin(RecordWriter& out);
	// One client frame. False = the session must end: an ERROR frame was written (when it
	// could be) and the caller flushes it and closes.
	bool onFrame(std::uint16_t type, const std::uint8_t* payload, std::size_t size, RecordWriter& out);
	// After both hellos and after every Simulation::tick(). initial = the full snapshot that
	// follows the handshake (clock, squads and bounds included).
	void sendTick(RecordWriter& out, bool initial);
	// Paused: the debug view's queued answers only, no snapshot and no tick (the loop calls it instead of sendTick).
	void sendDebugOnly(RecordWriter& out);
	// Tags the events observed since the last call with the current sim tick. The run loop calls it after every sim tick of a catch-up
	// batch, so one snapshot can carry the batch's events each with its own tick (SIMLINK.md snapshots, SIM_SCHEDULING 5O).
	void stampEvents();
	bool ready() const noexcept { return ready_; }
	const StringTable& strings() const noexcept { return strings_; }
	// A protocol error seen by the transport (a bad frame length): writes ERROR.
	void refuse(RecordWriter& out, const std::string& text);
	// Orders allowed per window (the server opens a window every wall second); excess = ERROR
	// and close. 0 = unlimited (the scripted scenario path).
	void setOrderRateLimit(std::size_t perWindow) noexcept { rateLimit_ = perWindow; }
	void resetRateWindow() noexcept
	{
		ordersInWindow_ = 0;
		rawDrivesInWindow_ = 0;
	}
	// Admin session (SIMLINK.md SAVES): the only one that may run orders 80-83. The server marks the loopback client;
	// every session starts without it.
	void setAdmin(bool admin) noexcept { admin_ = admin; }
	bool admin() const noexcept { return admin_; }
	// The debug view's peer check (docs/formats/SIMLINK.md "DEBUG VIEW"): the server marks the loopback client, and the data
	// frames go only to a marked peer of a --debug-view server.
	void setDebugPeer(bool peer) noexcept { debugPeer_ = peer; }
	bool debugPeer() const noexcept { return debugPeer_; }

private:
	struct Known
	{
		std::uint32_t version = 0;
		bool unconscious = false;
		std::uint32_t routeSent = 0; // the routeVersion the client last heard of
		std::uint32_t routeSeqSent = 0; // the seq of the route the client last heard of (the clear carries it)
	};
	struct Look
	{
		std::string signature;
		std::uint32_t version = 0;
	};
	// A CHAR_DETECT record: the highest sneak meter on a player character, who it is from, and the state.
	struct DetectRecord
	{
		std::uint64_t id = 0;
		std::uint64_t by = 0;
		float meter = 0.0f;
		std::uint8_t state = 0;
	};
	// A SEEN_BY record: the observers (sorted) that know a sneaking player character; empty clears it.
	struct SeenByRecord
	{
		std::uint64_t id = 0;
		std::vector<std::uint64_t> by;
	};
	// A DETECTED_BY record: one sneaking player character's detection map, observers sorted by id; empty clears it.
	struct DetectedByRecord
	{
		struct Entry
		{
			std::uint64_t observer = 0;
			std::uint8_t state = 0; // 2 yellow (being noticed), 1 red (noticed)
			std::uint8_t friendly = 0;
			bool operator==(const Entry&) const = default;
		};
		std::uint64_t id = 0;
		std::vector<Entry> by;
	};
	// A combat event as the arena reported it, kept until the tick's records are written.
	struct Observed
	{
		std::uint64_t tick = 0; // the sim tick it happened in (stampEvents); 0 = the tick of the snapshot that carries it
		std::uint16_t kind = 0; // wire event kind: 1 hit, 3 KO, 5 death
		std::uint64_t who = 0;
		std::uint64_t other = 0;
		std::uint8_t result = 0; // hit: 0 hit, 1 blocked, 2 dodged; interact done: InteractEvent result
		std::uint8_t action = 0; // interact done: the action
		float pos[3] = {};
		std::uint8_t dir = 0;
		float damage[4] = {};
		float absorbed[2] = {}; // hit: cut (pierce merged) and blunt the armour removed
		std::string part; // hit: the body part; picked up: the item record
		std::string dialogue, line, text; // speech (41): the DIALOGUE / DIALOGUE_LINE sids and the resolved text
		std::uint32_t seq = 0; // reply menu (43) / conversation end (44): the menuSeq
		std::vector<std::pair<std::string, std::string>> options; // reply menu (43): {line sid, text}
		float ragdollDir[3] = {}; // ragdoll (48): the unit direction the blow pushes the body, world frame; 0,0,0 = a collapse
		float strength = 0.0f; // ragdoll (48): the velocity change at the hit part, u/s
		std::uint32_t seed = 0; // ragdoll (48): the client's flop seed
		float shotTo[3] = {}; // shot (49): the target's point (pos is the muzzle)
		float chance = 0.0f; // stealth KO (50): the roll's chance 0..100 (0 for an abort)
		std::uint64_t turret = 0; // shot (49): the turret's building key, 0 = a hand crossbow
	};
	// An interactable as the plan found it this tick.
	struct InterRecord
	{
		std::uint64_t id = 0;
		std::uint8_t kind = 0;
		std::string record;
		float pos[3] = {};
		float use[3] = {};
		std::uint8_t state = 0;
		float health = 0.0f;
		float maxHealth = 0.0f;
		std::uint16_t count = 1;
		std::uint32_t version = 0;
		// Kind 4 only: the tail after stateVersion.
		std::string output;
		std::uint16_t cap = 0;
		std::uint8_t operators = 0;
		std::uint8_t maxOperators = 0;
		float progress = 0.0f;
		float rate = 0.0f;
		// the ownership tail (every kind)
		std::string owner;
		std::uint16_t lockLevel = 0;
		std::string model;
		float yaw = 0.0f;
		// Kind 5 (Bed) tail, after the ownership tail: the owning building (the doorKey the `building` field carries), the
		// furniture BUILDING record, and the occupant (0 free).
		std::uint64_t building = 0;
		std::string furniture;
		std::uint64_t occupant = 0;
		// Kind 3 (Container) tail, after the ownership tail: `building` above (0 = a scenario stand-in) and the piece's index in its layout (0xFFFF = none).
		std::uint16_t piece = 0xFFFF;
		// Kind 2 severed limb (item function 0x11) only, after the ownership tail: the tint (task limb-tint), 0..1 as the exe stores it.
		bool limbTint = false;
		float skin[3] = {};
		float blood[3] = {};
		float scale[3] = {};
	};
	struct Box
	{
		float minX, minZ, maxX, maxZ;
		bool operator==(const Box&) const = default;
	};

	bool handleOrder(const std::uint8_t* payload, std::size_t size, RecordWriter& out);
	// ---- save / load session controls (simlink_saves.cpp) ----
	// Orders 80-83 after the frame validated; admin and the name already checked. They answer the order themselves.
	void runSaveControl(std::uint16_t kind, std::uint32_t seq, const std::string& name, RecordWriter& out);
	void writeSaves(RecordWriter& out);
	// Order 84 after the frame validated (admin and a pristine mark checked): the world goes back to the mark (Simulation::newWorld),
	// the mesh's doors and containers are put back, and the answer is event 9, the full resend, STARTS (SIMLINK.md order 84).
	void runNewWorld(std::uint32_t seq, RecordWriter& out);
	// ---- playtest scenarios (simlink_playtest.cpp, task CE) ----
	// Frames 102-104 (SCENARIO_LIST_REQ / SCENARIO_LOAD / SCENARIO_END; not orders) after the frame validated. They answer the frame
	// themselves: 102 the list, 103 the world swap (newWorld, then the file applied; the full resend follows, as after a load) and 104
	// the run's metrics. No admin gate: a playtest runs on the user's own client (PLAYTEST.md 2).
	void runScenarioList(std::uint32_t seq, RecordWriter& out);
	void runScenarioLoad(std::uint32_t seq, const std::string& name, std::uint32_t seed, RecordWriter& out);
	void runScenarioEnd(std::uint32_t seq, RecordWriter& out);
	// SCENARIO_STARTED (55), the answer to SCENARIO_LOAD. Its tail (2026-11-08, client-approved) is the file's `source centre=` as
	// `f32 centreX, centreZ`, so a client can frame the scenario (the spectator camera); NaN for a refusal or a file without one. Then
	// `str bead` (2026-11-10, client-approved): the file's `bead=`, "" for a refusal or a file without one.
	void writeScenarioStarted(RecordWriter& out, std::uint32_t seq, std::uint8_t status, const std::string& error,
		const std::string& name, std::uint64_t fileHash, std::uint32_t runSeed,
		float centreX = std::numeric_limits<float>::quiet_NaN(), float centreZ = std::numeric_limits<float>::quiet_NaN(),
		const std::string& bead = std::string());
	void writeScenarioMetrics(RecordWriter& out, std::uint32_t seq);
	// Puts this session's camera (order 51) back into the world state after a load or a new world: the reload removes
	// state::ViewFocus when the save or the pristine mark has none.
	void restoreViewFocus();
	// NEW_GAME (111, simlink.cpp): the frame's shape is checked here, the rest by Simulation::checkNewGame; an accepted one is a NewGame
	// command for the next tick and the reply (62) goes out from sendTick when it applies.
	bool handleNewGame(const std::uint8_t* payload, std::size_t size, RecordWriter& out);
	// SCENARIO_LIST_REQ / SCENARIO_LOAD / SCENARIO_END (102-104, task CE): read the frame and run it at once (a world swap or a read);
	// the answer is SCENARIO_LIST / SCENARIO_STARTED / SCENARIO_METRICS, every refusal included. False only on a malformed frame.
	bool handleScenarioFrame(std::uint16_t type, const std::uint8_t* payload, std::size_t size, RecordWriter& out);
	void writeNewGameResult(RecordWriter& out, const NewGameOutcome& outcome);
	void writeStarts(RecordWriter& out, const std::vector<Simulation::StartReady>& starts);
	// After a save, load or delete: the other admin sessions get SAVES at their next sendTick (and, after a load, the full resend).
	void notifySaves(bool reload);
	// Forgets everything the client was told, as a fresh session does (the full resend follows).
	void resetKnown();
	void ackApplied(RecordWriter& out, std::uint32_t seq, std::uint64_t appliedTick);
	void ackRejected(RecordWriter& out, std::uint32_t seq, const std::string& reason);
	void writeEvent(RecordWriter& out, std::uint64_t tick, const Observed& e);
	void writeSimple(RecordWriter& out, std::uint64_t tick, std::uint16_t kind, std::uint64_t id);
	void flushStrings(RecordWriter& out);
	std::uint32_t sid(const std::string& text) { return strings_.intern(1, text); }
	void noteMissingItem(std::uint64_t id, const char* recordType, const std::string& name);
	void writeAppearance(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m, std::uint32_t version);
	// playerTeams: the teams of this session's player characters (flag bit 14 is set for a living non-player of another team).
	void writeDynamic(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m, std::uint32_t version,
		const std::set<std::int32_t>& playerTeams);
	void writeRoute(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m, std::uint32_t seq);
	void writeMedical(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m);
	struct DetectRecord;
	void writeDetect(RecordWriter& out, std::uint64_t tick, const DetectRecord& d);
	struct SeenByRecord;
	void writeSeenBy(RecordWriter& out, std::uint64_t tick, const SeenByRecord& s);
	void writeDetectedBy(RecordWriter& out, std::uint64_t tick, const DetectedByRecord& d);
	void writeCharStats(RecordWriter& out, std::uint64_t tick, std::uint64_t id, const Simulation::CharStats& stats);
	void writeInteractable(RecordWriter& out, std::uint64_t tick, const InterRecord& record);
	struct InvRecord
	{
		std::uint64_t id = 0;
		state::Inventory inventory;
	};
	void writeInventory(RecordWriter& out, std::uint64_t tick, const InvRecord& record);
	void writeItemQuality(RecordWriter& out, std::uint64_t tick, const InvRecord& record);
	void writeItemGrid(RecordWriter& out, std::uint64_t tick, const InvRecord& record);
	void writeItemLock(RecordWriter& out, std::uint64_t tick, const InvRecord& record);
	void writeCages(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::CageView>& cages, const std::vector<std::uint64_t>& ids);
	void writeMenuEntries(RecordWriter& out, std::uint64_t tick, std::uint32_t clientSeq, std::uint64_t id, std::uint64_t target, std::uint32_t entries, float pickChance = -1.0f);
#if KSIM_PROFILE
	void writeProfile(RecordWriter& out);
#endif
	void writeItemStats(RecordWriter& out, std::uint64_t tick, std::uint64_t id, std::uint32_t version, std::uint16_t index, const Simulation::ItemStats& stats);
	// ---- trade (trade.hpp): the windows this session has open, one per trading character ----
	struct OpenTrade
	{
		std::uint64_t trader = 0; // the character talked to
		std::uint64_t squad = 0; // its squad: the stock container's id
		std::uint64_t window = 0; // this window's id (the buyback list belongs to one window): the open tick and a counter
		bool sent = false;
		std::uint32_t stockVersion = 0;
		std::uint32_t actorVersion = 0;
		std::int64_t money = 0;
		std::vector<std::pair<std::uint64_t, std::uint32_t>> chests; // the chest-price tail last sent: container id, its INVENTORY version
	};
	struct TradeOut
	{
		std::uint64_t actor = 0;
		std::uint64_t trader = 0;
		Simulation::TradeView view;
	};
	void openTrade(std::uint64_t actor, std::uint64_t trader);
	// Closes the actor's trade (reason 0 client, 1 order, 2 trader gone) with event 40; nothing when it has none.
	void closeTrade(std::uint64_t actor, std::uint8_t reason);
	void writeTrade(RecordWriter& out, std::uint64_t tick, const TradeOut& trade);
	std::uint32_t navMeshId();
	void writeNavParams(RecordWriter& out, std::uint64_t tick);
	struct NavSend
	{
		int x = 0;
		int z = 0;
		std::uint32_t version = 0;
		std::vector<std::uint8_t> blob; // empty: the tile left (size 0)
	};
	struct NavPatchSend
	{
		int x = 0;
		int z = 0;
		const knav::NavMesh::TilePatch* patch = nullptr;
	};
	void writeNavTile(RecordWriter& out, std::uint64_t tick, const NavSend& tile);
	void writeNavPolyFlags(RecordWriter& out, const NavPatchSend& patch);
	void writeClock(RecordWriter& out, std::uint64_t tick);
	void writeWeather(RecordWriter& out, std::uint64_t tick);
	// BEAMS (58): one group's slice of the frame. `reset` = the client forgets that group's beams first (flags bit 0).
	struct BeamsOut
	{
		std::size_t group = 0;
		bool reset = false;
		std::vector<const Beam*> beams;
	};
	// The groups with something to say this tick: a reset (a replan, the connect or a load) and the beams that came into the lead or
	// into range since the last frame (each once; the sent set drops a beam at its end).
	std::vector<BeamsOut> beamsDue(std::uint64_t tick, bool initial, const CombatArena* arena);
	void writeBeams(RecordWriter& out, std::uint64_t tick, const std::vector<BeamsOut>& groups);
	void writeSquads(RecordWriter& out, std::uint64_t tick);
	void writePlayerSquads(RecordWriter& out, std::uint64_t tick, const PlayerSquadsView& view);
	void writeResearchState(RecordWriter& out, std::uint64_t tick, bool hasFaction, const std::vector<std::string>& completed);
	void writeUnlocks(RecordWriter& out, std::uint64_t tick, const std::vector<std::string>& research, const std::vector<std::string>& buildings);
	void writeFactionRelations(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::FactionRelationRow>& rows);
	void writeTaskProgress(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::TaskProgressRow>& rows);
	void writeDiscovered(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::DiscoveredPlace>& places);
	void writeBuildings(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::BuildingView>& rows);
	void writeJobs(RecordWriter& out, std::uint64_t tick, const std::vector<std::pair<std::uint64_t, JobsView>>& lists);
	void writeProspect(RecordWriter& out, std::uint64_t tick, const prospect::Result& result);
	std::vector<Box> computeBounds() const;
	bool inBounds(const std::vector<Box>& boxes, double x, double z) const;
	bool inInterest(double x, double z) const;
	std::uint32_t lookVersion(const CombatArena::Member& m);
	std::string lookSignature(const state::Fighter& f, const std::string& sheathed) const;

	Simulation& sim_;
	ServerContext& ctx_;
	bool admin_ = false;
	bool debugPeer_ = false; // the loopback peer check of the debug view (Session::setDebugPeer)
	debugview::View debug_; // the debug view's session state (frames 200-219), inert without KSIM_DEBUG_VIEW
	bool startsDue_ = false; // STARTS goes out at the next sendTick (a new world; the first sendTick sends it anyway)
	std::vector<Simulation::StartReady> startsSent_; // what the client last heard
	std::size_t startsKey_ = 0; // the navigation mesh's tile count at the last readiness check
	std::set<std::uint64_t> pendingNewGame_; // tickets of this session's NewGame commands not applied yet
	std::vector<NewGameOutcome> newGameOutcomes_; // applied, not yet answered
	std::map<std::uint64_t, std::uint64_t> offMeshLogged_; // fighter Id -> the last tick its position_corrected line was logged (one line per episode, not per tick)
	bool savesDue_ = false; // another session changed the saves: SAVES goes out at the next sendTick
	bool resendDue_ = false; // another session loaded a save: the next sendTick is a full snapshot
	StringTable strings_;
	std::size_t rateLimit_ = 0;
	std::size_t ordersInWindow_ = 0;
	std::size_t rawDrivesInWindow_ = 0;
	std::unordered_map<std::uint64_t, std::uint64_t> driveCounted_; // fighter -> tick its drive order was counted for
	bool gotHello_ = false;
	bool ready_ = false;
	bool hasInterest_ = false;
	float interestX_ = 0, interestZ_ = 0, interestR_ = 1000;
	std::unordered_map<std::uint64_t, Known> known_;
	std::vector<BloodEvent> bloodOut_;
	BloodTrail blood_; // ground-blood events (2, 16, 17), derived from the mirror per tick
	// CHAR_DETECT: what the client last heard for a player character (state, meter in 1/64 steps); an entry exists while it is non-zero.
	std::unordered_map<std::uint64_t, std::pair<std::uint8_t, int>> detectSent_;
	// SEEN_BY: the observer set the client last heard for a sneaking player character (an entry exists while it is non-empty).
	std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> seenBySent_;
	// DETECTED_BY: the detection map the client last heard per sneaking player character (an entry exists while it is non-empty).
	std::unordered_map<std::uint64_t, std::vector<DetectedByRecord::Entry>> detectedBySent_;
	std::uint64_t focus_ = 0; // order 87: the inspect focus (session control, never cleared by the server)
	std::uint64_t focusProducer_ = 0; // order 87 with a producer's id: the focus panel's node (replaces focus_)
	std::optional<std::uint32_t> producerSent_; // its record's version the client last heard; empty = send
	// The client's camera (order 51): session state, like focus_. The WEATHER wind tail reads it, so it survives a world reset; the
	// world's state::ViewFocus (residents, discovery) is re-applied from it after a load or a new world.
	bool hasView_ = false;
	float viewX_ = 0, viewZ_ = 0;
	std::unordered_map<std::uint64_t, Simulation::CharStats> charStatsSent_; // CHAR_STATS: what the client last heard per character
	bool charStatsNow_ = false; // the next send skips CHAR_STATS' 1 Hz slot (a paused order: the clock does not move, so the slot never comes)
	std::unordered_map<std::uint64_t, Look> looks_;
	std::set<std::pair<std::uint64_t, std::string>> missingItemLogged_; // (character, name) of an APPEARANCE item with no record, logged once
	std::vector<Box> sentBounds_;
	// BEAMS: per BeamField group, the replan epoch the client last heard and the beams sent since (id -> end tick).
	std::vector<std::uint64_t> beamEpochSent_;
	std::vector<std::unordered_map<std::uint32_t, std::uint64_t>> beamsSent_;
	bool beamsForget_ = true; // the next frame resets every group (connect, load, new world)
	std::uint64_t weatherPicksSent_ = ~0ull; // WeatherSystem::picks() the last WEATHER frame carried (resent when it changes)
	bool boundsSent_ = false;
	PlayerSquadsView squadsSent_; // PLAYER_SQUADS as the client last heard it (faction 0 = nothing sent)
	bool squadsKnown_ = false;
	std::vector<std::string> unlocksSent_; // UNLOCKS: the completed research the client last heard
	bool unlocksKnown_ = false;
	// CAGES: what the client last heard (a row per caged character in view, by id) and the cheap signature (id, cage, lock opened) that triggers a rebuild.
	struct CageSent
	{
		std::vector<std::uint64_t> ids;
		std::vector<Simulation::CageView> rows;
		std::vector<std::tuple<std::uint64_t, std::uint64_t, bool>> signature;
		bool known = false;
	} cagesSent_;
	std::vector<Simulation::DiscoveredPlace> discoveredSent_; // DISCOVERED: the list the client last heard
	bool discoveredKnown_ = false;
	std::vector<Simulation::FactionRelationRow> relationsSent_; // FACTION_RELATIONS: the list the client last heard
	bool relationsKnown_ = false;
	std::vector<Simulation::TaskProgressRow> taskSent_; // TASK_PROGRESS: the list the client last heard
	std::uint64_t taskSentTick_ = 0;
	std::uint64_t lastSendTick_ = 0; // the tick of the last sendTick: interval-aligned sends fire when a batch crossed a boundary
	bool taskKnown_ = false;
	std::uint8_t progressLevel_ = 1; // order 98: 0 none, 1 the original's bars of the player's characters, 2 every task of the player's, 3 plus everyone in view
	bool taskForce_ = false; // the level changed: the next tick sends the list whatever the change test says
	std::map<std::uint64_t, std::uint32_t> buildingsSent_; // BUILDING: each building's version as the client last heard it
	std::map<std::uint64_t, JobsView> jobsSent_; // JOBS: each player character's list as the client last heard it
	const std::unordered_map<std::uint64_t, std::uint64_t>* squadOf_ = nullptr; // character -> squad Id (the Simulation's cache), set each sendTick
	std::uint64_t squadIdOf(std::uint64_t character) const
	{
		if (squadOf_ == nullptr)
			return 0;
		const auto found = squadOf_->find(character);
		return found != squadOf_->end() ? found->second : 0;
	}
	std::shared_ptr<const prospect::Result> prospectDue_; // PROSPECT: the latest finished task, sent at the next sendTick
	std::vector<Observed> observed_;
	std::vector<std::pair<std::uint32_t, std::string>> lateRejects_; // event 10 after the order was applied (talk: nothing_to_say)
	std::uint8_t clockPausedSent_ = 0xFF; // the CLOCK `paused` bits last sent: a change sends CLOCK at once
	void holdMenu(std::uint64_t speaker);
	void releaseMenu(std::uint64_t speaker);
	std::map<std::pair<int, int>, std::uint32_t> navSent_; // tiles (x, z) the client holds -> the version it holds
	bool navParamsSent_ = false;
	std::uint64_t navRevision_ = 0; // the mesh revision and detail bounds the tile stream last ran for: it is skipped while both stand
	std::vector<Box> navBounds_;
	bool navPending_ = false; // the last pass stopped at the byte budget with tiles still to send
	std::uint32_t navMeshId_ = 0;
	std::unordered_map<std::uint64_t, std::uint32_t> knownInventory_; // player character id -> inventory version the client has
	std::uint32_t tradeWindows_ = 0;
	std::map<std::uint64_t, OpenTrade> trades_; // actor id -> its open trade (session state: the sim keeps only the stock and the wallets)
	std::map<std::uint64_t, std::set<std::uint64_t>> openLoots_; // opened body / container id -> the session's looters that opened it (loot open)
	struct InterKnown
	{
		std::uint32_t version = 0;
		std::uint64_t pass = 0; // the sendTick pass that last saw the entity alive: an entry left behind is gone from the world
	};
	std::unordered_map<std::uint64_t, InterKnown> knownInter_; // interactable id -> version the client has
	std::uint64_t interPass_ = 0;
};

// ---- the TCP server ----

struct ServerOptions
{
	std::string host = "127.0.0.1";
	int port = 0; // 0 = pick one; onListening says which
	std::string recordPath; // every byte sent to the client, appended (a recorded stream)
	std::function<void(int port)> onListening;
	int helloTimeoutMs = 10000; // connect -> valid HELLO
	int frameTimeoutMs = 10000; // a started frame must finish within this
	int pingTimeoutMs = 30000;  // after HELLO: no frame of any type for this long = dropped
	std::size_t orderRate = kDefaultOrderRate;
	// Default (false): run() does not tick the sim while no session has completed HELLO (CLOCK says paused); the first ready session
	// resumes it, the last one leaving pauses it again, the speed is kept, and an admin pause order still wins. ksim_server
	// --run-without-clients sets this to keep it ticking with no client (a server config replaces the flag later).
	bool runWithoutClients = false;
};

class SimLinkServer
{
public:
	SimLinkServer(Simulation& sim, ServerContext& context, ServerOptions options);
	~SimLinkServer();
	SimLinkServer(const SimLinkServer&) = delete;
	SimLinkServer& operator=(const SimLinkServer&) = delete;
	// Binds and listens; returns the port. Throws on failure.
	int listen();
	// Serves one client at a time and ticks the simulation in real time (20 Hz at speed 1)
	// until stop is set. Throws on a socket failure.
	void run(const std::atomic<bool>& stop);
	// Runs `fn` on the thread inside run() and returns when it has (at most one loop pass, 50 ms, later): the way another thread reads
	// or pokes the sim and the context without a race. Only while run() is going; run() drains what is left when it ends.
	void call(const std::function<void()>& fn);

private:
	void drainCalls();
	Simulation& sim_;
	ServerContext& ctx_;
	ServerOptions options_;
	int listenFd_ = -1;
	std::mutex callMutex_;
	std::deque<std::pair<const std::function<void()>*, std::promise<void>>> calls_;
};

} // namespace ksim::simlink
