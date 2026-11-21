#pragma once

// The debug view's enum tables (docs/formats/SIMLINK.md "DEBUG VIEW"): the raw sim values on the wire, with the names the
// client shows. Header-only and standard library only, so the client includes it as it does the kworld headers (no copy).
// Values mirror the sim's enums; a new value is APPENDED, never renumbered. The sim asserts the mirror (src/ksim/debugview/view.cpp).

#include <cstddef>
#include <cstdint>

namespace ksim::debugview {

// DEBUG_INFO `debugVersion` (the channel's own version, not the protocol's).
inline constexpr std::uint8_t kDebugVersion = 1;

// Debug frames (docs/formats/SIMLINK.md "DEBUG VIEW"); 200-219 are the reserved range.
inline constexpr std::uint16_t kDebugFirst = 200;
inline constexpr std::uint16_t kDebugLast = 219;
inline constexpr std::uint16_t kDebugInfo = 200; // server -> client: debugVersion, enabled
inline constexpr std::uint16_t kDebugSquads = 201; // server -> client: the AI fields of the SQUADS set
inline constexpr std::uint16_t kDebugTree = 202; // server -> client: one query's rows
inline constexpr std::uint16_t kDebugSpawned = 203; // server -> client: u32 clientSeq, u64 squad (0 refused), str reason
inline constexpr std::uint16_t kDebugAck = 204; // server -> client: u32 clientSeq, u16 frameType, u8 accepted, str reason
inline constexpr std::uint16_t kDebugSubscribe = 210; // client -> server: u8 on
inline constexpr std::uint16_t kDebugQuery = 211; // client -> server: u8 kind, u64 id
inline constexpr std::uint16_t kDebugSpawnSquad = 212; // client -> server: u32 clientSeq, str template, f32 sx, sz, tx, tz
inline constexpr std::uint16_t kDebugSetWeather = 213; // client -> server: u32 clientSeq, str weather, str group, f32 strength
inline constexpr std::uint16_t kDebugSetTime = 214; // client -> server: u32 clientSeq, f32 hour, u8 extraDays
inline constexpr std::uint16_t kDebugCheat = 216; // client -> server: u32 clientSeq, u64 character (0 = the first player character), u8 op (0 heal, 1 knock out, 2 wake, 3 kill)
inline constexpr std::uint16_t kDebugSetOption = 215; // client -> server: u32 clientSeq, str key, f32 value (the new-game options; SIMLINK "DEBUG VIEW")

// DEBUG_INFO `enabled`.
inline constexpr std::uint8_t kEnabled = 1;
inline constexpr std::uint8_t kDisabled = 0;

// The DEBUG_QUERY kind.
enum class QueryKind : std::uint8_t
{
	Squad = 0,
	Character = 1,
	// The world's new-game options (cod / gdm / ht, the medical clock and whether they are saved): the DEBUG_TREE rows are
	// {key, value} at depth 0; the id is ignored (send 0). ADVANCED_OPTIONS_STATIC.md "Ours".
	World = 2,
};

// state::GoalKind.
enum class GoalKind : std::uint8_t
{
	None = 0,
	Idle = 1,
	GoHome = 2,
	TravelTo = 3,
	Patrol = 4,
	Roam = 5,
	Raid = 6,
	ManGate = 7,
	Fight = 8,
};
inline constexpr const char* kGoalKindNames[] = {
	"none", "idle", "go home", "travel to", "patrol", "roam", "raid", "man gate", "fight",
};
inline constexpr std::size_t kGoalKindCount = sizeof(kGoalKindNames) / sizeof(kGoalKindNames[0]);

// state::AiReason (the fallback reason, and a raid order's failure).
enum class AiReason : std::uint8_t
{
	None = 0,
	Unmapped = 1,
	Planned = 2,
	NoPosition = 3,
	NoNavigation = 4,
	OutsideWindow = 5,
	NoRoute = 6,
	OffMesh = 7,
	Stuck = 8,
	NoTarget = 9,
	Expired = 10,
	DoorRefused = 11,
};
inline constexpr const char* kAiReasonNames[] = {
	"none", "unmapped", "planned", "no position", "no navigation", "outside window", "no route", "off mesh",
	"stuck", "no target", "expired", "door refused",
};
inline constexpr std::size_t kAiReasonCount = sizeof(kAiReasonNames) / sizeof(kAiReasonNames[0]);

// state::OrderStatus (a raid order).
enum class OrderStatus : std::uint8_t
{
	None = 0,
	Issued = 1,
	Outbound = 2,
	Staying = 3,
	Returning = 4,
	Done = 5,
	Failed = 6,
	Retreating = 7,
};
inline constexpr const char* kOrderStatusNames[] = {
	"none", "issued", "outbound", "staying", "returning", "done", "failed", "retreating",
};
inline constexpr std::size_t kOrderStatusCount = sizeof(kOrderStatusNames) / sizeof(kOrderStatusNames[0]);

// DetailLevel.
enum class Detail : std::uint8_t
{
	Full = 0,
	Squad = 1,
	Strategic = 2,
};
inline constexpr const char* kDetailNames[] = { "full", "squad", "strategic" };
inline constexpr std::size_t kDetailCount = sizeof(kDetailNames) / sizeof(kDetailNames[0]);

// DEBUG_SQUADS `targetKind`.
enum class TargetKind : std::uint8_t
{
	None = 0,
	Point = 1,
	Town = 2,
	Squad = 3,
	Character = 4,
};
inline constexpr const char* kTargetKindNames[] = { "none", "point", "town", "squad", "character" };
inline constexpr std::size_t kTargetKindCount = sizeof(kTargetKindNames) / sizeof(kTargetKindNames[0]);

// The name of a value, or "?" past the table (a client built before a new value was appended).
inline const char* goalKindName(std::uint8_t value) noexcept { return value < kGoalKindCount ? kGoalKindNames[value] : "?"; }
inline const char* aiReasonName(std::uint8_t value) noexcept { return value < kAiReasonCount ? kAiReasonNames[value] : "?"; }
inline const char* orderStatusName(std::uint8_t value) noexcept { return value < kOrderStatusCount ? kOrderStatusNames[value] : "?"; }
inline const char* detailName(std::uint8_t value) noexcept { return value < kDetailCount ? kDetailNames[value] : "?"; }
inline const char* targetKindName(std::uint8_t value) noexcept { return value < kTargetKindCount ? kTargetKindNames[value] : "?"; }

} // namespace ksim::debugview
