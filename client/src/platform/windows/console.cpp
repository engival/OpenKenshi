#include "platform/console.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <io.h>

#include <windows.h>

#include "app/userdirs.hpp"

// The Windows log (kenshi-379.31.9.1, platform/console.hpp): the exe is linked for the GUI subsystem, so there is no console unless asked for.
namespace plat {

namespace {
std::string gLogPath;

bool consoleWanted(int argc, char** argv)
{
	const char* e = getenv("KCLIENT_CONSOLE");
	if (e != nullptr && *e != '\0' && strcmp(e, "0") != 0) { return true; }
	for (int i = 1; i < argc; ++i) { if (strcmp(argv[i], "--console") == 0) { return true; } }
	return false;
}

// Points the CRT's stdout / stderr (and the Win32 standard handles a child inherits) at `path`, "CONOUT$" for a console.
void redirect(const wchar_t* path, bool append)
{
	FILE* f = nullptr;
	_wfreopen_s(&f, path, append ? L"a" : L"w", stdout);
	_wfreopen_s(&f, path, append ? L"a" : L"w", stderr);
	// Both streams opened the file separately: for a file, make stderr share stdout's handle so lines stay in order.
	if (_fileno(stdout) >= 0 && _fileno(stderr) >= 0) { _dup2(_fileno(stdout), _fileno(stderr)); }
	setvbuf(stdout, nullptr, _IOLBF, 4096);
	setvbuf(stderr, nullptr, _IONBF, 0);
	const intptr_t out = _get_osfhandle(_fileno(stdout));
	const intptr_t err = _get_osfhandle(_fileno(stderr));
	if (out != -1) { SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)out); }
	if (err != -1) { SetStdHandle(STD_ERROR_HANDLE, (HANDLE)err); }
}

std::wstring widen(const std::string& s)
{
	if (s.empty()) { return {}; }
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}
}

void setupLog(int argc, char** argv)
{
	if (consoleWanted(argc, argv))
	{
		if (!AttachConsole(ATTACH_PARENT_PROCESS)) { AllocConsole(); }	// started from cmd / a terminal: its console; else a window of our own
		SetConsoleOutputCP(CP_UTF8);
		redirect(L"CONOUT$", true);
		FILE* in = nullptr;
		_wfreopen_s(&in, L"CONIN$", L"r", stdin);
		return;
	}
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::path dir = fs::path(widen(userDataDir())) / L"logs";
	fs::create_directories(dir, ec);
	const fs::path log = dir / L"openkenshi.log";
	const fs::path prev = dir / L"openkenshi.log.1";
	MoveFileExW(log.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);	// the previous run stays as .1 (a failed move just truncates below)
	redirect(log.c_str(), false);
	const std::u8string u8 = log.u8string();	// UTF-8, not string()'s ANSI codepage: a user name outside it stays whole
	gLogPath.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
	fprintf(stderr, "OpenKenshi log: %s\n", gLogPath.c_str());
}

std::string logPath() { return gLogPath; }

}
