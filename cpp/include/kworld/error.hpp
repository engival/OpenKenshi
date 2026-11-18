#pragma once

#include <stdexcept>

namespace kworld {

// Raised for a malformed or unreadable world file. Record containers raise
// kdata::ParseError instead; both derive from std::runtime_error.
class Error : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

} // namespace kworld
