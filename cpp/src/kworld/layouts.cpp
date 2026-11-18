#include "kworld/layouts.hpp"

#include "kworld/gamedata.hpp"
#include "io.hpp"
#include "le.hpp"

#include "kdata/gamedata.hpp"
#include "kdata/format.hpp"

#include <filesystem>

namespace kworld {

std::vector<std::string> InteriorLayouts::levelFiles(std::string_view dataDir)
{
	namespace fs = std::filesystem;
	std::vector<std::string> files;
	const fs::path root = fs::path(dataDir) / "newland" / "leveldata";
	for (const char* layer : {"", "Newwworld", "Dialogue", "rebirth"})
	{
		const fs::path path = root / layer / "interiors.level";
		if (fs::exists(path))
			files.push_back(path.string());
	}
	return files;
}

InteriorLayouts InteriorLayouts::load(const std::vector<std::string>& files)
{
	kdata::GameData merged;
	for (const std::string& path : files)
		merged.apply(kdata::parse(detail::readFile(path), std::filesystem::path(path).filename().string()));
	InteriorLayouts out;
	for (const kdata::Record* record : merged.byType(itemTypeByName("INSTANCE_COLLECTION")))
	{
		InteriorLayout layout;
		layout.building = record->stringOr("associated building");
		if (layout.building.empty())
			continue;
		layout.sid = record->stringID;
		for (const kdata::Instance& instance : record->instances)
		{
			if (instance.targetStringID.empty())
				continue;
			const std::byte* t = instance.transform.data();
			layout.items.push_back({instance.idIsString ? instance.id : std::to_string(instance.idInt), instance.targetStringID,
				{detail::readF32(t), detail::readF32(t + 4), detail::readF32(t + 8)},
				{detail::readF32(t + 12), detail::readF32(t + 16), detail::readF32(t + 20), detail::readF32(t + 24)}});
		}
		out.index_.emplace(layout.sid, out.layouts_.size());
		out.layouts_.push_back(std::move(layout));
	}
	return out;
}

const InteriorLayout* InteriorLayouts::find(std::string_view sid) const noexcept
{
	const auto found = index_.find(std::string(sid));
	return found == index_.end() ? nullptr : &layouts_[found->second];
}

const InteriorLayout* InteriorLayouts::of(const PlacedInstance& placed) const
{
	return placed.interiorLayout.empty() ? nullptr : find(placed.buildingID + "-" + placed.interiorLayout);
}

PlacedInstance placeFurniture(const PlacedInstance& building, const LayoutItem& item, const kdata::GameData& gameData)
{
	const double w = building.qw, x = building.qx, y = building.qy, z = building.qz;
	const double s = building.scale;
	const double v[3] = {item.pos[0] * s, item.pos[1] * s, item.pos[2] * s};
	// v + 2 u x (u x v + w v), u = (x, y, z)
	const double cx = y * v[2] - z * v[1] + w * v[0];
	const double cy = z * v[0] - x * v[2] + w * v[1];
	const double cz = x * v[1] - y * v[0] + w * v[2];
	PlacedInstance out = building;
	out.instanceID = building.instanceID + "/" + item.id;
	out.buildingID = item.target;
	out.x = building.x + v[0] + 2 * (y * cz - z * cy);
	out.y = building.y + v[1] + 2 * (z * cx - x * cz);
	out.z = building.z + v[2] + 2 * (x * cy - y * cx);
	const auto& q = item.quat;
	out.qw = w * q[0] - x * q[1] - y * q[2] - z * q[3];
	out.qx = w * q[1] + x * q[0] + y * q[3] - z * q[2];
	out.qy = w * q[2] - x * q[3] + y * q[0] + z * q[1];
	out.qz = w * q[3] + x * q[2] - y * q[1] + z * q[0];
	const kdata::Record* record = gameData.find(item.target);
	out.scale = (record != nullptr ? record->floatOr("scale", 1.0f) : 1.0f) * building.scale;
	out.hasScale = record != nullptr;
	out.interiorLayout.clear();
	out.squadTemplate.clear();
	out.label.clear();
	return out;
}

} // namespace kworld
