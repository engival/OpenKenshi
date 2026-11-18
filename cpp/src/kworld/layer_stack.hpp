#pragma once

// The load-order stacking rule shared by Zones::resolveLayers and
// Towns::join: layers are applied from lowest to highest zoneLayerRank; an
// entry with a key already alive replaces it in the earlier entry's slot; a
// deletion removes the live entry; survivors keep first-appearance order.

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kworld::detail {

template <typename T>
class LayerStack
{
public:
	// Returns true when it replaced a live entry.
	bool put(const std::string& key, T value)
	{
		const auto found = slotOf_.find(key);
		const bool alive = found != slotOf_.end() && slots_[found->second].has_value();
		if (found != slotOf_.end())
		{
			slots_[found->second] = std::move(value);
		} else {
			slotOf_.emplace(key, slots_.size());
			slots_.emplace_back(std::move(value));
		}
		return alive;
	}

	// Returns true when it removed a live entry.
	bool remove(const std::string& key)
	{
		const auto found = slotOf_.find(key);
		if (found == slotOf_.end() || !slots_[found->second].has_value())
			return false;
		slots_[found->second].reset();
		return true;
	}

	std::vector<T> survivors() &&
	{
		std::vector<T> result;
		result.reserve(slots_.size());
		for (auto& slot : slots_)
			if (slot.has_value())
				result.push_back(std::move(*slot));
		return result;
	}

private:
	std::vector<std::optional<T>> slots_;
	std::unordered_map<std::string, std::size_t> slotOf_;
};

} // namespace kworld::detail
