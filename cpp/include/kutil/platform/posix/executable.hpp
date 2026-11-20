#pragma once
// executablePath, POSIX: _NSGetExecutablePath on Apple, /proc/self/exe elsewhere (see kutil/executable.hpp).
#include <cstdint>
#include <filesystem>
#include <system_error>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <vector>
#endif

namespace kutil::platform {

inline std::filesystem::path executablePath(std::error_code& ec)
{
#ifdef __APPLE__
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> path(size);
	if (!size || _NSGetExecutablePath(path.data(), &size))
	{
		ec = std::make_error_code(std::errc::no_such_file_or_directory);
		return {};
	}
	return std::filesystem::canonical(path.data(), ec);
#else
	return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

} // namespace kutil::platform
