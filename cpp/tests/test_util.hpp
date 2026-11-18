#pragma once

#include "kdata/format.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace test {

struct Failure : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

inline void require(bool condition, const std::string& message)
{
	if (!condition)
		throw Failure(message);
}

inline std::vector<std::byte> readFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw Failure("cannot open " + path);
	in.seekg(0, std::ios::end);
	auto size = in.tellg();
	in.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty())
		in.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	if (!in)
		throw Failure("cannot read " + path);
	return bytes;
}

// A whole server frame (u32 len, u16 type, payload) as the client decoder (client/src/net/simlink.cpp, which rejects trailing bytes) still
// reads it: without the 2026-11-02 tails (CHAR_DYNAMIC actionFlags u8; the kind 4 INTERACTABLE progress f32, rate f32, progressTick u64)
// the 2026-11-13 INTERACTABLE kind 3 tail (building u64, piece u16) and the 2026-11-04 INTERACTABLE ownership tail (14 bytes) and CHAR_DYNAMIC pose tail (20 bytes, flag 15 or 16).
// Delete this when the client decoder learns them.
inline std::vector<std::uint8_t> clientFrame(const std::uint8_t* frame, std::size_t size)
{
	std::uint16_t type = 0;
	std::memcpy(&type, frame + 4, 2);
	std::size_t cut = 0;
	if (type == 21)
	{
		std::uint32_t flags = 0;
		std::memcpy(&flags, frame + 6 + 40, 4);
		cut = 1 + ((flags & ((1u << 15) | (1u << 16))) != 0 ? 20 : 0); // + the 2026-11-04 pose tail of a seated / asleep character
	}
	else if (type == 24 && size > 6 + 16)
		cut = frame[6 + 16] == 5 ? 0 : (frame[6 + 16] == 4 ? 16 : frame[6 + 16] == 3 ? 10 : 0) + 14; // + the 2026-11-04 ownership tail; a bed (kind 5) the client decodes whole (188febfb); a container's 2026-11-13 building + piece tail (u64, u16)
	std::vector<std::uint8_t> out(frame, frame + size - cut);
	const std::uint32_t len = static_cast<std::uint32_t>(size - cut - 4);
	std::memcpy(out.data(), &len, 4);
	return out;
}

} // namespace test
