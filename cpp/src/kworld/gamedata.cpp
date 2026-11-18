#include "kworld/gamedata.hpp"

#include "kworld/error.hpp"
#include "io.hpp"
#include "le.hpp"

#include <filesystem>

namespace kworld {

kdata::GameData loadMergedGameData(std::string_view dataDir)
{
	return loadMergedGameData(vanillaLoadOrder(dataDir));
}

kdata::GameData loadMergedGameData(const LoadOrder& order)
{
	kdata::GameData merged;
	for (const LoadOrderEntry& entry : order.files)
	{
		auto bytes = detail::readFile(entry.path);
		auto file = kdata::parse(bytes, entry.name);
		merged.apply(file);
	}
	return merged;
}

std::int32_t itemTypeByName(std::string_view name) noexcept
{
	for (std::int32_t type = 0;; ++type)
	{
		const char* candidate = kdata::itemTypeName(type);
		if (candidate == nullptr)
			return -1;
		if (name == candidate)
			return type;
	}
}

} // namespace kworld
