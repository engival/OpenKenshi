#include "kutil/crashlog.hpp"

#include <csignal>
#include <cstring>
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <filesystem>
#include <unistd.h>

namespace kutil {

// Everything the handler touches is prepared here: it may only use async-signal-safe calls (open, write, read, close, raise),
// plus backtrace(), whose first call loads libgcc and is made at install time.
static char gPath[1024];
static char gProgram[64] = "program";
static volatile std::uint64_t gTick = 0;

static void put(int fd, const char* s)
{
	if (fd < 0)
		return;
	ssize_t r = write(fd, s, strlen(s));
	(void)r;
}

static void put_num(int fd, std::uint64_t v)
{
	char b[24];
	int n = 0;
	do {
		b[n++] = static_cast<char>('0' + v % 10);
		v /= 10;
	} while (v);
	char o[24];
	for (int i = 0; i < n; i++)
		o[i] = b[n - 1 - i];
	o[n] = 0;
	put(fd, o);
}

static void report(int fd, int sig, void** frames, int n)
{
	put(fd, gProgram);
	put(fd, ": crashed: signal ");
	put_num(fd, static_cast<std::uint64_t>(sig));
	put(fd, " (");
	put(fd, sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGFPE ? "SIGFPE"
		: sig == SIGILL ? "SIGILL" : sig == SIGABRT ? "SIGABRT" : "?");
	put(fd, ") at tick ");
	put_num(fd, gTick);
	put(fd, "\nbacktrace (addr2line -f -C -e ");
	put(fd, gProgram);
	put(fd, " ADDR):\n");
	backtrace_symbols_fd(frames, n, fd);
}

static void on_crash(int sig)
{
	void* frames[64];
	const int n = backtrace(frames, 64);
	report(2, sig, frames, n);
	const int fd = gPath[0] ? open(gPath, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
	if (fd >= 0)
	{
		report(fd, sig, frames, n);
		put(fd, "\nmaps:\n");
		const int maps = open("/proc/self/maps", O_RDONLY);
		char buf[4096];
		for (ssize_t r; maps >= 0 && (r = read(maps, buf, sizeof buf)) > 0; ) { ssize_t w = write(fd, buf, static_cast<size_t>(r)); (void)w; }
		if (maps >= 0)
			close(maps);
		close(fd);
		put(2, gProgram);
		put(2, ": crash report: ");
		put(2, gPath);
		put(2, "\n");
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

void crashlog_install(const std::string& dir, const std::string& program)
{
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
	const std::string path = dir + "/crash-" + std::to_string(static_cast<long long>(time(nullptr))) + "-"
		+ std::to_string(static_cast<long long>(getpid())) + ".txt";
	if (!ec && path.size() < sizeof gPath)
		memcpy(gPath, path.c_str(), path.size() + 1);
	if (!program.empty() && program.size() < sizeof gProgram)
		memcpy(gProgram, program.c_str(), program.size() + 1);
	void* warm[1];
	backtrace(warm, 1);
	// its own stack, so a stack overflow is reported too
	static char altStack[64 * 1024];
	stack_t ss = {};
	ss.ss_sp = altStack;
	ss.ss_size = sizeof altStack;
	sigaltstack(&ss, nullptr);
	for (const int sig : { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT })
	{
		struct sigaction sa = {};
		sa.sa_handler = on_crash;
		sa.sa_flags = SA_RESETHAND | SA_ONSTACK;
		sigemptyset(&sa.sa_mask);
		sigaction(sig, &sa, nullptr);
	}
}

void crashlog_tick(std::uint64_t tick)
{
	gTick = tick;
}

} // namespace kutil
