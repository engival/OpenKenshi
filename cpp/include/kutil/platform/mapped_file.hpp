#pragma once
#include <cstddef>
#include <string>

namespace kutil {

// A read-only, private mapping of a whole file (kworld::Heightmap's TIFF): mmap on POSIX, CreateFileMapping + MapViewOfFile on Windows
// (src/kutil/platform/<os>/mapped_file.cpp). Only the touched pages are read. Move-only.
class MappedFile
{
public:
	MappedFile() = default;
	MappedFile(MappedFile&& other) noexcept : data_(other.data_), size_(other.size_), handle_(other.handle_) { other.release(); }
	MappedFile& operator=(MappedFile&& other) noexcept;
	MappedFile(const MappedFile&) = delete;
	MappedFile& operator=(const MappedFile&) = delete;
	~MappedFile() { close(); }

	// Maps `path`. False with err set ("cannot open ...: reason", "empty file") on failure.
	bool open(const std::string& path, std::string& err);
	void close();

	const void* data() const { return data_; }
	std::size_t size() const { return size_; }
	explicit operator bool() const { return data_ != nullptr; }

private:
	void release() { data_ = nullptr; size_ = 0; handle_ = nullptr; }

	const void* data_ = nullptr;
	std::size_t size_ = 0;
	void* handle_ = nullptr; // Windows: the file-mapping handle; unused on POSIX
};

inline MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
{
	if (this != &other)
	{
		close();
		data_ = other.data_;
		size_ = other.size_;
		handle_ = other.handle_;
		other.release();
	}
	return *this;
}

} // namespace kutil
