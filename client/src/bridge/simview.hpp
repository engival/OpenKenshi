#pragma once
#include <deque>
#include <utility>
#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "characters/characters.hpp"
#include "knav/walk.hpp"
#include "net/simlink.hpp"
#include "kutil/platform/socket.hpp"

// The live sim link on screen (docs/formats/SIMLINK.md, docs/CLIENT.md "Live sim link"). Source: a recorded server stream (FILE.bin, replayed at the sim's
// 20 ticks/s) or a server (HOST:PORT, TCP). Drives Characters: one character per sim character with an appearance, rebuilt when the set or an appearance
// version changes; position and yaw interpolated between the two newest samples, about 100 ms behind the newest; actions started at their tick.
class SimView
{
public:
	static constexpr double kTickRate = 20.0;
	// The render delay in ticks at game speed 1 (100 ms). kenshi-k16.2.181.1: it scales with the game speed — the client renders about
	// 100 ms of wall time behind the newest snapshot, which is 2 ticks at 1x and 10 at 5x. A fixed tick count shrank to 20 ms at 5x, less
	// than the arrival jitter of a frame, and the render clock crossed the newest snapshot on any hitch: the drawn froze, then jumped when
	// the next snapshot landed. kDelayTicks stays the 1x value (the docs and the tests name it).
	static constexpr double kDelayTicks = 2.0;

	// target: FILE.bin, or HOST:PORT (then dataDir resolves the load order for the hello's dataHash). False with err set.
	bool open(const std::string& target, const std::string& dataDir, std::string& err);
	// The same with the hash already known (a worker computed it: ~24 ms of the startup step otherwise, kenshi-k16.2.26); a FILE target ignores it.
	bool open(const std::string& target, std::uint64_t dataHash, std::string& err);
	bool active() const { return mode_ != None; }
	void end();	// EXIT GAME (end_session): the link closed and every bit of this session's state dropped, as a fresh SimView
	bool live() const { return mode_ == Live; }	// a server session: the orders and the scenario frames go out
	bool pumpLink() { return pump(); }	// the socket's bytes into the decoder with no frame drawn: a quit waits for an answer this way
	double startTick = -1;	// replay: start at this tick (> 0) instead of the first snapshot
	// dt: real seconds. ground(x, z): terrain height, used while yValid is clear (the sim has no heights yet).
	void update(double dt, Characters& chars, const std::function<double(double, double)>& ground);
	double dtAnim() const { return dtAnim_; }	// the sim seconds the render clock advanced this frame (0 on a stall or pause): the animation's clock (kenshi-k16.2.479.1)
#ifdef KCLIENT_DEV_TOOLS
	void devStall(bool on) { devStall_ = on; }	// script `set stall 0|1`: hold the snapshots (the socket unread), a network gap
#endif
	double dtSim() const { return dtSim_; }	// this frame's sim seconds (0 paused, x the game speed), set by update
	// The render delay for the current game speed, in ticks: the ~100 ms of wall time the client draws behind the newest snapshot
	// (kDelayTicks at 1x, x5 at 5x; SIMLINK 4). A speed below 1 (slow motion) keeps at least a quarter tick of history.
	double delayTicks() const { const double speed = dec_.clock.valid ? static_cast<double>(dec_.clock.speed) : 1.0; return kDelayTicks * (speed > 0.25 ? speed : 0.25); }
	// While another user shares `chars` (the creator's preview): no character sync, the stream still runs. The release rebuilds the set, dropping the preview.
	void hold(bool on);
	// The first player-controlled character's position, else any character's; false when none has been seen yet.
	bool focus(double out[3]) const;
	std::string status() const;	// one HUD line
	const std::string& error() const { return error_; }	// the link's failure text once it failed (kenshi-k16.2.380)
	const simlink::SimLinkDecoder& decoder() const { return dec_; }
	// DEBUG VIEW (client/src/debug/): the sink for frames 200-219, and a control frame to the server (live only)
	void setDebugSink(std::function<void(uint16_t, const uint8_t*, size_t)> sink) { dec_.debugFrames = std::move(sink); }
	bool sendDebug(const std::vector<uint8_t>& frame) { return mode_ == Live && send(frame, "debug"); }
	std::deque<simlink::NavPatch> takeNavPatches() { return std::exchange(dec_.navPatches, {}); }	// NAV_POLY_FLAGS for ClientNav, in arrival order

	// Orders (live only; SIMLINK 7). Selection: player-controlled characters. pick: the character nearest the ray (origin, unit dir) within reach, 0 = none.
	// tOut: the hit's distance along dir; skip: leave a character out of the pick (the click passes one it does not draw, main.cpp)
	// body (k16.2.223): a lying / asleep character's drawn skeleton as world segments (point pairs, at most 4), their count; 0 = none, the sim's segment is used
	uint64_t pick(const double origin[3], const double dir[3], double* tOut = nullptr, const std::function<bool(uint64_t)>& skip = {}, const std::function<int(uint64_t, double[][3])>& body = {}) const;
	// pickBody (kenshi-1rnr): the same, by the drawn skeleton's per-bone capsules (Characters::pickCapsules, posed this frame), the click pick's character half.
	// `capsules` appends a character's capsules by its id (0: none, not built). pick above is the original's box / lying segment, kept for debug.pick_collision.
	uint64_t pickBody(const double origin[3], const double dir[3], double* tOut, const std::function<bool(uint64_t)>& skip, const std::function<int(uint64_t, std::vector<Characters::PickCapsule>&)>& capsules) const;
	bool isPlayer(uint64_t id) const;
	bool placed(uint64_t id, double pos[3], float hull[3]) const;
	// The catch-up's own world velocity this frame (u/s, x z; the slot's catchV: the correction the legs walk besides the wire's), the log gait dump's (kenshi-h2v.1.9.1). False: no such drawn character.
	bool catchVelocity(uint64_t id, double out[2]) const { const auto s = slots_.find(id); if (s == slots_.end()) { return false; } out[0] = s->second.catchV[0]; out[1] = s->second.catchV[1]; return true; }
	// where the sim puts id this frame (the interpolated wire position, with no display offset; kenshi-k16.2.73 trails); false: not drawn
	bool simPos(uint64_t id, double pos[3]) const;
	// The wire y the standing ray last cast from for id (the sim's interpolated y minus the prediction offset; simPos's y is the drawn y after the ray, the
	// height probe needs the input); false: no such drawn character or no cast yet (no ray result, a seated / asleep character, the ray off)
	bool wireY(uint64_t id, double& y) const;
	// id's wire state (flags, building, floor) at the render tick, the sample simPos interpolates from -- not `cur`, the newest snapshot: the render clock
	// sits behind it by design and further after a stall, so the newest floor / indoors bits can belong to a later position (the floor view read them
	// there and flapped 0/1/0/1 as a climb boarded its flight, kenshi-k16.2.236.17); false: no such character
	bool stateAt(uint64_t id, simlink::Dynamic& out) const;
	bool nudge(uint64_t id, double dx, double dz);	// debug: shift id's drawn position (the next frame sees the error: a catch-up, past 400 u a teleport); false: not drawn	// where id is drawn this frame (feet) and its race click box; false: not drawn
	const std::vector<uint64_t>& characterIds() const { return order_; }	// every sim character built this frame, in Characters index order
	// Ragdolls (CLIENT_GOALS "Ragdolls" P3): the EVENT 48s of this update, for the ragdoll driver (Ragdolls::update drains them), and whether
	// id is drawn lying (KO / dead, not carried, not getting up): a ragdoll lives only while it is.
	struct RagdollCue { uint64_t id = 0; uint8_t cause = 0; float dir[3] = {}; float strength = 0; std::string part; };
	std::vector<RagdollCue> ragdollCues;
	bool lying(uint64_t id) const;
	// The carrier's id while id is drawn carried (`carry me` on its shoulder; 0 otherwise): the carried ragdoll hangs from it (P4).
	uint64_t carrier(uint64_t id) const;
	// A lying pose the client could not build (a race with no lying clip, plans/animal-down): the wire says KO / dead but the state stays
	// uncommitted (layer-null) until a ragdoll carries the pose. Ragdolls starts a body for it.
	bool lyingUnbuilt(uint64_t id) const;
	// The ragdoll driver started a body for id: a lying state that could not be built (lyingUnbuilt) is committed as Lying now, and the character
	// is drawn by the ragdoll. True when this committed it: the caller stops the character's stride (SimView never updates a down char's speed).
	bool ragdollStarted(uint64_t id);
	// The rest point a prone body lies at and its lying yaw (CHAR_DYNAMIC flag bit 5: x, y, z are the rest point, plus the tail's restYaw);
	// false when the sim sent none (not prone, or an older server).
	bool restPoint(uint64_t id, double pos[3], float& yaw) const;
	std::string raceOf(uint64_t id) const;	// the race's name ("Greenlander"), "" unknown
	uint64_t idOfChar(int index) const { for (const auto& [id, s] : slots_) { if (s.index == index) { return id; } } return 0; }	// the sim id of Characters index (0: none)
	int charIndex(uint64_t id, uint32_t* version = nullptr) const;	// id's index in Characters (-1: not built) and its appearance version
	void select(uint64_t id, bool add);	// a non-player id (or 0) is ignored: the selection never empties
	const std::vector<uint64_t>& selected() const { return selected_; }
	// Follow mode (kenshi-cym.40.2.9): the stream's real empty selection. selectNone() empties it and the refill (one of ours always selected) stays off until
	// a select() or releaseEmpty(); normal play never calls it.
	void selectNone() { selected_.clear(); holdEmpty_ = true; }
	void releaseEmpty() { holdEmpty_ = false; }
	bool holdsEmpty() const { return holdEmpty_; }
	// The debug shot's route sidecar (kenshi-k16.2.231.1): where a move_to click came from, beside the order itself (a dev-only record).
	struct Click { const char* what = "click"; bool building = false; std::string buildingName; int floor = -1; };
	bool moveTo(const double at[3], bool queue, bool yValid = true, const Click* click = nullptr);	// the selection; at: the clicked surface point (y sent as yValid; false: the heightmap's only, the sim takes the surface nearest in xz, ksim k16.8.10); false when nothing is sent
	bool attack(uint64_t target);
	bool sneak();	// toggles sneak for the selection (order 10 toggle 1): on unless all of it already sneaks
	bool sneakOn() const;	// all of the selection sneaks: sneak() would turn it off
	bool follow(uint64_t leader);	// the selection except the leader; false when nobody else is selected
	// The serial of the last order that replaced a character's task (an unqueued move_to, attack, interact, stop / hold or follow sent by this
	// client); 0 none. taskSerial(): the latest one sent. The loot window closes when its looter gets a new one (kinspect loot.md).
	uint32_t taskOrder(uint64_t id) const { const auto i = tasked_.find(id); return i == tasked_.end() ? 0 : i->second; }
	uint32_t taskSerial() const { return taskSerial_; }
	bool interact(uint64_t target, uint8_t action, bool queue = false, uint8_t flags = 0);	// order 60 for the selection; queue: after its current orders (Shift) (SIMLINK 13: 1 open, 2 close, 5 loot, 7 pick_up, 10 first_aid); flags: 5 loot bit 0 = open the body
	// Order 60 action 15 pick / 16 cut / 17 break on the locked shackles `wearer` has on (SIMLINK order 60 "Shackle orders"), done by `actor`: itself, or a
	// squadmate who walks to it. Not the selection's: it is the inventory window's own character.
	bool shackle(uint64_t actor, uint64_t wearer, uint8_t action);
	bool drop(uint64_t id, uint32_t item, uint16_t count);	// order 61: count carried units of item (a strid from its INVENTORY) at id's feet
	bool equip(uint64_t id, uint32_t item, bool on);	// order 62: wield / wear a carried WEAPON or ARMOUR (item strid), or put it back
	// Grid inventory (SIMLINK 9): orders 64-68 on one entry of an INVENTORY (version + index as the client saw them); section 255 = autoplace (68)
	bool moveItem(uint64_t id, uint32_t version, uint16_t index, uint8_t section, uint8_t x, uint8_t y, uint16_t count = 0);	// 64 (id: a character or a container; count 0: the whole stack)
	bool arrange(uint64_t id, uint8_t section);	// 65
	bool dropEntry(uint64_t id, uint32_t version, uint16_t index, uint16_t count);	// 66
	bool equipEntry(uint64_t id, uint32_t version, uint16_t index, bool on);	// 67
	bool transferEntry(uint64_t id, uint64_t container, uint32_t version, uint16_t index, uint16_t count, bool take, uint8_t section, uint8_t x, uint8_t y);	// 68
	// Player squads (SIMLINK 70-75; the state is decoder().playerSquads, refreshed after each applied one)
	bool squadAdd();
	bool squadDelete(uint64_t squad);
	bool squadRename(uint64_t squad, const std::string& name);	// on every keystroke; the last one wins
	bool factionRename(const std::string& name);	// order 85, once per commit (Enter / focus loss)
	bool squadMove(uint64_t id, uint64_t squad, uint16_t index, bool swap);	// index 0xFFFF appends, 0 = leader; swap: with the member at index
	bool squadReorder(uint64_t squad, uint16_t index);
	bool dismiss(uint64_t id);
	// Saves (SIMLINK 80-83; the list is decoder().saves, refreshed after each). A load drops the world (decoder resetSeq) and the selection.
	bool saveGame(const std::string& name);
	bool loadGame(const std::string& name, bool resetPositions);
	bool deleteSave(const std::string& name);
	bool listSaves();
	// New game (SIMLINK 84, 111, 62): newWorld drops the world like a load; newGame then places the start-off's squad (decoder().newGame answers).
	bool newWorld();
	bool newGame(const std::string& name, uint32_t startoff, const std::vector<simlink::OrderWriter::NewGameSpec>& specs = {});
	// Scenarios (SIMLINK "SCENARIO", the playtest's wire): the list (decoder().scenarioList answers), a load (the world swap, a load's
	// reset) and a run's end (decoder().scenarioMetrics answers). Any session may send them: no admin gate.
	bool scenarioList();
	bool scenarioLoad(const std::string& name, uint32_t seed);
	bool scenarioEnd();
	bool transfer(uint64_t id, uint64_t container, uint32_t item, uint16_t count, bool take);	// order 63: take from / put into a container within 16 u
	// taking from it is theft: its owner (the INTERACTABLE tail) is set and not ours, or (an older server) a theft event named it
	bool foreign(uint64_t thing) const;
	uint32_t ownFaction() const;	// our faction's FACTION strid: SQUADS `faction` of a squad PLAYER_SQUADS lists; 0 before both arrived
	std::vector<uint16_t> takeCrimeLog() { return std::exchange(crimeLog_, {}); }	// event kinds 51 (caught_stealing) / 52 (alarm_raised) since the last take: HUD message-bar lines
	uint32_t takeLockTooHard() { return std::exchange(lockTooHard_, 0); }	// event 13 result 8 cage picks (a player character's, action 18) since the last take: the HUD message-bar line
	std::vector<uint32_t> takeDiscovered() { return std::exchange(discovered_, {}); }	// TOWN strids of event 19 (discovered) since the last take: the log line
	// Floating texts from events (the game's ScreenLabel, kinspect floating_text.md): event 1 hit (landed; other = the attacker, damage cut / blunt /
	// pierce / stun, part its strid) and 20 lock_roll. pos: the character's feet when it came (the label follows the character while it is drawn); dropped 12 sim-s after
	struct Popup { uint64_t ch = 0; uint16_t kind = 0; uint8_t result = 0; double tick = 0; double pos[3] = {}; uint64_t other = 0; float damage[4] = {}, absorbed[2] = {}; uint32_t part = 0; };
	const std::deque<Popup>& popups() const { return popups_; }
	// Speech bubbles (event 41, DIALOGUE slice 1): the line each character is saying, by speaker; dropped when its duration runs out, on event 42
	// (cut short) or when the speaker leaves. text: its strid (the server's resolved wording); tick: the render tick it came.
	struct Speech { uint32_t text = 0; double tick = 0; float duration = 0; uint64_t target = 0; uint8_t flags = 0; };
	const std::unordered_map<uint64_t, Speech>& speech() const { return speech_; }
	double tick() const { return tick_; }	// the render tick (fractional); < 0 before the first snapshot	// a theft (event 14) was seen there: not ours
	const simlink::Inventory* inventory(uint64_t id) const;
	const simlink::Medical* medical(uint64_t id) const;	// nullptr until its CHAR_MEDICAL arrives
	const simlink::CharStats* charStats(uint64_t id) const;	// nullptr until its CHAR_STATS arrives (player characters and the focus)
	void focus(uint64_t id, bool producer = false);	// order 87 on a change: the focus (0 = none) the server sends CHAR_STATS for; producer: a producer id (its BUILDING row, k16.19.8.2)
	// The focused producer's BUILDING row (order 87; nullptr until it arrives): kept apart from dec().buildings, where the player's own buildings live
	// (a focused producer row is never the player's: its owner is 0 for a wild node or an NPC faction's entity id; SIMLINK BUILDING "A producer's row")
	const simlink::SimLinkDecoder::Building* focusBuilding() const;
	const simlink::SimLinkDecoder::Building* building(uint64_t id) const { const auto it = dec_.buildings.find(id); return it == dec_.buildings.end() ? nullptr : &it->second; }	// a BUILDING 47 row by id
	bool playerBuilding(const simlink::SimLinkDecoder::Building& b) const;	// its owner is the session's faction (never owner 0: a wild or NPC producer row)

	bool progressLevel(uint8_t level);	// order 98: the over-head progress rows' level (0 none, 1 the original's set, 2 every player row, 3 plus NPCs)
	const simlink::Detect* detect(uint64_t id) const;	// CHAR_DETECT; nullptr while nobody is noticing it
	float health(uint64_t id, const Characters& chars) const;	// 0..1: the mean of the parts' health / race max (clamped); 1 without a record	// player characters only; nullptr until its INVENTORY arrives
	// Interactables (doors, ground items, containers). pickItem: the ground item or container whose pick sphere (kItemReach round pos + kItemLift;
	// containers kContainerReach / kContainerLift) the ray passes nearest, 0 = none; tOut: where the ray enters that sphere (the thing's own hit point,
	// kenshi-k16.2.259), 1e30 when none. MINE sizes until the original's pick shapes are measured. skip: things the caller picks by their own shape
	// (drawn containers: WorldBuildings::propRay).
	uint64_t pickItem(const double origin[3], const double dir[3], double* tOut = nullptr, const std::function<bool(uint64_t)>& skip = {}) const;
	static constexpr double kItemReach = 4, kItemLift = 1, kContainerReach = 6, kContainerLift = 4;
	const simlink::Interactable* thing(uint64_t id) const;
	// kenshi-k16.2.412.4: a ground item / container's DRAWN y: the sim's pos y through groundAt (the body's feet lookup), cached per thing (moves with its pos or the solver's generation). Falls back to pos y.
	double thingY(uint64_t id, const simlink::Interactable& t) const;
	// How far over its pos y the drawn ground may lie for a thing to count as buried and be lifted onto it (lift, never sink: furniture surfaces are not in the solver). The coarse navmesh y runs 12-33 u under
	// the drawn dune of the hill east of the Storm House (user 2026-11-19; measured in the findings of kenshi-k16.2.414.2: hill*.txt, hill_a.png), so 36 u covers it.
	static constexpr double kBuriedMax = 36;
	const std::unordered_map<uint64_t, simlink::Interactable>& things() const { return dec_.things; }
	uint32_t thingsSerial() const { return dec_.thingsSerial; }	// moves when things() changed (the props list's dirty tracking)
	std::string nameOf(uint32_t record, const Characters& chars) const;	// a record strid's display name
	std::string sidOf(uint32_t record) const { return dec_.str(record); }	// a record strid's stringID
	// For the HUD / UI: toggles for the selection (order 10: 0 block, 1 stealth, 2 run, 3 ai_off), stop / hold, game pause and speed (orders 40, 41)
	bool setToggle(uint8_t which, uint8_t value);
	bool jobAdd(uint16_t type, uint64_t target);	// order 76 for the selection (without target itself); type: the exe's TaskData number
	bool jobRemove(uint64_t id, uint32_t jobId);	// 77
	bool prospect(uint64_t id);	// 79: id prospects where it stands; PROSPECT (35) answers when it ends
	bool jobMove(uint64_t id, uint32_t jobId, uint16_t index);	// 78
	bool stop(bool hold);
	bool pause(bool paused);
	// Order 69 once per (id, version, index): the answer lands in decoder().itemStats; its applied stays quiet, a refusal (a stale version) is a reply
	bool askItemStats(uint64_t id, uint32_t version, uint16_t index);
	const simlink::SimLinkDecoder::ItemStats* itemStats(uint64_t id, uint32_t version, uint16_t index) const;
	// Order 120 profile_query (SIMLINK.md "PROFILE", kenshi-k16.2.148): at most once a second, only while the profiling overlay is open. The answer
	// lands in decoder().profile (the PROFILE frame); a refusal (not_allowed / unsupported) is kept in profileStatus() and stops every later query
	// (no retry storm). false when not live, rate-limited, refused, or the send failed.
	bool profileQuery();
	const simlink::SimLinkDecoder::Profile* profile() const;	// nullptr until a PROFILE answer arrives
	const std::string& profileStatus() const { return profileStatus_; }	// the refusal's word, "" while none
	bool reply(const simlink::SimLinkDecoder::ReplyMenu& m, uint8_t option);	// order 88 for the menu's char; the answer comes back as its speech (event 41 bit 2)
	bool endTalk(const simlink::SimLinkDecoder::ReplyMenu& m);	// order 89; event 44 removes the menu
	bool tradeClose();	// order 86; event 40 answers and drops the trade (decoder().trade)
	// Order 99 menu_query (SIMLINK MENU_ENTRIES, kenshi-k16.2.121): the first selected player character asks which right-click rows apply on
	// target. The seq sent (0: not sent: no live session, no selection or no target). MENU_ENTRIES (61) answers, or event 10 refuses it.
	uint32_t menuQuery(uint64_t target);
	using MenuEntries = simlink::SimLinkDecoder::MenuEntries;
	std::deque<MenuEntries> takeMenuEntries() { return std::exchange(dec_.menuEntries, {}); }	// every MENU_ENTRIES since the last call
	// event 9 / 10 answering a menu_query, by its clientSeq (the order-answer queue is the host's, k16.20.14.2); reason: the refusal's word
	struct MenuAnswer { uint32_t seq = 0; bool applied = false; std::string reason; };
	std::vector<MenuAnswer> takeMenuAnswers() { return std::move(menuAnswers_); }	// every one since the last call
	bool setSpeed(float speed);
	const auto& clock() const { return dec_.clock; }
	// Direct drive (SIMLINK 12): order 20 for one player character, 0 stop / 1 forward / 2 backward along heading (the sim's yaw: 0 = +z, towards +x).
	// Sent as is: the caller sends on change, at most at the tick rate. Its event 9 stays off the HUD; event 8 (blocked) shows there.
	bool drive(uint64_t id, uint8_t drive, float heading, int strafe = 0);
	bool predicting() const { return local_.on; }
	// kenshi-k16.2.237.9: the live heading of the driven character, every frame; the order on the wire goes at the tick rate, but the prediction (the drawn yaw and
	// the path) steps with the mouse, not in 20 Hz stairs
	void aim(uint64_t id, double heading) { if (local_.id == id) { local_.heading = heading; } }
	double localHeading() const { return local_.heading; }	// the heading the last drive order sent (kenshi-k16.2.237.9 log)
	// The standing ray (kenshi-k16.2.344): a drawn standing character's y is the drawn building surface nearest the wire's y under its x, z (WorldBuildings::groundDrawn; collision meshes left the stand search in kenshi-k16.2.382.2).
	// res: 2 a drawn-floor hit (y), 0 a drawn surface over x,z but none in the window, 3 open ground, -1 a building mesh there is still loading; gen changes with the drawn set (the per-character cache's key).
	using GroundRay = std::function<int(double x, double z, double wireY, double up, double down, double& y, const void** gen)>;
	// Display depenetration (kenshi-k16.2.345): the collision's push (dx, dz) for a body at (x, z) with feet y; false: none. Never fed back to the sim.
	using Depenetrate = std::function<bool(double x, double z, double y, double radius, double maxPush, double out[2], std::string* why)>;
	void setDepenetrate(Depenetrate d) { depen_ = std::move(d); }
	struct NudgeStats { uint64_t frames = 0, nudgedSum = 0; int frameNudged = 0, maxFrameNudged = 0; double maxOffset = 0, frameMax = 0, msSum = 0, frameMs = 0, maxFrameMs = 0; };
	const NudgeStats& nudgeStats() const { return nudge_; }
#ifdef KCLIENT_DEV_TOOLS
	std::function<double(double, double)> ylogAlt;	// KYLOG only (k16.2.382.1): the other terrain sampler, logged beside the one in use
#endif
	// The terrain point (x, y, z) lies inside a carved interior, where the terrain is not drawn (WorldBuildings::insideInterior; the sunk-dune rule of the stand search, kenshi-k16.2.412.1)
	void setGroundRay(GroundRay r, std::function<const void*()> gen) { groundRay_ = std::move(r); groundGen_ = std::move(gen); }
	// The standing ray's window around the wire y (kenshi-k16.2.344): up above, down below. The navmesh floats 0.45-0.71 u over the plank floor and up to ~4 u off on slopes (k16.7.36, k16.6.271).
	static constexpr double kRayUp = 1.5, kRayDown = 4.5, kDrawnDown = 8.0;	// kDrawnDown: how far under the wire y the drawn floors are looked for (kenshi-k16.2.382.2; the terrain still competes inside kRayDown)
	// One uncached stand-ray cast with update()'s rule (the ray, then solveGround: bridge/simview.cpp), for the height probe (dev/heightprobe.cpp): terrain is the
	// drawn triangle height update() gets. Returns the result as update() sees it (-1 collision not loaded, 0 miss, 1 hit, 2 drawn-face hit); y is set for >= 1.
	// kind: what decided it ("collision", "drawn", "terrain", "open ground (terrain)", or why it missed).
	int standRayAt(double x, double z, double wireY, double terrain, double& y, const char** kind = nullptr) const;
	// The ground solver, navmesh -> drawn (kenshi-k16.2.414.2): the drawn walkable ground nearest the sim's navmesh point (x, navY, z) in [navY - kDrawnDown, navY + up], the terrain
	// included under the interior-mask rule. >= 1: y is the drawn ground; 0 none in reach (keep the navmesh y); -1 not loaded yet / no solver. One rule for the body's feet
	// (update, predict), the selection ring, the move marker, the route line and a probe hit on a character. No floor argument: the nearest walkable surface to the navmesh
	// point IS the floor it is on (floors are separate navmesh islands, >= a storey apart). kind (optional) says what decided it.
	int groundAt(double x, double z, double navY, double up, double down, double& y, const char** kind = nullptr) const;
	int groundAt(double x, double z, double navY, double& y) const { return groundAt(x, z, navY, kRayUp, kRayDown, y); }
	// The terrain the solver joins (the drawn terrain triangles) and the carve test (is the point inside an interior mask whose BUILDING has `interior terrain` 0)
	void setTerrainFn(std::function<double(double, double)> t, std::function<bool(double, double, double)> masked) { terrainFn_ = std::move(t); terrainMasked_ = std::move(masked); }
	struct RayStats { uint64_t faces = 0, casts = 0, cached = 0, hits = 0, misses = 0, unloaded = 0; double ms = 0, maxFrameMs = 0; int frameCasts = 0, maxFrameCasts = 0; double frameMs = 0; double maxShift = 0, sumShift = 0; };
	const RayStats& rayStats() const { return rayStats_; }	// totals since the start; a dev counter (dump / the evlog standray line)
	void setWalker(const knav::Walker* w) { walker_ = w; }	// ClientNav's, for drive prediction (each frame; null: none)
	// A character's interpolated feet, yaw and route (the waypoints still ahead); false when it is not on screen.
	bool pose(uint64_t id, double pos[3], double& yaw, const simlink::Route*& route) const;
	// The debug shot's route sidecar (kenshi-k16.2.231.1): the wire's current CHAR_ROUTE of id (nullptr: the character is not known), its display
	// name ("" none), every player-controlled character seen (sorted by id), and the last orders this client sent for id — oldest first, at most
	// kOrdersKept — each with the answer the wire gave it so far (a route with that clientSeq, event 9 / 10, or nothing). Every order that sets a
	// character's task is kept, not only move_to (kenshi-4bps.1): what names the order.
	struct OrderRow
	{
		uint32_t seq = 0;
		uint64_t id = 0;
		uint64_t tick = 0;	// the sim tick of the newest snapshot when the order went out (0: before the first)
		std::string what;	// the order: "attack" / "follow" / "stop" / "hold" / "job" / an interact's name ("loot", "pick up"); for a move_to, where its click came from ("click" / "use point" / "script" / "map")
		uint64_t target = 0;	// attack / follow / interact / job: the sim id the order named (0: none)
		double goal[3] = {};	// a move_to: the clicked surface point as sent; else the target's wire position at the send
		bool yValid = false, queue = false, building = false;
		bool move = false;	// the order is a move_to (what is its click origin: the route sidecar's route / the marker's tick)
		std::string buildingName;	// the building the click was in ("" none)
		int floor = -1;	// its floor (-1 none)
		std::string outcome;	// filled by orders() only: the wire's answer (route / applied / rejected / nothing)
	};
	static constexpr size_t kOrdersKept = 8;
	const simlink::Route* routeOf(uint64_t id) const;
	std::string charName(uint64_t id) const;
	std::vector<uint64_t> players() const;
	std::vector<OrderRow> orders(uint64_t id) const;
	uint64_t lastMoveTick(uint64_t id) const;	// the sim tick of the newest move_to sent for id (0: none): the trails' longer window
	// The camera's ground point, every frame (live): set_interest (send filter, render.interest_radius round it) after kInterestMove units of travel, set_view
	// (a logged sim command; the server quantizes it to 4608-unit zones with hysteresis) after kViewMove, so both stay rare (SIMLINK 5, 7).
	void view(double x, double z);
	static constexpr double kInterestRadius = 2000, kInterestMove = 250, kViewMove = 1000;	// 2000: render.interest_radius's default (kenshi-k16.2.258.10.1)
	// a hit splash for the terrain blood (BLOOD_DECALS.md: 140793b20): world x, z, size (0.05..0.5), dirRad (x-z angle, the image's u axis), colour
	// kind: a hit splash (decal type 1), a bleed drop (2), a growing pool (2, resized by id: BloodDecals::pool) or a smear (3)
	enum SplashKind { kHit, kDrop, kPool, kSmear };
	struct Splash { double x, z; float size, dirRad, colour[4]; SplashKind kind = kHit; uint64_t id = 0; };
	std::vector<Splash> takeSplashes() { return std::move(splashes_); }
	std::vector<std::string> takeReplies() { return std::move(replies_); }	// every answer since the last call (UI toasts); status() keeps the newest
	bool takeRejected() { const bool r = rejected_; rejected_ = false; return r; }	// true once after event 10 rejected one of our orders
	// An event 9 / 10 answering one of our orders, by its clientSeq (kenshi-k16.20.14.2: the host's per-inventory order queue matches them up; the
	// reply strings alone carry no seq). reason: event 10's word, empty when applied.
	struct OrderAnswer { uint32_t seq = 0; bool applied = false; std::string reason; };
	std::vector<OrderAnswer> takeOrderAnswers() { return std::move(orderAnswers_); }	// every order answer since the last call
	// The clientSeq of the order sent last (every send records it; no other order can slip in between a send and this read).
	uint32_t lastSentSeq() const { return lastSentSeq_; }
	// The wire's answer to the order with this clientSeq, without consuming it ("applied tick N" / "rejected REASON"; "" none yet): the follow log's ack line (kenshi-cym.40.2.13).
	std::string orderResult(uint32_t seq) const { const auto r = orderResults_.find(seq); return r == orderResults_.end() ? std::string() : r->second; }
	// Combat sounds (kenshi-h2v.29.7; findings audio_sfx_triggers.md sections 7 and 8): the posts the original makes from receiveHit and the ranged
	// launcher, from SIMLINK events 1 (hit), 49 (shot) and 5 (death). Each is one post on a world object: `object` (a character's sim id, or a temporary
	// object for a bolt's ground impact; `temporary`: the host removes it after the post) at `pos`, with the switches set first.
	struct SoundCue
	{
		uint64_t object = 0, tick = 0;
		bool temporary = false;
		double pos[3] = {};
		std::vector<std::pair<std::string, std::string>> switches;
		std::string event;
		float gate = 1000;
	};
	std::vector<SoundCue> takeSoundCues() { return std::exchange(cues_, {}); }
	// Door and task sounds (kenshi-h2v.29.8; findings audio_sfx_triggers.md sections 4-6): a door's `door type` (the Door_Type switch: 0 Wood, 1
	// Mechanical_Metal, 2 Gate) and `door move speed` (the leaf's travel: Door_Shut follows Door_Open / Door_Close after 1 / speed s, UNSURE unit), by the
	// door's BUILDING stringID, from the host's data load.
	void setDoorSound(const std::string& sid, int type, float speed) { doorSounds_[sid] = { type, speed }; }
	// A BUILDING record's functionality `special tool` (an ITEM name): held in the right hand while a character operates a producer of that record
	void setSpecialTool(const std::string& buildingSid, const std::string& item) { specialTools_[buildingSid] = item; }
	bool logHits = false;	// script `log hits`: print every EVENT 1 (a hit, blocked or dodged) with its tick, victim, attacker, damage and point
	// What the overlay draws: each selected character's interpolated feet and its route (the waypoints still ahead; empty = none)
	struct Marker { double pos[3]; const simlink::Route* route; };
	std::vector<Marker> markers() const;

private:
	enum Mode { None, File, Live };
	enum Down { Up, Lying, Crawling, Carried, Seated, Asleep };
	struct Slot { int index = -1; uint32_t version = 0; uint32_t action = 0, poseClip = 0; uint64_t actionStart = 0; bool loop = false; Down down = Up; bool rising = false, seen = false; int carry = 0, severed = 0; double pos[3] = {}, yaw = 0, bodyYaw = 0; bool bodyOn = false; float hull[3] = { 5, 20, 5 }; bool drawn = false; float vFwd = 0, vLat = 0, vSim = 0; float speedWin[2][8] = {}; int speedHead = -1; bool failed = false; Down failedDown = Up; uint32_t failedClip = 0; bool ragdollHeld = false; double catchStart = -1, catchMax = 0; uint64_t jumpTick = 0; double off[2] = {}, basePrev[2] = {}, sim[3] = {}, catchV[2] = {}; uint64_t wireTick = 0, wireMoveTick = 0, stallLogTick = 0, offLogTick = 0, nobodyLogTick = 0; float wireLast[2] = {}; uint8_t nobodyState = 255, nobodyPending = 255, nobodyPendingCount = 0; double rayX = 1e30, rayZ = 0, rayWireY = 0, rayY = 0; int rayRes = -2; const void* rayGen = nullptr; double missX = 1e30, missZ = 0; double nudge[2] = {}; bool nudgeOn = false; double drawY = 0; bool drawYOn = false; };	// the standing ray's cache (kenshi-k16.2.344): the input (x, z, wire y, gather) and its result (rayRes 1 hit at rayY, 2 drawn face at rayY, 3 open ground (nothing covers x,z), 0 miss, -1 not loaded; -2 none); missX / missZ: where a miss was last logged	// bodyYaw / bodyOn: the drawn body's own yaw while drive.facing = travel (or easing back from it; k16.2.237.10); the tail (kenshi-k16.2.264.1): wireTick = the newest sample's tick last seen, wireLast its xz, wireMoveTick the sample tick it last changed at (a walkstall line); stallLogTick / offLogTick / nobodyLogTick = the render tick each case line was last written at (5 s apart); nobodyState = the DrawState the last nobody line named, nobodyPending / nobodyPendingCount = the state being confirmed (two frames: the draw state is one prepare behind, and a rebuild renumbers the indices); off: display slop, the drawn position's offset from the sim (xz; eased to 0, kenshi-k16.2.74); basePrev: the sim's interpolated position last frame (the base-jump test); sim: the sim's interpolated position this frame (gameplay reads it; a pick reads its x, z, and the drawn y, k16.2.395.1); jumpTick: the wire sample last logged as a sim jump (evlog simjump); catchStart / catchMax: the catch-up episode under way (its first sim tick, -1: none) and its largest error, logged when it ends (evlog); drawn: pos holds last frame's drawn position; vFwd / vLat: its filtered velocity in the body frame (the legs); speedWin / speedHead: the last 8 raw fwd / lat speeds, the next write at speedHead (-1: empty); rising: playing the get-up while down stays Lying; seen: drawn once (a character first seen already down is snapped, not fallen into); failed / failedDown / failedClip: the down state the last rebuild failed to build (no retry while the wire still reads it, sl.down keeps the pose); ragdollHeld: sl.down's Lying was committed for an unbuildable lying pose that a ragdoll carries (the get-up is Characters::riseLive); drawY / drawYOn: the drawn body's eased y (kenshi-k16.2.395.1: the stand height eases toward, so a stair tread does not step it) and whether it holds a value from a previous frame
	std::vector<SoundCue> cues_;
	struct DoorSound { int type = 0; float speed = 0.5f; };
	std::unordered_map<std::string, std::string> specialTools_;	// BUILDING stringID -> its functionality's special tool ITEM name
	std::unordered_map<std::string, DoorSound> doorSounds_;
	std::unordered_map<uint64_t, double> doorFirst_;	// door id -> the render tick it was first seen: its first second is arrival (the scenario's own opening), posts nothing
	std::unordered_map<uint64_t, uint8_t> doorState_;	// door id -> the state last seen (a first sight posts nothing)
	std::unordered_map<uint64_t, uint64_t> carried_;	// body id -> its carrier, while carried
	std::unordered_map<uint64_t, int> departing_;	// body id -> its index in Characters, while its departure dither runs (render.char_fade, kenshi-k16.2.258.10.1)
	std::vector<std::pair<double, SoundCue>> pending_;	// cues due at a render tick (Door_Shut)
	std::set<uint64_t> rollOk_;	// doors a lock_roll succeeded on in this batch (their Unlock_Success is the picker's, posted by the event)
	bool soundsPrimed_ = false;	// the first world pass seeds doorState_ / carried_ without posting
	void cueWorld(const Characters& chars);
	std::string wornMaterial(const Characters& chars, uint64_t id, const std::string& part) const;	// Armor_Worn value of the highest MaterialType worn (covering `part` when given); "" none
	std::unordered_map<uint64_t, uint64_t> lastAttacker_;	// victim -> the attacker of its last landed hit (VO_Creature_Victory on its death)
	// ids of the temporary objects (a bolt's ground impact): audiohook takes uint32 ids and the host casts the sim id to uint32, so these count down from
	// 0xFFFFFFFF, a range the sim never issues (its ids count up from 1 and stay far below 2^31); each is removed after its one post
	uint32_t tempObject_ = 0xFFFFFFFFu;
	void cueSounds(const Characters& chars, const simlink::Event& e, const std::set<std::pair<uint64_t, uint64_t>>& shots);
	bool pump();	// moves bytes into the decoder: the file up to the frames needed, the socket whatever is there; false on a fatal error
	void sync(Characters& chars, const std::function<double(double, double)>& ground);
	Characters::Spec specOf(uint64_t id, const Characters& chars) const;
	int severedOf(uint64_t id, const std::string& race, const Characters& chars) const;	// specOf's severed bits, race its race
	int severedOf(uint64_t id, const Characters& chars) const { return dec_.medical.count(id) ? severedOf(id, chars.nameOf(dec_.str(dec_.chars.at(id).look.race)), chars) : 0; }
	void fail(const std::string& why);
	bool send(const std::vector<uint8_t>& out, const char* what);
	bool sendNow(const std::vector<uint8_t>& out, const char* what);
	void answers();	// events 9 / 10 for our orders: the HUD's last reply

	Mode mode_ = None;
	simlink::SimLinkDecoder dec_;
	std::vector<uint8_t> file_;
	size_t fed_ = 0;
	kutil::net::Socket fd_ = kutil::net::kNoSocket;
	bool helloSent_ = false;
	std::vector<uint8_t> preHello_;	// frames sent before our HELLO: HELLO is always the first frame (the server refuses the link otherwise), these follow it in order
	uint64_t focusSent_ = 0;
	bool focusProducer_ = false;	// the focus is a producer's id: its BUILDING row is the focus row, not a player's building
	simlink::SimLinkDecoder::Building focusRow_;	// the focused producer's row (id 0: none yet), kept out of dec_.buildings
	std::set<uint64_t> focusProducers_;	// every producer id we sent order 87 for: a row under one is never a player's building
	std::chrono::steady_clock::time_point lastSend_;	// live: PING when 5 s pass without a frame sent (SIMLINK 11: kicked after 30 s silent)
	uint64_t dataHash_ = 0;
	// The character we drive (order 20) and its predicted position (predict)
	// gait: the target speed learnt from the server's steady forward samples (0: not yet; run: the flag it was learnt under); fwd: our ramped speed
	struct Local { uint64_t id = 0; uint8_t drive = 0; int strafe = 0; double heading = 0, gait = 0, fwd = 0, pos[3] = {}, vel[2] = {}; bool on = false, run = true; } local_;
	const knav::Walker* walker_ = nullptr;
	GroundRay groundRay_;
	std::function<double(double, double)> terrainFn_;
	std::function<bool(double, double, double)> terrainMasked_;
	int solveGround(int raw, double& ry, double x, double z, double th, double navY, double up, double down) const;
	Depenetrate depen_;
	std::unordered_map<int64_t, std::vector<uint64_t>> nudgeGrid_;
	bool nudgeOk(const simlink::Dynamic& d, uint64_t id) const;
	NudgeStats nudge_;
	uint64_t nudgeLogTick_ = 0;
	std::function<const void*()> groundGen_;
	struct ThingY { double x = 0, y = 0, z = 0, drawn = 0; const void* gen = nullptr; bool set = false; };
	mutable std::unordered_map<uint64_t, ThingY> thingY_;
	RayStats rayStats_;
	double predictYOff_ = 0;	// predict's wire-navmesh y offset: the drawn y when the standing ray has no hit
	double rayLogTick_ = 0;
	double dtAnim_ = 0;
#ifdef KCLIENT_DEV_TOOLS
	bool devStall_ = false;
#endif
	double dtSim_ = 0;	// this frame's sim seconds
	uint64_t drivenId_ = 0;	// the character under direct drive (the follow camera on a player), 0 none (setDriven; kenshi-k16.2.237.10: the travel facing)
	void predict(const simlink::Character& c, double p[3], float& yaw);
public:
	void setDriven(uint64_t id) { drivenId_ = id; }
private:
	double tick_ = -1;	// render tick (fractional); < 0 until the first complete snapshot
	// kenshi-k16.2.74: the startup clock backlog eases in instead of snapping (clockEasing_ while it does; clockSnapped_ set on a snap so a drawjump
	// can say the clock caused it). resetSnap_: a load this frame, snap every display offset. synced_: a sync has completed (a resetSerial bump before
	// that is the connection's own, not a load, and must not force a snap).
	// kenshi-k16.2.477.1: the sim's achieved tick rate (ticks per wall second, over >= 0.5 s windows, smoothed) the render clock follows when the sim cannot reach the
	// game speed (x5 on a long travel: 30-65 of 100 ticks/s), so the clock does not run ahead of the snapshots and get eased back ~10 ticks at a time
	double achievedRate_ = 0, rateWall_ = 0, rateFrom_ = 0;
	bool rateValid_ = false, rateSeeded_ = false;
	bool resetSnap_ = false, clockEasing_ = false, clockSnapped_ = false, synced_ = false;
	std::unordered_map<uint64_t, Slot> slots_;
	std::vector<uint64_t> order_;	// sim ids in Characters index order
	std::string error_;
	double ground_[3] = {};
	bool haveFocus_ = false;
	simlink::OrderWriter orders_;
	std::vector<uint64_t> selected_;
	bool selectedOnce_ = false;
	bool holdEmpty_ = false;	// selectNone(): leave the selection empty
	bool squadWaiting_ = false;	// a new squad member is still decoding: the first selection (and what acts on it) waits for its build (kenshi-k16.2.205.12.63)
	std::unordered_set<uint64_t> arrived_;	// dev: the ids whose arrival note was written (kenshi-k16.2.205.12.77)
	bool rosterWarm_ = false;	// the first roster's races were queued on the decode workers (kenshi-k16.2.205.12.63)
	bool held_ = false;	// hold(): no character sync
	bool rebuild_ = false;	// the next sync rebuilds the set
	uint32_t resetSeen_ = 0;	// decoder resetSerial handled (a load: the selection and the theft marks start over)
	std::unordered_map<uint32_t, std::string> sent_;	// clientSeq -> what, until event 9 / 10 answers it
	std::unordered_map<uint32_t, std::string> orderResults_;	// clientSeq -> the wire's answer ("applied tick N" / "rejected REASON"), kept for the shot's route sidecar
	std::unordered_map<uint64_t, std::deque<OrderRow>> orderRows_;	// the last kOrdersKept orders sent per character (kenshi-k16.2.231.1, kenshi-4bps.1)
	std::unordered_map<uint64_t, uint32_t> tasked_;	// taskOrder
	uint32_t taskSerial_ = 0;
	void tasked(const std::vector<uint64_t>& ids, bool queue) { if (queue) { return; } taskSerial_++; for (uint64_t id : ids) { tasked_[id] = taskSerial_; } }
	// kenshi-4bps.1: one row in the shot's route sidecar for one order this client just sent for id (goal: null = none)
	void keepOrder(uint64_t id, uint32_t seq, const char* what, uint64_t target, const double* goal, bool yValid, bool queue);
	// The wire position of a character or an interactable (the sidecar's goal for a targeted order); false when the wire does not know it
	bool wirePos(uint64_t id, double pos[3]) const;
	std::string reply_;	// the last answer, for the HUD
	std::vector<std::string> replies_;
	std::vector<OrderAnswer> orderAnswers_;	// event 9 / 10 for a sent order, awaiting takeOrderAnswers
	std::unordered_set<uint32_t> menuSent_;	// clientSeqs of the menu_query orders sent, awaiting takeMenuAnswers
	std::vector<MenuAnswer> menuAnswers_;
	uint32_t lastSentSeq_ = 0;	// the order sent last (lastSentSeq)
	std::set<std::array<uint64_t, 3>> statsAsked_;	// cleared by a world reset (a load)
	uint32_t statsReset_ = 0;
	std::unordered_set<uint32_t> quiet_;	// clientSeqs of view / interest orders
	std::unordered_set<uint32_t> profileSent_;	// clientSeqs of the profile_query orders sent, awaiting their answer (its event 10 is the overlay's line, not a toast)
	std::chrono::steady_clock::time_point profileAt_{};	// the last profile_query sent (epoch: none yet)
	std::string profileStatus_;	// the refusal word (not_allowed / unsupported) once the server said no: set, no more queries are sent
	std::vector<uint32_t> discovered_;
	std::vector<uint16_t> crimeLog_;	// event kinds 51 / 52 since the last take
	uint32_t lockTooHard_ = 0;	// event 13 result 8 cage picks since the last take (kenshi-k16.2.346; the only case left after ksim 7fd41c98, kenshi-888g.1)
	std::deque<Popup> popups_;
	std::unordered_map<uint64_t, Speech> speech_;
	std::unordered_set<uint64_t> foreign_;	// containers a theft event named
	bool rejected_ = false;
	std::vector<Splash> splashes_;
	double viewSent_[2] = {}, interestSent_[2] = {}, interestRadiusSent_ = 0;
	bool viewOnce_ = false;
};
