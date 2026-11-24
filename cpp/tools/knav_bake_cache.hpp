// knav_bake result cache (kenshi-k16.15.42): a content-addressed directory per bake; see docs/reference/NAVMESH_CACHE.md "Bake cache".
// <root>/<key>/{.lock,.complete|.failed,data/...}. Held .lock = a bake is running (the kernel drops it if the holder dies).
#pragma once

#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace knavcache {

namespace fs = std::filesystem;

// Two FNV-1a 64 lanes: a 128-bit key text (not cryptographic; the key is a name, not a defence).
inline std::string keyOf(const std::string& text)
{
	std::uint64_t a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
	for (const unsigned char c : text)
	{
		a = (a ^ c) * 0x100000001b3ull;
		b = (b ^ c) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull;
	}
	char out[40];
	std::snprintf(out, sizeof out, "%016llx%016llx", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
	return out;
}

// Names, sizes and mtimes of every file under the game data dir (the bake's inputs; ~16k stats).
inline std::string dataStamp(const std::string& dataDir)
{
	std::ostringstream out;
	std::error_code ec;
	std::vector<std::string> lines;
	for (fs::recursive_directory_iterator it(dataDir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
	{
		struct stat st;
		if (it->is_regular_file(ec) && ::stat(it->path().c_str(), &st) == 0)
			lines.push_back(it->path().lexically_relative(dataDir).string() + " " + std::to_string(st.st_size) + " "
				+ std::to_string(st.st_mtime) + "\n");
	}
	std::sort(lines.begin(), lines.end());
	for (const std::string& l : lines)
		out << l;
	return keyOf(out.str());
}

inline std::string slurp(const fs::path& p)
{
	std::ifstream in(p);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}

// Hard link (else copy) src -> dst, recursively; entry files are 0444 so a consumer cannot rewrite the shared inode.
inline void linkTree(const fs::path& src, const fs::path& dst)
{
	fs::create_directories(dst);
	// Files of an earlier bake that the entry does not have must not survive.
	std::vector<fs::path> stale;
	for (const auto& e : fs::directory_iterator(dst))
		if (!fs::exists(src / e.path().filename()))
			stale.push_back(e.path());
	for (const fs::path& p : stale)
		fs::remove_all(p);
	for (const auto& e : fs::directory_iterator(src))
	{
		const fs::path to = dst / e.path().filename();
		std::error_code ec;
		fs::remove(to, ec);
		if (e.is_directory())
			linkTree(e.path(), to);
		else {
			fs::create_hard_link(e.path(), to, ec);
			if (ec)
				fs::copy_file(e.path(), to, fs::copy_options::overwrite_existing);
		}
	}
}

inline void linkFile(const fs::path& src, const fs::path& dst)
{
	std::error_code ec;
	fs::remove(dst, ec);
	fs::create_hard_link(src, dst, ec);
	if (ec)
		fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
}

inline void readOnlyTree(const fs::path& dir)
{
	for (const auto& e : fs::recursive_directory_iterator(dir))
		if (e.is_regular_file())
			fs::permissions(e.path(), fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
}

// While alive, fds 1 and 2 are teed: every byte still reaches the original stdout/stderr, and the last kTail lines of both
// are kept for the .failed record.
class TeeTail
{
public:
	static constexpr std::size_t kTail = 40;
	TeeTail()
	{
		std::cout << std::flush;
		std::cerr << std::flush;
		for (int i = 0; i < 2; ++i)
		{
			saved_[i] = ::dup(i + 1);
			int p[2];
			if (saved_[i] < 0 || ::pipe(p) != 0)
				return;
			::dup2(p[1], i + 1);
			::close(p[1]);
			threads_[i] = std::thread([this, i, rd = p[0]] { pump(i, rd); });
			active_[i] = true;
		}
	}
	~TeeTail() { stop(); }
	// Restores the fds, drains the pipes, returns the kept tail.
	std::string stop()
	{
		std::cout << std::flush;
		std::cerr << std::flush;
		for (int i = 0; i < 2; ++i)
			if (active_[i])
			{
				::dup2(saved_[i], i + 1);	// closes the pipe's last write end: the pump sees EOF
				active_[i] = false;
				threads_[i].join();
				::close(saved_[i]);
			}
		std::string out;
		for (const std::string& l : lines_)
			out += l + "\n";
		return out;
	}
private:
	void pump(int which, int rd)
	{
		char buf[4096];
		std::string line;
		ssize_t n;
		while ((n = ::read(rd, buf, sizeof buf)) > 0)
		{
			for (ssize_t off = 0; off < n;)
			{
				const ssize_t w = ::write(saved_[which], buf + off, static_cast<std::size_t>(n - off));
				if (w <= 0)
					break;
				off += w;
			}
			std::lock_guard<std::mutex> lock(mutex_);
			for (ssize_t k = 0; k < n; ++k)
			{
				if (buf[k] == '\n')
				{
					lines_.push_back(line);
					if (lines_.size() > kTail)
						lines_.pop_front();
					line.clear();
				} else {
					line += buf[k];
				}
			}
		}
		if (!line.empty())
		{
			std::lock_guard<std::mutex> lock(mutex_);
			lines_.push_back(line);
			if (lines_.size() > kTail)
				lines_.pop_front();
		}
		::close(rd);
	}
	int saved_[2] = { -1, -1 };
	bool active_[2] = { false, false };
	std::thread threads_[2];
	std::mutex mutex_;
	std::deque<std::string> lines_;
};

// Runs `bake(dataDir)` (writes everything under dataDir; returns the exit status) at most once per key, then links the
// result into `place(entryData)`'s callers' target. Returns the bake's status (1 on a recorded failure).
inline int cached(const std::string& root, const std::string& keyText, const std::function<int(const fs::path& data)>& bake,
	const std::function<void(const fs::path& data)>& place)
{
	const std::string key = keyOf(keyText);
	const fs::path entry = fs::path(root) / key;
	for (;;)
	{
		fs::create_directories(entry);
		const std::string lockPath = (entry / ".lock").string();
		const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
		if (fd < 0)
			throw std::runtime_error("cache: cannot open " + lockPath);
		if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
		{
			std::cout << "cache: " << key << " is being baked by another run; waiting\n" << std::flush;
			::flock(fd, LOCK_EX);
		}
		struct stat held, now;
		// The pruner may have removed the entry while we waited: lock the live one instead.
		if (::fstat(fd, &held) != 0 || ::stat(lockPath.c_str(), &now) != 0 || held.st_ino != now.st_ino)
		{
			::close(fd);
			continue;
		}
		int status = 0;
		if (fs::exists(entry / ".failed"))
		{
			std::cerr << "knav_bake: cache " << key << " recorded a failed bake of this exact input (a fix changes the key):\n"
				<< slurp(entry / ".failed") << "\n";
			::close(fd);
			return 1;
		}
		if (fs::exists(entry / ".complete"))
		{
			std::cout << "cache hit " << key << "\n";
			::utimensat(AT_FDCWD, (entry / ".complete").c_str(), nullptr, 0);
		} else {
			// Neither marker and the lock was free: the earlier run was killed. Start clean.
			for (const auto& e : fs::directory_iterator(entry))
				if (e.path().filename() != ".lock")
					fs::remove_all(e.path());
			std::cout << "cache miss " << key << "\n" << std::flush;
			const fs::path data = entry / "data";
			fs::create_directories(data);
			std::string failure;
			std::string tail;
			{
				TeeTail tee;
				try {
					status = bake(data);
				} catch (const std::exception& e) {
					std::cerr << "knav_bake: " << e.what() << "\n";
					failure = std::string("exception: ") + e.what();
					status = 1;
				}
				tail = tee.stop();
			}
			if (status != 0)
			{
				if (failure.empty())
					failure = "exit status " + std::to_string(status);
				std::ofstream(entry / ".failed") << failure << "\nlast output (stdout+stderr, up to " << TeeTail::kTail << " lines):\n" << tail;
				::close(fd);
				return status;
			}
			readOnlyTree(data);
			std::ofstream(entry / ".complete") << keyText << "\n";
		}
		place(entry / "data");
		::close(fd);
		return status;
	}
}

} // namespace knavcache
