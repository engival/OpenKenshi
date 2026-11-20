// swapinfo.hpp off Linux: no GLX.
#include "platform/swapinfo.hpp"

namespace swapinfo
{
std::string effective() { return "n/a (not GLX)"; }
bool vblankCount(long long&) { return false; }
}
