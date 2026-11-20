#pragma once
// executablePath, Windows: GetModuleFileNameW (see kutil/executable.hpp). The wide path is kept as a std::filesystem::path, so callers'
// .string() / .u8string() convert it as they do for any other path.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace kutil::platform {

inline std::filesystem::path executablePath(std::error_code& ec)
{
	std::wstring path(512, L'\0');
	for (;;)
	{
		const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
		if (n == 0)
		{
			ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
			return {};
		}
		if (n < path.size()) // not truncated
		{
			path.resize(n);
			return std::filesystem::path(path);
		}
		path.resize(path.size() * 2); // truncated: grow (long paths)
	}
}

} // namespace kutil::platform
