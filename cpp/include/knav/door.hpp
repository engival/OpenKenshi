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

class dtNavMesh;

namespace knav {

using kworld::DoorKey;
using kworld::doorKey;

enum class DoorState : std::uint8_t
{
	Open,
	Closed, // a traveller with access waits for it to open: the DoorClosed area cost
	Locked, // nobody passes
};

// What a door says to one traveller class.
enum class DoorVerdict : std::uint8_t
{
	Free, // open and allowed: the DoorOpen cost
	Wait, // closed and allowed: the DoorClosed cost (the opening delay)
	Blocked, // locked, or the class has no access: excluded from the search
};

struct DoorSetting
{
	DoorState state = DoorState::Open;
	std::uint8_t denied = 0; // bit k set: traveller class k has no access

	DoorVerdict verdict(int doorClass) const noexcept
	{
		if (state == DoorState::Locked || (denied >> doorClass & 1) != 0)
			return DoorVerdict::Blocked;
		return state == DoorState::Closed ? DoorVerdict::Wait : DoorVerdict::Free;
	}

	bool operator==(const DoorSetting&) const = default;
};

// Every door's state and access policy. A door that is not in the book is open to everyone, which
// is also how the bake leaves it. Deterministic: ordered by key, default-valued entries not kept.
class DoorBook
{
public:
	void setState(DoorKey door, DoorState state);
	// `allowed` false = traveller class `doorClass` (0..kDoorClasses-1) may not use the door.
	void setAccess(DoorKey door, int doorClass, bool allowed);

	DoorSetting get(DoorKey door) const noexcept;
	const std::map<DoorKey, DoorSetting>& settings() const noexcept { return settings_; }

	bool operator==(const DoorBook&) const = default;

private:
	void store(DoorKey door, const DoorSetting& setting);

	std::map<DoorKey, DoorSetting> settings_;
};

// The door view of one query: whose state, and which class is asking. No book = every door as baked.
struct DoorQuery
{
	const DoorBook* book = nullptr;
	int doorClass = 0;
};

// What a door setting makes of its polygons: the area (open / closed) and flags (area bit + denied-class bits; a locked door
// denies every class). The same numbers NAV_POLY_FLAGS carries.
struct DoorPaint
{
	std::uint16_t flags = 0;
	std::uint8_t area = 0;
};
DoorPaint doorPaint(const DoorSetting& setting) noexcept;

// Paints one door polygon of a Detour mesh from a setting: its area (open / closed) and the
// denied-class flag bits. A locked door denies every class.
void paintDoor(dtNavMesh& mesh, std::uint32_t polyRef, const DoorSetting& setting);

} // namespace knav
