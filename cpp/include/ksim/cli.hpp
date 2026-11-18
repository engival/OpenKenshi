// Command-line helpers shared by the ksim executables.
#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace ksim {

// An "on" / "off" flag value; anything else throws naming the flag.
inline bool parseOnOff(std::string_view value, std::string_view flag)
{
	if (value != "on" && value != "off")
		throw std::runtime_error(std::string(flag) + " takes on or off");
	return value == "on";
}

} // namespace ksim
