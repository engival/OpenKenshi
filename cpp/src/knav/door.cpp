#include "knav/door.hpp"

#include <DetourNavMesh.h>

namespace knav {

void DoorBook::store(DoorKey door, const DoorSetting& setting)
{
	if (setting == DoorSetting{})
		settings_.erase(door);
	else
		settings_[door] = setting;
}

void DoorBook::setState(DoorKey door, DoorState state)
{
	DoorSetting setting = get(door);
	setting.state = state;
	store(door, setting);
}

DoorSetting DoorBook::get(DoorKey door) const noexcept
{
	const auto found = settings_.find(door);
	return found == settings_.end() ? DoorSetting{} : found->second;
}

DoorPaint doorPaint(const DoorSetting& setting) noexcept
{
	const Area area = setting.state == DoorState::Open ? Area::DoorOpen : Area::DoorClosed;
	const unsigned locked = setting.state == DoorState::Locked ? kDoorLockedFlag : 0u;
	return { static_cast<std::uint16_t>(areaFlag(area) | locked), static_cast<std::uint8_t>(area) };
}

void paintDoor(dtNavMesh& mesh, dtPolyRef polyRef, const DoorSetting& setting)
{
	const DoorPaint paint = doorPaint(setting);
	mesh.setPolyArea(polyRef, paint.area);
	mesh.setPolyFlags(polyRef, paint.flags);
}

} // namespace knav
