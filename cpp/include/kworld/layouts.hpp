#pragma once

// The interior furniture layouts (docs/reference/INTERIORS.md "Furniture placement"): interiors.level of each data layer, an
// INSTANCE_COLLECTION per layout, sid `<building sid>-<layout name>`, each instance a furniture BUILDING in the building's local
// frame. A zone placement picks its layout with its GAMESTATE_BUILDING `interior layout name` (PlacedInstance::interiorLayout).

#include "kworld/zones.hpp"

#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kdata {
class GameData;
}

namespace kworld {

struct LayoutItem
{
	std::string id; // the instance id within the collection
	std::string target; // the furniture BUILDING record
	std::array<double, 3> pos{}; // building-local
	std::array<double, 4> quat{1.0, 0.0, 0.0, 0.0}; // building-local rotation, w x y z
};

struct InteriorLayout
{
	std::string sid; // "<building sid>-<layout name>"
	std::string building; // `associated building`
	std::vector<LayoutItem> items;
};

class InteriorLayouts
{
public:
	// dataDir/newland/leveldata/{,Newwworld,Dialogue,rebirth}/interiors.level that exist, in layer order.
	static std::vector<std::string> levelFiles(std::string_view dataDir);
	// The files merged as game data (a later layer edits an earlier one's collection). Collections without an
	// `associated building` (the `EXT-` exterior attachments) are left out.
	static InteriorLayouts load(const std::vector<std::string>& files);
	static InteriorLayouts load(std::string_view dataDir) { return load(levelFiles(dataDir)); }

	const InteriorLayout* find(std::string_view sid) const noexcept;
	// The layout a placement names: `<buildingID>-<interiorLayout>`, nullptr when it names none or it does not join.
	const InteriorLayout* of(const PlacedInstance& placed) const;
	const std::vector<InteriorLayout>& all() const noexcept { return layouts_; }

private:
	std::vector<InteriorLayout> layouts_;
	std::unordered_map<std::string, std::size_t> index_;
};

// One layout item placed in the world: the building's transform composed with the item's (the client's buildings.cpp
// addFurniture: the local position is scaled by the building's scale, rotated by its quaternion and offset by its position; the
// rotations multiply). Scale = the item record's `scale` x the building's. Instance id `<building instance>/<item id>`.
PlacedInstance placeFurniture(const PlacedInstance& building, const LayoutItem& item, const kdata::GameData& gameData);

} // namespace kworld
