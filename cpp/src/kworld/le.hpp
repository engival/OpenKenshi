#pragma once

// Little-endian scalar reads used by the binary world formats. Bounds are
// checked by the callers.

#include <bit>
#include <cstdint>
#include <cstring>

namespace kworld::detail {

inline std::uint16_t readU16(const std::byte* bytes)
{
	return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0]))
		| static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1]) << 8);
}

inline std::uint32_t readU32(const std::byte* bytes)
{
	return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0]))
		| static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1]) << 8)
		| static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2]) << 16)
		| static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3]) << 24);
}

inline std::int32_t readI32(const std::byte* bytes)
{
	return std::bit_cast<std::int32_t>(readU32(bytes));
}

inline float readF32(const std::byte* bytes)
{
	return std::bit_cast<float>(readU32(bytes));
}

} // namespace kworld::detail
