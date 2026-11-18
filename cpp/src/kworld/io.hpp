#pragma once

#include "kworld/error.hpp"

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace kworld::detail {

inline std::vector<std::byte> readFile(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw Error("cannot open " + path);
	in.seekg(0, std::ios::end);
	const auto end = in.tellg();
	if (end < 0)
		throw Error("cannot size " + path);
	in.seekg(0, std::ios::beg);
	std::vector<std::byte> bytes(static_cast<std::size_t>(end));
	if (!bytes.empty())
		in.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	if (!in)
		throw Error("cannot read " + path);
	return bytes;
}

} // namespace kworld::detail
