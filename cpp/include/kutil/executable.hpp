#pragma once

// The running executable's path (macOS tester's Metal port, kenshi-379.32.16): _NSGetExecutablePath on Apple, /proc/self/exe elsewhere,
// GetModuleFileNameW on Windows (kenshi-379.19, 379.31.5). Header only (the client's kui_demo includes it without linking kutil): the
// OS code is in kutil/platform/<posix|windows>/executable.hpp.
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include "kutil/platform/windows/executable.hpp"
#else
#include "kutil/platform/posix/executable.hpp"
#endif

namespace kutil {

// An empty path with ec set on failure.
inline std::filesystem::path executablePath(std::error_code& ec)
{
	return platform::executablePath(ec);
}

} // namespace kutil
