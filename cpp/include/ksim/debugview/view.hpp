#pragma once

// The sim's debug view (docs/formats/SIMLINK.md "DEBUG VIEW"): the AI fields of the SQUADS set and one query's state tree,
// as debug-only frames 200-219. Compiled with the CMake option KSIM_DEBUG_VIEW (ON in every build type: the runtime gate
// is the server's --debug-view plus a loopback peer); with the option off this whole header is an inert stub, so the frame
// range is accepted and dropped silently. The view only reads the simulation, except the debug commands DEBUG_SPAWN_SQUAD,
// DEBUG_SET_WEATHER and DEBUG_SET_TIME, which do change the world.

#include "ksim/debugview/enums.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ksim {
class Simulation;
namespace simlink {
class RecordWriter;
}

namespace debugview {

#if KSIM_DEBUG_VIEW

class View
{
public:
	View() = default;
	View(const View&) = delete;
	View& operator=(const View&) = delete;

	// One client frame in the debug range (200-219). Always consumed: a malformed one, the view off or the peer refused,
	// all drop it (the frame must never end a session). `enabled`: the server has --debug-view and the peer is loopback.
	// The commands (212, 213, 214) change the world; they answer even when `enabled` is false, so the client learns why
	// (DEBUG_SPAWNED for 212, DEBUG_ACK for 213/214).
	bool frame(Simulation& sim, bool enabled, std::uint16_t type, const std::uint8_t* payload, std::size_t size);

	// A load (the resend): pending answers are dropped. The subscription itself stays (the client is still there).
	void reset() noexcept
	{
		pending_ = {};
		pendingSpawns_.clear();
		pendingAcks_.clear();
		clockDue_ = false;
	}

	// The next send tick's answers. Const: sendTick's emit runs twice (a null pass that interns the strings, then the real
	// one), so this must write the same frames both times; consumed() ends them after the second pass.
	// `squadsDue`: this tick writes SQUADS, so DEBUG_SQUADS follows it. `squadLimit`: the session's SQUADS limit (0 = every
	// squad). `strid` interns a string in the session's table.
	void write(simlink::RecordWriter& out, std::uint64_t tick, const Simulation& sim, std::size_t squadLimit,
		const std::function<std::uint32_t(const std::string&)>& strid, bool squadsDue) const;
	// The answers written: the next send tick starts clean.
	void consumed() noexcept
	{
		pendingInfo_ = false;
		pending_.clear();
		pendingSpawns_.clear();
		pendingAcks_.clear();
		clockDue_ = false;
	}

	// Answers are queued (queries, spawn/ack replies, DEBUG_INFO): a paused session sends them without a tick (Session::sendDebugOnly).
	bool hasWork() const noexcept { return pendingInfo_ || !pending_.empty() || !pendingSpawns_.empty() || !pendingAcks_.empty(); }
	// A DEBUG_SUBSCRIBE is being answered: a paused session sends its DEBUG_SQUADS with it (no SQUADS cadence runs while paused, and
	// a paused F12 shot learns its squads from it; kenshi-k16.2.104.1).
	bool infoDue() const noexcept { return pendingInfo_; }

	// A DEBUG_SET_TIME applied since the last send: that send carries the CLOCK frame (its tick jumped, so the change check
	// alone would miss it until the next 1 Hz CLOCK).
	bool clockDue() const noexcept { return clockDue_; }

private:
	struct Query
	{
		std::uint8_t kind = 0; // QueryKind
		std::uint64_t id = 0;
	};
	struct SpawnReply
	{
		std::uint32_t clientSeq = 0;
		std::uint64_t squadId = 0; // 0 = refused
		std::string reason;
	};
	struct AckReply
	{
		std::uint32_t clientSeq = 0;
		std::uint16_t frameType = 0; // the debug frame answered
		bool accepted = false;
		std::string reason;
	};
	bool subscribed_ = false;
	bool enabled_ = false;
	bool pendingInfo_ = false;
	bool clockDue_ = false;
	std::vector<Query> pending_;
	std::vector<SpawnReply> pendingSpawns_;
	std::vector<AckReply> pendingAcks_;
};

#else

struct View
{
	bool frame(Simulation&, bool, std::uint16_t, const std::uint8_t*, std::size_t) { return true; }
	void reset() noexcept {}
	void write(simlink::RecordWriter&, std::uint64_t, const Simulation&, std::size_t,
		const std::function<std::uint32_t(const std::string&)>&, bool) const {}
	void consumed() noexcept {}
	bool clockDue() const noexcept { return false; }
	bool hasWork() const noexcept { return false; }
	bool infoDue() const noexcept { return false; }
};

#endif

} // namespace debugview
} // namespace ksim
