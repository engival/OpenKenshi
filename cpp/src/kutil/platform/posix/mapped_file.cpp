#include "kutil/platform/mapped_file.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kutil {

bool MappedFile::open(const std::string& path, std::string& err)
{
	close();
	const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
	{
		err = "cannot open " + path + ": " + std::strerror(errno);
		return false;
	}
	struct stat info {};
	if (::fstat(fd, &info) != 0)
	{
		err = "cannot stat " + path + ": " + std::strerror(errno);
		::close(fd);
		return false;
	}
	if (info.st_size <= 0)
	{
		err = "empty file " + path;
		::close(fd);
		return false;
	}
	void* mapping = ::mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
	const int mapErrno = errno;
	::close(fd);
	if (mapping == MAP_FAILED)
	{
		err = "cannot mmap " + path + ": " + std::strerror(mapErrno);
		return false;
	}
	data_ = mapping;
	size_ = static_cast<std::size_t>(info.st_size);
	return true;
}

void MappedFile::close()
{
	if (data_ != nullptr)
		::munmap(const_cast<void*>(data_), size_);
	release();
}

} // namespace kutil
