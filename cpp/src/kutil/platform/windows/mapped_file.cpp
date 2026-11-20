#include "kutil/platform/mapped_file.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>

namespace kutil {

namespace {

std::string winError(DWORD code)
{
	return "error " + std::to_string(static_cast<unsigned long>(code));
}

} // namespace

bool MappedFile::open(const std::string& path, std::string& err)
{
	close();
	// u8path: the narrow path is UTF-8 everywhere in the project
	const std::filesystem::path wide = std::filesystem::u8path(path);
	const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		err = "cannot open " + path + ": " + winError(GetLastError());
		return false;
	}
	LARGE_INTEGER size {};
	if (!GetFileSizeEx(file, &size))
	{
		err = "cannot stat " + path + ": " + winError(GetLastError());
		CloseHandle(file);
		return false;
	}
	if (size.QuadPart <= 0)
	{
		err = "empty file " + path;
		CloseHandle(file);
		return false;
	}
	const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
	const DWORD mapError = GetLastError();
	CloseHandle(file); // the mapping object keeps the file open
	if (mapping == nullptr)
	{
		err = "cannot mmap " + path + ": " + winError(mapError);
		return false;
	}
	void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
	if (view == nullptr)
	{
		err = "cannot mmap " + path + ": " + winError(GetLastError());
		CloseHandle(mapping);
		return false;
	}
	data_ = view;
	size_ = static_cast<std::size_t>(size.QuadPart);
	handle_ = mapping;
	return true;
}

void MappedFile::close()
{
	if (data_ != nullptr)
		UnmapViewOfFile(data_);
	if (handle_ != nullptr)
		CloseHandle(static_cast<HANDLE>(handle_));
	release();
}

} // namespace kutil
