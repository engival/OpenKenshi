#include "kutil/crashlog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>

// Windows crash recorder (kenshi-379.31.5), the same report as posix/crashlog.cpp: the signal line, the tick, a backtrace, then the
// loaded modules in place of /proc/self/maps. Two entry points: SetUnhandledExceptionFilter (real faults: access violation, illegal
// instruction, divide by zero, stack overflow) and signal() handlers for what the CRT raises itself (abort, raise(SIGSEGV)). The stack is
// CaptureStackBackTrace, printed as `program+0xRVA` (the offset into the exe, for addr2line on a -g build); no dbghelp, so no symbol
// names and no extra link library. Calls here are plain Win32 file I/O, not strictly async-signal-safe (Windows has no such notion).
namespace kutil {

static char gPath[1024];
static char gProgram[64] = "program";
static volatile std::uint64_t gTick = 0;

static void put(HANDLE h, const char* s)
{
	if (h == INVALID_HANDLE_VALUE || h == nullptr)
		return;
	DWORD written = 0;
	WriteFile(h, s, static_cast<DWORD>(strlen(s)), &written, nullptr);
}

static void put_num(HANDLE h, std::uint64_t v)
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
	put(h, o);
}

static void put_hex(HANDLE h, std::uint64_t v)
{
	char o[19] = "0x";
	int n = 0;
	char b[17];
	do {
		b[n++] = "0123456789abcdef"[v & 15];
		v >>= 4;
	} while (v);
	for (int i = 0; i < n; i++)
		o[2 + i] = b[n - 1 - i];
	o[2 + n] = 0;
	put(h, o);
}

static const char* signal_name(int sig)
{
	return sig == SIGSEGV ? "SIGSEGV" : sig == SIGFPE ? "SIGFPE" : sig == SIGILL ? "SIGILL" : sig == SIGABRT ? "SIGABRT" : "?";
}

static void report(HANDLE h, int sig, void** frames, int n, unsigned long exceptionCode)
{
	const std::uint64_t base = reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
	put(h, gProgram);
	put(h, ": crashed: signal ");
	put_num(h, static_cast<std::uint64_t>(sig));
	put(h, " (");
	put(h, signal_name(sig));
	put(h, ") at tick ");
	put_num(h, gTick);
	if (exceptionCode)
	{
		put(h, "\nexception code ");
		put_hex(h, exceptionCode);
	}
	put(h, "\nbacktrace (addr2line -f -C -e ");
	put(h, gProgram);
	put(h, " RVA; the exe's load address is ");
	put_hex(h, base);
	put(h, "):\n");
	for (int i = 0; i < n; i++)
	{
		const std::uint64_t addr = reinterpret_cast<std::uint64_t>(frames[i]);
		put(h, gProgram);
		put(h, "+");
		put_hex(h, addr >= base ? addr - base : addr);
		put(h, " [");
		put_hex(h, addr);
		put(h, "]\n");
	}
}

static void write_reports(int sig, void** frames, int n, unsigned long exceptionCode)
{
	report(GetStdHandle(STD_ERROR_HANDLE), sig, frames, n, exceptionCode);
	if (!gPath[0])
		return;
	const HANDLE file = CreateFileA(gPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		return;
	report(file, sig, frames, n, exceptionCode);
	put(file, "\nmodules:\n");
	char name[MAX_PATH];
	const HMODULE exe = GetModuleHandleW(nullptr);
	if (GetModuleFileNameA(exe, name, sizeof name))
	{
		put_hex(file, reinterpret_cast<std::uint64_t>(exe));
		put(file, " ");
		put(file, name);
		put(file, "\n");
	}
	CloseHandle(file);
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	put(err, gProgram);
	put(err, ": crash report: ");
	put(err, gPath);
	put(err, "\n");
}

static int signal_for(unsigned long code)
{
	switch (code)
	{
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_PRIV_INSTRUCTION:
		return SIGILL;
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:
	case EXCEPTION_FLT_INVALID_OPERATION:
	case EXCEPTION_FLT_OVERFLOW:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_INT_OVERFLOW:
		return SIGFPE;
	default: // access violation, stack overflow, in-page error, ...
		return SIGSEGV;
	}
}

static LONG WINAPI on_exception(EXCEPTION_POINTERS* info)
{
	void* frames[64];
	const int n = CaptureStackBackTrace(0, 64, frames, nullptr);
	write_reports(signal_for(info->ExceptionRecord->ExceptionCode), frames, n, info->ExceptionRecord->ExceptionCode);
	return EXCEPTION_CONTINUE_SEARCH; // the default handling ends the process
}

static void on_signal(int sig)
{
	void* frames[64];
	const int n = CaptureStackBackTrace(0, 64, frames, nullptr);
	write_reports(sig, frames, n, 0);
	signal(sig, SIG_DFL);
	raise(sig);
}

void crashlog_install(const std::string& dir, const std::string& program)
{
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
	const std::string path = dir + "/crash-" + std::to_string(static_cast<long long>(time(nullptr))) + "-"
		+ std::to_string(static_cast<long long>(GetCurrentProcessId())) + ".txt";
	if (!ec && path.size() < sizeof gPath)
		memcpy(gPath, path.c_str(), path.size() + 1);
	if (!program.empty() && program.size() < sizeof gProgram)
		memcpy(gProgram, program.c_str(), program.size() + 1);
	void* warm[1];
	CaptureStackBackTrace(0, 1, warm, nullptr);
	ULONG guarantee = 64 * 1024; // stack left for the filter after a stack overflow, so that is reported too
	SetThreadStackGuarantee(&guarantee);
	SetUnhandledExceptionFilter(on_exception);
	for (const int sig : { SIGSEGV, SIGFPE, SIGILL, SIGABRT })
		signal(sig, on_signal);
}

void crashlog_tick(std::uint64_t tick)
{
	gTick = tick;
}

} // namespace kutil
