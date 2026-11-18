#include "knav/door.hpp"

#include "kworld/error.hpp"

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

void DoorBook::setAccess(DoorKey door, int doorClass, bool allowed)
{
	if (doorClass < 0 || doorClass >= kDoorClasses)
		throw kworld::Error("door class out of range");
	DoorSetting setting = get(door);
	const auto bit = static_cast<std::uint8_t>(1u << doorClass);
	setting.denied = allowed ? setting.denied & ~bit : setting.denied | bit;
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
	const unsigned denied = setting.state == DoorState::Locked ? 0xffu : setting.denied;
	return { static_cast<std::uint16_t>(areaFlag(area) | denied << kDoorDeniedShift), static_cast<std::uint8_t>(area) };
}

void paintDoor(dtNavMesh& mesh, std::uint32_t polyRef, const DoorSetting& setting)
{
	const DoorPaint paint = doorPaint(setting);
	mesh.setPolyArea(polyRef, paint.area);
	mesh.setPolyFlags(polyRef, paint.flags);
}

} // namespace knav
