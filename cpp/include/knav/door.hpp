#pragma once

// Door and gate state over the navmesh (docs/formats/NAVMESH_CACHE.md "Doors"). A doorway is
// always baked as walkable passage: tiles never depend on a door's state. The bake gives each door
// its own polygons (areas DoorOpen / DoorClosed) and records which polygons belong to it. The
// state lives in a DoorBook; NavMesh::applyDoors repaints those polygons (no tile is rebuilt), and
// the coarse graph reads the book directly.

#include "knav/travel.hpp"
#include "kworld/collision.hpp"

#include <cstdint>
#include <map>

#include <DetourNavMesh.h> // dtPolyRef (64-bit: DT_POLYREF64)

namespace knav {

using kworld::DoorKey;
using kworld::doorKey;

enum class DoorState : std::uint8_t
{
	Open,
	Closed, // a traveller waits for it to open: the DoorClosed area cost
	Locked, // nobody passes
};

// What a door says to a traveller.
enum class DoorVerdict : std::uint8_t
{
	Free, // open: the DoorOpen cost
	Wait, // closed: the DoorClosed cost (the opening delay)
	Blocked, // locked: excluded from the search
};

struct DoorSetting
{
	DoorState state = DoorState::Open;

	// `throughLocks`: a bash ignores the lock.
	DoorVerdict verdict(bool throughLocks = false) const noexcept
	{
		if (state == DoorState::Locked && !throughLocks)
			return DoorVerdict::Blocked;
		return state == DoorState::Closed ? DoorVerdict::Wait : DoorVerdict::Free;
	}

	bool operator==(const DoorSetting&) const = default;
};

// Every door's state. A door that is not in the book is open to everyone, which
// is also how the bake leaves it. Deterministic: ordered by key, default-valued entries not kept.
class DoorBook
{
public:
	void setState(DoorKey door, DoorState state);

	DoorSetting get(DoorKey door) const noexcept;
	const std::map<DoorKey, DoorSetting>& settings() const noexcept { return settings_; }

	bool operator==(const DoorBook&) const = default;

private:
	void store(DoorKey door, const DoorSetting& setting);

	std::map<DoorKey, DoorSetting> settings_;
};

// The door view of one query: whose state, and whether it breaks locks. No book = every door as baked.
struct DoorQuery
{
	const DoorBook* book = nullptr;
	bool throughLocks = false;
};

// What a door setting makes of its polygons: the area (open / closed) and flags (area bit + the locked bit). The same numbers NAV_POLY_FLAGS carries.
struct DoorPaint
{
	std::uint16_t flags = 0;
	std::uint8_t area = 0;
};
DoorPaint doorPaint(const DoorSetting& setting) noexcept;

// Paints one door polygon of a Detour mesh from a setting: its area (open / closed) and the
// locked flag bit.
void paintDoor(dtNavMesh& mesh, dtPolyRef polyRef, const DoorSetting& setting);

} // namespace knav
